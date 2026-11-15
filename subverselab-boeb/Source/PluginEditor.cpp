#include "PluginEditor.h"
#include "BinaryData.h"

/*  The BOE-B face, 1280 x 680: his 1992 sleeve (owner 24 Oct, sketch v4, docs/ui/mockup/boeb_panel_v4.png).
    Left: the name, the model on Dymo tape, LISTEN, the small knobs. Middle: his photocopied face, half length.
    Right: the five causes typed on a sheet taped to the sleeve, LIFT, EQUAL / THROW, MOOD on Dymo tape.
    While LISTEN listens, a photocopier's light runs down over his face; the numbers are typed in when they arrive.
    Interface signature (UI_SIGNATURE_STANDARD.md): knobs glide to values the product sets, double-click = the loaded
    default (LIFT 6, AIR = what LISTEN set), name + model top left, SubverseLab bottom right linked, "?" opens in place. */
namespace
{
using Pt = juce::Point<float>;
using R = juce::Rectangle<float>;
constexpr float kPi = juce::MathConstants<float>::pi;

const juce::String kMinus (juce::CharPointer_UTF8 ("\xe2\x88\x92"));
const juce::String kArrow (juce::CharPointer_UTF8 ("\xe2\x86\x92"));

// Layout (sketch v4 coordinates).
const R kName (26, 6, 436, 172);
const R kPortrait (318, 190, 490, 490);
const R kSheet (830, 24, 420, 250);    // the sheet's own drawing size ...
constexpr float kSheetScale = 1.25f;    // ... drawn this much larger, into the space right of his face (owner 24 Oct)
const R kSheetDraw (1250 - 420 * kSheetScale, 20, 420 * kSheetScale, 250 * kSheetScale);
constexpr float kSheetRot = 0.0f;   // straight (owner 24 Oct: "toparlayalım")
const Pt kLiftC (962, 452);
constexpr float kLiftR = 60.0f;
const R kLockup (1090, 648, 166, 26);

/** The SubverseLab icon in the lockup corner: icon only (owner 14 Nov 2026, no wordmark), right-aligned where the lockup
    ended, 22 px tall as before. Only the icon is the link. */
R lockupIcon (const juce::Image& mark)
{
    const float mh = 22.0f, mw = mark.isNull() ? mh : mh * (float) mark.getWidth() / (float) mark.getHeight();
    return { kLockup.getRight() - mw, kLockup.getCentreY() - mh / 2, mw, mh };
}

juce::String num (float v, bool sign = true)
{
    const juce::String s = juce::String (std::abs (v), 1);
    if (std::abs (v) < 0.05f) return "0.0";
    return (v < 0 ? "-" : (sign ? "+" : "")) + s;
}

void print (juce::Graphics& g, const juce::String& s, juce::Font f, R r, juce::Justification j, juce::Colour c, float track = 0.0f)
{
    if (track > 0.0f) f = f.withExtraKerningFactor (track / f.getHeight());
    g.setFont (f);
    g.setColour (c);
    g.drawText (s, r, j, false);
}

void lamp (juce::Graphics& g, Pt c, float r, int kind)   // 0 off, 1 red, 2 paper (inside the reference)
{
    g.setColour (Theme::ink);
    g.fillEllipse (c.x - r - 2, c.y - r - 2, 2 * r + 4, 2 * r + 4);
    if (kind == 1)
    {
        g.setGradientFill (juce::ColourGradient (Theme::led.withAlpha (0.55f), c.x, c.y, Theme::led.withAlpha (0.0f), c.x + r * 2.6f, c.y, true));
        g.fillEllipse (c.x - r * 2.6f, c.y - r * 2.6f, r * 5.2f, r * 5.2f);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xffFFC0B0), c.x - r * 0.3f, c.y - r * 0.35f, juce::Colour (0xffA01A0C), c.x + r, c.y + r, true));
    }
    else if (kind == 2)
        g.setGradientFill (juce::ColourGradient (juce::Colours::white, c.x - r * 0.3f, c.y - r * 0.35f, juce::Colour (0xffB9B09A), c.x + r, c.y + r, true));
    else
        g.setColour (Theme::ledOff);
    g.fillEllipse (c.x - r, c.y - r, 2 * r, 2 * r);
}

juce::Image makeGrain (int w, int h)
{
    juce::Image img (juce::Image::ARGB, w, h, true);
    juce::Random rnd (1992);
    juce::Image::BitmapData bd (img, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const float v = rnd.nextFloat();
            if (v < 0.16f)       bd.setPixelColour (x, y, juce::Colour (0, 0, 0).withAlpha (0.05f + 0.10f * rnd.nextFloat()));
            else if (v > 0.985f) bd.setPixelColour (x, y, juce::Colour (255, 240, 225).withAlpha (0.10f));
        }
    return img;
}

