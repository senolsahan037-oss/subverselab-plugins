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
#include <vector>
#include "PluginEditor.h"
#include "ElekLibrary.h"

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
    for (int i = 0; i < 4; ++i)
    {
        if (transportSources[i] != nullptr)
            transportSources[i]->prepareToPlay(static_cast<int>(samplesPerBlock), sampleRate);
    }
}

void SubverseSplitterAudioProcessor::releaseResources()
{
    mixerSource.releaseResources();
    for (int i = 0; i < 4; ++i)
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
    for (int i = 0; i < 4; ++i)
    {
        if (transportSources[i] != nullptr) { transportSources[i]->stop(); transportSources[i]->setSource (nullptr); }
        transportSources[i].reset();
        readerSources[i].reset();
        // a new result starts with a fresh mixer: no mute or solo left over from the last one, every stem at full level
        stemMutes[i].store (false);
        stemSolos[i].store (false);
        stemVolumes[i].store (1.0f);
    }
}

void SubverseSplitterAudioProcessor::loadStemForPlayback(int stemIndex, const juce::File& file)
{
    if (stemIndex < 0 || stemIndex >= 4) return;
    
    auto* reader = formatManager.createReaderFor(file);
    if (reader != nullptr)
    {
        auto newSource = std::make_unique<juce::AudioFormatReaderSource>(reader, true);
        auto newTransport = std::make_unique<juce::AudioTransportSource>();
        newTransport->setSource(newSource.get(), 0, nullptr, reader->sampleRate);
        
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
        for (int i = 0; i < 4; ++i)
        {
            if (transportSources[i] != nullptr && transportSources[i]->hasStreamFinished())
                transportSources[i]->setPosition(0.0);
            
            if (transportSources[i] != nullptr)
                transportSources[i]->start();
        }
    }
    else
    {
        for (int i = 0; i < 4; ++i)
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
    const bool anySolo = stemSolos[0] || stemSolos[1] || stemSolos[2] || stemSolos[3];

    for (int i = 0; i < 4; ++i)
    {
        if (transportSources[i] == nullptr)
            continue;

        const bool audible = anySolo ? stemSolos[i].load() : ! stemMutes[i].load();
        transportSources[i]->setGain (audible ? stemVolumes[i].load() : 0.0f);
    }
}

void SubverseSplitterAudioProcessor::setStemMute(int stemIndex, bool shouldMute)
{
    if (stemIndex >= 0 && stemIndex < 4)
    {
        stemMutes[stemIndex] = shouldMute;
        applyGains();
    }
}

void SubverseSplitterAudioProcessor::setStemVolume (int stemIndex, float volume)
{
    if (stemIndex >= 0 && stemIndex < 4)
    {
        stemVolumes[stemIndex] = juce::jlimit (0.0f, 1.0f, volume);
        applyGains();
    }
}

float SubverseSplitterAudioProcessor::getStemVolume (int stemIndex) const
{
    return (stemIndex >= 0 && stemIndex < 4) ? stemVolumes[stemIndex].load() : 1.0f;
}

void SubverseSplitterAudioProcessor::setPlaybackPosition (double seconds)
{
    const double length = getTotalLength();
    const double target = juce::jlimit (0.0, juce::jmax (0.0, length), seconds);

    for (int i = 0; i < 4; ++i)
        if (transportSources[i] != nullptr)
            transportSources[i]->setPosition (target);
}

bool SubverseSplitterAudioProcessor::getStemMute(int stemIndex) const
{
    return (stemIndex >= 0 && stemIndex < 4) ? stemMutes[stemIndex].load() : false;
}

void SubverseSplitterAudioProcessor::setStemSolo(int stemIndex, bool shouldSolo)
{
    if (stemIndex >= 0 && stemIndex < 4)
    {
        stemSolos[stemIndex] = shouldSolo;
        applyGains();
    }
}

bool SubverseSplitterAudioProcessor::getStemSolo(int stemIndex) const
{
    return (stemIndex >= 0 && stemIndex < 4) ? stemSolos[stemIndex].load() : false;
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

SubverseSplitterAudioProcessor::ModelRunner::~ModelRunner()
{
    stopThread(3000);
}

void SubverseSplitterAudioProcessor::ModelRunner::loadModel()
{
    if (session != nullptr) return; // already loaded
    
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
        int threads = juce::SystemStats::getNumCpus();
       #if JUCE_MAC || JUCE_IOS
        {
            int performanceCores = 0;
            size_t size = sizeof (performanceCores);
            if (sysctlbyname ("hw.perflevel0.logicalcpu", &performanceCores, &size, nullptr, 0) == 0
                && performanceCores > 0)
                threads = performanceCores;
        }
       #endif
        sessionOptions.SetIntraOpNumThreads (juce::jmax (1, threads));
        sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        
        // Find the model file (Search in multiple locations to support Standalone, VST, and Dev environments)
        juce::File currentExe = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
        
        juce::File modelFile = currentExe.getParentDirectory().getChildFile("htdemucs.onnx"); // Same folder
        
        if (!modelFile.existsAsFile())
            modelFile = currentExe.getParentDirectory().getParentDirectory().getChildFile("Resources").getChildFile("htdemucs.onnx"); // Mac App Bundle
            
        if (!modelFile.existsAsFile())
            modelFile = currentExe.getParentDirectory().getParentDirectory().getParentDirectory().getChildFile("htdemucs.onnx"); // Build Artefacts folder
            
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
            logMessage("Error: could not find htdemucs.onnx. Searched: "
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
        
        Ort::MemoryInfo memInfoLocal = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        memoryInfo = std::make_unique<Ort::MemoryInfo>(std::move(memInfoLocal));
        
        logMessage("ONNX Model loaded successfully.");
    }
    catch (const Ort::Exception& e) {
        logMessage(juce::String("ONNX Exception: ") + e.what());
    }
}

void SubverseSplitterAudioProcessor::ModelRunner::startOperation (const juce::File& audioFile, Operation op)
{
    logMessage("--- startSeparation called ---");
    logMessage("Input File: " + audioFile.getFullPathName());
    logMessage("File Exists: " + juce::String(audioFile.existsAsFile() ? "true" : "false"));

    if (isThreadRunning())
    {
        logMessage("Error: Thread is already running!");
        return;
    }

    inputFile = audioFile;
    operation = op;
    {
        const juce::ScopedLock sl (analysisLock);
        analysis = {};
    }
    progress.set(0.0);
    finishedSuccess.set(false);
    errorMessage = "";

    // the run's folder and file names are decided once tempo and key are known (end of run(), ElekLibrary.h)
    vocalsFile = drumsFile = instrumentalFile = bassFile = otherFile = outputFolder = juce::File();

    startThread();
}

bool writeValidWavFile (const juce::File& file)
{
    file.deleteFile();
    
    juce::WavAudioFormat wavFormat;
    auto outStream = file.createOutputStream();
    if (outStream == nullptr)
        return false;
        
    std::unique_ptr<juce::AudioFormatWriter> writer (wavFormat.createWriterFor (outStream.get(), 44100.0, 2, 16, {}, 0));
    if (writer != nullptr)
    {
        outStream.release(); // Writer took ownership of the raw pointer
        
        // Write 1 second of silence (44100 samples)
        juce::AudioBuffer<float> buffer (2, 44100);
        buffer.clear();
        return writer->writeFromAudioSampleBuffer (buffer, 0, 44100);
    }
    
    return false;
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

void SubverseSplitterAudioProcessor::ModelRunner::runImpl()
{
    logMessage("run() background worker started");
    
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
    
    // We expect stereo. If mono, we will duplicate.
    int numChannels = 2; // Demucs always wants 2 channels
    int totalSamples = static_cast<int>(reader->lengthInSamples);
    
    juce::AudioBuffer<float> inputBuffer(numChannels, totalSamples);
    inputBuffer.clear();
    
    // Read audio data.
    bool readSuccess = reader->read(&inputBuffer, 0, totalSamples, 0, true, true);
    if (!readSuccess)
    {
        errorMessage = "This file could not be decoded to the end. Try exporting it again as WAV.";
        logMessage("Error: " + errorMessage);
        currentState.set(static_cast<int>(State::Finished));
        finishedSuccess.set(false);
        return;
    }
    
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

    // 1. Load the ONNX model (after the reading, so tempo and key are on screen while it loads)
    loadModel();
    if (session == nullptr)
    {
        if (errorMessage.isEmpty())
            errorMessage = "The separation engine could not start. Restart ELEK; if it happens again, reinstall it.";

        logMessage("Error: " + errorMessage);
        currentState.set(static_cast<int>(State::Finished));
        finishedSuccess.set(false);
        return;
    }

    if (threadShouldExit()) return;

    logMessage("Starting ONNX Audio Separation on: " + inputFile.getFullPathName());
    progress.set(0.3);

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

    // Output buffers for the 4 stems, plus the per-sample weight total the
    // overlap-add divides by at the end.
    juce::AudioBuffer<float> outDrums(2, totalSamples); outDrums.clear();
    juce::AudioBuffer<float> outBass(2, totalSamples); outBass.clear();
    juce::AudioBuffer<float> outOther(2, totalSamples); outOther.clear();
    juce::AudioBuffer<float> outVocals(2, totalSamples); outVocals.clear();
    std::vector<float> weightSum((size_t) totalSamples, 0.0f);

    std::vector<int64_t> inputDims = {1, 2, CHUNK_SIZE};
    const char* inputNames[] = {"mix"};
    const char* outputNames[] = {"stems"};
    
    // Allocate input tensor memory once
    std::vector<float> inputTensorValues(2 * CHUNK_SIZE, 0.0f);
    
    logMessage(juce::String("Processing ") + juce::String(numChunks) + " overlapping windows...");
    
    for (int i = 0; i < numChunks; ++i)
    {
        if (threadShouldExit()) return;
        
        const int startSample = windowStarts[(size_t) i];
        const int samplesToProcess = std::min(CHUNK_SIZE, totalSamples - startSample);
        
        // Clear input tensor (for zero padding the last chunk if needed)
        std::fill(inputTensorValues.begin(), inputTensorValues.end(), 0.0f);
        
        // Copy JUCE buffer into contiguous planar vector (Left channel then Right channel)
        for (int ch = 0; ch < 2; ++ch)
        {
            const float* channelData = inputBuffer.getReadPointer(std::min(ch, inputBuffer.getNumChannels() - 1), startSample);
            float* destination = inputTensorValues.data() + (ch * CHUNK_SIZE);
            for (int n = 0; n < samplesToProcess; ++n)
                destination[n] = (channelData[n] - referenceMean) / referenceStd;
        }
        
        // Create ONNX Tensor
        Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
            *memoryInfo, inputTensorValues.data(), inputTensorValues.size(), inputDims.data(), inputDims.size()
        );
        
        // RUN AI MODEL (Extremely CPU heavy)
        auto outputTensors = session->Run(Ort::RunOptions{nullptr}, inputNames, &inputTensor, 1, outputNames, 1);
        
        // Output tensor shape is [1, 4, 2, 343980]
        // Stems order: 0=Drums, 1=Bass, 2=Other, 3=Vocals
        const float* outData = outputTensors.front().GetTensorMutableData<float>();
        
        // Accumulate under the window weight rather than overwriting.
        for (int stemIdx = 0; stemIdx < 4; ++stemIdx)
        {
            juce::AudioBuffer<float>* targetBuffer = nullptr;
            if (stemIdx == 0) targetBuffer = &outDrums;
            else if (stemIdx == 1) targetBuffer = &outBass;
            else if (stemIdx == 2) targetBuffer = &outOther;
            else if (stemIdx == 3) targetBuffer = &outVocals;
            
            for (int ch = 0; ch < 2; ++ch)
            {
                const size_t offset = (size_t) (stemIdx * 2 * CHUNK_SIZE) + (size_t) (ch * CHUNK_SIZE);
                const float* source = outData + offset;
                float* destination = targetBuffer->getWritePointer(ch, startSample);
                for (int n = 0; n < samplesToProcess; ++n)
                    destination[n] += source[n] * windowWeight[(size_t) n];
            }
        }

        for (int n = 0; n < samplesToProcess; ++n)
            weightSum[(size_t) (startSample + n)] += windowWeight[(size_t) n];
        
        progress.set(0.1 + (0.8 * (static_cast<double>(i + 1) / numChunks)));
    }

    // Normalise by the accumulated weight and undo the standardisation. Every
    // sample is covered by at least one window, but the guard costs nothing and
    // a zero here would be a silent NaN in a stem.
    {
        juce::AudioBuffer<float>* stems[4] = { &outDrums, &outBass, &outOther, &outVocals };
        for (auto* stem : stems)
            for (int ch = 0; ch < 2; ++ch)
            {
                float* data = stem->getWritePointer(ch);
                for (int n = 0; n < totalSamples; ++n)
                {
                    const float weight = weightSum[(size_t) n] > 0.0f ? weightSum[(size_t) n] : 1.0f;
                    data[n] = (data[n] / weight) * referenceStd + referenceMean;
                }
            }
    }
    
    // Log the magnitude of the separated stems
    logMessage("Max Vocals Magnitude: " + juce::String(outVocals.getMagnitude(0, totalSamples)));
    logMessage("Max Drums Magnitude: " + juce::String(outDrums.getMagnitude(0, totalSamples)));
    logMessage("Max Bass Magnitude: " + juce::String(outBass.getMagnitude(0, totalSamples)));
    logMessage("Max Other Magnitude: " + juce::String(outOther.getMagnitude(0, totalSamples)));
    
    // 4. Write Separated Buffers to File
    // The backing track: the mix with the vocal removed. Built here rather
    // than left to the listener to reconstruct by muting one lane, because
    // muting the vocal in the mixer plays three stems whose sum is not the
    // mix — whatever the model could not place goes missing with it.
    juce::AudioBuffer<float> outInstrumental (2, totalSamples);
    for (int ch = 0; ch < 2; ++ch)
    {
        const int sourceChannel = std::min (ch, inputBuffer.getNumChannels() - 1);
        const float* mix = inputBuffer.getReadPointer (sourceChannel);
        const float* vocal = outVocals.getReadPointer (ch);
        float* destination = outInstrumental.getWritePointer (ch);
        for (int n = 0; n < totalSamples; ++n)
            destination[n] = mix[n] - vocal[n];
    }

    logMessage("Writing actual separated WAV files...");
    
    auto writeBufferToFile = [this](const juce::AudioBuffer<float>& buffer, const juce::File& file, double sampleRate) -> bool {
        file.deleteFile();
        juce::WavAudioFormat wavFormat;
        if (auto outStream = file.createOutputStream())
        {
            // 24-bit, not 16. Separation output routinely exceeds the
            // input's peak — the model estimates a source rather than carving
            // up a fixed budget — so 16-bit clipped material that never
            // clipped going in.
            std::unique_ptr<juce::AudioFormatWriter> writer(wavFormat.createWriterFor(outStream.get(), sampleRate, 2, 24, {}, 0));
            if (writer != nullptr)
            {
                outStream.release();
                return writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
            }
        }
        return false;
    };

    const double sr = modelRate;   // the buffers are at the model's rate (resampled above when the file was not)

    // ELEK's own output (ElekLibrary.h): ~/Music/ELEK/<title>_<bpm>BPM_<key>_<date>/<title>_<Stem>_<bpm>BPM_<key>_ELEK.wav,
    // with elek.json beside them so the window can list and reopen the run later
    AudioAnalysis measured;
    {
        const juce::ScopedLock sl (analysisLock);
        measured = analysis;
    }
    const auto title = ElekLibrary::shortTitle (inputFile);
    const auto folder = ElekLibrary::newRunFolder (title, measured);
    auto named = [&] (const char* stem) { return folder.getChildFile (ElekLibrary::stemFileName (title, stem, measured)); };
    vocalsFile = named ("Vocals");
    instrumentalFile = named ("Instrumental");
    const bool allStems = operation == Operation::Stems;
    drumsFile = allStems ? named ("Drums") : juce::File();
    bassFile  = allStems ? named ("Bass")  : juce::File();
    otherFile = allStems ? named ("Other") : juce::File();
    outputFolder = folder;

    bool ok = writeBufferToFile (outVocals, vocalsFile, sr) && writeBufferToFile (outInstrumental, instrumentalFile, sr);
    if (allStems)
        ok = ok && writeBufferToFile (outDrums, drumsFile, sr) && writeBufferToFile (outBass, bassFile, sr)
                && writeBufferToFile (outOther, otherFile, sr);

    if (! ok)
    {
        errorMessage = "Could not write the stems to " + folder.getFullPathName() + ". Check that the disk has room.";
        logMessage("Error: " + errorMessage);
        currentState.set(static_cast<int>(State::Finished));
        finishedSuccess.set(false);
        progress.set(0.0);
        return;
    }

    ElekLibrary::Entry entry;
    entry.folder = folder;
    entry.title = title;
    entry.source = inputFile.getFullPathName();
    entry.operation = allStems ? "stem_separation" : "vocal_removal";
    entry.created = juce::Time::getCurrentTime();
    entry.analysis = measured;
    entry.files.set ("vocals", vocalsFile.getFileName());
    entry.files.set ("instrumental", instrumentalFile.getFileName());
    if (allStems)
    {
        entry.files.set ("drums", drumsFile.getFileName());
        entry.files.set ("bass", bassFile.getFileName());
        entry.files.set ("other", otherFile.getFileName());
    }
    ElekLibrary::writeManifest (entry);
    logMessage ("Wrote " + folder.getFullPathName());

    logMessage("Real DSP Separation finished successfully! Files mapped.");
    progress.set(1.0);
    currentState.set(static_cast<int>(State::Finished));
    finishedSuccess.set(true);
}

// This creates the instantiator for JUCE plugin
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SubverseSplitterAudioProcessor();
}
