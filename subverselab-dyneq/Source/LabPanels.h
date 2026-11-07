#pragma once
// ADALET LAB, UX redesign of 11 Oct (docs/ui/UX_REDESIGN_PROQ_EQ8.md, owner: "bant secmek pratik degil ... EQ Eight'ten ilham al, pratik, kullanisli ve etkili olmali").
// The parts EQ Eight has and the old LAB lacked: a numbered band strip that is always visible, and ONE docked strip that edits the selected band.
// Nothing here floats over the display except the two popovers the spec allows (the dynamic settings over the frequency scale, the ASSIST overlays under their chips).
// Included by PluginEditor.cpp after NodeInfo (the strip's read-out uses it).
#include "PanelPanels.h"
#include <array>

namespace panelui
{
namespace lab
{
// the zones, editor coordinates (spec 3.2)
// 1200 x 540 (11 Oct night, owner: "herşey dijital ekranda olsun, boyu biraz kısaltabiliriz"): ONE screen 6..534 holds everything:
// the top row (name and model, ASSIST, meters, analyzer keys), the band chips, the display, the control row (band | ADVANCED + OUTPUT)
constexpr int kScreenY = 6, kScreenH = 528;
constexpr int kTopY = 12, kTopH = 36;                                      // the top row, inside the screen
constexpr int kChipX0 = 16, kChipStep = 185, kChipY = 54, kChipW = 179, kChipH = 36;
constexpr int kPlotX = 16, kPlotY = 96, kPlotW = 1168, kPlotH = 378;      // the display: y 96 .. 474
constexpr int kStripX = 16, kStripY = 480, kStripW = 970, kStripH = 48;   // the control row of the selected band
constexpr int kFootX = 1004, kFootY = kStripY, kFootW = 180, kFootH = kStripH;   // ADVANCED + OUTPUT, the right end of the control row
constexpr int kMeterX = 680, kMeterW = 200;                                // IN / OUT, in the top row
// the ASSIST chips (TARGET / COLLIDE / SELF) and their overlays, a gap after the name block (8 Nov: the LAB | ORBIT switch that sat there is gone)
constexpr int kAssistX0 = 140, kAssistStep = 138, kAssistW = 130;
constexpr int kAdvX = 16, kAdvY = kPlotY + kPlotH - 88, kAdvW = 1168, kAdvH = 84;   // ADVANCED opens upwards, over the bottom of the display (25 Oct: +28 for the BAKE line)
constexpr int kPopX = 400, kPopY = kPlotY + kPlotH - 44, kPopW = 470, kPopH = 40;   // the "..." popover, over the frequency scale

// drag laws of the strip's boxes (pixels upwards -> value); Shift = one fifth
inline double dragHz (double v, double px, bool fine)    { return v * std::pow (2.0, px / 96.0 * (fine ? 0.2 : 1.0)); }   // 1/96 octave per pixel
inline double dragDb (double v, double px, bool fine)    { return v + px * 0.05 * (fine ? 0.2 : 1.0); }                   // 0.1 dB per 2 pixels
inline double dragThr (double v, double px, bool fine)   { return v + px * 0.1 * (fine ? 0.2 : 1.0); }
inline double dragRatio (double v, double px, bool fine) { return v * std::pow (1.01, px * (fine ? 0.2 : 1.0)); }         // Q, attack, release: 1 % per pixel
}

/** A band shape as a glyph: the engine's own response (Engine::freeBandMagDb) on a small log axis, so the icon is the curve the band really makes. */
inline void shapeGlyph (juce::Graphics& g, juce::Rectangle<float> r, int type, juce::Colour col, float stroke = 1.6f)
{
    static constexpr int N = 40;
    static const auto table = []
    {
        std::array<std::array<float, N>, 6> t {};
        for (int ty = 0; ty < 6; ++ty)
        {
            const double fc = ty == dyneq::kLowCut ? 300.0 : ty == dyneq::kHighCut ? 3000.0 : ty == dyneq::kLowShelf ? 400.0 : ty == dyneq::kHighShelf ? 2500.0 : 1000.0;
            const double gain = ty == dyneq::kBell ? 12.0 : (ty == dyneq::kLowShelf || ty == dyneq::kHighShelf) ? 10.0 : 0.0;
            const double q = ty == dyneq::kNotch ? 2.0 : ty == dyneq::kBell ? 1.2 : 0.71;
            for (int i = 0; i < N; ++i)
            {
                const double f = 20.0 * std::pow (1000.0, (double) i / (double) (N - 1));
                t[(size_t) ty][(size_t) i] = (float) juce::jlimit (-1.0, 1.0, dyneq::Engine::freeBandMagDb (ty, fc, gain, q, 24, f, 48000.0) / 14.0);
            }
        }
        return t;
    }();
    type = juce::jlimit (0, 5, type);
    const bool boost = type == dyneq::kBell || type == dyneq::kLowShelf || type == dyneq::kHighShelf;
    const float base = r.getCentreY() + (boost ? 0.24f : -0.24f) * r.getHeight(), amp = r.getHeight() * 0.62f;
    juce::Path p;
    for (int i = 0; i < N; ++i)
    {
        const float x = r.getX() + r.getWidth() * (float) i / (float) (N - 1), y = base - table[(size_t) type][(size_t) i] * amp;
        if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
    }
    g.setColour (col); g.strokePath (p, juce::PathStrokeType (stroke, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

//==============================================================================
/** One chip of the band strip (EQ Eight's numbered buttons): number badge, ON dot, shape glyph, frequency and gain / slope. One click selects.
    The dot switches the band on and off without selecting it; the glyph selects and opens the shape list; an unused slot is "+ ADD". */
class BandChip : public juce::Component, public PanelControl, private PanelCounted
{
public:
    BandChip (DyneqEditor& e, int band) : ed (e), b (band) { setRepaintsOnMouseActivity (true); }
    static juce::Rectangle<float> iconBox (float h) { return { 58.0f, h * 0.5f - 11.0f, 32.0f, 22.0f }; }
    void paint (juce::Graphics& g) override
    {
        auto& c = ed.ctl(); const auto& cb = ed.curves().band[b];
        const bool sel = c.selectedBand == b, on = c.bandOn (b), empty = c.isEmptySlot (b), over = isMouseOver (true);
        const auto r = getLocalBounds().toFloat().reduced (3.0f);
        const juce::Rectangle<float> badge (9.0f, r.getCentreY() - 11.0f, 22.0f, 22.0f);
        if (empty && ! sel)   // an unused slot recedes into the glass: a small number and a quiet "+" (six loud "+ ADD" keys read as a prototype)
        {
            dig::body (g, r, false, over, false);
            silk (g, juce::String (b + 1), Theme::condEm (12.0f), badge.getCentreX(), badge.getCentreY() + 0.5f, 'm', 0.0f, dig::text().withAlpha (over ? 0.6f : 0.35f));
            silk (g, over ? "+ ADD BAND" : "+", Theme::condEm (over ? 11.0f : 15.0f), r.getCentreX() + 10.0f, r.getCentreY(), 'm', 1.4f, dig::text().withAlpha (over ? 0.8f : 0.3f));
            return;
        }
        dig::body (g, r, sel, over, false, true, 5.0f, false);   // a chip has two lines of text down to its foot: no gold bar under it (the gold badge says selected)
        const float a = on || sel ? 1.0f : 0.5f;
        if (sel) { g.setColour (dig::gold()); g.fillRoundedRectangle (badge, 4.0f); }
        else { g.setColour (dig::text().withAlpha (0.08f * a)); g.fillRoundedRectangle (badge, 4.0f); }
        silk (g, juce::String (b + 1), Theme::condEm (12.5f), badge.getCentreX(), badge.getCentreY() + 0.5f, 'm', 0.0f, sel ? rgb (20, 30, 20) : dig::text().withAlpha (a));
        if (empty) { silk (g, "+ ADD BAND", Theme::condEm (11.5f), r.getCentreX() + 12.0f, r.getCentreY(), 'm', 1.4f, dig::accent()); return; }
        const float cy = r.getCentreY();
        if (on) { g.setColour (dig::text()); g.fillEllipse (44.0f - 5.0f, cy - 5.0f, 10.0f, 10.0f); }
        else { g.setColour (dig::text().withAlpha (0.6f)); g.drawEllipse (44.0f - 4.5f, cy - 4.5f, 9.0f, 9.0f, 1.2f); }
        const int t = c.bandType (b);
        const auto ib = iconBox ((float) getHeight());
        shapeGlyph (g, ib, t, (sel ? dig::accent() : dig::text()).withAlpha (on ? 1.0f : 0.45f));
        if (on && c.on (panel::bandId (b, "dyn")) && panel::Ctl::typeHasDyn (t)) { g.setColour (dig::gold()); g.fillRoundedRectangle (ib.getRight() + 3.0f, cy - 8.0f, 3.0f, 16.0f, 1.0f); }   // a dynamic band
        const auto f = Theme::monoEm (13.0f);
        const juce::String l1 = panel::fmtHzShort (cb.freq) + (cb.freq < 1000.0f ? " Hz" : "");
        const juce::String l2 = ! on ? juce::String ("OFF") : panel::Ctl::isCut (t) ? juce::String (c.bandSlopeDb (b)) + " dB/oct"
                              : t == dyneq::kNotch ? "Q " + dyneq::ptext::fixed (c.raw (panel::bandId (b, "q")), 2) : panel::sgn (c.raw (panel::bandId (b, "gain"))) + " dB";
        silk (g, l1, f, r.getRight() - 9.0f, cy - 8.5f, 'r', 0.0f, dig::text().withAlpha (on ? 1.0f : 0.55f));
        silk (g, l2, f, r.getRight() - 9.0f, cy + 9.0f, 'r', 0.0f, dig::dim());
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        downZone = -1;
        if (e.mods.isPopupMenu())
        {
            ed.ctl().selectLab (b); ed.refreshFace();
            ed.openItems (ed.ctl().menuItems (b), e.getEventRelativeTo (&ed).getPosition());
            return;
        }
        downZone = e.position.x >= 34.0f && e.position.x < 54.0f ? 1 : iconBox ((float) getHeight()).expanded (3.0f, 12.0f).contains (e.position) ? 2 : 0;
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (downZone < 0 || e.mouseWasDraggedSinceMouseDown()) return;
        auto& c = ed.ctl();
        if (c.isEmptySlot (b)) { c.addSlot (b); ed.refreshFace(); return; }
        if (downZone == 1) { if (e.getNumberOfClicks() < 2) c.toggleBand (b); ed.refreshFace(); return; }   // the dot: on / off, the selection stays
        c.selectLab (b); ed.refreshFace();
        if (downZone == 2)   // the glyph: the shape list under the chip
            ed.openItems (c.shapeItems (b), ed.getLocalPoint (this, juce::Point<int> ((int) iconBox ((float) getHeight()).getX() + 70, getHeight() - 10)));
    }
    void mouseEnter (const juce::MouseEvent&) override { ed.setHoverChip (b); }
    void mouseExit (const juce::MouseEvent&) override { if (ed.hoverChip() == b) ed.setHoverChip (-1); }
    juce::StringArray boundParams() const override { return { panel::bandId (b, "on"), panel::bandId (b, "type"), panel::bandId (b, "gain") }; }
    juce::String uiAction() const override { return "BAND CHIP"; }
    int band() const { return b; }
    /** What the chip shows, as one number: the face repaints a chip only when it changes. */
    double signature() const
    {
        auto& c = ed.ctl();
        return (double) ed.curves().band[b].freq + 31.0 * c.raw (panel::bandId (b, "gain")) + 1.0e5 * c.bandType (b) + 1.0e6 * (c.bandOn (b) ? 1 : 0) + 3.0e6 * c.bandSlopeDb (b)
             + 7.0e3 * c.raw (panel::bandId (b, "q")) + 5.0e8 * (c.selectedBand == b ? 1 : 0) + 9.0e8 * (c.isEmptySlot (b) ? 1 : 0) + 1.1e9 * (c.on (panel::bandId (b, "dyn")) ? 1 : 0);
    }
    double lastSig = -1.0;
private:
    DyneqEditor& ed; int b; int downZone = -1;
};

//==============================================================================
/** A dropdown key of the control row (SHAPE, SLOPE): what is chosen now, an arrow; a click opens the list. SHAPE also shows the glyph. */
class DropKey : public PlateKeyButton
{
public:
    DropKey (int shapeGlyphType = -1) : PlateKeyButton ("", [] { return false; }, 8.4f, 0.8f, 2.0f), glyph (shapeGlyphType) { arrow = true; }
    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        const auto box = getLocalBounds().toFloat().reduced (2.0f);
        dig::key (g, box, getButtonText(), false, over, down, 8.4f, glyph >= 0 ? 12.0f : -4.0f, isEnabled());
        if (glyph >= 0) shapeGlyph (g, { box.getX() + 8.0f, box.getCentreY() - 9.0f, 26.0f, 18.0f }, glyph, dig::accent().withAlpha (isEnabled() ? 1.0f : 0.4f), 1.5f);
        const float x = box.getRight() - 11.0f, y = box.getCentreY() + 0.5f;
        juce::Path t; t.addTriangle (x - 4.0f, y - 2.0f, x + 4.0f, y - 2.0f, x, y + 3.0f);
        g.setColour (dig::text().withAlpha (isEnabled() ? 0.8f : 0.3f)); g.fillPath (t);
    }
    int glyph;
};

//==============================================================================
/** The control row of the selected band, inside the screen under the display (owner 11 Oct: "her sey ekrana gomulu", "slope ve benzerleri acilir olsun, sade").
    BAND N, ON, SHAPE (list), SLOPE (list, live on a cut only), FREQ, GAIN, Q | RANGE (the dynamic switch), THRESHOLD, "..." (attack, release, trigger), DELETE. */
class BandStrip : public juce::Component, private PanelCounted
{
public:
    explicit BandStrip (DyneqEditor& e) : ed (e)
    {
        auto& c = ed.ctl();
        onKey = std::make_unique<PlateKeyButton> ("ON", [this, &c] { return c.bandOn (band()); }, 8.4f, 1.2f, 2.0f);
        onKey->bound = [this] { return juce::StringArray { id ("on") }; };
        onKey->onClick = [this, &c] { c.toggleBand (band()); ed.refreshFace(); };
        addAndMakeVisible (*onKey);
        shapeKey = std::make_unique<DropKey> (0);
        shapeKey->action = "SHAPE LIST";
        shapeKey->bound = [this] { return juce::StringArray { id ("type"), id ("on"), id ("gain") }; };   // a shape pick switches the band on and may start a gain (+3 / -3 / +2 dB)
        shapeKey->onClick = [this, &c] { openList (c.shapeItems (band()), *shapeKey); };
        addAndMakeVisible (*shapeKey);
        slopeKey = std::make_unique<DropKey>();
        slopeKey->action = "SLOPE LIST";
        slopeKey->bound = [this] { return juce::StringArray { id ("slope"), id ("on") }; };
        slopeKey->onClick = [this, &c] { openList (c.slopeItems (band()), *slopeKey); };
        addAndMakeVisible (*slopeKey);
        delKey = std::make_unique<PlateKeyButton> ("DELETE", [] { return false; }, 8.0f, 1.0f, 2.0f);
        delKey->bound = [this] { juce::StringArray a; for (const char* n : panel::Ctl::kBandParams) a.add (id (n)); return a; };
        delKey->action = "DELETE BAND";
        delKey->onClick = [this, &c] { c.deleteBand (band()); ed.refreshFace(); };
        addAndMakeVisible (*delKey);

        auto box = [&] (const char* n, std::function<double (double, double, bool)> law)
        {
            auto w = std::make_unique<ValueWin> (c, panel::bandId (0, n), 14.0f, true);
            w->dragMap = std::move (law); addAndMakeVisible (*w); return w;
        };
        freq = box ("freq", lab::dragHz); gain = box ("gain", lab::dragDb); q = box ("q", lab::dragRatio);
        thr = box ("thr", lab::dragThr); range = box ("range", lab::dragDb);   // THRESHOLD before RANGE in the child order: a RANGE reset switches the dynamics off and with them THRESHOLD
        gain->active = [this, &c] { return panel::Ctl::typeHasGain (c.bandType (band())); };
        range->active = [this, &c] { return panel::Ctl::typeHasDyn (c.bandType (band())); };
        thr->active = [this, &c] { return panel::Ctl::typeHasDyn (c.bandType (band())) && c.on (id ("dyn")); };
        gain->textFn = [this, &c] { return panel::sgn (c.raw (id ("gain"))) + " dB"; };
        range->textFn = [this, &c] { const float v = c.raw (id ("range")); return panel::Ctl::rangeMeansDyn (v) ? dyneq::ptext::format (id ("range"), v) : juce::String ("OFF"); };
        range->onPlain = [this, &c] (float v) { c.setQuiet (id ("dyn"), panel::Ctl::rangeMeansDyn (v) ? 1.0f : 0.0f); };   // RANGE != 0 = dynamic on (Pro-Q's range ring)
        moreKey = std::make_unique<PlateKeyButton> ("...", [this] { return ed.dynOpen(); }, 9.0f, 1.0f, 2.0f);
        moreKey->action = "DYNAMIC SETTINGS";
        moreKey->onClick = [this] { ed.setDynOpen (! ed.dynOpen()); };
        addAndMakeVisible (*moreKey);
        // hover help (25 Oct, tester report T3: DELETE was hard to find, a selected band did not say whether it processes)
        ed.setHelp (*delKey, "DELETE", [this] { return "Removes band " + juce::String (band() + 1) + ": every setting goes back to its default and the band stops processing (its chip shows + ADD). "
                                                       "Cmd+Z brings it back. The Delete key does the same for the selected band; a right-click on its node has DELETE too."; });
        ed.setHelp (*onKey, "ON", [this, &c] { const int b = band(); return c.isEmptySlot (b) ? "Band " + juce::String (b + 1) + " is UNUSED: it does not touch the audio. ON starts it as a bell at 0 dB; "
                                                                                 "then set GAIN, or drag its node on the display."
                                                      : c.bandOn (b) ? "Band " + juce::String (b + 1) + " is ACTIVE: it processes the audio. Click to bypass it (its settings stay)."
                                                                     : "Band " + juce::String (b + 1) + " is BYPASSED: it keeps its settings but does not touch the audio. Click to switch it on."; });
        ed.setHelp (*moreKey, "DYNAMIC SETTINGS", [] { return juce::String ("ATTACK, RELEASE and TRIGGER (SELF = this track, KEY = the sidechain) of a dynamic band. RANGE other than 0 makes the band dynamic."); });
        setSize (lab::kStripW, lab::kStripH);
    }
    int band() const { return juce::jmax (0, ed.ctl().selectedBand); }
    juce::String id (const char* n) const { return panel::bandId (band(), n); }
    void openList (std::vector<panel::ChooserItem> items, juce::Component& key)
    {
        const int h = 8 + 25 * (int) items.size();   // the list opens above the key (the row sits at the bottom of the window)
        const auto top = ed.getLocalPoint (&key, juce::Point<int> (0, 0));
        ed.openItems (std::move (items), { top.x + 70, top.y - h - 18 });
    }

    void resized() override
    {
        onKey->setBounds (74, 6, 48, 36);
        shapeKey->setBounds (128, 6, 140, 36);
        slopeKey->setBounds (274, 6, 104, 36);
        freq->setBounds (390, 14, 100, 32); gain->setBounds (496, 14, 86, 32); q->setBounds (588, 14, 68, 32);
        range->setBounds (674, 14, 84, 32); thr->setBounds (764, 14, 88, 32); moreKey->setBounds (856, 10, 40, 36);
        delKey->setBounds (getWidth() - 66, 6, 66, 36);
    }
    void paint (juce::Graphics& g) override
    {
        auto& c = ed.ctl();
        if (c.selectedBand < 0)
        {
            silk (g, "SELECT A BAND OR DOUBLE-CLICK THE DISPLAY", Theme::condEm (13.0f), (float) getWidth() * 0.5f, (float) getHeight() * 0.5f, 'm', 2.4f, dig::dim());
            return;
        }
        const int b = band(); const bool on = c.bandOn (b), unused = c.isEmptySlot (b);
        silk (g, "BAND " + juce::String (b + 1), Theme::condEm (15.0f), 2.0f, 17.0f, 'l', 1.4f, dig::accent());
        // 25 Oct (tester T3): the state says plainly whether the band processes: ACTIVE (lit), BYPASSED (off, settings kept), UNUSED (nothing set)
        silk (g, on ? "ACTIVE" : (unused ? "UNUSED" : "BYPASSED"), Theme::condEm (10.0f), 2.0f, 35.0f, 'l', 1.2f, on ? dig::accent() : dig::dim());
        if (unused)   // no value boxes for a band that does nothing: one sentence instead of 0.0 dB / RANGE OFF read-outs
        {
            silk (g, "THIS BAND IS UNUSED: IT DOES NOT TOUCH THE AUDIO.", Theme::condEm (12.0f), 290.0f, 17.0f, 'l', 1.4f, dig::text());
            silk (g, "PRESS ON, PICK A SHAPE OR DOUBLE-CLICK THE DISPLAY TO START IT.", Theme::condEm (10.0f), 290.0f, 34.0f, 'l', 1.2f, dig::dim());
            return;
        }
        const auto cap = Theme::legendEm (6.6f);
        auto caption = [&] (const char* t, const ValueWin& w) { silk (g, t, cap, (float) w.getX() + 3.0f, 7.0f, 'l', 1.3f, dig::dim().withAlpha (w.isActive() ? 1.0f : 0.5f)); };
        caption ("FREQ", *freq); caption ("GAIN", *gain); caption ("Q", *q); caption ("RANGE", *range); caption ("THRESHOLD", *thr);
        g.setColour (dig::line()); g.drawVerticalLine (665, 8.0f, 44.0f);
        g.drawVerticalLine (delKey->getX() - 5, 8.0f, 44.0f);   // DELETE stands apart: it acts on the whole band
    }
    /** Retargets every control to the selected band; repaints only when something it shows changed. Called by the face every frame. */
    void refresh()
    {
        auto& c = ed.ctl();
        const int sel = c.selectedBand;
        const bool unused = sel >= 0 && c.isEmptySlot (band());
        for (auto* ch : getChildren()) ch->setVisible (sel >= 0 && (! unused || ch == onKey.get() || ch == shapeKey.get()));
        if (sel < 0) { if (lastSig != -1.0) { lastSig = -1.0; repaint(); } return; }
        const int b = band(), t = c.bandType (b);
        freq->setId (id ("freq")); gain->setId (id ("gain")); q->setId (id ("q")); range->setId (id ("range")); thr->setId (id ("thr"));
        range->linked = juce::StringArray { id ("dyn") };
        shapeKey->glyph = t; shapeKey->setButtonText (panel::Ctl::shapeName (t));
        const bool cut = panel::Ctl::isCut (t);
        slopeKey->setEnabled (cut); slopeKey->setButtonText (cut ? juce::String (c.bandSlopeDb (b)) + " dB/oct" : juce::String ("SLOPE"));
        double sig = 1.0 + b * 1.0e9 + t * 1.0e8 + (c.bandOn (b) ? 1.0e7 : 0.0) + (unused ? 2.0e7 : 0.0) + c.bandSlopeDb (b) * 1.0e5 + (c.on (id ("dyn")) ? 5.0e4 : 0.0) + (ed.dynOpen() ? 2.5e4 : 0.0);
        for (const char* n : { "freq", "gain", "q", "range", "thr" }) sig += c.raw (id (n)) * (n[0] == 'f' ? 1.0 : 97.0);
        if (sig != lastSig) { lastSig = sig; repaint(); }
    }
    /** Typing in a box: the FREQ box (a double-click on a node), or the next / previous active box (Tab). */
    void beginEditFreq() { if (ed.ctl().selectedBand >= 0) freq->beginEdit(); }
    void editNext (bool backwards)
    {
        if (ed.ctl().selectedBand < 0) return;
        ValueWin* order[5] { freq.get(), gain.get(), q.get(), range.get(), thr.get() };
        int cur = -1; for (int i = 0; i < 5; ++i) if (order[i]->isEditing()) cur = i;
        if (cur < 0) cur = backwards ? 0 : 4;
        for (int k = 1; k <= 5; ++k)
        {
            auto* w = order[(cur + (backwards ? -k : k) + 10) % 5];
            if (w->isActive()) { for (auto* o : order) o->cancelEdit(); w->beginEdit(); return; }
        }
    }
    ValueWin& freqBox() { return *freq; }
    double lastSig = -2.0;
private:
    DyneqEditor& ed;
    std::unique_ptr<PlateKeyButton> onKey, delKey, moreKey;
    std::unique_ptr<DropKey> shapeKey, slopeKey;
    std::unique_ptr<ValueWin> freq, gain, q, range, thr;
};

//==============================================================================
/** The "..." popover of the strip: ATTACK, RELEASE, TRIGGER SELF / KEY of the selected band. One row over the frequency scale, never over the curve's body. */
class DynPop : public juce::Component, private PanelCounted
{
public:
    explicit DynPop (DyneqEditor& e) : ed (e)
    {
        auto& c = ed.ctl();
        att = std::make_unique<ValueWin> (c, "b1_att", 13.0f, true); att->dragMap = lab::dragRatio; addAndMakeVisible (*att);
        rel = std::make_unique<ValueWin> (c, "b1_rel", 13.0f, true); rel->dragMap = lab::dragRatio; addAndMakeVisible (*rel);
        for (int v = 0; v < 2; ++v)
        {
            trig[v] = std::make_unique<PlateKeyButton> (v == 0 ? "SELF" : "KEY", [this, &c, v] { return juce::roundToInt (c.raw (id ("trig"))) == v; }, 8.0f, 1.0f, 3.0f);
            trig[v]->bound = [this] { return juce::StringArray { id ("trig") }; };
            trig[v]->onClick = [this, &c, v] { c.setGesture (id ("trig"), (float) v); ed.refreshFace(); };
            addAndMakeVisible (*trig[v]);
        }
        setName ("POPOVER");
        setSize (lab::kPopW, lab::kPopH);
    }
    juce::String id (const char* n) const { return panel::bandId (juce::jmax (0, ed.ctl().selectedBand), n); }
    void refresh() { att->setId (id ("att")); rel->setId (id ("rel")); repaint(); }
    void resized() override
    {
        att->setBounds (66, 6, 88, 28); rel->setBounds (226, 6, 88, 28);
        trig[0]->setBounds (374, 3, 48, 34); trig[1]->setBounds (420, 3, 46, 34);
    }
    void paint (juce::Graphics& g) override
    {
        dig::panel (g, getLocalBounds().toFloat().reduced (1.0f), 0.98f);
        const auto cap = Theme::legendEm (7.0f);
        silk (g, "ATTACK", cap, 12.0f, 19.5f, 'l', 1.4f, dig::dim());
        silk (g, "RELEASE", cap, 166.0f, 19.5f, 'l', 1.4f, dig::dim());
        silk (g, "TRIGGER", cap, 326.0f, 19.5f, 'l', 1.4f, dig::dim());
    }
private:
    DyneqEditor& ed;
    std::unique_ptr<ValueWin> att, rel;
    std::unique_ptr<PlateKeyButton> trig[2];
};

//==============================================================================
/** An ASSIST chip of the top bar (TARGET / COLLIDE / SELF): the lamp switches the layer, the name opens its overlay. Neither ever changes the band selection.
    The lamp is the legend (gold / teal / cream) and glows with how much the layer is cutting now. */
class AssistChip : public juce::Component, public PanelControl, private PanelCounted
{
public:
    AssistChip (DyneqEditor& e, int layerMode) : ed (e), m (layerMode) { setRepaintsOnMouseActivity (true); }
    juce::Colour colour() const { return m == panel::kTarget ? dig::gold() : m == panel::kCollide ? Theme::teal.brighter (0.25f) : dig::text(); }
    juce::String name() const { return m == panel::kTarget ? "TARGET" : m == panel::kCollide ? "COLLIDE" : "SELF"; }
    juce::String state() const
    {
        auto& c = ed.ctl();
        if (m == panel::kTarget) return c.layerOn (m) ? c.targetName() : juce::String ("OFF");
        if (m == panel::kCollide) return c.layerOn (m) ? (c.proc.isSidechainConnected() ? "KEY" : "NO KEY") : juce::String ("OFF");
        return c.layerOn (m) ? "RESONANCES" : "OFF";
    }
    float heat() const   // 0..1: the deepest cut of the layer now, against 6 dB
    {
        const auto& cv = ed.curves(); const float* d = m == panel::kTarget ? cv.target : m == panel::kCollide ? cv.collide : cv.self;
        float lo = 0.0f; for (int i = 0; i < panel::kCurveN; ++i) lo = juce::jmin (lo, d[i]);
        return juce::jlimit (0.0f, 1.0f, -lo / 6.0f);
    }
    void paint (juce::Graphics& g) override
    {
        auto& c = ed.ctl();
        const bool open = ed.overlayId() == m, on = c.layerOn (m);
        const auto r = getLocalBounds().toFloat().reduced (1.0f);
        dig::body (g, r, open, isMouseOver (true), false);
        const float lx = 15.0f, ly = r.getCentreY();
        // the layer's LED; lit, it glows with how much the layer cuts now
        if (on)
        {
            const float a = 0.45f + 0.55f * heat();
            g.setColour (colour().withAlpha (0.18f * a)); g.fillEllipse (lx - 10.0f, ly - 10.0f, 20.0f, 20.0f);
            g.setColour (colour().withAlpha (0.35f * a)); g.fillEllipse (lx - 7.0f, ly - 7.0f, 14.0f, 14.0f);
            g.setColour (colour().withAlpha (0.55f + 0.45f * a)); g.fillEllipse (lx - 4.5f, ly - 4.5f, 9.0f, 9.0f);
            g.setColour (juce::Colours::white.withAlpha (0.5f)); g.fillEllipse (lx - 2.0f, ly - 3.0f, 2.6f, 1.8f);
        }
        else { g.setColour (colour().withAlpha (0.18f)); g.fillEllipse (lx - 4.5f, ly - 4.5f, 9.0f, 9.0f); g.setColour (colour().withAlpha (0.45f)); g.drawEllipse (lx - 4.5f, ly - 4.5f, 9.0f, 9.0f, 1.0f); }
        silk (g, name(), Theme::condEm (12.0f), 30.0f, ly - 7.0f, 'l', 1.2f, open ? dig::accent() : dig::text());
        auto st = state(); const auto f = Theme::condEm (10.0f);
        while (st.length() > 3 && fam::textWidth (st, f, 0.6f) > r.getWidth() - 36.0f) st = st.dropLastCharacters (1);
        silk (g, st, f, 30.0f, ly + 8.0f, 'l', 0.6f, on ? colour() : dig::dim());
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mouseWasDraggedSinceMouseDown() || e.mods.isPopupMenu()) return;
        if (e.position.x < 28.0f) ed.ctl().toggleLayer (m);   // the lamp
        else ed.toggleOverlay (m);                             // the name: the overlay (the selection stays where it is)
        ed.refreshFace();
    }
    juce::StringArray boundParams() const override { return m == panel::kTarget ? juce::StringArray { "targetInst" } : juce::StringArray { "mode", "assistOff" }; }
    juce::String uiAction() const override { return "ASSIST OVERLAY"; }
    int layerMode() const { return m; }
private:
    DyneqEditor& ed; int m;
};

//==============================================================================
/** The right end of the control row: ADVANCED (collapsed by default; a gold dot while BAKE is armed) and OUTPUT. Global, so it stays when no band is selected. */
class LabFooter : public juce::Component, private PanelCounted
{
public:
    explicit LabFooter (DyneqEditor& e) : ed (e)
    {
        advKey = std::make_unique<PlateKeyButton> ("ADVANCED", [this] { return ed.advancedOpen(); }, 8.2f, 1.6f, 2.0f);
        advKey->arrow = true; advKey->action = "ADVANCED";
        advKey->onClick = [this] { ed.setAdvancedOpen (! ed.advancedOpen()); };
        addAndMakeVisible (*advKey);
        ed.setHelp (*advKey, "ADVANCED", [this] { return "FOCUS LOW / HIGH, MIX, SWEET, TILING, LEVEL MATCH and BAKE (freeze what COLLIDE cuts into static bands: BAKE ARM, play, APPLY). "
                                                          "A gold dot on this key = BAKE is armed.\n\nBAKE now: " + ed.ctl().proc.bakeStatus().line1 + "."; });
        out = std::make_unique<ValueWin> (ed.ctl(), "output", 13.0f, true); out->dragMap = lab::dragDb; addAndMakeVisible (*out);
        setSize (lab::kFootW, lab::kFootH);
    }
    void resized() override { advKey->setBounds (0, 6, 96, 36); out->setBounds (getWidth() - 76, 14, 76, 32); }
    void paint (juce::Graphics& g) override
    {
        if (ed.ctl().on ("bakeArm")) { g.setColour (dig::gold()); g.fillEllipse (88.0f, 9.0f, 6.0f, 6.0f); }
        silk (g, "OUTPUT", Theme::legendEm (6.6f), (float) getWidth() - 73.0f, 7.0f, 'l', 1.3f, dig::dim());
    }
private:
    DyneqEditor& ed;
    std::unique_ptr<PlateKeyButton> advKey;
    std::unique_ptr<ValueWin> out;
};

//==============================================================================
/** IN / OUT, two thin bars in the top row of the screen. Takes no mouse. */
class LabMeters : public juce::Component, private PanelCounted
{
public:
    explicit LabMeters (DyneqEditor& e) : ed (e) { setInterceptsMouseClicks (false, false); }
    void paint (juce::Graphics& g) override
    {
        meter (g, 9.5f, "IN", ed.inDbShown()); meter (g, 26.5f, "OUT", ed.outDbShown());
    }
private:
    void meter (juce::Graphics& g, float cy, const char* nm, float db) const
    {
        silk (g, nm, Theme::legendEm (6.6f), 0.0f, cy, 'l', 1.3f, dig::dim());
        const float v = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f), x0 = 32.0f, w = (float) getWidth() - 86.0f;
        // a segmented LED bar: teal, gold over the top 12 dB
        constexpr int kSeg = 40; const float sw = w / (float) kSeg;
        for (int s = 0; s < kSeg; ++s)
        {
            const float sx = x0 + (float) s * sw, frac = ((float) s + 0.5f) / (float) kSeg;
            const auto c = frac > 0.8f ? dig::gold() : Theme::teal.brighter (0.15f);
            g.setColour (frac <= v ? c : c.withAlpha (0.10f)); g.fillRect (sx + 0.5f, cy - 2.5f, sw - 1.2f, 5.0f);
        }
        silk (g, dyneq::ptext::fixed (db, 1), Theme::monoEm (11.5f), (float) getWidth(), cy, 'r', 0.0f, dig::text());
    }
    DyneqEditor& ed;
};
} // namespace panelui
