#include "Proxy.h"
#include "MasterChain.h"
#include "../dsp/Equalizer.h"
#include "../dsp/Filters.h"
#include "../dsp/Spectrum.h"
#include <complex>

namespace mihenk
{
void LoudnessProxy::prepare()
{
    energyLin.resize (energyDb.size());
    peakLin.resize (peakDb.size());
    for (size_t i = 0; i < energyDb.size(); ++i) energyLin[i] = std::pow (10.0, (double) energyDb[i] / 10.0);
    for (size_t i = 0; i < peakDb.size(); ++i) peakLin[i] = std::pow (10.0, (double) peakDb[i] / 20.0);
}

LoudnessProxy makeLoudnessProxy (const juce::AudioBuffer<float>& b, const std::vector<float>& tp, double fs)
{
    LoudnessProxy p;
    const int segLen = loudnessSegmentLength (fs);
    p.hop = juce::jmax (1, segLen / 10);
    const int n = b.getNumSamples();
    if (n < segLen * 4 || b.getNumChannels() < 1) return p;
    const int blocks = (n / segLen) * 10;
    const auto e = kWeightedBlockEnergies (b, fs, p.hop);
    p.energyDb.resize ((size_t) blocks);
    p.peakDb.resize ((size_t) blocks);
    for (int k = 0; k < blocks; ++k)
    {
        const double en = (size_t) k < e.size() ? e[(size_t) k] : 0.0;
        p.energyDb[(size_t) k] = (float) juce::jmax (-200.0, 10.0 * std::log10 (en + 1.0e-20));
        float pk = 0.0f;
        if (! tp.empty())
        {
            const int base = k * p.hop;
            for (int i = 0; i < p.hop && base + i < (int) tp.size(); ++i) pk = juce::jmax (pk, tp[(size_t) (base + i)]);
        }
        p.peakDb[(size_t) k] = (float) juce::jmax (-200.0, 20.0 * std::log10 ((double) pk + 1.0e-12));
    }
    p.sampleRate = fs;

    // Each 100 ms segment's spectrum, as the share of every half-octave band in its K-weighted energy.
    {
        const int segs = blocks / 10;
        int order = 8;
        while ((1 << order) < (int) (fs * 0.085)) ++order;
        const int N = 1 << order;
        juce::dsp::FFT fft (order);
        std::vector<float> win ((size_t) N);
        for (int i = 0; i < N; ++i) win[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * kPi * i / N));
        const auto shelf = Biquad::kWeightingShelf (fs), hp = Biquad::kWeightingHighPass (fs);
        std::vector<double> kw ((size_t) N / 2 + 1);
        std::vector<int> bandOf ((size_t) N / 2 + 1, 0);
        for (int k = 0; k <= N / 2; ++k)
        {
            const double f = juce::jmax (1.0, (double) k * fs / N);
            kw[(size_t) k] = std::pow (shelf.magnitude (juce::jmin (f, fs * 0.499), fs) * hp.magnitude (juce::jmin (f, fs * 0.499), fs), 2.0);
            int band = (int) std::floor (2.0 * std::log2 (f / LoudnessProxy::bandEdgeHz (0)));
            bandOf[(size_t) k] = juce::jlimit (0, LoudnessProxy::numBands - 1, band);
        }
        p.shareDb.assign ((size_t) segs * LoudnessProxy::numBands, -90.0f);
        std::vector<std::complex<float>> in ((size_t) N), spec ((size_t) N);
        const int chans = juce::jmin (2, b.getNumChannels());
        const float* l = b.getReadPointer (0);
        const float* r = chans > 1 ? b.getReadPointer (1) : nullptr;
        std::vector<double> bandPow (LoudnessProxy::numBands);
        for (int sIdx = 0; sIdx < segs; ++sIdx)
        {
            const int start = sIdx * segLen + segLen / 2 - N / 2;
            for (int i = 0; i < N; ++i)
            {
                const int k = start + i;
                const bool inside = k >= 0 && k < n;
                in[(size_t) i] = { inside ? l[k] * win[(size_t) i] : 0.0f, (inside && r != nullptr) ? r[k] * win[(size_t) i] : 0.0f };
            }
            fft.perform (in.data(), spec.data(), false);
            std::fill (bandPow.begin(), bandPow.end(), 0.0);
            double total = 0.0;
            for (int k = 1; k <= N / 2; ++k)
            {
                const auto zk = spec[(size_t) k], zn = std::conj (spec[(size_t) ((N - k) % N)]);
                const auto xl = (zk + zn) * 0.5f;
                const auto xr = (zk - zn) * std::complex<float> (0.0f, -0.5f);
                const double pw = ((double) std::norm (xl) + (r != nullptr ? (double) std::norm (xr) : 0.0)) * kw[(size_t) k];
                bandPow[(size_t) bandOf[(size_t) k]] += pw;
                total += pw;
            }
            for (int bi = 0; bi < LoudnessProxy::numBands; ++bi)
            {
                const double share = total > 0.0 ? bandPow[(size_t) bi] / total : 1.0 / LoudnessProxy::numBands;
                p.shareDb[(size_t) sIdx * LoudnessProxy::numBands + (size_t) bi] = (float) juce::jmax (-90.0, 10.0 * std::log10 (share + 1.0e-12));
            }
        }
    }
    p.valid = true;
    p.prepare();
    return p;
}

