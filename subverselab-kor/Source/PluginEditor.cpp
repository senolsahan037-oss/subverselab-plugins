#include "PluginEditor.h"
#include "Theme.h"

namespace
{
constexpr float kPi = juce::MathConstants<float>::pi;
constexpr float kStart = 0.0f;            // 12 o'clock (JUCE angles: 0 = 12 o'clock, clockwise)
constexpr float kSpan = 2.0f * kPi;       // a full turn: 0 % and 100 % meet at 12 o'clock (owner, 8 Oct: 360 degrees)
constexpr float kDragPx = 220.0f;         // 0 .. 100 % over 220 px
const char* const kIds[4] { "low", "mid", "high", "mix" };
const char* const kLabels[3] { "LOW", "MID", "HIGH" };
const char* const kPushNames[3] { "FREE", "LINK", "GANG" };

float ringMid (int ring) noexcept { return KorEditor::ringOuter (ring) - KorEditor::ringWidth (ring) * 0.5f; }

juce::Path annulus (float rIn, float rOut)
{
    juce::Path p;
    p.addEllipse (KorEditor::kCx - rOut, KorEditor::kCy - rOut, 2.0f * rOut, 2.0f * rOut);
    p.addEllipse (KorEditor::kCx - rIn, KorEditor::kCy - rIn, 2.0f * rIn, 2.0f * rIn);
    p.setUsingNonZeroWinding (false);
    return p;
}

juce::Point<float> polar (float a, float r) noexcept
{
    return { KorEditor::kCx + r * std::sin (a), KorEditor::kCy - r * std::cos (a) };
}

float wrapPi (float a) noexcept
{
    while (a > kPi) a -= 2.0f * kPi;
    while (a < -kPi) a += 2.0f * kPi;
    return a;
}

/** A gradient lit from the top left over a path's bounds (in world coordinates, so rotated parts keep the light). */
juce::ColourGradient litFrom (juce::Rectangle<float> b, juce::Colour light, juce::Colour dark)
{
    return juce::ColourGradient (light, b.getX(), b.getY(), dark, b.getRight(), b.getBottom(), false);
}

juce::Line<float> xf (juce::Line<float> l, const juce::AffineTransform& t) noexcept
{
    return { l.getStart().transformedBy (t), l.getEnd().transformedBy (t) };
}

void strokeCircle (juce::Graphics& g, float r, float width)
{
    g.drawEllipse (KorEditor::kCx - r, KorEditor::kCy - r, 2.0f * r, 2.0f * r, width);
}

float hash01 (int k) noexcept
{
    const float s = std::sin ((float) k * 12.9898f) * 43758.5453f;
    return s - std::floor (s);
}

juce::String pct (float v) { return juce::String (juce::roundToInt (v * 100.0f)) + " %"; }

/** The product name in the family's squared, extended stroke letters (as TUTKAL's drawProductName: width 1.34 h,
    stroke 0.19 h, corner radius 0.30 h, gap 0.30 h), centred on (cx, cy). The strokes are turned into an outline,
    so every renderer draws the same corners. Letters: A D E G H I L M O V W X. */
juce::Path familyName (const juce::String& name, float cx, float cy, float h)
{
    const float w = 1.34f * h, t = 0.19f * h, r = 0.30f * h, gap = 0.30f * h, in = t * 0.5f;
    juce::Path all;
    float x = 0.0f;
    for (auto ch : name)
    {
        const bool narrow = ch == 'I';
        const float cw = narrow ? t : w;
        juce::Path p;
        const float L = x + in, R = x + cw - in, T = in, B = h - in, M = h * 0.5f, C = (L + R) * 0.5f;
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
            case 'V':
                p.startNewSubPath (L - in * 0.3f, T - in); p.lineTo (C, B); p.lineTo (R + in * 0.3f, T - in);
                break;
            case 'O':
                p.addRoundedRectangle (L, T, R - L, B - T, r);
                break;
            case 'W':
                p.startNewSubPath (L - in * 0.3f, T - in); p.lineTo (L + (R - L) * 0.25f, B); p.lineTo (C, T + h * 0.32f);
                p.lineTo (L + (R - L) * 0.75f, B); p.lineTo (R + in * 0.3f, T - in);
                break;
            case 'M':
                p.startNewSubPath (L, B + in); p.lineTo (L, T); p.lineTo (C, T + h * 0.55f); p.lineTo (R, T); p.lineTo (R, B + in);
                break;
            case 'I':
                p.startNewSubPath (C, T - in); p.lineTo (C, B + in);
                break;
            case 'D':
                p.startNewSubPath (L, T); p.lineTo (R - r, T); p.quadraticTo (R, T, R, T + r);
                p.lineTo (R, B - r); p.quadraticTo (R, B, R - r, B); p.lineTo (L, B); p.closeSubPath();
                break;
            case 'H':
                p.startNewSubPath (L, T - in); p.lineTo (L, B + in);
                p.startNewSubPath (R, T - in); p.lineTo (R, B + in);
                p.startNewSubPath (L, M); p.lineTo (R, M);
                break;
            case 'G':
                p.startNewSubPath (R + in * 0.2f, T); p.lineTo (L + r, T); p.quadraticTo (L, T, L, T + r);
                p.lineTo (L, B - r); p.quadraticTo (L, B, L + r, B); p.lineTo (R - r, B); p.quadraticTo (R, B, R, B - r);
                p.lineTo (R, M); p.lineTo (C + 0.05f * w, M);
                break;
            case 'X':
                p.startNewSubPath (L - in * 0.2f, T - in); p.lineTo (R + in * 0.2f, B + in);
                p.startNewSubPath (R + in * 0.2f, T - in); p.lineTo (L - in * 0.2f, B + in);
                break;
            default: break;
        }
        all.addPath (p);
        x += cw + gap;
    }
    juce::Path outline;
    juce::PathStrokeType (t, juce::PathStrokeType::mitered, juce::PathStrokeType::butt).createStrokedPath (outline, all);
    const auto bb = outline.getBounds();
    outline.applyTransform (juce::AffineTransform::translation (cx - bb.getCentreX(), cy - bb.getCentreY()));
    return outline;
}


