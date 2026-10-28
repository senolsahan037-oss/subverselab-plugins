#pragma once

#include <JuceHeader.h>
#include <map>
#include "Theme.h"

/*  The SubverseLab family's own drawing code, copied (not re-invented) for the DYNEQ / ADALET panel and, copied again, for SEDA (P4, 10 Oct; letter S added to familyName) (owner, 10 Oct: "stay as close as possible to the family's
    analog design, especially the buttons").  Sources:
      TUTKAL  subverselab-tutkal/Source/PluginEditor.cpp : rgb, polar, silk, textWidth, glow, grad, softShadow, brushedPlate, screw, groove, drawLamp / redLed,
                                                           drawLogo, PlateKey::paintButton, BatToggle::paintButton, drawRotarySlider (knob), paintKnobScale (ticks),
                                                           ValueBox::paint (the value window), the gold model badge and footer plaque, drawProductName (T)
      ALEV    subverselab-kor/Source/PluginEditor.cpp    : familyName, annulus, litFrom, buildCurvedText, the three brushed-steel rings, the black-steel pointer,
                                                           paintPushers (collar, stem, steel cap), the glass dome sheen (paintEmber)
    Only what a different geometry forced was changed: ALEV's constants kCx / kCy / kUnitR became arguments.  Everything here is drawn, no bitmaps except the logo. */
