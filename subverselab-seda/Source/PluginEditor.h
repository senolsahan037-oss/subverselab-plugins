#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Theme.h"
#include "FamilyDraw.h"
#if SVL_LICENSING
#include "../../subverselab-plugin-common/Licensing/LicenseOverlay.h"
#endif

/*  SEDA, P4: the release panel. "A new 1979 BBD rack delay, fresh from the factory", drawn in code at 1000 x 560 logical units and scaled as one
    piece (SIZE chip: 75 / 85 / 100 %). The family's own parts (FamilyDraw.h: plate keys, bat toggle, knob, value windows, lamps, screws, logo) are
    reused, nothing is invented. Layout, left to right: TIME | LOOP | MOD | STEREO | LISTEN | MIX and BYPASS.

    Everything that moves is read from the processor (the engine's own numbers) at 30 Hz; nothing is faked. A program (SPEC 2.5) is WRITTEN into the knobs
    (they move to its values, one undo step); from then on the knob's position is the value in force, there is no second value behind it.
    VOICE has no control in v1: only BBD is built, the parameter stays for hosts (SPEC 5).
*/
class SedaEditor;

namespace sedaui
{
/** Every control that is bound to a parameter says which one (the editor tests walk the tree and check this). */
struct PanelControl
{
    virtual ~PanelControl() = default;
    virtual juce::String paramId() const = 0;
};

class SedaLookAndFeel : public juce::LookAndFeel_V4
{
public:
    SedaLookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos, float startAngle, float endAngle, juce::Slider&) override;

private:
    juce::SharedResourcePointer<Theme::Faces> faces;   // the embedded typefaces stay loaded while an editor is open
};

/** The knob's slider: Option- or Cmd-click puts the parameter back to its default (one undo step), Shift-drag turns it five times finer,
    a double-click opens the value window's type-in field. The wheel is the Slider's own. */
class KnobSlider : public juce::Slider, public PanelControl
{
public:
    explicit KnobSlider (const juce::String& id) : pid (id) {}
    juce::String paramId() const override { return pid; }
    void bindParameter (juce::RangedAudioParameter* p) { param = p; }
    void resetToDefault();
    std::function<void()> onDoubleClick;
    std::function<void (bool)> onHold;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    juce::String pid;
    juce::RangedAudioParameter* param = nullptr;
    bool swallow = false;
};

/** The recessed value window: cream digits on dark glass, the value live while the knob turns. A double-click opens a type-in field (Enter sets the
    value as one undo step, Esc leaves; text it cannot read changes nothing). `customPaint` replaces the digits (the TIME window draws its seven segments). */
class ValueBox : public juce::Component, public PanelControl
{
public:
    ValueBox (juce::AudioProcessorValueTreeState&, const juce::String& paramId, std::function<juce::String()> text);
    juce::String paramId() const override { return id; }
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDoubleClick (const juce::MouseEvent&) override { beginEdit(); }

    void setLive (bool on) { if (live != on) { live = on; repaint(); } }
    void setDim (bool on) { if (dim != on) { dim = on; repaint(); } }
    /** Repaints the window only when what it would print has changed (the 30 Hz timer calls this for every window). */
    void refreshIfChanged();
    juce::String text() const { return textFn ? textFn() : juce::String(); }
    std::function<void (juce::Graphics&, juce::Rectangle<float>)> customPaint;
    float fontEm = 14.0f;
    std::function<juce::String()> reasonFn;   // shown instead of the value while the control is dimmed: the reason is on the panel

    void beginEdit();
    bool isEditing() const { return field != nullptr && field->isVisible(); }
    bool commitText (const juce::String&);
    void endEdit (bool commit);
    juce::TextEditor* textField() { return field.get(); }

private:
    juce::AudioProcessorValueTreeState& apvts;
    juce::String id;
    std::function<juce::String()> textFn;
    std::unique_ptr<juce::TextEditor> field;
    juce::String lastSig;
    bool live = false, closing = false, dim = false;
};

/** A cream key; gold when it is the current choice. With `lamp` a red LED sits beside it (LISTEN). */
class PlateKey : public juce::Button, public PanelControl
{
public:
    PlateKey (const juce::String& text, const juce::String& param, std::function<bool()> isLit, std::function<void()> onPress,
              float textSize = 8.2f, float trackPx = 1.4f, bool withLamp = false);
    juce::String paramId() const override { return pid; }
    void paintButton (juce::Graphics&, bool over, bool down) override;
    void clicked() override { if (press) press(); }
    void setDimmed (bool d) { if (dimmed != d) { dimmed = d; repaint(); } }
    bool isLit() const { return lit && lit(); }
    bool lastLit = false;   // what the last paint showed (the editor repaints a key only when this changes)
    static constexpr float kMargin = 9.0f;
    static constexpr float kLampW = 30.0f;

private:
    juce::String pid;
    std::function<bool()> lit;
    std::function<void()> press;
    float size, track;
    bool lamp, dimmed = false;
};

