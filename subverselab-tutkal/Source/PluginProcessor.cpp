#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "ParamText.h"

namespace
{
using juce::NormalisableRange;
using juce::ParameterID;

// VOICING choice order (index -> engine voicing): GLUE, LA, PUMP, CORE.
constexpr int kVoicingMap[4] = { tutkal::kGlue, tutkal::kLa, tutkal::kPump, tutkal::kCore };
constexpr float kPumpNotes[4] = { 0.0625f, 0.125f, 0.25f, 0.5f };   // 1/16, 1/8, 1/4, 1/2 of a whole note

// Value text: the same strings as the panel (ParamText.h). The unit is inside the text, so the parameters carry no label
// (a host would otherwise print it twice).
auto fromValue (const char* id)
{
    return [id] (float v, int) { return tutkal::ptext::format (id, v); };
}
auto toValue (const char* id, float fallback)
{
    return [id, fallback] (const juce::String& t) { float v; return tutkal::ptext::parse (id, t, v) ? v : fallback; };
}
}

juce::AudioProcessorValueTreeState::ParameterLayout TutkalAudioProcessor::createLayout()
{
    using F = juce::AudioParameterFloat;
    using B = juce::AudioParameterBool;
    using C = juce::AudioParameterChoice;
    using A = juce::AudioParameterFloatAttributes;
    using CA = juce::AudioParameterChoiceAttributes;
    const tutkal::Params d;   // engine defaults, where the listening pack used them
    auto attr = [] (const char* id, float fallback) { return A().withStringFromValueFunction (fromValue (id)).withValueFromStringFunction (toValue (id, fallback)); };
    auto choiceAttr = [] (const char* id)
    {
        return CA().withStringFromValueFunction ([id] (int i, int) { return tutkal::ptext::format (id, (float) i); })
                   .withValueFromStringFunction ([id] (const juce::String& t) { int i = 0; return tutkal::ptext::parseChoice (id, t, i) ? i : 0; });
    };

    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    p.push_back (std::make_unique<C> (ParameterID { "voicing", 1 }, "VOICING", juce::StringArray { "GLUE", "LA", "PUMP", "CORE" }, 0));
    p.push_back (std::make_unique<F> (ParameterID { "threshold", 1 }, "THRESHOLD", NormalisableRange<float> (-40.0f, 0.0f, 0.1f), d.thresholdDb, attr ("threshold", d.thresholdDb)));
    {
        NormalisableRange<float> r (1.5f, 20.0f, 0.1f); r.setSkewForCentre (4.0f);
        p.push_back (std::make_unique<F> (ParameterID { "ratio", 1 }, "RATIO", r, 4.0f, attr ("ratio", 4.0f)));
    }
    {
        NormalisableRange<float> r (0.1f, 30.0f, 0.01f); r.setSkewForCentre (3.0f);
        p.push_back (std::make_unique<F> (ParameterID { "attack", 1 }, "ATTACK", r, 10.0f, attr ("attack", 10.0f)));
    }
    {
        NormalisableRange<float> r (100.0f, 1200.0f, 1.0f); r.setSkewForCentre (400.0f);
        p.push_back (std::make_unique<F> (ParameterID { "release", 1 }, "RELEASE", r, 300.0f, attr ("release", 300.0f)));
    }
    p.push_back (std::make_unique<B> (ParameterID { "autoRelease", 1 }, "AUTO RELEASE", true));
    p.push_back (std::make_unique<F> (ParameterID { "makeup", 1 }, "MAKEUP", NormalisableRange<float> (0.0f, 20.0f, 0.1f), 0.0f, attr ("makeup", 0.0f)));
    p.push_back (std::make_unique<F> (ParameterID { "mix", 1 }, "MIX", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f, attr ("mix", 100.0f)));
    {
        NormalisableRange<float> r (0.0f, 300.0f, 1.0f); r.setSkewForCentre (60.0f);
        p.push_back (std::make_unique<F> (ParameterID { "scHpf", 1 }, "SC HPF", r, 0.0f, attr ("scHpf", 0.0f)));
    }
    p.push_back (std::make_unique<B> (ParameterID { "ext", 1 }, "EXT", false));
    p.push_back (std::make_unique<F> (ParameterID { "drive", 1 }, "DRIVE", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f, attr ("drive", 0.0f)));
    p.push_back (std::make_unique<B> (ParameterID { "in", 1 }, "IN", true));
    p.push_back (std::make_unique<F> (ParameterID { "peakReduction", 1 }, "PEAK REDUCTION", NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.5f, attr ("peakReduction", 0.5f)));
    p.push_back (std::make_unique<B> (ParameterID { "limit", 1 }, "LIMIT (LA)", false));
    p.push_back (std::make_unique<C> (ParameterID { "pumpNote", 1 }, "PUMP NOTE", juce::StringArray { "1/16", "1/8", "1/4", "1/2" }, 1, choiceAttr ("pumpNote")));
    p.push_back (std::make_unique<C> (ParameterID { "pumpShape", 1 }, "PUMP SHAPE", juce::StringArray { "EXP", "LINEAR", "SWELL" }, 0, choiceAttr ("pumpShape")));
    p.push_back (std::make_unique<F> (ParameterID { "depth", 1 }, "DEPTH", NormalisableRange<float> (0.0f, 24.0f, 0.1f), 10.0f, attr ("depth", 10.0f)));
    return { p.begin(), p.end() };
}

