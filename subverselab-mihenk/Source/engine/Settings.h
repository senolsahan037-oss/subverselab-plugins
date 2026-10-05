#pragma once

#include <JuceHeader.h>
#include <vector>

/*  Every setting of the MIHENK chain, as plain values. The engine reads a
    copy of this struct; nothing in the DSP keeps hidden state between renders.

        DC filter -> balance -> input trim (+ headroom assistant) -> EQ (static FIR, AIR)
        -> dynamic EQ (low-mid cut, upper-mid cut, low tightener, AIR de-harsh)
        -> compressor (WIDE glue or 3-BAND) -> stereo (width, low mono, high width)
        -> drive -> true-peak limiter -> output

    Every stage added in v2 is neutral by default, so v1 settings render bit for bit as before.
*/
namespace mihenk
{
/** A dynamic EQ band: a bell (or a low shelf) whose cut follows the level of its own band.
        y = x + (g(t) - 1) * C(x)
    C is the band's component filter (a constant-peak band-pass for a bell, a 2nd-order Butterworth low-pass for a
    low shelf), so at a fixed g this is exactly a bell / shelf of gain g. g(t) comes from the glue compressor's gain
    computer on the band's own level; in AUTO the threshold is solved so the mean cut over the loud part equals
    amountDb (the same solver as the glue), and the cut never exceeds rangeDb. */
struct DynBandSettings
{
    enum Shape { bell = 0, lowShelf = 1 };
    bool enabled = false;
    int shape = bell;
    double freqHz = 300.0;
    double q = 1.0;                    // bell only
    double amountDb = 1.5;             // mean cut over the loud part (AUTO)
    double rangeDb = 6.0;              // largest cut
    double ratio = 3.0;
    double attackMs = 15.0;
    double releaseMs = 150.0;
};

struct EqSettings
{
    bool enabled = true;
    bool linearPhase = false;          // false: minimum phase

    bool lowCutOn = false;
    double lowCutHz = 25.0;
    int lowCutSlope = 12;              // 12 or 24 dB/oct (Butterworth magnitude)

    double lowShelfDb = 0.0, lowShelfHz = 90.0;
    double tiltDbPerOct = 0.0;         // pivot 1 kHz, flat outside 20 Hz..20 kHz
    double highShelfDb = 0.0, highShelfHz = 10000.0;
    double bell1Db = 0.0, bell1Hz = 250.0, bell1Q = 1.0;
    double bell2Db = 0.0, bell2Hz = 3000.0, bell2Q = 1.0;

    // Tone match: a smooth correction on the spectrum grid (dB), scaled by matchAmount (0..1).
    std::vector<float> matchCurveDb;
    double matchAmount = 1.0;

    // AIR: a very broad, gentle high shelf, A(f) = 1 + (G - 1) * p(f), p = w / sqrt(1 + w^2), w = f / airHz,
    // G = +airDb (0..+6 dB). airHz is one of 10, 15, 20 or 40 kHz (40 kHz only lifts the top octave gently).
    double airDb = 0.0;
    double airHz = 20000.0;
    // Dynamic AIR (de-harsh): the AIR gain eases off, by up to airDeHarshDb below its static value, while the
    // 5-9 kHz band stands out of its own typical level (so with AIR at 0 dB it works as a broad de-esser).
    bool airDynamic = false;
    double airDeHarshDb = 4.0;

    // Dynamic EQ, set by the scenarios: low-mid mud, upper-mid harshness, and the sub / low tightener.
    DynBandSettings dynLowMid  { false, DynBandSettings::bell, 300.0, 1.0, 1.5, 6.0, 3.0, 15.0, 150.0 };
    DynBandSettings dynUpperMid { false, DynBandSettings::bell, 3500.0, 1.2, 1.5, 6.0, 3.0, 5.0, 80.0 };
    DynBandSettings tightener  { false, DynBandSettings::lowShelf, 90.0, 0.707, 1.5, 6.0, 2.5, 10.0, 120.0 };
};

/** One band of the 3-band compressor. */
struct BandSettings
{
    bool bypass = false;               // band passes uncompressed (gain still applies)
    bool solo = false;                 // listen: only soloed bands reach the output
    bool autoThreshold = true;
    double glueDb = 1.0;               // AUTO: mean gain reduction over the loud part (0..4 dB)
    double thresholdDb = -24.0;
    double ratio = 2.0;
    double attackMs = 20.0;
    double releaseMs = 150.0;
    double gainDb = 0.0;               // after the level-neutral make-up, -6..+6 dB
};

enum class CompMode { wide = 0, threeBand = 1 };

struct CompSettings
{
    bool enabled = false;
    bool autoThreshold = true;         // AUTO: threshold solved so the loud part gets glueDb of gain reduction
    double glueDb = 1.2;               // glue target, dB of mean gain reduction over the loud part (0..4)
    double thresholdDb = -18.0;
    double ratio = 2.0;                // 1.5 .. 4
    double attackMs = 30.0;
    double releaseMs = 250.0;
    bool autoRelease = true;           // program-dependent: fast release on transients, slow after sustained reduction
    double rmsBlend = 0.7;             // 0 = peak detector, 1 = RMS detector (sine-calibrated)
    double scHpfHz = 90.0;             // sidechain high-pass; <= 20 is off
    double mix = 1.0;                  // parallel mix
    double kneeDb = 6.0;
    double maxGrDb = 0.0;              // static gain reduction limit (dynamic EQ range); 0 = none. Not on the panel.

