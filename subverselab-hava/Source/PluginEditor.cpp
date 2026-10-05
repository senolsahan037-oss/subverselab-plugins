#include "PluginEditor.h"
#include "Theme.h"
#include "BinaryData.h"

/*  The family kit, ported from subverselab-koro/tools/concepts.py (the approved
    HAVA concepts are drawn with it by subverselab-hava/tools/concepts.py).
    Coordinates are the concept's base units: the face is 1600 x 640, the rear
    plate 1200 x 440. Colours, sizes, radii and tracking are the concept's own.
*/
namespace
{
using juce::Colour;
using juce::Graphics;
using juce::Path;
using juce::Point;
using juce::Rectangle;
using juce::String;

const Colour kCream { 242, 239, 231 }, kInk { 3, 55, 51 }, kInk2 { 73, 108, 104 }, kGold { 197, 160, 89 },
             kGoldL { 223, 191, 122 }, kMuted { 143, 170, 165 }, kLedRed { 255, 64, 40 }, kLedRedOff { 70, 14, 10 },
             kLampRedOff { 80, 18, 12 };

// Knob travel: -135 .. +135 degrees from 12 o'clock.
constexpr float kStart = juce::MathConstants<float>::pi * 1.25f, kEnd = juce::MathConstants<float>::pi * 2.75f;

String utf (const char* s) { return String::fromUTF8 (s); }
Colour rgba (int r, int g, int b, int a) { return Colour ((juce::uint8) r, (juce::uint8) g, (juce::uint8) b, (juce::uint8) a); }
float rad (float deg) { return juce::degreesToRadians (deg); }

Point<float> polar (float cx, float cy, float r, float deg)
{
    const float a = rad (deg);
    return { cx + r * std::sin (a), cy - r * std::cos (a) };
}

// ---- text: the concept's anchors (l/m/r, vertically centred) and tracking in units -------------
enum class Anchor { left, middle, right };

float textWidth (juce::Font f, const String& s, float track)
{
    if (track > 0.0f)
        f = f.withExtraKerningFactor (track / f.getHeight());
    const float w = juce::GlyphArrangement::getStringWidth (f, s);
    return track > 0.0f && s.isNotEmpty() ? w - track : w;
}

float text (Graphics& g, float x, float y, const String& s, juce::Font f, Colour col, Anchor a = Anchor::middle, float track = 0.0f)
{
    if (track > 0.0f)
        f = f.withExtraKerningFactor (track / f.getHeight());
    const float w = juce::GlyphArrangement::getStringWidth (f, s) - (track > 0.0f && s.isNotEmpty() ? track : 0.0f);
    const float x0 = a == Anchor::left ? x : a == Anchor::middle ? x - w * 0.5f : x - w;
    juce::GlyphArrangement ga;
    ga.addLineOfText (f, s, x0, y + (f.getAscent() - f.getDescent()) * 0.5f);
    g.setColour (col);
    ga.draw (g);
    return w;
}

juce::Font legend (float em) { return Theme::legendEm (em); }
juce::Font fine (float em) { return Theme::fineEm (em); }

/** Panel legend in the silkscreen cream (the concept's f.lt). */
float lt (Graphics& g, float x, float y, const String& s, float size = 10.0f, float track = 2.0f, bool isFine = false,
          Anchor a = Anchor::middle, Colour col = kCream)
{
    return text (g, x, y, s, isFine ? fine (size) : legend (size), col, a, track);
}

// ---- light and shadow ---------------------------------------------------------------------------
void soft (Graphics& g, const Path& shape, float blur, Colour col, float opacity, Point<float> offset = {})
{
    juce::DropShadow ds (col.withAlpha (opacity), juce::jmax (1, juce::roundToInt (blur * 2.2f)), {});
    Path p (shape);
    p.applyTransform (juce::AffineTransform::translation (offset));
    ds.drawForPath (g, p);
}

Path ellipse (float cx, float cy, float r)
{
    Path p;
    p.addEllipse (cx - r, cy - r, 2.0f * r, 2.0f * r);
    return p;
}

Path rrect (Rectangle<float> b, float r)
{
    Path p;
    p.addRoundedRectangle (b, r);
    return p;
}

/** The concept's grad(): a linear blend across a box at an angle (90 = top to bottom, 45 = top left to bottom right). */
juce::ColourGradient grad (Rectangle<float> b, Colour c0, Colour c1, float angleDeg)
{
    const float ca = std::cos (rad (angleDeg)), sa = std::sin (rad (angleDeg));
    const Point<float> gv (ca / b.getWidth(), sa / b.getHeight());
    const float len2 = gv.x * gv.x + gv.y * gv.y;
    const float fmax = 0.5f * (std::abs (ca) + std::abs (sa));
    const auto c = b.getCentre();
    return juce::ColourGradient (c0, c - gv * (fmax / len2), c1, c + gv * (fmax / len2), false);
}

void fillGrad (Graphics& g, const Path& p, Rectangle<float> b, Colour c0, Colour c1, float angleDeg)
{
    g.setGradientFill (grad (b, c0, c1, angleDeg));
    g.fillPath (p);
}

void line (Graphics& g, Point<float> a, Point<float> b, float width, Colour col)
{
    g.setColour (col);
    g.drawLine ({ a, b }, width);
}

// ---- plate, rack, screws -------------------------------------------------------------------------
float gauss (juce::Random& rng)
{
    float s = 0.0f;
    for (int i = 0; i < 4; ++i)
        s += rng.nextFloat();
    return (s - 2.0f) * 1.7320508f;
}

/** Brushed anodised plate: a vertical blend, per-row grain and short horizontal streaks. */
juce::Image makePlate (int w, int h, Colour top, Colour bottom, float streak, float grain, int seed)
{
    juce::Image img (juce::Image::RGB, w, h, false);
    juce::Image::BitmapData data (img, juce::Image::BitmapData::writeOnly);
    juce::Random rng (seed);
    const int knots = w / 21 + 2;
    std::vector<float> field ((size_t) knots);
    // Statistics of the concept's 4.22x render after its downscale: rows ~0.29 streak, streaks ~0.28 streak, grain ~0.25 grain.
    for (int y = 0; y < h; ++y)
    {
        const float t = (float) y / (float) juce::jmax (1, h - 1);
        const Colour base = top.interpolatedWith (bottom, t);
        const float row = gauss (rng) * streak * 0.29f;
        for (auto& k : field)
            k = gauss (rng) * streak * 0.28f;
        for (int x = 0; x < w; ++x)
        {
            const float fx = (float) x / 21.0f;
            const int i0 = (int) fx;
            const float fr = fx - (float) i0;
            const float v = row + field[(size_t) i0] * (1.0f - fr) + field[(size_t) i0 + 1] * fr + gauss (rng) * grain * 0.25f;
            auto ch = [v] (juce::uint8 c) { return (juce::uint8) juce::jlimit (0, 255, juce::roundToInt ((float) c + v)); };
            data.setPixelColour (x, y, Colour (ch (base.getRed()), ch (base.getGreen()), ch (base.getBlue())));
        }
    }
    return img;
}

void screw (Graphics& g, float cx, float cy, float r)
{
    soft (g, ellipse (cx, cy, r), 1.6f, juce::Colours::black, 0.6f, { 1.0f, 2.0f });
    fillGrad (g, ellipse (cx, cy, r), { cx - r, cy - r, 2 * r, 2 * r }, Colour (236, 237, 233), Colour (120, 124, 121), 45.0f);
    g.setColour (rgba (0, 0, 0, 90));
    g.strokePath (ellipse (cx, cy, r), juce::PathStrokeType (0.8f));
    for (float a : { 45.0f, 135.0f })
        line (g, polar (cx, cy, r * 0.62f, a), polar (cx, cy, r * 0.62f, a + 180.0f), 2.2f, rgba (40, 42, 41, 230));
}

void rack (Graphics& g)
{
    const float H = (float) HavaEditor::kH;
    for (auto [x0, x1] : { std::pair<float, float> { 0.0f, 44.0f }, std::pair<float, float> { 1556.0f, 1600.0f } })
    {
        g.setColour (rgba (0, 0, 0, 60));
        g.fillRect (Rectangle<float> (x0, 0.0f, x1 - x0, H));
        const float cx = (x0 + x1) * 0.5f;
        for (float cy : { 62.0f, 578.0f })
        {
            g.setColour (rgba (4, 8, 7, 235));
            g.fillRoundedRectangle ({ cx - 12.0f, cy - 17.0f, 24.0f, 34.0f }, 12.0f);
            screw (g, cx, cy, 8.5f);
        }
    }
    line (g, { 44.0f, 0.0f }, { 44.0f, H }, 1.2f, rgba (0, 0, 0, 110));
    line (g, { 45.5f, 0.0f }, { 45.5f, H }, 0.8f, rgba (255, 255, 255, 30));
    line (g, { 1556.0f, 0.0f }, { 1556.0f, H }, 1.2f, rgba (0, 0, 0, 110));
    line (g, { 1557.5f, 0.0f }, { 1557.5f, H }, 0.8f, rgba (255, 255, 255, 30));
}

void groove (Graphics& g, float y, float x0 = 64.0f, float x1 = 1536.0f)
{
    line (g, { x0, y }, { x1, y }, 1.0f, rgba (0, 0, 0, 120));
    line (g, { x0, y + 1.2f }, { x1, y + 1.2f }, 0.8f, rgba (255, 255, 255, 34));
}

void bracket (Graphics& g, float x0, float x1, float y, const String& title, float size = 10.5f)
{
    const float cx = (x0 + x1) * 0.5f, tw = textWidth (legend (size), title, 3.0f), gap = 10.0f;
    line (g, { x0, y }, { cx - tw * 0.5f - gap, y }, 1.1f, kCream);
    line (g, { cx + tw * 0.5f + gap, y }, { x1, y }, 1.1f, kCream);
    line (g, { x0, y }, { x0, y + 7.0f }, 1.1f, kCream);
    line (g, { x1, y }, { x1, y + 7.0f }, 1.1f, kCream);
    lt (g, cx, y, title, size, 3.0f);
}

// ---- the stroked wordmark (KUBBE/KASET letterforms; H, A, V as in tools/concepts.py) -------------
void quadTo (Path& p, Point<float> ctrl, Point<float> end) { p.quadraticTo (ctrl, end); }

void drawWordmark (Graphics& g, const String& name, float x, float y, float h, Colour col)
{
    const float w = 1.34f * h, t = 0.19f * h, r = 0.30f * h, gap = 0.30f * h, inn = t * 0.5f;
    Path all;
    float cx = x;
    for (auto ch : name)
    {
        const float L = cx + inn, R = cx + w - inn, T = y + inn, B = y + h - inn, M = y + h * 0.5f;
        Path p;
        if (ch == 'H')
        {
            p.startNewSubPath (L, T - inn); p.lineTo (L, B + inn);
            p.startNewSubPath (R, T - inn); p.lineTo (R, B + inn);
            p.startNewSubPath (L, M + t * 0.1f); p.lineTo (R, M + t * 0.1f);
        }
        else if (ch == 'A')
        {
            p.startNewSubPath (L, B + inn); p.lineTo (L, T + r); quadTo (p, { L, T }, { L + r, T });
            p.lineTo (R - r, T); quadTo (p, { R, T }, { R, T + r }); p.lineTo (R, B + inn);
            p.startNewSubPath (L, M + t * 0.35f); p.lineTo (R, M + t * 0.35f);
        }
        else if (ch == 'V')
        {
            p.startNewSubPath (L - inn * 0.2f, T - inn); p.lineTo ((L + R) * 0.5f, B); p.lineTo (R + inn * 0.2f, T - inn);
        }
        all.addPath (p);
        cx += w + gap;
    }
    const juce::PathStrokeType stroke (t, juce::PathStrokeType::curved, juce::PathStrokeType::butt);
    // The strokes are turned into an outline and filled (the KASET / TUTKAL way) instead of being
    // stroked by the renderer, so the letters come out the same on Windows (Direct2D) as on macOS.
    juce::Path outline;
    stroke.createStrokedPath (outline, all);
    g.setColour (rgba (0, 0, 0, 95));
    g.fillPath (outline, juce::AffineTransform::translation (0.0f, 1.3f));
    g.setColour (col);
    g.fillPath (outline);
}

// ---- knobs ----------------------------------------------------------------------------------------
/** The machined knob body: shadow, knurled skirt, turned cap, gold index. */
void knobBody (Graphics& g, float cx, float cy, float r, float val, bool enabled)
{
    soft (g, ellipse (cx, cy, r), r * 0.10f, juce::Colours::black, 0.55f, { r * 0.06f, r * 0.13f });
    fillGrad (g, ellipse (cx, cy, r), { cx - r, cy - r, 2 * r, 2 * r }, Colour (43, 48, 46), Colour (11, 13, 12), 90.0f);
    for (int i = 0; i < 60; ++i)
    {
        const float a = 360.0f * (float) i / 60.0f;
        const float light = 0.5f + 0.5f * std::cos (rad (a) + 0.8f);
        line (g, polar (cx, cy, r * 0.86f, a), polar (cx, cy, r * 0.985f, a), 0.55f,
              juce::Colours::white.withAlpha (0.05f + 0.15f * light));
    }
    const float cr = r * 0.74f;
    const Rectangle<float> capBox (cx - cr, cy - cr, 2 * cr, 2 * cr);
    fillGrad (g, ellipse (cx, cy, cr), capBox, Colour (222, 223, 218), Colour (110, 116, 113), 45.0f);
    const float step = juce::jmax (0.9f, cr / 26.0f);
    int i = 0;
    for (float rr = cr * 0.12f; rr < cr; rr += step, ++i)
    {
        g.setColour ((i % 2) != 0 ? rgba (255, 255, 255, 20) : rgba (0, 0, 0, 14));
        g.drawEllipse (cx - rr, cy - rr, 2 * rr, 2 * rr, 0.45f);
    }
    // Sheen: PIL pie slices 144..201 and -36..21 degrees (from 3 o'clock) = 234..291 and 54..111 from 12 o'clock.
    Path sheen;
    sheen.addPieSegment (capBox, rad (234.0f), rad (291.0f), 0.0f);
    sheen.addPieSegment (capBox, rad (54.0f), rad (111.0f), 0.0f);
    g.setColour (rgba (255, 255, 255, 50));
    g.fillPath (sheen);
    g.setColour (rgba (0, 0, 0, 90));
    g.drawEllipse (capBox, 0.8f);
    const float ang = -135.0f + 270.0f * val;
    line (g, polar (cx, cy, cr * 0.28f, ang), polar (cx, cy, r * 0.97f, ang), juce::jmax (2.0f, r * 0.13f), rgba (0, 0, 0, 128));
    line (g, polar (cx, cy, cr * 0.28f, ang), polar (cx, cy, r * 0.97f, ang), juce::jmax (1.3f, r * 0.08f),
          kGold.withMultipliedAlpha (enabled ? 1.0f : 0.45f));
}

/** Ticks, the three numbers, the name and the sub-legend around a knob. */
void knobScale (Graphics& g, float cx, float cy, float r, const String& label, const String& sub, float lsize, float labelDy,
                const String& n0 = "0", const String& n1 = "5", const String& n2 = "10")
{
    for (int i = 0; i <= 10; ++i)
    {
        const float a = -135.0f + 27.0f * (float) i;
        const bool major = i == 0 || i == 5 || i == 10;
        line (g, polar (cx, cy, r * 1.20f, a), polar (cx, cy, r * (major ? 1.42f : 1.31f), a), major ? 1.5f : 1.0f, kCream);
    }
    const std::pair<float, String> nums[] { { -135.0f, n0 }, { 0.0f, n1 }, { 135.0f, n2 } };
    for (const auto& [a, s] : nums)
    {
        const float ns = s.length() <= 3 ? juce::jmax (7.0f, r * 0.3f) : juce::jmax (7.0f, r * 0.22f);
        const auto p = polar (cx, cy, r * 1.66f, a);
        text (g, p.x, p.y + (juce::exactlyEqual (a, 0.0f) ? 0.0f : 1.0f), s, legend (ns), kCream);
    }
    lt (g, cx, cy + labelDy, label, lsize, 2.4f);
    if (sub.isNotEmpty())
        lt (g, cx, cy + labelDy + lsize * 1.25f, sub, lsize * 0.72f, 1.5f, true, Anchor::middle, kMuted);
}

// ---- lamps, keys, toggles ------------------------------------------------------------------------
void lamp (Graphics& g, float cx, float cy, float r, bool on, Colour col, Colour off)
{
    g.setColour (Colour (12, 14, 13));
    g.fillPath (ellipse (cx, cy, r * 1.4f));
    const Rectangle<float> box (cx - r, cy - r, 2 * r, 2 * r);
    if (on)
    {
        soft (g, ellipse (cx, cy, r * 3.0f), r * 1.2f, col, 0.55f);
        fillGrad (g, ellipse (cx, cy, r), box, Colour (255, 246, 222), col, 60.0f);
    }
    else
    {
        fillGrad (g, ellipse (cx, cy, r), box, off,
                  Colour ((juce::uint8) (off.getRed() * 0.4f), (juce::uint8) (off.getGreen() * 0.4f), (juce::uint8) (off.getBlue() * 0.4f)), 60.0f);
    }
    g.setColour (rgba (255, 255, 255, 70));
    g.fillEllipse (Rectangle<float>::leftTopRightBottom (cx - r * 0.55f, cy - r * 0.7f, cx - r * 0.05f, cy - r * 0.25f));
}

void redLed (Graphics& g, float cx, float cy, float r, bool on) { lamp (g, cx, cy, r, on, kLedRed, kLampRedOff); }

void keyCap (Graphics& g, Rectangle<float> box, const String& label, bool lit, float size, float track, bool down)
{
    const auto b = box.translated (0.0f, down ? 1.0f : 0.0f);
    g.setColour (Colour (8, 10, 9));
    g.fillRoundedRectangle (box.expanded (3.5f), 5.0f);
    soft (g, rrect (b, 3.0f), 1.5f, juce::Colours::black, 0.5f, { 1.0f, 2.5f });
    if (lit)
    {
        soft (g, rrect (b.expanded (6.0f), 6.0f), 7.0f, kGoldL, 0.35f);
        fillGrad (g, rrect (b, 3.0f), b, Colour (240, 212, 146), Colour (190, 152, 82), 90.0f);
    }
    else
        fillGrad (g, rrect (b, 3.0f), b, Colour (242, 238, 226), Colour (206, 199, 181), 90.0f);
    line (g, { b.getX() + 3.0f, b.getY() + 1.0f }, { b.getRight() - 3.0f, b.getY() + 1.0f }, 0.8f, rgba (255, 255, 255, 110));
    text (g, b.getCentreX(), b.getCentreY() + 0.5f, label, legend (size), kInk, Anchor::middle, track);
}

void toggleBody (Graphics& g, float cx, float cy, bool on)
{
    soft (g, ellipse (cx, cy, 12.0f), 2.0f, juce::Colours::black, 0.55f, { 1.5f, 3.0f });
    fillGrad (g, ellipse (cx, cy, 12.0f), { cx - 12.0f, cy - 12.0f, 24.0f, 24.0f }, Colour (232, 233, 229), Colour (96, 100, 98), 45.0f);
    g.setColour (Colour (40, 43, 42));
    g.fillPath (ellipse (cx, cy, 7.0f));
    const float tipy = cy + (on ? -30.0f : 30.0f);
    auto quad = [] (Point<float> a, Point<float> b, Point<float> c, Point<float> d)
    {
        Path p;
        p.startNewSubPath (a); p.lineTo (b); p.lineTo (c); p.lineTo (d); p.closeSubPath();
        return p;
    };
    g.setColour (rgba (0, 0, 0, 70));
    g.fillPath (quad ({ cx - 3.4f + 3, cy + 3 }, { cx + 3.4f + 3, cy + 3 }, { cx + 2.2f + 3, tipy + 3 }, { cx - 2.2f + 3, tipy + 3 }));
    g.setColour (Colour (200, 202, 198));
    g.fillPath (quad ({ cx - 3.4f, cy }, { cx + 3.4f, cy }, { cx + 2.2f, tipy }, { cx - 2.2f, tipy }));
    g.setColour (Colour (245, 246, 242));
    g.fillPath (quad ({ cx - 3.4f, cy }, { cx - 0.6f, cy }, { cx - 0.4f, tipy }, { cx - 2.2f, tipy }));
    soft (g, ellipse (cx, tipy, 5.5f), 1.2f, juce::Colours::black, 0.5f, { 2.0f, 3.0f });
    fillGrad (g, ellipse (cx, tipy, 5.5f), { cx - 5.5f, tipy - 5.5f, 11.0f, 11.0f }, Colour (250, 250, 247), Colour (120, 124, 121), 45.0f);
}

void toggleLegends (Graphics& g, float cx, float cy, const String& label)
{
    text (g, cx + 20.0f, cy - 26.0f, "ON", fine (7.5f), kCream);
    text (g, cx + 20.0f, cy + 26.0f, "OFF", fine (7.5f), kCream);
    if (label.isNotEmpty())
        lt (g, cx, cy + 50.0f, label, 10.5f, 2.4f);
}

// ---- red LEDs: windows, 7-segment digits, ladders -------------------------------------------------
void glowPaths (Graphics& g, const Path& lit, float blur, float strength)
{
    if (lit.isEmpty())
        return;
    juce::DropShadow (kLedRed.withAlpha (strength), juce::jmax (1, juce::roundToInt (blur * 2.2f)), {}).drawForPath (g, lit);
    g.setColour (kLedRed);
    g.fillPath (lit);
}

void redWindow (Graphics& g, Rectangle<float> b)
{
    g.setColour (Colour (12, 12, 12));
    g.fillRoundedRectangle (b.expanded (2.0f), 3.0f);
    g.setColour (Colour (28, 6, 5));
    g.fillRoundedRectangle (b, 2.0f);
}

const char* segmentsOf (juce::juce_wchar ch)
{
    switch (ch)
    {
        case '0': return "abcdef"; case '1': return "bc";    case '2': return "abged"; case '3': return "abgcd";
        case '4': return "fgbc";   case '5': return "afgcd"; case '6': return "afgedc"; case '7': return "abc";
        case '8': return "abcdefg"; case '9': return "abcdfg"; case '-': return "g";
        default: return "";
    }
}

/** A 7-segment string; a '.' lights the decimal point of the digit before it. */
void sevenDp (Graphics& g, float x, float y, float h, const String& s, float pitch)
{
    String digits;
    std::vector<int> dps;
    for (auto ch : s)
    {
        if (ch == '.') dps.push_back (digits.length() - 1);
        else digits += String::charToString (ch);
    }
    const float w = 0.56f * h, th = 0.13f * h, gp = th * 0.16f, sl = std::tan (rad (7.0f));
    Path lit;
    for (int i = 0; i < digits.length(); ++i)
    {
        const float ox = x + (float) i * pitch;
        auto map = [&] (float px, float py) { return Point<float> (ox + px - (py - h * 0.5f) * sl, y + py); };
        auto hs = [&] (float xa, float xb, float yc)
        {
            Path p;
            p.startNewSubPath (map (xa, yc)); p.lineTo (map (xa + th / 2, yc - th / 2)); p.lineTo (map (xb - th / 2, yc - th / 2));
            p.lineTo (map (xb, yc)); p.lineTo (map (xb - th / 2, yc + th / 2)); p.lineTo (map (xa + th / 2, yc + th / 2));
            p.closeSubPath();
            return p;
        };
        auto vs = [&] (float xc, float ya, float yb)
        {
            Path p;
            p.startNewSubPath (map (xc, ya)); p.lineTo (map (xc + th / 2, ya + th / 2)); p.lineTo (map (xc + th / 2, yb - th / 2));
            p.lineTo (map (xc, yb)); p.lineTo (map (xc - th / 2, yb - th / 2)); p.lineTo (map (xc - th / 2, ya + th / 2));
            p.closeSubPath();
            return p;
        };
        const float L = th / 2, R = w - th / 2, T = th / 2, M = h / 2, B = h - th / 2;
        const std::pair<char, Path> segs[] { { 'a', hs (L + gp, R - gp, T) }, { 'b', vs (R, T + gp, M - gp) }, { 'c', vs (R, M + gp, B - gp) },
                                             { 'd', hs (L + gp, R - gp, B) }, { 'e', vs (L, M + gp, B - gp) }, { 'f', vs (L, T + gp, M - gp) },
                                             { 'g', hs (L + gp, R - gp, M) } };
        const String on (segmentsOf (digits[i]));
        for (const auto& [k, p] : segs)
        {
            if (on.containsChar (k)) lit.addPath (p);
            else { g.setColour (kLedRedOff); g.fillPath (p); }
        }
        // decimal point
        const float d = 0.12f * h, dx = x + (float) (i + 1) * pitch - 0.13f * h;
        const Rectangle<float> dp (dx, y + h - d, d, d);
        if (std::find (dps.begin(), dps.end(), i) != dps.end())
            lit.addRectangle (dp);
        else
        {
            g.setColour (kLedRedOff);
            g.fillRect (dp);
        }
    }
    glowPaths (g, lit, h * 0.05f, 0.7f);
}

void ladder (Graphics& g, float x0, float x1, float y, int n, int litN)
{
    redWindow (g, Rectangle<float>::leftTopRightBottom (x0 - 5.0f, y - 8.0f, x1 + 5.0f, y + 8.0f));
    const float cw = (x1 - x0) / (float) n;
    Path lit;
    for (int i = 0; i < n; ++i)
    {
        const auto b = Rectangle<float>::leftTopRightBottom (x0 + (float) i * cw + 0.9f, y - 4.5f, x0 + (float) (i + 1) * cw - 0.9f, y + 4.5f);
        if (i < litN) lit.addRectangle (b);
        else { g.setColour (kLedRedOff); g.fillRect (b); }
    }
    glowPaths (g, lit, 2.0f, 0.8f);
}

// ---- the face ------------------------------------------------------------------------------------
const Rectangle<float> kWin = Rectangle<float>::leftTopRightBottom (450.0f, 34.0f, 1400.0f, 296.0f);
constexpr float kSecAir0 = 462.0f, kSecAir1 = 700.0f, kSecSheen0 = 720.0f, kSecSheen1 = 944.0f, kSecSafe0 = 964.0f, kSecSafe1 = 1388.0f;
constexpr float kKnobY = 458.0f, kKnobR = 58.0f;
constexpr float kRearKnobY = 228.0f, kRearKnobR = 40.0f;

void windowFrame (Graphics& g)
{
    const float x0 = kWin.getX(), y0 = kWin.getY(), x1 = kWin.getRight(), y1 = kWin.getBottom();
    soft (g, rrect (Rectangle<float>::leftTopRightBottom (x0 - 6, y0 - 6, x1 + 6, y1 + 6), 10.0f), 4.0f, juce::Colours::black, 0.6f, { 1.0f, 4.0f });
    const auto bezel = Rectangle<float>::leftTopRightBottom (x0 - 7, y0 - 7, x1 + 7, y1 + 7);
    fillGrad (g, rrect (bezel, 10.0f), bezel, Colour (30, 33, 32), Colour (4, 5, 5), 90.0f);
    fillGrad (g, rrect (kWin, 4.0f), kWin, Colour (245, 241, 229), Colour (222, 214, 193), 90.0f);
}

void labelWindow (Graphics& g, const DisplayState& st)
{
    const float x0 = kWin.getX(), y0 = kWin.getY(), x1 = kWin.getRight(), y1 = kWin.getBottom();
    // Background: the bezel shows at the rounded corners; the cream label on top.
    g.setGradientFill (grad (kWin.expanded (7.0f), Colour (30, 33, 32), Colour (4, 5, 5), 90.0f));
    g.fillRect (kWin);
    fillGrad (g, rrect (kWin, 4.0f), kWin, Colour (245, 241, 229), Colour (222, 214, 193), 90.0f);

    // headings + gold rule (the KASET cassette-label setting)
    text (g, kSecAir0 + 4, y0 + 16, utf ("AIR · TOP-BAND LIFT"), legend (8.0f), kInk, Anchor::left, 1.8f);
    text (g, kSecSheen0 + 4, y0 + 16, utf ("SHEEN · ACTIVITY"), legend (8.0f), kInk, Anchor::left, 1.8f);
    text (g, kSecSafe0 + 4, y0 + 16, "MASTER SAFE", legend (8.0f), kInk, Anchor::left, 1.8f);
    text (g, x1 - 14, y0 + 16, "SUBVERSELAB", legend (7.5f), kInk, Anchor::right, 1.8f);
    g.setColour (kGold);
    g.fillRect (Rectangle<float>::leftTopRightBottom (x0 + 12, y0 + 27, x1 - 12, y0 + 30));
    g.setColour (kGold.withAlpha ((juce::uint8) 150));
    for (float xs : { kSecSheen0 - 10, kSecSafe0 - 10, kSecSafe0 + 212 })
        g.fillRect (Rectangle<float>::leftTopRightBottom (xs, y0 + 40, xs + 1.0f, y1 - 26));

    // ---- AIR: the shelf target in 7-segment (6 x AIR dB at 11 kHz), the live lift on a 0..6 dB ladder
    const float ax = kSecAir0 + 4;
    redWindow (g, Rectangle<float>::leftTopRightBottom (ax, y0 + 42, ax + 118, y0 + 118));
    sevenDp (g, ax + 12, y0 + 52, 56, st.airDigits, 46);
    text (g, ax + 128, y0 + 58, "dB", legend (12.0f), kInk, Anchor::left, 1.2f);
    text (g, ax + 128, y0 + 78, "AT 11 kHz", legend (7.5f), kInk, Anchor::left, 1.0f);
    text (g, ax + 128, y0 + 92, "SET BY AIR", fine (7.0f), kInk2, Anchor::left, 0.8f);
    redLed (g, ax + 134, y0 + 111, 3.0f, st.liftOn);
    text (g, ax + 143, y0 + 111.5f, "LIFT", legend (7.5f), st.liftOn ? kInk : kInk2, Anchor::left, 1.2f);
    const float lx0 = ax + 6, lx1 = kSecAir1 - 10, ly = y0 + 170;
    text (g, ax, ly - 22, utf ("LIVE · dB ADDED ABOVE 8 kHz"), legend (7.5f), kInk, Anchor::left, 1.2f);
    ladder (g, lx0, lx1, ly, 12, st.liftLamps);
    for (int db = 0; db <= 6; ++db)
    {
        const float xx = lx0 + (lx1 - lx0) * (float) db / 6.0f;
        line (g, { xx, ly + 9 }, { xx, ly + 14 }, 1.0f, kInk);
        text (g, xx, ly + 22, String (db), legend (7.5f), kInk);
    }
    text (g, ax, y1 - 14, "AIR 10 = +6 dB SHELF AT 11 kHz", fine (7.0f), kInk2, Anchor::left, 0.8f);

    // ---- SHEEN: activity ladders (no time, no decay read-out: a colour, not a room)
    const float sx0 = kSecSheen0, sx1 = kSecSheen1;
    redLed (g, sx0 + 10, y0 + 50, 4.2f, st.sheenOn);
    text (g, sx0 + 22, y0 + 50.5f, "SHEEN ON", legend (9.0f), st.sheenOn ? kInk : kInk2, Anchor::left, 1.6f);
    text (g, sx0 + 4, y0 + 74, utf ("CHIRP · TWANG · SPARKLE"), legend (7.5f), kInk, Anchor::left, 1.2f);
    text (g, sx0 + 4, y0 + 88, "SPRING RETURN RE INPUT, dB", fine (7.0f), kInk2, Anchor::left, 0.8f);
    const float bx0 = sx0 + 24, bx1 = sx1 - 12, by = y0 + 116;
    for (int i = 0; i < 2; ++i)
    {
        const float yy = by + (float) i * 22.0f;
        text (g, bx0 - 12, yy + 0.5f, i == 0 ? "L" : "R", legend (8.0f), kInk);
        ladder (g, bx0, bx1, yy, 16, st.sheenLamps[i]);
    }
    const char* scale[] { "\xe2\x88\x92" "30", "\xe2\x88\x92" "24", "\xe2\x88\x92" "18", "\xe2\x88\x92" "12", "\xe2\x88\x92" "6", "0" };
    for (int k = 0; k < 6; ++k)
    {
        const float xx = bx0 + (bx1 - bx0) * (float) k / 5.0f;
        line (g, { xx, by + 31 }, { xx, by + 36 }, 1.0f, kInk);
        text (g, xx, by + 44, utf (scale[k]), legend (7.5f), kInk);
    }
    text (g, sx0 + 4, y0 + 214, "LIGHTS ON TRANSIENTS,", fine (7.0f), kInk2, Anchor::left, 0.8f);
    text (g, sx0 + 4, y0 + 226, "FALLS AWAY AT ONCE", fine (7.0f), kInk2, Anchor::left, 0.8f);
    text (g, sx0 + 4, y1 - 14, utf ("SHEEN · SHORT SPRING GLUE · NOT A ROOM"), fine (7.0f), kInk2, Anchor::left, 0.8f);

    // ---- MASTER SAFE: MID ONLY, CEILING and LATENCY lamps, the auto-gain read-out
    const float mx = kSecSafe0 + 4;
    const bool on = st.safeOn;
    redLed (g, mx + 8, y0 + 50, 4.2f, on);
    text (g, mx + 20, y0 + 50.5f, "MID ONLY", legend (9.0f), on ? kInk : kInk2, Anchor::left, 1.6f);
    text (g, mx + 20, y0 + 64, "SIDE PASSES UNTOUCHED", fine (6.8f), kInk2, Anchor::left, 0.6f);
    redLed (g, mx + 8, y0 + 92, 4.2f, on);
    text (g, mx + 20, y0 + 92.5f, utf ("CEILING \xe2\x88\x92" "0.3 dBTP"), legend (9.0f), on ? kInk : kInk2, Anchor::left, 1.4f);
    text (g, mx + 20, y0 + 106, utf ("TRUE-PEAK, 4\xc3\x97 ESTIMATE"), fine (6.8f), kInk2, Anchor::left, 0.6f);
    redLed (g, mx + 8, y0 + 134, 4.2f, on);
    text (g, mx + 20, y0 + 134.5f, "LATENCY " + st.latency + " SMP", legend (9.0f), on ? kInk : kInk2, Anchor::left, 1.4f);
    text (g, mx + 20, y0 + 148, utf ("AT 48 kHz · 74 AT 44.1 kHz"), fine (6.8f), kInk2, Anchor::left, 0.6f);
    text (g, mx + 4, y1 - 14, "OFF: FULL STEREO, 0 LATENCY", fine (7.0f), kInk2, Anchor::left, 0.8f);
    const float gx = kSecSafe0 + 222;
    text (g, gx + 4, y0 + 50.5f, "AUTO-GAIN", legend (9.0f), on ? kInk : kInk2, Anchor::left, 1.6f);
    text (g, gx + 4, y0 + 64, "K-WEIGHTED, HOLDS LOUDNESS", fine (6.8f), kInk2, Anchor::left, 0.6f);
    redWindow (g, Rectangle<float>::leftTopRightBottom (gx + 4, y0 + 82, gx + 142, y0 + 150));
    sevenDp (g, gx + 14, y0 + 91, 48, st.gainDigits, 38);
    text (g, gx + 150, y0 + 104, "dB", legend (12.0f), kInk, Anchor::left, 1.0f);
    text (g, gx + 4, y0 + 176, utf ("\xc2\xb1" "12 dB RANGE, 1.5 S WINDOW"), fine (6.8f), kInk2, Anchor::left, 0.6f);
    text (g, gx + 4, y0 + 188, "DARK WHEN MASTER SAFE IS OFF", fine (6.8f), kInk2, Anchor::left, 0.6f);
    juce::ignoreUnused (kSecSafe1);
}

juce::Image& frontPlate()
{
    static juce::Image img = makePlate (HavaEditor::kW, HavaEditor::kH, Colour (26, 70, 66), Colour (9, 42, 39), 9.0f, 3.2f, 77);
    return img;
}

juce::Image& rearPlate()
{
    static juce::Image img = makePlate (1200, 440, Colour (16, 52, 49), Colour (6, 32, 30), 6.0f, 3.0f, 11);
    return img;
}

/** A logo image drawn at a height, anchored at its right edge; returns its width. */
float logoRight (Graphics& g, const juce::Image& img, float xRight, float yMid, float h)
{
    if (img.isNull())
        return 0.0f;
    const float w = h * (float) img.getWidth() / (float) img.getHeight();
    g.setOpacity (1.0f);
    g.drawImage (img, Rectangle<float> (xRight - w, yMid - h * 0.5f, w, h), juce::RectanglePlacement::stretchToFit);
    return w;
}

void setupKnob (FrontPanel::Knob& k, juce::AudioProcessorValueTreeState& apvts, const juce::String& id, juce::Component& parent)
{
    k.id = id;
    k.slider.setSliderStyle (juce::Slider::RotaryVerticalDrag);
    k.slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    k.slider.setRotaryParameters (kStart, kEnd, true);
    k.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, id, k.slider);
    if (auto* p = apvts.getParameter (id))
        k.slider.setDoubleClickReturnValue (true, (double) p->convertFrom0to1 (p->getDefaultValue()));
    k.slider.setPopupDisplayEnabled (true, true, &parent);
    parent.addAndMakeVisible (k.slider);
}

