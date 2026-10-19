#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "ParamText.h"

namespace
{
using juce::NormalisableRange;
using juce::ParameterID;

constexpr int kNoteAutoSlots = seda::kNumNotes;   // the NOTE choice holds the 18 note values; NOTE AUTO is its own switch (the engine's noteAuto)

auto fromValue (const char* id)
{
    return [id] (float v, int) { return seda::ptext::format (id, v); };
}
auto toValue (const char* id, float fallback)
{
    return [id, fallback] (const juce::String& t) { float v; return seda::ptext::parse (id, t, v) ? v : fallback; };
}

juce::StringArray noteNames()
{
    juce::StringArray a;
    for (int i = 0; i < kNoteAutoSlots; ++i) a.add (seda::noteName (i));
    return a;
}

}

juce::AudioProcessorValueTreeState::ParameterLayout SedaAudioProcessor::createLayout()
{
    using F = juce::AudioParameterFloat;
    using B = juce::AudioParameterBool;
    using C = juce::AudioParameterChoice;
    using A = juce::AudioParameterFloatAttributes;
    const seda::Params d;   // engine defaults: nothing here decides a sound, the owner's listening may still change them
    auto attr = [] (const char* id, float fallback) { return A().withStringFromValueFunction (fromValue (id)).withValueFromStringFunction (toValue (id, fallback)); };

    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    // VOICE: BBD is the only voice built. DIGITAL and CLEAN are RESERVED values (SPEC 5): they exist so a session saved today still reads when the
    // voices arrive; until then the processor plays them as BBD and the panel greys them out. Never reorder or rename these three.
    p.push_back (std::make_unique<C> (ParameterID { "voice", 1 }, "VOICE", juce::StringArray { "BBD", "DIGITAL", "CLEAN" }, 0));
    // DELAY
    {
        NormalisableRange<float> r (20.0f, 1200.0f, 0.1f); r.setSkewForCentre (300.0f);
        p.push_back (std::make_unique<F> (ParameterID { "time", 1 }, "TIME", r, d.timeMs, attr ("time", d.timeMs)));
    }
    p.push_back (std::make_unique<B> (ParameterID { "sync", 1 }, "SYNC", d.sync));
    p.push_back (std::make_unique<C> (ParameterID { "note", 1 }, "NOTE", noteNames(), d.note));
    p.push_back (std::make_unique<B> (ParameterID { "noteAuto", 1 }, "NOTE AUTO", d.noteAuto));
    p.push_back (std::make_unique<F> (ParameterID { "feedback", 1 }, "FEEDBACK", NormalisableRange<float> (0.0f, 110.0f, 0.1f), d.feedback * 100.0f, attr ("feedback", d.feedback * 100.0f)));
    p.push_back (std::make_unique<F> (ParameterID { "mix", 1 }, "MIX", NormalisableRange<float> (0.0f, 100.0f, 0.1f), d.mix * 100.0f, attr ("mix", d.mix * 100.0f)));
    // CHARACTER
    p.push_back (std::make_unique<C> (ParameterID { "age", 1 }, "AGE", juce::StringArray { "NEW", "OLD" }, d.age));
    p.push_back (std::make_unique<F> (ParameterID { "comp", 1 }, "COMPANDER", NormalisableRange<float> (0.0f, 100.0f, 0.1f), d.comp * 100.0f, attr ("comp", d.comp * 100.0f)));
    p.push_back (std::make_unique<F> (ParameterID { "tone", 1 }, "TONE", NormalisableRange<float> (-100.0f, 100.0f, 0.1f), d.tone * 100.0f, attr ("tone", d.tone * 100.0f)));
    {
        NormalisableRange<float> r (0.05f, 8.0f, 0.01f); r.setSkewForCentre (0.8f);
        p.push_back (std::make_unique<F> (ParameterID { "rate", 1 }, "MOD RATE", r, d.rateHz, attr ("rate", d.rateHz)));
    }
    p.push_back (std::make_unique<F> (ParameterID { "depth", 1 }, "MOD DEPTH", NormalisableRange<float> (0.0f, 100.0f, 0.1f), d.depth * 100.0f, attr ("depth", d.depth * 100.0f)));
    p.push_back (std::make_unique<C> (ParameterID { "shape", 1 }, "MOD SHAPE", juce::StringArray { "SINE", "TRIANGLE" }, d.lfoShape));
    // STEREO
    p.push_back (std::make_unique<C> (ParameterID { "stereo", 1 }, "STEREO", juce::StringArray { "MONO", "WIDE", "PING-PONG" }, d.stereo));
    p.push_back (std::make_unique<F> (ParameterID { "width", 1 }, "WIDTH", NormalisableRange<float> (0.0f, 20.0f, 0.1f), d.widthMs, attr ("width", d.widthMs)));
    // LISTEN
    p.push_back (std::make_unique<B> (ParameterID { "listen", 1 }, "LISTEN", d.listen));
    p.push_back (std::make_unique<C> (ParameterID { "source", 1 }, "SOURCE", juce::StringArray { "OFF", "VOCAL", "DRUMS", "BASS", "KEYS" }, d.source));
    p.push_back (std::make_unique<F> (ParameterID { "duck", 1 }, "DUCK", NormalisableRange<float> (0.0f, 100.0f, 0.1f), d.duck * 100.0f, attr ("duck", d.duck * 100.0f)));
    p.push_back (std::make_unique<F> (ParameterID { "throw", 1 }, "THROW", NormalisableRange<float> (0.0f, 12.0f, 0.1f), d.throwDb, attr ("throw", d.throwDb)));
    {
        NormalisableRange<float> r (0.0f, 1000.0f, 1.0f); r.setSkewForCentre (200.0f);
        p.push_back (std::make_unique<F> (ParameterID { "wetHp", 1 }, "WET HIGH-PASS", r, d.wetHpHz, attr ("wetHp", d.wetHpHz)));
    }
    p.push_back (std::make_unique<B> (ParameterID { "bypass", 1 }, "BYPASS", false));
    // PERFORM (17 Oct 2026): appended at the END so the order of every earlier parameter stays as saved sessions and hosts know it. A state without them loads them at 0 (= off).
    p.push_back (std::make_unique<F> (ParameterID { "runaway", 1 }, "RUNAWAY", NormalisableRange<float> (0.0f, 100.0f, 0.1f), d.runaway * 100.0f, attr ("runaway", d.runaway * 100.0f)));
    p.push_back (std::make_unique<F> (ParameterID { "chaos", 1 }, "CHAOS", NormalisableRange<float> (0.0f, 100.0f, 0.1f), d.chaos * 100.0f, attr ("chaos", d.chaos * 100.0f)));
    return { p.begin(), p.end() };
}

