// JING FANG panel: every engine parameter in sections (3 oscillators, filter, amp / filter envelopes, pitch / click / drum,
// drift / floor, voice, tuning), the preset browser across the top, the 3-row keyboard at the bottom with the normal / micro
// switch beside it. Drawn at 1280 x 760 logical units and scaled as one piece.
#pragma once
#include "Ui.h"
#include "Keyboard.h"
#include "PatchBay.h"
#include "Scopes.h"
#include "AnalogDoor.h"
#include "LabDoor.h"
#include "FxDoor.h"

class JingFangEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit JingFangEditor (JingFangProcessor&);
    ~JingFangEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    bool keyStateChanged (bool isKeyDown) override;
    void focusLost (FocusChangeType) override { keyboard.releaseAllComputerKeys(); }

    // ---- tests
    jfui::Knob* knob (const juce::String& paramId) const;
    const std::vector<std::unique_ptr<jfui::Knob>>& allKnobs() const noexcept { return knobs; }
    jfui::Help& helpOverlay() noexcept { return help; }
    jfui::Keyboard3& keyboardView() noexcept { return keyboard; }
    jfui::Lockup& lockupLink() noexcept { return lockup; }
    juce::Component& panelView() noexcept { return panel; }
    jfui::PanelKey& helpKey() noexcept { return helpBtn; }
    jfui::PanelKey& microKey() noexcept { return microBtn; }
    jfui::PanelKey& normalKey() noexcept { return normalBtn; }
    jfui::PanelKey& patchKey() noexcept { return patchBtn; }
    juce::Component* slotCell (int s) const { return s >= 0 && s < (int) slotCells.size() ? slotCells[(size_t) s].get() : nullptr; }
    jfui::PatchBay& patchBay() noexcept { return bay; }
    jfui::AnalogDoor* seqDoorView() noexcept { return seqDoor.get(); }
    jfui::AnalogDoor* drumDoorView() noexcept { return drumDoor.get(); }
    jfui::PanelKey& drumKey() noexcept { return drumBtn; }
    jfui::PanelKey& closeKey() noexcept { return closeBtn; }
    jfui::PanelKey& seqKey() noexcept { return seqBtn; }
    jfui::LabDoor* labDoorView() noexcept { return labDoor.get(); }
    jfui::FxDoor* fxDoorView() noexcept { return fxDoor.get(); }
    void openDoorForTest (int which) { openDoor (which); }
    jfui::PanelKey& labKey() noexcept { return labBtn; }
    void tickForTest() { timerCallback(); }
    struct SaveTarget { juce::String family, name; bool fixed = false; };   // where SAVE puts a preset now (the open door decides)
    SaveTarget saveTarget() const;
    jfui::PanelKey& saveKey() noexcept { return saveBtn; }
    juce::Component& bpmReadout() noexcept { return bpmFace; }
    juce::String readoutText() const { return bpmFace.shownText(); }
    std::vector<std::pair<juce::String, juce::Rectangle<int>>> sectionsForTest() const;
    juce::String layoutJson() const;   // every section, knob, display and the keyboard, for the Blender scene

