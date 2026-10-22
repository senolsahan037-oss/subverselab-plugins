// JING FANG arpeggiator + 16-step sequencer: a note generator in front of the voices. Plain C++17, no JUCE, no allocation.
//
// Clock. While the host transport plays, steps sit on the host's beat grid (step k starts at beat k x step, so a pattern
// lines up with the bar); otherwise an own clock at the host / set tempo (120 BPM when nothing sets it) starts at 0 when the
// first key goes down (or when a free sequence is switched on), so the first step plays at once; keys that come back within a
// bar of the last stop keep the pattern's place in the bar instead (short detached notes still reach the off-beat steps).
// Swing delays every second
// step by swing x half a step. Onsets land on their exact sample (the first sample at or after the grid time).
// A key pressed while the host plays: if the current step began less than 40 ms (and less than a quarter step) ago, it plays
// late at once; otherwise the next step plays it.
//
// Sources. arp: the held keys (latched with arp.latch), ordered by mode, over arp.octaves. seq hold: the last held key
// transposes the pattern (step note 0 = that key). seq free: plays from MIDI 60 without keys; with a host transport present
// it plays only while the host plays. arp + seq: each seq step that is on takes the next arp note and adds its own note;
// with arp mode "chord", each step that is on plays every held key (a gated chord).
//
// daw.seq = midi: the arp and the seq are off (the DAW's clip already holds their notes). In a host with the transport stopped,
// PLAY previews a free sequence on the own clock (setPreview); when the host starts it carries on on the host's grid without a
// cut, when the host stops it stops (RU-8's DAW rules).
//
// Notes. Every generated note gets its own id and a scheduled note-off (gate x the step's length, swing included), so nothing
// can hang: switching the arp / seq off, all-notes-off and a host stop with a free seq end what sounds at once.
// At one sample, note-ons come before note-offs, so a gate of 1 or more overlaps the next note (legato in mono / legato).
#pragma once
#include "SvlParams.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace svl {

constexpr int kGenIdBase = 1 << 20;   // generated note ids (host keys are 0..127, the panel's row keys 2000..2383)

struct GenNote
{
    enum Kind : int { on, off, slide } kind = on;
    int id = 0;
    int key = -1;        // MIDI key, resolved through the tuning when the note starts; -1: use pitch
    float pitch = 0;     // semitones before the reference offset, when key < 0
    float add = 0;       // semitones added after the key is resolved (seq rows)
    float vel = 1;
    int prevId = -1;     // slide: the generated note that glides into this one
};

class NoteGen
{
public:
    void prepare (double sampleRate) { sr = sampleRate; spb = sr * 60.0 / tempo; reset(); }

    void reset()
    {
        numHeld = numLatched = numOffs = numQ = numSubs = 0;
        running = false; setBeat (0); nextStep = 0; stoppedBeat = -1.0e301; arpPos = 0; chordCycle = 0; lastOnId = -1; pressCount = 0; slidePending = false;
        hostPlaying = false; preview = false;
    }

    void setParams (const ParamSet& p)
    {
        const bool wasActive = active();
        const bool midiSrc = p[SEQ_SOURCE] > 0.5f;
        arpOn = p[ARP_ON] > 0.5f && ! midiSrc; seqOn = p[SEQ_ON] > 0.5f && ! midiSrc;
        arpMode = (int) p[ARP_MODE]; arpOct = std::max (1, std::min (4, (int) std::lround (p[ARP_OCT])));
        arpGate = p[ARP_GATE]; latch = p[ARP_LATCH] > 0.5f;
        arpChord = (int) p[ARP_CHORD];
        seqLen = std::max (1, std::min (kSeqSteps, (int) std::lround (p[SEQ_LEN])));
        seqFree = p[SEQ_MODE] > 0.5f;
        rowUp = p[ROW_UP] * 12.0f / p[TUNE_EDO]; rowDown = p[ROW_DOWN] * 12.0f / p[TUNE_EDO];
        seqDir = (int) p[SEQ_DIR];
        for (int s = 0; s < kSeqSteps; ++s)
        {
            Step& st = steps[(size_t) s];
            st.prob = p[stepProbParam (s)]; st.ratchet = std::max (1, std::min (4, (int) std::lround (p[stepRatchetParam (s)])));
            st.on = p[stepParam (s, S_ON)] > 0.5f; st.note = (int) std::lround (p[stepParam (s, S_NOTE)]);
            st.row = (int) p[stepParam (s, S_ROW)]; st.vel = p[stepParam (s, S_VEL)]; st.gate = p[stepParam (s, S_GATE)];
            st.slide = p[stepParam (s, S_SLIDE)] > 0.5f; st.accent = p[stepParam (s, S_ACCENT)] > 0.5f;
        }
        const double len = kStepRateBeats[seqOn ? (int) p[SEQ_RATE] : (int) p[ARP_RATE]];
        const double sw = seqOn ? p[SEQ_SWING] : p[ARP_SWING];
        if (len != stepLen || sw != swing) { stepLen = len; swing = sw; if (running) nextStep = firstStepAtOrAfter (beat()); }
        if (! latch) numLatched = 0;
        if (wasActive && ! active()) { endAll(); running = false; }
        rebuild = true;
    }

