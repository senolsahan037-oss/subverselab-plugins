#include "Session.h"
#include <algorithm>
#include "../engine/Entitlement.h"
#include "../engine/Preset.h"
#include "../plugin/Params.h"
#include "../dsp/Spectrum.h"

namespace mihenk
{
Session::Session() : juce::Thread ("MIHENK render")
{
    status = "Drop a WAV or AIFF on the panel, or press OPEN.";
    measurements.present = false;
    replan();
    shown = settings;
    startThread (juce::Thread::Priority::normal);
}

Session::~Session()
{
    cancelRender = true;
    signalThreadShouldExit();
    wake.signal();
    stopThread (10000);
}

//==============================================================================
void Session::enqueue (Job j)
{
    const juce::ScopedLock sl (jobLock);
    j.epoch = resetEpoch;
    const bool isRender = j.kind == Job::Kind::render || j.kind == Job::Kind::match;
    if (isRender)
    {
        // A newer render or match replaces any that has not started, and stops one that is running.
        for (auto it = jobs.begin(); it != jobs.end();)
        {
            if (it->kind == Job::Kind::render || (it->kind == Job::Kind::match && j.kind == Job::Kind::match))
            {
                it = jobs.erase (it);
                --jobsPending;
            }
            else ++it;
        }
        cancelRender = true;
        lastRequestMs = juce::Time::getMillisecondCounterHiRes();
    }
    jobs.push_back (std::move (j));
    ++jobsPending;
    wake.signal();
}

void Session::run()
{
    while (! threadShouldExit())
    {
        wake.wait (40);
        Job job;
        bool have = false;
        {
            const juce::ScopedLock sl (jobLock);
            if (! jobs.empty())
            {
                const auto& f = jobs.front();
                const bool isRender = f.kind == Job::Kind::render || f.kind == Job::Kind::match;
                // Renders (and target solves) wait until the knobs have been still for 120 ms.
                if (! isRender || juce::Time::getMillisecondCounterHiRes() - lastRequestMs >= 120.0)
                {
                    job = std::move (jobs.front());
                    jobs.pop_front();
                    have = true;
                    cancelRender = false;
                }
            }
        }
        if (! have) continue;

        juce::WeakReference<Session> weak (this);
        if (cacheClear.exchange (false)) cache = PreLimiterCache();
        const int epoch = job.epoch;
        switch (job.kind)
        {
            case Job::Kind::learn:
            {
                auto meas = std::make_shared<Measurement> (analyse (*job.in, job.rate));
                auto diag = std::make_shared<Diagnosis> (diagnose (*job.in, job.rate, job.settings));
                cache = PreLimiterCache();
                auto audio = job.in;
                const double rate = job.rate;
                const auto name = job.name;
                juce::MessageManager::callAsync ([weak, meas, diag, audio, rate, name, epoch]
                {
                    auto* s = weak.get();
                    if (s == nullptr || s->resetEpoch != epoch) return;
                    s->inputFile = juce::File();
                    s->inputName = name;
                    s->inputNote.clear();
                    s->sourceBits = 32;
                    s->diagnosis = *diag;
                    s->appliedScenarios.clear();
                    s->scenarioChanges.clear();
                    s->setInput (audio, rate, *meas);
                    const int locked = s->profileLocked;
                    s->status = "Learned " + juce::String (audio->getNumSamples() / rate, 1) + " s; diagnosis: "
                                + juce::String ((int) diag->findings.size()) + " finding(s)"
                                + (locked > 0 ? "; personal profile: " + juce::String (locked) + " knob(s) locked from your notes" : juce::String());
                    s->sendChangeMessage();
                });
                break;
            }
            case Job::Kind::loadInput:
            case Job::Kind::loadReference:
            {
                const bool isInput = job.kind == Job::Kind::loadInput;
                auto loaded = std::make_shared<LoadedAudio> (loadAudioFile (job.file));
                auto meas = std::make_shared<Measurement>();
                auto diag = std::make_shared<Diagnosis>();
                if (loaded->ok()) *meas = analyse (loaded->buffer, loaded->sampleRate);
                if (loaded->ok() && isInput) *diag = diagnose (loaded->buffer, loaded->sampleRate, job.settings);
                if (isInput) cache = PreLimiterCache();
                const auto file = job.file;
                juce::MessageManager::callAsync ([weak, loaded, meas, diag, file, isInput, epoch]
                {
                    auto* s = weak.get();
                    if (s == nullptr || s->resetEpoch != epoch) return;
                    if (! loaded->ok())
                    {
                        s->status = loaded->error;
                    }
                    else if (isInput)
                    {
                        s->inputFile = file;
                        s->inputName = file.getFileName();
                        s->inputNote = loaded->note;
                        s->sourceBits = loaded->sourceBits;
                        s->diagnosis = *diag;
                        s->appliedScenarios.clear();
                        s->scenarioChanges.clear();
                        s->clearOverridesQuietly();           // a new file is a new measurement: what was set on top of the old plan does not carry over
                        s->intent.referenceCurve.clear();
                        s->setInput (std::make_shared<const juce::AudioBuffer<float>> (std::move (loaded->buffer)), loaded->sampleRate, *meas);
                        const int locked = s->profileLocked;
                        s->status = "Loaded " + file.getFileName() + (loaded->note.isNotEmpty() ? "  (" + loaded->note + ")" : juce::String())
                                    + "; diagnosis: " + juce::String ((int) diag->findings.size()) + " finding(s)"
                                    + (locked > 0 ? "; personal profile: " + juce::String (locked) + " knob(s) locked from your notes" : juce::String());
                    }
                    else
                    {
                        s->referenceFile = file;
                        s->referenceName = file.getFileName();
                        s->reference = std::make_shared<const juce::AudioBuffer<float>> (std::move (loaded->buffer));
                        s->refMeas = *meas;
                        s->status = "Reference " + file.getFileName() + ": " + fmt (meas->integrated) + " LUFS, " + fmt (meas->truePeak, 2) + " dBTP";
                        s->recordUndo();                      // the reference choice is part of the intent: one step
                    }
                    s->sendChangeMessage();
                });
                break;
            }
            case Job::Kind::render:
            case Job::Kind::match:
            {
                if (job.in == nullptr) break;
                cache.keepTaps = job.name == "taps";
                cache.wantProxy = job.wantProxy;
                auto result = std::make_shared<RenderResult>();
                auto settingsOut = std::make_shared<MasterSettings> (job.settings);
                auto message = std::make_shared<juce::String>();
                if (job.kind == Job::Kind::render)
                {
                    // TARGET LOCK on: every change re-solves the drive so the output stays on the target.
                    MatchResult m;
                    *result = MasterChain::renderLocked (*job.in, job.rate, *settingsOut, cache, &cancelRender, &m);
                    if (settingsOut->targetLock && ! m.reached && m.message.isNotEmpty()) *message = m.message;
                }
                else
                {
                    const auto m = MasterChain::matchTarget (*job.in, job.rate, *settingsOut, cache, result.get(), &cancelRender);
                    *message = m.message;
                    if (result->output != nullptr) MasterChain::storeSolvedThresholds (*settingsOut, result->glue);
                    if (result->output == nullptr) result->cancelled = true;
                }
                if (result->cancelled) break;
                // Stage signals for the live meters: 16-bit copies, then the float copies are released.
                auto tapEq = std::make_shared<Player::TapPtr>(), tapComp = std::make_shared<Player::TapPtr>();
                if (result->taps.afterDynamicEq != nullptr) *tapEq = TapBuffer::make (*result->taps.afterDynamicEq);
                if (result->taps.afterCompressor != nullptr) *tapComp = TapBuffer::make (*result->taps.afterCompressor);
                result->taps = {};
                cache.taps = {};
                const int gen = job.generation;
                const bool isMatch = job.kind == Job::Kind::match;
                juce::MessageManager::callAsync ([weak, result, settingsOut, message, gen, isMatch, tapEq, tapComp, epoch]
                {
                    auto* s = weak.get();
                    if (s == nullptr || s->resetEpoch != epoch) return;
                    if (message->isNotEmpty() && gen == s->requestedGeneration) s->status = *message;
                    if (*tapEq != nullptr || *tapComp != nullptr)
                    {
                        s->keepEq = *tapEq;
                        s->keepComp = *tapComp;
                    }
                    s->applyRender (*result, gen, settingsOut.get());
                    if (gen == s->requestedGeneration && s->calibrationStep (result->measured.integrated)) s->renderRequest (Job::Kind::render);
                });
                break;
            }
        }
        --jobsPending;
        juce::MessageManager::callAsync ([weak] { if (auto* s = weak.get()) s->sendChangeMessage(); });
    }
}

//==============================================================================
void Session::loadInput (const juce::File& f)
{
    Job j;
    j.kind = Job::Kind::loadInput;
    j.file = f;
    j.settings = settings;
    status = "Loading " + f.getFileName() + " ...";
    enqueue (std::move (j));
    sendChangeMessage();
}

void Session::loadReference (const juce::File& f)
{
    Job j;
    j.kind = Job::Kind::loadReference;
    j.file = f;
    status = "Loading reference " + f.getFileName() + " ...";
    enqueue (std::move (j));
    sendChangeMessage();
}

void Session::learnInput (std::shared_ptr<const juce::AudioBuffer<float>> audio, double rate, const juce::String& name)
{
    if (audio == nullptr || audio->getNumSamples() == 0) return;
    Job j;
    j.kind = Job::Kind::learn;
    j.in = std::move (audio);
    j.rate = rate;
    j.name = name;
    j.settings = settings;
    status = "Analysing what LEARN heard ...";
    enqueue (std::move (j));
    sendChangeMessage();
}

void Session::setInput (Player::BufferPtr b, double rate, const Measurement& m)
{
    keepAlive = output;
    input = std::move (b);
    output = input;
    sampleRate = rate;
    inMeas = m;
    outMeas = m;
    compGr = {};
    limGr = {};
    glue = {};
    warnings.clear();
    lastRenderKey.clear();
    procMeas = {};
    procWarnings.clear();
    haveSolvedThresholds = false;
    player.setSources (input, output, rate);
    updateOriginalGain();
    // The one analysis of this source: from here on the measurements do not change, only the intent does.
    rebuildMeasurements (inputFile.existsAsFile() || ! inputName.startsWith ("LEARNED"));
    replan();
    profileLocked = applyProfile();
    settingsChanged();
    clearUndoHistory();                // a new source is a new history: what was undoable belonged to the old measurements
}

void Session::setInputForTesting (juce::AudioBuffer<float> b, double rate, const juce::String& name)
{
    inputName = name;
    sourceBits = 24;
    const auto m = analyse (b, rate);
    diagnosis = diagnose (b, rate, settings);
    appliedScenarios.clear();
    scenarioChanges.clear();
    applyProfile();
    setInput (std::make_shared<const juce::AudioBuffer<float>> (std::move (b)), rate, m);
}

void Session::setReferenceForTesting (juce::AudioBuffer<float> b, double rate, const juce::String& name)
{
    referenceName = name;
    refMeas = analyse (b, rate);
    reference = std::make_shared<const juce::AudioBuffer<float>> (std::move (b));
    recordUndo();                       // as when a reference file finishes loading
}

void Session::renderNowForTesting()
{
    {
        const juce::ScopedLock sl (jobLock);
        jobsPending -= (int) jobs.size();
        jobs.clear();
    }
    if (input == nullptr) return;
    for (int pass = 0; pass < 4; ++pass)
    {
        PreLimiterCache local;
        local.keepTaps = keepStageTaps;
        local.wantProxy = keepProxy;
        auto solved = settings;
        solved.bypass = false;                       // BYPASS is a monitor switch: the render is always the processed master
        {
            auto probe = settings;
            probe.bypass = false;
            lastRenderKey = settingsFingerprint (probe);
        }
        auto r = MasterChain::renderLocked (*input, sampleRate, solved, local);
        if (r.taps.afterDynamicEq != nullptr) keepEq = TapBuffer::make (*r.taps.afterDynamicEq);
        if (r.taps.afterCompressor != nullptr) keepComp = TapBuffer::make (*r.taps.afterCompressor);
        r.taps = {};
        applyRender (r, requestedGeneration, &solved);
        if (! calibrationStep (r.measured.integrated)) break;
    }
}

void Session::renderRequest (Job::Kind kind)
{
    if (input == nullptr) return;
    Job j;
    j.kind = kind;
    j.settings = settings;
    j.in = input;
    j.rate = sampleRate;
    j.name = keepStageTaps ? "taps" : "";
    j.wantProxy = keepProxy;
    j.generation = ++requestedGeneration;
    {
        // The job always renders the processed master; BYPASS is a monitor switch (the player in the app, the real-time
        // chain's own crossfade in the plug-in) and never costs a render.
        j.settings.bypass = false;
        auto probe = settings;
        probe.bypass = false;
        lastRenderKey = settingsFingerprint (probe);
    }
    enqueue (std::move (j));
}

void Session::showBypassState()
{
    // While bypassed the table and the loudness line show the input (as the old whole-file bypass render did).
    outMeas = settings.bypass ? inMeas : procMeas;
    warnings = settings.bypass ? juce::StringArray() : procWarnings;
    updateOriginalGain();
}

void Session::noteHandEdit (bool)
{
    // v2 stopped the automatic skip for finished input when a control was touched. v3 has no automatic skip: the plan decides
    // from the measurements, and an override on finished material is refused and said so.
}

void Session::settingsChanged()
{
    holdLocks();
    absorbEdits();
    {
        const auto key = calibrationKey();
        if (key != calibKey) { calibKey = key; calibRounds = 0; measurements.stageDeltaDb = 0.0; }   // a new intent: the stages differ, the measured delta is void
    }
    replan();
    recordUndo();                               // the intent is final here: one history step (or the end of a gesture will make it)
    if (onSettingsEdited) onSettingsEdited();
    shown = settings;                           // after the hook: the parameters may have rounded or clamped a value
    if (instantBypass) player.setBypass (settings.bypass);
    {
        auto probe = settings;
        probe.bypass = false;
        if (input != nullptr && output != nullptr && procMeas.valid && lastRenderKey == settingsFingerprint (probe))
        {
            // Only the BYPASS switch changed: nothing to render, nothing to solve, no state to change.
            showBypassState();
            refreshStatus();
            sendChangeMessage();
            return;
        }
    }
    if (input == nullptr && resolveWithoutAudio && resolveWithoutAudio())
    {
        refreshStatus();
        sendChangeMessage();
        return;
    }
    renderRequest (Job::Kind::render);
    refreshStatus();                            // after the request: the master measured so far is not this plan's
    sendChangeMessage();
}

void Session::matchTarget()
{
    // MATCH TARGET is gone (SPEC_V3 section 1): the target is part of the intent and the plan always meets it as far as the caps allow.
    if (input == nullptr) { status = "Load a file first."; sendChangeMessage(); return; }
    settingsChanged();
}

void Session::matchTone()
{
    if (input == nullptr || reference == nullptr || inMeas.spectrumDb.empty() || refMeas.spectrumDb.empty())
    {
        status = "MATCH TONE needs a source and a reference (REF).";
        sendChangeMessage();
        return;
    }
    // The reference is part of the intent. The plan applies its tone only with CHARACTER FULL, and never on a finished master.
    intent.referenceCurve = MasterChain::toneMatchCurve (inMeas.spectrumDb, refMeas.spectrumDb, EqSettings(), settings.toneSmoothOct);
    intent.referenceLoudness = refMeas.integrated;
    intent.character = PlanCharacter::FULL;
    pendingUndoName = "Match tone";
    settingsChanged();
}

void Session::matchLoudness()
{
    if (reference == nullptr || ! std::isfinite (refMeas.integrated))
    {
        status = "MATCH LOUD needs a reference with a measurable loudness.";
        sendChangeMessage();
        return;
    }
    intent.target = PlanTarget::CUSTOM;
    intent.customLufs = juce::jlimit (-30.0, -4.0, std::round (refMeas.integrated * 10.0) / 10.0);
    intent.referenceLoudness = refMeas.integrated;
    pendingUndoName = "Match loudness";
    settingsChanged();
}

void Session::applyGenre (Genre g)
{
    // A starting point used to write a dozen knobs at once (SPEC_V3: "genre moved nothing, other buttons moved every knob").
    // It now changes nothing; the intent is the target and the character.
    juce::ignoreUnused (g);
    status = "Starting points are gone: they wrote knobs. Pick the TARGET and the CHARACTER (OFF, GENTLE, FULL); the plan shows what it does and why.";
    sendChangeMessage();
}

void Session::applyScenario (int sc)
{
    if (sc < 1 || sc > kNumScenarios) return;
    // The diagnosis is information. With CHARACTER FULL the plan applies the findings it can (bounded, in dB, with the reason).
    status = juce::String (scenarioName (sc)) + " is a finding, not a setting: it writes no knob. "
             + (intent.character == PlanCharacter::FULL ? "CHARACTER FULL already applies what it can; the plan lists each step."
                                                        : "Set CHARACTER to FULL and MIHENK applies the findings it can, with the numbers.");
    sendChangeMessage();
}

void Session::removeScenario (int sc)
{
    juce::ignoreUnused (sc);
    appliedScenarios.clear();
    scenarioChanges.clear();
    sendChangeMessage();
}

void Session::applyRender (const RenderResult& r, int generation, const MasterSettings* solved)
{
    if (generation < renderedGeneration || r.output == nullptr) return;
    renderedGeneration = generation;
    // The AUTO thresholds the engine solved (what the compressor and the dynamic bands read off this source) go back to the panel,
    // unless the settings have changed again since this job was queued. They are a display: the plan owns every other value.
    if (solved != nullptr && generation == requestedGeneration)
    {
        MasterChain::storeSolvedThresholds (settings, r.glue);
        MasterChain::storeSolvedThresholds (shown, r.glue);
        if (r.glue.valid)
        {
            solvedCompThreshold = settings.comp.thresholdDb;
            for (int b = 0; b < 3; ++b) solvedBandThreshold[b] = settings.comp.bands[b].thresholdDb;
            solvedAtTrimDb = solved->trimDb;
            haveSolvedThresholds = true;
        }
    }
    glue = r.glue;
    inputStage = r.input;
    dynEq = r.dynEq;
    keepAlive = output;
    output = r.output;
    outMeas = r.measured;
    procMeas = r.measured;
    procWarnings = r.warnings;
    compGr = r.comp;
    limGr = r.limiter.gr;
    limResult = r.limiter;
    warnings = r.warnings;
    if (settings.bypass) { outMeas = inMeas; warnings.clear(); }
    player.setMaster (output);
    {
        auto g = std::make_shared<GrHistories>();
        g->hop = juce::jmax (1, (int) std::lround (0.01 * sampleRate));
        g->comp = compGr.history;
        if (glue.threeBand) for (int b = 0; b < 3; ++b) g->band[b] = glue.bands[b].gr.history;
        g->lim = limGr.history;
        keepGr = g;
        player.setTaps (keepEq, keepComp, keepGr);
    }
    updateOriginalGain();
    refreshStatus();
    if (onRendered) onRendered (r, settings);
    sendChangeMessage();
}

void Session::updateOriginalGain()
{
    // A is matched to B by integrated loudness (LEVEL-MATCHED, the default), so louder does not win the comparison.
    player.setOriginalGain ((float) dbToGain (originalGainDb()));
}

float Session::originalGainDb() const
{
    if (! abMatched) return 0.0f;
    // Against the processed master, also while BYPASS shows the input in the table: bypass and A/B are one level-matched comparison.
    const double master = procMeas.valid ? procMeas.integrated : outMeas.integrated;
    return (std::isfinite (master) && std::isfinite (inMeas.integrated)) ? (float) (master - inMeas.integrated) : 0.0f;
}

void Session::setAbLevelMatched (bool on)
{
    abMatched = on;
    updateOriginalGain();
    status = on ? "A/B LEVEL-MATCHED: the original plays at the master's loudness (" + fmt (originalGainDb(), 1) + " dB), so only the sound differs."
                : "A/B RAW LEVELS: the original plays at its own level; the louder one will sound better.";
    sendChangeMessage();
}

LoudnessStory Session::loudness() const
{
    if (! inMeas.valid) return {};
    const bool haveOut = outMeas.valid && output != nullptr && output != input;
    auto st = MasterChain::loudnessStory (settings, inMeas.integrated, haveOut || settings.bypass ? outMeas.integrated : kNegInf, limGr.maxDb);
    juce::String limiterText;
    for (const auto& a : plan.explain) if (a.action == "limiter") limiterText = a.reason;
    if (st.valid && ! settings.bypass && measurements.present && plan.targetUnreachable && plan.refused.size() > 0)
    {
        // Even the limiter cannot get there (or KEEP INPUT hits its cap): say so with the numbers, not "turned up".
        const double needed = plan.desiredGainDb, applied = plan.totalGainDb;
        st.sentence = plan.refused[0] + ".";
        st.shortLine = "OUT OF REACH: +" + fmt (needed, 1) + " DB NEEDED, +" + fmt (applied, 1) + " APPLIED";
        st.line2 = "OUT OF REACH: +" + fmt (needed, 1) + " NEEDED, +" + fmt (applied, 1) + " APPLIED";
        st.severity = 2;
    }
    else if (st.valid && ! settings.bypass && measurements.present && plan.limiterStatus.isNotEmpty())
    {
        // SPEC_V3 2 REVISED: the chosen target is reached; say what the limiter costs, in dB.
        const juce::String t = fmt (settings.targetLufs, 0);
        st.sentence = plan.limiterStatus;
        st.shortLine = t + " LUFS REACHED: LIMITER UP TO " + fmt (plan.predictedPeakGrDb, 1) + " DB (AVG " + fmt (plan.predictedAverageGrDb, 1) + ")";
        st.line2 = "LIMITER UP TO " + fmt (plan.predictedPeakGrDb, 1) + " DB, AVG " + fmt (plan.predictedAverageGrDb, 1) + " DB";
        st.severity = plan.limiterSeverity;
    }
    else if (st.valid && ! settings.bypass && measurements.present && limiterText.isNotEmpty())
    {
        st.sentence = "Target needs +" + fmt (plan.desiredGainDb, 1) + " dB; " + limiterText + ".";
        st.shortLine = "HELD: +" + fmt (plan.desiredGainDb, 1) + " DB NEEDED, +" + fmt (plan.totalGainDb, 1) + " APPLIED";
        st.line2 = "HELD: +" + fmt (plan.desiredGainDb, 1) + " NEEDED, +" + fmt (plan.totalGainDb, 1) + " APPLIED";
        st.severity = 1;
    }
    return st;
}

double Session::toneDistanceBefore() const
{
    if (reference == nullptr || inMeas.spectrumDb.empty() || refMeas.spectrumDb.empty()) return -1.0;
    return MasterChain::toneDistanceDb (inMeas.spectrumDb, refMeas.spectrumDb);
}

double Session::toneDistanceNow() const
{
    if (reference == nullptr || outMeas.spectrumDb.empty() || refMeas.spectrumDb.empty()) return -1.0;
    return MasterChain::toneDistanceDb (outMeas.spectrumDb, refMeas.spectrumDb);
}

void Session::matchToReference()
{
    if (input == nullptr || reference == nullptr || inMeas.spectrumDb.empty() || refMeas.spectrumDb.empty() || ! std::isfinite (refMeas.integrated))
    {
        status = "MATCH TO REFERENCE needs a source and a measurable reference (REF).";
        sendChangeMessage();
        return;
    }
    // The reference is intent: its tone (applied by CHARACTER FULL) and its loudness as the target.
    intent.referenceCurve = MasterChain::toneMatchCurve (inMeas.spectrumDb, refMeas.spectrumDb, EqSettings(), settings.toneSmoothOct);
    intent.referenceLoudness = refMeas.integrated;
    intent.character = PlanCharacter::FULL;
    intent.target = PlanTarget::CUSTOM;
    intent.customLufs = juce::jlimit (-30.0, -4.0, std::round (refMeas.integrated * 10.0) / 10.0);
    pendingUndoName = "Match to reference";
    settingsChanged();
}

void Session::clearReference()
{
    reference = nullptr;
    referenceFile = juce::File();
    referenceName.clear();
    refMeas = {};
    intent.referenceCurve.clear();
    intent.referenceLoudness = kNegInf;
    settingsChanged();
}

void Session::resetAll (bool keepInputFile)
{
    {
        const juce::ScopedLock sl (jobLock);
        jobsPending -= (int) jobs.size();
        jobs.clear();
    }
    cancelRender = true;               // a render that is running stops; whatever finishes later carries the old epoch and is dropped
    ++resetEpoch;
    renderedGeneration = ++requestedGeneration;
    cacheClear = true;
    wake.signal();

    intent = Intent();                 // target, ceiling, character, reference and every override, as new
    ovDelta.clear();
    ovSwitch.clear();
    haveSolvedThresholds = false;
    settings.bypass = false;
    for (auto& l : locks) l = {};
    overridden.clear();
    appliedScenarios.clear();
    scenarioChanges.clear();
    reference = nullptr;
    referenceFile = juce::File();
    referenceName.clear();
    refMeas = {};
    lastRenderKey.clear();
    procMeas = {};
    procWarnings.clear();
    if (! keepInputFile)
    {
        keepAlive = output;
        input = nullptr;
        output = nullptr;
        inputFile = juce::File();
        inputName.clear();
        inputNote.clear();
        sampleRate = 0.0;
        sourceBits = 0;
        inMeas = outMeas = {};
        diagnosis = {};
        compGr = {};
        limGr = {};
        glue = {};
        inputStage = {};
        dynEq = {};
        limResult = {};
        warnings.clear();
        keepEq = nullptr;
        keepComp = nullptr;
        keepGr = nullptr;
        player.setSources (nullptr, nullptr, 48000.0);
    }
    if (! keepInputFile) measurements = Measurements(), measurements.present = false;
    replan();
    status = "RESET ALL: a new instance. Target, character, every override, the reference and what MIHENK had learned are cleared.";
    if (keepInputFile && input != nullptr)
    {
        pendingUndoName = "Reset all";             // the file stays loaded, so this is one step that undo can take back
        gestureDepth = 0;
        gestureDirty = false;
        settingsChanged();
    }
    else
    {
        if (onSettingsEdited) onSettingsEdited();
        shown = settings;
        clearUndoHistory();                         // the source is gone too: nothing to go back to
    }
    sendChangeMessage();
}

//==============================================================================
// Personal profile

void Session::setProfileEnabled (bool on)
{
    if (on == profileEnabled) return;
    profileEnabled = on;
    if (! on)
    {
        for (auto& l : locks)
        {
            for (const auto& id : l.ids) { ovDelta.erase (id); ovSwitch.erase (id); }
            l = {};
        }
        overridden.clear();
        status = "Personal profile off: no knob is locked; your notes stay on this computer.";
    }
    else
    {
        const int n = diagnosis.m.valid ? applyProfile() : 0;
        status = "Personal profile on" + (n > 0 ? ": " + juce::String (n) + " knob(s) locked from your notes on similar tracks" : juce::String (": MIHENK learns from your listening notes (NOTE)"));
    }
    settingsChanged();
}

int Session::applyProfile()
{
    // The profile is a default intent: what it proposes goes in as overrides (shown as such) and the knobs it moved are locked.
    for (auto& l : locks)
    {
        for (const auto& id : l.ids) { ovDelta.erase (id); ovSwitch.erase (id); }
        l = {};
    }
    overridden.clear();
    if (! profileEnabled || ! diagnosis.m.valid) return 0;
    replan();
    const auto proposals = profile::propose (profile.learned(), profile::featuresOf (diagnosis.m), settings);
    if (proposals.empty()) return 0;
    auto edited = settings;
    std::vector<juce::StringArray> idsOf;
    for (const auto& p : proposals)
    {
        auto one = settings;
        profile::set (one, p.param, p.value);
        juce::StringArray ids;
        for (const auto& id : overrideIds())
        {
            const double a = settingValue (one, id), b = settingValue (settings, id);
            if (std::isfinite (a) && std::abs (a - b) > 1.0e-9) ids.add (id);
        }
        idsOf.push_back (ids);
        profile::set (edited, p.param, p.value);
    }
    settings = edited;
    absorbEdits();
    replan();
    int n = 0;
    for (size_t k = 0; k < proposals.size(); ++k)
    {
        const auto& p = proposals[k];
        auto& l = locks[(size_t) p.param];
        l.on = true;
        l.value = profile::get (settings, p.param);
        l.evidence = p.evidence;
        l.reason = p.reason;
        l.ruleKey = p.ruleKey;
        l.ids = idsOf[k];
        ++n;
    }
    return n;
}

int Session::lockCount() const
{
    int n = 0;
    for (auto& l : locks) n += l.on ? 1 : 0;
    return n;
}

void Session::unlock (int p)
{
    if (! isLocked (p)) return;
    overridden[p] = locks[(size_t) p].value;
    locks[(size_t) p].on = false;          // the override stays where it is; it is the user's from now on
    locks[(size_t) p].ids.clear();
    status = juce::String (profile::def (p).knob) + " unlocked: your setting is learned with your next note.";
}

void Session::holdLocks()
{
    for (int p = 0; p < profile::kNumParams; ++p)
    {
        const auto& l = locks[(size_t) p];
        if (l.on && std::abs (profile::get (settings, p) - l.value) > 1.0e-4) profile::set (settings, p, l.value);
    }
}

bool Session::addNote (const juce::StringArray& tags, const juce::String& text, const juce::String& startingPoint, juce::String& error)
{
    if (! profileEnabled) { error = "Turn the personal profile on first."; return false; }
    if (! diagnosis.m.valid) { error = "Load a track (or LEARN) first: a note is saved with its measurements."; return false; }
    if (tags.isEmpty() && text.trim().isEmpty()) { error = "Pick a tag or write a note."; return false; }
    profile::Context c;
    c.measures = &diagnosis.m;
    c.input = &inMeas;
    c.output = instantBypass && procMeas.valid ? &procMeas : &outMeas;
    c.limiter = &limGr;
    c.findings = &diagnosis.findings;
    auto noted = settings;
    if (instantBypass) noted.bypass = false;
    c.settings = &noted;
    c.source = profileSource;
    c.track = inputName;
    c.startingPoint = startingPoint;
    for (int sc : appliedScenarios) c.scenarios.add (scenarioId (sc));
    for (auto& [p, from] : overridden)
    {
        const double to = profile::get (settings, p);
        if (std::abs (to - from) > 1.0e-6) c.overrides.push_back ({ p, from, to });
    }
    if (! profile.add (profile::makeNote (tags, text, c), error)) return false;
    overridden.clear();
    status = "Note saved (" + juce::String ((int) profile.notes().size()) + " in your profile).";
    sendChangeMessage();
    return true;
}

juce::var Session::profileStateToVar() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("enabled", profileEnabled);
    juce::Array<juce::var> arr;
    for (int p = 0; p < profile::kNumParams; ++p)
    {
        const auto& l = locks[(size_t) p];
        if (! l.on) continue;
        auto* x = new juce::DynamicObject();
        x->setProperty ("knob", profile::def (p).id);
        x->setProperty ("value", l.value);
        x->setProperty ("evidence", l.evidence);
        x->setProperty ("reason", l.reason);
        x->setProperty ("rule", l.ruleKey);
        arr.add (juce::var (x));
    }
    o->setProperty ("locks", arr);
    return juce::var (o);
}

