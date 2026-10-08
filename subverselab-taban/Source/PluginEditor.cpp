#include "PluginEditor.h"
#include "BinaryData.h"
#include <map>

/*  The TABAN face: 760 x 500, the family's plate, screws, rails, silkscreen, red read-out and footer plaque
    (TUTKAL / KORO / HAVA), cut down to one job. The FLOOR fader sits in the middle; the IN and OUT bars beside it
    use the fader's own scale, so the OUT bar is seen to stop at the cap.
*/
namespace
{
using Pt = juce::Point<float>;
constexpr float kPi = juce::MathConstants<float>::pi;
constexpr float kW = (float) TabanAudioProcessorEditor::kW, kH = (float) TabanAudioProcessorEditor::kH;
constexpr float kFallDbPerS = 24.0f;    // meter fall
constexpr float kLiftHoldS = 0.4f;      // the LIFT read-out holds its peak this long

const juce::String kDot (juce::CharPointer_UTF8 ("\xc2\xb7"));
const juce::String kMinus (juce::CharPointer_UTF8 ("\xe2\x88\x92"));

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
    const float shift = h == 'm' ? track * 0.5f : h == 'r' ? track : 0.0f;
    g.drawText (s, juce::Rectangle<float> (x0 + shift, y - hh * 0.5f, w, hh), j, false);
}

float textWidth (const juce::String& s, juce::Font f, float track)
{
    if (track > 0.0f)
        f = f.withExtraKerningFactor (track / f.getHeight());
    return juce::GlyphArrangement::getStringWidth (f, s) - track;
}

juce::ColourGradient grad (juce::Rectangle<float> b, juce::Colour c0, juce::Colour c1, float angle)
{
    const float a = angle * kPi / 180.0f;
    const Pt d (std::cos (a) * b.getWidth() * 0.5f, std::sin (a) * b.getHeight() * 0.5f);
    return juce::ColourGradient (c0, b.getCentre() - d, c1, b.getCentre() + d, false);
}

void softShadow (juce::Graphics& g, juce::Rectangle<float> b, float blur, float opacity, Pt offset, float radius, bool ellipse)
{
    for (int i = 0; i < 3; ++i)
    {
        const auto r = b.translated (offset.x, offset.y).expanded (blur * (float) i * 0.6f);
        g.setColour (juce::Colours::black.withAlpha (opacity * (i == 0 ? 0.5f : 0.22f)));
        if (ellipse) g.fillEllipse (r);
        else g.fillRoundedRectangle (r, radius + blur * (float) i * 0.6f);
    }
}

void glow (juce::Graphics& g, Pt c, float radius, juce::Colour col, float alpha)
{
    g.setGradientFill (juce::ColourGradient (col.withAlpha (alpha), c.x, c.y, col.withAlpha (0.0f), c.x + radius, c.y, true));
    g.fillEllipse (c.x - radius, c.y - radius, radius * 2.0f, radius * 2.0f);
}

juce::Image brushedPlate (int w, int h, juce::Colour top, juce::Colour bottom, int seed)
{
    // Twice the logical size, so the face stays crisp on a Retina display.
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
    for (int i = 0; i < 2600; ++i)
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
        g.drawLine ({ polar (cx, cy, r * 0.62f, a), polar (cx, cy, r * 0.62f, a + 180.0f) }, 2.0f);
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

/** The product name in the family's squared, extended stroke letters. Returns the width.
    Letters drawn: A B K L N T U (TABAN, TUTKAL). Filled as an outline so every renderer draws the same corners. */
float productNameWidth (const juce::String& name, float h) { return (float) name.length() * 1.64f * h - 0.30f * h; }

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
            case 'B':
            {
                // Two squared bowls on a straight stem; the upper one a little narrower.
                const float rb = 0.20f * h, Ru = R - w * 0.08f, Mb = M + h * 0.02f;
                p.startNewSubPath (L - in, T); p.lineTo (Ru - rb, T); p.quadraticTo (Ru, T, Ru, T + rb);
                p.lineTo (Ru, Mb - rb); p.quadraticTo (Ru, Mb, Ru - rb, Mb); p.lineTo (L, Mb);
                p.startNewSubPath (L, Mb); p.lineTo (R - rb, Mb); p.quadraticTo (R, Mb, R, Mb + rb);
                p.lineTo (R, B - rb); p.quadraticTo (R, B, R - rb, B); p.lineTo (L - in, B);
                p.startNewSubPath (L, T - in); p.lineTo (L, B + in);
                break;
            }
            case 'N':
                p.startNewSubPath (L, B + in); p.lineTo (L, T - in);
                p.startNewSubPath (R, T - in); p.lineTo (R, B + in);
                p.startNewSubPath (L + in * 0.4f, T + in * 0.6f); p.lineTo (R - in * 0.4f, B - in * 0.6f);
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
}

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

