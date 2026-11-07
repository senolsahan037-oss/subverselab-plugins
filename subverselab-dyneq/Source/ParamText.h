#pragma once

#include <JuceHeader.h>

/*  How DYNEQ prints and reads its parameter values. One place, used by the parameters themselves (so a host's device view
    and automation lane show the same strings as the panel) and by the panel's type-in boxes. The unit is inside the text,
    so the parameters carry no label (a host would print it twice).

      depth / margin / gain / thr / range   6.0 dB        focusLow / focusHigh / freq   120 Hz, 2.40 kHz
      mix   100 %        att / rel   10.0 ms, 0.10 s      q   1.00
      choices: mode COLLIDE / SELF, speed FAST / MEDIUM / SLOW, type BELL ... HIGH CUT, slope 12 / 24 / 48 / 36 / 72 dB/oct, trig SELF / KEY,
               targetInst OFF / VOCAL / BASS / DRUMS / KICK / SNARE / HATS, targetSlope 12 / 24 / 36 / 48 / 72 dB/oct      targetAmount 9.0 dB, targetPart 50 %, targetWidth 1.00, targetSweet 120 ms

    A band parameter id is "b<1..6>_<name>"; the name decides the kind. parse() is strict: it returns false for text it cannot
    read (a typo changes nothing), accepts a unit only when it belongs to the parameter, and gives the plain value (not clamped).
*/
namespace dyneq::ptext
{
enum class Kind { None, Db, Hz, Percent, Ms, Q };

inline juce::String nameOf (const juce::String& id)
{
    return id.length() > 3 && id[0] == 'b' && id[2] == '_' ? id.substring (3) : id;
}

inline Kind kindOf (const juce::String& id)
{
    const auto n = nameOf (id);
    if (n == "depth" || n == "margin" || n == "gain" || n == "thr" || n == "range" || n == "targetAmount" || n == "output") return Kind::Db;
    if (n == "focusLow" || n == "focusHigh" || n == "freq") return Kind::Hz;
    if (n == "mix" || n == "targetPart") return Kind::Percent;
    if (n == "att" || n == "rel" || n == "targetSweet") return Kind::Ms;
    if (n == "q" || n == "targetWidth") return Kind::Q;
    return Kind::None;
}

inline juce::StringArray choiceNames (const juce::String& id)
{
    const auto n = nameOf (id);
    if (n == "mode")  return { "COLLIDE", "SELF" };
    if (n == "speed") return { "FAST", "MEDIUM", "SLOW" };
    if (n == "type")  return { "BELL", "LOW SHELF", "HIGH SHELF", "NOTCH", "LOW CUT", "HIGH CUT" };
    if (n == "slope") return { "12 dB/oct", "24 dB/oct", "48 dB/oct", "36 dB/oct", "72 dB/oct" };   // the first three are the old choices (saved states load unchanged); 36 and 72 are appended
    if (n == "trig")  return { "SELF", "KEY" };
    if (n == "analyzer") return { "OFF", "PRE", "POST", "PRE+POST" };
    if (n == "targetInst") return { "OFF", "VOCAL", "BASS", "DRUMS", "KICK", "SNARE", "HATS" };
    if (n == "targetSlope") return { "12 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct", "72 dB/oct" };
    return {};
}

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

inline juce::String hz (float v)
{
    if (v >= 999.5f) return fixed (v / 1000.0f, 2) + " kHz";
    return v < 100.0f ? fixed (v, 1) + " Hz" : juce::String (juce::roundToInt (v)) + " Hz";
}

inline juce::String percent (float v)
{
    return std::abs (v - std::round (v)) < 0.05f ? juce::String ((int) std::lround (v)) + " %" : fixed (v, 1) + " %";
}

/** The readable text for a value in the parameter's own range (choices: the index). */
inline juce::String format (const juce::String& id, float v)
{
    switch (kindOf (id))
    {
        case Kind::Db:      return fixed (v, 1) + " dB";
        case Kind::Hz:      return hz (v);
        case Kind::Percent: return percent (v);
        case Kind::Ms:      return v >= 1000.0f ? fixed (v / 1000.0f, 2) + " s" : v >= 100.0f ? juce::String (juce::roundToInt (v)) + " ms" : fixed (v, 1) + " ms";
        case Kind::Q:       return fixed (v, 2);
        case Kind::None:    break;
    }
    const auto names = choiceNames (id);
    if (names.size() > 0) return names[juce::jlimit (0, names.size() - 1, juce::roundToInt (v))];
    return fixed (v, 2);
}

/** Reads typed text for a numeric parameter. Returns false when the text is not a value for it. */
inline bool parse (const juce::String& id, const juce::String& text, float& plain)
{
    const auto t = normalised (text);
    if (t.isEmpty()) return false;
    double v = 0.0;
    juce::String u;
    if (! firstNumber (t, v, u)) return false;
    switch (kindOf (id))
    {
        case Kind::Db:      if (! oneOf (u, { "", "db", "dbfs" })) return false; plain = (float) v; break;
        case Kind::Hz:
            if (oneOf (u, { "", "hz" })) plain = (float) v;
            else if (oneOf (u, { "khz", "k" })) plain = (float) (v * 1000.0);
            else return false;
            break;
        case Kind::Percent: if (! oneOf (u, { "", "%", "pct", "percent" })) return false; plain = (float) v; break;
        case Kind::Ms:
            if (oneOf (u, { "ms", "msec" })) plain = (float) v;
            else if (oneOf (u, { "s", "sec" })) plain = (float) (v * 1000.0);
            else if (u.isEmpty()) plain = (float) v;
            else return false;
            break;
        case Kind::Q:       if (! u.isEmpty()) return false; plain = (float) v; break;
        case Kind::None:    return false;
    }
    return std::isfinite (plain);
}

/** Reads typed text for a choice parameter into an index (a unique prefix is enough; "12" / "24" / "48" pick a slope). */
inline bool parseChoice (const juce::String& id, const juce::String& text, int& index)
{
    const auto l = normalised (text).toLowerCase().removeCharacters (" \t");
    if (l.isEmpty()) return false;
    const auto names = choiceNames (id);
    for (int i = 0; i < names.size(); ++i)
        if (names[i].toLowerCase().removeCharacters (" \t") == l) { index = i; return true; }
    int hit = -1, hits = 0;
    for (int i = 0; i < names.size(); ++i)
        if (names[i].toLowerCase().removeCharacters (" \t").startsWith (l)) { hit = i; ++hits; }
    if (hits == 1) { index = hit; return true; }
    return false;
}
} // namespace dyneq::ptext
