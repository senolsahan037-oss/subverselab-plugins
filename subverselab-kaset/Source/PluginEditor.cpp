#include "PluginEditor.h"
#include "Theme.h"
#include "BinaryData.h"

namespace
{
constexpr int kW = 1100, kH = 640;
constexpr float kStart = juce::MathConstants<float>::pi * 1.25f, kEnd = juce::MathConstants<float>::pi * 2.75f;
const juce::Colour coral { 0xffD9573F };   // the tuner pointer and the top of the level ladder

juce::Point<float> polar (juce::Point<float> c, float r, float angle)
{
    return { c.x + std::sin (angle) * r, c.y - std::cos (angle) * r };
}

void tracked (juce::Graphics& g, const juce::String& s, juce::Font f, juce::Rectangle<float> r,
              juce::Justification j, float tracking)
{
    g.setFont (f.withExtraKerningFactor (tracking));
    g.drawText (s, r, j, false);
}

void glow (juce::Graphics& g, juce::Point<float> c, float radius, juce::Colour col, float alpha)
{
    g.setGradientFill (juce::ColourGradient (col.withAlpha (alpha), c.x, c.y, col.withAlpha (0.0f), c.x + radius, c.y, true));
    g.fillEllipse (c.x - radius, c.y - radius, radius * 2.0f, radius * 2.0f);
}

/** The product name in squared, extended 80s hi-fi letterforms, drawn as strokes. Returns the width. */
float drawProductName (juce::Graphics& g, const juce::String& name, float x, float y, float h, juce::Colour ink)
{
    const float w = 1.34f * h, t = 0.19f * h, r = 0.30f * h, gap = 0.30f * h;
    const float in = t * 0.5f;
    juce::Path all;
    float cx = x;
    for (auto ch : name)
    {
        juce::Path p;
        const float L = cx + in, R = cx + w - in, T = y + in, B = y + h - in, M = y + h * 0.5f;
        switch (ch)
        {
            case 'K':
                p.startNewSubPath (L, T); p.lineTo (L, B);
                p.startNewSubPath (L, M + t * 0.2f); p.lineTo (R, T);
                p.startNewSubPath (L + (R - L) * 0.36f, M - h * 0.12f); p.lineTo (R, B);
                break;
            case 'A':
                p.startNewSubPath (L, B); p.lineTo (L, T + r); p.quadraticTo (L, T, L + r, T);
                p.lineTo (R - r, T); p.quadraticTo (R, T, R, T + r); p.lineTo (R, B);
                p.startNewSubPath (L, M + h * 0.06f); p.lineTo (R, M + h * 0.06f);
                break;
            case 'S':
                p.startNewSubPath (R, T); p.lineTo (L + r, T); p.quadraticTo (L, T, L, T + r);
                p.lineTo (L, M - r * 0.5f); p.quadraticTo (L, M, L + r * 0.5f, M); p.lineTo (R - r * 0.5f, M);
                p.quadraticTo (R, M, R, M + r * 0.5f); p.lineTo (R, B - r); p.quadraticTo (R, B, R - r, B); p.lineTo (L, B);
                break;
            case 'E':
                p.startNewSubPath (R, T); p.lineTo (L + r, T); p.quadraticTo (L, T, L, T + r);
                p.lineTo (L, B - r); p.quadraticTo (L, B, L + r, B); p.lineTo (R, B);
                p.startNewSubPath (L, M); p.lineTo (L + (R - L) * 0.82f, M);
                break;
            case 'T':
                p.startNewSubPath (L - in, T); p.lineTo (R + in, T);
                p.startNewSubPath ((L + R) * 0.5f, T); p.lineTo ((L + R) * 0.5f, B + in);
                break;
            default: break;
        }
        all.addPath (p);
        cx += w + gap;
    }
    // The strokes are turned into an outline here and filled, instead of being
    // handed to the renderer to stroke: Direct2D on Windows joins these corners
    // differently and drew spurs on the letters.
    juce::Path outline;
    juce::PathStrokeType (t, juce::PathStrokeType::mitered, juce::PathStrokeType::butt).createStrokedPath (outline, all);
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillPath (outline, juce::AffineTransform::translation (0.0f, 1.2f));
    g.setColour (ink);
    g.fillPath (outline);
    return cx - gap - x;
}

float nameWidth (int letters, float h) { return (float) letters * 1.34f * h + (float) (letters - 1) * 0.3f * h; }

juce::Colour tapeStripe (int tape)
{
    return tape == 0 ? Theme::gold : tape == 1 ? Theme::teal : juce::Colour (0xffA9AEAB);
}

#if SVL_LICENSING
/** The activation card wears the panel's own palette and its embedded typefaces. */
svl::LicenseOverlay::Style licenseStyle()
{
    svl::LicenseOverlay::Style s;
    s.panel = Theme::panel;       s.panelDeep = Theme::panelDeep;  s.ink = Theme::ink;
    s.capCream = Theme::capCream; s.gold = Theme::gold;            s.goldDark = Theme::goldDark;
    s.screen = Theme::screen;     s.vfd = Theme::vfd;              s.danger = Theme::danger;
    s.legend = [] (float size) { return Theme::legend (size); };
    s.fine   = [] (float size) { return Theme::fine (size); };
    s.mono   = [] (float size) { return Theme::mono (size); };
    return s;
}
#endif

} // namespace

//==============================================================================
KasetLookAndFeel::KasetLookAndFeel()
{
    // Menus and tooltips too: nothing on the panel asks the system for a font.
    setDefaultSansSerifTypeface (faces->regular);
}

void KasetLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                         float startAngle, float endAngle, juce::Slider& s)
{
    // The machined aluminium knob shared with Kubbe: knurled skirt, turned cap, gold index.
    const float d = (float) juce::jmin (w, h) - 4.0f, r = d * 0.5f;
    const juce::Point<float> c ((float) x + (float) w * 0.5f, (float) y + (float) h * 0.5f);
    const float angle = startAngle + pos * (endAngle - startAngle);

    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.fillEllipse (c.x - r + 2.0f, c.y - r + 4.0f, d, d);

    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff2b302e), c.x, c.y - r, juce::Colour (0xff0b0d0c), c.x, c.y + r, false));
    g.fillEllipse (c.x - r, c.y - r, d, d);
    for (int i = 0; i < 60; ++i)
    {
        const float a = juce::MathConstants<float>::twoPi * (float) i / 60.0f;
        const float light = 0.5f + 0.5f * std::cos (a + 0.8f);
        g.setColour (juce::Colours::white.withAlpha (0.05f + 0.13f * light));
        g.drawLine ({ polar (c, r * 0.86f, a), polar (c, r - 0.8f, a) }, 1.0f);
    }

    const float cr = r * 0.74f;
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xffDCDDD8), c.x - cr, c.y - cr, juce::Colour (0xff6E7471), c.x + cr, c.y + cr, false));
    g.fillEllipse (c.x - cr, c.y - cr, cr * 2.0f, cr * 2.0f);
    for (float rr = cr * 0.12f; rr < cr; rr += 1.6f)
    {
        g.setColour ((((int) (rr * 3.1f)) & 1) ? juce::Colours::white.withAlpha (0.07f) : juce::Colours::black.withAlpha (0.05f));
        g.drawEllipse (c.x - rr, c.y - rr, rr * 2.0f, rr * 2.0f, 0.8f);
    }
    juce::Path sheen;
    sheen.addPieSegment (c.x - cr, c.y - cr, cr * 2.0f, cr * 2.0f, -2.2f, -1.2f, 0.0f);
    sheen.addPieSegment (c.x - cr, c.y - cr, cr * 2.0f, cr * 2.0f, 0.94f, 1.94f, 0.0f);
    g.setColour (juce::Colours::white.withAlpha (0.22f));
    g.fillPath (sheen);
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.drawEllipse (c.x - cr, c.y - cr, cr * 2.0f, cr * 2.0f, 1.0f);

    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.drawLine ({ polar (c, cr * 0.28f, angle), polar (c, r * 0.97f, angle) }, 4.0f);
    g.setColour (Theme::gold);
    g.drawLine ({ polar (c, cr * 0.28f, angle), polar (c, r * 0.97f, angle) }, 2.4f);

    // In BYPASS the knobs still move but the tape is out of the path: say so by dimming them.
    if ((bool) s.getProperties()["bypassed"])
    {
        g.setColour (Theme::panelDeep.withAlpha (0.55f));
        g.fillEllipse (c.x - r, c.y - r, d, d);
    }
}

//==============================================================================
float GenerationDial::xOf (int index) const
{
    const float left = 40.0f, right = (float) getWidth() - 40.0f;
    return left + (right - left) * (float) index / 4.0f;
}

void GenerationDial::pick (float x)
{
    int best = 0;
    for (int i = 1; i < 5; ++i)
        if (std::abs (x - xOf (i)) < std::abs (x - xOf (best))) best = i;
    if (auto* p = proc.apvts.getParameter ("generation"))
        p->setValueNotifyingHost (p->convertTo0to1 ((float) best));   // inside the gesture mouseDown / mouseUp open
    repaint();
}

void GenerationDial::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    g.setColour (juce::Colour (0xff020807));
    g.fillRoundedRectangle (b, 5.0f);
    const auto glass = b.reduced (4.0f);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff0B2522), glass.getX(), glass.getY(), juce::Colour (0xff041210), glass.getX(), glass.getBottom(), false));
    g.fillRoundedRectangle (glass, 3.0f);

    // Header row, as on a tuner scale.
    g.setColour (Theme::ink.withAlpha (0.8f));
    tracked (g, "DUB", Theme::legend (8.5f), { glass.getX() + 8.0f, glass.getY() + 5.0f, 40.0f, 11.0f }, juce::Justification::centredLeft, 0.2f);
    tracked (g, "COPIES", Theme::legend (8.0f), { glass.getRight() - 58.0f, glass.getY() + 5.0f, 50.0f, 11.0f }, juce::Justification::centredRight, 0.2f);

    // Frequency-style fine scale.
    const float scaleY = glass.getY() + glass.getHeight() * 0.52f;
    for (int i = 0; i <= 80; ++i)
    {
        const float x = xOf (0) + (xOf (4) - xOf (0)) * (float) i / 80.0f;
        const bool major = i % 20 == 0, mid = i % 5 == 0;
        g.setColour (Theme::ink.withAlpha (major ? 0.9f : 0.45f));
        g.drawLine (x, scaleY - (major ? 9.0f : mid ? 6.0f : 3.0f), x, scaleY, major ? 1.4f : 1.0f);
    }
    g.setColour (Theme::ink.withAlpha (0.45f));
    g.drawHorizontalLine ((int) scaleY, xOf (0), xOf (4));

    const int gen = (int) proc.apvts.getRawParameterValue ("generation")->load();
    for (int i = 0; i < 5; ++i)
    {
        g.setColour (i == gen ? Theme::lamp : Theme::ink.withAlpha (0.85f));
        tracked (g, KasetProcessor::generationNames[i], Theme::legend (9.5f), { xOf (i) - 40.0f, scaleY + 6.0f, 80.0f, 13.0f }, juce::Justification::centred, 0.12f);
        g.setColour (Theme::ink.withAlpha (0.5f));
        tracked (g, juce::String (i), Theme::legend (8.0f), { xOf (i) - 20.0f, scaleY - 24.0f, 40.0f, 11.0f }, juce::Justification::centred, 0.1f);
    }

    // The pointer.
    const float px = xOf (gen);
    g.setColour (coral.withAlpha (0.25f));
    g.fillRect (px - 3.0f, glass.getY() + 3.0f, 6.0f, glass.getHeight() - 6.0f);
    g.setColour (coral);
    g.fillRect (px - 1.0f, glass.getY() + 3.0f, 2.0f, glass.getHeight() - 6.0f);

    // In BYPASS the dial says so, across the scale.
    if (proc.apvts.getRawParameterValue ("play")->load() < 0.5f)
    {
        g.setColour (Theme::screen.withAlpha (0.8f));
        g.fillRoundedRectangle (glass, 3.0f);
        g.setColour (coral);
        tracked (g, "BYPASS", Theme::legend (22.0f), glass.withTrimmedBottom (glass.getHeight() * 0.35f), juce::Justification::centredBottom, 0.4f);
        g.setColour (Theme::ink.withAlpha (0.8f));
        tracked (g, "TAPE IS OUT OF THE SIGNAL PATH  -  PRESS TAPE", Theme::legend (8.5f), glass.withTrimmedTop (glass.getHeight() * 0.68f).withTrimmedBottom (8.0f),
                 juce::Justification::centredTop, 0.22f);
    }

    // Backlight and glass.
    g.setGradientFill (juce::ColourGradient (Theme::lamp.withAlpha (0.10f), glass.getCentreX(), glass.getBottom(), Theme::lamp.withAlpha (0.0f), glass.getCentreX(), glass.getY(), false));
    g.fillRoundedRectangle (glass, 3.0f);
    g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.10f), glass.getX(), glass.getY(), juce::Colours::white.withAlpha (0.0f), glass.getX(), glass.getCentreY(), false));
    g.fillRoundedRectangle (glass.withHeight (glass.getHeight() * 0.5f), 3.0f);
}

//==============================================================================
void CassetteWell::advance (float seconds)
{
    if (proc.uiRunning.load())
    {
        angle += seconds * juce::MathConstants<float>::twoPi * 0.62f;
        if (angle > juce::MathConstants<float>::twoPi) angle -= juce::MathConstants<float>::twoPi;
        repaint();
    }
}