// Radial alpha profiles of the three ember glows (position in the glow radius, weight), indexed LOW, MID, HIGH. The
// zones follow the steel rings (owner, 8 Oct): HIGH (inner ring) = the very centre, MID between, LOW (outer ring) = the edge.
struct Stop { float pos, w; };
// Three separate zones with dark coal between them (owner, 8 Oct: the bands looked squeezed together): HIGH a disc to
// 0.33 R, MID a ring around 0.51 R, LOW a ring around 0.83 R. Fixed in place: DRY/WET dims them, it does not move them.
const Stop kGlowProfile[3][5] {
    { { 0.00f, 0.00f }, { 0.68f, 0.00f }, { 0.83f, 0.95f }, { 0.97f, 0.00f }, { 1.00f, 0.00f } },
    { { 0.00f, 0.00f }, { 0.36f, 0.00f }, { 0.51f, 0.95f }, { 0.66f, 0.00f }, { 1.00f, 0.00f } },
    { { 0.00f, 1.00f }, { 0.18f, 0.85f }, { 0.33f, 0.00f }, { 0.50f, 0.00f }, { 1.00f, 0.00f } },
};

/** The glass dome as a lens: points inside it are pushed outwards a little, most near the centre (a magnifier),
    so the text under it reads as seen through curved glass. */
juce::Path throughDome (const juce::Path& src, float strength)
{
    juce::Path out;
    juce::PathFlatteningIterator it (src, {}, 0.15f);
    auto warp = [strength] (float x, float y)
    {
        const float dx = x - KorEditor::kCx, dy = y - KorEditor::kCy;
        const float r = std::sqrt (dx * dx + dy * dy) / KorEditor::kEmberR;
        const float k = 1.0f + strength * (1.0f - juce::jmin (1.0f, r * r));
        return juce::Point<float> (KorEditor::kCx + dx * k, KorEditor::kCy + dy * k);
    };
    bool open = false;
    while (it.next())
    {
        if (it.subPathIndex == 0 || ! open) { out.startNewSubPath (warp (it.x1, it.y1)); open = true; }
        out.lineTo (warp (it.x2, it.y2));
        if (it.closesSubPath) { out.closeSubPath(); open = false; }
    }
    out.setUsingNonZeroWinding (src.isUsingNonZeroWinding());
    return out;
}
} // namespace

//==============================================================================
KorEditor::KorEditor (KorAudioProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    setOpaque (true);
    for (int i = 0; i < 4; ++i)
    {
        params[i] = proc.apvts.getParameter (kIds[i]);
        jassert (params[i] != nullptr);
    }
    linkParam = proc.apvts.getParameter ("link");
    jassert (linkParam != nullptr);
    wordmark = juce::ImageCache::getFromMemory (BinaryData::wordmark_cream_png, BinaryData::wordmark_cream_pngSize);
    mark = juce::ImageCache::getFromMemory (BinaryData::mark_cream_png, BinaryData::mark_cream_pngSize);

    for (int b = 0; b < 3; ++b)
    {
        buildCurvedText (labelPath[b], kLabels[b], ringMid (b));
        // The position pointer (owner, 8 Oct): a raised bar of black steel across the ring (2 px short of both edges),
        // a cream inlay along its middle, two 1 px side faces that catch or lose the light as it turns.
        // Built pointing at 12 o'clock and rotated in paint().
        const float rIn = ringInner (b), rOut = ringOuter (b);
        const float top = kCy - (rOut - 2.0f), len = rOut - rIn - 4.0f;
        pointer[b].addRoundedRectangle (kCx - 2.6f, top, 5.2f, len, 1.6f);
        pointerInlay[b].addRoundedRectangle (kCx - 0.5f, top + 2.5f, 1.0f, len - 5.0f, 0.5f);
        pointerSide[b][0].addRectangle (kCx - 2.6f, top + 1.0f, 1.0f, len - 2.0f);
        pointerSide[b][1].addRectangle (kCx + 1.6f, top + 1.0f, 1.0f, len - 2.0f);
    }

    // The product name and the control names, in the family's squared, extended letters (owner, 8 Oct: the font).
    // Both are seen through the glass dome (owner, 8 Oct): magnified a little by the lens.
    namePath = throughDome (familyName ("ALEV", kCx, kCy, 20.0f), 0.10f);
    {
        const char* const names[4] { "LOW", "MID", "HIGH", "MIX" };
        for (int i = 0; i < 4; ++i) bandName[i] = throughDome (familyName (names[i], kCx, kCy - 7.0f, 16.0f), 0.10f);
    }
    // The model line on the bezel, centred at 12 o'clock, as on the other units (MODEL KR-77, HV-72, TK-79 ...).
    const juce::String dot (juce::CharPointer_UTF8 ("\xc2\xb7"));
    buildCurvedText (modelLine, "ALEV  " + dot + "  MODEL AL-81  " + dot + "  3-BAND SATURATOR", kBezelMid, 8.5f, 2.6f);
    // Bezel legends: the pushers' names under each pusher, DRY and WET at the ends of the fader slot.
    for (int i = 0; i < 3; ++i)
    {
        buildCurvedText (pushLabel[i], kPushNames[i], kBezelMid, 7.0f, 1.6f);
        pushLabel[i].applyTransform (juce::AffineTransform::rotation (kPushA[i], kCx, kCy));
    }
    buildCurvedText (dryLabel, "DRY", kBezelMid, 7.0f, 1.6f);
    dryLabel.applyTransform (juce::AffineTransform::rotation (kFaderA0 - 0.12f, kCx, kCy));
    buildCurvedText (wetLabel, "WET", kBezelMid, 7.0f, 1.6f);
    wetLabel.applyTransform (juce::AffineTransform::rotation (kFaderA1 + 0.12f, kCx, kCy));

    // Gradients reused every frame: only their colours (and the glow radius) change.
    const juce::Point<float> c (kCx, kCy);
    coalGrad = juce::ColourGradient (Theme::coalCentre, c, Theme::coalEdge, { kCx + kEmberR, kCy }, true);
    rimGrad = juce::ColourGradient (juce::Colours::transparentBlack, c, juce::Colours::black.withAlpha (0.6f), { kCx + kEmberR, kCy }, true);
    rimGrad.addColour (0.78, juce::Colours::transparentBlack);
    for (int b = 0; b < 3; ++b)
    {
        emberGrad[b] = juce::ColourGradient (juce::Colours::transparentBlack, c, juce::Colours::transparentBlack, { kCx + kEmberR, kCy }, true);
        for (int s = 1; s < 4; ++s) emberGrad[b].addColour ((double) kGlowProfile[b][s].pos, juce::Colours::transparentBlack);
    }

    setResizable (false, false);
    setSize (kW, kH);
    timerCallback();
    updateTimer();
}

