#include "PluginProcessor.h"
#include "PluginEditor.h"

const juce::StringArray HavaProcessor::moodNames { kPresetNames[0], kPresetNames[1], kPresetNames[2], kPresetNames[3], kPresetNames[4] };

namespace
{
constexpr int kChunk = 32;               // control-rate step while a knob is moving
constexpr double kSmoothSeconds = 0.05;  // knob smoothing
constexpr double kSafeFadeSeconds = 0.03;
constexpr double kBypassFadeSeconds = 0.02;

juce::String tenths (float v) { return juce::String (v * 10.0f, 1); }   // a 0..1 control on its 0..10 scale

/** MOOD writes AIR, SHEEN, MASTER SAFE and the rear controls: a meta parameter, so hosts expect those to move. */
class MoodParameter : public juce::AudioParameterChoice
{
public:
    using juce::AudioParameterChoice::AudioParameterChoice;
    bool isMetaParameter() const override { return true; }
};
} // namespace

int HavaProcessor::safeLatencyAt (double sr)
{
    return juce::jmax (1, (int) std::lround (0.0015 * sr)) + 8;   // = yay::TpLimiter latency (L + 8)
}

juce::AudioProcessorValueTreeState::ParameterLayout HavaProcessor::createLayout()
{
    using namespace juce;
    const auto d = yay::defaultControls();   // the state the plugin opens in: AIR FULL
    AudioProcessorValueTreeState::ParameterLayout l;

    auto scale10 = AudioParameterFloatAttributes()
                       .withStringFromValueFunction ([] (float v, int) { return tenths (v); })
                       .withValueFromStringFunction ([] (const String& s) { return jlimit (0.0f, 1.0f, s.getFloatValue() / 10.0f); });
    auto dbAttr = AudioParameterFloatAttributes()
                      .withStringFromValueFunction ([] (float v, int) { return (v > 0.0f ? "+" : "") + String (v, 1) + " dB"; })
                      .withValueFromStringFunction ([] (const String& s) { return jlimit (-12.0f, 12.0f, s.getFloatValue()); });
    auto hzAttr = AudioParameterFloatAttributes()
                      .withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v)) + " Hz"; })
                      .withValueFromStringFunction ([] (const String& s) { return jlimit (20.0f, 300.0f, s.getFloatValue()); });

    NormalisableRange<float> lowCut (20.0f, 300.0f);
    lowCut.setSkewForCentre (80.0f);   // the panel scale: 20 · 80 (top) · 300

    // front
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "air", 1 }, "Air", NormalisableRange<float> (0.0f, 1.0f), d.air, scale10));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "sheen", 1 }, "Sheen", NormalisableRange<float> (0.0f, 1.0f), d.sheen, scale10));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "output", 1 }, "Output", NormalisableRange<float> (-12.0f, 12.0f), d.output, dbAttr));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "bypass", 1 }, "Bypass", d.bypass));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "masterSafe", 1 }, "Master Safe", d.masterSafe));
    l.add (std::make_unique<MoodParameter> (ParameterID { "mood", 1 }, "Mood", moodNames, kDefaultMood));
    // rear
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "twang", 1 }, "Twang", NormalisableRange<float> (0.0f, 1.0f), d.twang, scale10));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "tension", 1 }, "Tension", NormalisableRange<float> (0.0f, 1.0f), d.tension, scale10));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "drip", 1 }, "Drip", NormalisableRange<float> (0.0f, 1.0f), d.drip, scale10));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "lowCut", 1 }, "Low Cut", lowCut, d.lowcut, hzAttr));
    return l;
}

HavaProcessor::HavaProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "Hava", createLayout())
{
    apvts.addParameterListener ("mood", this);
}

HavaProcessor::~HavaProcessor()
{
    apvts.removeParameterListener ("mood", this);
    cancelPendingUpdate();
}

bool HavaProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    const auto in = layouts.getMainInputChannelSet();
    if (out != juce::AudioChannelSet::stereo() && out != juce::AudioChannelSet::mono())
        return false;
    return in == out || (in == juce::AudioChannelSet::mono() && out == juce::AudioChannelSet::stereo());
}

