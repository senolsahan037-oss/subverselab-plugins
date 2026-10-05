#include "Diagnosis.h"
#include "MasterChain.h"
#include "../dsp/Loudness.h"
#include "../dsp/Spectrum.h"
#include "../dsp/Filters.h"
#include "../dsp/Dynamics.h"

namespace mihenk
{
namespace
{
struct ScenarioInfo { const char* id; const char* name; };
const ScenarioInfo kScenarios[kNumScenarios] {
    { "too-hot", "Too hot, no headroom" },
    { "clipped", "Clipped input" },
    { "too-quiet", "Too quiet, lots of headroom" },
    { "phase", "Phase problems in the lows" },
    { "too-wide", "Stereo too wide in the highs" },
    { "too-narrow", "Stereo too narrow" },
    { "imbalance", "L/R imbalance" },
    { "dc-offset", "DC offset" },
    { "muddy", "Muddy low-mids" },
    { "boomy", "Boomy, loose sub" },
    { "thin", "Thin, weak low end" },
    { "harsh", "Harsh upper mids" },
    { "sibilance", "Sibilance, hissy top" },
    { "dull", "Dull, dark top" },
    { "buried-vocal", "Buried vocal, weak presence" },
    { "over-compressed", "Over-compressed mix" },
    { "too-dynamic", "Too dynamic" },
    { "uneven-low", "Uneven low end between sections" },
    { "resonance", "Resonant peak" },
    { "codec-risk", "Codec clipping risk" },
};

double dB10 (double e) { return e > 0.0 ? 10.0 * std::log10 (e) : -300.0; }

double energy (const std::vector<double>& x) { double s = 0.0; for (double v : x) s += v * v; return s; }

double percentile (std::vector<float> v, double p)
{
    if (v.empty()) return 0.0;
    const size_t k = (size_t) juce::jlimit (0.0, (double) v.size() - 1.0, std::floor (p * (double) (v.size() - 1)));
    std::nth_element (v.begin(), v.begin() + (long) k, v.end());
    return v[k];
}

/** Long-term power spectrum of the channel mean on a 1/24-octave grid (20 Hz * 2^(i/24)), band power in dB. */
std::vector<float> fineSpectrum (const std::vector<double>& mono, double fs, std::vector<double>& grid)
{
    grid.clear();
    for (int i = 0; i <= 240; ++i) grid.push_back (20.0 * std::pow (2.0, i / 24.0));
    std::vector<float> out (grid.size(), -150.0f);
    const int n = (int) mono.size();
    int order = 12;
    while ((1 << order) < (int) (fs * 0.68)) ++order;
    const int N = 1 << order, hop = N / 2;
    if (n < N / 4) return out;
    juce::dsp::FFT fft (order);
    std::vector<float> win ((size_t) N), buf ((size_t) N * 2);
    for (int i = 0; i < N; ++i) win[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * kPi * i / N));
    std::vector<double> power ((size_t) N / 2 + 1, 0.0);
    const int frames = n <= N ? 1 : 1 + (n - N) / hop;
    for (int f = 0; f < frames; ++f)
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        for (int i = 0; i < N; ++i)
        {
            const int k = f * hop + i;
            buf[(size_t) i] = k < n ? (float) mono[(size_t) k] * win[(size_t) i] : 0.0f;
        }
        fft.performFrequencyOnlyForwardTransform (buf.data());
        for (int k = 0; k <= N / 2; ++k) power[(size_t) k] += (double) buf[(size_t) k] * buf[(size_t) k];
    }
    const double df = fs / N;
    for (size_t g = 0; g < grid.size(); ++g)
    {
        const double lo = grid[g] * std::pow (2.0, -1.0 / 48.0), hi = grid[g] * std::pow (2.0, 1.0 / 48.0);
        double sum = 0.0;
        const int k0 = juce::jmax (1, (int) std::floor (lo / df - 0.5)), k1 = juce::jmin (N / 2, (int) std::ceil (hi / df + 0.5));
        for (int k = k0; k <= k1; ++k)
        {
            const double a = juce::jmax (lo, (k - 0.5) * df), z = juce::jmin (hi, (k + 0.5) * df);
            if (z > a) sum += power[(size_t) k] * (z - a) / df;
        }
        out[g] = (float) (sum > 0.0 ? juce::jmax (-150.0, 10.0 * std::log10 (sum)) : -150.0);
    }
    return out;
}

struct Line { double slope = 0.0, icpt = 0.0; double at (double f) const { return icpt + slope * std::log2 (f / 1000.0); } };