SedaAudioProcessor::SedaAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "SEDA", createLayout())
{
    auto raw = [this] (const char* id) { return apvts.getRawParameterValue (id); };
    pVoice = raw ("voice"); pTime = raw ("time"); pSync = raw ("sync"); pNote = raw ("note"); pNoteAuto = raw ("noteAuto"); pFeedback = raw ("feedback");
    pMix = raw ("mix"); pTone = raw ("tone"); pRate = raw ("rate"); pDepth = raw ("depth"); pShape = raw ("shape"); pAge = raw ("age"); pComp = raw ("comp");
    pStereo = raw ("stereo"); pWidth = raw ("width"); pWetHp = raw ("wetHp"); pListen = raw ("listen"); pDuck = raw ("duck"); pThrow = raw ("throw");
    pSource = raw ("source"); pBypass = raw ("bypass"); pRunaway = raw ("runaway"); pChaos = raw ("chaos");
    bypassParam = apvts.getParameter ("bypass");
    // A parameter's stored plain value is convertFrom0to1(normalised) after every write, but the default it starts with is the number given
    // to the constructor. Write every default once, so a parameter that is put back on its normalised value by an undo is the same float as one
    // that was never touched (undo is then bit-exact in the raw values too).
    for (auto* ap : getParameters()) ap->setValueNotifyingHost (ap->getDefaultValue());
    for (auto& v : dryScratch) v.assign (4096, 0.0f);
    for (auto& v : outScratch) v.assign (4096, 0.0f);
    monoR.assign (4096, 0.0f);
    engine.prepare (48000.0);
}