    bool active() const { return arpOn || seqOn; }
    // the keys are its input: the arp, or a seq that follows the held key (HOLD). A FREE seq alone plays from C4 and ignores
    // keys, so the keys play the voices on top of it (owner 20 Oct: the plug-in "swallowed" the DAW's notes)
    bool takesKeys() const { return arpOn || (seqOn && ! seqFree); }
    bool needsService() const { return active() || numOffs > 0 || numQ > 0; }

    void setTempo (double bpm) { const double b = beat(); tempo = bpm; spb = sr * 60.0 / bpm; setBeat (b); }

    // once per host block, before process(): where the host transport is (ppq at the block's first sample)
    void setTransport (bool hasHostTransport, bool playing, double ppq)
    {
        hasHost = hasHostTransport;
        const bool was = hostPlaying;
        hostPlaying = hasHost && playing;
        if (hostPlaying && ! was) preview = false;   // the host takes over: the preview carries on on its grid, the host's stop ends it
        if (hostPlaying)
        {
            const bool jump = ! was || std::fabs (ppq - beat()) > 1.0e-3;
            setBeat (ppq);
            if (jump && running) { nextStep = firstStepAtOrAfter (ppq); if (! was || std::fabs (ppq - beat()) > 1.0e-3) numSubs = 0; }
        }
        else if (was && seqOn && seqFree && ! arpOn) { endAll(); running = false; }   // host stopped: a free sequence stops
    }

    // ---- keys (only while active())
    void keyOn (int id, int key, float pitch, float vel)
    {
        removeHeld (id);
        const bool fresh = numHeld == 0;
        if (numHeld < kMaxKeys) held[(size_t) numHeld++] = { id, key, pitch, vel, pressCount++ };
        if (latch)
        {
            if (fresh) { numLatched = 0; arpPos = 0; chordCycle = 0; }   // a new chord after all keys were up replaces the latched one
            if (numLatched < kMaxKeys) latched[(size_t) numLatched++] = held[(size_t) numHeld - 1];
        }
        rebuild = true;
        if (! running) startRunning();
    }
    void keyOff (int id) { removeHeld (id); rebuild = true; }
    // PLAY while a host is stopped: a free sequence runs on the own clock until the host starts (then the host's grid) or stops
    void setPreview (bool on) { preview = on && hasHost && ! hostPlaying; if (! preview && running && seqFree && hasHost && ! hostPlaying) { endAll(); running = false; } }
    bool previewing() const { return preview; }
    void allOff() { numHeld = numLatched = 0; rebuild = true; endAll(); running = false; }

    // ---- scheduling (the engine renders up to the next due sample, pops what is due, advances)
    int samplesToNext()
    {
        if (numQ > 0) return 0;
        updateRunning();
        double best = 1 << 30;
        for (int i = 0; i < numOffs; ++i) best = std::min (best, (double) offs[(size_t) i].left);
        if (running) best = std::min (best, std::max (0.0, samplesUntil (onsetBeat (nextStep))));
        for (int i = 0; i < numSubs; ++i) best = std::min (best, std::max (0.0, samplesUntil (subs[(size_t) i].beat)));
        return (int) std::max (0.0, best);
    }

