#include "Equalizer.h"
#include "Filters.h"
#include "Spectrum.h"
#include "Dynamics.h"

namespace mihenk
{
namespace
{
double bellDb (double f, double f0, double gainDb, double q)
{
    if (std::abs (gainDb) < 1.0e-9) return 0.0;
    const double A = std::pow (10.0, gainDb / 40.0), w = f / f0, u = 1.0 - w * w;
    const double num = u * u + (w * A / q) * (w * A / q);
    const double den = u * u + (w / (A * q)) * (w / (A * q));
    return 10.0 * std::log10 (num / den);
}

double lowShelfDb (double f, double f0, double gainDb)
{
    if (std::abs (gainDb) < 1.0e-9) return 0.0;
    const double A = std::pow (10.0, gainDb / 40.0), w = f / f0, w2 = w * w, k = A / 0.5;   // A / Q^2, Q = 1/sqrt 2
    const double num = A * A * ((A - w2) * (A - w2) + k * w2);
    const double den = (1.0 - A * w2) * (1.0 - A * w2) + k * w2;
    return 10.0 * std::log10 (num / den);
}

double highShelfDb (double f, double f0, double gainDb)
{
    if (std::abs (gainDb) < 1.0e-9) return 0.0;
    const double A = std::pow (10.0, gainDb / 40.0), w = f / f0, w2 = w * w, k = A / 0.5;
    const double num = A * A * ((1.0 - A * w2) * (1.0 - A * w2) + k * w2);
    const double den = (A - w2) * (A - w2) + k * w2;
    return 10.0 * std::log10 (num / den);
}

double lowCutDb (double f, double fc, int slope)
{
    const int order = slope >= 24 ? 4 : 2;
    if (f <= 0.0) return -300.0;
    const double w2n = std::pow (f / fc, 2.0 * order);
    return 10.0 * std::log10 (w2n / (1.0 + w2n));
}

double tiltDb (double f, double slope)
{
    if (std::abs (slope) < 1.0e-9) return 0.0;
    return slope * std::log2 (juce::jlimit (20.0, 20000.0, f) / 1000.0);
}

/** What the FIR carries: everything but the AIR shelf when AIR is dynamic. */
double eqFirDb (const EqSettings& s, double f)
{
    double db = eqTotalDb (s, f);
    if (s.airDynamic) db -= airShelfDb (f, s.airDb, s.airHz);
    return db;
}

/** Magnitude (linear) on the bins 0..N/2 of an N-point FFT. */
std::vector<double> sampledMagnitude (const EqSettings& s, double fs, int N)
{
    std::vector<double> m ((size_t) N / 2 + 1);
    for (int k = 0; k <= N / 2; ++k)
    {
        const double f = (double) k * fs / N;
        const double db = juce::jmax (-140.0, eqFirDb (s, juce::jmax (f, 1.0e-6)));
        m[(size_t) k] = std::pow (10.0, db / 20.0);
    }
    return m;
}

double airP (double f, double hz)
{
    const double w = juce::jmax (0.0, f) / juce::jmax (1.0, hz);
    return w / std::sqrt (1.0 + w * w);
}

/** Linear-phase FIR from a linear magnitude function (as the linear-phase EQ design). */
FirKernel linearPhaseFromMagnitude (const std::function<double (double)>& mag, double fs, int length = 0)
{
    const int N = length > 0 ? length : eqFirLength (fs);
    FirKernel k;
    k.taps.assign ((size_t) N, 0.0f);
    std::vector<std::complex<double>> x ((size_t) N);
    for (int i = 0; i <= N / 2; ++i)
    {
        const double m = mag ((double) i * fs / N);
        x[(size_t) i] = m;
        if (i > 0 && i < N / 2) x[(size_t) (N - i)] = m;
    }
    fftInPlace (x, true);
    const int c = N / 2;
    for (int n = 1; n < N; ++n)
        k.taps[(size_t) n] = (float) (x[(size_t) ((n - c + N) % N)].real() * kaiser ((double) (n - c) / c, 8.0));
    k.latency = c;
    return k;
}

float inverseFftScale (juce::dsp::FFT& fft, int size)
{
    // juce::dsp::FFT's inverse scaling depends on the engine; measure it once.
    std::vector<std::complex<float>> a ((size_t) size), b ((size_t) size);
    a[0] = 1.0f;
    fft.perform (a.data(), b.data(), false);
    fft.perform (b.data(), a.data(), true);
    return 1.0f / a[0].real();
}
} // namespace

double airShelfDb (double f, double gainDb, double hz)
{
    if (std::abs (gainDb) < 1.0e-9) return 0.0;
    return 20.0 * std::log10 (juce::jmax (1.0e-6, 1.0 + (std::pow (10.0, gainDb / 20.0) - 1.0) * airP (f, hz)));
}

double eqManualDb (const EqSettings& s, double f)
{
    double db = 0.0;
    if (s.lowCutOn) db += lowCutDb (f, s.lowCutHz, s.lowCutSlope);
    db += lowShelfDb (f, s.lowShelfHz, s.lowShelfDb);
    db += tiltDb (f, s.tiltDbPerOct);
    db += highShelfDb (f, s.highShelfHz, s.highShelfDb);
    db += bellDb (f, s.bell1Hz, s.bell1Db, s.bell1Q);
    db += bellDb (f, s.bell2Hz, s.bell2Db, s.bell2Q);
    db += airShelfDb (f, s.airDb, s.airHz);
    return db;
}

double eqTotalDb (const EqSettings& s, double f)
{
    double db = eqManualDb (s, f);
    if (! s.matchCurveDb.empty() && s.matchAmount > 0.0)
        db += s.matchAmount * interpGrid (s.matchCurveDb, f);
    return db;
}

bool eqIsNeutral (const EqSettings& s)
{
    auto zero = [] (double v) { return std::abs (v) < 1.0e-9; };
    bool matchZero = s.matchCurveDb.empty() || zero (s.matchAmount);
    if (! matchZero)
    {
        matchZero = true;
        for (float v : s.matchCurveDb) matchZero &= std::abs (v) < 1.0e-6f;
    }
    return ! s.lowCutOn && zero (s.lowShelfDb) && zero (s.tiltDbPerOct) && zero (s.highShelfDb)
           && zero (s.bell1Db) && zero (s.bell2Db) && matchZero && (s.airDynamic || zero (s.airDb));
}

bool dynamicEqActive (const EqSettings& s)
{
    auto on = [] (const DynBandSettings& b) { return b.enabled && b.amountDb > 0.0 && b.rangeDb > 0.0; };
    return on (s.dynLowMid) || on (s.dynUpperMid) || on (s.tightener)
           || (s.airDynamic && (std::abs (s.airDb) > 1.0e-9 || s.airDeHarshDb > 0.0));
}

int eqFirLength (double fs) { return fs <= 50000.0 ? 16384 : 32768; }
int airFirLength (double fs) { return fs <= 50000.0 ? 1024 : 2048; }

FirKernel designEqFir (const EqSettings& s, double fs)
{
    const int N = eqFirLength (fs);
    FirKernel k;
    k.taps.assign ((size_t) N, 0.0f);

    if (s.linearPhase)
        return linearPhaseFromMagnitude ([&s] (double f) { return std::pow (10.0, juce::jmax (-140.0, eqFirDb (s, juce::jmax (f, 1.0e-6))) / 20.0); }, fs);

    // Minimum phase from the real cepstrum, on a 4x longer grid to keep cepstral aliasing low.
    const int Nc = 4 * N;
    const auto mag = sampledMagnitude (s, fs, Nc);
    std::vector<std::complex<double>> x ((size_t) Nc);
    for (int i = 0; i <= Nc / 2; ++i)
    {
        const double lm = std::log (juce::jmax (1.0e-7, mag[(size_t) i]));
        x[(size_t) i] = lm;
        if (i > 0 && i < Nc / 2) x[(size_t) (Nc - i)] = lm;
    }
    fftInPlace (x, true);                         // real cepstrum
    for (int i = 1; i < Nc / 2; ++i) x[(size_t) i] *= 2.0;
    for (int i = Nc / 2 + 1; i < Nc; ++i) x[(size_t) i] = 0.0;
    fftInPlace (x, false);
    for (auto& v : x) v = std::exp (v);
    fftInPlace (x, true);                         // minimum-phase impulse response
    const int fade = N / 4;
    for (int n = 0; n < N; ++n)
    {
        double w = 1.0;
        if (n >= N - fade) w = 0.5 + 0.5 * std::cos (kPi * (double) (n - (N - fade)) / fade);
        k.taps[(size_t) n] = (float) (x[(size_t) n].real() * w);
    }
    k.latency = 0;
    return k;
}

FirKernel airPKernel (double hz, double fs)
{
    return linearPhaseFromMagnitude ([hz] (double f) { return airP (f, hz); }, fs, airFirLength (fs));
}

void convolveInPlace (juce::AudioBuffer<float>& buf, const FirKernel& kernel)
{
    const int n = buf.getNumSamples(), K = (int) kernel.taps.size();
    if (n == 0 || K == 0) return;
    int order = 1;
    while ((1 << order) < 2 * K) ++order;
    const int F = 1 << order, B = F - K + 1;
    juce::dsp::FFT fft (order);
    const float scale = inverseFftScale (fft, F);

    std::vector<std::complex<float>> kspec ((size_t) F), tmp ((size_t) F), spec ((size_t) F);
    for (int i = 0; i < K; ++i) tmp[(size_t) i] = kernel.taps[(size_t) i];
    fft.perform (tmp.data(), kspec.data(), false);

    // Channels in pairs: one complex FFT carries two real channels (the kernel is real).
    for (int c0 = 0; c0 < buf.getNumChannels(); c0 += 2)
    {
        const bool pair = c0 + 1 < buf.getNumChannels();
        std::vector<float> accL ((size_t) (n + F), 0.0f), accR (pair ? (size_t) (n + F) : 0, 0.0f);
        const float* l = buf.getReadPointer (c0);
        const float* r = pair ? buf.getReadPointer (c0 + 1) : nullptr;
        for (int pos = 0; pos < n; pos += B)
        {
            const int len = juce::jmin (B, n - pos);
            std::fill (tmp.begin(), tmp.end(), std::complex<float>());
            for (int i = 0; i < len; ++i)
                tmp[(size_t) i] = { l[pos + i], r != nullptr ? r[pos + i] : 0.0f };
            fft.perform (tmp.data(), spec.data(), false);
            for (int i = 0; i < F; ++i) spec[(size_t) i] *= kspec[(size_t) i];
            fft.perform (spec.data(), tmp.data(), true);
            const int valid = juce::jmin (F, n + F - pos);
            for (int i = 0; i < valid; ++i)
            {
                accL[(size_t) (pos + i)] += tmp[(size_t) i].real() * scale;
                if (pair) accR[(size_t) (pos + i)] += tmp[(size_t) i].imag() * scale;
            }
        }
        float* wl = buf.getWritePointer (c0);
        for (int i = 0; i < n; ++i) wl[i] = accL[(size_t) (i + kernel.latency)];
        if (pair)
        {
            float* wr = buf.getWritePointer (c0 + 1);
            for (int i = 0; i < n; ++i) wr[i] = accR[(size_t) (i + kernel.latency)];
        }
    }
}
} // namespace mihenk

