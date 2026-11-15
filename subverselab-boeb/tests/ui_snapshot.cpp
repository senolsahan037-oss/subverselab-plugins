/*  Draws the BOE-B editor into a PNG without a screen (no screen capture: root CLAUDE.md).

        ui_snapshot --out x.png [--wav take.wav] [--seconds S] [--stop-listening] [--help] [--scale 2] [id=value ...]

    --wav runs that file (looped to --seconds, default 20) through the processor in 512-sample blocks, as a host would;
    LISTEN starts by itself on the first audio, so the sheet shows real FOUND / NOW numbers. --stop-listening stops
    after 5 s of audio, while LISTEN is still listening (the scan light). id=value sets a parameter (lift=8, mood=2).
*/
#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"

int main (int argc, char** argv)
{
    juce::String out, wav;
    float scale = 1.0f, seconds = 20.0f;
    bool helpOpen = false, midListen = false, check = false;
    std::vector<std::pair<juce::String, float>> params;
    for (int i = 1; i < argc; ++i)
    {
        const juce::String a (argv[i]);
        auto next = [&] { return i + 1 < argc ? juce::String (argv[++i]) : juce::String(); };
        if (a == "--out") out = next();
        else if (a == "--wav") wav = next();
        else if (a == "--seconds") seconds = next().getFloatValue();
        else if (a == "--scale") scale = next().getFloatValue();
        else if (a == "--help") helpOpen = true;
        else if (a == "--stop-listening") midListen = true;
        else if (a == "--check") check = true;
        else if (a.contains ("=")) params.push_back ({ a.upToFirstOccurrenceOf ("=", false, false), a.fromFirstOccurrenceOf ("=", false, false).getFloatValue() });
    }
    if (out.isEmpty()) { std::printf ("usage: ui_snapshot --out x.png [--wav f] [--seconds S] [--stop-listening] [--help] [--scale 2] [id=v ...]\n"); return 1; }

    juce::ScopedJuceInitialiser_GUI init;
    BoebProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    for (const auto& [id, v] : params)
        if (auto* p = processor.apvts.getParameter (id)) p->setValueNotifyingHost (p->convertTo0to1 (v));

    std::unique_ptr<juce::AudioProcessorEditor> ed (processor.createEditor());
    auto* e = static_cast<BoebEditor*> (ed.get());

    if (wav.isNotEmpty())
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (juce::File (wav)));
        if (rd == nullptr) { std::printf ("cannot read %s\n", wav.toRawUTF8()); return 2; }
        juce::AudioBuffer<float> file (2, (int) rd->lengthInSamples);
        rd->read (&file, 0, (int) rd->lengthInSamples, 0, true, true);
        if (rd->numChannels == 1) file.copyFrom (1, 0, file, 0, 0, file.getNumSamples());
        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer midi;
        const int total = (int) ((midListen ? 5.0f : seconds) * 48000.0f);
        int pos = 0;
        for (int done = 0; done < total; done += 512)
        {
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < 512; ++i)
                    buf.setSample (c, i, file.getSample (c, (pos + i) % file.getNumSamples()));
            pos = (pos + 512) % file.getNumSamples();
            processor.processBlock (buf, midi);
            if (! midListen) processor.finishPendingWorkNow();
        }
        if (! midListen) { processor.finishPendingWorkNow(); juce::MessageManager::getInstance()->runDispatchLoopUntil (300); }
    }

    e->settleForSnapshot();
    if (check)
    {
        // Interface signature checks (UI_SIGNATURE_STANDARD.md "Checks").
        int fails = 0;
        auto say = [&] (bool ok, const juce::String& line) { std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", line.toRawUTF8()); fails += ok ? 0 : 1; };
        auto* lift = processor.apvts.getParameter ("lift");
        const double from = e->drawnValue ("lift");
        lift->setValueNotifyingHost (lift->convertTo0to1 (10.0f));   // a value the product sets (as LISTEN / a host would)
        e->advance (1.0f / 30.0f);
        const double one = e->drawnValue ("lift");
        say (one > from && one < 10.0, "glide: LIFT " + juce::String (from, 1) + " -> 10, after one tick drawn " + juce::String (one, 2) + " (between)");
        int ticks = 1;
        while (e->drawnValue ("lift") < 10.0 && ticks < 100) { e->advance (1.0f / 30.0f); ++ticks; }
        say (ticks <= 15, "glide settles at 10 after " + juce::String (ticks) + " ticks (" + juce::String (ticks / 30.0, 2) + " s) | want 0.3-0.5 s");
        say (std::abs (lift->convertFrom0to1 (lift->getValue()) - 10.0f) < 1e-4f, "the parameter itself changed at once");
        e->doubleClick ("lift");
        say (std::abs (lift->convertFrom0to1 (lift->getValue()) - 6.0f) < 1e-4f, "double-click LIFT -> 6 (what LISTEN found)");
        auto* air = processor.apvts.getParameter ("air");
        air->setValueNotifyingHost (air->convertTo0to1 (5.0f));
        e->doubleClick ("air");
        say (std::abs (air->convertFrom0to1 (air->getValue()) - processor.listenedAirDb()) < 0.051f,
             "double-click AIR -> LISTEN's value " + juce::String (processor.listenedAirDb(), 1) + " (not the factory 0 unless LISTEN set 0)");
        std::printf ("%d fail(s)\n", fails);
        return fails;
    }
    if (midListen) for (int i = 0; i < 20; ++i) e->advance (1.0f / 30.0f);   // the scan light part way down
    e->setHelpOpen (helpOpen);
    const auto r = processor.readout();
    std::printf ("found %d now %d | found lar %.1f spread %.1f pres %.1f mud %.1f air %.1f | now lar %.1f spread %.1f pres %.1f mud %.1f air %.1f | air knob %.1f\n",
                 (int) r.haveFound, (int) r.haveNow, r.found.larDb, r.found.spreadDb, r.found.presDb, r.found.mudDb, r.found.airDb,
                 r.now.larDb, r.now.spreadDb, r.now.presDb, r.now.mudDb, r.now.airDb,
                 processor.apvts.getRawParameterValue ("air")->load());

    const auto img = e->createComponentSnapshot (e->getLocalBounds(), true, scale);
    juce::File f (juce::File::getCurrentWorkingDirectory().getChildFile (out));
    f.deleteFile();
    juce::FileOutputStream os (f);
    if (! os.openedOk() || ! juce::PNGImageFormat().writeImageToStream (img, os)) { std::printf ("could not write %s\n", out.toRawUTF8()); return 2; }
    std::printf ("wrote %s (%d x %d)\n", f.getFullPathName().toRawUTF8(), img.getWidth(), img.getHeight());
    return 0;
}
