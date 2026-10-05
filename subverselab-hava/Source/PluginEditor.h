#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Theme.h"
#if SVL_LICENSING
#include "../../subverselab-plugin-common/Licensing/LicenseOverlay.h"
#endif

/*  HAVA, Model HV-72: the approved face (docs/concepts/hava_front.png,
    hava_front_master_air.png, hava_rear_plate.png), drawn in code at
    1600 x 640 logical units and scaled to the window. Every drawing part is
    the family kit of subverselab-koro/tools/concepts.py (plate, rack ears,
    machined knobs, bat toggles, cream keys, red lamps, red 7-segment digits)
    ported one to one, on KUBBE's palette and fonts. No bitmaps except the
    SubverseLab mark and wordmark.
*/

class HavaLookAndFeel : public juce::LookAndFeel_V4
{
public:
    HavaLookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider&) override;

private:
    juce::SharedResourcePointer<Theme::Faces> faces;   // the embedded panel typefaces, held while a panel is open
};

/** A cream push key of the family (MOOD, REAR, FRONT); lit = gold. */
class KeyButton : public juce::Button
{
public:
    KeyButton (const juce::String& text, float fontSize, float track);
    std::function<bool()> isLit;
    void paintButton (juce::Graphics&, bool over, bool down) override;

private:
    float size, tracking;
};

/** The family bat-handle toggle: up is on. */
class BatToggle : public juce::Button
{
public:
    BatToggle() : juce::Button ({}) { setClickingTogglesState (true); }
    void paintButton (juce::Graphics&, bool over, bool down) override;
};

/** Everything the cream label window shows; the window repaints only when this changes. */
struct DisplayState
{
    juce::String airDigits, gainDigits, latency;
    int liftLamps = 0, sheenLamps[2] { 0, 0 };
    bool liftOn = false, sheenOn = false, safeOn = false;
    bool operator== (const DisplayState& o) const
    {
        return airDigits == o.airDigits && gainDigits == o.gainDigits && latency == o.latency && liftLamps == o.liftLamps
            && sheenLamps[0] == o.sheenLamps[0] && sheenLamps[1] == o.sheenLamps[1]
            && liftOn == o.liftOn && sheenOn == o.sheenOn && safeOn == o.safeOn;
    }
    bool operator!= (const DisplayState& o) const { return ! (*this == o); }
};

/** The cream label window: AIR, SHEEN and MASTER SAFE sections. */
class LabelWindow : public juce::Component
{
public:
    LabelWindow() { setOpaque (true); }
    void show (const DisplayState& s) { if (s != state) { state = s; repaint(); } }
    void paint (juce::Graphics&) override;

private:
    DisplayState state;
};

class FrontPanel : public juce::Component
{
public:
    explicit FrontPanel (HavaProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;

    struct Knob
    {
        juce::String id;
        juce::Slider slider;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    HavaProcessor& proc;
    LabelWindow window;
    Knob air, sheen, output;
    BatToggle bypass, masterSafe;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bypassAttachment, safeAttachment;
    std::vector<std::unique_ptr<KeyButton>> moods;
    KeyButton rearKey { "REAR", 8.2f, 1.6f };
    juce::Image mark, wordmark;
    bool lampOn = false;
    int latencyShown = 0;
};

class RearPanel : public juce::Component
{
public:
    explicit RearPanel (HavaProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;

    HavaProcessor& proc;
    FrontPanel::Knob twang, tension, drip, lowCut;
    KeyButton frontKey { "FRONT", 8.2f, 1.6f };
    juce::Image mark, wordmark;
};

class HavaEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit HavaEditor (HavaProcessor&);
    ~HavaEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;   // Cmd+Z / Cmd+Shift+Z
    void mouseDown (const juce::MouseEvent&) override;  // takes the keyboard focus (knobs too, via addMouseListener)

    void showRear (bool rear);
    /** For the snapshot tool: take the meters as they are now, without ballistics. */
    void settleForSnapshot();

    static constexpr int kW = 1600, kH = 640;   // logical face size

private:
    void timerCallback() override;
    void tick (double dt);

    HavaProcessor& proc;
    HavaLookAndFeel lnf;
    FrontPanel front;
    RearPanel rear;
    float liftShown = 0.0f, sheenShown[2] { -120.0f, -120.0f };
    double lastTick = 0.0;
    int lastMood = -1;

#if SVL_LICENSING
    juce::TextButton licenseButton { "LICENSE" };
    svl::LicenseOverlay licenseOverlay;
#endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HavaEditor)
};
