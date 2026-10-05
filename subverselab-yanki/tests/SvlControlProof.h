// SvlControlProof.h: one shared harness that proves every knob of a plug-in behaves, in every variation.
// Master: subverselab-plugin-common/ControlProof (sync_controlproof.sh copies it into each project's tests/).
//
// It runs against any juce::AudioProcessor through prepareToPlay / processBlock (never the bare engine) and
// enumerates getParameters(): continuous, choice and bool. Include it after JuceHeader.h:
//
//   svl::controlproof::Config cfg;  cfg.name = "TUTKAL";  cfg.make = [] { return std::make_unique<MyProcessor>(); };
//   cfg.directions = { { "mix", +1, svl::controlproof::feat::outRmsDb(), 0.25 }, ... };
//   return svl::controlproof::main (cfg, argc, argv);        // parses --controlproof options
//
// Checks (numbers as in the owner's brief). Options: --quick, --checks 1,3, --param ID, --states N, --tail SEC, --set "id=0.5 ..." (normalised overrides of
// state 0), --probe "id=0.5 ..." (one render + one-parameter ablation), --out DIR, --docs FILE, --no-docs, --controlproof-selftest (the harness against injected faults).
//  1 sweep        every parameter, 11 positions (every choice index, both bool states) x 25 states (0 = defaults, 24 seeded random), 1.0 s per point;
//                 material rotated pink / drums / chord (+ a custom one if the plug-in gives it; state 0 runs all of them).
//                 finite; peak <= +12 dBFS and <= input peak + 24 dB (a point above that is accepted only if the plug-in's designed gain explains it:
//                 peak <= input peak + designed gain + 6 dB, and it is counted and reported); DC: |mean| over the last 100 ms, judged as the excess over
//                 the input's own mean x designed gain, and only if the 500 ms mean confirms an offset (noise-like LF wander is not DC); the strict
//                 |mean| < 0.01 count is reported too.
//                 Tails: 0.5 s drum burst, then silence for the documented tail; last 100 ms peak < -90 dBFS (or the plug-in's declared noise floor),
//                 peak after the burst <= 0 dBFS; states that sustain by design (freeze, swell, hiss) must stay <= +12 dBFS; silence in -> < -90 dBFS.
//  2 direction    declared direction per parameter, measured with a feature at every position, in the default state and 5 random states (with the
//                 declaration's forced parameters), monotonic within a stated tolerance (values under a declared floor count as equal); the default
//                 state must also span at least the tolerance (a knob that does nothing fails); a declaration that tests no state fails.
//  3 automation   triangle ramp min->max->min over 300 ms (parameter set once per host block) at blocks 512 and 64 vs block 1: max |diff|; discontinuity =
//                 largest sample step of (out - 33-sample centred moving average of out) must not exceed the larger of the static min / static max
//                 renders by more than 6 dB (or, for a knob whose effect is not monotone, the largest of 5 static positions); discrete parameters: reported.
//  4 blocks       blocks 1, 64, 333, 512, 4096 and random give the same output (every render on a fresh instance); static random states, and
//                 per-parameter ramps with sample-accurate automation (the host cuts the block at every 64-sample grid line, the value is defined on the grid).
//  5 sample rate  44.1 / 48 / 96 kHz: check 1 at 3 positions per parameter x 5 states; reported latency vs an impulse measurement (exact).
//  6 state        200 random states: getStateInformation -> fresh instance setStateInformation: every parameter equal (normalised, bit for bit; a float-ulp
//                 difference of a skewed range is reported, not failed), renders bit-identical (or <= 1e-6 when such an ulp difference explains it).
//  7 corners      all min, all max (with silence too), and 500 random states x 0.25 s: finite and bounded; with the sidechain bus on and off.
//  8 cpu          worst processBlock time over check 1 (block 512, 48 kHz) as % of real time; a worst call over 50 % is replayed 6 times and judged by its fastest.
#pragma once

#include <JuceHeader.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace svl::controlproof
{
// ---------------------------------------------------------------------------------------------------------------
// small utilities
inline double toDb (double x) { return 20.0 * std::log10 (std::max (x, 1e-12)); }

struct Rng
{
    uint64_t s;
    explicit Rng (uint64_t seed = 1) : s (seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) {}
    uint64_t u64() { s += 0x9E3779B97F4A7C15ull; uint64_t z = s; z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; return z ^ (z >> 31); }
    double uni() { return (double) (u64() >> 11) * (1.0 / 9007199254740992.0); }
    double gauss() { double a = std::max (uni(), 1e-12), b = uni(); return std::sqrt (-2.0 * std::log (a)) * std::cos (6.283185307179586 * b); }
    int below (int n) { return (int) (uni() * n) % std::max (1, n); }
};

struct Sig
{
    std::vector<float> c[2];
    int n() const { return (int) c[0].size(); }
    void zeros (int len) { c[0].assign ((size_t) len, 0.0f); c[1].assign ((size_t) len, 0.0f); }
    double peak() const { double m = 0; for (auto& ch : c) for (float v : ch) m = std::max (m, (double) std::abs (v)); return m; }
    bool finite() const { for (auto& ch : c) for (float v : ch) if (! std::isfinite (v)) return false; return true; }
};

enum Mat { kPink = 0, kDrums = 1, kChord = 2, kSilence = 3, kCustom = 4 };
inline std::string& customMatName() { static std::string n = "custom"; return n; }
inline const char* matName (int m) { static const char* n[] = { "pink", "drums", "chord", "silence" }; return m == 4 ? customMatName().c_str() : n[m & 3]; }

struct Materials
{
    Sig m[5];        // pink, drums, chord, silence, custom (length = seconds)
    Sig kick;        // the kick alone: the sidechain key
    double sr = 48000.0;
};

inline Materials makeMaterials (double sr, double seconds, const std::function<void (Sig&, double, int)>& custom = nullptr)
{
    Materials M; M.sr = sr;
    const int n = (int) std::lround (sr * seconds);
    // pink noise, Paul Kellet's filter, independent channels, scaled to -18 dBFS RMS (both channels together)
    {
        Rng r (11);
        Sig& p = M.m[kPink]; p.zeros (n);
        for (int ch = 0; ch < 2; ++ch)
        {
            double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
            for (int i = 0; i < n + 4000; ++i)
            {
                const double w = r.gauss();
                b0 = 0.99886 * b0 + w * 0.0555179; b1 = 0.99332 * b1 + w * 0.0750759; b2 = 0.96900 * b2 + w * 0.1538520;
                b3 = 0.86650 * b3 + w * 0.3104856; b4 = 0.55000 * b4 + w * 0.5329522; b5 = -0.7616 * b5 - w * 0.0168980;
                const double y = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362; b6 = w * 0.115926;
                if (i >= 4000) p.c[ch][(size_t) (i - 4000)] = (float) y;
            }
        }
        // 30 Hz high-pass (RBJ biquad, Q 0.7071): raw 1/f noise has a mean over 100 ms of ~0.09, which is a property of the material, not of a plug-in
        for (auto& ch : p.c)
        {
            const double w0 = 6.283185307179586 * 30.0 / sr, cw = std::cos (w0), al = std::sin (w0) / (2.0 * 0.7071), a0 = 1.0 + al;
            const double b0 = (1.0 + cw) * 0.5 / a0, b1 = -(1.0 + cw) / a0, b2 = b0, a1 = -2.0 * cw / a0, a2 = (1.0 - al) / a0;
            double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
            for (float& v : ch) { const double x = v, y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2; x2 = x1; x1 = x; y2 = y1; y1 = y; v = (float) y; }
        }
        double e = 0; for (auto& ch : p.c) for (float v : ch) e += (double) v * v;
        const double rms = std::sqrt (e / (2.0 * n)), g = std::pow (10.0, -18.0 / 20.0) / std::max (rms, 1e-12);
        for (auto& ch : p.c) for (float& v : ch) v = (float) (v * g);
    }
    // drum-like hit train, 120 bpm: kick (55 Hz sine, exp decay) on the beat, decaying noise burst (snare) on the off-beat,
    // short hat bursts on the 8ths; the whole train peaks at -6 dBFS.
    {
        Rng r (22);
        Sig& d = M.m[kDrums]; d.zeros (n); M.kick.zeros (n);
        auto addKick = [&] (Sig& s, double t0)
        {
            const int i0 = (int) (t0 * sr);
            for (int i = 0; i < (int) (0.45 * sr) && i0 + i < n; ++i)
            {
                const double t = (double) i / sr;
                const double env = std::min (1.0, t / 0.002) * std::exp (-t / 0.11);
                const float v = (float) (std::sin (6.283185307179586 * 55.0 * t) * env);
                s.c[0][(size_t) (i0 + i)] += v; s.c[1][(size_t) (i0 + i)] += v;
            }
        };
        auto addNoise = [&] (double t0, double len, double tau, double amp)
        {
            const int i0 = (int) (t0 * sr);
            for (int i = 0; i < (int) (len * sr) && i0 + i < n; ++i)
            {
                const double env = std::exp (-((double) i / sr) / tau) * amp;
                d.c[0][(size_t) (i0 + i)] += (float) (r.gauss() * env * 0.5);
                d.c[1][(size_t) (i0 + i)] += (float) (r.gauss() * env * 0.5);
            }
        };
        for (double t = 0.02; t < seconds; t += 0.5) { addKick (d, t); addKick (M.kick, t); }
        for (double t = 0.27; t < seconds; t += 0.5) addNoise (t, 0.25, 0.04, 0.45);
        for (double t = 0.145; t < seconds; t += 0.25) addNoise (t, 0.06, 0.008, 0.18);
        const double pk = d.peak(), g = 0.5011872336 / std::max (pk, 1e-9);   // -6 dBFS
        for (auto& ch : d.c) for (float& v : ch) v = (float) (v * g);
        const double kp = M.kick.peak(), kg = 0.5011872336 / std::max (kp, 1e-9);
        for (auto& ch : M.kick.c) for (float& v : ch) v = (float) (v * kg);
    }
    // sustained chord: three sines (A3 / C#4 / E4), each -12 dBFS peak, mono-compatible, 5 ms fade in
    {
        Sig& s = M.m[kChord]; s.zeros (n);
        const double f[3] = { 220.0, 277.18, 329.63 }, amp = std::pow (10.0, -12.0 / 20.0);
        for (int i = 0; i < n; ++i)
        {
            const double t = (double) i / sr;
            double v = 0; for (int k = 0; k < 3; ++k) v += std::sin (6.283185307179586 * f[k] * t + 0.7 * k);
            v *= amp * std::min (1.0, t / 0.005);
            s.c[0][(size_t) i] = s.c[1][(size_t) i] = (float) v;
        }
    }
    M.m[kSilence].zeros (n);
    if (custom) custom (M.m[kCustom], sr, n);
    return M;
}

/** the first `burstSec` of the drum train, then silence, `total` samples long */
inline Sig burstThenSilence (const Materials& M, double burstSec, int total)
{
    Sig s; s.zeros (total);
    const int b = std::min (total, (int) (burstSec * M.sr));
    for (int ch = 0; ch < 2; ++ch) std::copy (M.m[kDrums].c[ch].begin(), M.m[kDrums].c[ch].begin() + b, s.c[ch].begin());
    return s;
}

// ---------------------------------------------------------------------------------------------------------------
// parameters and states
struct Par
{
    juce::AudioProcessorParameter* p = nullptr;
    juce::String id, name, label, minText, maxText;
    bool isBool = false, isChoice = false, discrete = false;
    int steps = 0;                      // discrete: number of values
    float def = 0.0f;                   // normalised default
    juce::RangedAudioParameter* ranged = nullptr;
};
using State = std::vector<float>;       // normalised 0..1 per parameter, in getParameters() order

struct StateView
{
    const std::vector<Par>& pars; const State& v;
    int index (const juce::String& id) const { for (size_t i = 0; i < pars.size(); ++i) if (pars[i].id == id) return (int) i; return -1; }
    float norm (const juce::String& id) const { const int i = index (id); jassert (i >= 0); return v[(size_t) i]; }
    float raw (const juce::String& id) const { const int i = index (id); jassert (i >= 0); const auto& q = pars[(size_t) i]; return q.ranged ? q.ranged->convertFrom0to1 (v[(size_t) i]) : v[(size_t) i]; }
    int choice (const juce::String& id) const { return (int) std::lround (raw (id)); }
    bool on (const juce::String& id) const { return norm (id) > 0.5f; }
};

struct RenderView
{
    const Sig& in; const Sig& out; const Sig* key; double sr; int latency; juce::AudioProcessor& proc; const StateView& state;
    const Sig* ref = nullptr;   // the render at the parameter's minimum position, same state (for "amount" knobs: how far from the knob-off render)
};
using Feature = std::function<double (const RenderView&)>;
using Cond = std::function<bool (const StateView&)>;

// ---------------------------------------------------------------------------------------------------------------
// measurement features for the direction check (all in dB unless said otherwise); analysis window = after the first 100 ms
namespace feat
{
inline double rmsOf (const Sig& s, int a, int b)
{
    double e = 0; int cnt = 0;
    for (auto& ch : s.c) for (int i = a; i < b && i < (int) ch.size(); ++i) { e += (double) ch[(size_t) i] * ch[(size_t) i]; ++cnt; }
    return cnt ? std::sqrt (e / cnt) : 0.0;
}
inline void fft (std::vector<std::complex<double>>& a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) { size_t bit = n >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) std::swap (a[i], a[j]); }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = -6.283185307179586 / (double) len;
        const std::complex<double> wl (std::cos (ang), std::sin (ang));
        for (size_t i = 0; i < n; i += len)
        {
            std::complex<double> w (1);
            for (size_t k = 0; k < len / 2; ++k) { auto u = a[i + k], v = a[i + k + len / 2] * w; a[i + k] = u + v; a[i + k + len / 2] = u - v; w *= wl; }
        }
    }
}
/** band power (linear, summed over both channels) of the last 2^15 samples of s between lo and hi Hz */
inline double bandPower (const Sig& s, double sr, double lo, double hi)
{
    const int N = 32768; const int n = s.n(); double tot = 0;
    for (int ch = 0; ch < 2; ++ch)
    {
        std::vector<std::complex<double>> a ((size_t) N);
        for (int i = 0; i < N; ++i)
        {
            const int k = n - N + i; const double w = 0.5 - 0.5 * std::cos (6.283185307179586 * i / (N - 1));
            a[(size_t) i] = (k >= 0 ? (double) s.c[ch][(size_t) k] : 0.0) * w;
        }
        fft (a);
        for (int b = 1; b < N / 2; ++b) { const double f = b * sr / N; if (f >= lo && f < hi) tot += std::norm (a[(size_t) b]); }
    }
    return tot / ((double) N * N);
}
inline Feature outRmsDb()       { return [] (const RenderView& r) { const int a = (int) (0.1 * r.sr); return toDb (rmsOf (r.out, a, r.out.n())); }; }
inline Feature outPeakDb()      { return [] (const RenderView& r) { return toDb (r.out.peak()); }; }
/** output rms minus input rms (after the first 100 ms): the gain the plug-in applied to the programme */
inline Feature gainDb()         { return [] (const RenderView& r) { const int a = (int) (0.1 * r.sr); return toDb (rmsOf (r.out, a, r.out.n())) - toDb (rmsOf (r.in, a, r.in.n())); }; }
/** -gainDb: dB of reduction */
inline Feature reductionDb()    { return [] (const RenderView& r) { const int a = (int) (0.1 * r.sr); return toDb (rmsOf (r.in, a, r.in.n())) - toDb (rmsOf (r.out, a, r.out.n())); }; }
/** peak minus rms of the output (dB), after the first 100 ms: how much of the transient survives */
inline Feature crestDb()        { return [] (const RenderView& r)
{
    const int a = (int) (0.1 * r.sr); double pk = 0;
    for (auto& ch : r.out.c) for (int i = a; i < r.out.n(); ++i) pk = std::max (pk, (double) std::abs (ch[(size_t) i]));
    return toDb (pk) - toDb (rmsOf (r.out, a, r.out.n()));
}; }
inline Feature bandDb (double lo, double hi) { return [=] (const RenderView& r) { return 10.0 * std::log10 (std::max (bandPower (r.out, r.sr, lo, hi), 1e-30)); }; }
/** output band power minus input band power */
inline Feature bandGainDb (double lo, double hi) { return [=] (const RenderView& r) { return 10.0 * std::log10 (std::max (bandPower (r.out, r.sr, lo, hi), 1e-30)) - 10.0 * std::log10 (std::max (bandPower (r.in, r.sr, lo, hi), 1e-30)); }; }
/** power in band 1 over power in band 2 of the output (dB): a distortion-to-fundamental measure that does not move with the overall level */
inline Feature bandRatioDb (double lo1, double hi1, double lo2, double hi2) { return [=] (const RenderView& r) { return 10.0 * std::log10 (std::max (bandPower (r.out, r.sr, lo1, hi1), 1e-30) / std::max (bandPower (r.out, r.sr, lo2, hi2), 1e-30)); }; }
/** high band (hi..sr/2.1) power relative to the whole output, in dB: a brightness measure */
inline Feature brightnessDb (double lo = 4000.0) { return [=] (const RenderView& r) { return 10.0 * std::log10 (std::max (bandPower (r.out, r.sr, lo, r.sr * 0.47), 1e-30) / std::max (bandPower (r.out, r.sr, 20.0, r.sr * 0.47), 1e-30)); }; }
/** the output with the (latency-aligned) input subtracted: the wet part, rms dB */
inline Feature wetRmsDb()       { return [] (const RenderView& r)
{
    const int a = (int) (0.1 * r.sr), L = r.latency; double e = 0; int cnt = 0;
    for (int ch = 0; ch < 2; ++ch) for (int i = a; i < r.out.n(); ++i) { const float d = r.out.c[ch][(size_t) i] - (i - L >= 0 ? r.in.c[ch][(size_t) (i - L)] : 0.0f); e += (double) d * d; ++cnt; }
    return toDb (std::sqrt (e / std::max (cnt, 1)));
}; }
/** rms (dB) of (output - the render with the knob at its minimum), after the first 100 ms: grows with the amount of the effect (-240 dB at the minimum itself) */
inline Feature devFromMinDb() { return [] (const RenderView& r)
{
    if (! r.ref) return -240.0;
    Sig d; d.zeros (r.out.n());
    for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < r.out.n(); ++i) d.c[ch][(size_t) i] = r.out.c[ch][(size_t) i] - r.ref->c[ch][(size_t) i];
    return toDb (rmsOf (d, (int) (0.1 * r.sr), d.n()));
}; }
/** rms (dB) of the output between start and start + len seconds: an onset / delay measure (e.g. the energy that arrives early after a hit) */
inline Feature outWindowDb (double startSec, double lenSec) { return [=] (const RenderView& r) { return toDb (rmsOf (r.out, (int) (startSec * r.sr), (int) ((startSec + lenSec) * r.sr))); }; }
/** the wet part (output minus latency-aligned input) as a signal */
inline Sig wetSig (const RenderView& r)
{
    Sig d; d.zeros (r.out.n());
    for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < r.out.n(); ++i) d.c[ch][(size_t) i] = r.out.c[ch][(size_t) i] - (i - r.latency >= 0 ? r.in.c[ch][(size_t) (i - r.latency)] : 0.0f);
    return d;
}
/** stereo width of the output: side power over mid power (dB), after the first 100 ms */
inline Feature stereoWidthDb() { return [] (const RenderView& r)
{
    double m = 0, sd = 0; const int a = (int) (0.1 * r.sr);
    for (int i = a; i < r.out.n(); ++i) { const double l = r.out.c[0][(size_t) i], rr = r.out.c[1][(size_t) i]; m += (l + rr) * (l + rr) * 0.25; sd += (l - rr) * (l - rr) * 0.25; }
    return 10.0 * std::log10 (std::max (sd, 1e-20) / std::max (m, 1e-20));
}; }
/** right over left power of the wet part (dB) */
inline Feature wetBalanceDb() { return [] (const RenderView& r)
{
    const Sig d = wetSig (r); double l = 0, rr = 0; const int a = (int) (0.1 * r.sr);
    for (int i = a; i < d.n(); ++i) { l += (double) d.c[0][(size_t) i] * d.c[0][(size_t) i]; rr += (double) d.c[1][(size_t) i] * d.c[1][(size_t) i]; }
    return 10.0 * std::log10 (std::max (rr, 1e-20) / std::max (l, 1e-20));
}; }
/** spectral centroid of the wet part (Hz) */
inline Feature wetCentroidHz() { return [] (const RenderView& r)
{
    const Sig d = wetSig (r); const int N = 32768, n = d.n(); double num = 0, den = 0;
    std::vector<std::complex<double>> a ((size_t) N);
    for (int i = 0; i < N; ++i) { const int k = n - N + i; const double w = 0.5 - 0.5 * std::cos (6.283185307179586 * i / (N - 1)); a[(size_t) i] = (k >= 0 ? 0.5 * ((double) d.c[0][(size_t) k] + d.c[1][(size_t) k]) : 0.0) * w; }
    fft (a);
    for (int b = 1; b < N / 2; ++b) { const double f = b * r.sr / N, p = std::norm (a[(size_t) b]); num += f * p; den += p; }
    return num / std::max (den, 1e-30);
}; }
/** band power (dB) of the wet part: output minus the latency-aligned input */
inline Feature wetBandDb (double lo, double hi) { return [=] (const RenderView& r) { const Sig d = wetSig (r); return 10.0 * std::log10 (std::max (bandPower (d, r.sr, lo, hi), 1e-30)); }; }
inline Feature plugin (std::function<double (juce::AudioProcessor&)> f) { return [f] (const RenderView& r) { return f (r.proc); }; }
}   // namespace feat

