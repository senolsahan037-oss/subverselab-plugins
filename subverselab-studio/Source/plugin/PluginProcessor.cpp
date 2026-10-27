#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "FactoryPresets.h"

namespace
{
juce::String niceName (const svl::ParamDef& d)
{
    // "osc1.string_decay" -> "Osc 1 String Decay", "tuning.row.eb" -> "Tuning Row Eb"
    juce::String s = juce::String (d.id).replace (".", " ").replace ("_", " ");
    if (s.startsWith ("osc")) s = "Osc " + s.substring (3);
    juce::StringArray w; w.addTokens (s, " ", "");
    for (auto& x : w) x = x.substring (0, 1).toUpperCase() + x.substring (1);
    return w.joinIntoString (" ");
}

juce::String noteName (int key)
{
    static const char* n[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return juce::String (n[key % 12]) + juce::String (key / 12 - 1);
}

juce::String valueText (const svl::ParamDef& d, float v)
{
    const juce::String u (d.unit);
    if (u == "Hz") return v >= 1000.0f ? juce::String (v / 1000.0f, v >= 10000.0f ? 1 : 2) + " kHz" : juce::String (v, v < 100.0f ? 1 : 0) + " Hz";
    if (u == "s") return v < 0.9995f ? juce::String (v * 1000.0f, v < 0.01f ? 1 : 0) + " ms" : juce::String (v, 2) + " s";
    if (u == "dB") return juce::String (v, 1) + " dB";
    if (u == "ct") return (v > 0 ? "+" : "") + juce::String (v, 1) + " ct";
    if (u == "st") return (v > 0 ? "+" : "") + juce::String (v, std::abs (v - std::round (v)) < 1.0e-3f ? 0 : 2) + " st";
    if (u == "oct") return (v > 0 ? "+" : "") + juce::String (v, std::abs (v - std::round (v)) < 1.0e-3f ? 0 : 2) + " oct";
    if (u == "steps") return juce::String (juce::roundToInt (v));
    if (u == "key") return noteName (juce::roundToInt (v));
    if (d.id.find ("pan") != std::string::npos)
        return std::abs (v) < 0.005f ? juce::String ("C") : (v < 0 ? "L" : "R") + juce::String (juce::roundToInt (std::abs (v) * 100.0f));
    if (d.id == "voice.polyphony" || d.id == "voice.choke") return juce::String (juce::roundToInt (v));
    const juce::String id (d.id);
    if (id.endsWith (".unison")) return juce::String (juce::roundToInt (v)) + (juce::roundToInt (v) == 1 ? "" : " x");
    if (id == "fm.amount") return juce::String (v, 2);
    if (id == "fx.comp.ratio") return juce::String (v, v < 10.0f ? 1 : 0) + ":1";
    return juce::String (juce::roundToInt (v * 100.0f)) + " %";
}

juce::NormalisableRange<float> rangeFor (const svl::ParamDef& d)
{
    juce::NormalisableRange<float> r (d.min, d.max);
    const juce::String u (d.unit), id (d.id);
    const bool whole = id.endsWith (".octave") || id.endsWith (".semi") || id == "voice.polyphony" || id == "voice.choke"
                       || id == "tuning.edo" || id.startsWith ("tuning.row_") || id == "tuning.keyswitch_low" || id == "voice.bend_range"
                       || id == "drum.pitch" || id.endsWith (".unison");
    if (whole) r.interval = 1.0f;
    auto centre = [&] (float c) { if (c > d.min && c < d.max) r.setSkewForCentre (c); };
    if (u == "Hz") centre (id == "filter.cutoff" ? 640.0f : (id == "click.tone" ? 2000.0f : ((id == "drift.rate" || id.endsWith (".rate")) ? 2.0f : d.min * 4.0f)));
    if (id == "fm.amount") centre (1.5f);
    else if (id == "fx.comp.ratio") centre (4.0f);
    else if (u == "s") centre (d.max >= 10.0f ? 1.0f : (d.max >= 1.0f ? d.max * 0.1f : d.max * 0.1f));
    return r;
}

std::unique_ptr<juce::RangedAudioParameter> makeParam (const svl::ParamDef& d)
{
    const juce::ParameterID pid (d.id, 1);
    if (d.isEnum)
    {
        juce::StringArray names;
        for (int k = 0; k < d.numNames; ++k) names.add (juce::String (d.names[k]).replace ("_", " "));
        return std::make_unique<juce::AudioParameterChoice> (pid, niceName (d), names, (int) d.def);
    }
    auto attrs = juce::AudioParameterFloatAttributes()
                     .withLabel (d.unit)
                     .withStringFromValueFunction ([d] (float v, int) { return valueText (d, v); })
                     .withValueFromStringFunction ([d] (const juce::String& s)
                     {
                         float v = s.retainCharacters ("-+.0123456789").getFloatValue();
                         if (juce::String (d.unit) == "Hz" && s.containsIgnoreCase ("k")) v *= 1000.0f;
                         if (juce::String (d.unit) == "s" && s.containsIgnoreCase ("ms")) v *= 0.001f;
                         return v;
                     });
    return std::make_unique<juce::AudioParameterFloat> (pid, niceName (d), rangeFor (d), d.def, attrs);
}

juce::AudioProcessorValueTreeState::ParameterLayout makeLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    for (const auto& d : svl::paramDefs()) layout.add (makeParam (d));
    return layout;
}
} // namespace

const juce::StringArray& JingFangProcessor::paramIds()
{
    static const juce::StringArray ids = []
    {
        juce::StringArray a;
        for (const auto& d : svl::paramDefs()) a.add (d.id);
        return a;
    }();
    return ids;
}

//==============================================================================
JingFangProcessor::JingFangProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "JINGFANG", makeLayout())
{
    slots.fill (-1);
    for (auto& h : labHeld) h.store (-1.0f);
    const auto& defs = svl::paramDefs();
    for (int i = 0; i < svl::kNumParams; ++i)
    {
        hostParam[(size_t) i] = apvts.getParameter (defs[(size_t) i].id);
        raw[(size_t) i] = apvts.getRawParameterValue (defs[(size_t) i].id);
        jassert (hostParam[(size_t) i] != nullptr && raw[(size_t) i] != nullptr);
        exact[(size_t) i] = defs[(size_t) i].def;
        lastRaw[(size_t) i] = raw[(size_t) i]->load();
        exactShared[(size_t) i].store (exact[(size_t) i]);
    }
    for (auto& row : lit) for (auto& k : row) k.store (0);
    midiRow.fill (-1);
    rescanPresets();
    // start on the Init preset (the engine's defaults if it is missing)
    int init = -1;
    for (size_t i = 0; i < presetList.size(); ++i) if (presetList[i].family == "Init") { init = (int) i; break; }
    if (init >= 0) loadPresetIndex (init);
    else queueDrumKit();
    startTimerHz (30);
}

JingFangProcessor::~JingFangProcessor() { stopTimer(); labCancel.store (true); labPool.removeAllJobs (true, 20000); }

bool JingFangProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo() && l.getMainInputChannelSet().isDisabled();
}

