#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Theme.h"

/*  TUTKAL, Model TK-79: the family face (KORO's plate, keys, knobs and toggles; a cream window with a gain-reduction
    meter), drawn in code at 1600 x 640 logical units and scaled as one piece (fixed 5:2, 800 - 2400 wide).
    The knob row follows the VOICING key: only the controls that act in that voicing are on the face, the others
    keep their values. The rear plate holds SC HPF, DRIVE and EXT. Everything that lights is read from the processor.
*/

class TutkalLookAndFeel : public juce::LookAndFeel_V4
{
public:
    TutkalLookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider&) override;

private:
    juce::SharedResourcePointer<Theme::Faces> faces;   // the embedded typefaces stay loaded while an editor is open
};

/** A cream key; lamps gold when it is the current choice. */
class PlateKey : public juce::Button
{
public:
    PlateKey (const juce::String& text, std::function<bool()> isLit, float textSize = 8.2f, float trackPx = 1.0f);
    void paintButton (juce::Graphics&, bool over, bool down) override;
    static constexpr float kMargin = 9.0f;   // room for the bezel and the lamp glow around the key

private:
    std::function<bool()> lit;
    float size, track;
};

/** A bat-handle toggle: nut, lever, ball tip. Up is on. */
class BatToggle : public juce::Button
{
public:
    explicit BatToggle (float scale = 1.0f) : juce::Button ({}), s (scale) { setClickingTogglesState (true); }
    void paintButton (juce::Graphics&, bool over, bool down) override;

private:
    float s;
};

/** The knob's slider: Option- or Cmd-click puts the parameter back to its default (one undo step), Shift-drag turns it
    five times finer, a double-click opens the value box's type-in field. The wheel is the Slider's own. */
class KnobSlider : public juce::Slider
{
public:
    KnobSlider() = default;
    void bindParameter (juce::RangedAudioParameter* p) { param = p; }
    void resetToDefault();                              // one gesture = one undo step
    std::function<void()> onDoubleClick;
    std::function<void (bool)> onHold;                  // knob grabbed / let go

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    juce::RangedAudioParameter* param = nullptr;
    bool swallow = false;
};

/** The recessed value window under a knob's name: cream digits on a dark glass, the value live while the knob turns.
    A double-click opens an inline text field: Enter sets the value (one undo step, the host is told), Esc or a click
    elsewhere leaves it. Text it cannot read changes nothing. */
class ValueBox : public juce::Component
{
public:
    ValueBox (juce::AudioProcessorValueTreeState&, const juce::String& paramId, std::function<juce::String()> text);
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDoubleClick (const juce::MouseEvent&) override { beginEdit(); }

    void setLive (bool on) { if (live != on) { live = on; repaint(); } }
    juce::String text() const { return textFn ? textFn() : juce::String(); }

    void beginEdit();
    bool isEditing() const { return field != nullptr && field->isVisible(); }
    /** What Enter does with the typed text: true when it was read and applied. */
    bool commitText (const juce::String&);
    void endEdit (bool commit);
    juce::TextEditor* textField() { return field.get(); }

    static constexpr float kW = 128.0f, kH = 30.0f;

private:
    juce::AudioProcessorValueTreeState& apvts;
    juce::String id;
    std::function<juce::String()> textFn;
    std::unique_ptr<juce::TextEditor> field;
    bool live = false, closing = false;
};