SedaAudioProcessor::~SedaAudioProcessor() {}

bool SedaAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo()) return false;
    return layouts.getMainInputChannelSet() == out;
}

void SedaAudioProcessor::prepareToPlay (double sr, int samplesPerBlock)
{
    sampleRate = sr;
    engine.prepare (sr);
    setLatencySamples (0);   // the dry path is never delayed: no latency at any setting
    const size_t cap = (size_t) juce::jmax (4096, samplesPerBlock);
    for (auto& v : dryScratch) v.assign (cap, 0.0f);
    for (auto& v : outScratch) v.assign (cap, 0.0f);
    monoR.assign (cap, 0.0f);
    bypassG = -1.0f;
    engineNeedsReset = true;   // P3b: the first block resets the engine with the real parameters, the same path a state load takes, so a fresh instance and a restored one start alike
}

seda::Params SedaAudioProcessor::readParams (double bpm, int beatsPerBar) const noexcept
{
    seda::Params p;
    p.timeMs    = pTime->load();
    p.sync      = pSync->load() > 0.5f;
    p.note      = juce::jlimit (0, seda::kNumNotes - 1, juce::roundToInt (pNote->load()));
    p.bpm       = bpm;
    p.feedback  = pFeedback->load() * 0.01f;
    p.mix       = pMix->load() * 0.01f;
    p.tone      = pTone->load() * 0.01f;
    p.rateHz    = pRate->load();
    p.depth     = pDepth->load() * 0.01f;
    p.age       = juce::jlimit (0, 1, juce::roundToInt (pAge->load()));
    p.comp      = pComp->load() * 0.01f;
    p.stereo    = juce::jlimit (0, 2, juce::roundToInt (pStereo->load()));
    p.widthMs   = pWidth->load();
    p.lfoShape  = juce::jlimit (0, 1, juce::roundToInt (pShape->load()));
    p.bypass    = false;   // the plug-in handles BYPASS itself (render()): an engine bypass is a hard cut
    p.listen    = pListen->load() > 0.5f;
    p.duck      = pDuck->load() * 0.01f;
    p.throwDb   = pThrow->load();
    p.noteAuto  = pNoteAuto->load() > 0.5f;
    p.wetHpHz   = pWetHp->load();
    p.runaway   = pRunaway->load() * 0.01f;
    p.chaos     = pChaos->load() * 0.01f;
    p.source    = 0;   // 11 Oct: a program is written into the knobs (applyProgram), the engine adds no hidden starting point of its own
    p.touched   = 0;
    p.beatsPerBar = juce::jmax (1, beatsPerBar);
    // VOICE: only BBD exists (index 0). DIGITAL / CLEAN (1, 2) are reserved and play as BBD until those voices are built.
    return p;
}

double SedaAudioProcessor::getTailLengthSeconds() const
{
    const bool listen = pListen->load() > 0.5f, sync = pSync->load() > 0.5f;
    // LISTEN's throw adds up to 15 % FEEDBACK at a phrase end (SPEC 2.2): the tail is then at the raised value
    double fb = (double) pFeedback->load() * 0.01 + (listen ? seda::listen::kThrowFbMax * (double) pThrow->load() / 12.0 : 0.0);
    if (const double r = juce::jlimit (0.0, 1.0, (double) pRunaway->load() * 0.01); r > 0.0)   // RUNAWAY raises the loop gain (the engine's own curve)
        fb += juce::jmax (0.0, seda::perform::kRunMaxGain - fb) * r * r * (3.0 - 2.0 * r);
    if (fb >= 0.995) return std::numeric_limits<double>::infinity();   // 100 % and over: the loop does not decay (L4)
    // the longest delay the current settings can reach (a tail may be longer than needed, never shorter)
    const double beatSec = 60.0 / juce::jmax (20.0, lastBpm.load());
    double timeSec = sync ? beatSec * seda::noteBeats (juce::roundToInt (pNote->load())) : (double) pTime->load() * 0.001;
    if (sync && listen && pNoteAuto->load() > 0.5f) timeSec = juce::jmax (timeSec, beatSec * seda::noteBeats (seda::listen::kNoteSparse));   // NOTE AUTO picks up to 1/2
    timeSec = juce::jlimit (seda::kMinTimeSec, seda::kMaxTimeSec, timeSec);   // the engine clamps the same way; the +0.1 s below covers WIDTH (20 ms) and the line's own delay
    const double repeats = fb < 1.0e-3 ? 0.0 : std::ceil (kTailDb / juce::jmax (0.01, -20.0 * std::log10 (fb)));
    return juce::jmin (kTailCapSeconds, (repeats + 1.0) * timeSec + 0.1);
}

