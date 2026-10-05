#include "Spectrum.h"
#include "Filters.h"

namespace mihenk
{
const std::vector<double>& spectrumGrid()
{
    static const std::vector<double> grid = []
    {
        std::vector<double> g;
        for (int i = 0; i <= 120; ++i)
            g.push_back (20.0 * std::pow (2.0, i / 12.0));
        return g;
    }();
    return grid;
}

std::vector<float> longTermSpectrum (const juce::AudioBuffer<float>& b, double fs)
{
    const auto& grid = spectrumGrid();
    std::vector<float> out (grid.size(), -150.0f);
    const int n = b.getNumSamples(), chans = juce::jmin (2, b.getNumChannels());
    if (n == 0 || chans == 0 || fs <= 0.0) return out;

    int order = 10;
    while ((1 << order) < (int) (fs * 0.34)) ++order;
    const int N = 1 << order, hop = N / 2;
    juce::dsp::FFT fft (order);

    std::vector<float> win ((size_t) N);
    double winSq = 0.0;
    for (int i = 0; i < N; ++i)
    {
        win[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * kPi * i / N));
        winSq += (double) win[(size_t) i] * win[(size_t) i];
    }

    std::vector<double> power ((size_t) N / 2 + 1, 0.0);
    std::vector<std::complex<float>> in ((size_t) N), spec ((size_t) N);
    const int frames = n <= N ? 1 : 1 + (n - N) / hop;
    const float* l = b.getReadPointer (0);
    const float* r = chans > 1 ? b.getReadPointer (1) : nullptr;
    for (int f = 0; f < frames; ++f)
    {
        const int start = f * hop;
        for (int i = 0; i < N; ++i)
        {
            const int k = start + i;
            const float lv = k < n ? l[k] : 0.0f;
            const float rv = (r != nullptr && k < n) ? r[k] : 0.0f;
            in[(size_t) i] = { lv * win[(size_t) i], rv * win[(size_t) i] };
        }
        fft.perform (in.data(), spec.data(), false);
        // Two real channels in one complex FFT: L = (Z[k] + conj Z[N-k]) / 2, R = (Z[k] - conj Z[N-k]) / 2i.
        for (int k = 1; k <= N / 2; ++k)
        {
            const auto zk = spec[(size_t) k], zn = std::conj (spec[(size_t) ((N - k) % N)]);
            const auto xl = (zk + zn) * 0.5f;
            const auto xr = (zk - zn) * std::complex<float> (0.0f, -0.5f);
            power[(size_t) k] += (double) std::norm (xl) + (r != nullptr ? (double) std::norm (xr) : 0.0);
        }
    }
    const double norm = 2.0 / ((double) N * winSq * frames * chans);
    const double df = fs / N;
    for (auto& p : power) p *= norm;

    for (size_t g = 0; g < grid.size(); ++g)
    {
        const double lo = grid[g] * std::pow (2.0, -1.0 / 12.0), hi = grid[g] * std::pow (2.0, 1.0 / 12.0);
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

std::vector<float> smoothOctaves (const std::vector<float>& db, double octaves)
{
    const int h = juce::jmax (0, (int) std::lround (octaves * 0.5 * 12.0));
    const int last = (int) db.size() - 1;
    std::vector<float> out (db.size());
    for (int i = 0; i <= last; ++i)
    {
        const int k = juce::jmin (h, i, last - i);
        double s = 0.0;
        for (int j = i - k; j <= i + k; ++j) s += db[(size_t) j];
        out[(size_t) i] = (float) (s / (2 * k + 1));
    }
    return out;
}

double interpGrid (const std::vector<float>& db, double f)
{
    if (db.empty()) return 0.0;
    const double u = juce::jlimit (0.0, (double) db.size() - 1.0, 12.0 * std::log2 (juce::jmax (1.0e-3, f) / 20.0));
    const int i = juce::jmin ((int) u, (int) db.size() - 2);
    if (i < 0) return db[0];
    const double t = u - i;
    return db[(size_t) i] * (1.0 - t) + db[(size_t) i + 1] * t;
}

double meanInRange (const std::vector<float>& db, double lo, double hi)
{
    const auto& grid = spectrumGrid();
    double s = 0.0;
    int c = 0;
    for (size_t i = 0; i < db.size() && i < grid.size(); ++i)
        if (grid[i] >= lo && grid[i] <= hi) { s += db[i]; ++c; }
    return c > 0 ? s / c : 0.0;
}
} // namespace mihenk