void placeKnob (juce::Slider& s, float cx, float cy, float r)
{
    const float half = r * 1.3f;
    s.setBounds (Rectangle<float> (cx - half, cy - half, 2 * half, 2 * half).toNearestInt());
}
} // namespace

//==============================================================================
HavaLookAndFeel::HavaLookAndFeel()
{
    // Menus, tooltips and buttons too: nothing on the panel asks the system for a font.
    setDefaultSansSerifTypeface (faces->regular);
    setColour (juce::PopupMenu::backgroundColourId, Theme::panelDeep);
    setColour (juce::PopupMenu::textColourId, Theme::ink);
    setColour (juce::TooltipWindow::backgroundColourId, Theme::panelDeep);
    setColour (juce::BubbleComponent::backgroundColourId, Theme::panelDeep);
    setColour (juce::BubbleComponent::outlineColourId, Theme::gold);
    setColour (juce::TooltipWindow::textColourId, Theme::ink);
}

void HavaLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                         float, float, juce::Slider& s)
{
    const float r = (float) juce::jmin (w, h) * 0.5f / 1.3f;
    knobBody (g, (float) x + (float) w * 0.5f, (float) y + (float) h * 0.5f, r, pos, s.isEnabled());
}

//==============================================================================
KeyButton::KeyButton (const juce::String& t, float fontSize, float track) : juce::Button (t), size (fontSize), tracking (track) {}