namespace
{
/** Modelled loudness and limiting: each 10 ms block's energy and peak scaled by the drive and the segment's tone factor, the limiter run per block. */
ProxyLevel modelLevel (const LoudnessProxy& p, double driveDb, const ToneShift* shift, const ProxyLimiter& lim)
{
    ProxyLevel out;
    const size_t nb = p.energyLin.size();
    const double trim = shift != nullptr ? shift->trimDb : 0.0;
    const double G = std::pow (10.0, (driveDb + trim) / 20.0);
    const double ceilLin = lim.on ? std::pow (10.0, lim.ceilingDb / 20.0) : 1.0e30;
    const double rb = std::exp (-(double) p.hop / (p.sampleRate * juce::jmax (1.0, lim.releaseMs) * 1.0e-3));
    std::vector<double> seg (nb / 10, 0.0);
    double gPrev = 1.0, sumGr = 0.0, maxGr = 0.0;
    for (size_t k = 0; k < nb; ++k)
    {
        const size_t sIdx = k / 10;
        const double power = shift != nullptr && sIdx < shift->segmentPower.size() ? shift->segmentPower[sIdx] : 1.0;
        const double peakF = shift != nullptr && sIdx < shift->segmentPeak.size() ? shift->segmentPeak[sIdx] : 1.0;
        const double amp = std::sqrt (power) * G;
        const double pk = p.peakLin[k] * peakF * G;
        const double req = pk > ceilLin ? ceilLin / pk : 1.0;
        const double gStart = gPrev;
        const double g = req < gPrev ? req : req + (gPrev - req) * rb;
        gPrev = g;
        // Between its peaks the real limiter's gain recovers inside the block: the block's mean gain is above the one after the attack.
        const double gm = 0.5 * (gStart + g);
        const double ge = gm + p.recovery * (1.0 - rb) * (1.0 - gm);
        if (sIdx < seg.size()) seg[sIdx] += p.energyLin[k] * amp * amp * ge * ge;
        const double gr = g < 1.0 ? -20.0 * std::log10 (juce::jmax (1.0e-9, g)) : 0.0;
        sumGr += gr;
        maxGr = juce::jmax (maxGr, gr);
    }
    out.lufs = integratedFromSegments (seg, p.hop * 10);
    out.grMaxDb = maxGr;
    out.grMeanDb = sumGr / (double) nb;
    return out;
}
} // namespace

