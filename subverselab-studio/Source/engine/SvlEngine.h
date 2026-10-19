// SVL engine: plain C++17, no JUCE. The raw render tool (tools/svl.cpp) and the plug-in (milestone 5) run this same code.
//
// Signal path per voice: 3 oscillators (VA saw / square-PWM / triangle / sine, white / pink noise, Karplus-Strong string),
// each with level, pan, octave / semi / fine. The VA oscillators (polyBLEP / polyBLAMP) and the drive (tanh, ADAA) run at
// 2x and pass a 47-tap half-band decimator (11.5 samples of latency at the base rate) -> stereo state-variable filter
// (LP / BP / HP / notch) with key tracking, envelope and velocity amount -> amp ADSR x velocity.
// Voices sum -> master level -> 5 Hz DC blocker.
//
// Real time: prepare() allocates (voices, string delay lines); process() never allocates, locks or does I/O.
// Note events land on their exact sample inside the block; every time-dependent stage runs per sample, so the output does not
// depend on the block size (the selftest checks this bit for bit).
//
// Pitch is a float in MIDI semitones (60 = C4, 60.5 = a quarter tone above). noteOn() maps a MIDI key through the 128-entry
// tuning table (12-TET by default); noteOnPitch() takes any pitch directly. This is the hook for the microtonal keyboard.
#pragma once
#include "SvlDsp.h"
#include "SvlFx.h"
#include "SvlArpSeq.h"
#include <array>
#include <memory>

namespace svl {

constexpr int kMaxVoices = 16;        // polyphony ceiling (voice.polyphony)
constexpr int kVoicePool = 24;        // + spare slots, so a stolen voice can fade out while the new note starts on time
constexpr int kChunk = 32;            // internal render chunk (any block size is split into these and at events)
constexpr int kMaxEvents = 1024;
constexpr float kStealFadeSec = 0.003f;
constexpr int kMaxUni = 8;

struct Event
{
    enum Type : int { on, off, allOff } type = on;
    int offset = 0;      // sample inside the next process() block
    float pitch = 60;    // semitones (MIDI scale)
    float vel = 1;       // 0..1
    int id = 0;          // note id (MIDI key for noteOn())
    int key = -1;        // MIDI key when the pitch is resolved at the event's sample (tuning map, keyswitches)
};

constexpr int kOscOrder[kNumOsc] = { 1, 0, 2 };

struct VoiceInfo { bool active = false, gate = false, fading = false; int id = -1; float pitch = 0, ampLevel = 0; int ampStage = 0; };

class DrumMachine;

class Engine
{
public:
    Engine();
    ~Engine();
    Engine (const Engine&) = delete;
    Engine& operator= (const Engine&) = delete;

    void prepare (double sampleRate) { prepareCore (sampleRate, true); }
    // a drum machine track: the same voice engine without the FX rack and without its own drum machine
    void prepareTrack (double sampleRate) { prepareCore (sampleRate, false); }

    // the drum machine (SvlDrums.h): a track's sound = a drum preset's values + its note length (drumGateSeconds (family))
    void setDrumTrackSound (int track, const ParamSet& sound, float gateSeconds);
    void clearDrumTrackSound (int track);
    int drumStep() const;                        // the step playing now, -1 when stopped (the panel's LEDs)
    const DrumMachine* drumMachine() const { return drums.get(); }
    // nothing sounds and nothing is waiting (a drum track can be skipped)
    bool idle() const { return numEvents == 0 && activeVoices (true) == 0 && ! gen.needsService(); }

    void prepareCore (double sampleRate, bool full)
    {
        sr = (float) sampleRate;
        HalfbandDecimator::oddTaps();   // builds the static tap table here, not on the audio thread
        for (auto& v : voices)
        {
            for (auto& k : v.ks) k.prepare (sampleRate);
            v.aenv.setRate (sr); v.fenv.setRate (sr);
            v.reset();
        }
        const int ramp = (int) std::lround (0.02 * sampleRate);
        for (auto* s : allSmoothers()) s->setRampLength (ramp);
        dcR = 1.0f - (float) (2.0 * kPi * 5.0 / sampleRate);
        dcX[0] = dcX[1] = dcY[0] = dcY[1] = 0;
        for (int i = 0; i < 128; ++i) tuning[(size_t) i] = (float) i;
        isTrack = ! full;
        if (full) fx.prepare (sampleRate);
        gen.prepare (sampleRate); gen.setTempo (tempo);
        if (full) prepareDrums (sampleRate);
        setParams (params, true);
        prepared = true;
    }

    // ---- parameters (call between process() blocks, from the audio thread or with the plug-in's usual handoff)
    void setParam (int i, float value, bool snap = false)
    {
        if (i < 0 || i >= kNumParams) return;
        params[i] = clampParam (i, value);
        updateTargets (snap);
    }
    void setParams (const ParamSet& p, bool snap)
    {
        for (int i = 0; i < kNumParams; ++i) params[i] = clampParam (i, p[i]);
        updateTargets (snap);
    }
    const ParamSet& getParams() const { return params; }
    void setModWheel (float v01) { modWheel = std::fmax (0.0f, std::fmin (1.0f, v01)); }
    void setAftertouch (float v01) { aftertouch = std::fmax (0.0f, std::fmin (1.0f, v01)); }
    void setTempo (double bpm)
    {
        if (bpm > 1 && bpm < 1000 && (float) bpm != tempo) { tempo = (float) bpm; fx.set (fxSettings(), tempo, false); gen.setTempo (tempo); drumsTempo(); }
    }
    // once per host block, before process(): the host transport (ppq at the block's first sample). Without a host transport
    // (standalone, svl render) the arp / seq run on their own clock at the set tempo.
    void setTransport (bool hasHostTransport, bool playing, double ppq) { gen.setTransport (hasHostTransport, playing, ppq); drumsTransport (hasHostTransport, playing, ppq); }
    // PLAY in a stopped host: preview the free sequence / beat (RU-8 rule 2); the host's start takes over, its stop ends it
    void setPreview (bool seq, bool drumsToo);
    bool previewing (bool drumsUnit) const;
    const NoteGen& noteGen() const { return gen; }
    void setPitchBend (float m11) { bendNorm = std::fmax (-1.0f, std::fmin (1.0f, m11)); updateTargets (false); }
    void setTuning (const float* pitches128) { for (int i = 0; i < 128; ++i) tuning[(size_t) i] = pitches128[i]; }