TutkalAudioProcessor::TutkalAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",     juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output",    juce::AudioChannelSet::stereo(), true)
                          .withInput  ("Sidechain", juce::AudioChannelSet::stereo(), false)),
      apvts (*this, nullptr, "TUTKAL", createLayout())
{
    auto raw = [this] (const char* id) { return apvts.getRawParameterValue (id); };
    pVoicing = raw ("voicing"); pThreshold = raw ("threshold"); pRatio = raw ("ratio"); pAttack = raw ("attack");
    pRelease = raw ("release"); pAuto = raw ("autoRelease"); pMakeup = raw ("makeup"); pMix = raw ("mix");
    pScHpf = raw ("scHpf"); pExt = raw ("ext"); pDrive = raw ("drive"); pIn = raw ("in");
    pPeakRed = raw ("peakReduction"); pLimit = raw ("limit"); pPumpNote = raw ("pumpNote");
    pPumpShape = raw ("pumpShape"); pDepth = raw ("depth");

    grScratch.assign (4096, 0.0f);
    for (auto& l : bypassLine) l.assign ((size_t) tutkal::kLatency, 0.0f);
}

bool TutkalAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo()) return false;
    if (layouts.getMainInputChannelSet() != out) return false;
    const auto& sc = layouts.getChannelSet (true, 1);
    return sc.isDisabled() || sc == juce::AudioChannelSet::mono() || sc == juce::AudioChannelSet::stereo();
}

void TutkalAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate);
    setLatencySamples (tutkal::Engine::latency());   // 63 at every sample rate; a knob never changes it
    grScratch.assign ((size_t) juce::jmax (4096, samplesPerBlock), 0.0f);   // process() also chunks, so a larger host block never allocates
    for (auto& l : bypassLine) l.assign ((size_t) tutkal::kLatency, 0.0f);
    bypassPos = 0;
    engineNeedsReset = false;
    grMaxDb.store (0.0f);
}

bool TutkalAudioProcessor::sidechainBusEnabled() const
{
    const auto* bus = getBus (true, 1);
    return bus != nullptr && bus->isEnabled() && bus->getNumberOfChannels() > 0;
}

