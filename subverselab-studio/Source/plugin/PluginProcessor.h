// JING FANG (code name SVL): the plug-in around svl::Engine. Every engine parameter (svl::paramDefs) is a host parameter by id;
// the state is the preset XML of SvlPreset.h, so the plug-in and `svl render` read and write the same files.
//
// Exact values: the engine is fed the preset's own floats, not the host parameter's round trip through 0..1 (a skewed range
// does not give the same float back). A host parameter that moves (automation, a knob, the host's own editor) replaces its
// exact value. So a preset renders bit for bit as `svl render` renders it (plugin_check).
#pragma once
#include <JuceHeader.h>
#include "SvlEngine.h"
#include "SvlPreset.h"
#include "SvlGen.h"
#include "MpeState.h"
#include "tuning/TuningLearn.h"
#include "tuning/TuningBuiltin.h"

class JingFangProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    JingFangProcessor();
    ~JingFangProcessor() override;

    // ---- AudioProcessor
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "JING FANG"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorValueTreeState apvts;
    bool isStandalone() const noexcept { return wrapperType == wrapperType_Standalone; }

    // ---- presets (message thread)
    struct PresetEntry { juce::String family, name, xml; juce::File file; bool factory = true; };
    const std::vector<PresetEntry>& presets() const noexcept { return presetList; }
    juce::StringArray families() const;                           // in list order
    juce::StringArray categories() const;                         // in list order: Init, Keys, Pads, Leads, Bass, Plucks, Arps, ...
    static juce::String categoryOf (const juce::String& family);  // a family no category lists: "Yours"
    void rescanPresets();
    bool deletePreset (int index);                                // yours: to the bin; factory: hidden for good (hidden.txt)
    void showHiddenPresets();
    static juce::StringArray hiddenPresetNames();
    bool loadPresetIndex (int index);
    bool loadPresetXml (const juce::String& xml, int indexOrMinusOne = -1);
    void stepPreset (int delta);
    int currentPresetIndex() const noexcept { return currentIndex; }
    juce::String currentName() const { return loadedMeta.name; }
    juce::String currentFamily() const { return loadedMeta.family; }
    bool isModified() const;                                      // a value differs from the loaded preset
    juce::File saveUserPreset (const juce::String& family, const juce::String& name);
    static juce::File userPresetFolder();
    static juce::File& userFolderOverrideForTests();               // the checks use a temporary folder, never the user's
    juce::String stateXml() const;                                // the current values as a preset file (+ the loaded preset's base)
    float loadedValue (const juce::String& paramId) const;       // double-click target: the loaded preset's value, asked now

    // ---- preset synthesizer (owner 17 Oct): every panel takes its settings from its own preset slot, e.g. OSC 1 from preset
    // 5, OSC 2 from preset 9, the amp envelope from preset 17. A slot holds an index into presets(); -1 = not set.
    static constexpr int kNumSections = 14;
    static const char* sectionName (int s);                       // "OSC 1" ... "TUNING"
    static int sectionOf (const std::string& paramId);            // -1 for parameters no panel slot covers (LFOs, cables)
    bool loadSection (int section, int presetIndex);              // that panel's values from that preset; other panels stay
    void stepSection (int section, int delta);
    int sectionSlot (int section) const noexcept { return section >= 0 && section < kNumSections ? slots[(size_t) section] : -1; }
    float exactValue (int i) const noexcept { return exactShared[(size_t) i].load(); }

    // ---- the panel's keyboard (message thread); row -1 = as a MIDI key (tuning map applies), 0 / 1 / 2 = low / mid / high row
    void uiNoteOn (int key, int row, float velocity);
    void uiNoteOff (int key, int row);
    void uiAllNotesOff();
    bool keyLit (int row, int key) const noexcept { return lit[(size_t) juce::jlimit (0, 2, row)][(size_t) (key & 127)].load() > 0; }
    /** a voice sounds on this key now (arp / sequencer / chord notes included; owner 18 Oct: "arpejleyici farklı nota
        kombinasyonu çalsa da pad bunu göstermiyor") */
    bool keySounding (int row, int key) const noexcept { return sounding[(size_t) juce::jlimit (0, 2, row)][(size_t) (key & 127)].load (std::memory_order_relaxed); }
    // when a key last started / stopped sounding on any row (ms, Time::getMillisecondCounterHiRes): the display's delay probe
    double keySoundOnMs (int key) const noexcept { return soundOnMs[(size_t) (key & 127)].load (std::memory_order_relaxed); }
    double keySoundOffMs (int key) const noexcept { return soundOffMs[(size_t) (key & 127)].load (std::memory_order_relaxed); }
    static float rowPitch (const svl::ParamSet& ps, int key, int row);   // the pitch an explicit row plays (before the reference)

    // ---- the signature glide: the product is about to write values itself
    bool glideArmed() const noexcept { return juce::Time::getMillisecondCounter() < (juce::uint32) glideUntil.load(); }
    void armGlide (int ms = 600) noexcept { glideUntil = (juce::int64) juce::Time::getMillisecondCounter() + ms; }

    // ---- live view for the panel's screens (written by the audio thread once per block, read at 30 Hz)
    struct Live { float level = 0, pitch = 60, ampStage = 0; int voices = 0, seqStep = -1; };
    float fxReductionDb() const noexcept { return liveGr.load (std::memory_order_relaxed); }   // COMP's gain reduction, for the FX door
    Live live() const noexcept { return { liveLevel.load(), livePitch.load(), (float) liveStage.load(), liveVoices.load(), liveSeqStep.load() }; }
    /** the last kScopeLen output samples (mono), oldest first */
    static constexpr int kScopeLen = 2048;
    void copyScope (float* dest) const noexcept;

    // ---- the drum machine's kit: the preset each of the 8 tracks plays ("Family/Name"); kept in the state and in saved presets
    // as <drumkit t1=".." .../> (the engine reads the same line once its drum machine lands)
    // ---- tempo: the panel's TEMPO (standalone, or a host without one); the host's tempo wins when there is one
    float tempoBpm() const noexcept { const float h = hostBpm.load(); return h > 0 ? h : userBpm.load(); }
    bool tempoFromHost() const noexcept { return hostBpm.load() > 0; }
    float userTempo() const noexcept { return userBpm.load(); }
    void setUserBpm (float bpm) noexcept { if (bpm >= 20.0f && bpm <= 300.0f) userBpm.store (bpm); }
    // the DAW (RU-8's rules): is there a host transport (never in the Standalone), does it play
    bool inHost() const noexcept { return hostHas.load(); }
    bool hostPlaying() const noexcept { return hostPlay.load(); }
    // PLAY in a stopped host previews that unit (0 SEQ, 1 DRUM) on the own clock until the host starts or stops
    void requestPreview (int unit, bool on) noexcept { (unit == 0 ? wantSeqPreview : wantDrumPreview).store (on); previewSerial.fetch_add (1, std::memory_order_release); }
    // a clip of that unit was dragged out: the first note while the host plays switches it to daw = midi
    void armClip (int unit) noexcept { armed[(size_t) juce::jlimit (0, 1, unit)].store (true); }
    void disarmClip (int unit) noexcept { armed[(size_t) juce::jlimit (0, 1, unit)].store (false); }
    bool clipArmed (int unit) const noexcept { return armed[(size_t) juce::jlimit (0, 1, unit)].load(); }

    static constexpr int kDrumTracks = 8;
    int drumStep() const noexcept { return liveDrumStep.load(); }  // -1 until the engine's drum machine runs
    juce::String drumTrackName (int track, bool full = false) const;
    void setDrumTrack (int track, int presetIndex);
    std::vector<int> drumPresetIndices() const;                 // the presets of the "Drums - ..." families
    const PresetEntry& presetEntry (int i) const { return presetList[(size_t) juce::jlimit (0, (int) presetList.size() - 1, i)]; }

    // ---- Tuning Lab (SPEC.md "Tuning Lab"): shipped locked (jflab::Lab::unlocked() is false); the free tuning stays as it is.
    // While the Lab is active it decides every note-on pitch (directional / adaptive / morphed tables); off, nothing changes.
    struct LabState
    {
        bool on = false, directional = true, adaptive = false;
        float morph = 0, window = 20, pull = 0.5f;
        jflab::Tuning a, b;
    };
    static bool labUnlocked() noexcept { return jflab::Lab::unlocked(); }
    bool labActive() const noexcept { return labUnlocked() && labOnA.load(); }
    const LabState& labState() const noexcept { return labMsg; }   // message thread
    void setLabState (const LabState&);                            // message thread: rebuilds the tables, hands them over
    /** a factory preset with <labtuning a="rast"/> loads that built-in tuning into A and turns the Lab on; the next preset
        without one turns it off again, unless the user changed the Lab in between (then it is theirs and stays) */
    bool labFromPreset() const noexcept { return labByPreset; }
    void labTakenByUser() noexcept { labByPreset = false; }
    float labDrift() const noexcept { return labDriftA.load(); }
    int labDirection() const noexcept { return labDirA2.load(); }
    /** keys sounding through the Lab and the pitch each plays (semitones), for the interval view */
    std::vector<std::pair<int, float>> labSounding() const;
    /** learn a tuning from a recording into A (which = 0) or B (1), on a background thread; done(message, ok) on the message thread */
    void labLearn (const juce::File& audio, int which, std::function<void (juce::String, bool)> done);
    static juce::String labStateXml (const LabState&);
    static bool labStateFromXml (const juce::String& xml, LabState&);

    // ---- GENERATE (docs/GENERATE_PLAN.md §8; message thread). Unit 0 = SEQ door, 1 = DRUM door.
    struct GenEntry { int r = 0, n = 0; svl::gen::SeqPattern seq; svl::gen::DrumPattern drum; };
    struct GenUnit { std::vector<GenEntry> hist; int pos = -1; bool lockR = false, lockN = false, live = false; int r = -1, n = -1, skipped = 0; };
    /** the dice: new seeds (a locked one stays), through the repetition filter and MUTATE, written as one undoable gesture per value */
    void genRoll (int unit, int which = 0);   // which: 0 both seeds new, 1 a new rhythm (notes kept), 2 new notes (rhythm kept)
    /** < >: back / forward through the last rolls (the pattern as it was rolled) */
    void genHistory (int unit, int delta);
    void genSetLock (int unit, bool notes, bool on) { auto& u = genUnits[(size_t) juce::jlimit (0, 1, unit)]; (notes ? u.lockN : u.lockR) = on; }
    const GenUnit& genUnit (int unit) const { return genUnits[(size_t) juce::jlimit (0, 1, unit)]; }
    svl::gen::Role genRole() const;                               // what the SEQ door writes for the loaded sound
    bool genMicroAllowed() const;                                 // the keyboard on MICRO and the Lab off
    juce::String genWrites (int unit) const;                      // "MELODY · DORIAN", "BEAT · BREAKBEAT"
    svl::gen::SeqPattern genCurrentSeq() const;                   // the steps as they are now (user edits included)
    void genSeedRandomForTest (juce::int64 s) { genRandom.setSeed (s); }

    // ---- tests
    void runMessageWorkForTest() { timerCallback(); }
    const svl::ParamSet& engineParamsForTest() const noexcept { return engine.getParams(); }
    const svl::Engine& engineForTest() const noexcept { return engine; }
    static const juce::StringArray& paramIds();

