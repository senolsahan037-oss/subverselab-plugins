#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "ParamText.h"

namespace
{
using juce::NormalisableRange;
using juce::ParameterID;

constexpr int kScratch = 4096;   // the processor works in chunks of at most this many samples (a larger host block never allocates)

auto fromValue (const juce::String& id)
{
    return [id] (float v, int) { return dyneq::ptext::format (id, v); };
}
auto toValue (const juce::String& id, float fallback)
{
    return [id, fallback] (const juce::String& t) { float v; return dyneq::ptext::parse (id, t, v) ? v : fallback; };
}
juce::String bandId (int i, const char* name) { return "b" + juce::String (i + 1) + "_" + name; }
}

juce::AudioProcessorValueTreeState::ParameterLayout DyneqAudioProcessor::createLayout()
{
    using F = juce::AudioParameterFloat;
    using B = juce::AudioParameterBool;
    using C = juce::AudioParameterChoice;
    using A = juce::AudioParameterFloatAttributes;
    using CA = juce::AudioParameterChoiceAttributes;
    const dyneq::Params d;   // engine defaults: nothing here decides a sound, the P2 listening may still change them
    const dyneq::FreeBand fd;
    auto attr = [] (const juce::String& id, float fallback) { return A().withStringFromValueFunction (fromValue (id)).withValueFromStringFunction (toValue (id, fallback)); };
    auto choiceAttr = [] (const juce::String& id)
    {
        return CA().withStringFromValueFunction ([id] (int i, int) { return dyneq::ptext::format (id, (float) i); })
                   .withValueFromStringFunction ([id] (const juce::String& t) { int i = 0; return dyneq::ptext::parseChoice (id, t, i) ? i : 0; });
    };
    auto fl = [&] (const juce::String& id, const char* name, NormalisableRange<float> r, float def)
    {
        return std::make_unique<F> (ParameterID { id, 1 }, name, r, def, attr (id, def));
    };
    auto ch = [&] (const juce::String& id, const char* name, int def)
    {
        return std::make_unique<C> (ParameterID { id, 1 }, name, dyneq::ptext::choiceNames (id), def, choiceAttr (id));
    };
    auto skewed = [] (float lo, float hi, float step, float centre) { NormalisableRange<float> r (lo, hi, step); r.setSkewForCentre (centre); return r; };

    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    p.push_back (ch ("mode", "MODE", d.mode));
    p.push_back (fl ("depth", "DEPTH", NormalisableRange<float> (0.0f, 12.0f, 0.1f), d.depthDb));
    p.push_back (fl ("margin", "MARGIN / SENS", NormalisableRange<float> (0.0f, 12.0f, 0.1f), d.marginDb));   // COLLIDE: MARGIN (0 .. 9 used), SELF: SENS (0 .. 12)
    p.push_back (fl ("focusLow", "FOCUS LOW", skewed (40.0f, 16000.0f, 1.0f, 800.0f), d.focusLowHz));   // 25 Oct: 30 Hz / min 20 measured and not kept (SELF got worse), see DyneqEngine.h Params::focusLowHz
    p.push_back (fl ("focusHigh", "FOCUS HIGH", skewed (40.0f, 16000.0f, 1.0f, 800.0f), d.focusHighHz));
    p.push_back (ch ("speed", "SPEED", d.speed));
    p.push_back (fl ("mix", "MIX", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f));
    p.push_back (std::make_unique<B> (ParameterID { "delta", 1 }, "DELTA", d.delta));
    p.push_back (std::make_unique<B> (ParameterID { "bypass", 1 }, "BYPASS", false));
    p.push_back (std::make_unique<B> (ParameterID { "keyListen", 1 }, "KEY LISTEN", false));
    p.push_back (std::make_unique<B> (ParameterID { "tiling", 1 }, "TILING", d.tiling));   // SPEC 7.3 A/B, pending the owner's ear
    p.push_back (std::make_unique<B> (ParameterID { "bakeArm", 1 }, "BAKE ARM", false));
    for (int i = 0; i < dyneq::kFreeBands; ++i)
    {
        const juce::String n = "B" + juce::String (i + 1) + " ";
        p.push_back (std::make_unique<B> (ParameterID { bandId (i, "on"), 1 }, n + "ON", fd.on));
        p.push_back (ch (bandId (i, "type"), (n + "TYPE").toRawUTF8(), fd.type));
        static constexpr float kDefaultHz[6] { 80.0f, 200.0f, 500.0f, 1200.0f, 3000.0f, 8000.0f };   // UX redesign 11 Oct: six distinct starting points, so a new band is never hidden under another at 1 kHz
        p.push_back (fl (bandId (i, "freq"), (n + "FREQ").toRawUTF8(), skewed (20.0f, 20000.0f, 0.01f, 1000.0f), i < 6 ? kDefaultHz[i] : fd.freqHz));
        p.push_back (fl (bandId (i, "gain"), (n + "GAIN").toRawUTF8(), NormalisableRange<float> (-18.0f, 18.0f, 0.01f), fd.gainDb));
        p.push_back (fl (bandId (i, "q"), (n + "Q").toRawUTF8(), skewed (0.1f, 18.0f, 0.01f, 1.0f), fd.q));
        p.push_back (ch (bandId (i, "slope"), (n + "SLOPE").toRawUTF8(), 0));
        p.push_back (std::make_unique<B> (ParameterID { bandId (i, "dyn"), 1 }, n + "DYNAMIC", fd.dyn));
        p.push_back (fl (bandId (i, "thr"), (n + "THRESHOLD").toRawUTF8(), NormalisableRange<float> (-60.0f, 0.0f, 0.1f), fd.thrDb));
        p.push_back (fl (bandId (i, "range"), (n + "RANGE").toRawUTF8(), NormalisableRange<float> (-12.0f, 12.0f, 0.1f), fd.rangeDb));
        p.push_back (fl (bandId (i, "att"), (n + "ATTACK").toRawUTF8(), skewed (1.0f, 100.0f, 0.1f, 10.0f), fd.attackMs));
        p.push_back (fl (bandId (i, "rel"), (n + "RELEASE").toRawUTF8(), skewed (10.0f, 1000.0f, 1.0f, 100.0f), fd.releaseMs));
        p.push_back (ch (bandId (i, "trig"), (n + "TRIGGER").toRawUTF8(), fd.trigger));
    }
    // TARGET (SPEC 8): off unless the user picks an INSTRUMENT
    p.push_back (ch ("targetInst", "TARGET INSTRUMENT", d.targetInst));
    p.push_back (fl ("targetPart", "TARGET PART", NormalisableRange<float> (0.0f, 100.0f, 0.1f), d.targetPart));
    p.push_back (fl ("targetAmount", "TARGET AMOUNT", NormalisableRange<float> (0.0f, dyneq::kTargetAmountMaxDb, 0.1f), d.targetAmount));   // P1i: 0 .. 60 dB; the top is a real high-pass / low-pass on an edge cut (a bell is capped at 18)
    p.push_back (fl ("targetWidth", "TARGET WIDTH", skewed (0.5f, 2.0f, 0.01f, 1.0f), d.targetWidth));
    p.push_back (fl ("targetSweet", "TARGET SWEET", NormalisableRange<float> (20.0f, 400.0f, 1.0f), d.targetSweet));
    p.push_back (std::make_unique<B> (ParameterID { "reverbReady", 1 }, "REVERB-READY", d.reverbReady));
    p.push_back (ch ("targetSlope", "TARGET SLOPE", dyneq::slopeIndex (d.targetSlope)));   // P1i (SPEC 8e.2): 12 / 24 / 36 / 48 / 72 dB per octave of the edge cuts
    // P4 (appended, so every older parameter keeps its place): OUTPUT, COLLIDE / SELF lamp, level-matched bypass. The P4 "face" choice (LAB / ORBIT) was removed
    // with the ORBIT face on 8 Nov 2026 (1.0.0): a state saved by a tester build still holds it and loads with it ignored (setStateInformation).
    p.push_back (fl ("output", "OUTPUT", NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f));
    p.push_back (std::make_unique<B> (ParameterID { "assistOff", 1 }, "ASSIST OFF", false));       // true: neither COLLIDE nor SELF cuts (the lamp of the layer MODE points at is dark)
    p.push_back (std::make_unique<B> (ParameterID { "levelMatch", 1 }, "BYPASS LEVEL MATCH", true)); // BYPASS plays the dry signal at the loudness the processing has
    // P4c: the analyzer (what the display shows: nothing, the input, the output, or both); POST only by default (UX redesign 11 Oct: one trace). A panel preference: saved, not automatable, never touches the audio.
    p.push_back (std::make_unique<C> (ParameterID { "analyzer", 1 }, "ANALYZER", dyneq::ptext::choiceNames ("analyzer"), 2, CA().withAutomatable (false).withStringFromValueFunction ([] (int i, int) { return dyneq::ptext::format ("analyzer", (float) i); })));
    return { p.begin(), p.end() };
}

