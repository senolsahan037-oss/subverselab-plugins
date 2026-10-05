#if defined (__APPLE__)
 #include <Accelerate/Accelerate.h>   // before JUCE, whose using-directive would make Point ambiguous
#endif
#include "RtChain.h"
#include "../dsp/Equalizer.h"
#include "../dsp/Dynamics.h"
#include "../dsp/Loudness.h"
#include "../dsp/Filters.h"
#include <array>
#include <functional>

namespace mihenk
{
RtPlan makePlan (const MasterSettings& s, const RenderResult& r, double fs)
{
    RtPlan p;
    p.valid = r.output != nullptr && ! s.bypass;
    p.sampleRate = fs;
    p.learnedSamples = r.output != nullptr ? r.output->getNumSamples() : 0;
    p.autoTrimDb = r.input.autoTrim ? r.input.autoTrimDb : 0.0;
    p.sourceValid = r.density.valid;
    p.denseWide = r.density.valid && r.density.dense;
    p.densePeak = r.density.valid && (r.density.plr < kDensePlrLu || r.density.crest < kDenseCrestDb);
    if (r.glue.valid || s.comp.enabled)
    {
        if (r.glue.threeBand)
        {
            for (int b = 0; b < 3; ++b)
            {
                p.bandActive[b] = r.glue.bands[b].active;
                p.bandMakeupDb[b] = r.glue.bands[b].makeupDb;
            }
            p.bandFinalMakeupDb = r.glue.makeupDb;
        }
        else
            p.wideMakeupDb = r.glue.makeupDb;
    }
    const DynBandResult* dyn[3] { &r.dynEq.tightener, &r.dynEq.lowMid, &r.dynEq.upperMid };
    for (int i = 0; i < 3; ++i)
    {
        p.dynActive[i] = dyn[i]->active;
        p.dynThresholdDb[i] = dyn[i]->thresholdDb;
    }
    p.airActive = r.dynEq.air.active;
    p.airTypicalRelDb = r.dynEq.air.typicalRelDb;
    p.safeSign = r.width.safeSign;
    p.safeGain = r.width.safeGain;
    return p;
}

void sanitisePlan (RtPlan& p) noexcept
{
    // A value outside what a real analysis can produce marks the whole plan as corrupt: it is dropped (valid = false,
    // the chain then runs without learned values) and every number is clamped so nothing non-finite can reach the audio.
    bool ok = true;
    auto fin = [&ok] (double v, double lo, double hi, double fb)
    {
        if (! std::isfinite (v)) { ok = false; return fb; }
        if (v < lo || v > hi) ok = false;
        return juce::jlimit (lo, hi, v);
    };
    p.sampleRate = fin (p.sampleRate, 0.0, 768000.0, 0.0);
    p.autoTrimDb = fin (p.autoTrimDb, -kMaxAutoTrimDb, kMaxAutoTrimDb, 0.0);
    p.wideMakeupDb = fin (p.wideMakeupDb, -24.0, 24.0, 0.0);
    p.bandFinalMakeupDb = fin (p.bandFinalMakeupDb, -24.0, 24.0, 0.0);
    for (int i = 0; i < 3; ++i)
    {
        p.bandMakeupDb[i] = fin (p.bandMakeupDb[i], -24.0, 24.0, 0.0);
        p.dynThresholdDb[i] = fin (p.dynThresholdDb[i], -120.0, 24.0, 0.0);
    }
    p.airTypicalRelDb = fin (p.airTypicalRelDb, -120.0, 120.0, 0.0);
    p.safeGain = fin (p.safeGain, 0.25, 4.0, 1.0);
    if (! std::isfinite (p.safeSign) || std::abs (std::abs (p.safeSign) - 1.0) > 1.0e-6) { ok = false; p.safeSign = 1.0; }
    p.learnedSamples = juce::jmax ((juce::int64) 0, p.learnedSamples);
    if (! ok) p.valid = false;
}

namespace
{
constexpr int B = RtChain::blockSize;
constexpr int N = 2 * B;                 // FFT size of the partitioned convolution
constexpr int bins = N / 2 + 1;
constexpr int specFloats = 2 * bins;     // split complex: re[bins], im[bins]
constexpr int kFadeBlocks = 4;           // kernel / configuration crossfade, about 21 ms at 48 kHz
constexpr double kTailTolerance = 1.0e-5;
constexpr double kMinXoverLowHz = 40.0, kMinXoverHighHz = 2000.0, kMinMonoHz = 40.0, kMinHighWidthHz = 1000.0;
constexpr double kMaxLookaheadMs = 5.0;
constexpr int kTpHistory = TruePeakDetector::taps - 1;    // 63 samples of history for the 8x interpolator

int nextPow2 (int v) { int p = 1; while (p < v) p <<= 1; return p; }
double coefMs (double ms, double fs) { return ms <= 0.0 ? 0.0 : std::exp (-1.0 / (ms * 1.0e-3 * fs)); }

//==============================================================================
class Fft
{
public:
    Fft() : fft (juce::roundToInt (std::log2 ((double) N)))
    {
        std::vector<float> d ((size_t) 2 * N, 0.0f);
        d[0] = 1.0f;
        fft.performRealOnlyForwardTransform (d.data(), true);
        mirror (d.data());
        fft.performRealOnlyInverseTransform (d.data());
        scale = d[0] != 0.0f ? 1.0f / d[0] : 1.0f;
    }
    /** d: 2N floats, first N real input. Out: split complex (re[bins], im[bins]) into `split`. */
    void forward (float* d, float* split) const
    {
        fft.performRealOnlyForwardTransform (d, true);
        for (int k = 0; k < bins; ++k) { split[k] = d[2 * k]; split[bins + k] = d[2 * k + 1]; }
    }
    /** split complex in, N real samples out (in d, 2N floats of work space). */
    void inverse (const float* split, float* d) const
    {
        for (int k = 0; k < bins; ++k) { d[2 * k] = split[k]; d[2 * k + 1] = split[bins + k]; }
        mirror (d);
        fft.performRealOnlyInverseTransform (d);
        juce::FloatVectorOperations::multiply (d, scale, N);
    }

private:
    static void mirror (float* d)
    {
        for (int k = 1; k < N / 2; ++k) { d[2 * (N - k)] = d[2 * k]; d[2 * (N - k) + 1] = -d[2 * k + 1]; }
    }
    juce::dsp::FFT fft;
    float scale = 1.0f;
};

struct KernelSpectra
{
    int parts = 0;
    std::vector<float> spec;     // parts * specFloats
    static std::unique_ptr<KernelSpectra> make (const float* taps, int len, const Fft& fft)
    {
        auto k = std::make_unique<KernelSpectra>();
        k->parts = juce::jmax (1, (len + B - 1) / B);
        k->spec.assign ((size_t) k->parts * specFloats, 0.0f);
        std::vector<float> buf ((size_t) 2 * N);
        for (int p = 0; p < k->parts; ++p)
        {
            std::fill (buf.begin(), buf.end(), 0.0f);
            for (int i = 0; i < B && p * B + i < len; ++i) buf[(size_t) i] = taps[p * B + i];
            fft.forward (buf.data(), k->spec.data() + (size_t) p * specFloats);
        }
        return k;
    }
};

/** A set of kernels a stage swaps at once. dry: the stage passes its input (delayed by `latency`). */
struct KernelSet
{
    bool dry = true;
    int latency = 0;
    std::unique_ptr<KernelSpectra> k[2];
};

/** Frequency-domain delay line of one input stream. */
struct Fdl
{
    int maxParts = 1, head = 0;
    std::vector<float> spec, prev;
    void prepare (int parts) { maxParts = juce::jmax (1, parts); spec.assign ((size_t) maxParts * specFloats, 0.0f); prev.assign ((size_t) B, 0.0f); head = 0; }
    void clear() { std::fill (spec.begin(), spec.end(), 0.0f); std::fill (prev.begin(), prev.end(), 0.0f); head = 0; }
    void push (const float* x, const Fft& fft, float* work)
    {
        std::copy (prev.begin(), prev.end(), work);
        std::copy (x, x + B, work + B);
        std::fill (work + N, work + 2 * N, 0.0f);
        head = (head + 1) % maxParts;
        fft.forward (work, spec.data() + (size_t) head * specFloats);
        std::copy (x, x + B, prev.begin());
    }
    const float* at (int p) const
    {
        int i = head - p;
        if (i < 0) i += maxParts;
        return spec.data() + (size_t) i * specFloats;
    }
};

/** out[B] = the kernel applied to the stream (overlap-save, zero added latency inside a block). */
void convolve (const Fdl& f, const KernelSpectra& k, const Fft& fft, float* acc, float* work, float* out, int shiftParts = 0)
{
    // shiftParts: the kernel starts that many whole blocks later, i.e. the output is delayed by shiftParts * B (exactly, no truncation).
    std::fill (acc, acc + specFloats, 0.0f);
    float* aRe = acc;
    float* aIm = acc + bins;
    const int parts = juce::jmin (k.parts, f.maxParts - shiftParts);
    for (int p = 0; p < parts; ++p)
    {
        const float* x = f.at (p + shiftParts);
        const float* h = k.spec.data() + (size_t) p * specFloats;
        const float* xRe = x; const float* xIm = x + bins;
        const float* hRe = h; const float* hIm = h + bins;
        for (int b = 0; b < bins; ++b)
        {
            aRe[b] += xRe[b] * hRe[b] - xIm[b] * hIm[b];
            aIm[b] += xRe[b] * hIm[b] + xIm[b] * hRe[b];
        }
    }
    fft.inverse (acc, work);
    std::copy (work + B, work + N, out);
}

//==============================================================================
struct Ring
{
    std::vector<float> d;
    int mask = 0, w = 0;
    void prepare (int maxDelay) { const int s = nextPow2 (maxDelay + B + 2); d.assign ((size_t) s, 0.0f); mask = s - 1; w = 0; }
    void clear() { std::fill (d.begin(), d.end(), 0.0f); w = 0; }
    inline void push (float v) noexcept { d[(size_t) w] = v; w = (w + 1) & mask; }
    /** delay 0 = the value just pushed. */
    inline float get (int delay) const noexcept { return d[(size_t) ((w - 1 - delay) & mask)]; }
};

struct Ramp
{
    double v = 0.0, target = 0.0, step = 0.0;
    int left = 0;
    void set (double t, int len, bool snap) noexcept
    {
        if (! std::isfinite (t)) return;               // a poisoned target never reaches the audio
        if (snap || len <= 0) { v = target = t; left = 0; step = 0.0; return; }
        if (std::equal_to<double>{} (t, target)) return;
        target = t;
        left = len;
        step = (target - v) / len;
    }
    inline double next() noexcept
    {
        if (left > 0) { v += step; if (--left == 0) v = target; }
        return v;
    }
    bool moving() const noexcept { return left > 0; }
};

//==============================================================================
/** The glue compressor's detector and gain computer as a stream (same arithmetic as compressorDetector /
    compressorGainDb). */
struct CompCore
{
    Biquad hp[2];
    double hpHz = -1.0;
    bool useHp = false;
    double blend = 0.7, aRms = 0.0, aPk = 0.0;
    double ms = 0.0, pkEnv = 0.0;
    double env = 0.0, yS = 0.0, yF = 0.0;
    // gain computer
    double thr = 0.0, slope = 0.0, knee = 0.0, maxGr = 0.0, aA = 0.0, aR = 0.0, aAs = 0.0, aRs = 0.0, aRf = 0.0;
    bool autoRel = false;