void SedaAudioProcessor::publish() noexcept
{
    aDuckDb.store ((float) engine.duckDb(), std::memory_order_relaxed);
    aThrowAmt.store ((float) engine.throwAmount(), std::memory_order_relaxed);
    aThrowOn.store (engine.throwActive(), std::memory_order_relaxed);
    aNote.store (engine.chosenNote(), std::memory_order_relaxed);
    aGuardOn.store (engine.guardActive(), std::memory_order_relaxed);
    aGuardDb.store ((float) engine.guardDb(), std::memory_order_relaxed);
    aThr.store ((float) engine.thresholdDb(), std::memory_order_relaxed);
    aOnset.store ((float) engine.onsetRate(), std::memory_order_relaxed);
    aLoopGain.store ((float) engine.loopGain(), std::memory_order_relaxed);
    aChaosSteps.store (engine.chaosStepCount(), std::memory_order_relaxed);
    aPublished.store (true, std::memory_order_relaxed);
}

SedaReadouts SedaAudioProcessor::getReadouts() const noexcept
{
    SedaReadouts r;
    r.duckDb = aDuckDb.load(); r.throwOn = aThrowOn.load(); r.throwAmount = aThrowAmt.load(); r.chosenNote = aNote.load(); r.guardOn = aGuardOn.load();
    r.guardDb = aGuardDb.load(); r.thresholdDb = aThr.load(); r.onsetRate = aOnset.load();
    r.loopGain = aLoopGain.load(); r.chaosSteps = aChaosSteps.load();
    r.published = aPublished.load();
    return r;
}

void SedaAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    render (buffer, pBypass->load() > 0.5f);
}

void SedaAudioProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    render (buffer, true);   // the host's own bypass goes through the same fade as the BYPASS parameter
}

void SedaAudioProcessor::render (juce::AudioBuffer<float>& buffer, bool bypassTarget)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    const int nch = juce::jlimit (1, 2, getMainBusNumOutputChannels());
    if (n <= 0 || buffer.getNumChannels() < nch) return;
    float pkIn = 0.0f;   // P4: the panel's IN meter (a read of the input, no allocation, no change to the audio)
    for (int c = 0; c < nch; ++c) pkIn = juce::jmax (pkIn, buffer.getMagnitude (c, 0, n));
    raisePeak (inPeak, pkIn);
#if SVL_LICENSING
    if (! license.isLicensed()) return;   // no licence, no processing: the audio passes through untouched
