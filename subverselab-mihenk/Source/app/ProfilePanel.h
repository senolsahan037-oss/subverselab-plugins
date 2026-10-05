#pragma once

#include <JuceHeader.h>
#include "Widgets.h"

/*  Personal profile on the panel (SPEC_V2 §7): the lock badge a learned knob carries, and the PROFILE panel that the
    NOTE key opens over the CRT: the PERSONAL PROFILE switch, a listening note (quick tags, free text, what is saved
    with it) and the list of what was learned from the notes (value, evidence, last update; delete one, reset all,
    export / import). Nothing here is on the audio thread. */
namespace mihenk::ui
{
/** A small gold padlock at a locked knob's shoulder. Hover: why it is locked. Click: unlock. */
class LockBadge : public juce::Component, public juce::SettableTooltipClient
{
public:
    LockBadge() { setMouseCursor (juce::MouseCursor::PointingHandCursor); setRepaintsOnMouseActivity (true); }
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override { if (onClick) onClick(); }
    std::function<void()> onClick;
};

/** Draws a padlock (closed) in `r`. */
void drawPadlock (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c);

class ProfilePanel : public juce::Component, public juce::TooltipClient
{
public:
    explicit ProfilePanel (Context& c);
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    juce::String getTooltip() override;

    /** The session or the notes changed. */
    void refresh();
    /** Snapshot tool: pick tags and type a note. */
    void prefillForSnapshot (const juce::StringArray& tagIds, const juce::String& text);

    std::function<juce::String()> startingPoint;     // the preset / starting point shown as loaded
    std::function<void()> onClose, onChanged;         // onChanged: locks may have changed (re-sync the knobs)

private:
    struct Hit { juce::Rectangle<float> area; juce::String ruleKey; int kind = 0; juce::String tip; };   // kind 0 row, 1 delete
    juce::Rectangle<float> listArea() const;
    juce::Rectangle<float> contextArea() const;
    void saveNote();
    void deleteRule (const juce::String& key, const juce::String& what);
    void resetAll();
    void exportProfile();
    void importProfile();
    void setMessage (const juce::String& m, bool error = false);
    void clearLocksOfRule (const juce::String& key);

    Context& ctx;
    CapButton enable { "PERSONAL PROFILE" }, close { "CLOSE", false };
    std::array<std::unique_ptr<CapButton>, profile::kNumTags> tags;
    juce::TextEditor text;
    CapButton save { "SAVE NOTE", false }, exportKey { "EXPORT", false }, importKey { "IMPORT", false }, resetKey { "RESET ALL", false }, folderKey { "SHOW FILES", false };
    std::unique_ptr<juce::FileChooser> chooser;
    std::vector<Hit> hits;
    juce::Point<float> mouse { -1.0f, -1.0f };
    int scroll = 0;
    juce::String message;
    bool messageIsError = false;
};
} // namespace mihenk::ui
