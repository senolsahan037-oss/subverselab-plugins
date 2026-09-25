#include "PluginEditor.h"
#include "Theme.h"
#include "BinaryData.h"

namespace
{
constexpr int kW = 1120, kH = 440, kEar = 36;
constexpr float kStart = juce::MathConstants<float>::pi * 1.25f, kEnd = juce::MathConstants<float>::pi * 2.75f;

juce::Point<float> polar (juce::Point<float> c, float r, float angle)
{
    return { c.x + std::sin (angle) * r, c.y - std::cos (angle) * r };
}

/** Silkscreen text with letter spacing, as panel legends were set. */
void tracked (juce::Graphics& g, const juce::String& s, juce::Font f, juce::Rectangle<float> r,
              juce::Justification j, float tracking)
{
    g.setFont (f.withExtraKerningFactor (tracking));
    g.drawText (s, r, j, false);
}

/** The product name in the squared, extended letterforms of late-70s rack
    gear (the Microgramma look), drawn as strokes so no font is shipped.
    Only the letters the name needs. Returns the width drawn. */
float drawProductName (juce::Graphics& g, const juce::String& name, float x, float y, float h)
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
            case 'U':
                p.startNewSubPath (L, T); p.lineTo (L, B - r);
                p.quadraticTo (L, B, L + r, B); p.lineTo (R - r, B);
                p.quadraticTo (R, B, R, B - r); p.lineTo (R, T);
                break;
            case 'B':
            {
                const float rb = r * 0.75f, Rt = R - (R - L) * 0.06f;
                p.startNewSubPath (L, T); p.lineTo (Rt - rb, T); p.quadraticTo (Rt, T, Rt, T + rb);
                p.lineTo (Rt, M - rb * 0.6f); p.quadraticTo (Rt, M, Rt - rb * 0.6f, M); p.lineTo (L, M);
                p.startNewSubPath (L, M); p.lineTo (R - rb, M); p.quadraticTo (R, M, R, M + rb);
                p.lineTo (R, B - rb); p.quadraticTo (R, B, R - rb, B); p.lineTo (L, B); p.closeSubPath();
                p.startNewSubPath (L, T); p.lineTo (L, B);
                break;
            }
            case 'E':
                p.startNewSubPath (R, T); p.lineTo (L + r, T); p.quadraticTo (L, T, L, T + r);
                p.lineTo (L, B - r); p.quadraticTo (L, B, L + r, B); p.lineTo (R, B);
                p.startNewSubPath (L, M); p.lineTo (L + (R - L) * 0.82f, M);
                break;
            default: break;
        }
        all.addPath (p);
        cx += w + gap;
    }
    const juce::PathStrokeType stroke (t, juce::PathStrokeType::mitered, juce::PathStrokeType::butt);
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.strokePath (all, stroke, juce::AffineTransform::translation (0.0f, 1.2f));
    g.setColour (Theme::ink);
    g.strokePath (all, stroke);
    return cx - gap - x;
}

void glow (juce::Graphics& g, juce::Point<float> c, float radius, juce::Colour col, float alpha)
{
    juce::ColourGradient grad (col.withAlpha (alpha), c.x, c.y, col.withAlpha (0.0f), c.x + radius, c.y, true);
    g.setGradientFill (grad);
    g.fillEllipse (c.x - radius, c.y - radius, radius * 2.0f, radius * 2.0f);
}
} // namespace

//==============================================================================
KubbeLookAndFeel::KubbeLookAndFeel()
{
    setColour (juce::PopupMenu::backgroundColourId, Theme::panelDeep);
    setColour (juce::PopupMenu::textColourId, Theme::ink);
    setColour (juce::TooltipWindow::backgroundColourId, Theme::panelDeep);
}

void KubbeLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                          float startAngle, float endAngle, juce::Slider& s)
{
    const bool stepped = s.getProperties()["stepped"];
    const float d = (float) juce::jmin (w, h) - 4.0f, r = d * 0.5f;
    const juce::Point<float> c ((float) x + (float) w * 0.5f, (float) y + (float) h * 0.5f);
    const float angle = startAngle + pos * (endAngle - startAngle);
    const float dim = s.isEnabled() ? 1.0f : 0.45f;

    // Shadow on the panel, light from the top left.
    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.fillEllipse (c.x - r + 2.0f, c.y - r + 4.0f, d, d);

    if (stepped)
    {
        // A bakelite pointer knob: round body, a long nose, a cream line.
        juce::Path body;
        body.addEllipse (c.x - r * 0.78f, c.y - r * 0.78f, r * 1.56f, r * 1.56f);
        juce::Path nose;
        nose.addTriangle (c.x - r * 0.42f, c.y - r * 0.55f, c.x + r * 0.42f, c.y - r * 0.55f, c.x, c.y - r * 1.02f);
        nose.addEllipse (c.x - r * 0.14f, c.y - r * 1.06f, r * 0.28f, r * 0.28f);
        body.addPath (nose);
        body.applyTransform (juce::AffineTransform::rotation (angle, c.x, c.y));
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff2a2f2d), c.x - r, c.y - r, juce::Colour (0xff070909), c.x + r, c.y + r, false));
        g.fillPath (body);
        g.setColour (juce::Colours::white.withAlpha (0.10f));
        g.strokePath (body, juce::PathStrokeType (1.0f));
        g.setColour (Theme::ink.withAlpha (dim));
        g.drawLine ({ polar (c, r * 0.15f, angle), polar (c, r * 0.9f, angle) }, 2.2f);
        return;
    }

    // Knurled skirt.
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff2b302e), c.x, c.y - r, juce::Colour (0xff0b0d0c), c.x, c.y + r, false));
    g.fillEllipse (c.x - r, c.y - r, d, d);
    for (int i = 0; i < 60; ++i)
    {
        const float a = juce::MathConstants<float>::twoPi * (float) i / 60.0f;
        const float light = 0.5f + 0.5f * std::cos (a + 0.8f);
        g.setColour (juce::Colours::white.withAlpha (0.05f + 0.13f * light));
        g.drawLine ({ polar (c, r * 0.86f, a), polar (c, r - 0.8f, a) }, 1.0f);
    }

    // Machined cap: brushed, with fine turning rings and a sheen.
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

    // Gold index, engraved into the cap and run onto the skirt.
    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.drawLine ({ polar (c, cr * 0.28f, angle), polar (c, r * 0.97f, angle) }, 4.0f);
    g.setColour (Theme::gold.withMultipliedAlpha (dim));
    g.drawLine ({ polar (c, cr * 0.28f, angle), polar (c, r * 0.97f, angle) }, 2.4f);
}

