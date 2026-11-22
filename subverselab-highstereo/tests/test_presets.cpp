// Preset checks against the real processor (headless). Uses a temporary folder, never the user's preset folder.
#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"
#include <cstdio>
#include <functional>
#include <algorithm>

static int fails = 0, checks = 0;
static void check (bool ok, const char* what, double v)
{
    ++checks; if (! ok) ++fails;
    std::printf ("[%s] %-60s %.4f\n", ok ? "PASS" : "FAIL", what, v);
}

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("hs_preset_test_" + juce::String (juce::Random::getSystemRandom().nextInt()));
    {
        HighStereoAudioProcessor proc;
        proc.presets.setDirectory (dir);   // never the user's real preset folder
        PresetManager& pm = proc.presets;
        auto set = [&] (const char* id, float v) { auto* p = proc.apvts.getParameter (id); p->setValueNotifyingHost (p->convertTo0to1 (v)); };
        auto get = [&] (const char* id) { return proc.apvts.getRawParameterValue (id)->load(); };

        set ("width", 150); set ("spread", 40); set ("band4", 160); set ("fix", 1); set ("mix", 70);
        check (pm.save ("Wide test"), "save a user preset", 1);
        check (! pm.save ("Default"), "factory name cannot be overwritten", 1);
        check (! pm.save ("bad/name"), "path characters refused", 1);
        check (! pm.save ("   "), "empty name refused", 1);

        pm.loadDefault();
        check (get ("width") == 100 && get ("spread") == 0 && get ("band4") == 100 && get ("fix") < 0.5f && get ("mix") == 100,
               "Default resets every parameter", 1);
        check (pm.load ("Wide test"), "load the user preset", 1);
        check (get ("width") == 150 && get ("spread") == 40 && get ("band4") == 160 && get ("fix") > 0.5f && get ("mix") == 70,
               "loaded values equal saved values", 1);

        // No limit: 500 presets.
        for (int i = 0; i < 500; ++i) { set ("width", (float) (i % 200)); pm.save ("Bulk " + juce::String (i)); }
        check (pm.userNames().size() == 501, "500 more presets saved and listed (count)", pm.userNames().size());
        check (pm.load ("Bulk 123") && get ("width") == 123, "Bulk 123 loads width 123", get ("width"));
        check (pm.remove ("Bulk 123") && ! pm.exists ("Bulk 123") && pm.userNames().size() == 500, "delete one (count after)", pm.userNames().size());
        check (! pm.remove ("Default"), "Default cannot be deleted", 1);

        // Session state keeps the preset name and never brings LISTEN back on.
        pm.load ("Wide test");
        proc.setListen (svl::HighStereoEngine::Listen::Phone);
        juce::MemoryBlock mb; proc.getStateInformation (mb);
        HighStereoAudioProcessor proc2;
        proc2.setListen (svl::HighStereoEngine::Listen::Mono);
        proc2.setStateInformation (mb.getData(), (int) mb.getSize());
        check (proc2.presets.currentName() == "Wide test", "session reopen shows the preset name", 1);
        check (proc2.getListen() == svl::HighStereoEngine::Listen::Off, "session reopen: LISTEN back to STEREO", 1);
        check (proc2.apvts.getRawParameterValue ("band4")->load() == 160, "session reopen: band width restored", 1);

        // A preset is one undo step.
        pm.loadDefault();
        proc.undo.clear();
        proc.loadPreset ("Wide test");
        proc.undo.undo();
        check (get ("width") == 100 && get ("band4") == 100, "one Cmd+Z undoes a whole preset load", get ("width"));

        // Factory presets (owner 21 Nov): the six that passed the measurement, with their OUTPUT trims.
        // The values in code must be exactly the measured ones (logs/preset_measure/factory_presets.json).
        check (pm.factoryNames().size() == 6 && pm.factoryNames()[0] == "Default", "six factory presets, Default first", pm.factoryNames().size());
        const auto jf = juce::File (__FILE__).getParentDirectory().getParentDirectory().getChildFile ("logs/preset_measure/factory_presets.json");
        const auto js = juce::JSON::parse (jf);
        check (js.isObject(), "measured preset file found", jf.existsAsFile());
        static const std::map<juce::String, juce::String> toParam { { "b1", "band1" }, { "b2", "band2" }, { "b3", "band3" }, { "b4", "band4" },
                                                                    { "b5", "band5" }, { "b6", "band6" }, { "b7", "band7" }, { "b8", "band8" } };
        for (auto& name : pm.factoryNames())
        {
            set ("width", 7); set ("stage", 33); set ("mix", 12);   // must all go back to default / preset value
            const bool ok = pm.load (name);
            bool same = ok && js.hasProperty (juce::Identifier (name));
            if (auto* obj = js[juce::Identifier (name)].getDynamicObject())
            {
                std::map<juce::String, float> want;
                for (auto& pr : obj->getProperties()) want[toParam.count (pr.name.toString()) ? toParam.at (pr.name.toString()) : pr.name.toString()] = (float) (double) pr.value;
                for (auto* p : proc.getParameters())
                    if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
                    {
                        const float def = rp->convertFrom0to1 (rp->getDefaultValue());
                        const float w = want.count (rp->paramID) ? want[rp->paramID] : def;
                        same = same && std::abs (get (rp->paramID.toRawUTF8()) - w) < 0.051f;
                        same = same && std::abs (pm.valueAtLoad (rp->paramID, -999.0f) - w) < 0.051f;   // double-click target
                    }
            }
            check (same, ("factory " + name + " = measured values, double-click targets too").toRawUTF8(), 1);
            check (! pm.save (name) && ! pm.remove (name), ("factory " + name + " cannot be overwritten or deleted").toRawUTF8(), 1);
        }
        pm.load ("Default");
        check (pm.neighbour (+1) == "Mono Safe Wide" && pm.neighbour (-1) == pm.userNames()[pm.userNames().size() - 1],
               "next / previous walk factory then user presets", 1);
        pm.load ("Wide Master"); pm.refreshLoaded();
        check (std::abs (pm.valueAtLoad ("match", 0) - 1.0f) < 0.01f && std::abs (pm.valueAtLoad ("band8", 0) - 150.0f) < 0.01f,
               "session reopen on a factory preset keeps its double-click values", pm.valueAtLoad ("band8", 0));
        pm.load ("Mono Safe Wide"); pm.refreshLoaded();
        check (std::abs (pm.valueAtLoad ("output", 0) + 2.0f) < 0.01f, "Mono Safe Wide reopens with its -2 dB trim", pm.valueAtLoad ("output", 0));
    }
    // Default is the untouched path through the real processBlock (host 0..1 float round trip included).
    // 21 Nov: OUTPUT read back 1.8e-7 dB and STAGE 2.7e-6 deg on Default, so the engine processed and the guard acted.
    {
        auto runCase = [] (const char* what, std::function<void (HighStereoAudioProcessor&)> setup, bool expectSame, const juce::File& d)
        {
            HighStereoAudioProcessor proc;
            proc.presets.setDirectory (d);
            proc.setPlayConfigDetails (2, 2, 48000.0, 512);
            proc.prepareToPlay (48000.0, 512);
            setup (proc);
            juce::Random rng (1234);
            juce::AudioBuffer<float> buf (2, 512);
            juce::MidiBuffer midi;
            double maxDiff = 0; long differing = 0;
            for (int b = 0; b < 400; ++b)   // 4.3 s of hot, wide, transient material (peaks at 0 dBFS)
            {
                juce::AudioBuffer<float> ref (2, 512);
                for (int i = 0; i < 512; ++i)
                {
                    const int t = b * 512 + i;
                    const float hit = (t % 12000) < 400 ? 1.0f : 0.25f;
                    const float l = hit * (rng.nextFloat() * 2.0f - 1.0f), r = hit * (rng.nextFloat() * 2.0f - 1.0f);
                    ref.setSample (0, i, std::clamp (l, -1.0f, 1.0f)); ref.setSample (1, i, std::clamp (0.3f * l + 0.7f * r, -1.0f, 1.0f));
                }
                buf.makeCopyOf (ref);
                proc.processBlock (buf, midi);
                for (int c = 0; c < 2; ++c)
                    for (int i = 0; i < 512; ++i)
                    {
                        const double dlt = std::abs ((double) buf.getSample (c, i) - (double) ref.getSample (c, i));
                        if (dlt != 0.0) { ++differing; maxDiff = std::max (maxDiff, dlt); }
                    }
            }
            const double diffDb = maxDiff > 0 ? 20.0 * std::log10 (maxDiff) : -999.0;
            check (expectSame ? differing == 0 : differing > 0, what, expectSame ? (double) differing : diffDb);
        };
        auto setP = [] (HighStereoAudioProcessor& p, const char* id, float v) { auto* q = p.apvts.getParameter (id); q->setValueNotifyingHost (q->convertTo0to1 (v)); };
        runCase ("processBlock: fresh Default bit-identical (samples differing)", [] (HighStereoAudioProcessor&) {}, true, dir);
        runCase ("processBlock: Default after Wide Master bit-identical", [] (HighStereoAudioProcessor& p) { p.presets.load ("Wide Master"); p.presets.load ("Default"); }, true, dir);
        runCase ("processBlock: host sets OUTPUT 0 / STAGE 0 / WIDTH 100 bit-identical",
                 [&] (HighStereoAudioProcessor& p) { setP (p, "output", 3); setP (p, "stage", 40); setP (p, "width", 150);
                                                     setP (p, "output", 0); setP (p, "stage", 0); setP (p, "width", 100); }, true, dir);
        runCase ("processBlock: session state of Default bit-identical",
                 [&] (HighStereoAudioProcessor& p) { HighStereoAudioProcessor a; a.presets.setDirectory (dir); juce::MemoryBlock mb; a.getStateInformation (mb);
                                                     p.setStateInformation (mb.getData(), (int) mb.getSize()); }, true, dir);
        runCase ("processBlock: STAGE 1 deg still processes (max diff dB)", [&] (HighStereoAudioProcessor& p) { setP (p, "stage", 1); }, false, dir);
        runCase ("processBlock: OUTPUT -0.1 dB still processes (max diff dB)", [&] (HighStereoAudioProcessor& p) { setP (p, "output", -0.1f); }, false, dir);
        runCase ("processBlock: WIDTH 101 still processes (max diff dB)", [&] (HighStereoAudioProcessor& p) { setP (p, "width", 101); }, false, dir);
    }
    dir.deleteRecursively();
    std::printf ("\n%d checks, %d failed\n", checks, fails);
    return fails == 0 ? 0 : 1;
}