DyneqAudioProcessor::DyneqAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",     juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output",    juce::AudioChannelSet::stereo(), true)
                          .withInput  ("Sidechain", juce::AudioChannelSet::stereo(), false)),
      apvts (*this, nullptr, "DYNEQ", createLayout())
{
    auto raw = [this] (const juce::String& id) { auto* r = apvts.getRawParameterValue (id); jassert (r != nullptr); return r; };
    ptr.mode = raw ("mode"); ptr.depth = raw ("depth"); ptr.margin = raw ("margin"); ptr.focusLow = raw ("focusLow"); ptr.focusHigh = raw ("focusHigh");
    ptr.speed = raw ("speed"); ptr.mix = raw ("mix"); ptr.delta = raw ("delta"); ptr.bypass = raw ("bypass"); ptr.keyListen = raw ("keyListen");
    ptr.tiling = raw ("tiling"); ptr.bakeArm = raw ("bakeArm");
    ptr.output = raw ("output"); ptr.assistOff = raw ("assistOff"); ptr.levelMatch = raw ("levelMatch");
    ptr.targetInst = raw ("targetInst"); ptr.targetPart = raw ("targetPart"); ptr.targetAmount = raw ("targetAmount"); ptr.targetWidth = raw ("targetWidth"); ptr.targetSweet = raw ("targetSweet"); ptr.reverbReady = raw ("reverbReady"); ptr.targetSlope = raw ("targetSlope");
    for (int i = 0; i < dyneq::kFreeBands; ++i)
    {
        auto& b = ptr.band[i];
        b.on = raw (bandId (i, "on")); b.type = raw (bandId (i, "type")); b.freq = raw (bandId (i, "freq")); b.gain = raw (bandId (i, "gain"));
        b.q = raw (bandId (i, "q")); b.slope = raw (bandId (i, "slope")); b.dyn = raw (bandId (i, "dyn")); b.thr = raw (bandId (i, "thr"));
        b.range = raw (bandId (i, "range")); b.att = raw (bandId (i, "att")); b.rel = raw (bandId (i, "rel")); b.trig = raw (bandId (i, "trig"));
    }
    bypassParam = apvts.getParameter ("bypass");
    // A parameter's stored plain value is convertFrom0to1(normalised) after every write, but the default it starts with is the number given
    // to the constructor (1000 Hz starts as 999.99994 after the skew round trip). Write every default once, so a parameter that is put back on
    // its normalised value by an undo is the same float as one that was never touched (BAKE's undo is then bit-exact in the raw values too).
    for (auto* ap : getParameters()) ap->setValueNotifyingHost (ap->getDefaultValue());
    undo.ignore ("bakeArm"); undo.ignore ("delta"); undo.ignore ("analyzer");   // arming BAKE is not an edit of the sound
    for (auto& l : dry) l.assign ((size_t) kScratch, 0.0f);
    for (auto& v : bellView) v.store (0.0f);
    memSnapshot.reserve ((size_t) dyneq::Engine::memoryFloatsMax()); memPending.reserve ((size_t) dyneq::Engine::memoryFloatsMax());
}

const float* DyneqAudioProcessor::bellQs()
{
    static const std::array<float, dyneq::kBands> q = []
    {
        auto e = std::make_unique<dyneq::Engine>(); e->prepare (48000.0, 512);   // the bells' Q is fc / (scale x bandwidth): it does not depend on the sample rate
        std::array<float, dyneq::kBands> a {};
        for (int b = 0; b < dyneq::kBands; ++b) a[(size_t) b] = e->bellQs()[b];
        return a;
    }();
    return q.data();
}

bool DyneqAudioProcessor::copyTail (int tap, float* dest, int n) const noexcept
{
    if (tap < 0 || tap >= kTaps) return false;
    const uint32_t pos = tapPos[(size_t) tap].load (std::memory_order_acquire);
    if (n > kSpecN || pos < (uint32_t) n) return false;
    const auto& ring = tapRing[(size_t) tap];
    for (int i = 0; i < n; ++i) dest[i] = ring[(size_t) ((pos - (uint32_t) n + (uint32_t) i) % (uint32_t) kSpecN)].load (std::memory_order_relaxed);
    return true;
}

