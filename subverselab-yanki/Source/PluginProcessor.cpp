#include "PluginProcessor.h"
#include "PluginEditor.h"

const juce::StringArray YankiProcessor::moodNames { "Echo", "Slapback", "Three Heads", "Dub Spiral", "Worn Loop", "Yesilcam", "Ters" };
const juce::StringArray YankiProcessor::syncNames { "1/16", "1/8", "1/8 dotted", "1/4", "1/4 dotted", "1/2", "Free" };
const juce::StringArray YankiProcessor::endNames { "Hold", "Cut", "Dump" };
const juce::StringArray YankiProcessor::riseNames { "Slow", "Med", "Fast" };
const juce::StringArray YankiProcessor::lockNames { "Off", "1 bar", "2 bars", "4 bars" };
const juce::StringArray YankiProcessor::headsNames { "Mood", "1 head", "2 heads", "3 heads" };

namespace
{
constexpr double kKnobHold = yanki::YankiEngine::kKnobHold;   // 0.7: FEEDBACK knob position of the hold point

// FEEDBACK is stored as the engine knob (0..1, HOLD at 0.7); its normalised
// value is the face position (HOLD at 12 o'clock = 0.5), so hosts and the
// panel agree on where HOLD is.
float fbFrom0to1 (float, float, float n)
{
    return n < 0.5f ? n * (float) (kKnobHold / 0.5) : (float) kKnobHold + (n - 0.5f) * (float) ((1.0 - kKnobHold) / 0.5);
}
float fbTo0to1 (float, float, float k)
{
    return k < (float) kKnobHold ? k * (float) (0.5 / kKnobHold) : 0.5f + (k - (float) kKnobHold) * (float) (0.5 / (1.0 - kKnobHold));
}

// A parameter that sits on the MOOD's own value (as stored in float) hands
// the engine the mood's exact double, so a recalled mood renders bit-identical
// to yanki::moodControls() (the md5-checked presets).
double snapTo (float value, double moodValue, double span)
{
    return std::abs ((double) value - moodValue) <= 1.0e-5 * span ? moodValue : (double) value;
}

// A NaN or infinity in the tape loop never washes out (the loop feeds itself), so one such sample from the
// host would leave every control dead until the plug-in is reloaded. Tested on the bits, so fast-math cannot remove it.
inline bool nonFinite (float x)
{
    uint32_t b;
    std::memcpy (&b, &x, sizeof b);
    return (b & 0x7f800000u) == 0x7f800000u;
}

juce::AudioParameterFloatAttributes withText (std::function<juce::String (float)> f)
{
    return juce::AudioParameterFloatAttributes().withStringFromValueFunction ([f] (float v, int) { return f (v); });
}
} // namespace