juce::Path tornRect (R r, int seed, float jag)
{
    juce::Random rnd (seed);
    juce::Path p;
    const int nx = 28, ny = 12;
    p.startNewSubPath (r.getX(), r.getY() + rnd.nextFloat() * jag);
    for (int i = 1; i <= nx; ++i) p.lineTo (r.getX() + r.getWidth() * (float) i / nx, r.getY() + rnd.nextFloat() * jag);
    for (int i = 1; i <= ny; ++i) p.lineTo (r.getRight() - rnd.nextFloat() * jag * 0.5f, r.getY() + r.getHeight() * (float) i / ny);
    for (int i = nx - 1; i >= 0; --i) p.lineTo (r.getX() + r.getWidth() * (float) i / nx, r.getBottom() - rnd.nextFloat() * jag);
    for (int i = ny - 1; i >= 1; --i) p.lineTo (r.getX() + rnd.nextFloat() * jag * 0.5f, r.getY() + r.getHeight() * (float) i / ny);
    p.closeSubPath();
    return p;
}

void tapeStrip (juce::Graphics& g, Pt c, float w, float h, float deg)
{
    juce::Graphics::ScopedSaveState s (g);
    g.addTransform (juce::AffineTransform::rotation (deg * kPi / 180.0f, c.x, c.y));
    g.setColour (juce::Colours::black.withAlpha (0.12f));
    g.fillRect (R (c.x - w / 2, c.y - h / 2 + 1.5f, w, h));
    g.setColour (Theme::tape);
    g.fillRect (R (c.x - w / 2, c.y - h / 2, w, h));
}

/** A knob's scale: ticks and labels around it, printed in ink on the sleeve. */
void knobScale (juce::Graphics& g, Pt c, float r, const std::vector<std::pair<float, juce::String>>& marks, bool big)
{
    for (const auto& [t, s] : marks)
    {
        const float a = (-135.0f + 270.0f * t) * kPi / 180.0f;
        const Pt d (std::sin (a), -std::cos (a));
        g.setColour (Theme::ink);
        // outside the milled socket (r + 10 / r + 7)
        g.drawLine (juce::Line<float> (c + d * (r + (big ? 14.0f : 10.0f)), c + d * (r + (big ? 24.0f : 16.0f))), big ? 3.0f : 2.0f);
        if (s.isNotEmpty())
        {
            const Pt q = c + d * (r + (big ? 38.0f : 25.0f));
            print (g, s, big ? Theme::stamp (21.0f) : Theme::legend (9.0f), R (q.x - 20, q.y - 12, 40, 24), juce::Justification::centred, Theme::ink);
        }
    }
}
}