void JingFangProcessor::prepareToPlay (double sampleRate, int)
{
    // as `svl render`: the values first, then prepare (which resets every voice and snaps every smoother)
    applyHostChanges();
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
            if (auto bpm = pos->getBpm()) engine.setTempo (*bpm);   // LFO sync follows the host tempo
    if (hostBpm.load() <= 0) engine.setTempo (userBpm.load());
    svl::ParamSet ps;
    for (int i = 0; i < svl::kNumParams; ++i) ps[i] = exact[(size_t) i];
    engine.setParams (ps, true);
    engine.prepare (sampleRate);
    labSr = sampleRate;
    labTime = 0;
    labTableSent = -2;   // prepare put the engine's tuning table back to 12-equal: the next block sends the Lab's table (or 12-equal) again
    lab.allOff();
    for (auto& h : labHeld) h.store (-1.0f);
    prepared = true;
}

//==============================================================================
void JingFangProcessor::applyHostChanges()
{
    if (kitFlag.load (std::memory_order_acquire))
    {
        const juce::SpinLock::ScopedTryLockType lock (pendingLock);
        if (lock.isLocked())
        {
            for (int t = 0; t < kDrumTracks; ++t)
                if (pendingKitHas[(size_t) t]) engine.setDrumTrackSound (t, pendingKit[(size_t) t], pendingKitGate[(size_t) t]);
                else engine.clearDrumTrackSound (t);
            kitFlag.store (false, std::memory_order_release);
        }
    }
    if (pendingFlag.load (std::memory_order_acquire))
    {
        const juce::SpinLock::ScopedTryLockType lock (pendingLock);
        if (lock.isLocked())
        {
            for (int i = 0; i < svl::kNumParams; ++i)
            {
                exact[(size_t) i] = pendingExact[i];
                lastRaw[(size_t) i] = pendingRaw[(size_t) i];
            }
            pendingFlag.store (false, std::memory_order_release);
            svl::ParamSet ps;
            for (int i = 0; i < svl::kNumParams; ++i) ps[i] = exact[(size_t) i];
            engine.setParams (ps, false);
        }
    }
    for (int i = 0; i < svl::kNumParams; ++i)
    {
        const float r = raw[(size_t) i]->load (std::memory_order_relaxed);
        if (r != lastRaw[(size_t) i])
        {
            lastRaw[(size_t) i] = r;
            exact[(size_t) i] = svl::clampParam (i, r);
            engine.setParam (i, exact[(size_t) i]);
        }
    }
    for (int i = 0; i < svl::kNumParams; ++i) exactShared[(size_t) i].store (exact[(size_t) i], std::memory_order_relaxed);
}

void JingFangProcessor::bumpLit (int row, int key, int d) noexcept
{
    if (key < 0 || key > 127) return;
    auto& a = lit[(size_t) juce::jlimit (0, 2, row)][(size_t) key];
    // both threads bump: a compare-exchange loop, never below 0
    int cur = a.load();
    while (! a.compare_exchange_weak (cur, juce::jmax (0, cur + d))) {}
}

float JingFangProcessor::rowPitch (const svl::ParamSet& ps, int key, int row)
{
    const float steps = row == 0 ? -ps[svl::ROW_DOWN] : (row == 2 ? ps[svl::ROW_UP] : 0.0f);
    return (float) key + steps * 12.0f / ps[svl::TUNE_EDO];
}

void JingFangProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    buffer.clear();
    if (! prepared || buffer.getNumChannels() < 2) return;
    applyHostChanges();

    // the arpeggiator / sequencer clock: the host's grid while it plays, its own clock otherwise (no playhead = standalone)
    {
        bool has = false, playing = false; double ppq = 0, bpmNow = -1;
        if (auto* ph = getPlayHead())
            if (auto pos = ph->getPosition())
            {
                has = true;
                playing = pos->getIsPlaying();
                if (auto q = pos->getPpqPosition()) ppq = *q; else has = false;
                if (auto bpm = pos->getBpm()) bpmNow = *bpm;
            }
        // in a host the DAW is the boss (owner 20 Oct: "daw çalarken eklenti içinde play olmamalı"): a free SEQ / DRUM run (the
        // panel's PLAY) is only a preview while the host is stopped; when the host plays it goes back to HOLD, so the DAW's notes
        // drive the units on its grid and nothing runs on its own on top of the DAW. The host is told by the timer.
        if (has && playing && ! isStandalone())
            for (int i : { (int) svl::SEQ_MODE, (int) svl::DRUMS_MODE })
                if (exact[(size_t) i] > 0.5f)
                {
                    exact[(size_t) i] = 0.0f;
                    engine.setParam (i, 0.0f);
                    exactShared[(size_t) i].store (0.0f);
                    modeToHost.store (true);
                }
        // the tempo: the host's when it has one, else the panel's TEMPO (owner 18 Oct: "bpm tempo nereden geliyor belli değil")
        hostBpm.store ((float) bpmNow, std::memory_order_relaxed);
        engine.setTempo (bpmNow > 0 ? bpmNow : (double) userBpm.load (std::memory_order_relaxed));
        engine.setTransport (has, playing, ppq);
        hostHas.store (has && ! isStandalone(), std::memory_order_relaxed);
        hostPlay.store (has && playing && ! isStandalone(), std::memory_order_relaxed);
        hostNowPlaying = has && playing;
        // PLAY pressed in a stopped host: preview (RU-8 rule 2); the engine hands over to the host's start and stop by itself
        const int serial = previewSerial.load (std::memory_order_acquire);
        if (serial != previewSent)
        {
            previewSent = serial;
            engine.setPreview (wantSeqPreview.load(), wantDrumPreview.load());
        }
    }

    // Tuning Lab: switching it on or off releases what sounds, so no note keeps a pitch from the other path
    const bool labNow = labActive();
    if (labNow != labWasActive)
    {
        engine.allNotesOff (0);
        lab.allOff();
        for (auto& h : labHeld) h.store (-1.0f);
        labWasActive = labNow;
    }
    // the Lab's (morphed, rising) table is the engine's tuning table while the Lab is on, so the arp and the sequencer resolve
    // their keys through it (a step's +1 = the next key = the next step of the tuning); off: the usual 12-equal table
    if (labNow && lab.tableVersion() != labTableSent)
    {
        engine.setTuning (lab.risingTable().data());
        labTableSent = lab.tableVersion();
    }
    else if (! labNow && labTableSent != -1)
    {
        std::array<float, 128> id {};
        for (int k = 0; k < 128; ++k) id[(size_t) k] = (float) k;
        engine.setTuning (id.data());
        labTableSent = -1;
    }
    lab.settings.directional = labDirOnA.load();
    lab.settings.adaptive = labAdaptA.load();
    lab.settings.adaptiveWindowCents = labWindowA.load();
    lab.settings.homePull = labPullA.load();

    // the panel's keyboard: at the start of the block
    {
        const auto scope = uiFifo.read (uiFifo.getNumReady());
        auto take = [&] (int start, int size)
        {
            for (int k = start; k < start + size; ++k)
            {
                const UiNote& u = uiNotes[(size_t) k];
                const int id = u.row < 0 ? u.key : 2000 + u.row * 128 + u.key;
                if (u.key < 0) { engine.allNotesOff (0); continue; }
                if (u.on)
                {
                    if (u.row < 0 && labNow) labNoteOn (u.key, u.vel, 0, labTime);
                    else if (u.row < 0) engine.noteOn (u.key, u.vel, 0);
                    else engine.noteOnPitch (rowPitch (engine.getParams(), u.key, u.row), u.vel, id, 0);
                }
                else
                {
                    engine.noteOff (id, 0);
                    if (u.row < 0) { lab.noteOff (u.key); labHeld[(size_t) u.key].store (-1.0f); }
                }
            }
        };
        take (scope.startIndex1, scope.blockSize1);
        take (scope.startIndex2, scope.blockSize2);
    }

    float* L = buffer.getWritePointer (0);
    float* R = buffer.getWritePointer (1);
    int done = 0;
    const bool micro = exact[svl::TUNE_MODE] > 0.5f;
    for (const auto meta : midi)
    {
        const auto m = meta.getMessage();
        const int pos = juce::jlimit (0, n, meta.samplePosition);
        const int ch = m.getChannel();
        const bool member = mpe.isMember (ch);
        if (m.isController() && (m.getControllerNumber() == 101 || m.getControllerNumber() == 100 || m.getControllerNumber() == 6 || m.getControllerNumber() == 38)
            && [&] { bool zones = false; const bool used = mpe.controller (ch, m.getControllerNumber(), m.getControllerValue(), zones);
                     if (zones) for (int c = 1; c <= 16; ++c) engine.resetMember (c);
                     return used; }())
        {}   // RPN: MPE zone configuration and the bend range
        else if (member && m.isController() && m.getControllerNumber() == 74)
        {
            if (pos > done) { engine.process (L + done, R + done, pos - done); done = pos; }
            engine.setMemberSlide (ch, (float) m.getControllerValue() / 127.0f);
        }
        else if (member && (m.isChannelPressure() || m.isAftertouch()))
        {
            if (pos > done) { engine.process (L + done, R + done, pos - done); done = pos; }
            engine.setMemberPressure (ch, (float) (m.isChannelPressure() ? m.getChannelPressureValue() : m.getAfterTouchValue()) / 127.0f);
        }
        else if (member && m.isPitchWheel())
        {
            if (pos > done) { engine.process (L + done, R + done, pos - done); done = pos; }
            engine.setMemberBend (ch, (float) (m.getPitchWheelValue() - 8192) / 8192.0f * mpe.bendRange (ch));
        }
        else if (m.isNoteOn())
        {
            const int key = m.getNoteNumber();
            if (hostNowPlaying)
                for (int u = 0; u < 2; ++u)
                    if (armed[(size_t) u].exchange (false))
                    {   // a dragged clip is playing JING FANG: that unit plays the DAW's notes now (RU-8 rule 3)
                        const int i = u == 0 ? svl::SEQ_SOURCE : svl::DRUMS_SOURCE;
                        if (pos > done) { engine.process (L + done, R + done, pos - done); done = pos; }
                        exact[(size_t) i] = 1.0f;
                        engine.setParam (i, 1.0f);
                        exactShared[(size_t) i].store (1.0f);
                        sourceToHost.store (true);
                    }
            if (labNow) labNoteOn (key, (float) m.getVelocity() / 127.0f, pos - done, labTime + pos / labSr);
            else if (member) engine.noteOnMpe (key, (float) m.getVelocity() / 127.0f, ch, pos - done);
            else engine.noteOn (key, (float) m.getVelocity() / 127.0f, pos - done);   // as `svl render`: velocity / 127
            const int ksLow = (int) exact[svl::KSW_LOW];
            const bool keyswitch = micro && exact[svl::KSW_MODE] > 0.5f && key >= ksLow && key < ksLow + 24;   // never sounds
            const int row = keyswitch ? -1 : (micro ? juce::roundToInt (exact[(size_t) (svl::TUNE_ROW0 + key % 12)]) : 1);
            if (row >= 0 && midiRow[(size_t) key] >= 0) bumpLit (midiRow[(size_t) key], key, -1);   // the same key again
            midiRow[(size_t) key] = row;
            if (row >= 0) bumpLit (row, key, 1);
        }
        else if (m.isNoteOff())
        {
            const int key = m.getNoteNumber();
            if (member) engine.noteOffMpe (key, ch, pos - done); else engine.noteOff (key, pos - done);
            if (labNow) { lab.noteOff (key); labHeld[(size_t) key].store (-1.0f); }
            if (midiRow[(size_t) key] >= 0) bumpLit (midiRow[(size_t) key], key, -1);
            midiRow[(size_t) key] = -1;
        }
        else if (m.isAllNotesOff() || m.isAllSoundOff())
        {
            engine.allNotesOff (pos - done);
            lab.allOff();
            for (auto& h : labHeld) h.store (-1.0f);
            for (auto& row : lit) for (auto& k : row) k.store (0);
            midiRow.fill (-1);
        }
        else if (m.isController() && m.getControllerNumber() == 1)
        {   // mod wheel: a block-level value, like the bend
            if (pos > done) { engine.process (L + done, R + done, pos - done); done = pos; }
            engine.setModWheel ((float) m.getControllerValue() / 127.0f);
        }
        else if (m.isChannelPressure() || m.isAftertouch())
        {
            if (pos > done) { engine.process (L + done, R + done, pos - done); done = pos; }
            engine.setAftertouch ((float) (m.isChannelPressure() ? m.getChannelPressureValue() : m.getAfterTouchValue()) / 127.0f);
        }
        else if (m.isPitchWheel())
        {
            // the bend is a block-level value in the engine: render up to the event, then bend (the engine's output does not depend
            // on where a block is cut)
            if (pos > done) { engine.process (L + done, R + done, pos - done); done = pos; }
            engine.setPitchBend ((float) (m.getPitchWheelValue() - 8192) / 8192.0f);
        }
    }
    if (n > done) engine.process (L + done, R + done, n - done);
    labTime += n / labSr;
    liveGr.store (engine.fxReductionDb(), std::memory_order_relaxed);
    liveSeqStep.store (engine.noteGen().currentStep(), std::memory_order_relaxed);
    updateSounding();
    liveDrumStep.store (engine.drumStep(), std::memory_order_relaxed);
    if (labNow) { labDriftA.store (lab.driftCents(), std::memory_order_relaxed); labDirA2.store (lab.direction(), std::memory_order_relaxed); }

    // live view: the loudest sounding voice and the output, for the screens
    {
        float lvl = 0.0f, pitch = livePitch.load (std::memory_order_relaxed); int stage = 0, count = 0;
        for (int v = 0; v < svl::kVoicePool; ++v)
        {
            const auto vi = engine.voiceInfo (v);
            if (! vi.active) continue;
            ++count;
            if (vi.ampLevel > lvl) { lvl = vi.ampLevel; pitch = vi.pitch; stage = vi.ampStage; }
        }
        liveLevel.store (lvl, std::memory_order_relaxed); livePitch.store (pitch, std::memory_order_relaxed);
        liveStage.store (stage, std::memory_order_relaxed); liveVoices.store (count, std::memory_order_relaxed);
        int w = scopeWrite.load (std::memory_order_relaxed);
        for (int i = 0; i < n; ++i) { scopeBuf[(size_t) w].store (0.5f * (L[i] + R[i]), std::memory_order_relaxed); w = (w + 1) % kScopeLen; }
        scopeWrite.store (w, std::memory_order_release);
    }

    if (engine.tuningChangedByKeyswitch())
    {
        const auto& ep = engine.getParams();
        for (int pc = 0; pc < 12; ++pc)
        {
            const int i = svl::TUNE_ROW0 + pc;
            exact[(size_t) i] = ep[i];
            exactShared[(size_t) i].store (ep[i]);
        }
        rowsDirty.store (true);
    }
    for (int c = 2; c < buffer.getNumChannels(); ++c) buffer.clear (c, 0, n);
}