juce::AudioProcessorValueTreeState::ParameterLayout YankiProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout l;
    const auto echo = yanki::moodControls (0);
    auto pct = withText ([] (float v) { return String (roundToInt (v * 100.0f)) + " %"; });
    auto db = withText ([] (float v) { return (v > 0.0f ? "+" : "") + String (v, 1) + " dB"; });

    // A meta parameter: recalling a MOOD writes TONE, WEAR, MIX, SPREAD, TIME and FEEDBACK.
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { yankiIds::mood, 1 }, "Mood", moodNames, 0,
                                                   AudioParameterChoiceAttributes().withMeta (true)));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { yankiIds::timeSync, 2 }, "Time Sync", syncNames, 1));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { yankiIds::timeMs, 1 }, "Time (Free)", NormalisableRange<float> (50.0f, 700.0f),
                                                  (float) echo.timeMs, withText ([] (float v) { return String (v, 1) + " ms"; })));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { yankiIds::feedback, 1 }, "Feedback",
                                                  NormalisableRange<float> (0.0f, 1.0f, fbFrom0to1, fbTo0to1),
                                                  naturalFeedbackKnob (0, 1),
                                                  withText ([] (float k)
                                                  {
                                                      if (k < (float) kKnobHold)
                                                          return String (roundToInt (k / (float) kKnobHold * 100.0f)) + " % of hold";
                                                      return "Swell " + String (roundToInt ((k - (float) kKnobHold) / (float) (1.0 - kKnobHold) * 100.0f)) + " %";
                                                  })));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { yankiIds::capOn, 1 }, "Cap On", false));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { yankiIds::capDb, 1 }, "Cap", NormalisableRange<float> (-12.0f, 12.0f), 0.0f, db));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { yankiIds::tone, 1 }, "Tone", NormalisableRange<float> (1000.0f, 8000.0f), (float) echo.toneHz,
                                                  withText ([] (float v) { return String (v / 1000.0f, 2) + " kHz"; })));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { yankiIds::wear, 1 }, "Wear", NormalisableRange<float> (0.0f, 1.0f), (float) echo.wear,
                                                  withText ([] (float v) { return String (v * 10.0f, 1); })));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { yankiIds::mix, 1 }, "Mix", NormalisableRange<float> (0.0f, 1.0f), (float) echo.mix, pct));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { yankiIds::output, 1 }, "Output", NormalisableRange<float> (-12.0f, 12.0f), 0.0f, db));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { yankiIds::endMode, 1 }, "End Mode", endNames, 0));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { yankiIds::endTrigger, 1 }, "End Trigger", false));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { yankiIds::freeze, 1 }, "Freeze", false));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { yankiIds::bypass, 1 }, "Bypass", false));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { yankiIds::darken, 1 }, "Darken", NormalisableRange<float> (0.0f, 1.0f), 0.0f,
                                                  withText ([] (float v) { return String (v * 10.0f, 1); })));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { yankiIds::riseCurve, 1 }, "Rise Curve", riseNames, 1));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { yankiIds::lockBars, 1 }, "Tempo Lock", lockNames, 0));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { yankiIds::fineMs, 1 }, "Fine", NormalisableRange<float> (-10.0f, 10.0f), 0.0f,
                                                  withText ([] (float v) { return (v > 0.0f ? "+" : "") + String (v, 1) + " ms"; })));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { yankiIds::spread, 1 }, "Spread", NormalisableRange<float> (0.0f, 1.0f), (float) echo.width, pct));
    // Appended last (parameter order of saved projects stays): 0 = the MOOD's own heads, so a project saved before it opens as it was.
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { yankiIds::heads, 1 }, "Heads", headsNames, 0));
    return l;
}

float YankiProcessor::naturalFeedbackKnob (int mood, int riseCurve)
{
    const auto c = yanki::moodControls (juce::jlimit (0, 6, mood));
    const double hold = yanki::YankiEngine::holdFeedback (c);
    // The loop gain the mood runs, relative to the hold point (the stage and
    // clean loops cap their gain at 0.90).
    double fb = c.feedback;
    if (yanki::YankiEngine::stageVoice (c))
        fb = std::min (fb, yanki::YankiEngine::kStageMaxFeedback);
    else
        fb = std::min (fb, yanki::YankiEngine::kMaxEffectiveFeedback);
    const double q = fb / std::max (hold, 1.0e-9);
    if (q <= 1.0)
        return (float) (kKnobHold * q);
    // Past HOLD: RISE = log2 (q) / 2 (the engine's 2^(2 RISE) x hold), drawn on the current RISE CURVE.
    const double rise = 0.5 * std::log2 (q);
    const double top = kRiseMax[juce::jlimit (0, 2, riseCurve)];
    return (float) (kKnobHold + (1.0 - kKnobHold) * juce::jlimit (0.0, 1.0, rise / top));
}

