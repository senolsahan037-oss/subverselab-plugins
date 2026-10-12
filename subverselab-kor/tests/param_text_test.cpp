#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"
#include "SvlParamTextTests.h"
#include <cstdio>

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    int fails = 0;
    KorAudioProcessor p;
    const auto t = svl::paramtexttest::run (p, { "low", "mid", "high", "mix" });
    std::printf ("[%s] typein_all_float_params: %d of %d type-in checks passed\n", t.fail == 0 ? "PASS" : "FAIL", t.pass, t.pass + t.fail);
    for (auto& f : t.failures) std::printf ("   %s\n", f.toRawUTF8());
    fails += t.fail;

    struct Case { const char* id; const char* text; float want; };
    const Case cases[] = { { "low", "25 %", 0.25f }, { "low", "25", 0.25f }, { "mid", "50%", 0.5f }, { "high", "100", 1.0f }, { "mix", "75 %", 0.75f },
                           { "mix", "0", 0.0f }, { "low", "250", 1.0f }, { "high", "-5", 0.0f }, { "mid", "12,5", 0.125f }, { "mix", "20 PERCENT", 0.20f } };
    for (auto& c : cases)
    {
        const float got = p.apvts.getParameter (c.id)->getValueForText (c.text);
        const bool ok = std::abs (got - c.want) < 1e-5f;
        std::printf ("[%s] %s \"%s\" -> %.4f (want %.4f)\n", ok ? "PASS" : "FAIL", c.id, c.text, got, c.want);
        if (! ok) ++fails;
    }
    return fails;
}
