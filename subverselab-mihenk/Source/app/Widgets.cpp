#include "Widgets.h"
#include "Theme.h"
#include "../dsp/Spectrum.h"
#include "../dsp/Equalizer.h"
#include "../plugin/PluginProcessor.h"

namespace mihenk::ui
{
bool Context::isApp() const { return processor.isStandaloneApp(); }

juce::Point<float> polar (juce::Point<float> c, float r, float angle)
{
    return { c.x + std::sin (angle) * r, c.y - std::cos (angle) * r };
}

namespace
{
juce::Font fitted (juce::Font f, const juce::String& s, float width, float minScale = 0.75f)
{
    const float h = f.getHeight();
    for (float scale = 1.0f; scale >= minScale; scale -= 0.05f)
    {
        auto t = f.withHeight (h * scale);
        if (juce::GlyphArrangement::getStringWidth (t, s) <= width) return t;
    }
    return f.withHeight (h * minScale);
}
} // namespace

void tracked (juce::Graphics& g, const juce::String& s, juce::Font f, juce::Rectangle<float> r, juce::Justification j, float tracking)
{
    auto font = fitted (f.withExtraKerningFactor (tracking), s, r.getWidth());
    g.setFont (font);
    g.drawText (s, r, j, false);
}

void glow (juce::Graphics& g, juce::Point<float> c, float radius, juce::Colour col, float alpha)
{
    g.setGradientFill (juce::ColourGradient (col.withAlpha (alpha), c.x, c.y, col.withAlpha (0.0f), c.x + radius, c.y, true));
    g.fillEllipse (c.x - radius, c.y - radius, radius * 2.0f, radius * 2.0f);
}

void vfdText (juce::Graphics& g, const juce::String& s, juce::Rectangle<float> r, juce::Justification j, float size, juce::Colour c)
{
    const auto font = fitted (Theme::mono (size), s, r.getWidth(), 0.8f);
    g.setFont (font);
    g.setColour (c.withAlpha (c.getFloatAlpha() * 0.22f));
    g.drawText (s, r.translated (0.0f, 0.6f).expanded (0.6f, 0.0f), j, true);
    g.setColour (c);
    g.drawText (s, r, j, true);
}

void vfdGlass (juce::Graphics& g, juce::Rectangle<float> b)
{
    g.setColour (juce::Colour (0xff010605));
    g.fillRoundedRectangle (b, 6.0f);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff041A18), b.getCentreX(), b.getY(), juce::Colour (0xff010807), b.getCentreX(), b.getBottom(), false));
    g.fillRoundedRectangle (b.reduced (4.0f), 4.0f);
}

void vfdGlassTop (juce::Graphics& g, juce::Rectangle<float> b)
{
    g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.06f), b.getX(), b.getY(), juce::Colours::white.withAlpha (0.0f), b.getX(), b.getCentreY(), false));
    g.fillRoundedRectangle (b.reduced (4.0f).withHeight (b.getHeight() * 0.45f), 4.0f);
}

juce::Rectangle<float> smallScreen (juce::Graphics& g, juce::Rectangle<float> b)
{
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1d2624), b.getX(), b.getY(), juce::Colour (0xff040706), b.getX(), b.getBottom(), false));
    g.fillRoundedRectangle (b, 6.0f);
    g.setColour (juce::Colours::white.withAlpha (0.10f));
    g.drawRoundedRectangle (b.reduced (0.5f), 6.0f, 1.0f);
    const auto screen = b.reduced (5.0f);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff06302A), screen.getCentreX(), screen.getCentreY(), Theme::screen, screen.getX(), screen.getY(), true));
    g.fillRoundedRectangle (screen, 7.0f);
    return screen;
}

void screenGlass (juce::Graphics& g, juce::Rectangle<float> screen)
{
    g.saveState();
    juce::Path tube;
    tube.addRoundedRectangle (screen, 7.0f);
    g.reduceClipRegion (tube);
    g.setColour (juce::Colours::black.withAlpha (0.18f));
    for (float y = screen.getY(); y < screen.getBottom(); y += 3.0f) g.fillRect (screen.getX(), y, screen.getWidth(), 1.0f);
    g.setGradientFill (juce::ColourGradient (juce::Colours::transparentBlack, screen.getCentreX(), screen.getCentreY(),
                                             juce::Colours::black.withAlpha (0.45f), screen.getX() - 6.0f, screen.getY() - 6.0f, true));
    g.fillRect (screen);
    g.restoreState();
    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.drawRoundedRectangle (screen, 7.0f, 1.2f);
}

void phosphorPath (juce::Graphics& g, const juce::Path& p, juce::Colour c, float width)
{
    g.setColour (c.withAlpha (c.getFloatAlpha() * 0.12f));
    g.strokePath (p, juce::PathStrokeType (width + 4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (c.withAlpha (c.getFloatAlpha() * 0.3f));
    g.strokePath (p, juce::PathStrokeType (width + 1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (c);
    g.strokePath (p, juce::PathStrokeType (width));
}

juce::String timeText (double secs)
{
    if (secs < 0.0 || ! std::isfinite (secs)) secs = 0.0;
    const int m = (int) (secs / 60.0);
    return juce::String (m) + ":" + juce::String (secs - m * 60.0, 1).paddedLeft ('0', 4);
}

namespace
{
float letterWidth (juce::juce_wchar ch, float h)
{
    switch (ch)
    {
        case 'M': return 1.56f * h;
        case 'I': return 0.19f * h;
        default:  return 1.34f * h;
    }
}

juce::String dbText (double v, int dec = 1) { return std::isfinite (v) ? juce::String (v, dec) : juce::String ("-INF"); }
} // namespace

float productNameWidth (const juce::String& name, float h)
{
    float w = 0.0f;
    for (auto ch : name) w += letterWidth (ch, h);
    return w + 0.30f * h * (float) juce::jmax (0, name.length() - 1);
}

float drawProductName (juce::Graphics& g, const juce::String& name, float x, float y, float h, juce::Colour ink)
{
    const float t = 0.19f * h, r = 0.30f * h, gap = 0.30f * h, in = t * 0.5f;
    juce::Path all;
    float cx = x;
    for (auto ch : name)
    {
        const float w = letterWidth (ch, h);
        juce::Path p;
        const float L = cx + in, R = cx + w - in, T = y + in, B = y + h - in, M = y + h * 0.5f, C = (L + R) * 0.5f;
        switch (ch)
        {
            case 'M':
                p.startNewSubPath (L, B + in); p.lineTo (L, T); p.lineTo (C, M + h * 0.14f); p.lineTo (R, T); p.lineTo (R, B + in);
                break;
            case 'I':
                p.startNewSubPath (L, T - in); p.lineTo (L, B + in);
                break;
            case 'H':
                p.startNewSubPath (L, T - in); p.lineTo (L, B + in);
                p.startNewSubPath (R, T - in); p.lineTo (R, B + in);
                p.startNewSubPath (L, M); p.lineTo (R, M);
                break;
            case 'E':
                p.startNewSubPath (R, T); p.lineTo (L + r, T); p.quadraticTo (L, T, L, T + r);
                p.lineTo (L, B - r); p.quadraticTo (L, B, L + r, B); p.lineTo (R, B);
                p.startNewSubPath (L, M); p.lineTo (L + (R - L) * 0.82f, M);
                break;
            case 'N':
                p.startNewSubPath (L, B + in); p.lineTo (L, T); p.lineTo (R, B); p.lineTo (R, T - in);
                break;
            case 'K':
                p.startNewSubPath (L, T - in); p.lineTo (L, B + in);
                p.startNewSubPath (L, M + t * 0.2f); p.lineTo (R, T - in);
                p.startNewSubPath (L + (R - L) * 0.36f, M - h * 0.12f); p.lineTo (R, B + in);
                break;
            default: break;
        }
        all.addPath (p);
        cx += w + gap;
    }
    const juce::PathStrokeType stroke (t, juce::PathStrokeType::beveled, juce::PathStrokeType::butt);
    juce::Path outline;
    stroke.createStrokedPath (outline, all);
    g.saveState();
    g.reduceClipRegion (juce::Rectangle<float> (x - t, y, cx - x + t, h).toNearestInt());
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillPath (outline, juce::AffineTransform::translation (0.0f, 1.2f));
    g.setColour (ink);
    g.fillPath (outline);
    g.restoreState();
    return cx - gap - x;
}

//==============================================================================
MihenkLookAndFeel::MihenkLookAndFeel()
{
    setColour (juce::BubbleComponent::backgroundColourId, juce::Colour (0xff021512));
    setColour (juce::BubbleComponent::outlineColourId, Theme::gold);
    setColour (juce::TooltipWindow::backgroundColourId, juce::Colour (0xff021512));
    setColour (juce::TooltipWindow::textColourId, Theme::vfd);
    setColour (juce::TooltipWindow::outlineColourId, Theme::gold);
    setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff021512));
    setColour (juce::PopupMenu::textColourId, Theme::vfd);
    setColour (juce::PopupMenu::headerTextColourId, Theme::gold);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, Theme::teal.withAlpha (0.5f));
    setColour (juce::PopupMenu::highlightedTextColourId, Theme::ink);
}

juce::Font MihenkLookAndFeel::getSliderPopupFont (juce::Slider&) { return Theme::mono (12.0f); }

void MihenkLookAndFeel::fillTextEditorBackground (juce::Graphics& g, int w, int h, juce::TextEditor& e)
{
    g.setColour (e.findColour (juce::TextEditor::backgroundColourId));
    g.fillRect (0, 0, w, h);
}

void MihenkLookAndFeel::drawTextEditorOutline (juce::Graphics& g, int w, int h, juce::TextEditor& e)
{
    g.setColour (e.findColour (juce::TextEditor::outlineColourId));
    g.drawRect (0, 0, w, h, 1);
}

namespace
{
juce::TextLayout tooltipLayout (const juce::String& text, juce::Colour c)
{
    juce::AttributedString a;
    a.setJustification (juce::Justification::centredLeft);
    a.append (text, Theme::mono (10.0f), c);
    juce::TextLayout t;
    t.createLayoutWithBalancedLineLengths (a, 330.0f);
    return t;
}
} // namespace

juce::Rectangle<int> MihenkLookAndFeel::getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos, juce::Rectangle<int> parentArea)
{
    const auto t = tooltipLayout (tipText, Theme::vfd);
    const int w = (int) std::ceil (t.getWidth()) + 18, h = (int) std::ceil (t.getHeight()) + 12;
    return juce::Rectangle<int> (screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 12,
                                 screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 6) : screenPos.y + 6, w, h)
        .constrainedWithin (parentArea);
}

void MihenkLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height)
{
    const auto b = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height);
    g.setColour (findColour (juce::TooltipWindow::backgroundColourId));
    g.fillRoundedRectangle (b, 4.0f);
    g.setColour (findColour (juce::TooltipWindow::outlineColourId).withAlpha (0.8f));
    g.drawRoundedRectangle (b.reduced (0.5f), 4.0f, 1.0f);
    tooltipLayout (text, findColour (juce::TooltipWindow::textColourId)).draw (g, b.reduced (9.0f, 6.0f));
}

//==============================================================================
void KnobSlider::mouseDown (const juce::MouseEvent& e)
{
    if (! isEnabled()) return;
    if (e.getNumberOfClicks() >= 2 && isDoubleClickReturnEnabled())
    {
        setValue (getDoubleClickReturnValue(), juce::sendNotificationSync);
        return;
    }
    dragging = true;
    lastY = e.position.y;
    dragProportion = valueToProportionOfLength (getValue());
    if (onDragStart) onDragStart();
}

void KnobSlider::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging) return;
    const double dy = (double) (lastY - e.position.y);
    lastY = e.position.y;
    const double px = pixelsOverride > 0.0 ? pixelsOverride : pixelsForFullRange * (e.mods.isCommandDown() ? fineFactor : 1.0);
    dragProportion = juce::jlimit (0.0, 1.0, dragProportion + dy / px);
    setValue (proportionOfLengthToValue (dragProportion), juce::sendNotificationSync);
}

void KnobSlider::mouseUp (const juce::MouseEvent&)
{
    if (dragging && onDragEnd) onDragEnd();
    dragging = false;
}

void MihenkLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                          float startAngle, float endAngle, juce::Slider& s)
{
    const float d = (float) juce::jmin (w, h) - 4.0f, r = d * 0.5f;
    const juce::Point<float> c ((float) x + (float) w * 0.5f, (float) y + (float) h * 0.5f);
    const float angle = startAngle + pos * (endAngle - startAngle);

    g.setColour (juce::Colours::black.withAlpha (0.45f));
    g.fillEllipse (c.x - r + 1.5f, c.y - r + 3.0f, d, d);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff2b302e), c.x, c.y - r, juce::Colour (0xff0b0d0c), c.x, c.y + r, false));
    g.fillEllipse (c.x - r, c.y - r, d, d);
    for (int i = 0; i < 48; ++i)
    {
        const float a = juce::MathConstants<float>::twoPi * (float) i / 48.0f;
        const float light = 0.5f + 0.5f * std::cos (a + 0.8f);
        g.setColour (juce::Colours::white.withAlpha (0.05f + 0.13f * light));
        g.drawLine ({ polar (c, r * 0.84f, a), polar (c, r - 0.8f, a) }, 1.0f);
    }
    const float cr = r * 0.72f;
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xffDCDDD8), c.x - cr, c.y - cr, juce::Colour (0xff6E7471), c.x + cr, c.y + cr, false));
    g.fillEllipse (c.x - cr, c.y - cr, cr * 2.0f, cr * 2.0f);
    for (float rr = cr * 0.15f; rr < cr; rr += 1.6f)
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
    g.drawLine ({ polar (c, cr * 0.25f, angle), polar (c, r * 0.97f, angle) }, 3.4f);
    g.setColour (Theme::gold);
    g.drawLine ({ polar (c, cr * 0.25f, angle), polar (c, r * 0.97f, angle) }, 2.0f);

    if (! s.isEnabled() || (bool) s.getProperties()["dim"])
    {
        g.setColour (Theme::panelDeep.withAlpha (0.55f));
        g.fillEllipse (c.x - r, c.y - r, d, d);
    }
}