//==============================================================================
void BakeliteKnob::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    const Pt c = b.getCentre();
    const float r = std::min (b.getWidth(), b.getHeight()) * 0.5f - 3.0f;
    // drop shadow on the sleeve
    g.setColour (juce::Colours::black.withAlpha (0.30f));
    g.fillEllipse (c.x - r + 2, c.y - r + 4, 2 * r, 2 * r);
    // knurled skirt
    g.setColour (juce::Colour (0xff050505));
    g.fillEllipse (c.x - r, c.y - r, 2 * r, 2 * r);
    const int teeth = 48;
    for (int i = 0; i < teeth; ++i)
    {
        const float a = (float) i * 2.0f * kPi / teeth;
        const Pt d (std::sin (a), -std::cos (a));
        g.setColour ((i & 1) ? juce::Colour (0xff050505) : juce::Colour (0xff2A2828));
        g.drawLine (juce::Line<float> (c + d * (r * 0.86f), c + d * r), big ? 5.0f : 3.0f);
    }
    // cap
    const float rc = r * 0.84f;
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff4A4747), c.x - rc * 0.3f, c.y - rc * 0.45f, juce::Colour (0xff050505), c.x + rc, c.y + rc, true));
    g.fillEllipse (c.x - rc, c.y - rc, 2 * rc, 2 * rc);
    g.setColour (juce::Colours::white.withAlpha (0.06f));
    g.drawEllipse (c.x - rc + 1, c.y - rc + 1, 2 * rc - 2, 2 * rc - 2, 1.0f);
    // cream line
    const auto rp = getRotaryParameters();
    const float a = rp.startAngleRadians + (float) valueToProportionOfLength (getValue()) * (rp.endAngleRadians - rp.startAngleRadians);
    const Pt d (std::sin (a), -std::cos (a));
    g.setColour (Theme::paper);
    juce::Path line;
    line.startNewSubPath (c + d * (r * 0.2f));
    line.lineTo (c + d * (r * 0.92f));
    g.strokePath (line, juce::PathStrokeType (big ? 7.0f : 4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void BlackKey::paintButton (juce::Graphics& g, bool over, bool down)
{
    auto b = getLocalBounds().toFloat().reduced (3.0f);
    const float press = down ? 3.0f : 0.0f;
    g.setColour (juce::Colours::black);
    g.fillRoundedRectangle (b.translated (0, 4), 5.0f);            // the key's side
    b = b.translated (0, press);
    g.setGradientFill (juce::ColourGradient (lit ? juce::Colour (0xff3A3434) : juce::Colour (0xff2A2828), b.getX(), b.getY(),
                                             juce::Colour (0xff0C0B0B), b.getX(), b.getBottom(), false));
    g.fillRoundedRectangle (b, 5.0f);
    g.setColour (juce::Colours::black);
    g.drawRoundedRectangle (b, 5.0f, 2.0f);
    g.setColour (juce::Colours::white.withAlpha (over ? 0.16f : 0.10f));
    g.drawLine (b.getX() + 5, b.getY() + 2, b.getRight() - 5, b.getY() + 2, 1.0f);

    auto word = b.withTrimmedBottom (b.getHeight() * 0.36f);
    const auto small = b.withTrimmedTop (b.getHeight() * 0.62f).withTrimmedBottom (b.getHeight() * 0.08f);
    if (hasLamp)
    {
        const float wordW = juce::GlyphArrangement::getStringWidth (Theme::stamp (textPx).withExtraKerningFactor (0.10f), getButtonText());
        const float x0 = b.getCentreX() - (wordW + 28.0f) * 0.5f;
        lamp (g, { x0 + 7.0f, word.getCentreY() + 2.0f }, 7.0f, lampOn ? 1 : 0);
        word = word.withLeft (x0 + 26.0f).withWidth (wordW + 10.0f);
        print (g, getButtonText(), Theme::stamp (textPx), word.translated (0, 3), juce::Justification::centredLeft, Theme::paper, textPx * 0.10f);
    }
    else
    {
        if (lit) lamp (g, { b.getRight() - 16.0f, b.getY() + 15.0f }, 5.0f, 1);
        print (g, getButtonText(), Theme::stamp (textPx), word.translated (0, 3), juce::Justification::centred, Theme::paper, textPx * 0.08f);
    }
    print (g, sub, Theme::mono (10.0f), small, juce::Justification::centred, Theme::capText, 0.6f);
}

void DymoButton::paintButton (juce::Graphics& g, bool over, bool down)
{
    juce::Graphics::ScopedSaveState s (g);
    const auto all = getLocalBounds().toFloat();
    g.addTransform (juce::AffineTransform::rotation (rot * kPi / 180.0f, all.getCentreX(), all.getCentreY()));
    auto b = all.reduced (4.0f, 3.0f).translated (0, down ? 1.0f : 0.0f);
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillRoundedRectangle (b.translated (1.0f, 2.5f), 2.0f);
    g.setGradientFill (chosen ? juce::ColourGradient (juce::Colour (0xffF4F1EA), 0, b.getY(), juce::Colour (0xffD9D2C0), 0, b.getBottom(), false)
                              : juce::ColourGradient (juce::Colour (0xff1D1D1D), 0, b.getY(), juce::Colour (0xff0A0A0A), 0, b.getBottom(), false));
    g.fillRoundedRectangle (b, 2.0f);
    if (over && ! chosen) { g.setColour (juce::Colours::white.withAlpha (0.06f)); g.fillRoundedRectangle (b, 2.0f); }
    // One size and one letter spacing for every strip (MEGAPHONE got a tighter spacing to fit and looked wrong, owner 24 Oct).
    const auto t = getButtonText();
    const float track = 1.8f, px = 11.0f;
    // embossed: a dark line under the letters, a light one over them
    print (g, t, Theme::legend (px), b.translated (0.5f, 1.0f), juce::Justification::centred, chosen ? juce::Colours::white : juce::Colours::black, track);
    print (g, t, Theme::legend (px), b.translated (0, -0.5f), juce::Justification::centred, chosen ? juce::Colour (0xffB0A890) : juce::Colour (0xff6A6A6A), track);
    print (g, t, Theme::legend (px), b, juce::Justification::centred, chosen ? Theme::ink : juce::Colour (0xffF4F1EA), track);
}

void InSwitch::paintButton (juce::Graphics& g, bool, bool)
{
    const auto b = getLocalBounds().toFloat();
    print (g, "IN", Theme::mono (11.5f), b.withWidth (24), juce::Justification::centredLeft, Theme::ink);
    lamp (g, { b.getX() + 32.0f, b.getCentreY() }, 6.0f, on ? 1 : 0);
}

//==============================================================================
BoebEditor::BoebEditor (BoebProcessor& p) : juce::AudioProcessorEditor (p), proc (p)
{
    grain = makeGrain (kW, kH);
    portrait = juce::ImageCache::getFromMemory (BinaryData::portrait_ink_png, BinaryData::portrait_ink_pngSize);
    mark = juce::ImageCache::getFromMemory (BinaryData::mark_ink_png, BinaryData::mark_ink_pngSize);
    sheetEdge = tornRect (R (0, 0, kSheet.getWidth(), kSheet.getHeight()), 7, 5.0f);

    bind (lift,   "lift",   [] { return 6.0; });
    bind (body,   "body",   [] { return 0.0; });
    bind (air,    "air",    [this] { return (double) proc.listenedAirDb(); });
    bind (space,  "space",  [] { return 0.0; });
    bind (output, "output", [] { return 0.0; });
    bind (dryWet, "drywet", [] { return 1.0; });

    listenKey.hasLamp = true;
    listenKey.textPx = 38.0f;
    listenKey.onClick = [this] { proc.startListen(); };
    addAndMakeVisible (listenKey);

    equalKey.onClick = [this]
    {
        auto* prm = proc.apvts.getParameter ("equal");
        svl::setParamAsGesture (prm, prm->getValue() > 0.5f ? 0.0f : 1.0f);
    };
    addAndMakeVisible (equalKey);

    throwKey.onStateChange = [this]
    {
        const bool down = throwKey.isDown();
        if (down == throwHeld) return;
        throwHeld = down;
        auto* prm = proc.apvts.getParameter ("throw");
        if (down) { prm->beginChangeGesture(); prm->setValueNotifyingHost (1.0f); }
        else      { prm->setValueNotifyingHost (0.0f); prm->endChangeGesture(); }
    };
    addAndMakeVisible (throwKey);

    const char* names[6] = { "CLEAN", "PUSH", "90S", "RADIO", "PHONE", "MEGAPHONE" };
    const float rots[6] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    for (int i = 0; i < 6; ++i)
    {
        moods[(size_t) i] = std::make_unique<DymoButton> (names[i], rots[i]);
        moods[(size_t) i]->onClick = [this, i] { svl::setParamAsGesture (proc.apvts.getParameter ("mood"), (float) i); };
        addAndMakeVisible (*moods[(size_t) i]);
    }
    help.onClick = [this] { setHelpOpen (! helpOpen); };
    addAndMakeVisible (help);

    inSwitch.onClick = [this]
    {
        auto* prm = proc.apvts.getParameter ("in");
        svl::setParamAsGesture (prm, prm->getValue() > 0.5f ? 0.0f : 1.0f);
    };
    addAndMakeVisible (inSwitch);

    setWantsKeyboardFocus (true);
    addMouseListener (this, true);
    setSize (kW, kH);
    syncControls (false);
    startTimerHz (30);
}

BoebEditor::~BoebEditor()
{
    stopTimer();
    if (throwHeld) if (auto* prm = proc.apvts.getParameter ("throw")) { prm->setValueNotifyingHost (0.0f); prm->endChangeGesture(); }
}

void BoebEditor::bind (BakeliteKnob& k, const char* id, std::function<double()> dc)
{
    auto* prm = proc.apvts.getParameter (id);
    const auto range = prm->getNormalisableRange();
    k.setRange (range.start, range.end, range.interval);
    k.setDoubleClickReturnValue (false, 0.0);
    addAndMakeVisible (k);
    bound.push_back ({ &k, prm, std::move (dc) });
    const size_t i = bound.size() - 1;
    k.onDragStart = [this, i] { bound[i].held = true; bound[i].knob->animating = false; bound[i].param->beginChangeGesture(); };
    k.onDragEnd   = [this, i] { bound[i].param->endChangeGesture(); bound[i].held = false; };
    k.onValueChange = [this, i]
    {
        auto& b = bound[i];
        if (b.held) b.param->setValueNotifyingHost (b.param->convertTo0to1 ((float) b.knob->getValue()));
    };
    // Double-click: the default of what is loaded now, asked at the click (signature 2).
    k.onDoubleClick = [this, i]
    {
        auto& b = bound[i];
        svl::setParamAsGesture (b.param, (float) b.doubleClickValue());
    };
}

void BoebEditor::syncControls (bool glide)
{
    for (auto& b : bound)
    {
        auto& k = *b.knob;
        const double target = b.param->convertFrom0to1 (b.param->getValue());
        if (b.held) continue;
        if (std::abs (k.getValue() - target) < 1.0e-6) { k.animating = false; continue; }
        if (glide && k.isVisible()) { k.animTarget = target; k.animating = true; }
        else { k.animating = false; k.setValue (target, juce::dontSendNotification); }
    }
    const int mood = (int) proc.apvts.getRawParameterValue ("mood")->load();
    for (int i = 0; i < 6; ++i)
        if (moods[(size_t) i]->chosen != (i == mood)) { moods[(size_t) i]->chosen = (i == mood); moods[(size_t) i]->repaint(); }
    const bool eq = proc.apvts.getRawParameterValue ("equal")->load() > 0.5f;
    if (equalKey.lit != eq) { equalKey.lit = eq; equalKey.repaint(); }
    const bool on = proc.apvts.getRawParameterValue ("in")->load() > 0.5f;
    if (inSwitch.on != on) { inSwitch.on = on; inSwitch.repaint(); }
}

void BoebEditor::advance (float dt)
{
    syncControls (true);
    // Glide (signature 1): 30 % of the way per 30 Hz tick, at least one grid step.
    const float ticks = dt * 30.0f;
    for (auto& b : bound)
    {
        auto& k = *b.knob;
        if (! k.animating) continue;
        double v = k.getValue();
        for (int t = 0; t < (int) std::ceil (ticks); ++t)
        {
            const double step = std::max (std::abs (k.animTarget - v) * 0.3, k.getInterval());
            v = std::abs (k.animTarget - v) <= step ? k.animTarget : v + (k.animTarget > v ? step : -step);
        }
        k.setValue (v, juce::dontSendNotification);
        if (juce::exactlyEqual (v, k.animTarget)) k.animating = false;
    }

    // LISTEN: the scan light while listening, a flash when it is worked out.
    const int st = proc.listenState();
    if (lastState != 0 && st == 0) flash = 1.0f;
    lastState = st;
    scanPhase = st != 0 ? std::fmod (scanPhase + dt / 1.6f, 1.0f) : 0.0f;
    flash = std::max (0.0f, flash - dt * 1.6f);
    blink = std::fmod (blink + dt, 0.8f);
    listenKey.lampOn = st == 1 ? blink < 0.4f : st == 2;
    listenKey.sub = st == 1 ? juce::String ("LISTENING ") + juce::String (proc.listenHeard(), 1) + " / " + juce::String ((int) BoebProcessor::kListenSec) + " S"
                  : st == 2 ? juce::String ("WORKING IT OUT") : juce::String ((int) BoebProcessor::kListenSec) + " S OF THE SONG";
    listenKey.repaint();

    // Typewriter: new numbers are typed in, 60 characters a second.
    const auto r = proc.readout();
    if (r.foundSerial != typedFoundSerial) { typedFoundSerial = r.foundSerial; typeFound = 0.0f; }
    if (r.nowSerial != typedNowSerial)     { typedNowSerial = r.nowSerial; typeNow = 0.0f; }
    shown = r;
    typeFound += dt * 60.0f;
    typeNow += dt * 60.0f;
    repaint();
}

void BoebEditor::settleForSnapshot()
{
    syncControls (false);
    advance (0.0f);
    typeFound = typeNow = 1000.0f;
}

void BoebEditor::timerCallback() { advance (1.0f / 30.0f); }

double BoebEditor::drawnValue (const juce::String& id) const
{
    for (const auto& b : bound) if (b.param->getParameterID() == id) return b.knob->getValue();
    return 0.0;
}

void BoebEditor::doubleClick (const juce::String& id)
{
    for (auto& b : bound) if (b.param->getParameterID() == id && b.knob->onDoubleClick) b.knob->onDoubleClick();
}

//==============================================================================
void BoebEditor::resized()
{
    // Three columns (owner 24 Oct: "saçma sapan bir şey oldu, toparlayalım"): left the name, LISTEN and MOOD; the middle
    // only his face; right the sheet and every knob. Straight, on shared edges.
    listenKey.setBounds (32, 318, 260, 88);
    for (int i = 0; i < 6; ++i)
        moods[(size_t) i]->setBounds (30 + (i % 2) * 132, 500 + (i / 2) * 40, 132, 36);
    lift.setBounds (juce::Rectangle<int> (0, 0, (int) (2 * kLiftR), (int) (2 * kLiftR)).withCentre (kLiftC.toInt()));
    equalKey.setBounds (1074, 356, 176, 60);
    throwKey.setBounds (1074, 428, 176, 60);
    help.setBounds (1074, 504, 44, 30);
    inSwitch.setBounds (1196, 506, 50, 26);
    // The fine knobs, scattered along his shoulder just outside the jacket (owner 24 Oct: "boe-b'nin sağ omzuna doğru
    // serpiştirelim"); the shoulder line measured from the portrait: (599,430) (669,480) (730,530) (751,580) (761,630).
    const juce::Point<int> spots[4] = { { 676, 420 }, { 750, 470 }, { 808, 534 }, { 818, 612 } };
    int i = 0;
    for (auto* k : { &body, &air, &space, &output })
        k->setBounds (juce::Rectangle<int> (46, 46).withCentre (spots[i++]));
    // DRY/WET beside LIFT, under IN (owner 7 Nov): the whole channel against the untouched vocal.
    dryWet.setBounds (juce::Rectangle<int> (46, 46).withCentre (juce::Point<int> (1150, 596)));
}

void BoebEditor::paint (juce::Graphics& g)
{
    // The sleeve: red, print grain.
    g.setGradientFill (juce::ColourGradient (Theme::sleeve, 0, 0, Theme::sleeveDeep, (float) kW, (float) kH, false));
    g.fillAll();
    g.setColour (Theme::sleeve.withAlpha (0.65f));
    g.fillRect (getLocalBounds());
    g.setOpacity (1.0f);   // drawImage takes the current colour's alpha as its opacity
    g.drawImageAt (grain, 0, 0);

    // Knob sockets milled into the red plate (owner 24 Oct: "knob yuvalarını oyarak kırmızı zemine levha hissi").
    for (auto* k : { &lift, &body, &air, &space, &output, &dryWet })
    {
        const auto c = k->getBounds().toFloat().getCentre();
        const float r = k->getWidth() * 0.5f + (k == &lift ? 10.0f : 7.0f);
        const R well (c.x - r, c.y - r, 2 * r, 2 * r);
        g.setGradientFill (juce::ColourGradient (Theme::sleeveDeep.darker (0.55f), c.x, well.getY(), Theme::sleeve.brighter (0.12f), c.x, well.getBottom(), false));
        g.fillEllipse (well);                                         // the wall: dark at the top, lit at the bottom
        g.setGradientFill (juce::ColourGradient (Theme::sleeveDeep.darker (0.25f), c.x, c.y - r, Theme::sleeveDeep, c.x, c.y + r, false));
        g.fillEllipse (well.reduced (3.0f));                          // the floor of the pocket
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.drawEllipse (well.reduced (3.0f), 1.5f);
        g.setColour (juce::Colours::white.withAlpha (0.18f));
        g.drawEllipse (well.expanded (0.5f), 1.0f);                   // the plate's edge round the cut
    }

    // His face, printed onto the sleeve (ink with alpha: multiply-like on the red).
    g.setOpacity (1.0f);   // the sockets left a translucent colour set
    g.drawImage (portrait, kPortrait, juce::RectanglePlacement::stretchToFit);
    if (scanPhase > 0.0f || flash > 0.0f)
    {
        juce::Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (kPortrait.toNearestInt());
        if (scanPhase > 0.0f)
        {
            // the photocopier's light, running down
            const float y = kPortrait.getY() + scanPhase * (kPortrait.getHeight() + 60.0f) - 30.0f;
            juce::ColourGradient cg (juce::Colour (0x00FFF8E8), 0, y - 34, juce::Colour (0x00FFF8E8), 0, y + 34, false);
            cg.addColour (0.5, juce::Colour (0x88FFF8E8));
            g.setGradientFill (cg);
            g.fillRect (R (kPortrait.getX(), y - 34, kPortrait.getWidth(), 68));
            g.setColour (juce::Colour (0xeeFFFFFF));
            g.fillRect (R (kPortrait.getX(), y - 1.5f, kPortrait.getWidth(), 3));
        }
        if (flash > 0.0f) { g.setColour (juce::Colour (0xffFFF8E8).withAlpha (0.45f * flash)); g.fillRect (kPortrait); }
    }

    // Name and model, top left (signature 3).
    {
        const juce::String name ("BOE-B");
        auto f = Theme::stamp (220.0f);
        const float w = juce::GlyphArrangement::getStringWidth (f, name);
        f = f.withHorizontalScale (kName.getWidth() / w);
        print (g, name, f, kName.withHeight (196.0f).translated (0, -14), juce::Justification::centredLeft, Theme::ink);
    }
    // What it is (owner 24 Oct: "rap vocal channel yazması ne kadar doğru bilmiyorum"; it lifts any dull vocal, sung too).
    print (g, "LISTENING", Theme::stamp (34.0f), R (32, 188, 300, 38), juce::Justification::centredLeft, Theme::ink);
    print (g, "VOCAL ENHANCER", Theme::stamp (34.0f), R (32, 222, 300, 38), juce::Justification::centredLeft, Theme::ink);
    {
        // Dymo: MODEL VX-90
        juce::Graphics::ScopedSaveState s (g);
        const R d (34, 270, 150, 26);
                g.setColour (juce::Colours::black.withAlpha (0.35f)); g.fillRoundedRectangle (d.translated (1, 2.5f), 2);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1D1D1D), 0, d.getY(), juce::Colour (0xff0A0A0A), 0, d.getBottom(), false));
        g.fillRoundedRectangle (d, 2);
        print (g, "MODEL VX-90", Theme::legend (12.5f), d.translated (0.5f, 1), juce::Justification::centred, juce::Colours::black, 3.0f);
        print (g, "MODEL VX-90", Theme::legend (12.5f), d, juce::Justification::centred, juce::Colour (0xffF4F1EA), 3.0f);
    }

    // Beat on the side input.
    lamp (g, { 41, 428 }, 6.0f, proc.beatOnSide() ? 2 : 0);
    print (g, "BEAT ON SIDECHAIN", Theme::mono (11.5f), R (56, 418, 220, 20), juce::Justification::centredLeft, Theme::ink);

    // Fine knobs: one row under LIFT.
    struct Small { BakeliteKnob* k; const char* name; const char* id; };
    for (const auto& s : { Small { &body, "BODY", "body" }, Small { &air, "AIR", "air" }, Small { &space, "SPACE", "space" }, Small { &output, "OUTPUT", "output" }, Small { &dryWet, "DRY/WET", "drywet" } })
    {
        const auto c = s.k->getBounds().toFloat().getCentre();
        knobScale (g, c, 22.0f, { { 0.0f, {} }, { 0.5f, {} }, { 1.0f, {} } }, false);
        auto* prm = proc.apvts.getParameter (s.id);
        print (g, s.name, Theme::stamp (15.0f), R (c.x + 32, c.y - 17, 90, 18), juce::Justification::centredLeft, Theme::ink, 0.5f);
        print (g, prm->getCurrentValueAsText() + (juce::String (s.id) == "space" || juce::String (s.id) == "drywet" ? " %" : " dB"), Theme::mono (10.0f),
              R (c.x + 32, c.y + 1, 90, 14), juce::Justification::centredLeft, Theme::ink);
    }

    // The five causes.
    paintSheet (g);

    // LIFT
    {
        std::vector<std::pair<float, juce::String>> m;
        for (int i = 0; i <= 10; ++i) m.push_back ({ (float) i / 10.0f, juce::String (i) });
        knobScale (g, kLiftC, kLiftR, m, true);
        print (g, "LIFT", Theme::stamp (30.0f), R (930, 566, 70, 46), juce::Justification::centredLeft, Theme::ink);
        const R led (1000, 568, 92, 42);
        g.setColour (juce::Colours::black); g.fillRoundedRectangle (led.expanded (3), 4);
        g.setColour (Theme::ledWindow); g.fillRoundedRectangle (led, 3);
        const auto v = juce::String (lift.getValue(), 1);
        print (g, "8.8", Theme::mono (32.0f), led.withTrimmedRight (10), juce::Justification::centredRight, Theme::led.withAlpha (0.07f));
        print (g, v, Theme::mono (32.0f), led.withTrimmedRight (10), juce::Justification::centredRight, Theme::led);
    }

    print (g, "MOOD", Theme::stamp (24.0f), R (32, 456, 120, 40), juce::Justification::centredLeft, Theme::ink);

    // SubverseLab, bottom right, linked (signature 3): the icon only (owner 14 Nov 2026).
    {
        g.setOpacity (overLockup ? 1.0f : 0.92f);
        g.drawImage (mark, lockupIcon (mark), juce::RectanglePlacement::stretchToFit);
        g.setOpacity (1.0f);
    }
}