    // ---- events for the next process() block
    void noteOn (int key, float vel, int offset) { if (key >= 0 && key < 128) push ({ Event::on, offset, 0, vel, key, key }); }
    void noteOff (int key, int offset) { push ({ Event::off, offset, 0, 0, key, key }); }
    // pitch a MIDI key plays now (tuning table + mode + rows; before the reference offset)
    float keyPitch (int key) const
    {
        float p = tuning[(size_t) std::max (0, std::min (127, key))];
        if (params[TUNE_MODE] > 0.5f)
        {
            const int row = (int) params[TUNE_ROW0 + key % 12];
            const float steps = row == 0 ? -params[ROW_DOWN] : (row == 2 ? params[ROW_UP] : 0.0f);
            p += steps * 12.0f / params[TUNE_EDO];
        }
        return p;
    }
    bool tuningChangedByKeyswitch() { const bool c = tuningDirty; tuningDirty = false; return c; }
    void noteOnPitch (float pitch, float vel, int id, int offset) { push ({ Event::on, offset, pitch, vel, id }); }
    void allNotesOff (int offset) { push ({ Event::allOff, offset, 0, 0, 0 }); }

    void process (float* outL, float* outR, int n)
    {
        DenormalGuard guard;
        int pos = 0, e = 0;
        while (pos < n)
        {
            while (e < numEvents && events[(size_t) e].offset <= pos) apply (events[(size_t) e++]);
            int end = std::min (n, pos + kChunk);
            if (e < numEvents) end = std::min (end, events[(size_t) e].offset);
            const bool g = gen.needsService();
            if (g)
            {
                GenNote gn;
                while (gen.pop (gn)) applyGen (gn);
                end = std::min (end, pos + std::max (1, gen.samplesToNext()));
            }
            const bool d = drumsService (end - pos, end);
            if (d) end = std::min (end, pos + drumsNext());
            render (outL + pos, outR + pos, end - pos);
            if (g) gen.advance (end - pos);
            if (d) drumsAdvance (end - pos);
            pos = end;
        }
        while (e < numEvents) apply (events[(size_t) e++]);
        numEvents = 0;
    }

    // ---- inspection (tests, the panel's voice meter)
    int activeVoices (bool countFading = false) const
    {
        int c = 0;
        for (auto& v : voices) if (v.aenv.active() && (countFading || ! v.fading)) ++c;
        return c;
    }
    VoiceInfo voiceInfo (int i) const
    {
        const Voice& v = voices[(size_t) i];
        return { v.aenv.active(), v.gate, v.fading, v.id, v.pitch, v.aenv.level, (int) v.aenv.stage };
    }
    struct Stats { uint64_t steals = 0, hardSteals = 0, nonFiniteResets = 0, droppedEvents = 0; } stats;

private:
    struct Voice
    {
        int id = -1;
        bool gate = false, fading = false, sounding = false;
        uint64_t age = 0;
        int fadeRemain = 0; float fadeStep = 0, fadeGain = 1;
        float pitch = 60, target = 60, glideStep = 0; int glideRemain = 0;
        float vel = 1, velGain = 1;
        float lastP[kNumOsc] {}, dt[kNumOsc] {};
        float oscGain[kNumOsc] {}, oscMul[kNumOsc] {};   // per-oscillator decay
        float penv = 0, penvMul = 1, penvScale = 1;       // pitch envelope (1 -> 0), velocity scale
        float clickAmp = 0, clickMul = 0, clickHpA = 0, clickX = 0, clickY = 0;
        float lfoPhase[kNumLfo] {}, lfoHold[kNumLfo] {}, lfoRateMod[kNumLfo] {}, noteRnd = 0; int64_t noteAge = 0;
        Rng driftRng; int driftCount = 0; float dpT = 0, daT = 0, dp1 = 0, dp2 = 0, da1 = 0, da2 = 0;
        VaOsc va[kNumOsc][kMaxUni]; Noise noise[kNumOsc]; KsString ks[kNumOsc]; Noise floorN[2];
        Adsr aenv, fenv;
        Svf svf; AdaaTanh sat[2]; HalfbandDecimator dec;
        void reset()
        {
            id = -1; gate = fading = sounding = false; fadeGain = 1; fadeRemain = 0; glideRemain = 0;
            aenv.kill(); fenv.kill(); svf.reset(); sat[0].reset(); sat[1].reset(); dec.reset();
            for (int o = 0; o < kNumOsc; ++o) { for (int u = 0; u < kMaxUni; ++u) va[o][u].reset (u == 0 ? 0.0f : std::fmod (0.61803398875f * (float) u + 0.1f * (float) o, 1.0f)); ks[o].clear(); lastP[o] = -1e9f; dt[o] = 0; oscGain[o] = 1; oscMul[o] = 1; }
            for (int l = 0; l < kNumLfo; ++l) { lfoPhase[l] = 0; lfoHold[l] = 0; lfoRateMod[l] = 0; } noteAge = 0; noteRnd = 0;
            penv = 0; clickAmp = 0; clickX = clickY = 0; driftCount = 0; dpT = daT = dp1 = dp2 = da1 = da2 = 0;
        }
    };