//==============================================================================
void KeyButton::paintButton (juce::Graphics& g, bool over, bool down)
{
    const auto b = getLocalBounds().toFloat();
    bool on = getToggleState();
    if (blink && on) on = (juce::Time::getMillisecondCounter() / 400) % 2 == 0;
    const juce::Point<float> led (b.getCentreX(), 6.0f);
    if (on) glow (g, led, 12.0f, ledColour, 0.55f);
    g.setColour (on ? ledColour : juce::Colour (0xff3a2e15));
    g.fillEllipse (led.x - 3.5f, led.y - 3.5f, 7.0f, 7.0f);

    const auto key = b.withTrimmedTop (15.0f);
    const float press = down ? 2.0f : (getToggleState() && getClickingTogglesState() ? 1.5f : 0.0f);
    g.setColour (juce::Colour (0xff020d0c));
    g.fillRoundedRectangle (key, 3.0f);
    const auto face = key.reduced (2.0f).withTrimmedBottom (5.0f - press).translated (0.0f, press);
    const float dim = isEnabled() ? 0.0f : 0.25f;
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xffE9EAE5).brighter (over ? 0.04f : 0.0f).darker (dim), face.getX(), face.getY(),
                                             juce::Colour (0xffA2A7A4).darker (dim), face.getX(), face.getBottom(), false));
    g.fillRoundedRectangle (face, 2.0f);
    for (float y = face.getY() + 2.0f; y < face.getBottom() - 2.0f; y += 2.0f)
    {
        g.setColour (juce::Colours::black.withAlpha (0.03f));
        g.drawHorizontalLine ((int) y, face.getX() + 2.0f, face.getRight() - 2.0f);
    }
    g.setColour (juce::Colour (0xff5B605D));
    g.fillRect (face.getX(), face.getBottom(), face.getWidth(), 5.0f - press);
    g.setColour (Theme::panel.withAlpha (isEnabled() ? 1.0f : 0.5f));
    auto lines = juce::StringArray::fromTokens (getButtonText(), "\n", "");
    const float lh = 10.0f;
    auto area = face.withSizeKeepingCentre (face.getWidth() - 4.0f, lh * (float) lines.size());
    for (auto& l : lines)
        tracked (g, l, Theme::legend (8.0f), area.removeFromTop (lh), juce::Justification::centred, 0.14f);
}

void CapButton::paintButton (juce::Graphics& g, bool over, bool down)
{
    const auto b = getLocalBounds().toFloat().reduced (1.0f);
    const bool on = getToggleState();
    g.setColour (juce::Colour (0xff020d0c));
    g.fillRoundedRectangle (b, 3.0f);
    const auto cap = b.reduced (1.5f).translated (0.0f, down ? 1.0f : 0.0f);
    const float dim = isEnabled() ? 0.0f : 0.3f;
    g.setGradientFill (juce::ColourGradient (Theme::capCream.brighter (over && isEnabled() ? 0.06f : 0.0f).darker (dim), cap.getX(), cap.getY(),
                                             juce::Colour (0xffC4BBA3).darker (dim), cap.getX(), cap.getBottom(), false));
    g.fillRoundedRectangle (cap, 2.0f);
    if (! showLed)
    {
        g.setColour (Theme::panel.withAlpha (isEnabled() ? 1.0f : 0.45f));
        tracked (g, getButtonText(), Theme::legend (7.5f), cap.reduced (4.0f, 0.0f), juce::Justification::centred, 0.12f);
        return;
    }
    const auto win = juce::Rectangle<float> (cap.getX() + 5.0f, cap.getCentreY() - 3.0f, 9.0f, 6.0f);
    g.setColour (juce::Colour (0xff1a1408));
    g.fillRoundedRectangle (win.expanded (1.5f), 2.0f);
    if (on)
    {
        g.setGradientFill (juce::ColourGradient (ledColour.brighter (0.6f), win.getCentreX(), win.getY(), ledColour.darker (0.25f), win.getCentreX(), win.getBottom(), false));
        g.fillRoundedRectangle (win, 1.5f);
        g.setColour (ledColour.withAlpha (0.35f));
        g.drawRoundedRectangle (win.expanded (2.5f), 3.0f, 1.5f);
    }
    else
    {
        g.setColour (juce::Colour (0xff3a2c12));
        g.fillRoundedRectangle (win, 1.5f);
    }
    g.setColour (Theme::panel.withAlpha (isEnabled() ? 1.0f : 0.45f));
    tracked (g, getButtonText(), Theme::legend (7.5f), cap.withTrimmedLeft (16.0f).withTrimmedRight (3.0f), juce::Justification::centred, 0.12f);
}

//==============================================================================
// CRT

juce::Rectangle<float> CrtScope::plotArea() const
{
    return getLocalBounds().toFloat().reduced (12.0f).reduced (26.0f, 0.0f).withTrimmedTop (30.0f).withTrimmedBottom (22.0f);
}

juce::Rectangle<float> CrtScope::overviewArea() const
{
    const auto p = plotArea();
    return p.withTrimmedTop (p.getHeight() * 0.80f);
}

juce::int64 CrtScope::sampleAt (float x, juce::Rectangle<float> area) const
{
    const auto len = ctx.session.player.getLength();
    return (juce::int64) (juce::jlimit (0.0f, 1.0f, (x - area.getX()) / area.getWidth()) * (float) len);
}

void CrtScope::mouseDown (const juce::MouseEvent& e)
{
    dragStartX = -1.0f;
    if (mode != Mode::wave || ctx.session.input == nullptr || ! ctx.isApp()) return;
    dragInOverview = isLive() && overviewArea().expanded (0.0f, 4.0f).contains (e.position);
    if (isLive() && ! dragInOverview) return;
    dragStartX = e.position.x;
    dragging = false;
}

void CrtScope::mouseDrag (const juce::MouseEvent& e)
{
    if (dragStartX < 0.0f) return;
    const auto area = dragInOverview ? overviewArea() : plotArea();
    if (std::abs (e.position.x - dragStartX) > 4.0f)
    {
        dragging = true;
        ctx.session.player.setLoopRegion (sampleAt (dragStartX, area), sampleAt (e.position.x, area));
        ctx.session.player.setLoop (true);
        repaint();
    }
}

void CrtScope::mouseUp (const juce::MouseEvent& e)
{
    if (dragStartX < 0.0f) return;
    const auto area = dragInOverview ? overviewArea() : plotArea();
    if (! dragging) ctx.session.player.setPosition (sampleAt (e.position.x, area));
    else ctx.session.player.setPosition (ctx.session.player.getLoopStart());
    dragStartX = -1.0f;
    dragging = false;
    repaint();
}

void CrtScope::buildWave (int width)
{
    const juce::String key = juce::String::toHexString ((juce::pointer_sized_int) ctx.session.input.get()) + juce::String::toHexString ((juce::pointer_sized_int) ctx.session.output.get())
                             + juce::String (width);
    if (key == waveKey) return;
    waveKey = key;
    auto fill = [width] (const juce::AudioBuffer<float>* b, std::vector<float>& mn, std::vector<float>& mx)
    {
        mn.assign ((size_t) width, 0.0f);
        mx.assign ((size_t) width, 0.0f);
        if (b == nullptr || b->getNumSamples() == 0) return;
        const int n = b->getNumSamples();
        for (int col = 0; col < width; ++col)
        {
            const int a = (int) ((juce::int64) n * col / width), z = juce::jmax (a + 1, (int) ((juce::int64) n * (col + 1) / width));
            float lo = 0.0f, hi = 0.0f;
            for (int c = 0; c < b->getNumChannels(); ++c)
            {
                auto r = juce::FloatVectorOperations::findMinAndMax (b->getReadPointer (c, a), juce::jmin (z, n) - a);
                lo = juce::jmin (lo, r.getStart());
                hi = juce::jmax (hi, r.getEnd());
            }
            mn[(size_t) col] = lo;
            mx[(size_t) col] = hi;
        }
    };
    fill (ctx.session.input.get(), inMin, inMax);
    fill (ctx.session.output.get(), outMin, outMax);
}

void CrtScope::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1d2624), b.getX(), b.getY(), juce::Colour (0xff040706), b.getX(), b.getBottom(), false));
    g.fillRoundedRectangle (b, 12.0f);
    g.setColour (juce::Colours::white.withAlpha (0.10f));
    g.drawRoundedRectangle (b.reduced (0.5f), 12.0f, 1.0f);

    const auto screen = b.reduced (12.0f);
    juce::Path tube;
    tube.addRoundedRectangle (screen, 20.0f);
    g.saveState();
    g.reduceClipRegion (tube);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff06302A), screen.getCentreX(), screen.getCentreY(), Theme::screen, screen.getX(), screen.getY(), true));
    g.fillRect (screen);

    const auto plot = plotArea();
    g.setColour (Theme::phosphor.withAlpha (0.08f));
    for (int i = 0; i <= 10; ++i) g.drawVerticalLine ((int) (plot.getX() + plot.getWidth() * (float) i / 10.0f), plot.getY(), plot.getBottom());
    for (int i = 0; i <= 8; ++i) g.drawHorizontalLine ((int) (plot.getY() + plot.getHeight() * (float) i / 8.0f), plot.getX(), plot.getRight());

    const bool live = isLive();
    auto& s = ctx.session;
    if (! s.hasAnalysis() && ! live)
    {
        g.setColour (Theme::phosphor.withAlpha (0.8f));
        tracked (g, ctx.isApp() ? "DROP A WAV OR AIFF HERE" : "PRESS LEARN, THEN PLAY THE SONG",
                 Theme::mono (16.0f), plot.withTrimmedBottom (plot.getHeight() * 0.5f), juce::Justification::centredBottom, 0.1f);
        g.setColour (Theme::phosphor.withAlpha (0.5f));
        tracked (g, ctx.isApp() ? "OR PRESS OPEN.  44.1 / 48 / 96 KHZ, MONO OR STEREO" : "THE VIEWS FOLLOW THE AUDIO WHILE THE HOST PLAYS; LEARN MEASURES THE WHOLE SONG",
                 Theme::mono (10.0f), plot.withTrimmedTop (plot.getHeight() * 0.55f), juce::Justification::centredTop, 0.1f);
    }
    else if (mode == Mode::wave) { if (live) paintWaveLive (g, plot); else paintWave (g, plot); }
    else if (mode == Mode::spectrum) { if (live) paintSpectrumLive (g, plot); else paintSpectrum (g, plot); }
    else { if (live) paintLoudnessLive (g, plot); else paintLoudness (g, plot); }

    g.setColour (juce::Colours::black.withAlpha (0.20f));
    for (float y = screen.getY(); y < screen.getBottom(); y += 3.0f) g.fillRect (screen.getX(), y, screen.getWidth(), 1.0f);
    g.setGradientFill (juce::ColourGradient (juce::Colours::transparentBlack, screen.getCentreX(), screen.getCentreY(),
                                             juce::Colours::black.withAlpha (0.5f), screen.getX() - 10.0f, screen.getY() - 10.0f, true));
    g.fillRect (screen);
    g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.03f), screen.getX() + 40.0f, screen.getY() + 10.0f,
                                             juce::Colours::white.withAlpha (0.0f), screen.getX() + 40.0f, screen.getY() + screen.getHeight() * 0.45f, false));
    g.fillEllipse (screen.getX() + 10.0f, screen.getY() - 30.0f, screen.getWidth() * 0.7f, screen.getHeight() * 0.6f);
    g.restoreState();
    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.drawRoundedRectangle (screen, 20.0f, 1.5f);
}

namespace
{
void header (juce::Graphics& g, juce::Rectangle<float> plot, const juce::String& left, const juce::String& right)
{
    const auto head = juce::Rectangle<float> (plot.getX(), plot.getY() - 24.0f, plot.getWidth(), 14.0f);
    g.setColour (Theme::phosphor.withAlpha (0.85f));
    const float rw = right.isEmpty() ? 0.0f : juce::jmin (head.getWidth() * 0.42f, juce::GlyphArrangement::getStringWidth (Theme::mono (10.0f), right) + 8.0f);
    g.setFont (fitted (Theme::mono (10.0f), left, head.getWidth() - rw - 10.0f, 0.7f));
    g.drawText (left, head.withTrimmedRight (rw + 10.0f), juce::Justification::left);
    if (right.isNotEmpty())
    {
        g.setFont (fitted (Theme::mono (10.0f), right, rw, 0.7f));
        g.drawText (right, head.withTrimmedLeft (head.getWidth() - rw), juce::Justification::right);
    }
}

/** A coloured legend chip in a CRT header: "■ EQ". Returns the width used. */
float legendChip (juce::Graphics& g, float x, float y, const juce::String& t, juce::Colour c, bool on)
{
    g.setColour (c.withAlpha (on ? 1.0f : 0.25f));
    g.fillRect (x, y + 3.0f, 8.0f, 8.0f);
    g.setFont (Theme::mono (10.0f));
    const float w = juce::GlyphArrangement::getStringWidth (Theme::mono (10.0f), t);
    g.setColour (Theme::phosphor.withAlpha (on ? 0.85f : 0.3f));
    g.drawText (t, juce::Rectangle<float> (x + 11.0f, y, w + 2.0f, 14.0f), juce::Justification::left);
    return 11.0f + w + 12.0f;
}

const juce::Colour layerColour[4] { Theme::phosphor.withAlpha (0.42f), Theme::teal.brighter (0.25f), Theme::lamp, Theme::phosphor };
} // namespace

