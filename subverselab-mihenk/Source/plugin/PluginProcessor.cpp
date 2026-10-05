#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "StateIO.h"
#include "../engine/Preset.h"

namespace mihenk
{
namespace { constexpr int kHostChunk = 1024; }   // host blocks are handled in pieces of at most this many samples

/** Plays the standalone player at the device rate: straight through when the rates match, resampled when not. */
struct MihenkProcessor::DeviceStage : public juce::AudioSource
{
    explicit DeviceStage (Player& p) : player (p), resampler (&p, false, 2) {}

    void prepareToPlay (int block, double rate) override
    {
        deviceRate = rate;
        ratio = player.getFileRate() / rate;
        resampler.setResamplingRatio (ratio);
        resampler.prepareToPlay (block, rate);
        player.prepareToPlay (block, player.getFileRate());
    }
    void releaseResources() override { resampler.releaseResources(); }
    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info) override
    {
        const double r = player.getFileRate() / deviceRate;
        if (std::abs (r - 1.0) < 1.0e-9)
        {
            player.getNextAudioBlock (info);
            return;
        }
        if (std::abs (r - ratio) > 1.0e-9)
        {
            ratio = r;
            resampler.setResamplingRatio (r);
        }
        resampler.getNextAudioBlock (info);
    }

    Player& player;
    juce::ResamplingAudioSource resampler;
    double deviceRate = 48000.0, ratio = 1.0;
};

//==============================================================================
MihenkProcessor::MihenkProcessor()
    : juce::AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                             .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", params::createLayout())
{
    standalone = juce::PluginHostType::getPluginLoadedAs() == wrapperType_Standalone;
    bridge = std::make_unique<params::Bridge> (apvts);
    for (const auto& d : params::definitions()) apvts.addParameterListener (d.id, this);
    session.keepStageTaps = true;
    session.profileSource = standalone ? "app" : "plugin";
    session.instantBypass = standalone;
    session.keepProxy = ! standalone;
    session.undoHostOwnsParams = ! standalone;        // in a host the host's own Cmd+Z undoes the parameters; the editor's history keeps the rest
    session.resolveWithoutAudio = [this] { return resolveWithoutAudio(); };
    session.player.setMeterFifo (&meters);
    session.onSettingsEdited = [this] { onSettingsEdited(); };
    session.onRendered = [this] (const RenderResult& r, const MasterSettings& s) { onRendered (r, s); };
    stage = std::make_unique<DeviceStage> (session.player);
    rt.setMeterFifo (&meters);
    // The settings are the plan of what is known (nothing yet); the parameters show them.
    bridge->write (session.settings, false);
    bridge->read (session.settings);
    session.shownFromSettings();
    publishBase();
    startTimerHz (30);
}

MihenkProcessor::~MihenkProcessor()
{
    stopTimer();
    for (const auto& d : params::definitions()) apvts.removeParameterListener (d.id, this);
    session.onSettingsEdited = nullptr;
    session.onRendered = nullptr;
    session.player.setMeterFifo (nullptr);
}

bool MihenkProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet(), out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::stereo() && out != juce::AudioChannelSet::mono()) return false;
    return in == out || in.isDisabled();
}

//==============================================================================
void MihenkProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    if (standalone)
    {
        stage->prepareToPlay (samplesPerBlock, sampleRate);
        setLatencySamples (0);
        return;
    }
    const int chans = juce::jmax (1, juce::jmin (2, getTotalNumOutputChannels()));
    rt.prepare (sampleRate, chans, juce::jmax (samplesPerBlock, 32));
    MasterSettings s;
    {
        const juce::SpinLock::ScopedLockType sl (baseLock);
        s = base;
        rtPlan = plan;
    }
    bridge->read (s);
    s.eq.matchCurveDb = session.settings.eq.matchCurveDb;
    rt.setMatchCurve (s.eq.matchCurveDb);
    lastCurve = s.eq.matchCurveDb;
    rt.prime (s);
    rt.setPlan (rtPlan);
    rtVersion = -1;
    hostDry.prepare (rt.maxLatencySamples() + 2 * kHostChunk + 16);
    meters.sampleRate.store (sampleRate);
    meters.clear();
    setLatencySamples (rt.latencySamples (s));
}