void DyneqAudioProcessor::clearLayerViews() noexcept
{
    for (auto& v : bellView) v.store (0.0f, std::memory_order_relaxed);
    for (auto& v : tbView) v.store (0.0f, std::memory_order_relaxed);
    for (auto& e : edgeView) { e.cutDb.store (0.0f, std::memory_order_relaxed); e.run.store (false, std::memory_order_relaxed); e.mix.store (0.0f, std::memory_order_relaxed); }
    for (auto& l : bandLayer) { l.conflict.store (0.0f, std::memory_order_relaxed); l.self.store (0.0f, std::memory_order_relaxed); l.target.store (0.0f, std::memory_order_relaxed); l.net.store (0.0f, std::memory_order_relaxed); }
    for (auto& h : hunterView) h[1].store (0.0f, std::memory_order_relaxed);
    for (auto& f : freeView) f.store (0.0f, std::memory_order_relaxed);
}

const float* DyneqAudioProcessor::bandCentresHz()
{
    static const std::array<float, dyneq::kBands> fc = []
    {
        std::array<float, dyneq::kBands> a {};
        const float e0 = dyneq::erbRate (dyneq::kLowHz), e1 = dyneq::erbRate (dyneq::kHighHz), step = (e1 - e0) / (float) (dyneq::kBands - 1);
        for (int b = 0; b < dyneq::kBands; ++b) a[(size_t) b] = dyneq::erbToHz (e0 + step * (float) b);
        return a;
    }();
    return fc.data();
}

bool DyneqAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo()) return false;
    if (layouts.getMainInputChannelSet() != out) return false;
    const auto& sc = layouts.getChannelSet (true, 1);
    return sc.isDisabled() || sc == juce::AudioChannelSet::mono() || sc == juce::AudioChannelSet::stereo();
}

void DyneqAudioProcessor::prepareToPlay (double sampleRate, int)
{
    sampleRateHz = sampleRate;
    engine.prepare (sampleRate, kScratch);
    engine.bakeReset();
    setLatencySamples (0);   // the engine is minimum phase and has no delay line
    for (auto& l : dry) l.assign ((size_t) kScratch, 0.0f);
    for (auto* s : { &blendDry, &blendWet, &listenAmount }) s->reset (sampleRate, 0.020);
    outGain.reset (sampleRate, 0.020);
    outGain.setCurrentAndTargetValue (1.0f); msIn = msWet = 0.0; gridCount = 0; gridIn = gridWet = 0.0; gMatch = matchCur = matchMid = 1.0f;
    cMatch = 1.0f - (float) std::exp (-1.0 / (0.01 * sampleRate)); a64 = 1.0 - std::exp (-64.0 / (0.4 * sampleRate));
    blendDry.setCurrentAndTargetValue (1.0f); blendWet.setCurrentAndTargetValue (1.0f); listenAmount.setCurrentAndTargetValue (0.0f);
    firstBlock = true;
    wasArmed = false;
}

void DyneqAudioProcessor::reset()
{
    // the same cold start as prepareToPlay, without re-allocating: the TARGET memory (the learned sounds) is kept, as it is across prepareToPlay
    engine.reset();
    for (auto* s : { &blendDry, &blendWet, &listenAmount }) s->reset (sampleRateHz, 0.020);
    outGain.reset (sampleRateHz, 0.020); outGain.setCurrentAndTargetValue (1.0f);
    msIn = msWet = 0.0; gridCount = 0; gridIn = gridWet = 0.0; gMatch = matchCur = matchMid = 1.0f;
    blendDry.setCurrentAndTargetValue (1.0f); blendWet.setCurrentAndTargetValue (1.0f); listenAmount.setCurrentAndTargetValue (0.0f);
    firstBlock = true;
}

bool DyneqAudioProcessor::sidechainBusEnabled() const
{
    const auto* bus = getBus (true, 1);
    return bus != nullptr && bus->isEnabled() && bus->getNumberOfChannels() > 0;
}

dyneq::Params DyneqAudioProcessor::readParams() const noexcept
{
    dyneq::Params p;
    p.mode        = juce::jlimit (0, 1, juce::roundToInt (ptr.mode->load()));
    p.depthDb     = ptr.assistOff->load() > 0.5f ? 0.0f : ptr.depth->load();   // the layer lamp is dark: no depth, no cut (the engine skips a bell at 0 dB)
    p.marginDb    = ptr.margin->load();
    if (p.mode == dyneq::kCollide) p.marginDb = juce::jmin (p.marginDb, 9.0f);   // SPEC 2: MARGIN is 0 .. 9; the knob goes to 12 for SELF's SENS
    p.focusLowHz  = ptr.focusLow->load();
    p.focusHighHz = ptr.focusHigh->load();
    p.speed       = juce::jlimit (0, 2, juce::roundToInt (ptr.speed->load()));
    p.mix         = 1.0f;     // MIX and DELTA are crossfaded here (smoothly), the engine always delivers the wet signal
    p.delta       = false;
    p.tiling      = ptr.tiling->load() > 0.5f;
    p.targetInst   = juce::jlimit (0, dyneq::kTargetInsts - 1, juce::roundToInt (ptr.targetInst->load()));
    p.targetPart  = ptr.targetPart->load();
    p.targetAmount = ptr.targetAmount->load();
    p.targetWidth  = ptr.targetWidth->load();
    p.targetSweet  = ptr.targetSweet->load();
    p.reverbReady  = ptr.reverbReady->load() > 0.5f;
    p.targetSlope  = dyneq::kSlopeDb[juce::jlimit (0, dyneq::kSlopeSteps - 1, juce::roundToInt (ptr.targetSlope->load()))];
    for (int i = 0; i < dyneq::kFreeBands; ++i)
    {
        const auto& s = ptr.band[i]; auto& b = p.band[i];
        b.on        = s.on->load() > 0.5f;
        b.type      = juce::jlimit (0, 5, juce::roundToInt (s.type->load()));
        b.freqHz    = s.freq->load();
        b.gainDb    = s.gain->load();
        b.q         = s.q->load();
        b.slopeDb   = dyneq::freeSlopeDbFromIndex (juce::roundToInt (s.slope->load()));   // choice order 12 / 24 / 48 / 36 / 72: the first three are the old states
        b.dyn       = s.dyn->load() > 0.5f;
        b.thrDb     = s.thr->load();
        b.rangeDb   = s.range->load();
        b.attackMs  = s.att->load();
        b.releaseMs = s.rel->load();
        b.trigger   = s.trig->load() > 0.5f ? dyneq::kTrigKey : dyneq::kTrigSelf;
    }
    return p;
}

void DyneqAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) { processImpl (buffer, false); }

void DyneqAudioProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) { processImpl (buffer, true); }

