#pragma once

#include <JuceHeader.h>
#include <vector>
#include "Settings.h"

/*  Mix diagnosis: measured findings, each tied to a scenario preset.

    On load the mix is measured once (measureMix) and 20 rules run on the numbers (diagnose). Every finding carries
    the scenario it maps to, a severity, a one-line reason with the measured values, and the settings change the
    scenario would make ("suggested delta", written by comparing the settings before and after applyScenario).
    Nothing is guessed: each rule is a documented threshold on a measured value (the constants below), and each has
    a synthetic trigger case and a "clean mix" non-trigger case in `render --selftest`.
*/
namespace mihenk
{
enum class Severity { info = 0, warn = 1, problem = 2 };

enum Scenario
{
    scTooHot = 1, scClipped, scTooQuiet, scPhase, scTooWide, scTooNarrow, scImbalance, scDcOffset, scMuddy, scBoomy,
    scThin, scHarsh, scSibilance, scDull, scBuriedVocal, scOverCompressed, scTooDynamic, scUnevenLow, scResonance, scCodecRisk
};
constexpr int kNumScenarios = 20;

const char* scenarioId (int scenario);      // "too-hot" ...
const char* scenarioName (int scenario);    // "Too hot, no headroom" ...
int scenarioFromId (const juce::String& id); // 0 when unknown

/*  Thresholds of the 20 rules. Spectral values are on the long-term 1/6-octave band-power spectrum (pink noise
    reads flat). "Line" = least-squares straight line (dB against log2 f) over 100 Hz..10 kHz, fitted with the region
    under test (+-1/2 octave) left out; a region's deviation is its mean distance above that line, so a mix's overall
    tilt never counts as an excess and a bump cannot hide itself by pulling the line up. Levels are dBFS / LUFS.
    Chosen against synthetic cases and checked on real mixes and masters (STATUS.md lists the calibration set). */
namespace diag
{
    constexpr double hotSamplePeakDbfs = -1.0, hotTruePeakDbtp = -0.5;        // 1: either at or above
    constexpr int clipMinRunSamples = 4;                                       // 2: flat run = >= 4 equal samples at >= 99.9 % of the file peak
    constexpr double clipRunsPerMinWarn = 6.0, clipRunsPerMinProblem = 60.0;   //    and at least 10 runs in the file
    constexpr int clipMinRuns = 10;
    constexpr double quietPeakDbfs = -12.0, quietPeakProblemDbfs = -20.0;      // 3: sample peak below
    constexpr double phaseLowCorr = 0.2, phaseMonoLowLossDb = -4.5, phaseCorr = 0.0; // 4: low-band (< 150 Hz) correlation, mono loss of the lows, overall correlation
    constexpr double wideSideMidHighDb = -1.0;                                 // 5: side / mid energy above 4 kHz
    constexpr double narrowSideMidDb = -20.0;                                  // 6: side / mid energy, whole band
    constexpr double imbalanceDb = 1.0, imbalanceLowDb = 1.5;                  // 7: RMS L - R, whole band / below 150 Hz
    constexpr double dcDbfs = -60.0;                                           // 8: largest channel mean
    constexpr double mudDb = 4.0, mudProblemDb = 6.0;                          // 9: 200-500 Hz above the line
    constexpr double boomySubDb = 6.0, boomyLowCrestDb = 10.0, boomySubAloneDb = 10.0; // 10: 25-60 Hz over 100-300 Hz; with low crest (< 100 Hz) under 10 dB, or alone
    constexpr double thinLowDb = -3.0, thinProblemDb = -6.0;                   // 11: 40-120 Hz over 300 Hz-1 kHz
    constexpr double harshDb = 4.0, harshProblemDb = 6.5;                      // 12: 2-5 kHz above the line
    constexpr double sibPeakDb = 12.0, sibAbsDb = -2.0;                        // 13: 99th percentile of the 5-9 kHz / rest-of-signal ratio (20 ms) over its median, and absolute
                                                                               //     (hi-hats reach 14-24 dB over the median in real mixes and trap hat rolls -2.7 dB absolute)
    constexpr double dullDb = -18.0, dullProblemDb = -24.0;                    // 14: 10-16 kHz over 1-4 kHz
    constexpr double presenceDipDb = -3.5, presenceProblemDb = -6.0;           // 15: 1-4 kHz vs 300 Hz-1 kHz, relative to the line
    constexpr double overCompPlrProblem = 8.0;                                 // 16: PLR < 11 LU or crest < 9 dB (the dense rule's peak criteria; a low LRA alone is
                                                                               //     a steady arrangement, not compression); PLR < 8 is a problem
    constexpr double dynamicLraLu = 12.0, dynamicLraProblemLu = 16.0, dynamicPlrLu = 21.0; // 17: LRA, or PLR
    constexpr double unevenLowDb = 4.0, unevenLowProblemDb = 8.0;              // 18: p80 - p20 of (low band < 120 Hz minus the rest above it) over 3 s windows within 8 dB
                                                                               //     of the loudest (the main sections; intros and breaks are arrangement, not mix), leaving
                                                                               //     out windows where the low band sits 12 dB under its usual (p90) share (no bass there)
    constexpr double resonanceDb = 16.0, resonanceProblemDb = 20.0;            // 19: 1/24-oct band over its +-1/2 oct surroundings, 150 Hz..10 kHz
                                                                               //     (held notes of the key read 9-15 dB on real songs, so only a stronger peak counts)
    constexpr double codecTruePeakDbtp = -1.0, codecTolDb = 0.05;             // 20: true peak over -1 dBTP (+0.05 dB meter tolerance: a master limited to -1.0 reads -0.99),
    constexpr double codecIspDb = 1.0, codecIspMinTpDbtp = -2.0;              //     or true peak - sample peak over 1 dB with the true peak above -2 dBTP
}

/** Everything the rules read, measured once per mix. */
struct DiagnosisMeasures
{
    bool valid = false;
    bool stereo = false;
    double seconds = 0.0;
    double integrated = -300.0, shortTermMax = -300.0, lra = 0.0, truePeak = -300.0, samplePeak = -300.0, rms = -300.0, crest = 0.0, plr = 0.0;
    double correlation = 1.0;
    bool dense = false;                     // the glue's dense rule (PLR / crest / LRA)
    bool overCompressed = false;            // PLR < 11 LU or crest < 9 dB
    int clipRuns = 0;
    double clipRunsPerMin = 0.0;
    double dcDbfs = -300.0;
    double balanceDb = 0.0, lowBalanceDb = 0.0;        // L - R, dB
    double lowCorrelation = 1.0, monoLowLossDb = 0.0;
    double sideMidDb = -300.0, sideMidHighDb = -300.0;
    double lineSlopeDbPerOct = 0.0;
    double mudDb = 0.0, mudPeakHz = 300.0;
    double harshDb = 0.0, harshPeakHz = 3500.0;
    double presenceDb = 0.0;
    double subDb = 0.0, lowCrestDb = 0.0;
    double lowDb = 0.0;
    double airRelDb = 0.0;
    double sibPeakDb = 0.0, sibAbsDb = -300.0;
    double unevenLowDb = 0.0;
    double resonanceDb = 0.0, resonanceHz = 0.0;
    double ispDb = 0.0;                     // true peak - sample peak
    std::vector<float> spectrumDb;          // 1/6-oct, on spectrumGrid()
};

struct Finding
{
    int scenario = 0;
    juce::String id;
    Severity severity = Severity::info;
    juce::String title;
    juce::String reason;                    // one line with the measured numbers
    juce::String delta;                     // the settings change applyScenario makes, against the current settings
    double value = 0.0;                     // the rule's main measured value
};

struct Diagnosis
{
    DiagnosisMeasures m;
    std::vector<Finding> findings;          // most severe first, then in scenario order
    const Finding* find (int scenario) const;
    bool has (int scenario) const { return find (scenario) != nullptr; }
};

DiagnosisMeasures measureMix (const juce::AudioBuffer<float>& buffer, double sampleRate);

/** Runs the 20 rules. `current` is what the deltas are written against. */
Diagnosis diagnose (const DiagnosisMeasures& m, const MasterSettings& current);
Diagnosis diagnose (const juce::AudioBuffer<float>& buffer, double sampleRate, const MasterSettings& current);

/** Applies a scenario's corrective settings on top of `s`. With measurements the amounts follow them (trim, balance,
    band frequencies, resonance); without (a factory scenario preset loaded by hand) documented defaults are used. */
void applyScenario (MasterSettings& s, int scenario, const DiagnosisMeasures* m = nullptr);

/** Human-readable list of the settings that differ ("AIR 0.0 -> +3.0 dB, AIR FREQ 20 -> 15 kHz"). */
juce::String describeDelta (const MasterSettings& before, const MasterSettings& after);
} // namespace mihenk
