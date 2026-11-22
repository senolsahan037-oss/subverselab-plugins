// HIGH STEREO panel snapshot, headless (CLAUDE.md: no screen capture; the product's own snapshot tool).
// Usage: ui_snapshot in.f32 rate out.png [seconds] [key=value ...]
//   in.f32: stereo float32 interleaved audio, fed through the real processor block by block, the panel ticked at 30 Hz.
//   keys: width=<%> stage=<deg> band<k>=<%> listen=<0..4> match=<0|1> fix=<0|1> spread=<%> hover=<control id>
//         hoverrow=<0..7> (a WIDTH field row, top 8k+ = 0)
// Writes the editor at 2x. Also prints the hover summary for hover=.
#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"

int main (int argc, char** argv)
{
    if (argc < 4) { std::fprintf (stderr, "usage: ui_snapshot in.f32 rate out.png [seconds] [key=value ...]\n"); return 2; }
    juce::ScopedJuceInitialiser_GUI gui;
    std::vector<float> inter;
    { juce::File f (juce::File::getCurrentWorkingDirectory().getChildFile (argv[1])); juce::MemoryBlock mb; f.loadFileAsData (mb);
      inter.resize (mb.getSize() / sizeof (float)); std::memcpy (inter.data(), mb.getData(), inter.size() * sizeof (float)); }
    const double rate = std::atof (argv[2]);
    const double seconds = argc > 4 ? std::atof (argv[4]) : 6.0;

    HighStereoAudioProcessor proc;
    proc.setPlayConfigDetails (2, 2, rate, 512);
    proc.prepareToPlay (rate, 512);
    std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
    auto* hs = dynamic_cast<HighStereoAudioProcessorEditor*> (ed.get());
    juce::String hover;
    for (int a = 5; a < argc; ++a)
    {
        const juce::String kv (argv[a]); const auto k = kv.upToFirstOccurrenceOf ("=", false, false), v = kv.fromFirstOccurrenceOf ("=", false, false);
        if (k == "listen") proc.setListen ((svl::HighStereoEngine::Listen) v.getIntValue());
        else if (k == "hover") hover = v;
        else if (k == "hoverrow") { hover = "field"; hs->widthField().setHoverRowForSnapshot (v.getIntValue()); }
        else if (auto* p = proc.apvts.getParameter (k)) p->setValueNotifyingHost (p->convertTo0to1 (v.getFloatValue()));
        else { std::fprintf (stderr, "unknown key %s\n", k.toRawUTF8()); return 2; }
    }
    const size_t frames = inter.size() / 2, total = std::min (frames, (size_t) (seconds * rate));
    juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
    const int tickEvery = (int) (rate / 30.0); int sinceTick = 0;
    for (size_t i = 0; i < total; i += 512)
    {
        const int n = (int) std::min ((size_t) 512, total - i);
        buf.setSize (2, n, false, false, true);
        for (int s = 0; s < n; ++s) { buf.setSample (0, s, inter[2 * (i + s)]); buf.setSample (1, s, inter[2 * (i + s) + 1]); }
        proc.processBlock (buf, midi);
        sinceTick += n;
        if (sinceTick >= tickEvery) { sinceTick = 0; hs->tickForSnapshot(); }
    }
    if (hover.isNotEmpty())
    {
        auto* c = hs->controlNamed (hover);
        if (c == nullptr) { std::fprintf (stderr, "no control %s\n", hover.toRawUTF8()); return 2; }
        hs->setHoverForSnapshot (c);
        std::printf ("hover %s: %s\n", hover.toRawUTF8(), hs->currentSummary().toRawUTF8());
    }
    const auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f);
    juce::File out (juce::File::getCurrentWorkingDirectory().getChildFile (argv[3]));
    out.deleteFile();
    juce::FileOutputStream os (out); juce::PNGImageFormat png;
    if (! os.openedOk() || ! png.writeImageToStream (img, os)) { std::fprintf (stderr, "write failed\n"); return 1; }
    std::printf ("%s %dx%d, correlation %.2f\n", argv[3], img.getWidth(), img.getHeight(), proc.getCorrelation());
    ed.reset();
    return 0;
}
