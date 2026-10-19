// The Tuning Lab door (SPEC.md "Tuning Lab"): rolls down over the modules like the SEQ door, opened by LAB beside NORMAL /
// MICRO. Built like the other doors: the Blender plate (door_lab_2x.png) and the photoreal parts placed from doors.json. Two tunings (A, B): load .scl / .kbm, learn from a recording, export; MORPH between them; directional and adaptive
// switches; a pitch wheel with both tunings and the sounding notes; the intervals of what sounds (ratio, cents off, beats).
// Shipped locked: everything is drawn, nothing is touchable, and the processor ignores the Lab (JingFangProcessor::labActive).
#pragma once
#include "Analog.h"

namespace jfui
{
class LabDoor : public juce::Component, public juce::FileDragAndDropTarget
{
public:
    LabDoor (JingFangProcessor&, juce::Rectangle<int> fullBounds);

    void setOpen (bool o) { target = o ? 1.0f : 0.0f; }
    bool isOpenOrOpening() const noexcept { return target > 0.5f; }
    void snapOpen (bool o) { target = amount = o ? 1.0f : 0.0f; layoutDoor(); }
    void tick();                                     // 30 Hz: the door, the wheel's sounding notes, intervals, drift
    bool locked() const noexcept { return ! JingFangProcessor::labUnlocked(); }
    /** LEARN is open (picking the recording or listening to it): it works on this page, the other pages wait (owner 19 Oct:
        "hangi sayfada learn açıksa learn o sayfada çalışır; learn açıkken sayfalar arası gezinti olmaz") */
    bool learnOpen() const noexcept { return learning || pickingToLearn; }

    void paint (juce::Graphics&) override;
    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int x, int y) override;

    // tests
    juce::String statusText() const { return status; }
    void loadFileForTest (const juce::File& f, int which) { loadFile (f, which); }
    void loadBuiltinForTest (int which, int entry) { loadBuiltin (which, entry); }

private:
    void layoutDoor();
    void applyLock();                                // enable / dim the controls when the lock state changes
    void apply (const std::function<void (JingFangProcessor::LabState&)>& change);
    void loadFile (const juce::File&, int which);
    void choose (int which, bool learn);
    void loadMenu (int which);                       // LOAD: the built-in tunings, or a file
    void loadBuiltin (int which, int entry);
    void exportTuning (int which);
    void drawCard (juce::Graphics&, juce::Rectangle<float>, int which);
    void drawWheel (juce::Graphics&, juce::Rectangle<float>);
    void drawIntervals (juce::Graphics&, juce::Rectangle<float>);

    JingFangProcessor& proc;
    juce::Rectangle<int> full;
    float amount = 0, target = 0;
    std::vector<std::unique_ptr<juce::Component>> owned;
    juce::Rectangle<int> cardA, cardB, wheel, list, statusBox;
    juce::String status;
    juce::String shownState;                         // what the cards show (name, cents, switches), to redraw on outside changes
    std::vector<std::pair<int, float>> sounding;
    float drift = 0;
    int dir = 1;
    bool learning = false, pickingToLearn = false, shownLocked = false;
    std::unique_ptr<juce::FileChooser> chooser;
    std::vector<std::pair<juce::String, ValueKnob*>> labKnobs;   // morph / window / pull
};
} // namespace jfui