//==============================================================================
void HavaProcessor::applyMood (int index)
{
    index = juce::jlimit (0, kNumMoods - 1, index);
    svl::ParamUndo::Group oneStep (undo, "Mood " + moodNames[index]);   // everything the mood writes = one undo step
    const auto c = preset (kPresetNames[index]);
    auto set = [this] (const char* id, float value)
    {
        auto* p = apvts.getParameter (id);
        const float norm = p->convertTo0to1 (value);
        if (juce::exactlyEqual (p->getValue(), norm))
            return;
        p->beginChangeGesture();
        p->setValueNotifyingHost (norm);
        p->endChangeGesture();
    };
    set ("air", c.air);
    set ("sheen", c.sheen);
    set ("masterSafe", c.masterSafe ? 1.0f : 0.0f);
    set ("twang", c.twang);
    set ("tension", c.tension);
    set ("drip", c.drip);
    set ("lowCut", c.lowcut);
}

void HavaProcessor::selectMood (int index)
{
    svl::ParamUndo::Group oneStep (undo, "Mood");   // the key press: the MOOD value and everything its recall writes
    auto* p = apvts.getParameter ("mood");
    if ((int) apvts.getRawParameterValue ("mood")->load() == index)
    {
        applyMood (index);   // the same key again: recall it again
        return;
    }
    p->beginChangeGesture();
    p->setValueNotifyingHost (p->convertTo0to1 ((float) index));   // the listener recalls it
    p->endChangeGesture();
}

void HavaProcessor::parameterChanged (const juce::String& id, float)
{
    if (id != "mood" || suppressMoodRecall.load())
        return;
    svl::ParamUndo::Derived derived (undo);   // host automation of MOOD must not leave steps; a key press keeps its own
    // A mood change writes other parameters, which belongs on the message thread.
    if (auto* mm = juce::MessageManager::getInstanceWithoutCreating(); mm != nullptr && mm->isThisTheMessageThread())
        applyMood ((int) apvts.getRawParameterValue ("mood")->load());
    else
        triggerAsyncUpdate();
}

void HavaProcessor::handleAsyncUpdate()
{
    svl::ParamUndo::Derived derived (undo);
    applyMood ((int) apvts.getRawParameterValue ("mood")->load());
}

//==============================================================================
yay::Controls HavaProcessor::targetControls() const
{
    auto v = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };
    yay::Controls c;
    c.air = v ("air");
    c.sheen = v ("sheen");
    c.output = v ("output");
    c.twang = v ("twang");
    c.tension = v ("tension");
    c.drip = v ("drip");
    c.lowcut = v ("lowCut");
    c.masterSafe = v ("masterSafe") > 0.5f;
    c.bypass = false;   // BYPASS is a crossfade in the plugin layer (see processBlock)
    return c;
}

void HavaProcessor::prepareToPlay (double sr, int block)
{
    sampleRate = sr;
    for (auto& e : engines)
        e.prepare (sr);
    jassert (engines[1].latency (true) == safeLatency());
    jassert (safeLatency() < kRing);

    const int size = juce::jmax (block, 512);
    input.setSize (2, size);
    for (auto& w : work)
        w.setSize (2, size);
    chunks.clear();
    chunks.reserve ((size_t) (size / kChunk + 2));
    std::memset (ring, 0, sizeof (ring));
    ringPos = 0;

    const auto c = targetControls();
    const std::pair<juce::SmoothedValue<float>*, float> sm[] { { &smAir, c.air }, { &smSheen, c.sheen }, { &smOutput, c.output },
                                                             { &smTwang, c.twang }, { &smTension, c.tension }, { &smDrip, c.drip },
                                                             { &smLowCut, c.lowcut } };
    for (auto [s, value] : sm)
    {
        s->reset (sr, kSmoothSeconds);
        s->setCurrentAndTargetValue (value);
    }

    safeActive = c.masterSafe;
    fadeFrom = -1;
    fadePos = 0;
    fadeLength = juce::jmax (1, (int) std::lround (kSafeFadeSeconds * sr));
    bypassGain = apvts.getRawParameterValue ("bypass")->load() > 0.5f ? 1.0f : 0.0f;
    bypassStep = (float) (1.0 / (kBypassFadeSeconds * sr));
    inputRefDb = -120.0f;
    uiSafeActive = safeActive;
    setLatencySamples (safeActive ? safeLatency() : 0);
}

void HavaProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    const int inCh = juce::jmax (1, getTotalNumInputChannels()), outCh = getTotalNumOutputChannels();
    if (n <= 0)
        return;

#if SVL_LICENSING
    // No licence, no processing: the audio passes through untouched.
    if (! license.isLicensed())
    {
        if (inCh == 1 && buffer.getNumChannels() > 1)
            buffer.copyFrom (1, 0, buffer, 0, 0, n);
        return;
    }
#endif

    if (n > input.getNumSamples())
    {
        input.setSize (2, n, false, false, true);
        for (auto& w : work)
            w.setSize (2, n, false, false, true);
    }
    for (int c = 0; c < 2; ++c)
        input.copyFrom (c, 0, buffer, juce::jmin (c, inCh - 1), 0, n);

    // MASTER SAFE: start a crossfade to the other engine; the latency follows the mode.
    const auto target = targetControls();
    if (target.masterSafe != safeActive && fadeFrom < 0)
    {
        fadeFrom = safeActive ? 1 : 0;
        safeActive = target.masterSafe;
        engines[safeActive ? 1 : 0].reset();
        fadePos = 0;
        setLatencySamples (safeActive ? safeLatency() : 0);
    }
    uiSafeActive = safeActive;
    const int active = safeActive ? 1 : 0;

    // Knob smoothing: block-constant engine controls, stepped every 32 samples while a knob moves.
    smAir.setTargetValue (target.air);
    smSheen.setTargetValue (target.sheen);
    smOutput.setTargetValue (target.output);
    smTwang.setTargetValue (target.twang);
    smTension.setTargetValue (target.tension);
    smDrip.setTargetValue (target.drip);
    smLowCut.setTargetValue (target.lowcut);
    juce::SmoothedValue<float>* smoothers[] { &smAir, &smSheen, &smOutput, &smTwang, &smTension, &smDrip, &smLowCut };
    const bool moving = std::any_of (std::begin (smoothers), std::end (smoothers), [] (auto* s) { return s->isSmoothing(); });
    chunks.clear();
    for (int pos = 0; pos < n;)
    {
        const int len = moving ? juce::jmin (kChunk, n - pos) : n - pos;
        Chunk ch;
        ch.start = pos;
        ch.length = len;
        ch.controls = target;
        ch.controls.air = smAir.skip (len);
        ch.controls.sheen = smSheen.skip (len);
        ch.controls.output = smOutput.skip (len);
        ch.controls.twang = smTwang.skip (len);
        ch.controls.tension = smTension.skip (len);
        ch.controls.drip = smDrip.skip (len);
        ch.controls.lowcut = smLowCut.skip (len);
        chunks.push_back (ch);
        pos += len;
    }

    // Run the engine(s): the active mode always (kept warm under BYPASS), the old one while fading out.
    double airLift = 0.0, sheenPower[2] { 0.0, 0.0 };
    for (int role = 0; role < 2; ++role)
    {
        if (role != active && role != fadeFrom)
            continue;
        auto* l = work[role].getWritePointer (0);
        auto* r = work[role].getWritePointer (1);
        std::copy (input.getReadPointer (0), input.getReadPointer (0) + n, l);
        std::copy (input.getReadPointer (1), input.getReadPointer (1) + n, r);
        for (const auto& ch : chunks)
        {
            auto c = ch.controls;
            c.masterSafe = role == 1;
            engines[role].process (l + ch.start, r + ch.start, ch.length, c);
            if (role == active)
            {
                airLift += (double) engines[role].airLiftDb() * ch.length;
                for (int k = 0; k < 2; ++k)
                    sheenPower[k] += std::pow (10.0, engines[role].sheenLevelDb (k) / 10.0) * ch.length;
            }
        }
    }

    // Mix: BYPASS fade against the dry of each mode, then the MASTER SAFE crossfade.
    const float bypassTarget = apvts.getRawParameterValue ("bypass")->load() > 0.5f ? 1.0f : 0.0f;
    const bool steady = fadeFrom < 0 && juce::exactlyEqual (bypassGain, bypassTarget);
    const int lat = safeLatency();
    const float* in[2] { input.getReadPointer (0), input.getReadPointer (1) };
    const float* wet[2][2] { { work[0].getReadPointer (0), work[0].getReadPointer (1) },
                             { work[1].getReadPointer (0), work[1].getReadPointer (1) } };
    double inPower = 0.0;

    if (steady && active == 0 && juce::exactlyEqual (bypassGain, 1.0f))
    {
        // Bypassed, MASTER SAFE off: the input leaves untouched. Keep the dry delay warm.
        for (int i = 0; i < n; ++i)
        {
            for (int c = 0; c < 2; ++c)
                ring[c][ringPos] = in[c][i];
            ringPos = (ringPos + 1) & (kRing - 1);
            inPower += 0.25 * ((double) in[0][i] + in[1][i]) * ((double) in[0][i] + in[1][i]);
        }
        if (inCh == 1 && outCh > 1)
            buffer.copyFrom (1, 0, buffer, 0, 0, n);
    }
    else
    {
        for (int i = 0; i < n; ++i)
        {
            float delayed[2];
            for (int c = 0; c < 2; ++c)
            {
                ring[c][ringPos] = in[c][i];
                delayed[c] = ring[c][(ringPos - lat) & (kRing - 1)];
            }
            ringPos = (ringPos + 1) & (kRing - 1);
            inPower += 0.25 * ((double) in[0][i] + in[1][i]) * ((double) in[0][i] + in[1][i]);

            if (! juce::exactlyEqual (bypassGain, bypassTarget))
                bypassGain = bypassTarget > bypassGain ? juce::jmin (bypassTarget, bypassGain + bypassStep)
                                                       : juce::jmax (bypassTarget, bypassGain - bypassStep);
            const float b = bypassGain;
            auto mode = [&] (int role, int c)
            {
                const float w = wet[role][c][i], dry = role == 1 ? delayed[c] : in[c][i];
                if (juce::exactlyEqual (b, 0.0f)) return w;
                if (juce::exactlyEqual (b, 1.0f)) return dry;
                return dry * b + w * (1.0f - b);
            };

            float o[2];
            if (fadeFrom >= 0)
            {
                const float x = (float) fadePos / (float) fadeLength;
                for (int c = 0; c < 2; ++c)
                    o[c] = mode (fadeFrom, c) * (1.0f - x) + mode (active, c) * x;
                if (++fadePos >= fadeLength)
                    fadeFrom = -1;
            }
            else
            {
                for (int c = 0; c < 2; ++c)
                    o[c] = mode (active, c);
            }

            if (outCh == 1)
                buffer.setSample (0, i, 0.5f * (o[0] + o[1]));
            else
                for (int c = 0; c < juce::jmin (2, outCh); ++c)
                    buffer.setSample (c, i, o[c]);
        }
    }

    // Meters for the display.
    const double seconds = (double) n / sampleRate;
    const float inDb = (float) (10.0 * std::log10 (inPower / n + 1.0e-30));
    inputRefDb = juce::jmax (inDb, inputRefDb - (float) (20.0 * seconds));   // the input level, falling 20 dB/s
    if (juce::exactlyEqual (bypassGain, 1.0f))
    {
        uiAirLiftDb = 0.0f;
        uiSheenReDb[0] = uiSheenReDb[1] = -120.0f;
    }
    else
    {
        uiAirLiftDb = (float) (airLift / n);
        for (int k = 0; k < 2; ++k)
        {
            const float sheenDb = (float) (10.0 * std::log10 (sheenPower[k] / n + 1.0e-30));
            uiSheenReDb[k] = (sheenDb <= -119.0f || inputRefDb < -90.0f) ? -120.0f : sheenDb - inputRefDb;
        }
    }
    uiAutoGainDb = engines[1].autoGainDb();
}

//==============================================================================
juce::AudioProcessorEditor* HavaProcessor::createEditor() { return new HavaEditor (*this); }

void HavaProcessor::getStateInformation (juce::MemoryBlock& data)
{
    auto state = apvts.copyState();
    state.setProperty ("editorWidth", editorWidth, nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, data);
}

void HavaProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
        {
            // A saved session keeps its own values: restoring the mood must not recall its preset over them.
            suppressMoodRecall = true;
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
            suppressMoodRecall = false;
            undo.clear();   // the old steps describe a state that is gone
            editorWidth = (int) apvts.state.getProperty ("editorWidth", 0);
            setLatencySamples (apvts.getRawParameterValue ("masterSafe")->load() > 0.5f ? safeLatency() : 0);
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new HavaProcessor(); }