namespace mihenk
{
namespace
{
/** One-pole smoothing of squares, then dB. */
void rmsDb (const std::vector<double>& x, double fs, double ms, std::vector<float>& out)
{
    const double a = std::exp (-1.0 / (ms * 1.0e-3 * fs));
    out.resize (x.size());
    double e = 0.0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        e = a * e + (1.0 - a) * x[i] * x[i];
        out[i] = (float) (10.0 * std::log10 (e + 1.0e-20));
    }
}
} // namespace

std::vector<float> harshRatioDb (const juce::AudioBuffer<float>& b, double fs)
{
    const int n = b.getNumSamples(), chans = b.getNumChannels();
    juce::AudioBuffer<float> sum (1, n);
    sum.clear();
    for (int c = 0; c < chans; ++c) sum.addFrom (0, 0, b, c, 0, n, 1.0f / (float) chans);
    // Zero-phase Linkwitz-Riley split at 5 and 9 kHz: the 5-9 kHz band against everything else, so an ess that
    // dominates its own frame still reads as far above the rest (a ratio to the full band would saturate at 0 dB).
    juce::AudioBuffer<float> lo, band, hi;
    splitThreeBands (sum, fs, 5000.0, juce::jmin (9000.0, fs * 0.45), lo, band, hi);
    std::vector<double> vb ((size_t) n), vr ((size_t) n);
    for (int i = 0; i < n; ++i) { vb[(size_t) i] = band.getSample (0, i); vr[(size_t) i] = (double) lo.getSample (0, i) + hi.getSample (0, i); }
    std::vector<float> eb, er;
    rmsDb (vb, fs, 20.0, eb);
    rmsDb (vr, fs, 20.0, er);
    for (size_t i = 0; i < eb.size(); ++i) eb[i] -= er[i];
    return eb;
}

