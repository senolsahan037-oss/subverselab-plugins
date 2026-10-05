#include "PluginEditor.h"
#include "Theme.h"
#include "BinaryData.h"

using namespace kit;

namespace
{
const juce::StringArray kSyncLabels { "1/16", "1/8", "1/8.", "1/4", "1/4.", "1/2", "FREE" };
const std::vector<float> kSyncAngles { -130.0f, -87.0f, -43.0f, 0.0f, 43.0f, 87.0f, 130.0f };
constexpr float kKnobY = 745.0f;
constexpr float kSwellCx = 1401.0f;
const Colour kPhos = rgb (157, 247, 221), kTeal = rgb (99, 242, 216), kAmber = rgb (236, 184, 96), kRust = rgb (196, 98, 58),
             kDimText = rgb (92, 168, 152), kGuide = rgb (30, 90, 80);

double nowSeconds() { return juce::Time::getMillisecondCounterHiRes() * 0.001; }

float raw (YankiProcessor& p, const char* id) { return p.apvts.getRawParameterValue (id)->load(); }

ringmodel::Params modelParamsFor (YankiProcessor& p)
{
    ringmodel::Params m;
    m.mood = juce::jlimit (0, 6, juce::roundToInt (raw (p, yankiIds::mood)));
    m.heads = juce::jlimit (0, 3, juce::roundToInt (raw (p, yankiIds::heads)));
    m.bpm = p.uiBpm.load();
    m.timeS = p.resolvedMs (m.bpm) * 0.001;
    m.mix = YankiProcessor::mixToEngine (raw (p, yankiIds::mix));
    m.fbKnob = raw (p, yankiIds::feedback);
    m.tone = raw (p, yankiIds::tone);
    m.wear = raw (p, yankiIds::wear);
    m.spread = raw (p, yankiIds::spread);
    m.capOn = raw (p, yankiIds::capOn) > 0.5f;
    m.capDb = raw (p, yankiIds::capDb);
    m.lockBars = (int) YankiProcessor::kLockBars[juce::jlimit (0, 3, juce::roundToInt (raw (p, yankiIds::lockBars)))];
    m.riseMax = YankiProcessor::kRiseMax[juce::jlimit (0, 2, juce::roundToInt (raw (p, yankiIds::riseCurve)))];
    return m;
}

/** Draws the static print once per pixel scale into an image and blits it. */
void paintCached (juce::Graphics& g, juce::Image& cache, float& cacheScale, int w, int h, const std::function<void (juce::Image&, float)>& build)
{
    const float s = juce::jlimit (0.25f, 4.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
    if (cache.isNull() || std::abs (s - cacheScale) > 0.05f)
    {
        cache = juce::Image (juce::Image::RGB, juce::jmax (1, juce::roundToInt ((float) w * s)), juce::jmax (1, juce::roundToInt ((float) h * s)), false);
        build (cache, s);
        cacheScale = s;
    }
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    g.drawImageTransformed (cache, juce::AffineTransform::scale ((float) w / (float) cache.getWidth(), (float) h / (float) cache.getHeight()));
}

void drawLogo (juce::Graphics& g, const juce::Image& img, float x, float y, float h, bool rightAnchor, float& widthOut)
{
    if (img.isNull())
    {
        widthOut = 0.0f;
        return;
    }
    const float w = (float) img.getWidth() * h / (float) img.getHeight();
    widthOut = w;
    g.setOpacity (1.0f);
    g.drawImage (img, { rightAnchor ? x - w : x, y - h * 0.5f, w, h }, juce::RectanglePlacement::stretchToFit);
}

void chip (juce::Graphics& g, float x, float y, const juce::String& t, float size, Colour col)
{
    const float w = tlen (t, Face::mono, size, 0.5f);
    rrect (g, { x - 4, y - size * 0.8f, x + w + 4, y + size * 0.8f }, 3, rgb (1, 14, 12, 225));
    text (g, x, y, t, Face::mono, size, col, "lm", 0.5f);
}

// ---- the static print of the front ------------------------------------------------------------
void frontStatic (juce::Graphics& g, const juce::Image& mark, const juce::Image& wmark)
{
    // frame
    g.setColour (GOLD_D);
    g.drawRect (juce::Rectangle<float> (0, 0, 1600, 900), 3.0f);
    g.setColour (GOLD_L.withAlpha ((juce::uint8) 150));
    g.drawRect (juce::Rectangle<float> (1.5f, 1.5f, 1597, 897), 1.0f);
    for (auto [x, y] : { std::pair<float, float> { 24, 24 }, { 1576, 24 }, { 24, 876 }, { 1576, 876 } })
        screw (g, x, y, 8.5f);
    wordmark (g, "YANKI", 72, 34, 46, CREAM);
    lt (g, 74, 104, U ("ECHO · MODEL YK-74"), 10.0f, 3.0f, Face::legend, "lm");

    // BPM window, host lamp, bypass legend
    text (g, 1220, 54, "BPM", Face::legend, 8, CREAM, "lm", 1.8f);
    rrect (g, { 1252, 36, 1352, 82 }, 3, rgb (12, 12, 12));
    rrect (g, { 1254, 38, 1350, 80 }, 2, rgb (28, 6, 5));
    text (g, 1230, 74.5f, "HOST", Face::legend, 7, CREAM, "lm", 1.2f);
    text (g, 1405, 106, "BYPASS", Face::legend, 7.5f, CREAM, "mm", 1.4f);

    // the echo field's glass frame (the screen itself is live)
    {
        const auto b = EchoField::kBox;
        soft (g, b.expanded (8.0f), 5.0f, juce::Colours::black, 0.65f, { 1.0f, 5.0f }, false, 14.0f);
        grad (g, b.expanded (9.0f), rgb (36, 40, 39), rgb (4, 5, 5), 90.0f, false, 12.0f);
    }

    // WET, the hero
    bracket (g, 72, 330, 150, "WET");
    knobScale (g, 200, 340, 76, { "DRY", "", "WET" }, true);
    lt (g, 200, 340 + 76 * 1.62f + 14, "WET", 17.0f, 5.0f);

    // SWELL
    bracket (g, 1272, 1530, 150, "SWELL");
    knobScale (g, kSwellCx - 50, 280, 30, { "SLOW", "", "FAST" }, true, {}, 9.5f);
    lt (g, kSwellCx - 50, 339, "RISE", 9.5f, 2.4f);
    knobScale (g, kSwellCx + 50, 280, 30, {}, true);
    knobMarks (g, kSwellCx + 50, 280, 30, { { -135.0f, "OFF" }, { CapKnob::angleForDb (0), "0" }, { 135.0f, "+12" } }, 8.0f);
    lt (g, kSwellCx + 50, 339, "CAP", 9.5f, 2.4f);
    lt (g, kSwellCx, 402, "END", 8.5f, 2.0f);
    bracket (g, 1272, 1530, 470, "TEMPO LOCK");
    lt (g, kSwellCx, 536, "BARS", 8.5f, 2.0f);
    knobScale (g, kSwellCx - 50, 592, 24, { "0", "", "10" }, true);
    lt (g, kSwellCx - 50, 592 + 24 * 1.98f + 4, "DARKEN", 9.0f, 2.2f);
    knobScale (g, kSwellCx + 50, 592, 24, { U ("−10"), "0", "+10" }, true);
    lt (g, kSwellCx + 50, 592 + 24 * 1.98f + 4, "FINE", 9.0f, 2.2f);

    groove (g, 668);

    // HEADS (under the WET hero, left of the TIME ring)
    bracket (g, 72, 330, 722, "HEADS");

    // TIME: the SYNC ring around the FREE-ms cap
    {
        const float cx = 432, cy = kKnobY, R = 46;
        pointerScale (g, cx, cy, R, kSyncLabels, kSyncAngles, 7.6f, 1.58f);
        lt (g, cx, cy + 68.0f, "TIME", 10.0f, 2.2f);
    }
    // FEEDBACK (0 .. HOLD at 12 o'clock .. MAX; the arc past HOLD is the swell)
    {
        const float cx = 600, cy = kKnobY, r = 33;
        juce::Path arc;
        for (int i = 0; i < 40; ++i)
        {
            const auto pt = polar (cx, cy, r * 1.50f, 2.0f + 133.0f * (float) i / 39.0f);
            if (i == 0) arc.startNewSubPath (pt); else arc.lineTo (pt);
        }
        g.setColour (SWELL_RED);
        g.strokePath (arc, juce::PathStrokeType (2.8f));
        knobScale (g, cx, cy, r, {}, true);
        knobMarks (g, cx, cy, r, { { -135.0f, "0" }, { 135.0f, "MAX" } }, 8.0f);
        line (g, polar (cx, cy, r * 1.18f, 0), polar (cx, cy, r * 1.62f, 0), 2.0f, CREAM);
        lt (g, cx, kKnobY + 62, "FEEDBACK", 10.0f, 2.2f);
    }
    knobScale (g, 750, kKnobY, 33, { "1k", "", "8k" }, true);
    lt (g, 750, kKnobY + 62, "TONE", 10.0f, 2.2f);
    knobScale (g, 890, kKnobY, 33, { "0", "5", "10" }, true);
    lt (g, 890, kKnobY + 62, "WEAR", 10.0f, 2.2f);
    knobScale (g, 1030, kKnobY, 33, { "MONO", "", "WIDE" }, true);
    lt (g, 1030, kKnobY + 62, "SPREAD", 10.0f, 2.2f);
    knobScale (g, 1170, kKnobY, 33, { U ("−12"), "0", "+12" }, true);
    lt (g, 1170, kKnobY + 62, "OUTPUT", 10.0f, 2.2f);

    // footer
    const auto t = U ("YANKI · MODEL YK-74 · ECHO");
    const float tw = tlen (t, Face::legend, 9.5f, 2.6f);
    rrect (g, { 800 - tw / 2 - 18, 857, 800 + tw / 2 + 18, 883 }, 1, {}, GOLD, 1.3f);
    text (g, 800, 870.5f, t, Face::legend, 9.5f, GOLD, "mm", 2.6f);
    lt (g, 72, 871, "MEASURED, NOT GUESSED", 7.5f, 1.8f, Face::fine, "lm", MUTED);
    float wlen = 0.0f, mlen = 0.0f;
    drawLogo (g, wmark, 1530, 871, 17, true, wlen);
    drawLogo (g, mark, 1530 - wlen - 8, 870, 26, true, mlen);
}
} // namespace

//==============================================================================
KnobBase::KnobBase (juce::Point<float> centre, float radius, float hitRadius, float margin)
    : c (centre), r (radius), hitR (hitRadius)
{
    const float half = radius + margin;
    setBounds (juce::Rectangle<float> (centre.x - half, centre.y - half, half * 2.0f, half * 2.0f).getSmallestIntegerContainer());
    setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
}

bool KnobBase::hitTest (int x, int y)
{
    const float d = juce::Point<float> ((float) (x + getX()), (float) (y + getY())).getDistanceFrom (c);
    return d <= hitR && d >= hole;
}

void KnobBase::mouseDown (const juce::MouseEvent& e)
{
    startU = getU();
    startY = e.position.y;
    begin();
}

void KnobBase::mouseDrag (const juce::MouseEvent& e)
{
    const float px = e.mods.isShiftDown() ? dragPixels() * 5.0f : dragPixels();
    setU (juce::jlimit (0.0f, 1.0f, startU + (startY - e.position.y) / px));
    refresh();
}

void KnobBase::mouseUp (const juce::MouseEvent&) { end(); }

void KnobBase::mouseDoubleClick (const juce::MouseEvent&)
{
    begin();
    setU (defaultU());
    end();
    refresh();
}

void KnobBase::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    begin();
    setU (juce::jlimit (0.0f, 1.0f, getU() + (w.isReversed ? -w.deltaY : w.deltaY) * 0.06f));
    end();
    refresh();
}

