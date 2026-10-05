#pragma once

#include <JuceHeader.h>
#include <deque>
#include "../engine/MasterChain.h"
#include "../engine/AudioIO.h"
#include "../engine/Diagnosis.h"
#include "../engine/Profile.h"
#include "../engine/Plan.h"
#include <map>
#include "Player.h"

/*  MIHENK v3 (SPEC_V3): the master is a pure function, master = solvePlan (measurements, intent).
      - `measurements` come from the one analysis of the loaded file (or LEARN) and never change afterwards.
      - `intent` (target, ceiling, character, reference, overrides) is the only thing a click, a knob, a preset, a profile or a
        host parameter changes. An override is a value the user put on top of the plan; it is stored as such and shown as a delta.
      - `settings` is never a place to keep a decision: after every change it is replaced by `plan.settings` (+ overrides).
        The panel and the plug-in still write knob values into `settings` and then call settingsChanged(); that one call is the
        only place where such a write is read, as an edit of the intent or as an override, and then `settings` is rebuilt.

    Everything the panel shows and does, off the panel: the source (a loaded file in the app, what LEARN captured in
    the plug-in), its measurements and diagnosis, the settings, the rendered master and the player.
    Loading, analysing, rendering and target matching run on one worker thread; results come back on the message
    thread and a change message tells the panel. A new render request cancels a render that is still running.
    The audio device belongs to the processor (the standalone wrapper opens it); Session never touches it.
*/
namespace mihenk
{
class Session : private juce::Thread, public juce::ChangeBroadcaster
{
public:
    Session();
    ~Session() override;

    void loadInput (const juce::File& f);
    void loadReference (const juce::File& f);
    /** The plug-in's LEARN: analyse and diagnose this audio as the source, then render it (worker thread). */
    void learnInput (std::shared_ptr<const juce::AudioBuffer<float>> audio, double rate, const juce::String& name);
    /** Load from memory (snapshot tool / tests); runs synchronously. */
    void setInputForTesting (juce::AudioBuffer<float> b, double rate, const juce::String& name);
    void setReferenceForTesting (juce::AudioBuffer<float> b, double rate, const juce::String& name);
    void renderNowForTesting();

    /** The panel changed `settings`; render again (debounced). */
    void settingsChanged();
    void matchTarget();
    void matchTone();
    void matchLoudness();
    /** MATCH TO REFERENCE: the tone match curve and the reference's loudness as the target, in one action. */
    void matchToReference();
    void clearReference();
    /** RESET ALL (message thread): every setting, the tone-match curve, the dynamic-EQ details, the profile locks, the analysis and
        the reference go back to a new instance; running work is dropped. keepInputFile (the standalone app): the loaded file
        stays loaded and is rendered with the new settings; otherwise the source (what LEARN captured) is cleared too. */
    void resetAll (bool keepInputFile);
    /** What the loudness stage does to this mix, in plain words (header VFD and TARGET block). */
    LoudnessStory loudness() const;
    /** Tone distance to the reference (level-aligned RMS dB, 60 Hz..12 kHz) of the source and of the master; < 0 when none. */
    double toneDistanceBefore() const;
    double toneDistanceNow() const;

    /** A / B: A (the original) is played at the master's loudness (default), or at its own level (raw) when off. */
    bool abLevelMatched() const noexcept { return abMatched; }
    void setAbLevelMatched (bool on);
    /** The gain A is played with now (1 when raw). */
    float originalGainDb() const;
    /** Starting points and scenarios no longer write knobs (SPEC_V3 section 1): these only say so. The diagnosis stays information;
        with CHARACTER FULL the plan applies the findings it can. */
    void applyGenre (Genre g);
    /** Applies a diagnosis scenario on top of the settings, with the amounts measured on the source
        (documented defaults when nothing is loaded), and re-renders. The change is remembered so it can be removed. */
    void applyScenario (int scenario);
    /** Takes an applied scenario back out: every setting it changed that still has the value it set returns to the
        value before it (settings changed by hand since then are left alone). */
    void removeScenario (int scenario);
    /** The findings with their deltas written against the settings as they are now. */
    Diagnosis currentDiagnosis() const { return diagnose (diagnosis.m, settings); }
    bool exportMaster (const juce::File& f, int bits, juce::String& message);

    MasterSettings settings;