YankiProcessor::YankiProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "Yanki", createLayout())
{
    const char* ids[20] { yankiIds::mood, yankiIds::timeSync, yankiIds::timeMs, yankiIds::feedback, yankiIds::capOn, yankiIds::capDb,
                          yankiIds::tone, yankiIds::wear, yankiIds::mix, yankiIds::output, yankiIds::endMode, yankiIds::endTrigger,
                          yankiIds::freeze, yankiIds::bypass, yankiIds::darken, yankiIds::riseCurve, yankiIds::lockBars,
                          yankiIds::fineMs, yankiIds::spread, yankiIds::heads };
    for (int i = 0; i < 20; ++i)
        p[i] = apvts.getRawParameterValue (ids[i]);
    bypassParam = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter (yankiIds::bypass));

    naturalKnob = v (pFeedback);
    feedbackNatural = true;
    undo.ignore (yankiIds::endTrigger);   // a momentary pulse, not an edit
    apvts.addParameterListener (yankiIds::mood, this);
    apvts.addParameterListener (yankiIds::endTrigger, this);
    setLatencySamples (engine.latency());   // 0
}

YankiProcessor::~YankiProcessor()
{
    cancelPendingUpdate();
    apvts.removeParameterListener (yankiIds::mood, this);
    apvts.removeParameterListener (yankiIds::endTrigger, this);
}

juce::AudioProcessorParameter* YankiProcessor::getBypassParameter() const { return bypassParam; }

bool YankiProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    const auto in = layouts.getMainInputChannelSet();
    if (out != juce::AudioChannelSet::stereo() && out != juce::AudioChannelSet::mono())
        return false;
    return in == out || (in == juce::AudioChannelSet::mono() && out == juce::AudioChannelSet::stereo());
}

//==============================================================================
// MOOD recall: a mood sets its heads/drive/voicing (read live from MOOD) and
// writes its TONE, WEAR, MIX, SPREAD, free TIME and FEEDBACK position to the
// parameters. DARKEN, RISE CURVE, TEMPO LOCK and FINE stay where they are.
void YankiProcessor::parameterChanged (const juce::String& id, float value)
{
    if (id == yankiIds::endTrigger)
    {
        if (value > 0.5f)
            endPulsePending = true;
        return;
    }
    if (restoring.load())
        return;
    if (juce::MessageManager::existsAndIsCurrentThread())
        applyMoodIfChanged();
    else
        triggerAsyncUpdate();
}

void YankiProcessor::handleAsyncUpdate() { applyMoodIfChanged(); }

void YankiProcessor::applyMoodIfChanged()
{
    if (restoring.load())
        return;
    const int m = juce::roundToInt (v (pMood));
    if (m == appliedMood.load())
        return;
    appliedMood = m;
    svl::ParamUndo::Derived derived (undo);   // host automation of MOOD must not leave steps; a MOOD click from the editor keeps its own
    recallMood (m);
}

void YankiProcessor::recallMood (int mood)
{
    const auto c = yanki::moodControls (mood);
    auto set = [this] (const char* id, float value)
    {
        if (auto* param = apvts.getParameter (id))
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost (param->convertTo0to1 (value));
            param->endChangeGesture();
        }
    };
    set (yankiIds::timeMs, (float) c.timeMs);
    set (yankiIds::tone, (float) c.toneHz);
    set (yankiIds::wear, (float) c.wear);
    set (yankiIds::mix, (float) c.mix);
    set (yankiIds::spread, (float) c.width);
    set (yankiIds::heads, (float) kHeadsMood);
    set (yankiIds::feedback, naturalFeedbackKnob (mood, juce::roundToInt (v (pRise))));
    naturalKnob = v (pFeedback);
    feedbackNatural = true;
}