//==============================================================================
PlaceButton::PlaceButton (int i, KubbeProcessor& p) : juce::Button (KubbeProcessor::placeNames[i]), index (i), proc (p) {}

void PlaceButton::paintButton (juce::Graphics& g, bool over, bool down)
{
    const bool on = (int) proc.apvts.getRawParameterValue ("place")->load() == index;
    const auto b = getLocalBounds().toFloat();
    const auto well = b.reduced (8.0f);

    if (on)
        glow (g, well.getCentre(), well.getWidth() * 0.75f, Theme::gold, 0.38f);

    g.setColour (juce::Colour (0xff011512));
    g.fillRoundedRectangle (well, 6.0f);
    g.setColour (juce::Colours::white.withAlpha (0.08f));
    g.drawRoundedRectangle (well.translated (0.0f, 1.0f), 6.0f, 1.0f);

    auto cap = well.reduced (4.0f).translated (0.0f, down ? 1.5f : 0.0f);
    if (on)
        g.setGradientFill (juce::ColourGradient (Theme::lamp, cap.getCentreX(), cap.getCentreY() - 4.0f, Theme::gold, cap.getX(), cap.getBottom(), true));
    else
        g.setGradientFill (juce::ColourGradient (Theme::capCream.brighter (over ? 0.08f : 0.0f), cap.getX(), cap.getY(),
                                                 juce::Colour (0xffC4BBA3), cap.getX(), cap.getBottom(), false));
    g.fillRoundedRectangle (cap, 4.0f);
    g.setColour (juce::Colours::white.withAlpha (on ? 0.5f : 0.35f));
    g.drawLine (cap.getX() + 4.0f, cap.getY() + 1.0f, cap.getRight() - 4.0f, cap.getY() + 1.0f, 1.0f);
    g.setColour (juce::Colours::black.withAlpha (0.25f));
    g.drawRoundedRectangle (cap, 4.0f, 1.0f);

    g.setColour (Theme::panel.withAlpha (on ? 0.95f : 0.85f));
    tracked (g, KubbeProcessor::placeNames[index].toUpperCase(), Theme::legend (12.0f), cap.withTrimmedTop (4.0f), juce::Justification::centred, 0.14f);
    g.setColour (Theme::panel.withAlpha (0.5f));
    g.setFont (Theme::mono (8.0f));
    g.drawText (juce::String (index + 1), cap.reduced (5.0f, 3.0f), juce::Justification::topLeft);
}

//==============================================================================
void ToggleSwitch::paintButton (juce::Graphics& g, bool, bool)
{
    const auto b = getLocalBounds().toFloat();
    const bool on = getToggleState();
    const float dim = isEnabled() ? 1.0f : 0.4f;

    // LED.
    const juce::Point<float> led (b.getCentreX(), 12.0f);
    if (on) glow (g, led, 16.0f, Theme::lamp, 0.55f * dim);
    g.setColour (juce::Colour (0xff0b0a06));
    g.fillEllipse (led.x - 7.0f, led.y - 7.0f, 14.0f, 14.0f);
    g.setGradientFill (juce::ColourGradient (on ? Theme::lamp.withMultipliedAlpha (dim) : juce::Colour (0xff4a3a18), led.x - 2.0f, led.y - 2.0f,
                                             on ? Theme::gold : juce::Colour (0xff231a09), led.x + 5.0f, led.y + 5.0f, true));
    g.fillEllipse (led.x - 5.5f, led.y - 5.5f, 11.0f, 11.0f);
    g.setColour (juce::Colours::white.withAlpha (on ? 0.7f : 0.25f));
    g.fillEllipse (led.x - 3.0f, led.y - 3.5f, 2.5f, 2.0f);

    // Nut.
    const juce::Point<float> c (b.getCentreX(), b.getHeight() * 0.6f);
    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.fillEllipse (c.x - 13.0f, c.y - 11.0f, 28.0f, 28.0f);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xffEDEDE8), c.x - 12.0f, c.y - 12.0f, juce::Colour (0xff7E8481), c.x + 12.0f, c.y + 12.0f, false));
    g.fillEllipse (c.x - 13.0f, c.y - 13.0f, 26.0f, 26.0f);
    juce::Path hex;
    for (int i = 0; i < 6; ++i)
    {
        const auto p = polar (c, 8.5f, juce::MathConstants<float>::pi / 3.0f * (float) i + 0.52f);
        if (i == 0) hex.startNewSubPath (p); else hex.lineTo (p);
    }
    hex.closeSubPath();
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xffB9BDBA), c.x - 8.0f, c.y - 8.0f, juce::Colour (0xff4F5553), c.x + 8.0f, c.y + 8.0f, false));
    g.fillPath (hex);

    // Lever: up is on.
    const float dir = on ? -1.0f : 1.0f, len = 34.0f;
    juce::Path lever;
    lever.startNewSubPath (c.x - 4.5f, c.y);
    lever.lineTo (c.x - 3.0f, c.y + dir * len);
    lever.lineTo (c.x + 3.0f, c.y + dir * len);
    lever.lineTo (c.x + 4.5f, c.y);
    lever.closeSubPath();
    const juce::Point<float> tip (c.x, c.y + dir * len);
    g.setColour (juce::Colours::black.withAlpha (0.4f));
    g.fillPath (lever, juce::AffineTransform::translation (3.0f, 4.0f));
    g.fillEllipse (tip.x - 3.0f, tip.y - 2.0f, 12.0f, 12.0f);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xffF7F7F3), c.x - 5.0f, c.y, juce::Colour (0xff6F7572), c.x + 5.0f, c.y, false));
    g.fillPath (lever);
    g.setGradientFill (juce::ColourGradient (juce::Colours::white, tip.x - 3.0f, tip.y - 3.0f, juce::Colour (0xff767C79), tip.x + 5.0f, tip.y + 5.0f, true));
    g.fillEllipse (tip.x - 6.0f, tip.y - 6.0f, 12.0f, 12.0f);
}