    //==============================================================================
    // v3: the intent, the frozen measurements, the plan
    Intent intent;
    Measurements measurements;                    // present = false until a file is loaded or LEARN has heard something
    struct SavedThresholds { bool valid = false; double comp = -18.0, band[3] { -24.0, -24.0, -24.0 }, atTrim = 0.0; } savedThresholds;   // the AUTO thresholds LEARN solved, saved with the learned block (a state that lost its parameters still has them)
    SavedThresholds currentThresholds() const
    {
        SavedThresholds t; t.valid = true;
        t.comp = haveSolvedThresholds ? solvedCompThreshold : settings.comp.thresholdDb;
        for (int b = 0; b < 3; ++b) t.band[b] = haveSolvedThresholds ? solvedBandThreshold[b] : settings.comp.bands[b].thresholdDb;
        t.atTrim = haveSolvedThresholds ? solvedAtTrimDb : settings.trimDb;
        return t;
    }
    double savedStageDelta = 0.0;   // the measured stage loudness (Measurements::stageDeltaDb) saved with the learned block
    LimiterResponse savedLimiter;   // the limiter's response saved with a LEARN block: a reopened project has no audio to measure it on
    Plan plan, basePlan;                          // basePlan: the same intent without the overrides (what an override is a delta over)
    void setTarget (PlanTarget t);
    void setCustomTarget (double lufs);
    void setCeiling (double dbtp);
    void setCharacter (PlanCharacter c);
    /** The panel's / the host's / a preset's / the profile's write, read as intent: compares `settings` with what was shown last. */
    void absorbEdits();
    /** settings = solvePlan (measurements, intent). No render is requested. */
    void replan();
    /** SPEC_V3 2 REVISED, calibration: the table of the limiter is measured on the untouched mix; the stages an intent switches on
        (EQ, compressor, AIR) add loudness before the limiter. After a render, the miss against the target is measured once and held
        in measurements.stageDeltaDb (a measurement of the real chain, not a second solver: it is reset whenever the intent changes and
        is an absolute value, never accumulated across intents). Returns true when the plan changed and another render is needed. */
    bool calibrationStep (double renderedLufs);
    juce::String calibrationKey() const;
    juce::String calibKey;
    int calibRounds = 0;
    void clearOverrides();
    void clearOverridesQuietly();                 // no settingsChanged (a new file, RESET)
    /** A preset (a user file, a batch row's own settings) read as intent: target and ceiling become intent, every other value that
        differs from the plan becomes an override. The level (trim, drive) is the plan's and is never taken from a preset. */
    void applyPreset (const MasterSettings& loaded);
    bool isOverridden (const juce::String& id) const { return intent.overrides.count (id) > 0; }
    int overrideCount() const { return (int) intent.overrides.size(); }
    /** The shown value minus the plan's own value for this parameter id (0 when it is not overridden). */
    double overrideDelta (const juce::String& id) const;
    /** One paragraph in plain words: measured input, what the plan does (dB), what it refused and why, the overrides, the measured result. */
    juce::String planParagraph() const;
    void shownFromSettings() { shown = settings; }
    /** A plug-in state without what LEARN found: the source, its analysis and the measurements are cleared (settings stay). */
    void forgetSource();
    /** A restored plug-in state: the intent as saved (or, for a project from before v3, from `legacy`), then the plan. */
    void restoreState (const juce::var& savedIntent, const MasterSettings& legacy);
    juce::var intentToVar() const;
    void intentFromVar (const juce::var&);
    /** The measurements (and the plan) from inMeas / diagnosis, as after a load; used when a state is restored. songSecondsKnown: a file, not LEARN. */
    void rebuildMeasurements (bool songSecondsKnown);
    static PlanTarget targetOfPreset (TargetPreset p);
    static TargetPreset presetOfTarget (PlanTarget t);

    /** The user set an EQ, compressor, stereo or tone-match control by hand (a knob, a switch, a starting point, a scenario, a
        preset, a reference match): the automatic skip for finished input stops managing them for this track. With restoreStash
        the stages the skip had switched off come back first (the control the user just touched is applied after). */
    void noteHandEdit (bool restoreStash = true);

    /** The standalone app: BYPASS is an instant switch in the player (the original, raw), not a re-render of the file.
        The master keeps rendering in the background with every other change, so switching back is instant too. */
    bool instantBypass = false;

    // State for the panel (message thread).
    juce::File inputFile, referenceFile;
    juce::String inputName, referenceName, inputNote;
    double sampleRate = 0.0;
    int sourceBits = 0;
    Player::BufferPtr input, reference, output;
    Measurement inMeas, refMeas, outMeas;
    GainReductionStats compGr, limGr;
    GlueAnalysis glue;                  // compressor: density, solved threshold, loud-part GR (per band in 3-BAND)
    InputStage inputStage;              // headroom assistant
    DynamicEqAnalysis dynEq;            // dynamic EQ / dynamic AIR
    Diagnosis diagnosis;                // of the source, measured once on load / learn
    juce::Array<int> appliedScenarios;  // since the source was loaded, in the order applied (the chips)
    LimiterResult limResult;
    juce::StringArray warnings;
    juce::String status;
    bool busy() const { return jobsPending.load() > 0; }
    bool outputCurrent() const { return output != nullptr && ! busy() && renderedGeneration == requestedGeneration; }
    /** A whole-track analysis is available (a loaded file, a LEARN, or one restored with a plug-in state). */
    bool hasAnalysis() const { return inMeas.valid; }
    /** The measurement of the processed master (outMeas shows the input while BYPASS is on). */
    const Measurement& processed() const noexcept { return procMeas; }

