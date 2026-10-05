#include "PluginEditor.h"
#include "BinaryData.h"
#include "ParamText.h"
#include <map>

/*  Geometry: the front is 1600 x 640 logical units, the rear plate 1200 x 440, as in the KORO / HAVA faces
    (same plate, screws, rack ears, keys, knobs, toggles, footer plaque and wordmark positions).
*/
namespace
{
using Pt = juce::Point<float>;
constexpr float kPi = juce::MathConstants<float>::pi;

juce::Colour rgb (int r, int g, int b, int a = 255) { return juce::Colour ((juce::uint8) r, (juce::uint8) g, (juce::uint8) b, (juce::uint8) a); }

/** Degrees clockwise from 12 o'clock. */
Pt polar (float cx, float cy, float r, float deg)
{
    const float a = deg * kPi / 180.0f;
    return { cx + std::sin (a) * r, cy - std::cos (a) * r };
}

/** Text anchored 'l', 'm' or 'r' on x, vertically centred on y; track in px per letter. */
void silk (juce::Graphics& g, const juce::String& s, juce::Font f, float x, float y, char h, float track, juce::Colour c)
{
    if (track > 0.0f)
        f = f.withExtraKerningFactor (track / f.getHeight());
    g.setFont (f);
    g.setColour (c);
    const float w = juce::GlyphArrangement::getStringWidth (f, s) + 40.0f, hh = f.getHeight() + 4.0f;
    const float x0 = h == 'l' ? x : h == 'm' ? x - w * 0.5f : x - w;
    const auto j = h == 'l' ? juce::Justification::centredLeft : h == 'm' ? juce::Justification::centred : juce::Justification::centredRight;
    // A trailing letter space would push centred and right-aligned text left: compensate.
    const float shift = h == 'm' ? track * 0.5f : h == 'r' ? track : 0.0f;
    g.drawText (s, juce::Rectangle<float> (x0 + shift, y - hh * 0.5f, w, hh), j, false);
}

float textWidth (const juce::String& s, juce::Font f, float track)
{
    if (track > 0.0f)
        f = f.withExtraKerningFactor (track / f.getHeight());
    return juce::GlyphArrangement::getStringWidth (f, s) - track;
}

void glow (juce::Graphics& g, Pt c, float radius, juce::Colour col, float alpha)
{
    g.setGradientFill (juce::ColourGradient (col.withAlpha (alpha), c.x, c.y, col.withAlpha (0.0f), c.x + radius, c.y, true));
    g.fillEllipse (c.x - radius, c.y - radius, radius * 2.0f, radius * 2.0f);
}

/** A linear gradient across a box at an angle (degrees: 90 = top to bottom, 45 = top-left to bottom-right). */
juce::ColourGradient grad (juce::Rectangle<float> b, juce::Colour c0, juce::Colour c1, float angle)
{
    const float a = angle * kPi / 180.0f;
    const Pt d (std::cos (a) * b.getWidth() * 0.5f, std::sin (a) * b.getHeight() * 0.5f);
    return juce::ColourGradient (c0, b.getCentre() - d, c1, b.getCentre() + d, false);
}

void softShadow (juce::Graphics& g, juce::Rectangle<float> b, float blur, float opacity, Pt offset, float radius, bool ellipse)
{
    // Three widening layers stand in for a Gaussian blur.
    for (int i = 0; i < 3; ++i)
    {
        const auto r = b.translated (offset.x, offset.y).expanded (blur * (float) i * 0.6f);
        g.setColour (juce::Colours::black.withAlpha (opacity * (i == 0 ? 0.5f : 0.22f)));
        if (ellipse) g.fillEllipse (r);
        else g.fillRoundedRectangle (r, radius + blur * (float) i * 0.6f);
    }
}

juce::Image brushedPlate (int w, int h, juce::Colour top, juce::Colour bottom, int seed)
{
    // Drawn at twice the logical size so the face stays crisp when it is scaled up.
    juce::Image img (juce::Image::RGB, w * 2, h * 2, false);
    juce::Graphics g (img);
    g.setGradientFill (juce::ColourGradient (top, 0.0f, 0.0f, bottom, 0.0f, (float) h * 2.0f, false));
    g.fillAll();
    juce::Random rng (seed);
    for (int y = 0; y < h * 2; ++y)
    {
        const float v = rng.nextFloat();
        g.setColour ((v > 0.5f ? juce::Colours::white : juce::Colours::black).withAlpha (0.016f + 0.028f * std::abs (v - 0.5f)));
        g.fillRect (0, y, w * 2, 1);
    }
    for (int i = 0; i < 5200; ++i)
    {
        const float x = rng.nextFloat() * (float) w * 2.0f, len = 60.0f + rng.nextFloat() * 520.0f;
        const int y = rng.nextInt (h * 2);
        g.setColour ((rng.nextBool() ? juce::Colours::white : juce::Colours::black).withAlpha (0.022f + rng.nextFloat() * 0.03f));
        g.drawHorizontalLine (y, x, x + len);
    }
    return img;
}

void screw (juce::Graphics& g, float cx, float cy, float r)
{
    softShadow (g, { cx - r, cy - r, 2 * r, 2 * r }, 1.6f, 0.6f, { 1.0f, 2.0f }, 0.0f, true);
    const juce::Rectangle<float> b (cx - r, cy - r, 2 * r, 2 * r);
    g.setGradientFill (grad (b, rgb (236, 237, 233), rgb (120, 124, 121), 45.0f));
    g.fillEllipse (b);
    g.setColour (rgb (0, 0, 0, 90));
    g.drawEllipse (b, 0.8f);
    g.setColour (rgb (40, 42, 41, 230));
    for (float a : { 45.0f, 135.0f })
        g.drawLine ({ polar (cx, cy, r * 0.62f, a), polar (cx, cy, r * 0.62f, a + 180.0f) }, 2.2f);
}

void groove (juce::Graphics& g, float y, float x0, float x1)
{
    g.setColour (rgb (0, 0, 0, 120));
    g.drawLine (x0, y, x1, y, 1.0f);
    g.setColour (rgb (255, 255, 255, 34));
    g.drawLine (x0, y + 1.2f, x1, y + 1.2f, 0.8f);
}

void bracket (juce::Graphics& g, float x0, float x1, float y, const juce::String& title, float size = 10.5f)
{
    const auto f = Theme::legendEm (size);
    const float cx = (x0 + x1) * 0.5f, tw = textWidth (title, f, 3.0f), gap = 10.0f;
    g.setColour (Theme::ink);
    g.drawLine (x0, y, cx - tw * 0.5f - gap, y, 1.1f);
    g.drawLine (cx + tw * 0.5f + gap, y, x1, y, 1.1f);
    g.drawLine (x0, y, x0, y + 7.0f, 1.1f);
    g.drawLine (x1, y, x1, y + 7.0f, 1.1f);
    silk (g, title, f, cx, y, 'm', 3.0f, Theme::ink);
}

/** The product name in the family's squared, extended stroke letters: T, U, K, A, L. Returns the width.
    The strokes are turned into an outline and filled (not stroked by the renderer), so every renderer draws the same corners. */
float drawProductName (juce::Graphics& g, const juce::String& name, float x, float y, float h, juce::Colour ink)
{
    const float w = 1.34f * h, t = 0.19f * h, r = 0.30f * h, gap = 0.30f * h, in = t * 0.5f;
    juce::Path all;
    float cx = x;
    for (auto ch : name)
    {
        juce::Path p;
        const float L = cx + in, R = cx + w - in, T = y + in, B = y + h - in, M = y + h * 0.5f;
        switch (ch)
        {
            case 'T':
                p.startNewSubPath (L - in, T); p.lineTo (R + in, T);
                p.startNewSubPath ((L + R) * 0.5f, T); p.lineTo ((L + R) * 0.5f, B + in);
                break;
            case 'U':
                p.startNewSubPath (L, T - in); p.lineTo (L, B - r); p.quadraticTo (L, B, L + r, B);
                p.lineTo (R - r, B); p.quadraticTo (R, B, R, B - r); p.lineTo (R, T - in);
                break;
            case 'K':
                p.startNewSubPath (L, T - in); p.lineTo (L, B + in);
                p.startNewSubPath (L, M + t * 0.2f); p.lineTo (R, T - in * 0.3f);
                p.startNewSubPath (L + (R - L) * 0.36f, M - h * 0.12f); p.lineTo (R + in * 0.2f, B + in);
                break;
            case 'A':
                p.startNewSubPath (L, B + in); p.lineTo (L, T + r); p.quadraticTo (L, T, L + r, T);
                p.lineTo (R - r, T); p.quadraticTo (R, T, R, T + r); p.lineTo (R, B + in);
                p.startNewSubPath (L, M + h * 0.06f); p.lineTo (R, M + h * 0.06f);
                break;
            case 'L':
                p.startNewSubPath (L, T - in); p.lineTo (L, B); p.lineTo (R + in * 0.2f, B);
                break;
            default: break;
        }
        all.addPath (p);
        cx += w + gap;
    }
    juce::Path outline;
    juce::PathStrokeType (t, juce::PathStrokeType::mitered, juce::PathStrokeType::butt).createStrokedPath (outline, all);
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillPath (outline, juce::AffineTransform::translation (0.0f, 1.2f));
    g.setColour (ink);
    g.fillPath (outline);
    return cx - gap - x;
}

/** Lamp (gold) or LED (red). */
void drawLamp (juce::Graphics& g, float cx, float cy, float r, bool on, juce::Colour col, juce::Colour off)
{
    g.setColour (rgb (12, 14, 13));
    g.fillEllipse (cx - r * 1.4f, cy - r * 1.4f, r * 2.8f, r * 2.8f);
    const juce::Rectangle<float> b (cx - r, cy - r, 2 * r, 2 * r);
    if (on)
    {
        glow (g, { cx, cy }, r * 3.2f, col, 0.55f);
        g.setGradientFill (grad (b, rgb (255, 246, 222), col, 60.0f));
    }
    else
        g.setGradientFill (grad (b, off, off.darker (1.2f), 60.0f));
    g.fillEllipse (b);
    g.setColour (rgb (255, 255, 255, 70));
    g.fillEllipse (cx - r * 0.55f, cy - r * 0.7f, r * 0.5f, r * 0.45f);
}

void redLed (juce::Graphics& g, float cx, float cy, float r, bool on) { drawLamp (g, cx, cy, r, on, Theme::ledRed, rgb (80, 18, 12)); }

/** One 7-segment digit, slanted 7 degrees. */
void sevenSegment (juce::Graphics& g, float x, float y, float h, char ch)
{
    static const std::map<char, const char*> lit {
        { '0', "abcdef" }, { '1', "bc" }, { '2', "abdeg" }, { '3', "abcdg" }, { '4', "bcfg" }, { '5', "acdfg" },
        { '6', "acdefg" }, { '7', "abc" }, { '8', "abcdefg" }, { '9', "abcdfg" }, { '-', "g" }, { ' ', "" } };
    const float w = 0.56f * h, th = 0.13f * h, gp = th * 0.16f, sl = std::tan (7.0f * kPi / 180.0f);
    const float L = th * 0.5f, R = w - th * 0.5f, T = th * 0.5f, M = h * 0.5f, B = h - th * 0.5f;
    auto hs = [&] (float xa, float xb, float yc)
    {
        return std::vector<Pt> { { xa, yc }, { xa + th / 2, yc - th / 2 }, { xb - th / 2, yc - th / 2 }, { xb, yc }, { xb - th / 2, yc + th / 2 }, { xa + th / 2, yc + th / 2 } };
    };
    auto vs = [&] (float xc, float ya, float yb)
    {
        return std::vector<Pt> { { xc, ya }, { xc + th / 2, ya + th / 2 }, { xc + th / 2, yb - th / 2 }, { xc, yb }, { xc - th / 2, yb - th / 2 }, { xc - th / 2, ya + th / 2 } };
    };
    const std::pair<char, std::vector<Pt>> segs[] {
        { 'a', hs (L + gp, R - gp, T) }, { 'b', vs (R, T + gp, M - gp) }, { 'c', vs (R, M + gp, B - gp) }, { 'd', hs (L + gp, R - gp, B) },
        { 'e', vs (L, M + gp, B - gp) }, { 'f', vs (L, T + gp, M - gp) }, { 'g', hs (L + gp, R - gp, M) } };
    const auto it = lit.find (ch);
    const juce::String on (it != lit.end() ? it->second : "");
    for (const auto& [name, pts] : segs)
    {
        juce::Path p;
        for (size_t i = 0; i < pts.size(); ++i)
        {
            const Pt q (x + pts[i].x - (pts[i].y - h * 0.5f) * sl, y + pts[i].y);
            if (i == 0) p.startNewSubPath (q); else p.lineTo (q);
        }
        p.closeSubPath();
        if (on.containsChar ((juce::juce_wchar) name))
        {
            g.setColour (Theme::ledRed.withAlpha (0.22f));
            g.strokePath (p, juce::PathStrokeType (h * 0.09f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (Theme::ledRed.withAlpha (0.35f));
            g.strokePath (p, juce::PathStrokeType (h * 0.045f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (Theme::ledRed);
        }
        else
            g.setColour (Theme::ledRedOff);
        g.fillPath (p);
    }
}

void paintKnobScale (juce::Graphics& g, const Knob& k)
{
    const float cx = k.centre.x, cy = k.centre.y, r = k.radius;
    const auto col = Theme::ink;
    g.setColour (col);
    if (k.style == Knob::pointer)
    {
        for (const auto& [a, s] : k.marks)
        {
            g.setColour (col);
            g.drawLine ({ polar (cx, cy, r * 1.02f, a), polar (cx, cy, r * 1.18f, a) }, 1.0f);
            const auto p = polar (cx, cy, r * k.ring, a);
            silk (g, s, Theme::legendEm (k.markSize), p.x, p.y, 'm', 0.0f, col);
        }
    }
    else
    {
        if (k.ticks)
            for (int i = 0; i < 11; ++i)
            {
                const float a = -135.0f + 27.0f * (float) i;
                const bool major = i == 0 || i == 5 || i == 10;
                g.setColour (col);
                g.drawLine ({ polar (cx, cy, r * 1.20f, a), polar (cx, cy, r * (major ? 1.42f : 1.31f), a) }, major ? 1.5f : 1.0f);
            }
        for (const auto& [a, s] : k.marks)
        {
            if (s.isEmpty()) continue;
            if (k.style == Knob::intervalMarks)
            {
                g.setColour (col);
                g.drawLine ({ polar (cx, cy, r * 1.20f, a), polar (cx, cy, r * 1.44f, a) }, 1.5f);
                const auto p = polar (cx, cy, r * k.ring, a);
                silk (g, s, Theme::legendEm (k.markSize), p.x, p.y, 'm', 0.6f, col);
                continue;
            }
            const float ns = s.length() <= 3 ? juce::jmax (7.0f, r * 0.3f) : juce::jmax (7.0f, r * 0.22f);
            const auto p = polar (cx, cy, r * 1.66f, a);
            silk (g, s, Theme::legendEm (ns), p.x, p.y + (juce::exactlyEqual (a, 0.0f) ? 0.0f : 1.0f), 'm', 0.0f, col);
        }
    }
    const float ly = k.legendY > 0.0f ? k.legendY : cy + r * 1.98f;
    silk (g, k.name, Theme::legendEm (k.legendSize), cx, ly, 'm', 2.4f, col);
}

/** The value window sits under the knob's name. */
juce::Rectangle<float> valueBoxRect (const Knob& k)
{
    const float ly = k.legendY > 0.0f ? k.legendY : k.centre.y + k.radius * 1.98f;
    return { k.centre.x - ValueBox::kW * 0.5f, ly + 11.0f, ValueBox::kW, ValueBox::kH };
}

float angleOf (juce::AudioProcessorValueTreeState& apvts, const char* id, float value)
{
    auto* p = dynamic_cast<juce::RangedAudioParameter*> (apvts.getParameter (id));
    return p != nullptr ? -135.0f + 270.0f * p->convertTo0to1 (value) : 0.0f;
}

float rawOf (juce::AudioProcessorValueTreeState& apvts, const char* id) { return apvts.getRawParameterValue (id)->load(); }

/** The value as the panel and the host print it ("-18.0 dB", "4.0 : 1", "1/8", "AUTO" ...). */
juce::String paramReadout (juce::AudioProcessorValueTreeState& apvts, const char* id)
{
    return tutkal::ptext::format (id, rawOf (apvts, id));
}

using M = std::vector<std::pair<float, juce::String>>;

/** Marks at parameter values (printed text), at the angle the knob puts that value. */
M marksAt (juce::AudioProcessorValueTreeState& apvts, const char* id, std::initializer_list<std::pair<float, const char*>> list)
{
    M m;
    for (const auto& [v, s] : list)
        m.push_back ({ angleOf (apvts, id, v), juce::String (juce::CharPointer_UTF8 (s)) });
    return m;
}

std::unique_ptr<Knob> makeKnob (juce::AudioProcessorValueTreeState& apvts, const char* id, const char* name, Pt centre, float radius,
                                M marks, Knob::Style style, int layout, float legendSize = 10.5f)
{
    auto k = std::make_unique<Knob>();
    k->id = id; k->name = name; k->centre = centre; k->radius = radius; k->marks = std::move (marks);
    k->style = style; k->layout = layout; k->legendSize = legendSize;
    if (style == Knob::intervalMarks) { k->ticks = false; k->ring = 1.72f; }
    k->readout = [&apvts, id] { return paramReadout (apvts, id); };
    k->box = std::make_unique<ValueBox> (apvts, id, k->readout);
    k->slider.setSliderStyle (juce::Slider::RotaryVerticalDrag);
    k->slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    k->slider.setRotaryParameters (kPi * 1.25f, kPi * 2.75f, true);
    k->attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, id, k->slider);
    k->slider.bindParameter (dynamic_cast<juce::RangedAudioParameter*> (apvts.getParameter (id)));   // Option / Cmd-click: default
    k->slider.setDoubleClickReturnValue (false, 0.0);                                                   // a double-click types a value
    ValueBox* box = k->box.get();
    k->slider.onDoubleClick = [box] { box->beginEdit(); };
    k->slider.onHold = [box] (bool held) { box->setLive (held); };
    return k;
}

void placeKnob (Knob& k)
{
    const float side = 2.0f * k.radius + 4.0f;
    k.slider.setBounds (juce::Rectangle<float> (side, side).withCentre (k.centre).toNearestInt());
    k.box->setBounds (valueBoxRect (k).toNearestInt());
}

/** Toggle legends that belong to the panel, not the lever. */
void toggleLegends (juce::Graphics& g, const Toggle& t)
{
    const float cx = t.centre.x, cy = t.centre.y, s = t.scale;
    silk (g, t.top, Theme::fineEm (7.5f * s), cx + 13.0f * s, cy - 26.0f * s, 'l', 0.0f, Theme::ink);
    silk (g, t.bottom, Theme::fineEm (7.5f * s), cx + 13.0f * s, cy + 26.0f * s, 'l', 0.0f, Theme::ink);
    if (t.label.isNotEmpty())
        silk (g, t.label, Theme::legendEm (10.5f * s), cx, cy + 50.0f * s, 'm', 2.4f, Theme::ink);
    if (t.sub.isNotEmpty())
        silk (g, t.sub, Theme::fineEm (7.5f * s), cx, cy + 63.0f * s, 'm', 1.2f, Theme::muted);
}

void placeToggle (BatToggle& t, Pt c, float s)
{
    t.setBounds (juce::Rectangle<float> (36.0f * s, 84.0f * s).withCentre (c).toNearestInt());
}

std::unique_ptr<Toggle> makeToggle (juce::AudioProcessorValueTreeState& apvts, const char* id, Pt c, float scale, int layout,
                                    const char* label, const char* sub, const char* top = "ON", const char* bottom = "OFF")
{
    auto t = std::make_unique<Toggle> (scale);
    t->centre = c; t->layout = layout; t->label = label; t->sub = sub; t->top = top; t->bottom = bottom;
    t->attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, id, t->lever);
    return t;
}

void placeKey (juce::Component& k, juce::Rectangle<float> box)
{
    k.setBounds (box.expanded (PlateKey::kMargin).toNearestInt());
}

void drawLogo (juce::Graphics& g, const juce::Image& mark, const juce::Image& wordmark, float right, float y, float wordH, float markH)
{
    if (wordmark.isNull() || mark.isNull()) return;
    const float ww = wordH * (float) wordmark.getWidth() / (float) wordmark.getHeight();
    const float mw = markH * (float) mark.getWidth() / (float) mark.getHeight();
    g.setOpacity (1.0f);
    g.drawImage (wordmark, { right - ww, y - wordH * 0.5f, ww, wordH }, juce::RectanglePlacement::stretchToFit);
    g.drawImage (mark, { right - ww - 8.0f - mw, y - markH * 0.5f - 1.0f, mw, markH }, juce::RectanglePlacement::stretchToFit);
}

const juce::String kDot (juce::CharPointer_UTF8 ("\xc2\xb7"));

// The panel layouts: 0 GLUE (and CORE), 1 LA, 2 PUMP.
const char* const kVoicingNames[3] { "GLUE", "LA", "PUMP" };
const char* const kNoteNames[4] { "1/16", "1/8", "1/4", "1/2" };
const char* const kShapeNames[3] { "EXP", "LINEAR", "SWELL" };

/** What the SC HPF actually does for the current setting: the AUTO value follows the voicing (GLUE 200 Hz, the others off). */
int effectiveHpfHz (float raw, int voicingIndex)
{
    if (raw < 0.5f) return voicingIndex == 0 ? 200 : 0;
    if (raw < 20.0f) return 0;
    return juce::roundToInt (raw);
}

int layoutOfVoicing (int voicingIndex) { return voicingIndex == 1 ? 1 : voicingIndex == 2 ? 2 : 0; }   // CORE shows the GLUE knobs
} // namespace

//==============================================================================
TutkalLookAndFeel::TutkalLookAndFeel()
{
    // Menus and tooltips too: nothing on the panel asks the system for a font.
    setDefaultSansSerifTypeface (faces->regular);
    setColour (juce::PopupMenu::backgroundColourId, Theme::panelDeep);
    setColour (juce::PopupMenu::textColourId, Theme::ink);
    setColour (juce::TooltipWindow::backgroundColourId, Theme::panelDeep);
}

void TutkalLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                          float startAngle, float endAngle, juce::Slider& s)
{
    const bool stepped = s.getProperties()["stepped"];
    const float r = ((float) juce::jmin (w, h) - 4.0f) * 0.5f;
    const float cx = (float) x + (float) w * 0.5f, cy = (float) y + (float) h * 0.5f;
    const float ang = (startAngle + pos * (endAngle - startAngle)) * 180.0f / kPi;

    if (stepped)
    {
        // Bakelite pointer knob: a round body with a nose, a cream index.
        softShadow (g, { cx - r * 0.8f, cy - r * 0.8f, r * 1.6f, r * 1.6f }, r * 0.1f, 0.6f, { r * 0.06f, r * 0.14f }, 0.0f, true);
        juce::Path nose;
        nose.addTriangle (polar (cx, cy, r * 0.55f, ang - 50.0f), polar (cx, cy, r * 1.02f, ang), polar (cx, cy, r * 0.55f, ang + 50.0f));
        g.setColour (rgb (20, 23, 22));
        g.fillPath (nose);
        const juce::Rectangle<float> body (cx - r * 0.78f, cy - r * 0.78f, r * 1.56f, r * 1.56f);
        g.setGradientFill (grad (body, rgb (44, 49, 47), rgb (7, 9, 9), 45.0f));
        g.fillEllipse (body);
        const auto tip = polar (cx, cy, r * 0.93f, ang);
        g.setColour (rgb (22, 25, 24));
        g.fillEllipse (tip.x - r * 0.13f, tip.y - r * 0.13f, r * 0.26f, r * 0.26f);
        g.setColour (rgb (255, 255, 255, 26));
        g.drawEllipse (body, 0.7f);
        g.setColour (Theme::ink);
        g.drawLine ({ polar (cx, cy, r * 0.15f, ang), polar (cx, cy, r * 0.9f, ang) }, juce::jmax (1.4f, r * 0.09f));
        return;
    }

    // Shadow, knurled skirt.
    const juce::Rectangle<float> body (cx - r, cy - r, 2 * r, 2 * r);
    softShadow (g, body, r * 0.10f, 0.55f, { r * 0.06f, r * 0.13f }, 0.0f, true);
    g.setGradientFill (grad (body, rgb (43, 48, 46), rgb (11, 13, 12), 90.0f));
    g.fillEllipse (body);
    for (int i = 0; i < 60; ++i)
    {
        const float a = 360.0f * (float) i / 60.0f;
        const float light = 0.5f + 0.5f * std::cos (a * kPi / 180.0f + 0.8f);
        g.setColour (juce::Colours::white.withAlpha (0.05f + 0.15f * light));
        g.drawLine ({ polar (cx, cy, r * 0.86f, a), polar (cx, cy, r * 0.985f, a) }, 0.55f);
    }

    // Machined cap: turned rings, two sheens.
    const float cr = r * 0.74f;
    const juce::Rectangle<float> cap (cx - cr, cy - cr, 2 * cr, 2 * cr);
    g.setGradientFill (grad (cap, rgb (222, 223, 218), rgb (110, 116, 113), 45.0f));
    g.fillEllipse (cap);
    int ring = 0;
    for (float rr = cr * 0.12f; rr < cr; rr += juce::jmax (0.9f, cr / 26.0f), ++ring)
    {
        g.setColour ((ring & 1) ? rgb (255, 255, 255, 20) : rgb (0, 0, 0, 14));
        g.drawEllipse (cx - rr, cy - rr, rr * 2.0f, rr * 2.0f, 0.45f);
    }
    juce::Path sheen;
    sheen.addPieSegment (cap, 234.0f * kPi / 180.0f, 291.0f * kPi / 180.0f, 0.0f);
    sheen.addPieSegment (cap, 54.0f * kPi / 180.0f, 111.0f * kPi / 180.0f, 0.0f);
    g.setColour (rgb (255, 255, 255, 50));
    g.fillPath (sheen);
    g.setColour (rgb (0, 0, 0, 90));
    g.drawEllipse (cap, 0.8f);

    // Gold index, engraved into the cap and run onto the skirt.
    const juce::Line<float> index (polar (cx, cy, cr * 0.28f, ang), polar (cx, cy, r * 0.97f, ang));
    g.setColour (rgb (0, 0, 0, 128));
    g.drawLine (index, juce::jmax (2.0f, r * 0.13f));
    g.setColour (Theme::gold);
    g.drawLine (index, juce::jmax (1.3f, r * 0.08f));
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
        resetToDefault();
        return;
    }
    setMouseDragSensitivity (m.isShiftDown() ? 1250 : 250);   // Shift: five times finer, decided when the knob is taken
    if (onHold) onHold (true);
    juce::Slider::mouseDown (e);
}

