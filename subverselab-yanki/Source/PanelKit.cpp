#include "PanelKit.h"
#include "Theme.h"

namespace kit
{
namespace
{
constexpr float kPi = juce::MathConstants<float>::pi;

float rad (float deg) { return deg * kPi / 180.0f; }

// Standard normal CDF.
float phi (float x) { return 0.5f * std::erfc (-x / std::sqrt (2.0f)); }

/** Layered fills that add up to a Gaussian-blurred edge (sigma = blur): a
    shape grown by e gets the alpha step between the blurred coverage at e and
    at the next, larger e. Resolution independent, so the cached plate and a
    live repaint look the same at every scale. */
template <typename Draw>
void blurredLayers (Graphics& g, Colour colour, float opacity, float blur, float fromSigma, float toSigma, Draw&& draw)
{
    constexpr int n = 14;
    float prev = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const float e = blur * (fromSigma + (toSigma - fromSigma) * (float) i / (float) (n - 1));   // largest first
        const float target = opacity * phi (-e / juce::jmax (blur, 1.0e-4f));
        const float a = prev >= 1.0f ? 0.0f : juce::jlimit (0.0f, 1.0f, 1.0f - (1.0f - target) / (1.0f - prev));
        prev = target;
        if (a <= 0.0f)
            continue;
        g.setColour (colour.withMultipliedAlpha (a));
        draw (e);
    }
}

juce::Path polyPath (const std::vector<Point<float>>& pts)
{
    juce::Path p;
    for (size_t i = 0; i < pts.size(); ++i)
    {
        if (i == 0) p.startNewSubPath (pts[i]);
        else p.lineTo (pts[i]);
    }
    p.closeSubPath();
    return p;
}

std::vector<Point<float>> quad (Point<float> p0, Point<float> p1, Point<float> p2, int n = 12)
{
    std::vector<Point<float>> out;
    for (int i = 0; i < n; ++i)
    {
        const float t = (float) i / (float) (n - 1);
        out.push_back ({ (1 - t) * (1 - t) * p0.x + 2 * (1 - t) * t * p1.x + t * t * p2.x,
                         (1 - t) * (1 - t) * p0.y + 2 * (1 - t) * t * p1.y + t * t * p2.y });
    }
    return out;
}
} // namespace

Point<float> polar (float cx, float cy, float r, float deg)
{
    const float a = rad (deg);
    return { cx + r * std::sin (a), cy - r * std::cos (a) };
}

juce::Font font (Face f, float size)
{
    switch (f)
    {
        case Face::fine: return Theme::fineEm (size);
        case Face::mono: return Theme::monoEm (size);
        case Face::legend: break;
    }
    return Theme::legendEm (size);
}

float tlen (const String& s, Face f, float size, float track)
{
    const auto fo = font (f, size);
    if (track == 0.0f)
        return juce::GlyphArrangement::getStringWidth (fo, s);
    float tot = 0.0f;
    for (auto ch : s)
        tot += juce::GlyphArrangement::getStringWidth (fo, String::charToString (ch));
    return tot + track * (float) juce::jmax (0, s.length() - 1);
}

float text (Graphics& g, float x, float y, const String& s, Face f, float size, Colour col, const char* anchor, float track)
{
    const auto fo = font (f, size);
    const float w = tlen (s, f, size, track);
    const char h = anchor[0];
    float x0 = h == 'm' ? x - w * 0.5f : h == 'r' ? x - w : x;
    const float baseline = y + (fo.getAscent() - fo.getDescent()) * 0.5f;
    g.setFont (fo);
    g.setColour (col);
    if (track == 0.0f)
    {
        juce::GlyphArrangement ga;
        ga.addLineOfText (fo, s, x0, baseline);
        ga.draw (g);
        return w;
    }
    for (auto ch : s)
    {
        const auto cs = String::charToString (ch);
        juce::GlyphArrangement ga;
        ga.addLineOfText (fo, cs, x0, baseline);
        ga.draw (g);
        x0 += juce::GlyphArrangement::getStringWidth (fo, cs) + track;
    }
    return w;
}