void KeyButton::paintButton (juce::Graphics& g, bool, bool down)
{
    // The cap sits 12 units inside the component, leaving room for the gold glow of a lit key.
    const auto box = getLocalBounds().toFloat().reduced (12.0f);
    keyCap (g, box, getButtonText(), isLit != nullptr && isLit(), size, tracking, down);
}

void BatToggle::paintButton (juce::Graphics& g, bool, bool)
{
    const auto c = getLocalBounds().toFloat().getCentre();
    toggleBody (g, c.x, c.y, getToggleState());
}

void LabelWindow::paint (juce::Graphics& g)
{
    g.addTransform (juce::AffineTransform::translation ((float) -getX(), (float) -getY()));
    labelWindow (g, state);
}

//==============================================================================
FrontPanel::FrontPanel (HavaProcessor& p) : proc (p)
{
    mark = juce::ImageCache::getFromMemory (BinaryData::mark_cream_png, BinaryData::mark_cream_pngSize);
    wordmark = juce::ImageCache::getFromMemory (BinaryData::wordmark_cream_png, BinaryData::wordmark_cream_pngSize);

    addAndMakeVisible (window);
    setupKnob (air, proc.apvts, "air", *this);
    setupKnob (sheen, proc.apvts, "sheen", *this);
    setupKnob (output, proc.apvts, "output", *this);

    addAndMakeVisible (bypass);
    addAndMakeVisible (masterSafe);
    bypassAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "bypass", bypass);
    safeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "masterSafe", masterSafe);
    bypass.setTitle ("Bypass");
    masterSafe.setTitle ("Master Safe");

    for (int i = 0; i < HavaProcessor::kNumMoods; ++i)
    {
        auto k = std::make_unique<KeyButton> (HavaProcessor::moodNames[i], 8.2f, 1.0f);
        k->isLit = [this, i] { return (int) proc.apvts.getRawParameterValue ("mood")->load() == i; };
        k->onClick = [this, i] { proc.selectMood (i); };
        addAndMakeVisible (*k);
        moods.push_back (std::move (k));
    }
    addAndMakeVisible (rearKey);
}