void CrtScope::paintWave (juce::Graphics& g, juce::Rectangle<float> plot)
{
    auto& s = ctx.session;
    if (s.input == nullptr)
    {
        g.setColour (Theme::phosphor.withAlpha (0.6f));
        tracked (g, "WHOLE-TRACK WAVE: PLAY TO SEE IT LIVE, OR LEARN AGAIN", Theme::mono (11.0f), plot, juce::Justification::centred, 0.1f);
        header (g, plot, "WAVE", {});
        return;
    }
    const int width = (int) plot.getWidth();
    buildWave (width);
    const float cy = plot.getCentreY(), half = plot.getHeight() * 0.5f * 0.92f;
    auto yOf = [&] (float v) { return cy - juce::jlimit (-1.1f, 1.1f, v) * half; };
    const double rate = s.sampleRate > 0.0 ? s.sampleRate : 48000.0;
    const juce::int64 len = s.input->getNumSamples();
    auto xOfSample = [&] (juce::int64 x) { return plot.getX() + (float) x / (float) juce::jmax ((juce::int64) 1, len) * plot.getWidth(); };
    auto& p = s.player;
    if (ctx.isApp() && p.isLooping() && p.getLoopEnd() > p.getLoopStart())
    {
        const float a = xOfSample (p.getLoopStart()), z = xOfSample (p.getLoopEnd());
        g.setColour (Theme::gold.withAlpha (0.10f));
        g.fillRect (a, plot.getY(), z - a, plot.getHeight());
        g.setColour (Theme::gold.withAlpha (0.6f));
        g.drawVerticalLine ((int) a, plot.getY(), plot.getBottom());
        g.drawVerticalLine ((int) z, plot.getY(), plot.getBottom());
    }
    const bool showA = p.isListeningOriginal() || ctx.processor.isListeningOriginal();
    auto drawColumns = [&] (const std::vector<float>& mn, const std::vector<float>& mx, juce::Colour c, float alpha)
    {
        g.setColour (c.withAlpha (alpha));
        for (int x = 0; x < width && x < (int) mn.size(); ++x)
            g.drawVerticalLine ((int) plot.getX() + x, yOf (mx[(size_t) x]), yOf (mn[(size_t) x]) + 1.0f);
    };
    if (ctx.layers[0]) drawColumns (showA ? outMin : inMin, showA ? outMax : inMax, Theme::phosphor, 0.18f);
    if (ctx.layers[3]) drawColumns (showA ? inMin : outMin, showA ? inMax : outMax, Theme::phosphor, 0.75f);

    const float ceil = (float) dbToGain (s.settings.limiter.ceilingDbtp);
    g.setColour (Theme::gold.withAlpha (0.55f));
    for (float x = plot.getX(); x < plot.getRight(); x += 8.0f)
    {
        g.drawLine (x, yOf (ceil), juce::jmin (x + 4.0f, plot.getRight()), yOf (ceil), 1.0f);
        g.drawLine (x, yOf (-ceil), juce::jmin (x + 4.0f, plot.getRight()), yOf (-ceil), 1.0f);
    }
    auto grTrace = [&] (const std::vector<float>& h, juce::Colour c)
    {
        if (h.empty()) return;
        juce::Path path;
        const float scale = plot.getHeight() * 0.25f / 12.0f;
        for (int x = 0; x < width; ++x)
        {
            const size_t a = (size_t) ((double) h.size() * x / width), z = juce::jmax (a + 1, (size_t) ((double) h.size() * (x + 1) / width));
            float m = 0.0f;
            for (size_t i = a; i < z && i < h.size(); ++i) m = juce::jmax (m, h[i]);
            const float y = plot.getY() + juce::jmin (12.0f, m) * scale;
            if (x == 0) path.startNewSubPath (plot.getX(), y); else path.lineTo (plot.getX() + (float) x, y);
        }
        phosphorPath (g, path, c, 1.0f);
    };
    if (! s.settings.bypass)
    {
        if (s.settings.comp.enabled) grTrace (s.compGr.history, Theme::teal);
        if (s.settings.limiter.enabled) grTrace (s.limGr.history, Theme::gold);
    }
    if (ctx.isApp())
    {
        const float px = xOfSample (p.getPosition());
        g.setColour (Theme::lamp.withAlpha (0.3f));
        g.fillRect (px - 2.0f, plot.getY(), 4.0f, plot.getHeight());
        g.setColour (Theme::lamp);
        g.drawVerticalLine ((int) px, plot.getY(), plot.getBottom());
    }
    g.setColour (Theme::phosphor.withAlpha (0.55f));
    g.setFont (Theme::mono (9.0f));
    const double secs = (double) len / rate;
    const double step = secs > 240 ? 30.0 : secs > 90 ? 15.0 : secs > 30 ? 5.0 : secs > 8 ? 2.0 : 0.5;
    for (double t = 0.0; t <= secs; t += step)
        g.drawText (timeText (t), juce::Rectangle<float> (plot.getX() + (float) (t / secs) * plot.getWidth() - 20.0f, plot.getBottom() + 4.0f, 40.0f, 12.0f),
                    juce::Justification::centred);
    juce::String left = showA ? "WHOLE TRACK  A ORIGINAL  " + fmt (gainToDb (p.getOriginalGain()), 1) + " DB LEVEL-MATCHED" : juce::String ("WHOLE TRACK  IN (DIM)  OUT");
    if (! s.settings.bypass) left << "   GR: " << (s.settings.comp.enabled ? "COMP (TEAL) " : "") << (s.settings.limiter.enabled ? "LIM (GOLD)" : "");
    header (g, plot, left, ctx.isApp() ? timeText ((double) p.getPosition() / rate) + " / " + timeText (secs)
                                       + (p.isLooping() ? "  LOOP " + timeText ((double) p.getLoopStart() / rate) + "-" + timeText ((double) p.getLoopEnd() / rate) : juce::String())
                                       : timeText (secs) + " LEARNED");
}

void CrtScope::paintOverview (juce::Graphics& g, juce::Rectangle<float> strip)
{
    auto& s = ctx.session;
    const int width = (int) strip.getWidth();
    buildWave (width);
    g.setColour (Theme::phosphor.withAlpha (0.10f));
    g.drawRect (strip);
    const float cy = strip.getCentreY(), half = strip.getHeight() * 0.46f;
    g.setColour (Theme::phosphor.withAlpha (0.45f));
    for (int x = 0; x < width && x < (int) outMin.size(); ++x)
        g.drawVerticalLine ((int) strip.getX() + x, cy - juce::jlimit (-1.0f, 1.0f, outMax[(size_t) x]) * half, cy - juce::jlimit (-1.0f, 1.0f, outMin[(size_t) x]) * half + 1.0f);
    auto& p = s.player;
    const auto len = juce::jmax ((juce::int64) 1, p.getLength());
    if (p.isLooping() && p.getLoopEnd() > p.getLoopStart())
    {
        const float a = strip.getX() + (float) p.getLoopStart() / (float) len * strip.getWidth(), z = strip.getX() + (float) p.getLoopEnd() / (float) len * strip.getWidth();
        g.setColour (Theme::gold.withAlpha (0.14f));
        g.fillRect (a, strip.getY(), z - a, strip.getHeight());
    }
    const float px = strip.getX() + (float) p.getPosition() / (float) len * strip.getWidth();
    g.setColour (Theme::lamp);
    g.drawVerticalLine ((int) px, strip.getY(), strip.getBottom());
}

void CrtScope::paintWaveLive (juce::Graphics& g, juce::Rectangle<float> plot)
{
    auto& s = ctx.session;
    auto& L = ctx.live;
    const bool overview = ctx.isApp() && s.output != nullptr;
    auto area = plot;
    if (overview) area = plot.withHeight (plot.getHeight() * 0.76f);
    const auto grStrip = area.withTrimmedTop (area.getHeight() * 0.74f);
    const auto waveArea = area.withHeight (area.getHeight() * 0.62f);
    const auto timeRow = juce::Rectangle<float> (area.getX(), waveArea.getBottom() + 1.0f, area.getWidth(), grStrip.getY() - waveArea.getBottom() - 2.0f);
    const int visible = 600;                                  // 6 s of 10 ms columns
    const int count = juce::jmin (L.columnCount(), visible);
    const float colW = waveArea.getWidth() / (float) visible;
    const float cy = waveArea.getCentreY(), half = waveArea.getHeight() * 0.5f * 0.94f;
    auto yOf = [&] (float v) { return cy - juce::jlimit (-1.05f, 1.05f, v) * half; };
    const float ceil = (float) dbToGain (s.settings.limiter.ceilingDbtp);
    g.setColour (Theme::gold.withAlpha (0.5f));
    for (float x = waveArea.getX(); x < waveArea.getRight(); x += 8.0f)
    {
        g.drawLine (x, yOf (ceil), juce::jmin (x + 4.0f, waveArea.getRight()), yOf (ceil), 1.0f);
        g.drawLine (x, yOf (-ceil), juce::jmin (x + 4.0f, waveArea.getRight()), yOf (-ceil), 1.0f);
    }
    // Waveform: newest column at the right edge.
    for (int layer : { 0, 3 })
    {
        if (! ctx.layers[(size_t) layer]) continue;
        g.setColour (Theme::phosphor.withAlpha (layer == 0 ? 0.20f : 0.78f));
        for (int i = 0; i < count; ++i)
        {
            const auto& c = L.column (L.columnCount() - count + i);
            const float x = waveArea.getRight() - (float) (count - i) * colW;
            const float lo = layer == 0 ? c.inMin : c.outMin, hi = layer == 0 ? c.inMax : c.outMax;
            g.fillRect (x, yOf (hi), juce::jmax (1.0f, colW), juce::jmax (1.0f, yOf (lo) - yOf (hi)));
        }
    }
    // Gain reduction band under the wave: the compressor (teal) and the limiter (gold), 0..12 dB, hanging down.
    g.setColour (Theme::phosphor.withAlpha (0.15f));
    g.drawHorizontalLine ((int) grStrip.getY(), grStrip.getX(), grStrip.getRight());
    const float grScale = grStrip.getHeight() / 12.0f;
    for (int kind = 0; kind < 2; ++kind)
    {
        juce::Path path;
        path.startNewSubPath (grStrip.getRight() - (float) count * colW, grStrip.getY());
        for (int i = 0; i < count; ++i)
        {
            const auto& c = L.column (L.columnCount() - count + i);
            const float v = kind == 0 ? c.comp : c.lim;
            path.lineTo (grStrip.getRight() - (float) (count - i) * colW, grStrip.getY() + juce::jmin (12.0f, v) * grScale);
        }
        path.lineTo (grStrip.getRight(), grStrip.getY());
        path.closeSubPath();
        const auto col = kind == 0 ? Theme::teal : Theme::gold;
        g.setColour (col.withAlpha (0.35f));
        g.fillPath (path);
        g.setColour (col);
        g.strokePath (path, juce::PathStrokeType (1.0f));
    }
    g.setColour (Theme::phosphor.withAlpha (0.5f));
    g.setFont (Theme::mono (8.5f));
    for (int db : { 6, 12 })
        g.drawText (juce::String (db), juce::Rectangle<float> (grStrip.getX() - 26.0f, grStrip.getY() + (float) db * grScale - (db == 12 ? 11.0f : 6.0f), 22.0f, 12.0f), juce::Justification::right);
    // The playhead is "now", at the right edge.
    g.setColour (Theme::lamp);
    g.drawVerticalLine ((int) waveArea.getRight() - 1, waveArea.getY(), grStrip.getBottom());
    g.setColour (Theme::phosphor.withAlpha (0.55f));
    g.setFont (Theme::mono (9.0f));
    for (int sec = 0; sec <= 6; ++sec)
    {
        const float x = waveArea.getRight() - (float) sec * 100.0f * colW;
        const auto r = juce::Rectangle<float> (x - 22.0f, timeRow.getCentreY() - 6.0f, 44.0f, 12.0f).constrainedWithin (timeRow);
        g.drawText (sec == 0 ? juce::String ("NOW") : "-" + juce::String (sec) + " S", r, sec == 0 ? juce::Justification::centredRight : juce::Justification::centred);
    }
    g.setColour (Theme::phosphor.withAlpha (0.4f));
    g.drawText ("GR", juce::Rectangle<float> (grStrip.getX() - 26.0f, grStrip.getY() - 13.0f, 22.0f, 12.0f), juce::Justification::right);
    if (overview) paintOverview (g, overviewArea());
    auto& p = s.player;
    const double rate = s.sampleRate > 0.0 ? s.sampleRate : 48000.0;
    header (g, plot, "WAVE LIVE  " + juce::String (ctx.layers[0] ? "IN (DIM)  " : "") + (ctx.layers[3] ? "OUT  " : "") + "GR: COMP (TEAL) LIM (GOLD) 0-12 DB",
            ctx.isApp() ? timeText ((double) p.getPosition() / rate) + " / " + timeText ((double) p.getLength() / rate) : juce::String ("HOST PLAYING"));
}