void KnobSlider::mouseDrag (const juce::MouseEvent& e)
{
    if (! swallow) juce::Slider::mouseDrag (e);
}

void KnobSlider::mouseUp (const juce::MouseEvent& e)
{
    if (swallow) { swallow = false; return; }
    juce::Slider::mouseUp (e);
    if (onHold) onHold (false);
}

void KnobSlider::mouseDoubleClick (const juce::MouseEvent&)
{
    if (onDoubleClick) onDoubleClick();
}

//==============================================================================
ValueBox::ValueBox (juce::AudioProcessorValueTreeState& a, const juce::String& paramId, std::function<juce::String()> t)
    : apvts (a), id (paramId), textFn (std::move (t))
{
    setRepaintsOnMouseActivity (false);
}

void ValueBox::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat().reduced (3.0f);
    // The bezel, then the glass: a dark recess with a shadow on top and a catch-light along the lower edge.
    g.setColour (rgb (8, 10, 9));
    g.fillRoundedRectangle (b.expanded (2.5f), 5.0f);
    g.setGradientFill (grad (b, rgb (1, 8, 7), rgb (8, 26, 23), 90.0f));
    g.fillRoundedRectangle (b, 3.0f);
    g.setColour (rgb (0, 0, 0, 170));
    g.drawLine (b.getX() + 3.0f, b.getY() + 1.0f, b.getRight() - 3.0f, b.getY() + 1.0f, 1.6f);
    g.setColour (rgb (255, 255, 255, 34));
    g.drawLine (b.getX() + 4.0f, b.getBottom() + 3.0f, b.getRight() - 4.0f, b.getBottom() + 3.0f, 0.8f);
    if (live || isEditing())
    {
        g.setColour (Theme::gold.withAlpha (0.75f));
        g.drawRoundedRectangle (b, 3.0f, 1.2f);
    }
    if (! isEditing())
        silk (g, text(), Theme::monoEm (22.0f), b.getCentreX(), b.getCentreY() + 0.5f, 'm', 0.4f, live ? Theme::lamp : Theme::label);
}