KorEditor::~KorEditor()
{
    stopTimer();
    endMove();
}

void KorEditor::updateTimer()
{
    if (isShowing()) { if (! isTimerRunning()) startTimerHz (30); }
    else stopTimer();
}

void KorEditor::timerCallback()
{
    clock += 1.0 / 30.0;
    const float t = (float) clock;
    for (int b = 0; b < 3; ++b)
    {
        glow[b] = juce::jlimit (0.0f, 1.0f, proc.glow (b));
        // Slow organic flicker, +-4 % brightness: three incommensurate sines per band, driven only by the tick count.
        const float ph = 1.7f * (float) b;
        const float n = 0.5f * std::sin (2.0f * kPi * 0.37f * t + ph)
                      + 0.3f * std::sin (2.0f * kPi * 0.91f * t + 1.3f + 2.0f * ph)
                      + 0.2f * std::sin (2.0f * kPi * 1.73f * t + 2.1f + 3.0f * ph);
        flick[b] = 1.0f + 0.04f * n;
    }
    repaint();
}

//==============================================================================
KorEditor::Control KorEditor::controlAt (juce::Point<float> p) noexcept
{
    const float dx = p.x - kCx, dy = p.y - kCy;
    const float r = std::sqrt (dx * dx + dy * dy);
    float a = std::atan2 (dx, -dy);
    if (a < 0.0f) a += 2.0f * kPi;
    if (r > kUnitR + 22.0f) return none;
    if (r >= kBezelIn - 2.0f)
    {
        for (int i = 0; i < 3; ++i)
            if (std::abs (wrapPi (a - kPushA[i])) < 0.12f) return (Control) (push0 + i);
        if (r <= kUnitR + 2.0f && a >= kFaderA0 - 0.06f && a <= kFaderA1 + 0.06f) return mix;
        return none;
    }
    if (r <= kEmberR + 3.0f) return none;                         // the ember is a light, not a knob (panel v4)
    if (r < 0.5f * (kPosOut0 + ringInner (mid))) return high;
    if (r < 0.5f * (kPosOut1 + ringInner (low))) return mid;
    return low;
}

float KorEditor::value (int c) const noexcept
{
    return params[c]->convertFrom0to1 (params[c]->getValue());
}

void KorEditor::setValue (int c, float v)
{
    params[c]->setValueNotifyingHost (params[c]->convertTo0to1 (juce::jlimit (0.0f, 1.0f, v)));
}

int KorEditor::linkMode() const noexcept
{
    return juce::jlimit (0, 2, juce::roundToInt (linkParam->convertFrom0to1 (linkParam->getValue())));
}

void KorEditor::beginMove (int c)
{
    for (auto& m : moveBy) m = 0.0f;
    moveBy[c] = 1.0f;
    if (c >= low && c <= high)
    {
        const int mode = linkMode();
        if (mode == linkFollow) for (int b = 0; b < 3; ++b) moveBy[b] = linkShare (c, b);
        if (mode == linkGang) for (int b = 0; b < 3; ++b) moveBy[b] = 1.0f;
    }
    for (int k = 0; k < 4; ++k)
        if (moveBy[k] > 0.0f) { dragValue[k] = value (k); params[k]->beginChangeGesture(); }
}

void KorEditor::endMove()
{
    for (int k = 0; k < 4; ++k)
        if (moveBy[k] > 0.0f) { params[k]->endChangeGesture(); moveBy[k] = 0.0f; }
}

