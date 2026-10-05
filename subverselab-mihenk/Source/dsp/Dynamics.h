#pragma once

#include <JuceHeader.h>
#include "../engine/Settings.h"
#include "Filters.h"

namespace mihenk
{
/** The limiter detects at 8x (the meter reads at 4x, as BS.1770 specifies), so it also
    holds the continuous waveform between the meter's points. */
constexpr int kLimiterOversampling = 8;

struct GainReductionStats
{
    double maxDb = 0.0;          // largest gain reduction, positive dB
    double meanDb = 0.0;         // mean over the whole file
    double activeFraction = 0.0; // share of samples reduced by more than 0.1 dB
    std::vector<float> history;  // largest reduction per 10 ms, positive dB
};

/*  Glue compressor. Feed-forward, stereo-linked, soft knee.
    Detector: sidechain high-pass (12 dB/oct), then a blend of a peak detector
    (instant attack, 50 ms decay) and a 25 ms RMS detector calibrated +3.01 dB
    so a sine reads the same on both. Gain reduction is smoothed in dB.
    Auto release is program dependent: a slow stage (attack x3, release x4)
    follows sustained reduction and a fast stage (release / 3) follows
    transients on top of it, so short peaks recover fast and long passages
    recover slowly. Mix is a parallel blend with the dry signal (no latency,
    so no comb filtering). */
void processCompressor (juce::AudioBuffer<float>& buffer, double sampleRate, const CompSettings& s, GainReductionStats& stats,
                        std::vector<float>* grDbOut = nullptr, const std::vector<float>* levelDbIn = nullptr);

/** The compressor's detector level in dB per sample (sidechain HPF, peak / RMS blend). It does not depend on
    threshold, ratio, knee or ballistics, so the glue solver computes it once. */
void compressorDetector (const juce::AudioBuffer<float>& buffer, double sampleRate, const CompSettings& s, std::vector<float>& levelDb);

/** Gain computer and ballistics on a detector level: gain reduction in dB per sample (<= 0). */
void compressorGainDb (const std::vector<float>& levelDb, double sampleRate, const CompSettings& s, std::vector<float>& grDb);

/** Per-sample mask of the loud part of a track: samples whose (centred) short-term loudness is at or above the
    given percentile of the gated short-term values. Everything counts when there are no short-term values. */
std::vector<uint8_t> loudPartMask (const std::vector<float>& shortTerm, int numSamples, double sampleRate, double percentile = 0.7);

/** Mean and max gain reduction (positive dB) over the masked samples. */
void maskedGrStats (const std::vector<float>& grDb, const std::vector<uint8_t>& mask, double& meanDb, double& maxDb);

/** AUTO threshold: the threshold at which the mean gain reduction over the masked (loud) samples equals targetGrDb,
    found by bracketed regula falsi with the real detector and ballistics (settings c, threshold ignored). */
double solveGlueThreshold (const std::vector<float>& levelDb, const std::vector<uint8_t>& mask, double sampleRate, CompSettings c,
                           double targetGrDb, int* iterations = nullptr);

//==============================================================================
/*  Zero-phase filtering and the 3-band split.
    A 2nd-order Butterworth run forward and then backward has the magnitude |H|^2 and no phase: the Linkwitz-Riley
    4th-order magnitude (-6 dB at fc, 24 dB/oct). Because a Butterworth pair is power complementary
    (|LP|^2 + |HP|^2 = 1), x - LP_zp(x) is exactly the LR4 high-pass, so the bands sum back to the input. */
std::vector<double> zeroPhaseLowPass (const float* x, int n, double sampleRate, double fc);
void zeroPhaseLowPassInPlace (std::vector<double>& x, double sampleRate, double fc);

/** low = LR4 low-pass at f1, rest = x - low, mid = LR4 low-pass of rest at f2, high = rest - mid (low + mid + high = x). */
void splitThreeBands (const juce::AudioBuffer<float>& in, double sampleRate, double f1, double f2,
                      juce::AudioBuffer<float>& low, juce::AudioBuffer<float>& mid, juce::AudioBuffer<float>& high);

/** DC filter: zero-phase 2nd-order Butterworth high-pass at 5 Hz (forward-backward, so nothing above is shifted in time). */
void dcFilter (juce::AudioBuffer<float>& buffer, double sampleRate);

/** L/R balance correction: left x 10^(-dB/40), right x 10^(+dB/40). */
void applyBalance (juce::AudioBuffer<float>& buffer, double balanceDb);

//==============================================================================
/*  Dynamic EQ band (see DynBandSettings): y = x + (g(t) - 1) * C(x). The detector is the stereo-linked level of C(x)
    (70 % RMS, the glue's detector without its sidechain filter), the gain computer and ballistics are the glue's,
    with the static cut limited to rangeDb. In AUTO the threshold is solved so the mean cut over `loudMask` = amountDb. */
struct DynBandResult
{
    bool active = false;
    double thresholdDb = 0.0;
    double loudMeanCutDb = 0.0;        // positive dB
    double maxCutDb = 0.0;
    GainReductionStats gr;
};
void processDynBand (juce::AudioBuffer<float>& buffer, double sampleRate, const DynBandSettings& s, const std::vector<uint8_t>& loudMask,
                     DynBandResult& result);

/** The component filter C of a dynamic band (for checks): constant 0 dB peak band-pass, or Butterworth low-pass. */
Biquad dynBandComponent (const DynBandSettings& s, double sampleRate);

/*  M/S width. Side gain = width; "mono below" removes the side signal below
    the frequency with a zero-phase (forward-backward) 2nd-order Butterworth
    split, so the part above is exactly complementary and the mid is never
    touched. Stereo only; returns untouched when neutral. */
void processWidth (juce::AudioBuffer<float>& buffer, double sampleRate, const WidthSettings& s, double* lowCorrelationOut = nullptr,
                   double* safeSignOut = nullptr, double* safeGainOut = nullptr);

/** Correlation of the two channels' low bands (zero-phase LR4 low-pass at fc); 1 for mono or silence. */
double lowBandCorrelation (const juce::AudioBuffer<float>& buffer, double sampleRate, double fc);
bool widthIsNeutral (const WidthSettings& s);

/*  True-peak limiter, offline.
      1. Detection on the true-peak envelope (8x oversampled, linked).
      2. Required gain per sample = min(1, ceiling / (drive * truePeak)).
      3. Look-ahead minimum over L samples, then exponential release, then a
         moving average over L samples. The average of L values that are all
         <= the required gain at the peak is <= that gain, so every detected
         peak is at or under the ceiling, with a smooth L-sample attack.
      4. The gain is applied at the base rate, so the result is measured
         again at 8x; any residual inter-sample overshoot gets another pass
         (up to 3), and a last resort is a plain trim, which is reported. */
struct LimiterResult
{
    GainReductionStats gr;
    int passes = 0;
    double finalTrimDb = 0.0;    // nonzero only if the passes could not hold the ceiling
    double truePeakOut = 0.0;    // linear, measured at 8x
};

/** Gain curve for a given drive, from a precomputed true-peak envelope of the undriven signal. */
void limiterGainCurve (const std::vector<float>& truePeakEnv, double drive, double ceiling, int lookahead,
                       double releaseCoef, std::vector<float>& gain);

int limiterLookaheadSamples (const LimiterSettings& s, double sampleRate);
double limiterReleaseCoef (const LimiterSettings& s, double sampleRate);

/** out = in * drive * gain(drive), no safety passes (used by the target solver). */
void applyDriveAndLimit (const juce::AudioBuffer<float>& in, const std::vector<float>& truePeakEnv, double sampleRate,
                         const LimiterSettings& s, double driveDb, juce::AudioBuffer<float>& out, std::vector<float>& gainScratch);

/** Full limiter stage with safety passes and statistics. `truePeakEnv` is the 8x envelope of `in`. */
LimiterResult limitWithSafety (const juce::AudioBuffer<float>& in, const std::vector<float>& truePeakEnv, double sampleRate,
                               const LimiterSettings& s, juce::AudioBuffer<float>& out);

GainReductionStats gainStats (const std::vector<float>& gain, double sampleRate);
} // namespace mihenk
