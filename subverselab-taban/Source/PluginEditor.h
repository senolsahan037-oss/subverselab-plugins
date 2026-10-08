#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Theme.h"

class TabanLookAndFeel : public juce::LookAndFeel_V4
{
public:
    static constexpr float kCapW = 46.0f, kCapH = 26.0f;

    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider&) override;
    void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float minPos, float maxPos, const juce::Slider::SliderStyle, juce::Slider&) override;
    /** The cap's half height: the slider's travel then runs exactly between the cap's centre positions. */
    int getSliderThumbRadius (juce::Slider&) override { return (int) (kCapH * 0.5f); }
};

class BatToggle : public juce::Button
{
public:
    BatToggle() : juce::Button ({}) { setClickingTogglesState (true); }
    void paintButton (juce::Graphics&, bool over, bool down) override;
};

class TabanAudioProcessorEditor : public juce::AudioProcessorEditor,
                                  private juce::Timer
{
public:
    static constexpr int kW = 760, kH = 500;

    explicit TabanAudioProcessorEditor (TabanAudioProcessor&);
    ~TabanAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    /** Steps the meters by dt seconds from the processor's current values (the timer calls it; so does ui_snapshot). */
    void advance (float dt);

private:
    void timerCallback() override;
    /** y of a dB value on the fader's own scale, in editor coordinates. */
    float faderY (float db) const;

    TabanAudioProcessor& proc;
    TabanLookAndFeel lookAndFeel;

    juce::Slider floorFader;
    juce::Slider outputKnob;
    BatToggle inToggle;

    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> floorAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> outputAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> inAttach;

    // Meter ballistics (dB): instant rise, falling at kFallDbPerS; the lift read-out holds its peak briefly.
    float shownIn = -200.0f, shownOut = -200.0f, shownLift = 0.0f, liftHold = 0.0f;

    juce::Image plate, mark, wordmark;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TabanAudioProcessorEditor)
};