    float sr = 44100;
    bool prepared = false, isTrack = false;
    std::unique_ptr<DrumMachine> drums;   // the top-level engine only (created in prepare)
    // the drum machine glue, defined after SvlDrums.h
    void prepareDrums (double sampleRate);
    void drumsTempo();
    void drumsTransport (bool hasHost, bool playing, double ppq);
    void drumsParams();
    bool drumsService (int maxN, int end);
    int drumsNext();
    void drumsAdvance (int n);
    void drumsRender (float* L, float* R, int n);
    bool drumsKeys() const;
    void drumsHit (int track, float vel);
    void drumsAllOff();
    void drumsKey (const Event& ev);
    bool drumsTakeKeys() const;
    ParamSet params;
    std::array<float, 128> tuning {};
    std::array<Voice, kVoicePool> voices;
    std::array<Event, kMaxEvents> events;
    int numEvents = 0;
    uint64_t ageCounter = 0, seedCounter = 1;
    float bendNorm = 0, modWheel = 0, aftertouch = 0, tempo = 120;
    double lfoGlobalPhase[kNumLfo] {};
    bool tuningDirty = false;

    // held-note stack for mono / legato (last-note priority)
    struct Held { float pitch, vel; int id; };
    std::array<Held, 64> held {}; int numHeld = 0;
    int monoVoice = 0;

    // smoothed engine-level values
    Smoother sLevel[kNumOsc], sPw[kNumOsc], sPan[kNumOsc], sCut, sReso, sDrive, sEnvAmt, sMaster, sBend;
    std::array<Smoother*, 3 * kNumOsc + 6> allSmoothers()
    {
        return { &sLevel[0], &sLevel[1], &sLevel[2], &sPw[0], &sPw[1], &sPw[2], &sPan[0], &sPan[1], &sPan[2],
                 &sCut, &sReso, &sDrive, &sEnvAmt, &sMaster, &sBend };
    }
    // per-chunk arrays filled from the smoothers
    float aLevelL[kNumOsc][kChunk], aLevelR[kNumOsc][kChunk], aPw[kNumOsc][kChunk];
    float aCut[kChunk], aReso[kChunk], aDriveDb[kChunk], aDriveG[kChunk], aDriveMix[kChunk], aEnvAmt[kChunk], aMaster[kChunk], aBend[kChunk];
    float mixL[kChunk], mixR[kChunk];
    float dcR = 0.9993f, dcX[2] {}, dcY[2] {};

    void updateTargets (bool snap)
    {
        for (int o = 0; o < kNumOsc; ++o)
        {
            set (sLevel[o], params[oscParam (o, O_LEVEL)], snap);
            set (sPw[o], params[oscParam (o, O_PW)], snap);
            set (sPan[o], params[oscParam (o, O_PAN)], snap);
        }
        set (sCut, std::log2 (params[FLT_CUTOFF]), snap);
        set (sReso, params[FLT_RESO], snap);
        set (sDrive, params[FLT_DRIVE], snap);
        set (sEnvAmt, params[FLT_ENVAMT], snap);
        set (sMaster, dbToGain (params[MASTER]), snap);
        set (sBend, bendNorm * params[V_BEND], snap);
        fx.set (fxSettings(), tempo, snap);
        gen.setParams (params);
        drumsParams();
    }
    FxRack::Settings fxSettings() const
    {
        FxRack::Settings s;
        s.driveAmount = params[FXD_AMOUNT]; s.driveTone = params[FXD_TONE]; s.driveMix = params[FXD_MIX];
        s.echoTime = (int) params[FXE_TIME]; s.echoFb = params[FXE_FB]; s.echoDamp = params[FXE_DAMP]; s.echoMix = params[FXE_MIX];
        s.revSize = params[FXR_SIZE]; s.revDecay = params[FXR_DECAY]; s.revPre = params[FXR_PRE]; s.revDamp = params[FXR_DAMP]; s.revMix = params[FXR_MIX];
        return s;
    }
    FxRack fx;
    NoteGen gen;
    static void set (Smoother& s, float v, bool snap) { if (snap) s.reset (v); else s.set (v); }

    void push (const Event& ev)
    {
        if (numEvents >= kMaxEvents) { ++stats.droppedEvents; return; }
        Event x = ev; if (x.offset < 0) x.offset = 0;
        int i = numEvents++;
        while (i > 0 && events[(size_t) i - 1].offset > x.offset) { events[(size_t) i] = events[(size_t) i - 1]; --i; }   // stable
        events[(size_t) i] = x;
    }

    VoiceMode mode() const { return (VoiceMode) (int) params[V_MODE]; }

    float refSemis() const { return 12.0f * std::log2 (params[TUNE_REF] / 440.0f); }

    bool isKeyswitch (int key) const
    {
        return key >= 0 && params[TUNE_MODE] > 0.5f && params[KSW_MODE] > 0.5f
               && key >= (int) params[KSW_LOW] && key < (int) params[KSW_LOW] + 24;
    }