void Session::profileStateFromVar (const juce::var& v)
{
    for (auto& l : locks) l = {};
    overridden.clear();
    profileEnabled = v.isObject() && (bool) v.getProperty ("enabled", false);
    if (! profileEnabled) return;
    if (auto* arr = v["locks"].getArray())
        for (auto& x : *arr)
        {
            const int p = profile::paramFromId (x["knob"].toString());
            if (p < 0) continue;
            auto& l = locks[(size_t) p];
            l.on = true;
            l.value = x.getProperty ("value", 0.0);
            l.evidence = x.getProperty ("evidence", 0);
            l.reason = x["reason"].toString();
            l.ruleKey = x["rule"].toString();
        }
}

//==============================================================================
// v3: intent, plan, overrides

namespace
{
bool differs (double a, double b) { return ! (std::abs (a - b) <= 1.0e-4 + 2.0e-6 * juce::jmax (std::abs (a), std::abs (b))); }

/** Switches and choices: stored as the absolute value; everything else is stored as a delta over the plan. */
bool isSwitchId (const juce::String& id)
{
    static const juce::StringArray sw { "dc", "eqOn", "eqLinear", "lowCutOn", "lowCutSteep", "airHz", "airDyn", "lowMidOn", "upperMidOn", "tightOn",
                                        "compOn", "compMode", "compAuto", "autoRel", "phaseSafe", "widthOn" };
    if (sw.contains (id)) return true;
    return id.length() > 2 && id[0] == 'b' && id[1] >= '1' && id[1] <= '3' && (id.endsWithIgnoreCase ("bypass") || id.endsWithIgnoreCase ("solo") || id.endsWithIgnoreCase ("auto"));
}
} // namespace

