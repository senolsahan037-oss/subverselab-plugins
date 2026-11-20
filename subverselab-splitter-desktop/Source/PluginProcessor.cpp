#include "PluginProcessor.h"

#include "OverlapAdd.h"

// Defined further down, used from the audio callbacks above it.
void logMessage (const juce::String& message);

#if JUCE_MAC || JUCE_IOS
 #include <sys/sysctl.h>
#endif

#if JUCE_WINDOWS
 // onnxruntime.dll is delay-loaded (CMakeLists.txt, /DELAYLOAD) and loaded here from ELEK's own folder by full path. A DAW loads
 // a VST3 with the DAW's folder and System32 on the DLL search path, never the plug-in's folder, and Windows 11 ships an older
 // onnxruntime.dll in System32 (Windows ML): a plain import would either fail to load ELEK.vst3 or bind it to the wrong version.
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <delayimp.h>

static HMODULE elekOrtModule = nullptr;

// Every delay-load of onnxruntime.dll resolves to the copy loaded below, whatever else the host process has loaded by that name.
static FARPROC WINAPI elekDelayHook (unsigned notify, PDelayLoadInfo info)
{
    if (notify == dliNotePreLoadLibrary && info != nullptr && elekOrtModule != nullptr && _stricmp (info->szDll, "onnxruntime.dll") == 0)
        return reinterpret_cast<FARPROC> (elekOrtModule);
    return nullptr;
}
extern "C" const PfnDliHook __pfnDliNotifyHook2 = elekDelayHook;

// Loads ELEK's onnxruntime.dll from the folder of the module that holds this code (ELEK.exe, or the DLL inside
// ELEK.vst3/Contents/x86_64-win), then sets up the ORT C++ API (ORT_API_MANUAL_INIT). Once per process.
static bool elekLoadOrt (juce::String& why)
{
    static juce::String failure;
    static const bool ok = []
    {
        const auto folder = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
        const auto dll = folder.getChildFile ("onnxruntime.dll");
        if (! dll.existsAsFile()) { failure = "onnxruntime.dll not found in " + folder.getFullPathName(); return false; }
        elekOrtModule = LoadLibraryExW (dll.getFullPathName().toWideCharPointer(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (elekOrtModule == nullptr) { failure = "LoadLibraryEx failed (" + juce::String ((int) GetLastError()) + ") for " + dll.getFullPathName(); return false; }
        Ort::InitApi();
        logMessage ("ONNX Runtime loaded from " + dll.getFullPathName());
        return true;
    }();
    why = failure;
    return ok;
}
#endif

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>
#include "PluginEditor.h"
#include "ElekLibrary.h"
#include "DrumSplit.h"
#include "OrtSystemAllocator.h"
#include <cstring>

#if JUCE_MAC
 #include <mach/mach.h>
 #include <malloc/malloc.h>
 #include <sys/resource.h>
#endif

// "rss 812 MB, footprint 790 MB, peak 2301 MB" for the log: where the memory is between the two separation stages.
// footprint is what Activity Monitor shows (phys_footprint); peak is the process' max resident size so far (getrusage).
static juce::String elekMemoryLine()
{
   #if JUCE_MAC
    task_vm_info_data_t vm {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    rusage ru {};
    getrusage (RUSAGE_SELF, &ru);
    const auto mb = [] (double bytes) { return juce::String (juce::roundToInt (bytes / 1048576.0)); };
    if (task_info (mach_task_self(), TASK_VM_INFO, (task_info_t) &vm, &count) == KERN_SUCCESS)
        return "rss " + mb ((double) vm.resident_size) + " MB, footprint " + mb ((double) vm.phys_footprint)
             + " MB, peak " + mb ((double) ru.ru_maxrss) + " MB";
   #endif
    return "n/a";
}

// The three places a model can sit (same folder, Mac bundle Resources, build artefacts folder); a missing file returns {}.
static juce::File elekFindModel (const juce::String& modelFileName)
{
    const juce::File currentExe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
    for (const auto& candidate : { currentExe.getParentDirectory().getChildFile (modelFileName),
                                   currentExe.getParentDirectory().getParentDirectory().getChildFile ("Resources").getChildFile (modelFileName),
                                   currentExe.getParentDirectory().getParentDirectory().getParentDirectory().getChildFile (modelFileName) })
        if (candidate.existsAsFile())
            return candidate;
    return {};
}

// Performance cores, not every core (reason at the main model's session options in loadModel()).
static int elekThreadCount()
{
    // TEST ONLY, off by default: ELEK_TEST_THREADS=<n> (thread-scaling measurements for FAST)
    if (const char* v = std::getenv ("ELEK_TEST_THREADS"); v != nullptr && std::atoi (v) > 0) return std::atoi (v);
    int threads = juce::SystemStats::getNumCpus();
   #if JUCE_MAC || JUCE_IOS
    int performanceCores = 0;
    size_t size = sizeof (performanceCores);
    if (sysctlbyname ("hw.perflevel0.logicalcpu", &performanceCores, &size, nullptr, 0) == 0 && performanceCores > 0)
        threads = performanceCores;
   #endif
    return juce::jmax (1, threads);
}

SubverseSplitterAudioProcessor::Machine SubverseSplitterAudioProcessor::machine()
{
    Machine m;
    m.ramGB = (double) juce::SystemStats::getMemorySizeInMegabytes() / 1024.0;
    m.cores = juce::SystemStats::getNumPhysicalCpus();
    m.performanceCores = m.cores;
    m.chip = juce::SystemStats::getCpuModel();
   #if JUCE_MAC || JUCE_IOS
    int p = 0;
    size_t size = sizeof (p);
    if (sysctlbyname ("hw.perflevel0.physicalcpu", &p, &size, nullptr, 0) == 0 && p > 0) m.performanceCores = p;
    char brand[128] = {};
    size = sizeof (brand) - 1;
    if (sysctlbyname ("machdep.cpu.brand_string", brand, &size, nullptr, 0) == 0 && brand[0] != 0) m.chip = juce::String (brand).trim();
   #endif
    return m;
}

// The drum split model (DrumSep, scripts/export_drumsep_onnx.py; the light file is work/drumsep_light.onnx). NOT shipped yet:
// whether it goes into Resources/ is the owner's release decision. Without it Drums only and Split the drums are refused.
static constexpr const char* kDrumSepModelName = "drumsep.onnx";

// TEST ONLY, off by default: a full path to a DrumSep ONNX used instead of the Resources/ lookup, so the engine can be tested
// before the model ships (docs/drumsplit_engine_test). Never set by the product or the installer.
static constexpr const char* kDrumSepTestOverrideEnv = "ELEK_TEST_DRUMSEP_MODEL";

// Hands freed large heap blocks back to the system (macOS keeps them otherwise). Cheap; often frees nothing.
static void elekRelieveMalloc()
{
   #if JUCE_MAC
    malloc_zone_pressure_relief (nullptr, 0);
   #endif
}

// MEMORY BUDGET (measured 2026-11-18 on the 8 GB Mac mini, STATUS.md memory entry): the most a stage adds to the process'
// footprint, with OrtSystemAllocator. They do not depend on the song's length any more (the engine streams to disk).
static constexpr double kMainStage4MB = 3300.0;     // htdemucs.onnx, 4 stems and vocal removal
static constexpr double kMainStage6MB = 3300.0;     // htdemucs_6s.onnx
static constexpr double kDrumStageMB  = 2600.0;     // DrumSep
// The decoded file is held in memory while tempo and key are measured (8 bytes per sample, more while resampling a
// 48 kHz file): above this length that alone would compete with the model for memory, so ELEK refuses the file.
static constexpr double kMaxInputSeconds = 20.0 * 60.0;

struct ElekMemoryState
{
    double availableMB = -1.0;   // what macOS counts as free (memory_pressure's "free percentage" of RAM); -1 unknown
    int pressure = 0;            // 1 normal, 2 warning, 4 critical (macOS levels); 0 unknown
};

static ElekMemoryState elekMemoryState()
{
    ElekMemoryState m;
   #if JUCE_MAC
    int level = 0;
    size_t size = sizeof (level);
    unsigned long long memsize = 0;
    size_t memsizeSize = sizeof (memsize);
    if (sysctlbyname ("kern.memorystatus_level", &level, &size, nullptr, 0) == 0
        && sysctlbyname ("hw.memsize", &memsize, &memsizeSize, nullptr, 0) == 0)
        m.availableMB = (double) memsize / 1048576.0 * (double) level / 100.0;
    int pressure = 0;
    size = sizeof (pressure);
    if (sysctlbyname ("kern.memorystatus_vm_pressure_level", &pressure, &size, nullptr, 0) == 0)
        m.pressure = pressure;
   #elif JUCE_WINDOWS
    MEMORYSTATUSEX status {};
    status.dwLength = sizeof (status);
    if (GlobalMemoryStatusEx (&status))
    {
        m.availableMB = (double) status.ullAvailPhys / 1048576.0;
        m.pressure = status.dwMemoryLoad >= 95 ? 4 : status.dwMemoryLoad >= 85 ? 2 : 1;
    }
   #endif
    // TEST ONLY, off by default: pretend the system is in this state (test_ui_snapshot checks of the refusal paths).
    if (const char* v = std::getenv ("ELEK_TEST_MEMORY_PRESSURE"); v != nullptr && *v != 0) m.pressure = std::atoi (v);
    if (const char* v = std::getenv ("ELEK_TEST_AVAILABLE_MB"); v != nullptr && *v != 0)   m.availableMB = std::atof (v);
    return m;
}

static juce::String elekGB (double mb) { return juce::String (mb / 1024.0, 1) + " GB"; }

// KICK ATTACKS OUT OF THE SNARE (owner 18 Nov: "snare'in içine yoğun şekilde kick sızıyor"; prototype approved by ear, ported
// 20 Nov). DrumSep puts the kick's attack (everything above its low body) into the snare part. At each kick hit, a snare-part burst
// with no tail is that attack: energy 45-140 ms after the onset under 5 % of the first 30 ms (measured 18 Nov on the owner's track:
// 192 such bursts at kick hits, median ratio 0.0021, max 0.0064; real snares 0.0459 or more). That burst, from 5 ms before the
// onset to 48 ms after it with 5 ms raised-cosine edges, moves from the snare part to the kick part, so kick + snare is unchanged.
// Rewrites both files; returns the number of hits moved, -1 when a file could not be read or written.
static int elekMoveKickAttacks (const juce::File& kickFile, const juce::File& snareFile)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    auto load = [&] (const juce::File& f, juce::AudioBuffer<float>& b, double& sr)
    {
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
        if (r == nullptr || r->numChannels < 1) return false;
        b.setSize (2, (int) r->lengthInSamples);
        r->read (&b, 0, (int) r->lengthInSamples, 0, true, true);
        if (r->numChannels == 1) b.copyFrom (1, 0, b, 0, 0, b.getNumSamples());
        sr = r->sampleRate;
        return true;
    };
    juce::AudioBuffer<float> kick, snare;
    double sr = 44100.0, sr2 = 44100.0;
    if (! load (kickFile, kick, sr) || ! load (snareFile, snare, sr2) || kick.getNumSamples() != snare.getNumSamples()) return -1;
    const int n = kick.getNumSamples();
    const int hop = juce::jmax (1, (int) (sr * 0.0025));                    // 2.5 ms envelope frames
    const int frames = n / hop;
    std::vector<float> env ((size_t) frames, 0.0f);
    float top = 0.0f;
    for (int f = 0; f < frames; ++f)
    {
        double e = 0.0;
        for (int i = f * hop; i < (f + 1) * hop; ++i) { const float m = 0.5f * (kick.getSample (0, i) + kick.getSample (1, i)); e += (double) m * m; }
        env[(size_t) f] = (float) std::sqrt (e / hop);
        top = juce::jmax (top, env[(size_t) f]);
    }
    auto energy = [&] (int from, int to)                                       // snare part, both channels
    {
        double e = 0.0;
        for (int i = juce::jmax (0, from); i < juce::jmin (n, to); ++i)
            e += (double) snare.getSample (0, i) * snare.getSample (0, i) + (double) snare.getSample (1, i) * snare.getSample (1, i);
        return e;
    };
    const auto ms = [sr] (double v) { return (int) std::lround (v * sr / 1000.0); };
    int moved = 0, lastOnset = -ms (1000.0);
    for (int f = 4; f < frames && top > 0.0f; ++f)
    {
        const float before = 0.25f * (env[(size_t) f - 1] + env[(size_t) f - 2] + env[(size_t) f - 3] + env[(size_t) f - 4]);
        if (! (env[(size_t) f] > 2.0f * before && env[(size_t) f] > 0.1f * top)) continue;
        const int onset = f * hop;
        if (onset - lastOnset < ms (60.0)) continue;
        lastOnset = onset;
        const double attack = energy (onset, onset + ms (30.0)), tail = energy (onset + ms (45.0), onset + ms (140.0));
        if (attack <= 0.0 || tail / attack >= 0.05) continue;                 // a real snare (or nothing) there: leave it
        const int a = onset - ms (5.0), b = onset + ms (48.0), edge = ms (5.0);
        for (int i = juce::jmax (0, a); i < juce::jmin (n, b); ++i)
        {
            float w = 1.0f;
            if (i < a + edge)      w = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * (float) (i - a) / (float) edge);
            else if (i > b - edge) w = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * (float) (b - i) / (float) edge);
            for (int c = 0; c < 2; ++c)
            {
                const float x = w * snare.getSample (c, i);
                kick.addSample (c, i, x);
                snare.addSample (c, i, -x);
            }
        }
        ++moved;
    }
    auto save = [&] (const juce::File& f, const juce::AudioBuffer<float>& b)
    {
        const auto tmp = f.getSiblingFile (f.getFileNameWithoutExtension() + ".tmp.wav");
        tmp.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> os (tmp.createOutputStream());
        if (os == nullptr) return false;
        std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), sr, 2, 24, {}, 0));
        if (w == nullptr) return false;
        os.release();
        const bool ok = w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
        w.reset();
        return ok && tmp.moveFileTo (f);
    };
    if (moved > 0 && (! save (kickFile, kick) || ! save (snareFile, snare))) return -1;
    return moved;
}