    void clear() { ms = pkEnv = env = yS = yF = 0.0; for (auto& h : hp) h.reset(); }
    void setDetector (double fs, double scHpfHz, double rmsBlend)
    {
        useHp = scHpfHz > 20.0;
        const double f = juce::jmax (20.0, scHpfHz);
        if (! std::equal_to<double>{} (f, hpHz))
        {
            hpHz = f;
            for (auto& h : hp) { const double z1 = h.z1, z2 = h.z2; h = Biquad::butterworthHighPass (fs, f); h.z1 = z1; h.z2 = z2; }
        }
        blend = juce::jlimit (0.0, 1.0, rmsBlend);
        aRms = coefMs (25.0, fs);
        aPk = coefMs (50.0, fs);
    }
    void setGain (double fs, double ratio, double kneeDb, double attackMs, double releaseMs, bool autoRelease, double maxGrDb)
    {
        const double r = juce::jlimit (1.0, 20.0, ratio);
        slope = 1.0 / r - 1.0;
        knee = juce::jmax (0.0, kneeDb);
        maxGr = maxGrDb;
        autoRel = autoRelease;
        aA = coefMs (attackMs, fs); aR = coefMs (releaseMs, fs);
        aAs = coefMs (attackMs * 3.0, fs); aRs = coefMs (releaseMs * 4.0, fs); aRf = coefMs (releaseMs / 3.0, fs);
    }
    inline float level (float x0, float x1, int chans) noexcept
    {
        double pk = 0.0, sq = 0.0;
        double x = x0;
        if (useHp) x = hp[0].process (x);
        pk = std::abs (x);
        sq = x * x;
        if (chans > 1)
        {
            double y = x1;
            if (useHp) y = hp[1].process (y);
            pk = juce::jmax (pk, std::abs (y));
            sq += y * y;
        }
        sq /= chans;
        ms = aRms * ms + (1.0 - aRms) * sq;
        pkEnv = juce::jmax (pk, aPk * pkEnv);
        const double peakDb = 20.0 * std::log10 (pkEnv + 1.0e-12);
        const double rmsDb = 10.0 * std::log10 (ms + 1.0e-24) + 3.0103;
        return (float) ((1.0 - blend) * peakDb + blend * rmsDb);
    }
    inline float gain (float levelDb) noexcept
    {
        const double over = (double) levelDb - thr;
        double gr;
        if (2.0 * over <= -knee) gr = 0.0;
        else if (knee > 0.0 && 2.0 * std::abs (over) <= knee) gr = slope * (over + knee * 0.5) * (over + knee * 0.5) / (2.0 * knee);
        else gr = slope * over;
        if (maxGr > 0.0) gr = juce::jmax (gr, -maxGr);
        if (autoRel)
        {
            yS = gr < yS ? aAs * yS + (1.0 - aAs) * gr : aRs * yS + (1.0 - aRs) * gr;
            const double res = juce::jmin (0.0, gr - yS);
            yF = res < yF ? aA * yF + (1.0 - aA) * res : aRf * yF + (1.0 - aRf) * res;
            env = yS + yF;
        }
        else
            env = gr < env ? aA * env + (1.0 - aA) * gr : aR * env + (1.0 - aR) * gr;
        return (float) env;
    }
};

//==============================================================================
/** The 8x true-peak detector's coefficients, built exactly as TruePeakDetector builds them. */
std::vector<float> tpCoefficients (int factor)
{
    constexpr int taps = TruePeakDetector::taps;
    std::vector<float> coeff ((size_t) (factor * taps));
    const double half = taps / 2.0, beta = 9.0;
    for (int p = 0; p < factor; ++p)
    {
        double sum = 0.0;
        double tmp[taps];
        for (int j = 0; j < taps; ++j)
        {
            const double t = (double) p / factor + half - 1.0 - (double) j;
            const double sinc = std::abs (t) < 1.0e-12 ? 1.0 : std::sin (kPi * t) / (kPi * t);
            tmp[j] = sinc * kaiser (t / half, beta);
            sum += tmp[j];
        }
        for (int j = 0; j < taps; ++j)
            coeff[(size_t) (p * taps + j)] = (float) (tmp[j] / sum);
    }
    return coeff;
}

/** Streaming form of TruePeakDetector::envelope (same coefficients, same vDSP calls): on a window
    w = [x(e0 - 31) .. x(e0 + n + 31)] it gives out[i] = max over the 8 phases of |y(e0 + i + p/8)|. */
struct StreamTp
{
    int factor = kLimiterOversampling;
    std::vector<float> coeff = tpCoefficients (kLimiterOversampling);
    void envelope (const float* w, int n, float* out, float* scratch) const
    {
        constexpr int taps = TruePeakDetector::taps;
#if defined (__APPLE__)
        vDSP_vabs (w + (taps / 2 - 1), 1, out, 1, (vDSP_Length) n);
        for (int p = 1; p < factor; ++p)
        {
            vDSP_conv (w, 1, coeff.data() + p * taps, 1, scratch, 1, (vDSP_Length) n, (vDSP_Length) taps);
            vDSP_vmaxmg (scratch, 1, out, 1, out, 1, (vDSP_Length) n);
        }
#else
        for (int i = 0; i < n; ++i)
        {
            float m = std::abs (w[i + taps / 2 - 1]);
            for (int p = 1; p < factor; ++p)
            {
                const float* c = coeff.data() + p * taps;
                float acc = 0.0f;
                for (int j = 0; j < taps; ++j) acc += c[j] * w[i + j];
                m = juce::jmax (m, std::abs (acc));
            }
            out[i] = m;
        }
        (void) scratch;
#endif
    }
};

//==============================================================================
/** One pass of the true-peak limiter as a stream: the offline limiterGainCurve (look-ahead minimum over L,
    release, box average over L) on the 8x true-peak envelope of its input. Fixed delay D = 32 + Lmax - 1. */
struct LimPass
{
    int chans = 2, L = 1, Lmax = 1, D = 1;
    double rel = 0.0;
    std::vector<float> win[2];               // kTpHistory + B
    Ring sig[2];
    std::vector<float> rq, hr, gain, env, envB, ph;
    int rqMask = 0, hrMask = 0, gMask = 0;
    std::vector<juce::int64> dq;
    int dqMask = 0, dqHead = 0, dqTail = 0;   // [head, tail)
    juce::int64 t = 0, hIndex = 0;
    bool started = false;
    float before = 1.0f, prev = 1.0f;
    double sum = 0.0;

    void prepare (int numChannels, int lmax)
    {
        chans = numChannels;
        Lmax = lmax;
        D = 32 + Lmax - 1;
        for (auto& w : win) w.assign ((size_t) (kTpHistory + B), 0.0f);
        for (auto& s : sig) s.prepare (D + 4);
        const int rs = nextPow2 (Lmax + 2 * B + 128);
        rq.assign ((size_t) rs, 1.0f); rqMask = rs - 1;
        const int hs = nextPow2 (Lmax + 8);
        hr.assign ((size_t) hs, 1.0f); hrMask = hs - 1;
        const int gs = nextPow2 (8 * D + 4 * B + 64);
        gain.assign ((size_t) gs, 1.0f); gMask = gs - 1;
        const int ds = nextPow2 (Lmax + 2 * B + 128);
        dq.assign ((size_t) ds, 0); dqMask = ds - 1;
        env.assign ((size_t) B, 0.0f);
        envB.assign ((size_t) B, 0.0f);
        ph.assign ((size_t) B, 0.0f);
        clear();
    }
    void clear()
    {
        for (auto& w : win) std::fill (w.begin(), w.end(), 0.0f);
        for (auto& s : sig) s.clear();
        std::fill (rq.begin(), rq.end(), 1.0f);
        std::fill (hr.begin(), hr.end(), 1.0f);
        std::fill (gain.begin(), gain.end(), 1.0f);
        dqHead = dqTail = 0;
        t = 0; hIndex = 0;
        started = false;
        before = prev = 1.0f;
        sum = 0.0;
    }
    void setParams (int lookahead, double releaseCoef)
    {
        lookahead = juce::jlimit (1, Lmax, lookahead);
        if (lookahead != L)
        {
            // A new look-ahead restarts the gain pipeline from here (a knob move, not automation-smooth).
            L = lookahead;
            dqHead = dqTail = 0;
            started = false;
            hIndex = juce::jmax ((juce::int64) 0, t - 32);
        }
        rel = releaseCoef;
    }
    float gainAt (juce::int64 index) const noexcept { return index >= 0 ? gain[(size_t) (index & gMask)] : 1.0f; }

