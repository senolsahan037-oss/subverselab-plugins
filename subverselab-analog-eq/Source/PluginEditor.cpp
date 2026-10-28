#include "PluginEditor.h"

using namespace torna::ui;
using fam::rgb;

namespace
{
const juce::Colour kInk { 0xffE2E4E0 }, kInkDim { 0xffA8B2AC }, kCellLine { 0xff5C7872 }, kSlot { 0xff040606 }, kPlateLo { 0xff021e1c };
const juce::Colour kAqua { 0xff78EEEC }, kAquaUnlit { 0xff101e3e }, kAquaText { 0xff6EBEC8 }, kCobalt { 0xff2868FF }, kCobaltHi { 0xffAAD2FF };
const juce::Colour kCapRed { 0xffFF3E60 }, kAmber { 0xffE9C97E };
constexpr float kPitchL = 41.0f, kPitchR = 40.6f;
inline float xsL (int i) { return 370.0f + kPitchL * (float) i; }
inline float xsR (int i) { return 1245.0f + kPitchR * (float) i; }
inline float slotW (int ch) { return (ch == 0 ? kPitchL : kPitchR) * 0.42f; }
const float kLeftCuts[] = { 352, 449, 599, 654, 713, 771, 838 };
const float kRightCuts[] = { 1222, 1283, 1344, 1405, 1464, 1523, 1581, 1640, 1699 };

juce::Font mono (float em) { return Theme::monoEm (em * 1.1f); }
juce::Font legend (float em) { return Theme::legendEm (em * 1.1f); }
void silkD (juce::Graphics& g, const juce::String& s, juce::Font f, float dx, float dy, char a, float track, juce::Colour c) { fam::silk (g, s, f, X (dx), Y (dy), a, track, c); }
void silkD (juce::Graphics& g, const char* s, juce::Font f, float dx, float dy, char a, float track, juce::Colour c) { fam::silk (g, juce::String::fromUTF8 (s), f, X (dx), Y (dy), a, track, c); }
void lineD (juce::Graphics& g, float x0, float y0, float x1, float y1, juce::Colour c, float w) { g.setColour (c); g.drawLine (X (x0), Y (y0), X (x1), Y (y1), w); }

/** the glass windows of the centre bezel: dark recess, glass tinted top to bottom, a faint reflection, a thin light frame */
void glassWindow (juce::Graphics& g, float x0, float y0, float x1, float y1, juce::Colour top, juce::Colour bot)
{
    g.setColour (rgb (6, 7, 8)); g.fillRect (Rf (x0 - 6, y0 - 6, x1 + 6, y1 + 6));
    const auto b = Rf (x0, y0, x1, y1);
    g.setGradientFill (juce::ColourGradient (top, b.getX(), b.getY(), bot, b.getX(), b.getBottom(), false)); g.fillRect (b);
    juce::Path sheen; sheen.addQuadrilateral (b.getX(), b.getY(), b.getX() + b.getWidth() * 0.42f, b.getY(), b.getX() + b.getWidth() * 0.18f, b.getBottom(), b.getX(), b.getBottom());
    g.setColour (juce::Colours::white.withAlpha (0.055f)); g.fillPath (sheen);
    g.setColour (rgb (128, 136, 138)); g.drawRect (Rf (x0 - 3, y0 - 3, x1 + 3, y1 + 3), 0.9f);
}
}

// ======================================================================================================== CapSlider
CapSlider::CapSlider (TornaEditor& e, int c, int b) : ch (c), band (b), ed (e)
{
    setSliderStyle (juce::Slider::LinearVertical);
    setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    setSliderSnapsToMousePosition (false);
    setMouseDragSensitivity (300);
    setDoubleClickReturnValue (false, 0.0);
    setPopupDisplayEnabled (false, false, nullptr);
}

void CapSlider::tick()
{
    const float v = (float) getValue();
    if (isMouseButtonDown() || std::abs (v - shown) < 0.05f) { if (shown != v) { shown = v; repaint(); } return; }
    // UI signature 1: 30 % of the remaining distance per 30 Hz tick, at least one 0.1 dB step
    const float d = (v - shown) * 0.3f;
    shown += std::abs (d) < 0.1f ? (v > shown ? 0.1f : -0.1f) : d;
    if ((v - shown) * d < 0.0f) shown = v;
    repaint();
}

void CapSlider::paint (juce::Graphics& g)
{
    const float r = (float) getLookAndFeel().getSliderThumbRadius (*this);
    const float h = (float) getHeight(), w = (float) getWidth();
    const float y = r + (12.0f - shown) / 24.0f * (h - 2.0f * r);
    const float cw = slotW (ch) * 1.1f * 0.76f, ch2 = 21.0f * 1.1f;
    const juce::Rectangle<float> cap (w * 0.5f - cw * 0.5f, y - ch2, cw, ch2 * 2.0f);
    fam::softShadow (g, cap, 3.0f, 0.75f, { 2.0f, 7.0f }, 1.0f, false);
    g.setGradientFill (juce::ColourGradient (rgb (64, 67, 69), cap.getX(), cap.getY(), rgb (12, 13, 14), cap.getX(), cap.getY() + cap.getHeight() * 0.62f, false));
    g.fillRect (cap);
    g.setColour (juce::Colours::black); g.drawRect (cap, 1.0f);
    g.setColour (rgb (150, 156, 160)); g.drawLine (cap.getX() + 1, cap.getY() + 1, cap.getRight() - 1, cap.getY() + 1, 1.0f);   // the top bevel's edge
    g.setColour (rgb (60, 64, 68)); g.drawLine (cap.getX() + 1, cap.getY() + 2, cap.getX() + 1, cap.getBottom() - 1, 1.0f);
    g.setColour (rgb (8, 9, 10));
    for (float k : { -14.0f, -10.0f, 10.0f, 14.0f }) g.drawLine (cap.getX() + 2, y + k * 1.1f, cap.getRight() - 2, y + k * 1.1f, 0.8f);
    // the cap's LED: lit while EQ is IN and the unit is on (the original's "Christmas lights")
    const int lights = ed.proc.capLights.load();   // rear plate: ON, DIM, OFF
    const bool lit = ed.powered() && ed.proc.apvts.getRawParameterValue ("eqIn")->load() > 0.5f && lights < 2;
    const float cx = w * 0.5f;
    juce::Path led; led.addTriangle (cx - 2.9f, y + 2.4f, cx + 2.9f, y + 2.4f, cx, y - 3.5f);
    const float dim = lights == 1 ? 0.45f : 1.0f;
    if (lit) fam::glow (g, { cx, y }, 9.0f, kCapRed, 0.55f * dim);
    g.setColour (lit ? kCapRed.interpolatedWith (rgb (60, 14, 18), 1.0f - dim) : rgb (60, 14, 18)); g.fillPath (led);
    if (lit) { g.setColour (rgb (255, 220, 225)); g.fillEllipse (cx - 0.9f, y - 1.3f, 1.4f, 1.4f); }
}

void CapSlider::mouseDoubleClick (const juce::MouseEvent&)
{
    // UI signature 2: back to the memory last recalled, else 0 dB, asked now
    if (auto* p = ed.proc.apvts.getParameter (torna::sliderId (ch, band)))
        svl::setParamAsGesture (dynamic_cast<juce::RangedAudioParameter*> (p), ed.proc.sliderDefault (ch, band));
}

