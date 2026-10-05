#pragma once

#include <cmath>
#include <complex>
#include <vector>
#include <algorithm>

namespace mihenk
{
constexpr double kPi = 3.14159265358979323846;

/** Transposed direct form II biquad in double precision. a0 is normalised to 1. */
struct Biquad
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1 = 0.0, z2 = 0.0;

    inline double process (double x) noexcept
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() noexcept { z1 = z2 = 0.0; }

    /** Magnitude at frequency f, for checks. */
    double magnitude (double f, double fs) const
    {
        const std::complex<double> z = std::polar (1.0, -2.0 * kPi * f / fs);
        const auto num = b0 + b1 * z + b2 * z * z;
        const auto den = 1.0 + a1 * z + a2 * z * z;
        return std::abs (num / den);
    }

    static Biquad butterworthHighPass (double fs, double fc)
    {
        const double w0 = 2.0 * kPi * fc / fs, cs = std::cos (w0), alpha = std::sin (w0) / (2.0 * std::sqrt (0.5));
        const double a0 = 1.0 + alpha;
        Biquad q;
        q.b0 = (1.0 + cs) * 0.5 / a0; q.b1 = -(1.0 + cs) / a0; q.b2 = q.b0;
        q.a1 = -2.0 * cs / a0; q.a2 = (1.0 - alpha) / a0;
        return q;
    }

    static Biquad butterworthLowPass (double fs, double fc)
    {
        const double w0 = 2.0 * kPi * fc / fs, cs = std::cos (w0), alpha = std::sin (w0) / (2.0 * std::sqrt (0.5));
        const double a0 = 1.0 + alpha;
        Biquad q;
        q.b0 = (1.0 - cs) * 0.5 / a0; q.b1 = (1.0 - cs) / a0; q.b2 = q.b0;
        q.a1 = -2.0 * cs / a0; q.a2 = (1.0 - alpha) / a0;
        return q;
    }

    /** ITU-R BS.1770-4 K-weighting, stage 1 (the head's high shelf), for any sample rate.
        The analogue parameters are the ones that reproduce the standard's 48 kHz
        coefficients exactly (the libebur128 / pyloudnorm derivation). */
    static Biquad kWeightingShelf (double fs)
    {
        const double G = 3.999843853973347, fc = 1681.974450955533, Q = 0.7071752369554196;
        const double K = std::tan (kPi * fc / fs);
        const double Vh = std::pow (10.0, G / 20.0), Vb = std::pow (Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        Biquad q;
        q.b0 = (Vh + Vb * K / Q + K * K) / a0;
        q.b1 = 2.0 * (K * K - Vh) / a0;
        q.b2 = (Vh - Vb * K / Q + K * K) / a0;
        q.a1 = 2.0 * (K * K - 1.0) / a0;
        q.a2 = (1.0 - K / Q + K * K) / a0;
        return q;
    }

    /** BS.1770-4 K-weighting, stage 2 (RLB high-pass). */
    static Biquad kWeightingHighPass (double fs)
    {
        const double fc = 38.13547087602444, Q = 0.5003270373238773;
        const double K = std::tan (kPi * fc / fs);
        const double a0 = 1.0 + K / Q + K * K;
        Biquad q;
        q.b0 = 1.0; q.b1 = -2.0; q.b2 = 1.0;
        q.a1 = 2.0 * (K * K - 1.0) / a0;
        q.a2 = (1.0 - K / Q + K * K) / a0;
        return q;
    }
};

/** In-place radix-2 complex FFT in double precision (size must be a power of two).
    Used for filter design only; bulk convolution uses juce::dsp::FFT.
    The inverse includes the 1/N scale. */
inline void fftInPlace (std::vector<std::complex<double>>& a, bool inverse)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap (a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = 2.0 * kPi / (double) len * (inverse ? 1.0 : -1.0);
        std::vector<std::complex<double>> tw (len / 2);
        for (size_t k = 0; k < len / 2; ++k)
            tw[k] = std::polar (1.0, ang * (double) k);
        for (size_t i = 0; i < n; i += len)
            for (size_t k = 0; k < len / 2; ++k)
            {
                const auto u = a[i + k], v = a[i + k + len / 2] * tw[k];
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
            }
    }
    if (inverse)
        for (auto& x : a) x /= (double) n;
}

/** Zeroth-order modified Bessel function, for Kaiser windows. */
inline double besselI0 (double x)
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 60; ++k)
    {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < sum * 1.0e-17) break;
    }
    return sum;
}

/** Kaiser window value for u in [-1, 1]. */
inline double kaiser (double u, double beta)
{
    if (std::abs (u) >= 1.0) return 0.0;
    return besselI0 (beta * std::sqrt (1.0 - u * u)) / besselI0 (beta);
}

inline double dbToGain (double db) { return std::pow (10.0, db / 20.0); }
inline double gainToDb (double g, double floorDb = -300.0) { return g > 0.0 ? std::max (floorDb, 20.0 * std::log10 (g)) : floorDb; }
} // namespace mihenk
