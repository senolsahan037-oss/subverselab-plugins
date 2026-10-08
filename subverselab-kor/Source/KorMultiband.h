// KOR v2: three heat bands (LOW / MID / HIGH) around the measured single-band kor::Engine, a phase-coherent
// dry/wet (the ember) and a true-peak output limiter at -1 dBTP. Plain C++17, no JUCE. See SPEC.md "KOR v2".
//
//   x -> LR4 crossover 200 Hz / 3 kHz -> per band: heat gain (-18 .. +24 dB, heat^0.4 taper) -> kor::Engine -> sum = wet
//   x -> the same crossover summed untouched (allpass), delayed by the engine latency = dry
//   out = dry + mix (wet - dry) -> true-peak limiter (4x interpolated detection, lookahead) -> clamp -1 dBFS
#pragma once

#include "KorEngine.h"
#include <array>
#include <cmath>
#include <vector>

namespace kor
{
// Per-band loudness trim (dB) vs heat 0, 0.1, ..., 1, by tools/make_bandtrim.py: the engine's auto-gain table was
// made on full-band pink, a single band needs this on top so one ring 0 -> 100 keeps the loudness (N4).
static constexpr int kTrimN = 11;
static constexpr float kBandTrimDb[3][kTrimN] = {
    { 0.000f, 0.014f, -0.031f, -0.133f, -0.332f, -0.618f, -0.971f, -1.366f, -1.815f, -2.250f, -2.629f },
    { 0.000f, 0.020f, -0.077f, -0.295f, -0.700f, -1.245f, -1.860f, -2.434f, -2.945f, -3.380f, -3.734f },
    { 0.000f, 0.018f, -0.096f, -0.349f, -0.812f, -1.434f, -2.143f, -2.801f, -3.350f, -3.788f, -4.127f },
};
inline float bandTrimDb (int b, float heat) noexcept
{
    const float f = std::clamp (heat, 0.0f, 1.0f) * (float) (kTrimN - 1);
    const int i = std::min ((int) f, kTrimN - 2);
    return kBandTrimDb[b][i] + (f - (float) i) * (kBandTrimDb[b][i + 1] - kBandTrimDb[b][i]);
}

struct MbParams
{
    float low = 0.30f, mid = 0.30f, high = 0.20f;   // heat 0 .. 1
    float mix = 1.0f;                                  // 0 .. 1 (the ember)
};

class Multiband
{
public:
    static constexpr int kBands = 3;
    static constexpr double kXoLow = 200.0, kXoHigh = 3000.0;
    static constexpr float kCharacter = 0.35f;
    static constexpr float kCeilingDb = -1.0f;                  // true peak
    static constexpr int kTpTaps = 8, kTpPhases = 4;            // 4x interpolated peak detection
    static constexpr int kTpDelay = kTpTaps / 2;                // samples
    static constexpr int kLook = 32;                            // limiter lookahead (samples, every rate)
    static constexpr int latencySamples() noexcept { return Engine::latencySamples() + kTpDelay + kLook - 1; }

    void prepare (double sampleRate, int maxBlock, int channels)
    {
        fs = sampleRate; maxBlk = std::max (1, maxBlock); numCh = std::clamp (channels, 1, Engine::kMaxChannels);
        for (auto& e : eng) e.prepare (fs, maxBlk, numCh);
        dryEng.prepare (fs, maxBlk, numCh);
        { Params dp; dp.driveDb = 0.0f; dp.character = kCharacter; dp.toneDb = 0.0f; dp.ceilingDb = 0.0f; dp.mix = 1.0f; dp.autoGain = false; dp.in = true; dryEng.setParams (dp); }
        for (auto& b : band) for (auto& v : b) v.assign ((size_t) maxBlk, 0.0f);
        for (auto& v : dryBuf) v.assign ((size_t) maxBlk, 0.0f);
        designFilters();
        // The ember follows the music (2026-10-08): 15 ms attack, 250 ms release on the band power. Display only.
        glowCoef = (float) (1.0 - std::exp (-1.0 / (0.015 * fs)));
        glowRelCoef = (float) (1.0 - std::exp (-1.0 / (0.250 * fs)));
        relCoef = (float) (1.0 - std::exp (-1.0 / (0.050 * fs)));
        smoothCoef = (float) (1.0 - std::exp (-1.0 / (0.020 * fs)));
        designTpFir();
        reset();
    }