void DyneqAudioProcessor::processImpl (juce::AudioBuffer<float>& buffer, bool hostBypassed)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    const int nch = juce::jlimit (1, 2, getMainBusNumOutputChannels());
    if (n <= 0 || buffer.getNumChannels() < nch) return;

    const bool scOn = sidechainBusEnabled();
    scConnected.store (scOn, std::memory_order_relaxed);

#if SVL_LICENSING
    if (! license.isLicensed()) return;   // no licence, no processing: the audio passes through untouched
#endif
    // a NaN / Inf from the host or an upstream plug-in would live on in every filter state and silence the plug-in until the
    // next prepareToPlay (plugin_check 13.6): non-finite input samples (main and key) become 0 before anything reads them
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        float* x = buffer.getWritePointer (ch);
        for (int i = 0; i < n; ++i) if (! std::isfinite (x[i])) x[i] = 0.0f;
    }
    // the panel's meters and the spectrum: the input as it arrives (mono sum), before anything touches it
    double sumIn = 0.0;
    {
        uint32_t wp = tapPos[kTapPre].load (std::memory_order_relaxed);
        const float* l = buffer.getReadPointer (0); const float* r = nch > 1 ? buffer.getReadPointer (1) : nullptr;
        auto& ring = tapRing[kTapPre];
        for (int i = 0; i < n; ++i)
        {
            const float m = r != nullptr ? 0.5f * (l[i] + r[i]) : l[i];
            sumIn += (double) l[i] * l[i] + (r != nullptr ? (double) r[i] * r[i] : 0.0);
            ring[(size_t) (wp % (uint32_t) kSpecN)].store (m, std::memory_order_relaxed); ++wp;
        }
        tapPos[kTapPre].store (wp, std::memory_order_release);
        // the sidechain KEY tap (mono sum of the key bus); when the bus is not connected the tap keeps its last samples and the editor does not read it
        if (scOn)
        {
            const int mc = getMainBusNumInputChannels(), kc = juce::jlimit (0, 2, getBus (true, 1)->getNumberOfChannels());
            if (kc > 0 && mc < buffer.getNumChannels())
            {
                uint32_t kp = tapPos[kTapKey].load (std::memory_order_relaxed);
                const float* k0 = buffer.getReadPointer (mc); const float* k1 = (kc > 1 && mc + 1 < buffer.getNumChannels()) ? buffer.getReadPointer (mc + 1) : nullptr;
                auto& kr = tapRing[kTapKey];
                for (int i = 0; i < n; ++i) { kr[(size_t) (kp % (uint32_t) kSpecN)].store (k1 != nullptr ? 0.5f * (k0[i] + k1[i]) : k0[i], std::memory_order_relaxed); ++kp; }
                tapPos[kTapKey].store (kp, std::memory_order_release);
            }
        }
    }
    const double inMs = sumIn / (double) (n * nch);
    inDbView.store (inMs > 1e-12 ? (float) (10.0 * std::log10 (inMs)) : -120.0f, std::memory_order_relaxed);

    // BYPASS (the plug-in's own, with or without level matching, and the host's): the engine keeps running (no reset, so its states are warm) and the output
    // crossfades between the processed and the dry signal over 20 ms (smootherstep), both ways. Once the fade is over, the true bypass copies the input.
    const bool bypassedParam = ptr.bypass->load() > 0.5f;
    const bool matched = bypassedParam && ! hostBypassed && ptr.levelMatch->load() > 0.5f;
    const bool trueBypass = hostBypassed || (bypassedParam && ! matched);

    if (memImportPending.load (std::memory_order_acquire))   // a project was loaded: its memory goes into the engine before the first sample (the engine checks it against the audio)
    {
        const juce::SpinLock::ScopedTryLockType tl (memLock);
        if (tl.isLocked())
        {
            if (engine.memoryImport (memPending)) memSnapshot = memPending; else memSnapshot.clear();   // a memory of another layout is ignored
            memPublished = engine.memoryVersion();
            memImportPending.store (false, std::memory_order_release);
        }
    }

    const dyneq::Params p = readParams();
    engine.setParams (p);

    // BAKE statistics: reset when armed, collected only while the transport runs (no play-head: always running), COLLIDE only
    const bool armed = ptr.bakeArm->load() > 0.5f;
    if (armed && ! wasArmed) engine.bakeReset();
    wasArmed = armed;
    if (bakeResetRequest.exchange (false)) engine.bakeReset();
    bool playing = true;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
            playing = pos->getIsPlaying();
    playingView.store (playing, std::memory_order_relaxed);
    // 25 Oct: also only while the COLLIDE lamp is on and a key bus is connected. Before, a dark lamp (assistOff, no cut) or a missing key counted
    // blocks of zero cut and diluted the mean; after APPLY switches the lamp off the statistics stand still by this rule.
    const bool collecting = armed && playing && p.mode == dyneq::kCollide && ptr.assistOff->load() <= 0.5f && scOn && ! trueBypass;
    engine.bakeCollect (collecting);
    bakeActive.store (collecting, std::memory_order_relaxed);

    const float* key[2] = { nullptr, nullptr };
    int keyCh = 0;
    const int mainCh = getMainBusNumInputChannels();
    if (scOn)
    {
        keyCh = juce::jlimit (0, 2, getBus (true, 1)->getNumberOfChannels());
        for (int c = 0; c < keyCh; ++c)
            if (mainCh + c < buffer.getNumChannels()) key[c] = buffer.getReadPointer (mainCh + c);
            else keyCh = c;
    }

    const bool wantDelta = ptr.delta->load() > 0.5f && ! matched && ! trueBypass;
    const float mix = juce::jlimit (0.0f, 1.0f, ptr.mix->load() * 0.01f);
    // BYPASS with level matching: the dry signal at the loudness of the processed one (slow mean squares, +-12 dB at most), so the comparison is fair.
    // The gain is worked out on an absolute grid of 64 samples and applied per sample, so it does not depend on the host's block size.
    matchDbView.store (matched ? 20.0f * std::log10 (gMatch) : 0.0f, std::memory_order_relaxed);
    const float tDry = wantDelta ? 0.0f : 1.0f, tWet = (matched || trueBypass) ? 0.0f : wantDelta ? -1.0f : mix;   // out = dry * tDry + tWet * (wet - dry)
    const float tListen = ptr.keyListen->load() > 0.5f && keyCh > 0 ? 1.0f : 0.0f;
    if (firstBlock)
    {
        blendDry.setCurrentAndTargetValue (tDry); blendWet.setCurrentAndTargetValue (tWet); listenAmount.setCurrentAndTargetValue (tListen);
        firstBlock = false;
    }
    else { blendDry.setTargetValue (tDry); blendWet.setTargetValue (tWet); listenAmount.setTargetValue (tListen); }

    {
        const float og = std::pow (10.0f, ptr.output->load() * 0.05f);
        outGain.setTargetValue (std::abs (ptr.output->load()) < 1e-6f ? 1.0f : og);
    }
    for (int pos = 0; pos < n; pos += kScratch)
    {
        const int len = juce::jmin (kScratch, n - pos);
        float* io[2] = { nullptr, nullptr }; const float* keyC[2] = { nullptr, nullptr };
        for (int c = 0; c < nch; ++c) io[c] = buffer.getWritePointer (c) + pos;
        for (int c = 0; c < keyCh; ++c) keyC[c] = key[c] + pos;
        const bool blend = blendDry.isSmoothing() || blendWet.isSmoothing() || blendDry.getTargetValue() != 1.0f || blendWet.getTargetValue() != 1.0f;
        const bool settledBypass = trueBypass && ! blendDry.isSmoothing() && ! blendWet.isSmoothing() && blendDry.getTargetValue() == 1.0f && blendWet.getTargetValue() == 0.0f && matchCur == 1.0f && matchMid == 1.0f;
        for (int c = 0; c < nch; ++c) std::copy (io[c], io[c] + len, dry[(size_t) c].begin());   // the input: for the blend and for the loudness measurement
        engine.process (io, keyCh > 0 ? keyC : nullptr, nch, len);
        for (int i = 0; i < len; ++i)
        {
            const float matchTarget = matched ? gMatch : 1.0f;   // read per sample: the grid below moves it at absolute positions
            for (int c = 0; c < nch; ++c) { const double x = dry[(size_t) c][(size_t) i], w = io[c][i]; gridIn += x * x; gridWet += w * w; }
            if (settledBypass) { for (int c = 0; c < nch; ++c) io[c][i] = dry[(size_t) c][(size_t) i]; }   // the true bypass, fade over: exactly the input
            else if (blend)
            {
                const float g = blendDry.getNextValue(), m = blendWet.getNextValue();
                for (int c = 0; c < nch; ++c) { const float x = dry[(size_t) c][(size_t) i]; io[c][i] = g * x + m * (io[c][i] - x); }
            }
            if (matchCur != 1.0f || matchMid != 1.0f || matchTarget != 1.0f)
            {
                // P4c: two cascaded poles (10 ms each) instead of one of 20 ms: the gain's slope is continuous where the bypass starts, so a level-matched BYPASS leaves no kink
                // (the single pole measured -49 dBc above 4 kHz on a bell +9 dB cosine; the bound is -60 dBc). Still per sample, so the result does not depend on the host's block size.
                matchMid += (matchTarget - matchMid) * cMatch;
                matchCur += (matchMid - matchCur) * cMatch;
                if (matchTarget == 1.0f && std::abs (matchMid - 1.0f) < 1e-4f && std::abs (matchCur - 1.0f) < 1e-4f) matchMid = matchCur = 1.0f;   // 1e-4 (0.0009 dB): in float the 10 ms pole stops moving about 1.4e-5 short of 1, so a 1e-6 test never fired and the output stayed scaled after a level-matched BYPASS (plugin_check 13.3b)
                for (int c = 0; c < nch; ++c) io[c][i] *= matchCur;
            }
            if (++gridCount == 64)
            {
                msIn += (gridIn / (double) (64 * nch) - msIn) * a64; msWet += (gridWet / (double) (64 * nch) - msWet) * a64;
                gMatch = juce::jlimit (0.25f, 4.0f, (float) std::sqrt ((msWet + 1e-12) / (msIn + 1e-12)));
                gridCount = 0; gridIn = gridWet = 0.0;
            }
        }
        if (listenAmount.isSmoothing() || listenAmount.getTargetValue() != 0.0f)   // KEY LISTEN: the sidechain solo (a mono key feeds both sides)
            for (int i = 0; i < len; ++i)
            {
                const float l = listenAmount.getNextValue();
                for (int c = 0; c < nch; ++c)
                {
                    const float* k = keyC[juce::jmin (c, juce::jmax (keyCh, 1) - 1)];
                    io[c][i] += l * ((k != nullptr ? k[i] : 0.0f) - io[c][i]);
                }
            }
        if (outGain.isSmoothing() || outGain.getTargetValue() != 1.0f)   // OUTPUT crown: a smoothed gain, absent at exactly 0 dB
            for (int i = 0; i < len; ++i)
            {
                const float g = outGain.getNextValue();
                for (int c = 0; c < nch; ++c) io[c][i] *= g;
            }
    }
    {
        double sumOut = 0.0;
        for (int c = 0; c < nch; ++c) { const float* o = buffer.getReadPointer (c); for (int i = 0; i < n; ++i) sumOut += (double) o[i] * o[i]; }
        {   // the POST tap: the output as it leaves (after BYPASS, DELTA, KEY LISTEN and OUTPUT)
            uint32_t wp = tapPos[kTapPost].load (std::memory_order_relaxed);
            const float* l = buffer.getReadPointer (0); const float* r = nch > 1 ? buffer.getReadPointer (1) : nullptr;
            auto& ring = tapRing[kTapPost];
            for (int i = 0; i < n; ++i) { ring[(size_t) (wp % (uint32_t) kSpecN)].store (r != nullptr ? 0.5f * (l[i] + r[i]) : l[i], std::memory_order_relaxed); ++wp; }
            tapPos[kTapPost].store (wp, std::memory_order_release);
        }
        const double om = sumOut / (double) (n * nch);
        outDbView.store (om > 1e-12 ? (float) (10.0 * std::log10 (om)) : -120.0f, std::memory_order_relaxed);
    }

    if (engine.memoryVersion() != memPublished)   // the engine learned (or dropped) something: the copy the state save reads follows
    {
        const juce::SpinLock::ScopedTryLockType tl (memLock);
        if (tl.isLocked() && ! memImportPending.load (std::memory_order_relaxed)) { engine.memoryExport (memSnapshot); memPublished = engine.memoryVersion(); }
    }

    // what the panel reads
    for (int b = 0; b < dyneq::kBands; ++b)
    {
        cutView[(size_t) b].store (engine.currentCutsDb()[b], std::memory_order_relaxed);
        bakeMeanView[(size_t) b].store (engine.bakeMeanCutDb (b), std::memory_order_relaxed);
        bakePctView[(size_t) b].store (engine.bakePercentCut (b), std::memory_order_relaxed);
    }
    for (int s = 0; s < dyneq::kHunters; ++s)
    {
        hunterView[(size_t) s][0].store (engine.hunterFreqHz (s), std::memory_order_relaxed);
        hunterView[(size_t) s][1].store (engine.hunterGainDb (s), std::memory_order_relaxed);
        hunterView[(size_t) s][2].store (engine.hunterQ (s), std::memory_order_relaxed);
    }
    for (int i = 0; i < dyneq::kFreeBands; ++i) freeView[(size_t) i].store (engine.freeBandGainDb (i), std::memory_order_relaxed);
    targetView.store (engine.targetCutDb(), std::memory_order_relaxed);
    {
        const auto& ln = engine.targetLearned(); const int pt = engine.targetPartNow();
        targetHitsView.store (ln.hits, std::memory_order_relaxed); targetF0View.store (ln.f0, std::memory_order_relaxed);
        targetOnsetsView.store (engine.targetOnsets(), std::memory_order_relaxed); targetLearnedView.store (engine.targetHitsLearned(), std::memory_order_relaxed);
        for (int i = 0; i < 4; ++i) targetRejView[(size_t) i].store (engine.targetRejected (i), std::memory_order_relaxed);
        targetCentreView.store (pt >= 0 && pt < dyneq::kMaxParts && ln.ok[pt] ? ln.centre[pt] : 0.0f, std::memory_order_relaxed);
    }
    // the layers' numbers for the panel (the audio thread only stores; the editor does the drawing maths)
    for (int b = 0; b < dyneq::kBands; ++b) bellView[(size_t) b].store (engine.bellGainsDb()[b], std::memory_order_relaxed);
    for (int i = 0; i < 3; ++i)
    {
        tbView[(size_t) (3 * i)].store (engine.targetBellFreqHz (i), std::memory_order_relaxed);
        tbView[(size_t) (3 * i + 1)].store (engine.targetBellQ (i), std::memory_order_relaxed);
        tbView[(size_t) (3 * i + 2)].store (engine.targetBellActive (i) ? engine.targetBellCutDb (i) : 0.0f, std::memory_order_relaxed);
    }
    for (int k = 0; k < 2; ++k)
    {
        auto& e = edgeView[(size_t) k];
        e.hz.store (engine.targetEdgeHz (k), std::memory_order_relaxed); e.cutDb.store (engine.targetEdgeCutDb (k), std::memory_order_relaxed);
        e.mix.store (engine.targetEdgeMix (k), std::memory_order_relaxed); e.slope.store (engine.targetEdgeSlopeIdx (k), std::memory_order_relaxed);
        e.below.store (engine.targetEdgeBelow (k), std::memory_order_relaxed); e.run.store (engine.targetEdgeRunning (k), std::memory_order_relaxed);
    }
    for (int i = 0; i < dyneq::kFreeBands; ++i)
    {
        auto& l = bandLayer[(size_t) i];
        l.conflict.store (engine.bandConflictDb (i), std::memory_order_relaxed); l.self.store (engine.bandSelfDb (i), std::memory_order_relaxed);
        l.target.store (engine.bandTargetDb (i), std::memory_order_relaxed); l.net.store (engine.bandNetDb (i), std::memory_order_relaxed);
    }
    bakeSecondsView.store ((double) engine.bakeBlocksCounted() * (double) dyneq::kBlock / sampleRateHz, std::memory_order_relaxed);
    if (trueBypass)   // the true bypass shows no layer at work (the engine runs, but its cuts are not what is heard)
    {
        bakeActive.store (false, std::memory_order_relaxed);
        for (auto& c : cutView) c.store (0.0f, std::memory_order_relaxed);
        clearLayerViews();
        targetView.store (0.0f, std::memory_order_relaxed); targetF0View.store (0.0f, std::memory_order_relaxed); targetCentreView.store (0.0f, std::memory_order_relaxed); targetHitsView.store (0, std::memory_order_relaxed);
    }
}

