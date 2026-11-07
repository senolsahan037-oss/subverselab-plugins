#pragma once
// ADALET P4c: the panel's generic controls (keys, knobs, value boxes, the chooser). Included by PluginEditor.cpp only; DyneqEditor must be complete.
// Rules every control here follows (TASK_P4c section 4):
//   * double-click on a knob, ring, value box or slider returns its parameter to the default; Alt/Option-click does too;
//   * typed entry is one single click into the value box (the box keeps the keyboard until Return / Escape / focus loss);
//   * a reset is ONE undo step (a complete gesture), and a drag is one step however many values it passes through.
#include "PluginEditor.h"
#include "ParamText.h"

namespace panelui
{
using fam::rgb;
using fam::silk;

constexpr float kPx = 0.80f;                       // em per prototype pixel (the prototype sizes are CSS px; the family's em are point heights)

/** A cached image of something static, redrawn only when the physical pixel scale changes. */
struct CachedLayer
{
    juce::Image img; float scale = 0.0f;
    template <class F> void draw (juce::Graphics& g, int w, int h, F&& paintFn)
    {
        const float s = g.getInternalContext().getPhysicalPixelScaleFactor();
        if (img.isNull() || std::abs (s - scale) > 1.0e-3f)
        {
            scale = s;
            img = juce::Image (juce::Image::ARGB, juce::roundToInt ((float) w * s), juce::roundToInt ((float) h * s), true);
            juce::Graphics ig (img);
            ig.addTransform (juce::AffineTransform::scale (s));
            paintFn (ig);
        }
        g.drawImageTransformed (img, juce::AffineTransform::scale (1.0f / scale));
    }
};


//==============================================================================
// The panel is a fully digital screen (owner, 11 Oct: "Pro-Q tipi tam dijital olmali, her sey ekrana gomulmeli ve anlasilir olmali"):
// every control draws itself flat and screen-like.
namespace dig
{
    // UX redesign 11 Oct (docs/ui/UX_REDESIGN_PROQ_EQ8.md 3.11): the brand greens, cream text, one gold for "selected / lit"
    inline juce::Colour body()    { return rgb (3, 55, 51); }       // #033733 behind the zones
    inline juce::Colour panelBg() { return rgb (5, 42, 39); }       // #052A27 the zones (top bar, band strip, control strip, footer)
    inline juce::Colour plot()    { return rgb (4, 34, 31); }       // #04221F the display
    inline juce::Colour grid()    { return rgb (15, 61, 56); }      // #0F3D38
    inline juce::Colour screen()  { return panelBg(); }
    inline juce::Colour line()    { return rgb (52, 81, 77); }      // cream at 20 % on a panel: an unlit outline
    inline juce::Colour text()    { return rgb (242, 239, 231); }   // cream #F2EFE7
    inline juce::Colour dim()     { return rgb (138, 152, 146); }   // cream at 55 %: 5.5 : 1 on the panel
    inline juce::Colour accent()  { return rgb (217, 188, 122); }   // #D9BC7A, the light gold of a selection
    inline juce::Colour gold()    { return rgb (197, 160, 89); }    // #C5A059
    /** The body of every key and chip on the glass (owner 26 Oct: "defalarca butonları ekrana gömmeyi istedim ama butonlar hala ekrandan ayrı görünüyor"):
        no box, no rim, no shadow: the control is part of the screen. Hover = a faint light on the glass; lit = a soft gold light under the legend and a
        short gold bar at the foot (the legend itself turns gold). */
    inline void body (juce::Graphics& g, juce::Rectangle<float> b, bool on, bool over, bool down, bool enabled = true, float radius = 5.0f, bool footBar = true)
    {
        if (! enabled) return;
        if (on)
        {
            g.setGradientFill (juce::ColourGradient (gold().withAlpha (0.0f), 0.0f, b.getY(), gold().withAlpha (down ? 0.16f : 0.11f), 0.0f, b.getBottom(), false));
            g.fillRoundedRectangle (b, radius);
            if (footBar) { g.setColour (gold()); g.fillRoundedRectangle (b.getCentreX() - b.getWidth() * 0.3f, b.getBottom() - 2.0f, b.getWidth() * 0.6f, 2.0f, 1.0f); }
        }
        else if (over || down) { g.setColour (text().withAlpha (down ? 0.08f : 0.05f)); g.fillRoundedRectangle (b, radius); }
    }
    /** A key on the glass: a legend written on the screen (gold when lit). */
    inline void key (juce::Graphics& g, juce::Rectangle<float> b, const juce::String& t, bool on, bool over, bool down, float em, float textShift = 0.0f, bool enabled = true)
    {
        body (g, b, on, over, down, enabled);
        silk (g, t, Theme::condEm (juce::jmin (em * 1.45f, 12.5f)), b.getCentreX() + textShift, b.getCentreY() + (down ? 1.0f : 0.0f), 'm', 0.6f, (on ? accent() : text()).withAlpha (enabled ? 1.0f : 0.4f));
    }
    /** A value on the glass: just the number (no box: it is part of the screen); editing / live = gold with a gold rule under it. */
    inline void value (juce::Graphics& g, juce::Rectangle<float> b, const juce::String& t, bool live, bool editing, float em, bool enabled = true)
    {
        if (editing || live) { g.setColour (accent().withAlpha (0.9f)); g.fillRect (b.getX() + 6.0f, b.getBottom() - 3.0f, b.getWidth() - 12.0f, 1.0f); }
        silk (g, t, Theme::monoEm (em * 0.92f), b.getCentreX(), b.getCentreY(), 'm', 0.0f, live || editing ? accent() : text().withAlpha (enabled ? 0.95f : 0.35f));
    }
    /** A ring knob on the screen (Pro-Q): the 270 degree track, the value arc in gold from the start (or the centre for a bipolar parameter), a pointer. */
    inline void knob (juce::Graphics& g, float cx, float cy, float r, float norm, bool bipolar)
    {
        const float a0 = -135.0f * fam::kPi / 180.0f, a1 = 135.0f * fam::kPi / 180.0f, av = a0 + (a1 - a0) * norm, ac = bipolar ? 0.0f : a0;
        juce::Path track; track.addCentredArc (cx, cy, r - 3.0f, r - 3.0f, 0.0f, a0, a1, true);
        g.setColour (text().withAlpha (0.14f)); g.strokePath (track, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        juce::Path v; v.addCentredArc (cx, cy, r - 3.0f, r - 3.0f, 0.0f, juce::jmin (ac, av), juce::jmax (ac, av), true);
        g.setColour (accent()); g.strokePath (v, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        const juce::Point<float> tip (cx + (r - 8.0f) * std::sin (av), cy - (r - 8.0f) * std::cos (av));
        g.setColour (text()); g.drawLine (cx, cy, tip.x, tip.y, 1.6f);
    }
    /** A popover on the screen (help, lists, ADVANCED): a darker smoked layer of the same glass with one faint edge. */
    inline void panel (juce::Graphics& g, juce::Rectangle<float> b, float alpha = 0.94f)
    {
        juce::ignoreUnused (alpha);   // opaque: the scale and the lockup must not show through a drawer (26 Oct)
        g.setColour (rgb (2, 22, 20)); g.fillRoundedRectangle (b, 6.0f);
        g.setColour (text().withAlpha (0.10f)); g.drawRoundedRectangle (b.reduced (0.5f), 6.0f, 1.0f);
    }
    inline void lamp (juce::Graphics& g, float cx, float cy, bool on)
    {
        if (on)
        {
            g.setColour (Theme::teal.withAlpha (0.22f)); g.fillEllipse (cx - 7.5f, cy - 7.5f, 15.0f, 15.0f);
            g.setColour (Theme::teal.withAlpha (0.40f)); g.fillEllipse (cx - 5.0f, cy - 5.0f, 10.0f, 10.0f);
        }
        g.setColour (on ? Theme::teal.brighter (0.5f) : rgb (14, 30, 28)); g.fillEllipse (cx - 3.5f, cy - 3.5f, 7.0f, 7.0f);
        if (on) { g.setColour (juce::Colours::white.withAlpha (0.55f)); g.fillEllipse (cx - 1.6f, cy - 2.4f, 2.2f, 1.6f); }
    }
    /** A hairline that fades out at both ends (the dividers between the zones of the glass). */
    inline void divider (juce::Graphics& g, float x0, float x1, float y, float alpha = 1.0f)
    {
        juce::ColourGradient cg (grid().withAlpha (0.0f), x0, y, grid().withAlpha (0.0f), x1, y, false);
        cg.addColour (0.12, grid().withAlpha (alpha)); cg.addColour (0.88, grid().withAlpha (alpha));
        g.setGradientFill (cg); g.fillRect (x0, y, x1 - x0, 1.0f);
        g.setColour (juce::Colours::black.withAlpha (0.35f * alpha)); g.fillRect (x0 + (x1 - x0) * 0.12f, y + 1.0f, (x1 - x0) * 0.76f, 1.0f);
    }
}

inline juce::Path dashed (const juce::Path& src, float on, float off, float width)
{
    // owner 26 Oct: dashed ("kağıt kesme") lines make the product look amateur: every former dashed line is a plain line now
    juce::ignoreUnused (on, off, width);
    return src;
}

//==============================================================================
/** A key on the screen. Optional lamp on the left (a separate click zone that can toggle something else), optional arrow (opens a list), optional dimming. */
class PlateKeyButton : public juce::Button, public PanelControl, private PanelCounted
{
public:
    PlateKeyButton (const juce::String& text, std::function<bool()> isLit, float textSize, float trackPx, float marginPx = 9.0f)
        : juce::Button (text), lit (std::move (isLit)), size (textSize), margin (marginPx) { juce::ignoreUnused (trackPx); }   // trackPx: the letter spacing of the old hardware face, unused on the screen
    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        const bool on = lit && lit();
        const auto box = getLocalBounds().toFloat().reduced (margin);
        dig::key (g, box, getButtonText(), on, over, down, size, lamp ? 7.0f : 0.0f, isEnabled());
        if (lamp) dig::lamp (g, box.getX() + 11.0f, box.getCentreY(), lamp());
        if (arrow)
        {
            const float x = box.getRight() - 11.0f, y = box.getCentreY() + 0.5f;
            juce::Path t; t.addTriangle (x - 4.0f, y - 2.0f, x + 4.0f, y - 2.0f, x, y + 3.0f);
            g.setColour (dig::text()); g.fillPath (t);
        }
    }
    bool hitTest (int x, int y) override { return getLocalBounds().reduced ((int) margin - 3).contains (x, y); }
    bool isLitNow() const { return lit && lit(); }
    void mouseDown (const juce::MouseEvent& e) override
    {
        lampClick = lamp && onLamp && juce::Rectangle<float> (getLocalBounds().toFloat().reduced (margin).getX(), 0.0f, 22.0f, (float) getHeight()).contains (e.position);
        if (lampClick) return;
        juce::Button::mouseDown (e);
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (lampClick) { lampClick = false; if (! e.mouseWasDraggedSinceMouseDown()) onLamp(); return; }
        juce::Button::mouseUp (e);
    }
    void mouseDoubleClick (const juce::MouseEvent&) override { if (onDouble) onDouble(); }
    void buttonStateChanged() override { if (onDownChange) onDownChange (isDown()); }
    juce::StringArray boundParams() const override { return bound ? bound() : juce::StringArray(); }
    juce::String uiAction() const override { return action; }
    std::function<void (bool)> onDownChange;
    std::function<juce::StringArray()> bound;
    std::function<void()> onDouble, onLamp;
    std::function<bool()> lamp;
    juce::String action;
    bool arrow = false;
private:
    std::function<bool()> lit; float size, margin; bool lampClick = false;
};

/** A small cream legend (not a control). */
class Legend : public juce::Component, private PanelCounted
{
public:
    Legend (juce::String t, float em, float track, juce::Colour c, char anchor = 'm') : text (std::move (t)), size (em), tr (track), col (c), h (anchor) {}
    void paint (juce::Graphics& g) override
    {
        const float x = h == 'l' ? 0.0f : h == 'r' ? (float) getWidth() : (float) getWidth() * 0.5f;
        silk (g, textFn ? textFn() : text, Theme::legendEm (size), x, (float) getHeight() * 0.5f, h, tr, col == Theme::label ? dig::dim() : col);
    }
    bool hitTest (int, int) override { return false; }
    juce::String text; std::function<juce::String()> textFn;
private:
    float size, tr; juce::Colour col; char h;
};

//==============================================================================
/** The hover help of a control (25 Oct, tester report T3: "hovering BAKE ARM shows no tooltip or instructions"). UI signature standard 4: help opens in
    place as a popover over the panel, never as a panel that makes the window taller. A title and a wrapped text, placed above the control (below it
    when there is no room), kept inside the window. It takes no mouse: a click goes to what is under it, and any click or Escape closes it. */
class HelpTip : public juce::Component, private PanelCounted
{
public:
    HelpTip() { setInterceptsMouseClicks (false, false); setName ("POPOVER"); }
    static constexpr int kW = 400;
    void show (const juce::String& title, const juce::String& body, juce::Rectangle<int> anchor, juce::Rectangle<int> area)
    {
        ttl = title; txt = body;
        juce::TextLayout tl; tl.createLayout (bodyText(), (float) (kW - 28));
        const int h = 36 + (int) std::ceil (tl.getHeight());
        const int x = juce::jlimit (area.getX() + 4, area.getRight() - kW - 4, anchor.getCentreX() - kW / 2);
        int y = anchor.getY() - h - 6;
        if (y < area.getY() + 4) y = juce::jmin (anchor.getBottom() + 6, area.getBottom() - h - 4);
        setBounds (x, y, kW, h);
        setVisible (true); toFront (false); repaint();
    }
    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat().reduced (1.0f);
        dig::panel (g, r, 0.99f);
        g.setColour (dig::gold()); g.fillRoundedRectangle (r.getX() + 4.0f, r.getY() + 7.0f, 3.0f, 16.0f, 1.5f);
        silk (g, ttl, Theme::condEm (11.5f), 16.0f, 15.0f, 'l', 1.0f, dig::accent());
        juce::TextLayout tl; tl.createLayout (bodyText(), (float) (kW - 28));
        tl.draw (g, { 14.0f, 28.0f, (float) (kW - 28), r.getHeight() - 30.0f });
    }
    const juce::String& title() const { return ttl; }
    const juce::String& text() const { return txt; }
private:
    juce::AttributedString bodyText() const
    {
        juce::AttributedString as; as.setJustification (juce::Justification::topLeft); as.setLineSpacing (2.0f);
        as.append (txt, Theme::fineEm (8.6f), dig::text());
        return as;
    }
    juce::String ttl, txt;
};

//==============================================================================
/** The black value window of a parameter. A single click opens typed entry; a vertical drag moves the value; double-click and Alt-click return the default. */
class ValueWin : public juce::Component, public PanelControl, private PanelCounted
{
public:
    ValueWin (panel::Ctl& c, juce::String paramId, float fontEm = 17.0f, bool editable = true) : ctl (c), id (std::move (paramId)), em (fontEm), canEdit (editable) {}
    juce::String text() const { return textFn ? textFn() : dyneq::ptext::format (id, ctl.raw (id)); }
    void paint (juce::Graphics& g) override
    {
        if (! isActive()) { dig::value (g, getLocalBounds().toFloat(), "--", false, false, em, false); return; }
        dig::value (g, getLocalBounds().toFloat(), text(), live, isEditing(), em);
    }
    // LAB's docked strip (UX redesign 11 Oct): a box can have its own drag law, write linked parameters with its value, and be inactive (dimmed, keeps its place).
    std::function<double (double plain, double upPixels, bool fine)> dragMap;   // the value after a vertical drag of upPixels from `plain`; empty = 200 px for the whole range
    juce::StringArray linked;                                                  // written together with this parameter, in the same gesture / undo step
    std::function<void (float plain)> onPlain;                                 // writes the linked parameters for a new value
    std::function<bool()> active;                                              // false: dimmed "--", not a control
    bool isActive() const { return ! active || active(); }
    void resized() override { if (field != nullptr) field->setBounds (getLocalBounds().reduced (5, 4)); }
    void setLive (bool l) { live = l; repaint(); }
    bool isEditing() const { return field != nullptr && field->isVisible(); }
    void beginEdit()
    {
        if (! canEdit || isEditing()) return;
        if (field == nullptr)
        {
            field = std::make_unique<juce::TextEditor>();
            field->setMultiLine (false); field->setReturnKeyStartsNewLine (false); field->setScrollbarsShown (false);
            field->setJustification (juce::Justification::centred); field->setInputRestrictions (18); field->setSelectAllWhenFocused (true);
            field->setColour (juce::TextEditor::backgroundColourId, rgb (3, 14, 13)); field->setColour (juce::TextEditor::textColourId, Theme::lamp);
            field->setColour (juce::TextEditor::highlightColourId, Theme::goldDark); field->setColour (juce::TextEditor::highlightedTextColourId, Theme::label);
            field->setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack); field->setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
            field->setColour (juce::CaretComponent::caretColourId, Theme::lamp);
            field->setInterceptsMouseClicks (false, false);   // every click stays with the box: a second click is a double-click (default), not a caret move
            field->onReturnKey = [this] { endEdit (true); };
            field->onEscapeKey = [this] { endEdit (false); };
            field->onFocusLost = [this] { endEdit (true); };   // a click elsewhere keeps what was typed (Escape cancels)
            addChildComponent (*field);
        }
        field->setFont (Theme::monoEm (em));
        field->setText (text(), false);
        field->setBounds (getLocalBounds().reduced (5, 4));
        field->setVisible (true); field->grabKeyboardFocus(); field->selectAll(); repaint();
    }
    /** Test hook: the text in the box now, and a typed value committed as the Return key would. */
    juce::String editText() const { return field != nullptr ? field->getText() : juce::String(); }
    void typeAndCommit (const juce::String& t) { if (! isEditing()) beginEdit(); if (field != nullptr) { field->setText (t, false); endEdit (true); } }
    void cancelEdit() { if (isEditing()) endEdit (false); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        swallow = false; dragging = false;
        if (! isActive()) { swallow = true; return; }
        if (! e.mods.isPopupMenu() && e.mods.isAltDown()) { swallow = true; resetNow(); return; }
        startNorm = isNumeric() ? ctl.par (id)->convertTo0to1 (ctl.raw (id)) : 0.0f;
        startPlain = isNumeric() ? ctl.raw (id) : 0.0f;
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (swallow || ! canEdit || isEditing()) return;
        if (! dragging)
        {
            if (std::abs (e.getDistanceFromDragStartY()) < 4) return;
            dragging = true; ctl.par (id)->beginChangeGesture(); for (auto& l : linked) ctl.par (l)->beginChangeGesture(); setLive (true);
        }
        if (dragMap) { writePlain ((float) dragMap (startPlain, (double) -e.getDistanceFromDragStartY(), e.mods.isShiftDown())); return; }
        const float n = juce::jlimit (0.0f, 1.0f, startNorm - (float) e.getDistanceFromDragStartY() / (e.mods.isShiftDown() ? 1000.0f : 200.0f));
        ctl.par (id)->setValueNotifyingHost (n);
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (dragging) { ctl.par (id)->endChangeGesture(); for (auto& l : linked) ctl.par (l)->endChangeGesture(); dragging = false; setLive (false); return; }
        const bool wasSwallowed = swallow; swallow = false;
        if (! wasSwallowed && canEdit && e.getNumberOfClicks() < 2 && ! e.mouseWasDraggedSinceMouseDown() && ! isEditing()) beginEdit();
    }
    void mouseDoubleClick (const juce::MouseEvent&) override { if (! isActive()) return; cancelEdit(); resetNow(); }
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (! dragMap || ! isActive() || ! canEdit || isEditing()) { juce::Component::mouseWheelMove (e, w); return; }
        const float d = w.deltaY != 0.0f ? w.deltaY : w.deltaX;
        if (d == 0.0f) return;
        svl::ParamUndo::Group one (ctl.proc.undo, "WHEEL");
        writePlain ((float) dragMap (ctl.raw (id), d > 0.0f ? 6.0 : -6.0, e.mods.isShiftDown()));
    }
    juce::StringArray boundParams() const override { if (! isActive()) return {}; juce::StringArray a { id }; a.addArray (linked); return a; }
    juce::StringArray resetParams() const override { if (! isActive() || ! canEdit) return {}; juce::StringArray a { id }; a.addArray (linked); return a; }
    juce::String uiAction() const override { return isActive() ? juce::String() : juce::String ("INACTIVE"); }
    void setId (const juce::String& newId) { if (newId != id) { cancelEdit(); id = newId; repaint(); } }
    const juce::String& paramId() const { return id; }
    std::function<juce::String()> textFn;