// Planar float audio parked in a temporary file, read back by range: the mix during the separation and the Drums stem for
// the drum split, so neither is held in memory for the whole song. Deleted when this goes out of scope.
class ElekFloatSpill
{
public:
    explicit ElekFloatSpill (const juce::String& tag)
        : file (juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ELEK_" + tag + "_" + juce::Uuid().toString() + ".f32")) {}
    // (a Uuid, not getNonexistentChildFile: that only picks a name, and two ELEK instances then appended to one file)

    ~ElekFloatSpill() { out.reset(); in.reset(); file.deleteFile(); }

    const juce::File& getFile() const { return file; }

    bool append (const float* left, const float* right, int numSamples)
    {
        if (out == nullptr)
        {
            out = std::make_unique<juce::FileOutputStream> (file, 1 << 20);
            if (out->failedToOpen()) return false;
        }
        scratch.resize ((size_t) kBlock * 2);
        for (int done = 0; done < numSamples;)
        {
            const int k = juce::jmin (kBlock, numSamples - done);
            for (int i = 0; i < k; ++i) { scratch[(size_t) (2 * i)] = left[done + i]; scratch[(size_t) (2 * i + 1)] = right[done + i]; }
            if (! out->write (scratch.data(), (size_t) k * 2 * sizeof (float))) return false;
            done += k;
        }
        return true;
    }

    bool finishWriting()
    {
        if (out == nullptr) return true;
        out->flush();
        const bool ok = out->getStatus().wasOk();
        out.reset();
        return ok;
    }

    bool read (juce::int64 start, int numSamples, float* left, float* right)
    {
        if (in == nullptr)
        {
            in = std::make_unique<juce::FileInputStream> (file);
            if (in->failedToOpen()) return false;
        }
        if (! in->setPosition (start * 2 * (juce::int64) sizeof (float))) return false;
        scratch.resize ((size_t) kBlock * 2);
        for (int done = 0; done < numSamples;)
        {
            const int k = juce::jmin (kBlock, numSamples - done);
            const int bytes = k * 2 * (int) sizeof (float);
            if (in->read (scratch.data(), bytes) != bytes) return false;
            for (int i = 0; i < k; ++i) { left[done + i] = scratch[(size_t) (2 * i)]; right[done + i] = scratch[(size_t) (2 * i + 1)]; }
            done += k;
        }
        return true;
    }

private:
    static constexpr int kBlock = 1 << 15;
    juce::File file;
    std::unique_ptr<juce::FileOutputStream> out;
    std::unique_ptr<juce::FileInputStream> in;
    std::vector<float> scratch;
};

// The DrumSep model for this run: the test-only override when it is set, otherwise drumsep.onnx where the main models are.
// {} when there is none (the split is then skipped).
static juce::File elekDrumSepModel()
{
    if (const char* overridePath = std::getenv (kDrumSepTestOverrideEnv); overridePath != nullptr && *overridePath != 0)
    {
        const juce::File model (juce::String::fromUTF8 (overridePath));
        logMessage ("Drum split: TEST override " + juce::String (kDrumSepTestOverrideEnv) + " = " + model.getFullPathName());
        return model.existsAsFile() ? model : juce::File();
    }
    return elekFindModel (kDrumSepModelName);
}

// The Standalone app has no audio input: ELEK plays files, and an input bus there only makes JUCE open the microphone and show
// its yellow "Audio input is muted to avoid feedback loop" bar. Inside a DAW the input bus stays (an effect needs one).
juce::AudioProcessor::BusesProperties SubverseSplitterAudioProcessor::elekBuses()
{
    BusesProperties buses;
    if (juce::PluginHostType::getPluginLoadedAs() != juce::AudioProcessor::wrapperType_Standalone)
        buses = buses.withInput ("Input", juce::AudioChannelSet::stereo(), true);
    return buses.withOutput ("Output", juce::AudioChannelSet::stereo(), true);
}

SubverseSplitterAudioProcessor::SubverseSplitterAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
     : AudioProcessor (elekBuses()),
       modelRunner(*this)
#endif
{
    formatManager.registerBasicFormats();
}

SubverseSplitterAudioProcessor::~SubverseSplitterAudioProcessor()
{
}

const juce::String SubverseSplitterAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool SubverseSplitterAudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool SubverseSplitterAudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool SubverseSplitterAudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double SubverseSplitterAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int SubverseSplitterAudioProcessor::getNumPrograms()
{
    return 1;
}

int SubverseSplitterAudioProcessor::getCurrentProgram()
{
    return 0;
}

void SubverseSplitterAudioProcessor::setCurrentProgram (int index)
{
    juce::ignoreUnused(index);
}

const juce::String SubverseSplitterAudioProcessor::getProgramName (int index)
{
    juce::ignoreUnused(index);
    return {};
}

void SubverseSplitterAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
    juce::ignoreUnused(index, newName);
}

void SubverseSplitterAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    logMessage ("prepareToPlay: " + juce::String (sampleRate, 0) + " Hz, block "
                + juce::String (samplesPerBlock) + ", in " + juce::String (getTotalNumInputChannels())
                + " out " + juce::String (getTotalNumOutputChannels()));
    mixerSource.prepareToPlay(static_cast<int>(samplesPerBlock), sampleRate);
    for (int i = 0; i < kMaxStems; ++i)
    {
        if (transportSources[i] != nullptr)
            transportSources[i]->prepareToPlay(static_cast<int>(samplesPerBlock), sampleRate);
    }
}

void SubverseSplitterAudioProcessor::releaseResources()
{
    mixerSource.releaseResources();
    for (int i = 0; i < kMaxStems; ++i)
    {
        if (transportSources[i] != nullptr)
            transportSources[i]->releaseResources();
    }
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool SubverseSplitterAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
  #if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
  #else
    if (layouts.getChannelSet (false, 0) != juce::AudioChannelSet::mono()
     && layouts.getChannelSet (false, 0) != juce::AudioChannelSet::stereo())
        return false;

   #if ! JucePlugin_IsSynth
    // We don't require inputs for this stem splitter plugin!
    // So we don't check if input matches output.
   #endif

    return true;
  #endif
}
#endif

void SubverseSplitterAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused(midiMessages);
    juce::ScopedNoDenormals noDenormals;

    if (isPlaying.load())
    {
        buffer.clear();
        juce::AudioSourceChannelInfo info(&buffer, 0, buffer.getNumSamples());
        mixerSource.getNextAudioBlock(info);
        const float pk = buffer.getMagnitude (0, buffer.getNumSamples());
        if (pk > outputPeak.load (std::memory_order_relaxed)) outputPeak.store (pk, std::memory_order_relaxed);
    }
    else
    {
        // Pass-through processing since stem splitting is offline
        auto totalNumInputChannels  = getTotalNumInputChannels();
        auto totalNumOutputChannels = getTotalNumOutputChannels();
        for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
            buffer.clear (i, 0, buffer.getNumSamples());
    }
}