float lt (Graphics& g, float x, float y, const String& s, float size, float track, Face f, const char* anchor, Colour col)
{
    return text (g, x, y, s, f, size, col, anchor, track);
}

void line (Graphics& g, Point<float> a, Point<float> b, float width, Colour c)
{
    g.setColour (c);
    g.drawLine ({ a, b }, width);
}

void poly (Graphics& g, const std::vector<Point<float>>& pts, Colour c)
{
    g.setColour (c);
    g.fillPath (polyPath (pts));
}

void rrect (Graphics& g, Box b, float r, std::optional<Colour> fill, std::optional<Colour> outline, float width)
{
    if (fill)
    {
        g.setColour (*fill);
        g.fillRoundedRectangle (b.rect(), r);
    }
    if (outline)
    {
        // PIL strokes the outline inside the box.
        g.setColour (*outline);
        g.drawRoundedRectangle (b.rect().reduced (width * 0.5f), juce::jmax (0.0f, r - width * 0.5f), width);
    }
}

void ellipse (Graphics& g, Box b, std::optional<Colour> fill, std::optional<Colour> outline, float width)
{
    if (fill)
    {
        g.setColour (*fill);
        g.fillEllipse (b.rect());
    }
    if (outline)
    {
        g.setColour (*outline);
        g.drawEllipse (b.rect().reduced (width * 0.5f), width);
    }
}

void soft (Graphics& g, Box b, float blur, Colour colour, float opacity, Point<float> offset, bool isEllipse, float radius)
{
    const auto r0 = b.rect().translated (offset.x, offset.y);
    blurredLayers (g, colour, opacity, blur, 2.6f, -2.6f, [&] (float e)
    {
        const auto r = r0.expanded (e);
        if (r.getWidth() <= 0.0f || r.getHeight() <= 0.0f)
            return;
        if (isEllipse)
            g.fillEllipse (r);
        else
            g.fillRoundedRectangle (r, juce::jmax (0.0f, radius + e));
    });
}

void grad (Graphics& g, Box b, Colour c0, Colour c1, float angle, bool isEllipse, float radius)
{
    const float w = b.x1 - b.x0, h = b.y1 - b.y0;
    if (w <= 0.0f || h <= 0.0f)
        return;
    // The generator's gradient runs along the angle in the box's normalised
    // coordinates: t = (x/w - 0.5) cos a + (y/h - 0.5) sin a, stretched to 0..1.
    const float a = rad (angle), cs = std::cos (a), sn = std::sin (a);
    const float pmax = 0.5f * (std::abs (cs) + std::abs (sn));
    const Point<float> G (cs / w / (2.0f * pmax), sn / h / (2.0f * pmax));
    const float g2 = G.x * G.x + G.y * G.y;
    const Point<float> D (G.x / g2, G.y / g2);
    const auto c = b.rect().getCentre();
    g.setGradientFill (juce::ColourGradient (c0, c - D * 0.5f, c1, c + D * 0.5f, false));
    if (isEllipse)
        g.fillEllipse (b.rect());
    else
        g.fillRoundedRectangle (b.rect(), radius);
}

