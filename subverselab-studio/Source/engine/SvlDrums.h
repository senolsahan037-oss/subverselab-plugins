// JING FANG drum machine: 8 tracks x 16 steps. Each track is a full drum sound (a drum preset's values, run by its own
// svl::Engine without FX), hit on the step grid; the tracks mix in after master.level and before the FX rack, so drive /
// echo / reverb apply to them. Plain C++17, no JUCE. Included by SvlEngine.h after class Engine.
//
// Clock: as the seq (SvlArpSeq.h): the host's beat grid while the transport plays (step k at beat k x step, swing delays
// every second step), otherwise an own clock from 0 when drums.on goes on; with a host transport present and stopped, the
// drum machine is silent. Hits land on their exact sample.
// A hit: note 60 at velocity 0.7 (accent 1.0); its note-off follows after the track's gate (by the sound's family: kick,
// snare, clap 0.2 s, perc 0.1 s, hat 0.05 s, as the drum demo phrases) x drums.tN.decay. A new hit on a track first ends the
// previous note. Tune: drum.pitch + tune for drum-mode sounds, else the note is moved. Decay scales the amp decay / release,
// the oscillator decays and the string decay of the sound.
#pragma once
#include <string>

namespace svl {

// the default kit: drum presets by "Family/Name" (the plug-in and svl resolve these from presets/)
inline const char* const kDefaultDrumKit[kDrumTracks] = {
    "Drums - Kick/Thump Kick", "Drums - Snare/Crack Snare", "Drums - Clap/House Clap", "Drums - Hat/Tight Hat",
    "Drums - Hat/Open Hat", "Drums - Perc/Low Tom", "Drums - Perc/Conga", "Drums - Perc/Shaker" };

// note length of a hit, by the drum preset's family (the length its sound was measured with)
inline float drumGateSeconds (const std::string& family)
{
    if (family.find ("Hat") != std::string::npos) return 0.05f;
    if (family.find ("Perc") != std::string::npos) return 0.1f;
    return 0.2f;
}

class DrumMachine
{
public:
    DrumMachine()
    {
        for (auto& b : base) b = ParamSet();
        for (auto& g : gate) g = 0.2f;
        for (auto& h : has) h = false;
    }

    void prepare (double sampleRate)
    {
        sr = sampleRate; spb = sr * 60.0 / tempo;
        for (int t = 0; t < kDrumTracks; ++t) { voice[(size_t) t].prepareTrack (sampleRate); applyTrack (t, true); }
        running = false; setBeat (0); nextStep = 0; numHeld = 0; stoppedBeat = -1.0e301;
        for (auto& o : offLeft) o = -1;
    }

    // the sound of a track: a drum preset's values and its note length (drumGateSeconds of its family). Not real-time safe
    // only in that it copies a parameter set; call it from the audio thread's parameter handoff, as setParams.
    void setTrackSound (int t, const ParamSet& p, float gateSeconds)
    {
        if (t < 0 || t >= kDrumTracks) return;
        base[(size_t) t] = p; gate[(size_t) t] = gateSeconds; has[(size_t) t] = true;
        applyTrack (t, true);
    }
    void clearTrackSound (int t) { if (t >= 0 && t < kDrumTracks) { has[(size_t) t] = false; voice[(size_t) t].allNotesOff (0); } }
    bool hasTrackSound (int t) const { return t >= 0 && t < kDrumTracks && has[(size_t) t]; }

    void setParams (const ParamSet& p)
    {
        const bool was = on;
        midiSrc = p[DRUMS_SOURCE] > 0.5f;              // daw.drums = midi: the DAW's notes 36..43 hit the tracks, the pattern is silent
        on = p[DRUMS_ON] > 0.5f || midiSrc;
        len = std::max (1, std::min (kDrumSteps, (int) std::lround (p[DRUMS_LEN])));
        keys = p[DRUMS_KEYS] > 0.5f || midiSrc;
        holdMode = p[DRUMS_MODE] < 0.5f;
        const float oct = 12.0f * (float) std::lround (p[DRUMS_OCT]);
        const bool octChanged = oct != octSemis; octSemis = oct;
        masterGain = p[DRUMS_LEVEL] <= -59.9f ? 0.0f : std::pow (10.0f, p[DRUMS_LEVEL] / 20.0f);
        const double l = kStepRateBeats[(int) p[DRUMS_RATE]], sw = p[DRUMS_SWING];
        if (l != stepLen || sw != swing) { stepLen = l; swing = sw; if (running) nextStep = firstStepAtOrAfter (beat()); }
        for (int t = 0; t < kDrumTracks; ++t)
        {
            float tr[kDrumTrackFields];
            for (int f = 0; f < kDrumTrackFields; ++f) tr[f] = p[drumTrackParam (t, (DrumTrackField) f)];
            const bool soundChanged = octChanged || tr[DT_TUNE] != tune[(size_t) t] || tr[DT_DECAY] != decay[(size_t) t];
            tune[(size_t) t] = tr[DT_TUNE]; decay[(size_t) t] = tr[DT_DECAY];
            mute[(size_t) t] = tr[DT_MUTE] > 0.5f;
            const float g = tr[DT_LEVEL] <= -59.9f ? 0.0f : std::pow (10.0f, tr[DT_LEVEL] / 20.0f);
            const float a = 0.25f * (float) kPi * (std::max (-1.0f, std::min (1.0f, tr[DT_PAN])) + 1.0f);   // equal power, 0 dB at centre
            gainL[(size_t) t] = g * std::cos (a) * (float) kSqrt2; gainR[(size_t) t] = g * std::sin (a) * (float) kSqrt2;
            for (int s = 0; s < kDrumSteps; ++s) steps[(size_t) t][(size_t) s] = (int) p[drumStepParam (t, s)];
            if (soundChanged) applyTrack (t, false);
        }
        if (was && ! on) running = false;   // what sounds ends on its own gate
    }