PlanTarget Session::targetOfPreset (TargetPreset p)
{
    switch (p)
    {
        case TargetPreset::streaming: return PlanTarget::STREAM_14;
        case TargetPreset::apple:     return PlanTarget::APPLE_16;
        case TargetPreset::club:      return PlanTarget::CLUB_8;
        case TargetPreset::keepInput: return PlanTarget::KEEP_INPUT;
        case TargetPreset::custom:    break;
    }
    return PlanTarget::CUSTOM;
}

TargetPreset Session::presetOfTarget (PlanTarget t)
{
    switch (t)
    {
        case PlanTarget::STREAM_14:  return TargetPreset::streaming;
        case PlanTarget::APPLE_16:   return TargetPreset::apple;
        case PlanTarget::CLUB_8:     return TargetPreset::club;
        case PlanTarget::KEEP_INPUT: return TargetPreset::keepInput;
        case PlanTarget::ALBUM:
        case PlanTarget::CUSTOM:     break;
    }
    return TargetPreset::custom;
}

void Session::setTarget (PlanTarget t) { intent.target = t; settingsChanged(); }
void Session::setCustomTarget (double lufs) { intent.target = PlanTarget::CUSTOM; intent.customLufs = juce::jlimit (-30.0, -4.0, lufs); settingsChanged(); }
void Session::setCeiling (double dbtp) { intent.ceilingDbtp = juce::jlimit (-3.0, -0.1, dbtp); settingsChanged(); }
void Session::setCharacter (PlanCharacter c) { intent.character = c; settingsChanged(); }