// Playback Engine Implementation
void SubverseSplitterAudioProcessor::clearPlayback()
{
    isPlaying.store (false);
    mixerSource.removeAllInputs();
    for (int i = 0; i < kMaxStems; ++i)
    {
        // no stop() here: it waits up to a second per transport for an audio callback, and with the mixer already emptied
        // none can reach it (the window froze 6-10 s when a playing result was replaced)
        if (transportSources[i] != nullptr) transportSources[i]->setSource (nullptr);
        transportSources[i].reset();
        readerSources[i].reset();
        // a new result starts with a fresh mixer: no mute or solo left over from the last one, every stem at full level
        stemMutes[i].store (false);
        stemSolos[i].store (false);
        stemVolumes[i].store (1.0f);
    }
    drumsChannel.store (-1);
}

void SubverseSplitterAudioProcessor::setDrumsChannel (int index)
{
    drumsChannel.store (index >= 0 && index < kMainStems ? index : -1);
    applyGains();
}

void SubverseSplitterAudioProcessor::loadStemForPlayback(int stemIndex, const juce::File& file)
{
    // Was `>= 4`: in a 6-stem result the editor's rows 4 and 5 (Piano, Other) were never loaded and played nothing.
    if (stemIndex < 0 || stemIndex >= kMaxStems) return;

    auto* reader = formatManager.createReaderFor(file);
    if (reader != nullptr)
    {
        auto newSource = std::make_unique<juce::AudioFormatReaderSource>(reader, true);
        auto newTransport = std::make_unique<juce::AudioTransportSource>();
        newTransport->setSource(newSource.get(), 0, nullptr, reader->sampleRate);

        // Loading into a channel that is still in the mixer would leave the mixer holding a transport freed below.
        if (transportSources[stemIndex] != nullptr)
        {
            mixerSource.removeInputSource (transportSources[stemIndex].get());
            transportSources[stemIndex]->setSource (nullptr);
        }

        mixerSource.addInputSource(newTransport.get(), false);
        
        readerSources[stemIndex] = std::move(newSource);
        transportSources[stemIndex] = std::move(newTransport);
        applyGains();      // the stem plays at what the mixer shows, not at unity
    }
}

void SubverseSplitterAudioProcessor::togglePlayPause()
{
    bool currentlyPlaying = isPlaying.load();
    if (!currentlyPlaying)
    {
        // Check if we need to rewind
        for (int i = 0; i < kMaxStems; ++i)
        {
            if (transportSources[i] != nullptr && transportSources[i]->hasStreamFinished())
                transportSources[i]->setPosition(0.0);
            
            if (transportSources[i] != nullptr)
                transportSources[i]->start();
        }
    }
    else
    {
        for (int i = 0; i < kMaxStems; ++i)
        {
            if (transportSources[i] != nullptr)
                transportSources[i]->stop();
        }
    }
    isPlaying.store(!currentlyPlaying);
}

void SubverseSplitterAudioProcessor::applyGains()
{
    // Solo, where anything is soloed, decides who is audible; mute decides it
    // otherwise. Either way the level a stem is audible *at* is its own, so a
    // stem turned down to a third stays there through a solo and comes back at
    // a third, rather than snapping to unity.
    //
    // DRUM PARTS RULE. The four parts (kick, snare, cymbals, toms) are split from the Drums stem and add back up to it, so the
    // Drums stem and the parts must never both be audible, or the drums play twice.
    //  - Default ("drums mode"): every part control is at rest (no part muted, none soloed, every part level at full).
    //    The Drums stem plays as before and the parts are silent.
    //  - "Parts mode": as soon as ANY part is muted, soloed or turned down, the parts REPLACE the Drums stem: the Drums
    //    stem is silent and the parts play in its place. Turning the kick off therefore gives "drums without kick".
    //  - In parts mode the Drums row still governs its group: a part plays at (its level x the Drums row level); muting
    //    the Drums row silences every part; soloing the Drums row solos the whole group (minus the parts muted in it).
    //  - A soloed part is audible on its own like any soloed row (solo wins over mute, as for the main rows).
    // Putting every part control back at rest returns to the Drums stem. The audio thread is not involved: this only
    // sets each transport's gain, as before.
    bool anySolo = false;
    for (int i = 0; i < kMaxStems; ++i) anySolo = anySolo || stemSolos[i].load();

    const int drums = drumsChannel.load();
    // owner 20 Nov ("drum split edildikten sonra split edilen kanal sessize geçmiyor"): once a run has its parts, they play in
    // place of the Drums stem, always (was: only after a part was muted, soloed or turned down)
    bool partsMode = false;
    if (drums >= 0)
        for (int i = kFirstDrumPart; i < kMaxStems; ++i)
            if (transportSources[i] != nullptr)
                partsMode = true;

    for (int i = 0; i < kMaxStems; ++i)
    {
        if (transportSources[i] == nullptr)
            continue;

        float gain = 0.0f;
        if (isDrumPartChannel (i))
        {
            if (partsMode)
            {
                const bool audible = anySolo ? (stemSolos[i].load() || (stemSolos[drums].load() && ! stemMutes[i].load()))
                                             : (! stemMutes[i].load() && ! stemMutes[drums].load());
                gain = audible ? stemVolumes[i].load() * stemVolumes[drums].load() : 0.0f;
            }
        }
        else if (! (partsMode && i == drums))
        {
            const bool audible = anySolo ? stemSolos[i].load() : ! stemMutes[i].load();
            gain = audible ? stemVolumes[i].load() : 0.0f;
        }
        transportSources[i]->setGain (gain);
    }
}

void SubverseSplitterAudioProcessor::setStemMute(int stemIndex, bool shouldMute)
{
    if (stemIndex >= 0 && stemIndex < kMaxStems)
    {
        stemMutes[stemIndex] = shouldMute;
        applyGains();
    }
}

void SubverseSplitterAudioProcessor::setStemVolume (int stemIndex, float volume)
{
    if (stemIndex >= 0 && stemIndex < kMaxStems)
    {
        stemVolumes[stemIndex] = juce::jlimit (0.0f, 1.0f, volume);
        applyGains();
    }
}

float SubverseSplitterAudioProcessor::getStemVolume (int stemIndex) const
{
    return (stemIndex >= 0 && stemIndex < kMaxStems) ? stemVolumes[stemIndex].load() : 1.0f;
}

void SubverseSplitterAudioProcessor::setPlaybackPosition (double seconds)
{
    const double length = getTotalLength();
    const double target = juce::jlimit (0.0, juce::jmax (0.0, length), seconds);

    for (int i = 0; i < kMaxStems; ++i)
        if (transportSources[i] != nullptr)
            transportSources[i]->setPosition (target);
}

bool SubverseSplitterAudioProcessor::getStemMute(int stemIndex) const
{
    return (stemIndex >= 0 && stemIndex < kMaxStems) ? stemMutes[stemIndex].load() : false;
}

void SubverseSplitterAudioProcessor::setStemSolo(int stemIndex, bool shouldSolo)
{
    if (stemIndex >= 0 && stemIndex < kMaxStems)
    {
        stemSolos[stemIndex] = shouldSolo;
        applyGains();
    }
}

bool SubverseSplitterAudioProcessor::getStemSolo(int stemIndex) const
{
    return (stemIndex >= 0 && stemIndex < kMaxStems) ? stemSolos[stemIndex].load() : false;
}

double SubverseSplitterAudioProcessor::getPlaybackPosition() const
{
    if (transportSources[0] != nullptr)
        return transportSources[0]->getCurrentPosition();
    return 0.0;
}

double SubverseSplitterAudioProcessor::getTotalLength() const
{
    if (transportSources[0] != nullptr)
        return transportSources[0]->getLengthInSeconds();
    return 0.0;
}

bool SubverseSplitterAudioProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* SubverseSplitterAudioProcessor::createEditor()
{
    return new SubverseSplitterAudioProcessorEditor (*this);
}

void SubverseSplitterAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::ignoreUnused(destData);
}

void SubverseSplitterAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    juce::ignoreUnused(data, sizeInBytes);
}

// ==============================================================================
// ModelRunner Implementation (Asynchronous AI Execution)
// ==============================================================================
void logMessage(const juce::String& message)
{
    // Written where the operating system keeps logs, not into one developer's
    // scratch directory. The previous path was absolute and personal, so on
    // every other machine `appendText` had nowhere to write and failed
    // silently — the log existed only where it was least needed, and a user
    // reporting "it does nothing" had nothing to send.
    static const juce::File logFile = []
    {
        auto file = juce::FileLogger::getSystemLogFileFolder()
                        .getChildFile ("SubverseLab")
                        .getChildFile ("ELEK.log");
        file.getParentDirectory().createDirectory();

        // A separation logs a line per window, so this grows. Start fresh once
        // it passes a megabyte rather than accumulating every run ever made.
        if (file.existsAsFile() && file.getSize() > 1024 * 1024)
            file.deleteFile();

        return file;
    }();

    logFile.appendText (juce::Time::getCurrentTime().toISO8601 (true) + "  " + message + "\n");
}

SubverseSplitterAudioProcessor::ModelRunner::ModelRunner(SubverseSplitterAudioProcessor& p)
    : Thread("SubverseSplitterAIWorker"), owner(p)
{
}

// The RunOptions of the model calls of one stage, registered so cancel() can end a call that is in progress.
struct SubverseSplitterAudioProcessor::ModelRunner::ActiveRun
{
    explicit ActiveRun (ModelRunner& r) : runner (r) { const juce::ScopedLock sl (runner.activeRunLock); runner.activeRun = &options; }
    ~ActiveRun() { const juce::ScopedLock sl (runner.activeRunLock); runner.activeRun = nullptr; }
    ModelRunner& runner;
    Ort::RunOptions options;
};

void SubverseSplitterAudioProcessor::ModelRunner::cancel()
{
    signalThreadShouldExit();
    const juce::ScopedLock sl (activeRunLock);
    if (activeRun != nullptr)
        activeRun->SetTerminate();          // the Run in progress throws; run() sees threadShouldExit() and ends quietly
}

