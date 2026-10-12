#pragma once

#include <JuceHeader.h>
#include "SvlUndo.h"
#include "RingModel.h"
#if SVL_LICENSING
#include "LicenseConfig.h"
#endif

// The engine: a verbatim copy of prototypes/YANKI/Source/YankiEngine.h,
// md5 608b9d2317a63ab7939f7b64b3c2814e (render --selftest re-checks the md5).
// The plugin never edits it; the warnings JUCE's stricter set would raise in
// it are silenced here, at the include, not in the file.
JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wfloat-equal", "-Wshadow", "-Wshadow-field-in-constructor", "-Wshadow-uncaptured-local",
                                     "-Wconversion", "-Wsign-conversion", "-Wshorten-64-to-32", "-Wimplicit-int-conversion",
                                     "-Wimplicit-float-conversion", "-Wdouble-promotion", "-Wunused-member-function",
                                     "-Wunused-const-variable", "-Wunused-variable", "-Wunused-parameter", "-Wcomma",
                                     "-Wextra-semi", "-Wzero-as-null-pointer-constant", "-Wmissing-prototypes",
                                     "-Wunused-template", "-Wcast-align", "-Wswitch-enum", "-Wunreachable-code",
                                     "-Wunused-private-field")
#include "YankiEngine.h"
JUCE_END_IGNORE_WARNINGS_GCC_LIKE

namespace yankiIds
{
    inline constexpr const char* mood = "MOOD";
    inline constexpr const char* timeSync = "TIME_SYNC";
    inline constexpr const char* timeMs = "TIME_MS";
    inline constexpr const char* feedback = "FEEDBACK";
    inline constexpr const char* capOn = "CAP_ON";
    inline constexpr const char* capDb = "CAP_DB";
    inline constexpr const char* tone = "TONE";
    inline constexpr const char* wear = "WEAR";
    inline constexpr const char* mix = "MIX";
    inline constexpr const char* output = "OUTPUT";
    inline constexpr const char* endMode = "END_MODE";
    inline constexpr const char* endTrigger = "END_TRIGGER";
    inline constexpr const char* freeze = "FREEZE";
    inline constexpr const char* bypass = "BYPASS";
    inline constexpr const char* darken = "DARKEN";
    inline constexpr const char* riseCurve = "RISE_CURVE";
    inline constexpr const char* lockBars = "LOCK_BARS";
    inline constexpr const char* fineMs = "FINE_MS";
    inline constexpr const char* spread = "SPREAD";
    inline constexpr const char* heads = "HEADS";
}

class YankiProcessor : public juce::AudioProcessor,
                       private juce::AudioProcessorValueTreeState::Listener,
                       private juce::AsyncUpdater
{
public:
    YankiProcessor();
    ~YankiProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 10.0; }
    juce::AudioProcessorParameter* getBypassParameter() const override;

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    /** The host's "clear your tails" call (AU Reset, VST3 setActive): done on the audio thread at the next block. */
    void reset() override { resetRequested = true; }

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    static const juce::StringArray moodNames, syncNames, endNames, riseNames, lockNames, headsNames;
    // TIME_SYNC (v2): 1/16, 1/8, 1/8., 1/4, 1/4., 1/2, FREE. The plug-in resolves the division itself (quarter notes x factor)
    // and hands the engine free milliseconds, so the engine's own sync list (no dotted quarter) is not used.
    static constexpr int kSyncFree = 6;
    static constexpr double kSyncQuarters[6] { 0.25, 0.5, 0.75, 1.0, 1.5, 2.0 };
    static constexpr int kStateVersion = 2;                        // 1: sync list of six (1/2 = 4, FREE = 5); 2: seven entries
    static constexpr double kRiseMax[3] { 0.4, 0.7, 1.0 };        // RISE CURVE: SLOW / MED / FAST
    static constexpr double kLockBars[4] { 0.0, 1.0, 2.0, 4.0 };
    static constexpr double kFallbackBpm = 120.0;                 // no host tempo (standalone, no transport)

    /** The FEEDBACK knob position (engine knob, 0.7 = HOLD) that stands for a
        MOOD's own loop gain, drawn at recall while the loop runs the mood's
        exact arithmetic (RISE off) until FEEDBACK is touched. */
    static float naturalFeedbackKnob (int mood, int riseCurve);

    /** A starting point: measured on the house sources (tools/presets_v2.py, STATUS.md "v2 step 2"). Values are plain parameter
        values; timeMs < 0 leaves TIME (free) alone. Everything not listed goes back to its default. */
    struct Preset
    {
        const char* name;
        const char* group;
        int mood, sync;
        float timeMs, feedback, mix, tone, wear, spread;
        bool capOn;
        float capDb;
        int lockBars;
        float outputDb;
    };
    static constexpr int kNumPresets = 10;
    static const Preset kPresets[kNumPresets];
    /** Message thread. Writes the preset's parameters (gestures, so the host records them) and remembers it. */
    void applyPreset (int index);
    /** -1 = none (the plug-in's own opening state). */
    int currentPreset() const { return presetIndex.load(); }
    /** Double-click value (plain, not normalised) of a parameter: the active preset's value if it sets that parameter, else the current
        MOOD's recalled value (TIME, TONE, WEAR, MIX, SPREAD, FEEDBACK, HEADS), else the parameter default. Message thread. */
    float resetValueFor (const juce::String& paramId) const;
    /** True once a parameter of the current preset has moved. */
    bool presetEdited() const;

    /** MIX as the engine gets it: the first 3 % of the knob is a small ramp (x * x / 0.03), identical from 3 % up. */
    static float mixToEngine (float m) { return m >= 0.03f ? m : m * (m / 0.03f); }

    /** HEADS: 0 = the MOOD's own heads (what every project saved before this parameter has), 1 = one head, 2 = two heads (left / right),
        3 = three heads. Recalling a MOOD or a preset sets it back to 0. */
    static constexpr int kHeadsMood = 0;
    /** Heads a voicing runs by itself: Three Heads, Dub Spiral and Worn Loop play three, the others one. */
    static int moodHeads (int mood) { return ringmodel::effectiveHeads (mood, 0); }
    static int effectiveHeads (int mood, int headsParam) { return ringmodel::effectiveHeads (mood, headsParam); }
    /** The engine's head mode for a HEADS choice (the MOOD's own mode for 0): 1 = head 1, 4 = heads 1 + 2, 12 = three heads. */
    static int engineMode (int moodMode, int headsParam) { return headsParam == 1 ? 1 : headsParam == 2 ? 4 : headsParam == 3 ? 12 : moodMode; }

    /** SPREAD as the engine's width. Two loops pan hard at 100 %: the stage loop (ECHO, YESILCAM, TERS) uses a constant-power pan, so a
        hard-panned head reads 3.01 dB lower in the mono sum; and one head, in any voicing, sits wholly in one ear. In both cases the width
        above 0.70 is compressed to reach 0.80 at 100 % (mono sum -0.97 dB, opposite ear -7 dB, power unchanged). Below 0.70, and for the
        classic loop (SLAPBACK .. WORN LOOP) with two or three heads, it is the identity, so every starting point and every recalled MOOD
        stays bit-identical. The dry path never sees the width. */
    static double engineWidth (int mood, int heads, double spread) { return ringmodel::engineWidth (mood, heads, spread); }

    /** The resolved TIME in ms for the current parameters at a tempo. */
    double resolvedMs (double bpm) const;

    juce::AudioProcessorValueTreeState apvts;
    svl::ParamUndo undo { *this };   // Cmd+Z / Cmd+Shift+Z: editor edits only, never host automation (SvlUndo.h)
