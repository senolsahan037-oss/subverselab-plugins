#include "PluginEditor.h"
#include "Theme.h"
#include "BinaryData.h"
#include <map>

/*  Geometry: every coordinate below is the one tools/concepts.py used to draw the
    approved face (front 1600 x 640, rear plate 1200 x 440), so the plugin can be
    laid over docs/concepts/koro_A2_front_label7seg.png and koro_rear_plate.png.
*/
namespace
{
using Pt = juce::Point<float>;
constexpr float kPi = juce::MathConstants<float>::pi;

juce::Colour rgb (int r, int g, int b, int a = 255) { return juce::Colour ((juce::uint8) r, (juce::uint8) g, (juce::uint8) b, (juce::uint8) a); }

/** Degrees clockwise from 12 o'clock, as in the concept generator. */
Pt polar (float cx, float cy, float r, float deg)
{
    const float a = deg * kPi / 180.0f;
    return { cx + std::sin (a) * r, cy - std::cos (a) * r };
}

/** Text placed like PIL: anchor 'l', 'm' or 'r' on x, vertically centred on y; track in px per letter. */
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

/** A linear gradient across a box at an angle (degrees, as PIL: 90 = top to bottom, 45 = top-left to bottom-right). */
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

/** The product name in KUBBE's squared, extended stroke letters; K, O and R. */
void drawProductName (juce::Graphics& g, const juce::String& name, float x, float y, float h, juce::Colour ink)
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
            case 'K':
                p.startNewSubPath (L, T - in); p.lineTo (L, B + in);
                p.startNewSubPath (L, M + t * 0.2f); p.lineTo (R, T - in * 0.3f);
                p.startNewSubPath (L + (R - L) * 0.36f, M - h * 0.12f); p.lineTo (R + in * 0.2f, B + in);
                break;
            case 'O':
                p.startNewSubPath ((L + R) * 0.5f, T); p.lineTo (R - r, T); p.quadraticTo (R, T, R, T + r);
                p.lineTo (R, B - r); p.quadraticTo (R, B, R - r, B); p.lineTo (L + r, B); p.quadraticTo (L, B, L, B - r);
                p.lineTo (L, T + r); p.quadraticTo (L, T, L + r, T); p.closeSubPath();
                break;
            case 'R':
            {
                const float rb = r * 0.85f;
                p.startNewSubPath (L, B + in); p.lineTo (L, T);
                p.lineTo (R - rb, T); p.quadraticTo (R, T, R, T + rb); p.lineTo (R, M - rb); p.quadraticTo (R, M, R - rb, M); p.lineTo (L, M);
                p.startNewSubPath (L + (R - L) * 0.46f, M); p.lineTo (R + in * 0.2f, B + in);
                break;
            }
            default: break;
        }
        all.addPath (p);
        cx += w + gap;
    }
    const juce::PathStrokeType stroke (t, juce::PathStrokeType::curved, juce::PathStrokeType::butt);
    // The strokes are turned into an outline and filled (the KASET / TUTKAL way) instead of being
    // stroked by the renderer, so the letters come out the same on Windows (Direct2D) as on macOS.
    juce::Path outline;
    stroke.createStrokedPath (outline, all);
    g.setColour (rgb (0, 0, 0, 95));
    g.fillPath (outline, juce::AffineTransform::translation (0.0f, 1.3f));
    g.setColour (ink);
    g.fillPath (outline);
}

/** Lamp (gold) or LED (red), as lamp() in the concept generator. */
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

/** One 7-segment digit, slanted 7 degrees, as seg_polys() in the concept generator. */
void sevenSegment (juce::Graphics& g, float x, float y, float h, char ch)
{
    static const std::map<char, const char*> lit {
        { 'A', "abcefg" }, { 'b', "fgcde" }, { 'C', "afed" }, { 'd', "bcdeg" }, { 'E', "afged" }, { 'F', "afge" },
        { 'G', "afedc" }, { '-', "g" }, { ' ', "" } };
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
    const auto col = Theme::ink.withAlpha (k.slider.isEnabled() ? 1.0f : 0.4f);
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
        silk (g, k.name, Theme::legendEm (k.legendSize), cx, cy + r * 2.1f, 'm', 2.4f, col);
        return;
    }
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
            const auto p = polar (cx, cy, r * 1.78f, a);
            silk (g, s, Theme::legendEm (7.5f), p.x, p.y, 'm', 0.8f, col);
            continue;
        }
        const float ns = s.length() <= 3 ? juce::jmax (7.0f, r * 0.3f) : juce::jmax (7.0f, r * 0.22f);
        const auto p = polar (cx, cy, r * 1.66f, a);
        silk (g, s, Theme::legendEm (ns), p.x, p.y + (juce::exactlyEqual (a, 0.0f) ? 0.0f : 1.0f), 'm', 0.0f, col);
    }
    const float ly = cy + r * 1.98f;
    silk (g, k.name, Theme::legendEm (k.legendSize), cx, ly, 'm', 2.4f, col);
    if (k.sub.isNotEmpty())
        silk (g, k.sub, Theme::fineEm (k.legendSize * 0.72f), cx, ly + k.legendSize * 1.25f, 'm', 1.5f, Theme::muted);
}

std::unique_ptr<Knob> makeKnob (juce::AudioProcessorValueTreeState& apvts, const char* id, const char* name, Pt centre, float radius,
                                std::vector<std::pair<float, juce::String>> marks, const char* sub = "", float legendSize = 10.5f)
{
    auto k = std::make_unique<Knob>();
    k->id = id; k->name = name; k->sub = sub; k->centre = centre; k->radius = radius; k->marks = std::move (marks); k->legendSize = legendSize;
    k->slider.setSliderStyle (juce::Slider::RotaryVerticalDrag);
    k->slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    k->slider.setRotaryParameters (kPi * 1.25f, kPi * 2.75f, true);
    k->attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, id, k->slider);
    if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (apvts.getParameter (id)))
    {
        k->slider.setDoubleClickReturnValue (true, p->convertFrom0to1 (p->getDefaultValue()));
        k->slider.setTooltip (p->getName (64));
    }
    k->slider.getProperties().set ("radius", radius);
    return k;
}

