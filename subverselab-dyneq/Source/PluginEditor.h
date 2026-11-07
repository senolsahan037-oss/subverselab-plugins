#pragma once

#include <JuceHeader.h>
#include <optional>
#include <map>
#include "PluginProcessor.h"
#include "PanelModel.h"
#include "FamilyDraw.h"
#include "Theme.h"
#if SVL_LICENSING
#include "../../subverselab-plugin-common/Licensing/LicenseOverlay.h"
#endif

/*  ADALET / DYNEQ: the panel. LAB, a flat digital screen (Pro-Q's display with EQ Eight's band strip: docs/ui/UX_REDESIGN_PROQ_EQ8.md, 11 Oct).
    The second face of P4 (ORBIT, a round case) was removed on 8 Nov 2026 (owner: two faces confused); 1.0.0 ships with LAB only.
    The controls edit the parameters through one panel::Ctl. Window 1200 x 540 at 100 %, offered at 75 / 85 %.
    The product name is Theme::kProductName and nowhere else. */

/** A control of the panel: what it writes (parameter ids) or, when it only changes the view, what it does. The editor tests walk the tree with this. */
struct PanelControl
{
    virtual ~PanelControl() = default;
    virtual juce::StringArray boundParams() const = 0;
    virtual juce::String uiAction() const { return {}; }
    virtual bool isDeadNow() const { return false; }
    /** The parameters a double-click (or Alt-click) on this control returns to their defaults; empty = not a knob, ring, value box or slider. */
    virtual juce::StringArray resetParams() const { return {}; }
};

/** Counts the live objects of the panel (the leak test opens and closes the editor and wants this back at 0). */
struct PanelCounted
{
    PanelCounted() { ++count(); }
    ~PanelCounted() { --count(); }
    static std::atomic<int>& count() { static std::atomic<int> n { 0 }; return n; }
};

namespace panelui
{
class LabFace;
class SizeChip;
class Chooser;
class TopBar;
class AssistOverlay;
class AdvancedPanel;
class BandChip;
class BandStrip;
class DynPop;
class AssistChip;
class LabFooter;
class HelpTip;
}

class DyneqEditor : public juce::AudioProcessorEditor, private juce::Timer, private PanelCounted
{
public:
    explicit DyneqEditor (DyneqAudioProcessor&);
    ~DyneqEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;   // Cmd+Z / Cmd+Shift+Z, Delete = delete the selected band
    bool keyStateChanged (bool isKeyDown) override;     // hold D = DELTA
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;          // the hover help
    void mouseExit (const juce::MouseEvent&) override;

    void tickForSnapshot() { timerCallback(); }
    static constexpr int kW = 1200, kH = 540;            // 11 Oct, owner: "yatay pencereyi yap 1200x600", then "boyu biraz kısaltabiliriz" (an EQ Eight-like strip; was 1000 x 960)
    static constexpr int kTimerHz = 40;                  // the analyzer and the curves redraw 40 times a second
    static int liveObjects() { return PanelCounted::count().load(); }

    panel::Ctl& ctl() { return model; }
    panel::Curves& curves() { return cv; }
    panel::Spectrum& spectrum() { return spec; }
    float inDbShown() const { return inShown; }
    float outDbShown() const { return outShown; }
    int  spectrumFrames() const { return spec.frames; }  // how many analyzer frames the timer has computed

    /** Opens the TARGET instrument / part list at a point in editor coordinates. */
    bool openChooser (juce::Point<int> where);
    /** Opens any list (shape, slope, a band's right-click menu) at a point in editor coordinates. */
    void openItems (std::vector<panel::ChooserItem> items, juce::Point<int> where);
    void closeChooser();
    bool chooserOpen() const;
    int  chooserCount() const;
    juce::String chooserText (int i) const;
    bool chooserSelected (int i) const;
    bool chooserHeading (int i) const;
    void chooserPick (int i);

    // ASSIST overlays (TARGET / COLLIDE / SELF = panel::kTarget .. kSelf) and ADVANCED
    int  overlayId() const { return overlay; }                  // -1 = none
    void toggleOverlay (int m);
    void closeOverlay();
    bool advancedOpen() const;                                  // the ADVANCED popover is shown
    void setAdvancedOpen (bool open);
    bool dynOpen() const { return dynShown; }                   // the "..." popover of the strip (the selected band's dynamics) is open
    void setDynOpen (bool open);