// ---------------------------------------------------------------------------------------------------------------
struct Direction
{
    juce::String id;                 // parameter id
    int dir = +1;                    // +1 the feature rises with the knob, -1 it falls, 0 none (by design), 2 unclear (listed, not tested)
    Feature feature;                 // what is measured
    double tol = 0.25;               // a step may fall against the direction by at most this (feature units, dB)
    Cond applies;                    // states where the declaration holds (null = always)
    std::vector<std::pair<juce::String, float>> force;   // id -> NORMALISED value set before the sweep
    int material = kDrums;
    juce::String note;               // plain words: what the feature is
    juce::String featureName;
    double floorValue = -1.0e9;      // feature values below this are treated as equal to it (an effect below the floor is inaudible, not judged)
};

struct Config
{
    juce::String name;
    std::function<std::unique_ptr<juce::AudioProcessor>()> make;
    int scBus = -1;                                  // input bus index of the sidechain, -1 = none
    bool scBusAlsoDisabledInCorners = true;          // corners/random states are also run with the sidechain bus disabled
    double tailSeconds = 1.0;                        // documented tail (silence after the burst before the -90 dBFS test)
    std::vector<Direction> directions;
    Cond selfSustains;                               // states that sustain by design: tail must stay bounded, decay not required
    std::function<double (const StateView&)> noiseFloorDb;   // states that make noise by design (hiss): the silence-in peak must stay under this instead of -90 dBFS
    std::function<double (const StateView&)> designedGainDb;   // extra output gain the controls give by design (dB), for the peak bound
    std::vector<std::pair<juce::String, float>> latencyForce;  // normalised overrides for the impulse latency probe
    std::vector<juce::String> applyFirst;            // parameter ids written before the others (recall-type parameters)
    std::vector<juce::String> recallParams;          // recall-type parameters (a MOOD that rewrites others): swept on their own they are written LAST and their recall is kept
    std::vector<std::pair<juce::String, float>> automationForce;   // normalised overrides applied to every state of check 3 only (e.g. an auto-gain that follows loudness over seconds and would turn a fast sweep of another knob into a level excursion)
    std::vector<std::vector<std::pair<juce::String, float>>> latencyVariants;   // extra impulse latency probes (normalised overrides each), e.g. a mode that changes the latency
    std::vector<juce::String> skip;                  // excluded parameter ids (documented in notes)
    std::map<juce::String, juce::String> notes;      // per parameter text for the table
    std::function<void (juce::AudioProcessor&)> afterCreate;   // anything a host would do after construction
    std::function<void (Sig&, double sr, int n)> customMaterial;   // a fourth material (e.g. a voiced tone for a harmonizer), joins the rotation; Direction.material = kCustom
    juce::String customName = "custom";
};

struct Options
{
    int sweepStates = 25, tailStates = 9, blockStates = 12, autoStates = 3, stateTests = 200, cornerStates = 500, srStates = 5;
    int onlyChecks = 0;              // bitmask of checks to run (0 = all)
    juce::String onlyParam;          // restrict checks 1,3,4,5 to this parameter id
    double tailOverride = 0.0;     // --tail SECONDS: replaces Config.tailSeconds
    juce::String baseSet;          // --set "id=0.5 id2=1": normalised overrides of state 0 (the defaults) in every check, for isolating a finding
    juce::String probe, probeMat = "silence", probePre; int probeSamples = 48000;   // --probe "id=0.5 id2=1 ..." (normalised values, others default): render 1 s, print peak / rms, then each parameter back to default and to min, one at a time
    juce::String outDir = "build/controlproof", docsFile = "docs/CONTROL_PROOF.md";
    bool quick = false, noDocs = false;
};

// ---------------------------------------------------------------------------------------------------------------
class Host
{
public:
    explicit Host (const Config& c) : cfg (c) {}

    void create (bool scEnabled)
    {
        proc = cfg.make();
        scOn = scEnabled && cfg.scBus >= 0;
        auto layout = proc->getBusesLayout();
        layout.inputBuses.set (0, juce::AudioChannelSet::stereo());
        layout.outputBuses.set (0, juce::AudioChannelSet::stereo());
        if (cfg.scBus >= 0 && cfg.scBus < layout.inputBuses.size())
            layout.inputBuses.set (cfg.scBus, scOn ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::disabled());
        if (! proc->setBusesLayout (layout)) { std::printf ("controlproof: setBusesLayout refused\n"); std::exit (2); }
        if (cfg.afterCreate) cfg.afterCreate (*proc);
        enumerate();
        total = std::max (proc->getTotalNumInputChannels(), proc->getTotalNumOutputChannels());
        store.setSize (total, 16384);
        playHead.sr = 48000.0;
        proc->setPlayHead (&playHead);
        prepared = false;
    }