void placeKnob (Knob& k)
{
    const float side = 2.0f * k.radius + 4.0f;
    k.slider.setBounds (juce::Rectangle<float> (side, side).withCentre (k.centre).toNearestInt());
}

/** Toggle legends that belong to the panel, not the lever. */
void toggleLegends (juce::Graphics& g, float cx, float cy, float s, const juce::String& label, const juce::String& sub)
{
    silk (g, "ON", Theme::fineEm (7.5f * s), cx + 20.0f * s, cy - 26.0f * s, 'm', 0.0f, Theme::ink);
    silk (g, "OFF", Theme::fineEm (7.5f * s), cx + 20.0f * s, cy + 26.0f * s, 'm', 0.0f, Theme::ink);
    if (label.isNotEmpty())
        silk (g, label, Theme::legendEm (10.5f * s), cx, cy + 50.0f * s, 'm', 2.4f, Theme::ink);
    if (sub.isNotEmpty())
        silk (g, sub, Theme::fineEm (7.5f * s), cx, cy + 63.0f * s, 'm', 1.2f, Theme::muted);
}

void placeToggle (BatToggle& t, float cx, float cy, float s)
{
    t.setBounds (juce::Rectangle<float> (36.0f * s, 84.0f * s).withCentre ({ cx, cy }).toNearestInt());
}

void placeKey (juce::Component& k, juce::Rectangle<float> box)
{
    k.setBounds (box.expanded (MoodKey::kMargin).toNearestInt());
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

const char* const kKeyDigit = "CCddEFFGGAAb";                // 7-segment letter per root, sharps on the # LED
const bool kKeySharp[12] { false, true, false, true, false, false, true, false, true, false, true, false };
const char* const kNoteNames[12] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
} // namespace

//==============================================================================
KoroLookAndFeel::KoroLookAndFeel()
{
    // Menus, tooltips and buttons too: nothing on the panel asks the system for a font.
    setDefaultSansSerifTypeface (faces->regular);
    setColour (juce::PopupMenu::backgroundColourId, Theme::panelDeep);
    setColour (juce::PopupMenu::textColourId, Theme::ink);
    setColour (juce::TooltipWindow::backgroundColourId, Theme::panelDeep);
}

void KoroLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                         float startAngle, float endAngle, juce::Slider& s)
{
    const bool stepped = s.getProperties()["stepped"];
    const float r = ((float) juce::jmin (w, h) - 4.0f) * 0.5f;
    const float cx = (float) x + (float) w * 0.5f, cy = (float) y + (float) h * 0.5f;
    const float ang = (startAngle + pos * (endAngle - startAngle)) * 180.0f / kPi;
    const float dim = s.isEnabled() ? 1.0f : 0.45f;

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
        g.setColour (Theme::ink.withMultipliedAlpha (dim));
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
    g.setColour (Theme::gold.withMultipliedAlpha (dim));
    g.drawLine (index, juce::jmax (1.3f, r * 0.08f));
}

//==============================================================================
MoodKey::MoodKey (const juce::String& t, std::function<bool()> isLit, float textSize, float trackPx)
    : juce::Button (t), lit (std::move (isLit)), size (textSize), track (trackPx) {}

void MoodKey::paintButton (juce::Graphics& g, bool over, bool down)
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
void Lamp::paint (juce::Graphics& g)
{
    const auto c = getLocalBounds().toFloat().getCentre();
    drawLamp (g, c.x, c.y, r, lit, Theme::lamp, rgb (70, 52, 22));
}

//==============================================================================
namespace
{
constexpr float dX0 = 480.0f, dY0 = 34.0f, dX1 = 990.0f, dY1 = 296.0f;   // the label window
constexpr float dPad = 10.0f;                                             // component margin for bezel and shadow
}

void LabelDisplay::refresh()
{
    State s;
    auto& u = proc.ui;
    const auto c = proc.targetControls();
    s.hasKey = u.hasKey.load();
    s.manual = u.manual.load();
    s.locked = u.locked.load();
    s.root = u.root.load();
    s.scale = u.scale.load();
    s.dots = s.hasKey ? juce::jlimit (0, 4, juce::roundToInt (u.confidence.load() * 4.0f)) : 0;
    s.harmony = c.harmony > 0.0f;
    s.safe = s.harmony && u.safe.load();

    // COLOR: which of the four intervals the knob position (and the mood's voice count) plays.
    if (s.harmony)
    {
        const float col = juce::jlimit (0.0f, 1.0f, c.color);
        const int vc = juce::jlimit (1, 3, c.harmonyVoices);
        for (int v = 0; v < vc; ++v)
        {
            int a = v;
            float f = col;
            if (vc == 1)
            {
                const float x = col * 3.0f;
                a = juce::jmin (2, (int) std::floor (x));
                f = x - (float) a;
            }
            if (f <= 0.75f) s.colour[(size_t) a] = true;
            if (f >= 0.25f) s.colour[(size_t) a + 1] = true;
        }
    }

    const float base = 100.0f * (float) (s.hasKey ? s.root : 0);
    auto lampOf = [base] (float cents)
    {
        float rel = std::fmod (cents - base, 1200.0f);
        if (rel < 0.0f) rel += 1200.0f;
        return juce::jlimit (0, 47, (int) std::floor (rel / 25.0f));
    };
    s.inOn = u.pitchValid.load();
    s.outOn = s.harmony && u.outValid.load();
    s.inLamp = s.inOn ? lampOf (u.inCents.load()) : -1;
    s.outLamp = s.outOn ? lampOf (u.outCents.load()) : -1;

    if (! (s == st))
    {
        st = s;
        repaint();
    }
}