    /** x: B input samples per channel (in place: becomes the pass output, delayed by D). ceiling per sample. */
    void process (float* const* x, const double* ceiling, const StreamTp& tp)
    {
        // 1. 8x true-peak envelope of indices t-32 .. t-32+B-1 (window = [x(t-63) .. x(t+B-1)]), max over channels.
        for (int c = 0; c < chans; ++c)
        {
            auto& w = win[c];
            std::copy (x[c], x[c] + B, w.begin() + kTpHistory);
            tp.envelope (w.data(), B, c == 0 ? env.data() : envB.data(), ph.data());
            if (c > 0)
                for (int i = 0; i < B; ++i) env[(size_t) i] = juce::jmax (env[(size_t) i], envB[(size_t) i]);
        }
        // 2. Required gain, look-ahead minimum, release, box average.
        for (int i = 0; i < B; ++i)
        {
            const juce::int64 e = t - 32 + i;
            if (e < 0) continue;
            const double v = (double) env[(size_t) i];
            const double ceil = ceiling[i];
            const float r = v > ceil ? (float) (ceil / v) : 1.0f;
            rq[(size_t) (e & rqMask)] = r;
            while (dqTail != dqHead && rq[(size_t) (dq[(size_t) ((dqTail - 1) & dqMask)] & rqMask)] >= r) dqTail = (dqTail - 1) & dqMask;
            dq[(size_t) dqTail] = e;
            dqTail = (dqTail + 1) & dqMask;
            const juce::int64 j = e - (L - 1);
            if (j < hIndex) continue;
            while (dq[(size_t) dqHead] < j) dqHead = (dqHead + 1) & dqMask;
            const float h = rq[(size_t) (dq[(size_t) dqHead] & rqMask)];
            if (! started)
            {
                before = prev = h;
                sum = (double) L * before;
                for (auto& v2 : hr) v2 = before;
                started = true;
                hIndex = j;
            }
            prev = h < prev ? h : (float) (h + (prev - h) * rel);
            const juce::int64 k0 = j - hIndex;                      // index since the pipeline (re)started
            hr[(size_t) (j & hrMask)] = prev;
            const float leaving = k0 >= L ? hr[(size_t) ((j - L) & hrMask)] : before;
            sum += (double) prev - (double) leaving;
            if ((k0 & 4095) == 4095)
            {
                sum = 0.0;
                for (juce::int64 q = j - L + 1; q <= j; ++q) sum += q - hIndex >= 0 ? (double) hr[(size_t) (q & hrMask)] : (double) before;
            }
            gain[(size_t) (j & gMask)] = (float) juce::jmin (1.0, sum / L);
        }
        // 3. Output = input delayed by D times its gain.
        for (int i = 0; i < B; ++i)
        {
            const juce::int64 o = t + i - D;
            const float g = gainAt (o);
            for (int c = 0; c < chans; ++c)
            {
                sig[c].push (x[c][i]);
                x[c][i] = sig[c].get (D) * g;
            }
        }
        for (int c = 0; c < chans; ++c)
            std::copy (win[c].begin() + B, win[c].end(), win[c].begin());
        t += B;
    }
};
} // namespace

//==============================================================================
struct RtChain::Impl
{
    //--------------------------------------------------------------------------
    // Configuration (prepare)
    double fs = 48000.0;
    int chans = 2, maxHost = 512;
    Fft fft;
    StreamTp tp;
    int eqTaps = 16384, eqCentre = 8192;
    int Mb = 0, Dw = 0, Dair = 0, Lmax = 1, Dp = 1;

    //--------------------------------------------------------------------------
    // Kernel slots
    struct Slot
    {
        KernelSet* cur = nullptr;
        KernelSet* old = nullptr;
        int fade = 0;
        bool primed = false;
        std::atomic<KernelSet*> ready { nullptr };
        std::atomic<uint64_t> wanted { 0 };
        uint64_t requested = 0;      // audio side
        uint64_t built = 0;          // worker side
        juce::SpinLock lock;         // guards the request payload
    };
    enum SlotId { sEq = 0, sBands, sWidth, sAir, numSlots };
    Slot slots[numSlots];
    struct EqRequest { EqSettings eq; };
    EqRequest eqReq;
    double bandReq[2] { 120.0, 4000.0 }, widthReq[2] { 120.0, 4000.0 }, airReq = 20000.0;
    std::unique_ptr<KernelSpectra> harsh;          // fixed: the 5-9 kHz band of the de-harsh detector

    juce::SpinLock curveLock;
    std::vector<float> curve;
    std::atomic<uint32_t> curveVersion { 0 };
    std::atomic<bool> curveNonZero { false };

    // Retired kernel sets: the audio thread hands them to the worker, which deletes them.
    juce::AbstractFifo retireFifo { 128 };
    std::array<KernelSet*, 128> retireBuf {};
    std::atomic<bool> synchronous { false };

    struct Worker : juce::Thread
    {
        explicit Worker (Impl& i) : juce::Thread ("MIHENK kernels"), impl (i) {}
        void run() override { while (! threadShouldExit()) { impl.service(); wait (3); } }
        Impl& impl;
    };
    std::unique_ptr<Worker> worker;

    //--------------------------------------------------------------------------
    // Streaming state
    Fdl eqFdl[2], harshFdl, airFdl[2], compFdl[2], sideFdl, lFdl, rFdl;
    Ring eqDry[2], airY[2], airSum, compX[2], widthL, widthR, tapIn[2], tapEq[2], tapComp[2], grRing[4];
    Biquad dcHp[2], airBell[2];
    CompCore wide, band[3], dyn[3];
    Biquad dynC[3][2];
    double dynFreq[3] { -1, -1, -1 }, dynQ[3] { -1, -1, -1 };
    int dynShape[3] { -1, -1, -1 };
    double airEb = 0.0, airEr = 0.0, airEnv = 0.0;
    LimPass lim[4];
    std::vector<float> work, acc, tmpA, tmpB, tmpC, tmpD, tmpE, tmpF;
    std::vector<float> blk[2], inCopy[2], eqOut[2], compOut[2];
    std::vector<float> grWide, grBand[3];
    std::vector<double> ceilBlk;
    std::vector<float> frames;

    // Host framing
    std::vector<float> inRing[2], outRing[2];
    int inCount = 0, outCount = 0, outRead = 0, outWrite = 0, ringMask = 0;

    // Parameters
    Ramp safeGate, monoGate, dynGate[3], airGate, balL, balR, trim, compThr, compMakeup, compMix, bandThr[3], bandGain[3], dynThr[3], width, highWidth, drive, ceiling, airAmt, out, ab;
    bool snap = true;
    RtPlan plan;
    MeterFifo* meters = nullptr;
    std::atomic<bool> listenInput { false };
    std::atomic<float> listenGain { 1.0f };
    int prevCompConfig = -1, compFade = kFadeBlocks, compFromConfig = 0;
    std::atomic<float> lastCompGr { 0.0f }, lastLimGr { 0.0f };
    int eqLatencyNow = 0;

    // The delaying stages are always in: every stage that is switched out passes its input delayed by the same amount, so the
    // latency is one fixed number for the sample rate (stereo: with the split) and no switch or knob ever changes it. A switch
    // then only changes what a stage does to a signal that is already time-aligned, which the kernel and configuration
    // crossfades make click-free.
    struct Structure
    {
        bool linear = true, air = true, bands = true, split = true;
    };
    Structure cur;
    std::atomic<int> latencyNow { 0 };

    Structure structureFor (const MasterSettings&) const noexcept
    {
        Structure st;
        st.split = chans > 1;
        return st;
    }
    int latencyFor (const Structure& st) const noexcept
    {
        return B + (st.linear ? eqCentre : 0) + (st.air ? Dair : 0) + (st.bands ? Mb : 0) + (st.split ? Dw : 0) + 4 * Dp;
    }

    //--------------------------------------------------------------------------
    ~Impl()
    {
        if (worker != nullptr) worker->stopThread (2000);
        for (auto& s : slots)
        {
            delete s.cur; delete s.old; delete s.ready.exchange (nullptr);
            s.cur = s.old = nullptr;
        }
        drainRetired();
    }

    void drainRetired()
    {
        int s1, n1, s2, n2;
        retireFifo.prepareToRead (retireFifo.getNumReady(), s1, n1, s2, n2);
        for (int i = 0; i < n1; ++i) delete retireBuf[(size_t) (s1 + i)];
        for (int i = 0; i < n2; ++i) delete retireBuf[(size_t) (s2 + i)];
        retireFifo.finishedRead (n1 + n2);
    }

    bool retire (KernelSet* k) noexcept
    {
        if (k == nullptr) return true;
        if (synchronous.load()) { delete k; return true; }
        int s1, n1, s2, n2;
        retireFifo.prepareToWrite (1, s1, n1, s2, n2);
        if (n1 + n2 == 0) return false;
        retireBuf[(size_t) (n1 > 0 ? s1 : s2)] = k;
        retireFifo.finishedWrite (1);
        return true;
    }

    //--------------------------------------------------------------------------
    // Zero-phase kernels: the impulse response of forward-backward 2nd-order Butterworth low-pass filtering,
    // which is the autocorrelation of the Butterworth impulse response.
    std::vector<double> zeroPhaseKernel (double fc, int half) const
    {
        fc = juce::jlimit (1.0, fs * 0.45, fc);
        auto bq = Biquad::butterworthLowPass (fs, fc);
        const int T = 3 * half + 4096;
        const int F = nextPow2 (2 * T);
        std::vector<std::complex<double>> x ((size_t) F);
        x[0] = bq.process (1.0);
        for (int i = 1; i < T; ++i) x[(size_t) i] = bq.process (0.0);
        fftInPlace (x, false);
        for (auto& c : x) c = std::norm (c);
        fftInPlace (x, true);
        std::vector<double> r ((size_t) (2 * half + 1));
        for (int k = -half; k <= half; ++k) r[(size_t) (k + half)] = x[(size_t) ((k + F) % F)].real();
        return r;
    }

