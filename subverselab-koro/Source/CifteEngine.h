#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

// KORO API: double + harmony together, continuous key detection, safe-interval rule, COLOR, fixed latency.
#define CIFTE_KORO_API 1

namespace cifte
{
constexpr float kPi = 3.14159265358979323846f;

enum class KeyMode { Auto = 0, Manual = 1 };

struct Rng
{
    uint32_t state;
    explicit Rng (uint32_t seed = 1970u) : state (seed) {}
    float next () { state = state * 1664525u + 1013904223u; return float (state >> 8) / 16777216.0f; }
    float bipolar () { return next () * 2.0f - 1.0f; }
};

struct OnePole
{
    float a = 0.0f, z = 0.0f;
    void lowpass (float hz, float sr) { a = std::exp (-2.0f * kPi * std::min (hz, 0.49f * sr) / sr); }
    float process (float x) { z = x + a * (z - x); return z; }
};

struct Biquad
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f, z1 = 0.0f, z2 = 0.0f;
    void highShelf (float hz, float db, float sr)
    {
        hz = std::min (hz, 0.46f * sr);
        const float A = std::pow (10.0f, db / 40.0f), w = 2.0f * kPi * hz / sr;
        const float c = std::cos (w), s = std::sin (w), alpha = s / (2.0f * 0.72f), beta = 2.0f * std::sqrt (A) * alpha;
        const float B0 = A * ((A + 1.0f) + (A - 1.0f) * c + beta);
        const float B1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * c);
        const float B2 = A * ((A + 1.0f) + (A - 1.0f) * c - beta);
        const float A0 = (A + 1.0f) - (A - 1.0f) * c + beta;
        const float A1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * c);
        const float A2 = (A + 1.0f) - (A - 1.0f) * c - beta;
        b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0;
    }
    float process (float x) { const float y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
};

struct Controls
{
    int voices = 2;          // 1..3
    float spread = 0.55f;    // 0..1, drift depth
    float tight = 0.35f;     // 0..1, inverse timing offset
    float width = 0.75f;     // 0..1
    float tone = 0.65f;      // 0..1, 3..16 kHz
    float mix = 0.48f;       // 0..1
    bool monoSafe = false;
    float harmony = 0.0f;     // 0..1, off by default. 0 = doubler only, 0.5 = (doubled) dry + harmony at full, 1 = harmony voices only
    KeyMode keyMode = KeyMode::Auto;   // Auto: the always-on detector picks root/scale; Manual: root/scale below
    bool keyLock = false;     // Auto only: freeze the detected key and its confidence
    float color = -1.0f;      // <0: per-voice degree[] (advanced); 0..1: 0 low 3rd, 1/3 high 3rd, 2/3 5th, 1 octave
    bool shareWidthTone = true;        // WIDTH scales the harmony pans, TONE high-cuts the harmony voices too
    bool shifterV2 = true;             // KORO shifter (refined marks, 2.8-period grains, smoothed target, jitter, 6 kHz shelf); false = approved legacy PSOLA
    int forcedKeys = 0;                // test/demo hook (AUTO): > 0 replaces the plausible key set by these keys
    std::array<int, 6> forcedRoot{{0, 0, 0, 0, 0, 0}}, forcedScale{{0, 0, 0, 0, 0, 0}};
    int root = 0;             // Manual key: 0=C .. 11=B
    int scale = 0;            // Manual key: 0 Major, 1 Minor, 2 Nihavent, 3 Hicaz, 4 Rast, 5 Chromatic (12 notes)
    float glide = 0.0f;       // 0..1, 0..120 ms portamento of the harmony interval
    float timing = 0.0f;      // 0..30 ms, per-voice late offset of the harmony voices
    int harmonyVoices = 1;    // 1..3
    std::array<int, 3> degree{{2, -2, 4}};             // scale steps: +2 third above, +4 fifth above, -2 third below, +-7 octave
    std::array<float, 3> voiceLevel{{1.0f, 0.82f, 0.72f}};
    std::array<float, 3> voicePan{{-0.65f, 0.65f, 0.0f}};
};

// ---------------------------------------------------------------------------------------------
// Scales (cents above the root). Chromatic has 12 steps, all others 7.
namespace scales
{
inline int count (int s) { return s == 5 ? 12 : 7; }
inline int cents (int s, int i)
{
    static const int major[7] = { 0, 200, 400, 500, 700, 900, 1100 };
    static const int minor[7] = { 0, 200, 300, 500, 700, 800, 1000 };
    static const int nihavent[7] = { 0, 150, 300, 500, 700, 850, 1000 };
    static const int hicaz[7] = { 0, 112, 384, 498, 702, 792, 1088 };
    static const int rast[7] = { 0, 204, 355, 498, 702, 906, 1053 };
    switch (s)
    {
        case 1: return minor[i];
        case 2: return nihavent[i];
        case 3: return hicaz[i];
        case 4: return rast[i];
        case 5: return 100 * i;
        default: return major[i];
    }
}
inline const char* name (int s)
{
    static const char* n[6] = { "Major", "Minor", "Nihavent", "Hicaz", "Rast", "Chromatic" };
    return n[std::max (0, std::min (5, s))];
}
inline int floorDiv (int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// Nearest scale note of a pitch given in MIDI cents (6900 = A4). Returns absolute octave o and index i.
// If 'held' is true the previously held note is kept unless another note is closer by more than 'hyst' cents.
inline void nearest (double midiCents, int root, int scale, bool held, int& idx, int& oct, double hyst)
{
    const int n = count (scale);
    const double rel = midiCents - 100.0 * root;
    const int o0 = int (std::floor (rel / 1200.0));
    double best = 1.0e9; int bi = 0, bo = o0;
    for (int o = o0 - 1; o <= o0 + 1; ++o)
        for (int i = 0; i < n; ++i)
        {
            const double d = std::fabs (rel - (1200.0 * o + cents (scale, i)));
            if (d < best) { best = d; bi = i; bo = o; }
        }
    if (held && idx >= 0 && idx < n)
    {
        const double dh = std::fabs (rel - (1200.0 * oct + cents (scale, idx)));
        if (dh <= best + hyst) return;
    }
    idx = bi; oct = bo;
}
// Interval in cents from scale note (idx, oct) to the note 'degree' scale steps away.
inline double intervalCents (int idx, int oct, int degree, int scale)
{
    const int n = count (scale);
    const int j = idx + degree;
    const int o2 = oct + floorDiv (j, n);
    const int jj = j - floorDiv (j, n) * n;
    return (1200.0 * o2 + cents (scale, jj)) - (1200.0 * oct + cents (scale, idx));
}
}

// ---------------------------------------------------------------------------------------------
// Streaming monophonic f0 tracker: YIN (symmetric-window difference, CMND, absolute threshold)
// on a low-passed, decimated copy (~11 kHz), refined on the full-rate signal with parabolic
// interpolation. 5 ms hop, voiced/unvoiced decision, 5-frame median smoothing (centred).
class PitchTracker
{
public:
    struct Frame { float logHz = 0.0f; bool voiced = false; float aperiodicity = 1.0f; };

