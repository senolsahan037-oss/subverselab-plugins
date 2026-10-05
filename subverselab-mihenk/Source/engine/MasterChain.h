#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <memory>
#include "Settings.h"
#include "Proxy.h"
#include "../dsp/Loudness.h"
#include "../dsp/Dynamics.h"
#include "../dsp/Equalizer.h"

/*  The offline mastering chain. Everything runs on whole buffers:

        DC filter -> balance -> input trim (+ headroom assistant) -> EQ (FIR, linear or minimum phase, AIR)
        -> dynamic EQ (low-mid, upper-mid, low tightener, AIR de-harsh)
        -> compressor (WIDE glue or 3-BAND) -> stereo (width, low mono, high width)
        -> drive -> true-peak limiter -> output

    A stage that would not change the signal is skipped, so a neutral chain
    (or BYPASS) returns the input bit for bit.

    The part before the drive does not depend on the drive, so it is cached:
    moving only the limiter or the target re-renders just the limiter.
*/
namespace mihenk
{
/** One band of the 3-band compressor: what it solved and did. */
struct BandAnalysis
{
    bool active = false;                // compressing (not bypassed)
    bool solo = false;
    double thresholdDb = 0.0;           // used (solved in AUTO)
    double ratio = 0.0;
    double targetGrDb = 0.0;
    double loudMeanGrDb = 0.0;          // over the loud part
    double loudMaxGrDb = 0.0;
    double makeupDb = 0.0;              // level-neutral make-up of this band (band energy before / after)
    GainReductionStats gr;              // per-10 ms history for the meters
};

/** What the glue compressor measured and decided: density of the source mix, then the threshold, ratio and gain
    reduction on the signal entering the compressor (after trim and EQ). */
struct GlueAnalysis
{
    bool valid = false;                 // compressor in and the signal is measurable
    bool autoThreshold = false;
    bool dense = false;                 // input already dense: glue target and ratio were reduced
    double plr = 0.0;                   // sample peak minus integrated loudness, LU
    double crest = 0.0;                 // sample peak minus RMS, dB
    double lra = 0.0;                   // loudness range, LU
    double targetGrDb = 0.0;            // glue target actually used (after the dense reduction)
    double ratio = 0.0;                 // ratio actually used
    double thresholdDb = 0.0;           // threshold actually used (solved in AUTO)
    double loudMeanGrDb = 0.0;          // compressor gain reduction over the loud part (short-term >= 70th percentile)
    double loudMaxGrDb = 0.0;
    double makeupDb = 0.0;              // level-neutral make-up applied after the compressor
    int solverIterations = 0;
    juce::String denseReason;
    // 3-BAND mode: the fields above then describe the whole stage (loud-part GR energy-weighted over the bands,
    // max of the bands, make-up = the final level-neutral correction); thresholdDb / ratio are unused.
    bool threeBand = false;
    double xoverLowHz = 0.0, xoverHighHz = 0.0;
    BandAnalysis bands[3];
};

/** The source mix as it arrives (before anything in the chain). Cached per input buffer. */
struct SourceStats
{
    bool valid = false;
    double samplePeakDb = kNegInf, integrated = kNegInf, shortTermMax = kNegInf, lra = 0.0, plr = 0.0, crest = 0.0;
};

/** Input stage: the headroom assistant's decision. */
struct InputStage
{
    bool autoTrim = false;
    double autoTrimDb = 0.0;            // assistant's pre-trim (0 when off)
    double effectiveTrimDb = 0.0;       // user trim + assistant
    juce::String message;               // "Input arrives hot ... pre-trim -6.0 dB ..."
};

/** The pre-trim that brings a source to the working level (kWorkingPeakDbfs / kWorkingShortTermLufs), with the reason. */
InputStage headroomAssistant (const SourceStats& src);

/** Dynamic EQ: what each band solved and did. */
struct DynamicEqAnalysis
{
    DynBandResult lowMid, upperMid, tightener;
    AirDynamicResult air;
};

/** What the stereo stage decided from the whole signal (phase-safe low mono: polarity and energy restore). */
struct WidthAnalysis
{
    double safeSign = 1.0, safeGain = 1.0;
};

/** Audio after each stage, for the layered real-time views (filled only when PreLimiterCache::keepTaps is on).
    The pre-limiter signal is PreLimiterCache::buffer; the output is RenderResult::output. */
struct StageTaps
{
    std::shared_ptr<const juce::AudioBuffer<float>> afterEq, afterDynamicEq, afterCompressor;
};

/*  Dense-input rule (documented in STATUS.md): the source mix counts as already dense when
    its peak-to-loudness ratio is under 11 LU (finished, limited masters sit at 8-10 LU; unmastered mixes at 12+),
    or its crest factor is under 9 dB, or its loudness range is under 4 LU. Then the glue target is capped at
    0.5 dB and the ratio at 1.5:1. In 3-BAND mode only the peak criteria (PLR, crest) cap the bands. */
/** KEEP INPUT: the most drive above the level-neutral drive (dB) the limiter may use to hold the input's loudness. */
constexpr double kKeepMaxExtraDriveDb = 1.0;

/** Finished-master guard (a limited master with no headroom left: PLR under kDensePlrLu and a sample peak within
    kFinishedHeadroomDb of the ceiling). Reaching a louder target must not cost it more than this; beyond it the target
    is reported as not reachable instead of being forced. */
constexpr double kFinishedMaxBoostDb = 3.0, kFinishedMaxPeakGrDb = 2.0, kFinishedMaxMeanGrDb = 2.0, kFinishedHeadroomDb = 4.0;
/** LEARN that heard less than this many seconds may be a quiet part only: the drive is held to kShortLearnMaxDriveDb. */
constexpr double kShortLearnSeconds = 10.0, kShortLearnMaxDriveDb = 4.0;
/** True for a finished master: the source is limited (PLR under kDensePlrLu) and its sample peak is within kFinishedHeadroomDb of the ceiling. */
bool isFinishedMaster (const SourceStats& src, double ceilingDbtp);

constexpr double kDensePlrLu = 11.0, kDenseCrestDb = 9.0, kDenseLraLu = 4.0, kDenseGlueDb = 0.5, kDenseRatio = 1.5;

struct PreLimiterCache
{
    bool keepTaps = false;              // keep the per-stage buffers (StageTaps) for the real-time views
    bool wantProxy = false;             // also make the LoudnessProxy of the signal entering the drive (the plug-in keeps it in the project)
    juce::String key;
    juce::String detectorKey;           // compressor input and detector settings of `level`
    std::vector<float> level;           // compressor detector level, dB per sample (WIDE)
    juce::String bandDetectorKey;       // compressor input, crossovers and detector settings of `bandLevel`
    std::vector<float> bandLevel[3];    // 3-BAND detector levels
    juce::String maskKey;               // the post-EQ signal `loudMask` was measured on
    std::vector<uint8_t> loudMask;      // loud part (short-term >= 70th percentile) of the post-EQ signal
    juce::String densityKey;            // the source buffer `density` / `source` were measured on
    SourceStats source;
    GlueAnalysis density;               // plr / crest / lra / dense of the source mix
    GlueAnalysis glue;
    InputStage input;
    DynamicEqAnalysis dynEq;
    WidthAnalysis width;
    StageTaps taps;
    std::shared_ptr<juce::AudioBuffer<float>> buffer;
    std::vector<float> truePeakEnv;     // of `buffer`, empty until needed
    GainReductionStats comp;
    double integratedPre = kNegInf;     // loudness of `buffer`
};

struct RenderResult
{
    std::shared_ptr<juce::AudioBuffer<float>> output;
    Measurement measured;
    GainReductionStats comp;
    GlueAnalysis glue;
    LimiterResult limiter;
    GlueAnalysis density;              // the source mix's density (dense rule), whether or not the compressor is in
    InputStage input;
    DynamicEqAnalysis dynEq;
    WidthAnalysis width;
    StageTaps taps;
    std::shared_ptr<const juce::AudioBuffer<float>> preLimiter;
    std::shared_ptr<const LoudnessProxy> proxy;       // when PreLimiterCache::wantProxy
    juce::StringArray warnings;
    double driveDb = 0.0;
    bool cancelled = false;
};

/** What the loudness stage does to the mix, in plain words (the header VFD, the TARGET block, the report).
    A hot mix brought down to a streaming target is said to be turned DOWN, with the platform that would do the same. */
struct LoudnessStory
{
    bool valid = false;
    double inLufs = kNegInf, outLufs = kNegInf, changeDb = 0.0;
    juce::String sentence;             // "Your mix is -9.1 LUFS; STREAM -14 turns it down 4.9 dB (Spotify and YouTube would do the same)."
    juce::String shortLine;            // "MIX -9.1 LUFS > -14.0: TURNED DOWN 4.9 DB (SPOTIFY WOULD TOO)"
    juce::String line1, line2;         // two lines for the TARGET block
    int severity = 0;                  // what the limiter costs to reach the target: 0 fine (avg GR <= 2 dB), 1 warn (2..4 dB), 2 strong warn (> 4 dB)
};

struct MatchResult
{
    bool reached = false;
    double driveDb = 0.0;
    double achievedLufs = kNegInf;
    double truePeak = kNegInf;
    double maxLimitingDb = 0.0;
    int iterations = 0;
    juce::String message;
};

class MasterChain
{
public:
    /** The loudness the drive is solved for: the preset's (or CUSTOM) value, or with KEEP INPUT the source's own
        integrated loudness (as it arrives, before trim). */
    static double effectiveTarget (const MasterSettings& s, const SourceStats& source);