void KnobBase::refresh()
{
    const float u = getU();
    if (std::abs (u - lastU) > 1.0e-7f)
    {
        lastU = u;
        repaint();
    }
}

//==============================================================================
ParamKnob::ParamKnob (juce::RangedAudioParameter& p, juce::Point<float> centre, float radius, bool isPointer, std::vector<float> a)
    : KnobBase (centre, radius, radius * 1.08f, radius * 0.5f), param (p), pointer (isPointer), angles (std::move (a))
{
}

void ParamKnob::paintPanel (juce::Graphics& g)
{
    if (pointer)
    {
        const int idx = juce::jlimit (0, (int) angles.size() - 1, juce::roundToInt (param.convertFrom0to1 (param.getValue())));
        pointerBody (g, c.x, c.y, r, angles[(size_t) idx]);
    }
    else
        knobBody (g, c.x, c.y, r, param.getValue());
}

//==============================================================================
CapKnob::CapKnob (juce::RangedAudioParameter& onParam, juce::RangedAudioParameter& dbParam, juce::Point<float> centre, float radius)
    : KnobBase (centre, radius, radius * 1.08f, radius * 0.5f), on (onParam), db (dbParam)
{
}

float CapKnob::getU() const
{
    if (on.getValue() < 0.5f)
        return 0.0f;
    return (angleForDb (db.convertFrom0to1 (db.getValue())) + 135.0f) / 270.0f;
}