void LabelDisplay::paint (juce::Graphics& g)
{
    g.addTransform (juce::AffineTransform::translation (dPad - dX0, dPad - dY0));
    const float x0 = dX0, y0 = dY0, x1 = dX1, y1 = dY1;
    const juce::Rectangle<float> win (x0, y0, x1 - x0, y1 - y0);
    const auto ink = Theme::panel, ink2 = Theme::labelInk2;

    softShadow (g, win.expanded (6.0f), 4.0f, 0.6f, { 1.0f, 4.0f }, 10.0f, false);
    g.setGradientFill (grad (win.expanded (7.0f), rgb (30, 33, 32), rgb (4, 5, 5), 90.0f));
    g.fillRoundedRectangle (win.expanded (7.0f), 10.0f);
    g.setGradientFill (grad (win, Theme::label, Theme::labelDeep, 90.0f));
    g.fillRoundedRectangle (win, 4.0f);

    // Headings and the gold rule, set like the KASET cassette label.
    const juce::String dot (juce::CharPointer_UTF8 ("\xc2\xb7"));
    silk (g, "KEY", Theme::legendEm (8.0f), x0 + 16, y0 + 16, 'l', 1.8f, ink);
    silk (g, "SCALE " + dot + " AUTO (READ-OUT)", Theme::legendEm (8.0f), x0 + 108, y0 + 16, 'l', 1.8f, ink);
    silk (g, "CONFIDENCE", Theme::legendEm (8.0f), x0 + 262, y0 + 16, 'l', 1.8f, ink);
    silk (g, "SUBVERSELAB", Theme::legendEm (7.5f), x1 - 16, y0 + 16, 'r', 1.8f, ink);
    g.setColour (Theme::gold);
    g.fillRect (x0 + 12, y0 + 27, x1 - x0 - 24, 3.0f);

    // KEY: red filter window, one 7-segment digit and the sharp LED.
    const float wx0 = x0 + 14, wy0 = y0 + 40, wx1 = x0 + 92, wy1 = y0 + 124;
    g.setColour (rgb (12, 12, 12));
    g.fillRoundedRectangle (juce::Rectangle<float> (wx0 - 2, wy0 - 2, wx1 - wx0 + 4, wy1 - wy0 + 4), 3.0f);
    g.setColour (Theme::redWindow);
    g.fillRoundedRectangle (juce::Rectangle<float> (wx0, wy0, wx1 - wx0, wy1 - wy0), 2.0f);
    sevenSegment (g, wx0 + 12, wy0 + 10, 64.0f, st.hasKey ? kKeyDigit[st.root] : '-');
    redLed (g, wx1 - 9, wy0 + 11, 2.4f, st.hasKey && kKeySharp[st.root]);
    silk (g, "#", Theme::legendEm (7.0f), wx1 - 9, wy0 + 22, 'm', 0.0f, rgb (190, 120, 110));

    // SCALE: printed names, one LED lit (7 segments cannot spell MAJOR or NIHAVENT).
    const char* scales[5] { "MAJOR", "MINOR", "NIHAVENT", "HICAZ", "RAST" };
    for (int i = 0; i < 5; ++i)
    {
        const float yy = y0 + 46 + (float) i * 17;
        const bool on = st.hasKey && st.scale == i;
        redLed (g, x0 + 114, yy, 3.0f, on);
        silk (g, scales[i], Theme::legendEm (8.5f), x0 + 126, yy + 0.5f, 'l', 1.4f, on ? ink : ink2);
    }
    {
        const float fy = y0 + 46 + 5 * 17 - 4;
        const auto f = Theme::fineEm (6.8f);
        silk (g, "TO OVERRIDE: REAR", f, x0 + 114, fy, 'l', 0.8f, ink2);
        const float tx = x0 + 114 + textWidth ("TO OVERRIDE: REAR", f, 0.8f) + 4.0f;
        juce::Path tri;
        tri.addTriangle (tx, fy - 3.0f, tx, fy + 3.0f, tx + 4.5f, fy);
        g.setColour (ink2);
        g.fillPath (tri);
        silk (g, "KEY / SCALE", f, tx + 8.5f, fy, 'l', 0.8f, ink2);
    }

    // CONFIDENCE: four LEDs, AUTO / LOCK.
    for (int i = 0; i < 4; ++i)
        redLed (g, x0 + 272 + (float) i * 30, y0 + 58, 5.5f, i < st.dots);
    const bool autoOn = ! st.manual;
    redLed (g, x0 + 272, y0 + 100, 3.2f, autoOn);
    silk (g, "AUTO", Theme::legendEm (8.0f), x0 + 282, y0 + 100.5f, 'l', 1.4f, autoOn ? ink : ink2);
    redLed (g, x0 + 336, y0 + 100, 3.2f, st.locked);
    silk (g, "LOCK", Theme::legendEm (8.0f), x0 + 346, y0 + 100.5f, 'l', 1.4f, st.locked ? ink : ink2);

    // HARMONY and its latency (the plugin reports 54 ms at all times; the lamp says why).
    redLed (g, x1 - 92, y0 + 58, 3.2f, st.harmony);
    silk (g, "HARMONY", Theme::legendEm (8.0f), x1 - 82, y0 + 58.5f, 'l', 1.2f, st.harmony ? ink : ink2);
    silk (g, "LATENCY 54 MS", Theme::fineEm (7.0f), x1 - 82, y0 + 72, 'l', 0.8f, ink2);
    g.setColour (Theme::gold.withAlpha (200.0f / 255.0f));
    g.fillRect (x0 + 12, y0 + 134, x1 - x0 - 24, 1.2f);

    // COLOR row.
    silk (g, "COLOR", Theme::legendEm (8.0f), x0 + 16, y0 + 154, 'l', 1.8f, ink);
    const char* colours[4] { "LOW 3RD", "HIGH 3RD", "5TH", "OCT" };
    for (int i = 0; i < 4; ++i)
    {
        const float cx = x0 + 88 + (float) i * 82;
        redLed (g, cx, y0 + 154, 3.2f, st.colour[(size_t) i]);
        silk (g, colours[i], Theme::legendEm (8.0f), cx + 9, y0 + 154.5f, 'l', 1.2f, st.colour[(size_t) i] ? ink : ink2);
    }
    redLed (g, x1 - 92, y0 + 154, 3.2f, st.safe);
    silk (g, "SAFE UNISON", Theme::legendEm (7.5f), x1 - 82, y0 + 154.5f, 'l', 0.8f, st.safe ? ink : ink2);

    // PITCH: 48 lamps over one octave from the detected key, 25 cents per lamp.
    const float bx0 = x0 + 64, bx1 = x1 - 24, by = y0 + 206;
    silk (g, "PITCH", Theme::legendEm (8.0f), x0 + 16, by, 'l', 1.8f, ink);
    g.setColour (Theme::redWindow);
    g.fillRoundedRectangle (juce::Rectangle<float> (bx0 - 5, by - 8, bx1 - bx0 + 10, 16), 2.0f);
    const float cw = (bx1 - bx0) / 48.0f;
    for (int i = 0; i < 48; ++i)
    {
        const juce::Rectangle<float> seg (bx0 + (float) i * cw + 0.8f, by - 4.5f, cw - 1.6f, 9.0f);
        const bool on = i == st.inLamp || i == st.outLamp;
        if (on)
        {
            glow (g, seg.getCentre(), cw * 1.6f, Theme::ledRed, 0.5f);
            g.setColour (Theme::ledRed);
        }
        else
            g.setColour (st.inLamp >= 0 && std::abs (i - st.inLamp) <= 1 ? Theme::ledRedDim : Theme::ledRedOff);
        g.fillRect (seg);
    }
    auto xOf = [&] (float cents) { return bx0 + (bx1 - bx0) * cents / 1200.0f; };
    if (st.hasKey)
    {
        const int n = cifte::scales::count (st.scale);
        for (int i = 0; i <= n; ++i)
        {
            const int cents = i == n ? 1200 : cifte::scales::cents (st.scale, i);
            const float xx = xOf ((float) cents);
            g.setColour (ink);
            g.drawLine (xx, by + 9, xx, by + 14, 1.0f);
            const int note = (st.root + juce::roundToInt ((float) cents / 100.0f)) % 12;
            silk (g, kNoteNames[note], Theme::legendEm (7.5f), xx, by + 22, 'm', 0.0f, ink);
        }
    }
    if (st.inLamp >= 0)
        silk (g, "IN", Theme::legendEm (7.0f), bx0 + ((float) st.inLamp + 0.5f) * cw, by - 16, 'm', 0.0f, ink);
    if (st.outLamp >= 0)
        silk (g, "OUT", Theme::legendEm (7.0f), bx0 + ((float) st.outLamp + 0.5f) * cw, by - 16, 'm', 0.0f, ink);
    silk (g, "ONE OCTAVE FROM THE DETECTED KEY  " + dot + "  25 CENTS PER LAMP", Theme::fineEm (7.0f), x0 + 16, y1 - 14, 'l', 0.8f, ink2);
}