/** The bat-handle toggle. Up is "on"; `isUp` says what up means for the bound parameter. */
class BatToggle : public juce::Button, public PanelControl
{
public:
    BatToggle (const juce::String& param, float scale, std::function<bool()> isUp, std::function<void()> onPress)
        : juce::Button ({}), pid (param), s (scale), up (std::move (isUp)), press (std::move (onPress)) {}
    juce::String paramId() const override { return pid; }
    void paintButton (juce::Graphics& g, bool, bool) override { lastUp = up && up(); fam::paintBatToggle (g, getWidth(), getHeight(), lastUp, s); }
    void clicked() override { if (press) press(); }
    bool lastUp = false;

private:
    juce::String pid;
    float s;
    std::function<bool()> up;
    std::function<void()> press;
};

/** A window that shows a choice and steps through it: click = next, Shift- or right-click = previous, the wheel too. */
class NoteWindow : public juce::Component, public PanelControl
{
public:
    NoteWindow (std::function<juce::String()> text, std::function<void (int)> step) : textFn (std::move (text)), stepFn (std::move (step)) {}
    juce::String paramId() const override { return "note"; }
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void setDim (bool d) { if (dim != d) { dim = d; repaint(); } }
    std::function<bool()> isAuto;

private:
    std::function<juce::String()> textFn;
    std::function<void (int)> stepFn;
    bool dim = false;
};

/** What the two displays (the cream ECHO window and the amber VFD program strip) show. The editor fills it from the engine's accessors at 30 Hz. */
struct DisplayState
{
    // meter part (changes every tick while audio runs)
    float wetDb = -40.0f;       // the ECHO needle: wet level after the duck, dB (smoothed, about 300 ms)
    float duckDb = 0.0f;        // <= 0: the red needle
    bool  peak = false;
    // the rest
    bool listen = false, sync = false, noteAuto = false, guard = false;
    float throwLamp = 0.0f, guardDb = 0.0f;
    float runLamp = 0.0f;       // the RUNAWAY LED: 0 off, 0.55 .. 1 lit while the loop gain is over 1.0 (FEEDBACK over 100 % or RUNAWAY)
    int program = 0;            // the SOURCE parameter: 0 MANUAL .. 4 KEYS (the printed legend lights its LED)
    juce::String note, seg, segUnit, bbdLine;   // seg: the repeat time in red seven segments (ms, or the note), segUnit: the printed unit under it
    bool sameMeter (const DisplayState& o) const { return std::abs (o.wetDb - wetDb) < 0.04f && o.duckDb == duckDb && o.peak == peak; }
    bool sameRest (const DisplayState& o) const
    {
        return listen == o.listen && sync == o.sync && noteAuto == o.noteAuto && guard == o.guard && throwLamp == o.throwLamp && guardDb == o.guardDb && runLamp == o.runLamp
            && note == o.note && program == o.program && seg == o.seg && segUnit == o.segUnit && bbdLine == o.bbdLine;
    }
};

/** The cream instrument window (TUTKAL's GR window): ECHO needle, red duck needle on an inner scale, PEAK lamp, red seven-segment repeat time. */
class EchoWindow : public juce::Component
{
public:
    static constexpr float kMaxDb = 12.0f;   // the duck LED bar: 0 .. -12 dB
    static constexpr float kX0 = 486.0f, kY0 = 34.0f, kX1 = 1390.0f, kY1 = 296.0f, kPad = 10.0f;
    void set (const DisplayState& s);
    const DisplayState& state() const { return st; }
    void paint (juce::Graphics&) override;
private:
    DisplayState st;
};

/** A small family-style step key (a triangle on a cream key): steps the VFD program, i.e. the `source` parameter. dir = +1 next, -1 previous. */
class StepKey : public juce::Button, public PanelControl
{
public:
    StepKey (int direction, std::function<void (int)> step) : juce::Button ({}), dir (direction), stepFn (std::move (step)) {}
    juce::String paramId() const override { return "source"; }
    void paintButton (juce::Graphics&, bool over, bool down) override;
    void clicked() override { if (stepFn) stepFn (dir); }
    void setDimmed (bool d) { if (dimmed != d) { dimmed = d; repaint(); } }
private:
    int dir;
    std::function<void (int)> stepFn;
    bool dimmed = false;
};

