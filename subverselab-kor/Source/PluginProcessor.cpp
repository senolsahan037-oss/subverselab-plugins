#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "SvlParamText.h"

namespace
{
juce::String pctText (float v, int) { return juce::String (juce::roundToInt (v * 100.0f)) + " %"; }
}

KorAudioProcessor::KorAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "ALEV", createLayout())
{
    pLow  = apvts.getRawParameterValue ("low");
    pMid  = apvts.getRawParameterValue ("mid");
    pHigh = apvts.getRawParameterValue ("high");
    pMix  = apvts.getRawParameterValue ("mix");
    setLatencySamples (kor::Multiband::latencySamples());
}

juce::AudioProcessorValueTreeState::ParameterLayout KorAudioProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;
    // Type-in: "25", "25 %", "25%" -> 25 % (family batch 2026-10-12; JUCE's default read "25 %" as a raw 25 and clamped to 100 %).
    const auto pct = svl::paramtext::percent (AudioParameterFloatAttributes().withStringFromValueFunction (pctText), 0.0f, 1.0f, 0.0f);
    const NormalisableRange<float> unit (0.0f, 1.0f, 0.001f);
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "low", 1 }, "LOW", unit, 0.30f, pct));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "mid", 1 }, "MID", unit, 0.30f, pct));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "high", 1 }, "HIGH", unit, 0.20f, pct));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "mix", 1 }, "MIX", unit, 1.00f, pct));
    // How the rings move together (owner, 8 Oct): FREE = each on its own; LINK = the next ring (LOW -> MID -> HIGH -> LOW)
    // follows the turned one 30 % behind (70 % of its move); GANG = all three turn together. A panel setting, saved with
    // the session, not automatable: it changes no sound.
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "link", 1 }, "LINK", StringArray { "FREE", "LINK", "GANG" }, 0,
                                                         AudioParameterChoiceAttributes().withAutomatable (false)));
    return { p.begin(), p.end() };
}

bool KorAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet(), out = layouts.getMainOutputChannelSet();
    if (in != out) return false;
    return out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
}

kor::MbParams KorAudioProcessor::readParams() const
{
    kor::MbParams p;
    p.low = pLow->load(); p.mid = pMid->load(); p.high = pHigh->load(); p.mix = pMix->load();
    return p;
}

void KorAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, samplesPerBlock, juce::jmax (1, getTotalNumOutputChannels()));
    engine.setParams (readParams());
    engine.reset();
    setLatencySamples (kor::Multiband::latencySamples());
    for (int c = 0; c < 2; ++c)
    {
        delayLine[c].assign ((size_t) kor::Multiband::latencySamples(), 0.0f);
        dryChunk[c].assign ((size_t) kChunk, 0.0f);
    }
    delayPos = 0;
    wetStep = 1.0f / (float) (0.020 * sampleRate);
    wetPrimed = false;
}

void KorAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    render (buffer, false);
}

void KorAudioProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    render (buffer, true);
}

void KorAudioProcessor::render (juce::AudioBuffer<float>& buffer, bool bypassed) noexcept
{
    juce::ScopedNoDenormals noDenormals;
    const int numIn = getTotalNumInputChannels(), numOut = getTotalNumOutputChannels();
    for (int c = numIn; c < numOut; ++c) buffer.clear (c, 0, buffer.getNumSamples());
    const int ch = juce::jmin (numOut, kor::Engine::kMaxChannels, 2);
    const int L = (int) delayLine[0].size();
    if (ch <= 0 || L == 0) return;
    const float target = bypassed ? 0.0f : 1.0f;
    if (! wetPrimed) { wet = target; wetPrimed = true; }
    engine.setParams (readParams());

    for (int off = 0; off < buffer.getNumSamples(); off += kChunk)
    {
        const int n = juce::jmin (kChunk, buffer.getNumSamples() - off);
        // The input delayed by the latency (taken before the engine overwrites the buffer).
        int pos = delayPos;
        for (int c = 0; c < ch; ++c)
        {
            const float* x = buffer.getReadPointer (c, off);
            auto& ring = delayLine[c];
            auto& dry = dryChunk[c];
            pos = delayPos;
            for (int i = 0; i < n; ++i)
            {
                dry[(size_t) i] = ring[(size_t) pos];
                ring[(size_t) pos] = x[i];
                if (++pos == L) pos = 0;
            }
        }
        delayPos = pos;

        float* ptrs[2] { buffer.getWritePointer (0, off), ch > 1 ? buffer.getWritePointer (1, off) : nullptr };
        engine.process (ptrs, ch, n);

        if (wet == target && target == 1.0f) continue;
        if (wet == target && target == 0.0f)
        {
            for (int c = 0; c < ch; ++c) std::copy (dryChunk[c].begin(), dryChunk[c].begin() + n, ptrs[c]);
            continue;
        }
        float w = wet;
        for (int i = 0; i < n; ++i)
        {
            w = target > w ? juce::jmin (target, w + wetStep) : juce::jmax (target, w - wetStep);
            for (int c = 0; c < ch; ++c) ptrs[c][i] = dryChunk[c][(size_t) i] + w * (ptrs[c][i] - dryChunk[c][(size_t) i]);
        }
        wet = w;
    }
}

juce::AudioProcessorEditor* KorAudioProcessor::createEditor()
{
    return new KorEditor (*this);
}

void KorAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml()) copyXmlToBinary (*xml, destData);
}

void KorAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType())) apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new KorAudioProcessor(); }
