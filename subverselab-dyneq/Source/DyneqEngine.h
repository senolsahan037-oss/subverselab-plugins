// Dynamic EQ engine (collision cleaner): plain C++, no JUCE, so the offline renderer, the
// measurements and the plug-in all run this one file. See SPEC.md sections 1, 1b-B and 7.
//
// COLLIDE:
//   main in -> 32-band analysis (mono) -+
//                                        +-> signal law (SPEC 10) -> per-band target cut -> time smoothing
//   key in  -> 32-band analysis (mono) -+        -> FOCUS mask -> solved bell gains -> 32 minimum-phase bells on main -> MIX / DELTA
//   (the 0.9.1 law with its peak-preserving spread is kept as kLaw091, for dyneqraw --keylaw 091 only)
// SELF (peak hunter, SPEC 7.4):
//   main in -> 64-band analysis (half-ERB, mono) -> spectral envelope (median b-6..b-3 and b+3..b+6, 300 ms) -> excess
//           -> up to 4 local maxima above SENS (>= 3 bands apart) -> frequency (parabolic), Q (-3 dB width), cut
//           -> 4 hunter slots with hysteresis (1/6 oct), glide (1/12 oct per 10 ms), time smoothing -> 4 bells on main -> MIX / DELTA
//
// TARGET (SPEC 8b / 8c / 8d, TASK_P1g / P1h): after the free bands and the bank. The user names the source (INSTRUMENT) and a frequency PART of it
//   (BODY, SUB, CLICK ...). The fixed part ranges are SEARCH RANGES only: per detected hit the engine reads the transient (0-5 ms), early
//   (5-40 ms) and late (40-200 ms) windows of the material itself (FFT, autocorrelation), keeps a running median of the last 8 hits and cuts
//   where the learned part is. SPEC 8d: the engine ALWAYS listens (seven detectors, one memory each, whatever TARGET is set to, output with TARGET
//   OFF still bit-exact); the memory belongs to the instance, is saved with the project (memoryExport / memoryImport), is never reset by a slider,
//   and is replaced only when the sound changes (3 hits in a row > 1/3 oct away) or a loaded memory disagrees with the first hits' audio.
//   An empty memory ramps the cut in with its hit count (1/3, 2/3, full). A BODY / SUB cut starts after each hit's learned
//   transient end; a CLICK-type cut is armed between hits and released after the transient (the 1 dB/ms limit cannot reach 6 dB in 5 ms
//   otherwise). REVERB-READY is a static band-limit from the learned body. OFF (the default) touches nothing: bit-exact.
//
// The law runs once per 32-sample sub-block whatever the host block size; the bell gains ramp linearly over the
// following sub-block. Zero latency. When no bell is active the main buffer is not touched at all (bit-exact passthrough).
#pragma once

