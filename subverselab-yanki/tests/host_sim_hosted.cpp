/*  hostsim_hosted: the same host-like run, but the plug-in is loaded the way a
    DAW loads it: the installed AU (or VST3) component, through its wrapper.
    Parameters are moved with AudioProcessorParameter::setValue (AudioUnitSetParameter /
    the VST3 controller), audio is rendered through the wrapper.

        hostsim_hosted <au|vst3> [moves] [seed] [scenario 0|1]
*/
#include "host_sim.h"

using namespace hostsim;

struct NameParams : Params
{
    explicit NameParams (juce::AudioProcessor& proc) : p (proc) {}
    juce::AudioProcessorParameter* get (const char* id) override
    {
        static const std::map<juce::String, juce::String> names {
            { "MOOD", "Mood" }, { "TIME_SYNC", "Time Sync" }, { "TIME_MS", "Time (Free)" }, { "FEEDBACK", "Feedback" },
            { "CAP_ON", "Cap On" }, { "CAP_DB", "Cap" }, { "TONE", "Tone" }, { "WEAR", "Wear" }, { "MIX", "Mix" },
            { "OUTPUT", "Output" }, { "END_MODE", "End Mode" }, { "END_TRIGGER", "End Trigger" }, { "FREEZE", "Freeze" },
            { "BYPASS", "Bypass" }, { "DARKEN", "Darken" }, { "RISE_CURVE", "Rise Curve" }, { "LOCK_BARS", "Tempo Lock" },
            { "FINE_MS", "Fine" }, { "SPREAD", "Spread" } };
        const auto want = names.at (id);
        for (auto* prm : p.getParameters())
            if (prm->getName (64) == want)
                return prm;
        return nullptr;
    }
    float norm (const char* id, float plain) override
    {
        const juce::String s (id);
        if (s == "OUTPUT") return (plain + 12.0f) / 24.0f;
        if (s == "TIME_MS") return (plain - 50.0f) / 650.0f;
        if (s == "TONE") return (plain - 1000.0f) / 7000.0f;
        if (s == "TIME_SYNC") return plain / 6.0f;
        return plain;   // MIX, FEEDBACK-position, etc. are 0..1 or handled by the caller
    }
    juce::AudioProcessor& p;
};

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::String fmtName = argc > 1 ? argv[1] : "au";
    const int moves = argc > 2 ? juce::String (argv[2]).getIntValue() : 300;
    const int seed = argc > 3 ? juce::String (argv[3]).getIntValue() : 1;
    const int scenario = argc > 4 ? juce::String (argv[4]).getIntValue() : 1;

    juce::AudioPluginFormatManager fm;
    fm.addFormat (new juce::AudioUnitPluginFormat());
    fm.addFormat (new juce::VST3PluginFormat());
    const auto path = fmtName == "au" ? juce::String ("~/Library/Audio/Plug-Ins/Components/YANKI.component")
                                      : juce::String ("~/Library/Audio/Plug-Ins/VST3/YANKI.vst3");
    const auto full = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile (path.substring (2)).getFullPathName();
    juce::OwnedArray<juce::PluginDescription> descs;
    for (auto* f : fm.getFormats())
        if (f->getName().containsIgnoreCase (fmtName == "au" ? "AudioUnit" : "VST3"))
            f->findAllTypesForFile (descs, full);
    if (descs.isEmpty()) { std::cout << "no plug-in found at " << full << std::endl; return 2; }
    juce::String err;
    auto inst = fm.createPluginInstance (*descs[0], 48000.0, 512, err);
    if (inst == nullptr) { std::cout << "cannot load: " << err << std::endl; return 2; }
    std::cout << "loaded " << inst->getName() << " via " << descs[0]->pluginFormatName << ", " << inst->getParameters().size() << " parameters" << std::endl;

    if (scenario == 9)
    {
        for (auto* prm : inst->getParameters())
            std::cout << prm->getParameterIndex() << " '" << prm->getName (64) << "' label '" << prm->getLabel() << "' steps " << prm->getNumSteps()
                      << " discrete " << (int) prm->isDiscrete() << " bool " << (int) prm->isBoolean() << " meta " << (int) prm->isMetaParameter()
                      << " automatable " << (int) prm->isAutomatable() << " def " << prm->getDefaultValue() << " val " << prm->getValue() << std::endl;
        return 0;
    }
    inst->setPlayHead (nullptr);
    NameParams np (*inst);
    juce::AudioProcessor::BusesLayout layout;
    layout.inputBuses.add (juce::AudioChannelSet::stereo());
    layout.outputBuses.add (juce::AudioChannelSet::stereo());
    inst->setBusesLayout (layout);
    Rig rig (*inst, np);
    rig.start();
    rig.waitSamples (48000);

    auto show = [&] (const juce::String& tag)
    {
        auto r = probe (rig);
        std::cout << tag << ": diff@MIX0 " << juce::String (r.diffDbAt0, 1) << " dB, diff@MIX0.3 " << juce::String (r.diffDbAtMix, 1)
                  << " dB, rise " << juce::String (r.riseDb(), 1) << " dB -> " << (r.follows() ? "FOLLOWS" : "DEAD") << std::endl;
        return r.follows();
    };
    bool ok = show ("start");
    juce::Random rnd (seed);
    if (scenario == 0)
    {
        for (float v : { 0.6f, 0.2f }) { move (rig, ids::mix, v); rig.waitSamples (20000); }
        ok = show ("after 2 MIX moves") && ok;
    }
    else
    {
        static const char* all[] { ids::timeSync, ids::timeMs, ids::feedback, ids::tone, ids::wear, ids::mix, ids::output, ids::spread, ids::fineMs };
        for (int i = 0; i < moves; ++i)
        {
            move (rig, all[rnd.nextInt ((int) (sizeof (all) / sizeof (all[0])))], rnd.nextFloat());
            if (rnd.nextInt (4) == 0) rig.waitSamples (rnd.nextInt (6000));
            else juce::MessageManager::getInstance()->runDispatchLoopUntil (1);
            if ((i + 1) % 100 == 0) ok = show ("after " + juce::String (i + 1) + " moves") && ok;
        }
        ok = show ("end") && ok;
    }
    rig.stop();
    inst.reset();
    std::cout << (ok ? "OK" : "FAILED: the output stopped following MIX") << std::endl;
    return ok ? 0 : 1;
}
