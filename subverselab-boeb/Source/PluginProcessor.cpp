#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
const juce::StringArray kMoodNames { "CLEAN", "PUSH", "90S", "RADIO", "PHONE", "MEGAPHONE" };
constexpr float kSignalPeak = 0.0032f;   // -50 dBFS: a block quieter than this is not heard (LISTEN, NOW)
constexpr int kScratch = 2048;
constexpr float kRelistenRatio = 4.0f;   // +12 dB over the loudest block LISTEN heard           // the engine runs in chunks of at most this many samples

float blockPeak (const float* const* ch, int numCh, int start, int n)
{
    float pk = 0.0f;
    for (int c = 0; c < numCh; ++c)
        for (int i = 0; i < n; ++i)
            pk = std::max (pk, std::abs (ch[c][start + i]));
    return pk;
}
}

BoebProcessor::BoebProcessor()
    : AudioProcessor (BusesProperties()
                        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                        .withInput  ("Beat",   juce::AudioChannelSet::stereo(), false)),
      apvts (*this, nullptr, "Parameters", createLayout())
{
    worker.startThread (juce::Thread::Priority::low);
    startTimerHz (10);
}

BoebProcessor::~BoebProcessor()
{
    stopTimer();
    worker.stopThread (2000);
}

juce::AudioProcessorValueTreeState::ParameterLayout BoebProcessor::createLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    auto db = [] (int dec) { return juce::AudioParameterFloatAttributes().withLabel ("dB")
                                   .withStringFromValueFunction ([dec] (float v, int) { return (v > 0.005f ? "+" : "") + juce::String (v, dec); }); };

    // LIFT 6 = what LISTEN found; 0 = untouched.
    p.push_back (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "lift", 1 }, "Lift",
        juce::NormalisableRange<float> (0.0f, 10.0f, 0.1f), 6.0f,
        juce::AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return juce::String (v, 1); })));
    p.push_back (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "mood", 1 }, "Mood", kMoodNames, 0));
    p.push_back (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "body", 1 }, "Body",
        juce::NormalisableRange<float> (-9.0f, 9.0f, 0.1f), 0.0f, db (1)));
    p.push_back (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "air", 1 }, "Air",
        juce::NormalisableRange<float> (0.0f, 12.0f, 0.1f), 0.0f, db (1)));
    p.push_back (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "space", 1 }, "Space",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("%")
            .withStringFromValueFunction ([] (float v, int) { return juce::String (juce::roundToInt (v * 100.0f)); })));
    p.push_back (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "output", 1 }, "Output",
        juce::NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f, db (1)));
    // DRY/WET (owner 7 Nov: "listen boostu fazla gelirse diye"): 100 % = all BOE-B, 0 % = the input.
    p.push_back (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "drywet", 1 }, "Dry/Wet",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 1.0f,
        juce::AudioParameterFloatAttributes().withLabel ("%")
            .withStringFromValueFunction ([] (float v, int) { return juce::String (juce::roundToInt (v * 100.0f)); })));
    p.push_back (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "throw", 1 }, "Throw", false));
    p.push_back (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "equal", 1 }, "Equal", false));
    p.push_back (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "in", 1 }, "In", true));
    return { p.begin(), p.end() };
}

bool BoebProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in = layouts.getMainInputChannelSet();
    if (in != layouts.getMainOutputChannelSet()) return false;
    if (in != juce::AudioChannelSet::mono() && in != juce::AudioChannelSet::stereo()) return false;
    if (layouts.inputBuses.size() > 1)
    {
        const auto& sc = layouts.getChannelSet (true, 1);
        if (! sc.isDisabled() && sc != juce::AudioChannelSet::mono() && sc != juce::AudioChannelSet::stereo()) return false;
    }
    return true;
}