//==============================================================================
namespace
{
constexpr float vX0 = 1036.0f, vY0 = 50.0f, vX1 = 1336.0f, vY1 = 246.0f;
const std::pair<float, float> kVuMarks[] { { -20.0f, 0.0f }, { -10.0f, 0.2f }, { -7.0f, 0.31f }, { -5.0f, 0.41f }, { -3.0f, 0.53f }, { -2.0f, 0.60f },
                                           { -1.0f, 0.68f }, { 0.0f, 0.76f }, { 1.0f, 0.84f }, { 2.0f, 0.92f }, { 3.0f, 1.0f } };
float vuFraction (float db)
{
    if (db <= kVuMarks[0].first) return juce::jmax (-0.04f, (db + 20.0f) * 0.02f * 0.1f);
    for (size_t i = 1; i < std::size (kVuMarks); ++i)
        if (db <= kVuMarks[i].first)
        {
            const auto [d0, f0] = kVuMarks[i - 1];
            const auto [d1, f1] = kVuMarks[i];
            return f0 + (f1 - f0) * (db - d0) / (d1 - d0);
        }
    return juce::jmin (1.06f, 1.0f + (db - 3.0f) * 0.02f);
}
}

void VuMeter::setLevels (float rmsL, float rmsR)
{
    bool moved = false;
    const float in[2] { rmsL, rmsR };
    for (int c = 0; c < 2; ++c)
    {
        // 0 VU = -18 dBFS RMS; about 300 ms of needle integration at the 30 Hz UI rate.
        const float target = juce::Decibels::gainToDecibels (in[c], -60.0f) + 18.0f;
        const float next = vu[c] + (target - vu[c]) * 0.22f;
        if (std::abs (next - vu[c]) > 0.02f) moved = true;
        vu[c] = next;
    }
    if (moved) repaint();
}