tutkal::Params TutkalAudioProcessor::readParams (bool sidechainEnabled, double bpm) const noexcept
{
    tutkal::Params p;
    p.voicing       = kVoicingMap[juce::jlimit (0, 3, juce::roundToInt (pVoicing->load()))];
    p.thresholdDb   = pThreshold->load();
    p.ratio         = pRatio->load();
    p.attackMs      = pAttack->load();
    p.releaseMs     = pRelease->load();
    p.autoRelease   = pAuto->load() > 0.5f;
    p.makeupDb      = pMakeup->load();
    p.mix           = pMix->load() * 0.01f;
    const float hpf = pScHpf->load();
    p.scHpfHz       = hpf < 0.5f ? -1.0f : (hpf < 20.0f ? 0.0f : hpf);   // 0 = AUTO -> the voicing's own default
    // EXT hears only the sidechain bus. With the bus disabled there is no key to hear (the engine would hear silence
    // and never compress), so the detector stays on the programme: PUMP then keys from the main input.
    p.ext           = pExt->load() > 0.5f && sidechainEnabled;
    p.drive         = pDrive->load() * 0.01f;
    p.in            = pIn->load() > 0.5f;
    p.peakReduction = pPeakRed->load();
    p.limit         = pLimit->load() > 0.5f;
    p.bpm           = (float) bpm;
    p.pumpNote      = kPumpNotes[juce::jlimit (0, 3, juce::roundToInt (pPumpNote->load()))];
    p.pumpShape     = juce::jlimit (0, 2, juce::roundToInt (pPumpShape->load()));
    p.depthDb       = pDepth->load();
    return p;
}

void TutkalAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    const int nch = juce::jlimit (1, 2, getMainBusNumOutputChannels());
    if (n <= 0 || buffer.getNumChannels() < nch) return;

    if (engineNeedsReset) { engine.reset(); engineNeedsReset = false; }

    double bpm = 120.0;
    float seenBpm = 0.0f;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
            if (auto b = pos->getBpm())
                if (*b > 0.0) { bpm = *b; seenBpm = (float) *b; }
    hostBpm.store (seenBpm, std::memory_order_relaxed);

    const bool scOn = sidechainBusEnabled();
    scConnected.store (scOn, std::memory_order_relaxed);
    const tutkal::Params p = readParams (scOn, bpm);

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

    const int chunk = (int) grScratch.size();
    float blockMax = 0.0f;
    for (int pos = 0; pos < n; pos += chunk)
    {
        const int len = juce::jmin (chunk, n - pos);
        float* ioC[2]; const float* keyC[2];
        for (int c = 0; c < nch; ++c) ioC[c] = buffer.getWritePointer (c) + pos;
        for (int c = 0; c < keyCh; ++c) keyC[c] = key[c] + pos;
        engine.process (ioC, nch, keyCh > 0 ? keyC : nullptr, keyCh, len, p, grScratch.data());
        for (int i = 0; i < len; ++i) blockMax = juce::jmax (blockMax, grScratch[(size_t) i]);
    }
    grMaxDb.store (blockMax, std::memory_order_relaxed);
}

void TutkalAudioProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    // The host's own bypass: the dry input, late by the reported latency, so nothing jumps in time.
    const int n = buffer.getNumSamples();
    const int nch = juce::jmin (2, getMainBusNumInputChannels(), buffer.getNumChannels());
    const int L = tutkal::kLatency;
    int pos = bypassPos;
    for (int c = 0; c < nch; ++c)
    {
        auto* d = buffer.getWritePointer (c);
        auto& line = bypassLine[(size_t) c];
        pos = bypassPos;
        for (int i = 0; i < n; ++i)
        {
            const float in = d[i];
            d[i] = line[(size_t) pos];
            line[(size_t) pos] = in;
            if (++pos == L) pos = 0;
        }
    }
    bypassPos = pos;
    engineNeedsReset = true;
    grMaxDb.store (0.0f, std::memory_order_relaxed);
}

juce::AudioProcessorEditor* TutkalAudioProcessor::createEditor() { return new TutkalEditor (*this); }

void TutkalAudioProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    state.setProperty ("editorWidth", editorWidth.load(), nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, dest);
}

void TutkalAudioProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto tree = juce::ValueTree::fromXml (*xml);
            editorWidth = juce::jlimit (800, 2400, (int) tree.getProperty ("editorWidth", editorWidth.load()));
            apvts.replaceState (tree);
            undo.clear();   // the old steps describe a state that is gone
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new TutkalAudioProcessor(); }
