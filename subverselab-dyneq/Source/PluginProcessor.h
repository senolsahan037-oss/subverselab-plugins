// DYNEQ plug-in shell (P3, DEV build). Wraps the header-only engine in DyneqEngine.h; no DSP law lives here.
// DYNEQ is a working name until the owner names the product (plug-in code Dyeq, manufacturer Svlb).
#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <string>
#include <vector>
#include "DyneqEngine.h"
#include "DyneqBake.h"
#include "SvlUndo.h"
#if SVL_LICENSING
#include "LicenseConfig.h"
#endif

// A crossfade whose weight ramps with a smootherstep shape (zero slope at both ends): the 20 ms blends (BYPASS, DELTA, MIX, KEY LISTEN) have no kink, so they
// add nothing above the signal's own band (TASK_P1n fix 2). The same calls as juce::SmoothedValue; a new target restarts the ramp from the current value.
class ShapedRamp
{
public:
    void reset (double sampleRate, double seconds) { len = std::max (1, (int) std::lround (sampleRate * seconds)); pos = len; }
    void setCurrentAndTargetValue (float v) { cur = start = target = v; pos = len; }
    void setTargetValue (float v) { if (v == target) return; start = cur; target = v; pos = 0; }
    float getTargetValue() const noexcept { return target; }
    bool isSmoothing() const noexcept { return pos < len; }
    float getNextValue() noexcept
    {
        if (pos >= len) return cur = target;
        ++pos; const float x = (float) pos / (float) len;
        return cur = start + (target - start) * (x * x * x * (x * (x * 6.0f - 15.0f) + 10.0f));
    }
private:
    float cur = 0.0f, start = 0.0f, target = 0.0f; int len = 960, pos = 960;
};