    bool pop (GenNote& out)
    {
        if (numQ == 0)
        {
            updateRunning();
            if (running && samplesUntil (onsetBeat (nextStep)) <= 0) fireStep();
            for (int i = 0; i < numSubs && numQ < kMaxQ - 4;)
                if (samplesUntil (subs[(size_t) i].beat) <= 0)
                {
                    const Sub sb = subs[(size_t) i];
                    subs[(size_t) i] = subs[(size_t) --numSubs];
                    emitOn (sb.key, sb.semis, sb.add, sb.vel, sb.gateBeats, false);
                }
                else ++i;
            if (numQ == 0)
                for (int i = 0; i < numOffs; ++i)
                    if (offs[(size_t) i].left <= 0)
                    {
                        GenNote g; g.kind = GenNote::off; g.id = offs[(size_t) i].id;
                        offs[(size_t) i] = offs[(size_t) --numOffs];
                        if (g.id == lastOnId) lastOnId = -1;
                        out = g; return true;
                    }
        }
        if (numQ == 0) return false;
        out = q[(size_t) qHead]; qHead = (qHead + 1) % kMaxQ; --numQ;
        return true;
    }

    void advance (int n)
    {
        clk += n;
        for (int i = 0; i < numOffs; ++i) offs[(size_t) i].left -= n;
    }

    // ---- inspection (tests, the panel)
    int currentStep() const { return running ? stepIndex (nextStep - 1) : -1; }
    bool isRunning() const { return running; }
    // the beat the own clock is at (a running unit's grid); the drum machine locks its start to it (owner 21 Oct: two PLAYs ran out of step)
    double beatNow() const { return beat(); }
    void setPeer (bool running, double beatOfPeer) { peerRunning = running; peerBeat = beatOfPeer; }
    int soundingNotes() const { return numOffs; }

private:
    static constexpr int kMaxKeys = 32, kMaxList = kMaxKeys * 5 * 4, kMaxOffs = 160, kMaxQ = 96;
    struct Key { int id = -1, key = -1; float pitch = 0, vel = 1; int64_t order = 0; };
    struct Step { bool on = true; int note = 0, row = 1; float vel = 0.8f, gate = 0.6f; bool slide = false, accent = false; float prob = 1; int ratchet = 1; };
    // the step a grid position k plays (direction lane); random and the probability are hashes of k, so they never depend on
    // the block size or on where a render starts, and a bar plays the same way every time it comes round
    static uint64_t mix64 (uint64_t x) { x += 0x9E3779B97F4A7C15ull; x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull; x = (x ^ (x >> 27)) * 0x94D049BB133111EBull; return x ^ (x >> 31); }
    int stepIndex (int64_t k) const
    {
        const int64_t L = seqLen;
        switch (seqDir)
        {
            case 1: return (int) (L - 1 - ((k % L) + L) % L);
            case 2: { if (L == 1) return 0; const int64_t P = 2 * L - 2, r = ((k % P) + P) % P; return (int) (r < L ? r : P - r); }
            case 3: return (int) (mix64 ((uint64_t) k * 2654435761u + 0x51u) % (uint64_t) L);
            default: return (int) (((k % L) + L) % L);
        }
    }
    bool stepPlays (int64_t k, float prob) const
    {
        if (prob >= 1.0f) return true;
        if (prob <= 0.0f) return false;
        return (double) (mix64 ((uint64_t) k ^ 0xA5A5A5A5u) >> 11) * (1.0 / 9007199254740992.0) < (double) prob;
    }
    // ratchet repeats waiting for their time (a step's 2nd..4th hit, for each chord key)
    struct Sub { double beat; Key key; int semis; float add, vel; double gateBeats; };
    static constexpr int kMaxSubs = 64;
    std::array<Sub, kMaxSubs> subs {}; int numSubs = 0;
    int seqDir = 0;
    struct Off { int id; int64_t left; };
    struct Elem { int k; int oct; };   // index into the sorted / played set, octave

    double onsetBeat (int64_t k) const { return (double) k * stepLen + ((k & 1) ? 0.5 * swing * stepLen : 0.0); }

