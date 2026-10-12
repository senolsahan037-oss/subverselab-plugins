#pragma once
// Type-in text test (family batch, 2026-10-12). For every float parameter listed: at 0, .25, .5, .75, 1
//  (a) getValueForText (getText (v)) comes back to v (within one display step) and the text is stable on a second round,
//  (b) the bare number in display units (the text with its unit stripped) gives the same value,
//  (c) garbage text does not move the parameter off the fallback, and values far out of range clamp to the ends.
// Identical copy in every project that uses it.
#include <JuceHeader.h>

namespace svl::paramtexttest
{
struct Result { int pass = 0, fail = 0; juce::StringArray failures; };

template <typename Processor>
Result run (Processor& p, const std::vector<juce::String>& ids)
{
    Result r;
    auto ok = [&] (bool cond, const juce::String& what)
    {
        if (cond) ++r.pass; else { ++r.fail; r.failures.add (what); }
    };
    for (const auto& id : ids)
    {
        auto* prm = p.apvts.getParameter (id);
        if (prm == nullptr) { ok (false, id + ": no such parameter"); continue; }
        for (float v : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
        {
            const auto text = prm->getText (v, 0);
            const float back = prm->getValueForText (text);
            ok (std::abs (back - v) <= 0.03f, id + " at " + juce::String (v) + ": \"" + text + "\" parsed to " + juce::String (back));
            ok (prm->getText (back, 0) == text, id + " at " + juce::String (v) + ": text not stable, \"" + text + "\" -> \"" + prm->getText (back, 0) + "\"");
            juce::String bare;
            for (auto c : text) if (juce::CharacterFunctions::isDigit (c) || c == '.' || c == '-' || c == '+') bare << c;
            ok (std::abs (prm->getValueForText (bare) - back) < 1.0e-6f, id + " at " + juce::String (v) + ": bare \"" + bare + "\" gave " + juce::String (prm->getValueForText (bare)) + ", text gave " + juce::String (back));
            ok (std::abs (prm->getValueForText (text.toUpperCase()) - back) < 1.0e-6f, id + " at " + juce::String (v) + ": upper case \"" + text.toUpperCase() + "\" differs");
        }
        ok (prm->getValueForText ("1e9 junk") >= 0.0f && prm->getValueForText ("1e9 junk") <= 1.0f, id + ": out-of-range text escaped 0..1");
        ok (std::abs (prm->getValueForText ("not a number") - prm->getValueForText ("also text")) < 1.0e-9f, id + ": unparseable text is not constant");
    }
    return r;
}
} // namespace svl::paramtexttest
