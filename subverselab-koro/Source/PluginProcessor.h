#pragma once

#include <JuceHeader.h>
#include "SvlUndo.h"
#if SVL_LICENSING
#include "LicenseConfig.h"
#endif
#include "Engine.h"

/*  KORO, Model KR-77: vocal doubler + scale-aware harmony.

    Parameters are 1:1 with the approved face (docs/concepts/koro_A2_front_label7seg.png)
    and the rear plate (koro_rear_plate.png). The engine is cifte::CifteEngine (verbatim).
    Latency is fixed (2386 samples at 44.1 kHz, scaled with the rate) and reported to
    the host; BYPASS is the dry signal delayed by the same latency, so it never jumps.
*/
class KoroProcessor : public juce::AudioProcessor
{
public:
    KoroProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorParameter* getBypassParameter() const override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.2; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    static const juce::StringArray moodNames, keyNames, scaleOverrideNames;

    /** Recall a MOOD: selects it and writes every face / rear control it defines
        (the key override is left alone). Message thread only. */
    void applyMood (int index);

    /** The front DOUBLE knob (0..10) as the engine's doubler mix and voice count. */
    static float doubleToMix (float dbl) { return juce::jlimit (0.0f, 1.0f, dbl / 10.0f) * kMixAtTen; }
    static int doubleToVoices (float dbl) { return dbl <= 3.0f ? 1 : dbl <= 6.5f ? 2 : 3; }
    static constexpr float kMixAtTen = 0.75f;

    /** The engine controls the parameters ask for (targets, unsmoothed). */
    cifte::Controls targetControls() const;
    int latencySamples() const { return latency; }
    /** Harmony voices of the active MOOD (1..3). LEVEL 2/3 and PAN 2/3 act only when this is 3 (CHOIR). */
    int moodHarmonyVoices() const { return juce::jlimit (1, 3, moods[juce::jlimit (0, 5, (int) param ("mood"))].harmonyVoices); }

    juce::AudioProcessorValueTreeState apvts;
    svl::ParamUndo undo { *this };   // Cmd+Z / Cmd+Shift+Z: editor edits only, never host automation (SvlUndo.h)
#if SVL_LICENSING
    svl::License license { koroLicenseConfig() };   // no licence: audio passes through (delayed, latency stays)
#endif

    /** Read-outs for the editor, written once per block. */
    struct Ui
    {
        std::atomic<bool> hasKey { false }, locked { false }, manual { false }, safe { false };
        std::atomic<int> root { 0 }, scale { 0 };
        std::atomic<float> confidence { 0.0f };
        std::atomic<bool> pitchValid { false }, outValid { false };
        std::atomic<float> inCents { 0.0f }, outCents { 0.0f };   // MIDI cents with the tuning offset removed
        std::atomic<float> rmsL { 0.0f }, rmsR { 0.0f };          // output, per block
    } ui;

    std::atomic<int> editorWidth { 1200 };   // the scalable face's last size, kept in the state

private:
    void runEngine (float* l, float* r, int n);
    void updateReadouts (const float* inMono, int n);
    float param (const char* id) const { return apvts.getRawParameterValue (id)->load(); }

    cifte::CifteEngine engine;
    cifte::PitchTracker tracker;      // the display's pitch IN (the engine's own tracker is private)
    int64_t trackerFrames = 0;
    cifte::Controls moods[6];

    juce::AudioParameterBool* bypassParam = nullptr;
    juce::AudioBuffer<float> work, dryDelay, monoIn;
    int dryWrite = 0, latency = 0;
    float bypassMix = 0.0f, bypassStep = 0.0f;
    double sampleRate = 44100.0;

    enum Smooth { sDouble, sWidth, sTone, sHarmony, sColor, sOutput, sLevel1, sLevel2, sLevel3,
                  sPan1, sPan2, sPan3, sGlide, sTiming, kNumSmooth };
    juce::SmoothedValue<float> smooth[kNumSmooth];

    bool lockLatched = false;
    int lockRoot = 0, lockScale = 0; bool lockHas = false; float lockConf = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KoroProcessor)
};