    void apply (const Event& evIn)
    {
        Event ev = evIn;
        if (ev.key >= 0)
        {
            // only note-ons are filtered by the keyswitch / drum-key state: an off always reaches the voices, so a key that was
            // sounding when a keyswitch range or drums.keys was switched on cannot hang
            if (ev.type == Event::on && isKeyswitch (ev.key))
            {   // first 12 keys toggle "low" for that pitch class, next 12 toggle "high"; keyswitches never sound
                const int pc = ev.key % 12, want = ev.key < (int) params[KSW_LOW] + 12 ? 0 : 2;
                float& r = params[TUNE_ROW0 + pc];
                r = (int) r == want ? 1.0f : (float) want;
                tuningDirty = true;
                return;
            }
            if (ev.type == Event::on && ev.key >= 36 && ev.key <= 43 && drumsKeys())
            {   // drums.keys: MIDI 36..43 hit tracks 1..8 (a hit has its own length)
                drumsHit (ev.key - 36, ev.vel);
                return;
            }
            drumsKey (ev);   // drums.mode hold: the pattern runs while a key is held
            if (ev.type == Event::on && drumsTakeKeys()) return;   // hold: keys are the drum machine's transport only (owner 19 Oct)
            if (gen.active() && ev.type != Event::allOff)
            {   // the arp / seq take the key; a note-off also reaches a voice the key started before they were switched on
                if (ev.type == Event::on) { gen.keyOn (ev.id, ev.key, 0, ev.vel); return; }
                gen.keyOff (ev.id);
            }
            else if (ev.type == Event::on) ev.pitch = keyPitch (ev.key);
        }
        else if (ev.type != Event::allOff)
        {
            drumsKey (ev);
            if (ev.type == Event::on && drumsTakeKeys()) return;
        }
        if (ev.key < 0 && gen.active() && ev.type != Event::allOff)
        {   // a panel row key (explicit pitch)
            if (ev.type == Event::on) { gen.keyOn (ev.id, -1, ev.pitch, ev.vel); return; }
            gen.keyOff (ev.id);
        }
        if (ev.type == Event::on) ev.pitch += refSemis();
        if (ev.type == Event::allOff)
        {
            gen.allOff();
            drumsAllOff();
            numHeld = 0;
            for (auto& v : voices) if (v.gate) release (v);
            return;
        }
        if (mode() == VoiceMode::poly) { ev.type == Event::on ? polyOn (ev) : polyOff (ev.id); return; }
        ev.type == Event::on ? monoOn (ev) : monoOff (ev.id);
    }

    // a note from the arp / seq: resolve its pitch now (tuning, rows, reference), then the usual voice allocation
    void applyGen (const GenNote& g)
    {
        if (g.kind == GenNote::off) { mode() == VoiceMode::poly ? polyOff (g.id) : monoOff (g.id); return; }
        Event ev; ev.type = Event::on; ev.id = g.id; ev.vel = g.vel;
        ev.pitch = (g.key >= 0 ? keyPitch (g.key) : g.pitch) + g.add + refSemis();
        if (g.kind == GenNote::slide)
            for (auto& v : voices)
                if (v.gate && ! v.fading && v.id == g.prevId)
                {   // glide the sounding voice to the new note, no retrigger (voice.glide, or 60 ms when it is 0)
                    v.id = g.id;
                    for (int i = 0; i < numHeld; ++i) if (held[(size_t) i].id == g.prevId) { held[(size_t) i].id = g.id; held[(size_t) i].pitch = ev.pitch; }
                    glideTo (v, ev.pitch, params[V_GLIDE] > 0.001f ? params[V_GLIDE] : 0.06f);
                    return;
                }
        mode() == VoiceMode::poly ? polyOn (ev) : monoOn (ev);
    }

    // ---------------------------------------------------------------- voice allocation
    void polyOn (const Event& ev)
    {
        for (auto& v : voices) if (v.gate && v.id == ev.id) release (v);   // same key again: let the old one ring out
        const int limit = (int) params[V_POLY];
        if (activeVoices (false) >= limit)
        {
            Voice* victim = nullptr;
            for (auto& v : voices)   // oldest released voice first, then the oldest held one
                if (v.aenv.active() && ! v.fading && ! v.gate && (! victim || v.age < victim->age)) victim = &v;
            if (! victim)
                for (auto& v : voices)
                    if (v.aenv.active() && ! v.fading && (! victim || v.age < victim->age)) victim = &v;
            if (victim) { startFade (*victim); ++stats.steals; }
        }
        Voice* slot = nullptr;
        for (auto& v : voices) if (! v.aenv.active()) { slot = &v; break; }
        if (! slot)
        {   // every slot busy (only fading voices left): take the one closest to silence
            for (auto& v : voices) if (v.fading && (! slot || v.fadeGain < slot->fadeGain)) slot = &v;
            if (! slot) slot = &voices[0];
            ++stats.hardSteals;
        }
        start (*slot, ev.pitch, ev.vel, ev.id, false);
    }

    bool oneShot() const { return params[DRUM_ONESHOT] > 0.5f; }

    void polyOff (int id)
    {
        if (oneShot()) { for (auto& v : voices) if (v.gate && v.id == id) v.gate = false; return; }   // the hit plays out
        for (auto& v : voices) if (v.gate && v.id == id) release (v);
    }

    void monoOn (const Event& ev)
    {
        removeHeld (ev.id);
        if (numHeld < (int) held.size()) held[(size_t) numHeld++] = { ev.pitch, ev.vel, ev.id };
        Voice& v = voices[(size_t) monoVoice];
        for (int i = 0; i < kVoicePool; ++i) if (i != monoVoice && voices[(size_t) i].aenv.active() && ! voices[(size_t) i].fading) startFade (voices[(size_t) i]);
        const bool wasHeld = numHeld > 1 && v.gate;
        if (wasHeld && mode() == VoiceMode::legato) { v.id = ev.id; glideTo (v, ev.pitch); return; }   // no retrigger
        start (v, ev.pitch, ev.vel, ev.id, v.aenv.active());
    }

    void monoOff (int id)
    {
        for (int i = 0; i < kVoicePool; ++i)   // a voice left from poly mode (the mode changed while keys were held)
            if (i != monoVoice && voices[(size_t) i].gate && voices[(size_t) i].id == id) release (voices[(size_t) i]);
        const bool wasTop = numHeld > 0 && held[(size_t) numHeld - 1].id == id;
        removeHeld (id);
        if (oneShot()) { if (numHeld == 0) voices[(size_t) monoVoice].gate = false; return; }   // the hit plays out
        Voice& v = voices[(size_t) monoVoice];
        if (! v.gate) return;
        if (numHeld == 0) { if (v.id == id || wasTop) release (v); return; }
        if (wasTop) { const Held& h = held[(size_t) numHeld - 1]; v.id = h.id; glideTo (v, h.pitch); }   // back to the previous key
    }

