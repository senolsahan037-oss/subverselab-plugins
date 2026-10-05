#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "../app/MainComponent.h"

/*  The editor: the MH-86 panel, drawn at its design size and scaled as a whole (fixed aspect ratio), so it fits a
    laptop screen or fills a large one. The zoom is saved with the state. */
namespace mihenk
{
class MihenkEditor : public juce::AudioProcessorEditor
{
public:
    explicit MihenkEditor (MihenkProcessor&);
    ~MihenkEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    MainComponent& getPanel() noexcept { return panel; }
    static float defaultScale();
    static float fitScale();

private:
    MihenkProcessor& processor;
    MainComponent panel;
    std::unique_ptr<juce::TooltipWindow> tooltips;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MihenkEditor)
};
} // namespace mihenk