void BoebEditor::paintSheet (juce::Graphics& g)
{
  {
    juce::Graphics::ScopedSaveState s (g);
    g.addTransform (juce::AffineTransform::rotation (kSheetRot * kPi / 180.0f, kSheetDraw.getCentreX(), kSheetDraw.getCentreY()));
    g.addTransform (juce::AffineTransform::scale (kSheetScale).translated (kSheetDraw.getX(), kSheetDraw.getY()));
    g.setColour (juce::Colours::black.withAlpha (0.28f));
    g.fillPath (sheetEdge, juce::AffineTransform::translation (3, 5));
    g.setGradientFill (juce::ColourGradient (Theme::paper, 0, 0, Theme::paperDeep, 0, kSheet.getHeight(), false));
    g.fillPath (sheetEdge);

    const float x0 = 18.0f, w = kSheet.getWidth() - 36.0f;
    print (g, "WHY IT SOUNDS DULL", Theme::stamp (25.0f), R (x0, 10, w, 32), juce::Justification::centredLeft, Theme::ink, 0.3f);
    print (g, "FOUND " + kArrow + " NOW", Theme::mono (10.0f), R (x0, 14, w, 26), juce::Justification::centredRight, Theme::ink);
    g.setColour (Theme::ink);
    g.fillRect (R (x0, 44, w, 3));

    using namespace svl::boeb;
    struct Row { const char* name; float lo, hi, a, b; std::function<bool (const Diagnosis&)> has; std::function<float (const Diagnosis&)> get; bool sign; };
    const auto any = [] (const Diagnosis& d) { return d.valid; };
    const Row rows[5] = {
        { "LEVEL",      kLarGoodLo, kLarGoodHi, -10.0f, 10.0f, [] (const Diagnosis& d) { return d.valid && d.hasBeat; }, [] (const Diagnosis& d) { return d.larDb; }, true },
        { "WORDS DROP", 0.0f, kRefSpreadDb + 2.0f, 0.0f, 20.0f, any, [] (const Diagnosis& d) { return d.spreadDb; }, false },
        { "PRESENCE",   kRefPres - kRefTolDb, kRefPres + kRefTolDb, -16.0f, 0.0f, any, [] (const Diagnosis& d) { return d.presDb; }, true },
        { "MUD",        kRefMud - kRefTolDb, kRefMud + kRefTolDb, -6.0f, 8.0f, any, [] (const Diagnosis& d) { return d.mudDb; }, true },
        { "AIR",        kRefAir - kRefTolDb, kRefAir + kRefTolDb, -40.0f, -10.0f, any, [] (const Diagnosis& d) { return d.airDb; }, true },
    };
    const bool fHave = shown.haveFound, nHave = shown.haveNow;
    int typedF = (int) typeFound, typedN = (int) typeNow;
    for (int i = 0; i < 5; ++i)
    {
        const auto& r = rows[i];
        const float y = 52.0f + 36.0f * (float) i, cy = y + 18.0f;
        const bool fOk = fHave && r.has (shown.found), nOk = nHave && r.has (shown.now);
        const float fv = fOk ? r.get (shown.found) : 0.0f, nv = nOk ? r.get (shown.now) : 0.0f;
        const auto inside = [&] (float v) { return v >= r.lo && v <= r.hi; };
        const int lampKind = nOk ? (inside (nv) ? 2 : 1) : fOk ? (inside (fv) ? 2 : 1) : 0;
        lamp (g, { x0 + 7.0f, cy }, 6.5f, lampKind);
        print (g, r.name, Theme::stamp (19.0f), R (x0 + 22, y, 130, 36), juce::Justification::centredLeft, Theme::ink, 0.3f);

        // scale: the reference band in sleeve red, found (open) -> now (filled)
        const R sc (x0 + 150, cy - 10, 140, 20);
        const auto X = [&] (float v) { return sc.getX() + juce::jlimit (0.0f, 1.0f, (v - r.a) / (r.b - r.a)) * sc.getWidth(); };
        g.setColour (Theme::sleeve.withAlpha (0.88f));
        g.fillRect (R (X (r.lo), sc.getY() + 3, std::max (4.0f, X (r.hi) - X (r.lo)), sc.getHeight() - 6));
        g.setColour (Theme::ink);
        g.drawLine (sc.getX(), cy, sc.getRight(), cy, 1.5f);
        for (int k = 0; k <= 10; ++k)
        {
            const float x = sc.getX() + sc.getWidth() * (float) k / 10.0f, h = k % 5 ? 3.0f : 6.0f;
            g.drawLine (x, cy - h, x, cy + h, 1.0f);
        }
        if (fOk && nOk)
        {
            const float dashes[] = { 3.0f, 2.0f };
            g.drawDashedLine (juce::Line<float> (X (fv), cy, X (nv), cy), dashes, 2, 2.0f);
        }
        if (fOk && typedF > 0)
        {
            g.setColour (Theme::paper); g.fillEllipse (X (fv) - 5, cy - 5, 10, 10);
            g.setColour (Theme::ink); g.drawEllipse (X (fv) - 5, cy - 5, 10, 10, 2.0f);
        }
        if (nOk && typedN > 0) { g.setColour (Theme::ink); g.fillEllipse (X (nv) - 5.5f, cy - 5.5f, 11, 11); }

        // numbers, typed in
        const auto typed = [] (const juce::String& str, int& budget) { const int n = juce::jlimit (0, str.length(), budget); budget -= str.length(); return str.substring (0, n); };
        const juce::String fs = fOk ? num (fv, r.sign) : (fHave && i == 0 ? juce::String ("NO BEAT") : juce::String ("--.-"));
        const juce::String ns = nOk ? num (nv, r.sign) : juce::String ("--.-");
        print (g, fOk || (fHave && i == 0) ? typed (fs, typedF) : fs, Theme::mono (fOk ? 13.5f : 11.0f), R (x0 + 286, y, 56, 36),
              juce::Justification::centredRight, fOk ? Theme::paperInk2 : Theme::paperInk2.withAlpha (0.5f));
        print (g, nOk ? typed (ns, typedN) : ns, Theme::mono (13.5f), R (x0 + 330, y, 52, 36), juce::Justification::centredRight,
              nOk ? Theme::ink : Theme::paperInk2.withAlpha (0.5f));
        if (i < 4)
        {
            const float dashes[] = { 4.0f, 3.0f };
            g.setColour (Theme::ink.withAlpha (0.3f));
            g.drawDashedLine (juce::Line<float> (x0, y + 36, x0 + w, y + 36), dashes, 2, 1.2f);
        }
    }
  }
    tapeStrip (g, { kSheetDraw.getX() + 12, kSheetDraw.getY() + 10 }, 86, 24, -30);
    tapeStrip (g, { kSheetDraw.getRight() - 12, kSheetDraw.getY() + 10 }, 86, 24, 30);
}