void Session::rebuildOverrides()
{
    intent.overrides.clear();
    for (const auto& [id, d] : ovDelta)
    {
        const double base = settingValue (basePlan.settings, id);
        if (std::isfinite (base) && std::isfinite (d)) intent.overrides[id] = base + d;
    }
    for (const auto& [id, v] : ovSwitch)
        if (std::isfinite (v)) intent.overrides[id] = v;
}

void Session::absorbEdits()
{
    const auto& st = settings;
    const auto& sh = shown;
    // Intent: the target and the ceiling.
    if (st.targetPreset != sh.targetPreset)
    {
        intent.target = targetOfPreset (st.targetPreset);
        if (st.targetPreset == TargetPreset::custom && std::isfinite (st.targetLufs)) intent.customLufs = juce::jlimit (-30.0, -4.0, st.targetLufs);
    }
    else if (st.targetPreset == TargetPreset::custom && differs (st.targetLufs, sh.targetLufs) && std::isfinite (st.targetLufs))
    {
        intent.target = PlanTarget::CUSTOM;
        intent.customLufs = juce::jlimit (-30.0, -4.0, st.targetLufs);
    }
    if (differs (st.limiter.ceilingDbtp, sh.limiter.ceilingDbtp) && std::isfinite (st.limiter.ceilingDbtp))
        intent.ceilingDbtp = juce::jlimit (-3.0, -0.1, st.limiter.ceilingDbtp);

    // Overrides: every value that is not what was shown is the user's (or the host's) edit, kept as a delta over the plan.
    Intent bare = intent;
    bare.overrides.clear();
    const auto base = solvePlan (measurements, bare);
    for (const auto& id : overrideIds())
    {
        const double cur = settingValue (st, id), was = settingValue (sh, id);
        if (! std::isfinite (cur) || ! differs (cur, was)) continue;
        const double b = settingValue (base.settings, id);
        if (isSwitchId (id))
        {
            // A stage's own IN switch turned OFF while a knob of that stage is overridden: the plan has the stage out already, but a
            // hand-moved knob switches its stage in, so the "off" must be kept as an explicit override or the switch would do nothing.
            bool stageSwitch = false;
            const int stage = overrideStage (id, &stageSwitch);
            bool keepOff = false;
            if (stageSwitch && cur < 0.5 && std::abs (cur - b) < 0.5)
            {
                auto inStage = [&] (const juce::String& other) { bool sw = false; return other != id && overrideStage (other, &sw) == stage && ! sw; };
                for (const auto& kv : ovDelta) if (inStage (kv.first)) keepOff = true;
                for (const auto& kv : ovSwitch) if (inStage (kv.first)) keepOff = true;
            }
            if (std::abs (cur - b) < 0.5 && ! keepOff) ovSwitch.erase (id); else ovSwitch[id] = cur;
        }
        else
        {
            if (! differs (cur, b)) ovDelta.erase (id); else ovDelta[id] = cur - b;
        }
    }
}

