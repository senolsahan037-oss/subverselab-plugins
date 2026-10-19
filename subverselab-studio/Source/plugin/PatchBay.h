// The cable compartment: a door that rolls down over the modules (owner 18 Oct: on a plate like the other doors and inside the
// panel) with the three LFOs, a row of source jacks, two rows of destination jacks and the 16 amount attenuators, all placed
// from Assets/Panel/doors.json ("patch") on the Blender plate door_patch_2x.png; the jacks are photoreal fal parts. A cable from a source to a destination is one slot of the engine's
// modulation matrix (modN.source / modN.destination / modN.amount); the plug-in state stores it like any other parameter.
#pragma once
#include "Analog.h"

namespace jfui
{
class PatchBay : public juce::Component
{
public:
    static constexpr int kFullW = 1162, kFullH = 575;   // the door area (doors.json "size")
    static constexpr int kNumSrc = (int) svl::ModSrc::count - 1, kNumDst = (int) svl::ModDst::count - 1;

    PatchBay (JingFangProcessor&, std::function<Knob& (const juce::String&, const juce::String&, juce::Rectangle<int>, juce::Component*)> makeKnob);

    void setOpen (bool shouldBeOpen) { target = shouldBeOpen ? 1.0f : 0.0f; }
    bool isOpenOrOpening() const noexcept { return target > 0.5f; }
    float openAmount() const noexcept { return amount; }
    void tick();                       // 30 Hz from the editor: the door and the cables
    void snapOpen (bool o) { target = amount = o ? 1.0f : 0.0f; layoutDoor(); }

    // geometry and actions (also used by the checks)
    juce::Point<float> jack (bool source, int index) const;   // index 0-based within the source / destination list
    bool connect (int src, int dst);   // src / dst: ModSrc / ModDst values (1-based); returns false if all 16 slots are in use
    void disconnect (int slot);
    int cableCount() const;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    void layoutDoor();
    int hitJack (juce::Point<float>, bool& isSource) const;
    int slotValue (int slot, svl::ModField f) const;
    static juce::Colour cableColour (int slot);
    void drawCable (juce::Graphics&, juce::Point<float> a, juce::Point<float> b, juce::Colour, float alpha) const;

    JingFangProcessor& proc;
    float amount = 0, target = 0;
    std::vector<std::unique_ptr<juce::Component>> owned;
    int dragSrc = -1;                  // ModSrc value while a cable is in the hand
    juce::Point<float> dragPos;
    juce::String lastState;
};
} // namespace jfui