void FrontPanel::resized()
{
    window.setBounds (kWin.toNearestInt());
    placeKnob (air.slider, 581.0f, kKnobY, kKnobR);
    placeKnob (sheen.slider, 832.0f, kKnobY, kKnobR);
    placeKnob (output.slider, 1176.0f, kKnobY, kKnobR);
    bypass.setBounds (1478 - 24, 118 - 40, 48, 80);
    masterSafe.setBounds (124 - 24, 440 - 40, 48, 80);

    // MOOD keys: 106 x 38, three over two, centred under the MOOD bracket.
    const float bw = 106.0f, bh = 38.0f, gp = 13.0f;
    for (int i = 0; i < (int) moods.size(); ++i)
    {
        const int row = i / 3, col = i % 3, inRow = row == 0 ? 3 : 2;
        const float xStart = 73.0f + (348.0f - ((float) inRow * bw + (float) (inRow - 1) * gp)) * 0.5f;
        const float x0 = xStart + (float) col * (bw + gp), y0 = 190.0f + (float) row * (bh + 14.0f);
        moods[(size_t) i]->setBounds (Rectangle<float> (x0, y0, bw, bh).expanded (12.0f).toNearestInt());
    }
    rearKey.setBounds (Rectangle<float>::leftTopRightBottom (1434.0f, 246.0f, 1522.0f, 280.0f).expanded (12.0f).toNearestInt());
}