void CapKnob::setU (float u)
{
    const float angle = -135.0f + 270.0f * juce::jlimit (0.0f, 1.0f, u);
    if (angle < (-135.0f + angleForDb (-12.0f)) * 0.5f)
    {
        on.setValueNotifyingHost (0.0f);
        return;
    }
    const float d = juce::jlimit (-12.0f, 12.0f, -12.0f + 24.0f * (angle + 100.0f) / 235.0f);
    on.setValueNotifyingHost (1.0f);
    db.setValueNotifyingHost (db.convertTo0to1 (d));
}

void CapKnob::paintPanel (juce::Graphics& g) { knobBody (g, c.x, c.y, r, getU()); }

//==============================================================================
Key::Key (Box b, juce::String l, KeyStyle s, float sz, float tr)
    : box (b), label (std::move (l)), style (s), size (sz), track (tr)
{
    setBounds (b.expanded (26.0f).rect().getSmallestIntegerContainer());
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

bool Key::hitTest (int x, int y)
{
    return box.expanded (2.0f).rect().contains ((float) (x + getX()), (float) (y + getY()));
}

void Key::paintPanel (juce::Graphics& g)
{
    const bool lit = isLit ? isLit() : false;
    if (down)
        g.addTransform (juce::AffineTransform::translation (0.0f, 0.8f));
    button (g, box, label, lit, style, size, track);
}

void Key::mouseDown (const juce::MouseEvent&)
{
    down = true;
    if (onPress) onPress();
    repaint();
}

void Key::mouseUp (const juce::MouseEvent&)
{
    down = false;
    if (onRelease) onRelease();
    repaint();
}

void Key::refresh()
{
    const bool lit = isLit ? isLit() : false;
    if (lit != lastLit)
    {
        lastLit = lit;
        repaint();
    }
}

//==============================================================================
BypassToggle::BypassToggle (juce::RangedAudioParameter& p, float x, float y, float scale) : param (p), cx (x), cy (y), s (scale)
{
    setBounds (juce::Rectangle<float> (cx - 40 * s, cy - 50 * s, 80 * s, 100 * s).getSmallestIntegerContainer());
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void BypassToggle::paintPanel (juce::Graphics& g) { toggle (g, cx, cy, param.getValue() < 0.5f, s); }

void BypassToggle::mouseDown (const juce::MouseEvent&)
{
    param.beginChangeGesture();
    param.setValueNotifyingHost (param.getValue() < 0.5f ? 1.0f : 0.0f);
    param.endChangeGesture();
    refresh();
}

void BypassToggle::refresh()
{
    const bool b = param.getValue() >= 0.5f;
    if (b != last)
    {
        last = b;
        repaint();
    }
}

//==============================================================================
ValueLabel::ValueLabel (float x, float y, float width, std::function<juce::String()> f, float sz, juce::Colour col)
    : cx (x), cy (y), size (sz), colour (col), getText (std::move (f))
{
    setBounds (juce::Rectangle<float> (cx - width / 2, cy - 10, width, 20).getSmallestIntegerContainer());
    setInterceptsMouseClicks (false, false);
}

void ValueLabel::paintPanel (juce::Graphics& g) { text (g, cx, cy, getText(), Face::mono, size, colour, "mm", 0.5f); }

void ValueLabel::refresh()
{
    const auto t = getText();
    if (t != shown)
    {
        shown = t;
        repaint();
    }
}

//==============================================================================
PresetBar::PresetBar (YankiProcessor& p) : proc (p)
{
    setBounds (juce::Rectangle<float> (kX0 - 6, kY - 6, kX1 - kX0 + 12, 70).getSmallestIntegerContainer());
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

bool PresetBar::hitTest (int x, int y)
{
    const float fx = (float) (x + getX()), fy = (float) (y + getY());
    return fx >= kX0 && fx <= kX1 && fy >= kY && fy <= kY + 56;
}

void PresetBar::paintPanel (juce::Graphics& g)
{
    const float x0 = kX0, x1 = kX1, y = kY;
    soft (g, { x0 - 4, y - 4, x1 + 4, y + 60 }, 4.0f, juce::Colours::black, 0.55f, { 1.0f, 3.0f }, false, 12.0f);
    grad (g, { x0 - 5, y - 5, x1 + 5, y + 61 }, rgb (36, 40, 39), rgb (4, 5, 5), 90.0f, false, 10.0f);
    grad (g, { x0, y, x1, y + 56 }, rgb (4, 30, 26), rgb (1, 12, 10), 90.0f, false, 7.0f);
    button (g, { x0 + 12, y + 12, x0 + 52, y + 44 }, "<", false, KeyStyle::dark, 13.0f, 1.6f);
    button (g, { x1 - 52, y + 12, x1 - 12, y + 44 }, ">", false, KeyStyle::dark, 13.0f, 1.6f);
    const int idx = proc.currentPreset();
    const bool edited = proc.presetEdited();
    juce::String name = idx >= 0 ? juce::String (YankiProcessor::kPresets[idx].name).toUpperCase() : juce::String ("ECHO");
    juce::String group = idx >= 0 ? YankiProcessor::kPresets[idx].group : "INIT";
    text (g, x0 + 70, y + 14, "PRESET", Face::mono, 8.0f, kDimText, "lm", 1.2f);
    soft (g, { (x0 + x1) / 2 - 120, y + 16, (x0 + x1) / 2 + 120, y + 44 }, 10.0f, GOLD, 0.10f, {}, false, 10.0f);
    text (g, (x0 + x1) / 2, y + 30, name, Face::mono, 21.0f, GOLD_L, "mm", 0.0f);
    text (g, x1 - 70, y + 14, group, Face::mono, 8.5f, kPhos, "rm", 1.2f);
    text (g, x1 - 70, y + 44, idx >= 0 ? juce::String (idx + 1) + " OF " + juce::String (YankiProcessor::kNumPresets) : juce::String ("PRESETS 1-10"),
          Face::mono, 7.5f, kDimText, "rm", 1.0f);
    text (g, x0 + 70, y + 44, idx < 0 ? "OPENING STATE" : edited ? "EDITED" : "STARTING POINT", Face::mono, 7.5f,
          edited ? kAmber : kDimText, "lm", 1.0f);
}

void PresetBar::step (int delta)
{
    const int n = YankiProcessor::kNumPresets;
    const int i = proc.currentPreset();
    proc.applyPreset (i < 0 ? (delta > 0 ? 0 : n - 1) : (i + delta + n) % n);
    repaint();
}

void PresetBar::mouseDown (const juce::MouseEvent& e)
{
    const float fx = e.position.x + (float) getX();
    if (fx < kX0 + 60)
    {
        step (-1);
        return;
    }
    if (fx > kX1 - 60)
    {
        step (+1);
        return;
    }
    juce::PopupMenu menu;
    juce::String lastGroup;
    for (int i = 0; i < YankiProcessor::kNumPresets; ++i)
    {
        const auto& q = YankiProcessor::kPresets[i];
        if (lastGroup != q.group)
        {
            if (lastGroup.isNotEmpty()) menu.addSeparator();
            menu.addSectionHeader (q.group);
            lastGroup = q.group;
        }
        menu.addItem (i + 1, q.name, true, i == proc.currentPreset());
    }
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this), [this] (int r)
    {
        if (r > 0)
            proc.applyPreset (r - 1);
        repaint();
    });
}

void PresetBar::refresh()
{
    const int i = proc.currentPreset();
    const bool ed = proc.presetEdited();
    if (i != lastIndex || ed != lastEdited)
    {
        lastIndex = i;
        lastEdited = ed;
        repaint();
    }
}

//==============================================================================
const Box EchoField::kBox { 360.0f, 128.0f, 1240.0f, 640.0f };

EchoField::EchoField (YankiProcessor& p) : proc (p)
{
    setBounds (kBox.rect().getSmallestIntegerContainer());
}

ringmodel::Params EchoField::modelParams() const { return modelParamsFor (proc); }

EchoField::Geo EchoField::geo() const
{
    const float cx = (kBox.x1 - kBox.x0) / 2, cy = (kBox.y1 - kBox.y0) / 2 + 8;
    return { kBox.x0 + cx, kBox.y0 + cy, juce::jmin (cx - 20, cy - 26) };
}

juce::Point<float> EchoField::handlePos (Handle h) const
{
    const auto G = geo();
    if (h == handleTime)
    {
        const auto m = ringmodel::compute (modelParamsFor (proc));
        const float r1 = G.R * (float) (modelParamsFor (proc).timeS / m.viewSeconds);
        return polar (G.ox, G.oy, juce::jlimit (30.0f, G.R * 0.92f, r1), -40.0f);
    }
    if (h == handleFeedback)
        return polar (G.ox, G.oy, G.R * 0.88f, 118.0f);
    return polar (G.ox, G.oy, G.R * 0.50f, 200.0f);
}

EchoField::Handle EchoField::handleAt (juce::Point<float> p) const
{
    for (Handle h : { handleTime, handleFeedback, handleSpread })
        if (handlePos (h).getDistanceFrom (p) <= 11.0f)
            return h;
    return none;
}

juce::Point<float> EchoField::toFace (const juce::MouseEvent& e) const
{
    return { e.position.x + (float) getX(), e.position.y + (float) getY() };
}

bool EchoField::hitTest (int x, int y) { return handleAt ({ (float) (x + getX()), (float) (y + getY()) }) != none || active != none; }

void EchoField::mouseMove (const juce::MouseEvent&) {}

void EchoField::mouseDown (const juce::MouseEvent& e)
{
    const auto p = toFace (e);
    active = handleAt (p);
    if (active == none)
        return;
    startX = p.x;
    startY = p.y;
    dragView = (float) ringmodel::compute (modelParamsFor (proc)).viewSeconds;
    if (active == handleTime)
    {
        proc.apvts.getParameter (yankiIds::timeSync)->beginChangeGesture();
        proc.apvts.getParameter (yankiIds::timeMs)->beginChangeGesture();
    }
    else
    {
        auto* prm = proc.apvts.getParameter (active == handleFeedback ? yankiIds::feedback : yankiIds::spread);
        startU = prm->getValue();
        prm->beginChangeGesture();
    }
}

void EchoField::setSyncNearest (double ms)
{
    const double quarter = 60000.0 / std::max (20.0, (double) proc.uiBpm.load());
    int best = 0;
    double bestErr = 1.0e9;
    for (int i = 0; i < 6; ++i)
    {
        const double err = std::abs (std::log (std::max (1.0, ms) / (quarter * YankiProcessor::kSyncQuarters[i])));
        if (err < bestErr) { bestErr = err; best = i; }
    }
    auto* prm = proc.apvts.getParameter (yankiIds::timeSync);
    if (juce::roundToInt (prm->convertFrom0to1 (prm->getValue())) != best)
        prm->setValueNotifyingHost (prm->convertTo0to1 ((float) best));
}

void EchoField::mouseDrag (const juce::MouseEvent& e)
{
    if (active == none)
        return;
    const auto p = toFace (e);
    const auto G = geo();
    if (active == handleTime)
    {
        // ring 1 follows the cursor: its radius is the time of one repeat on the view's scale
        const double ms = (double) p.getDistanceFrom ({ G.ox, G.oy }) / (double) G.R * (double) dragView * 1000.0;
        if (juce::roundToInt (raw (proc, yankiIds::timeSync)) == YankiProcessor::kSyncFree)
        {
            auto* prm = proc.apvts.getParameter (yankiIds::timeMs);
            const float plain = juce::jlimit (50.0f, 700.0f, (float) ms - raw (proc, yankiIds::fineMs));
            prm->setValueNotifyingHost (prm->convertTo0to1 (plain));
        }
        else
            setSyncNearest (ms);
    }
    else if (active == handleFeedback)
        proc.apvts.getParameter (yankiIds::feedback)->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, startU + (startY - p.y) / 220.0f));
    else
        proc.apvts.getParameter (yankiIds::spread)->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, startU + (p.x - startX) / 260.0f));
    repaint();
}