//==============================================================================
void CrtDisplay::refresh()
{
    const auto ctl = proc.controlsFor (proc.uiBpm.load());
    const int place = (int) proc.apvts.getRawParameterValue ("place")->load();
    const juce::String key = juce::String (place) + "|" + juce::String (ctl.decay) + "|" + juce::String (ctl.predelayMs)
                           + "|" + juce::String (ctl.distance) + "|" + juce::String (ctl.lowCutHz) + "|" + juce::String (ctl.highCutHz)
                           + "|" + juce::String ((int) ctl.freeze) + "|" + juce::String (ctl.bpm);
    if (key == lastKey)
        return;
    lastKey = key;

    // Freeze is shown as what it is: no decay.
    svrb::Controls measure = ctl;
    measure.freeze = false;

    const double sr = 16000.0;
    engine.prepare (sr, svrb::characterAt (place));
    engine.update (measure);

    const float seconds = juce::jlimit (3.0f, 20.0f, svrb::characterAt (place).rt60 * ctl.decay * 1.6f + 1.5f);
    const int total = (int) (seconds * sr), block = 512;
    std::vector<float> l ((size_t) total, 0.0f), r ((size_t) total, 0.0f);
    l[0] = r[0] = 1.0f;
    for (int p = 0; p < total; p += block)
    {
        const int n = juce::jmin (block, total - p);
        engine.process (&l[(size_t) p], &r[(size_t) p], &l[(size_t) p], &r[(size_t) p], n);
    }

    const int binLen = (int) (binSeconds * sr);
    const int bins = total / binLen;
    envDb.assign ((size_t) bins, -120.0f);
    edcDb.assign ((size_t) bins, -120.0f);
    std::vector<double> energy ((size_t) bins, 0.0);
    for (int b = 0; b < bins; ++b)
        for (int i = b * binLen; i < (b + 1) * binLen; ++i)
            energy[(size_t) b] += (double) l[(size_t) i] * l[(size_t) i] + (double) r[(size_t) i] * r[(size_t) i];

    const double peak = *std::max_element (energy.begin(), energy.end()) + 1.0e-30;
    double tail = 0.0;
    std::vector<double> edc ((size_t) bins);
    for (int b = bins - 1; b >= 0; --b)
        edc[(size_t) b] = (tail += energy[(size_t) b]);
    for (int b = 0; b < bins; ++b)
    {
        envDb[(size_t) b] = (float) (10.0 * std::log10 (energy[(size_t) b] / peak + 1.0e-12));
        edcDb[(size_t) b] = (float) (10.0 * std::log10 (edc[(size_t) b] / edc[0] + 1.0e-12));
    }

    // T30, extrapolated to 60 dB.
    auto crossing = [this] (float db)
    {
        for (size_t b = 0; b < edcDb.size(); ++b)
            if (edcDb[b] < db) return (float) b * binSeconds;
        return (float) edcDb.size() * binSeconds;
    };
    t60 = (crossing (-35.0f) - crossing (-5.0f)) * 2.0f;
    repaint();
}

