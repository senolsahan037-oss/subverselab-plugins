#pragma once

/*  The cassette engine. Plain C++, no JUCE, so the demo tool and the plugin
    run the same code.

    One pass is one trip onto tape and back:

        record EQ (treble pre-emphasis) -> tape saturation (soft, asymmetric)
        -> playback EQ (de-emphasis) -> head bump and low-end loss
        -> gap / spacing loss (the top end a slow cassette cannot hold)
        -> transport: wow (slow drift) and flutter (fast) as a moving read head
        -> hiss added on the way out, and dropouts where the oxide is worn.

    Generation N runs the pass N times: a copy of a copy of a copy. Each trip
    loses a little more top, adds its own hiss and its own speed errors, the
    way a dubbed cassette on a Turkish market stall did.

    Deterministic: every "random" movement comes from a seeded generator, so
    the same input and settings give the same output, bit for bit.
*/

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace kaset
{
constexpr float kPi = 3.14159265358979f;

struct Rng
{
    uint32_t s;
    explicit Rng (uint32_t seed = 1983u) : s (seed) {}
    float next() { s = s * 1664525u + 1013904223u; return (float) (s >> 8) / 16777216.0f; }   // 0..1
    float bipolar() { return next() * 2.0f - 1.0f; }
};

struct OnePole
{
    float a = 0.0f, z = 0.0f;
    void setLowpass (float hz, float sr) { a = std::exp (-2.0f * kPi * std::min (hz, 0.49f * sr) / sr); }
    float lp (float x) { z = x + a * (z - x); return z; }
    float hp (float x) { return x - lp (x); }
};

/** RBJ biquad: low/high shelf, peak, low/high pass. */
struct Biquad
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    enum Type { lowpass, highpass, peak, lowShelf, highShelf };

    void set (Type t, float hz, float q, float db, float sr)
    {
        hz = std::min (hz, 0.47f * sr);
        const float A = std::pow (10.0f, db / 40.0f);
        const float w = 2.0f * kPi * hz / sr, c = std::cos (w), s = std::sin (w), al = s / (2.0f * q);
        float B0, B1, B2, A0, A1, A2;
        switch (t)
        {
            case lowpass:  B0 = (1 - c) / 2; B1 = 1 - c; B2 = B0; A0 = 1 + al; A1 = -2 * c; A2 = 1 - al; break;
            case highpass: B0 = (1 + c) / 2; B1 = -(1 + c); B2 = B0; A0 = 1 + al; A1 = -2 * c; A2 = 1 - al; break;
            case peak:     B0 = 1 + al * A; B1 = -2 * c; B2 = 1 - al * A; A0 = 1 + al / A; A1 = -2 * c; A2 = 1 - al / A; break;
            case lowShelf:
            {
                const float r = 2 * std::sqrt (A) * al;
                B0 = A * ((A + 1) - (A - 1) * c + r); B1 = 2 * A * ((A - 1) - (A + 1) * c); B2 = A * ((A + 1) - (A - 1) * c - r);
                A0 = (A + 1) + (A - 1) * c + r;       A1 = -2 * ((A - 1) + (A + 1) * c);    A2 = (A + 1) + (A - 1) * c - r;
                break;
            }
            default:
            {
                const float r = 2 * std::sqrt (A) * al;
                B0 = A * ((A + 1) + (A - 1) * c + r); B1 = -2 * A * ((A - 1) + (A + 1) * c); B2 = A * ((A + 1) + (A - 1) * c - r);
                A0 = (A + 1) - (A - 1) * c + r;        A1 = 2 * ((A - 1) - (A + 1) * c);      A2 = (A + 1) - (A - 1) * c - r;
                break;
            }
        }
        b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0;
    }
    float process (float x)
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() { z1 = z2 = 0.0f; }
};

/** What the plugin can change while playing. */
enum class TapeType { normal, chrome, metal };

struct Controls
{
    int generation = 3;        // 1 = master .. 5 = fourth dub
    TapeType tape = TapeType::normal;
    float wow = 0.5f;          // 0..1, slow speed drift
    float flutter = 0.5f;      // 0..1, fast speed wobble
    float drive = 0.5f;        // 0..1, how hard the tape is hit
    float hiss = 0.5f;         // 0..1
    float wear = 0.3f;         // 0..1, dropouts and worn oxide
};