private:
    bool isNumeric() const { return ctl.par (id) != nullptr; }
    /** Writes a plain value (clamped to the range) and the linked parameters; the caller holds the gesture or the undo group. */
    void writePlain (float plain)
    {
        auto* p = ctl.par (id); const auto r = p->getNormalisableRange();
        plain = juce::jlimit (r.start, r.end, plain);
        p->setValueNotifyingHost (p->convertTo0to1 (plain));
        if (onPlain) onPlain (ctl.raw (id));
    }
    void resetNow()
    {
        if (! canEdit) return;
        if (linked.isEmpty()) { ctl.setGesture (id, ctl.defaultOf (id)); return; }
        svl::ParamUndo::Group one (ctl.proc.undo, "DEFAULT");
        writePlain (ctl.defaultOf (id));
    }
    void endEdit (bool commit)
    {
        if (! isEditing() || closing) return;
        closing = true;
        const auto typed = field->getText();
        field->setVisible (false);
        if (commit)
        {
            float plain = 0.0f; int idx = 0;
            const auto r = ctl.par (id)->getNormalisableRange();
            if (dyneq::ptext::choiceNames (id).size() > 0)
            {
                if (dyneq::ptext::parseChoice (id, typed, idx)) ctl.setGesture (id, (float) idx);
            }
            else if (dyneq::ptext::parse (id, typed, plain))
            {
                if (linked.isEmpty()) ctl.setGesture (id, juce::jlimit (r.start, r.end, plain));
                else { svl::ParamUndo::Group one (ctl.proc.undo, "TYPED"); writePlain (plain); }
            }
        }
        closing = false; repaint();
        if (auto* ed = findParentComponentOfClass<DyneqEditor>()) ed->grabKeyboardFocus();
    }
    panel::Ctl& ctl; juce::String id; float em; bool canEdit; bool live = false, closing = false, swallow = false, dragging = false; float startNorm = 0.0f, startPlain = 0.0f;
    std::unique_ptr<juce::TextEditor> field;
};