    void reset() noexcept
    {
        for (auto& e : eng) e.reset();
        dryEng.reset();
        for (auto& c : ch) c.clear();   // states only: the coefficients stay (designFilters)
        hist.fill ({}); histPos = 0;
        rLine.fill (1.0f); boxLine.fill (1.0f); for (auto& d : audDelay) d.fill (0.0f); audPos = 0;
        rPos = 0; boxSum = (double) kLook; hRel = 1.0f;
        for (auto& a : aLine) a.fill (0.0f);
        aPos = 0;
        glow.fill (0.0f);
        sMix = tMix; first = true;
    }

    void setParams (const MbParams& p) noexcept
    {
        const float h[kBands] = { p.low, p.mid, p.high };
        for (int b = 0; b < kBands; ++b)
        {
            const float heat = std::clamp (h[b], 0.0f, 1.0f);
            // Taper (2026-10-08, panel v4): drive = -18 + 42 * heat^0.4 dB. The linear map left the first half of each
            // ring inaudible (change vs dry below -45 dB up to heat 0.5 on drums and a master); with the taper heat 0.1
            // is just audible, the default 0.3 is a clear warmth and the top keeps the old +24 dB.
            const float driveDb = -18.0f + 42.0f * std::pow (heat, 0.4f);
            Params ep;
            ep.driveDb = std::max (driveDb, 0.0f);
            ep.character = kCharacter; ep.toneDb = 0.0f; ep.ceilingDb = 0.0f; ep.mix = 1.0f; ep.autoGain = true; ep.in = true;
            eng[b].setParams (ep);
            preTarget[b] = driveDb < 0.0f ? std::pow (10.0f, driveDb / 20.0f) : 1.0f;
            trimTarget[b] = std::pow (10.0f, bandTrimDb (b, heat) / 20.0f);
            glowGain[b] = std::pow (10.0f, driveDb / 20.0f);
        }
        tMix = std::clamp (p.mix, 0.0f, 1.0f);
        if (first) { for (int b = 0; b < kBands; ++b) { pre[b] = preTarget[b]; trim[b] = trimTarget[b]; } sMix = tMix; first = false; }
    }

    float glowOf (int b) const noexcept { return b >= 0 && b < kBands ? glow[(size_t) b] : 0.0f; }

    void process (float* const* io, int nCh, int n) noexcept
    {
        nCh = std::min (nCh, numCh);
        int done = 0;
        while (done < n)
        {
            const int len = std::min (maxBlk, n - done);
            processChunk (io, nCh, done, len);
            done += len;
        }
    }

private:
    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1 = 0, z2 = 0;
        inline double run (double x) noexcept
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };
    struct Lr4
    {
        Biquad a, b;
        inline double run (double x) noexcept { return b.run (a.run (x)); }
        void clear() noexcept { a.z1 = a.z2 = b.z1 = b.z2 = 0.0; }
    };

    struct Chan
    {
        Lr4 lp1, hp1, lp2, hp2, apL_lp, apL_hp;           // wet crossover; AP3k on the low band = LP3k + HP3k
        Lr4 dlp1, dhp1, dlp2, dhp2, dapL_lp, dapL_hp;       // dry path: the same crossover (incl. AP3k on the low band), summed untouched
        void clear() noexcept
        {
            for (Lr4* f : { &lp1, &hp1, &lp2, &hp2, &apL_lp, &apL_hp, &dlp1, &dhp1, &dlp2, &dhp2, &dapL_lp, &dapL_hp }) f->clear();
        }
    };

