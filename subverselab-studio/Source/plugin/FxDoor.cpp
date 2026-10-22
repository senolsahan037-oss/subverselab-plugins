#include "FxDoor.h"
#include <complex>

namespace jfui
{
namespace
{
constexpr float kKnobD = 46.0f;

/** the EQ's magnitude (dB) at f: the same RBJ shelves and peak the engine runs (SvlFx.h), evaluated on the message thread */
float eqDbAt (float f, float gl, float gm, float fm, float gh, float sr = 48000.0f)
{
    struct C { double b0, b1, b2, a0, a1, a2; };
    auto shelf = [sr] (double fc, double db, bool high)
    {
        const double A = std::pow (10.0, db / 40.0), w0 = 2.0 * juce::MathConstants<double>::pi * fc / sr, c = std::cos (w0);
        const double al = std::sin (w0) * 0.5 * std::sqrt (2.0), sa = 2.0 * std::sqrt (A) * al;
        if (! high) return C { A * ((A + 1) - (A - 1) * c + sa), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sa),
                               (A + 1) + (A - 1) * c + sa, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sa };
        return C { A * ((A + 1) + (A - 1) * c + sa), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sa),
                   (A + 1) - (A - 1) * c + sa, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sa };
    };
    auto peak = [sr] (double fc, double db)
    {
        const double A = std::pow (10.0, db / 40.0), w0 = 2.0 * juce::MathConstants<double>::pi * fc / sr, c = std::cos (w0), al = std::sin (w0) / 1.8;
        return C { 1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A };
    };
    auto mag = [f, sr] (const C& k)
    {
        const std::complex<double> z = std::polar (1.0, -2.0 * juce::MathConstants<double>::pi * f / sr);
        return std::abs ((k.b0 + k.b1 * z + k.b2 * z * z) / (k.a0 + k.a1 * z + k.a2 * z * z));
    };
    return (float) (20.0 * std::log10 (mag (shelf (120, gl, false)) * mag (peak (fm, gm)) * mag (shelf (6000, gh, true)) + 1e-12));
}
} // namespace

FxDoor::FxDoor (JingFangProcessor& p, std::function<Knob& (const juce::String&, const juce::String&, juce::Rectangle<int>, juce::Component*)> makeKnob,
                juce::Rectangle<int> area)
    : proc (p), full (area)
{
    setInterceptsMouseClicks (true, true);
    const float W = (float) area.getWidth(), H = (float) area.getHeight(), m = 14.0f;
    // the top row carries the two screens (EQ curve, COMP transfer + gain reduction), the bottom row only selectors and knobs
    const float mw = (W - 3 * m) * 0.5f, top = (H - 3 * m) * 0.6f, bottom = (H - 3 * m) - top;
    auto cell = [&] (int col, int row) { return juce::Rectangle<float> (m + col * (mw + m), row == 0 ? m : 2 * m + top, mw, row == 0 ? top : bottom); };
    modules = { { "EQ", cell (0, 0) }, { "COMP", cell (1, 0) }, { "MOD", cell (0, 1) }, { "ECHO", cell (1, 1) } };

    auto knobRow = [&] (const Module& mod, std::initializer_list<std::pair<const char*, const char*>> ks, float firstX = -1.0f)
    {
        const int n = (int) ks.size();
        const float y = mod.r.getBottom() - 62.0f;
        const float x0 = firstX >= 0 ? firstX : mod.r.getX() + 40.0f, x1 = mod.r.getRight() - 40.0f;
        int i = 0;
        for (auto& [id, text] : ks)
        {
            const float x = n == 1 ? 0.5f * (x0 + x1) : x0 + (x1 - x0) * (float) i / (float) (n - 1);
            const int box = juce::roundToInt (kKnobD + 8.0f);
            if (proc.apvts.getParameter (id) != nullptr)
                makeKnob (id, text, juce::Rectangle<int> (box, box).withCentre ({ juce::roundToInt (x), juce::roundToInt (y) }), this).setPrinted (true);
            labels.push_back ({ text, { x, y + kKnobD * 0.5f + 14.0f } });
            ++i;
        }
    };
    auto stepper = [&] (const char* id, juce::Rectangle<float> r)
    {
        auto* st = new TypeStepper (proc, id);
        owned.emplace_back (st); addAndMakeVisible (st); st->setBounds (r.toNearestInt());
    };
    const auto& eq = modules[0]; const auto& comp = modules[1]; const auto& mod = modules[2]; const auto& echo = modules[3];
    eqView = juce::Rectangle<float> (eq.r.getX() + 16.0f, eq.r.getY() + 34.0f, eq.r.getWidth() - 32.0f, eq.r.getHeight() - 140.0f);
    knobRow (eq, { { "fx.eq.low", "LOW" }, { "fx.eq.mid", "MID" }, { "fx.eq.mid_freq", "FREQ" }, { "fx.eq.high", "HIGH" } });
    compView = juce::Rectangle<float> (comp.r.getX() + 16.0f, comp.r.getY() + 34.0f, comp.r.getWidth() - 32.0f, comp.r.getHeight() - 140.0f - 30.0f);
    grView = juce::Rectangle<float> (compView.getX(), compView.getBottom() + 8.0f, compView.getWidth(), 22.0f);
    knobRow (comp, { { "fx.comp.threshold", "THRESH" }, { "fx.comp.ratio", "RATIO" }, { "fx.comp.attack", "ATTACK" }, { "fx.comp.release", "RELEASE" }, { "fx.comp.makeup", "MAKEUP" } });
    stepper ("fx.mod.type", { mod.r.getX() + 16.0f, mod.r.getY() + 34.0f, 160.0f, 22.0f });
    knobRow (mod, { { "fx.mod.rate", "RATE" }, { "fx.mod.depth", "DEPTH" }, { "fx.mod.feedback", "FDBK" }, { "fx.mod.mix", "MIX" } });
    stepper ("fx.echo.type", { echo.r.getX() + 16.0f, echo.r.getY() + 34.0f, 160.0f, 22.0f });
    stepper ("fx.echo.time", { echo.r.getX() + 186.0f, echo.r.getY() + 34.0f, 140.0f, 22.0f });
    knobRow (echo, { { "fx.echo.feedback", "FDBK" }, { "fx.echo.damp", "DAMP" }, { "fx.echo.mix", "MIX" } });
    layoutDoor();
}