void CassetteWell::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    const int tape = (int) proc.apvts.getRawParameterValue ("tape")->load();
    const bool running = proc.uiRunning.load();
    const bool engaged = proc.apvts.getRawParameterValue ("play")->load() > 0.5f;

    // Deck bay: a moulded recess with a chamfered edge, lit from the top left.
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff142120), b.getX(), b.getY(), juce::Colour (0xff050908), b.getX(), b.getBottom(), false));
    g.fillRoundedRectangle (b, 7.0f);
    g.setColour (juce::Colours::white.withAlpha (0.16f));
    g.drawLine (b.getX() + 7.0f, b.getY() + 0.5f, b.getRight() - 7.0f, b.getY() + 0.5f, 1.0f);
    g.drawLine (b.getX() + 0.5f, b.getY() + 7.0f, b.getX() + 0.5f, b.getBottom() - 7.0f, 1.0f);
    g.setColour (juce::Colours::black.withAlpha (0.7f));
    g.drawLine (b.getX() + 7.0f, b.getBottom() - 0.5f, b.getRight() - 7.0f, b.getBottom() - 0.5f, 1.0f);
    g.drawLine (b.getRight() - 0.5f, b.getY() + 7.0f, b.getRight() - 0.5f, b.getBottom() - 7.0f, 1.0f);

    // Deck badge: gold triangle A on the recording side, teal square B on the dub side.
    auto head = b.reduced (12.0f, 8.0f).removeFromTop (16.0f);
    const auto badge = head.removeFromLeft (16.0f);
    if (dub)
    {
        g.setColour (Theme::teal);
        g.fillRoundedRectangle (badge, 2.0f);
    }
    else
    {
        juce::Path tri;
        tri.addTriangle (badge.getCentreX(), badge.getY(), badge.getRight(), badge.getBottom(), badge.getX(), badge.getBottom());
        g.setColour (Theme::gold);
        g.fillPath (tri);
    }
    g.setColour (Theme::panelDeep);
    g.setFont (Theme::legend (9.0f));
    g.drawText (dub ? "B" : "A", badge.translated (0.0f, dub ? 0.0f : 2.0f), juce::Justification::centred);
    g.setColour (Theme::ink);
    tracked (g, dub ? "PLAY BACK" : "RECORD  PLAY BACK", Theme::legend (8.5f), head.withTrimmedLeft (8.0f), juce::Justification::centredLeft, 0.2f);
    const juce::Point<float> lampAt (head.getRight() - 4.0f, head.getCentreY());
    if (running) glow (g, lampAt, 12.0f, dub ? Theme::lamp : coral, 0.55f);
    g.setColour (running ? (dub ? Theme::lamp : coral) : juce::Colour (0xff3a2e15));
    g.fillEllipse (lampAt.x - 3.5f, lampAt.y - 3.5f, 7.0f, 7.0f);

    // The door: a smoked acrylic panel in its own frame, with a finger notch at the top.
    const auto door = b.reduced (10.0f).withTrimmedTop (18.0f);
    g.setColour (juce::Colour (0xff020504));
    g.fillRoundedRectangle (door, 5.0f);
    const auto window = door.reduced (6.0f);

    // Cassette: a smoke-tinted shell you can see into.
    const float sw = juce::jmin (window.getWidth() * 0.92f, window.getHeight() * 0.94f / 0.64f), sh = sw * 0.64f;
    const auto shell = juce::Rectangle<float> (sw, sh).withCentre (window.getCentre().translated (0.0f, 2.0f));
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff343A38), shell.getX(), shell.getY(), juce::Colour (0xff141817), shell.getX(), shell.getBottom(), false));
    g.fillRoundedRectangle (shell, 5.0f);

    // Tape packs seen through the shell, then the label over the top half.
    const double secs = proc.uiTapeSeconds.load();
    const float progress = (float) std::fmod (secs / 1800.0, 1.0);   // a C-60 side is 30 minutes
    const float hubR = sh * 0.085f, maxPack = sh * 0.3f;
    const juce::Point<float> hubs[2] { { shell.getX() + sw * 0.305f, shell.getY() + sh * 0.43f },
                                        { shell.getRight() - sw * 0.305f, shell.getY() + sh * 0.43f } };
    g.saveState();
    g.reduceClipRegion (shell.toNearestInt());
    for (int k = 0; k < 2; ++k)
    {
        const float fill = k == 0 ? 1.0f - progress : progress;
        const float packR = hubR + 2.0f + (maxPack - hubR) * (0.12f + 0.88f * fill);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff5E3F29).withAlpha (0.85f), hubs[k].x, hubs[k].y, juce::Colour (0xff24170E).withAlpha (0.85f), hubs[k].x + packR, hubs[k].y, true));
        g.fillEllipse (hubs[k].x - packR, hubs[k].y - packR, packR * 2.0f, packR * 2.0f);
        // Wound-tape rings and one bright splice that shows the pack turning.
        for (float rr = hubR + 4.0f; rr < packR; rr += 3.0f)
        {
            g.setColour (juce::Colours::black.withAlpha (0.12f));
            g.drawEllipse (hubs[k].x - rr, hubs[k].y - rr, rr * 2.0f, rr * 2.0f, 0.6f);
        }
        const auto splice = polar (hubs[k], packR - 2.0f, angle * (hubR / packR) * 2.4f + (float) k);
        g.setColour (juce::Colour (0xffC9A27A).withAlpha (0.6f));
        g.fillEllipse (splice.x - 1.5f, splice.y - 1.5f, 3.0f, 3.0f);
    }
    // Tape path along the bottom: guide rollers, the felt pad, the tape.
    const float ty = shell.getBottom() - sh * 0.1f;
    g.setColour (juce::Colour (0xff4A3120));
    g.drawLine (shell.getX() + sw * 0.1f, ty, shell.getRight() - sw * 0.1f, ty, 2.0f);
    g.drawLine (hubs[0].x - maxPack * 0.6f, hubs[0].y + maxPack * 0.5f, shell.getX() + sw * 0.1f, ty, 1.4f);
    g.drawLine (hubs[1].x + maxPack * 0.6f, hubs[1].y + maxPack * 0.5f, shell.getRight() - sw * 0.1f, ty, 1.4f);
    for (float gx : { shell.getX() + sw * 0.1f, shell.getRight() - sw * 0.1f })
    {
        g.setColour (juce::Colour (0xffD8D5CC));
        g.fillEllipse (gx - 4.0f, ty - 4.0f, 8.0f, 8.0f);
        g.setColour (juce::Colour (0xff3A3E3C));
        g.fillEllipse (gx - 1.5f, ty - 1.5f, 3.0f, 3.0f);
    }
    g.setColour (juce::Colour (0xff8E7C62));
    g.fillRect (shell.getCentreX() - 8.0f, ty - 5.0f, 16.0f, 4.0f);    // pressure pad
    g.restoreState();

    // Label with the window cut out of it.
    const auto label = juce::Rectangle<float> (sw * 0.86f, sh * 0.66f).withCentre ({ shell.getCentreX(), shell.getY() + sh * 0.41f });
    juce::Path labelPath;
    labelPath.addRoundedRectangle (label, 3.0f);
    const auto hole = juce::Rectangle<float> (sw * 0.58f, sh * 0.22f).withCentre ({ shell.getCentreX(), hubs[0].y });
    juce::Path holePath;
    holePath.addRoundedRectangle (hole, hole.getHeight() * 0.5f);
    labelPath.setUsingNonZeroWinding (false);
    labelPath.addPath (holePath);
    g.setColour (juce::Colour (0xffEDE6D2));
    g.fillPath (labelPath);
    g.setColour (tapeStripe (tape));
    g.fillRect (label.getX(), label.getY() + label.getHeight() * 0.17f, label.getWidth(), label.getHeight() * 0.06f);
    g.setColour (Theme::panel);
    auto top = label.reduced (7.0f, 2.0f).withHeight (label.getHeight() * 0.17f);
    tracked (g, "SUBVERSELAB", Theme::legend (7.0f), top, juce::Justification::centredLeft, 0.2f);
    tracked (g, dub ? "C-60   B" : "C-60   A", Theme::legend (7.0f), top, juce::Justification::centredRight, 0.15f);
    // Ruled title lines, with a hand-written title, as a dubbed tape would have.
    g.setColour (Theme::panel.withAlpha (0.25f));
    for (float ly : { label.getBottom() - label.getHeight() * 0.13f })
        g.drawHorizontalLine ((int) ly, label.getX() + 8.0f, label.getRight() - 8.0f);
    const char* typeName[] { "TYPE I  NORMAL", "TYPE II  CHROME", "TYPE IV  METAL" };
    g.setColour (Theme::panel);
    tracked (g, typeName[juce::jlimit (0, 2, tape)], Theme::legend (6.5f),
             juce::Rectangle<float> (label.getX() + 8.0f, label.getBottom() - label.getHeight() * 0.13f - 11.0f, label.getWidth() * 0.5f, 10.0f), juce::Justification::centredLeft, 0.18f);
    g.setFont (Theme::fineItalic (10.0f));
    g.drawText (dub ? "dub" : "mixtape", juce::Rectangle<float> (label.getCentreX(), label.getBottom() - label.getHeight() * 0.13f - 13.0f, label.getWidth() * 0.5f - 8.0f, 12.0f),
                juce::Justification::centredRight);

    // Hubs, turning.
    for (int k = 0; k < 2; ++k)
    {
        g.setColour (juce::Colour (0xffECE8DE));
        g.fillEllipse (hubs[k].x - hubR, hubs[k].y - hubR, hubR * 2.0f, hubR * 2.0f);
        g.setColour (juce::Colour (0xff1a1c1b));
        g.fillEllipse (hubs[k].x - hubR * 0.62f, hubs[k].y - hubR * 0.62f, hubR * 1.24f, hubR * 1.24f);
        for (int tooth = 0; tooth < 6; ++tooth)
        {
            const float a = angle + juce::MathConstants<float>::twoPi * (float) tooth / 6.0f;
            g.setColour (juce::Colour (0xffECE8DE));
            g.drawLine ({ polar (hubs[k], hubR * 0.35f, a), polar (hubs[k], hubR * 0.68f, a) }, 2.0f);
        }
    }
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.drawRoundedRectangle (hole, hole.getHeight() * 0.5f, 1.0f);

    // Shell details: five screws, write-protect tabs, moulded edge.
    for (auto pt : { shell.getTopLeft().translated (6, 6), shell.getTopRight().translated (-6, 6), shell.getBottomLeft().translated (6, -6),
                     shell.getBottomRight().translated (-6, -6), juce::Point<float> (shell.getCentreX(), shell.getBottom() - 5.0f) })
    {
        g.setColour (juce::Colour (0xff9A9E9C));
        g.fillEllipse (pt.x - 2.5f, pt.y - 2.5f, 5.0f, 5.0f);
        g.setColour (juce::Colour (0xff3A3E3C));
        g.drawLine (pt.x - 1.5f, pt.y, pt.x + 1.5f, pt.y, 0.8f);
    }
    for (float tx : { shell.getX() + sw * 0.12f, shell.getRight() - sw * 0.16f })
    {
        g.setColour (juce::Colour (0xff0D100F));
        g.fillRect (tx, shell.getY(), sw * 0.04f, 3.0f);
    }
    g.setColour (juce::Colours::white.withAlpha (0.14f));
    g.drawRoundedRectangle (shell, 5.0f, 1.0f);

    // Smoked acrylic over it, with a reflection band and the finger notch.
    g.setColour (juce::Colour (0xff021a17).withAlpha (engaged ? 0.22f : 0.5f));
    g.fillRoundedRectangle (window, 4.0f);
    juce::Path reflection;
    reflection.startNewSubPath (window.getX() + window.getWidth() * 0.08f, window.getY());
    reflection.lineTo (window.getX() + window.getWidth() * 0.38f, window.getY());
    reflection.lineTo (window.getX() + window.getWidth() * 0.18f, window.getBottom());
    reflection.lineTo (window.getX() - window.getWidth() * 0.12f, window.getBottom());
    reflection.closeSubPath();
    g.saveState();
    g.reduceClipRegion (window.toNearestInt());
    g.setColour (juce::Colours::white.withAlpha (0.06f));
    g.fillPath (reflection);
    g.restoreState();
    const auto notch = juce::Rectangle<float> (40.0f, 10.0f).withCentre ({ door.getCentreX(), door.getY() + 1.0f });
    g.setColour (juce::Colour (0xff020504));
    g.fillRoundedRectangle (notch, 5.0f);
    g.setColour (juce::Colours::white.withAlpha (0.12f));
    g.drawRoundedRectangle (window, 4.0f, 1.0f);
}