juce::String Session::calibrationKey() const
{
    juce::String k;
    k << (int) intent.target << '|' << intent.customLufs << '|' << intent.albumTarget << '|' << intent.relativeDb << '|' << intent.ceilingDbtp << '|'
      << (int) intent.character << '|' << intent.referenceLoudness << '|' << (int) intent.referenceCurve.size();
    for (const auto& [id, d] : ovDelta) k << '|' << id << '=' << d;
    for (const auto& [id, v] : ovSwitch) k << '|' << id << '#' << v;
    return k;
}

bool Session::calibrationStep (double renderedLufs)
{
    if (input == nullptr || ! measurements.present || ! std::isfinite (renderedLufs)) return false;
    if (intent.target == PlanTarget::KEEP_INPUT) return false;     // it keeps the earlier rule
    if (plan.targetUnreachable || calibRounds >= 3) return false;
    if (intent.overrides.count ("trim") > 0 || intent.overrides.count ("drive") > 0) return false;   // the level is the user's: calibrating would pull a hand-set TRIM / DRIVE back to the target
    if (! (measurements.integrated > -70.0) || plan.totalGainDb <= -23.9) return false;
    const double miss = renderedLufs - plan.settings.targetLufs;
    if (! (std::abs (miss) > 0.15 && std::abs (miss) <= 6.0)) return false;
    // The render came out `miss` LU louder (or quieter) than the table says: that much is the stages'. Held as an absolute measurement.
    measurements.stageDeltaDb = juce::jlimit (-6.0, 6.0, measurements.stageDeltaDb + miss);
    ++calibRounds;
    replan();
    if (onSettingsEdited) onSettingsEdited();
    shown = settings;
    return true;
}

void Session::replan()
{
    Intent bare = intent;
    bare.overrides.clear();
    basePlan = solvePlan (measurements, bare);
    rebuildOverrides();
    plan = solvePlan (measurements, intent);
    const bool bypass = settings.bypass;
    const double warn = settings.limitWarnDb, smooth = settings.toneSmoothOct;
    settings = plan.settings;
    settings.bypass = bypass;
    settings.limitWarnDb = warn;
    settings.toneSmoothOct = smooth;
    settings.targetPreset = presetOfTarget (intent.target);
    settings.targetLufs = plan.settings.targetLufs;
    settings.targetLock = false;
    settings.autoTrim = false;
    settings.planned = true;
    if (haveSolvedThresholds)
    {
        // The thresholds were solved at one trim; at another trim the same compression needs the threshold moved by the same dB
        // (the compressor sits after the trim and is level-covariant), so a level change keeps the compression, and its make-up, the same.
        const double follow = settings.trimDb - solvedAtTrimDb;
        if (settings.comp.autoThreshold) settings.comp.thresholdDb = solvedCompThreshold + follow;
        for (int b = 0; b < 3; ++b)
            if (settings.comp.bands[b].autoThreshold) settings.comp.bands[b].thresholdDb = solvedBandThreshold[b] + follow;
    }
    shown = settings;
}