/** A ring knob on the screen, bound to one parameter. Vertical drag (shift = fine); double-click and Alt-click = default; wheel steps. */
class KnobComp : public juce::Component, public PanelControl, private PanelCounted
{
public:
    KnobComp (panel::Ctl& c, juce::String paramId, float radius) : ctl (c), id (std::move (paramId)), r (radius) { setRepaintsOnMouseActivity (false); }
    void paint (juce::Graphics& g) override
    {
        const float cx = (float) getWidth() * 0.5f, cy = (float) getHeight() * 0.5f;
        const bool bip = id.endsWith ("gain") || id == "output";
        dig::knob (g, cx, cy, r * 1.08f, ctl.par (id)->convertTo0to1 (ctl.raw (id)), bip);
    }
    bool hitTest (int x, int y) override { return juce::Point<float> ((float) x, (float) y).getDistanceFrom ({ (float) getWidth() * 0.5f, (float) getHeight() * 0.5f }) <= r * 1.1f; }
    void mouseDown (const juce::MouseEvent& e) override
    {
        swallow = false;
        if (! e.mods.isPopupMenu() && e.mods.isAltDown()) { swallow = true; ctl.setGesture (id, ctl.defaultOf (id)); return; }
        startNorm = ctl.par (id)->convertTo0to1 (ctl.raw (id)); fine = e.mods.isShiftDown();
        ctl.par (id)->beginChangeGesture(); gesture = true;
        if (win != nullptr) win->setLive (true);
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (swallow || ! gesture) return;
        const float n = juce::jlimit (0.0f, 1.0f, startNorm - (float) e.getDistanceFromDragStartY() / (fine ? 1250.0f : 250.0f));
        ctl.par (id)->setValueNotifyingHost (n);
    }
    void mouseUp (const juce::MouseEvent&) override
    {
        if (gesture) { ctl.par (id)->endChangeGesture(); gesture = false; }
        if (win != nullptr) win->setLive (false);
        swallow = false;
    }
    void mouseDoubleClick (const juce::MouseEvent&) override { ctl.setGesture (id, ctl.defaultOf (id)); }
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        auto* p = ctl.par (id);
        const float step = p->getNumSteps() > 1 && p->getNumSteps() < 64 ? 1.0f / (float) (p->getNumSteps() - 1) : 0.02f;
        p->beginChangeGesture(); p->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, p->getValue() + (w.deltaY > 0 ? step : -step))); p->endChangeGesture();
    }
    juce::StringArray boundParams() const override { return { id }; }
    juce::StringArray resetParams() const override { return { id }; }
    void setId (const juce::String& newId) { if (newId != id) { id = newId; repaint(); } }
    ValueWin* win = nullptr;