    int64_t firstStepAtOrAfter (double b) const
    {
        int64_t k = (int64_t) std::floor (b / stepLen);
        if (k > 0 && onsetBeat (k - 1) >= b - 1.0e-9) --k;   // a swung step can sit after the next grid line
        while (onsetBeat (k) < b - 1.0e-9) ++k;
        return k;
    }

    const Key* set() const { return latch ? latched.data() : held.data(); }
    int setSize() const { return latch ? numLatched : numHeld; }

    bool hasSource() const
    {
        if (seqOn && seqFree) return ! hasHost || hostPlaying || preview;   // arp + seq or seq alone: PLAY runs without keys
        if (arpOn) return setSize() > 0;
        if (seqOn) return setSize() > 0;
        return false;
    }

    void updateRunning()
    {
        if (! running && hasSource()) startRunning();
        else if (running && ! hasSource()) { running = false; stoppedBeat = beat(); }   // what sounds ends on its own gate
    }

    void startRunning()
    {
        if (! hasSource()) return;
        running = true; arpPos = 0; chordCycle = 0;
        if (hostPlaying)
        {   // grid-locked: the current step if it began a moment ago, else the next one
            const double b = beat();
            const int64_t k = firstStepAtOrAfter (b);
            const double catchBeats = std::min (0.04 * tempo / 60.0, 0.25 * stepLen);
            nextStep = (k > 0 && b - onsetBeat (k - 1) < catchBeats) ? k - 1 : k;
        }
        else if (peerRunning)
        {   // own clock, the drum machine already runs: join its grid at the next step, so the two never drift apart
            setBeat (peerBeat);
            nextStep = firstStepAtOrAfter (peerBeat);
        }
        else if (stoppedBeat > -1.0e300 && beat() - stoppedBeat < 4.0)
        {   // own clock, restarted within a bar of the last stop: keep the pattern's place in the bar (short, detached keys
            // still reach the off-beat steps), the step that began a moment ago plays late at once
            const double b = beat();
            const int64_t k = firstStepAtOrAfter (b);
            const double catchBeats = std::min (0.04 * tempo / 60.0, 0.25 * stepLen);
            nextStep = (k > 0 && b - onsetBeat (k - 1) < catchBeats) ? k - 1 : k;
        }
        else { setBeat (0); nextStep = 0; }
        stoppedBeat = -1.0e301;
    }

    void endAll() { for (int i = 0; i < numOffs; ++i) offs[(size_t) i].left = 0; numSubs = 0; }

    void removeHeld (int id)
    {
        int j = 0;
        for (int i = 0; i < numHeld; ++i) if (held[(size_t) i].id != id) held[(size_t) j++] = held[(size_t) i];
        numHeld = j;
    }

    static float sortPitch (const Key& k) { return k.key >= 0 ? (float) k.key : k.pitch; }

    void buildList()
    {
        rebuild = false;
        // the held keys, each expanded to the arp.chord shape (intervals in keys: they resolve through the tuning table)
        // PLAY (seq free) with no key held: the arp plays from the sequence's root, C4, as if that key were held (owner 19 Oct:
        // "sequencer play tuşu çalışmıyor" — an arp + seq preset ran its steps in silence, the arp had no keys)
        Key root; root.id = -2; root.key = 60; root.vel = 0.8f;
        const bool useRoot = seqOn && seqFree && setSize() == 0;
        const Key* src = useRoot ? &root : set(); const int nsrc = useRoot ? 1 : setSize();
        const int* shape = kArpChordShapes[arpChord]; const int nshape = kArpChordSize[arpChord];
        numExp = 0;
        for (int i = 0; i < nsrc; ++i)
            for (int c = 0; c < nshape && numExp < kMaxExp; ++c)
            {
                Key k = src[i];
                if (k.key >= 0)
                {
                    k.key = std::max (0, std::min (127, k.key + shape[c]));
                    bool dup = false;
                    for (int j = 0; j < numExp; ++j) dup |= exp[(size_t) j].key == k.key;
                    if (dup) continue;
                }
                else k.pitch += (float) shape[c];
                k.order = src[i].order * 8 + c;
                exp[(size_t) numExp++] = k;
            }
        const Key* s = exp.data(); const int n = numExp;
        numSorted = 0;
        for (int i = 0; i < n; ++i) sorted[(size_t) numSorted++] = i;
        const bool byOrder = arpMode == 6;   // played
        for (int i = 1; i < numSorted; ++i)   // insertion sort, stable
            for (int j = i; j > 0; --j)
            {
                const Key& a = s[sorted[(size_t) j - 1]]; const Key& b = s[sorted[(size_t) j]];
                const bool swap = byOrder ? a.order > b.order : sortPitch (a) > sortPitch (b);
                if (! swap) break;
                std::swap (sorted[(size_t) j - 1], sorted[(size_t) j]);
            }
        Elem up[kMaxList]; int nu = 0;
        for (int o = 0; o < arpOct; ++o) for (int i = 0; i < numSorted; ++i) up[nu++] = { sorted[(size_t) i], o };
        numList = 0;
        auto add = [&] (const Elem& e) { if (numList < kMaxList) list[(size_t) numList++] = e; };
        switch (arpMode)
        {
            case 1: for (int i = nu - 1; i >= 0; --i) add (up[i]); break;                                       // down
            case 2: for (int i = 0; i < nu; ++i) add (up[i]); for (int i = nu - 2; i >= 1; --i) add (up[i]); break;   // updown
            case 3: for (int i = nu - 1; i >= 0; --i) add (up[i]); for (int i = 1; i <= nu - 2; ++i) add (up[i]); break; // downup
            default: for (int i = 0; i < nu; ++i) add (up[i]); break;                                           // up, random, played
        }
    }

