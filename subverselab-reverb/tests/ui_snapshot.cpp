/*  Draws the editor into a PNG without a screen, so the layout can be checked
    without Screen Recording permission.

        ui_snapshot <out.png> [place 0-2] [decay] [echoSync 0/1]
*/
#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"

int main (int argc, char** argv)
{
    if (argc < 2)
        return 1;

    juce::ScopedJuceInitialiser_GUI init;
    KubbeProcessor processor;
    // Licensed unless UNLICENSED is set in the environment, to draw the activation card.
    processor.license.setLicensedForTesting (std::getenv ("UNLICENSED") == nullptr);
    auto set = [&] (const char* id, float v)
    {
        auto* p = processor.apvts.getParameter (id);
        p->setValueNotifyingHost (p->convertTo0to1 (v));
    };
    if (argc > 2) set ("place", (float) std::atof (argv[2]));
    if (argc > 3) set ("decay", (float) std::atof (argv[3]));
    if (argc > 4) set ("echoSync", (float) std::atof (argv[4]));
    processor.prepareToPlay (48000.0, 512);
    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());

    const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
    juce::File out { juce::String (argv[1]) };
    out.deleteFile();
    juce::FileOutputStream stream (out);
    juce::PNGImageFormat().writeImageToStream (image, stream);
    return 0;
}