void MihenkProcessor::releaseResources()
{
    if (standalone) stage->releaseResources();
}

void MihenkProcessor::renderStandalone (juce::AudioBuffer<float>& buffer)
{
    buffer.clear();
    juce::AudioSourceChannelInfo info (&buffer, 0, buffer.getNumSamples());
    stage->getNextAudioBlock (info);
}

void MihenkProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    if (standalone)
    {
        renderStandalone (buffer);
        return;
    }
    const int n = buffer.getNumSamples();
    const int chans = juce::jmin (buffer.getNumChannels(), juce::jmax (1, getTotalNumOutputChannels()));
    for (int c = getTotalNumInputChannels(); c < buffer.getNumChannels(); ++c) buffer.clear (c, 0, n);

    bool playing = false;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
            playing = pos->getIsPlaying();
    hostPlaying.store (playing);

    // LEARN: capture the input while the host plays.
    auto st = learnState();
    if (st == Learn::armed && playing) { learn.store ((int) Learn::capturing); st = Learn::capturing; }
    if (st == Learn::capturing)
    {
        if (playing) capture.write (buffer.getArrayOfReadPointers(), chans, n, isNonRealtime());
        else if (capture.samples() > 0) stopRequested.store (true);
        if (capture.isFull()) stopRequested.store (true);
    }

    // Settings: the published non-parameter part and the plan, then the parameters (automation).
    {
        const juce::SpinLock::ScopedTryLockType sl (baseLock);
        if (sl.isLocked() && rtVersion != baseVersion)
        {
            rtBase = base;
            rtPlan = plan;
            rtVersion = baseVersion;
            rt.setPlan (rtPlan);
        }
    }
    rtSettings = rtBase;
    bridge->read (rtSettings);
    rt.setSynchronousKernels (isNonRealtime());
    rt.setListenInput (listenOriginal.load(), effectiveListenGain());

    float peak = 0.0f;
    for (int c = 0; c < chans; ++c) peak = juce::jmax (peak, buffer.getMagnitude (c, 0, n));
    {
        // The chain always runs on the input. In the host's bypass its output is replaced, through a short crossfade, by
        // the input delayed by the reported latency, so bypass neither jumps in time nor leaves old audio behind.
        const float target = hostBypassNow ? 1.0f : 0.0f;
        const float step = 1.0f / (float) juce::jmax (1.0, 0.02 * getSampleRate());
        const int ringSize = hostDry.mask + 1;
        const int delay = juce::jlimit (0, juce::jmax (0, ringSize - 2 * kHostChunk - 8), getLatencySamples());
        float* ch[2] { nullptr, nullptr };
        for (int pos = 0; pos < n; pos += kHostChunk)
        {
            const int len = juce::jmin (kHostChunk, n - pos);
            for (int c = 0; c < chans; ++c) ch[c] = buffer.getWritePointer (c) + pos;
            const bool dryNeeded = hostBypassNow || hostDry.mix > 0.0f;
            if (ringSize > 1)
                for (int c = 0; c < chans; ++c)
                    for (int i = 0; i < len; ++i) hostDry.ring[c][(size_t) ((hostDry.write + i) & hostDry.mask)] = ch[c][i];
            rt.process (ch, chans, len, rtSettings);
            if (ringSize > 1 && dryNeeded)
                for (int i = 0; i < len; ++i)
                {
                    if (hostDry.mix < target) hostDry.mix = juce::jmin (target, hostDry.mix + step);
                    else if (hostDry.mix > target) hostDry.mix = juce::jmax (target, hostDry.mix - step);
                    const float m = hostDry.mix;
                    if (m <= 0.0f) continue;
                    const size_t at = (size_t) ((hostDry.write + i - delay) & hostDry.mask);
                    for (int c = 0; c < chans; ++c) ch[c][i] += (hostDry.ring[c][at] - ch[c][i]) * m;
                }
            if (ringSize > 1) hostDry.write = (hostDry.write + len) & hostDry.mask;
        }
    }
    meters.sampleRate.store (getSampleRate());
    meters.monoSource.store (chans == 1);
    meters.running.store (playing || peak > 1.0e-5f);
}

void MihenkProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    if (standalone)
    {
        processBlock (buffer, midi);
        return;
    }
    hostBypassNow = true;
    processBlock (buffer, midi);
    hostBypassNow = false;
}

//==============================================================================
void MihenkProcessor::parameterChanged (const juce::String& id, float value)
{
    if (bridge != nullptr && bridge->isEcho (id, value)) return;
    paramsDirty.store (true);
}

void MihenkProcessor::applyParamsToSession()
{
    // The host moved a parameter (automation, a generic editor, a preset of the host): the session reads it as an edit of the
    // intent (the target) or as an override over the plan, rebuilds the settings from the plan and renders again.
    const Session::HostEdit hostEdit (session);        // the host's move (automation, its own undo) is not a step of the editor's history
    applyingParams = true;
    bridge->read (session.settings);
    // The host moved a locked knob (automation, a generic editor): that is an override, so the lock lets go.
    for (int p = 0; p < profile::kNumParams; ++p)
        if (session.isLocked (p) && std::abs (profile::get (session.settings, p) - session.locks[(size_t) p].value) > 1.0e-3)
            session.unlock (p);
    session.settingsChanged();
    applyingParams = false;
    publishBase();
}

void MihenkProcessor::onSettingsEdited()
{
    // The panel changed the settings: the parameters take them (the host records the change), and the session then
    // renders exactly what the parameters hold, so the offline render and the real-time chain use the same values.
    // From the host (applyingParams) only what the session derived from the host's move is written back, without a gesture:
    // the target preset sets targetLufs, a skipped stage switches its parameter off. Reading the parameters first would put
    // the stale derived value back over it (CLUB -8 from the host kept the -14 loudness).
    bridge->write (session.settings, ! applyingParams && ! dragGesture);
    bridge->read (session.settings);
    publishBase();
}

void MihenkProcessor::onRendered (const RenderResult& r, const MasterSettings& solved)
{
    // The engine's solved values (drive, AUTO thresholds) go to the parameters, so the host and the real-time chain see them.
    applyingParams = true;
    {
        auto w = solved;
        w.bypass = session.settings.bypass;       // BYPASS is the user's switch, never something a render writes back
        bridge->write (w, false);
    }
    applyingParams = false;
    if (! standalone && r.output != nullptr)
    {
        plan = makePlan (solved, r, session.sampleRate);
        planValid = plan.valid;
        planFingerprint = settingsFingerprint (params::quantised (solved));
        if (r.proxy != nullptr) { proxy = *r.proxy; proxyBase = solved; }
        // A = the input, level-matched to what the chain outputs.
        const double d = (std::isfinite (r.measured.integrated) && std::isfinite (session.inMeas.integrated))
                             ? session.inMeas.integrated - r.measured.integrated : 0.0;
        listenGain.store ((float) dbToGain (-d));
    }
    publishBase();
    if (learnState() == Learn::analysing) learn.store ((int) Learn::idle);
}

void MihenkProcessor::publishBase()
{
    const auto& cur = session.settings.eq.matchCurveDb;
    if (cur != lastCurve)
    {
        lastCurve = cur;
        rt.setMatchCurve (cur);
    }
    MasterSettings b = session.settings;
    b.eq.matchCurveDb.clear();
    b.eq.matchCurveDb.shrink_to_fit();
    const juce::SpinLock::ScopedLockType sl (baseLock);
    base = b;
    ++baseVersion;
}