    // MODE: WIDE is the broadband glue above. 3-BAND splits at two Linkwitz-Riley 4th-order crossovers
    // (zero phase, perfectly complementary) and runs one compressor per band; knee, RMS / PEAK and AUTO REL are shared.
    CompMode mode = CompMode::wide;
    double xoverLowHz = 120.0;         // 40..400 Hz
    double xoverHighHz = 4000.0;       // 2..10 kHz
    BandSettings bands[3] {
        { false, false, true, 1.0, -24.0, 2.0, 30.0, 200.0, 0.0 },
        { false, false, true, 1.0, -24.0, 2.0, 20.0, 150.0, 0.0 },
        { false, false, true, 0.8, -24.0, 2.0, 5.0, 80.0, 0.0 },
    };
};

struct WidthSettings
{
    bool enabled = true;
    double width = 1.0;                // side gain, 0 .. 2
    double monoBelowHz = 0.0;          // 0 = off, else side removed below this frequency (zero phase)
    // Phase-safe low mono: below monoBelowHz the low band is summed with the polarity that adds (measured low-band
    // correlation), and its level is restored to the stereo low-band energy, so anti-phase lows do not cancel.
    bool phaseSafeLowMono = false;
    double highWidth = 1.0;            // side gain above highWidthHz (zero-phase split); 1 = off
    double highWidthHz = 4000.0;
};

struct LimiterSettings
{
    bool enabled = true;
    double ceilingDbtp = -1.0;
    double releaseMs = 150.0;
    double lookaheadMs = 1.5;
    double driveDb = 0.0;              // gain into the limiter; MATCH TARGET solves it
    // The most drive MATCH TARGET may use (dB). Not a setting of the panel and not saved in presets: the plug-in lowers it
    // when LEARN heard too little of the song (a quiet part says nothing about the loud ones). 36 = no cap.
    double maxDriveDb = 36.0;
};

/** Loudness targets. KEEP INPUT holds the source mix's own integrated loudness: the limiter then only guards the
    true-peak ceiling and the master is never made louder (or quieter) than the mix that came in. It is appended
    after CUSTOM so saved presets and host states keep their indices. */
enum class TargetPreset { streaming = 0, apple, club, custom, keepInput };

struct MasterSettings
{
    bool bypass = false;               // whole chain out: output is the input, bit for bit
    bool dcFilter = false;             // zero-phase 2nd-order high-pass at 5 Hz (removes DC offset)
    double balanceDb = 0.0;            // L/R balance correction: + turns the left down and the right up by half each
    bool autoTrim = false;             // input headroom assistant: pre-trim to the working level (see kWorkingPeakDbfs)
    double trimDb = 0.0;
    EqSettings eq;
    CompSettings comp;
    WidthSettings width;
    LimiterSettings limiter;

    TargetPreset targetPreset = TargetPreset::streaming;
    double targetLufs = -14.0;
    double limitWarnDb = 6.0;          // warn when reaching the target takes more peak limiting than this
    double toneSmoothOct = 1.0;        // smoothing of the tone-match curve, octaves
    bool targetLock = true;            // re-solve the drive after every change so the output stays on the target
    bool planned = false;              // v3: these settings are the output of solvePlan (level fixed by the plan, nothing re-solves it)

    // Already-finished input: when the source is at or above the loudness target, EQ, compressor and stereo are switched off
    // for this track (the switches show it), unless the user has set one of those controls by hand. The three values are what
    // they were before, so they come back when the target no longer sits at or below the input.
    bool toneByHand = false;
    struct AutoSkip { bool active = false, eq = false, comp = false, width = false; } autoSkip;