SubverseSplitterAudioProcessor::ModelRunner::~ModelRunner()
{
    // Never kill the thread (stopThread with a short timeout did, when one model call outlasted it: the session was then
    // destroyed under a running thread, and the temporary files and the half-written run stayed on disk). End the model
    // call, then wait for the thread to unwind.
    cancel();
    stopThread (-1);
}

void SubverseSplitterAudioProcessor::ModelRunner::releaseModel()
{
    session.reset(); memoryInfo.reset(); env.reset();
    loadedModelName = {};
}

void SubverseSplitterAudioProcessor::ModelRunner::loadModel (const char* modelFileName)
{
    if (session != nullptr && loadedModelName == modelFileName) return; // the asked-for model is already loaded
    if (session != nullptr)                                             // another model was loaded: release it first, session before env
        releaseModel();
    
    logMessage("Loading ONNX Model dynamically from disk...");

   #if JUCE_WINDOWS
    {
        juce::String why;
        if (! elekLoadOrt (why))
        {
            errorMessage = "ELEK's separation engine is missing from this installation. Reinstall ELEK.";
            logMessage ("Error: " + why);
            return;
        }
    }
   #endif

    try {
        Ort::Env envLocal(ORT_LOGGING_LEVEL_WARNING, "Demucs");
        Ort::SessionOptions sessionOptions;
        // Performance cores, not every core.
        //
        // Handing this the full core count is the obvious move and it is
        // slower: measured on an M2, three windows took 17.5 s across all eight
        // threads and 14 s across four. An Apple Silicon machine reports its
        // efficiency cores in the same total, and a parallel matrix op split
        // evenly across cores of two different speeds runs at the pace of the
        // slow ones while the fast ones wait at the barrier. Asking for the
        // performance cores specifically is what the number was meant to be.
        const int threads = elekThreadCount();
        sessionOptions.SetIntraOpNumThreads (juce::jmax (1, threads));
        sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        // MEMORY (2026-11-18): ORT's CPU arena off, the session's memory through OrtSystemAllocator (large blocks returned
        // to the system on free). Measured with this 6-stem model on a 40 s clip: footprint 4.7-5.2 GB with the default
        // arena, 3.4-4.0 GB arena off with malloc, 3.2-3.3 GB at the top and ~1 GB between windows with this allocator,
        // same speed (OrtSystemAllocator.h, STATUS.md).
        {
            std::string why;
            if (! OrtSystemAllocator::useFor (envLocal, sessionOptions, &why))
                logMessage ("Note: ELEK's allocator could not be registered (" + juce::String (why) + "); ORT arena off, plain malloc.");
        }
        
        // Find the model file (Search in multiple locations to support Standalone, VST, and Dev environments):
        // same folder, Mac app bundle Resources, build artefacts folder (elekFindModel)
        juce::File currentExe = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
        juce::File modelFile = elekFindModel (modelFileName);

        // There was a fourth candidate here: an absolute path into one
        // developer's home directory. It is the only reason this ever loaded a
        // model — the three real searches all missed, because the build copied
        // htdemucs.onnx next to the shared library rather than into any bundle.
        // Removed with the build fixed, since a path like that cannot work on
        // anyone else's machine and hid the failure on the one where it did.

        if (!modelFile.existsAsFile())
        {
            // CharPointer_UTF8 because the string carries an em dash and a
            // bare const char* is decoded in the system encoding, which turned
            // it into mojibake the moment it reached a label.
            errorMessage = "ELEK's separation engine is missing from this installation. Reinstall ELEK.";
            logMessage(juce::String("Error: could not find ") + modelFileName + ". Searched: "
                       + currentExe.getParentDirectory().getFullPathName() + ", "
                       + currentExe.getParentDirectory().getParentDirectory()
                                   .getChildFile("Resources").getFullPathName());
            return;
        }
        
        logMessage("Found model at: " + modelFile.getFullPathName());
        
        // ONNX expects a path string (wide string on Windows, std::string on Mac/Linux)
#if JUCE_WINDOWS
        std::wstring modelPathStr = modelFile.getFullPathName().toWideCharPointer();
#else
        std::string modelPathStr = modelFile.getFullPathName().toStdString();
#endif
        
        env = std::make_unique<Ort::Env>(std::move(envLocal));
        session = std::make_unique<Ort::Session>(*env, modelPathStr.c_str(), sessionOptions);
        logMessage ("Session created: " + elekMemoryLine() + ", ORT live " + juce::String (juce::roundToInt (OrtSystemAllocator::liveMB())) + " MB");
        
        Ort::MemoryInfo memInfoLocal = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        memoryInfo = std::make_unique<Ort::MemoryInfo>(std::move(memInfoLocal));
        
        loadedModelName = modelFileName;
        logMessage(juce::String("ONNX Model loaded successfully: ") + modelFileName);
    }
    catch (const Ort::Exception& e) {
        logMessage(juce::String("ONNX Exception: ") + e.what());
    }
}

void SubverseSplitterAudioProcessor::ModelRunner::startOperation (const juce::File& audioFile, Operation op, double regionStartSec,
                                                                  double regionEndSec, const juce::File& runFolder)
{
    logMessage("--- startOperation called ---");
    logMessage("Input File: " + audioFile.getFullPathName());
    logMessage("File Exists: " + juce::String(audioFile.existsAsFile() ? "true" : "false"));

    if (isThreadRunning())
    {
        logMessage("Error: Thread is already running!");
        return;
    }

    inputFile = audioFile;
    operation = op;
    existingRunFolder = runFolder;
    const bool intoRun = op == Operation::DrumParts && runFolder != juce::File();      // parts of a run's Drums stem: never a region of it
    regionStart = intoRun ? -1.0 : regionStartSec;
    regionEnd = intoRun ? -1.0 : regionEndSec;
    {
        const juce::ScopedLock sl (analysisLock);
        analysis = {};
    }
    progress.set(0.0);
    finishedSuccess.set(false);
    errorMessage = "";

    // the run's folder and file names are decided once tempo and key are known (end of run(), ElekLibrary.h)
    vocalsFile = drumsFile = instrumentalFile = bassFile = otherFile = guitarFile = pianoFile = outputFolder = juce::File();
    for (auto& f : drumPartFiles) f = juce::File();

    startThread();
}

void SubverseSplitterAudioProcessor::ModelRunner::run()
{
    // Nothing thrown on this thread may escape it: an uncaught exception ends the process, and inside a DAW that is the DAW.
    // Out of memory on a long file, or an engine error, becomes a message in the window instead.
    try
    {
        runImpl();
    }
    catch (const std::bad_alloc&)
    {
        errorMessage = "Not enough memory for this file. Close other apps, or split the track into shorter parts.";
        logMessage ("Error: out of memory");
        currentState.set (static_cast<int> (State::Finished));
        finishedSuccess.set (false);
    }
    catch (const std::exception& e)
    {
        if (threadShouldExit())         // cancel() ended the model call: not an error
        {
            logMessage (juce::String ("Run cancelled (") + e.what() + ")");
            currentState.set (static_cast<int> (State::Finished));
            finishedSuccess.set (false);
            return;
        }
        errorMessage = "The separation stopped with an error. Try again; if it repeats, restart ELEK.";
        logMessage (juce::String ("Error: exception in run(): ") + e.what());
        currentState.set (static_cast<int> (State::Finished));
        finishedSuccess.set (false);
    }
    catch (...)
    {
        errorMessage = "The separation stopped with an error. Try again; if it repeats, restart ELEK.";
        logMessage ("Error: unknown exception in run()");
        currentState.set (static_cast<int> (State::Finished));
        finishedSuccess.set (false);
    }
}

// GUARD RAILS (2026-11-18). Before a stage loads its model: refuse when macOS reports critical memory pressure, or when it is
// under pressure AND has less free memory than the stage needs; with free memory short but no pressure it runs (macOS makes
// room by compressing other apps' memory) and says so in the log. Fewer threads do not help (measured: same memory with 2
// threads as with 4), and the models take a fixed 7.8 s / 10 s window, so stopping with a clear message is the only option.
bool SubverseSplitterAudioProcessor::ModelRunner::memoryAllowsStage (double needMB, const char* stage)
{
    const auto m = elekMemoryState();
    logMessage (juce::String ("Memory check before the ") + stage + ": needs about " + juce::String (juce::roundToInt (needMB))
                + " MB, free " + juce::String (juce::roundToInt (m.availableMB)) + " MB, pressure level " + juce::String (m.pressure)
                + ", " + elekMemoryLine());
    const bool shortOfMemory = m.availableMB >= 0.0 && m.availableMB < needMB;
    // 20 Nov: refuse only at CRITICAL pressure. The warning-level rule refused real runs that would have fitted (19 Nov 19:34
    // refused at "3.0 GB free", the same file ran two minutes later; measured stage peaks 2446-2642 MB, under the 3300 budget);
    // between windows memoryAllowsNextWindow() still stops cleanly at critical pressure.
    // FAST (owner 20 Nov: "biz bu 3,3'lük eşiği fast tuşu için koyduk"): with FAST on, the stricter rule stays: under memory
    // pressure and short of the stage's budget, it refuses and says to run without FAST.
    const bool fast = owner.fastMode.load();
    if (fast && shortOfMemory && m.pressure >= 2)
    {
        errorMessage = juce::String ("Not enough free memory for FAST right now: it needs about ") + elekGB (needMB)
                     + (m.availableMB >= 0.0 ? " and " + elekGB (m.availableMB) + " is free" : juce::String())
                     + ". Turn FAST off to run at normal speed, or close other apps.";
        logMessage ("Error: " + errorMessage);
        return false;
    }
    if (m.pressure >= 4)
    {
        errorMessage = "Your computer is critically low on memory right now. Close other apps and try again.";      // owner 20 Nov
        logMessage ("Error: " + errorMessage);
        return false;
    }
    if (shortOfMemory)
        logMessage ("Warning: less free memory than this stage uses; macOS will compress other apps' memory to make room.");
    return true;
}

// Between windows: stop cleanly (no crash, no half-written run) if the system reaches critical memory pressure.
bool SubverseSplitterAudioProcessor::ModelRunner::memoryAllowsNextWindow()
{
    auto m = elekMemoryState();
    // TEST ONLY, off by default: the pressure level seen between windows only (checks the stop-and-clean-up path).
    if (const char* v = std::getenv ("ELEK_TEST_WINDOW_PRESSURE"); v != nullptr && *v != 0) m.pressure = std::atoi (v);
    if (m.pressure < 4)
        return true;
    errorMessage = "Stopped: the computer ran critically low on memory. Close other apps and run it again.";
    logMessage ("Error: " + errorMessage + " (" + elekMemoryLine() + ")");
    return false;
}