void Session::clearOverridesQuietly()
{
    ovDelta.clear();
    ovSwitch.clear();
    intent.overrides.clear();
    for (auto& l : locks) { l.on = false; l.ids.clear(); }
    overridden.clear();
}

void Session::clearOverrides()
{
    clearOverridesQuietly();
    status = "Overrides cleared: every knob shows the plan again.";
    pendingUndoName = "Reset overrides";
    settingsChanged();
}

void Session::applyPreset (const MasterSettings& loaded)
{
    // Level is the plan's: a preset made on another song must not carry its trim or drive.
    auto edited = loaded;
    edited.trimDb = settings.trimDb;
    edited.limiter.driveDb = settings.limiter.driveDb;
    edited.bypass = settings.bypass;
    edited.limiter.enabled = true;
    // What the preset sets that the plan would not: against the neutral plan, not against this song's.
    ovDelta.clear();
    ovSwitch.clear();
    Intent bare = intent;
    bare.overrides.clear();
    bare.character = PlanCharacter::OFF;
    const auto neutral = solvePlan (measurements, bare);
    shown = neutral.settings;
    shown.targetPreset = settings.targetPreset;
    shown.targetLufs = settings.targetLufs;
    shown.limiter.ceilingDbtp = settings.limiter.ceilingDbtp;
    shown.trimDb = edited.trimDb;
    shown.limiter.driveDb = edited.limiter.driveDb;
    settings = edited;
    pendingUndoName = "Load preset";
    settingsChanged();
}

double Session::overrideDelta (const juce::String& id) const
{
    const auto it = ovDelta.find (id);
    return it != ovDelta.end() ? it->second : 0.0;
}

void Session::rebuildMeasurements (bool songSecondsKnown)
{
    if (! inMeas.valid)
    {
        measurements = Measurements();
        measurements.present = false;
        return;
    }
    const double secs = (input != nullptr && sampleRate > 0.0) ? (double) input->getNumSamples() / sampleRate : inMeas.seconds;
    measurements = measurementsFrom (inMeas, diagnosis.m.valid ? &diagnosis.m : nullptr, songSecondsKnown ? secs : 0.0);
    if (! songSecondsKnown) measurements.songSeconds = 0.0;      // LEARN: the length of the song is not known
    measurements.present = true;
    calibKey.clear();                                  // the next settingsChanged starts the calibration of this source from nothing
    calibRounds = 0;
    if (input != nullptr && sampleRate > 0.0) measureLimiterResponse (*input, sampleRate, measurements);   // the real limiter's response on this audio, once
    else measurements.limiter = savedLimiter;                                                              // a reopened project: the response that was saved with the measurements
}

void Session::forgetSource()
{
    keepAlive = output;
    input = nullptr;
    output = nullptr;
    inputName.clear();
    sampleRate = 0.0;
    inMeas = outMeas = {};
    procMeas = {};
    diagnosis = {};
    compGr = {};
    limGr = {};
    glue = {};
    limResult = {};
    warnings.clear();
    procWarnings.clear();
    lastRenderKey.clear();
    player.setSources (nullptr, nullptr, 48000.0);
    savedLimiter = {};
    savedThresholds = {};
    savedStageDelta = 0.0;
    measurements = Measurements();
    measurements.present = false;
}

void Session::restoreState (const juce::var& savedIntent, const MasterSettings& legacy)
{
    ovDelta.clear();
    ovSwitch.clear();
    rebuildMeasurements (false);
    // A learned project: the AUTO thresholds LEARN solved are in the parameters (the real-time chain reads them there); they stay.
    // The parameters hold the thresholds the last render solved (a state without a LEARN block: the defaults, which are the plan's own).
    // The learned block carries them too, so a state that lost its parameters keeps the same sound.
    haveSolvedThresholds = true;
    solvedCompThreshold = legacy.comp.thresholdDb;
    for (int b = 0; b < 3; ++b) solvedBandThreshold[b] = legacy.comp.bands[b].thresholdDb;
    solvedAtTrimDb = legacy.trimDb;
    if (inMeas.valid && savedThresholds.valid)
    {
        solvedCompThreshold = savedThresholds.comp;
        for (int b = 0; b < 3; ++b) solvedBandThreshold[b] = savedThresholds.band[b];
        solvedAtTrimDb = savedThresholds.atTrim;
    }
    if (savedIntent.isObject())
    {
        intentFromVar (savedIntent);
        settings.bypass = legacy.bypass;
        settings.limitWarnDb = legacy.limitWarnDb;
        settings.toneSmoothOct = legacy.toneSmoothOct;
        if (inMeas.valid) { measurements.stageDeltaDb = juce::jlimit (-6.0, 6.0, savedStageDelta); calibKey = calibrationKey(); }
        replan();
        clearUndoHistory();
        return;
    }
    intent = Intent();
    intent.target = targetOfPreset (legacy.targetPreset);
    if (std::isfinite (legacy.targetLufs)) intent.customLufs = juce::jlimit (-30.0, -4.0, legacy.targetLufs);
    if (std::isfinite (legacy.limiter.ceilingDbtp)) intent.ceilingDbtp = juce::jlimit (-3.0, -0.1, legacy.limiter.ceilingDbtp);
    settings.bypass = legacy.bypass;
    settings.limitWarnDb = legacy.limitWarnDb;
    settings.toneSmoothOct = legacy.toneSmoothOct;
    replan();
    auto edited = legacy;                       // its level (trim, drive, AUTO) belonged to the old solver: the plan owns it now ...
    if (measurements.present)               // ... unless nothing was measured (a state that lost its LEARN block keeps the level its parameters hold)
    {
        edited.trimDb = settings.trimDb;
        edited.limiter.driveDb = settings.limiter.driveDb;
    }
    edited.limiter.ceilingDbtp = settings.limiter.ceilingDbtp;
    edited.targetPreset = settings.targetPreset;
    edited.targetLufs = settings.targetLufs;
    settings = edited;
    absorbEdits();
    replan();
    clearUndoHistory();
}

juce::var Session::intentToVar() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("target", (int) intent.target);
    o->setProperty ("customLufs", intent.customLufs);
    o->setProperty ("albumTarget", intent.albumTarget);
    o->setProperty ("relativeDb", intent.relativeDb);
    o->setProperty ("ceiling", intent.ceilingDbtp);
    o->setProperty ("character", (int) intent.character);
    auto* d = new juce::DynamicObject();
    for (const auto& [id, v] : ovDelta) d->setProperty (juce::Identifier (id), v);
    o->setProperty ("delta", juce::var (d));
    auto* w = new juce::DynamicObject();
    for (const auto& [id, v] : ovSwitch) w->setProperty (juce::Identifier (id), v);
    o->setProperty ("switch", juce::var (w));
    juce::Array<juce::var> curve;
    for (float f : intent.referenceCurve) curve.add ((double) f);
    o->setProperty ("referenceCurve", curve);
    return juce::var (o);
}

