#pragma once
// ADALET P4c: the popovers over the screen: the three ASSIST overlays and ADVANCED.
// The band controls are the docked strip of LabPanels.h since the UX redesign of 11 Oct (the floating band panel is gone).
// Every control of a panel is a PanelControl with its parameter ids, so the editor tests can walk them.
#include "PanelWidgets.h"

namespace panelui
{
/** A popover's glass: the darker smoked layer of the screen (dig::panel). */
inline void paintGlass (juce::Graphics& g, juce::Rectangle<float> b) { dig::panel (g, b, 0.94f); }

//==============================================================================
/** An ASSIST layer's small glass overlay with all its parameters. */
class AssistOverlay : public juce::Component, public PanelControl, private PanelCounted
{
public:
    AssistOverlay (DyneqEditor& e, int layerMode) : ed (e), m (layerMode)
    {
        setName ("POPOVER");
        auto& c = ed.ctl();
        const char* nm = m == panel::kTarget ? "TARGET" : m == panel::kCollide ? "COLLIDE" : "SELF";
        onKey = std::make_unique<PlateKeyButton> (juce::String (nm) + " ON", [this, &c] { return c.layerOn (m); }, 8.0f, 1.2f, 5.0f);
        onKey->lamp = [this, &c] { return c.layerOn (m); };
        onKey->bound = [this] { return m == panel::kTarget ? juce::StringArray { "targetInst" } : juce::StringArray { "mode", "assistOff" }; };
        onKey->onClick = [this, &c] { c.toggleLayer (m); ed.refreshFace(); };
        addAndMakeVisible (*onKey);
        closeKey = std::make_unique<PlateKeyButton> ("X", [] { return false; }, 8.4f, 0.0f, 5.0f); closeKey->action = "CLOSE";
        closeKey->onClick = [this] { ed.closeOverlay(); };
        addAndMakeVisible (*closeKey);
        if (m == panel::kTarget)
        {
            chooserKey = std::make_unique<PlateKeyButton> ("OFF", [] { return false; }, 9.0f, 1.2f, 5.0f);
            chooserKey->arrow = true; chooserKey->action = "TARGET INSTRUMENT AND PART";
            chooserKey->bound = [] { return juce::StringArray { "targetInst", "targetPart", "reverbReady" }; };
            chooserKey->onClick = [this] { ed.openChooser (ed.getLocalPoint (this, chooserKey->getBounds().getCentre())); };
            addAndMakeVisible (*chooserKey);
            chooserLegend = std::make_unique<Legend> ("INSTRUMENT - PART", 7.0f, 1.6f, Theme::label, 'l'); addAndMakeVisible (*chooserLegend);
            k[0] = std::make_unique<ParamKnob> (c, "targetAmount", "AMOUNT", 20.0f, 92, 15.0f);
            k[1] = std::make_unique<ParamKnob> (c, "targetWidth", "WIDTH", 20.0f, 92, 15.0f);
            k[2] = std::make_unique<ParamKnob> (c, "targetSlope", "SLOPE", 20.0f, 92, 14.0f);
            setSize (404, 292);   // +30: the LISTENING / LEARNED line
        }
        else if (m == panel::kCollide)
        {
            keyKey = makeToggle (c, ed, "keyListen", "KEY LISTEN", 8.4f, 1.2f, 5.0f);
            keyStatus = std::make_unique<Legend> ("", 7.2f, 1.4f, Theme::amber, 'l'); keyStatus->textFn = [&c] { return c.keyText(); };
            addAndMakeVisible (*keyKey); addAndMakeVisible (*keyStatus);
            k[0] = std::make_unique<ParamKnob> (c, "depth", "DEPTH", 20.0f, 92, 15.0f);
            k[1] = std::make_unique<ParamKnob> (c, "margin", "MARGIN", 20.0f, 92, 15.0f);
            for (int v = 0; v < 3; ++v) { speed[v] = makeChoiceKey (c, ed, "speed", v, v == 0 ? "FAST" : v == 1 ? "MEDIUM" : "SLOW", 8.0f, 1.0f, 5.0f); addAndMakeVisible (*speed[v]); }
            speedLegend = std::make_unique<Legend> ("SPEED", 7.0f, 1.6f, Theme::label, 'l'); addAndMakeVisible (*speedLegend);
            setSize (404, 302);   // +20 (25 Oct): the note has a third line, the path to BAKE
        }
        else
        {
            k[0] = std::make_unique<ParamKnob> (c, "depth", "DEPTH", 20.0f, 92, 15.0f);
            k[1] = std::make_unique<ParamKnob> (c, "margin", "SENS", 20.0f, 92, 15.0f);
            setSize (320, 230);
        }
        for (auto& x : k) if (x) addAndMakeVisible (*x);
        note = m == panel::kTarget ? "Cuts a part of the chosen instrument, always. No sidechain needed."
             : m == panel::kCollide ? "Cuts this track where it meets the KEY track, only while the key plays. COLLIDE and SELF share one engine: lighting one darkens the other. To freeze COLLIDE's cuts into static bands: ADVANCED > BAKE ARM, play, APPLY."
             : "Hunts resonances in this track and cuts them. COLLIDE and SELF share one engine: lighting one darkens the other.";
        setVisible (false); setBufferedToImage (true);
    }
    void refresh()
    {
        auto& c = ed.ctl();
        if (chooserKey != nullptr) chooserKey->setButtonText (c.targetName());
        const auto st = m == panel::kTarget ? c.targetStatus() : juce::String();
        if (st != lastStatus || ! statusOnly) { lastStatus = st; repaint(); }
        statusOnly = true;
    }
    void resized() override
    {
        const int w = getWidth();
        onKey->setBounds (10, 8, 150, 38); closeKey->setBounds (w - 52, 8, 44, 38);
        int y = 56;
        if (m == panel::kTarget)
        {
            chooserLegend->setBounds (16, y, 200, 12); chooserKey->setBounds (10, y + 8, w - 20, 40); y += 60;
            for (int i = 0; i < 3; ++i) k[i]->setBounds (14 + i * 128, y, k[i]->getWidth(), k[i]->getHeight());
        }
        else if (m == panel::kCollide)
        {
            keyKey->setBounds (10, y - 2, 120, 40); keyStatus->setBounds (140, y + 4, w - 150, 24); y += 44;
            for (int i = 0; i < 2; ++i) k[i]->setBounds (14 + i * 128, y, k[i]->getWidth(), k[i]->getHeight());
            speedLegend->setBounds (276, y + 20, 100, 12);
            for (int v = 0; v < 3; ++v) speed[v]->setBounds (270 + 0, y + 34 + v * 38, 120, 38);
        }
        else
            for (int i = 0; i < 2; ++i) k[i]->setBounds (14 + i * 128, y, k[i]->getWidth(), k[i]->getHeight());
    }
    void paint (juce::Graphics& g) override
    {
        paintGlass (g, getLocalBounds().toFloat().reduced (2.0f));
        const auto col = m == panel::kTarget ? Theme::gold : m == panel::kCollide ? Theme::teal : Theme::labelDeep;
        g.setColour (col); g.fillRoundedRectangle (4.0f, 6.0f, 4.0f, 42.0f, 2.0f);
        // the note, wrapped in two lines at the bottom
        juce::AttributedString as; as.setJustification (juce::Justification::centredLeft);
        as.append (note, Theme::fineEm (7.6f), Theme::muted);
        const float nh = m == panel::kCollide ? 46.0f : 32.0f;
        as.draw (g, juce::Rectangle<float> (14.0f, (float) getHeight() - nh - 8.0f, (float) getWidth() - 28.0f, nh));
        if (m == panel::kTarget)   // what TARGET has heard so far: LISTENING (nothing cut yet) or LEARNED (cutting)
        {
            bool ready = false; const auto st = ed.ctl().targetStatus (&ready);
            if (st.isNotEmpty())
            {
                juce::AttributedString a2; a2.setJustification (juce::Justification::centredLeft);
                a2.append (st, Theme::condEm (10.5f), ready ? Theme::gold : Theme::ink);
                a2.draw (g, juce::Rectangle<float> (14.0f, (float) getHeight() - 70.0f, (float) getWidth() - 28.0f, 30.0f));
            }
        }
    }
    juce::StringArray boundParams() const override { return {}; }
    juce::String uiAction() const override { return "ASSIST OVERLAY"; }
    int layerMode() const { return m; }
private:
    DyneqEditor& ed; int m; juce::String note, lastStatus; bool statusOnly = false;
    std::unique_ptr<PlateKeyButton> onKey, closeKey, chooserKey, keyKey, speed[3];
    std::unique_ptr<Legend> chooserLegend, keyStatus, speedLegend;
    std::unique_ptr<ParamKnob> k[3];
};

//==============================================================================
/** A knob on the left with its value window on the right and the caption above (the compact unit of the ADVANCED strip). */
class MicroKnob : public juce::Component, private PanelCounted
{
public:
    static constexpr int kW = 124, kH = 52;
    MicroKnob (panel::Ctl& c, const juce::String& paramId, const juce::String& caption) : label (caption)
    {
        knob = std::make_unique<KnobComp> (c, paramId, 12.0f); win = std::make_unique<ValueWin> (c, paramId, 12.0f, true);
        knob->win = win.get(); addAndMakeVisible (*knob); addAndMakeVisible (*win);
        setInterceptsMouseClicks (false, true);
        setSize (kW, kH);
    }
    void resized() override { knob->setBounds (0, 14, 40, 38); win->setBounds (40, 20, 84, 28); }
    void paint (juce::Graphics& g) override { silk (g, label, Theme::legendEm (7.2f), 6.0f, 7.0f, 'l', 1.6f, dig::dim()); }
    std::unique_ptr<KnobComp> knob; std::unique_ptr<ValueWin> win;   // they take the mouse (setInterceptsMouseClicks (false, true); see ParamKnob)
private:
    juce::String label;
};

/** The BAKE line (25 Oct, tester report T5): what BAKE is doing now and the next step, in one line across the ADVANCED popover.
    The text is the processor's bakeStatus(); the colour says the state (gold = baked, amber = refused / not listening, teal = listening). */
class BakeLine : public juce::Component, private PanelCounted
{
public:
    explicit BakeLine (DyneqEditor& e) : ed (e) { setInterceptsMouseClicks (false, false); }
    void paint (juce::Graphics& g) override
    {
        const auto st = ed.ctl().proc.bakeStatus(); shown = st.line1 + " | " + st.line2;
        const auto col = st.state == DyneqAudioProcessor::kBakeBaked ? dig::gold() : st.state == DyneqAudioProcessor::kBakeListening ? Theme::teal.brighter (0.45f)
                       : st.state == DyneqAudioProcessor::kBakeIdle ? dig::dim() : Theme::amber;
        const auto f1 = Theme::condEm (11.0f), f2 = Theme::fineEm (8.6f);   // one line: line1, then line2
        const float cy = (float) getHeight() * 0.5f, x1 = 44.0f;
        silk (g, "BAKE", Theme::legendEm (7.2f), 0.0f, cy, 'l', 1.6f, dig::dim());
        silk (g, st.line1, f1, x1, cy, 'l', 0.4f, col);
        const float w1 = fam::textWidth (st.line1, f1, 0.4f);
        silk (g, st.line2, f2, x1 + w1 + 18.0f, cy, 'l', 0.0f, dig::text().withAlpha (0.85f));
    }
    juce::String shown;   // the last text drawn (tests read it)
private:
    DyneqEditor& ed;
};

/** ADVANCED: FOCUS LOW / HIGH, MIX, SWEET, TILING, BYPASS LEVEL MATCH and BAKE (ARM, RESET, APPLY) with the BAKE line. Two rows in the popover
    (the controls, then the BAKE line across the whole width, 25 Oct). */
class AdvancedPanel : public juce::Component, public PanelControl, private PanelCounted
{
public:
    explicit AdvancedPanel (DyneqEditor& e) : ed (e)
    {
        auto& c = ed.ctl();
        struct K { const char* id; const char* cap; } ks[] = { { "focusLow", "FOCUS LOW" }, { "focusHigh", "FOCUS HIGH" }, { "mix", "MIX" }, { "targetSweet", "SWEET" } };
        for (auto& x : ks) { micro.push_back (std::make_unique<MicroKnob> (c, x.id, x.cap)); addAndMakeVisible (*micro.back()); }
        tiling = makeToggle (c, ed, "tiling", "TILING", 8.0f, 1.2f, 5.0f); match = makeToggle (c, ed, "levelMatch", "LEVEL MATCH", 7.4f, 0.9f, 5.0f);
        arm = makeToggle (c, ed, "bakeArm", "BAKE ARM", 7.6f, 1.0f, 5.0f);
        reset = std::make_unique<PlateKeyButton> ("RESET", [] { return false; }, 7.6f, 1.0f, 5.0f); reset->action = "BAKE RESET";
        reset->onClick = [this] { ed.ctl().proc.bakeResetStats(); ed.refreshFace(); };
        apply = std::make_unique<PlateKeyButton> ("APPLY", [] { return false; }, 7.6f, 1.0f, 5.0f); apply->action = "BAKE APPLY";
        apply->onClick = [this] { ed.ctl().proc.bakeApply(); ed.refreshFace(); bake->repaint(); };
        for (auto* b : { tiling.get(), match.get(), arm.get(), reset.get(), apply.get() }) addAndMakeVisible (*b);
        bake = std::make_unique<BakeLine> (ed); addAndMakeVisible (*bake);
        auto& p = c.proc;
        ed.setHelp (*arm, "BAKE ARM - step 1 of 3", [&p] { return "Starts listening. While COLLIDE cuts (its lamp on, a key routed) and the track plays, BAKE adds up where and how deep it cut. "
                                                                 "Then press APPLY. Arming again starts from zero.\n\nNow: " + p.bakeStatus().line1 + ". " + p.bakeStatus().line2 + "."; });
        ed.setHelp (*apply, "APPLY - step 3 of 3", [&p] { const auto st = p.bakeStatus(); return st.help + "\n\nNow: " + st.line1 + ". " + st.line2 + "."; });
        ed.setHelp (*reset, "BAKE RESET", [] { return juce::String ("Forgets what BAKE has heard so far and starts counting again. The bands are not touched."); });
        ed.setHelp (*tiling, "TILING", [] { return juce::String ("COLLIDE's analysis bands meet at -3 dB instead of -12 dB: defined carve edges (off = soft, the default)."); });
        ed.setHelp (*match, "LEVEL MATCH", [] { return juce::String ("BYPASS plays the unprocessed track at the loudness of the processed one, so the comparison is fair."); });
        setSize (960, 70); setBufferedToImage (true);
    }
    /** A popover over the bottom of the display, two rows: the controls, then the BAKE line across the whole width. */
    void layoutRow()
    {
        setName ("POPOVER");
        int x = 10;
        for (auto& m : micro) { m->setBounds (x, 2, MicroKnob::kW, MicroKnob::kH); x += MicroKnob::kW + 6; }
        const std::pair<juce::Component*, int> keys[] { { tiling.get(), 66 }, { match.get(), 96 }, { arm.get(), 80 }, { reset.get(), 66 }, { apply.get(), 66 } };
        for (auto& k : keys) { if (k.first == arm.get()) { x += 10; bakeX = x - 8; } k.first->setBounds (x, 8, k.second, 40); x += k.second + 6; }
        bake->setBounds (14, 56, getWidth() - 28, 24);
    }
    void paint (juce::Graphics& g) override
    {
        paintGlass (g, getLocalBounds().toFloat().reduced (2.0f));
        if (bakeX > 0) { g.setColour (dig::line()); g.drawVerticalLine (bakeX, 10.0f, 46.0f); }   // BAKE ARM / RESET / APPLY are one group
    }
    /** The BAKE line repaints only when its text changes (the panel is buffered). */
    void refreshBake() { const auto st = ed.ctl().proc.bakeStatus(); const auto t = st.line1 + " | " + st.line2; if (t != lastBake) { lastBake = t; bake->repaint(); } }
    juce::String bakeText() const { const auto st = ed.ctl().proc.bakeStatus(); return st.line1 + " | " + st.line2; }
    juce::StringArray boundParams() const override { return {}; }
    juce::String uiAction() const override { return "ADVANCED"; }
private:
    DyneqEditor& ed; int bakeX = -1; juce::String lastBake;
    std::vector<std::unique_ptr<MicroKnob>> micro;
    std::unique_ptr<PlateKeyButton> tiling, match, arm, reset, apply;
    std::unique_ptr<BakeLine> bake;
};
} // namespace panelui