//==============================================================================
void JingFangProcessor::timerCallback()
{
    if (sourceToHost.exchange (false))                       // the audio thread switched a unit to MIDI: tell the host
        for (int i : { (int) svl::SEQ_SOURCE, (int) svl::DRUMS_SOURCE })
            if (juce::roundToInt (raw[(size_t) i]->load()) != juce::roundToInt (exactShared[(size_t) i].load())) setHostValue (i, exactShared[(size_t) i].load());
    if (modeToHost.exchange (false))                         // the host started: a free run went back to HOLD
        for (int i : { (int) svl::SEQ_MODE, (int) svl::DRUMS_MODE })
            if (juce::roundToInt (raw[(size_t) i]->load()) != juce::roundToInt (exactShared[(size_t) i].load())) setHostValue (i, exactShared[(size_t) i].load());
    if (rowsDirty.exchange (false))
        for (int pc = 0; pc < 12; ++pc)
        {
            const int i = svl::TUNE_ROW0 + pc;
            const float v = exactShared[(size_t) i].load();
            if (juce::roundToInt (raw[(size_t) i]->load()) != juce::roundToInt (v)) setHostValue (i, v);
        }
    genFollowKnobs();
}

void JingFangProcessor::setHostValue (int i, float plain)
{
    auto* p = hostParam[(size_t) i];
    p->setValueNotifyingHost (p->convertTo0to1 (plain));
}

void JingFangProcessor::uiNoteOn (int key, int row, float velocity)
{
    if (key < 0 || key > 127) return;
    if (uiFifo.getFreeSpace() < 16) return;                    // keep room for the note-offs (the audio may not be running)
    const bool micro = exactShared[svl::TUNE_MODE].load() > 0.5f;
    const int litRow = row >= 0 ? row : (micro ? juce::roundToInt (exactShared[(size_t) (svl::TUNE_ROW0 + key % 12)].load()) : 1);
    uiLitRow[(size_t) key] = litRow;
    bumpLit (litRow, key, 1);
    const auto w = uiFifo.write (1);
    if (w.blockSize1 > 0) uiNotes[(size_t) w.startIndex1] = { true, key, row, velocity };
}

void JingFangProcessor::uiNoteOff (int key, int row)
{
    if (key < 0 || key > 127) return;
    bumpLit (row >= 0 ? row : uiLitRow[(size_t) key], key, -1);
    const auto w = uiFifo.write (1);
    if (w.blockSize1 > 0) uiNotes[(size_t) w.startIndex1] = { false, key, row, 0.0f };
}

void JingFangProcessor::uiAllNotesOff()
{
    for (auto& row : lit) for (auto& k : row) k.store (0);
    const auto w = uiFifo.write (1);
    if (w.blockSize1 > 0) uiNotes[(size_t) w.startIndex1] = { false, -1, -1, 0.0f };
}

//==============================================================================
// presets
juce::File& JingFangProcessor::userFolderOverrideForTests() { static juce::File f; return f; }

juce::File JingFangProcessor::userPresetFolder()
{
    if (userFolderOverrideForTests() != juce::File()) return userFolderOverrideForTests();
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("SubverseLab").getChildFile ("JING FANG").getChildFile ("Presets");
}

// the preset list's categories (owner 20 Oct: a clear structure in a sensible order). A category groups families; the family
// stays the preset's identity ("Family/Name" in saved projects, slots and drum kits), so no saved reference changes.
namespace
{
struct CategoryDef { const char* name; std::vector<const char*> families; };
const std::vector<CategoryDef>& categoryDefs()
{
    static const std::vector<CategoryDef> defs {
        { "Init", { "Init" } },
        { "Keys", { "Piano", "Keys - E-Piano", "Organ", "Mallet", "Bell" } },
        { "Pads", { "Pad", "Strings", "Choir" } },
        { "Leads", { "Lead", "Brass", "Winds" } },
        { "Bass", { "Bass", "Bass - 808" } },
        { "Plucks", { "Pluck", "Guitar" } },
        { "Arps", { "Arp - Classic", "Arp - Chord", "Arp - Rhythmic", "Arp - Wide" } },
        { "Sequences", { "Sequence - Melodic", "Sequence - Bass", "Sequence - Acid", "Sequence - Pulse", "Sequence - Micro", "Sequence - Drums" } },
        { "Maqam", { "Kanun", "Zurna", "Sitar", "Bansuri", "Guzheng", "Erhu", "Dizi", "Sequence - Lab" } },
        { "Drums", { "Drum Machine", "Drums - Kick", "Drums - Snare", "Drums - Clap", "Drums - Hat", "Drums - Perc" } },
        { "FX", { "FX" } },
    };
    return defs;
}
// category rank * 100 + the family's place in it; a family no category lists (the user's own) sorts after them, by name
int familyRank (const juce::String& family)
{
    const auto& defs = categoryDefs();
    for (size_t c = 0; c < defs.size(); ++c)
    {
        int f = 0;
        for (auto* name : defs[c].families) { if (family == name) return (int) c * 100 + f; ++f; }
    }
    return 100000;
}
} // namespace

void JingFangProcessor::rescanPresets()
{
    presetList.clear();
    hiddenList.clear();
    const auto hidden = hiddenPresetNames();
    for (int i = 0; i < FactoryPresets::namedResourceListSize; ++i)
    {
        int size = 0;
        const char* data = FactoryPresets::getNamedResource (FactoryPresets::namedResourceList[i], size);
        if (data == nullptr) continue;
        const juce::String xml = juce::String::fromUTF8 (data, size);
        svl::Preset p;
        if (! svl::presetFromXml (xml.toStdString(), p)) continue;
        PresetEntry e { juce::String (p.meta.family), juce::String (p.meta.name), xml, {}, true };
        // a factory preset the user deleted stays out of every list (owner 18 Oct: "hoşlanmadığı sesler varsayılan bile olsa
        // yeniden karşılaşmaz"); it is still found as a drum kit's sound
        if (hidden.contains (e.family + "/" + e.name)) hiddenList.push_back (e);
        else presetList.push_back (e);
    }
    for (const auto& f : userPresetFolder().findChildFiles (juce::File::findFiles, true, "*.xml"))
    {
        const juce::String xml = f.loadFileAsString();
        svl::Preset p;
        if (! svl::presetFromXml (xml.toStdString(), p)) continue;
        presetList.push_back ({ p.meta.family.empty() ? f.getParentDirectory().getFileName() : juce::String (p.meta.family),
                                p.meta.name.empty() ? f.getFileNameWithoutExtension() : juce::String (p.meta.name), xml, f, false });
    }
    std::stable_sort (presetList.begin(), presetList.end(), [] (const PresetEntry& a, const PresetEntry& b)
    {
        const int ra = familyRank (a.family), rb = familyRank (b.family);
        if (ra != rb) return ra < rb;                     // Init, Keys, Pads, Leads, Bass, Plucks, Arps, Sequences, Maqam, Drums, FX, yours
        if (a.family != b.family) return a.family.compareNatural (b.family) < 0;
        if (a.factory != b.factory) return ! a.factory;   // yours first: a name you saved over a factory one finds yours
        return a.name.compareNatural (b.name) < 0;
    });
    currentIndex = -1;
    for (size_t i = 0; i < presetList.size(); ++i)
        if (presetList[i].family == juce::String (loadedMeta.family) && presetList[i].name == juce::String (loadedMeta.name)) { currentIndex = (int) i; break; }
}


