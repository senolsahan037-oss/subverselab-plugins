#pragma once

#include "MasterChain.h"
#include "Diagnosis.h"
#include <map>

/*  MIHENK v3: the master as a pure function.  master = solvePlan (measurements, intent).
    Nothing here keeps state, reads a file or draws a random number; the same measurements and the same intent give the
    same plan, in any order of calls. The plan carries every setting the chain runs with, written so that the chain
    applies exactly the plan (TARGET LOCK off, headroom assistant off, the drive cap set to the planned drive). */
namespace mihenk
{
enum class PlanTarget { STREAM_14, APPLE_16, CLUB_8, KEEP_INPUT, ALBUM, CUSTOM };
enum class PlanCharacter { OFF, GENTLE, FULL };

/** The true-peak limiter's measured response on this very audio (made once by measureLimiterResponse, with the analysis pass).
    Indexed by the drive over the ceiling (dB): the limiter is scale-free, so one table serves every ceiling. Empty = not measured
    (a plug-in before it has audio); the plan then uses a model. */
struct LimiterResponse
{
    std::vector<float> x, loss, avgGr, maxGr;   // ascending drive over the ceiling; loudness lost to the limiter; mean GR on the loudest 30 % of the song; peak GR (dB)
    bool valid() const { return x.size() >= 2; }
};

struct Measurements
{
    double integrated = -300.0, shortTermMax = -300.0, momentaryMax = -300.0, lra = 0.0;
    double samplePeak = -300.0, truePeak = -300.0, plr = 0.0, crest = 0.0;
    double loud30Percent = 0.0;                  // integrated loudness of the loudest 30 % of the short-term history, LUFS (0 = not measured)
    double correlation = 1.0, lowCorrelation = 1.0, dcOffset = -300.0;
    std::vector<float> spectrum;                 // 1/6-oct band power on the engine grid (121 points, 20 Hz..20.48 kHz); any length is read as log-spaced over that range
    // Band energies, each in dB relative to the mix's own curve (what diagnose measures):
    // sub 25-60 Hz over 100-300 Hz, low 40-120 Hz over 300 Hz-1 kHz, lowMid 200-500 Hz above the line, mid 1-4 kHz vs 300 Hz-1 kHz,
    // presence 2-5 kHz above the line, sibilance 5-9 kHz peak over its median, air 10-16 kHz over 1-4 kHz.
    struct BandEnergies { double sub = 0, low = 0, lowMid = 0, mid = 0, presence = 0, sibilance = 0, air = 0; } bands;
    bool bandsValid = false;                     // true when `bands` was measured; otherwise they are derived from `spectrum`
    std::vector<Finding> findings;
    double coverageSeconds = 0.0, songSeconds = 0.0;
    LimiterResponse limiter;
    double stageDeltaDb = 0.0;                   // loudness the stages the intent switches on (EQ, compressor, AIR, stereo) add before the limiter, MEASURED on the real render by the session / batch (0 = none known; always 0 for CHARACTER OFF without stage overrides)
    bool present = true;                         // false: nothing was analysed (a plug-in before LEARN); the plan then leaves the level alone
};

struct Intent
{
    PlanTarget target = PlanTarget::STREAM_14;
    double customLufs = -14.0, albumTarget = -14.0, relativeDb = 0.0, ceilingDbtp = -1.0;
    PlanCharacter character = PlanCharacter::OFF;
    std::vector<float> referenceCurve;
    double referenceLoudness = kNegInf;
    std::map<juce::String, double> overrides;    // parameter id -> absolute value, applied last, reported as a delta over the plan
};

struct PlanAction { juce::String action; double value = 0.0; juce::String reason; };
struct Plan
{
    MasterSettings settings;
    std::vector<PlanAction> explain;             // ordered; reasons are plain words (entries named "detail" carry the technical readout)
    juce::StringArray refused, warnings;
    bool targetUnreachable = false;
    bool finishedMaster = false;
    juce::String finishedWhy;                    // plain words: "its peaks are only 9.5 dB above the average loudness"
    double totalGainDb = 0.0, predictedAverageGrDb = 0.0, predictedPeakGrDb = 0.0;
    double desiredGainDb = 0.0;                  // what the target asks for before the caps (0 when nothing was measured)
    juce::String limiterStatus;                  // "-8 LUFS reached: the limiter takes up to 5.1 dB on the loudest peaks (avg 1.4 dB). Expect less punch on the drums." (empty when the limiter is idle)
    int limiterSeverity = 0;                     // 0 fine (avg GR <= 2 dB), 1 warn (2..4 dB, or noise floor), 2 strong warn (> 4 dB); the target is reached in every case
    juce::String summary() const;                // = plainSummary()
    juce::String plainSummary() const;           // one paragraph: what was done in dB, why, what was refused, "nothing else"
};

Plan solvePlan (const Measurements&, const Intent&);

/** Parameter ids the plan accepts as overrides (the panel's and the plug-in's ids), the value of one in a set of settings (NaN when
    unknown), and a way to set it. Level ids (trim, drive, limiter timing) stay allowed on finished material; the others do not. */
const juce::StringArray& overrideIds();
double settingValue (const MasterSettings&, const juce::String& id);
bool setSettingValue (MasterSettings&, const juce::String& id, double v);
bool overrideIsLevel (const juce::String& id);
/** The stage an override id belongs to: 1 EQ, 2 compressor, 3 stereo, 0 level / none; stageSwitch: it is that stage's own IN switch (eqOn / compOn / widthOn). */
int overrideStage (const juce::String& id, bool* stageSwitch = nullptr);
/** Runs the real limiter over `in` at a range of drives and stores the response in m.limiter (call after measurementsFrom, with the audio the measurements came from). */
void measureLimiterResponse (const juce::AudioBuffer<float>& in, double sampleRate, Measurements& m);
Measurements measurementsFrom (const Measurement&, const DiagnosisMeasures* = nullptr, double songSeconds = 0.0);
}
