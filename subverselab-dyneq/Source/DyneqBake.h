// BAKE fit (SPEC 1 C, item 14): turns the mean cut per COLLIDE band into up to N static bells for the EMPTY free bands.
// Plain C++, no JUCE: the plug-in, dyneqraw --bake and the tests all run this one file.
//
// Greedy: take the deepest peak of the residual, put a bell at that band's centre with gain = -residual, choose the Q (0.3 .. 18) that
// leaves the smallest squared residual over the 32 bands, subtract the bell's response (cascaded bells add in dB, so the subtraction
// is exact), repeat until the residual is under kBakeStopDb or no band is left.
#pragma once

#include "DyneqEngine.h"
#include <cstdio>

namespace dyneq
{
constexpr float kBakeStopDb = 0.5f;              // the residual is accepted below this
constexpr float kBakeMinCutDb = 0.5f;            // never the same threshold as a rounding: a bell is worth placing from here
constexpr float kBakeQMin = 0.3f, kBakeQMax = 18.0f;
constexpr int   kBakeQSteps = 48;
constexpr double kBakeGainLo = 0.8, kBakeGainHi = 1.1;   // the bell's gain stays within this fraction of the residual's peak (the fit may not drift away from the measured cut)
constexpr int   kBakeMaxBells = kFreeBands;

struct BakeBell
{
    int   band = 0;          // COLLIDE band index the bell sits on
    float freqHz = 1000.0f;
    float gainDb = 0.0f;     // negative = cut
    float q = 1.0f;
    float meanCutDb = 0.0f;  // the statistic at that band (positive)
    float percentCut = 0.0f; // % of the counted time that band was cut
};

// Magnitude response (dB) of an RBJ peaking bell, double precision, same formula as the free band's bell.
inline double bellResponseDb (double f0, double gainDb, double q, double f, double sr)
{
    const double A = std::pow (10.0, gainDb / 40.0), w0 = 2.0 * M_PI * f0 / sr, al = std::sin (w0) / (2.0 * q), cw = std::cos (w0);
    const double b0 = 1.0 + al * A, b1 = -2.0 * cw, b2 = 1.0 - al * A, a0 = 1.0 + al / A, a1 = -2.0 * cw, a2 = 1.0 - al / A;
    const double w = 2.0 * M_PI * f / sr, c1 = std::cos (w), s1 = std::sin (w), c2 = std::cos (2.0 * w), s2 = std::sin (2.0 * w);
    const double nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2), dr = a0 + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2);
    return 10.0 * std::log10 (std::max ((nr * nr + ni * ni) / (dr * dr + di * di), 1e-30));
}

