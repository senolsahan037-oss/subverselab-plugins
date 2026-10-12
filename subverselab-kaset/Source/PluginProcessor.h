#pragma once

#include <JuceHeader.h>
#include "SvlUndo.h"
#if SVL_LICENSING
#include "LicenseConfig.h"
#endif
#include "KasetEngine.h"

class KasetProcessor : public juce::AudioProcessor,
                       private juce::AudioProcessorValueTreeState::Listener,
                       private juce::AsyncUpdater
{
public:
    KasetProcessor();
    ~KasetProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.05; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    static const juce::StringArray generationNames, tapeNames;

    kaset::Controls controls() const;

    juce::AudioProcessorValueTreeState apvts;
    svl::ParamUndo undo { *this };   // Cmd+Z / Cmd+Shift+Z: editor edits only, never host automation (SvlUndo.h)
#if SVL_LICENSING
    svl::License license { kasetLicenseConfig() };   // no licence: audio passes through untouched
#endif

    // Read by the editor.
    std::atomic<float> uiLevel[2] { { 0.0f }, { 0.0f } };   // output RMS, linear
    std::atomic<float> uiAutoGainDb { 0.0f };
    std::atomic<bool> uiRunning { false };                  // signal is passing: the reels turn
    std::atomic<double> uiTapeSeconds { 0.0 };              // drives the counter

private:
    // TRACK: the latency follows the switch (set from the message thread: directly when we are on it, else through an async update).
    void parameterChanged (const juce::String& id, float newValue) override;
    void handleAsyncUpdate() override;
    std::atomic<int> pendingLatency { 0 };

    kaset::KasetEngine engine;
    // Switching modes, sample-exact so the result does not depend on the host's block size: 10 ms fade out, the engine switches at the end of
    // the fade (inside the block if need be), the dry tap moves to the new latency, silence while the new path fills once, 10 ms fade in.
    // switchPhase 0 = steady (the output is not touched), 1 = fading out, 3 = filling / fading in.
    int switchPhase = 0, fadeLen = 480, fadeLeft = 0, fillLeft = 0, inCount = 0, latNow = 660;
    bool trackApplied = false;      // the mode the engine is in
    bool switchFrom = false;        // the mode it was in when the fade-out began
    bool switchTarget = false;      // the mode it switches to
    bool engineSwitchPending = false;   // the fade ended on the last sample of a block: the engine switches at the start of the next one
    juce::AudioBuffer<float> wet;
    std::vector<float> dryDelay[2];
    int dw = 0;

    juce::SmoothedValue<float> inGain, outGain, mixSmooth;
    double inPower = 0.0, outPower = 0.0, powerCoef = 0.0;
    float autoGain = 1.0f, playGain = 1.0f, playStep = 0.001f;
    double sampleRate = 44100.0;
    int silentBlocks = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KasetProcessor)
};
