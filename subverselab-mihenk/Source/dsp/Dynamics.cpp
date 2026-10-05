#include "Dynamics.h"
#include "Filters.h"
#include "Loudness.h"
#include <deque>
#include <algorithm>

namespace mihenk
{
namespace
{
double coef (double ms, double fs) { return ms <= 0.0 ? 0.0 : std::exp (-1.0 / (ms * 1.0e-3 * fs)); }
} // namespace

GainReductionStats gainStats (const std::vector<float>& gain, double fs)
{
    GainReductionStats st;
    const int n = (int) gain.size();
    if (n == 0) return st;
    const int block = juce::jmax (1, (int) std::lround (0.01 * fs));
    st.history.reserve ((size_t) (n / block + 1));
    double sum = 0.0;
    int active = 0;
    float blockMax = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const double gr = gain[(size_t) i] >= 1.0f ? 0.0 : -20.0 * std::log10 (juce::jmax (1.0e-9f, gain[(size_t) i]));
        sum += gr;
        if (gr > 0.1) ++active;
        st.maxDb = juce::jmax (st.maxDb, gr);
        blockMax = juce::jmax (blockMax, (float) gr);
        if ((i + 1) % block == 0 || i == n - 1)
        {
            st.history.push_back (blockMax);
            blockMax = 0.0f;
        }
    }
    st.meanDb = sum / n;
    st.activeFraction = (double) active / n;
    return st;
}

//==============================================================================
void compressorDetector (const juce::AudioBuffer<float>& b, double fs, const CompSettings& s, std::vector<float>& levelDb)
{
    const int chans = b.getNumChannels(), n = b.getNumSamples();
    std::vector<Biquad> hp ((size_t) chans, Biquad::butterworthHighPass (fs, juce::jmax (20.0, s.scHpfHz)));
    const bool useHp = s.scHpfHz > 20.0;
    const double blend = juce::jlimit (0.0, 1.0, s.rmsBlend);
    const double aRms = coef (25.0, fs), aPk = coef (50.0, fs);
    levelDb.resize ((size_t) n);
    double ms = 0.0, pkEnv = 0.0;
    for (int i = 0; i < n; ++i)
    {
        double pk = 0.0, sq = 0.0;
        for (int c = 0; c < chans; ++c)
        {
            double x = b.getReadPointer (c)[i];
            if (useHp) x = hp[(size_t) c].process (x);
            pk = juce::jmax (pk, std::abs (x));
            sq += x * x;
        }
        sq /= chans;
        ms = aRms * ms + (1.0 - aRms) * sq;
        pkEnv = juce::jmax (pk, aPk * pkEnv);
        const double peakDb = 20.0 * std::log10 (pkEnv + 1.0e-12);
        const double rmsDb = 10.0 * std::log10 (ms + 1.0e-24) + 3.0103;
        levelDb[(size_t) i] = (float) ((1.0 - blend) * peakDb + blend * rmsDb);
    }
}

void compressorGainDb (const std::vector<float>& levelDb, double fs, const CompSettings& s, std::vector<float>& grDb)
{
    const int n = (int) levelDb.size();
    const double ratio = juce::jlimit (1.0, 20.0, s.ratio), slope = 1.0 / ratio - 1.0;
    const double knee = juce::jmax (0.0, s.kneeDb), thr = s.thresholdDb;
    const double aA = coef (s.attackMs, fs), aR = coef (s.releaseMs, fs);
    const double aAs = coef (s.attackMs * 3.0, fs), aRs = coef (s.releaseMs * 4.0, fs), aRf = coef (s.releaseMs / 3.0, fs);
    grDb.resize ((size_t) n);
    double env = 0.0, yS = 0.0, yF = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double over = (double) levelDb[(size_t) i] - thr;
        double gr = 0.0;
        if (2.0 * over <= -knee) gr = 0.0;
        else if (knee > 0.0 && 2.0 * std::abs (over) <= knee) gr = slope * (over + knee * 0.5) * (over + knee * 0.5) / (2.0 * knee);
        else gr = slope * over;
        if (s.maxGrDb > 0.0) gr = juce::jmax (gr, -s.maxGrDb);

        if (s.autoRelease)
        {
            yS = gr < yS ? aAs * yS + (1.0 - aAs) * gr : aRs * yS + (1.0 - aRs) * gr;
            const double res = juce::jmin (0.0, gr - yS);
            yF = res < yF ? aA * yF + (1.0 - aA) * res : aRf * yF + (1.0 - aRf) * res;
            env = yS + yF;
        }
        else
        {
            env = gr < env ? aA * env + (1.0 - aA) * gr : aR * env + (1.0 - aR) * gr;
        }
        grDb[(size_t) i] = (float) env;
    }
}

