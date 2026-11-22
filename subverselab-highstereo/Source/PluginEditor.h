#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "HsWidgets.h"
#include "HsStage.h"
#include "HsField.h"
#include "SvlMarkLink.h"

/** HIGH STEREO panel (SPEC.md section 7, sketch v7, owner sessions 20-21 Nov 2026). 960 x 520.
    Left: STAGE (360 goniometer + puck). Right: WIDTH field. Bottom: round controls, switches, phase bar.
    Bottom left: the hover summary (family rule). Bottom right: the SubverseLab icon, linked to the manual. */
class HighStereoAudioProcessorEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit HighStereoAudioProcessorEditor (HighStereoAudioProcessor&);
    ~HighStereoAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    static constexpr int kW = 960, kH = 520;
    static constexpr const char* kManualUrl = "https://subverselab.com/plugins/high-stereo/manual";

    // snapshot tool / tests
    void tickForSnapshot() { timerCallback(); }
    void setHoverForSnapshot (juce::Component* c) { forcedHover = c; timerCallback(); }
    juce::Component* controlNamed (const juce::String& id);
    juce::String currentSummary() const { return summaryText; }
    svl::MarkLink& markLink() { return mark; }
    hsui::WidthField& widthField() { return field; }

private:
    void timerCallback() override;
    float loadedValue (const juce::String& id) const;

    HighStereoAudioProcessor& proc;
    hsui::PresetBar presetBar;
    hsui::ListenBar listenBar;
    hsui::StageScope stage;
    hsui::WidthField field;
    std::unique_ptr<hsui::RoundControl> width, spread, monoBass, output, mix;
    std::unique_ptr<hsui::SlideSwitch> inSw, matchSw, fixSw;
    hsui::PhaseBar phase;
    hsui::GuardMeter guard;
    svl::MarkLink mark { kManualUrl };
    juce::Image wordmark, inkMark;
    juce::String summaryText;
    juce::Component* forcedHover = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HighStereoAudioProcessorEditor)
};