//==============================================================================
void LevelLadder::setLevels (float l, float r)
{
    bool changed = false;
    for (int c = 0; c < 2; ++c)
    {
        // 0 on the ladder = -18 dBFS RMS, like a 0 VU reference.
        const float target = juce::Decibels::gainToDecibels (c == 0 ? l : r, -80.0f) + 18.0f;
        const float next = target > db[c] ? target : db[c] - 1.2f;
        if (std::abs (next - db[c]) > 0.05f) changed = true;
        db[c] = juce::jmax (-40.0f, next);
    }
    if (changed) repaint();
}

void LevelLadder::paint (juce::Graphics& g)
{
    // Horizontal: two rows of segments, L over R, as on the centre strip of the reference compo.
    const auto b = getLocalBounds().toFloat();
    g.setColour (juce::Colour (0xff020807));
    g.fillRoundedRectangle (b, 4.0f);
    const int segs = 16;
    const auto area = b.reduced (6.0f).withTrimmedLeft (12.0f);
    const float sw = area.getWidth() / (float) segs, rh = area.getHeight() * 0.5f;
    for (int c = 0; c < 2; ++c)
    {
        const float y = area.getY() + (float) c * rh;
        g.setColour (Theme::ink.withAlpha (0.8f));
        g.setFont (Theme::legend (7.5f));
        g.drawText (c == 0 ? "L" : "R", juce::Rectangle<float> (b.getX() + 4.0f, y, 10.0f, rh), juce::Justification::centred);
        for (int i = 0; i < segs; ++i)
        {
            const float threshold = -30.0f + 2.25f * (float) i;   // -30 .. +3.75, 0 at the 14th segment
            const juce::Colour col = i < 11 ? Theme::teal : i < 13 ? Theme::lamp : coral;
            const bool on = db[c] >= threshold;
            const auto seg = juce::Rectangle<float> (area.getX() + (float) i * sw + 1.0f, y + 2.0f, sw - 2.0f, rh - 4.0f);
            g.setColour (on ? col : col.withAlpha (0.12f));
            g.fillRoundedRectangle (seg, 1.0f);
        }
    }
}