//==============================================================================
// The ten starting points. tools/presets_v2.json holds what the fit found; STATUS.md the measured table.
const YankiProcessor::Preset YankiProcessor::kPresets[YankiProcessor::kNumPresets] {
    //  name                      group     mood sync timeMs  fb      mix     tone   wear  spread cap   capDb lock out
    { "Slap 80 ms",              "SLAP",    0, 6,  80.0f, 0.4643f, 0.2531f, 5200.0f, 0.25f, 0.30f, false, 0.0f, 0, 0.0f },
    { "Sixteenth tight",         "TIGHT",   0, 0,  -1.0f, 0.5101f, 0.2348f, 4500.0f, 0.25f, 0.30f, false, 0.0f, 0, 0.0f },
    { "Eighth clean",            "CLASSIC", 0, 1,  -1.0f, 0.5075f, 0.2197f, 4200.0f, 0.25f, 0.30f, false, 0.0f, 0, 0.0f },
    { "Dotted eighth lift",      "CLASSIC", 0, 2,  -1.0f, 0.4093f, 0.3118f, 3800.0f, 0.25f, 0.40f, false, 0.0f, 0, 0.0f },
    { "Quarter open",            "SPACE",   0, 3,  -1.0f, 0.5088f, 0.2219f, 3600.0f, 0.25f, 0.40f, false, 0.0f, 0, 0.0f },
    { "Dotted eighth behind",    "SPACE",   0, 2,  -1.0f, 0.4433f, 0.1593f, 3000.0f, 0.30f, 0.40f, false, 0.0f, 0, 0.0f },
    { "Half note long",          "SPACE",   0, 5,  -1.0f, 0.3687f, 0.1587f, 3200.0f, 0.30f, 0.40f, false, 0.0f, 0, 0.0f },
    { "Dotted eighth wide",      "WIDE",    2, 2,  -1.0f, 0.1947f, 0.2100f, 3800.0f, 0.30f, 0.90f, false, 0.0f, 0, 0.0f },
    { "Swell build",             "SWELL",   3, 2,  -1.0f, 0.8500f, 0.4800f, 2600.0f, 0.80f, 0.65f, true,  3.0f, 0, -2.0f },
    { "Swell four-bar arrival",  "SWELL",   3, 2,  -1.0f, 0.8500f, 0.4800f, 2600.0f, 0.80f, 0.65f, true,  0.0f, 3, -2.0f },
};

void YankiProcessor::applyPreset (int index)
{
    if (index < 0 || index >= kNumPresets)
        return;
    const auto& q = kPresets[index];
    svl::ParamUndo::Group oneStep (undo, juce::String ("Preset ") + q.name);   // the whole preset = one undo step
    auto set = [this] (const char* id, float plain)
    {
        if (auto* param = apvts.getParameter (id))
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost (param->convertTo0to1 (plain));
            param->endChangeGesture();
        }
    };
    set (yankiIds::mood, (float) q.mood);               // the voicing; its recall writes TONE, WEAR, MIX, SPREAD, TIME, FEEDBACK ...
    if (juce::MessageManager::existsAndIsCurrentThread())
        applyMoodIfChanged();
    set (yankiIds::timeSync, (float) q.sync);
    if (q.timeMs >= 0.0f)
        set (yankiIds::timeMs, q.timeMs);
    set (yankiIds::fineMs, 0.0f);
    set (yankiIds::feedback, q.feedback);
    set (yankiIds::tone, q.tone);
    set (yankiIds::wear, q.wear);
    set (yankiIds::mix, q.mix);
    set (yankiIds::spread, q.spread);
    set (yankiIds::capOn, q.capOn ? 1.0f : 0.0f);
    set (yankiIds::capDb, q.capDb);
    set (yankiIds::lockBars, (float) q.lockBars);
    set (yankiIds::output, q.outputDb);
    set (yankiIds::darken, 0.0f);
    set (yankiIds::riseCurve, 1.0f);
    set (yankiIds::endMode, 0.0f);
    set (yankiIds::freeze, 0.0f);
    set (yankiIds::heads, (float) kHeadsMood);
    presetIndex = index;
}