void processCompressor (juce::AudioBuffer<float>& b, double fs, const CompSettings& s, GainReductionStats& stats,
                        std::vector<float>* grDbOut, const std::vector<float>* levelDbIn)
{
    const int chans = b.getNumChannels(), n = b.getNumSamples();
    std::vector<float> level, grLocal;
    if (levelDbIn == nullptr || (int) levelDbIn->size() != n)
    {
        compressorDetector (b, fs, s, level);
        levelDbIn = &level;
    }
    auto& gr = grDbOut != nullptr ? *grDbOut : grLocal;
    compressorGainDb (*levelDbIn, fs, s, gr);

    const double mix = juce::jlimit (0.0, 1.0, s.mix);
    std::vector<float> gain ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        const double g = std::pow (10.0, (double) gr[(size_t) i] / 20.0);
        gain[(size_t) i] = (float) g;
        const float k = (float) (1.0 - mix + mix * g);
        for (int c = 0; c < chans; ++c) b.getWritePointer (c)[i] *= k;
    }
    // Stats on the effective (mixed) gain is misleading for parallel use; report the compressor's own reduction.
    stats = gainStats (gain, fs);
}

//==============================================================================
std::vector<uint8_t> loudPartMask (const std::vector<float>& shortTerm, int numSamples, double fs, double percentile)
{
    std::vector<uint8_t> mask ((size_t) juce::jmax (0, numSamples), 1);
    std::vector<float> gated;
    for (float v : shortTerm) if (v > -70.0f) gated.push_back (v);
    if (gated.empty()) return mask;              // shorter than 3 s or silent: the whole file counts
    std::sort (gated.begin(), gated.end());
    const float p = gated[(size_t) std::floor (juce::jlimit (0.0, 1.0, percentile) * (double) (gated.size() - 1))];
    const int block = juce::jmax (1, (int) std::lround (0.1 * fs));
    const int last = (int) shortTerm.size() - 1;
    for (int i = 0; i < numSamples; ++i)
    {
        // Short-term entry k covers [0.1 k, 0.1 k + 3) s; use the window centred on this 100 ms block.
        const int k = juce::jlimit (0, last, i / block - 14);
        mask[(size_t) i] = shortTerm[(size_t) k] >= p ? 1 : 0;
    }
    return mask;
}

void maskedGrStats (const std::vector<float>& grDb, const std::vector<uint8_t>& mask, double& meanDb, double& maxDb)
{
    double sum = 0.0, mx = 0.0;
    size_t count = 0;
    const size_t n = juce::jmin (grDb.size(), mask.size());
    for (size_t i = 0; i < n; ++i)
        if (mask[i] != 0)
        {
            const double r = -(double) grDb[i];
            sum += r;
            mx = juce::jmax (mx, r);
            ++count;
        }
    meanDb = count > 0 ? sum / (double) count : 0.0;
    maxDb = mx;
}

double solveGlueThreshold (const std::vector<float>& levelDb, const std::vector<uint8_t>& mask, double fs, CompSettings c,
                           double targetGrDb, int* iterations)
{
    int its = 0;
    std::vector<float> gr;
    float maxLevel = -120.0f;
    for (float v : levelDb) maxLevel = juce::jmax (maxLevel, v);
    auto f = [&] (double thr)
    {
        ++its;
        c.thresholdDb = thr;
        compressorGainDb (levelDb, fs, c, gr);
        double mean = 0.0, mx = 0.0;
        maskedGrStats (gr, mask, mean, mx);
        return mean - targetGrDb;             // falls as the threshold rises
    };
    // Above this threshold nothing is ever reduced (level + half the knee below it).
    double hi = juce::jmin (0.0, (double) maxLevel + 0.5 * juce::jmax (0.0, c.kneeDb) + 0.1);
    double lo = -80.0;
    if (targetGrDb <= 1.0e-3 || hi <= lo) { if (iterations) *iterations = its; return hi; }
    double fhi = f (hi), flo = f (lo);
    double thr = hi;
    if (fhi >= 0.0) thr = hi;
    else if (flo <= 0.0) thr = lo;
    else
    {
        // Illinois regula falsi on a monotonic function, 0.01 dB of GR is plenty.
        int side = 0;
        for (int it = 0; it < 40; ++it)
        {
            const double t = hi - fhi * (hi - lo) / (fhi - flo);
            const double ft = f (t);
            thr = t;
            if (std::abs (ft) < 0.01 || hi - lo < 1.0e-3) break;
            if (ft > 0.0) { lo = t; flo = ft; if (side == -1) fhi *= 0.5; side = -1; }
            else          { hi = t; fhi = ft; if (side == +1) flo *= 0.5; side = +1; }
        }
    }
    if (iterations) *iterations = its;
    return thr;
}