    /** Half length at which the kernel's tail (both sides) holds less than kTailTolerance of its absolute area. */
    int halfLength (double fc) const
    {
        const int guess = juce::jmax (64, (int) std::ceil (12.0 / juce::jlimit (1.0, fs * 0.45, fc) * fs));
        const auto r = zeroPhaseKernel (fc, guess);
        double total = 0.0;
        for (double v : r) total += std::abs (v);
        double tail = 0.0;
        for (int k = guess; k >= 1; --k)
        {
            tail += std::abs (r[(size_t) (guess + k)]) + std::abs (r[(size_t) (guess - k)]);
            if (tail > kTailTolerance * total) return k;
        }
        return 1;
    }

    static std::vector<double> convolveSym (const std::vector<double>& a, const std::vector<double>& b)
    {
        std::vector<double> y (a.size() + b.size() - 1, 0.0);
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i] != 0.0)
                for (size_t j = 0; j < b.size(); ++j) y[i + j] += a[i] * b[j];
        return y;
    }

    /** Centre-aligned copy of a symmetric kernel into a (2 * half + 1)-tap float kernel (truncated or padded). */
    static std::vector<float> centred (const std::vector<double>& k, int half)
    {
        std::vector<float> out ((size_t) (2 * half + 1), 0.0f);
        const int kh = ((int) k.size() - 1) / 2;
        for (int i = -half; i <= half; ++i)
            if (i >= -kh && i <= kh) out[(size_t) (i + half)] = (float) k[(size_t) (i + kh)];
        return out;
    }

    //--------------------------------------------------------------------------
    // Kernel design (worker thread, or inline in synchronous mode / prime()).
    KernelSet* buildEq (const EqSettings& req, uint64_t key)
    {
        auto* set = new KernelSet();
        const bool linear = req.linearPhase;
        set->latency = linear ? eqCentre : 0;
        if ((key & 1u) != 0u) return set;                       // dry
        EqSettings e = req;
        {
            const juce::SpinLock::ScopedLockType sl (curveLock);
            e.matchCurveDb = curve;
        }
        const auto k = designEqFir (e, fs);
        set->dry = false;
        set->latency = k.latency;
        set->k[0] = KernelSpectra::make (k.taps.data(), (int) k.taps.size(), fft);
        return set;
    }

    KernelSet* buildBands (double f1, double f2)
    {
        auto* set = new KernelSet();
        set->dry = false;
        set->latency = Mb;
        const int h2 = halfLength (juce::jmax (f2, kMinXoverHighHz));
        const auto r1 = zeroPhaseKernel (f1, Mb);
        const auto r2 = zeroPhaseKernel (f2, h2);
        auto r21 = convolveSym (r2, r1);                    // half Mb + h2
        const int h21 = ((int) r21.size() - 1) / 2;
        for (int i = -h2; i <= h2; ++i) r21[(size_t) (i + h21)] = r2[(size_t) (i + h2)] - r21[(size_t) (i + h21)];
        for (int i = -h21; i <= h21; ++i) if (std::abs (i) > h2) r21[(size_t) (i + h21)] = -r21[(size_t) (i + h21)];
        const auto low = centred (r1, Mb), mid = centred (r21, Mb);
        set->k[0] = KernelSpectra::make (low.data(), (int) low.size(), fft);
        set->k[1] = KernelSpectra::make (mid.data(), (int) mid.size(), fft);
        return set;
    }

    KernelSet* buildWidth (double monoHz, double highHz)
    {
        auto* set = new KernelSet();
        set->dry = false;
        set->latency = Dw;
        const auto a = centred (zeroPhaseKernel (juce::jmax (kMinMonoHz, monoHz), Dw), Dw);
        const auto b = centred (zeroPhaseKernel (juce::jlimit (kMinHighWidthHz, fs * 0.45, highHz), Dw), Dw);
        set->k[0] = KernelSpectra::make (a.data(), (int) a.size(), fft);
        set->k[1] = KernelSpectra::make (b.data(), (int) b.size(), fft);
        return set;
    }

    KernelSet* buildAir (double hz)
    {
        auto* set = new KernelSet();
        set->dry = false;
        set->latency = Dair;
        const auto p = airPKernel (hz, fs);
        std::vector<float> t ((size_t) (2 * Dair + 1), 0.0f);
        for (int i = -Dair; i <= Dair; ++i)
        {
            const int n = p.latency + i;
            if (n >= 0 && n < (int) p.taps.size()) t[(size_t) (i + Dair)] = p.taps[(size_t) n];
        }
        set->k[0] = KernelSpectra::make (t.data(), (int) t.size(), fft);
        return set;
    }

    KernelSet* build (int id)
    {
        auto& s = slots[id];
        uint64_t key = 0;
        EqSettings eq;
        double a = 0.0, b = 0.0;
        {
            const juce::SpinLock::ScopedLockType sl (s.lock);
            key = s.wanted.load();
            if (id == sEq) eq = eqReq.eq;
            else if (id == sBands) { a = bandReq[0]; b = bandReq[1]; }
            else if (id == sWidth) { a = widthReq[0]; b = widthReq[1]; }
            else a = airReq;
        }
        KernelSet* k = nullptr;
        switch (id)
        {
            case sEq:    k = buildEq (eq, key); break;
            case sBands: k = buildBands (a, b); break;
            case sWidth: k = buildWidth (a, b); break;
            default:     k = buildAir (a); break;
        }
        s.built = key;
        return k;
    }

    /** Worker: build what is wanted, delete what was retired. */
    void service()
    {
        if (synchronous.load()) { drainRetired(); return; }
        for (int id = 0; id < numSlots; ++id)
        {
            auto& s = slots[id];
            if (s.wanted.load() != s.built && s.ready.load() == nullptr)
            {
                auto* k = build (id);
                KernelSet* expected = nullptr;
                if (! s.ready.compare_exchange_strong (expected, k)) delete k;
            }
        }
        drainRetired();
    }

    //--------------------------------------------------------------------------
    static uint64_t mix (uint64_t h, double v) noexcept
    {
        uint64_t bits;
        std::memcpy (&bits, &v, sizeof bits);
        h ^= bits + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        return h;
    }

    uint64_t eqKey (const EqSettings& e) const noexcept
    {
        EqSettings probe = e;              // no curve on the audio side: the empty vector copies without allocating
        const bool curveOn = curveNonZero.load() && e.matchAmount > 1.0e-9;
        const bool dry = ! e.enabled || (eqIsNeutral (probe) && ! curveOn);
        uint64_t h = 0x51ed27;
        h = mix (h, fs); h = mix (h, e.linearPhase ? 1.0 : 0.0);
        if (! dry)
        {
            for (double v : { (double) e.lowCutOn, e.lowCutHz, (double) e.lowCutSlope, e.lowShelfDb, e.lowShelfHz, e.tiltDbPerOct,
                              e.highShelfDb, e.highShelfHz, e.bell1Db, e.bell1Hz, e.bell1Q, e.bell2Db, e.bell2Hz, e.bell2Q,
                              e.airDb, e.airHz, (double) e.airDynamic })
                h = mix (h, v);
            if (curveOn) { h = mix (h, e.matchAmount); h = mix (h, (double) curveVersion.load()); }
        }
        h = (h << 1) | (dry ? 1u : 0u);
        return h == 0 ? 2 : h;
    }

    /** Audio side: ask for a kernel set. Synchronous mode builds it here. */
    template <typename Fill>
    void request (int id, uint64_t key, Fill&& fill)
    {
        auto& s = slots[id];
        if (key == s.requested) return;
        {
            const juce::SpinLock::ScopedTryLockType sl (s.lock);
            if (! sl.isLocked()) return;
            fill();
            s.requested = key;
            s.wanted.store (key);
        }
        if (synchronous.load())
        {
            auto* k = build (id);
            delete s.ready.exchange (k);
        }
    }

    void adopt (Slot& s) noexcept
    {
        if (s.old != nullptr) return;
        auto* r = s.ready.exchange (nullptr);
        if (r == nullptr) return;
        if (s.cur != nullptr && s.primed) { s.old = s.cur; s.fade = 0; }
        else if (s.cur != nullptr) retire (s.cur);
        s.cur = r;
        s.primed = true;
    }

    void endBlock (Slot& s) noexcept
    {
        if (s.old != nullptr && ++s.fade >= kFadeBlocks)
            if (retire (s.old)) s.old = nullptr;
    }

    /** Crossfade weight of the new kernel at sample i of this block. */
    static float fadeW (const Slot& s, int i) noexcept
    {
        if (s.old == nullptr) return 1.0f;
        return juce::jmin (1.0f, (float) (s.fade * B + i + 1) / (float) (kFadeBlocks * B));
    }

    //--------------------------------------------------------------------------
    void prepare (double sampleRate, int numChannels, int maxHostBlock)
    {
        if (worker != nullptr) worker->stopThread (2000);
        for (auto& s : slots)
        {
            delete s.cur; delete s.old; delete s.ready.exchange (nullptr);
            s.cur = s.old = nullptr; s.requested = s.built = 0; s.wanted.store (0); s.primed = false;
        }
        drainRetired();

        fs = sampleRate;
        chans = juce::jlimit (1, 2, numChannels);
        maxHost = juce::jmax (1, maxHostBlock);
        eqTaps = eqFirLength (fs);
        eqCentre = eqTaps / 2;

        // Stage delays: each zero-phase kernel at its lowest allowed frequency.
        Mb = halfLength (kMinXoverLowHz) + halfLength (kMinXoverHighHz);
        Dw = juce::jmax (halfLength (kMinMonoHz), halfLength (kMinHighWidthHz));
        const int h5 = halfLength (5000.0), h9 = halfLength (juce::jmin (9000.0, fs * 0.45));
        int hp = 1;
        for (double hz : { 10000.0, 15000.0, 20000.0, 40000.0 })
        {
            const auto p = airPKernel (hz, fs);
            double total = 0.0;
            for (float v : p.taps) total += std::abs (v);
            double tail = 0.0;
            int k = p.latency;
            for (; k >= 1; --k)
            {
                const int a = p.latency + k, b = p.latency - k;
                tail += (a < (int) p.taps.size() ? std::abs (p.taps[(size_t) a]) : 0.0) + (b >= 0 ? std::abs (p.taps[(size_t) b]) : 0.0);
                if (tail > kTailTolerance * total) break;
            }
            hp = juce::jmax (hp, k);
        }
        Dair = juce::jmax (h5 + h9, hp);
        Lmax = limiterLookaheadSamples (LimiterSettings { true, -1.0, 150.0, kMaxLookaheadMs, 0.0 }, fs);
        Dp = 32 + Lmax - 1;

        // Harsh detector band kernel (fixed): band = r9 - r9 * r5.
        {
            const auto r5 = zeroPhaseKernel (5000.0, h5);
            const auto r9 = zeroPhaseKernel (juce::jmin (9000.0, fs * 0.45), h9);
            auto r95 = convolveSym (r9, r5);
            const int h95 = ((int) r95.size() - 1) / 2;
            for (auto& v : r95) v = -v;
            for (int i = -h9; i <= h9; ++i) r95[(size_t) (i + h95)] += r9[(size_t) (i + h9)];
            const auto k = centred (r95, Dair);
            harsh = KernelSpectra::make (k.data(), (int) k.size(), fft);
        }

        auto parts = [] (int len) { return (len + B - 1) / B; };
        for (auto& f : eqFdl) f.prepare (parts (eqTaps) + eqCentre / B);       // the minimum-phase kernel is delayed by the linear-phase centre
        harshFdl.prepare (parts (2 * Dair + 1));
        for (auto& f : airFdl) f.prepare (parts (2 * Dair + 1));
        for (auto& f : compFdl) f.prepare (parts (2 * Mb + 1));
        for (auto* f : { &sideFdl, &lFdl, &rFdl }) f->prepare (parts (2 * Dw + 1));
        for (auto& r : eqDry) r.prepare (eqCentre);
        for (auto& r : airY) r.prepare (Dair);
        airSum.prepare (Dair);
        for (auto& r : compX) r.prepare (Mb);
        for (auto* r : { &widthL, &widthR }) r->prepare (Dw);
        const int maxChain = eqCentre + Dair + Mb + Dw + 4 * Dp;
        for (auto& r : tapIn) r.prepare (maxChain);
        for (auto& r : tapEq) r.prepare (Mb + Dw + 4 * Dp);
        for (auto& r : tapComp) r.prepare (Dw + 4 * Dp);
        for (auto& r : grRing) r.prepare (Dw + 4 * Dp);
        for (auto& l : lim) l.prepare (chans, Lmax);

        for (auto* v : { &work, &acc, &tmpA, &tmpB, &tmpC, &tmpD, &tmpE, &tmpF }) v->assign ((size_t) 2 * N, 0.0f);
        for (auto& v : blk) v.assign ((size_t) B, 0.0f);
        for (auto& v : inCopy) v.assign ((size_t) B, 0.0f);
        for (auto& v : eqOut) v.assign ((size_t) B, 0.0f);
        for (auto& v : compOut) v.assign ((size_t) B, 0.0f);
        grWide.assign ((size_t) B, 0.0f);
        for (auto& g : grBand) g.assign ((size_t) B, 0.0f);
        ceilBlk.assign ((size_t) B, 1.0);
        frames.assign ((size_t) B * MeterFifo::frameSize, 0.0f);

        const int rs = nextPow2 (4 * B + 2 * maxHost + 16);
        for (auto& r : inRing) r.assign ((size_t) rs, 0.0f);
        for (auto& r : outRing) r.assign ((size_t) rs, 0.0f);
        ringMask = rs - 1;

        reset();
        worker = std::make_unique<Worker> (*this);
        worker->startThread (juce::Thread::Priority::low);
    }

    /** Every stream state back to silence: delay lines, detectors and envelopes, DC filter, limiter sums. The host
        framing (the input / output rings) is left alone, so this can also run in the middle of process(). */
    void clearDsp()
    {
        for (auto& f : eqFdl) f.clear();
        harshFdl.clear();
        for (auto& f : airFdl) f.clear();
        for (auto& f : compFdl) f.clear();
        for (auto* f : { &sideFdl, &lFdl, &rFdl }) f->clear();
        for (auto* r : { &eqDry[0], &eqDry[1], &airY[0], &airY[1], &airSum, &compX[0], &compX[1], &widthL, &widthR,
                         &tapIn[0], &tapIn[1], &tapEq[0], &tapEq[1], &tapComp[0], &tapComp[1], &grRing[0], &grRing[1], &grRing[2], &grRing[3] })
            r->clear();
        for (auto& b : dcHp) b = Biquad::butterworthHighPass (fs, 5.0);
        wide.clear();
        for (auto& b : band) b.clear();
        for (auto& d : dyn) d.clear();
        for (auto& c : dynC) for (auto& q : c) q.reset();
        {
            DynBandSettings bell;
            bell.freqHz = kAirDeHarshBellHz;
            bell.q = kAirDeHarshBellQ;
            for (auto& q : airBell) q = dynBandComponent (bell, fs);
        }
        airEb = airEr = airEnv = 0.0;
        for (auto& l : lim) l.clear();
        prevCompConfig = -1;
        compFade = kFadeBlocks;
        snap = true;
    }

    void reset()
    {
        clearDsp();
        for (auto& r : inRing) std::fill (r.begin(), r.end(), 0.0f);
        for (auto& r : outRing) std::fill (r.begin(), r.end(), 0.0f);
        inCount = 0;
        outRead = 0;
        outWrite = B;                   // the framing latency: one block of silence first
        outCount = B;
    }

    //--------------------------------------------------------------------------
    // One internal block of B samples, in place in blk[].
    void processBlock (const MasterSettings& s)
    {
        const int rampLen = juce::jmax (1, (int) (0.02 * fs));
        bool resnap = false;                  // set when the safety below cleared the state: the ramps snap at the next block
        float* x[2] { blk[0].data(), blk[1].data() };
        const int C = chans;
        for (int c = 0; c < C; ++c) std::copy (x[c], x[c] + B, inCopy[c].begin());

        //------------------------------------------------------------ targets
        const double effTrim = s.trimDb + (s.autoTrim && plan.valid ? plan.autoTrimDb : 0.0);
        const bool balOn = C > 1 && std::abs (s.balanceDb) > 1.0e-9;
        balL.set (balOn ? (double) (float) dbToGain (-0.5 * s.balanceDb) : 1.0, rampLen, snap);
        balR.set (balOn ? (double) (float) dbToGain (0.5 * s.balanceDb) : 1.0, rampLen, snap);
        trim.set (std::abs (effTrim) > 1.0e-9 ? (double) (float) dbToGain (effTrim) : 1.0, rampLen, snap);

        cur = structureFor (s);
        latencyNow.store (latencyFor (cur));
        const int Dd = cur.air ? Dair : 0, Dc = cur.bands ? Mb : 0, Ds = cur.split ? Dw : 0;

        // EQ kernel
        const auto ek = eqKey (s.eq);
        request (sEq, ek, [&] { eqReq.eq = s.eq; });
        eqLatencyNow = eqCentre;
        // Bands / width / air kernels follow their frequencies.
        const double f1 = juce::jlimit (kMinXoverLowHz, 400.0, s.comp.xoverLowHz), f2 = juce::jlimit (kMinXoverHighHz, juce::jmin (10000.0, fs * 0.45), s.comp.xoverHighHz);
        request (sBands, mix (mix (7, f1), f2), [&] { bandReq[0] = f1; bandReq[1] = f2; });
        const double wm = juce::jmax (kMinMonoHz, s.width.monoBelowHz), wh = s.width.highWidthHz;
        request (sWidth, mix (mix (11, wm), wh), [&] { widthReq[0] = wm; widthReq[1] = wh; });
        request (sAir, mix (13, s.eq.airHz), [&] { airReq = s.eq.airHz; });
        for (auto& sl : slots) adopt (sl);

        //------------------------------------------------------------ 1. input: DC filter, balance, trim
        for (int i = 0; i < B; ++i)
        {
            const float gl = (float) balL.next(), gr = (float) balR.next(), gt = (float) trim.next();
            for (int c = 0; c < C; ++c)
            {
                float v = x[c][i];
                if (s.dcFilter) v = (float) dcHp[c].process ((double) v);
                if (C > 1) v *= (c == 0 ? gl : gr);
                v *= gt;
                x[c][i] = v;
            }
        }

        //------------------------------------------------------------ 2. EQ (FIR)
        {
            auto& sl = slots[sEq];
            for (int c = 0; c < C; ++c) eqFdl[c].push (x[c], fft, work.data());
            for (int c = 0; c < C; ++c)
            {
                // Dry path of the current and the previous kernel set, delayed like each set's own latency.
                for (int i = 0; i < B; ++i) eqDry[c].push (x[c][i]);
                auto run = [&] (const KernelSet* k, float* dst)
                {
                    // Every kernel set delivers the same bulk delay (eqCentre): a linear-phase kernel has it inside, a minimum-phase
                    // one (latency 0) is started eqCentre / B blocks later, a dry set is the input delayed by it.
                    if (k == nullptr || k->dry || k->k[0] == nullptr)
                        for (int i = 0; i < B; ++i) dst[i] = eqDry[c].get (B - 1 - i + eqCentre);
                    else
                        convolve (eqFdl[c], *k->k[0], fft, acc.data(), work.data(), dst, juce::jmax (0, eqCentre - k->latency) / B);
                };
                run (sl.cur, tmpA.data());
                if (sl.old != nullptr)
                {
                    run (sl.old, tmpB.data());
                    for (int i = 0; i < B; ++i) { const float w = fadeW (sl, i); x[c][i] = tmpB[(size_t) i] + (tmpA[(size_t) i] - tmpB[(size_t) i]) * w; }
                }
                else
                    std::copy (tmpA.begin(), tmpA.begin() + B, x[c]);
            }
            endBlock (sl);
        }

        //------------------------------------------------------------ 3. dynamic EQ: tightener, low-mid, upper-mid (causal)
        const bool eqOn = s.eq.enabled;
        const DynBandSettings* dset[3] { &s.eq.tightener, &s.eq.dynLowMid, &s.eq.dynUpperMid };
        for (int d = 0; d < 3; ++d)
        {
            const auto& ds = *dset[d];
            const bool on = eqOn && ds.enabled && ds.amountDb > 0.0 && ds.rangeDb > 0.0 && plan.valid && plan.dynActive[d];
            dynThr[d].set (plan.dynThresholdDb[d], rampLen, snap);
            // Switching a band in or out glides its cut in or out over 20 ms (a step of the cut would click).
            dynGate[d].set (on ? 1.0 : 0.0, rampLen, snap);
            if (! on && ! dynGate[d].moving() && dynGate[d].v <= 0.0) { for (int i = 0; i < B; ++i) dynThr[d].next(); continue; }
            if (! std::equal_to<double>{} (ds.freqHz, dynFreq[d]) || ! std::equal_to<double>{} (ds.q, dynQ[d]) || ds.shape != dynShape[d])
            {
                dynFreq[d] = ds.freqHz; dynQ[d] = ds.q; dynShape[d] = ds.shape;
                for (auto& q : dynC[d]) { const double z1 = q.z1, z2 = q.z2; q = dynBandComponent (ds, fs); q.z1 = z1; q.z2 = z2; }
            }
            auto& core = dyn[d];
            core.setDetector (fs, 0.0, 0.7);
            core.setGain (fs, ds.ratio, 6.0, ds.attackMs, ds.releaseMs, false, ds.rangeDb);
            for (int i = 0; i < B; ++i)
            {
                core.thr = dynThr[d].next();
                const float c0 = (float) dynC[d][0].process ((double) x[0][i]);
                const float c1 = C > 1 ? (float) dynC[d][1].process ((double) x[1][i]) : 0.0f;
                const float gr = core.gain (core.level (c0, c1, C));
                const double g = std::pow (10.0, (double) gr / 20.0);
                const double gt = (g - 1.0) * dynGate[d].next();
                x[0][i] += (float) (gt * c0);
                if (C > 1) x[1][i] += (float) (gt * c1);
            }
        }

        //------------------------------------------------------------ 3b. dynamic AIR (delay Dair)
        {
            auto& sl = slots[sAir];
            float* sum = tmpE.data();
            for (int i = 0; i < B; ++i)
            {
                float v = 0.0f;
                for (int c = 0; c < C; ++c) v += x[c][i] * (1.0f / (float) C);
                sum[i] = v;
            }
            harshFdl.push (sum, fft, work.data());
            for (int c = 0; c < C; ++c) airFdl[c].push (x[c], fft, work.data());
            for (int i = 0; i < B; ++i) airSum.push (sum[i]);
            for (int c = 0; c < C; ++c) for (int i = 0; i < B; ++i) airY[c].push (x[c][i]);

            const double air = juce::jmax (0.0, s.eq.airDb), range = juce::jmax (0.0, s.eq.airDeHarshDb);
            const bool want = eqOn && s.eq.airDynamic && (std::abs (s.eq.airDb) > 1.0e-9 || s.eq.airDeHarshDb > 0.0) && plan.valid && plan.airActive;
            // Switching AIR DYN fades the dynamic AIR in or out over the EQ kernel crossfade (the static AIR leaves / joins the EQ
            // kernel in the same time), so a switch is a level glide, not a step.
            airGate.set (want ? 1.0 : 0.0, kFadeBlocks * B, snap);
            const bool on = want || airGate.moving() || airGate.v > 0.0;
            for (int c = 0; c < C; ++c)
                for (int i = 0; i < B; ++i) x[c][i] = airY[c].get (B - 1 - i + Dair);
            if (on)
            {
                float* bandSig = tmpF.data();
                convolve (harshFdl, *harsh, fft, acc.data(), work.data(), bandSig);
                float* pSig[2] { tmpC.data(), tmpD.data() };
                const bool pOn = air > 0.0 && sl.cur != nullptr && ! sl.cur->dry;
                if (pOn)
                    for (int c = 0; c < C; ++c) convolve (airFdl[c], *sl.cur->k[0], fft, acc.data(), work.data(), pSig[c]);
                const double thr = plan.airTypicalRelDb + kAirDeHarshMarginDb;
                const double aA = std::exp (-1.0 / (0.002 * fs)), aR = std::exp (-1.0 / (0.060 * fs)), a = std::exp (-1.0 / (20.0e-3 * fs));
                for (int i = 0; i < B; ++i)
                {
                    const double vb = bandSig[i];
                    const double vr = (double) airSum.get (B - 1 - i + Dair) - vb;
                    airEb = a * airEb + (1.0 - a) * vb * vb;
                    airEr = a * airEr + (1.0 - a) * vr * vr;
                    const float rel = (float) (10.0 * std::log10 (airEb + 1.0e-20)) - (float) (10.0 * std::log10 (airEr + 1.0e-20));
                    const double airWant = juce::jlimit (0.0, range, (double) rel - thr);
                    airEnv = airWant > airEnv ? aA * airEnv + (1.0 - aA) * airWant : aR * airEnv + (1.0 - aR) * airWant;
                    const double r = (double) (float) airEnv;
                    const double gate = airGate.next();
                    const float gAir = (float) (gate * (std::pow (10.0, juce::jmax (0.0, air - r) / 20.0) - 1.0));
                    const double cut = juce::jmax (0.0, r - air);
                    const float gCut = (float) (gate * (1.0 - std::pow (10.0, -cut / 20.0)));
                    for (int c = 0; c < C; ++c)
                    {
                        const float bb = (float) airBell[c].process ((double) x[c][i]);
                        float y = x[c][i];
                        if (pOn) y += gAir * pSig[c][i];
                        if (cut > 0.0) y -= gCut * bb;
                        x[c][i] = y;
                    }
                }
            }
            else
            {
                // The bell's band-pass runs on the whole stream, as offline, so switching AIR DYN on starts in step.
                for (int c = 0; c < C; ++c) for (int i = 0; i < B; ++i) airBell[c].process ((double) x[c][i]);
            }
            endBlock (sl);
        }
        for (int c = 0; c < C; ++c) std::copy (x[c], x[c] + B, eqOut[c].begin());

        //------------------------------------------------------------ 4. compressor (delay Mb)
        {
            auto& sl = slots[sBands];
            for (int c = 0; c < C; ++c) compFdl[c].push (x[c], fft, work.data());
            for (int c = 0; c < C; ++c) for (int i = 0; i < B; ++i) compX[c].push (x[c][i]);
            float* xd[2] { tmpA.data(), tmpB.data() };
            for (int c = 0; c < C; ++c) for (int i = 0; i < B; ++i) xd[c][i] = compX[c].get (B - 1 - i + Dc);

            const auto& cs = s.comp;
            int config = ! cs.enabled ? 0 : (cs.mode == CompMode::threeBand ? 2 : 1);
            if (prevCompConfig < 0) prevCompConfig = config;
            if (config != prevCompConfig && compFade >= kFadeBlocks) { compFromConfig = prevCompConfig; compFade = 0; prevCompConfig = config; }

            // Shared targets.
            const bool autoOk = plan.valid && plan.sourceValid;
            compThr.set (cs.thresholdDb, rampLen, snap);
            compMakeup.set (plan.valid ? plan.wideMakeupDb : 0.0, rampLen, snap);
            compMix.set (juce::jlimit (0.0, 1.0, cs.mix), rampLen, snap);
            bool anySolo = false;
            for (const auto& b : cs.bands) anySolo |= b.solo;
            for (int b = 0; b < 3; ++b)
            {
                bandThr[b].set (cs.bands[b].thresholdDb, rampLen, snap);
                const bool audible = ! anySolo || cs.bands[b].solo;
                const double mk = plan.valid ? (plan.bandActive[b] && ! cs.bands[b].bypass ? plan.bandMakeupDb[b] : 0.0) + plan.bandFinalMakeupDb : 0.0;
                bandGain[b].set (audible ? dbToGain (mk + juce::jlimit (-12.0, 12.0, cs.bands[b].gainDb)) : 0.0, rampLen, snap);
            }

            auto runConfig = [&] (int cfg, float* const* dst, bool meter)
            {
                if (cfg == 0)
                {
                    for (int c = 0; c < C; ++c) std::copy (xd[c], xd[c] + B, dst[c]);
                    if (meter) { std::fill (grWide.begin(), grWide.end(), 0.0f); for (auto& g : grBand) std::fill (g.begin(), g.end(), 0.0f); }
                    return;
                }
                if (cfg == 1)
                {
                    double ratio = cs.ratio;
                    if (cs.autoThreshold && autoOk && plan.denseWide) ratio = juce::jmin (kDenseRatio, ratio);
                    wide.setDetector (fs, cs.scHpfHz, cs.rmsBlend);
                    wide.setGain (fs, ratio, cs.kneeDb, cs.attackMs, cs.releaseMs, cs.autoRelease, cs.maxGrDb);
                    for (int i = 0; i < B; ++i)
                    {
                        wide.thr = compThr.next();
                        const double mixv = compMix.next();
                        const float mk = (float) dbToGain (compMakeup.next());
                        const float gr = wide.gain (wide.level (xd[0][i], C > 1 ? xd[1][i] : 0.0f, C));
                        const double g = std::pow (10.0, (double) gr / 20.0);
                        const float k = (float) (1.0 - mixv + mixv * g);
                        for (int c = 0; c < C; ++c) dst[c][i] = (xd[c][i] * k) * mk;
                        if (meter) grWide[(size_t) i] = -gr;
                    }
                    if (meter) for (auto& g : grBand) std::fill (g.begin(), g.end(), 0.0f);
                    return;
                }
                // 3-BAND: split (linear phase, the offline zero-phase split), a compressor per band, level-neutral sum.
                float* lo[2] { tmpC.data(), tmpD.data() };
                float* md[2] { tmpE.data(), tmpF.data() };
                const KernelSet* ks = sl.cur;
                for (int c = 0; c < C; ++c)
                {
                    if (ks != nullptr && ! ks->dry)
                    {
                        convolve (compFdl[c], *ks->k[0], fft, acc.data(), work.data(), lo[c]);
                        convolve (compFdl[c], *ks->k[1], fft, acc.data(), work.data(), md[c]);
                        if (sl.old != nullptr && ! sl.old->dry)
                        {
                            // Crossover move: blend the old split into the new one.
                            convolve (compFdl[c], *sl.old->k[0], fft, acc.data(), work.data(), blkScratch.data());
                            for (int i = 0; i < B; ++i) { const float w = fadeW (sl, i); lo[c][i] = blkScratch[(size_t) i] + (lo[c][i] - blkScratch[(size_t) i]) * w; }
                            convolve (compFdl[c], *sl.old->k[1], fft, acc.data(), work.data(), blkScratch.data());
                            for (int i = 0; i < B; ++i) { const float w = fadeW (sl, i); md[c][i] = blkScratch[(size_t) i] + (md[c][i] - blkScratch[(size_t) i]) * w; }
                        }
                    }
                    else
                    {
                        std::fill (lo[c], lo[c] + B, 0.0f);
                        std::fill (md[c], md[c] + B, 0.0f);
                    }
                }
                for (int b = 0; b < 3; ++b)
                {
                    auto& core = band[b];
                    const auto& bs = cs.bands[b];
                    double ratio = bs.ratio;
                    if (bs.autoThreshold && autoOk && plan.densePeak) ratio = juce::jmin (kDenseRatio, ratio);
                    core.setDetector (fs, 0.0, cs.rmsBlend);
                    core.setGain (fs, ratio, cs.kneeDb, bs.attackMs, bs.releaseMs, cs.autoRelease, 0.0);
                }
                for (int i = 0; i < B; ++i)
                {
                    const double mixv = compMix.next();
                    float bandS[3][2];
                    for (int c = 0; c < C; ++c)
                    {
                        bandS[0][c] = lo[c][i];
                        bandS[1][c] = md[c][i];
                        bandS[2][c] = (xd[c][i] - lo[c][i]) - md[c][i];
                    }
                    float y[2] { 0.0f, 0.0f };
                    float worst = 0.0f;
                    for (int b = 0; b < 3; ++b)
                    {
                        auto& core = band[b];
                        core.thr = bandThr[b].next();
                        const float gb = (float) bandGain[b].next();
                        float k = 1.0f;
                        float grv = 0.0f;
                        if (! cs.bands[b].bypass)
                        {
                            grv = core.gain (core.level (bandS[b][0], C > 1 ? bandS[b][1] : 0.0f, C));
                            const double g = std::pow (10.0, (double) grv / 20.0);
                            k = (float) (1.0 - mixv + mixv * g);
                        }
                        else
                            core.level (bandS[b][0], C > 1 ? bandS[b][1] : 0.0f, C);
                        for (int c = 0; c < C; ++c) y[c] += (bandS[b][c] * k) * gb;
                        if (meter) grBand[b][(size_t) i] = -grv;
                        worst = juce::jmin (worst, grv);
                    }
                    for (int c = 0; c < C; ++c) dst[c][i] = y[c];
                    if (meter) grWide[(size_t) i] = -worst;
                }
            };

            float* dst[2] { compOut[0].data(), compOut[1].data() };
            if (compFade < kFadeBlocks)
            {
                // Mode or on/off change: run the old configuration too and crossfade.
                float* alt[2] { x[0], x[1] };
                runConfig (compFromConfig, alt, false);
                // ramps advanced twice above; re-snap them to their targets after this block (they are steady in use)
                runConfig (config, dst, true);
                for (int c = 0; c < C; ++c)
                    for (int i = 0; i < B; ++i)
                    {
                        const float w = juce::jmin (1.0f, (float) (compFade * B + i + 1) / (float) (kFadeBlocks * B));
                        dst[c][i] = alt[c][i] + (dst[c][i] - alt[c][i]) * w;
                    }
                ++compFade;
            }
            else
                runConfig (config, dst, true);
            for (int c = 0; c < C; ++c) std::copy (dst[c], dst[c] + B, x[c]);
            endBlock (sl);
        }

        //------------------------------------------------------------ 5. stereo (delay Dw)
        {
            auto& sl = slots[sWidth];
            const auto& ws = s.width;
            if (C > 1)
            {
                float* side = tmpA.data();
                for (int i = 0; i < B; ++i) side[i] = (float) (0.5 * ((double) x[0][i] - x[1][i]));
                sideFdl.push (side, fft, work.data());
                lFdl.push (x[0], fft, work.data());
                rFdl.push (x[1], fft, work.data());
                for (int i = 0; i < B; ++i) { widthL.push (x[0][i]); widthR.push (x[1][i]); }
            }
            width.set (ws.enabled ? ws.width : 1.0, rampLen, snap);        // a disabled stage glides to neutral: switching it in starts from 100 %
            highWidth.set (ws.enabled ? ws.highWidth : 1.0, rampLen, snap);
            monoGate.set (ws.enabled && ws.monoBelowHz > 0.0 ? 1.0 : 0.0, rampLen, snap);
            safeGate.set (ws.enabled && ws.monoBelowHz > 0.0 && ws.phaseSafeLowMono ? 1.0 : 0.0, rampLen, snap);
            // Neutral only once every gate and ramp has settled on its neutral value: a stage switched out glides out first.
            const bool neutral = C < 2 || (widthIsNeutral (ws) && ! width.moving() && ! highWidth.moving() && ! monoGate.moving() && ! safeGate.moving());
            if (C < 2)
            {
                for (int i = 0; i < B; ++i) widthL.push (x[0][i]);
            }
            else if (neutral)
            {
                for (int i = 0; i < B; ++i) { x[0][i] = widthL.get (B - 1 - i + Dw); x[1][i] = widthR.get (B - 1 - i + Dw); width.next(); highWidth.next(); monoGate.next(); safeGate.next(); }
            }
            else
            {
                const bool monoActive = monoGate.v > 0.0 || monoGate.moving();
                const bool hiOn = ws.highWidthHz > 0.0 && ((ws.enabled && std::abs (ws.highWidth - 1.0) > 1.0e-9) || highWidth.moving() || std::abs (highWidth.v - 1.0) > 1.0e-9);
                const bool safeActive = safeGate.v > 0.0 || safeGate.moving();
                auto stage = [&] (const KernelSet* k, float* outL, float* outR)
                {
                    float* low = tmpB.data();
                    float* lpHw = tmpC.data();
                    float* lowL = tmpD.data();
                    float* lowR = tmpE.data();
                    if (monoActive) convolve (sideFdl, *k->k[0], fft, acc.data(), work.data(), low);
                    if (hiOn) convolve (sideFdl, *k->k[1], fft, acc.data(), work.data(), lpHw);
                    if (safeActive)
                    {
                        convolve (lFdl, *k->k[0], fft, acc.data(), work.data(), lowL);
                        convolve (rFdl, *k->k[0], fft, acc.data(), work.data(), lowR);
                    }
                    for (int i = 0; i < B; ++i)
                    {
                        const double l = widthL.get (B - 1 - i + Dw), r = widthR.get (B - 1 - i + Dw);
                        const double sideD = 0.5 * (l - r);
                        double sd = sideD;
                        if (monoActive) sd -= monoGate.v * low[i];
                        double hs = 0.0;
                        if (hiOn) { hs = sideD - lpHw[i]; sd -= hs; }
                        const double wv = width.v, hw = highWidth.v, ps = safeGate.v;
                        sd = (sd + hs * hw) * wv;
                        if (safeActive && ps >= 1.0)
                        {
                            const double midHigh = 0.5 * (l + r) - 0.5 * ((double) lowL[i] + lowR[i]);
                            const double lowMono = plan.safeGain * 0.5 * ((double) lowL[i] + plan.safeSign * lowR[i]);
                            outL[i] = (float) (midHigh + lowMono + sd);
                            outR[i] = (float) (midHigh + lowMono - sd);
                        }
                        else if (safeActive && ps > 0.0)
                        {
                            // The phase-safe low mono glides in or out: the low mid is replaced by the polarity-matched sum by the share ps.
                            const double mid = 0.5 * (l + r);
                            const double lowMid = 0.5 * ((double) lowL[i] + lowR[i]);
                            const double lowMono = plan.safeGain * 0.5 * ((double) lowL[i] + plan.safeSign * lowR[i]);
                            const double m2 = mid + ps * (lowMono - lowMid);
                            outL[i] = (float) (m2 + sd);
                            outR[i] = (float) (m2 - sd);
                        }
                        else
                        {
                            const double mid = 0.5 * (l + r);
                            outL[i] = (float) (mid + sd);
                            outR[i] = (float) (mid - sd);
                        }
                        if (outL == x[0]) { width.next(); highWidth.next(); monoGate.next(); safeGate.next(); }
                    }
                };
                if (sl.cur != nullptr && ! sl.cur->dry)
                {
                    if (sl.old != nullptr && ! sl.old->dry)
                    {
                        float* oL = wOld[0].data();
                        float* oR = wOld[1].data();
                        stage (sl.old, oL, oR);
                        stage (sl.cur, x[0], x[1]);
                        for (int i = 0; i < B; ++i)
                        {
                            const float w = fadeW (sl, i);
                            x[0][i] = oL[i] + (x[0][i] - oL[i]) * w;
                            x[1][i] = oR[i] + (x[1][i] - oR[i]) * w;
                        }
                    }
                    else
                        stage (sl.cur, x[0], x[1]);
                }
                else
                    for (int i = 0; i < B; ++i) { x[0][i] = widthL.get (B - 1 - i + Dw); x[1][i] = widthR.get (B - 1 - i + Dw); width.next(); highWidth.next(); monoGate.next(); safeGate.next(); }
            }
            endBlock (sl);
        }

        //------------------------------------------------------------ 6. drive + true-peak limiter (4 passes, delay 4 Dp)
        {
            const auto& ls = s.limiter;
            drive.set ((double) (float) dbToGain (ls.driveDb), rampLen, snap);
            ceiling.set (dbToGain (ls.ceilingDbtp), rampLen, snap);
            for (int i = 0; i < B; ++i)
            {
                const float d = (float) drive.next();
                for (int c = 0; c < C; ++c) x[c][i] *= d;
                ceilBlk[(size_t) i] = ls.enabled ? ceiling.next() : 1.0e30;
            }
            const int L = limiterLookaheadSamples (ls, fs);
            const double rel = limiterReleaseCoef (ls, fs);
            for (auto& p : lim)
            {
                p.setParams (L, rel);
                p.process (x, ceilBlk.data(), tp);
            }
        }

        //------------------------------------------------------------ safety: a non-finite sample never leaves the chain
        {
            bool bad = false;
            for (int c = 0; c < C && ! bad; ++c)
                for (int i = 0; i < B; ++i)
                    if (! std::isfinite (x[c][i])) { bad = true; break; }
            if (bad)
            {
                // Whatever poisoned the filters, detectors or the limiter is dropped; the block is silent and the next one starts clean.
                clearDsp();
                resnap = true;
                for (int c = 0; c < C; ++c)
                {
                    std::fill (x[c], x[c] + B, 0.0f);
                    std::fill (eqOut[c].begin(), eqOut[c].end(), 0.0f);
                    std::fill (compOut[c].begin(), compOut[c].end(), 0.0f);
                }
                std::fill (grWide.begin(), grWide.end(), 0.0f);
                for (auto& g : grBand) std::fill (g.begin(), g.end(), 0.0f);
            }
        }

        //------------------------------------------------------------ taps, bypass / A-B, meters
        const int De = eqLatencyNow;
        const int Dtail = Dc + Ds + 4 * Dp;           // EQ tap -> output
        const int Dchain = De + Dd + Dtail;
        const bool listen = listenInput.load();
        const float lg = listenGain.load();
        ab.set (s.bypass ? 1.0 : (listen ? (double) lg : 0.0), rampLen, snap);
        out.set (s.bypass || listen ? 0.0 : 1.0, rampLen, snap);
        float limGrMax = 0.0f, compGrMax = 0.0f;
        for (int i = 0; i < B; ++i)
        {
            const float go = (float) out.next(), gi = (float) ab.next();
            float gTot = 1.0f;
            for (int k = 0; k < 4; ++k) gTot *= lim[k].gainAt (lim[k].t - B + i - (juce::int64) (4 - k) * Dp);
            const float limGr = gTot < 1.0f ? (float) (-20.0 * std::log10 (juce::jmax (1.0e-9f, gTot))) : 0.0f;
            for (int k = 0; k < 4; ++k) grRing[k].push (k == 0 ? grWide[(size_t) i] : grBand[k - 1][(size_t) i]);
            for (int c = 0; c < C; ++c)
            {
                tapIn[c].push (inCopy[c][(size_t) i]);
                tapEq[c].push (eqOut[c][(size_t) i]);
                tapComp[c].push (compOut[c][(size_t) i]);
            }
            float* f = frames.data() + (size_t) i * MeterFifo::frameSize;
            for (int c = 0; c < 2; ++c)
            {
                const int cc = juce::jmin (c, C - 1);
                const float vin = tapIn[cc].get (Dchain);
                const float processed = x[cc][i];
                const float heard = go * processed + gi * vin;
                if (c < C) x[c][i] = heard;
                f[MeterFifo::inL + c] = vin;
                f[MeterFifo::eqL + c] = tapEq[cc].get (Dtail);
                f[MeterFifo::compL + c] = tapComp[cc].get (Ds + 4 * Dp);
                f[MeterFifo::outL + c] = heard;
            }
            for (int k = 0; k < 4; ++k) f[MeterFifo::grComp + k] = grRing[k].get (Ds + 4 * Dp);
            f[MeterFifo::grLim] = s.limiter.enabled ? limGr : 0.0f;
            limGrMax = juce::jmax (limGrMax, f[MeterFifo::grLim]);
            compGrMax = juce::jmax (compGrMax, f[MeterFifo::grComp]);
        }
        lastCompGr.store (compGrMax);
        lastLimGr.store (limGrMax);
        if (meters != nullptr) meters->push (frames.data(), B);
        snap = resnap;
    }

    std::vector<float> blkScratch = std::vector<float> ((size_t) B, 0.0f);
    std::vector<float> wOld[2] { std::vector<float> ((size_t) B, 0.0f), std::vector<float> ((size_t) B, 0.0f) };

    //--------------------------------------------------------------------------
    void process (float* const* io, int numChannels, int n, const MasterSettings& s)
    {
        const int C = juce::jmin (numChannels, chans);
        int pos = 0;
        while (pos < n)
        {
            const int take = juce::jmin (n - pos, B - inCount);
            for (int i = 0; i < take; ++i)
                for (int c = 0; c < chans; ++c)
                {
                    const float v = io[juce::jmin (c, C - 1)][pos + i];
                    blk[c][(size_t) (inCount + i)] = std::isfinite (v) ? v : 0.0f;       // NaN / Inf in -> silence in
                }
            // Output side: what was processed earlier.
            for (int i = 0; i < take; ++i)
            {
                for (int c = 0; c < C; ++c) io[c][pos + i] = outRing[c][(size_t) ((outRead + i) & ringMask)];
            }
            outRead = (outRead + take) & ringMask;
            outCount -= take;
            inCount += take;
            pos += take;
            if (inCount == B)
            {
                processBlock (s);
                for (int i = 0; i < B; ++i)
                    for (int c = 0; c < chans; ++c) outRing[c][(size_t) ((outWrite + i) & ringMask)] = blk[c][(size_t) i];
                outWrite = (outWrite + B) & ringMask;
                outCount += B;
                inCount = 0;
            }
        }
    }
};

