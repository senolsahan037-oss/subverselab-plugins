// host_check: loads the INSTALLED (shipped) TUTKAL binary the way a DAW does and turns every front knob end to end.
// Owner 2026-10-12, in Live: "threshold, ratio, release, makeup, LA, PUMP: nothing works, every knob is empty".
// plugin_check tests the processor compiled from Source/; this tests the bundle a buyer has. Read-only: nothing is written
// to ~/Library/Audio/Plug-Ins.
//
//   host_check [path to .vst3 or .component]    (default: both installed bundles)
#include <JuceHeader.h>
#include <cstdio>

static int fails = 0;
static void check (bool ok, const juce::String& what, const juce::String& detail)
{
    if (! ok) ++fails;
    std::printf ("[%s] %s  %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8(), detail.toRawUTF8());
}

static juce::AudioProcessorParameter* findParam (juce::AudioPluginInstance& p, const juce::String& name)
{
    for (auto* q : p.getParameters())
        if (q->getName (64).equalsIgnoreCase (name)) return q;
    return nullptr;
}

static bool setText (juce::AudioPluginInstance& p, const juce::String& name, const juce::String& text)
{
    auto* q = findParam (p, name);
    if (q == nullptr) return false;
    q->setValueNotifyingHost (q->getValueForText (text));
    return true;
}

struct Run { double outDb = -200.0; double keyedOutDb = -200.0; };

/** 3 s of programme (and key), 512-sample blocks, returns the RMS of the last second of the main output in dBFS. */
static double render (juce::AudioPluginInstance& p, bool sidechain, int prog, int keyKind, double sr = 48000.0)
{
    const int block = 512, n = (int) (3.0 * sr);
    const int mainCh = 2, scCh = sidechain ? 2 : 0;
    juce::AudioBuffer<float> buf (juce::jmax (mainCh + scCh, p.getTotalNumInputChannels(), p.getTotalNumOutputChannels()), block);
    juce::MidiBuffer midi;
    double acc = 0.0; long cnt = 0;
    for (int pos = 0; pos < n; pos += block)
    {
        buf.clear();
        for (int i = 0; i < block; ++i)
        {
            const double t = (pos + i) / sr;
            float x = 0.0f;
            if (prog == 0) x = (float) (0.25 * std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * t));                       // steady -12 dBFS
            else           x = (float) ((std::fmod (t, 0.5) < 0.15 ? 0.5 : 0.03) * std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * t));   // bursts
            buf.setSample (0, i, x); buf.setSample (1, i, x);
            if (scCh > 0 && keyKind > 0)
            {
                const double ph = std::fmod (t, 0.5);
                const float k = (float) (0.8 * std::exp (-ph / 0.08) * std::sin (2.0 * juce::MathConstants<double>::pi * 55.0 * ph));   // a kick every 0.5 s, peak ~ -2 dBFS
                buf.setSample (2, i, k); buf.setSample (3, i, k);
            }
        }
        p.processBlock (buf, midi);
        if (pos >= n - (int) sr)
            for (int i = 0; i < block; ++i) { const double v = buf.getSample (0, i); acc += v * v; ++cnt; }
    }
    return 10.0 * std::log10 (juce::jmax (1e-20, acc / (double) juce::jmax (1L, cnt)));
}