//==============================================================================
bool widthIsNeutral (const WidthSettings& s)
{
    return ! s.enabled || (std::abs (s.width - 1.0) < 1.0e-9 && s.monoBelowHz <= 0.0 && std::abs (s.highWidth - 1.0) < 1.0e-9);
}

namespace
{
double corrOf (const std::vector<double>& a, const std::vector<double>& b, double& ea, double& eb)
{
    double ab = 0.0;
    ea = eb = 0.0;
    for (size_t i = 0; i < a.size(); ++i) { ab += a[i] * b[i]; ea += a[i] * a[i]; eb += b[i] * b[i]; }
    return (ea > 0.0 && eb > 0.0) ? ab / std::sqrt (ea * eb) : 1.0;
}
} // namespace

double lowBandCorrelation (const juce::AudioBuffer<float>& b, double fs, double fc)
{
    if (b.getNumChannels() < 2) return 1.0;
    const int n = b.getNumSamples();
    auto l = zeroPhaseLowPass (b.getReadPointer (0), n, fs, fc), r = zeroPhaseLowPass (b.getReadPointer (1), n, fs, fc);
    double el, er;
    return corrOf (l, r, el, er);
}

void processWidth (juce::AudioBuffer<float>& b, double fs, const WidthSettings& s, double* lowCorrOut, double* safeSignOut, double* safeGainOut)
{
    if (b.getNumChannels() < 2 || widthIsNeutral (s)) return;
    const int n = b.getNumSamples();
    float* l = b.getWritePointer (0);
    float* r = b.getWritePointer (1);
    std::vector<double> side ((size_t) n), low, high;
    for (int i = 0; i < n; ++i) side[(size_t) i] = 0.5 * ((double) l[i] - r[i]);
    if (s.monoBelowHz > 0.0)
    {
        low = side;
        auto lp = Biquad::butterworthLowPass (fs, s.monoBelowHz);
        for (int i = 0; i < n; ++i) low[(size_t) i] = lp.process (low[(size_t) i]);
        lp.reset();
        for (int i = n - 1; i >= 0; --i) low[(size_t) i] = lp.process (low[(size_t) i]);
    }
    const bool hiOn = std::abs (s.highWidth - 1.0) > 1.0e-9 && s.highWidthHz > 0.0;
    if (hiOn)
    {
        // Side above highWidthHz = side - LR4 low-pass (zero phase, complementary).
        high = side;
        zeroPhaseLowPassInPlace (high, fs, s.highWidthHz);
        for (int i = 0; i < n; ++i) high[(size_t) i] = side[(size_t) i] - high[(size_t) i];
    }
    // Phase-safe low mono: the low band is summed with the polarity that adds, and its energy restored.
    double safeSign = 1.0, safeGain = 1.0;
    std::vector<double> lowL, lowR;
    if (s.phaseSafeLowMono && s.monoBelowHz > 0.0)
    {
        lowL = zeroPhaseLowPass (l, n, fs, s.monoBelowHz);
        lowR = zeroPhaseLowPass (r, n, fs, s.monoBelowHz);
        double el, er;
        const double rho = corrOf (lowL, lowR, el, er);
        if (lowCorrOut != nullptr) *lowCorrOut = rho;
        safeSign = rho < 0.0 ? -1.0 : 1.0;
        double em = 0.0;
        for (int i = 0; i < n; ++i) { const double m = 0.5 * (lowL[(size_t) i] + safeSign * lowR[(size_t) i]); em += m * m; }
        if (em > 0.0) safeGain = juce::jlimit (1.0, 2.0, std::sqrt (0.5 * (el + er) / em));
    }
    if (safeSignOut != nullptr) *safeSignOut = safeSign;
    if (safeGainOut != nullptr) *safeGainOut = safeGain;
    for (int i = 0; i < n; ++i)
    {
        double sd = side[(size_t) i];
        if (! low.empty()) sd -= low[(size_t) i];
        double hs = 0.0;
        if (hiOn) { hs = high[(size_t) i]; sd -= hs; }
        sd = (sd + hs * s.highWidth) * s.width;
        if (! lowL.empty())
        {
            // Mid without its low band, plus the phase-safe low mono.
            const double midHigh = 0.5 * ((double) l[i] + r[i]) - 0.5 * (lowL[(size_t) i] + lowR[(size_t) i]);
            const double lowMono = safeGain * 0.5 * (lowL[(size_t) i] + safeSign * lowR[(size_t) i]);
            // The side's own low band (zero-phase LR4, 'low') is already removed from sd; lowL - lowR is the same band of 2 x side.
            l[i] = (float) (midHigh + lowMono + sd);
            r[i] = (float) (midHigh + lowMono - sd);
        }
        else
        {
            const double mid = 0.5 * ((double) l[i] + r[i]);
            l[i] = (float) (mid + sd);
            r[i] = (float) (mid - sd);
        }
    }
}