void CrtDisplay::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();

    // Bezel.
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1d2624), b.getX(), b.getY(), juce::Colour (0xff040706), b.getX(), b.getBottom(), false));
    g.fillRoundedRectangle (b, 12.0f);
    g.setColour (juce::Colours::white.withAlpha (0.10f));
    g.drawRoundedRectangle (b.reduced (0.5f), 12.0f, 1.0f);

    const auto screen = b.reduced (12.0f);
    juce::Path tube;
    tube.addRoundedRectangle (screen, 22.0f);
    g.saveState();
    g.reduceClipRegion (tube);

    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff06302A), screen.getCentreX(), screen.getCentreY(),
                                             Theme::screen, screen.getX(), screen.getY(), true));
    g.fillRect (screen);

    const auto plot = screen.reduced (22.0f, 0.0f).withTrimmedTop (30.0f).withTrimmedBottom (24.0f);

    // Graticule: 10 by 6, centre axis with minor ticks, as on a scope.
    g.setColour (Theme::phosphor.withAlpha (0.09f));
    for (int i = 0; i <= 10; ++i)
        g.drawVerticalLine ((int) (plot.getX() + plot.getWidth() * (float) i / 10.0f), plot.getY(), plot.getBottom());
    for (int i = 0; i <= 6; ++i)
        g.drawHorizontalLine ((int) (plot.getY() + plot.getHeight() * (float) i / 6.0f), plot.getX(), plot.getRight());
    g.setColour (Theme::phosphor.withAlpha (0.22f));
    for (int i = 0; i <= 50; ++i)
    {
        const float x = plot.getX() + plot.getWidth() * (float) i / 50.0f;
        g.drawLine (x, plot.getCentreY() - 2.5f, x, plot.getCentreY() + 2.5f, 1.0f);
    }

    if (! envDb.empty())
    {
        const float totalSec = (float) envDb.size() * binSeconds;
        auto xOf = [&] (float sec) { return plot.getX() + sec / totalSec * plot.getWidth(); };
        auto yOf = [&] (float db) { return plot.getY() + juce::jlimit (0.0f, 1.0f, -db / 72.0f) * plot.getHeight(); };

        // Energy per 20 ms as a haze of phosphor; reflections and echoes show as spikes.
        g.setColour (Theme::phosphor.withAlpha (0.16f));
        for (size_t i = 0; i < envDb.size(); ++i)
        {
            const float x = xOf ((float) i * binSeconds);
            g.drawLine (x, yOf (envDb[i]), x, plot.getBottom(), 1.6f);
        }

        g.setColour (Theme::gold.withAlpha (0.55f));
        const float y60 = yOf (-60.0f);
        for (float x = plot.getX(); x < plot.getRight(); x += 8.0f)
            g.drawLine (x, y60, juce::jmin (x + 4.0f, plot.getRight()), y60, 1.0f);
        g.setFont (Theme::mono (9.0f));
        g.drawText ("-60 dB", juce::Rectangle<float> (plot.getRight() - 50.0f, y60 - 13.0f, 50.0f, 12.0f), juce::Justification::right);

        juce::Path edc;
        for (size_t i = 0; i < edcDb.size(); ++i)
        {
            const juce::Point<float> pt (xOf ((float) i * binSeconds), yOf (edcDb[i]));
            if (i == 0) edc.startNewSubPath (pt); else edc.lineTo (pt);
        }
        const std::pair<float, float> halo[] { { 9.0f, 0.06f }, { 5.0f, 0.14f }, { 2.8f, 0.35f } };
        for (auto [w, a] : halo)
        {
            g.setColour (Theme::phosphor.withAlpha (a));
            g.strokePath (edc, juce::PathStrokeType (w, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        g.setColour (Theme::phosphor);
        g.strokePath (edc, juce::PathStrokeType (1.4f));

        g.setColour (Theme::phosphor.withAlpha (0.6f));
        g.setFont (Theme::mono (9.0f));
        for (int s = 0; s <= (int) totalSec; s += totalSec > 10.0f ? 2 : 1)
            g.drawText (juce::String (s) + "s", juce::Rectangle<float> (xOf ((float) s) - 12.0f, plot.getBottom() + 5.0f, 24.0f, 12.0f),
                        juce::Justification::centred);
    }

    const int place = (int) proc.apvts.getRawParameterValue ("place")->load();
    const bool frozen = proc.apvts.getRawParameterValue ("freeze")->load() > 0.5f;
    g.setColour (Theme::phosphor.withAlpha (0.85f));
    g.setFont (Theme::mono (10.5f));
    const auto head = screen.reduced (24.0f, 10.0f).removeFromTop (14.0f);
    g.drawText ("DECAY ENERGY  MEASURED ON AN IMPULSE", head, juce::Justification::left);
    g.drawText (frozen ? "FREEZE  HOLD" : KubbeProcessor::placeNames[place].toUpperCase() + "  T60 " + juce::String (t60, 2) + "S",
                head, juce::Justification::right);

    // Scanlines, curvature shading, glass.
    g.setColour (juce::Colours::black.withAlpha (0.22f));
    for (float y = screen.getY(); y < screen.getBottom(); y += 3.0f)
        g.fillRect (screen.getX(), y, screen.getWidth(), 1.0f);
    g.setGradientFill (juce::ColourGradient (juce::Colours::transparentBlack, screen.getCentreX(), screen.getCentreY(),
                                             juce::Colours::black.withAlpha (0.55f), screen.getX() - 10.0f, screen.getY() - 10.0f, true));
    g.fillRect (screen);
    g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.035f), screen.getX() + 40.0f, screen.getY() + 10.0f,
                                             juce::Colours::white.withAlpha (0.0f), screen.getX() + 40.0f, screen.getY() + screen.getHeight() * 0.45f, false));
    g.fillEllipse (screen.getX() + 10.0f, screen.getY() - 30.0f, screen.getWidth() * 0.7f, screen.getHeight() * 0.6f);
    g.restoreState();

    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.drawRoundedRectangle (screen, 22.0f, 1.5f);
}

//==============================================================================
void VfdReadout::show (const juce::String& d, const juce::String& c)
{
    if (d != digits || c != caption)
    {
        digits = d;
        caption = c;
        repaint();
    }
}

