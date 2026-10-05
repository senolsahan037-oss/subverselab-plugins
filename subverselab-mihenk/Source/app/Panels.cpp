#include "Panels.h"
#include "Theme.h"
#include "../dsp/Spectrum.h"
#include "../plugin/PluginProcessor.h"

namespace mihenk::ui
{
//==============================================================================
namespace
{
juce::File& prefsOverride()
{
    static juce::File f;
    return f;
}

juce::var readPrefs()
{
    const auto f = UiPrefs::file();
    return f.existsAsFile() ? juce::JSON::parse (f) : juce::var();
}

juce::String signedDb (double v, int dec = 1) { return (v > 0.0 ? "+" : "") + juce::String (v, dec); }
} // namespace

juce::File UiPrefs::file()
{
    if (prefsOverride() != juce::File()) return prefsOverride();
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("SubverseLab/MIHENK/ui.json");
}

void UiPrefs::setFileForTesting (const juce::File& f) { prefsOverride() = f; }

bool UiPrefs::advancedFace()
{
    const auto v = readPrefs();
    return v.isObject() && v["face"].toString() == "advanced";
}

void UiPrefs::setAdvancedFace (bool advanced)
{
    auto v = readPrefs();
    if (! v.isObject()) v = juce::var (new juce::DynamicObject());
    v.getDynamicObject()->setProperty ("face", advanced ? "advanced" : "simple");
    const auto f = file();
    f.getParentDirectory().createDirectory();
    f.replaceWithText (juce::JSON::toString (v, false));
}

//==============================================================================
void AutoLamp::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    const auto c = b.getCentre();
    if (lit) glow (g, c, 9.0f, Theme::lamp, 0.6f);
    else if (isMouseOver()) glow (g, c, 7.0f, Theme::lamp, 0.25f);
    g.setColour (juce::Colour (0xff0b0a06));
    g.fillEllipse (c.x - 4.5f, c.y - 4.5f, 9.0f, 9.0f);
    g.setGradientFill (juce::ColourGradient (lit ? Theme::lamp.brighter (0.4f) : Theme::lamp.withMultipliedBrightness (0.22f), c.x - 1.5f, c.y - 1.5f,
                                             lit ? Theme::gold : Theme::gold.withMultipliedBrightness (0.15f), c.x + 3.5f, c.y + 3.5f, true));
    g.fillEllipse (c.x - 3.3f, c.y - 3.3f, 6.6f, 6.6f);
    g.setColour (juce::Colours::white.withAlpha (lit ? 0.75f : 0.18f));
    g.fillEllipse (c.x - 1.9f, c.y - 2.3f, 1.8f, 1.4f);
}

//==============================================================================
void LoudnessVfd::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    vfdGlass (g, b);
    auto area = b.reduced (10.0f, 5.0f);
    const auto st = ctx.session.loudness();
    if (! st.valid)
    {
        vfdText (g, ctx.isApp() ? "LOAD A MIX: ITS LOUDNESS AND WHAT" : "LEARN THE SONG: ITS LOUDNESS AND WHAT", area.removeFromTop (area.getHeight() * 0.5f),
                 juce::Justification::centredLeft, 9.0f, Theme::vfd.withAlpha (0.45f));
        vfdText (g, "THE TARGET DOES TO IT SHOW HERE", area, juce::Justification::centredLeft, 9.0f, Theme::vfd.withAlpha (0.45f));
    }
    else
    {
        const bool down = st.changeDb < -0.3 && ctx.session.settings.levelHeld() && ! ctx.session.settings.bypass;
        vfdText (g, st.line1, area.removeFromTop (area.getHeight() * 0.5f), juce::Justification::centredLeft, 10.0f, Theme::vfd);
        vfdText (g, st.line2.isNotEmpty() ? st.line2 : juce::String ("RENDERING ..."), area, juce::Justification::centredLeft, 9.5f,
                 st.severity == 2 ? Theme::coral : (down || st.severity == 1) ? Theme::lamp : ctx.session.settings.levelHeld() ? Theme::vfd.withAlpha (0.85f) : Theme::coral);
    }
    vfdGlassTop (g, b);
}

juce::String LoudnessVfd::getTooltip()
{
    const auto st = ctx.session.loudness();
    return st.valid ? st.sentence : juce::String ("The loudness of the mix, of the master, and what the target does to it.");
}

//==============================================================================
juce::String ReferencePanel::summary (const Session& s)
{
    if (s.reference == nullptr) return {};
    const double a = s.toneDistanceBefore(), n = s.toneDistanceNow();
    return "TONE " + juce::String (a, 1) + " > " + juce::String (n, 1) + " DB RMS, LOUD " + fmt (s.refMeas.integrated) + " / "
           + (s.outMeas.valid ? fmt (s.outMeas.integrated) : juce::String ("--")) + " LUFS";
}