// ---- BAKE
DyneqAudioProcessor::BakeSuggestion DyneqAudioProcessor::bakeSuggest() const
{
    BakeSuggestion s;
    int slots[dyneq::kFreeBands]; int ne = 0;
    for (int j = 0; j < dyneq::kFreeBands; ++j) if (bandIsEmpty (j)) slots[ne++] = j;
    s.emptyBands = ne;
    s.seconds = bakeSecondsView.load (std::memory_order_relaxed);
    if (ne == 0 || s.seconds <= 0.0) return s;
    float mean[dyneq::kBands], pct[dyneq::kBands];
    for (int b = 0; b < dyneq::kBands; ++b) { mean[b] = bakeMeanView[(size_t) b].load (std::memory_order_relaxed); pct[b] = bakePctView[(size_t) b].load (std::memory_order_relaxed); }
    s.count = dyneq::bakeFit (mean, pct, bandCentresHz(), sampleRateHz, ne, s.bells);
    for (int i = 0; i < s.count; ++i)
    {
        s.slot[i] = slots[i];
        char buf[160]; dyneq::bakeBellText (s.bells[i], buf, sizeof (buf)); s.text[i] = buf;
    }
    return s;
}

void DyneqAudioProcessor::setPlain (const juce::String& id, float plain)
{
    if (auto* p = apvts.getParameter (id))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 (plain));
        p->endChangeGesture();
    }
}

