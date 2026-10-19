// SourceSense v0: names the kind of source in an input stream from measured features. Plain C++17, header-only, no JUCE.
// Shared by the family-2 devices (AILE_2_PLAN.md "SourceSense"). Measured, not learned at run time: seven audio features are compared with a
// table of class profiles (mean and sigma per feature, 7 x 8 numbers) that tools/sourcesense_fit.py measured on labelled synthetic clips.
// Every threshold and constant is named in namespace `rules` with a comment saying where it came from. No network, no model file.
//
// Real-time safe: every buffer is sized in prepare(); process() does no allocation, no locking, no I/O.
// Usage:   SourceSense ss; ss.prepare (sr); ...audio thread: ss.process (L, R, n); ...any tick: auto r = ss.result();
// Result:  class + confidence after 10 s of input (before that: Unsure, confidence 0, ready = false).
//          Below kMinConfidence (0.7) the class is Unsure and the device keeps its manual defaults.
//
// What it measures (per ~85 ms analysis frame of the mid signal; energy-weighted over the last ~20 s with a decay):
//   centroid, flatness, low-band share (< 150 Hz), onset rate, transient crest, harmonicity (autocorrelation pitch strength), stereo width.
// Plus one helper feature: duty (share of time the source is audible), which separates a phrased voice from sustained notes.
//
// Honest limits: v0 was tuned on synthetic clips (tools/sourcesense_set.py in the SEDA project). Real stems come later.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <vector>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace svl
{
namespace sourcesense
{
enum Class : int { Vocal = 0, Drums, Kick, Bass, Keys, Pad, FullMix, Unsure, kNumClasses };

inline const char* className (int c)
{
    static const char* n[kNumClasses] = { "vocal", "drums", "kick", "bass", "keys", "pad", "fullmix", "unsure" };
    return n[std::max (0, std::min ((int) kNumClasses - 1, c))];
}

struct Features
{
    double centroidHz = 0;    // spectral centroid, 40 Hz .. Nyquist, power-weighted
    double flatness = 0;      // geometric / arithmetic mean of the power spectrum, 100 Hz .. 8 kHz (1 = noise, 0 = a pure tone)
    double lowShare = 0;      // power below 150 Hz / total power
    double onsetRate = 0;     // onsets per second (all time, silence included)
    double crestDb = 0;       // peak / RMS of the analysis frame, dB
    double harmonicity = 0;   // normalised autocorrelation peak, 60 .. 1000 Hz, 0 .. 1
    double width = 0;         // side / (mid + side) power, 0 = mono
    double duty = 0;          // share of frames above the audibility gate
    double activeSec = 0;     // seconds of audible material in the window (decayed)
};

struct Result
{
    Class cls = Unsure;
    double confidence = 0;
    bool ready = false;       // 10 s of input seen
    Features f;
    double score[kNumClasses] = {};
};

// ---------------------------------------------------------------------------------------------------------------------------------
// Rule constants. "dev set" = the synthetic clips of tools/sourcesense_set.py sets A and B (10 clips per class each, 20 per class pooled) that the
// class table was measured on; set C was never used for anything but the final count. Feature distributions: tools/sourcesense_eval.py --features.
namespace rules
{
constexpr double kMinConfidence   = 0.70;    // AILE_2_PLAN.md: below 0.7 it says "not sure" and leaves the manual defaults
constexpr double kReadySeconds    = 10.0;    // AILE_2_PLAN.md / TASK_P1b: the class is offered after 10 s
constexpr double kMinActiveSec    = 2.0;     // at least 2 s of audible material, else nothing to judge (a quiet room is "unsure")
constexpr double kWindowSeconds   = 20.0;    // AILE_2_PLAN.md: "reads the input for 10-20 s"; statistics decay with this time constant
constexpr double kGateDb          = -60.0;   // frame RMS (mid) below this is silence; the clip set goes down to -30 dBFS peak, so 30 dB of margin
constexpr double kLowBandHz       = 150.0;   // TASK_P1b: low-band share is below 150 Hz
constexpr double kOnsetGateDb     = -55.0;   // flux is only read while the 1024-sample frame RMS is above this (power dB, mid); 5 dB under the audibility gate so a decaying hit still counts
constexpr double kOnsetBandFloorDb = -75.0;  // band levels are floored here before the flux is taken, so noise-floor jitter does not add to it (a -30 dBFS clip has 45 dB above it)
constexpr double kOnsetMinFlux    = 16.0;    // dB summed over the bands: an onset needs at least this much new energy; dev set: stationary noise and pads stay near 0 at this value, a plucked note adds 40+
constexpr double kOnsetRatio      = 2.5;     // and at least this many times the running mean flux (adaptive threshold peak picking after Boeck & Widmer 2013, SuperFlux); 2.2 let white noise through, 3.0 lost soft keys
constexpr double kOnsetMeanSec    = 0.5;     // time constant of the running mean flux
constexpr double kDensitySeconds  = 4.0;     // time constant of the short-window onset rate (onsetRateShort), for devices that follow the density of the playing, e.g. SEDA's note choice; short enough to follow a section change in a few seconds
constexpr double kOnsetRefractory = 0.060;   // s; fastest repeat the rules need (16 per second); a note repeated faster than this is one onset
constexpr double kPitchMinHz      = 60.0;    // lowest pitch looked for in the autocorrelation (a 4096-sample frame holds 5 periods)
constexpr double kPitchMaxHz      = 1000.0;  // highest; above this the main lobe of the lag-0 peak is not resolved

// Feature space the rules live in (the same transform is applied by tools/sourcesense_fit.py): spectral features are multiplicative, so they are taken on a log scale.
//   0 centroid  log2 (Hz)          1 flatness  log10 (x + 1e-5)     2 low share  x            3 onset rate  log2 (x + 0.25)
//   4 crest     dB                 5 harmonicity x                  6 width      log10 (x + 0.003)   7 duty  x
constexpr int kNumFeatures = 8;
constexpr int kWidthFeature = 6;   // index of the stereo-width feature, see above

// Match of a recording to a class profile: z = |x - mean| / sigma per feature, against the class mean and sigma measured on the dev set (table below).
constexpr double kTemper      = 1.0;    // posterior = softmax (-0.5 * sum z^2 / kTemper): plain naive-Bayes weighting. Tried 1, 2, 3, 4, 6 on set A (tools/sourcesense_sweep.py): 1 gave the most correct clips, 4 only 62 of 80
constexpr double kFitFull     = 3.0;    // the best class counts as a full fit up to a mean z^2 of 3 (a sample inside about 1.7 sigma on every feature); swept 2..6 on sets A+B, the result moved by 2 clips
constexpr double kFitNone     = 6.0;    // and as no fit at all from a mean z^2 of 6 (2.4 sigma on every feature): then the clip is none of the seven, whatever the other six score; swept 6..25, 6 was best
// Sigma floors, per feature, in the transformed units above: with 10 clips per class a measured sigma can be unrealistically small, and a held-out clip would then fall off the edge.
constexpr double kSigmaFloor[kNumFeatures] = { 0.35 /*octaves: spread of one voice or one patch*/, 0.30 /*decades*/, 0.06, 0.45 /*log2 onsets/s*/, 0.9 /*dB*/, 0.05, 0.25 /*decades*/, 0.04 };

// Per-class mean and sigma on the dev sets A+B pooled, features in the order above. Generated by `tools/sourcesense_fit.py --write` (do not edit by hand).
// BEGIN FIT
struct MuSigma { double mu, sigma; };
constexpr MuSigma kClassFit[7][kNumFeatures] = {
    { { 8.3702, 0.4090 }, { -4.5229, 0.2430 }, { 0.2322, 0.1915 }, { 1.2353, 0.3716 }, { 9.3530, 1.0653 }, { 0.9493, 0.0249 }, { -2.5229, 0.0000 }, { 0.6916, 0.0918 } },   // vocal, n = 20
    { { 12.1097, 0.4125 }, { -0.7551, 0.0939 }, { 0.5331, 0.1027 }, { 2.3681, 0.3818 }, { 12.2845, 0.8173 }, { 0.4522, 0.1244 }, { -1.7502, 0.2148 }, { 0.9437, 0.0571 } },   // drums, n = 20
    { { 6.3221, 0.3233 }, { -1.5687, 0.4239 }, { 0.9818, 0.0176 }, { 1.0695, 0.6112 }, { 5.2825, 0.7843 }, { 0.5393, 0.3441 }, { -2.5229, 0.0000 }, { 0.9015, 0.1893 } },   // kick, n = 20
    { { 7.3537, 0.4831 }, { -4.4212, 0.4716 }, { 0.5373, 0.2465 }, { 0.9466, 1.2686 }, { 7.2460, 1.8422 }, { 0.8140, 0.1668 }, { -2.5229, 0.0000 }, { 0.9327, 0.0542 } },   // bass, n = 20
    { { 8.6863, 0.7440 }, { -4.0939, 0.9045 }, { 0.1456, 0.1458 }, { 1.3067, 1.0952 }, { 9.3960, 0.6874 }, { 0.6592, 0.1323 }, { -1.9494, 0.3777 }, { 0.9689, 0.0757 } },   // keys, n = 20
    { { 9.1308, 0.3957 }, { -3.2080, 0.6552 }, { 0.0347, 0.0653 }, { -1.5942, 0.5405 }, { 8.5800, 1.1724 }, { 0.7903, 0.1587 }, { -1.6495, 0.6613 }, { 0.9666, 0.0114 } },   // pad, n = 20
    { { 10.1372, 0.3918 }, { -1.5039, 0.2725 }, { 0.3950, 0.1335 }, { 2.4499, 0.2953 }, { 10.6075, 0.4937 }, { 0.5004, 0.0655 }, { -0.9836, 0.4112 }, { 0.9990, 0.0024 } },   // fullmix, n = 20
};
// END FIT
}

// ---------------------------------------------------------------------------------------------------------------------------------
class SourceSense
{
public:
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        N = 1; while ((double) N < 0.08 * sr) N <<= 1;           // analysis frame: 4096 samples at 44.1 / 48 kHz
        hop = N;   // no overlap between analysis frames (CPU: two 8192-point FFTs per frame)
        M = 2 * N; F = 1 << ((int) std::ceil (std::log2 (0.02 * sr))); hopF = F / 4;   // 8192 and 1024 at 48 kHz
        big.init (M); small.init (F);
        ringM.assign ((size_t) N, 0.0); ringS.assign ((size_t) N, 0.0);
        win.assign ((size_t) N, 0.0); winAc.assign ((size_t) N, 0.0); winF.assign ((size_t) F, 0.0);
        xin.assign ((size_t) M, 0.0); ac.assign ((size_t) M, 0.0); re.assign ((size_t) M / 2 + 1, 0.0); im.assign ((size_t) M / 2 + 1, 0.0); pw.assign ((size_t) M / 2 + 1, 0.0); zeroI.assign ((size_t) M / 2 + 1, 0.0);
        xinF.assign ((size_t) F, 0.0); reF.assign ((size_t) F / 2 + 1, 0.0); imF.assign ((size_t) F / 2 + 1, 0.0);
        for (int i = 0; i < N; ++i) win[(size_t) i] = 0.5 - 0.5 * std::cos (2.0 * M_PI * (i + 0.5) / N);
        for (int i = 0; i < F; ++i) winF[(size_t) i] = 0.5 - 0.5 * std::cos (2.0 * M_PI * (i + 0.5) / F);
        // autocorrelation of the window, to undo the bias a windowed frame puts on r(tau)
        std::fill (xin.begin(), xin.end(), 0.0);
        for (int i = 0; i < N; ++i) xin[(size_t) i] = win[(size_t) i];
        big.forward (xin.data(), re.data(), im.data());
        for (int k = 0; k <= M / 2; ++k) pw[(size_t) k] = re[(size_t) k] * re[(size_t) k] + im[(size_t) k] * im[(size_t) k];
        big.inverse (pw.data(), zeroI.data(), ac.data());
        for (int i = 0; i < N; ++i) winAc[(size_t) i] = ac[(size_t) i] / std::max (1e-30, ac[0]);
        // onset bands: 1/2-octave from 100 Hz to 12 kHz (or 0.45 sr), as bin ranges of the small FFT
        nBands = 0; const double top = std::min (12000.0, 0.45 * sr); double lo = 100.0;
        while (lo < top && nBands < kMaxBands) { const double hi = std::min (top, lo * 1.4142135); bandLo[nBands] = std::max (1, (int) std::floor (lo / sr * F)); bandHi[nBands] = std::max (bandLo[nBands], (int) std::floor (hi / sr * F)); ++nBands; lo = hi; }
        for (int b = 0; b < nBands; ++b) if (b > 0 && bandLo[b] <= bandHi[b - 1]) bandLo[b] = bandHi[b - 1] + 1;   // a bin belongs to one band only; a band with no bin is skipped below
        refractoryHops = std::max (1, (int) std::lround (rules::kOnsetRefractory * sr / hopF));
        meanCoef = 1.0 - std::exp (-(double) hopF / sr / rules::kOnsetMeanSec);
        hopSec = (double) hop / sr; decay = std::exp (-hopSec / rules::kWindowSeconds); decayS = std::exp (-hopSec / rules::kDensitySeconds);
        lagMin = std::max (2, (int) std::floor (sr / rules::kPitchMaxHz)); lagMax = std::min (N / 2, (int) std::ceil (sr / rules::kPitchMinHz));
        binLow = (int) std::floor (rules::kLowBandHz / sr * M); fl0 = (int) std::ceil (100.0 / sr * M); fl1 = std::min (M / 2, (int) std::floor (8000.0 / sr * M));
        c0 = std::max (1, (int) std::ceil (40.0 / sr * M));
        reset();
    }

    void reset()
    {
        std::fill (ringM.begin(), ringM.end(), 0.0); std::fill (ringS.begin(), ringS.end(), 0.0);
        wpos = 0; sinceHop = 0; sinceHopF = 0; seen = 0; elapsed = 0.0;
        sOnS = sTS = 0.0; sW = sT = sOn = sCent = sFlat = sLow = sCrest = sHarm = sWidth = 0.0;
        for (double& v : prevBand) v = rules::kOnsetBandFloorDb;
        fluxMean = 0.0; flux1 = flux2 = 0.0; cool = 0; haveFlux = 0;
    }

    // L and R may be the same buffer for a mono source (or R = nullptr). Any block size.
    void process (const float* l, const float* r, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            const double a = l[i], b = r ? r[i] : l[i], m = 0.5 * (a + b), s = 0.5 * (a - b);
            ringM[(size_t) wpos] = m; ringS[(size_t) wpos] = s; if (++wpos >= N) wpos = 0;
            ++seen;
            if (++sinceHopF >= hopF) { sinceHopF = 0; if (seen >= (uint64_t) F) fluxHop(); }
            if (++sinceHop >= hop) { sinceHop = 0; if (seen >= (uint64_t) N) frame(); }
        }
        elapsed = (double) seen / sr;
    }

    Result result() const
    {
        Result res; res.ready = elapsed >= rules::kReadySeconds && sW * hopSec >= rules::kMinActiveSec;
        Features& f = res.f;
        f.onsetRate = sT > 0 ? sOn / sT : 0.0; f.duty = sT > 0 ? sW * hopSec / sT : 0.0; f.activeSec = sW * hopSec;
        if (sW > 0) { f.centroidHz = sCent / sW; f.flatness = sFlat / sW; f.lowShare = sLow / sW; f.crestDb = sCrest / sW; f.harmonicity = sHarm / sW; f.width = sWidth / sW; }
        if (elapsed < rules::kReadySeconds) { res.cls = Unsure; res.confidence = 0.0; return res; }   // not enough input yet
        classify (res);
        return res;
    }

    double elapsedSeconds() const { return elapsed; }

    // Onsets per second over the last few seconds (time constant kDensitySeconds, silence counts as time); -1 until 2 s of input have been seen.
    double onsetRateShort() const { return (elapsed >= 2.0 && sTS > 0.0) ? sOnS / sTS : -1.0; }

    // The rules. Public so the evaluation tool can classify recorded feature vectors without re-running audio.
    static void classify (Result& res);

private:
    // iterative radix-2 FFT with its own tables; run() works in place on re/im, inverse divides by n
    struct Fft
    {
        int n = 0; std::vector<int> rev; std::vector<double> tw;
        void init (int size)
        {
            n = size; rev.assign ((size_t) n, 0); tw.assign ((size_t) n, 0.0);
            int lg = 0; while ((1 << lg) < n) ++lg;
            for (int i = 0; i < n; ++i) { int r = 0; for (int b = 0; b < lg; ++b) if (i & (1 << b)) r |= 1 << (lg - 1 - b); rev[(size_t) i] = r; }
            for (int k = 0; k < n / 2; ++k) { tw[(size_t) (2 * k)] = std::cos (2.0 * M_PI * k / n); tw[(size_t) (2 * k + 1)] = -std::sin (2.0 * M_PI * k / n); }
        }
        void run (double* re, double* im, bool inverse) const
        {
            for (int i = 0; i < n; ++i) { const int j = rev[(size_t) i]; if (j > i) { std::swap (re[i], re[j]); std::swap (im[i], im[j]); } }
            for (int i = 0; i < n; i += 2)   // len 2: twiddle 1
            {
                const double xr = re[i + 1], xi = im[i + 1]; re[i + 1] = re[i] - xr; im[i + 1] = im[i] - xi; re[i] += xr; im[i] += xi;
            }
            for (int len = 4; len <= n; len <<= 1)
            {
                const int half = len / 2, step = n / len;
                for (int k = 0; k < half; ++k)   // twiddle fetched once per k, then applied down the whole array
                {
                    const double wr = tw[(size_t) (2 * k * step)], wi = inverse ? -tw[(size_t) (2 * k * step + 1)] : tw[(size_t) (2 * k * step + 1)];
                    for (int a = k; a < n; a += len)
                    {
                        const int b = a + half;
                        const double xr = re[b] * wr - im[b] * wi, xi = re[b] * wi + im[b] * wr;
                        re[b] = re[a] - xr; im[b] = im[a] - xi; re[a] += xr; im[a] += xi;
                    }
                }
            }
            if (inverse) { const double g = 1.0 / n; for (int i = 0; i < n; ++i) { re[i] *= g; im[i] *= g; } }
        }
    };

    // real FFT of n points through a complex FFT of n/2 (even samples = real part, odd = imaginary part), with the usual split step.
    // forward: x[0..n) -> X[0..n/2] (re, im); inverse: Hermitian X[0..n/2] -> x[0..n), scaled so that inverse (forward (x)) = x. Scratch buffers are members.
    struct RealFft
    {
        int n = 0, h = 0; Fft cf; std::vector<double> zr, zi, wr, wi;   // wr/wi = e^{-2 pi i k / n}, k = 0..h
        void init (int size)
        {
            n = size; h = n / 2; cf.init (h); zr.assign ((size_t) h, 0.0); zi.assign ((size_t) h, 0.0); wr.assign ((size_t) h + 1, 0.0); wi.assign ((size_t) h + 1, 0.0);
            for (int k = 0; k <= h; ++k) { wr[(size_t) k] = std::cos (2.0 * M_PI * k / n); wi[(size_t) k] = -std::sin (2.0 * M_PI * k / n); }
        }
        void forward (const double* x, double* Xr, double* Xi)
        {
            for (int m = 0; m < h; ++m) { zr[(size_t) m] = x[2 * m]; zi[(size_t) m] = x[2 * m + 1]; }
            cf.run (zr.data(), zi.data(), false);
            for (int k = 0; k <= h; ++k)
            {
                const int k1 = k == h ? 0 : k, k2 = k == 0 ? 0 : h - k;
                const double ar = zr[(size_t) k1], ai = zi[(size_t) k1], br = zr[(size_t) k2], bi = -zi[(size_t) k2];   // Z[k], conj Z[h-k]
                const double er = 0.5 * (ar + br), ei = 0.5 * (ai + bi), orr = 0.5 * (ai - bi), oi = -0.5 * (ar - br);   // Xe = (a + b) / 2, Xo = (a - b) / (2i)
                Xr[k] = er + wr[(size_t) k] * orr - wi[(size_t) k] * oi; Xi[k] = ei + wr[(size_t) k] * oi + wi[(size_t) k] * orr;
            }
        }
        void inverse (const double* Xr, const double* Xi, double* x)
        {
            for (int k = 0; k < h; ++k)
            {
                const double ar = Xr[k], ai = Xi[k], br = Xr[h - k], bi = -Xi[h - k];   // X[k], conj X[h-k]
                const double er = 0.5 * (ar + br), ei = 0.5 * (ai + bi);
                const double dr = 0.5 * (ar - br), di = 0.5 * (ai - bi);                // (X[k] - conj X[h-k]) / 2, to be multiplied by W^-k = (wr, -wi)
                const double or_ = dr * wr[(size_t) k] + di * wi[(size_t) k], oi = di * wr[(size_t) k] - dr * wi[(size_t) k];
                zr[(size_t) k] = er - oi; zi[(size_t) k] = ei + or_;                   // Z = Xe + i Xo
            }
            cf.run (zr.data(), zi.data(), true);
            for (int m = 0; m < h; ++m) { x[2 * m] = zr[(size_t) m]; x[2 * m + 1] = zi[(size_t) m]; }
        }
    };

    // ---- onsets: half-octave band levels of a 1024-sample frame every 256 samples, summed positive change in dB (spectral flux), adaptive peak picking
    void fluxHop()
    {
        double em = 0;
        for (int i = 0; i < F; ++i)
        {
            const double m = ringM[(size_t) ((wpos - F + i + 2 * N) & (N - 1))];
            em += m * m; xinF[(size_t) i] = m * winF[(size_t) i];
        }
        const bool audible = 10.0 * std::log10 (em / F + 1e-20) >= rules::kOnsetGateDb;
        small.forward (xinF.data(), reF.data(), imF.data());
        const double scale = 4.0 / ((double) F * (double) F * 0.25 * 0.25 * 4.0);   // |X| / (sum(win) / 2) with sum(win) = F / 2: a full-scale sine reads 0 dB per band
        double flux = 0.0; int used = 0;
        for (int b = 0; b < nBands; ++b)
        {
            if (bandHi[b] < bandLo[b]) continue;
            double e = 0; for (int k = bandLo[b]; k <= std::min (bandHi[b], F / 2); ++k) e += reF[(size_t) k] * reF[(size_t) k] + imF[(size_t) k] * imF[(size_t) k];
            const double lv = std::max (rules::kOnsetBandFloorDb, 10.0 * std::log10 (e * scale + 1e-30));
            curBand[b] = lv; ++used;
        }
        // SuperFlux: compare with the maximum of the neighbouring bands one hop ago, so a steady partial wobbling between two bands is not new energy
        for (int b = 0; b < nBands; ++b)
        {
            const double ref = std::max (prevBand[b], std::max (b > 0 ? prevBand[b - 1] : -1e9, b + 1 < nBands ? prevBand[b + 1] : -1e9));
            if (audible && curBand[b] > ref) flux += curBand[b] - ref;
        }
        for (int b = 0; b < nBands; ++b) prevBand[b] = curBand[b];
        (void) used;
        // flux1 = flux one hop ago (the candidate), flux2 = two hops ago, flux = now: a candidate is a local maximum
        if (cool > 0) --cool;
        if (haveFlux >= 2 && cool == 0 && flux1 >= flux2 && flux1 > flux && flux1 >= rules::kOnsetMinFlux && flux1 >= rules::kOnsetRatio * fluxMean)
        { sOn += 1.0; sOnS += 1.0; cool = refractoryHops; }
        fluxMean += (flux1 - fluxMean) * meanCoef;
        flux2 = flux1; flux1 = flux; if (haveFlux < 2) ++haveFlux;
    }

    // ---- one analysis frame (the last N samples)
    void frame()
    {
        // decay all statistics by one hop
        sW *= decay; sT *= decay; sOn *= decay; sCent *= decay; sFlat *= decay; sLow *= decay; sCrest *= decay; sHarm *= decay; sWidth *= decay;
        sT += hopSec; sOnS *= decayS; sTS = sTS * decayS + hopSec;
        double em = 0, es = 0, pk = 0;
        for (int i = 0; i < N; ++i)
        {
            const size_t k = (size_t) ((wpos + i) & (N - 1));   // oldest first (N is a power of two)
            const double m = ringM[k], s = ringS[k];
            em += m * m; es += s * s; pk = std::max (pk, std::fabs (m));
            xin[(size_t) i] = m * win[(size_t) i];
        }
        const double rmsDb = 10.0 * std::log10 (em / N + 1e-20);
        if (rmsDb < rules::kGateDb) return;   // silence: only the time base advanced
        for (int i = N; i < M; ++i) xin[(size_t) i] = 0.0;
        big.forward (xin.data(), re.data(), im.data());
        double tot = 0, low = 0, cn = 0, cd = 0;
        for (int k = 0; k <= M / 2; ++k)
        {
            const double p = re[(size_t) k] * re[(size_t) k] + im[(size_t) k] * im[(size_t) k]; pw[(size_t) k] = p;
            tot += p; if (k < binLow) low += p;
            if (k >= c0) { cn += p * k; cd += p; }
        }
        double logSum = 0, arith = 0; int cnt = 0;
        for (int k = fl0; k <= fl1; ++k) { const double p = pw[(size_t) k] + 1e-18 * tot + 1e-30; logSum += std::log (p); arith += p; ++cnt; }
        const double flat = cnt > 0 ? std::exp (logSum / cnt) / (arith / cnt) : 0.0;
        // autocorrelation = inverse FFT of the power spectrum
        big.inverse (pw.data(), zeroI.data(), ac.data());
        const double r0 = std::max (1e-30, ac[0]);
        int t = lagMin; while (t < lagMax && ac[(size_t) t] > 0.0) ++t;   // leave the lobe around lag 0
        double best = 0.0;
        for (; t <= lagMax; ++t) best = std::max (best, ac[(size_t) t] / r0 / std::max (0.05, winAc[(size_t) t]));
        best = std::max (0.0, std::min (1.0, best));
        sW += 1.0;
        sCent += cd > 0 ? cn / cd / M * sr : 0.0;
        sFlat += flat; sLow += tot > 0 ? low / tot : 0.0;
        sCrest += 20.0 * std::log10 (std::max (1e-9, pk) / std::sqrt (em / N + 1e-30));
        sHarm += best; sWidth += (em + es) > 0 ? es / (em + es) : 0.0;
    }

    static constexpr int kMaxBands = 32;
    double sr = 48000.0; int N = 4096, M = 8192, F = 1024, hop = 2048, hopF = 256, refractoryHops = 12, lagMin = 48, lagMax = 800, binLow = 0, fl0 = 0, fl1 = 0, c0 = 1;
    double hopSec = 0.0427, decay = 0.998, decayS = 0.99, meanCoef = 0.01, sOnS = 0, sTS = 0;
    RealFft big, small;
    std::vector<double> ringM, ringS, win, winAc, winF, xin, ac, re, im, pw, zeroI, xinF, reF, imF;
    int nBands = 0, bandLo[kMaxBands] = {}, bandHi[kMaxBands] = {}; double prevBand[kMaxBands] = {}, curBand[kMaxBands] = {};
    int wpos = 0, sinceHop = 0, sinceHopF = 0; uint64_t seen = 0; double elapsed = 0.0;
    double sW = 0, sT = 0, sOn = 0, sCent = 0, sFlat = 0, sLow = 0, sCrest = 0, sHarm = 0, sWidth = 0;
    double fluxMean = 0, flux1 = 0, flux2 = 0; int cool = 0, haveFlux = 0;
};

