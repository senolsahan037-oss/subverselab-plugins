/*  Draws the editor into PNGs without a screen (no Screen Recording permission
    needed). Program material runs through the real processor first, so the
    meters in the label window show measured values, not drawn ones.

        ui_snapshot [out_dir] [scale]      default docs/ui, scale 1 (1600 x 640)

    Writes front_default.png (AIR FULL, the state the plugin opens in),
    front_master_air.png (mood MASTER AIR: MASTER SAFE on) and rear.png.
*/
#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"

namespace
{
/** Noise bursts every 250 ms over a low sine; stops 30 ms after a burst, so the sheen is lit. */
void feed (HavaProcessor& p, double sr, double seconds)
{
    const int block = 480;
    const int total = (int) (sr * seconds);
    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer midi;
    juce::Random rng (72);
    for (int pos = 0; pos < total; pos += block)
    {
        for (int i = 0; i < block; ++i)
        {
            const double t = (double) (pos + i) / sr;
            const float burst = (float) std::exp (-std::fmod (t, 0.25) / 0.04) * 0.4f;
            const float s = 0.2f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 110.0 * t);
            buf.setSample (0, i, s + burst * (rng.nextFloat() * 2.0f - 1.0f));
            buf.setSample (1, i, 0.9f * s + burst * (rng.nextFloat() * 2.0f - 1.0f));
        }
        p.processBlock (buf, midi);
    }
}

bool snap (HavaEditor& editor, const juce::File& file, float scale)
{
    editor.settleForSnapshot();
    const auto image = editor.createComponentSnapshot (editor.getLocalBounds(), true, scale);
    file.deleteFile();
    juce::FileOutputStream stream (file);
    const bool ok = stream.openedOk() && juce::PNGImageFormat().writeImageToStream (image, stream);
    std::printf ("%s %s (%d x %d)\n", ok ? "wrote" : "FAILED", file.getFullPathName().toRawUTF8(), image.getWidth(), image.getHeight());
    return ok;
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::File dir = juce::File::getCurrentWorkingDirectory().getChildFile (argc > 1 ? argv[1] : "docs/ui");
    const float scale = argc > 2 ? (float) std::atof (argv[2]) : 1.0f;
    dir.createDirectory();
    const double sr = 48000.0;
    bool ok = true;

    {
        HavaProcessor p;
#if SVL_LICENSING
        p.license.setLicensedForTesting (true);
#endif
        p.setRateAndBufferSizeDetails (sr, 480);
        p.prepareToPlay (sr, 480);
        feed (p, sr, 2.28);
        std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
        auto& editor = *static_cast<HavaEditor*> (ed.get());
        editor.setSize (HavaEditor::kW, HavaEditor::kH);
        ok = snap (editor, dir.getChildFile ("front_default.png"), scale) && ok;
        editor.showRear (true);
        ok = snap (editor, dir.getChildFile ("rear.png"), scale) && ok;
    }
    {
        HavaProcessor p;
#if SVL_LICENSING
        p.license.setLicensedForTesting (true);
#endif
        p.setRateAndBufferSizeDetails (sr, 480);
        p.prepareToPlay (sr, 480);
        p.selectMood (4);   // MASTER AIR
        feed (p, sr, 4.28);  // long enough for the 1.5 s auto-gain window
        std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
        auto& editor = *static_cast<HavaEditor*> (ed.get());
        editor.setSize (HavaEditor::kW, HavaEditor::kH);
        ok = snap (editor, dir.getChildFile ("front_master_air.png"), scale) && ok;
    }
    if (argc > 3 && juce::String (argv[3]) == "--moods")   // one shot per MOOD: ui_snapshot <dir> <scale> --moods
    {
        for (int m = 0; m < HavaProcessor::moodNames.size(); ++m)
        {
            HavaProcessor p;
#if SVL_LICENSING
            p.license.setLicensedForTesting (true);
#endif
            p.setRateAndBufferSizeDetails (sr, 480);
            p.prepareToPlay (sr, 480);
            p.selectMood (m);
            feed (p, sr, 4.28);
            std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
            auto& editor = *static_cast<HavaEditor*> (ed.get());
            editor.setSize (HavaEditor::kW, HavaEditor::kH);
            ok = snap (editor, dir.getChildFile (juce::String ("hava_mood_") + juce::String (m) + "_" + HavaProcessor::moodNames[m].removeCharacters (" +") + ".png"), scale) && ok;
        }
    }
    return ok ? 0 : 1;
}