void FxDoor::layoutDoor()
{
    setBounds (full.withHeight (juce::jmax (1, juce::roundToInt ((float) full.getHeight() * amount))));
    setVisible (amount > 0.001f);
}

void FxDoor::tick()
{
    if (amount != target)
    {
        const float d = target - amount;
        amount = std::abs (d) < 0.02f ? target : amount + d * 0.42f;   // ~6 frames at 30 Hz, as the other doors
        layoutDoor();
    }
    if (! isVisible()) return;
    juce::String e;
    e << proc.exactValue (svl::FXQ_LOW) << ":" << proc.exactValue (svl::FXQ_MID) << ":" << proc.exactValue (svl::FXQ_MIDF) << ":" << proc.exactValue (svl::FXQ_HIGH);
    if (e != lastEq) { lastEq = e; repaint (eqView.expanded (2.0f).toNearestInt()); }
    juce::String c;
    c << proc.exactValue (svl::FXC_THRESH) << ":" << proc.exactValue (svl::FXC_RATIO) << ":" << proc.exactValue (svl::FXC_MAKEUP);
    if (c != lastComp) { lastComp = c; repaint (compView.expanded (2.0f).toNearestInt()); }
    const float gr = juce::jlimit (0.0f, 24.0f, proc.fxReductionDb());
    if (std::abs (gr - shownGr) > 0.05f) { shownGr = gr; repaint (grView.expanded (2.0f).toNearestInt()); }
}