/** The recessed value window (as TUTKAL's ValueBox): cream digits on dark glass. */
void valueBox (juce::Graphics& g, juce::Rectangle<float> b, const juce::String& text, bool lit)
{
    g.setColour (rgb (8, 10, 9));
    g.fillRoundedRectangle (b.expanded (2.5f), 5.0f);
    g.setGradientFill (grad (b, rgb (1, 8, 7), rgb (8, 26, 23), 90.0f));
    g.fillRoundedRectangle (b, 3.0f);
    g.setColour (rgb (0, 0, 0, 170));
    g.drawLine (b.getX() + 3.0f, b.getY() + 1.0f, b.getRight() - 3.0f, b.getY() + 1.0f, 1.6f);
    g.setColour (rgb (255, 255, 255, 34));
    g.drawLine (b.getX() + 4.0f, b.getBottom() + 3.0f, b.getRight() - 4.0f, b.getBottom() + 3.0f, 0.8f);
    silk (g, text, Theme::monoEm (17.0f), b.getCentreX(), b.getCentreY() + 0.5f, 'm', 0.4f, lit ? Theme::lamp : Theme::label);
}

juce::String dbText (float v, bool offAtBottom)
{
    if (offAtBottom && v <= -59.95f) return "OFF";
    const auto s = juce::String (std::abs (v), 1);
    return (v < -0.049f ? kMinus : v > 0.049f ? juce::String ("+") : juce::String()) + s + " dB";
}

juce::Image loadPng (const void* d, int n) { return juce::ImageCache::getFromMemory (d, n); }
} // namespace

//==============================================================================
void TabanLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h,
                                         float sliderPos, float, float, const juce::Slider::SliderStyle, juce::Slider&)
{
    // The slot: a dark recess with a centre groove, running the cap's travel.
    const float cx = (float) x + (float) w * 0.5f, slotW = 9.0f;
    const float top = (float) y + kCapH * 0.5f - 4.0f, bot = (float) (y + h) - kCapH * 0.5f + 4.0f;
    const juce::Rectangle<float> slot (cx - slotW * 0.5f, top, slotW, bot - top);
    g.setColour (rgb (6, 9, 8));
    g.fillRoundedRectangle (slot, 3.5f);
    g.setColour (rgb (0, 0, 0, 200));
    g.drawRoundedRectangle (slot, 3.5f, 1.2f);
    g.setColour (rgb (255, 255, 255, 30));
    g.drawLine (slot.getX() + 2.0f, slot.getBottom() + 1.5f, slot.getRight() - 2.0f, slot.getBottom() + 1.5f, 0.8f);

    // The cap: machined aluminium, ribbed grips, one cream index line engraved across its middle.
    const juce::Rectangle<float> cap (cx - kCapW * 0.5f, sliderPos - kCapH * 0.5f, kCapW, kCapH);
    softShadow (g, cap, 3.0f, 0.65f, { 1.5f, 3.5f }, 2.5f, false);
    g.setGradientFill (grad (cap, rgb (236, 238, 234), rgb (128, 133, 130), 90.0f));
    g.fillRoundedRectangle (cap, 2.5f);
    g.setColour (rgb (255, 255, 255, 140));
    g.drawLine (cap.getX() + 2.0f, cap.getY() + 1.0f, cap.getRight() - 2.0f, cap.getY() + 1.0f, 0.9f);
    g.setColour (rgb (0, 0, 0, 90));
    g.drawLine (cap.getX() + 2.0f, cap.getBottom() - 0.8f, cap.getRight() - 2.0f, cap.getBottom() - 0.8f, 0.9f);
    for (int i = 0; i < 4; ++i)
    {
        // grips above and below the index
        for (float yy : { cap.getY() + 4.0f + (float) i * 2.2f, cap.getBottom() - 5.0f - (float) i * 2.2f })
        {
            g.setColour (rgb (70, 74, 72, 150));
            g.drawLine (cap.getX() + 4.0f, yy, cap.getRight() - 4.0f, yy, 0.7f);
            g.setColour (rgb (255, 255, 255, 60));
            g.drawLine (cap.getX() + 4.0f, yy + 0.8f, cap.getRight() - 4.0f, yy + 0.8f, 0.5f);
        }
    }
    g.setColour (rgb (0, 0, 0, 150));
    g.fillRect (cap.getX() + 2.0f, sliderPos - 1.6f, kCapW - 4.0f, 3.2f);
    g.setColour (Theme::ink);
    g.fillRect (cap.getX() + 2.0f, sliderPos - 0.7f, kCapW - 4.0f, 1.4f);
}

void TabanLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                         float startAngle, float endAngle, juce::Slider&)
{
    const float r = ((float) juce::jmin (w, h) - 6.0f) * 0.5f;
    const float cx = (float) x + (float) w * 0.5f, cy = (float) y + (float) h * 0.5f;
    const float ang = (startAngle + pos * (endAngle - startAngle)) * 180.0f / kPi;

    const juce::Rectangle<float> body (cx - r, cy - r, 2 * r, 2 * r);
    softShadow (g, body, r * 0.10f, 0.55f, { r * 0.06f, r * 0.13f }, 0.0f, true);
    g.setGradientFill (grad (body, rgb (43, 48, 46), rgb (11, 13, 12), 90.0f));
    g.fillEllipse (body);
    for (int i = 0; i < 60; ++i)
    {
        const float a = 360.0f * (float) i / 60.0f;
        const float light = 0.5f + 0.5f * std::cos (a * kPi / 180.0f + 0.8f);
        g.setColour (juce::Colours::white.withAlpha (0.05f + 0.15f * light));
        g.drawLine ({ polar (cx, cy, r * 0.86f, a), polar (cx, cy, r * 0.985f, a) }, 0.7f);
    }

    const float cr = r * 0.74f;
    const juce::Rectangle<float> cap (cx - cr, cy - cr, 2 * cr, 2 * cr);
    g.setGradientFill (grad (cap, rgb (222, 223, 218), rgb (110, 116, 113), 45.0f));
    g.fillEllipse (cap);
    int ring = 0;
    for (float rr = cr * 0.15f; rr < cr; rr += juce::jmax (1.0f, cr / 18.0f), ++ring)
    {
        g.setColour ((ring & 1) ? rgb (255, 255, 255, 22) : rgb (0, 0, 0, 14));
        g.drawEllipse (cx - rr, cy - rr, rr * 2.0f, rr * 2.0f, 0.45f);
    }
    juce::Path sheen;
    sheen.addPieSegment (cap, 234.0f * kPi / 180.0f, 291.0f * kPi / 180.0f, 0.0f);
    sheen.addPieSegment (cap, 54.0f * kPi / 180.0f, 111.0f * kPi / 180.0f, 0.0f);
    g.setColour (rgb (255, 255, 255, 45));
    g.fillPath (sheen);
    g.setColour (rgb (0, 0, 0, 80));
    g.drawEllipse (cap, 0.8f);

    const juce::Line<float> index (polar (cx, cy, cr * 0.28f, ang), polar (cx, cy, r * 0.97f, ang));
    g.setColour (rgb (0, 0, 0, 128));
    g.drawLine (index, juce::jmax (2.0f, r * 0.12f));
    g.setColour (Theme::gold);
    g.drawLine (index, juce::jmax (1.2f, r * 0.08f));
}