juce::String JingFangProcessor::categoryOf (const juce::String& family)
{
    const int r = familyRank (family);
    return r >= 100000 ? juce::String ("Yours") : juce::String (categoryDefs()[(size_t) (r / 100)].name);
}

juce::StringArray JingFangProcessor::categories() const
{
    juce::StringArray c;
    for (const auto& p : presetList) c.addIfNotAlreadyThere (categoryOf (p.family));
    return c;
}

juce::StringArray JingFangProcessor::families() const
{
    juce::StringArray f;
    for (const auto& p : presetList) f.addIfNotAlreadyThere (p.family);
    return f;
}

void JingFangProcessor::pushPreset (const svl::ParamSet& valuesIn, const svl::PresetMeta& meta, const svl::ParamSet& baseIn, int index)
{
    // daw.seq / daw.drums belong to the project, not to a sound: a preset never changes them
    svl::ParamSet values = valuesIn, base = baseIn;
    for (int i : { (int) svl::SEQ_SOURCE, (int) svl::DRUMS_SOURCE }) values[i] = base[i] = exactShared[(size_t) i].load();
    // knobs glide to what the product writes now; on the message thread their callbacks run inside this call, so the window
    // closes again at the end (a host move right after a load must not glide); from another thread they arrive later
    const bool messageThread = juce::MessageManager::existsAndIsCurrentThread();
    armGlide();
    loadedMeta = meta;
    loadedBase = base;
    currentIndex = index;
    for (int i = 0; i < svl::kNumParams; ++i) setHostValue (i, values[i]);
    {
        const juce::SpinLock::ScopedLockType lock (pendingLock);
        for (int i = 0; i < svl::kNumParams; ++i)
        {
            pendingExact[i] = svl::clampParam (i, values[i]);
            pendingRaw[(size_t) i] = raw[(size_t) i]->load();
            exactShared[(size_t) i].store (pendingExact[i]);
        }
        pendingFlag.store (true, std::memory_order_release);
    }
    if (messageThread) glideUntil = 0;
    updateHostDisplay (ChangeDetails().withProgramChanged (true));
}

bool JingFangProcessor::loadPresetXml (const juce::String& xml, int index)
{
    svl::Preset p;
    if (! svl::presetFromXml (xml.toStdString(), p)) return false;
    readDrumKit (p);                                           // a preset without a kit plays the default kit
    pushPreset (p.params, p.meta, p.params, index);
    genReadXml (xml);                                          // the seeds it was rolled with (shown); nothing is live until a roll
    return true;
}

bool JingFangProcessor::loadPresetIndex (int index)
{
    if (index < 0 || index >= (int) presetList.size()) return false;
    if (! loadPresetXml (presetList[(size_t) index].xml, index)) return false;
    slots.fill (index);                                        // a whole preset: every panel comes from it
    readSlots (presetList[(size_t) index].xml);                // a saved mix: each panel's own source
    for (auto& sl : slots) if (sl < 0) sl = index;
    applyPresetLab (presetList[(size_t) index].xml);
    return true;
}

void JingFangProcessor::applyPresetLab (const juce::String& xml)
{
    const auto els = svl::detail::elements (xml.toStdString(), "labtuning");
    if (! els.empty())
    {
        jflab::Tuning t;
        if (! jflab::builtin::find (svl::detail::attr (els.front(), "a"), t)) return;
        auto st = labMsg;
        st.a = t; st.on = true; st.morph = 0; st.adaptive = false;
        setLabState (st);
        labByPreset = true;
        labPresetId = juce::String (svl::detail::attr (els.front(), "a"));
        return;
    }
    if (labByPreset)
    {   // the previous preset switched the Lab on; this one is an ordinary sound
        auto st = labMsg;
        st.on = false;
        setLabState (st);
        labByPreset = false;
    }
}

void JingFangProcessor::copyScope (float* dest) const noexcept
{
    const int w = scopeWrite.load (std::memory_order_acquire);
    for (int i = 0; i < kScopeLen; ++i) dest[i] = scopeBuf[(size_t) ((w + i) % kScopeLen)].load (std::memory_order_relaxed);
}

//==============================================================================
static const char* const kSectionNames[JingFangProcessor::kNumSections] = {
    "OSC 1", "OSC 2", "OSC 3", "FILTER", "AMP ENV", "FILTER ENV", "PITCH CLICK DRUM", "DRIFT FLOOR", "VOICE OUTPUT", "FM", "TUNING", "FX", "SEQ", "DRUMS" };
static const char* const kSectionKeys[JingFangProcessor::kNumSections] = {
    "osc1", "osc2", "osc3", "filter", "amp_env", "filter_env", "pitch", "drift", "voice", "fm", "tuning", "fx", "seq", "drums" };

const char* JingFangProcessor::sectionName (int s) { return s >= 0 && s < kNumSections ? kSectionNames[s] : ""; }

int JingFangProcessor::sectionOf (const std::string& id)
{
    auto starts = [&id] (const char* p) { return id.rfind (p, 0) == 0; };
    if (starts ("osc1.")) return 0;
    if (starts ("osc2.")) return 1;
    if (starts ("osc3.")) return 2;
    if (starts ("filter.")) return 3;
    if (starts ("amp_env.")) return 4;
    if (starts ("filter_env.")) return 5;
    if (starts ("pitch_env.") || starts ("click.") || starts ("drum.") || id == "voice.choke") return 6;
    if (starts ("drift.") || id == "noise.floor") return 7;
    if (starts ("voice.") || starts ("master.")) return 8;
    if (starts ("fm.")) return 9;
    if (starts ("tuning.")) return 10;
    if (starts ("fx.")) return 11;
    if (starts ("gen.drum_")) return 13;                       // GENERATE's settings travel with the unit they write
    if (starts ("arp.") || starts ("seq.") || starts ("gen.")) return 12;
    if (starts ("drums.")) return 13;
    return -1;
}

bool JingFangProcessor::loadSection (int section, int presetIndex)
{
    if (section < 0 || section >= kNumSections || presetIndex < 0 || presetIndex >= (int) presetList.size()) return false;
    svl::Preset p;
    if (! svl::presetFromXml (presetList[(size_t) presetIndex].xml.toStdString(), p)) return false;
    svl::ParamSet values, base = loadedBase;
    for (int i = 0; i < svl::kNumParams; ++i) values[i] = exactShared[(size_t) i].load();
    for (int i = 0; i < svl::kNumParams; ++i)
        if (sectionOf (svl::paramDefs()[(size_t) i].id) == section) { values[i] = p.params[i]; base[i] = p.params[i]; }
    if (section == 13)
    {   // DRUMS: the kit comes with the drum machine's values
        readDrumKit (p);
    }
    slots[(size_t) section] = presetIndex;
    bool allSame = true;
    for (auto s : slots) allSame = allSame && s == presetIndex;
    svl::PresetMeta meta = loadedMeta;
    if (! allSame) { meta.name = "Mix"; meta.family = "Preset synth"; }
    pushPreset (values, meta, base, allSame ? presetIndex : -1);
    return true;
}