// ======================================================================================================== StripKey / SlideH
void StripKey::paintButton (juce::Graphics& g, bool over, bool down)
{
    auto b = getLocalBounds().toFloat().reduced (1.5f, 0.0f).translated (0.0f, down ? 1.0f : 0.0f);
    fam::softShadow (g, b, 1.5f, 0.5f, { 0.0f, 2.0f }, 1.0f, false);
    g.setGradientFill (juce::ColourGradient (rgb (22, 24, 25).brighter (over ? 0.08f : 0.0f), b.getX(), b.getY(), rgb (8, 9, 10), b.getX(), b.getBottom(), false));
    g.fillRect (b);
    g.setColour (juce::Colours::black); g.drawRect (b, 1.0f);
    g.setColour (rgb (34, 37, 39));
    for (int k = 1; k < 5; ++k) { const float yy = b.getY() + b.getHeight() * (float) k / 5.0f; g.drawLine (b.getX() + 4, yy, b.getRight() - 4, yy, 0.8f); }   // ribbing
    g.setColour (rgb (78, 84, 86)); g.drawLine (b.getX() + 2, b.getBottom() - 1, b.getRight() - 2, b.getBottom() - 1, 1.4f);
}

void SlideH::paint (juce::Graphics& g)
{
    const float r = (float) getLookAndFeel().getSliderThumbRadius (*this);
    const float w = (float) getWidth(), h = (float) getHeight();
    const float x = r + (float) valueToProportionOfLength (getValue()) * (w - 2.0f * r);
    const juce::Rectangle<float> cap (x - r, 0.0f, 2.0f * r, h);
    fam::softShadow (g, cap, 2.0f, 0.6f, { 1.5f, 3.0f }, 1.0f, false);
    if (steelCap)
    {
        g.setGradientFill (juce::ColourGradient (rgb (214, 218, 220), cap.getX(), cap.getY(), rgb (150, 154, 156), cap.getX(), cap.getBottom(), false));
        g.fillRect (cap);
        g.setColour (rgb (110, 114, 116));
        for (int k = 1; k < 8; ++k) g.drawLine (cap.getX() + 3, cap.getY() + h * (float) k / 8.0f, cap.getRight() - 3, cap.getY() + h * (float) k / 8.0f, 0.9f);
        g.setColour (rgb (30, 32, 34)); g.fillRect (x - 1.0f, cap.getY(), 2.0f, h);
        g.setColour (rgb (40, 42, 44)); g.drawRect (cap, 1.0f);
    }
    else
    {
        g.setGradientFill (juce::ColourGradient (rgb (64, 67, 69), cap.getX(), cap.getY(), rgb (12, 13, 14), cap.getX(), cap.getBottom(), false));
        g.fillRect (cap);
        g.setColour (juce::Colours::black); g.drawRect (cap, 1.0f);
        g.setColour (rgb (150, 156, 160)); g.drawLine (cap.getX() + 1, cap.getY() + 1, cap.getRight() - 1, cap.getY() + 1, 1.0f);
    }
}

void RearSwitch::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    const float slotY = b.getHeight() * 0.62f, w = b.getWidth(), n = (float) labels.size();
    for (int i = 0; i < labels.size(); ++i)
        fam::silk (g, labels[i], Theme::legendEm (9.5f), w * ((float) i + 0.5f) / n, b.getHeight() * 0.18f, 'm', 0.6f, kInk);
    const juce::Rectangle<float> slot (w * 0.5f / n - 10.0f, slotY - 6.0f, w * (n - 1.0f) / n + 20.0f, 12.0f);
    g.setColour (juce::Colours::black); g.fillRoundedRectangle (slot, 4.0f);
    g.setColour (rgb (60, 66, 66)); g.drawRoundedRectangle (slot, 4.0f, 1.0f);
    const int at = current ? current() : 0;
    const float cx = w * ((float) at + 0.5f) / n;
    const juce::Rectangle<float> knob (cx - 13.0f, slotY - 11.0f, 26.0f, 22.0f);
    fam::softShadow (g, knob, 2.0f, 0.6f, { 1.5f, 3.0f }, 2.0f, false);
    g.setGradientFill (juce::ColourGradient (rgb (220, 223, 224), knob.getX(), knob.getY(), rgb (132, 136, 138), knob.getX(), knob.getBottom(), false));
    g.fillRoundedRectangle (knob, 2.5f);
    g.setColour (rgb (96, 100, 102));
    for (int k = -2; k <= 2; ++k) g.drawLine (cx + (float) k * 4.0f, knob.getY() + 4.0f, cx + (float) k * 4.0f, knob.getBottom() - 4.0f, 1.0f);
    g.setColour (rgb (40, 42, 44)); g.drawRoundedRectangle (knob, 2.5f, 1.0f);
}

// ======================================================================================================== FLDisplay
void FLDisplay::paint (juce::Graphics& g)
{
    // local coordinates: the component covers the analyser glass Rf (882, 346, 1168, 482); draw in the face's units
    g.addTransform (juce::AffineTransform::translation (-X (882), -Y (346)));
    const int cols = 13; const float cx0 = 925, cx1 = 1127, pitch = (cx1 - cx0) / (float) (cols - 1);
    const int segN = 20; const float sy0 = 372, sy1 = 464, segH = (sy1 - sy0) / (float) segN;
    const bool pw = proc.apvts.getRawParameterValue ("power")->load() > 0.5f;
    for (int c = 0; c < cols; ++c)
    {
        const int lit = pw ? proc.analyserSegments (c) * 2 : 0;   // each comparator lights a pair of dashes
        const float x = cx0 + (float) c * pitch;
        for (int k = 0; k < segN; ++k)
        {
            const float y = sy1 - ((float) k + 0.5f) * segH;
            const auto b = Rf (x - 4.6f, y - segH * 0.26f, x + 4.6f, y + segH * 0.26f);
            if (k < lit) { g.setColour (kAqua.withAlpha (0.22f)); g.fillRect (b.expanded (2.5f, 1.5f)); g.setColour (kAqua); }
            else g.setColour (kAquaUnlit);
            g.fillRect (b);
        }
    }
    g.setColour (pw ? kAqua : kAquaUnlit);
    for (float side : { 902.0f, 1150.0f })
        for (int k = 0; k < segN; ++k) { const float y = sy1 - ((float) k + 0.5f) * segH; g.fillEllipse (Rf (side - 2.0f, y - 1.5f, side + 2.0f, y + 1.5f)); }
    const char* sc[] = { "+", "6", "3", "0", "3", "6", "9", "12", "15", "18", "24" };
    for (int i = 0; i < 11; ++i)
    {
        const float y = sy0 + (float) i * (sy1 - sy0) / 10.0f + 2.0f;
        silkD (g, sc[i], mono (5.6f), 891, y, 'm', 0.0f, kAquaText); silkD (g, sc[i], mono (5.6f), 1161, y, 'm', 0.0f, kAquaText);
    }
    const bool dl = proc.apvts.getRawParameterValue ("dispL")->load() > 0.5f, dr = proc.apvts.getRawParameterValue ("dispR")->load() > 0.5f;
    struct Pill { float x0, x1; const char* t; bool on; } pills[] = { { 965, 981, "L", pw && dl }, { 991, 1033, "DISPLAY", pw }, { 1043, 1059, "R", pw && dr }, { 1112, 1142, "TOTAL", pw } };
    for (auto& p : pills)
    {
        const auto b = Rf (p.x0, 354, p.x1, 364);
        if (p.on) { g.setColour (kAqua.withAlpha (0.25f)); g.fillRoundedRectangle (b.expanded (2.0f), 4.0f); }
        g.setColour (p.on ? kAqua : kAquaUnlit); g.fillRoundedRectangle (b, 3.0f);
        silkD (g, p.t, mono (5.2f), (p.x0 + p.x1) * 0.5f, 359, 'm', 0.0f, rgb (3, 8, 20));
    }
    const char* fl[] = { "16", "32", "63", "125", "250", "500", "1k", "2k", "4k", "8k", "16k", "32k", "dB" };
    for (int c = 0; c < cols; ++c) silkD (g, fl[c], mono (4.8f), cx0 + (float) c * pitch, 474, 'm', 0.0f, kAquaText);
}