void CrtScope::paintSpectrum (juce::Graphics& g, juce::Rectangle<float> plot)
{
    auto& s = ctx.session;
    const auto& grid = spectrumGrid();
    auto main = plot.withHeight (plot.getHeight() * 0.64f);
    auto diff = plot.withTrimmedTop (plot.getHeight() * 0.70f);
    auto xOf = [&] (double f) { return plot.getX() + (float) (std::log2 (f / 20.0) / std::log2 (20480.0 / 20.0)) * plot.getWidth(); };
    g.setFont (Theme::mono (9.0f));
    for (double f : { 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })
    {
        g.setColour (Theme::phosphor.withAlpha (0.14f));
        g.drawVerticalLine ((int) xOf (f), plot.getY(), plot.getBottom());
        g.setColour (Theme::phosphor.withAlpha (0.55f));
        g.drawText (f >= 1000.0 ? juce::String ((int) (f / 1000.0)) + "K" : juce::String ((int) f),
                    juce::Rectangle<float> (xOf (f) - 16.0f, plot.getBottom() + 4.0f, 32.0f, 12.0f), juce::Justification::centred);
    }
    auto curve = [&] (const std::vector<float>& db, juce::Rectangle<float> area, float range, bool align)
    {
        juce::Path p;
        if (db.size() != grid.size()) return p;
        const float off = align ? (float) meanInRange (db, 60.0, 12000.0) : 0.0f;
        for (size_t i = 0; i < grid.size(); ++i)
        {
            const float v = juce::jlimit (-range, range, db[i] - off);
            const juce::Point<float> pt (xOf (grid[i]), area.getCentreY() - v / range * area.getHeight() * 0.5f);
            if (i == 0) p.startNewSubPath (pt); else p.lineTo (pt);
        }
        return p;
    };
    g.setColour (Theme::phosphor.withAlpha (0.2f));
    g.drawHorizontalLine ((int) main.getCentreY(), plot.getX(), plot.getRight());
    if (ctx.layers[0]) phosphorPath (g, curve (s.inMeas.spectrumDb, main, 24.0f, true), Theme::phosphor.withAlpha (0.35f), 1.0f);
    if (s.reference != nullptr) phosphorPath (g, curve (s.refMeas.spectrumDb, main, 24.0f, true), Theme::gold, 1.2f);
    if (ctx.layers[3]) phosphorPath (g, curve (s.outMeas.spectrumDb, main, 24.0f, true), Theme::phosphor, 1.3f);

    g.setColour (Theme::phosphor.withAlpha (0.25f));
    g.drawHorizontalLine ((int) diff.getCentreY(), plot.getX(), plot.getRight());
    g.setColour (Theme::phosphor.withAlpha (0.5f));
    g.setFont (Theme::mono (8.5f));
    g.drawText ("+12", diff.withWidth (30.0f).translated (-30.0f, 0.0f), juce::Justification::topRight);
    g.drawText ("-12", diff.withWidth (30.0f).translated (-30.0f, 0.0f), juce::Justification::bottomRight);
    std::vector<float> eqDb (grid.size());
    for (size_t i = 0; i < grid.size(); ++i) eqDb[i] = s.settings.eq.enabled && ! s.settings.bypass ? (float) eqTotalDb (s.settings.eq, grid[i]) : 0.0f;
    juce::Path eqPath = curve (eqDb, diff, 12.0f, false), dashed;
    const float dashes[] { 5.0f, 4.0f };
    juce::PathStrokeType (1.4f).createDashedStroke (dashed, eqPath, dashes, 2);
    g.setColour (Theme::lamp.withAlpha (0.9f));
    g.fillPath (dashed);
    // The reference difference is drawn once, in the REFERENCE module.
    header (g, plot, "WHOLE TRACK, 1/6 OCT, LEVEL-ALIGNED   IN (DIM)  OUT" + juce::String (s.reference != nullptr ? "  REF (GOLD)" : ""),
            "LOWER: EQ CURVE (DASHED)");
}

void CrtScope::paintSpectrumLive (juce::Graphics& g, juce::Rectangle<float> plot)
{
    auto& s = ctx.session;
    auto& L = ctx.live;
    const auto& grid = spectrumGrid();
    auto main = plot.withHeight (plot.getHeight() * 0.66f);
    auto diff = plot.withTrimmedTop (plot.getHeight() * 0.72f);
    auto xOf = [&] (double f) { return plot.getX() + (float) (std::log2 (f / 20.0) / std::log2 (20480.0 / 20.0)) * plot.getWidth(); };
    constexpr float top = -12.0f, bottom = -96.0f;
    auto yOf = [&] (float db) { return main.getY() + juce::jlimit (0.0f, 1.0f, (top - db) / (top - bottom)) * main.getHeight(); };
    g.setFont (Theme::mono (9.0f));
    for (double f : { 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })
    {
        g.setColour (Theme::phosphor.withAlpha (0.14f));
        g.drawVerticalLine ((int) xOf (f), plot.getY(), plot.getBottom());
        g.setColour (Theme::phosphor.withAlpha (0.55f));
        g.drawText (f >= 1000.0 ? juce::String ((int) (f / 1000.0)) + "K" : juce::String ((int) f),
                    juce::Rectangle<float> (xOf (f) - 16.0f, plot.getBottom() + 4.0f, 32.0f, 12.0f), juce::Justification::centred);
    }
    g.setFont (Theme::mono (8.5f));
    for (float db = -24.0f; db >= -84.0f; db -= 24.0f)
    {
        g.setColour (Theme::phosphor.withAlpha (0.12f));
        g.drawHorizontalLine ((int) yOf (db), plot.getX(), plot.getRight());
        g.setColour (Theme::phosphor.withAlpha (0.5f));
        g.drawText (juce::String ((int) db), juce::Rectangle<float> (plot.getX() - 30.0f, yOf (db) - 6.0f, 26.0f, 12.0f), juce::Justification::right);
    }
    auto absCurve = [&] (const std::vector<float>& db)
    {
        juce::Path p;
        if (db.size() != grid.size()) return p;
        for (size_t i = 0; i < grid.size(); ++i)
        {
            const juce::Point<float> pt (xOf (grid[i]), yOf (db[i]));
            if (i == 0) p.startNewSubPath (pt); else p.lineTo (pt);
        }
        return p;
    };
    // Whole-track averages as faint references.
    if (ctx.layers[0] && s.inMeas.spectrumDb.size() == grid.size())
    {
        g.setColour (Theme::phosphor.withAlpha (0.16f));
        g.strokePath (absCurve (s.inMeas.spectrumDb), juce::PathStrokeType (1.0f));
    }
    if (ctx.layers[3] && s.outMeas.spectrumDb.size() == grid.size())
    {
        g.setColour (Theme::phosphor.withAlpha (0.26f));
        g.strokePath (absCurve (s.outMeas.spectrumDb), juce::PathStrokeType (1.0f));
    }
    if (L.spectrumValid)
        for (int l = 0; l < LiveAnalysis::layers; ++l)
            if (ctx.layers[(size_t) l])
                phosphorPath (g, absCurve (L.spectrum[l]), layerColour[l], l == 3 ? 1.4f : 1.1f);
    // Lower strip: what the chain does right now (OUT - IN, level-aligned) and the static EQ.
    g.setColour (Theme::phosphor.withAlpha (0.25f));
    g.drawHorizontalLine ((int) diff.getCentreY(), plot.getX(), plot.getRight());
    g.setColour (Theme::phosphor.withAlpha (0.5f));
    g.drawText ("+12", diff.withWidth (30.0f).translated (-30.0f, 0.0f), juce::Justification::topRight);
    g.drawText ("-12", diff.withWidth (30.0f).translated (-30.0f, 0.0f), juce::Justification::bottomRight);
    auto relCurve = [&] (const std::vector<float>& db)
    {
        juce::Path p;
        for (size_t i = 0; i < grid.size() && i < db.size(); ++i)
        {
            const float v = juce::jlimit (-12.0f, 12.0f, db[i]);
            const juce::Point<float> pt (xOf (grid[i]), diff.getCentreY() - v / 12.0f * diff.getHeight() * 0.5f);
            if (i == 0) p.startNewSubPath (pt); else p.lineTo (pt);
        }
        return p;
    };
    std::vector<float> eqDb (grid.size());
    for (size_t i = 0; i < grid.size(); ++i) eqDb[i] = s.settings.eq.enabled && ! s.settings.bypass ? (float) eqTotalDb (s.settings.eq, grid[i]) : 0.0f;
    juce::Path dashed;
    const float dashes[] { 5.0f, 4.0f };
    juce::PathStrokeType (1.3f).createDashedStroke (dashed, relCurve (eqDb), dashes, 2);
    g.setColour (Theme::lamp.withAlpha (0.8f));
    g.fillPath (dashed);
    if (L.spectrumValid)
    {
        std::vector<float> d (grid.size());
        for (size_t i = 0; i < grid.size(); ++i) d[i] = L.spectrum[3][i] - L.spectrum[0][i];
        const float off = (float) meanInRange (d, 60.0, 12000.0);
        for (auto& v : d) v -= off;
        phosphorPath (g, relCurve (d), Theme::coral, 1.1f);
    }
    // Header: coloured layer legend.
    const auto head = juce::Rectangle<float> (plot.getX(), plot.getY() - 24.0f, plot.getWidth(), 14.0f);
    g.setColour (Theme::phosphor.withAlpha (0.85f));
    g.setFont (Theme::mono (10.0f));
    g.drawText ("LIVE 1/6 OCT", head, juce::Justification::left);
    float x = head.getX() + 92.0f;
    const char* names[] { "IN", "EQ", "COMP", "OUT" };
    for (int l = 0; l < 4; ++l) x += legendChip (g, x, head.getY(), names[l], layerColour[l].withAlpha (1.0f), ctx.layers[(size_t) l]);
    g.setColour (Theme::phosphor.withAlpha (0.6f));
    g.setFont (fitted (Theme::mono (9.0f), "FAINT: WHOLE TRACK   LOWER: OUT-IN NOW, EQ DASHED", head.getRight() - x - 4.0f, 0.7f));
    g.drawText ("FAINT: WHOLE TRACK   LOWER: OUT-IN NOW, EQ DASHED", juce::Rectangle<float> (x, head.getY(), head.getRight() - x, 14.0f), juce::Justification::right);
}

void CrtScope::paintLoudness (juce::Graphics& g, juce::Rectangle<float> plot)
{
    auto& s = ctx.session;
    const float top = 0.0f, bottom = -40.0f;
    auto yOf = [&] (float l) { return plot.getY() + juce::jlimit (0.0f, 1.0f, (top - l) / (top - bottom)) * plot.getHeight(); };
    g.setFont (Theme::mono (9.0f));
    for (float l = 0.0f; l >= bottom; l -= 5.0f)
    {
        g.setColour (Theme::phosphor.withAlpha (0.5f));
        g.drawText (juce::String ((int) l), juce::Rectangle<float> (plot.getX() - 28.0f, yOf (l) - 6.0f, 24.0f, 12.0f), juce::Justification::right);
    }
    auto hist = [&] (const std::vector<float>& h, juce::Colour c, float w)
    {
        if (h.empty()) return;
        juce::Path p;
        const double total = (double) h.size() + 29.0;
        for (size_t i = 0; i < h.size(); ++i)
        {
            const float x = plot.getX() + (float) (((double) i + 30.0) / total) * plot.getWidth();
            if (i == 0) p.startNewSubPath (x, yOf (h[i])); else p.lineTo (x, yOf (h[i]));
        }
        phosphorPath (g, p, c, w);
    };
    if (ctx.layers[0]) hist (s.inMeas.shortTermHistory, Theme::phosphor.withAlpha (0.35f), 1.0f);
    if (ctx.layers[3]) hist (s.outMeas.shortTermHistory, Theme::phosphor, 1.3f);
    const float yt = yOf ((float) s.settings.targetLufs);
    g.setColour (Theme::gold.withAlpha (0.8f));
    for (float x = plot.getX(); x < plot.getRight(); x += 8.0f) g.drawLine (x, yt, juce::jmin (x + 4.0f, plot.getRight()), yt, 1.0f);
    g.drawText ("TARGET " + fmt (s.settings.targetLufs), juce::Rectangle<float> (plot.getRight() - 110.0f, yt - 14.0f, 110.0f, 12.0f), juce::Justification::right);
    if (std::isfinite (s.outMeas.integrated))
    {
        const float yi = yOf ((float) s.outMeas.integrated);
        g.setColour (Theme::phosphor.withAlpha (0.8f));
        g.drawHorizontalLine ((int) yi, plot.getX(), plot.getX() + 40.0f);
        g.drawText ("I " + fmt (s.outMeas.integrated), juce::Rectangle<float> (plot.getX() + 44.0f, yi - 6.0f, 90.0f, 12.0f), juce::Justification::left);
    }
    header (g, plot, "WHOLE TRACK SHORT-TERM (3 S), LUFS   IN (DIM)  OUT", "LRA IN " + fmt (s.inMeas.lra) + "  OUT " + fmt (s.outMeas.lra) + " LU");
}

