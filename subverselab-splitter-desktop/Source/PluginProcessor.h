#pragma once

#include <JuceHeader.h>

#include "Analysis.h"
#if SVL_LICENSING
 #include "LicenseConfig.h"
#endif
#include <onnxruntime_cxx_api.h>

class ElekFloatSpill;   // PluginProcessor.cpp: audio parked in a temporary file during a run

class SubverseSplitterAudioProcessor  : public juce::AudioProcessor
{
public:
    SubverseSplitterAudioProcessor();
    ~SubverseSplitterAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

   #ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
   #endif

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // Asynchronous AI Model Runner Thread
    class ModelRunner : public juce::Thread
    {
    public:
        enum class State {
            Idle,
            SeparatingAudio,
            Finished
        };

        /** What the user asked for, chosen after the file is loaded. No model runs for Analysis. */
        enum class Operation
        {
            Analysis, VocalRemoval, Stems,
            Stems6,        // htdemucs_6s, adds guitar and piano (ELEK 2.0.0)
            DrumsOnly,     // ELEK 2.0.0: a full mix in; only the Drums stem and its four parts out, nothing else is written
            DrumParts      // ELEK 2.0.0: the input already IS a drums file (an ELEK Drums stem or any drums-only file): straight to the four parts
        };

        ModelRunner(SubverseSplitterAudioProcessor& processor);
        ~ModelRunner() override;

        /** regionStartSec / regionEndSec (ELEK 2.0.0): when regionEndSec > regionStartSec only that part of the file is read and
            separated (clamped to the file; -1: the whole file). The files carry the range in their names and elek.json records it.
            existingRunFolder (DrumParts only): the run folder that holds audioFile (its Drums stem); the four parts are written
            into it and elek.json is updated. Empty: the parts go to a new run folder. */
        void startOperation (const juce::File& audioFile, Operation op, double regionStartSec = -1.0, double regionEndSec = -1.0,
                             const juce::File& existingRunFolder = {});
        Operation getOperation() const { return operation; }
        juce::File getInputFile() const { return inputFile; }
        /** The run's folder under ~/Music/ELEK (ElekLibrary.h); empty until a separation has written its stems. */
        juce::File getOutputFolder() const { return outputFolder; }
        /** Stops a run now: asks the thread to exit AND ends the model call it is inside (one call is 5 s or more, and the
            thread only looks at threadShouldExit() between calls). Safe from any thread; nothing happens when idle. */
        void cancel();
        void run() override;      // catches everything runImpl() throws
        void runImpl();

        double getProgress() const { return progress.get(); }
        State getState() const { return static_cast<State>(currentState.get()); }
        bool isRunning() const { return isThreadRunning(); }
        bool isFinishedSuccessfully() const { return finishedSuccess.get(); }
        juce::String getErrorMessage() const { return errorMessage; }

        juce::File getVocalsFile() const { return vocalsFile; }
        juce::File getDrumsFile() const { return drumsFile; }
        juce::File getBassFile() const { return bassFile; }
        juce::File getOtherFile() const { return otherFile; }
        juce::File getGuitarFile() const { return guitarFile; }   // 6-stem runs only
        juce::File getPianoFile() const { return pianoFile; }     // 6-stem runs only
        /** Drum parts (0 kick, 1 snare, 2 cymbals, 3 toms) of the last Drums-only or Split-the-drums run; empty otherwise. */
        juce::File getDrumPartFile (int part) const { return (part >= 0 && part < 4) ? drumPartFiles[part] : juce::File(); }

        /** The mix with the vocal taken out — the backing track.

            Derived as mix minus vocals rather than as the sum of the other
            three stems. Those are different signals: the model's four stems do
            not account for every sample of the input, and summing them drops
            whatever it could not place. Subtracting keeps it, which is what
            someone singing over the result actually wants. */
        juce::File getInstrumentalFile() const { return instrumentalFile; }