//==============================================================================
void TapeCounter::mouseDown (const juce::MouseEvent&)
{
    offset = proc.uiTapeSeconds.load();
    repaint();
}

void TapeCounter::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    const auto drums = b.withWidth (b.getHeight() * 2.4f);
    g.setColour (juce::Colour (0xff050807));
    g.fillRoundedRectangle (drums, 4.0f);
    const double count = std::fmod (std::max (0.0, proc.uiTapeSeconds.load() - offset), 1000.0);
    const float cw = (drums.getWidth() - 8.0f) / 3.0f;
    for (int i = 0; i < 3; ++i)
    {
        const auto cell = juce::Rectangle<float> (drums.getX() + 4.0f + (float) i * cw, drums.getY() + 3.0f, cw - 2.0f, drums.getHeight() - 6.0f);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1c1f1e), cell.getX(), cell.getY(), juce::Colour (0xff050606), cell.getX(), cell.getBottom(), false));
        g.fillRect (cell);
        const int place = 2 - i;
        const double v = count / std::pow (10.0, place);
        const int digit = (int) std::fmod (std::floor (v), 10.0);
        const float roll = place == 0 ? (float) (v - std::floor (v)) : 0.0f;
        g.saveState();
        g.reduceClipRegion (cell.toNearestInt());
        g.setColour (juce::Colour (0xffEDE6D2));
        g.setFont (Theme::legend (cell.getHeight() * 0.8f));
        g.drawText (juce::String (digit), cell.translated (0.0f, -roll * cell.getHeight()), juce::Justification::centred);
        g.drawText (juce::String ((digit + 1) % 10), cell.translated (0.0f, (1.0f - roll) * cell.getHeight()), juce::Justification::centred);
        g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.55f), cell.getX(), cell.getY(), juce::Colours::transparentBlack, cell.getX(), cell.getCentreY(), false));
        g.fillRect (cell.withHeight (cell.getHeight() * 0.5f));
        g.setGradientFill (juce::ColourGradient (juce::Colours::transparentBlack, cell.getX(), cell.getCentreY(), juce::Colours::black.withAlpha (0.55f), cell.getX(), cell.getBottom(), false));
        g.fillRect (cell.withTrimmedTop (cell.getHeight() * 0.5f));
        g.restoreState();
    }
    const auto btn = juce::Rectangle<float> (drums.getRight() + 10.0f, b.getCentreY() - 7.0f, 14.0f, 14.0f);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xffDCDDD8), btn.getX(), btn.getY(), juce::Colour (0xff6E7471), btn.getRight(), btn.getBottom(), false));
    g.fillEllipse (btn);
    g.setColour (Theme::ink);
    tracked (g, "RESET", Theme::legend (7.5f), { btn.getRight() + 6.0f, btn.getY(), 50.0f, btn.getHeight() }, juce::Justification::centredLeft, 0.2f);
}

//==============================================================================
void PianoKey::paintButton (juce::Graphics& g, bool over, bool down)
{
    const auto b = getLocalBounds().toFloat();
    const bool on = getToggleState();
    const juce::Point<float> led (b.getCentreX(), 6.0f);
    if (on) glow (g, led, 12.0f, Theme::lamp, 0.55f);
    g.setColour (on ? Theme::lamp : juce::Colour (0xff3a2e15));
    g.fillEllipse (led.x - 3.5f, led.y - 3.5f, 7.0f, 7.0f);

    auto key = b.withTrimmedTop (16.0f);
    const float press = on ? 3.0f : (down ? 2.0f : 0.0f);
    g.setColour (juce::Colour (0xff020d0c));
    g.fillRoundedRectangle (key, 3.0f);
    auto face = key.reduced (2.0f).withTrimmedBottom (6.0f - press).translated (0.0f, press);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xffE9EAE5).darker (on ? 0.08f : 0.0f).brighter (over ? 0.04f : 0.0f), face.getX(), face.getY(),
                                             juce::Colour (0xffA2A7A4).darker (on ? 0.08f : 0.0f), face.getX(), face.getBottom(), false));
    g.fillRoundedRectangle (face, 2.0f);
    for (float y = face.getY() + 2.0f; y < face.getBottom() - 2.0f; y += 2.0f)
    {
        g.setColour (juce::Colours::black.withAlpha (0.03f));
        g.drawHorizontalLine ((int) y, face.getX() + 2.0f, face.getRight() - 2.0f);
    }
    g.setColour (juce::Colour (0xff5B605D));
    g.fillRect (face.getX(), face.getBottom(), face.getWidth(), 6.0f - press);
    g.setColour (Theme::panel);
    tracked (g, getButtonText(), Theme::legend (8.5f), face, juce::Justification::centred, 0.18f);
}

void SelectorButton::paintButton (juce::Graphics& g, bool over, bool down)
{
    const auto b = getLocalBounds().toFloat();
    const bool on = getToggleState();
    const auto cap = b.withTrimmedBottom (14.0f).reduced (4.0f, 2.0f).translated (0.0f, down ? 1.0f : 0.0f);
    if (on) glow (g, cap.getCentre(), cap.getWidth() * 0.8f, Theme::gold, 0.35f);
    g.setColour (juce::Colour (0xff020d0c));
    g.fillRoundedRectangle (cap.expanded (2.0f), 3.0f);
    if (on)
        g.setGradientFill (juce::ColourGradient (Theme::lamp, cap.getCentreX(), cap.getCentreY(), Theme::gold, cap.getX(), cap.getBottom(), true));
    else
        g.setGradientFill (juce::ColourGradient (Theme::capCream.brighter (over ? 0.08f : 0.0f), cap.getX(), cap.getY(), juce::Colour (0xffC4BBA3), cap.getX(), cap.getBottom(), false));
    g.fillRoundedRectangle (cap, 2.0f);
    g.setColour (Theme::ink);
    tracked (g, getButtonText(), Theme::legend (8.0f), b.withTop (b.getBottom() - 12.0f), juce::Justification::centred, 0.18f);
}

//==============================================================================
KasetEditor::KasetEditor (KasetProcessor& p)
    : AudioProcessorEditor (&p), proc (p), dial (p), deckA (p, false), deckB (p, true), counter (p)
#if SVL_LICENSING
    , licenseOverlay (p.license, licenseStyle())