void CrtScope::paintLoudnessLive (juce::Graphics& g, juce::Rectangle<float> plot)
{
    auto& s = ctx.session;
    auto& L = ctx.live;
    const float top = 0.0f, bottom = -40.0f;
    auto yOf = [&] (float l) { return plot.getY() + juce::jlimit (0.0f, 1.0f, (top - l) / (top - bottom)) * plot.getHeight(); };
    g.setFont (Theme::mono (9.0f));
    for (float l = 0.0f; l >= bottom; l -= 5.0f)
    {
        g.setColour (Theme::phosphor.withAlpha (0.5f));
        g.drawText (juce::String ((int) l), juce::Rectangle<float> (plot.getX() - 28.0f, yOf (l) - 6.0f, 24.0f, 12.0f), juce::Justification::right);
    }
    const int visible = 300;                                  // 30 s of 100 ms steps
    const int count = juce::jmin (L.stepCount(), visible);
    const float dx = plot.getWidth() / (float) visible;
    auto trace = [&] (int which, juce::Colour c, float w)
    {
        juce::Path p;
        bool started = false;
        for (int i = 0; i < count; ++i)
        {
            const int k = L.stepCount() - count + i;
            const float v = which == 0 ? L.stepMomentary (k) : which == 1 ? L.stepShortTerm (k) : L.stepInShortTerm (k);
            if (v <= -119.0f) { started = false; continue; }
            const float x = plot.getRight() - (float) (count - i) * dx;
            if (! started) { p.startNewSubPath (x, yOf (v)); started = true; } else p.lineTo (x, yOf (v));
        }
        phosphorPath (g, p, c, w);
    };
    if (ctx.layers[0]) trace (2, Theme::phosphor.withAlpha (0.35f), 1.0f);
    if (ctx.layers[3])
    {
        trace (0, Theme::teal.brighter (0.2f).withAlpha (0.8f), 0.9f);
        trace (1, Theme::phosphor, 1.4f);
    }
    const float yt = yOf ((float) s.settings.targetLufs);
    g.setColour (Theme::gold.withAlpha (0.8f));
    for (float x = plot.getX(); x < plot.getRight(); x += 8.0f) g.drawLine (x, yt, juce::jmin (x + 4.0f, plot.getRight()), yt, 1.0f);
    g.setFont (Theme::mono (9.0f));
    g.drawText ("TARGET " + fmt (s.settings.targetLufs), juce::Rectangle<float> (plot.getX() + 4.0f, yt - 14.0f, 110.0f, 12.0f), juce::Justification::left);
    if (std::isfinite (L.integrated))
    {
        const float yi = yOf ((float) L.integrated);
        g.setColour (Theme::lamp.withAlpha (0.85f));
        for (float x = plot.getX(); x < plot.getRight(); x += 3.0f) g.fillRect (x, yi, 1.0f, 1.0f);
    }
    // Readout box, top right.
    auto box = juce::Rectangle<float> (plot.getRight() - 150.0f, plot.getY() + 4.0f, 146.0f, 44.0f);
    g.setColour (Theme::screen.withAlpha (0.85f));
    g.fillRect (box);
    g.setColour (Theme::phosphor.withAlpha (0.3f));
    g.drawRect (box);
    g.setFont (Theme::mono (10.0f));
    g.setColour (Theme::teal.brighter (0.3f));
    g.drawText ("M  " + (L.momentary > -119.0f ? fmt (L.momentary) : juce::String ("--")), box.removeFromTop (14.0f).reduced (6.0f, 0.0f), juce::Justification::left);
    g.setColour (Theme::phosphor);
    g.drawText ("S  " + (L.shortTerm > -119.0f ? fmt (L.shortTerm) : juce::String ("--")), box.removeFromTop (14.0f).reduced (6.0f, 0.0f), juce::Justification::left);
    g.setColour (Theme::lamp);
    g.drawText ("I  " + (std::isfinite (L.integrated) ? fmt (L.integrated) : juce::String ("--")) + " SO FAR", box.removeFromTop (14.0f).reduced (6.0f, 0.0f), juce::Justification::left);
    g.setColour (Theme::phosphor.withAlpha (0.55f));
    g.setFont (Theme::mono (9.0f));
    for (int sec = 0; sec <= 30; sec += 5)
        g.drawText (sec == 0 ? juce::String ("NOW") : "-" + juce::String (sec) + " S",
                    juce::Rectangle<float> (plot.getRight() - (float) sec * 10.0f * dx - 22.0f, plot.getBottom() + 4.0f, 44.0f, 12.0f), juce::Justification::centred);
    header (g, plot, "LOUDNESS LIVE, LUFS   M 400 MS (TEAL)  S 3 S (BRIGHT)" + juce::String (ctx.layers[0] ? "  IN S (DIM)" : ""), "I = INTEGRATED SINCE PLAY");
}

//==============================================================================
void VfdTable::paint (juce::Graphics& g)
{
    auto& session = ctx.session;
    const auto b = getLocalBounds().toFloat();
    vfdGlass (g, b);
    const auto area = b.reduced (12.0f, 10.0f);
    const bool hasRef = session.reference != nullptr;
    const float labelW = area.getWidth() * 0.40f, colW = (area.getWidth() - labelW) / 3.0f;
    const float rowH = (area.getHeight() - 18.0f) / 9.0f;
    auto col = [&] (int c) { return juce::Rectangle<float> (area.getX() + labelW + colW * (float) c, area.getY(), colW, 14.0f); };
    vfdText (g, ctx.isApp() ? "INPUT" : "LEARNED", col (0), juce::Justification::centredRight, 9.0f, Theme::vfd.withAlpha (0.7f));
    vfdText (g, "OUTPUT", col (1), juce::Justification::centredRight, 9.0f, Theme::vfd.withAlpha (0.7f));
    vfdText (g, "REF", col (2), juce::Justification::centredRight, 9.0f, Theme::vfd.withAlpha (hasRef ? 0.7f : 0.2f));

    struct Row { const char* name; const char* unit; double Measurement::* field; int dec; };
    const Row rows[] {
        { "INTEGRATED", "LUFS", &Measurement::integrated, 1 },
        { "SHORT-TERM MAX", "LUFS", &Measurement::shortTermMax, 1 },
        { "MOMENTARY MAX", "LUFS", &Measurement::momentaryMax, 1 },
        { "LOUDNESS RANGE", "LU", &Measurement::lra, 1 },
        { "TRUE PEAK", "DBTP", &Measurement::truePeak, 2 },
        { "SAMPLE PEAK", "DBFS", &Measurement::samplePeak, 2 },
        { "RMS", "DBFS", &Measurement::rms, 1 },
        { "CREST", "DB", &Measurement::crest, 1 },
        { "CORRELATION", "", &Measurement::correlation, 2 },
    };
    const Measurement* ms[3] { &session.inMeas, &session.outMeas, hasRef ? &session.refMeas : nullptr };
    const bool rendering = session.busy();
    for (int r = 0; r < 9; ++r)
    {
        const float y = area.getY() + 18.0f + rowH * (float) r;
        const auto labelRect = juce::Rectangle<float> (area.getX(), y, labelW, rowH);
        vfdText (g, rows[r].name, labelRect.withTrimmedBottom (rowH * 0.42f), juce::Justification::bottomLeft, 8.5f, Theme::vfd.withAlpha (0.75f));
        vfdText (g, rows[r].unit, labelRect.withTrimmedTop (rowH * 0.55f), juce::Justification::topLeft, 7.0f, Theme::vfd.withAlpha (0.4f));
        for (int c = 0; c < 3; ++c)
        {
            const auto cell = juce::Rectangle<float> (area.getX() + labelW + colW * (float) c, y, colW, rowH);
            if (ms[c] == nullptr || ! ms[c]->valid)
            {
                vfdText (g, "--.-", cell, juce::Justification::centredRight, 12.5f, Theme::vfd.withAlpha (0.12f));
                continue;
            }
            const double v = ms[c]->*(rows[r].field);
            juce::Colour colr = Theme::vfd;
            if (c == 1 && rendering) colr = Theme::vfd.withAlpha (0.45f);
            if (rows[r].field == &Measurement::truePeak && v > session.settings.limiter.ceilingDbtp + 0.005) colr = Theme::coral;
            if (c == 1 && rows[r].field == &Measurement::integrated && std::isfinite (v) && std::abs (v - session.settings.targetLufs) <= 0.2) colr = Theme::lamp;
            vfdText (g, rows[r].field == &Measurement::correlation && ms[c]->channels < 2 ? juce::String ("MONO") : fmt (v, rows[r].dec),
                     cell, juce::Justification::centredRight, 12.5f, colr);
        }
    }
    vfdGlassTop (g, b);
}

//==============================================================================
// OUTPUT meters

float OutputMeters::levelToY (float db, juce::Rectangle<float> bar)
{
    // Piecewise dB law: 0..-12 dB take the top 45 %, -12..-30 the next 30 %, -30..-60 the last 25 %.
    const float d = juce::jlimit (-60.0f, 0.0f, db);
    float pos;
    if (d >= -12.0f) pos = -d / 12.0f * 0.45f;
    else if (d >= -30.0f) pos = 0.45f + (-d - 12.0f) / 18.0f * 0.30f;
    else pos = 0.75f + (-d - 30.0f) / 30.0f * 0.25f;
    return bar.getY() + pos * bar.getHeight();
}

void OutputMeters::mouseDown (const juce::MouseEvent&)
{
    ctx.live.resetMaxHold();
    repaint();
}