class DyneqAudioProcessor : public juce::AudioProcessor
{
public:
    DyneqAudioProcessor();
    ~DyneqAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void reset() override;   // a host reset (transport jump, offline render start): the engine and the processor's ramps start cold, as after prepareToPlay
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;
    using juce::AudioProcessor::processBlockBypassed;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "ADALET"; }   // the state tree keeps the tag "DYNEQ" (saved projects)
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    juce::AudioProcessorParameter* getBypassParameter() const override { return bypassParam; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // ---- what the panel shows. Written by the audio thread (relaxed atomics), read by the editor's timer.
    bool  isSidechainConnected() const noexcept { return scConnected.load (std::memory_order_relaxed); }   // the sidechain bus is enabled in the host
    float getCutDb (int band) const noexcept { return cutView[(size_t) band].load (std::memory_order_relaxed); }          // COLLIDE target cut per band, positive = cut
    float getHunterFreqHz (int s) const noexcept { return hunterView[(size_t) s][0].load (std::memory_order_relaxed); }
    float getHunterGainDb (int s) const noexcept { return hunterView[(size_t) s][1].load (std::memory_order_relaxed); }  // negative = cut
    float getHunterQ (int s) const noexcept { return hunterView[(size_t) s][2].load (std::memory_order_relaxed); }
    float getTargetCutDb() const noexcept { return targetView.load (std::memory_order_relaxed); }   // the TARGET bell, positive = cut
    int   getTargetHits() const noexcept { return targetHitsView.load (std::memory_order_relaxed); }       // hits in the fish memory (0 .. 8)
    float getTargetF0Hz() const noexcept { return targetF0View.load (std::memory_order_relaxed); }         // learned fundamental, 0 = none
    /** What TARGET has heard of the chosen sound so far (the panel's LISTENING / LEARNED line): onsets taken, hits that passed the analysis, and the rejections (0 next onset too soon, 1 late window too short, 2 no fundamental, 3 nothing found). */
    struct TargetListen { int onsets = 0, learned = 0, rejected[4] {}; };
    TargetListen getTargetListen() const noexcept
    {
        TargetListen t; t.onsets = targetOnsetsView.load (std::memory_order_relaxed); t.learned = targetLearnedView.load (std::memory_order_relaxed);
        for (int i = 0; i < 4; ++i) t.rejected[i] = targetRejView[(size_t) i].load (std::memory_order_relaxed);
        return t;
    }
    float getTargetCentreHz() const noexcept { return targetCentreView.load (std::memory_order_relaxed); } // learned centre of the part slider 2 points at, 0 = not found
    float getFreeBandGainDb (int i) const noexcept { return freeView[(size_t) i].load (std::memory_order_relaxed); }
    static const float* bandCentresHz();     // the 32 COLLIDE band centres (they only depend on the engine constants)
    std::atomic<int> editorScalePct { 100 }; // P4: the panel's size, 75 / 85 / 100 %, kept in the state
    std::atomic<int> uiMode { 0 };           // P4: the panel's mode, kept in the state: 0..5 = the band selected last, 6 = TARGET, 7 = COLLIDE, 8 = SELF
    std::atomic<int> lastTargetInst { dyneq::kTgtKick };   // P4: what TARGET's lamp turns back on

    // ---- P4 panel: the layers' own numbers, written by the audio thread once per block (relaxed atomics, a torn read is only a drawing glitch).
    static constexpr int kSpecN = 8192;
    struct EdgeView { std::atomic<float> hz { 0.0f }, cutDb { 0.0f }, mix { 0.0f }; std::atomic<int> slope { 1 }; std::atomic<bool> below { true }, run { false }; };
    struct BandLayerView { std::atomic<float> conflict { 0.0f }, self { 0.0f }, target { 0.0f }, net { 0.0f }; };   // the engine's bandConflictDb / bandSelfDb / bandTargetDb / bandNetDb
    float getBellGainDb (int b) const noexcept { return bellView[(size_t) b].load (std::memory_order_relaxed); }       // COLLIDE bell gain, positive = cut
    static const float* bellQs();                                                                                      // the 32 bells' Q
    /** TARGET bell i of the plateau (11 Oct: three bells; 1 is the middle one): frequency, Q, cut (positive). False when it is not cutting. */
    bool  getTargetBell (int i, float& hz, float& q, float& cutDb) const noexcept
    {
        const size_t o = 3 * (size_t) juce::jlimit (0, 2, i);
        hz = tbView[o].load (std::memory_order_relaxed); q = tbView[o + 1].load (std::memory_order_relaxed); cutDb = tbView[o + 2].load (std::memory_order_relaxed); return cutDb > 0.0f;
    }
    bool  getTargetBell (float& hz, float& q, float& cutDb) const noexcept { return getTargetBell (1, hz, q, cutDb); }
    const EdgeView& getEdge (int k) const noexcept { return edgeView[(size_t) k]; }
    const BandLayerView& getBandLayers (int i) const noexcept { return bandLayer[(size_t) i]; }
    float getInDb() const noexcept { return inDbView.load (std::memory_order_relaxed); }
    float getOutDb() const noexcept { return outDbView.load (std::memory_order_relaxed); }
    float getMatchGainDb() const noexcept { return matchDbView.load (std::memory_order_relaxed); }
    /** The newest `n` mono samples of a tap, oldest first (the mono sum): 0 = the plug-in's input (PRE), 1 = its output (POST, after OUTPUT), 2 = the sidechain KEY.
        Returns false before enough audio has run through that tap. */
    enum Tap { kTapPre = 0, kTapPost = 1, kTapKey = 2, kTaps = 3 };
    bool  copyTail (int tap, float* dest, int n) const noexcept;
    bool  copyInputTail (float* dest, int n) const noexcept { return copyTail (kTapPre, dest, n); }
    /** Total samples that went through a tap since prepareToPlay (the editor's analyzer uses it to know that audio is flowing). */
    uint32_t tapCount (int tap) const noexcept { return tapPos[(size_t) tap].load (std::memory_order_acquire); }

    // ---- BAKE (SPEC 1 C). Message thread.
    struct BakeSuggestion
    {
        int count = 0;
        dyneq::BakeBell bells[dyneq::kBakeMaxBells];
        int slot[dyneq::kBakeMaxBells] {};     // free band (0 .. 5) each bell would go into
        int emptyBands = 0;
        double seconds = 0.0;                  // time that was counted
        std::string text[dyneq::kBakeMaxBells];
    };
    BakeSuggestion bakeSuggest() const;        // fit of the published statistics into the bands that are empty right now
    // Writes the suggestion into the empty bands and switches the COLLIDE lamp off (assistOff), as ONE undoable step (undo.undo() restores every
    // parameter bit-exact: the dynamic cut is back). Returns the bells written; 0 writes nothing and leaves the reason in bakeStatus() (25 Oct).
    int  bakeApply();
    void bakeResetStats();                     // clears the statistics (also done when BAKE is armed)
    bool bakeIsCollecting() const noexcept { return bakeActive.load (std::memory_order_relaxed); }   // armed AND the transport runs AND the COLLIDE lamp is on AND a key bus is connected
    double bakeCountedSeconds() const noexcept { return bakeSecondsView.load (std::memory_order_relaxed); }
    /** A free band is EMPTY (BAKE may write into it) when it is off and every one of its parameters is at its default: an OFF band that holds the
        user's settings is never overwritten (25 Oct; until 0.9.0 any OFF band counted). */
    bool bandIsEmpty (int band) const;
    /** What the panel says about BAKE now, in two lines (25 Oct, tester report T5: "BAKE ARM lit up with no explanation of the next step"):
        line1 = the state (and the count), line2 = the next step or the preview; help = the long text of the hover popover. Message thread. */
    struct BakeStatus { juce::String line1, line2, help; int state = 0; };
    enum BakeState { kBakeIdle = 0, kBakeNotListening, kBakeWaiting, kBakeListening, kBakeBaked, kBakeRefused };
    BakeStatus bakeStatus() const;
    bool transportPlaying() const noexcept { return playingView.load (std::memory_order_relaxed); }

    // SPEC 8d: what TARGET has learned from this channel's sound belongs to this instance and is saved with the project (never in a preset).
    // Message thread: true when nothing has been learned yet (also true while a loaded memory waits for the audio thread).
    bool targetMemoryEmpty() const;

    juce::AudioProcessorValueTreeState apvts;
    svl::ParamUndo undo { *this };   // Cmd+Z / Cmd+Shift+Z: editor edits and BAKE APPLY, never host automation (SvlUndo.h)
#if SVL_LICENSING
    svl::License license { dyneqLicenseConfig() };   // no licence: audio passes through untouched (OFF in P3, the seam only)
#endif

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

private:
    dyneq::Params readParams() const noexcept;
    bool sidechainBusEnabled() const;
    // raw parameter pointers (looked up once; the audio thread never searches by name)
    struct BandPtrs { std::atomic<float> *on, *type, *freq, *gain, *q, *slope, *dyn, *thr, *range, *att, *rel, *trig; };
    struct Ptrs
    {
        std::atomic<float> *mode, *depth, *margin, *focusLow, *focusHigh, *speed, *mix, *delta, *bypass, *keyListen, *tiling, *bakeArm, *targetInst, *targetPart, *targetAmount, *targetWidth, *targetSweet, *reverbReady, *targetSlope, *output, *assistOff, *levelMatch;
        BandPtrs band[dyneq::kFreeBands];
    } ptr {};
    void setPlain (const juce::String& id, float plain);
    void processImpl (juce::AudioBuffer<float>&, bool hostBypassed);   // processBlock / processBlockBypassed: the engine runs in both, BYPASS is a 20 ms crossfade (TASK_P1n)

    dyneq::Engine engine;
    // the memory's copy for the message thread (state save / load). The audio thread publishes after the engine's memory changed and takes a pending
    // import before it processes; both with a try-lock, so the audio thread never waits.
    mutable juce::SpinLock memLock;
    std::vector<float> memSnapshot, memPending;
    std::atomic<bool> memImportPending { false };
    uint32_t memPublished = 0;
    bool wasArmed = false, firstBlock = true;
    std::array<std::vector<float>, 2> dry;
    ShapedRamp blendDry, blendWet, listenAmount;   // MIX / DELTA / BYPASS / KEY LISTEN crossfades (20 ms, smootherstep)
    double sampleRateHz = 48000.0;

    std::atomic<bool> scConnected { false };
    std::atomic<bool> bakeResetRequest { false };
    std::atomic<bool> playingView { true };
    // the last APPLY (message thread): what it wrote, or why it wrote nothing; shown by bakeStatus() while it still describes the bands
    struct LastBake { int written = 0; int slot[dyneq::kFreeBands] {}; float freq[dyneq::kFreeBands] {}, gain[dyneq::kFreeBands] {}; juce::String refused; uint32_t atMs = 0; double seconds = 0.0; };
    LastBake lastBake;
    juce::String bakeBellsText (const dyneq::BakeBell* bells, const int* slots, int n, int maxShown) const;
    std::atomic<bool> bakeActive { false };
    std::atomic<double> bakeSecondsView { 0.0 };
    std::array<std::atomic<float>, dyneq::kBands> cutView {}, bakeMeanView {}, bakePctView {};
    std::array<std::array<std::atomic<float>, 3>, dyneq::kHunters> hunterView {};
    std::array<std::atomic<float>, dyneq::kFreeBands> freeView {};
    std::atomic<float> targetView { 0.0f }, targetF0View { 0.0f }, targetCentreView { 0.0f };
    std::array<std::atomic<float>, dyneq::kBands> bellView {};
    std::array<std::atomic<float>, 9> tbView {};   // three bells x (frequency, Q, cut)
    std::array<EdgeView, 2> edgeView;
    std::array<BandLayerView, dyneq::kFreeBands> bandLayer;
    std::atomic<float> inDbView { -120.0f }, outDbView { -120.0f }, matchDbView { 0.0f };
    std::array<std::array<std::atomic<float>, kSpecN>, kTaps> tapRing {};   // the analyzer's three taps: input, output, sidechain key (mono sums)
    std::array<std::atomic<uint32_t>, kTaps> tapPos {};
    int gridCount = 0; double gridIn = 0.0, gridWet = 0.0, a64 = 0.01; float gMatch = 1.0f, matchCur = 1.0f, matchMid = 1.0f, cMatch = 0.001f;   // the loudness grid (64 samples) and the match gain
    double msIn = 0.0, msWet = 0.0;                  // slow mean squares of the plug-in's input and of the processed signal: the level-matched bypass
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> outGain;
    void clearLayerViews() noexcept;
    std::atomic<int> targetHitsView { 0 }, targetOnsetsView { 0 }, targetLearnedView { 0 };
    std::array<std::atomic<int>, 4> targetRejView {};
    juce::AudioProcessorParameter* bypassParam = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DyneqAudioProcessor)
};