void VfdReadout::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    g.setColour (juce::Colour (0xff010605));
    g.fillRoundedRectangle (b, 6.0f);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff041A18), b.getCentreX(), b.getY(), juce::Colour (0xff010807), b.getCentreX(), b.getBottom(), false));
    g.fillRoundedRectangle (b.reduced (5.0f), 4.0f);

    // Cells: each char is a digit, '-', '/' or ' '; a '.' lights the previous cell's point.
    struct Cell { char ch = ' '; bool dp = false; };
    std::vector<Cell> cells;
    for (auto ch : digits)
    {
        if (ch == '.' && ! cells.empty()) cells.back().dp = true;
        else cells.push_back ({ (char) ch, false });
    }
    const int numCells = 5;
    while ((int) cells.size() < numCells) cells.insert (cells.begin(), Cell {});
    while ((int) cells.size() > numCells) cells.erase (cells.begin());

    static const uint8_t seg[10] = { 0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f };  // gfedcba
    const float cw = 26.0f, ch = 42.0f, t = 5.0f, slant = 0.12f, gap = 14.0f;
    const float x0 = b.getRight() - 22.0f - (float) numCells * (cw + gap) + gap, y0 = b.getY() + 13.0f;

    auto segment = [&] (float cx, int s)
    {
        juce::Point<float> p1, p2;
        const float hh = ch * 0.5f;
        switch (s)
        {
            case 0: p1 = { 0, 0 };   p2 = { cw, 0 };   break;   // a
            case 1: p1 = { cw, 0 };  p2 = { cw, hh };  break;   // b
            case 2: p1 = { cw, hh }; p2 = { cw, ch };  break;   // c
            case 3: p1 = { 0, ch };  p2 = { cw, ch };  break;   // d
            case 4: p1 = { 0, hh };  p2 = { 0, ch };   break;   // e
            case 5: p1 = { 0, 0 };   p2 = { 0, hh };   break;   // f
            default: p1 = { 0, hh }; p2 = { cw, hh };  break;   // g
        }
        auto map = [&] (juce::Point<float> p) { return juce::Point<float> (cx + p.x + (ch - p.y) * slant, y0 + p.y); };
        const auto a = map (p1), z = map (p2);
        const auto dir = (z - a) / a.getDistanceFrom (z);
        const juce::Point<float> n (-dir.y, dir.x);
        const auto a2 = a + dir * (t * 0.9f), z2 = z - dir * (t * 0.9f);
        juce::Path path;
        path.startNewSubPath (a2 - dir * (t * 0.5f));
        path.lineTo (a2 + n * (t * 0.5f));
        path.lineTo (z2 + n * (t * 0.5f));
        path.lineTo (z2 + dir * (t * 0.5f));
        path.lineTo (z2 - n * (t * 0.5f));
        path.lineTo (a2 - n * (t * 0.5f));
        path.closeSubPath();
        return path;
    };

    for (int i = 0; i < numCells; ++i)
    {
        const float cx = x0 + (float) i * (cw + gap);
        const Cell& cell = cells[(size_t) i];
        uint8_t lit = 0;
        if (cell.ch >= '0' && cell.ch <= '9') lit = seg[cell.ch - '0'];
        else if (cell.ch == '-') lit = 0x40;

        for (int s = 0; s < 7; ++s)
        {
            const bool on = (lit >> s) & 1;
            const auto path = segment (cx, s);
            if (on)
            {
                g.setColour (Theme::vfd.withAlpha (0.18f));
                g.strokePath (path, juce::PathStrokeType (4.0f));
            }
            g.setColour (on ? Theme::vfd : Theme::vfdOff);
            g.fillPath (path);
        }
        if (cell.ch == '/')
        {
            g.setColour (Theme::vfd.withAlpha (0.18f));
            g.drawLine (cx + cw + ch * slant * 0.1f, y0 + 3.0f, cx + ch * slant * 0.9f, y0 + ch - 3.0f, t + 4.0f);
            g.setColour (Theme::vfd);
            g.drawLine (cx + cw + ch * slant * 0.1f, y0 + 3.0f, cx + ch * slant * 0.9f, y0 + ch - 3.0f, t);
        }
        const juce::Point<float> dp (cx + cw + 7.0f, y0 + ch);
        g.setColour (cell.dp ? Theme::vfd : Theme::vfdOff);
        g.fillEllipse (dp.x - 3.0f, dp.y - 3.0f, 6.0f, 6.0f);
    }

    g.setColour (Theme::vfd.withAlpha (0.9f));
    g.setFont (Theme::mono (10.0f));
    g.drawText (caption, b.reduced (18.0f, 7.0f).removeFromBottom (13.0f), juce::Justification::centredLeft);

    // Blue-green filter glass.
    g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.07f), b.getX(), b.getY(), juce::Colours::white.withAlpha (0.0f), b.getX(), b.getCentreY(), false));
    g.fillRoundedRectangle (b.reduced (5.0f), 4.0f);
    g.setColour (juce::Colours::white.withAlpha (0.08f));
    g.drawRoundedRectangle (b.reduced (0.5f), 6.0f, 1.0f);
}

//==============================================================================
void DuckMeter::setDb (float db)
{
    // Needle ballistics: quick down, slower back, like a VU.
    const float target = juce::jlimit (-24.0f, 0.0f, db);
    const float next = needleDb + (target - needleDb) * (target < needleDb ? 0.45f : 0.12f);
    if (std::abs (next - needleDb) > 0.01f)
    {
        needleDb = next;
        repaint();
    }
}

void DuckMeter::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    g.setColour (juce::Colour (0xff050807));
    g.fillRoundedRectangle (b, 6.0f);

    const auto face = b.reduced (6.0f);
    g.saveState();
    g.reduceClipRegion (face.toNearestInt());
    // Backlit card, warm like the bulb behind it.
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xffFBF5E3), face.getCentreX(), face.getBottom(),
                                             juce::Colour (0xffD9CBA4), face.getX(), face.getY(), true));
    g.fillRect (face);

    const juce::Point<float> pivot (face.getCentreX(), face.getBottom() + face.getHeight() * 0.62f);
    const float R = face.getHeight() * 1.0f;
    auto angleOf = [] (float db) { return juce::jmap (db, -24.0f, 0.0f, -0.6f, 0.6f); };

    juce::Path zone;
    zone.addCentredArc (pivot.x, pivot.y, R - 3.0f, R - 3.0f, 0.0f, angleOf (-24.0f), angleOf (-12.0f), true);
    g.setColour (Theme::gold);
    g.strokePath (zone, juce::PathStrokeType (4.0f));
    juce::Path arc;
    arc.addCentredArc (pivot.x, pivot.y, R, R, 0.0f, angleOf (-24.0f), angleOf (0.0f), true);
    g.setColour (Theme::panel);
    g.strokePath (arc, juce::PathStrokeType (1.2f));

    g.setFont (Theme::legend (8.5f));
    for (float db : { 0.0f, -3.0f, -6.0f, -9.0f, -12.0f, -18.0f, -24.0f })
    {
        const float a = angleOf (db);
        g.drawLine ({ polar (pivot, R, a), polar (pivot, R + 6.0f, a) }, 1.2f);
        const auto p = polar (pivot, R + 14.0f, a);
        g.drawText (juce::String ((int) db), juce::Rectangle<float> (p.x - 12.0f, p.y - 6.0f, 24.0f, 12.0f), juce::Justification::centred);
    }
    for (float db = -24.0f; db <= 0.0f; db += 1.5f)
        g.drawLine ({ polar (pivot, R, angleOf (db)), polar (pivot, R + 3.0f, angleOf (db)) }, 0.7f);

    g.setFont (Theme::legend (9.0f).withExtraKerningFactor (0.2f));
    g.drawText ("DUCK   dB", juce::Rectangle<float> (face.getX(), face.getBottom() - 20.0f, face.getWidth(), 14.0f), juce::Justification::centred);

    const float a = angleOf (needleDb);
    g.setColour (juce::Colours::black.withAlpha (0.18f));
    g.drawLine ({ pivot.translated (2.0f, 2.0f), polar (pivot, R + 8.0f, a).translated (2.0f, 2.0f) }, 1.6f);
    g.setColour (juce::Colour (0xff111413));
    g.drawLine ({ pivot, polar (pivot, R + 8.0f, a) }, 1.4f);

    g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.35f), face.getX(), face.getY(), juce::Colours::white.withAlpha (0.0f), face.getX(), face.getCentreY(), false));
    g.fillRect (face.withHeight (face.getHeight() * 0.45f));
    g.restoreState();
    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.drawRect (face, 1.0f);
}