void FxDoor::paint (juce::Graphics& g)
{
    const auto plate = juce::Rectangle<float> (0.0f, 0.0f, (float) full.getWidth(), (float) full.getHeight());
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1a1718), 0.0f, 0.0f, juce::Colour (0xff0e0c0d), 0.0f, plate.getBottom(), false));
    g.fillRoundedRectangle (plate, 6.0f);
    g.setColour (juce::Colours::black.withAlpha (0.8f)); g.drawRoundedRectangle (plate.reduced (0.5f), 6.0f, 1.2f);
    for (const auto& mod : modules)
    {   // a recessed module: dark face, a hairline highlight on top, the title tracked like the panel's
        g.setColour (juce::Colour (0xff201c1d)); g.fillRoundedRectangle (mod.r, 5.0f);
        g.setColour (juce::Colours::white.withAlpha (0.07f)); g.drawHorizontalLine ((int) mod.r.getY() + 1, mod.r.getX() + 5.0f, mod.r.getRight() - 5.0f);
        g.setColour (juce::Colours::black.withAlpha (0.75f)); g.drawRoundedRectangle (mod.r, 5.0f, 1.0f);
        g.setColour (kKeyText);
        drawTracked (g, mod.title, { mod.r.getX() + 16.0f, mod.r.getY() + 8.0f, 200.0f, 16.0f }, heros (10.5f, true), 0.22f, juce::Justification::centredLeft);
    }
    g.setColour (kKeyTextDim);
    for (const auto& l : labels)
        drawTracked (g, l.text, juce::Rectangle<float> (90.0f, 12.0f).withCentre (l.at), heros (8.6f, true), 0.12f, juce::Justification::centred);

    // the EQ's curve: 20 Hz .. 20 kHz, +-12 dB
    {
        const auto v = eqView;
        g.setColour (juce::Colour (0xff0b0f0d)); g.fillRoundedRectangle (v, 3.0f);
        g.setColour (kLed.withAlpha (0.12f));
        for (float db : { -6.0f, 0.0f, 6.0f }) { const float y = v.getCentreY() - db / 12.0f * v.getHeight() * 0.45f; g.drawHorizontalLine ((int) y, v.getX(), v.getRight()); }
        for (float f : { 100.0f, 1000.0f, 10000.0f }) { const float x = v.getX() + v.getWidth() * std::log10 (f / 20.0f) / 3.0f; g.drawVerticalLine ((int) x, v.getY(), v.getBottom()); }
        const float gl = proc.exactValue (svl::FXQ_LOW), gm = proc.exactValue (svl::FXQ_MID), fm = proc.exactValue (svl::FXQ_MIDF), gh = proc.exactValue (svl::FXQ_HIGH);
        juce::Path p;
        for (int i = 0; i <= 160; ++i)
        {
            const float t = (float) i / 160.0f, f = 20.0f * std::pow (1000.0f, t);
            const float y = v.getCentreY() - juce::jlimit (-13.0f, 13.0f, eqDbAt (f, gl, gm, fm, gh)) / 12.0f * v.getHeight() * 0.45f;
            const float x = v.getX() + v.getWidth() * t;
            if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
        }
        g.setColour (kLed); g.strokePath (p, juce::PathStrokeType (1.6f));
    }
    // COMP: the transfer curve (input -48 .. 0 dB across, output down), the threshold, the same soft knee the engine runs
    {
        const auto v = compView;
        g.setColour (juce::Colour (0xff0b0f0d)); g.fillRoundedRectangle (v, 3.0f);
        auto px = [&] (float db) { return v.getX() + v.getWidth() * (db + 48.0f) / 48.0f; };
        auto py = [&] (float db) { return v.getBottom() - v.getHeight() * (juce::jlimit (-48.0f, 12.0f, db) + 48.0f) / 60.0f; };
        g.setColour (kLed.withAlpha (0.12f));
        for (float db : { -36.0f, -24.0f, -12.0f }) { g.drawVerticalLine ((int) px (db), v.getY(), v.getBottom()); g.drawHorizontalLine ((int) py (db), v.getX(), v.getRight()); }
        const float T = proc.exactValue (svl::FXC_THRESH), Rt = juce::jmax (1.0f, proc.exactValue (svl::FXC_RATIO)), mk = proc.exactValue (svl::FXC_MAKEUP), Wk = 6.0f;
        g.setColour (kKeyTextDim.withAlpha (0.6f)); g.drawVerticalLine ((int) px (T), v.getY(), v.getBottom());
        juce::Path p;
        for (int i = 0; i <= 96; ++i)
        {
            const float x = -48.0f + 48.0f * (float) i / 96.0f;
            float y = x;
            if (2.0f * (x - T) > Wk) y = T + (x - T) / Rt;
            else if (2.0f * (x - T) > -Wk) { const float d = x - T + 0.5f * Wk; y = x + (1.0f / Rt - 1.0f) * d * d / (2.0f * Wk); }
            if (i == 0) p.startNewSubPath (px (x), py (y + mk)); else p.lineTo (px (x), py (y + mk));
        }
        g.setColour (kLed); g.strokePath (p, juce::PathStrokeType (1.6f));
    }
    // COMP: gain reduction, 0 .. 24 dB from the right
    {
        const auto v = grView;
        g.setColour (juce::Colour (0xff0b0f0d)); g.fillRoundedRectangle (v, 3.0f);
        const float gr = juce::jmax (0.0f, shownGr), w = v.getWidth() * gr / 24.0f;
        g.setColour (kLed); g.fillRect (juce::Rectangle<float> (v.getRight() - w, v.getY() + 4.0f, w, v.getHeight() - 8.0f));
        g.setColour (kKeyTextDim);
        drawTracked (g, "GR " + juce::String (gr, 1) + " dB", v.reduced (8.0f, 0.0f), heros (8.6f, true), 0.1f, juce::Justification::centredLeft);
    }
}
} // namespace jfui
