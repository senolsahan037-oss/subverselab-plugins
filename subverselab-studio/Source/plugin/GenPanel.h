// GENERATE on the doors (docs/GENERATE_PLAN.md §8.5). On the SEQ door the 16-column knob field became a piano roll with the
// GENERATE strip above it; the selected step's own knobs and switches (the same host parameters as before) sit beside it.
// On the DRUM door a small ROLL block in the top module. Every value is a host parameter; the roll only draws and edits them.
#pragma once
#include "Analog.h"

namespace jfui
{
/** The sequencer's steps drawn like a DAW clip: piano keys, bar.beat ruler, notes with their names (chord notes stacked), raised /
    lowered steps offset with their cents, accents, slides, chance and ratchets, a velocity lane and the playhead.
    Click a note: select its step (drag up / down: its note). Click an empty place: that step plays that key. Double-click: off.
    Shift-click above a note: add / remove a chord note there. Drag in the velocity lane: velocity. Wheel: scroll the keys. */
class PianoRoll : public juce::Component, public juce::SettableTooltipClient
{
public:
    PianoRoll (JingFangProcessor&, std::function<int()> currentStep);
    void tick();                                     // 30 Hz: repaint when a step or the playhead changed
    int selected() const noexcept { return sel; }
    void select (int s);
    std::function<void (int)> onSelect;
    int lowKey() const noexcept { return low; }      // the lowest key on view (relative to the root, C4 = 0)

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    static constexpr int kKeys = 25, kKeysW = 34, kRulerH = 20, kVelH = 54;

private:
    struct View { bool on; int note, row; float vel, gate, prob; int ratchet; bool slide, accent; std::array<int, 3> chord; };
    std::array<View, 16> read() const;
    float v (const juce::String& id) const;
    void set (const juce::String& id, float value);
    juce::Rectangle<float> grid() const;
    juce::Rectangle<float> velLane() const;
    int stepAt (float x) const;
    int keyAt (float y) const;
    float yOfKey (float key) const;
    void fitView (const std::array<View, 16>&);
    JingFangProcessor& proc;
    std::function<int()> stepNow;
    int sel = 0, low = -6, len = 16, play = -1, dragStep = -1, dragNote = 0, dragStartKey = 0;
    bool dragVel = false;
    juce::String shownSig;
};

/** The SEQ door's GENERATE panel: the strip (direction, ROLL / RHYTHM / NOTES, DENSITY / COMPLEX / MUTATE / RANGE / MICRO, what it
    writes and the seeds, the scale, < > through the last rolls), the piano roll, and the selected step's column. */
class SeqGenPanel : public juce::Component
{
public:
    using KnobMaker = std::function<Knob& (const juce::String&, const juce::String&, juce::Rectangle<int>, juce::Component*)>;
    /** stepControls: the door's own controls of every step by parameter id (NOTE / VEL / GATE / PROB / RATCH knobs, ROW / SLIDE /
        ACCENT switches); the panel shows the selected step's set in its column (door coordinates = panel + origin) */
    SeqGenPanel (JingFangProcessor&, KnobMaker, std::function<int()> currentStep, std::map<juce::String, juce::Component*> stepControls, juce::Point<int> origin);
    void tick();
    void paint (juce::Graphics&) override;
    PianoRoll& roll() noexcept { return pianoRoll; }
    PanelKey* key (const juce::String& text) const;
    void selectStep (int s);

private:
    JingFangProcessor& proc;
    PianoRoll pianoRoll;
    std::map<juce::String, juce::Component*> stepCtl;
    juce::Point<int> origin;
    std::vector<std::unique_ptr<juce::Component>> owned;
    Knob* microKnob = nullptr;
    juce::String shownWrites, shownSeeds;
    bool microShown = true;
};

/** The DRUM door's GENERATE line above the step keys: ROLL, < >, DENSITY / COMPLEX / MUTATE, what it wrote and the seed */
class DrumGenBlock : public juce::Component
{
public:
    DrumGenBlock (JingFangProcessor&, SeqGenPanel::KnobMaker);
    void paint (juce::Graphics&) override;
    void tick();
    PanelKey* rollKey() const noexcept { return roll; }
private:
    JingFangProcessor& proc;
    std::vector<std::unique_ptr<juce::Component>> owned;
    PanelKey* roll = nullptr;
    juce::String shown;
};
} // namespace jfui