//==============================================================================
KubbeEditor::KubbeEditor (KubbeProcessor& p)
    : AudioProcessorEditor (&p), proc (p), crt (p), licenseOverlay (p.license)
{
    setLookAndFeel (&lnf);
    mark = juce::ImageCache::getFromMemory (BinaryData::mark_cream_png, BinaryData::mark_cream_pngSize);
    wordmark = juce::ImageCache::getFromMemory (BinaryData::wordmark_cream_png, BinaryData::wordmark_cream_pngSize);

    addAndMakeVisible (crt);
    addAndMakeVisible (vfd);
    addAndMakeVisible (meter);

    for (int i = 0; i < svrb::kNumCharacters; ++i)
    {
        auto b = std::make_unique<PlaceButton> (i, proc);
        b->onClick = [this, i]
        {
            if (auto* param = proc.apvts.getParameter ("place"))
                param->setValueNotifyingHost (param->convertTo0to1 ((float) i));
        };
        addAndMakeVisible (*b);
        places.push_back (std::move (b));
    }

    const std::pair<const char*, const char*> ids[] {
        { "mix", "MIX" }, { "decay", "DECAY" }, { "predelay", "PRE-DELAY" }, { "predelaySync", "SYNC" },
        { "distance", "DISTANCE" }, { "width", "WIDTH" }, { "lowCut", "LOW CUT" }, { "highCut", "HIGH CUT" }, { "duck", "DUCK" }
    };
    for (auto [id, name] : ids)
    {
        auto k = std::make_unique<Knob>();
        k->id = id;
        k->name = name;
        k->stepped = juce::String (id) == "predelaySync";
        k->slider.getProperties().set ("stepped", k->stepped);
        k->slider.setSliderStyle (juce::Slider::RotaryVerticalDrag);
        k->slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        k->slider.setRotaryParameters (kStart, kEnd, true);
        k->attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, id, k->slider);
        addAndMakeVisible (k->slider);
        lastValues.push_back ((float) k->slider.getValue());
        knobs.push_back (std::move (k));
    }

    addAndMakeVisible (freezeSwitch);
    addAndMakeVisible (echoSyncSwitch);
    freezeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "freeze", freezeSwitch);
    echoSyncAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "echoSync", echoSyncSwitch);

    setSize (kW, kH);
    crt.refresh();
    timerCallback();
    // Licence: the card covers the panel until the plugin is activated; LICENSE reopens it.
    licenseButton.setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    licenseButton.setColour (juce::TextButton::textColourOffId, Theme::ink.withAlpha (0.6f));
    licenseButton.onClick = [this] { licenseOverlay.refresh(); licenseOverlay.setVisible (true); licenseOverlay.toFront (true); };
    addAndMakeVisible (licenseButton);
    addChildComponent (licenseOverlay);
    licenseOverlay.setVisible (! proc.license.isLicensed());
    startTimerHz (30);
}

KubbeEditor::~KubbeEditor()
{
    setLookAndFeel (nullptr);
}

void KubbeEditor::buildPanelTexture()
{
    // Brushed, anodized: fine horizontal grain over a soft top light.
    panelTexture = juce::Image (juce::Image::ARGB, kW, kH, true);
    juce::Graphics g (panelTexture);
    g.setGradientFill (juce::ColourGradient (Theme::panel.brighter (0.12f), 0.0f, 0.0f, Theme::panelDeep, 0.0f, (float) kH, false));
    g.fillAll();

    juce::Random rng (1979);
    for (int y = 0; y < kH; ++y)
    {
        const float v = rng.nextFloat();
        g.setColour ((v > 0.5f ? juce::Colours::white : juce::Colours::black).withAlpha (0.018f + 0.03f * std::abs (v - 0.5f)));
        g.fillRect (0, y, kW, 1);
    }
    for (int i = 0; i < 2600; ++i)
    {
        const float x = rng.nextFloat() * kW, len = 30.0f + rng.nextFloat() * 260.0f;
        const int y = rng.nextInt (kH);
        g.setColour ((rng.nextBool() ? juce::Colours::white : juce::Colours::black).withAlpha (0.025f + rng.nextFloat() * 0.03f));
        g.drawHorizontalLine (y, x, x + len);
    }
}

