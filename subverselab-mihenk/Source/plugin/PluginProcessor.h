#pragma once

#include <JuceHeader.h>
#include "../engine/RtChain.h"
#include "../engine/MeterFifo.h"
#include "../app/Session.h"
#include "Params.h"
#include "Capture.h"

/*  MIHENK as one AudioProcessor, built as the standalone app, VST3 and AU from the same code.

    In a DAW (VST3 / AU) the host's audio runs through RtChain in real time. Parameters are automatable and saved
    with the project. What needs the whole song (AUTO thresholds, TARGET LOCK, the diagnosis) comes from LEARN:
    press it, play the song once; MIHENK keeps what it heard, analyses it offline exactly as the app does, and hands
    the solved values to the real-time chain. The results stay in the project until the next LEARN; while the
    captured audio is still in memory every settings change re-solves as in the app.

    As the standalone app it keeps the offline file workflow: open a file, analyse, render; the player plays the
    rendered master (the render cache) and feeds the same meters.
*/
namespace mihenk
{
class MihenkProcessor : public juce::AudioProcessor,
                        private juce::AudioProcessorValueTreeState::Listener,
                        private juce::Timer
{
public:
    MihenkProcessor();
    ~MihenkProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;
    /** The host's own bypass (VST3 Bypass, AU bypass, a DAW's device switch): the input, delayed by the latency the host was
        told, crossfaded with the chain. The chain keeps running on the input, so coming back never plays old audio. */
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlockBypassed;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "MIHENK"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.5; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "MIHENK"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    /** The standalone app (file workflow, player) rather than a plug-in in a host. */
    bool isStandaloneApp() const noexcept { return standalone; }
    void setStandaloneModeForTesting (bool isApp) { standalone = isApp; session.instantBypass = isApp; session.keepProxy = ! isApp; session.undoHostOwnsParams = ! isApp; session.profileSource = isApp ? "app" : "plugin"; }

    Session& getSession() noexcept { return session; }
    MeterFifo& getMeters() noexcept { return meters; }
    RtChain& getChain() noexcept { return rt; }
    params::Bridge& getBridge() noexcept { return *bridge; }
    juce::AudioProcessorValueTreeState& getState() noexcept { return apvts; }

    // LEARN (plug-in)
    enum class Learn { idle = 0, armed, capturing, analysing };
    Learn learnState() const noexcept { return (Learn) learn.load(); }
    /** idle -> armed (capture starts when the host plays); armed / capturing -> finish and analyse. */
    void toggleLearn();
    double learnSeconds() const noexcept { return capture.seconds(); }
    bool isLearned() const noexcept { return planValid; }
    /** The settings changed since the last LEARN and the captured audio is gone (a reloaded project): AUTO values
        and the drive are the learned ones until the next LEARN. */
    bool learnedIsStale() const;
    /** RESET ALL: back to a new instance (message thread). Every parameter and every setting outside the parameters (tone-match
        curve, dynamic-EQ details, flags), the learned analysis and the real-time plan, the saved proxy, the captured audio and a
        LEARN in progress are cleared. Presets, listening notes and the view stay. The standalone app keeps its loaded file. */
    void resetAll();
    /** Tests: stop capturing and analyse now, synchronously. */
    void finishLearnNow();
    bool hostIsPlaying() const noexcept { return hostPlaying.load(); }

    // A / B in the plug-in: A = the input, level-matched to the learned output
    void setListenOriginal (bool a);
    bool isListeningOriginal() const noexcept { return listenOriginal.load(); }
    float originalGain() const noexcept { return listenGain.load(); }
    /** A / B LEVEL-MATCHED (default on): A at the output's loudness; off = raw levels. App and plug-in; saved with the state. */
    void setAbLevelMatched (bool on);
    bool isAbLevelMatched() const noexcept { return abMatched.load(); }
    /** The gain A is played with now (plug-in path; 1 when raw). */
    float effectiveListenGain() const noexcept { return abMatched.load() ? listenGain.load() : 1.0f; }

    /** Standalone: the audio settings dialog (the wrapper owns the device). */
    std::function<void()> showAudioSettings;

    /** The panel's zoom, saved with the state. */
    float uiScale = 0.0f;
    /** A knob drag is open as one automation gesture (the panel begins / ends it); settings changes then carry none. */
    bool dragGesture = false;

    /** Tests: run the plug-in path as the processor does after a render (the plan and solved values), synchronously. */
    void publishNow() { publishBase(); }

private:
    void parameterChanged (const juce::String& id, float value) override;
    void timerCallback() override;
    void applyParamsToSession();
    void onSettingsEdited();
    void onRendered (const RenderResult& r, const MasterSettings& solved);
    void publishBase();
    void finishLearn (bool synchronous);
    void renderStandalone (juce::AudioBuffer<float>& buffer);

    bool standalone = false;
    Session session;
    juce::AudioProcessorValueTreeState apvts;
    std::unique_ptr<params::Bridge> bridge;
    MeterFifo meters;
    RtChain rt;
    Capture capture;

    // Settings for the audio thread: the non-parameter part of the settings and the plan, published by the message thread.
    juce::SpinLock baseLock;
    MasterSettings base;
    RtPlan plan;
    int baseVersion = 0;
    // audio thread copies
    MasterSettings rtBase, rtSettings;
    RtPlan rtPlan;
    int rtVersion = -1;
    std::vector<float> lastCurve;

    // Host bypass (audio thread): the dry input delayed like the chain, and the crossfade between the two.
    struct HostDry
    {
        std::vector<float> ring[2];
        int mask = 0, write = 0;
        float mix = 0.0f;                  // 0 = the chain, 1 = the delayed input
        void prepare (int size) { int s = 1; while (s < size) s <<= 1; for (auto& r : ring) r.assign ((size_t) s, 0.0f); mask = s - 1; write = 0; mix = 0.0f; }
        void clear() { for (auto& r : ring) std::fill (r.begin(), r.end(), 0.0f); write = 0; }
    };
    HostDry hostDry;
    bool hostBypassNow = false;            // set by processBlockBypassed for the duration of its processBlock call

    // What a reopened project can solve the drive from (the captured audio itself is not kept): see Proxy.h.
    LoudnessProxy proxy;
    MasterSettings proxyBase;              // the settings the proxy's baseline render used
    bool resolveWithoutAudio();

    bool planValid = false;
    juce::String planFingerprint;
    bool applyingParams = false;
    std::atomic<bool> paramsDirty { false }, stopRequested { false }, hostPlaying { false }, listenOriginal { false }, abMatched { true };
    std::atomic<float> listenGain { 1.0f };
    std::atomic<int> learn { 0 };
    bool learnSynchronous = false;

    // Standalone playback: the player at the device rate.
    struct DeviceStage;
    std::unique_ptr<DeviceStage> stage;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MihenkProcessor)
};
} // namespace mihenk