void FrontPanel::paint (juce::Graphics& g)
{
    g.drawImageAt (frontPlate(), 0, 0);
    rack (g);

    // Name block, MOOD bracket.
    drawWordmark (g, "HAVA", 72.0f, 34.0f, 50.0f, kCream);
    lt (g, 74.0f, 110.0f, utf ("TOP-END AIR · SPRING SHEEN"), 10.5f, 3.0f, false, Anchor::left);
    {
        const String model ("MODEL HV-72");
        const float tw = textWidth (legend (10.0f), model, 2.4f), x = 74.0f, y = 138.0f;
        g.setColour (kGold);
        g.drawRoundedRectangle (Rectangle<float>::leftTopRightBottom (x, y - 11.0f, x + tw + 22.0f, y + 11.0f), 1.0f, 1.3f);
        text (g, x + 11.0f, y + 0.5f, model, legend (10.0f), kGold, Anchor::left, 2.4f);
    }
    bracket (g, 72.0f, 420.0f, 172.0f, "MOOD");

    // The label window's frame and shadow (the window component paints the label itself).
    windowFrame (g);

    // Right column: BYPASS, REAR.
    toggleLegends (g, 1478.0f, 118.0f, "BYPASS");
    lt (g, 1478.0f, 292.0f, "SET-UP PANEL", 7.0f, 1.2f, true, Anchor::middle, kMuted);

    groove (g, 318.0f);

    // The three big knobs, each under its own section of the window.
    bracket (g, kSecAir0 + 6, kSecAir1 - 6, 334.0f, "AIR");
    knobScale (g, 581.0f, kKnobY, kKnobR, "AIR", "TOP-END LIFT", 12.0f, kKnobR * 1.86f);
    bracket (g, kSecSheen0 + 6, kSecSheen1 - 6, 334.0f, "SHEEN");
    knobScale (g, 832.0f, kKnobY, kKnobR, "SHEEN", "SPRING COLOR", 12.0f, kKnobR * 1.86f);
    bracket (g, kSecSafe0 + 60, kSecSafe1 - 60, 334.0f, "OUT");
    knobScale (g, 1176.0f, kKnobY, kKnobR, "OUTPUT", "dB", 12.0f, kKnobR * 1.86f, utf ("\xe2\x88\x92" "12"), "0", "+12");

    // Bottom left: MASTER SAFE and its ceiling lamp.
    bracket (g, 72.0f, 420.0f, 334.0f, "MASTER SAFE");
    toggleLegends (g, 124.0f, 440.0f, {});
    lt (g, 124.0f, 500.0f, "MASTER SAFE", 10.5f, 2.4f);
    lamp (g, 204.0f, 404.0f, 5.5f, lampOn, kLedRed, kLampRedOff);
    lt (g, 218.0f, 404.0f, utf ("CEILING \xe2\x88\x92" "0.3 dBTP · ") + String (latencyShown) + " SMP", 8.0f, 1.2f, false, Anchor::left);
    const char* notes[] { "MID ONLY + AUTO-GAIN.", "FOR THE MIX BUS AND MASTER.", "80 SAMPLES AT 48 kHz,", "74 AT 44.1 kHz; OFF = 0." };
    for (int i = 0; i < 4; ++i)
        lt (g, 200.0f, 432.0f + (float) i * 13.0f, notes[i], 7.0f, 1.0f, true, Anchor::left, kMuted);

    // Footer: the name plate, the motto, the maker's badge.
    {
        const String t = utf ("HAVA · MODEL HV-72 · STUDIO AIR UNIT");
        const float tw = textWidth (legend (9.5f), t, 2.6f), cx = 800.0f;
        g.setColour (kGold);
        g.drawRoundedRectangle (Rectangle<float>::leftTopRightBottom (cx - tw * 0.5f - 18.0f, 597.0f, cx + tw * 0.5f + 18.0f, 623.0f), 1.0f, 1.3f);
        text (g, cx, 610.5f, t, legend (9.5f), kGold, Anchor::middle, 2.6f);
        lt (g, 72.0f, 611.0f, "MEASURED, NOT GUESSED", 7.5f, 1.8f, true, Anchor::left, kMuted);
        const float wl = logoRight (g, wordmark, 1530.0f, 611.0f, 17.0f);
        logoRight (g, mark, 1530.0f - wl - 8.0f, 610.0f, 26.0f);
    }
}