    static Biquad butter (double fs, double f, bool high)
    {
        const double w = 2.0 * 3.14159265358979323846 * f / fs, cw = std::cos (w), sw = std::sin (w);
        const double q = 0.70710678118654752440, al = sw / (2.0 * q), a0 = 1.0 + al;
        Biquad bq;
        if (high) { bq.b0 = (1 + cw) / 2; bq.b1 = -(1 + cw); bq.b2 = (1 + cw) / 2; }
        else      { bq.b0 = (1 - cw) / 2; bq.b1 = 1 - cw;    bq.b2 = (1 - cw) / 2; }
        bq.b0 /= a0; bq.b1 /= a0; bq.b2 /= a0; bq.a1 = -2 * cw / a0; bq.a2 = (1 - al) / a0;
        return bq;
    }
    static Lr4 lr4 (double fs, double f, bool high) { Lr4 l; l.a = butter (fs, f, high); l.b = l.a; return l; }

    void designFilters()
    {
        for (auto& c : ch)
        {
            c.lp1 = lr4 (fs, kXoLow, false);  c.hp1 = lr4 (fs, kXoLow, true);
            c.lp2 = lr4 (fs, kXoHigh, false); c.hp2 = lr4 (fs, kXoHigh, true);
            c.apL_lp = lr4 (fs, kXoHigh, false); c.apL_hp = lr4 (fs, kXoHigh, true);
            c.dlp1 = c.lp1; c.dhp1 = c.hp1; c.dlp2 = c.lp2; c.dhp2 = c.hp2; c.dapL_lp = c.apL_lp; c.dapL_hp = c.apL_hp;
        }
    }

    void designTpFir()
    {
        // 4x interpolation for peak detection only: windowed sinc, 8 taps per phase (phase 0 = the sample itself).
        for (int p = 0; p < kTpPhases; ++p)
        {
            double sum = 0;
            for (int k = 0; k < kTpTaps; ++k)
            {
                const double t = (double) (k - kTpTaps / 2 + 1) - (double) p / kTpPhases;   // taps around the point
                const double s = std::abs (t) < 1e-12 ? 1.0 : std::sin (3.14159265358979323846 * t) / (3.14159265358979323846 * t);
                const double wn = 0.5 + 0.5 * std::cos (3.14159265358979323846 * t / (kTpTaps / 2 + 0.5));
                tpFir[(size_t) p][(size_t) k] = (float) (s * wn); sum += s * wn;
            }
            for (int k = 0; k < kTpTaps; ++k) tpFir[(size_t) p][(size_t) k] = (float) (tpFir[(size_t) p][(size_t) k] / sum);
        }
    }