bool DyneqAudioProcessor::bandIsEmpty (int band) const
{
    static const char* const names[] { "on", "type", "freq", "gain", "q", "slope", "dyn", "thr", "range", "att", "rel", "trig" };
    if (ptr.band[band].on->load() > 0.5f) return false;
    for (const char* n : names)
        if (auto* par = apvts.getParameter (bandId (band, n)))
            if (std::abs (par->getValue() - par->getDefaultValue()) > 1.0e-6f) return false;
    return true;
}

int DyneqAudioProcessor::bakeApply()
{
    const auto s = bakeSuggest();
    lastBake = LastBake(); lastBake.atMs = juce::Time::getMillisecondCounter(); lastBake.seconds = s.seconds;
    if (s.count == 0)   // nothing is written: the panel says why (bakeStatus)
    {
        int offWithSettings = 0;
        for (int j = 0; j < dyneq::kFreeBands; ++j) if (ptr.band[j].on->load() <= 0.5f && ! bandIsEmpty (j)) ++offWithSettings;
        lastBake.refused = s.seconds <= 0.0 ? juce::String ("NOTHING TO BAKE YET: nothing was heard (arm BAKE, switch COLLIDE on with a key, play)")
                         : s.emptyBands == 0 ? (offWithSettings > 0 ? juce::String ("NO EMPTY BAND: the switched-off bands hold settings. DELETE a band to bake into it")
                                                                    : juce::String ("NO EMPTY BAND: all six bands are in use. DELETE a band to bake into it"))
                         : juce::String ("NOTHING TO BAKE: COLLIDE cut less than ") + juce::String (dyneq::kBakeMinCutDb, 1) + " dB on average in " + juce::String (s.seconds, 1) + " s";
        return 0;
    }
    {
        svl::ParamUndo::Group oneStep (undo, "BAKE");   // every parameter written below is ONE undo step
        for (int i = 0; i < s.count; ++i)
        {
            const int j = s.slot[i];
            setPlain (bandId (j, "on"), 1.0f);
            setPlain (bandId (j, "type"), (float) dyneq::kBell);
            setPlain (bandId (j, "freq"), s.bells[i].freqHz);
            setPlain (bandId (j, "gain"), s.bells[i].gainDb);
            setPlain (bandId (j, "q"), s.bells[i].q);
            setPlain (bandId (j, "dyn"), 0.0f);
        }
        // the baked bands replace the dynamic cut (SPEC 1 C "freeze the map into a static EQ"): with COLLIDE still cutting on top the result would be the
        // static cut PLUS the dynamic one, not what was heard (tester T5: "is the result what you heard before baking?"). Undo brings the dynamic cut back.
        setPlain ("assistOff", 1.0f);
    }
    lastBake.written = s.count;
    for (int i = 0; i < s.count; ++i) { lastBake.slot[i] = s.slot[i]; lastBake.freq[i] = s.bells[i].freqHz; lastBake.gain[i] = s.bells[i].gainDb; }
    return s.count;
}