    /** The plain statement of what the loudness stage does: input loudness, output loudness, turned up or down, and why. */
    static LoudnessStory loudnessStory (const MasterSettings& s, double inLufs, double outLufs, double limiterMaxGrDb);

    /** Distance between two spectra (dB on spectrumGrid()): level-aligned RMS difference over 60 Hz..12 kHz, dB. */
    static double toneDistanceDb (const std::vector<float>& a, const std::vector<float>& b);

    /** Key of everything that affects the pre-limiter signal. */
    static juce::String preLimiterKey (const MasterSettings& s, double sampleRate, int numSamples);

    /** Source statistics (peak, loudness, LRA, PLR, crest), the ones the dense rule and the headroom assistant use. */
    static SourceStats measureSource (const juce::AudioBuffer<float>& in, double sampleRate);

    /** Trim, EQ, compressor, width. Fills or reuses the cache. */
    static void renderPreLimiter (const juce::AudioBuffer<float>& in, double sampleRate, const MasterSettings& s, PreLimiterCache& cache);

    /** Whole chain. With BYPASS the output is a copy of the input. */
    static RenderResult render (const juce::AudioBuffer<float>& in, double sampleRate, const MasterSettings& s,
                                PreLimiterCache& cache, const std::atomic<bool>* cancel = nullptr);

