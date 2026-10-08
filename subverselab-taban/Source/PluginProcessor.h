#pragma once

#include <JuceHeader.h>
#include <atomic>
#include "TabanEngine.h"
#include "SvlUndo.h"

class TabanAudioProcessor : public juce::AudioProcessor
{
public:
    TabanAudioProcessor();
    ~TabanAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "TABAN"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    /** For the panel: largest lift of the last block (dB), detector level (dBFS), output sample peak (dBFS). */
    float getLiftDb() const noexcept     { return liftDb.load (std::memory_order_relaxed); }
    float getInputDb() const noexcept    { return inputDb.load (std::memory_order_relaxed); }
    float getOutputDb() const noexcept   { return outputDb.load (std::memory_order_relaxed); }

    juce::AudioProcessorValueTreeState apvts;
    svl::ParamUndo undo { *this };

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

private:
    svl::TabanEngine::Parameters readParams() const noexcept;

    svl::TabanEngine engine;
    std::atomic<float> liftDb   { 0.0f };
    std::atomic<float> inputDb  { -200.0f };
    std::atomic<float> outputDb { -200.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TabanAudioProcessor)
};