#endif
{
    setLookAndFeel (&lnf);
    setWantsKeyboardFocus (true);
    addMouseListener (this, true);
    mark = juce::ImageCache::getFromMemory (BinaryData::mark_cream_png, BinaryData::mark_cream_pngSize);
    wordmark = juce::ImageCache::getFromMemory (BinaryData::wordmark_cream_png, BinaryData::wordmark_cream_pngSize);

    for (auto* c : std::initializer_list<juce::Component*> { &dial, &deckA, &deckB, &ladder, &counter })
        addAndMakeVisible (*c);

    for (int i = 0; i < KasetProcessor::tapeNames.size(); ++i)
    {
        auto b = std::make_unique<SelectorButton> (KasetProcessor::tapeNames[i]);
        b->onClick = [this, i]
        {
            svl::setParamAsGesture (proc.apvts.getParameter ("tape"), (float) i);   // one click = one undo step
        };
        addAndMakeVisible (*b);
        tapeButtons.push_back (std::move (b));
    }

    auto setPlay = [this] (bool on)
    {
        svl::setParamAsGesture (proc.apvts.getParameter ("play"), on ? 1.0f : 0.0f);   // one click = one undo step
    };
    playKey.onClick = [setPlay] { setPlay (true); };
    stopKey.onClick = [setPlay] { setPlay (false); };
    autoKey.setClickingTogglesState (true);
    autoAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "autoGain", autoKey);
    for (auto* k : { &playKey, &stopKey, &autoKey })
        addAndMakeVisible (*k);

    // TRACK: a latching selector in the tape selector's language, under it. One click = one undo step (the attachment opens and closes the gesture).
    trackKey.setClickingTogglesState (true);
    trackKey.setTooltip ("TRACK: one low-latency head for recording through KASET. MIX (off): the full five-head transport.");
    trackAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "track", trackKey);
    addAndMakeVisible (trackKey);

    const std::pair<const char*, const char*> ids[] {
        { "input", "INPUT" }, { "drive", "SATURATION" }, { "wow", "WOW" }, { "flutter", "FLUTTER" },
        { "hiss", "HISS" }, { "wear", "WEAR" }, { "mix", "MIX" }, { "output", "OUTPUT" }
    };
    for (auto [id, name] : ids)
    {
        auto k = std::make_unique<Knob>();
        k->id = id;
        k->name = name;
        k->slider.setSliderStyle (juce::Slider::RotaryVerticalDrag);
        k->slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        k->slider.setRotaryParameters (kStart, kEnd, true);
        k->slider.setPopupDisplayEnabled (true, true, this);
        k->attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, id, k->slider);
        addAndMakeVisible (k->slider);
        knobs.push_back (std::move (k));
    }

    setSize (kW, kH);
    lastTick = juce::Time::getMillisecondCounterHiRes();
    timerCallback();
#if SVL_LICENSING
    // Licence: the card covers the panel until the plugin is activated; LICENSE reopens it.
    licenseButton.setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    licenseButton.setColour (juce::TextButton::textColourOffId, Theme::ink.withAlpha (0.6f));
    licenseButton.onClick = [this] { licenseOverlay.refresh(); licenseOverlay.setVisible (true); licenseOverlay.toFront (true); };
    addAndMakeVisible (licenseButton);
    addChildComponent (licenseOverlay);
    licenseOverlay.setVisible (! proc.license.isLicensed());
#endif
    startTimerHz (30);
}

bool KasetEditor::keyPressed (const juce::KeyPress& key)
{
#if SVL_LICENSING
    if (licenseOverlay.isVisible())
        return false;
#endif
    return svl::handleUndoKey (proc.undo, key);
}

void KasetEditor::mouseDown (const juce::MouseEvent& e)
{
#if SVL_LICENSING
    if (licenseOverlay.isVisible())
        return;
#endif
    svl::takeFocusOnClick (*this, e);
}

KasetEditor::~KasetEditor()
{
    setLookAndFeel (nullptr);
}

void KasetEditor::buildPanelTexture()
{
    panelTexture = juce::Image (juce::Image::ARGB, kW, kH, true);
    juce::Graphics g (panelTexture);

    // The wall behind the set.
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff141a19), 0.0f, 0.0f, juce::Colour (0xff070a09), 0.0f, (float) kH, false));
    g.fillAll();

    // Body: dark green moulded front with a fine brushed grain.
    const auto body = juce::Rectangle<float> (16.0f, 60.0f, (float) kW - 32.0f, (float) kH - 76.0f);
    juce::Path shape;
    shape.addRoundedRectangle (body, 30.0f);
    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.fillPath (shape, juce::AffineTransform::translation (0.0f, 8.0f));
    g.saveState();
    g.reduceClipRegion (shape);
    g.setGradientFill (juce::ColourGradient (Theme::panel.brighter (0.14f), 0.0f, body.getY(), Theme::panelDeep, 0.0f, body.getBottom(), false));
    g.fillRect (body);
    juce::Random rng (1983);
    for (int y = (int) body.getY(); y < (int) body.getBottom(); ++y)
    {
        const float v = rng.nextFloat();
        g.setColour ((v > 0.5f ? juce::Colours::white : juce::Colours::black).withAlpha (0.015f + 0.025f * std::abs (v - 0.5f)));
        g.fillRect ((int) body.getX(), y, (int) body.getWidth(), 1);
    }
    // Top strip: a lighter brushed band, as the silver strip on the reference sets.
    const auto strip = juce::Rectangle<float> (body.getX(), body.getY(), body.getWidth(), 124.0f);
    g.setGradientFill (juce::ColourGradient (Theme::panel.brighter (0.45f), 0.0f, strip.getY(), Theme::panel.brighter (0.2f), 0.0f, strip.getBottom(), false));
    g.fillRect (strip);
    for (int i = 0; i < 1400; ++i)
    {
        const float x = strip.getX() + rng.nextFloat() * strip.getWidth(), len = 40.0f + rng.nextFloat() * 300.0f;
        const int y = (int) strip.getY() + rng.nextInt ((int) strip.getHeight());
        g.setColour ((rng.nextBool() ? juce::Colours::white : juce::Colours::black).withAlpha (0.03f + rng.nextFloat() * 0.03f));
        g.drawHorizontalLine (y, x, x + len);
    }
    g.setColour (juce::Colour (0xffE7E8E3).withAlpha (0.6f));
    g.fillRect (strip.getX(), strip.getBottom() - 3.0f, strip.getWidth(), 1.0f);
    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.fillRect (strip.getX(), strip.getBottom() - 2.0f, strip.getWidth(), 2.0f);
    g.restoreState();
    g.setColour (juce::Colours::white.withAlpha (0.12f));
    g.strokePath (shape, juce::PathStrokeType (1.2f));
}