bool YankiProcessor::presetEdited() const
{
    const int i = presetIndex.load();
    if (i < 0 || i >= kNumPresets)
        return false;
    const auto& q = kPresets[i];
    auto differs = [] (float a, float b, float tol) { return std::abs (a - b) > tol; };
    if (juce::roundToInt (v (pMood)) != q.mood || juce::roundToInt (v (pSync)) != q.sync || juce::roundToInt (v (pHeads)) != kHeadsMood)
        return true;
    if (q.timeMs >= 0.0f && differs (v (pTime), q.timeMs, 0.6f))
        return true;
    return differs (v (pFeedback), q.feedback, 0.004f) || differs (v (pMix), q.mix, 0.004f) || differs (v (pTone), q.tone, 15.0f)
        || differs (v (pWear), q.wear, 0.01f) || differs (v (pSpread), q.spread, 0.01f) || (v (pCapOn) > 0.5f) != q.capOn
        || (q.capOn && differs (v (pCapDb), q.capDb, 0.15f)) || juce::roundToInt (v (pLock)) != q.lockBars || differs (v (pOutput), q.outputDb, 0.15f);
}

//==============================================================================
double YankiProcessor::resolvedMs (double bpm) const
{
    const int idx = juce::jlimit (0, kSyncFree, juce::roundToInt (v (pSync)));
    const auto base = yanki::moodControls (juce::jlimit (0, 6, juce::roundToInt (v (pMood))));
    if (idx == kSyncFree)
        return snapTo (v (pTime), base.timeMs, 650.0) + (double) v (pFine);
    return 60000.0 / std::max (20.0, bpm) * kSyncQuarters[idx];
}

yanki::Controls YankiProcessor::controlsFor (int mood, double bpm, double ms, bool endTrig) const
{
    const auto base = yanki::moodControls (mood);
    yanki::Controls c = base;
    c.bpm = bpm;
    // TIME is resolved here (sync division at the host tempo, or free ms +
    // FINE) and handed over as free ms, so it can glide; syncMs() of a synced
    // Controls returns exactly this value.
    c.sync = 0;
    c.timeMs = ms;
    c.toneHz = snapTo (toneSmooth.getCurrentValue(), base.toneHz, 7000.0);
    c.wear = snapTo (wearSmooth.getCurrentValue(), base.wear, 1.0);
    c.mix = snapTo (mixSmooth.getCurrentValue(), base.mix, 1.0);
    const int headsParam = juce::jlimit (0, 3, juce::roundToInt (v (pHeads)));
    c.mode = engineMode (base.mode, headsParam);
    c.width = engineWidth (mood, effectiveHeads (mood, headsParam), snapTo (spreadSmooth.getCurrentValue(), base.width, 1.0));
    if (! feedbackNatural.load())
    {
        const double k = fbSmooth.getCurrentValue();
        const double top = kRiseMax[juce::jlimit (0, 2, juce::roundToInt (v (pRise)))];
        yanki::YankiEngine::feedbackKnob (c, k < kKnobHold ? k : kKnobHold + (k - kKnobHold) * top);
    }
    c.capOn = v (pCapOn) > 0.5f;
    c.capDb = capSmooth.getCurrentValue();
    c.darken = darkenSmooth.getCurrentValue();
    c.endMode = juce::jlimit (0, 2, juce::roundToInt (v (pEndMode)));
    c.endTrigger = endTrig;
    c.lockBars = kLockBars[juce::jlimit (0, 3, juce::roundToInt (v (pLock)))];
    c.freeze = v (pFreeze) > 0.5f;
    return c;
}

