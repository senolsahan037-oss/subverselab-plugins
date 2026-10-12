#pragma once

#include <JuceHeader.h>

#include "Analysis.h"
#if SVL_LICENSING
 #include "LicenseConfig.h"
#endif
#include <onnxruntime_cxx_api.h>

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

        /** What the user asked for, chosen after the file is loaded (the web tool's three doors): tempo and key only,
            the vocal against everything behind it, or the four stems. The model runs only for the last two. */
        enum class Operation { Analysis, VocalRemoval, Stems };

        ModelRunner(SubverseSplitterAudioProcessor& processor);
        ~ModelRunner() override;

        void startOperation (const juce::File& audioFile, Operation op);
        void startSeparation (const juce::File& audioFile) { startOperation (audioFile, Operation::Stems); }
        Operation getOperation() const { return operation; }
        juce::File getInputFile() const { return inputFile; }
        /** The run's folder under ~/Music/ELEK (ElekLibrary.h); empty until a separation has written its stems. */
        juce::File getOutputFolder() const { return outputFolder; }
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

        void loadModel();
        
        SubverseSplitterAudioProcessor& owner;
        juce::File inputFile;
        
        // ONNX Runtime variables
        std::unique_ptr<Ort::Env> env;
        std::unique_ptr<Ort::Session> session;
        std::unique_ptr<Ort::MemoryInfo> memoryInfo;

        // Progress and State atomic variables
        juce::Atomic<int> currentState{ static_cast<int>(State::Idle) };
        juce::Atomic<double> progress{ 0.0 };
        juce::Atomic<bool> finishedSuccess{ false };
        juce::String errorMessage;
        AudioAnalysis analysis;
        juce::CriticalSection analysisLock;
        juce::File outputFolder;
        Operation operation { Operation::Stems };

        // Output file handles
        juce::File vocalsFile;
        juce::File drumsFile;
        juce::File bassFile;
        juce::File otherFile;
        juce::File instrumentalFile;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ModelRunner)
    };

    ModelRunner& getModelRunner() { return modelRunner; }
    
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
    std::unique_ptr<juce::AudioFormatReaderSource> readerSources[4];
    std::unique_ptr<juce::AudioTransportSource> transportSources[4];
    juce::MixerAudioSource mixerSource;
    
    std::atomic<bool> isPlaying { false };
    std::atomic<bool> stemMutes[4] { false, false, false, false };
    std::atomic<bool> stemSolos[4] { false, false, false, false };
    std::atomic<float> stemVolumes[4] { 1.0f, 1.0f, 1.0f, 1.0f };

    /** Recomputes every transport's gain from mute, solo and level together.

        Mute and solo used to set the gain directly, each from its own copy of
        the rule, and `setStemSolo` reached its half by calling `setStemMute`
        with the value it already had. Adding a third input to that would have
        meant a third copy. There is one rule and it lives here. */
    void applyGains();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SubverseSplitterAudioProcessor)
};