void Session::intentFromVar (const juce::var& v)
{
    if (! v.isObject()) return;
    auto num = [&] (const char* k, double def, double lo, double hi)
    {
        const double x = (double) v.getProperty (k, def);
        return std::isfinite (x) ? juce::jlimit (lo, hi, x) : def;
    };
    intent = Intent();
    intent.target = (PlanTarget) juce::jlimit (0, 5, (int) num ("target", 0, 0, 5));
    intent.customLufs = num ("customLufs", -14.0, -30.0, -4.0);
    intent.albumTarget = num ("albumTarget", -14.0, -30.0, -4.0);
    intent.relativeDb = num ("relativeDb", 0.0, -12.0, 12.0);
    intent.ceilingDbtp = num ("ceiling", -1.0, -3.0, -0.1);
    intent.character = (PlanCharacter) juce::jlimit (0, 2, (int) num ("character", 0, 0, 2));
    ovDelta.clear();
    ovSwitch.clear();
    auto read = [&] (const char* key, std::map<juce::String, double>& into)
    {
        if (auto* obj = v[key].getDynamicObject())
            for (const auto& prop : obj->getProperties())
            {
                const double x = (double) prop.value;
                if (std::isfinite (x) && overrideIds().contains (prop.name.toString())) into[prop.name.toString()] = juce::jlimit (-1.0e5, 1.0e5, x);
            }
    };
    read ("delta", ovDelta);
    read ("switch", ovSwitch);
    if (auto* arr = v["referenceCurve"].getArray())
        if (arr->size() == (int) spectrumGrid().size())
            for (auto& f : *arr) intent.referenceCurve.push_back ((float) juce::jlimit (-24.0, 24.0, (double) f));
}

namespace
{
juce::String overrideLabel (const juce::String& id, double delta, bool isSwitch, double value)
{
    juce::String name = id, unit;
    for (const auto& d : params::definitions())
        if (d.id == id) { name = d.name; unit = d.unit; break; }
    if (isSwitch) return name + (value > 0.5 ? " on" : " off");
    return name + " " + (delta >= 0.0 ? "+" : "") + juce::String (delta, 1) + (unit.isNotEmpty() ? " " + unit : juce::String()) + " over plan";
}
}

juce::String Session::planParagraph() const
{
    if (! measurements.present)
        return "Nothing measured yet. Load a file" + juce::String (inputName.isEmpty() ? " (or press LEARN and play the song)." : ".");
    juce::String t;
    t << "Measured " << fmt (measurements.integrated) << " LUFS, true peak " << fmt (juce::jmax (measurements.truePeak, measurements.samplePeak), 1) << " dBTP. ";
    t << plan.plainSummary() << ".";
    if (intent.character == PlanCharacter::OFF && ! plan.finishedMaster) t << " CHARACTER OFF: level and ceiling only.";
    juce::StringArray ov;
    for (const auto& [id, d] : ovDelta) ov.add (overrideLabel (id, d, false, 0.0));
    for (const auto& [id, v] : ovSwitch) ov.add (overrideLabel (id, 0.0, true, v));
    if (! ov.isEmpty())
    {
        const int n = ov.size();
        while (ov.size() > 3) ov.remove (ov.size() - 1);
        t << " Your changes: " << ov.joinIntoString ("; ") << (n > 3 ? "; +" + juce::String (n - 3) + " more" : juce::String()) << ".";
    }
    if (outputCurrent() && procMeas.valid)
        t << " Master measured " << fmt (procMeas.integrated) << " LUFS, " << fmt (procMeas.truePeak, 1) << " dBTP, limiter at most " << fmt (limGr.maxDb, 1) << " dB.";
    return t;
}

void Session::refreshStatus()
{
    if (! measurements.present) return;
    status = planParagraph();
}

bool Session::exportMaster (const juce::File& f, int bits, juce::String& message)
{
    if (! Entitlement::mayExport()) { message = "Export is not licensed."; return false; }
    if (! outputCurrent()) { message = "The master is still rendering; export when the RENDER lamp is off."; return false; }
    juce::String err;
    int clipped = 0;
    if (! exportWav (f, *output, sampleRate, bits, err, &clipped))
    {
        message = err;
        return false;
    }
    RenderResult r;
    r.comp = compGr;
    r.limiter = limResult;
    r.warnings = instantBypass ? procWarnings : warnings;
    if (clipped > 0) r.warnings.add (juce::String (clipped) + " samples clipped in quantisation.");
    const auto reportFile = f.getSiblingFile (f.getFileNameWithoutExtension() + ".mihenk.txt");
    auto reported = settings;
    if (instantBypass) reported.bypass = false;      // BYPASS is a monitor switch: the file is the processed master
    reportFile.replaceWithText (MasterChain::report (inputName, f.getFileName(), bits, reported, inMeas, instantBypass ? procMeas : outMeas, r,
                                                     reference != nullptr ? &refMeas : nullptr));
    message = "Exported " + f.getFileName() + " (" + juce::String (bits) + "-bit" + (bits == 16 ? ", TPDF dither" : "") + ") and " + reportFile.getFileName();
    status = message;
    sendChangeMessage();
    return true;
}
} // namespace mihenk

//==============================================================================
// Undo / redo

namespace mihenk
{
namespace
{
bool sameLock (const Session::Lock& a, const Session::Lock& b)
{
    return a.on == b.on && juce::exactlyEqual (a.value, b.value) && a.evidence == b.evidence && a.ruleKey == b.ruleKey && a.ids == b.ids;
}

juce::String targetText (PlanTarget t, double customLufs)
{
    switch (t)
    {
        case PlanTarget::STREAM_14:  return "STREAM -14";
        case PlanTarget::APPLE_16:   return "APPLE -16";
        case PlanTarget::CLUB_8:     return "CLUB -8";
        case PlanTarget::KEEP_INPUT: return "KEEP INPUT";
        case PlanTarget::ALBUM:      return "ALBUM";
        case PlanTarget::CUSTOM:     break;
    }
    return "CUSTOM " + juce::String (customLufs, 1) + " LUFS";
}

struct ParamName { juce::String name, unit; };
ParamName paramName (const juce::String& id)
{
    for (const auto& d : params::definitions())
        if (d.id == id) return { d.name, d.unit };
    return { id, {} };
}

struct LambdaAction : juce::UndoableAction
{
    LambdaAction (std::function<void()> u, std::function<void()> r) : undoFn (std::move (u)), redoFn (std::move (r)) {}
    bool perform() override { if (first) { first = false; return true; } if (redoFn) redoFn(); return true; }
    bool undo() override { if (undoFn) undoFn(); return true; }
    int getSizeInUnits() override { return 16; }
    std::function<void()> undoFn, redoFn;
    bool first = true;
};
} // namespace

struct Session::UndoStep : juce::UndoableAction
{
    UndoStep (Session& s, UndoState b, UndoState a, int g, std::vector<OvChange> o)
        : owner (s), before (std::move (b)), after (std::move (a)), groups (g), ov (std::move (o)) {}
    bool perform() override
    {
        if (first) { first = false; return true; }          // the user's own edit has already happened
        owner.applyUndoStep (*this, false);
        return true;
    }
    bool undo() override { owner.applyUndoStep (*this, true); return true; }
    int getSizeInUnits() override { return 32 + (int) (before.referenceCurve.size() + after.referenceCurve.size()) / 4 + 4 * (int) ov.size(); }

