// SVL DSP building blocks: oscillators (polyBLEP virtual analog, noise, Karplus-Strong string), state-variable filter,
// ADSR with curves, smoothers, denormal guard. Plain C++17, no JUCE. Nothing here allocates outside prepare().
#pragma once
#include "SvlParams.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
#if defined(__SSE__) || defined(_M_X64) || defined(__x86_64__)
 #include <xmmintrin.h>
 #define SVL_X86 1
#endif

namespace svl {

constexpr double kPi = 3.14159265358979323846;
inline float dbToGain (float db) { return std::pow (10.0f, db * 0.05f); }
inline float pitchToHz (double pitch) { return (float) (440.0 * std::exp2 ((pitch - 69.0) / 12.0)); }

// Flush-to-zero / denormals-are-zero for the scope of one process() call; the previous mode is restored afterwards.
struct DenormalGuard
{
#if SVL_X86
    unsigned int old;
    DenormalGuard() : old (_mm_getcsr()) { _mm_setcsr (old | 0x8040); }
    ~DenormalGuard() { _mm_setcsr (old); }
#elif defined(__aarch64__)
    uint64_t old;
    DenormalGuard()
    {
        asm volatile ("mrs %0, fpcr" : "=r" (old));
        const uint64_t on = old | (1ull << 24);
        asm volatile ("msr fpcr, %0" : : "r" (on));
    }
    ~DenormalGuard() { asm volatile ("msr fpcr, %0" : : "r" (old)); }
#else
    DenormalGuard() {}
#endif
};

// ---------------------------------------------------------------- deterministic random
struct Rng
{
    uint64_t s = 0x9E3779B97F4A7C15ull;
    void seed (uint64_t v)
    {
        uint64_t z = v + 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; z ^= z >> 31;
        s = z ? z : 0x9E3779B97F4A7C15ull;
    }
    float uni()   // [-1, 1)
    {
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        const uint64_t r = s * 0x2545F4914F6CDD1Dull;
        return (float) ((double) (r >> 11) * (1.0 / 9007199254740992.0) * 2.0 - 1.0);
    }
};

// ---------------------------------------------------------------- linear-ramp smoother (per sample, block-size independent)
struct Smoother
{
    float cur = 0, target = 0, step = 0;
    int remain = 0, rampLen = 1;
    void setRampLength (int samples) { rampLen = samples < 1 ? 1 : samples; }
    void reset (float v) { cur = target = v; step = 0; remain = 0; }
    void set (float v)
    {
        if (v == target) return;
        target = v;
        remain = rampLen;
        step = (target - cur) / (float) rampLen;
    }
    float next()
    {
        if (remain > 0) { cur += step; if (--remain == 0) cur = target; }
        return cur;
    }
};

// ---------------------------------------------------------------- polyBLEP / polyBLAMP residuals (2-point)
inline float polyBlep (float t, float dt)
{
    if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}
inline float polyBlamp (float t, float dt)
{
    if (t < dt) { t = t / dt - 1.0f; return -(1.0f / 3.0f) * t * t * t; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt + 1.0f; return (1.0f / 3.0f) * t * t * t; }
    return 0.0f;
}

// Virtual-analog oscillator: saw / square (PWM) / triangle / sine. Phase in [0, 1).
struct VaOsc
{
    float phase = 0;
    void reset (float ph = 0) { phase = ph; }
    // naive = true is for the selftest's aliasing baseline only.
    float next (OscType type, float dt, float pw, bool naive = false, float pm = 0)
    {
        float y = 0;
        float t = phase;
        if (pm != 0) { t += pm; t -= std::floor (t); }   // phase modulation (FM), in cycles
        switch (type)
        {
            case OscType::saw:
                y = 2.0f * t - 1.0f;
                if (! naive) y -= polyBlep (t, dt);
                break;
            case OscType::square:
            {
                y = t < pw ? 1.0f : -1.0f;
                if (! naive)
                {
                    float t2 = t - pw; if (t2 < 0) t2 += 1.0f;
                    y += polyBlep (t, dt) - polyBlep (t2, dt);
                }
                y -= 2.0f * pw - 1.0f;   // remove the duty-cycle DC
                break;
            }
            case OscType::triangle:
            {
                y = 1.0f - 4.0f * std::fabs (t - 0.5f);   // -1 at 0, +1 at 0.5
                if (! naive)
                {
                    float t2 = t - 0.5f; if (t2 < 0) t2 += 1.0f;
                    y += 4.0f * dt * (polyBlamp (t, dt) - polyBlamp (t2, dt));   // 4, not 8: measured optimum (selftest 1)
                }
                break;
            }
            case OscType::sine: y = (float) std::sin (2.0 * kPi * t); break;
            default: break;
        }
        phase += dt;
        if (phase >= 1.0f) phase -= 1.0f;
        return y;
    }
};

// White / pink (Paul Kellet's refined filter) noise.
struct Noise
{
    Rng rng;
    float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
    void reset (uint64_t seed) { rng.seed (seed); b0 = b1 = b2 = b3 = b4 = b5 = b6 = 0; }
    float white() { return rng.uni(); }
    float pink()
    {
        const float w = rng.uni();
        b0 = 0.99886f * b0 + w * 0.0555179f; b1 = 0.99332f * b1 + w * 0.0750759f;
        b2 = 0.96900f * b2 + w * 0.1538520f; b3 = 0.86650f * b3 + w * 0.3104856f;
        b4 = 0.55000f * b4 + w * 0.5329522f; b5 = -0.7616f * b5 - w * 0.0168980f;
        const float o = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362f;
        b6 = w * 0.115926f;
        return o * 0.11f;
    }
};

// Karplus-Strong string. Loop: delay (linear-interpolated fraction) -> two-tap loss filter with stretch S -> gain g.
// Loop delay = d + S samples = one period. g is set so the fundamental reaches -60 dB after `decay` seconds.
struct KsString
{
    std::vector<float> buf;   // allocated in prepare()
    int mask = 0, w = 0;
    float d = 100, S = 0.5f, g = 0.99f, prev = 0;
    float sr = 44100;
    void prepare (double sampleRate)
    {
        sr = (float) sampleRate;
        int need = (int) std::ceil (sampleRate / 15.0) + 8, n = 1;
        while (n < need) n <<= 1;
        buf.assign ((size_t) n, 0.0f);
        mask = n - 1; w = 0; prev = 0;
    }
    void tune (float hz, float decaySec, float tone)
    {
        hz = std::fmax (16.0f, std::fmin (hz, sr * 0.45f));
        const float P = sr / hz;
        S = 0.5f - 0.45f * tone;                       // tone 0: classic averaging, tone 1: little loss per pass
        d = std::fmax (1.0f, P - S);
        const double w0 = 2.0 * kPi * hz / sr;
        const double h = std::sqrt ((1 - S) * (1 - S) + S * S + 2.0 * S * (1 - S) * std::cos (w0));   // |H(f0)|
        const double perPeriod = std::pow (10.0, -3.0 / (hz * std::fmax (0.01f, decaySec)));
        g = (float) std::fmin (0.99999, perPeriod / std::fmax (1e-6, h));
    }
    // Writes a filtered noise burst of one period into the line (adds to what is ringing, so a re-pluck does not click).
    void pluck (Rng& rng, float amp, float tone)
    {
        const int n = std::max (2, std::min ((int) d + 1, mask));
        const float a = 0.15f + 0.8f * tone;          // burst brightness (one-pole low-pass)
        float lp = 0, mean = 0;
        // burst values are generated twice from a copy of the generator so the mean can be removed without a buffer
        Rng probe = rng;
        for (int i = 0; i < n; ++i) { lp += a * (probe.uni() - lp); mean += lp; }
        mean /= (float) n; lp = 0;
        for (int i = 0; i < n; ++i)
        {
            lp += a * (rng.uni() - lp);
            buf[(size_t) ((w - n + i) & mask)] += amp * (lp - mean) * 2.0f;
        }
    }
    void clear() { std::fill (buf.begin(), buf.end(), 0.0f); prev = 0; }
    float next()
    {
        const int di = (int) d;
        const float fr = d - (float) di;
        const float a = buf[(size_t) ((w - di) & mask)], b = buf[(size_t) ((w - di - 1) & mask)];
        const float out = a + fr * (b - a);
        buf[(size_t) w] = g * ((1 - S) * out + S * prev);
        prev = out;
        w = (w + 1) & mask;
        return out;
    }
};

// ---------------------------------------------------------------- 2x -> 1x half-band decimator (stereo)
// Linear-phase FIR, 47 taps at the 2x rate (Kaiser beta 8): flat to 18 kHz at 44.1 kHz (-0.04 dB), -1.1 dB at 20 kHz.
// Group delay 11.5 samples at the base rate, the same for every voice and note.
struct HalfbandDecimator
{
    static constexpr int N = 47, M = (N - 1) / 2, kPairs = (M + 1) / 2;
    float hist[2][2 * N] {};
    int pos = 0;
    static const float* oddTaps()   // h[M + k] for k = 1, 3, 5, ...
    {
        static const std::array<float, kPairs> t = []
        {
            auto i0 = [] (double x) { double s = 1, u = 1; for (int k = 1; k < 30; ++k) { u *= (x / 2 / k) * (x / 2 / k); s += u; } return s; };
            std::array<double, N> h {}; double sum = 0;
            for (int i = 0; i < N; ++i)
            {
                const int k = i - M;
                const double sinc = k == 0 ? 0.5 : std::sin (kPi * k / 2.0) / (kPi * k);
                h[(size_t) i] = sinc * i0 (8.0 * std::sqrt (1.0 - std::pow ((double) k / M, 2))) / i0 (8.0);
                sum += h[(size_t) i];
            }
            std::array<float, kPairs> o {};
            for (int j = 0; j < kPairs; ++j) o[(size_t) j] = (float) (h[(size_t) (M + 1 + 2 * j)] / sum);
            centre() = (float) (h[(size_t) M] / sum);
            return o;
        }();
        return t.data();
    }
    static float& centre() { static float c = 0.5f; return c; }
    void reset() { for (auto& c : hist) for (auto& v : c) v = 0; pos = 0; }
    // push two 2x-rate samples per channel, get one base-rate sample per channel
    void process (const float* in2L, const float* in2R, float& outL, float& outR)
    {
        const float* t = oddTaps();
        const float c = centre();
        for (int k = 0; k < 2; ++k)
        {
            hist[0][pos] = hist[0][pos + N] = in2L[k];
            hist[1][pos] = hist[1][pos + N] = in2R[k];
            pos = pos + 1 == N ? 0 : pos + 1;
        }
        for (int ch = 0; ch < 2; ++ch)
        {
            const float* x = hist[ch] + pos;   // x[0] oldest .. x[N-1] newest
            float acc = c * x[M];
            for (int j = 0; j < kPairs; ++j) acc += t[j] * (x[M + 1 + 2 * j] + x[M - 1 - 2 * j]);
            (ch == 0 ? outL : outR) = acc;
        }
    }
};

// ---------------------------------------------------------------- state-variable filter (Simper / Cytomic TPT), stereo
struct Svf
{
    float ic1[2] {}, ic2[2] {};
    float a1 = 1, a2 = 0, a3 = 0, k = 2;
    void reset() { ic1[0] = ic1[1] = ic2[0] = ic2[1] = 0; }
    void setCoeffs (float hz, float reso, float sr)
    {
        hz = std::fmax (10.0f, std::fmin (hz, sr * 0.45f));
        const float g = (float) std::tan (kPi * hz / sr);
        k = 2.0f - 1.98f * std::fmin (1.0f, std::fmax (0.0f, reso));
        a1 = 1.0f / (1.0f + g * (g + k)); a2 = g * a1; a3 = g * a2;
    }
    float process (int ch, float v0, FilterMode mode)
    {
        const float v3 = v0 - ic2[ch];
        const float v1 = a1 * ic1[ch] + a2 * v3;
        const float v2 = ic2[ch] + a2 * ic1[ch] + a3 * v3;
        ic1[ch] = 2 * v1 - ic1[ch]; ic2[ch] = 2 * v2 - ic2[ch];
        switch (mode)
        {
            case FilterMode::lp: return v2;
            case FilterMode::bp: return v1;
            case FilterMode::hp: return v0 - k * v1 - v2;
            case FilterMode::notch: return v0 - k * v1;
            default: return v2;
        }
    }
};

// tanh with first-order antiderivative anti-aliasing (no oversampling, half a sample of delay).
struct AdaaTanh
{
    float xp = 0;
    static double logcosh (double x)
    {
        const double a = std::fabs (x);
        return a + std::log1p (std::exp (-2.0 * a)) - 0.69314718055994530942;
    }
    void reset() { xp = 0; }
    float process (float x)
    {
        const float dx = x - xp;
        float y;
        if (std::fabs (dx) > 1e-4f) y = (float) ((logcosh (x) - logcosh (xp)) / dx);
        else y = std::tanh (0.5f * (x + xp));
        xp = x;
        return y;
    }
};

// ---------------------------------------------------------------- ADSR with curves
// curve c in [-1, 1]: 0 linear, > 0 fast start (analog-like), < 0 slow start. Attack and release start from the current level.
inline float curveShape (float p, float c)
{
    if (std::fabs (c) < 1e-3f) return p;
    const float kk = 6.0f * std::fabs (c);
    if (c > 0) return (1.0f - std::exp (-kk * p)) / (1.0f - std::exp (-kk));
    return (std::exp (kk * p) - 1.0f) / (std::exp (kk) - 1.0f);
}

struct Adsr
{
    enum Stage { idle, attack, decay, sustain, release };
    Stage stage = idle;
    float level = 0, from = 0, to = 0, pos = 0, inc = 0, curve = 0;
    float a = 0.003f, d = 0.3f, s = 1, r = 0.15f, ca = 0, cd = 0.5f, cr = 0.5f;
    float sr = 44100;
    void setRate (float sampleRate) { sr = sampleRate; }
    void set (float A, float D, float Sus, float R, float CA, float CD, float CR)
    { a = A; d = D; s = Sus; r = R; ca = CA; cd = CD; cr = CR; }
    void begin (Stage st, float target, float seconds, float c)
    {
        stage = st; from = level; to = target; pos = 0; curve = c;
        inc = seconds <= 0 ? 1.0f : 1.0f / (seconds * sr);
    }
    void trigger() { begin (attack, 1.0f, a, ca); }
    void noteOff() { if (stage != idle) begin (release, 0.0f, r, cr); }
    void kill() { stage = idle; level = 0; }
    bool active() const { return stage != idle; }
    float next()
    {
        switch (stage)
        {
            case idle: return 0;
            case sustain: level = s; return level;
            default: break;
        }
        pos += inc;
        if (pos >= 1.0f)
        {
            level = to;
            if (stage == attack) begin (decay, s, d, cd);
            else if (stage == decay) stage = sustain;
            else if (stage == release) { stage = idle; level = 0; }
            return level;
        }
        level = from + (to - from) * curveShape (pos, curve);
        return level;
    }
};

} // namespace svl
