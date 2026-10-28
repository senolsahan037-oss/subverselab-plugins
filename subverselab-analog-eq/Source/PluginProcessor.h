// TORNA plug-in shell (P2). Wraps the header-only engines (Ge1100Engine.h: the equaliser, Ge1100Analyser.h: the FL
// spectrum analyser); no DSP law lives here. SPEC §4 is the contract for every parameter and panel behaviour.
#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <vector>
#include "Ge1100Engine.h"
#include "Ge1100Analyser.h"
#include "SvlUndo.h"
#include "Presets.h"

namespace torna
{
constexpr int kBands = 12;
inline const char* const kBandNames[kBands] = { "16Hz", "32Hz", "63Hz", "125Hz", "250Hz", "500Hz", "1kHz", "2kHz", "4kHz", "8kHz", "16kHz", "32kHz" };
inline juce::String sliderId (int channel, int band) { return juce::String (channel == 0 ? "l" : "r") + juce::String (band).paddedLeft ('0', 2); }
constexpr int kMemories = 3;
}

class TornaAudioProcessor : public juce::AudioProcessor
{
public:
    TornaAudioProcessor();
    ~TornaAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;
    using juce::AudioProcessor::processBlockBypassed;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "TORNA"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 3.0; }   // the 16 Hz resonator rings for about 2 s

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // ---- panel actions (message thread). Each is ONE undo step and writes the real parameters through gestures.
    void recallMemory (int m);                  // MEMORY A / B / C press: all 24 sliders
    void storeMemory (int m);                   // MEMORY hold: the current 24 sliders
    void copyBank (int from);                   // COPY L > R (from 0) / R > L (from 1)
    void flatAll();                             // FLAT: every slider to 0 dB
    void setLink (bool on);                     // LINK: on engage the right bank takes the left bank's values
    void applyPreset (int i);                   // PRESET < >: the preset's marks (both banks; they glide), one undo step
    void stepPreset (int dir) { const int n = presetCount(); applyPreset ((presetIdx.load() + dir + n) % n); }
    int presetIndex() const noexcept { return presetIdx.load(); }
    // The list is the factory presets, then the user's own (files in userPresetDir(), shared by every project).
    int presetCount() const { return torna::kNumPresets + (int) userPresets.size(); }
    bool isUserPreset (int i) const { return i >= torna::kNumPresets; }
    juce::String presetName (int i) const;
    /** PRESET > held 1 s: the current 24 sliders become a new user preset, "USER nn" (the next free number), and the loaded
        preset. Returns its name, or an empty string if the file could not be written. Message thread. */
    juce::String saveUserPreset();
    void reloadUserPresets();
    juce::File userPresetDir() const;
    void setUserPresetDirForTests (const juce::File& d) { presetDirOverride = d; reloadUserPresets(); }
    bool hasMemory (int m) const noexcept { return memValid[(size_t) m]; }
    int lastMemory() const noexcept { return lastMem.load(); }
    void clearLastMemory() noexcept { lastMem.store (-1); }
    /** UI signature 2: the value a slider returns to on double-click, asked at click time: the memory last recalled, else
        the preset loaded, else 0 dB. */
    float sliderDefault (int channel, int band) const;

    // ---- read by the panel (audio thread writes)
    int analyserSegments (int column) const noexcept { return segs[(size_t) column].load (std::memory_order_relaxed); }
    float takeOutPeak (int ch) noexcept { return outPeak[(size_t) ch].exchange (0.0f, std::memory_order_relaxed); }
    std::atomic<int> editorScalePct { 75 };   // SIZE 75 / 100 %, kept in the state
    std::atomic<int> capLights { 0 };         // rear plate CAP LIGHTS: 0 ON, 1 DIM, 2 OFF (the panel only), kept in the state

    juce::AudioProcessorValueTreeState apvts;
    svl::ParamUndo undo { *this };
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    /** the marks the engine plays for a channel (LINK: the right channel plays the left bank) */
    std::array<double, torna::kBands> marksFor (int channel) const noexcept;

private:
    void render (juce::AudioBuffer<float>&, bool hostBypassed);
    void putParam (const juce::String& id, float plain);
    std::array<float, 24> currentSliders() const;

    std::array<ge::Channel, 2> eq;
    gefl::Analyser analyser;
    double sampleRate = 48000.0;
    // POWER off / host bypass: the dry signal delayed by the engine's latency, crossfaded over 10 ms
    std::array<std::vector<float>, 2> dryDelay;
    int dryPos = 0;
    float powerG = -1.0f;   // 1 = engine, 0 = dry; < 0: snap to the target on the first block
    std::array<std::vector<float>, 2> scratch;
    float inGain = 1.0f, outGain = 1.0f;   // smoothed per block end to end

    std::array<std::atomic<int>, gefl::kColumns> segs {};
    std::array<std::atomic<float>, 2> outPeak {};
    std::array<std::array<float, 24>, torna::kMemories> mem {};
    std::array<bool, torna::kMemories> memValid {};
    std::atomic<int> lastMem { -1 };
    std::atomic<int> presetIdx { 0 };   // FLAT
    struct UserPreset { juce::String name; std::array<float, 24> marks {}; };
    std::vector<UserPreset> userPresets;
    juce::File presetDirOverride;
    std::array<float, 24> presetMarks (int i) const;

    std::array<std::atomic<float>*, 24> pSl {};
    std::atomic<float> *pPower = nullptr, *pEqIn = nullptr, *pAtt = nullptr, *pRev = nullptr, *pLink = nullptr, *pIn = nullptr, *pOut = nullptr,
                       *pLevel = nullptr, *pDispL = nullptr, *pDispR = nullptr, *pAnaSrc = nullptr, *pLine = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TornaAudioProcessor)
};