void JingFangProcessor::stepSection (int section, int delta)
{
    if (presetList.empty() || section < 0 || section >= kNumSections) return;
    const int count = (int) presetList.size();
    const int from = slots[(size_t) section] < 0 ? (currentIndex < 0 ? 0 : currentIndex) : slots[(size_t) section];
    loadSection (section, ((from + delta) % count + count) % count);
}

juce::String JingFangProcessor::slotsXmlLine() const
{
    juce::String l = "  <slots";
    for (int s = 0; s < kNumSections; ++s)
    {
        const int i = slots[(size_t) s];
        const auto ref = i >= 0 && i < (int) presetList.size() ? presetList[(size_t) i].family + "/" + presetList[(size_t) i].name : juce::String();
        l << " " << kSectionKeys[s] << "=\"" << ref.replace ("&", "&amp;").replace ("\"", "&quot;") << "\"";
    }
    return l + "/>\n";
}

void JingFangProcessor::readSlots (const juce::String& xml)
{
    const auto els = svl::detail::elements (xml.toStdString(), "slots");
    if (els.empty()) return;
    for (int s = 0; s < kNumSections; ++s)
    {
        const juce::String ref (svl::detail::attr (els.front(), kSectionKeys[s]));
        slots[(size_t) s] = -1;
        for (size_t i = 0; i < presetList.size(); ++i)
            if (presetList[i].family + "/" + presetList[i].name == ref) { slots[(size_t) s] = (int) i; break; }
    }
}

void JingFangProcessor::stepPreset (int delta)
{
    if (presetList.empty()) return;
    const int count = (int) presetList.size();
    const int from = currentIndex < 0 ? (delta > 0 ? -1 : 0) : currentIndex;
    loadPresetIndex (((from + delta) % count + count) % count);
}

bool JingFangProcessor::isModified() const
{
    for (int i = 0; i < svl::kNumParams; ++i)
        if (i != svl::SEQ_SOURCE && i != svl::DRUMS_SOURCE
            && std::abs (exactShared[(size_t) i].load() - loadedBase[i]) > 1.0e-4f * (1.0f + std::abs (loadedBase[i]))) return true;
    return false;
}

float JingFangProcessor::loadedValue (const juce::String& paramId) const
{
    const int i = svl::findParam (paramId.toStdString());
    return i >= 0 ? loadedBase[i] : 0.0f;
}

juce::String JingFangProcessor::stateXml() const
{
    svl::Preset p;
    p.meta = loadedMeta;
    for (int i = 0; i < svl::kNumParams; ++i) p.params[i] = exactShared[(size_t) i].load();
    for (int t = 0; t < kDrumTracks; ++t) p.drumKit[(size_t) t] = drumKit[(size_t) t].toStdString();
    juce::String xml (svl::presetToXml (p));
    // the loaded preset's own values (the double-click targets); `svl render` ignores these lines
    juce::String base;
    for (int i = 0; i < svl::kNumParams; ++i)
        base << "  <base id=\"" << svl::paramDefs()[(size_t) i].id << "\" value=\"" << svl::valueToString (i, loadedBase[i]) << "\"/>\n";
    return xml.replace ("</svlpreset>", base + slotsXmlLine() + genXmlLine() + "  <tempo bpm=\"" + juce::String (userBpm.load(), 2) + "\"/>\n" + labStateXml (labMsg)
                       + (labByPreset ? "  <labtuning a=\"" + labPresetId + "\" restore=\"1\"/>\n" : juce::String()) + "</svlpreset>");
}

juce::File JingFangProcessor::saveUserPreset (const juce::String& family, const juce::String& name)
{
    const auto safe = [] (juce::String s) { s = juce::File::createLegalFileName (s.trim()); return s.isEmpty() ? juce::String ("Untitled") : s; };
    svl::Preset p;
    p.meta.name = safe (name).toStdString(); p.meta.family = safe (family).toStdString(); p.meta.author = "SubverseLab";
    for (int i = 0; i < svl::kNumParams; ++i) p.params[i] = exactShared[(size_t) i].load();
    for (int t = 0; t < kDrumTracks; ++t) p.drumKit[(size_t) t] = drumKit[(size_t) t].toStdString();
    const auto folder = userPresetFolder().getChildFile (safe (family));
    folder.createDirectory();
    const auto file = folder.getChildFile (safe (name) + ".xml");
    // the preset file carries where each panel came from (svl render ignores the line)
    const juce::String labLine = labByPreset && labMsg.on ? "  <labtuning a=\"" + labPresetId + "\"/>\n" : juce::String();
    if (! file.replaceWithText (juce::String (svl::presetToXml (p)).replace ("</svlpreset>", slotsXmlLine() + genXmlLine() + labLine + "</svlpreset>"))) return {};
    loadedMeta = p.meta;
    loadedBase = p.params;
    const auto slotRefs = slotsXmlLine();                     // the slots by name: the rescan re-sorts the list
    rescanPresets();
    readSlots ("<svlpreset>" + slotRefs + "</svlpreset>");
    for (auto& sl : slots) if (sl < 0) sl = currentIndex;
    return file;
}

void JingFangProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    const auto xml = stateXml();
    dest.replaceAll (xml.toRawUTF8(), xml.getNumBytesAsUTF8());
}

void JingFangProcessor::setStateInformation (const void* data, int size)
{
    const juce::String xml = juce::String::fromUTF8 (static_cast<const char*> (data), size);
    if (auto* mm = juce::MessageManager::getInstanceWithoutCreating(); mm != nullptr && ! mm->isThisTheMessageThread())
    {   // some hosts restore from a worker thread: the preset list, the kit and the Lab belong to the message thread
        std::weak_ptr<int> alive = labAlive;
        auto done = std::make_shared<juce::WaitableEvent>();
        juce::MessageManager::callAsync ([this, xml, alive, done] { if (! alive.expired()) applyStateXml (xml); done->signal(); });
        done->wait (400);   // a host that reads the values back right after (pluginval's state restoration, 20 Oct AU run) sees them; a busy
                            // message thread cannot deadlock us: after 400 ms we return and the restore still happens
        return;
    }
    applyStateXml (xml);
}