#endif

    double bpm = 120.0; float seenBpm = 0.0f; int beatsPerBar = 4; bool hostBars = false, hostBeats = false; double hostPpq = 0.0, hostBarStart = 0.0;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            if (auto b = pos->getBpm()) if (*b > 0.0) { bpm = *b; seenBpm = (float) *b; }
            if (auto ts = pos->getTimeSignature()) if (ts->denominator > 0) beatsPerBar = juce::roundToInt (4.0 * ts->numerator / ts->denominator);
            // bar lines of the host (NOTE AUTO): needs both the position and the bar start; otherwise the engine counts from the start of playback
            if (auto q = pos->getPpqPosition()) if (auto b = pos->getPpqPositionOfLastBarStart()) { hostBars = true; hostPpq = *q; hostBarStart = *b; }
            // CHAOS steps on the host's beats: its position while it plays (a stopped host repeats one position, which would re-trigger a beat every block)
            if (auto q = pos->getPpqPosition()) if (pos->getIsPlaying()) { hostBeats = true; hostPpq = *q; }
        }
    hostBpm.store (seenBpm, std::memory_order_relaxed);
    lastBpm.store (bpm, std::memory_order_relaxed);

    // BYPASS: the engine is faded out over 10 ms (a delay cut dead clicks on its own tail), then the block is a plain copy: bit-exact. The
    // fade runs per sample, so it does not depend on the host block size. When it has finished the engine is reset, so the repeats that were
    // in the line do not come back when BYPASS is switched off again.
    const float step = (float) (1.0 / (0.010 * sampleRate));
    const float target = bypassTarget ? 1.0f : 0.0f;
    if (bypassG < 0.0f) bypassG = target;
    if (bypassG == 1.0f && bypassTarget) { engineNeedsReset = true; raisePeak (outPeak, pkIn); return; }   // fully bypassed: the buffer is the input, untouched

    seda::Params p = readParams (bpm, beatsPerBar);
    p.hostBars = hostBars; p.hostBarStartPpq = hostBarStart; p.hostBeats = hostBeats;
    // A mono output bus runs the engine as MONO (tester finding T1, 17 Oct 2026): the engine is stereo, and on one channel the right output is dropped, so with PING-PONG every
    // second repeat (the ones on the right) vanished, and WIDE's offset means nothing on one channel. The STEREO parameter itself is not touched (state, automation).
    if (nch == 1) p.stereo = seda::kMono;
    engine.setParams (p);
    if (engineNeedsReset.exchange (false)) engine.reset();

    const bool fading = bypassG != target || bypassG > 0.0f;   // an engine at rest with BYPASS off (g = 0 = target) is not mixed at all
    const int chunk = (int) dryScratch[0].size();
    for (int pos = 0; pos < n; pos += chunk)
    {
        const int len = juce::jmin (chunk, n - pos);
        if (hostBars || hostBeats) { p.hostPpq = hostPpq + (double) pos * bpm / (60.0 * sampleRate); engine.setParams (p); }
        float* ch[2] = { buffer.getWritePointer (0) + pos, nullptr };
        ch[1] = nch == 2 ? buffer.getWritePointer (1) + pos : monoR.data();
        if (nch == 1) std::copy (ch[0], ch[0] + len, monoR.begin());   // the engine is stereo: a mono bus feeds both sides with the same signal
        if (fading) for (int c = 0; c < 2; ++c) std::copy (ch[c], ch[c] + len, dryScratch[(size_t) c].begin());
        engine.process (ch[0], ch[1], ch[0], ch[1], len);
        if (fading)
        {
            float g = bypassG;
            for (int i = 0; i < len; ++i)
            {
                g = g < target ? juce::jmin (target, g + step) : juce::jmax (target, g - step);
                for (int c = 0; c < nch; ++c) ch[c][i] = dryScratch[(size_t) c][(size_t) i] * g + ch[c][i] * (1.0f - g);
            }
            bypassG = g;
            if (g == 1.0f && bypassTarget) engineNeedsReset = true;   // the repeats of the old line must not come back at un-bypass
        }
    }
    float pkOut = 0.0f;
    for (int c = 0; c < nch; ++c) pkOut = juce::jmax (pkOut, buffer.getMagnitude (c, 0, n));
    raisePeak (outPeak, pkOut);
    raisePeak (wetPeak, engine.takeWetPeak());
    publish();
}

juce::AudioProcessorEditor* SedaAudioProcessor::createEditor() { return new SedaEditor (*this); }

