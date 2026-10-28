#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
auto dbText (int decimals)
{
    return juce::AudioParameterFloatAttributes()
        .withStringFromValueFunction ([decimals] (float v, int) { return (v > 0.0f ? "+" : "") + juce::String (v, decimals) + " dB"; })
        .withValueFromStringFunction ([] (const juce::String& t) { return t.retainCharacters ("+-.0123456789").getFloatValue(); });
}
}

juce::AudioProcessorValueTreeState::ParameterLayout TornaAudioProcessor::createLayout()
{
    using F = juce::AudioParameterFloat;
    using B = juce::AudioParameterBool;
    using C = juce::AudioParameterChoice;
    using juce::ParameterID;
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    // the 24 sliders: panel marks, -12 ... +12 dB in 0.1 dB steps (Ge1100Taper.h maps a mark to the pot position)
    for (int ch = 0; ch < 2; ++ch)
        for (int b = 0; b < torna::kBands; ++b)
            p.push_back (std::make_unique<F> (ParameterID { torna::sliderId (ch, b), 1 },
                                              juce::String (ch == 0 ? "L " : "R ") + torna::kBandNames[b],
                                              juce::NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f, dbText (1)));
    p.push_back (std::make_unique<B> (ParameterID { "power", 1 }, "POWER", true));
    p.push_back (std::make_unique<B> (ParameterID { "eqIn", 1 }, "EQ IN", true));
    p.push_back (std::make_unique<B> (ParameterID { "att", 1 }, "ATT -6dB", false));
    p.push_back (std::make_unique<B> (ParameterID { "rev", 1 }, "NOR/REV", false));
    p.push_back (std::make_unique<B> (ParameterID { "link", 1 }, "LINK", false));
    p.push_back (std::make_unique<F> (ParameterID { "input", 1 }, "INPUT", juce::NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f, dbText (1)));
    p.push_back (std::make_unique<F> (ParameterID { "output", 1 }, "OUTPUT", juce::NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f, dbText (1)));
    p.push_back (std::make_unique<F> (ParameterID { "level", 1 }, "ANALYZER LEVEL", juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.2f));
    p.push_back (std::make_unique<B> (ParameterID { "dispL", 1 }, "ANALYZER LEFT", true));
    p.push_back (std::make_unique<B> (ParameterID { "dispR", 1 }, "ANALYZER RIGHT", true));
    p.push_back (std::make_unique<C> (ParameterID { "anaSrc", 1 }, "ANALYZER", juce::StringArray { "OUT", "IN" }, 0));
    // rear plate (owner 27 Oct): what digital full scale means in volts at the original's input. The circuit is linear up to its
    // rails (13.3 V), so this only moves the point where the output stage clips: 1 V +19 dB above full scale, 2 V +13, 4 V +7.
    p.push_back (std::make_unique<C> (ParameterID { "lineLevel", 1 }, "LINE LEVEL", juce::StringArray { "0 dBFS = 1 V", "0 dBFS = 2 V", "0 dBFS = 4 V" }, 1));
    return { p.begin(), p.end() };
}

TornaAudioProcessor::TornaAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "TORNA", createLayout())
{
    for (int ch = 0; ch < 2; ++ch)
        for (int b = 0; b < torna::kBands; ++b) pSl[(size_t) (ch * 12 + b)] = apvts.getRawParameterValue (torna::sliderId (ch, b));
    auto raw = [this] (const char* id) { return apvts.getRawParameterValue (id); };
    pPower = raw ("power"); pEqIn = raw ("eqIn"); pAtt = raw ("att"); pRev = raw ("rev"); pLink = raw ("link"); pIn = raw ("input");
    pOut = raw ("output"); pLevel = raw ("level"); pDispL = raw ("dispL"); pDispR = raw ("dispR"); pAnaSrc = raw ("anaSrc"); pLine = raw ("lineLevel");
    for (auto* ap : getParameters()) ap->setValueNotifyingHost (ap->getDefaultValue());   // stored values equal their normalised round trip (undo is bit-exact)
    for (auto& c : eq) c.prepare (48000.0);
    analyser.prepare (48000.0);
    reloadUserPresets();
    for (auto& s : segs) s.store (0);
    for (auto& o : outPeak) o.store (0.0f);
}

bool TornaAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo()) return false;
    return layouts.getMainInputChannelSet() == out;
}