void JingFangProcessor::applyStateXml (const juce::String& xml)
{
    svl::Preset p;
    if (! svl::presetFromXml (xml.toStdString(), p)) return;
    svl::ParamSet base = p.params;
    for (auto& e : svl::detail::elements (xml.toStdString(), "base"))
    {
        const int i = svl::findParam (svl::detail::attr (e, "id"));
        float v = 0.0f;
        if (i >= 0 && svl::stringToValue (i, svl::detail::attr (e, "value"), v)) base[i] = v;
    }
    int index = -1;
    for (size_t i = 0; i < presetList.size(); ++i)
        if (presetList[i].family == juce::String (p.meta.family) && presetList[i].name == juce::String (p.meta.name)) { index = (int) i; break; }
    if (isStandalone())
    {   // PLAY (a sequence / beat on its own clock) is a "now" state: the Standalone opens waiting for a key (owner 18 Oct: the
        // saved state brought a free sequence back, and it played the same note from the moment the app opened). In a host a
        // free unit already waits for the host's transport.
        for (int i : { (int) svl::SEQ_MODE, (int) svl::DRUMS_MODE, (int) svl::ARP_LATCH }) { p.params[i] = 0.0f; base[i] = 0.0f; }
    }
    pushPreset (p.params, p.meta, base, index);
    slots.fill (index);
    readSlots (xml);
    readDrumKit (p);
    genReadXml (xml);
    {
        const auto t = svl::detail::elements (xml.toStdString(), "tempo");
        if (! t.empty()) setUserBpm (juce::String (svl::detail::attr (t.front(), "bpm")).getFloatValue());
    }
    LabState ls;
    labByPreset = false;
    if (labStateFromXml (xml, ls))
    {
        setLabState (ls);
        const auto lt = svl::detail::elements (xml.toStdString(), "labtuning");
        if (! lt.empty()) { labByPreset = true; labPresetId = juce::String (svl::detail::attr (lt.front(), "a")); }
    }
}

//==============================================================================
// deleting presets: the user's own file goes to the bin; a factory preset is hidden (listed in hidden.txt in the user folder)
juce::StringArray JingFangProcessor::hiddenPresetNames()
{
    juce::StringArray a;
    a.addLines (userPresetFolder().getChildFile ("hidden.txt").loadFileAsString());
    a.removeEmptyStrings();
    return a;
}

bool JingFangProcessor::deletePreset (int index)
{
    if (index < 0 || index >= (int) presetList.size()) return false;
    const auto e = presetList[(size_t) index];
    if (e.factory)
    {
        auto names = hiddenPresetNames();
        names.addIfNotAlreadyThere (e.family + "/" + e.name);
        const auto f = userPresetFolder().getChildFile ("hidden.txt");
        f.getParentDirectory().createDirectory();
        if (! f.replaceWithText (names.joinIntoString ("\n") + "\n")) return false;
    }
    else if (! e.file.moveToTrash()) return false;
    const bool wasCurrent = index == currentIndex;
    const auto slotRefs = slotsXmlLine();                     // slots by name: the rescan shifts indices
    rescanPresets();
    readSlots ("<svlpreset>" + slotRefs + "</svlpreset>");
    if (wasCurrent && ! presetList.empty()) loadPresetIndex (juce::jmin (index, (int) presetList.size() - 1));
    return true;
}

void JingFangProcessor::showHiddenPresets()
{
    userPresetFolder().getChildFile ("hidden.txt").deleteFile();
    const auto slotRefs = slotsXmlLine();
    rescanPresets();
    readSlots ("<svlpreset>" + slotRefs + "</svlpreset>");
    for (auto& sl : slots) if (sl < 0) sl = currentIndex;
}

//==============================================================================
// drum kit
juce::String JingFangProcessor::drumTrackName (int t, bool full) const
{
    if (t < 0 || t >= kDrumTracks) return {};
    const juce::String n = drumKit[(size_t) t].isNotEmpty() ? drumKit[(size_t) t] : juce::String (svl::kDefaultDrumKit[t]);
    return full ? n : n.fromLastOccurrenceOf ("/", false, false);
}

std::vector<int> JingFangProcessor::drumPresetIndices() const
{
    std::vector<int> v;
    for (size_t i = 0; i < presetList.size(); ++i) if (presetList[i].family.startsWith ("Drums")) v.push_back ((int) i);
    return v;
}

void JingFangProcessor::setDrumTrack (int t, int presetIndex)
{
    if (t < 0 || t >= kDrumTracks || presetIndex < 0 || presetIndex >= (int) presetList.size()) return;
    drumKit[(size_t) t] = presetList[(size_t) presetIndex].family + "/" + presetList[(size_t) presetIndex].name;
    queueDrumKit();
}

void JingFangProcessor::queueDrumKit()
{
    // as `svl render` (applyKit): each track's preset by "Family/Name" (empty: the default kit), its values and its gate
    const juce::SpinLock::ScopedLockType lock (pendingLock);
    for (int t = 0; t < kDrumTracks; ++t)
    {
        const auto name = drumTrackName (t, true);
        pendingKitHas[(size_t) t] = false;
        std::vector<const PresetEntry*> all;
        for (auto& pe : presetList) all.push_back (&pe);
        for (auto& pe : hiddenList) all.push_back (&pe);
        for (const auto* pp : all)
            if (const auto& pe = *pp; pe.family + "/" + pe.name == name)
            {
                svl::Preset d;
                if (svl::presetFromXml (pe.xml.toStdString(), d))
                {
                    pendingKit[(size_t) t] = d.params;
                    pendingKitGate[(size_t) t] = svl::drumGateSeconds (d.meta.family);
                    pendingKitHas[(size_t) t] = true;
                }
                break;
            }
    }
    kitFlag.store (true, std::memory_order_release);
}

void JingFangProcessor::readDrumKit (const svl::Preset& p)
{
    for (int t = 0; t < kDrumTracks; ++t) drumKit[(size_t) t] = juce::String (p.drumKit[(size_t) t]);
    queueDrumKit();
}

void JingFangProcessor::updateSounding() noexcept
{
    // every gated voice -> the key (and row) it sounds on: Lab on = the Lab table's nearest key; micro = the row whose offset
    // matches; otherwise the nearest key. Voice pitches carry the A4 reference offset, taken off first.
    std::array<std::array<bool, 128>, 3> now {};
    const float ref = 12.0f * std::log2 (exact[svl::TUNE_REF] / 440.0f);
    const bool labOn = labActive();
    const float stepSt = 12.0f / juce::jmax (1.0f, exact[svl::TUNE_EDO]);
    const float up = exact[svl::ROW_UP] * stepSt, down = exact[svl::ROW_DOWN] * stepSt;
    const auto& table = lab.risingTable();
    for (int v = 0; v < svl::kVoicePool; ++v)
    {
        const auto vi = engine.voiceInfo (v);
        if (! vi.active || ! vi.gate) continue;
        const float p = vi.pitch - ref;
        int row = 1, key = juce::roundToInt (p);
        if (labOn)
        {
            float best = 1.0e9f;
            for (int k = 0; k < 128; ++k) { const float d = std::abs (table[(size_t) k] - p); if (d < best) { best = d; key = k; } }
        }
        else if (std::abs (p - (float) key) > 0.01f)   // a row note (micro rows, or the panel's outer rows in normal mode)
        {
            const int ku = juce::roundToInt (p - up), kd = juce::roundToInt (p + down);
            if (std::abs (p - up - (float) ku) < 0.01f) { row = 2; key = ku; }
            else if (std::abs (p + down - (float) kd) < 0.01f) { row = 0; key = kd; }
        }
        if (key >= 0 && key < 128) now[(size_t) row][(size_t) key] = true;
    }
    const double t = juce::Time::getMillisecondCounterHiRes();
    for (int k = 0; k < 128; ++k)
    {
        bool was = false, is = false;
        for (int r = 0; r < 3; ++r) { was |= sounding[(size_t) r][(size_t) k].load (std::memory_order_relaxed); is |= now[(size_t) r][(size_t) k]; }
        if (is && ! was) soundOnMs[(size_t) k].store (t, std::memory_order_relaxed);
        if (was && ! is) soundOffMs[(size_t) k].store (t, std::memory_order_relaxed);
    }
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 128; ++k) sounding[(size_t) r][(size_t) k].store (now[(size_t) r][(size_t) k], std::memory_order_relaxed);
}

