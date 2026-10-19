#include "PluginEditor.h"
#include "ParamText.h"

namespace
{
using fam::rgb;
using fam::silk;
using Pt = juce::Point<float>;
constexpr float kPi = fam::kPi;

//==============================================================================
// seven segments in amber on dark glass: digits, a dot, a slash and the few letters the read-outs need (AUTO, 1/8t, 1/4d)
float segAdvance (char ch, float h) { return ch == '.' ? 0.30f * h : ch == '/' ? 0.52f * h : ch == ' ' ? 0.40f * h : 0.76f * h; }

float segWidth (const juce::String& s, float h)
{
    float w = 0.0f;
    for (auto c : s) w += segAdvance ((char) c, h);
    return w;
}

void segChar (juce::Graphics& g, float x, float y, float h, char ch, juce::Colour on, juce::Colour off)
{
    static const std::map<char, const char*> lit {
        { '0', "abcdef" }, { '1', "bc" }, { '2', "abdeg" }, { '3', "abcdg" }, { '4', "bcfg" }, { '5', "acdfg" }, { '6', "acdefg" }, { '7', "abc" },
        { '8', "abcdefg" }, { '9', "abcdfg" }, { '-', "g" }, { ' ', "" }, { 'A', "abcefg" }, { 'U', "bcdef" }, { 'O', "abcdef" }, { 'T', "aef" },
        { 't', "defg" }, { 'd', "bcdeg" }, { 'F', "aefg" }, { 'r', "eg" }, { 'E', "adefg" }, { 'S', "acdfg" }, { 'N', "abcef" }, { 'o', "cdeg" } };
    const float th = 0.13f * h, sl = std::tan (7.0f * kPi / 180.0f);
    auto warp = [&] (float px, float py) { return Pt (x + px - (py - h * 0.5f) * sl, y + py); };
    if (ch == '.')
    {
        const auto c = warp (0.10f * h, h - 0.07f * h);
        fam::glow (g, c, h * 0.22f, on, 0.5f);
        g.setColour (on);
        g.fillEllipse (c.x - th * 0.55f, c.y - th * 0.55f, th * 1.1f, th * 1.1f);
        return;
    }
    if (ch == '/')
    {
        juce::Path p;
        p.startNewSubPath (warp (0.40f * h, 0.06f * h)); p.lineTo (warp (0.08f * h, h - 0.06f * h));
        g.setColour (on.withAlpha (0.25f));
        g.strokePath (p, juce::PathStrokeType (th * 1.9f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (on);
        g.strokePath (p, juce::PathStrokeType (th * 0.95f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        return;
    }
    const float w = 0.56f * h, gp = th * 0.16f;
    const float L = th * 0.5f, R = w - th * 0.5f, T = th * 0.5f, M = h * 0.5f, B = h - th * 0.5f;
    auto hs = [&] (float xa, float xb, float yc)
    { return std::vector<Pt> { { xa, yc }, { xa + th / 2, yc - th / 2 }, { xb - th / 2, yc - th / 2 }, { xb, yc }, { xb - th / 2, yc + th / 2 }, { xa + th / 2, yc + th / 2 } }; };
    auto vs = [&] (float xc, float ya, float yb)
    { return std::vector<Pt> { { xc, ya }, { xc + th / 2, ya + th / 2 }, { xc + th / 2, yb - th / 2 }, { xc, yb }, { xc - th / 2, yb - th / 2 }, { xc - th / 2, ya + th / 2 } }; };
    const std::pair<char, std::vector<Pt>> segs[] {
        { 'a', hs (L + gp, R - gp, T) }, { 'b', vs (R, T + gp, M - gp) }, { 'c', vs (R, M + gp, B - gp) }, { 'd', hs (L + gp, R - gp, B) },
        { 'e', vs (L, M + gp, B - gp) }, { 'f', vs (L, T + gp, M - gp) }, { 'g', hs (L + gp, R - gp, M) } };
    const auto it = lit.find (ch);
    const juce::String lights (it != lit.end() ? it->second : "");
    for (const auto& [name, pts] : segs)
    {
        juce::Path p;
        for (size_t i = 0; i < pts.size(); ++i)
        {
            const auto q = warp (pts[i].x, pts[i].y);
            if (i == 0) p.startNewSubPath (q); else p.lineTo (q);
        }
        p.closeSubPath();
        if (lights.containsChar ((juce::juce_wchar) name))
        {
            g.setColour (on.withAlpha (0.20f));
            g.strokePath (p, juce::PathStrokeType (h * 0.09f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (on.withAlpha (0.32f));
            g.strokePath (p, juce::PathStrokeType (h * 0.045f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (on);
        }
        else
            g.setColour (off);
        g.fillPath (p);
    }
}

/** Draws `s` in seven segments, centred on `cx` (the box's centre), top at y. */
void segText (juce::Graphics& g, const juce::String& s, float cx, float y, float h, juce::Colour on, juce::Colour off)
{
    float x = cx - segWidth (s, h) * 0.5f;
    for (auto c : s)
    {
        segChar (g, x, y, h, (char) c, on, off);
        x += segAdvance ((char) c, h);
    }
}

// 5 x 7 dot-matrix characters of the program name, drawn as red LEDs (lit dots glow, unlit dots stay dark)
const char* const* matrixGlyph (char ch)
{
    static const char* const P[] { "XXXX.", "X...X", "X...X", "XXXX.", "X....", "X....", "X...." };
    static const char* const M[] { "X...X", "XX.XX", "X.X.X", "X.X.X", "X...X", "X...X", "X...X" };
    static const char* const A[] { ".XXX.", "X...X", "X...X", "XXXXX", "X...X", "X...X", "X...X" };
    static const char* const N[] { "X...X", "XX..X", "X.X.X", "X..XX", "X...X", "X...X", "X...X" };
    static const char* const U[] { "X...X", "X...X", "X...X", "X...X", "X...X", "X...X", ".XXX." };
    static const char* const L[] { "X....", "X....", "X....", "X....", "X....", "X....", "XXXXX" };
    static const char* const V[] { "X...X", "X...X", "X...X", "X...X", "X...X", ".X.X.", "..X.." };
    static const char* const O[] { ".XXX.", "X...X", "X...X", "X...X", "X...X", "X...X", ".XXX." };
    static const char* const C[] { ".XXX.", "X...X", "X....", "X....", "X....", "X...X", ".XXX." };
    static const char* const D[] { "XXXX.", "X...X", "X...X", "X...X", "X...X", "X...X", "XXXX." };
    static const char* const R[] { "XXXX.", "X...X", "X...X", "XXXX.", "X.X..", "X..X.", "X...X" };
    static const char* const S[] { ".XXXX", "X....", "X....", ".XXX.", "....X", "....X", "XXXX." };
    static const char* const B[] { "XXXX.", "X...X", "X...X", "XXXX.", "X...X", "X...X", "XXXX." };
    static const char* const K[] { "X...X", "X..X.", "X.X..", "XX...", "X.X..", "X..X.", "X...X" };
    static const char* const E[] { "XXXXX", "X....", "X....", "XXXX.", "X....", "X....", "XXXXX" };
    static const char* const Y[] { "X...X", "X...X", ".X.X.", "..X..", "..X..", "..X..", "..X.." };
    static const char* const d0[] { ".XXX.", "X...X", "X..XX", "X.X.X", "XX..X", "X...X", ".XXX." };
    static const char* const d1[] { "..X..", ".XX..", "..X..", "..X..", "..X..", "..X..", ".XXX." };
    static const char* const d2[] { ".XXX.", "X...X", "....X", "...X.", "..X..", ".X...", "XXXXX" };
    static const char* const d3[] { "XXXX.", "....X", "....X", ".XXX.", "....X", "....X", "XXXX." };
    static const char* const d4[] { "...X.", "..XX.", ".X.X.", "X..X.", "XXXXX", "...X.", "...X." };
    switch (ch)
    {
        case 'P': return P; case 'M': return M; case 'A': return A; case 'N': return N; case 'U': return U; case 'L': return L; case 'V': return V;
        case 'O': return O; case 'C': return C; case 'D': return D; case 'R': return R; case 'S': return S; case 'B': return B; case 'K': return K;
        case 'E': return E; case 'Y': return Y; case '0': return d0; case '1': return d1; case '2': return d2; case '3': return d3; case '4': return d4;
        default: return nullptr;
    }
}

/** `s` as a dot-matrix LED line: `cells` character cells (so the unlit dots show where text can be), lit dots in `on`. */
void matrixText (juce::Graphics& g, const juce::String& s, float x, float y, float pitch, int cells, juce::Colour on, juce::Colour off)
{
    for (int i = 0; i < cells; ++i)
    {
        const auto* gl = i < s.length() ? matrixGlyph ((char) s[i]) : nullptr;
        for (int r = 0; r < 7; ++r)
            for (int q = 0; q < 5; ++q)
            {
                const bool lit = gl != nullptr && gl[r][q] == 'X';
                const float cx = x + ((float) i * 6.0f + (float) q + 0.5f) * pitch, cy = y + ((float) r + 0.5f) * pitch, rad = pitch * 0.40f;
                if (lit) fam::glow (g, { cx, cy }, pitch * 1.1f, on, 0.35f);
                g.setColour (lit ? on : off);
                g.fillEllipse (cx - rad, cy - rad, 2.0f * rad, 2.0f * rad);
            }
    }
}

void bracket (juce::Graphics& g, float x0, float x1, float y, const juce::String& title, float size = 10.0f)
{
    const auto f = Theme::legendEm (size);
    const float cx = (x0 + x1) * 0.5f, tw = fam::textWidth (title, f, 3.0f), gap = 10.0f;
    g.setColour (Theme::ink);
    g.drawLine (x0, y, cx - tw * 0.5f - gap, y, 1.1f);
    g.drawLine (cx + tw * 0.5f + gap, y, x1, y, 1.1f);
    g.drawLine (x0, y, x0, y + 7.0f, 1.1f);
    g.drawLine (x1, y, x1, y + 7.0f, 1.1f);
    silk (g, title, f, cx, y, 'm', 3.0f, Theme::ink);
}

void vgroove (juce::Graphics& g, float x, float y0, float y1)
{
    g.setColour (rgb (0, 0, 0, 120));
    g.drawLine (x, y0, x, y1, 1.0f);
    g.setColour (rgb (255, 255, 255, 34));
    g.drawLine (x + 1.2f, y0, x + 1.2f, y1, 0.8f);
}

const char* const kNoteTextAuto = "AUTO";
constexpr float kLegendK = 1.62f;   // the knob's name sits this many radii below its centre

juce::String dbText (float v) { return seda::ptext::fixed (v, 1) + " dB"; }
} // namespace

//==============================================================================
namespace sedaui
{
SedaLookAndFeel::SedaLookAndFeel()
{
    setDefaultSansSerifTypeface (faces->regular);   // menus and tooltips too: nothing on the panel asks the system for a font
    setColour (juce::PopupMenu::backgroundColourId, Theme::panelDeep);
    setColour (juce::PopupMenu::textColourId, Theme::ink);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, Theme::goldDark);
    setColour (juce::PopupMenu::highlightedTextColourId, Theme::label);
    setColour (juce::TextEditor::backgroundColourId, rgb (3, 14, 13));
}

void SedaLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float startAngle, float endAngle, juce::Slider& s)
{
    const float r = ((float) juce::jmin (w, h) - 4.0f) * 0.5f;
    const float cx = (float) x + (float) w * 0.5f, cy = (float) y + (float) h * 0.5f;
    const float eff = (float) s.getProperties().getWithDefault ("effPos", -1.0f);   // while a program moves the knob: where it is on its way, 0..1
    if (eff >= 0.0f) pos = eff;
    const float ang = (startAngle + pos * (endAngle - startAngle)) * 180.0f / kPi;
    const bool dim = (bool) s.getProperties().getWithDefault ("dim", false);
    if (dim) g.beginTransparencyLayer (0.5f);
    fam::paintKnob (g, cx, cy, r, ang, false);
    if (dim) g.endTransparencyLayer();
}

//==============================================================================
void KnobSlider::resetToDefault()
{
    if (param != nullptr)
        svl::setParamAsGesture (param, param->convertFrom0to1 (param->getDefaultValue()));
}

void KnobSlider::mouseDown (const juce::MouseEvent& e)
{
    swallow = false;
    const auto m = e.mods;
    if (! m.isPopupMenu() && (m.isAltDown() || m.isCommandDown()))
    {
        swallow = true;
        if (onDoubleClick) onDoubleClick(); else resetToDefault();   // the editor's reset: the loaded program's value, with the glide
        return;
    }
    setMouseDragSensitivity (m.isShiftDown() ? 1250 : 250);
    if (onHold) onHold (true);
    juce::Slider::mouseDown (e);
}

void KnobSlider::mouseDrag (const juce::MouseEvent& e) { if (! swallow) juce::Slider::mouseDrag (e); }

void KnobSlider::mouseUp (const juce::MouseEvent& e)
{
    if (swallow) { swallow = false; return; }
    juce::Slider::mouseUp (e);
    if (onHold) onHold (false);
}

void KnobSlider::mouseDoubleClick (const juce::MouseEvent&) { if (onDoubleClick) onDoubleClick(); }

//==============================================================================
ValueBox::ValueBox (juce::AudioProcessorValueTreeState& a, const juce::String& paramId, std::function<juce::String()> t)
    : apvts (a), id (paramId), textFn (std::move (t))
{
    setRepaintsOnMouseActivity (false);
}

void ValueBox::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    if (dim) g.beginTransparencyLayer (0.55f);
    if (customPaint)
    {
        fam::paintValueWindow (g, b, {}, false, isEditing());
        if (! isEditing()) customPaint (g, b.reduced (3.0f));
    }
    else if (dim && reasonFn && ! isEditing())
    {
        fam::paintValueWindow (g, b, {}, false, false);
        silk (g, reasonFn(), Theme::legendEm (juce::jmin (8.0f, fontEm * 0.62f)), b.getCentreX(), b.getCentreY() + 0.5f, 'm', 1.4f, Theme::muted);
    }
    else
        fam::paintValueWindow (g, b, text(), live, isEditing(), fontEm);
    if (dim) g.endTransparencyLayer();
}

void ValueBox::refreshIfChanged()
{
    const auto sig = text() + (dim ? "|d" + (reasonFn ? reasonFn() : juce::String()) : juce::String()) + (live ? "|l" : "");
    if (sig != lastSig) { lastSig = sig; repaint(); }
}

void ValueBox::resized() { if (field != nullptr) field->setBounds (getLocalBounds().reduced (5, 4)); }

void ValueBox::beginEdit()
{
    if (isEditing()) return;
    if (field == nullptr)
    {
        field = std::make_unique<juce::TextEditor>();
        field->setMultiLine (false);
        field->setReturnKeyStartsNewLine (false);
        field->setScrollbarsShown (false);
        field->setJustification (juce::Justification::centred);
        field->setInputRestrictions (18);
        field->setSelectAllWhenFocused (true);
        field->setColour (juce::TextEditor::backgroundColourId, rgb (3, 14, 13));
        field->setColour (juce::TextEditor::textColourId, Theme::lamp);
        field->setColour (juce::TextEditor::highlightColourId, Theme::goldDark);
        field->setColour (juce::TextEditor::highlightedTextColourId, Theme::label);
        field->setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        field->setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
        field->setColour (juce::CaretComponent::caretColourId, Theme::lamp);
        field->setFont (Theme::monoEm (fontEm));
        field->onReturnKey = [this] { endEdit (true); };
        field->onEscapeKey = [this] { endEdit (false); };
        field->onFocusLost = [this] { endEdit (false); };
        addChildComponent (*field);
    }
    field->setText (text(), false);
    field->applyFontToAllText (Theme::monoEm (fontEm));
    field->setBounds (getLocalBounds().reduced (5, 4));
    field->setVisible (true);
    field->grabKeyboardFocus();
    field->selectAll();
    repaint();
}

bool ValueBox::commitText (const juce::String& typed)
{
    auto* p = dynamic_cast<juce::RangedAudioParameter*> (apvts.getParameter (id));
    if (p == nullptr) return false;
    float plain = 0.0f;
    if (! seda::ptext::parse (id, typed, plain)) return false;
    const auto r = p->getNormalisableRange();
    svl::setParamAsGesture (p, juce::jlimit (r.start, r.end, plain));   // clamped to the range; one gesture, one undo step
    return true;
}

void ValueBox::endEdit (bool commit)
{
    if (! isEditing() || closing) return;
    closing = true;
    const auto typed = field->getText();
    field->setVisible (false);   // hidden, not deleted: this runs inside the field's own key callback
    if (commit) commitText (typed);
    closing = false;
    repaint();
    if (auto* ed = findParentComponentOfClass<SedaEditor>())
        ed->grabKeyboardFocus();   // Cmd+Z works again at once
}

//==============================================================================
PlateKey::PlateKey (const juce::String& t, const juce::String& param, std::function<bool()> isLit, std::function<void()> onPress, float textSize, float trackPx, bool withLamp)
    : juce::Button (t), pid (param), lit (std::move (isLit)), press (std::move (onPress)), size (textSize), track (trackPx), lamp (withLamp) {}

void PlateKey::paintButton (juce::Graphics& g, bool over, bool down)
{
    const bool on = isLit();
    lastLit = on;
    const bool dim = dimmed || ! isEnabled();
    auto keyArea = getLocalBounds().toFloat();
    if (lamp) keyArea.removeFromRight (kLampW);
    if (dim) g.beginTransparencyLayer (0.5f);
    fam::paintPlateKey (g, keyArea, getButtonText(), on, over && isEnabled(), down, size, track, kMargin);
    if (lamp)
        fam::redLed (g, (float) getWidth() - kLampW * 0.5f - 2.0f, keyArea.getCentreY(), 6.0f, on);
    if (dim) g.endTransparencyLayer();
}

//==============================================================================
void NoteWindow::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    if (dim) g.beginTransparencyLayer (0.5f);
    fam::paintValueWindow (g, b, {}, false, false);
    const auto in = b.reduced (3.0f);
    const bool autoOn = isAuto && isAuto();
    silk (g, autoOn ? kNoteTextAuto : "NOTE", Theme::legendEm (6.6f), in.getX() + 9.0f, in.getY() + 8.0f, 'l', 1.2f, autoOn ? Theme::amberLight : Theme::muted);
    silk (g, textFn(), Theme::monoEm (17.0f), in.getCentreX() + 8.0f, in.getCentreY() + 1.0f, 'm', 0.4f, autoOn ? Theme::amberLight : Theme::label);
    juce::Path l, r;   // click = next, Shift-click = previous
    const float ty = in.getCentreY(), tx = in.getRight() - 12.0f;
    l.addTriangle (tx - 11.0f, ty, tx - 6.0f, ty - 4.0f, tx - 6.0f, ty + 4.0f);
    r.addTriangle (tx + 1.0f, ty - 4.0f, tx + 1.0f, ty + 4.0f, tx + 6.0f, ty);
    g.setColour (Theme::gold.withAlpha (0.8f));
    g.fillPath (l);
    g.fillPath (r);
    if (dim) g.endTransparencyLayer();
}

void NoteWindow::mouseUp (const juce::MouseEvent& e)
{
    if (e.mouseWasDraggedSinceMouseDown()) return;
    if (stepFn) stepFn (e.mods.isShiftDown() || e.mods.isPopupMenu() ? -1 : 1);
}

void NoteWindow::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    if (stepFn && std::abs (w.deltaY) > 0.0f) stepFn (w.deltaY > 0.0f ? 1 : -1);
}

} // namespace sedaui

//==============================================================================
namespace
{
constexpr float kScaleMinDb = -36.0f, kScaleMaxDb = 0.0f, kSpanDeg = 52.0f;
float echoAng (float db) { return -kSpanDeg + 2.0f * kSpanDeg * juce::jlimit (-0.02f, 1.04f, (db - kScaleMinDb) / (kScaleMaxDb - kScaleMinDb)); }
const juce::String kDot (juce::CharPointer_UTF8 ("\xc2\xb7"));
}

namespace sedaui
{
namespace
{
// world coordinates of the meter (the window paints in them after one translation); the needles pivot at (cx, py)
struct MeterGeo
{
    float cx, py, R, rd;
    MeterGeo()
    {
        const juce::Rectangle<float> box (EchoWindow::kX0 + 14, EchoWindow::kY0 + 38, 402, 196);
        cx = box.getCentreX(); py = box.getBottom() - box.getHeight() * 0.06f; R = box.getHeight() * 0.76f; rd = R * 0.56f;
    }
    juce::Rectangle<float> echoNeedle (float db) const
    {
        const auto tip = fam::polar (cx, py, R + 6.0f, echoAng (db));
        return juce::Rectangle<float> (juce::Point<float> (cx, py), tip).expanded (4.0f).getUnion ({ cx - 8.0f, py - 8.0f, 16.0f, 16.0f });
    }
    juce::Rectangle<float> duckNeedle (float duckDb, bool listen) const
    {
        const float da = -kSpanDeg + 2.0f * kSpanDeg * juce::jlimit (-0.02f, 1.04f, (listen ? juce::jlimit (0.0f, EchoWindow::kMaxDb, -duckDb) : 0.0f) / EchoWindow::kMaxDb);
        const auto tip = fam::polar (cx, py, rd + 4.0f, da);
        return juce::Rectangle<float> (juce::Point<float> (cx, py), tip).expanded (3.0f);
    }
    juce::Rectangle<float> peakLed() const { return { EchoWindow::kX0 + 14 + 402 - 66.0f, EchoWindow::kY0 + 38 + 18.0f - 8.0f, 60.0f, 16.0f }; }
    juce::Rectangle<float> lamp() const { return { EchoWindow::kX0 + 448 + 4.0f, EchoWindow::kY0 + 172.0f - 8.0f, 150.0f, 16.0f }; }
    juce::Rectangle<float> runLed() const { return { EchoWindow::kX0 + 448 - 4.0f, EchoWindow::kY0 + 207.0f - 13.0f, 34.0f, 26.0f }; }   // the RUNAWAY LED and its glow
};
}

void EchoWindow::set (const DisplayState& s)
{
    const auto old = st;
    st = s;
    const bool big = old.listen != s.listen || old.sync != s.sync || old.noteAuto != s.noteAuto || old.guard != s.guard || old.note != s.note
                  || old.program != s.program || old.seg != s.seg || old.segUnit != s.segUnit || old.bbdLine != s.bbdLine;
    if (big) { repaint(); return; }
    static const MeterGeo geo;
    const auto off = juce::Point<float> (kPad - kX0, kPad - kY0);   // world -> this component
    auto region = [&] (juce::Rectangle<float> r) { repaint (r.translated (off.x, off.y).getSmallestIntegerContainer()); };
    if (old.throwLamp != s.throwLamp) region (geo.lamp());
    if (old.runLamp != s.runLamp) region (geo.runLed().getUnion ({ kX0 + 448 + 20.0f, kY0 + 197.0f, 190.0f, 20.0f }));
    if (old.wetDb != s.wetDb) region (geo.echoNeedle (old.wetDb).getUnion (geo.echoNeedle (s.wetDb)));
    if (old.duckDb != s.duckDb) region (geo.duckNeedle (old.duckDb, old.listen).getUnion (geo.duckNeedle (s.duckDb, s.listen)));
    if (old.peak != s.peak) region (geo.peakLed());
}

void EchoWindow::paint (juce::Graphics& g)
{
    g.addTransform (juce::AffineTransform::translation (kPad - kX0, kPad - kY0));
    const float x0 = kX0, y0 = kY0, x1 = kX1, y1 = kY1;
    const auto clipR = g.getClipBounds().toFloat();   // a small repaint (a needle, a lamp) draws only what touches it: the same pixels, a fraction of the work
    auto vis = [&] (juce::Rectangle<float> r) { return clipR.intersects (r); };
    const juce::Rectangle<float> win (x0, y0, x1 - x0, y1 - y0);
    const auto ink = Theme::panel, ink2 = Theme::labelInk2;

    // the recessed case: shadow, dark bevel ring, then the cream face
    fam::softShadow (g, win.expanded (6.0f), 4.0f, 0.6f, { 1.0f, 4.0f }, 10.0f, false);
    g.setGradientFill (fam::grad (win.expanded (7.0f), rgb (30, 33, 32), rgb (4, 5, 5), 90.0f));
    g.fillRoundedRectangle (win.expanded (7.0f), 10.0f);
    g.setColour (rgb (255, 255, 255, 22));
    g.drawRoundedRectangle (win.expanded (6.5f), 10.0f, 0.8f);
    g.setGradientFill (fam::grad (win, Theme::label, Theme::labelDeep, 90.0f));
    g.fillRoundedRectangle (win, 4.0f);
    g.setColour (rgb (0, 0, 0, 60));   // inner shadow along the top and left edge of the recess
    g.drawLine (x0 + 2.0f, y0 + 0.8f, x1 - 2.0f, y0 + 0.8f, 1.6f);
    g.drawLine (x0 + 0.8f, y0 + 2.0f, x0 + 0.8f, y1 - 2.0f, 1.4f);

    if (vis ({ x0, y0, x1 - x0, 34.0f }))
    {
        silk (g, "ECHO " + kDot + " dB", Theme::legendEm (8.0f), x0 + 16, y0 + 16, 'l', 1.8f, ink);
        silk (g, "LISTEN " + kDot + " PROGRAM, DUCK AND THROW", Theme::legendEm (8.0f), x0 + 448, y0 + 16, 'l', 1.8f, ink);
        silk (g, "SUBVERSELAB", Theme::legendEm (7.5f), x1 - 16, y0 + 16, 'r', 1.8f, ink);
        g.setColour (Theme::gold);
        g.fillRect (x0 + 12, y0 + 27, x1 - x0 - 24, 3.0f);
    }

    // ---- the meter: wet level after the duck, -36 .. 0 dB, with the duck depth (0 .. -12 dB) on a red inner scale
    const juce::Rectangle<float> box (x0 + 14, y0 + 38, 402, 196);
    const float bw = box.getWidth(), bh = box.getHeight();
    g.setGradientFill (fam::grad (box.expanded (3.0f), rgb (30, 33, 32), rgb (4, 5, 5), 90.0f));
    g.fillRoundedRectangle (box.expanded (3.0f), 4.0f);
    g.setGradientFill (fam::grad (box, rgb (247, 241, 224), rgb (224, 214, 188), 90.0f));
    g.fillRoundedRectangle (box, 2.0f);
    const float cx = box.getCentreX(), py = box.getBottom() - bh * 0.06f, R = bh * 0.76f;
    const auto goldInk = rgb (128, 97, 42), red = rgb (190, 55, 38), dark = rgb (32, 32, 30);
    g.saveState();
    g.reduceClipRegion (box.toNearestInt());
    {
        juce::Path arc;
        arc.addCentredArc (cx, py, R, R, 0.0f, echoAng (kScaleMinDb) * fam::kPi / 180.0f, echoAng (kScaleMaxDb) * fam::kPi / 180.0f, true);
        g.setColour (goldInk);
        g.strokePath (arc, juce::PathStrokeType (1.3f));
        juce::Path band;
        band.addCentredArc (cx, py, R + 2.0f, R + 2.0f, 0.0f, echoAng (-6.0f) * fam::kPi / 180.0f, echoAng (kScaleMaxDb) * fam::kPi / 180.0f, true);
        g.setColour (Theme::gold);
        g.strokePath (band, juce::PathStrokeType (4.0f));
    }
    for (int db = -36; db <= 0; db += 3)
    {
        const float an = echoAng ((float) db);
        const bool major = (db % 6) == 0;
        g.setColour (goldInk);
        g.drawLine ({ fam::polar (cx, py, R, an), fam::polar (cx, py, R + (major ? 9.0f : 5.0f), an) }, major ? 1.3f : 0.9f);
        if (major && vis (juce::Rectangle<float> (fam::polar (cx, py, R + 20.0f, an), fam::polar (cx, py, R + 20.0f, an)).expanded (14.0f)))
        {
            const auto t = fam::polar (cx, py, R + 20.0f, an);
            silk (g, db == 0 ? juce::String ("0") : juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92")) + juce::String (-db), Theme::legendEm (7.4f), t.x, t.y, 'm', 0.0f, db >= -6 ? rgb (120, 70, 30) : goldInk);
        }
    }
    if (vis ({ box.getX() + bw * 0.16f - 40.0f, py - R * 0.30f - 14.0f, 80.0f, 40.0f }))
    {
        silk (g, "ECHO", Theme::legendEm (17.0f), box.getX() + bw * 0.16f, py - R * 0.30f, 'm', 2.0f, dark);
        silk (g, "dB", Theme::fineEm (8.5f), box.getX() + bw * 0.16f, py - R * 0.30f + 17.0f, 'm', 1.0f, ink2);
    }
    if (vis ({ box.getRight() - 68.0f, box.getY() + 8.0f, 64.0f, 20.0f }))
    {
        fam::redLed (g, box.getRight() - 62.0f, box.getY() + 18.0f, 3.0f, st.peak);
        silk (g, "PEAK", Theme::legendEm (7.0f), box.getRight() - 54.0f, box.getY() + 18.5f, 'l', 0.8f, st.peak ? ink : ink2);
    }

    {   // the duck: a red inner scale 0 .. 12 dB on the same pivot, read by a thin red second needle (the analog way, like a GR needle)
        const float rd = R * 0.56f;
        auto duckAng = [] (float dDb) { return -kSpanDeg + 2.0f * kSpanDeg * juce::jlimit (-0.02f, 1.04f, dDb / EchoWindow::kMaxDb); };
        juce::Path darc;
        darc.addCentredArc (cx, py, rd, rd, 0.0f, duckAng (0.0f) * fam::kPi / 180.0f, duckAng (EchoWindow::kMaxDb) * fam::kPi / 180.0f, true);
        g.setColour (red);
        g.strokePath (darc, juce::PathStrokeType (1.1f));
        for (int d = 0; d <= 12; d += 3)
        {
            const float an = duckAng ((float) d);
            g.drawLine ({ fam::polar (cx, py, rd, an), fam::polar (cx, py, rd + 5.0f, an) }, 1.0f);
            const auto t = fam::polar (cx, py, rd + 13.0f, an);
            if (vis (juce::Rectangle<float> (t, t).expanded (10.0f))) silk (g, juce::String (d), Theme::legendEm (6.6f), t.x, t.y, 'm', 0.0f, red);
        }
        if (vis ({ cx - 40.0f, py - rd - 44.0f, 80.0f, 20.0f })) silk (g, "DUCK dB", Theme::legendEm (6.6f), cx, py - rd - 34.0f, 'm', 1.4f, red);
        const float da = duckAng (st.listen ? juce::jlimit (0.0f, EchoWindow::kMaxDb, -st.duckDb) : 0.0f);
        g.setColour (red);
        g.drawLine ({ Pt (cx, py), fam::polar (cx, py, rd + 4.0f, da) }, 1.2f);
    }
    const float na = echoAng (st.wetDb);
    g.setColour (rgb (0, 0, 0, 40));
    g.drawLine ({ Pt (cx + 1.5f, py + 2.0f), fam::polar (cx + 1.5f, py + 2.0f, R + 6.0f, na) }, 1.8f);
    g.setColour (rgb (20, 20, 20));
    g.drawLine ({ Pt (cx, py), fam::polar (cx, py, R + 6.0f, na) }, 1.4f);
    g.setColour (rgb (30, 32, 31));
    g.fillEllipse (cx - 4.5f, py - 4.5f, 9.0f, 9.0f);
    const float hy = box.getBottom() - bh * 0.13f;
    g.setColour (rgb (22, 24, 23));
    g.fillRect (box.getX(), hy, bw, box.getBottom() - hy);
    g.setColour (rgb (0, 0, 0, 220));
    g.drawLine (box.getX(), hy, box.getRight(), hy, 0.8f);
    const float sy = (hy + box.getBottom()) * 0.5f + 0.5f;
    if (vis ({ box.getX(), hy, bw, box.getBottom() - hy }))
    {
        silk (g, "WET LEVEL", Theme::legendEm (7.5f), cx, sy, 'm', 2.2f, Theme::ink);
        silk (g, juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92")) + "36", Theme::legendEm (7.5f), box.getX() + 16.0f, sy, 'm', 0.0f, Theme::muted);
        silk (g, "0", Theme::legendEm (7.5f), box.getRight() - 14.0f, sy, 'm', 0.0f, Theme::muted);
    }
    juce::Path sheen;   // the glass
    sheen.addQuadrilateral (box.getX(), box.getY(), box.getX() + bw * 0.42f, box.getY(), box.getX() + bw * 0.18f, hy, box.getX(), hy);
    g.setColour (rgb (255, 255, 255, 20));
    g.fillPath (sheen);
    g.restoreState();

    // ---- right half: the LISTEN plate (printed), the red LED display (program name and repeat time), printed captions, step keys
    g.setColour (Theme::gold.withAlpha (200.0f / 255.0f));
    g.fillRect (x0 + 430, y0 + 38, 1.2f, 196.0f);
    const float rx = x0 + 448, rw = 442.0f;
    const auto warn = rgb (190, 55, 38);
    if (vis ({ rx, y0 + 38.0f, rw, 22.0f }))
    {
        silk (g, "LISTEN", Theme::legendEm (8.0f), rx + 6.0f, y0 + 46.0f, 'l', 2.4f, ink);
        if (! st.listen) silk (g, "NEEDS LISTEN", Theme::legendEm (7.4f), rx + rw - 6.0f, y0 + 46.0f, 'r', 1.8f, warn);
        g.setColour (Theme::gold.withAlpha (180.0f / 255.0f));
        g.fillRect (rx, y0 + 54.0f, rw, 1.0f);
    }
    if (vis ({ rx - 4.0f, y0 + 56.0f, rw + 8.0f, 110.0f }))
    {   // the LED display sits right under its LISTEN plate: the program name in dot-matrix on the left, the repeat time in 7 segments on the right
        const juce::Rectangle<float> gl (rx + 8.0f, y0 + 72.0f, rw - 16.0f, 62.0f);
        // the layer between plate and glass: a dark metal bezel mounted on the plate (drop shadow, bevelled frame, lit top edge), the glass recessed inside it
        const auto bez = gl.expanded (7.0f);
        fam::softShadow (g, bez, 3.0f, 0.55f, { 0.0f, 2.5f }, 6.0f, false);
        g.setGradientFill (fam::grad (bez, rgb (58, 62, 60), rgb (14, 16, 15), 90.0f));
        g.fillRoundedRectangle (bez, 5.0f);
        g.setColour (rgb (255, 255, 255, 46));
        g.drawLine (bez.getX() + 5.0f, bez.getY() + 0.8f, bez.getRight() - 5.0f, bez.getY() + 0.8f, 1.0f);
        g.setColour (rgb (0, 0, 0, 160));
        g.drawRoundedRectangle (bez.reduced (0.5f), 5.0f, 1.0f);
        g.setGradientFill (fam::grad (gl.expanded (2.5f), rgb (6, 6, 6), rgb (34, 36, 35), 90.0f));
        g.fillRoundedRectangle (gl.expanded (2.5f), 3.0f);
        g.setColour (Theme::redWindow);
        g.fillRoundedRectangle (gl, 2.0f);
        g.setColour (rgb (0, 0, 0, 170));   // inner shadow under the bezel's top lip
        g.drawLine (gl.getX() + 2.0f, gl.getY() + 1.0f, gl.getRight() - 2.0f, gl.getY() + 1.0f, 2.0f);
        const char* names[5] = { "P0 MANUAL", "P1 VOCAL", "P2 DRUMS", "P3 BASS", "P4 KEYS" };
        if (! st.listen) g.beginTransparencyLayer (0.40f);
        matrixText (g, names[juce::jlimit (0, 4, st.program)], gl.getX() + 10.0f, gl.getY() + 14.0f, 4.9f, 9, Theme::ledRed, rgb (58, 16, 12));
        if (! st.listen) g.endTransparencyLayer();
        g.setColour (rgb (0, 0, 0, 140));
        g.drawLine (gl.getX() + 290.0f, gl.getY() + 8.0f, gl.getX() + 290.0f, gl.getBottom() - 8.0f, 1.0f);
        const float tcx = gl.getX() + 290.0f + (rw - 290.0f) * 0.5f;
        segText (g, st.seg, tcx, gl.getY() + 11.0f, 40.0f, Theme::ledRed, Theme::ledRedOff);
        silk (g, "PROGRAM", Theme::legendEm (7.0f), gl.getX() + 12.0f + 27.0f * 4.9f, gl.getBottom() + 19.0f, 'm', 1.6f, ink);
        silk (g, "REPEAT " + kDot + " " + st.segUnit, Theme::legendEm (7.0f), tcx, gl.getBottom() + 19.0f, 'm', 1.6f, ink);
    }
    if (vis ({ rx, y0 + 160.0f, rw, 24.0f }))
    {   // printed lamp labels with their red LED dots, under the display
        auto lamp = [&] (float x, const char* t, float amt)
        {
            const float ly = y0 + 172.0f;
            fam::redLed (g, x + 5.0f, ly, 3.4f, false);
            if (amt > 0.02f)
            {
                g.beginTransparencyLayer (juce::jlimit (0.0f, 1.0f, 0.35f + 0.65f * amt));
                fam::redLed (g, x + 5.0f, ly, 3.4f, true);
                g.endTransparencyLayer();
            }
            silk (g, t, Theme::legendEm (7.8f), x + 16.0f, ly + 0.5f, 'l', 1.4f, amt > 0.02f ? ink : ink2);
        };
        lamp (rx + 6.0f, "THROW", st.listen ? st.throwLamp : 0.0f);
        lamp (rx + 150.0f, "GUARD", st.listen && st.guard ? 1.0f : 0.0f);
        lamp (rx + 300.0f, "AUTO NOTE", st.listen && st.noteAuto ? 1.0f : 0.0f);
    }
    if (vis ({ rx - 6.0f, y0 + 192.0f, 270.0f, 30.0f }))
    {   // the RUNAWAY LED (17 Oct 2026): red, a little larger and brighter than THROW / GUARD / AUTO NOTE, lit while the loop gain is over 1.0 (FEEDBACK over 100 % or RUNAWAY)
        const float lx = rx + 11.0f, ly = y0 + 207.0f;
        fam::redLed (g, lx, ly, 3.9f, false);
        if (st.runLamp > 0.02f)
        {
            fam::glow (g, { lx, ly }, 12.0f, Theme::ledRed, 0.30f * st.runLamp);
            g.beginTransparencyLayer (juce::jlimit (0.0f, 1.0f, st.runLamp));
            fam::redLed (g, lx, ly, 3.9f, true);
            g.endTransparencyLayer();
        }
        const auto f = Theme::legendEm (7.8f);
        const bool on = st.runLamp > 0.02f;
        silk (g, "RUNAWAY", f, lx + 11.0f, ly + 0.5f, 'l', 1.4f, on ? ink : ink2);
        silk (g, kDot + " LOOP OVER 100 %", Theme::legendEm (7.0f), lx + 11.0f + fam::textWidth ("RUNAWAY", f, 1.4f) + 6.0f, ly + 0.5f, 'l', 1.2f, ink2);
    }
    if (vis ({ x0 + 600.0f, y0 + 194.0f, 170.0f, 22.0f })) silk (g, "PROGRAM STEP", Theme::legendEm (7.0f), x0 + 764.0f, y0 + 206.0f, 'r', 1.6f, ink2);
    if (vis ({ x0, y1 - 26.0f, x1 - x0, 26.0f }))
    {
        silk (g, "METER: WET LEVEL AFTER THE DUCK  " + kDot + "  RED NEEDLE: DUCK DEPTH", Theme::fineEm (7.0f), x0 + 16, y1 - 14, 'l', 0.8f, ink2);
        silk (g, st.bbdLine + "  " + kDot + "  NEEDLE 300 MS", Theme::fineEm (7.0f), x1 - 16, y1 - 14, 'r', 0.8f, ink2);
    }
}

//==============================================================================
void StepKey::paintButton (juce::Graphics& g, bool over, bool down)
{
    const bool dim = dimmed || ! isEnabled();
    if (dim) g.beginTransparencyLayer (0.5f);
    fam::paintPlateKey (g, getLocalBounds().toFloat(), {}, false, over, down, 8.0f, 1.0f, PlateKey::kMargin);
    const auto c = getLocalBounds().toFloat().reduced (PlateKey::kMargin).getCentre().translated (0.0f, down ? 1.0f : 0.0f);
    juce::Path t;
    if (dir < 0) t.addTriangle (c.x + 5.0f, c.y - 6.0f, c.x + 5.0f, c.y + 6.0f, c.x - 6.0f, c.y);
    else         t.addTriangle (c.x - 5.0f, c.y - 6.0f, c.x - 5.0f, c.y + 6.0f, c.x + 6.0f, c.y);
    g.setColour (Theme::panel);
    g.fillPath (t);
    if (dim) g.endTransparencyLayer();
}

//==============================================================================
void SizeChip::paint (juce::Graphics& g)
{
    fam::paintPlateKey (g, getLocalBounds().toFloat(), "SIZE " + juce::String (ed.scalePercent()) + " %", false, isMouseOver(), false, 8.2f, 1.4f, 9.0f);
}

void SizeChip::mouseUp (const juce::MouseEvent& e)
{
    if (e.mouseWasDraggedSinceMouseDown()) return;
    juce::PopupMenu m;
    for (int p : { 60, 75, 100 }) m.addItem (p, juce::String (p) + " %", true, p == ed.scalePercent());
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this), [this] (int r) { if (r > 0) ed.setScalePercent (r); });
}
} // namespace sedaui

//==============================================================================
using namespace sedaui;

namespace
{
/** RUNAWAY / CHAOS corner (17 Oct 2026, owner's concept docs/ui/concept/seda_runaway_chaos_concept.png): a FADED diagonal band of three stripes, cream / gold / red,
    bottom-left to top-right behind the two knobs. No box, no frame, no screws: the stripes are feathered across their width and fade out at both ends into the plate.
    Drawn once at 2x into an image (kRect in panel units, kOpacity baked in) and composited into the shared plate image (PlateCache), so a paint costs nothing extra. */
struct StripeCache
{
    static constexpr float kS = 2.0f, kOpacity = 0.30f;
    inline static const juce::Rectangle<float> kRect { 50.0f, 190.0f, 430.0f, 140.0f };
    juce::Image img;
    StripeCache()
    {
        img = juce::Image (juce::Image::ARGB, (int) (kRect.getWidth() * kS), (int) (kRect.getHeight() * kS), true, juce::SoftwareImageType());
        const Pt a (64.0f, 320.0f), b (456.0f, 212.0f);                         // the band's centre line, panel units
        const auto dir = (b - a) / a.getDistanceFrom (b);
        {
        juce::Graphics g (img);   // closed before the pixels are touched below
        g.addTransform (juce::AffineTransform::translation (-kRect.getX(), -kRect.getY()).scaled (kS));
        const auto nrm = Pt (dir.y, -dir.x);  // points up-left
        const float w = 5.6f, gap = 4.0f, feather = 1.3f;
        const juce::Colour cols[3] { rgb (0xF2, 0xEC, 0xDE), rgb (0xC5, 0xA0, 0x59), rgb (0xC4, 0x3A, 0x2C) };   // cream, gold, red from the top
        for (int k = 0; k < 3; ++k)
        {
            const Pt off = nrm * ((1.0f - (float) k) * (w + gap));
            juce::Path line; line.startNewSubPath (a + off - dir * 20.0f); line.lineTo (b + off + dir * 20.0f);
            for (int j = 0; j < 6; ++j)   // six strokes, each a little narrower: the edges ramp up over `feather` instead of a hard line
            {
                g.setColour (cols[k].withAlpha (0.40f));   // six layers of 0.4: 0.95 in the middle of a stripe, 0.4 at its feathered edge
                g.strokePath (line, juce::PathStrokeType (w + 2.0f * feather * (1.0f - (float) j / 5.0f) - feather, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
            }
        }
        }
        // fade along the band: 0 at both ends, full over the middle (smoothstep over the first and last 28 %)
        juce::Image::BitmapData bd (img, juce::Image::BitmapData::readWrite);
        const float len = a.getDistanceFrom (b);
        for (int y = 0; y < bd.height; ++y)
            for (int x = 0; x < bd.width; ++x)
            {
                const Pt q (kRect.getX() + ((float) x + 0.5f) / kS, kRect.getY() + ((float) y + 0.5f) / kS);
                const float t = (q - a).getDotProduct (dir) / len, e = juce::jlimit (0.0f, 1.0f, juce::jmin (t, 1.0f - t) / 0.28f);
                auto* px = (juce::PixelARGB*) bd.getPixelPointer (x, y);
                px->multiplyAlpha (kOpacity * e * e * (3.0f - 2.0f * e));   // the band's opacity is baked in (no opacity layer at paint time)
            }
    }
};

/** The plate is built once (it is large) and shared by every open editor: the family's brushed plate plus a faint vertical grain and a soft vignette. */
struct PlateCache
{
    juce::Image img;
    PlateCache()
    {
        img = fam::brushedPlate (SedaEditor::kW, SedaEditor::kH, rgb (26, 70, 66), rgb (9, 42, 39), 77);
        juce::Graphics g (img);
        const int w = img.getWidth(), h = img.getHeight();
        juce::Random rng (311);
        for (int i = 0; i < 1500; ++i)   // vertical grain
        {
            const int x = rng.nextInt (w);
            const float y = rng.nextFloat() * (float) h, len = 40.0f + rng.nextFloat() * 260.0f;
            g.setColour ((rng.nextBool() ? juce::Colours::white : juce::Colours::black).withAlpha (0.010f + rng.nextFloat() * 0.014f));
            g.drawVerticalLine (x, y, y + len);
        }
        g.setGradientFill (juce::ColourGradient (rgb (0, 0, 0, 0), (float) w * 0.5f, (float) h * 0.5f, rgb (0, 0, 0, 70), 0.0f, 0.0f, true));   // vignette
        g.fillAll();
        {   // the faded RUNAWAY / CHAOS stripe band, part of the plate (every legend, the seam and the knobs are drawn over it)
            const StripeCache sc;
            g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
            g.drawImage (sc.img, StripeCache::kRect * ((float) w / (float) SedaEditor::kW), juce::RectanglePlacement::stretchToFit);   // the plate is drawn at 2x: panel units x 2 (= kS, no resampling)
        }
    }
};

void engravedBracket (juce::Graphics& g, float x0, float x1, float y, const juce::String& title)
{
    g.saveState();
    g.addTransform (juce::AffineTransform::translation (0.0f, 1.3f));
    const auto f = Theme::legendEm (10.0f);
    const float cx = (x0 + x1) * 0.5f, tw = fam::textWidth (title, f, 3.0f), gap = 10.0f;
    g.setColour (rgb (0, 0, 0, 150));
    g.drawLine (x0, y, cx - tw * 0.5f - gap, y, 1.1f);
    g.drawLine (cx + tw * 0.5f + gap, y, x1, y, 1.1f);
    g.drawLine (x0, y, x0, y + 7.0f, 1.1f);
    g.drawLine (x1, y, x1, y + 7.0f, 1.1f);
    g.restoreState();
    bracket (g, x0, x1, y, title);
}

/** A recessed sub-panel of the bottom band: a thin dark inset line, a faint light line under it, a whisper of shade inside. Not a box. */
void insetPanel (juce::Graphics& g, juce::Rectangle<float> r)
{
    g.setColour (rgb (0, 0, 0, 34));
    g.fillRoundedRectangle (r, 7.0f);
    g.setColour (rgb (0, 0, 0, 120));
    g.drawRoundedRectangle (r, 7.0f, 1.0f);
    g.setColour (rgb (255, 255, 255, 26));
    g.drawRoundedRectangle (r.translated (0.0f, 1.2f).expanded (0.4f), 7.0f, 0.8f);
}
}

SedaEditor::SedaEditor (SedaAudioProcessor& p) : AudioProcessorEditor (p), proc (p)
#if SVL_LICENSING
    , licenseOverlay (p.license)
#endif
{
    setLookAndFeel (&lnf);
    mark = juce::ImageCache::getFromMemory (BinaryData::mark_cream_png, BinaryData::mark_cream_pngSize);
    wordmark = juce::ImageCache::getFromMemory (BinaryData::wordmark_cream_png, BinaryData::wordmark_cream_pngSize);
    { juce::SharedResourcePointer<PlateCache> pc; plate = pc->img; }

    const juce::String minus (juce::CharPointer_UTF8 ("\xe2\x88\x92"));

    // ---------------------------------------------------------------- TIME
    titles.push_back ({ "TIME", 72.0f, 346.0f, 348.0f });
    insets.push_back ({ 66.0f, 338.0f, 286.0f, 256.0f });
    {
        auto& k = addKnob ("time", "TIME", 140.0f, 450.0f, 41.0f, { { -135.0f, "20" }, { 135.0f, "1200" } }, [] { return juce::String ("SYNC ON"); });
        (void) k;
    }
    addKey ("SYNC", "sync", { 248.0f, 378.0f, 80.0f, 32.0f }, [this] { return effSync(); },
            [this] { svl::ParamUndo::Group one (proc.undo, "SYNC"); setGesture ("sync", onOf ("sync") ? 0.0f : 1.0f); }, 8.4f, 1.4f);
    addKey ("AUTO", "noteAuto", { 248.0f, 416.0f, 80.0f, 32.0f }, [this] { return effAuto(); },
            [this] { svl::ParamUndo::Group one (proc.undo, "AUTO"); setGesture ("noteAuto", onOf ("noteAuto") ? 0.0f : 1.0f); }, 8.4f, 1.4f);
    // NOTE is a stepped family knob with its value window (TUTKAL's NOTE), not a click window
    addKnob ("note", "NOTE", 288.0f, 500.0f, 25.0f, { { -135.0f, "1/32" }, { 135.0f, "1/1d" } }, [] { return juce::String ("SYNC OFF"); });

    // ---------------------------------------------------------------- RUNAWAY / CHAOS (17 Oct 2026): the free corner under the name, over the faded stripe band.
    // No value window (no room above the TIME / LOOP band): the label under the knob prints the value while the knob is held; a double-click types nothing in.
    addKnob ("runaway", "RUNAWAY", 145.0f, 268.0f, 25.0f, { { -135.0f, "0" }, { 135.0f, "100" } }, nullptr, false);
    addKnob ("chaos", "CHAOS", 365.0f, 268.0f, 25.0f, { { -135.0f, "0" }, { 135.0f, "100" } }, nullptr, false);

    // ---------------------------------------------------------------- LOOP
    titles.push_back ({ "LOOP", 366.0f, 668.0f, 348.0f });
    insets.push_back ({ 360.0f, 338.0f, 314.0f, 256.0f });
    addKnob ("feedback", "FEEDBACK", 434.0f, 450.0f, 41.0f, { { -135.0f, "0" }, { 135.0f, "110" } });
    addKnob ("tone", "TONE", 556.0f, 393.0f, 25.0f, { { -135.0f, minus }, { 135.0f, "+" } });
    addKnob ("comp", "COMPANDER", 556.0f, 504.0f, 25.0f, { { -135.0f, "0" }, { 135.0f, "100" } });
    addToggle ("age", 628.0f, 450.0f, 0.85f, "NEW", "OLD", "AGE", "", "", [this] { return choiceOf ("age") == 0; },
               [this] { setGesture ("age", choiceOf ("age") == 0 ? 1.0f : 0.0f); });

    // ---------------------------------------------------------------- MOD
    titles.push_back ({ "MOD", 688.0f, 866.0f, 348.0f });
    insets.push_back ({ 682.0f, 338.0f, 190.0f, 256.0f });
    addKnob ("rate", "RATE", 738.0f, 393.0f, 25.0f, { { -135.0f, "0.05" }, { 135.0f, "8" } });
    addKnob ("depth", "DEPTH", 738.0f, 504.0f, 25.0f, { { -135.0f, "0" }, { 135.0f, "100" } });
    legends.push_back ({ "SHAPE", 824.0f, 408.0f, 8.4f, 'm', Theme::ink, 2.2f });
    addChoiceKey ("SINE", "shape", 0, { 798.0f, 424.0f, 52.0f, 28.0f }, 7.4f, 1.0f);
    addChoiceKey ("TRI", "shape", 1, { 798.0f, 464.0f, 52.0f, 28.0f }, 7.4f, 1.0f);

    // ---------------------------------------------------------------- STEREO
    titles.push_back ({ "STEREO", 886.0f, 1100.0f, 348.0f });
    insets.push_back ({ 880.0f, 338.0f, 226.0f, 256.0f });
    addChoiceKey ("MONO", "stereo", 0, { 894.0f, 396.0f, 96.0f, 32.0f }, 8.0f, 1.2f);
    addChoiceKey ("WIDE", "stereo", 1, { 894.0f, 438.0f, 96.0f, 32.0f }, 8.0f, 1.2f);
    addChoiceKey ("PING-PONG", "stereo", 2, { 894.0f, 480.0f, 96.0f, 32.0f }, 7.6f, 1.0f);
    addKnob ("width", "WIDTH", 1050.0f, 393.0f, 25.0f, { { -135.0f, "0" }, { 135.0f, "20" } }, [this] { return juce::String (monoBus() ? "MONO BUS" : "MONO"); });
    addKnob ("wetHp", "WET HP", 1050.0f, 504.0f, 25.0f, { { -135.0f, "OFF" }, { 135.0f, "1k" } }, [] { return juce::String ("LISTEN OFF"); });

    // ---------------------------------------------------------------- LISTEN
    titles.push_back ({ "LISTEN", 1120.0f, 1240.0f, 348.0f });
    insets.push_back ({ 1114.0f, 338.0f, 132.0f, 256.0f });
    addKnob ("duck", "DUCK", 1180.0f, 393.0f, 25.0f, { { -135.0f, "0" }, { 135.0f, "100" } }, [] { return juce::String ("LISTEN OFF"); });
    addKnob ("throw", "THROW", 1180.0f, 504.0f, 25.0f, { { -135.0f, "0" }, { 135.0f, "12" } }, [] { return juce::String ("LISTEN OFF"); });

    // ---------------------------------------------------------------- OUT
    titles.push_back ({ "OUT", 1260.0f, 1536.0f, 348.0f });
    insets.push_back ({ 1254.0f, 338.0f, 288.0f, 256.0f });
    addKnob ("mix", "MIX", 1340.0f, 450.0f, 41.0f, { { -135.0f, "DRY" }, { 135.0f, "WET" } });

    // ---------------------------------------------------------------- right column of the top band: LISTEN and BYPASS bat toggles, SIZE
    addToggle ("listen", 1478.0f, 70.0f, 1.0f, "ON", "OFF", "LISTEN", "DUCKS UNDER THE PHRASE,", "THROWS AT ITS END", [this] { return onOf ("listen"); },
               [this] { setGesture ("listen", onOf ("listen") ? 0.0f : 1.0f); });
    addToggle ("bypass", 1478.0f, 190.0f, 1.0f, "ON", "OFF", "IN", "OFF = BYPASSED", "", [this] { return ! onOf ("bypass"); },
               [this] { setGesture ("bypass", onOf ("bypass") ? 0.0f : 1.0f); });
    sizeChip = std::make_unique<SizeChip> (*this);
    sizeChip->setBounds (juce::Rectangle<float> (1434.0f, 268.0f, 88.0f, 28.0f).expanded (9.0f).toNearestInt());
    addAndMakeVisible (*sizeChip);

    // ---------------------------------------------------------------- the two displays
    display.setBounds (juce::Rectangle<float> (EchoWindow::kX0 - EchoWindow::kPad, EchoWindow::kY0 - EchoWindow::kPad, EchoWindow::kX1 - EchoWindow::kX0 + 2 * EchoWindow::kPad,
                                               EchoWindow::kY1 - EchoWindow::kY0 + 2 * EchoWindow::kPad + 4.0f).toNearestInt());
    addAndMakeVisible (display);
    programLn = std::make_unique<ProgramLine> ([this] (int d) { stepSource (d); });
    programLn->setBounds (946, 134, 280, 72);
    addAndMakeVisible (*programLn);
    stepPrev = std::make_unique<StepKey> (-1, [this] (int d) { stepSource (d); });
    stepNext = std::make_unique<StepKey> (+1, [this] (int d) { stepSource (d); });
    stepPrev->setBounds (juce::Rectangle<float> (1266.0f, 236.0f, 52.0f, 20.0f).expanded (PlateKey::kMargin).toNearestInt());
    stepNext->setBounds (juce::Rectangle<float> (1324.0f, 236.0f, 52.0f, 20.0f).expanded (PlateKey::kMargin).toNearestInt());
    addAndMakeVisible (*stepPrev); addAndMakeVisible (*stepNext);

    addMouseListener (this, true);
    setWantsKeyboardFocus (true);
    setResizable (false, false);
    setSize (kW, kH);
#if SVL_LICENSING
    licenseButton.setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    licenseButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::transparentBlack);
    licenseButton.setColour (juce::TextButton::textColourOffId, Theme::ink.withAlpha (0.6f));
    licenseButton.setBounds (1236, 598, 84, 24);
    licenseButton.onClick = [this] { licenseOverlay.refresh(); licenseOverlay.setVisible (true); licenseOverlay.toFront (true); };
    addAndMakeVisible (licenseButton);
    addChildComponent (licenseOverlay);
    licenseOverlay.setBounds (getLocalBounds());
    licenseOverlay.setVisible (! proc.license.isLicensed());
#endif
    setScaleFactor ((float) proc.editorScalePct.load() / 100.0f);
    refresh();
    startTimerHz (30);
}

SedaEditor::~SedaEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

SedaEditor::KnobSpec& SedaEditor::addKnob (const juce::String& id, const juce::String& name, float cx, float cy, float r,
                                           std::vector<std::pair<float, juce::String>> marks, std::function<juce::String()> reason, bool withBox)
{
    knobs.push_back (std::make_unique<KnobSpec>());
    auto& k = *knobs.back();
    k.id = id; k.name = name; k.centre = { cx, cy }; k.radius = r; k.marks = std::move (marks); k.big = r > 35.0f; k.reason = reason;
    k.slider = std::make_unique<KnobSlider> (id);
    k.slider->setName (id);
    k.slider->setSliderStyle (juce::Slider::RotaryVerticalDrag);
    k.slider->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    k.slider->setRotaryParameters (kPi * 1.25f, kPi * 2.75f, true);
    k.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, id, *k.slider);
    k.slider->bindParameter (dynamic_cast<juce::RangedAudioParameter*> (proc.apvts.getParameter (id)));
    k.slider->setDoubleClickReturnValue (false, 0.0);
    k.slider->onDoubleClick = [this, id] { resetKnob (id); };   // UI signature 2; typing a value is a double-click on its value window
    const float side = 2.0f * r + 4.0f;
    k.slider->setBounds (juce::Rectangle<float> (side, side).withCentre (k.centre).toNearestInt());
    addAndMakeVisible (*k.slider);
    k.hasBox = withBox;
    if (! withBox)
    {   // the label is the read-out: it shows the value while the knob is held (and as it moves)
        KnobSpec* ks = &k;
        const auto labelArea = juce::Rectangle<float> (110.0f, 18.0f).withCentre ({ cx, cy + r * 1.62f }).getSmallestIntegerContainer();
        k.slider->onHold = [this, ks, labelArea] (bool held) { ks->held = held; repaint (labelArea); };
        k.slider->onValueChange = [this, ks, labelArea] { if (ks->held) repaint (labelArea); };
        return k;
    }
    k.box = std::make_unique<ValueBox> (proc.apvts, id, [this, id] { return ptextOf (id); });
    if (k.big)
    {
        const float ly = cy + r * 1.98f;
        k.box->setBounds (juce::Rectangle<float> (120.0f, 30.0f).withPosition ({ cx - 60.0f, ly + 11.0f }).toNearestInt());
        k.box->fontEm = 19.0f;
    }
    else
    {
        const float ly = cy + r * 1.62f;
        k.box->setBounds (juce::Rectangle<float> (92.0f, 26.0f).withPosition ({ cx - 46.0f, ly + 6.0f }).toNearestInt());
        k.box->fontEm = 14.0f;
    }
    k.box->reasonFn = reason;
    ValueBox* bx = k.box.get();
    if (id == "note")
        k.slider->onHold = [this, bx] (bool held)
        {
            bx->setLive (held);
            if (held) noteGroup = std::make_unique<svl::ParamUndo::Group> (proc.undo, "NOTE");   // AUTO off / SYNC on / the new note: one step
            else noteGroup.reset();
        };
    else
        k.slider->onHold = [bx] (bool held) { bx->setLive (held); };
    if (id == "note") k.slider->onValueChange = [this] { if (noteGroup != nullptr) noteGrabbed(); };
    addAndMakeVisible (*k.box);
    return k;
}

void SedaEditor::addToggle (const juce::String& id, float cx, float cy, float s, const juce::String& top, const juce::String& bottom, const juce::String& label,
                            const juce::String& sub, const juce::String& sub2, std::function<bool()> up, std::function<void()> press)
{
    toggles.push_back (std::make_unique<ToggleSpec>());
    auto& t = *toggles.back();
    t.centre = { cx, cy }; t.scale = s; t.top = top; t.bottom = bottom; t.label = label; t.sub = sub; t.sub2 = sub2; t.up = up;
    t.lever = std::make_unique<BatToggle> (id, s, up, std::move (press));
    t.lever->setBounds (juce::Rectangle<float> (36.0f * s, 84.0f * s).withCentre (t.centre).toNearestInt());
    addAndMakeVisible (*t.lever);
}

PlateKey& SedaEditor::addKey (const juce::String& text, const juce::String& param, juce::Rectangle<float> box, std::function<bool()> lit, std::function<void()> press,
                              float size, float track, bool lamp)
{
    keys.push_back (std::make_unique<PlateKey> (text, param, std::move (lit), std::move (press), size, track, lamp));
    auto& k = *keys.back();
    k.setBounds (box.expanded (PlateKey::kMargin).toNearestInt());
    addAndMakeVisible (k);
    return k;
}

void SedaEditor::addChoiceKey (const juce::String& text, const juce::String& param, int index, juce::Rectangle<float> box, float size, float track)
{
    const juce::String pid = param;
    auto& k = addKey (text, param, box,
                      [this, pid, index]
                      {
                          return choiceOf (pid.toRawUTF8()) == index;
                      },
                      [this, pid, index] { setGesture (pid.toRawUTF8(), (float) index); }, size, track);
    k.getProperties().set ("value", index);
}

//==============================================================================
bool SedaEditor::effAuto() const { return onOf ("noteAuto") && onOf ("listen") && onOf ("sync"); }
int  SedaEditor::shownNote() const { const auto r = proc.getReadouts(); return r.published ? r.chosenNote : choiceOf ("note"); }

float SedaEditor::shownTimeMs() const
{
    return juce::jmax (rawOf ("time"), (float) (seda::Engine::shortestTimeSec (choiceOf ("age"), 48000.0) * 1000.0));
}

juce::String SedaEditor::ptextOf (const juce::String& id) const
{
    if (id == "note") return (effAuto() ? juce::String ("AUTO ") : juce::String()) + seda::noteName (shownNote());   // the note in force, AUTO included
    if (id == "time") return seda::ptext::format (id, shownTimeMs());   // the delay the line really makes (NEW cannot go under 41 ms)
    return seda::ptext::format (id, rawOf (id.toRawUTF8()));
}

juce::String SedaEditor::timeWindowText() const
{
    if (effSync()) return seda::noteName (shownNote());
    const float ms = shownTimeMs();
    return ms >= 999.95f ? juce::String (juce::roundToInt (ms)) : seda::ptext::fixed (ms, 1);
}

juce::String SedaEditor::knobLabelText (const juce::String& id) const
{
    for (auto& k : knobs)
        if (k->id == id) return k->held && ! k->hasBox ? ptextOf (id) : k->name;
    return {};
}

bool SedaEditor::widthDimmed() const { return choiceOf ("stereo") == 0; }
bool SedaEditor::monoBus() const { return proc.getMainBusNumOutputChannels() == 1; }
bool SedaEditor::autoKeyEnabled() const { return onOf ("sync"); }

void SedaEditor::setGesture (const char* id, float plain) { svl::setParamAsGesture (dynamic_cast<juce::RangedAudioParameter*> (proc.apvts.getParameter (id)), plain); }

void SedaEditor::noteGrabbed()
{
    // the user turns the NOTE knob: a note only means something with SYNC on, and AUTO would override the note, so the knob takes the choice over
    if (onOf ("noteAuto")) setGesture ("noteAuto", 0.0f);
    if (! onOf ("sync")) setGesture ("sync", 1.0f);
}

void SedaEditor::resetKnob (const juce::String& id)
{
    auto* p = dynamic_cast<juce::RangedAudioParameter*> (proc.apvts.getParameter (id));
    if (p == nullptr) return;
    const float from = p->getValue();
    svl::setParamAsGesture (p, proc.knobDefault (id));   // asked now: the program loaded at the moment of the click
    anims.erase (std::remove_if (anims.begin(), anims.end(), [&id] (const KnobAnim& a) { return a.id == id; }), anims.end());
    if (std::abs (p->getValue() - from) > 1.0e-4f) anims.push_back ({ id, from, p->getValue(), 0 });   // the drawing glides, the parameter is already there
}

juce::Rectangle<float> SedaEditor::lockupArea() const
{
    if (wordmark.isNull() || mark.isNull()) return {};
    const float ww = kLogoWordH * (float) wordmark.getWidth() / (float) wordmark.getHeight();
    const float mw = kLogoMarkH * (float) mark.getWidth() / (float) mark.getHeight();
    return { kLogoRight - ww - 8.0f - mw, kLogoY - kLogoMarkH * 0.5f - 1.0f, ww + 8.0f + mw, kLogoMarkH };
}

void SedaEditor::mouseMove (const juce::MouseEvent& e)
{
    setMouseCursor (lockupArea().expanded (4.0f).contains (e.position) ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
}

void SedaEditor::mouseUp (const juce::MouseEvent& e)
{
#if SVL_LICENSING
    if (licenseOverlay.isVisible()) return;
#endif
    if (! e.mouseWasDraggedSinceMouseDown() && lockupArea().expanded (4.0f).contains (e.position))
        openLockupLink();
}

void SedaEditor::openLockupLink()
{
    if (openLockupLink_forTest) { openLockupLink_forTest(); return; }
    juce::URL (kLockupUrl).launchInDefaultBrowser();
}

void SedaEditor::stepNote (int dir)
{
    svl::ParamUndo::Group one (proc.undo, "NOTE");
    int cur = choiceOf ("note");
    if (effAuto()) { cur = shownNote(); setGesture ("noteAuto", 0.0f); }   // choosing a note takes the choice over from AUTO
    setGesture ("note", (float) ((cur + dir + seda::kNumNotes) % seda::kNumNotes));
    if (! onOf ("sync")) setGesture ("sync", 1.0f);                           // a note only means something with SYNC on
}

void SedaEditor::stepSource (int dir)
{
    // a program is written into the real parameters (one undo step); the knobs then glide from where they were to where the program put them
    std::vector<KnobAnim> start;
    for (auto& k : knobs)
        if (auto* p = proc.apvts.getParameter (k->id)) start.push_back ({ k->id, p->getValue(), p->getValue(), 0 });
    proc.applyProgram ((choiceOf ("source") + dir + 5) % 5);
    anims.clear();
    for (auto& a : start)
        if (auto* p = proc.apvts.getParameter (a.id))
            if (std::abs (p->getValue() - a.from) > 1.0e-4f) { a.to = p->getValue(); anims.push_back (a); }
}

//==============================================================================
std::vector<PanelControl*> SedaEditor::controls()
{
    std::vector<PanelControl*> out;
    std::function<void (juce::Component&)> walk = [&] (juce::Component& c)
    {
        for (auto* ch : c.getChildren())
        {
            if (! ch->isVisible()) continue;
            if (auto* pc = dynamic_cast<PanelControl*> (ch)) out.push_back (pc);
            walk (*ch);
        }
    };
    walk (*this);
    return out;
}

KnobSlider* SedaEditor::knob (const juce::String& id)
{
    for (auto& k : knobs) if (k->id == id) return k->slider.get();
    return nullptr;
}

PlateKey* SedaEditor::key (const juce::String& id, int value)
{
    for (auto& k : keys)
        if (k->paramId() == id && (value < 0 || (int) k->getProperties().getWithDefault ("value", -1) == value)) return k.get();
    return nullptr;
}

BatToggle* SedaEditor::toggle (const juce::String& id)
{
    for (auto& t : toggles) if (t->lever->paramId() == id) return t->lever.get();
    return nullptr;
}

void SedaEditor::setScalePercent (int pct)
{
    pct = pct == 60 || pct == 100 ? pct : 75;
    proc.editorScalePct = pct;
    setScaleFactor ((float) pct / 100.0f);
    if (sizeChip != nullptr) sizeChip->repaint();
}

//==============================================================================
void SedaEditor::timerCallback()
{
#if SVL_LICENSING
    if (! proc.license.isLicensed() && ! licenseOverlay.isVisible())   // a licence the server revoked while the editor is open
    {
        licenseOverlay.refresh();
        licenseOverlay.setVisible (true);
        licenseOverlay.toFront (true);
    }
#endif
    refresh();
}

void SedaEditor::refresh()
{
    const auto r = proc.getReadouts();
    const bool listen = onOf ("listen"), sy = onOf ("sync"), au = effAuto();

    // the ECHO needle: the wet peak the processor measured since the last tick, about 300 ms of ballistics (0.40 per 33 ms tick)
    {
        const float pk = proc.takeWetPeak();
        wetTarget = pk > 1.0e-5f ? 20.0f * std::log10 (pk) : -90.0f;
        const float aim = juce::jlimit (-40.0f, 3.0f, wetTarget);
        wetShown += (aim - wetShown) * 0.40f;
        if (wetTarget >= -3.0f) peakTicks = 15;
    }
    if (r.throwOn && ! lastThrowOn) flashTicks = 14;
    lastThrowOn = r.throwOn;

    DisplayState ds;
    ds.wetDb = wetShown;
    ds.duckDb = listen ? juce::jmin (0.0f, r.duckDb) : 0.0f;
    ds.peak = peakTicks > 0;
    if (peakTicks > 0) --peakTicks;
    ds.listen = listen; ds.sync = sy; ds.noteAuto = au;
    ds.throwLamp = listen ? juce::jmax (r.throwOn ? 0.45f + 0.55f * r.throwAmount : 0.0f, (float) flashTicks / 14.0f) : 0.0f;
    ds.guard = listen && r.guardOn; ds.guardDb = r.guardDb;
    ds.runLamp = r.published && r.loopGain > 1.0f ? 0.85f + 0.15f * juce::jlimit (0.0f, 1.0f, (r.loopGain - 1.0f) / 0.05f) : 0.0f;   // lit over 1.0 (a little brighter as it climbs to 1.05)
    if (flashTicks > 0) --flashTicks;
    ds.note = sy ? juce::String (seda::noteName (shownNote())) : juce::String ("FREE");

    const float bpm = proc.getHostBpm() > 0.0f ? proc.getHostBpm() : 120.0f;
    const float ms = sy ? (float) (seda::noteBeats (shownNote()) * 60000.0 / bpm) : shownTimeMs();
    const juce::String msText = ms >= 999.95f ? juce::String (juce::roundToInt (ms)) : seda::ptext::fixed (ms, 1);
    const juce::String msShort = ms >= 99.95f ? juce::String (juce::roundToInt (ms)) : seda::ptext::fixed (ms, 1);
    ds.program = juce::jlimit (0, 4, choiceOf ("source"));
    ds.seg = sy ? ds.note : msShort;
    ds.segUnit = sy ? "NOTE" : "MS";
    ds.bbdLine = "BBD " + juce::String (choiceOf ("age") == 0 ? 8192 : 4096) + " STAGES " + kDot + (rawOf ("comp") > 0.5f ? " COMPANDED" : " COMPANDER OFF");
    display.set (ds);

    // dimmed controls (their reason is visible on the panel, in the value window or on the SOURCE line)
    auto dimKnob = [this] (const char* id, bool dim)
    {
        for (auto& k : knobs)
            if (k->id == id)
            {
                if ((bool) k->slider->getProperties().getWithDefault ("dim", false) != dim) { k->slider->getProperties().set ("dim", dim); k->slider->repaint(); }
                if (k->box != nullptr) k->box->setDim (dim);
            }
    };
    dimKnob ("time", sy);
    dimKnob ("width", widthDimmed() || monoBus());   // on a mono output bus the engine runs as MONO whatever STEREO says (the value window then reads MONO BUS)
    for (const char* id : { "duck", "throw", "wetHp" }) dimKnob (id, ! listen);
    dimKnob ("note", ! sy);
    stepPrev->setDimmed (! listen); stepNext->setDimmed (! listen);
    for (auto& k : keys)
    {
        if (k->paramId() == "noteAuto") { k->setEnabled (sy); k->setDimmed (! sy); }
        if (k->paramId() == "stereo" && (k->getButtonText() == "WIDE" || k->getButtonText() == "PING-PONG")) k->setDimmed (monoBus());   // still clickable, the parameter is untouched; only the picture says it plays as MONO
        if (k->isLit() != k->lastLit) k->repaint();
    }
    for (auto& t : toggles)
        if (t->lever->lastUp != (t->up && t->up())) t->lever->repaint();

    // knobs and windows. A program glides the knobs to their new positions; the NOTE knob follows AUTO's choice while AUTO is in force (the knob shows the note in force).
    for (auto& k : knobs)
    {
        float eff = -1.0f;
        for (auto it = anims.begin(); it != anims.end(); ++it)
            if (it->id == k->id)
            {
                const float t = juce::jmin (1.0f, (float) (++it->tick) / 8.0f), e = t * t * (3.0f - 2.0f * t);
                eff = t >= 1.0f ? -1.0f : it->from + (it->to - it->from) * e;
                if (t >= 1.0f) { anims.erase (it); }
                break;
            }
        if (eff < 0.0f && k->id == "note" && effAuto())
            if (auto* p = proc.apvts.getParameter ("note")) eff = p->convertTo0to1 ((float) shownNote());
        const float had = (float) k->slider->getProperties().getWithDefault ("effPos", -1.0f);
        if (had != eff) { k->slider->getProperties().set ("effPos", eff); k->slider->repaint(); }
        if (k->box != nullptr) k->box->refreshIfChanged();
    }

    // what the editor itself paints (legends, the SOURCE line) changes with these
    const unsigned bits = (listen ? 1u : 0u) | (sy ? 2u : 0u) | (widthDimmed() ? 4u : 0u) | (monoBus() ? 8u : 0u);
    if (bits != paintBits) { paintBits = bits; repaint(); }
}

//==============================================================================
void SedaEditor::paint (juce::Graphics& g)
{
    const float W = (float) kW, H = (float) kH;
    g.drawImage (plate, { 0.0f, 0.0f, W, H }, juce::RectanglePlacement::stretchToFit);
    // The 30 Hz timer repaints small regions (a needle, a lamp, one value window). Everything below is drawn only when it touches the region being repainted: the same
    // pixels as drawing it all, a fraction of the work (11 Oct: a value window cost 1 ms because every legend of the panel was laid out again).
    const auto clipR = g.getClipBounds().toFloat();
    auto vis = [&] (juce::Rectangle<float> r) { return clipR.intersects (r); };

    // rack ears: separate, darker strips with a bevel, slots and screws
    for (float ex : { 0.0f, 1556.0f })
    {
        if (! vis ({ ex, 0.0f, 44.0f, H })) continue;
        g.setGradientFill (juce::ColourGradient (rgb (0, 0, 0, 92), ex, 0.0f, rgb (0, 0, 0, 52), ex + 44.0f, 0.0f, false));
        g.fillRect (ex, 0.0f, 44.0f, H);
        const float cx = ex + 22.0f;
        for (float cy : { 62.0f, 578.0f })
        {
            g.setColour (rgb (4, 8, 7, 235));
            g.fillRoundedRectangle (cx - 12.0f, cy - 17.0f, 24.0f, 34.0f, 12.0f);
            fam::screw (g, cx, cy, 8.5f);
        }
    }
    for (float x : { 44.0f, 1556.0f })
    {
        if (! vis ({ x - 2.0f, 0.0f, 6.0f, H })) continue;
        g.setColour (rgb (0, 0, 0, 120));
        g.drawLine (x, 0.0f, x, H, 1.2f);
        g.setColour (rgb (255, 255, 255, 34));
        g.drawLine (x + 1.5f, 0.0f, x + 1.5f, H, 0.8f);
    }

    // name, subtitle fitted to a width, model plate, SOURCE
    if (vis ({ 60.0f, 30.0f, 400.0f, 130.0f }))
    {
    fam::paintFamilyName (g, fam::familyName (Theme::kProductName, 72.0f, 38.0f, 38.0f), Theme::ink);
    {
        const juce::String t = juce::String (juce::CharPointer_UTF8 (Theme::kSubtitle));
        float em = 10.5f;
        while (em > 6.0f && fam::textWidth (t, Theme::legendEm (em), 3.0f * em / 10.5f) > 330.0f) em -= 0.25f;
        silk (g, t, Theme::legendEm (em), 74.0f, 108.0f, 'l', 3.0f * em / 10.5f, Theme::ink);
    }
    {
        const auto f = Theme::legendEm (10.0f);
        const float tw = fam::textWidth (Theme::kModelCode, f, 2.4f);
        g.setColour (Theme::gold);
        g.drawRect (juce::Rectangle<float> (74.0f, 127.0f, tw + 22.0f, 22.0f), 1.3f);
        silk (g, Theme::kModelCode, f, 85.0f, 138.5f, 'l', 2.4f, Theme::gold);
    }
    }
    if (vis ({ 60.0f, 180.0f, 400.0f, 40.0f }))
    {   // the freed corner: plain engraved facts of the unit (not controls)
        const auto lineCol = Theme::muted;
        g.setColour (rgb (0, 0, 0, 120)); g.drawLine (74.0f, 186.0f, 430.0f, 186.0f, 1.0f);
        g.setColour (rgb (255, 255, 255, 34)); g.drawLine (74.0f, 187.2f, 430.0f, 187.2f, 0.8f);
        silk (g, "20 TO 1200 MS  " + kDot + "  FEEDBACK TO 110 %", Theme::fineEm (7.4f), 74.0f, 208.0f, 'l', 1.6f, lineCol);
    }

    // the seam between the two bands
    if (vis ({ 64.0f, 314.0f, 1472.0f, 8.0f })) fam::groove (g, 318.0f, 64.0f, 1536.0f);

    // bottom band: inset sub-panels, engraved bracket headers, knob scales and names
    for (auto r : insets) if (vis (r.expanded (4.0f))) insetPanel (g, r);
    for (const auto& t : titles) if (vis ({ t.x0 - 4.0f, t.y - 14.0f, t.x1 - t.x0 + 8.0f, 30.0f })) engravedBracket (g, t.x0, t.x1, t.y, t.text);
    for (const auto& l : legends) if (vis ({ l.x - 80.0f, l.y - 14.0f, 160.0f, 28.0f })) silk (g, l.text, Theme::legendEm (l.em), l.x, l.y, l.anchor, l.track, l.col);
    for (const auto& k : knobs)
    {
        const float cx = k->centre.x, cy = k->centre.y, r = k->radius;
        if (! vis ({ cx - r * 2.4f, cy - r * 2.3f, r * 4.8f, r * 4.8f })) continue;
        const bool dim = (bool) k->slider->getProperties().getWithDefault ("dim", false);
        if (dim) g.beginTransparencyLayer (0.55f);
        fam::paintKnobTicks (g, cx, cy, r);
        for (const auto& [a, s] : k->marks)
        {
            const float ns = s.length() <= 3 ? juce::jmax (7.0f, r * 0.3f) : juce::jmax (7.0f, r * 0.22f);
            const auto p = fam::polar (cx, cy, r * 1.66f, a);
            silk (g, s, Theme::legendEm (ns), p.x, p.y + 1.0f, 'm', 0.0f, Theme::ink);
        }
        const float ly = k->big ? cy + r * 1.98f : cy + r * 1.62f;
        silk (g, knobLabelText (k->id), Theme::legendEm (k->big ? 10.5f : 8.2f), cx, ly, 'm', k->big ? 2.4f : 1.7f, k->held && ! k->hasBox ? Theme::lamp : Theme::ink);
        if (dim) g.endTransparencyLayer();
    }
    for (const auto& t : toggles)
    {
        const float cx = t->centre.x, cy = t->centre.y, s = t->scale;
        if (! vis ({ cx - 70.0f * s, cy - 40.0f * s, 140.0f * s, 130.0f * s })) continue;
        silk (g, t->top, Theme::fineEm (7.5f * s), cx + 13.0f * s, cy - 26.0f * s, 'l', 0.0f, Theme::ink);
        silk (g, t->bottom, Theme::fineEm (7.5f * s), cx + 13.0f * s, cy + 26.0f * s, 'l', 0.0f, Theme::ink);
        silk (g, t->label, Theme::legendEm (10.5f * s), cx, cy + 50.0f * s, 'm', 2.4f, Theme::ink);
        if (t->sub.isNotEmpty()) silk (g, t->sub, Theme::fineEm (7.0f), cx, cy + 63.0f * s, 'm', 1.0f, Theme::muted);
        if (t->sub2.isNotEmpty()) silk (g, t->sub2, Theme::fineEm (7.0f), cx, cy + 74.0f * s, 'm', 1.0f, Theme::muted);
    }
    if (vis ({ 1400.0f, 296.0f, 140.0f, 24.0f })) silk (g, "WINDOW SIZE", Theme::fineEm (7.0f), 1478.0f, 308.0f, 'm', 1.2f, Theme::muted);
    if (vis ({ 1420.0f, 438.0f, 100.0f, 54.0f }))
    {
        silk (g, "LATENCY", Theme::fineEm (8.0f), 1470.0f, 450.0f, 'm', 1.6f, Theme::ink);
        silk (g, "0 SMP", Theme::fineEm (8.0f), 1470.0f, 463.0f, 'm', 1.6f, Theme::ink);
        silk (g, "ALL RATES", Theme::fineEm (7.0f), 1470.0f, 480.0f, 'm', 1.2f, Theme::muted);
    }

    // foot of the panel
    if (vis ({ 60.0f, 590.0f, 1480.0f, 44.0f }))
    {
        fam::goldPlaque (g, juce::String (juce::CharPointer_UTF8 (Theme::kPlaque)), 800.0f, 610.5f, 9.5f, 2.6f, 'm', 18.0f, 26.0f);
        silk (g, "MEASURED, NOT GUESSED", Theme::fineEm (7.5f), 72.0f, 611.0f, 'l', 1.8f, Theme::muted);
        fam::drawLogo (g, mark, wordmark, kLogoRight, kLogoY, kLogoWordH, kLogoMarkH);
    }
}

bool SedaEditor::keyPressed (const juce::KeyPress& k)
{
#if SVL_LICENSING
    if (licenseOverlay.isVisible()) return false;
#endif
    return svl::handleUndoKey (proc.undo, k);
}
void SedaEditor::mouseDown (const juce::MouseEvent& e)
{
#if SVL_LICENSING
    if (licenseOverlay.isVisible()) return;
#endif
    svl::takeFocusOnClick (*this, e);
}