    void setTempo (double bpm)
    {
        const double b = beat(); tempo = bpm; spb = sr * 60.0 / bpm; setBeat (b);
        for (auto& v : voice) v.setTempo (bpm);   // synced LFOs inside a drum sound
    }
    void setTransport (bool hasHostTransport, bool playing, double ppq)
    {
        hasHost = hasHostTransport;
        const bool was = hostPlaying;
        hostPlaying = hasHost && playing;
        if (hostPlaying && ! was) preview = false;
        if (hostPlaying)
        {
            const bool jump = ! was || std::fabs (ppq - beat()) > 1.0e-3;
            setBeat (ppq);
            if (jump && running) nextStep = firstStepAtOrAfter (ppq);
        }
        else if (was) running = false;
    }

    bool active() const { return on || anySounding(); }
    bool keysOn() const { return on && keys; }
    // drums.on + hold: a played key only starts / keeps / stops the pattern; it does not play the synth or the arp / seq
    bool holdsKeys() const { return on && holdMode && ! midiSrc; }
    // PLAY while a host is stopped: a free beat runs on the own clock until the host starts (then its grid) or stops
    void setPreview (bool p) { preview = p && hasHost && ! hostPlaying; }
    bool previewing() const { return preview; }

    // a hit from a key (drums.keys): at the start of the current chunk
    void hit (int t, float vel) { if (t >= 0 && t < kDrumTracks) trigger (t, vel); }

    int samplesToNext()
    {
        updateRunning();
        double best = 1 << 30;
        for (int t = 0; t < kDrumTracks; ++t) if (offLeft[(size_t) t] >= 0) best = std::min (best, (double) offLeft[(size_t) t]);
        if (running) best = std::min (best, std::max (0.0, samplesUntil (onsetBeat (nextStep))));
        return (int) std::max (0.0, best);
    }

    // what is due at this sample: note-offs first (a track's previous note), then the step's hits
    void fireDue()
    {
        for (int t = 0; t < kDrumTracks; ++t)
            if (offLeft[(size_t) t] == 0) { voice[(size_t) t].noteOff (60, 0); offLeft[(size_t) t] = -1; }
        updateRunning();
        if (running && samplesUntil (onsetBeat (nextStep)) <= 0)
        {
            const int64_t k = nextStep++;
            const int s = (int) (((k % len) + len) % len);
            current = s;
            for (int t = 0; t < kDrumTracks; ++t)
            {
                const int st = steps[(size_t) t][(size_t) s];
                if (st > 0 && ! mute[(size_t) t]) trigger (t, st == 2 ? 1.0f : 0.7f);
            }
        }
    }

    // adds the tracks into L / R (n <= 64)
    void render (float* L, float* R, int n)
    {
        float l[64], r[64];
        for (int t = 0; t < kDrumTracks; ++t)
        {
            Engine& v = voice[(size_t) t];
            if (! has[(size_t) t]) continue;   // a track with a sound always runs: skipping it while idle would freeze its DC blocker
                                                // at a block-size-dependent sample (the output must not depend on the block size)
            v.process (l, r, n);
            const float gl = gainL[(size_t) t] * masterGain, gr = gainR[(size_t) t] * masterGain;
            for (int i = 0; i < n; ++i) { L[i] += gl * l[i]; R[i] += gr * r[i]; }
        }
    }

    void advance (int n)
    {
        clk += n;
        for (auto& o : offLeft) if (o > 0) o = std::max ((int64_t) 0, o - n);
    }

    int currentStep() const { return running ? current : -1; }
    bool isRunning() const { return running; }
    double beatNow() const { return beat(); }
    void setPeer (bool running, double beatOfPeer) { peerRunning = running; peerBeat = beatOfPeer; }
    int soundingTracks() const { int c = 0; for (int t = 0; t < kDrumTracks; ++t) c += ! voice[(size_t) t].idle(); return c; }
    void allOff() { numHeld = 0; for (int t = 0; t < kDrumTracks; ++t) { voice[(size_t) t].allNotesOff (0); offLeft[(size_t) t] = -1; } }