void KorEditor::mouseDown (const juce::MouseEvent& e)
{
    const auto c = controlAt (e.position);
    if (c == none) return;
    if (c >= push0)
    {
        const int mode = (int) c - (int) push0;
        if (mode != linkMode())
        {
            linkParam->beginChangeGesture();
            linkParam->setValueNotifyingHost (linkParam->convertTo0to1 ((float) mode));
            linkParam->endChangeGesture();
        }
        repaint();
        return;
    }
    active = c;
    lastY = e.position.y;
    beginMove (c);
    repaint();
}

void KorEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (active == none) return;
    const float dy = e.position.y - lastY;
    lastY = e.position.y;
    const float d = -dy / kDragPx * (e.mods.isShiftDown() ? 0.25f : 1.0f);
    for (int k = 0; k < 4; ++k)
        if (moveBy[k] > 0.0f)
        {
            dragValue[k] = juce::jlimit (0.0f, 1.0f, dragValue[k] + d * moveBy[k]);
            setValue (k, dragValue[k]);
        }
    repaint();
}

void KorEditor::mouseUp (const juce::MouseEvent&)
{
    if (active == none) return;
    endMove();
    active = none;
    repaint();
}

void KorEditor::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto c = controlAt (e.position);
    if (c == none || c >= push0) return;
    // Only the clicked control returns to its default; its followers stay where they are.
    const bool inGesture = moveBy[c] > 0.0f;   // the second click's mouseDown opened it; its mouseUp closes it
    if (! inGesture) params[c]->beginChangeGesture();
    params[c]->setValueNotifyingHost (params[c]->getDefaultValue());
    if (! inGesture) params[c]->endChangeGesture();
    if (inGesture) dragValue[c] = value (c);
    repaint();
}

void KorEditor::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (active != none) return;
    const auto c = controlAt (e.position);
    if (c == none || c >= push0) return;
    const float d = std::abs (w.deltaX) > std::abs (w.deltaY) ? -w.deltaX : w.deltaY;
    if (d == 0.0f) return;
    const float step = (d > 0.0f ? 0.01f : -0.01f) * (w.isReversed ? -1.0f : 1.0f);
    beginMove (c);
    for (int k = 0; k < 4; ++k)
        if (moveBy[k] > 0.0f) setValue (k, dragValue[k] + step * moveBy[k]);
    endMove();
    repaint();
}

//==============================================================================
void KorEditor::buildCurvedText (juce::Path& dest, const juce::String& text, float rMid, float em, float tracking) const
{
    // Letter-spaced caps set along the top of a ring, centred at 12 o'clock, cap height centred in the band.
    dest.clear();
    const auto font = Theme::legendEm (em);
    const float capH = 0.72f * em;
    const float rBase = rMid - capH * 0.5f;
    const int n = text.length();
    float total = 0.0f;
    for (int i = 0; i < n; ++i) total += juce::GlyphArrangement::getStringWidth (font, juce::String::charToString (text[i]));
    total += tracking * (float) juce::jmax (0, n - 1);
    float s = -0.5f * total;
    for (int i = 0; i < n; ++i)
    {
        const auto ch = juce::String::charToString (text[i]);
        const float w = juce::GlyphArrangement::getStringWidth (font, ch);
        const float theta = (s + 0.5f * w) / rMid;
        juce::GlyphArrangement ga;
        ga.addLineOfText (font, ch, 0.0f, 0.0f);
        juce::Path gp;
        ga.createPath (gp);
        dest.addPath (gp, juce::AffineTransform::translation (-0.5f * w, -rBase).rotated (theta).translated (kCx, kCy));
        s += w + tracking;
    }
}