void YankiProcessor::prepareToPlay (double sr, int block)
{
    sampleRate = sr;
    engine.prepare (sr);
    dry.setSize (2, block);
    wet.setSize (2, block);

    msSmooth.reset (sr, 0.25);
    for (auto* s : { &toneSmooth, &wearSmooth, &mixSmooth, &spreadSmooth, &darkenSmooth, &capSmooth, &fbSmooth })
        s->reset (sr, 0.05);
    toneSmooth.setCurrentAndTargetValue (v (pTone));
    wearSmooth.setCurrentAndTargetValue (v (pWear));
    mixSmooth.setCurrentAndTargetValue (mixToEngine (v (pMix)));
    spreadSmooth.setCurrentAndTargetValue (v (pSpread));
    darkenSmooth.setCurrentAndTargetValue (v (pDarken));
    capSmooth.setCurrentAndTargetValue (v (pCapDb));
    fbSmooth.setCurrentAndTargetValue (v (pFeedback));
    outGain.reset (sr, 0.05);
    outGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (v (pOutput)));
    effectAmount.reset (sr, 0.02);
    effectAmount.setCurrentAndTargetValue (v (pBypass) > 0.5f ? 0.0f : 1.0f);
    firstBlock = true;

    onEnv = 0.0;
    onRelease = std::exp (-1.0 / (0.05 * sr));
    gapSamples = (uint64_t) sr;
    onQuiet = gapSamples;
    beatPhase = 0.0;
    uiEndLatched = false;
    setLatencySamples (engine.latency());
}

void YankiProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    if (n <= 0)
        return;
    if (n > dry.getNumSamples())
    {
        dry.setSize (2, n, false, false, true);
        wet.setSize (2, n, false, false, true);
    }

    if (resetRequested.exchange (false))
        engine.reset();

    // Non-finite input (a NaN / inf from the host or the device before us) is replaced by silence.
    for (int c = 0; c < buffer.getNumChannels(); ++c)
    {
        float* d = buffer.getWritePointer (c);
        for (int i = 0; i < n; ++i)
            if (nonFinite (d[i]))
            {
                d[i] = 0.0f;
                ++nonFiniteEvents;
            }
    }

    // Host tempo, read every block; 120 bpm only when the host gives none.
    double bpm = kFallbackBpm;
    bool hostTempo = false, playing = false;
    juce::Optional<double> ppq;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            if (auto b = pos->getBpm(); b && std::isfinite (*b) && *b > 0.0)
            {
                bpm = juce::jlimit (20.0, 400.0, *b);
                hostTempo = true;
            }
            playing = pos->getIsPlaying();
            ppq = pos->getPpqPosition();
        }
    uiBpm = bpm;
    uiHostTempo = hostTempo;

    // BEAT lamp: one count per host quarter note (free-running at the tempo without a transport).
    const double beats = (double) n / sampleRate * bpm / 60.0;
    if (playing && ppq.hasValue())
    {
        if (std::ceil (*ppq) < *ppq + beats)
            ++uiBeatCount;
        beatPhase = *ppq - std::floor (*ppq);
    }
    else
    {
        beatPhase += beats;
        if (beatPhase >= 1.0)
        {
            beatPhase -= std::floor (beatPhase);
            ++uiBeatCount;
        }
    }

    const int inCh = juce::jmax (1, getTotalNumInputChannels());
    const int outCh = getTotalNumOutputChannels();
    for (int c = 0; c < 2; ++c)
        dry.copyFrom (c, 0, buffer, juce::jmin (c, inCh - 1), 0, n);

#if SVL_LICENSING
    // No licence, no processing: the audio passes through untouched.
    if (! license.isLicensed())
    {
        if (inCh == 1 && buffer.getNumChannels() > 1)
            buffer.copyFrom (1, 0, buffer, 0, 0, n);
        return;
    }