//==============================================================================
void zeroPhaseLowPassInPlace (std::vector<double>& x, double fs, double fc)
{
    auto lp = Biquad::butterworthLowPass (fs, juce::jlimit (1.0, fs * 0.45, fc));
    const int n = (int) x.size();
    for (int i = 0; i < n; ++i) x[(size_t) i] = lp.process (x[(size_t) i]);
    lp.reset();
    for (int i = n - 1; i >= 0; --i) x[(size_t) i] = lp.process (x[(size_t) i]);
}

std::vector<double> zeroPhaseLowPass (const float* x, int n, double fs, double fc)
{
    std::vector<double> y (x, x + n);
    zeroPhaseLowPassInPlace (y, fs, fc);
    return y;
}

void splitThreeBands (const juce::AudioBuffer<float>& in, double fs, double f1, double f2,
                      juce::AudioBuffer<float>& low, juce::AudioBuffer<float>& mid, juce::AudioBuffer<float>& high)
{
    const int chans = in.getNumChannels(), n = in.getNumSamples();
    low.setSize (chans, n, false, false, true);
    mid.setSize (chans, n, false, false, true);
    high.setSize (chans, n, false, false, true);
    for (int c = 0; c < chans; ++c)
    {
        const float* x = in.getReadPointer (c);
        auto lo = zeroPhaseLowPass (x, n, fs, f1);
        std::vector<double> rest ((size_t) n);
        for (int i = 0; i < n; ++i) rest[(size_t) i] = (double) x[i] - lo[(size_t) i];
        auto md = rest;
        zeroPhaseLowPassInPlace (md, fs, f2);
        float* L = low.getWritePointer (c);
        float* M = mid.getWritePointer (c);
        float* H = high.getWritePointer (c);
        for (int i = 0; i < n; ++i)
        {
            L[i] = (float) lo[(size_t) i];
            M[i] = (float) md[(size_t) i];
            H[i] = (float) (rest[(size_t) i] - md[(size_t) i]);
        }
    }
}

void dcFilter (juce::AudioBuffer<float>& b, double fs)
{
    const int n = b.getNumSamples();
    for (int c = 0; c < b.getNumChannels(); ++c)
    {
        float* x = b.getWritePointer (c);
        std::vector<double> y (x, x + n);
        auto hp = Biquad::butterworthHighPass (fs, 5.0);
        for (int i = 0; i < n; ++i) y[(size_t) i] = hp.process (y[(size_t) i]);
        hp.reset();
        for (int i = n - 1; i >= 0; --i) y[(size_t) i] = hp.process (y[(size_t) i]);
        for (int i = 0; i < n; ++i) x[i] = (float) y[(size_t) i];
    }
}