    // LAB (UX redesign 11 Oct)
    int  hoverChip() const { return hoverChipBand; }            // the band chip under the pointer (its curve is lit on the display), -1 = none
    void setHoverChip (int b) { hoverChipBand = b; }
    bool keyTraceOn() const { return keyTrace; }                // the analyzer's sidechain trace (shown while COLLIDE runs on a connected key)
    void setKeyTrace (bool on) { keyTrace = on; refreshFace(); }
    void editFreqBox();                                         // a double-click on a node: typing in the strip's FREQ box
    bool labKey (const juce::KeyPress& k);                      // 1-6, Esc, Delete, arrows, Tab, Return, B
    juce::String advancedBakeText() const;                      // the BAKE line of the ADVANCED popover (state | next step), as drawn

    // hover help (25 Oct, UI signature standard 4: help is a popover over the panel; the window never grows). A control registers a title and a text
    // (asked when the popover opens, so it can say what is true now); the popover opens after kHelpDelayTicks over it and closes on leave, click or Escape.
    using HelpFn = std::function<juce::String()>;
    void setHelp (juce::Component& c, const juce::String& title, HelpFn body);
    bool showHelpFor (juce::Component& c);                      // opens c's popover now (the hover path, snapshots and tests); false when c has none
    void hideHelp();
    bool helpShown() const;
    juce::String helpTitle() const;
    juce::String helpText() const;
    static constexpr int kHelpDelayTicks = 20;                  // 0.5 s at kTimerHz

    void setScalePercent (int pct);                      // 75, 85 or 100
    int  scalePercent() const { return proc.editorScalePct.load(); }
    void refreshFace();                                  // brings the panel up to date (popovers, strip, chips) after an edit; the name is from the two-face days
    // test hooks: the node of a band in editor coordinates, and the hover of the node read-out
    juce::Point<float> testNodePosition (int band);
    juce::Point<float> testLabPoint (double hz, double db);     // the display's own coordinates of a frequency and a level
    void testHover (int band);
    void testPointer (std::optional<juce::Point<float>> labPoint);   // the pointer over the LAB display (its own coordinates), or away
    juce::String testReadout (int band);                 // the node read-out text, second line (engine numbers)
    juce::String testReadout1 (int band);                // the node read-out text, first line (shape, frequency, gain, Q)
    bool testCarved (int band);                           // the teal ring is drawn
    juce::Rectangle<float> testLockupArea();              // the SubverseLab mark (bottom right of the display), editor coordinates
    void setLockupLinkForTest (std::function<void()> fn); // a click on the mark calls fn instead of opening a browser (empty = the browser again)
    juce::Component& labFace();
    juce::Component& labScreen();                        // the display
    void forEachControl (const std::function<void (juce::Component&, PanelControl&)>& fn);

private:
    void timerCallback() override;
    void layoutGlobals();

    DyneqAudioProcessor& proc;
    panel::Ctl model;
    panel::Curves cv;
    panel::Spectrum spec;
    juce::LookAndFeel_V4 lnf;
    float inShown = -120.0f, outShown = -120.0f;
    int overlay = -1, tick = 0, hoverChipBand = -1;
    bool keyTrace = true;
    bool dynShown = false, advShown = false;
    std::unique_ptr<panelui::LabFace> lab;
    std::unique_ptr<panelui::SizeChip> sizeChip;
    std::unique_ptr<panelui::AssistOverlay> assist[3];
    std::unique_ptr<panelui::AdvancedPanel> adv;
    std::unique_ptr<panelui::Chooser> chooser;
    std::unique_ptr<panelui::HelpTip> helpTip;
    std::map<juce::Component*, std::pair<juce::String, HelpFn>> helps;
    juce::Component::SafePointer<juce::Component> helpOver;     // the control the pointer rests on (one with help), and for how many ticks
    int helpTicks = 0;
#if SVL_LICENSING
    juce::TextButton licenseButton { "LICENCE" };   // reopens the card when licensed: key status, deactivate this computer
    svl::LicenseOverlay licenseOverlay;   // tester build: the activation card covers the panel until a key is accepted (audio passes through untouched meanwhile)
#endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DyneqEditor)
};
