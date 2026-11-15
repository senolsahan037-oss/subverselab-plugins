#pragma once

/*  BOE-B reference values: what a finished rap vocal measures like. Every number here has its source next to it
    (SPEC.md "Reference values"); the engine and the analyser read them from here only, so one line moves a target.

    Band balance is measured as the power of a band relative to the MID band (500-800 Hz third octaves), on the
    vocal's active frames. Bands are third-octave groups, edges at the third-octave band limits:
        LOW   63-100 Hz    ( 56 .. 112 Hz)   rumble / proximity, sets the high-pass
        MUD   200-400 Hz   (178 .. 447 Hz)
        MID   500-800 Hz   (447 .. 891 Hz)   the reference band
        PRES  1-3.15 kHz   (891 .. 3548 Hz)
        SIB   4-8 kHz      (3548 .. 8913 Hz)
        AIR   10-16 kHz    (8913 .. 17783 Hz)

    Natural speech: Byrne, Dillon & Tran 1994, JASA 96(4) 2108, Table II, "combined" universal LTASS (17 samples,
    12 languages), third-octave levels summed in power per band:
        LOW -10.2, MUD +1.5, PRES -6.2, SIB -14.5, AIR -18.4 dB re MID.
    A finished rap vocal = natural speech + the usual finishing moves from engineering guides (Vocalfy, MuseHub,
    Audiofanzine "Mixing rap vocals part 2"): mud cut 2-4 dB (we take 2), presence +1-3 dB (2), air +1-2 dB with
    less air on rap than on singing (1).
*/
namespace svl::boeb {

// Band edges, Hz (third-octave band limits of the groups above).
inline constexpr double kLowLo  = 56.2,   kLowHi  = 112.2;
inline constexpr double kMudLo  = 177.8,  kMudHi  = 446.7;
inline constexpr double kMidLo  = 446.7,  kMidHi  = 891.3;
inline constexpr double kPresLo = 891.3,  kPresHi = 3548.1;
inline constexpr double kSibLo  = 3548.1, kSibHi  = 8912.5;
inline constexpr double kAirLo  = 8912.5, kAirHi  = 17782.8;

// Natural speech, dB re MID (Byrne 1994 Table II, combined).
inline constexpr float kLtassLow  = -10.2f;
inline constexpr float kLtassMud  =   1.5f;
inline constexpr float kLtassPres =  -6.2f;
inline constexpr float kLtassSib  = -14.5f;
inline constexpr float kLtassAir  = -18.4f;

// Finished rap vocal, dB re MID.
inline constexpr float kRefMud  = kLtassMud - 2.0f;    // -0.5
inline constexpr float kRefPres = kLtassPres + 2.0f;   // -4.2
inline constexpr float kRefAir  = kLtassAir + 1.0f;    // -17.4
inline constexpr float kRefSib  = kLtassSib + 1.0f;    // -13.5: the most sibilance we let through
inline constexpr float kRefTolDb = 2.0f;               // "inside the reference" = within +-2 dB

// Band-limited sources (recorded at 16 kHz, phone, old converters, 22 kHz speech synthesis): no content above the
// cut, so no shelf can lift AIR; the exciter makes the octave above the cut from the octave under it. LISTEN finds the
// cut on its averaged spectrum: the highest frequency still within kCutBelowMidDb of MID's density. Natural speech
// stays within about 25 dB of MID up to 16 kHz (Byrne 1994); the takes here fall 50 dB at 7.4 kHz.
inline constexpr float kCutBelowMidDb = 50.0f;
inline constexpr double kFullBandHz = 14000.0;   // a cut at or above this (or above 0.9 x Nyquist) is not band-limited
inline constexpr float kExciteTargetBelowRefDb = 0.5f;   // the exciter aims AIR at the reference minus this

// Level against the beat: lead-vocal-to-accompaniment ratio of rap, A-weighted RMS, 1 s windows, windows more than
// 20 dB under the track's maximum dropped. 3.2 dB mean [2.7, 3.6], 83 Grammy-nominated rap songs 1990-2020
// (JASA Express Letters 3(4) 043201, "Lead-vocal level in recordings of popular music 1946-2020").
inline constexpr float kRapLarDb   = 3.2f;
inline constexpr float kLarLowDb   = 1.0f;     // lamp 1 red under this
inline constexpr float kLarGoodLo  = 2.0f;     // lamp 1 green from here ...
inline constexpr float kLarGoodHi  = 4.5f;     // ... to here

// Level spread of the vocal: 90th minus 10th percentile of the 400 ms short-term A-weighted level over active
// frames. NO published source found for a finished rap vocal: this is our own working value, to be replaced by a
// measurement on finished vocals (STATUS "open").
inline constexpr float kRefSpreadDb = 9.0f;

// Analysis framing.
inline constexpr double kFrameSec   = 0.1;     // hop
inline constexpr int    kShortTermFrames = 4;  // 400 ms short-term window
inline constexpr float  kActiveFloorDb = -55.0f;   // a frame is active above this (A-weighted, dBFS) ...
inline constexpr float  kActiveBelowMaxDb = 20.0f; // ... and within this of the loudest frame (as the LAR paper)

} // namespace svl::boeb