#endif

    const int mood = juce::jlimit (0, 6, juce::roundToInt (v (pMood)));

    // FEEDBACK leaves the mood's own loop gain the moment it is moved.
    if (feedbackNatural.load() && std::abs (v (pFeedback) - naturalKnob.load()) > 1.0e-5f)
        feedbackNatural = false;
    uiFeedbackNatural = feedbackNatural.load();

    // END: CUT / DUMP fire on the press (a rising edge, even if the key was
    // released before this block); HOLD clears the lamp.
    const bool pulse = endPulsePending.exchange (false);
    const bool endTrig = pulse || v (pEndTrigger) > 0.5f;
    const int endMode = juce::roundToInt (v (pEndMode));
    if (pulse && endMode != yanki::kEndHold)
        uiEndLatched = true;
    if (endMode == yanki::kEndHold)
        uiEndLatched = false;

    // Targets.
    const double msTarget = resolvedMs (bpm);
    uiMs = msTarget;
    if (firstBlock)
    {
        msSmooth.setCurrentAndTargetValue (msTarget);
        firstBlock = false;
    }
    else
        msSmooth.setTargetValue (msTarget);
    toneSmooth.setTargetValue (v (pTone));
    wearSmooth.setTargetValue (v (pWear));
    mixSmooth.setTargetValue (mixToEngine (v (pMix)));
    spreadSmooth.setTargetValue (v (pSpread));
    darkenSmooth.setTargetValue (v (pDarken));
    capSmooth.setTargetValue (v (pCapDb));
    fbSmooth.setTargetValue (v (pFeedback));
    outGain.setTargetValue (juce::Decibels::decibelsToGain (v (pOutput)));
    effectAmount.setTargetValue (v (pBypass) > 0.5f ? 0.0f : 1.0f);

    // The engine runs on a copy, always (also while bypassed, so the tape
    // stays current). While anything glides it runs in short steps with the
    // controls updated between them; otherwise in one call.
    for (int c = 0; c < 2; ++c)
        wet.copyFrom (c, 0, dry, c, 0, n);
    double dryPow = 0.0, loopPow = 0.0;
    bool swelling = false;
    int pos = 0;
    while (pos < n)
    {
        const bool gliding = msSmooth.isSmoothing() || toneSmooth.isSmoothing() || wearSmooth.isSmoothing() || mixSmooth.isSmoothing()
                          || spreadSmooth.isSmoothing() || darkenSmooth.isSmoothing() || capSmooth.isSmoothing() || fbSmooth.isSmoothing();
        // A TIME glide moves the heads every sample (a tape-speed change, as
        // the engine reads one delay per call); other glides every 32 samples.
        const int len = msSmooth.isSmoothing() ? 1 : gliding ? juce::jmin (32, n - pos) : n - pos;
        const auto ctl = controlsFor (mood, bpm, msSmooth.getCurrentValue(), endTrig);
        engine.process (wet.getWritePointer (0) + pos, wet.getWritePointer (1) + pos, len, ctl);
        dryPow += std::pow (10.0, engine.dryLevelDb() / 10.0) * len;
        loopPow += std::pow (10.0, engine.loopLevelDb() / 10.0) * len;
        swelling = swelling || engine.swelling();
        msSmooth.skip (len);
        for (auto* s : { &toneSmooth, &wearSmooth, &mixSmooth, &spreadSmooth, &darkenSmooth, &capSmooth, &fbSmooth })
            s->skip (len);
        pos += len;
    }
    // Safety net: if the engine ever produces a non-finite sample, drop its state and pass the dry signal this block.
    {
        bool bad = false;
        for (int c = 0; c < 2 && ! bad; ++c)
        {
            const float* w = wet.getReadPointer (c);
            for (int i = 0; i < n; ++i)
                if (nonFinite (w[i])) { bad = true; break; }
        }
        if (bad)
        {
            engine.reset();
            for (int c = 0; c < 2; ++c)
                wet.copyFrom (c, 0, dry, c, 0, n);
            dryPow = loopPow = 0.0;
            swelling = false;
            ++nonFiniteEvents;
        }
    }
    uiDryDb = (float) (10.0 * std::log10 (std::max (dryPow / n, 1.0e-24)));
    uiLoopDb = (float) (10.0 * std::log10 (std::max (loopPow / n, 1.0e-24)));
    uiRefDb = (float) engine.refRmsDb();
    uiSwelling = swelling;
    uiFrozen = v (pFreeze) > 0.5f;

    // END lamp re-arm: a new phrase (onset after >= 1 s under -50 dBFS), as the engine does it.
    {
        const float* l = dry.getReadPointer (0);
        const float* r = dry.getReadPointer (1);
        for (int i = 0; i < n; ++i)
        {
            const double a = std::max (std::abs ((double) l[i]), std::abs ((double) r[i]));
            onEnv = a > onEnv ? a : onEnv * onRelease;
            if (onEnv < 0.00316) { if (onQuiet < gapSamples) ++onQuiet; }
            else if (onEnv > 0.01)
            {
                if (onQuiet >= gapSamples && ! (pulse && i == 0))
                    uiEndLatched = false;
                onQuiet = 0;
            }
        }
    }

    // OUTPUT gain and the BYPASS crossfade. Fully bypassed = the input, bit-exact.
    const bool steadyBypass = ! effectAmount.isSmoothing() && effectAmount.getTargetValue() <= 0.0f;
    if (steadyBypass)
    {
        outGain.skip (n);
        if (outCh == 1)
            buffer.copyFrom (0, 0, dry, 0, 0, n);
        else
            for (int c = 0; c < outCh; ++c)
                buffer.copyFrom (c, 0, dry, juce::jmin (c, 1), 0, n);
        return;
    }
    for (int i = 0; i < n; ++i)
    {
        const float g = outGain.getNextValue();
        const float e = effectAmount.getNextValue();
        const float dl = dry.getSample (0, i), dr = dry.getSample (1, i);
        if (outCh == 1)
        {
            const float w = 0.5f * (wet.getSample (0, i) + wet.getSample (1, i)) * g;
            buffer.setSample (0, i, e < 1.0f ? dl + (w - dl) * e : w);
            continue;
        }
        float wl = wet.getSample (0, i) * g, wr = wet.getSample (1, i) * g;
        if (e < 1.0f)
        {
            wl = dl + (wl - dl) * e;
            wr = dr + (wr - dr) * e;
        }
        buffer.setSample (0, i, wl);
        buffer.setSample (1, i, wr);
    }
}