// ======================================================================================================== CurveDisplay
CurveDisplay::CurveDisplay (TornaAudioProcessor& p) : proc (p)
{
    setInterceptsMouseClicks (false, false);
    for (int i = 0; i < 200; ++i) f.push_back (12.0 * std::pow (40000.0 / 12.0, (double) i / 199.0));
    std::array<double, 12> flat; flat.fill (0.5);
    flatRef = ge::responseDb (ge::buildContinuous (flat), { 1000.0 })[0];
}

void CurveDisplay::refreshIfChanged()
{
    const bool rev = proc.apvts.getRawParameterValue ("rev")->load() > 0.5f, eqIn = proc.apvts.getRawParameterValue ("eqIn")->load() > 0.5f;
    juce::String sig; sig << (rev ? "r" : "n") << (eqIn ? "i" : "o") << proc.presetIndex();
    std::array<std::array<double, 12>, 2> m { proc.marksFor (0), proc.marksFor (1) };
    for (auto& a : m) for (double v : a) sig << juce::String (v, 1) << ",";
    const bool fading = juce::Time::getMillisecondCounterHiRes() - readoutAt < 2800.0;   // the read-out fades, then the preset name fades in
    if (sig == signature) { if (fading) repaint(); return; }
    signature = sig;
    for (int c = 0; c < 2; ++c)
    {
        if (! eqIn) { db[(size_t) c].assign (f.size(), 0.0); continue; }
        std::array<double, 12> pos;
        for (int b = 0; b < 12; ++b) { pos[(size_t) b] = ge::positionForMark (b, m[(size_t) c][(size_t) b]); if (rev) pos[(size_t) b] = 1.0 - pos[(size_t) b]; }
        db[(size_t) c] = ge::responseDb (ge::buildContinuous (pos), f);
        for (auto& v : db[(size_t) c]) v -= flatRef;
    }
    repaint();
}

void CurveDisplay::paint (juce::Graphics& g)
{
    g.addTransform (juce::AffineTransform::translation (-X (886), -Y (516)));
    const float px0 = 904, px1 = 1140, py0 = 538, py1 = 614;
    auto fx = [&] (double fr) { return px0 + (float) ((std::log10 (fr) - std::log10 (12.0)) / (std::log10 (40000.0) - std::log10 (12.0))) * (px1 - px0); };
    auto dy = [&] (double v) { return (py0 + py1) * 0.5f - (float) juce::jlimit (-13.0, 13.0, v) / 12.0f * (py1 - py0) * 0.5f; };
    for (int v : { 12, 6, 0, -6, -12 })
    {
        lineD (g, px0, dy (v), px1, dy (v), v == 0 ? rgb (40, 70, 140) : rgb (16, 28, 60), v == 0 ? 1.1f : 0.7f);
        silkD (g, (v > 0 ? "+" : "") + juce::String (v), mono (5.6f), px1 + 6, dy (v), 'l', 0.0f, rgb (90, 130, 200));
    }
    for (double fc : { 16.0, 32.0, 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0, 32000.0 }) lineD (g, fx (fc), py0, fx (fc), py1, rgb (16, 28, 60), 0.6f);
    const std::pair<double, const char*> labs[] = { { 32, "32" }, { 125, "125" }, { 500, "500" }, { 2000, "2k" }, { 8000, "8k" }, { 32000, "32k" } };
    for (auto& [fc, t] : labs) silkD (g, t, mono (5.0f), fx (fc), py1 + 5.5f, 'm', 0.0f, rgb (90, 130, 200));
    if (db[0].empty()) return;
    auto path = [&] (int c) { juce::Path p; for (size_t i = 0; i < f.size(); ++i) { const float x = X (fx (f[i])), y = Y (dy (db[(size_t) c][i])); if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y); } return p; };
    auto pl = path (0), pr = path (1);
    juce::Path fill (pl); fill.lineTo (X (fx (f.back())), Y (dy (0))); fill.lineTo (X (fx (f.front())), Y (dy (0))); fill.closeSubPath();
    g.setColour (kCobalt.withAlpha (0.27f)); g.fillPath (fill);
    g.setColour (rgb (90, 140, 220)); g.strokePath (pr, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved));
    g.setColour (kCobalt.withAlpha (0.35f)); g.strokePath (pl, juce::PathStrokeType (7.0f, juce::PathStrokeType::curved));
    g.setColour (kCobaltHi); g.strokePath (pl, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved));
    lineD (g, 892, 531, 1158, 531, rgb (24, 40, 80), 0.8f);
    const double age = juce::Time::getMillisecondCounterHiRes() - readoutAt;
    if (age >= 2200.0)   // the preset's name once the read-out has gone (never both at once)
    {
        const int pi = proc.presetIndex();
        const float a = age < 2700.0 ? (float) ((age - 2200.0) / 500.0) : 1.0f;
        const juce::String tag = proc.isUserPreset (pi) ? juce::String::formatted ("U%02d  ", pi - torna::kNumPresets + 1) : juce::String::formatted ("P%02d  ", pi + 1);
        silkD (g, tag + proc.presetName (pi), mono (7.6f), 898, 523.5f, 'l', 0.0f, rgb (150, 200, 255).withAlpha (a * 0.85f));
    }
    if (age < 2200.0)
    {
        const float a = age < 1500.0 ? 1.0f : (float) (1.0 - (age - 1500.0) / 700.0);   // fades 1.5 s after the last move
        silkD (g, readout, mono (7.6f), 898, 523.5f, 'l', 0.0f, rgb (150, 200, 255).withAlpha (a));
    }
}