void ValueBox::resized()
{
    if (field != nullptr)
        field->setBounds (getLocalBounds().reduced (5, 4));
}

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
        field->setFont (Theme::monoEm (22.0f));
        field->onReturnKey = [this] { endEdit (true); };
        field->onEscapeKey = [this] { endEdit (false); };
        field->onFocusLost = [this] { endEdit (false); };
        addChildComponent (*field);
    }
    field->setText (text(), false);
    field->applyFontToAllText (Theme::monoEm (22.0f));
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
    if (dynamic_cast<juce::AudioParameterChoice*> (p) != nullptr)
    {
        int index = 0;
        if (! tutkal::ptext::parseChoice (id, typed, index)) return false;
        plain = (float) index;
    }
    else if (! tutkal::ptext::parse (id, typed, plain))
        return false;
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
    if (auto* ed = findParentComponentOfClass<TutkalEditor>())
        ed->grabKeyboardFocus();   // Cmd+Z works again at once
}

//==============================================================================
PlateKey::PlateKey (const juce::String& t, std::function<bool()> isLit, float textSize, float trackPx)
    : juce::Button (t), lit (std::move (isLit)), size (textSize), track (trackPx) {}

void PlateKey::paintButton (juce::Graphics& g, bool over, bool down)
{
    const bool on = lit && lit();
    const auto box = getLocalBounds().toFloat().reduced (kMargin).translated (0.0f, down ? 1.0f : 0.0f);
    g.setColour (rgb (8, 10, 9));
    g.fillRoundedRectangle (box.expanded (3.5f), 5.0f);
    softShadow (g, box, 1.5f, 0.5f, { 1.0f, 2.5f }, 3.0f, false);
    if (on)
    {
        glow (g, box.getCentre(), box.getWidth() * 0.62f, Theme::goldLight, 0.35f);
        g.setGradientFill (grad (box, rgb (240, 212, 146), rgb (190, 152, 82), 90.0f));
    }
    else
        g.setGradientFill (grad (box, rgb (242, 238, 226).brighter (over ? 0.05f : 0.0f), rgb (206, 199, 181), 90.0f));
    g.fillRoundedRectangle (box, 3.0f);
    g.setColour (rgb (255, 255, 255, 110));
    g.drawLine (box.getX() + 3.0f, box.getY() + 1.0f, box.getRight() - 3.0f, box.getY() + 1.0f, 0.8f);
    silk (g, getButtonText(), Theme::legendEm (size), box.getCentreX(), box.getCentreY() + 0.5f, 'm', track, Theme::panel);
}