std::array<double, torna::kBands> TornaAudioProcessor::marksFor (int channel) const noexcept
{
    const int src = (pLink->load() > 0.5f) ? 0 : channel;
    std::array<double, torna::kBands> m {};
    for (int b = 0; b < torna::kBands; ++b) m[(size_t) b] = (double) pSl[(size_t) (src * 12 + b)]->load();
    return m;
}

void TornaAudioProcessor::prepareToPlay (double sr, int samplesPerBlock)
{
    sampleRate = sr;
    for (int ch = 0; ch < 2; ++ch)
    {
        eq[(size_t) ch].setMarks (marksFor (ch));
        eq[(size_t) ch].setReverse (pRev->load() > 0.5f);
        eq[(size_t) ch].setAttenuate (pAtt->load() > 0.5f);
        eq[(size_t) ch].setBypass (pEqIn->load() < 0.5f);
        eq[(size_t) ch].prepare (sr);
    }
    analyser.prepare (sr);
    setLatencySamples (ge::Channel::latency());
    for (auto& d : dryDelay) d.assign ((size_t) ge::Channel::latency(), 0.0f);
    dryPos = 0;
    const size_t cap = (size_t) juce::jmax (4096, samplesPerBlock);
    for (auto& s : scratch) s.assign (cap, 0.0f);
    powerG = -1.0f;
    inGain = juce::Decibels::decibelsToGain (pIn->load());
    outGain = juce::Decibels::decibelsToGain (pOut->load());
}

void TornaAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) { render (buffer, false); }
void TornaAudioProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) { render (buffer, true); }

void TornaAudioProcessor::render (juce::AudioBuffer<float>& buffer, bool hostBypassed)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    const int nch = juce::jlimit (1, 2, getMainBusNumOutputChannels());
    if (n <= 0 || buffer.getNumChannels() < nch) return;

    const bool rev = pRev->load() > 0.5f, att = pAtt->load() > 0.5f, eqIn = pEqIn->load() > 0.5f;
    for (int ch = 0; ch < 2; ++ch)
    {
        eq[(size_t) ch].setMarks (marksFor (ch));
        eq[(size_t) ch].setReverse (rev);
        eq[(size_t) ch].setAttenuate (att);
        eq[(size_t) ch].setBypass (! eqIn);
    }
    analyser.setLevel (pLevel->load());
    const bool dispL = pDispL->load() > 0.5f, dispR = pDispR->load() > 0.5f, anaIn = pAnaSrc->load() > 0.5f;
    const float target = (pPower->load() > 0.5f && ! hostBypassed) ? 1.0f : 0.0f;
    if (powerG < 0.0f) powerG = target;
    const float step = (float) (1.0 / (0.010 * sampleRate));
    const float gIn0 = inGain, gIn1 = juce::Decibels::decibelsToGain (pIn->load());
    const float gOut0 = outGain, gOut1 = juce::Decibels::decibelsToGain (pOut->load());
    inGain = gIn1; outGain = gOut1;

    const int lat = ge::Channel::latency();
    // LINE LEVEL: the engine's full scale is 2 V rms; 1 V / 4 V scale the signal into it and back out (only the rails see it)
    const float ls = juce::jlimit (0, 2, juce::roundToInt (pLine->load())) == 0 ? 0.5f : (juce::roundToInt (pLine->load()) == 2 ? 2.0f : 1.0f);
    float pk[2] = { 0.0f, 0.0f };
    for (int i = 0; i < n; ++i)
    {
        const float t = (float) (i + 1) / (float) n;
        const float gi = gIn0 + (gIn1 - gIn0) * t, go = gOut0 + (gOut1 - gOut0) * t;
        float x[2], y[2];
        for (int c = 0; c < 2; ++c) x[c] = buffer.getSample (juce::jmin (c, nch - 1), i);
        float g = powerG;
        if (g != target) g = g < target ? juce::jmin (target, g + step) : juce::jmax (target, g - step);
        powerG = g;
        for (int c = 0; c < nch; ++c)
        {
            const float dry = dryDelay[(size_t) c][(size_t) dryPos];
            dryDelay[(size_t) c][(size_t) dryPos] = x[c];
            const float wet = ls == 1.0f ? eq[(size_t) c].process (x[c] * gi) * go                     // the engine runs always, so POWER on is seamless
                                         : eq[(size_t) c].process (x[c] * gi * ls) * go / ls;
            y[c] = g * wet + (1.0f - g) * dry;
            buffer.setSample (c, i, y[c]);
            pk[c] = juce::jmax (pk[c], std::abs (y[c]));
        }
        if (nch == 1) y[1] = y[0];
        if (++dryPos >= lat) dryPos = 0;
        // the analyser reads LINE OUT (after OUTPUT), or the input; LEFT / RIGHT choose the channels (both = the sum)
        const float aL = anaIn ? x[0] : y[0], aR = anaIn ? x[nch - 1] : y[1];
        analyser.process (dispL ? aL : 0.0f, dispR ? aR : 0.0f);
    }
    for (int c = 0; c < 2; ++c)
    {
        float o = outPeak[(size_t) c].load (std::memory_order_relaxed);
        const float v = pk[juce::jmin (c, nch - 1)];
        while (v > o && ! outPeak[(size_t) c].compare_exchange_weak (o, v, std::memory_order_relaxed)) {}
    }
    for (int k = 0; k < gefl::kColumns; ++k) segs[(size_t) k].store (analyser.segments (k), std::memory_order_relaxed);
}