//==============================================================================
void BatToggle::paintButton (juce::Graphics& g, bool, bool)
{
    const float cx = (float) getWidth() * 0.5f, cy = (float) getHeight() * 0.5f;
    const bool on = getToggleState();
    const juce::Rectangle<float> nut (cx - 10.0f, cy - 10.0f, 20.0f, 20.0f);
    softShadow (g, nut, 2.0f, 0.55f, { 1.5f, 3.0f }, 0.0f, true);
    g.setGradientFill (grad (nut, rgb (232, 233, 229), rgb (96, 100, 98), 45.0f));
    g.fillEllipse (nut);
    g.setColour (rgb (40, 43, 42));
    g.fillEllipse (cx - 6.0f, cy - 6.0f, 12.0f, 12.0f);

    const float dir = on ? -1.0f : 1.0f, tipy = cy + dir * 22.0f;
    juce::Path lever;
    lever.addQuadrilateral (cx - 2.8f, cy, cx + 2.8f, cy, cx + 1.8f, tipy, cx - 1.8f, tipy);
    g.setColour (rgb (0, 0, 0, 70));
    g.fillPath (lever, juce::AffineTransform::translation (2.0f, 2.0f));
    g.setColour (rgb (210, 212, 208));
    g.fillPath (lever);
    const juce::Rectangle<float> ball (cx - 4.5f, tipy - 4.5f, 9.0f, 9.0f);
    softShadow (g, ball, 1.2f, 0.5f, { 1.5f, 2.5f }, 0.0f, true);
    g.setGradientFill (grad (ball, rgb (250, 250, 247), rgb (120, 124, 121), 45.0f));
    g.fillEllipse (ball);
}

//==============================================================================
namespace layout
{
    constexpr float faderCx = kW * 0.5f, faderTop = 186.0f, faderH = 196.0f;
    constexpr float knobCx = 600.0f, knobCy = 262.0f, knobR = 44.0f;
    const juce::Rectangle<float> lcd { 82.0f, 210.0f, 170.0f, 84.0f };
    constexpr float inBarX = faderCx - 40.0f, outBarX = faderCx + 34.0f, barW = 6.0f;
    constexpr float boxY = 400.0f, boxH = 30.0f;
    constexpr float footY = kH - 26.0f;
}

TabanAudioProcessorEditor::TabanAudioProcessorEditor (TabanAudioProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    setLookAndFeel (&lookAndFeel);

    floorFader.setSliderStyle (juce::Slider::LinearVertical);
    floorFader.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    floorFader.setDoubleClickReturnValue (true, -60.0);
    floorFader.setMouseDragSensitivity (300);
    addAndMakeVisible (floorFader);

    outputKnob.setSliderStyle (juce::Slider::RotaryVerticalDrag);
    outputKnob.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    outputKnob.setRotaryParameters (juce::degreesToRadians (-135.0f), juce::degreesToRadians (135.0f), true);
    outputKnob.setDoubleClickReturnValue (true, 0.0);
    addAndMakeVisible (outputKnob);

    addAndMakeVisible (inToggle);

    floorAttach  = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, "floor", floorFader);
    outputAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, "output", outputKnob);
    inAttach     = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "in", inToggle);

    plate    = brushedPlate (kW, kH, Theme::panel, Theme::panelDeep, 83);
    mark     = loadPng (BinaryData::mark_cream_png, BinaryData::mark_cream_pngSize);
    wordmark = loadPng (BinaryData::wordmark_cream_png, BinaryData::wordmark_cream_pngSize);

    setWantsKeyboardFocus (true);
    setSize (kW, kH);
    startTimerHz (30);
}

TabanAudioProcessorEditor::~TabanAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