/** One trip onto tape and back, for one channel. */
class Pass
{
public:
    /** transportSeed is shared by both channels (one tape, one motor); noiseSeed is per channel. */
    void prepare (float sampleRate, uint32_t transportSeed, uint32_t noiseSeed, int index)
    {
        sr = sampleRate;
        rng = Rng (transportSeed);
        noise = Rng (noiseSeed);
        pass = index;
        baseDelay = std::round (0.003f * sr);
        delay.assign ((size_t) (0.02f * sr) + 8, 0.0f);
        w = 0;

        // Each copy was made on a different deck: slightly different corners.
        const float spread = 1.0f + 0.06f * rng.bipolar();
        preEmph.set (Biquad::highShelf, 3200.0f * spread, 0.7f, 9.0f, sr);
        deEmph.set (Biquad::highShelf, 3200.0f * spread, 0.7f, -9.0f, sr);
        headBump.set (Biquad::peak, 85.0f * spread, 1.1f, 2.2f, sr);
        lowLoss.set (Biquad::highpass, 32.0f * spread, 0.6f, 0.0f, sr);

        wowRate = 0.55f + 0.25f * rng.next();
        flutterRate = 6.5f + 3.0f * rng.next();
        wowPhase = rng.next();
        flutterPhase = rng.next();
        driftLp.setLowpass (0.35f, sr);
        hissLp.setLowpass (7500.0f, sr);
        hissHp.setLowpass (900.0f, sr);
        // prepare() starts a new run: the filters must not carry what the previous run left in them (controlproof: 1 s of silence after a
        // chord on the same instance began with a decaying copy of the chord, up to -5 dBFS at full settings). A fresh instance already holds zeros.
        preEmph.reset(); deEmph.reset(); headBump.reset(); lowLoss.reset(); gap1.reset(); gap2.reset();
        driftLp.z = hissLp.z = hissHp.z = 0.0f;
        dropEnv = 1.0f;
        dropLeft = 0;
        depthInit = 0;
    }

    void setTop (float hz)
    {
        // Gap and spacing loss: two gentle poles, the second well above the first.
        gap1.set (Biquad::lowpass, hz, 0.58f, 0.0f, sr);
        gap2.set (Biquad::lowpass, hz * 1.7f, 0.5f, 0.0f, sr);
    }

    float process (float x, const Controls& c)
    {
        // Record: pre-emphasis, then the tape itself. Past the second dub each copy is
        // recorded lower, so saturation stops piling up and the far end stays usable.
        float y = preEmph.process (x);
        // Chrome and metal take more level before they compress.
        const float headroom = c.tape == TapeType::normal ? 1.0f : c.tape == TapeType::chrome ? 0.85f : 0.72f;
        const float drive = (0.5f + 1.9f * c.drive) * (pass < 3 ? 1.0f : 0.7f) * headroom;
        const float bias = 0.08f;   // a slightly asymmetric curve: even harmonics, like real oxide
        y = (std::tanh (drive * y + bias) - std::tanh (bias)) / drive;
        y = deEmph.process (y);
        y = lowLoss.process (headBump.process (y));
        y = gap2.process (gap1.process (y));

        // Transport: write, then read back from a head whose position wobbles.
        delay[(size_t) w] = y;
        if (++w == (int) delay.size()) w = 0;

        wowPhase += wowRate / sr;       if (wowPhase >= 1.0f) wowPhase -= 1.0f;
        flutterPhase += flutterRate / sr; if (flutterPhase >= 1.0f) flutterPhase -= 1.0f;
        const float drift = driftLp.lp (rng.bipolar());   // slow random wander on top of the wow cycle

        // Depth in seconds of head travel: 0.35 % wow and 0.12 % flutter at full, per pass.
        const float later = pass < 3 ? 1.0f : 0.6f;   // past the second dub, speed errors stop piling up
        // The depths come from controls that a host moves once per block; used as they arrive, every step is a jump of the read head
        // (an audible click on sustained material at large blocks: controlproof, WOW ramp at block 512). A 20 ms glide per sample; a constant setting
        // stays bit-identical because the glide starts on the first value it sees and adds exactly 0 once there.
        if (depthInit == 0) { wowDepth = c.wow; flutDepth = c.flutter; depthInit = 1; }
        const float glide = 1.0f - std::exp (-1.0f / (0.02f * sr));
        wowDepth += (c.wow - wowDepth) * glide;
        flutDepth += (c.flutter - flutDepth) * glide;
        const float wowSec = later * wowDepth * 0.0035f / (2.0f * kPi * wowRate);
        const float flutSec = later * flutDepth * 0.0012f / (2.0f * kPi * flutterRate);
        const float d = baseDelay
                      + sr * (wowSec * (std::sin (2.0f * kPi * wowPhase) + 6.0f * drift)
                              + flutSec * std::sin (2.0f * kPi * flutterPhase));
        y = readCubic (d);

        // Worn oxide: now and then the level sags for a few tens of ms.
        if (dropLeft > 0)
        {
            --dropLeft;
            dropEnv += (dropDepth - dropEnv) * 0.004f;
        }
        else
        {
            dropEnv += (1.0f - dropEnv) * 0.002f;
            const float chancePerSec = c.wear * c.wear * 1.6f;
            if (rng.next() < chancePerSec / sr)
            {
                dropLeft = (int) ((0.02f + 0.07f * rng.next()) * sr);
                dropDepth = 1.0f - (0.25f + 0.55f * rng.next()) * c.wear;
            }
        }
        y *= dropEnv;

        // Hiss of this trip: band-shaped noise, about -62 dB at full per pass.
        const float n = hissLp.lp (noise.bipolar());
        const float shaped = n - hissHp.lp (n);
        const float hissScale = c.tape == TapeType::normal ? 1.0f : c.tape == TapeType::chrome ? 0.63f : 0.5f;   // -4 dB, -6 dB
        y += shaped * c.hiss * 0.0035f * hissScale;
        return y;
    }

private:
    float readCubic (float d) const
    {
        const int N = (int) delay.size();
        const int i = (int) d;
        const float f = d - (float) i;
        auto at = [&] (int k) { int j = w - k; while (j < 0) j += N; return delay[(size_t) j]; };
        const float y0 = at (i - 1), y1 = at (i), y2 = at (i + 1), y3 = at (i + 2);
        const float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * f + c2) * f + c1) * f + y1;
    }

    float sr = 44100.0f, baseDelay = 132.0f;
    Rng rng, noise;
    int pass = 0;
    Biquad preEmph, deEmph, headBump, lowLoss, gap1, gap2;
    std::vector<float> delay;
    int w = 0;
    float wowRate = 0.6f, flutterRate = 7.0f, wowPhase = 0.0f, flutterPhase = 0.0f;
    OnePole driftLp, hissLp, hissHp;
    float dropEnv = 1.0f, dropDepth = 1.0f;
    int dropLeft = 0;
    float wowDepth = 0.0f, flutDepth = 0.0f; int depthInit = 0;
};

