#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Theme.h"
#if SVL_LICENSING
#include "../../subverselab-plugin-common/Licensing/LicenseOverlay.h"
#endif

/*  KORO, Model KR-77: the approved face A2 (docs/concepts/koro_A2_front_label7seg.png)
    and the rear plate (koro_rear_plate.png), drawn in code at 1600 x 640 logical
    units and scaled as one piece. KUBBE's plate, knobs and toggles; KASET's cream
    label with red LEDs and a red 7-segment digit. Everything that lights is driven
    by the processor: nothing on the face is decorative.
*/

class KoroLookAndFeel : public juce::LookAndFeel_V4
{
public:
    KoroLookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider&) override;

private:
    juce::SharedResourcePointer<Theme::Faces> faces;   // the embedded panel typefaces, held while a panel is open
};

/** A cream key; lamps gold when it is the current MOOD. */
class MoodKey : public juce::Button
{
public:
    MoodKey (const juce::String& text, std::function<bool()> isLit, float textSize = 8.2f, float trackPx = 1.0f);
    void paintButton (juce::Graphics&, bool over, bool down) override;
    static constexpr float kMargin = 9.0f;   // room for the bezel and the lamp glow around the key

private:
    std::function<bool()> lit;
    float size, track;
};

/** A bat-handle toggle as on the face: nut, lever, ball tip. Up is on. */
class BatToggle : public juce::Button
{
public:
    explicit BatToggle (float scale = 1.0f) : juce::Button ({}), s (scale) { setClickingTogglesState (true); }
    void paintButton (juce::Graphics&, bool over, bool down) override;

private:
    float s;
};

/** A panel lamp (gold) or LED (red). */
class Lamp : public juce::Component
{
public:
    explicit Lamp (float radius) : r (radius) { setInterceptsMouseClicks (false, false); }
    void setOn (bool on) { if (on != lit) { lit = on; repaint(); } }
    void paint (juce::Graphics&) override;

private:
    float r;
    bool lit = false;
};

/** The cream label window: key digit, scale read-out, confidence, AUTO/LOCK, HARMONY
    latency, COLOR position, SAFE, and the 48-lamp pitch bar. */
class LabelDisplay : public juce::Component
{
public:
    explicit LabelDisplay (KoroProcessor& p) : proc (p) { setInterceptsMouseClicks (false, false); }
    void refresh();
    void paint (juce::Graphics&) override;

private:
    struct State
    {
        bool hasKey = false, manual = false, locked = false, harmony = false, safe = false, inOn = false, outOn = false;
        int root = 0, scale = 0, dots = 0, inLamp = -1, outLamp = -1;
        std::array<bool, 4> colour {};
        bool operator== (const State& o) const
        {
            return hasKey == o.hasKey && manual == o.manual && locked == o.locked && harmony == o.harmony && safe == o.safe
                && inOn == o.inOn && outOn == o.outOn && root == o.root && scale == o.scale && dots == o.dots
                && inLamp == o.inLamp && outLamp == o.outLamp && colour == o.colour;
        }
    };
    KoroProcessor& proc;
    State st;
};

/** Stereo VU needle meter of the output, 0 VU = -18 dBFS RMS. */
class VuMeter : public juce::Component
{
public:
    VuMeter() { setInterceptsMouseClicks (false, false); }
    void setLevels (float rmsL, float rmsR);
    void paint (juce::Graphics&) override;

private:
    float vu[2] { -30.0f, -30.0f };
};

/** One knob with its legend, drawn by the panel that owns it. */
struct Knob
{
    juce::String id, name, sub;
    std::vector<std::pair<float, juce::String>> marks;   // angle (deg) -> text around the knob
    juce::Point<float> centre;
    float radius = 41.0f, legendSize = 10.5f;
    enum Style { numbers, intervalMarks, pointer } style = numbers;   // how the scale around it is printed
    float ring = 1.66f, markSize = 0.0f;
    bool stepped = false, ticks = true;
    juce::Slider slider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

class FrontPanel : public juce::Component
{
public:
    FrontPanel (KoroProcessor&, std::function<void()> showRear);
    void paint (juce::Graphics&) override;
    void resized() override;
    void tick();

private:
    KoroProcessor& proc;
    juce::Image texture, mark, wordmark;
    std::vector<std::unique_ptr<Knob>> knobs;
    std::vector<std::unique_ptr<MoodKey>> moods;
    MoodKey rearKey;
    BatToggle lockToggle, bypassToggle;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> lockAttachment, bypassAttachment;
    Lamp lockLamp { 5.5f }, latencyLamp { 5.5f };
    LabelDisplay display;
    VuMeter vu;
    int lastMood = -1;
};

class RearPanel : public juce::Component
{
public:
    RearPanel (KoroProcessor&, std::function<void()> showFront);
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    /** The 1200 x 440 plate of the concept, scaled into the 1600 x 640 face. */
    class Plate : public juce::Component
    {
    public:
        Plate (KoroProcessor&, std::function<void()> showFront);
        void paint (juce::Graphics&) override;
        void resized() override;

    private:
        KoroProcessor& proc;
        juce::Image texture, mark, wordmark;
        std::vector<std::unique_ptr<Knob>> knobs;
        BatToggle monoSafe { 0.8f };
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> monoSafeAttachment;
        MoodKey frontKey;
    };
    Plate plate;
};

class KoroEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit KoroEditor (KoroProcessor&);
    ~KoroEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;   // Cmd+Z / Cmd+Shift+Z
    void mouseDown (const juce::MouseEvent&) override;  // takes the keyboard focus (knobs too, via addMouseListener)

    /** For the snapshot tool. */
    void showRear (bool rear);
    void settleForSnapshot() { for (int i = 0; i < 60; ++i) timerCallback(); }

    static constexpr int kW = 1600, kH = 640;

private:
    void timerCallback() override;

    KoroProcessor& proc;
    KoroLookAndFeel lnf;
    FrontPanel front;
    RearPanel rear;

#if SVL_LICENSING
    juce::TextButton licenseButton { "LICENSE" };
    svl::LicenseOverlay licenseOverlay;
#endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KoroEditor)
};
