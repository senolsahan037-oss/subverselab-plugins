#pragma once

#include <JuceHeader.h>
#include "SvlUndo.h"
#if SVL_LICENSING
#include "LicenseConfig.h"
#endif
#include "HavaEngine.h"

/*  HAVA, Model HV-72: top-end air plus a short spring sheen, a colour box.

    Parameters are 1:1 with the panel (docs/concepts/README.md):
      front: air, sheen, output (dB), bypass, masterSafe, mood (0-4)
      rear:  twang, tension, drip, lowCut (Hz)
    A mood recalls AIR, SHEEN, MASTER SAFE and the rear defaults from
    tools/presets.h; OUTPUT is left where the user put it.

    Two engine instances run the two MASTER SAFE modes, so the toggle can
    crossfade (30 ms) instead of clicking, and the reported latency follows it:
    0 with MASTER SAFE off, engine latency(true) with it on (80 @ 48 kHz,
    74 @ 44.1 kHz). BYPASS fades over 20 ms; once it has landed it is the input
    untouched (MASTER SAFE off) or the input delayed by exactly the latency.
*/
class HavaProcessor : public juce::AudioProcessor,
                      private juce::AudioProcessorValueTreeState::Listener,
                      private juce::AsyncUpdater
{
public:
    HavaProcessor();
    ~HavaProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 1.0; }   // spring tank T60 0.63 s
    juce::AudioProcessorParameter* getBypassParameter() const override { return apvts.getParameter ("bypass"); }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    static const juce::StringArray moodNames;
    static constexpr int kNumMoods = 5, kDefaultMood = 1;   // AIR FULL

    /** MASTER SAFE latency at a sample rate: the engine's look-ahead 1.5 ms + 8. */
    static int safeLatencyAt (double sampleRate);
    int safeLatency() const { return safeLatencyAt (sampleRate); }

    /** A MOOD key: select the mood and recall it (again, if it is already selected). */
    void selectMood (int index);
    /** Writes the mood's AIR, SHEEN, MASTER SAFE and rear defaults to the parameters. */
    void applyMood (int index);

    juce::AudioProcessorValueTreeState apvts;
    svl::ParamUndo undo { *this };   // Cmd+Z / Cmd+Shift+Z: editor edits only, never host automation (SvlUndo.h)
#if SVL_LICENSING
    svl::License license { havaLicenseConfig() };   // no licence: audio passes through untouched
#endif

    // Meter taps for the display, written once per block by the audio thread.
    std::atomic<float> uiAirLiftDb { 0.0f };                        // engine airLiftDb()
    std::atomic<float> uiSheenReDb[2] { { -120.0f }, { -120.0f } }; // sheenLevelDb(ch) re the input level
    std::atomic<float> uiAutoGainDb { 0.0f };                       // engine autoGainDb(), MASTER SAFE on
    std::atomic<bool> uiSafeActive { false };                       // the mode the audio is in now

    // Editor window size, kept with the session.
    int editorWidth = 0;

private:
    void parameterChanged (const juce::String& id, float value) override;
    void handleAsyncUpdate() override;
    yay::Controls targetControls() const;

    struct Chunk
    {
        int start = 0, length = 0;
        yay::Controls controls;
    };

    yay::YayEngine engines[2];   // [0] MASTER SAFE off, [1] MASTER SAFE on
    juce::AudioBuffer<float> input, work[2];
    std::vector<Chunk> chunks;

    static constexpr int kRing = 1024;   // dry delay for BYPASS with MASTER SAFE on (latency <= 296 @ 192 kHz)
    float ring[2][kRing] {};
    int ringPos = 0;

    juce::SmoothedValue<float> smAir, smSheen, smOutput, smTwang, smTension, smDrip, smLowCut;

    double sampleRate = 44100.0;
    bool safeActive = false;
    int fadeFrom = -1, fadePos = 0, fadeLength = 1;   // MASTER SAFE crossfade
    float bypassGain = 0.0f, bypassStep = 0.001f;
    float inputRefDb = -120.0f;
    std::atomic<bool> suppressMoodRecall { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HavaProcessor)
};
