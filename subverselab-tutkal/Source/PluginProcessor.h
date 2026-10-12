// TUTKAL plug-in shell (phase 3, DEV build). Wraps the header-only engine in TutkalEngine.h; no DSP lives here.
#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <vector>
#include "TutkalEngine.h"
#include "SvlUndo.h"

class TutkalAudioProcessor : public juce::AudioProcessor
{
public:
    TutkalAudioProcessor();
    ~TutkalAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;
    using juce::AudioProcessor::processBlockBypassed;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "TUTKAL"; }
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

    /** The largest gain reduction (dB, >= 0) the engine reported in the last block. Real: read from the engine's
        per-sample gain reduction output, not derived from levels. */
    float getGainReductionDb() const noexcept { return grMaxDb.load (std::memory_order_relaxed); }

    /** What the panel shows besides the gain reduction. Written by the audio thread, read by the editor's timer. */
    float getHostBpm() const noexcept { return hostBpm.load (std::memory_order_relaxed); }                 // 0: the host gave no tempo (the engine then uses 120)
    bool  isSidechainConnected() const noexcept { return scConnected.load (std::memory_order_relaxed); }    // the sidechain bus is enabled in the host
    bool  isKeyHeard() const noexcept { return keyHeardFlag.load (std::memory_order_relaxed); }             // signal has arrived on the sidechain bus (KEY AUTO)

    std::atomic<int> editorWidth { 1280 };   // the scalable panel's last width, kept in the state

    juce::AudioProcessorValueTreeState apvts;
    svl::ParamUndo undo { *this };   // Cmd+Z / Cmd+Shift+Z: editor edits only, never host automation (SvlUndo.h)

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

private:
    tutkal::Params readParams (bool sidechainEnabled, bool keyHeard, double bpm) const noexcept;
    bool sidechainBusEnabled() const;

    tutkal::Engine engine;
    std::vector<float> grScratch;
    std::array<std::vector<float>, 2> bypassLine;
    int bypassPos = 0;
    bool engineNeedsReset = false;
    std::atomic<float> grMaxDb { 0.0f };
    std::atomic<float> hostBpm { 0.0f };
    std::atomic<bool> scConnected { false };
    std::atomic<bool> keyHeardFlag { false };
    bool keyHeard = false;   // audio thread: set when the sidechain carries signal; cleared after kKeyLetGoS of silence, when the bus goes away, on prepare
    int64_t keySilent = 0;
    static constexpr double kKeyLetGoS = 4.0;

    std::atomic<float>* pVoicing = nullptr; std::atomic<float>* pThreshold = nullptr; std::atomic<float>* pRatio = nullptr;
    std::atomic<float>* pAttack = nullptr;  std::atomic<float>* pRelease = nullptr;   std::atomic<float>* pAuto = nullptr;
    std::atomic<float>* pMakeup = nullptr;  std::atomic<float>* pMix = nullptr;       std::atomic<float>* pScHpf = nullptr;
    std::atomic<float>* pExt = nullptr;     std::atomic<float>* pDrive = nullptr;     std::atomic<float>* pIn = nullptr;
    std::atomic<float>* pPeakRed = nullptr; std::atomic<float>* pLimit = nullptr;     std::atomic<float>* pPumpNote = nullptr;
    std::atomic<float>* pPumpShape = nullptr; std::atomic<float>* pDepth = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TutkalAudioProcessor)
};