/** The red LED program name (P1 VOCAL ...): click = next, Shift- or right-click = previous, the wheel steps. Draws nothing: the window draws the LEDs under it. */
class ProgramLine : public juce::Component, public PanelControl
{
public:
    explicit ProgramLine (std::function<void (int)> step) : stepFn (std::move (step)) {}
    juce::String paramId() const override { return "source"; }
    void paint (juce::Graphics&) override {}
    void mouseUp (const juce::MouseEvent& e) override { if (! e.mouseWasDraggedSinceMouseDown()) step (e.mods.isShiftDown() || e.mods.isPopupMenu() ? -1 : 1); }
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override { if (std::abs (w.deltaY) > 0.0f) step (w.deltaY > 0.0f ? 1 : -1); }
    void step (int d) { if (stepFn) stepFn (d); }
private:
    std::function<void (int)> stepFn;
};

class SizeChip : public juce::Component, public PanelControl
{
public:
    explicit SizeChip (SedaEditor& e) : ed (e) {}
    juce::String paramId() const override { return {}; }   // a panel setting, not a parameter (kept in the state as editorScalePct)
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    SedaEditor& ed;
};
} // namespace sedaui

class SedaEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit SedaEditor (SedaAudioProcessor&);
    ~SedaEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override {}
    bool keyPressed (const juce::KeyPress&) override;   // Cmd+Z / Cmd+Shift+Z
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;     // a click on the SubverseLab lockup opens https://subverselab.com
    void mouseMove (const juce::MouseEvent&) override;   // pointing hand over the lockup

    /** UI signature 2: knob `id` to the default of what is loaded now (SedaAudioProcessor::knobDefault), one gesture, the drawing glides. */
    void resetKnob (const juce::String& id);
    /** UI signature 3: where the lockup is drawn (panel units) and the link it opens. */
    juce::Rectangle<float> lockupArea() const;
    static constexpr const char* kLockupUrl = "https://subverselab.com";
    std::function<void()> openLockupLink_forTest;   // tests: set to catch the click instead of opening a browser

    int  scalePercent() const { return proc.editorScalePct.load(); }
    void setScalePercent (int pct);

    // ---- for the tests and the snapshot tool
    void tickForSnapshot() { timerCallback(); }
    const juce::Image& plateImageForTest() const { return plate; }
    /** Every visible control that is bound to a parameter (knobs, value windows, keys, toggles, the note window). */
    std::vector<sedaui::PanelControl*> controls();
    sedaui::KnobSlider* knob (const juce::String& id);
    sedaui::PlateKey* key (const juce::String& id, int value = -1);   // the key of a switch (value -1) or the key for choice index `value`
    sedaui::BatToggle* toggle (const juce::String& id);              // LISTEN, BYPASS, AGE
    float shownDuckDb() const { return display.state().duckDb; }      // the red duck needle and the VFD bar: the engine's number
    float shownEchoDb() const { return display.state().wetDb; }       // the ECHO needle (smoothed)
    float echoTargetDb() const { return wetTarget; }                  // what the needle aims at: the wet peak the processor measured, dB
    bool  throwLampLit() const { return display.state().throwLamp > 0.05f; }
    bool  runawayLampLit() const { return display.state().runLamp > 0.05f; }   // the RUNAWAY LED under the LED display
    juce::String knobLabelText (const juce::String& id) const;                   // what is printed under a knob (RUNAWAY / CHAOS print their value while held)
    juce::String shownNoteText() const { return display.state().note; }
    int   sourceIndex() const { return choiceOf ("source"); }
    sedaui::StepKey* stepKey (int dir) { return dir > 0 ? stepNext.get() : stepPrev.get(); }
    sedaui::ProgramLine* programLine() { return programLn.get(); }
    int   programIndex() const { return display.state().program; }   // what the printed legend lights
    juce::String shownSegText() const { return display.state().seg; }
    juce::String timeWindowText() const;
    bool  widthDimmed() const;
    bool  monoBus() const;   // the main output bus has one channel: the engine runs as MONO (STEREO is shown dimmed)
    bool  autoKeyEnabled() const;

    static constexpr int kW = 1600, kH = 640;