    void enumerate()
    {
        pars.clear();
        for (auto* p : proc->getParameters())
        {
            Par q; q.p = p;
            if (auto* w = dynamic_cast<juce::AudioProcessorParameterWithID*> (p)) q.id = w->paramID; else q.id = p->getName (64);
            if (std::find (cfg.skip.begin(), cfg.skip.end(), q.id) != cfg.skip.end()) continue;
            q.name = p->getName (64); q.label = p->getLabel();
            q.ranged = dynamic_cast<juce::RangedAudioParameter*> (p);
            q.isBool = p->isBoolean(); q.isChoice = dynamic_cast<juce::AudioParameterChoice*> (p) != nullptr;
            q.discrete = p->isDiscrete() || q.isBool || q.isChoice;
            q.steps = q.isBool ? 2 : (q.discrete ? std::max (2, p->getNumSteps()) : 0);
            q.def = p->getDefaultValue();
            q.minText = p->getText (0.0f, 24) + (q.label.isEmpty() ? "" : " " + q.label);
            q.maxText = p->getText (1.0f, 24) + (q.label.isEmpty() ? "" : " " + q.label);
            pars.push_back (q);
        }
    }

    const std::vector<Par>& params() const { return pars; }
    juce::AudioProcessor& processor() { return *proc; }
    bool scEnabled() const { return scOn; }

    static float snapNorm (const Par& q, float v)
    {
        v = juce::jlimit (0.0f, 1.0f, v);
        if (q.discrete) { const int k = (int) std::lround (v * (float) (q.steps - 1)); return (float) k / (float) (q.steps - 1); }
        return v;
    }

    void setOne (size_t i, float v)
    {
        const auto& q = pars[i];
        const float s = snapNorm (q, v);
        if (q.p->getValue() != s) q.p->setValueNotifyingHost (s);
    }

    /** write the whole state; recall-type parameters first; repeats until read-back matches (listeners that rewrite others) */
    bool applyState (const State& st, int lastIdx = -1)
    {
        std::vector<size_t> order;
        const bool recallLast = lastIdx >= 0 && std::find (cfg.recallParams.begin(), cfg.recallParams.end(), pars[(size_t) lastIdx].id) != cfg.recallParams.end();
        for (auto& id : cfg.applyFirst) for (size_t i = 0; i < pars.size(); ++i) if (pars[i].id == id && ! (recallLast && (int) i == lastIdx)) order.push_back (i);
        for (size_t i = 0; i < pars.size(); ++i) if (std::find (order.begin(), order.end(), i) == order.end() && ! (recallLast && (int) i == lastIdx)) order.push_back (i);
        if (recallLast) order.push_back ((size_t) lastIdx);   // a recall-type parameter swept on its own: its recall (which rewrites others) is the variation under test
        for (int pass = 0; pass < (recallLast ? 1 : 4); ++pass)
        {
            if (recallLast)
            {
                for (size_t i : order) { if ((int) i == lastIdx) setOne (i, snapNorm (pars[i], st[i]) > 0.5f ? 0.0f : 1.0f); setOne (i, st[i]); }   // move it away first, so the recall fires every time
                return true;
            }
            for (size_t i : order) setOne (i, st[i]);
            bool same = true;   // 0.01 normalised = more than any interval snapping; a recall listener rewrites by far more
            for (size_t i = 0; i < pars.size(); ++i)
            {
                const float want = snapNorm (pars[i], st[i]), got = pars[i].p->getValue();
                const float legal = pars[i].ranged ? pars[i].ranged->convertTo0to1 (pars[i].ranged->convertFrom0to1 (want)) : want;   // interval snapping (skewed ranges move by more than 0.01 at the low end)
                if (std::abs (got - want) > 0.01f && std::abs (got - legal) > 1e-4f) same = false;
            }
            if (same) return true;
        }
        return false;
    }

    State current() const { State s; for (auto& q : pars) s.push_back (q.p->getValue()); return s; }
    State defaults() const { State s; for (auto& q : pars) s.push_back (snapNorm (q, q.def)); return s; }

    void prepare (double sr, int block)
    {
        if (prepared) proc->releaseResources();
        sampleRate = sr; prepBlock = block;
        proc->setRateAndBufferSizeDetails (sr, block);
        proc->prepareToPlay (sr, block);
        playHead.sr = sr;
        prepared = true;
    }
    int latency() const { return proc->getLatencySamples(); }

    using AutoFn = std::function<void (int)>;
    /** blockList != null: host block sizes from the list (cycled). grid > 0: the host also cuts at every multiple of grid.
        autoAt (sample) is called at the start of every segment (a segment is a processBlock call). */
    void render (const Sig& in, const Sig* key, int n, int block, const std::vector<int>* blockList, int grid,
                 const AutoFn& autoAt, Sig& out, std::vector<float>* callMs = nullptr)
    {
        out.zeros (n);
        juce::MidiBuffer midi;
        const int mainCh = 2;
        int a = 0, li = 0;
        while (a < n)
        {
            int len = blockList ? (*blockList)[(size_t) (li++ % (int) blockList->size())] : block;
            len = std::min (len, n - a);
            const int b = a + len;
            int s = a;
            while (s < b)
            {
                int e = b;
                if (grid > 0) e = std::min (b, (s / grid + 1) * grid);
                if (autoAt) autoAt (s);
                const int L = e - s;
                for (int c = 0; c < total; ++c)
                {
                    float* w = store.getWritePointer (c);
                    if (c < mainCh) std::copy (in.c[c].begin() + s, in.c[c].begin() + e, w);
                    else if (scOn && key && c - mainCh < 2 && c >= firstScChannel()) std::copy (key->c[c - firstScChannel()].begin() + s, key->c[c - firstScChannel()].begin() + e, w);
                    else std::fill (w, w + L, 0.0f);
                }
                juce::AudioBuffer<float> view (store.getArrayOfWritePointers(), total, L);
                playHead.pos = s;
                const auto t0 = std::chrono::high_resolution_clock::now();
                proc->processBlock (view, midi);
                if (callMs) callMs->push_back ((float) std::chrono::duration<double, std::milli> (std::chrono::high_resolution_clock::now() - t0).count());
                for (int c = 0; c < 2; ++c) std::copy (view.getReadPointer (c), view.getReadPointer (c) + L, out.c[c].begin() + s);
                s = e;
            }
            a = b;
        }
    }

private:
    int firstScChannel() const { return 2; }

    struct PH : juce::AudioPlayHead
    {
        double sr = 48000.0, bpm = 120.0; int64_t pos = 0;
        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo i; i.setBpm (bpm); i.setTimeInSamples (pos); i.setTimeInSeconds ((double) pos / sr);
            i.setPpqPosition ((double) pos / sr * bpm / 60.0); i.setIsPlaying (true); i.setTimeSignature (juce::AudioPlayHead::TimeSignature { 4, 4 });
            return i;
        }
    };

    const Config& cfg;
    std::unique_ptr<juce::AudioProcessor> proc;
    std::vector<Par> pars;
    juce::AudioBuffer<float> store;
    PH playHead;
    int total = 2, prepBlock = 512; double sampleRate = 48000.0; bool scOn = false, prepared = false;
};

// ---------------------------------------------------------------------------------------------------------------
struct Meas { bool finite = true; double peakDb = -240, dcAbs = 0, lastPeakDb = -240, dcIn = 0, dc500 = 0, dcIn500 = 0; };
inline juce::String meanTrail (const Sig& o, double sr)   // channel-max |mean| of the last 9 windows of 100 ms, oldest first: persistent DC or a wander?
{
    juce::String t; const int n = o.n(), w = (int) (0.1 * sr);
    for (int k = 8; k >= 0; --k)
    {
        const int b = n - (k + 1) * w, e = b + w; if (b < 0) continue;
        double m = 0; for (int ch = 0; ch < 2; ++ch) { double s2 = 0; for (int i = b; i < e; ++i) s2 += o.c[ch][(size_t) i]; m = std::max (m, std::abs (s2 / w)); }
        t << juce::String (m, 4) << " ";
    }
    return t.trim();
}
inline double lastMean (const Sig& o, double sr, double sec = 0.1)
{
    double m = 0; const int n = o.n(), w = std::max (1, std::min (n, (int) (sec * sr)));
    for (int ch = 0; ch < 2; ++ch) { double s = 0; for (int i = n - w; i < n; ++i) s += o.c[ch][(size_t) i]; m = std::max (m, std::abs (s / w)); }
    return m;
}
inline Meas measure (const Sig& o, double sr, int from = 0, const Sig* in = nullptr)
{
    Meas m; m.finite = o.finite();
    if (in) { m.dcIn = lastMean (*in, sr); m.dcIn500 = lastMean (*in, sr, 0.5); }
    m.dc500 = lastMean (o, sr, 0.5);
    if (! m.finite) return m;
    double pk = 0, lp = 0; const int n = o.n(), w = (int) (0.1 * sr);
    for (int ch = 0; ch < 2; ++ch)
    {
        double mean = 0;
        for (int i = from; i < n; ++i) pk = std::max (pk, (double) std::abs (o.c[ch][(size_t) i]));
        for (int i = std::max (0, n - w); i < n; ++i) { mean += o.c[ch][(size_t) i]; lp = std::max (lp, (double) std::abs (o.c[ch][(size_t) i])); }
        m.dcAbs = std::max (m.dcAbs, std::abs (mean / std::max (1, std::min (w, n))));
    }
    m.peakDb = toDb (pk); m.lastPeakDb = toDb (lp);
    return m;
}

/** largest sample-to-sample step of (out - centred 33-sample moving average of out) over [from,to) */
inline double hpStep (const Sig& o, int from, int to, int* atSample = nullptr)
{
    double best = 0; const int n = o.n(), H = 16;
    for (int ch = 0; ch < 2; ++ch)
    {
        std::vector<double> ps ((size_t) n + 1, 0.0);
        for (int i = 0; i < n; ++i) ps[(size_t) i + 1] = ps[(size_t) i] + o.c[ch][(size_t) i];
        double prev = 0; bool have = false;
        for (int i = std::max (from, H); i < std::min (to, n - H); ++i)
        {
            const double ma = (ps[(size_t) (i + H + 1)] - ps[(size_t) (i - H)]) / (2 * H + 1);
            const double hp = o.c[ch][(size_t) i] - ma;
            if (have && std::abs (hp - prev) > best) { best = std::abs (hp - prev); if (atSample) *atSample = i; }
            prev = hp; have = true;
        }
    }
    return best;
}