juce::AudioProcessorEditor* TornaAudioProcessor::createEditor() { return new TornaEditor (*this); }

// ---------------------------------------------------------------- panel actions
void TornaAudioProcessor::putParam (const juce::String& id, float plain)
{
    if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (apvts.getParameter (id)))
    {
        const float v = p->convertTo0to1 (plain);
        if (p->convertFrom0to1 (p->getValue()) == p->convertFrom0to1 (v)) return;
        p->beginChangeGesture();
        p->setValueNotifyingHost (v);
        p->endChangeGesture();
    }
}

std::array<float, 24> TornaAudioProcessor::currentSliders() const
{
    std::array<float, 24> a {};
    for (int i = 0; i < 24; ++i) a[(size_t) i] = pSl[(size_t) i]->load();
    return a;
}

void TornaAudioProcessor::recallMemory (int m)
{
    if (m < 0 || m >= torna::kMemories || ! memValid[(size_t) m]) return;
    svl::ParamUndo::Group one (undo, "MEMORY");
    for (int i = 0; i < 24; ++i) putParam (torna::sliderId (i / 12, i % 12), mem[(size_t) m][(size_t) i]);
    lastMem.store (m);
}

void TornaAudioProcessor::storeMemory (int m)
{
    if (m < 0 || m >= torna::kMemories) return;
    mem[(size_t) m] = currentSliders();
    memValid[(size_t) m] = true;
    lastMem.store (m);
}

void TornaAudioProcessor::copyBank (int from)
{
    svl::ParamUndo::Group one (undo, from == 0 ? "COPY L>R" : "COPY R>L");
    for (int b = 0; b < torna::kBands; ++b) putParam (torna::sliderId (1 - from, b), pSl[(size_t) (from * 12 + b)]->load());
}

void TornaAudioProcessor::flatAll()
{
    svl::ParamUndo::Group one (undo, "FLAT");
    for (int i = 0; i < 24; ++i) putParam (torna::sliderId (i / 12, i % 12), 0.0f);
}

void TornaAudioProcessor::setLink (bool on)
{
    svl::ParamUndo::Group one (undo, "LINK");
    if (on) for (int b = 0; b < torna::kBands; ++b) putParam (torna::sliderId (1, b), pSl[(size_t) b]->load());
    putParam ("link", on ? 1.0f : 0.0f);
}

std::array<float, 24> TornaAudioProcessor::presetMarks (int i) const
{
    std::array<float, 24> m {};
    if (isUserPreset (i) && i - torna::kNumPresets < (int) userPresets.size()) return userPresets[(size_t) (i - torna::kNumPresets)].marks;
    const auto& f = torna::presets()[(size_t) juce::jlimit (0, torna::kNumPresets - 1, i)].marks;
    for (int c = 0; c < 2; ++c) for (int b = 0; b < 12; ++b) m[(size_t) (c * 12 + b)] = f[(size_t) b];
    return m;
}

juce::String TornaAudioProcessor::presetName (int i) const
{
    if (isUserPreset (i) && i - torna::kNumPresets < (int) userPresets.size()) return userPresets[(size_t) (i - torna::kNumPresets)].name;
    return torna::presets()[(size_t) juce::jlimit (0, torna::kNumPresets - 1, i)].name;
}

void TornaAudioProcessor::applyPreset (int i)
{
    i = juce::jlimit (0, presetCount() - 1, i);
    svl::ParamUndo::Group one (undo, "PRESET");
    const auto m = presetMarks (i);
    for (int k = 0; k < 24; ++k) putParam (torna::sliderId (k / 12, k % 12), m[(size_t) k]);
    presetIdx.store (i);
    lastMem.store (-1);   // what is loaded now is the preset
}