//==============================================================================
RtChain::RtChain() : impl (std::make_unique<Impl>()) {}
RtChain::~RtChain() = default;

void RtChain::prepare (double sampleRate, int numChannels, int maxHostBlock) { impl->prepare (sampleRate, numChannels, maxHostBlock); }
void RtChain::reset() { impl->reset(); }
double RtChain::getSampleRate() const noexcept { return impl->fs; }
int RtChain::getNumChannels() const noexcept { return impl->chans; }

int RtChain::latencySamples (const MasterSettings& s) const noexcept { return impl->latencyFor (impl->structureFor (s)); }
int RtChain::maxLatencySamples() const noexcept
{
    Impl::Structure all;
    all.linear = all.air = all.bands = all.split = true;
    return impl->latencyFor (all);
}
int RtChain::currentLatency() const noexcept { return impl->latencyNow.load(); }

void RtChain::setMatchCurve (const std::vector<float>& curveDb)
{
    bool nonZero = false;
    for (float v : curveDb) nonZero |= std::abs (v) > 1.0e-6f;
    {
        const juce::SpinLock::ScopedLockType sl (impl->curveLock);
        impl->curve = curveDb;
    }
    impl->curveNonZero.store (nonZero);
    impl->curveVersion.fetch_add (1);
}

void RtChain::setSynchronousKernels (bool b) noexcept { impl->synchronous.store (b); }
void RtChain::setPlan (const RtPlan& p) noexcept { impl->plan = p; sanitisePlan (impl->plan); }
const RtPlan& RtChain::getPlan() const noexcept { return impl->plan; }
void RtChain::setListenInput (bool l, float g) noexcept { impl->listenInput.store (l); impl->listenGain.store (g); }
void RtChain::setMeterFifo (MeterFifo* f) noexcept { impl->meters = f; }
float RtChain::lastCompGrDb() const noexcept { return impl->lastCompGr.load(); }
float RtChain::lastLimiterGrDb() const noexcept { return impl->lastLimGr.load(); }