void SedaAudioProcessor::applyProgram (int program)
{
    program = juce::jlimit (0, seda::kNumSources - 1, program);
    svl::ParamUndo::Group one (undo, "PROGRAM");
    auto put = [this] (const char* id, float plain)
    {
        auto* p = dynamic_cast<juce::RangedAudioParameter*> (apvts.getParameter (id));
        if (p == nullptr) return;
        const float v = p->convertTo0to1 (plain);
        if (p->convertFrom0to1 (p->getValue()) == p->convertFrom0to1 (v)) return;   // already there: no host noise
        p->beginChangeGesture();
        p->setValueNotifyingHost (v);
        p->endChangeGesture();
    };
    put ("source", (float) program);
    if (program == 0) return;
    const auto& t = seda::program (program);
    if (t.sync >= 0)   put ("sync", (float) t.sync);
    if (t.note >= 0)   put ("note", (float) t.note);
    if (t.stereo >= 0) put ("stereo", (float) t.stereo);
    put ("wetHp", t.wetHpHz);
    put ("duck", t.duckPct);
    put ("throw", t.throwDb);
    if (t.mixMaxPct < 100.0f && pMix->load() > t.mixMaxPct) put ("mix", t.mixMaxPct);
}

float SedaAudioProcessor::knobDefault (const juce::String& id) const
{
    auto* p = dynamic_cast<juce::RangedAudioParameter*> (apvts.getParameter (id));
    if (p == nullptr) return 0.0f;
    const float factory = p->convertFrom0to1 (p->getDefaultValue());
    const int program = juce::jlimit (0, seda::kNumSources - 1, juce::roundToInt (pSource->load()));
    if (program == 0) return factory;
    const auto& t = seda::program (program);
    if (id == "wetHp")  return t.wetHpHz;
    if (id == "duck")   return t.duckPct;
    if (id == "throw")  return t.throwDb;
    if (id == "note" && t.note >= 0) return (float) t.note;
    if (id == "mix" && t.mixMaxPct < 100.0f) return juce::jmin (factory, t.mixMaxPct);
    return factory;
}

/** A session saved before 11 Oct has no "stateV": back then SOURCE was an overlay the engine added (LISTEN on, SOURCE 1..4) to every knob whose "touched" bit was
    clear, and the knobs held the factory value underneath. To sound as it did, those overlay cells are written into the knobs now, once. A session saved by this
    build carries stateV 2 and is loaded as saved (SOURCE is then only the label of the last program chosen). */
void SedaAudioProcessor::migrateLegacyState (uint32_t touched)
{
    const int source = juce::jlimit (0, seda::kNumSources - 1, juce::roundToInt (pSource->load()));
    if (source == 0 || pListen->load() <= 0.5f) return;   // the old engine ignored SOURCE with LISTEN off
    const auto& t = seda::program (source);
    auto put = [this] (const char* id, float plain)
    {
        if (auto* p = apvts.getParameter (id)) p->setValueNotifyingHost (p->convertTo0to1 (plain));
    };
    if (t.note >= 0 && ! (touched & seda::kTouchNote)) { put ("note", (float) t.note); put ("sync", 1.0f); }
    if (source != 4 && source != 0 && ! (touched & seda::kTouchWetHp)) put ("wetHp", t.wetHpHz);
    if ((source == 1 || source == 2) && ! (touched & seda::kTouchDuck)) put ("duck", t.duckPct);
    if (source == 1 && ! (touched & seda::kTouchThrow)) put ("throw", t.throwDb);
    if (t.mixMaxPct < 100.0f && ! (touched & seda::kTouchMix) && pMix->load() > t.mixMaxPct) put ("mix", t.mixMaxPct);
    if (t.stereo >= 0 && ! (touched & seda::kTouchStereo)) put ("stereo", (float) t.stereo);
}

void SedaAudioProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    state.setProperty ("stateV", 2, nullptr);
    state.removeProperty ("touched", nullptr);
    state.setProperty ("editorScalePct", editorScalePct.load(), nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, dest);
}

void SedaAudioProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto tree = juce::ValueTree::fromXml (*xml);
            apvts.replaceState (tree);
            { const int sc = (int) tree.getProperty ("editorScalePct", editorScalePct.load()); editorScalePct = (sc == 60 || sc == 100) ? sc : 75; }
            if (! tree.hasProperty ("stateV")) migrateLegacyState ((uint32_t) (int) tree.getProperty ("touched", 0));
            engineNeedsReset = true;   // the line holds the old settings' repeats
            undo.clear();              // the old steps describe a state that is gone
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new SedaAudioProcessor(); }