juce::String DyneqAudioProcessor::bakeBellsText (const dyneq::BakeBell* bells, const int* slots, int n, int maxShown) const
{
    juce::StringArray parts;
    for (int i = 0; i < juce::jmin (n, maxShown); ++i)
    {
        const float f = bells[i].freqHz;
        parts.add ((slots != nullptr ? "BAND " + juce::String (slots[i] + 1) + " " : juce::String()) + juce::String (bells[i].gainDb, 1) + " dB @ "
                   + (f >= 1000.0f ? juce::String (f * 0.001f, 2) + " kHz" : juce::String (juce::roundToInt (f)) + " Hz"));
    }
    if (n > maxShown) parts.add ("+" + juce::String (n - maxShown) + " MORE");
    return parts.joinIntoString (", ");
}

DyneqAudioProcessor::BakeStatus DyneqAudioProcessor::bakeStatus() const
{
    BakeStatus st;
    const bool armed = ptr.bakeArm->load() > 0.5f;
    const bool collide = juce::roundToInt (ptr.mode->load()) == dyneq::kCollide, lampOn = ptr.assistOff->load() <= 0.5f;
    const auto s = bakeSuggest();
    const juce::String how = "BAKE freezes what COLLIDE cuts into static bells in the empty free bands. 1  BAKE ARM (BAKE listens).  2  Play the part "
                             "with the collisions, COLLIDE on and a key routed.  3  APPLY: the bells go into the empty bands and COLLIDE switches off, so you "
                             "hear the static EQ. Cmd+Z returns to the dynamic cut, Cmd+Shift+Z bakes again. RESET clears what was heard.";
    st.help = how;
    // the last APPLY, as long as the bands still hold what it wrote (after an undo it no longer applies)
    if (lastBake.written > 0)
    {
        bool still = true;
        for (int i = 0; i < lastBake.written && still; ++i)
        {
            const int j = lastBake.slot[i];
            still = ptr.band[j].on->load() > 0.5f && std::abs (ptr.band[j].freq->load() - lastBake.freq[i]) < 0.5f && std::abs (ptr.band[j].gain->load() - lastBake.gain[i]) < 0.05f;
        }
        if (still)
        {
            juce::StringArray b; for (int i = 0; i < lastBake.written; ++i) b.add (juce::String (lastBake.slot[i] + 1));
            st.state = kBakeBaked;
            st.line1 = "BAKED " + juce::String (lastBake.written) + (lastBake.written == 1 ? " BAND: " : " BANDS: ") + b.joinIntoString (", ") + " (from " + juce::String (lastBake.seconds, 1) + " s)" + (lampOn ? "" : ". COLLIDE IS OFF");
            st.line2 = lampOn ? juce::String ("COLLIDE is on again: the static bands AND the dynamic cut. Cmd+Z = the dynamic cut only")
                              : juce::String ("You hear the static EQ now. Cmd+Z = back to the dynamic cut, Cmd+Shift+Z = baked again");
            return st;
        }
    }
    if (lastBake.refused.isNotEmpty() && juce::Time::getMillisecondCounter() - lastBake.atMs < 6000)
    {
        st.state = kBakeRefused; st.line1 = "APPLY WROTE NOTHING"; st.line2 = lastBake.refused; return st;
    }
    const double secs = s.seconds;
    if (! armed && secs <= 0.0)
    {
        st.state = kBakeIdle; st.line1 = "Freezes COLLIDE's cuts into static bands";
        st.line2 = "1 BAKE ARM   2 play the part with the collisions   3 APPLY";
        return st;
    }
    if (armed && (! collide || ! lampOn))
    {
        st.state = kBakeNotListening; st.line1 = "ARMED, NOT LISTENING: BAKE learns from COLLIDE only";
        st.line2 = collide ? juce::String ("Switch the COLLIDE lamp on, then play") : juce::String ("SELF is on. Switch COLLIDE on (with a key), then play");
        if (secs > 0.0) st.line2 << ". " << juce::String (secs, 1) << " s heard so far";
        return st;
    }
    if (armed && ! isSidechainConnected())
    {
        st.state = kBakeNotListening; st.line1 = "ARMED, NO KEY: route a key into the sidechain"; st.line2 = "BAKE learns where COLLIDE cuts, and COLLIDE needs the key"; return st;
    }
    if (armed && ! bakeIsCollecting() && secs <= 0.0)
    {
        st.state = kBakeWaiting; st.line1 = "ARMED: LISTENING STARTS WITH PLAYBACK"; st.line2 = "Play the part with the collisions (above -60 dBFS), then APPLY"; return st;
    }
    st.state = kBakeListening;
    st.line1 = juce::String (bakeIsCollecting() ? "LISTENING: " : armed ? "PAUSED: " : "NOT ARMED: ") + juce::String (secs, 1) + " s HEARD";
    if (s.count > 0)
    {
        st.line1 << " - " << s.count << (s.count == 1 ? " BELL READY" : " BELLS READY");
        st.line2 = "APPLY writes " + bakeBellsText (s.bells, s.slot, s.count, 2);
        st.help = "APPLY will write: " + bakeBellsText (s.bells, s.slot, s.count, dyneq::kBakeMaxBells) + ". " + how;
    }
    else if (s.emptyBands == 0) st.line2 = "No empty band: DELETE a band to bake into it";
    else st.line2 = "Nothing to bake yet: COLLIDE cut less than " + juce::String (dyneq::kBakeMinCutDb, 1) + " dB on average";
    return st;
}