// ======================================================================================================== TornaEditor
TornaEditor::TornaEditor (TornaAudioProcessor& p) : juce::AudioProcessorEditor (p), proc (p)
{
    mark = juce::ImageCache::getFromMemory (BinaryData::mark_cream_png, BinaryData::mark_cream_pngSize);
    wordmark = juce::ImageCache::getFromMemory (BinaryData::wordmark_cream_png, BinaryData::wordmark_cream_pngSize);
    capLnF.r = (int) std::round (21.0f * 1.1f);
    steelLnF.r = (int) std::round (17.0f * 1.1f);
    levelLnF.r = (int) std::round (10.0f * 1.1f);
    addAndMakeVisible (face);
    face.onPaint = [this] (juce::Graphics& g)
    {
        if (rear) { if (rearBackground.isNull()) buildRear(); g.drawImage (rearBackground, face.getLocalBounds().toFloat()); return; }
        if (background.isNull()) buildBackground();
        g.drawImage (background, face.getLocalBounds().toFloat());
        paintLamps (g);
    };
    face.onPaintOver = [this] (juce::Graphics& g)
    {
        if (! helpOpen) return;
        // "?" help: short popovers drawn over the panel, never a taller window (UI signature 4)
        struct Tip { float x, y; const char* t; } tips[] = {
            { 330, 360, "24 sliders: \xc2\xb1" "12 dB per band. The bands overlap, as on the original. Double-click: back to the loaded preset or memory." },
            { 900, 300, "Analyzer: LEFT / RIGHT choose what it reads, LEVEL its sensitivity. It only draws." },
            { 900, 520, "Equalizer graphic: the true response of both channels, computed from the circuit." },
            { 900, 596, "PRESET \xe2\x97\x80 \xe2\x96\xb6: step through the presets. Hold \xe2\x96\xb6 1 s to save the sliders as your own (USER nn)." },
            { 360, 655, "ATT -6 dB: the original's input attenuator. NOR/REV reverses the curve. LINK: right follows left." },
            { 1230, 655, "MEMORY: press to recall, hold 1 s to store. COPY copies a bank; FLAT sets all sliders to 0 dB." },
            { 890, 655, "INPUT / OUTPUT: level before and after the equalizer." } };
        for (auto& t : tips)
        {
            const auto f = Theme::fineEm (11.0f);
            const float w = juce::jmin (300.0f, fam::textWidth (juce::String::fromUTF8 (t.t), f, 0.0f) + 24.0f);
            juce::Rectangle<float> b (X (t.x), Y (t.y), w, 46.0f);
            fam::softShadow (g, b, 4.0f, 0.5f, { 2.0f, 4.0f }, 6.0f, false);
            g.setColour (rgb (242, 239, 231)); g.fillRoundedRectangle (b, 6.0f);
            g.setColour (Theme::gold); g.drawRoundedRectangle (b, 6.0f, 1.2f);
            g.setColour (Theme::panel); g.setFont (f);
            g.drawFittedText (juce::String::fromUTF8 (t.t), b.reduced (10.0f, 4.0f).toNearestInt(), juce::Justification::centredLeft, 3);
        }
    };

    // ---- sliders
    for (int c = 0; c < 2; ++c)
        for (int b = 0; b < 12; ++b)
        {
            auto s = std::make_unique<CapSlider> (*this, c, b);
            s->setLookAndFeel (&capLnF);
            const float x = c == 0 ? xsL (b) : xsR (b), half = slotW (c) * 0.5f + 3.0f, r = (float) capLnF.r / 1.1f;
            s->setBounds (Ri (x - half, markY (12) - r, x + half, markY (-12) + r));
            attachments.push_back (std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, torna::sliderId (c, b), *s));
            s->shown = (float) s->getValue();
            auto* raw = s.get();
            s->onValueChange = [this, raw] { if (raw->isMouseButtonDown()) sliderMoved (raw->ch, raw->band, raw->getValue()); };
            face.addAndMakeVisible (*s);
            sliders[(size_t) (c * 12 + b)] = std::move (s);
        }
    // ---- horizontal slides
    auto addSlide = [this] (SlideH& s, ThumbLnF& lnf, const char* id, juce::Rectangle<int> b)
    {
        s.setLookAndFeel (&lnf); s.setBounds (b);
        attachments.push_back (std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, id, s));
        face.addAndMakeVisible (s);
    };
    addSlide (input, steelLnF, "input", Ri (898 - 17, 703, 898 + 124 + 17, 733));
    addSlide (output, steelLnF, "output", Ri (1040 - 17, 703, 1040 + 124 + 17, 733));
    input.setDoubleClickReturnValue (true, 0.0); output.setDoubleClickReturnValue (true, 0.0);
    addSlide (level, levelLnF, "level", Ri (945 - 10, 312, 1105 + 10, 332));
    level.setDoubleClickReturnValue (true, 0.2);

    // ---- keys
    auto addKey = [this] (const juce::String& name, juce::Rectangle<int> b, std::function<void (bool)> act)
    {
        auto* k = keys.add (new StripKey (name));
        k->setBounds (b); k->onRelease = std::move (act);
        face.addAndMakeVisible (k);
    };
    auto bar = [] (float xa, float xb) { return Ri (xa + 1.5f, 718, xb - 1.5f, 740); };
    addKey ("POWER", bar (kLeftCuts[0], kLeftCuts[1]), [this] (bool) { toggle ("power"); });
    addKey ("EQ IN/OUT", bar (kLeftCuts[2], kLeftCuts[3]), [this] (bool) { toggle ("eqIn"); });
    addKey ("ATT", bar (kLeftCuts[3], kLeftCuts[4]), [this] (bool) { toggle ("att"); });
    addKey ("NOR/REV", bar (kLeftCuts[4], kLeftCuts[5]), [this] (bool) { toggle ("rev"); });
    addKey ("LINK", bar (kLeftCuts[5], kLeftCuts[6]), [this] (bool) { proc.setLink (! on ("link")); });
    const char* mems[] = { "A", "B", "C" };
    for (int m = 0; m < 3; ++m)
        addKey (mems[m], bar (kRightCuts[m], kRightCuts[m + 1]), [this, m] (bool hold) { if (hold || ! proc.hasMemory (m)) proc.storeMemory (m); else proc.recallMemory (m); face.repaint(); });
    addKey ("L>R", bar (kRightCuts[3], kRightCuts[4]), [this] (bool) { proc.copyBank (0); });
    addKey ("R>L", bar (kRightCuts[4], kRightCuts[5]), [this] (bool) { proc.copyBank (1); });
    addKey ("FLAT", bar (kRightCuts[5], kRightCuts[6]), [this] (bool) { proc.flatAll(); });
    addKey ("ANALYZER", bar (kRightCuts[6], kRightCuts[7]), [this] (bool) { if (auto* p = param ("anaSrc")) svl::setParamAsGesture (p, on ("anaSrc") ? 0.0f : 1.0f); });
    addKey ("SIZE", bar (kRightCuts[7], kRightCuts[8]), [this] (bool) { setScalePct (scale > 0.8f ? 75 : 100); });
    addKey ("LEFT", Ri (885, 314, 921, 330), [this] (bool) { toggle ("dispL"); });
    addKey ("REAR", juce::Rectangle<int> (840, 568, 64, 22), [this] (bool) { showRear (! rear); });
    addKey ("PRESET<", Ri (898, 630, 940, 643), [this] (bool) { proc.stepPreset (-1); });
    addKey ("PRESET>", Ri (1110, 630, 1152, 643), [this] (bool hold)
    {
        if (! hold) { proc.stepPreset (1); return; }
        // held 1 s: the current sliders become the user's own preset (as MEMORY stores on a hold)
        const auto name = proc.saveUserPreset();
        curve.showReadout (name.isEmpty() ? juce::String ("COULD NOT SAVE") : "SAVED  " + name);
    });
    addKey ("RIGHT", Ri (1127, 314, 1163, 330), [this] (bool) { toggle ("dispR"); });

    // ---- displays, help, lockup
    fl.setBounds (Ri (882, 346, 1168, 482)); face.addAndMakeVisible (fl);
    curve.setBounds (Ri (886, 516, 1164, 626)); face.addAndMakeVisible (curve);
    help.setBounds (Ri (896, 792, 914, 810)); face.addAndMakeVisible (help);
    help.setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    help.setColour (juce::TextButton::textColourOffId, Theme::ink);
    help.onClick = [this] { helpOpen = ! helpOpen; face.repaint(); };
    lockup.setBounds ((int) (kW - 238.0f), (int) (kH - 34.0f), 210, 30); face.addAndMakeVisible (lockup);
    lockup.setTooltip ("subverselab.com");

    // ---- the rear plate's switches (shown with the rear)
    lineSwitch.setBounds (690, 214, 300, 70);
    lineSwitch.current = [this] { return juce::roundToInt (proc.apvts.getRawParameterValue ("lineLevel")->load()); };
    lineSwitch.onPick = [this] (int i) { if (auto* p = param ("lineLevel")) svl::setParamAsGesture (p, (float) i); lineSwitch.repaint(); };
    lightSwitch.setBounds (690, 354, 300, 70);
    lightSwitch.current = [this] { return proc.capLights.load(); };
    lightSwitch.onPick = [this] (int i) { proc.capLights = i; lightSwitch.repaint(); for (auto& sl : sliders) sl->repaint(); };
    face.addChildComponent (lineSwitch); face.addChildComponent (lightSwitch);

    addMouseListener (this, true);
    setWantsKeyboardFocus (true);
    setScalePct (proc.editorScalePct.load());
    curve.refreshIfChanged();
    startTimerHz (30);
}