bool RtChain::kernelsSettling() const noexcept
{
    for (auto& s : impl->slots)
        if (s.old != nullptr || s.ready.load() != nullptr || s.wanted.load() != s.built) return true;
    return false;
}

void RtChain::prime (const MasterSettings& s)
{
    auto& I = *impl;
    const bool wasSync = I.synchronous.load();
    I.synchronous.store (true);
    MasterSettings probe = s;
    probe.eq.matchCurveDb.clear();
    const auto ek = I.eqKey (probe.eq);
    I.slots[Impl::sEq].requested = 0;
    I.request (Impl::sEq, ek, [&] { I.eqReq.eq = probe.eq; });
    const double f1 = juce::jlimit (kMinXoverLowHz, 400.0, s.comp.xoverLowHz), f2 = juce::jlimit (kMinXoverHighHz, juce::jmin (10000.0, I.fs * 0.45), s.comp.xoverHighHz);
    I.slots[Impl::sBands].requested = 0;
    I.request (Impl::sBands, Impl::mix (Impl::mix (7, f1), f2), [&] { I.bandReq[0] = f1; I.bandReq[1] = f2; });
    const double wm = juce::jmax (kMinMonoHz, s.width.monoBelowHz), wh = s.width.highWidthHz;
    I.slots[Impl::sWidth].requested = 0;
    I.request (Impl::sWidth, Impl::mix (Impl::mix (11, wm), wh), [&] { I.widthReq[0] = wm; I.widthReq[1] = wh; });
    I.slots[Impl::sAir].requested = 0;
    I.request (Impl::sAir, Impl::mix (13, s.eq.airHz), [&] { I.airReq = s.eq.airHz; });
    for (auto& sl : I.slots)
    {
        sl.primed = false;
        I.adopt (sl);
    }
    I.synchronous.store (wasSync);
}

void RtChain::process (float* const* channels, int numChannels, int numSamples, const MasterSettings& s) noexcept
{
    impl->process (channels, numChannels, numSamples, s);
}
} // namespace mihenk