/** Least-squares line over lo..hi, leaving out exLo..exHi (the region under test, so its own bump cannot pull the line up). */
Line fitLine (const std::vector<float>& spec, double lo, double hi, double exLo = 0.0, double exHi = 0.0)
{
    const auto& grid = spectrumGrid();
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    int c = 0;
    for (size_t i = 0; i < spec.size() && i < grid.size(); ++i)
        if (grid[i] >= lo && grid[i] <= hi && ! (grid[i] >= exLo && grid[i] <= exHi))
        {
            const double x = std::log2 (grid[i] / 1000.0), y = spec[i];
            sx += x; sy += y; sxx += x * x; sxy += x * y; ++c;
        }
    Line l;
    if (c >= 2 && std::abs (c * sxx - sx * sx) > 1.0e-12)
    {
        l.slope = (c * sxy - sx * sy) / (c * sxx - sx * sx);
        l.icpt = (sy - l.slope * sx) / c;
    }
    return l;
}

double meanDev (const std::vector<float>& spec, const Line& l, double lo, double hi, double* peakHz = nullptr)
{
    const auto& grid = spectrumGrid();
    double s = 0.0, best = -1.0e9;
    int c = 0;
    for (size_t i = 0; i < spec.size() && i < grid.size(); ++i)
        if (grid[i] >= lo && grid[i] <= hi)
        {
            const double d = spec[i] - l.at (grid[i]);
            s += d; ++c;
            if (d > best) { best = d; if (peakHz) *peakHz = grid[i]; }
        }
    return c > 0 ? s / c : 0.0;
}

juce::String f1 (double v) { return fmt (v, 1); }
juce::String sgn (double v, int d = 1) { return (v > 0.0 ? "+" : "") + fmt (v, d); }
juce::String hzText (double v) { return v >= 1000.0 ? fmt (v / 1000.0, v < 10000.0 ? 2 : 1) + " kHz" : fmt (v, 0) + " Hz"; }
} // namespace

const char* scenarioId (int s) { return s >= 1 && s <= kNumScenarios ? kScenarios[s - 1].id : ""; }
const char* scenarioName (int s) { return s >= 1 && s <= kNumScenarios ? kScenarios[s - 1].name : ""; }
int scenarioFromId (const juce::String& id)
{
    for (int s = 1; s <= kNumScenarios; ++s) if (id == kScenarios[s - 1].id) return s;
    return 0;
}

const Finding* Diagnosis::find (int scenario) const
{
    for (auto& f : findings) if (f.scenario == scenario) return &f;
    return nullptr;
}