void VuMeter::paint (juce::Graphics& g)
{
    g.addTransform (juce::AffineTransform::translation (dPad - vX0, dPad - vY0));
    const juce::Rectangle<float> box (vX0, vY0, vX1 - vX0, vY1 - vY0);
    const float hw = box.getWidth(), hh = box.getHeight();
    softShadow (g, box.expanded (6.0f), 4.0f, 0.6f, { 1.0f, 4.0f }, 8.0f, false);
    g.setGradientFill (grad (box.expanded (7.0f), rgb (30, 33, 32), rgb (4, 5, 5), 90.0f));
    g.fillRoundedRectangle (box.expanded (7.0f), 8.0f);
    g.setGradientFill (grad (box, rgb (247, 241, 224), rgb (224, 214, 188), 90.0f));
    g.fillRoundedRectangle (box, 2.0f);

    const float cx = (vX0 + vX1) * 0.5f, py = vY1 - hh * 0.08f, R = hh * 0.66f, span = 47.0f;
    auto ang = [span] (float fr) { return -span + 2.0f * span * fr; };
    const auto dark = rgb (32, 32, 30), red = rgb (190, 55, 38);
    auto arc = [&] (float f0, float f1, float radius, float width, juce::Colour c)
    {
        juce::Path p;
        p.addCentredArc (cx, py, radius, radius, 0.0f, ang (f0) * kPi / 180.0f, ang (f1) * kPi / 180.0f, true);
        g.setColour (c);
        g.strokePath (p, juce::PathStrokeType (width));
    };
    g.saveState();
    g.reduceClipRegion (box.toNearestInt());
    arc (0.0f, 0.76f, R, 1.2f, dark);
    arc (0.76f, 1.0f, R + 1.5f, 4.0f, red);
    for (const auto& [db, fr] : kVuMarks)
    {
        const float a = ang (fr);
        const auto c = db > 0.0f ? red : dark;
        const int d = juce::roundToInt (db);
        const bool major = d == -20 || d == -10 || d == -7 || d == -5 || d == -3 || d == 0 || d == 3;
        g.setColour (c);
        g.drawLine ({ polar (cx, py, R, a), polar (cx, py, R + (major ? 8.0f : 5.0f), a) }, major ? 1.2f : 0.9f);
        const auto t = polar (cx, py, R + 17.0f, a);
        silk (g, juce::String ((int) std::abs (db)), Theme::legendEm (7.5f), t.x, t.y, 'm', 0.0f, c);
    }
    silk (g, "VU", Theme::legendEm (13.0f), vX0 + hw * 0.2f, py - R * 0.34f, 'm', 2.0f, dark);
    silk (g, juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92")), Theme::legendEm (11.0f), cx - R * 0.95f, py - R * 0.28f, 'm', 0.0f, dark);
    silk (g, "+", Theme::legendEm (11.0f), cx + R * 0.95f, py - R * 0.28f, 'm', 0.0f, red);

    const juce::Colour needle[2] { rgb (20, 20, 20), rgb (170, 40, 30) };
    for (int c = 0; c < 2; ++c)
    {
        g.setColour (rgb (0, 0, 0, 40));
        g.drawLine ({ Pt (cx + 1.5f, py + 2.0f), polar (cx + 1.5f, py + 2.0f, R + 6.0f, ang (vuFraction (vu[c]))) }, 1.6f);
    }
    for (int c = 0; c < 2; ++c)
    {
        g.setColour (needle[c]);
        g.drawLine ({ Pt (cx, py), polar (cx, py, R + 6.0f, ang (vuFraction (vu[c]))) }, 1.2f);
    }
    const float hy = vY1 - hh * 0.14f, my = (hy + vY1) * 0.5f;
    g.setColour (rgb (22, 24, 23));
    g.fillRect (vX0, hy, hw, vY1 - hy);
    g.setColour (rgb (0, 0, 0, 220));
    g.drawLine (vX0, hy, vX1, hy, 0.8f);
    g.setColour (rgb (230, 226, 214));
    g.drawLine (vX0 + 8.0f, my, vX0 + 26.0f, my, 1.6f);
    silk (g, "L", Theme::legendEm (8.0f), vX0 + 32.0f, my + 0.5f, 'm', 0.0f, Theme::ink);
    g.setColour (needle[1]);
    g.drawLine (vX1 - 26.0f, my, vX1 - 8.0f, my, 1.6f);
    silk (g, "R", Theme::legendEm (8.0f), vX1 - 32.0f, my + 0.5f, 'm', 0.0f, Theme::ink);
    juce::Path sheen;
    sheen.addQuadrilateral (vX0, vY0, vX0 + hw * 0.42f, vY0, vX0 + hw * 0.18f, hy, vX0, hy);
    g.setColour (rgb (255, 255, 255, 20));
    g.fillPath (sheen);
    g.restoreState();
}

//==============================================================================
FrontPanel::FrontPanel (KoroProcessor& p, std::function<void()> showRear)
    : proc (p),
      rearKey ("REAR", nullptr, 8.2f, 1.6f),
      display (p)
{
    texture = brushedPlate (KoroEditor::kW, KoroEditor::kH, rgb (26, 70, 66), rgb (9, 42, 39), 77);
    mark = juce::ImageCache::getFromMemory (BinaryData::mark_cream_png, BinaryData::mark_cream_pngSize);
    wordmark = juce::ImageCache::getFromMemory (BinaryData::wordmark_cream_png, BinaryData::wordmark_cream_pngSize);

    for (int i = 0; i < 6; ++i)
    {
        auto k = std::make_unique<MoodKey> (KoroProcessor::moodNames[i], [this, i] { return (int) proc.apvts.getRawParameterValue ("mood")->load() == i; });
        k->onClick = [this, i] { proc.applyMood (i); };
        k->setTooltip ("Mood: sets DOUBLE, WIDTH, TONE, HARMONY, COLOR and the rear plate");
        addAndMakeVisible (*k);
        moods.push_back (std::move (k));
    }
    rearKey.onClick = std::move (showRear);
    addAndMakeVisible (rearKey);

    const float cy = 450.0f, r = 41.0f;
    using M = std::vector<std::pair<float, juce::String>>;
    const juce::String minus12 = juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92")) + "12";
    knobs.push_back (makeKnob (proc.apvts, "double", "DOUBLE", { 140, cy }, r, M { { -135.0f, "0" }, { 0.0f, "5" }, { 135.0f, "10" } }, "0 = OFF"));
    knobs.push_back (makeKnob (proc.apvts, "width", "WIDTH", { 305, cy }, r, M { { -135.0f, "0" }, { 0.0f, "5" }, { 135.0f, "10" } }));
    knobs.push_back (makeKnob (proc.apvts, "tone", "TONE", { 470, cy }, r, M { { -135.0f, "DARK" }, { 0.0f, "" }, { 135.0f, "BRIGHT" } }));
    knobs.push_back (makeKnob (proc.apvts, "harmony", "HARMONY", { 698, cy }, r, M { { -135.0f, "0" }, { 0.0f, "5" }, { 135.0f, "10" } }, "0 = OFF"));
    knobs.push_back (makeKnob (proc.apvts, "color", "COLOR", { 890, cy }, r,
                               M { { -135.0f, "LOW 3RD" }, { -45.0f, "HIGH 3RD" }, { 45.0f, "5TH" }, { 135.0f, "OCT" } }, "INTERVAL"));
    knobs.back()->style = Knob::intervalMarks;
    knobs.push_back (makeKnob (proc.apvts, "output", "OUTPUT", { 1150, cy }, r, M { { -135.0f, minus12 }, { 0.0f, "0" }, { 135.0f, "+12" } }, "dB"));
    for (auto& k : knobs)
        addAndMakeVisible (k->slider);

    addAndMakeVisible (lockToggle);
    addAndMakeVisible (bypassToggle);
    lockToggle.setTooltip ("LOCK holds the detected key");
    bypassToggle.setTooltip ("BYPASS: the dry signal, still 54 ms late so nothing jumps");
    lockAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "lock", lockToggle);
    bypassAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "bypass", bypassToggle);
    addAndMakeVisible (lockLamp);
    addAndMakeVisible (latencyLamp);
    addAndMakeVisible (display);
    addAndMakeVisible (vu);
    setSize (KoroEditor::kW, KoroEditor::kH);
}