    // the next arp note; false when there is none
    bool nextArp (Key& out, int& octave)
    {
        if (rebuild) buildList();
        if (numList == 0) return false;
        int i;
        if (arpMode == 4) { rng = rng * 1664525u + 1013904223u; i = (int) ((rng >> 8) % (uint32_t) numList); }
        else i = (int) (arpPos++ % numList);
        out = exp[(size_t) list[(size_t) i].k]; octave = list[(size_t) i].oct;
        return true;
    }

    // semis: keys (a step of whatever tuning table is loaded); add: semitones after the key is resolved (rows, arp octaves:
    // an octave stays an octave in any table). holdOut: this note stays on into the next one (seq slide); the next note glides from it
    void emitOn (const Key& k, int semis, float add, float vel, double gateBeats, bool holdOut)
    {
        const bool slideIn = slidePending; slidePending = holdOut;
        GenNote g;
        g.id = kGenIdBase + (nextId++ & 0xFFFFF);
        if (k.key >= 0) { g.key = std::max (0, std::min (127, k.key + semis)); }
        else { g.key = -1; g.pitch = k.pitch + (float) semis; }
        g.add = add; g.vel = std::max (0.0f, std::min (1.0f, vel));
        if (slideIn && lastOnId >= 0)
            for (int i = 0; i < numOffs; ++i)
                if (offs[(size_t) i].id == lastOnId)
                {   // the previous note glides into this one: its own note-off is dropped, this note's off ends the voice
                    offs[(size_t) i] = offs[(size_t) --numOffs];
                    g.kind = GenNote::slide; g.prevId = lastOnId;
                    break;
                }
        push (g);
        // a note that has to reach the next onset (gate >= 1, slide) gets 2 samples more, so its off never lands one sample early
        const bool reaches = holdOut || gateBeats >= curDur - 1.0e-9;
        const int64_t L = std::max ((int64_t) 1, (int64_t) std::llround (gateBeats * spb) + (reaches ? 2 : 0));
        if (numOffs < kMaxOffs) offs[(size_t) numOffs++] = { g.id, L };
        else { GenNote o; o.kind = GenNote::off; o.id = g.id; push (o); }   // no room to schedule: do not let it hang
        lastOnId = g.id;
    }

    void push (const GenNote& g) { if (numQ < kMaxQ) { q[(size_t) ((qHead + numQ) % kMaxQ)] = g; ++numQ; } }