//==============================================================================
// Tuning Lab
void JingFangProcessor::labNoteOn (int key, float vel, int offset, double timeSec) noexcept
{
    // the Lab decides the pitch; when it is the table's own pitch (no falling value, no adaptive move) the note takes the key
    // path, so the arp / sequencer and the TUNING rows treat it like any key; otherwise it plays its pitch, the rows added
    const bool micro = exact[svl::TUNE_MODE] > 0.5f;
    const int ksLow = (int) exact[svl::KSW_LOW];
    const bool keyswitch = micro && exact[svl::KSW_MODE] > 0.5f && key >= ksLow && key < ksLow + 24;
    const bool drumKey = exact[svl::DRUMS_ON] > 0.5f && exact[svl::DRUMS_KEYS] > 0.5f && key >= 36 && key <= 43;
    if (keyswitch || drumKey) { engine.noteOn (key, vel, offset); return; }   // the engine's own handling (never sounds as a note)
    const float p = lab.noteOn (key, timeSec);
    const float tab = lab.risingTable()[(size_t) juce::jlimit (0, 127, key)];
    const float rows = engine.keyPitch (key) - tab;                   // the TUNING rows on top of the table (0 in normal mode)
    labHeld[(size_t) juce::jlimit (0, 127, key)].store (p + rows);
    if (std::abs (p - tab) < 1.0e-6f) engine.noteOn (key, vel, offset);
    else engine.noteOnPitch (p + rows, vel, key, offset);
}

void JingFangProcessor::setLabState (const LabState& st)
{
    labMsg = st;
    lab.a = st.a;
    lab.b = st.b;
    lab.settings.morph = st.morph;
    lab.rebuild();
    labDirOnA.store (st.directional); labAdaptA.store (st.adaptive);
    labWindowA.store (st.window); labPullA.store (st.pull);
    labOnA.store (st.on);
}

std::vector<std::pair<int, float>> JingFangProcessor::labSounding() const
{
    std::vector<std::pair<int, float>> v;
    for (int k = 0; k < 128; ++k)
    {
        const float p = labHeld[(size_t) k].load (std::memory_order_relaxed);
        if (p >= 0) v.push_back ({ k, p });
    }
    return v;
}

static juce::String tuningBlob (const jflab::Tuning& t)
{   // .scl, .kbm and the falling .scl in one base64 attribute; `svl render` ignores the <lab> element
    juce::String s (jflab::scala::writeScl (t));
    s << "@@KBM\n" << jflab::scala::writeKbm (t);
    if (t.directional()) s << "@@FALL\n" << jflab::scala::writeScl (t, true);
    return juce::Base64::toBase64 (s.toRawUTF8(), s.getNumBytesAsUTF8());
}

static bool tuningFromBlob (const juce::String& b64, jflab::Tuning& t)
{
    juce::MemoryOutputStream mo;
    if (! juce::Base64::convertFromBase64 (mo, b64)) return false;
    const auto s = mo.toString();
    const auto kbmAt = s.indexOf ("@@KBM\n"), fallAt = s.indexOf ("@@FALL\n");
    jflab::Tuning out;
    if (! jflab::scala::readScl (s.substring (0, kbmAt < 0 ? s.length() : kbmAt).toStdString(), out)) return false;
    if (kbmAt >= 0) jflab::scala::readKbm (s.substring (kbmAt + 6, fallAt < 0 ? s.length() : fallAt).toStdString(), out);
    if (fallAt >= 0)
    {
        jflab::Tuning f;
        if (jflab::scala::readScl (s.substring (fallAt + 7).toStdString(), f) && f.size() == out.size()) out.fall = f.cents;
    }
    t = out;
    return true;
}

juce::String JingFangProcessor::labStateXml (const LabState& st)
{
    juce::String l = "  <lab";
    l << " on=\"" << (st.on ? 1 : 0) << "\" directional=\"" << (st.directional ? 1 : 0) << "\" adaptive=\"" << (st.adaptive ? 1 : 0)
      << "\" morph=\"" << st.morph << "\" window=\"" << st.window << "\" pull=\"" << st.pull
      << "\" a=\"" << tuningBlob (st.a) << "\" b=\"" << tuningBlob (st.b) << "\"/>\n";
    return l;
}

bool JingFangProcessor::labStateFromXml (const juce::String& xml, LabState& st)
{
    const auto els = svl::detail::elements (xml.toStdString(), "lab");
    if (els.empty()) return false;
    const auto& e = els.front();
    auto num = [&e] (const char* k, float d) { const auto v = svl::detail::attr (e, k); return v.empty() ? d : juce::String (v).getFloatValue(); };
    st.on = num ("on", 0) > 0.5f; st.directional = num ("directional", 1) > 0.5f; st.adaptive = num ("adaptive", 0) > 0.5f;
    st.morph = juce::jlimit (0.0f, 1.0f, num ("morph", 0)); st.window = num ("window", 20); st.pull = juce::jlimit (0.0f, 1.0f, num ("pull", 0.5f));
    tuningFromBlob (juce::String (svl::detail::attr (e, "a")), st.a);
    tuningFromBlob (juce::String (svl::detail::attr (e, "b")), st.b);
    return true;
}

void JingFangProcessor::labLearn (const juce::File& file, int which, std::function<void (juce::String, bool)> done)
{
    std::weak_ptr<int> alive = labAlive;
    labPool.addJob ([this, file, which, done, alive]
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (file));
        juce::String msg;
        bool ok = false;
        jflab::LearnResult res;
        if (r == nullptr) msg = "This file could not be read (WAV, AIFF or FLAC).";
        else
        {
            const auto len = (int) juce::jmin<juce::int64> (r->lengthInSamples, (juce::int64) (r->sampleRate * 180));   // first 3 minutes
            juce::AudioBuffer<float> buf ((int) r->numChannels, len);
            r->read (&buf, 0, len, 0, true, true);
            std::vector<float> mono ((size_t) len, 0.0f);
            for (int c = 0; c < buf.getNumChannels(); ++c)
                for (int i = 0; i < len; ++i) mono[(size_t) i] += buf.getSample (c, i) / (float) buf.getNumChannels();
            res = jflab::learnTuning (mono.data(), len, r->sampleRate, -1, file.getFileNameWithoutExtension().toStdString(), &labCancel);
            ok = res.ok;
            msg = res.message;
        }
        juce::MessageManager::callAsync ([this, which, done, res, ok, msg, alive]
        {
            if (alive.expired()) return;   // the plug-in was closed while learning
            if (ok)
            {
                auto st = labMsg;
                (which == 0 ? st.a : st.b) = res.tuning;
                st.on = true;                                  // the learned tuning plays at once, on the Lab page (owner 19 Oct)
                setLabState (st);
            }
            if (done) done (msg, ok);
        });
    });
}

juce::AudioProcessorEditor* JingFangProcessor::createEditor() { return new JingFangEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new JingFangProcessor(); }