void EchoField::mouseUp (const juce::MouseEvent&)
{
    if (active == handleTime)
    {
        proc.apvts.getParameter (yankiIds::timeSync)->endChangeGesture();
        proc.apvts.getParameter (yankiIds::timeMs)->endChangeGesture();
    }
    else if (active != none)
        proc.apvts.getParameter (active == handleFeedback ? yankiIds::feedback : yankiIds::spread)->endChangeGesture();
    active = none;
}

void EchoField::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto h = handleAt (toFace (e));
    if (h == none)
        return;
    const char* id = h == handleTime ? yankiIds::timeSync : h == handleFeedback ? yankiIds::feedback : yankiIds::spread;
    auto* prm = proc.apvts.getParameter (id);
    prm->beginChangeGesture();
    prm->setValueNotifyingHost (prm->getDefaultValue());
    prm->endChangeGesture();
}

void EchoField::paintPanel (juce::Graphics& g)
{
    const auto G = geo();
    const auto& b = kBox;
    // glass
    grad (g, b, rgb (4, 30, 26), rgb (1, 12, 10), 90.0f, false, 6.0f);
    rrect (g, { b.x0 + 1, b.y0 + 1, b.x1 - 1, b.y1 - 1 }, 6, {}, rgb (60, 120, 108, 60), 1.0f);

    const auto P = modelParamsFor (proc);
    const auto M = ringmodel::compute (P);
    const double bar = 240.0 / std::max (20.0, P.bpm);
    const float view = (float) M.viewSeconds;
    const bool swell = M.swell;

    text (g, b.x0 + 18, b.y0 + 20, "ECHO FIELD", Face::mono, 9.5f, kDimText, "lm", 2.0f);
    text (g, b.x1 - 18, b.y0 + 20, "VIEW " + juce::String (view, 1) + " S", Face::mono, 8.5f, kDimText, "rm", 1.0f);

    // guides
    g.setColour (kGuide.withAlpha ((juce::uint8) 90));
    g.drawLine (G.ox - G.R - 8, G.oy, G.ox + G.R + 8, G.oy, 0.8f);
    g.drawLine (G.ox, G.oy - G.R - 8, G.ox, G.oy + G.R + 8, 0.8f);
    g.setColour (kGuide.withAlpha ((juce::uint8) 110));
    g.drawEllipse (G.ox - G.R, G.oy - G.R, 2 * G.R, 2 * G.R, 1.0f);
    {
        const float dash[] { 2.5f, 3.5f };
        for (double t : { 0.5 * bar, bar, 2 * bar, 4 * bar })
        {
            if (t > view * 1.001) continue;
            const float r = G.R * (float) t / view;
            juce::Path ring;
            ring.addEllipse (G.ox - r, G.oy - r, 2 * r, 2 * r);
            juce::Path dashed;
            juce::PathStrokeType (0.8f).createDashedStroke (dashed, ring, dash, 2);
            g.setColour (kGuide.withAlpha ((juce::uint8) 130));
            g.fillPath (dashed);
            const char* lab = t < bar * 0.75 ? "1/2 BAR" : t < bar * 1.5 ? "1 BAR" : t < bar * 3 ? "2 BARS" : "4 BARS";
            text (g, G.ox + r + 4, G.oy + 11, lab, Face::mono, 8.0f, rgb (80, 170, 150), "lm", 0.6f);
        }
    }

    // the repeats
    const float toneN = (float) ((P.tone - 1000.0) / 7000.0);
    const float spacing = G.R * (float) P.timeS / view;
    const float loopDb = proc.uiLoopDb.load();
    const float pulse = loopDb > -90.0f ? 0.88f + 0.12f * juce::jlimit (0.0f, 1.0f, (loopDb + 50.0f) / 40.0f) : 0.88f;
    for (const auto& rg : M.rings)
    {
        const float r = G.R * (float) rg.seconds / view;
        if (r > G.R + 1.0f) break;
        const float a = std::pow (juce::jlimit (0.0f, 1.0f, (float) ((rg.levelDb + 60.0) / 66.0)), 1.5f);
        const float w = juce::jmin (0.9f + 5.2f * a, juce::jmax (1.0f, 0.42f * spacing));
        float heat = (float) rg.k * 0.075f * (1.15f - toneN) + (float) P.wear * 0.18f;
        Colour col = (toneN > 0.4f ? kTeal : kPhos).interpolatedWith (kAmber, juce::jlimit (0.0f, 1.0f, heat));
        col = col.interpolatedWith (kRust, juce::jlimit (0.0f, 1.0f, (heat - 0.55f) * 1.3f));
        if (swell)
            col = col.interpolatedWith (SWELL_RED, 0.12f * juce::jmin (1.0f, (float) rg.k / 12.0f));
        const float off = M.stereo ? (float) P.spread * G.R * 0.16f * ((rg.k % 2) ? -1.0f : 1.0f) : -0.8f * (float) P.spread * G.R * 0.16f;
        const float cx = G.ox + off;
        const float alpha = juce::jlimit (0.0f, 1.0f, (70.0f + 185.0f * a) / 255.0f) * pulse;
        const float glow = swell ? 0.45f : 1.0f;
        g.setColour (col.withAlpha (juce::jlimit (0.0f, 1.0f, 0.10f * a * glow + 0.02f)));
        g.drawEllipse (cx - r, G.oy - r, 2 * r, 2 * r, w + 7.0f);
        g.setColour (col.withAlpha (juce::jlimit (0.0f, 1.0f, 0.22f * a * glow + 0.03f)));
        g.drawEllipse (cx - r, G.oy - r, 2 * r, 2 * r, w + 3.0f);
        g.setColour (col.withAlpha (alpha));
        g.drawEllipse (cx - r, G.oy - r, 2 * r, 2 * r, w);
    }
    // the dry core, brighter with the input
    {
        const float dryDb = proc.uiDryDb.load();
        const float k = dryDb > -90.0f ? juce::jlimit (0.0f, 1.0f, (dryDb + 50.0f) / 40.0f) : 0.0f;
        const auto core = rgb (255, 250, 232);
        soft (g, { G.ox - 14, G.oy - 14, G.ox + 14, G.oy + 14 }, 7.0f, core, 0.35f + 0.4f * k);
        ellipse (g, { G.ox - 6.5f, G.oy - 6.5f, G.ox + 6.5f, G.oy + 6.5f }, core);
    }
    text (g, G.ox - 2, G.oy + 24, "DRY", Face::mono, 8.0f, rgb (80, 170, 150), "mm", 1.0f);

    // readout
    {
        const juce::String syncName = kSyncLabels[juce::jlimit (0, 6, juce::roundToInt (raw (proc, yankiIds::timeSync)))];
        const double ms = P.timeS * 1000.0;
        const auto timeTxt = syncName + (ms >= 999.5 ? "  " + juce::String (ms / 1000.0, 2) + " S" : "  " + juce::String (juce::roundToInt (ms)) + " MS");
        struct Row { juce::String k, v; bool hot; };
        const Row rows[] {
            { "TIME", timeTxt, false },
            { "REPEATS VISIBLE", juce::String ((int) M.rings.size()), false },
            { "FIRST REPEAT", (M.firstDb >= 0 ? "+" : "") + juce::String (juce::roundToInt (M.firstDb)) + " dB", false },
            { U ("DECAY −60 dB"), swell ? juce::String (P.capOn ? "HOLDS AT CAP" : "SWELLS") : juce::String (M.decayBars, 1) + " BARS", swell },
            { "TONE / WEAR", juce::String (P.tone / 1000.0, 1) + "k / " + juce::String (P.wear * 10.0, 1), false },
            { "WIDTH (L/R CORR)", juce::String (M.corr, 2), false } };
        for (int i = 0; i < 6; ++i)
        {
            const float yy = b.y0 + 56 + (float) i * 19;
            text (g, b.x0 + 20, yy, rows[i].k, Face::mono, 10.0f, kDimText, "lm", 0.4f);
            text (g, b.x0 + 20 + 215, yy, rows[i].v, Face::mono, 10.5f, rows[i].hot ? SWELL_RED : kPhos, "rm", 0.4f);
        }
    }

    // annotations
    if (! M.rings.empty())
    {
        chip (g, G.ox + G.R * 0.30f, G.oy - G.R * 1.02f + 10, "FIRST REPEAT " + juce::String (M.firstDb >= 0 ? "+" : "") + juce::String (juce::roundToInt (M.firstDb)) + " dB", 9.5f, kPhos);
        const auto lab = swell ? (P.capOn ? "HELD AT CAP " + juce::String (P.capDb >= 0 ? "+" : "") + juce::String (juce::roundToInt (P.capDb)) + " dB" : juce::String ("SWELLING"))
                               : U ("−60 dB · ") + juce::String (M.decayBars, 1) + " BARS";
        chip (g, G.ox + G.R * 0.42f, G.oy + G.R * 1.0f - 4, lab, 9.5f, swell ? SWELL_RED : kPhos);
    }
    // handles
    for (Handle h : { handleTime, handleFeedback, handleSpread })
    {
        const auto hp = handlePos (h);
        ellipse (g, { hp.x - 5.5f, hp.y - 5.5f, hp.x + 5.5f, hp.y + 5.5f }, {}, GOLD_L, 1.6f);
        ellipse (g, { hp.x - 2, hp.y - 2, hp.x + 2, hp.y + 2 }, GOLD_L);
        const char* nm = h == handleTime ? "TIME" : h == handleFeedback ? "FEEDBACK" : "SPREAD";
        if (h == handleTime)
            text (g, hp.x + 12, hp.y - 10, nm, Face::legend, 7.2f, GOLD_L, "lm", 1.2f);
        else
            text (g, hp.x - 12, hp.y + (h == handleFeedback ? 10.0f : 0.0f), nm, Face::legend, 7.2f, GOLD_L, "rm", 1.2f);
    }
    text (g, b.x0 + 20, b.y1 - 16, U ("DRAG A HANDLE:  TIME = OUT / IN  ·  FEEDBACK = UP / DOWN  ·  SPREAD = SIDEWAYS"), Face::mono, 7.6f, kDimText, "lm", 0.5f);
}

