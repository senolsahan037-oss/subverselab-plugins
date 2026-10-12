#pragma once

#include <JuceHeader.h>

/*  How TUTKAL prints and reads its parameter values. One place, used by the parameters themselves (so Live's device
    view and automation lane show the same strings as the panel) and by the panel's type-in boxes.

      threshold  -18.0 dB       ratio  4.0 : 1        attack   10.0 ms / 0.10 ms     release  0.30 s
      makeup/depth  6.0 dB      mix/drive  100 %      peakReduction  5.0 (0 .. 10)   scHpf  OFF / 200 Hz
      pumpNote  1/8             pumpShape  EXP / LINEAR / SWELL

    parse() is strict: it returns false for text it cannot read (so a typo changes nothing), accepts a unit only when it
    is one that belongs to the parameter, and gives the plain value in the parameter's own range (not clamped).
*/
namespace tutkal::ptext
{
inline juce::String normalised (juce::String s)
{
    s = s.replace (juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92")), "-")    // U+2212 minus
         .replace (juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x93")), "-")    // en dash
         .replace (juce::String (juce::CharPointer_UTF8 ("\xc2\xa0")), " ");
    return s.trim();
}

/** First number in the text (sign, digits, one decimal point or comma); `unit` gets what follows, lower case, without blanks. */
inline bool firstNumber (const juce::String& raw, double& value, juce::String& unit)
{
    const auto s = normalised (raw);
    const int n = s.length();
    int i = 0;
    if (i < n && (s[i] == '-' || s[i] == '+')) ++i;
    const int digitsFrom = i;
    bool dot = false;
    int digits = 0;
    while (i < n && (juce::CharacterFunctions::isDigit (s[i]) || ((s[i] == '.' || s[i] == ',') && ! dot)))
    {
        if (s[i] == '.' || s[i] == ',') dot = true; else ++digits;
        ++i;
    }
    if (digits == 0) return false;
    value = s.substring (0, digitsFrom).getCharPointer()[0] == '-' ? -1.0 : 1.0;
    value *= s.substring (digitsFrom, i).replaceCharacter (',', '.').getDoubleValue();
    unit = s.substring (i).toLowerCase().removeCharacters (" \t");
    return true;
}

inline bool oneOf (const juce::String& u, std::initializer_list<const char*> list)
{
    for (const char* c : list) if (u == c) return true;
    return false;
}

inline juce::String fixed (double v, int decimals)
{
    if (std::abs (v) < 0.5 * std::pow (10.0, -decimals)) v = 0.0;   // never "-0.0"
    return juce::String (v, decimals);
}

inline juce::String percent (float v)
{
    return std::abs (v - std::round (v)) < 0.05f ? juce::String ((int) std::lround (v)) + " %" : fixed (v, 1) + " %";
}

inline juce::String hpf (float v)
{
    if (v < 20.0f) return "OFF";   // AUTO (0) and OFF are the same since 2026-10-12: no filter until a frequency is set
    return juce::String (juce::roundToInt (v)) + " Hz";
}

inline const char* noteName (int i)  { static const char* n[4] { "1/16", "1/8", "1/4", "1/2" }; return n[juce::jlimit (0, 3, i)]; }
inline const char* shapeName (int i) { static const char* n[3] { "EXP", "LINEAR", "SWELL" }; return n[juce::jlimit (0, 2, i)]; }

/** The readable text for a value in the parameter's own range (peakReduction: 0 .. 1 is printed 0 .. 10). */
inline juce::String format (const juce::String& id, float v)
{
    if (id == "threshold" || id == "makeup" || id == "depth") return fixed (v, 1) + " dB";
    if (id == "ratio")   return fixed (v, 1) + " : 1";
    if (id == "attack")  return fixed (v, v < 1.0f ? 2 : 1) + " ms";
    if (id == "release") return fixed (v / 1000.0f, 2) + " s";
    if (id == "mix" || id == "drive") return percent (v);
    if (id == "peakReduction") return fixed (v * 10.0f, 1);
    if (id == "scHpf")   return hpf (v);
    if (id == "pumpNote")  return noteName (juce::roundToInt (v));
    if (id == "pumpShape") return shapeName (juce::roundToInt (v));
    return fixed (v, 2);
}

/** Reads typed text for a numeric parameter. Returns false when the text is not a value for it. */
inline bool parse (const juce::String& id, const juce::String& text, float& plain)
{
    const auto t = normalised (text);
    if (t.isEmpty()) return false;
    if (id == "scHpf")
    {
        const auto l = t.toLowerCase();
        if (l.startsWith ("auto") || l == "a") { plain = 0.0f; return true; }
        if (l.startsWith ("off")) { plain = 0.0f; return true; }
    }
    double v = 0.0;
    juce::String u;
    if (! firstNumber (t, v, u)) return false;

    if (id == "threshold" || id == "makeup" || id == "depth")
    {
        if (! oneOf (u, { "", "db", "dbfs" })) return false;
        plain = (float) v;
    }
    else if (id == "ratio")
    {
        if (! oneOf (u, { "", ":1", ":", "x", ":1.0", "to1" })) return false;
        plain = (float) v;
    }
    else if (id == "attack")
    {
        if (oneOf (u, { "", "ms", "msec" })) plain = (float) v;
        else if (oneOf (u, { "s", "sec" })) plain = (float) (v * 1000.0);
        else return false;
    }
    else if (id == "release")
    {
        if (oneOf (u, { "ms", "msec" })) plain = (float) v;
        else if (oneOf (u, { "s", "sec" })) plain = (float) (v * 1000.0);
        else if (u.isEmpty()) plain = (float) (v <= 5.0 ? v * 1000.0 : v);   // a bare number: seconds up to 5, else milliseconds
        else return false;
    }
    else if (id == "mix" || id == "drive")
    {
        if (! oneOf (u, { "", "%", "pct", "percent" })) return false;
        plain = (float) v;
    }
    else if (id == "peakReduction")
    {
        if (! u.isEmpty()) return false;
        plain = (float) (v * 0.1);
    }
    else if (id == "scHpf")
    {
        if (oneOf (u, { "", "hz" })) plain = (float) v;
        else if (oneOf (u, { "khz", "k" })) plain = (float) (v * 1000.0);
        else return false;
        if (plain < 20.0f) plain = 10.0f;   // below 20 Hz there is no filter: OFF
    }
    else
        return false;
    return std::isfinite (plain);
}

/** Reads typed text for a choice parameter (pumpNote, pumpShape) into an index. */
inline bool parseChoice (const juce::String& id, const juce::String& text, int& index)
{
    const auto l = normalised (text).toLowerCase().removeCharacters (" \t");
    if (l.isEmpty()) return false;
    if (id == "pumpNote")
    {
        for (int i = 0; i < 4; ++i)
            if (l == juce::String (noteName (i))) { index = i; return true; }
        return false;
    }
    if (id == "pumpShape")
    {
        for (int i = 0; i < 3; ++i)
            if (juce::String (shapeName (i)).toLowerCase().startsWith (l)) { index = i; return true; }
        return false;
    }
    return false;
}
} // namespace tutkal::ptext
