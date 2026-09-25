#pragma once

#include <JuceHeader.h>
#include "LicenseConfig.h"
#include "ReverbEngine.h"

class KubbeProcessor : public juce::AudioProcessor
{
public:
    KubbeProcessor();

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
    double getTailLengthSeconds() const override { return 20.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    static const juce::StringArray placeNames, placeLines, noteNames;
    static double noteBeats (int index);   // pre-delay sync; 0 = off

    /** The engine controls the parameters ask for right now, at a given tempo. */
    svrb::Controls controlsFor (double bpm) const;

    juce::AudioProcessorValueTreeState apvts;
    svl::License license { kubbeLicenseConfig() };   // no licence: audio passes through untouched

    std::atomic<float> uiDuck { 0.0f };    // how far the wet is ducked, 0..1
    std::atomic<double> uiBpm { 120.0 };

private:
    svrb::ReverbEngine engines[svrb::kNumCharacters];
    int current = 0, fading = -1;
    float fadeGain = 0.0f, fadeStep = 0.0f;

    juce::AudioBuffer<float> dry, wet, fadeWet, silence;
    juce::SmoothedValue<float> mixSmooth;
    float duckEnv = 0.0f, duckAttack = 0.0f, duckRelease = 0.0f;
    double sampleRate = 44100.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KubbeProcessor)
};