//==============================================================================
void BatToggle::paintButton (juce::Graphics& g, bool, bool)
{
    const float cx = (float) getWidth() * 0.5f, cy = (float) getHeight() * 0.5f;
    const bool on = getToggleState();
    const juce::Rectangle<float> nut (cx - 12.0f * s, cy - 12.0f * s, 24.0f * s, 24.0f * s);
    softShadow (g, nut, 2.0f * s, 0.55f, { 1.5f, 3.0f }, 0.0f, true);
    g.setGradientFill (grad (nut, rgb (232, 233, 229), rgb (96, 100, 98), 45.0f));
    g.fillEllipse (nut);
    g.setColour (rgb (40, 43, 42));
    g.fillEllipse (cx - 7.0f * s, cy - 7.0f * s, 14.0f * s, 14.0f * s);

    const float dir = on ? -1.0f : 1.0f, tipy = cy + dir * 30.0f * s;
    juce::Path lever, lit;
    lever.addQuadrilateral (cx - 3.4f * s, cy, cx + 3.4f * s, cy, cx + 2.2f * s, tipy, cx - 2.2f * s, tipy);
    lit.addQuadrilateral (cx - 3.4f * s, cy, cx - 0.6f * s, cy, cx - 0.4f * s, tipy, cx - 2.2f * s, tipy);
    g.setColour (rgb (0, 0, 0, 70));
    g.fillPath (lever, juce::AffineTransform::translation (3.0f, 3.0f));
    g.setColour (rgb (200, 202, 198));
    g.fillPath (lever);
    g.setColour (rgb (245, 246, 242));
    g.fillPath (lit);
    const juce::Rectangle<float> ball (cx - 5.5f * s, tipy - 5.5f * s, 11.0f * s, 11.0f * s);
    softShadow (g, ball, 1.2f, 0.5f, { 2.0f, 3.0f }, 0.0f, true);
    g.setGradientFill (grad (ball, rgb (250, 250, 247), rgb (120, 124, 121), 45.0f));
    g.fillEllipse (ball);
}

//==============================================================================
namespace
{
constexpr float dX0 = 486.0f, dY0 = 34.0f, dX1 = 1376.0f, dY1 = 296.0f;   // the cream window
constexpr float dPad = 10.0f;                                              // component margin for bezel and shadow
constexpr float kMeterMaxDb = 20.0f;
constexpr float kMeterSpan = 52.0f;                                        // needle sweep, degrees either side of vertical
constexpr float kNeedleAlpha = 0.40f;                                      // per 30 Hz tick: 1 - exp(-33 ms / 65 ms), 99 % in 300 ms
constexpr int   kPeakHoldTicks = 45;                                       // 1.5 s
}

void DisplayWindow::refresh()
{
    const float gr = juce::jlimit (0.0f, 40.0f, proc.getGainReductionDb());
    needle += (gr - needle) * kNeedleAlpha;
    if (needle < 0.005f) needle = 0.0f;
    if (gr >= peak) { peak = gr; peakHold = kPeakHoldTicks; }
    else if (peakHold > 0) --peakHold;
    else peak = juce::jmax (0.0f, peak - 0.5f);

    auto& a = proc.apvts;
    juce::String key;
    key << juce::roundToInt (needle * 20.0f) << '|' << juce::roundToInt (peak * 10.0f) << '|' << juce::roundToInt (rawOf (a, "voicing"))
        << '|' << rawOf (a, "scHpf") << '|' << rawOf (a, "autoRelease") << '|' << rawOf (a, "limit") << '|' << rawOf (a, "ext")
        << '|' << rawOf (a, "pumpNote") << '|' << rawOf (a, "pumpShape") << '|' << juce::roundToInt (proc.getHostBpm() * 10.0f)
        << '|' << (int) proc.isSidechainConnected();
    if (key != lastKey)
    {
        lastKey = key;
        repaint();
    }
}

