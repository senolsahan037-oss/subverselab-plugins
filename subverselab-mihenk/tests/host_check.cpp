/*  Loads the built MIHENK plug-in binaries the way a host does (JUCE's VST3 and AU hosting) and exercises them:
    instantiate, bus layout, parameters, latency, process audio, automate, save / restore state, open the editor.

        host_check <path/to/MIHENK.vst3> [<path/to/MIHENK.component>]
*/
#include <JuceHeader.h>

namespace
{
int failures = 0;
void check (bool ok, const juce::String& what)
{
    std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8());
    std::fflush (stdout);
    if (! ok) ++failures;
}

juce::AudioBuffer<float> testSignal (double fs, double secs)
{
    const int n = (int) (secs * fs);
    juce::AudioBuffer<float> b (2, n);
    juce::Random rng (7);
    double lp[2] { 0, 0 };
    for (int i = 0; i < n; ++i)
    {
        const double t = i / fs, tb = std::fmod (t, 0.5);
        const double kick = std::exp (-tb * 16.0) * std::sin (2.0 * juce::MathConstants<double>::pi * (50.0 * tb + 2.0 * (1.0 - std::exp (-tb * 30.0))));
        for (int c = 0; c < 2; ++c)
        {
            lp[c] += 0.08 * ((rng.nextDouble() * 2.0 - 1.0) - lp[c]);
            b.setSample (c, i, (float) (0.4 * kick + 0.3 * lp[c]));
        }
    }
    return b;
}

void testPlugin (juce::AudioPluginFormat& format, const juce::String& path)
{
    std::printf ("\n-- %s: %s\n", format.getName().toRawUTF8(), path.toRawUTF8());
    juce::OwnedArray<juce::PluginDescription> types;
    format.findAllTypesForFile (types, path);
    check (types.size() == 1, "Found " + juce::String (types.size()) + " plug-in type(s) in the bundle");
    if (types.isEmpty()) return;
    const auto& d = *types[0];
    check (d.name == "MIHENK" && d.manufacturerName == "SubverseLab", "Name \"" + d.name + "\", manufacturer \"" + d.manufacturerName + "\", version " + d.version
                                                                        + ", category " + d.category);
    const double fs = 48000.0;
    const int block = 512;
    juce::String error;
    auto inst = format.createInstanceFromDescription (d, fs, block, error);
    check (inst != nullptr, "Instantiated" + (error.isNotEmpty() ? " (" + error + ")" : juce::String()));
    if (inst == nullptr) return;
    check (inst->getTotalNumInputChannels() == 2 && inst->getTotalNumOutputChannels() == 2, "Stereo in / stereo out");
    auto params = inst->getParameters();
    check (params.size() >= 90, juce::String (params.size()) + " automatable parameters");
    inst->setPlayConfigDetails (2, 2, fs, block);
    inst->prepareToPlay (fs, block);
    const int latency = inst->getLatencySamples();
    check (latency > 0 && latency < (int) fs, "Latency reported to the host: " + juce::String (latency) + " samples (" + juce::String (1000.0 * latency / fs, 1) + " ms)");

    auto sig = testSignal (fs, 6.0);
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> io (2, block);
    double outEnergy = 0.0, maxAbs = 0.0;
    bool finite = true;
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    for (int pos = 0; pos + block <= sig.getNumSamples(); pos += block)
    {
        for (int c = 0; c < 2; ++c) io.copyFrom (c, 0, sig, c, pos, block);
        if (pos == block * 200 && ! params.isEmpty())
            for (auto* p : params)
                if (p->getName (64).containsIgnoreCase ("Drive")) { p->setValueNotifyingHost (p->getValue() + 0.05f); break; }
        inst->processBlock (io, midi);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < block; ++i)
            {
                const float v = io.getSample (c, i);
                finite &= std::isfinite (v);
                outEnergy += (double) v * v;
                maxAbs = juce::jmax (maxAbs, (double) std::abs (v));
            }
    }
    const double secs = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
    check (finite && outEnergy > 1.0 && maxAbs < 1.0, "Processed 6 s: output finite and audible, sample peak " + juce::String (juce::Decibels::gainToDecibels (maxAbs), 2)
                                                          + " dBFS (under the ceiling), a parameter automated mid-stream; " + juce::String (100.0 * secs / 6.0, 2) + " % of real time");

    // v3: the parameters are what the plan + the user's overrides show; the plug-in's timer reads a host's automation as an override over the plan. Let it run once, as a host's message loop does.
    juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
    juce::MemoryBlock state;
    inst->getStateInformation (state);
    auto inst2 = format.createInstanceFromDescription (d, fs, block, error);
    bool restored = inst2 != nullptr;
    if (restored)
    {
        inst2->setStateInformation (state.getData(), (int) state.getSize());
        auto p2 = inst2->getParameters();
        restored = p2.size() == params.size();
        for (int i = 0; i < params.size() && restored; ++i)
        {
            const bool same = std::abs (params[i]->getValue() - p2[i]->getValue()) < 1.0e-5f;
            if (! same) std::printf ("      differs: %s  saved %.5f (%s)  restored %.5f (%s)\n", params[i]->getName (64).toRawUTF8(), params[i]->getValue(), params[i]->getText (params[i]->getValue(), 32).toRawUTF8(), p2[i]->getValue(), p2[i]->getText (p2[i]->getValue(), 32).toRawUTF8());
            restored &= same;
        }
    }
    check (restored, "State saved (" + juce::String (state.getSize()) + " bytes) and restored into a new instance: every parameter matches");

    if (inst->hasEditor())
    {
        std::unique_ptr<juce::AudioProcessorEditor> ed (inst->createEditorIfNeeded());
        check (ed != nullptr && ed->getWidth() > 600 && ed->getHeight() > 300, "Editor opens: " + (ed != nullptr ? juce::String (ed->getWidth()) + " x " + juce::String (ed->getHeight()) : juce::String ("none")));
        ed = nullptr;
    }
    inst->releaseResources();
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc < 2) { std::printf ("host_check <MIHENK.vst3> [MIHENK.component]\n"); return 2; }
    juce::VST3PluginFormat vst3;
    testPlugin (vst3, juce::String::fromUTF8 (argv[1]));
   #if JUCE_PLUGINHOST_AU && JUCE_MAC
    if (argc >= 3)
    {
        juce::AudioUnitPluginFormat au;
        juce::OwnedArray<juce::PluginDescription> found;
        testPlugin (au, "AudioUnit:Effects/aufx,Mhnk,Svlb");
    }
   #endif
    std::printf ("\n%s\n", failures == 0 ? "ALL PASS" : (juce::String (failures) + " FAILED").toRawUTF8());
    return failures == 0 ? 0 : 1;
}