        /** Tempo and key for the file most recently separated. Computed on
            the mix rather than on a stem: it is a property of the arrangement,
            and measuring it on the mix means it is ready before separation
            ends rather than after. */
        AudioAnalysis getAnalysis() const { const juce::ScopedLock sl (analysisLock); return analysis; }   // written on the worker thread

    private:

        void loadModel (const char* modelFileName);   // reloads when a different model is asked for
        void releaseModel();                          // session before env; the next run loads again

        /** The DrumSep stage on a Drums stem (Drums only, and Split the drums), read back from its temporary file window by
            window and written part by part. Releases the main model first. Returns false only when the thread was asked to
            stop; when no parts could be made it returns true with manifestFiles empty and drumSplitProblem saying why. */
        bool runDrumSplit (ElekFloatSpill& drums, int numSamples, const juce::File& model,
                           const std::function<juce::File (int part)>& partFile, juce::StringPairArray& manifestFiles);

        /** Guard rails (2026-11-18): false, with errorMessage set, when the system has too little memory for a stage, or
            reaches critical memory pressure between windows. */
        bool memoryAllowsStage (double needMB, const char* stage);
        bool memoryAllowsNextWindow();

        SubverseSplitterAudioProcessor& owner;
        juce::File inputFile;
        
        // ONNX Runtime variables
        std::unique_ptr<Ort::Env> env;
        std::unique_ptr<Ort::Session> session;
        std::unique_ptr<Ort::MemoryInfo> memoryInfo;
        juce::String loadedModelName;
        Ort::RunOptions* activeRun { nullptr };     // the RunOptions of the model calls in progress (guarded by activeRunLock)
        juce::CriticalSection activeRunLock;
        struct ActiveRun;                             // sets and clears activeRun for a scope

        // Progress and State atomic variables
        juce::Atomic<int> currentState{ static_cast<int>(State::Idle) };
        juce::Atomic<double> progress{ 0.0 };
        juce::Atomic<bool> finishedSuccess{ false };
        juce::String errorMessage;
        AudioAnalysis analysis;
        juce::CriticalSection analysisLock;
        juce::File outputFolder;
        Operation operation { Operation::Stems };
        double regionStart { -1.0 }, regionEnd { -1.0 };     // requested range in seconds of the source file; -1: whole file
        juce::File existingRunFolder;
        double splitProgressBase { 0.9 }, splitProgressSpan { 0.09 };   // where runDrumSplit() reports its windows in getProgress()
        juce::String drumSplitProblem;                        // why runDrumSplit() produced no parts (empty: it did); the caller decides if that is an error

        // Output file handles
        juce::File vocalsFile;
        juce::File drumsFile;
        juce::File bassFile;
        juce::File otherFile;
        juce::File guitarFile;
        juce::File pianoFile;
        juce::File instrumentalFile;
        juce::File drumPartFiles[4];

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ModelRunner)
    };

    ModelRunner& getModelRunner() { return modelRunner; }

    /** What this computer is, read once (ELEK 2.0.0: the window shows it, and FAST is offered from 16 GB up). */
    struct Machine { juce::String chip; double ramGB { 0.0 }; int performanceCores { 0 }, cores { 0 }; };
    static Machine machine();
    static constexpr double kFastMinRamGB = 16.0;
    static bool fastAvailable() { return machine().ramGB >= kFastMinRamGB - 0.5; }     // a 16 GB machine reports 16.0
    /** FAST asked for in the window. The engine does not act on it yet (see STATUS.md, 18 Nov: its parallel path needs a
        16 GB machine to be measured on); it is logged with every run. */
    std::atomic<bool> fastMode { false };

    /** Playback channels. 0..5 are the result's main rows (the editor's row order); 6..9 are the drum parts
        (kick, snare, cymbals, toms) split from the Drums row. */
    static constexpr int kMainStems = 6;                       // drums, bass, other, vocals, guitar, piano at most
    static constexpr int kDrumParts = 4;
    static constexpr int kFirstDrumPart = kMainStems;
    static constexpr int kMaxStems = kMainStems + kDrumParts;
    static bool isDrumPartChannel (int index) { return index >= kFirstDrumPart && index < kMaxStems; }

    /** Which main channel holds the Drums stem the parts were split from (-1: none). Set by the editor when it loads a
        result; clearPlayback() resets it. Read by applyGains() for the parts rule there. */
    void setDrumsChannel (int index);

    // Playback Engine Controls
