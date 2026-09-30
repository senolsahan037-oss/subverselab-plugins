#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Theme.h"
#if SVL_LICENSING
#include "../../subverselab-plugin-common/Licensing/LicenseOverlay.h"
#endif

/*  The face of a reverb unit as it would have left the factory in 1979:
    brand new, not worn. A dark green anodized rack panel with cream
    silkscreen, machined aluminium knobs, lit push buttons, a phosphor CRT
    for the measured decay, a VFD readout, a needle meter for ducking and
    bat-handle toggles. Everything is drawn in code; no bitmaps except the
    SubverseLab mark.
*/

class KubbeLookAndFeel : public juce::LookAndFeel_V4
{
public:
    KubbeLookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider&) override;

private:
    juce::SharedResourcePointer<Theme::Faces> faces;   // the embedded panel typefaces
};

/** A square push button with a lamp behind its cap. */
class PlaceButton : public juce::Button
{
public:
    PlaceButton (int index, KubbeProcessor& p);
    void paintButton (juce::Graphics&, bool over, bool down) override;

private:
    int index;
    KubbeProcessor& proc;
};

/** A bat-handle toggle under an amber LED. */
class ToggleSwitch : public juce::Button
{
public:
    ToggleSwitch() : juce::Button ({}) { setClickingTogglesState (true); }
    void paintButton (juce::Graphics&, bool over, bool down) override;
};

/** Phosphor CRT: the energy decay of the current settings, measured on an impulse. */
class CrtDisplay : public juce::Component
{
public:
    explicit CrtDisplay (KubbeProcessor& p) : proc (p) {}
    void paint (juce::Graphics&) override;
    void refresh();
    float measuredT60() const { return t60; }

private:
    KubbeProcessor& proc;
    svrb::ReverbEngine engine;
    std::vector<float> envDb, edcDb;
    float binSeconds = 0.02f, t60 = 0.0f;
    juce::String lastKey;
};

/** Four 7-segment VFD digits and a line of text under them. */
class VfdReadout : public juce::Component
{
public:
    void show (const juce::String& digits, const juce::String& caption);
    void paint (juce::Graphics&) override;

private:
    juce::String digits, caption;
};

/** Needle meter for how far the tail is ducked, 0 to -24 dB. */
class DuckMeter : public juce::Component
{
public:
    void setDb (float db);
    void paint (juce::Graphics&) override;

private:
    float needleDb = 0.0f;
};

class KubbeEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit KubbeEditor (KubbeProcessor&);
    ~KubbeEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void buildPanelTexture();
    void paintScale (juce::Graphics&, const juce::Slider&, bool stepped) const;
    std::pair<juce::String, juce::String> readout (int knob) const;

    struct Knob
    {
        juce::String id, name;
        juce::Slider slider;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
        bool stepped = false;
    };

    KubbeProcessor& proc;
    KubbeLookAndFeel lnf;
    CrtDisplay crt;
    VfdReadout vfd;
    DuckMeter meter;
    std::vector<std::unique_ptr<PlaceButton>> places;
    std::vector<std::unique_ptr<Knob>> knobs;

    ToggleSwitch freezeSwitch, echoSyncSwitch;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> freezeAttachment, echoSyncAttachment;

    juce::Image panelTexture, mark, wordmark;
    int touched = -1, lastPlace = -1;
    double touchedAt = 0.0;
    std::vector<float> lastValues;

#if SVL_LICENSING
    juce::TextButton licenseButton { "LICENSE" };
    svl::LicenseOverlay licenseOverlay;
#endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KubbeEditor)
};