/** One knob with its legend, drawn by the panel that owns it. */
struct Knob
{
    juce::String id, name;
    std::vector<std::pair<float, juce::String>> marks;   // angle (deg, 0 = 12 o'clock) -> text around the knob
    juce::Point<float> centre;
    float legendY = 0.0f;                               // 0: below the knob (centre + 1.98 radius)
    float radius = 41.0f, legendSize = 10.5f, ring = 1.66f, markSize = 7.5f;
    enum Style { numbers, intervalMarks, pointer } style = numbers;
    bool ticks = true, stepped = false;
    int layout = -1;                                    // front panel: the voicing layout this knob belongs to (-1: always)
    std::function<juce::String()> readout;              // the value, shown in the value window under the name
    KnobSlider slider;
    std::unique_ptr<ValueBox> box;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

/** A toggle with the legends the panel prints around it. */
struct Toggle
{
    BatToggle lever;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> attachment;
    juce::String label, sub, top = "ON", bottom = "OFF";
    juce::Point<float> centre;
    float scale = 1.0f;
    int layout = -1;
    explicit Toggle (float s) : lever (s), scale (s) {}
};

/** The cream window: the gain-reduction needle, and per voicing the lamps, the 7-segment GR and the notes. */
class DisplayWindow : public juce::Component
{
public:
    explicit DisplayWindow (TutkalAudioProcessor& p) : proc (p) { setInterceptsMouseClicks (false, false); }
    void refresh();                         // one 30 Hz tick: needle ballistics (300 ms), peak hold, repaint when something changed
    void paint (juce::Graphics&) override;

    float needleDb() const { return needle; }

private:
    TutkalAudioProcessor& proc;
    float needle = 0.0f, peak = 0.0f;
    int peakHold = 0;
    juce::String lastKey;
};

class FrontPanel : public juce::Component
{
public:
    FrontPanel (TutkalAudioProcessor&, std::function<void()> showRear);
    void paint (juce::Graphics&) override;
    void resized() override;
    void tick();

    float needleDb() const { return display.needleDb(); }
    PlateKey& voicingKey (int i) { return *voicingKeys[(size_t) i]; }
    Knob* findKnob (const juce::String& id) const;      // the one on the face now (a knob can sit in two voicings)

private:
    int currentLayout() const;
    void showLayout (int layout);
    Knob& addKnob (std::unique_ptr<Knob> k);

    TutkalAudioProcessor& proc;
    juce::Image texture, mark, wordmark;
    std::vector<std::unique_ptr<Knob>> knobs;
    std::vector<std::unique_ptr<Toggle>> toggles;
    std::vector<std::unique_ptr<PlateKey>> voicingKeys;
    PlateKey rearKey;
    DisplayWindow display;
    int layout = -1;
};

class RearPanel : public juce::Component
{
public:
    RearPanel (TutkalAudioProcessor&, std::function<void()> showFront);
    void paint (juce::Graphics&) override;
    void resized() override;
    Knob* findKnob (const juce::String& id) const { return plate.findKnob (id); }

private:
    /** The 1200 x 440 plate, scaled into the 1600 x 640 face. */
    class Plate : public juce::Component
    {
    public:
        Plate (TutkalAudioProcessor&, std::function<void()> showFront);
        void paint (juce::Graphics&) override;
        void resized() override;
        Knob* findKnob (const juce::String& id) const;

    private:
        TutkalAudioProcessor& proc;
        juce::Image texture, mark, wordmark;
        std::vector<std::unique_ptr<Knob>> knobs;
        std::vector<std::unique_ptr<Toggle>> toggles;
        PlateKey frontKey;
    };
    Plate plate;
};

class TutkalEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit TutkalEditor (TutkalAudioProcessor&);
    ~TutkalEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;   // Cmd+Z / Cmd+Shift+Z
    void mouseDown (const juce::MouseEvent&) override;  // takes the keyboard focus (knobs too, via addMouseListener)

    /** For the snapshot tool. */
    void showRear (bool rear);
    void tickForSnapshot() { timerCallback(); }
    FrontPanel& frontPanel() { return front; }
    RearPanel& rearPanel() { return rear; }

    static constexpr int kW = 1600, kH = 640;

private:
    void timerCallback() override;

    TutkalAudioProcessor& proc;
    TutkalLookAndFeel lnf;
    FrontPanel front;
    RearPanel rear;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TutkalEditor)
};
