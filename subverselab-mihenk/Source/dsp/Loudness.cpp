#if defined (__APPLE__)
 #include <Accelerate/Accelerate.h>   // before JUCE, whose using-directive would make Point ambiguous
#endif
#include "Loudness.h"
#include "Filters.h"
#include "Spectrum.h"

namespace mihenk
{
namespace
{
/** K-weighted energy per 100 ms segment, summed over channels (weight 1.0 each). */
std::vector<double> segmentEnergies (const juce::AudioBuffer<float>& b, double fs, int& segLen)
{
    segLen = juce::jmax (1, (int) std::lround (0.1 * fs));
    const int n = b.getNumSamples();
    const int numSegs = n / segLen;
    std::vector<double> seg ((size_t) numSegs, 0.0);
    for (int c = 0; c < b.getNumChannels(); ++c)
    {
        auto s1 = Biquad::kWeightingShelf (fs), s2 = Biquad::kWeightingHighPass (fs);
        const float* x = b.getReadPointer (c);
        for (int k = 0; k < numSegs; ++k)
        {
            double acc = 0.0;
            const int base = k * segLen;
            for (int i = 0; i < segLen; ++i)
            {
                const double y = s2.process (s1.process ((double) x[base + i]));
                acc += y * y;
            }
            seg[(size_t) k] += acc;
        }
    }
    return seg;
}

/** Mean-square energies of windows of `w` segments, hop one segment. */
std::vector<double> windows (const std::vector<double>& seg, int w, int segLen)
{
    std::vector<double> out;
    if ((int) seg.size() < w) return out;
    out.resize (seg.size() - (size_t) w + 1);
    double run = 0.0;
    for (int i = 0; i < w; ++i) run += seg[(size_t) i];
    const double norm = 1.0 / ((double) w * (double) segLen);
    for (size_t j = 0; j < out.size(); ++j)
    {
        out[j] = juce::jmax (0.0, run) * norm;
        if (j + (size_t) w < seg.size())
            run += seg[j + (size_t) w] - seg[j];
    }
    // Recompute exactly every so often would remove drift; the sums are double and short, so drift is < 1e-12 relative.
    return out;
}

double gatedIntegrated (const std::vector<double>& blocks)
{
    double sum = 0.0;
    int count = 0;
    for (double z : blocks)
        if (energyToLufs (z) > -70.0) { sum += z; ++count; }
    if (count == 0) return kNegInf;
    const double relGate = energyToLufs (sum / count) - 10.0;
    sum = 0.0; count = 0;
    for (double z : blocks)
    {
        const double l = energyToLufs (z);
        if (l > -70.0 && l > relGate) { sum += z; ++count; }
    }
    return count > 0 ? energyToLufs (sum / count) : kNegInf;
}

double loudnessRange (const std::vector<double>& shortTerm)
{
    double sum = 0.0;
    int count = 0;
    for (double z : shortTerm)
        if (energyToLufs (z) > -70.0) { sum += z; ++count; }
    if (count == 0) return 0.0;
    const double relGate = energyToLufs (sum / count) - 20.0;
    std::vector<double> kept;
    for (double z : shortTerm)
    {
        const double l = energyToLufs (z);
        if (l > -70.0 && l > relGate) kept.push_back (l);
    }
    if (kept.size() < 2) return 0.0;
    std::sort (kept.begin(), kept.end());
    auto pct = [&] (double p) { return kept[(size_t) std::lround (p * (double) (kept.size() - 1))]; };
    return pct (0.95) - pct (0.10);
}
} // namespace

LoudnessResult measureLoudness (const juce::AudioBuffer<float>& b, double fs)
{
    LoudnessResult r;
    int segLen = 0;
    const auto seg = segmentEnergies (b, fs, segLen);
    const auto mom = windows (seg, 4, segLen);
    const auto st = windows (seg, 30, segLen);
    r.integrated = gatedIntegrated (mom);
    r.lra = loudnessRange (st);
    r.momentary.reserve (mom.size());
    for (double z : mom)
    {
        const double l = energyToLufs (z);
        r.momentaryMax = juce::jmax (r.momentaryMax, l);
        r.momentary.push_back ((float) juce::jmax (-120.0, l));
    }
    r.shortTerm.reserve (st.size());
    for (double z : st)
    {
        const double l = energyToLufs (z);
        r.shortTermMax = juce::jmax (r.shortTermMax, l);
        r.shortTerm.push_back ((float) juce::jmax (-120.0, l));
    }
    return r;
}

int loudnessSegmentLength (double fs) { return juce::jmax (1, (int) std::lround (0.1 * fs)); }

std::vector<double> kWeightedBlockEnergies (const juce::AudioBuffer<float>& b, double fs, int blockLen)
{
    blockLen = juce::jmax (1, blockLen);
    const int n = b.getNumSamples(), numBlocks = n / blockLen;
    std::vector<double> out ((size_t) numBlocks, 0.0);
    for (int c = 0; c < b.getNumChannels(); ++c)
    {
        auto s1 = Biquad::kWeightingShelf (fs), s2 = Biquad::kWeightingHighPass (fs);
        const float* x = b.getReadPointer (c);
        for (int k = 0; k < numBlocks; ++k)
        {
            double acc = 0.0;
            const int base = k * blockLen;
            for (int i = 0; i < blockLen; ++i)
            {
                const double y = s2.process (s1.process ((double) x[base + i]));
                acc += y * y;
            }
            out[(size_t) k] += acc;
        }
    }
    return out;
}

double integratedFromSegments (const std::vector<double>& seg, int segLen) { return gatedIntegrated (windows (seg, 4, segLen)); }

double integratedLoudness (const juce::AudioBuffer<float>& b, double fs)
{
    int segLen = 0;
    const auto seg = segmentEnergies (b, fs, segLen);
    return gatedIntegrated (windows (seg, 4, segLen));
}

//==============================================================================
TruePeakDetector::TruePeakDetector (int f) : factor (juce::jmax (1, f)), coeff ((size_t) (factor * taps))
{
    // y(i + p/4) = sum_j c[p][j] * x[i + j - (taps/2 - 1)], c[p][j] = s(p/4 + taps/2 - 1 - j),
    // s(t) = sinc(t) * kaiser(t / (taps/2)). Each phase is normalised to unity DC gain.
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
}

void TruePeakDetector::envelope (const float* x, int n, float* out) const
{
    constexpr int front = taps / 2 - 1;
    std::vector<float> xp ((size_t) (n + taps + 1), 0.0f);
    std::copy (x, x + n, xp.begin() + front);
#if defined (__APPLE__)
    // Each phase is a 64-tap correlation: vDSP does it several times faster than a plain loop.
    std::vector<float> ph ((size_t) n);
    vDSP_vabs (x, 1, out, 1, (vDSP_Length) n);
    for (int p = 1; p < factor; ++p)
    {
        vDSP_conv (xp.data(), 1, coeff.data() + p * taps, 1, ph.data(), 1, (vDSP_Length) n, (vDSP_Length) taps);
        vDSP_vmaxmg (ph.data(), 1, out, 1, out, 1, (vDSP_Length) n);
    }
#else
    for (int i = 0; i < n; ++i)
    {
        float m = std::abs (x[i]);
        const float* w = xp.data() + i;
        for (int p = 1; p < factor; ++p)
        {
            const float* c = coeff.data() + p * taps;
            float acc = 0.0f;
            for (int j = 0; j < taps; ++j)
                acc += c[j] * w[j];
            m = juce::jmax (m, std::abs (acc));
        }
        out[i] = m;
    }
#endif
}

float TruePeakDetector::maxPeak (const float* x, int n) const
{
    std::vector<float> env ((size_t) n);
    envelope (x, n, env.data());
    float m = 0.0f;
    for (float v : env) m = juce::jmax (m, v);
    return m;
}

std::vector<float> truePeakEnvelope (const juce::AudioBuffer<float>& b, int factor)
{
    static const TruePeakDetector det4 (4), det8 (8);
    const TruePeakDetector local (factor);
    const auto& det = factor == 4 ? det4 : factor == 8 ? det8 : local;
    const int n = b.getNumSamples();
    std::vector<float> env ((size_t) n, 0.0f), tmp ((size_t) n);
    for (int c = 0; c < b.getNumChannels(); ++c)
    {
        det.envelope (b.getReadPointer (c), n, tmp.data());
        for (int i = 0; i < n; ++i)
            env[(size_t) i] = juce::jmax (env[(size_t) i], tmp[(size_t) i]);
    }
    return env;
}

float truePeakMax (const juce::AudioBuffer<float>& b, int factor)
{
    float m = 0.0f;
    for (float v : truePeakEnvelope (b, factor)) m = juce::jmax (m, v);
    return m;
}

//==============================================================================
Measurement analyse (const juce::AudioBuffer<float>& b, double fs, bool withSpectrum)
{
    Measurement m;
    m.sampleRate = fs;
    m.channels = b.getNumChannels();
    const int n = b.getNumSamples();
    m.seconds = fs > 0.0 ? (double) n / fs : 0.0;
    if (n == 0 || m.channels == 0) return m;

    const auto loud = measureLoudness (b, fs);
    m.integrated = loud.integrated;
    m.momentaryMax = loud.momentaryMax;
    m.shortTermMax = loud.shortTermMax;
    m.lra = loud.lra;
    m.shortTermHistory = loud.shortTerm;

    {
        const auto env = truePeakEnvelope (b, 4);
        float tpMax = 0.0f;
        for (int i = 0; i < n; ++i)
            if (env[(size_t) i] > tpMax) { tpMax = env[(size_t) i]; m.truePeakAt = i; }
        m.truePeak = gainToDb (tpMax, -300.0);
        if (m.truePeak <= -300.0) m.truePeak = kNegInf;
    }

    double peak = 0.0, sumSq = 0.0;
    for (int c = 0; c < m.channels; ++c)
    {
        const float* x = b.getReadPointer (c);
        for (int i = 0; i < n; ++i)
        {
            const double a = std::abs (x[i]);
            if (a > peak) { peak = a; m.samplePeakAt = i; }
            sumSq += (double) x[i] * x[i];
        }
    }
    m.samplePeak = peak > 0.0 ? 20.0 * std::log10 (peak) : kNegInf;
    const double ms = sumSq / ((double) n * m.channels);
    m.rms = ms > 0.0 ? 10.0 * std::log10 (ms) : kNegInf;
    m.crest = (peak > 0.0 && ms > 0.0) ? m.samplePeak - m.rms : 0.0;

    if (m.channels >= 2)
    {
        const float* l = b.getReadPointer (0);
        const float* r = b.getReadPointer (1);
        double lr = 0.0, ll = 0.0, rr = 0.0;
        for (int i = 0; i < n; ++i)
        {
            lr += (double) l[i] * r[i];
            ll += (double) l[i] * l[i];
            rr += (double) r[i] * r[i];
        }
        m.correlation = (ll > 0.0 && rr > 0.0) ? lr / std::sqrt (ll * rr) : 1.0;
    }

    if (withSpectrum)
        m.spectrumDb = longTermSpectrum (b, fs);
    m.valid = true;
    return m;
}

juce::String fmt (double v, int decimals)
{
    if (! std::isfinite (v)) return v < 0 ? "-inf" : "+inf";
    return juce::String (v, decimals);
}
} // namespace mihenk
