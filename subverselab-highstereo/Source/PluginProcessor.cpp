#include "PluginProcessor.h"
#include "PluginEditor.h"

HighStereoAudioProcessor::HighStereoAudioProcessor()
    : AudioProcessor (BusesProperties()
                        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "Parameters", createLayout())
{
}

juce::AudioProcessorValueTreeState::ParameterLayout HighStereoAudioProcessor::createLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;
    auto pct = juce::AudioParameterFloatAttributes().withLabel ("%")
                   .withStringFromValueFunction ([] (float v, int) { return juce::String (juce::roundToInt (v)); });

    params.push_back (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "in", 1 }, "In", true));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "width", 1 }, "Width", juce::NormalisableRange<float> (0.0f, 200.0f, 1.0f), 100.0f, pct));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "spread", 1 }, "Spread", juce::NormalisableRange<float> (0.0f, 100.0f, 1.0f), 0.0f, pct));

    // MONO BASS: 30 reads OFF; 40 .. 300 Hz above it, log-ish travel.
    juce::NormalisableRange<float> mb (30.0f, 300.0f, 1.0f);
    mb.setSkewForCentre (100.0f);
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "monobass", 1 }, "Mono Bass", mb, 30.0f,
        juce::AudioParameterFloatAttributes().withLabel ("Hz")
            .withStringFromValueFunction ([] (float v, int) { return v < 40.0f ? juce::String ("OFF") : juce::String (juce::roundToInt (v)); })
            .withValueFromStringFunction ([] (const juce::String& t) { return t.trim().equalsIgnoreCase ("off") ? 30.0f : t.getFloatValue(); })));

    // Band widths: dragged on the STEREO FIELD, on top of WIDTH.
    static const char* bandNames[] = { "<100", "100-250", "250-500", "500-1k", "1k-2k", "2k-4k", "4k-8k", "8k+" };
    for (int k = 0; k < kBands; ++k)
        params.push_back (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { bandId (k), 1 }, juce::String ("Width ") + bandNames[k],
            juce::NormalisableRange<float> (0.0f, 200.0f, 1.0f), 100.0f, pct));

    // FIX: one key. No band may go past correlation 0 (vanish in mono).
    params.push_back (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "fix", 1 }, "Fix", false));

    params.push_back (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "match", 1 }, "Match", false));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "mix", 1 }, "Mix", juce::NormalisableRange<float> (0.0f, 100.0f, 1.0f), 100.0f, pct));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "output", 1 }, "Output", juce::NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));

    // STAGE: where the centre of the image sits on the 360 stage; set by dragging the puck inside the goniometer.
    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "stage", 1 }, "Stage", juce::NormalisableRange<float> (-180.0f, 180.0f, 0.1f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("deg")));

    return { params.begin(), params.end() };
}

bool HighStereoAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in = layouts.getMainInputChannelSet();
    if (in != layouts.getMainOutputChannelSet()) return false;
    return in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo();
}

void HighStereoAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, samplesPerBlock);
    rate.store (sampleRate, std::memory_order_relaxed);
    setLatencySamples (0);
}

svl::HighStereoEngine::Parameters HighStereoAudioProcessor::readParams() const noexcept
{
    svl::HighStereoEngine::Parameters p;
    // The host stores parameters as 0..1 floats, so a value set to 0 dB / 0 deg / 100 % reads back a few 1e-7 off
    // (OUTPUT 1.8e-7 dB, STAGE 2.7e-6 deg seen on Default). Snap those to the exact neutral value so Default is the
    // untouched path. Tolerances are far below one control step (0.1 % / 0.1 dB / 1 deg).
    const auto snap = [] (double v, double neutral, double tol) { return std::abs (v - neutral) < tol ? neutral : v; };
    p.enabled  = apvts.getRawParameterValue ("in")->load() > 0.5f;
    p.width    = snap (apvts.getRawParameterValue ("width")->load() / 100.0, 1.0, 5.0e-5);
    p.spread   = snap (apvts.getRawParameterValue ("spread")->load() / 100.0, 0.0, 5.0e-5);
    const float mb = apvts.getRawParameterValue ("monobass")->load();
    p.monoBassHz = mb < 40.0f ? 0.0 : (double) mb;
    for (int k = 0; k < kBands; ++k)
        p.bandWidth[(size_t) k] = snap (apvts.getRawParameterValue (bandId (k))->load() / 100.0, 1.0, 5.0e-5);
    p.fix      = apvts.getRawParameterValue ("fix")->load() > 0.5f;
    p.match    = apvts.getRawParameterValue ("match")->load() > 0.5f;
    p.outputDb = snap (apvts.getRawParameterValue ("output")->load(), 0.0, 0.005);
    p.mix      = snap (apvts.getRawParameterValue ("mix")->load() / 100.0, 1.0, 5.0e-5);
    p.stageDeg = snap (apvts.getRawParameterValue ("stage")->load(), 0.0, 0.05);
    p.listen   = getListen();
    return p;
}

void HighStereoAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int nIn = getTotalNumInputChannels(), nOut = getTotalNumOutputChannels(), n = buffer.getNumSamples();
    for (int i = nIn; i < nOut; ++i) buffer.clear (i, 0, n);

    // LIMIT peaks reset: on request (panel click, preset load) and when the host transport starts playing.
    bool playing = false;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition()) playing = pos->getIsPlaying();
    if ((playing && ! wasPlaying) || peakResetRequest.exchange (false, std::memory_order_relaxed)) engine.resetPeaks();
    wasPlaying = playing;

    engine.process (buffer.getArrayOfReadPointers(), buffer.getArrayOfWritePointers(), nIn, n, readParams());
    if (nIn >= 1)
    {
        const float* o0 = buffer.getReadPointer (0); const float* o1 = nIn >= 2 ? buffer.getReadPointer (1) : o0;
        const int c = engine.preStageCount();
        if (c == n) ringPre.push (engine.preStage (0), engine.preStage (1), n); else ringPre.push (o0, o1, n);
        ringPost.push (o0, o1, n);
    }

    for (int k = 0; k < kBands; ++k)
    {
        const auto r = engine.band (k);
        bands[(size_t) k].corr.store (r.correlation, std::memory_order_relaxed);
        bands[(size_t) k].lossDb.store (r.monoLossDb, std::memory_order_relaxed);
        bands[(size_t) k].fixDb.store (r.fixGainDb, std::memory_order_relaxed);
        bands[(size_t) k].levelDb.store (r.levelDb, std::memory_order_relaxed);
        bands[(size_t) k].overflow.store (r.overflow, std::memory_order_relaxed);
        auto& v = bands[(size_t) k];
        v.liveWidth.store (r.liveWidth, std::memory_order_relaxed);   v.liveLevelDb.store (r.liveLevelDb, std::memory_order_relaxed);
        v.liveLossDb.store (r.liveLossDb, std::memory_order_relaxed); v.liveBalance.store (r.liveBalance, std::memory_order_relaxed);
        v.limit.store (r.limit, std::memory_order_relaxed);           v.peakFill.store (r.peakFill, std::memory_order_relaxed);
        v.noSide.store (r.noSide, std::memory_order_relaxed);         v.learned.store (r.learned, std::memory_order_relaxed);
    }
    correlation.store (engine.correlation(), std::memory_order_relaxed);
    matchDb.store (engine.matchDb(), std::memory_order_relaxed);
    {   // keep the deepest guard reduction until the panel reads it (blocks are shorter than the panel's 33 ms tick)
        const float g = engine.guardDb();
        float cur = guardDeepest.load (std::memory_order_relaxed);
        while (g < cur && ! guardDeepest.compare_exchange_weak (cur, g, std::memory_order_relaxed)) {}
    }
}

void HighStereoAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty ("preset", presets.currentName(), nullptr);   // the panel shows it again when the session reopens
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void HighStereoAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml != nullptr && xml->hasTagName (apvts.state.getType()))
    {
        apvts.replaceState (juce::ValueTree::fromXml (*xml));
        presets.setCurrentName (apvts.state.getProperty ("preset", PresetManager::kDefault).toString());
        presets.refreshLoaded();
        requestPeakReset();
    }
    setListen (svl::HighStereoEngine::Listen::Off);   // a session never opens with a monitoring filter on
    undo.clear();
}

juce::AudioProcessorEditor* HighStereoAudioProcessor::createEditor()
{
    return new HighStereoAudioProcessorEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new HighStereoAudioProcessor();
}
