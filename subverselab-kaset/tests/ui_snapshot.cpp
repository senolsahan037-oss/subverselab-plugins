/*  Draws the editor into a PNG without a screen, so the layout can be checked
    without Screen Recording permission.

        ui_snapshot <out.png> [generation 0-4] [tape 0-2] [level] [tape in 0/1]
*/
#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"

int main (int argc, char** argv)
{
    if (argc < 2)
        return 1;

    juce::ScopedJuceInitialiser_GUI init;
    KasetProcessor processor;
#if SVL_LICENSING
    // Licensed unless UNLICENSED is set in the environment, to draw the activation card.
    processor.license.setLicensedForTesting (std::getenv ("UNLICENSED") == nullptr);
#endif
    auto set = [&] (const char* id, float v)
    {
        auto* p = processor.apvts.getParameter (id);
        p->setValueNotifyingHost (p->convertTo0to1 (v));
    };
    if (argc > 2) set ("generation", (float) std::atof (argv[2]));
    if (argc > 3) set ("tape", (float) std::atof (argv[3]));
    if (argc > 5) set ("play", (float) std::atof (argv[5]));
    // Pretend audio has been playing, so the meters, reels and counter show life.
    const float level = argc > 4 ? (float) std::atof (argv[4]) : 0.12f;
    processor.uiLevel[0] = level; processor.uiLevel[1] = level * 0.85f;
    processor.uiRunning = true; processor.uiTapeSeconds = 431.4;
    processor.prepareToPlay (48000.0, 512);
    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());

    static_cast<KasetEditor*> (editor.get())->settleForSnapshot();
    const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
    juce::File out { juce::String (argv[1]) };
    out.deleteFile();
    juce::FileOutputStream stream (out);
    juce::PNGImageFormat().writeImageToStream (image, stream);
    return 0;
}