//==============================================================================
RearPanel::RearPanel (HavaProcessor& p) : proc (p)
{
    mark = juce::ImageCache::getFromMemory (BinaryData::mark_cream_png, BinaryData::mark_cream_pngSize);
    wordmark = juce::ImageCache::getFromMemory (BinaryData::wordmark_cream_png, BinaryData::wordmark_cream_pngSize);
    setupKnob (twang, proc.apvts, "twang", *this);
    setupKnob (tension, proc.apvts, "tension", *this);
    setupKnob (drip, proc.apvts, "drip", *this);
    setupKnob (lowCut, proc.apvts, "lowCut", *this);
    addAndMakeVisible (frontKey);
}

void RearPanel::resized()
{
    placeKnob (twang.slider, 150.0f, kRearKnobY, kRearKnobR);
    placeKnob (tension.slider, 400.0f, kRearKnobY, kRearKnobR);
    placeKnob (drip.slider, 650.0f, kRearKnobY, kRearKnobR);
    placeKnob (lowCut.slider, 998.0f, kRearKnobY, kRearKnobR);
    // Not on the concept plate: the key back to the front, at the plate's bottom right.
    frontKey.setBounds (Rectangle<float>::leftTopRightBottom (1068.0f, 380.0f, 1156.0f, 408.0f).expanded (12.0f).toNearestInt());
}