TornaEditor::~TornaEditor()
{
    stopTimer();
    attachments.clear();   // before the sliders they listen to (input / output / level are declared after the vector)
    for (auto& s : sliders) s->setLookAndFeel (nullptr);
    input.setLookAndFeel (nullptr); output.setLookAndFeel (nullptr); level.setLookAndFeel (nullptr);
}

StripKey* TornaEditor::key (const juce::String& name) { for (auto* k : keys) if (k->getName() == name) return k; return nullptr; }

bool TornaEditor::powered() const { return proc.apvts.getRawParameterValue ("power")->load() > 0.5f; }

void TornaEditor::toggle (const char* id) { if (auto* p = param (id)) svl::setParamAsGesture (p, on (id) ? 0.0f : 1.0f); }

void TornaEditor::setScalePct (int pct)
{
    scale = pct >= 100 ? 1.0f : 0.75f;
    proc.editorScalePct = pct >= 100 ? 100 : 75;
    face.setBounds (0, 0, (int) kW, (int) kH);
    face.setTransform (juce::AffineTransform::scale (scale));
    setSize ((int) std::round (kW * scale), (int) std::round (kH * scale));
}

void TornaEditor::resized() {}

void TornaEditor::paint (juce::Graphics& g) { g.fillAll (juce::Colours::black); }
void TornaEditor::paintOverChildren (juce::Graphics&) {}

void TornaEditor::mouseDown (const juce::MouseEvent& e)
{
    svl::takeFocusOnClick (*this, e);
    if (helpOpen && e.originalComponent != &help) { helpOpen = false; face.repaint(); }
}

bool TornaEditor::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey && helpOpen) { helpOpen = false; face.repaint(); return true; }
    const bool cmd = k.getModifiers().isCommandDown();
    if (cmd && k.getKeyCode() == 'Z') { if (k.getModifiers().isShiftDown()) proc.undo.redo(); else proc.undo.undo(); return true; }
    if (cmd && k.getKeyCode() == 'Y') { proc.undo.redo(); return true; }
    return false;
}

void TornaEditor::sliderMoved (int c, int b, double v)
{
    proc.clearLastMemory();
    curve.showReadout ((c == 0 ? "L  " : "R  ") + juce::String (torna::kBandNames[b]) + "  " + (v > 0.0 ? "+" : "") + juce::String (v, 1) + " dB");
    if (on ("link") && ! linkGuard)   // LINK: a held slider moves its partner
    {
        const juce::ScopedValueSetter<bool> guard (linkGuard, true);
        if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (proc.apvts.getParameter (torna::sliderId (1 - c, b))))
            p->setValueNotifyingHost (p->convertTo0to1 ((float) v));
    }
}

void TornaEditor::timerCallback()
{
    for (auto& s : sliders) s->tick();
    fl.repaint();
    curve.refreshIfChanged();
    juce::String sig;
    for (auto* id : { "power", "eqIn", "att", "rev", "link", "anaSrc", "dispL", "dispR" }) sig << (on (id) ? "1" : "0");
    sig << proc.lastMemory() << (int) (scale * 100.0f);
    if (sig != lampSignature) { lampSignature = sig; face.repaint(); for (auto& s : sliders) s->repaint(); }
}

void TornaEditor::paintLamps (juce::Graphics& g)
{
    auto lamp = [&] (float dx, float dy, bool lit) { fam::drawLamp (g, X (dx), Y (dy), 3.1f, lit, Theme::ledRed, rgb (70, 16, 12)); };
    lamp (626, 690, on ("eqIn") && powered());
    lamp (742, 690, on ("rev") && powered());
    lamp (804, 690, on ("link") && powered());
    lamp (1593, 674, ! on ("anaSrc") && powered()); lamp (1593, 690, on ("anaSrc") && powered());
    lamp (1652, 674, scale < 0.8f && powered()); lamp (1652, 690, scale > 0.8f && powered());
    for (int i = 0; i < 6; ++i)   // the selectors' amber triangles: MEMORY lights the memory last recalled or stored
    {
        const float xm = (kRightCuts[i] + kRightCuts[i + 1]) * 0.5f;
        const bool lit = i < 3 && proc.lastMemory() == i && powered();
        juce::Path t; t.addTriangle (X (xm - 4), Y (686), X (xm + 4), Y (686), X (xm), Y (694));
        if (lit) fam::glow (g, { X (xm), Y (689) }, 10.0f, kAmber, 0.5f);
        g.setColour (lit ? kAmber : rgb (70, 60, 36)); g.fillPath (t);
    }
}