juce::AudioProcessorEditor* YankiProcessor::createEditor() { return new YankiEditor (*this); }

void YankiProcessor::getStateInformation (juce::MemoryBlock& data)
{
    auto state = apvts.copyState();
    state.setProperty ("feedbackNatural", feedbackNatural.load(), nullptr);
    state.setProperty ("naturalKnob", naturalKnob.load(), nullptr);
    state.setProperty ("stateVersion", kStateVersion, nullptr);
    state.setProperty ("presetIndex", presetIndex.load(), nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, data);
}

void YankiProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto tree = juce::ValueTree::fromXml (*xml);
            // Projects saved before v2 have six sync entries (1/2 = 4, FREE = 5); the list now has 1/4 dotted at 4.
            if ((int) tree.getProperty ("stateVersion", 1) < 2)
                for (auto child : tree)
                    if (child.getProperty ("id").toString() == yankiIds::timeSync)
                    {
                        const int old = juce::roundToInt ((float) (double) child.getProperty ("value"));
                        child.setProperty ("value", old >= 4 ? old + 1 : old, nullptr);
                    }
            restoring = true;
            apvts.replaceState (tree);
            undo.clear();   // the old steps describe a state that is gone
            bool hasHeads = false;
            for (auto child : tree)
                hasHeads = hasHeads || child.getProperty ("id").toString() == yankiIds::heads;
            if (! hasHeads)   // a project saved before HEADS: the MOOD's own heads
                if (auto* hp = apvts.getParameter (yankiIds::heads))
                    hp->setValueNotifyingHost (0.0f);
            appliedMood = juce::roundToInt (v (pMood));
            naturalKnob = (float) (double) tree.getProperty ("naturalKnob", (double) naturalFeedbackKnob (appliedMood.load(), 1));
            feedbackNatural = (bool) tree.getProperty ("feedbackNatural", false);
            presetIndex = (int) tree.getProperty ("presetIndex", -1);
            restoring = false;
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new YankiProcessor(); }