inline void SourceSense::classify (Result& res)
{
    using namespace rules;
    const Features& f = res.f;
    const double x[kNumFeatures] = { std::log2 (std::max (1.0, f.centroidHz)), std::log10 (f.flatness + 1e-5), f.lowShare, std::log2 (f.onsetRate + 0.25),
                                     f.crestDb, f.harmonicity, std::log10 (f.width + 0.003), f.duty };
    double z2[Unsure], ll[Unsure]; int best = 0;
    for (int c = 0; c < Unsure; ++c)
    {
        double s = 0.0;
        for (int k = 0; k < kNumFeatures; ++k)
        {
            const double sd = std::max (kClassFit[c][k].sigma, kSigmaFloor[k]);
            double z = (x[k] - kClassFit[c][k].mu) / sd;
            if (k == kWidthFeature && z < 0.0) z = 0.0;   // P1c: width is one-sided. A source can always be folded to mono or panned to the centre (a mono stem of any class), so a width BELOW the class mean is no evidence against it; only a width above what the class can have counts
            s += z * z;
        }
        z2[c] = s / kNumFeatures; ll[c] = -0.5 * s / kTemper;
        if (ll[c] > ll[best]) best = c;
    }
    double sum = 0.0; for (int c = 0; c < Unsure; ++c) sum += std::exp (ll[c] - ll[best]);
    const double fit = z2[best] <= kFitFull ? 1.0 : (z2[best] >= kFitNone ? 0.0 : (kFitNone - z2[best]) / (kFitNone - kFitFull));
    for (int c = 0; c < Unsure; ++c) res.score[c] = std::exp (ll[c] - ll[best]) / sum * (c == best ? fit : 0.0);
    res.score[Unsure] = 0.0;
    res.confidence = res.score[best];
    res.cls = (res.ready && res.confidence >= kMinConfidence) ? (Class) best : Unsure;
}
}
}