void KasetEditor::paintScale (juce::Graphics& g, const juce::Slider& s) const
{
    const auto c = s.getBounds().toFloat().getCentre();
    const float r = (float) s.getWidth() * 0.5f;
    g.setColour (Theme::ink.withAlpha (0.85f));
    g.setFont (Theme::fine (8.5f));
    for (int i = 0; i <= 10; ++i)
    {
        const float a = kStart + (kEnd - kStart) * (float) i / 10.0f;
        g.drawLine ({ polar (c, r + 2.0f, a), polar (c, r + (i % 5 == 0 ? 8.0f : 5.0f), a) }, i % 5 == 0 ? 1.4f : 1.0f);
        if (i % 5 == 0)
        {
            const auto p = polar (c, r + 15.0f, a);
            g.drawText (juce::String (i), juce::Rectangle<float> (p.x - 10.0f, p.y - 6.0f, 20.0f, 12.0f), juce::Justification::centred);
        }
    }
}

void KasetEditor::paint (juce::Graphics& g)
{
    if (panelTexture.isNull())
        buildPanelTexture();
    g.drawImageAt (panelTexture, 0, 0);
    const float W = (float) kW, H = (float) kH;

    // Carrying handle: a chrome tube on two pivot brackets, with a ribbed rubber grip.
    {
        const float x1 = W * 0.5f - 250.0f, x2 = W * 0.5f + 250.0f, cy = 24.0f, tube = 16.0f;
        auto chromeV = [&] (juce::Rectangle<float> r)   // cylinder lit from above
        {
            juce::ColourGradient cg (juce::Colour (0xff2E3331), 0.0f, r.getY(), juce::Colour (0xff1A1E1D), 0.0f, r.getBottom(), false);
            cg.addColour (0.28, juce::Colour (0xffF2F3EF));
            cg.addColour (0.45, juce::Colour (0xffA9AEAB));
            cg.addColour (0.8, juce::Colour (0xff4B5250));
            g.setGradientFill (cg);
        };
        // Brackets on the body.
        for (float bx : { x1, x2 })
        {
            juce::Path br;
            br.startNewSubPath (bx - 22.0f, 62.0f);
            br.lineTo (bx - 12.0f, cy);
            br.lineTo (bx + 12.0f, cy);
            br.lineTo (bx + 22.0f, 62.0f);
            br.closeSubPath();
            g.setColour (juce::Colours::black.withAlpha (0.5f));
            g.fillPath (br, juce::AffineTransform::translation (3.0f, 3.0f));
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3C4442), bx - 22.0f, 0.0f, juce::Colour (0xff111615), bx + 22.0f, 0.0f, false));
            g.fillPath (br);
            g.setColour (juce::Colours::white.withAlpha (0.15f));
            g.strokePath (br, juce::PathStrokeType (1.0f));
        }
        // The tube.
        const juce::Rectangle<float> bar (x1, cy - tube * 0.5f, x2 - x1, tube);
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRoundedRectangle (bar.translated (0.0f, 5.0f), tube * 0.5f);
        chromeV (bar);
        g.fillRoundedRectangle (bar, tube * 0.5f);
        // Rubber grip in the middle.
        const juce::Rectangle<float> grip (W * 0.5f - 110.0f, cy - tube * 0.5f - 3.0f, 220.0f, tube + 6.0f);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff2A302E), 0.0f, grip.getY(), juce::Colour (0xff070909), 0.0f, grip.getBottom(), false));
        g.fillRoundedRectangle (grip, 8.0f);
        for (float rx = grip.getX() + 10.0f; rx < grip.getRight() - 8.0f; rx += 6.0f)
        {
            g.setColour (juce::Colours::black.withAlpha (0.55f));
            g.drawVerticalLine ((int) rx, grip.getY() + 3.0f, grip.getBottom() - 3.0f);
            g.setColour (juce::Colours::white.withAlpha (0.07f));
            g.drawVerticalLine ((int) rx + 1, grip.getY() + 3.0f, grip.getBottom() - 3.0f);
        }
        g.setColour (juce::Colours::white.withAlpha (0.12f));
        g.drawRoundedRectangle (grip.reduced (0.5f), 8.0f, 1.0f);
        // Pivot hubs with their screws.
        for (float bx : { x1, x2 })
        {
            const juce::Rectangle<float> hub (bx - 14.0f, cy - 14.0f, 28.0f, 28.0f);
            g.setColour (juce::Colours::black.withAlpha (0.5f));
            g.fillEllipse (hub.translated (2.0f, 3.0f));
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xffF4F4F0), hub.getX(), hub.getY(), juce::Colour (0xff545A58), hub.getRight(), hub.getBottom(), false));
            g.fillEllipse (hub);
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff6A706E), hub.getX(), hub.getY(), juce::Colour (0xffDADBD6), hub.getRight(), hub.getBottom(), false));
            g.fillEllipse (hub.reduced (5.0f));
            g.setColour (juce::Colour (0xff2A2F2D));
            g.drawLine (bx - 4.0f, cy - 4.0f, bx + 4.0f, cy + 4.0f, 1.6f);
            g.drawLine (bx - 4.0f, cy + 4.0f, bx + 4.0f, cy - 4.0f, 1.6f);
        }
    }

    // Strip legends.
    auto bracket = [&] (float x1, float x2, float y, const juce::String& text)
    {
        const auto f = Theme::legend (8.5f).withExtraKerningFactor (0.24f);
        const float tw = juce::GlyphArrangement::getStringWidth (f, text) + 12.0f;
        const float mid = (x1 + x2) * 0.5f;
        g.setColour (Theme::ink.withAlpha (0.75f));
        g.drawLine (x1, y, mid - tw * 0.5f, y, 1.0f);
        g.drawLine (mid + tw * 0.5f, y, x2, y, 1.0f);
        g.drawLine (x1, y, x1, y + 4.0f, 1.0f);
        g.drawLine (x2, y, x2, y + 4.0f, 1.0f);
        g.setColour (Theme::ink);
        tracked (g, text, Theme::legend (8.5f), { mid - tw * 0.5f, y - 6.0f, tw, 12.0f }, juce::Justification::centred, 0.24f);
    };
    bracket ((float) playKey.getX(), (float) stopKey.getRight(), 80.0f, "SIGNAL PATH");
    bracket ((float) autoKey.getX(), (float) autoKey.getRight(), 80.0f, "LEVEL");
    bracket ((float) tapeButtons.front()->getX(), (float) tapeButtons.back()->getRight(), 80.0f, "TAPE SELECTOR");
    // TRACK latency window: what the host is told, in samples (66 / 72 with TRACK on at 44.1 / 48 kHz, 660 / 720 off).
    {
        const auto win = juce::Rectangle<float> (908.0f, 144.0f, 66.0f, 22.0f);
        g.setColour (juce::Colour (0xff020d0c));
        g.fillRoundedRectangle (win, 3.0f);
        g.setColour (Theme::vfdOff);
        g.drawRoundedRectangle (win.reduced (0.5f), 3.0f, 1.0f);
        g.setColour (trackKey.getToggleState() ? Theme::vfd : Theme::vfd.withAlpha (0.45f));
        tracked (g, juce::String (proc.getLatencySamples()) + " SMP", Theme::mono (9.0f), win, juce::Justification::centred, 0.1f);
        g.setColour (Theme::ink.withAlpha (0.85f));
        tracked (g, "LATENCY", Theme::legend (8.0f), { 982.0f, 144.0f, 62.0f, 22.0f }, juce::Justification::centredLeft, 0.24f);
    }
    // Knob row: the machined knobs shared with Kubbe.
    const std::tuple<int, int, const char*> groups[] { { 0, 1, "RECORD" }, { 2, 3, "TRANSPORT" }, { 4, 5, "TAPE" }, { 6, 7, "OUTPUT" } };
    for (auto [a, z, name] : groups)
        bracket ((float) knobs[(size_t) a]->slider.getX() - 20.0f, (float) knobs[(size_t) z]->slider.getRight() + 20.0f, 204.0f, name);
    for (auto& k : knobs)
    {
        paintScale (g, k->slider);
        g.setColour (Theme::ink);
        tracked (g, k->name, Theme::legend (9.5f), k->slider.getBounds().toFloat().withY ((float) k->slider.getBottom() + 20.0f).withHeight (13.0f).expanded (30.0f, 0.0f),
                 juce::Justification::centred, 0.2f);
    }

    // Centre strip between the decks: counter and record level, as on the reference compo.
    const auto centre = juce::Rectangle<float> ((float) deckA.getRight() + 10.0f, (float) deckA.getY(), (float) (deckB.getX() - deckA.getRight()) - 20.0f, (float) deckA.getHeight());
    g.setColour (juce::Colour (0xff020d0c).withAlpha (0.5f));
    g.fillRoundedRectangle (centre, 6.0f);
    g.setColour (juce::Colours::white.withAlpha (0.08f));
    g.drawRoundedRectangle (centre.reduced (0.5f), 6.0f, 1.0f);
    g.setColour (Theme::ink.withAlpha (0.85f));
    tracked (g, "TAPE COUNTER", Theme::legend (8.0f), { centre.getX(), (float) counter.getY() - 16.0f, centre.getWidth(), 11.0f }, juce::Justification::centred, 0.24f);
    tracked (g, "REC LEVEL", Theme::legend (8.0f), { centre.getX(), (float) ladder.getY() - 16.0f, centre.getWidth(), 11.0f }, juce::Justification::centred, 0.24f);


    // Name plate under the decks.
    const float nh = 26.0f;
    drawProductName (g, "KASET", W * 0.5f - nameWidth (5, nh) * 0.5f, 562.0f, nh, Theme::ink);
    const juce::Rectangle<float> plate (W * 0.5f - 150.0f, 596.0f, 300.0f, 18.0f);
    g.setColour (Theme::gold);
    g.drawRect (plate, 1.0f);
    tracked (g, "HIGH SPEED DUBBING   MODEL KS-83", Theme::legend (8.5f), plate, juce::Justification::centred, 0.22f);

    // Foot: slogan left, maker's badge right.
    g.setColour (Theme::ink.withAlpha (0.55f));
    tracked (g, "MEASURED, NOT GUESSED", Theme::fine (8.0f), { 50.0f, H - 40.0f, 240.0f, 12.0f }, juce::Justification::centredLeft, 0.2f);
    const float badgeRight = W - 50.0f;
    g.setOpacity (0.85f);
    g.drawImage (wordmark, juce::Rectangle<float> (badgeRight - 104.0f, H - 41.0f, 104.0f, 13.0f), juce::RectanglePlacement::xRight | juce::RectanglePlacement::yMid);
    g.drawImage (mark, juce::Rectangle<float> (badgeRight - 104.0f - 32.0f, H - 46.0f, 26.0f, 20.0f), juce::RectanglePlacement::centred);
    g.setOpacity (1.0f);
}