void BoebProcessor::prepareToPlay (double sampleRate, int)
{
    worker.stopThread (2000);
    fs = sampleRate > 0 ? sampleRate : 48000.0;
    engine.prepare (fs, kScratch);
    listenAn.prepare (fs, kListenSec + 1.0);
    for (auto& a : nowAn) a.prepare (fs, kListenSec + 1.0);
    listenMono.assign ((size_t) std::ceil ((kListenSec + 1.0) * fs), 0.0f);
    listenMonoPos = 0;
    if (state.load() != 0) { state = 0; listenRequest = true; }   // a listen cut short by a re-prepare starts again
    heard = 0.0f;
    nowWrite = 0; nowPending = -1;
    setLatencySamples (engine.getLatencySamples());
    worker.startThread (juce::Thread::Priority::low);
}

BoebProcessor::Engine::Parameters BoebProcessor::readParams() const noexcept
{
    Engine::Parameters p;
    p.lift     = apvts.getRawParameterValue ("lift")->load();
    p.mood     = (int) apvts.getRawParameterValue ("mood")->load();
    p.bodyDb   = apvts.getRawParameterValue ("body")->load();
    p.airDb    = apvts.getRawParameterValue ("air")->load();
    p.space    = apvts.getRawParameterValue ("space")->load();
    p.outputDb = apvts.getRawParameterValue ("output")->load();
    p.dryWet   = apvts.getRawParameterValue ("drywet")->load();
    p.throwOn  = apvts.getRawParameterValue ("throw")->load() > 0.5f;
    p.equal    = apvts.getRawParameterValue ("equal")->load() > 0.5f;
    p.enabled  = apvts.getRawParameterValue ("in")->load() > 0.5f;
    return p;
}

void BoebProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    auto main = getBusBuffer (buffer, true, 0);
    const int numCh = std::min (2, main.getNumChannels());
    const int n = buffer.getNumSamples();
    if (numCh == 0 || n == 0) return;

    const float* key[2] = { nullptr, nullptr };
    int keyCh = 0;
    if (getBusCount (true) > 1 && getBus (true, 1) != nullptr && getBus (true, 1)->isEnabled())
    {
        auto side = getBusBuffer (buffer, true, 1);
        keyCh = std::min (2, side.getNumChannels());
        for (int c = 0; c < keyCh; ++c) key[c] = side.getReadPointer (c);
    }
    beatSeen = keyCh > 0 && blockPeak (key, keyCh, 0, n) > 1.0e-4f;

    if (settingsSerial.load() != liveSerial && settingsLock.tryEnter())
    {
        liveSettings = pendingSettings;
        liveSerial = settingsSerial.load();
        settingsLock.exit();
    }
    auto p = readParams();
    p.listen = liveSettings;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
            if (auto bpm = pos->getBpm()) p.bpm = *bpm;

    // LISTEN: a request (the button, or the first audio of a fresh instance) resets the analyser here, where it is fed.
    // A take often starts with bleed or room noise well under the voice (owner's take 0004: 10 s at -25 dBFS peak, then
    // the vocal at -1): when audio comes in 12 dB over everything LISTEN has heard, it was not the vocal, so LISTEN
    // starts again, while listening or (once) after it finished.
    const float inPeak = blockPeak (main.getArrayOfReadPointers(), numCh, 0, n);
    const bool louder = listenPeak > 0.0f && inPeak > listenPeak * kRelistenRatio;
    const bool restart  = state.load() == 1 && louder;                       // while listening: always
    const bool byLouder = state.load() == 0 && relistenArmed && louder;      // after it: once
    const bool byAsk    = state.load() == 0 && listenRequest.exchange (false);
    const bool byAuto   = state.load() == 0 && autoListen.load() && inPeak > kSignalPeak;
    if (restart || byLouder || byAsk || byAuto)
    {
        relistenArmed = ! byLouder;
        autoListen = false;
        listenAn.reset();
        listenMonoPos = 0;
        listenPeak = 0.0f;
        heard = 0.0f;
        state = 1;
    }

    // In chunks through a copy of the input (on the stack, 16 kB): the engine reads in[] and writes out[] separately.
    float inL[kScratch], inR[kScratch];
    for (int start = 0; start < n; start += kScratch)
    {
        const int len = std::min (kScratch, n - start);
        float* io[2] = { main.getWritePointer (0) + start, main.getWritePointer (numCh > 1 ? 1 : 0) + start };
        std::copy (io[0], io[0] + len, inL);
        if (numCh > 1) std::copy (io[1], io[1] + len, inR);
        const float* in[2] = { inL, numCh > 1 ? inR : inL };
        const float* k[2] = { key[0] ? key[0] + start : nullptr, key[1] ? key[1] + start : nullptr };

        if (state.load() == 1 && blockPeak (in, numCh, 0, len) > kSignalPeak)
        {
            listenPeak = std::max (listenPeak, blockPeak (in, numCh, 0, len));
            listenAn.feed (in, numCh, keyCh > 0 ? k : nullptr, keyCh, len);
            for (int i = 0; i < len && listenMonoPos < listenMono.size(); ++i)
                listenMono[listenMonoPos++] = numCh > 1 ? 0.5f * (in[0][i] + in[1][i]) : in[0][i];
            heard = (float) listenAn.secondsFed();
            if (listenAn.secondsFed() >= kListenSec) state = 2;
        }

        engine.process (in, io, numCh, keyCh > 0 ? k : nullptr, keyCh, len, p);

        // NOW: the processed output, kListenSec at a time, two analysers taking turns.
        if (blockPeak (io, numCh, 0, len) > kSignalPeak)
        {
            int w = nowWrite.load();
            const float* o[2] = { io[0], io[1] };
            nowAn[w].feed (o, numCh, keyCh > 0 ? k : nullptr, keyCh, len);
            if (nowAn[w].secondsFed() >= kListenSec && nowPending.load() < 0)
            {
                nowPending = w;
                w = 1 - w;
                nowAn[w].reset();
                nowWrite = w;
            }
        }
    }
}