bool TabanAudioProcessorEditor::keyPressed (const juce::KeyPress& key) { return svl::handleUndoKey (proc.undo, key); }

void TabanAudioProcessorEditor::advance (float dt)
{
    const float fall = kFallDbPerS * dt;
    const float in = proc.getInputDb(), out = proc.getOutputDb(), lift = proc.getLiftDb();
    shownIn  = in  >= shownIn  ? in  : juce::jmax (in,  shownIn  - fall);
    shownOut = out >= shownOut ? out : juce::jmax (out, shownOut - fall);
    if (lift >= shownLift) { shownLift = lift; liftHold = kLiftHoldS; }
    else if ((liftHold -= dt) <= 0.0f) shownLift = juce::jmax (lift, shownLift - fall);
}

void TabanAudioProcessorEditor::timerCallback()
{
    advance (1.0f / 30.0f);
    repaint();
}

float TabanAudioProcessorEditor::faderY (float db) const
{
    return (float) floorFader.getY() + (float) floorFader.getPositionOfValue (juce::jlimit (-60.0, 0.0, (double) db));
}

void TabanAudioProcessorEditor::resized()
{
    using namespace layout;
    floorFader.setBounds (juce::Rectangle<float> (faderCx - 30.0f, faderTop, 60.0f, faderH).toNearestInt());
    outputKnob.setBounds (juce::Rectangle<float> (knobCx - knobR - 4.0f, knobCy - knobR - 4.0f, knobR * 2.0f + 8.0f, knobR * 2.0f + 8.0f).toNearestInt());
    inToggle.setBounds (juce::Rectangle<float> (kW - 104.0f, 44.0f, 32.0f, 60.0f).toNearestInt());
}