//==============================================================================
DiagnosisMeasures measureMix (const juce::AudioBuffer<float>& b, double fs)
{
    DiagnosisMeasures m;
    const int n = b.getNumSamples(), chans = b.getNumChannels();
    if (n == 0 || chans == 0 || fs <= 0.0) return m;
    const auto a = analyse (b, fs, true);
    m.valid = a.valid && std::isfinite (a.integrated);
    m.stereo = chans >= 2;
    m.seconds = a.seconds;
    m.integrated = a.integrated; m.shortTermMax = a.shortTermMax; m.lra = a.lra; m.truePeak = a.truePeak; m.samplePeak = a.samplePeak;
    m.rms = a.rms; m.crest = a.crest; m.correlation = a.correlation; m.spectrumDb = a.spectrumDb;
    m.plr = m.samplePeak - m.integrated;
    m.ispDb = m.truePeak - m.samplePeak;
    m.dense = m.valid && (m.plr < kDensePlrLu || m.crest < kDenseCrestDb || m.lra < kDenseLraLu);
    m.overCompressed = m.valid && (m.plr < kDensePlrLu || m.crest < kDenseCrestDb);
    if (! m.valid) return m;

    // 2. Flat-top runs at the file peak.
    {
        float peak = 0.0f;
        for (int c = 0; c < chans; ++c) peak = juce::jmax (peak, b.getMagnitude (c, 0, n));
        const float thr = 0.999f * peak, tol = 1.0e-6f * peak;
        for (int c = 0; c < chans; ++c)
        {
            const float* x = b.getReadPointer (c);
            int run = 1;
            for (int i = 1; i <= n; ++i)
            {
                const bool same = i < n && std::abs (x[i]) >= thr && std::abs (x[i] - x[i - 1]) <= tol;
                if (same) ++run;
                else
                {
                    if (run >= diag::clipMinRunSamples) ++m.clipRuns;
                    run = 1;
                }
            }
        }
        m.clipRunsPerMin = m.clipRuns / juce::jmax (1.0 / 60.0, m.seconds / 60.0);
    }

    // 8. DC.
    double dcMax = 0.0;
    for (int c = 0; c < chans; ++c)
    {
        double s = 0.0;
        const float* x = b.getReadPointer (c);
        for (int i = 0; i < n; ++i) s += x[i];
        dcMax = juce::jmax (dcMax, std::abs (s / n));
    }
    m.dcDbfs = gainToDb (dcMax);

    // Channel mean for the mono measures.
    std::vector<double> mono ((size_t) n, 0.0);
    for (int c = 0; c < chans; ++c)
    {
        const float* x = b.getReadPointer (c);
        for (int i = 0; i < n; ++i) mono[(size_t) i] += (double) x[i] / chans;
    }

    // Stereo measures.
    if (m.stereo)
    {
        const float* l = b.getReadPointer (0);
        const float* r = b.getReadPointer (1);
        std::vector<double> L (l, l + n), R (r, r + n);
        m.balanceDb = dB10 (energy (L)) - dB10 (energy (R));
        auto lowL = zeroPhaseLowPass (l, n, fs, 150.0), lowR = zeroPhaseLowPass (r, n, fs, 150.0);
        const double el = energy (lowL), er = energy (lowR);
        m.lowBalanceDb = dB10 (el) - dB10 (er);
        double lr = 0.0, em = 0.0;
        for (int i = 0; i < n; ++i)
        {
            lr += lowL[(size_t) i] * lowR[(size_t) i];
            const double mm = 0.5 * (lowL[(size_t) i] + lowR[(size_t) i]);
            em += mm * mm;
        }
        m.lowCorrelation = (el > 0.0 && er > 0.0) ? lr / std::sqrt (el * er) : 1.0;
        m.monoLowLossDb = (el + er) > 0.0 ? dB10 (em) - dB10 (0.5 * (el + er)) : 0.0;

        std::vector<double> mid ((size_t) n), side ((size_t) n);
        for (int i = 0; i < n; ++i) { mid[(size_t) i] = 0.5 * (L[(size_t) i] + R[(size_t) i]); side[(size_t) i] = 0.5 * (L[(size_t) i] - R[(size_t) i]); }
        m.sideMidDb = dB10 (energy (side)) - dB10 (energy (mid));
        auto hi = [fs] (std::vector<double> x)
        {
            auto lp = x;
            zeroPhaseLowPassInPlace (lp, fs, 4000.0);
            for (size_t i = 0; i < x.size(); ++i) x[i] -= lp[i];
            return x;
        };
        m.sideMidHighDb = dB10 (energy (hi (side))) - dB10 (energy (hi (mid)));
    }

    // Low-band crest (< 100 Hz).
    {
        auto low = mono;
        zeroPhaseLowPassInPlace (low, fs, 100.0);
        double pk = 0.0;
        for (double v : low) pk = juce::jmax (pk, std::abs (v));
        const double e = energy (low) / n;
        m.lowCrestDb = (pk > 0.0 && e > 0.0) ? gainToDb (pk) - dB10 (e) : 0.0;
    }

    // Spectral balance.
    {
        const auto& s = m.spectrumDb;
        // Each region is measured against a line fitted without it (the region +-1/2 octave left out).
        m.lineSlopeDbPerOct = fitLine (s, 100.0, 10000.0).slope;
        m.mudDb = meanDev (s, fitLine (s, 100.0, 10000.0, 140.0, 710.0), 200.0, 500.0, &m.mudPeakHz);
        m.harshDb = meanDev (s, fitLine (s, 100.0, 10000.0, 1400.0, 7100.0), 2000.0, 5000.0, &m.harshPeakHz);
        const auto pl = fitLine (s, 100.0, 10000.0, 300.0, 4000.0);
        m.presenceDb = meanDev (s, pl, 1000.0, 4000.0) - meanDev (s, pl, 300.0, 1000.0);
        m.subDb = meanInRange (s, 25.0, 60.0) - meanInRange (s, 100.0, 300.0);
        m.lowDb = meanInRange (s, 40.0, 120.0) - meanInRange (s, 300.0, 1000.0);
        m.airRelDb = meanInRange (s, 10000.0, 16000.0) - meanInRange (s, 1000.0, 4000.0);
    }

    // 13. Sibilance: 5-9 kHz against the rest of the signal, 20 ms RMS, every 10 ms, where the music plays.
    {
        juce::AudioBuffer<float> monoBuf (1, n);
        for (int i = 0; i < n; ++i) monoBuf.setSample (0, i, (float) mono[(size_t) i]);
        const auto rel = harshRatioDb (monoBuf, fs);
        const double a = std::exp (-1.0 / (0.02 * fs));
        std::vector<float> full ((size_t) n);
        double e = 0.0;
        for (int i = 0; i < n; ++i) { e = a * e + (1.0 - a) * mono[(size_t) i] * mono[(size_t) i]; full[(size_t) i] = (float) dB10 (e + 1.0e-20); }
        const int step = juce::jmax (1, (int) (0.01 * fs));
        std::vector<float> fl;
        for (int i = step; i < n; i += step) fl.push_back (full[(size_t) i]);
        const double gate = percentile (fl, 0.95) - 20.0;
        std::vector<float> rs;
        for (int i = step; i < n; i += step) if (full[(size_t) i] >= gate) rs.push_back (rel[(size_t) i]);
        if (! rs.empty())
        {
            const double p99 = percentile (rs, 0.99), med = percentile (rs, 0.5);
            m.sibAbsDb = p99;
            m.sibPeakDb = p99 - med;
        }
    }

    // 18. Low band against the full band over time (3 s windows, 0.5 s hop), where the music is within 15 dB of its loudest.
    {
        auto low = mono;
        zeroPhaseLowPassInPlace (low, fs, 120.0);      // zero phase, so x - low is exactly the LR4 high band
        const int win = (int) (3.0 * fs), hop = (int) (0.5 * fs);
        std::vector<float> fullW, diffW;
        for (int s0 = 0; s0 + win <= n; s0 += hop)
        {
            // Low band against the rest of the mix (above 120 Hz): the full band would be dominated by the lows themselves.
            double ef = 0.0, eL = 0.0, eR = 0.0;
            for (int i = s0; i < s0 + win; ++i)
            {
                const double x = mono[(size_t) i], l = low[(size_t) i];
                ef += x * x; eL += l * l; eR += (x - l) * (x - l);
            }
            fullW.push_back ((float) dB10 (ef / win));
            diffW.push_back ((float) (dB10 (eL / win) - dB10 (eR / win)));
        }
        if (fullW.size() >= 4)
        {
            const float top = *std::max_element (fullW.begin(), fullW.end());
            std::vector<float> d;
            for (size_t i = 0; i < fullW.size(); ++i)
                if (fullW[i] >= top - 8.0f && diffW[i] > -80.0f) d.push_back (diffW[i]);
            // Sections where the bass is essentially absent (12 dB under its usual share) are arrangement, not mix.
            if (d.size() >= 4)
            {
                const double usual = percentile (d, 0.9);
                d.erase (std::remove_if (d.begin(), d.end(), [usual] (float v) { return v < usual - 12.0; }), d.end());
            }
            if (d.size() >= 4) m.unevenLowDb = percentile (d, 0.8) - percentile (d, 0.2);
        }
    }

    // 19. Resonance: 1/24-octave band against its +-1/2 octave surroundings (the +-1/8 octave next to it left out).
    {
        std::vector<double> grid;
        const auto fine = fineSpectrum (mono, fs, grid);
        for (int i = 0; i < (int) grid.size(); ++i)
        {
            if (grid[(size_t) i] < 150.0 || grid[(size_t) i] > 10000.0 || grid[(size_t) i] > fs * 0.4) continue;
            double s = 0.0;
            int c = 0;
            for (int j = i - 12; j <= i + 12; ++j)
                if (std::abs (j - i) > 3 && j >= 0 && j < (int) grid.size()) { s += fine[(size_t) j]; ++c; }
            const double prom = fine[(size_t) i] - s / juce::jmax (1, c);
            if (prom > m.resonanceDb) { m.resonanceDb = prom; m.resonanceHz = grid[(size_t) i]; }
        }
    }
    return m;
}