static void testOne (juce::AudioPluginFormatManager& fm, const juce::File& file)
{
    juce::OwnedArray<juce::PluginDescription> types;
    for (auto* f : fm.getFormats())
        if (f->fileMightContainThisPluginType (file.getFullPathName()))
            f->findAllTypesForFile (types, file.getFullPathName());
    if (types.isEmpty()) { check (false, "load " + file.getFileName(), "no plug-in found in the bundle"); return; }
    for (auto* d : types)
    {
        juce::String err;
        auto inst = fm.createPluginInstance (*d, 48000.0, 512, err);
        check (inst != nullptr, "load " + d->pluginFormatName + " " + d->name + " " + d->version, err);
        if (auto* want = std::getenv ("HOST_CHECK_VERSION"))
            check (d->version == juce::String (want), "version " + d->pluginFormatName, d->version + " (want " + juce::String (want) + ")");
        if (inst == nullptr) continue;
        auto& p = *inst;

        for (bool sc : { true, false })
        {
            // Live's layout: stereo main in/out, stereo sidechain enabled (or no sidechain).
            auto layout = p.getBusesLayout();
            if (layout.inputBuses.size() > 1) layout.inputBuses.getReference (1) = sc ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::disabled();
            const bool okLayout = p.setBusesLayout (layout);
            p.prepareToPlay (48000.0, 512);
            const juce::String tag = d->pluginFormatName + (sc ? " sidechain on" : " sidechain off") + ": ";
            check (okLayout, tag + "bus layout accepted", juce::String (p.getTotalNumInputChannels()) + " in / " + juce::String (p.getTotalNumOutputChannels()) + " out");

            auto reset = [&]
            {
                setText (p, "VOICING", "GLUE"); setText (p, "THRESHOLD", "-18"); setText (p, "RATIO", "4"); setText (p, "ATTACK", "10");
                setText (p, "RELEASE", "0.3 s"); setText (p, "AUTO RELEASE", "0"); setText (p, "MAKEUP", "0"); setText (p, "MIX", "100");
                setText (p, "EXT", "0"); setText (p, "DRIVE", "0"); setText (p, "IN", "1");
            };
            auto sweep = [&] (const juce::String& what, std::function<void()> pre, const juce::String& name, const juce::String& a, const juce::String& b,
                              int prog, int key, double wantAbs)
            {
                reset(); p.prepareToPlay (48000.0, 512); if (pre) pre();   // a fresh instance state per sweep (KEY AUTO holds for 4 s)
                const bool found = setText (p, name, a); const double ya = render (p, sc, prog, key);
                setText (p, name, b); const double yb = render (p, sc, prog, key);
                check (found && std::abs (yb - ya) >= wantAbs, tag + what + ": " + name + " " + a + " -> " + b,
                       juce::String::formatted ("output %.2f -> %.2f dBFS (moves %+.2f dB, want >= %.1f)", ya, yb, yb - ya, wantAbs));
            };
            reset();
            const double dry = render (p, sc, 0, 0);
            setText (p, "IN", "0"); const double byp = render (p, sc, 0, 0); setText (p, "IN", "1");
            check (true, tag + "steady -12 dBFS tone", juce::String::formatted ("GLUE defaults %.2f dBFS, IN off %.2f dBFS", dry, byp));
            sweep ("GLUE", nullptr, "MAKEUP", "0", "12", 0, 0, 11.0);
            sweep ("GLUE", nullptr, "THRESHOLD", "0", "-40", 0, 0, 6.0);
            sweep ("GLUE", [&] { setText (p, "THRESHOLD", "-35"); }, "RATIO", "1.5", "20", 0, 0, 6.0);
            sweep ("GLUE", [&] { setText (p, "THRESHOLD", "-30"); }, "RELEASE", "0.1 s", "1.2 s", 1, 0, 1.0);
            sweep ("GLUE", [&] { setText (p, "THRESHOLD", "-30"); }, "MIX", "100", "0", 0, 0, 3.0);
            sweep ("LA", [&] { setText (p, "VOICING", "LA"); }, "PEAK REDUCTION", "0", "10", 0, 0, 3.0);
            sweep ("LA", [&] { setText (p, "VOICING", "LA"); }, "MAKEUP", "0", "12", 0, 0, 11.0);
            if (sc)
                sweep ("PUMP keyed by a kick", [&] { setText (p, "VOICING", "PUMP"); setText (p, "EXT", "1"); setText (p, "THRESHOLD", "-30"); },
                       "DEPTH", "0", "24", 0, 1, 3.0);
            sweep ("PUMP self-keyed", [&] { setText (p, "VOICING", "PUMP"); setText (p, "THRESHOLD", "-30"); }, "DEPTH", "0", "24", 1, 0, 3.0);
            if (sc)   // KEY AUTO (1.0.3): the switch left on AUTO, a kick on the sidechain -> PUMP ducks the steady tone on the kick
                sweep ("PUMP, KEY on AUTO, kick on the sidechain", [&] { setText (p, "VOICING", "PUMP"); setText (p, "EXT", "0"); setText (p, "THRESHOLD", "-30"); },
                       "DEPTH", "0", "24", 0, 1, 3.0);
            if (sc)   // the owner's Live case: GLUE, kick keyed, -18 / 4:1 / 0.1 ms / 0.3 s
                sweep ("GLUE keyed by a kick (KEY AUTO)", [&] { setText (p, "ATTACK", "0.1"); }, "THRESHOLD", "0", "-18", 0, 1, 3.0);
            p.releaseResources();
        }
    }
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    juce::AudioPluginFormatManager fm;
    juce::addDefaultFormatsToManager (fm);
    juce::Array<juce::File> files;
    if (argc > 1) files.add (juce::File (argv[1]));
    else
    {
        const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
        files.add (home.getChildFile ("Library/Audio/Plug-Ins/VST3/TUTKAL.vst3"));
        files.add (home.getChildFile ("Library/Audio/Plug-Ins/Components/TUTKAL.component"));
    }
    for (auto& f : files) testOne (fm, f);
    std::printf ("\nhost_check: %d failed\n", fails);
    return fails;
}
