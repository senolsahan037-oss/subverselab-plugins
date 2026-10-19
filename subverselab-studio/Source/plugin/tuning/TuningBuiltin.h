// Built-in tunings of the Tuning Lab (owner 19 Oct: "devam" to the proposal of seven makam tunings in the Lab's list, values
// from sources, his ear decides). The names live only in the Lab's LOAD list and on its cards; panel labels, presets' names and
// public images stay neutral (SPEC.md "Tuning Lab", "Built-in tunings").
//
// System: Arel-Ezgi-Uzdilek, the octave in 53 equal commas (1 koma = 1200/53 = 22.64 ct). Each tuning is a 12-key table: the
// makam's perdes sit on the keys of their staff notes (rast = G, dugah = A, segah = B, cargah = C, neva = D, huseyni = E,
// eviç = F#, acem = F), retuned to their comma positions; keys the makam does not use stay 12-equal. A4 = dugah = 440 Hz
// (refKey 69), so C (cargah) sits 5.66 ct under 12-equal C and every non-scale key is 12-equal against A.
// Comma positions (above C, mod 53) from the AEU perde table; scales from TDV Islam Ansiklopedisi (Segah, Huzzam, Kurdi, Saba)
// and the AEU tetrachords (Rast, Ussak, Hicaz). Sources and checks: notes/MAKAM_TUNINGS_2026-10-19.md.
#pragma once
#include "TuningLab.h"

namespace jflab::builtin
{
struct Entry
{
    const char* name;          // shown in the LOAD list and on the card
    const char* id;            // preset files refer to it: <labtuning a="rast"/>
    int commas[12];            // per key C..B: comma position above C (0..52), -1 = 12-equal against A
    int fallKey = -1, fallCommas = 0;   // one key that plays lower when the melody comes down onto it (rast: eviç -> acem)
};

inline const std::vector<Entry>& entries()
{
    //                     C   C#  D   D#  E   F   F#  G   G#  A   A#  B
    static const std::vector<Entry> e = {
        { "Rast (53)",   "rast",   { 0, -1,  9, -1, 18, 22, 26, 31, -1, 40, -1, 48 }, 6, 22 },
        { "Ussak (53)",  "ussak",  { 0, -1,  9, -1, 18, 22, -1, 31, -1, 40, -1, 48 } },
        { "Hicaz (53)",  "hicaz",  { 0,  4,  9, -1, 18, 22, 26, 31, -1, 40, 45, -1 } },
        { "Huzzam (53)", "huzzam", { 0, -1,  9, 14, 18, -1, 26, 31, -1, 40, 44, 48 } },
        { "Saba (53)",   "saba",   { 0,  5,  9, -1, 17, 22, -1, 31, 36, 40, -1, 48 } },
        { "Segah (53)",  "segah",  { 0, -1,  9, -1, 17, 22, 26, 31, -1, 40, 44, 48 } },
        { "Kurdi (53)",  "kurdi",  { 0, -1,  9, -1, 18, 22, -1, 31, -1, 40, 44, -1 } },
    };
    return e;
}

constexpr double kComma = 1200.0 / 53.0;

inline Tuning make (const Entry& en)
{
    Tuning t;
    t.name = en.name;
    const double aOff = 40.0 * kComma - 900.0;            // A's comma position against 12-equal A (C = 0): +5.66 ct
    auto keyCents = [&] (int k, int commas) { return commas >= 0 ? commas * kComma : k * 100.0 + aOff; };
    t.cents.assign (12, 0.0);
    for (int k = 1; k < 12; ++k) t.cents[(size_t) k - 1] = keyCents (k, en.commas[k]);
    t.cents[11] = 1200.0;
    if (en.fallKey > 0)
    {
        t.fall = t.cents;
        t.fall[(size_t) en.fallKey - 1] = en.fallCommas * kComma;
    }
    t.mapSize = 0; t.middleKey = 60; t.refKey = 69; t.refHz = 440.0;
    return t;
}

inline bool find (const std::string& id, Tuning& out)
{
    for (auto& en : entries())
        if (id == en.id || id == en.name) { out = make (en); return true; }
    return false;
}
} // namespace jflab::builtin