void KubbeEditor::paintScale (juce::Graphics& g, const juce::Slider& s, bool stepped) const
{
    const auto c = s.getBounds().toFloat().getCentre();
    const float r = (float) s.getWidth() * 0.5f;
    g.setColour (Theme::ink.withAlpha (s.isEnabled() ? 0.85f : 0.35f));

    if (stepped)
    {
        g.setFont (Theme::legend (8.5f));
        const int n = KubbeProcessor::noteNames.size();
        for (int i = 0; i < n; ++i)
        {
            const float a = kStart + (kEnd - kStart) * (float) i / (float) (n - 1);
            g.drawLine ({ polar (c, r + 1.0f, a), polar (c, r + 5.0f, a) }, 1.2f);
            const auto p = polar (c, r + 15.0f, a);
            g.drawText (KubbeProcessor::noteNames[i].toUpperCase(), juce::Rectangle<float> (p.x - 16.0f, p.y - 6.0f, 32.0f, 12.0f), juce::Justification::centred);
        }
        return;
    }

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

void KubbeEditor::paint (juce::Graphics& g)
{
    if (panelTexture.isNull())
        buildPanelTexture();
    g.drawImageAt (panelTexture, 0, 0);

    const float W = (float) kW, H = (float) kH;

    // Rack ears: a fold line, and two slots with their cap screws.
    for (float ex : { 0.0f, W - (float) kEar })
    {
        const float fold = ex == 0.0f ? (float) kEar : ex;
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRect (fold - 1.0f, 0.0f, 2.0f, H);
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.fillRect (fold + 1.0f, 0.0f, 1.0f, H);
        for (float sy : { 30.0f, H - 64.0f })
        {
            const juce::Rectangle<float> slot (ex + 11.0f, sy, 14.0f, 34.0f);
            g.setColour (juce::Colour (0xff010d0c));
            g.fillRoundedRectangle (slot, 7.0f);
            g.setColour (juce::Colours::white.withAlpha (0.1f));
            g.drawRoundedRectangle (slot.translated (0.0f, 1.0f), 7.0f, 1.0f);
            const auto sc = slot.getCentre().translated (0.0f, 4.0f);
            g.setColour (juce::Colours::black.withAlpha (0.5f));
            g.fillEllipse (sc.x - 8.0f + 1.5f, sc.y - 8.0f + 2.0f, 16.0f, 16.0f);
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xffEFEFEA), sc.x - 6.0f, sc.y - 6.0f, juce::Colour (0xff6C7270), sc.x + 6.0f, sc.y + 6.0f, false));
            g.fillEllipse (sc.x - 8.0f, sc.y - 8.0f, 16.0f, 16.0f);
            g.setColour (juce::Colour (0xff3a3f3d));
            g.drawLine (sc.x - 4.5f, sc.y - 1.5f, sc.x + 4.5f, sc.y + 1.5f, 1.8f);
            g.drawLine (sc.x - 1.5f, sc.y + 4.5f, sc.x + 1.5f, sc.y - 4.5f, 1.8f);
        }
    }
    // Panel bevel.
    g.setColour (juce::Colours::white.withAlpha (0.14f));
    g.fillRect (0.0f, 0.0f, W, 1.0f);
    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.fillRect (0.0f, H - 2.0f, W, 2.0f);

    // Name block: the product owns the top left; the maker's badge sits at the foot.
    const float nameW = drawProductName (g, "KUBBE", 62.0f, 34.0f, 32.0f);
    g.setColour (Theme::ink);
    tracked (g, "DIGITAL REVERBERATION SYSTEM", Theme::legend (8.5f), { 62.0f, 80.0f, nameW, 12.0f }, juce::Justification::centredLeft, 0.26f);
    const juce::Rectangle<float> plate (62.0f, 97.0f, 86.0f, 17.0f);
    g.setColour (Theme::gold);
    g.drawRect (plate, 1.0f);
    tracked (g, "MODEL KB-79", Theme::legend (9.0f), plate, juce::Justification::centred, 0.18f);
    g.setColour (Theme::ink.withAlpha (0.7f));
    tracked (g, "3 SPACES", Theme::legend (8.0f), { 62.0f + nameW - 80.0f, 97.0f, 80.0f, 17.0f }, juce::Justification::centredRight, 0.24f);

    // Section legends: a label with bracket lines, as silkscreened on consoles.
    auto bracket = [&] (float x1, float x2, float y, const juce::String& text)
    {
        const auto f = Theme::legend (9.5f).withExtraKerningFactor (0.24f);
        const float tw = juce::GlyphArrangement::getStringWidth (f, text) + 14.0f;
        const float mid = (x1 + x2) * 0.5f;
        g.setColour (Theme::ink.withAlpha (0.75f));
        g.drawLine (x1, y, mid - tw * 0.5f, y, 1.0f);
        g.drawLine (mid + tw * 0.5f, y, x2, y, 1.0f);
        g.drawLine (x1, y, x1, y + 5.0f, 1.0f);
        g.drawLine (x2, y, x2, y + 5.0f, 1.0f);
        g.setColour (Theme::ink);
        tracked (g, text, Theme::legend (9.5f), { mid - tw * 0.5f, y - 7.0f, tw, 14.0f }, juce::Justification::centred, 0.24f);
    };
    bracket ((float) places.front()->getX() + 8.0f, (float) places.back()->getRight() - 8.0f, 128.0f, "PLACE");

    // Engraved rule between the display row and the controls.
    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.fillRect ((float) kEar + 14.0f, 250.0f, W - 2.0f * kEar - 28.0f, 1.0f);
    g.setColour (juce::Colours::white.withAlpha (0.10f));
    g.fillRect ((float) kEar + 14.0f, 251.0f, W - 2.0f * kEar - 28.0f, 1.0f);

    const std::tuple<int, int, const char*> groups[] { { 0, 0, "LEVEL" }, { 1, 3, "TIME" }, { 4, 5, "SPACE" }, { 6, 7, "TONE" }, { 8, 8, "DYNAMICS" } };
    for (auto [a, z, name] : groups)
        bracket ((float) knobs[(size_t) a]->slider.getX() - 18.0f, (float) knobs[(size_t) z]->slider.getRight() + 18.0f, 272.0f, name);

    for (auto& k : knobs)
    {
        paintScale (g, k->slider, k->stepped);
        g.setColour (Theme::ink.withAlpha (k->slider.isEnabled() ? 1.0f : 0.4f));
        const float below = k->stepped ? 32.0f : 22.0f;
        tracked (g, k->name, Theme::legend (10.5f),
                 juce::Rectangle<float> ((float) k->slider.getX() - 24.0f, (float) k->slider.getBottom() + below, (float) k->slider.getWidth() + 48.0f, 14.0f),
                 juce::Justification::centred, 0.2f);
    }

    // Switch legends.
    for (auto* s : { &freezeSwitch, &echoSyncSwitch })
    {
        const auto r = s->getBounds().toFloat();
        g.setColour (Theme::ink);
        tracked (g, s == &freezeSwitch ? "FREEZE" : "ECHO SYNC", Theme::legend (9.0f), { r.getX() - 16.0f, r.getBottom() + 2.0f, r.getWidth() + 32.0f, 13.0f },
                 juce::Justification::centred, 0.16f);
        g.setColour (Theme::ink.withAlpha (0.55f));
        g.setFont (Theme::fine (7.0f));
        const float cy = r.getY() + r.getHeight() * 0.6f;
        g.drawText ("ON", juce::Rectangle<float> (r.getRight() - 8.0f, cy - 38.0f, 20.0f, 10.0f), juce::Justification::left);
        g.drawText ("OFF", juce::Rectangle<float> (r.getRight() - 8.0f, cy + 28.0f, 20.0f, 10.0f), juce::Justification::left);
    }
    g.setColour (Theme::ink.withAlpha (0.45f));
    g.setFont (Theme::fine (7.0f));
    g.drawText ("VALLEY ONLY", echoSyncSwitch.getBounds().toFloat().translated (0.0f, 16.0f).withHeight (10.0f).withY ((float) echoSyncSwitch.getBottom() + 15.0f)
                    .expanded (14.0f, 0.0f), juce::Justification::centred);

    // Foot of the panel.
    g.setColour (Theme::ink.withAlpha (0.55f));
    tracked (g, "MEASURED, NOT GUESSED", Theme::fine (8.0f), { (float) kEar + 20.0f, H - 22.0f, 300.0f, 12.0f }, juce::Justification::centredLeft, 0.2f);
    // Maker's badge: mark and wordmark, the way a manufacturer signed the foot of a panel.
    const float badgeRight = W - (float) kEar - 20.0f;
    g.setOpacity (0.8f);
    g.drawImage (wordmark, juce::Rectangle<float> (badgeRight - 104.0f, H - 23.0f, 104.0f, 13.0f), juce::RectanglePlacement::xRight | juce::RectanglePlacement::yMid);
    g.drawImage (mark, juce::Rectangle<float> (badgeRight - 104.0f - 32.0f, H - 28.0f, 26.0f, 20.0f), juce::RectanglePlacement::centred);
    g.setOpacity (1.0f);
}

