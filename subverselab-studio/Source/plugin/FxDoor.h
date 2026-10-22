// The FX rack door (work order 20 Oct, item 4): rolls down over the modules like PATCH / SEQ / DRUM and holds the FX layer the
// panel has no room for — EQ (with its curve), COMP (with its gain reduction), MOD (chorus / flanger / phaser) and the ECHO's
// type, time and loop. Drawn in code on the doors' dark plate; the knobs are the panel's photoreal knobs (glide, double-click
// to the loaded preset's value). Every value is an ordinary parameter, so presets, the state and the slots keep it.
#pragma once
#include "Ui.h"

namespace jfui
{
class FxDoor : public juce::Component
{
public:
    FxDoor (JingFangProcessor&, std::function<Knob& (const juce::String&, const juce::String&, juce::Rectangle<int>, juce::Component*)> makeKnob,
            juce::Rectangle<int> area);

    void setOpen (bool o) { target = o ? 1.0f : 0.0f; }
    bool isOpenOrOpening() const noexcept { return target > 0.5f; }
    float openAmount() const noexcept { return amount; }
    void snapOpen (bool o) { target = amount = o ? 1.0f : 0.0f; layoutDoor(); }
    void tick();
    void paint (juce::Graphics&) override;

    struct Label { juce::String text; juce::Point<float> at; };
    const std::vector<Label>& labelsForTest() const noexcept { return labels; }

private:
    void layoutDoor();
    JingFangProcessor& proc;
    juce::Rectangle<int> full;
    float amount = 0, target = 0;
    struct Module { juce::String title; juce::Rectangle<float> r; };
    std::vector<Module> modules;
    std::vector<Label> labels;
    std::vector<std::unique_ptr<juce::Component>> owned;
    juce::Rectangle<float> eqView, compView, grView;
    juce::String lastEq, lastComp;
    float shownGr = -1.0f;
};
} // namespace jfui