private:
    struct Section { juce::String title; juce::Rectangle<int> r; };
    struct Panel : juce::Component
    {
        explicit Panel (JingFangEditor& e) : ed (e) {}
        void paint (juce::Graphics&) override;
        JingFangEditor& ed;
    };
    /** the dragon (owner 17 Oct: "üzerine koy"): one RGBA layer drawn above the controls; clicks pass through it */
    struct DragonLayer : juce::Component
    {
        DragonLayer() { setInterceptsMouseClicks (false, false); setComponentID ("dragon"); }
        void paint (juce::Graphics&) override;
    };
    /** one slot of the preset synthesizer: the panel's name, the preset number it takes that panel from, the preset's name.
        Left third steps down, right third steps up, the middle opens the list, the wheel steps. */
    struct SlotCell : juce::Component, juce::SettableTooltipClient
    {
        SlotCell (JingFangProcessor& p, int s) : proc (p), section (s)
        { setMouseCursor (juce::MouseCursor::PointingHandCursor); setTooltip (juce::String (JingFangProcessor::sectionName (s)) + ": the preset this panel comes from"); }
        void paint (juce::Graphics&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override { if (w.deltaY != 0.0f) proc.stepSection (section, w.deltaY > 0 ? -1 : 1); }
        void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
        void mouseExit (const juce::MouseEvent&) override { hover = false; repaint(); }
        JingFangProcessor& proc;
        int section;
        bool hover = false;
    };
    /** BPM on its own small screen outside the preset arrows: SAVE ? [BPM] < preset > PATCH SEQ DRUM (owner 19 Oct: "BPM yok";
        "ok tuşları bunların dışında kalmış"; KEY taken out again: "key DAW'a kilitlenmiyorsa key koymaya gerek yok"). BPM: drag / wheel, double-click
        120; in a DAW the host's tempo with HOST and a lock, not editable (RU-8 rule 1). */
    struct Readout : juce::Component, juce::SettableTooltipClient
    {
        explicit Readout (JingFangProcessor& p) : proc (p) { setMouseCursor (juce::MouseCursor::UpDownResizeCursor); }
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
        juce::String shownText() const;                      // what it shows now (the timer repaints on a change)
        JingFangProcessor& proc;
        float dragFrom = 120;
    };
    struct PresetFace : juce::Component
    {
        explicit PresetFace (JingFangProcessor& p) : proc (p) { setMouseCursor (juce::MouseCursor::PointingHandCursor); }
        void paint (juce::Graphics&) override;
        void mouseUp (const juce::MouseEvent&) override;
        JingFangProcessor& proc;
    };

    void timerCallback() override;
    jfui::Knob& addKnob (const juce::String& id, const juce::String& label, juce::Rectangle<int> cell, juce::Component* parent = nullptr);
    void build();
    void addScope (jfui::Scope*, juce::Rectangle<int>);
    void savePreset();

    JingFangProcessor& proc;
    // keeps the embedded typefaces alive while the editor is open: heros() / mono() look them up through a
    // SharedResourcePointer, which otherwise built all three typefaces again on every call (measured 18 Oct: ~60 % CPU while
    // the doors painted, the owner: "takıldı mı")
    juce::SharedResourcePointer<jfui::Faces> faces;
    Panel panel { *this };
    std::vector<Section> sections;
    std::vector<std::unique_ptr<jfui::Knob>> knobs;
    std::vector<std::unique_ptr<juce::Component>> others;
    std::vector<jfui::Scope*> scopes;
    PresetFace presetFace { proc };
    Readout bpmFace { proc };                                  // its own screen outside the preset arrows (owner 19 Oct)
    jfui::PanelKey prevBtn { "" }, nextBtn { "" }, saveBtn { "SAVE" }, helpBtn { "?" }, patchBtn { "PATCH" }, seqBtn { "SEQ" }, drumBtn { "DRUM" }, normalBtn { "NORMAL" }, microBtn { "MICRO" }, labBtn { "LAB" }, closeBtn { "CLOSE" };
    jfui::Keyboard3 keyboard { proc };
    jfui::PatchBay bay { proc, [this] (const juce::String& id, const juce::String& l, juce::Rectangle<int> r, juce::Component* parent) -> jfui::Knob&
                         { return addKnob (id, l, r, parent); } };
    jfui::Lockup lockup;
    DragonLayer dragon;
    std::unique_ptr<jfui::AnalogDoor> seqDoor, drumDoor;
    std::unique_ptr<jfui::LabDoor> labDoor;
    std::unique_ptr<jfui::FxDoor> fxDoor;
    jfui::PanelKey fxBtn { "FX RACK" };
    bool anyDoorOpen() const;
    juce::Rectangle<int> doorArea;                   // where the SEQ / DRUM / LAB doors roll down (help hides what they cover)
    void stopEverything();                                     // Esc on the panel
    bool seqWasRunning = false, drumWasRunning = false;
    juce::String seqScratchFor;                                // the loaded preset the SEQ door last cleared for
    void openDoor (int which);                                 // 0 none, 1 PATCH, 2 SEQ, 3 LAB, 4 DRUM, 5 FX: one door at a time
    std::vector<std::unique_ptr<SlotCell>> slotCells;
    juce::String lastSlots, lastReadouts;
    int probeTick = 0, probeGaps = 0, probeStalls = 0;
    double probeLastTick = 0, probeGapSum = 0, probeGapMax = 0;
    jfui::Help help;
    juce::String lastFace, lastTuning;
    std::unique_ptr<juce::AlertWindow> saveWindow;
    juce::TooltipWindow tooltips { this, 600 };
    int focusTick = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (JingFangEditor)
};