namespace fam
{
using Pt = juce::Point<float>;
constexpr float kPi = juce::MathConstants<float>::pi;

inline juce::Colour rgb (int r, int g, int b, int a = 255) { return juce::Colour ((juce::uint8) r, (juce::uint8) g, (juce::uint8) b, (juce::uint8) a); }

/** Degrees clockwise from 12 o'clock. */
inline Pt polar (float cx, float cy, float r, float deg)
{
    const float a = deg * kPi / 180.0f;
    return { cx + std::sin (a) * r, cy - std::cos (a) * r };
}

/** Text anchored 'l', 'm' or 'r' on x, vertically centred on y; track in px per letter. */
inline void silk (juce::Graphics& g, const juce::String& s, juce::Font f, float x, float y, char h, float track, juce::Colour c)
{
    if (track > 0.0f)
        f = f.withExtraKerningFactor (track / f.getHeight());
    g.setFont (f);
    g.setColour (c);
    const float w = juce::GlyphArrangement::getStringWidth (f, s) + 40.0f, hh = f.getHeight() + 4.0f;
    const float x0 = h == 'l' ? x : h == 'm' ? x - w * 0.5f : x - w;
    const auto j = h == 'l' ? juce::Justification::centredLeft : h == 'm' ? juce::Justification::centred : juce::Justification::centredRight;
    const float shift = h == 'm' ? track * 0.5f : h == 'r' ? track : 0.0f;   // a trailing letter space would push centred and right-aligned text left
    g.drawText (s, juce::Rectangle<float> (x0 + shift, y - hh * 0.5f, w, hh), j, false);
}

inline float textWidth (const juce::String& s, juce::Font f, float track)
{
    if (track > 0.0f)
        f = f.withExtraKerningFactor (track / f.getHeight());
    return juce::GlyphArrangement::getStringWidth (f, s) - track;
}

inline void glow (juce::Graphics& g, Pt c, float radius, juce::Colour col, float alpha)
{
    g.setGradientFill (juce::ColourGradient (col.withAlpha (alpha), c.x, c.y, col.withAlpha (0.0f), c.x + radius, c.y, true));
    g.fillEllipse (c.x - radius, c.y - radius, radius * 2.0f, radius * 2.0f);
}

/** A linear gradient across a box at an angle (degrees: 90 = top to bottom, 45 = top-left to bottom-right). */
inline juce::ColourGradient grad (juce::Rectangle<float> b, juce::Colour c0, juce::Colour c1, float angle)
{
    const float a = angle * kPi / 180.0f;
    const Pt d (std::cos (a) * b.getWidth() * 0.5f, std::sin (a) * b.getHeight() * 0.5f);
    return juce::ColourGradient (c0, b.getCentre() - d, c1, b.getCentre() + d, false);
}

inline void softShadow (juce::Graphics& g, juce::Rectangle<float> b, float blur, float opacity, Pt offset, float radius, bool ellipse)
{
    for (int i = 0; i < 3; ++i)   // three widening layers stand in for a Gaussian blur
    {
        const auto r = b.translated (offset.x, offset.y).expanded (blur * (float) i * 0.6f);
        g.setColour (juce::Colours::black.withAlpha (opacity * (i == 0 ? 0.5f : 0.22f)));
        if (ellipse) g.fillEllipse (r);
        else g.fillRoundedRectangle (r, radius + blur * (float) i * 0.6f);
    }
}

inline juce::Image brushedPlate (int w, int h, juce::Colour top, juce::Colour bottom, int seed)
{
    juce::Image img (juce::Image::RGB, w * 2, h * 2, false);   // drawn at twice the logical size so the face stays crisp when it is scaled up
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
    for (int i = 0; i < 5200 * w * h / (1600 * 640) + 1; ++i)
    {
        const float x = rng.nextFloat() * (float) w * 2.0f, len = 60.0f + rng.nextFloat() * 520.0f;
        const int y = rng.nextInt (h * 2);
        g.setColour ((rng.nextBool() ? juce::Colours::white : juce::Colours::black).withAlpha (0.022f + rng.nextFloat() * 0.03f));
        g.drawHorizontalLine (y, x, x + len);
    }
    return img;
}

inline void screw (juce::Graphics& g, float cx, float cy, float r)
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

/** Lamp (gold) or LED (red). */
inline void drawLamp (juce::Graphics& g, float cx, float cy, float r, bool on, juce::Colour col, juce::Colour off)
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

inline void redLed (juce::Graphics& g, float cx, float cy, float r, bool on) { drawLamp (g, cx, cy, r, on, Theme::ledRed, rgb (80, 18, 12)); }

inline void drawLogo (juce::Graphics& g, const juce::Image& mark, const juce::Image& wordmark, float right, float y, float wordH, float markH)
{
    if (wordmark.isNull() || mark.isNull()) return;
    const float ww = wordH * (float) wordmark.getWidth() / (float) wordmark.getHeight();
    const float mw = markH * (float) mark.getWidth() / (float) mark.getHeight();
    g.setOpacity (1.0f);
    g.drawImage (wordmark, { right - ww, y - wordH * 0.5f, ww, wordH }, juce::RectanglePlacement::stretchToFit);
    g.drawImage (mark, { right - ww - 8.0f - mw, y - markH * 0.5f - 1.0f, mw, markH }, juce::RectanglePlacement::stretchToFit);
}

/** TUTKAL's PlateKey::paintButton: a cream latching key that turns gold when engaged. `bounds` = the whole component, `margin` = room for bezel and lamp glow. */
inline void paintPlateKey (juce::Graphics& g, juce::Rectangle<float> bounds, const juce::String& text, bool on, bool over, bool down, float size, float track, float margin = 9.0f)
{
    const auto box = bounds.reduced (margin).translated (0.0f, down ? 1.0f : 0.0f);
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
    silk (g, text, Theme::legendEm (size), box.getCentreX(), box.getCentreY() + 0.5f, 'm', track, Theme::panel);
}

/** TUTKAL's BatToggle::paintButton: the bat-handle toggle (up = on). s = the toggle's scale. */
inline void paintBatToggle (juce::Graphics& g, int width, int height, bool on, float s)
{
    const float cx = (float) width * 0.5f, cy = (float) height * 0.5f;
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

/** TUTKAL's drawRotarySlider (the round knob: knurled skirt, machined cap, gold index) at angle `ang` (degrees, 0 = 12 o'clock). */
inline void paintKnob (juce::Graphics& g, float cx, float cy, float r, float ang, bool dead = false)
{
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
    const juce::Line<float> index (polar (cx, cy, cr * 0.28f, ang), polar (cx, cy, r * 0.97f, ang));
    g.setColour (rgb (0, 0, 0, 128));
    g.drawLine (index, juce::jmax (2.0f, r * 0.13f));
    g.setColour (dead ? Theme::goldDark : Theme::gold);
    g.drawLine (index, juce::jmax (1.3f, r * 0.08f));
    if (dead) { g.setColour (rgb (0, 0, 0, 120)); g.fillEllipse (body); }
}

/** TUTKAL's paintKnobScale: the 11 scale ticks round the knob (-135 .. +135 degrees). */
inline void paintKnobTicks (juce::Graphics& g, float cx, float cy, float r, juce::Colour col = Theme::ink)
{
    g.setColour (col);
    for (int i = 0; i < 11; ++i)
    {
        const float a = -135.0f + 27.0f * (float) i;
        const bool major = i == 0 || i == 5 || i == 10;
        g.drawLine ({ polar (cx, cy, r * 1.20f, a), polar (cx, cy, r * (major ? 1.42f : 1.31f), a) }, major ? 1.5f : 1.0f);
    }
}

/** TUTKAL's ValueBox::paint: the black value window (bezel, dark recess, shadow on top, catch-light below), mono read-out. */
inline void paintValueWindow (juce::Graphics& g, juce::Rectangle<float> bounds, const juce::String& text, bool live, bool editing, float fontEm = 22.0f)
{
    const auto b = bounds.reduced (3.0f);
    g.setColour (rgb (8, 10, 9));
    g.fillRoundedRectangle (b.expanded (2.5f), 5.0f);
    g.setGradientFill (grad (b, rgb (1, 8, 7), rgb (8, 26, 23), 90.0f));
    g.fillRoundedRectangle (b, 3.0f);
    g.setColour (rgb (0, 0, 0, 170));
    g.drawLine (b.getX() + 3.0f, b.getY() + 1.0f, b.getRight() - 3.0f, b.getY() + 1.0f, 1.6f);
    g.setColour (rgb (255, 255, 255, 34));
    g.drawLine (b.getX() + 4.0f, b.getBottom() + 3.0f, b.getRight() - 4.0f, b.getBottom() + 3.0f, 0.8f);
    if (live || editing)
    {
        g.setColour (Theme::gold.withAlpha (0.75f));
        g.drawRoundedRectangle (b, 3.0f, 1.2f);
    }
    if (! editing)
        silk (g, text, Theme::monoEm (fontEm), b.getCentreX(), b.getCentreY() + 0.5f, 'm', 0.4f, live ? Theme::lamp : Theme::label);
}

inline void groove (juce::Graphics& g, float y, float x0, float x1)
{
    g.setColour (rgb (0, 0, 0, 120));
    g.drawLine (x0, y, x1, y, 1.0f);
    g.setColour (rgb (255, 255, 255, 34));
    g.drawLine (x0, y + 1.2f, x1, y + 1.2f, 0.8f);
}

/** The gold-outlined model badge / footer plaque of the family faces. */
inline void goldPlaque (juce::Graphics& g, const juce::String& text, float x, float y, float em, float track, char anchor, float padX, float h)
{
    const auto f = Theme::legendEm (em);
    const float tw = textWidth (text, f, track);
    const float left = anchor == 'l' ? x : x - (tw + 2.0f * padX) * 0.5f;
    g.setColour (Theme::gold);
    g.drawRect (juce::Rectangle<float> (left, y - h * 0.5f, tw + 2.0f * padX, h), 1.3f);
    silk (g, text, f, left + padX, y + 0.5f, 'l', track, Theme::gold);
}

/** The product name in the family's squared, extended stroke letters (TUTKAL drawProductName / ALEV familyName; letters A D E L T here, S for SEDA, O R N for TORNA), as an outline path. */
inline juce::Path familyName (const juce::String& name, float x0, float y0, float h)
{
    const float w = 1.34f * h, t = 0.19f * h, r = 0.30f * h, gap = 0.30f * h, in = t * 0.5f;
    juce::Path all;
    float x = x0;
    for (auto ch : name)
    {
        juce::Path p;
        const float L = x + in, R = x + w - in, T = y0 + in, B = y0 + h - in, M = y0 + h * 0.5f;
        switch (ch)
        {
            case 'A':
                p.startNewSubPath (L, B + in); p.lineTo (L, T + r); p.quadraticTo (L, T, L + r, T);
                p.lineTo (R - r, T); p.quadraticTo (R, T, R, T + r); p.lineTo (R, B + in);
                p.startNewSubPath (L, M + h * 0.06f); p.lineTo (R, M + h * 0.06f);
                break;
            case 'L':
                p.startNewSubPath (L, T - in); p.lineTo (L, B); p.lineTo (R + in * 0.2f, B);
                break;
            case 'E':
                p.startNewSubPath (R + in * 0.2f, T); p.lineTo (L + r, T); p.quadraticTo (L, T, L, T + r);
                p.lineTo (L, B - r); p.quadraticTo (L, B, L + r, B); p.lineTo (R + in * 0.2f, B);
                p.startNewSubPath (L, M); p.lineTo (R - 0.10f * w, M);
                break;
            case 'D':
                p.startNewSubPath (L, T); p.lineTo (R - r, T); p.quadraticTo (R, T, R, T + r);
                p.lineTo (R, B - r); p.quadraticTo (R, B, R - r, B); p.lineTo (L, B); p.closeSubPath();
                break;
            case 'S':   // SEDA, P4: top bar, upper left stem, middle bar, lower right stem, bottom bar, with the same rounded corners
                p.startNewSubPath (R + in * 0.2f, T); p.lineTo (L + r, T); p.quadraticTo (L, T, L, T + r);
                p.lineTo (L, M - r); p.quadraticTo (L, M, L + r, M); p.lineTo (R - r, M); p.quadraticTo (R, M, R, M + r);
                p.lineTo (R, B - r); p.quadraticTo (R, B, R - r, B); p.lineTo (L - in * 0.2f, B);
                break;
            case 'T':
                p.startNewSubPath (L - in, T); p.lineTo (R + in, T);
                p.startNewSubPath ((L + R) * 0.5f, T); p.lineTo ((L + R) * 0.5f, B + in);
                break;
            case 'O':   // TORNA: a closed square loop with the family's corner radius
                p.startNewSubPath (L + r, T); p.lineTo (R - r, T); p.quadraticTo (R, T, R, T + r);
                p.lineTo (R, B - r); p.quadraticTo (R, B, R - r, B); p.lineTo (L + r, B); p.quadraticTo (L, B, L, B - r);
                p.lineTo (L, T + r); p.quadraticTo (L, T, L + r, T); p.closeSubPath();
                break;
            case 'R':   // TORNA: stem, a bowl closed at the middle, a straight leg
                p.startNewSubPath (L, B + in); p.lineTo (L, T); p.lineTo (R - r, T); p.quadraticTo (R, T, R, T + r);
                p.lineTo (R, M - r); p.quadraticTo (R, M, R - r, M); p.lineTo (L, M);
                p.startNewSubPath (L + (R - L) * 0.45f, M); p.lineTo (R, B + in);
                break;
            case 'N':   // TORNA: two stems and the diagonal
                p.startNewSubPath (L, B + in); p.lineTo (L, T - in);
                p.startNewSubPath (L, T); p.lineTo (R, B);
                p.startNewSubPath (R, B + in); p.lineTo (R, T - in);
                break;
            default: break;
        }
        all.addPath (p);
        x += w + gap;
    }
    juce::Path outline;
    juce::PathStrokeType (t, juce::PathStrokeType::mitered, juce::PathStrokeType::butt).createStrokedPath (outline, all);
    return outline;
}

inline float familyNameWidth (const juce::String& name, float h) { return (float) name.length() * (1.34f + 0.30f) * h - 0.30f * h; }

inline void paintFamilyName (juce::Graphics& g, const juce::Path& outline, juce::Colour ink)
{
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillPath (outline, juce::AffineTransform::translation (0.0f, 1.2f));
    g.setColour (ink);
    g.fillPath (outline);
}

//==================================================================================================================================================
// ALEV
inline juce::Path annulus (float cx, float cy, float rIn, float rOut)
{
    juce::Path p;
    p.addEllipse (cx - rOut, cy - rOut, 2.0f * rOut, 2.0f * rOut);
    p.addEllipse (cx - rIn, cy - rIn, 2.0f * rIn, 2.0f * rIn);
    p.setUsingNonZeroWinding (false);
    return p;
}

/** A gradient lit from the top left over a path's bounds (in world coordinates, so rotated parts keep the light). */
inline juce::ColourGradient litFrom (juce::Rectangle<float> b, juce::Colour light, juce::Colour dark)
{
    return juce::ColourGradient (light, b.getX(), b.getY(), dark, b.getRight(), b.getBottom(), false);
}

inline juce::Line<float> xf (juce::Line<float> l, const juce::AffineTransform& t) noexcept { return { l.getStart().transformedBy (t), l.getEnd().transformedBy (t) }; }

inline float hash01 (int k) noexcept
{
    const float s = std::sin ((float) k * 12.9898f) * 43758.5453f;
    return s - std::floor (s);
}

inline void strokeCircle (juce::Graphics& g, float cx, float cy, float r, float width) { g.drawEllipse (cx - r, cy - r, 2.0f * r, 2.0f * r, width); }

/** ALEV's brushed-steel ring: anisotropic sheen (two light lobes), brushing hairlines, bevels, knurl ticks on the outer edge, the domed face. */
inline void paintSteelRing (juce::Graphics& g, float cx, float cy, float rIn, float rOut, int seed)
{
    const float rMid = 0.5f * (rIn + rOut), width = rOut - rIn;
    constexpr int kSeg = 360;
    for (int i = 0; i < kSeg; ++i)
    {
        const float a0 = 2.0f * kPi * (float) i / (float) kSeg, a1 = 2.0f * kPi * (float) (i + 1) / (float) kSeg + 0.004f;
        const float am = 0.5f * (a0 + a1);
        const float sheen = 0.5f + 0.5f * std::cos (2.0f * (am + 0.25f * kPi));
        const float top = 0.5f + 0.5f * std::cos (am + 0.25f * kPi);
        const float t = juce::jlimit (0.0f, 1.0f, 0.18f + 0.62f * std::pow (sheen, 1.6f) + 0.18f * top);
        juce::Path p;
        p.addCentredArc (cx, cy, rMid, rMid, 0.0f, a0, a1, true);
        g.setColour (Theme::steelDark.interpolatedWith (Theme::steelLight, t));
        g.strokePath (p, juce::PathStrokeType (width, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
    }
    for (int k = 0; k < (int) width * 2; ++k)
    {
        const float h = hash01 (k + 31 * seed);
        g.setColour ((h > 0.5f ? juce::Colours::white : juce::Colours::black).withAlpha (0.025f + 0.05f * std::abs (h - 0.5f)));
        strokeCircle (g, cx, cy, rIn + 0.5f * (float) k + 0.25f, 0.5f);
    }
    g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.55f), cx, cy - rOut, juce::Colours::black.withAlpha (0.45f), cx, cy + rOut, false));
    strokeCircle (g, cx, cy, rOut - 0.75f, 1.5f);
    g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.5f), cx, cy - rIn, juce::Colours::white.withAlpha (0.35f), cx, cy + rIn, false));
    strokeCircle (g, cx, cy, rIn + 0.75f, 1.5f);
    for (int k = 0; k < 90; ++k)
    {
        const float a = 360.0f * (float) k / 90.0f;
        g.setColour (Theme::steelEdge.withAlpha (0.55f));
        g.drawLine ({ polar (cx, cy, rOut - 5.0f, a), polar (cx, cy, rOut - 1.5f, a) }, 1.0f);
    }
    g.setColour (Theme::steelEdge);
    strokeCircle (g, cx, cy, rIn - 0.5f, 1.0f);
    strokeCircle (g, cx, cy, rOut + 0.5f, 1.0f);
    juce::ColourGradient slope (juce::Colours::white.withAlpha (0.16f), cx, cy, juce::Colours::black.withAlpha (0.24f), cx + rOut, cy, true);   // the dome: lighter inside, darker at the outer edge
    slope.addColour ((double) (rIn / rOut), juce::Colours::white.withAlpha (0.16f));
    slope.addColour ((double) (rMid / rOut), juce::Colours::transparentBlack);
    g.setGradientFill (slope);
    g.fillPath (annulus (cx, cy, rIn, rOut));
}