private:
    panel::Ctl& ctl; juce::String id; float r; float startNorm = 0.0f; bool swallow = false, fine = false, gesture = false;
};

/** A knob with its caption above and its value window below; the unit most panels use (FREQ, GAIN, Q, THRESHOLD ...). */
class ParamKnob : public juce::Component, private PanelCounted
{
public:
    ParamKnob (panel::Ctl& c, const juce::String& paramId, const juce::String& caption, float radius = 22.0f, int winWidth = 92, float winEm = 15.0f)
        : label (caption), r (radius), ww (winWidth)
    {
        knob = std::make_unique<KnobComp> (c, paramId, radius);
        win = std::make_unique<ValueWin> (c, paramId, winEm, true);
        knob->win = win.get();
        addAndMakeVisible (*knob); addAndMakeVisible (*win);
        setInterceptsMouseClicks (false, true);
        setSize (sizeFor (radius, winWidth).x, sizeFor (radius, winWidth).y);
    }
    static juce::Point<int> sizeFor (float radius, int winWidth) { return { juce::jmax (winWidth + 4, (int) (radius * 2.9f)), 15 + (int) (radius * 2.9f) + 28 }; }
    void resized() override
    {
        const int kd = (int) (r * 2.9f);
        knob->setBounds ((getWidth() - kd) / 2, 15, kd, kd);
        win->setBounds ((getWidth() - ww) / 2, 15 + kd, ww, 28);
    }
    void paint (juce::Graphics& g) override { silk (g, label, Theme::legendEm (7.6f), (float) getWidth() * 0.5f, 7.5f, 'm', 1.6f, dimmed ? Theme::muted.withAlpha (0.5f) : dig::dim()); }
    void setParamId (const juce::String& id) { knob->setId (id); win->setId (id); }
    // The knob and the window take the mouse. NOT hitTest() == false: JUCE asks a component's own hitTest before its children, so that made the knob and
    // its value window unreachable in a real host (owner, Live 11 Oct: "knoblar elle cevrilemiyor, elle deger girilemiyor"); the synthesized test events never noticed.
    juce::String label; bool dimmed = false;
    std::unique_ptr<KnobComp> knob; std::unique_ptr<ValueWin> win;
private:
    float r; int ww;
};