    void fireStep()
    {
        const int64_t k = nextStep++;
        const double dur = onsetBeat (k + 1) - onsetBeat (k);
        curDur = dur;
        if (seqOn)
        {
            const Step& st = steps[(size_t) stepIndex (k)];
            if (! st.on || ! stepPlays (k, st.prob)) { slidePending = false; return; }
            const float addRow = st.row == 0 ? -rowDown : (st.row == 2 ? rowUp : 0.0f);
            const float velS = st.vel + (st.accent ? 0.25f : 0.0f);
            // RATCHET: r hits inside the step, each a gate's share of 1 / r of it (never tied, no slide)
            const int r = st.ratchet;
            const double gateB = r > 1 ? std::min (0.95f, st.gate) * dur / r : (st.slide ? std::max (1.0f, st.gate) : st.gate) * dur;
            const double onset = onsetBeat (k);
            curDur = r > 1 ? dur / r : dur;
            auto hit = [&] (const Key& key, int semis, float add, float vel, bool slide)
            {
                emitOn (key, semis, add, vel, gateB, r > 1 ? false : slide);
                for (int i = 1; i < r && numSubs < kMaxSubs; ++i) subs[(size_t) numSubs++] = { onset + dur * i / r, key, semis, add, vel, gateB };
            };
            if (arpOn && arpMode == 5)
            {   // arp chord + seq: every step that is on plays all the keys, in the step's rhythm (a gated chord)
                if (rebuild) buildList();
                if (numSorted == 0) return;
                const int o = (int) (chordCycle++ % arpOct);
                for (int i = 0; i < numSorted; ++i) hit (exp[(size_t) sorted[(size_t) i]], st.note, addRow + 12.0f * (float) o, velS, false);
                return;
            }
            Key base; int oct = 0;
            if (arpOn) { if (! nextArp (base, oct)) return; }
            else if (seqFree) { base.key = 60; }
            else { if (setSize() == 0) return; base = set()[setSize() - 1]; }   // the last key pressed
            hit (base, st.note, addRow + 12.0f * (float) oct, velS, st.slide);
            return;
        }
        if (arpMode == 5)
        {   // chord: every key at once, one octave further each step
            if (rebuild) buildList();
            const int o = (int) (chordCycle++ % arpOct);
            for (int i = 0; i < numSorted; ++i) { const Key& kk = exp[(size_t) sorted[(size_t) i]]; emitOn (kk, 0, 12.0f * (float) o, kk.vel, arpGate * dur, false); }
            return;
        }
        Key key; int oct;
        if (nextArp (key, oct)) emitOn (key, 0, 12.0f * (float) oct, key.vel, arpGate * dur, false);
    }

    double sr = 44100, tempo = 120, spb = 44100 * 0.5;
    // the clock: beat = base + clk / spb, with clk counted in whole samples, so onsets never drift (no running sum of fractions)
    double base = 0, stoppedBeat = -1.0e301; int64_t clk = 0;   // stoppedBeat: where the own clock last stopped (very low: never)
    double beat() const { return base + (double) clk / spb; }
    void setBeat (double b) { base = b; clk = 0; }
    double samplesUntil (double b) const { return std::ceil ((b - base) * spb - 1.0e-6) - (double) clk; }
    double stepLen = 0.25, swing = 0;
    int64_t nextStep = 0, arpPos = 0, chordCycle = 0, pressCount = 0;
    bool arpOn = false, seqOn = false, latch = false, seqFree = false, running = false, rebuild = true;
    bool hasHost = false, hostPlaying = false, preview = false;
    bool peerRunning = false; double peerBeat = 0;
    int arpMode = 0, arpOct = 1, seqLen = 16, arpChord = 0;
    float arpGate = 0.6f, rowUp = 0, rowDown = 0;
    std::array<Step, kSeqSteps> steps {};
    std::array<Key, kMaxKeys> held {}, latched {};
    int numHeld = 0, numLatched = 0;
    static constexpr int kMaxExp = kMaxKeys * 5;
    std::array<Key, kMaxExp> exp {}; int numExp = 0;   // held keys x arp.chord shape
    std::array<int, kMaxExp> sorted {}; int numSorted = 0;
    std::array<Elem, kMaxList> list {}; int numList = 0;
    std::array<Off, kMaxOffs> offs {}; int numOffs = 0;
    std::array<GenNote, kMaxQ> q {}; int qHead = 0, numQ = 0;
    int lastOnId = -1; uint32_t nextId = 0, rng = 0x5eed1234u;
    bool slidePending = false; double curDur = 0.25;
};

} // namespace svl