/** The raised black-steel pointer bar across a ring (ALEV), pointing at 12 o'clock before `rotDeg` turns it about (cx, cy). */
inline void paintPointer (juce::Graphics& g, float cx, float cy, float rIn, float rOut, float rotDeg, float alpha = 1.0f)
{
    const float top = cy - (rOut - 2.0f), len = rOut - rIn - 4.0f;
    juce::Path bar, inlay, s0, s1;
    bar.addRoundedRectangle (cx - 2.6f, top, 5.2f, len, 1.6f);
    inlay.addRoundedRectangle (cx - 0.5f, top + 2.5f, 1.0f, len - 5.0f, 0.5f);
    s0.addRectangle (cx - 2.6f, top + 1.0f, 1.0f, len - 2.0f);
    s1.addRectangle (cx + 1.6f, top + 1.0f, 1.0f, len - 2.0f);
    const float a = rotDeg * kPi / 180.0f;
    const auto rot = juce::AffineTransform::rotation (a, cx, cy);
    for (int k = 3; k >= 1; --k)
    {
        g.setColour (juce::Colours::black.withAlpha (0.14f * alpha));
        g.fillPath (bar, rot.translated (0.55f * (float) k, 0.8f * (float) k));
    }
    g.setGradientFill (litFrom (bar.getBoundsTransformed (rot), Theme::blackSteelLight.withAlpha (alpha), Theme::blackSteelDark.withAlpha (alpha)));
    g.fillPath (bar, rot);
    const float facing = (std::cos (a) + std::sin (a)) * 0.7071f;
    g.setColour (juce::Colours::white.withAlpha (0.42f * alpha * juce::jmax (0.0f, facing)));
    g.fillPath (s0, rot);
    g.setColour (juce::Colours::white.withAlpha (0.42f * alpha * juce::jmax (0.0f, -facing)));
    g.fillPath (s1, rot);
    g.setColour (juce::Colours::black.withAlpha (0.7f * alpha));
    g.strokePath (bar, juce::PathStrokeType (0.6f), rot);
    g.setColour (Theme::ink.withAlpha (0.9f * alpha));
    g.fillPath (inlay, rot);
}

