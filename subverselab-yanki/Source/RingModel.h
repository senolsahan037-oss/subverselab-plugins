#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

/*  The model behind the echo field: from the plug-in's parameters to the list of repeats the display draws.
    Ring k stands for repeat k: radius = k x TIME, level = first repeat + (k - 1) x slope (dB re the dry),
    brightness and stroke follow the level, warmth follows TONE and WEAR, the left/right offset follows SPREAD
    (alternating only where the voicing has heads on both sides).

    The numbers are measured, not guessed: tools/calib_rings.py renders a pink burst through the real processor for every
    voicing and FEEDBACK position (1/8 dotted at 120 bpm, MIX 0.30, two TONE values) and the first-repeat level and the
    per-repeat slope below are what it found (tools/ring_calib.json). The swell uses the engine's own measured build rates.
    tests/render.cpp checks the model against fresh renders of the 10 starting points. */
namespace ringmodel
{
struct Params
{
    int mood = 0;               // voicing (MOOD), 0 Echo (clean loop) .. 6
    int heads = 0;              // HEADS: 0 = the voicing's own, 1, 2 or 3 heads
    double timeS = 0.25;        // resolved TIME in seconds
    double mix = 0.30;          // the engine's MIX (after the dead-zone ramp)
    double fbKnob = 0.35;       // FEEDBACK as the engine knob 0..1 (0.7 = HOLD)
    double tone = 4500.0;       // Hz
    double wear = 0.25;
    double spread = 0.30;
    bool capOn = false;
    double capDb = 0.0;
    int lockBars = 0;           // 0 off, 1, 2, 4
    double riseMax = 0.7;       // RISE CURVE: 0.4 / 0.7 / 1.0
    double bpm = 120.0;
};

struct Ring
{
    int k = 1;
    double levelDb = -60.0;     // re the dry
    double seconds = 0.0;
};

struct Result
{
    std::vector<Ring> rings;
    double firstDb = -60.0;
    double slopeDb = -6.0;      // per repeat
    double decayBars = 0.0;     // time to -60 dB in bars at the host tempo (0 for a swell)
    double viewSeconds = 1.3;   // radius of the display
    bool swell = false;
    bool stereo = false;        // heads on both sides (alternating offset)
    double corr = 1.0;          // wet L/R correlation, from the head pan law
    double capDb = 0.0;
};

namespace detail
{
// first repeat (dB re dry) at MIX 0.30 and the slope per repeat at FEEDBACK knob 0, .15, .30, .45, .60, .69, per voicing.
inline constexpr double kFirstAt030[7] { -9.22, -11.23, -11.23, -10.59, -10.01, -13.29, -9.22 };
inline constexpr double kKnobPos[7] { 0.0, 0.15, 0.30, 0.45, 0.60, 0.69, 0.70 };
inline constexpr double kSlope[7][7] {
    { -40.0, -14.25, -7.93, -4.13, -1.41, -1.11, -1.0 },   // 0 Echo
    { -40.0, -12.30, -3.96, 0.14, 0.41, 0.41, 0.4 },       // 1 Slapback
    { -40.0, -5.95, -2.63, -0.50, 0.52, 0.67, 0.7 },       // 2 Three Heads
    { -40.0, -5.95, -2.63, -0.51, 0.50, 0.63, 0.65 },      // 3 Dub Spiral
    { -40.0, -5.96, -2.56, -0.51, 0.47, 0.60, 0.62 },      // 4 Worn Loop
    { -40.0, -13.85, -7.34, -3.34, -1.63, -1.16, -1.1 },   // 5 Yesilcam
    { -40.0, -14.25, -7.93, -4.13, -1.41, -1.11, -1.0 } }; // 6 Ters (reversed repeats: drawn like Echo)
// the same, with HEADS forced to 1, 2, 3 heads (tools/calib_rings_heads.py -> ring_calib_heads.json); index mood * 3 + heads - 1.
// Slope = the per-repeat slope that puts the -60 dB crossing where the render has it; the first repeat does not depend on the head count.
inline constexpr double kSlopeHeads[21][7] {
    { -40.00, -14.18, -7.89, -4.16, -1.41, -1.11, -1.11 },   // 0 Echo, 1 head
    { -40.00, -6.83, -3.91, -2.12, -0.81, -0.66, -0.66 },   // 0 Echo, 2 heads
    { -40.00, -4.54, -2.71, -1.45, -0.63, -0.53, -0.53 },   // 0 Echo, 3 heads
    { -40.00, -11.62, -4.09, 0.14, 0.41, 0.41, 0.41 },   // 1 Slapback, 1 head
    { -40.00, -7.31, -3.07, -0.29, 0.59, 0.59, 0.59 },   // 1 Slapback, 2 heads
    { -40.00, -5.51, -2.46, -0.50, 0.52, 0.67, 0.67 },   // 1 Slapback, 3 heads
    { -40.00, -11.62, -4.09, 0.14, 0.41, 0.41, 0.41 },   // 2 Three Heads, 1 head
    { -40.00, -7.31, -3.07, -0.29, 0.59, 0.59, 0.59 },   // 2 Three Heads, 2 heads
    { -40.00, -5.51, -2.46, -0.50, 0.52, 0.67, 0.67 },   // 2 Three Heads, 3 heads
    { -40.00, -11.59, -4.08, 0.12, 0.36, 0.36, 0.36 },   // 3 Dub Spiral, 1 head
    { -40.00, -7.27, -3.06, -0.29, 0.56, 0.56, 0.56 },   // 3 Dub Spiral, 2 heads
    { -40.00, -5.54, -2.46, -0.51, 0.50, 0.63, 0.63 },   // 3 Dub Spiral, 3 heads
    { -40.00, -11.57, -4.07, 0.11, 0.34, 0.31, 0.31 },   // 4 Worn Loop, 1 head
    { -40.00, -7.23, -3.06, -0.30, 0.53, 0.53, 0.53 },   // 4 Worn Loop, 2 heads
    { -40.00, -5.56, -2.42, -0.51, 0.47, 0.60, 0.60 },   // 4 Worn Loop, 3 heads
    { -40.00, -13.77, -7.23, -3.55, -1.74, -1.16, -1.16 },   // 5 Yesilcam, 1 head
    { -40.00, -6.42, -3.62, -2.14, -0.96, -0.69, -0.69 },   // 5 Yesilcam, 2 heads
    { -40.00, -4.27, -2.65, -1.49, -0.73, -0.55, -0.55 },   // 5 Yesilcam, 3 heads
    { -40.00, -14.18, -7.89, -4.16, -1.41, -1.11, -1.11 },   // 6 Ters, 1 head (drawn like Echo)
    { -40.00, -6.83, -3.91, -2.12, -0.81, -0.66, -0.66 },   // 6 Ters, 2 heads (drawn like Echo)
    { -40.00, -4.54, -2.71, -1.45, -0.63, -0.53, -0.53 }   // 6 Ters, 3 heads (drawn like Echo)
};
// the engine's measured build rate in dB per second at RISE 0, .1 ... 1 (YankiEngine::kRiseRate)
inline constexpr double kRiseRate[11] { 0.00, 1.00, 1.60, 2.86, 5.22, 6.00, 7.06, 7.06, 7.50, 7.50, 8.00 };

inline double interp (const double* xs, const double* ys, int n, double x)
{
    if (x <= xs[0]) return ys[0];
    for (int i = 1; i < n; ++i)
        if (x <= xs[i])
            return ys[i - 1] + (ys[i] - ys[i - 1]) * (x - xs[i - 1]) / (xs[i] - xs[i - 1]);
    return ys[n - 1];
}
} // namespace detail

/** Heads a voicing plays by itself (Three Heads, Dub Spiral, Worn Loop: three; the others one) and the HEADS choice on top of it. */
inline int effectiveHeads (int mood, int headsParam)
{
    return headsParam >= 1 && headsParam <= 3 ? headsParam : ((mood == 2 || mood == 3 || mood == 4) ? 3 : 1);
}

/** SPREAD as the engine's width (the plug-in hands the engine exactly this; see YankiProcessor::engineWidth). The stage loop (ECHO, YESILCAM,
    TERS) and any single head would sit hard in one ear / lose 3 dB in mono at 100 %, so the width above 0.70 is compressed to 0.80 at 100 %. */
inline double engineWidth (int mood, int heads, double spread)
{
    const bool stageLoop = mood == 0 || mood == 5 || mood == 6;
    if ((! stageLoop && heads != 1) || spread <= 0.70) return spread;
    return 0.70 + (spread - 0.70) / 3.0;
}

/** Wet L/R correlation from the heads' pan law (uncorrelated head signals): heads left, right, left; the stage loop pans with a
    constant-power law, the classic loop linearly. */
inline double correlation (int mood, int headsParam, double spread)
{
    const int n = effectiveHeads (mood, headsParam);
    if (n < 2)
        return 1.0;
    const bool stageLoop = mood == 0 || mood == 5 || mood == 6;
    const double w = std::clamp (engineWidth (mood, n, spread), 0.0, 1.0);
    double num = 0.0, gl2 = 0.0, gr2 = 0.0;
    for (int h = 0; h < n; ++h)
    {
        const double p = (h == 1) ? w : -w;
        const double gl = stageLoop ? std::sqrt (1.0 - p) : 0.5 * (1.0 - p), gr = stageLoop ? std::sqrt (1.0 + p) : 0.5 * (1.0 + p);
        const double hg = stageLoop ? (h == 0 ? 1.0 : h == 1 ? 0.75 : 0.55) : 1.0;
        num += hg * hg * gl * gr;
        gl2 += hg * hg * gl * gl;
        gr2 += hg * hg * gr * gr;
    }
    return num / std::sqrt (gl2 * gr2);
}

inline Result compute (const Params& p)
{
    using namespace detail;
    Result r;
    const int m = std::clamp (p.mood, 0, 6);
    const double mixEff = std::clamp (p.mix, 1.0e-4, 0.5);
    r.firstDb = kFirstAt030[m] + 20.0 * std::log10 (mixEff / 0.30) + (m == 0 || m == 5 ? (p.tone - 4500.0) / 2300.0 * 0.5 : 0.0);
    // The classic loop pans its heads linearly (power (1 + w^2) / 2 across both channels), the clean loop with a constant-power law:
    // SPREAD moves the first repeat's level only on the classic voicings (calibration was at SPREAD 0.30).
    const int heads = effectiveHeads (m, p.heads);
    const double w = engineWidth (m, heads, p.spread);
    if (m >= 1 && m <= 4)
        r.firstDb += 10.0 * std::log10 ((1.0 + w * w) / 1.09);
    r.stereo = heads >= 2;
    // Repeats closer than ~250 ms smear into each other and read lower on the burst (measured: -1.6 dB at 80 ms, -0.8 dB at 125 ms).
    {
        static const double ts[] { 0.02, 0.08, 0.125, 0.25 }, cs[] { -1.8, -1.6, -0.8, 0.0 };
        r.firstDb += detail::interp (ts, cs, 4, std::max (0.02, p.timeS));
    }
    r.corr = correlation (m, p.heads, p.spread);
    r.capDb = p.capDb;
    const double T = std::max (0.02, p.timeS);
    const double bar = 240.0 / std::max (20.0, p.bpm);

    const bool pastHold = p.fbKnob >= 0.7 - 1.0e-9;
    const bool locked = p.lockBars > 0;
    r.swell = pastHold || locked;
    // Heads forced by the HEADS keys to something other than the voicing's own: the loop feeds from the longest head, so the decay differs.
    const bool forcedHeads = p.heads >= 1 && p.heads <= 3 && p.heads != effectiveHeads (m, 0);
    double slope = interp (kKnobPos, forcedHeads ? kSlopeHeads[m * 3 + p.heads - 1] : kSlope[m], 7, std::min (p.fbKnob, 0.7));
    double plateau = p.capOn ? p.capDb : 6.0;
    if (pastHold)
    {
        const double rise = std::clamp ((p.fbKnob - 0.7) / 0.3 * p.riseMax, 0.0, 1.0);
        const double x = rise * 10.0;
        const int i = std::min (9, (int) x);
        const double rate = kRiseRate[i] + (kRiseRate[i + 1] - kRiseRate[i]) * (x - i);
        slope = std::max (0.25, rate * T);     // dB per repeat; RISE 0 = the hold point, a slow crawl
    }
    else if (locked)
    {
        const double lockS = p.lockBars * bar;
        slope = std::max (0.25, (plateau - r.firstDb) / std::max (lockS, T) * T);
    }
    r.slopeDb = slope;

    // how far the display reaches: the decay length, or the swell until it holds, on a ladder of round views
    double reach;
    if (r.swell)
        reach = T * (1.0 + std::max (0.0, plateau - r.firstDb) / slope) + 2.0 * T;
    else if (slope < -0.05)
        reach = T * (1.0 + (-60.0 - r.firstDb) / slope);
    else
        reach = 60.0;
    if (! r.swell)
        r.decayBars = reach / bar;
    static const double ladder[] { 1.3, 2.2, 4.4, 8.8, 13.0 };
    r.viewSeconds = ladder[4];
    for (double v : ladder)
        if (reach * 1.04 <= v) { r.viewSeconds = v; break; }
    r.viewSeconds = std::max (r.viewSeconds, T * 3.0);

    double level = r.firstDb;
    for (int k = 1; k <= 120; ++k)
    {
        const double t = k * T;
        if (t > r.viewSeconds * 1.001)
            break;
        double L = level;
        if (r.swell)
            L = std::min (level, plateau);
        if (! r.swell && L < -60.0)
            break;
        r.rings.push_back ({ k, L, t });
        level += slope;
    }
    return r;
}
} // namespace ringmodel