void FrontPanel::resized()
{
    const float bw = 110.7f, bh = 38.0f, gap = 13.0f;
    for (int i = 0; i < 6; ++i)
    {
        const int col = i % 3, row = i / 3;
        placeKey (*moods[(size_t) i], { 73.0f + (float) col * (bw + gap), 190.0f + (float) row * (bh + 14.0f), bw, bh });
    }
    placeKey (rearKey, { 1404.0f, 246.0f, 88.0f, 34.0f });
    for (auto& k : knobs)
        placeKnob (*k);
    placeToggle (lockToggle, 1448.0f, 146.0f, 1.0f);
    placeToggle (bypassToggle, 1300.0f, 446.0f, 1.0f);
    lockLamp.setBounds (juce::Rectangle<int> (40, 40).withCentre ({ 1448, 66 }));
    latencyLamp.setBounds (juce::Rectangle<int> (40, 40).withCentre ({ 1448, 426 }));
    display.setBounds (juce::Rectangle<float> (dX0 - dPad, dY0 - dPad, dX1 - dX0 + 2 * dPad, dY1 - dY0 + 2 * dPad + 4.0f).toNearestInt());
    vu.setBounds (juce::Rectangle<float> (vX0 - dPad, vY0 - dPad, vX1 - vX0 + 2 * dPad, vY1 - vY0 + 2 * dPad + 4.0f).toNearestInt());
}