void BoebEditor::paintOverChildren (juce::Graphics& g)
{
    if (helpOpen) paintHelp (g);
}

void BoebEditor::paintHelp (juce::Graphics& g)
{
    // Signature 4: help opens in place, over the panel; the window never grows.
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillAll();
    struct Note { R box; Pt to; const char* text; };
    const Note notes[] = {
        { R (290, 300, 250, 0),  { 262, 360 },  "LISTEN plays 6 s of your vocal (with the beat, if you route it to the side input), measures the five causes and sets the channel. It starts by itself the first time." },
        { R (470, 200, 240, 0),  { 725, 160 },  "FOUND is what LISTEN measured, NOW the last 6 s after BOE-B. Red lamp: still outside the reference (the red band)." },
        { R (560, 370, 230, 0),  { 896, 452 },  "LIFT is the one knob. 6 = what LISTEN found, 0 = untouched, 10 = more of everything." },
        { R (600, 470, 240, 0),  { 1074, 386 }, "EQUAL compares at the same loudness: louder always sounds better, this shows it is not just level." },
        { R (300, 560, 250, 0),  { 294, 560 },  "MOOD is the colour of the whole channel. RADIO, PHONE and MEGAPHONE are for intros and hooks." },
        { R (890, 616, 220, 0),  { 1150, 596 }, "DRY/WET: 100 % = all BOE-B. Turn it down if LISTEN lifts too much." },
        { R (860, 150, 230, 0),  { 1074, 458 }, "THROW: hold it and the last word is thrown into an echo in time with the song. The dry vocal is untouched." },
    };
    const auto f = Theme::mono (11.0f);
    for (const auto& n : notes)
    {
        juce::GlyphArrangement ga;
        ga.addJustifiedText (f, n.text, n.box.getX() + 12, n.box.getY() + 10 + f.getAscent(), n.box.getWidth() - 24, juce::Justification::left);
        const auto box = n.box.withHeight (ga.getBoundingBox (0, -1, true).getBottom() - n.box.getY() + 10);
        g.setColour (Theme::ink);
        g.drawLine (juce::Line<float> (box.getCentre(), n.to), 2.0f);
        g.fillEllipse (n.to.x - 4, n.to.y - 4, 8, 8);
        g.setColour (juce::Colours::black.withAlpha (0.3f));
        g.fillRect (box.translated (3, 4));
        g.setColour (Theme::paper);
        g.fillRect (box);
        g.setColour (Theme::ink);
        ga.draw (g);
    }
    print (g, "CLICK ANYWHERE OR PRESS ESC TO CLOSE", Theme::mono (11.0f), R (0, 650, (float) kW, 20), juce::Justification::centred, Theme::paper);
}

//==============================================================================
bool BoebEditor::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey && helpOpen) { setHelpOpen (false); return true; }
    return svl::handleUndoKey (proc.undo, k);
}

void BoebEditor::mouseDown (const juce::MouseEvent& e)
{
    svl::takeFocusOnClick (*this, e);
    if (helpOpen && e.originalComponent != &help) setHelpOpen (false);
}

void BoebEditor::mouseUp (const juce::MouseEvent& e)
{
    // The panel's one outbound link: the icon opens BOE-B's HTML manual (owner rule 14 Nov 2026).
    if (e.originalComponent == this && lockupIcon (mark).contains (e.position) && e.mouseWasClicked())
        juce::URL ("https://subverselab.com/plugins/boeb/manual").launchInDefaultBrowser();
}

void BoebEditor::mouseMove (const juce::MouseEvent& e)
{
    const bool over = e.originalComponent == this && lockupIcon (mark).contains (e.position);
    if (over != overLockup) { overLockup = over; setMouseCursor (over ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor); repaint (kLockup.toNearestInt()); }
}
