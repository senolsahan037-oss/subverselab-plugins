#pragma once

#include <JuceHeader.h>
#include "KiymaEngine.h"
#include "Presets.h"
#include "SvlUndo.h"

namespace kiymaIds
{
    inline constexpr const char* glitch = "glitch";
    inline constexpr const char* brutal = "brutal";
    inline constexpr const char* offset = "offset";
    inline constexpr const char* envelope = "envelope";
    inline constexpr const char* attack = "attack";
    inline constexpr const char* release = "release";
    inline constexpr const char* mix = "mix";
    inline constexpr const char* crank = "crank";
    inline constexpr const char* output = "output";
}

class KiymaProcessor : public juce::AudioProcessor
{
public:
    KiymaProcessor();
    ~KiymaProcessor() override;

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
    double getTailLengthSeconds() const override { return tailSeconds; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    juce::AudioProcessorValueTreeState apvts;
    svl::ParamUndo undo { *this };   // Cmd+Z / Cmd+Shift+Z: editor edits only, never host automation (SvlUndo.h)

    // ---- the sweet-spot preset bank (not a parameter: it lives in the state tree) ----
    /** -1 = none, 0..4 = LEAN, MINCE, CHUNK, GRISTLE, BONE. Drops to -1 by itself when a preset parameter moves more than 0.01 away. */
    int getActivePreset() const noexcept { return activePreset.load(); }
    /** Writes the preset's values (and CRANK = 0) as ONE undo step, then makes it the active one. */
    void recallPreset (int index);
    void clearPreset() noexcept { activePreset.store (-1); }
    float presetNormalised (int index, int slot) const;   // slot = position in kiyma::kPresetParamIds
    void forceActivePresetForTest (int index) noexcept { activePreset.store (index); }

    // Read by the tests and the editor.
    const kiyma::Engine& getEngine() const noexcept { return engine; }
    std::atomic<float> uiEnvelope { 0.0f };   // the echo's attack/release gain right now, 0..1
    std::atomic<double> uiBpm { 120.0 };
    std::atomic<float> uiInputRms { 0.0f }, uiOutputRms { 0.0f };   // per block, written by the audio thread
    std::atomic<uint32_t> uiBlocks { 0 };                            // counts blocks, so the UI can tell when audio stopped

    static constexpr int kVizSize = 2048, kVizDecimate = 16;
    /** Lock-free: one writer (the audio thread, or the UI itself for a fake signal), any reader. Values are the output
        decimated by kVizDecimate (a box average), newest last. */
    void pushViz (float v) noexcept
    {
        const auto w = vizPos.load (std::memory_order_relaxed);
        viz[w & (kVizSize - 1)].store (v, std::memory_order_relaxed);
        vizPos.store (w + 1, std::memory_order_release);
    }
    void readViz (float* dst, int n) const noexcept
    {
        const auto w = vizPos.load (std::memory_order_acquire);
        for (int i = 0; i < n; ++i)
            dst[i] = viz[(w - (uint32_t) n + (uint32_t) i) & (kVizSize - 1)].load (std::memory_order_relaxed);
    }

private:
    struct PresetWatch : juce::AudioProcessorValueTreeState::Listener
    {
        explicit PresetWatch (KiymaProcessor& o) : owner (o) {}
        void parameterChanged (const juce::String& id, float newPlain) override;
        KiymaProcessor& owner;
    };
    PresetWatch presetWatch { *this };
    std::atomic<int> activePreset { -1 };
    std::atomic<bool> recalling { false };
    kiyma::Engine engine;
    std::array<std::atomic<float>, kVizSize> viz {};
    std::atomic<uint32_t> vizPos { 0 };
    float vizAcc = 0.0f;
    int vizCount = 0;
    double tailSeconds = 6.0;
    std::atomic<float>* pGlitch = nullptr; std::atomic<float>* pBrutal = nullptr; std::atomic<float>* pOffset = nullptr;
    std::atomic<float>* pEnvelope = nullptr; std::atomic<float>* pAttack = nullptr; std::atomic<float>* pRelease = nullptr;
    std::atomic<float>* pMix = nullptr; std::atomic<float>* pCrank = nullptr; std::atomic<float>* pOutput = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KiymaProcessor)
};
