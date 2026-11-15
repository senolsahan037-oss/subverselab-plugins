#pragma once

#include <algorithm>
#include <cmath>

/*  Small DSP pieces for BOE-B. Plain C++, no JUCE. Coefficients in double, state in double (low corners at
    192 kHz stay exact); the audio path stays float at the edges.
*/
namespace svl::boeb {

inline constexpr double kPi = 3.14159265358979323846;

inline float dbToGain (float db) { return std::pow (10.0f, db * 0.05f); }
inline float powToDb (double p)  { return p > 1.0e-30 ? (float) (10.0 * std::log10 (p)) : -300.0f; }

/** Transposed direct form II biquad, RBJ cookbook designs. */
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;

    void reset() { z1 = z2 = 0; }

    double process (double x)
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    void set (double nb0, double nb1, double nb2, double a0, double na1, double na2)
    {
        b0 = nb0 / a0; b1 = nb1 / a0; b2 = nb2 / a0; a1 = na1 / a0; a2 = na2 / a0;
    }

    void identity() { b0 = 1; b1 = b2 = a1 = a2 = 0; }

    void lowPass (double fs, double f, double q)
    {
        const double w = 2 * kPi * std::min (f, 0.49 * fs) / fs, c = std::cos (w), al = std::sin (w) / (2 * q);
        set ((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
    }

    void highPass (double fs, double f, double q)
    {
        const double w = 2 * kPi * std::min (f, 0.49 * fs) / fs, c = std::cos (w), al = std::sin (w) / (2 * q);
        set ((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
    }

    void peak (double fs, double f, double q, double gainDb)
    {
        const double A = std::pow (10.0, gainDb / 40), w = 2 * kPi * std::min (f, 0.49 * fs) / fs;
        const double c = std::cos (w), al = std::sin (w) / (2 * q);
        set (1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
    }

    void lowShelf (double fs, double f, double q, double gainDb)
    {
        const double A = std::pow (10.0, gainDb / 40), w = 2 * kPi * std::min (f, 0.49 * fs) / fs;
        const double c = std::cos (w), al = std::sin (w) / (2 * q), s = 2 * std::sqrt (A) * al;
        set (A * ((A + 1) - (A - 1) * c + s), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - s),
             (A + 1) + (A - 1) * c + s, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - s);
    }

    void highShelf (double fs, double f, double q, double gainDb)
    {
        const double A = std::pow (10.0, gainDb / 40), w = 2 * kPi * std::min (f, 0.49 * fs) / fs;
        const double c = std::cos (w), al = std::sin (w) / (2 * q), s = 2 * std::sqrt (A) * al;
        set (A * ((A + 1) + (A - 1) * c + s), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - s),
             (A + 1) - (A - 1) * c + s, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - s);
    }
};

/** First-order section from the bilinear transform with prewarping (A-weighting is built from these). */
struct OnePoleSection {
    double b0 = 1, b1 = 0, a1 = 0, z = 0;
    void reset() { z = 0; }
    double process (double x) { const double y = b0 * x + z; z = b1 * x - a1 * y; return y; }
    void highPass (double fs, double f)
    {
        const double K = 2 * fs, wa = K * std::tan (kPi * std::min (f, 0.49 * fs) / fs);
        b0 = K / (K + wa); b1 = -b0; a1 = (wa - K) / (K + wa);
    }
    void lowPass (double fs, double f)
    {
        const double K = 2 * fs, wa = K * std::tan (kPi * std::min (f, 0.49 * fs) / fs);
        b0 = wa / (K + wa); b1 = b0; a1 = (wa - K) / (K + wa);
    }
};

/** IEC 61672 A-weighting: s^4 / ((s+w1)^2 (s+w2) (s+w3) (s+w4)^2), normalised to 0 dB at 1 kHz. */
struct AWeighting {
    OnePoleSection s[6];
    double gain = 1;

    void prepare (double fs)
    {
        s[0].highPass (fs, 20.598997); s[1].highPass (fs, 20.598997);
        s[2].highPass (fs, 107.65265); s[3].highPass (fs, 737.86223);
        s[4].lowPass (fs, 12194.217);  s[5].lowPass (fs, 12194.217);
        // Normalise at 1 kHz from the sections' own response.
        const double w = 2 * kPi * 1000.0 / fs;
        double mag = 1;
        for (auto& sec : s)
        {
            const double re = std::cos (w), im = -std::sin (w);              // z^-1
            const double nr = sec.b0 + sec.b1 * re, ni = sec.b1 * im;
            const double dr = 1 + sec.a1 * re,     di = sec.a1 * im;
            mag *= std::sqrt ((nr * nr + ni * ni) / (dr * dr + di * di));
        }
        gain = 1.0 / mag;
        reset();
    }
    void reset() { for (auto& sec : s) sec.reset(); }
    double process (double x) { for (auto& sec : s) x = sec.process (x); return x * gain; }
};

/** ITU-R BS.1770 K-weighting (the two-stage pre-filter, designed for any sample rate). */
struct KWeighting {
    Biquad shelf, hp;
    void prepare (double fs)
    {
        shelf.highShelf (fs, 1681.9744509555319, 0.7071752369554193, 3.99984385397);
        hp.highPass (fs, 38.13547087613982, 0.5003270373253953);
        reset();
    }
    void reset() { shelf.reset(); hp.reset(); }
    double process (double x) { return hp.process (shelf.process (x)); }
};

/** Band filter: a 4th-order Butterworth high-pass at lo and a 4th-order Butterworth low-pass at hi (sections Q 0.5412
    and 1.3066), -3 dB at both edges: the same bands as the measurement tools (scipy butter(4)). */
struct BandFilter {
    Biquad h1, h2, l1, l2;
    void prepare (double fs, double lo, double hi)
    {
        h1.highPass (fs, lo, 0.54119610); h2.highPass (fs, lo, 1.30656296);
        if (hi < 0.45 * fs) { l1.lowPass (fs, hi, 0.54119610); l2.lowPass (fs, hi, 1.30656296); }
        else                { l1.identity(); l2.identity(); }
        reset();
    }
    void reset() { h1.reset(); h2.reset(); l1.reset(); l2.reset(); }
    double process (double x) { return l2.process (l1.process (h2.process (h1.process (x)))); }
};

/** In-place iterative radix-2 complex FFT (n a power of two). Used by LISTEN only, never in the audio path. */
inline void fftInPlace (double* re, double* im, int n)
{
    for (int i = 1, j = 0; i < n; ++i)
    {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap (re[i], re[j]); std::swap (im[i], im[j]); }
    }
    for (int len = 2; len <= n; len <<= 1)
    {
        const double ang = -2 * kPi / len, wr = std::cos (ang), wi = std::sin (ang);
        for (int i = 0; i < n; i += len)
        {
            double cr = 1, ci = 0;
            for (int k = 0; k < len / 2; ++k)
            {
                const int a = i + k, b = a + len / 2;
                const double tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr; im[b] = im[a] - ti; re[a] += tr; im[a] += ti;
                const double nr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = nr;
            }
        }
    }
}

/** One-pole smoother coefficient for a time constant. */
inline double coefFor (double seconds, double fs) { return seconds > 0 ? std::exp (-1.0 / (seconds * fs)) : 0.0; }

} // namespace svl::boeb