/** ALEV's paintPushers, one pusher: a collar on the case, a stem, a steel cap, grip lines. The case is the circle (cx, cy, caseR); angleDeg is clockwise from
    12 o'clock; depth > 0 = pushed in; s scales the pusher (ALEV's is 1.0); knurl = a wider cap with grip lines all over it (the OUTPUT crown). */
inline void paintChronoPusher (juce::Graphics& g, float cx, float cy, float caseR, float angleDeg, float depth, float s, bool knurl)
{
    const auto rot = juce::AffineTransform::scale (s, s, cx, cy - caseR).followedBy (juce::AffineTransform::rotation (angleDeg * kPi / 180.0f, cx, cy));
    const float capW = knurl ? 26.0f : 20.0f, capH = knurl ? 15.0f : 13.0f;
    juce::Path collar, stem, cap;
    collar.addRoundedRectangle (cx - 11.0f, cy - (caseR + 3.5f), 22.0f, 5.5f, 2.0f);
    stem.addRectangle (cx - 5.0f, cy - (caseR + 8.0f - depth), 10.0f, 5.5f - depth);
    cap.addRoundedRectangle (cx - capW * 0.5f, cy - (caseR + 7.0f + capH - depth), capW, capH, 4.0f);
    for (auto* part : { &stem, &cap })
        for (int k = 3; k >= 1; --k)
        {
            g.setColour (juce::Colours::black.withAlpha (0.13f));
            g.fillPath (*part, rot.translated (0.5f * (float) k, 0.8f * (float) k));
        }
    g.setGradientFill (litFrom (collar.getBoundsTransformed (rot), Theme::blackSteelLight, Theme::blackSteelDark));
    g.fillPath (collar, rot);
    g.setGradientFill (litFrom (stem.getBoundsTransformed (rot), Theme::steelDark, Theme::steelEdge));
    g.fillPath (stem, rot);
    g.setGradientFill (litFrom (cap.getBoundsTransformed (rot), Theme::steelLight, Theme::steelDark));
    g.fillPath (cap, rot);
    if (knurl)
        for (float x = -capW * 0.5f + 2.0f; x < capW * 0.5f - 1.0f; x += 1.7f)
        {
            g.setColour (Theme::steelEdge.withAlpha (0.5f));
            g.drawLine (xf (juce::Line<float> (cx + x, cy - (caseR + 5.5f + capH - depth), cx + x, cy - (caseR + 8.5f - depth)), rot), 0.6f);
        }
    else
        for (float x : { -6.0f, -3.0f, 0.0f, 3.0f, 6.0f })
        {
            g.setColour (Theme::steelEdge.withAlpha (0.45f));
            g.drawLine (xf (juce::Line<float> (cx + x, cy - (caseR + 18.0f - depth), cx + x, cy - (caseR + 9.5f - depth)), rot), 0.7f);
        }
    g.setColour (juce::Colours::white.withAlpha (0.4f));
    g.strokePath (cap, juce::PathStrokeType (0.7f), rot.translated (-0.3f, -0.3f));
    g.setColour (juce::Colours::black.withAlpha (0.65f));
    g.strokePath (cap, juce::PathStrokeType (0.6f), rot);
    g.strokePath (collar, juce::PathStrokeType (0.5f), rot);
}