void applyBalance (juce::AudioBuffer<float>& b, double balanceDb)
{
    if (b.getNumChannels() < 2 || std::abs (balanceDb) < 1.0e-9) return;
    b.applyGain (0, 0, b.getNumSamples(), (float) dbToGain (-0.5 * balanceDb));
    b.applyGain (1, 0, b.getNumSamples(), (float) dbToGain (0.5 * balanceDb));
}

//==============================================================================
Biquad dynBandComponent (const DynBandSettings& s, double fs)
{
    const double f = juce::jlimit (10.0, fs * 0.45, s.freqHz);
    if (s.shape == DynBandSettings::lowShelf)
        return Biquad::butterworthLowPass (fs, f);
    // RBJ band-pass, constant 0 dB peak gain: 1 + (g - 1) * BP is then a peaking bell of gain g at f.
    const double w0 = 2.0 * kPi * f / fs, alpha = std::sin (w0) / (2.0 * juce::jmax (0.1, s.q)), a0 = 1.0 + alpha;
    Biquad q;
    q.b0 = alpha / a0; q.b1 = 0.0; q.b2 = -alpha / a0;
    q.a1 = -2.0 * std::cos (w0) / a0; q.a2 = (1.0 - alpha) / a0;
    return q;
}

void processDynBand (juce::AudioBuffer<float>& b, double fs, const DynBandSettings& s, const std::vector<uint8_t>& mask, DynBandResult& res)
{
    res = {};
    if (! s.enabled || s.amountDb <= 0.0 || s.rangeDb <= 0.0) return;
    const int chans = b.getNumChannels(), n = b.getNumSamples();
    juce::AudioBuffer<float> comp (chans, n);
    for (int c = 0; c < chans; ++c)
    {
        auto q = dynBandComponent (s, fs);
        const float* x = b.getReadPointer (c);
        float* y = comp.getWritePointer (c);
        for (int i = 0; i < n; ++i) y[i] = (float) q.process ((double) x[i]);
    }
    CompSettings c;
    c.ratio = s.ratio; c.attackMs = s.attackMs; c.releaseMs = s.releaseMs; c.autoRelease = false;
    c.rmsBlend = 0.7; c.scHpfHz = 0.0; c.kneeDb = 6.0; c.maxGrDb = s.rangeDb;
    std::vector<float> level, gr;
    compressorDetector (comp, fs, c, level);
    c.thresholdDb = solveGlueThreshold (level, mask, fs, c, juce::jmin (s.amountDb, s.rangeDb));
    compressorGainDb (level, fs, c, gr);
    std::vector<float> gain ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        const double g = std::pow (10.0, (double) gr[(size_t) i] / 20.0);
        gain[(size_t) i] = (float) g;
        for (int ch = 0; ch < chans; ++ch)
            b.getWritePointer (ch)[i] += (float) ((g - 1.0) * comp.getReadPointer (ch)[i]);
    }
    res.active = true;
    res.thresholdDb = c.thresholdDb;
    maskedGrStats (gr, mask, res.loudMeanCutDb, res.maxCutDb);
    res.gr = gainStats (gain, fs);
    res.maxCutDb = res.gr.maxDb;
}

//==============================================================================
int limiterLookaheadSamples (const LimiterSettings& s, double fs)
{
    return juce::jmax (1, (int) std::lround (juce::jlimit (0.1, 10.0, s.lookaheadMs) * 1.0e-3 * fs));
}

double limiterReleaseCoef (const LimiterSettings& s, double fs) { return coef (juce::jmax (1.0, s.releaseMs), fs); }