//==============================================================================
FrontPanel::FrontPanel()
{
    mark = juce::ImageCache::getFromMemory (BinaryData::mark_cream_png, BinaryData::mark_cream_pngSize);
    wordmark = juce::ImageCache::getFromMemory (BinaryData::wordmark_cream_png, BinaryData::wordmark_cream_pngSize);
    setOpaque (true);
}

void FrontPanel::paint (juce::Graphics& g)
{
    paintCached (g, cache, cacheScale, YankiEditor::kW, YankiEditor::kH, [this] (juce::Image& img, float s)
    {
        plate (img, s, rgb (26, 70, 66), rgb (9, 42, 39), 9.0f, 3.2f, 77);
        juce::Graphics gg (img);
        gg.addTransform (juce::AffineTransform::scale (s));
        frontStatic (gg, mark, wordmark);
    });
}

namespace
{
/** The tempo window: host BPM in 7-segment and the HOST lamp. */
class BpmWindow : public PanelPart
{
public:
    explicit BpmWindow (YankiProcessor& p) : proc (p)
    {
        setBounds (juce::Rectangle<float> (1214, 30, 150, 60).getSmallestIntegerContainer());
        setInterceptsMouseClicks (false, false);
    }
    void paintPanel (juce::Graphics& g) override
    {
        seven (g, 1266, 44, 30, juce::String (juce::jlimit (0, 999, juce::roundToInt (proc.uiBpm.load()))).paddedLeft (' ', 3), 26);
        redLed (g, 1220, 74, 3.2f, proc.uiHostTempo.load());
    }
    void refresh() override
    {
        const int b = juce::roundToInt (proc.uiBpm.load());
        const bool h = proc.uiHostTempo.load();
        if (b != lastBpm || h != lastHost)
        {
            lastBpm = b;
            lastHost = h;
            repaint();
        }
    }

private:
    YankiProcessor& proc;
    int lastBpm = -1;
    bool lastHost = false;
};
} // namespace