ProxyLevel proxyLevel (const LoudnessProxy& p, double driveDb, const ToneShift& shift, const ProxyLimiter* limiterNow)
{
    const size_t nb = p.energyLin.size();
    if (! p.valid || nb < 40 || p.peakLin.size() != nb) return {};
    const ProxyLimiter baseLim { p.limiterOn, p.ceilingDb, p.releaseMs };
    auto out = modelLevel (p, driveDb, &shift, limiterNow != nullptr ? *limiterNow : baseLim);
    // Calibrate on the real render the proxy was made with: the same model at the baseline drive and limiter.
    const auto base = modelLevel (p, p.baselineDriveDb, nullptr, baseLim);
    if (std::isfinite (p.baselineLufs) && std::isfinite (base.lufs) && std::isfinite (out.lufs)) out.lufs += p.baselineLufs - base.lufs;
    return out;
}

void fitProxyRecovery (LoudnessProxy& p, double probeDriveDb, double probeLufs)
{
    if (! p.valid || ! std::isfinite (probeLufs)) return;
    p.recovery = 0.0;
    auto at = [&] (double kappa)
    {
        p.recovery = kappa;
        return proxyLevel (p, probeDriveDb).lufs - probeLufs;
    };
    // More recovery gives a louder model: bisect for the match, within what is physical.
    if (at (0.0) >= 0.0) { p.recovery = 0.0; return; }
    double lo = 0.0, hi = 40.0;
    if (at (hi) < 0.0) { p.recovery = hi; return; }
    for (int i = 0; i < 30; ++i)
    {
        const double mid = 0.5 * (lo + hi);
        if (at (mid) < 0.0) lo = mid; else hi = mid;
    }
    p.recovery = 0.5 * (lo + hi);
}

ToneShift estimateToneShift (const LoudnessProxy& p, const MasterSettings& base, double atBase, const MasterSettings& now, double atNow)
{
    ToneShift t;
    t.trimDb = (now.trimDb + (now.autoTrim ? atNow : 0.0)) - (base.trimDb + (base.autoTrim ? atBase : 0.0));
    const int nbands = LoudnessProxy::numBands;
    if (p.shareDb.empty() || p.shareDb.size() % (size_t) nbands != 0) return t;
    // The EQ change in every band: the mean power gain over five log-spaced points of the band.
    std::vector<double> gain ((size_t) nbands, 1.0);
    bool any = false;
    for (int bi = 0; bi < nbands; ++bi)
    {
        double sum = 0.0;
        for (int j = 0; j < 5; ++j)
        {
            const double f = juce::jmin (LoudnessProxy::bandEdgeHz (bi) * std::pow (2.0, 0.5 * (0.1 + 0.2 * j)), p.sampleRate * 0.49);
            const double eb = base.eq.enabled ? eqTotalDb (base.eq, f) : 0.0, en = now.eq.enabled ? eqTotalDb (now.eq, f) : 0.0;
            sum += std::pow (10.0, (en - eb) / 10.0);
        }
        gain[(size_t) bi] = sum / 5.0;
        any |= std::abs (gain[(size_t) bi] - 1.0) > 1.0e-6;
    }
    if (! any) return t;
    const size_t segs = p.shareDb.size() / (size_t) nbands;
    t.segmentPower.assign (segs, 1.0);
    t.segmentPeak.assign (segs, 1.0);
    for (size_t k = 0; k < segs; ++k)
    {
        double f = 0.0, pa = 0.0, pb = 0.0;
        for (int bi = 0; bi < nbands; ++bi)
        {
            const double share = std::pow (10.0, (double) p.shareDb[k * (size_t) nbands + (size_t) bi] / 10.0);
            f += share * gain[(size_t) bi];
            pa += std::sqrt (share * gain[(size_t) bi]);
            pb += std::sqrt (share);
        }
        t.segmentPower[k] = f;
        // A peak is a sum of the bands, which add coherently at the top of the waveform (pa / pb) but not always in step: an EQ
        // change moves it half way between "not at all" and the coherent sum.
        t.segmentPeak[k] = pb > 0.0 ? 0.5 + 0.5 * (pa / pb) : 1.0;
    }
    return t;
}