void limiterGainCurve (const std::vector<float>& tp, double drive, double ceiling, int L, double relCoef, std::vector<float>& gain)
{
    const int n = (int) tp.size();
    gain.resize ((size_t) n);
    // Required gain, then the look-ahead minimum h[i] = min(req[i .. i+L-1]) (monotonic deque, backwards).
    std::vector<float> h ((size_t) n);
    std::deque<int> dq;
    auto req = [&] (int i)
    {
        const double v = (double) tp[(size_t) i] * drive;
        return v > ceiling ? (float) (ceiling / v) : 1.0f;
    };
    std::vector<float> rq ((size_t) n);
    for (int i = 0; i < n; ++i) rq[(size_t) i] = req (i);
    for (int i = n - 1; i >= 0; --i)
    {
        while (! dq.empty() && rq[(size_t) dq.back()] >= rq[(size_t) i]) dq.pop_back();
        dq.push_back (i);
        while (dq.front() >= i + L) dq.pop_front();
        h[(size_t) i] = rq[(size_t) dq.front()];
    }
    // Instant attack to h, exponential release towards it. Before the first sample the
    // gain is taken as h[0] (the minimum over the first L samples), so peaks right at
    // the start are covered by the average too.
    const float before = n > 0 ? h[0] : 1.0f;
    float prev = before;
    for (int i = 0; i < n; ++i)
    {
        const float t = h[(size_t) i];
        prev = t < prev ? t : (float) (t + (prev - t) * relCoef);
        h[(size_t) i] = prev;
    }
    // Moving average of the last L values.
    double sum = (double) L * before;
    for (int i = 0; i < n; ++i)
    {
        sum += (double) h[(size_t) i] - (i >= L ? (double) h[(size_t) (i - L)] : (double) before);
        if ((i & 4095) == 4095)
        {
            // Re-sum exactly now and then so rounding cannot drift.
            sum = 0.0;
            for (int j = i - L + 1; j <= i; ++j) sum += j >= 0 ? (double) h[(size_t) j] : (double) before;
        }
        gain[(size_t) i] = (float) juce::jmin (1.0, sum / L);
    }
}

void applyDriveAndLimit (const juce::AudioBuffer<float>& in, const std::vector<float>& tp, double fs,
                         const LimiterSettings& s, double driveDb, juce::AudioBuffer<float>& out, std::vector<float>& g)
{
    const int n = in.getNumSamples(), chans = in.getNumChannels();
    out.setSize (chans, n, false, false, true);
    const double drive = dbToGain (driveDb);
    if (s.enabled)
        limiterGainCurve (tp, drive, dbToGain (s.ceilingDbtp), limiterLookaheadSamples (s, fs), limiterReleaseCoef (s, fs), g);
    else
        g.assign ((size_t) n, 1.0f);
    for (int c = 0; c < chans; ++c)
    {
        const float* x = in.getReadPointer (c);
        float* y = out.getWritePointer (c);
        for (int i = 0; i < n; ++i)
            y[i] = x[i] * (float) (drive * g[(size_t) i]);
    }
}

LimiterResult limitWithSafety (const juce::AudioBuffer<float>& in, const std::vector<float>& tp, double fs,
                               const LimiterSettings& s, juce::AudioBuffer<float>& out)
{
    LimiterResult res;
    std::vector<float> g, total;
    applyDriveAndLimit (in, tp, fs, s, s.driveDb, out, g);
    total = g;
    res.passes = 1;
    const int n = out.getNumSamples();
    if (! s.enabled)
    {
        res.gr = gainStats (total, fs);
        res.truePeakOut = truePeakMax (out, kLimiterOversampling);
        return res;
    }
    const double ceiling = dbToGain (s.ceilingDbtp);
    auto env = truePeakEnvelope (out, kLimiterOversampling);
    auto maxOf = [] (const std::vector<float>& v) { float m = 0.0f; for (float x : v) m = juce::jmax (m, x); return m; };
    float peak = maxOf (env);
    LimiterSettings pass = s;
    while (peak > ceiling * (1.0 + 1.0e-6) && res.passes < 4)
    {
        limiterGainCurve (env, 1.0, ceiling, limiterLookaheadSamples (pass, fs), limiterReleaseCoef (pass, fs), g);
        for (int c = 0; c < out.getNumChannels(); ++c)
        {
            float* y = out.getWritePointer (c);
            for (int i = 0; i < n; ++i) y[i] *= g[(size_t) i];
        }
        for (int i = 0; i < n; ++i) total[(size_t) i] *= g[(size_t) i];
        ++res.passes;
        env = truePeakEnvelope (out, kLimiterOversampling);
        peak = maxOf (env);
    }
    if (peak > ceiling * (1.0 + 1.0e-6))
    {
        const float trim = (float) (ceiling / peak) * (1.0f - 1.0e-6f);
        out.applyGain (trim);
        for (auto& v : total) v *= trim;
        res.finalTrimDb = gainToDb (trim);
        peak *= trim;
    }
    res.truePeakOut = peak;
    res.gr = gainStats (total, fs);
    return res;
}
} // namespace mihenk