#if SVL_LICENSING
    svl::License license { yankiLicenseConfig() };   // no licence: audio passes through untouched
#endif

    // Read by the editor (written once per block by the audio thread).
    std::atomic<double> uiBpm { kFallbackBpm }, uiMs { 250.0 };
    std::atomic<bool> uiHostTempo { false }, uiSwelling { false }, uiEndLatched { false }, uiFrozen { false };
    std::atomic<float> uiDryDb { -120.0f }, uiLoopDb { -120.0f }, uiRefDb { -240.0f };
    std::atomic<int> uiBeatCount { 0 };
    std::atomic<bool> uiFeedbackNatural { true };

private:
    void parameterChanged (const juce::String&, float) override;
    void handleAsyncUpdate() override;
    void applyMoodIfChanged();
    void recallMood (int mood);
    yanki::Controls controlsFor (int mood, double bpm, double ms, bool endTrig) const;

    yanki::YankiEngine engine;
    double sampleRate = 44100.0;

    std::atomic<float>* p[20] {};
    juce::AudioParameterBool* bypassParam = nullptr;
    enum P { pMood, pSync, pTime, pFeedback, pCapOn, pCapDb, pTone, pWear, pMix, pOutput,
             pEndMode, pEndTrigger, pFreeze, pBypass, pDarken, pRise, pLock, pFine, pSpread, pHeads };
    float v (P i) const { return p[i]->load(); }

    juce::SmoothedValue<double> msSmooth;
    juce::SmoothedValue<float> toneSmooth, wearSmooth, mixSmooth, spreadSmooth, darkenSmooth, capSmooth, fbSmooth;
    juce::SmoothedValue<float> outGain, effectAmount;
    bool firstBlock = true;

    std::atomic<bool> feedbackNatural { true };
    std::atomic<float> naturalKnob { 0.0f };
    std::atomic<bool> endPulsePending { false };
    std::atomic<bool> restoring { false };
    std::atomic<bool> resetRequested { false };
    std::atomic<int> presetIndex { -1 };
public:
    /** How many times a non-finite sample (host input or engine output) was caught: the loop was reset instead of staying dead. */
    std::atomic<int> nonFiniteEvents { 0 };
private:
    std::atomic<int> appliedMood { 0 };

    // END lamp: replica of the engine's phrase-onset detector (a new phrase re-arms END).
    double onEnv = 0.0, onRelease = 0.0;
    uint64_t onQuiet = 0, gapSamples = 0;
    double beatPhase = 0.0;

    juce::AudioBuffer<float> dry, wet;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (YankiProcessor)
};
