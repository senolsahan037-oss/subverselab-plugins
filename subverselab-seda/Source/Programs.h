// SEDA programs (SPEC 2.5, rewritten 11 Oct): what a click on P1 VOCAL / P2 DRUMS / P3 BASS / P4 KEYS writes into the real parameters.
// A program is a recall, not an overlay: the processor writes these values into the knobs (one undo step, visible to the host), and from then on the
// knobs are the truth. Nothing here is read by the audio thread.
//
// The cells of the SPEC table:                 NOTE      WET HP   DUCK   THROW  other
//   P1 VOCAL    1/4 dotted, SYNC on            250 Hz    60 %     6 dB
//   P2 DRUMS    1/16, SYNC on                  400 Hz    30 %     0
//   P3 BASS     (not written)                  150 Hz    50 %     0      MIX capped at 20 % (a higher MIX is lowered to 20, a lower one stays)
//   P4 KEYS     1/8, SYNC on                   OFF       50 %     0      STEREO PING-PONG
// "Not written" cells (NOTE and SYNC of P3, STEREO of P1-P3) stay as the user has them. The three LISTEN cells (WET HP, DUCK, THROW) are always written:
// a dash in the SPEC table means the factory value (WET HP off, DUCK 50 %, THROW 0), so a program is the same whatever program came before it.
// P0 MANUAL writes nothing; it only sets the SOURCE selector back to 0.
#pragma once

#include "SedaEngine.h"

namespace seda
{
struct Program
{
    const char* name;
    int   sync;        // -1: not written, 0 / 1
    int   note;        // -1: not written, else note table index
    int   stereo;      // -1: not written, else Stereo
    float wetHpHz;     // always written
    float duckPct;     // always written (0 .. 100)
    float throwDb;     // always written (0 .. 12)
    float mixMaxPct;   // 100: not written; lower: MIX is lowered to this value when it is higher
};

inline const Program& program (int index)
{
    static const Program table[kNumSources] = {
        { "MANUAL", -1, -1, -1, 0.0f,                           50.0f,                            0.0f,                      100.0f },
        { "VOCAL",   1, listen::kSsNoteVocal, -1, (float) listen::kSsHpVocal, listen::kSsDuckVocal * 100.0f, listen::kSsThrowVocalDb, 100.0f },
        { "DRUMS",   1, listen::kSsNoteDrums, -1, (float) listen::kSsHpDrums, listen::kSsDuckDrums * 100.0f, 0.0f,                  100.0f },
        { "BASS",   -1, -1,                  -1, (float) listen::kSsHpBass,  50.0f,                          0.0f,                  listen::kSsMixCapBass * 100.0f },
        { "KEYS",    1, listen::kSsNoteSynth, kPingPong, 0.0f,               50.0f,                          0.0f,                  100.0f } };
    return table[std::max (0, std::min ((int) kNumSources - 1, index))];
}
}