    //==============================================================================
    // Personal profile (SPEC_V2 §7, opt-in). Message thread only; the audio thread only ever sees parameter values.
    profile::Store profile;
    bool profileEnabled = false;
    juce::String profileSource { "app" };
    struct Lock { bool on = false; double value = 0.0; int evidence = 0; juce::String reason, ruleKey; juce::StringArray ids; };
    std::array<Lock, profile::kNumParams> locks;
    /** Knobs the user unlocked on this track (the value they were locked at); saved with the next note as overrides. */
    std::map<int, double> overridden;
    void setProfileEnabled (bool on);
    /** For the source just measured: proposes the learned values of the nearest group of your notes and locks those
        knobs (settings changed, no render requested). Returns the number of knobs locked. */
    int applyProfile();                 // v3: the proposals become overrides (the profile is a default intent) and the knobs are locked
    bool isLocked (int param) const { return param >= 0 && param < profile::kNumParams && locks[(size_t) param].on; }
    int lockCount() const;
    /** The user unlocked a knob (lock icon, or moved it): it stays where it is and the change is learned with the next note. */
    void unlock (int param);
    /** Puts every locked knob back on its locked value (a starting point, scenario or preset does not move a lock). */
    void holdLocks();
    /** Saves a listening note with the full context of the current source, settings and result. */
    bool addNote (const juce::StringArray& tags, const juce::String& text, const juce::String& startingPoint, juce::String& error);
    juce::var profileStateToVar() const;
    void profileStateFromVar (const juce::var&);

    //==============================================================================
    // Undo / redo (owner 2026-10-04). One step = the difference between two snapshots of what the USER decided: {target, ceiling, character,
    // the tone-match curve, every override, the reference choice, the profile locks}. Measurements, the plan, the render and the audio
    // thread are never in the history: undo writes the snapshot back and goes through settingsChanged(), so the master is solved again from
    // the same measurements and the same intent, bit for bit. The history is recorded at the end of settingsChanged() (and when a reference
    // finishes loading), so every way of changing the intent (a click, a knob, a typed value, a preset, a test) is one step; a gesture
    // (beginUndoGesture .. endUndoGesture, a knob drag) is one step however many settingsChanged() calls it holds.
    // A new source (file loaded, LEARN) or a restored plug-in state starts a new history; RESET ALL with the file kept is one step.
    bool canUndo() const;
    bool canRedo() const;
    /** The name of the step undo / redo would take ("Target CLUB -8", "AIR override +1.2 dB"); empty when there is none. */
    juce::String undoName() const;
    juce::String redoName() const;
    bool undo();
    bool redo();
    void beginUndoGesture();
    void endUndoGesture();
    /** A new history from here: the current state is the base. */
    void clearUndoHistory();
    int undoSteps() const;
    int redoSteps() const;
    /** A step for state that lives outside the session (the batch panel's per-song settings): `undoFn` puts it back, `redoFn` does it again.
        Both run on the message thread and must tolerate that what they refer to is gone (return, never fail). */
    void pushUndoAction (const juce::String& name, std::function<void()> undoFn, std::function<void()> redoFn);
    /** The plug-in: the automatable parameters (target, ceiling, every override) are undone by the host (its own Cmd+Z). With this on, a change
        that touches only those is not put in this history; whatever else a step changes (character, reference, locks) is. The app leaves it off. */
    bool undoHostOwnsParams = false;
    /** The host moved a parameter (automation, the host's own undo): the session follows it, and the history does not record it. */
    class HostEdit
    {
    public:
        explicit HostEdit (Session& s) : owner (s) { ++owner.hostEditDepth; }
        ~HostEdit() { --owner.hostEditDepth; }
        HostEdit (const HostEdit&) = delete;
        HostEdit& operator= (const HostEdit&) = delete;
    private:
        Session& owner;
    };

    /** The app keeps the stage signals of each render for the live meters (16-bit copies in the player). */
    bool keepStageTaps = false;
    /** The plug-in keeps a LoudnessProxy of every render (RenderResult::proxy) so a reopened project can solve the drive again. */
    bool keepProxy = false;
    /** The plug-in's answer to a settings change when there is no audio to render (a reopened project): true when it handled it. */
    std::function<bool()> resolveWithoutAudio;

