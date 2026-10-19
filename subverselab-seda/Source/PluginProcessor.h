// SEDA plug-in shell (P3, DEV build). Wraps the header-only engine in SedaEngine.h; no DSP law lives here.
// SEDA is a working name until the owner names the product (plug-in code Seda, manufacturer Svlb).
#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <vector>
#include "SedaEngine.h"
#include "Programs.h"
#define SEDA_HAS_PROGRAMS 1   // applyProgram(): a program is written into the real parameters (the test rig keys on this)
#include "SvlUndo.h"
#if SVL_LICENSING
#include "LicenseConfig.h"
#endif

/** What the panel shows of the LISTEN layer: copied from the engine at the end of every block (audio thread writes, the editor's timer reads). */
struct SedaReadouts
{
    float  duckDb = 0.0f;          // <= 0: current reduction of the repeats
    bool   throwOn = false;
    float  throwAmount = 0.0f;     // 0 .. 1
    int    chosenNote = 11;        // note table index in force (NOTE AUTO: the engine's choice)
    bool   guardOn = false;
    float  guardDb = 0.0f;         // <= 0: feedback pulled down by the guard
    float  thresholdDb = -45.0f;
    float  onsetRate = 0.0f;
    float  loopGain = 0.3f;        // effective loop gain (FEEDBACK, guard, throw, RUNAWAY); > 1 = the loop runs away (the RUNAWAY LED)
    int    chaosSteps = 0;         // CHAOS pitch steps since the last reset
    bool   published = false;      // P4: false until the audio thread has run one block (the effective values below are then the engine's, not a guess)
};

class SedaAudioProcessor : public juce::AudioProcessor
{
public:
    SedaAudioProcessor();
    ~SedaAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;
    using juce::AudioProcessor::processBlockBypassed;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "SEDA"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    /** How long the repeats keep ringing after the input stops, from the CURRENT settings: the time for the loop to fall kTailDb, capped at
        kTailCapSeconds; infinity when FEEDBACK is 100 % or more (the loop does not decay). */
    double getTailLengthSeconds() const override;
    juce::AudioProcessorParameter* getBypassParameter() const override { return bypassParam; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // ---- read by the editor (message thread), written by the audio thread
    SedaReadouts getReadouts() const noexcept;
    float getHostBpm() const noexcept { return hostBpm.load (std::memory_order_relaxed); }   // 0: the host gave no tempo (the engine then uses 120)

    /** Selects program 0..4 (MANUAL, VOCAL, DRUMS, BASS, KEYS): writes SOURCE and, for 1..4, the program's table (Programs.h) into the real parameters as ONE undo
        step, through parameter gestures so the host sees them. 0 writes only SOURCE. Message thread. After this the knobs are the truth: nothing overrides them. */
    void applyProgram (int program);
    /** UI signature 2: the value a knob returns to on double-click, asked at click time: the loaded program's cell for that control when the
        program writes it (Programs.h), otherwise the parameter's factory default. Plain units. */
    float knobDefault (const juce::String& id) const;

    /** P4: level meters of the panel. Linear peak of the block(s) since the last call (the audio thread raises it, the panel's 30 Hz timer takes it). */
    float takeInPeak() noexcept  { return inPeak.exchange (0.0f, std::memory_order_relaxed); }
    float takeOutPeak() noexcept { return outPeak.exchange (0.0f, std::memory_order_relaxed); }
    /** P4b: peak of the wet signal after the duck and the throw (linear, before MIX) since the last call: the ECHO needle of the panel. */
    float takeWetPeak() noexcept { return wetPeak.exchange (0.0f, std::memory_order_relaxed); }
    std::atomic<int> editorScalePct { 75 };   // P4b: the panel's size, 60 / 75 / 100 %, kept in the state (default 75 % = 1200 x 480)

    static constexpr double kTailCapSeconds = 60.0, kTailDb = 66.0;

    juce::AudioProcessorValueTreeState apvts;
    svl::ParamUndo undo { *this };   // Cmd+Z / Cmd+Shift+Z: editor edits only, never host automation (SvlUndo.h)
#if SVL_LICENSING
    svl::License license { sedaLicenseConfig() };   // no licence: audio passes through untouched (OFF in P3, the seam only)
#endif

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

private:
    seda::Params readParams (double bpm, int beatsPerBar) const noexcept;
    void render (juce::AudioBuffer<float>&, bool bypassTarget);
    void publish() noexcept;
    void migrateLegacyState (uint32_t touched);

    seda::Engine engine;
    std::array<std::vector<float>, 2> dryScratch, outScratch;   // bypass crossfade
    std::vector<float> monoR;                                    // the engine's right channel on a mono bus
    float bypassG = -1.0f;                                       // 0 = engine, 1 = dry; < 0: not set since prepareToPlay (snaps to the target)
    std::atomic<bool> engineNeedsReset { false };
    double sampleRate = 48000.0;

    std::atomic<float> inPeak { 0.0f }, outPeak { 0.0f }, wetPeak { 0.0f };
    static void raisePeak (std::atomic<float>& a, float v) noexcept { float o = a.load (std::memory_order_relaxed); while (v > o && ! a.compare_exchange_weak (o, v, std::memory_order_relaxed)) {} }
    std::atomic<bool> aPublished { false };

    std::atomic<float> hostBpm { 0.0f };
    std::atomic<double> lastBpm { 120.0 };
    // published read-outs (plain atomics, one per field: the panel needs no lock)
    std::atomic<float> aDuckDb { 0.0f }, aThrowAmt { 0.0f }, aGuardDb { 0.0f }, aThr { -45.0f }, aOnset { 0.0f };
    std::atomic<int> aNote { 11 }, aChaosSteps { 0 };
    std::atomic<float> aLoopGain { 0.3f };
    std::atomic<bool> aThrowOn { false }, aGuardOn { false };

    juce::AudioProcessorParameter* bypassParam = nullptr;
    std::atomic<float> *pVoice = nullptr, *pTime = nullptr, *pSync = nullptr, *pNote = nullptr, *pNoteAuto = nullptr, *pFeedback = nullptr, *pMix = nullptr,
                       *pTone = nullptr, *pRate = nullptr, *pDepth = nullptr, *pShape = nullptr, *pAge = nullptr, *pComp = nullptr, *pStereo = nullptr,
                       *pWidth = nullptr, *pWetHp = nullptr, *pListen = nullptr, *pDuck = nullptr, *pThrow = nullptr, *pSource = nullptr, *pBypass = nullptr,
                       *pRunaway = nullptr, *pChaos = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SedaAudioProcessor)
};