// ======================================================================================================== the static face
void TornaEditor::buildBackground()
{
    background = juce::Image (juce::Image::RGB, (int) kW * 2, (int) kH * 2, true);
    juce::Graphics g (background);
    g.addTransform (juce::AffineTransform::scale (2.0f));
    g.fillAll (rgb (1, 14, 13));
    const float faceBottom = Y (779);
    g.setColour (rgb (2, 37, 35)); g.fillRect (Rf (295, 650, 1750, 779));

    // ---- the blocks: the family's brushed plate, a soft shadow, lit bevels
    auto block = [&] (juce::Rectangle<float> b, int seed)
    {
        fam::softShadow (g, b, 5.0f, 0.75f, { 3.0f, 6.0f }, 0.0f, false);
        const auto img = fam::brushedPlate ((int) b.getWidth(), (int) b.getHeight(), rgb (6, 66, 61), rgb (2, 42, 39), seed);
        g.drawImage (img, b);
        g.setColour (rgb (40, 110, 102)); g.drawLine (b.getX(), b.getY() + 0.8f, b.getRight(), b.getY() + 0.8f, 1.6f);
        g.setColour (rgb (22, 86, 80)); g.drawLine (b.getX() + 0.5f, b.getY(), b.getX() + 0.5f, b.getBottom(), 1.0f);
        g.setColour (rgb (1, 20, 18)); g.drawLine (b.getX(), b.getBottom() - 0.9f, b.getRight(), b.getBottom() - 0.9f, 1.8f);
        g.setColour (rgb (1, 24, 22)); g.drawLine (b.getRight() - 0.6f, b.getY(), b.getRight() - 0.6f, b.getBottom(), 1.2f);
    };
    block (Rf (296, 273, 859, 645), 11); block (Rf (1191, 273, 1749, 645), 23);
    for (auto [xa, xb] : { std::pair<float, float> { 296, 859 }, { 866, 1184 }, { 1191, 1749 } }) { g.setColour (kPlateLo); g.drawRect (Rf (xa, 652, xb, 777), 1.0f); }

    // ---- the centre bezel: black smoked plastic, rounded, its own top light
    fam::softShadow (g, Rf (866, 278, 1184, 645), 5.0f, 0.75f, { 3.0f, 6.0f }, 9.0f, false);
    g.setColour (juce::Colours::black); g.fillRoundedRectangle (Rf (864, 276, 1186, 647), 10.0f);
    g.setColour (rgb (17, 19, 20)); g.fillRoundedRectangle (Rf (866, 278, 1184, 645), 9.0f);
    lineD (g, 874, 278.6f, 1176, 278.6f, rgb (70, 74, 76), 1.2f);
    g.setColour (rgb (9, 10, 11)); g.fillRoundedRectangle (Rf (876, 286, 1174, 334), 4.0f);
    lineD (g, 876, 334, 1174, 334, rgb (52, 56, 58), 1.0f);
    silkD (g, "SPECTRUM ANALYZER", legend (6.6f), 1025, 293, 'm', 1.0f, kInk);
    silkD (g, "LEFT", legend (6.4f), 903, 305, 'm', 0.4f, kInk); silkD (g, "RIGHT", legend (6.4f), 1146, 305, 'm', 0.4f, kInk);
    silkD (g, "LOW", legend (6.0f), 955, 305, 'm', 0.4f, kInkDim); silkD (g, "LEVEL", legend (6.4f), 1025, 303, 'm', 0.6f, kInk); silkD (g, "HIGH", legend (6.0f), 1095, 305, 'm', 0.4f, kInkDim);
    g.setColour (juce::Colours::black); g.fillRect (Rf (945, 320, 1105, 325));
    for (int k = 0; k < 17; ++k) lineD (g, 950 + 9.4f * (float) k, 313, 950 + 9.4f * (float) k, 316, kInkDim, 0.8f);
    glassWindow (g, 882, 346, 1168, 482, rgb (8, 18, 40), rgb (3, 8, 20));
    silkD (g, "SPECTRUM ANALYZER", legend (5.8f), 1025, 493, 'm', 0.9f, kInkDim);
    silkD (g, "EQUALIZER GRAPHIC", legend (5.8f), 1025, 506, 'm', 0.9f, kInkDim);
    glassWindow (g, 886, 516, 1164, 626, rgb (6, 10, 22), rgb (3, 5, 12));
    silkD (g, "\xe2\x97\x80  PRESET  \xe2\x96\xb6", legend (5.8f), 1025, 637, 'm', 0.9f, kInkDim);

    // ---- the slider banks: Hz labels, scale numbers, the short white ticks between wide black slots, LEFT / RIGHT
    for (int c = 0; c < 2; ++c)
    {
        const float xl = c == 0 ? 296.0f : 1191.0f, xr = c == 0 ? 859.0f : 1749.0f, pitch = c == 0 ? kPitchL : kPitchR, sw = slotW (c);
        auto xs = [c] (int i) { return c == 0 ? xsL (i) : xsR (i); };
        for (int m = 12; m >= -12; m -= 2)
        {
            const juce::String lab (std::abs (m));
            silkD (g, lab, mono (7.4f), xl + 21, markY ((float) m), 'm', 0.0f, kInk); silkD (g, lab, mono (7.4f), xr - 21, markY ((float) m), 'm', 0.0f, kInk);
        }
        silkD (g, "+", mono (8), xl + 21, markY (12) - 12, 'm', 0.0f, kInk); silkD (g, "+", mono (8), xr - 21, markY (12) - 12, 'm', 0.0f, kInk);
        silkD (g, "-", mono (8), xl + 21, markY (-12) + 12, 'm', 0.0f, kInk); silkD (g, "-", mono (8), xr - 21, markY (-12) + 12, 'm', 0.0f, kInk);
        for (int i = -1; i < 12; ++i)
        {
            float x0 = (i < 0 ? xs (0) - pitch : xs (i)) + sw * 0.5f + 3.2f, x1 = (i < 11 ? xs (i + 1) : xs (11) + pitch) - sw * 0.5f - 3.2f;
            if (i < 0) x0 = juce::jmax (x0, xl + 32); if (i == 11) x1 = juce::jmin (x1, xr - 32);
            for (int m = 12; m >= -12; m -= 2) lineD (g, x0, markY ((float) m), x1, markY ((float) m), kInk, 1.15f);
        }
        for (int i = 0; i < 12; ++i)
        {
            silkD (g, torna::kBandNames[i], mono (6.6f), xs (i), 337, 'm', 0.0f, kInk);
            g.setColour (kSlot); g.fillRect (Rf (xs (i) - sw * 0.5f, kSlotTop, xs (i) + sw * 0.5f, kSlotBot));
            g.setGradientFill (juce::ColourGradient (juce::Colours::black, X (xs (i)), Y (kSlotTop), kSlot, X (xs (i)), Y (kSlotTop + 14), false));
            g.fillRect (Rf (xs (i) - sw * 0.5f, kSlotTop, xs (i) + sw * 0.5f, kSlotTop + 14));
        }
        const float xm = (xl + xr) * 0.5f;
        lineD (g, xl + 66, 636, xm - 24, 636, kInkDim, 1.0f); lineD (g, xm + 24, 636, xr - 66, 636, kInkDim, 1.0f);
        lineD (g, xl + 66, 630, xl + 66, 636, kInkDim, 1.0f); lineD (g, xr - 66, 630, xr - 66, 636, kInkDim, 1.0f);
        silkD (g, c == 0 ? "LEFT" : "RIGHT", legend (6.6f), xm, 636, 'm', 1.4f, kInk);
    }

    // ---- name and model where the original prints its maker and model (the family's stroke letters)
    const float nh = 19.0f * 1.1f;
    const auto name = fam::familyName (Theme::kProductName, X (312), Y (282), nh);
    fam::paintFamilyName (g, name, Theme::ink);
    const float xe = (X (312) + fam::familyNameWidth (Theme::kProductName, nh)) / 1.1f + 295.0f;
    silkD (g, Theme::kSubtitle, legend (7.2f), xe + 14, 290, 'l', 1.6f, kInk);
    g.setColour (Theme::gold); g.drawRect (Rf (xe + 14, 299, xe + 86, 311), 1.3f);
    silkD (g, Theme::kModelCode, mono (6.6f), xe + 50, 305, 'm', 1.0f, Theme::gold);

    // ---- key strip cells and legends
    auto cells = [&] (const float* cuts, int n, std::initializer_list<float> fullHeight)
    {
        g.setColour (kCellLine); g.drawRect (Rf (cuts[0], 664, cuts[n - 1], 716), 1.0f);
        for (int i = 1; i < n - 1; ++i)
        {
            bool full = fullHeight.size() == 0 || std::find (fullHeight.begin(), fullHeight.end(), cuts[i]) != fullHeight.end();
            lineD (g, cuts[i], full ? 664.0f : 677.0f, cuts[i], 716, kCellLine, 0.9f);
        }
    };
    cells (kLeftCuts, 7, {});
    g.setColour (rgb (12, 13, 14)); g.fillRect (Rf (kLeftCuts[1] + 1.5f, 718, kLeftCuts[2] - 1.5f, 740));   // the bar under the text plate (no key)
    silkD (g, "POWER", legend (5.6f), 400, 708, 'm', 0.4f, kInk);
    silkD (g, "\xe2\x96\xa3 ON", mono (5.4f), 400, 680, 'm', 0.0f, kInkDim); silkD (g, "\xe2\x96\xa1 OFF", mono (5.4f), 400, 690, 'm', 0.0f, kInkDim);
    silkD (g, "12 POINT EQUALIZER WITH", legend (5.4f), 524, 677, 'm', 0.4f, kInk); silkD (g, "SPECTRUM ANALYZER", legend (5.4f), 524, 687, 'm', 0.4f, kInk);
    silkD (g, "IN", legend (5.6f), 626, 678, 'm', 0.4f, kInk); silkD (g, "EQ IN/OUT", legend (5.6f), 626, 708, 'm', 0.2f, kInk);
    silkD (g, "\xe2\x96\xa3 ON", mono (5.4f), 683, 680, 'm', 0.0f, kInkDim); silkD (g, "\xe2\x96\xa1 OFF", mono (5.4f), 683, 690, 'm', 0.0f, kInkDim);
    silkD (g, "ATT -6dB", legend (5.6f), 683, 708, 'm', 0.2f, kInk);
    silkD (g, "REVERSE", legend (5.6f), 742, 678, 'm', 0.2f, kInk); silkD (g, "NOR/REV", legend (5.6f), 742, 708, 'm', 0.2f, kInk);
    silkD (g, "L = R", legend (5.6f), 804, 678, 'm', 0.2f, kInk); silkD (g, "LINK", legend (5.6f), 804, 708, 'm', 0.4f, kInk);
    // centre: INPUT / OUTPUT slides
    g.setColour (kCellLine); g.drawRect (Rf (884, 664, 1176, 744), 1.0f);
    for (auto [xa, nm] : { std::pair<float, const char*> { 898, "INPUT" }, { 1040, "OUTPUT" } })
    {
        const float xb = xa + 124, xm = (xa + xb) * 0.5f;
        silkD (g, nm, legend (5.8f), xm, 671, 'm', 0.6f, kInk);
        silkD (g, "-12", mono (5.2f), xa, 690, 'm', 0.0f, kInkDim); silkD (g, "0", mono (5.2f), xm + 9, 690, 'm', 0.0f, kInkDim); silkD (g, "+12", mono (5.2f), xb, 690, 'm', 0.0f, kInkDim);
        for (int k = 0; k < 13; ++k) lineD (g, xa + 124.0f * (float) k / 12.0f, k % 6 ? 677.0f : 675.0f, xa + 124.0f * (float) k / 12.0f, 682, kInkDim, 0.8f);
        g.setColour (kSlot); g.fillRect (Rf (xa, 700, xb, 736));
    }
    // right: MEMORY, COPY, ANALYZER, SIZE
    cells (kRightCuts, 9, { 1405.0f, 1581.0f, 1640.0f });
    lineD (g, 1222, 677, 1581, 677, kCellLine, 0.9f);
    silkD (g, "MEMORY", legend (5.6f), 1313, 671, 'm', 0.6f, kInk); silkD (g, "COPY", legend (5.6f), 1493, 671, 'm', 0.6f, kInk);
    const char* labs[] = { "A", "B", "C", "L \xe2\x96\xb8 R", "R \xe2\x96\xb8 L", "FLAT", "ANALYZER", "SIZE" };
    for (int i = 0; i < 8; ++i) silkD (g, juce::String::fromUTF8 (labs[i]), legend (5.6f), (kRightCuts[i] + kRightCuts[i + 1]) * 0.5f, 708, 'm', 0.2f, kInk);
    silkD (g, "OUT", mono (5.2f), 1614, 674, 'm', 0.0f, kInk); silkD (g, "IN", mono (5.2f), 1611, 690, 'm', 0.0f, kInkDim);
    silkD (g, "75", mono (5.2f), 1674, 674, 'm', 0.0f, kInk); silkD (g, "100", mono (5.2f), 1676, 690, 'm', 0.0f, kInkDim);

    // ---- footer: the family plate with its groove, screws, the gold plaque, the lockup
    const juce::Rectangle<float> foot (0.0f, faceBottom, kW, kH - faceBottom);
    g.drawImage (fam::brushedPlate ((int) foot.getWidth(), (int) foot.getHeight(), rgb (4, 52, 48), rgb (2, 36, 33), 77), foot);
    fam::groove (g, faceBottom + 1.0f, 0.0f, kW);
    const float fy = (faceBottom + kH) * 0.5f;
    fam::screw (g, 16.0f, fy, 7.0f); fam::screw (g, kW - 16.0f, fy, 7.0f);
    fam::silk (g, "MEASURED, NOT GUESSED", legend (5.8f), 38.0f, fy, 'l', 1.6f, kInkDim);
    fam::goldPlaque (g, juce::String::fromUTF8 (Theme::kPlaque), X (690), fy, 7.0f, 1.3f, 'm', 12.0f, 24.0f);
    fam::drawLogo (g, mark, wordmark, kW - 38.0f, fy, 18.0f, 26.0f);
    fam::silk (g, "REAR", Theme::legendEm (6.4f), 832.0f, fy, 'r', 1.6f, kInkDim);

    // ---- the family's vignette
    for (auto [b, x0, y0, x1, y1] : { std::tuple<juce::Rectangle<float>, float, float, float, float> { { 0, 0, kW, 70 }, 0, 0, 0, 70 },
                                        { { 0, kH - 70, kW, 70 }, 0, kH, 0, kH - 70 }, { { 0, 0, 90, kH }, 0, 0, 90, 0 }, { { kW - 90, 0, 90, kH }, kW, 0, kW - 90, 0 } })
    {
        g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.28f), x0, y0, juce::Colours::black.withAlpha (0.0f), x1, y1, false));
        g.fillRect (b);
    }
}