    void processChunk (float* const* io, int nCh, int off, int len) noexcept
    {
        for (int b = 0; b < kBands; ++b) trim[b] += (trimTarget[b] - trim[b]) * std::min (1.0f, smoothCoef * (float) len);
        // 1. Split into bands (with the heat pre-gain) and build the dry path.
        for (int c = 0; c < nCh; ++c)
        {
            Chan& s = ch[(size_t) c];
            float* L = band[0][(size_t) c].data(); float* M = band[1][(size_t) c].data(); float* H = band[2][(size_t) c].data();
            float* D = dryBuf[(size_t) c].data();
            const float* x = io[c] + off;
            float p0 = pre[0], p1 = pre[1], p2 = pre[2];
            for (int i = 0; i < len; ++i)
            {
                p0 += (preTarget[0] - p0) * smoothCoef; p1 += (preTarget[1] - p1) * smoothCoef; p2 += (preTarget[2] - p2) * smoothCoef;
                const double v = (double) x[i];
                const double lo = s.lp1.run (v), rest = s.hp1.run (v);
                const double mi = s.lp2.run (rest), hi = s.hp2.run (rest);
                const double loA = s.apL_lp.run (lo) + s.apL_hp.run (lo);
                L[i] = (float) loA * p0; M[i] = (float) mi * p1; H[i] = (float) hi * p2;
                const double dlo = s.dlp1.run (v), drest = s.dhp1.run (v);
                D[i] = (float) ((s.dapL_lp.run (dlo) + s.dapL_hp.run (dlo) + s.dlp2.run (drest) + s.dhp2.run (drest)) * kDryScale);
                // glow: band level after the heat drive (dBFS of the drive-scaled band)
                if (c == 0)
                {
                    const float g0 = (float) loA * glowGain[0], g1 = (float) mi * glowGain[1], g2 = (float) hi * glowGain[2];
                    const float q[3] = { g0 * g0, g1 * g1, g2 * g2 };
                    for (int k = 0; k < 3; ++k) gms[k] += (q[k] - gms[k]) * (q[k] > gms[k] ? glowCoef : glowRelCoef);
                }
            }
            if (c == nCh - 1) { pre[0] = p0; pre[1] = p1; pre[2] = p2; }
        }
        for (int b = 0; b < kBands; ++b)
        {
            const float rmsDb = 10.0f * std::log10 (gms[b] + 1e-20f);
            glow[(size_t) b] = std::clamp ((rmsDb + 40.0f) / 34.0f, 0.0f, 1.0f);   // -40 dB = dark, -6 dB = white hot (was -18/18: a drum loop at the default heat stayed dark)
        }

        // 2. Each band through its engine (the engine's own latency is the same for all three).
        for (int b = 0; b < kBands; ++b)
        {
            float* ptrs[Engine::kMaxChannels] = { band[(size_t) b][0].data(), band[(size_t) b][1].data() };
            eng[(size_t) b].process (ptrs, nCh, len);
        }
        // The dry path goes through an engine of its own at -60 dB, where the shaper is linear (THD < -120 dB):
        // the same ADAA filtering and fractional delay as the wet bands (3/16 sample, -0.12 dB at 20 kHz), so
        // dry/wet mixes without comb filtering (N2).
        {
            float* ptrs[Engine::kMaxChannels] = { dryBuf[0].data(), dryBuf[1].data() };
            dryEng.process (ptrs, nCh, len);
        }

        // 3. Undo the pre-gain, sum, mix with the delayed dry, limit.
        for (int i = 0; i < len; ++i)
        {
            sMix += (tMix - sMix) * smoothCoef;
            float out[Engine::kMaxChannels] = { 0, 0 };
            for (int c = 0; c < nCh; ++c)
            {
                const float wet = band[0][(size_t) c][(size_t) i] / postGain (0) + band[1][(size_t) c][(size_t) i] / postGain (1)
                                + band[2][(size_t) c][(size_t) i] / postGain (2);
                const float dry = dryBuf[(size_t) c][(size_t) i] * (1.0f / kDryScale);
                out[c] = dry + sMix * (wet - dry);
            }
            limit (out, nCh);
            for (int c = 0; c < nCh; ++c) io[c][off + i] = out[c];
        }
    }

    // The pre-gain of a band, as it was when the sample now leaving its engine went in. The pre-gain is smoothed
    // over 20 ms and the engine latency is ~1 ms, so the current value is used; the error is bounded by one
    // latency's worth of smoothing and is zero once a heat move has settled.
    inline float postGain (int b) const noexcept { return pre[b] / trim[b]; }