void KorEditor::rebuildCaches (float scale)
{
    cacheScale = scale;
    const int pw = juce::roundToInt ((float) kW * scale), ph = juce::roundToInt ((float) kH * scale);

    staticLayer = juce::Image (juce::Image::ARGB, pw, ph, true);
    {
        juce::Graphics g (staticLayer);
        g.addTransform (juce::AffineTransform::scale (scale));
        g.fillAll (Theme::hostDark);

        // The unit's face.
        g.setColour (Theme::body);
        g.fillEllipse (kCx - kUnitR, kCy - kUnitR, 2.0f * kUnitR, 2.0f * kUnitR);

        // Brushed bezel: top-lit band with fine concentric hairlines.
        g.setGradientFill (juce::ColourGradient (Theme::bezelLight, kCx, kCy - kUnitR, Theme::bezelDark, kCx, kCy + kUnitR, false));
        g.fillPath (annulus (kBezelIn, kUnitR));
        for (int k = 0; k < 20; ++k)
        {
            const float h = hash01 (k + 7);
            g.setColour ((k % 2 == 0 ? juce::Colours::white : juce::Colours::black).withAlpha (0.015f + 0.035f * h));
            strokeCircle (g, kBezelIn + 0.25f + 0.5f * (float) k, 0.5f);
        }
        g.setColour (juce::Colours::black.withAlpha (0.7f));
        strokeCircle (g, kUnitR - 0.4f, 0.8f);
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        strokeCircle (g, kBezelIn - 1.2f, 1.5f);
        g.setColour (Theme::gold.withAlpha (0.35f));
        strokeCircle (g, kBezelIn, 1.5f);

        // The model line (gold silkscreen) at the top of the bezel, the SubverseLab badge (mark + wordmark) at its foot.
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.fillPath (modelLine, juce::AffineTransform::translation (0.0f, 0.7f));
        g.setColour (Theme::gold);
        g.fillPath (modelLine);
        if (wordmark.isValid() && mark.isValid())
        {
            const float wordH = 9.0f, markH = 13.0f, gap = 5.0f;
            const float ww = wordH * (float) wordmark.getWidth() / (float) wordmark.getHeight();
            const float mw = markH * (float) mark.getWidth() / (float) mark.getHeight();
            const float y = kCy + kBezelMid, x0 = kCx - 0.5f * (mw + gap + ww);
            g.setOpacity (0.9f);
            g.drawImage (mark, { x0, y - 0.5f * markH, mw, markH }, juce::RectanglePlacement::stretchToFit);
            g.drawImage (wordmark, { x0 + mw + gap, y - 0.5f * wordH, ww, wordH }, juce::RectanglePlacement::stretchToFit);
            g.setOpacity (1.0f);
        }

        // The fader slot: a groove cut into the bezel on the left, its outer lip lit, five scale ticks outside it,
        // DRY at the bottom end and WET at the top.
        {
            juce::Path slot;
            slot.addCentredArc (kCx, kCy, kBezelMid, kBezelMid, 0.0f, kFaderA0 - 0.03f, kFaderA1 + 0.03f, true);
            g.setColour (juce::Colours::black.withAlpha (0.85f));
            g.strokePath (slot, juce::PathStrokeType (6.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            juce::Path lip;
            lip.addCentredArc (kCx, kCy, kBezelMid - 3.6f, kBezelMid - 3.6f, 0.0f, kFaderA0 - 0.02f, kFaderA1 + 0.02f, true);
            g.setColour (juce::Colours::white.withAlpha (0.12f));
            g.strokePath (lip, juce::PathStrokeType (0.8f));
            for (int k = 0; k <= 4; ++k)
            {
                const float a = kFaderA0 + (kFaderA1 - kFaderA0) * 0.25f * (float) k;
                g.setColour (Theme::ink.withAlpha (k == 0 || k == 4 ? 0.6f : 0.35f));
                g.drawLine ({ polar (a, kBezelMid + 5.5f), polar (a, kUnitR - 2.5f) }, 0.9f);
            }
            for (auto* label : { &dryLabel, &wetLabel })
            {
                g.setColour (juce::Colours::black.withAlpha (0.55f));
                g.fillPath (*label, juce::AffineTransform::translation (0.0f, 0.6f));
                g.setColour (Theme::ink.withAlpha (0.8f));
                g.fillPath (*label);
            }
        }

        // The three rings: brushed steel. Anisotropic sheen (two light lobes at 10:30 and 4:30, as a top-left light on
        // concentric brushing), fine concentric hairlines, a bevel on each edge, knurl ticks on the outer edge.
        for (int b = 0; b < 3; ++b)
        {
            const float rIn = ringInner (b), rOut = ringOuter (b), rMid = 0.5f * (rIn + rOut);
            constexpr int kSeg = 360;
            for (int i = 0; i < kSeg; ++i)
            {
                const float a0 = 2.0f * kPi * (float) i / (float) kSeg, a1 = 2.0f * kPi * (float) (i + 1) / (float) kSeg + 0.004f;
                const float am = 0.5f * (a0 + a1);
                const float sheen = 0.5f + 0.5f * std::cos (2.0f * (am + 0.25f * kPi));       // peaks at 10:30 and 4:30
                const float top = 0.5f + 0.5f * std::cos (am + 0.25f * kPi);                 // the 10:30 lobe a little brighter
                const float t = juce::jlimit (0.0f, 1.0f, 0.18f + 0.62f * std::pow (sheen, 1.6f) + 0.18f * top);
                juce::Path p;
                p.addCentredArc (kCx, kCy, rMid, rMid, 0.0f, a0, a1, true);
                g.setColour (Theme::steelDark.interpolatedWith (Theme::steelLight, t));
                g.strokePath (p, juce::PathStrokeType (ringWidth (b), juce::PathStrokeType::curved, juce::PathStrokeType::butt));
            }
            for (int k = 0; k < (int) ringWidth (b) * 2; ++k)                              // brushing hairlines
            {
                const float h = hash01 (k + 31 * b);
                g.setColour ((h > 0.5f ? juce::Colours::white : juce::Colours::black).withAlpha (0.025f + 0.05f * std::abs (h - 0.5f)));
                strokeCircle (g, rIn + 0.5f * (float) k + 0.25f, 0.5f);
            }
            // Bevels: light on the outer top edge, shadow on the inner top edge, and the reverse at the bottom.
            g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.55f), kCx, kCy - rOut,
                                                     juce::Colours::black.withAlpha (0.45f), kCx, kCy + rOut, false));
            strokeCircle (g, rOut - 0.75f, 1.5f);
            g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.5f), kCx, kCy - rIn,
                                                     juce::Colours::white.withAlpha (0.35f), kCx, kCy + rIn, false));
            strokeCircle (g, rIn + 0.75f, 1.5f);
            // Knurl: short ticks every 4 degrees along the outer edge.
            for (int k = 0; k < 90; ++k)
            {
                const float a = 2.0f * kPi * (float) k / 90.0f;
                g.setColour (Theme::steelEdge.withAlpha (0.55f));
                g.drawLine ({ polar (a, rOut - 5.0f), polar (a, rOut - 1.5f) }, 1.0f);
            }
            g.setColour (Theme::steelEdge);
            strokeCircle (g, rIn - 0.5f, 1.0f);
            strokeCircle (g, rOut + 0.5f, 1.0f);

            // Dome (owner, 8 Oct: "ortadan bombeli"): each ring's face slopes down towards its outer edge, lighter at
            // the inner edge and darker at the outer one, so the three rings step up towards the ember.
            juce::ColourGradient slope (juce::Colours::white.withAlpha (0.16f), kCx, kCy, juce::Colours::black.withAlpha (0.24f), kCx + rOut, kCy, true);
            slope.addColour ((double) (rIn / rOut), juce::Colours::white.withAlpha (0.16f));
            slope.addColour ((double) (rMid / rOut), juce::Colours::transparentBlack);
            g.setGradientFill (slope);
            g.fillPath (annulus (rIn, rOut));
        }
        // The step shadows: each inner ring sits higher and casts a soft shadow onto the face just outside it.
        for (int b : { (int) high, (int) mid })
        {
            const float r = ringOuter (b) + 1.5f;
            for (int k = 0; k < 4; ++k)
            {
                g.setColour (juce::Colours::black.withAlpha (0.16f - 0.035f * (float) k));
                g.drawEllipse (kCx - r - (float) k, kCy - r - (float) k + 1.5f, 2.0f * (r + (float) k), 2.0f * (r + (float) k), 1.0f);
            }
        }

        // Ring labels, engraved at 12 o'clock (under the pointers, which stand on the steel).
        for (int b = 0; b < 3; ++b)
        {
            g.setColour (juce::Colours::white.withAlpha (0.45f));
            g.fillPath (labelPath[b], juce::AffineTransform::translation (0.0f, 0.7f));
            g.setColour (Theme::engrave);
            g.fillPath (labelPath[b]);
        }

        // The ember's well.
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        strokeCircle (g, kEmberR + 1.5f, 3.0f);
    }

    // The coal's crust: Voronoi cells (fixed seeds), a dark crust over each cell that thins towards its edges, clear
    // in the cracks between cells, so the glow underneath shows most through the cracks, as in a real ember.
    {
        const int d = juce::roundToInt (2.0f * kEmberR * scale);
        coalTex = juce::Image (juce::Image::ARGB, d, d, true);
        constexpr int kSeeds = 70;
        float sx[kSeeds], sy[kSeeds], sv[kSeeds];
        for (int k = 0; k < kSeeds; ++k)
        {
            // Seeds spread evenly over the disc (sunflower spiral), jittered.
            const float rr = std::sqrt (((float) k + 0.5f) / (float) kSeeds) * 1.05f;
            const float th = (float) k * 2.39996f + 0.6f * hash01 (k + 101);
            sx[k] = rr * std::cos (th) + 0.06f * (hash01 (k + 211) - 0.5f);
            sy[k] = rr * std::sin (th) + 0.06f * (hash01 (k + 307) - 0.5f);
            sv[k] = hash01 (k + 401);
        }
        juce::Image::BitmapData px (coalTex, juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < d; ++y)
            for (int x = 0; x < d; ++x)
            {
                const float u = ((float) x + 0.5f) / (float) d * 2.0f - 1.0f, v = ((float) y + 0.5f) / (float) d * 2.0f - 1.0f;
                if (u * u + v * v > 1.0f) continue;
                float d1 = 1e9f, d2 = 1e9f; int n1 = 0;
                for (int k = 0; k < kSeeds; ++k)
                {
                    const float e = (u - sx[k]) * (u - sx[k]) + (v - sy[k]) * (v - sy[k]);
                    if (e < d1) { d2 = d1; d1 = e; n1 = k; } else if (e < d2) d2 = e;
                }
                const float edge = std::sqrt (d2) - std::sqrt (d1);              // 0 on a crack
                const float crust = juce::jlimit (0.0f, 1.0f, (edge - 0.012f) / 0.07f);
                const float a = crust * (0.30f + 0.22f * sv[n1]);
                px.setPixelColour (x, y, juce::Colour::fromFloatRGBA (0.02f, 0.0f, 0.01f, a));
            }
    }
}

