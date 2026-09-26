#pragma once

#include <JuceHeader.h>
#if SVL_LICENSING
#include "LicenseConfig.h"
#endif
#include "KasetEngine.h"

class KasetProcessor : public juce::AudioProcessor
{
public:
    KasetProcessor();

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
#if SVL_LICENSING
    svl::License license { kasetLicenseConfig() };   // no licence: audio passes through untouched
#endif

    // Read by the editor.
    std::atomic<float> uiLevel[2] { { 0.0f }, { 0.0f } };   // output RMS, linear
    std::atomic<float> uiAutoGainDb { 0.0f };
    std::atomic<bool> uiRunning { false };                  // signal is passing: the reels turn
    std::atomic<double> uiTapeSeconds { 0.0 };              // drives the counter

private:
    kaset::KasetEngine engine;
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
