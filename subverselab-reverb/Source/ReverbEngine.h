#pragma once

/*  The reverb engine. Plain C++, no JUCE, so the demo tool and the plugin run
    the same code.

    Signal path, per block:

        input -> low/high cut -> predelay line
            -> early reflections (tapped from the predelay line, optional
               flutter comb, short allpass smear)
            -> discrete echoes (tapped further back, each one darker and more
               smeared than the one before)
            -> input diffusion (4 allpasses) -> 8-line feedback delay network
               (Householder mix, 3-band decay per line, slow modulation)
        wet = early + echoes + late, through one peaking "body" filter.

    A Character is a fixed set of numbers for one space. The product ships
    three of them as modes.
*/

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace svrb
{
constexpr float kPi = 3.14159265358979f;

struct Tap { float ms, gain, pan; };  // pan: -1 left .. +1 right

struct Character
{
    const char* name;
    float predelayMs;

    int numTaps;
    std::array<Tap, 16> taps;
    float erLevel, erSmear;          // smear: allpass gain on the early sum
    float flutterMs, flutterFb, flutterLevel;

    int numEchoes;
    std::array<Tap, 6> echoes;
    float echoToLate;

    float sizeMs;                    // mean FDN line length
    float rt60, hfRatio, lfRatio;    // mid decay (s); hi/lo decay as a ratio of it
    float modMs, modHz;
    float lateLevel;

    float lowCutHz, highCutHz;
    float bodyHz, bodyDb, bodyQ;
    float outGain;                   // wet at ~-3 dB of the input at full mix, measured on a vocal+drum stem
};

// ---------------------------------------------------------------------------
// Characters

inline const Character& hammam()
{
    // Marble dome over a stone floor: dense, bright first reflections, a
    // flutter between parallel walls, a ring in the low mids from the dome.
    static const Character c {
        "Hammam", 7.0f,
        12, {{ {3.1f,.90f,-.6f},{5.3f,.82f,.7f},{7.9f,.78f,-.3f},{10.4f,.70f,.4f},
               {13.7f,.66f,-.8f},{16.2f,.60f,.8f},{19.9f,.55f,-.2f},{24.1f,.50f,.3f},
               {28.6f,.46f,-.5f},{33.0f,.42f,.6f},{38.7f,.38f,-.7f},{44.9f,.35f,.1f} }},
        0.85f, 0.55f,
        11.3f, 0.62f, 0.32f,
        0, {}, 0.0f,
        52.0f, 3.0f, 0.78f, 0.9f,
        0.35f, 0.42f,
        0.85f,
        170.0f, 11500.0f,
        760.0f, 5.0f, 1.3f,
        0.61f
    };
    return c;
}

inline const Character& cistern()
{
    // Underground cistern: stone, water, rows of columns. The first sound
    // back is late and sparse (column slaps), the tail is long and dark and
    // moves slowly like water.
    static const Character c {
        "Cistern", 42.0f,
        7, {{ {37.f,.55f,-.5f},{71.f,.44f,.6f},{108.f,.38f,-.2f},{146.f,.31f,.8f},
              {197.f,.25f,-.8f},{262.f,.19f,.3f},{331.f,.13f,-.4f} }},
        0.70f, 0.68f,
        0.0f, 0.0f, 0.0f,
        0, {}, 0.0f,
        118.0f, 6.8f, 0.30f, 1.12f,
        1.25f, 0.17f,
        1.0f,
        85.0f, 6200.0f,
        230.0f, 3.0f, 0.8f,
        0.92f
    };
    return c;
}

inline const Character& valley()
{
    // Open valley: almost no room, the ground right under you, then the far
    // slopes answering one by one, each answer darker and more blurred.
    static const Character c {
        "Valley", 0.0f,
        2, {{ {18.f,.32f,-.2f},{27.f,.22f,.3f} }},
        0.6f, 0.2f,
        0.0f, 0.0f, 0.0f,
        5, {{ {340.f,.60f,-.75f},{610.f,.46f,.70f},{890.f,.33f,-.35f},
              {1310.f,.22f,.45f},{1760.f,.13f,0.0f} }},
        0.45f,
        70.0f, 1.9f, 0.5f, 0.9f,
        0.22f, 0.3f,
        0.35f,
        240.0f, 8200.0f,
        0.0f, 0.0f, 1.0f,
        1.67f
    };
    return c;
}

// ---------------------------------------------------------------------------
// Building blocks

struct OnePole
{
    float a = 0.0f, z = 0.0f;
    void setLowpass (float hz, float sr) { a = std::exp (-2.0f * kPi * hz / sr); }
    float lp (float x) { z = x + a * (z - x); return z; }
    float hp (float x) { return x - lp (x); }
};

struct Delay
{
    std::vector<float> buf;
    int w = 0;
    void setMax (int n) { buf.assign ((size_t) n + 4, 0.0f); w = 0; }
    void clear() { std::fill (buf.begin(), buf.end(), 0.0f); }
    void push (float x) { buf[(size_t) w] = x; if (++w == (int) buf.size()) w = 0; }
    /** Sample written d pushes ago (d >= 1). */
    float at (int d) const
    {
        int i = w - d;
        if (i < 0) i += (int) buf.size();
        return buf[(size_t) i];
    }
    float atFrac (float d) const
    {
        const int i = (int) d;
        const float f = d - (float) i;
        return at (i) + f * (at (i + 1) - at (i));
    }
};

struct Allpass
{
    Delay d;
    int len = 1;
    float g = 0.5f;
    void setup (int samples, float gain) { len = samples < 1 ? 1 : samples; d.setMax (len); g = gain; }
    float process (float x)
    {
        const float delayed = d.at (len);
        const float v = x + g * delayed;
        d.push (v);
        return delayed - g * v;
    }
};

struct Peak
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void setup (float hz, float db, float q, float sr)
    {
        if (hz <= 0.0f || db == 0.0f) { b0 = 1; b1 = b2 = a1 = a2 = 0; return; }
        const float A = std::pow (10.0f, db / 40.0f);
        const float w = 2.0f * kPi * hz / sr, al = std::sin (w) / (2.0f * q), c = std::cos (w);
        const float a0 = 1.0f + al / A;
        b0 = (1.0f + al * A) / a0; b1 = -2.0f * c / a0; b2 = (1.0f - al * A) / a0;
        a1 = -2.0f * c / a0;       a2 = (1.0f - al / A) / a0;
    }
    float process (float x)
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// ---------------------------------------------------------------------------

/** RBJ lowpass / highpass, used for the wet low and high cut. */
struct Biquad
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void set (bool high, float hz, float sr)
    {
        const float w = 2.0f * kPi * std::fmin (hz, 0.45f * sr) / sr, al = std::sin (w) / (2.0f * 0.7071f), c = std::cos (w);
        const float a0 = 1.0f + al;
        if (high) { b0 = (1.0f + c) * 0.5f / a0; b1 = -(1.0f + c) / a0; }
        else      { b0 = (1.0f - c) * 0.5f / a0; b1 =  (1.0f - c) / a0; }
        b2 = b0; a1 = -2.0f * c / a0; a2 = (1.0f - al) / a0;
    }
    float process (float x)
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

/** What the plugin can change while playing. Everything else is the Character. */
struct Controls
{
    float decay = 1.0f;          // multiplies the character's own decay time
    float predelayMs = 0.0f;     // added to the character's own predelay
    float distance = 0.5f;       // 0 near (early reflections up front) .. 1 far (late field, darker)
    float lowCutHz = 20.0f, highCutHz = 20000.0f;
    float width = 1.0f;          // 0 mono .. 1 as built .. 1.5 wider
    bool freeze = false;
    float bpm = 0.0f;            // > 0: discrete echoes land on the beat grid
};

class ReverbEngine
{
public:
    static constexpr int N = 8;
    /** Where the echoes land when synced, in beats. Off the downbeat on purpose. */
    static constexpr float kEchoBeats[6] = { 0.75f, 1.25f, 1.75f, 2.5f, 3.5f, 4.5f };
    static constexpr float kMaxDelayMs = 4700.0f;

    void prepare (double sampleRate, const Character& ch)
    {
        sr = (float) sampleRate;
        c = &ch;

        pre.setMax ((int) (kMaxDelayMs * 0.001f * sr) + 8);

        for (int k = 0; k < 2; ++k)
        {
            lowCut[k].setLowpass (c->lowCutHz, sr);
            highCut[k].setLowpass (c->highCutHz, sr);
            body[k].setup (c->bodyHz, c->bodyDb, c->bodyQ, sr);
            erSmear[k][0].setup (ms (4.3f + 1.1f * k), c->erSmear);
            erSmear[k][1].setup (ms (7.9f - 1.3f * k), c->erSmear);
            flutter[k].setMax (ms (c->flutterMs + 0.4f * k) + 40);
            flutterDamp[k].setLowpass (5000.0f, sr);

            for (int e = 0; e < 6; ++e)
            {
                const float dark = 6500.0f / (1.0f + 0.9f * (float) e);
                echoLp[e][k].setLowpass (dark, sr);
                const float g = std::fmin (0.35f + 0.08f * (float) e, 0.72f);
                echoAp[e][k][0].setup (ms (3.1f + 2.3f * (float) e + 0.7f * k), g);
                echoAp[e][k][1].setup (ms (9.7f + 4.1f * (float) e - 0.9f * k), g);
            }
        }

        const float diffMs[4] = { 4.77f, 3.59f, 12.73f, 9.31f };
        for (int i = 0; i < 4; ++i)
            diffuser[i].setup (ms (diffMs[i] * std::fmax (c->sizeMs / 60.0f, 0.6f)), i < 2 ? 0.75f : 0.625f);

        // Mutually prime-ish ratios spread around 1.
        const float ratio[N] = { 0.62f, 0.71f, 0.79f, 0.88f, 0.97f, 1.09f, 1.21f, 1.37f };
        for (int i = 0; i < N; ++i)
        {
            lineLen[i] = c->sizeMs * ratio[i] * 0.001f * sr;
            line[i].setMax ((int) (lineLen[i] + c->modMs * 0.001f * sr) + 8);
            crossover[i].setLowpass (260.0f, sr);
            lfoPhase[i] = (float) i / (float) N;
        }
        lfoInc = c->modHz / sr;

        applied.decay = applied.distance = applied.lowCutHz = applied.highCutHz = -1.0f;   // force the first update
        update (Controls());
        reset();
    }

    void reset()
    {
        pre.clear();
        for (int k = 0; k < 2; ++k)
        {
            lowCut[k].z = highCut[k].z = flutterDamp[k].z = 0.0f;
            body[k].z1 = body[k].z2 = 0.0f;
            wetLo[k].z1 = wetLo[k].z2 = wetHi[k].z1 = wetHi[k].z2 = 0.0f;
            farLp[k].z = 0.0f;
            flutter[k].clear();
            for (auto& a : erSmear[k]) a.d.clear();
            for (int e = 0; e < 6; ++e) { echoLp[e][k].z = 0.0f; echoAp[e][k][0].d.clear(); echoAp[e][k][1].d.clear(); }
        }
        for (auto& d : diffuser) d.d.clear();
        for (int i = 0; i < N; ++i) { line[i].clear(); crossover[i].z = damp[i].z = state[i] = 0.0f; }
    }

    /** Cheap: recomputes a few coefficients only when a value moved. */
    void update (const Controls& ctl)
    {
        if (ctl.decay != applied.decay || ctl.freeze != applied.freeze)
        {
            const float rt = c->rt60 * ctl.decay;
            for (int i = 0; i < N; ++i)
            {
                if (ctl.freeze) { gLow[i] = gRest[i] = 1.0f; damp[i].a = 0.0f; continue; }
                const float secs = lineLen[i] / sr;
                const float gMid = std::pow (10.0f, -3.0f * secs / rt);
                const float gHi  = std::pow (10.0f, -3.0f * secs / (rt * c->hfRatio));
                gLow[i] = std::pow (10.0f, -3.0f * secs / (rt * c->lfRatio));
                gRest[i] = gMid;
                damp[i].a = (gMid - gHi) / (gMid + gHi);   // one-pole: DC gain 1, Nyquist gain gHi/gMid
            }
        }
        if (ctl.lowCutHz != applied.lowCutHz)
            for (auto& f : wetLo) f.set (true, ctl.lowCutHz, sr);
        if (ctl.highCutHz != applied.highCutHz)
            for (auto& f : wetHi) f.set (false, ctl.highCutHz, sr);
        if (ctl.distance != applied.distance)
        {
            // Far away: less of the first reflections, more of the room, less air.
            const float d = ctl.distance;
            erGain = 1.5f - 1.2f * d;
            lateGainDist = 0.6f + 0.8f * d;
            for (auto& f : farLp) f.setLowpass (18000.0f * std::pow (0.25f, d), sr);
        }
        applied = ctl;
    }

    const Character& character() const { return *c; }

    /** Wet only. in and out may alias. */
    void process (const float* inL, const float* inR, float* outL, float* outR, int n)
    {
        const Character& ch = *c;
        const Controls& ctl = applied;
        const float maxD = kMaxDelayMs * 0.001f * sr - 4.0f;
        const float preD = std::fmin ((ch.predelayMs + ctl.predelayMs) * 0.001f * sr, maxD * 0.5f);
        const float inGain = ctl.freeze ? 0.0f : 1.0f;

        float echoD[6], echoG[6];
        for (int e = 0; e < ch.numEchoes; ++e)
        {
            // Decay reaches the echoes too: shorter decay, the far slopes fade sooner.
            echoG[e] = std::pow (ch.echoes[(size_t) e].gain, 1.0f / ctl.decay);
            const float msAt = ctl.bpm > 0.0f ? kEchoBeats[e] * 60000.0f / ctl.bpm : ch.echoes[(size_t) e].ms;
            echoD[e] = std::fmin (preD + msAt * 0.001f * sr + 1.0f, maxD);
        }
        const float lateGain = ch.lateLevel * 0.35f * lateGainDist;
        const float wMid = 1.0f, wSide = ctl.width;

        for (int s = 0; s < n; ++s)
        {
            float x[2] = { inL[s] * inGain, inR[s] * inGain };
            for (int k = 0; k < 2; ++k)
                x[k] = highCut[k].lp (lowCut[k].hp (x[k]));
            pre.push (0.5f * (x[0] + x[1]));

            // Early reflections.
            float er[2] = { 0, 0 };
            for (int t = 0; t < ch.numTaps; ++t)
            {
                const auto& tap = ch.taps[(size_t) t];
                const float v = pre.atFrac (preD + tap.ms * 0.001f * sr + 1.0f) * tap.gain;
                er[0] += v * (1.0f - tap.pan) * 0.5f;
                er[1] += v * (1.0f + tap.pan) * 0.5f;
            }

            // Discrete echoes.
            float echo[2] = { 0, 0 };
            for (int e = 0; e < ch.numEchoes; ++e)
            {
                const auto& t = ch.echoes[(size_t) e];
                const float v = pre.atFrac (echoD[e]) * echoG[e];
                for (int k = 0; k < 2; ++k)
                {
                    const float p = k == 0 ? (1.0f - t.pan) * 0.5f : (1.0f + t.pan) * 0.5f;
                    float y = echoLp[e][k].lp (v * p);
                    y = echoAp[e][k][1].process (echoAp[e][k][0].process (y));
                    echo[k] += y;
                }
            }

            for (int k = 0; k < 2; ++k)
            {
                float y = er[k];
                if (ch.flutterFb > 0.0f)
                {
                    const float fb = flutterDamp[k].lp (flutter[k].at ((int) (ch.flutterMs * 0.001f * sr) + k * 17));
                    flutter[k].push (y + ch.flutterFb * fb);
                    y += ch.flutterLevel * fb;
                }
                y = erSmear[k][1].process (erSmear[k][0].process (y));
                er[k] = y * ch.erLevel * erGain;
            }

            // Late field.
            float lateIn = pre.atFrac (preD + 1.0f) * 0.6f + 0.25f * (er[0] + er[1])
                         + ch.echoToLate * 0.5f * (echo[0] + echo[1]);
            for (auto& d : diffuser)
                lateIn = d.process (lateIn);

            float out[N];
            for (int i = 0; i < N; ++i)
            {
                float ph = lfoPhase[i] + lfoInc;
                if (ph >= 1.0f) ph -= 1.0f;
                lfoPhase[i] = ph;
                const float mod = ch.modMs * 0.001f * sr * 0.5f * (1.0f + std::sin (2.0f * kPi * ph));
                out[i] = line[i].atFrac (lineLen[i] + mod);
            }

            float sum = 0.0f;
            for (int i = 0; i < N; ++i)
            {
                // 3-band decay: lows below the crossover, the rest through a
                // one-pole whose top end dies faster.
                const float lo = crossover[i].lp (out[i]);
                const float rest = out[i] - lo;
                const float a = damp[i].a;
                damp[i].z = (1.0f - a) * rest + a * damp[i].z;
                state[i] = gLow[i] * lo + gRest[i] * damp[i].z;
                sum += state[i];
            }
            const float h = sum * (2.0f / (float) N);

            constexpr float sign[N] = { 1, -1, 1, -1, 1, 1, -1, -1 };
            float lateL = 0.0f, lateR = 0.0f;
            for (int i = 0; i < N; ++i)
            {
                line[i].push (state[i] - h + sign[i] * lateIn * 0.5f);
                lateL += (i & 1 ? -1.0f : 1.0f) * state[i];
                lateR += (i < 4 ? 1.0f : -1.0f) * state[i] * (i & 2 ? -1.0f : 1.0f);
            }

            float y[2] = { er[0] + echo[0] + lateL * lateGain, er[1] + echo[1] + lateR * lateGain };
            for (int k = 0; k < 2; ++k)
                y[k] = farLp[k].lp (wetHi[k].process (wetLo[k].process (body[k].process (y[k])))) * ch.outGain;

            const float m = 0.5f * (y[0] + y[1]) * wMid, sd = 0.5f * (y[0] - y[1]) * wSide;
            outL[s] = m + sd;
            outR[s] = m - sd;
        }
    }

private:
    int ms (float m) const { return (int) std::lround (m * 0.001f * sr); }

    float sr = 44100.0f;
    const Character* c = nullptr;
    Controls applied;
    float erGain = 1.0f, lateGainDist = 1.0f;

    Delay pre;
    OnePole lowCut[2], highCut[2], farLp[2];
    Biquad wetLo[2], wetHi[2];
    Peak body[2];
    Allpass erSmear[2][2];
    Delay flutter[2];
    OnePole flutterDamp[2];
    OnePole echoLp[6][2];
    Allpass echoAp[6][2][2];
    Allpass diffuser[4];

    Delay line[N];
    float lineLen[N] {};
    OnePole crossover[N], damp[N];
    float gLow[N] {}, gRest[N] {}, state[N] {};
    float lfoPhase[N] {}, lfoInc = 0.0f;
};

inline const Character& characterAt (int i)
{
    return i == 1 ? cistern() : i == 2 ? valley() : hammam();
}
constexpr int kNumCharacters = 3;
} // namespace svrb