private:
    struct KnobSpec
    {
        juce::String id, name;
        juce::Point<float> centre;
        float radius = 28.0f;
        std::vector<std::pair<float, juce::String>> marks;   // angle (deg, 0 = 12 o'clock) -> short text beside the scale
        std::unique_ptr<sedaui::KnobSlider> slider;
        std::unique_ptr<sedaui::ValueBox> box;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
        bool hasBox = true, big = false, held = false;
        std::function<juce::String()> reason;
    };
    struct ToggleSpec { juce::Point<float> centre; float scale; juce::String top, bottom, label, sub, sub2; std::function<bool()> up; std::unique_ptr<sedaui::BatToggle> lever; };
    struct Legend { juce::String text; float x, y, em; char anchor; juce::Colour col; float track; };
    struct Title { juce::String text; float x0, x1, y; };

    KnobSpec& addKnob (const juce::String& id, const juce::String& name, float cx, float cy, float r, std::vector<std::pair<float, juce::String>> marks, std::function<juce::String()> reason = nullptr, bool withBox = true);
    void addToggle (const juce::String& id, float cx, float cy, float s, const juce::String& top, const juce::String& bottom, const juce::String& label, const juce::String& sub, const juce::String& sub2, std::function<bool()> up, std::function<void()> press);
    sedaui::PlateKey& addKey (const juce::String& text, const juce::String& param, juce::Rectangle<float> box, std::function<bool()> lit, std::function<void()> press,
                              float size = 8.2f, float track = 1.4f, bool lamp = false);
    void addChoiceKey (const juce::String& text, const juce::String& param, int index, juce::Rectangle<float> box, float size = 8.2f, float track = 1.4f);
    void timerCallback() override;
    void refresh();

    float rawOf (const char* id) const { return proc.apvts.getRawParameterValue (id)->load(); }
    bool  onOf (const char* id) const { return rawOf (id) > 0.5f; }
    int   choiceOf (const char* id) const { return juce::roundToInt (rawOf (id)); }
    bool  effSync() const { return onOf ("sync"); }
    int   shownNote() const;                              // the note in force: AUTO's choice, else the one set
    bool  effAuto() const;
    void  noteGrabbed();                                  // the NOTE knob is being turned: AUTO off and SYNC on, in the drag's own undo step
    void  setGesture (const char* id, float plain);
    void  stepNote (int dir);
    void  stepSource (int dir);
    float shownTimeMs() const;                            // TIME as the line plays it: not under the shortest delay of the AGE in force
    juce::String ptextOf (const juce::String& id) const;

    SedaAudioProcessor& proc;
    sedaui::SedaLookAndFeel lnf;
    juce::SharedResourcePointer<Theme::Faces> faces;
    juce::Image plate, mark, wordmark;
    std::vector<std::unique_ptr<KnobSpec>> knobs;
    std::vector<std::unique_ptr<ToggleSpec>> toggles;
    std::vector<std::unique_ptr<sedaui::PlateKey>> keys;
    std::unique_ptr<sedaui::NoteWindow> noteWindow;
    sedaui::EchoWindow display;
    std::unique_ptr<sedaui::StepKey> stepPrev, stepNext;
    std::unique_ptr<sedaui::ProgramLine> programLn;
    std::vector<juce::Rectangle<float>> insets;   // the recessed sub-panels of the bottom band
    std::unique_ptr<sedaui::SizeChip> sizeChip;
    std::vector<Title> titles;
    juce::String sourceLine;
    unsigned paintBits = 0xffffffffu;
    std::vector<Legend> legends;

    void openLockupLink();
    static constexpr float kLogoRight = 1530.0f, kLogoY = 611.0f, kLogoWordH = 17.0f, kLogoMarkH = 26.0f;   // the lockup, bottom right
    struct KnobAnim { juce::String id; float from, to; int tick; };
    std::vector<KnobAnim> anims;   // a program moves the knobs: they glide to their new positions over 8 ticks (the parameters already hold the new values)
    std::unique_ptr<svl::ParamUndo::Group> noteGroup;
    float wetTarget = -90.0f, wetShown = -40.0f;
    int flashTicks = 0, peakTicks = 0;
    bool lastThrowOn = false;
#if SVL_LICENSING
    juce::TextButton licenseButton { "LICENCE" };   // reopens the card when licensed: key status, deactivate this computer
    svl::LicenseOverlay licenseOverlay;   // tester build: the activation card covers the panel until a key is accepted (audio passes through untouched meanwhile)
#endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SedaEditor)
};
