#include "PluginProcessor.h"
#include "PluginEditor.h"

TabanAudioProcessor::TabanAudioProcessor()
    : AudioProcessor (BusesProperties()
                        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "Parameters", createLayout())
{
}

juce::AudioProcessorValueTreeState::ParameterLayout TabanAudioProcessor::createLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // FLOOR: -60 is OFF. Linear in dB, so every 6 dB is the same length of travel.
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "floor", 1 }, "Floor",
        juce::NormalisableRange<float> (-60.0f, 0.0f, 0.1f), -60.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dBFS")
            .withStringFromValueFunction ([] (float v, int) { return v <= -59.95f ? juce::String ("OFF") : juce::String (v, 1); })
            .withValueFromStringFunction ([] (const juce::String& t) { return t.trim().equalsIgnoreCase ("off") ? -60.0f : t.getFloatValue(); })));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "output", 1 }, "Output",
        juce::NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));

    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "in", 1 }, "In", true));

    return { params.begin(), params.end() };
}

bool TabanAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& mainIn  = layouts.getMainInputChannelSet();
    const auto& mainOut = layouts.getMainOutputChannelSet();

    if (mainIn != mainOut) return false;
    return (mainIn == juce::AudioChannelSet::mono() || mainIn == juce::AudioChannelSet::stereo());
}

void TabanAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, samplesPerBlock);
    setLatencySamples (0);
}

svl::TabanEngine::Parameters TabanAudioProcessor::readParams() const noexcept
{
    svl::TabanEngine::Parameters p;
    p.floorDb  = apvts.getRawParameterValue ("floor")->load();
    p.outputDb = apvts.getRawParameterValue ("output")->load();
    p.enabled  = apvts.getRawParameterValue ("in")->load() > 0.5f;
    return p;
}

void TabanAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int totalNumInputChannels  = getTotalNumInputChannels();
    const int totalNumOutputChannels = getTotalNumOutputChannels();
    const int numSamples = buffer.getNumSamples();

    for (int i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear (i, 0, numSamples);

    auto p = readParams();
    const float* const* inPtrs = buffer.getArrayOfReadPointers();
    float* const* outPtrs = buffer.getArrayOfWritePointers();

    engine.process (inPtrs, outPtrs, totalNumInputChannels, numSamples, p);

    liftDb.store   (engine.getLiftDb(), std::memory_order_relaxed);
    inputDb.store  (engine.getDetectorDb(), std::memory_order_relaxed);
    const float op = engine.getOutputPeak();
    outputDb.store (op > 0.0f ? juce::Decibels::gainToDecibels (op, -200.0f) : -200.0f, std::memory_order_relaxed);
}

void TabanAudioProcessor::processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&)
{
    liftDb.store   (0.0f, std::memory_order_relaxed);
    inputDb.store  (-200.0f, std::memory_order_relaxed);
    outputDb.store (-200.0f, std::memory_order_relaxed);
}

void TabanAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void TabanAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));
    if (xmlState != nullptr && xmlState->hasTagName (apvts.state.getType()))
    {
        auto vt = juce::ValueTree::fromXml (*xmlState);
        apvts.replaceState (vt);
    }
}

juce::AudioProcessorEditor* TabanAudioProcessor::createEditor()
{
    return new TabanAudioProcessorEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TabanAudioProcessor();
}
