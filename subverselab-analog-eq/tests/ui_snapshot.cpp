// ui_snapshot: draws the TORNA editor to PNGs without a screen, after real audio has gone through the processor, so the
// analyser and the curve show what the engine did. Writes docs/ui/p2_*.png (1x and 2x).
#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"

namespace
{
void setP (TornaAudioProcessor& p, const juce::String& id, float plain)
{
    if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (p.apvts.getParameter (id))) r->setValueNotifyingHost (r->convertTo0to1 (plain));
}

void feed (TornaAudioProcessor& p, double sr, double seconds, unsigned seed)
{
    // a mix-like signal: pink-ish noise plus a bass line and a few partials, so every column has something to read
    juce::AudioBuffer<float> b (2, 512);
    juce::MidiBuffer m;
    juce::Random rng ((juce::int64) seed);
    double ph[4] = {}, b0 = 0, b1 = 0, b2 = 0;
    const double fr[4] = { 55.0, 110.0, 440.0, 2200.0 }, am[4] = { 0.25, 0.12, 0.06, 0.02 };
    for (int done = 0; done < (int) (seconds * sr); done += 512)
    {
        for (int i = 0; i < 512; ++i)
        {
            const double w = rng.nextFloat() * 2.0 - 1.0;
            b0 = 0.99765 * b0 + w * 0.0990460; b1 = 0.96300 * b1 + w * 0.2965164; b2 = 0.57000 * b2 + w * 1.0526913;
            double s = (b0 + b1 + b2 + w * 0.1848) * 0.05;
            for (int k = 0; k < 4; ++k) { ph[k] += fr[k] / sr; s += am[k] * std::sin (2.0 * juce::MathConstants<double>::pi * ph[k]); }
            b.setSample (0, i, (float) s); b.setSample (1, i, (float) (s * 0.97));
        }
        p.processBlock (b, m);
    }
}

void snap (TornaEditor& ed, const juce::File& dir, const juce::String& name)
{
    for (int i = 0; i < 40; ++i) ed.timerTickForTests();   // let the caps glide home and the read-outs settle
    for (int scale : { 1, 2 })
    {
        auto img = ed.createComponentSnapshot (ed.getLocalBounds(), true, (float) scale / (ed.getWidth() / 1600.0f));
        juce::File f = dir.getChildFile ("p2_" + name + (scale == 2 ? "_2x" : "") + ".png");
        f.deleteFile();
        juce::FileOutputStream out (f);
        juce::PNGImageFormat().writeImageToStream (img, out);
        std::printf ("wrote %s (%d x %d)\n", f.getFullPathName().toRawUTF8(), img.getWidth(), img.getHeight());
    }
}
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    // ui_snapshot [dir]: the Windows CI passes its own folder (panel_dir); without one the PNGs go to docs/ui.
    const juce::File dir = argc > 1 ? juce::File::getCurrentWorkingDirectory().getChildFile (juce::String (argv[1]))
                                    : juce::File (TORNA_ROOT).getChildFile ("docs/ui");
    dir.createDirectory();
    auto owned = std::make_unique<TornaAudioProcessor>();
    auto& p = *owned;
    p.setUserPresetDirForTests (juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("torna_snapshot_presets"));
    p.setPlayConfigDetails (2, 2, 48000.0, 512);
    p.prepareToPlay (48000.0, 512);
    const float smile[12] = { 3, 4.5f, 3, 1, -1, -2, -2.5f, -1, 1, 3, 4.5f, 3.5f };
    for (int b = 0; b < 12; ++b) { setP (p, torna::sliderId (0, b), smile[b]); setP (p, torna::sliderId (1, b), smile[b] * 0.8f); }
    feed (p, 48000.0, 1.5, 1);
    {
        std::unique_ptr<TornaEditor> ed (dynamic_cast<TornaEditor*> (p.createEditor()));
        ed->setScalePct (100);
        ed->curve.showReadout ("L  4kHz  +1.0 dB");
        snap (*ed, dir, "default");
        setP (p, "rev", 1.0f); feed (p, 48000.0, 0.5, 2);
        snap (*ed, dir, "rev");
        setP (p, "rev", 0.0f); setP (p, "eqIn", 0.0f);
        snap (*ed, dir, "eq_out");
        setP (p, "eqIn", 1.0f);
        ed->helpOpen = true;
        snap (*ed, dir, "help");
        ed->helpOpen = false;
        p.applyPreset (5);
        snap (*ed, dir, "preset_vocal");
        ed->showRear (true);
        snap (*ed, dir, "rear");
        ed->showRear (false);
        ed->setScalePct (75);
        snap (*ed, dir, "at75");
        std::fprintf (stderr, "[marker] deleting the editor\n");
    }
    std::fprintf (stderr, "[marker] editor gone, processor next\n");
    owned.reset();
    std::fprintf (stderr, "[marker] processor gone\n");
    return 0;
}