// ======================================================================================================== the rear plate
void TornaEditor::showRear (bool r)
{
    rear = r;
    for (auto* c : face.getChildren())
        if (c != &help && c != &lockup && c != key ("REAR")) c->setVisible (! r);
    lineSwitch.setVisible (r); lightSwitch.setVisible (r);
    face.repaint();
}

void TornaEditor::buildRear()
{
    // the original's back as its owner's manual draws it (RCA jack groups on the left, the unswitched AC outlet and the cord on
    // the right), on the family's plate; the two working controls look like the rear's slide switch; the family's cream label
    rearBackground = juce::Image (juce::Image::RGB, (int) kW * 2, (int) kH * 2, true);
    juce::Graphics g (rearBackground);
    g.addTransform (juce::AffineTransform::scale (2.0f));
    const float faceBottom = Y (779);
    g.drawImage (fam::brushedPlate ((int) kW, (int) faceBottom, rgb (4, 46, 43), rgb (1, 26, 24), 91), { 0.0f, 0.0f, kW, faceBottom });
    // ventilation slots along the top
    for (int i = 0; i < 46; ++i)
    {
        const juce::Rectangle<float> v (60.0f + (float) i * 32.0f, 26.0f, 18.0f, 54.0f);
        g.setColour (juce::Colours::black); g.fillRoundedRectangle (v, 8.0f);
        g.setColour (rgb (40, 90, 84)); g.drawLine (v.getX() + 4.0f, v.getBottom() + 0.8f, v.getRight() - 4.0f, v.getBottom() + 0.8f, 1.0f);
    }
    // the jack board: six groups of two RCA jacks (L white, R red), each in its printed box
    const juce::Rectangle<float> board (70.0f, 130.0f, 560.0f, 330.0f);
    fam::softShadow (g, board, 4.0f, 0.6f, { 2.0f, 4.0f }, 3.0f, false);
    g.setColour (rgb (14, 16, 17)); g.fillRoundedRectangle (board, 3.0f);
    const char* groups[] = { "LINE\nIN", "LINE\nOUT", "TAPE A\nREC", "TAPE A\nPLAY", "TAPE B\nREC", "TAPE B\nPLAY" };
    for (int k = 0; k < 6; ++k)
    {
        const float x = board.getX() + 20.0f + (float) k * 90.0f;
        g.setColour (rgb (190, 196, 192)); g.drawRect (juce::Rectangle<float> (x, board.getY() + 18.0f, 80.0f, 294.0f), 1.0f);
        const auto lines = juce::StringArray::fromLines (groups[k]);
        for (int l = 0; l < lines.size(); ++l) fam::silk (g, lines[l], Theme::legendEm (9.0f), x + 40.0f, board.getY() + 42.0f + 14.0f * (float) l, 'm', 0.6f, kInk);
        for (int j = 0; j < 2; ++j)
        {
            const float cx = x + 40.0f, cy = board.getY() + 130.0f + 110.0f * (float) j, r = 24.0f;
            fam::softShadow (g, { cx - r, cy - r, 2 * r, 2 * r }, 2.0f, 0.6f, { 1.5f, 3.0f }, 0.0f, true);
            g.setGradientFill (juce::ColourGradient (rgb (236, 238, 236), cx - r, cy - r, rgb (110, 114, 114), cx + r, cy + r, false));
            g.fillEllipse (cx - r, cy - r, 2 * r, 2 * r);
            g.setColour (j == 0 ? rgb (236, 236, 228) : rgb (190, 40, 36)); g.fillEllipse (cx - r * 0.72f, cy - r * 0.72f, r * 1.44f, r * 1.44f);
            g.setGradientFill (juce::ColourGradient (rgb (210, 212, 210), cx - 9, cy - 9, rgb (90, 94, 94), cx + 9, cy + 9, false));
            g.fillEllipse (cx - 11.0f, cy - 11.0f, 22.0f, 22.0f);
            g.setColour (juce::Colours::black); g.fillEllipse (cx - 5.0f, cy - 5.0f, 10.0f, 10.0f);
            fam::silk (g, j == 0 ? "L" : "R", Theme::legendEm (8.0f), cx, cy + r + 12.0f, 'm', 0.0f, kInkDim);
        }
    }
    // the two working controls, framed like the rear's selector
    for (auto [y, t1, t2] : { std::tuple<float, const char*, const char*> { 190.0f, "LINE LEVEL", "0 dBFS AT THE INPUT  \xc2\xb7  THE OUTPUT STAGE CLIPS +19 / +13 / +7 dB ABOVE IT" },
                                { 330.0f, "CAP LIGHTS", "THE RED LIGHTS IN THE SLIDER CAPS" } })
    {
        g.setColour (rgb (190, 196, 192)); g.drawRect (juce::Rectangle<float> (680.0f, y - 6.0f, 320.0f, 110.0f), 1.0f);
        fam::silk (g, t1, Theme::legendEm (10.5f), 840.0f, y + 6.0f, 'm', 1.6f, kInk);
        fam::silk (g, juce::String::fromUTF8 (t2), Theme::fineEm (7.5f), 840.0f, y + 96.0f, 'm', 0.2f, kInkDim);
    }
    // AC outlet and the cord
    const juce::Rectangle<float> outlet (1090.0f, 160.0f, 120.0f, 74.0f);
    fam::silk (g, "UNSWITCHED", Theme::legendEm (8.0f), outlet.getCentreX(), outlet.getY() - 26.0f, 'm', 0.6f, kInk);
    fam::silk (g, "200W MAX", Theme::legendEm (8.0f), outlet.getCentreX(), outlet.getY() - 12.0f, 'm', 0.6f, kInk);
    g.setColour (rgb (30, 32, 33)); g.fillRoundedRectangle (outlet, 6.0f);
    g.setColour (rgb (70, 74, 76)); g.drawRoundedRectangle (outlet, 6.0f, 1.4f);
    for (float dx : { -24.0f, 24.0f }) { g.setColour (juce::Colours::black); g.fillRect (outlet.getCentreX() + dx - 4.0f, outlet.getCentreY() - 15.0f, 8.0f, 30.0f); }
    const float gx = 1515.0f, gy = 300.0f;
    g.setColour (rgb (22, 24, 25)); g.fillEllipse (gx - 26.0f, gy - 26.0f, 52.0f, 52.0f);
    g.setColour (rgb (8, 9, 10)); g.fillRoundedRectangle (gx - 11.0f, gy, 22.0f, faceBottom - gy, 6.0f);
    g.setColour (rgb (50, 54, 56)); g.drawLine (gx - 6.0f, gy + 4.0f, gx - 6.0f, faceBottom, 1.2f);
    fam::silk (g, "AC 220-240V 50Hz 30W", Theme::legendEm (8.0f), gx, gy - 44.0f, 'm', 0.4f, kInkDim);
    // the family's cream label
    const juce::Rectangle<float> label (1040.0f, 370.0f, 420.0f, 150.0f);
    fam::softShadow (g, label, 3.0f, 0.5f, { 1.5f, 3.0f }, 3.0f, false);
    g.setGradientFill (juce::ColourGradient (Theme::label, label.getX(), label.getY(), Theme::labelDeep, label.getX(), label.getBottom(), false));
    g.fillRoundedRectangle (label, 3.0f);
    const auto nm = fam::familyName (Theme::kProductName, label.getX() + 22.0f, label.getY() + 18.0f, 26.0f);
    fam::paintFamilyName (g, nm, Theme::panel);
    fam::silk (g, juce::String::fromUTF8 ("MODEL TR-83  \xc2\xb7  GRAPHIC EQUALIZER"), Theme::legendEm (10.0f), label.getX() + 22.0f, label.getY() + 66.0f, 'l', 1.2f, Theme::panel);
    fam::silk (g, "MODELLED PART BY PART FROM A 1983 SERVICE SCHEMATIC", Theme::fineEm (8.5f), label.getX() + 22.0f, label.getY() + 90.0f, 'l', 0.2f, Theme::panel);
    fam::silk (g, juce::String::fromUTF8 ("VST3  \xc2\xb7  AU  \xc2\xb7  LATENCY 64 SAMPLES  \xc2\xb7  44.1\xe2\x80\x93" "192 kHz"), Theme::fineEm (8.5f), label.getX() + 22.0f, label.getY() + 108.0f, 'l', 0.2f, Theme::panel);
    fam::silk (g, "SUBVERSELAB", Theme::legendEm (9.0f), label.getRight() - 22.0f, label.getBottom() - 18.0f, 'r', 2.0f, Theme::goldDark);
    for (auto [sx, sy] : { std::pair<float, float> { 24.0f, 120.0f }, { kW - 24.0f, 120.0f }, { 24.0f, faceBottom - 30.0f }, { kW - 24.0f, faceBottom - 30.0f } }) fam::screw (g, sx, sy, 7.0f);
    // footer, as the front's
    const juce::Rectangle<float> foot (0.0f, faceBottom, kW, kH - faceBottom);
    g.drawImage (fam::brushedPlate ((int) foot.getWidth(), (int) foot.getHeight(), rgb (4, 52, 48), rgb (2, 36, 33), 77), foot);
    fam::groove (g, faceBottom + 1.0f, 0.0f, kW);
    const float fy = (faceBottom + kH) * 0.5f;
    fam::screw (g, 16.0f, fy, 7.0f); fam::screw (g, kW - 16.0f, fy, 7.0f);
    fam::silk (g, "FRONT", Theme::legendEm (6.4f), 832.0f, fy, 'r', 1.6f, kInkDim);
    fam::drawLogo (g, mark, wordmark, kW - 38.0f, fy, 18.0f, 26.0f);
}