/** ALEV's buildCurvedText, generalised: letter-spaced caps along a circle (cx, cy, rMid) centred on `centreDeg` (clockwise from 12). At the bottom of the case
    (upright = true) the letters stand the other way up and run against the clock so they read upright. */
inline juce::Path curvedText (const juce::String& text, float cx, float cy, float rMid, float centreDeg, float em, float tracking, bool upright)
{
    juce::Path dest;
    const auto font = Theme::legendEm (em);
    const float capH = 0.72f * em;
    const int n = text.length();
    float total = 0.0f;
    for (int i = 0; i < n; ++i) total += juce::GlyphArrangement::getStringWidth (font, juce::String::charToString (text[i]));
    total += tracking * (float) juce::jmax (0, n - 1);
    float s = -0.5f * total;
    const float c0 = centreDeg * kPi / 180.0f;
    for (int i = 0; i < n; ++i)
    {
        const auto ch = juce::String::charToString (text[i]);
        const float w = juce::GlyphArrangement::getStringWidth (font, ch);
        const float step = (s + 0.5f * w) / rMid;
        const float phi = upright ? c0 - step : c0 + step;
        juce::GlyphArrangement ga;
        ga.addLineOfText (font, ch, 0.0f, 0.0f);
        juce::Path gp;
        ga.createPath (gp);
        if (upright)   // baseline on the outer edge, letters stand towards the centre
            dest.addPath (gp, juce::AffineTransform::translation (-0.5f * w, 0.0f).rotated (phi - kPi).translated (cx + (rMid + capH * 0.5f) * std::sin (phi), cy - (rMid + capH * 0.5f) * std::cos (phi)));
        else
            dest.addPath (gp, juce::AffineTransform::translation (-0.5f * w, 0.0f).rotated (phi).translated (cx + (rMid - capH * 0.5f) * std::sin (phi), cy - (rMid - capH * 0.5f) * std::cos (phi)));
        s += w + tracking;
    }
    return dest;
}