//==============================================================================
/** The list popup (type / instrument chooser and the band's right-click menu). Headings are drawn small and cannot be picked. */
class Chooser : public juce::Component, private PanelCounted
{
public:
    explicit Chooser (DyneqEditor& e) : ed (e) { setName ("POPOVER"); setVisible (false); }
    void show (std::vector<panel::ChooserItem> list, juce::Point<int> where)
    {
        items = std::move (list); hover = -1; rowY.clear();
        int y = 4; for (auto& it : items) { rowY.push_back (y); y += it.heading ? kHead : kRow; }
        const int w = 214, h = y + 4;
        const auto parent = getParentComponent() != nullptr ? getParentComponent()->getLocalBounds() : juce::Rectangle<int> (0, 0, 1000, 960);
        setBounds (juce::jlimit (4, parent.getWidth() - w - 4, where.x - 70), juce::jlimit (4, parent.getHeight() - h - 4, where.y + 14), w, h);
        setVisible (true); toFront (false); repaint();
    }
    int numItems() const { return (int) items.size(); }
    juce::String itemText (int i) const { return items[(size_t) i].text; }
    bool itemSelected (int i) const { return items[(size_t) i].selected; }
    bool itemHeading (int i) const { return items[(size_t) i].heading; }
    void choose (int i)
    {
        if (i < 0 || i >= numItems() || items[(size_t) i].heading) return;
        auto fn = items[(size_t) i].apply;
        ed.closeChooser();
        if (fn) fn();
        ed.refreshFace();
    }
    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        dig::panel (g, b, 0.98f);
        for (int i = 0; i < numItems(); ++i)
        {
            const auto& it = items[(size_t) i];
            const auto row = juce::Rectangle<float> (4.0f, (float) rowY[(size_t) i], b.getWidth() - 8.0f, (float) (it.heading ? kHead : kRow));
            if (it.heading) { silk (g, it.text, Theme::legendEm (7.0f), row.getX() + 8.0f, row.getCentreY(), 'l', 1.8f, Theme::muted); continue; }
            if (i == hover) { g.setColour (rgb (10, 52, 48)); g.fillRoundedRectangle (row, 3.0f); }
            silk (g, it.text, Theme::condEm (15.0f * kPx), row.getX() + 10.0f, row.getCentreY(), 'l', 1.0f, it.selected ? Theme::gold : Theme::labelDeep);
            if (it.selected) { g.setColour (Theme::gold); g.fillEllipse (row.getRight() - 14.0f, row.getCentreY() - 3.0f, 6.0f, 6.0f); }
        }
    }
    void mouseMove (const juce::MouseEvent& e) override { const int h = rowAt (e.position); if (h != hover) { hover = h; repaint(); } }
    void mouseExit (const juce::MouseEvent&) override { hover = -1; repaint(); }
    void mouseUp (const juce::MouseEvent& e) override { choose (rowAt (e.position)); }
    bool keyPressed (const juce::KeyPress& k) override { if (k == juce::KeyPress::escapeKey) { ed.closeChooser(); return true; } return false; }