    /** Solve the drive so the output integrated loudness hits s.targetLufs (within 0.2 LU; we aim for 0.05),
        with the limiter holding the true-peak ceiling. Writes the drive into s.limiter.driveDb. */
    static MatchResult matchTarget (const juce::AudioBuffer<float>& in, double sampleRate, MasterSettings& s, PreLimiterCache& cache,
                                    RenderResult* finalRender = nullptr, const std::atomic<bool>* cancel = nullptr);

    /** What the panel does after every change: with TARGET LOCK on (and BYPASS off) the limiter drive is re-solved
        (matchTarget) so the output stays on the target; otherwise a plain render with the current drive.
        Writes the solved drive into s.limiter.driveDb and, in AUTO, the solved threshold(s) into s.comp.thresholdDb
        (WIDE) or s.comp.bands[b].thresholdDb (3-BAND). */
    static RenderResult renderLocked (const juce::AudioBuffer<float>& in, double sampleRate, MasterSettings& s, PreLimiterCache& cache,
                                      const std::atomic<bool>* cancel = nullptr, MatchResult* match = nullptr);

    /** Smooth corrective EQ curve (dB on spectrumGrid()) that moves `source` (with the manual EQ bands) towards `reference`.
        Levels are aligned first (mean difference over 60 Hz..12 kHz), then smoothed over `smoothOct`
        octaves, limited to +-6 dB and faded out below 40 Hz and above 14 kHz. */
    static std::vector<float> toneMatchCurve (const std::vector<float>& sourceSpectrum, const std::vector<float>& referenceSpectrum,
                                              const EqSettings& manual, double smoothOct);

    /** Reference minus source (with the full EQ applied analytically), level-aligned, for display. */
    static std::vector<float> differenceCurve (const std::vector<float>& sourceSpectrum, const std::vector<float>& referenceSpectrum,
                                               const EqSettings* eq);

    /** Writes the solved AUTO thresholds of a render back into the settings (WIDE: comp.thresholdDb; 3-BAND: the bands'). */
    static void storeSolvedThresholds (MasterSettings& s, const GlueAnalysis& glue);

    /** Honest warnings about a finished render. */
    static juce::StringArray warningsFor (const MasterSettings& s, const Measurement& out, const LimiterResult& lim);

    /** Plain-text report written next to an export. */
    static juce::String report (const juce::String& sourceName, const juce::String& outName, int bits, const MasterSettings& s,
                                const Measurement& before, const Measurement& after, const RenderResult& r,
                                const Measurement* reference);
};
} // namespace mihenk
