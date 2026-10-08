/*  Draws the TABAN editor into a PNG without a screen, so the layout can be checked without Screen Recording permission.

        ui_snapshot --out x.png [--floor dB] [--output dB] [--in 0|1] [--signal dBFS] [--scale 2]

    --signal runs one second of a 100 Hz tone at that level through the processor first, so the IN / OUT bars and
    the LIFT read-out show what they would in a host.
*/
#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"

int main (int argc, char** argv)
{
    juce::String out;
    float scale = 1.0f, signalDb = -200.0f;
    std::vector<std::pair<const char*, float>> params;
    for (int i = 1; i < argc; ++i)
    {
        const juce::String a (argv[i]);
        auto next = [&] { return i + 1 < argc ? juce::String (argv[++i]) : juce::String(); };
        if (a == "--out") out = next();
        else if (a == "--scale") scale = next().getFloatValue();
        else if (a == "--signal") signalDb = next().getFloatValue();
        else if (a == "--floor") params.push_back ({ "floor", next().getFloatValue() });
        else if (a == "--output") params.push_back ({ "output", next().getFloatValue() });
        else if (a == "--in") params.push_back ({ "in", next().getFloatValue() });
    }
    if (out.isEmpty())
    {
        std::printf ("usage: ui_snapshot --out x.png [--floor dB] [--output dB] [--in 0|1] [--signal dBFS] [--scale 2]\n");
        return 1;
    }

    juce::ScopedJuceInitialiser_GUI init;
    TabanAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    for (const auto& [id, v] : params)
    {
        auto* p = processor.apvts.getParameter (id);
        p->setValueNotifyingHost (p->convertTo0to1 (v));
    }

    std::unique_ptr<juce::AudioProcessorEditor> ed (processor.createEditor());
    auto* e = static_cast<TabanAudioProcessorEditor*> (ed.get());

    if (signalDb > -150.0f)
    {
        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer midi;
        const float a = juce::Decibels::decibelsToGain (signalDb);
        for (int b = 0, n = 0; b < 94; ++b)
        {
            for (int i = 0; i < 512; ++i, ++n)
                for (int c = 0; c < 2; ++c)
                    buf.setSample (c, i, a * std::sin (2.0f * juce::MathConstants<float>::pi * 100.0f * (float) n / 48000.0f));
            processor.processBlock (buf, midi);
            e->advance (512.0f / 48000.0f);
        }
    }
    else
        e->advance (0.0f);

    const auto img = e->createComponentSnapshot (e->getLocalBounds(), true, scale);
    juce::File f (juce::File::getCurrentWorkingDirectory().getChildFile (out));
    f.deleteFile();
    juce::FileOutputStream os (f);
    if (! os.openedOk() || ! juce::PNGImageFormat().writeImageToStream (img, os))
    {
        std::printf ("could not write %s\n", out.toRawUTF8());
        return 2;
    }
    std::printf ("wrote %s (%d x %d)\n", f.getFullPathName().toRawUTF8(), img.getWidth(), img.getHeight());
    return 0;
}
