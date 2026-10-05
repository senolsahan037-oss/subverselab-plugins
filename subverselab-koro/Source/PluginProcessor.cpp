#include "PluginProcessor.h"
#include "PluginEditor.h"

const juce::StringArray KoroProcessor::moodNames { "TIGHT DOUBLE", "ARABESK CHOIR", "BACKING STACK", "THIRD", "FIFTH", "CHOIR" };
const juce::StringArray KoroProcessor::keyNames { "AUTO", "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
const juce::StringArray KoroProcessor::scaleOverrideNames { "AUTO", "MAJOR", "MINOR", "NIHAVENT", "HICAZ", "RAST", "CHROM" };

namespace
{
constexpr int kSubBlock = 32;          // parameter smoothing granularity, samples
const char* const kLevelIds[3] { "level1", "level2", "level3" };
const char* const kPanIds[3] { "pan1", "pan2", "pan3" };

cifte::Controls moodControls (int index)
{
    cifte::Controls c;
    const bool ok = cifte::moods::get (cifte::moods::kNames[juce::jlimit (0, 5, index)], c);
    jassert (ok);
    juce::ignoreUnused (ok);
    return c;
}

/** The face value of every parameter a mood writes. */
struct MoodFace
{
    float dbl, width, tone, harmony, color, level[3], pan[3], glide, timing;
    bool monoSafe;
};

MoodFace faceOf (const cifte::Controls& c)
{
    MoodFace f {};
    f.dbl = juce::jlimit (0.0f, 10.0f, c.mix / KoroProcessor::kMixAtTen * 10.0f);
    f.width = c.width * 10.0f;
    f.tone = c.tone;
    f.harmony = c.harmony * 10.0f;
    f.color = juce::jlimit (0.0f, 1.0f, c.color);
    for (int v = 0; v < 3; ++v)
    {
        f.level[v] = juce::jlimit (0.0f, 10.0f, c.voiceLevel[(size_t) v] * 10.0f);
        f.pan[v] = juce::jlimit (-1.0f, 1.0f, c.voicePan[(size_t) v]);
    }
    f.glide = c.glide * 120.0f;
    f.timing = c.timing;
    f.monoSafe = c.monoSafe;
    return f;
}

juce::String colourText (float v)
{
    static const char* names[4] { "LOW 3RD", "HIGH 3RD", "5TH", "OCT" };
    const float x = v * 3.0f;
    const int near = juce::jlimit (0, 3, juce::roundToInt (x));
    if (std::abs (x - (float) near) < 0.05f)
        return names[near];
    const int a = juce::jlimit (0, 2, (int) std::floor (x));
    return juce::String (names[a]) + " > " + names[a + 1];
}
} // namespace

juce::AudioProcessorValueTreeState::ParameterLayout KoroProcessor::createLayout()
{
    using namespace juce;
    const MoodFace d = faceOf (moodControls (0));   // defaults = TIGHT DOUBLE
    AudioProcessorValueTreeState::ParameterLayout l;

    auto tenths = [] (const char* offText)
    {
        return AudioParameterFloatAttributes().withStringFromValueFunction ([offText] (float v, int)
                   { return v <= 0.0f && offText != nullptr ? String (offText) : String (v, 1); });
    };
    auto ms = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v)) + " ms"; });
    auto pan = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int)
                   { const int p = roundToInt (v * 100.0f); return p == 0 ? String ("C") : (p < 0 ? "L " : "R ") + String (std::abs (p)); });

    // Front.
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "double", 1 }, "Double", NormalisableRange<float> (0.0f, 10.0f), d.dbl, tenths ("OFF")));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "width", 1 }, "Width", NormalisableRange<float> (0.0f, 10.0f), d.width, tenths (nullptr)));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "tone", 1 }, "Tone", NormalisableRange<float> (0.0f, 1.0f), d.tone,
                                                  AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int)
                                                      { return String (roundToInt (3000.0 * std::pow (16.0 / 3.0, (double) v))) + " Hz"; })));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "harmony", 1 }, "Harmony", NormalisableRange<float> (0.0f, 10.0f), d.harmony, tenths ("OFF")));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "color", 1 }, "Color", NormalisableRange<float> (0.0f, 1.0f), d.color,
                                                  AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return colourText (v); })));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "output", 1 }, "Output", NormalisableRange<float> (-12.0f, 12.0f), 0.0f,
                                                  AudioParameterFloatAttributes().withLabel ("dB").withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " dB"; })));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "lock", 1 }, "Lock", false));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "bypass", 1 }, "Bypass", false));
    // MOOD writes other parameters when it is pressed, so it is a META parameter (auval checks this).
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "mood", 1 }, "Mood", moodNames, 0, AudioParameterChoiceAttributes().withMeta (true)));

    // Rear plate.
    for (int v = 0; v < 3; ++v)
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { kLevelIds[v], 1 }, "Level " + String (v + 1), NormalisableRange<float> (0.0f, 10.0f), d.level[v], tenths (nullptr)));
    for (int v = 0; v < 3; ++v)
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { kPanIds[v], 1 }, "Pan " + String (v + 1), NormalisableRange<float> (-1.0f, 1.0f), d.pan[v], pan));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "glide", 1 }, "Glide", NormalisableRange<float> (0.0f, 120.0f), d.glide, ms));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "timing", 1 }, "Timing", NormalisableRange<float> (0.0f, 30.0f), d.timing, ms));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "monoSafe", 1 }, "Mono Safe", d.monoSafe));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "keyOverride", 1 }, "Key Override", keyNames, 0));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "scaleOverride", 1 }, "Scale Override", scaleOverrideNames, 0));
    return l;
}