    /** The level is held for the user: by TARGET LOCK (v2) or because the settings are a plan (v3). */
    bool levelHeld() const noexcept { return targetLock || planned; }
};

/** The input headroom assistant's working level: sample peak -6 dBFS and short-term max -14 LUFS.
    The pre-trim is the larger cut / smaller lift that satisfies both, limited to +-24 dB. */
constexpr double kWorkingPeakDbfs = -6.0, kWorkingShortTermLufs = -14.0, kMaxAutoTrimDb = 24.0;

inline double presetLufs (TargetPreset p, double custom)
{
    switch (p)
    {
        case TargetPreset::streaming: return -14.0;
        case TargetPreset::apple:     return -16.0;
        case TargetPreset::club:      return -8.0;
        case TargetPreset::custom:    return custom;
        case TargetPreset::keepInput: return custom; // resolved from the source by MasterChain
        default:                      return custom;     // defensive fallback for invalid enum values
    }
}

inline const char* presetName (TargetPreset p)
{
    switch (p)
    {
        case TargetPreset::streaming: return "STREAMING -14";
        case TargetPreset::apple:     return "APPLE -16";
        case TargetPreset::club:      return "CLUB -8";
        case TargetPreset::custom:    return "CUSTOM";
        case TargetPreset::keepInput: return "KEEP INPUT";
        default:                      return "CUSTOM"; // defensive fallback for invalid enum values
    }
}

/** Genre starting points. They set tone and dynamics only, never the delivery target, the ceiling or TARGET LOCK.
    The glue compressor is set by a gain-reduction target (AUTO), never by an absolute threshold, so it does
    about the same amount of work on a quiet mix and on a loud one; dense input reduces it further.
    They are starting settings to adjust by ear and by meter, not an analysis of the track. */
enum class Genre { rapTrap = 0, arabeskPop, electronicLofi };

inline const char* genreName (Genre g)
{
    switch (g)
    {
        case Genre::rapTrap:        return "HIP-HOP / RAP";
        case Genre::arabeskPop:     return "R&B / POP";
        case Genre::electronicLofi: return "ELECTRONIC / LO-FI";
        default:                    return "ELECTRONIC / LO-FI"; // defensive fallback for invalid enum values
    }
}

inline void applyGenreStartingPoint (MasterSettings& s, Genre g)
{
    auto& e = s.eq;
    auto& c = s.comp;
    auto& w = s.width;
    auto& l = s.limiter;
    e.enabled = true; c.enabled = true; w.enabled = true; l.enabled = true;
    c.autoThreshold = true;
    c.kneeDb = 6.0;
    e.tiltDbPerOct = 0.0;
    switch (g)
    {
        case Genre::rapTrap:
            // Solid sub with mono below 120 Hz, punchy glue and clear vocal upper mids.
            e.lowCutOn = true;  e.lowCutHz = 22.0; e.lowCutSlope = 24;
            e.lowShelfDb = 1.0; e.lowShelfHz = 70.0;
            e.bell1Db = -1.0;   e.bell1Hz = 300.0;  e.bell1Q = 1.0;
            e.bell2Db = 1.0;    e.bell2Hz = 3000.0; e.bell2Q = 1.0;
            e.highShelfDb = 1.0; e.highShelfHz = 10000.0;
            c.glueDb = 1.5; c.ratio = 2.0; c.attackMs = 30.0; c.releaseMs = 250.0; c.autoRelease = true;
            c.rmsBlend = 0.7; c.scHpfHz = 110.0; c.mix = 1.0;
            w.width = 1.0; w.monoBelowHz = 120.0;
            l.releaseMs = 100.0; l.lookaheadMs = 1.5;
            break;
        case Genre::arabeskPop:
            // Smooth top, controlled low end, gentle glue and vocal-forward 2–5 kHz presence.
            e.lowCutOn = true;  e.lowCutHz = 30.0; e.lowCutSlope = 12;
            e.lowShelfDb = 0.0; e.lowShelfHz = 90.0;
            e.bell1Db = -1.0;   e.bell1Hz = 250.0;  e.bell1Q = 0.9;
            e.bell2Db = 1.0;    e.bell2Hz = 2500.0; e.bell2Q = 0.8;
            e.highShelfDb = 0.8; e.highShelfHz = 12000.0;
            c.glueDb = 1.0; c.ratio = 1.8; c.attackMs = 25.0; c.releaseMs = 320.0; c.autoRelease = true;
            c.rmsBlend = 0.8; c.scHpfHz = 120.0; c.mix = 1.0;
            w.width = 1.05; w.monoBelowHz = 120.0;
            l.releaseMs = 200.0; l.lookaheadMs = 2.0;
            break;
        case Genre::electronicLofi:
            // Solid sub, slightly warm tilt, parallel glue that keeps the transients.
            e.lowCutOn = true;  e.lowCutHz = 20.0; e.lowCutSlope = 24;
            e.lowShelfDb = 1.5; e.lowShelfHz = 60.0;
            e.tiltDbPerOct = -0.2;
            e.bell1Db = -0.8;   e.bell1Hz = 400.0;  e.bell1Q = 1.0;
            e.bell2Db = 0.0;    e.bell2Hz = 3000.0; e.bell2Q = 1.0;
            e.highShelfDb = 0.0; e.highShelfHz = 12000.0;
            c.glueDb = 1.2; c.ratio = 2.0; c.attackMs = 10.0; c.releaseMs = 150.0; c.autoRelease = false;
            c.rmsBlend = 0.5; c.scHpfHz = 60.0; c.mix = 0.8;
            w.width = 1.15; w.monoBelowHz = 150.0;
            l.releaseMs = 80.0; l.lookaheadMs = 1.0;
            break;
    }
}
} // namespace mihenk