    void prepare (double sampleRate, float minHz = 70.0f, float maxHz = 1000.0f)
    {
        sr = sampleRate; fMin = minHz; fMax = maxHz;
        D = std::max (1, int (sampleRate / 11000.0));
        const double fsd = sampleRate / D;
        hop = std::max (D, int (std::lround (0.005 * sampleRate / D)) * D);
        Wd = int (std::lround (0.020 * fsd));
        tauMinD = std::max (2, int (std::floor (fsd / maxHz)));
        tauMaxD = int (std::ceil (fsd / minHz)) + 1;
        spanD = Wd + tauMaxD + 4;
        spanFull = spanD * D;
        Wf = Wd * D;
        size_t fs = 1; while (fs < size_t (spanFull + 64)) fs <<= 1; full.assign (fs, 0.0f); fmask = int64_t (fs) - 1;
        size_t ds = 1; while (ds < size_t (spanD + 16)) ds <<= 1; dec.assign (ds, 0.0f); dmask = int64_t (ds) - 1;
        xs.assign (size_t (spanD), 0.0f); d.assign (size_t (tauMaxD + 2), 0.0f); cm.assign (size_t (tauMaxD + 2), 1.0f);
        dl.assign (size_t (4 * D + 8), 0.0f);
        // 2nd-order Butterworth low-pass at 1.5 kHz (anti-alias for the decimated YIN)
        const double w = 2.0 * 3.14159265358979323846 * std::min (1500.0, 0.2 * sampleRate) / sampleRate, c = std::cos (w), s = std::sin (w), al = s / (2.0 * 0.70710678);
        const double a0 = 1.0 + al; lb0 = (1.0 - c) / 2.0 / a0; lb1 = (1.0 - c) / a0; lb2 = lb0; la1 = -2.0 * c / a0; la2 = (1.0 - al) / a0;
        la = int (3 * hop + spanFull / 2 + 2);
        reset ();
    }
    void reset ()
    {
        std::fill (full.begin (), full.end (), 0.0f); std::fill (dec.begin (), dec.end (), 0.0f);
        z1 = z2 = 0.0; n = 0; m = 0; smCount = 0; raw.fill (Frame ()); sm.fill (Frame ());
    }
    void push (float x)
    {
        full[size_t (n & fmask)] = x;
        const double y = lb0 * x + z1; z1 = lb1 * x - la1 * y + z2; z2 = lb2 * x - la2 * y;
        ++n;
        if (n % D == 0) { dec[size_t (m & dmask)] = float (y); ++m; }
        if (n % hop == 0) analyse ();
    }
    // Pitch at absolute sample time t (valid for t <= samplesPushed - lookahead()).
    bool pitchAt (double t, float& hz) const
    {
        const double kf = (t - frameCenter (0)) / double (hop);
        if (kf < 0.0 || smCount <= 0) return false;
        int64_t k0 = int64_t (std::floor (kf));
        double fr = kf - double (k0);
        if (k0 >= smCount - 1) { if (k0 > smCount - 1) return false; fr = 0.0; }
        const Frame& a = sm[size_t (k0 & 255)];
        const Frame& b = sm[size_t ((k0 + 1 < smCount ? k0 + 1 : k0) & 255)];
        if (a.voiced && b.voiced) { hz = float (std::exp (a.logHz + (b.logHz - a.logHz) * fr)); return true; }
        const Frame& q = fr < 0.5 ? a : b;
        if (q.voiced) { hz = std::exp (q.logHz); return true; }
        return false;
    }
    int lookahead () const { return la; }
    void setOctaveCheck (bool on) { octaveCheck = on; }
    int hopSize () const { return hop; }
    int64_t smoothedFrames () const { return smCount; }
    double frameCenter (int64_t k) const { return double ((k + 1) * hop) - 0.5 * double (spanFull); }
    Frame smoothedFrame (int64_t k) const { return sm[size_t (k & 255)]; }
    Frame rawFrame (int64_t k) const { return raw[size_t (k & 255)]; }

private:
    void analyse ()
    {
        const int64_t k = n / hop - 1;
        Frame f;
        if (n >= spanFull && m >= spanD)
        {
            for (int j = 0; j < spanD; ++j) xs[size_t (j)] = dec[size_t ((m - spanD + j) & dmask)];
            const int cD = spanD / 2;
            double run = 0.0; cm[0] = 1.0f;
            for (int tau = 1; tau <= tauMaxD; ++tau)
            {
                const int a = cD - Wd / 2 - tau / 2;
                const float* p = xs.data () + a; const float* q = p + tau;
                float s = 0.0f; for (int j = 0; j < Wd; ++j) { const float e = p[j] - q[j]; s += e * e; }
                d[size_t (tau)] = s; run += s;
                cm[size_t (tau)] = run > 0.0 ? float (s * tau / run) : 1.0f;
            }
            int tb = -1;
            for (int tau = tauMinD; tau < tauMaxD; ++tau)
                if (cm[size_t (tau)] < 0.15f) { while (tau + 1 < tauMaxD && cm[size_t (tau + 1)] < cm[size_t (tau)]) ++tau; tb = tau; break; }
            if (tb < 0) { float best = 1.0e9f; for (int tau = tauMinD; tau < tauMaxD; ++tau) if (cm[size_t (tau)] < best) { best = cm[size_t (tau)]; tb = tau; } }
            if (octaveCheck && tb > 0 && 2 * tb + 2 < tauMaxD)
            {   // octave-error check (KORO only): if the double period matches clearly better, the first dip was an octave too high
                int t2 = 2 * tb; for (int t = 2 * tb - 2; t <= 2 * tb + 2; ++t) if (cm[size_t (t)] < cm[size_t (t2)]) t2 = t;
                if (cm[size_t (t2)] < cm[size_t (tb)] - 0.03f) tb = t2;
            }
            double tf = tb;
            if (tb > 1 && tb < tauMaxD)
            {
                const double y0 = cm[size_t (tb - 1)], y1 = cm[size_t (tb)], y2 = cm[size_t (tb + 1)], den = y0 - 2.0 * y1 + y2;
                if (std::fabs (den) > 1.0e-12) tf = tb + 0.5 * (y0 - y2) / den;
            }
            f.aperiodicity = cm[size_t (tb)];
            // full-rate refinement around D * tf
            const int64_t cF = n - spanFull / 2;
            const int G = int (std::lround (tf * D));
            const int lo = std::max (2, G - D - 1), hi = std::min (tauMaxD * D, G + D + 1);
            int nb = 0; double period = tf * D;
            if (hi - lo >= 2)
            {
                for (int L = lo; L <= hi; ++L)
                {
                    const int64_t a = cF - Wf / 2 - L / 2;
                    double s = 0.0;
                    for (int j = 0; j < Wf; ++j) { const float e = full[size_t ((a + j) & fmask)] - full[size_t ((a + j + L) & fmask)]; s += double (e) * e; }
                    dl[size_t (L - lo)] = float (s);
                }
                nb = hi - lo + 1; int bi = 0;
                for (int i = 1; i < nb; ++i) if (dl[size_t (i)] < dl[size_t (bi)]) bi = i;
                if (bi > 0 && bi < nb - 1)
                {
                    const double y0 = dl[size_t (bi - 1)], y1 = dl[size_t (bi)], y2 = dl[size_t (bi + 1)], den = y0 - 2.0 * y1 + y2;
                    period = lo + bi + (std::fabs (den) > 1.0e-20 ? 0.5 * (y0 - y2) / den : 0.0);
                }
            }
            double e = 0.0; for (int j = 0; j < Wf; ++j) { const float v = full[size_t ((cF - Wf / 2 + j) & fmask)]; e += double (v) * v; }
            const double rms = std::sqrt (e / Wf), hz = sr / std::max (1.0, period);
            f.voiced = f.aperiodicity < 0.25f && rms > 0.0025 && hz >= fMin && hz <= fMax;
            f.logHz = float (std::log (hz));
        }
        raw[size_t (k & 255)] = f;
        const int64_t kc = k - 2;
        if (kc < 0) return;
        float v[5]; int nv = 0;
        for (int64_t j = kc - 2; j <= kc + 2; ++j) if (j >= 0 && raw[size_t (j & 255)].voiced) v[nv++] = raw[size_t (j & 255)].logHz;
        Frame s; s.aperiodicity = raw[size_t (kc & 255)].aperiodicity;
        if (nv >= 3) { std::sort (v, v + nv); s.voiced = true; s.logHz = v[nv / 2]; }
        sm[size_t (kc & 255)] = s; smCount = kc + 1;
    }
    double sr = 44100.0; float fMin = 70.0f, fMax = 1000.0f;
    int D = 4, hop = 220, Wd = 220, Wf = 880, tauMinD = 11, tauMaxD = 160, spanD = 384, spanFull = 1536, la = 0;
    std::vector<float> full, dec, xs, d, cm, dl;
    int64_t fmask = 0, dmask = 0, n = 0, m = 0, smCount = 0;
    double lb0 = 1, lb1 = 0, lb2 = 0, la1 = 0, la2 = 0, z1 = 0, z2 = 0;
    bool octaveCheck = false;
    std::array<Frame, 256> raw{}, sm{};
};

// Offline helper (allocates; for tests and demo tools only).
struct TrackedFrame { double t; float hz; bool voiced; };
inline std::vector<TrackedFrame> trackOffline (const std::vector<float>& mono, double sampleRate, bool octaveCheck = false)
{
    PitchTracker tr; tr.prepare (sampleRate); tr.setOctaveCheck (octaveCheck); std::vector<TrackedFrame> out; int64_t got = 0;
    for (float x : mono)
    {
        tr.push (x);
        while (got < tr.smoothedFrames ()) { const auto f = tr.smoothedFrame (got); out.push_back ({ tr.frameCenter (got), f.voiced ? std::exp (f.logHz) : 0.0f, f.voiced }); ++got; }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Continuous key / scale detector (always on). Fed with the tracker's smoothed frames (5 ms hop).
// Only steady voiced frames count (voiced now and 10 ms ago, moved < 30 cents): glides and scoops
// between notes are not evidence. Each counts as a linear split into a 120-bin (10 cent) pitch-class
// histogram with exponential forgetting (time constant 8 s). The singer's reference tuning is estimated
// on the fly (circular mean of cents mod 100, same forgetting) and removed before binning.
// Major/Minor get a +0.01 tie-break (a near-tie between e.g. G Major and G Rast shows the diatonic key). Candidates: 12 roots x {Major, Minor,
// Hicaz, Rast} (Nihavent and Chromatic are never auto-selected).
// score = fit + 0.25 * tonic share; fit = mean Gaussian(25 cent) closeness to the nearest scale note,
// tonic share = fraction of the evidence within 50 cents of the root.
// Key choice: first key after >= 1 s of evidence when the leader has beaten every candidate with a
// different note set by 0.03 and its same-note-set twins by 0.02, for 0.3 s; later a challenger must beat the held key continuously for 1.5 s
// (hysteresis) by 0.02, or by 0.03 when it has the same note set (a tonic-only change, e.g. C Major -> A Minor).
// Confidence = clamp(margin / 0.10) * clamp(evidence / 2 s), margin = score(held key) - best score of a
// candidate with a DIFFERENT note set (C Major and A Minor give the same harmony notes, so they do
// not compete for confidence; they only decide the tonic that is shown).
class KeyDetector
{
public:
    static constexpr int kBins = 120, kCands = 48;
    struct Candidate { int root = 0, scale = 0; float score = 0.0f; };
    static int scaleOf (int c) { static const int ids[4] = { 0, 1, 3, 4 }; return ids[c / 12]; }
    static int rootOf (int c) { return c % 12; }

    void prepare (double hopSeconds)
    {
        hopS = float (hopSeconds);
        decay = float (std::exp (-hopSeconds / 8.0));
        switchFrames = int (std::ceil (1.5 / hopSeconds));
        acquireFrames = int (std::ceil (0.3 / hopSeconds));
        double notes[kCands][7];
        for (int c = 0; c < kCands; ++c)
            for (int i = 0; i < 7; ++i) notes[c][i] = std::fmod (100.0 * rootOf (c) + scales::cents (scaleOf (c), i), 1200.0);
        for (int c = 0; c < kCands; ++c)
            for (int b = 0; b < kBins; ++b)
            {
                const double pc = 10.0 * b; double dmin = 1.0e9;
                for (double nt : notes[c]) { double d = std::fabs (pc - nt); d = std::min (d, 1200.0 - d); dmin = std::min (dmin, d); }
                W[size_t (c)][size_t (b)] = float (std::exp (-dmin * dmin / (2.0 * 25.0 * 25.0)));
                double dr = std::fabs (pc - 100.0 * rootOf (c)); dr = std::min (dr, 1200.0 - dr);
                T[size_t (c)][size_t (b)] = dr < 50.0 ? 1.0f : 0.0f;
            }
        for (int a = 0; a < kCands; ++a)
            for (int b = 0; b < kCands; ++b)
            {
                bool same = true;
                for (int i = 0; i < 7 && same; ++i)
                {
                    double dmin = 1.0e9;
                    for (int j = 0; j < 7; ++j) { double d = std::fabs (notes[a][i] - notes[b][j]); d = std::min (d, 1200.0 - d); dmin = std::min (dmin, d); }
                    same = dmin < 30.0;
                }
                equiv[size_t (a)][size_t (b)] = same;
            }
        reset ();
    }
    void reset ()
    {
        hist.fill (0.0f); score.fill (0.0f); evidence = 0.0f; tuneRe = tuneIm = 0.0; tuneCents = 0.0f; frames = 0; pv1 = pv2 = false; pc1 = pc2 = 0.0;
        has = false; cur = 0; acqCand = -1; acqCount = 0; chal = -1; chalCount = 0; conf = 0.0f; scored = false;
        lockFrame = -1; switches = 0;
    }
    void push (bool voiced, float logHz)
    {
        ++frames;
        const double cents = voiced ? 6900.0 + 1200.0 * (double (logHz) - std::log (440.0)) / std::log (2.0) : 0.0;
        const bool steady = voiced && pv1 && pv2 && std::fabs (cents - pc2) < 30.0;
        pv2 = pv1; pc2 = pc1; pv1 = voiced; pc1 = cents;
        evidence *= decay; tuneRe *= decay; tuneIm *= decay;
        for (float& h : hist) { h *= decay; if (h < 1.0e-20f) h = 0.0f; }
        if (evidence < 1.0e-20f) evidence = 0.0f;
        if (steady)
        {
            // reference-tuning estimate: circular mean of (cents mod 100), same forgetting as the histogram
            const double ph = 2.0 * 3.14159265358979323846 * std::fmod (cents, 100.0) / 100.0;
            tuneRe += hopS * std::cos (ph); tuneIm += hopS * std::sin (ph);
            tuneCents = float (std::atan2 (tuneIm, tuneRe) * 100.0 / (2.0 * 3.14159265358979323846));
            double pos = std::fmod (cents - double (tuneCents), 1200.0) / 10.0; if (pos < 0.0) pos += kBins;
            const int b0 = int (pos) % kBins, b1 = (b0 + 1) % kBins; const float f = float (pos - std::floor (pos));
            hist[size_t (b0)] += hopS * (1.0f - f); hist[size_t (b1)] += hopS * f; evidence += hopS;
            rescore ();
        }
        updateKey ();
    }
    bool hasKey () const { return has; }
    int root () const { return rootOf (cur); }
    int scale () const { return scaleOf (cur); }
    float confidence () const { return has ? conf : 0.0f; }
    float evidenceSeconds () const { return evidence; }
    float tuningCents () const { return tuneCents; }   // estimated reference offset (cents vs A440 12-TET)
    int64_t framesSeen () const { return frames; }
    int64_t firstKeyFrame () const { return lockFrame; }
    int keySwitches () const { return switches; }
    // Plausible key set: every candidate whose score is within 0.08 of the best (min 1, max 6), best first.
    int plausible (int* roots, int* scalesOut, int nmax = 6) const
    {
        if (!scored) return 0;
        std::array<int, kCands> idx; for (int c = 0; c < kCands; ++c) idx[size_t (c)] = c;
        std::sort (idx.begin (), idx.end (), [&] (int a, int b) { return score[size_t (a)] > score[size_t (b)]; });
        const float best = score[size_t (idx[0])]; int n = 0;
        for (int i = 0; i < kCands && n < nmax; ++i) { if (i > 0 && score[size_t (idx[size_t (i)])] < best - 0.08f) break; roots[n] = rootOf (idx[size_t (i)]); scalesOut[n] = scaleOf (idx[size_t (i)]); ++n; }
        return n;
    }
    // Top-n candidates by score, best first (n <= 8).
    int top (Candidate* out, int nmax) const
    {
        if (!scored) return 0;
        std::array<int, kCands> idx; for (int c = 0; c < kCands; ++c) idx[size_t (c)] = c;
        std::partial_sort (idx.begin (), idx.begin () + nmax, idx.end (), [&] (int a, int b) { return score[size_t (a)] > score[size_t (b)]; });
        for (int i = 0; i < nmax; ++i) out[i] = { rootOf (idx[size_t (i)]), scaleOf (idx[size_t (i)]), score[size_t (idx[size_t (i)])] };
        return nmax;
    }

private:
    void rescore ()
    {
        if (evidence <= 0.0f) return;
        const float inv = 1.0f / std::max (1.0e-9f, std::accumulate (hist.begin (), hist.end (), 0.0f));
        for (int c = 0; c < kCands; ++c)
        {
            float fit = 0.0f, ton = 0.0f;
            for (int b = 0; b < kBins; ++b) { fit += hist[size_t (b)] * W[size_t (c)][size_t (b)]; ton += hist[size_t (b)] * T[size_t (c)][size_t (b)]; }
            score[size_t (c)] = (fit + 0.25f * ton) * inv + (scaleOf (c) <= 1 ? kDiatonicPrior : 0.0f);
        }
        scored = true;
    }
    float twinOf (int k) const   // best other candidate with the SAME note set (relative major/minor etc.)
    {
        float twin = -1.0f; for (int c = 0; c < kCands; ++c) if (c != k && equiv[size_t (k)][size_t (c)]) twin = std::max (twin, score[size_t (c)]);
        return twin;
    }
    float rivalOf (int k) const  // best candidate with a DIFFERENT note set
    {
        float rival = -1.0f; for (int c = 0; c < kCands; ++c) if (!equiv[size_t (k)][size_t (c)]) rival = std::max (rival, score[size_t (c)]);
        return rival;
    }
    void updateKey ()
    {
        if (!scored) { conf = 0.0f; return; }
        int best = 0; for (int c = 1; c < kCands; ++c) if (score[size_t (c)] > score[size_t (best)]) best = c;
        if (!has)
        {
            // first key: >= 1 s of evidence and the leader beats every candidate with a different note set by 0.03, for 0.3 s
            if (evidence >= 1.0f && score[size_t (best)] - rivalOf (best) > 0.03f && score[size_t (best)] - twinOf (best) > 0.02f) { if (best == acqCand) ++acqCount; else { acqCand = best; acqCount = 1; } }
            else acqCount = 0;
            if (acqCount >= acquireFrames) { has = true; cur = best; lockFrame = frames; }
        }
        else if (best != cur && score[size_t (best)] - score[size_t (cur)] > (equiv[size_t (cur)][size_t (best)] ? 0.03f : 0.02f))
        {
            if (best == chal) ++chalCount; else { chal = best; chalCount = 1; }
            if (chalCount >= switchFrames) { cur = best; chal = -1; chalCount = 0; ++switches; }
        }
        else { chal = -1; chalCount = 0; }
        if (!has) { conf = 0.0f; return; }
        const float mf = std::clamp ((score[size_t (cur)] - rivalOf (cur)) / 0.10f, 0.0f, 1.0f), ef = std::clamp (evidence / 2.0f, 0.0f, 1.0f);
        conf = mf * ef;
    }
    std::array<std::array<float, kBins>, kCands> W{}, T{};
    std::array<std::array<bool, kCands>, kCands> equiv{};
    std::array<float, kBins> hist{};
    std::array<float, kCands> score{};
    static constexpr float kDiatonicPrior = 0.01f;
    float hopS = 0.005f, decay = 0.999f, evidence = 0.0f, conf = 0.0f, tuneCents = 0.0f;
    double tuneRe = 0.0, tuneIm = 0.0;
    int switchFrames = 300, acquireFrames = 100, cur = 0, acqCand = -1, acqCount = 0, chal = -1, chalCount = 0, switches = 0;
    int64_t frames = 0, lockFrame = -1;
    bool pv1 = false, pv2 = false, has = false, scored = false;
    double pc1 = 0.0, pc2 = 0.0;
};

// ---------------------------------------------------------------------------------------------
// Pitch-shifting harmony: TD-PSOLA, formant-keeping. Unvoiced frames pass through unshifted (8 ms
// crossfade). Fixed latency = latency(), always (harmony on or off).
//  - legacy shifter (Controls::shifterV2 = false, the approved 2026-09-30 sound): analysis marks one
//    tracked period apart, Hann grains of 2 source periods, synthesis spacing = source period / ratio.
//  - KORO shifter (shifterV2 = true): analysis marks re-aligned per period by normalised cross-correlation
//    with the previous mark (sub-sample, parabolic), Hann grains of 2.8 periods, synthesis spacing from the
//    SMOOTHED target pitch (one-pole 25 ms on the tracked pitch, slew-limited to 15 c/ms, jumps > 600 c held off
//    for up to 30 ms as tracker octave errors), interpolated grains (blend of the two source periods around the
//    synthesis time) and a -3 dB high shelf at 6 kHz on each shifted voice. The requested +-3 % placement jitter
//    is implemented (part bit 8) but OFF: it measurably increased roughness (see STATUS.md).
//
// Each harmony voice owns a pool of shifter SLOTS; a slot renders one interval SPEC (a scale degree in a key,
// or a fixed interval in cents). Slots fade in/out over 250 ms at constant power.
// AUTO interval rule, decided ONCE PER NOTE:
//  - note onset = voiced after >= 60 ms unvoiced, or the pitch leaves the note's anchor by > 80 cents for
//    10 ms. The interval class is decided 70 ms after the onset (agreement over frames 30-70 ms) and HELD
//    until the next onset: no interval switching inside a sustained note.
//  - (A) the fixed perfect fifth (+-702 c) and octave (+-1200 c) are always allowed on a decided note.
//  - fallback for an unsafe third: UNISON (plain double, -3 dB) for COLOR <= 0.5 and degree mode, the fifth for
//    COLOR 0.5..0.83; never an octave. The octave only sounds where COLOR chooses it (>= 0.83). Early upgrade
//    window 600 ms.
//  - (B) a third is played only if ALL plausible keys (score within 0.08 of the best, 1..6 keys, >= 1 s
//    evidence) give the same third pitch (30 c) on every checked frame; otherwise fifth (COLOR >= 0.5) or
//    octave in the third's direction (COLOR < 0.5); SAFE lamp = "third suppressed on this note".
//  - a voice changes its interval set at most once per 300 ms, and only at a note decision (<= 100 ms after
//    the onset); a change that would come too soon is skipped for that note, except a third that must be
//    withdrawn (safety), which is applied at the decision anyway and counted as an override.
//  - no third before the first decided note; the first state is unison.
//  MANUAL key: the user's key is trusted, every degree is a scale degree, per sample, as before.
class Harmonizer
{
public:
    struct Spec
    {
        bool fixed = true, tuned = false; int cents = 0, degree = 0, root = 0, scale = 0;
        bool operator== (const Spec& o) const { return fixed == o.fixed && (fixed ? cents == o.cents : (degree == o.degree && root == o.root && scale == o.scale && tuned == o.tuned)); }
    };
    enum Mode { kUnison = 0, kFifth = 1, kOctave = 2, kThird = 3, kOther = 4 };
    struct Status
    {
        bool hasKey = false; int root = 0, scale = 0; float confidence = 0.0f, evidenceSeconds = 0.0f, tuningCents = 0.0f;
        bool safe = false, locked = false, manual = false, voicedNow = false, gate = false; int plausibleCount = 0;
        std::array<int, 3> mode{{ 0, 0, 0 }};                 // applied interval class per voice (dominant COLOR weight)
        double noteCents = 0.0;                               // sung note (MIDI cents, tuning removed) at the output time
        std::array<double, 3> thirdCents{{ 0.0, 0.0, 0.0 }};  // absolute third pitch when mode == kThird (tuned space)
        int64_t noteId = 0; double noteOnset = -1.0;          // current note and its onset (input samples)
        bool thirdDisagreesNow = false;                       // a playing third that the current plausible set does not agree on (info)
    };
    struct Stats
    {
        double maxJumpSustained = 0.0, maxJumpNote = 0.0;     // target-pitch jump between consecutive grains of a slot (cents)
        int64_t grainPairsSustained = 0; int changes = 0, overrides = 0, lateChanges = 0, skipped = 0, withdrawals = 0, upgrades = 0, thirdsApplied = 0, thirdsWithoutAgreement = 0;
        double minGapNonSafety = 1.0e9;                        // seconds between non-withdrawal changes of one voice
    };

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        tracker.prepare (sampleRate, kMinHz, 1000.0f);
        detector.prepare (double (tracker.hopSize ()) / sampleRate);
        pmax = int (std::ceil (sampleRate / kMinHz)) + 2;
        lat = std::max (int (std::ceil (2.5 * pmax)), int (std::ceil (1.5 * pmax)) + tracker.lookahead ()) + 8;
        size_t rs = 1; while (rs < size_t (lat + 4 * pmax + 256)) rs <<= 1; mask = int64_t (rs) - 1;
        mono.assign (rs, 0.0f); inL.assign (rs, 0.0f); inR.assign (rs, 0.0f); smLog.assign (rs, 0.0f); smOk.assign (rs, 0);
        refBuf.assign (size_t (2 * int (0.12 * pmax) + 16), 0.0);
        const size_t td = size_t (std::ceil (0.031 * sampleRate)) + 4;
        for (int v = 0; v < 3; ++v)
        {
            voice[size_t (v)].late.assign (td, 0.0f); voice[size_t (v)].shelf.highShelf (6000.0f, -3.0f, float (sampleRate));
            for (auto& s : voice[size_t (v)].slot) s.ola.assign (rs, 0.0f);
        }
        xfadeStep = float (1.0 / (0.008 * sampleRate));
        envA = float (std::exp (-1.0 / (0.040 * sampleRate)));
        rampStep = float (1.0 / (0.250 * sampleRate));
        smA = kSmoothMs > 0.0 ? 1.0 - std::exp (-1.0 / (kSmoothMs * 0.001 * sampleRate)) : 1.0; slewMax = 15.0 * 1000.0 / sampleRate; outHold = int (0.030 * sampleRate);
        minGap = 0.300 * sampleRate; window = 0.100 * sampleRate; upgradeWindow = 0.600 * sampleRate; duckStep = float (1.0 / (0.250 * sampleRate));
        const double hopS = double (tracker.hopSize ()) / sampleRate;
        gateFrames = int (std::ceil (0.060 / hopS)); decideFrames = int (std::ceil (0.070 / hopS)); winFrom = int (std::ceil (0.045 / hopS));
        reset ();
    }
    void reset ()
    {
        tracker.reset (); detector.reset (); detFrames = 0; n = 0; nextMark = 0.0; markCount = 0; vgain = 0.0f;
        started = false; lockPrev = false; st = Status (); stats = Stats (); toneL = toneR = 0.0f; gInit = false;
        finfo.fill (FrameInfo ()); unvoicedRun = 0; lockN = 0; lockHas = false;
        sv1 = sv2 = false; sc1 = sc2 = 0.0; inNote = false; decided = false; noteId = 0; onsetFrame = 0; anchorC = 0.0; anchorN = 0; jumpRun = 0; lastGood = 0.0; lastGoodOk = false; outlierPrev = false;
        winUp = winDown = winHas = true; heldUp = heldDown = heldHas = false; heldR = heldS = 0;
        smCur = 0.0; smValid = false; outRun = 0; smGap = 0; octHolding = false; octStart = -1.0e18;
        std::fill (mono.begin (), mono.end (), 0.0f); std::fill (inL.begin (), inL.end (), 0.0f); std::fill (inR.begin (), inR.end (), 0.0f);
        std::fill (smLog.begin (), smLog.end (), 0.0f); std::fill (smOk.begin (), smOk.end (), uint8_t (0));
        for (int v = 0; v < 3; ++v)
        {
            Voice& V = voice[size_t (v)];
            std::fill (V.late.begin (), V.late.end (), 0.0f); V.latePos = 0; V.envIn = V.envOut = 0.0f; V.duck = 1.0f; V.shelf.z1 = V.shelf.z2 = 0.0f;
            V.lateD = 0.0f; V.lateInit = false; V.hasApplied = false; V.appN = 0; V.lastChange = -1.0e18; V.lastNonSafety = -1.0e18; V.skipNote = V.decNote = V.upNote = -1; V.noteSafe = false;
            for (int q = 0; q < kSlots; ++q)
            {
                Slot& s = V.slot[size_t (q)];
                std::fill (s.ola.begin (), s.ola.end (), 0.0f); s.live = false; s.p = 0.0f; s.target = 0.0f; s.nextSynth = 0.0; s.active = false; s.held = false; s.idx = -1; s.oct = 0; s.ratioCents = 0.0; s.lastGrain = 0.0;
                s.jit = Rng (0x51A7u + uint32_t (v * 97 + q * 13)); s.lastValid = false;
            }
        }
    }
    int latency () const { return lat; }
    const PitchTracker& pitchTracker () const { return tracker; }
    const KeyDetector& keyDetector () const { return detector; }
    const Status& status () const { return st; }
    const Stats& shifterStats () const { return stats; }
    double voiceIntervalCents (int v) const
    {
        const Voice& V = voice[size_t (std::clamp (v, 0, 2))]; const Slot* b = nullptr;
        for (const auto& s : V.slot) if (s.live && (!b || s.p > b->p)) b = &s;
        return b ? b->ratioCents : 0.0;
    }

    // dryL/dryR: the dry input (analysis + harmony source). left/right: on entry the doubled signal
    // (the doubler's output, zero latency), on exit the engine output (latency() samples late).
    void process (const float* dryL, const float* dryR, float* left, float* right, int count, const Controls& c)
    {
        const float h = std::clamp (c.harmony, 0.0f, 1.0f);
        const float dryGain = std::min (1.0f, 2.0f * (1.0f - h)), wetGain = std::min (1.0f, 2.0f * h);
        const int vc = std::max (1, std::min (3, c.harmonyVoices));
        const double tauGlide = std::clamp (c.glide, 0.0f, 1.0f) * 0.120 * sr / 3.0;
        const float timingMs = std::clamp (c.timing, 0.0f, 30.0f);
        const float width = std::clamp (c.width, 0.0f, 1.0f);
        const float toneHz = 3000.0f * std::pow (16.0f / 3.0f, std::clamp (c.tone, 0.0f, 1.0f));
        const float toneA = std::exp (-2.0f * kPi * std::min (0.45f * float (sr), toneHz) / float (sr));
        const bool manual = c.keyMode == KeyMode::Manual, v2 = c.shifterV2;
        tracker.setOctaveCheck (v2);
        static const float lateShape[3] = { 1.0f, 0.55f, 0.8f };
        // KORO plug-in change (click fix): the controls arrive once per call (32-sample sub-blocks in the plug-in), so a gain or a
        // delay that moves with automation would be a staircase. The HARMONY dry / wet gains and the TIMING offsets are interpolated
        // linearly per sample from the previous call's value to this call's value. A constant setting has step 0, so the arithmetic
        // is the original one and the output is bit-identical.
        const float invCount = 1.0f / float (std::max (1, count));
        if (!gInit) { gDry = dryGain; gWet = wetGain; gInit = true; }
        const float dryStep = (dryGain - gDry) * invCount, wetStep = (wetGain - gWet) * invCount;
        float lateTgt[3] = { 0.0f, 0.0f, 0.0f }, lateStep[3] = { 0.0f, 0.0f, 0.0f };
        for (int v = 0; v < 3; ++v)
        {
            Voice& V = voice[size_t (v)];
            if (v >= vc) { V.lateInit = false; continue; }
            // While TIMING moves (the exact target changed since the last call) the offset follows the exact, fractional target.
            // Once it holds still it settles, at most 0.02 sample per sample, onto the whole-sample offset the engine always used,
            // so a constant TIMING is read exactly as before (bit-identical).
            const float tf = timingMs * 0.001f * float (sr) * lateShape[v];
            if (!V.lateInit) { V.lateD = float (int (tf)); V.lateTf = tf; V.lateInit = true; }
            const bool moving = tf != V.lateTf; V.lateTf = tf;
            lateTgt[v] = moving ? tf : float (int (tf));
            float step = (lateTgt[v] - V.lateD) * invCount;
            if (!moving) { const float cap = 0.02f; if (step > cap) step = cap; else if (step < -cap) step = -cap; }
            lateStep[v] = step;
        }
        for (int i = 0; i < count; ++i)
        {
            if (dryStep != 0.0f) gDry += dryStep;
            if (wetStep != 0.0f) gWet += wetStep;
            const float x = 0.5f * (dryL[i] + dryR[i]);
            mono[size_t (n & mask)] = x; inL[size_t (n & mask)] = left[i]; inR[size_t (n & mask)] = right[i];
            tracker.push (x);
            while (detFrames < tracker.smoothedFrames ()) { const auto f = tracker.smoothedFrame (detFrames); detector.push (f.voiced, f.logHz); frameInfo (detFrames, f, c); ++detFrames; }
            ++n;
            // analysis marks, one tracked period apart, as far as the pitch track is final
            const double known = double (n - tracker.lookahead ());
            while (nextMark <= known)
            {
                float hz = 0.0f;
                if (tracker.pitchAt (nextMark, hz))
                {
                    double per = sr / hz; double pos = nextMark;
                    if (v2 && (kV2Parts & 4) && markCount > 0)
                    {   // octave-error hold at mark level: a period jump > 600 c from the previous mark keeps the previous period for up to 30 ms
                        const Mark& pm = marks[size_t ((markCount - 1) & kMarkMask)];
                        if (pos - pm.pos < 1.5 * pm.per && std::fabs (1200.0 * std::log2 (pm.per / per)) > 600.0 && pos - octStart < 0.030 * sr) { if (!octHolding) { octHolding = true; octStart = pos; } per = pm.per; }
                        else octHolding = false;
                    }
                    if (v2 && (kV2Parts & 1) && markCount > 0)
                    {
                        const Mark& pm = marks[size_t ((markCount - 1) & kMarkMask)]; const double gap = pos - pm.pos;
                        if (gap > 0.5 * per && gap < 1.5 * per) pos = refineMark (pm.pos, pos, per);
                    }
                    marks[size_t (markCount & kMarkMask)] = { pos, per }; ++markCount; nextMark = pos + per;
                }
                else nextMark += 0.25 * tracker.hopSize ();
            }
            // smoothed target pitch at the newest known time (one-pole 25 ms, single-frame jumps > 600 c ignored)
            {
                const int64_t th = n - 1 - tracker.lookahead ();
                if (th >= 0)
                {
                    float hz = 0.0f;
                    if (tracker.pitchAt (double (th), hz))
                    {
                        const double lc = 6900.0 + 1200.0 * std::log2 (double (hz) / 440.0);
                        if (!smValid || smGap >= gateFrames * tracker.hopSize ()) { smCur = lc; outRun = 0; }
                        smValid = true; smGap = 0;
                        if (false) {}
                        else if (std::fabs (lc - smCur) > 600.0 && outRun < outHold) ++outRun;   // octave error / outlier: hold up to 30 ms
                        else
                        {   // one-pole 25 ms, slew-limited to 15 cents per ms
                            const double d = std::clamp (smA * (lc - smCur), -slewMax, slewMax); smCur += d;
                            if (std::fabs (lc - smCur) <= 600.0) outRun = 0;
                        }
                        smLog[size_t (th & mask)] = float (smCur); smOk[size_t (th & mask)] = 1;
                    }
                    else { ++smGap; smOk[size_t (th & mask)] = 0; }
                }
            }
            const int64_t td = n - 1 - lat;   // the input time whose output is emitted now
            float wet[2] = { 0.0f, 0.0f };
            float hzNow = 0.0f; const bool voicedNow = td >= 0 && tracker.pitchAt (double (td), hzNow);
            const FrameInfo fi = frameAt (td);
            beginStatus (c, voicedNow, fi);
            vgain = voicedNow ? std::min (1.0f, vgain + xfadeStep) : std::max (0.0f, vgain - xfadeStep);
            const float xd = td >= 0 ? mono[size_t (td & mask)] : 0.0f;
            const size_t k = size_t ((td + (int64_t (mask) + 1)) & mask);
            const double horizon = v2 ? double (td) + 1.4 * pmax : double (td + pmax);
            for (int v = 0; v < vc; ++v)
            {
                Voice& V = voice[size_t (v)];
                if (manual) assignManual (V, c, v, vc, td); else assignAuto (V, c, v, vc, td, fi);
                float shifted = 0.0f;
                for (auto& s : V.slot)
                {
                    if (!s.live) { s.ola[k] = 0.0f; continue; }
                    const float pt = s.target * s.target;
                    if (!started) s.p = pt;
                    else if (s.p < pt) s.p = std::min (pt, s.p + rampStep);
                    else if (s.p > pt) s.p = std::max (pt, s.p - rampStep);
                    while (s.nextSynth <= horizon) scheduleGrain (s, td, tauGlide, v2);
                    const float val = s.ola[k]; s.ola[k] = 0.0f;
                    if (s.p > 0.0f) shifted += std::sqrt (s.p) * val;
                    else if (pt <= 0.0f) s.live = false;
                }
                if (v2 && (kV2Parts & 16)) shifted = V.shelf.process (shifted);
                // envelope match (40 ms, voiced only): LEVEL is relative to the dry voice
                if (vgain > 0.0f) { V.envOut = envA * V.envOut + (1.0f - envA) * shifted * shifted; V.envIn = envA * V.envIn + (1.0f - envA) * xd * xd; }
                const float match = std::clamp (std::sqrt ((V.envIn + 1.0e-9f) / (V.envOut + 1.0e-9f)), 0.5f, v2 ? kMatchMax : 2.0f);
                float y = vgain * shifted * match + (1.0f - vgain) * xd;
                if (!manual)
                {   // an AUTO voice sitting on unison is a plain double: duck it 3 dB so the lead does not jump in loudness
                    const float dt = st.mode[size_t (v)] == kUnison ? 0.70795f : 1.0f;
                    V.duck = started ? (V.duck < dt ? std::min (dt, V.duck + duckStep) : std::max (dt, V.duck - duckStep)) : dt;
                    y *= V.duck;
                }
                // humanised late entry
                V.late[size_t (V.latePos)] = y;
                if (lateStep[v] != 0.0f) V.lateD += lateStep[v];
                if (V.lateD > 0.0f)
                {
                    const int lateN = int (V.lateD); const float lateFrac = V.lateD - float (lateN);
                    int rp = V.latePos - lateN; if (rp < 0) rp += int (V.late.size ());
                    const float a = V.late[size_t (rp)];
                    if (lateFrac > 0.0f)
                    {
                        int rp1 = rp - 1; if (rp1 < 0) rp1 += int (V.late.size ());
                        y = a + (V.late[size_t (rp1)] - a) * lateFrac;
                    }
                    else y = a;
                }
                if (++V.latePos >= int (V.late.size ())) V.latePos = 0;
                float pan = std::clamp (c.voicePan[size_t (v)], -1.0f, 1.0f); const float lev = c.voiceLevel[size_t (v)];
                if (c.shareWidthTone) pan *= width;
                wet[0] += y * lev * std::min (1.0f, 1.0f - pan);
                wet[1] += y * lev * std::min (1.0f, 1.0f + pan);
            }
            if (c.shareWidthTone)
            {
                toneL = wet[0] + toneA * (toneL - wet[0]); toneR = wet[1] + toneA * (toneR - wet[1]);
                wet[0] = toneL; wet[1] = toneR;
            }
            started = true;
            const float dl = td >= 0 ? inL[size_t (td & mask)] : 0.0f, dr = td >= 0 ? inR[size_t (td & mask)] : 0.0f;
            left[i] = gDry * dl + gWet * wet[0];
            right[i] = gDry * dr + gWet * wet[1];
        }
        gDry = dryGain; gWet = wetGain;
        for (int v = 0; v < vc; ++v)
        {
            Voice& V = voice[size_t (v)];
            if (lateStep[v] == 0.0f || std::fabs (lateTgt[v] - V.lateD) <= 1.0e-3f) V.lateD = lateTgt[v];   // reached (float accumulation error only): land exactly
        }
    }

private:
    static constexpr float kMinHz = 70.0f;
    static constexpr int64_t kMarkMask = 1023;
    static constexpr int kSlots = 6;
    // KORO shifter parts (bit mask): 1 mark refinement, 2 long grains, 4 smoothed target, 8 placement jitter, 16 6 kHz shelf,
    // 32 interpolated grains (blend of the two source periods around the synthesis time)
    static constexpr int kV2Parts = 4 | 16 | 32;   // measured per part (STATUS.md): conditioned target (4), 6 kHz shelf (16), interpolated grains (32)
    static constexpr float kMinEvidence = 1.0f;    // seconds of steady-voiced evidence before any third (the agreement rule does the rest)
    static constexpr bool kGlideSwap = true;       // AUTO unison <-> third: glide the same voice (70 ms time constant) instead of a two-pitch crossfade
    static constexpr float kMatchMax = 4.0f;       // KORO shifter: level match may lift a shifted voice up to +12 dB (octave-up keeps only the even harmonics)
    static constexpr double kBlendAlpha = -1.0;   // < 0: blend weight = fractional position; >= 0: fixed weight
    static constexpr double kSmoothMs = 0.0;    // target pitch: slew limit 15 c/ms + 30 ms octave-error hold; the requested 25 ms one-pole is off (its vibrato lag added roughness, STATUS.md)
    struct Mark { double pos = 0.0, per = 0.0; };
    struct Slot
    {
        std::vector<float> ola; Spec spec; bool live = false; float p = 0.0f, target = 0.0f;
        double nextSynth = 0.0, ratioCents = 0.0, lastGrain = 0.0; bool active = false, held = false; int idx = -1, oct = 0;
        Rng jit; bool lastValid = false, lastSus = false; int64_t lastNote = 0; double lastTarget = 0.0, glideUntil = -1.0;
    };
    struct Voice
    {
        std::array<Slot, kSlots> slot; std::vector<float> late; float envIn = 0.0f, envOut = 0.0f, duck = 1.0f; int latePos = 0; Biquad shelf;
        float lateD = 0.0f, lateTf = 0.0f; bool lateInit = false;   // KORO plug-in change: current late offset in samples (interpolated while TIMING moves) and the last exact target
        bool hasApplied = false; int appN = 0; std::array<Spec, 2> app{}; std::array<float, 2> appW{{ 0.0f, 0.0f }}; std::array<int, 2> appMode{{ 0, 0 }}, appDeg{{ 0, 0 }};
        double lastChange = -1.0e18, lastNonSafety = -1.0e18; int64_t skipNote = -1, decNote = -1, upNote = -1; bool noteSafe = false, swapPending = false; Spec swapFrom;
    };
    struct FrameInfo
    {
        bool valid = false, voiced = false, steady = false, inNote = false, decided = false, hasKey = false, agreeUp = false, agreeDown = false, suppressed = false, instUp = false, instDown = false;
        int instR = 0, instS = 0, refRoot = 0, refScale = 0, plausible = 0; int64_t noteId = 0; double onsetT = 0.0, noteCents = 0.0, upAbs = 0.0, downAbs = 0.0;
    };
    static int anchor (int i, int scale)
    {
        static const int deg[4] = { -2, 2, 4, 7 }, chrom[4] = { -4, 4, 7, 12 };
        return scale == 5 ? chrom[i] : deg[i];
    }
    static double thirdAbs (double x, int r, int s, int dir)
    {
        int idx = -1, oct = 0; scales::nearest (x, r, s, false, idx, oct, 0.0);
        return 100.0 * r + 1200.0 * oct + scales::cents (s, idx) + scales::intervalCents (idx, oct, dir, s);
    }
    // re-align a proposed analysis mark to the previous one (normalised cross-correlation, sub-sample)
    double refineMark (double prev, double p, double per)
    {
        const int D = std::max (2, int (0.12 * per)), W = std::max (8, int (0.5 * per)), stp = W > 64 ? 2 : 1;
        const int64_t ip = int64_t (std::lround (prev)), ic = int64_t (std::lround (p));
        int bd = 0; double bc = -1.0e30;
        for (int d = -D; d <= D; ++d)
        {
            double s = 0.0, e = 0.0;
            for (int u = -W; u <= W; u += stp) { const double a = mono[size_t ((ip + u) & mask)], b = mono[size_t ((ic + d + u) & mask)]; s += a * b; e += b * b; }
            const double cc = s / std::sqrt (e + 1.0e-12); refBuf[size_t (d + D)] = cc;
            if (cc > bc) { bc = cc; bd = d; }
        }
        double frac = 0.0;
        if (bd > -D && bd < D)
        {
            const double y0 = refBuf[size_t (bd - 1 + D)], y1 = refBuf[size_t (bd + D)], y2 = refBuf[size_t (bd + 1 + D)], den = y0 - 2.0 * y1 + y2;
            if (std::fabs (den) > 1.0e-18) frac = std::clamp (0.5 * (y0 - y2) / den, -0.5, 0.5);
        }
        return double (ic + bd) + frac + (prev - double (ip));
    }
    // Per tracker frame: note onsets, plausible key set, per-note third agreement, held decision.
    void frameInfo (int64_t k, const PitchTracker::Frame& f, const Controls& c)
    {
        FrameInfo fi; fi.valid = true; fi.voiced = f.voiced;
        double cents = f.voiced ? 6900.0 + 1200.0 * (double (f.logHz) - std::log (440.0)) / std::log (2.0) : 0.0;
        fi.steady = f.voiced && sv1 && sv2 && std::fabs (cents - sc2) < 30.0; sv2 = sv1; sc2 = sc1; sv1 = f.voiced; sc1 = cents;
        if (f.voiced)
        {
            if (lastGoodOk && std::fabs (cents - lastGood) > 600.0 && !outlierPrev) { cents = lastGood; outlierPrev = true; }   // single-frame jump: ignore
            else outlierPrev = false;
            lastGood = cents; lastGoodOk = true;
        }
        // plausible key set
        int pr[6], ps[6], np = 0; bool has = false; int refR = 0, refS = 0;
        if (c.forcedKeys > 0)
        {
            np = std::min (6, c.forcedKeys); for (int q = 0; q < np; ++q) { pr[q] = std::clamp (c.forcedRoot[size_t (q)], 0, 11); ps[q] = std::clamp (c.forcedScale[size_t (q)], 0, 4); }
            has = true; refR = pr[0]; refS = ps[0]; lockPrev = false;
        }
        else
        {
            if (!(c.keyLock && lockPrev))
            {
                np = detector.plausible (pr, ps); has = detector.evidenceSeconds () >= kMinEvidence && np > 0;
                if (has) { refR = pr[0]; refS = ps[0]; for (int q = 0; q < np; ++q) if (pr[q] == detector.root () && ps[q] == detector.scale ()) { refR = pr[q]; refS = ps[q]; break; } }
                if (c.keyLock) { lockN = np; lockHas = has; lockRefR = refR; lockRefS = refS; for (int q = 0; q < np; ++q) { lockR[size_t (q)] = pr[q]; lockS[size_t (q)] = ps[q]; } }
            }
            else { np = lockN; has = lockHas; refR = lockRefR; refS = lockRefS; for (int q = 0; q < np; ++q) { pr[q] = lockR[size_t (q)]; ps[q] = lockS[size_t (q)]; } }
            lockPrev = c.keyLock;
        }
        fi.plausible = np;
        const double x = cents - double (detector.tuningCents ());
        bool agUp = false, agDown = false;
        if (f.voiced && has)
        {
            for (int dir : { 2, -2 })
            {   // every plausible key must give the same third
                double lo = 1.0e9, hi = -1.0e9;
                for (int q = 0; q < np; ++q) { const double a = thirdAbs (x, pr[q], ps[q], dir); lo = std::min (lo, a); hi = std::max (hi, a); }
                (dir > 0 ? agUp : agDown) = hi - lo <= 30.0;
            }
        }
        // note tracking
        if (!f.voiced) { ++unvoicedRun; if (unvoicedRun >= gateFrames) inNote = false; }
        else
        {
            bool onset = !inNote;
            if (inNote) { if (std::fabs (cents - anchorC) > 80.0) { if (++jumpRun >= 2) onset = true; } else jumpRun = 0; }
            unvoicedRun = 0;
            if (onset) { inNote = true; decided = false; ++noteId; onsetFrame = k; anchorC = cents; anchorN = 1; jumpRun = 0; winUp = winDown = winHas = true; winN = winUpN = winDownN = 0; }
            else if (!decided) { anchorC = (anchorC * anchorN + cents) / double (anchorN + 1); ++anchorN; }   // frozen after the decision
            if (!decided)
            {
                if (k - onsetFrame >= winFrom) { ++winN; winUpN += agUp; winDownN += agDown; winHas = winHas && has; }
                if (k - onsetFrame >= decideFrames)
                {   // third allowed only if every settled window frame (45-70 ms) agrees
                    decided = true; heldHas = winHas && has;
                    heldUp = heldHas && agUp && winUpN == winN && winN > 0; heldDown = heldHas && agDown && winDownN == winN && winN > 0; heldR = refR; heldS = refS;
                }
            }
        }
        fi.inNote = inNote; fi.decided = inNote && decided; fi.noteId = noteId; fi.onsetT = tracker.frameCenter (onsetFrame);
        fi.instUp = f.voiced && has && agUp; fi.instDown = f.voiced && has && agDown; fi.instR = refR; fi.instS = refS;
        fi.hasKey = heldHas; fi.agreeUp = heldUp; fi.agreeDown = heldDown; fi.refRoot = heldR; fi.refScale = heldS;
        if (f.voiced) { fi.noteCents = x; if (fi.decided && heldHas) { fi.upAbs = thirdAbs (x, heldR, heldS, 2); fi.downAbs = thirdAbs (x, heldR, heldS, -2); } }
        finfo[size_t (k & 1023)] = fi;
    }
    FrameInfo frameAt (int64_t td) const
    {
        if (td < 0 || detFrames <= 0) return FrameInfo ();
        const int64_t k = int64_t (std::floor ((double (td) - tracker.frameCenter (0)) / double (tracker.hopSize ()) + 0.5));
        if (k < 0 || k >= detFrames || detFrames - k > 1000) return FrameInfo ();
        return finfo[size_t (k & 1023)];
    }
    void beginStatus (const Controls& c, bool voicedNow, const FrameInfo& fi)
    {
        Status s;
        if (c.keyMode == KeyMode::Manual) { s.manual = true; s.hasKey = true; s.root = std::clamp (c.root, 0, 11); s.scale = std::clamp (c.scale, 0, 5); s.confidence = 1.0f; }
        else
        {
            s.hasKey = detector.hasKey (); s.root = detector.root (); s.scale = detector.scale (); s.confidence = detector.confidence ();
            s.locked = c.keyLock; s.gate = fi.decided; s.plausibleCount = fi.plausible;
            if (c.forcedKeys > 0) { s.hasKey = true; s.root = fi.refRoot; s.scale = fi.refScale; }
        }
        s.noteCents = fi.noteCents; s.noteId = fi.noteId; s.noteOnset = fi.valid ? fi.onsetT : -1.0;
        s.evidenceSeconds = detector.evidenceSeconds (); s.tuningCents = detector.tuningCents (); s.voicedNow = voicedNow;
        st = s;
    }
    static int classOf (int d, int scale)
    {
        const int a = std::abs (d);
        if (d == 0) return kUnison;
        if (scale == 5) return a == 7 ? kFifth : a == 12 ? kOctave : (a == 3 || a == 4) ? kThird : kOther;
        return a == 2 ? kThird : a == 4 ? kFifth : a == 7 ? kOctave : kOther;
    }
    // wanted degrees and COLOR weights of voice v (up to two)
    static int wantedDegrees (const Controls& c, int v, int vc, int sc, int* deg, float* w)
    {
        deg[0] = deg[1] = 0; w[0] = 1.0f; w[1] = 0.0f;
        if (c.color < 0.0f) { deg[0] = c.degree[size_t (v)]; return 1; }
        const float col = std::clamp (c.color, 0.0f, 1.0f);
        float f = col; int a = 0, b = 1;
        if (vc == 1) { const float x = col * 3.0f; const int i = std::min (2, int (std::floor (x))); f = x - float (i); a = i; b = i + 1; }
        else { static const int setA[3] = { 0, 1, 2 }, setB[3] = { 1, 2, 3 }; a = setA[v]; b = setB[v]; }
        deg[0] = anchor (a, sc); deg[1] = anchor (b, sc);
        w[0] = std::cos (0.5f * kPi * f); w[1] = std::sin (0.5f * kPi * f);
        if (w[1] < 1.0e-3f) { w[0] = 1.0f; return 1; }
        if (w[0] < 1.0e-3f) { deg[0] = deg[1]; w[0] = 1.0f; return 1; }
        return 2;
    }
    // AUTO spec of degree d on a decided note (held decision)
    static Spec autoSpec (int d, const Controls& c, const FrameInfo& fi, int& mode, bool& suppressed)
    {
        Spec s; suppressed = false; s.fixed = true; const int a = std::abs (d);
        if (d == 0) { s.cents = 0; mode = kUnison; return s; }
        if (a <= 2)
        {
            if (fi.hasKey && (d > 0 ? fi.agreeUp : fi.agreeDown)) { s.fixed = false; s.tuned = true; s.degree = d; s.root = fi.refRoot; s.scale = fi.refScale; mode = kThird; return s; }
            suppressed = true;
            // an unsafe third is NEVER replaced by an octave: COLOR in the third range (<= 0.5) and per-voice degree
            // mode fall back to UNISON (a plain double, ducked 3 dB); only COLOR between 0.5 and 0.83 falls back to the fifth
            const bool toFifth = c.color > 0.5f && c.color < 0.83f;
            s.cents = toFifth ? 702 : 0; mode = toFifth ? kFifth : kUnison; return s;
        }
        if (a <= 5) { s.cents = d > 0 ? 702 : -702; mode = kFifth; return s; }
        s.cents = d > 0 ? 1200 : -1200; mode = kOctave; return s;
    }
    void assignManual (Voice& V, const Controls& c, int v, int vc, int64_t td)
    {
        int deg[2]; float w[2]; int nd = wantedDegrees (c, v, vc, st.scale, deg, w);
        Spec sp[2];
        for (int j = 0; j < 2; ++j) { sp[j].fixed = false; sp[j].degree = deg[j]; sp[j].root = st.root; sp[j].scale = st.scale; }
        if (nd == 2 && sp[0] == sp[1]) { nd = 1; w[0] = std::sqrt (w[0] * w[0] + w[1] * w[1]); }
        const int dom = (nd == 2 && w[1] > w[0]) ? 1 : 0;
        st.mode[size_t (v)] = classOf (deg[dom], st.scale);
        assignFromSpecs (V, sp, w, nd, td);
    }
    void assignAuto (Voice& V, const Controls& c, int v, int vc, int64_t td, const FrameInfo& fi)
    {
        if (!V.hasApplied) { V.app[0] = Spec (); V.appW[0] = 1.0f; V.appMode[0] = kUnison; V.appDeg[0] = 0; V.appN = 1; V.hasApplied = true; V.lastChange = -1.0e18; }
        if (fi.valid && fi.inNote)
        {
            int deg[2]; float w[2]; int nd = wantedDegrees (c, v, vc, 0, deg, w);
            const double sinceOnset = double (td) - fi.onsetT;
            const bool eligible = double (td) - V.lastNonSafety >= minGap;   // withdrawals are exempt from spacing and do not restart it
            // current agreement of an applied third with the CURRENT plausible set (for withdrawals and upgrades)
            auto agreesNow = [&] (const Spec& spc, int d) {
                if (!fi.voiced || !fi.steady) return true;   // unvoiced or gliding between notes: not a sustained note, keep
                const int dir = d > 0 ? 2 : -2; const bool ag = d > 0 ? fi.instUp : fi.instDown;
                return ag && std::fabs (thirdAbs (fi.noteCents, spc.root, spc.scale, dir) - thirdAbs (fi.noteCents, fi.instR, fi.instS, dir)) <= 30.0;
            };
            bool withdraw = false;
            for (int q = 0; q < V.appN; ++q) if (V.appMode[size_t (q)] == kThird && !agreesNow (V.app[size_t (q)], V.appDeg[size_t (q)])) withdraw = true;
            int kind = 0;   // 1 decision, 2 upgrade, 3 withdrawal
            FrameInfo fd = fi;
            if (withdraw) { kind = 3; fd.agreeUp = fd.agreeDown = false; }
            else if (fi.decided && V.decNote != fi.noteId) kind = 1;
            else if (fi.decided && V.upNote != fi.noteId && sinceOnset <= upgradeWindow && fi.voiced)
            {   // one early upgrade to a third, within 600 ms of the onset, if the current plausible set agrees
                bool appThird = false; for (int q = 0; q < V.appN; ++q) appThird = appThird || V.appMode[size_t (q)] == kThird;
                if (!appThird && (fi.instUp || fi.instDown)) { kind = 2; fd.hasKey = true; fd.agreeUp = fi.instUp; fd.agreeDown = fi.instDown; fd.refRoot = fi.instR; fd.refScale = fi.instS; }
            }
            if (kind != 0)
            {
                int md[2] = { 0, 0 }; bool sup[2] = { false, false };
                Spec sp[2] = { autoSpec (deg[0], c, fd, md[0], sup[0]), autoSpec (deg[1], c, fd, md[1], sup[1]) };
                if (nd == 2 && sp[0] == sp[1]) { nd = 1; w[0] = std::sqrt (w[0] * w[0] + w[1] * w[1]); }
                if (kind == 1) { V.decNote = fi.noteId; V.noteSafe = sup[0] || (nd == 2 && sup[1]); }
                bool same = nd == V.appN;
                for (int jj = 0; jj < nd && same; ++jj) { bool f = false; for (int q = 0; q < V.appN; ++q) f = f || V.app[size_t (q)] == sp[jj]; same = f; }
                bool wantThird = false; for (int jj = 0; jj < nd; ++jj) wantThird = wantThird || md[jj] == kThird;
                if (kind == 2 && !wantThird) { /* nothing to upgrade to */ }
                else if (same) { for (int jj = 0; jj < nd; ++jj) for (int q = 0; q < V.appN; ++q) if (V.app[size_t (q)] == sp[jj]) V.appW[size_t (q)] = w[jj]; }
                else if (kind == 3 || eligible)
                {
                    ++stats.changes;
                    if (kind == 3) ++stats.withdrawals; else { if (kind == 2) ++stats.upgrades; if (sinceOnset > upgradeWindow) ++stats.lateChanges; if (V.lastNonSafety > -1.0e17) stats.minGapNonSafety = std::min (stats.minGapNonSafety, (double (td) - V.lastNonSafety) / sr); V.lastNonSafety = double (td); }
                    if (wantThird && kind != 3)
                    {   // a third is applied only if the plausible set agrees at this moment (the product promise)
                        for (int jj = 0; jj < nd; ++jj) if (md[jj] == kThird && !agreesNow (sp[jj], deg[jj])) ++stats.thirdsWithoutAgreement;
                        ++stats.thirdsApplied;
                    }
                    {   // unison <-> third on a single voice: glide the same shifter to the new interval (no two-pitch crossfade, no beating)
                        const bool oldUT = V.appN == 1 && (V.appMode[0] == kUnison || V.appMode[0] == kThird), newUT = nd == 1 && (md[0] == kUnison || md[0] == kThird);
                        if (kGlideSwap && oldUT && newUT) { V.swapPending = true; V.swapFrom = V.app[0]; }
                    }
                    V.appN = nd; for (int jj = 0; jj < nd; ++jj) { V.app[size_t (jj)] = sp[jj]; V.appW[size_t (jj)] = w[jj]; V.appMode[size_t (jj)] = md[jj]; V.appDeg[size_t (jj)] = deg[jj]; }
                    V.lastChange = double (td);
                    if (kind == 2) V.upNote = fi.noteId;
                    if (kind == 3) V.noteSafe = true;
                }
                else if (kind == 1) ++stats.skipped;
                else if (kind == 2) V.upNote = fi.noteId;   // upgrade blocked by spacing: used up for this note
            }
            if (V.noteSafe) st.safe = true;
            // agreement of what is playing now (info)
            for (int q = 0; q < V.appN; ++q) if (V.appMode[size_t (q)] == kThird && fi.voiced && !agreesNow (V.app[size_t (q)], V.appDeg[size_t (q)])) st.thirdDisagreesNow = true;
        }
        const int dom = (V.appN == 2 && V.appW[1] > V.appW[0]) ? 1 : 0;
        st.mode[size_t (v)] = V.appMode[size_t (dom)];
        st.thirdCents[size_t (v)] = (V.appMode[size_t (dom)] == kThird && fi.voiced) ? thirdAbs (fi.noteCents, V.app[size_t (dom)].root, V.app[size_t (dom)].scale, V.appDeg[size_t (dom)] > 0 ? 2 : -2) : 0.0;
        Spec sp[2] = { V.app[0], V.app[1] }; float w[2] = { V.appW[0], V.appW[1] };
        assignFromSpecs (V, sp, w, V.appN, td);
    }
    void assignFromSpecs (Voice& V, const Spec* sp, const float* w, int nd, int64_t td)
    {
        bool matched[kSlots] = { false, false, false, false, false, false };
        if (V.swapPending && nd == 1)
        {
            V.swapPending = false;
            for (auto& s : V.slot)
                if (s.live && s.target > 0.0f && s.spec == V.swapFrom)
                {   // keep the slot, its level and its current ratio; the next grains glide to the new interval
                    s.spec = sp[0]; s.held = false; s.idx = -1; s.lastValid = false; s.glideUntil = double (td) + 0.400 * sr;
                    for (auto& o : V.slot) if (&o != &s && o.live && o.spec == sp[0]) o.spec.cents += 100000;   // an old slot with the same spec keeps fading out
                    break;
                }
        }
        V.swapPending = false;
        for (int j = 0; j < nd; ++j)
        {
            int found = -1;
            for (int q = 0; q < kSlots; ++q) if (!matched[q] && V.slot[size_t (q)].live && V.slot[size_t (q)].spec == sp[j]) { found = q; break; }
            if (found < 0)
            {
                for (int q = 0; q < kSlots; ++q) if (!matched[q] && !V.slot[size_t (q)].live) { found = q; break; }
                if (found < 0) { float lo = 2.0f; for (int q = 0; q < kSlots; ++q) if (!matched[q] && V.slot[size_t (q)].p < lo) { lo = V.slot[size_t (q)].p; found = q; } }
                Slot& s = V.slot[size_t (found)];
                std::fill (s.ola.begin (), s.ola.end (), 0.0f);
                s.spec = sp[j]; s.live = true; s.p = 0.0f; s.nextSynth = std::max (0.0, double (td)); s.active = false; s.held = false; s.idx = -1; s.oct = 0; s.ratioCents = 0.0; s.lastGrain = 0.0; s.lastValid = false;
            }
            matched[found] = true; V.slot[size_t (found)].target = w[j];
        }
        for (int q = 0; q < kSlots; ++q) if (!matched[q]) V.slot[size_t (q)].target = 0.0f;
    }
    float readMono (double pos) const
    {
        const double fl = std::floor (pos); const int64_t i = int64_t (fl); const float f = float (pos - fl);
        const float a = mono[size_t (i & mask)], b = mono[size_t ((i + 1) & mask)];
        return a + (b - a) * f;
    }
    void scheduleGrain (Slot& V, int64_t td, double tauGlide, bool v2)
    {
        const double ts = std::max (V.nextSynth, double (td));
        float hz = 0.0f;
        const Mark* best = nullptr; const Mark* m2 = nullptr; double alpha = 0.0;
        if (tracker.pitchAt (ts, hz))
        {
            double bd = 1.0e18; int64_t bj = -1;
            for (int64_t j = markCount - 1; j >= 0 && j >= markCount - kMarkMask; --j)
            {
                const Mark& mk = marks[size_t (j & kMarkMask)];
                const double dd = std::fabs (mk.pos - ts);
                if (dd < bd) { bd = dd; best = &mk; bj = j; }
                if (mk.pos < ts - 2.0 * pmax) break;
            }
            if (best && bd > best->per) best = nullptr;
            if (best && v2 && (kV2Parts & 32))
            {   // the two marks around ts: blend their grains by the fractional position
                const int64_t ja = best->pos <= ts ? bj : bj - 1, jb = ja + 1;
                if (ja >= 0 && jb < markCount && ja > markCount - kMarkMask)
                {
                    const Mark& ma = marks[size_t (ja & kMarkMask)]; const Mark& mb = marks[size_t (jb & kMarkMask)]; const double span = mb.pos - ma.pos;
                    if (span > 0.5 * ma.per && span < 1.5 * ma.per && ts >= ma.pos && ts <= mb.pos) { best = &ma; m2 = &mb; alpha = kBlendAlpha < 0.0 ? (ts - ma.pos) / span : kBlendAlpha; }
                }
            }
        }
        if (!best) { V.nextSynth = ts + 0.25 * tracker.hopSize (); V.active = false; V.held = false; V.lastValid = false; return; }
        double target = 0.0;
        if (V.spec.fixed) target = V.spec.cents;
        else
        {
            const double midiCents = 6900.0 + 1200.0 * std::log2 (double (hz) / 440.0);
            scales::nearest (V.spec.tuned ? midiCents - double (detector.tuningCents ()) : midiCents, V.spec.root, V.spec.scale, V.held, V.idx, V.oct, 20.0);
            V.held = true;
            target = scales::intervalCents (V.idx, V.oct, V.spec.degree, V.spec.scale);
        }
        const double tauUse = ts < V.glideUntil ? 0.070 * sr : tauGlide;
        if (!V.active || tauUse <= 0.0) V.ratioCents = target;
        else V.ratioCents += (target - V.ratioCents) * (1.0 - std::exp (-(ts - V.lastGrain) / tauUse));
        const double ratio = std::pow (2.0, V.ratioCents / 1200.0);
        double pa = best->per, pt = pa / ratio, half = pa, g = 1.0 / ratio;
        if (v2)
        {
            const int64_t it = int64_t (std::lround (ts));
            const double rawC = 6900.0 + 1200.0 * std::log2 (double (hz) / 440.0);
            // spacing = analysis period / ratio (phase-coherent with the grain content); the conditioned pitch replaces
            // it only when the mark's period is an outlier (> 50 c off the conditioned track: tracker octave errors)
            const double inC = 6900.0 + 1200.0 * std::log2 (sr / pa / 440.0); (void) rawC;   // the grain's own (octave-error-held) period
            const double ptNom = pa / ratio;                                                  // phase-coherent spacing
            half = ((kV2Parts & 2) && ratio > 1.0) ? 1.4 * pa : pa; g = ptNom / half; pt = ptNom;   // long grains only when shifting up (2.8-period grains at a 2-period spacing re-create the source pitch)
            if (kV2Parts & 8)
            {   // placement jitter +-3 % of the synthesis period around the regular grid (seeded by spec and grid time: repeatable)
                uint32_t hsh = uint32_t (it) * 2654435761u ^ uint32_t (V.spec.fixed ? V.spec.cents + 5000 : V.spec.degree * 131 + V.spec.root * 7 + V.spec.scale);
                hsh ^= hsh >> 13; hsh *= 0x5bd1e995u; hsh ^= hsh >> 15;
                jitterOff = 0.03 * ptNom * (double (hsh & 0xffffu) / 32767.5 - 1.0);
            }
            // target-jump statistics (within a note, per slot)
            const FrameInfo fi = frameAt (it); const double tc = inC + V.ratioCents;
            if (fi.valid && fi.inNote)
            {
                const FrameInfo fa = frameAt (it + int64_t (0.020 * sr));
                const bool sus = double (it) - fi.onsetT >= window && (!fa.valid || fa.noteId == fi.noteId);   // not within 20 ms of the next onset
                if (V.lastValid && V.lastNote == fi.noteId)
                {
                    const double j = std::fabs (tc - V.lastTarget); stats.maxJumpNote = std::max (stats.maxJumpNote, j);
                    if (sus && V.lastSus) { stats.maxJumpSustained = std::max (stats.maxJumpSustained, j); ++stats.grainPairsSustained; }
                }
                V.lastValid = true; V.lastNote = fi.noteId; V.lastTarget = tc; V.lastSus = sus;
            }
            else V.lastValid = false;
        }
        const double tp = ts + jitterOff; jitterOff = 0.0;
        const int64_t m0 = std::max (td, int64_t (std::ceil (tp - half))), m1 = int64_t (std::floor (tp + half));
        for (int64_t mm = m0; mm <= m1; ++mm)
        {
            const double u = double (mm) - tp;
            const double w = 0.5 * (1.0 + std::cos (3.14159265358979323846 * u / half));
            const float src = m2 ? float ((1.0 - alpha) * readMono (best->pos + u) + alpha * readMono (m2->pos + u)) : readMono (best->pos + u);
            V.ola[size_t (mm & mask)] += float (g * w) * src;
        }
        V.nextSynth = ts + pt; V.active = true; V.lastGrain = ts;
    }
    double sr = 44100.0;
    PitchTracker tracker;
    KeyDetector detector;
    int64_t detFrames = 0;
    int outHold = 1323, winN = 0, winUpN = 0, winDownN = 0, smGap = 0, pmax = 640, lat = 0, gateFrames = 12, decideFrames = 14, winFrom = 6, unvoicedRun = 0, lockN = 0, lockRefR = 0, lockRefS = 0, anchorN = 0, jumpRun = 0, outRun = 0, heldR = 0, heldS = 0;
    std::array<FrameInfo, 1024> finfo{};
    std::array<int, 6> lockR{}, lockS{};
    bool sv1 = false, sv2 = false, octHolding = false, lockHas = false, inNote = false, decided = false, lastGoodOk = false, outlierPrev = false, winUp = true, winDown = true, winHas = true, heldUp = false, heldDown = false, heldHas = false, smValid = false;
    int64_t noteId = 0, onsetFrame = 0;
    double upgradeWindow = 26460.0, sc1 = 0.0, sc2 = 0.0, octStart = -1.0e18, slewMax = 0.34, jitterOff = 0.0, anchorC = 0.0, lastGood = 0.0, smCur = 0.0, smA = 0.001, minGap = 13230.0, window = 4410.0;
    int64_t mask = 0, n = 0, markCount = 0;
    double nextMark = 0.0;
    float duckStep = 1.0e-4f, vgain = 0.0f, xfadeStep = 0.003f, envA = 0.999f, rampStep = 1.0e-4f, toneL = 0.0f, toneR = 0.0f;
    bool started = false, lockPrev = false;
    float gDry = 1.0f, gWet = 0.0f; bool gInit = false;   // KORO plug-in change: dry / wet gains of the HARMONY law, interpolated per sample across a call
    Status st; Stats stats;
    std::vector<float> mono, inL, inR, smLog;
    std::vector<uint8_t> smOk;
    std::vector<double> refBuf;
    std::array<Mark, 1024> marks{};
    std::array<Voice, 3> voice;
};

class CifteEngine
{
public:
    void prepare (double sampleRate)
    {
        sr = float (sampleRate);
        const size_t size = size_t (std::ceil (0.060 * sr)) + 8u;
        for (int v = 0; v < 3; ++v)
        {
            for (int ch = 0; ch < 2; ++ch) delay[v][ch].assign (size, 0.0f);
            writePos[v] = 0;
            rng[v] = Rng (0xC1F7u + uint32_t (v) * 7919u);
            drift[v].lowpass (0.55f + 0.38f * float (v), sr);
            drift[v].z = 0.0f;
            for (int ch = 0; ch < 2; ++ch) tilt[v][ch].highShelf (3600.0f + 500.0f * v, (v - 1) * 0.75f, sr);
            tone[v][0].lowpass = 0.0f; tone[v][1].lowpass = 0.0f;
            toneState[v][0] = toneState[v][1] = 0.0f;
            vGain[v] = 0.0f;
        }
        vgInit = false; mixCur = 0.0f; mixInit = false;
        harmonizer.prepare (sampleRate);
        dryL.assign (kChunk, 0.0f); dryR.assign (kChunk, 0.0f);
        prepared = true;
    }

    // dry -> doubler (zero-latency, unchanged) -> delayed by latency(); harmony voices from the DRY input
    // are added by the harmonizer. Output is always latency() samples late, harmony on or off.
    void process (float* left, float* right, int n, const Controls& controls)
    {
        if (!prepared || n <= 0) return;
        for (int p = 0; p < n; p += kChunk)
        {
            const int m = std::min (kChunk, n - p);
            std::copy (left + p, left + p + m, dryL.begin ()); std::copy (right + p, right + p + m, dryR.begin ());
            processLegacy (left + p, right + p, m, controls);
            harmonizer.process (dryL.data (), dryR.data (), left + p, right + p, m, controls);
        }
    }

    // The doubler alone, zero latency (the approved doubler; used by process() and as the test reference).
    void processLegacy (float* left, float* right, int n, const Controls& controls)
    {
        if (!prepared || n <= 0) return;
        const int count = std::max (1, std::min (3, controls.voices));
        const float mix = std::max (0.0f, std::min (1.0f, controls.mix));
        const float toneHz = 3000.0f * std::pow (16.0f / 3.0f, std::max (0.0f, std::min (1.0f, controls.tone)));
        for (int v = 0; v < 3; ++v)
        {
            const float cutoff = std::min (0.45f * sr, toneHz * (1.0f - 0.035f * float (v)));
            tone[v][0].a = std::exp (-2.0f * kPi * cutoff / sr); tone[v][1].a = tone[v][0].a;
        }
        // KORO plug-in change (click fix, approved engine change): a voice that enters or leaves (DOUBLE crossing 3.0 / 6.5) fades
        // linearly over 20 ms instead of appearing or vanishing at full level, and MIX is interpolated per sample across the call.
        // A voice that is fully in has gain 1 and a constant MIX has step 0, so a constant setting is bit-identical to the original.
        if (!vgInit) { for (int v = 0; v < 3; ++v) vGain[v] = v < count ? 1.0f : 0.0f; vgInit = true; }
        if (!mixInit) { mixCur = mix; mixInit = true; }
        const float fadeStep = 1.0f / (0.020f * sr), mixStep = (mix - mixCur) / float (n);
        for (int i = 0; i < n; ++i)
        {
            const float inL = left[i], inR = right[i];
            float wetL = 0.0f, wetR = 0.0f;
            if (mixStep != 0.0f) mixCur += mixStep;
            for (int v = 0; v < 3; ++v)
            {
                float& vg = vGain[v];
                if (v >= count && vg <= 0.0f)
                {   // fully out: keep the delay line fed so a voice that comes back reads the recent input, not old audio
                    delay[v][0][size_t (writePos[v])] = inL;
                    delay[v][1][size_t (writePos[v])] = inR;
                    continue;
                }
                if (v < count) { if (vg < 1.0f) vg = std::min (1.0f, vg + fadeStep); }
                else vg = std::max (0.0f, vg - fadeStep);
                const float wander = drift[v].process (rng[v].bipolar());
                const float safe = controls.monoSafe ? 0.18f : 1.0f;
                const float baseMs = (8.0f + 13.0f * float (v)) * (1.0f - 0.72f * std::max (0.0f, std::min (1.0f, controls.tight))) * safe;
                const float depthMs = 12.0f * std::max (0.0f, std::min (1.0f, controls.spread)) * safe;
                const float delaySamples = std::max (1.0f, (baseMs + depthMs * wander) * 0.001f * sr);
                delay[v][0][size_t (writePos[v])] = inL;
                delay[v][1][size_t (writePos[v])] = inR;
                const float dl = read (delay[v][0], delaySamples, writePos[v]);
                const float dr = read (delay[v][1], delaySamples, writePos[v]);
                const float pl = (v == 0 ? 0.5f + 0.5f * controls.width : v == 1 ? 0.5f - 0.5f * controls.width : 0.5f);
                const float pr = (v == 0 ? 0.5f - 0.5f * controls.width : v == 1 ? 0.5f + 0.5f * controls.width : 0.5f);
                const float level = std::pow (10.0f, (-1.0f - float (v)) / 20.0f) * 0.92f;
                float vl = tone[v][0].process (tilt[v][0].process (dl)) * level;
                float vr = tone[v][1].process (tilt[v][1].process (dr)) * level;
                if (v == 2) { vl = tone[v][0].process (0.5f * (dl + dr)) * level; vr = vl; }
                if (vg != 1.0f) { vl *= vg; vr *= vg; }
                wetL += vl * pl; wetR += vr * pr;
            }
            ++writePos[0]; ++writePos[1]; ++writePos[2];
            for (int v = 0; v < 3; ++v) if (writePos[v] >= int (delay[v][0].size())) writePos[v] = 0;
            const float mx = mixStep != 0.0f ? mixCur : mix;
            left[i] = inL * (1.0f - mx) + wetL * mx;
            right[i] = inR * (1.0f - mx) + wetR * mx;
        }
        mixCur = mix;
    }

    // Fixed plugin latency (54.1 ms: 2386 samples at 44.1 kHz), the same for every setting.
    int latency () const { return harmonizer.latency (); }
    int latency (const Controls&) const { return harmonizer.latency (); }
    int harmonyLatency () const { return harmonizer.latency (); }
    const Harmonizer::Status& keyStatus () const { return harmonizer.status (); }
    const Harmonizer::Stats& shifterStats () const { return harmonizer.shifterStats (); }
    const KeyDetector& keyDetector () const { return harmonizer.keyDetector (); }
    double voiceIntervalCents (int v) const { return harmonizer.voiceIntervalCents (v); }

private:
    static constexpr int kChunk = 1024;
    static float read (const std::vector<float>& d, float delaySamples, int write)
    {
        const int n = int (d.size()), i = int (delaySamples);
        const int i0 = (write - i + n) % n, ip1 = (i0 + 1) % n, im1 = (i0 - 1 + n) % n, im2 = (i0 - 2 + n) % n;
        const float f = delaySamples - float (i), y0 = d[size_t (ip1)], y1 = d[size_t (i0)], y2 = d[size_t (im1)], y3 = d[size_t (im2)];
        const float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * f + c2) * f + c1) * f + y1;
    }
    struct Tone { float a = 0.0f, lowpass = 0.0f; float process (float x) { lowpass = x + a * (lowpass - x); return lowpass; } };
    float sr = 44100.0f;
    bool prepared = false;
    std::array<std::array<std::vector<float>, 2>, 3> delay;
    std::array<int, 3> writePos{};
    std::array<Rng, 3> rng{ Rng (1), Rng (2), Rng (3) };
    std::array<OnePole, 3> drift;
    std::array<std::array<Biquad, 2>, 3> tilt;
    std::array<std::array<Tone, 2>, 3> tone;
    std::array<std::array<float, 2>, 3> toneState{};
    std::array<float, 3> vGain{};   // KORO plug-in change: per-voice entry / exit fade
    float mixCur = 0.0f; bool vgInit = false, mixInit = false;
    std::vector<float> dryL, dryR;
    Harmonizer harmonizer;
};
}
