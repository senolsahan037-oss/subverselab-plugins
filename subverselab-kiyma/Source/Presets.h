#pragma once

// The five "sweet spot" presets of the lever-switch bank (docs/UI_SPEC_v4_switches.md). Plain values in the parameters' own units: the
// knob units, edge in ms, output in dB. Order of v[] = kPresetParamIds below.
// Plain C++, no JUCE, so render can use the table too.
namespace kiyma
{
struct Preset
{
    const char* name;
    float v[8];   // glitch, brutal, offset, envelope, attack, release, mix, output
};

inline constexpr const char* kPresetParamIds[8] { "glitch", "brutal", "offset", "envelope", "attack", "release", "mix", "output" };

// DSP v6 moods (docs/DSP_V6_CHOPPER.md). Plain values: density 0..1, reach as its 0..1 knob ((steps - 1) / 15), pattern (seed - 1) / 15,
// length index / 3 (1/2, 1, 2, 4 bars), edge ms, steps index / 2 (8, 16, 32), mix, output dB. CRANK (shuffle) is not part of a mood: a
// recall sets it to 0. MINCE is the owner's ChopperLab sound: seed 1, density 40, reach 6, a 2-bar phrase in one bar, hard cuts.
inline constexpr Preset kPresets[5] {
    { "LEAN",    { 0.00f, 5.0f / 15.0f, 0.0f / 15.0f, 1.0f / 3.0f, 2.0f, 0.5f, 1.00f,  0.0f } },   // no chop: one bar replayed on the grid
    { "MINCE",   { 0.40f, 5.0f / 15.0f, 0.0f / 15.0f, 2.0f / 3.0f, 0.0f, 0.5f, 1.00f,  0.0f } },   // the ChopperLab Haram sound
    { "CHUNK",   { 0.40f, 5.0f / 15.0f, 2.0f / 15.0f, 1.0f / 3.0f, 1.0f, 0.0f, 1.00f,  0.0f } },   // 8 big slices of one bar, pattern 3
    { "GRISTLE", { 0.60f, 3.0f / 15.0f, 4.0f / 15.0f, 3.0f / 3.0f, 0.0f, 1.0f, 1.00f, -1.0f } },   // 4 bars in one, 32 steps, pattern 5
    { "BONE",    { 0.50f, 7.0f / 15.0f, 1.0f / 15.0f, 0.0f / 3.0f, 0.0f, 0.5f, 1.00f, -1.0f } } }; // half a bar stretched over one, pattern 2

/** The auto-trip rule, on normalised values: a parameter further than 0.01 from the active preset's value trips the switch. */
inline constexpr float kTripTolerance = 0.01f;
inline bool presetTrips (float currentNormalised, float presetNormalised) noexcept
{
    const float d = currentNormalised - presetNormalised;
    return (d < 0.0f ? -d : d) > kTripTolerance;
}
} // namespace kiyma