// meanCut / percentCut: per band (kBands values), positive = cut. fc: the band centres. Returns the number of bells written (<= maxBells).
// residualOut (optional, kBands values): what the bells leave of the mean cut, in dB (positive = still to be cut).
// The bell's centre is the parabolic peak of the residual over the three bands around its maximum (in the ERB domain, so a plateau over two
// bands gets a bell between them); its gain and Q are the pair on the grid (Q 0.3 .. 18, gain 0.8 .. 1.1 x the peak, never over 18 dB)
// that leaves the smallest squared residual at the 32 band centres.
inline int bakeFit (const float* meanCut, const float* percentCut, const float* fc, double sr, int maxBells, BakeBell* out, double* residualOut = nullptr)
{
    double resid[kBands];
    for (int b = 0; b < kBands; ++b) resid[b] = meanCut[b];
    const double e0 = erbRate (fc[0]), eStep = (erbRate (fc[kBands - 1]) - e0) / (double) (kBands - 1);
    int n = 0;
    maxBells = std::min (maxBells, kBakeMaxBells);
    while (n < maxBells)
    {
        int pk = 0;
        for (int b = 1; b < kBands; ++b) if (resid[b] > resid[pk]) pk = b;
        if (resid[pk] < kBakeStopDb || resid[pk] < kBakeMinCutDb) break;
        const double ym = pk > 0 ? resid[pk - 1] : resid[pk], y0 = resid[pk], yp = pk < kBands - 1 ? resid[pk + 1] : resid[pk];
        const double den = ym - 2.0 * y0 + yp;
        double delta = den < -1e-9 ? 0.5 * (ym - yp) / den : 0.0;
        delta = std::max (-0.5, std::min (0.5, delta));
        const double ypk = std::min (y0 - 0.25 * (ym - yp) * delta, 18.0);
        const double f0 = (double) erbToHz ((float) (e0 + eStep * ((double) pk + delta)));
        double bestErr = 1e300, bestQ = 1.0, bestG = ypk;
        for (int qi = 0; qi < kBakeQSteps; ++qi)
        {
            const double q = kBakeQMin * std::pow ((double) kBakeQMax / kBakeQMin, (double) qi / (kBakeQSteps - 1));
            double num = 0.0, dd = 0.0;
            for (int b = 0; b < kBands; ++b)
            {
                const double sh = -bellResponseDb (f0, -ypk, q, fc[b], sr) / ypk;   // shape at the peak gain, positive = cut
                num += resid[b] * sh; dd += sh * sh;
            }
            const double g = std::max (kBakeGainLo * ypk, std::min (std::min (kBakeGainHi * ypk, 18.0), ypk * num / std::max (dd, 1e-12)));
            double err = 0.0;
            for (int b = 0; b < kBands; ++b) { const double d = resid[b] + bellResponseDb (f0, -g, q, fc[b], sr); err += d * d; }
            if (err < bestErr) { bestErr = err; bestQ = q; bestG = g; }
        }
        for (int b = 0; b < kBands; ++b) resid[b] += bellResponseDb (f0, -bestG, bestQ, fc[b], sr);
        BakeBell& o = out[n++];
        int nearest = 0;
        for (int b = 1; b < kBands; ++b) if (std::abs (erbRate ((float) f0) - erbRate (fc[b])) < std::abs (erbRate ((float) f0) - erbRate (fc[nearest]))) nearest = b;
        o.band = nearest; o.freqHz = (float) f0; o.gainDb = (float) -bestG; o.q = (float) bestQ;
        o.meanCutDb = meanCut[pk]; o.percentCut = percentCut != nullptr ? percentCut[pk] : 0.0f;
    }
    if (residualOut != nullptr) for (int b = 0; b < kBands; ++b) residualOut[b] = resid[b];
    return n;
}

// The static curve (dB, negative = cut) of a set of baked bells at one frequency.
inline double bakedCurveDb (const BakeBell* bells, int n, double f, double sr)
{
    double s = 0.0;
    for (int i = 0; i < n; ++i) s += bellResponseDb (bells[i].freqHz, bells[i].gainDb, bells[i].q, f, sr);
    return s;
}

// "-2.1 dB @ 2.4 kHz, Q 1.8, cut 34 % of the time" (the preview line)
inline void bakeBellText (const BakeBell& b, char* buf, size_t size)
{
    if (b.freqHz >= 1000.0f) std::snprintf (buf, size, "%.1f dB @ %.2f kHz, Q %.1f, cut %.0f %% of the time", (double) b.gainDb, (double) b.freqHz * 0.001, (double) b.q, (double) b.percentCut);
    else                     std::snprintf (buf, size, "%.1f dB @ %.0f Hz, Q %.1f, cut %.0f %% of the time", (double) b.gainDb, (double) b.freqHz, (double) b.q, (double) b.percentCut);
}

// Writes the bells into the EMPTY (on == false) bands of p, lowest slot first, as static bells (dyn off). Returns how many were written.
inline int bakeApplyToParams (Params& p, const BakeBell* bells, int n)
{
    int slot = 0, written = 0;
    for (int i = 0; i < n; ++i)
    {
        while (slot < kFreeBands && p.band[slot].on) ++slot;
        if (slot >= kFreeBands) break;
        FreeBand& b = p.band[slot++];
        b.on = true; b.type = kBell; b.freqHz = bells[i].freqHz; b.gainDb = bells[i].gainDb; b.q = bells[i].q; b.dyn = false;
        ++written;
    }
    return written;
}
} // namespace dyneq