    void removeHeld (int id)
    {
        int j = 0;
        for (int i = 0; i < numHeld; ++i) if (held[(size_t) i].id != id) held[(size_t) j++] = held[(size_t) i];
        numHeld = j;
    }

    void glideTo (Voice& v, float pitch, float seconds = -1)
    {
        v.target = pitch;
        const int len = (int) std::lround ((seconds >= 0 ? seconds : params[V_GLIDE]) * sr);
        if (len <= 0 || ! v.sounding) { v.pitch = pitch; v.glideRemain = 0; retune (v); return; }
        v.glideRemain = len;
        v.glideStep = (v.target - v.pitch) / (float) len;
    }

    void start (Voice& v, float pitch, float vel, int id, bool continuing)
    {
        if (params[DRUM_MODE] > 0.5f) pitch = params[DRUM_PITCH] + refSemis() + params[DRUM_KEYTRACK] * (pitch - refSemis() - 60.0f);
        if (params[CHOKE] > 0.5f)
            for (auto& o : voices) if (&o != &v && o.aenv.active() && ! o.fading) startFade (o);
        const bool legatoPitch = continuing && v.sounding && mode() != VoiceMode::poly;
        if (! continuing) { v.reset(); }
        v.id = id; v.gate = true; v.fading = false; v.fadeGain = 1; v.fadeRemain = 0;
        v.age = ++ageCounter;
        v.vel = std::fmax (0.0f, std::fmin (1.0f, vel));
        const float va = params[V_VELAMP];
        v.velGain = 1.0f - va + va * v.vel;
        if (legatoPitch) glideTo (v, pitch);
        else { v.pitch = v.target = pitch; v.glideRemain = 0; }
        v.sounding = true;
        v.aenv.set (params[AENV_A], params[AENV_D], params[AENV_S], params[AENV_R], params[AENV_CA], params[AENV_CD], params[AENV_CR]);
        v.fenv.set (params[FENV_A], params[FENV_D], params[FENV_S], params[FENV_R], params[FENV_CA], params[FENV_CD], params[FENV_CR]);
        v.aenv.trigger(); v.fenv.trigger();
        v.penv = 1.0f; v.penvMul = (float) std::pow (10.0, -1.0 / (params[PENV_DECAY] * sr));
        v.clickAmp = params[CLICK_LEVEL] * (1.0f - params[CLICK_VEL] + params[CLICK_VEL] * v.vel);
        v.penvScale = 1.0f - params[PENV_VEL] + params[PENV_VEL] * v.vel;
        v.clickMul = (float) std::pow (10.0, -3.0 / (params[CLICK_DECAY] * sr));
        v.clickHpA = (float) std::exp (-2.0 * kPi * params[CLICK_HP] / sr);
        for (int l = 0; l < kNumLfo; ++l)
            if (params[lfoParam (l, L_RETRIG)] > 0.5f || ! continuing)
                v.lfoPhase[l] = params[lfoParam (l, L_RETRIG)] > 0.5f ? 0.0f : (float) lfoGlobalPhase[l];
        v.noteAge = 0;
        if (! continuing) { v.driftRng.seed (seedCounter++ * 0x9E3779B97F4A7C15ull); v.floorN[0].reset (seedCounter++); v.floorN[1].reset (seedCounter++);
                             v.noteRnd = v.driftRng.uni(); for (int l = 0; l < kNumLfo; ++l) v.lfoHold[l] = v.driftRng.uni(); }
        for (int o = 0; o < kNumOsc; ++o)
        {
            const float dsec = params[oscParam (o, O_DECAY)];
            v.oscGain[o] = 1.0f; v.oscMul[o] = dsec > 0 ? (float) std::pow (10.0, -3.0 / (dsec * sr)) : 1.0f;
            const OscType t = (OscType) (int) params[oscParam (o, O_TYPE)];
            if (! continuing) v.noise[o].reset (seedCounter++ * 0x100000001B3ull);
            if (t == OscType::string)
            {
                v.ks[o].tune (pitchToHz (oscPitch (v.pitch, o)), params[oscParam (o, O_KS_DECAY)], params[oscParam (o, O_KS_TONE)]);
                v.ks[o].pluck (v.noise[o].rng, 0.5f + 0.5f * v.vel, params[oscParam (o, O_KS_TONE)] * (0.6f + 0.4f * v.vel));
            }
            v.lastP[o] = -1e9f;
        }
    }

    void release (Voice& v) { v.gate = false; v.aenv.noteOff(); v.fenv.noteOff(); }

    void startFade (Voice& v)
    {
        v.gate = false; v.fading = true;
        v.fadeRemain = std::max (1, (int) std::lround (kStealFadeSec * sr));
        v.fadeStep = v.fadeGain / (float) v.fadeRemain;
    }

    float oscPitch (float p, int o) const
    {
        return p + 12.0f * params[oscParam (o, O_OCT)] + params[oscParam (o, O_SEMI)] + 0.01f * params[oscParam (o, O_FINE)];
    }

    void retune (Voice& v)
    {
        for (int o = 0; o < kNumOsc; ++o) v.lastP[o] = -1e9f;
    }