inline double maxAbsDiff (const Sig& a, const Sig& b)
{
    double m = 0;
    for (int ch = 0; ch < 2; ++ch) for (size_t i = 0; i < std::min (a.c[ch].size(), b.c[ch].size()); ++i) m = std::max (m, (double) std::abs (a.c[ch][i] - b.c[ch][i]));
    return m;
}
inline bool bitIdentical (const Sig& a, const Sig& b)
{
    for (int ch = 0; ch < 2; ++ch)
        if (a.c[ch].size() != b.c[ch].size() || (! a.c[ch].empty() && std::memcmp (a.c[ch].data(), b.c[ch].data(), a.c[ch].size() * sizeof (float)) != 0)) return false;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
class Proof
{
public:
    Proof (Config c, Options o) : cfg (std::move (c)), opt (std::move (o)), host (cfg) { if (opt.tailOverride > 0.0) cfg.tailSeconds = opt.tailOverride; }

    int run()
    {
        const auto t0 = std::chrono::steady_clock::now();
        juce::File (opt.outDir).createDirectory();
        tsv = std::fopen (juce::File (opt.outDir).getChildFile (cfg.name + ".tsv").getFullPathName().toRawUTF8(), "w");
        if (tsv) std::fprintf (tsv, "check\tparam\tposition\tstate\tmaterial\tblock\tsr\tmetric\tvalue\tlimit\tstatus\n");
        customMatName() = cfg.customName.toStdString();
        host.create (true);
        pars = host.params();
        res.assign (pars.size(), ParRes {});
        for (size_t i = 0; i < pars.size(); ++i) { res[i].range = pars[i].minText + " .. " + pars[i].maxText; }
        mats[48000] = std::make_shared<Materials> (makeMaterials (48000.0, 1.0, cfg.customMaterial));
        std::printf ("[controlproof] %s: %zu parameters, sidechain bus %s, quick=%d\n", cfg.name.toRawUTF8(), pars.size(), cfg.scBus >= 0 ? "yes (fed with the kick)" : "none", (int) opt.quick);
        for (auto& q : pars) std::printf ("   %-18s %s%s  default %.3f  [%s .. %s]\n", q.id.toRawUTF8(), q.isBool ? "bool" : (q.isChoice ? "choice" : (q.discrete ? "discrete" : "float")), q.discrete ? (" x" + juce::String (q.steps)).toRawUTF8() : "", q.def, q.minText.toRawUTF8(), q.maxText.toRawUTF8());
        std::fflush (stdout);

        if (opt.probe.isNotEmpty()) { runProbe(); return 0; }
        auto want = [&] (int c) { return opt.onlyChecks == 0 || (opt.onlyChecks & (1 << c)); };
        if (want (1)) check1();
        if (want (2)) check2();
        if (want (3)) check3();
        if (want (4)) check4();
        if (want (5)) check5();
        if (want (6)) check6();
        if (want (7)) check7();
        if (want (8)) check8();

        const double secs = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        int fails = 0;
        std::printf ("\n===== %s control proof: summary (%.0f s) =====\n", cfg.name.toRawUTF8(), secs);
        for (auto& l : summary) { std::printf ("%s\n", l.line.toRawUTF8()); if (! l.pass) ++fails; }
        std::printf ("%s: %d check line(s) FAILED\n", cfg.name.toRawUTF8(), fails);
        if (tsv) std::fclose (tsv);
        if (! opt.noDocs && opt.onlyChecks == 0 && opt.onlyParam.isEmpty()) writeDocs (secs);
        std::fflush (stdout);
        return fails == 0 ? 0 : 1;
    }

    struct SumLine { juce::String line; bool pass; };
    const std::vector<SumLine>& results() const { return summary; }

private:
    void runProbe()
    {
        auto& mt = M (48000.0); State st = host.defaults();
        for (auto& tok : juce::StringArray::fromTokens (opt.probe, " ", ""))
        {
            const int eq = tok.indexOfChar ('='); if (eq < 0) continue;
            const int i = StateView { pars, st }.index (tok.substring (0, eq));
            if (i >= 0) st[(size_t) i] = Host::snapNorm (pars[(size_t) i], tok.substring (eq + 1).getFloatValue()); else std::printf ("unknown parameter %s\n", tok.substring (0, eq).toRawUTF8());
        }
        int m = kSilence; for (int k = 0; k < 5; ++k) if (opt.probeMat == matName (k)) m = k;
        Sig out; int pre = -1; for (int k = 0; k < 5; ++k) if (opt.probePre == matName (k)) pre = k;
        auto go = [&] (const State& s2, const juce::String& label)
        {
            bool rb; prep (s2, 48000.0, 512, rb);
            if (pre >= 0) { host.render (mt.m[pre], keyFor (48000.0), 12000, 512, nullptr, 0, nullptr, out); prep (s2, 48000.0, 512, rb); }   // a first render of another material, then prepareToPlay again, as the corner check does
            if (opt.probeMat == "burst") { const Sig b = burstThenSilence (mt, 0.5, opt.probeSamples); host.render (b, keyFor (48000.0), b.n(), 512, nullptr, 0, nullptr, out); }   // 0.5 s of drums, then silence
            else host.render (mt.m[m], keyFor (48000.0), std::min (mt.m[m].n(), opt.probeSamples), 512, nullptr, 0, nullptr, out);
            const auto me = measure (out, 48000.0); std::printf ("  %-28s peak %8.2f dBFS  last100ms peak %8.2f  rms %8.2f dB\n", label.toRawUTF8(), me.peakDb, me.lastPeakDb, toDb (feat::rmsOf (out, 4800, out.n())));
        };
        std::printf ("probe on %s: %s\n", matName (m), stateText (st).toRawUTF8());
        go (st, "as given");
        for (size_t i = 0; i < pars.size(); ++i)
        {
            State a = st; a[i] = Host::snapNorm (pars[i], pars[i].def); if (a[i] != st[i]) go (a, pars[i].id + " -> default");
            State b = st; b[i] = 0.0f; if (b[i] != st[i]) go (b, pars[i].id + " -> min");
        }
    }
    struct ParRes
    {
        juce::String range, dirText = "no direction (by design)", monotone = "-", autoText = "-", note;
        double maxPeakDb = -240; double maxAutoRatioDb = -240, maxAutoDiff = 0;
        bool sweepOk = true, dirOk = true, autoOk = true, tailOk = true, srOk = true, blockOk = true;
        int dirChecked = 0, dirFailed = 0;
        bool autoDiscrete = false;
    };
    Config cfg; Options opt; Host host;
    std::vector<Par> pars; std::vector<ParRes> res; std::vector<SumLine> summary;
    std::map<int, std::shared_ptr<Materials>> mats;
    std::vector<float> callMs;
    FILE* tsv = nullptr;
    int failPrinted = 0;

    // ---- helpers -----------------------------------------------------------------------------------------
    Materials& M (double sr) { const int k = (int) std::lround (sr); if (! mats.count (k)) mats[k] = std::make_shared<Materials> (makeMaterials (sr, 1.0, cfg.customMaterial)); return *mats[k]; }
    int nrot() const { return cfg.customMaterial ? 4 : 3; }
    int rotMat (size_t i) const { return i % (size_t) nrot() < 3 ? (int) (i % (size_t) nrot()) : (int) kCustom; }

    void row (int chk, const juce::String& param, const juce::String& pos, const juce::String& st, const juce::String& mat, int block, double sr,
              const juce::String& metric, double val, const juce::String& lim, const char* status)
    {
        if (tsv) std::fprintf (tsv, "%d\t%s\t%s\t%s\t%s\t%d\t%.0f\t%s\t%.6g\t%s\t%s\n", chk, param.toRawUTF8(), pos.toRawUTF8(), st.toRawUTF8(), mat.toRawUTF8(), block, sr, metric.toRawUTF8(), val, lim.toRawUTF8(), status);
    }
    void failNote (const juce::String& s) { if (failPrinted++ < 25) { std::printf ("   FAIL: %s\n", s.toRawUTF8()); std::fflush (stdout); } }
    void addSummary (const juce::String& line, bool pass) { failPrinted = 0; summary.push_back ({ line, pass }); std::printf ("%s\n", line.toRawUTF8()); std::fflush (stdout); }
    struct Progress
    {
        juce::String label; long total, done = 0; int last = -1; double t0;
        Progress (const juce::String& l, long t) : label (l), total (std::max (1L, t)), t0 ((double) std::clock() / CLOCKS_PER_SEC) {}
        void tick (long k = 1) { done += k; const int pct = (int) (100 * done / total) / 10 * 10; if (pct != last) { last = pct; std::printf ("   [%s] %d%%  (%.0f s cpu)\n", label.toRawUTF8(), pct, (double) std::clock() / CLOCKS_PER_SEC - t0); std::fflush (stdout); } }
    };

    juce::String posName (const Par& q, int k, int count) const
    {
        if (q.isChoice || q.discrete) return juce::String (k);
        return juce::String (k) + "/" + juce::String (count - 1);
    }
    std::vector<float> positions (const Par& q, int& count) const
    {
        std::vector<float> v;
        if (q.discrete && q.steps <= 16) { for (int k = 0; k < q.steps; ++k) v.push_back ((float) k / (float) (q.steps - 1)); }
        else { for (int k = 0; k <= 10; ++k) v.push_back ((float) k / 10.0f); }
        count = (int) v.size(); return v;
    }
    State randomState (Rng& r) const
    {
        State s;
        for (auto& q : pars)
        {
            if (q.discrete) s.push_back ((float) r.below (q.steps) / (float) (q.steps - 1));
            else { const double u = r.uni(); s.push_back (u < 0.1 ? 0.0f : (u < 0.2 ? 1.0f : (float) r.uni())); }
        }
        for (size_t i = 0; i < pars.size(); ++i) s[i] = Host::snapNorm (pars[i], s[i]);
        return s;
    }
    std::vector<State> makeStates (int count, uint64_t seed) const
    {
        State d0 = host.defaults();
        for (auto& tok : juce::StringArray::fromTokens (opt.baseSet, " ", ""))
        {
            const int eq = tok.indexOfChar ('='); if (eq < 0) continue;
            const int i = StateView { pars, d0 }.index (tok.substring (0, eq));
            if (i >= 0) d0[(size_t) i] = Host::snapNorm (pars[(size_t) i], tok.substring (eq + 1).getFloatValue());
        }
        std::vector<State> v; v.push_back (d0);
        Rng r (seed);
        while ((int) v.size() < count) v.push_back (randomState (r));
        return v;
    }
    juce::String stateText (const State& s) const
    {
        juce::String t; for (size_t i = 0; i < pars.size(); ++i) t << pars[i].id << "=" << juce::String (s[i], 3) << " ";
        return t.trimEnd();
    }
    bool skipParam (const Par& q) const { return opt.onlyParam.isNotEmpty() && q.id != opt.onlyParam; }
    double designedGain (const State& st) { StateView v { pars, st }; return cfg.designedGainDb ? cfg.designedGainDb (v) : 0.0; }

    /** bounded?  0 fail, 1 pass, 2 pass by designed gain */
    int peakVerdict (double peakDb, double inPeakDb, const State& st) const
    {
        if (peakDb <= 12.0 && peakDb <= inPeakDb + 24.0) return 1;
        StateView v { pars, st };
        const double dg = cfg.designedGainDb ? cfg.designedGainDb (v) : 0.0;
        return peakDb <= inPeakDb + dg + 6.0 ? 2 : 0;
    }

    /** DC growth: |mean over the last 100 ms| of the output beyond what the input's own mean explains (times the designed gain + 6 dB).
        The strict |mean| < 0.01 is also reported (dcAbs); raw material means can exceed 0.01 on their own. */
    double dcExcess (const Meas& me, const State& st)
    {
        const double g = std::pow (10.0, (designedGain (st) + 6.0) / 20.0);
        const double e100 = std::max (0.0, me.dcAbs - me.dcIn * g), e500 = std::max (0.0, me.dc500 - me.dcIn500 * g);
        // a 100 ms mean above 0.01 only counts when the 500 ms mean (signed, so a wander cancels) still holds more than 0.005: that is an offset, not low-frequency noise
        return e500 > 0.005 ? e100 : std::min (e100, 0.0099);
    }

    void prep (const State& st, double sr, int block, bool& readbackOk, int sweptIdx = -1)
    {
        readbackOk = host.applyState (st, sweptIdx);
        host.prepare (sr, block);
    }

    const Sig* keyFor (double sr) { return cfg.scBus >= 0 && host.scEnabled() ? &M (sr).kick : nullptr; }

    // ===========================================================================================================
    // check 1: sweep + tails
    void check1()
    {
        std::printf ("\n--- check 1: sweep (%d positions x %d states, 1.0 s per point) + tails (%.1f s) ---\n", 11, opt.sweepStates, cfg.tailSeconds);
        auto states = makeStates (opt.sweepStates, 1001);
        auto& mt = M (48000.0);
        long nPoints = 0, bad = 0, excused = 0, readback = 0, tailPoints = 0, tailBad = 0, sustainSkipped = 0, dcStrict = 0;
        double maxPeak = -240, maxDc = 0; juce::String maxPeakAt, maxDcAt;
        long total = 0;
        for (auto& q : pars) { if (skipParam (q)) continue; int cnt; positions (q, cnt); total += (long) cnt * (3 + (opt.sweepStates - 1)) + (long) cnt * opt.tailStates; }
        Progress pg ("check 1", total);
        callMs.clear();
        Sig out;
        struct Worst { size_t pi = 0; int k = 0; size_t s = 0; int m = 0; size_t call = 0; float ms = 0; } worstCall;
        for (size_t pi = 0; pi < pars.size(); ++pi)
        {
            const auto& q = pars[pi]; if (skipParam (q)) continue;
            int cnt; auto pos = positions (q, cnt);
            for (int k = 0; k < cnt; ++k)
                for (size_t s = 0; s < states.size(); ++s)
                {
                    State st = states[s]; st[pi] = pos[(size_t) k];
                    const int nm = s == 0 ? nrot() : 1;
                    for (int mi = 0; mi < nm; ++mi)
                    {
                        const int m = s == 0 ? rotMat ((size_t) mi) : rotMat (s + (size_t) k + pi);
                        bool rb; prep (st, 48000.0, 512, rb, (int) pi); if (! rb) ++readback;
                        const size_t before = callMs.size();
                        host.render (mt.m[m], keyFor (48000.0), mt.m[m].n(), 512, nullptr, 0, nullptr, out, &callMs);
                        for (size_t ci = before + 1; ci < callMs.size(); ++ci) if (callMs[ci] > worstCall.ms) worstCall = { pi, k, s, m, ci - before, callMs[ci] };
                        auto me = measure (out, 48000.0, 0, &mt.m[m]);
                        const double inPk = toDb (mt.m[m].peak());
                        const int v = me.finite ? peakVerdict (me.peakDb, inPk, st) : 0;
                        const double dcx = me.finite ? dcExcess (me, st) : 1.0;
                        const bool dcOk = dcx < 0.01;
                        if (me.dcAbs >= 0.01) ++dcStrict;
                        const bool ok = me.finite && v > 0 && dcOk;
                        ++nPoints; if (v == 2) ++excused;
                        if (me.finite && me.peakDb > maxPeak) { maxPeak = me.peakDb; maxPeakAt = q.id + " pos " + posName (q, k, cnt) + " state " + juce::String ((int) s) + " " + matName (m); }
                        if (me.finite && me.dcAbs > maxDc) { maxDc = me.dcAbs; maxDcAt = q.id + " pos " + posName (q, k, cnt) + " state " + juce::String ((int) s) + " " + matName (m); }
                        res[pi].maxPeakDb = std::max (res[pi].maxPeakDb, me.finite ? me.peakDb : 999.0);
                        row (1, q.id, posName (q, k, cnt), juce::String ((int) s), matName (m), 512, 48000, "peak_dBFS", me.peakDb, "12|in+24", ok ? (v == 2 ? "PASS_EXCUSED" : "PASS") : "FAIL");
                        row (1, q.id, posName (q, k, cnt), juce::String ((int) s), matName (m), 512, 48000, "dc_abs_mean_last100ms(strict_info)", me.dcAbs, "0.01", me.dcAbs < 0.01 ? "PASS" : "INFO_EXCEEDS");
                        row (1, q.id, posName (q, k, cnt), juce::String ((int) s), matName (m), 512, 48000, "dc_excess_over_input_mean", dcx, "0.01", dcOk ? "PASS" : "FAIL");
                        if (! ok) { ++bad; res[pi].sweepOk = false; failNote ("check1 " + q.id + " pos " + posName (q, k, cnt) + " state " + juce::String ((int) s) + " " + matName (m) + (me.finite ? "" : " NON-FINITE") + " peak " + juce::String (me.peakDb, 2) + " dBFS (in " + juce::String (inPk, 1) + ") dc " + juce::String (me.dcAbs, 5) + " [" + stateText (st) + "]"); }
                        pg.tick();
                    }
                    if (s != 0) pg.tick (0);
                }
            // tails
            const int tn = (int) std::lround (48000.0 * (0.5 + cfg.tailSeconds));
            Sig burst = burstThenSilence (mt, 0.5, tn);
            Sig sil; sil.zeros ((int) (0.5 * 48000));
            for (int k = 0; k < cnt; ++k)
                for (int s = 0; s < opt.tailStates && s < (int) states.size(); ++s)
                {
                    State st = states[(size_t) s]; st[pi] = pos[(size_t) k];
                    bool rb; prep (st, 48000.0, 512, rb, (int) pi);
                    if (! rb || (int) pi >= 0) st = host.current();   // what the processor really holds now (a recall such as MOOD rewrites other parameters)
                    StateView sv { pars, st };
                    host.render (burst, keyFor (48000.0), tn, 512, nullptr, 0, nullptr, out);
                    auto me = measure (out, 48000.0);
                    const bool sustain = cfg.selfSustains && cfg.selfSustains (sv);
                    // post-burst peak: from 0.6 s on
                    double post = 0; for (int ch = 0; ch < 2; ++ch) for (int i = (int) (0.6 * 48000); i < tn; ++i) post = std::max (post, (double) std::abs (out.c[ch][(size_t) i]));
                    const double postDb = toDb (post);
                    const int v = me.finite ? peakVerdict (me.peakDb, toDb (burst.peak()), st) : 0;
                    const double ceil = cfg.noiseFloorDb ? cfg.noiseFloorDb (sv) : -90.0;
                    bool ok = me.finite && v > 0 && postDb <= (sustain ? 12.0 : 0.0) && (sustain || me.lastPeakDb < ceil);   // a sustaining state is held to the absolute +12 dBFS bound, a decaying one to 0 dBFS
                    if (sustain) ++sustainSkipped;
                    // silence in -> silence out
                    prep (st, 48000.0, 512, rb, (int) pi);
                    Sig o2; host.render (sil, keyFor (48000.0), sil.n(), 512, nullptr, 0, nullptr, o2);
                    const double silDb = o2.finite() ? toDb (o2.peak()) : 999.0;
                    const bool silOk = silDb < ceil || (sustain && silDb <= 0.0);
                    ++tailPoints;
                    row (1, q.id, posName (q, k, cnt), juce::String (s), "burst+silence", 512, 48000, "tail_last100ms_peak_dBFS", me.lastPeakDb, juce::String (ceil, 0), ok ? (sustain ? "PASS_SUSTAINS" : "PASS") : "FAIL");
                    row (1, q.id, posName (q, k, cnt), juce::String (s), "burst+silence", 512, 48000, "after_burst_peak_dBFS", postDb, sustain ? "12 (sustains)" : "0", postDb <= (sustain ? 12.0 : 0.0) ? "PASS" : "FAIL");
                    row (1, q.id, posName (q, k, cnt), juce::String (s), "silence", 512, 48000, "silence_in_peak_dBFS", silDb, juce::String (ceil, 0), silOk ? "PASS" : "FAIL");
                    if (! ok || ! silOk) { ++tailBad; res[pi].tailOk = false; failNote ("check1 tail " + q.id + " pos " + posName (q, k, cnt) + " state " + juce::String (s) + " last100ms " + juce::String (me.lastPeakDb, 1) + " dBFS, after-burst peak " + juce::String (postDb, 1) + ", silence-in " + juce::String (silDb, 1) + " [" + stateText (st) + "]"); }
                    pg.tick();
                }
        }
        const bool pass = bad == 0 && tailBad == 0 && readback == 0;
        addSummary ("[check 1 sweep] " + juce::String (pass ? "PASS" : "FAIL") + ": " + juce::String ((int) nPoints) + " material points (finite, peak, DC) + " + juce::String ((int) tailPoints) + " tail points; failures " + juce::String ((int) bad) + " + tail " + juce::String ((int) tailBad) + "; max peak " + juce::String (maxPeak, 2) + " dBFS (" + maxPeakAt + "); max |mean| last 100 ms " + juce::String (maxDc, 5) + " (" + maxDcAt + "), strict |mean| >= 0.01 at " + juce::String ((int) dcStrict) + " point(s) (judged as excess over the input mean x designed gain, see header); peak bound met only through designed gain at " + juce::String ((int) excused) + " point(s); sustain-by-design tail points " + juce::String ((int) sustainSkipped) + "; read-back mismatches " + juce::String ((int) readback), pass);
        cpuPass = true;
        if (! callMs.empty())
        {
            const size_t per = (size_t) std::ceil (48000.0 / 512.0);   // calls per 1 s render; the first call of each render follows prepareToPlay (cold)
            std::vector<float> v, first;
            for (size_t i = 0; i < callMs.size(); ++i) (i % per == 0 ? first : v).push_back (callMs[i]);
            std::sort (v.begin(), v.end());
            const double rt = 512.0 / 48000.0 * 1000.0;
            cpuWorst = v.back() / rt * 100.0; cpuP999 = v[(size_t) ((double) v.size() * 0.999)] / rt * 100.0; cpuMed = v[v.size() / 2] / rt * 100.0; cpuCalls = (long) v.size();
            cpuFirstWorst = first.empty() ? 0.0 : *std::max_element (first.begin(), first.end()) / rt * 100.0;
            cpuHave = true;
            // is the worst call the product or the scheduler?  replay that exact render 6 times and take the fastest time of that call
            {
                State st = states[worstCall.s]; int cnt; auto pos = positions (pars[worstCall.pi], cnt); st[worstCall.pi] = pos[(size_t) worstCall.k];
                double best = 1e9; std::vector<float> cm;
                for (int rep = 0; rep < 6; ++rep)
                {
                    bool rb; prep (st, 48000.0, 512, rb, (int) worstCall.pi); cm.clear();
                    host.render (mt.m[worstCall.m], keyFor (48000.0), mt.m[worstCall.m].n(), 512, nullptr, 0, nullptr, out, &cm);
                    best = std::min (best, (double) cm[worstCall.call]);
                }
                cpuReplay = best / rt * 100.0;
                cpuWorstWhat = pars[worstCall.pi].id + " pos " + posName (pars[worstCall.pi], worstCall.k, cnt) + " state " + juce::String ((int) worstCall.s) + " " + matName (worstCall.m) + " call " + juce::String ((int) worstCall.call);
            }
        }
    }

    // ===========================================================================================================
    // check 2: direction
    void check2()
    {
        std::printf ("\n--- check 2: declared directions ---\n");
        auto& mt = M (48000.0);
        auto states = makeStates (6, 2002);
        int declared = 0, failedDecl = 0, nodir = 0, unclear = 0, untested = 0;
        long total = 0; for (auto& d : cfg.directions) if (d.dir == 1 || d.dir == -1) total += 6 * 11;
        Progress pg ("check 2", total);
        Sig out;
        auto forced = [&] (const Direction& d, State st)
        {
            for (auto& f : d.force) { const int i = StateView { pars, st }.index (f.first); if (i >= 0) st[(size_t) i] = Host::snapNorm (pars[(size_t) i], f.second); }
            return st;
        };
        for (size_t pi = 0; pi < pars.size(); ++pi)
        {
            const auto& q = pars[pi];
            if (skipParam (q)) continue;
            std::vector<const Direction*> ds; for (auto& x : cfg.directions) if (x.id == q.id) ds.push_back (&x);
            if (ds.empty()) { ++nodir; res[pi].dirText = "no direction (by design)"; continue; }
            juce::String texts, monos; bool first = true;
            for (auto* d : ds)
            {
                const juce::String sep = first ? "" : " ; "; first = false;
                if (d->dir == 0) { ++nodir; texts << sep << "no direction (by design)" << (d->note.isNotEmpty() ? " - " + d->note : juce::String()); continue; }
                if (d->dir == 2)
                {
                    ++unclear; texts << sep << "unclear (not judged)" << (d->note.isNotEmpty() ? " - " + d->note : juce::String());
                    if (d->feature)   // still measured, default state only, for the record
                    {
                        int cnt2; auto pos2 = positions (q, cnt2); State st = forced (*d, states[0]);
                        juce::String fs; double lo = 1e9, hi = -1e9; Sig refSig;
                        for (int k = 0; k < cnt2; ++k)
                        {
                            State sk = st; sk[pi] = pos2[(size_t) k]; bool rb; prep (sk, 48000.0, 512, rb);
                            host.render (mt.m[d->material], keyFor (48000.0), mt.m[d->material].n(), 512, nullptr, 0, nullptr, out);
                            if (k == 0) refSig = out;
                            StateView sv { pars, sk };
                            RenderView rv { mt.m[d->material], out, keyFor (48000.0), 48000.0, host.latency(), host.processor(), sv, &refSig };
                            const double f = d->feature (rv); lo = std::min (lo, f); hi = std::max (hi, f); fs << juce::String (f, 2) << ",";
                        }
                        row (2, q.id, "values", "0", matName (d->material), 512, 48000, "feature_by_position(unclear,info)", hi - lo, fs, "INFO");
                        monos << sep << "measured only: " << (d->featureName.isEmpty() ? juce::String ("feature") : d->featureName) << " spans " << juce::String (lo, 1) << " .. " << juce::String (hi, 1);
                    }
                    continue;
                }
                ++declared;
                const juce::String label = juce::String (d->dir > 0 ? "up " : "down ") + (d->featureName.isEmpty() ? juce::String ("feature") : d->featureName) + " (tol " + juce::String (d->tol, 2) + (d->note.isNotEmpty() ? "; " + d->note : juce::String()) + ")";
                texts << sep << label;
                int cnt; auto pos = positions (q, cnt);
                int okStates = 0, usedStates = 0; double defaultSpan = 0;
                for (size_t s = 0; s < states.size(); ++s)
                {
                    State st = forced (*d, states[s]);
                    if (d->applies && ! d->applies (StateView { pars, st })) continue;
                    std::vector<double> f; Sig refSig;
                    for (int k = 0; k < cnt; ++k)
                    {
                        State sk = st; sk[pi] = pos[(size_t) k];
                        bool rb; prep (sk, 48000.0, 512, rb);
                        host.render (mt.m[d->material], keyFor (48000.0), mt.m[d->material].n(), 512, nullptr, 0, nullptr, out);
                        if (k == 0) refSig = out;
                        StateView sv { pars, sk };
                        RenderView rv { mt.m[d->material], out, keyFor (48000.0), 48000.0, host.latency(), host.processor(), sv, &refSig };
                        f.push_back (d->feature (rv)); pg.tick();
                    }
                    ++usedStates;
                    double worstViol = 0;
                    std::vector<double> g = f; for (double& x : g) x = std::max (x, d->floorValue);
                    for (int k = 0; k + 1 < cnt; ++k) worstViol = std::max (worstViol, (double) d->dir * (g[(size_t) k] - g[(size_t) k + 1]));
                    const double span = d->dir * (g.back() - g.front());
                    bool ok = worstViol <= d->tol;
                    if (s == 0) { defaultSpan = span; if (span < d->tol) ok = false; }
                    if (ok) ++okStates; else ++res[pi].dirFailed;
                    ++res[pi].dirChecked;
                    juce::String fs; for (double x : f) fs << juce::String (x, 2) << ",";
                    row (2, q.id, "11", juce::String ((int) s), matName (d->material), 512, 48000, "monotonic_worst_violation", worstViol, juce::String (d->tol, 3), ok ? "PASS" : "FAIL");
                    row (2, q.id, "11", juce::String ((int) s), matName (d->material), 512, 48000, "span_in_direction", span, juce::String (d->tol, 3), (s == 0 && span < d->tol) ? "FAIL" : "PASS");
                    row (2, q.id, "values", juce::String ((int) s), matName (d->material), 512, 48000, "feature_by_position", 0, fs, "INFO");
                    if (! ok) { res[pi].dirOk = false; ++failedDecl; failNote ("check2 " + q.id + " state " + juce::String ((int) s) + " worst violation " + juce::String (worstViol, 3) + " (tol " + juce::String (d->tol, 2) + "), span " + juce::String (span, 2) + ", values " + fs + " [" + stateText (st) + "]"); }
                }
                if (usedStates == 0) { res[pi].dirOk = false; ++untested; ++failedDecl; failNote ("check2 " + q.id + ": the declaration applied to none of the 6 states (add a force): not tested"); }
                monos << sep << okStates << "/" << usedStates << " states (default-state span " << juce::String (defaultSpan, 1) << ")";
            }
            res[pi].dirText = texts; if (monos.isNotEmpty()) res[pi].monotone = monos;
        }
        const bool pass = failedDecl == 0;
        addSummary ("[check 2 direction] " + juce::String (pass ? "PASS" : "FAIL") + ": " + juce::String (declared) + " declared directions, failed (parameter x state) " + juce::String (failedDecl) + " (declarations that tested no state: " + juce::String (untested) + "); no direction by design " + juce::String (nodir) + "; unclear " + juce::String (unclear), pass);
    }

    // ===========================================================================================================
    // check 3: automation
    struct RampSpec { int start, len; };
    Host::AutoFn rampFn (Host& H, size_t pi, RampSpec rs, int grid = 0)
    {
        auto last = std::make_shared<float> (-1.0f);
        return [this, &H, pi, rs, last, grid] (int s)
        {
            if (grid > 0) s = (s / grid) * grid;   // sample-accurate automation: the value is defined on the grid, whatever the host block
            float t = (float) (s - rs.start) / (float) rs.len;
            float v = (t <= 0.0f || t >= 1.0f) ? 0.0f : 1.0f - std::abs (2.0f * t - 1.0f);
            v = Host::snapNorm (pars[pi], v);
            if (v != *last) { H.setOne (pi, v); *last = v; }
        };
    }

    void check3()
    {
        std::printf ("\n--- check 3: automation (ramp min->max->min over 300 ms, host blocks 512 and 64 vs block 1) ---\n");
        auto states = makeStates (opt.autoStates, 3003);
        for (auto& st : states) for (auto& f : cfg.automationForce) { const int i = StateView { pars, st }.index (f.first); if (i >= 0) st[(size_t) i] = Host::snapNorm (pars[(size_t) i], f.second); }
        auto& mt = M (48000.0);
        const int n = (int) (0.7 * 48000); const RampSpec rs { (int) (0.1 * 48000), (int) (0.3 * 48000) };
        long total = 0; for (auto& q : pars) if (! skipParam (q)) total += (long) opt.autoStates * 2;
        Progress pg ("check 3", total);
        long bad = 0, pts = 0, discreteInfo = 0; double worstRatio = -240, worstDiff = 0; juce::String worstAt;
        Sig ref, o, smin, smax, o64;
        Host F (cfg);   // fresh instance for every render (a re-prepared instance can carry history, see check 6 info)
        auto prepF = [&] (const State& st, double sr, int block, int swept) { F.create (true); F.applyState (st, swept); F.prepare (sr, block); };
        for (size_t pi = 0; pi < pars.size(); ++pi)
        {
            const auto& q = pars[pi]; if (skipParam (q)) continue;
            res[pi].autoDiscrete = q.discrete;
            for (size_t s = 0; s < states.size(); ++s)
                for (int m : (cfg.customMaterial ? std::vector<int> { (int) kDrums, (int) kChord, (int) kCustom } : std::vector<int> { (int) kDrums, (int) kChord }))
                {
                    State st = states[s]; bool rb;
                    auto run = [&] (int block, const Host::AutoFn& fn, Sig& dst)
                    {
                        State a = st; a[pi] = 0.0f; prepF (a, 48000.0, block, (int) pi);
                        F.render (mt.m[m], (F.scEnabled() ? &mt.kick : nullptr), n, block, nullptr, 0, fn, dst);
                    };
                    auto stat = [&] (float v, Sig& dst) { State a = st; a[pi] = v; prepF (a, 48000.0, 512, (int) pi); F.render (mt.m[m], (F.scEnabled() ? &mt.kick : nullptr), n, 512, nullptr, 0, nullptr, dst); };
                    stat (0.0f, smin); stat (1.0f, smax);
                    const double refStep = std::max (hpStep (smin, 0, n), hpStep (smax, 0, n));
                    // a second reference, only for judging a knob whose effect is not monotone (a saturating gain stage can be busier in the middle than at both ends):
                    // the largest step of the static renders at 5 positions (0, 1/4, 1/2, 3/4, 1)
                    double envStep = refStep; { for (float v : { 0.25f, 0.5f, 0.75f }) { stat (v, o); envStep = std::max (envStep, hpStep (o, 0, n)); } }
                    run (1, rampFn (F, pi, rs), ref);
                    for (int block : { 512, 64, 1 })
                    {
                        Sig& dst = block == 1 ? ref : o;
                        if (block != 1) run (block, rampFn (F, pi, rs), dst);
                        const double diff = block == 1 ? 0.0 : maxAbsDiff (dst, ref);
                        int stepAt = 0; const double step = hpStep (dst, 0, n, &stepAt);
                        const double ratio = toDb (std::max (step, 1e-9)) - toDb (std::max (refStep, 1e-4));
                        const double ratioEnv = toDb (std::max (step, 1e-9)) - toDb (std::max (envStep, 1e-4));
                        const bool fin = dst.finite();
                        const bool ok = fin && (ratio <= 6.0 || ratioEnv <= 6.0 || step < 1e-4);   // literal rule (min / max) or, for a non-monotone knob, the 5-position envelope
                        const bool disc = q.discrete;
                        ++pts; if (disc) ++discreteInfo;
                        worstDiff = std::max (worstDiff, diff);
                        res[pi].maxAutoDiff = std::max (res[pi].maxAutoDiff, diff);
                        if (! disc && ratio > worstRatio) { worstRatio = ratio; worstAt = q.id + " state " + juce::String ((int) s) + " " + matName (m) + " block " + juce::String (block); }
                        res[pi].maxAutoRatioDb = std::max (res[pi].maxAutoRatioDb, ratio);
                        row (3, q.id, "ramp", juce::String ((int) s), matName (m), block, 48000, "max_abs_diff_vs_block1", diff, "info", "INFO");
                        row (3, q.id, "ramp", juce::String ((int) s), matName (m), block, 48000, "step_vs_static_dB", ratio, disc ? "info(discrete)" : "6", disc ? "INFO" : (ok ? (ratio <= 6.0 ? "PASS" : "PASS_ENVELOPE") : "FAIL"));
                        row (3, q.id, "ramp", juce::String ((int) s), matName (m), block, 48000, "step_vs_static_envelope_dB", ratioEnv, "6", "INFO");
                        if (! disc && ! ok) { ++bad; res[pi].autoOk = false; failNote ("check3 " + q.id + " state " + juce::String ((int) s) + " " + matName (m) + " block " + juce::String (block) + ": step " + juce::String (step, 6) + " vs static " + juce::String (refStep, 6) + " = +" + juce::String (ratio, 1) + " dB (vs the 5-position envelope " + juce::String (ratioEnv, 1) + " dB) at " + juce::String ((double) stepAt / 48000.0, 4) + " s (ramp value there " + juce::String (1.0 - std::abs (2.0 * ((double) (stepAt - rs.start) / rs.len) - 1.0), 3) + ")" + (fin ? "" : " NON-FINITE")); }
                    }
                    pg.tick();
                }
        }
        const bool pass = bad == 0;
        addSummary ("[check 3 automation] " + juce::String (pass ? "PASS" : "FAIL") + ": " + juce::String ((int) pts) + " ramp renders (blocks 1/64/512), continuous failures " + juce::String ((int) bad) + "; worst continuous step " + juce::String (worstRatio, 1) + " dB vs static min/max (" + worstAt + "); largest |block 512 or 64 - block 1| " + juce::String (worstDiff, 6) + " (" + juce::String (toDb (worstDiff), 1) + " dBFS); discrete-parameter renders (reported, not judged) " + juce::String ((int) discreteInfo), pass);
    }

    // ===========================================================================================================
    // check 4: block-size independence
    void check4()
    {
        std::printf ("\n--- check 4: block-size independence ---\n");
        auto& mt = M (48000.0);
        std::vector<int> rnd; { Rng r (77); for (int i = 0; i < 4000; ++i) rnd.push_back (1 + r.below (2048)); }
        struct Cfg { int block; const std::vector<int>* list; const char* name; };
        const std::vector<Cfg> cfgs = { { 1, nullptr, "1" }, { 64, nullptr, "64" }, { 333, nullptr, "333" }, { 4096, nullptr, "4096" }, { 512, &rnd, "random" } };
        auto states = makeStates (opt.blockStates, 4004);
        const int n = (int) (0.5 * 48000);
        long total = (long) states.size() + 0; for (auto& q : pars) if (! skipParam (q)) total += 2;
        Progress pg ("check 4", total);
        double worstA = 0, worstB = 0; long bad = 0; bool allExactA = true, allExactB = true; juce::String worstAtA, worstAtB; std::map<juce::String, int> failBlocks; std::map<juce::String, int> failParams;
        Sig ref, o;
        Host F (cfg);   // every render of this check runs on a FRESH instance (a re-prepared instance can carry history, see check 6 info)
        auto prepF = [&] (const State& st, double sr, int block, int swept = -1) { F.create (true); F.applyState (st, swept); F.prepare (sr, block); };
        // A: static random states
        if (opt.onlyParam.isEmpty())
            for (size_t s = 0; s < states.size(); ++s)
            {
                const int m = rotMat (s);
                prepF (states[s], 48000.0, 512);
                F.render (mt.m[m], (F.scEnabled() ? &mt.kick : nullptr), n, 512, nullptr, 0, nullptr, ref);
                for (auto& c : cfgs)
                {
                    prepF (states[s], 48000.0, c.list ? 2048 : c.block);
                    F.render (mt.m[m], (F.scEnabled() ? &mt.kick : nullptr), n, c.block, c.list, 0, nullptr, o);
                    const double d = maxAbsDiff (o, ref); const bool exact = bitIdentical (o, ref); const bool ok = o.finite() && d <= 1e-6;
                    worstA = std::max (worstA, d); if (! exact) allExactA = false;
                    if (d >= worstA) worstAtA = "state " + juce::String ((int) s) + " " + matName (m) + " block " + c.name;
                    row (4, "static_state", "-", juce::String ((int) s), matName (m), c.block, 48000, juce::String ("max_abs_diff_vs_512_") + c.name, d, "1e-6", ok ? (exact ? "PASS_BITEXACT" : "PASS") : "FAIL");
                    if (! ok) { ++failBlocks[c.name]; }
                    if (! ok) { ++bad; failNote ("check4A state " + juce::String ((int) s) + " block " + c.name + " diff " + juce::String (d, 9) + " [" + stateText (states[s]) + "]"); }
                }
                pg.tick();
            }
        // B: per-parameter ramp, sample-accurate automation on a 64-sample grid
        const RampSpec rs { (int) (0.05 * 48000), (int) (0.3 * 48000) };
        for (size_t pi = 0; pi < pars.size(); ++pi)
        {
            const auto& q = pars[pi]; if (skipParam (q)) continue;
            for (size_t si = 0; si < 2; ++si)
            {
                State st = states[si == 0 ? 0 : std::min<size_t> (1, states.size() - 1)]; st[pi] = 0.0f;
                prepF (st, 48000.0, 512, (int) pi);
                F.render (mt.m[kDrums], (F.scEnabled() ? &mt.kick : nullptr), n, 512, nullptr, 64, rampFn (F, pi, rs, 64), ref);
                double worst = 0; bool exactAll = true;
                for (auto& c : cfgs)
                {
                    prepF (st, 48000.0, c.list ? 2048 : c.block, (int) pi);
                    F.render (mt.m[kDrums], (F.scEnabled() ? &mt.kick : nullptr), n, c.block, c.list, 64, rampFn (F, pi, rs, 64), o);
                    const double d = maxAbsDiff (o, ref); const bool exact = bitIdentical (o, ref); const bool ok = o.finite() && d <= 1e-6;
                    worst = std::max (worst, d); if (! exact) exactAll = false;
                    row (4, q.id, "ramp64grid", juce::String ((int) si), "drums", c.block, 48000, juce::String ("max_abs_diff_vs_512_") + c.name, d, "1e-6", ok ? (exact ? "PASS_BITEXACT" : "PASS") : "FAIL");
                    if (! ok) { ++failBlocks[c.name]; ++failParams[q.id]; }
                    if (! ok) { ++bad; res[pi].blockOk = false; failNote ("check4B " + q.id + " state " + juce::String ((int) si) + " block " + c.name + " diff " + juce::String (d, 9)); }
                }
                if (worst > worstB) { worstB = worst; worstAtB = q.id + " state " + juce::String ((int) si); }
                if (! exactAll) allExactB = false;
                pg.tick();
            }
        }
        const bool pass = bad == 0;
        addSummary ("[check 4 blocks] " + juce::String (pass ? "PASS" : "FAIL") + ": blocks 1/64/333/512/4096/random vs 512 (host also cut at a 64-sample grid for automation); static states: max diff " + juce::String (worstA, 9) + (allExactA ? " (bit-exact)" : " (within 1e-6, not bit-exact)") + "; per-parameter ramps: max diff " + juce::String (worstB, 9) + (allExactB ? " (bit-exact)" : " (within 1e-6, not bit-exact)") + "; failures " + juce::String ((int) bad) + [&] { juce::String t; for (auto& kv : failBlocks) t << " block " << kv.first << ": " << kv.second << " failing renders;"; for (auto& kv : failParams) t << " " << kv.first << "(" << kv.second << ")"; return t.isEmpty() ? juce::String() : " [" + t + "]"; }(), pass);
    }

    // ===========================================================================================================
    // check 5: sample rates + latency
    void check5()
    {
        std::printf ("\n--- check 5: 44.1 / 48 / 96 kHz, 3 positions per parameter, latency ---\n");
        auto states = makeStates (opt.srStates, 5005);
        long bad = 0, pts = 0; double maxPeak = -240; juce::String latLine;
        bool latOk = true;
        Sig out;
        long total = 0; for (auto& q : pars) if (! skipParam (q)) total += 3 * 3 * (long) states.size() + 3;
        Progress pg ("check 5", total);
        for (double sr : { 44100.0, 48000.0, 96000.0 })
        {
            auto& mt = M (sr);
            // latency by impulse (default state, then each variant)
            std::vector<std::vector<std::pair<juce::String, float>>> variants = cfg.latencyVariants; variants.insert (variants.begin(), cfg.latencyForce);
            for (size_t vi = 0; vi < variants.size(); ++vi)
            {
                State st = host.defaults();
                for (auto& f : variants[vi]) { const int i = StateView { pars, st }.index (f.first); if (i >= 0) st[(size_t) i] = Host::snapNorm (pars[(size_t) i], f.second); }
                bool rb; prep (st, sr, 512, rb);
                Sig imp; const int n = (int) (0.5 * sr); imp.zeros (n); imp.c[0][2000] = imp.c[1][2000] = 0.5f;
                host.render (imp, keyFor (sr), n, 512, nullptr, 0, nullptr, out);
                int arg = 0; float best = 0; for (int i = 0; i < n; ++i) if (std::abs (out.c[0][(size_t) i]) > best) { best = std::abs (out.c[0][(size_t) i]); arg = i; }
                const int measured = arg - 2000, reported = host.latency();
                const bool ok = measured == reported;
                latLine << juce::String ((int) sr) << " Hz" << (vi ? " (variant " + juce::String ((int) vi) + ")" : juce::String()) << ": reported " << reported << " measured " << measured << (ok ? "" : " MISMATCH") << "; ";
                row (5, "latency", "impulse", juce::String ((int) vi), "impulse", 512, sr, "reported_vs_measured", (double) (measured - reported), "0", ok ? "PASS" : "FAIL");
                if (! ok) { latOk = false; ++bad; failNote ("check5 latency at " + juce::String (sr) + " Hz: reported " + juce::String (reported) + " measured " + juce::String (measured)); }
            }
            for (size_t pi = 0; pi < pars.size(); ++pi)
            {
                const auto& q = pars[pi]; if (skipParam (q)) continue;
                int cnt; auto all = positions (q, cnt);
                const float pos3[3] = { all.front(), all[all.size() / 2], all.back() };
                for (int k = 0; k < 3; ++k)
                    for (size_t s = 0; s < states.size(); ++s)
                    {
                        State st = states[s]; st[pi] = pos3[k];
                        const int m = rotMat (s + (size_t) k);
                        bool rb; prep (st, sr, 512, rb, (int) pi);
                        host.render (mt.m[m], keyFor (sr), mt.m[m].n(), 512, nullptr, 0, nullptr, out);
                        auto me = measure (out, sr, 0, &mt.m[m]);
                        const int v = me.finite ? peakVerdict (me.peakDb, toDb (mt.m[m].peak()), st) : 0;
                        const bool ok = me.finite && v > 0 && dcExcess (me, st) < 0.01;
                        ++pts; if (me.finite) maxPeak = std::max (maxPeak, me.peakDb);
                        row (5, q.id, juce::String (k), juce::String ((int) s), matName (m), 512, sr, "peak_dBFS", me.peakDb, "12|in+24", ok ? "PASS" : "FAIL");
                        if (! ok) { ++bad; res[pi].srOk = false; failNote ("check5 " + q.id + " sr " + juce::String (sr) + " pos " + juce::String (k) + " state " + juce::String ((int) s) + " peak " + juce::String (me.peakDb, 2) + " dc " + juce::String (me.dcAbs, 5) + " (input mean " + juce::String (me.dcIn, 5) + ", designed gain " + juce::String (designedGain (st), 1) + " dB; output means of the last 9 x 100 ms: " + meanTrail (out, sr) + ") [" + stateText (st) + "]"); }
                        pg.tick();
                    }
                // tail at default state, position 3
                pg.tick (0);
            }
        }
        const bool pass = bad == 0;
        addSummary ("[check 5 sample rates] " + juce::String (pass ? "PASS" : "FAIL") + ": " + juce::String ((int) pts) + " points (finite, peak, DC); max peak " + juce::String (maxPeak, 2) + " dBFS; failures " + juce::String ((int) bad) + "; latency: " + latLine, pass && latOk);
    }

    // ===========================================================================================================
    // check 6: state round trip
    void check6()
    {
        std::printf ("\n--- check 6: state round trip (%d random states) ---\n", opt.stateTests);
        auto& mt = M (48000.0);
        Rng r (6006);
        long bad = 0, paramMismatch = 0, renderMismatch = 0; double lateWorst = 0, reuseWorst = 0, maxUlp = 0; long lateDiff = 0, reuseDiff = 0, ulpStates = 0, renderUlp = 0; std::map<juce::String, int> ulpParams;
        Progress pg ("check 6", opt.stateTests);
        Host A (cfg), B (cfg);
        Sig a, b, c, d;
        for (int t = 0; t < opt.stateTests; ++t)
        {
            State st = t == 0 ? host.defaults() : randomState (r);
            const int m = rotMat ((size_t) t);
            // A: a fresh instance with the state written, prepared, rendered
            A.create (true); A.applyState (st); A.prepare (48000.0, 512);
            juce::MemoryBlock mb; A.processor().getStateInformation (mb);
            A.render (mt.m[m], A.scEnabled() ? &mt.kick : nullptr, 12000, 512, nullptr, 0, nullptr, a);
            // B: a fresh instance restored from A's state, then prepared
            B.create (true);
            B.processor().setStateInformation (mb.getData(), (int) mb.getSize());
            int mism = 0, ulps = 0;
            for (size_t i = 0; i < pars.size(); ++i)
            {
                const float va = A.params()[i].p->getValue(), vb = B.params()[i].p->getValue();
                if (va == vb) continue;
                if (std::abs (va - vb) <= 2.5e-7f) { ++ulps; ulpParams[pars[i].id]++; maxUlp = std::max (maxUlp, (double) std::abs (va - vb)); continue; }   // 1-3 ulp of a float: skewed range without interval, plain -> normalised is not idempotent
                ++mism; if (mism < 3) failNote ("check6 state " + juce::String (t) + " " + pars[i].id + ": " + juce::String (va, 9) + " vs restored " + juce::String (vb, 9));
            }
            if (ulps) ++ulpStates;
            B.prepare (48000.0, 512);
            B.render (mt.m[m], B.scEnabled() ? &mt.kick : nullptr, 12000, 512, nullptr, 0, nullptr, b);
            const double dAB = maxAbsDiff (a, b);
            const bool exact = bitIdentical (a, b) || (ulps > 0 && dAB <= 1.0e-6);   // a parameter that came back 1 float-ulp away (skewed range) moves a filter coefficient by that much: -120 dB, same state
            if (! bitIdentical (a, b) && exact) ++renderUlp;
            // info 1: restore AFTER prepare (some hosts do)
            B.create (true); B.prepare (48000.0, 512); B.processor().setStateInformation (mb.getData(), (int) mb.getSize());
            B.render (mt.m[m], B.scEnabled() ? &mt.kick : nullptr, 12000, 512, nullptr, 0, nullptr, c);
            const double ld = maxAbsDiff (a, c); lateWorst = std::max (lateWorst, ld); if (ld != 0.0) ++lateDiff;
            // info 2: the long-lived instance (reused through every earlier render) vs the fresh one
            bool rb; prep (st, 48000.0, 512, rb);
            host.render (mt.m[m], keyFor (48000.0), 12000, 512, nullptr, 0, nullptr, d);
            const double rd = maxAbsDiff (a, d); reuseWorst = std::max (reuseWorst, rd); if (rd != 0.0) ++reuseDiff;
            if (mism) { ++paramMismatch; ++bad; }
            if (! exact) { ++renderMismatch; ++bad; failNote ("check6 state " + juce::String (t) + " render differs (max " + juce::String (maxAbsDiff (a, b), 9) + ") [" + stateText (st) + "]"); }
            row (6, "state", "-", juce::String (t), matName (m), 512, 48000, "param_mismatches", mism, "0", mism ? "FAIL" : "PASS");
            row (6, "state", "-", juce::String (t), matName (m), 512, 48000, "render_bit_identical", exact ? 1 : 0, "1", exact ? "PASS" : "FAIL");
            row (6, "state", "restore_after_prepare", juce::String (t), matName (m), 512, 48000, "max_abs_diff", ld, "info", "INFO");
            row (6, "state", "reused_instance", juce::String (t), matName (m), 512, 48000, "max_abs_diff_vs_fresh", rd, "info", "INFO");
            pg.tick();
        }
        const bool pass = bad == 0;
        addSummary ("[check 6 state] " + juce::String (pass ? "PASS" : "FAIL") + ": " + juce::String (opt.stateTests) + " states; parameter mismatches in " + juce::String ((int) paramMismatch) + " states (normalised value compared bit for bit; " + juce::String ((int) ulpStates) + " state(s) differ at float-ulp level only, max " + juce::String (maxUlp, 9) + [&] { juce::String t; for (auto& kv : ulpParams) t << " " << kv.first << "(" << kv.second << ")"; return t.isEmpty() ? juce::String() : ":" + t; }() + "; not counted as a mismatch); fresh-vs-restored renders not bit-identical in " + juce::String ((int) renderMismatch) + "; (info) restoring AFTER prepareToPlay differs in " + juce::String ((int) lateDiff) + " state(s), max " + juce::String (lateWorst, 9) + "; (info) the long-lived test instance differs from a fresh one in " + juce::String ((int) reuseDiff) + " state(s), max " + juce::String (reuseWorst, 9), pass);
    }

    // ===========================================================================================================
    // check 7: corners
    void check7()
    {
        std::printf ("\n--- check 7: corners (all min, all max, %d random states x 0.25 s) ---\n", opt.cornerStates);
        auto& mt = M (48000.0);
        Rng r (7007);
        long bad = 0, pts = 0; double maxPeak = -240; juce::String maxAt;
        const int n = (int) (0.25 * 48000);
        std::vector<State> list;
        { State lo (pars.size(), 0.0f), hi (pars.size(), 1.0f); list.push_back (lo); list.push_back (hi); }
        for (int i = 0; i < opt.cornerStates; ++i) list.push_back (randomState (r));
        std::vector<bool> scModes = { true }; if (cfg.scBus >= 0 && cfg.scBusAlsoDisabledInCorners) scModes.push_back (false);
        Progress pg ("check 7", (long) list.size() * (long) scModes.size() * 2);
        Sig out;
        for (bool sc : scModes)
        {
            Host H (cfg); H.create (sc);
            for (size_t s = 0; s < list.size(); ++s)
                for (int mi = 0; mi < (s < 2 ? nrot() + 1 : 1); ++mi)
                {
                    const int m = s < 2 ? (mi < nrot() ? rotMat ((size_t) mi) : (int) kSilence) : rotMat (s);
                    State st = list[s];
                    for (size_t i = 0; i < st.size(); ++i) st[i] = Host::snapNorm (pars[i], st[i]);
                    H.applyState (st); H.prepare (48000.0, 512);
                    H.render (mt.m[m], H.scEnabled() ? &mt.kick : nullptr, n, 512, nullptr, 0, nullptr, out);
                    auto me = measure (out, 48000.0);
                    const double inPk = toDb (mt.m[m].peak());
                    const int v = me.finite ? peakVerdict (me.peakDb, inPk, st) : 0;
                    const bool ok = me.finite && (m == kSilence ? me.peakDb < (cfg.noiseFloorDb ? cfg.noiseFloorDb (StateView { pars, st }) : -90.0) : v > 0);
                    ++pts; if (me.finite && me.peakDb > maxPeak) { maxPeak = me.peakDb; maxAt = juce::String (s == 0 ? "all min" : (s == 1 ? "all max" : "random " + juce::String ((int) s - 2))) + " " + matName (m) + (sc ? "" : " (sidechain bus off)"); }
                    row (7, s == 0 ? "all_min" : (s == 1 ? "all_max" : "random"), juce::String ((int) s), sc ? "scBusOn" : "scBusOff", matName (m), 512, 48000, "peak_dBFS", me.peakDb, "12|in+24", ok ? (v == 2 ? "PASS_EXCUSED" : "PASS") : "FAIL");
                    if (! ok) { ++bad; failNote ("check7 " + juce::String (s == 0 ? "all min" : (s == 1 ? "all max" : "random " + juce::String ((int) s - 2))) + " " + matName (m) + " sc " + juce::String ((int) sc) + " peak " + juce::String (me.peakDb, 2) + (me.finite ? "" : " NON-FINITE") + " [" + stateText (st) + "]"); }
                    if (mi == 0) pg.tick();
                }
        }
        const bool pass = bad == 0;
        addSummary ("[check 7 corners] " + juce::String (pass ? "PASS" : "FAIL") + ": " + juce::String ((int) pts) + " renders (all min, all max incl. silence, " + juce::String (opt.cornerStates) + " random)" + (scModes.size() > 1 ? " with the sidechain bus on and off" : "") + "; max peak " + juce::String (maxPeak, 2) + " dBFS (" + maxAt + "); failures " + juce::String ((int) bad), pass);
    }

    // ===========================================================================================================
    // check 8: cpu
    void check8()
    {
        if (! cpuHave) { addSummary ("[check 8 cpu] not run (needs check 1)", false); return; }
        const bool pass = cpuWorst <= 50.0 || cpuReplay <= 50.0;
        addSummary ("[check 8 cpu] " + juce::String (pass ? (cpuWorst <= 50.0 ? "PASS" : "PASS (worst call was a one-off: replayed 6x its fastest time is " + juce::String (cpuReplay, 2) + " %)") : "FAIL (worst > 50 % and it reproduces)") + " [worst call " + cpuWorstWhat + ", replay best " + juce::String (cpuReplay, 2) + " %]: worst processBlock " + juce::String (cpuWorst, 2) + " % of real time (block 512, 48 kHz), p99.9 " + juce::String (cpuP999, 2) + " %, median " + juce::String (cpuMed, 2) + " %, over " + juce::String ((int) cpuCalls) + " steady calls (the first call after each prepareToPlay excluded: worst of those " + juce::String (cpuFirstWorst, 2) + " %); worst cases include OS scheduling spikes on a shared machine", pass);
    }
    bool cpuHave = false, cpuPass = true; double cpuWorst = 0, cpuP999 = 0, cpuMed = 0, cpuFirstWorst = 0, cpuReplay = 0; juce::String cpuWorstWhat; long cpuCalls = 0;

    // ===========================================================================================================
    void writeDocs (double secs)
    {
        auto f = juce::File (opt.docsFile); f.getParentDirectory().createDirectory();
        juce::String t;
        t << "# " << cfg.name << " control proof\n\n"
          << "Generated by `--controlproof` (tests/SvlControlProof.h, master subverselab-plugin-common/ControlProof). Do not edit by hand; rerun the harness.\n"
          << "Run: " << juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H:%M") << ", " << juce::String (secs, 0) << " s, " << pars.size() << " parameters. Raw numbers: `" << opt.outDir << "/" << cfg.name << ".tsv`.\n\n"
          << "## Check results\n\n";
        for (auto& l : summary) t << "- " << l.line << "\n";
        t << "\n## Per parameter\n\n| parameter | range | direction declared | monotonic | max peak (dBFS) | automation step (dB vs static) | result |\n|---|---|---|---|---|---|---|\n";
        for (size_t i = 0; i < pars.size(); ++i)
        {
            auto& r = res[i];
            const bool ok = r.sweepOk && r.tailOk && r.dirOk && r.autoOk && r.srOk && r.blockOk;
            juce::String a = r.maxAutoRatioDb < -200 ? juce::String ("-") : juce::String (r.maxAutoRatioDb, 1) + (r.autoDiscrete ? " (discrete, reported)" : "") + ", max diff vs block 1 " + juce::String (r.maxAutoDiff, 4);
            juce::String note = cfg.notes.count (pars[i].id) ? " (" + cfg.notes.at (pars[i].id) + ")" : juce::String();
            t << "| " << pars[i].id << " | " << r.range << " | " << r.dirText << " | " << r.monotone << " | " << (r.maxPeakDb < -200 ? juce::String ("-") : juce::String (r.maxPeakDb, 1)) << " | " << a << " | " << (ok ? "PASS" : "FAIL") << note << " |\n";
        }
        f.replaceWithText (t);
        std::printf ("wrote %s and %s/%s.tsv\n", opt.docsFile.toRawUTF8(), opt.outDir.toRawUTF8(), cfg.name.toRawUTF8());
    }
};


// ---------------------------------------------------------------------------------------------------------------
// self-test of the harness: a tiny processor into which known faults can be put back. Every fault must turn exactly the
// check that is meant to catch it red, and the fault-free processor must be green. (A guard is trusted only after the
// fault it guards against was seen to fail it.)   Run: <tool> --controlproof-selftest
namespace selftest
{
enum Fault { None = 0, Click = 1, BlockDep = 2, NaNAtMax = 4, StateLoss = 8, BadLatency = 16, DirInverted = 32, DcGrowth = 64, SelfOsc = 128, Hog = 256 };
inline int activeFaults = 0;

class Dummy : public juce::AudioProcessor
{
public:
    Dummy() : AudioProcessor (BusesProperties().withInput ("In", juce::AudioChannelSet::stereo(), true).withOutput ("Out", juce::AudioChannelSet::stereo(), true)),
              apvts (*this, nullptr, "Dummy", layout())
    {
        pGain = apvts.getRawParameterValue ("gain"); pTone = apvts.getRawParameterValue ("tone"); pMode = apvts.getRawParameterValue ("mode"); pSw = apvts.getRawParameterValue ("sw");
    }
    static juce::AudioProcessorValueTreeState::ParameterLayout layout()
    {
        juce::AudioProcessorValueTreeState::ParameterLayout l;
        l.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "gain", 1 }, "Gain", juce::NormalisableRange<float> (0.0f, 1.0f), 0.5f));
        l.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "tone", 1 }, "Tone", juce::NormalisableRange<float> (0.0f, 1.0f), 0.0f));
        l.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "mode", 1 }, "Mode", juce::StringArray { "A", "B", "C" }, 0));
        l.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "sw", 1 }, "Sw", false));
        return l;
    }
    const juce::String getName() const override { return "Dummy"; }
    void prepareToPlay (double sr, int) override
    {
        sampleRate = sr; g = *pGain * 2.0f; aS = 1.0f - *pTone * 0.9f; lp[0] = lp[1] = 0; dc = 0; phase = 0; calls = 0;
        delay.assign (2, std::vector<float> (kLat, 0.0f)); dpos = 0;
        setLatencySamples ((activeFaults & BadLatency) ? 10 : kLat);
    }
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& l) const override { return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo() && l.getMainInputChannelSet() == juce::AudioChannelSet::stereo(); }
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        const int n = b.getNumSamples();
        const float target = (activeFaults & DirInverted) ? (1.0f - *pGain) * 2.0f : *pGain * 2.0f;
        const float aT = 1.0f - *pTone * 0.9f;
        if (activeFaults & BlockDep) g += (target - g) * 0.2f;
        const bool hog = (activeFaults & Hog) && (++calls % 8 == 0);
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i)
        {
            aS += (aT - aS) * 0.001f; const float a = (activeFaults & Click) ? aT : aS;
            if (activeFaults & Click) g = target;
            else if (! (activeFaults & BlockDep)) g += (target - g) * 0.001f;
            for (int c = 0; c < 2; ++c)
            {
                float x = b.getReadPointer (c)[i];
                lp[c] += a * (x - lp[c]);
                float y = lp[c];
                float d = delay[(size_t) c][(size_t) dpos]; delay[(size_t) c][(size_t) dpos] = y; y = d;
                y *= g;
                if ((activeFaults & NaNAtMax) && *pGain > 0.999f) y = std::nanf ("");
                if ((activeFaults & DcGrowth) && *pTone > 0.9f) y += (float) dc;
                if ((activeFaults & SelfOsc) && *pMode > 1.5f && *pSw > 0.5f) y += 1.5f * std::sin ((float) phase);
                b.getWritePointer (c)[i] = y;
            }
            dc += 1e-5; phase += 6.283185307 * 440.0 / sampleRate; if (phase > 6.283185307) phase -= 6.283185307;
            if (++dpos == kLat) dpos = 0;
        }
        if (hog) while (std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count() < 0.85 * n / sampleRate) {}
    }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    bool hasEditor() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock& m) override { if (auto x = apvts.copyState().createXml()) copyXmlToBinary (*x, m); }
    void setStateInformation (const void* d, int sz) override
    {
        if (auto x = getXmlFromBinary (d, sz)) if (x->hasTagName (apvts.state.getType()))
        {
            auto vt = juce::ValueTree::fromXml (*x);
            if (activeFaults & StateLoss) { auto c = vt.getChildWithProperty ("id", "tone"); if (c.isValid()) vt.removeChild (c, nullptr); }
            apvts.replaceState (vt);
        }
    }
    juce::AudioProcessorValueTreeState apvts;