    // Hooks for the processor (message thread).
    std::function<void()> onSettingsEdited;                                               // settingsChanged() was called
    std::function<void (const RenderResult&, const MasterSettings& solved)> onRendered;   // a render was applied

    /** Remembered per applied scenario: the settings (as JSON) just before and just after it was applied. */
    struct ScenarioChange { int scenario = 0; juce::var before, after; };
    std::vector<ScenarioChange> scenarioChanges;

    Player player;

private:
    struct Job
    {
        enum class Kind { loadInput, loadReference, learn, render, match } kind;
        juce::File file;
        MasterSettings settings;
        Player::BufferPtr in;
        double rate = 0.0;
        juce::String name;
        int generation = 0;
        bool wantProxy = false;
        int epoch = 0;                 // resetEpoch when queued: a result that arrives after RESET ALL is dropped
    };

    void run() override;
    void enqueue (Job j);
    void renderRequest (Job::Kind kind);
    void applyRender (const RenderResult& r, int generation, const MasterSettings* solved = nullptr);
    void setInput (Player::BufferPtr b, double rate, const Measurement& m);
    void updateOriginalGain();

    juce::CriticalSection jobLock;
    std::deque<Job> jobs;
    std::atomic<int> jobsPending { 0 };
    std::atomic<bool> cancelRender { false };
    juce::WaitableEvent wake;
    double lastRequestMs = 0.0;
    int requestedGeneration = 0, renderedGeneration = 0;
    PreLimiterCache cache;             // worker thread only
    bool abMatched = true;
    Player::BufferPtr keepAlive;       // previous master, so the audio thread never frees a buffer
    Player::TapPtr keepEq, keepComp;
    Player::GrPtr keepGr;
    int resetEpoch = 0;
    std::atomic<bool> cacheClear { false };   // the worker drops its pre-limiter cache at the next job
    juce::String lastRenderKey;        // settingsFingerprint of the last render request (bypass left out)
    Measurement procMeas;              // measurement of the processed master (outMeas shows the input while bypassed)
    juce::StringArray procWarnings;
    void showBypassState();
    void refreshStatus();
    MasterSettings shown;              // what the panel / host was last given: the base an edit is read against
    std::map<juce::String, double> ovDelta;    // numeric overrides, stored as the delta over the plan (so a new target keeps them)
    std::map<juce::String, double> ovSwitch;   // switches and choices, stored as the absolute value
    void rebuildOverrides();
    double solvedCompThreshold = -18.0, solvedBandThreshold[3] { -24.0, -24.0, -24.0 };
    bool haveSolvedThresholds = false;
    double solvedAtTrimDb = 0.0;                 // the trim the AUTO thresholds were solved at: the compressor is level-covariant, so a threshold follows the trim dB for dB
    int profileLocked = 0;

    // Undo (see the public section)
    struct UndoState
    {
        PlanTarget target = PlanTarget::STREAM_14;
        double customLufs = -14.0, albumTarget = -14.0, relativeDb = 0.0, ceilingDbtp = -1.0, referenceLoudness = kNegInf;
        PlanCharacter character = PlanCharacter::OFF;
        std::vector<float> referenceCurve;
        std::map<juce::String, double> ovDelta, ovSwitch;
        std::array<Lock, profile::kNumParams> locks;
        std::map<int, double> overridden;
        bool profileEnabled = false;
        Player::BufferPtr reference;
        juce::File referenceFile;
        juce::String referenceName;
        std::shared_ptr<const Measurement> refMeas;
    };
    enum UndoGroup { ugTarget = 1, ugCeiling = 2, ugCharacter = 4, ugRefCurve = 8, ugRefChoice = 16, ugLocks = 32, ugAlbum = 64 };
    struct OvChange { juce::String id; bool isSwitch = false, hadFrom = false, hasTo = false; double from = 0.0, to = 0.0; };
    struct UndoStep;
    UndoState captureUndoState() const;
    static bool diffUndoStates (const UndoState& a, const UndoState& b, int& groups, std::vector<OvChange>& ov);
    static juce::String describeUndo (const UndoState& a, const UndoState& b, int groups, const std::vector<OvChange>& ov);
    void recordUndo();
    void commitUndo();
    void applyUndoStep (const UndoStep&, bool backward);
    juce::UndoManager undoManager { 30000, 400 };
    UndoState committed;
    int gestureDepth = 0, hostEditDepth = 0;
    bool restoring = false, gestureDirty = false;
    juce::String pendingUndoName;

    JUCE_DECLARE_WEAK_REFERENCEABLE (Session)
};
} // namespace mihenk