    // ---------------------------------------------------------------- rendering
    void render (float* outL, float* outR, int n)
    {
        if (n <= 0) return;
        for (int i = 0; i < n; ++i)
        {
            for (int o = 0; o < kNumOsc; ++o)
            {
                const float lv = sLevel[o].next(), pn = sPan[o].next();
                const float ang = (float) ((pn + 1.0) * 0.25 * kPi);
                aLevelL[o][i] = lv * std::cos (ang); aLevelR[o][i] = lv * std::sin (ang);
                aPw[o][i] = sPw[o].next();
            }
            aCut[i] = sCut.next(); aReso[i] = sReso.next();
            const float dDb = sDrive.next();
            aDriveDb[i] = dDb; aDriveG[i] = dbToGain (dDb); aDriveMix[i] = std::fmin (1.0f, dDb / 3.0f);
            aEnvAmt[i] = sEnvAmt.next(); aMaster[i] = sMaster.next(); aBend[i] = sBend.next();
            mixL[i] = mixR[i] = 0;
        }
        for (int l = 0; l < kNumLfo; ++l)
        {
            lfoRate[l] = lfoBaseRate (l);
            const double ph0 = lfoGlobalPhase[l];
            lfoGlobalPhase[l] = std::fmod (ph0 + (double) lfoRate[l] * n / sr, 1.0);
        }
        for (auto& v : voices) if (v.aenv.active()) renderVoice (v, n);
        for (int i = 0; i < n; ++i) { mixL[i] *= aMaster[i]; mixR[i] *= aMaster[i]; }
        if (drums) drumsRender (mixL, mixR, n);
        fx.process (mixL, mixR, n);
        for (int i = 0; i < n; ++i)
        {
            const float l = mixL[i], r = mixR[i];
            dcY[0] = l - dcX[0] + dcR * dcY[0]; dcX[0] = l;
            dcY[1] = r - dcX[1] + dcR * dcY[1]; dcX[1] = r;
            outL[i] = dcY[0]; outR[i] = dcY[1];
        }
    }

    float lfoRate[kNumLfo] {};
    float lfoBaseRate (int l) const
    {
        const int sy = (int) params[lfoParam (l, L_SYNC)];
        if (sy > 0) return tempo / 60.0f / kSyncBeats[sy];
        return params[lfoParam (l, L_RATE)];
    }
    static float lfoShape (LfoShape s, float ph, float hold)
    {
        switch (s)
        {
            case LfoShape::sine: return (float) std::sin (2.0 * kPi * ph);
            case LfoShape::triangle: return 1.0f - 4.0f * std::fabs (ph - 0.5f);
            case LfoShape::saw: return 2.0f * ph - 1.0f;
            case LfoShape::square: return ph < 0.5f ? 1.0f : -1.0f;
            case LfoShape::sampleHold: return hold;
            default: return 0;
        }
    }