void BoebProcessor::processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&)
{
    beatSeen = false;   // latency 0: the host's bypass passes the audio as it is
}

//==============================================================================
void BoebProcessor::publishSettings (const Settings& s)
{
    {
        const juce::SpinLock::ScopedLockType l (settingsLock);
        pendingSettings = s;
    }
    ++settingsSerial;
}

void BoebProcessor::work()
{
    if (state.load() == 2)
    {
        const Diagnosis d = listenAn.diagnose();
        Settings s = Analyser::settingsFor (d);
        if (s.bandLimited)
            svl::boeb::calibrateExciter (s, listenMono.data(), listenMonoPos, fs);
        if (d.valid)
        {
            publishSettings (s);
            const juce::SpinLock::ScopedLockType l (readLock);
            read.found = d;
            read.settings = s;
            read.haveFound = true;
            ++read.foundSerial;
            listenAir = s.airDb;
            airFromListen = true;
        }
        state = 0;
    }
    const int pnd = nowPending.load();
    if (pnd >= 0)
    {
        const Diagnosis d = nowAn[pnd].diagnose();
        if (d.valid)
        {
            const juce::SpinLock::ScopedLockType l (readLock);
            read.now = d;
            read.haveNow = true;
            ++read.nowSerial;
        }
        nowPending = -1;
    }
}

void BoebProcessor::finishPendingWorkNow()
{
    worker.stopThread (2000);
    work();
    worker.startThread (juce::Thread::Priority::low);
}

void BoebProcessor::startListen()
{
    if (state.load() == 1) state = 0;   // restart
    listenRequest = true;
}

BoebProcessor::Readout BoebProcessor::readout() const
{
    const juce::SpinLock::ScopedLockType l (readLock);
    return read;
}

float BoebProcessor::listenedAirDb() const
{
    const juce::SpinLock::ScopedLockType l (readLock);
    return read.haveFound ? listenAir : 0.0f;
}