void processAirDynamic (juce::AudioBuffer<float>& b, double fs, const EqSettings& s, const std::vector<uint8_t>& mask, AirDynamicResult& res)
{
    res = {};
    if (! s.airDynamic || (std::abs (s.airDb) < 1.0e-9 && s.airDeHarshDb <= 0.0)) return;
    const int n = b.getNumSamples(), chans = b.getNumChannels();
    const auto rel = harshRatioDb (b, fs);
    // Typical ratio: median over the loud part, on a 10 ms grid.
    std::vector<float> pick;
    const int step = juce::jmax (1, (int) (0.01 * fs));
    for (int i = 0; i < n; i += step)
        if (mask.empty() || mask[(size_t) i] != 0) pick.push_back (rel[(size_t) i]);
    if (pick.empty()) return;
    std::nth_element (pick.begin(), pick.begin() + (long) pick.size() / 2, pick.end());
    res.typicalRelDb = pick[pick.size() / 2];
    const double thr = res.typicalRelDb + kAirDeHarshMarginDb, range = juce::jmax (0.0, s.airDeHarshDb);
    const double aA = std::exp (-1.0 / (0.002 * fs)), aR = std::exp (-1.0 / (0.060 * fs));
    std::vector<float> red ((size_t) n);
    double env = 0.0, sum = 0.0;
    size_t cnt = 0;
    for (int i = 0; i < n; ++i)
    {
        const double want = juce::jlimit (0.0, range, (double) rel[(size_t) i] - thr);
        env = want > env ? aA * env + (1.0 - aA) * want : aR * env + (1.0 - aR) * want;
        red[(size_t) i] = (float) env;
        res.maxReductionDb = juce::jmax (res.maxReductionDb, env);
        if (mask.empty() || mask[(size_t) i] != 0) { sum += env; ++cnt; }
    }
    res.meanReductionDb = cnt > 0 ? sum / (double) cnt : 0.0;
    const double hz = s.airHz, air = juce::jmax (0.0, s.airDb);
    std::vector<float> gAir ((size_t) n), gCut ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        const double r = red[(size_t) i];
        gAir[(size_t) i] = (float) (std::pow (10.0, juce::jmax (0.0, air - r) / 20.0) - 1.0);
        const double cut = juce::jmax (0.0, r - air);
        gCut[(size_t) i] = (float) (1.0 - std::pow (10.0, -cut / 20.0));
        res.maxBandCutDb = juce::jmax (res.maxBandCutDb, cut);
    }
    juce::AudioBuffer<float> p (b);
    if (air > 0.0)
        convolveInPlace (p, airPKernel (hz, fs));
    juce::AudioBuffer<float> band;
    if (res.maxBandCutDb > 0.0)
    {
        // The cut is a bell at 7 kHz, Q 1.2 (-3 dB points about 4.6 and 10.6 kHz): B is its constant 0 dB peak
        // band-pass, so x - (1 - g) B has exactly the gain g at 7 kHz.
        DynBandSettings bell;
        bell.freqHz = kAirDeHarshBellHz;
        bell.q = kAirDeHarshBellQ;
        band = b;
        for (int c = 0; c < chans; ++c)
        {
            auto q = dynBandComponent (bell, fs);
            float* x = band.getWritePointer (c);
            for (int i = 0; i < n; ++i) x[i] = (float) q.process ((double) x[i]);
        }
    }
    for (int c = 0; c < chans; ++c)
    {
        float* y = b.getWritePointer (c);
        const float* q = p.getReadPointer (c);
        const float* bb = res.maxBandCutDb > 0.0 ? band.getReadPointer (c) : nullptr;
        for (int i = 0; i < n; ++i)
        {
            if (air > 0.0) y[i] += gAir[(size_t) i] * q[i];
            if (bb != nullptr) y[i] -= gCut[(size_t) i] * bb[i];
        }
    }
    res.active = true;
}
} // namespace mihenk