    void renderVoice (Voice& v, int n)
    {
        // modulation matrix: the active slots, amounts scaled to their destination's units
        int nSlots = 0; int slotSrc[kNumModSlots], slotDst[kNumModSlots]; float slotAmt[kNumModSlots];
        for (int m = 0; m < kNumModSlots; ++m)
        {
            const int sr_ = (int) params[modParam (m, M_SRC)], ds = (int) params[modParam (m, M_DST)];
            const float am = params[modParam (m, M_AMT)];
            if (sr_ <= 0 || ds <= 0 || am == 0) continue;
            slotSrc[nSlots] = sr_; slotDst[nSlots] = ds; slotAmt[nSlots] = am * kModDstRange[ds]; ++nSlots;
        }
        LfoShape lshape[kNumLfo]; float lfade[kNumLfo];
        for (int l = 0; l < kNumLfo; ++l) { lshape[l] = (LfoShape) (int) params[lfoParam (l, L_SHAPE)]; lfade[l] = params[lfoParam (l, L_FADE)] * sr; }
        constexpr int ND = (int) ModDst::count;
        float mod[ND] = {};
        OscType type[kNumOsc]; float off[kNumOsc];
        bool any = false;
        for (int o = 0; o < kNumOsc; ++o)
        {
            type[o] = (OscType) (int) params[oscParam (o, O_TYPE)];
            off[o] = oscPitch (0, o);
            any |= type[o] != OscType::off;
        }
        const FilterMode fm = (FilterMode) (int) params[FLT_MODE];
        const float keyTrack = params[FLT_KEYTRACK], velOct = params[FLT_VELAMT] * v.vel;
        const float ksDecay[kNumOsc] = { params[oscParam (0, O_KS_DECAY)], params[oscParam (1, O_KS_DECAY)], params[oscParam (2, O_KS_DECAY)] };
        const float ksTone[kNumOsc] = { params[oscParam (0, O_KS_TONE)], params[oscParam (1, O_KS_TONE)], params[oscParam (2, O_KS_TONE)] };
        const float floorLv = params[FLOOR_LEVEL];
        const bool oneShotNow = oneShot();
        int uniN[kNumOsc]; float uniLog[kNumOsc][kMaxUni], uniRatio[kNumOsc][kMaxUni], uniL[kNumOsc][kMaxUni], uniR[kNumOsc][kMaxUni];
        for (int o = 0; o < kNumOsc; ++o)
        {
            const int nu = std::max (1, std::min (kMaxUni, (int) params[oscParam (o, O_UNI)]));
            const float det = params[oscParam (o, O_DETUNE)], spr = params[oscParam (o, O_SPREAD)], norm = 1.0f / std::sqrt ((float) nu);
            uniN[o] = nu;
            for (int u = 0; u < nu; ++u)
            {
                const float x = nu > 1 ? (float) u / (float) (nu - 1) * 2.0f - 1.0f : 0.0f;   // -1 .. 1
                uniLog[o][u] = 0.5f * det * x / 1200.0f; uniRatio[o][u] = std::exp2 (uniLog[o][u]);
                uniL[o][u] = norm * std::sqrt (1.0f - spr * x); uniR[o][u] = norm * std::sqrt (1.0f + spr * x);   // equal power per copy
            }
        }
        const float fmCycles = params[FM_AMOUNT] / (float) (2.0 * kPi);
        const int fmT = (int) params[FM_TARGET];
        bool fmMod_ = false; for (int m = 0; m < nSlots; ++m) fmMod_ |= slotDst[m] == (int) ModDst::fm;
        const bool fmAny = (fmCycles > 0 || fmMod_) && type[1] != OscType::off;
        const bool fmOn[kNumOsc] = { fmAny && fmT != 1, false, fmAny && fmT != 0 };
        const float penvAmt = params[PENV_AMT], driftC = params[DRIFT_CENTS], driftA = params[DRIFT_AMP];
        const bool drift = driftC > 0 || driftA > 0;
        const int driftPeriod = std::max (1, (int) (sr / params[DRIFT_RATE]));
        const float driftA1 = 1.0f - (float) std::exp (-2.0 * kPi * params[DRIFT_RATE] / sr);
        bool bad = false;
        for (int i = 0; i < n; ++i)
        {
            if (v.glideRemain > 0) { v.pitch += v.glideStep; if (--v.glideRemain == 0) v.pitch = v.target; }
            float p = v.pitch + aBend[i] + penvAmt * v.penvScale * v.penv;
            v.penv *= v.penvMul;
            float driftGain = 1.0f;
            if (drift)
            {   // a new random target every 1/rate s, smoothed by two one-pole stages at the rate
                if (v.driftCount-- <= 0) { v.driftCount = driftPeriod; v.dpT = v.driftRng.uni(); v.daT = v.driftRng.uni(); }
                v.dp1 += driftA1 * (v.dpT - v.dp1); v.dp2 += driftA1 * (v.dp1 - v.dp2);
                v.da1 += driftA1 * (v.daT - v.da1); v.da2 += driftA1 * (v.da1 - v.da2);
                p += 0.01f * driftC * v.dp2;
                driftGain = dbToGain (driftA * v.da2);
            }
            if (nSlots > 0)
            {
                float src[(int) ModSrc::count] = {};
                for (int l = 0; l < kNumLfo; ++l)
                {
                    float y = lfoShape (lshape[l], v.lfoPhase[l], v.lfoHold[l]);
                    if (lfade[l] > 0 && (float) v.noteAge < lfade[l]) y *= (float) v.noteAge / lfade[l];
                    src[1 + l] = y;
                    v.lfoPhase[l] += lfoRate[l] * std::exp2 (v.lfoRateMod[l]) / sr;
                    if (v.lfoPhase[l] >= 1.0f) { v.lfoPhase[l] -= std::floor (v.lfoPhase[l]); v.lfoHold[l] = v.driftRng.uni(); }
                }
                src[(int) ModSrc::ampEnv] = v.aenv.level; src[(int) ModSrc::filterEnv] = v.fenv.level;
                src[(int) ModSrc::velocity] = v.vel; src[(int) ModSrc::key] = (v.pitch - 60.0f) / 60.0f;
                src[(int) ModSrc::modWheel] = modWheel; src[(int) ModSrc::aftertouch] = aftertouch; src[(int) ModSrc::random] = v.noteRnd;
                for (int d = 0; d < ND; ++d) mod[d] = 0;
                for (int m = 0; m < nSlots; ++m) mod[slotDst[m]] += src[slotSrc[m]] * slotAmt[m];
                for (int l = 0; l < kNumLfo; ++l) v.lfoRateMod[l] = mod[(int) ModDst::lfo1Rate + l];
                p += mod[(int) ModDst::pitch];
            }
            ++v.noteAge;
            if (oneShotNow && v.aenv.stage == Adsr::sustain) { v.aenv.noteOff(); v.fenv.noteOff(); }   // one-shot: no sustain
            const float amp = v.aenv.next(), fe = v.fenv.next();
            // VA oscillators and the drive run at 2x, then one half-band decimator per voice; noise and string are
            // generated at the base rate and held for both 2x samples.
            float L2[2] = { 0, 0 }, R2[2] = { 0, 0 }, Lb = 0, Rb = 0;
            float fmMod[2] = { 0, 0 };   // osc 2's first copy, the FM modulator
            if (any)
                for (int oi = 0; oi < kNumOsc; ++oi)
                {
                    const int o = kOscOrder[oi];   // osc 2 first, so its output can modulate osc 1 / 3 in the same sample
                    if (type[o] == OscType::off) continue;
                    const float og = v.oscGain[o];
                    v.oscGain[o] *= v.oscMul[o];
                    const float lm = nSlots ? std::fmax (0.0f, 1.0f + mod[(int) ModDst::osc1Level + o]) : 1.0f;
                    const float op = p + off[o] + (nSlots ? mod[(int) ModDst::osc1Pitch + o] : 0.0f);
                    const float gl = aLevelL[o][i] * og * lm, gr = aLevelR[o][i] * og * lm;
                    if (type[o] == OscType::noiseWhite || type[o] == OscType::noisePink || type[o] == OscType::string)
                    {
                        float y;
                        if (type[o] == OscType::noiseWhite) y = v.noise[o].white();
                        else if (type[o] == OscType::noisePink) y = v.noise[o].pink();
                        else
                        {
                            if (op != v.lastP[o]) { v.ks[o].tune (pitchToHz (op), ksDecay[o], ksTone[o]); v.lastP[o] = op; }
                            y = v.ks[o].next();
                        }
                        Lb += y * gl; Rb += y * gr;
                        continue;
                    }
                    if (op != v.lastP[o]) { v.dt[o] = std::fmin (0.49f, pitchToHz (op) / (2.0f * sr)); v.lastP[o] = op; }
                    const int nu = uniN[o];
                    const float pw = nSlots ? std::fmax (0.05f, std::fmin (0.95f, aPw[o][i] + mod[(int) ModDst::osc1Pw + o])) : aPw[o][i];
                    const float detMul = nSlots ? std::fmax (0.0f, 1.0f + mod[(int) ModDst::detune]) : 1.0f;
                    const float fmC = nSlots ? std::fmax (0.0f, fmCycles + mod[(int) ModDst::fm] / (float) (2.0 * kPi)) : fmCycles;
                    for (int k = 0; k < 2; ++k)
                    {
                        const float pm = (o != 1 && fmOn[o]) ? fmC * fmMod[k] : 0.0f;
                        float sL = 0, sR = 0;
                        for (int u = 0; u < nu; ++u)
                        {
                            const float ratio = detMul == 1.0f ? uniRatio[o][u] : std::exp2 (detMul * uniLog[o][u]);
                            const float y = v.va[o][u].next (type[o], std::fmin (0.49f, v.dt[o] * ratio), pw, false, pm);
                            sL += y * uniL[o][u]; sR += y * uniR[o][u];
                            if (o == 1 && u == 0) fmMod[k] = y;
                        }
                        L2[k] += sL * gl; R2[k] += sR * gr;
                    }
                }
            if (floorLv > 0) { Lb += floorLv * v.floorN[0].pink(); Rb += floorLv * v.floorN[1].pink(); }
            L2[0] += Lb; L2[1] += Lb; R2[0] += Rb; R2[1] += Rb;
            // drive: tanh(g x) / sqrt(g), blended in over the first 3 dB so 0 dB is exactly linear
            float mix = aDriveMix[i], g = aDriveG[i];
            if (nSlots && mod[(int) ModDst::drive] != 0)
            {
                const float dDb = std::fmax (0.0f, std::fmin (48.0f, aDriveDb[i] + mod[(int) ModDst::drive]));
                g = dbToGain (dDb); mix = std::fmin (1.0f, dDb / 3.0f);
            }
            if (mix > 0)
            {
                const float mk = 1.0f / std::sqrt (g);
                for (int k = 0; k < 2; ++k)
                {
                    L2[k] += mix * (v.sat[0].process (L2[k] * g) * mk - L2[k]);
                    R2[k] += mix * (v.sat[1].process (R2[k] * g) * mk - R2[k]);
                }
            }
            float L, R;
            v.dec.process (L2, R2, L, R);
            const float oct = aCut[i] + aEnvAmt[i] * fe + keyTrack * (p - 60.0f) / 12.0f + velOct + (nSlots ? mod[(int) ModDst::cutoff] : 0.0f);
            v.svf.setCoeffs (std::exp2 (oct), nSlots ? std::fmax (0.0f, std::fmin (1.0f, aReso[i] + mod[(int) ModDst::reso])) : aReso[i], sr);
            L = v.svf.process (0, L, fm); R = v.svf.process (1, R, fm);
            if (v.clickAmp > 1e-5f)
            {   // click: white noise burst through a one-pole high-pass, after the filter
                const float x = v.clickAmp * v.driftRng.uni();
                v.clickY = v.clickHpA * (v.clickY + x - v.clickX); v.clickX = x;
                v.clickAmp *= v.clickMul;
                L += v.clickY; R += v.clickY;
            }
            float gain = amp * v.velGain * driftGain;
            if (nSlots)
            {
                gain *= std::fmax (0.0f, 1.0f + mod[(int) ModDst::amp]);
                const float pn = std::fmax (-1.0f, std::fmin (1.0f, mod[(int) ModDst::pan]));
                if (pn != 0) { L *= std::fmin (1.0f, 1.0f - pn); R *= std::fmin (1.0f, 1.0f + pn); }
            }
            if (v.fading)
            {
                v.fadeGain -= v.fadeStep;
                if (--v.fadeRemain <= 0 || v.fadeGain <= 0) { v.fadeGain = 0; }
                gain *= v.fadeGain;
            }
            L *= gain; R *= gain;
            if (! std::isfinite (L) || ! std::isfinite (R)) { bad = true; break; }
            mixL[i] += L; mixR[i] += R;
            if (v.fading && v.fadeGain <= 0) { v.reset(); return; }
            if (! v.aenv.active()) { v.reset(); return; }
        }
        if (bad) { ++stats.nonFiniteResets; v.reset(); }
    }
};

} // namespace svl

