#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    /** The selectors (REACH, PATTERN, LENGTH, STEPS): float parameters that tell the host how many steps they have. */
    struct SteppedFloat : juce::AudioParameterFloat
    {
        SteppedFloat (int n, const juce::ParameterID& id, const juce::String& name, juce::NormalisableRange<float> range, float def, const juce::AudioParameterFloatAttributes& attr)
            : juce::AudioParameterFloat (id, name, range, def, attr), steps (n) {}
        int getNumSteps() const override { return steps; }
        int steps;
    };

    juce::String percent (float v, int) { return juce::String (juce::roundToInt (v * 100.0f)) + " %"; }
}

juce::AudioProcessorValueTreeState::ParameterLayout KiymaProcessor::createLayout()
{
    using namespace juce;
    using Attr = AudioParameterFloatAttributes;
    AudioProcessorValueTreeState::ParameterLayout layout;

    auto unit = [&] (const char* id, const char* name, float def)
    {
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name,
            NormalisableRange<float> (0.0f, 1.0f, 0.001f), def,
            Attr().withStringFromValueFunction (percent)));
    };

    // v6 (docs/DSP_V6_CHOPPER.md): the ChopperLab step engine. The ids stay; the meanings are the Max device's controls.
    auto stepped = [&] (int n, const char* id, const char* name, float def, std::function<String (float)> text)
    {
        layout.add (std::make_unique<SteppedFloat> (n, ParameterID { id, 1 }, name,
            NormalisableRange<float> (0.0f, 1.0f, 1.0f / (float) (n - 1)), def,
            Attr().withStringFromValueFunction ([text] (float v, int) { return text (v); })));
    };
    unit (kiymaIds::glitch, "Density", 0.40f);
    stepped (16, kiymaIds::brutal, "Reach", 5.0f / 15.0f, [] (float v) { return String (kiyma::reachOf (v)) + " steps"; });
    stepped (16, kiymaIds::offset, "Pattern", 0.0f, [] (float v) { return String (kiyma::patternIndex (v) + 1); });
    stepped (4, kiymaIds::envelope, "Length", 2.0f / 3.0f, [] (float v) { return String (kiyma::lengthName (kiyma::lengthIndex (v))); });
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { kiymaIds::attack, 1 }, "Edge",
        NormalisableRange<float> (0.0f, 10.0f, 0.1f), 0.0f,
        Attr().withStringFromValueFunction ([] (float v, int) { return v < 0.05f ? String ("HARD") : String (v, 1) + " ms"; })));
    stepped (3, kiymaIds::release, "Steps", 0.5f, [] (float v) { return String (kiyma::stepsOf (kiyma::stepsIndex (v))); });
    unit (kiymaIds::mix, "Mix", 1.0f);
    unit (kiymaIds::crank, "Shuffle", 0.0f);
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { kiymaIds::output, 1 }, "Output",
        NormalisableRange<float> (-24.0f, 6.0f, 0.1f), 0.0f,
        Attr().withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " dB"; })));
    return layout;
}

KiymaProcessor::KiymaProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "Kiyma", createLayout())
{
    pGlitch = apvts.getRawParameterValue (kiymaIds::glitch);
    pBrutal = apvts.getRawParameterValue (kiymaIds::brutal);
    pOffset = apvts.getRawParameterValue (kiymaIds::offset);
    pEnvelope = apvts.getRawParameterValue (kiymaIds::envelope);
    pAttack = apvts.getRawParameterValue (kiymaIds::attack);
    pRelease = apvts.getRawParameterValue (kiymaIds::release);
    pMix = apvts.getRawParameterValue (kiymaIds::mix);
    pCrank = apvts.getRawParameterValue (kiymaIds::crank);
    pOutput = apvts.getRawParameterValue (kiymaIds::output);
    setLatencySamples (kiyma::kLatency);
    for (auto* id : kiyma::kPresetParamIds)
        apvts.addParameterListener (id, &presetWatch);
}

KiymaProcessor::~KiymaProcessor()
{
    for (auto* id : kiyma::kPresetParamIds)
        apvts.removeParameterListener (id, &presetWatch);
}

float KiymaProcessor::presetNormalised (int index, int slot) const
{
    return apvts.getParameter (kiyma::kPresetParamIds[slot])->convertTo0to1 (kiyma::kPresets[index].v[slot]);
}

void KiymaProcessor::PresetWatch::parameterChanged (const juce::String& id, float newPlain)
{
    // Any thread: only atomics here. A breaker: once a preset parameter is more than 0.01 away, the switch is down and stays down.
    const int active = owner.activePreset.load();
    if (active < 0 || owner.recalling.load())
        return;
    for (int slot = 0; slot < 8; ++slot)
        if (id == kiyma::kPresetParamIds[slot])
        {
            auto* par = owner.apvts.getParameter (id);
            if (kiyma::presetTrips (par->convertTo0to1 (newPlain), owner.presetNormalised (active, slot)))
                owner.activePreset.store (-1);
            return;
        }
}