juce::File TornaAudioProcessor::userPresetDir() const
{
    if (presetDirOverride != juce::File()) return presetDirOverride;
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("SubverseLab/TORNA/Presets");
}

void TornaAudioProcessor::reloadUserPresets()
{
    userPresets.clear();
    auto files = userPresetDir().findChildFiles (juce::File::findFiles, false, "*.tornapreset");
    std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b) { return a.getFileName().compareNatural (b.getFileName()) < 0; });
    for (auto& f : files)
        if (auto xml = juce::parseXML (f))
        {
            UserPreset u; u.name = xml->getStringAttribute ("name", f.getFileNameWithoutExtension()).toUpperCase().substring (0, 20);
            const auto parts = juce::StringArray::fromTokens (xml->getStringAttribute ("marks"), ",", "");
            if (parts.size() != 24) continue;
            for (int k = 0; k < 24; ++k) u.marks[(size_t) k] = juce::jlimit (-12.0f, 12.0f, parts[k].getFloatValue());
            userPresets.push_back (u);
        }
}

juce::String TornaAudioProcessor::saveUserPreset()
{
    const auto dir = userPresetDir();
    if (! dir.createDirectory()) return {};
    int n = 1;
    while (dir.getChildFile (juce::String::formatted ("USER %02d.tornapreset", n)).exists()) ++n;
    const juce::String name = juce::String::formatted ("USER %02d", n);
    juce::XmlElement x ("TORNAPRESET");
    x.setAttribute ("version", 1);
    x.setAttribute ("name", name);
    juce::String marks;
    for (int k = 0; k < 24; ++k) marks << juce::String (pSl[(size_t) k]->load(), 1) << (k < 23 ? "," : "");
    x.setAttribute ("marks", marks);
    if (! x.writeTo (dir.getChildFile (name + ".tornapreset"))) return {};
    reloadUserPresets();
    for (int i = torna::kNumPresets; i < presetCount(); ++i) if (presetName (i) == name) { presetIdx.store (i); lastMem.store (-1); }
    return name;
}

float TornaAudioProcessor::sliderDefault (int channel, int band) const
{
    const int m = lastMem.load();
    if (m >= 0 && m < torna::kMemories && memValid[(size_t) m]) return mem[(size_t) m][(size_t) (channel * 12 + band)];
    return presetMarks (presetIdx.load())[(size_t) (channel * 12 + band)];
}

// ---------------------------------------------------------------- state
void TornaAudioProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    state.setProperty ("stateV", 1, nullptr);
    state.setProperty ("editorScalePct", editorScalePct.load(), nullptr);
    state.setProperty ("capLights", capLights.load(), nullptr);
    state.setProperty ("lastMemory", lastMem.load(), nullptr);
    state.setProperty ("preset", presetIdx.load(), nullptr);
    for (int m = 0; m < torna::kMemories; ++m)
    {
        juce::String s;
        if (memValid[(size_t) m]) for (int i = 0; i < 24; ++i) s << juce::String (mem[(size_t) m][(size_t) i], 1) << (i < 23 ? "," : "");
        state.setProperty ("mem" + juce::String (m), s, nullptr);
    }
    if (auto xml = state.createXml()) copyXmlToBinary (*xml, dest);
}

void TornaAudioProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto tree = juce::ValueTree::fromXml (*xml);
            apvts.replaceState (tree);
            { const int sc = (int) tree.getProperty ("editorScalePct", 75); editorScalePct = sc == 100 ? 100 : 75; }
            capLights = juce::jlimit (0, 2, (int) tree.getProperty ("capLights", 0));
            for (int m = 0; m < torna::kMemories; ++m)
            {
                const auto parts = juce::StringArray::fromTokens (tree.getProperty ("mem" + juce::String (m)).toString(), ",", "");
                memValid[(size_t) m] = parts.size() == 24;
                if (memValid[(size_t) m]) for (int i = 0; i < 24; ++i) mem[(size_t) m][(size_t) i] = juce::jlimit (-12.0f, 12.0f, parts[i].getFloatValue());
            }
            lastMem.store ((int) tree.getProperty ("lastMemory", -1));
            presetIdx.store (juce::jlimit (0, presetCount() - 1, (int) tree.getProperty ("preset", 0)));
            undo.clear();
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new TornaAudioProcessor(); }