void FrontPanel::paint (juce::Graphics& g)
{
    const float W = (float) KoroEditor::kW, H = (float) KoroEditor::kH;
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

    // Name, model plate, MOOD keys.
    const juce::String dot (juce::CharPointer_UTF8 ("\xc2\xb7"));
    drawProductName (g, "KORO", 72.0f, 34.0f, 50.0f, Theme::ink);
    silk (g, "VOCAL DOUBLER " + dot + " HARMONIZER", Theme::legendEm (10.5f), 74.0f, 110.0f, 'l', 3.0f, Theme::ink);
    {
        const auto f = Theme::legendEm (10.0f);
        const float tw = textWidth ("MODEL KR-77", f, 2.4f);
        g.setColour (Theme::gold);
        g.drawRect (juce::Rectangle<float> (74.0f, 127.0f, tw + 22.0f, 22.0f), 1.3f);
        silk (g, "MODEL KR-77", f, 85.0f, 138.5f, 'l', 2.4f, Theme::gold);
    }
    silk (g, "MOODS", Theme::legendEm (7.5f), 430.0f, 138.0f, 'r', 2.0f, Theme::ink);
    bracket (g, 72.0f, 430.0f, 172.0f, "MOOD");

    groove (g, 318.0f, 64.0f, 1536.0f);
    bracket (g, 72.0f, 540.0f, 348.0f, "DOUBLE");
    bracket (g, 598.0f, 992.0f, 348.0f, "HARMONY");
    bracket (g, 1052.0f, 1536.0f, 348.0f, "OUT");
    for (auto& k : knobs)
        paintKnobScale (g, *k);

    toggleLegends (g, 1448.0f, 146.0f, 1.0f, "LOCK", "HOLDS THE KEY");
    toggleLegends (g, 1300.0f, 446.0f, 1.0f, "BYPASS", {});
    silk (g, "SET-UP PANEL", Theme::fineEm (7.0f), 1448.0f, 292.0f, 'm', 1.2f, Theme::muted);
    silk (g, "LATENCY", Theme::fineEm (8.0f), 1448.0f, 450.0f, 'm', 1.6f, Theme::ink);
    silk (g, "54 MS", Theme::fineEm (8.0f), 1448.0f, 463.0f, 'm', 1.6f, Theme::ink);
    silk (g, "HARMONY UP", Theme::fineEm (7.0f), 1448.0f, 480.0f, 'm', 1.2f, Theme::muted);

    // Foot of the panel.
    {
        const juce::String t = "KORO " + dot + " MODEL KR-77 " + dot + " VOCAL DOUBLER " + dot + " HARMONIZER";
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
    display.refresh();
    vu.setLevels (proc.ui.rmsL.load(), proc.ui.rmsR.load());
    lockLamp.setOn (proc.ui.locked.load());
    latencyLamp.setOn (proc.apvts.getRawParameterValue ("harmony")->load() > 0.0f);
    const int mood = (int) proc.apvts.getRawParameterValue ("mood")->load();
    if (mood != lastMood)
    {
        lastMood = mood;
        for (auto& m : moods) m->repaint();
    }
}

//==============================================================================
RearPanel::Plate::Plate (KoroProcessor& p, std::function<void()> showFront)
    : proc (p), frontKey ("FRONT", nullptr, 8.2f, 1.6f)
{
    texture = brushedPlate (1200, 440, rgb (16, 52, 49), rgb (6, 32, 30), 11);
    mark = juce::ImageCache::getFromMemory (BinaryData::mark_cream_png, BinaryData::mark_cream_pngSize);
    wordmark = juce::ImageCache::getFromMemory (BinaryData::wordmark_cream_png, BinaryData::wordmark_cream_pngSize);

    using M = std::vector<std::pair<float, juce::String>>;
    const char* levels[3] { "level1", "level2", "level3" };
    const char* pans[3] { "pan1", "pan2", "pan3" };
    const float xs[3] { 130.0f, 300.0f, 470.0f };
    for (int i = 0; i < 3; ++i)
    {
        knobs.push_back (makeKnob (proc.apvts, levels[i], "LEVEL", { xs[i], 184.0f }, 24.0f, M { { -135.0f, "0" }, { 0.0f, "5" }, { 135.0f, "10" } }, "", 9.0f));
        knobs.push_back (makeKnob (proc.apvts, pans[i], "PAN", { xs[i], 318.0f }, 24.0f, M { { -135.0f, "L" }, { 0.0f, "C" }, { 135.0f, "R" } }, "", 9.0f));
    }
    knobs.push_back (makeKnob (proc.apvts, "glide", "GLIDE", { 690.0f, 184.0f }, 24.0f, M { { -135.0f, "0" }, { 0.0f, "" }, { 135.0f, "120" } }, "MS", 9.0f));
    knobs.push_back (makeKnob (proc.apvts, "timing", "TIMING", { 690.0f, 318.0f }, 24.0f, M { { -135.0f, "0" }, { 0.0f, "" }, { 135.0f, "30" } }, "MS", 9.0f));

    // KEY: AUTO, C .. B over 300 degrees; SCALE / MAKAM: seven positions over 270.
    {
        M marks;
        for (int i = 0; i < 13; ++i)
            marks.push_back ({ -150.0f + (float) i * 25.0f, KoroProcessor::keyNames[i] });
        auto k = makeKnob (proc.apvts, "keyOverride", "KEY", { 910.0f, 196.0f }, 25.0f, marks, "", 9.0f);
        k->style = Knob::pointer; k->ring = 1.9f; k->markSize = 6.8f; k->stepped = true;
        k->slider.setRotaryParameters (kPi * (2.0f - 150.0f / 180.0f), kPi * (2.0f + 150.0f / 180.0f), true);
        knobs.push_back (std::move (k));
    }
    {
        M marks;
        const char* names[7] { "AUTO", "MAJ", "MIN", "NIHAV", "HICAZ", "RAST", "CHROM" };
        for (int i = 0; i < 7; ++i)
            marks.push_back ({ -135.0f + (float) i * 45.0f, names[i] });
        auto k = makeKnob (proc.apvts, "scaleOverride", "SCALE / MAKAM", { 1072.0f, 196.0f }, 25.0f, marks, "", 9.0f);
        k->style = Knob::pointer; k->ring = 2.05f; k->markSize = 6.8f; k->stepped = true;
        knobs.push_back (std::move (k));
    }
    for (auto& k : knobs)
    {
        k->slider.getProperties().set ("stepped", k->stepped);
        addAndMakeVisible (k->slider);
    }

    addAndMakeVisible (monoSafe);
    monoSafe.setTooltip ("MONO SAFE keeps the doubler's voice delays under 12 ms");
    monoSafeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "monoSafe", monoSafe);
    frontKey.onClick = std::move (showFront);
    addAndMakeVisible (frontKey);
    setSize (1200, 440);
}

void RearPanel::Plate::resized()
{
    for (auto& k : knobs)
        placeKnob (*k);
    placeToggle (monoSafe, 910.0f, 342.0f, 0.8f);
    placeKey (frontKey, { 1030.0f, 382.0f, 84.0f, 30.0f });
}