void glowPolys (Graphics& g, const std::vector<std::vector<Point<float>>>& polys, Colour col, float blur, float strength)
{
    if (polys.empty())
        return;
    juce::Path all;
    for (auto& pl : polys)
        all.addPath (polyPath (pl));
    blurredLayers (g, col, strength, blur, 2.8f, 0.05f, [&] (float e)
    {
        g.fillPath (all);
        g.strokePath (all, juce::PathStrokeType (2.0f * e, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    });
    g.setColour (col);
    g.fillPath (all);
}

void plate (juce::Image& img, float scale, Colour top, Colour bottom, float streak, float grain, int seed)
{
    // The generator draws at K = 4.22 px/unit and reduces; noise that it
    // averages away is scaled down here by the same averaging.
    const int W = img.getWidth(), H = img.getHeight();
    const float K = 4.22f;
    const float rowsAvg = juce::jmax (1.0f, K / scale), areaAvg = juce::jmax (1.0f, (K / scale) * (K / scale));
    const float rowStd = 0.6f * streak / std::sqrt (rowsAvg), lowStd = 0.55f * streak / std::sqrt (rowsAvg);
    const float grainStd = grain / std::sqrt (areaAvg);
    const float spacing = juce::jmax (1.0f, 90.0f / K * scale);
    const int cols = (int) ((float) W / spacing) + 3;
    juce::Random rng (seed);
    auto gauss = [&rng]
    {
        // Irwin-Hall (sum of 4 uniforms), unit variance: close enough for grain.
        float s = 0.0f;
        for (int i = 0; i < 4; ++i) s += rng.nextFloat();
        return (s - 2.0f) * 1.7320508f;
    };
    std::vector<float> low ((size_t) cols);
    juce::Image::BitmapData bd (img, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < H; ++y)
    {
        const float t = H > 1 ? (float) y / (float) (H - 1) : 0.0f;
        const float br = (float) top.getRed() * (1 - t) + (float) bottom.getRed() * t;
        const float bgc = (float) top.getGreen() * (1 - t) + (float) bottom.getGreen() * t;
        const float bb = (float) top.getBlue() * (1 - t) + (float) bottom.getBlue() * t;
        const float rowN = gauss() * rowStd;
        for (auto& l : low) l = gauss() * lowStd;
        for (int x = 0; x < W; ++x)
        {
            const float lx = (float) x / spacing;
            const int i = (int) lx;
            const float f = lx - (float) i;
            const float v = rowN + low[(size_t) i] * (1 - f) + low[(size_t) i + 1] * f + gauss() * grainStd;
            auto c8 = [v] (float base) { return (juce::uint8) juce::jlimit (0, 255, (int) (base + v)); };
            bd.setPixelColour (x, y, Colour (c8 (br), c8 (bgc), c8 (bb)));
        }
    }
}

void screw (Graphics& g, float cx, float cy, float r)
{
    soft (g, { cx - r, cy - r, cx + r, cy + r }, 1.6f, juce::Colours::black, 0.6f, { 1.0f, 2.0f });
    grad (g, { cx - r, cy - r, cx + r, cy + r }, rgb (236, 237, 233), rgb (120, 124, 121), 45.0f);
    ellipse (g, { cx - r, cy - r, cx + r, cy + r }, {}, rgb (0, 0, 0, 90), 0.8f);
    for (float a : { 45.0f, 135.0f })
        line (g, polar (cx, cy, r * 0.62f, a), polar (cx, cy, r * 0.62f, a + 180.0f), 2.2f, rgb (40, 42, 41, 230));
}

void rack (Graphics& g)
{
    const float H = 640.0f;
    for (auto [x0, x1] : { std::pair<float, float> { 0.0f, 44.0f }, { 1556.0f, 1600.0f } })
    {
        g.setColour (rgb (0, 0, 0, 60));
        g.fillRect (juce::Rectangle<float> (x0, 0.0f, x1 - x0, H));
        const float cx = (x0 + x1) / 2.0f;
        for (float cy : { 62.0f, 578.0f })
        {
            rrect (g, { cx - 12, cy - 17, cx + 12, cy + 17 }, 12.0f, rgb (4, 8, 7, 235));
            screw (g, cx, cy, 8.5f);
        }
    }
    line (g, { 44, 0 }, { 44, H }, 1.2f, rgb (0, 0, 0, 110));
    line (g, { 45.5f, 0 }, { 45.5f, H }, 0.8f, rgb (255, 255, 255, 30));
    line (g, { 1556, 0 }, { 1556, H }, 1.2f, rgb (0, 0, 0, 110));
    line (g, { 1557.5f, 0 }, { 1557.5f, H }, 0.8f, rgb (255, 255, 255, 30));
}

void groove (Graphics& g, float y, float x0, float x1)
{
    line (g, { x0, y }, { x1, y }, 1.0f, rgb (0, 0, 0, 120));
    line (g, { x0, y + 1.2f }, { x1, y + 1.2f }, 0.8f, rgb (255, 255, 255, 34));
}

void bracket (Graphics& g, float x0, float x1, float y, const String& title, float size)
{
    const float cx = (x0 + x1) / 2.0f;
    const float tw = tlen (title, Face::legend, size, 3.0f);
    const float gap = 10.0f;
    line (g, { x0, y }, { cx - tw / 2 - gap, y }, 1.1f, CREAM);
    line (g, { cx + tw / 2 + gap, y }, { x1, y }, 1.1f, CREAM);
    line (g, { x0, y }, { x0, y + 7 }, 1.1f, CREAM);
    line (g, { x1, y }, { x1, y + 7 }, 1.1f, CREAM);
    lt (g, cx, y, title, size, 3.0f);
}

float wordmark (Graphics& g, const String& name, float x, float y, float h, Colour col)
{
    const float w = 1.34f * h, t = 0.19f * h, r = 0.30f * h, gap = 0.30f * h, inn = t * 0.5f;
    std::vector<std::vector<Point<float>>> paths;
    float cx = x;
    for (auto ch : name)
    {
        float adv = w;
        const float L = cx + inn, R = cx + w - inn, T = y + inn, B = y + h - inn, M = y + h * 0.5f;
        auto join = [] (std::vector<Point<float>> a, const std::vector<Point<float>>& b) { a.insert (a.end(), b.begin(), b.end()); return a; };
        if (ch == 'K')
        {
            paths.push_back ({ { L, T - inn }, { L, B + inn } });
            paths.push_back ({ { L, M + t * 0.2f }, { R, T - inn * 0.3f } });
            paths.push_back ({ { L + (R - L) * 0.36f, M - h * 0.12f }, { R + inn * 0.2f, B + inn } });
        }
        else if (ch == 'A')
        {
            auto p = join ({ { L, B + inn }, { L, T + r } }, quad ({ L, T + r }, { L, T }, { L + r, T }));
            p.push_back ({ R - r, T });
            p = join (p, quad ({ R - r, T }, { R, T }, { R, T + r }));
            p.push_back ({ R, B + inn });
            paths.push_back (p);
            paths.push_back ({ { L, M + h * 0.06f }, { R, M + h * 0.06f } });
        }
        else if (ch == 'Y')
        {
            const float mx = (L + R) / 2.0f;
            auto p = join ({ { L, T - inn }, { L, M - r } }, quad ({ L, M - r }, { L, M }, { L + r, M }));
            p.push_back ({ R - r, M });
            p = join (p, quad ({ R - r, M }, { R, M }, { R, M - r }));
            p.push_back ({ R, T - inn });
            paths.push_back (p);
            paths.push_back ({ { mx, M }, { mx, B + inn } });
        }
        else if (ch == 'N')
        {
            paths.push_back ({ { L, B + inn }, { L, T - inn } });
            paths.push_back ({ { L, T }, { R, B } });
            paths.push_back ({ { R, B + inn }, { R, T - inn } });
        }
        else if (ch == 'I')
        {
            adv = t;
            paths.push_back ({ { cx + inn, T - inn }, { cx + inn, B + inn } });
        }
        cx += adv + gap;
    }
    juce::Path all;
    for (auto& pl : paths)
    {
        juce::Path p;
        for (size_t i = 0; i < pl.size(); ++i)
        {
            if (i == 0) p.startNewSubPath (pl[i]);
            else p.lineTo (pl[i]);
        }
        all.addPath (p);
    }
    const juce::PathStrokeType stroke (t, juce::PathStrokeType::curved, juce::PathStrokeType::butt);
    // The strokes are turned into an outline and filled (the KASET / TUTKAL way) instead of being
    // stroked by the renderer, so the letters come out the same on Windows (Direct2D) as on macOS.
    juce::Path outline;
    stroke.createStrokedPath (outline, all);
    g.setColour (rgb (0, 0, 0, 95));
    g.fillPath (outline, juce::AffineTransform::translation (0.0f, 1.3f));
    g.setColour (col);
    g.fillPath (outline);
    return cx - gap - x;
}

void knobScale (Graphics& g, float cx, float cy, float r, const juce::StringArray& nums, bool ticks, const String& label, float lsize, const String& sub)
{
    if (ticks)
        for (int i = 0; i < 11; ++i)
        {
            const float a = -135.0f + 27.0f * (float) i;
            const bool major = i == 0 || i == 5 || i == 10;
            line (g, polar (cx, cy, r * 1.20f, a), polar (cx, cy, r * (major ? 1.42f : 1.31f), a), major ? 1.5f : 1.0f, CREAM);
        }
    if (nums.size() == 3)
    {
        const float angles[3] { -135.0f, 0.0f, 135.0f };
        for (int i = 0; i < 3; ++i)
        {
            const auto& s = nums[i];
            if (s.isEmpty())
                continue;
            const float ns = s.length() <= 3 ? juce::jmax (7.0f, r * 0.3f) : juce::jmax (7.0f, r * 0.22f);
            const auto p = polar (cx, cy, r * 1.66f, angles[i]);
            text (g, p.x, p.y + (angles[i] == 0.0f ? 0.0f : 1.0f), s, Face::legend, ns, CREAM);
        }
    }
    if (label.isNotEmpty())
        lt (g, cx, cy + r * 1.98f, label, lsize, 2.4f);
    if (sub.isNotEmpty())
        lt (g, cx, cy + r * 1.98f + lsize * 1.25f, sub, lsize * 0.72f, 1.5f, Face::fine, "mm", MUTED);
}

void knobBody (Graphics& g, float cx, float cy, float r, float val)
{
    soft (g, { cx - r, cy - r, cx + r, cy + r }, r * 0.10f, juce::Colours::black, 0.55f, { r * 0.06f, r * 0.13f });
    grad (g, { cx - r, cy - r, cx + r, cy + r }, rgb (43, 48, 46), rgb (11, 13, 12), 90.0f);
    for (int i = 0; i < 60; ++i)
    {
        const float a = 360.0f * (float) i / 60.0f;
        const float light = 0.5f + 0.5f * std::cos (rad (a) + 0.8f);
        line (g, polar (cx, cy, r * 0.86f, a), polar (cx, cy, r * 0.985f, a), 0.55f, rgb (255, 255, 255, (int) (255.0f * (0.05f + 0.15f * light))));
    }
    const float cr = r * 0.74f;
    grad (g, { cx - cr, cy - cr, cx + cr, cy + cr }, rgb (222, 223, 218), rgb (110, 116, 113), 45.0f);
    const float step = juce::jmax (0.9f, cr / 26.0f);
    int i = 0;
    for (float rr = cr * 0.12f; rr < cr; rr += step, ++i)
        ellipse (g, { cx - rr, cy - rr, cx + rr, cy + rr }, {}, (i % 2) ? rgb (255, 255, 255, 20) : rgb (0, 0, 0, 14), 0.45f);
    // PIL pieslice angles (0 = 3 o'clock) + 90 = JUCE angles (0 = 12 o'clock).
    juce::Path sheen;
    sheen.addPieSegment (cx - cr, cy - cr, cr * 2, cr * 2, rad (144 + 90), rad (201 + 90), 0.0f);
    sheen.addPieSegment (cx - cr, cy - cr, cr * 2, cr * 2, rad (-36 + 90), rad (21 + 90), 0.0f);
    g.setColour (rgb (255, 255, 255, 50));
    g.fillPath (sheen);
    ellipse (g, { cx - cr, cy - cr, cx + cr, cy + cr }, {}, rgb (0, 0, 0, 90), 0.8f);
    const float ang = -135.0f + 270.0f * val;
    line (g, polar (cx, cy, cr * 0.28f, ang), polar (cx, cy, r * 0.97f, ang), juce::jmax (2.0f, r * 0.13f), rgb (0, 0, 0, 128));
    line (g, polar (cx, cy, cr * 0.28f, ang), polar (cx, cy, r * 0.97f, ang), juce::jmax (1.3f, r * 0.08f), GOLD);
}

void knobMarks (Graphics& g, float cx, float cy, float r, const std::vector<std::pair<float, String>>& marks, float size)
{
    for (auto& [a, s] : marks)
    {
        line (g, polar (cx, cy, r * 1.20f, a), polar (cx, cy, r * 1.44f, a), 1.5f, CREAM);
        const auto p = polar (cx, cy, r * 1.78f, a);
        text (g, p.x, p.y, s, Face::legend, size, CREAM, "mm", 0.8f);
    }
}

void pointerScale (Graphics& g, float cx, float cy, float r, const juce::StringArray& labels, const std::vector<float>& angles,
                   float tsize, float ring, const String& label, float lsize)
{
    for (int i = 0; i < labels.size() && i < (int) angles.size(); ++i)
    {
        const float a = angles[(size_t) i];
        line (g, polar (cx, cy, r * 1.02f, a), polar (cx, cy, r * 1.18f, a), 1.0f, CREAM);
        const auto p = polar (cx, cy, r * ring, a);
        text (g, p.x, p.y, labels[i], Face::legend, tsize, CREAM);
    }
    if (label.isNotEmpty())
        lt (g, cx, cy + r * 2.1f, label, lsize, 2.4f);
}

void pointerBody (Graphics& g, float cx, float cy, float r, float ang)
{
    soft (g, { cx - r * 0.8f, cy - r * 0.8f, cx + r * 0.8f, cy + r * 0.8f }, r * 0.1f, juce::Colours::black, 0.6f, { r * 0.06f, r * 0.14f });
    poly (g, { polar (cx, cy, r * 0.55f, ang - 50), polar (cx, cy, r * 1.02f, ang), polar (cx, cy, r * 0.55f, ang + 50) }, rgb (20, 23, 22));
    grad (g, { cx - r * 0.78f, cy - r * 0.78f, cx + r * 0.78f, cy + r * 0.78f }, rgb (44, 49, 47), rgb (7, 9, 9), 45.0f);
    const auto tip = polar (cx, cy, r * 0.93f, ang);
    ellipse (g, { tip.x - r * 0.13f, tip.y - r * 0.13f, tip.x + r * 0.13f, tip.y + r * 0.13f }, rgb (22, 25, 24));
    ellipse (g, { cx - r * 0.78f, cy - r * 0.78f, cx + r * 0.78f, cy + r * 0.78f }, {}, rgb (255, 255, 255, 26), 0.7f);
    line (g, polar (cx, cy, r * 0.15f, ang), polar (cx, cy, r * 0.9f, ang), juce::jmax (1.4f, r * 0.09f), CREAM);
}

void lamp (Graphics& g, float cx, float cy, float r, bool on, Colour col, Colour off)
{
    ellipse (g, { cx - r * 1.4f, cy - r * 1.4f, cx + r * 1.4f, cy + r * 1.4f }, rgb (12, 14, 13));
    if (on)
    {
        soft (g, { cx - r * 3, cy - r * 3, cx + r * 3, cy + r * 3 }, r * 1.2f, col, 0.55f);
        grad (g, { cx - r, cy - r, cx + r, cy + r }, rgb (255, 246, 222), col, 60.0f);
    }
    else
        grad (g, { cx - r, cy - r, cx + r, cy + r }, off,
              rgb ((int) (off.getRed() * 0.4f), (int) (off.getGreen() * 0.4f), (int) (off.getBlue() * 0.4f)), 60.0f);
    ellipse (g, { cx - r * 0.55f, cy - r * 0.7f, cx - r * 0.05f, cy - r * 0.25f }, rgb (255, 255, 255, 70));
}

void toggle (Graphics& g, float cx, float cy, bool on, float s)
{
    soft (g, { cx - 12 * s, cy - 12 * s, cx + 12 * s, cy + 12 * s }, 2 * s, juce::Colours::black, 0.55f, { 1.5f, 3.0f });
    grad (g, { cx - 12 * s, cy - 12 * s, cx + 12 * s, cy + 12 * s }, rgb (232, 233, 229), rgb (96, 100, 98), 45.0f);
    ellipse (g, { cx - 7 * s, cy - 7 * s, cx + 7 * s, cy + 7 * s }, rgb (40, 43, 42));
    const float dirn = on ? -1.0f : 1.0f, tipy = cy + dirn * 30 * s;
    poly (g, { { cx - 3.4f * s + 3, cy + 3 }, { cx + 3.4f * s + 3, cy + 3 }, { cx + 2.2f * s + 3, tipy + 3 }, { cx - 2.2f * s + 3, tipy + 3 } }, rgb (0, 0, 0, 70));
    poly (g, { { cx - 3.4f * s, cy }, { cx + 3.4f * s, cy }, { cx + 2.2f * s, tipy }, { cx - 2.2f * s, tipy } }, rgb (200, 202, 198));
    poly (g, { { cx - 3.4f * s, cy }, { cx - 0.6f * s, cy }, { cx - 0.4f * s, tipy }, { cx - 2.2f * s, tipy } }, rgb (245, 246, 242));
    soft (g, { cx - 5.5f * s, tipy - 5.5f * s, cx + 5.5f * s, tipy + 5.5f * s }, 1.2f, juce::Colours::black, 0.5f, { 2.0f, 3.0f });
    grad (g, { cx - 5.5f * s, tipy - 5.5f * s, cx + 5.5f * s, tipy + 5.5f * s }, rgb (250, 250, 247), rgb (120, 124, 121), 45.0f);
    text (g, cx + 20 * s, cy - 26 * s, "ON", Face::fine, 7.5f * s, CREAM);
    text (g, cx + 20 * s, cy + 26 * s, "OFF", Face::fine, 7.5f * s, CREAM);
}

void button (Graphics& g, Box b, const String& label, bool lit, KeyStyle style, float size, float track)
{
    rrect (g, b.expanded (3.5f), 5.0f, rgb (8, 10, 9));
    soft (g, b, 1.5f, juce::Colours::black, 0.5f, { 1.0f, 2.5f }, false, 3.0f);
    Colour tc = INK;
    if (lit)
    {
        soft (g, b.expanded (6.0f), 7.0f, GOLD_L, 0.35f, {}, false, 6.0f);
        grad (g, b, rgb (240, 212, 146), rgb (190, 152, 82), 90.0f, false, 3.0f);
    }
    else if (style == KeyStyle::cream)
        grad (g, b, rgb (242, 238, 226), rgb (206, 199, 181), 90.0f, false, 3.0f);
    else
    {
        grad (g, b, rgb (22, 74, 68), rgb (4, 40, 37), 90.0f, false, 3.0f);
        tc = CREAM;
    }
    line (g, { b.x0 + 3, b.y0 + 1 }, { b.x1 - 3, b.y0 + 1 }, 0.8f, rgb (255, 255, 255, (style == KeyStyle::cream || lit) ? 110 : 40));
    text (g, (b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2 + 0.5f, label, Face::legend, size, tc, "mm", track);
}

namespace
{
const char* segmentsFor (juce::juce_wchar ch)
{
    switch (ch)
    {
        case '0': return "abcdef"; case '1': return "bc"; case '2': return "abged"; case '3': return "abgcd";
        case '4': return "fgbc"; case '5': return "afgcd"; case '6': return "afgedc"; case '7': return "abc";
        case '8': return "abcdefg"; case '9': return "abcdfg"; case '-': return "g"; case 'H': return "fbgec";
        case 'I': return "ef"; case 'L': return "fed"; case 'o': return "cdeg"; default: return "";
    }
}

std::vector<std::pair<char, std::vector<Point<float>>>> segPolys (float x, float y, float h)
{
    const float w = 0.56f * h, th = 0.13f * h, gg = th * 0.16f, sl = std::tan (rad (7.0f));
    auto hs = [th] (float xa, float xb, float yc) -> std::vector<Point<float>>
    { return { { xa, yc }, { xa + th / 2, yc - th / 2 }, { xb - th / 2, yc - th / 2 }, { xb, yc }, { xb - th / 2, yc + th / 2 }, { xa + th / 2, yc + th / 2 } }; };
    auto vs = [th] (float xc, float ya, float yb) -> std::vector<Point<float>>
    { return { { xc, ya }, { xc + th / 2, ya + th / 2 }, { xc + th / 2, yb - th / 2 }, { xc, yb }, { xc - th / 2, yb - th / 2 }, { xc - th / 2, ya + th / 2 } }; };
    const float L = th / 2, R = w - th / 2, T = th / 2, M = h / 2, B = h - th / 2;
    std::vector<std::pair<char, std::vector<Point<float>>>> segs {
        { 'a', hs (L + gg, R - gg, T) }, { 'b', vs (R, T + gg, M - gg) }, { 'c', vs (R, M + gg, B - gg) }, { 'd', hs (L + gg, R - gg, B) },
        { 'e', vs (L, M + gg, B - gg) }, { 'f', vs (L, T + gg, M - gg) }, { 'g', hs (L + gg, R - gg, M) } };
    for (auto& [k, pl] : segs)
        for (auto& pt : pl)
            pt = { x + pt.x - (pt.y - h / 2) * sl, y + pt.y };
    return segs;
}
} // namespace

float seven (Graphics& g, float x, float y, float h, const String& s, float pitch)
{
    // Cells: every character except '.' is a digit; '.' lights the previous cell's point.
    struct Cell { juce::juce_wchar ch; bool dp; };
    std::vector<Cell> cells;
    for (auto ch : s)
    {
        if (ch == '.' && ! cells.empty()) cells.back().dp = true;
        else cells.push_back ({ ch, false });
    }
    std::vector<std::vector<Point<float>>> lit;
    for (size_t i = 0; i < cells.size(); ++i)
    {
        const float cx = x + (float) i * pitch;
        const juce::String on (segmentsFor (cells[i].ch));
        for (auto& [k, pl] : segPolys (cx, y, h))
        {
            if (on.containsChar (k)) lit.push_back (pl);
            else poly (g, pl, LED_RED_OFF);
        }
        // decimal point, bottom right of the cell
        const float d = 0.07f * h, px = cx + 0.56f * h + 0.07f * h, py = y + h - d;
        if (cells[i].dp)
        {
            std::vector<Point<float>> dp;
            for (int k = 0; k < 12; ++k)
                dp.push_back (polar (px, py, d, 30.0f * (float) k));
            lit.push_back (dp);
        }
    }
    if (! lit.empty())
        glowPolys (g, lit, LED_RED, h * 0.05f, 0.7f);
    return x + (float) cells.size() * pitch;
}

void modelBox (Graphics& g, float x, float y, const String& t, float size)
{
    const float tw = tlen (t, Face::legend, size, 2.4f);
    rrect (g, { x, y - 11, x + tw + 22, y + 11 }, 1.0f, {}, GOLD, 1.3f);
    text (g, x + 11, y + 0.5f, t, Face::legend, size, GOLD, "lm", 2.4f);
}
} // namespace kit