juce::String ReferencePanel::getTooltip()
{
    auto& s = ctx.session;
    if (s.reference == nullptr) return "Load a reference (REF, or drop a master you like on the right half of the panel); MATCH TO REFERENCE then matches its tone and loudness.";
    return "Reference minus mix, level-aligned, 1/6 oct. Dim: before (the mix as it came in); bright: now (the master). "
           "Tone distance = RMS of that difference over 60 Hz - 12 kHz: " + juce::String (s.toneDistanceBefore(), 2) + " dB before, "
           + juce::String (s.toneDistanceNow(), 2) + " dB now. Loudness: reference " + fmt (s.refMeas.integrated) + " LUFS, master "
           + fmt (s.outMeas.integrated) + " LUFS.";
}

void ReferencePanel::paint (juce::Graphics& g)
{
    auto& s = ctx.session;
    const auto b = getLocalBounds().toFloat();
    if (s.reference == nullptr)
    {
        auto area = b.reduced (6.0f, 0.0f);
        g.setColour (Theme::ink.withAlpha (0.55f));
        tracked (g, "NO REFERENCE", Theme::legend (8.0f), area.removeFromTop (area.getHeight() * 0.42f).withTrimmedTop (area.getHeight() * 0.2f),
                 juce::Justification::centredBottom, 0.16f);
        g.setColour (Theme::ink.withAlpha (0.4f));
        const juce::StringArray lines { "PRESS REF, OR DROP", "A MASTER YOU LIKE", "ON THE RIGHT HALF.", "", "MIHENK THEN MATCHES", "ITS TONE AND", "LOUDNESS IN ONE STEP." };
        area.removeFromTop (8.0f);
        for (auto& l : lines) tracked (g, l, Theme::fine (7.5f), area.removeFromTop (11.0f), juce::Justification::centred, 0.1f);
        return;
    }
    auto area = b;
    vfdGlass (g, area);
    auto in = area.reduced (8.0f, 6.0f);
    vfdText (g, s.referenceName.toUpperCase(), in.removeFromTop (11.0f), juce::Justification::centredLeft, 7.5f, Theme::lamp.withAlpha (0.9f));
    vfdText (g, "REF - MIX, 1/6 OCT", in.removeFromTop (10.0f), juce::Justification::centredLeft, 6.8f, Theme::vfd.withAlpha (0.5f));
    auto foot = in.removeFromBottom (32.0f);
    const auto plot = in.reduced (2.0f, 3.0f);
    const auto& grid = spectrumGrid();
    auto xOf = [&] (double f) { return plot.getX() + (float) (std::log2 (f / 20.0) / std::log2 (20480.0 / 20.0)) * plot.getWidth(); };
    g.setColour (Theme::vfd.withAlpha (0.10f));
    for (double f : { 100.0, 1000.0, 10000.0 }) g.drawVerticalLine ((int) xOf (f), plot.getY(), plot.getBottom());
    g.setColour (Theme::vfd.withAlpha (0.3f));
    g.drawHorizontalLine ((int) plot.getCentreY(), plot.getX(), plot.getRight());
    g.setFont (Theme::mono (6.5f));
    g.setColour (Theme::vfd.withAlpha (0.4f));
    g.drawText ("+6", plot.withHeight (8.0f), juce::Justification::topLeft);
    g.drawText ("-6", plot.withTrimmedTop (plot.getHeight() - 8.0f), juce::Justification::bottomLeft);
    for (auto [f, t] : { std::pair<double, const char*> { 100.0, "100" }, { 1000.0, "1K" }, { 10000.0, "10K" } })
        g.drawText (t, juce::Rectangle<float> (xOf (f) - 12.0f, plot.getBottom() - 8.0f, 24.0f, 8.0f), juce::Justification::centred);
    auto curve = [&] (const std::vector<float>& d)
    {
        juce::Path p;
        if (d.size() != grid.size()) return p;
        bool started = false;
        for (size_t i = 0; i < grid.size(); ++i)
        {
            if (grid[i] < 30.0 || grid[i] > 18000.0) continue;
            const float v = juce::jlimit (-6.0f, 6.0f, d[i]);
            const juce::Point<float> pt (xOf (grid[i]), plot.getCentreY() - v / 6.0f * plot.getHeight() * 0.5f);
            if (! started) { p.startNewSubPath (pt); started = true; } else p.lineTo (pt);
        }
        return p;
    };
    if (! s.inMeas.spectrumDb.empty())
        phosphorPath (g, curve (MasterChain::differenceCurve (s.inMeas.spectrumDb, s.refMeas.spectrumDb, nullptr)), Theme::coral.withAlpha (0.7f), 1.0f);
    if (s.outMeas.valid && s.output != nullptr && s.output != s.input)
        phosphorPath (g, curve (MasterChain::differenceCurve (s.outMeas.spectrumDb, s.refMeas.spectrumDb, nullptr)), Theme::phosphor, 1.3f);
    const double a = s.toneDistanceBefore(), n = s.toneDistanceNow();
    auto row = [&] (const juce::String& l, const juce::String& v, juce::Colour c)
    {
        auto r = foot.removeFromTop (10.5f);
        vfdText (g, l, r.removeFromLeft (r.getWidth() * 0.42f), juce::Justification::centredLeft, 6.8f, Theme::vfd.withAlpha (0.55f));
        vfdText (g, v, r, juce::Justification::centredRight, 7.5f, c);
    };
    row ("BEFORE", s.input != nullptr ? juce::String (a, 1) + " DB RMS" : juce::String ("--"), Theme::coral.withAlpha (0.9f));
    row ("NOW", s.outMeas.valid ? juce::String (n, 1) + " DB RMS" : juce::String ("--"), n >= 0.0 && n < 0.6 * a ? Theme::lamp : Theme::vfd);
    row ("LOUD", fmt (s.refMeas.integrated) + " / " + (s.outMeas.valid ? fmt (s.outMeas.integrated) : juce::String ("--")), Theme::vfd);
    vfdGlassTop (g, b);
}

