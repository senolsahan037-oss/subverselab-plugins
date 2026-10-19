// SEDA BBD delay engine: plain C++17, no JUCE, header-only, no allocation in process() (all buffers are sized in prepare()).
// One file for the offline renderer, the measurements and the plug-in. SPEC.md section 1.
//
// Per channel:
//   in (+ feedback) -> pre-emphasis -> compressor (2:1 at COMPANDER 1) -> AA low-pass (8th order Butterworth = four 2nd-order sections, 0.38 f_clock; P1b, was 4th)
//   -> BBD line: N/2 samples at the clock rate (sample-and-hold, fractional resampling between host rate and clock rate)
//   -> box-averaged back to the host rate (+ clock leakage) -> reconstruction low-pass (4th order, 0.38 f_clock)
//   -> expander (1:2) -> de-emphasis = wet -> feedback path (tone tilt, tanh, level servo above 100 %) -> back to the input
//   wet -> soft ceiling at -3 dBFS -> MIX.
//
// Everything is per sample and depends only on the sample counter, never on the host block size (bit-identical at any block size).
//
// LISTEN (SPEC.md section 2, P1b): with Params::listen = false none of the code below touches the signal (bit-exact to the plain engine, L5).
// With it on, the layer moves INTERNAL values over the user's own settings and exposes every move through accessors (duckDb(), throwActive(),
// chosenNote(), guardActive(), sourceClass() ...) so the panel can draw them:
//   1. phrase duck (P1d): the repeats (wet output, not the feedback loop) sit down by up to 12 dB while the source plays and bloom in the gaps;
//      30 ms attack, 120 ms hold, 300 ms S-shaped release, so the duck moves like a hand on a fader and does not chatter between syllables;
//   2. phrase-end throw (P1d): after >= 1.2 s of material and a gap confirmed by max(250 ms, half the repeat time) (P3e; breath gaps are bridged, the returning source cancels the throw in 40 ms), the repeats rise towards +3 dB over their un-ducked level
//      (THROW = how much of the gap between the ducked level and +3 dB is filled) and FEEDBACK by up to +15 %, for one note value, then fall back over two;
//   3. density note value (SYNC on, Params::noteAuto): at bar lines only, a second line takes over the input with a short crossfade while the
//      old line rings out and fades: no pitch glide, no click;
//   4. feedback guard: wet level grows for 2 s with the input silent (P3e: the plateau trigger of P3d never fired and is gone) and FEEDBACK <= 100 %: the effective feedback is pulled down until it decays;
//   5. SOURCE selector (P1d, replaces SourceSense detection): OFF / VOCAL / DRUMS / BASS / KEYS loads the SPEC 2.5 starting point at once, for the knobs
//      the user has not touched (Params::touched). The engine never guesses the source. The SourceSense analyser is only used for the onset rate of 3.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include "../../subverselab-plugin-common/SourceSense/SourceSense.h"
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace seda
{
enum Age      : int { kNew = 0, kOld = 1 };                   // 8192 / 4096 stages (P1c: Memory Man class / worn, darker)
enum Stereo   : int { kMono = 0, kWide = 1, kPingPong = 2 };
enum LfoShape : int { kSine = 0, kTriangle = 1 };

constexpr int    kStages[2]      = { 8192, 4096 };
constexpr double kGainAttackSec  = 0.0005;   // P3c: the expander's limit follows the compressor gain smoothed (0.5 ms up, 2 ms down): a hard step in it showed as a click when TIME is automated (ControlProof check 3)
constexpr double kGainHoldSec    = 0.002;
constexpr int    kBufMax         = 16384;      // samples per channel: the longest chain (NEW 4 lines of 4096 samples, OLD 8 lines of 2048)
constexpr double kChainClockHz   = 10000.0;    // a line is chained to the next one when its clock would fall under this (SPEC section 1)
constexpr double kBwFactor[2]    = { 0.38, 0.19 };   // both low-passes sit at kBwFactor * f_clock: OLD is half of NEW (charge-transfer loss of a worn line)
constexpr double kMinTimeSec     = 0.020;
constexpr double kMaxTimeSec     = 1.2;
constexpr double kMaxClockHz     = 100000.0;
constexpr double kMinClockHz     = 100.0;
constexpr double kGlideOctPerSec = 10.0;       // 1 octave per 100 ms
constexpr double kModDepthMax    = 0.03;       // DEPTH 100 % = +-3 % time
constexpr double kDriftMax       = 0.003;      // +-0.3 % at DEPTH 100 %
constexpr double kWetCeilingDb   = -3.0;

// Note values, in quarter-note beats: index = 3 * base + {0 straight, 1 triplet, 2 dotted}; base 0 = 1/32 ... 5 = 1/1.
constexpr int kNumNotes = 18;
inline double noteBeats (int idx)
{
    static const double base[6] = { 0.125, 0.25, 0.5, 1.0, 2.0, 4.0 };
    idx = std::max (0, std::min (kNumNotes - 1, idx));
    const double mult[3] = { 1.0, 2.0 / 3.0, 1.5 };
    return base[idx / 3] * mult[idx % 3];
}
inline const char* noteName (int idx)
{
    static const char* n[kNumNotes] = { "1/32", "1/32t", "1/32d", "1/16", "1/16t", "1/16d", "1/8", "1/8t", "1/8d",
                                        "1/4", "1/4t", "1/4d", "1/2", "1/2t", "1/2d", "1/1", "1/1t", "1/1d" };
    return n[std::max (0, std::min (kNumNotes - 1, idx))];
}
inline int noteIndex (const char* s)   // -1 when unknown
{
    for (int i = 0; i < kNumNotes; ++i) if (std::strcmp (s, noteName (i)) == 0) return i;
    return -1;
}

struct Params
{
    float  timeMs   = 300.0f;    // 20 .. 1200 (used when sync is off)
    bool   sync     = false;
    int    note     = 11;        // index into the note table (1/4 dotted); used when sync is on
    double bpm      = 120.0;     // host tempo
    float  feedback = 0.3f;      // 0 .. 1.1
    float  mix      = 0.5f;      // 0 .. 1, 0 = dry only
    float  tone     = 0.0f;      // -1 .. 1 = feedback tilt -6 .. +6 dB (positive = brighter, darker at the low end is negative)
    float  rateHz   = 0.5f;      // 0.05 .. 8
    float  depth    = 0.0f;      // 0 .. 1 (1 = +-3 % time); also scales the slow drift
    int    age      = kNew;
    float  comp     = 1.0f;      // COMPANDER 0 (clean line) .. 1 (full NE570 2:1)
    int    stereo   = kMono;
    float  widthMs  = 0.0f;      // 0 .. 20, right channel time offset (WIDE)
    int    lfoShape = kSine;
    bool   bypass   = false;     // bit-exact copy
    // ---- LISTEN (P1b)
    bool   listen   = false;     // the interactive layer; false = plain engine, bit-exact
    float  duck     = 0.5f;      // 0 .. 1 = 0 .. 12 dB of repeat reduction while the source plays
    float  throwDb  = 0.0f;      // THROW 0 .. 12 (0 = no throw): fills throwDb / 12 of the gap between the ducked level and +3 dB at a phrase end; FEEDBACK rises by up to 15 % x throwDb / 12
    bool   noteAuto = false;     // NOTE = AUTO: with sync on, the note value follows the density of the source (bar lines only)
    float  wetHpHz  = 0.0f;      // wet high-pass, 0 = off (6 dB/oct, inside the feedback loop)
    int    source   = 0;         // SOURCE selector (Source enum): OFF = no starting point; the others load SPEC 2.5 for untouched knobs (needs listen)
    uint32_t touched = 0;        // bit mask of Touch values: knobs the user has moved; the SOURCE starting point leaves those alone
    int    beatsPerBar = 4;
    bool   hostBars = false;     // the host gave a position: hostPpq = quarter notes at the first sample of the block, hostBarStartPpq = quarter note of the bar start (bar lines then follow the host)
    double hostPpq = 0.0, hostBarStartPpq = 0.0;
    // ---- PERFORM (17 Oct 2026, owner): the pedal "losing control". Both 0 = the plain engine, bit-exact.
    float  runaway  = 0.0f;      // RUNAWAY 0 .. 1: loop gain pushed towards perform::kRunMaxGain over FEEDBACK, the loop darkens and hisses more as it goes
    float  chaos    = 0.0f;      // CHAOS 0 .. 1: 0 .. 0.5 smoothed random clock wobble (coupled to loop energy); 0.5 .. 1 also random pitch steps on beats
    bool   hostBeats = false;    // hostPpq is valid and the host is playing: CHAOS steps on the host's beats (else every delay time, counted from the start)
};
enum Source : int { kSrcOff = 0, kSrcVocal, kSrcDrums, kSrcBass, kSrcKeys, kNumSources };
inline const char* sourceName (int s) { static const char* n[kNumSources] = { "off", "vocal", "drums", "bass", "keys" }; return n[std::max (0, std::min ((int) kNumSources - 1, s))]; }
inline int sourceIndex (const char* s) { for (int i = 0; i < kNumSources; ++i) if (std::strcmp (s, sourceName (i)) == 0) return i; return -1; }
enum Touch : uint32_t { kTouchNote = 1, kTouchWetHp = 2, kTouchDuck = 4, kTouchThrow = 8, kTouchMix = 16, kTouchStereo = 32 };

// Event log of the internal moves (for sedaraw --eventlog and the panel); a ring that drops the oldest when nobody reads it.
enum EventType : int { kEvDuck = 0, kEvThrowOn, kEvThrowOff, kEvNote, kEvGuardOn, kEvGuardOff, kEvThrowCancel, kEvChaosStep, kNumEventTypes };
struct Event { uint64_t frame = 0; int type = 0; double a = 0, b = 0; };
inline const char* eventName (int t)
{
    static const char* n[kNumEventTypes] = { "duck", "throw_on", "throw_off", "note", "guard_on", "guard_off", "throw_cancel", "chaos_step" };
    return n[std::max (0, std::min ((int) kNumEventTypes - 1, t))];
}

// LISTEN constants. "SPEC" = SPEC.md section 2.
namespace listen
{
constexpr double kDuckMaxDb      = 12.0;    // P1d (SPEC 2.1 amended, was 18): DUCK 100 % = 12 dB
constexpr double kDuckAttackSec  = 0.030;   // P1d: the duck goes in over 30 ms (raised-cosine ramp), was 10 ms
constexpr double kDuckHoldSec    = 0.120;   // P1d: the duck stays in this long after the level falls under the gap threshold, so it does not chatter between syllables
constexpr double kDuckReleaseSec = 0.300;   // P1d: S-shaped (raised-cosine) release into the gap, 250 .. 400 ms; was a 60 ms one-pole
constexpr double kEnvSec         = 0.030;   // level follower of the playing / gap detector (power, one time constant); own choice: short enough for a 150 ms gap, long enough to ride over pitch periods down to 80 Hz
constexpr double kHystDb         = 4.0;     // gap threshold = playing threshold - 4 dB, so a decaying note does not chatter; own choice
constexpr double kLevelBlockSec  = 0.050;   // SPEC 2.1: 300 ms RMS = 6 blocks of 50 ms
constexpr int    kLevelBlocks    = 6;
constexpr double kHistMinDb      = -90.0;   // level histogram: 1 dB bins from here to 0 dBFS
constexpr double kHistFloorDb    = -60.0;   // frames under this are silence and are not counted in the statistics (SourceSense uses the same gate)
constexpr double kHistHalfLifeSec= 30.0;    // statistics forget with this half life; own choice (a section of a song)
constexpr double kThrDefaultDb   = -45.0;   // threshold until 20 frames (1 s) of statistics exist; own choice
constexpr int    kThrMinFrames   = 20;
constexpr double kThrOffsetDb    = -6.0;    // threshold = median of the 300 ms RMS (frames above -60 dBFS) + this. SPEC 2.1 says +6 dB over the median of ALL frames; that cannot work when the source plays more than half the time (the median is then the play level and +6 dB is above it), so the median is taken over audible frames and the offset is -6 dB. See STATUS.md P1b.
constexpr double kThrMinDb       = -55.0, kThrMaxDb = -6.0;   // clamp of the threshold
constexpr double kPhraseMinSec   = 1.200;   // P3e (SPEC 2.2 amended, was 400 ms): sustained material >= 1.2 s; a gap that is not confirmed as a phrase end (below) is bridged and counts as material
constexpr double kPhraseGapSec   = 0.250;   // P3e (was 150 ms): a throw is CONFIRMED by a gap that has lasted max(250 ms, half the current repeat time); breath gaps shorter than that never throw
constexpr double kReturnSec      = 0.020;   // P3e: the source is "back" after 20 ms above the threshold; a throw in progress is cancelled at once
constexpr double kThrowCancelSec = 0.040;   // P3e: the cancelled throw fades out linearly over 40 ms (the duck engages with its normal 30 ms attack)
constexpr double kThrowRiseSec   = 0.020;   // P1d: one-pole time constant of the throw edge: 95 % in 60 ms (was 10 ms)
constexpr double kThrowFbMax     = 0.15;    // P1d (SPEC 2.2 amended, was 25 %): FEEDBACK up to +15 %
constexpr double kThrowFbRel     = 0.08;    // P3e: the feedback lift is also at most 8 % of the user's FEEDBACK (+0.7 dB per repeat) and acts only during the hold
constexpr double kThrowCeilDb    = 3.0;     // P1d: the throw lifts the repeats to at most +3 dB over their un-ducked level
constexpr double kDenseHi        = 6.0;     // SPEC 2.3: onset rate > 6/s -> 1/8
constexpr double kSparseLo       = 2.0;     // SPEC 2.3: < 2/s -> 1/2; between: 1/4 dotted
constexpr double kDensityHyst    = 0.5;     // onsets/s of hysteresis around those limits once a choice is made; own choice
constexpr int    kNoteBusy = 6, kNoteMedium = 11, kNoteSparse = 12;   // note table indices: 1/8, 1/4 dotted, 1/2
constexpr double kXfadeSec       = 0.040;   // input crossfade between the two lines at a bar line; own choice
constexpr double kRetireSec      = 0.750;   // the old line's output fades out over this after the crossfade; own choice
constexpr double kCompFadeFb     = 0.01;    // T4: the compander is faded out between 100 % and 101 % FEEDBACK (smoothstep)
constexpr double kGuardBlockSec  = 0.100;   // wet / input level are read every 100 ms
constexpr double kGuardSilentDb  = -66.0;   // input under this (power, dBFS) counts as silent; own choice, 6 dB over the SourceSense frame gate
constexpr int    kGuardGrowBlocks= 20;      // SPEC 2.4: grows for 2 s
constexpr double kGuardStepDb    = 0.2;     // "grows" = +0.2 dB over each of the two 1 s halves; own choice
constexpr double kGuardMinWetDb  = -70.0;   // a loop quieter than this is not "ringing"
constexpr double kGuardPullDb    = -10.0;   // while the wet level falls by less than 10 dB per second the guard keeps pulling the feedback down; own choice (L4: 46 dB in under 6 s)
constexpr double kGuardPullStep  = 0.9;     // x0.9 per 100 ms block (-0.9 dB)
constexpr double kGuardMinGain   = 0.02;
constexpr double kGuardReleaseSec= 0.300;   // feedback comes back this fast once the input is back
constexpr double kSsHpVocal = 250.0, kSsHpDrums = 400.0, kSsHpBass = 150.0;   // SPEC 2.5 table
constexpr float  kSsDuckVocal = 0.6f, kSsDuckDrums = 0.3f, kSsThrowVocalDb = 6.0f, kSsMixCapBass = 0.2f;   // SPEC 2.5 table; THROW "on" = 6 dB (own choice)
constexpr int    kSsNoteVocal = 11, kSsNoteDrums = 3, kSsNoteSynth = 6;   // 1/4 dotted, 1/16, 1/8 (SOURCE selector starting points)
}

// RUNAWAY / CHAOS constants (17 Oct 2026, owner-approved sound design; STATUS.md "RUNAWAY and CHAOS").
namespace perform
{
constexpr double kRunMaxGain    = 1.20;    // loop gain at RUNAWAY 100 %, whatever FEEDBACK is: G = fb + (1.20 - fb) * smoothstep(RUNAWAY) (never lower than fb)
constexpr double kRunNoise      = 3.0;     // BBD hiss x (1 + 3 RUNAWAY): +12 dB at 100 %, so the loop can start from its own noise
constexpr double kRunDarkHiHz   = 6000.0;  // in-loop one-pole low-pass, mixed in by RUNAWAY; its corner falls from 6 kHz (RUNAWAY 0) to 2.2 kHz (RUNAWAY 100 %)
constexpr double kRunDarkLoHz   = 2200.0;
constexpr double kChaosWobMax   = 0.012;   // +-1.2 % of the delay time at CHAOS >= 50 % and a full loop (self-oscillation level)
constexpr double kChaosWobFloor = 0.30;    // share of the wobble that does not depend on the loop energy
constexpr double kChaosEnergyRef= 0.20;    // loop RMS (after the tanh) that counts as "full": the level servo's target
constexpr double kWowMinSec = 0.35, kWowMaxSec = 1.40, kFlutMinSec = 0.07, kFlutMaxSec = 0.22, kFlutShare = 0.30;   // two smoothstep-interpolated random walks
constexpr double kChaosStepFrom = 0.5;     // pitch steps above CHAOS 50 %
constexpr double kChaosProbMax  = 0.85;    // step probability per beat at CHAOS 100 % (linear from 0 at 50 %)
constexpr double kChaosGlideSec = 0.006;   // a step glides in 6 ms (smoothstep on the log of the clock)
constexpr double kChaosReturnSec= 0.030;   // CHAOS turned under 50 %: back to the user's clock in 30 ms
constexpr int    kChaosSemis[7] = { -12, -7, -5, 0, 5, 7, 12 };
constexpr uint64_t kChaosSeed   = 0x5EDA0C4A05ull;
}

class Engine
{
public:
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        for (int v = 0; v < 2; ++v)
            for (int c = 0; c < 2; ++c) { vo[v].ch[c].buf.assign ((size_t) kBufMax, 0.0); vo[v].ch[c].tmp.assign ((size_t) kBufMax, 0.0); vo[v].ch[c].gbuf.assign ((size_t) kBufMax, 1.0); vo[v].ch[c].gtmp.assign ((size_t) kBufMax, 1.0); vo[v].ch[c].dbuf.assign ((size_t) kBufMax, kDepthCurrent); }
        seedAll();
        glideStep = std::log (2.0) / (sr / kGlideOctPerSec);
        smoothCoef = 1.0 - std::exp (-1.0 / (0.02 * sr));
        wetSmooth = 1.0 - std::exp (-1.0 / (0.004 * sr)); hpSmooth = 1.0 - std::exp (-1.0 / (0.010 * sr)); hpGlast = std::tan (M_PI * 250.0 / sr);
        envAtt = envRel = 1.0 - std::exp (-1.0 / (0.008 * sr));   // one time constant both ways: the encoder and the decoder then read the same statistics and the pair stays level-transparent
        acCoef = std::exp (-2.0 * M_PI * kAcCouplingHz / sr);
        servoCoef = 1.0 - std::exp (-1.0 / (0.150 * sr)); servoRate = 0.7 / sr;
        const double kz = std::tan (M_PI * 1500.0 / sr), kp = std::tan (M_PI * 6000.0 / sr);   // pre-emphasis shelf: +12 dB above ~6 kHz
        eb0 = 1.0 + 1.0 / kz; eb1 = 1.0 - 1.0 / kz; ea0 = 1.0 + 1.0 / kp; ea1 = 1.0 - 1.0 / kp;
        toneG = std::tan (M_PI * 1000.0 / sr); toneA = 1.0 / (1.0 + toneG);
        darkKey = -1.0; setDark (0.0);
        ss.prepare (sr);
        evBuf.assign ((size_t) kEvCap, Event());
        reset();
    }

    void setParams (const Params& np) { p = np; }

    void seedAll()
    {
        for (int v = 0; v < 2; ++v)
            for (int c = 0; c < 2; ++c) vo[v].ch[c].seed ((0x9E3779B97F4A7C15ull * (uint64_t) (c + 1)) ^ (v ? 0xC2B2AE3D27D4EB4Full : 0ull));
    }

    void reset()
    {
        ageFade = 1.0; ageG = 1.0; ageFadeDir = 0; wetGs = 1.0; hpAmt = 0.0;
        age = p.age; stages = kStages[age] / 2;
        seedAll();   // noise and drift generators start from the fixed seeds again: a render after reset() is bit-identical to a fresh instance's
        for (auto& v : vo) { for (auto& c : v.ch) c.clear (stages); v.lnT = 0.0; v.inG = 1.0; v.outG = 1.0; v.retiring = false; }
        mainV = 0; xfadeT = -1.0; retireT = -1.0;
        snap = true; frame = 0; lfoPhase = 0.0;
        resetListen();
        resetPerform();
    }

    // ---- LISTEN read-outs (audio thread writes, UI thread reads: plain doubles for now, atomics when the processor exists)
    double duckDb()            const { return duckDbNow; }                 // <= 0: current reduction of the repeats
    bool   throwActive()       const { return tStage != 0; }
    double throwLiftDb()       const { return lastLiftDb; }                // >= 0: what the throw adds to the wet gain on top of the duck, in dB (P3e, for the picture of what LISTEN did)
    double throwAmount()       const { return tAmt; }                      // 0 .. 1
    int    chosenNote()        const { return noteEff; }                   // note table index in force (AUTO: the engine's choice)
    bool   crossfading()       const { return vo[mainV ^ 1].retiring; }
    bool   guardActive()       const { return guardOn; }
    double guardDb()           const { return 20.0 * std::log10 (std::max (1e-6, guardG)); }
    double thresholdDb()       const { return thrDb; }
    // PERFORM read-outs
    double loopGain()          const { return loopGainNow; }               // effective loop gain of the last sample (FEEDBACK, guard, throw, RUNAWAY): > 1 = the loop runs away
    int    chaosStepCount()    const { return chSteps; }                   // CHAOS pitch steps since reset()
    int    chaosSemitones()    const { return chSemi; }                    // the step target in force (0 = the user's clock)
    double chaosClockRatio()   const { return chMul > 0.0 ? 1.0 / chMul : 1.0; }   // clock factor CHAOS applies now (wobble and step), 1 = none
    double onsetRate()         const { return ss.onsetRateShort(); }
    /** P4b: peak of the wet signal after the duck and the throw (linear, before MIX) since the last call; the caller is the audio thread. */
    float  takeWetPeak()             { const float v = (float) wetPk; wetPk = 0.0; return v; }
    void   setEventLog (bool on) { evOn = on; evHead = evTail = 0; }
    bool   popEvent (Event& e) { if (evTail == evHead) return false; e = evBuf[(size_t) evTail]; evTail = (evTail + 1) % kEvCap; return true; }

    // The SOURCE selector's starting point (SPEC 2.5), loaded at once, no detection; a knob the user has touched keeps its own value.
    struct Effective { bool sync; int note; float wetHpHz, duck, throwDb, mixMax; int stereo; };
    Effective effective() const
    {
        Effective e { p.sync, p.note, p.wetHpHz, p.duck, p.throwDb, 1.0f, p.stereo };
        if (p.listen && p.source != kSrcOff)
        {
            int ovNote = -1, ovStereo = -1; float ovHp = -1.0f, ovDuck = -1.0f, ovThrow = -1.0f, ovMixMax = 1.0f;
            switch (p.source)
            {
                case kSrcVocal: ovNote = listen::kSsNoteVocal; ovHp = (float) listen::kSsHpVocal; ovDuck = listen::kSsDuckVocal; ovThrow = listen::kSsThrowVocalDb; break;
                case kSrcDrums: ovNote = listen::kSsNoteDrums; ovHp = (float) listen::kSsHpDrums; ovDuck = listen::kSsDuckDrums; break;
                case kSrcBass:  ovHp = (float) listen::kSsHpBass; ovMixMax = listen::kSsMixCapBass; break;
                case kSrcKeys:  ovNote = listen::kSsNoteSynth; ovStereo = kPingPong; break;
                default: break;
            }
            if (ovNote >= 0 && ! (p.touched & kTouchNote)) { e.note = ovNote; e.sync = true; }
            if (ovHp >= 0.0f && ! (p.touched & kTouchWetHp)) e.wetHpHz = ovHp;
            if (ovDuck >= 0.0f && ! (p.touched & kTouchDuck)) e.duck = ovDuck;
            if (ovThrow >= 0.0f && ! (p.touched & kTouchThrow)) e.throwDb = ovThrow;
            if (ovMixMax < 1.0f && ! (p.touched & kTouchMix)) e.mixMax = ovMixMax;
            if (ovStereo >= 0 && ! (p.touched & kTouchStereo)) e.stereo = ovStereo;
        }
        return e;
    }

    // inL/inR may alias outL/outR. Mono hosts: pass the same buffer for both.
    void process (const float* inL, const float* inR, float* outL, float* outR, int n)
    {
        if (p.bypass) { for (int i = 0; i < n; ++i) { const float l = inL[i], r = inR[i]; outL[i] = l; outR[i] = r; } return; }
        // AGE changes the line (its length, its filters): the old line is cleared. A cut would click, so the wet is faded out over 8 ms first, the line is switched at a block start
        // once it is silent, and faded back in (the dry path is never touched).
        if (p.age != age)
        {
            if (ageFadeDir >= 0) ageFadeDir = -1;
            if (ageFade <= 0.0) { age = p.age; stages = kStages[age] / 2; for (auto& v : vo) for (auto& c : v.ch) c.clear (stages); vo[mainV ^ 1].retiring = false; ageFadeDir = 1; }
        }
        else if (ageFadeDir < 0) ageFadeDir = 1;
        const bool L = p.listen;
        const Effective e = effective();
        const double ceil = std::pow (10.0, kWetCeilingDb / 20.0) * 0.9999;
        const double noiseAmp = age == kNew ? 1.5e-4 : 5.0e-4;
        const bool   wide = e.stereo != kMono;
        const double compE = -0.5 * (double) p.comp, compD = 0.5 * (double) p.comp / (1.0 - 0.5 * (double) p.comp);
        const bool   ping = e.stereo == kPingPong;
        const bool   autoNote = L && e.sync && p.noteAuto;
        if (! autoNote) noteEff = e.note;
        const double tMin = minTimeSec();
        const double bwk = kBwFactor[age];
        const double timeTarget = std::max (tMin, std::min (kMaxTimeSec,
                                    e.sync ? 60.0 / std::max (20.0, p.bpm) * noteBeats (noteEff) : (double) p.timeMs * 0.001));
        const double depth = std::max (0.0f, std::min (1.0f, p.depth));
        const double tgtFb = std::max (0.0f, std::min (1.1f, p.feedback)), tgtMix = std::max (0.0f, std::min (1.0f, std::min (p.mix, e.mixMax)));
        const double tgtTone = std::max (-1.0f, std::min (1.0f, p.tone)), tgtWidth = std::max (0.0f, std::min (20.0f, p.widthMs)) * 0.001;
        const bool   hpOn = L && e.wetHpHz > 0.0f;
        if (hpOn) hpGlast = std::tan (M_PI * std::min ((double) e.wetHpHz, 0.45 * sr) / sr);
        const double hpG = hpGlast, hpA = 1.0 / (1.0 + hpG);   // the filter keeps running (warm) when WET HP is off, so switching it on never meets a stale state
        const double duckAmt = L ? (double) std::max (0.0f, std::min (1.0f, e.duck)) : 0.0;
        const double throwDbE = L ? (double) std::max (0.0f, std::min (12.0f, e.throwDb)) : 0.0;
        const double duckDepthDb = listen::kDuckMaxDb * duckAmt;   // depth the repeats duck to while the source plays
        const double tgtRun = std::max (0.0f, std::min (1.0f, p.runaway)), tgtChaos = std::max (0.0f, std::min (1.0f, p.chaos));
        runTargetGain = tgtRun > 0.0 ? runGain (std::max (0.0f, std::min (1.1f, p.feedback)), tgtRun) : (double) p.feedback;   // the guard leaves a deliberate runaway alone
        if (tgtChaos <= perform::kChaosStepFrom && chTo != 0.0) startChaosGlide (0, perform::kChaosReturnSec);   // under 50 %: back to the user's clock
        if (snap)
        {
            hpAmt = hpOn ? 1.0 : 0.0; wetGs = 1.0;
            vo[mainV].lnT = std::log (timeTarget); fbS = tgtFb; mixS = tgtMix; toneS = tgtTone; widthS = tgtWidth; runS = tgtRun; chS = tgtChaos;
            for (int c = 0; c < 2; ++c) vo[mainV].ch[c].initDrift();
            snap = false;
        }
        double lnTarget = std::log (timeTarget);

        for (int i = 0; i < n; ++i)
        {
            const double dryIn[2] = { (double) inL[i], (double) inR[i] };

            // ---- LISTEN, per sample: source analysis, phrase logic, bar lines
            double wetG = 1.0, fbAdd = 0.0;
            if (L)
            {
                if (autoNote) { const float a = inL[i], b = inR[i]; ss.process (&a, &b, 1); }   // onset rate for the density note only
                listenInput (dryIn[0], dryIn[1], duckDepthDb, throwDbE);
                double gDb = duckDbCur;   // wet gain in dB: the duck, and the throw lifting it towards +3 dB by the filled share of the gap
                lastLiftDb = 0.0;
                if (tAmt > 0.0)
                {   // P3e: the duck overrides the throw: while the duck is engaged the throw's lift goes away with it (1 - duckS), so a throw can never hold the wet up over a new phrase
                    const double fill = tAmt * (throwDbE / 12.0) * (duckOn ? 1.0 - duckS : 1.0);
                    lastLiftDb = (listen::kThrowCeilDb - gDb) * fill; gDb += lastLiftDb;
                    // the feedback lift writes extra energy INTO the loop, and that energy is still there when the next phrase starts (it can only be ducked, not recalled): it is limited to the hold (one note value)
                    // and to +0.7 dB per repeat (x1.08 of the user's feedback), so the duck's 7.2 dB still leaves >= 6 dB under the plain engine at the next onset (L7)
                    if (tStage == 1) fbAdd = std::min (listen::kThrowFbMax * fill, listen::kThrowFbRel * fbS);
                }
                if (gDb != 0.0) wetG = std::exp (gDb * 0.11512925464970229);
            }
            // the output gain of the wet (duck, throw, AGE fade) moves in 4 ms, never in one sample: DUCK, THROW or LISTEN switched under a note do not click
            wetGs += (wetG - wetGs) * wetSmooth; if (std::fabs (wetG - wetGs) < 1e-12) wetGs = wetG;
            if (ageFadeDir < 0) { ageFade -= 1.0 / (0.008 * sr); if (ageFade < 0.0) ageFade = 0.0; }
            else if (ageFadeDir > 0) { ageFade += 1.0 / (0.008 * sr); if (ageFade >= 1.0) { ageFade = 1.0; ageFadeDir = 0; } }
            hpAmt += ((hpOn ? 1.0 : 0.0) - hpAmt) * hpSmooth; if (std::fabs ((hpOn ? 1.0 : 0.0) - hpAmt) < 1e-9) hpAmt = hpOn ? 1.0 : 0.0;
            ageG = ageFade >= 1.0 ? 1.0 : 0.5 * (1.0 - std::cos (M_PI * ageFade));   // raised cosine: a linear fade has a corner at its end, and the corner clicks
            wetG = wetGs * ageG;
            if (L)
            {
                if (e.sync)
                {
                    const double bpb = (double) std::max (1, p.beatsPerBar);
                    if (p.hostBars)   // bar lines of the host: its ppq / bar start; a jump of the playhead re-reads the bar without a change
                    {
                        const double rel = (p.hostPpq - p.hostBarStartPpq) + (double) i * p.bpm / (60.0 * sr);
                        const int64_t bar = (int64_t) std::llround (p.hostBarStartPpq / bpb) + (int64_t) std::floor (rel / bpb + 1e-9);
                        if (bar != lastBar)
                        {
                            const bool onLine = lastBar >= 0 && bar == lastBar + 1 && ! lastBarFallback;
                            const bool atStart = lastBar < 0 && std::fabs (rel - std::floor (rel / bpb + 1e-9) * bpb) < 1e-6;   // playback starts exactly on a bar line
                            lastBar = bar; lastBarFallback = false;
                            if ((onLine || atStart) && autoNote && onBarLine()) lnTarget = vo[mainV].lnT;
                        }
                    }
                    else
                    {
                        const int64_t bar = (int64_t) std::floor ((double) frame * p.bpm / (60.0 * sr) / bpb + 1e-9);
                        if (bar != lastBar) { lastBar = bar; lastBarFallback = true; if (autoNote && onBarLine()) lnTarget = vo[mainV].lnT; }   // the new line starts on its own target: no glide
                    }
                }
                if (xfadeT >= 0.0 || retireT >= 0.0) advanceCrossfade();
            }

            // ---- per-sample control: glide, smoothing, modulation
            Voice& mv = vo[mainV];
            const double d = lnTarget - mv.lnT; mv.lnT += std::max (-glideStep, std::min (glideStep, d));
            fbS += (tgtFb - fbS) * smoothCoef; toneS += (tgtTone - toneS) * smoothCoef; widthS += (tgtWidth - widthS) * smoothCoef;
            mixS += (tgtMix - mixS) * smoothCoef; if (tgtMix == 0.0 && mixS < 1e-7) mixS = 0.0;
            if (runS != tgtRun) { runS += (tgtRun - runS) * smoothCoef; if (std::fabs (tgtRun - runS) < 1e-7) runS = tgtRun; }   // exactly 0 again once the knob is back at 0
            if (chS != tgtChaos) { chS += (tgtChaos - chS) * smoothCoef; if (std::fabs (tgtChaos - chS) < 1e-7) chS = tgtChaos; }
            const bool chaosOn = chS > 0.0 || chLn != 0.0 || chTo != 0.0;
            if (chaosOn) advanceChaos (tgtChaos, timeTarget, (double) i);
            else chMul = 1.0;
            lfoPhase += (double) p.rateHz / sr; lfoPhase -= std::floor (lfoPhase);
            if (p.lfoShape != kSine) lfoSynced = false;
            else
            {
                if ((double) p.rateHz != lfoRate) { lfoRate = (double) p.rateHz; lfoSd = std::sin (2.0 * M_PI * lfoRate / sr); lfoCd = std::cos (2.0 * M_PI * lfoRate / sr); }
                if ((frame & 4095) == 0 || ! lfoSynced) { lfoSynced = true; lfoS = std::sin (2.0 * M_PI * lfoPhase); lfoC = std::cos (2.0 * M_PI * lfoPhase); }
                else { const double s = lfoS * lfoCd + lfoC * lfoSd; lfoC = lfoC * lfoCd - lfoS * lfoSd; lfoS = s; }
            }
            const bool updateFilters = (frame & 15) == 0;

            // effective feedback: the guard scales the user's value down, the throw adds on top but never past 100 % unless the user is already there
            double fbE = fbS;
            if (L) { fbE = fbS * guardG; if (fbAdd > 0.0) fbE = std::min (fbE + fbAdd, std::max (fbE, 1.0)); }
            // RUNAWAY: the loop gain is pushed towards kRunMaxGain on top of whatever FEEDBACK (guard, throw) gives; everything that already acts above 100 % (compander fade,
            // level servo, relaxed expander limit) follows this gain, so the runaway loop is held at the same level as FEEDBACK 110 %. Not touched at RUNAWAY 0 (bit-exact).
            double nAmp = noiseAmp;
            if (runS > 0.0)
            {
                fbE = runGain (fbE, runS);
                nAmp = noiseAmp * (1.0 + perform::kRunNoise * runS);
                if (updateFilters || darkKey < 0.0) setDark (runS);
            }
            loopGainNow = fbE;

            // T4 (17 Oct 2026, owner's option A): above 100 % FEEDBACK the compander fades out so the self-oscillating loop is clean. Level-dependent gain in a loop above unity
            // condensed the loop into a burst train with clicks (tools/crackle_probe.cpp). The depth follows a smoothstep that is finished at 101 %; fbE is already smoothed (20 ms),
            // so moving FEEDBACK across 100 % is a continuous ramp. At or below 100 % the original values are used untouched (bit-exact). The pair stays consistent: compD is
            // recomputed from the reduced depth.
            double cmpE = compE, cmpD = compD;
            double depthMark = (double) std::max (0.0f, p.comp);   // the compander depth each sample is encoded with travels through the line with it; only RUNAWAY decodes with it (see Chan::run)
            if (fbE > 1.0 && p.comp > 0.0f)
            {
                const double u = std::min (1.0, (fbE - 1.0) * (1.0 / listen::kCompFadeFb)), c = (double) p.comp * (1.0 - u * u * (3.0 - 2.0 * u));
                cmpE = -0.5 * c; cmpD = 0.5 * c / (1.0 - 0.5 * c);
                depthMark = c;
            }
            // one line: input (+ its feedback) -> BBD chain -> wet[2]
            auto runVoice = [&] (Voice& v, double baseT, double inG, double wetOut[2])
            {
                double chIn[2];
                const double d0 = dryIn[0] * inG * ageG, d1 = dryIn[1] * inG * ageG;   // ageFade: 1.0 outside an AGE change; during one the line's input is faded too, so the new line starts from silence, not from a cut   // inG is exactly 1.0 outside a crossfade, and x * 1.0 is exact
                if (ping) { chIn[0] = 0.5 * (d0 + d1) + v.ch[1].fbOut; chIn[1] = v.ch[0].fbOut; }
                else      { chIn[0] = d0 + v.ch[0].fbOut; chIn[1] = d1 + v.ch[1].fbOut; }
                const double driftV[2] = { v.ch[0].advanceDrift (sr), v.ch[1].advanceDrift (sr) };
                for (int c = 0; c < 2; ++c)
                {
                    Chan& h = v.ch[c];
                    double ph = lfoPhase + ((wide && c == 1) ? 0.25 : 0.0); ph -= std::floor (ph);
                    const double lfo = p.lfoShape == kTriangle ? 4.0 * std::fabs (ph - 0.5) - 1.0 : ((wide && c == 1) ? lfoC : lfoS);
                    const double drift = wide ? driftV[c] : driftV[0];   // MONO: both channels share channel 0's drift
                    const double mod = depth * kModDepthMax * lfo + depth * kDriftMax * drift;
                    double T = (chaosOn ? baseT * chMul : baseT) * (1.0 + mod) + ((e.stereo == kWide && c == 1) ? widthS : 0.0);   // CHAOS scales the clock of both sides (one clock)
                    T = std::max (tMin, std::min (1.4, T));
                    h.updateChain (baseT - kHostOffsetSamples / sr, kBufMax / stages, clockOffset (bwk));
                    double fclk = ((double) h.mStages + clockOffset (bwk)) / std::max (1e-4, T - kHostOffsetSamples / sr);
                    fclk = std::max (kMinClockHz, std::min (kMaxClockHz, fclk));
                    double w = h.run (chIn[c], fclk, sr, updateFilters, bwk, nAmp, cmpE, cmpD, eb0, eb1, ea0, ea1, envAtt, envRel, std::min (2.0, 1.0 + 10.0 * std::max (0.0, fbE - 1.0)), depthMark, runS > 0.0);
                    { const double lpv = h.hpLp.tick (w, hpG, hpA); if (hpAmt != 0.0) w -= hpAmt == 1.0 ? lpv : hpAmt * lpv; }   // wet high-pass (LISTEN), faded in and out over 10 ms
                    wetOut[c] = w;
                }
            };
            // feedback path of one line: tone tilt (+-6 dB), tanh, level servo (only above 100 %), gain
            if (toneS != toneKey)
            {   // passive tilt: the lo/hi ratio is g^2 as before (+-12 dB end to end), but it is realised by cutting the other side, so no band is ever above unity
                toneKey = toneS; const double g = std::pow (10.0, toneS * 6.0 / 20.0);
                toneLoG = g > 1.0 ? 1.0 : g * g; toneHiG = g > 1.0 ? 1.0 / (g * g) : 1.0;
            }
            auto feedback = [&] (Voice& v, const double wet[2], double fbE)
            {
                const double gLo = toneLoG, gHi = toneHiG;
                for (int c = 0; c < 2; ++c)
                {
                    Chan& h = v.ch[c];
                    const double ac = acCoef * (h.acY1 + wet[c] - h.acX1); h.acX1 = wet[c]; h.acY1 = ac;   // AC coupling: the loop carries no DC
                    const double lo = h.toneLp.tick (ac, toneG, toneA), hi = ac - lo;
                    double f = lo * gLo + hi * gHi;
                    if (runS > 0.0)
                    {   // RUNAWAY darkens the loop. The filter only runs while RUNAWAY is up; when it comes on, its state starts on the signal (output = input), so nothing jumps
                        if (! h.darkOn) { h.darkLp.s = f; h.darkOn = true; }
                        f += runS * (h.darkLp.tick (f, darkG, darkA) - f);
                    }
                    else h.darkOn = false;
                    f = std::tanh (f);
                    h.fbEnv += (f * f - h.fbEnv) * servoCoef;
                    // level servo above 100 %: an integrator plus a proportional term on the loop gain hold the loop's RMS at kServoTarget (it only ever lowers the gain),
                    // so the self-oscillation settles at a fixed level instead of creeping up against the tanh. At or below 100 % it relaxes to 1.
                    if (fbE > 1.0) h.servo = std::max (0.05, std::min (1.0, h.servo + (kServoTarget - std::sqrt (h.fbEnv)) * servoRate));
                    else           h.servo += (1.0 - h.servo) * smoothCoef;
                    const double servo = fbE > 1.0 ? h.servo * std::min (1.0, kServoTarget / (std::sqrt (h.fbEnv) + 1e-9)) : h.servo;   // + a proportional term: the integrator alone hunts
                    double o = f * fbE * servo; if (std::fabs (o) < 1e-20) o = 0.0;
                    h.fbOut = o;
                }
            };

            double wet[2], wetOld[2] = { 0.0, 0.0 };
            runVoice (mv, std::exp (mv.lnT), mv.inG, wet);
            Voice& ov = vo[mainV ^ 1];
            const bool oldOn = ov.retiring;
            if (oldOn) runVoice (ov, std::exp (ov.lnT), ov.inG, wetOld);
            ++frame;

            feedback (mv, wet, fbE);
            if (oldOn) feedback (ov, wetOld, fbE);
            if (oldOn) { wet[0] += ov.outG * wetOld[0]; wet[1] += ov.outG * wetOld[1]; }
            if (L) guardInput (dryIn[0], dryIn[1], wet[0], wet[1]);

            const double ws[2] = { softClip (wet[0], ceil), softClip (wet[1], ceil) };
            const double m = mixS;
            {   // P4b: the panel's ECHO needle reads the wet level after the duck and the throw (a read-only tap: it changes no sample)
                const double a0 = std::fabs (ws[0] * wetG), a1 = std::fabs (ws[1] * wetG);
                const double a = a0 > a1 ? a0 : a1;
                if (a > wetPk) wetPk = a;
            }
            if (m == 0.0) { outL[i] = (float) dryIn[0]; outR[i] = (float) dryIn[1]; }
            else
            {
                const double w0 = ws[0] * wetG, w1 = ws[1] * wetG;   // duck / throw act on the output only; the loop keeps its tail
                outL[i] = flush ((float) (dryIn[0] * (1.0 - m) + w0 * m));
                outR[i] = flush ((float) (dryIn[1] * (1.0 - m) + w1 * m));
            }
        }
    }

    /** The shortest delay the line can make (its clock is capped at kMaxClockHz): NEW 41.0 ms, OLD 20.6 ms. A TIME or note value under it plays at this value; the panel shows it. */
    static double shortestTimeSec (int ageIdx, double sampleRate)
    {
        return std::max (kMinTimeSec, ((double) (kStages[ageIdx] / 2) + clockOffset (kBwFactor[ageIdx])) / kMaxClockHz + 2.0 / sampleRate);
    }

    // Calibration constants of the clock formula T = (stages + kClockStagesOffset) / f_clock + kHostOffsetSamples / sr,
    // which makes the TIME knob the delay of the impulse response's peak (filter group delay, hold and interpolation included).
    // Extra delay of the filters, hold and interpolation in clock periods: 3.74 at 0.38 (NEW), 7.08 at 0.19 (OLD), calibrated on the impulse centroid (D2); linear in 1 / factor.
    static double clockOffset (double bwk) { return 3.74 + 3.34 * (0.38 / bwk - 1.0); }
    static constexpr double kClockStagesOffset = 3.74;   // NEW (0.38)   // was 2.69 with the 4th-order AA: the 8th-order AA adds 2.5128 / (2 pi 0.38) = 1.05 clock periods of group delay
    static constexpr double kHostOffsetSamples = 1.0;
    static constexpr double kAcCouplingHz = 16.0;   // P3b: coupling capacitor of the feedback path (1st-order high-pass), after the compander / expander pair. Opus said about 8 Hz; measured: 8 Hz leaves up to 0.0056 of DC (D13 limit 0.002), 12 Hz 0.0024, 16 Hz 0.0018, 20 Hz 0.0022
    static constexpr double kServoTarget = 0.2;   // loop RMS the level servo holds above 100 % feedback
    // Compander depth marks carried by the line (read by RUNAWAY only): the depth (COMPANDER, faded above a loop gain of 1.0) each sample was encoded with; kDepthCurrent =
    // unknown (a cleared line), decode with the depth in force now. At RUNAWAY 0 the marks are written but never read (bit-exact to the engine before them). A RUNAWAY sweep fades the compander
    // in and out (above / under a loop gain of 1.0) faster than a sample takes to cross the line; decoding it with the depth it was encoded with keeps the pair complementary.
    static constexpr double kDepthCurrent = -2.0;

private:
    // shortest time one line can give: its clock is capped at kMaxClockHz
    double minTimeSec() const { return std::max (kMinTimeSec, ((double) stages + clockOffset (kBwFactor[age])) / kMaxClockHz + 2.0 / sr); }
    static float flush (float v) { return std::fabs (v) < 1e-30f ? 0.0f : v; }
    static double softClip (double x, double c)
    {
        const double k = 0.6 * c, ax = std::fabs (x);
        if (ax <= k) return x;
        const double y = k + (c - k) * std::tanh ((ax - k) / (c - k));
        return x < 0 ? -y : y;
    }

    struct Lp1   // one-pole TPT low-pass
    {
        double s = 0;
        inline double tick (double x, double g, double a) { const double v = (x - s) * (g * a); const double y = v + s; s = y + v; return y; }
    };

    // 2nd-order TPT state-variable low-pass with explicit coefficients
    struct Bq
    {
        double ic1 = 0, ic2 = 0, a1 = 0, a2 = 0, a3 = 0;
        void set (double g, double k) { a1 = 1.0 / (1.0 + g * (g + k)); a2 = g * a1; a3 = g * a2; }
        inline double tick (double x)
        {
            const double v3 = x - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0 * v1 - ic1; ic2 = 2.0 * v2 - ic2; return v2;
        }
    };

    struct Chan
    {
        std::vector<double> buf, tmp; std::vector<double> gbuf, gtmp, dbuf; double heldD = -2.0; double gHeld = 1.0, heldG = 1.0, gAcc = 1.0; int idx = 0;   // gbuf: the compressor gain each line sample was made with, it travels through the line with the sample
        int cmpCnt = 0; double gEnc = 1.0, gDec = 1.0;
        double lastTRef = -1.0; int ns = 2048, lines = 1; bool fresh = true;   // chain: ns samples per line, `lines` identical lines in series
        // emphasis
        double pX1 = 0, pY1 = 0, dX1 = 0, dY1 = 0;
        // compander detectors (mean square)
        double msEnc = 0, msDec = 0;
        // line
        Bq aa[4], rc[2]; double a[4] = { 0, 0, 0, 0 }; double held = 0, phase = 0, leakPh = 0;
        uint64_t rng = 1;
        // feedback
        Lp1 toneLp, hpLp, darkLp; bool darkOn = false; double acX1 = 0, acY1 = 0; double fbEnv = 0, fbOut = 0, servo = 1.0;
        // drift
        double drU = 0, dr0 = 0, dr1 = 0, drT = 0, drLen = 1; int drCnt = 0; uint64_t drRng = 1;

        void seed (uint64_t s) { rng = s | 1; drRng = (s * 0xD1B54A32D192ED03ull) | 1; }
        void clear (int stages)
        {
            std::fill (buf.begin(), buf.end(), 0.0); std::fill (gbuf.begin(), gbuf.end(), 1.0); std::fill (dbuf.begin(), dbuf.end(), kDepthCurrent); heldD = kDepthCurrent; gHeld = heldG = gAcc = 1.0; idx = 0; ns = stages; lines = 1; fresh = true; mStages = stages;
            pX1 = pY1 = dX1 = dY1 = 0; msEnc = msDec = 0; for (auto& s : aa) s = Bq(); for (auto& s : rc) s = Bq();
            for (double& v : a) v = 0; cmpCnt = 0; gEnc = gDec = 1.0; held = 0; phase = 0; leakPh = 0; toneLp = Lp1(); hpLp = Lp1(); darkLp = Lp1(); darkOn = false; acX1 = acY1 = 0; fbEnv = 0; fbOut = 0; servo = 1.0;
        }
        inline double nextRand()   // xorshift64*, -1 .. 1
        {
            rng ^= rng >> 12; rng ^= rng << 25; rng ^= rng >> 27;
            return (double) (int64_t) (rng * 0x2545F4914F6CDD1Dull) * (1.0 / 9223372036854775808.0);
        }
        inline double driftRand()
        {
            drRng ^= drRng >> 12; drRng ^= drRng << 25; drRng ^= drRng >> 27;
            return (double) (int64_t) (drRng * 0x2545F4914F6CDD1Dull) * (1.0 / 9223372036854775808.0);
        }
        // Chain: as many identical lines in series as keep the clock at or above kChainClockHz (each line has its own sample-and-hold; at one common clock a chain
        // of N lines holds N x ns samples, so it is one longer ring with the filters shared at the two ends). tRef = the unmodulated delay. When the line count changes
        // (hysteresis +-3 %) the delay stays and the clock jumps, so the history in the ring is resampled to the new length: the repeats go on without a gap.
        void updateChain (double tRef, int maxLines, double off)
        {
            if (! fresh && tRef == lastTRef) return;   // nothing to decide: the unmodulated time did not move
            lastTRef = tRef;
            auto need = [&] (double clk) { return std::max (1, std::min (maxLines, (int) std::ceil ((clk * tRef - off) / ns))); };
            if (fresh) { lines = need (kChainClockHz); mStages = ns * lines; fresh = false; return; }
            const int up = need (kChainClockHz * 0.97), dn = need (kChainClockHz * 1.03);
            int want = lines;
            if (lines < up) want = up; else if (lines > dn) want = dn;
            if (want != lines) rechain (want);
        }
        void rechain (int nl)
        {
            const int Mo = mStages, Mn = ns * nl;
            for (int i = 0; i < Mo; ++i) { int k = idx + i; if (k >= Mo) k -= Mo; tmp[(size_t) i] = buf[(size_t) k]; gtmp[(size_t) i] = gbuf[(size_t) k]; }   // oldest .. newest
            const double r = (double) Mo / (double) Mn;
            for (int j = 0; j < Mn; ++j)
            {
                double v, vg;
                if (r <= 1.0) { const double pos = j * r; const int i0 = (int) pos; const int i1 = std::min (i0 + 1, Mo - 1); v = tmp[(size_t) i0] + (pos - i0) * (tmp[(size_t) i1] - tmp[(size_t) i0]); vg = gtmp[(size_t) i0] + (pos - i0) * (gtmp[(size_t) i1] - gtmp[(size_t) i0]); }
                else   // fewer samples for the same history: box average over the old samples that fall into the new one
                {
                    const double a = j * r, b = (j + 1) * r; double acc = 0.0, accg = 0.0;
                    for (int i = (int) a; i < Mo && i < (int) std::ceil (b); ++i) { const double wgt = std::min (b, (double) i + 1.0) - std::max (a, (double) i); acc += tmp[(size_t) i] * wgt; accg += gtmp[(size_t) i] * wgt; }
                    v = acc / (b - a); vg = accg / (b - a);
                }
                buf[(size_t) j] = v; gbuf[(size_t) j] = vg;
            }
            {   // the depth marks are labels, not signal: nearest old sample (oldest .. newest order, via tmp)
                for (int i = 0; i < Mo; ++i) { int k = idx + i; if (k >= Mo) k -= Mo; tmp[(size_t) i] = dbuf[(size_t) k]; }
                for (int j = 0; j < Mn; ++j) dbuf[(size_t) j] = tmp[(size_t) std::min (Mo - 1, (int) ((j + 0.5) * r))];
            }
            idx = 0; mStages = Mn; lines = nl;
        }
        void initDrift() { drU = 0.0; dr0 = driftRand(); dr1 = driftRand(); drT = 0; drLen = 3.3 + 6.7 * (0.5 + 0.5 * driftRand()); }
        // slow random drift in -1 .. 1: cosine-interpolated random values, a new one every 3.3 .. 10 s (0.1 .. 0.3 Hz)
        double advanceDrift (double sr)
        {
            drT += 1.0 / sr;
            if (drT >= drLen) { drT -= drLen; dr0 = dr1; dr1 = driftRand(); drLen = 3.3 + 6.7 * (0.5 + 0.5 * driftRand()); }
            if ((++drCnt & 15) == 0 || drT < 2.0 / sr) drU = 0.5 - 0.5 * std::cos (M_PI * drT / drLen);   // the slow drift is re-evaluated every 16 samples
            return dr0 + (dr1 - dr0) * drU;
        }

        static inline double catmull (double p0, double p1, double p2, double p3, double u)
        { return p1 + 0.5 * u * (p2 - p0 + u * (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3 + u * (3.0 * (p1 - p2) + p3 - p0))); }

        double run (double x, double fclk, double sr, bool updateFilters, double bwk, double noiseAmp, double compE, double compD,
                    double eb0, double eb1, double ea0, double ea1, double envAtt, double envRel, double limScale, double depthMark, bool useMarks)
        {
            if (updateFilters)
            {
                const double fc = std::min (bwk * fclk, 0.45 * sr), g = std::tan (M_PI * fc / sr);
                // AA: 8th-order Butterworth (k = 2 sin((2m-1) pi / 16)), 48 dB/oct; the reconstruction filter stays 4th order.
                aa[0].set (g, 1.9616); aa[1].set (g, 1.6629); aa[2].set (g, 1.1111); aa[3].set (g, 0.3902);
                rc[0].set (g, 1.8478); rc[1].set (g, 0.7654);
            }
            // anti-denormal floor: alternating +-1e-18 (-360 dBFS)
            x += (frameParity ^= 1) ? 1e-18 : -1e-18;
            // pre-emphasis
            double y = (eb0 * x + eb1 * pX1 - ea1 * pY1) / ea0; pX1 = x; pY1 = y;
            // compressor
            { const double q = y * y; msEnc += (q - msEnc) * (q > msEnc ? envAtt : envRel); }
            const bool cmpUpd = (cmpCnt++ & 3) == 0;   // the compander gains follow an 8 ms detector: they are re-evaluated every 4th sample (0.08 ms), the log / exp are the cost
            if (compE != 0.0 || gEnc != 1.0) { if (cmpUpd) gEnc = std::exp (0.5 * compE * std::log (std::max (msEnc, 1e-10) * (1.0 / (kCompRef * kCompRef)))); y *= gEnc; }
            // anti-alias filter
            y = aa[3].tick (aa[2].tick (aa[1].tick (aa[0].tick (y))));
            a[3] = a[2]; a[2] = a[1]; a[1] = a[0]; a[0] = y;
            // BBD: sample-and-hold line at the clock rate, box-averaged over each host sample
            const double dTicks = fclk / sr;
            const int M = mStages;
            double remaining = 1.0, acc = 0.0, accg = 0.0;
            for (;;)
            {
                const double toTick = (1.0 - phase) / dTicks;
                if (toTick <= remaining)
                {
                    acc += held * toTick; accg += heldG * toTick; remaining -= toTick;
                    const double f = 1.0 - remaining;
                    const double in = catmull (a[3], a[2], a[1], a[0], f) + noiseAmp * nextRand();
                    held = buf[(size_t) idx]; buf[(size_t) idx] = in; heldG = gbuf[(size_t) idx]; gbuf[(size_t) idx] = gEnc; heldD = dbuf[(size_t) idx]; dbuf[(size_t) idx] = depthMark; if (++idx >= M) idx = 0;
                    phase = 0.0;
                }
                else { acc += held * remaining; accg += heldG * remaining; phase += remaining * dTicks; break; }
            }
            double v = acc;
            if (fclk < 0.45 * sr)   // clock leakage, -70 dBc, before the reconstruction filter
            {
                leakPh += dTicks; leakPh -= std::floor (leakPh);
                v += 3.1623e-4 * std::sin (2.0 * M_PI * leakPh);
            }
            v = rc[1].tick (rc[0].tick (v));
            // expander
            { const double q = v * v; msDec += (q - msDec) * (q > msDec ? envAtt : envRel); }
            // P3c: complementary companding. The expander never gets more gain than the inverse of the gain the compressor used on the very samples now coming out of the line
            // (that gain travels through the line with them, so it holds for any TIME, glide or chain). The filters, the line and the noise change what its own detector reads;
            // at low levels that made compressor x expander exceed 1 and the loop self-sustain. Its own detector can only lower the gain on top of that, which keeps the breathing on transients.
            // The line filters smear a click over the samples after it, and those were compressed with the click's gain: the gain is smoothed on this side (0.5 ms attack, 2 ms release: a peak hold).
            // Above 100 % feedback (self-oscillation by design, held by the level servo) the limit is relaxed (up to +6 dB at 110 %), or the loop could not start or hold steady.
            gHeld += (accg - gHeld) * (accg > gHeld ? 1.0 / (kGainAttackSec * sr) : 1.0 / (kGainHoldSec * sr));
            if (useMarks && heldD >= 0.0) compD = 0.5 * heldD / (1.0 - 0.5 * heldD);   // RUNAWAY: decode with the depth the sample was encoded with
            if (compD != 0.0 || gDec != 1.0)
            {
                if (cmpUpd) gDec = std::exp (0.5 * compD * std::log (std::max (msDec, 1e-10) * (1.0 / (kCompRef * kCompRef))));
                v *= std::min (gDec, limScale / std::max (1e-6, gHeld));
            }
            // de-emphasis
            const double o = (ea0 * v + ea1 * dX1 - eb1 * dY1) / eb0; dX1 = v; dY1 = o;
            return o;
        }
        int mStages = 2048; int frameParity = 0;
        static constexpr double kCompRef = 0.1;
    };

public:
    // ---- state
    Params p;
private:
    struct Voice { Chan ch[2]; double lnT = 0.0, inG = 1.0, outG = 1.0; bool retiring = false; };
    Voice vo[2]; int mainV = 0;
    double sr = 48000.0, acCoef = 0.999, glideStep = 0, smoothCoef = 0, envAtt = 0, envRel = 0, servoCoef = 0, servoRate = 0;
    double eb0 = 1, eb1 = 0, ea0 = 1, ea1 = 0, toneG = 0, toneA = 0;
    double toneKey = -9.0, toneLoG = 1.0, toneHiG = 1.0;
    double lfoS = 0, lfoC = 1, lfoSd = 0, lfoCd = 1, lfoRate = -1.0; bool lfoSynced = false;   // sine LFO as a rotating pair, re-synchronised to lfoPhase every 4096 samples
    double fbS = 0, mixS = 0, toneS = 0, widthS = 0, lfoPhase = 0;
    int age = kNew, stages = 4096; uint64_t frame = 0; bool snap = true;
    double wetGs = 1.0, wetSmooth = 0.0, hpSmooth = 0.0, hpAmt = 0.0, hpGlast = 0.0, ageFade = 1.0, ageG = 1.0; int ageFadeDir = 0;
    double wetPk = 0.0;   // P4b: see takeWetPeak()

    // ------------------------------------------------------------------------------------------------ PERFORM: RUNAWAY and CHAOS
    double runS = 0.0, chS = 0.0, darkG = 0.0, darkA = 1.0, darkKey = -1.0, loopGainNow = 0.3, runTargetGain = 0.3;
    // CHAOS: wow / flutter random walks (a = from, b = to), the pitch step (log of the TIME factor) and its glide, the beat counter
    uint64_t chRng = 1; double wowA = 0, wowB = 0, wowT = 0, wowLen = 1, flA = 0, flB = 0, flT = 0, flLen = 0.1, chEnergy = 0.0, chMul = 1.0;
    double chLn = 0.0, chFrom = 0.0, chTo = 0.0, chGlT = -1.0, chGlLen = 0.006, chStepClk = 0.0; int chSemi = 0, chSteps = 0, chTick = 0; int64_t chLastBeat = INT64_MIN;

    static double smooth3 (double u) { return u * u * (3.0 - 2.0 * u); }
    /** RUNAWAY's loop gain over `fb`: a smoothstep from fb to kRunMaxGain (gentle for the first 20 %: +10 % of the way at 20 %, runs away from about 70 % at FEEDBACK 30 %). */
    static double runGain (double fb, double r) { return fb + std::max (0.0, perform::kRunMaxGain - fb) * smooth3 (r); }
    void setDark (double r)
    {
        darkKey = r;
        const double fc = perform::kRunDarkHiHz * std::pow (perform::kRunDarkLoHz / perform::kRunDarkHiHz, r);
        darkG = std::tan (M_PI * std::min (fc, 0.45 * sr) / sr); darkA = 1.0 / (1.0 + darkG);
    }
    double chRand()   // 0 .. 1
    {
        chRng ^= chRng >> 12; chRng ^= chRng << 25; chRng ^= chRng >> 27;
        return (double) ((chRng * 0x2545F4914F6CDD1Dull) >> 11) * (1.0 / 9007199254740992.0);
    }
    void resetPerform()
    {
        chRng = perform::kChaosSeed;   // every instance starts from the same seed: a render is reproducible
        wowA = 0.0; wowB = 2.0 * chRand() - 1.0; wowT = 0.0; wowLen = perform::kWowMinSec + (perform::kWowMaxSec - perform::kWowMinSec) * chRand();
        flA = 0.0; flB = 2.0 * chRand() - 1.0; flT = 0.0; flLen = perform::kFlutMinSec + (perform::kFlutMaxSec - perform::kFlutMinSec) * chRand();
        chEnergy = 0.0; chMul = 1.0; chLn = chFrom = chTo = 0.0; chGlT = -1.0; chStepClk = 0.0; chSemi = 0; chSteps = 0; chTick = 0; chLastBeat = INT64_MIN;
        loopGainNow = runTargetGain = (double) p.feedback;
    }
    void startChaosGlide (int semi, double len)
    {
        chSemi = semi; chFrom = chLn; chTo = -(double) semi / 12.0 * 0.6931471805599453;   // higher pitch = faster clock = shorter time
        chGlT = 0.0; chGlLen = len;
    }
    /** One sample of CHAOS: the wobble (always, depth by the knob up to 50 % and by the loop energy), the beat clock and the steps (above 50 %), the step glide. */
    void advanceChaos (double tgt, double period, double i)
    {
        const double dt = 1.0 / sr;
        wowT += dt; if (wowT >= wowLen) { wowT -= wowLen; wowA = wowB; wowB = 2.0 * chRand() - 1.0; wowLen = perform::kWowMinSec + (perform::kWowMaxSec - perform::kWowMinSec) * chRand(); }
        flT += dt;  if (flT >= flLen)   { flT -= flLen; flA = flB; flB = 2.0 * chRand() - 1.0; flLen = perform::kFlutMinSec + (perform::kFlutMaxSec - perform::kFlutMinSec) * chRand(); }
        const double wob = (1.0 - perform::kFlutShare) * (wowA + (wowB - wowA) * smooth3 (std::min (1.0, wowT / wowLen)))
                         + perform::kFlutShare * (flA + (flB - flA) * smooth3 (std::min (1.0, flT / flLen)));
        if ((chTick++ & 15) == 0)
        {   // loop energy: the main line's feedback RMS after the tanh (150 ms detector), 1 = the self-oscillation level
            const double ms = 0.5 * (vo[mainV].ch[0].fbEnv + vo[mainV].ch[1].fbEnv);
            chEnergy = std::min (1.0, std::sqrt (ms) / perform::kChaosEnergyRef);
        }
        const double depth = perform::kChaosWobMax * std::min (1.0, chS / perform::kChaosStepFrom) * (perform::kChaosWobFloor + (1.0 - perform::kChaosWobFloor) * chEnergy);
        // beats: the host's (quarter notes of its position while it plays), else one per delay time counted from the start
        bool beat = false;
        if (p.hostBeats)
        {
            const int64_t b = (int64_t) std::floor (p.hostPpq + i * p.bpm / (60.0 * sr) + 1e-9);
            if (b != chLastBeat) { beat = chLastBeat != INT64_MIN && b == chLastBeat + 1; chLastBeat = b; }   // a jump of the playhead re-reads the beat without a step
            chStepClk = 0.0;
        }
        else
        {
            chLastBeat = INT64_MIN;
            chStepClk += dt; if (chStepClk >= period) { chStepClk -= period; beat = true; }
        }
        if (beat && tgt > perform::kChaosStepFrom)
        {
            const double prob = perform::kChaosProbMax * (tgt - perform::kChaosStepFrom) / (1.0 - perform::kChaosStepFrom);
            if (chRand() < prob)
            {
                int k = (int) (chRand() * 6.0); if (k > 5) k = 5;   // one of the six values that are not the current one
                int semi = 0, seen = 0;
                for (int s : perform::kChaosSemis) { if (s == chSemi) continue; if (seen++ == k) { semi = s; break; } }
                startChaosGlide (semi, perform::kChaosGlideSec);
                ++chSteps; logEvent (kEvChaosStep, (double) semi, (double) chSteps);
            }
        }
        if (chGlT >= 0.0)
        {
            chGlT += dt;
            if (chGlT >= chGlLen) { chLn = chTo; chGlT = -1.0; }
            else chLn = chFrom + (chTo - chFrom) * smooth3 (chGlT / chGlLen);
        }
        chMul = std::exp (depth * wob + chLn);
    }

    // ------------------------------------------------------------------------------------------------ LISTEN state and logic
    static constexpr int kEvCap = 4096, kHistBins = 91;
    svl::sourcesense::SourceSense ss;
    // level statistics, phrase detector
    double lvlSum = 0, lvlRing[listen::kLevelBlocks] = {}, hist[kHistBins] = {}, histW = 0, thrDb = listen::kThrDefaultDb, thrLin = 0, thrLowLin = 0;
    int lvlN = 0, lvlIdx = 0, lvlFilled = 0, lvlBlockLen = 2400; double lvlFrames = 0;
    double envMs = 0, envCoef = 0, histDecay = 1.0, phraseMat = 0, gapT = 0, duckS = 0, duckHoldT = 0, duckDbCur = 0, duckDbNow = 0; bool playing = false, gapDone = true, duckOn = false; int duckTick = 0;
    // throw
    int tStage = 0; double tClock = 0, tLen = 0, tTarget = 0, tAmt = 0, tCoef = 0, tCancelRate = 0, playRun = 0, lastLiftDb = 0; bool tCancel = false;
    // note / crossfade
    int noteEff = 11; int64_t lastBar = -1; bool lastBarFallback = false; double xfadeT = -1.0, retireT = -1.0;
    // guard
    double gIn = 0, gWet = 0, gHist[32] = {}; int gN = 0, gBlockLen = 4800, gIdx = 0, gCount = 0, silentBlocks = 0; bool guardOn = false; double guardG = 1.0, guardRel = 0;
    // events
    bool evOn = false; int evHead = 0, evTail = 0; std::vector<Event> evBuf;

    void logEvent (int type, double a, double b = 0.0)
    {
        if (! evOn || evBuf.empty()) return;
        Event& ev = evBuf[(size_t) evHead]; ev.frame = frame; ev.type = type; ev.a = a; ev.b = b;
        evHead = (evHead + 1) % kEvCap; if (evHead == evTail) evTail = (evTail + 1) % kEvCap;
    }

    void resetListen()
    {
        ss.reset();
        lvlSum = 0; lvlN = 0; lvlIdx = 0; lvlFilled = 0; lvlFrames = 0; for (double& v : lvlRing) v = 0; for (double& v : hist) v = 0; histW = 0;
        lvlBlockLen = std::max (1, (int) std::lround (listen::kLevelBlockSec * sr));
        histDecay = std::pow (0.5, listen::kLevelBlockSec / listen::kHistHalfLifeSec);
        thrDb = listen::kThrDefaultDb; setThreshold (thrDb);
        envMs = 0; envCoef = 1.0 - std::exp (-1.0 / (listen::kEnvSec * sr));
        
        playing = false; gapDone = true; phraseMat = 0; gapT = 0; duckS = 0; duckHoldT = 0; duckDbCur = 0; duckOn = false; duckDbNow = 0; duckTick = 0;
        tStage = 0; tClock = 0; tTarget = 0; tAmt = 0; tCancelRate = 0; lastLiftDb = 0; tCancel = false; playRun = 0; tCoef = 1.0 - std::exp (-1.0 / (listen::kThrowRiseSec * sr));
        noteEff = p.note; lastBar = -1; lastBarFallback = false;
        gIn = gWet = 0; gN = 0; gIdx = 0; gCount = 0; silentBlocks = 0; guardOn = false; guardG = 1.0; for (double& v : gHist) v = -120.0;
        gBlockLen = std::max (1, (int) std::lround (listen::kGuardBlockSec * sr)); guardRel = 1.0 - std::exp (-1.0 / (listen::kGuardReleaseSec * sr));
        evHead = evTail = 0;
    }
    void setThreshold (double db) { thrDb = db; thrLin = std::pow (10.0, db / 10.0); thrLowLin = std::pow (10.0, (db - listen::kHystDb) / 10.0); }

    // input level statistics, playing / gap detector, duck, throw
    void listenInput (double l, double r, double duckDepthDb, double throwDbE)
    {
        const double x2 = 0.5 * (l * l + r * r), dt = 1.0 / sr;
        // 300 ms RMS from 50 ms blocks; histogram of its audible values; threshold from the median
        lvlSum += x2;
        if (++lvlN >= lvlBlockLen)
        {
            lvlRing[lvlIdx] = lvlSum / lvlN; lvlSum = 0; lvlN = 0; lvlIdx = (lvlIdx + 1) % listen::kLevelBlocks; if (lvlFilled < listen::kLevelBlocks) ++lvlFilled;
            double mean = 0; for (int k = 0; k < lvlFilled; ++k) mean += lvlRing[k]; mean /= lvlFilled;
            const double db = 10.0 * std::log10 (mean + 1e-20);
            for (double& v : hist) v *= histDecay; histW *= histDecay;
            if (lvlFilled == listen::kLevelBlocks && db >= listen::kHistFloorDb)
            {
                const int bin = std::max (0, std::min (kHistBins - 1, (int) std::floor (db - listen::kHistMinDb))); hist[bin] += 1.0; histW += 1.0; lvlFrames += 1.0;
            }
            if (lvlFrames >= listen::kThrMinFrames && histW > 0)
            {
                double c = 0, med = listen::kHistFloorDb;
                for (int k = 0; k < kHistBins; ++k) { c += hist[k]; if (c >= 0.5 * histW) { med = listen::kHistMinDb + k + 0.5; break; } }
                setThreshold (std::max (listen::kThrMinDb, std::min (listen::kThrMaxDb, med + listen::kThrOffsetDb)));
            }
        }
        envMs += (x2 - envMs) * envCoef;
        const bool wasPlaying = playing;
        if (! playing) { if (envMs > thrLin) playing = true; }
        else if (envMs < thrLowLin) playing = false;
        (void) wasPlaying;

        // phrase end (P3e): >= 1.2 s of material (a gap that is not confirmed as a phrase end is bridged and counts as material), then a gap that has lasted
        // max(250 ms, half the current repeat time). The throw is confirmed by the gap, never guessed at its start.
        if (playing)
        {
            if (gapT > 0.0) { if (gapDone) phraseMat = 0.0; else phraseMat += gapT; }   // the gap just ended: confirmed = a new phrase starts, else it was a breath and is bridged
            phraseMat += dt; gapT = 0.0; gapDone = false; playRun += dt;
            // the duck overrides the throw: the source is back for 20 ms, so a throw in progress is cancelled (40 ms fade), the duck engages with its normal attack
            if (playRun >= listen::kReturnSec && (tStage != 0 || tAmt > 0.0) && ! tCancel)
            { tCancel = true; tStage = 0; tTarget = 0.0; tCancelRate = tAmt / listen::kThrowCancelSec; logEvent (kEvThrowCancel, tAmt, phraseMat); }
        }
        else
        {
            playRun = 0.0; gapT += dt;
            if (! gapDone && gapT >= std::max (listen::kPhraseGapSec, 0.5 * std::exp (vo[mainV].lnT)))
            {
                gapDone = true;
                if (phraseMat >= listen::kPhraseMinSec && throwDbE > 0.0)
                { tStage = 1; tClock = 0.0; tCancel = false; tLen = std::exp (vo[mainV].lnT); logEvent (kEvThrowOn, throwDbE, phraseMat); }
                phraseMat = 0.0;
            }
        }
        // duck (P1d): the depth follows a raised-cosine ramp, 30 ms in while the source plays, 300 ms out into the gap; a 120 ms hold after the level falls under the gap threshold
        // keeps it in between syllables. duckS is the position on the ramp (0 = open, 1 = full depth), so a change of direction half way continues smoothly.
        duckHoldT = playing ? listen::kDuckHoldSec : std::max (0.0, duckHoldT - dt);
        // P3e: a gap shorter than 250 ms is a breath: the duck stays in through it (it replaces the 120 ms hold there), so the middle of a phrase stays clean and the 300 ms release starts only into a real gap.
        const bool breath = ! playing && ! gapDone && gapT < listen::kPhraseGapSec;
        const bool want = duckDepthDb > 0.0 && (playing || duckHoldT > 0.0 || breath);
        if (want != duckOn) { duckOn = want; logEvent (kEvDuck, duckOn ? 1.0 : 0.0, duckDepthDb); }   // one event per state change: the number the L6 / P1d report counts
        duckS = std::max (0.0, std::min (1.0, duckS + (want ? dt / listen::kDuckAttackSec : -dt / listen::kDuckReleaseSec)));
        duckDbCur = duckS > 0.0 ? -duckDepthDb * 0.5 * (1.0 - std::cos (M_PI * duckS)) : 0.0;
        if ((++duckTick & 63) == 0) duckDbNow = duckDbCur;
        // throw envelope: one note value at full, then a linear fall over two
        if (tStage == 1) { tTarget = 1.0; tClock += dt; if (tClock >= tLen) { tStage = 2; tClock = 0.0; } }
        else if (tStage == 2) { tClock += dt; tTarget = 1.0 - tClock / (2.0 * tLen); if (tClock >= 2.0 * tLen) { tStage = 0; tTarget = 0.0; logEvent (kEvThrowOff, 0.0); } }
        if (tCancel) { tAmt = std::max (0.0, tAmt - tCancelRate * dt); if (tAmt <= 0.0) tCancel = false; }
        else tAmt += (tTarget - tAmt) * tCoef;
        if (tStage == 0 && tAmt < 1e-6) tAmt = 0.0;
    }

    // density note: decided at bar lines only
    bool onBarLine()   // true when the note changed
    {
        const double rate = ss.onsetRateShort();
        if (rate < 0.0 || vo[mainV ^ 1].retiring) return false;   // no estimate yet, or the last change is still fading out
        const int cur = noteEff; const bool known = cur == listen::kNoteBusy || cur == listen::kNoteMedium || cur == listen::kNoteSparse;
        const double h = known ? listen::kDensityHyst : 0.0;
        int want = listen::kNoteMedium;
        if (rate > listen::kDenseHi + (cur == listen::kNoteBusy ? -h : h)) want = listen::kNoteBusy;
        else if (rate < listen::kSparseLo + (cur == listen::kNoteSparse ? h : -h)) want = listen::kNoteSparse;
        if (want == cur) return false;
        startCrossfade (want); return true;
    }
    void startCrossfade (int note)
    {
        noteEff = note;
        const double T = std::max (minTimeSec(), std::min (kMaxTimeSec, 60.0 / std::max (20.0, p.bpm) * noteBeats (note)));
        Voice& old = vo[mainV]; mainV ^= 1; Voice& nv = vo[mainV];
        for (auto& c : nv.ch) c.clear (stages);
        for (auto& c : nv.ch) c.initDrift();
        nv.lnT = std::log (T); nv.inG = 0.0; nv.outG = 1.0; nv.retiring = false;
        old.retiring = true; old.inG = 1.0; old.outG = 1.0;
        xfadeT = 0.0; retireT = -1.0;
        logEvent (kEvNote, (double) note, T);
    }
    void advanceCrossfade()
    {
        Voice& nv = vo[mainV]; Voice& old = vo[mainV ^ 1];
        if (xfadeT >= 0.0)
        {
            xfadeT += 1.0 / sr;
            if (xfadeT < listen::kXfadeSec) { const double th = 0.5 * M_PI * xfadeT / listen::kXfadeSec; nv.inG = std::sin (th); old.inG = std::cos (th); }
            else { nv.inG = 1.0; old.inG = 0.0; xfadeT = -1.0; retireT = 0.0; }
        }
        if (retireT >= 0.0)
        {
            retireT += 1.0 / sr;
            if (retireT < listen::kRetireSec) old.outG = 0.5 + 0.5 * std::cos (M_PI * retireT / listen::kRetireSec);
            else { old.outG = 0.0; old.retiring = false; retireT = -1.0; }
        }
    }
    // retire clock also runs when the crossfade is over: advanceCrossfade() is called every sample while either timer is active
public:
    bool listenBusy() const { return xfadeT >= 0.0 || retireT >= 0.0; }
private:

    // feedback guard: reads the wet and input level every 100 ms
    void guardInput (double l, double r, double w0, double w1)
    {
        gIn += 0.5 * (l * l + r * r); gWet += 0.5 * (w0 * w0 + w1 * w1);
        if (! guardOn) guardG += (1.0 - guardG) * guardRel;
        if (++gN < gBlockLen) return;
        const double inDb = 10.0 * std::log10 (gIn / gN + 1e-20), wDb = 10.0 * std::log10 (gWet / gN + 1e-20);
        gIn = gWet = 0; gN = 0;
        const bool silent = inDb < listen::kGuardSilentDb;
        gIdx = (gIdx + 1) & 31; gHist[gIdx] = wDb; ++gCount;
        silentBlocks = silent ? silentBlocks + 1 : 0;
        auto at = [&] (int back) { return gHist[(gIdx - back) & 31]; };
        if (p.feedback > 1.0f || runTargetGain > 1.0 || ! silent) { if (guardOn && ! silent) { guardOn = false; logEvent (kEvGuardOff, guardDb()); } return; }   // deliberate self-oscillation is left alone; input back = release
        if (! guardOn && silentBlocks >= listen::kGuardGrowBlocks && gCount > listen::kGuardGrowBlocks
            && wDb > listen::kGuardMinWetDb && wDb >= at (10) + listen::kGuardStepDb && at (10) >= at (20) + listen::kGuardStepDb)
        { guardOn = true; logEvent (kEvGuardOn, wDb); }
        if (guardOn && wDb > -100.0 && wDb > at (10) + listen::kGuardPullDb) guardG = std::max (listen::kGuardMinGain, guardG * listen::kGuardPullStep);
    }
};
}