void OutputMeters::paint (juce::Graphics& g)
{
    auto& s = ctx.session;
    auto& L = ctx.live;
    const bool live = L.running();
    const auto b = getLocalBounds().toFloat();
    vfdGlass (g, b);
    const auto inner = b.reduced (10.0f, 8.0f);

    vfdText (g, "METERS", inner.withHeight (12.0f), juce::Justification::centred, 9.0f, Theme::vfd.withAlpha (0.6f));
    const float textH = 124.0f;
    auto bars = inner.withTrimmedTop (44.0f).withTrimmedBottom (textH + 10.0f);
    // Columns: IN | scale | L R | M | C LIM
    const float scaleW = 20.0f, barW = 14.5f, gap = 3.0f;
    float x = inner.getX();
    auto take = [&] (float w) { auto r = juce::Rectangle<float> (x, bars.getY(), w, bars.getHeight()); x += w + gap; return r; };
    const auto inBar = take (barW);
    const auto scale = take (scaleW);
    const auto lBar = take (barW), rBar = take (barW);
    x += 2.0f;
    const auto mBar = take (barW);
    x += 2.0f;
    const auto cBar = take (barW), limBar = take (barW);

    const float ceiling = (float) s.settings.limiter.ceilingDbtp;
    auto label = [&] (const juce::Rectangle<float>& bar, const juce::String& t) { vfdText (g, t, juce::Rectangle<float> (bar.getX() - 5.0f, inner.getY() + 15.0f, bar.getWidth() + 10.0f, 11.0f), juce::Justification::centred, 8.0f, Theme::vfd.withAlpha (0.75f)); };
    auto value = [&] (const juce::Rectangle<float>& bar, const juce::String& t, juce::Colour c) { vfdText (g, t, juce::Rectangle<float> (bar.getX() - 5.0f, inner.getY() + 28.0f, bar.getWidth() + 10.0f, 11.0f), juce::Justification::centred, 7.5f, c); };
    label (inBar, "IN"); label (lBar, "L"); label (rBar, "R"); label (mBar, "M"); label (cBar, "C"); label (limBar, "LIM");

    // Scale ticks (dBFS / dBTP), shared by IN, L, R and M (LUFS).
    for (int db : { 0, -1, -3, -6, -9, -12, -18, -24, -30, -40, -50, -60 })
    {
        const float y = levelToY ((float) db, bars);
        g.setColour (Theme::vfd.withAlpha (0.35f));
        g.drawHorizontalLine ((int) y, scale.getX() + 1.0f, scale.getX() + 4.0f);
        g.drawHorizontalLine ((int) y, scale.getRight() - 4.0f, scale.getRight() - 1.0f);
        vfdText (g, db == 0 ? juce::String ("0") : juce::String (-db), juce::Rectangle<float> (scale.getX(), y - 5.0f, scale.getWidth(), 10.0f), juce::Justification::centred, 7.0f,
                 Theme::vfd.withAlpha (db >= -1 ? 0.8f : 0.55f));
    }
    // Segmented level bar with peak hold, max hold and the file's peak marks.
    auto levelBar = [&] (const juce::Rectangle<float>& bar, float level, float hold, float maxv, float fileTp, float fileSp, bool dim)
    {
        const int segs = 60;
        for (int k = 0; k < segs; ++k)
        {
            const float yTop = bar.getY() + bar.getHeight() * (float) k / segs, yBot = bar.getY() + bar.getHeight() * (float) (k + 1) / segs;
            const float segY = (yTop + yBot) * 0.5f;
            // The dB value of this segment (inverse of levelToY by search on the piecewise law).
            float lo = -60.0f, hi = 0.0f;
            for (int it = 0; it < 18; ++it) { const float mid = 0.5f * (lo + hi); if (levelToY (mid, bar) > segY) lo = mid; else hi = mid; }
            const float db = 0.5f * (lo + hi);
            juce::Colour c = dim ? Theme::vfd.withAlpha (0.55f) : Theme::vfd;
            if (db >= ceiling) c = Theme::coral;
            else if (db >= -6.0f) c = Theme::lamp;
            const bool on = level >= db;
            g.setColour (on ? c : c.withAlpha (0.07f));
            g.fillRect (bar.getX(), yTop + 0.5f, bar.getWidth(), juce::jmax (1.0f, yBot - yTop - 1.0f));
        }
        if (hold > -60.0f)
        {
            g.setColour (hold >= ceiling ? Theme::coral : Theme::ink);
            g.fillRect (bar.getX(), levelToY (hold, bar) - 1.0f, bar.getWidth(), 2.0f);
        }
        if (maxv > -60.0f)
        {
            g.setColour ((maxv >= ceiling ? Theme::coral : Theme::lamp).withAlpha (0.9f));
            g.drawHorizontalLine ((int) levelToY (maxv, bar), bar.getX() - 2.0f, bar.getRight() + 2.0f);
        }
        if (std::isfinite (fileTp) && fileTp > -60.0f)
        {
            // The file's true peak: a gold wedge on the left edge; its sample peak: a short tick.
            const float y = levelToY (fileTp, bar);
            juce::Path w;
            w.addTriangle (bar.getX() - 4.0f, y - 3.5f, bar.getX() - 4.0f, y + 3.5f, bar.getX(), y);
            g.setColour (Theme::gold);
            g.fillPath (w);
            if (std::isfinite (fileSp) && fileSp > -60.0f)
                g.drawHorizontalLine ((int) levelToY (fileSp, bar), bar.getRight(), bar.getRight() + 2.0f);
        }
    };
    const float inLevel = live ? juce::jmax (L.in[0].shown, L.in[1].shown) : -120.0f;
    const float inHold = juce::jmax (L.in[0].hold, L.in[1].hold), inMax = juce::jmax (L.in[0].max, L.in[1].max);
    const float inTp = s.inMeas.valid ? (float) s.inMeas.truePeak : kNegInf, inSp = s.inMeas.valid ? (float) s.inMeas.samplePeak : kNegInf;
    const float outTp = s.outMeas.valid ? (float) s.outMeas.truePeak : kNegInf, outSp = s.outMeas.valid ? (float) s.outMeas.samplePeak : kNegInf;
    levelBar (inBar, inLevel, live ? inHold : -120.0f, inMax, inTp, inSp, true);
    levelBar (lBar, live ? L.out[0].shown : -120.0f, live ? L.out[0].hold : -120.0f, L.out[0].max, outTp, outSp, false);
    levelBar (rBar, live ? L.out[1].shown : -120.0f, live ? L.out[1].hold : -120.0f, L.out[1].max, outTp, outSp, false);
    auto shortDb = [] (float v) { return v <= -99.0f ? juce::String ("--") : (v > -10.0f ? juce::String (v, 1) : juce::String (juce::roundToInt (v))); };
    value (inBar, shortDb (live ? inHold : (float) inTp), Theme::vfd.withAlpha (0.7f));
    value (lBar, shortDb (live ? L.out[0].hold : (float) outTp), Theme::vfd);
    value (rBar, shortDb (live ? L.out[1].hold : (float) outTp), Theme::vfd);

    // M: momentary loudness, with short-term (white tick), integrated so far (lamp) and the target (gold).
    {
        const float m = live ? L.momentary : -120.0f;
        const int segs = 60;
        for (int k = 0; k < segs; ++k)
        {
            const float yTop = mBar.getY() + mBar.getHeight() * (float) k / segs, yBot = mBar.getY() + mBar.getHeight() * (float) (k + 1) / segs;
            float lo = -60.0f, hi = 0.0f;
            const float segY = (yTop + yBot) * 0.5f;
            for (int it = 0; it < 18; ++it) { const float mid = 0.5f * (lo + hi); if (levelToY (mid, mBar) > segY) lo = mid; else hi = mid; }
            const bool on = m >= 0.5f * (lo + hi);
            g.setColour ((Theme::teal.brighter (0.4f)).withAlpha (on ? 1.0f : 0.07f));
            g.fillRect (mBar.getX(), yTop + 0.5f, mBar.getWidth(), juce::jmax (1.0f, yBot - yTop - 1.0f));
        }
        const float yt = levelToY ((float) s.settings.targetLufs, mBar);
        g.setColour (Theme::gold);
        g.fillRect (mBar.getX() - 3.0f, yt - 1.0f, mBar.getWidth() + 6.0f, 2.0f);
        if (live && L.shortTerm > -60.0f) { g.setColour (Theme::ink); g.fillRect (mBar.getX(), levelToY (L.shortTerm, mBar) - 1.0f, mBar.getWidth(), 2.0f); }
        if (std::isfinite (L.integrated)) { g.setColour (Theme::lamp); g.drawHorizontalLine ((int) levelToY ((float) L.integrated, mBar), mBar.getX() - 2.0f, mBar.getRight() + 2.0f); }
        value (mBar, live ? shortDb (L.momentary) : (s.outMeas.valid ? shortDb ((float) s.outMeas.integrated) : juce::String ("--")), Theme::teal.brighter (0.4f));
    }
    // C and LIM: gain reduction hanging from the top on the same scale (a 3 dB reduction reaches the "3" tick), max hold.
    const auto grArea = bars.withHeight (levelToY (-24.0f, bars) - bars.getY());
    auto grBar = [&] (const juce::Rectangle<float>& fullBar, float v, float maxv, juce::Colour c, bool enabled)
    {
        const auto bar = fullBar.withHeight (grArea.getHeight());
        const int segs = 40;
        for (int k = 0; k < segs; ++k)
        {
            const float yTop = bar.getY() + bar.getHeight() * (float) k / segs, yBot = bar.getY() + bar.getHeight() * (float) (k + 1) / segs;
            const float segY = (yTop + yBot) * 0.5f;
            float lo = -60.0f, hi = 0.0f;
            for (int it = 0; it < 18; ++it) { const float mid = 0.5f * (lo + hi); if (levelToY (mid, bars) > segY) lo = mid; else hi = mid; }
            const float db = -0.5f * (lo + hi);
            const bool on = enabled && v > db + 0.05f;
            g.setColour (c.withAlpha (on ? 1.0f : 0.07f));
            g.fillRect (bar.getX(), yTop + 0.5f, bar.getWidth(), juce::jmax (1.0f, yBot - yTop - 1.0f));
        }
        if (enabled && maxv > 0.05f)
        {
            g.setColour (c.brighter (0.4f));
            g.drawHorizontalLine ((int) levelToY (-juce::jmin (24.0f, maxv), bars), bar.getX() - 2.0f, bar.getRight() + 2.0f);
        }
    };
    const bool compOn = s.settings.comp.enabled && ! s.settings.bypass, limOn = s.settings.limiter.enabled && ! s.settings.bypass;
    const float compNow = live ? L.compGr : 0.0f, limNow = live ? L.limGr : 0.0f;
    grBar (cBar, compNow, L.compGrMax, Theme::teal, compOn);
    grBar (limBar, limNow, L.limGrMax, Theme::gold, limOn);
    value (cBar, compOn ? juce::String (live ? compNow : (float) s.glue.loudMeanGrDb, 1) : juce::String ("--"), Theme::teal.brighter (0.3f));
    value (limBar, limOn ? juce::String (live ? limNow : (float) s.limGr.maxDb, 1) : juce::String ("--"), Theme::gold);
    // GR scale words
    vfdText (g, "GR DB", juce::Rectangle<float> (cBar.getX() - 2.0f, grArea.getBottom() + 2.0f, limBar.getRight() - cBar.getX() + 4.0f, 9.0f), juce::Justification::centred, 6.5f, Theme::vfd.withAlpha (0.45f));
    vfdText (g, "LUFS", juce::Rectangle<float> (mBar.getX() - 4.0f, bars.getBottom() + 1.0f, mBar.getWidth() + 8.0f, 9.0f), juce::Justification::centred, 6.5f, Theme::vfd.withAlpha (0.45f));
    vfdText (g, "DBFS/TP", juce::Rectangle<float> (inBar.getX() - 2.0f, bars.getBottom() + 1.0f, rBar.getRight() - inBar.getX() + 4.0f, 9.0f), juce::Justification::centred, 6.5f, Theme::vfd.withAlpha (0.45f));

    // Text: the file's peaks with their time, live true peak, loudness.
    auto text = juce::Rectangle<float> (inner.getX(), inner.getBottom() - textH, inner.getWidth(), textH);
    const double rate = s.sampleRate > 0.0 ? s.sampleRate : 48000.0;
    auto at = [&] (long long idx) { return idx >= 0 ? " @" + timeText ((double) idx / rate) : juce::String(); };
    auto line = [&] (const juce::String& t, juce::Colour c) { vfdText (g, t, text.removeFromTop (13.0f), juce::Justification::centredLeft, 8.0f, c); };
    const bool haveOut = s.outMeas.valid, haveIn = s.inMeas.valid;
    line (haveOut ? "OUT TP " + dbText (s.outMeas.truePeak, 2) + at (s.outMeas.truePeakAt) : juce::String ("OUT TP --"),
          haveOut && s.outMeas.truePeak > ceiling + 0.005 ? Theme::coral : Theme::gold);
    line (haveOut ? "    SP " + dbText (s.outMeas.samplePeak, 2) + at (s.outMeas.samplePeakAt) : juce::String ("    SP --"), Theme::vfd.withAlpha (0.8f));
    line (haveIn ? "IN  TP " + dbText (s.inMeas.truePeak, 2) + at (s.inMeas.truePeakAt) : juce::String ("IN  TP --"), Theme::gold.withAlpha (0.85f));
    line (haveIn ? "    SP " + dbText (s.inMeas.samplePeak, 2) + at (s.inMeas.samplePeakAt) : juce::String ("    SP --"), Theme::vfd.withAlpha (0.8f));
    text.removeFromTop (4.0f);
    const float tpNow = L.outTruePeakHold, tpMax = L.outTruePeakMax;
    line ("TP NOW " + (live && tpNow > -99.0f ? juce::String (tpNow, 1) : juce::String ("--")) + "  MAX " + (tpMax > -99.0f ? juce::String (tpMax, 1) : juce::String ("--")),
          tpMax > ceiling + 0.01f ? Theme::coral : Theme::vfd);
    line ("M " + (live ? shortDb (L.momentary) : juce::String ("--")) + "  S " + (live ? shortDb (L.shortTerm) : juce::String ("--")), Theme::vfd);
    line ("I " + (std::isfinite (L.integrated) ? juce::String (L.integrated, 1) : juce::String ("--")) + " SINCE PLAY", Theme::lamp);
    line ("CLICK: RESET MAX", Theme::vfd.withAlpha (0.35f));
    vfdGlassTop (g, b);
}

//==============================================================================
void Lamp::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    const juce::Point<float> c (b.getCentreX(), 9.0f);
    if (lit) glow (g, c, 16.0f, colour, 0.55f);
    g.setColour (juce::Colour (0xff0b0a06));
    g.fillEllipse (c.x - 7.0f, c.y - 7.0f, 14.0f, 14.0f);
    g.setGradientFill (juce::ColourGradient (lit ? colour.brighter (0.3f) : colour.withMultipliedBrightness (0.25f), c.x - 2.0f, c.y - 2.0f,
                                             lit ? colour : colour.withMultipliedBrightness (0.12f), c.x + 5.0f, c.y + 5.0f, true));
    g.fillEllipse (c.x - 5.5f, c.y - 5.5f, 11.0f, 11.0f);
    g.setColour (juce::Colours::white.withAlpha (lit ? 0.7f : 0.2f));
    g.fillEllipse (c.x - 3.0f, c.y - 3.5f, 2.5f, 2.0f);
    g.setColour (Theme::ink);
    tracked (g, legend, Theme::legend (7.5f), b.withTrimmedTop (20.0f), juce::Justification::centredTop, 0.16f);
}

//==============================================================================
void InfoDisplay::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    vfdGlass (g, b);
    auto area = b.reduced (12.0f, 8.0f);
    const float lh = area.getHeight() / 4.0f;
    auto line = [&] (const juce::String& label, const juce::String& text, juce::Colour c)
    {
        auto r = area.removeFromTop (lh);
        vfdText (g, label, r.removeFromLeft (70.0f), juce::Justification::centredLeft, 9.0f, Theme::vfd.withAlpha (0.5f));
        vfdText (g, text, r, juce::Justification::centredLeft, 10.0f, c);
    };
    const auto& s = ctx.session;
    juce::String src = "--";
    if (ctx.isApp())
    {
        if (s.input != nullptr)
            src = s.inputName.toUpperCase() + "   " + juce::String (s.sampleRate / 1000.0, 1) + " KHZ  " + juce::String (s.sourceBits) + "-BIT  "
                  + (s.input->getNumChannels() == 1 ? "MONO  " : "STEREO  ") + timeText ((double) s.input->getNumSamples() / s.sampleRate);
        line ("SOURCE", src, Theme::vfd);
    }
    else
    {
        src = "HOST INPUT  " + juce::String (ctx.processor.getSampleRate() / 1000.0, 1) + " KHZ";
        if (s.hasAnalysis()) src << "   " << s.inputName.toUpperCase();
        else src << "   NOT LEARNED";
        line ("SOURCE", src, Theme::vfd.withAlpha (s.hasAnalysis() ? 1.0f : 0.6f));
    }
    // What the loudness stage does, in plain words (the reference's numbers are in the table's REF column).
    const auto story = s.loudness();
    const bool down = story.valid && story.changeDb < -0.3 && s.settings.levelHeld() && ! s.settings.bypass;
    line ("LOUDNESS", story.valid ? story.shortLine : juce::String ("--"), story.valid ? (story.severity == 2 ? Theme::coral : (down || story.severity == 1) ? Theme::lamp : Theme::vfd) : Theme::vfd.withAlpha (0.3f));
    line ("STATUS", s.status.toUpperCase(), Theme::vfd.withAlpha (0.85f));
    line ("WARNING", s.warnings.isEmpty() ? juce::String ("NONE") : s.warnings[0].toUpperCase() + (s.warnings.size() > 1 ? "  (+" + juce::String (s.warnings.size() - 1) + ")" : ""),
          s.warnings.isEmpty() ? Theme::vfd.withAlpha (0.3f) : Theme::coral);
    vfdGlassTop (g, b);
}

void LearnDisplay::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    vfdGlass (g, b);
    auto area = b.reduced (12.0f, 9.0f);
    auto& p = ctx.processor;
    const auto st = p.learnState();
    juce::String big, small;
    juce::Colour c = Theme::vfd;
    const double secs = p.learnSeconds();
    switch (st)
    {
        case MihenkProcessor::Learn::armed:
            big = "LEARN ARMED"; small = "PLAY THE SONG FROM THE START"; c = Theme::lamp; break;
        case MihenkProcessor::Learn::capturing:
            big = "LISTENING " + timeText (secs); small = "STOP THE HOST (OR PRESS LEARN) TO SOLVE"; c = Theme::lamp; break;
        case MihenkProcessor::Learn::analysing:
            big = "ANALYSING"; small = "MEASURING, DIAGNOSING, SOLVING THE TARGET"; c = Theme::lamp; break;
        case MihenkProcessor::Learn::idle:
        default:
            if (p.isLearned())
            {
                big = ctx.session.inputName.toUpperCase();
                if (big.isEmpty()) big = "LEARNED";
                small = p.learnedIsStale() ? "SETTINGS CHANGED: LEARN AGAIN TO RE-SOLVE"
                                           : ctx.session.input != nullptr ? "RE-SOLVES AFTER EVERY CHANGE" : "RESULTS SAVED WITH THE PROJECT";
                if (p.learnedIsStale()) c = Theme::coral;
            }
            else
            {
                big = "NOT LEARNED";
                small = "LEARN, THEN PLAY THE SONG ONCE";
                c = Theme::vfd.withAlpha (0.6f);
            }
            break;
    }
    vfdText (g, big, area.removeFromTop (area.getHeight() * 0.52f), juce::Justification::centredLeft, 13.0f, c);
    vfdText (g, small, area.removeFromTop (14.0f), juce::Justification::centredLeft, 8.5f, (c == Theme::coral ? Theme::coral : Theme::vfd).withAlpha (0.75f));
    if (st == MihenkProcessor::Learn::capturing)
    {
        const auto bar = area.withTrimmedTop (4.0f).withHeight (4.0f);
        g.setColour (Theme::vfd.withAlpha (0.12f));
        g.fillRect (bar);
        g.setColour (Theme::lamp);
        g.fillRect (bar.withWidth (bar.getWidth() * (float) juce::jlimit (0.0, 1.0, secs / (Capture::maxMinutes * 60.0))));
    }
    vfdGlassTop (g, b);
}

