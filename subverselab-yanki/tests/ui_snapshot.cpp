/*  Draws the editor into PNGs without a screen (no DAW, no Screen Recording
    permission). The processor is run first on a test signal with a fake host
    at 120 bpm, so the display shows what the engine measures, not staged
    numbers.

        ui_snapshot <out-dir> [scale]

    writes (the v2 face, docs/ui/v2_concept_A.png)
        v2_editor_slap.png     preset 1, Slap 80 ms
        v2_editor_dotted.png   preset 8, Dotted eighth wide (the concept's main state)
        v2_editor_swell.png    preset 9, Swell build, 8 s of the tone
        v2_editor_init.png     the opening state (no preset)
        v2_editor_heads2_dump.png  preset 3, HEADS 2, END = DUMP fired (keys show the mode, the line says DUMPED)
*/
#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"

namespace
{
double gBpm = 120.0;   // ui_snapshot <dir> <scale> --moods [bpm]

struct FakeHost : juce::AudioPlayHead
{
    double ppq = 0.0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setBpm (gBpm);
        info.setIsPlaying (true);
        info.setPpqPosition (ppq);
        return info;
    }
};

/** seconds of a harmonic 220 Hz tone at -18 dBFS RMS through the processor. */
void feed (YankiProcessor& p, double seconds, std::function<void (int)> atBlock = {})
{
    const double sr = 48000.0;
    const int block = 512;
    FakeHost host;
    p.setPlayHead (&host);
    p.setRateAndBufferSizeDetails (sr, block);
    p.prepareToPlay (sr, block);
    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer midi;
    const double amp = std::pow (10.0, -18.0 / 20.0) * std::sqrt (2.0) / 1.2;
    long long t = 0;
    for (int b = 0; b < (int) (seconds * sr / block); ++b)
    {
        if (atBlock) atBlock (b);
        for (int i = 0; i < block; ++i, ++t)
        {
            double v = 0;
            for (int h = 1; h <= 3; ++h)
                v += std::sin (2 * juce::MathConstants<double>::pi * 220 * h * (double) t / sr) / h;
            buf.setSample (0, i, (float) (amp * v / 1.0));
            buf.setSample (1, i, (float) (amp * v / 1.0));
        }
        p.processBlock (buf, midi);
        host.ppq += block / sr * 2.0;
    }
    p.setPlayHead (nullptr);
}

bool save (const juce::Image& img, const juce::File& f)
{
    f.deleteFile();
    juce::FileOutputStream stream (f);
    return stream.openedOk() && juce::PNGImageFormat().writeImageToStream (img, stream);
}

juce::Image shot (juce::Component& c, float scale)
{
    return c.createComponentSnapshot (c.getLocalBounds(), true, scale);
}
} // namespace

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: ui_snapshot <out-dir> [scale]\n";
        return 1;
    }
    juce::ScopedJuceInitialiser_GUI init;
    const juce::File dir { juce::File::getCurrentWorkingDirectory().getChildFile (juce::String (argv[1])) };
    dir.createDirectory();
    const float scale = argc > 2 ? (float) std::atof (argv[2]) : 1.0f;
    bool ok = true;

    struct Shot { const char* file; int preset; double seconds; int heads = 0; int endMode = 0; };
    for (const auto& sh : { Shot { "v2_editor_slap.png", 0, 3.0 }, { "v2_editor_dotted.png", 7, 4.0 }, { "v2_editor_swell.png", 8, 8.0 }, { "v2_editor_init.png", -1, 2.0 }, { "v2_editor_heads2_dump.png", 2, 3.0, 2, 2 } })
    {
        YankiProcessor p;
        if (sh.preset >= 0)
            p.applyPreset (sh.preset);
        auto setp = [&p] (const char* id, float v) { if (auto* a = p.apvts.getParameter (id)) a->setValueNotifyingHost (a->convertTo0to1 (v)); };
        if (sh.heads > 0) setp (yankiIds::heads, (float) sh.heads);
        if (sh.endMode > 0) setp (yankiIds::endMode, (float) sh.endMode);
        // a phrase is already playing when the key is pressed (a press in silence re-arms on the next note)
        feed (p, sh.seconds, [&] (int b)
        {
            if (sh.endMode > 0 && b == 100) { setp (yankiIds::endTrigger, 1.0f); setp (yankiIds::endTrigger, 0.0f); }
        });
        std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
        ed->setSize (YankiEditor::kW, YankiEditor::kH);
        dynamic_cast<YankiEditor*> (ed.get())->snapshotRefresh();
        ok &= save (shot (*ed, scale), dir.getChildFile (sh.file));
        std::cout << sh.file << ": bpm " << p.uiBpm.load() << " ms " << p.uiMs.load() << " dry " << p.uiDryDb.load() << " dB loop "
                  << p.uiLoopDb.load() << " dB swelling " << p.uiSwelling.load() << " preset " << p.currentPreset() << (p.presetEdited() ? " edited" : "") << "\n";
    }
    if (argc > 4) gBpm = std::atof (argv[4]);
    if (argc > 3 && juce::String (argv[3]) == "--moods")   // one shot per MOOD at MIX 40 %: ui_snapshot <dir> <scale> --moods
    {
        for (int m = 0; m < YankiProcessor::moodNames.size(); ++m)
        {
            YankiProcessor p;
            auto setp = [&p] (const char* id, float v) { if (auto* a = p.apvts.getParameter (id)) a->setValueNotifyingHost (a->convertTo0to1 (v)); };
            setp (yankiIds::mood, (float) m);
            feed (p, 8.0, [&] (int b) { if (b == 40) setp (yankiIds::mix, 0.4f); });
            std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
            ed->setSize (YankiEditor::kW, YankiEditor::kH);
            dynamic_cast<YankiEditor*> (ed.get())->snapshotRefresh();
            const auto name = juce::String ("v2_mood_") + juce::String (m) + "_" + YankiProcessor::moodNames[m].replace (" ", "") + "_mix40.png";
            ok &= save (shot (*ed, scale), dir.getChildFile (name));
            std::cout << name << ": ms " << p.uiMs.load() << " dry " << p.uiDryDb.load() << " dB loop " << p.uiLoopDb.load() << " dB\n";
        }
    }
    if (argc > 3 && juce::String (argv[3]) == "--presets")   // one shot per starting-point preset: ui_snapshot <dir> <scale> --presets [bpm]
    {
        for (int i = 0; i < YankiProcessor::kNumPresets; ++i)
        {
            YankiProcessor p;
            p.applyPreset (i);
            if (argc > 5)   // optional MIX scale: ui_snapshot <dir> <scale> --presets <bpm> <mixScale>
                if (auto* a = p.apvts.getParameter (yankiIds::mix))
                    a->setValueNotifyingHost (a->convertTo0to1 (YankiProcessor::kPresets[i].mix * (float) std::atof (argv[5])));
            feed (p, i >= 8 ? 8.0 : 4.0);
            std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
            ed->setSize (YankiEditor::kW, YankiEditor::kH);
            dynamic_cast<YankiEditor*> (ed.get())->snapshotRefresh();
            const auto name = juce::String ("v2_preset_") + juce::String (i) + "_" + juce::String (YankiProcessor::kPresets[i].name).removeCharacters (" ") + ".png";
            ok &= save (shot (*ed, scale), dir.getChildFile (name));
            std::cout << name << ": ms " << p.uiMs.load() << " preset " << p.currentPreset() << "\n";
        }
    }
    std::cout << (ok ? "wrote " : "FAILED writing ") << dir.getFullPathName() << "\n";
    return ok ? 0 : 1;
}
