// TORNA presets (owner 27 Oct: "a preset step would be very good, even if it steps out of the analog feel").
// Panel marks in dB, 16 Hz ... 32 kHz, the same on both banks. A preset writes the 24 sliders (they glide, one undo step);
// a double-click then returns a slider to the preset's value (UI signature 2). Bus-oriented starting points; the
// bands overlap as on the original, so the real curve (EQUALIZER GRAPHIC) can rise above the marks.
#pragma once
#include <array>

namespace torna
{
struct Preset { const char* name; std::array<float, 12> marks; };

inline const std::array<Preset, 12>& presets()
{
    static const std::array<Preset, 12> p { {
        { "FLAT",           { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
        { "MASTER SMILE",   { 3, 4.5f, 3, 1, -1, -2, -2.5f, -1, 1, 3, 4.5f, 3.5f } },
        { "MASTER GENTLE",  { 1, 1.5f, 1, 0.5f, 0, -0.5f, -1, -0.5f, 0.5f, 1, 1.5f, 1 } },
        { "MIX BUS CLEAN",  { 0, 0, 0, -1, -2.5f, -2, -1, 0, 1, 1, 0.5f, 0 } },
        { "LOW END WEIGHT", { 2, 4, 3, 1, 0, 0, 0, 0, 0, 0, 0, 0 } },
        { "VOCAL PRESENCE", { -2, -2, -1, -3, -2, 0, 0.5f, 2, 4, 2, 1, 0 } },
        { "DRUM BUS",       { 0, 2, 6, 2, -1, -4, -1, 0.5f, 3, 2, 1, 0 } },
        { "AIR",            { 0, 0, 0, 0, 0, 0, 0, 0, 0.5f, 1.5f, 3.5f, 4 } },
        { "LOUDNESS",       { 6, 5, 3, 1, 0, 0, 0, 0, 1, 2, 3, 3 } },
        { "BRIGHT HI-FI",   { 0, 1, 1, 0, -0.5f, -1, -0.5f, 1, 2.5f, 3, 3, 2 } },
        { "WARM",           { 1, 2, 2, 2, 1, 0, -0.5f, -1, -1.5f, -2, -2.5f, -3 } },
        { "RADIO",          { -12, -12, -9, -4, 1, 4, 5, 4, 1, -4, -9, -12 } },
    } };
    return p;
}
constexpr int kNumPresets = 12;
}