void RearPanel::Plate::paint (juce::Graphics& g)
{
    g.drawImage (texture, { 0.0f, 0.0f, 1200.0f, 440.0f }, juce::RectanglePlacement::stretchToFit);
    g.setColour (juce::Colours::black);
    g.drawRect (0.0f, 0.0f, 1200.0f, 440.0f, 3.0f);
    for (auto [x, y] : { std::pair<float, float> { 18, 18 }, { 1182, 18 }, { 18, 422 }, { 1182, 422 } })
        screw (g, x, y, 7.5f);

    const juce::String dot (juce::CharPointer_UTF8 ("\xc2\xb7"));
    silk (g, "KORO " + dot + " MODEL KR-77 " + dot + " REAR PANEL", Theme::legendEm (11.0f), 44.0f, 40.0f, 'l', 3.0f, Theme::ink);
    silk (g, "SET-UP. THE MOOD ON THE FRONT WRITES THESE; CHANGE THEM ONLY TO OVERRIDE.", Theme::fineEm (7.5f), 44.0f, 58.0f, 'l', 1.2f, Theme::muted);
    drawLogo (g, mark, wordmark, 1156.0f, 42.0f, 14.0f, 22.0f);
    groove (g, 76.0f, 30.0f, 1170.0f);

    bracket (g, 44.0f, 560.0f, 104.0f, "HARMONY VOICES");
    for (int i = 0; i < 3; ++i)
        silk (g, "VOICE " + juce::String (i + 1), Theme::fineEm (8.0f), 130.0f + 170.0f * (float) i, 128.0f, 'm', 1.8f, Theme::muted);
    bracket (g, 600.0f, 780.0f, 104.0f, "MOTION");
    bracket (g, 820.0f, 1156.0f, 104.0f, "KEY OVERRIDE");
    bracket (g, 820.0f, 1156.0f, 286.0f, "DOUBLE");
    for (auto& k : knobs)
        paintKnobScale (g, *k);

    toggleLegends (g, 910.0f, 342.0f, 0.8f, {}, {});
    silk (g, "MONO SAFE", Theme::legendEm (9.0f), 910.0f, 390.0f, 'm', 2.0f, Theme::ink);
    silk (g, "VOICE DELAYS", Theme::fineEm (7.0f), 1072.0f, 332.0f, 'm', 1.0f, Theme::muted);
    silk (g, "UNDER 12 MS FOR", Theme::fineEm (7.0f), 1072.0f, 344.0f, 'm', 1.0f, Theme::muted);
    silk (g, "SAFER MONO SUMS", Theme::fineEm (7.0f), 1072.0f, 356.0f, 'm', 1.0f, Theme::muted);
}

RearPanel::RearPanel (KoroProcessor& p, std::function<void()> showFront)
    : plate (p, std::move (showFront))
{
    addAndMakeVisible (plate);
    setSize (KoroEditor::kW, KoroEditor::kH);
}

void RearPanel::resized()
{
    // The 1200 x 440 plate at the front panel's scale (4/3), centred in the rack opening.
    const float s = (float) KoroEditor::kW / 1200.0f;
    plate.setBounds (0, 0, 1200, 440);
    plate.setTransform (juce::AffineTransform::scale (s).translated (0.0f, ((float) KoroEditor::kH - 440.0f * s) * 0.5f));
}

void RearPanel::paint (juce::Graphics& g)
{
    // Behind the plate: the dark rack opening.
    g.fillAll (rgb (3, 8, 7));
}

//==============================================================================
KoroEditor::KoroEditor (KoroProcessor& p)
    : AudioProcessorEditor (&p), proc (p),
      front (p, [this] { showRear (true); }),
      rear (p, [this] { showRear (false); })
#if SVL_LICENSING
    , licenseOverlay (p.license)
#endif
{
    setLookAndFeel (&lnf);
    setWantsKeyboardFocus (true);
    addMouseListener (this, true);
    addAndMakeVisible (front);
    addChildComponent (rear);
#if SVL_LICENSING
    licenseButton.setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    licenseButton.setColour (juce::TextButton::textColourOffId, Theme::ink.withAlpha (0.6f));
    licenseButton.onClick = [this] { licenseOverlay.refresh(); licenseOverlay.setVisible (true); licenseOverlay.toFront (true); };
    addAndMakeVisible (licenseButton);
    addChildComponent (licenseOverlay);
    licenseOverlay.setVisible (! proc.license.isLicensed());
#endif
    setResizable (true, true);
    setResizeLimits (800, 320, 2400, 960);
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio ((double) kW / (double) kH);
    const int w = juce::jlimit (800, 2400, proc.editorWidth.load());
    setSize (w, juce::roundToInt ((float) w * (float) kH / (float) kW));
    timerCallback();
    startTimerHz (30);
}

bool KoroEditor::keyPressed (const juce::KeyPress& key)
{
#if SVL_LICENSING
    if (licenseOverlay.isVisible())
        return false;
#endif
    return svl::handleUndoKey (proc.undo, key);
}

void KoroEditor::mouseDown (const juce::MouseEvent& e)
{
#if SVL_LICENSING
    if (licenseOverlay.isVisible())
        return;
#endif
    svl::takeFocusOnClick (*this, e);
}

KoroEditor::~KoroEditor()
{
    setLookAndFeel (nullptr);
}

void KoroEditor::showRear (bool r)
{
    rear.setVisible (r);
    front.setVisible (! r);
}

void KoroEditor::paint (juce::Graphics& g)
{
    g.fillAll (rgb (3, 8, 7));
}

void KoroEditor::resized()
{
    const float s = (float) getWidth() / (float) kW;
    for (juce::Component* c : { static_cast<juce::Component*> (&front), static_cast<juce::Component*> (&rear) })
    {
        c->setBounds (0, 0, kW, kH);
        c->setTransform (juce::AffineTransform::scale (s));
    }
    proc.editorWidth = getWidth();
#if SVL_LICENSING
    licenseOverlay.setBounds (getLocalBounds());
    licenseButton.setBounds (getWidth() / 2 + juce::roundToInt (260 * s), getHeight() - juce::roundToInt (26 * s), 70, 20);
#endif
}

void KoroEditor::timerCallback()
{
#if SVL_LICENSING
    if (! proc.license.isLicensed() && ! licenseOverlay.isVisible())
    {
        licenseOverlay.refresh();
        licenseOverlay.setVisible (true);
    }
#endif
    front.tick();
}