void MihenkProcessor::timerCallback()
{
    if (paramsDirty.exchange (false)) applyParamsToSession();
    if (learnState() == Learn::capturing || learnState() == Learn::armed) capture.ensureAhead (4);
    if (stopRequested.exchange (false) && learnState() == Learn::capturing) finishLearn (false);
    if (! standalone)
    {
        const int l = rt.currentLatency();
        if (l > 0 && l != getLatencySamples()) setLatencySamples (l);
    }
}

bool MihenkProcessor::resolveWithoutAudio()
{
    // A reopened project has no captured audio, so no render. It needs none: the settings are the plan of the saved
    // measurements and the saved intent, and the session has already rebuilt them. The learned values for the AUTO
    // thresholds stay as saved until the next LEARN.
    return ! standalone;
}

void MihenkProcessor::resetAll()
{
    if (learnState() != Learn::idle)
    {
        learn.store ((int) Learn::idle);
        stopRequested.store (false);
    }
    capture.freeAll();
    session.resetAll (standalone);                 // settings (parameters through onSettingsEdited), source, analysis, reference, locks
    plan = RtPlan();
    planValid = false;
    planFingerprint.clear();
    proxy = LoudnessProxy();
    proxyBase = MasterSettings();
    listenGain.store (1.0f);
    publishBase();                                  // the empty plan and the empty tone-match curve reach the audio thread
    session.sendChangeMessage();
}

//==============================================================================
void MihenkProcessor::toggleLearn()
{
    switch (learnState())
    {
        case Learn::idle:
            capture.start (getSampleRate() > 0.0 ? getSampleRate() : 48000.0, juce::jmax (1, juce::jmin (2, getTotalNumInputChannels())));
            learn.store ((int) Learn::armed);
            session.status = "LEARN armed: play the song from the start; MIHENK listens until you stop.";
            session.sendChangeMessage();
            break;
        case Learn::armed:
            learn.store ((int) Learn::idle);
            capture.freeAll();
            session.status = "LEARN cancelled.";
            session.sendChangeMessage();
            break;
        case Learn::capturing:
            finishLearn (false);
            break;
        case Learn::analysing:
            break;
    }
}

void MihenkProcessor::finishLearn (bool synchronous)
{
    const double secs = capture.seconds();
    if (secs < 1.0)
    {
        learn.store ((int) Learn::idle);
        capture.freeAll();
        session.status = "LEARN heard less than a second; nothing learned.";
        session.sendChangeMessage();
        return;
    }
    learn.store ((int) Learn::analysing);
    // A very short LEARN may be a quiet part of the song only: the plan then solves conservatively and says so (Plan: low coverage).
    auto audio = capture.assemble();
    const double rate = capture.sampleRate();
    const bool overflow = capture.overflowed();
    capture.freeAll();
    const int m = (int) (secs / 60.0);
    const auto name = "LEARNED " + juce::String (m) + ":" + juce::String (secs - m * 60.0, 1).paddedLeft ('0', 4) + (overflow ? " (CAPTURE CUT SHORT)" : "");
    if (synchronous)
    {
        session.setInputForTesting (std::move (*audio), rate, name);
        session.renderNowForTesting();
    }
    else
        session.learnInput (std::shared_ptr<const juce::AudioBuffer<float>> (std::move (audio)), rate, name);
}

void MihenkProcessor::finishLearnNow() { finishLearn (true); }

bool MihenkProcessor::learnedIsStale() const
{
    return planValid && session.input == nullptr && settingsFingerprint (params::quantised (session.settings)) != planFingerprint;
}

void MihenkProcessor::setAbLevelMatched (bool on)
{
    abMatched.store (on);
    session.setAbLevelMatched (on);
}

void MihenkProcessor::setListenOriginal (bool a)
{
    listenOriginal.store (a);
    if (standalone) session.player.setListenOriginal (a);
}