void DisplayWindow::paint (juce::Graphics& g)
{
    g.addTransform (juce::AffineTransform::translation (dPad - dX0, dPad - dY0));
    const float x0 = dX0, y0 = dY0, x1 = dX1, y1 = dY1;
    const juce::Rectangle<float> win (x0, y0, x1 - x0, y1 - y0);
    const auto ink = Theme::panel, ink2 = Theme::labelInk2;
    auto& a = proc.apvts;
    const int voicing = juce::jlimit (0, 3, juce::roundToInt (rawOf (a, "voicing")));
    const int lay = layoutOfVoicing (voicing);

    softShadow (g, win.expanded (6.0f), 4.0f, 0.6f, { 1.0f, 4.0f }, 10.0f, false);
    g.setGradientFill (grad (win.expanded (7.0f), rgb (30, 33, 32), rgb (4, 5, 5), 90.0f));
    g.fillRoundedRectangle (win.expanded (7.0f), 10.0f);
    g.setGradientFill (grad (win, Theme::label, Theme::labelDeep, 90.0f));
    g.fillRoundedRectangle (win, 4.0f);

    // Headings and the gold rule, set like the KASET cassette label.
    const char* heads[4] { "GLUE", "LA", "PUMP", "CORE" };
    const char* subs[4] { "BUS GLUE", "VOCAL LEVELLER", "SIDECHAIN PUMP", "PLAIN ENGINE" };
    silk (g, "GAIN REDUCTION " + kDot + " dB", Theme::legendEm (8.0f), x0 + 16, y0 + 16, 'l', 1.8f, ink);
    silk (g, juce::String (heads[voicing]) + " " + kDot + " " + subs[voicing], Theme::legendEm (8.0f), x0 + 446, y0 + 16, 'l', 1.8f, ink);
    silk (g, "SUBVERSELAB", Theme::legendEm (7.5f), x1 - 16, y0 + 16, 'r', 1.8f, ink);
    g.setColour (Theme::gold);
    g.fillRect (x0 + 12, y0 + 27, x1 - x0 - 24, 3.0f);

    // ---- the meter: 0 .. 20 dB of gain reduction, gold scale, 300 ms needle, peak hold ----
    const juce::Rectangle<float> box (x0 + 14, y0 + 38, 402, 196);
    const float bw = box.getWidth(), bh = box.getHeight();
    g.setGradientFill (grad (box.expanded (3.0f), rgb (30, 33, 32), rgb (4, 5, 5), 90.0f));
    g.fillRoundedRectangle (box.expanded (3.0f), 4.0f);
    g.setGradientFill (grad (box, rgb (247, 241, 224), rgb (224, 214, 188), 90.0f));
    g.fillRoundedRectangle (box, 2.0f);

    const float cx = box.getCentreX(), py = box.getBottom() - bh * 0.06f, R = bh * 0.76f;
    auto ang = [] (float db) { return -kMeterSpan + 2.0f * kMeterSpan * juce::jlimit (-0.02f, 1.04f, db / kMeterMaxDb); };
    const auto dark = rgb (32, 32, 30), goldInk = rgb (128, 97, 42), red = rgb (190, 55, 38);
    g.saveState();
    g.reduceClipRegion (box.toNearestInt());
    {
        juce::Path arc;
        arc.addCentredArc (cx, py, R, R, 0.0f, ang (0.0f) * kPi / 180.0f, ang (kMeterMaxDb) * kPi / 180.0f, true);
        g.setColour (goldInk);
        g.strokePath (arc, juce::PathStrokeType (1.3f));
        juce::Path band;
        band.addCentredArc (cx, py, R + 2.0f, R + 2.0f, 0.0f, ang (12.0f) * kPi / 180.0f, ang (kMeterMaxDb) * kPi / 180.0f, true);
        g.setColour (Theme::gold);
        g.strokePath (band, juce::PathStrokeType (4.0f));
    }
    for (int db = 0; db <= 20; ++db)
    {
        const float an = ang ((float) db);
        const bool major = (db % 2) == 0;
        g.setColour (goldInk);
        g.drawLine ({ polar (cx, py, R, an), polar (cx, py, R + (major ? 9.0f : 5.0f), an) }, major ? 1.3f : 0.9f);
        if (major)
        {
            const auto t = polar (cx, py, R + 19.0f, an);
            silk (g, juce::String (db), Theme::legendEm (7.8f), t.x, t.y, 'm', 0.0f, db >= 12 ? rgb (120, 70, 30) : goldInk);
        }
    }
    silk (g, "GR", Theme::legendEm (17.0f), box.getX() + bw * 0.17f, py - R * 0.30f, 'm', 2.0f, dark);
    silk (g, "dB", Theme::fineEm (8.5f), box.getX() + bw * 0.17f, py - R * 0.30f + 17.0f, 'm', 1.0f, ink2);

    // peak hold: a red marker on the inside of the scale, and its lamp
    if (peak > 0.05f)
    {
        const float pa = ang (juce::jmin (peak, kMeterMaxDb));
        juce::Path tri;
        tri.addTriangle (polar (cx, py, R - 2.0f, pa), polar (cx, py, R - 15.0f, pa - 2.6f), polar (cx, py, R - 15.0f, pa + 2.6f));
        g.setColour (red);
        g.fillPath (tri);
    }
    redLed (g, box.getRight() - 86.0f, box.getY() + 18.0f, 3.0f, peak >= 0.5f);
    silk (g, "PEAK HOLD", Theme::legendEm (7.0f), box.getRight() - 78.0f, box.getY() + 18.5f, 'l', 0.8f, peak >= 0.5f ? ink : ink2);

    const float na = ang (needle);
    g.setColour (rgb (0, 0, 0, 40));
    g.drawLine ({ Pt (cx + 1.5f, py + 2.0f), polar (cx + 1.5f, py + 2.0f, R + 6.0f, na) }, 1.8f);
    g.setColour (rgb (20, 20, 20));
    g.drawLine ({ Pt (cx, py), polar (cx, py, R + 6.0f, na) }, 1.4f);
    const float hy = box.getBottom() - bh * 0.13f;
    g.setColour (rgb (22, 24, 23));
    g.fillRect (box.getX(), hy, bw, box.getBottom() - hy);
    g.setColour (rgb (0, 0, 0, 220));
    g.drawLine (box.getX(), hy, box.getRight(), hy, 0.8f);
    silk (g, "GAIN REDUCTION", Theme::legendEm (7.5f), cx, (hy + box.getBottom()) * 0.5f + 0.5f, 'm', 2.2f, Theme::ink);
    silk (g, "0", Theme::legendEm (7.5f), box.getX() + 14.0f, (hy + box.getBottom()) * 0.5f + 0.5f, 'm', 0.0f, Theme::muted);
    silk (g, "20", Theme::legendEm (7.5f), box.getRight() - 14.0f, (hy + box.getBottom()) * 0.5f + 0.5f, 'm', 0.0f, Theme::muted);
    juce::Path sheen;
    sheen.addQuadrilateral (box.getX(), box.getY(), box.getX() + bw * 0.42f, box.getY(), box.getX() + bw * 0.18f, hy, box.getX(), hy);
    g.setColour (rgb (255, 255, 255, 20));
    g.fillPath (sheen);
    g.restoreState();

    // ---- right half ----
    g.setColour (Theme::gold.withAlpha (200.0f / 255.0f));
    g.fillRect (x0 + 430, y0 + 38, 1.2f, 196.0f);

    // lamps, per voicing
    const float lx = x0 + 452, tx = x0 + 464;
    const float rowY[3] { y0 + 66, y0 + 100, y0 + 134 };
    auto row = [&] (int i, bool on, const juce::String& title, const juce::String& sub)
    {
        redLed (g, lx, rowY[i], 3.2f, on);
        silk (g, title, Theme::legendEm (8.5f), tx, rowY[i] + 0.5f, 'l', 1.4f, on ? ink : ink2);
        if (sub.isNotEmpty())
            silk (g, sub, Theme::fineEm (7.0f), tx, rowY[i] + 13.0f, 'l', 0.8f, ink2);
    };
    const bool ext = rawOf (a, "ext") > 0.5f;
    const bool keyLive = ext && proc.isSidechainConnected();
    const int hpf = effectiveHpfHz (rawOf (a, "scHpf"), voicing);
    const juce::String hpfText = hpf > 0 ? juce::String (hpf) + " HZ" : juce::String ("OFF");
    if (lay == 0)
    {
        row (0, hpf > 0, "SC HPF  " + hpfText, rawOf (a, "scHpf") < 0.5f ? "AUTO: THE VOICING'S OWN" : "SET ON THE REAR PLATE");
        row (1, rawOf (a, "autoRelease") > 0.5f, "AUTO RELEASE", "RELEASE REMEMBERS THE SQUEEZE");
        row (2, keyLive, keyLive ? "KEY  SIDECHAIN" : "KEY  SELF", ext ? (proc.isSidechainConnected() ? "EXT ON, BUS CONNECTED" : "EXT ON, NO SIDECHAIN BUS") : "THE DETECTOR HEARS THE INPUT");
    }
    else if (lay == 1)
    {
        const bool lim = rawOf (a, "limit") > 0.5f;
        row (0, ! lim, "COMPRESS", "3:1, SOFT KNEE");
        row (1, lim, "LIMIT", "12:1, TIGHT KNEE");
        row (2, keyLive, keyLive ? "KEY  SIDECHAIN" : "KEY  SELF", ext ? (proc.isSidechainConnected() ? "EXT ON, BUS CONNECTED" : "EXT ON, NO SIDECHAIN BUS") : "THE DETECTOR HEARS THE INPUT");
    }
    else
    {
        const int note = juce::jlimit (0, 3, juce::roundToInt (rawOf (a, "pumpNote")));
        const int shape = juce::jlimit (0, 2, juce::roundToInt (rawOf (a, "pumpShape")));
        const float bpm = proc.getHostBpm();
        const juce::String tempo = bpm > 0.0f ? juce::String (bpm, 1) + " BPM" : juce::String ("120 BPM");
        silk (g, juce::String (kNoteNames[note]) + "  " + kDot + "  " + tempo, Theme::legendEm (14.0f), lx - 4.0f, rowY[0] + 2.0f, 'l', 1.6f, ink);
        silk (g, bpm > 0.0f ? "NOTE  " + kDot + "  HOST TEMPO" : "NOTE  " + kDot + "  NO HOST TEMPO, 120 USED", Theme::fineEm (7.0f), lx - 4.0f, rowY[0] + 20.0f, 'l', 0.8f, ink2);
        redLed (g, lx, rowY[1] + 4.0f, 3.2f, keyLive);
        silk (g, keyLive ? "KEY  SIDECHAIN" : "KEY  SELF", Theme::legendEm (8.5f), tx, rowY[1] + 4.5f, 'l', 1.4f, keyLive ? ink : ink2);
        silk (g, ext ? (proc.isSidechainConnected() ? "EXT ON, BUS CONNECTED" : "EXT ON, NO SIDECHAIN BUS") : "EXT OFF: THE INPUT KEYS ITSELF", Theme::fineEm (7.0f), tx, rowY[1] + 17.0f, 'l', 0.8f, ink2);
        for (int i = 0; i < 3; ++i)
        {
            const float sx = lx + (float) i * 62.0f;
            redLed (g, sx, rowY[2] + 6.0f, 3.0f, shape == i);
            silk (g, kShapeNames[i], Theme::legendEm (7.8f), sx + 8.0f, rowY[2] + 6.5f, 'l', 1.0f, shape == i ? ink : ink2);
        }
    }

    // GR now, in 7 segments
    {
        const float bx = x0 + 650, by = y0 + 52, bwid = 224, bhei = 90;
        silk (g, "GR NOW", Theme::legendEm (8.0f), bx, y0 + 42, 'l', 1.8f, ink);
        g.setColour (rgb (12, 12, 12));
        g.fillRoundedRectangle (juce::Rectangle<float> (bx - 2, by - 2, bwid + 4, bhei + 4), 3.0f);
        g.setColour (Theme::redWindow);
        g.fillRoundedRectangle (juce::Rectangle<float> (bx, by, bwid, bhei), 2.0f);
        const int tenths = juce::jlimit (0, 999, juce::roundToInt (needle * 10.0f));
        const char d[3] { tenths >= 100 ? (char) ('0' + tenths / 100) : ' ', (char) ('0' + (tenths / 10) % 10), (char) ('0' + tenths % 10) };
        const float dh = 62.0f, dx = bx + 22.0f, dy = by + (bhei - dh) * 0.5f;
        for (int i = 0; i < 3; ++i)
            sevenSegment (g, dx + (float) i * 52.0f, dy, dh, d[i]);
        {
            const float px = dx + 52.0f + 0.56f * dh + 7.0f, pyy = dy + dh - 4.0f;
            glow (g, { px, pyy }, 6.0f, Theme::ledRed, 0.5f);
            g.setColour (Theme::ledRed);
            g.fillEllipse (px - 2.8f, pyy - 2.8f, 5.6f, 5.6f);
        }
        silk (g, "dB", Theme::legendEm (13.0f), bx + bwid - 28.0f, by + bhei * 0.5f, 'm', 1.0f, Theme::ledRedDim);
    }

    g.setColour (Theme::gold.withAlpha (200.0f / 255.0f));
    g.fillRect (x0 + 440, y0 + 162, x1 - x0 - 452, 1.2f);
    const char* notes[3][3] = {
        { "RELEASE THAT REMEMBERS: FAST AFTER A HIT, SLOW AFTER A LONG SQUEEZE (AUTO).",
          "THE KICK ALONE DOES NOT DRIVE IT: THE DETECTOR SITS BEHIND AN SC HPF.",
          "RATIO UP, MORE GAIN REDUCTION. ON DRUMS AND THE MIX BUS." },
        { "PEAK REDUCTION SETS HOW HARD IT LEVELS; COMPRESS AND LIMIT SET THE SHAPE.",
          "THE LONGER IT WORKS, THE SLOWER IT LETS GO.",
          "MADE FOR VOCALS AND BASS. RATIO AND ATTACK ARE NOT USED." },
        { "THE KICK ON THE SIDECHAIN BUS SETS THE PUMP: DEPTH IN dB.",
          "NOTE LENGTH COMES FROM THE HOST TEMPO; SHAPE IS THE CURVE OF THE RECOVERY.",
          "EXT ON: ONLY THE SIDECHAIN IS HEARD BY THE DETECTOR." } };
    for (int i = 0; i < 3; ++i)
        silk (g, notes[lay][i], Theme::fineEm (7.2f), x0 + 446, y0 + 180 + (float) i * 16.0f, 'l', 0.7f, ink2);
    silk (g, "METER: PEAK GAIN REDUCTION OF EACH BLOCK  " + kDot + "  NEEDLE 300 MS", Theme::fineEm (7.0f), x0 + 16, y1 - 14, 'l', 0.8f, ink2);
    silk (g, "LATENCY 63 SAMPLES AT EVERY SAMPLE RATE", Theme::fineEm (7.0f), x1 - 16, y1 - 14, 'r', 0.8f, ink2);
}