private:
    static constexpr int kLat = 5;
    std::atomic<float> *pGain = nullptr, *pTone = nullptr, *pMode = nullptr, *pSw = nullptr;
    float g = 0, aS = 1.0f, lp[2] {}; double dc = 0, phase = 0, sampleRate = 48000.0; int calls = 0, dpos = 0; std::vector<std::vector<float>> delay;
};

inline int run()
{
    struct Case { const char* name; int fault; const char* mustFail; };   // mustFail: the line prefix that has to turn red
    const Case cases[] = { { "none (baseline, all green)", None, "" }, { "unsmoothed gain (click)", Click, "[check 3" }, { "block-size dependent smoothing", BlockDep, "[check 4" },
                           { "NaN at gain = max", NaNAtMax, "[check 1" }, { "state restore drops a parameter", StateLoss, "[check 6" }, { "reported latency != real", BadLatency, "[check 5" },
                           { "gain direction inverted", DirInverted, "[check 2" }, { "DC growth when tone > 0.9", DcGrowth, "[check 1" }, { "self-oscillation in silence (mode C + sw)", SelfOsc, "[check 1" },
                           { "CPU hog (85 % of the block in 1 call of 8)", Hog, "[check 8" } };
    int bad = 0;
    std::printf ("controlproof self-test: each injected fault must turn its check red, the baseline must be green\n");
    for (auto& c : cases)
    {
        activeFaults = c.fault;
        Config cfg; cfg.name = "SELFTEST"; cfg.make = [] { return std::make_unique<Dummy>(); };
        Direction d; d.id = "gain"; d.dir = +1; d.feature = feat::outRmsDb(); d.tol = 0.25; d.featureName = "output rms";
        cfg.directions = { d };
        cfg.designedGainDb = [] (const StateView&) { return 6.0; };
        Options o; o.noDocs = true; o.outDir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("svl_cp_selftest").getFullPathName();
        o.sweepStates = 3; o.tailStates = 3; o.blockStates = 3; o.autoStates = 1; o.stateTests = 6; o.cornerStates = 10; o.srStates = 2;
        if (c.fault == Hog) { o.onlyChecks = (1 << 1) | (1 << 8); o.sweepStates = 1; o.tailStates = 1; }
        std::printf ("\n##### fault: %s\n", c.name);
        std::fflush (stdout);
        Proof p (cfg, o);
        p.run();
        bool anyRed = false, targetRed = false;
        for (auto& l : p.results())
        {
            if (! l.pass) anyRed = true;
            if (! l.pass && juce::String (c.mustFail).isNotEmpty() && l.line.startsWith (c.mustFail)) targetRed = true;
        }
        const bool ok = c.fault == None ? ! anyRed : targetRed;
        std::printf ("##### self-test %s: %s\n", c.name, ok ? "OK" : "WRONG (the harness did not behave as expected)");
        if (! ok) ++bad;
    }
    activeFaults = None;
    std::printf ("\ncontrolproof self-test: %d of %d cases behaved as expected\n", (int) (sizeof (cases) / sizeof (cases[0])) - bad, (int) (sizeof (cases) / sizeof (cases[0])));
    return bad == 0 ? 0 : 1;
}
}   // namespace selftest