void BoebProcessor::timerCallback()
{
    // LISTEN writes its AIR into the AIR knob (the knob glides there on the panel). Not a user edit: no undo step.
    if (airFromListen.exchange (false))
        if (auto* prm = apvts.getParameter ("air"))
            prm->setValueNotifyingHost (prm->convertTo0to1 (listenedAirDb()));
}

//==============================================================================
namespace
{
void putDiag (juce::ValueTree& t, const svl::boeb::Diagnosis& d)
{
    t.setProperty ("valid", d.valid, nullptr);   t.setProperty ("hasBeat", d.hasBeat, nullptr);
    t.setProperty ("lar", d.larDb, nullptr);     t.setProperty ("spread", d.spreadDb, nullptr);
    t.setProperty ("pres", d.presDb, nullptr);   t.setProperty ("mud", d.mudDb, nullptr);
    t.setProperty ("airD", d.airDb, nullptr);    t.setProperty ("vocal", d.vocalLevelDb, nullptr);
}
void getDiag (const juce::ValueTree& t, svl::boeb::Diagnosis& d)
{
    d.valid = t.getProperty ("valid", false);    d.hasBeat = t.getProperty ("hasBeat", false);
    d.larDb = t.getProperty ("lar", 0.0f);       d.spreadDb = t.getProperty ("spread", 0.0f);
    d.presDb = t.getProperty ("pres", 0.0f);     d.mudDb = t.getProperty ("mud", 0.0f);
    d.airDb = t.getProperty ("airD", 0.0f);      d.vocalLevelDb = t.getProperty ("vocal", -300.0f);
}
}

void BoebProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto st = apvts.copyState();
    st.removeChild (st.getChildWithName ("LISTEN"), nullptr);
    const auto r = readout();
    if (r.haveFound)
    {
        juce::ValueTree t ("LISTEN");
        const auto& s = r.settings;
        t.setProperty ("hpf", s.hpfHz, nullptr);          t.setProperty ("presence", s.presenceDb, nullptr);
        t.setProperty ("mudcut", s.mudCutDb, nullptr);    t.setProperty ("air", s.airDb, nullptr);
        t.setProperty ("compneed", s.compNeed, nullptr);  t.setProperty ("band", s.bandLimited, nullptr);
        t.setProperty ("cut", s.cutHz, nullptr);          t.setProperty ("excite", s.exciteDb, nullptr);
        putDiag (t, r.found);
        st.appendChild (t, nullptr);
    }
    std::unique_ptr<juce::XmlElement> xml (st.createXml());
    copyXmlToBinary (*xml, destData);
}

void BoebProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType())) return;
    auto vt = juce::ValueTree::fromXml (*xml);
    const auto t = vt.getChildWithName ("LISTEN");
    vt.removeChild (t, nullptr);
    apvts.replaceState (vt);
    undo.clear();
    if (t.isValid())
    {
        Settings s;
        s.hpfHz = t.getProperty ("hpf", 80.0f);          s.presenceDb = t.getProperty ("presence", 0.0f);
        s.mudCutDb = t.getProperty ("mudcut", 0.0f);     s.airDb = t.getProperty ("air", 0.0f);
        s.compNeed = t.getProperty ("compneed", 1.0f);   s.bandLimited = t.getProperty ("band", false);
        s.cutHz = t.getProperty ("cut", 0.0f);           s.exciteDb = t.getProperty ("excite", 0.0f);
        publishSettings (s);
        const juce::SpinLock::ScopedLockType l (readLock);
        getDiag (t, read.found);
        read.settings = s;
        read.haveFound = true;
        ++read.foundSerial;
        listenAir = s.airDb;
    }
    autoListen = ! t.isValid();   // a saved session already knows its vocal
}

juce::AudioProcessorEditor* BoebProcessor::createEditor()
{
    return new BoebEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new BoebProcessor();
}