#include <algorithm>
#ifdef DYNEQ_PROFILE
#include <chrono>
#endif
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <vector>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace dyneq
{
enum Mode  : int { kCollide = 0, kSelf = 1 };
enum Speed : int { kFast = 0, kMedium = 1, kSlow = 2 };

// Six free dynamic bands (SPEC 1b-A), placed before the 32-band bank.
enum BandType : int { kBell = 0, kLowShelf, kHighShelf, kNotch, kLowCut, kHighCut };
enum Trigger  : int { kTrigSelf = 0, kTrigKey = 1 };
constexpr int kFreeBands = 6;

struct FreeBand
{
    bool  on        = false;
    int   type      = kBell;
    float freqHz    = 1000.0f;   // 20 .. 20000, and never above 0.45 x the sample rate (Engine::freeBandMaxFreqHz)
    float gainDb    = 0.0f;      // +-18 (bell, shelves; ignored by notch and the cuts)
    float q         = 1.0f;      // 0.1 .. 18. The cuts: Butterworth at Q = 1; the last (highest-Q) stage's Q is multiplied by Q, so Q > 1 adds resonance
    int   slopeDb   = 12;        // low / high cut only: 12, 24, 36, 48 or 72 dB per octave
    // optional DYNAMIC section (bell and shelves only)
    bool  dyn       = false;
    float thrDb     = -30.0f;    // -60 .. 0, on the trigger's level (a sine reads its peak dBFS) through a band-pass at FREQ / Q
    float rangeDb   = 0.0f;      // +-12, added to GAIN across a 6 dB knee above THRESHOLD (cut or boost)
    float attackMs  = 10.0f;     // 1 .. 100
    float releaseMs = 100.0f;    // 10 .. 1000
    int   trigger   = kTrigSelf; // the band's own input, or the sidechain key
};

struct Params
{
    FreeBand band[kFreeBands];
    int   mode        = kCollide;
    float depthDb     = 6.0f;      // 0 .. 12, largest cut in any band / hunter bell
    float marginDb    = 3.0f;      // 0 .. 9 (COLLIDE: how far the key should lead); SELF: SENS 0 .. 12 (default 3)
    // 2026-10-25: the research's default of 30 Hz (COMPETITOR_BEHAVIOUR_RESEARCH_2026-10-12.md, Recommendations / COLLIDE 1) was built and measured and NOT kept:
    // FOCUS also bounds SELF's hunters, and at 30 Hz SELF on plain pink noise cut 1.06 dB on average (0.35 at 120; one slot 2.48 dB, A13d wants < 0.5) and on the
    // drum bus 25 % of the hunters' work moved below 120 Hz (variation rig: "SELF on pink noise" 0.649 dB, limit 0.5). Lowering the minimum below 40 Hz changes
    // nothing audible (the lowest band centre is 40 Hz). Owner decision, STATUS.md 2026-10-25.
    float focusLowHz  = 120.0f;    // 40 .. 16000, bands (and hunter peaks) outside FOCUS are never cut
    // COLLIDE law (2026-10-26, tools/bench): 0 = kLawSignal, the signal law; 1 = kLaw091, the 0.9.0 / 0.9.1 SPEC 1 law.
    // 0.9.1-test2 ships kLaw091 (lead session 26 Oct): the signal law regresses vocal over beat on real material (measure.py A8 SNR +1.91 -> +0.85 dB,
    // STATUS 2026-10-25/26); it stays selectable in dyneqraw (--keylaw signal) until that is solved
    int   keyLawMode  = 1;
    float focusHighHz = 8000.0f;
    int   speed       = kMedium;   // release 40 / 120 / 300 ms (RMS envelope and cut), attack fixed 5 ms
    float mix         = 1.0f;      // 0 .. 1, dry is the same signal (zero latency)
    bool  delta       = false;     // hear only what is removed: dry - wet
    // COLLIDE only. true: the 32 bell gains are solved (Gauss-Seidel on the bells' dB shapes) so the composite response at
    // every band centre equals the target cut (SPEC 7.2). false: each bell's gain is -cut_b literally; overlapping bells add in dB.
    bool  solveBells  = true;
    // SPEC 7.3 A/B. false: the 32 + 64 analysis band-passes are -12 dB at the band edges (default). true: -3 dB (tiling).
    bool  tiling      = false;
    // TARGET (SPEC 8). targetInst OFF = the layer does not run at all.
    int   targetInst   = 0;        // TargetInst: OFF, VOCAL, BASS, DRUMS, KICK, SNARE, HATS
    float targetPart   = 50.0f;    // 0 .. 100: steps through the parts of the instrument (equal segments, see partIndex)
    float targetAmount = 6.0f;     // 0 .. 18 dB, deepest cut
    float targetWidth  = 1.0f;     // 0.5 .. 2, scales the learned Q
    float targetSweet  = 120.0f;   // 20 .. 400 ms, the release of the cut
    bool  reverbReady  = false;
    int   targetSlope  = 24;       // SPEC 8e.2: 12 / 24 / 36 / 48 / 72 dB per octave of every edge cut (SUB, CLICK, REVERB-READY low and high)
};

constexpr int   kBands = 32;
constexpr int   kBlock = 32;                    // samples per law update
constexpr float kLowHz = 40.0f, kHighHz = 16000.0f;
constexpr float kAttackMs = 5.0f;
constexpr float kReleaseMs[3] = { 40.0f, 120.0f, 300.0f };
constexpr float kRiseDbPerMs = 1.0f;            // a cut may not rise faster than this
constexpr float kKeyFloorDb = -60.0f;           // K_b (COLLIDE) and M_b (SELF, 64-band) must be above this to count
constexpr float kPresenceLoDb = -30.0f, kPresenceHiDb = -18.0f;   // the 0.9.1 COLLIDE law (kLaw091): soft knee of keyRel -> presence
enum KeyLawMode : int { kLawSignal = 0, kLaw091 = 1 };
// ---- the signal law (2026-10-26). Every constant is measured against the synthetic bench's ground truth (tools/bench/README.md) or cited.
//   presence: a band counts for a signal only with energy of its OWN (not what a neighbouring band's component leaks into it) and within kSigNDb of that
//   signal's own loudest band; WHETHER to cut = both present, from levels averaged over an auditory filter's time resolution, so a cut ends when the key ends.
//   HOW MUCH = the SPEC 1 need (M - K) + MARGIN on the same levels held at their peak and released with SPEED (a fading key never makes the cut swell).
//   a low-end key (kick, 808): in the band of its loudest component, under kSigLowTopHz, the main is ducked by DEPTH whoever leads (research section 7).
//   No spread into neighbouring bands (SPEC 7.1's spread put half a cut into bands with no collision; the bells are 1.4 bands wide anyway).
constexpr float kSigNDb = 20.0f;          // a key band counts while it stays within this of its own recent level in that band: once a hit's level has fallen 20 dB, what
                                          // is left of an exponential decay holds 1 % of its energy (the bench's ground truth uses the same N and hold)
constexpr float kSigKneeDb = 12.0f;       // presence rises from 0 at (reference - kSigNDb) to 1 kSigKneeDb above: the SPEC 1 knee's width, ending where the ground truth ends
constexpr float kSigOwnKneeDb = 6.0f;     // leakage: 0 when the band reads no more than the other bands would leak into it, 1 at 6 dB over it (3/4 of the power its own)
constexpr float kSigRefDecayDbPerS = 20.0f;  // the key band's reference is a peak hold that falls this fast: slower than the fastest decay it judges (a kick, 120 ms:
                                             // 72 dB/s, so its tail still drops out of presence), fast enough that a key section 20 dB quieter counts again within 1 s
                                             // (plugin_check 13.3b: with 2 dB/s a fresh instance and one that had played loud still differed 12 s later)
constexpr float kSigEdgeZoneErb = 0.075f;    // measured on this bank (cascade, -12 dB edges): within 0.075 ERB of a band edge the two neighbouring bands read a tone within
                                             // 3 dB of each other, so a component there belongs to both (the bench's ground truth uses the same zone)
constexpr float kSigUnmaskDb = 4.0f;         // the masking threshold of a tone in noise within one auditory filter, signal to masker (Patterson 1976; Moore, power-spectrum model)
constexpr float kSigLoneDb = 9.0f;           // a band (or pair) holds ONE lone component when it stands this far above the band beyond it (one tone: ~18 dB, flat noise: 0 dB)
constexpr float kSigLoneLeakDb = kSigLoneDb;   // (26 Oct: 15 / 20 / 30 dB measured on the demo vocal over beat: A8-like SNR +0.89 / +0.91 / +0.91 against +0.82 dB; not changed)
constexpr int   kSigLeakSpan = 3;
constexpr int   kSigDoubleBands = 8;         // analysis in double up to band 7 (546 Hz): the float states of 40 / 171 / 292 Hz never reconverged (analyseD); float above            // the leakage test looks this many bands each way (three bands away a component reads under -40 dB)
// The low-end rule's top: research section 7, Mastrng "Overlap below ~100-200 Hz" (the upper end), iZotope's kick/bass example "masking from about 150 Hz and below"
// (section 3). Bands 40 / 78 / 121 / 171 Hz.
constexpr float kSigLowTopHz = 200.0f;
constexpr float kSpreadFactor = 0.5f;           // SPEC 7.1: cut'_b = max(cut_b, 0.5 cut_{b-1}, 0.5 cut_{b+1})
// SELF peak hunter (SPEC 7.4)
constexpr int   kHBands = 64;                   // half-ERB spacing over the same 40 Hz .. 16 kHz
constexpr int   kHunters = 4;
constexpr int   kBellGroup = 4;                 // bank bells run together in the skewed pipeline (apply)
constexpr int   kHMedNear = 3, kHMedFar = 6;   // spectral envelope = median over b-6 .. b-3 and b+3 .. b+6 (the band's own +-2 are left out)
constexpr float kHEnvMs = 300.0f;               // ... smoothed over 300 ms
constexpr int   kHMinSep = 3;                   // picked peaks are at least this many 64-bands apart
constexpr float kHHysteresisOct = 1.0f / 6.0f;  // a slot keeps its peak while it stays within this
constexpr float kHGlideOctPer10ms = 1.0f / 12.0f;
constexpr float kHQMin = 2.0f, kHQMax = 16.0f;
constexpr float kHQSmoothMs = 20.0f;            // the slot's Q follows the measured width through this one-pole (implementation choice)
constexpr float kHPresenceLoDb = -60.0f, kHPresenceHiDb = -48.0f;   // SELF presence: soft knee on the 64-band level (M_b) above the floor
#ifndef DYNEQ_ANALYSIS_EDGE_DB
#define DYNEQ_ANALYSIS_EDGE_DB 6.0f
#endif
constexpr float kAnalysisEdgeDb = DYNEQ_ANALYSIS_EDGE_DB;   // each of the two band-pass stages is this far down at the band edges (both banks). 6: cascade -12 dB at the edges, -22 dB at the neighbour's centre. 1.5: -3 dB at the edges (tiling). Both measured in STATUS.md
constexpr float kBellBwScale = 1.4f;   // (2026-10-26 bench: 1.0 / 0.7 were measured as the skirt trade-off, docs/audit/SIMPLIFY_PLAN_2026-10-25.md; not changed)            // COLLIDE bell -3 dB bandwidth = this x the band width (Q = fc / (scale x width))
constexpr float kShapeRefDb = 6.0f;             // the bell shapes for the solve are measured at this cut
constexpr int   kSolveIters = 8;
constexpr float kDbFloor = -200.0f;

// ---- TARGET parts (SPEC 8b / 8c). The ranges are SEARCH RANGES and priors; the cut is placed from the material (fish memory).
enum TargetInst  : int { kTgtOff = 0, kTgtVocal, kTgtBass, kTgtDrums, kTgtKick, kTgtSnare, kTgtHats };
enum TargetVoice : int { kVoKick = 0, kVoSnare, kVoHats, kVoBass, kVoVocal };
// kPkSub: centre = the fundamental. kPkBody: early window minus the fundamental, above 1.5 x fundamental. kPkBand: early window spectrum.
// kPkTrans: transient window minus early window (CLICK / CRACK / SNAP / CHICK). SPEC 8f: every cut is static (no per-hit gating); holdMs is unused since P1j.
enum PartKind : int { kPkSub = 0, kPkBody, kPkBand, kPkTrans };
constexpr int kTargetInsts = 7, kTargetVoices = 5, kMaxParts = 5, kMemHits = 8, kTargetBells = 3;   // 11 Oct: a part that is not an edge is cut by three bells (a plateau), was one
// SPEC 8d: the engine always listens. One detector + memory per (voice, bus) pair that any preset can ask for: the five voices alone, plus the kick and the
// snare as the DRUMS bus hears them (the hats are the same detector alone and in the bus).
constexpr int kTSlots = 7;
constexpr int kSlotVoice[kTSlots] = { 0, 1, 2, 3, 4, 0, 1 };
constexpr bool kSlotBus[kTSlots]  = { false, false, false, false, false, true, true };
constexpr int kDetStep = 16;        // the detector's followers update once per this many samples
constexpr int kJobEveryTicks = 4;   // one hit analysis per this many 32-sample law ticks
constexpr int kMemFormat = 2;       // the saved memory's layout; a different number is ignored on load
// edge: 0 = the part is cut by a bell (SOFT edge); 1 = an edge cut, the cut band is everything BELOW the part's upper edge (SUB); 2 = an edge cut, the cut band is everything ABOVE the part's lower edge (CLICK)
struct PartDef { const char* name; float lo, hi; int kind; float holdMs; int edge; };
struct VoiceDef
{
    const char* name;
    float detLo, detHi;       // onset detector band when the instrument is chosen alone (its overall range)
    float busLo, busHi;       // ... inside the DRUMS bus
    float rejLo, rejHi;       // bus only: an onset is dropped when this band is louder than half the detector band 6 ms later (a kick's boom starting the snare detector, a snare starting the kick detector); 0 = none
    float f0Lo, f0Hi;         // fundamental search range (autocorrelation of the late window); 0 = the voice has none
    float guardHz;            // the transient's end is read above this
    float fastMs, sensDb, gapMs;   // detector: fast follower, onset = fast / slow (60 ms) above sensDb, minimum gap between onsets
    int   keyPart;            // the part whose centre tracks "the same sample" when the voice has no fundamental
    int   n; PartDef part[kMaxParts];
    int   revBody, revTop;    // REVERB-READY: low-cut at 0.8 x the lower edge of part revBody, high-cut at the upper edge of part revTop; -1 = none
};
// Part ranges checked against published mixing guides on 12 Oct (docs/audit/TARGET_PART_RANGES_RESEARCH_2026-10-12.md; owner: "neye göre body kick ... doğru presetleri ekle"):
// KICK body 60-150 (was 60-250, it landed in the mud), + MUD 100-250, + BOX 200-500; SNARE body 140-300, + BOX 300-600, crack 0.9-3.5 kHz; HATS + BITE 3-6 kHz;
// BASS body 80-250, + MUD 200-400; VOCAL box 350-900 (was 150-600: chest, not box), nasal 0.8-1.6 kHz, harsh 2-4.5 kHz, S 5-10 kHz.
constexpr VoiceDef kVoice[kTargetVoices] =
{
    { "KICK",  30, 8000,   30, 150,   800, 3500,    30, 90,   1500.0f, 6.0f, 8.0f, 60.0f, 0, 5,
      { { "SUB", 30, 90, kPkSub, 200, 1 }, { "BODY", 60, 160, kPkBody, 80, 0 }, { "MUD", 100, 250, kPkBand, 80, 0 }, { "BOX", 200, 500, kPkBand, 80, 0 }, { "CLICK", 1500, 8000, kPkTrans, 0, 2 } }, -1, -1 },
    { "SNARE", 120, 10000, 800, 3500,  30, 110,   120, 350,  800.0f, 6.0f, 8.0f, 60.0f, 0, 4,
      { { "BODY", 140, 300, kPkBand, 200, 0 }, { "BOX", 300, 600, kPkBand, 200, 0 }, { "CRACK", 900, 3500, kPkTrans, 0, 0 }, { "SNAP/AIR", 4000, 10000, kPkTrans, 0, 2 } }, 0, 2 },
    { "HATS",  4000, 14000,   4000, 14000,   0, 0,    0, 0,   3000.0f, 3.0f, 8.0f, 25.0f, 2, 3,
      { { "CHICK", 250, 1000, kPkTrans, 0, 0 }, { "BITE", 3000, 6000, kPkBand, 120, 0 }, { "SHIMMER", 6000, 14000, kPkBand, 120, 0 } }, -1, -1 },
    { "BASS",  30, 2000,   30, 2000,   0, 0,    30, 150,  500.0f, 6.0f, 8.0f, 80.0f, 0, 4,
      { { "SUB", 30, 90, kPkSub, 300, 1 }, { "BODY", 80, 250, kPkBody, 200, 0 }, { "MUD", 200, 400, kPkBand, 200, 0 }, { "TOP", 500, 2000, kPkTrans, 0, 2 } }, -1, -1 },
    { "VOCAL", 150, 10000,   150, 10000,   0, 0,    0, 0,   2000.0f, 6.0f, 6.0f, 80.0f, 0, 4,
      { { "BOX", 350, 900, kPkBand, 300, 0 }, { "NASAL", 800, 1600, kPkBand, 300, 0 }, { "HARSH", 2000, 4500, kPkBand, 200, 0 }, { "S", 5000, 10000, kPkBand, 150, 0 } }, 0, 2 },
};
struct SlotDef { int voice, part; };
struct InstDef { const char* name; int n; SlotDef slot[kMaxParts]; bool bus, reverbOk; };
constexpr InstDef kInst[kTargetInsts] =
{
    { "OFF", 0, {}, false, false },
    { "VOCAL", 4, { { kVoVocal, 0 }, { kVoVocal, 1 }, { kVoVocal, 2 }, { kVoVocal, 3 } }, false, true },
    { "BASS",  4, { { kVoBass, 0 }, { kVoBass, 1 }, { kVoBass, 2 }, { kVoBass, 3 } }, false, false },
    { "DRUMS", 3, { { kVoKick, 1 }, { kVoSnare, 0 }, { kVoHats, 2 } }, true, false },    // KICK BODY, SNARE BODY, HATS SHIMMER, each its own voice and memory
    { "KICK",  5, { { kVoKick, 0 }, { kVoKick, 1 }, { kVoKick, 2 }, { kVoKick, 3 }, { kVoKick, 4 } }, false, false },
    { "SNARE", 4, { { kVoSnare, 0 }, { kVoSnare, 1 }, { kVoSnare, 2 }, { kVoSnare, 3 } }, false, true },
    { "HATS",  3, { { kVoHats, 0 }, { kVoHats, 1 }, { kVoHats, 2 } }, false, false },
};
constexpr const char* kDrumsPartNames[3] = { "KICK BODY", "SNARE BODY", "HATS SHIMMER" };
inline int   partIndex (int inst, float slider)   // slider 0..100 -> part: equal segments
{
    inst = std::clamp (inst, 0, kTargetInsts - 1); const int n = kInst[inst].n; if (n <= 0) return 0;
    return std::clamp ((int) std::floor (std::clamp (slider, 0.0f, 100.0f) * 0.01f * (float) n), 0, n - 1);
}
inline float sliderForPart (int inst, int idx) { const int n = std::max (1, kInst[std::clamp (inst, 0, kTargetInsts - 1)].n); return 100.0f * ((float) idx + 0.5f) / (float) n; }
inline const char* partName (int inst, int idx)
{
    inst = std::clamp (inst, 0, kTargetInsts - 1); const InstDef& I = kInst[inst]; if (I.n <= 0) return "";
    idx = std::clamp (idx, 0, I.n - 1);
    return inst == kTgtDrums ? kDrumsPartNames[idx] : kVoice[I.slot[idx].voice].part[I.slot[idx].part].name;
}
constexpr float kTargetSlowMs = 60.0f;              // the detector's slow (background) power follower
constexpr float kTargetF0MinShare = 0.2f;           // a fundamental must carry this share of the (low-passed) late window's energy
constexpr float kTargetSlowDelayMs = 10.0f;         // the onset compares the fast level with the slow level this long ago
constexpr float kTargetFloor = 1e-7f;               // mean square under this (-70 dBFS) never starts an event
constexpr float kTargetWarmMs = 100.0f;             // after the voice changes in a running stream the detector settles this long before an onset can count
constexpr float kTargetPreRollMs = 1.0f;            // the analysis windows start this long before the detected onset
// the evidence collector (SPEC 8e.3)
constexpr int   kDec = 8;                           // the idle onset detector works on groups of this many samples
constexpr float kColFastMsA = 5.0f, kColFastMsH = 3.0f;   // fast followers: the power of the mono sum, the power of its first difference
constexpr float kColSensDb = 9.0f, kColGapMs = 40.0f;      // onset: fast over the slow level 10 ms ago by this; no second onset within kColGapMs
constexpr float kColSlowDelayMs = 10.0f;
constexpr float kColSettleMs = 8.0f;                // a hit is located and (DRUMS bus) classified this long after its onset
constexpr float kColPreRollMs = 0.7f;               // the analysis windows start this long before the hit's first sample
constexpr float kTargetTransMs = 5.0f, kTargetEarlyMs = 40.0f, kTargetLateMs = 200.0f;   // windows after the onset: 0-5, 5-40, 40-200 ms
constexpr float kTargetNewSampleOct = 1.0f / 3.0f;  // a hit this far from the memory's median key, 3 in a row, is a new sample
constexpr float kTargetQuickAgreeOct = 1.0f / 12.0f; // two far hits this close to each other replace the memory at once (SPEC 8d 7)
constexpr int   kTargetMinHits = 3;                 // before this many hits: no cut
constexpr float kTargetBwScale = 0.9f, kTargetBwScaleBody = 0.70f;   // (the single bell up to 10 Oct) its bandwidth = this x the learned -6 dB extent (octaves)
// 11 Oct (owner: "kick body sectim, yarim cut etti"): one bell reaches AMOUNT only at its centre (KICK BODY at AMOUNT 12 measured -10.3 dB at 100 Hz, -5 dB a third of an
// octave away; every non-edge part reached full depth over 0.17 - 0.5 octave only). A part that is not an edge is now a PLATEAU of three RBJ bells, spaced evenly in log
// frequency over the part, one Q, one gain solved so the mean cut between the outer centres is AMOUNT. Minimum phase like the bell (a Linkwitz-Riley pair was tried: flat,
// but it moved the kick's low end by 17-19 ms, against 1.1 ms for the bell and 2.9 ms for the three bells; docs/audit/TARGET_AUDIT_2026-10-11.md).
constexpr float kPlateauSpan = 1.6f;          // octaves between the outer bells = this x the learned -6 dB extent x WIDTH
constexpr float kPlateauMinOct = 1.0f, kPlateauMaxOct = 3.0f;
constexpr float kPlateauQMul = 0.75f;          // Q = this / (2^(s/2) - 2^(-s/2)), s = the spacing of the bells in octaves (neighbours meet near their half-cut points)
constexpr float kPlateauRefDb = 12.0f;        // the gain solve and the SUB protection are computed at this cut (protection at kBellMaxDb: the deepest a plateau goes)
constexpr float kTargetValidPsd = 1e-8f;            // a part counts as present when its smoothed PSD peak is above this (about -80 dB white noise)
// SPEC 8e.2: every edge cut is a Linkwitz-Riley split at the learned edge; the cut band's gain is -AMOUNT, the other band's 0 dB.
constexpr int   kSlopeSteps = 5;
constexpr int   kSlopeDb[kSlopeSteps] = { 12, 24, 36, 48, 72 };
constexpr float kTargetAmountMaxDb = 60.0f;         // AMOUNT max: the cut band reaches a real high-pass / low-pass (<= -60 dB)
constexpr float kBellMaxDb = 18.0f;                 // a bell (SOFT edge) never cuts deeper than this
constexpr float kEdgeBelowShare = 1.2f;             // SUB: the crossover sits at 1.2 x the sub's upper edge (1.5 x f0) = 1.8 x f0, so the whole sub band is inside the cut band
constexpr float kEdgeAboveShare = 0.8f;             // CLICK: the crossover sits at 0.8 x the click's lower edge
constexpr float kRevLowShare = 0.6f;                // REVERB-READY: low corner = this x the body's lower edge (the crossover is wide: the body must stay untouched at the default slope)
constexpr float kRevHighShare = 1.25f;              // REVERB-READY: high corner = this x the crack's upper edge
constexpr float kRevGlideOctPerBlockMs = (1.0f / 12.0f) / 100.0f;   // corner glide: 1/12 octave per 100 ms
constexpr float kEdgeMixUpMs = 6.0f, kEdgeMixDownMs = 12.0f;        // the split fades in while a cut is wanted and out when it is gone (x <-> the split's all-pass sum)
constexpr float kCutHoldOct = 1.0f / 96.0f;                // SPEC 8f: a learned point that wobbles less than this from the cut's own never moves the cut (the median's own jitter is not a sound change)
constexpr float kEdgeGlideOctPerBlockMs = (1.0f / 3.0f) / 100.0f;   // a real sound change moves an edge at most 1/3 octave per 100 ms (a continuous coefficient ramp, never a swap)
constexpr float kEdgeSlopeFadeMs = 30.0f;           // a SLOPE change crossfades from the old filter set to the new one over this
inline int protectPart (int voice) { return voice == kVoKick ? 1 : voice == kVoSnare ? 0 : voice == kVoBass ? 1 : -1; }   // SPEC 8g: the part an edge cut must not eat (BODY; the vocal and hats have no edge parts)
inline int slopeIndex (int db) { int best = 1, d = 1000; for (int i = 0; i < kSlopeSteps; ++i) { const int e = std::abs (db - kSlopeDb[i]); if (e < d) { d = e; best = i; } } return best; }
// SPEC 8g: the exact response of one edge split (the engine's EdgeBank: Butterworth n squared, bilinear, the cut band at -cutDb, the other at 0 dB). Positive result = dB of cut.
inline double edgeCutDbAt (double f, double fx, int slopeIdx, double cutDb, bool below, double sr)
{
    static const double qs[kSlopeSteps][3] = { { 0.0, 0, 0 }, { 0.70710678118654752, 0, 0 }, { 0.0, 1.0, 0 }, { 0.54119610014619698, 1.30656296487637653, 0 }, { 0.51763809020504152, 0.70710678118654752, 1.93185165257813657 } };
    static const int cnt[kSlopeSteps] = { 1, 1, 2, 2, 3 }, ord[kSlopeSteps] = { 1, 2, 3, 4, 6 };
    const double t = std::tan (M_PI * std::min (f, 0.499 * sr) / sr) / std::tan (M_PI * std::min (fx, 0.49 * sr) / sr);
    const std::complex<double> s (0.0, t); std::complex<double> B (1.0, 0.0);
    for (int j = 0; j < cnt[slopeIdx]; ++j) B *= qs[slopeIdx][j] <= 0.0 ? s + 1.0 : s * s + s / qs[slopeIdx][j] + 1.0;
    const std::complex<double> B2 = B * B, lo = 1.0 / B2, hi = std::pow (s, 2 * ord[slopeIdx]) / B2;
    const double c = cutDb >= kTargetAmountMaxDb - 1e-3 ? 0.0 : std::pow (10.0, -cutDb / 20.0), sgn = slopeIdx == 0 || slopeIdx == 2 ? -1.0 : 1.0;
    const std::complex<double> H = below ? hi + sgn * c * lo : lo + sgn * c * hi;
    return -20.0 * std::log10 (std::max (std::abs (H), 1e-12));
}
// SPEC 8h: the exact magnitude (dB) of an RBJ peaking bell (gain gainDb, Q q at f0) at frequency f, the engine's bell
inline double bellDbAt (double f, double f0, double q, double gainDb, double sr)
{
    const double A = std::pow (10.0, gainDb / 40.0), w0 = 2.0 * M_PI * f0 / sr, al = std::sin (w0) / (2.0 * q), c = std::cos (w0);
    const double b0 = 1.0 + al * A, b1 = -2.0 * c, b2 = 1.0 - al * A, a0 = 1.0 + al / A, a1 = -2.0 * c, a2 = 1.0 - al / A;
    const double w = 2.0 * M_PI * std::min (f, 0.499 * sr) / sr; const std::complex<double> z1 = std::polar (1.0, -w), z2 = std::polar (1.0, -2.0 * w);
    return 20.0 * std::log10 (std::max (std::abs ((b0 + b1 * z1 + b2 * z2) / (a0 + a1 * z1 + a2 * z2)), 1e-12));
}
constexpr float kProtectLossDb = 0.9f;     // SPEC 8g: the protected neighbour part's centre loses at most 1 dB; the edge is placed for 0.9 dB, so the 1.0 dB limit holds with a margin for the analysis window
constexpr float kGentleMarginDb = 1.0f;    // "slope too gentle" when the cut at the target part's centre is under AMOUNT - this
// Places an edge crossover so that the protected part's centre loses <= kProtectLossDb at this SLOPE and AMOUNT: from the rule's crossover (defF) it moves away from the
// protected part (SUB-type: down, CLICK-type: up) in 1/96-octave steps until the loss at protF is small enough, then bisects the last step.
inline float edgePlaceHz (bool below, float defF, float protF, int slopeIdx, float amountDb, double sr)
{
    const double lim = (double) kProtectLossDb; const double fMin = 20.0, fMax = 0.45 * sr;
    if (protF <= 0.0f || amountDb <= lim) return defF;
    auto bad = [&] (double fx) { return edgeCutDbAt (protF, fx, slopeIdx, amountDb, below, sr) > lim; };   // the cut at the protected centre is deeper than allowed
    double f = defF; const double st = std::exp2 (below ? -1.0 / 96.0 : 1.0 / 96.0);
    if (! bad (f)) return defF;
    double prev = f;
    while (bad (f) && (below ? f > fMin : f < fMax)) { prev = f; f *= st; }
    if (bad (f)) return (float) std::clamp (f, fMin, fMax);
    double a = prev, b = f;   // a fails, b passes
    for (int i = 0; i < 10; ++i) { const double m = std::sqrt (a * b); (bad (m) ? a : b) = m; }
    return (float) b;
}
// what the engine has learned from the material (median of the last hits), for the panel and the tests. Frequencies in Hz, fractions in 0..1.
struct TargetLearned
{
    bool  ready = false;               // at least kTargetMinHits hits in memory
    int   hits = 0;                    // hits in memory (0 .. 8)
    float confidence = 0.0f;           // hits / 8 x (1 - the interquartile spread of the key, in octaves / 0.5)
    float f0 = 0.0f;                   // the fundamental of the boom (voices that have one), 0 = none
    float transEndMs = 0.0f;           // the learned transient end, ms after the detected onset
    int   nParts = 0;
    bool  ok[kMaxParts] {};            // the part was found in at least kTargetMinHits hits
    float centre[kMaxParts] {}, lo[kMaxParts] {}, hi[kMaxParts] {};   // part centre and the edges of its -6 dB extent
};
// BAKE statistics
constexpr float kFocusFadeMs = 12.0f;           // a band that enters or leaves FOCUS fades its cut over this (12 dB at most: 1 dB/ms), so moving FOCUS never clicks
constexpr float kBakeGateDb = -60.0f;           // a block counts when the main mono sum is at or above this RMS level
constexpr float kBakeCutSeenDb = 0.1f;          // 'cut' for the percentage of time
// free bands
constexpr float kBandKneeDb = 6.0f;             // gain = GAIN + RANGE x clamp((L - THR) / knee, 0, 1)
constexpr float kBandRiseDbPerMs = 1.0f;        // a band's gain never moves faster than this (either direction)
constexpr float kBandGainSmoothMs = 2.0f;       // one-pole after the rate limit: removes the kinks of the limiter's ramps
constexpr float kBandSwitchFadeMs = 5.0f;       // a band that stops at once (notch / cut off, type or slope change) crossfades from the old filter's output over this
constexpr float kBandShapeMs = 10.0f;           // FREQ / Q changes glide through this one-pole
constexpr float kPeakFromRmsDb = 3.0103f;       // a sine's mean square + 3.01 dB = its peak dBFS
// Butterworth stage Q for the 12 / 24 / 48 dB per octave cuts (1, 2, 4 biquads)
constexpr double kButterQ[7] = { 0.70710678118654752, 0.54119610014619698, 1.30656296487637653,
                                 0.50979557910415917, 0.60134488693504529, 0.89997622313641570, 2.5629154477415055 };

constexpr int   kFTick = 8;                     // free-band parameter / coefficient tick (samples, on the absolute grid)
constexpr float kBandPrerollMs = 4.0f;          // a new filter of a type / slope change (or a cut that engages) is run over this much of its input history first
constexpr float kBandFreqSmoothMs = 15.0f;      // FREQ / Q: three cascaded poles, this long in total (the old one-pole of 10 ms took 30 ms to settle; so does this)
constexpr float kBandUserGainMs = 6.0f;         // GAIN: three cascaded poles, this long in total (the 1 dB/ms limiter follows)
constexpr float kBandDetMinWinMs = 2.0f;        // the dynamic detector averages y^2 over a whole number of periods of FREQ, at least this long
constexpr float kBandFadeMs = 20.0f;            // equal-power fade of a band that engages from dry or changes type / slope
constexpr int   kHistN = 2048;                  // input history ring (samples)
constexpr int   kFreeSlopeCount = 5;
constexpr int   kFreeSlopeDb[kFreeSlopeCount] = { 12, 24, 48, 36, 72 };   // SLOPE choice order: the first three are the old ones, so saved states load unchanged
inline int freeSlopeDbFromIndex (int idx) { return kFreeSlopeDb[idx < 0 ? 0 : idx >= kFreeSlopeCount ? kFreeSlopeCount - 1 : idx]; }
inline int freeSlopeIndexFromDb (int db) { for (int i = 0; i < kFreeSlopeCount; ++i) if (kFreeSlopeDb[i] == db) return i; return db >= 60 ? 4 : db >= 42 ? 2 : db >= 30 ? 3 : db >= 18 ? 1 : 0; }

// ---- P1n: free-band filter design. Matched second-order sections (Vicanek 2016 style): the poles are the analog poles mapped by z = exp(sT);
// the numerator is solved so that |H| equals the analog magnitude at DC and at two more frequencies (the analog one at the band's own corner / zero and
// one near Nyquist). A gain type is designed for its BOOST; a cut is the exact inverse (poles and zeros swapped), so boost and cut mirror exactly.
// A design that is not finite or whose numerator is not minimum phase falls back to the RBJ bilinear section (always stable).
namespace fdesign
{
constexpr int kMaxStages = 6;                  // 72 dB per octave = order 12 = six biquads
struct Bq { double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0; };   // a0 = 1
enum { tBell = 0, tLowShelf, tHighShelf, tNotch, tLowCut, tHighCut };
constexpr double kPi = 3.14159265358979323846;

inline double sq (double x) { return x * x; }
inline bool fin (const Bq& c) { return std::isfinite (c.b0) && std::isfinite (c.b1) && std::isfinite (c.b2) && std::isfinite (c.a1) && std::isfinite (c.a2); }
// numerator zeros inside the unit circle (b0 > 0)
inline bool minPhase (double b0, double b1, double b2) { return b0 > 0.0 && std::abs (b2) < b0 * 0.9999999 && std::abs (b1) < (b0 + b2) * 0.9999999; }

// the analog section s^2 + s / Q + 1 at natural frequency wp (rad per sample), poles mapped by z = exp(sT)
inline void polesZ (double wp, double Qp, double& a1, double& a2)
{
    wp = std::min (wp, 0.99 * kPi);
    const double z = 0.5 / Qp, e = std::exp (-z * wp);
    a2 = e * e;
    a1 = z < 1.0 ? -2.0 * e * std::cos (wp * std::sqrt (1.0 - z * z)) : -2.0 * e * std::cosh (wp * std::sqrt (z * z - 1.0));
}
inline double dmag2 (double a1, double a2, double w)   // |1 + a1 e^-jw + a2 e^-2jw|^2 in the phi basis (no cancellation at low w)
{
    const double s = std::sin (0.5 * w), c = std::cos (0.5 * w);
    return sq (1.0 + a1 + a2) * c * c + sq (1.0 - a1 + a2) * s * s - 4.0 * a2 * sq (std::sin (w));
}

// numerator (b0, b1, b2) for the denominator (1, a1, a2) with |H|^2 = H0 at DC, HA at wA, HB at wB
inline bool matchNum (double a1, double a2, double H0, double wA, double HA, double wB, double HB, double& b0, double& b1, double& b2, bool zeroAtNyquistOk = false)
{
    const double B0 = sq (1.0 + a1 + a2) * H0;
    const double sA = std::sin (0.5 * wA), cA = std::cos (0.5 * wA), sB = std::sin (0.5 * wB), cB = std::cos (0.5 * wB);
    const double pA0 = cA * cA, pA1 = sA * sA, pA2 = 4.0 * pA0 * pA1, pB0 = cB * cB, pB1 = sB * sB, pB2 = 4.0 * pB0 * pB1;
    const double rA = dmag2 (a1, a2, wA) * HA - B0 * pA0, rB = dmag2 (a1, a2, wB) * HB - B0 * pB0;
    const double det = pA1 * pB2 - pA2 * pB1;
    if (! (std::abs (det) > 1e-300)) return false;
    double B1 = (rA * pB2 - pA2 * rB) / det; const double B2 = (pA1 * rB - rA * pB1) / det;
    if (zeroAtNyquistOk) B1 = std::max (B1, 0.0);
    if (! (B0 > 0.0) || ! (B1 > 1e-7 * B0 || (zeroAtNyquistOk && B1 >= 0.0))) return false;
    const double W0 = std::sqrt (B0), W1 = std::sqrt (B1), S = 0.5 * (W0 + W1), d = S * S + B2;
    if (! (d >= 0.0)) return false;
    const double r = std::sqrt (d);
    b0 = 0.5 * (S + r); b1 = 0.5 * (W0 - W1); b2 = 0.5 * (S - r);
    return true;
}

inline Bq invertBq (const Bq& c) { Bq o; o.b0 = 1.0 / c.b0; o.b1 = c.a1 / c.b0; o.b2 = c.a2 / c.b0; o.a1 = c.b1 / c.b0; o.a2 = c.b2 / c.b0; return o; }

// RBJ bilinear section (the old design): fallback and reference
inline Bq rbj (int type, double A, double w0, double Q)
{
    const double cw = std::cos (w0), al = std::sin (w0) / (2.0 * Q);
    double a0, b0, b1, b2, a1, a2;
    if (type == tBell) { a0 = 1.0 + al / A; b0 = 1.0 + al * A; b1 = -2.0 * cw; b2 = 1.0 - al * A; a1 = b1; a2 = 1.0 - al / A; }
    else if (type == tLowShelf || type == tHighShelf)
    {
        const double Ap = A + 1.0, Am = A - 1.0, t = 2.0 * std::sqrt (A) * al;
        if (type == tLowShelf) { a0 = Ap + Am * cw + t; a1 = -2.0 * (Am + Ap * cw); a2 = Ap + Am * cw - t; b0 = A * (Ap - Am * cw + t); b1 = 2.0 * A * (Am - Ap * cw); b2 = A * (Ap - Am * cw - t); }
        else                   { a0 = Ap - Am * cw + t; a1 = 2.0 * (Am - Ap * cw); a2 = Ap - Am * cw - t; b0 = A * (Ap + Am * cw + t); b1 = -2.0 * A * (Am + Ap * cw); b2 = A * (Ap + Am * cw - t); }
    }
    else if (type == tNotch)  { a0 = 1.0 + al; a1 = -2.0 * cw; a2 = 1.0 - al; b0 = 1.0; b1 = -2.0 * cw; b2 = 1.0; }
    else if (type == tLowCut) { a0 = 1.0 + al; a1 = -2.0 * cw; a2 = 1.0 - al; b0 = 0.5 * (1.0 + cw); b1 = -(1.0 + cw); b2 = b0; }
    else                      { a0 = 1.0 + al; a1 = -2.0 * cw; a2 = 1.0 - al; b0 = 0.5 * (1.0 - cw); b1 = 1.0 - cw; b2 = b0; }
    const double ia = 1.0 / a0; Bq o; o.b0 = b0 * ia; o.b1 = b1 * ia; o.b2 = b2 * ia; o.a1 = a1 * ia; o.a2 = a2 * ia; return o;
}

// ---- boost designs. A = 10^(gain / 40) >= 1, w0 = 2 pi f / fs. false = no valid matched design (the caller falls back).
inline bool bellBoost (double w0, double A, double q, Bq& c)
{
    double a1, a2; polesZ (w0, A * q, a1, a2);
    const double o0 = std::min (std::max (0.85 * kPi, w0 + 0.08 * kPi), 0.99 * kPi), tries[4] = { o0, 0.93 * kPi, 0.78 * kPi, 0.985 * kPi };   // the first try is continuous in w0; the others are fallbacks that should not be needed
    const double A2 = A * A, q2 = q * q;
    for (double wm : tries)
    {
        if (wm <= w0 + 0.02 * kPi) continue;
        const double o = wm / w0, o2 = o * o, u = sq (1.0 - o2);
        const double Hm = (u + o2 * A2 / q2) / (u + o2 / (A2 * q2));
        double b0, b1, b2;
        if (matchNum (a1, a2, 1.0, w0, sq (A2), wm, Hm, b0, b1, b2) && minPhase (b0, b1, b2)) { c.b0 = b0; c.b1 = b1; c.b2 = b2; c.a1 = a1; c.a2 = a2; return fin (c); }
    }
    return false;
}
inline bool lowShelfBoost (double w0, double A, double q, Bq& c)
{
    const double sA = std::sqrt (A), A2 = A * A, q2 = q * q;
    double a1, a2; polesZ (w0 / sA, q, a1, a2);
    auto H2 = [&] (double w) { const double o = w / w0, o2 = o * o, k = o2 * A / q2; return A2 * (sq (A - o2) + k) / (sq (1.0 - A * o2) + k); };
    const double wz = w0 * sA; double wa[5], wb[5];
    {   // the two nodes are a continuous function of (w0, A): three candidate pairs, blended by where the shelf's own resonance (wz) sits
        const auto sstep = [] (double x) { x = std::min (std::max (x, 0.0), 1.0); return x * x * (3.0 - 2.0 * x); };
        const double P1a = w0, P1b = wz;                                                   // corner and resonance: best while wz is well below Nyquist
        double P2b = std::min (std::max (0.8 * kPi, w0 + 0.06 * kPi), 0.99 * kPi), P2a = std::max (std::min (wz, 0.6 * kPi), w0 + 0.1 * (P2b - w0));
        double P3a = std::max (std::min (wz - 0.02 * kPi, 0.74 * kPi), w0 + 0.02 * kPi), P3b = std::min (P3a + 0.14 * kPi, 0.99 * kPi); P3b = std::max (P3b, P3a + 0.03 * kPi);
        const double w1 = 1.0 - sstep ((wz - 0.45 * kPi) / (0.1 * kPi)), w3 = std::max (sstep ((wz - 0.65 * kPi) / (0.1 * kPi)), sstep ((q - 2.5) / 2.5));   // a high Q (a sharp resonance) takes the upper nodes too
        const double Ma = (1.0 - w3) * P2a + w3 * P3a, Mb = (1.0 - w3) * P2b + w3 * P3b;
        wa[0] = w1 * P1a + (1.0 - w1) * Ma; wb[0] = w1 * P1b + (1.0 - w1) * Mb;
    }
    wa[1] = w0; wb[1] = w0 < 0.7 * kPi ? 0.85 * kPi : 0.985 * kPi;   // second try: the corner and the band near Nyquist
    wa[2] = w0; wb[2] = 0.92 * kPi;
    wa[3] = w0; wb[3] = 0.985 * kPi;
    wa[4] = w0; wb[4] = std::min (w0 + 0.12 * kPi, 0.99 * kPi);
    for (int t = 0; t < 5; ++t)
    {
        if (! (wb[t] > wa[t] + 0.01 * kPi)) continue;
        double b0, b1, b2;
        if (matchNum (a1, a2, sq (A2), wa[t], H2 (wa[t]), wb[t], H2 (wb[t]), b0, b1, b2) && minPhase (b0, b1, b2)) { c.b0 = b0; c.b1 = b1; c.b2 = b2; c.a1 = a1; c.a2 = a2; return fin (c); }
    }
    return false;
}
// high shelf = A^2 / low shelf (exact in the analog domain, kept exact here)
inline Bq highShelfFromLow (const Bq& l, double A)
{
    const double A2 = A * A; Bq o;
    o.b0 = A2 / l.b0; o.b1 = A2 * l.a1 / l.b0; o.b2 = A2 * l.a2 / l.b0; o.a1 = l.b1 / l.b0; o.a2 = l.b2 / l.b0; return o;
}
inline Bq highShelfCutFromLow (const Bq& l, double A)
{
    const double A2 = A * A; Bq o; o.b0 = l.b0 / A2; o.b1 = l.b1 / A2; o.b2 = l.b2 / A2; o.a1 = l.a1; o.a2 = l.a2; return o;
}
inline bool notchSection (double w0, double q, Bq& c)
{
    double a1, a2; polesZ (w0, q, a1, a2);
    const double cw = std::cos (w0), s2 = std::sin (0.5 * w0);
    const double k = (1.0 + a1 + a2) / (4.0 * s2 * s2);          // DC gain 1; zeros on the unit circle at w0
    c.b0 = k; c.b1 = -2.0 * k * cw; c.b2 = k; c.a1 = a1; c.a2 = a2; return fin (c);
}
// 2nd-order low pass (high cut stage): DC 1, |H(w0)| = Q, and the analog magnitude at a second node. Two node choices, blended on the numerator
// (same poles, so the blend stays a valid section and the design is continuous in w0): Nyquist side for corners below 0.7 pi, 0.55 w0 for corners in the top octave.
inline bool lowPassSection (double w0, double Q, Bq& c)
{
    double a1, a2; polesZ (w0, Q, a1, a2);
    const double x = std::min (std::max ((w0 - 0.66 * kPi) / (0.12 * kPi), 0.0), 1.0), t = x * x * (3.0 - 2.0 * x);
    double nb[3] {};
    for (int k = 0; k < 2; ++k)
    {
        const double wk = (k == 0) ? 0.9 * kPi : 0.55 * w0, wgt = (k == 0) ? 1.0 - t : t;
        if (wgt <= 0.0) continue;
        const double o = wk / w0, o2 = o * o, Hm = 1.0 / (sq (1.0 - o2) + o2 / (Q * Q));
        double b0, b1, b2;
        if (! matchNum (a1, a2, 1.0, w0, Q * Q, wk, Hm, b0, b1, b2, true)) return false;
        nb[0] += wgt * b0; nb[1] += wgt * b1; nb[2] += wgt * b2;
    }
    c.b0 = nb[0]; c.b1 = nb[1]; c.b2 = nb[2]; c.a1 = a1; c.a2 = a2; return fin (c);
}
// 2nd-order high pass (low cut stage), Vicanek: double zero at DC, |H(w0)| = Q
inline bool highPassSection (double w0, double Q, Bq& c)
{
    double a1, a2; polesZ (w0, Q, a1, a2);
    const double s2 = std::sin (0.5 * w0), c2 = std::cos (0.5 * w0), p1 = s2 * s2, p0 = c2 * c2, p2 = 4.0 * p0 * p1;
    const double A0 = sq (1.0 + a1 + a2), A1 = sq (1.0 - a1 + a2), A2 = -4.0 * a2;
    const double b0 = Q * std::sqrt (std::max (A0 * p0 + A1 * p1 + A2 * p2, 0.0)) / (4.0 * p1);
    c.b0 = b0; c.b1 = -2.0 * b0; c.b2 = b0; c.a1 = a1; c.a2 = a2; return fin (c) && b0 > 0.0;
}

// Butterworth stage Qs of an order-N cascade, k = 0 is the highest-Q stage (the one the band Q controls)
inline double butterStageQ (int N, int k) { return 1.0 / (2.0 * std::sin ((2.0 * k + 1.0) * kPi / (2.0 * N))); }

inline double sanitiseFreq (double freqHz, double sr) { return std::min (std::max (freqHz, 1.0), 0.45 * sr); }

// the stages of one band. Returns the number of stages. slopeDb: 12 / 24 / 36 / 48 / 72 (cuts only).
inline int designBand (int type, double freqHz, double gainDb, double q, int slopeDb, double sr, Bq* out)
{
    const double w0 = 2.0 * kPi * sanitiseFreq (freqHz, sr) / sr;
    q = std::min (std::max (q, 0.05), 40.0);
    if (type == tBell || type == tLowShelf || type == tHighShelf)
    {
        const double ag = std::abs (gainDb);
        if (ag < 1e-4) { out[0] = Bq(); return 1; }                                     // 0 dB: exactly the identity
        const double A = std::pow (10.0, ag / 40.0);
        Bq c; bool ok;
        if (type == tBell) { ok = bellBoost (w0, A, q, c); if (ok && gainDb < 0.0) c = invertBq (c); }
        else
        {
            ok = lowShelfBoost (w0, A, q, c);
            if (ok)
            {
                if (type == tLowShelf) { if (gainDb < 0.0) c = invertBq (c); }
                else c = gainDb < 0.0 ? highShelfCutFromLow (c, A) : highShelfFromLow (c, A);
            }
        }
        if (! ok) { const double Ad = std::pow (10.0, gainDb / 40.0); c = rbj (type, Ad, w0, q); }
        out[0] = c; return 1;
    }
    if (type == tNotch)
    {
        Bq c; if (! notchSection (w0, q, c)) c = rbj (tNotch, 1.0, w0, q);
        out[0] = c; return 1;
    }
    const int N = slopeDb >= 72 ? 12 : slopeDb >= 48 ? 8 : slopeDb >= 36 ? 6 : slopeDb >= 24 ? 4 : 2, ns = N / 2;
    for (int k = 0; k < ns; ++k)
    {
        double Qs = butterStageQ (N, k); if (k == 0) Qs *= q;
        Bq c; const bool ok = type == tLowCut ? highPassSection (w0, Qs, c) : lowPassSection (w0, Qs, c);
        out[k] = ok ? c : rbj (type, 1.0, w0, Qs);
    }
    return ns;
}
inline double stageMagDb (const Bq& c, double w)
{
    const double c1 = std::cos (w), s1 = std::sin (w), c2 = std::cos (2.0 * w), s2 = std::sin (2.0 * w);
    const double nr = c.b0 + c.b1 * c1 + c.b2 * c2, ni = -(c.b1 * s1 + c.b2 * s2), dr = 1.0 + c.a1 * c1 + c.a2 * c2, di = -(c.a1 * s1 + c.a2 * s2);
    return 10.0 * std::log10 (std::max ((nr * nr + ni * ni) / (dr * dr + di * di), 1e-24));
}
} // namespace fdesign

inline float erbRate (float hz)  { return 21.4f * std::log10 (1.0f + 0.00437f * hz); }
inline float erbToHz (float erb) { return (std::pow (10.0f, erb / 21.4f) - 1.0f) / 0.00437f; }

class Engine
{
public:
    void prepare (double sampleRate, int maxBlock)
    {
        sr = sampleRate;
        maxBlockSize = std::max (maxBlock, 1);
        blockMs = 1000.0f * (float) kBlock / (float) sr;
        fadeSamples = std::max (1, (int) std::lround (kBandSwitchFadeMs * 0.001 * sr));
        fadeEqLen = std::max (1, (int) std::lround (kBandFadeMs * 0.001 * sr));
        // fade shapes: smootherstep progress (no jump at the start, no kink at the end); equal-power = cos / sin of a quarter turn, linear = the progress itself
        auto prog = [] (double x) { return x * x * x * (x * (x * 6.0 - 15.0) + 10.0); };
        fadeCos.assign ((size_t) fadeEqLen, 0.0f); fadeSin.assign ((size_t) fadeEqLen, 1.0f); fadeLin.assign ((size_t) fadeSamples, 1.0f);
        for (int k = 0; k < fadeEqLen; ++k) { const double th = 0.5 * M_PI * prog ((double) (k + 1) / (double) fadeEqLen); fadeCos[(size_t) k] = (float) std::cos (th); fadeSin[(size_t) k] = (float) std::sin (th); }
        for (int k = 0; k < fadeSamples; ++k) fadeLin[(size_t) k] = (float) prog ((double) (k + 1) / (double) fadeSamples);
        histBuf.assign ((size_t) (1 + kFreeBands) * 2 * kHistN, 0.0f);
        { uint32_t n = 1024; while ((double) n < 0.05 * sr + 0.003 * sr + 16.0) n <<= 1; cumMask = n - 1; for (auto& r : cumRing) r.assign ((size_t) n, 0.0); }
        delete[] dryStore; dryStore = new float[(size_t) (2 * maxBlockSize)];

        const float e0 = erbRate (kLowHz), e1 = erbRate (kHighHz), step = (e1 - e0) / (float) (kBands - 1);
        for (int b = 0; b < kBands; ++b)
        {
            const float e = e0 + step * (float) b;
            fc[b] = erbToHz (e);
            bw[b] = erbToHz (e + 0.5f * step) - erbToHz (e - 0.5f * step);
            const float w0 = 2.0f * (float) M_PI * fc[b] / (float) sr;
            bellQ[b] = fc[b] / (kBellBwScale * bw[b]);
            cosw0[b] = std::cos (w0); sinw0[b] = std::sin (w0);
            bellAlpha[b] = sinw0[b] / (2.0f * bellQ[b]);
        }
        hE0 = e0; hStep = (e1 - e0) / (float) (kHBands - 1);
        for (int b = 0; b < kHBands; ++b)
        {
            const float e = hE0 + hStep * (float) b;
            hfc[b] = erbToHz (e);
            hbwHz[b] = erbToHz (e + 0.5f * hStep) - erbToHz (e - 0.5f * hStep);
        }
        analysisTiling = false;
        setAnalysisEdge (kAnalysisEdgeDb);
        // bell shapes (dB, normalised to the bell's own gain) at every band centre, for the solve
        for (int k = 0; k < kBands; ++k)
        {
            float c[5]; bellCoeffs (k, kShapeRefDb, c);
            for (int b = 0; b < kBands; ++b)
            {
                const float w = 2.0f * (float) M_PI * fc[b] / (float) sr;
                const float cw = std::cos (w), sw = std::sin (w), c2w = std::cos (2 * w), s2w = std::sin (2 * w);
                const float nr = c[0] + c[1] * cw + c[2] * c2w, ni = -(c[1] * sw + c[2] * s2w);
                const float dr = 1.0f + c[3] * cw + c[4] * c2w, di = -(c[3] * sw + c[4] * s2w);
                const float mag2 = (nr * nr + ni * ni) / (dr * dr + di * di);
                const float resp = -10.0f * std::log10 (std::max (mag2, 1e-30f));   // positive = cut
                shape[b][k] = std::max (resp / kShapeRefDb, 0.0f);
            }
            shape[k][k] = 1.0f;
        }
        for (int b = 0; b < kBands; ++b) for (int k = 0; k < kBands; ++k) shapeT[k][b] = shape[b][k];
        targetPrepare();
        setParams (Params());
        reset();
    }

    // analysis: RBJ band-pass (0 dB peak), two stages. Per stage |H|^2 = 1 / (1 + Q^2 (w/w0 - w0/w)^2); at the band
    // edge w/w0 - w0/w ~ bw/fc, and we want -edgeDb there (per stage; the cascade is twice that).
    void setAnalysisEdge (float edgeDb)
    {
        const float xq = std::sqrt (std::pow (10.0f, edgeDb / 10.0f) - 1.0f);
        for (int b = 0; b < kBands; ++b)
        {
            const float qa = xq * fc[b] / bw[b];
            const float w0 = 2.0f * (float) M_PI * fc[b] / (float) sr, al = std::sin (w0) / (2.0f * qa), a0 = 1.0f + al;
            ab0[b] = al / a0; aa1[b] = -2.0f * std::cos (w0) / a0; aa2[b] = (1.0f - al) / a0;
            const double xqd = std::sqrt (std::pow (10.0, (double) edgeDb / 10.0) - 1.0), qad = xqd * (double) fc[b] / (double) bw[b];
            const double w0d = 2.0 * M_PI * (double) fc[b] / sr, ald = std::sin (w0d) / (2.0 * qad), a0d = 1.0 + ald;
            db0[b] = ald / a0d; da1[b] = -2.0 * std::cos (w0d) / a0d; da2[b] = (1.0 - ald) / a0d;
        }
        // the signal law's leakage test: leakDb[b][j] = the most that band b can read, relative to band j's reading, from ONE component that lies in band j's
        // extent and more than kSigEdgeZoneErb outside band b's extent (where the bench's ground truth says the component is not in band b).
        // Exact responses of both stages, 64 positions per band.
        {
            auto bandDb = [this] (int b, double f) {
                const double w = 2.0 * M_PI * f / sr; const std::complex<double> z1 = std::polar (1.0, -w), z2 = std::polar (1.0, -2.0 * w);
                const std::complex<double> H = (double) ab0[b] * (1.0 - z2) / (1.0 + (double) aa1[b] * z1 + (double) aa2[b] * z2);
                return 20.0 * std::log10 (std::max (std::norm (H), 1e-30));   // |H|^2 per stage, two stages
            };
            const double e0 = erbRate (kLowHz), st = (erbRate (kHighHz) - e0) / (double) (kBands - 1);
            for (int b = 0; b < kBands; ++b)
                for (int j = 0; j < kBands; ++j)
                {
                    for (int which = 0; which < 2; ++which)   // 0: anywhere outside the edge zone (a lone tone may sit next to it), 1: from j's centre outward
                    {
                        const double dMin = which == 0 ? (double) kSigEdgeZoneErb : 0.5 * st;
                        double worst = -300.0;
                        if (j != b)
                            for (int q = 0; q <= 64; ++q)
                            {
                                const double e = e0 + st * ((double) j - 0.5 + (double) q / 64.0), f = erbToHz ((float) e);
                                if (f <= 1.0 || f >= 0.49 * sr) continue;
                                const double distOut = std::abs (e - (e0 + st * (double) b)) - 0.5 * st;   // ERB outside band b's extent
                                if (distOut < dMin) continue;
                                worst = std::max (worst, bandDb (b, f) - bandDb (j, f));
                            }
                        (which == 0 ? leakDb : leakDbC)[b][j] = (float) worst;
                        (which == 0 ? leakLin : leakLinC)[b][j] = worst > -299.0 ? std::pow (10.0, 0.1 * worst) : 0.0;
                    }
                }
            for (int b = 0; b + 1 < kBands; ++b)   // one component from centre b (q = 0) to centre b + 1 (q = kSigPairN - 1)
                for (int q = 0; q < kSigPairN; ++q)
                {
                    const double e = e0 + st * ((double) b + (double) q / (double) (kSigPairN - 1)), f = erbToHz ((float) e);
                    const double rb = bandDb (b, f), rn = bandDb (b + 1, f);
                    pairD[b][q] = (float) (rb - rn); pairRb[b][q] = (float) rb; pairRn[b][q] = (float) rn;
                }
        }
        for (int b = 0; b < kHBands; ++b)
        {
            const float qa = xq * hfc[b] / hbwHz[b];
            const float w0 = 2.0f * (float) M_PI * hfc[b] / (float) sr, al = std::sin (w0) / (2.0f * qa), a0 = 1.0f + al;
            hb0[b] = al / a0; ha1[b] = -2.0f * std::cos (w0) / a0; ha2[b] = (1.0f - al) / a0;
        }
    }

    void reset()
    {
        const bool wasRunning = blocks > 0;
        resetCollideAnalysis(); resetSelfAnalysis();
        std::memset (cutSm, 0, sizeof (cutSm)); std::memset (target, 0, sizeof (target));
        std::memset (gain, 0, sizeof (gain)); std::memset (gainStart, 0, sizeof (gainStart));
        std::memset (bst, 0, sizeof (bst));
        std::memset (active, 0, sizeof (active));
        for (int b = 0; b < kBands; ++b) { bellA[b] = 1.0f; bellDA[b] = 0.0f; }
        for (int s = 0; s < kHunters; ++s) hunt[s] = Slot();
        phase = 0; anyActive = false; blocks = 0;
        targetReset (wasRunning);
        sampTotal = 0;
        std::fill (histBuf.begin(), histBuf.end(), 0.0f);
        for (int i = 0; i < kFreeBands; ++i) { fb[i] = FBand(); ghost[i] = FGhost(); ghost[i].total = 0; bandHistEnd[i] = 0; bandHistCount[i] = 0; std::fill (cumRing[i].begin(), cumRing[i].end(), 0.0); }
        updateFreeActive();
        for (int i = 0; i < kFreeBands; ++i) if (par.band[i].on) engageBand (i, false);   // a cold start: at full gain from the very first sample
    }

    void setParams (const Params& p)
    {
        const bool modeChanged = p.mode != par.mode;
        const Params prev = par;
        par = p;
        if (par.tiling != analysisTiling)
        {
            analysisTiling = par.tiling;
            setAnalysisEdge (analysisTiling ? 1.5f : kAnalysisEdgeDb);   // states keep running; only the coefficients change
        }
        // free bands: ranges, smoothing coefficients; a band that changes type or slope crossfades from its old filter (TASK_P1n)
        cBandSm = 1.0f - std::exp (-blockMs / kBandGainSmoothMs);     // (TARGET's bells use these three)
        cBandShape = 1.0f - std::exp (-blockMs / kBandShapeMs);
        maxBandRise = kBandRiseDbPerMs * blockMs;
        {
            const double tickMs = 1000.0 * (double) kFTick / sr;
            aFShape = 1.0 - std::exp (-tickMs / (kBandFreqSmoothMs / 3.0));
            aFGain = 1.0 - std::exp (-tickMs / (kBandUserGainMs / 3.0));
            maxFRise = (float) (kBandRiseDbPerMs * tickMs);
            cFGainSm = (float) (1.0 - std::exp (-tickMs / kBandGainSmoothMs));
        }
        for (int i = 0; i < kFreeBands; ++i)
        {
            FreeBand& b = par.band[i];
            b.type = std::clamp (b.type, (int) kBell, (int) kHighCut);
            b.freqHz = std::clamp (b.freqHz, 20.0f, (float) freeBandMaxFreqHz (sr));
            b.gainDb = std::clamp (b.gainDb, -18.0f, 18.0f);
            b.q = std::clamp (b.q, 0.1f, 18.0f);
            b.slopeDb = kFreeSlopeDb[freeSlopeIndexFromDb (b.slopeDb)];
            b.thrDb = std::clamp (b.thrDb, -60.0f, 0.0f);
            b.rangeDb = std::clamp (b.rangeDb, -12.0f, 12.0f);
            b.attackMs = std::clamp (b.attackMs, 1.0f, 100.0f);
            b.releaseMs = std::clamp (b.releaseMs, 10.0f, 1000.0f);
            b.trigger = b.trigger == kTrigKey ? kTrigKey : kTrigSelf;
            const double tickMs = 1000.0 * (double) kFTick / sr;
            fcAtt[i] = (float) (1.0 - std::exp (-tickMs / b.attackMs));
            fcRel[i] = (float) (1.0 - std::exp (-tickMs / b.releaseMs));
            const FreeBand& pb = prev.band[i];
            if (! sameFreeBand (pb, b)) fb[i].settled = false;
            const bool changed = pb.type != b.type || (isCutType (b.type) && pb.slopeDb != b.slopeDb);
            if (changed && fb[i].engaged)
            {
                retireBand (i, false);
                if (b.on) engageBand (i, true); else fb[i] = FBand();
            }
            else if (b.on && ! fb[i].engaged) engageBand (i, false);
        }
        updateFreeActive();
        par.targetInst   = std::clamp (par.targetInst, 0, kTargetInsts - 1);
        par.targetPart   = std::clamp (par.targetPart, 0.0f, 100.0f);
        par.targetAmount = std::clamp (par.targetAmount, 0.0f, kTargetAmountMaxDb);
        par.targetSlope  = kSlopeDb[slopeIndex (par.targetSlope)];
        par.targetWidth  = std::clamp (par.targetWidth, 0.5f, 2.0f);
        par.targetSweet  = std::clamp (par.targetSweet, 20.0f, 400.0f);
        tgtRun = par.targetInst != kTgtOff || tgtAnyActive();
        const float rel = kReleaseMs[std::clamp (p.speed, 0, 2)];
        cAtt = 1.0f - std::exp (-blockMs / kAttackMs);
        cRel = 1.0f - std::exp (-blockMs / rel);
        cHEnv = 1.0f - std::exp (-blockMs / kHEnvMs);
        // stage-1 time constant = 0.5 / ERB(fc): a one-pole with the same mean delay as an average over 1 / ERB, an auditory filter's time resolution (Glasberg &
        // Moore 1990) and the window of the bench's ground truth. A tone at 40 Hz ripples +-0.5 dB. Bench: a 5 ms attack read a 15 ms burst 2-3 dB over its
        // 1 / ERB average; the asymmetric 1 / ERB release of the first version kept a kick's knock (40 ms decay) present 80-100 ms after it had gone.
        for (int b = 0; b < kBands; ++b) cRelF[b] = 1.0f - std::exp (-blockMs / std::max (blockMs, 500.0f / (24.7f * (4.37f * fc[b] / 1000.0f + 1.0f))));
        cHQ = 1.0f - std::exp (-blockMs / kHQSmoothMs);
        maxRise = kRiseDbPerMs * blockMs;
        fadeStep = blockMs / kFocusFadeMs;
        maxGlideOct = kHGlideOctPer10ms * blockMs / 10.0f;
        for (int b = 0; b < kBands; ++b) { inFocus[b] = fc[b] >= p.focusLowHz && fc[b] <= p.focusHighHz; if (blocks == 0) focusFade[b] = inFocus[b] ? 1.0f : 0.0f; }
        // the bank that stops being analysed is cleared, so stale energy cannot surface when the mode comes back;
        // its cuts / hunter gains are released by the law (raw target 0) and the bells fade before they are skipped
        if (modeChanged) { resetCollideAnalysis(); resetSelfAnalysis(); }
    }

    // main: numCh channels, processed in place. key: numCh channels (or nullptr = no key). n samples.
    void process (float* const* main, const float* const* key, int numCh, int n)
    {
        numCh = std::clamp (numCh, 1, 2);
        int pos = 0;
        while (pos < n)
        {
            const int m = std::min (std::min (kBlock - phase, n - pos), maxBlockSize);
            // MIX / DELTA reference: the signal as it came in, before the free bands and the bank
            const bool blend = (freeActive || anyActive || tgtRun || par.delta) && (par.delta || par.mix < 1.0f);   // DELTA is always dry - wet (0 when nothing is active)
            float* dry[2] { dryStore, dryStore + maxBlockSize };
            // SELF and TARGET detection read this copy too (SPEC 8h): the source, not what the user's own bands made of it
            const bool needIn = blend || par.mode == kSelf || listening;
            if (needIn) for (int ch = 0; ch < numCh; ++ch) std::memcpy (dry[ch], main[ch] + pos, sizeof (float) * (size_t) m);
            if (bakeOn)
            {
                const float* l0 = main[0] + pos; const float* r0 = numCh > 1 ? main[1] + pos : l0;
                double e = 0.0;
                for (int i = 0; i < m; ++i) { const float x = numCh > 1 ? 0.5f * (l0[i] + r0[i]) : l0[i]; e += (double) x * (double) x; }
                bakeMainSum += e;
            }
            { float* ip[2] { main[0] + pos, numCh > 1 ? main[1] + pos : nullptr }; pushHist (histOf (0), ip, numCh, m, sampTotal); }          // the free bands' pre-roll source: the engine input, before anything touched it
            if (freeActive) freeApply (main, key, numCh, pos, m);   // trigger levels are read from the untouched input (SELF) or the key inside
            if (par.mode == kSelf) analyseSelf (dry, numCh, 0, m);   // SELF hunts the source: the input, before the free bands (SPEC 8h)
            else                   analyse (main, key, numCh, pos, m);
            if (anyActive) apply (main, numCh, pos, m);
            if (listening) targetDetect (dry, numCh, 0, m);        // always listening (SPEC 8d), TARGET on or off: reads only, the input (SPEC 8h)
            if (tgtRun) targetApply (main, numCh, pos, m);          // TARGET: after the free bands and the bank
            if (blend)
                for (int ch = 0; ch < numCh; ++ch)
                {
                    float* y = main[ch] + pos; const float* x = dry[ch];
                    if (par.delta) for (int i = 0; i < m; ++i) y[i] = x[i] - y[i];
                    else           for (int i = 0; i < m; ++i) y[i] = x[i] + par.mix * (y[i] - x[i]);
                }
            pos += m; phase += m; sampTotal += (uint64_t) m;
            if (phase == kBlock) { law(); phase = 0; }
        }
    }

    // ---- COLLIDE accessors (32-band bank)
    int          numBands() const { return kBands; }
    const float* bandCentresHz() const { return fc; }
    const float* currentCutsDb() const { return target; }   // the target cut per band after smoothing, spread and FOCUS (positive = cut); all 0 in SELF
    const float* bellGainsDb() const { return gain; }       // the gains the 32 bells actually run with (positive = cut)
    const float* mainLevelsDb() const { return lvlM; }      // 32-band main levels (COLLIDE only; frozen in SELF)
    const float* keyLevelsDb() const { return lvlK; }
    const float* fastMainLevelsDb() const { return lvlMf; }   // the signal law's presence levels (stage 1: symmetric, 0.5 / ERB)
    const float* fastKeyLevelsDb() const { return lvlKf; }
    const float* sigPresenceKey() const { return sigDbgPK; }
    const float* sigPresenceMain() const { return sigDbgPM; }
    const float* sigNeedDb() const { return sigDbgNeed; }
    float        keyTotalDb() const { return lvlKT; }
    // ---- SELF accessors (peak hunter)
    int          numHunters() const { return kHunters; }
    float        hunterFreqHz (int s) const { return hunt[s].freq; }    // the slot's bell centre (kept while the slot releases)
    float        hunterGainDb (int s) const { return -hunt[s].cut; }    // bell gain, negative = cut, 0 = idle
    float        hunterQ (int s) const { return hunt[s].q; }
    bool         hunterActive (int s) const { return hunt[s].active; }
    int          numHunterBands() const { return kHBands; }
    const float* hunterBandCentresHz() const { return hfc; }
    const float* hunterLevelsDb() const { return lvlH; }     // 64-band main levels (SELF only)
    const float* hunterExcessDb() const { return excess; }   // M_b - env_b per 64-band (SELF only)
    // ---- free band accessors (signed dB, negative = cut, 0 = idle / off)
    int          numFreeBands() const { return kFreeBands; }
    float        freeBandGainDb (int i) const { return fb[i].engaged ? fb[i].gSm : 0.0f; }   // the gain the band is moving to this sub-block (the panel's ghost curve)
    float        freeBandFreqHz (int i) const { return fb[i].freq; }
    float        freeBandQ (int i) const { return fb[i].q; }
    float        freeBandLevelDb (int i) const { return toDb (fb[i].env) + kPeakFromRmsDb; }   // the trigger level the law sees
    bool         freeBandActive (int i) const { return fb[i].active; }
    const Params& params() const { return par; }
    // ---- TARGET accessors (cut in dB, positive = cut)
    int          numTargetBells() const { return kTargetBells; }
    float        targetBellCutDb (int i) const { return tbs[std::clamp (i, 0, kTargetBells - 1)].gSm; }            // the cut bell i is moving to this sub-block (0 = idle)
    float        targetBellFreqHz (int i) const { return tbs[std::clamp (i, 0, kTargetBells - 1)].freq; }
    float        targetCutDb() const { float m = std::max (edge[0].gSm, edge[1].gSm); for (auto& b : tbs) m = std::max (m, b.gSm); return m; }   // the deepest of the bells and the two edge cuts
    float        targetEventMs (int = 0) const { const int64_t o = ts[curSlot >= 0 ? curSlot : 0].onsetAbs; return o >= 0 ? (float) ((double) (tSamp - o) * 1000.0 / sr) : 1e6f; }   // ms since the last accepted onset (>= 1e5: none yet)
    const TargetLearned& targetLearned() const { return ts[curSlot >= 0 ? curSlot : 0].L; }   // the learned points of the active voice (the kick's while TARGET is OFF)
    const TargetLearned& targetLearnedSlot (int slot) const { return ts[std::clamp (slot, 0, kTSlots - 1)].L; }
    static int   slotOf (int voice, bool bus) { return bus && kVoice[voice].rejHi > 0.0f ? 5 + (voice == kVoSnare ? 1 : 0) : voice; }
    int          targetVoice() const { return curSlot >= 0 ? kSlotVoice[curSlot] : -1; }
    int          targetPartNow() const { return vPart; }                    // the part slider 2 points at (index in the instrument's list)
    int          targetOnsets() const { return ts[curSlot >= 0 ? curSlot : 0].nOnsets; }   // onsets that were not cancelled
    int          targetRelearns() const { return ts[curSlot >= 0 ? curSlot : 0].nRelearn; }   // times the memory was replaced because the sound changed (SPEC 8d 5, 7)
    int          targetHitsLearned() const { return ts[curSlot >= 0 ? curSlot : 0].nLearned; }   // hits that passed the analysis
    int          targetRejected (int why) const { return ts[curSlot >= 0 ? curSlot : 0].rejectWhy[std::clamp (why, 0, 3)]; }   // 0 next onset too soon, 1 late window too short, 2 no fundamental, 3 nothing found
    float        targetRevLowHz() const { return revOn && edge[0].run ? edge[0].freq : 0.0f; }   // REVERB-READY corners now (0 when idle)
    float        targetRevHighHz() const { return revOn && edge[1].run ? edge[1].freq : 0.0f; }
    float        targetRevMix() const { return revOn ? (float) edge[0].mix : 0.0f; }
    float        targetEdgeHz (int k) const { return edge[k & 1].run ? edge[k & 1].freq : 0.0f; }   // 0 = SUB-type (cut below), 1 = CLICK-type (cut above): the crossover now (0 = stage out of the path)
    float        targetEdgeRuleHz (int k) const { return plan[k & 1].on ? plan[k & 1].ruleF : 0.0f; }   // SPEC 8g: the crossover the placement started from (0 = no edge cut wanted)
    float        targetEdgePlacedHz (int k) const { return plan[k & 1].on ? plan[k & 1].f : 0.0f; }     // ... and where it was placed (the stage glides to it)
    float        targetProtectLossDb (int k) const { return plan[k & 1].on ? plan[k & 1].lossDb : 0.0f; }   // cut at the protected neighbour part's centre (SPEC 8g: <= 1 dB), 0 = nothing protected
    float        targetAchievedCutDb (int k) const { return plan[k & 1].on ? plan[k & 1].achDb : 0.0f; }    // cut reached at the target part's centre (REVERB-READY: one octave outside the body / top centre)
    bool         targetSlopeTooGentle() const { return gentle; }   // SPEC 8g: the cut at the target part's centre is under AMOUNT - 1 dB at this SLOPE
    float        targetEdgeCutDb (int k) const { return edge[k & 1].gSm; }
    float        targetEdgeMix (int k) const { return (float) edge[k & 1].mix; }
    // ---- SPEC 8h: what the other layers do at a free band's frequency right now (signed dB, <= 0 = cut). For a cut-type band the frequency is its corner.
    // The panel (P4) shows the breakdown; dyneqraw writes the columns. Each layer is the sum of its bells' exact RBJ responses at that frequency.
    float        bandFreqForLayersHz (int i) const { return fb[i].freq; }
    float        bandConflictDb (int i) const { return collideDbAt ((double) fb[i].freq); }   // COLLIDE: the 32-band bank (it judges the signal AFTER the free bands)
    float        bandSelfDb (int i) const     { return selfDbAt ((double) fb[i].freq); }      // SELF: the peak hunters (they judge the INPUT)
    float        bandTargetDb (int i) const   { return targetDbAt ((double) fb[i].freq); }    // TARGET: the bells and the two edge cuts
    // the same three layers at any frequency (signed dB, <= 0 = cut): the realised response the bells run with now (tools/bench reads them through dyneqraw --reallog)
    float        collideDbAt (double f) const
    {
        double db = 0.0;
        for (int b = 0; b < kBands; ++b) if (active[b] && gain[b] > 0.0f) db += bellDbAt (f, (double) fc[b], (double) bellQ[b], -(double) gain[b], sr);
        return (float) db;
    }
    float        selfDbAt (double f) const
    {
        double db = 0.0;
        for (int s = 0; s < kHunters; ++s) if (hunt[s].active && hunt[s].cut > 0.0f) db += bellDbAt (f, (double) hunt[s].freq, (double) hunt[s].q, -(double) hunt[s].cut, sr);
        return (float) db;
    }
    float        targetDbAt (double f) const
    {
        double db = 0.0;
        for (auto& b : tbs) if (b.active && b.gSm > 0.0f) db += bellDbAt (f, (double) b.freq, (double) b.q, -(double) b.gSm, sr);
        for (int k = 0; k < 2; ++k)
            if (edge[k].run && edge[k].gSm > 0.0f) db -= (double) edge[k].mix * edgeCutDbAt (f, (double) edge[k].freq, edge[k].slope, (double) edge[k].gSm, edge[k].below, sr);
        return (float) db;
    }
    // ---- P4 panel (read-only; no behaviour): what the editor needs to draw the layers' real responses itself.
    const float* bellQs() const { return bellQ; }                   // the 32 COLLIDE bells' Q (constant per band)
    bool         bellActive (int b) const { return active[std::clamp (b, 0, kBands - 1)]; }   // the bell runs (false = skipped, the band is untouched)
    bool         targetBellActive (int i = 0) const { const auto& b = tbs[std::clamp (i, 0, kTargetBells - 1)]; return b.active && b.gSm > 0.0f; }
    float        targetBellQ (int i = 0) const { return tbs[std::clamp (i, 0, kTargetBells - 1)].q; }
    int          targetEdgeSlopeIdx (int k) const { return edge[k & 1].slope; }
    bool         targetEdgeBelow (int k) const { return edge[k & 1].below; }
    bool         targetEdgeRunning (int k) const { return edge[k & 1].run; }
    // The highest FREQ a free band runs at: 0.45 x the sample rate (the panel shows this, not the requested value)
    static double freeBandMaxFreqHz (double sampleRate) { return std::min (20000.0, 0.45 * sampleRate); }
    // The magnitude (dB) of one free band's filter at f: the engine's own matched design (the same fdesign::designBand the audio path runs), every stage multiplied.
    static double freeBandMagDb (int type, double freqHz, double gainDb, double q, int slopeDb, double f, double sr)
    {
        fdesign::Bq st[fdesign::kMaxStages];
        const int n = fdesign::designBand (std::clamp (type, (int) kBell, (int) kHighCut), std::min (freqHz, freeBandMaxFreqHz (sr)), std::clamp (gainDb, -18.0, 18.0), std::clamp (q, 0.1, 18.0),
                                           kFreeSlopeDb[freeSlopeIndexFromDb (slopeDb)], sr, st);
        const double w = 2.0 * M_PI * std::min (f, 0.499 * sr) / sr; double db = 0.0;
        for (int k = 0; k < n; ++k) db += fdesign::stageMagDb (st[k], w);
        return db;
    }
    float        bandNetDb (int i) const { return freeBandGainDb (i) + bandConflictDb (i) + bandSelfDb (i) + bandTargetDb (i); }   // band gain + COLLIDE + SELF + TARGET at the band's frequency
    // SPEC 8d memory, saved with the project. Export / import are for the message thread's copy (the plug-in publishes after memoryVersion() changes).
    void         setEdgeAlwaysIn (bool on) { edgeForceIn = on; }   // measurement only: the split stays in the path at AMOUNT 0 (its all-pass sum is measured); the plug-in never sets it
    void         setListening (bool on) { listening = on; }   // measurement only: the cost of the always-on analysis
    uint32_t     memoryVersion() const { return memVersion; }
    bool         memoryEmpty() const { for (const TSlot& q : ts) if (q.nRec > 0) return false; return true; }
    static constexpr int memoryFloatsMax() { return 2 + kTSlots * (1 + kMemHits * (3 + 4 * kMaxParts)); }
    void memoryExport (std::vector<float>& out) const
    {
        out.clear(); out.push_back ((float) kMemFormat); out.push_back ((float) kTSlots);
        for (const TSlot& q : ts)
        {
            out.push_back ((float) q.nRec);
            for (int i = 0; i < q.nRec; ++i)
            {
                const HitRec& h = q.rec[i]; out.push_back (h.key); out.push_back (h.f0); out.push_back (h.tEnd);
                for (int p = 0; p < kMaxParts; ++p) { out.push_back (h.c[p]); out.push_back (h.lo[p]); out.push_back (h.hi[p]); out.push_back (h.ok[p] ? 1.0f : 0.0f); }
            }
        }
    }
    // false = the data is not a memory of this layout (nothing changed). A good memory is checked against the audio of the first hits (SPEC 8d 5).
    bool memoryImport (const std::vector<float>& in)
    {
        const size_t per = 3 + 4 * kMaxParts; size_t at = 0;
        auto get = [&] (float& v) { if (at >= in.size()) return false; v = in[at++]; return std::isfinite (v); };
        float f = 0.0f;
        if (! get (f) || (int) f != kMemFormat || ! get (f) || (int) f != kTSlots) return false;
        HitRec (&tmp)[kTSlots][kMemHits] = impTmp; int cnt[kTSlots] {};   // a member, so the audio thread allocates nothing
        for (int k = 0; k < kTSlots; ++k)
        {
            if (! get (f) || f < 0.0f || f > (float) kMemHits) return false;
            cnt[k] = (int) f;
            if (at + (size_t) cnt[k] * per > in.size()) return false;
            for (int i = 0; i < cnt[k]; ++i)
            {
                HitRec& h = tmp[k][i]; h = HitRec(); float ok = 0.0f;
                get (h.key); get (h.f0); get (h.tEnd);
                for (int p = 0; p < kMaxParts; ++p) { get (h.c[p]); get (h.lo[p]); get (h.hi[p]); get (ok); h.ok[p] = ok > 0.5f; }
                if (h.key < 0.0f || h.key > 30000.0f || h.f0 < 0.0f || h.tEnd < 0.0f || h.tEnd > 1000.0f) return false;
                for (int p = 0; p < kMaxParts; ++p) if (h.ok[p] && ! (h.lo[p] > 0.0f && h.lo[p] <= h.c[p] && h.c[p] <= h.hi[p] && h.hi[p] <= 30000.0f)) return false;
            }
        }
        for (int k = 0; k < kTSlots; ++k)
        {
            TSlot& q = ts[k]; q.nRec = cnt[k]; q.devCount = 0;
            for (int i = 0; i < cnt[k]; ++i) q.rec[i] = tmp[k][i];
            q.verifyLeft = q.nRec > 0 ? 3 : 0; q.verifyOk = q.verifyBad = 0;
            updateLearned (q);
        }
        ++memVersion; return true;
    }
    // ---- BAKE statistics (SPEC 1 C): the mean of the 32 COLLIDE target cuts over the blocks counted while collecting is on.
    // A block counts when the mono sum of the main input is at or above kBakeGateDb (RMS dBFS). Only COLLIDE counts: in SELF the
    // statistics stand still. Collected once per 32-sample law tick, so the numbers do not depend on the host block size.
    // Nothing here touches the audio path.
    void   bakeCollect (bool on) { bakeOn = on; }
    bool   bakeCollecting() const { return bakeOn; }
    void   bakeReset() { std::memset (bakeSum, 0, sizeof (bakeSum)); std::memset (bakeCutBlocks, 0, sizeof (bakeCutBlocks)); bakeCounted = 0; bakeSeen = 0; bakeMainSum = 0.0; }
    uint64_t bakeBlocksCounted() const { return bakeCounted; }   // blocks above the gate
    uint64_t bakeBlocksSeen() const { return bakeSeen; }         // every block while collecting (COLLIDE)
    float  bakeMeanCutDb (int b) const { return bakeCounted > 0 ? (float) (bakeSum[b] / (double) bakeCounted) : 0.0f; }
    float  bakePercentCut (int b) const { return bakeCounted > 0 ? 100.0f * (float) bakeCutBlocks[b] / (float) bakeCounted : 0.0f; }   // % of counted blocks with a cut above kBakeCutSeenDb
    ~Engine() { delete[] dryStore; }

private:
    // ======================================================================================================================================
    // ---- free bands (TASK_P1n). The six bands run in series, in double precision, before the 32-band bank.
    //  * design: matched second-order sections (fdesign above); the stages move per sample between two ticks (8 samples apart).
    //  * FREQ / Q / GAIN go through three cascaded poles (smooth3) per tick, and the coefficients are recomputed from the SMOOTHED parameters,
    //    so a host that sends one value per block gets a slope-continuous path (no staircase, no lerp of b / a).
    //  * a gain type that engages starts at 0 dB (the identity) and the 1 dB/ms limiter takes it to GAIN; a cut / notch that engages, and every type
    //    or slope change, crossfades equal-power over 20 ms from a ghost (dry, or the old filter) to the new filter, which was first run over the last
    //    4 ms of its own input. Switching OFF is unchanged (gain types ramp to 0, the others fade out over 5 ms).
    //  * the dynamic detector is the mean square of the trigger band-pass over a whole number of periods of FREQ (>= 2 ms): a steady sine has no ripple.
    struct FStage { fdesign::Bq c, cT, dc; bool ramp = false; double st[4] {}; };   // c: coefficients now; cT: at the next tick; dc: per-sample step between them
    struct FBand
    {
        bool   engaged = false, active = false, settled = false;
        double lf[3] {}, lq[3] {}, lg[3] {};                 // the three poles of the FREQ (log2) / Q (log2) / GAIN (dB) smoothers
        float  gLim = 0.0f, gSm = 0.0f;                      // dB: rate-limited gain, smoothed (what the filter runs with)
        float  freq = 1000.0f, q = 1.0f;                     // smoothed
        float  env = 0.0f;                                   // trigger power envelope
        double db0 = 0.0, da1 = 0.0, da2 = 0.0, d1 = 0.0, d2 = 0.0, cum = 0.0;   // trigger band-pass (RBJ, 0 dB peak), its state, the running sum of y^2
        uint32_t cpos = 0;                                   // samples written to this band's cumulative ring
        double dFreq = -1.0, dQ = -1.0, dGain = 1e9; int dType = -1, dSlope = -1;   // what the stages were designed for
        int    nStages = 1;
        FStage stg[fdesign::kMaxStages];
    };
    // the old filter of a band that was switched off / changed type (frozen, running next to the new path), or "dry" for a band that engages from the input
    struct FGhost { int left = 0, total = 1; bool dry = true, eq = true; int nStages = 0; FStage stg[fdesign::kMaxStages]; };
    FGhost ghost[kFreeBands];
    int fadeSamples = 240, fadeEqLen = 960;
    std::vector<float> fadeCos, fadeSin, fadeLin;
    std::vector<double> cumRing[kFreeBands];
    uint32_t cumMask = 0;
    std::vector<float> histBuf;                              // [engine input, band 1 .. 6 inputs] x 2 channels x kHistN (on the heap: a big member array shifts the hot arrays' alignment)
    float* histOf (int k) { return histBuf.data() + (size_t) k * 2 * kHistN; }   // k = 0: the engine input, 1 + i: band i's input
    uint64_t bandHistEnd[kFreeBands] {}; int bandHistCount[kFreeBands] {};
    uint64_t sampTotal = 0;                                  // samples processed since reset(); 0 = cold start (a band that engages then starts at full gain)
    double aFShape = 0.1, aFGain = 0.1; float maxFRise = 0.3333f, cFGainSm = 0.1f;

    static bool hasGainType (int t) { return t == kBell || t == kLowShelf || t == kHighShelf; }
    static bool isCutType (int t) { return t == kLowCut || t == kHighCut; }
    void updateFreeActive() { freeActive = false; for (int i = 0; i < kFreeBands; ++i) if (fb[i].engaged || ghost[i].left > 0) freeActive = true; }
    static bool sameFreeBand (const FreeBand& a, const FreeBand& b)
    {
        return a.on == b.on && a.type == b.type && a.freqHz == b.freqHz && a.gainDb == b.gainDb && a.q == b.q && a.slopeDb == b.slopeDb && a.dyn == b.dyn && a.thrDb == b.thrDb
            && a.rangeDb == b.rangeDb && a.attackMs == b.attackMs && a.releaseMs == b.releaseMs && a.trigger == b.trigger;
    }

    static double smooth3 (double* x, double target, double a)
    {
        x[0] += (target - x[0]) * a; x[1] += (x[0] - x[1]) * a; x[2] += (x[1] - x[2]) * a;
        if (std::abs (x[0] - target) < 1e-7 && std::abs (x[1] - target) < 1e-7 && std::abs (x[2] - target) < 1e-7) x[0] = x[1] = x[2] = target;
        return x[2];
    }

    // the band's input history: bandHist[i] while the free bands run, inHist (the engine input) otherwise
    void pushHist (float* h, float* const* io, int numCh, int len, uint64_t base)
    {
        const int i0 = (int) ((uint32_t) base & (kHistN - 1)), first = std::min (len, kHistN - i0);
        for (int ch = 0; ch < numCh; ++ch)
        {
            std::memcpy (h + (size_t) ch * kHistN + i0, io[ch], sizeof (float) * (size_t) first);
            if (first < len) std::memcpy (h + (size_t) ch * kHistN, io[ch] + first, sizeof (float) * (size_t) (len - first));
        }
    }
    void recordBandHist (int i, float* const* io, int numCh, int len, uint64_t base)
    {
        if (bandHistEnd[i] != base) bandHistCount[i] = 0;
        pushHist (histOf (1 + i), io, numCh, len, base);
        bandHistEnd[i] = base + (uint64_t) len; bandHistCount[i] = std::min (kHistN, bandHistCount[i] + len);
    }
    // run the band's new filter over the last kBandPrerollMs of its input (output discarded), so it does not start from the zero state
    void preroll (int i)
    {
        FBand& s = fb[i];
        const int want = std::max (1, (int) std::lround (kBandPrerollMs * 0.001 * sr));
        const float* H; int cnt;
        if (bandHistEnd[i] == sampTotal && bandHistCount[i] > 0) { H = histOf (1 + i); cnt = std::min (want, bandHistCount[i]); }
        else { H = histOf (0); cnt = (int) std::min<uint64_t> ((uint64_t) want, std::min<uint64_t> (sampTotal, (uint64_t) kHistN)); }
        for (int n = 0; n < cnt; ++n)
        {
            const uint32_t idx = (uint32_t) (sampTotal - (uint64_t) (cnt - n)) & (kHistN - 1);
            for (int ch = 0; ch < 2; ++ch)
            {
                double x = H[(size_t) ch * kHistN + idx];
                for (int k = 0; k < s.nStages; ++k)
                {
                    FStage& g = s.stg[k]; double& s1 = g.st[2 * ch]; double& s2 = g.st[2 * ch + 1];
                    const double y = g.c.b0 * x + s1;
                    s1 = g.c.b1 * x - g.c.a1 * y + s2; s2 = g.c.b2 * x - g.c.a2 * y; x = y;
                }
            }
        }
    }

    // the old filter goes to the ghost (true) when it was running; linear = the 5 ms fade of a switch-off, otherwise the 20 ms equal-power fade
    bool retireBand (int i, bool linear)
    {
        const FBand& s = fb[i]; if (! s.engaged || ! s.active) return false;
        FGhost& g = ghost[i]; g.dry = false; g.eq = ! linear; g.total = g.left = linear ? fadeSamples : fadeEqLen; g.nStages = s.nStages;
        for (int k = 0; k < s.nStages; ++k) { g.stg[k] = s.stg[k]; g.stg[k].dc = fdesign::Bq(); g.stg[k].ramp = false; }
        return true;
    }
    void dryGhost (int i) { FGhost& g = ghost[i]; g.dry = true; g.eq = true; g.total = g.left = fadeEqLen; g.nStages = 0; }

    // (re)design the stages from the smoothed parameters. jump: the coefficients are set at once (a band that engages); otherwise they move per sample to the new
    // design over the next tick, so the coefficient path is continuous (the end points are exact designs of smoothed parameters, a tick apart). false = nothing changed.
    bool designNow (int i, bool jump)
    {
        FBand& s = fb[i]; const FreeBand& p = par.band[i];
        const double gdb = hasGainType (p.type) ? (double) s.gSm : 0.0;
        if (! jump && (double) s.freq == s.dFreq && (double) s.q == s.dQ && gdb == s.dGain && p.type == s.dType && p.slopeDb == s.dSlope)
        {
            bool was = false;
            for (int k = 0; k < s.nStages; ++k) { was = was || s.stg[k].ramp; s.stg[k].c = s.stg[k].cT; s.stg[k].dc = fdesign::Bq(); s.stg[k].ramp = false; }
            return was;
        }
        fdesign::Bq tmp[fdesign::kMaxStages];
        const int n = fdesign::designBand (p.type, (double) s.freq, gdb, (double) s.q, p.slopeDb, sr, tmp);
        const bool sameShape = n == s.nStages && ! jump;
        s.nStages = n;
        for (int k = 0; k < n; ++k)
        {
            FStage& g = s.stg[k];
            if (sameShape)
            {
                g.c = g.cT; g.cT = tmp[k]; const double inv = 1.0 / (double) kFTick;
                g.dc.b0 = (g.cT.b0 - g.c.b0) * inv; g.dc.b1 = (g.cT.b1 - g.c.b1) * inv; g.dc.b2 = (g.cT.b2 - g.c.b2) * inv; g.dc.a1 = (g.cT.a1 - g.c.a1) * inv; g.dc.a2 = (g.cT.a2 - g.c.a2) * inv;
                g.ramp = true;
            }
            else { g.c = g.cT = tmp[k]; g.dc = fdesign::Bq(); g.ramp = false; }
        }
        s.dFreq = (double) s.freq; s.dQ = (double) s.q; s.dGain = gdb; s.dType = p.type; s.dSlope = p.slopeDb;
        return true;
    }
    void triggerCoeffs (int i)
    {
        FBand& s = fb[i];
        const double w0 = 2.0 * M_PI * (double) s.freq / sr, al = std::sin (w0) / (2.0 * (double) s.q), a0 = 1.0 + al;
        s.db0 = al / a0; s.da1 = -2.0 * std::cos (w0) / a0; s.da2 = (1.0 - al) / a0;
    }

    // a band switches on (typeChange false) or changes type / slope while on (true, the old filter is already in the ghost if it was running)
    void engageBand (int i, bool typeChange)
    {
        const FreeBand& p = par.band[i]; FBand& s = fb[i];
        const bool hasGain = hasGainType (p.type), cold = sampTotal == 0;
        s = FBand(); s.engaged = true; s.freq = p.freqHz; s.q = p.q;
        for (int k = 0; k < 3; ++k) { s.lf[k] = std::log2 ((double) p.freqHz); s.lq[k] = std::log2 ((double) p.q); s.lg[k] = hasGain ? (double) p.gainDb : 0.0; }
        s.gLim = s.gSm = hasGain ? ((cold || typeChange) ? p.gainDb : 0.0f) : 0.0f;
        s.active = hasGain ? s.gSm != 0.0f : true;
        if (p.dyn && hasGain) triggerCoeffs (i);
        if (s.active) designNow (i, true);
        if (! cold && s.active)
        {
            preroll (i);
            if (ghost[i].left == 0) dryGhost (i);
        }
        updateFreeActive();
    }

    // detector window: the mean of y^2 over a whole number of periods of FREQ (at least kBandDetMinWinMs)
    float detectorPower (int i)
    {
        FBand& s = fb[i];
        const double period = sr / std::max ((double) s.freq, 1.0), minW = kBandDetMinWinMs * 0.001 * sr;
        double W = period * std::max (1.0, std::ceil (minW / period));
        W = std::min (W, (double) cumMask - 4.0);
        const int i0 = (int) W; const double fr = W - (double) i0;
        const double* ring = cumRing[i].data();
        const uint32_t last = s.cpos - 1;
        const double c0 = s.cpos >= (uint32_t) (i0 + 1) ? ring[(last - (uint32_t) i0) & cumMask] : 0.0;
        const double c1 = s.cpos >= (uint32_t) (i0 + 2) ? ring[(last - (uint32_t) i0 - 1u) & cumMask] : 0.0;
        const double old = c0 + (c1 - c0) * fr;
        return (float) std::max ((s.cum - old) / W, 0.0);
    }

    // one tick (every kFTick samples, on the absolute grid): smoothers, gain law, limiter, coefficients
    void freeTick (int i)
    {
        FBand& s = fb[i]; if (! s.engaged) return;
        const FreeBand& p = par.band[i];
        const bool hasGain = hasGainType (p.type);
        if (! p.on && ! hasGain) { retireBand (i, true); s = FBand(); updateFreeActive(); return; }
        if (s.settled) return;
        const double lfT = std::log2 ((double) p.freqHz), lqT = std::log2 ((double) p.q), gT = (p.on && hasGain) ? (double) p.gainDb : 0.0;
        s.freq = (float) std::exp2 (smooth3 (s.lf, lfT, aFShape));
        s.q    = (float) std::exp2 (smooth3 (s.lq, lqT, aFShape));
        const double userG = smooth3 (s.lg, gT, aFGain);
        const bool dynOn = hasGain && p.on && p.dyn;
        float tgt = 0.0f;
        if (hasGain && p.on)
        {
            tgt = (float) userG;
            if (p.dyn)
            {
                const float pm = detectorPower (i);
                s.env += (pm - s.env) * (pm > s.env ? fcAtt[i] : fcRel[i]);
                const float L = toDb (s.env) + kPeakFromRmsDb;
                tgt += p.rangeDb * std::clamp ((L - p.thrDb) / kBandKneeDb, 0.0f, 1.0f);
            }
        }
        {   // at most 1 dB/ms, then a short one-pole so the limiter's ramp has no kinks (still never faster than the limit)
            const float d = tgt - s.gLim;
            s.gLim = std::abs (d) <= maxFRise ? tgt : s.gLim + (d > 0.0f ? maxFRise : -maxFRise);
            s.gSm += (s.gLim - s.gSm) * cFGainSm;
            if (std::abs (s.gLim - s.gSm) < 1e-4f) s.gSm = s.gLim;
        }
        if (hasGain && ! p.on && s.gSm == 0.0f && s.gLim == 0.0f) { s = FBand(); updateFreeActive(); return; }   // faded out
        if (dynOn) triggerCoeffs (i);
        const bool act = hasGain ? s.gSm != 0.0f : true;
        if (act && ! s.active)   // a gain type leaves 0 dB: from the identity, from the zero state
        {
            for (int k = 0; k < fdesign::kMaxStages; ++k) { std::memset (s.stg[k].st, 0, sizeof (s.stg[k].st)); s.stg[k].c = s.stg[k].cT = fdesign::Bq(); s.stg[k].dc = fdesign::Bq(); s.stg[k].ramp = false; }
            s.nStages = 1; s.dFreq = -1.0;
        }
        s.active = act;
        bool moved = false;
        if (act) moved = designNow (i, false);
        s.settled = ! moved && ! dynOn && s.lf[2] == lfT && s.lq[2] == lqT && s.lg[2] == gT && s.gLim == s.gSm && s.gLim == tgt;
    }

    // trigger levels: band-pass at FREQ / Q on the mono sum of the band's own input (before any free band touched it) or of the key
    void freeDetectChunk (int i, float* const* main, const float* const* key, int numCh, int off, int len)
    {
        const FreeBand& p = par.band[i]; FBand& s = fb[i];
        if (! (s.engaged && p.on && p.dyn && hasGainType (p.type))) return;
        const float *x0, *x1;
        if (p.trigger == kTrigKey)
        {
            if (key == nullptr || key[0] == nullptr) return;
            x0 = key[0] + off; x1 = numCh > 1 && key[1] != nullptr ? key[1] + off : x0;
        }
        else { x0 = main[0] + off; x1 = numCh > 1 ? main[1] + off : x0; }
        double d1 = s.d1, d2 = s.d2, cum = s.cum; const double b0 = s.db0, a1 = s.da1, a2 = s.da2;
        uint32_t cp = s.cpos; double* ring = cumRing[i].data();
        for (int j = 0; j < len; ++j)
        {
            const double x = numCh > 1 ? (double) (0.5f * (x0[j] + x1[j])) : (double) x0[j];
            const double y = b0 * x + d1;
            d1 = -a1 * y + d2; d2 = -b0 * x - a2 * y;
            cum += y * y; ring[cp & cumMask] = cum; ++cp;
        }
        if (cum > 1e7) { const double k = cum; for (double& v : cumRing[i]) v -= k; cum = 0.0; }
        s.d1 = d1; s.d2 = d2; s.cum = cum; s.cpos = cp;
    }

    static void runStagesD (FStage* stg, int n, double* l, double* r, int m)
    {
        for (int k = 0; k < n; ++k)
        {
            FStage& g = stg[k]; double b0 = g.c.b0, b1 = g.c.b1, b2 = g.c.b2, a1 = g.c.a1, a2 = g.c.a2;
            const double d0 = g.dc.b0, d1 = g.dc.b1, d2 = g.dc.b2, e1 = g.dc.a1, e2 = g.dc.a2;
            double s1 = g.st[0], s2 = g.st[1];
            if (g.ramp)
            {
                double c0 = b0, c1 = b1, c2 = b2, f1 = a1, f2 = a2;
                for (int i = 0; i < m; ++i)
                {
                    c0 += d0; c1 += d1; c2 += d2; f1 += e1; f2 += e2;
                    const double x = l[i], y = c0 * x + s1; s1 = c1 * x - f1 * y + s2; s2 = c2 * x - f2 * y; l[i] = y;
                }
                g.st[0] = s1; g.st[1] = s2;
                if (r != nullptr)
                {
                    s1 = g.st[2]; s2 = g.st[3]; c0 = b0; c1 = b1; c2 = b2; f1 = a1; f2 = a2;
                    for (int i = 0; i < m; ++i)
                    {
                        c0 += d0; c1 += d1; c2 += d2; f1 += e1; f2 += e2;
                        const double x = r[i], y = c0 * x + s1; s1 = c1 * x - f1 * y + s2; s2 = c2 * x - f2 * y; r[i] = y;
                    }
                    g.st[2] = s1; g.st[3] = s2;
                }
                g.c.b0 += d0 * m; g.c.b1 += d1 * m; g.c.b2 += d2 * m; g.c.a1 += e1 * m; g.c.a2 += e2 * m;
                continue;
            }
            for (int i = 0; i < m; ++i) { const double x = l[i], y = b0 * x + s1; s1 = b1 * x - a1 * y + s2; s2 = b2 * x - a2 * y; l[i] = y; }
            g.st[0] = s1; g.st[1] = s2;
            if (r != nullptr)
            {
                s1 = g.st[2]; s2 = g.st[3];
                for (int i = 0; i < m; ++i) { const double x = r[i], y = b0 * x + s1; s1 = b1 * x - a1 * y + s2; s2 = b2 * x - a2 * y; r[i] = y; }
                g.st[2] = s1; g.st[3] = s2;
            }
        }
    }

    void freeApply (float* const* main, const float* const* key, int numCh, int pos, int m)
    {
        int done = 0;
        while (done < m)
        {
            const int ph = (phase + done) & (kFTick - 1);
            int len = std::min (kFTick - ph, m - done);
            bool anyGhost = false, allSettled = true;
            for (int i = 0; i < kFreeBands; ++i) { if (ghost[i].left > 0) anyGhost = true; if (fb[i].engaged && ! fb[i].settled) allSettled = false; }
            if (allSettled && ! anyGhost) len = m - done;      // nothing moves: one pass
            else if (ph == 0) for (int i = 0; i < kFreeBands; ++i) freeTick (i);
            const int off = pos + done;
            float* io[2] { main[0] + off, numCh > 1 ? main[1] + off : nullptr };
            const uint64_t base = sampTotal + (uint64_t) done;
            for (int i = 0; i < kFreeBands; ++i) freeDetectChunk (i, main, key, numCh, off, len);
            double xl[kBlock], xr[kBlock], ol[kBlock], orr[kBlock];
            for (int i = 0; i < kFreeBands; ++i)
            {
                FBand& s = fb[i]; FGhost& g = ghost[i];
                recordBandHist (i, io, numCh, len, base);
                const bool fading = g.left > 0;
                if (! s.active && ! fading) continue;
                for (int j = 0; j < len; ++j) xl[j] = io[0][j];
                if (numCh > 1) for (int j = 0; j < len; ++j) xr[j] = io[1][j];
                double* r = numCh > 1 ? xr : nullptr;
                if (fading) { std::memcpy (ol, xl, sizeof (double) * (size_t) len); if (r != nullptr) std::memcpy (orr, xr, sizeof (double) * (size_t) len); }
                if (s.active) runStagesD (s.stg, s.nStages, xl, r, len);
                if (fading)
                {
                    if (! g.dry) runStagesD (g.stg, g.nStages, ol, r != nullptr ? orr : nullptr, len);
                    for (int j = 0; j < len; ++j)
                    {
                        const int k = g.total - g.left + j;
                        double wo, wn;
                        if (k >= g.total) { wo = 0.0; wn = 1.0; }
                        else if (g.eq) { wo = (double) fadeCos[(size_t) k]; wn = (double) fadeSin[(size_t) k]; }
                        else { wn = (double) fadeLin[(size_t) k]; wo = 1.0 - wn; }
                        xl[j] = wo * ol[j] + wn * xl[j];
                        if (r != nullptr) xr[j] = wo * orr[j] + wn * xr[j];
                    }
                    g.left = std::max (0, g.left - len);
                }
                for (int j = 0; j < len; ++j) io[0][j] = (float) xl[j];
                if (numCh > 1) for (int j = 0; j < len; ++j) io[1][j] = (float) xr[j];
            }
            for (int i = 0; i < kFreeBands; ++i) if (ghost[i].left == 0 && ghost[i].total > 0) { ghost[i].total = 0; updateFreeActive(); }
            done += len;
        }
    }

    struct Slot
    {
        float freq = 1000.0f, targetFreq = 1000.0f, q = 4.0f, targetQ = 4.0f;
        float raw = 0.0f, cut = 0.0f, cutStart = 0.0f;       // dB, positive = cut
        float cosw0 = 0.0f, dCosw0 = 0.0f, alpha = 0.1f, dAlpha = 0.0f, A = 1.0f, dA = 0.0f;   // cos w0 and alpha ramp per sample too (the centre glides)
        float st[4] {};
        bool  active = false, matched = false;
    };

    void resetCollideAnalysis()
    {
        std::memset (ms1, 0, sizeof (ms1)); std::memset (ms2, 0, sizeof (ms2)); std::memset (ms3, 0, sizeof (ms3)); std::memset (ms4, 0, sizeof (ms4));
        std::memset (ks1, 0, sizeof (ks1)); std::memset (ks2, 0, sizeof (ks2)); std::memset (ks3, 0, sizeof (ks3)); std::memset (ks4, 0, sizeof (ks4));
        std::memset (dms, 0, sizeof (dms)); std::memset (dks, 0, sizeof (dks));
        std::memset (sumM, 0, sizeof (sumM)); std::memset (sumK, 0, sizeof (sumK)); sumKT = 0.0;
        std::memset (envM, 0, sizeof (envM)); std::memset (envK, 0, sizeof (envK)); envKT = 0.0;
        std::memset (envMf, 0, sizeof (envMf)); std::memset (envKf, 0, sizeof (envKf)); std::memset (envMn, 0, sizeof (envMn)); std::memset (envKn, 0, sizeof (envKn));
        for (int b = 0; b < kBands; ++b) { lvlMf[b] = kDbFloor; lvlKf[b] = kDbFloor; lvlMn[b] = kDbFloor; lvlKn[b] = kDbFloor; sigKRefB[b] = kDbFloor; }
        for (int b = 0; b < kBands; ++b) { lvlM[b] = kDbFloor; lvlK[b] = kDbFloor; }
        lvlKT = kDbFloor;
    }

    void resetSelfAnalysis()
    {
        std::memset (hs1, 0, sizeof (hs1)); std::memset (hs2, 0, sizeof (hs2)); std::memset (hs3, 0, sizeof (hs3)); std::memset (hs4, 0, sizeof (hs4));
        std::memset (sumH, 0, sizeof (sumH)); std::memset (envH, 0, sizeof (envH)); std::memset (excess, 0, sizeof (excess));
        for (int b = 0; b < kHBands; ++b) { lvlH[b] = kDbFloor; hEnv[b] = kDbFloor; }
    }

    // ---- COLLIDE analysis: two cascaded band-pass biquads per band on the mono sum, mean square per sub-block
    void analyse (float* const* main, const float* const* key, int numCh, int pos, int m)
    {
        // Both laws read the double-precision analysis (8 Nov 2026, ADALET 1.0.0): with the float states the 0.9.1 law never became bit-identical to a
        // fresh instance after an automation fuzz (plugin_check 13.3b: 2.4-4.5e-5 abs up to 48 s, the low bands' float biquads never reconverge; see analyseD).
        // The float path below is kept for reference only.
        if (true) { analyseD (main, key, numCh, pos, m); return; }
        const float* l = main[0] + pos; const float* r = numCh > 1 ? main[1] + pos : l;
        for (int i = 0; i < m; ++i)
        {
            const float x = numCh > 1 ? 0.5f * (l[i] + r[i]) : l[i];
            for (int b = 0; b < kBands; ++b)
            {
                const float y1 = ab0[b] * x + ms1[b];
                ms1[b] = -aa1[b] * y1 + ms2[b];
                ms2[b] = -ab0[b] * x - aa2[b] * y1;
                const float y2 = ab0[b] * y1 + ms3[b];
                ms3[b] = -aa1[b] * y2 + ms4[b];
                ms4[b] = -ab0[b] * y1 - aa2[b] * y2;
                sumM[b] += y2 * y2;
            }
        }
        if (key != nullptr && key[0] != nullptr)
        {
            const float* kl = key[0] + pos; const float* kr = numCh > 1 && key[1] != nullptr ? key[1] + pos : kl;
            for (int i = 0; i < m; ++i)
            {
                const float x = numCh > 1 ? 0.5f * (kl[i] + kr[i]) : kl[i];
                sumKT += (double) (x * x);
                for (int b = 0; b < kBands; ++b)
                {
                    const float y1 = ab0[b] * x + ks1[b];
                    ks1[b] = -aa1[b] * y1 + ks2[b];
                    ks2[b] = -ab0[b] * x - aa2[b] * y1;
                    const float y2 = ab0[b] * y1 + ks3[b];
                    ks3[b] = -aa1[b] * y2 + ks4[b];
                    ks4[b] = -ab0[b] * y1 - aa2[b] * y2;
                    sumK[b] += y2 * y2;
                }
            }
        }
    }

    // The signal law's analysis: the same band-passes, the low bands with double states and coefficients. In float, two instances whose states differ (a host that
    // played other audio, an automation fuzz) never become identical again in some low bands (40 / 171 / 292 Hz on the demo beat: a standalone float biquad pair with
    // the same input stayed 1e-7 apart for 24 s; double converged; 923 Hz converged in float), and the signal law cuts there, so plugin_check 13.3b found the outputs
    // 1e-5 apart 22 s later.
    void analyseD (float* const* main, const float* const* key, int numCh, int pos, int m)
    {
        for (int sig = 0; sig < 2; ++sig)
        {
            const float* const* in = sig == 0 ? main : key;
            if (sig == 1 && (key == nullptr || key[0] == nullptr)) break;
            const float* l = in[0] + pos; const float* r = numCh > 1 && in[1] != nullptr ? in[1] + pos : l;
            double (*ds)[kBands] = sig == 0 ? dms : dks; float* sum = sig == 0 ? sumM : sumK;
            float* f1 = sig == 0 ? ms1 : ks1; float* f2 = sig == 0 ? ms2 : ks2; float* f3 = sig == 0 ? ms3 : ks3; float* f4 = sig == 0 ? ms4 : ks4;
            for (int i = 0; i < m; ++i)
            {
                const float xf = numCh > 1 ? 0.5f * (l[i] + r[i]) : l[i];
                if (sig == 1) sumKT += (double) (xf * xf);
                const double x = (double) xf;
                for (int b = 0; b < kSigDoubleBands; ++b)   // double where float states never reconverge (the low bands)
                {
                    const double y1 = db0[b] * x + ds[0][b];
                    ds[0][b] = -da1[b] * y1 + ds[1][b];
                    ds[1][b] = -db0[b] * x - da2[b] * y1;
                    const double y2 = db0[b] * y1 + ds[2][b];
                    ds[2][b] = -da1[b] * y2 + ds[3][b];
                    ds[3][b] = -db0[b] * y1 - da2[b] * y2;
                    sum[b] += (float) (y2 * y2);
                }
                for (int b = kSigDoubleBands; b < kBands; ++b)   // float above (they did reconverge, bit for bit)
                {
                    const float y1 = ab0[b] * xf + f1[b];
                    f1[b] = -aa1[b] * y1 + f2[b];
                    f2[b] = -ab0[b] * xf - aa2[b] * y1;
                    const float y2 = ab0[b] * y1 + f3[b];
                    f3[b] = -aa1[b] * y2 + f4[b];
                    f4[b] = -ab0[b] * y1 - aa2[b] * y2;
                    sum[b] += y2 * y2;
                }
            }
        }
    }

    // ---- SELF analysis: 64 half-ERB bands on the mono main, same filter design
    void analyseSelf (float* const* main, int numCh, int pos, int m)
    {
        const float* l = main[0] + pos; const float* r = numCh > 1 ? main[1] + pos : l;
        for (int i = 0; i < m; ++i)
        {
            const float x = numCh > 1 ? 0.5f * (l[i] + r[i]) : l[i];
            for (int b = 0; b < kHBands; ++b)
            {
                const float y1 = hb0[b] * x + hs1[b];
                hs1[b] = -ha1[b] * y1 + hs2[b];
                hs2[b] = -hb0[b] * x - ha2[b] * y1;
                const float y2 = hb0[b] * y1 + hs3[b];
                hs3[b] = -ha1[b] * y2 + hs4[b];
                hs4[b] = -hb0[b] * y1 - ha2[b] * y2;
                sumH[b] += y2 * y2;
            }
        }
    }

    static float toDb (float power) { return power > 1e-20f ? 10.0f * std::log10 (power) : kDbFloor; }
    // the signal law's dB: log2 from the float's exponent and a degree-7 polynomial on its mantissa (exact at both ends, so continuous; max error 1.5e-6 dB,
    // fitted 2026-10-26). The law takes ~190 logs per 32-sample tick; with log10 it ran 49x real time against 60x for 0.9.1 (A9 wants 60x).
    static float fastDb (double power)
    {
        if (! (power > 1e-20)) return kDbFloor;
        const float x = (float) power; uint32_t u; std::memcpy (&u, &x, 4);
        const int e = (int) ((u >> 23) & 255u) - 127; u = (u & 0x7FFFFFu) | 0x3F800000u; float m; std::memcpy (&m, &u, 4);
        const float t = m - 1.0f;
        const float p = t * (1.4426565128f + t * (-0.7204034418f + t * (0.4725160415f + t * (-0.3230872370f + t * (0.1903444919f + t * (-0.0767866789f + t * 0.0147603116f))))));
        return 3.0102999566f * ((float) e + p);
    }
    static float smoothstep (float t) { t = std::clamp (t, 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }
    float hIndexToHz (float idx) const { return erbToHz (hE0 + hStep * idx); }

    // ---- the signal law (kLawSignal): cut the main only where BOTH signals have their own energy, only while they have it (tools/bench/README.md)
    // A lone component between two band centres reads up to 12 dB low in both (the cascade is -12 dB at the edges; bench: a 260 Hz vocal fundamental on the
    // 227 / 292 Hz edge read -35 dBFS against its true -26). Where a pair (b, b+1) stands kSigLoneDb above both outer neighbours (one component: about 18 dB,
    // flat noise: 0 dB), the reading difference places the component (one component's table) and its level is put back into the band that holds it, or into
    // both inside the edge zone. Everything else keeps its reading.
    // Every decision below is a smooth weight, never a switch: two instances whose analysis differs by float rounding then cut by amounts that differ by
    // rounding too (plugin_check 13.3b: with switches - a table index, "lone or not", the loudest band - a 1-ulp difference flipped a decision and the outputs of a
    // fuzzed and a fresh instance still differed by 1e-5 22 s later).
    // ownW / leakW (optional): how much band b is known to hold a lone component itself / to read only its leakage (0 .. 1 each)
    void sigRefine (const float* lvl, float* out, float* ownW = nullptr, float* leakW = nullptr) const
    {
        for (int b = 0; b < kBands; ++b) out[b] = lvl[b];
        if (ownW != nullptr) for (int b = 0; b < kBands; ++b) { ownW[b] = 0.0f; leakW[b] = 0.0f; }
        const float stp = erbRate (fc[1]) - erbRate (fc[0]);
        const float qEdge = 0.5f * (float) (kSigPairN - 1), qZone = kSigEdgeZoneErb / stp * (float) (kSigPairN - 1);
        for (int b = 0; b + 1 < kBands; ++b)
        {
            const float hi = std::max (lvl[b], lvl[b + 1]);
            const float lo1 = b > 0 ? lvl[b - 1] : kDbFloor, lo2 = b + 2 < kBands ? lvl[b + 2] : kDbFloor;
            // one lone component: the pair stands kSigLoneDb above both outer neighbours (a 6 dB wide knee around it) and above the floor
            if (hi - std::max (lo1, lo2) <= kSigLoneDb - 3.0f || hi <= kKeyFloorDb) continue;   // (both weights below are 0 there)
            const float w = smoothstep ((hi - std::max (lo1, lo2) - (kSigLoneDb - 3.0f)) / 6.0f) * smoothstep ((hi - kKeyFloorDb) / 3.0f);
            const float d = lvl[b] - lvl[b + 1];
            const float* D = pairD[b];
            int lo = 0, hiq = kSigPairN - 2;   // D falls from + (at centre b) to - (at centre b + 1): the last q with D[q + 1] > d, by bisection
            if (D[1] <= d) hiq = 0;
            else while (lo < hiq) { const int mid = (lo + hiq + 1) / 2; if (D[mid] > d) lo = mid; else hiq = mid - 1; }
            const int q = std::min (lo, kSigPairN - 2);
            const float fr = std::clamp ((D[q] - d) / std::max (D[q] - D[q + 1], 1e-6f), 0.0f, 1.0f), t = (float) q + fr;
            const float P = 0.5f * ((lvl[b] - (pairRb[b][q] + fr * (pairRb[b][q + 1] - pairRb[b][q]))) + (lvl[b + 1] - (pairRn[b][q] + fr * (pairRn[b][q + 1] - pairRn[b][q]))));
            const float ob = 1.0f - smoothstep (t - (qEdge + qZone)), on = smoothstep (t - (qEdge - qZone - 1.0f));   // band b holds it up to the edge zone's far side, b + 1 from its near side
            out[b]     = std::max (out[b],     lvl[b]     + w * ob * std::clamp (P - lvl[b],     0.0f, 12.0f));
            out[b + 1] = std::max (out[b + 1], lvl[b + 1] + w * on * std::clamp (P - lvl[b + 1], 0.0f, 12.0f));
            if (ownW != nullptr)
            {
                ownW[b] = std::max (ownW[b], w * ob); leakW[b] = std::max (leakW[b], w * (1.0f - ob));
                ownW[b + 1] = std::max (ownW[b + 1], w * on); leakW[b + 1] = std::max (leakW[b + 1], w * (1.0f - on));
            }
        }
        if (ownW != nullptr) for (int b = 0; b < kBands; ++b) { const float sum = ownW[b] + leakW[b]; if (sum > 1.0f) { ownW[b] /= sum; leakW[b] /= sum; } }
    }
    // presence of band b's OWN energy in one signal: lvl = the fast readings (the leakage test), lvlR = the refined ones (against the reference): 0 .. 1
    // lone[j][0]: band j holds one lone component as seen from below (the band beyond it, j + 1, reads kSigLoneDb lower), lone[j][1]: from above (j - 1)
    void sigLone (const float* lvl, float (*lone)[2]) const
    {
        for (int j = 0; j < kBands; ++j)
        {
            lone[j][0] = j + 1 >= kBands ? 1.0f : smoothstep ((lvl[j] - lvl[j + 1] - (kSigLoneLeakDb - 3.0f)) / 6.0f);
            lone[j][1] = j - 1 < 0 ? 1.0f : smoothstep ((lvl[j] - lvl[j - 1] - (kSigLoneLeakDb - 3.0f)) / 6.0f);
        }
    }
    float sigPresence (const float* lvl, const double* env, const float* lvlR, int b, float ref, float ownW, float leakW, const float (*lone)[2]) const
    {
        const float time = smoothstep ((lvlR[b] - (ref - kSigNDb)) / kSigKneeDb) * smoothstep ((lvl[b] - kKeyFloorDb) / 3.0f);
        if (time <= 0.0f) return 0.0f;
        // a lone component between two centres (sigRefine) is this band's own energy (ownW) or only its leakage (leakW), whatever the general test below would
        // guess (bench: a 1 kHz tone on the 923 / 1086 Hz edge got 0.5 x 0.5 of its cut from that guess)
        float own = 0.0f;
        const float general = 1.0f - ownW - leakW;
        if (general > 0.0f)
        {
            // what the other bands would leak into b if all their energy were one component each (power sum, kSigLeakSpan bands each way). A neighbour that holds
            // one lone component (it stands kSigLoneDb above the band beyond it, or there is none) may hold it right at the edge zone: the worst case (leakDb);
            // one whose energy is spread is taken at its centre or beyond (leakDbC); a 6 dB knee between the two.
            // Bench (law FALSE s / MISSED %, an intermediate law and ground truth of 26 Oct): worst case everywhere 8 / 44, centre everywhere 55 / 12, this rule 19 / 16.
            double predP = 0.0;   // in power: the neighbours' powers times the leak ratios (the knee mixes the two ratios)
            for (int j = std::max (0, b - kSigLeakSpan); j <= std::min (kBands - 1, b + kSigLeakSpan); ++j)
            {
                if (j == b || lvl[j] <= kDbFloor + 1.0f) continue;
                const double ln = (double) lone[j][j > b ? 0 : 1];
                predP += env[j] * (leakLinC[b][j] + ln * (leakLin[b][j] - leakLinC[b][j]));
            }
            const float margin = predP > 0.0 ? fastDb (env[b]) - fastDb (predP) : 100.0f;   // dB the band reads over the predicted leakage
            own = general * smoothstep (margin / kSigOwnKneeDb);
        }
        return (own + ownW) * time;
    }
    void signalLaw (float* raw)
    {
        const float dec = kSigRefDecayDbPerS * blockMs * 0.001f;
        float kLow = kDbFloor, kHigh = kDbFloor;   // the key's loudest band at or under kSigLowTopHz, and above it (the low-end rule)
        for (int b = 0; b < kBands; ++b) { if (fc[b] <= kSigLowTopHz) kLow = std::max (kLow, lvlKf[b]); else kHigh = std::max (kHigh, lvlKf[b]); }
        for (int b = 0; b < kBands; ++b) sigKRefB[b] = std::max (lvlKf[b], sigKRefB[b] - dec);
        float pK[kBands], pM[kBands], rKf[kBands], rMf[kBands], rK[kBands], rM[kBands], oK[kBands], lK[kBands], oM[kBands], lM[kBands];
        sigRefine (lvlKf, rKf, oK, lK); sigRefine (lvlMf, rMf, oM, lM);
        for (int b = 0; b < kBands; ++b) { rK[b] = lvlKn[b] + (rKf[b] - lvlKf[b]); rM[b] = lvlMn[b] + (rMf[b] - lvlMf[b]); }   // the need levels get the same correction (the same components)
        // Main and key count wherever they have audible energy of their own: masking is the main against the key in the band (the need), not either against
        // its own loudest band (demo beat + vocal: a beat's mid / high bands sit 20-40 dB under its kick band and a vocal's consonant bands 20-30 dB under its
        // first formant, and they mask each other all the same; judged against their own loudest band the law cut 0.08 dB on average, 0.9.1 0.52). What ends
        // a key band is TIME: it counts while it stays within kSigNDb of its own recent level in that band (a peak hold falling kSigRefDecayDbPerS), so the
        // tail of a hit stops counting once it holds under 1 % of the hit's energy. The main has no time reference (its floor only).
        float loneK[kBands][2], loneM[kBands][2]; sigLone (lvlKf, loneK); sigLone (lvlMf, loneM);
        for (int b = 0; b < kBands; ++b) { pK[b] = sigPresence (lvlKf, envKf, rKf, b, sigKRefB[b], oK[b], lK[b], loneK); pM[b] = sigPresence (lvlMf, envMf, rMf, b, kKeyFloorDb + kSigNDb, oM[b], lM[b], loneM); }
        // a low-end key (its loudest band at or under kSigLowTopHz): in that band, and in any low band that reads the key within 3 dB of it (a fundamental between
        // two centres), the main is ducked by DEPTH whoever leads, while the key is present there (research section 7: engineers duck the bass at the kick
        // fundamental). A finer frequency than the band cannot be had in time: a 1/6-octave band at 40 Hz is 4.6 Hz wide and rings ~120 ms, a kick's sweep lasts 30 ms.
        const float lowKey = smoothstep ((kLow - kHigh + 1.5f) / 3.0f);
        for (int b = 0; b < kBands; ++b)
        {
            // HOW MUCH: the SPEC 1 need on the levels held at their peak and released with SPEED (a key that fades keeps its level for the release, so the cut never
            // swells as the key leaves; bench: on the unheld levels the cut rose 3 -> 5 dB in the 30 ms after a key stopped). WHETHER: the presences above.
            const float need = (rM[b] - rK[b]) + par.marginDb;
            sigDbgPK[b] = pK[b]; sigDbgPM[b] = pM[b]; sigDbgNeed[b] = need;
            // a key band so far under the main that even a DEPTH cut leaves it below the masking threshold (a tone in noise is heard at about -4 dB to the noise
            // in its auditory filter: Patterson 1976 / Moore's power-spectrum model) gains nothing from the cut: no collision there (demo: the beat's hats were cut
            // 2 dB under a vocal's sibilance 25 dB below them)
            const float reach = smoothstep ((rK[b] - rM[b] + par.depthDb + kSigUnmaskDb) / 3.0f);
            float r = std::clamp (need, 0.0f, par.depthDb) * pK[b] * pM[b] * reach;
            if (lowKey > 0.0f && fc[b] <= 1.5f * kSigLowTopHz)
                r = std::max (r, par.depthDb * lowKey * smoothstep ((lvlKf[b] - kLow + 4.5f) / 3.0f) * pK[b] * pM[b]);
            raw[b] = r;
        }
    }

    // ---- once per 32 samples: envelopes, law, smoothing, bell gains, next ramp
    void law()
    {
        ++blocks;
        const float inv = 1.0f / (float) kBlock;
        float raw[kBands];
        if (par.mode == kSelf)
        {
            huntLaw (inv);
            for (int b = 0; b < kBands; ++b) raw[b] = 0.0f;   // the 32-band bank releases and stays idle
        }
        else
        {
            for (int b = 0; b < kBands; ++b)
            {
                const double pm = (double) (sumM[b] * inv), pk = (double) (sumK[b] * inv);
                envM[b] += (pm - envM[b]) * (double) (pm > envM[b] ? cAtt : cRel);
                envK[b] += (pk - envK[b]) * (double) (pk > envK[b] ? cAtt : cRel);
                // the signal law's levels. Stage 1 (presence): a symmetric one-pole over one period of the band centre, the band's power (RMS), not its peaks
                // (bench: the 5 ms attack of the SPEC 1 follower read a steady 370 Hz tone 1.7 dB over its RMS and beating partials by their peaks). Stage 2 (need):
                // stage 1 held at its peak and released with SPEED, so a key that fades keeps its level for the release (the cut never swells as it leaves).
                envMf[b] += (pm - envMf[b]) * (double) cRelF[b];
                envKf[b] += (pk - envKf[b]) * (double) cRelF[b];
                envMn[b] = envMf[b] > envMn[b] ? envMf[b] : envMn[b] + (envMf[b] - envMn[b]) * (double) cRel;
                envKn[b] = envKf[b] > envKn[b] ? envKf[b] : envKn[b] + (envKf[b] - envKn[b]) * (double) cRel;
                // a release that is within 1e-6 (relative, -120 dB) of stage 1 has ended: snap, so two instances with different pasts become bit-identical (plugin_check
                // 13.3b: without it a fuzzed instance and a fresh one still differed by 1e-5 22 s later)
                if (envMn[b] - envMf[b] <= 1e-6 * envMf[b]) envMn[b] = envMf[b];
                if (envKn[b] - envKf[b] <= 1e-6 * envKf[b]) envKn[b] = envKf[b];
                lvlMf[b] = fastDb (envMf[b]); lvlKf[b] = fastDb (envKf[b]); lvlMn[b] = fastDb (envMn[b]); lvlKn[b] = fastDb (envKn[b]);
                sumM[b] = 0.0f; sumK[b] = 0.0f;
                lvlM[b] = toDb ((float) envM[b]); lvlK[b] = toDb ((float) envK[b]);
            }
            const double pkt = (double) (float) (sumKT * inv); sumKT = 0.0;
            envKT += (pkt - envKT) * (double) (pkt > envKT ? cAtt : cRel);
            lvlKT = toDb ((float) envKT);
            if (par.keyLawMode != kLaw091) signalLaw (raw);
            else   // the 0.9.0 / 0.9.1 law (SPEC 1), dyneqraw --keylaw 091 only
            for (int b = 0; b < kBands; ++b)
            {
                const float need = (lvlM[b] - lvlK[b]) + par.marginDb;
                const float keyRel = lvlK[b] - lvlKT;
                if (lvlK[b] < kKeyFloorDb || keyRel < kPresenceLoDb) { raw[b] = 0.0f; continue; }
                const float presence = smoothstep ((keyRel - kPresenceLoDb) / (kPresenceHiDb - kPresenceLoDb));
                raw[b] = std::clamp (need, 0.0f, par.depthDb) * presence;
            }
            for (int s = 0; s < kHunters; ++s) hunt[s].raw = 0.0f;   // hunter slots release and stay idle
        }
        // time: rise at most kRiseDbPerMs, fall with RELEASE. FOCUS is applied here and again after the spread, so a band
        // outside FOCUS never cuts and never pushes cut into its neighbour.
        for (int b = 0; b < kBands; ++b)
        {
            if (! inFocus[b]) raw[b] = 0.0f;
            if (raw[b] > cutSm[b]) cutSm[b] = std::min (raw[b], cutSm[b] + maxRise);
            else cutSm[b] += (raw[b] - cutSm[b]) * cRel;
            if (cutSm[b] < 1e-4f) cutSm[b] = 0.0f;
        }
        // frequency: peak-preserving spread (SPEC 7.1), then FOCUS (bands outside are never cut)
        for (int b = 0; b < kBands; ++b)
        {
            const float lo = cutSm[std::max (b - 1, 0)], hi = cutSm[std::min (b + 1, kBands - 1)];
            // the signal law does not spread (bench: the spread put 0.5 x a neighbour's cut into bands with no collision, a FALSE CUT; the bells are 1.4 bands wide anyway)
            float t = par.keyLawMode != kLaw091 ? cutSm[b] : std::max (cutSm[b], kSpreadFactor * std::max (lo, hi));
            const float goal = inFocus[b] ? 1.0f : 0.0f;   // FOCUS: a band that enters or leaves fades (static FOCUS: exactly 1 or 0, as before)
            focusFade[b] += std::clamp (goal - focusFade[b], -fadeStep, fadeStep);
            t *= focusFade[b];
            if (t < 1e-4f) t = 0.0f;
            target[b] = t;
        }
        if (bakeOn)
        {
            if (par.mode == kCollide)
            {
                ++bakeSeen;
                const double ms = bakeMainSum * (double) inv;
                if (ms > 0.0 && 10.0 * std::log10 (ms) >= (double) kBakeGateDb)
                {
                    ++bakeCounted;
                    for (int b = 0; b < kBands; ++b) { bakeSum[b] += (double) target[b]; if (target[b] > kBakeCutSeenDb) ++bakeCutBlocks[b]; }
                }
            }
            bakeMainSum = 0.0;
        }
        // bell gains
        if (par.solveBells)
        {
            // Gauss-Seidel: gain[b] = max (target[b] - sum over k != b of gain[k] * shape[b][k], 0), the sum in the order k = 0 .. 31, gains below b
            // already updated in this pass. Same arithmetic as that plain double loop, bit for bit, in column order: the terms k < b of every row
            // are applied to a row accumulator as soon as gain[k] exists (one vector update per finished row), so only the terms k > b stay in the
            // serial chain behind a row (half the dependent multiply-subtracts).
            // nothing to solve: zero targets and zero gains give zero gains again (idle bank, and SELF, where the bank is held at zero)
            bool idle = true;
            for (int b = 0; b < kBands; ++b) if (target[b] != 0.0f || gain[b] != 0.0f) { idle = false; break; }
            for (int it = 0; it < (idle ? 0 : kSolveIters); ++it)
            {
                float acc[kBands];
                for (int b = 0; b < kBands; ++b) acc[b] = target[b];
#if defined (__clang__)
                #pragma clang loop unroll(full)
#endif
                for (int j = 0; j < kBands; ++j)
                {
                    float r = acc[j];
                    const float* sj = shape[j];
#if defined (__clang__)
                    #pragma clang loop vectorize(disable) interleave(disable) unroll(full)   // a vectorised chain splits the multiply from the subtract (no fused multiply-subtract): other bits
#endif
                    for (int k = j + 1; k < kBands; ++k) r -= gain[k] * sj[k];
                    const float g = std::max (r, 0.0f);
                    gain[j] = g;
                    const float* col = shapeT[j];
                    for (int b = 0; b < kBands; ++b) acc[b] -= g * col[b];   // rows <= j are finished: their accumulators are never read again, so the full width is harmless
                }
            }
            for (int b = 0; b < kBands; ++b) if (target[b] <= 0.0f && gain[b] < 1e-3f) gain[b] = 0.0f;
        }
        else
            for (int b = 0; b < kBands; ++b) gain[b] = target[b];

        // next ramp: the bell's linear gain A = 10^(-cut/40) moves linearly from the previous value to the new one over the
        // next sub-block, and the coefficients are derived from A at every sample (a linear interpolation of the
        // coefficients themselves leaves a block-rate residue: -73 dBc at 1000 - 1500 Hz on a gated 1 kHz collision).
        anyActive = false;
        for (int b = 0; b < kBands; ++b)
        {
            const bool on = gain[b] > 0.0f || gainStart[b] > 0.0f;
            if (on && ! active[b]) std::memset (bst[b], 0, sizeof (bst[b]));
            active[b] = on;
            if (on)
            {
                bellA[b] = std::pow (10.0f, -gainStart[b] / 40.0f);   // exact restart from the previous end point
                bellDA[b] = (std::pow (10.0f, -gain[b] / 40.0f) - bellA[b]) * inv;
                anyActive = true;
            }
            gainStart[b] = gain[b];
        }
        // hunter slots: time smoothing, glide, next ramp (runs in both modes so a slot can release after a mode change)
        for (int s = 0; s < kHunters; ++s)
        {
            Slot& h = hunt[s];
            if (h.raw > h.cut) h.cut = std::min (h.raw, h.cut + maxRise);
            else h.cut += (h.raw - h.cut) * cRel;
            if (h.cut < 1e-4f) h.cut = 0.0f;
            if (h.matched)
            {
                const float d = std::log2 (h.targetFreq / h.freq);
                h.freq *= std::exp2 (std::clamp (d, -maxGlideOct, maxGlideOct));
                h.q += (h.targetQ - h.q) * cHQ;
            }
            const bool on = h.cut > 0.0f || h.cutStart > 0.0f;
            const bool fresh = on && ! h.active;
            if (fresh) std::memset (h.st, 0, sizeof (h.st));
            h.active = on;
            if (on)
            {
                // the bell's shape (cos w0, alpha) moves linearly over the next sub-block as well, like its gain: a per-block
                // step of the centre left a -89.8 dBc line at the block rate (1500 Hz) on a gliding sine (measured, STATUS.md)
                const float w0 = 2.0f * (float) M_PI * h.freq / (float) sr;
                const float cwT = std::cos (w0), alT = std::sin (w0) / (2.0f * h.q);
                if (fresh) { h.cosw0 = cwT; h.alpha = alT; }
                h.dCosw0 = (cwT - h.cosw0) * inv; h.dAlpha = (alT - h.alpha) * inv;
                h.A = std::pow (10.0f, -h.cutStart / 40.0f);
                h.dA = (std::pow (10.0f, -h.cut / 40.0f) - h.A) * inv;
                anyActive = true;
            }
            h.cutStart = h.cut;
        }
        targetLaw();
    }

    // ---- SELF: 64-band envelopes, spectral envelope, excess, peak picking, slot matching (SPEC 7.4)
    void huntLaw (float inv)
    {
        for (int b = 0; b < kHBands; ++b)
        {
            const float pm = sumH[b] * inv;
            envH[b] += (pm - envH[b]) * (pm > envH[b] ? cAtt : cRel);
            sumH[b] = 0.0f;
            lvlH[b] = toDb (envH[b]);
        }
        for (int b = 0; b < kHBands; ++b)
        {
            float win[2 * (kHMedFar - kHMedNear + 1)]; int cnt = 0;
            for (int k = std::max (0, b - kHMedFar); k <= b - kHMedNear; ++k) win[cnt++] = lvlH[k];
            for (int k = b + kHMedNear; k <= std::min (kHBands - 1, b + kHMedFar); ++k) win[cnt++] = lvlH[k];
            std::sort (win, win + cnt);
            const float med = (cnt & 1) ? win[cnt / 2] : 0.5f * (win[cnt / 2 - 1] + win[cnt / 2]);
            if (lvlH[b] < kKeyFloorDb) { hEnv[b] = med; excess[b] = 0.0f; continue; }   // silence: nothing to hunt; the envelope follows at once
            if (hEnv[b] <= kDbFloor + 1.0f) hEnv[b] = med;                               // first block with signal: start at the median
            hEnv[b] += (med - hEnv[b]) * cHEnv;
            excess[b] = lvlH[b] - hEnv[b];
        }
        // candidates: local maxima of excess above SENS, inside FOCUS, strongest first, at least kHMinSep bands apart
        struct Cand { float freq, q, cut, exc; int band; };
        Cand cand[kHunters]; int nc = 0;
        int peaks[kHBands]; int np = 0;
        for (int b = 0; b < kHBands; ++b)
        {
            if (excess[b] <= par.marginDb || lvlH[b] < kKeyFloorDb) continue;
            const float lo = b > 0 ? excess[b - 1] : -1e9f, hi = b < kHBands - 1 ? excess[b + 1] : -1e9f;
            if (excess[b] >= lo && excess[b] > hi) peaks[np++] = b;
        }
        // selection sort by excess, descending; greedy separation
        for (int i = 0; i < np; ++i)
            for (int j = i + 1; j < np; ++j)
                if (excess[peaks[j]] > excess[peaks[i]]) std::swap (peaks[i], peaks[j]);
        for (int i = 0; i < np && nc < kHunters; ++i)
        {
            const int b = peaks[i]; bool ok = true;
            for (int c = 0; c < nc; ++c) if (std::abs (cand[c].band - b) < kHMinSep) { ok = false; break; }
            if (! ok) continue;
            // frequency by parabolic interpolation over the three bands around the peak (band-index domain)
            const float ym = b > 0 ? excess[b - 1] : excess[b], y0 = excess[b], yp = b < kHBands - 1 ? excess[b + 1] : excess[b];
            const float den = ym - 2.0f * y0 + yp;
            float delta = den < -1e-6f ? 0.5f * (ym - yp) / den : 0.0f;
            delta = std::clamp (delta, -0.5f, 0.5f);
            const float ypk = y0 - 0.25f * (ym - yp) * delta;
            const float freq = hIndexToHz ((float) b + delta);
            if (freq < par.focusLowHz || freq > par.focusHighHz) continue;
            // Q from the -3 dB width of the excess peak: linear crossings on both sides, in band-index units
            const float thr = ypk - 3.0f;
            float left = 0.0f, right = (float) (kHBands - 1);
            for (int k = b; k > 0; --k) if (excess[k - 1] <= thr) { left = (float) k - (excess[k] - thr) / std::max (excess[k] - excess[k - 1], 1e-6f); break; }
            for (int k = b; k < kHBands - 1; ++k) if (excess[k + 1] <= thr) { right = (float) k + (excess[k] - thr) / std::max (excess[k] - excess[k + 1], 1e-6f); break; }
            const float width = std::max (hIndexToHz (right) - hIndexToHz (left), 1e-3f);
            const float q = std::clamp (freq / width, kHQMin, kHQMax);
            const float presence = smoothstep ((lvlH[b] - kHPresenceLoDb) / (kHPresenceHiDb - kHPresenceLoDb));
            const float cut = std::clamp (ypk - par.marginDb, 0.0f, par.depthDb) * presence;
            cand[nc++] = { freq, q, cut, ypk, b };
        }
        // matching: a slot that still has gain keeps the candidate within 1/6 octave of its centre (nearest wins)
        bool used[kHunters] = {};
        for (int s = 0; s < kHunters; ++s)
        {
            Slot& h = hunt[s]; h.matched = false; h.raw = 0.0f;
            if (! (h.cut > 0.0f || h.cutStart > 0.0f)) continue;
            int best = -1; float bestD = kHHysteresisOct;
            for (int c = 0; c < nc; ++c)
            {
                if (used[c]) continue;
                const float d = std::abs (std::log2 (cand[c].freq / h.freq));
                if (d <= bestD) { bestD = d; best = c; }
            }
            if (best >= 0) { used[best] = true; h.matched = true; h.targetFreq = cand[best].freq; h.targetQ = cand[best].q; h.raw = cand[best].cut; }
        }
        // the remaining candidates (strongest first) take the free slots: a free slot is at 0 dB, so its centre may jump
        for (int c = 0; c < nc; ++c)
        {
            if (used[c]) continue;
            for (int s = 0; s < kHunters; ++s)
            {
                Slot& h = hunt[s];
                if (h.matched || h.cut > 0.0f || h.cutStart > 0.0f) continue;
                h.matched = true; used[c] = true;
                h.freq = h.targetFreq = cand[c].freq; h.q = h.targetQ = cand[c].q; h.raw = cand[c].cut;
                break;
            }
        }
    }

    // RBJ peaking bell, gain = -cutDb, normalised {b0, b1, b2, a1, a2}
    void bellCoeffs (int b, float cutDb, float* c) const
    {
        const float A = std::pow (10.0f, -cutDb / 40.0f);
        const float al = sinw0[b] / (2.0f * bellQ[b]);
        const float a0 = 1.0f + al / A, ia0 = 1.0f / a0;
        c[0] = (1.0f + al * A) * ia0; c[1] = -2.0f * cosw0[b] * ia0; c[2] = (1.0f - al * A) * ia0;
        c[3] = c[1]; c[4] = (1.0f - al / A) * ia0;
    }

    // one RBJ peaking bell with A ramping per sample (A = linear gain): a0 = 1 + al/A, so 1/a0 = A/(A + al); a2 = (A - al)/(A + al).
    // RampShape: cos w0 and alpha ramp per sample as well (hunter bells); false keeps the 32-band path exactly as before.
    template <bool RampShape>
    static inline void runBell (float* const* main, int numCh, int pos, int m, float& al, float dal, float& cw0, float dcw, float& A, float dA, float* st)
    {
        float m2c = -2.0f * cw0;
        if (numCh == 2)
        {
            float* l = main[0] + pos; float* r = main[1] + pos;
            float s1l = st[0], s2l = st[1], s1r = st[2], s2r = st[3];
            for (int i = 0; i < m; ++i)
            {
                A += dA;
                if (RampShape) { al += dal; cw0 += dcw; m2c = -2.0f * cw0; }
                const float inv = 1.0f / (A + al), ia0 = A * inv, alA = al * A;
                const float b0 = (1.0f + alA) * ia0, b1 = m2c * ia0, b2 = (1.0f - alA) * ia0, a2 = (A - al) * inv;
                const float xl = l[i], xr = r[i];
                const float yl = b0 * xl + s1l, yr = b0 * xr + s1r;
                s1l = b1 * xl - b1 * yl + s2l; s1r = b1 * xr - b1 * yr + s2r;
                s2l = b2 * xl - a2 * yl;       s2r = b2 * xr - a2 * yr;
                l[i] = yl; r[i] = yr;
            }
            st[0] = s1l; st[1] = s2l; st[2] = s1r; st[3] = s2r;
        }
        else
        {
            float* l = main[0] + pos; float s1 = st[0], s2 = st[1];
            for (int i = 0; i < m; ++i)
            {
                A += dA;
                if (RampShape) { al += dal; cw0 += dcw; m2c = -2.0f * cw0; }
                const float inv = 1.0f / (A + al), ia0 = A * inv, alA = al * A;
                const float b0 = (1.0f + alA) * ia0, b1 = m2c * ia0, b2 = (1.0f - alA) * ia0, a2 = (A - al) * inv;
                const float x = l[i], y = b0 * x + s1;
                s1 = b1 * x - b1 * y + s2; s2 = b2 * x - a2 * y; l[i] = y;
            }
            st[0] = s1; st[1] = s2;
        }
    }

    // G consecutive active bank bells (stereo) run as one skewed pipeline: band g works on sample i - g while band 0 works on sample i. A bell's
    // output is the next bell's input, one sample behind, so every sample goes through the same bells in the same order with the same
    // arithmetic as runBell (bit-identical); the point is that the G recursions are independent chains the core can overlap (one bell alone is
    // bound by the latency of its own feedback).
    template <int G>
    void runBellGroup (float* const* main, int pos, int m, const int* idx)
    {
        float* l = main[0] + pos; float* r = main[1] + pos;
        float A[G], dA[G], al[G], m2c[G], s1l[G], s2l[G], s1r[G], s2r[G];
        for (int g = 0; g < G; ++g)
        {
            const int b = idx[g];
            A[g] = bellA[b]; dA[g] = bellDA[b]; al[g] = bellAlpha[b]; m2c[g] = -2.0f * cosw0[b];
            s1l[g] = bst[b][0]; s2l[g] = bst[b][1]; s1r[g] = bst[b][2]; s2r[g] = bst[b][3];
        }
        auto one = [&] (int g, int i)
        {
            A[g] += dA[g];
            const float inv = 1.0f / (A[g] + al[g]), ia0 = A[g] * inv, alA = al[g] * A[g];
            const float b0 = (1.0f + alA) * ia0, b1 = m2c[g] * ia0, b2 = (1.0f - alA) * ia0, a2 = (A[g] - al[g]) * inv;
            const float xl = l[i], xr = r[i];
            const float yl = b0 * xl + s1l[g], yr = b0 * xr + s1r[g];
            s1l[g] = b1 * xl - b1 * yl + s2l[g]; s1r[g] = b1 * xr - b1 * yr + s2r[g];
            s2l[g] = b2 * xl - a2 * yl;          s2r[g] = b2 * xr - a2 * yr;
            l[i] = yl; r[i] = yr;
        };
        for (int s = 0; s < m + G - 1; ++s)
        {
            if (s >= G - 1 && s < m)
            {
                for (int g = 0; g < G; ++g) one (g, s - g);
            }
            else
                for (int g = 0; g < G; ++g) { const int i = s - g; if (i >= 0 && i < m) one (g, i); }
        }
        for (int g = 0; g < G; ++g)
        {
            const int b = idx[g];
            bellA[b] = A[g];
            bst[b][0] = s1l[g]; bst[b][1] = s2l[g]; bst[b][2] = s1r[g]; bst[b][3] = s2r[g];
        }
    }

    void apply (float* const* main, int numCh, int pos, int m)
    {
        if (numCh == 2)
        {
            int idx[kBands], n = 0;
            for (int b = 0; b < kBands; ++b) if (active[b]) idx[n++] = b;
            int q = 0;
            for (; q + kBellGroup <= n; q += kBellGroup) runBellGroup<kBellGroup> (main, pos, m, idx + q);
            for (; q < n; ++q) { const int b = idx[q]; runBell<false> (main, numCh, pos, m, bellAlpha[b], 0.0f, cosw0[b], 0.0f, bellA[b], bellDA[b], bst[b]); }
        }
        else
        for (int b = 0; b < kBands; ++b)
            if (active[b]) runBell<false> (main, numCh, pos, m, bellAlpha[b], 0.0f, cosw0[b], 0.0f, bellA[b], bellDA[b], bst[b]);
        for (int s = 0; s < kHunters; ++s)
        {
            Slot& h = hunt[s];
            if (h.active) runBell<true> (main, numCh, pos, m, h.alpha, h.dAlpha, h.cosw0, h.dCosw0, h.A, h.dA, h.st);
        }
    }


    // ---- TARGET (SPEC 8b / 8c, TASK_P1g). One voice runs at a time (the voice the instrument and PART slider point at). Per sample: mono capture
    // ring + an onset detector (2nd-order band-pass on the voice's range; fast power > slow power x sensDb). Per hit, 200 ms later (or when the
    // next onset arrives): the three windows are read, the features go into an 8-hit memory, the cut follows the running median.
    struct Bq
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        inline double run (double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
        void clear() { z1 = z2 = 0.0; }
        void design (bool highPass, double f, double rate, double q = 0.70710678118654752)
        {
            const double w0 = 2.0 * M_PI * f / rate, cw = std::cos (w0), al = std::sin (w0) / (2.0 * q), a0 = 1.0 + al;
            b0 = (highPass ? 0.5 * (1.0 + cw) : 0.5 * (1.0 - cw)) / a0; b1 = (highPass ? -(1.0 + cw) : (1.0 - cw)) / a0; b2 = b0;
            a1 = -2.0 * cw / a0; a2 = (1.0 - al) / a0;
        }
    };
    struct Fft
    {
        int n = 0; std::vector<double> cs, sn; std::vector<int> rev;
        void init (int N)
        {
            n = N; cs.assign ((size_t) N / 2, 0.0); sn.assign ((size_t) N / 2, 0.0); rev.assign ((size_t) N, 0); int lg = 0; while ((1 << lg) < N) ++lg;
            for (int i = 0; i < N / 2; ++i) { cs[(size_t) i] = std::cos (2.0 * M_PI * i / N); sn[(size_t) i] = -std::sin (2.0 * M_PI * i / N); }
            for (int i = 0; i < N; ++i) { int r = 0; for (int b = 0; b < lg; ++b) if (i & (1 << b)) r |= 1 << (lg - 1 - b); rev[(size_t) i] = r; }
        }
        void run (double* re, double* im) const
        {
            for (int i = 0; i < n; ++i) { const int j = rev[(size_t) i]; if (j > i) { std::swap (re[i], re[j]); std::swap (im[i], im[j]); } }
            for (int len = 2; len <= n; len <<= 1)
            {
                const int half = len >> 1, step = n / len;
                for (int i = 0; i < n; i += len)
                    for (int j = 0; j < half; ++j)
                    {
                        const double wr = cs[(size_t) (j * step)], wi = sn[(size_t) (j * step)]; const int a = i + j, b = a + half;
                        const double tr = re[b] * wr - im[b] * wi, ti = re[b] * wi + im[b] * wr;
                        re[b] = re[a] - tr; im[b] = im[a] - ti; re[a] += tr; im[a] += ti;
                    }
            }
        }
    };
    struct HitRec
    {
        float key = 0.0f, f0 = 0.0f, tEnd = 0.0f;
        float c[kMaxParts] {}, lo[kMaxParts] {}, hi[kMaxParts] {}; bool ok[kMaxParts] {};
    };
    struct TBellState
    {
        int   tag = -1;                                         // voice * 8 + part the bell is tuned for; it changes only while the bell is idle
        float freq = 1000.0f, q = 1.0f, gLim = 0.0f, gSm = 0.0f, gStart = 0.0f;   // dB, positive = cut: rate-limited target, smoothed (what the bell runs to), value at the sub-block start
        float cosw0 = 0.0f, dCosw0 = 0.0f, alpha = 0.1f, dAlpha = 0.0f, A = 1.0f, dA = 0.0f, st[4] {};
        bool  active = false;
    };
    // SPEC 8e.2: one edge cut = a Linkwitz-Riley split (Butterworth n squared: 12 / 24 / 36 / 48 / 72 dB per octave = n 1 / 2 / 3 / 4 / 6) at the learned edge.
    // The cut band's gain is -AMOUNT, the other band's 0 dB; LP + HP is all-pass (HP is subtracted for the 4k+2 orders, LR12 and LR36). While no cut is wanted the stage
    // is out of the path (x), so the output is exact between hits and with TARGET OFF. Every section is a trapezoidal state-variable filter (the same transfer function
    // as the bilinear Butterworth biquad, double precision): its states are the integrators' values, so the crossover can glide and the coefficients ramp per
    // sample without a click (a biquad's states do not survive a coefficient step: the glide of a learned edge was a -40 dBFS tick train above 1 kHz).
    struct EdgeBank
    {
        static constexpr int kMaxSec = 6;
        int ns = 0; bool flip = false;
        bool first[kMaxSec] {}; double k[kMaxSec] {};
        double a1[kMaxSec] {}, a2[kMaxSec] {}, a3[kMaxSec] {}, d1[kMaxSec] {}, d2[kMaxSec] {}, d3[kMaxSec] {};
        double zl[2][kMaxSec][2] {}, zh[2][kMaxSec][2] {};
        void clear() { std::memset (zl, 0, sizeof (zl)); std::memset (zh, 0, sizeof (zh)); }
        void setup (int idx)   // the sections of one Butterworth factor, used twice (the square); k = 1 / Q, first-order sections marked
        {
            static const double qs[kSlopeSteps][3] = { { 0.0, 0, 0 }, { 0.70710678118654752, 0, 0 }, { 0.0, 1.0, 0 }, { 0.54119610014619698, 1.30656296487637653, 0 }, { 0.51763809020504152, 0.70710678118654752, 1.93185165257813657 } };
            static const int cnt[kSlopeSteps] = { 1, 1, 2, 2, 3 };
            ns = 2 * cnt[idx]; flip = idx == 0 || idx == 2;
            for (int j = 0; j < ns; ++j) { const double q = qs[idx][j % cnt[idx]]; first[j] = q <= 0.0; k[j] = q <= 0.0 ? 0.0 : 1.0 / q; }
        }
        // coefficients for crossover f at the end of the next sub-block: they ramp there per sample (snap: they are there now)
        void target (double f, double rate, double inv, bool snap)
        {
            const double g = std::tan (M_PI * std::min (f, 0.49 * rate) / rate);
            for (int j = 0; j < ns; ++j)
            {
                double t1, t2 = 0.0, t3 = 0.0;
                if (first[j]) t1 = g / (1.0 + g); else { t1 = 1.0 / (1.0 + g * (g + k[j])); t2 = g * t1; t3 = g * t2; }
                if (snap) { a1[j] = t1; a2[j] = t2; a3[j] = t3; d1[j] = d2[j] = d3[j] = 0.0; }
                else { d1[j] = (t1 - a1[j]) * inv; d2[j] = (t2 - a2[j]) * inv; d3[j] = (t3 - a3[j]) * inv; }
            }
        }
        inline void advance() { for (int j = 0; j < ns; ++j) { a1[j] += d1[j]; a2[j] += d2[j]; a3[j] += d3[j]; } }
        inline void run (int ch, double x, double& lo, double& hi)
        {
            double v = x, u = x;
            for (int j = 0; j < ns; ++j)
            {
                double* s = zl[ch][j]; double* t = zh[ch][j];
                if (first[j])
                {
                    const double vv = (v - s[0]) * a1[j], l = vv + s[0]; s[0] = l + vv; v = l;
                    const double ww = (u - t[0]) * a1[j], m = ww + t[0]; t[0] = m + ww; u -= m;   // the high band is what the low-pass leaves
                }
                else
                {
                    const double v3 = v - s[1], v1 = a1[j] * s[0] + a2[j] * v3, v2 = s[1] + a2[j] * s[0] + a3[j] * v3;
                    s[0] = 2.0 * v1 - s[0]; s[1] = 2.0 * v2 - s[1]; v = v2;
                    const double w3 = u - t[1], w1 = a1[j] * t[0] + a2[j] * w3, w2 = t[1] + a2[j] * t[0] + a3[j] * w3;
                    t[0] = 2.0 * w1 - t[0]; t[1] = 2.0 * w2 - t[1]; u = u - k[j] * w1 - w2;
                }
            }
            lo = v; hi = u;
        }
    };
    struct EdgeStage
    {
        bool below = true;                                      // true: the cut band is the low-pass side
        int tag = -1, slope = 1, oldSlope = 1;                  // what the stage is tuned for (it retunes only while its cut is out); index into kSlopeDb
        float freq = 1000.0f, gLim = 0.0f, gSm = 0.0f, gStart = 0.0f;   // crossover Hz; cut dB (positive): rate-limited target, smoothed, value at the sub-block start
        double c = 1.0, dc = 0.0, mix = 0.0, dMix = 0.0, w = 1.0, dW = 0.0;   // cut band's linear gain; split-in-the-path blend; weight of the new filter set (they ramp per sample over a sub-block)
        bool armed = false, run = false, oldRun = false;
        EdgeBank cur, old;
    };
    TBellState tbs[kTargetBells]; EdgeStage edge[2]; bool tgtRun = false; bool revOn = false;
    // the plateau of a non-edge part, planned from the learned points (recomputed only when they, the protected neighbour or WIDTH change)
    struct PlateauPlan { float lo = 0.0f, hi = 0.0f, q = 1.0f, k = 1.0f, lossDb = 0.0f; float kCen = -1.0f, kLo = -1.0f, kHi = -1.0f, kProt = -1.0f, kW = -1.0f; } pplan;
    void planPlateau (float cen, float elo, float ehi, float protF, float width)
    {
        PlateauPlan& P = pplan;
        if (P.kCen == cen && P.kLo == elo && P.kHi == ehi && P.kProt == protF && P.kW == width) return;
        P.kCen = cen; P.kLo = elo; P.kHi = ehi; P.kProt = protF; P.kW = width;
        const float span = std::clamp (kPlateauSpan * std::log2 (std::max (ehi, cen * 1.01f) / std::max (std::min (elo, cen / 1.01f), 1.0f)) * width, kPlateauMinOct, kPlateauMaxOct);
        float lo = cen * std::exp2 (-0.5f * span), hi = std::min (cen * std::exp2 (0.5f * span), 0.45f * (float) sr);
        lo = std::max (lo, 20.0f);
        auto shape = [&] (float l, float h, float& q, float& k)   // Q from the spacing; k = mean cut over [l, h] / per-bell cut, at the reference cut
        {
            const double sp = 0.5 * std::log2 ((double) h / (double) l);
            q = (float) (kPlateauQMul / (std::exp2 (0.5 * sp) - std::exp2 (-0.5 * sp)));
            double sum = 0.0; const int n = 9;
            for (int j = 0; j < n; ++j)
            {
                const double fj = (double) l * std::pow ((double) h / (double) l, (double) j / (n - 1)); double c = 0.0;
                for (int i = 0; i < kTargetBells; ++i) c -= bellDbAt (fj, (double) l * std::pow ((double) h / (double) l, 0.5 * i), (double) q, -(double) kPlateauRefDb, sr);
                sum += c;
            }
            k = (float) (sum / n / (double) kPlateauRefDb);
        };
        float q = 1.0f, k = 1.0f; shape (lo, hi, q, k);
        auto lossAt = [&] (float fp, float l, float h, float qq, float kk)   // the cut at the protected centre at the deepest plateau (kBellMaxDb)
        {
            double c = 0.0; const double g = (double) kBellMaxDb / std::max ((double) kk, 0.3);
            for (int i = 0; i < kTargetBells; ++i) c -= bellDbAt ((double) fp, (double) l * std::pow ((double) h / (double) l, 0.5 * i), (double) qq, -g, sr);
            return (float) c;
        };
        float loss = protF > 0.0f ? lossAt (protF, lo, hi, q, k) : 0.0f;
        for (int it = 0; it < 24 && protF > 0.0f && loss > kProtectLossDb && lo < cen / 1.19f; ++it)   // SPEC 8g: the protected neighbour (the SUB under a BODY) loses <= 1 dB: the low end of the plateau moves up
        {
            lo *= std::exp2 (1.0f / 24.0f); shape (lo, hi, q, k); loss = lossAt (protF, lo, hi, q, k);
        }
        P.lo = lo; P.hi = hi; P.q = q; P.k = std::max (k, 0.3f); P.lossDb = loss;
    }
    // SPEC 8g: where each edge was placed (the rule's crossover, the placed one), the loss at the protected part's centre and the cut reached at the target part's centre, with the key it was computed for
    struct EdgePlan
    {
        bool on = false; float ruleF = 0.0f, f = 0.0f, prot = 0.0f, ref = 0.0f, lossDb = 0.0f, achDb = 0.0f;
        float kDef = -1.0f, kMin = -1.0f, kProt = -1.0f, kRef = -1.0f, kAmt = -1.0f; int kSlope = -1; bool kBelow = true;
    };
    EdgePlan plan[2]; bool gentle = false;
    // the crossover for edge k from the rule's one (defF): moved away from the protected part until its centre loses <= 1 dB at this SLOPE and AMOUNT (SPEC 8g); protF = 0: nothing to protect
    float planEdge (int k, bool below, float defF, float protF, float refF, float fMin = 20.0f, float fMax = -1.0f)
    {
        EdgePlan& P = plan[k]; const int si = slopeIndex (par.targetSlope); const float amt = std::min (par.targetAmount, kTargetAmountMaxDb);
        if (! (P.kDef == defF && P.kMin == fMin && P.kProt == protF && P.kRef == refF && P.kAmt == amt && P.kSlope == si && P.kBelow == below))
        {
            P.kDef = defF; P.kMin = fMin; P.kProt = protF; P.kRef = refF; P.kAmt = amt; P.kSlope = si; P.kBelow = below;
            P.ruleF = defF; P.f = std::clamp (edgePlaceHz (below, defF, protF, si, amt, sr), fMin, fMax > fMin ? fMax : 0.45f * (float) sr);   // the clamp is the stage's own range
            P.prot = protF; P.ref = refF;
            P.lossDb = protF > 0.0f ? (float) edgeCutDbAt (protF, P.f, si, amt, below, sr) : 0.0f;
            P.achDb = refF > 0.0f ? (float) edgeCutDbAt (refF, P.f, si, amt, below, sr) : amt;
        }
        P.on = true; return P.f;
    }
    // one memory per slot (SPEC 8d). The memory (rec, nRec, L) outlives reset(); only prepare() of a brand-new engine and a failed verification empty it.
    struct TSlot
    {
        int voice = 0; bool bus = false;
        Bq dHp, dLp, rHp, rLp; double cF = 0.0; bool hasRej = false;   // DRUMS-bus slots only: the instrument's own band and the band that must not be louder (classifyBus)
        int64_t onsetAbs = -1;   // the event clock: the last onset this slot accepted
        HitRec rec[kMemHits]; int nRec = 0, devCount = 0, nOnsets = 0, nLearned = 0, nRelearn = 0; float keyMed = 0.0f, farKey = 0.0f; bool quick = false;   // farKey: the key of the previous far hit; quick: the memory was replaced by 2 hits, the cut stays at full amount until the third   // nRelearn: sound changes seen (a new sample, a failed check); a statistic, never saved
        int rejectWhy[4] {};
        int verifyLeft = 0, verifyOk = 0, verifyBad = 0;   // after a state load: the first hits are checked against the stored memory
        TargetLearned L;
    };
    TSlot ts[kTSlots]; int curSlot = -1; uint32_t memVersion = 0;
    HitRec impTmp[kTSlots][kMemHits];   // memoryImport's scratch
    bool listening = true, edgeForceIn = false;   // only the CPU measurement (dyneqraw --no-listen) switches this off; the plug-in never does
    std::vector<float> cap; int64_t capMask = 0, tSamp = 0;
    int vPart = 0;
    Fft fftE, fftT; int nE = 0, nT = 0;
    std::vector<double> xe, xr, re_, im_, scrSm, scrPre, acY;
    std::vector<double> win;

    // ---- the evidence collector (SPEC 8e.3). Idle: the mono sum goes into a ring (0.5 s) and ONE onset detector watches it in groups of kDec samples (the power of the sum and of its first
    // difference, each: a fast follower against the slow level 10 ms ago). Nothing else runs. On an onset the hit waits kColSettleMs, its first sample is located in the ring and (DRUMS bus)
    // it is classified; 200 ms later ONE evidence record is built from the ring in small jobs (one per kJobEveryTicks ticks, so no block spikes): the windows' spectra are computed once and
    // every slot reads its search ranges from them, then pushes its reading into its own memory (median of 8).
    struct PendHit { int64_t t = 0, o = 0, settleAt = 0; uint8_t mask = 0, coin = 0; bool settled = false; };   // coin: DRUMS-bus slots that took the hit as "struck together with the other instrument", confirmed by the analysis (coincidentIsOwn)   // t: the event clock's onset, o: the analysis windows' zero, mask: the slots that accepted it
    static constexpr int kQueue = 16;
    PendHit queue[kQueue]; int qHead = 0, qCount = 0;
    double accA = 0.0, accH = 0.0, fA = 0.0, sA = 0.0, fH = 0.0, sH = 0.0, cFA = 0.0, cFH = 0.0, cSl = 0.0, ratioC = 1.0, sqrtRatioC = 1.0;
    std::vector<double> slowA, slowH; int slowPos = 0; double decPrev = 0.0; int decCnt = 0;
    bool armedC = true; int64_t gapLeftC = 0, warmLeftC = 0, gapSamplesC = 0;
    static constexpr int kFlav = 3;
    static constexpr double kFlavHz[kFlav] = { 125.0, 400.0, 750.0 };   // the transient's high-pass corners (half the lower edge of the parts they serve)
    static int flavorOf (const PartDef& P) { int f = 0; for (int i = 0; i < kFlav; ++i) if (0.5 * (double) P.lo >= kFlavHz[i]) f = i; return f; }
    struct EvJob
    {
        bool active = false; int stage = 0, cursor = 0; PendHit h;
        int64_t o = 0, tS = 0, tE = 0, eS = 0, eE = 0, lS = 0, endAbs[kTSlots] {};
        float f0[kTSlots] {}; bool f0Ok[kTSlots] {}; bool prOk[kTSlots] {}, dtOk[kFlav] {}; uint8_t needFlav = 0, need = 0; bool peOk = false;
        float tEnd[kTargetVoices] {};
        int sub = 0, curK = 0; bool busy = false; int64_t startAbs = 0;   // the place of the resumable job: the stage's piece, the slot / flavour it is on, whether a piece is under way
        struct Work   // the state of the piece under way (reset when a piece starts)
        {
            int ph = 0, D = 1, n = 0, lmin = 0, lmax = 0, l = 0, t = 0, len = 2, fi = 0, fj = 0; int64_t i = 0, j = 0;
            double sw2 = 0.0, e0 = 0.0, fs = 0.0, f0 = 0.0, num = 0.0, d1 = 0.0, d2 = 0.0, cr = 0.0, ci = 0.0, w = 0.0;
            double rr = 0.0, u = 0.0, v = 0.0, suu = 0.0, suv = 0.0, svv = 0.0, sxu = 0.0, sxv = 0.0, sxx = 0.0, bestRes = 1e300, ba = 0.0, bb = 0.0, br = 1.0, cw = 0.0, sw = 0.0;
            Bq q1, q2; double e[64] {};
        } w;
    };
    EvJob ev; std::vector<double> PeS, PrS[kTSlots], PeH[kFlav], PtH[kFlav], DtS[kFlav];

    bool tgtAnyActive() const { for (auto& b : tbs) if (b.active || b.gSm != 0.0f || b.gStart != 0.0f) return true; return edge[0].run || edge[1].run; }
    int64_t ms2s (float ms) const { return (int64_t) std::llround ((double) ms * 0.001 * sr); }
    static int nextPow2 (int v) { int p = 1; while (p < v) p <<= 1; return p; }

    void targetPrepare()   // all the allocation happens here, never in process(). The memory is not touched (a new Engine starts empty).
    {
        const int capN = std::max (16384, nextPow2 ((int) std::ceil (0.5 * sr)));
        cap.assign ((size_t) capN, 0.0f); capMask = capN - 1;
        const int Ne = (int) ms2s (kTargetEarlyMs - kTargetTransMs) + 4, Nt = (int) ms2s (kTargetTransMs) + 4;
        nE = nextPow2 (2 * Ne); nT = nextPow2 (4 * Nt);
        fftE.init (nE); fftT.init (nT);
        const int big = std::max (nE, nT);
        xe.assign ((size_t) Ne, 0.0); xr.assign ((size_t) Ne, 0.0); win.assign ((size_t) std::max (Ne, Nt), 0.0);
        re_.assign ((size_t) big, 0.0); im_.assign ((size_t) big, 0.0);
        PeS.assign ((size_t) nE / 2 + 1, 0.0);
        for (int k = 0; k < kTSlots; ++k) PrS[k].assign ((size_t) nE / 2 + 1, 0.0);
        for (int f = 0; f < kFlav; ++f) { PeH[f].assign ((size_t) nE / 2 + 1, 0.0); PtH[f].assign ((size_t) nT / 2 + 1, 0.0); DtS[f].assign ((size_t) nT / 2 + 1, 0.0); }
        scrSm.assign ((size_t) big / 2 + 2, 0.0); scrPre.assign ((size_t) big / 2 + 3, 0.0);
        acY.assign ((size_t) ms2s (kTargetLateMs) + 8, 0.0);
        const int slowLen = std::max (1, (int) ms2s (kColSlowDelayMs) / kDec);
        slowA.assign ((size_t) slowLen, 0.0); slowH.assign ((size_t) slowLen, 0.0);
        cFA = 1.0 - std::exp (-1000.0 * kDec / (sr * (double) kColFastMsA)); cFH = 1.0 - std::exp (-1000.0 * kDec / (sr * (double) kColFastMsH)); cSl = 1.0 - std::exp (-1000.0 * kDec / (sr * (double) kTargetSlowMs));
        ratioC = std::pow (10.0, (double) kColSensDb / 10.0); sqrtRatioC = std::sqrt (ratioC); gapSamplesC = ms2s (kColGapMs);
        for (int k = 0; k < kTSlots; ++k) targetConfigureSlot (k);
    }

    void targetSelect (int& voice, int& part, bool& bus) const
    {
        voice = -1; part = 0; bus = false;
        const InstDef& I = kInst[std::clamp (par.targetInst, 0, kTargetInsts - 1)];
        if (I.n <= 0) return;
        const SlotDef& sl = I.slot[partIndex (par.targetInst, par.targetPart)];
        voice = sl.voice; part = sl.part; bus = I.bus;
    }

    // the engine was reset (host bypass, a new run): the detector and the clocks start over, the memory stays
    void targetReset (bool wasRunning)
    {
        for (auto& b : tbs) b = TBellState(); pplan = PlateauPlan(); edge[0] = EdgeStage(); edge[1] = EdgeStage(); edge[0].below = true; edge[1].below = false; revOn = false;
        std::fill (cap.begin(), cap.end(), 0.0f);
        tSamp = 0; tgtRun = false; curSlot = -1;
        accA = accH = fA = sA = fH = sH = 0.0; std::fill (slowA.begin(), slowA.end(), 0.0); std::fill (slowH.begin(), slowH.end(), 0.0); slowPos = 0; decPrev = 0.0; decCnt = 0;
        armedC = true; gapLeftC = 0; warmLeftC = wasRunning ? ms2s (kTargetWarmMs) : 0;   // a stream that has not started yet has nothing to settle: a hit at sample 0 counts
        qHead = 0; qCount = 0; ev.active = false;
        for (TSlot& s : ts) { s.nOnsets = 0; s.nLearned = 0; s.nRelearn = 0; std::fill (s.rejectWhy, s.rejectWhy + 4, 0); s.onsetAbs = -1; }
    }

    // tunes one slot for the sample rate (prepare); the memory is not touched
    void targetConfigureSlot (int k)
    {
        TSlot& s = ts[k]; s.voice = kSlotVoice[k]; s.bus = kSlotBus[k];
        const VoiceDef& V = kVoice[s.voice]; const double nyq = 0.45 * sr;
        s.hasRej = s.bus && V.rejHi > 0.0f;
        if (s.hasRej)
        {
            s.dHp.design (true, std::min ((double) V.busLo, nyq), sr); s.dLp.design (false, std::min ((double) V.busHi, nyq), sr);
            s.rHp.design (true, (double) V.rejLo, sr); s.rLp.design (false, (double) V.rejHi, sr);
        }
        s.cF = 1.0 - std::exp (-1000.0 * kDetStep / (sr * (double) V.fastMs));
        s.onsetAbs = -1;
    }

    inline float capAt (int64_t i) const { return i < 0 ? 0.0f : cap[(size_t) (i & capMask)]; }
    /** Does the low band (a 2nd-order low-pass at 150 Hz) rise at this onset? Its mean square 1 .. 8 ms after the onset against 25 .. 3 ms before: more than twice.
        A hi-hat or a snare's crack over a kick's tail does not; a new kick or bass note does. Reads the capture ring only (about 2,000 samples of a biquad). */
    bool lowBandRises (int64_t t) const
    {
        const double w0 = 2.0 * M_PI * 150.0 / sr, al = std::sin (w0) / (2.0 * 0.7071), cw = std::cos (w0), a0 = 1.0 + al;
        const double b0 = (1.0 - cw) * 0.5 / a0, b1 = (1.0 - cw) / a0, b2 = b0, a1 = -2.0 * cw / a0, a2 = (1.0 - al) / a0;
        double z1 = 0.0, z2 = 0.0, pre = 0.0, post = 0.0; int np = 0, nq = 0;
        const int64_t s0 = t - ms2s (40.0f), p0 = t - ms2s (25.0f), p1 = t - ms2s (3.0f), q0 = t + ms2s (1.0f), q1 = t + ms2s (8.0f);
        for (int64_t i = s0; i < q1; ++i)
        {
            const double x = (double) capAt (i), y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y;
            if (i >= p0 && i < p1) { pre += y * y; ++np; } else if (i >= q0) { post += y * y; ++nq; }
        }
        pre /= std::max (np, 1); post /= std::max (nq, 1);
        return post > 2.0 * pre + 1e-12;
    }

    // SPEC 8d 2: always listening, whatever TARGET is set to. Reads the signal only; nothing is written to it. The idle work: the mono sum into the ring, and per group of kDec
    // samples the power of the sum and of its first difference (the detector's decision is detStep, once per group).
    void targetDetect (float* const* main, int numCh, int pos, int m)
    {
        static_assert (kDec == 8, "the group loop below is written for 8 samples");
        const float* l = main[0] + pos; const float* r = numCh > 1 ? main[1] + pos : l;
        const int64_t t0 = tSamp;
        for (int i = 0; i < m;)
        {
            const size_t w = (size_t) ((t0 + i) & capMask); const int seg = (int) std::min<int64_t> ((int64_t) (m - i), capMask + 1 - (int64_t) w);   // the part that does not wrap
            float* dst = cap.data() + w;
            if (numCh > 1) for (int k = 0; k < seg; ++k) dst[k] = 0.5f * (l[i + k] + r[i + k]);
            else std::memcpy (dst, l + i, sizeof (float) * (size_t) seg);
            int k = 0;
            while (k < seg)
            {
                if (decCnt == 0 && seg - k >= kDec)   // a whole group
                {
                    const float* p = dst + k; double sq[kDec], df[kDec]; double prev = decPrev;
                    for (int j = 0; j < kDec; ++j) { const double v = (double) p[j]; sq[j] = v * v; df[j] = v - (j ? (double) p[j - 1] : prev); }
                    decPrev = (double) p[kDec - 1];
                    accA = ((sq[0] + sq[1]) + (sq[2] + sq[3])) + ((sq[4] + sq[5]) + (sq[6] + sq[7]));
                    accH = ((df[0] * df[0] + df[1] * df[1]) + (df[2] * df[2] + df[3] * df[3])) + ((df[4] * df[4] + df[5] * df[5]) + (df[6] * df[6] + df[7] * df[7]));
                    detStep (t0 + i + k + kDec - 1); k += kDec;
                }
                else
                {
                    const double v = (double) dst[k], d = v - decPrev; decPrev = v; accA += v * v; accH += d * d;
                    if (++decCnt == kDec) { decCnt = 0; detStep (t0 + i + k); }
                    ++k;
                }
            }
            i += seg;
        }
        tSamp = t0 + m;
        for (int q = 0; q < qCount; ++q) { PendHit& h = queue[(qHead + q) % kQueue]; if (! h.settled && tSamp >= h.settleAt) settleHit (h); }
    }

    // once per kDec samples: the onset decision on the mono sum. Onset: a fast follower of the power of the sum (or of its first difference, the high-frequency channel) over the
    // slow level 10 ms ago by kColSensDb, above the floor, not within kColGapMs of the last one; re-armed when both are back under the slow level.
    void detStep (int64_t t)
    {
        constexpr double inv = 1.0 / (double) kDec;
        const double pA = accA * inv, pH = accH * inv; accA = accH = 0.0;
        const double fAprev = fA, fHprev = fH;
        fA += (pA - fA) * cFA; fH += (pH - fH) * cFH; sA += (pA - sA) * cSl; sH += (pH - sH) * cSl;
        const double oA = slowA[(size_t) slowPos], oH = slowH[(size_t) slowPos]; slowA[(size_t) slowPos] = sA; slowH[(size_t) slowPos] = sH; if (++slowPos >= (int) slowA.size()) slowPos = 0;
        if (warmLeftC > 0) { warmLeftC -= kDec; return; }
        if (gapLeftC > 0) gapLeftC -= kDec;
        if (armedC)
        {
            const double thrA = std::max ((double) kTargetFloor, oA * ratioC), thrH = std::max ((double) kTargetFloor, oH * ratioC);
            const bool a = fA > thrA, h = fH > thrH;
            if (gapLeftC <= 0 && (a || h))
            {
                // the sample where the follower crossed its threshold, interpolated inside the group (the windows hang off this sample)
                const double f = a ? (fAprev >= thrA ? 0.0 : std::clamp ((thrA - fAprev) / std::max (fA - fAprev, 1e-30), 0.0, 1.0))
                                   : (fHprev >= thrH ? 0.0 : std::clamp ((thrH - fHprev) / std::max (fH - fHprev, 1e-30), 0.0, 1.0));
                onsetEvent (t - (kDec - 1) + (int64_t) std::ceil (f * (double) (kDec - 1)));
            }
        }
        else if (fA < sA * sqrtRatioC && fH < sH * sqrtRatioC) armedC = true;
    }

    void onsetEvent (int64_t tc)
    {
        gapLeftC = gapSamplesC; armedC = false;
        if (qCount == kQueue) { qHead = (qHead + 1) % kQueue; --qCount; for (TSlot& s : ts) ++s.rejectWhy[0]; }   // the analysis fell behind: the oldest hit is lost
        PendHit& h = queue[(qHead + qCount) % kQueue]; h = PendHit(); h.t = tc; h.o = tc; h.settleAt = tc + ms2s (kColSettleMs); ++qCount;
        for (int k = 0; k < kTSlots; ++k) if (! ts[k].hasRej) { h.mask |= (uint8_t) (1 << k); ts[k].onsetAbs = tc; ++ts[k].nOnsets; }   // the slots that hear everything: the clock starts at once
    }

    // kColSettleMs after the onset: where the hit really starts (the first sample whose first difference reaches 1/4 of the largest one around the onset), and, for the two DRUMS-bus
    // slots, whether the hit is theirs
    void settleHit (PendHit& h)
    {
        const int64_t a = h.t - ms2s (12.0f), b = h.t + ms2s (6.0f); double dm = 0.0;
        for (int64_t i = a; i <= b; ++i) dm = std::max (dm, (double) std::abs (capAt (i) - capAt (i - 1)));
        int64_t first = h.t;
        if (dm > 1e-6) for (int64_t i = a; i <= b; ++i) if ((double) std::abs (capAt (i) - capAt (i - 1)) >= 0.25 * dm) { first = i; break; }
        h.o = first - ms2s (kColPreRollMs);
        for (int k = 0; k < kTSlots; ++k)
            if (ts[k].hasRej)
                if (const int c = classifyBus (ts[k], h.t)) { h.mask |= (uint8_t) (1 << k); if (c == 2) h.coin |= (uint8_t) (1 << k); ts[k].onsetAbs = h.t; ++ts[k].nOnsets; }
        h.settled = true;
    }

    // DRUMS bus, once per hit: the slot's own band must have risen and the reject band (the other instrument's) must not be louder than half of it 6 ms after the onset
    // (a kick's boom is not a snare, a snare's crack is not a kick). The two bands' power followers are run over the ring, 30 ms before to 6 ms after the onset.
    /** 0 = not this slot's hit, 1 = its hit, 2 = a snare struck together with a kick: taken now, confirmed later by coincidentIsOwn (the snare band must still ring 15 - 35 ms on). */
    int classifyBus (TSlot& s, int64_t t)
    {
        s.dHp.clear(); s.dLp.clear(); s.rHp.clear(); s.rLp.clear();
        double dF = 0.0, dR = 0.0, pre = 0.0, preR = 0.0, accD = 0.0, accR = 0.0; int g = 0;
        const int64_t a = t - ms2s (30.0f), b = t + ms2s (6.0f), preAt = t - ms2s (2.0f);
        for (int64_t i = a; i < b; ++i)
        {
            const double x = (double) capAt (i), yd = s.dLp.run (s.dHp.run (x)), yr = s.rLp.run (s.rHp.run (x)); accD += yd * yd; accR += yr * yr;
            if (++g == kDetStep)
            {
                g = 0; dF += (accD / kDetStep - dF) * s.cF; dR += (accR / kDetStep - dR) * s.cF; accD = accR = 0.0;
                if (i <= preAt) { pre = dF; preR = dR; }
            }
        }
        // 11 Oct: the reject band blocks the hit only when it ROSE with it (the other instrument struck at this moment). A sustained level there (a bass or an
        // 808 tail under the snare) is not a hit, but it used to drop every snare whose band was quieter than the bass under it.
        const bool otherStruck = dR > 0.5 * dF && dR > 2.0 * preR;
        if (dF <= (double) kTargetFloor) return 0;
        if (! otherStruck) return dF > 4.0 * pre ? 1 : 0;
        // 11 Oct, beat.wav (four on the floor: every snare lands ON a kick, DRUMS - SNARE BODY learned nothing): with the kick struck at the same moment the snare band rose only
        // 2.0 - 3.5x (ringing hats under it), a kick alone 1.2x. Such a hit is the snare's when its band rose 2x AND it still rings 15 - 35 ms later (a snare's noise lasts,
        // a kick's click is gone in 5 ms); the second half is read when the hit is analysed (coincidentIsOwn), when the ring holds it.
        return s.voice == kVoSnare && dF > 2.0 * pre ? 2 : 0;
    }
    double busBandPow (TSlot& s, int64_t a, int64_t b)   // mean power of the slot's own band (its detector band-pass) over [a, b), the filters warmed 10 ms before a
    {
        s.dHp.clear(); s.dLp.clear(); double acc = 0.0; int64_t n = 0;
        for (int64_t i = a - ms2s (10.0f); i < b; ++i) { const double y = s.dLp.run (s.dHp.run ((double) capAt (i))); if (i >= a) { acc += y * y; ++n; } }
        return acc / (double) std::max<int64_t> (n, 1);
    }
    // the snare band 15 - 35 ms after the onset must keep >= 10 % of its 0 - 6 ms power and be > 2x its level before the hit. Measured 11 Oct: a real snare on a kick (beat.wav)
    // keeps 47 - 64 %, a kick alone with a 3 kHz click (measure.py T5) 0.14 - 0.26 % (its click is gone; a "2x the level before" test alone let those kicks in)
    bool coincidentIsOwn (TSlot& s, int64_t t)
    {
        const double late = busBandPow (s, t + ms2s (15.0f), t + ms2s (35.0f)), pre = busBandPow (s, t - ms2s (14.0f), t - ms2s (2.0f)), peak = busBandPow (s, t, t + ms2s (6.0f));
        return late > 0.1 * peak && late > 2.0 * pre;
    }

    void finishSlot (TSlot& s, const HitRec& h, bool ok, int why)
    {
        if (! ok)
        {
            if (why == 2 || why == 3) verifyHit (s, false);   // "not this instrument" counts against a memory that has not been confirmed yet
            return;
        }
        verifyHit (s, s.keyMed > 0.0f && h.key > 0.0f && std::abs (std::log2 (h.key / s.keyMed)) <= kTargetNewSampleOct);
        memAdd (s, h); ++s.nLearned;
    }

    // SPEC 8d 5, second case: after a state load the first hits are compared with the stored memory. Two that disagree (a duplicated or pasted
    // instance on another channel, a project opened with a different sound on the track) drop it; two that agree confirm it.
    void verifyHit (TSlot& s, bool agree)
    {
        if (s.verifyLeft <= 0) return;
        if (s.nRec == 0) { s.verifyLeft = 0; return; }
        --s.verifyLeft; if (agree) ++s.verifyOk; else ++s.verifyBad;
        if (s.verifyBad >= 2) { memClear (s); ++s.nRelearn; s.verifyLeft = 0; }
        else if (s.verifyOk >= 2) s.verifyLeft = 0;
    }

    void memClear (TSlot& s) { s.nRec = 0; s.devCount = 0; s.keyMed = 0.0f; s.farKey = 0.0f; s.quick = false; s.L = TargetLearned(); ++memVersion; }

    // ---- per-hit analysis
    // ---- the resumable pieces of the per-hit analysis. Each *Step does what the credit allows, keeps its place in ev.w and returns true when it is done; the arithmetic and its order are
    // those of the plain loops they were cut from (the pieces are bit-identical to them). kCost*: tenths of ns per element on the dev Mac (DYNEQ_PROFILE, dyneqraw), not a promise.
#ifdef DYNEQ_EV_TICK_UNITS
    static constexpr int64_t kEvTickUnits = DYNEQ_EV_TICK_UNITS;   // measurement only
#else
    static constexpr int64_t kEvTickUnits = 180000;   // work per law tick (32 samples): about 18 us on the dev Mac, a quarter of the 10 % a 32-sample host block can give
#endif
    static constexpr int64_t kCostStep = 2000, kCostRecord = 45000, kCostCopy = 5, kCostWin = 17, kCostRev = 10, kCostBf = 15, kCostPsdOut = 3, kCostBq2 = 16, kCostBq1 = 44, kCostHp = 65, kCostDiff = 30, kCostRs = 42, kCostAc = 7, kCostFit = 65;
    int64_t credit = 0, evLatencyMax = 0, evLatencyLast = 0, evRecords = 0;
#ifdef DYNEQ_PROFILE
    // DYNEQ_PROFILE: time and charged units per piece of work (tag), to calibrate the kCost* constants against ns
    struct ProfScope
    {
        Engine& e; int tag; std::chrono::steady_clock::time_point t0; int64_t c0;
        ProfScope (Engine& en, int tg) : e (en), tag (tg), t0 (std::chrono::steady_clock::now()), c0 (en.credit) {}
        ~ProfScope() { e.pPrimUs[tag] += std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count(); e.pPrimUnits[tag] += (double) (c0 - e.credit); }
    };
public:
    double pPrimUs[16] {}, pPrimUnits[16] {};
private:
#define DYNEQ_PRIM(tag) ProfScope profScope_ (*this, tag)
#else
#define DYNEQ_PRIM(tag)
#endif
    int64_t budgetCount (int64_t cost) const { return std::max<int64_t> (1, credit / cost); }
    void nextStage (int s) { ev.stage = s; ev.cursor = 0; ev.sub = 0; ev.busy = false; ev.w = EvJob::Work(); }
    double acR[1024] {};

    bool copyWindow (int64_t start, int N, double* dst)
    {
        DYNEQ_PRIM(0);
        EvJob::Work& W = ev.w; const int cnt = (int) std::min<int64_t> (N - W.i, budgetCount (kCostCopy));
        for (int n = (int) W.i; n < (int) W.i + cnt; ++n) dst[n] = (double) capAt (start + n);
        W.i += cnt; credit -= (int64_t) cnt * kCostCopy; return W.i >= N;
    }

    // F0 of the late window [a, b): low-pass (4th order Butterworth at 2.5 x the top of the search range), decimate, normalised autocorrelation, smallest lag within 0.85 of the best peak.
    // Phases: 0 set-up, 1 warm-up, 2 decimate, 3 energy, 4 autocorrelation (per lag), 5 choose the lag, 6 the share of the window's energy that is a sinusoid at f0, 7 done
    bool f0Step (int64_t a, int64_t b, const VoiceDef& V, float& result)
    {
        DYNEQ_PRIM(1);
        EvJob::Work& W = ev.w;
        for (;;)
        {
            if (credit <= 0 && W.ph != 0) return false;
            switch (W.ph)
            {
            case 0:
            {
                const double fLP = std::min ((double) V.f0Hi * 2.5, 0.4 * sr);
                W.q1.design (false, fLP, sr, kButterQ[1]); W.q2.design (false, fLP, sr, kButterQ[2]);
                W.D = std::max (1, (int) std::floor (sr / (8.0 * fLP))); W.fs = sr / (double) W.D;
                W.n = (int) std::min ((int64_t) acY.size() - 2, (b - a) / W.D);
                W.i = a - ms2s (20.0f); W.ph = 1; credit -= kCostStep; break;
            }
            case 1:   // warm-up
            {
                const int64_t cnt = std::min<int64_t> (a - W.i, budgetCount (kCostBq2));
                for (int64_t n = 0; n < cnt; ++n) W.q2.run (W.q1.run ((double) capAt (W.i + n)));
                W.i += cnt; credit -= cnt * kCostBq2; if (W.i >= a) { W.ph = 2; W.j = 0; } break;
            }
            case 2:   // decimate: the last of every D samples
            {
                const int64_t cnt = std::min<int64_t> (W.n - W.j, std::max<int64_t> (1, credit / (kCostBq2 * W.D)));
                for (int64_t jj = W.j; jj < W.j + cnt; ++jj)
                {
                    double v = 0.0;
                    for (int k = 0; k < W.D; ++k) v = W.q2.run (W.q1.run ((double) capAt (a + jj * W.D + k)));
                    acY[(size_t) jj] = v;
                }
                W.j += cnt; credit -= cnt * kCostBq2 * W.D; if (W.j >= W.n) { W.ph = 3; W.j = 0; W.e0 = 0.0; W.lmin = std::max (2, (int) std::floor (W.fs / (double) V.f0Hi)); W.lmax = (int) std::ceil (W.fs / (double) V.f0Lo); if (W.n < 3 * W.lmax) { result = 0.0f; return true; } }
                break;
            }
            case 3:   // the energy
            {
                const int cnt = (int) std::min<int64_t> (W.n - W.j, budgetCount (kCostAc));
                for (int jj = (int) W.j; jj < (int) W.j + cnt; ++jj) W.e0 += acY[(size_t) jj] * acY[(size_t) jj];
                W.j += cnt; credit -= (int64_t) cnt * kCostAc;
                if (W.j >= W.n)
                {
                    if (W.e0 < 1e-12 * W.n || W.lmax + 2 >= 1024) { result = 0.0f; return true; }
                    W.ph = 4; W.l = W.lmin - 1; W.j = 0; W.num = W.d1 = W.d2 = 0.0;
                }
                break;
            }
            case 4:   // r[l] for every lag, a lag's sums in pieces
            {
                const int lim = W.n - W.l; const int cnt = (int) std::min<int64_t> (lim - W.j, budgetCount (3 * kCostAc));
                for (int jj = (int) W.j; jj < (int) W.j + cnt; ++jj)
                { W.num += acY[(size_t) jj] * acY[(size_t) (jj + W.l)]; W.d1 += acY[(size_t) jj] * acY[(size_t) jj]; W.d2 += acY[(size_t) (jj + W.l)] * acY[(size_t) (jj + W.l)]; }
                W.j += cnt; credit -= (int64_t) cnt * 3 * kCostAc;
                if (W.j >= lim) { acR[W.l] = W.num / std::sqrt (W.d1 * W.d2 + 1e-30); ++W.l; W.j = 0; W.num = W.d1 = W.d2 = 0.0; if (W.l > W.lmax + 1) W.ph = 5; }
                break;
            }
            case 5:   // the lag
            {
                const double* r = acR; double best = 0.0; credit -= kCostStep;
                for (int l = W.lmin; l <= W.lmax; ++l) if (r[l] > r[l - 1] && r[l] >= r[l + 1]) best = std::max (best, r[l]);
                if (best < 0.4) { result = 0.0f; return true; }
                bool found = false;
                for (int l = W.lmin; l <= W.lmax && ! found; ++l)
                    if (r[l] > r[l - 1] && r[l] >= r[l + 1] && r[l] >= 0.85 * best)
                    {
                        const double den = r[l - 1] - 2.0 * r[l] + r[l + 1], dl = std::abs (den) < 1e-12 ? 0.0 : 0.5 * (r[l - 1] - r[l + 1]) / den;
                        W.f0 = W.fs / ((double) l + dl); found = true;
                    }
                if (! found) { result = 0.0f; return true; }
                W.ph = 6; W.j = 0; W.cr = W.ci = 0.0; W.w = 2.0 * M_PI * W.f0 / W.fs; break;
            }
            default:  // the share of the late window's energy that is a sinusoid at f0: a 200 Hz ring correlates with itself at 15 and 20 ms too (66 and 50 Hz), but holds no energy there
            {
                const int cnt = (int) std::min<int64_t> (W.n - W.j, budgetCount (kCostFit));
                for (int jj = (int) W.j; jj < (int) W.j + cnt; ++jj) { W.cr += acY[(size_t) jj] * std::cos (W.w * jj); W.ci += acY[(size_t) jj] * std::sin (W.w * jj); }
                W.j += cnt; credit -= (int64_t) cnt * kCostFit;
                if (W.j >= W.n) { result = (2.0 / W.n) * (W.cr * W.cr + W.ci * W.ci) < kTargetF0MinShare * W.e0 ? 0.0f : (float) W.f0; return true; }
                break;
            }
            }
        }
    }

    // ms after the event clock's onset tc when the energy above guardHz (the hit starting at o) is 24 dB under its peak (+1 ms safety); 1.5 .. 15 ms
    bool transEndStep (int64_t o, int64_t tc, float guardHz, float& result)
    {
        DYNEQ_PRIM(2);
        EvJob::Work& W = ev.w; const int64_t hop = std::max<int64_t> (1, ms2s (0.5f));
        if (W.ph == 0) { W.q1.design (true, std::min ((double) guardHz, 0.4 * sr), sr); W.i = o - ms2s (3.0f); W.ph = 1; credit -= kCostStep; }
        if (W.ph == 1)
        {
            const int64_t cnt = std::min<int64_t> (o - W.i, budgetCount (kCostBq1));
            for (int64_t n = 0; n < cnt; ++n) W.q1.run ((double) capAt (W.i + n));
            W.i += cnt; credit -= cnt * kCostBq1; if (W.i < o) return false;
            W.ph = 2; W.j = 0; if (credit <= 0) return false;
        }
        while (W.j < 60 && credit > 0)
        {
            double s = 0.0; for (int64_t k = 0; k < hop; ++k) { const double y = W.q1.run ((double) capAt (o + W.j * hop + k)); s += y * y; }
            W.e[W.j++] = s; credit -= hop * kCostBq1;
        }
        if (W.j < 60) return false;
        const double* e = W.e; const int nh = 60;
        int kp = 0; for (int k = 0; k < 16; ++k) if (e[k] > e[kp]) kp = k;
        if (e[kp] < 1e-9) { result = 5.0f; return true; }
        int kEnd = -1; for (int k = kp + 1; k < nh; ++k) if (e[k] < e[kp] * 0.004) { kEnd = k; break; }
        const float t = kEnd < 0 ? 15.0f : (float) kEnd * 0.5f + 1.0f + (float) ((double) (o - tc) * 1000.0 / sr);
        result = std::clamp (t, 1.5f, 15.0f); return true;
    }

    // damped sinusoid at f0 fitted to x (least squares over a few decay rates) and subtracted: the sub's lobe would otherwise bury the body above it.
    // Phases: 0 the energy of x, 1 one decay rate's sums (t = 0..5, in pieces), 2 the subtraction
    bool removeSineStep (const double* x, double* out, int N, double f0)
    {
        DYNEQ_PRIM(3);
        static const double tau[6] = { 0.03, 0.06, 0.12, 0.25, 0.5, 1e9 };
        EvJob::Work& W = ev.w;
        if (W.ph == 0)
        {
            if (W.i == 0) { W.w = 2.0 * M_PI * f0 / sr; W.cw = std::cos (W.w); W.sw = std::sin (W.w); W.bestRes = 1e300; W.ba = 0; W.bb = 0; W.br = 1.0; W.sxx = 0.0; credit -= kCostStep; }
            const int cnt = (int) std::min<int64_t> (N - W.i, budgetCount (kCostAc));
            for (int n = (int) W.i; n < (int) W.i + cnt; ++n) W.sxx += x[n] * x[n];
            W.i += cnt; credit -= (int64_t) cnt * kCostAc;
            if (W.i < N) return false;
            W.ph = 1; W.t = 0; W.i = -1;
        }
        while (W.ph == 1)
        {
            if (credit <= 0) return false;
            if (W.i < 0) { W.rr = std::exp (-1.0 / (tau[W.t] * sr)); W.u = 1.0; W.v = 0.0; W.suu = W.suv = W.svv = W.sxu = W.sxv = 0.0; W.i = 0; }
            const int cnt = (int) std::min<int64_t> (N - W.i, budgetCount (kCostRs));
            for (int n = (int) W.i; n < (int) W.i + cnt; ++n)
            {
                W.suu += W.u * W.u; W.suv += W.u * W.v; W.svv += W.v * W.v; W.sxu += x[n] * W.u; W.sxv += x[n] * W.v;
                const double un = W.rr * (W.u * W.cw - W.v * W.sw); W.v = W.rr * (W.u * W.sw + W.v * W.cw); W.u = un;
            }
            W.i += cnt; credit -= (int64_t) cnt * kCostRs;
            if (W.i < N) return false;
            const double det = W.suu * W.svv - W.suv * W.suv;
            if (det >= 1e-18)
            {
                const double a = (W.sxu * W.svv - W.sxv * W.suv) / det, b = (W.suu * W.sxv - W.suv * W.sxu) / det, res = W.sxx - a * W.sxu - b * W.sxv;
                if (res < W.bestRes) { W.bestRes = res; W.ba = a; W.bb = b; W.br = W.rr; }
            }
            W.i = -1; if (++W.t == 6) { W.ph = 2; W.i = 0; W.u = 1.0; W.v = 0.0; }
        }
        const int cnt = (int) std::min<int64_t> (N - W.i, budgetCount (kCostRs));
        for (int n = (int) W.i; n < (int) W.i + cnt; ++n)
        {
            out[n] = x[n] - (W.ba * W.u + W.bb * W.v); const double un = W.br * (W.u * W.cw - W.v * W.sw); W.v = W.br * (W.u * W.sw + W.v * W.cw); W.u = un;
        }
        W.i += cnt; credit -= (int64_t) cnt * kCostRs;
        return W.i >= N;
    }

    // window with a cosine taper at the ends (taperA at the start, taperB at the end, in samples), power spectrum per sample (PSD) of x into P (nfft/2 + 1 bins).
    // Phases: 0 window + zero padding, 1 bit reversal, 2 butterflies (stage len, group i, offset j), 3 the power
    bool psdStep (const double* x, int N, int taperA, int taperB, const Fft& f, int nfft, std::vector<double>& P)
    {
        EvJob::Work& W = ev.w;
        if (W.ph == 0)
        {
            DYNEQ_PRIM(4);
            const int cnt = (int) std::min<int64_t> (nfft - W.i, budgetCount (kCostWin));
            for (int n = (int) W.i; n < (int) W.i + cnt; ++n)
            {
                if (n < N)
                {
                    double wv = 1.0;
                    if (taperA > 0 && n < taperA) wv = 0.5 * (1.0 - std::cos (M_PI * (n + 0.5) / taperA));
                    if (taperB > 0 && n >= N - taperB) wv *= 0.5 * (1.0 - std::cos (M_PI * (N - n - 0.5) / taperB));
                    win[(size_t) n] = wv; W.sw2 += wv * wv; re_[(size_t) n] = x[n] * wv; im_[(size_t) n] = 0.0;
                }
                else { re_[(size_t) n] = 0.0; im_[(size_t) n] = 0.0; }
            }
            W.i += cnt; credit -= (int64_t) cnt * kCostWin;
            if (W.i < nfft) return false;
            W.ph = 1; W.i = 0; if (credit <= 0) return false;
        }
        if (W.ph == 1)
        {
            DYNEQ_PRIM(5);
            const int cnt = (int) std::min<int64_t> (f.n - W.i, budgetCount (kCostRev));
            for (int i = (int) W.i; i < (int) W.i + cnt; ++i) { const int j = f.rev[(size_t) i]; if (j > i) { std::swap (re_[(size_t) i], re_[(size_t) j]); std::swap (im_[(size_t) i], im_[(size_t) j]); } }
            W.i += cnt; credit -= (int64_t) cnt * kCostRev;
            if (W.i < f.n) return false;
            W.ph = 2; W.len = 2; W.fi = 0; W.fj = 0; if (credit <= 0) return false;
        }
        if (W.ph == 2)
        {
            DYNEQ_PRIM(6);
            int64_t left = budgetCount (kCostBf), used = 0; double* re = re_.data(); double* im = im_.data();
            while (left > 0 && W.len <= f.n)
            {
                const int half = W.len >> 1, step = f.n / W.len;
                const int run = (int) std::min<int64_t> (left, half - W.fj);
                for (int j = W.fj; j < W.fj + run; ++j)
                {
                    const double wr = f.cs[(size_t) (j * step)], wi = f.sn[(size_t) (j * step)]; const int a = W.fi + j, b = a + half;
                    const double tr = re[b] * wr - im[b] * wi, ti = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - tr; im[b] = im[a] - ti; re[a] += tr; im[a] += ti;
                }
                left -= run; used += run; W.fj += run;
                if (W.fj >= half) { W.fj = 0; W.fi += W.len; if (W.fi >= f.n) { W.fi = 0; W.len <<= 1; } }
            }
            credit -= used * kCostBf;
            if (W.len <= f.n) return false;
            W.ph = 3; W.i = 0; if (credit <= 0) return false;
        }
        DYNEQ_PRIM(7);
        const int total = nfft / 2 + 1; const int cnt = (int) std::min<int64_t> (total - W.i, budgetCount (kCostPsdOut));
        for (int k = (int) W.i; k < (int) W.i + cnt; ++k) P[(size_t) k] = (re_[(size_t) k] * re_[(size_t) k] + im_[(size_t) k] * im_[(size_t) k]) / std::max (W.sw2, 1e-12);
        W.i += cnt; credit -= (int64_t) cnt * kCostPsdOut;
        return W.i >= total;
    }

    // the part's band inside [lo, hi] of the spectrum P: 1/6-octave smoothing, peak, the -6 dB (power /4) extent around it, log-frequency centroid inside the extent
    bool findBand (const std::vector<double>& P, double binHz, float lo, float hi, float& c, float& l, float& h)
    {
        const int k0 = (int) std::ceil ((double) lo / binHz), k1 = (int) std::floor ((double) hi / binHz);
        if (k1 - k0 < 3) return false;
        const int cnt = k1 - k0 + 1;
        scrPre[0] = 0.0; for (int i = 0; i < cnt; ++i) scrPre[(size_t) i + 1] = scrPre[(size_t) i] + P[(size_t) (k0 + i)];
        int kp = k0; double pk = -1.0;
        for (int k = k0; k <= k1; ++k)
        {
            const double f = (double) k * binHz;
            int ka = std::max (k0, (int) std::floor (f * 0.943874 / binHz)), kb = std::min (k1, (int) std::ceil (f * 1.059463 / binHz));
            if (kb - ka < 2) { ka = std::max (k0, k - 1); kb = std::min (k1, k + 1); }
            const double v = (scrPre[(size_t) (kb - k0) + 1] - scrPre[(size_t) (ka - k0)]) / (double) (kb - ka + 1);
            scrSm[(size_t) (k - k0)] = v; if (v > pk) { pk = v; kp = k; }
        }
        if (pk < (double) kTargetValidPsd) return false;
        int ka = kp, kb = kp;
        while (ka > k0 && scrSm[(size_t) (ka - 1 - k0)] >= 0.25 * pk) --ka;
        while (kb < k1 && scrSm[(size_t) (kb + 1 - k0)] >= 0.25 * pk) ++kb;
        double ws = 0.0, ls = 0.0;   // linear-frequency centroid: the window's smear is the same number of Hz on both sides, so a log-frequency centroid would lean low
        for (int k = ka; k <= kb; ++k) { const double wv = scrSm[(size_t) (k - k0)]; ws += wv; ls += wv * (double) k * binHz; }
        double cc = ls / std::max (ws, 1e-30), ll = ka == k0 ? (double) lo : ((double) ka - 0.5) * binHz, hh = kb == k1 ? (double) hi : ((double) kb + 0.5) * binHz;
        if (std::log2 (hh / ll) < 0.4) { ll = cc / std::pow (2.0, 0.2); hh = cc * std::pow (2.0, 0.2); }
        c = (float) std::clamp (cc, ll, hh); l = (float) ll; h = (float) hh;
        return true;
    }

    // transient minus early for one CLICK / CRACK / SNAP / CHICK part. Both windows are high-passed (4th order, half the part's lower edge) first,
    // so the boom's leakage through the window edges cannot sit under the click band; the difference is taken per bin of the transient grid
    bool hpWindowStep (int64_t start, int N, double fcHz, double* out)
    {
        DYNEQ_PRIM(8);
        EvJob::Work& W = ev.w;
        if (W.ph == 0)
        {
            W.q1.design (true, std::min (fcHz, 0.4 * sr), sr, kButterQ[1]); W.q2.design (true, std::min (fcHz, 0.4 * sr), sr, kButterQ[2]);
            W.i = start - ms2s (3.0f); W.ph = 1; credit -= kCostStep;
        }
        if (W.ph == 1)
        {
            const int64_t cnt = std::min<int64_t> (start - W.i, budgetCount (kCostHp));
            for (int64_t n = 0; n < cnt; ++n) W.q2.run (W.q1.run ((double) capAt (W.i + n)));
            W.i += cnt; credit -= cnt * kCostHp; if (W.i < start) return false;
            W.ph = 2; W.i = 0; if (credit <= 0) return false;
        }
        const int cnt = (int) std::min<int64_t> (N - W.i, budgetCount (kCostHp));
        for (int n = (int) W.i; n < (int) W.i + cnt; ++n) out[n] = W.q2.run (W.q1.run ((double) capAt (start + n)));
        W.i += cnt; credit -= (int64_t) cnt * kCostHp;
        return W.i >= N;
    }

    // one record's last step for one slot: the slot's own reading of the shared spectra (SPEC 8e.3). The rules are the ones the per-slot analysis always had.
    bool slotRecord (int k, HitRec& h, int& why)
    {
        TSlot& s = ts[k]; const VoiceDef& V = kVoice[s.voice]; const EvJob& E = ev; why = -1;
        const int64_t endAbs = E.endAbs[k], lE = std::min (E.o + ms2s (kTargetLateMs), endAbs);
        const bool hasF0 = V.f0Hi > 0.0f;
        if (! E.peOk || endAbs < E.eE) { ++s.rejectWhy[0]; why = 0; return false; }   // the next onset came before the early window was over (or the hit is out of the ring)
        // the fundamental decides "is this the instrument" for the kick and the bass, and for the snare inside a DRUMS bus (where the kick would otherwise count as a snare);
        // a snare chosen alone has no such need (a noise snare has no pitch): it is read when the late window allows
        const bool coin = (E.h.coin >> k) & 1;   // a snare struck with a kick (classifyBus): its own band must still ring 15 - 35 ms on, and its pitch is not asked (the kick's boom fills the late window)
        if (coin && ! coincidentIsOwn (s, E.h.t)) { ++s.rejectWhy[2]; why = 2; return false; }
        const bool needF0 = hasF0 && ! coin && (s.voice != kVoSnare || s.bus), canF0 = hasF0 && lE - E.lS >= ms2s (90.0f);
        if (needF0 && ! canF0) { ++s.rejectWhy[1]; why = 1; return false; }   // too little late window to read a period from
        h = HitRec();
        if (canF0) { h.f0 = E.f0Ok[k] ? E.f0[k] : 0.0f; if (h.f0 <= 0.0f && needF0) { ++s.rejectWhy[2]; why = 2; return false; } }   // no fundamental in the search range: not this instrument
        h.tEnd = E.tEnd[s.voice];
        const bool haveF0 = h.f0 > 0.0f; const double binE = sr / (double) nE, binT = sr / (double) nT;
        const std::vector<double>& Pr = E.prOk[k] ? PrS[prSrc[k]] : PeS;
        bool any = false;
        for (int p = 0; p < V.n; ++p)
        {
            const PartDef& P = V.part[p]; float c = 0, l = 0, hi = 0; bool ok = false;
            if (P.kind == kPkSub) { if (haveF0) { c = h.f0; l = 0.8f * h.f0; hi = 1.5f * h.f0; ok = true; } }
            else if (P.kind == kPkBody) { const float lo2 = std::max (P.lo, haveF0 ? 1.5f * h.f0 : P.lo); if (lo2 < 0.9f * P.hi) ok = findBand (haveF0 ? Pr : PeS, binE, lo2, P.hi, c, l, hi); }
            else if (P.kind == kPkBand)
            {
                float lo2 = P.lo;
                if (coin)   // a snare on a kick: the kick's body fills the low end of the window; search above the body the bus's kick memory has learned (11 Oct, beat.wav: 135 Hz -> the snare's own)
                {
                    const TargetLearned& LK = ts[slotOf (kVoKick, true)].L;
                    if (LK.ready && LK.ok[1] && LK.hi[1] < 0.8f * P.hi) lo2 = std::max (lo2, LK.hi[1]);
                }
                ok = findBand (PeS, binE, lo2, P.hi, c, l, hi);
            }
            else { const int f = flavorOf (P); if (E.dtOk[f]) ok = findBand (DtS[f], binT, P.lo, P.hi, c, l, hi); }
            h.ok[p] = ok; h.c[p] = c; h.lo[p] = l; h.hi[p] = hi; any = any || ok;
        }
        if (! haveF0 && ! any) { ++s.rejectWhy[3]; why = 3; return false; }
        h.key = haveF0 ? h.f0 : (h.ok[V.keyPart] ? h.c[V.keyPart] : 0.0f);
        return true;
    }
    int prSrc[kTSlots] {};

    // The evidence record of the oldest finished hit, as a resumable job: each call does what the credit allows of the current stage (a window copy, a spectrum's FFT, an autocorrelation,
    // one slot's reading) and leaves its place in ev.w; the stages and the arithmetic are the ones of the one-unit-per-call job, so the record is identical.
    void evidenceStep()
    {
        EvJob& E = ev; EvJob::Work& W = E.w;
        switch (E.stage)
        {
        case 0:   // the windows, the end of every slot's late window, what is needed
        {
            E.o = E.h.o; E.tS = E.o; E.tE = E.o + ms2s (kTargetTransMs); E.eS = E.tE; E.eE = E.o + ms2s (kTargetEarlyMs); E.lS = E.eE;
            E.need = E.h.mask; E.needFlav = 0; E.peOk = false;
            for (int k = 0; k < kTSlots; ++k)
            {
                E.f0Ok[k] = false; E.prOk[k] = false; E.f0[k] = 0.0f; E.endAbs[k] = tSamp; prSrc[k] = k;
                if (! (E.need & (1 << k))) continue;
                // the next hit this slot accepted ends its late window. A voice with a fundamental chosen alone (KICK, BASS) hears every onset of the track, so on a full kit
                // the hi-hats ended every kick's window before its period could be read (owner 12 Oct, Live: TARGET KICK learned nothing on a drum kit; synthetic kick + 16th hats:
                // 0 of 128 learned, all "late window too short"): for those, only a hit whose low band rises ends the window (lowBandRises)
                const bool lowOnly = kVoice[ts[k].voice].f0Hi > 0.0f && ! ts[k].bus;
                for (int q = 0; q < qCount; ++q) { const PendHit& n = queue[(qHead + q) % kQueue]; if (n.t > E.h.t && (n.mask & (1 << k)) && (! lowOnly || lowBandRises (n.t))) { E.endAbs[k] = n.t - ms2s (1.0f); break; } }
                const VoiceDef& V = kVoice[ts[k].voice]; for (int p = 0; p < V.n; ++p) if (V.part[p].kind == kPkTrans) E.needFlav |= (uint8_t) (1 << flavorOf (V.part[p]));
            }
            for (int f = 0; f < kFlav; ++f) E.dtOk[f] = false;
            credit -= kCostStep; nextStage (1); break;
        }
        case 1:   // the early window's spectrum (shared by every slot)
        {
            const int Ne = (int) (E.eE - E.eS), tp = (int) ms2s (1.0f);
            if (E.sub == 0)
            {
                if (W.i == 0 && (tSamp - E.o > (int64_t) cap.size() - 64 || Ne > (int) xe.size())) { credit -= kCostStep; nextStage (6); break; }   // the hit has left the ring: every slot counts it as too late
                if (copyWindow (E.eS, Ne, xe.data())) { E.sub = 1; W = EvJob::Work(); }
                break;
            }
            if (psdStep (xe.data(), Ne, tp, tp, fftE, nE, PeS)) { E.peOk = true; nextStage (2); }
            break;
        }
        case 2:   // the fundamental of every slot that has one (one autocorrelation per unit; a slot of the same voice with the same late window shares it)
        {
            if (! E.busy)
            {
                bool found = false; credit -= kCostStep;
                while (E.cursor < kTSlots && ! found)
                {
                    const int k = E.cursor++;
                    if (! (E.need & (1 << k))) continue;
                    const VoiceDef& V = kVoice[ts[k].voice]; if (V.f0Hi <= 0.0f) continue;
                    const int64_t lE = std::min (E.o + ms2s (kTargetLateMs), E.endAbs[k]);
                    if (E.endAbs[k] < E.eE || lE - E.lS < ms2s (90.0f)) continue;
                    bool shared = false;
                    for (int j = 0; j < k && ! shared; ++j)
                        if ((E.need & (1 << j)) && E.f0Ok[j] && ts[j].voice == ts[k].voice && std::min (E.o + ms2s (kTargetLateMs), E.endAbs[j]) == lE) { E.f0[k] = E.f0[j]; E.f0Ok[k] = true; shared = true; }
                    if (shared) continue;
                    E.curK = k; E.busy = true; W = EvJob::Work(); found = true;
                }
                if (! found) nextStage (3);
                break;
            }
            const int k = E.curK; float r = 0.0f;
            if (f0Step (E.lS, std::min (E.o + ms2s (kTargetLateMs), E.endAbs[k]), kVoice[ts[k].voice], r)) { E.f0[k] = r; E.f0Ok[k] = true; E.busy = false; }
            break;
        }
        case 3:   // the early window without the fundamental's damped sine (kick, bass: the body above the sub), one spectrum per unit
        {
            const int Ne = (int) (E.eE - E.eS), tp = (int) ms2s (1.0f);
            if (! E.busy)
            {
                bool found = false; credit -= kCostStep;
                while (E.cursor < kTSlots && ! found)
                {
                    const int k = E.cursor++;
                    if (! (E.need & (1 << k)) || ! E.f0Ok[k] || E.f0[k] <= 0.0f) continue;
                    const VoiceDef& V = kVoice[ts[k].voice]; bool body = false; for (int p = 0; p < V.n; ++p) body = body || V.part[p].kind == kPkBody; if (! body) continue;
                    bool shared = false;
                    for (int j = 0; j < k && ! shared; ++j) if (E.prOk[j] && ts[j].voice == ts[k].voice && E.f0[j] == E.f0[k]) { prSrc[k] = prSrc[j]; E.prOk[k] = true; shared = true; }
                    if (shared) continue;
                    E.curK = k; E.busy = true; E.sub = 0; W = EvJob::Work(); found = true;
                }
                if (! found) nextStage (4);
                break;
            }
            const int k = E.curK;
            if (E.sub == 0) { if (copyWindow (E.eS, Ne, xe.data())) { E.sub = 1; W = EvJob::Work(); } }
            else if (E.sub == 1) { if (removeSineStep (xe.data(), xr.data(), Ne, (double) E.f0[k])) { E.sub = 2; W = EvJob::Work(); } }
            else if (psdStep (xr.data(), Ne, tp, tp, fftE, nE, PrS[k])) { E.prOk[k] = true; prSrc[k] = k; E.busy = false; }
            break;
        }
        case 4:   // CLICK / CRACK / SNAP / CHICK / TOP: the transient window minus the early window, both high-passed at the flavour's corner (two spectra per flavour)
        {
            const int Ne = (int) (E.eE - E.eS), Nt = (int) (E.tE - E.tS), tp = (int) ms2s (1.0f);
            if (! E.busy)
            {
                bool found = false; credit -= kCostStep;
                while (E.cursor < 2 * kFlav && ! found)
                {
                    const int f = E.cursor / 2; ++E.cursor;
                    if (! (E.needFlav & (1 << f))) { if (E.cursor % 2 == 1) ++E.cursor; continue; }
                    E.curK = E.cursor - 1; E.busy = true; E.sub = 0; W = EvJob::Work(); found = true;
                }
                if (! found) nextStage (5);
                break;
            }
            const int f = E.curK / 2, half = E.curK % 2;
            if (half == 0)
            {
                if (E.sub == 0) { if (hpWindowStep (E.eS, Ne, kFlavHz[f], xr.data())) { E.sub = 1; W = EvJob::Work(); } }
                else if (psdStep (xr.data(), Ne, tp, tp, fftE, nE, PeH[f])) E.busy = false;
            }
            else
            {
                if (E.sub == 0) { if (hpWindowStep (E.tS, Nt, kFlavHz[f], xr.data())) { E.sub = 1; W = EvJob::Work(); } }
                else if (E.sub == 1) { if (psdStep (xr.data(), Nt, 0, (int) ms2s (1.5f), fftT, nT, PtH[f])) { E.sub = 2; W = EvJob::Work(); } }
                else
                {
                    const double binE = sr / (double) nE, binT = sr / (double) nT; const int total = nT / 2 + 1;
                    const int cnt = (int) std::min<int64_t> (total - W.i, budgetCount (kCostDiff)); credit -= (int64_t) cnt * kCostDiff;
                    for (int k = (int) W.i; k < (int) W.i + cnt; ++k)
                    {
                        const double fe = (double) k * binT / binE; const int i = std::min ((int) fe, nE / 2 - 1); const double fr = fe - i;
                        DtS[f][(size_t) k] = std::max (0.0, PtH[f][(size_t) k] - (PeH[f][(size_t) i] * (1.0 - fr) + PeH[f][(size_t) (i + 1)] * fr));
                    }
                    W.i += cnt; if (W.i >= total) { E.dtOk[f] = true; E.busy = false; }
                }
            }
            break;
        }
        case 5:   // where each voice's transient ends (relative to the event clock's onset)
        {
            if (! E.busy)
            {
                credit -= kCostStep;
                if (E.cursor == 0) for (int v = 0; v < kTargetVoices; ++v) E.tEnd[v] = 5.0f;
                while (E.cursor < kTSlots && ! (E.need & (1 << E.cursor))) ++E.cursor;
                if (E.cursor >= kTSlots) { nextStage (6); break; }
                E.curK = E.cursor++; E.busy = true; W = EvJob::Work(); break;
            }
            float r = 0.0f; const int k = E.curK;
            if (transEndStep (E.o, E.h.t, kVoice[ts[k].voice].guardHz, r)) { E.tEnd[ts[k].voice] = r; E.busy = false; }
            break;
        }
        default:  // every accepting slot reads its part of the record, one slot per unit
        {
            while (E.cursor < kTSlots && ! (E.need & (1 << E.cursor))) ++E.cursor;
            if (E.cursor >= kTSlots) { E.active = false; break; }
            const int k = E.cursor++; HitRec h; int why = -1; const bool ok = slotRecord (k, h, why); finishSlot (ts[k], h, ok, why); credit -= kCostRecord;
#ifdef DYNEQ_RECLOG
            recLog.push_back ({ k, ok ? 1 : 0, why, h });
#endif
            break;
        }
        }
    }

    // ---- memory: running median of the last 8 hits
    static float medianOf (float* v, int n)
    {
        for (int i = 1; i < n; ++i) { const float x = v[i]; int j = i - 1; while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; --j; } v[j + 1] = x; }
        return n & 1 ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
    }

    static int bodyPartOf (int voice) { return voice == kVoKick || voice == kVoBass ? 1 : -1; }   // the voices that have a fundamental also track their body centre

    void memAdd (TSlot& s, const HitRec& h)
    {
        bool quickSwap = false;
        // SPEC 8d 5, first case: the fundamental (or the key) or the body centre moved more than 1/3 octave, 3 hits in a row
        if (s.nRec >= kTargetMinHits && s.keyMed > 0.0f && h.key > 0.0f)
        {
            bool far = std::abs (std::log2 (h.key / s.keyMed)) > kTargetNewSampleOct;
            const int bp = bodyPartOf (s.voice);
            if (! far && bp >= 0 && h.ok[bp] && s.L.ok[bp] && s.L.centre[bp] > 0.0f) far = std::abs (std::log2 (h.c[bp] / s.L.centre[bp])) > kTargetNewSampleOct;
            // SPEC 8d 7: two far hits that agree with each other (within 1/12 octave) are already a new sound; waiting for a third would play one more hit on the old one
            const bool agree = far && s.devCount >= 1 && s.farKey > 0.0f && std::abs (std::log2 (h.key / s.farKey)) <= kTargetQuickAgreeOct;
            s.devCount = far ? s.devCount + 1 : 0; s.farKey = far ? h.key : 0.0f;
            if (agree && s.devCount == 2) quickSwap = true;
        }
        else { s.devCount = 0; s.farKey = 0.0f; }
        if (s.nRec == kMemHits) { for (int i = 1; i < kMemHits; ++i) s.rec[i - 1] = s.rec[i]; --s.nRec; }
        s.rec[s.nRec++] = h;
        if (s.devCount >= 3)   // a new sample: the three that differ are all that is kept
        {
            for (int i = 0; i < 3; ++i) s.rec[i] = s.rec[s.nRec - 3 + i];
            s.nRec = 3; s.devCount = 0; s.farKey = 0.0f; s.quick = false; ++s.nRelearn;
        }
        else if (quickSwap)   // the two that agree are all that is kept; the cut does not drop to 2/3 meanwhile (SPEC 8d 6 is for an empty memory)
        {
            for (int i = 0; i < 2; ++i) s.rec[i] = s.rec[s.nRec - 2 + i];
            s.nRec = 2; s.devCount = 0; s.farKey = 0.0f; s.quick = true; ++s.nRelearn;
        }
        else if (s.nRec >= 3) s.quick = false;
        ++memVersion;
        updateLearned (s);
    }

    void updateLearned (TSlot& s)
    {
        TargetLearned N; N.hits = s.nRec; N.ready = s.nRec >= 1;
        const VoiceDef& V = kVoice[s.voice]; N.nParts = V.n;
        const int need = std::min (kTargetMinHits, s.nRec);   // an empty memory fills in over the first hits (SPEC 8d 6): one hit already gives a point, the cut is scaled in targetLaw
        float v[kMemHits]; int n = 0;
        for (int i = 0; i < s.nRec; ++i) if (s.rec[i].key > 0.0f) v[n++] = s.rec[i].key;
        float spread = 0.0f;
        if (n > 0)
        {
            s.keyMed = medianOf (v, n);
            if (n >= 4) spread = std::log2 (std::max (v[(3 * n) / 4], 1e-3f) / std::max (v[n / 4], 1e-3f));   // v is sorted now: the interquartile range in octaves
        }
        else s.keyMed = 0.0f;
        N.confidence = std::min (1.0f, (float) s.nRec / (float) kMemHits) * std::clamp (1.0f - spread / 0.5f, 0.0f, 1.0f);
        n = 0; for (int i = 0; i < s.nRec; ++i) if (s.rec[i].f0 > 0.0f) v[n++] = s.rec[i].f0;
        N.f0 = n >= need && n > 0 ? medianOf (v, n) : 0.0f;
        n = 0; for (int i = 0; i < s.nRec; ++i) v[n++] = s.rec[i].tEnd;
        N.transEndMs = n > 0 ? medianOf (v, n) : 0.0f;
        for (int p = 0; p < N.nParts; ++p)
        {
            float a[kMemHits], b[kMemHits], c[kMemHits]; int m = 0;
            for (int i = 0; i < s.nRec; ++i) if (s.rec[i].ok[p]) { a[m] = s.rec[i].c[p]; b[m] = s.rec[i].lo[p]; c[m] = s.rec[i].hi[p]; ++m; }
            if (m >= need && m > 0)
            {
                N.ok[p] = true; N.centre[p] = medianOf (a, m); N.lo[p] = medianOf (b, m); N.hi[p] = medianOf (c, m);
                N.centre[p] = std::clamp (N.centre[p], N.lo[p], N.hi[p]);
            }
        }
        s.L = N;
    }

    static float qFromBw (float bwOct) { return 1.0f / (2.0f * std::sinh (0.34657359f * std::clamp (bwOct, 0.3f, 2.5f))); }

    // The evidence job runs on a work budget, not on a clock: every law tick (32 samples, whatever the host block size) gives kEvTickUnits of work, so a call of n samples gets
    // n / 32 ticks' worth and a 32-sample call never carries a whole hit's analysis. The unit is a tenth of a nanosecond (kCost* below, calibrated with DYNEQ_PROFILE); the pieces of
    // work are sliced to the credit that is left, so a slice ends where the credit does. A new job starts only on the same tick phase as before (blocks % kJobEveryTicks), so
    // what it reads from the queue and the ring is what the one-stage-per-4-ticks job read, and the record is the same bit for bit.
    void runAnalysisJob()
    {
        if (! ev.active)
        {
            credit = 0;
            if (qCount == 0 || (blocks % (uint64_t) kJobEveryTicks) != 0) return;
            const PendHit& h = queue[qHead];
            if (! h.settled || tSamp - h.t < ms2s (kTargetLateMs + kColSettleMs + 1.0f)) return;   // the late window is over and every later hit of the window has been classified
            ev.active = true; ev.stage = 0; ev.cursor = 0; ev.sub = 0; ev.busy = false; ev.w = EvJob::Work(); ev.h = h; ev.startAbs = tSamp; qHead = (qHead + 1) % kQueue; --qCount;
        }
        credit = std::min<int64_t> (credit, 0) + kEvTickUnits;   // an overdraft (one piece of work that cannot be cut) is paid back from the next ticks
#ifdef DYNEQ_PROFILE
        const auto p0 = std::chrono::steady_clock::now(); const int64_t c0 = credit;
#endif
        while (ev.active && credit > 0)
        {
#ifdef DYNEQ_PROFILE
            const auto s0 = std::chrono::steady_clock::now(); const int st = ev.stage; const int64_t cs = credit; evidenceStep();
            const double us = std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - s0).count();
            if (st >= 0 && st < 8) { profMax[st] = std::max (profMax[st], us); profSum[st] += us; profUnits[st] += (double) (cs - credit); ++profN[st]; }
#else
            evidenceStep();
#endif
        }
        if (! ev.active) { const int64_t lat = tSamp - ev.startAbs; evLatencyMax = std::max (evLatencyMax, lat); evLatencyLast = lat; ++evRecords; }
#ifdef DYNEQ_PROFILE
        const double us = std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - p0).count();
        profCallMax = std::max (profCallMax, us); profCallSum += us; ++profCalls; profCallUnitsMax = std::max (profCallUnitsMax, (double) (c0 - credit));
#endif
    }