#if SVL_LICENSING
    svl::License license { elekLicenseConfig() };   // separations need it; Key & BPM and earlier results do not
#endif

    /** Set by the desktop app (StandaloneApp.cpp): opens the audio output dialog. Empty inside a DAW. */
    std::function<void()> showAudioSettings;

    /** Stops playback and drops every loaded stem, so a new result never plays on top of the last one. */
    void clearPlayback();
    void loadStemForPlayback(int stemIndex, const juce::File& file);
    void togglePlayPause();
    bool getIsPlaying() const { return isPlaying.load(); }
    /** Playing, but the track has run out (owner 20 Nov: Play stayed on at the end and had to be pressed twice). */
    bool playbackReachedEnd() const
    {
        if (! isPlaying.load() || transportSources[0] == nullptr) return false;
        return transportSources[0]->hasStreamFinished() || transportSources[0]->getCurrentPosition() >= transportSources[0]->getLengthInSeconds() - 0.01;
    }
    void setStemMute(int stemIndex, bool shouldMute);
    bool getStemMute(int stemIndex) const;
    void setStemSolo(int stemIndex, bool shouldSolo);
    bool getStemSolo(int stemIndex) const;
    
    double getPlaybackPosition() const;
    double getTotalLength() const;

    /** Moves every stem to the same point.

        All four transports are driven together rather than through one master
        clock, so a seek has to reach all of them or the mixer drifts apart —
        which is the one thing a stem player must never do. */
    void setPlaybackPosition (double seconds);

    /** Per-stem level, 0 to 1. Independent of mute and solo: turning a stem
        down and muting it are different intentions, and coming back from a
        solo should restore the level you had set, not reset it. */
    void setStemVolume (int stemIndex, float volume);
    float getStemVolume (int stemIndex) const;

private:
    static BusesProperties elekBuses();   // no input bus in the Standalone (PluginProcessor.cpp)
    ModelRunner modelRunner;

    // Playback Engine
    juce::AudioFormatManager formatManager;
    std::unique_ptr<juce::AudioFormatReaderSource> readerSources[kMaxStems];
    std::unique_ptr<juce::AudioTransportSource> transportSources[kMaxStems];
    juce::MixerAudioSource mixerSource;
    
    std::atomic<bool> isPlaying { false };
public:
    /** Diagnostics (20 Nov, owner heard nothing from the preview): the loudest output sample since the last read, and the device. */
    float takeOutputPeak() { return outputPeak.exchange (0.0f); }
    std::function<juce::String()> describeAudioDevice;     // set by the desktop app
private:
    std::atomic<float> outputPeak { 0.0f };
    std::atomic<bool> stemMutes[kMaxStems] {};
    std::atomic<bool> stemSolos[kMaxStems] {};
    std::atomic<float> stemVolumes[kMaxStems] { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
    static_assert (kMaxStems == 10, "stemVolumes lists one 1.0f per channel: a missing one starts that channel silent");
    std::atomic<int> drumsChannel { -1 };

    /** Recomputes every transport's gain from mute, solo and level together (drum parts rule: see applyGains()).

        Mute and solo used to set the gain directly, each from its own copy of
        the rule, and `setStemSolo` reached its half by calling `setStemMute`
        with the value it already had. Adding a third input to that would have
        meant a third copy. There is one rule and it lives here. */
    void applyGains();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SubverseSplitterAudioProcessor)
};
