#pragma once
// Type-in text for float parameters (family batch, 2026-10-12). A host's parameter box (Logic, Reaper, Cubase, Bitwig, Live)
// calls AudioParameterFloat::getValueForText(); JUCE's default reads the number as a raw value, so "25 %" set 100 %.
// Every parser here accepts (a) exactly what the matching stringFromValue prints, (b) the bare number in DISPLAY units,
// case-insensitive, with spaces and units optional, and clamps to the range. Text with no number returns the fallback.
// Identical copy in every project that uses it.
#include <JuceHeader.h>

namespace svl::paramtext
{
// Reads the first number in the text (sign, digits, one decimal point or comma). rest = what follows it, lower-case, trimmed.
inline bool readNumber (const juce::String& text, double& value, juce::String& rest)
{
    const auto t = text.trim();
    int i = 0;
    const int n = t.length();
    while (i < n && ! (t[i] == '-' || t[i] == '+' || t[i] == '.' || t[i] == ',' || (t[i] >= '0' && t[i] <= '9'))) ++i;
    const int start = i;
    if (i < n && (t[i] == '-' || t[i] == '+')) ++i;
    bool digits = false;
    while (i < n && ((t[i] >= '0' && t[i] <= '9') || t[i] == '.' || t[i] == ','))
    {
        digits |= (t[i] >= '0' && t[i] <= '9');
        ++i;
    }
    if (! digits) return false;
    value = t.substring (start, i).replaceCharacter (',', '.').getDoubleValue();
    rest = t.substring (i).trim().toLowerCase();
    return true;
}

/** Percent: "25", "25 %", "25%" -> 0.25. Range may exceed 1 (Width 0..1.5 shows 0..150 %). */
inline juce::AudioParameterFloatAttributes percent (juce::AudioParameterFloatAttributes a, float lo, float hi, float fallback)
{
    return a.withValueFromStringFunction ([=] (const juce::String& s)
    {
        double v; juce::String rest;
        if (! readNumber (s, v, rest)) return fallback;
        return juce::jlimit (lo, hi, (float) (v / 100.0));
    });
}

/** Multiplier shown as "x1.80": "x1.8", "1.8", "X 1,8", "1.8x" -> 1.8. */
inline juce::AudioParameterFloatAttributes multiplier (juce::AudioParameterFloatAttributes a, float lo, float hi, float fallback)
{
    return a.withValueFromStringFunction ([=] (const juce::String& s)
    {
        double v; juce::String rest;
        if (! readNumber (s, v, rest)) return fallback;
        return juce::jlimit (lo, hi, (float) v);
    });
}

/** Milliseconds shown as "120 ms": "120", "120ms", "0.12 s" -> 120. */
inline juce::AudioParameterFloatAttributes milliseconds (juce::AudioParameterFloatAttributes a, float lo, float hi, float fallback)
{
    return a.withValueFromStringFunction ([=] (const juce::String& s)
    {
        double v; juce::String rest;
        if (! readNumber (s, v, rest)) return fallback;
        if (rest.startsWith ("s") || rest.startsWith ("sec")) v *= 1000.0;
        return juce::jlimit (lo, hi, (float) v);
    });
}

/** Frequency shown as "850 Hz" below 1 kHz and "6.0 kHz" above. Units win; a bare number is kHz when that lands inside the
    range ("6" = 6 kHz), otherwise Hz ("850" = 850 Hz). "6k" = 6 kHz. */
inline juce::AudioParameterFloatAttributes frequency (juce::AudioParameterFloatAttributes a, float lo, float hi, float fallback)
{
    return a.withValueFromStringFunction ([=] (const juce::String& s)
    {
        double v; juce::String rest;
        if (! readNumber (s, v, rest)) return fallback;
        double hz;
        if (rest.startsWith ("k"))        hz = v * 1000.0;
        else if (rest.startsWith ("h"))   hz = v;
        else                              hz = (v * 1000.0 >= lo && v * 1000.0 <= hi) ? v * 1000.0 : v;
        return juce::jlimit (lo, hi, (float) hz);
    });
}

/** Decibels shown as "+3.0 dB": "3", "-6", "-6 dB", "+3db" -> dB. */
inline juce::AudioParameterFloatAttributes decibels (juce::AudioParameterFloatAttributes a, float lo, float hi, float fallback)
{
    return a.withValueFromStringFunction ([=] (const juce::String& s)
    {
        double v; juce::String rest;
        if (! readNumber (s, v, rest)) return fallback;
        return juce::jlimit (lo, hi, (float) v);
    });
}
} // namespace svl::paramtext