//==============================================================================
void KorEditor::paint (juce::Graphics& g)
{
    const float scale = g.getInternalContext().getPhysicalPixelScaleFactor();
    if (std::abs (scale - cacheScale) > 1.0e-3f) rebuildCaches (scale);
    const auto toLogical = juce::AffineTransform::scale (1.0f / cacheScale);

    g.drawImageTransformed (staticLayer, toLogical);
    for (int b = 0; b < 3; ++b) paintPointer (g, b);
    paintFader (g);
    paintPushers (g);
    paintEmber (g);
}

void KorEditor::paintPointer (juce::Graphics& g, int b)
{
    // The raised black-steel bar: a soft shadow cast down and to the right (fixed light at the top left, whatever the
    // angle), the bar lit across its bounds, the side face towards the light bright and the other dark, a cream inlay.
    const float a = kSpan * value (b);
    const auto rot = juce::AffineTransform::rotation (kStart + a, kCx, kCy);
    for (int k = 3; k >= 1; --k)
    {
        g.setColour (juce::Colours::black.withAlpha (0.14f));
        g.fillPath (pointer[b], rot.translated (0.55f * (float) k, 0.8f * (float) k));
    }
    g.setGradientFill (litFrom (pointer[b].getBoundsTransformed (rot), Theme::blackSteelLight, Theme::blackSteelDark));
    g.fillPath (pointer[b], rot);
    // Side 0's outward normal is (-1, 0) at 12 o'clock; rotated by a it is (-cos a, -sin a). Light from the top left.
    const float facing = (std::cos (a) + std::sin (a)) * 0.7071f;
    g.setColour (juce::Colours::white.withAlpha (0.42f * juce::jmax (0.0f, facing)));
    g.fillPath (pointerSide[b][0], rot);
    g.setColour (juce::Colours::white.withAlpha (0.42f * juce::jmax (0.0f, -facing)));
    g.fillPath (pointerSide[b][1], rot);
    g.setColour (juce::Colours::black.withAlpha (0.7f));
    g.strokePath (pointer[b], juce::PathStrokeType (0.6f), rot);
    g.setColour (Theme::ink.withAlpha (0.9f));
    g.fillPath (pointerInlay[b], rot);
}

