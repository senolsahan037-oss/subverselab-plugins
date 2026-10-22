// A door that rolls down over the modules (SEQ, DRUM), built from its entry in Assets/Panel/doors.json: the Blender plate
// (door_<name>_2x.png) with the photoreal knobs, bat-lever switches, chicken-head selectors, step keys and step LEDs on it.
// Owner 18 Oct: the doors are analog like the face ("arayüz analog tarafa uyumlu olmalı"; "iki arayüz birbiriyle tutarlı").
// Every control is a host parameter; one the engine does not have is left out (its print stays on the plate).
#pragma once
#include "Analog.h"

namespace jfui
{
class AnalogDoor : public juce::Component
{
public:
    using KnobMaker = std::function<Knob& (const juce::String&, const juce::String&, juce::Rectangle<int>, juce::Component*)>;
    AnalogDoor (JingFangProcessor&, KnobMaker, juce::Rectangle<int> fullBounds, juce::String name, std::function<int()> currentStep);

    void setOpen (bool o) { target = o ? 1.0f : 0.0f; }
    bool isOpenOrOpening() const noexcept { return target > 0.5f; }
    void snapOpen (bool o) { target = amount = o ? 1.0f : 0.0f; layoutDoor(); }
    void tick();                                     // 30 Hz: the roll and the playhead
    int controlCount() const noexcept { return (int) owned.size() + knobCount; }
    int stepKeyCount() const noexcept { return (int) keys.size(); }
    const juce::String& doorName() const noexcept { return name; }

    void paint (juce::Graphics&) override;

    /** a drum track's sound: the window shows the preset it plays, a click lists the drum presets */
    struct TrackPicker : juce::Component, juce::SettableTooltipClient
    {
        TrackPicker (JingFangProcessor& p, int t) : proc (p), track (t) { setMouseCursor (juce::MouseCursor::PointingHandCursor); setTooltip ("The sound this track plays"); }
        void paint (juce::Graphics&) override;
        void mouseUp (const juce::MouseEvent&) override;
        JingFangProcessor& proc;
        int track;
        juce::String shown;
    };

private:
    void layoutDoor();
    JingFangProcessor& proc;
    juce::Rectangle<int> full;
    juce::String name;
    std::function<int()> stepNow;
    float amount = 0, target = 0;
    int knobCount = 0, lastStep = -2, shownLength = -1, playSig = -1;
    bool tempoHostShown = false;
    std::vector<std::unique_ptr<juce::Component>> owned;
    std::vector<std::pair<int, StepLed*>> leds;            // step index, LED
    std::vector<std::pair<int, StepKey*>> keys;            // step index (column), key
    std::vector<PanelKey*> playKeys;
    std::vector<std::pair<PanelKey*, int>> sourceKeys;      // PATTERN / MIDI key, unit (0 SEQ, 1 DRUM)
    std::vector<std::pair<ValueKnob*, juce::Point<float>>> tempoKnobs;   // knob, where its BPM is printed
    std::array<std::vector<Knob*>, 3> lanes;                // SEQ: the VEL row's lanes (VEL, PROB, RATCH), one shown at a time
    int shownLane = 0;
    PanelKey* laneKey = nullptr;
public:
    void showLaneForTest (int l) { while (shownLane != l && laneKey != nullptr) laneKey->onClick(); }
    int laneKnobCount (int l) const { return (int) lanes[(size_t) l].size(); }
};
} // namespace jfui