void TabanAudioProcessorEditor::paint (juce::Graphics& g)
{
    using namespace layout;
    g.drawImage (plate, getLocalBounds().toFloat());

    // Rails and screws.
    for (float x : { 40.0f, kW - 40.0f })
    {
        g.setColour (rgb (0, 0, 0, 110));
        g.drawLine (x, 0.0f, x, kH, 1.2f);
        g.setColour (rgb (255, 255, 255, 30));
        g.drawLine (x + 1.5f, 0.0f, x + 1.5f, kH, 0.8f);
    }
    for (float x : { 20.0f, kW - 20.0f })
        for (float y : { 34.0f, kH - 34.0f })
            screw (g, x, y, 7.0f);

    // Name, subtitle, model plate.
    const juce::String name = "TABAN";
    const float nameH = 34.0f, nameW = productNameWidth (name, nameH);
    drawProductName (g, name, 70.0f, 34.0f, nameH, Theme::ink);
    {
        const juce::String t = "REVERSE LIMITER " + kDot + " MINIMUM LEVEL FLOOR";
        float em = 10.5f;
        while (em > 6.0f && textWidth (t, Theme::legendEm (em), 3.0f * em / 10.5f) > nameW + 60.0f) em -= 0.25f;
        silk (g, t, Theme::legendEm (em), 72.0f, 94.0f, 'l', 3.0f * em / 10.5f, Theme::ink);
    }
    {
        const auto f = Theme::legendEm (10.0f);
        const float tw = textWidth ("MODEL TB-83", f, 2.4f);
        g.setColour (Theme::gold);
        g.drawRect (juce::Rectangle<float> (72.0f, 112.0f, tw + 22.0f, 22.0f), 1.3f);
        silk (g, "MODEL TB-83", f, 83.0f, 123.5f, 'l', 2.4f, Theme::gold);
    }

    // IN switch: legends beside the lever, name below.
    {
        const auto t = inToggle.getBounds().toFloat();
        silk (g, "ON",  Theme::legendEm (7.5f), t.getRight() + 4.0f, t.getY() + 9.0f, 'l', 1.0f, Theme::ink);
        silk (g, "OFF", Theme::legendEm (7.5f), t.getRight() + 4.0f, t.getBottom() - 9.0f, 'l', 1.0f, Theme::ink);
        silk (g, "IN", Theme::legendEm (11.0f), t.getCentreX(), t.getBottom() + 14.0f, 'm', 3.0f, Theme::ink);
        silk (g, "OFF = BYPASSED", Theme::fineEm (7.0f), t.getCentreX(), t.getBottom() + 29.0f, 'm', 1.2f, Theme::muted);
    }

    groove (g, 150.0f, 60.0f, kW - 60.0f);

    // LIFT: how far the floor is lifting right now.
    bracket (g, 72.0f, 262.0f, 172.0f, "LIFT");
    {
        softShadow (g, lcd, 2.5f, 0.6f, { 1.5f, 2.5f }, 4.0f, false);
        g.setColour (rgb (8, 10, 9));
        g.fillRoundedRectangle (lcd.expanded (3.0f), 6.0f);
        g.setColour (Theme::redWindow);
        g.fillRoundedRectangle (lcd, 4.0f);
        const float v = juce::jlimit (0.0f, 99.9f, shownLift);
        const auto digits = juce::String (v, 1).paddedLeft (' ', 4);   // "12.3", " 0.0"
        const float dh = 50.0f, dw = 0.56f * dh, step = dw + 9.0f, dx = lcd.getX() + 16.0f, dy = lcd.getY() + 17.0f;
        int pos = 0;
        for (auto ch : digits)
        {
            if (ch == '.')
            {
                g.setColour (Theme::ledRed);
                g.fillEllipse (dx + (float) pos * step - 10.0f, dy + dh - 6.0f, 6.0f, 6.0f);
                continue;
            }
            sevenSegment (g, dx + (float) pos * step, dy, dh, (char) ch);
            ++pos;
        }
        silk (g, "dB", Theme::legendEm (10.0f), lcd.getRight() - 10.0f, lcd.getBottom() - 14.0f, 'r', 1.0f, Theme::ledRed);
        const bool holding = shownLift > 0.05f;
        drawLamp (g, lcd.getX() + 10.0f, lcd.getBottom() + 24.0f, 4.0f, holding, Theme::lamp, rgb (45, 40, 20));
        silk (g, "HOLDING THE FLOOR", Theme::legendEm (8.0f), lcd.getX() + 24.0f, lcd.getBottom() + 24.0f, 'l', 1.6f, holding ? Theme::ink : Theme::muted);
        silk (g, "LATENCY 0 SMP " + kDot + " ALL RATES", Theme::fineEm (7.2f), lcd.getX(), lcd.getBottom() + 48.0f, 'l', 1.2f, Theme::muted);
    }

    // FLOOR: scale, IN and OUT bars on the fader's own scale, value window.
    bracket (g, faderCx - 90.0f, faderCx + 90.0f, 172.0f, "FLOOR");
    {
        for (int db = 0; db >= -60; db -= 6)
        {
            const float y = faderY ((float) db);
            const bool major = db % 12 == 0;
            const float len = major ? 6.0f : 3.5f;
            g.setColour (Theme::ink.withAlpha (major ? 0.85f : 0.55f));
            g.drawLine (inBarX - 3.0f - len, y, inBarX - 3.0f, y, 1.0f);
            g.drawLine (outBarX + barW + 3.0f, y, outBarX + barW + 3.0f + len, y, 1.0f);
            if (! major) continue;
            const juce::String s = db == -60 ? juce::String ("OFF") : db == 0 ? juce::String ("0") : kMinus + juce::String (-db);
            silk (g, s, Theme::legendEm (8.0f), inBarX - 13.0f, y, 'r', 0.5f, Theme::ink);
            silk (g, s, Theme::legendEm (8.0f), outBarX + barW + 13.0f, y, 'l', 0.5f, Theme::ink);
        }
        const float top = faderY (0.0f), bot = faderY (-60.0f);
        auto bar = [&] (float x, float db, juce::Colour c)
        {
            const juce::Rectangle<float> slot (x, top - 3.0f, barW, bot - top + 6.0f);
            g.setColour (rgb (6, 9, 8));
            g.fillRoundedRectangle (slot.expanded (1.5f), 2.5f);
            if (db > -60.0f)
            {
                const float y = faderY (db);
                g.setColour (c);
                g.fillRect (juce::Rectangle<float> (x + 1.0f, y, barW - 2.0f, bot - y));
            }
        };
        bar (inBarX, shownIn, Theme::muted);
        bar (outBarX, shownOut, Theme::lamp);
        silk (g, "IN",  Theme::legendEm (7.5f), inBarX + barW * 0.5f, bot + 16.0f, 'm', 1.0f, Theme::muted);
        silk (g, "OUT", Theme::legendEm (7.5f), outBarX + barW * 0.5f, bot + 16.0f, 'm', 1.0f, Theme::lamp);

        const float v = (float) floorFader.getValue();
        valueBox (g, { faderCx - 56.0f, boxY, 112.0f, boxH }, dbText (v, true), floorFader.isMouseButtonDown());
    }

    // OUTPUT: scale round the knob, value window.
    bracket (g, knobCx - 90.0f, knobCx + 90.0f, 172.0f, "OUTPUT");
    {
        const float marks[] { -12.0f, -6.0f, 0.0f, 6.0f, 12.0f };
        for (float m : marks)
        {
            const float a = -135.0f + 270.0f * (m + 12.0f) / 24.0f;
            g.setColour (Theme::ink);
            g.drawLine ({ polar (knobCx, knobCy, knobR * 1.08f, a), polar (knobCx, knobCy, knobR * 1.24f, a) }, 1.0f);
            const auto p = polar (knobCx, knobCy, knobR * 1.50f, a);
            const juce::String s = m == 0.0f ? juce::String ("0") : (m < 0.0f ? kMinus : juce::String ("+")) + juce::String ((int) std::abs (m));
            silk (g, s, Theme::legendEm (8.0f), p.x, p.y, 'm', 0.0f, Theme::ink);
        }
        valueBox (g, { knobCx - 56.0f, boxY, 112.0f, boxH }, dbText ((float) outputKnob.getValue(), false), outputKnob.isMouseButtonDown());
        silk (g, "AFTER THE FLOOR", Theme::fineEm (7.0f), knobCx, knobCy + knobR + 34.0f, 'm', 1.2f, Theme::muted);
    }

    // Foot: "measured" left, plaque in the middle, wordmark right.
    groove (g, footY - 26.0f, 60.0f, kW - 60.0f);
    silk (g, "MEASURED, NOT GUESSED", Theme::fineEm (7.0f), 72.0f, footY, 'l', 1.5f, Theme::muted);
    {
        const juce::String t = "TABAN " + kDot + " MODEL TB-83 " + kDot + " FLOOR LEVELLER";
        const auto f = Theme::legendEm (8.5f);
        const float tw = textWidth (t, f, 2.2f);
        g.setColour (Theme::gold);
        g.drawRect (juce::Rectangle<float> (kW * 0.5f - tw * 0.5f - 14.0f, footY - 11.0f, tw + 28.0f, 22.0f), 1.3f);
        silk (g, t, f, kW * 0.5f, footY, 'm', 2.2f, Theme::gold);
    }
    if (! wordmark.isNull() && ! mark.isNull())
    {
        const float wordH = 13.0f, markH = 20.0f, right = kW - 66.0f, y = footY;
        const float ww = wordH * (float) wordmark.getWidth() / (float) wordmark.getHeight();
        const float mw = markH * (float) mark.getWidth() / (float) mark.getHeight();
        g.setOpacity (1.0f);
        g.drawImage (wordmark, { right - ww, y - wordH * 0.5f, ww, wordH }, juce::RectanglePlacement::stretchToFit);
        g.drawImage (mark, { right - ww - 7.0f - mw, y - markH * 0.5f - 1.0f, mw, markH }, juce::RectanglePlacement::stretchToFit);
    }
}