private:
    static constexpr int kRow = 25, kHead = 20;
    int rowAt (juce::Point<float> p) const
    {
        for (int i = 0; i < numItems(); ++i)
            if (! items[(size_t) i].heading && p.y >= (float) rowY[(size_t) i] && p.y < (float) (rowY[(size_t) i] + kRow)) return i;
        return -1;
    }
    DyneqEditor& ed; std::vector<panel::ChooserItem> items; std::vector<int> rowY; int hover = -1;
};

//==============================================================================
// helpers that build the common key shapes
inline std::unique_ptr<PlateKeyButton> makeToggle (panel::Ctl& c, DyneqEditor& ed, const juce::String& paramId, const juce::String& text, float em, float track, float margin = 5.0f)
{
    auto k = std::make_unique<PlateKeyButton> (text, [&c, paramId] { return c.on (paramId); }, em, track, margin);
    k->bound = [paramId] { return juce::StringArray { paramId }; };
    k->onClick = [&c, &ed, paramId] { c.setGesture (paramId, c.on (paramId) ? 0.0f : 1.0f); ed.refreshFace(); };
    return k;
}
/** One key of a choice parameter (SPEED FAST / MEDIUM / SLOW ...): lit when the parameter is this value. */
inline std::unique_ptr<PlateKeyButton> makeChoiceKey (panel::Ctl& c, DyneqEditor& ed, const juce::String& paramId, int value, const juce::String& text, float em, float track, float margin = 5.0f)
{
    auto k = std::make_unique<PlateKeyButton> (text, [&c, paramId, value] { return juce::roundToInt (c.raw (paramId)) == value; }, em, track, margin);
    k->bound = [paramId] { return juce::StringArray { paramId }; };
    k->onClick = [&c, &ed, paramId, value] { c.setGesture (paramId, (float) value); ed.refreshFace(); };
    return k;
}
} // namespace panelui