//==============================================================================
Diagnosis diagnose (const juce::AudioBuffer<float>& b, double fs, const MasterSettings& current)
{
    return diagnose (measureMix (b, fs), current);
}

Diagnosis diagnose (const DiagnosisMeasures& m, const MasterSettings& current)
{
    Diagnosis d;
    d.m = m;
    if (! m.valid) return d;
    auto add = [&] (int sc, Severity sev, double value, const juce::String& reason)
    {
        Finding f;
        f.scenario = sc;
        f.id = scenarioId (sc);
        f.title = scenarioName (sc);
        f.severity = sev;
        f.value = value;
        f.reason = reason;
        auto after = current;
        applyScenario (after, sc, &m);
        f.delta = describeDelta (current, after);
        d.findings.push_back (f);
    };
    using namespace diag;

    // 1. Too hot.
    if (m.samplePeak >= hotSamplePeakDbfs || m.truePeak >= hotTruePeakDbtp)
        add (scTooHot, m.truePeak > 0.0 ? Severity::problem : Severity::warn, m.truePeak,
             "Input is " + f1 (m.samplePeak - kWorkingPeakDbfs) + " dB hotter than a mastering-ready level (peak " + fmt (kWorkingPeakDbfs, 0)
             + " dBFS): true peak " + sgn (m.truePeak, 2) + " dBTP, sample peak " + sgn (m.samplePeak, 2) + " dBFS, no headroom");
    // 2. Clipped.
    if (m.clipRuns >= clipMinRuns && m.clipRunsPerMin >= clipRunsPerMinWarn)
        add (scClipped, m.clipRunsPerMin >= clipRunsPerMinProblem ? Severity::problem : Severity::warn, m.clipRunsPerMin,
             juce::String (m.clipRuns) + " flat-top runs (>= " + juce::String (clipMinRunSamples) + " equal samples at the peak), " + f1 (m.clipRunsPerMin)
             + " per minute: the mix was clipped; re-bounce it without clipping if you can");
    // 3. Too quiet.
    if (m.samplePeak < quietPeakDbfs)
        add (scTooQuiet, m.samplePeak < quietPeakProblemDbfs ? Severity::warn : Severity::info, m.samplePeak,
             "Input peaks at " + fmt (m.samplePeak, 1) + " dBFS: " + f1 (kWorkingPeakDbfs - m.samplePeak) + " dB under the working level ("
             + fmt (kWorkingPeakDbfs, 0) + " dBFS), integrated " + f1 (m.integrated) + " LUFS");
    if (m.stereo)
    {
        // 4. Phase.
        if (m.lowCorrelation < phaseLowCorr || m.monoLowLossDb < phaseMonoLowLossDb || m.correlation < phaseCorr)
            add (scPhase, (m.lowCorrelation < -0.2 || m.correlation < -0.1) ? Severity::problem : Severity::warn, m.lowCorrelation,
                 "Low-band (< 150 Hz) correlation " + fmt (m.lowCorrelation, 2) + ", mono sum loses " + f1 (-m.monoLowLossDb) + " dB of the lows, overall correlation "
                 + fmt (m.correlation, 2));
        // 5. Too wide.
        if (m.sideMidHighDb > wideSideMidHighDb)
            add (scTooWide, m.sideMidHighDb > 1.0 ? Severity::problem : Severity::warn, m.sideMidHighDb,
                 "Above 4 kHz the side is " + sgn (m.sideMidHighDb) + " dB against the mid (limit " + fmt (wideSideMidHighDb, 1) + " dB): the top is wider than it is centred");
        // 6. Too narrow.
        if (m.sideMidDb < narrowSideMidDb)
            add (scTooNarrow, Severity::info, m.sideMidDb,
                 m.sideMidDb < -60.0 ? juce::String ("The two channels are identical (side " + fmt (m.sideMidDb, 0) + " dB): mono, there is no width to lift")
                                     : "Side " + f1 (m.sideMidDb) + " dB under the mid (correlation " + fmt (m.correlation, 2) + "): nearly mono");
        // 7. Imbalance.
        if (std::abs (m.balanceDb) > imbalanceDb || std::abs (m.lowBalanceDb) > imbalanceLowDb)
            add (scImbalance, std::abs (m.balanceDb) > 2.0 ? Severity::problem : Severity::warn, m.balanceDb,
                 juce::String (m.balanceDb >= 0.0 ? "Left" : "Right") + " is " + f1 (std::abs (m.balanceDb)) + " dB louder (RMS), below 150 Hz "
                 + f1 (std::abs (m.lowBalanceDb)) + " dB (" + (m.lowBalanceDb >= 0.0 ? "left" : "right") + ")");
    }
    // 8. DC offset.
    if (m.dcDbfs > dcDbfs)
        add (scDcOffset, m.dcDbfs > -40.0 ? Severity::problem : Severity::warn, m.dcDbfs,
             "DC offset " + f1 (m.dcDbfs) + " dBFS (" + fmt (dbToGain (m.dcDbfs) * 100.0, 2) + " % of full scale): wastes headroom and thumps at edits");
    // 9. Muddy.
    if (m.mudDb > mudDb)
        add (scMuddy, m.mudDb > mudProblemDb ? Severity::problem : Severity::warn, m.mudDb,
             "200-500 Hz sits " + sgn (m.mudDb) + " dB above the mix's own spectral line (most at " + hzText (m.mudPeakHz) + ")");
    // 10. Boomy.
    if ((m.subDb > boomySubDb && m.lowCrestDb < boomyLowCrestDb) || m.subDb > boomySubAloneDb)
        add (scBoomy, m.subDb > boomySubAloneDb ? Severity::problem : Severity::warn, m.subDb,
             "25-60 Hz is " + sgn (m.subDb) + " dB over 100-300 Hz and the lows (< 100 Hz) have only " + f1 (m.lowCrestDb) + " dB crest: a sustained, loose sub");
    // 11. Thin.
    if (m.lowDb < thinLowDb)
        add (scThin, m.lowDb < thinProblemDb ? Severity::problem : Severity::warn, m.lowDb,
             "40-120 Hz is " + sgn (m.lowDb) + " dB against 300 Hz-1 kHz: the low end is weak");
    // 12. Harsh.
    if (m.harshDb > harshDb)
        add (scHarsh, m.harshDb > harshProblemDb ? Severity::problem : Severity::warn, m.harshDb,
             "2-5 kHz sits " + sgn (m.harshDb) + " dB above the mix's own spectral line (most at " + hzText (m.harshPeakHz) + ")");
    // 13. Sibilance.
    if (m.sibPeakDb > sibPeakDb && m.sibAbsDb > sibAbsDb)
        add (scSibilance, m.sibPeakDb > sibPeakDb + 4.0 ? Severity::problem : Severity::warn, m.sibPeakDb,
             "5-9 kHz peaks stand " + f1 (m.sibPeakDb) + " dB over their typical level (99th percentile " + f1 (m.sibAbsDb) + " dB against the rest of the signal)");
    // 14. Dull.
    if (m.airRelDb < dullDb)
        add (scDull, m.airRelDb < dullProblemDb ? Severity::problem : Severity::warn, m.airRelDb,
             "10-16 kHz is " + sgn (m.airRelDb) + " dB against 1-4 kHz (limit " + fmt (dullDb, 0) + " dB): the top is dark");
    // 15. Buried vocal.
    if (m.presenceDb < presenceDipDb)
        add (scBuriedVocal, m.presenceDb < presenceProblemDb ? Severity::problem : Severity::warn, m.presenceDb,
             "1-4 kHz dips " + f1 (-m.presenceDb) + " dB against 300 Hz-1 kHz (relative to the spectral line): weak presence");
    // 16. Over-compressed.
    if (m.overCompressed)
    {
        juce::StringArray why;
        if (m.plr < kDensePlrLu) why.add ("PLR " + f1 (m.plr) + " LU");
        if (m.crest < kDenseCrestDb) why.add ("crest " + f1 (m.crest) + " dB");
        add (scOverCompressed, m.plr < overCompPlrProblem ? Severity::problem : Severity::warn, m.plr,
             "Already dense (" + why.joinIntoString (", ") + "; finished masters sit at PLR 8-10 LU): more glue or limiting only flattens it");
    }
    // 17. Too dynamic.
    if (m.lra > dynamicLraLu || m.plr > dynamicPlrLu)
        add (scTooDynamic, m.lra > dynamicLraProblemLu ? Severity::problem : Severity::warn, m.lra,
             "Loudness range " + f1 (m.lra) + " LU, PLR " + f1 (m.plr) + " LU (limits " + fmt (dynamicLraLu, 0) + " LU / " + fmt (dynamicPlrLu, 0) + " LU): quiet parts will drop out on the target");
    // 18. Uneven low end.
    if (m.unevenLowDb > unevenLowDb)
        add (scUnevenLow, m.unevenLowDb > unevenLowProblemDb ? Severity::problem : Severity::warn, m.unevenLowDb,
             "The low band (< 120 Hz) against the rest of the mix swings " + f1 (m.unevenLowDb) + " dB between the main sections (20th to 80th percentile, 3 s windows)");
    // 19. Resonance.
    if (m.resonanceDb > resonanceDb)
        add (scResonance, m.resonanceDb > resonanceProblemDb ? Severity::warn : Severity::info, m.resonanceDb,
             "Narrow peak at " + hzText (m.resonanceHz) + ": " + sgn (m.resonanceDb) + " dB over its surroundings (1/24 octave against +-1/2 octave); a held note reads the same, check by ear");
    // 20. Codec risk.
    if (m.truePeak > codecTruePeakDbtp + codecTolDb || (m.ispDb > codecIspDb && m.truePeak > codecIspMinTpDbtp))
        add (scCodecRisk, m.truePeak > 0.0 ? Severity::problem : Severity::warn, m.truePeak,
             "True peak " + sgn (m.truePeak, 2) + " dBTP, inter-sample overshoot " + fmt (m.ispDb, 2) + " dB: lossy encoding (AAC / MP3 / Opus) will clip");

    std::stable_sort (d.findings.begin(), d.findings.end(), [] (const Finding& x, const Finding& y) { return (int) x.severity > (int) y.severity; });
    return d;
}