void SubverseSplitterAudioProcessor::ModelRunner::runImpl()
{
    logMessage("run() background worker started");
    {
        const auto m = SubverseSplitterAudioProcessor::machine();
        logMessage ("Machine: " + m.chip + ", " + juce::String (m.ramGB, 1) + " GB, " + juce::String (m.performanceCores) + " performance cores of "
                    + juce::String (m.cores) + "; FAST " + (owner.fastMode.load() ? "asked for (no engine path yet: runs at normal speed)" : "off"));
    }
    
    currentState.set(static_cast<int>(State::SeparatingAudio));
    progress.set(0.1); 
    
    logMessage("Reading: " + inputFile.getFullPathName());
    
    // 2. Read Audio File into Buffer
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();
    
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(inputFile));
    if (reader == nullptr)
    {
        errorMessage = "This file could not be read. Try WAV, AIFF, MP3 or FLAC.";
        logMessage("Error: " + errorMessage);
        currentState.set(static_cast<int>(State::Finished));
        finishedSuccess.set(false);
        return;
    }
    
    auto refuse = [&] (const juce::String& message)
    {
        errorMessage = message;
        logMessage ("Error: " + errorMessage);
        currentState.set (static_cast<int> (State::Finished));
        finishedSuccess.set (false);
    };
    if (reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
    {
        refuse ("There is no audio in this file.");
        return;
    }

    // REGION (ELEK 2.0.0): only [regionStart, regionEnd) of the file is READ (seconds of the source), so a long recording with
    // a short region costs what the region costs. A range outside the file or under a second is refused rather than guessed.
    const double sourceRate = reader->sampleRate, fileSeconds = (double) reader->lengthInSamples / sourceRate;
    bool hasRegion = false;
    double regionFrom = 0.0, regionTo = 0.0;
    juce::int64 readStart = 0, readCount = reader->lengthInSamples;
    if (regionEnd > regionStart && regionStart >= -0.5)
    {
        regionFrom = juce::jmax (0.0, regionStart);
        regionTo = juce::jmin (fileSeconds, regionEnd);
        if (regionTo - regionFrom < 1.0)
        {
            refuse ("The selected range is too short or lies outside the file. Select at least one second of the track.");
            logMessage ("(asked " + juce::String (regionStart, 2) + " - " + juce::String (regionEnd, 2) + " s of a " + juce::String (fileSeconds, 2) + " s file)");
            return;
        }
        readStart = (juce::int64) std::llround (regionFrom * sourceRate);
        readCount = juce::jmin (reader->lengthInSamples - readStart, (juce::int64) std::llround ((regionTo - regionFrom) * sourceRate));
        hasRegion = true;
        logMessage ("Region: " + juce::String (regionFrom, 2) + " - " + juce::String (regionTo, 2) + " s -> source samples " + juce::String (readStart)
                    + " + " + juce::String (readCount));
    }

    // GUARD RAIL: more than kMaxInputSeconds is refused before it is decoded (the decoded samples are held in memory while tempo
    // and key are measured). It applies to what is read: the region when there is one.
    if ((double) readCount / sourceRate > kMaxInputSeconds)
    {
        const auto minutes = juce::String (juce::roundToInt ((double) readCount / sourceRate / 60.0));
        refuse ((hasRegion ? "The selected range is " + minutes + " minutes long. " : "This file is " + minutes + " minutes long. ")
                + "ELEK separates up to " + juce::String (juce::roundToInt (kMaxInputSeconds / 60.0))
                + (hasRegion ? " minutes at a time: select a shorter range." : " minutes at a time: select the part you need on the waveform, or split the recording."));
        return;
    }

    // Two channels always: a mono file is duplicated by the reader.
    int totalSamples = static_cast<int> (readCount);
    juce::AudioBuffer<float> inputBuffer (2, totalSamples);
    inputBuffer.clear();
    if (! reader->read (&inputBuffer, 0, totalSamples, readStart, true, true))
    {
        refuse ("This file could not be decoded to the end. Try exporting it again as WAV.");
        return;
    }

    logMessage ("MEM decoded: " + elekMemoryLine());
    // HT-Demucs was trained at 44.1 kHz, and demucs itself converts every input to the model's rate (and writes its stems at
    // that rate). Fed a 48 kHz file unconverted, the model hears the music 8.8 % slower and lower than anything it learned.
    const double modelRate = 44100.0;
    if (std::abs (reader->sampleRate - modelRate) > 0.5)
    {
        const double ratio = reader->sampleRate / modelRate;          // input samples per output sample
        const int outLen = (int) std::ceil ((double) totalSamples / ratio);
        // The sinc interpolator delays its output by its base latency (input samples). Uncorrected, the stems came out 92
        // samples late on a 48 kHz file (measured): they would not line up with the track in a DAW. Run it over the input plus
        // that much silence and drop the delayed head.
        const double latency = (double) juce::WindowedSincInterpolator::getBaseLatency();
        const int skip = juce::roundToInt (latency / ratio);
        const int padIn = totalSamples + (int) std::ceil (latency) + 8;
        juce::AudioBuffer<float> padded (2, padIn);
        padded.clear();
        juce::AudioBuffer<float> resampled (2, outLen);
        std::vector<float> tmp ((size_t) (outLen + skip));
        for (int ch = 0; ch < 2; ++ch)
        {
            padded.copyFrom (ch, 0, inputBuffer, ch, 0, totalSamples);
            juce::WindowedSincInterpolator interp;
            interp.process (ratio, padded.getReadPointer (ch), tmp.data(), outLen + skip, padIn, 0);
            resampled.copyFrom (ch, 0, tmp.data() + skip, outLen);
        }
        logMessage ("Resampled " + juce::String (reader->sampleRate, 0) + " Hz -> 44100 Hz for the model (" + juce::String (totalSamples)
                    + " -> " + juce::String (outLen) + " samples); stems are written at 44100 Hz, as demucs writes them.");
        inputBuffer = std::move (resampled);
        totalSamples = outLen;
    }

    // Under a second there is nothing to separate (and a zero-length buffer gave NaN statistics and an "empty" successful run).
    if (operation != Operation::Analysis && totalSamples < (int) modelRate)
    {
        refuse ("This file is too short to separate: it needs at least one second of audio.");
        return;
    }
    auto regionTag = [&]
    {
        auto mmss = [] (double sec) { const int t = juce::roundToInt (sec); return juce::String (t / 60) + "m" + juce::String (t % 60).paddedLeft ('0', 2) + "s"; };
        return hasRegion ? " " + mmss (regionFrom) + "-" + mmss (regionTo) : juce::String();
    };

    // Tempo and key, measured on the mix before separation starts (seconds, where separation takes minutes, so the reading is on
    // screen while the model works). Measured on the 44.1 kHz buffer: a 22.05 kHz MP3 read 71.8 BPM at its own rate where the
    // same music reads 143.6 (12 Oct). Not measured at all on silence or under 6 s: there it returned "107.7 BPM, C major" for
    // five seconds of digital silence, and a key from 1 s of music that the full 30 s contradicted.
    {
        const float peak = inputBuffer.getMagnitude (0, totalSamples);
        const bool silent = peak < 1.0e-4f;                                   // -80 dBFS
        const bool tooShort = (double) totalSamples / modelRate < 6.0;
        auto measured = (silent || tooShort) ? AudioAnalysis() : AudioAnalysis::analyse (inputBuffer, modelRate);
        const juce::ScopedLock sl (analysisLock);
        analysis = measured;
        if (operation == Operation::Analysis && ! measured.valid)
            errorMessage = silent ? "There is no audible signal in this file."
                         : tooShort ? "Too short to measure: tempo and key need at least 6 seconds."
                                    : "Not enough audio to measure tempo or key.";
    }
    logMessage ("MEM analysed: " + elekMemoryLine());
    if (analysis.valid)
        logMessage("Analysis: " + juce::String(analysis.bpm, 1) + " BPM, key "
                   + analysis.key + " " + analysis.scale + " (" + analysis.camelot
                   + "), confidence " + juce::String(analysis.keyConfidence, 2));
    else
        logMessage("Analysis: not measured (silent, under 6 s, or nothing to measure).");

    // Key & BPM only: no model, no stems
    if (operation == Operation::Analysis)
    {
        progress.set (1.0);
        currentState.set (static_cast<int> (State::Finished));
        finishedSuccess.set (analysis.valid);
        return;
    }

    // DRUM PARTS of a drums file (ELEK 2.0.0): no main model; the input is the Drums stem (or any drums-only file) and goes
    // straight to the DrumSep stage. Used by "Split the drums" on an ELEK run (existingRunFolder: the parts are written into
    // that run and its elek.json is updated); without a run folder the parts go to a new one (no door in the window uses that yet).
    if (operation == Operation::DrumParts)
    {
        auto fail = [&] (const juce::String& message)
        {
            errorMessage = message;
            logMessage ("Error: " + errorMessage);
            currentState.set (static_cast<int> (State::Finished));
            finishedSuccess.set (false);
        };
        const juce::File drumModel = elekDrumSepModel();
        if (! drumModel.existsAsFile())
        {
            fail ("The drum model is not installed. Reinstall ELEK.");
            return;
        }
        ElekFloatSpill drumsSpill ("drums");
        if (! drumsSpill.append (inputBuffer.getReadPointer (0), inputBuffer.getReadPointer (1), totalSamples) || ! drumsSpill.finishWriting())
        {
            fail ("Could not write ELEK's temporary file. Check that the disk has room.");
            return;
        }
        inputBuffer = juce::AudioBuffer<float>();
        reader.reset();
        elekRelieveMalloc();

        AudioAnalysis measured;
        {
            const juce::ScopedLock sl (analysisLock);
            measured = analysis;
        }
        const auto title = ElekLibrary::shortTitle (inputFile) + regionTag();
        const bool intoRun = existingRunFolder.isDirectory();
        const juce::File folder = intoRun ? existingRunFolder : ElekLibrary::newRunFolder (title, measured);
        // Into an existing run the part names follow that run's Drums file ("<title>_Drums_<bpm>_<key>_ELEK.wav" -> "..._Kick_...").
        auto partFile = [&] (int s)
        {
            const juce::String part = DrumSplit::stemDisplayName (s);
            const auto name = inputFile.getFileName();
            return folder.getChildFile (intoRun && name.contains ("_Drums_") ? name.replace ("_Drums_", "_" + part + "_")
                                                                             : ElekLibrary::stemFileName (title, part, measured));
        };
        juce::StringPairArray partFiles;
        splitProgressBase = 0.1; splitProgressSpan = 0.89;       // the split is the whole run here
        const bool keepGoing = runDrumSplit (drumsSpill, totalSamples, drumModel, partFile, partFiles);
        if (! keepGoing || partFiles.size() == 0)
        {
            if (! intoRun) folder.deleteRecursively();
            if (keepGoing) fail ("The drums could not be split: " + (drumSplitProblem.isEmpty() ? juce::String ("unknown reason") : drumSplitProblem));
            return;
        }
        ElekLibrary::Entry entry;
        if (! (intoRun && ElekLibrary::readManifest (folder, entry)))
        {
            entry = {};
            entry.folder = folder;
            entry.title = title;
            entry.source = inputFile.getFullPathName();
            entry.operation = "drum_parts";
            entry.created = juce::Time::getCurrentTime();
            entry.analysis = measured;
            if (hasRegion) { entry.regionStart = regionFrom; entry.regionEnd = regionTo; }
        }
        for (auto& k : partFiles.getAllKeys())
            entry.files.set (k, partFiles[k]);
        entry.files.remove ("toms");          // three parts from 20 Nov: the toms are in the kick
        if (! ElekLibrary::writeManifest (entry))
        {
            fail ("Could not write the result to " + folder.getFullPathName() + ". Check that the disk has room.");
            return;
        }
        outputFolder = folder;
        logMessage ("Drum parts written to " + folder.getFullPathName());
        progress.set (1.0);
        currentState.set (static_cast<int> (State::Finished));
        finishedSuccess.set (true);
        return;
    }

    // 1. Memory check, then the ONNX model (after the reading, so tempo and key are on screen while it loads)
    const bool six = operation == Operation::Stems6;
    const bool drumsOnly = operation == Operation::DrumsOnly;      // ELEK 2.0.0: the 4-stem model, but only the Drums stem is written, then split
    const bool allStems = operation == Operation::Stems || six;
    if (drumsOnly && ! elekDrumSepModel().existsAsFile())
    {
        errorMessage = "The drum model is not installed. Reinstall ELEK.";
        logMessage ("Error: " + errorMessage);
        currentState.set (static_cast<int> (State::Finished));
        finishedSuccess.set (false);
        return;
    }
    if (! memoryAllowsStage (six ? kMainStage6MB : kMainStage4MB, "separation"))
    {
        progress.set (0.0);
        currentState.set (static_cast<int> (State::Finished));
        finishedSuccess.set (false);
        return;
    }

    loadModel (six ? "htdemucs_6s.onnx" : "htdemucs.onnx");
    if (session == nullptr)
    {
        if (errorMessage.isEmpty())
            errorMessage = "The separation engine could not start. Restart ELEK; if it happens again, reinstall it.";

        logMessage("Error: " + errorMessage);
        currentState.set(static_cast<int>(State::Finished));
        finishedSuccess.set(false);
        return;
    }

    // The session is released at the end of every run, however it ends: an idle ELEK holds no model memory (about 1 GB
    // with the session loaded). Loading again costs 0.4-2.2 s per run (measured).
    struct ReleaseModelAtEnd { ModelRunner& r; ~ReleaseModelAtEnd() { r.releaseModel(); logMessage ("Model released: " + elekMemoryLine()); } } releaseAtEnd { *this };

    if (threadShouldExit()) return;

    logMessage("Starting ONNX Audio Separation on: " + inputFile.getFullPathName());

    // Check RMS to verify we actually loaded audio
    float maxVal = inputBuffer.getMagnitude(0, totalSamples);
    logMessage(juce::String("Input Audio Max Magnitude: ") + juce::String(maxVal));
    if (maxVal < 0.0001f)
    {
        logMessage("Warning: Input audio appears to be completely silent!");
    }

    // 3. Inference Setup
    //
    // Two things here are corrections rather than preferences, and both were
    // measured on real music before being written.
    //
    // OVERLAPPING WINDOWS. This loop used to step through the file in
    // consecutive 343980-sample blocks and concatenate the model's output for
    // each. HT-Demucs is not a per-sample filter: it denoises a whole window at
    // once and its edges are the least certain part of it, so butt-joining two
    // independently-produced windows leaves a step at every join — an audible
    // tick every 7.8 seconds, in every stem. Measured on a 40-second excerpt,
    // the jump at a seam was 35.4x the surrounding signal, worst in the vocal.
    // With the 25% overlap and triangular cross-fade below it is 2.4x, which is
    // indistinguishable from ordinary signal. The cost is a third more model
    // runs, and it is the whole reason a separation takes as long as it does.
    //
    // INPUT STANDARDISATION. Demucs is trained on input normalised to zero mean
    // and unit variance from the mono reference. This fed it raw samples, so a
    // quiet master and a loud one landed in different parts of the model's
    // input space and only one of them was the part it was trained on.
    // Normalising in and denormalising out costs nothing.
    const int CHUNK_SIZE = 343980; // 7.8 seconds at 44.1kHz
    const double OVERLAP = 0.25;

    // Window start positions. The last one is pinned to the end of the file so
    // the tail is covered by a full window rather than by padding alone.
    const std::vector<int> windowStarts = OverlapAdd::windowStarts(totalSamples, CHUNK_SIZE, OVERLAP);
    const int numChunks = (int) windowStarts.size();

    // A triangular ramp peaking mid-window. Where two windows disagree, the one
    // whose centre is nearer carries more of the result; a rectangular weight
    // would average them equally and leave the seam audible.
    const std::vector<float> windowWeight = OverlapAdd::triangularWeight(CHUNK_SIZE);

    // Standardise against the mono reference, exactly as Demucs does.
    float referenceMean = 0.0f, referenceStd = 1.0f;
    {
        double sum = 0.0;
        for (int i = 0; i < totalSamples; ++i)
        {
            double mono = 0.0;
            for (int ch = 0; ch < inputBuffer.getNumChannels(); ++ch)
                mono += inputBuffer.getReadPointer(ch)[i];
            sum += mono / inputBuffer.getNumChannels();
        }
        referenceMean = (float) (sum / totalSamples);

        double variance = 0.0;
        for (int i = 0; i < totalSamples; ++i)
        {
            double mono = 0.0;
            for (int ch = 0; ch < inputBuffer.getNumChannels(); ++ch)
                mono += inputBuffer.getReadPointer(ch)[i];
            const double d = mono / inputBuffer.getNumChannels() - referenceMean;
            variance += d * d;
        }
        referenceStd = (float) std::sqrt(variance / totalSamples);
        // Digital silence has zero variance, and dividing by it would reach the
        // listener as a stem full of NaN rather than as an error.
        if (referenceStd < 1.0e-8f) referenceStd = 1.0f;
    }

    // MEMORY (2026-11-18): nothing below holds the whole song. The 44.1 kHz mix goes to a temporary file and each window
    // reads its part back; each stem is written to its WAV as soon as no later window can change it. Before this, the mix,
    // every full-length stem, the instrumental and the weight sum were held at once (68 bytes per sample with six stems,
    // ~790 MB for a 4.4-minute song) on top of the model. The numbers written are the same, bit for bit (STATUS.md).
    ElekFloatSpill mixSpill ("mix");
    if (! mixSpill.append (inputBuffer.getReadPointer (0), inputBuffer.getReadPointer (juce::jmin (1, inputBuffer.getNumChannels() - 1)), totalSamples)
        || ! mixSpill.finishWriting())
    {
        errorMessage = "Could not write ELEK's temporary file. Check that the disk has room.";
        logMessage ("Error: " + errorMessage + " (" + mixSpill.getFile().getFullPathName() + ")");
        currentState.set (static_cast<int> (State::Finished));
        finishedSuccess.set (false);
        return;
    }
    inputBuffer = juce::AudioBuffer<float>();
    reader.reset();
    elekRelieveMalloc();
    logMessage ("MEM mix moved to a temporary file: " + elekMemoryLine());

    // The model's own order: 0 drums, 1 bass, 2 other, 3 vocals, then (htdemucs_6s only) 4 guitar, 5 piano.
    // The six-stem order was read from the model (scripts/export_6s_onnx.py, htdemucs_6s.onnx.sources.json), not assumed.
    const int numStems = six ? 6 : 4;
    const int kVocalStem = 3;
    const double sr = modelRate;   // the stems are at the model's rate (resampled above when the file was not)

    // ELEK's own output (ElekLibrary.h): ~/Music/ELEK/<title>_<bpm>BPM_<key>_<date>/<title>_<Stem>_<bpm>BPM_<key>_ELEK.wav,
    // with elek.json beside them so the window can list and reopen the run later. Made before the windows run now (the
    // stems are written while they are computed); a run that stops early deletes its folder again.
    AudioAnalysis measured;
    {
        const juce::ScopedLock sl (analysisLock);
        measured = analysis;
    }
    const auto title = ElekLibrary::shortTitle (inputFile) + regionTag();
    const auto folder = ElekLibrary::newRunFolder (title, measured);
    auto named = [&] (const char* stem) { return folder.getChildFile (ElekLibrary::stemFileName (title, stem, measured)); };
    vocalsFile = drumsOnly ? juce::File() : named ("Vocals");
    instrumentalFile = drumsOnly ? juce::File() : named ("Instrumental");
    drumsFile = (allStems || drumsOnly) ? named ("Drums") : juce::File();
    bassFile  = allStems ? named ("Bass")  : juce::File();
    otherFile = allStems ? named ("Other") : juce::File();
    guitarFile = six ? named ("Guitar") : juce::File();
    pianoFile  = six ? named ("Piano")  : juce::File();
    outputFolder = folder;

    // One 24-bit writer per output, open for the whole run. source: the stem index, or -1 for the instrumental (mix - vocal).
    // 24-bit, not 16: separation output routinely exceeds the input's peak (the model estimates a source rather than carving
    // up a fixed budget), so 16-bit clipped material that never clipped going in.
    struct StemOut { juce::File file; int source; const char* label; std::unique_ptr<juce::AudioFormatWriter> writer {}; float peak = 0.0f; };
    std::vector<StemOut> outs;
    outs.reserve (7);
    if (! drumsOnly)
    {
        outs.push_back ({ vocalsFile, kVocalStem, "Vocals" });
        outs.push_back ({ instrumentalFile, -1, "Instrumental" });
    }
    if (drumsOnly) outs.push_back ({ drumsFile, 0, "Drums" });
    if (allStems) { outs.push_back ({ drumsFile, 0, "Drums" }); outs.push_back ({ bassFile, 1, "Bass" }); outs.push_back ({ otherFile, 2, "Other" }); }
    if (six)      { outs.push_back ({ guitarFile, 4, "Guitar" }); outs.push_back ({ pianoFile, 5, "Piano" }); }

    // Any way out before the stems are complete removes the half-written run, so the library never lists it.
    bool runComplete = false;
    struct DropUnfinishedRun
    {
        std::vector<StemOut>& o; const juce::File& f; const bool& complete; juce::File& published;
        ~DropUnfinishedRun()
        {
            if (complete) return;
            for (auto& s : o) s.writer.reset();
            f.deleteRecursively();
            published = juce::File();        // getOutputFolder() must not name a folder that is gone
            logMessage ("Unfinished run removed: " + f.getFullPathName());
        }
    } dropUnfinished { outs, folder, runComplete, outputFolder };

    auto failWrite = [&]
    {
        errorMessage = "Could not write the stems to " + folder.getFullPathName() + ". Check that the disk has room.";
        logMessage ("Error: " + errorMessage);
        currentState.set (static_cast<int> (State::Finished));
        finishedSuccess.set (false);
        progress.set (0.0);
    };

    for (auto& o : outs)
    {
        o.file.deleteFile();
        juce::WavAudioFormat wavFormat;
        if (auto stream = o.file.createOutputStream())
        {
            o.writer.reset (wavFormat.createWriterFor (stream.get(), sr, 2, 24, {}, 0));
            if (o.writer != nullptr)
                stream.release();     // the writer owns the stream now
        }
        if (o.writer == nullptr) { failWrite(); return; }
    }

    // The drum split reads the Drums stem back window by window: kept as floats in a temporary file (not re-read from the
    // 24-bit WAV), so the split gets exactly the numbers it got from memory before.
    const juce::File drumModel = drumsOnly ? elekDrumSepModel() : juce::File();     // no longer automatic in 4/6-stem runs (owner, 18 Nov)
    std::unique_ptr<ElekFloatSpill> drumsSpill;
    if (drumModel.existsAsFile())
        drumsSpill = std::make_unique<ElekFloatSpill> ("drums");

    // The overlap-add accumulator holds absolute samples [accStart, accStart + CHUNK_SIZE): exactly what the windows not yet
    // finished can still add to. Per sample the arithmetic is the one the whole-song buffers did, in the same order: window
    // contributions added to a zeroed float in window order, then (sum / weight) * std + mean.
    std::vector<float> acc ((size_t) numStems * 2 * CHUNK_SIZE, 0.0f);
    std::vector<float> wacc ((size_t) CHUNK_SIZE, 0.0f);
    int accStart = 0;
    std::vector<float> mixL ((size_t) CHUNK_SIZE), mixR ((size_t) CHUNK_SIZE), instL ((size_t) CHUNK_SIZE), instR ((size_t) CHUNK_SIZE);
    auto accChannel = [&] (int stem, int ch) { return acc.data() + ((size_t) stem * 2 + (size_t) ch) * (size_t) CHUNK_SIZE; };

    // Normalise the first `count` samples of the accumulator, write them to every output, then move the rest to the front.
    auto flush = [&] (int count) -> bool
    {
        if (count <= 0) return true;
        for (int stem = 0; stem < numStems; ++stem)
            for (int ch = 0; ch < 2; ++ch)
            {
                float* data = accChannel (stem, ch);
                for (int n = 0; n < count; ++n)
                {
                    const float weight = wacc[(size_t) n] > 0.0f ? wacc[(size_t) n] : 1.0f;
                    // De-standardised WITHOUT the mix's mean (demucs adds it to every source, and ELEK did until 18 Nov): a DC
                    // offset in the file belongs to no instrument. With it, each stem of a recording with an offset carried
                    // the whole offset (-47.8 dBFS on the test clip: a silent piano stem "measured" -47.8 dB RMS), and the
                    // stems summed to the mix plus several copies of it. The instrumental (mix - vocals) keeps the file's own.
                    data[n] = (data[n] / weight) * referenceStd;
                }
            }

        // The backing track: the mix with the vocal removed. Built here rather than left to the listener to reconstruct by
        // muting one lane, because muting the vocal in the mixer plays three stems whose sum is not the mix: whatever the
        // model could not place goes missing with it.
        if (! drumsOnly)
        {
            if (! mixSpill.read (accStart, count, mixL.data(), mixR.data()))
                return false;
            for (int n = 0; n < count; ++n)
            {
                instL[(size_t) n] = mixL[(size_t) n] - accChannel (kVocalStem, 0)[n];
                instR[(size_t) n] = mixR[(size_t) n] - accChannel (kVocalStem, 1)[n];
            }
        }

        for (auto& o : outs)
        {
            float* chans[2] = { o.source < 0 ? instL.data() : accChannel (o.source, 0),
                                o.source < 0 ? instR.data() : accChannel (o.source, 1) };
            const juce::AudioBuffer<float> view (chans, 2, count);     // refers to the data, no copy
            o.peak = juce::jmax (o.peak, view.getMagnitude (0, count));
            if (! o.writer->writeFromAudioSampleBuffer (view, 0, count))
                return false;
        }
        if (drumsSpill != nullptr && ! drumsSpill->append (accChannel (0, 0), accChannel (0, 1), count))
            return false;

        const int keep = CHUNK_SIZE - count;
        for (int c = 0; c < numStems * 2; ++c)
        {
            float* d = acc.data() + (size_t) c * (size_t) CHUNK_SIZE;
            if (keep > 0) std::memmove (d, d + count, (size_t) keep * sizeof (float));
            std::fill (d + juce::jmax (0, keep), d + CHUNK_SIZE, 0.0f);
        }
        if (keep > 0) std::memmove (wacc.data(), wacc.data() + count, (size_t) keep * sizeof (float));
        std::fill (wacc.begin() + juce::jmax (0, keep), wacc.end(), 0.0f);
        accStart += count;
        return true;
    };

    std::vector<int64_t> inputDims = {1, 2, CHUNK_SIZE};
    const char* inputNames[] = {"mix"};
    const char* outputNames[] = {"stems"};

    // Allocate input tensor memory once
    std::vector<float> inputTensorValues(2 * CHUNK_SIZE, 0.0f);
    logMessage ("MEM before the windows: " + elekMemoryLine());

    logMessage(juce::String("Processing ") + juce::String(numChunks) + " overlapping windows...");
    ActiveRun activeRunScope (*this);      // lets cancel() end a window that is being computed

    for (int i = 0; i < numChunks; ++i)
    {
        if (threadShouldExit()) return;
        if (! memoryAllowsNextWindow())
        {
            currentState.set (static_cast<int> (State::Finished));
            finishedSuccess.set (false);
            return;
        }

        const int startSample = windowStarts[(size_t) i];
        const int samplesToProcess = std::min(CHUNK_SIZE, totalSamples - startSample);

        // Every sample before this window's start is covered by no later window: finished, written now.
        if (! flush (startSample - accStart)) { failWrite(); return; }

        // Clear input tensor (for zero padding the last chunk if needed)
        std::fill(inputTensorValues.begin(), inputTensorValues.end(), 0.0f);

        // Read the window's mix back and standardise it into the planar tensor (Left channel then Right channel)
        if (! mixSpill.read (startSample, samplesToProcess, mixL.data(), mixR.data())) { failWrite(); return; }
        for (int ch = 0; ch < 2; ++ch)
        {
            const float* channelData = ch == 0 ? mixL.data() : mixR.data();
            float* destination = inputTensorValues.data() + (ch * CHUNK_SIZE);
            for (int n = 0; n < samplesToProcess; ++n)
                destination[n] = (channelData[n] - referenceMean) / referenceStd;
        }

        // Create ONNX Tensor
        Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
            *memoryInfo, inputTensorValues.data(), inputTensorValues.size(), inputDims.data(), inputDims.size()
        );

        // RUN AI MODEL (Extremely CPU heavy)
        auto outputTensors = session->Run (activeRunScope.options, inputNames, &inputTensor, 1, outputNames, 1);

        // Output tensor shape is [1, numStems, 2, 343980]; a model with another stem count would be read out of bounds
        {
            const auto shape = outputTensors.front().GetTensorTypeAndShapeInfo().GetShape();
            if (shape.size() != 4 || shape[1] != numStems || shape[2] != 2 || shape[3] != CHUNK_SIZE)
            {
                errorMessage = "The separation engine does not match this version of ELEK. Reinstall ELEK.";
                logMessage ("Error: model output shape does not match " + juce::String (numStems) + " stems");
                currentState.set (static_cast<int> (State::Finished));
                finishedSuccess.set (false);
                return;
            }
        }
        const float* outData = outputTensors.front().GetTensorMutableData<float>();

        // Accumulate under the window weight rather than overwriting.
        for (int stemIdx = 0; stemIdx < numStems; ++stemIdx)
            for (int ch = 0; ch < 2; ++ch)
            {
                const size_t offset = (size_t) (stemIdx * 2 * CHUNK_SIZE) + (size_t) (ch * CHUNK_SIZE);
                const float* source = outData + offset;
                float* destination = accChannel (stemIdx, ch);
                for (int n = 0; n < samplesToProcess; ++n)
                    destination[n] += source[n] * windowWeight[(size_t) n];
            }

        for (int n = 0; n < samplesToProcess; ++n)
            wacc[(size_t) n] += windowWeight[(size_t) n];

        progress.set(0.1 + (0.8 * (static_cast<double>(i + 1) / numChunks)));
        logMessage ("Window " + juce::String (i + 1) + "/" + juce::String (numChunks) + ": " + elekMemoryLine()
                    + ", ORT live " + juce::String (juce::roundToInt (OrtSystemAllocator::liveMB())) + " MB, ORT high "
                    + juce::String (juce::roundToInt (OrtSystemAllocator::highMB())) + " MB");
    }

    // The tail after the last window's start, then close the files (the writers finish the WAV headers on close).
    if (! flush (totalSamples - accStart)) { failWrite(); return; }
    for (auto& o : outs) o.writer.reset();
    if (drumsSpill != nullptr && ! drumsSpill->finishWriting()) { failWrite(); return; }
    runComplete = true;
    std::vector<float>().swap (acc);

    logMessage ("Main separation done: " + elekMemoryLine());

    // Log the magnitude of the separated stems
    for (const auto& o : outs)
        logMessage (juce::String ("Max ") + o.label + " Magnitude: " + juce::String (o.peak));
    logMessage ("Wrote the separated WAV files.");

    ElekLibrary::Entry entry;
    entry.folder = folder;
    entry.title = title;
    entry.source = inputFile.getFullPathName();
    entry.operation = six ? "stem_separation_6" : allStems ? "stem_separation" : drumsOnly ? "drums_only" : "vocal_removal";
    entry.created = juce::Time::getCurrentTime();
    entry.analysis = measured;
    if (hasRegion) { entry.regionStart = regionFrom; entry.regionEnd = regionTo; }
    if (! drumsOnly)
    {
        entry.files.set ("vocals", vocalsFile.getFileName());
        entry.files.set ("instrumental", instrumentalFile.getFileName());
    }
    if (allStems || drumsOnly)
        entry.files.set ("drums", drumsFile.getFileName());
    if (allStems)
    {
        entry.files.set ("bass", bassFile.getFileName());
        entry.files.set ("other", otherFile.getFileName());
    }
    if (six)
    {
        entry.files.set ("guitar", guitarFile.getFileName());
        entry.files.set ("piano", pianoFile.getFileName());
    }
    if (! ElekLibrary::writeManifest (entry)) { runComplete = false; failWrite(); return; }      // without elek.json the run could never be opened
    logMessage ("Wrote " + folder.getFullPathName());

    // 5. Drum parts, ONLY in the drums-only operation (owner, 18 Nov: not automatic in 4/6-stem runs; those offer "split the drums"
    // afterwards on the stem they produced, Operation::DrumParts). The Drums stem and elek.json are already on disk, so a failed
    // split leaves the Drums stem and reports the reason.
    if (drumsOnly)
    {
        juce::StringPairArray partFiles;
        auto partFile = [&] (int s) { return named (DrumSplit::stemDisplayName (s)); };
        splitProgressBase = 0.9; splitProgressSpan = 0.09;       // after the main windows (0.1 .. 0.9)
        if (! runDrumSplit (*drumsSpill, totalSamples, drumModel, partFile, partFiles))
            return;      // asked to stop (same as a stop during the main separation)
        if (partFiles.size() == 0)
        {
            errorMessage = "The drums were separated but could not be split into parts: " + (drumSplitProblem.isEmpty() ? juce::String ("unknown reason") : drumSplitProblem);
            logMessage ("Error: " + errorMessage);
            currentState.set (static_cast<int> (State::Finished));
            finishedSuccess.set (false);
            return;
        }
        for (auto& k : partFiles.getAllKeys())
            entry.files.set (k, partFiles[k]);
        if (! ElekLibrary::writeManifest (entry)) { failWrite(); return; }
        logMessage ("Drum split: elek.json updated with " + juce::String (partFiles.size()) + " parts");
    }

    logMessage("Real DSP Separation finished successfully! Files mapped. " + elekMemoryLine());
    progress.set(1.0);
    currentState.set(static_cast<int>(State::Finished));
    finishedSuccess.set(true);
}

