#include "Scopes.h"
#include <complex>

namespace jfui
{
namespace
{
float curveShape (float p, float c)   // the engine's envelope curve (SvlDsp.h)
{
    if (std::abs (c) < 1e-3f) return p;
    const float kk = 6.0f * std::abs (c);
    if (c > 0) return (1.0f - std::exp (-kk * p)) / (1.0f - std::exp (-kk));
    return (std::exp (kk * p) - 1.0f) / (std::exp (kk) - 1.0f);
}

void glass (juce::Graphics& g, juce::Rectangle<float> r)
{
    g.setColour (juce::Colours::black.withAlpha (0.35f)); g.fillRoundedRectangle (r.translated (0.0f, 1.0f).expanded (1.0f), 4.0f);
    juce::ColourGradient gl (juce::Colour (0xff0A1F12), r.getX(), r.getY(), juce::Colour (0xff020804), r.getX(), r.getBottom(), false);   // dark green-tinted glass
    g.setGradientFill (gl); g.fillRoundedRectangle (r, 3.5f);
    g.setColour (kLed.withAlpha (0.07f));
    for (int i = 1; i < 8; ++i) g.drawVerticalLine (juce::roundToInt (r.getX() + r.getWidth() * (float) i / 8.0f), r.getY() + 2.0f, r.getBottom() - 2.0f);
    g.drawHorizontalLine (juce::roundToInt (r.getCentreY()), r.getX() + 2.0f, r.getRight() - 2.0f);
    // depth (owner 17 Oct: "ekran derinliği yok"): the glass sits in a well — inner shadow from the top and the sides,
    // a lit lip along the bottom edge, a faint reflection over the upper half
    {
        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (r.toNearestInt());
        juce::ColourGradient top (juce::Colours::black.withAlpha (0.75f), 0.0f, r.getY(), juce::Colours::transparentBlack, 0.0f, r.getY() + 9.0f, false);
        g.setGradientFill (top); g.fillRect (r.withHeight (9.0f));
        juce::ColourGradient left (juce::Colours::black.withAlpha (0.5f), r.getX(), 0.0f, juce::Colours::transparentBlack, r.getX() + 6.0f, 0.0f, false);
        g.setGradientFill (left); g.fillRect (r.withWidth (6.0f));
        juce::ColourGradient right (juce::Colours::transparentBlack, r.getRight() - 6.0f, 0.0f, juce::Colours::black.withAlpha (0.5f), r.getRight(), 0.0f, false);
        g.setGradientFill (right); g.fillRect (r.withLeft (r.getRight() - 6.0f));
        juce::ColourGradient sheen (juce::Colours::white.withAlpha (0.07f), r.getX(), r.getY(), juce::Colours::transparentWhite, r.getX() + r.getWidth() * 0.35f, r.getBottom(), false);
        g.setGradientFill (sheen); g.fillRect (r.withHeight (r.getHeight() * 0.5f));
    }
    g.setColour (juce::Colours::black.withAlpha (0.85f)); g.drawRoundedRectangle (r.expanded (0.5f), 3.5f, 1.4f);
    g.setColour (juce::Colours::white.withAlpha (0.16f)); g.drawHorizontalLine (juce::roundToInt (r.getBottom() + 1.5f), r.getX() + 3.0f, r.getRight() - 3.0f);
}

void trace (juce::Graphics& g, const juce::Path& p, juce::Colour)
{
    const juce::Colour c = kLed;   // v3: every screen draws in the panel's green
    g.setColour (c.withAlpha (0.10f)); g.strokePath (p, juce::PathStrokeType (7.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));   // glow
    g.setColour (c.withAlpha (0.25f)); g.strokePath (p, juce::PathStrokeType (3.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (c); g.strokePath (p, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}
} // namespace

float Scope::val (const juce::String& id) const
{
    if (auto* v = proc.apvts.getRawParameterValue (id)) return v->load();
    return 0.0f;
}

void Scope::tick()
{
    juce::String s;
    for (const auto& id : watched()) s << val (id) << ";";
    lv = proc.live();
    const bool was = liveNow;
    liveNow = lv.voices > 0 && lv.level > 1.0e-4f;
    if (liveNow) anim += 1.0f / 30.0f;
    if (s != last || liveNow || was) { last = s; repaint(); }
}

void Scope::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced (1.0f);
    glass (g, r);
    juce::Graphics::ScopedSaveState ss (g);
    g.reduceClipRegion (r.reduced (1.5f).toNearestInt());
    drawTrace (g, r.reduced (6.0f, 4.0f));
}

//==============================================================================
float OscScope::shape (int type, float t, float pw)
{
    switch ((svl::OscType) type)
    {
        case svl::OscType::saw: return 2.0f * t - 1.0f;
        case svl::OscType::square: return t < pw ? 1.0f : -1.0f;
        case svl::OscType::triangle: return 1.0f - 4.0f * std::abs (t - 0.5f);
        case svl::OscType::sine: return std::sin (juce::MathConstants<float>::twoPi * t);
        default: return 0.0f;
    }
}

juce::StringArray OscScope::watched() const
{
    const juce::String p = "osc" + juce::String (o + 1) + ".";
    juce::StringArray w { p + "type", p + "level", p + "pw", p + "unison", p + "unison_detune", p + "decay", p + "string_decay", p + "string_tone",
                          "fm.amount", "fm.target", "osc2.type", "osc2.octave", "osc2.semi", p + "octave", p + "semi" };
    return w;
}

void OscScope::drawTrace (juce::Graphics& g, juce::Rectangle<float> a)
{
    const juce::String p = "osc" + juce::String (o + 1) + ".";
    const int type = juce::roundToInt (val (p + "type"));
    const float level = juce::jlimit (0.0f, 1.0f, val (p + "level")), pw = val (p + "pw");
    float amp = a.getHeight() * 0.46f * (0.25f + 0.75f * level);
    const float cy = a.getCentreY();
    const int N = 220;
    float cycles = 2.0f, phase0 = 0.0f;
    if (liveNow)
    {   // the wave runs while it sounds: more cycles for higher notes, scrolling, its height following the voice's level
        const float semis = lv.pitch + 12.0f * val (p + "octave") + val (p + "semi") - 60.0f;
        cycles = juce::jlimit (1.0f, 8.0f, 2.0f * std::exp2 (semis / 24.0f));
        phase0 = anim * (0.8f + 0.25f * cycles);
        amp *= 0.3f + 0.7f * juce::jlimit (0.0f, 1.0f, lv.level);
    }
    auto xAt = [&] (int i) { return a.getX() + a.getWidth() * (float) i / (float) N; };

    if (type == (int) svl::OscType::off)
    {
        g.setColour (Pal::dim.withAlpha (0.6f));
        g.setFont (heros (8.6f, true));
        g.drawText ("OFF", a, juce::Justification::centred, false);
        return;
    }
    if (type == (int) svl::OscType::noiseWhite || type == (int) svl::OscType::noisePink)
    {
        juce::Random rnd (o * 31 + 5 + (liveNow ? juce::roundToInt (anim * 30.0f) : 0));
        juce::Path path; float y = 0;
        for (int i = 0; i <= N; ++i)
        {
            const float w = rnd.nextFloat() * 2.0f - 1.0f;
            y = type == (int) svl::OscType::noisePink ? 0.82f * y + 0.45f * w : w;
            const auto pt = juce::Point<float> (xAt (i), cy - y * amp * 0.85f);
            i == 0 ? path.startNewSubPath (pt) : path.lineTo (pt);
        }
        trace (g, path, Pal::cream);
        return;
    }
    if (type == (int) svl::OscType::string)
    {   // a plucked string: a bright burst that rings down over the window
        const float ring = juce::jlimit (0.05f, 20.0f, val (p + "string_decay")), tone = val (p + "string_tone");
        juce::Path path;
        for (int i = 0; i <= N; ++i)
        {
            const float t = (float) i / (float) N + (liveNow ? std::fmod (anim * 0.6f, 1.0f) : 0.0f);
            const float env = std::exp (-std::fmod (t, 1.0f) * 3.0f / juce::jmax (0.3f, ring));
            const float y = std::sin (juce::MathConstants<float>::twoPi * 6.0f * t) + tone * 0.45f * std::exp (-t * 8.0f) * std::sin (juce::MathConstants<float>::twoPi * 18.0f * t);
            const auto pt = juce::Point<float> (xAt (i), cy - y * env * amp * 0.8f);
            i == 0 ? path.startNewSubPath (pt) : path.lineTo (pt);
        }
        trace (g, path, Pal::cream);
        return;
    }
    // unison copies, faint behind the main trace
    const int uni = juce::jlimit (1, 8, juce::roundToInt (val (p + "unison")));
    const float det = val (p + "unison_detune");
    if (uni > 1)
        for (int u = 0; u < uni; ++u)
        {
            const float x = (float) u / (float) (uni - 1) * 2.0f - 1.0f;
            const float stretch = 1.0f + x * det / 1200.0f * 6.0f;   // exaggerated so the spread is visible
            juce::Path path;
            for (int i = 0; i <= N; ++i)
            {
                float ph = cycles * stretch * (float) i / (float) N + 0.13f * (float) u;
                ph -= std::floor (ph);
                const auto pt = juce::Point<float> (xAt (i), cy - shape (type, ph, pw) * amp);
                i == 0 ? path.startNewSubPath (pt) : path.lineTo (pt);
            }
            g.setColour (kLed.withAlpha (0.22f));
            g.strokePath (path, juce::PathStrokeType (1.0f));
        }
    // main trace, with FM when osc 2 modulates this one
    const float fm = val ("fm.amount");
    const int tgt = juce::roundToInt (val ("fm.target"));
    const bool fmHere = o != 1 && fm > 0.0f && juce::roundToInt (val ("osc2.type")) != (int) svl::OscType::off
                        && (tgt == 2 || (tgt == 0 && o == 0) || (tgt == 1 && o == 2));
    const float ratio = std::exp2 ((12.0f * (val ("osc2.octave") - val (p + "octave")) + val ("osc2.semi") - val (p + "semi")) / 12.0f);
    const int type2 = juce::roundToInt (val ("osc2.type"));
    juce::Path path;
    for (int i = 0; i <= N; ++i)
    {
        float ph = cycles * (float) i / (float) N + phase0;
        if (fmHere)
        {
            float ph2 = ph * ratio; ph2 -= std::floor (ph2);
            ph += fm / juce::MathConstants<float>::twoPi * shape (type2, ph2, val ("osc2.pw"));
        }
        ph -= std::floor (ph);
        const auto pt = juce::Point<float> (xAt (i), cy - shape (type, ph, pw) * amp);
        i == 0 ? path.startNewSubPath (pt) : path.lineTo (pt);
    }
    trace (g, path, fmHere ? Pal::red.brighter (0.4f) : Pal::cream);
}

//==============================================================================
juce::StringArray EnvScope::watched() const
{
    return { pre + "attack", pre + "decay", pre + "sustain", pre + "release", pre + "attack_curve", pre + "decay_curve", pre + "release_curve" };
}

void EnvScope::drawTrace (juce::Graphics& g, juce::Rectangle<float> a)
{
    const float A = val (pre + "attack"), D = val (pre + "decay"), S = juce::jlimit (0.0f, 1.0f, val (pre + "sustain")), Rl = val (pre + "release");
    const float ca = val (pre + "attack_curve"), cd = val (pre + "decay_curve"), cr = val (pre + "release_curve");
    // widths on a soft log scale, so a 3 ms attack and a 20 s release both read
    auto w = [] (float t) { return std::log1p (t * 12.0f) + 0.08f; };
    const float hold = 0.9f, total = w (A) + w (D) + hold + w (Rl);
    const float px = a.getWidth() / total;
    const float x0 = a.getX(), x1 = x0 + w (A) * px, x2 = x1 + w (D) * px, x3 = x2 + hold * px, x4 = x3 + w (Rl) * px;
    auto Y = [&] (float lv) { return a.getBottom() - lv * a.getHeight(); };
    juce::Path p;
    p.startNewSubPath (x0, Y (0));
    const int n = 40;
    for (int i = 1; i <= n; ++i) { const float t = (float) i / (float) n; p.lineTo (x0 + (x1 - x0) * t, Y (curveShape (t, ca))); }
    for (int i = 1; i <= n; ++i) { const float t = (float) i / (float) n; p.lineTo (x1 + (x2 - x1) * t, Y (1.0f + (S - 1.0f) * curveShape (t, cd))); }
    p.lineTo (x3, Y (S));
    for (int i = 1; i <= n; ++i) { const float t = (float) i / (float) n; p.lineTo (x3 + (x4 - x3) * t, Y (S * (1.0f - curveShape (t, cr)))); }
    juce::Path fill (p);
    fill.lineTo (x4, Y (0)); fill.closeSubPath();
    g.setColour (kLed.withAlpha (0.12f)); g.fillPath (fill);
    trace (g, p, Pal::gold);
    g.setColour (Pal::dim.withAlpha (0.45f));
    for (float x : { x1, x2, x3 }) g.drawVerticalLine (juce::roundToInt (x), a.getY(), a.getBottom());
    if (liveNow)
    {   // where the loudest sounding voice is on its envelope (the amp envelope's stage and level)
        const float L = juce::jlimit (0.0f, 1.0f, lv.level);
        float x = x0;
        switch ((int) lv.ampStage)
        {
            case 1: x = x0 + (x1 - x0) * L; break;
            case 2: x = x1 + (x2 - x1) * juce::jlimit (0.0f, 1.0f, (1.0f - L) / juce::jmax (1.0e-3f, 1.0f - S)); break;
            case 3: x = x2 + (x3 - x2) * (0.5f + 0.45f * std::sin (anim * 2.0f)); break;
            case 4: x = x3 + (x4 - x3) * juce::jlimit (0.0f, 1.0f, 1.0f - L / juce::jmax (1.0e-3f, S)); break;
            default: break;
        }
        const float y = a.getBottom() - (pre == "amp_env." ? L : juce::jlimit (0.0f, 1.0f, L)) * a.getHeight();
        g.setColour (kLed.withAlpha (0.25f)); g.fillEllipse (juce::Rectangle<float> (12.0f, 12.0f).withCentre ({ x, y }));
        g.setColour (kLed); g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre ({ x, y }));
        g.drawVerticalLine (juce::roundToInt (x), a.getY(), a.getBottom());
    }
}

//==============================================================================
juce::StringArray FilterScope::watched() const
{
    return { "filter.mode", "filter.cutoff", "filter.reso", "filter.drive" };
}

void FilterScope::drawTrace (juce::Graphics& g, juce::Rectangle<float> a)
{
    const int mode = juce::roundToInt (val ("filter.mode"));
    const float fc = val ("filter.cutoff"), reso = val ("filter.reso");
    const double k = 2.0 - 1.98 * juce::jlimit (0.0f, 1.0f, reso);   // the engine's damping
    const float lo = std::log2 (20.0f), hi = std::log2 (20000.0f), dbTop = 18.0f, dbBot = -42.0f;
    juce::Path p;
    const int N = 200;
    for (int i = 0; i <= N; ++i)
    {
        const float f = std::exp2 (lo + (hi - lo) * (float) i / (float) N);
        const std::complex<double> s (0.0, (double) f / (double) juce::jmax (10.0f, fc));
        const auto den = s * s + k * s + 1.0;
        std::complex<double> num;
        switch (mode) { case 0: num = 1.0; break; case 1: num = s; break; case 2: num = s * s; break; default: num = s * s + 1.0; break; }
        const float db = (float) (20.0 * std::log10 (std::abs (num / den) + 1e-9));
        const float y = juce::jmap (juce::jlimit (dbBot, dbTop, db), dbBot, dbTop, a.getBottom(), a.getY());
        const float x = a.getX() + a.getWidth() * (float) i / (float) N;
        i == 0 ? p.startNewSubPath (x, y) : p.lineTo (x, y);
    }
    g.setColour (Pal::dim.withAlpha (0.4f));
    g.drawHorizontalLine (juce::roundToInt (juce::jmap (0.0f, dbBot, dbTop, a.getBottom(), a.getY())), a.getX(), a.getRight());
    juce::Path fill (p); fill.lineTo (a.getRight(), a.getBottom()); fill.lineTo (a.getX(), a.getBottom()); fill.closeSubPath();
    g.setColour (kLed.withAlpha (0.12f)); g.fillPath (fill);
    trace (g, p, Pal::red.brighter (0.45f));
}

//==============================================================================
juce::StringArray PitchScope::watched() const
{
    return { "pitch_env.amount", "pitch_env.decay", "click.level", "click.decay" };
}

void PitchScope::drawTrace (juce::Graphics& g, juce::Rectangle<float> a)
{
    const float amt = val ("pitch_env.amount"), dec = juce::jmax (0.001f, val ("pitch_env.decay"));
    const float click = val ("click.level"), cdec = val ("click.decay");
    const float window = juce::jlimit (0.05f, 2.0f, dec * 4.0f + cdec * 4.0f);
    juce::Path p;
    const int N = 160;
    for (int i = 0; i <= N; ++i)
    {
        const float t = window * (float) i / (float) N;
        const float v = amt / 48.0f * std::pow (10.0f, -t / dec);
        const float x = a.getX() + a.getWidth() * (float) i / (float) N, y = a.getCentreY() - v * a.getHeight() * 0.48f;
        i == 0 ? p.startNewSubPath (x, y) : p.lineTo (x, y);
    }
    if (click > 0.0f)
    {   // the click burst as a spike at the start
        const float cw = juce::jmax (2.0f, a.getWidth() * cdec / window);
        g.setColour (Pal::cream.withAlpha (0.25f + 0.5f * click));
        g.fillRect (juce::Rectangle<float> (a.getX(), a.getY() + a.getHeight() * (0.5f - 0.45f * click), cw, a.getHeight() * 0.9f * click));
    }
    trace (g, p, Pal::gold);
}

//==============================================================================
juce::StringArray LfoScope::watched() const
{
    const juce::String p = "lfo" + juce::String (l + 1) + ".";
    return { p + "shape", p + "fade", p + "rate", p + "sync" };
}

void LfoScope::drawTrace (juce::Graphics& g, juce::Rectangle<float> a)
{
    const juce::String pre = "lfo" + juce::String (l + 1) + ".";
    const int shp = juce::roundToInt (val (pre + "shape"));
    const float fade = val (pre + "fade");
    juce::Path p;
    juce::Random rnd (l * 7 + 3);
    float hold = rnd.nextFloat() * 2.0f - 1.0f;
    const int N = 200; const float cycles = 3.0f;
    float lastPh = 0.0f;
    for (int i = 0; i <= N; ++i)
    {
        const float t = (float) i / (float) N;
        float ph = cycles * t; ph -= std::floor (ph);
        if (ph < lastPh) hold = rnd.nextFloat() * 2.0f - 1.0f;
        lastPh = ph;
        float y;
        switch (shp)
        {
            case 0: y = std::sin (juce::MathConstants<float>::twoPi * ph); break;
            case 1: y = 1.0f - 4.0f * std::abs (ph - 0.5f); break;
            case 2: y = 2.0f * ph - 1.0f; break;
            case 3: y = ph < 0.5f ? 1.0f : -1.0f; break;
            default: y = hold; break;
        }
        if (fade > 0.0f) y *= juce::jmin (1.0f, t * 3.0f / juce::jmin (3.0f, fade + 0.2f));
        const float x = a.getX() + a.getWidth() * t, yy = a.getCentreY() - y * a.getHeight() * 0.45f;
        i == 0 ? p.startNewSubPath (x, yy) : p.lineTo (x, yy);
    }
    trace (g, p, Pal::red.brighter (0.4f));
}
} // namespace jfui