void SmallVfd::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    vfdGlass (g, b);
    auto area = b.reduced (10.0f, 2.0f);
    vfdText (g, l, area.removeFromLeft (area.getWidth() * 0.42f), juce::Justification::centredLeft, 8.5f, Theme::vfd.withAlpha (0.55f));
    vfdText (g, v, area, juce::Justification::centredRight, 10.0f, Theme::vfd);
    vfdGlassTop (g, b);
}

//==============================================================================
void PresetDisplay::set (const juce::String& n, bool m, bool f)
{
    if (n == name && m == modified && f == factory) return;
    name = n; modified = m; factory = f;
    repaint();
}

void PresetDisplay::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    vfdGlass (g, b);
    auto area = b.reduced (12.0f, 2.0f);
    vfdText (g, factory ? "FACTORY" : "USER", area.removeFromLeft (58.0f), juce::Justification::centredLeft, 8.5f, Theme::vfd.withAlpha (0.5f));
    auto starArea = area.removeFromRight (14.0f);
    vfdText (g, name.toUpperCase(), area, juce::Justification::centredLeft, 10.0f, Theme::vfd);
    if (modified) vfdText (g, "*", starArea, juce::Justification::centred, 11.0f, Theme::lamp);
    vfdGlassTop (g, b);
}

void GrReadout::paint (juce::Graphics& g)
{
    auto& s = ctx.session;
    const auto b = getLocalBounds().toFloat();
    const auto glass = b.withHeight ((float) glassHeight).withSizeKeepingCentre (54.0f, (float) glassHeight);
    vfdGlass (g, glass);
    const auto& gl = s.glue;
    const bool three = s.settings.comp.mode == CompMode::threeBand;
    const int band = juce::jlimit (0, 2, ctx.selectedBand);
    const bool on = gl.valid && s.settings.comp.enabled && ! s.settings.bypass && (! three || (gl.threeBand && gl.bands[band].active));
    const double avg = three ? gl.bands[band].loudMeanGrDb : gl.loudMeanGrDb, mx = three ? gl.bands[band].loudMaxGrDb : gl.loudMaxGrDb;
    auto rows = glass.reduced (7.0f, 7.0f);
    const float rh = rows.getHeight() * 0.5f;
    auto row = [&] (const char* label, double v)
    {
        auto r = rows.removeFromTop (rh);
        vfdText (g, label, r.removeFromLeft (20.0f), juce::Justification::centredLeft, 7.0f, Theme::vfd.withAlpha (on ? 0.55f : 0.2f));
        vfdText (g, on ? juce::String (v, 1) : juce::String ("--"), r, juce::Justification::centredRight, 9.5f, Theme::vfd.withAlpha (on ? 1.0f : 0.3f));
    };
    row ("AVG", avg);
    row ("MAX", mx);
    vfdGlassTop (g, glass);
    const float y = (float) glassHeight;
    const char* names[] { "LOW GR", "MID GR", "HIGH GR" };
    g.setColour (Theme::ink.withAlpha (on ? 1.0f : 0.45f));
    tracked (g, three ? names[band] : "COMP GR", Theme::legend (7.5f), { 0.0f, y + 5.0f, b.getWidth(), 11.0f }, juce::Justification::centred, 0.12f);
    const bool dense = on && gl.autoThreshold && gl.dense;
    g.setColour ((dense ? Theme::coral : on ? Theme::lamp : Theme::ink).withAlpha (on ? 0.9f : 0.35f));
    g.setFont (Theme::mono (8.5f));
    g.drawText (dense ? "DENSE IN" : "DB, LOUD", juce::Rectangle<float> (0.0f, y + 17.0f, b.getWidth(), 11.0f), juce::Justification::centred);
}

//==============================================================================
// DIAGNOSIS

DiagnosisPanel::DiagnosisPanel (Context& c) : ctx (c)
{
    for (auto* cap : { &startRap, &startPop, &startElectronic, &saveScenario }) addAndMakeVisible (*cap);
    saveScenario.showLed = false;
    // v3: starting points and scenario buttons are gone (they wrote knobs). The panel is information only.
    for (auto* cap : { &startRap, &startPop, &startElectronic, &saveScenario }) cap->setVisible (false);
    startRap.onClick = [this] { if (onStartingPoint) onStartingPoint (Genre::rapTrap); };
    startPop.onClick = [this] { if (onStartingPoint) onStartingPoint (Genre::arabeskPop); };
    startElectronic.onClick = [this] { if (onStartingPoint) onStartingPoint (Genre::electronicLofi); };
    saveScenario.onClick = [this] { if (onSaveScenario) onSaveScenario(); };
    startRap.setTooltip ("Starting point HIP-HOP / RAP: tone and dynamics only, never the target or the ceiling");
    startPop.setTooltip ("Starting point R&B / POP");
    startElectronic.setTooltip ("Starting point ELECTRONIC / LO-FI");
    saveScenario.setTooltip ("Save the settings with the applied scenarios as your own preset (category Scenarios)");
}

juce::Rectangle<float> DiagnosisPanel::listArea() const { return { 0.0f, 44.0f, (float) getWidth(), 248.0f }; }
juce::Rectangle<float> DiagnosisPanel::chipArea() const { return { 0.0f, 296.0f, (float) getWidth(), 40.0f }; }

void DiagnosisPanel::resized()
{
    const int w = getWidth();
    const int x0 = 40, cw = (w - x0 - 6) / 3;
    startRap.setBounds (x0, 17, cw, 22);
    startPop.setBounds (x0 + cw + 3, 17, cw, 22);
    startElectronic.setBounds (x0 + 2 * (cw + 3), 17, w - x0 - 2 * (cw + 3), 22);
    saveScenario.setBounds (0, getHeight() - 23, w, 22);
}

void DiagnosisPanel::refresh()
{
    shown = ctx.session.currentDiagnosis();
    const int rows = (int) shown.findings.size();
    scroll = juce::jlimit (0, juce::jmax (0, rows - 5), scroll);
    saveScenario.setEnabled (! ctx.session.appliedScenarios.isEmpty());
    repaint();
}

void DiagnosisPanel::paint (juce::Graphics& g)
{
    hits.clear();
    auto& s = ctx.session;
    const float w = (float) getWidth();
    g.setColour (Theme::ink);
    tracked (g, "DIAGNOSIS", Theme::legend (8.5f), { 0.0f, 0.0f, 90.0f, 13.0f }, juce::Justification::centredLeft, 0.16f);
    g.setColour (Theme::ink.withAlpha (0.7f));
    const int n = (int) shown.findings.size();
    tracked (g, ! shown.m.valid ? juce::String ("NOTHING MEASURED YET") : n == 0 ? juce::String ("NO FINDINGS") : juce::String (n) + (n == 1 ? " FINDING" : " FINDINGS") + ", MEASURED",
             Theme::fine (7.5f), { 92.0f, 0.0f, w - 92.0f, 13.0f }, juce::Justification::centredRight, 0.12f);
    g.setColour (Theme::ink.withAlpha (0.55f));
    tracked (g, "INFORMATION: CHARACTER FULL APPLIES WHAT IT CAN", Theme::fine (7.0f), { 0.0f, 17.0f, w, 22.0f }, juce::Justification::centredLeft, 0.1f);

    const auto list = listArea();
    vfdGlass (g, list);
    const auto inner = list.reduced (9.0f, 7.0f);
    if (! shown.m.valid || n == 0)
    {
        const juce::String a = ! shown.m.valid ? (ctx.isApp() ? "LOAD A MIX: THE DIAGNOSIS RUNS ON THE WHOLE SONG" : "PRESS LEARN AND PLAY THE SONG: THE DIAGNOSIS RUNS ON IT")
                                               : "EVERY RULE IS INSIDE ITS THRESHOLD";
        const juce::String b = ! shown.m.valid ? "20 MEASURED RULES, EACH WITH A SCENARIO TO APPLY" : "NOTHING TO FIX BY MEASUREMENT; TRUST YOUR EARS";
        vfdText (g, a, inner.withHeight (inner.getHeight() * 0.5f), juce::Justification::centredBottom, 8.5f, Theme::vfd.withAlpha (0.75f));
        vfdText (g, b, inner.withTrimmedTop (inner.getHeight() * 0.5f + 4.0f).withHeight (14.0f), juce::Justification::centredTop, 7.5f, Theme::vfd.withAlpha (0.45f));
    }
    else
    {
        const float rowH = inner.getHeight() / 5.0f;
        for (int i = 0; i < 5 && scroll + i < n; ++i)
        {
            const auto& f = shown.findings[(size_t) (scroll + i)];
            auto row = juce::Rectangle<float> (inner.getX(), inner.getY() + rowH * (float) i, inner.getWidth(), rowH);
            const bool applied = s.appliedScenarios.contains (f.scenario);
            if (row.contains (mouse)) { g.setColour (Theme::vfd.withAlpha (0.05f)); g.fillRect (row); }
            if (i > 0) { g.setColour (Theme::vfd.withAlpha (0.10f)); g.drawHorizontalLine ((int) row.getY(), row.getX(), row.getRight()); }
            const juce::Colour sev = f.severity == Severity::problem ? Theme::coral : f.severity == Severity::warn ? Theme::lamp : Theme::teal.brighter (0.3f);
            const juce::Point<float> lampC (row.getX() + 5.0f, row.getY() + 9.0f);
            glow (g, lampC, 8.0f, sev, 0.45f);
            g.setColour (sev);
            g.fillEllipse (lampC.x - 3.5f, lampC.y - 3.5f, 7.0f, 7.0f);
            const auto apply = juce::Rectangle<float> (row.getRight() - 4.0f, row.getY() + 2.0f, 0.0f, 15.0f);
            const juce::String sevText = f.severity == Severity::problem ? "PROBLEM" : f.severity == Severity::warn ? "WARN" : "INFO";
            vfdText (g, f.title.toUpperCase(), juce::Rectangle<float> (row.getX() + 14.0f, row.getY() + 2.0f, apply.getX() - row.getX() - 58.0f, 15.0f),
                     juce::Justification::centredLeft, 9.0f, Theme::vfd);
            vfdText (g, sevText, juce::Rectangle<float> (apply.getX() - 44.0f, row.getY() + 2.0f, 40.0f, 15.0f), juce::Justification::centredRight, 7.0f, sev);
            // One measured reason, on up to two lines.
            g.setFont (Theme::mono (7.5f));
            g.setColour (Theme::vfd.withAlpha (0.72f));
            g.drawFittedText (f.reason, juce::Rectangle<float> (row.getX() + 14.0f, row.getY() + 18.0f, row.getWidth() - 14.0f, rowH - 20.0f).toNearestInt(),
                              juce::Justification::topLeft, 2, 1.0f);
            hits.push_back ({ row, f.scenario, 0, f.reason + "\nA hand edit for this would be: " + f.delta + "\nCHARACTER FULL applies what it can." });
        }
        if (n > 5)
        {
            const juce::String more = (scroll > 0 ? juce::String ("^ ") : juce::String()) + juce::String (n - 5 - scroll) + " MORE (SCROLL)";
            vfdText (g, more, juce::Rectangle<float> (list.getRight() - 120.0f, list.getBottom() - 12.0f, 112.0f, 10.0f), juce::Justification::centredRight, 7.0f, Theme::lamp.withAlpha (0.8f));
        }
    }
    vfdGlassTop (g, list);

    // Applied scenarios used to be chips; v3 has none (a finding is information, the plan lists what it did).
    const auto chips = chipArea();
    g.setColour (Theme::ink.withAlpha (0.45f));
    tracked (g, "THE PLAN LISTS EVERY STEP IT TOOK, IN DB, ON THE LEFT", Theme::fine (7.5f), chips.withTrimmedTop (14.0f).withHeight (13.0f), juce::Justification::centredLeft, 0.1f);
}

void DiagnosisPanel::mouseDown (const juce::MouseEvent& e)
{
    for (const auto& h : hits)
        if (h.area.contains (e.position))
        {
            if (h.kind == 1 && ! ctx.session.appliedScenarios.contains (h.scenario) && onApply) onApply (h.scenario);
            if (h.kind == 2 && onRemove) onRemove (h.scenario);
            return;
        }
}

void DiagnosisPanel::mouseMove (const juce::MouseEvent& e) { mouse = e.position; repaint(); }
void DiagnosisPanel::mouseExit (const juce::MouseEvent&) { mouse = { -1.0f, -1.0f }; repaint(); }

void DiagnosisPanel::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    const int n = (int) shown.findings.size();
    scroll = juce::jlimit (0, juce::jmax (0, n - 5), scroll + (w.deltaY < 0.0f ? 1 : -1));
    repaint();
}

juce::String DiagnosisPanel::getTooltip()
{
    for (const auto& h : hits)
        if (h.area.contains (mouse)) return h.tip;
    return {};
}