void KorEditor::paintFader (juce::Graphics& g)
{
    // The fader cap rides in the slot: a small brushed-steel block across the bezel, grip ridges, a cream index line.
    const float a = kFaderA0 + (kFaderA1 - kFaderA0) * value (mix);
    const auto rot = juce::AffineTransform::rotation (a, kCx, kCy);
    const float top = kCy - (kUnitR - 1.5f), h = kUnitR - kBezelIn - 3.0f;
    juce::Path cap;
    cap.addRoundedRectangle (kCx - 6.5f, top, 13.0f, h, 2.5f);
    for (int k = 3; k >= 1; --k)
    {
        g.setColour (juce::Colours::black.withAlpha (0.16f));
        g.fillPath (cap, rot.translated (0.5f * (float) k, 0.8f * (float) k));
    }
    g.setGradientFill (litFrom (cap.getBoundsTransformed (rot), Theme::steelLight, Theme::steelDark));
    g.fillPath (cap, rot);
    for (float x : { -4.0f, 4.0f })
    {
        g.setColour (Theme::steelEdge.withAlpha (0.7f));
        g.drawLine (xf (juce::Line<float> (kCx + x, top + 3.0f, kCx + x, top + h - 3.0f), rot), 0.8f);
    }
    g.setColour (juce::Colours::white.withAlpha (0.35f));
    g.strokePath (cap, juce::PathStrokeType (0.7f), rot.translated (-0.3f, -0.3f));
    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.strokePath (cap, juce::PathStrokeType (0.6f), rot);
    g.setColour (Theme::ink);
    g.drawLine (xf (juce::Line<float> (kCx, top + 2.0f, kCx, top + h - 2.0f), rot), 1.4f);
}

void KorEditor::paintPushers (juce::Graphics& g)
{
    // Three pushers on the case at 2, 3 and 4 o'clock, as on a chronograph: a collar on the case, a stem, a steel cap.
    // The chosen one is pushed in 4 px and its legend is gold; the others stand out and their legends are cream.
    const int mode = linkMode();
    for (int i = 0; i < 3; ++i)
    {
        const auto rot = juce::AffineTransform::rotation (kPushA[i], kCx, kCy);
        const float depth = i == mode ? 4.0f : 0.0f;
        juce::Path collar, stem, cap;
        collar.addRoundedRectangle (kCx - 11.0f, kCy - (kUnitR + 3.5f), 22.0f, 5.5f, 2.0f);
        stem.addRectangle (kCx - 5.0f, kCy - (kUnitR + 8.0f - depth), 10.0f, 5.5f - depth);
        cap.addRoundedRectangle (kCx - 10.0f, kCy - (kUnitR + 20.0f - depth), 20.0f, 13.0f, 4.0f);
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
        // Grip lines across the cap's end.
        for (float x : { -6.0f, -3.0f, 0.0f, 3.0f, 6.0f })
        {
            g.setColour (Theme::steelEdge.withAlpha (0.45f));
            g.drawLine (xf (juce::Line<float> (kCx + x, kCy - (kUnitR + 18.0f - depth), kCx + x, kCy - (kUnitR + 9.5f - depth)), rot), 0.7f);
        }
        g.setColour (juce::Colours::white.withAlpha (0.4f));
        g.strokePath (cap, juce::PathStrokeType (0.7f), rot.translated (-0.3f, -0.3f));
        g.setColour (juce::Colours::black.withAlpha (0.65f));
        g.strokePath (cap, juce::PathStrokeType (0.6f), rot);
        g.strokePath (collar, juce::PathStrokeType (0.5f), rot);

        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.fillPath (pushLabel[i], juce::AffineTransform::translation (0.0f, 0.6f));
        g.setColour (i == mode ? Theme::gold : Theme::ink.withAlpha (0.55f));
        g.fillPath (pushLabel[i]);
    }
}