/** The glass sheen of ALEV's dome (paintEmber): a broad soft sheen from the top left, a crescent high on the left with a hot spot, a thin meniscus. */
inline void paintDomeSheen (juce::Graphics& g, float cx, float cy, float R)
{
    const juce::Rectangle<float> disc (cx - R, cy - R, 2.0f * R, 2.0f * R);
    juce::ColourGradient sheen (juce::Colours::transparentWhite, cx - R, cy - R, juce::Colours::transparentWhite, cx + R, cy + R, false);
    sheen.addColour (0.30, juce::Colours::white.withAlpha (0.07f));
    sheen.addColour (0.47, juce::Colours::white.withAlpha (0.025f));
    sheen.addColour (0.60, juce::Colours::transparentWhite);
    g.setGradientFill (sheen);
    g.fillEllipse (disc);
    juce::Path crescent;
    crescent.addCentredArc (cx, cy, R - 9.0f, R - 9.0f, 0.0f, 1.58f * kPi, 1.97f * kPi, true);
    g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.12f), cx - 0.6f * R, cy - 0.7f * R, juce::Colours::transparentWhite, cx, cy - 0.1f * R, false));
    g.strokePath (crescent, juce::PathStrokeType (9.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (juce::Colours::white.withAlpha (0.4f));
    g.fillEllipse (cx - 0.52f * R - 4.5f, cy - 0.58f * R - 2.5f, 9.0f, 5.0f);
    juce::Path meniscus;
    meniscus.addCentredArc (cx, cy, R - 1.0f, R - 1.0f, 0.0f, 1.35f * kPi, 2.15f * kPi, true);
    g.setColour (juce::Colours::white.withAlpha (0.16f));
    g.strokePath (meniscus, juce::PathStrokeType (1.0f));
}
} // namespace fam