class KasetEngine
{
public:
    static constexpr int kMaxGen = 5;

    void prepare (double sampleRate)
    {
        sr = (float) sampleRate;
        for (int ch = 0; ch < 2; ++ch)
            for (int g = 0; g < kMaxGen; ++g)
                // Left and right share the deck's speed and dropouts, not its hiss.
                passes[ch][g].prepare (sr, 1983u + 7919u * (uint32_t) g, 5501u + 7919u * (uint32_t) g + (uint32_t) ch * 104729u, g);
        applyTape (TapeType::normal);

        // Azimuth: the right head sits a hair behind the left.
        azimuth.assign ((size_t) (0.001f * sr) + 4, 0.0f);
        aw = 0;
        azimuthSamples = 0.00004f * sr;

        // Fewer generations wait in a plain delay, so the total latency never changes.
        passDelay = (int) std::round (0.003f * sr);
        for (auto& p : pad) p.assign ((size_t) (kMaxGen * passDelay) + 4, 0.0f);
        pw = 0;
    }

    /** In place, stereo. */
    void process (float* L, float* R, int n, const Controls& c)
    {
        if (c.tape != currentTape)
            applyTape (c.tape);
        const int gen = std::clamp (c.generation, 1, kMaxGen);
        const int padSamples = (kMaxGen - gen) * passDelay;
        const int N = (int) pad[0].size();

        for (int s = 0; s < n; ++s)
        {
            float x[2] = { L[s], R[s] };
            for (int ch = 0; ch < 2; ++ch)
            {
                float y = x[ch];
                for (int g = 0; g < gen; ++g)
                    y = passes[ch][g].process (y, c);
                pad[ch][(size_t) pw] = y;
                int j = pw - padSamples;
                if (j < 0) j += N;
                x[ch] = pad[ch][(size_t) j];
            }
            if (++pw == N) pw = 0;

            // Azimuth error on the right channel: a tiny delay that dulls the stereo top.
            azimuth[(size_t) aw] = x[1];
            const int M = (int) azimuth.size();
            const int i = (int) azimuthSamples;
            const float f = azimuthSamples - (float) i;
            auto at = [&] (int k) { int q = aw - k; while (q < 0) q += M; return azimuth[(size_t) q]; };
            const float r = at (i) + f * (at (i + 1) - at (i));
            if (++aw == M) aw = 0;
            L[s] = x[0];
            R[s] = r;
        }
    }

    /** Fixed for every generation: five trips' worth of head delay. */
    int latency() const { return kMaxGen * passDelay; }

private:
    void applyTape (TapeType t)
    {
        currentTape = t;
        // Top end each trip can hold. Type I at 4.76 cm/s; chrome and metal hold more.
        // The first dubs lose the most; later ones lose less, so the far end still works on a mix bus.
        const float type = t == TapeType::normal ? 1.0f : t == TapeType::chrome ? 1.18f : 1.3f;
        for (int ch = 0; ch < 2; ++ch)
            for (int g = 0; g < kMaxGen; ++g)
            {
                const float loss = g <= 2 ? std::pow (0.86f, (float) g) : 0.7396f * std::pow (0.95f, (float) (g - 2));
                passes[ch][g].setTop (15500.0f * loss * type);
            }
    }

    float sr = 44100.0f;
    Pass passes[2][kMaxGen];
    TapeType currentTape = TapeType::normal;
    std::vector<float> azimuth, pad[2];
    int aw = 0, pw = 0, passDelay = 132;
    float azimuthSamples = 0.0f;
};
} // namespace kaset
