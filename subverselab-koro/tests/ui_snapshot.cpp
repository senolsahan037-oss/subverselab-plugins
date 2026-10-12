/*  Draws the editor into PNGs without a screen, after real audio has gone through the
    processor, so every lamp shows what the engine measured (nothing is faked).

        ui_snapshot [outdir]        (default docs/ui)

    The singer is the engine's own synthetic scale-passage voice (tools/engine_render.cpp,
    scaleMelody) in B Hicaz, the key of the approved face; MOOD = CHOIR.
      front.png         : after 9 s of singing, AUTO
      front_locked.png  : LOCK on, one more second
      rear.png          : the rear plate
*/
#include <JuceHeader.h>
#include "EngineCli.h"
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"

namespace
{
void sing (KoroProcessor& p, const std::vector<float>& x, size_t from, size_t to)
{
    juce::AudioBuffer<float> buf (2, 480);
    juce::MidiBuffer midi;
    for (size_t pos = from; pos + 480 <= to; pos += 480)
    {
        for (int i = 0; i < 480; ++i) { buf.setSample (0, i, x[pos + (size_t) i]); buf.setSample (1, i, x[pos + (size_t) i]); }
        p.processBlock (buf, midi);
    }
}

bool shoot (KoroEditor& e, const juce::File& f)
{
    e.settleForSnapshot();
    const auto image = e.createComponentSnapshot (e.getLocalBounds(), true, 1.0f);
    f.deleteFile();
    juce::FileOutputStream stream (f);
    const bool ok = stream.openedOk() && juce::PNGImageFormat().writeImageToStream (image, stream);
    std::printf ("%s %s %dx%d\n", ok ? "wrote" : "FAILED", f.getFullPathName().toRawUTF8(), image.getWidth(), image.getHeight());
    return ok;
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::File dir = juce::File::getCurrentWorkingDirectory().getChildFile (argc > 1 ? argv[1] : "docs/ui");
    dir.createDirectory();

    KoroProcessor processor;
#if SVL_LICENSING
    processor.license.setLicensedForTesting (std::getenv ("UNLICENSED") == nullptr);
#endif
    processor.applyMood (5);                       // CHOIR
    processor.prepareToPlay (48000.0, 480);
    auto x = enginecli::scaleMelody (48000, { { 12.0, 11, 3 } }, 21u);   // B Hicaz
    size_t singTo = 48000 * 9;
    // Demo hook (env, unset = unchanged behaviour): KORO_WAV = 48 kHz mono/stereo file sung instead of the synthetic voice,
    // KORO_MOOD index, KORO_KEY / KORO_SCALE override index (see keyNames / scaleOverrideNames), KORO_BYPASS=1,
    // KORO_UNTIL seconds, KORO_ONLY=<name.png>: write only front shot to that file name inside outdir.
    if (const char* w = std::getenv ("KORO_WAV"))
    {
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (juce::File (w)));
        if (rd == nullptr) { std::printf ("cannot read %s\n", w); return 2; }
        juce::AudioBuffer<float> b ((int) rd->numChannels, (int) rd->lengthInSamples);
        rd->read (&b, 0, (int) rd->lengthInSamples, 0, true, true);
        x.assign (b.getReadPointer (0), b.getReadPointer (0) + b.getNumSamples());
    }
    auto envi = [] (const char* n, int d) { const char* v = std::getenv (n); return v != nullptr ? std::atoi (v) : d; };
    if (std::getenv ("KORO_MOOD")) processor.applyMood (envi ("KORO_MOOD", 5));
    if (std::getenv ("KORO_KEY"))   processor.apvts.getParameter ("keyOverride")->setValueNotifyingHost (processor.apvts.getParameter ("keyOverride")->convertTo0to1 ((float) envi ("KORO_KEY", 0)));
    if (std::getenv ("KORO_SCALE")) processor.apvts.getParameter ("scaleOverride")->setValueNotifyingHost (processor.apvts.getParameter ("scaleOverride")->convertTo0to1 ((float) envi ("KORO_SCALE", 0)));
    if (envi ("KORO_BYPASS", 0)) processor.apvts.getParameter ("bypass")->setValueNotifyingHost (1.0f);
    if (std::getenv ("KORO_UNTIL")) singTo = (size_t) (48000.0 * std::atof (std::getenv ("KORO_UNTIL")));
    singTo = std::min (singTo, x.size());
    sing (processor, x, 0, singTo);

    std::unique_ptr<juce::AudioProcessorEditor> ed (processor.createEditor());
    auto& editor = *static_cast<KoroEditor*> (ed.get());
    editor.setSize (KoroEditor::kW, KoroEditor::kH);
    std::printf ("state: key %s %s, confidence %.2f, pitch in %s (%.0f c), out %s (%.0f c), safe %d\n",
                 KoroProcessor::keyNames[processor.ui.root.load() + 1].toRawUTF8(),
                 KoroProcessor::scaleOverrideNames[processor.ui.scale.load() + 1].toRawUTF8(),
                 (double) processor.ui.confidence.load(), processor.ui.pitchValid.load() ? "on" : "off", (double) processor.ui.inCents.load(),
                 processor.ui.outValid.load() ? "on" : "off", (double) processor.ui.outCents.load(), (int) processor.ui.safe.load());
    const char* only = std::getenv ("KORO_ONLY");
    if (only != nullptr) return shoot (editor, dir.getChildFile (only)) ? 0 : 1;
    bool ok = shoot (editor, dir.getChildFile ("front.png"));

    auto* lock = processor.apvts.getParameter ("lock");
    lock->setValueNotifyingHost (1.0f);
    sing (processor, x, 48000 * 9, 48000 * 10);
    ok &= shoot (editor, dir.getChildFile ("front_locked.png"));

    editor.showRear (true);
    ok &= shoot (editor, dir.getChildFile ("rear.png"));
    return ok ? 0 : 1;
}