void RearPanel::paint (juce::Graphics& g)
{
    g.drawImageAt (rearPlate(), 0, 0);
    g.setColour (juce::Colours::black);
    g.drawRect (Rectangle<float> (0.0f, 0.0f, 1200.0f, 440.0f), 3.0f);
    for (auto [x, y] : { std::pair<float, float> { 18, 18 }, std::pair<float, float> { 1182, 18 }, std::pair<float, float> { 18, 422 }, std::pair<float, float> { 1182, 422 } })
        screw (g, x, y, 7.5f);
    lt (g, 44.0f, 40.0f, utf ("HAVA · MODEL HV-72 · REAR PANEL"), 11.0f, 3.0f, false, Anchor::left);
    lt (g, 44.0f, 58.0f, "SET-UP FOR THE SPRING SHEEN. EVERY MOOD LOADS THE MARKED DEFAULTS; CHANGE THEM ONLY TO OVERRIDE.",
        7.5f, 1.2f, true, Anchor::left, kMuted);
    const float wl = logoRight (g, wordmark, 1156.0f, 42.0f, 14.0f);
    logoRight (g, mark, 1156.0f - wl - 8.0f, 41.0f, 22.0f);
    groove (g, 76.0f, 30.0f, 1170.0f);

    const float r = kRearKnobR, cy = kRearKnobY;
    bracket (g, 44.0f, 760.0f, 110.0f, "SPRING SHEEN");
    knobScale (g, 150.0f, cy, r, "TWANG", "+10 dB AT 1.5 kHz", 10.5f, r * 2.0f);
    knobScale (g, 400.0f, cy, r, "TENSION", "CHIRP LENGTH", 10.5f, r * 2.0f);
    knobScale (g, 650.0f, cy, r, "DRIP", "TRANSIENT DRIP", 10.5f, r * 2.0f);
    bracket (g, 840.0f, 1156.0f, 110.0f, "SHEEN SEND");
    knobScale (g, 998.0f, cy, r, "LOW CUT", "Hz", 10.5f, r * 2.0f, "20", "80", "300");

    // Gold dots: where every MOOD puts the control (the parameter defaults).
    const std::pair<float, const char*> dots[] { { 150.0f, "twang" }, { 400.0f, "tension" }, { 650.0f, "drip" }, { 998.0f, "lowCut" } };
    g.setColour (kGold);
    for (auto [cx, id] : dots)
    {
        const float v = proc.apvts.getParameter (id)->getDefaultValue();
        const auto p = polar (cx, cy, r * 1.2f, -135.0f + 270.0f * v);
        g.fillEllipse (p.x - 2.6f, p.y - 2.6f, 5.2f, 5.2f);
    }
    lt (g, 998.0f, 342.0f, "FILTERS THE SHEEN SEND ONLY.", 7.0f, 1.0f, true, Anchor::middle, kMuted);
    lt (g, 998.0f, 354.0f, "THE DRY IS NEVER FILTERED.", 7.0f, 1.0f, true, Anchor::middle, kMuted);
    lt (g, 600.0f, 396.0f, utf ("\xe2\x97\x8f = DEFAULT, LOADED BY EVERY MOOD (TWANG 7 · TENSION 5.5 · DRIP 5 · LOW CUT 80 Hz)"),
        7.5f, 1.2f, true, Anchor::middle, kMuted);
}

