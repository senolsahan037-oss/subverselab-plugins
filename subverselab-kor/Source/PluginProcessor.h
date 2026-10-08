// KOR processor: parameters and host glue around kor::Engine (Source/KorEngine.h). DEV shell (P2 preview):
// a generic editor stands in for the panel (P4) and there is no licence gate yet (P5).
#pragma once

#include <JuceHeader.h>
#include "KorMultiband.h"

class KorAudioProcessor final : public juce::AudioProcessor
{
public:
    KorAudioProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    /** Host bypass: the input delayed by the plug-in's latency (so nothing shifts in time), crossfaded over 20 ms both
        ways (no click). The engine keeps running underneath so it is warm when the bypass is released. */
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;
    using AudioProcessor::processBlockBypassed;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    kor::MbParams readParams() const;

public:
    float glow (int band) const noexcept { return engine.glowOf (band); }

private:
    void render (juce::AudioBuffer<float>&, bool bypassed) noexcept;

    kor::Multiband engine;
    static constexpr int kChunk = 1024;
    std::vector<float> delayLine[2], dryChunk[2];   // latency-sized ring per channel; one chunk of delayed input
    int delayPos = 0;
    float wet = 1.0f, wetStep = 0.0f;               // 1 = processed, 0 = bypassed; ramps over 20 ms
    bool wetPrimed = false;
    std::atomic<float>* pLow = nullptr;
    std::atomic<float>* pMid = nullptr;
    std::atomic<float>* pHigh = nullptr;
    std::atomic<float>* pMix = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KorAudioProcessor)
};