//==============================================================================
void MihenkProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    juce::ValueTree root ("MIHENK");
    root.setProperty ("version", 2, nullptr);
    root.appendChild (apvts.copyState(), nullptr);
    root.setProperty ("settings", juce::JSON::toString (settingsToVar (session.settings), true), nullptr);
    root.setProperty ("uiScale", uiScale, nullptr);
    root.setProperty ("abMatched", abMatched.load(), nullptr);
    root.setProperty ("profile", juce::JSON::toString (session.profileStateToVar(), true), nullptr);
    root.setProperty ("intent", juce::JSON::toString (session.intentToVar(), true), nullptr);
    if (! standalone && planValid)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("plan", state::toVar (plan));
        o->setProperty ("fingerprint", planFingerprint);
        o->setProperty ("name", session.inputName);
        o->setProperty ("rate", session.sampleRate);
        o->setProperty ("inMeas", state::toVar (session.inMeas));
        o->setProperty ("outMeas", state::toVar (session.outMeas));
        o->setProperty ("diagnosis", state::toVar (session.diagnosis));
        juce::Array<juce::var> chips;
        for (const auto& c : session.scenarioChanges)
        {
            auto* x = new juce::DynamicObject();
            x->setProperty ("scenario", c.scenario);
            x->setProperty ("before", c.before);
            x->setProperty ("after", c.after);
            chips.add (juce::var (x));
        }
        o->setProperty ("scenarios", chips);
        o->setProperty ("maxDriveDb", session.settings.limiter.maxDriveDb);
        o->setProperty ("stageDelta", session.measurements.stageDeltaDb);
        {
            juce::Array<juce::var> th;
            const auto t = session.currentThresholds();
            th.add (t.comp);
            for (int b = 0; b < 3; ++b) th.add (t.band[b]);
            th.add (t.atTrim);
            o->setProperty ("thresholds", th);
        }
        {
            const auto& lr = session.measurements.limiter;
            if (lr.valid())
            {
                auto arr = [] (const std::vector<float>& v) { juce::Array<juce::var> a; for (float f : v) a.add ((double) f); return a; };
                auto* l = new juce::DynamicObject();
                l->setProperty ("x", arr (lr.x)); l->setProperty ("loss", arr (lr.loss)); l->setProperty ("avg", arr (lr.avgGr)); l->setProperty ("max", arr (lr.maxGr));
                o->setProperty ("limiterResponse", juce::var (l));
            }
        }
        if (proxy.valid)
        {
            o->setProperty ("proxy", state::toVar (proxy));
            o->setProperty ("proxyBase", settingsToVar (proxyBase));
        }
        root.setProperty ("learned", juce::JSON::toString (juce::var (o), true), nullptr);
    }
    if (auto xml = root.createXml()) copyXmlToBinary (*xml, dest);
}

