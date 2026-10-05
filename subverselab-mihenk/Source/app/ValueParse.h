#pragma once

#include <JuceHeader.h>

/*  Typed knob values: the text the panel prints ("-14.0 LUFS", "3.50 KHZ", "Q 1.20", "2.00:1", "70 % RMS", "OFF")
    and what people type ("3k5" is not accepted, "3.5k", "3.5 kHz", "+1", "-0.5db", "1,5" are). The number is in the
    displayed unit; "k" multiplies by 1000; "off" means the minimum. The result is clamped to [minimum, maximum].
    Returns false when there is no number in the text. */
namespace mihenk
{
inline bool parseKnobText (const juce::String& text, double minimum, double maximum, double& value)
{
    const auto t = text.trim().toLowerCase().replaceCharacter (',', '.');
    if (t == "off" || t == "-inf") { value = minimum; return true; }
    int i = 0;
    const int n = t.length();
    while (i < n && ! (juce::CharacterFunctions::isDigit (t[i]) || ((t[i] == '-' || t[i] == '+' || t[i] == '.') && i + 1 < n
                                                                     && (juce::CharacterFunctions::isDigit (t[i + 1]) || t[i + 1] == '.'))))
        ++i;
    if (i >= n) return false;
    int j = i + 1;
    while (j < n && (juce::CharacterFunctions::isDigit (t[j]) || t[j] == '.')) ++j;
    double v = t.substring (i, j).getDoubleValue();
    const auto rest = t.substring (j).trim();
    if (rest.startsWithChar ('k')) v *= 1000.0;
    if (! std::isfinite (v)) return false;
    value = juce::jlimit (minimum, maximum, v);
    return true;
}
}