KoroProcessor::KoroProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "Koro", createLayout())
{
    for (int i = 0; i < 6; ++i)
        moods[i] = moodControls (i);
    bypassParam = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("bypass"));
    // A sensible latency before prepareToPlay, so hosts that ask early see the real number.
    engine.prepare (44100.0);
    latency = engine.latency();
    setLatencySamples (latency);
}

juce::AudioProcessorParameter* KoroProcessor::getBypassParameter() const { return bypassParam; }

bool KoroProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    const auto in = layouts.getMainInputChannelSet();
    if (out != juce::AudioChannelSet::stereo() && out != juce::AudioChannelSet::mono())
        return false;
    return in == out || (in == juce::AudioChannelSet::mono() && out == juce::AudioChannelSet::stereo());
}

void KoroProcessor::applyMood (int index)
{
    index = juce::jlimit (0, 5, index);
    svl::ParamUndo::Group oneStep (undo, "Mood " + moodNames[index]);   // every control the mood writes = one undo step
    const MoodFace f = faceOf (moods[index]);
    auto set = [this] (const char* id, float value)
    {
        auto* p = apvts.getParameter (id);
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 (value));
        p->endChangeGesture();
    };
    set ("mood", (float) index);
    set ("double", f.dbl);
    set ("width", f.width);
    set ("tone", f.tone);
    set ("harmony", f.harmony);
    set ("color", f.color);
    for (int v = 0; v < 3; ++v)
    {
        set (kLevelIds[v], f.level[v]);
        set (kPanIds[v], f.pan[v]);
    }
    set ("glide", f.glide);
    set ("timing", f.timing);
    set ("monoSafe", f.monoSafe ? 1.0f : 0.0f);
}

cifte::Controls KoroProcessor::targetControls() const
{
    // The mood supplies what has no knob (doubler SPREAD / TIGHT, the number of harmony
    // voices); every control on the face or the rear plate overrides the rest.
    cifte::Controls c = moods[juce::jlimit (0, 5, (int) param ("mood"))];
    const float dbl = param ("double");
    c.voices = doubleToVoices (dbl);
    c.mix = doubleToMix (dbl);
    c.width = param ("width") / 10.0f;
    c.tone = param ("tone");
    c.harmony = param ("harmony") / 10.0f;
    c.color = param ("color");
    c.keyLock = param ("lock") > 0.5f;
    for (int v = 0; v < 3; ++v)
    {
        c.voiceLevel[(size_t) v] = param (kLevelIds[v]) / 10.0f;
        c.voicePan[(size_t) v] = param (kPanIds[v]);
    }
    c.glide = param ("glide") / 120.0f;
    c.timing = param ("timing");
    c.monoSafe = param ("monoSafe") > 0.5f;
    c.shareWidthTone = true;
    c.forcedKeys = 0;

    const int keyOv = (int) param ("keyOverride"), scaleOv = (int) param ("scaleOverride");
    if (keyOv > 0 || scaleOv > 0)
    {
        // Manual: what is overridden is fixed, what is left on AUTO follows the detector.
        const auto& det = engine.keyDetector();
        c.keyMode = cifte::KeyMode::Manual;
        c.root = keyOv > 0 ? keyOv - 1 : (det.hasKey() ? det.root() : 0);
        c.scale = scaleOv > 0 ? scaleOv - 1 : (det.hasKey() ? det.scale() : 0);
        c.keyLock = false;
    }
    else
        c.keyMode = cifte::KeyMode::Auto;
    return c;
}

