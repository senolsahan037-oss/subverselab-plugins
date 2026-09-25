#include "PluginProcessor.h"
#include "PluginEditor.h"

const juce::StringArray KubbeProcessor::placeNames { "Hammam", "Cistern", "Valley" };
const juce::StringArray KubbeProcessor::placeLines {
    "Marble dome. Dense, bright, a flutter between the walls.",
    "Underground stone and water. Late column slaps, a long dark tail.",
    "Open air. The far slopes answer one by one, each darker."
};
const juce::StringArray KubbeProcessor::noteNames { "ms", "1/64", "1/32", "1/16", "1/8", "1/4" };

double KubbeProcessor::noteBeats (int index)
{
    return index <= 0 ? 0.0 : 4.0 / (double) (64 >> (index - 1));
}

namespace
{
juce::NormalisableRange<float> hzRange (float lo, float hi, float centre)
{
    juce::NormalisableRange<float> r (lo, hi);
    r.setSkewForCentre (centre);
    return r;
}
} // namespace

juce::AudioProcessorValueTreeState::ParameterLayout KubbeProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout l;
    auto pct = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v * 100.0f)) + " %"; });
    auto hz  = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int)
                   { return v >= 1000.0f ? String (v / 1000.0f, 1) + " kHz" : String (roundToInt (v)) + " Hz"; });

    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "place", 1 }, "Place", placeNames, 0));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "mix", 1 }, "Mix", NormalisableRange<float> (0.0f, 1.0f), 0.3f, pct));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "decay", 1 }, "Decay",
                                                  NormalisableRange<float> (0.25f, 3.0f, 0.0f, 0.5f), 1.0f,
                                                  AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return "x" + String (v, 2); })));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "predelay", 1 }, "Pre-delay",
                                                  NormalisableRange<float> (0.0f, 250.0f, 0.0f, 0.5f), 0.0f,
                                                  AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v)) + " ms"; })));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "predelaySync", 1 }, "Pre-delay Sync", noteNames, 0));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "distance", 1 }, "Distance", NormalisableRange<float> (0.0f, 1.0f), 0.5f, pct));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "lowCut", 1 }, "Low Cut", hzRange (20.0f, 1000.0f, 150.0f), 20.0f, hz));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "highCut", 1 }, "High Cut", hzRange (1000.0f, 20000.0f, 6000.0f), 20000.0f, hz));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "width", 1 }, "Width", NormalisableRange<float> (0.0f, 1.5f), 1.0f, pct));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "duck", 1 }, "Duck", NormalisableRange<float> (0.0f, 1.0f), 0.0f, pct));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "freeze", 1 }, "Freeze", false));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "echoSync", 1 }, "Echo Sync", false));
    return l;
}

KubbeProcessor::KubbeProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "Kubbe", createLayout())
{
}

bool KubbeProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    const auto in = layouts.getMainInputChannelSet();
    if (out != juce::AudioChannelSet::stereo() && out != juce::AudioChannelSet::mono())
        return false;
    return in == out || (in == juce::AudioChannelSet::mono() && out == juce::AudioChannelSet::stereo());
}

svrb::Controls KubbeProcessor::controlsFor (double bpm) const
{
    auto v = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };
    svrb::Controls c;
    c.decay = v ("decay");
    const double beats = noteBeats ((int) v ("predelaySync"));
    c.predelayMs = beats > 0.0 ? (float) (beats * 60000.0 / bpm) : v ("predelay");
    c.distance = v ("distance");
    c.lowCutHz = v ("lowCut");
    c.highCutHz = v ("highCut");
    c.width = v ("width");
    c.freeze = v ("freeze") > 0.5f;
    c.bpm = v ("echoSync") > 0.5f ? (float) bpm : 0.0f;
    return c;
}

void KubbeProcessor::prepareToPlay (double sr, int block)
{
    sampleRate = sr;
    for (int i = 0; i < svrb::kNumCharacters; ++i)
        engines[i].prepare (sr, svrb::characterAt (i));
    current = (int) apvts.getRawParameterValue ("place")->load();
    fading = -1;

    dry.setSize (2, block);
    wet.setSize (2, block);
    fadeWet.setSize (2, block);
    silence.setSize (2, block);
    silence.clear();

    mixSmooth.reset (sr, 0.05);
    mixSmooth.setCurrentAndTargetValue (apvts.getRawParameterValue ("mix")->load());
    duckEnv = 0.0f;
    duckAttack = std::exp (-1.0f / (0.005f * (float) sr));
    duckRelease = std::exp (-1.0f / (0.30f * (float) sr));
}

void KubbeProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    if (n > dry.getNumSamples())
    {
        dry.setSize (2, n, false, false, true);
        wet.setSize (2, n, false, false, true);
        fadeWet.setSize (2, n, false, false, true);
        silence.setSize (2, n, false, false, true);
        silence.clear();
    }

    double bpm = 120.0;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
            if (auto b = pos->getBpm())
                bpm = juce::jlimit (20.0, 400.0, *b);
    uiBpm = bpm;

    // No licence, no processing: the audio passes through untouched.
    if (! license.isLicensed())
    {
        if (getTotalNumInputChannels() == 1 && buffer.getNumChannels() > 1)
            buffer.copyFrom (1, 0, buffer, 0, 0, n);
        uiDuck = 0.0f;
        return;
    }

    // Mono in: copy to both sides.
    const int inCh = getTotalNumInputChannels();
    for (int c = 0; c < 2; ++c)
        dry.copyFrom (c, 0, buffer, juce::jmin (c, inCh - 1), 0, n);

    // A new place: the old one keeps ringing out without input.
    const int wanted = (int) apvts.getRawParameterValue ("place")->load();
    if (wanted != current)
    {
        fading = current;
        fadeGain = 1.0f;
        fadeStep = 1.0f / (1.5f * (float) sampleRate);
        current = wanted;
        engines[current].reset();
    }

    const auto ctl = controlsFor (bpm);
    engines[current].update (ctl);
    engines[current].process (dry.getReadPointer (0), dry.getReadPointer (1),
                              wet.getWritePointer (0), wet.getWritePointer (1), n);

    if (fading >= 0)
    {
        engines[fading].process (silence.getReadPointer (0), silence.getReadPointer (1),
                                 fadeWet.getWritePointer (0), fadeWet.getWritePointer (1), n);
        for (int i = 0; i < n; ++i)
        {
            fadeGain = juce::jmax (0.0f, fadeGain - fadeStep);
            for (int c = 0; c < 2; ++c)
                wet.addSample (c, i, fadeWet.getSample (c, i) * fadeGain);
        }
        if (fadeGain <= 0.0f)
            fading = -1;
    }

    mixSmooth.setTargetValue (apvts.getRawParameterValue ("mix")->load());
    const float duck = apvts.getRawParameterValue ("duck")->load();
    const int outCh = getTotalNumOutputChannels();
    float duckedMost = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        // Ducking follows the dry signal: the tail steps back while the source plays.
        const float level = juce::jmax (std::abs (dry.getSample (0, i)), std::abs (dry.getSample (1, i)));
        const float coef = level > duckEnv ? duckAttack : duckRelease;
        duckEnv = level + coef * (duckEnv - level);
        // Up to 24 dB down at full Duck, scaled by how loud the source is (full at -12 dBFS).
        const float ducked = 1.0f - juce::Decibels::decibelsToGain (-24.0f * duck * juce::jmin (1.0f, duckEnv * 4.0f));
        duckedMost = juce::jmax (duckedMost, ducked);

        const float mix = mixSmooth.getNextValue();
        const float dryGain = std::cos (mix * juce::MathConstants<float>::halfPi);
        const float wetGain = std::sin (mix * juce::MathConstants<float>::halfPi) * (1.0f - ducked);
        for (int c = 0; c < outCh; ++c)
        {
            const float d = outCh == 1 ? 0.5f * (dry.getSample (0, i) + dry.getSample (1, i)) : dry.getSample (c, i);
            const float w = outCh == 1 ? 0.5f * (wet.getSample (0, i) + wet.getSample (1, i)) : wet.getSample (c, i);
            buffer.setSample (c, i, d * dryGain + w * wetGain);
        }
    }
    uiDuck = duckedMost;
}

juce::AudioProcessorEditor* KubbeProcessor::createEditor() { return new KubbeEditor (*this); }

void KubbeProcessor::getStateInformation (juce::MemoryBlock& data)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, data);
}

void KubbeProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new KubbeProcessor(); }
