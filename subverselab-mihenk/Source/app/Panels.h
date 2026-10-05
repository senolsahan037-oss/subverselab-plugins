#pragma once

#include <JuceHeader.h>
#include "Widgets.h"

/*  v2 usability parts of the front panel: the AUTO lamp on a solved knob, the loudness VFD of the TARGET block,
    the reference module (one difference graph and a measured summary), the SIMPLE face's chain readout, and the
    panel preferences (SIMPLE / ADVANCED face) kept on this computer. */
namespace mihenk::ui
{
/** Panel preferences shared by the app and the plug-in: ~/Library/Application Support/SubverseLab/MIHENK/ui.json. */
struct UiPrefs
{
    static juce::File file();
    /** Tests / snapshot tool: use another file (empty = the default). */
    static void setFileForTesting (const juce::File& f);
    /** "simple" (the default for a new user) or "advanced". */
    static bool advancedFace();
    static void setAdvancedFace (bool advanced);
};

/** The AUTO lamp on the shoulder of a knob whose value MIHENK solves (threshold, release, trim, drive).
    Lit = AUTO: the knob shows the solved value; click to switch AUTO on or off. */
class AutoLamp : public juce::Component, public juce::SettableTooltipClient
{
public:
    AutoLamp() { setMouseCursor (juce::MouseCursor::PointingHandCursor); setRepaintsOnMouseActivity (true); }
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override { if (onClick) onClick(); }
    void setOn (bool on) { if (on != lit) { lit = on; repaint(); } }
    bool isOn() const noexcept { return lit; }
    std::function<void()> onClick;

private:
    bool lit = false;
};

/** The TARGET block's VFD: the mix's loudness, the master's, and what that means ("TURNED DOWN 4.9 DB"). */
class LoudnessVfd : public juce::Component, public juce::TooltipClient
{
public:
    explicit LoudnessVfd (Context& c) : ctx (c) {}
    void paint (juce::Graphics&) override;
    juce::String getTooltip() override;

private:
    Context& ctx;
};

/** REFERENCE module: nothing but a hint until a reference is loaded; then one difference graph (reference minus the
    mix before, and minus the master now) and a measured one-line summary. */
class ReferencePanel : public juce::Component, public juce::TooltipClient
{
public:
    explicit ReferencePanel (Context& c) : ctx (c) {}
    void paint (juce::Graphics&) override;
    juce::String getTooltip() override;
    /** The one-line summary: "TONE 3.0 > 0.4 DB RMS, LOUD -9.3 / -9.3 LUFS". */
    static juce::String summary (const Session& s);

private:
    Context& ctx;
};

/** SIMPLE face: what the chain is doing now, line by line, with the values MIHENK solved marked AUTO. */
class ChainReadout : public juce::Component
{
public:
    explicit ChainReadout (Context& c) : ctx (c) {}
    void paint (juce::Graphics&) override;
    /** The lines (also used by the tests). */
    static juce::StringArray lines (const Session& s);

private:
    Context& ctx;
};
} // namespace mihenk::ui
