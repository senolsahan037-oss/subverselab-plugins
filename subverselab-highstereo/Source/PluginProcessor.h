#pragma once

#include <JuceHeader.h>
#include <atomic>
#include "HighStereoEngine.h"
#include "SvlUndo.h"
#include "PresetManager.h"

class HighStereoAudioProcessor : public juce::AudioProcessor
{
public:
    static constexpr int kBands = svl::HighStereoEngine::kBands;

    HighStereoAudioProcessor();
    ~HighStereoAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "HIGH STEREO"; }
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

    /** LISTEN is monitoring only: not a parameter, not saved, OFF on load. */
    void setListen (svl::HighStereoEngine::Listen l) noexcept { listen.store ((int) l, std::memory_order_relaxed); }
    svl::HighStereoEngine::Listen getListen() const noexcept { return (svl::HighStereoEngine::Listen) listen.load (std::memory_order_relaxed); }

    struct BandView { std::atomic<float> corr { 1.0f }, lossDb { 0.0f }, fixDb { 0.0f }, levelDb { -120.0f };
                      std::atomic<bool> overflow { false };
                      // SPEC 7 readouts for the WIDTH field (live fill, held peak, LIMIT notch, loss in the LISTEN mode)
                      std::atomic<float> liveWidth { 0.0f }, liveLevelDb { -120.0f }, liveLossDb { 0.0f }, liveBalance { 0.0f },
                                         limit { 2.0f }, peakFill { 0.0f };
                      std::atomic<bool> noSide { true }, learned { false }; };
    /** Sample rings for the panel (audio thread writes, UI reads the latest frames; a torn read only costs a pixel).
        pre = before STAGE (WIDTH screen FFT), post = the output (STAGE goniometer). */
    struct SampleRing
    {
        static constexpr int N = 16384;
        std::vector<float> l = std::vector<float> (N), r = std::vector<float> (N);
        std::atomic<int> w { 0 };
        void push (const float* a, const float* b, int n) noexcept
        {
            int i = w.load (std::memory_order_relaxed);
            for (int k = 0; k < n; ++k) { l[(size_t) i] = a[k]; r[(size_t) i] = b[k]; i = (i + 1) & (N - 1); }
            w.store (i, std::memory_order_release);
        }
        /** Copies the last `count` frames (oldest first). */
        void latest (float* outL, float* outR, int count) const noexcept
        {
            const int end = w.load (std::memory_order_acquire);
            for (int k = 0; k < count; ++k) { const int i = (end - count + k + N) & (N - 1); outL[k] = l[(size_t) i]; outR[k] = r[(size_t) i]; }
        }
        int writePos() const noexcept { return w.load (std::memory_order_acquire); }
    };
    SampleRing ringPre, ringPost;
    double getRate() const noexcept { return rate.load (std::memory_order_relaxed); }

    /** LIMIT peaks: forget them (click on the field). Also done on transport start and on preset load. */
    void requestPeakReset() noexcept { peakResetRequest.store (true, std::memory_order_relaxed); }
    const BandView& bandView (int k) const noexcept { return bands[(size_t) k]; }
    float getCorrelation() const noexcept { return correlation.load (std::memory_order_relaxed); }
    float getMatchDb() const noexcept { return matchDb.load (std::memory_order_relaxed); }
    /** Deepest PEAK GUARD gain reduction (dB, <= 0) since the panel last asked; the panel calls this at 30 Hz. */
    float takeGuardDb() noexcept { return guardDeepest.exchange (0.0f, std::memory_order_relaxed); }

    juce::AudioProcessorValueTreeState apvts;
    svl::ParamUndo undo { *this };
    PresetManager presets { apvts };

    /** Preset load is one undo step. The knob glide on load is the panel's job (UI signature). */
    bool loadPreset (const juce::String& name) { svl::ParamUndo::Group g (undo, "Preset"); requestPeakReset(); return presets.load (name); }

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    static juce::String bandId (int k) { return "band" + juce::String (k + 1); }

private:
    svl::HighStereoEngine::Parameters readParams() const noexcept;

    svl::HighStereoEngine engine;
    std::atomic<int> listen { 0 };
    std::array<BandView, kBands> bands;
    std::atomic<float> correlation { 1.0f }, matchDb { 0.0f }, guardDeepest { 0.0f };
    std::atomic<bool> peakResetRequest { false };
    bool wasPlaying = false;
    std::atomic<double> rate { 48000.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HighStereoAudioProcessor)
};