    // keys that hold the pattern (drums.mode hold); every played key counts, the synth still plays it
    void keyDown (int id)
    {
        for (int i = 0; i < numHeld; ++i) if (heldIds[(size_t) i] == id) return;
        if (numHeld < (int) heldIds.size()) heldIds[(size_t) numHeld++] = id;
    }
    void keyUp (int id)
    {
        int j = 0;
        for (int i = 0; i < numHeld; ++i) if (heldIds[(size_t) i] != id) heldIds[(size_t) j++] = heldIds[(size_t) i];
        numHeld = j;
    }

private:
    static constexpr double kSqrt2 = 1.4142135623730951;

    bool anySounding() const { for (int t = 0; t < kDrumTracks; ++t) if (offLeft[(size_t) t] >= 0 || ! voice[(size_t) t].idle()) return true; return false; }

    void trigger (int t, float vel)
    {
        if (! has[(size_t) t]) return;
        Engine& v = voice[(size_t) t];
        if (offLeft[(size_t) t] >= 0) v.noteOff (60, 0);
        v.noteOnPitch (notePitch[(size_t) t], vel, 60, 0);
        offLeft[(size_t) t] = std::max ((int64_t) 1, (int64_t) std::llround (gate[(size_t) t] * decay[(size_t) t] * sr));
    }

    // the track's engine values: the sound with its FX, arp / seq / drums off, tune and decay applied
    void applyTrack (int t, bool snap)
    {
        ParamSet p = base[(size_t) t];
        for (int i : { (int) FXD_AMOUNT, (int) FXE_MIX, (int) FXR_MIX, (int) ARP_ON, (int) SEQ_ON, (int) DRUMS_ON }) p[i] = 0;
        const float d = decay[(size_t) t];
        p[AENV_D] *= d; p[AENV_R] *= d;
        for (int o = 0; o < kNumOsc; ++o) { p[oscParam (o, O_DECAY)] *= d; p[oscParam (o, O_KS_DECAY)] *= d; }
        float pitch = 60;
        const float shift = tune[(size_t) t] + octSemis;   // drums.tN.tune + drums.octave (12 semitones per step)
        if (p[DRUM_MODE] > 0.5f) p[DRUM_PITCH] += shift; else pitch += shift;
        notePitch[(size_t) t] = pitch;
        voice[(size_t) t].setParams (p, snap);
    }

    // free: runs while drums.on (with a host transport present, only while it plays). hold: runs while a key is held; the
    // first key from silence starts at step 1 (own clock) or on the host's grid; a key back within a bar of the stop keeps
    // the bar position (as the seq). The step that began less than 40 ms (and a quarter step) ago plays late, at once.
    void updateRunning()
    {
        const bool src = on && ! midiSrc && (holdMode ? numHeld > 0 : (! hasHost || hostPlaying || preview));
        if (! running && src)
        {
            running = true;
            const double b = beat();
            const bool keepPlace = hostPlaying || (holdMode && b - stoppedBeat < 4.0);
            if (! hostPlaying && peerRunning)
            {   // own clock, the arp / seq already runs: join its grid at the next step (owner 21 Oct: two PLAYs ran out of step)
                setBeat (peerBeat);
                nextStep = firstStepAtOrAfter (peerBeat);
            }
            else if (keepPlace)
            {
                const int64_t k = firstStepAtOrAfter (b);
                const double catchBeats = std::min (0.04 * tempo / 60.0, 0.25 * stepLen);
                nextStep = (holdMode && k > 0 && b - onsetBeat (k - 1) < catchBeats) ? k - 1 : k;
            }
            else { setBeat (0); nextStep = 0; }
        }
        else if (running && ! src) { running = false; stoppedBeat = beat(); }
    }

    double onsetBeat (int64_t k) const { return (double) k * stepLen + ((k & 1) ? 0.5 * swing * stepLen : 0.0); }
    int64_t firstStepAtOrAfter (double b) const
    {
        int64_t k = (int64_t) std::floor (b / stepLen);
        while (onsetBeat (k) < b - 1.0e-9) ++k;
        return k;
    }
    double beat() const { return base0 + (double) clk / spb; }
    void setBeat (double b) { base0 = b; clk = 0; }
    double samplesUntil (double b) const { return std::ceil ((b - base0) * spb - 1.0e-6) - (double) clk; }

    std::array<Engine, kDrumTracks> voice;
    std::array<ParamSet, kDrumTracks> base;
    std::array<float, kDrumTracks> gate {}, tune {}, decay { 1, 1, 1, 1, 1, 1, 1, 1 }, gainL {}, gainR {}, notePitch {};
    std::array<bool, kDrumTracks> has {}, mute {};
    std::array<int64_t, kDrumTracks> offLeft { -1, -1, -1, -1, -1, -1, -1, -1 };
    std::array<std::array<int, kDrumSteps>, kDrumTracks> steps {};
    double sr = 44100, tempo = 120, spb = 22050, stepLen = 0.25, swing = 0, base0 = 0;
    int64_t clk = 0, nextStep = 0;
    int len = 16, current = -1;
    bool on = false, keys = false, running = false, hasHost = false, hostPlaying = false, holdMode = true, midiSrc = false, preview = false;
    std::array<int, 64> heldIds {}; int numHeld = 0;
    double stoppedBeat = -1.0e301;
    bool peerRunning = false; double peerBeat = 0;
    float masterGain = 1, octSemis = 0;
};

} // namespace svl