//==============================================================================
FrontPanel::FrontPanel (TutkalAudioProcessor& p, std::function<void()> showRear)
    : proc (p),
      rearKey ("REAR", nullptr, 8.2f, 1.6f),
      display (p)
{
    texture = brushedPlate (TutkalEditor::kW, TutkalEditor::kH, rgb (26, 70, 66), rgb (9, 42, 39), 77);
    mark = juce::ImageCache::getFromMemory (BinaryData::mark_cream_png, BinaryData::mark_cream_pngSize);
    wordmark = juce::ImageCache::getFromMemory (BinaryData::wordmark_cream_png, BinaryData::wordmark_cream_pngSize);

    auto& a = proc.apvts;
    for (int i = 0; i < 3; ++i)
    {
        auto k = std::make_unique<PlateKey> (kVoicingNames[i], [this, i] { return juce::roundToInt (rawOf (proc.apvts, "voicing")) == i; }, 10.0f, 1.8f);
        k->onClick = [this, i] { svl::setParamAsGesture (proc.apvts.getParameter ("voicing"), (float) i); };   // one gesture = one undo step
        addAndMakeVisible (*k);
        voicingKeys.push_back (std::move (k));
    }
    rearKey.onClick = std::move (showRear);
    addAndMakeVisible (rearKey);

    const float cy = 450.0f, r = 41.0f;
    // ---- GLUE (layout 0) ----
    {
        auto k = makeKnob (a, "threshold", "THRESHOLD", { 160, cy }, r,
                           marksAt (a, "threshold", { { -40.0f, "\xe2\x88\x92" "40" }, { -30.0f, "\xe2\x88\x92" "30" }, { -20.0f, "\xe2\x88\x92" "20" }, { -10.0f, "\xe2\x88\x92" "10" }, { 0.0f, "0" } }),
                           Knob::intervalMarks, 0);
        addKnob (std::move (k));
        addKnob (makeKnob (a, "ratio", "RATIO", { 350, cy }, r,
                           marksAt (a, "ratio", { { 1.5f, "1.5" }, { 2.0f, "2" }, { 4.0f, "4" }, { 10.0f, "10" }, { 20.0f, "20" } }), Knob::intervalMarks, 0));
        {
            auto at = makeKnob (a, "attack", "ATTACK", { 560, cy }, r,
                                marksAt (a, "attack", { { 0.1f, "0.1" }, { 0.3f, "0.3" }, { 1.0f, "1" }, { 3.0f, "3" }, { 10.0f, "10" }, { 30.0f, "30" } }), Knob::intervalMarks, 0);
            addKnob (std::move (at));
        }
        {
            auto rl = makeKnob (a, "release", "RELEASE", { 760, cy }, r,
                                marksAt (a, "release", { { 100.0f, "0.1" }, { 300.0f, "0.3" }, { 600.0f, "0.6" }, { 1200.0f, "1.2" } }), Knob::intervalMarks, 0);
            addKnob (std::move (rl));
        }
        addKnob (makeKnob (a, "makeup", "MAKEUP", { 1130, cy }, r,
                           marksAt (a, "makeup", { { 0.0f, "0" }, { 5.0f, "5" }, { 10.0f, "10" }, { 15.0f, "15" }, { 20.0f, "20" } }), Knob::intervalMarks, 0));
        addKnob (makeKnob (a, "mix", "MIX", { 1310, cy }, r,
                           marksAt (a, "mix", { { 0.0f, "0" }, { 50.0f, "50" }, { 100.0f, "100" } }), Knob::intervalMarks, 0));
        toggles.push_back (makeToggle (a, "autoRelease", { 930, 446 }, 1.0f, 0, "AUTO", "RELEASE MEMORY"));
    }
    // ---- LA (layout 1) ----
    {
        auto pr = makeKnob (a, "peakReduction", "PEAK REDUCTION", { 230, cy }, 48.0f,
                            M { { -135.0f, "0" }, { 0.0f, "5" }, { 135.0f, "10" } }, Knob::numbers, 1);
        pr->legendY = cy + 41.0f * 1.98f;   // the legend line of every other knob
        addKnob (std::move (pr));
        toggles.push_back (makeToggle (a, "limit", { 470, 446 }, 1.0f, 1, "MODE", "COMPRESS 3:1 / LIMIT 12:1", "LIMIT", "COMPRESS"));
        addKnob (makeKnob (a, "makeup", "GAIN", { 900, cy }, r,
                           marksAt (a, "makeup", { { 0.0f, "0" }, { 5.0f, "5" }, { 10.0f, "10" }, { 15.0f, "15" }, { 20.0f, "20" } }), Knob::intervalMarks, 1));
        addKnob (makeKnob (a, "mix", "MIX", { 1170, cy }, r,
                           marksAt (a, "mix", { { 0.0f, "0" }, { 50.0f, "50" }, { 100.0f, "100" } }), Knob::intervalMarks, 1));
    }
    // ---- PUMP (layout 2) ----
    {
        addKnob (makeKnob (a, "depth", "DEPTH", { 140, cy }, r,
                           marksAt (a, "depth", { { 0.0f, "0" }, { 6.0f, "6" }, { 12.0f, "12" }, { 18.0f, "18" }, { 24.0f, "24" } }), Knob::intervalMarks, 2));
        {
            auto n = makeKnob (a, "pumpNote", "NOTE", { 305, cy }, r,
                               marksAt (a, "pumpNote", { { 0.0f, "1/16" }, { 1.0f, "1/8" }, { 2.0f, "1/4" }, { 3.0f, "1/2" } }), Knob::pointer, 2);
            n->ring = 1.62f; n->markSize = 8.0f; n->stepped = true;
            addKnob (std::move (n));
        }
        {
            auto s = makeKnob (a, "pumpShape", "SHAPE", { 470, cy }, r,
                               marksAt (a, "pumpShape", { { 0.0f, "EXP" }, { 1.0f, "LINEAR" }, { 2.0f, "SWELL" } }), Knob::pointer, 2);
            s->ring = 1.62f; s->markSize = 8.0f; s->stepped = true;
            addKnob (std::move (s));
        }
        addKnob (makeKnob (a, "threshold", "THRESHOLD", { 760, cy }, r,
                           marksAt (a, "threshold", { { -40.0f, "\xe2\x88\x92" "40" }, { -30.0f, "\xe2\x88\x92" "30" }, { -20.0f, "\xe2\x88\x92" "20" }, { -10.0f, "\xe2\x88\x92" "10" }, { 0.0f, "0" } }),
                           Knob::intervalMarks, 2));
        toggles.push_back (makeToggle (a, "ext", { 930, 446 }, 1.0f, 2, "EXT", "SIDECHAIN KEY"));
        addKnob (makeKnob (a, "mix", "MIX", { 1220, cy }, r,
                           marksAt (a, "mix", { { 0.0f, "0" }, { 50.0f, "50" }, { 100.0f, "100" } }), Knob::intervalMarks, 2));
    }
    toggles.push_back (makeToggle (a, "in", { 1450, 146 }, 1.0f, -1, "IN", "OFF = BYPASSED"));

    for (auto& k : knobs)
    {
        k->slider.getProperties().set ("stepped", k->stepped);
        addChildComponent (k->slider);
        addChildComponent (*k->box);
        Knob* kp = k.get();
        kp->slider.onValueChange = [kp] { kp->box->repaint(); };
    }
    for (auto& t : toggles)
        addChildComponent (t->lever);
    addAndMakeVisible (display);
    setSize (TutkalEditor::kW, TutkalEditor::kH);
    showLayout (currentLayout());
}