void KoroProcessor::prepareToPlay (double sr, int block)
{
    sampleRate = sr;
    engine.prepare (sr);
    latency = engine.latency();
    setLatencySamples (latency);
    tracker.prepare (sr, 70.0f, 1000.0f);
    tracker.reset();
    trackerFrames = 0;

    const int n = juce::jmax (block, 32);
    work.setSize (2, n);
    monoIn.setSize (1, n);
    dryDelay.setSize (2, juce::jmax (1, latency));
    dryDelay.clear();
    dryWrite = 0;

    bypassMix = param ("bypass") > 0.5f ? 1.0f : 0.0f;
    bypassStep = 1.0f / (0.02f * (float) sr);

    const auto c = targetControls();
    const float init[kNumSmooth] { param ("double"), c.width, c.tone, c.harmony, c.color, param ("output"),
                                   c.voiceLevel[0], c.voiceLevel[1], c.voiceLevel[2],
                                   c.voicePan[0], c.voicePan[1], c.voicePan[2], c.glide, c.timing };
    for (int i = 0; i < kNumSmooth; ++i)
    {
        smooth[i].reset (sr, 0.05);
        smooth[i].setCurrentAndTargetValue (init[i]);
    }
    lockLatched = false;
}

void KoroProcessor::runEngine (float* l, float* r, int n)
{
    const auto target = targetControls();
    const float targets[kNumSmooth] { param ("double"), target.width, target.tone, target.harmony, target.color, param ("output"),
                                      target.voiceLevel[0], target.voiceLevel[1], target.voiceLevel[2],
                                      target.voicePan[0], target.voicePan[1], target.voicePan[2], target.glide, target.timing };
    for (int i = 0; i < kNumSmooth; ++i)
        smooth[i].setTargetValue (targets[i]);

    for (int p = 0; p < n; p += kSubBlock)
    {
        const int m = juce::jmin (kSubBlock, n - p);
        float v[kNumSmooth];
        for (int i = 0; i < kNumSmooth; ++i)
            v[i] = smooth[i].skip (m);

        cifte::Controls c = target;
        c.voices = doubleToVoices (v[sDouble]);
        c.mix = doubleToMix (v[sDouble]);
        c.width = v[sWidth];
        c.tone = v[sTone];
        c.harmony = v[sHarmony];
        c.color = v[sColor];
        for (int k = 0; k < 3; ++k)
        {
            c.voiceLevel[(size_t) k] = v[sLevel1 + k];
            c.voicePan[(size_t) k] = v[sPan1 + k];
        }
        c.glide = v[sGlide];
        c.timing = v[sTiming];
        engine.process (l + p, r + p, m, c);

        const float gain = juce::Decibels::decibelsToGain (v[sOutput]);
        for (int i = p; i < p + m; ++i)
        {
            l[i] *= gain;
            r[i] *= gain;
        }
    }
}

void KoroProcessor::updateReadouts (const float* inMono, int n)
{
    // Pitch IN from our own tracker on the input (the engine's tracker is private).
    for (int i = 0; i < n; ++i)
        tracker.push (inMono[i]);
    trackerFrames = tracker.smoothedFrames();
    bool voiced = false;
    float logHz = 0.0f;
    if (trackerFrames > 0)
    {
        const auto f = tracker.smoothedFrame (trackerFrames - 1);   // the newest smoothed 5 ms frame
        voiced = f.voiced;
        logHz = f.logHz;
    }
    const auto& det = engine.keyDetector();
    const auto& st = engine.keyStatus();
    const float inCents = 6900.0f + 1200.0f * (logHz - std::log (440.0f)) / std::log (2.0f) - det.tuningCents();
    ui.pitchValid = voiced;
    ui.inCents = inCents;

    const auto c = targetControls();
    const bool manual = c.keyMode == cifte::KeyMode::Manual;
    const bool locked = ! manual && c.keyLock;
    if (locked && ! lockLatched)
    {
        lockHas = det.hasKey(); lockRoot = det.root(); lockScale = det.scale(); lockConf = det.confidence();
    }
    lockLatched = locked;

    if (manual)
    {
        ui.hasKey = true; ui.root = c.root; ui.scale = c.scale; ui.confidence = 1.0f;
    }
    else if (locked)
    {
        ui.hasKey = lockHas; ui.root = lockRoot; ui.scale = lockScale; ui.confidence = lockConf;
    }
    else
    {
        ui.hasKey = st.hasKey; ui.root = st.root; ui.scale = st.scale; ui.confidence = st.confidence;
    }
    ui.manual = manual;
    ui.locked = locked;
    ui.safe = c.harmony > 0.0f && st.safe;

    const bool harmonyOn = c.harmony > 0.0f;
    ui.outValid = harmonyOn && voiced;
    ui.outCents = inCents + (float) engine.voiceIntervalCents (0);
}

void KoroProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    if (n <= 0)
        return;
    if (n > work.getNumSamples())
    {
        work.setSize (2, n, false, false, true);
        monoIn.setSize (1, n, false, false, true);
    }
    const int inCh = juce::jmax (1, getTotalNumInputChannels());
    const int outCh = juce::jmin (buffer.getNumChannels(), getTotalNumOutputChannels());

    for (int ch = 0; ch < 2; ++ch)
        work.copyFrom (ch, 0, buffer, juce::jmin (ch, inCh - 1), 0, n);
    auto* L = work.getWritePointer (0);
    auto* R = work.getWritePointer (1);
    auto* mono = monoIn.getWritePointer (0);
    for (int i = 0; i < n; ++i)
        mono[i] = 0.5f * (L[i] + R[i]);

    bool licensed = true;
#if SVL_LICENSING
    licensed = license.isLicensed();   // no licence: the dry signal, delayed like everything else
#endif

    // The same latency on every path: the dry signal delayed by the engine latency.
    auto* dL = dryDelay.getWritePointer (0);
    auto* dR = dryDelay.getWritePointer (1);
    const int D = dryDelay.getNumSamples();
    float* outL = buffer.getWritePointer (0);
    float* outR = outCh > 1 ? buffer.getWritePointer (1) : nullptr;

    if (licensed)
        runEngine (L, R, n);
    updateReadouts (mono, n);

    const float bypassTarget = (! licensed || param ("bypass") > 0.5f) ? 1.0f : 0.0f;
    double sumL = 0.0, sumR = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const float inL = buffer.getSample (0, i);
        const float inR = inCh > 1 ? buffer.getSample (1, i) : inL;
        const float delL = dL[dryWrite], delR = dR[dryWrite];
        dL[dryWrite] = inL;
        dR[dryWrite] = inR;
        if (++dryWrite >= D) dryWrite = 0;

        if (bypassMix < bypassTarget) bypassMix = juce::jmin (bypassTarget, bypassMix + bypassStep);
        else if (bypassMix > bypassTarget) bypassMix = juce::jmax (bypassTarget, bypassMix - bypassStep);
        const float wl = licensed ? L[i] : delL, wr = licensed ? R[i] : delR;
        // Written so that a full bypass (1) is the delayed dry sample exactly.
        const float yl = wl * (1.0f - bypassMix) + delL * bypassMix;
        const float yr = wr * (1.0f - bypassMix) + delR * bypassMix;
        sumL += (double) yl * yl;
        sumR += (double) yr * yr;
        if (outR != nullptr) { outL[i] = yl; outR[i] = yr; }
        else outL[i] = 0.5f * (yl + yr);
    }
    for (int ch = 2; ch < buffer.getNumChannels(); ++ch)
        buffer.clear (ch, 0, n);
    ui.rmsL = (float) std::sqrt (sumL / n);
    ui.rmsR = (float) std::sqrt (outR != nullptr ? sumR / n : sumL / n);
}

void KoroProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    // Host bypass: the dry signal, still delayed by the reported latency.
    const int n = buffer.getNumSamples();
    const int inCh = juce::jmax (1, getTotalNumInputChannels());
    const int outCh = juce::jmin (buffer.getNumChannels(), getTotalNumOutputChannels());
    if (dryDelay.getNumSamples() < 1)
        return;
    auto* dL = dryDelay.getWritePointer (0);
    auto* dR = dryDelay.getWritePointer (1);
    const int D = dryDelay.getNumSamples();
    for (int i = 0; i < n; ++i)
    {
        const float inL = buffer.getSample (0, i);
        const float inR = inCh > 1 ? buffer.getSample (1, i) : inL;
        const float delL = dL[dryWrite], delR = dR[dryWrite];
        dL[dryWrite] = inL;
        dR[dryWrite] = inR;
        if (++dryWrite >= D) dryWrite = 0;
        if (outCh > 1) { buffer.setSample (0, i, delL); buffer.setSample (1, i, delR); }
        else buffer.setSample (0, i, 0.5f * (delL + delR));
    }
    ui.rmsL = 0.0f;
    ui.rmsR = 0.0f;
}

juce::AudioProcessorEditor* KoroProcessor::createEditor() { return new KoroEditor (*this); }

void KoroProcessor::getStateInformation (juce::MemoryBlock& data)
{
    auto state = apvts.copyState();
    state.setProperty ("editorWidth", editorWidth.load(), nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, data);
}

void KoroProcessor::setStateInformation (const void* data, int size)
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

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new KoroProcessor(); }