void MihenkProcessor::setStateInformation (const void* data, int size)
{
    auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr || ! xml->hasTagName ("MIHENK")) return;
    auto root = juce::ValueTree::fromXml (*xml);
    MasterSettings s = session.settings;
    settingsFromVar (juce::JSON::parse (root.getProperty ("settings").toString()), s);
    const auto params = root.getChildWithName (apvts.state.getType());
    applyingParams = true;
    if (params.isValid()) apvts.replaceState (params);
    bridge->read (s);
    session.settings = s;
    applyingParams = false;
    uiScale = (float) (double) root.getProperty ("uiScale", 0.0);
    abMatched.store ((bool) root.getProperty ("abMatched", true));
    if (session.abLevelMatched() != abMatched.load()) session.setAbLevelMatched (abMatched.load());
    session.profileStateFromVar (juce::JSON::parse (root.getProperty ("profile", "").toString()));
    if (! standalone && root.hasProperty ("learned"))
    {
        const auto o = juce::JSON::parse (root.getProperty ("learned").toString());
        state::fromVar (o["plan"], plan);
        planValid = plan.valid;
        planFingerprint = o.getProperty ("fingerprint", "").toString();
        session.inputName = o.getProperty ("name", "").toString();
        session.sampleRate = o.getProperty ("rate", 0.0);
        state::fromVar (o["inMeas"], session.inMeas);
        state::fromVar (o["outMeas"], session.outMeas);
        state::fromVar (o["diagnosis"], session.diagnosis);
        state::fromVar (o["proxy"], proxy);
        proxyBase = MasterSettings();
        settingsFromVar (o["proxyBase"], proxyBase);
        session.savedThresholds = {};
        { const double sd = (double) o.getProperty ("stageDelta", 0.0); session.savedStageDelta = std::isfinite (sd) ? juce::jlimit (-6.0, 6.0, sd) : 0.0; }
        if (auto* th = o["thresholds"].getArray())
            if (th->size() == 5)
            {
                bool ok = true;
                for (auto& v : *th) ok = ok && std::isfinite ((double) v);
                if (ok)
                {
                    session.savedThresholds.valid = true;
                    session.savedThresholds.comp = juce::jlimit (-60.0, 0.0, (double) (*th)[0]);
                    for (int b = 0; b < 3; ++b) session.savedThresholds.band[b] = juce::jlimit (-60.0, 0.0, (double) (*th)[b + 1]);
                    session.savedThresholds.atTrim = juce::jlimit (-24.0, 24.0, (double) (*th)[4]);
                }
            }
        session.savedLimiter = {};
        {
            auto rd = [&] (const char* k, std::vector<float>& v)
            {
                if (auto* a = o["limiterResponse"][k].getArray())
                    for (auto& f : *a) { const double d = (double) f; v.push_back (std::isfinite (d) ? (float) juce::jlimit (-400.0, 400.0, d) : 0.0f); }
            };
            auto& lr = session.savedLimiter;
            rd ("x", lr.x); rd ("loss", lr.loss); rd ("avg", lr.avgGr); rd ("max", lr.maxGr);
            if (lr.x.size() < 2 || lr.loss.size() != lr.x.size() || lr.avgGr.size() != lr.x.size() || lr.maxGr.size() != lr.x.size()) lr = {};
            else for (size_t k = 1; k < lr.x.size(); ++k) if (! (lr.x[k] > lr.x[k - 1])) { lr = {}; break; }
        }
        session.scenarioChanges.clear();
        session.appliedScenarios.clear();
        if (auto* arr = o["scenarios"].getArray())
            for (auto& x : *arr)
            {
                Session::ScenarioChange c;
                c.scenario = x.getProperty ("scenario", 0);
                c.before = x["before"];
                c.after = x["after"];
                session.scenarioChanges.push_back (c);
                session.appliedScenarios.add (c.scenario);
            }
        const double d = (std::isfinite (session.outMeas.integrated) && std::isfinite (session.inMeas.integrated))
                             ? session.inMeas.integrated - session.outMeas.integrated : 0.0;
        listenGain.store ((float) dbToGain (-d));
    }
    if (! standalone && ! root.hasProperty ("learned"))
    {
        // No LEARN in this state: whatever this instance had learned before is not this project's.
        session.forgetSource();
        plan = RtPlan();
        planValid = false;
        planFingerprint.clear();
        proxy = LoudnessProxy();
        proxyBase = MasterSettings();
    }
    // The settings are the plan of the saved measurements and the saved intent (an older project: its target and ceiling are the
    // intent, what differs from the plan an override).
    session.restoreState (juce::JSON::parse (root.getProperty ("intent", "").toString()), s);
    applyingParams = true;
    bridge->write (session.settings, false);
    bridge->read (session.settings);
    applyingParams = false;
    session.shownFromSettings();
    paramsDirty.store (false);
    if (! standalone && root.hasProperty ("learned"))
        session.status = "Restored what LEARN found (" + session.inputName + "); press LEARN to measure again.";
    publishBase();
    session.sendChangeMessage();
}

//==============================================================================
juce::AudioProcessorEditor* MihenkProcessor::createEditor() { return new MihenkEditor (*this); }
} // namespace mihenk

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new mihenk::MihenkProcessor(); }