//==============================================================================
void applyScenario (MasterSettings& s, int sc, const DiagnosisMeasures* m)
{
    auto toThreeBand = [&s]
    {
        if (s.comp.mode != CompMode::threeBand)
        {
            // Keep the character of the broadband glue in the bands the scenario does not target.
            s.comp.mode = CompMode::threeBand;
            for (auto& b : s.comp.bands) { b.glueDb = s.comp.glueDb; b.autoThreshold = true; b.bypass = false; b.solo = false; }
        }
        s.comp.enabled = true;
    };
    switch (sc)
    {
        case scTooHot:
            s.autoTrim = true;
            s.limiter.releaseMs = juce::jmax (s.limiter.releaseMs, 200.0);
            s.limiter.lookaheadMs = juce::jmax (s.limiter.lookaheadMs, 2.0);
            break;
        case scClipped:
            s.autoTrim = true;
            s.limiter.releaseMs = juce::jmax (s.limiter.releaseMs, 250.0);
            s.limiter.lookaheadMs = juce::jmax (s.limiter.lookaheadMs, 2.5);
            break;
        case scTooQuiet:
            s.autoTrim = true;
            break;
        case scPhase:
            s.width.enabled = true;
            s.width.phaseSafeLowMono = true;
            s.width.monoBelowHz = juce::jmax (s.width.monoBelowHz, 150.0);
            break;
        case scTooWide:
            s.width.enabled = true;
            s.width.highWidthHz = 4000.0;
            s.width.highWidth = m != nullptr ? juce::jlimit (0.5, 0.9, dbToGain (-4.0 - m->sideMidHighDb)) : 0.8;
            break;
        case scTooNarrow:
            s.width.enabled = true;
            s.width.highWidthHz = 2000.0;
            s.width.highWidth = 1.3;
            break;
        case scImbalance:
            if (m != nullptr) s.balanceDb = juce::jlimit (-6.0, 6.0, s.balanceDb + m->balanceDb);
            break;
        case scDcOffset:
            s.dcFilter = true;
            break;
        case scMuddy:
            s.eq.enabled = true;
            s.eq.dynLowMid.enabled = true;
            s.eq.dynLowMid.freqHz = m != nullptr ? juce::jlimit (200.0, 500.0, m->mudPeakHz) : 300.0;
            s.eq.dynLowMid.amountDb = m != nullptr ? juce::jlimit (1.0, 3.0, 0.6 * m->mudDb) : 1.5;
            break;
        case scBoomy:
            s.eq.enabled = true;
            s.eq.tightener.enabled = true;
            s.eq.tightener.amountDb = 2.0;
            if (! s.eq.lowCutOn) { s.eq.lowCutOn = true; s.eq.lowCutHz = 28.0; s.eq.lowCutSlope = 24; }
            toThreeBand();
            s.comp.bands[0].glueDb = 2.0; s.comp.bands[0].ratio = 3.0; s.comp.bands[0].attackMs = 10.0; s.comp.bands[0].releaseMs = 150.0;
            break;
        case scThin:
            s.eq.enabled = true;
            s.eq.lowShelfDb = juce::jmin (6.0, s.eq.lowShelfDb + (m != nullptr ? juce::jlimit (1.5, 4.0, -0.5 * m->lowDb) : 2.0));
            s.eq.lowShelfHz = 80.0;
            if (s.comp.mode == CompMode::threeBand) s.comp.bands[0].bypass = true;
            break;
        case scHarsh:
            s.eq.enabled = true;
            s.eq.dynUpperMid.enabled = true;
            s.eq.dynUpperMid.freqHz = m != nullptr ? juce::jlimit (2000.0, 5000.0, m->harshPeakHz) : 3500.0;
            s.eq.dynUpperMid.amountDb = m != nullptr ? juce::jlimit (1.0, 3.0, 0.6 * m->harshDb) : 1.5;
            break;
        case scSibilance:
            s.eq.enabled = true;
            s.eq.airDynamic = true;
            s.eq.airHz = 10000.0;
            s.eq.airDeHarshDb = m != nullptr ? juce::jlimit (3.0, 6.0, m->sibPeakDb - diag::sibPeakDb + 3.0) : 4.0;
            break;
        case scDull:
            s.eq.enabled = true;
            s.eq.airDb = m != nullptr ? juce::jlimit (1.5, 5.0, 0.25 * (diag::dullDb + 8.0 - m->airRelDb)) : 3.0;
            s.eq.airHz = 15000.0;
            break;
        case scBuriedVocal:
            s.eq.enabled = true;
            s.eq.bell2Db = m != nullptr ? juce::jlimit (1.5, 4.0, -0.75 * m->presenceDb) : 2.0;
            s.eq.bell2Hz = 2500.0;
            s.eq.bell2Q = 0.7;
            break;
        case scOverCompressed:
            s.comp.glueDb = juce::jmin (s.comp.glueDb, kDenseGlueDb);
            for (auto& b : s.comp.bands) b.glueDb = juce::jmin (b.glueDb, kDenseGlueDb);
            break;
        case scTooDynamic:
            toThreeBand();
            s.comp.bands[0].glueDb = 3.0; s.comp.bands[1].glueDb = 3.0; s.comp.bands[2].glueDb = 2.5;
            for (auto& b : s.comp.bands) { b.ratio = juce::jmax (b.ratio, 3.0); b.releaseMs = juce::jmax (b.releaseMs, 300.0); b.attackMs = juce::jmax (b.attackMs, 20.0); }
            s.limiter.enabled = true;
            break;
        case scUnevenLow:
            toThreeBand();
            // A slow, firm low band: it rides the bass line's level between sections rather than the kick hits.
            s.comp.bands[0].glueDb = 3.0; s.comp.bands[0].ratio = 4.0; s.comp.bands[0].attackMs = 60.0; s.comp.bands[0].releaseMs = 400.0;
            s.comp.bands[0].bypass = false;
            break;
        case scResonance:
            if (m != nullptr && m->resonanceHz > 0.0)
            {
                s.eq.enabled = true;
                s.eq.bell1Hz = m->resonanceHz;
                s.eq.bell1Db = -juce::jlimit (2.0, 6.0, 0.6 * m->resonanceDb);
                s.eq.bell1Q = 6.0;
            }
            break;
        case scCodecRisk:
            s.limiter.enabled = true;
            s.limiter.ceilingDbtp = juce::jmin (s.limiter.ceilingDbtp, (m != nullptr && m->ispDb > diag::codecIspDb) ? -1.5 : -1.0);
            break;
        default: break;
    }
}