/** parses --controlproof options and runs; returns the exit code */
inline int main (const Config& cfg, int argc, char** argv)
{
    Options o;
    for (int i = 1; i < argc; ++i) if (juce::String (argv[i]) == "--controlproof-selftest") return selftest::run();
    for (int i = 1; i < argc; ++i)
    {
        const juce::String a (argv[i]);
        auto next = [&] { return i + 1 < argc ? juce::String (argv[++i]) : juce::String(); };
        if (a == "--quick") { o.quick = true; o.sweepStates = 4; o.tailStates = 2; o.blockStates = 4; o.autoStates = 1; o.stateTests = 12; o.cornerStates = 30; o.srStates = 2; }
        else if (a == "--out") o.outDir = next();
        else if (a == "--docs") o.docsFile = next();
        else if (a == "--no-docs") o.noDocs = true;
        else if (a == "--param") o.onlyParam = next();
        else if (a == "--probe") o.probe = next();
        else if (a == "--set") o.baseSet = next();
        else if (a == "--probe-material") o.probeMat = next();
        else if (a == "--probe-pre") o.probePre = next();
        else if (a == "--probe-samples") o.probeSamples = next().getIntValue();
        else if (a == "--checks") { auto l = juce::StringArray::fromTokens (next(), ",", ""); o.onlyChecks = 0; for (auto& s : l) o.onlyChecks |= 1 << s.getIntValue(); }
        else if (a == "--states") o.sweepStates = next().getIntValue();
        else if (a == "--tail") o.tailOverride = next().getDoubleValue();
        else if (a == "--tail-states") o.tailStates = next().getIntValue();
    }
    Proof p (cfg, o);
    return p.run();
}
}   // namespace svl::controlproof