Knob* FrontPanel::findKnob (const juce::String& id) const
{
    for (auto& k : knobs)
        if (k->id == id && k->slider.isVisible()) return k.get();
    return nullptr;
}

Knob* RearPanel::Plate::findKnob (const juce::String& id) const
{
    for (auto& k : knobs)
        if (k->id == id) return k.get();
    return nullptr;
}

Knob& FrontPanel::addKnob (std::unique_ptr<Knob> k)
{
    knobs.push_back (std::move (k));
    return *knobs.back();
}

int FrontPanel::currentLayout() const
{
    return layoutOfVoicing (juce::jlimit (0, 3, juce::roundToInt (rawOf (proc.apvts, "voicing"))));
}

void FrontPanel::showLayout (int l)
{
    layout = l;
    for (auto& k : knobs)
    {
        const bool on = k->layout < 0 || k->layout == l;
        if (! on && k->box->isEditing()) k->box->endEdit (false);
        k->slider.setVisible (on);
        k->box->setVisible (on);
    }
    for (auto& t : toggles) t->lever.setVisible (t->layout < 0 || t->layout == l);
    repaint();
}

void FrontPanel::resized()
{
    const float bw = 110.7f, bh = 44.0f, gap = 13.0f;
    for (int i = 0; i < 3; ++i)
        placeKey (*voicingKeys[(size_t) i], { 73.0f + (float) i * (bw + gap), 190.0f, bw, bh });
    placeKey (rearKey, { 1406.0f, 246.0f, 88.0f, 34.0f });
    for (auto& k : knobs)
        placeKnob (*k);
    for (auto& t : toggles)
        placeToggle (t->lever, t->centre, t->scale);
    display.setBounds (juce::Rectangle<float> (dX0 - dPad, dY0 - dPad, dX1 - dX0 + 2 * dPad, dY1 - dY0 + 2 * dPad + 4.0f).toNearestInt());
}

void FrontPanel::paint (juce::Graphics& g)
{
    const float W = (float) TutkalEditor::kW, H = (float) TutkalEditor::kH;
    g.drawImage (texture, { 0.0f, 0.0f, W, H }, juce::RectanglePlacement::stretchToFit);

    // Rack ears, slots and screws.
    for (float x0 : { 0.0f, 1556.0f })
    {
        g.setColour (rgb (0, 0, 0, 60));
        g.fillRect (x0, 0.0f, 44.0f, H);
        const float cx = x0 + 22.0f;
        for (float cy : { 62.0f, 578.0f })
        {
            g.setColour (rgb (4, 8, 7, 235));
            g.fillRoundedRectangle (cx - 12.0f, cy - 17.0f, 24.0f, 34.0f, 12.0f);
            screw (g, cx, cy, 8.5f);
        }
    }
    for (float x : { 44.0f, 1556.0f })
    {
        g.setColour (rgb (0, 0, 0, 110));
        g.drawLine (x, 0.0f, x, H, 1.2f);
        g.setColour (rgb (255, 255, 255, 30));
        g.drawLine (x + 1.5f, 0.0f, x + 1.5f, H, 0.8f);
    }

    // Name, model plate, VOICING keys.
    drawProductName (g, "TUTKAL", 72.0f, 38.0f, 38.0f, Theme::ink);
    {
        // The subtitle is fitted to the width of the name.
        const juce::String t = "BUS GLUE " + kDot + " VOCAL LEVELLER " + kDot + " SIDECHAIN PUMP";
        float em = 10.5f;
        while (em > 6.0f && textWidth (t, Theme::legendEm (em), 3.0f * em / 10.5f) > 356.0f) em -= 0.25f;
        silk (g, t, Theme::legendEm (em), 74.0f, 108.0f, 'l', 3.0f * em / 10.5f, Theme::ink);
    }
    {
        const auto f = Theme::legendEm (10.0f);
        const float tw = textWidth ("MODEL TK-79", f, 2.4f);
        g.setColour (Theme::gold);
        g.drawRect (juce::Rectangle<float> (74.0f, 127.0f, tw + 22.0f, 22.0f), 1.3f);
        silk (g, "MODEL TK-79", f, 85.0f, 138.5f, 'l', 2.4f, Theme::gold);
    }
    bracket (g, 72.0f, 430.0f, 172.0f, "VOICING");
    const char* captions[3] { "DRUMS AND MIX BUS", "VOCALS AND BASS", "KEYED BY THE KICK" };
    for (int i = 0; i < 3; ++i)
        silk (g, captions[i], Theme::fineEm (7.2f), 73.0f + (float) i * 123.7f + 55.3f, 252.0f, 'm', 1.0f, Theme::muted);
    {
        const int v = juce::roundToInt (rawOf (proc.apvts, "voicing"));
        silk (g, v == 3 ? "VOICING: CORE (A SAVED SESSION OR THE HOST SET IT)" : "ONE VOICING AT A TIME  " + kDot + "  THE KNOBS BELOW FOLLOW IT",
              Theme::fineEm (7.0f), 74.0f, 284.0f, 'l', 1.0f, Theme::muted);
    }

    groove (g, 318.0f, 64.0f, 1536.0f);
    struct B { int lay; float x0, x1; const char* title; };
    const B brackets[] { { 0, 72, 440, "DETECTOR" }, { 0, 470, 1010, "TIMING" }, { 0, 1040, 1400, "OUT" },
                         { 1, 72, 640, "LEVELLER" }, { 1, 700, 1400, "OUT" },
                         { 2, 72, 640, "PUMP" }, { 2, 670, 1010, "KEY" }, { 2, 1040, 1400, "OUT" } };
    for (const auto& b : brackets)
        if (b.lay == layout)
            bracket (g, b.x0, b.x1, 348.0f, b.title);
    for (auto& k : knobs)
        if (k->layout < 0 || k->layout == layout)
            paintKnobScale (g, *k);
    for (auto& t : toggles)
        if (t->layout < 0 || t->layout == layout)
            toggleLegends (g, *t);

    silk (g, "SET-UP PANEL", Theme::fineEm (7.0f), 1450.0f, 292.0f, 'm', 1.2f, Theme::muted);
    silk (g, "LATENCY", Theme::fineEm (8.0f), 1450.0f, 450.0f, 'm', 1.6f, Theme::ink);
    silk (g, "63 SMP", Theme::fineEm (8.0f), 1450.0f, 463.0f, 'm', 1.6f, Theme::ink);
    silk (g, "ALL RATES", Theme::fineEm (7.0f), 1450.0f, 480.0f, 'm', 1.2f, Theme::muted);

    // Foot of the panel.
    {
        const juce::String t = "TUTKAL " + kDot + " MODEL TK-79 " + kDot + " BUS COMPRESSOR";
        const auto f = Theme::legendEm (9.5f);
        const float tw = textWidth (t, f, 2.6f);
        g.setColour (Theme::gold);
        g.drawRect (juce::Rectangle<float> (800.0f - tw * 0.5f - 18.0f, 597.0f, tw + 36.0f, 26.0f), 1.3f);
        silk (g, t, f, 800.0f, 610.5f, 'm', 2.6f, Theme::gold);
    }
    silk (g, "MEASURED, NOT GUESSED", Theme::fineEm (7.5f), 72.0f, 611.0f, 'l', 1.8f, Theme::muted);
    drawLogo (g, mark, wordmark, 1530.0f, 611.0f, 17.0f, 26.0f);
}