//==============================================================================
juce::StringArray ChainReadout::lines (const Session& s)
{
    juce::StringArray out;
    if (! s.hasAnalysis()) return out;
    const auto& st = s.settings;
    if (st.bypass) { out.add ("BYPASS     THE CHAIN IS OUT: YOU HEAR THE MIX AS IT IS"); return out; }
    out.add ("INPUT      TRIM " + signedDb (st.trimDb) + " DB" + (st.planned ? " PLAN" : "")
             + (st.dcFilter ? ", DC FILTER" : "") + (std::abs (st.balanceDb) > 0.01 ? ", BALANCE " + signedDb (st.balanceDb, 2) : juce::String()));
    const auto& e = st.eq;
    if (e.enabled)
        out.add ("TONE       LOW " + signedDb (e.lowShelfDb) + ", TILT " + signedDb (e.tiltDbPerOct, 2) + "/OCT, AIR " + signedDb (e.airDb)
                 + (e.matchCurveDb.empty() ? juce::String() : ", REF MATCH " + juce::String (juce::roundToInt (e.matchAmount * 100.0)) + " %")
                 + (e.dynLowMid.enabled || e.dynUpperMid.enabled || e.tightener.enabled || e.airDynamic ? ", DYNAMIC EQ" : ""));
    else
        out.add ("TONE       EQ OUT");
    const auto& gl = s.glue;
    if (st.comp.enabled && gl.valid)
        out.add ("GLUE       " + juce::String (gl.loudMeanGrDb, 1) + " DB ON THE LOUD PART"
                 + (gl.threeBand ? juce::String (", 3-BAND") : ", THRESHOLD " + juce::String (gl.thresholdDb, 1) + (st.comp.autoThreshold ? " AUTO" : " DBFS"))
                 + (gl.dense && gl.autoThreshold ? ", DENSE INPUT: KEPT LIGHT" : ""));
    else
        out.add (st.comp.enabled ? "GLUE       --" : "GLUE       OFF");
    const auto& w = st.width;
    out.add ("STEREO     WIDTH " + juce::String (juce::roundToInt (w.width * 100.0)) + " %"
             + (w.monoBelowHz > 0.0 ? ", MONO BELOW " + juce::String (juce::roundToInt (w.monoBelowHz)) + " HZ" : juce::String()));
    out.add ("LIMITER    DRIVE " + signedDb (st.limiter.driveDb, 2) + " DB" + (st.levelHeld() ? " PLAN" : " BY HAND") + ", PEAKS "
             + juce::String (s.limGr.maxDb, 1) + " DB, CEILING " + juce::String (st.limiter.ceilingDbtp, 1) + " DBTP");
    const auto story = s.loudness();
    if (story.valid && story.line2.isNotEmpty()) out.add ("RESULT     " + story.line1 + ": " + story.line2);
    return out;
}

void ChainReadout::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    vfdGlass (g, b);
    auto area = b.reduced (14.0f, 9.0f);
    vfdText (g, "WHAT MIHENK DID, AND WHY (MEASURED FIRST, THEN PLANNED; NOTHING HERE NEEDS SETTING)", area.removeFromTop (13.0f),
             juce::Justification::centredLeft, 8.0f, Theme::vfd.withAlpha (0.5f));
    const auto& s = ctx.session;
    if (! s.hasAnalysis())
    {
        vfdText (g, ctx.isApp() ? "LOAD A MIX (OPEN, OR DROP IT ON THE LEFT HALF)" : "PRESS LEARN, THEN PLAY THE SONG ONCE", area, juce::Justification::centred, 11.0f, Theme::vfd.withAlpha (0.5f));
    }
    else
    {
        // One paragraph in plain words: the measured input, what the plan does in dB, what it refused and why, what you changed over the plan.
        g.setFont (Theme::mono (10.5f));
        g.setColour (s.settings.bypass ? Theme::vfd.withAlpha (0.6f) : Theme::vfd);
        const juce::String text = s.settings.bypass ? juce::String ("BYPASS: you hear the mix as it is. The plan is unchanged and waiting.") : s.planParagraph();
        g.drawFittedText (text, area.toNearestInt(), juce::Justification::topLeft, 12, 1.0f);
    }
    vfdGlassTop (g, b);
}
} // namespace mihenk::ui