//==============================================================================
void CrossoverDisplay::mouseDown (const juce::MouseEvent& e)
{
    const auto& c = ctx.session.settings.comp;
    const auto plot = getLocalBounds().toFloat().reduced (9.0f, 8.0f);
    const float xl = plot.getX() + (float) (std::log2 (c.xoverLowHz / 20.0) / std::log2 (1000.0)) * plot.getWidth();
    const float xh = plot.getX() + (float) (std::log2 (c.xoverHighHz / 20.0) / std::log2 (1000.0)) * plot.getWidth();
    const int b = e.position.x < xl ? 0 : e.position.x < xh ? 1 : 2;
    if (onSelectBand) onSelectBand (b);
}

void CrossoverDisplay::paint (juce::Graphics& g)
{
    auto& s = ctx.session;
    const auto& c = s.settings.comp;
    const auto screen = smallScreen (g, getLocalBounds().toFloat());
    const auto plot = screen.reduced (4.0f, 5.0f).withTrimmedBottom (9.0f);
    auto xOf = [&] (double f) { return plot.getX() + (float) (std::log2 (f / 20.0) / std::log2 (1000.0)) * plot.getWidth(); };
    auto yOf = [&] (double db) { return plot.getY() + (float) juce::jlimit (0.0, 1.0, -db / 24.0) * plot.getHeight(); };
    const bool live = ctx.live.running();
    const char* names[] { "LOW", "MID", "HIGH" };
    const juce::Colour bandCol[] { Theme::teal.brighter (0.3f), Theme::lamp, Theme::phosphor };
    for (int b = 0; b < 3; ++b)
    {
        juce::Path p;
        for (int i = 0; i <= 120; ++i)
        {
            const double f = 20.0 * std::pow (2.0, i / 12.0);
            const double lp1 = 1.0 / (1.0 + std::pow (f / c.xoverLowHz, 4.0)), lp2 = 1.0 / (1.0 + std::pow (f / c.xoverHighHz, 4.0));
            const double mag = b == 0 ? lp1 : b == 1 ? (1.0 - lp1) * lp2 : (1.0 - lp1) * (1.0 - lp2);
            const auto pt = juce::Point<float> (xOf (f), yOf (20.0 * std::log10 (juce::jmax (1.0e-6, mag))));
            if (i == 0) p.startNewSubPath (pt); else p.lineTo (pt);
        }
        const bool sel = b == ctx.selectedBand;
        const auto& bs = c.bands[b];
        phosphorPath (g, p, bandCol[b].withAlpha (bs.bypass ? 0.25f : (sel ? 1.0f : 0.5f)), sel ? 1.4f : 1.0f);
        // The band's gain reduction as a bar hanging from 0 dB in its region.
        const double lo = b == 0 ? 20.0 : b == 1 ? c.xoverLowHz : c.xoverHighHz, hi = b == 0 ? c.xoverLowHz : b == 1 ? c.xoverHighHz : 20000.0;
        const float gr = live ? ctx.live.bandGr[b] : (s.glue.threeBand && s.glue.bands[b].active ? (float) s.glue.bands[b].loudMeanGrDb : 0.0f);
        const float x0 = xOf (lo) + 3.0f, x1 = xOf (hi) - 3.0f;
        if (! bs.bypass && gr > 0.02f && x1 > x0)
        {
            g.setColour (bandCol[b].withAlpha (sel ? 0.45f : 0.25f));
            // 0..6 dB of reduction over the top 60 % of the plot, so a 1 dB glue is visible.
            g.fillRect (juce::Rectangle<float> (x0 + (x1 - x0) * 0.3f, plot.getY(), (x1 - x0) * 0.4f, (float) juce::jmin (6.0f, gr) / 6.0f * plot.getHeight() * 0.6f));
        }
        g.setFont (Theme::mono (7.5f));
        g.setColour (bandCol[b].withAlpha (sel ? 1.0f : 0.6f));
        juce::String tag = names[b];
        if (bs.solo) tag << " S";
        if (bs.bypass) tag << " BYP";
        g.drawText (tag, juce::Rectangle<float> (x0 - 4.0f, plot.getY() + plot.getHeight() * 0.45f, juce::jmax (24.0f, x1 - x0 + 8.0f), 10.0f), juce::Justification::centred);
    }
    g.setFont (Theme::mono (7.5f));
    g.setColour (Theme::phosphor.withAlpha (0.75f));
    for (double f : { c.xoverLowHz, c.xoverHighHz })
    {
        const float x = xOf (f);
        g.setColour (Theme::gold.withAlpha (0.7f));
        g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
        g.setColour (Theme::phosphor.withAlpha (0.85f));
        g.drawText (f >= 1000.0 ? juce::String (f / 1000.0, 1) + "K" : juce::String (juce::roundToInt (f)), juce::Rectangle<float> (x - 18.0f, screen.getBottom() - 11.0f, 36.0f, 9.0f),
                    juce::Justification::centred);
    }
    g.setColour (Theme::phosphor.withAlpha (0.55f));
    g.drawText ("BANDS, GR " + juce::String (live ? "LIVE" : "LOUD AVG"), screen.reduced (7.0f, 4.0f).withHeight (9.0f), juce::Justification::topLeft);
    screenGlass (g, screen);
}

void GrTrace::paint (juce::Graphics& g)
{
    auto& s = ctx.session;
    auto& L = ctx.live;
    const auto screen = smallScreen (g, getLocalBounds().toFloat());
    const auto plot = screen.reduced (6.0f, 6.0f).withTrimmedTop (9.0f);
    const bool on = s.settings.comp.enabled && ! s.settings.bypass;
    const bool live = L.running();
    auto yOf = [&] (float db) { return plot.getY() + juce::jlimit (0.0f, 1.0f, db / 6.0f) * plot.getHeight(); };
    g.setColour (Theme::phosphor.withAlpha (0.12f));
    for (float db : { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f }) g.drawHorizontalLine ((int) yOf (db), plot.getX(), plot.getRight());
    juce::Path p;
    if (on && live)
    {
        const int visible = 600, count = juce::jmin (L.columnCount(), visible);
        const float dx = plot.getWidth() / (float) visible;
        for (int i = 0; i < count; ++i)
        {
            const auto& col = L.column (L.columnCount() - count + i);
            const auto pt = juce::Point<float> (plot.getRight() - (float) (count - i) * dx, yOf (col.comp));
            if (i == 0) p.startNewSubPath (pt); else p.lineTo (pt);
        }
    }
    else if (on && ! s.compGr.history.empty())
    {
        const auto& h = s.compGr.history;
        const int w = (int) plot.getWidth();
        for (int x = 0; x < w; ++x)
        {
            const size_t a = (size_t) ((double) h.size() * x / w), z = juce::jmax (a + 1, (size_t) ((double) h.size() * (x + 1) / w));
            float m = 0.0f;
            for (size_t i = a; i < z && i < h.size(); ++i) m = juce::jmax (m, h[i]);
            const auto pt = juce::Point<float> (plot.getX() + (float) x, yOf (m));
            if (x == 0) p.startNewSubPath (pt); else p.lineTo (pt);
        }
    }
    phosphorPath (g, p, Theme::teal.brighter (0.3f), 1.0f);
    if (on && s.glue.valid)
    {
        const float ya = yOf ((float) s.glue.loudMeanGrDb);
        g.setColour (Theme::lamp.withAlpha (0.8f));
        for (float x = plot.getX(); x < plot.getRight(); x += 6.0f) g.drawLine (x, ya, juce::jmin (x + 3.0f, plot.getRight()), ya, 1.0f);
    }
    g.setFont (Theme::mono (7.5f));
    g.setColour (Theme::phosphor.withAlpha (0.8f));
    const auto head = screen.reduced (6.0f, 3.0f).withHeight (9.0f);
    g.drawText (on ? (live ? "GAIN REDUCTION  LAST 6 S" : "GAIN REDUCTION  WHOLE TRACK") : "GAIN REDUCTION  (COMP OUT)", head, juce::Justification::left);
    g.setColour (Theme::lamp.withAlpha (0.85f));
    g.drawText (on && s.glue.valid ? "LOUD AVG " + juce::String (s.glue.loudMeanGrDb, 1) + " DB (DASHED)" : juce::String(), head, juce::Justification::right);
    g.setColour (Theme::phosphor.withAlpha (0.45f));
    g.drawText ("6 DB", juce::Rectangle<float> (plot.getX(), plot.getBottom() - 9.0f, 30.0f, 9.0f), juce::Justification::left);
    screenGlass (g, screen);
}

void CorrelationMeter::paint (juce::Graphics& g)
{
    auto& s = ctx.session;
    auto& L = ctx.live;
    const auto b = getLocalBounds().toFloat();
    vfdGlass (g, b);
    auto area = b.reduced (9.0f, 7.0f);
    vfdText (g, "CORRELATION", area.removeFromTop (11.0f), juce::Justification::centredLeft, 7.5f, Theme::vfd.withAlpha (0.6f));
    const bool live = L.running();
    const float full = live ? L.correlation : (s.outMeas.valid ? (float) s.outMeas.correlation : 1.0f);
    const float low = live ? L.lowCorrelation : (s.diagnosis.m.valid ? (float) s.diagnosis.m.lowCorrelation : 1.0f);
    const bool haveLow = live || s.diagnosis.m.valid;
    auto meterRow = [&] (const juce::String& name, float v, bool valid, bool whole)
    {
        auto r = area.removeFromTop (area.getHeight() * 0.5f);
        vfdText (g, name, r.removeFromTop (10.0f), juce::Justification::centredLeft, 7.0f, Theme::vfd.withAlpha (0.5f));
        const auto track = r.withHeight (7.0f).translated (0.0f, 2.0f);
        g.setColour (Theme::vfd.withAlpha (0.10f));
        g.fillRect (track);
        g.setColour (Theme::vfd.withAlpha (0.35f));
        g.drawVerticalLine ((int) track.getCentreX(), track.getY() - 2.0f, track.getBottom() + 2.0f);
        if (valid)
        {
            const float x = track.getX() + (juce::jlimit (-1.0f, 1.0f, v) + 1.0f) * 0.5f * track.getWidth();
            const auto c = v < 0.0f ? Theme::coral : v < 0.3f ? Theme::lamp : Theme::vfd;
            g.setColour (c.withAlpha (whole ? 0.6f : 1.0f));
            g.fillRect (juce::Rectangle<float> (juce::jmin (x, track.getCentreX()), track.getY(), std::abs (x - track.getCentreX()), track.getHeight()));
            g.fillRect (x - 1.5f, track.getY() - 2.0f, 3.0f, track.getHeight() + 4.0f);
            vfdText (g, juce::String (v, 2), juce::Rectangle<float> (track.getRight() - 30.0f, track.getY() - 11.0f, 30.0f, 10.0f), juce::Justification::centredRight, 7.5f, c);
        }
        vfdText (g, "-1", juce::Rectangle<float> (track.getX(), track.getBottom() + 1.0f, 14.0f, 8.0f), juce::Justification::centredLeft, 6.5f, Theme::vfd.withAlpha (0.4f));
        vfdText (g, "+1", juce::Rectangle<float> (track.getRight() - 14.0f, track.getBottom() + 1.0f, 14.0f, 8.0f), juce::Justification::centredRight, 6.5f, Theme::vfd.withAlpha (0.4f));
    };
    meterRow (live ? "FULL, LIVE" : "FULL, WHOLE TRACK", full, live || s.outMeas.valid, ! live);
    meterRow (live ? "BELOW 150 HZ, LIVE" : "BELOW 150 HZ, INPUT", low, haveLow, ! live);
    vfdGlassTop (g, b);
}

void LimiterReadout::paint (juce::Graphics& g)
{
    auto& s = ctx.session;
    const auto b = getLocalBounds().toFloat();
    vfdGlass (g, b);
    auto area = b.reduced (9.0f, 7.0f);
    const bool on = s.settings.limiter.enabled && ! s.settings.bypass;
    const auto& lr = s.limResult;
    const bool compact = b.getWidth() < 100.0f;     // ADVANCED: beside WARN AT in the limiter's last row
    vfdText (g, compact ? "LIMITER" : "LIMITER, 8X TRUE PEAK", area.removeFromTop (11.0f), juce::Justification::centredLeft, 7.5f, Theme::vfd.withAlpha (0.6f));
    auto row = [&] (const juce::String& label, const juce::String& value, juce::Colour c)
    {
        auto r = area.removeFromTop (area.getHeight() / 3.0f);
        if (compact)
        {
            vfdText (g, label, r.removeFromTop (r.getHeight() * 0.42f), juce::Justification::centredLeft, 6.5f, Theme::vfd.withAlpha (0.5f));
            vfdText (g, value, r, juce::Justification::centredRight, 8.0f, c);
            return;
        }
        vfdText (g, label, r.removeFromLeft (r.getWidth() * 0.46f), juce::Justification::centredLeft, 7.5f, Theme::vfd.withAlpha (0.5f));
        vfdText (g, value, r, juce::Justification::centredRight, 9.0f, c);
    };
    const bool have = s.outMeas.valid && on;
    row ("GR MAX", have ? juce::String (lr.gr.maxDb, 1) + " DB" : juce::String ("--"), lr.gr.maxDb > s.settings.limitWarnDb ? Theme::coral : Theme::vfd);
    row ("ACTIVE", have ? juce::String (juce::roundToInt (lr.gr.activeFraction * 100.0)) + (compact ? " %" : " % OF TIME") : juce::String ("--"), Theme::vfd);
    row ("TP OUT", s.outMeas.valid ? fmt (s.outMeas.truePeak, 2) + (compact ? "" : " DBTP") : juce::String ("--"),
         s.outMeas.valid && s.outMeas.truePeak > s.settings.limiter.ceilingDbtp + 0.005 ? Theme::coral : Theme::lamp);
    vfdGlassTop (g, b);
}
} // namespace mihenk::ui