void KasetEditor::resized()
{
#if SVL_LICENSING
    licenseOverlay.setBounds (getLocalBounds());
    licenseButton.setBounds (getWidth() / 2 + 170, kH - 44, 70, 20);
#endif
    // Top strip.
    playKey.setBounds (46, 92, 66, 62);
    stopKey.setBounds (118, 92, 66, 62);
    autoKey.setBounds (202, 92, 96, 62);
    dial.setBounds (318, 76, 480, 92);
    for (int i = 0; i < (int) tapeButtons.size(); ++i)
        tapeButtons[(size_t) i]->setBounds (832 + i * 72, 92, 66, 40);
    trackKey.setBounds (832, 138, 66, 34);

    // Knob row.
    const int size = 62, cell = 112, gap = 20, cy = 262;
    int x = (kW - (8 * cell + 3 * gap)) / 2;
    for (int i = 0; i < (int) knobs.size(); ++i)
    {
        if (i > 0 && i % 2 == 0) x += gap;
        knobs[(size_t) i]->slider.setBounds (x + (cell - size) / 2, cy - size / 2, size, size);
        x += cell;
    }

    // Decks with the centre strip between them.
    deckA.setBounds (40, 346, 360, 206);
    deckB.setBounds (kW - 40 - 360, 346, 360, 206);
    counter.setBounds (kW / 2 - 60, 382, 140, 30);
    ladder.setBounds (kW / 2 - 110, 452, 220, 50);
}

void KasetEditor::timerCallback()
{
#if SVL_LICENSING
    if (! proc.license.isLicensed() && ! licenseOverlay.isVisible())
    {
        licenseOverlay.refresh();
        licenseOverlay.setVisible (true);
    }
#endif
    const double now = juce::Time::getMillisecondCounterHiRes();
    const float dt = (float) juce::jlimit (0.0, 0.2, (now - lastTick) / 1000.0);
    lastTick = now;
    deckA.advance (dt);
    deckB.advance (dt);
    const float l = proc.uiLevel[0].load(), r = proc.uiLevel[1].load();
    ladder.setLevels (l, r);
    counter.repaint();

    const int tape = (int) proc.apvts.getRawParameterValue ("tape")->load();
    if (tape != lastTape)
    {
        lastTape = tape;
        for (int i = 0; i < (int) tapeButtons.size(); ++i)
            tapeButtons[(size_t) i]->setToggleState (i == tape, juce::dontSendNotification);
        deckA.repaint();
        deckB.repaint();
    }
    const bool play = proc.apvts.getRawParameterValue ("play")->load() > 0.5f;
    if ((int) play != lastPlay)
    {
        lastPlay = (int) play;
        playKey.setToggleState (play, juce::dontSendNotification);
        stopKey.setToggleState (! play, juce::dontSendNotification);
        for (auto& k : knobs)
        {
            k->slider.getProperties().set ("bypassed", ! play);
            k->slider.repaint();
        }
        deckA.repaint();
        deckB.repaint();
    }
    dial.repaint();
    if (proc.getLatencySamples() != lastLatency)
    {
        lastLatency = proc.getLatencySamples();
        repaint (900, 138, 150, 40);
    }
}