juce::String describeDelta (const MasterSettings& a, const MasterSettings& b)
{
    using S = const MasterSettings&;
    auto onOff = [] (bool v) { return juce::String (v ? "on" : "off"); };
    auto dbT = [] (double v) { return sgn (v, 1) + " dB"; };
    auto hzT = [] (double v) { return hzText (v); };
    struct F { const char* name; std::function<juce::String (S)> text; };
    const F fields[] {
        { "TRIM ASSIST", [&] (S s) { return onOff (s.autoTrim); } },
        { "DC FILTER", [&] (S s) { return onOff (s.dcFilter); } },
        { "BALANCE", [&] (S s) { return dbT (s.balanceDb); } },
        { "TRIM", [&] (S s) { return dbT (s.trimDb); } },
        { "EQ", [&] (S s) { return onOff (s.eq.enabled); } },
        { "LOW CUT", [&] (S s) { return s.eq.lowCutOn ? hzT (s.eq.lowCutHz) + " " + juce::String (s.eq.lowCutSlope) + " dB/oct" : juce::String ("off"); } },
        { "LOW SHELF", [&] (S s) { return dbT (s.eq.lowShelfDb) + " @ " + hzT (s.eq.lowShelfHz); } },
        { "BELL 1", [&] (S s) { return dbT (s.eq.bell1Db) + " @ " + hzT (s.eq.bell1Hz) + " Q " + fmt (s.eq.bell1Q, 2); } },
        { "BELL 2", [&] (S s) { return dbT (s.eq.bell2Db) + " @ " + hzT (s.eq.bell2Hz) + " Q " + fmt (s.eq.bell2Q, 2); } },
        { "AIR", [&] (S s) { return dbT (s.eq.airDb) + " @ " + fmt (s.eq.airHz / 1000.0, 0) + " kHz"; } },
        { "AIR DYN", [&] (S s) { return s.eq.airDynamic ? "on, de-harsh " + fmt (s.eq.airDeHarshDb, 1) + " dB" : juce::String ("off"); } },
        { "DYN LOW-MID", [&] (S s) { return s.eq.dynLowMid.enabled ? hzT (s.eq.dynLowMid.freqHz) + ", cut " + fmt (s.eq.dynLowMid.amountDb, 1) + " dB" : juce::String ("off"); } },
        { "DYN UPPER-MID", [&] (S s) { return s.eq.dynUpperMid.enabled ? hzT (s.eq.dynUpperMid.freqHz) + ", cut " + fmt (s.eq.dynUpperMid.amountDb, 1) + " dB" : juce::String ("off"); } },
        { "TIGHTENER", [&] (S s) { return s.eq.tightener.enabled ? hzT (s.eq.tightener.freqHz) + ", " + fmt (s.eq.tightener.amountDb, 1) + " dB" : juce::String ("off"); } },
        { "COMP", [&] (S s) { return onOff (s.comp.enabled); } },
        { "MODE", [&] (S s) { return juce::String (s.comp.mode == CompMode::threeBand ? "3-BAND" : "WIDE"); } },
        { "GLUE", [&] (S s) { return fmt (s.comp.glueDb, 1) + " dB"; } },
        { "LOW BAND", [&] (S s) { const auto& x = s.comp.bands[0]; return x.bypass ? juce::String ("bypass") : "glue " + fmt (x.glueDb, 1) + " dB, " + fmt (x.ratio, 1) + ":1, " + fmt (x.attackMs, 0) + "/" + fmt (x.releaseMs, 0) + " ms"; } },
        { "MID BAND", [&] (S s) { const auto& x = s.comp.bands[1]; return x.bypass ? juce::String ("bypass") : "glue " + fmt (x.glueDb, 1) + " dB, " + fmt (x.ratio, 1) + ":1"; } },
        { "HIGH BAND", [&] (S s) { const auto& x = s.comp.bands[2]; return x.bypass ? juce::String ("bypass") : "glue " + fmt (x.glueDb, 1) + " dB, " + fmt (x.ratio, 1) + ":1"; } },
        { "WIDTH", [&] (S s) { return fmt (s.width.width * 100.0, 0) + " %"; } },
        { "MONO BELOW", [&] (S s) { return s.width.monoBelowHz > 0.0 ? hzT (s.width.monoBelowHz) + (s.width.phaseSafeLowMono ? " phase-safe" : "") : juce::String ("off"); } },
        { "HIGH WIDTH", [&] (S s) { return std::abs (s.width.highWidth - 1.0) < 1.0e-9 ? juce::String ("off") : fmt (s.width.highWidth * 100.0, 0) + " % above " + hzT (s.width.highWidthHz); } },
        { "LIMITER", [&] (S s) { return onOff (s.limiter.enabled); } },
        { "CEILING", [&] (S s) { return fmt (s.limiter.ceilingDbtp, 1) + " dBTP"; } },
        { "LIM RELEASE", [&] (S s) { return fmt (s.limiter.releaseMs, 0) + " ms"; } },
        { "LOOK-AHEAD", [&] (S s) { return fmt (s.limiter.lookaheadMs, 1) + " ms"; } },
    };
    juce::StringArray out;
    for (auto& f : fields)
    {
        const auto x = f.text (a), y = f.text (b);
        if (x != y) out.add (juce::String (f.name) + " " + x + " -> " + y);
    }
    return out.isEmpty() ? juce::String ("no change (already set)") : out.joinIntoString ("; ");
}
} // namespace mihenk
