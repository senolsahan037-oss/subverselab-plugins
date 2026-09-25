#include "PluginProcessor.h"
#include "PluginEditor.h"

const juce::StringArray KasetProcessor::generationNames { "MASTER", "1ST DUB", "2ND DUB", "3RD DUB", "4TH DUB" };
const juce::StringArray KasetProcessor::tapeNames { "NORMAL", "CHROME", "METAL" };

juce::AudioProcessorValueTreeState::ParameterLayout KasetProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout l;
    auto pct = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v * 100.0f)) + " %"; });
    auto dbAttr = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return (v > 0 ? "+" : "") + String (v, 1) + " dB"; });

    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "generation", 1 }, "Generation", generationNames, 2));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "tape", 1 }, "Tape", tapeNames, 0));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "input", 1 }, "Input", NormalisableRange<float> (-12.0f, 12.0f), 0.0f, dbAttr));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "drive", 1 }, "Saturation", NormalisableRange<float> (0.0f, 1.0f), 0.55f, pct));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "wow", 1 }, "Wow", NormalisableRange<float> (0.0f, 1.0f), 0.5f, pct));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "flutter", 1 }, "Flutter", NormalisableRange<float> (0.0f, 1.0f), 0.5f, pct));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "hiss", 1 }, "Hiss", NormalisableRange<float> (0.0f, 1.0f), 0.5f, pct));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "wear", 1 }, "Wear", NormalisableRange<float> (0.0f, 1.0f), 0.25f, pct));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "mix", 1 }, "Mix", NormalisableRange<float> (0.0f, 1.0f), 1.0f, pct));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "output", 1 }, "Output", NormalisableRange<float> (-12.0f, 12.0f), 0.0f, dbAttr));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "autoGain", 1 }, "Auto Gain", true));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "play", 1 }, "Tape In", true));   // off = BYPASS: the dry signal, latency-aligned
    return l;
}

KasetProcessor::KasetProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "Kaset", createLayout())
{
}

bool KasetProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    const auto in = layouts.getMainInputChannelSet();
    if (out != juce::AudioChannelSet::stereo() && out != juce::AudioChannelSet::mono())
        return false;
    return in == out || (in == juce::AudioChannelSet::mono() && out == juce::AudioChannelSet::stereo());
}

kaset::Controls KasetProcessor::controls() const
{
    auto v = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };
    kaset::Controls c;
    c.generation = (int) v ("generation") + 1;
    c.tape = (kaset::TapeType) (int) v ("tape");
    c.drive = v ("drive");
    c.wow = v ("wow");
    c.flutter = v ("flutter");
    c.hiss = v ("hiss");
    c.wear = v ("wear");
    return c;
}

void KasetProcessor::prepareToPlay (double sr, int block)
{
    sampleRate = sr;
    engine.prepare (sr);
    setLatencySamples (engine.latency());

    wet.setSize (2, block);
    for (auto& d : dryDelay) d.assign ((size_t) engine.latency() + 4, 0.0f);
    dw = 0;

    inGain.reset (sr, 0.05);
    outGain.reset (sr, 0.05);
    mixSmooth.reset (sr, 0.05);
    inGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (apvts.getRawParameterValue ("input")->load()));
    outGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (apvts.getRawParameterValue ("output")->load()));
    mixSmooth.setCurrentAndTargetValue (apvts.getRawParameterValue ("mix")->load());

    // Auto gain follows loudness over about 1.5 s, slow enough to leave dynamics alone.
    powerCoef = std::exp (-1.0 / (1.5 * sr));
    inPower = outPower = 0.0;
    autoGain = 1.0f;
    playGain = (apvts.getRawParameterValue ("play")->load() > 0.5f && license.isLicensed()) ? 1.0f : 0.0f;
    playStep = (float) (1.0 - std::exp (-1.0 / (0.02 * sr)));
}

void KasetProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    if (n > wet.getNumSamples())
        wet.setSize (2, n, false, false, true);

    const int inCh = getTotalNumInputChannels(), outCh = getTotalNumOutputChannels();
    inGain.setTargetValue (juce::Decibels::decibelsToGain (apvts.getRawParameterValue ("input")->load()));
    outGain.setTargetValue (juce::Decibels::decibelsToGain (apvts.getRawParameterValue ("output")->load()));
    mixSmooth.setTargetValue (apvts.getRawParameterValue ("mix")->load());
    const bool autoOn = apvts.getRawParameterValue ("autoGain")->load() > 0.5f;

    // Input gain goes into the tape; the dry path keeps the original level.
    float peak = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const float g = inGain.getNextValue();
        for (int c = 0; c < 2; ++c)
        {
            const float x = buffer.getSample (juce::jmin (c, inCh - 1), i);
            wet.setSample (c, i, x * g);
            peak = juce::jmax (peak, std::abs (x));
        }
    }

    engine.process (wet.getWritePointer (0), wet.getWritePointer (1), n, controls());

    // STOP bypasses the tape, fading over 20 ms; latency stays the same either way.
    // Without a licence the tape stays out: the same latency-aligned dry path as BYPASS.
    const float playTarget = (apvts.getRawParameterValue ("play")->load() > 0.5f && license.isLicensed()) ? 1.0f : 0.0f;

    const int N = (int) dryDelay[0].size(), lat = engine.latency();
    double sum[2] = { 0.0, 0.0 };
    for (int i = 0; i < n; ++i)
    {
        float dry[2];
        for (int c = 0; c < 2; ++c)
        {
            dryDelay[c][(size_t) dw] = buffer.getSample (juce::jmin (c, inCh - 1), i);
            int j = dw - lat;
            if (j < 0) j += N;
            dry[c] = dryDelay[c][(size_t) j];
        }
        if (++dw == N) dw = 0;

        // Loudness in (what went onto tape, aligned) against loudness out.
        const float wl = wet.getSample (0, i), wr = wet.getSample (1, i);
        inPower = powerCoef * inPower + (1.0 - powerCoef) * 0.5 * (dry[0] * dry[0] + dry[1] * dry[1]);
        outPower = powerCoef * outPower + (1.0 - powerCoef) * 0.5 * (wl * wl + wr * wr);
        if (autoOn && outPower > 1.0e-9 && inPower > 1.0e-9)
            autoGain = (float) juce::jlimit (0.25, 4.0, std::sqrt (inPower / outPower));   // ±12 dB: a 4th dub on bright material needs more than 6
        else if (! autoOn)
            autoGain += (1.0f - autoGain) * 0.001f;

        playGain += (playTarget - playGain) * playStep;
        // In BYPASS the output level is bypassed too: the signal leaves exactly as it came in.
        const float mix = mixSmooth.getNextValue() * playGain, og = 1.0f + (outGain.getNextValue() - 1.0f) * playGain;
        float o[2];
        for (int c = 0; c < 2; ++c)
        {
            o[c] = (dry[c] * (1.0f - mix) + wet.getSample (c, i) * autoGain * mix) * og;
            sum[c] += (double) o[c] * o[c];
        }
        if (outCh == 1)
            buffer.setSample (0, i, 0.5f * (o[0] + o[1]));
        else
            for (int c = 0; c < 2; ++c)
                buffer.setSample (c, i, o[c]);
    }

    for (int c = 0; c < 2; ++c)
        uiLevel[c] = (float) std::sqrt (sum[c] / juce::jmax (1, n));
    uiAutoGainDb = juce::Decibels::gainToDecibels (autoGain);

    silentBlocks = peak > 1.0e-4f ? 0 : silentBlocks + 1;
    const bool running = silentBlocks < (int) (0.5 * sampleRate / juce::jmax (1, n));
    uiRunning = running && playTarget > 0.5f;
    if (running && playTarget > 0.5f)
        uiTapeSeconds = uiTapeSeconds.load() + n / sampleRate;
}

juce::AudioProcessorEditor* KasetProcessor::createEditor() { return new KasetEditor (*this); }

void KasetProcessor::getStateInformation (juce::MemoryBlock& data)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, data);
}

void KasetProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new KasetProcessor(); }
