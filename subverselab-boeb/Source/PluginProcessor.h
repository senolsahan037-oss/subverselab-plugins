#pragma once

#include <JuceHeader.h>
#include <atomic>
#include "BoebEngine.h"
#include "SvlUndo.h"

/*  BOE-B plug-in: the engine (Source/BoebEngine.h) in a JUCE shell.

    LISTEN: the audio thread feeds kListenSec (6 s) of the vocal that has signal (and the beat on the side input) into an analyser;
    a worker thread then diagnoses it, works out the settings and calibrates the exciter, and hands the settings back.
    The first LISTEN of a fresh instance starts by itself with the first audio ("kanala tak, işi bitir", owner 24 Oct).

    NOW: two more analysers take turns on the processed output, kListenSec each, so the panel can show each cause after BOE-B
    (FOUND -> NOW). Nothing on the audio thread allocates or locks: hand-overs are atomics, the published results sit
    behind a SpinLock the audio thread only try-locks. */
class BoebProcessor : public juce::AudioProcessor,
                      private juce::Timer
{
public:
    using Engine    = svl::boeb::BoebEngine;
    using Analyser  = svl::boeb::BoebAnalyser;
    using Diagnosis = svl::boeb::Diagnosis;
    using Settings  = svl::boeb::Settings;

    static constexpr double kListenSec = 6.0;   // 7 Nov: 8 -> 6 (measure.py B4 33/42 at both, B12 7/7 at 6; 5 s drops B4 to 24/42)

    BoebProcessor();
    ~BoebProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "BOE-B"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 2.5; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorValueTreeState apvts;
    svl::ParamUndo undo { *this };
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    //==============================================================================
    /** LISTEN: start (or restart) the kListenSec listen. Message thread. */
    void startListen();
    /** 0 idle, 1 listening, 2 working it out. */
    int listenState() const noexcept { return state.load(); }
    /** Seconds of signal heard in the current listen. */
    float listenHeard() const noexcept { return heard.load(); }
    bool beatOnSide() const noexcept { return beatSeen.load(); }

    struct Readout
    {
        bool haveFound = false, haveNow = false;
        Diagnosis found, now;
        Settings settings;
        int foundSerial = 0, nowSerial = 0;   // change counters, for the panel's typewriter
    };
    /** The panel's copy of FOUND / NOW. Message thread. */
    Readout readout() const;
    /** LISTEN's AIR value (the AIR knob's double-click target). */
    float listenedAirDb() const;

    /** ui_snapshot only: run the worker's job now, on this thread, instead of waiting for the worker. */
    void finishPendingWorkNow();

private:
    void timerCallback() override;
    Engine::Parameters readParams() const noexcept;
    void work();                       // the worker thread's job: diagnose LISTEN and NOW
    void publishSettings (const Settings& s);

    struct Worker : juce::Thread
    {
        explicit Worker (BoebProcessor& o) : juce::Thread ("BOE-B listen"), owner (o) {}
        void run() override { while (! threadShouldExit()) { owner.work(); wait (40); } }
        BoebProcessor& owner;
    };

    Engine engine;
    Analyser listenAn, nowAn[2];
    std::vector<float> listenMono;     // the listened vocal, for the exciter's calibration (allocated in prepare)
    size_t listenMonoPos = 0;
    float listenPeak = 0.0f;           // audio thread: the loudest block the current / last LISTEN heard
    bool relistenArmed = true;         // audio thread: one automatic re-listen after a LISTEN, if much louder audio comes
    double fs = 48000.0;

    std::atomic<int> state { 0 };              // 0 idle, 1 listening (audio thread feeds), 2 worker
    std::atomic<bool> listenRequest { false }; // message thread -> audio thread: reset and start
    std::atomic<bool> autoListen { true };     // a fresh instance listens by itself
    std::atomic<float> heard { 0.0f };
    std::atomic<bool> beatSeen { false };
    std::atomic<int> nowWrite { 0 };           // the NOW analyser the audio thread is feeding
    std::atomic<int> nowPending { -1 };        // a full NOW analyser waiting for the worker

    // Settings for the audio thread: the worker writes 'pending' under the lock and bumps the serial;
    // the audio thread copies them when it gets the lock.
    juce::SpinLock settingsLock;
    Settings pendingSettings, liveSettings;
    std::atomic<int> settingsSerial { 0 };
    int liveSerial = 0;

    mutable juce::SpinLock readLock;
    Readout read;
    std::atomic<bool> airFromListen { false }; // the timer copies LISTEN's AIR into the AIR knob
    float listenAir = 0.0f;

    Worker worker { *this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BoebProcessor)
};