void FrontPanel::tick()
{
    if (const int l = currentLayout(); l != layout)
        showLayout (l);
    display.refresh();
    for (auto& k : voicingKeys) k->repaint();
}

//==============================================================================
RearPanel::Plate::Plate (TutkalAudioProcessor& p, std::function<void()> showFront)
    : proc (p), frontKey ("FRONT", nullptr, 8.2f, 1.6f)
{
    texture = brushedPlate (1200, 440, rgb (16, 52, 49), rgb (6, 32, 30), 11);
    mark = juce::ImageCache::getFromMemory (BinaryData::mark_cream_png, BinaryData::mark_cream_pngSize);
    wordmark = juce::ImageCache::getFromMemory (BinaryData::wordmark_cream_png, BinaryData::wordmark_cream_pngSize);

    auto& a = proc.apvts;
    {
        // SC HPF: AUTO, OFF and 50 .. 300 Hz on one knob (the parameter's own scale).
        auto k = makeKnob (a, "scHpf", "SC HPF", { 250.0f, 214.0f }, 46.0f,
                           marksAt (a, "scHpf", { { 0.0f, "AUTO" }, { 10.0f, "OFF" }, { 50.0f, "50" }, { 100.0f, "100" }, { 200.0f, "200" }, { 300.0f, "300" } }),
                           Knob::intervalMarks, -1, 10.5f);
        k->ring = 1.72f; k->markSize = 7.5f;
        knobs.push_back (std::move (k));
        auto d = makeKnob (a, "drive", "DRIVE", { 940.0f, 214.0f }, 46.0f,
                           marksAt (a, "drive", { { 0.0f, "0" }, { 25.0f, "25" }, { 50.0f, "50" }, { 75.0f, "75" }, { 100.0f, "100" } }),
                           Knob::intervalMarks, -1, 10.5f);
        knobs.push_back (std::move (d));
    }
    toggles.push_back (makeToggle (a, "ext", { 600.0f, 214.0f }, 1.0f, -1, "EXT", "SIDECHAIN KEY"));
    for (auto& k : knobs)
    {
        k->slider.getProperties().set ("stepped", false);
        addAndMakeVisible (k->slider);
        addAndMakeVisible (*k->box);
        Knob* kp = k.get();
        kp->slider.onValueChange = [kp] { kp->box->repaint(); };
    }
    for (auto& t : toggles)
        addAndMakeVisible (t->lever);
    frontKey.onClick = std::move (showFront);
    addAndMakeVisible (frontKey);
    setSize (1200, 440);
}

void RearPanel::Plate::resized()
{
    for (auto& k : knobs)
        placeKnob (*k);
    for (auto& t : toggles)
        placeToggle (t->lever, t->centre, t->scale);
    placeKey (frontKey, { 1030.0f, 382.0f, 84.0f, 30.0f });
}

void RearPanel::Plate::paint (juce::Graphics& g)
{
    g.drawImage (texture, { 0.0f, 0.0f, 1200.0f, 440.0f }, juce::RectanglePlacement::stretchToFit);
    g.setColour (juce::Colours::black);
    g.drawRect (0.0f, 0.0f, 1200.0f, 440.0f, 3.0f);
    for (auto [x, y] : { std::pair<float, float> { 18, 18 }, { 1182, 18 }, { 18, 422 }, { 1182, 422 } })
        screw (g, x, y, 7.5f);

    silk (g, "TUTKAL " + kDot + " MODEL TK-79 " + kDot + " REAR PANEL", Theme::legendEm (11.0f), 44.0f, 40.0f, 'l', 3.0f, Theme::ink);
    silk (g, "SET-UP. THE SIDECHAIN FILTER AND THE KEY SOURCE, AND THE OUTPUT TRANSFORMER.", Theme::fineEm (7.5f), 44.0f, 58.0f, 'l', 1.2f, Theme::muted);
    drawLogo (g, mark, wordmark, 1156.0f, 42.0f, 14.0f, 22.0f);
    groove (g, 76.0f, 30.0f, 1170.0f);

    bracket (g, 44.0f, 700.0f, 104.0f, "SIDECHAIN");
    bracket (g, 740.0f, 1156.0f, 104.0f, "OUTPUT STAGE");
    for (auto& k : knobs)
        paintKnobScale (g, *k);
    for (auto& t : toggles)
        toggleLegends (g, *t);
    silk (g, "AUTO = THE VOICING'S OWN (GLUE 200 HZ, THE OTHERS OFF)", Theme::fineEm (7.0f), 250.0f, 356.0f, 'm', 0.8f, Theme::muted);
    silk (g, "12 dB/OCT, ON THE DETECTOR ONLY", Theme::fineEm (7.0f), 250.0f, 368.0f, 'm', 0.8f, Theme::muted);
    silk (g, "ON: ONLY THE SIDECHAIN BUS IS HEARD", Theme::fineEm (7.0f), 600.0f, 356.0f, 'm', 0.8f, Theme::muted);
    silk (g, "BY THE DETECTOR; OFF: IT IS IGNORED", Theme::fineEm (7.0f), 600.0f, 368.0f, 'm', 0.8f, Theme::muted);
    silk (g, "TRANSFORMER SATURATION, LOW-FREQUENCY WEIGHTED", Theme::fineEm (7.0f), 940.0f, 356.0f, 'm', 0.8f, Theme::muted);
    silk (g, "0 = THE OUTPUT PATH IS BIT-EXACT", Theme::fineEm (7.0f), 940.0f, 368.0f, 'm', 0.8f, Theme::muted);
}

RearPanel::RearPanel (TutkalAudioProcessor& p, std::function<void()> showFront)
    : plate (p, std::move (showFront))
{
    addAndMakeVisible (plate);
    setSize (TutkalEditor::kW, TutkalEditor::kH);
}

void RearPanel::resized()
{
    // The 1200 x 440 plate at the front panel's scale (4/3), centred in the rack opening.
    const float s = (float) TutkalEditor::kW / 1200.0f;
    plate.setBounds (0, 0, 1200, 440);
    plate.setTransform (juce::AffineTransform::scale (s).translated (0.0f, ((float) TutkalEditor::kH - 440.0f * s) * 0.5f));
}

void RearPanel::paint (juce::Graphics& g)
{
    // Behind the plate: the dark rack opening.
    g.fillAll (rgb (3, 8, 7));
}

//==============================================================================
TutkalEditor::TutkalEditor (TutkalAudioProcessor& p)
    : AudioProcessorEditor (&p), proc (p),
      front (p, [this] { showRear (true); }),
      rear (p, [this] { showRear (false); })
{
    // Read the saved width first: resized() writes the live width back into the processor, and setResizeLimits can call it.
    const int savedWidth = juce::jlimit (800, 2400, proc.editorWidth.load());
    setLookAndFeel (&lnf);
    setWantsKeyboardFocus (true);
    addMouseListener (this, true);
    addAndMakeVisible (front);
    addChildComponent (rear);
    setResizable (true, true);
    setResizeLimits (800, 320, 2400, 960);
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio ((double) kW / (double) kH);
    setSize (savedWidth, juce::roundToInt ((float) savedWidth * (float) kH / (float) kW));
    timerCallback();
    startTimerHz (30);
}

TutkalEditor::~TutkalEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

bool TutkalEditor::keyPressed (const juce::KeyPress& key) { return svl::handleUndoKey (proc.undo, key); }

void TutkalEditor::mouseDown (const juce::MouseEvent& e) { svl::takeFocusOnClick (*this, e); }

void TutkalEditor::showRear (bool r)
{
    rear.setVisible (r);
    front.setVisible (! r);
}

void TutkalEditor::paint (juce::Graphics& g)
{
    g.fillAll (rgb (3, 8, 7));
}

void TutkalEditor::resized()
{
    const float s = (float) getWidth() / (float) kW;
    for (juce::Component* c : { static_cast<juce::Component*> (&front), static_cast<juce::Component*> (&rear) })
    {
        c->setBounds (0, 0, kW, kH);
        c->setTransform (juce::AffineTransform::scale (s));
    }
    proc.editorWidth = getWidth();
}

void TutkalEditor::timerCallback()
{
    front.tick();
}