bool SubverseSplitterAudioProcessor::ModelRunner::runDrumSplit (ElekFloatSpill& drums, int n, const juce::File& model,
                                                               const std::function<juce::File (int part)>& partFile,
                                                               juce::StringPairArray& manifestFiles)
{
    drumSplitProblem = {};
   #if JUCE_WINDOWS
    {
        // "Split the drums" can be the first model use in the process: onnxruntime.dll must be loaded and the API set up here too
        juce::String why;
        if (! elekLoadOrt (why))
        {
            drumSplitProblem = "ELEK's separation engine is missing from this installation. Reinstall ELEK.";
            logMessage ("Error: " + why);
            return true;
        }
    }
   #endif
    if (n <= 0)
    {
        drumSplitProblem = "there is no drums audio";
        logMessage ("Drum split skipped: no drums audio.");
        return true;
    }

    // MEMORY ORDER (8 GB Macs): the main model's session is released BEFORE the DrumSep session is created; the two are never
    // held together. The next separation loads the main model again (a few seconds).
    logMessage ("Drum split: before releasing the main model: " + elekMemoryLine());
    releaseModel();
    elekRelieveMalloc();
    OrtSystemAllocator::resetHigh();     // "ORT high" in the drum split lines is DrumSep's own
    logMessage ("Drum split: main model released: " + elekMemoryLine());

    if (threadShouldExit()) return false;
    if (! memoryAllowsStage (kDrumStageMB, "drum split"))
    {
        logMessage ("Drum split stopped for memory: " + errorMessage);
        drumSplitProblem = errorMessage;
        errorMessage = {};
        return true;
    }

    // File names follow the main stems ("<title>_Kick_<bpm>BPM_<key>_ELEK.wav"); manifest keys are the stem word in lower case,
    // as for the main stems ("drums" -> "..._Drums_..."): kick, snare, cymbals, toms. Model order: bombo, redoblante, platillos, toms.
    // THREE PARTS (owner 20 Nov: "tomu çıkaralım, kick snare hi hat"): the model's toms output is added to the kick and no Toms
    // file is written. Measured on the owner's track the same day: the "toms" output was the loudest part (-24.3 dBFS against the
    // kick's -29.4) with 51 % of its energy under 120 Hz, i.e. the kick / 808 body, not toms. Kick + Snare + Cymbals still add up
    // to the drums exactly as the four parts did.
    constexpr int kWritten = 3;                      // kick (bombo + toms), snare, cymbals
    juce::File files[DrumSplit::kStems];
    std::unique_ptr<juce::AudioFormatWriter> writers[DrumSplit::kStems];
    auto dropParts = [&]
    {
        for (int s = 0; s < kWritten; ++s) { writers[s].reset(); if (files[s] != juce::File()) files[s].deleteFile(); }
    };
    partFile (3).deleteFile();                       // a Toms file from an earlier split of this run
    for (int s = 0; s < kWritten; ++s)
    {
        files[s] = partFile (s);
        files[s].deleteFile();
        juce::WavAudioFormat wavFormat;
        if (auto stream = files[s].createOutputStream())
        {
            writers[s].reset (wavFormat.createWriterFor (stream.get(), (double) DrumSplit::kSampleRate, 2, 24, {}, 0));
            if (writers[s] != nullptr)
                stream.release();
        }
        if (writers[s] == nullptr)
        {
            drumSplitProblem = "could not write " + files[s].getFullPathName();
            logMessage ("Drum split: could not write " + files[s].getFullPathName() + ".");
            dropParts();
            return true;
        }
    }

    DrumSplit::Options options;
    options.intraOpThreads = elekThreadCount();
    options.progress = [this] (int done, int total)
    {
        progress.set (splitProgressBase + splitProgressSpan * (double) done / (double) juce::jmax (1, total));
        logMessage ("Drum split window " + juce::String (done) + "/" + juce::String (total) + ": " + elekMemoryLine()
                    + ", ORT live " + juce::String (juce::roundToInt (OrtSystemAllocator::liveMB())) + " MB, ORT high "
                    + juce::String (juce::roundToInt (OrtSystemAllocator::highMB())) + " MB");
        return ! threadShouldExit() && (done >= total || memoryAllowsNextWindow());      // after the last window only the flush remains
    };

    logMessage ("Drum split: running " + model.getFullPathName() + " on " + juce::String (n) + " samples");
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    ActiveRun activeRunScope (*this);          // lets cancel() end a split window that is being computed
    options.runOptions = &activeRunScope.options;
    DrumSplit::DrumSplitter splitter (model.getFullPathName().toStdString(), options);
    bool writeFailed = false;
    std::vector<float> kickPlusToms;
    const auto parts = splitter.processStream (n,
        [&] (int start, int count, float* left, float* right) { return drums.read (start, count, left, right); },
        [&] (int, int count, const float* const* channels)
        {
            kickPlusToms.resize ((size_t) count * 2);
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < count; ++i)
                    kickPlusToms[(size_t) c * (size_t) count + (size_t) i] = channels[c][i] + channels[3 * 2 + c][i];
            for (int s = 0; s < kWritten; ++s)
            {
                float* chans[2] = { const_cast<float*> (channels[s * 2]), const_cast<float*> (channels[s * 2 + 1]) };
                if (s == 0) { chans[0] = kickPlusToms.data(); chans[1] = kickPlusToms.data() + count; }
                const juce::AudioBuffer<float> view (chans, 2, count);     // refers to the data, no copy
                if (! writers[s]->writeFromAudioSampleBuffer (view, 0, count)) { writeFailed = true; return false; }
            }
            return true;
        });
    logMessage ("Drum split: " + juce::String (parts.windowsRun) + " windows, "
                + juce::String ((juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0, 2) + " s, " + elekMemoryLine());

    if (threadShouldExit()) { dropParts(); return false; }
    if (! parts.ok)
    {
        drumSplitProblem = errorMessage.isNotEmpty() ? errorMessage      // e.g. stopped for memory: the reason, not "cancelled"
                                                     : juce::String (parts.error) + (writeFailed ? " (writing a part failed)" : "");
        logMessage ("Drum split failed: " + juce::String (parts.error)
                    + (writeFailed ? " (writing a part failed)" : "") + (errorMessage.isNotEmpty() ? " - " + errorMessage : juce::String()));
        errorMessage = {};      // the caller reports drumSplitProblem
        dropParts();
        return true;
    }

    for (int s = 0; s < kWritten; ++s)
    {
        writers[s].reset();     // finishes the WAV header
        drumPartFiles[s] = files[s];
        manifestFiles.set (juce::String (DrumSplit::stemDisplayName (s)).toLowerCase(), files[s].getFileName());
    }
    drumPartFiles[3] = juce::File();
    const int moved = elekMoveKickAttacks (files[0], files[1]);
    logMessage ("Drum split: wrote 3 parts (toms in the kick), kick attacks moved out of the snare: "
                + (moved >= 0 ? juce::String (moved) : juce::String ("failed")) + ", " + elekMemoryLine());
    return true;
}

// This creates the instantiator for JUCE plugin
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SubverseSplitterAudioProcessor();
}