#ifdef DYNEQ_PROFILE
public:
    double profMax[8] {}, profSum[8] {}, profUnits[8] {}; int profN[8] {}; double profCallMax = 0.0, profCallSum = 0.0, profCallUnitsMax = 0.0; int profCalls = 0;
private:
#endif
public:
    // how long the last / the longest record took from the start of its job to its last slot (samples at the host rate), and how many records were finished
    int64_t evidenceLatencyMaxSamples() const { return evLatencyMax; }
    int64_t evidenceLatencyLastSamples() const { return evLatencyLast; }
    int64_t evidenceRecords() const { return evRecords; }
#ifdef DYNEQ_RECLOG
    struct RecLogRow { int slot, ok, why; HitRec h; };
    std::vector<RecLogRow> recLog;
#endif
private:

    // one edge stage, once per law tick: the cut depth follows the goal (1 dB/ms, 2 ms smoothing), the crossover glides, the split is blended in while a cut is wanted
    void edgeUpdate (EdgeStage& e, bool want, float wantF, float goalDb, int tag, bool limitedGlide, float inv)
    {
        const int slopeIdx = slopeIndex (par.targetSlope);
        const bool tuned = e.tag == tag;
        goalDb = std::clamp (tuned && want ? goalDb : 0.0f, 0.0f, kTargetAmountMaxDb);
        const float d = goalDb - e.gLim;
        e.gLim = std::abs (d) <= maxBandRise ? goalDb : e.gLim + (d > 0.0f ? maxBandRise : -maxBandRise);
        e.gSm += (e.gLim - e.gSm) * cBandSm;
        if (std::abs (e.gLim - e.gSm) < 1e-4f) e.gSm = e.gLim;
        const bool idle = e.gSm == 0.0f && e.gStart == 0.0f && e.gLim == 0.0f;
        const bool start = want && ! e.armed;
        if (want && (start || (idle && ! tuned)))   // first use, or the stage's job changed while its cut is out: tune to the new edge from a clean state
        {
            e.tag = tag; e.freq = wantF; e.slope = slopeIdx; e.armed = true; e.oldRun = false; e.w = 1.0; e.dW = 0.0;
            e.cur.setup (slopeIdx); e.cur.target ((double) e.freq, sr, 1.0, true); e.cur.clear();
            if (start) { e.mix = 0.0; e.dMix = 0.0; e.c = 1.0; e.dc = 0.0; }
        }
        else if (want && e.armed)
        {
            const float df = std::log2 (wantF / e.freq);   // SPEC 8f: held until the sound really moved (1/96 octave dead band), then a glide that is never faster than 1/3 octave per 100 ms (REVERB-READY: 1/12)
            const bool towardProtection = e.below ? df < 0.0f : df > 0.0f;   // SPEC 8g: the dead band is only for jitter; a placement that moves AWAY from the protected part is never held back, or the cut at its centre would exceed the limit by up to the dead band's 1/96 octave x SLOPE (0.75 dB at 72)
            if (std::abs (df) > kCutHoldOct || towardProtection) { const float step = (limitedGlide ? kRevGlideOctPerBlockMs : kEdgeGlideOctPerBlockMs) * blockMs; e.freq *= std::exp2 (std::clamp (df, -step, step)); }
        }
        if (! want && idle && e.mix == 0.0 && e.dMix <= 0.0) { e.armed = false; e.run = false; e.oldRun = false; e.gStart = 0.0f; e.tag = -1; return; }
        if (! e.armed) return;
        if (e.slope != slopeIdx)   // SLOPE moved: the old filter set keeps running and fades out under the new one
        {
            e.old = e.cur; e.oldSlope = e.slope; e.oldRun = true; e.w = 0.0; e.slope = slopeIdx;
            e.cur.setup (slopeIdx); e.cur.target ((double) e.freq, sr, 1.0, true); e.cur.clear();
        }
        else e.cur.target ((double) e.freq, sr, (double) inv, false);
        if (e.oldRun) e.old.target ((double) e.freq, sr, (double) inv, false);
        e.run = true;
        // per-sample ramps over the next sub-block (they end at the target after kBlock samples)
        const double cT = e.gSm >= kTargetAmountMaxDb - 1e-3f ? 0.0 : std::pow (10.0, -(double) e.gSm / 20.0);   // AMOUNT max: the cut band is removed (a real high-pass / low-pass)
        e.dc = (cT - e.c) * (double) inv; if (e.gStart == 0.0f && e.gSm == 0.0f && e.gLim == 0.0f) { e.c = 1.0; e.dc = 0.0; }
        const bool cutting = e.gLim > 0.0f || e.gSm > 0.0f || edgeForceIn;
        const double mT = cutting ? 1.0 : 0.0, up = (double) (blockMs / kEdgeMixUpMs), dn = (double) (blockMs / kEdgeMixDownMs);
        const double m1 = e.mix + std::clamp (mT - e.mix, -dn, up);
        e.dMix = (m1 - e.mix) * (double) inv;
        if (e.oldRun) { const double w1 = std::min (1.0, e.w + (double) (blockMs / kEdgeSlopeFadeMs)); e.dW = (w1 - e.w) * (double) inv; }
        e.gStart = e.gSm;
    }

    // once per 32 samples (from law): late analysis, memory, event clock, cut target, rate limit, smoothing, bell ramp, edge stages
    void targetLaw()
    {
        // listening, whatever TARGET is set to: the evidence job gets its work budget every tick (a new job starts every kJobEveryTicks ticks at the earliest). One hit's analysis is a
        // few hundred microseconds; run in one go it would be a burst inside one host block (small buffers would not survive it), so it is sliced to the budget (see runAnalysisJob)
        runAnalysisJob();
        const bool on = par.targetInst != kTgtOff;
        const float inv = 1.0f / (float) kBlock;
        float goal = 0.0f, wantFs[kTargetBells], wantQ = tbs[0].q; bool valid = false; int tag = tbs[0].tag;
        for (int i = 0; i < kTargetBells; ++i) wantFs[i] = tbs[i].freq;
        plan[0].on = plan[1].on = false;
        bool eWant[2] = { false, false }; float eF[2] = { 1000.0f, 1000.0f }, eGoal[2] = { 0.0f, 0.0f }; int eTag[2] = { -1, -1 }; bool rev = false;
        float ramp = 1.0f;
        curSlot = -1;
        if (on)
        {
            int v, pt; bool bus; targetSelect (v, pt, bus); vPart = pt;
            curSlot = slotOf (v, bus);
            const VoiceDef& V = kVoice[v]; const TSlot& S = ts[curSlot]; const TargetLearned& L = S.L;
            const bool reverbOn = par.reverbReady && kInst[par.targetInst].reverbOk;
            ramp = S.quick ? 1.0f : (float) std::min (S.nRec, kTargetMinHits) / (float) kTargetMinHits;   // SPEC 8d 6: an empty memory ramps in 1/3, 2/3, full; any memory with 3 hits is full
            tag = v * 8 + vPart;
            if (reverbOn)   // a band-limit: the low edge under the learned body, the high edge over the learned crack / harsh band
            {
                if (L.ready && V.revBody >= 0 && L.ok[V.revBody] && L.ok[V.revTop])
                {
                    const float bc = L.centre[V.revBody], tc = L.centre[V.revTop];   // SPEC 8g: the low corner protects the body's centre, the high one the top part's; the cut reached is read one octave outside each
                    const float f0 = std::clamp (kRevLowShare * L.lo[V.revBody], 20.0f, 0.45f * (float) sr), f1 = std::clamp (kRevHighShare * L.hi[V.revTop], 2.0f * f0, 0.45f * (float) sr);
                    eF[0] = planEdge (0, true, f0, bc, 0.5f * bc); eF[1] = planEdge (1, false, f1, tc, std::min (2.0f * tc, 0.45f * (float) sr), 2.0f * eF[0]);
                    eWant[0] = eWant[1] = true; eGoal[0] = eGoal[1] = par.targetAmount * ramp; eTag[0] = eTag[1] = 100 + v; rev = true;
                }
            }
            else if (L.ready && L.ok[vPart])
            {
                const PartDef& P = V.part[vPart];
                const float g = 1.0f;   // SPEC 8f: a TARGET cut is static. Once the memory has the points it stays at AMOUNT, between hits too; the hits never move its depth (P1j)
                if (P.edge == 0)   // the plateau: three bells over the part, mean cut = AMOUNT (11 Oct)
                {
                    const int sub = (v == kVoKick || v == kVoBass) && vPart != 0 ? 0 : -1;   // a BODY over a SUB: the SUB keeps its level (SPEC 8g, as for the edges)
                    planPlateau (L.centre[vPart], L.lo[vPart], L.hi[vPart], sub >= 0 && L.ok[sub] ? L.centre[sub] : 0.0f, par.targetWidth);
                    valid = true; wantQ = pplan.q;
                    for (int i = 0; i < kTargetBells; ++i) wantFs[i] = pplan.lo * std::pow (pplan.hi / pplan.lo, 0.5f * (float) i);
                    goal = std::min (par.targetAmount, kBellMaxDb) / pplan.k * g * ramp;
                }
                else
                {
                    const int k = P.edge - 1;
                    eWant[k] = true; eTag[k] = tag; eGoal[k] = par.targetAmount * g * ramp;
                    const float defF = P.edge == 1 ? std::clamp (kEdgeBelowShare * L.hi[vPart], 20.0f, 0.45f * (float) sr) : std::clamp (kEdgeAboveShare * L.lo[vPart], 20.0f, 0.45f * (float) sr);
                    const int bp = protectPart (v); const float prot = bp >= 0 && L.ok[bp] ? L.centre[bp] : 0.0f;   // SPEC 8g: the neighbour part (BODY) keeps its level
                    eF[k] = planEdge (k, P.edge == 1, defF, prot, L.centre[vPart]);
                }
            }
        }
        // the bells (one plateau of kTargetBells): each drains, glides and ramps as the single bell did
        for (int bi = 0; bi < kTargetBells; ++bi)
        {
            TBellState& b = tbs[bi]; const float wantF = wantFs[bi];
            const bool tuned = b.tag == tag;
            const float gl = std::clamp (tuned ? goal : 0.0f, 0.0f, kBellMaxDb / 0.3f);   // a bell tuned for another voice / part drains to 0 dB first
            const float d = gl - b.gLim;
            b.gLim = std::abs (d) <= maxBandRise ? gl : b.gLim + (d > 0.0f ? maxBandRise : -maxBandRise);
            b.gSm += (b.gLim - b.gSm) * cBandSm;
            if (std::abs (b.gLim - b.gSm) < 1e-4f) b.gSm = b.gLim;
            const bool idle = b.gSm == 0.0f && b.gStart == 0.0f && b.gLim == 0.0f;
            if (idle && ! tuned) b.tag = tag;
            if (valid)
            {
                if (idle) { b.freq = wantF; b.q = wantQ; }
                else
                {
                    const float df = std::log2 (wantF / b.freq), dq = std::log2 (wantQ / b.q);   // SPEC 8f: the bell holds still until the sound really moved (hit-to-hit wobble of the median stays inside the dead band)
                    if (std::abs (df) > kCutHoldOct) b.freq = b.freq * std::exp2 (df * cBandShape);
                    if (std::abs (dq) > kCutHoldOct) b.q    = b.q * std::exp2 (dq * cBandShape);
                }
            }
            const bool run = b.gSm > 0.0f || b.gStart > 0.0f;
            const bool fresh = run && ! b.active;
            if (fresh) std::memset (b.st, 0, sizeof (b.st));
            b.active = run;
            if (run)
            {
                const float w0 = 2.0f * (float) M_PI * b.freq / (float) sr;
                const float cwT = std::cos (w0), alT = std::sin (w0) / (2.0f * b.q);
                if (fresh) { b.cosw0 = cwT; b.alpha = alT; }
                b.dCosw0 = (cwT - b.cosw0) * inv; b.dAlpha = (alT - b.alpha) * inv;
                b.A = std::pow (10.0f, -b.gStart / 40.0f);
                b.dA = (std::pow (10.0f, -b.gSm / 40.0f) - b.A) * inv;
            }
            b.gStart = b.gSm;
        }
        // the edge stages (SUB / CLICK / REVERB-READY low and high)
        for (int k = 0; k < 2; ++k) edgeUpdate (edge[k], eWant[k], eF[k], eGoal[k], eTag[k], rev, inv);
        for (int k = 0; k < 2; ++k)   // what the filter is doing now (it glides to the placed crossover): the numbers the panel reads
            if (plan[k].on && edge[k].armed)
            {
                EdgePlan& P = plan[k]; const int si = slopeIndex (par.targetSlope); const float amt = std::min (par.targetAmount, kTargetAmountMaxDb), fe = edge[k].freq;
                P.lossDb = P.prot > 0.0f ? (float) edgeCutDbAt (P.prot, fe, si, amt, k == 0, sr) : 0.0f; P.achDb = P.ref > 0.0f ? (float) edgeCutDbAt (P.ref, fe, si, amt, k == 0, sr) : amt;
            }
        gentle = false; for (int k = 0; k < 2; ++k) if (plan[k].on && plan[k].achDb < std::min (par.targetAmount, kTargetAmountMaxDb) - kGentleMarginDb) gentle = true;
        revOn = rev;
        tgtRun = on || tgtAnyActive();
    }

    // the stage's filters run for every sample while it is armed (so the split is warm when a cut starts); the blend decides what is heard
    void runEdge (EdgeStage& e, float* const* main, int numCh, int pos, int m)
    {
        // LR12 / LR36 sum flat only with one band inverted: the CUT band is the one that is (the other band keeps its polarity, so the signal that stays does not flip while the split fades in)
        const double sgn = e.cur.flip ? -1.0 : 1.0, osg = e.old.flip ? -1.0 : 1.0; const bool below = e.below;
        double c = e.c, mx = e.mix, w = e.w;
        for (int i = 0; i < m; ++i)
        {
            e.cur.advance(); if (e.oldRun) e.old.advance();
            for (int ch = 0; ch < numCh; ++ch)
            {
                float* y = main[ch] + pos + i; const double x = (double) *y; double lo, hi; e.cur.run (ch, x, lo, hi);
                double S = below ? hi + sgn * c * lo : lo + sgn * c * hi;
                if (e.oldRun)
                {
                    double ol, oh; e.old.run (ch, x, ol, oh);
                    const double So = below ? oh + osg * c * ol : ol + osg * c * oh;
                    const double we = w * w * (3.0 - 2.0 * w); S = So + we * (S - So);
                }
                const double me = mx * mx * (3.0 - 2.0 * mx);   // smoothstep: no kink where the blend starts and ends (a linear fade of the all-pass difference was a -40 dBFS tick above 1 kHz)
                *y = (float) (x + me * (S - x));
            }
            c += e.dc; mx += e.dMix; w += e.dW;
        }
        e.c = c; e.mix = mx; if (e.dMix < 0.0 && e.mix < 1e-9) e.mix = 0.0;
        if (e.oldRun) { e.w = w; if (e.w >= 1.0) e.oldRun = false; }
    }

    void targetApply (float* const* main, int numCh, int pos, int m)
    {
        for (auto& b : tbs) if (b.active) runBell<true> (main, numCh, pos, m, b.alpha, b.dAlpha, b.cosw0, b.dCosw0, b.A, b.dA, b.st);
        for (int k = 0; k < 2; ++k) if (edge[k].run) runEdge (edge[k], main, numCh, pos, m);
    }

    Params par;
    double sr = 48000.0; int maxBlockSize = 512; float blockMs = 0.6667f;
    float fadeStep = 0.0556f, cAtt = 0.1f, cRel = 0.01f, cHEnv = 0.002f, cHQ = 0.03f, maxRise = 0.6667f, maxGlideOct = 0.0056f;
    float fc[kBands] {}, bw[kBands] {}, bellQ[kBands] {}, cosw0[kBands] {}, sinw0[kBands] {};
    // bellCoeffs() (used for the shape matrix) and runBell() are the same RBJ peaking filter
    float ab0[kBands] {}, aa1[kBands] {}, aa2[kBands] {};
    float ms1[kBands] {}, ms2[kBands] {}, ms3[kBands] {}, ms4[kBands] {};
    double dms[4][kBands] {}, dks[4][kBands] {}, db0[kBands] {}, da1[kBands] {}, da2[kBands] {};   // the signal law's analysis (analyseD)
    float ks1[kBands] {}, ks2[kBands] {}, ks3[kBands] {}, ks4[kBands] {};
    float sumM[kBands] {}, sumK[kBands] {}; double sumKT = 0.0;
    double envM[kBands] {}, envK[kBands] {}, envKT = 0.0;
    double envMf[kBands] {}, envKf[kBands] {};   // the signal law's presence levels: symmetric, one period of fc
    double envMn[kBands] {}, envKn[kBands] {};   // ... and its need levels: those held at their peak, released with SPEED
    float lvlMn[kBands] {}, lvlKn[kBands] {};
    float lvlMf[kBands] {}, lvlKf[kBands] {}, cRelF[kBands] {}, sigKRefB[kBands] {};
    float leakDbC[kBands][kBands] {};
    double leakLin[kBands][kBands] {}, leakLinC[kBands][kBands] {};   // the same as power ratios            // the same for a component at j's centre or beyond (a neighbour whose energy is spread)
    float leakDb[kBands][kBands] {};             // worst case: the most band b reads, relative to band j, from one component in j outside the edge zone
    static constexpr int kSigPairN = 49;         // positions between two neighbouring centres (the lone-pair level refinement)
    float pairD[kBands][kSigPairN] {}, pairRb[kBands][kSigPairN] {}, pairRn[kBands][kSigPairN] {};   // pair (b, b+1): reading b - reading b+1, and each reading, for one component
    float sigDbgPK[kBands] {}, sigDbgPM[kBands] {}, sigDbgNeed[kBands] {};   // the signal law's last presence and need per band (dyneqraw --levellog)                     // the signal law cut this band itself this tick (the spread only joins bands that collide)   // double: a float one-pole with a slow release has a dead band (the step falls under half an ulp), so the level kept a trace of old audio (plugin_check 13.3b)
    float lvlM[kBands] {}, lvlK[kBands] {}, lvlKT = kDbFloor;
    float cutSm[kBands] {}, target[kBands] {};
    float gain[kBands] {}, gainStart[kBands] {};
    float shape[kBands][kBands] {}, shapeT[kBands][kBands] {};   // shapeT[k][b] = shape[b][k]
    bool  inFocus[kBands] {}, active[kBands] {}; bool anyActive = false;
    float focusFade[kBands] {};   // 1 inside FOCUS, 0 outside, moving between them at 1/kFocusFadeMs; snapped to the pattern until the first law tick
    float bellAlpha[kBands] {}, bellA[kBands] {}, bellDA[kBands] {};
    float bst[kBands][4] {};
    // SELF
    float hE0 = 0.0f, hStep = 0.0f;
    bool analysisTiling = false;
    float hbwHz[kHBands] {};
    float hfc[kHBands] {}, hb0[kHBands] {}, ha1[kHBands] {}, ha2[kHBands] {};
    float hs1[kHBands] {}, hs2[kHBands] {}, hs3[kHBands] {}, hs4[kHBands] {};
    float sumH[kHBands] {}, envH[kHBands] {}, lvlH[kHBands] {}, hEnv[kHBands] {}, excess[kHBands] {};
    Slot  hunt[kHunters];
    // free bands
    FBand fb[kFreeBands];
    float fcAtt[kFreeBands] {}, fcRel[kFreeBands] {};
    float cBandSm = 0.2f, cBandShape = 0.05f, maxBandRise = 0.6667f;
    bool  freeActive = false;
    float* dryStore = nullptr;
    bool bakeOn = false; double bakeSum[kBands] {}; uint64_t bakeCutBlocks[kBands] {}; uint64_t bakeCounted = 0, bakeSeen = 0; double bakeMainSum = 0.0;
    int phase = 0; uint64_t blocks = 0;
};
} // namespace dyneq
