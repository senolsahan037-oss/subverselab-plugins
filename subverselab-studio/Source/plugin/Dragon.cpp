// The dragon that runs across the JING FANG body: one long body from the head (top right) to the tail (bottom left), drawn in
// code (no outside artwork). Two alpha masks the size of the panel: the solid silhouette and its line work (scales, whiskers,
// horn and mane strokes, the eye). The panel tints them; built once and cached.
#include "Ui.h"

namespace jfui
{
namespace
{
using P = juce::Point<float>;

struct Spine
{
    static constexpr int N = 420;
    P pt[N + 1], tan[N + 1], nor[N + 1];
    float rad[N + 1];
    Spine()
    {
        for (int i = 0; i <= N; ++i)
        {
            const float t = (float) i / (float) N;
            pt[i] = { 1110.0f - 1035.0f * t, 150.0f + 470.0f * t + 112.0f * std::sin (juce::MathConstants<float>::twoPi * 1.72f * t + 1.95f) };
            rad[i] = 4.0f + 40.0f * std::pow (1.0f - 0.86f * t, 1.15f);
        }
        for (int i = 0; i <= N; ++i)
        {
            const P d = pt[juce::jmin (N, i + 1)] - pt[juce::jmax (0, i - 1)];
            const float l = std::sqrt (d.x * d.x + d.y * d.y) + 1e-6f;
            tan[i] = { d.x / l, d.y / l };
            nor[i] = { -tan[i].y, tan[i].x };   // "up" side of the body for the leftward run
        }
    }
    P top (int i, float k = 1.0f) const { return pt[i] + nor[i] * (rad[i] * k); }
    P bot (int i, float k = 1.0f) const { return pt[i] - nor[i] * (rad[i] * k); }
};

void spike (juce::Path& p, P a, P b, P apex) { p.startNewSubPath (a); p.lineTo (apex); p.lineTo (b); p.closeSubPath(); }

juce::Path stroke (const juce::Path& centre, float w)
{
    juce::Path out;
    juce::PathStrokeType (w, juce::PathStrokeType::curved, juce::PathStrokeType::rounded).createStrokedPath (out, centre);
    return out;
}

void build (juce::Graphics& body, juce::Graphics& lines)
{
    const Spine s;
    const int N = Spine::N;
    body.setColour (juce::Colours::white);
    lines.setColour (juce::Colours::white);

    // body
    {
        juce::Path p;
        p.startNewSubPath (s.top (0));
        for (int i = 1; i <= N; ++i) p.lineTo (s.top (i));
        p.lineTo (s.pt[N] + s.tan[N] * 26.0f);
        for (int i = N; i >= 0; --i) p.lineTo (s.bot (i));
        p.closeSubPath();
        body.fillPath (p);
    }
    // dorsal spikes, leaning toward the tail
    for (int i = 16; i < N - 20; i += 13)
    {
        juce::Path p;
        const float h = s.rad[i] * 0.95f + 5.0f;
        spike (p, s.top (juce::jmax (0, i - 5), 0.85f), s.top (i + 5, 0.85f), s.top (i) + s.nor[i] * h + s.tan[i] * (h * 0.55f));
        body.fillPath (p);
    }
    // tail flame
    for (int k = -3; k <= 3; ++k)
    {
        const float a = 0.32f * (float) k;
        const P d { s.tan[N].x * std::cos (a) - s.tan[N].y * std::sin (a), s.tan[N].x * std::sin (a) + s.tan[N].y * std::cos (a) };
        juce::Path p;
        const P base = s.pt[N - 6];
        const P side { -d.y * 5.0f, d.x * 5.0f };
        spike (p, base + side, base - side, base + d * (38.0f - 6.0f * (float) std::abs (k)));
        body.fillPath (p);
    }
    // four legs from the belly: thigh, shin, three claws
    for (float lt : { 0.13f, 0.31f, 0.56f, 0.73f })
    {
        const int i = (int) (lt * (float) N);
        const float r = s.rad[i];
        const P down = -s.nor[i], back = s.tan[i];
        const P hip = s.bot (i, 0.5f), knee = hip + down * (r * 1.25f + 10.0f) + back * (r * 0.55f), foot = knee + down * (r * 0.4f + 6.0f) - back * (r * 0.9f + 8.0f);
        juce::Path leg;
        leg.startNewSubPath (hip); leg.lineTo (knee); leg.lineTo (foot);
        body.fillPath (stroke (leg, juce::jmax (5.0f, r * 0.58f)));
        for (int c = -1; c <= 1; ++c)
        {
            juce::Path claw;
            const P dir = (-back) * 0.8f + down * (0.45f * (float) c + 0.2f);
            claw.startNewSubPath (foot); claw.quadraticTo (foot + dir * 11.0f + down * 3.0f, foot + dir * 18.0f + down * 9.0f);
            body.fillPath (stroke (claw, 3.4f));
        }
    }
    // head, in its own frame: forward = away from the body, up = the body's top side
    const P f = -s.tan[0], u = s.nor[0], o = s.pt[0];
    auto H = [&] (float x, float y) { return o + f * (x * 1.5f) + u * (y * 1.5f); };   // the head at 1.5x
    {
        juce::Path p;
        const float pts[][2] = { { -14, 34 }, { 12, 42 }, { 34, 36 }, { 52, 28 }, { 74, 22 }, { 92, 16 }, { 99, 8 }, { 95, 1 }, { 84, -1 },
                                 { 56, 3 }, { 34, -3 }, { 58, -9 }, { 80, -14 }, { 88, -19 }, { 82, -25 }, { 52, -27 }, { 22, -30 }, { -14, -34 } };
        p.startNewSubPath (H (pts[0][0], pts[0][1]));
        for (auto& q : pts) p.lineTo (H (q[0], q[1]));
        p.closeSubPath();
        body.fillPath (p);
        // horns, swept back
        for (int h = 0; h < 2; ++h)
        {
            juce::Path c;
            c.startNewSubPath (H (24.0f - 8.0f * (float) h, 36.0f));
            c.cubicTo (H (6.0f - 6.0f * (float) h, 58.0f), H (-24.0f, 70.0f + 6.0f * (float) h), H (-56.0f - 10.0f * (float) h, 66.0f + 14.0f * (float) h));
            body.fillPath (stroke (c, 5.0f - 1.2f * (float) h));
        }
        // mane: flames along the neck
        for (int m = 0; m < 6; ++m)
        {
            juce::Path q;
            const float x = -6.0f - 16.0f * (float) m;
            spike (q, H (x + 6.0f, 30.0f - (float) m), H (x - 8.0f, 28.0f - (float) m), H (x - 22.0f, 58.0f - 3.0f * (float) m));
            body.fillPath (q);
        }
        // beard
        for (int m = 0; m < 3; ++m)
        {
            juce::Path q;
            spike (q, H (60.0f - 10.0f * (float) m, -22.0f), H (48.0f - 10.0f * (float) m, -24.0f), H (40.0f - 14.0f * (float) m, -46.0f + 4.0f * (float) m));
            body.fillPath (q);
        }
    }
    // line work: scales across the body, whiskers, eye ring, brow
    for (int i = 10; i < N - 10; i += 6)
    {
        juce::Path c;
        c.startNewSubPath (s.top (i, 0.8f));
        c.quadraticTo (s.pt[i] - s.tan[i] * (s.rad[i] * 0.55f), s.bot (i, 0.8f));
        lines.fillPath (stroke (c, 1.1f));
    }
    {
        juce::Path c;
        for (int i = 0; i <= N; i += 2) { if (i == 0) c.startNewSubPath (s.top (i, 0.55f)); else c.lineTo (s.top (i, 0.55f)); }
        lines.fillPath (stroke (c, 1.0f));
    }
    for (int w = 0; w < 2; ++w)
    {
        juce::Path c;
        const float sy = w == 0 ? 1.0f : -1.0f;
        c.startNewSubPath (H (90.0f, 9.0f * sy + 2.0f));
        c.cubicTo (H (128.0f, 30.0f * sy), H (150.0f, -20.0f * sy), H (196.0f, 18.0f * sy));
        lines.fillPath (stroke (c, 2.2f));
    }
    lines.drawEllipse (juce::Rectangle<float> (11.0f, 11.0f).withCentre (H (42.0f, 18.0f)), 2.2f);
    lines.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre (H (43.0f, 18.0f)));
    {
        juce::Path c; c.startNewSubPath (H (28.0f, 30.0f)); c.quadraticTo (H (44.0f, 34.0f), H (58.0f, 26.0f));
        lines.fillPath (stroke (c, 2.0f));
    }
}

struct Masks
{
    juce::Image body { juce::Image::SingleChannel, kW, kH, true }, lines { juce::Image::SingleChannel, kW, kH, true };
    Masks()
    {
        juce::Graphics gb (body), gl (lines);
        build (gb, gl);
    }
};
const Masks& masks() { static const Masks m; return m; }
} // namespace

const juce::Image& dragonMask() { return masks().body; }

std::pair<juce::Image, juce::Image> dragonMasksScaled (float scale)
{
    juce::Image b (juce::Image::SingleChannel, juce::roundToInt (kW * scale), juce::roundToInt (kH * scale), true), l (b.createCopy());
    l.clear (l.getBounds());
    juce::Graphics gb (b), gl (l);
    gb.addTransform (juce::AffineTransform::scale (scale)); gl.addTransform (juce::AffineTransform::scale (scale));
    build (gb, gl);
    return { b, l };
}
const juce::Image& dragonLines() { return masks().lines; }
} // namespace jfui