    void limit (float* s, int nCh) noexcept
    {
        // Detection: 4x interpolated peak of the sample kTpDelay back (its neighbours are in hist).
        hist[(size_t) histPos] = { s[0], nCh > 1 ? s[1] : s[0] };
        const int newest = histPos;
        if (++histPos == kTpTaps) histPos = 0;
        float peak = 0.0f;
        for (int c = 0; c < nCh; ++c)
            for (int p = 0; p < kTpPhases; ++p)
            {
                float acc = 0.0f;
                for (int k = 0; k < kTpTaps; ++k)
                {
                    const int idx = (newest - k + kTpTaps) % kTpTaps;      // hist[newest - k] = x[n - k]
                    acc += tpFir[(size_t) p][(size_t) (kTpTaps - 1 - k)] * hist[(size_t) idx][(size_t) c];
                }
                peak = std::max (peak, std::abs (acc));
            }
        const float ceil = kCeilLin;
        const float r = peak > ceil ? ceil / peak : 1.0f;

        // The audio sample that peak belongs to: x[n - kTpDelay + 1] .. use a delay line of kTpDelay for the audio.
        float a[Engine::kMaxChannels];
        for (int c = 0; c < nCh; ++c) { a[c] = aLine[(size_t) c][(size_t) aPos]; aLine[(size_t) c][(size_t) aPos] = s[c]; }
        if (++aPos == kTpDelay) aPos = 0;

        // Sliding minimum over kLook, release, boxcar over kLook; the audio is delayed by kLook - 1.
        rLine[(size_t) rPos] = r;
        float h = 1.0f;
        for (int k = 0; k < kLook; ++k) h = std::min (h, rLine[(size_t) k]);
        hRel = h < hRel ? h : hRel + (h - hRel) * relCoef;
        boxSum += (double) hRel - (double) boxLine[(size_t) rPos];
        boxLine[(size_t) rPos] = hRel;
        const float g = (float) (boxSum / (double) kLook);
        // Audio delayed by kLook - 1: the sample whose peak entered rLine kLook - 1 samples ago leaves now,
        // when the boxcar window covers exactly the kLook held-minimum values that start at it.
        for (int c = 0; c < nCh; ++c)
        {
            const float delayed = audDelay[(size_t) c][(size_t) audPos];
            audDelay[(size_t) c][(size_t) audPos] = a[c];
            s[c] = std::clamp (delayed * std::min (g, 1.0f), -ceil, ceil);
        }
        if (++audPos == kLook - 1) audPos = 0;
        if (++rPos == kLook) rPos = 0;
    }

    static constexpr float kDryScale = 0.001f;
    static constexpr float kCeilLin = 0.89125093813f * 0.994f;   // -1 dBTP minus 0.05 dB margin

    double fs = 48000.0;
    int maxBlk = 512, numCh = 2;
    std::array<Engine, kBands> eng;
    std::array<std::array<std::vector<float>, Engine::kMaxChannels>, kBands> band;
    std::array<std::vector<float>, Engine::kMaxChannels> dryBuf;
    Engine dryEng;
    std::array<Chan, Engine::kMaxChannels> ch {};
    float preTarget[kBands] { 1, 1, 1 }, pre[kBands] { 1, 1, 1 }, trimTarget[kBands] { 1, 1, 1 }, trim[kBands] { 1, 1, 1 }, glowGain[kBands] { 1, 1, 1 };
    float gms[kBands] { 0, 0, 0 };
    std::array<float, kBands> glow {};
    float glowCoef = 0.0f, glowRelCoef = 0.0f, relCoef = 0.0f, smoothCoef = 0.0f;
    float tMix = 1.0f, sMix = 1.0f;
    bool first = true;
    std::array<std::array<float, kTpTaps>, kTpPhases> tpFir {};
    std::array<std::array<float, Engine::kMaxChannels>, kTpTaps> hist {};
    int histPos = 0;
    std::array<std::array<float, kTpDelay>, Engine::kMaxChannels> aLine {};
    int aPos = 0;
    std::array<float, kLook> rLine {}, boxLine {};
    std::array<std::array<float, kLook - 1>, Engine::kMaxChannels> audDelay {};
    int rPos = 0, audPos = 0;
    double boxSum = kLook;
    float hRel = 1.0f;
};
} // namespace kor
