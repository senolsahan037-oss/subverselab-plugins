#pragma once

#include <JuceHeader.h>
#include "SedaEngine.h"

/*  How SEDA prints and reads its parameter values: one place, used by the parameters themselves (so a host's device view and automation
    lane show the same strings as the panel) and by the panel's type-in boxes. The unit is inside the text, so the parameters carry no
    label (a host would print it twice).

      time 300.0 ms (typed "0.3 s" also reads)     feedback / mix / depth / comp / duck   30.0 %     tone   +25.0 %  (-100 .. +100)
      rate 0.50 Hz      width 4.0 ms      throw 6.0 dB      wetHp OFF / 250 Hz

    parse() is strict: it returns false for text it cannot read (a typo changes nothing) and gives the plain value, not clamped.
*/
namespace seda::ptext
{
inline juce::String fixed (float v, int decimals)   // never "-0.0"
{
    auto s = juce::String (v, decimals);
    if (s.containsOnly ("-0.")) s = s.replace ("-", "");
    return s;
}

enum class Kind { None, Ms, Percent, SignedPercent, Hz, Db, WetHp, Note };

inline Kind kindOf (const juce::String& id)
{
    if (id == "time" || id == "width") return Kind::Ms;
    if (id == "feedback" || id == "mix" || id == "depth" || id == "comp" || id == "duck" || id == "runaway" || id == "chaos") return Kind::Percent;
    if (id == "tone") return Kind::SignedPercent;
    if (id == "rate") return Kind::Hz;
    if (id == "throw") return Kind::Db;
    if (id == "wetHp") return Kind::WetHp;
    if (id == "note") return Kind::Note;
    return Kind::None;
}

inline juce::String format (const juce::String& id, float v)
{
    switch (kindOf (id))
    {
        case Kind::Ms:            return fixed (v, 1) + " ms";
        case Kind::Percent:       return fixed (v, 1) + " %";
        case Kind::SignedPercent: return (v > 0.04f ? "+" : "") + fixed (v, 1) + " %";
        case Kind::Hz:            return fixed (v, 2) + " Hz";
        case Kind::Db:            return fixed (v, 1) + " dB";
        case Kind::WetHp:         return v < 0.5f ? juce::String ("OFF") : juce::String (juce::roundToInt (v)) + " Hz";
        case Kind::Note:          return juce::String (seda::noteName (juce::roundToInt (v)));
        case Kind::None:          break;
    }
    return fixed (v, 2);
}

/** First number in the text and what follows it (lower case, no blanks). */
inline bool firstNumber (const juce::String& raw, double& value, juce::String& unit)
{
    auto s = raw.replace (juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92")), "-").replace (juce::String (juce::CharPointer_UTF8 ("\xc2\xa0")), " ").trim();
    int i = 0;
    if (i < s.length() && (s[i] == '-' || s[i] == '+')) ++i;
    const int digitsFrom = i;
    bool dot = false;
    while (i < s.length() && (juce::CharacterFunctions::isDigit (s[i]) || ((s[i] == '.' || s[i] == ',') && ! dot))) { dot = dot || s[i] == '.' || s[i] == ','; ++i; }
    if (i == digitsFrom || ! juce::CharacterFunctions::isDigit (s[digitsFrom])) return false;
    value = s.substring (0, i).replaceCharacter (',', '.').getDoubleValue();
    unit = s.substring (i).removeCharacters (" \t").toLowerCase();
    return true;
}

inline bool parse (const juce::String& id, const juce::String& text, float& out)
{
    const auto k = kindOf (id);
    if (k == Kind::None) return false;
    if (k == Kind::WetHp && text.trim().equalsIgnoreCase ("off")) { out = 0.0f; return true; }
    if (k == Kind::Note)
    {   // "1/32", "1/8t" (triplet), "1/4d" or "1/4." (dotted); blanks and a leading "AUTO" (the window's own text while AUTO is on) are ignored
        auto t = text.toLowerCase().removeCharacters (" \t");
        if (t.startsWith ("auto")) t = t.substring (4);
        if (! t.startsWith ("1/")) return false;
        t = t.substring (2);
        int n = 0, i = 0;
        while (i < t.length() && juce::CharacterFunctions::isDigit (t[i]) && n < 1000) n = n * 10 + (t[i++] - '0');
        const auto suffix = t.substring (i);
        int base = -1;
        if (n == 32) base = 0; else if (n == 16) base = 1; else if (n == 8) base = 2; else if (n == 4) base = 3; else if (n == 2) base = 4; else if (n == 1) base = 5;
        int kind = -1;
        if (suffix.isEmpty()) kind = 0; else if (suffix == "t") kind = 1; else if (suffix == "d" || suffix == ".") kind = 2;
        if (base < 0 || kind < 0) return false;
        out = (float) (3 * base + kind);
        return true;
    }
    double v; juce::String u;
    if (! firstNumber (text, v, u)) return false;
    switch (k)
    {
        case Kind::Ms:            if (u == "s") v *= 1000.0; else if (! (u.isEmpty() || u == "ms")) return false; break;
        case Kind::Percent:
        case Kind::SignedPercent: if (! (u.isEmpty() || u == "%")) return false; break;
        case Kind::Hz:            if (! (u.isEmpty() || u == "hz")) return false; break;
        case Kind::Db:            if (! (u.isEmpty() || u == "db")) return false; break;
        case Kind::WetHp:         if (! (u.isEmpty() || u == "hz")) return false; break;
        case Kind::Note:
        case Kind::None:          return false;
    }
    out = (float) v;
    return true;
}
}