void KiymaProcessor::recallPreset (int index)
{
    if (index < 0 || index >= 5)
        return;
    recalling.store (true);
    {
        svl::ParamUndo::Group oneStep (undo, juce::String ("Preset ") + kiyma::kPresets[index].name);   // the whole recall is one undo step
        auto write = [this] (const char* id, float normalised)
        {
            auto* par = apvts.getParameter (id);
            par->beginChangeGesture();
            par->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, normalised));
            par->endChangeGesture();
        };
        for (int slot = 0; slot < 8; ++slot)
            write (kiyma::kPresetParamIds[slot], presetNormalised (index, slot));
        write (kiymaIds::crank, 0.0f);
    }
    activePreset.store (index);
    recalling.store (false);
}

bool KiymaProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void KiymaProcessor::prepareToPlay (double sampleRate, int)
{
    engine.prepare (sampleRate, getTotalNumOutputChannels());
    tailSeconds = 8.0;    // the wet path replays up to 4 bars of the past
    setLatencySamples (kiyma::kLatency);   // 0: the engine reads only what has already arrived
}

void KiymaProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();

    for (auto i = getTotalNumInputChannels(); i < getTotalNumOutputChannels(); ++i)
        buffer.clear (i, 0, numSamples);

    // Tempo and position from the host. Without a playhead the plug-in still works at 120 BPM.
    kiyma::Transport tr;
    if (auto* head = getPlayHead())
        if (auto pos = head->getPosition())
        {
            if (auto b = pos->getBpm()) tr.bpm = *b;
            if (auto q = pos->getPpqPosition()) tr.ppq = *q;
            if (auto ts = pos->getTimeSignature())
                if (ts->denominator > 0)
                    tr.beatsPerBar = 4.0 * (double) ts->numerator / (double) ts->denominator;
            tr.playing = pos->getIsPlaying();
        }
    tr.bpm = juce::jlimit (20.0, 999.0, tr.bpm);
    uiBpm = tr.bpm;

    kiyma::Params p;
    p.glitch = pGlitch->load();
    p.brutal = pBrutal->load();
    p.offset = pOffset->load();
    p.envelope = pEnvelope->load();
    p.attackMs = pAttack->load();
    p.releaseMs = pRelease->load();
    p.mix = pMix->load();
    p.crank = pCrank->load();
    p.outputDb = pOutput->load();

    auto rmsOf = [&buffer, numSamples] ()
    {
        if (numSamples == 0 || buffer.getNumChannels() == 0)
            return 0.0f;
        const float* d = buffer.getReadPointer (0);
        double s = 0.0;
        for (int i = 0; i < numSamples; ++i)
            s += (double) d[i] * d[i];
        const float r = (float) std::sqrt (s / numSamples);
        return std::isfinite (r) ? r : 0.0f;
    };
    uiInputRms.store (rmsOf(), std::memory_order_relaxed);

    engine.process (buffer.getArrayOfWritePointers(), juce::jmin (2, buffer.getNumChannels()), numSamples, p, tr);

    uiOutputRms.store (rmsOf(), std::memory_order_relaxed);
    {   // the editor's strands: output decimated, into the lock-free ring
        const float* d = buffer.getReadPointer (0);
        for (int i = 0; i < numSamples; ++i)
        {
            vizAcc += std::isfinite (d[i]) ? d[i] : 0.0f;
            if (++vizCount == kVizDecimate)
            {
                pushViz (vizAcc * (1.0f / (float) kVizDecimate));
                vizAcc = 0.0f;
                vizCount = 0;
            }
        }
    }
    uiBlocks.fetch_add (1, std::memory_order_relaxed);

    uiEnvelope = engine.envelopeLevel();
}

juce::AudioProcessorEditor* KiymaProcessor::createEditor()
{
    return new KiymaEditor (*this);
}

void KiymaProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    apvts.state.setProperty ("activePreset", activePreset.load(), nullptr);   // a property of the state tree, not an automatable parameter
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, dest);
}

void KiymaProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
        {
            recalling.store (true);
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
            int restored = (int) apvts.state.getProperty ("activePreset", -1);
            if (restored < 0 || restored >= 5)
                restored = -1;
            for (int slot = 0; restored >= 0 && slot < 8; ++slot)   // the values must still match the preset, else no switch is up
                if (kiyma::presetTrips (apvts.getParameter (kiyma::kPresetParamIds[slot])->getValue(), presetNormalised (restored, slot)))
                    restored = -1;
            activePreset.store (restored);
            recalling.store (false);
            undo.clear();   // the old steps describe a state that is gone
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new KiymaProcessor();
}
