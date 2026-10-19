// The analog parts of the doors (owner 18 Oct: "arayüz analog tarafa uyumlu olmalı"): photoreal fal parts
// (tools/concept/fal_seq_parts.py -> Assets/Panel/seq/*.png) on the Blender door plates (tools/blender/v3/door_v3.py), placed
// from Assets/Panel/doors.json (tools/blender/v3/doors_layout.py), like the face is placed from layout_v3.json.
// A bat-lever switch, a chicken-head selector, a step key (off / on / accent) and a step LED. Each part that drives a host
// parameter is a gesture to the host and follows the parameter (preset loads, automation, slot changes).
#pragma once
#include "Ui.h"

namespace jfui
{
const juce::var& doorsLayout();
const juce::Image& doorPlate (const juce::String& name);          // "seq" / "lab" / "drum": the Blender render at 2x
const juce::Image& partImage (const juce::String& name);          // "switch_up", "led_on", "step_acc", "chicken", ...
juce::Rectangle<int> partBox (const juce::var& c, float d);        // a square of side d (plus room for the shadow) at c

/** a bat-lever toggle: 2 positions (up / down) or 3 (up / mid / down). Every click moves the lever: 2 positions toggle,
    3 positions step mid -> up -> down -> mid. Bound to a host parameter (value index per position) or to plain callbacks. */
class BatSwitch : public juce::Component, public juce::SettableTooltipClient
{
public:
    BatSwitch (JingFangProcessor&, const juce::String& paramId, int positions, int upValue, int downValue, int midValue);
    BatSwitch (std::function<int()> get, std::function<void (int)> set, int positions);   // 0 = down, 1 = mid, 2 = up
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;
    int position() const;                                          // 0 down, 1 mid, 2 up
private:
    void setPosition (int);
    juce::RangedAudioParameter* param = nullptr;
    std::unique_ptr<juce::ParameterAttachment> attachment;
    std::function<int()> getter;
    std::function<void (int)> setter;
    int positions = 2, upV = 1, downV = 0, midV = -1;
};

/** a chicken-head rotary selector for a choice parameter: the choices are printed around it on the plate (-135..+135
    degrees); click steps clockwise, Shift+click or the wheel steps back, drag turns it */
class ChickenSelector : public juce::Component, public juce::SettableTooltipClient
{
public:
    ChickenSelector (JingFangProcessor&, const juce::String& paramId, int count);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    int index() const;
private:
    void setIndex (int);
    juce::RangedAudioParameter* param = nullptr;
    std::unique_ptr<juce::ParameterAttachment> attachment;
    int count = 2, dragFrom = 0;
    bool dragged = false;
};

/** a step key: 2 states (off / on, green window) or 3 (off / on / accent, red window); click cycles, drag paints the state
    of the first key across its neighbours (keys that share a parent and a row) */
class StepKey : public juce::Component, public juce::SettableTooltipClient
{
public:
    StepKey (JingFangProcessor&, const juce::String& paramId, int states);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    int state() const;
    void setState (int);
    juce::String paramId() const { return param != nullptr ? param->getParameterID() : juce::String(); }
    bool playing = false;                                          // the playhead is on this step (a brighter window)
    std::function<void()> onLit;                                   // called when a click turns this step on
private:
    juce::RangedAudioParameter* param = nullptr;
    std::unique_ptr<juce::ParameterAttachment> attachment;
    int states = 2;
    static int paintState;
};

/** a step LED (red when the playhead is on its step) */
class StepLed : public juce::Component
{
public:
    StepLed() { setInterceptsMouseClicks (false, false); }
    void paint (juce::Graphics&) override;
    bool lit = false;
};

/** the photoreal knob for a value that is not a host parameter (the Tuning Lab's own values): same drawing as Knob */
class ValueKnob : public juce::Component, public juce::SettableTooltipClient
{
public:
    float value = 0, lo = 0, hi = 1, def = 0;
    std::function<void (float)> onChange;
    std::function<juce::String (float)> format;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override { from = value; hover = true; dragging = true; }
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override { dragging = false; repaint(); }
    bool dragging = false;
    void mouseDoubleClick (const juce::MouseEvent&) override { set (def); }
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hover = false; repaint(); }
    void set (float v) { value = juce::jlimit (lo, hi, v); if (onChange) onChange (value); repaint(); }
private:
    float from = 0;
    bool hover = false;
};
} // namespace jfui