ProxySolve solveDriveFromProxy (const LoudnessProxy& p, const MasterSettings& s, const ToneShift& shift, const Measurement& source)
{
    ProxySolve r;
    if (! p.valid) return r;
    const bool keep = s.targetPreset == TargetPreset::keepInput;
    double T = presetLufs (s.targetPreset, s.targetLufs);
    if (keep && source.valid && std::isfinite (source.integrated)) T = std::round (source.integrated * 100.0) / 100.0;
    r.targetLufs = T;
    const double minDrive = -36.0;
    double maxDrive = juce::jlimit (minDrive, 36.0, s.limiter.maxDriveDb);
    const ProxyLimiter lim { s.limiter.enabled, s.limiter.ceilingDbtp, s.limiter.releaseMs };
    auto level = [&] (double d) { return proxyLevel (p, d, shift, &lim); };
    if (keep)
    {
        // As MasterChain::matchTarget: at most kKeepMaxExtraDriveDb above the level-neutral drive.
        const ProxyLimiter open { false, 0.0, 150.0 };
        const double lpre = proxyLevel (p, 0.0, shift, &open).lufs;
        if (std::isfinite (lpre)) maxDrive = juce::jmin (maxDrive, juce::jlimit (minDrive, 36.0, T - lpre + kKeepMaxExtraDriveDb));
    }
    // Guard for a finished master (same limits as the render).
    const double plr = source.samplePeak - source.integrated;
    const bool finished = ! keep && s.limiter.enabled && source.valid && std::isfinite (plr) && plr < kDensePlrLu
                          && source.samplePeak >= s.limiter.ceilingDbtp - kFinishedHeadroomDb;
    auto within = [&] (double d)
    {
        const auto l = level (d);
        return l.grMaxDb <= kFinishedMaxPeakGrDb - 0.1 && l.grMeanDb <= kFinishedMaxMeanGrDb - 0.1 && l.lufs - source.integrated <= kFinishedMaxBoostDb - 0.05;
    };
    const double top = level (maxDrive).lufs, bottom = level (minDrive).lufs;
    if (! std::isfinite (top) || ! std::isfinite (bottom)) return r;
    // Where the loudness curve flattens a small model error is a large drive error and more limiting for nothing: the model
    // is solved to 0.1 LU under the target (the engine itself accepts 0.2 LU).
    const double Ts = T - 0.1;
    double drive;
    if (top < Ts) drive = maxDrive;
    else if (bottom > Ts) drive = minDrive;
    else
    {
        double lo = minDrive, hi = maxDrive;
        for (int i = 0; i < 40 && hi - lo > 0.005; ++i)
        {
            const double mid = 0.5 * (lo + hi);
            if (level (mid).lufs < Ts) lo = mid; else hi = mid;
        }
        drive = 0.5 * (lo + hi);
    }
    bool guarded = false;
    if (finished && ! within (drive))
    {
        double lo = minDrive, hi = drive;
        for (int i = 0; i < 40 && hi - lo > 0.005; ++i)
        {
            const double mid = 0.5 * (lo + hi);
            if (within (mid)) lo = mid; else hi = mid;
        }
        drive = lo;
        guarded = true;
    }
    r.ok = true;
    r.driveDb = drive;
    r.lufs = level (drive).lufs;
    if (guarded)
        r.message = "Target not reachable: this master is already finished; held at " + fmt (r.lufs) + " LUFS (about, from the saved analysis).";
    else if (top < Ts)
        r.message = "Target not reachable: about " + fmt (r.lufs) + " LUFS at the largest drive (from the saved analysis).";
    return r;
}
} // namespace mihenk