    Session& owner;
    UndoState before, after;
    int groups;
    std::vector<OvChange> ov;
    bool first = true;
};

Session::UndoState Session::captureUndoState() const
{
    UndoState u;
    u.target = intent.target;
    u.customLufs = intent.customLufs;
    u.albumTarget = intent.albumTarget;
    u.relativeDb = intent.relativeDb;
    u.ceilingDbtp = intent.ceilingDbtp;
    u.character = intent.character;
    u.referenceCurve = intent.referenceCurve;
    u.referenceLoudness = intent.referenceLoudness;
    u.ovDelta = ovDelta;
    u.ovSwitch = ovSwitch;
    u.locks = locks;
    u.overridden = overridden;
    u.profileEnabled = profileEnabled;
    u.reference = reference;
    u.referenceFile = referenceFile;
    u.referenceName = referenceName;
    if (reference != nullptr)
        u.refMeas = (committed.reference == reference && committed.refMeas != nullptr) ? committed.refMeas : std::make_shared<const Measurement> (refMeas);
    return u;
}

bool Session::diffUndoStates (const UndoState& a, const UndoState& b, int& groups, std::vector<OvChange>& ov)
{
    groups = 0;
    ov.clear();
    if (a.target != b.target || ! juce::exactlyEqual (a.customLufs, b.customLufs)) groups |= ugTarget;
    if (! juce::exactlyEqual (a.ceilingDbtp, b.ceilingDbtp)) groups |= ugCeiling;
    if (a.character != b.character) groups |= ugCharacter;
    if (a.referenceCurve != b.referenceCurve || ! juce::exactlyEqual (a.referenceLoudness, b.referenceLoudness)) groups |= ugRefCurve;
    if (a.reference != b.reference || a.referenceFile != b.referenceFile || a.referenceName != b.referenceName) groups |= ugRefChoice;
    if (a.profileEnabled != b.profileEnabled || a.overridden != b.overridden) groups |= ugLocks;
    for (size_t i = 0; i < a.locks.size(); ++i)
        if (! sameLock (a.locks[i], b.locks[i])) groups |= ugLocks;
    if (! juce::exactlyEqual (a.albumTarget, b.albumTarget) || ! juce::exactlyEqual (a.relativeDb, b.relativeDb)) groups |= ugAlbum;
    auto diffMap = [&] (const std::map<juce::String, double>& x, const std::map<juce::String, double>& y, bool isSwitch)
    {
        for (const auto& [id, v] : x)
        {
            const auto it = y.find (id);
            if (it == y.end()) ov.push_back ({ id, isSwitch, true, false, v, 0.0 });
            else if (! juce::exactlyEqual (it->second, v)) ov.push_back ({ id, isSwitch, true, true, v, it->second });
        }
        for (const auto& [id, v] : y)
            if (x.find (id) == x.end()) ov.push_back ({ id, isSwitch, false, true, 0.0, v });
    };
    diffMap (a.ovDelta, b.ovDelta, false);
    diffMap (a.ovSwitch, b.ovSwitch, true);
    return groups != 0 || ! ov.empty();
}

juce::String Session::describeUndo (const UndoState& a, const UndoState& b, int groups, const std::vector<OvChange>& ov)
{
    juce::StringArray parts;
    if (groups & ugTarget) parts.add ("Target " + targetText (b.target, b.customLufs));
    if (groups & ugCeiling) parts.add ("Ceiling " + juce::String (b.ceilingDbtp, 1) + " dBTP");
    if (groups & ugCharacter)
        parts.add (juce::String ("Character ") + (b.character == PlanCharacter::FULL ? "FULL" : b.character == PlanCharacter::GENTLE ? "GENTLE" : "OFF"));
    if (groups & ugRefChoice) parts.add (b.reference != nullptr ? "Reference " + b.referenceName : juce::String ("Clear reference"));
    if ((groups & ugRefCurve) && ! (groups & ugRefChoice)) parts.add (b.referenceCurve.empty() ? "Clear tone match" : "Tone match");
    if (groups & ugLocks) parts.add ("Profile locks");
    if (groups & ugAlbum) parts.add ("Album target");
    juce::ignoreUnused (a);
    if (ov.size() == 1 && parts.isEmpty())
    {
        const auto& c = ov.front();
        const auto n = paramName (c.id);
        if (! c.hasTo) return "Reset " + n.name;
        if (c.isSwitch) return n.name + (c.to > 0.5 ? " on" : " off");
        return n.name + " override " + (c.to >= 0.0 ? "+" : "") + juce::String (c.to, 1) + (n.unit.isNotEmpty() ? " " + n.unit : juce::String());
    }
    if (! ov.empty())
    {
        const bool removed = std::all_of (ov.begin(), ov.end(), [] (const OvChange& c) { return ! c.hasTo; });
        parts.add (removed ? juce::String ("Reset overrides") : juce::String ((int) ov.size()) + " overrides");
    }
    if (parts.size() == 1) return parts[0];
    return parts.isEmpty() ? juce::String ("Edit") : juce::String (parts.size()) + " changes";
}

void Session::recordUndo()
{
    if (restoring) return;
    if (gestureDepth > 0) { gestureDirty = true; return; }
    commitUndo();
}

void Session::commitUndo()
{
    auto cur = captureUndoState();
    int groups = 0;
    std::vector<OvChange> ov;
    if (! diffUndoStates (committed, cur, groups, ov))
    {
        pendingUndoName.clear();
        return;
    }
    const bool paramBackedOnly = (groups & ~(ugTarget | ugCeiling)) == 0;
    if (hostEditDepth > 0 || (undoHostOwnsParams && paramBackedOnly))
    {
        committed = std::move (cur);                 // the host owns this change (its own undo covers it); the next step is measured from here
        pendingUndoName.clear();
        return;
    }
    const auto name = pendingUndoName.isNotEmpty() ? pendingUndoName : describeUndo (committed, cur, groups, ov);
    pendingUndoName.clear();
    undoManager.beginNewTransaction (name);
    undoManager.perform (new UndoStep (*this, committed, cur, groups, ov));
    committed = std::move (cur);
}

void Session::applyUndoStep (const UndoStep& st, bool backward)
{
    const auto& src = backward ? st.before : st.after;
    {
        const juce::ScopedValueSetter<bool> guard (restoring, true);
        const int g = st.groups;
        if (g & ugTarget) { intent.target = src.target; intent.customLufs = src.customLufs; }
        if (g & ugCeiling) intent.ceilingDbtp = src.ceilingDbtp;
        if (g & ugCharacter) intent.character = src.character;
        if (g & ugAlbum) { intent.albumTarget = src.albumTarget; intent.relativeDb = src.relativeDb; }
        if (g & ugRefCurve) { intent.referenceCurve = src.referenceCurve; intent.referenceLoudness = src.referenceLoudness; }
        if (g & ugRefChoice)
        {
            reference = src.reference;
            referenceFile = src.referenceFile;
            referenceName = src.referenceName;
            refMeas = src.refMeas != nullptr ? *src.refMeas : Measurement();
        }
        if (g & ugLocks)
        {
            locks = src.locks;
            overridden = src.overridden;
            profileEnabled = src.profileEnabled;
        }
        for (const auto& c : st.ov)
        {
            auto& m = c.isSwitch ? ovSwitch : ovDelta;
            const bool has = backward ? c.hadFrom : c.hasTo;
            if (has) m[c.id] = backward ? c.from : c.to;
            else m.erase (c.id);
        }
        replan();                       // settings = the plan of the restored intent; shown = settings, so nothing below reads an edit
        settingsChanged();              // calibration key, parameters (the plug-in's host sees the move), render
    }
    committed = captureUndoState();
    sendChangeMessage();
}

bool Session::canUndo() const { return gestureDepth == 0 && undoManager.canUndo(); }
bool Session::canRedo() const { return gestureDepth == 0 && undoManager.canRedo(); }
juce::String Session::undoName() const { return canUndo() ? undoManager.getUndoDescription() : juce::String(); }
juce::String Session::redoName() const { return canRedo() ? undoManager.getRedoDescription() : juce::String(); }
int Session::undoSteps() const { return undoManager.getUndoDescriptions().size(); }
int Session::redoSteps() const { return undoManager.getRedoDescriptions().size(); }

bool Session::undo()
{
    if (gestureDepth > 0) return false;
    commitUndo();                       // anything that changed the intent without settingsChanged() becomes a step first
    if (! undoManager.canUndo()) return false;
    return undoManager.undo();
}

bool Session::redo()
{
    if (gestureDepth > 0 || ! undoManager.canRedo()) return false;
    return undoManager.redo();
}

void Session::beginUndoGesture()
{
    if (gestureDepth++ == 0) gestureDirty = false;
}

void Session::endUndoGesture()
{
    if (gestureDepth == 0) return;
    if (--gestureDepth > 0) return;
    if (gestureDirty) commitUndo();
    gestureDirty = false;
}

void Session::clearUndoHistory()
{
    undoManager.clearUndoHistory();
    committed = UndoState();
    committed = captureUndoState();
    gestureDirty = false;
    pendingUndoName.clear();
}

void Session::pushUndoAction (const juce::String& name, std::function<void()> undoFn, std::function<void()> redoFn)
{
    commitUndo();
    undoManager.beginNewTransaction (name);
    undoManager.perform (new LambdaAction (std::move (undoFn), std::move (redoFn)));
}
} // namespace mihenk