void DyneqAudioProcessor::bakeResetStats() { bakeResetRequest.store (true); }

juce::AudioProcessorEditor* DyneqAudioProcessor::createEditor() { return new DyneqEditor (*this); }

// the memory as a base64 string of little-endian float32; "" = nothing learned
static juce::String memoryToText (const std::vector<float>& m)
{
    if (m.size() < 2) return {};
    const int slots = (int) m[1]; size_t at = 2; bool any = false;
    for (int k = 0; k < slots && at < m.size(); ++k) { const int cnt = (int) m[at]; any = any || cnt > 0; at += 1 + (size_t) cnt * (3 + 4 * dyneq::kMaxParts); }
    if (! any) return {};
    juce::MemoryBlock mb (m.data(), m.size() * sizeof (float));
    return mb.toBase64Encoding();
}

bool DyneqAudioProcessor::targetMemoryEmpty() const
{
    const juce::SpinLock::ScopedLockType sl (memLock);
    return memoryToText (memImportPending.load() ? memPending : memSnapshot).isEmpty();
}

void DyneqAudioProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    state.setProperty ("editorScalePct", editorScalePct.load(), nullptr);
    state.setProperty ("uiMode", uiMode.load(), nullptr);
    state.removeProperty ("targetMemory", nullptr);   // only this instance's own memory is written, never one that came in with a loaded preset or project
    {   // P4c: the exact value of every parameter, normalised and plain (float bit patterns). A JUCE float parameter stores its PLAIN value and hands out convertTo0to1 of it; going plain -> normalised ->
        // plain through a skewed, stepped range does not return to the same float, so a restore through the tree alone can land one float-ulp away (ControlProof check 6 saw b6_freq 1.2e-7 off).
        // setStateInformation puts every parameter back on its own two bit patterns.
        juce::String exact;
        for (auto* ap : getParameters())
            if (auto* wid = dynamic_cast<juce::AudioProcessorParameterWithID*> (ap))
            {
                const float n = ap->getValue(), pl = apvts.getRawParameterValue (wid->paramID)->load(); uint32_t un, up; std::memcpy (&un, &n, 4); std::memcpy (&up, &pl, 4);
                exact << wid->paramID << "=" << juce::String::toHexString ((int) un) << "," << juce::String::toHexString ((int) up) << ";";
            }
        state.setProperty ("exactNorm", exact, nullptr);
    }
    {
        const juce::SpinLock::ScopedLockType sl (memLock);
        const auto text = memoryToText (memImportPending.load() ? memPending : memSnapshot);
        if (text.isNotEmpty()) state.setProperty ("targetMemory", text, nullptr);
    }
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, dest);
}

void DyneqAudioProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto tree = juce::ValueTree::fromXml (*xml);
            { const int sc = (int) tree.getProperty ("editorScalePct", editorScalePct.load()); editorScalePct = (sc == 75 || sc == 85) ? sc : 100; }
            uiMode = juce::jlimit (0, 8, (int) tree.getProperty ("uiMode", uiMode.load()));
            tree.removeProperty ("editorScalePct", nullptr); tree.removeProperty ("uiMode", nullptr);
            const juce::String text = tree.getProperty ("targetMemory", juce::String()).toString();
            tree.removeProperty ("targetMemory", nullptr);   // not a parameter: it must not stay in the tree and come back in a later preset
            const juce::String exact = tree.getProperty ("exactNorm", juce::String()).toString();
            tree.removeProperty ("exactNorm", nullptr);
            for (int i = tree.getNumChildren(); --i >= 0;)   // a parameter this version no longer has (the tester builds' "face"): dropped, so it never comes back in a later save
            {
                const auto child = tree.getChild (i);
                if (child.hasType ("PARAM") && apvts.getParameter (child.getProperty ("id").toString()) == nullptr) tree.removeChild (i, nullptr);
            }
            apvts.replaceState (tree);
            for (auto& tok : juce::StringArray::fromTokens (exact, ";", {}))   // the exact values written by getStateInformation: put every parameter on its own bits
            {
                const int eq = tok.indexOfChar ('='), cm = tok.indexOfChar (',');
                if (eq <= 0 || cm < eq) continue;
                auto* ap = apvts.getParameter (tok.substring (0, eq)); auto* rawv = apvts.getRawParameterValue (tok.substring (0, eq));
                if (ap == nullptr || rawv == nullptr) continue;
                const uint32_t un = (uint32_t) tok.substring (eq + 1, cm).getHexValue32(), up = (uint32_t) tok.substring (cm + 1).getHexValue32(); float n, pl; std::memcpy (&n, &un, 4); std::memcpy (&pl, &up, 4);
                if (! std::isfinite (n) || n < 0.0f || n > 1.0f) continue;
                auto good = [&] { return ap->getValue() == n && rawv->load() == pl; };
                if (good()) continue;
                float up1 = n, dn1 = n;   // the plain value is convertFrom0to1 of the normalised one that was written: look a few ulps either side of n for the one that gives both bit patterns back
                for (int k = 0; k < 9 && ! good(); ++k)
                {
                    float cand = n;
                    if (k > 0 && (k & 1)) { up1 = std::nextafter (up1, 2.0f); cand = up1; }
                    else if (k > 0) { dn1 = std::nextafter (dn1, -1.0f); cand = dn1; }
                    if (cand >= 0.0f && cand <= 1.0f) ap->setValueNotifyingHost (cand);
                }
                if (! good()) ap->setValueNotifyingHost (n);
            }
            undo.clear();   // the old steps describe a state that is gone
            if (text.isNotEmpty())   // a project written by this plug-in: the audio thread takes it before its next block
            {
                juce::MemoryBlock mb;
                if (mb.fromBase64Encoding (text) && mb.getSize() >= 2 * sizeof (float) && mb.getSize() % sizeof (float) == 0
                    && mb.getSize() <= (size_t) dyneq::Engine::memoryFloatsMax() * sizeof (float))
                {
                    const juce::SpinLock::ScopedLockType sl (memLock);
                    memPending.resize (mb.getSize() / sizeof (float)); std::memcpy (memPending.data(), mb.getData(), mb.getSize());
                    memImportPending.store (true, std::memory_order_release);
                }
            }
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new DyneqAudioProcessor(); }