void KubbeEditor::resized()
{
    licenseOverlay.setBounds (getLocalBounds());
    licenseButton.setBounds (getWidth() / 2 + 170, kH - 26, 70, 20);
    for (int i = 0; i < (int) places.size(); ++i)
        places[(size_t) i]->setBounds (52 + i * 90, 138, 96, 80);

    crt.setBounds (340, 22, 430, 214);
    vfd.setBounds (792, 24, 272, 88);
    meter.setBounds (792, 126, 168, 108);
    freezeSwitch.setBounds (968, 116, 44, 100);
    echoSyncSwitch.setBounds (1022, 116, 44, 100);

    // Knob row: groups with a little air between them.
    const int gaps[] { 0, 1, 0, 0, 1, 0, 1, 0, 1 };
    const int knobSize = 70, stepSize = 48, cell = 102, groupGap = 24;
    int x = 62;
    for (size_t i = 0; i < knobs.size(); ++i)
    {
        x += gaps[i] * groupGap;
        const int size = knobs[i]->stepped ? stepSize : knobSize;
        knobs[i]->slider.setBounds (x + (cell - size) / 2, 336 - size / 2, size, size);
        x += cell;
    }
}

std::pair<juce::String, juce::String> KubbeEditor::readout (int i) const
{
    const auto& k = *knobs[(size_t) i];
    if (k.id == "decay")
    {
        const float t = crt.measuredT60();
        return { juce::String (t, t < 10.0f ? 2 : 1), "DECAY  T60 SEC  MEASURED" };
    }
    if (k.id == "predelay" || k.id == "predelaySync")
    {
        const int sync = (int) proc.apvts.getRawParameterValue ("predelaySync")->load();
        if (sync > 0)
            return { KubbeProcessor::noteNames[sync], "PRE-DELAY  NOTE" };
        return { juce::String (juce::roundToInt (proc.apvts.getRawParameterValue ("predelay")->load())), "PRE-DELAY  MS" };
    }
    const auto text = proc.apvts.getParameter (k.id)->getCurrentValueAsText();
    return { text.upToFirstOccurrenceOf (" ", false, false), k.name + "  " + text.fromFirstOccurrenceOf (" ", false, false).toUpperCase() };
}

void KubbeEditor::timerCallback()
{
    if (! proc.license.isLicensed() && ! licenseOverlay.isVisible())
    {
        licenseOverlay.refresh();
        licenseOverlay.setVisible (true);
    }
    const int place = (int) proc.apvts.getRawParameterValue ("place")->load();
    const bool synced = (int) proc.apvts.getRawParameterValue ("predelaySync")->load() > 0;
    if (echoSyncSwitch.isEnabled() != (place == 2) || knobs[2]->slider.isEnabled() == synced)
    {
        echoSyncSwitch.setEnabled (place == 2);
        knobs[2]->slider.setEnabled (! synced);
        repaint();
    }

    // The last control that moved takes the VFD for two seconds.
    const double now = juce::Time::getMillisecondCounterHiRes();
    for (size_t i = 0; i < knobs.size(); ++i)
    {
        const float v = (float) knobs[i]->slider.getValue();
        if (v != lastValues[i])
        {
            lastValues[i] = v;
            touched = (int) i;
            touchedAt = now;
        }
    }
    crt.refresh();

    const bool frozen = proc.apvts.getRawParameterValue ("freeze")->load() > 0.5f;
    if (touched >= 0 && now - touchedAt < 2000.0)
    {
        const auto [digits, caption] = readout (touched);
        vfd.show (digits, caption);
    }
    else if (frozen)
        vfd.show ("----", "FREEZE  HOLD");
    else
        vfd.show (juce::String (crt.measuredT60(), 2), "T60 SEC  " + KubbeProcessor::placeNames[place].toUpperCase() + "  MEASURED");

    meter.setDb (juce::Decibels::gainToDecibels (1.0f - proc.uiDuck.load(), -24.0f));

    if (place != lastPlace)
    {
        lastPlace = place;
        for (auto& b : places) b->repaint();
        crt.repaint();
    }
}