//==============================================================================
YankiEditor::YankiEditor (YankiProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
#if SVL_LICENSING
    , licenseOverlay (p.license)
#endif
{
    setLookAndFeel (&lnf);
    setWantsKeyboardFocus (true);
    addMouseListener (this, true);
    auto& ap = proc.apvts;
    auto param = [&ap] (const char* id) -> juce::RangedAudioParameter& { return *ap.getParameter (id); };

    stage.setSize (kW, kH);
    front.setBounds (0, 0, kW, kH);
    stage.addAndMakeVisible (front);
    addAndMakeVisible (stage);

    auto add = [this] (std::unique_ptr<PanelPart> part) -> PanelPart*
    {
        front.addAndMakeVisible (*part);
        parts.push_back (std::move (part));
        return parts.back().get();
    };
    auto value = [&] (float cx, float cy, std::function<juce::String()> f, float width = 120.0f, float size = 9.0f, juce::Colour col = kPhos)
    {
        add (std::make_unique<ValueLabel> (cx, cy, width, std::move (f), size, col));
    };

    presetBar = static_cast<PresetBar*> (add (std::make_unique<PresetBar> (proc)));
    add (std::make_unique<BpmWindow> (proc));
    add (std::make_unique<BypassToggle> (param (yankiIds::bypass), 1405.0f, 64.0f, 0.6f));
    add (std::make_unique<EchoField> (proc));

    // WET, the hero (the MIX parameter; the first 3 % is a small ramp in the engine feed)
    add (std::make_unique<ParamKnob> (param (yankiIds::mix), juce::Point<float> (200.0f, 340.0f), 76.0f));
    value (200.0f, 340.0f + 76.0f * 1.62f + 36.0f, [this]
    {
        const auto P = modelParamsFor (proc);
        const auto M = ringmodel::compute (P);
        return U ("MIX ") + juce::String (juce::roundToInt (raw (proc, yankiIds::mix) * 100.0f)) + U (" %  ·  FIRST REPEAT ")
               + (M.firstDb >= 0 ? "+" : "") + juce::String (juce::roundToInt (M.firstDb)) + " dB";
    }, 280.0f, 9.5f);

    // SWELL
    {
        auto key = std::make_unique<Key> (Box { kSwellCx - 62, 174, kSwellCx + 62, 208 }, "SWELL", KeyStyle::cream, 11.0f, 2.2f);
        key->isLit = [this] { return ringmodel::compute (modelParamsFor (proc)).swell; };
        key->onPress = [this]
        {
            const float k = raw (proc, yankiIds::feedback);
            if (ringmodel::compute (modelParamsFor (proc)).swell)
            {
                if (k >= 0.7f) setParam (yankiIds::feedback, 0.62f);          // back under HOLD: the repeats decay
                if (juce::roundToInt (raw (proc, yankiIds::lockBars)) > 0) setParam (yankiIds::lockBars, 0.0f);
            }
            else
                setParam (yankiIds::feedback, juce::jmax (k, 0.78f));         // just past HOLD: the loop builds
        };
        add (std::move (key));
    }
    value (kSwellCx, 222.0f, [this]
    {
        return ringmodel::compute (modelParamsFor (proc)).swell ? juce::String ("LOOP SWELLS PAST HOLD") : juce::String (U ("OFF · REPEATS DECAY"));
    }, 220.0f, 7.0f, MUTED);
    add (std::make_unique<ParamKnob> (param (yankiIds::riseCurve), juce::Point<float> (kSwellCx - 50, 280.0f), 30.0f));
    add (std::make_unique<CapKnob> (param (yankiIds::capOn), param (yankiIds::capDb), juce::Point<float> (kSwellCx + 50, 280.0f), 30.0f));
    for (int i = 0; i < 3; ++i)
    {
        const float x0 = kSwellCx - 78 + (float) i * 53;
        auto key = std::make_unique<Key> (Box { x0, 364, x0 + 48, 390 }, juce::StringArray { "HOLD", "CUT", "DUMP" }[i], KeyStyle::cream, 7.4f, 0.8f);
        key->isLit = [this, i]
        {
            return juce::roundToInt (raw (proc, yankiIds::endMode)) == i;   // the key shows the MODE; the loop state is the line below
        };
        if (i == 0)
            key->onPress = [this] { setParam (yankiIds::endMode, 0.0f); };
        else
        {
            key->onPress = [this, i]
            {
                setParam (yankiIds::endMode, (float) i);
                if (auto* t = proc.apvts.getParameter (yankiIds::endTrigger))
                {
                    t->beginChangeGesture();
                    t->setValueNotifyingHost (1.0f);
                }
            };
            key->onRelease = [this]
            {
                if (auto* t = proc.apvts.getParameter (yankiIds::endTrigger))
                {
                    t->setValueNotifyingHost (0.0f);
                    t->endChangeGesture();
                }
            };
        }
        add (std::move (key));
    }
    // END: CUT and DUMP are triggers, so the keys show the chosen mode only; this line says where the loop stands.
    value (kSwellCx + 60.0f, 402.0f, [this]
    {
        const int mode = juce::roundToInt (raw (proc, yankiIds::endMode));
        if (mode == 0) return juce::String ("LOOP OPEN");
        return proc.uiEndLatched.load() ? (mode == 1 ? juce::String ("CUT") : juce::String ("DUMPED")) : juce::String ("ARMED");
    }, 100.0f, 7.4f, kPhos);
    {
        auto key = std::make_unique<Key> (Box { kSwellCx - 62, 416, kSwellCx + 62, 444 }, "FREEZE", KeyStyle::dark, 8.6f, 1.4f);
        key->isLit = [this] { return raw (proc, yankiIds::freeze) > 0.5f; };
        key->onPress = [this]
        {
            freezeWasOn = raw (proc, yankiIds::freeze) > 0.5f;
            freezePressedAt = nowSeconds();
            setParam (yankiIds::freeze, 1.0f);
        };
        key->onRelease = [this]
        {
            if (freezeWasOn || nowSeconds() - freezePressedAt >= 0.35)
                setParam (yankiIds::freeze, 0.0f);
        };
        add (std::move (key));
    }
    for (int i = 0; i < 4; ++i)
    {
        const float x0 = kSwellCx - 101 + (float) i * 52;
        auto key = std::make_unique<Key> (Box { x0, 494, x0 + 46, 520 }, juce::StringArray { "OFF", "1", "2", "4" }[i], KeyStyle::cream, 7.4f, 0.8f);
        key->isLit = [this, i] { return juce::roundToInt (raw (proc, yankiIds::lockBars)) == i; };
        key->onPress = [this, i] { setParam (yankiIds::lockBars, (float) i); };
        add (std::move (key));
    }
    add (std::make_unique<ParamKnob> (param (yankiIds::darken), juce::Point<float> (kSwellCx - 50, 592.0f), 24.0f));
    add (std::make_unique<ParamKnob> (param (yankiIds::fineMs), juce::Point<float> (kSwellCx + 50, 592.0f), 24.0f));

    // HEADS 1 / 2 / 3: how many heads play (the rings split into two groups from 2). A MOOD or a preset sets it back to its own.
    for (int i = 0; i < 3; ++i)
    {
        const float x0 = 200.0f - 84.0f + (float) i * 58.0f;
        auto key = std::make_unique<Key> (Box { x0, 748, x0 + 52, 776 }, juce::StringArray { "1", "2", "3" }[i], KeyStyle::cream, 9.0f, 0.8f);
        key->isLit = [this, i]
        {
            return YankiProcessor::effectiveHeads (juce::roundToInt (raw (proc, yankiIds::mood)), juce::roundToInt (raw (proc, yankiIds::heads))) == i + 1;
        };
        key->onPress = [this, i] { setParam (yankiIds::heads, (float) (i + 1)); };
        add (std::move (key));
    }
    value (200.0f, 806.0f, [this]
    {
        const int n = YankiProcessor::effectiveHeads (juce::roundToInt (raw (proc, yankiIds::mood)), juce::roundToInt (raw (proc, yankiIds::heads)));
        return n == 1 ? juce::String ("ONE HEAD") : n == 2 ? juce::String (U ("TWO HEADS · LEFT / RIGHT")) : juce::String (U ("THREE HEADS · L / R / L"));
    }, 240.0f, 8.0f);

    // the six knobs under the display
    {
        auto ring = std::make_unique<ParamKnob> (param (yankiIds::timeSync), juce::Point<float> (432.0f, kKnobY), 46.0f, true, kSyncAngles);
        ring->setHitHole (18.0f * 1.12f);
        add (std::move (ring));
        add (std::make_unique<ParamKnob> (param (yankiIds::timeMs), juce::Point<float> (432.0f, kKnobY), 18.0f));
    }
    add (std::make_unique<ParamKnob> (param (yankiIds::feedback), juce::Point<float> (600.0f, kKnobY), 33.0f));
    add (std::make_unique<ParamKnob> (param (yankiIds::tone), juce::Point<float> (750.0f, kKnobY), 33.0f));
    add (std::make_unique<ParamKnob> (param (yankiIds::wear), juce::Point<float> (890.0f, kKnobY), 33.0f));
    add (std::make_unique<ParamKnob> (param (yankiIds::spread), juce::Point<float> (1030.0f, kKnobY), 33.0f));
    add (std::make_unique<ParamKnob> (param (yankiIds::output), juce::Point<float> (1170.0f, kKnobY), 33.0f));

    value (432.0f, kKnobY + 86.0f, [this]
    {
        const auto P = modelParamsFor (proc);
        const double ms = P.timeS * 1000.0;
        return kSyncLabels[juce::jlimit (0, 6, juce::roundToInt (raw (proc, yankiIds::timeSync)))] + U (" · ")
               + (ms >= 999.5 ? juce::String (ms / 1000.0, 2) + " S" : juce::String (juce::roundToInt (ms)) + " MS");
    }, 160.0f);
    value (600.0f, kKnobY + 77.0f, [this]
    {
        const auto M = ringmodel::compute (modelParamsFor (proc));
        return M.swell ? juce::String ("PAST HOLD") : juce::String (M.decayBars, 1) + " BARS";
    });
    value (750.0f, kKnobY + 77.0f, [this] { return juce::String (raw (proc, yankiIds::tone) / 1000.0f, 1) + " kHz"; });
    value (890.0f, kKnobY + 77.0f, [this] { return juce::String (raw (proc, yankiIds::wear) * 10.0f, 1); });
    value (1030.0f, kKnobY + 77.0f, [this] { return juce::String (juce::roundToInt (raw (proc, yankiIds::spread) * 100.0f)) + " %"; });
    value (1170.0f, kKnobY + 77.0f, [this] { return juce::String (raw (proc, yankiIds::output), 1) + " dB"; });

    setResizable (true, true);
    setResizeLimits (800, 450, 2400, 1350);
    if (auto* cons = getConstrainer())
        cons->setFixedAspectRatio ((double) kW / (double) kH);
    setSize (1280, 720);

#if SVL_LICENSING
    licenseButton.setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    licenseButton.setColour (juce::TextButton::textColourOffId, Theme::ink.withAlpha (0.6f));
    licenseButton.onClick = [this] { licenseOverlay.refresh(); licenseOverlay.setVisible (true); licenseOverlay.toFront (true); };
    addAndMakeVisible (licenseButton);
    addChildComponent (licenseOverlay);
    licenseOverlay.setVisible (! proc.license.isLicensed());
#endif

    lastTick = nowSeconds();
    snapshotRefresh();
    startTimerHz (30);
}

YankiEditor::~YankiEditor() { stopTimer(); setLookAndFeel (nullptr); }

bool YankiEditor::keyPressed (const juce::KeyPress& key)
{
#if SVL_LICENSING
    if (licenseOverlay.isVisible())
        return false;
#endif
    return svl::handleUndoKey (proc.undo, key);
}

void YankiEditor::mouseDown (const juce::MouseEvent& e)
{
#if SVL_LICENSING
    if (licenseOverlay.isVisible())
        return;
#endif
    svl::takeFocusOnClick (*this, e);
}

void YankiEditor::paint (juce::Graphics& g) { g.fillAll (rgb (10, 12, 11)); }

void YankiEditor::resized()
{
    stage.setTransform (juce::AffineTransform::scale ((float) getWidth() / (float) kW, (float) getHeight() / (float) kH));
#if SVL_LICENSING
    licenseOverlay.setBounds (getLocalBounds());
    licenseButton.setBounds (getWidth() / 2 + 200, getHeight() - 24, 70, 18);
#endif
}

void YankiEditor::setParam (const char* id, float value, bool gesture)
{
    if (auto* p = proc.apvts.getParameter (id))
    {
        if (gesture) p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 (value));
        if (gesture) p->endChangeGesture();
    }
}

void YankiEditor::snapshotRefresh()
{
    for (auto& part : parts)
        part->refresh();
}

void YankiEditor::timerCallback()
{
#if SVL_LICENSING
    if (! proc.license.isLicensed() && ! licenseOverlay.isVisible())
    {
        licenseOverlay.refresh();
        licenseOverlay.setVisible (true);
    }
#endif
    snapshotRefresh();
}