//==============================================================================
HavaEditor::HavaEditor (HavaProcessor& p)
    : AudioProcessorEditor (&p), proc (p), front (p), rear (p)
#if SVL_LICENSING
    , licenseOverlay (p.license)
#endif
{
    setLookAndFeel (&lnf);
    setWantsKeyboardFocus (true);
    addMouseListener (this, true);
    front.setBounds (0, 0, kW, kH);
    rear.setBounds (0, 0, 1200, 440);
    addAndMakeVisible (front);
    addChildComponent (rear);
    front.rearKey.onClick = [this] { showRear (true); };
    rear.frontKey.onClick = [this] { showRear (false); };

    // Scalable: the face keeps its 5:2 proportions.
    setResizable (true, true);
    setResizeLimits (800, 320, 2400, 960);
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio ((double) kW / (double) kH);
    const int w = proc.editorWidth >= 800 ? juce::jmin (2400, proc.editorWidth) : 1280;
    setSize (w, w * kH / kW);

#if SVL_LICENSING
    licenseButton.setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    licenseButton.setColour (juce::TextButton::textColourOffId, Theme::ink.withAlpha (0.6f));
    licenseButton.onClick = [this] { licenseOverlay.refresh(); licenseOverlay.setVisible (true); licenseOverlay.toFront (true); };
    addAndMakeVisible (licenseButton);
    addChildComponent (licenseOverlay);
    licenseOverlay.setVisible (! proc.license.isLicensed());
#endif
    lastTick = juce::Time::getMillisecondCounterHiRes();
    tick (0.0);
    startTimerHz (30);
}

bool HavaEditor::keyPressed (const juce::KeyPress& key)
{
#if SVL_LICENSING
    if (licenseOverlay.isVisible())
        return false;
#endif
    return svl::handleUndoKey (proc.undo, key);
}

void HavaEditor::mouseDown (const juce::MouseEvent& e)
{
#if SVL_LICENSING
    if (licenseOverlay.isVisible())
        return;
#endif
    svl::takeFocusOnClick (*this, e);
}

HavaEditor::~HavaEditor()
{
    setLookAndFeel (nullptr);
}

void HavaEditor::paint (juce::Graphics& g)
{
    // Behind the rear plate (its proportions are wider than the face's).
    g.fillAll (Colour (4, 26, 24));
}

void HavaEditor::resized()
{
    const float s = (float) getWidth() / (float) kW;
    front.setTransform (juce::AffineTransform::scale (s));
    const float rs = s * (float) kW / 1200.0f;
    rear.setTransform (juce::AffineTransform::scale (rs).translated (0.0f, ((float) getHeight() - 440.0f * rs) * 0.5f));
    proc.editorWidth = getWidth();
#if SVL_LICENSING
    licenseOverlay.setBounds (getLocalBounds());
    licenseButton.setBounds (getWidth() / 2 + juce::roundToInt (260.0f * s), getHeight() - juce::roundToInt (28.0f * s), 70, 20);
#endif
}

void HavaEditor::showRear (bool showIt)
{
    rear.setVisible (showIt);
    front.setVisible (! showIt);
}

void HavaEditor::settleForSnapshot()
{
    liftShown = 0.0f;
    sheenShown[0] = sheenShown[1] = -120.0f;
    tick (0.0);
}

void HavaEditor::timerCallback()
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    tick ((now - lastTick) * 0.001);
    lastTick = now;
}

void HavaEditor::tick (double dt)
{
#if SVL_LICENSING
    if (! proc.license.isLicensed() && ! licenseOverlay.isVisible())
    {
        licenseOverlay.refresh();
        licenseOverlay.setVisible (true);
    }
#endif
    auto v = [this] (const char* id) { return proc.apvts.getRawParameterValue (id)->load(); };
    const float air = v ("air"), sheen = v ("sheen");
    const bool safe = v ("masterSafe") > 0.5f, bypassed = v ("bypass") > 0.5f;

    // Ballistics: the ladders jump up and fall back (the lift slowly, the sheen at once).
    const float fdt = (float) dt;
    liftShown = juce::jmax (proc.uiAirLiftDb.load(), liftShown - 12.0f * fdt);
    for (int k = 0; k < 2; ++k)
        sheenShown[k] = juce::jmax (proc.uiSheenReDb[k].load(), sheenShown[k] - 45.0f * fdt);

    DisplayState st;
    st.airDigits = String (6.0f * air, 1);
    st.liftOn = air > 0.0f && ! bypassed;
    st.sheenOn = (air > 0.0f || sheen > 0.0f) && ! bypassed;
    st.liftLamps = juce::jlimit (0, 12, (int) std::floor (liftShown / 0.5f + 1.0e-4f));
    for (int k = 0; k < 2; ++k)
        st.sheenLamps[k] = juce::jlimit (0, 16, (int) std::ceil ((sheenShown[k] + 30.0f) / 1.875f - 1.0e-4f));
    st.safeOn = safe;
    st.latency = String (proc.safeLatency());
    if (safe && proc.uiSafeActive.load())
    {
        float ag = proc.uiAutoGainDb.load();
        if (std::abs (ag) < 0.05f)
            ag = 0.0f;
        const String s = std::abs (ag) < 9.95f ? String::formatted ("%+.1f", (double) ag) : String::formatted ("%+.0f", (double) ag);
        st.gainDigits = s.replaceCharacter ('+', ' ');
    }
    else
        st.gainDigits = "---";
    front.window.show (st);

    if (front.lampOn != safe || front.latencyShown != proc.safeLatency())
    {
        front.lampOn = safe;
        front.latencyShown = proc.safeLatency();
        front.repaint (180, 380, 240, 50);
    }
    const int mood = (int) v ("mood");
    if (mood != lastMood)
    {
        lastMood = mood;
        for (auto& k : front.moods)
            k->repaint();
    }
}