private:
    void timerCallback() override;
    void applyHostChanges();                                  // audio thread: pending preset, then moved host parameters
    void setHostValue (int i, float plain);                   // message thread: one host parameter, notifying the host
    void pushPreset (const svl::ParamSet& values, const svl::PresetMeta& meta, const svl::ParamSet& base, int index);
    void bumpLit (int row, int key, int d) noexcept;

    svl::Engine engine;
    std::array<juce::RangedAudioParameter*, svl::kNumParams> hostParam {};
    std::array<std::atomic<float>*, svl::kNumParams> raw {};
    std::array<float, svl::kNumParams> exact {}, lastRaw {};         // audio thread
    std::array<std::atomic<float>, svl::kNumParams> exactShared;       // what the engine plays, for the message thread
    // a preset (or a restored state) waiting for the audio thread
    juce::SpinLock pendingLock;
    svl::ParamSet pendingExact;
    std::array<float, svl::kNumParams> pendingRaw {};
    std::atomic<bool> pendingFlag { false };
    std::atomic<bool> rowsDirty { false };                     // keyswitches moved a row: write it back to the host parameter
    bool prepared = false;

    // panel notes
    struct UiNote { bool on; int key, row; float vel; };
    juce::AbstractFifo uiFifo { 256 };
    std::array<UiNote, 256> uiNotes {};
    std::array<std::array<std::atomic<int>, 128>, 3> lit {};
    std::array<std::array<std::atomic<bool>, 128>, 3> sounding {};   // audio thread writes once per block
    std::array<std::atomic<double>, 128> soundOnMs {}, soundOffMs {};
    void updateSounding() noexcept;
    std::array<int, 128> midiRow {};
    MpeState mpe;                                              // audio thread: the MIDI input's MPE zones (work order 1.x item 8)
    std::array<int, 128> uiLitRow {};                          // the row a panel / computer key was lit on (message thread)                           // the row a sounding MIDI key was lit on

    std::vector<PresetEntry> presetList;
    std::vector<PresetEntry> hiddenList;                       // factory presets the user deleted (still found by drum kits)
    int currentIndex = -1;
    std::array<int, kNumSections> slots;                       // preset index per panel (-1 = not set)
    juce::String slotsXmlLine() const;                         // <slots osc1="Family/Name" .../> for state and saved presets
    void readSlots (const juce::String& xml);
    svl::PresetMeta loadedMeta;
    svl::ParamSet loadedBase;
    std::atomic<juce::int64> glideUntil { 0 };
    std::atomic<float> liveLevel { 0.0f }, livePitch { 60.0f }, liveGr { 0.0f };
    std::atomic<int> liveStage { 0 }, liveVoices { 0 }, liveSeqStep { -1 };   // seq step: set once the engine has a sequencer
    std::array<std::atomic<float>, kScopeLen> scopeBuf {};
    std::atomic<int> scopeWrite { 0 };

    std::array<juce::String, kDrumTracks> drumKit;             // "Family/Name" per track, empty = svl::kDefaultDrumKit
    std::atomic<int> liveDrumStep { -1 };
    std::atomic<float> userBpm { 120.0f }, hostBpm { -1.0f };
    std::atomic<bool> hostHas { false }, hostPlay { false }, sourceToHost { false }, modeToHost { false };
    bool hostNowPlaying = false;                               // audio thread
    std::atomic<bool> wantSeqPreview { false }, wantDrumPreview { false };
    std::atomic<int> previewSerial { 0 };
    int previewSent = 0;                                       // audio thread
    std::array<std::atomic<bool>, 2> armed {};                 // 0 SEQ, 1 DRUM: a clip was dragged out
    void readDrumKit (const svl::Preset&);
    void queueDrumKit();                                       // message thread: resolve the kit's presets, hand them to the audio thread
    std::array<svl::ParamSet, kDrumTracks> pendingKit {};      // under pendingLock
    std::array<float, kDrumTracks> pendingKitGate {};
    std::array<bool, kDrumTracks> pendingKitHas {};
    std::atomic<bool> kitFlag { false };
    jflab::Lab lab;                                            // tables: message thread builds, audio thread reads
    LabState labMsg;                                           // message thread's copy
    bool labByPreset = false;                                  // message thread: the Lab state came from a preset's <labtuning>
    juce::String labPresetId;                                  // that preset's built-in tuning id (written back on SAVE)
    void applyPresetLab (const juce::String& presetXml);
    std::atomic<bool> labOnA { false }, labDirOnA { true }, labAdaptA { false };
    std::atomic<float> labWindowA { 20 }, labPullA { 0.5f }, labDriftA { 0 };
    std::atomic<int> labDirA2 { 1 };
    std::array<std::atomic<float>, 128> labHeld {};            // pitch per sounding key, -1 = silent
    bool labWasActive = false;                                 // audio thread
    int labTableSent = -1;                                     // audio thread: the Lab table version the engine's tuning table holds
    void labNoteOn (int key, float vel, int offset, double timeSec) noexcept;
    double labSr = 48000, labTime = 0;                         // audio thread: running time for the phrase rule
    std::atomic<bool> labCancel { false };                     // the destructor stops a running LEARN
    void applyStateXml (const juce::String& xml);
    std::shared_ptr<int> labAlive = std::make_shared<int> (0);  // learn results check it before touching the processor
    juce::ThreadPool labPool { 1 };

    std::array<GenUnit, 2> genUnits;
    juce::Random genRandom;
    std::array<float, 9> genKnobsSeen {};                      // gen.* values last applied: a moved knob re-writes the same seeds
    svl::gen::Settings genSettings (int unit) const;
    svl::gen::Scale genScale() const;                          // the Lab's degrees, or empty (the generator picks a 12-key scale)
    std::array<svl::gen::Drum, svl::kDrumTracks> genKit() const;
    void genWrite (int unit, const GenEntry&);
    void genFollowKnobs();                                     // timer
    juce::String genXmlLine() const;
    void genReadXml (const juce::String& xml);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (JingFangProcessor)
};