void KorEditor::paintEmber (juce::Graphics& g)
{
    const float lit = value (mix);
    const juce::Rectangle<float> disc (kCx - kEmberR, kCy - kEmberR, 2.0f * kEmberR, 2.0f * kEmberR);
    // Ignition when the panel opens: the ember comes up over 0.9 s (smoothstep).
    const float t0 = juce::jlimit (0.0f, 1.0f, (float) clock / 0.9f);
    const float ignite = t0 * t0 * (3.0f - 2.0f * t0);

    // The coal: dark and unlit at mix 0, burgundy at mix 100.
    coalGrad.setColour (0, Theme::coalCentre.interpolatedWith (Theme::litCentre, lit * ignite));
    coalGrad.setColour (1, Theme::coalEdge.interpolatedWith (Theme::litEdge, lit * ignite));
    g.setGradientFill (coalGrad);
    g.fillEllipse (disc);

    // Three zones, LOW (the edge) first so the inner ones lie on top.
    // Zones follow the rings (owner, 8 Oct): HIGH = the centre, MID = the middle ring, LOW = the edge. Each shows its
    // band's heat: at rest 80 % of its ring's setting (so the spread is there with no audio) or the measured saturation
    // when the music drives the band (fast attack, slow release: the ember breathes with the programme); a ring being
    // turned (with LINK or GANG its followers too) shows its full setting and the others go dark. DRY/WET dims it all.
    const float sat = 0.3f + 0.7f * lit;
    const bool turningRing = active >= low && active <= high;
    float heatShown[3];
    for (int b = 0; b < 3; ++b)
    {
        heatShown[b] = turningRing ? (moveBy[b] > 0.0f ? juce::jmax (glow[b], value (b)) : -1.0f)
                                   : juce::jmax (glow[b], 0.8f * value (b));
        if (heatShown[b] < 0.0f) continue;
        const float h = heatShown[b];
        const float intensity = ignite * lit * (0.08f + 0.92f * std::pow (h, 0.75f)) * flick[b];
        const auto col = Theme::heat (h).withMultipliedSaturation (sat).withMultipliedBrightness (flick[b]);
        auto& gr = emberGrad[b];
        gr.point2 = { kCx + kEmberR, kCy };
        for (int s = 0; s < 5; ++s)
            gr.setColour (s, col.withAlpha (juce::jlimit (0.0f, 1.0f, intensity * kGlowProfile[b][s].w)));
        g.setGradientFill (gr);
        g.fillEllipse (disc);
    }

    // The crust over the glow, then the dome's inner shading.
    if (coalTex.isValid())
        g.drawImage (coalTex, disc, juce::RectanglePlacement::stretchToFit);

    // The name, INSIDE the glass dome (owner, 8 Oct): magnified by the lens (throughDome), lit from below by the ember,
    // a little translucent, and floating above the coal: its shadow falls well below and to the right, soft.
    {
        const juce::Path& title = active == none ? namePath : bandName[active];
        float warm = 0.0f;
        for (int b = 0; b < 3; ++b) warm = juce::jmax (warm, juce::jmax (glow[b], value (b)));
        warm *= lit * ignite;
        for (int k = 5; k >= 1; --k)
        {
            g.setColour (juce::Colours::black.withAlpha (0.075f));
            g.fillPath (title, juce::AffineTransform::scale (1.0f + 0.006f * (float) k, 1.0f + 0.006f * (float) k, kCx, kCy)
                                   .translated (1.2f + 0.45f * (float) k, 2.6f + 0.6f * (float) k));
        }
        // Light from the ember scattering in the glass around the letters.
        const auto halo = Theme::heat3.withAlpha (0.10f * warm);
        g.setColour (halo);
        g.strokePath (title, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        // Refraction: the glass throws a faint warm ghost of each letter down and to the right.
        g.setColour (Theme::heat3.withAlpha (0.16f + 0.10f * warm));
        g.fillPath (title, juce::AffineTransform::translation (0.8f, 0.9f));
        g.setColour (Theme::ink.interpolatedWith (Theme::heat3, 0.30f * warm).withAlpha (0.80f));
        g.fillPath (title);
        if (active != none)
        {
            g.setFont (Theme::legendEm (11.0f));
            g.setColour (Theme::ink.withAlpha (0.8f));
            g.drawText (pct (value (active)), juce::Rectangle<float> (kCx - 50.0f, kCy + 15.0f, 100.0f, 16.0f), juce::Justification::centred, false);
        }
    }

    g.setGradientFill (rimGrad);
    g.fillEllipse (disc);

    // The glass. A broad soft sheen across the whole dome from the top left (it passes over the letters, which puts
    // them under the glass), the window reflection as a crescent high on the left with a small hot spot, the ember's
    // own light caught low on the right of the rim, and a thin meniscus where the glass meets the well.
    {
        juce::ColourGradient sheen (juce::Colours::transparentWhite, kCx - kEmberR, kCy - kEmberR,
                                    juce::Colours::transparentWhite, kCx + kEmberR, kCy + kEmberR, false);
        sheen.addColour (0.30, juce::Colours::white.withAlpha (0.10f));
        sheen.addColour (0.47, juce::Colours::white.withAlpha (0.035f));
        sheen.addColour (0.60, juce::Colours::transparentWhite);
        g.setGradientFill (sheen);
        g.fillEllipse (disc);

        juce::Path crescent;
        crescent.addCentredArc (kCx, kCy, kEmberR - 9.0f, kEmberR - 9.0f, 0.0f, 1.58f * kPi, 1.97f * kPi, true);
        g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.30f), kCx - 0.6f * kEmberR, kCy - 0.7f * kEmberR,
                                                 juce::Colours::transparentWhite, kCx, kCy - 0.1f * kEmberR, false));
        g.strokePath (crescent, juce::PathStrokeType (9.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (juce::Colours::white.withAlpha (0.55f));
        g.fillEllipse (kCx - 0.52f * kEmberR - 4.5f, kCy - 0.58f * kEmberR - 2.5f, 9.0f, 5.0f);

        juce::Path caustic;
        caustic.addCentredArc (kCx, kCy, kEmberR - 5.0f, kEmberR - 5.0f, 0.0f, 0.55f * kPi, 0.95f * kPi, true);
        float hot = 0.0f;
        for (int b = 0; b < 3; ++b) hot = juce::jmax (hot, heatShown[b]);
        g.setColour (Theme::heat (hot).withAlpha (0.22f * lit * ignite));
        g.strokePath (caustic, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        juce::Path meniscus;
        meniscus.addCentredArc (kCx, kCy, kEmberR - 1.0f, kEmberR - 1.0f, 0.0f, 1.35f * kPi, 2.15f * kPi, true);
        g.setColour (juce::Colours::white.withAlpha (0.16f));
        g.strokePath (meniscus, juce::PathStrokeType (1.0f));
    }
}