#include "SvlDrums.h"

namespace svl {

inline Engine::Engine() = default;
inline Engine::~Engine() = default;

inline void Engine::prepareDrums (double sampleRate)
{
    if (! drums) drums = std::make_unique<DrumMachine>();
    drums->setTempo (tempo);
    drums->prepare (sampleRate);
}
inline void Engine::setDrumTrackSound (int track, const ParamSet& sound, float gateSeconds)
{
    if (! drums) drums = std::make_unique<DrumMachine>();
    drums->setTrackSound (track, sound, gateSeconds);
}
inline void Engine::clearDrumTrackSound (int track) { if (drums) drums->clearTrackSound (track); }
inline int Engine::drumStep() const { return drums ? drums->currentStep() : -1; }
inline void Engine::drumsTempo() { if (drums) drums->setTempo (tempo); }
inline void Engine::drumsTransport (bool hasHost, bool playing, double ppq) { if (drums) drums->setTransport (hasHost, playing, ppq); }
inline void Engine::setPreview (bool seq, bool drumsToo) { gen.setPreview (seq); if (drums) drums->setPreview (drumsToo); }
inline bool Engine::previewing (bool drumsUnit) const { return drumsUnit ? (drums && drums->previewing()) : gen.previewing(); }
inline void Engine::drumsParams() { if (drums) drums->setParams (params); }
inline bool Engine::drumsService (int, int)
{
    if (! drums || ! drums->active()) return false;
    for (int guard = 0; guard < 4 && drums->samplesToNext() == 0; ++guard) drums->fireDue();
    return true;
}
inline int Engine::drumsNext() { return std::max (1, drums->samplesToNext()); }
inline void Engine::drumsAdvance (int n) { drums->advance (n); }
inline void Engine::drumsRender (float* L, float* R, int n) { drums->render (L, R, n); }
inline bool Engine::drumsKeys() const { return drums && drums->keysOn(); }
inline void Engine::drumsHit (int track, float vel) { if (drums) drums->hit (track, vel); }
inline void Engine::drumsAllOff() { if (drums) drums->allOff(); }
inline bool Engine::drumsTakeKeys() const { return drums && drums->holdsKeys(); }
inline void Engine::drumsKey (const Event& ev)
{
    if (! drums) return;
    if (ev.type == Event::on) drums->keyDown (ev.id); else if (ev.type == Event::off) drums->keyUp (ev.id);
}

} // namespace svl
