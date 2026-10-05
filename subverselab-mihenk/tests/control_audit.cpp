/*  MIHENK control audit (2026-10-04): "every control on the panel must be checked; a control that does nothing must not exist."

        control_audit            all rows (exit code = number of FAIL rows)

    For every knob and every switch the panel exposes (SIMPLE and ADVANCED face, WIDE and 3-BAND), a fresh Rig (one real
    MihenkProcessor + one real MainComponent, standalone mode, no audio device) loads a music-like stereo signal, sets the context the
    control needs (AIR above 0 dB for AIR FREQ, the compressor in and a manual threshold for RATIO / ATTACK, ...), turns the control
    to two values THROUGH THE PANEL (so the knob -> Session -> plan -> overrides -> render path is what is measured, not the engine
    alone) and compares the two rendered masters:

        delta = 20 log10 ( rms (B - A) / rms (A) )        both channels, whole file; 0 dB = as large as the signal itself

    PASS: delta > -60 dB. Also checked per knob: the value sticks (after the render the knob shows what was set), the knob is not
    dimmed in the context where it works, and a knob that needs a context is dimmed (or visibly a no-op) in the context where it does
    nothing (an "OFF-CTX" row: a silent no-op with no dimming is a FAIL). Controls that are not audio (views, A/B, export word length,
    WARN AT) get their own check. Writes build-v3/control_audit_out/control_audit.tsv.
*/
#include "audit_common.h"
#include "../Source/plugin/Params.h"
#include "../Source/engine/Plan.h"
#include <cstdlib>

using namespace audit;

namespace
{
constexpr double fs = 48000.0;
constexpr double kPassDb = -60.0;      // anything below this is "the output did not change"

/** A kick / bass / low stereo / snare / hats / sibilance / vocal-ish / mud / pad mix with DC offset: every band has something to move. */
Buffer musicLike (double secs, bool bright, uint32_t seed, bool flat = false)
{
    const int n = (int) (secs * fs);
    Buffer b (2, n);
    juce::Random rng ((juce::int64) seed);
    const double twoPi = 6.283185307179586;
    double padLp[2] {}, hatHp[2] {}, hatLast[2] {}, sibLp[2] {}, sibLast[2] {}, snLp[2] {}, snLast[2] {};
    for (int i = 0; i < n; ++i)
    {
        const double t = i / fs, tb = std::fmod (t, 0.5), sec = flat ? 0.6 : (std::fmod (t, 8.0) >= 4.0 ? 1.0 : 0.4);
        const double kick = (flat && std::fmod (t, 2.0) < 0.5 ? 2.6 : 1.0) * std::exp (-tb * 14.0) * std::sin (twoPi * (48.0 * tb + 2.5 * (1.0 - std::exp (-tb * 28.0))));
        const double note = std::fmod (t, 2.0) < 1.0 ? 55.0 : 73.4;
        const double bass = 0.18 * std::sin (twoPi * note * t) + 0.06 * std::sin (twoPi * 2.0 * note * t);
        const double lowL = 0.10 * std::sin (twoPi * 90.0 * t), lowR = -0.06 * std::sin (twoPi * 90.0 * t + 0.4);       // wide, partly out of phase lows
        const double f0 = 220.0 * (1.0 + 0.01 * std::sin (twoPi * 5.0 * t));
        double vox = 0.0;
        for (int h = 1; h <= 16; ++h) vox += std::sin (twoPi * f0 * h * t) * std::exp (-std::pow ((f0 * h - 2800.0) / 1500.0, 2.0)) / h * 2.0;
        vox *= 0.10 * (std::fmod (t, 4.0) < 3.0 ? 1.0 : 0.2);
        const double mud = 0.05 * std::sin (twoPi * 300.0 * t) + 0.03 * std::sin (twoPi * 420.0 * t);
        const bool snareBeat = std::fmod (t, 1.0) >= 0.5;
        const double snEnv = snareBeat ? std::exp (-(std::fmod (t, 1.0) - 0.5) * 22.0) : 0.0;
        const double hatEnv = std::exp (-std::fmod (t, 0.25) * 60.0);
        const bool sibOn = std::fmod (t, 2.0) > 1.2 && std::fmod (t, 2.0) < 1.5;
        for (int c = 0; c < 2; ++c)
        {
            const double w = rng.nextDouble() * 2.0 - 1.0;
            padLp[c] += 0.05 * (w - padLp[c]);                                   // dark, wide pad
            const double pad = (bright ? 0.5 : 1.0) * padLp[c] * 0.9 + (bright ? 0.5 : 0.0) * (w - padLp[c]) * 0.15;
            const double wh = rng.nextDouble() * 2.0 - 1.0;
            hatHp[c] = wh - hatLast[c]; hatLast[c] = wh;                         // first difference: bright noise
            const double sb = rng.nextDouble() * 2.0 - 1.0;
            sibLp[c] += 0.45 * (sb - sibLp[c]);                                  // 5-9 kHz-ish hiss
            const double sib = sibLp[c] - sibLast[c]; sibLast[c] = sibLp[c];
            const double sn = rng.nextDouble() * 2.0 - 1.0;
            snLp[c] += 0.3 * (sn - snLp[c]); const double snr = snLp[c] - snLast[c] * 0.5; snLast[c] = snLp[c];
            const double hat = hatHp[c] * hatEnv * (bright ? 0.22 : 0.05);
            const double s = 0.55 * kick + bass + (c == 0 ? lowL : lowR) + vox + mud + 0.25 * pad + hat + snr * snEnv * 0.25 + (sibOn ? sib * 0.5 : 0.0);
            b.setSample (c, i, (float) (sec * s + 0.01));                         // + 0.01: a DC offset for the DC filter
        }
    }
    const float pk = b.getMagnitude (0, n);
    b.applyGain ((float) (std::pow (10.0, -3.0 / 20.0) / pk));
    return b;
}

double rmsOf (const Buffer& b)
{
    double s = 0.0;
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = 0; i < b.getNumSamples(); ++i) s += (double) b.getSample (c, i) * b.getSample (c, i);
    return std::sqrt (s / (double) (b.getNumChannels() * b.getNumSamples()));
}
/** 20 log10 (rms (B - A) / rms (A)); +99 when the files differ in length (a latency change), -240 when identical. */
double deltaDb (const Buffer& a, const Buffer& b)
{
    if (a.getNumSamples() != b.getNumSamples() || a.getNumChannels() != b.getNumChannels()) return 99.0;
    Buffer d (a.getNumChannels(), a.getNumSamples());
    for (int c = 0; c < d.getNumChannels(); ++c)
        for (int i = 0; i < d.getNumSamples(); ++i) d.setSample (c, i, b.getSample (c, i) - a.getSample (c, i));
    return toDb (rmsOf (d) / juce::jmax (1.0e-12, rmsOf (a)));
}
Buffer copyOf (const Buffer& b)
{
    Buffer c (b.getNumChannels(), b.getNumSamples());
    for (int ch = 0; ch < c.getNumChannels(); ++ch) c.copyFrom (ch, 0, b, ch, 0, b.getNumSamples());
    return c;
}

struct Row
{
    juce::String control, face, context;
    double delta = -240.0;
    bool pass = false;
    juce::String verdict, note;
};
std::vector<Row> rows;
int fails = 0;

void addRow (Row r)
{
    if (! r.pass) ++fails;
    std::printf ("%-5s | %-22s | %-6s | %-52s | %8.1f | %s\n", r.pass ? "PASS" : "FAIL", r.control.toRawUTF8(), r.face.toRawUTF8(), r.context.toRawUTF8(), r.delta, r.note.toRawUTF8());
    std::fflush (stdout);
    rows.push_back (std::move (r));
}

using Fn = std::function<void (Rig&)>;
juce::Array<Buffer>* dummy = nullptr;

struct Fixture
{
    Buffer signal = musicLike (10.0, false, 7);
    Buffer reference = musicLike (10.0, true, 11);
};

std::unique_ptr<Rig> makeRig (const Fixture& fx, bool adv, bool withRef = false)
{
    auto r = std::make_unique<Rig> (adv, "control_audit");
    r->load (fx.signal, fs, "music.wav");
    if (withRef)
    {
        r->session().setReferenceForTesting (copyOf (fx.reference), fs, "ref.wav");
        r->sync();
        if (auto* b = findButton (*r->panel, "MATCH TO\nREFERENCE")) fire (b); else r->session().matchToReference();
        r->render();
    }
    return r;
}
Buffer outputOf (Rig& r)
{
    r.render();
    r.panel->tickForTesting (40);
    return copyOf (*r.session().output);
}

struct Ctl
{
    juce::String name, face, ctxText;
    Fn ctx, setA, setB;
    juce::String knob;            // a knob: checked for "sticks", dimming
    double bValue = 0.0;          // what the knob was turned to in B (display units)
    double stickTol = 0.06;
    bool needsCtx = false;        // an off-context row is run too
    bool wide = true;
    bool advanced = true;
    bool withRef = false;
};

void runCtl (const Fixture& fx, const Ctl& c)
{
    const juce::String ctxText = c.ctxText.isEmpty() ? "none" : c.ctxText;
    auto rig = makeRig (fx, c.advanced, c.withRef);
    auto& r = *rig;
    if (c.ctx) { c.ctx (r); r.render(); r.panel->tickForTesting (40); }      // the panel resyncs after a change (its change message); here by hand
    c.setA (r);
    const auto A = outputOf (r);
    c.setB (r);
    const auto B = outputOf (r);
    Row row { c.name, c.advanced ? "ADV" : "SIMPLE", ctxText, deltaDb (A, B), false, {}, {} };
    juce::StringArray notes;
    bool ok = row.delta > kPassDb;
    if (c.knob.isNotEmpty())
    {
        if (! r.panel->knobEnabledForTesting (c.knob)) { ok = false; notes.add ("knob disabled"); }
        if (r.panel->knobDimmedForTesting (c.knob)) { ok = false; notes.add ("DIMMED although its context is on"); }
        const double shown = r.panel->knobValueForTesting (c.knob);
        if (std::abs (shown - c.bValue) > c.stickTol) { ok = false; notes.add ("value did not stick: set " + f2 (c.bValue, 3) + ", shows " + f2 (shown, 3)); }
    }
    if (! (row.delta > kPassDb)) notes.add ("OUTPUT UNCHANGED");
    row.pass = ok;
    if (! ok)
    {
        auto& ss = r.session();
        notes.add ("[eq " + juce::String ((int) ss.settings.eq.enabled) + " comp " + juce::String ((int) ss.settings.comp.enabled) + " width " + juce::String ((int) ss.settings.width.enabled)
                   + " drive " + f2 (ss.settings.limiter.driveDb, 2) + " trim " + f2 (ss.settings.trimDb, 2) + " overrides " + juce::String (ss.overrideCount()) + " planned " + juce::String ((int) ss.settings.planned) + " refused: " + ss.plan.refused.joinIntoString ("|") + " needed " + f2 (ss.plan.totalGainDb, 2) + " ovDeltaTrim " + f2 (ss.overrideDelta ("trim"), 2) + "]");
    }
    row.note = notes.isEmpty() ? juce::String ("ok") : notes.joinIntoString ("; ");
    addRow (row);

    if (c.needsCtx)
    {
        // The same turn without the context: either the output changes anyway, or the knob says it is out of play (dimmed).
        auto rig2 = makeRig (fx, c.advanced, c.withRef);
        auto& r2 = *rig2;
        c.setA (r2);
        const auto A2 = outputOf (r2);
        c.setB (r2);
        const auto B2 = outputOf (r2);
        Row off { c.name + "  [OFF-CTX]", c.advanced ? "ADV" : "SIMPLE", "context off", deltaDb (A2, B2), false, {}, {} };
        const bool dim = c.knob.isNotEmpty() && r2.panel->knobDimmedForTesting (c.knob);
        const bool hidden = c.knob.isNotEmpty() && ! r2.panel->knobVisibleForTesting (c.knob);
        const bool same = ! (off.delta > kPassDb);
        off.pass = ! same || dim || hidden || c.knob.isEmpty();
        off.note = same ? (hidden ? "no-op here, the knob is not shown outside its context (honest)" : dim ? "no-op here, knob is dimmed (honest)" : (c.knob.isEmpty() ? "no-op without context (switch)" : "SILENT NO-OP: nothing changes and the knob is not dimmed")) : "works without the context too";
        addRow (off);
    }
}

// ---- builders --------------------------------------------------------------------------------------------------------
Fn knobTo (const juce::String& label, double v) { return [label, v] (Rig& r) { r.panel->turnKnobForTesting (label, v); }; }
Fn swTo (const juce::String& name, bool on) { return [name, on] (Rig& r) { r.panel->clickSwitchForTesting (name, on); }; }
Fn seq (std::initializer_list<Fn> fns) { std::vector<Fn> v (fns); return [v] (Rig& r) { for (auto& f : v) f (r); }; }
Fn selectBand (int b) { return [b] (Rig& r) { r.panel->selectBand (b); }; }
Fn clickText (const juce::String& t) { return [t] (Rig& r) { if (auto* b = findButton (*r.panel, t)) fire (b); }; }

Ctl knobCtl (const juce::String& label, double a, double b, Fn ctx = {}, const juce::String& ctxText = {}, bool needsCtx = false, bool adv = true, const juce::String& nameSuffix = {})
{
    Ctl c;
    c.name = label + nameSuffix; c.face = adv ? "ADV" : "SIMPLE"; c.ctxText = ctxText; c.ctx = std::move (ctx);
    c.setA = knobTo (label, a); c.setB = knobTo (label, b);
    c.knob = label; c.bValue = b; c.needsCtx = needsCtx; c.advanced = adv;
    return c;
}
Ctl swCtl (const juce::String& name, Fn ctx = {}, const juce::String& ctxText = {}, bool needsCtx = false)
{
    Ctl c;
    c.name = name + " (switch)"; c.ctxText = ctxText; c.ctx = std::move (ctx);
    c.setA = swTo (name, false); c.setB = swTo (name, true); c.needsCtx = needsCtx;
    return c;
}

// ---- non-audio rows ----------------------------------------------------------------------------------------------------
void check (const juce::String& control, const juce::String& face, const juce::String& context, bool ok, const juce::String& note, double delta = -240.0)
{
    Row r { control, face, context, delta, ok, {}, note };
    addRow (r);
}
void na (const juce::String& control, const juce::String& face, const juce::String& reason)
{
    Row r { control, face, "display / monitor only", -240.0, true, {}, "N/A: " + reason };
    addRow (r);
}
} // namespace

int main()
{
    if (std::getenv ("AUDIT_OUT") == nullptr)
        setenv ("AUDIT_OUT", (juce::File (__FILE__).getParentDirectory().getParentDirectory().getChildFile ("build-v3/control_audit_out").getFullPathName()).toRawUTF8(), 1);
    juce::ScopedJuceInitialiser_GUI init;
    Fixture fx;
    std::printf ("MIHENK control audit: signal %.0f s stereo %.0f Hz, delta = 20 log10 (rms (B-A) / rms (A)); PASS above %.0f dB\n\n", 10.0, fs, kPassDb);
    std::printf ("%-5s | %-22s | %-6s | %-52s | %8s | %s\n", "", "control", "face", "context", "delta dB", "note");

    const Fn compOnManual = seq ({ swTo ("COMP IN", true), knobTo ("THRESHOLD", -30.0) });
    const Fn compHard = seq ({ swTo ("COMP IN", true), knobTo ("THRESHOLD", -30.0), knobTo ("RATIO", 4.0) });
    const juce::String compText = "COMP IN, manual THRESHOLD -30 dBFS", compHardText = "COMP IN, THRESHOLD -30, RATIO 4";
    std::vector<Ctl> all;

    // ---- INPUT ----------------------------------------------------------------------------------------------------------
    all.push_back (knobCtl ("TRIM", -6.0, 2.0));   // the plan caps a hand-made level at the gain the target needs (+3.9 dB here)
    all.push_back (knobCtl ("BALANCE", -3.0, 3.0));
    all.push_back (swCtl ("DC FILTER"));

    // ---- EQUALIZER ------------------------------------------------------------------------------------------------------
    all.push_back (knobCtl ("LOW CUT", 25.0, 150.0, swTo ("LOW CUT", true), "LOW CUT switch on", true));
    all.push_back (swCtl ("LOW CUT", knobTo ("LOW CUT", 150.0), "LOW CUT knob 150 Hz"));
    all.push_back (swCtl ("24 DB", seq ({ swTo ("LOW CUT", true), knobTo ("LOW CUT", 120.0) }), "LOW CUT on at 120 Hz"));
    all.push_back (knobCtl ("LOW SHELF", -3.0, 4.0));
    all.push_back (knobCtl ("SHELF FREQ", 50.0, 250.0, knobTo ("LOW SHELF", 4.0), "LOW SHELF +4 dB", true));
    all.push_back (knobCtl ("BELL 1", -4.0, 4.0));
    all.push_back (knobCtl ("BELL 1 FREQ", 150.0, 2000.0, knobTo ("BELL 1", 4.0), "BELL 1 +4 dB", true));
    all.push_back (knobCtl ("BELL 1 Q", 0.4, 5.0, seq ({ knobTo ("BELL 1", 4.0), knobTo ("BELL 1 FREQ", 1000.0) }), "BELL 1 +4 dB at 1 kHz", true));
    all.push_back (knobCtl ("AIR", 0.0, 4.0));
    {
        auto c = knobCtl ("AIR FREQ", 0.5, 3.5, knobTo ("AIR", 4.0), "AIR +4 dB", true);       // slider units: zone index + 0.5 (10 kHz .. 40 kHz)
        c.name = "AIR FREQ 10k vs 40k";
        all.push_back (c);
        auto d = knobCtl ("AIR FREQ", 0.5, 1.5, knobTo ("AIR", 4.0), "AIR +4 dB", false); d.name = "AIR FREQ 10k vs 15k"; all.push_back (d);
        auto e = knobCtl ("AIR FREQ", 1.5, 2.5, knobTo ("AIR", 4.0), "AIR +4 dB", false); e.name = "AIR FREQ 15k vs 20k"; all.push_back (e);
        auto f = knobCtl ("AIR FREQ", 2.5, 3.5, knobTo ("AIR", 4.0), "AIR +4 dB", false); f.name = "AIR FREQ 20k vs 40k"; all.push_back (f);
    }
    all.push_back (knobCtl ("TILT", -1.0, 1.0));
    all.push_back (knobCtl ("HIGH SHELF", -4.0, 4.0));
    all.push_back (knobCtl ("SHELF FREQ#2", 3000.0, 12000.0, knobTo ("HIGH SHELF", 4.0), "HIGH SHELF +4 dB", true, true, " (high)"));
    all.push_back (knobCtl ("BELL 2", -4.0, 4.0));
    all.push_back (knobCtl ("BELL 2 FREQ", 1000.0, 8000.0, knobTo ("BELL 2", 4.0), "BELL 2 +4 dB", true));
    all.push_back (knobCtl ("BELL 2 Q", 0.4, 5.0, seq ({ knobTo ("BELL 2", 4.0), knobTo ("BELL 2 FREQ", 3000.0) }), "BELL 2 +4 dB at 3 kHz", true));
    all.push_back (knobCtl ("LO-MID CUT", 0.0, 3.5));
    all.push_back (knobCtl ("LO-MID FREQ", 180.0, 600.0, knobTo ("LO-MID CUT", 3.5), "LO-MID CUT 3.5 dB (band switched in)", true));
    all.push_back (knobCtl ("HI-MID CUT", 0.0, 3.5));
    all.push_back (knobCtl ("HI-MID FREQ", 2000.0, 6000.0, knobTo ("HI-MID CUT", 3.5), "HI-MID CUT 3.5 dB", true));
    all.push_back (knobCtl ("TIGHT CUT", 0.0, 3.5));
    all.push_back (knobCtl ("TIGHT FREQ", 50.0, 150.0, knobTo ("TIGHT CUT", 3.5), "TIGHT CUT 3.5 dB", true));
    all.push_back (knobCtl ("DE-HARSH", 0.0, 8.0, seq ({ knobTo ("AIR", 3.0), swTo ("AIR DYN", true) }), "AIR +3 dB, AIR DYN on", true));
    all.push_back (swCtl ("EQ IN", seq ({ knobTo ("LOW SHELF", 4.0), knobTo ("BELL 1", 3.0) }), "LOW SHELF +4, BELL 1 +3"));
    all.push_back (swCtl ("LINEAR", knobTo ("BELL 1", 4.0), "BELL 1 +4 dB"));
    all.push_back (swCtl ("AIR DYN", knobTo ("AIR", 4.0), "AIR +4 dB"));
    all.push_back (swCtl ("LOW-MID DYN", knobTo ("LO-MID CUT", 3.5), "LO-MID CUT 3.5 dB"));
    all.push_back (swCtl ("UPPER-MID DYN", knobTo ("HI-MID CUT", 3.5), "HI-MID CUT 3.5 dB"));
    all.push_back (swCtl ("TIGHTEN", knobTo ("TIGHT CUT", 3.5), "TIGHT CUT 3.5 dB"));

    // ---- COMPRESSOR, WIDE -----------------------------------------------------------------------------------------------
    all.push_back (knobCtl ("GLUE", 0.0, 3.5, swTo ("COMP IN", true), "COMP IN", true));
    all.push_back (knobCtl ("THRESHOLD", -45.0, -12.0, swTo ("COMP IN", true), "COMP IN", true));
    all.push_back (knobCtl ("RATIO", 1.5, 6.0, compOnManual, compText, true));
    all.push_back (knobCtl ("ATTACK", 1.0, 100.0, compHard, compHardText, true));
    all.push_back (knobCtl ("RELEASE", 40.0, 800.0, compHard, compHardText, true));
    all.push_back (knobCtl ("SC HPF", 20.0, 300.0, compHard, compHardText, true));
    all.push_back (knobCtl ("RMS / PEAK", 0.0, 100.0, compHard, compHardText, true));
    all.push_back (knobCtl ("KNEE", 0.0, 12.0, seq ({ swTo ("COMP IN", true), knobTo ("THRESHOLD", -22.0), knobTo ("RATIO", 4.0) }), "COMP IN, THRESHOLD -22, RATIO 4", true));
    all.push_back (knobCtl ("MIX", 100.0, 30.0, compHard, compHardText, true));
    all.push_back (swCtl ("COMP IN", seq ({ knobTo ("THRESHOLD", -30.0), knobTo ("RATIO", 4.0) }), "THRESHOLD -30, RATIO 4"));

    // ---- COMPRESSOR, 3-BAND (every band) --------------------------------------------------------------------------------
    const char* bandName[] { "LOW", "MID", "HIGH" };
    const juce::String bandText[] { "3-BAND, LOW band", "3-BAND, MID band", "3-BAND, HIGH band" };
    for (int b = 0; b < 3; ++b)
    {
        const Fn threeBand = seq ({ swTo ("3-BAND", true), selectBand (b) });
        const Fn threeHard = seq ({ swTo ("3-BAND", true), selectBand (b), knobTo ("THRESHOLD", -30.0), knobTo ("RATIO", 4.0) });
        const juce::String suf = juce::String (" [") + bandName[b] + "]";
        const juce::String hardText = bandText[b] + ", THRESHOLD -30, RATIO 4";
        auto add = [&] (Ctl c) { all.push_back (std::move (c)); };
        add (knobCtl ("GLUE", 0.0, 3.5, threeBand, bandText[b], true, true, suf));
        add (knobCtl ("THRESHOLD", -45.0, -12.0, threeBand, bandText[b], true, true, suf));
        add (knobCtl ("RATIO", 1.5, 6.0, seq ({ swTo ("3-BAND", true), selectBand (b), knobTo ("THRESHOLD", -30.0) }), bandText[b] + ", THRESHOLD -30", true, true, suf));
        add (knobCtl ("ATTACK", 1.0, 100.0, threeHard, hardText, true, true, suf));
        add (knobCtl ("RELEASE", 40.0, 800.0, threeHard, hardText, true, true, suf));
        add (knobCtl ("BAND GAIN", -4.0, 4.0, threeBand, bandText[b], true, true, suf));
        { auto c = swCtl ("SOLO", threeBand, bandText[b], true); c.name += suf; add (c); }
        { auto c = swCtl ("BAND BYPASS", threeHard, hardText, true); c.name += suf; add (c); }
    }
    {
        const Fn threeHard = seq ({ swTo ("3-BAND", true), selectBand (1), knobTo ("THRESHOLD", -30.0), knobTo ("RATIO", 4.0) });
        const juce::String t = "3-BAND, MID band THRESHOLD -30, RATIO 4";
        all.push_back (knobCtl ("RMS / PEAK", 0.0, 100.0, threeHard, t, true, true, " [3-BAND]"));
        all.push_back (knobCtl ("KNEE", 0.0, 12.0, seq ({ swTo ("3-BAND", true), selectBand (1), knobTo ("THRESHOLD", -22.0), knobTo ("RATIO", 4.0) }), "3-BAND, MID band THRESHOLD -22, RATIO 4", true, true, " [3-BAND]"));
        all.push_back (knobCtl ("MIX", 100.0, 30.0, threeHard, t, true, true, " [3-BAND]"));
    }
    all.push_back (knobCtl ("XOVER LOW", 60.0, 300.0, seq ({ swTo ("3-BAND", true), selectBand (0), knobTo ("BAND GAIN", 4.0) }), "3-BAND, LOW band gain +4 dB", true));
    all.push_back (knobCtl ("XOVER HIGH", 2500.0, 8000.0, seq ({ swTo ("3-BAND", true), selectBand (2), knobTo ("BAND GAIN", 4.0) }), "3-BAND, HIGH band gain +4 dB", true));
    all.push_back (swCtl ("3-BAND", seq ({ swTo ("COMP IN", true), knobTo ("THRESHOLD", -30.0), knobTo ("RATIO", 4.0) }), "COMP IN, THRESHOLD -30, RATIO 4"));

    // ---- STEREO / LOW END -----------------------------------------------------------------------------------------------
    all.push_back (knobCtl ("WIDTH", 0.0, 200.0));
    all.push_back (knobCtl ("MONO BELOW", 0.0, 300.0));
    all.push_back (knobCtl ("HIGH WIDTH", 0.0, 200.0));
    all.push_back (knobCtl ("HIGH FROM", 1500.0, 9000.0, knobTo ("HIGH WIDTH", 0.0), "HIGH WIDTH 0 %", true));
    all.push_back (swCtl ("WIDTH IN", knobTo ("WIDTH", 200.0), "WIDTH 200 %"));
    all.push_back (swCtl ("PHASE SAFE", knobTo ("MONO BELOW", 200.0), "MONO BELOW 200 Hz", true));

    // ---- LIMITER (CLUB -8 so the limiter works) -------------------------------------------------------------------------
    const Fn club = clickText ("CLUB -8");
    all.push_back (knobCtl ("DRIVE", -6.0, -2.0, {}, {}, false, true));
    all.push_back (knobCtl ("CEILING", -3.0, -0.3, club, "target CLUB -8", true));
    all.push_back (knobCtl ("RELEASE#2", 10.0, 800.0, club, "target CLUB -8", true, true, " (limiter)"));
    all.push_back (knobCtl ("LOOK-AHEAD", 0.5, 5.0, club, "target CLUB -8", true));

    // ---- TARGET, both faces ---------------------------------------------------------------------------------------------
    all.push_back (knobCtl ("TARGET", -20.0, -10.0));
    all.push_back (knobCtl ("TARGET", -20.0, -10.0, {}, {}, false, false));
    all.push_back (knobCtl ("CEILING", -3.0, -0.3, club, "target CLUB -8", true, false));

    // ---- REFERENCE ------------------------------------------------------------------------------------------------------
    {
        auto c = knobCtl ("TONE AMOUNT", 0.0, 100.0, {}, "reference loaded + MATCH TO REFERENCE"); c.withRef = true; all.push_back (c);
        auto d = knobCtl ("SMOOTH", 0.33, 3.0, {}, "reference loaded + MATCH TO REFERENCE"); d.withRef = true; all.push_back (d);
    }

    for (const auto& c : all) runCtl (fx, c);

    // ---- AIR FREQ: zones, click, dimming -------------------------------------------------------------------------------------
    {
        Rig r (true, "control_audit");
        r.load (fx.signal, fs, "music.wav");
        const double zoneStarts[] { 0.0, 0.05, 0.99, 1.01, 1.99, 2.01, 2.99, 3.01, 3.99, 4.0 };
        const double want[] { 10000, 10000, 10000, 15000, 15000, 20000, 20000, 40000, 40000, 40000 };
        bool ok = true; juce::String bad;
        for (int i = 0; i < 10; ++i)
        {
            r.panel->turnKnobForTesting ("AIR FREQ", zoneStarts[i] == 2.01 ? 0.3 : 3.7);   // a jump away first, so the next value always changes
            r.panel->turnKnobForTesting ("AIR FREQ", zoneStarts[i]);
            if (std::abs (r.session().settings.eq.airHz - want[i]) > 1.0) { ok = false; bad += " " + f2 (zoneStarts[i], 2) + "->" + f2 (r.session().settings.eq.airHz, 0); }
        }
        check ("AIR FREQ zones", "ADV", "slider 0..4, four equal zones", ok, ok ? "every zone edge picks the right step (10k/15k/20k/40k)" : "wrong:" + bad);
        // From rest (the middle of the 20 kHz zone) a drag of 30 px (1 zone on the 120 px throw) selects the neighbour.
        r.panel->turnKnobForTesting ("AIR FREQ", 2.5);
        r.panel->turnKnobForTesting ("AIR FREQ", 3.01);
        const bool crossed = std::abs (r.session().settings.eq.airHz - 40000.0) < 1.0;
        check ("AIR FREQ crossing", "ADV", "from rest, +0.51 zone", crossed, crossed ? "crossing into the next zone selects it (resting pointer " + f2 (r.panel->knobValueForTesting ("AIR FREQ"), 2) + ")" : "stayed on " + f2 (r.session().settings.eq.airHz, 0));
        const double rest = r.panel->knobValueForTesting ("AIR FREQ");
        check ("AIR FREQ rest", "ADV", "after a step", std::abs (rest - 3.5) < 1e-6, "pointer rests in the middle of its zone: " + f2 (rest, 2) + " (want 3.50)");
        // Click on the value: next step, wrapping.
        r.panel->turnKnobForTesting ("AIR FREQ", 2.5);
        double seqHz[4]; for (int i = 0; i < 4; ++i) { r.panel->clickKnobValueForTesting ("AIR FREQ"); seqHz[i] = r.session().settings.eq.airHz; }
        check ("AIR FREQ click", "ADV", "click on the value", seqHz[0] == 40000.0 && seqHz[1] == 10000.0 && seqHz[2] == 15000.0 && seqHz[3] == 20000.0,
              "20k -> " + f2 (seqHz[0] / 1000, 0) + " -> " + f2 (seqHz[1] / 1000, 0) + " -> " + f2 (seqHz[2] / 1000, 0) + " -> " + f2 (seqHz[3] / 1000, 0) + " kHz");
        // Dimming / hint.
        r.panel->turnKnobForTesting ("AIR", 0.0);
        const bool dim0 = r.panel->knobDimmedForTesting ("AIR FREQ"); const auto hint0 = r.panel->knobHintForTesting ("AIR FREQ");
        r.panel->turnKnobForTesting ("AIR", 3.0);
        r.sync();
        const bool dim3 = r.panel->knobDimmedForTesting ("AIR FREQ");
        check ("AIR FREQ dimming", "ADV", "AIR 0 dB / AIR +3 dB", dim0 && ! dim3 && hint0 == "SET AIR", juce::String ("dimmed at AIR 0: ") + (dim0 ? "yes" : "NO") + ", hint \"" + hint0 + "\", dimmed at +3: " + (dim3 ? "YES" : "no"));
    }

    // ---- WARN AT: no audio, only the warning threshold ------------------------------------------------------------------
    {
        auto rig = makeRig (fx, true);
        auto& r = *rig;
        r.click ("CLUB -8");
        r.panel->turnKnobForTesting ("WARN AT", 1.0); r.render();
        const auto lo = r.session().warnings.joinIntoString ("|") + "/" + r.session().plan.limiterStatus + "/" + juce::String (r.session().plan.limiterSeverity);
        const bool loLamp = r.session().warnings.size() > 0;
        r.panel->turnKnobForTesting ("WARN AT", 12.0); r.render();
        const auto hi = r.session().warnings.joinIntoString ("|") + "/" + r.session().plan.limiterStatus + "/" + juce::String (r.session().plan.limiterSeverity);
        const bool hiLamp = r.session().warnings.size() > 0;
        check ("WARN AT", "ADV", "CLUB -8 (audio unchanged by design)", lo != hi || loLamp != hiLamp,
              juce::String ("warnings at 1 dB: ") + (loLamp ? "yes" : "no") + ", at 12 dB: " + (hiLamp ? "yes" : "no") + (lo == hi ? " (text identical)" : " (text differs)"));
    }

    // ---- target presets -------------------------------------------------------------------------------------------------
    {
        const char* names[] { "STREAM -14", "APPLE -16", "CLUB -8", "KEEP INPUT" };
        const double want[] { -14.0, -16.0, -8.0, 0.0 };
        auto rig = makeRig (fx, true);
        auto& r = *rig;
        const double inL = r.session().inMeas.integrated;
        for (int i = 0; i < 4; ++i)
        {
            r.click (names[i]);
            const double out = integratedLoudness (*r.session().output, fs);
            const double w = i == 3 ? inL : want[i];
            check (names[i], "ADV", "preset cap", std::abs (out - w) <= 0.6, "output " + f2 (out, 2) + " LUFS, wanted " + f2 (w, 1) + (i == 3 ? " (input " + f2 (inL, 2) + ")" : ""), out - w);
        }
        r.panel->turnKnobForTesting ("TARGET", -11.0); r.render();
        const double out = integratedLoudness (*r.session().output, fs);
        check ("CUSTOM", "ADV", "TARGET knob -11", std::abs (out + 11.0) <= 0.6, "output " + f2 (out, 2) + " LUFS, wanted -11.0", out + 11.0);
    }

    // ---- CHARACTER ----------------------------------------------------------------------------------------------------------
    {
        auto rig = makeRig (fx, true);
        auto& r = *rig;
        r.click ("OFF"); const auto off = copyOf (*r.session().output);
        r.click ("GENTLE"); const auto gentle = copyOf (*r.session().output);
        r.click ("FULL"); const auto full = copyOf (*r.session().output);
        const double d1 = deltaDb (off, gentle), d2 = deltaDb (off, full);
        check ("CHARACTER GENTLE", "ADV", "vs OFF, dark synthetic top", d1 > kPassDb, d1 > kPassDb ? "ok" : "OUTPUT UNCHANGED", d1);
        check ("CHARACTER FULL", "ADV", "vs OFF, synthetic mix", d2 > kPassDb, d2 > kPassDb ? "ok" : "OUTPUT UNCHANGED", d2);
    }

    // ---- band selector caps ---------------------------------------------------------------------------------------------------
    {
        auto rig = makeRig (fx, true);
        auto& r = *rig;
        r.panel->clickSwitchForTesting ("3-BAND", true);
        r.panel->clickSwitchForTesting ("BAND LOW", true);
        r.panel->selectBand (0); r.panel->turnKnobForTesting ("RATIO", 5.0);
        r.panel->selectBand (1); r.panel->tickForTesting (40); const double mid = r.panel->knobValueForTesting ("RATIO");
        r.panel->selectBand (0); r.panel->tickForTesting (40); const double low = r.panel->knobValueForTesting ("RATIO");
        check ("BAND LOW / MID / HIGH", "ADV", "3-BAND: select a band", std::abs (mid - low) > 0.5 && std::abs (low - 5.0) < 0.06, "RATIO shows " + f2 (low, 2) + " on LOW and " + f2 (mid, 2) + " on MID (the caps pick whose values the knobs show)");
    }

    // ---- A/B MATCHED, export word length ----------------------------------------------------------------------------------------
    {
        auto rig = makeRig (fx, true);
        auto& r = *rig;
        r.click ("STREAM -14");
        const float gOn = r.session().originalGainDb();
        if (auto* b = findButton (*r.panel, "A/B MATCHED")) fire (b);
        const float gOff = r.session().originalGainDb();
        check ("A/B MATCHED", "ADV", "STREAM -14", std::abs (gOn - gOff) > 0.5, "A plays " + f2 (gOn, 2) + " dB with it on, " + f2 (gOff, 2) + " dB raw", (double) (gOn - gOff));
        const auto f16 = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("mihenk_ca_16.wav"), f24 = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("mihenk_ca_24.wav");
        juce::String m1, m2;
        const bool ok1 = r.session().exportMaster (f16, 16, m1), ok2 = r.session().exportMaster (f24, 24, m2);
        const auto a16 = loadAudioFile (f16), a24 = loadAudioFile (f24);
        const double d = a16.ok() && a24.ok() ? deltaDb (a24.buffer, a16.buffer) : 99.0;
        check ("EXPORT 16 / 24-BIT", "ADV", "exportMaster", ok1 && ok2 && d > -140.0 && f16.getSize() < f24.getSize(), "16-bit file " + juce::String (f16.getSize()) + " B vs 24-bit " + juce::String (f24.getSize()) + " B, quantisation difference", d);
        f16.deleteFile(); f24.deleteFile(); f16.withFileExtension ("mihenk.txt").deleteFile(); f24.withFileExtension ("mihenk.txt").deleteFile();
    }

    // ---- SIMPLE face: the macros are plan displays ---------------------------------------------------------------------------------
    {
        auto rig = makeRig (fx, false);
        auto& r = *rig;
        r.click ("FULL");
        const char* macros[] { "LOW END", "TONE", "AIR", "GLUE", "WIDTH", "LEVEL" };
        for (auto* m : macros)
        {
            const bool disabled = ! r.panel->knobEnabledForTesting (m);
            check (juce::String (m) + " (macro)", "SIMPLE", "plan display (SPEC_V3)", disabled, "read-only by design; shows the plan: " + r.panel->knobTextForTesting (m) + (disabled ? "" : "  <- ENABLED: would write nothing"));
        }
    }

    // ---- not audio ---------------------------------------------------------------------------------------------------------------
    na ("WAVE / SPECTRUM / LOUDNESS", "both", "CRT view selector");
    na ("IN / EQ / COMP / OUT (layers)", "ADV", "which layers the live spectrum draws");
    na ("A / B key", "app", "monitor: original vs master in the player; covered by ui_check / v3_session_check");
    na ("BYPASS key", "both", "monitor: instant original in the player, master keeps rendering; bit-exact, covered by v3_session_check");
    na ("LIMITER IN (switch)", "ADV", "disabled on purpose: the true-peak limiter is the ceiling and is always in (SPEC_V3); tooltip says so");
    na ("PLAY / STOP / LOOP / OPEN / REF / EXPORT / BATCH / AUDIO", "app", "transport and file keys, no sound parameter");
    na ("SAVE / LOAD / DELETE (presets)", "both", "file actions, covered by ui_check / render --selftest");

    // ---- UI wiring: every knob's parameter id is known to the processor bridge and to the plan ------------------------------------------------
    {
        Rig r (true, "control_audit");
        r.load (fx.signal, fs, "music.wav");
        const char* labels[] { "TRIM", "BALANCE", "LOW CUT", "LOW SHELF", "SHELF FREQ", "BELL 1", "BELL 1 FREQ", "BELL 1 Q", "AIR", "TILT", "HIGH SHELF", "BELL 2", "BELL 2 FREQ", "BELL 2 Q",
                               "AIR FREQ", "LO-MID FREQ", "LO-MID CUT", "HI-MID FREQ", "HI-MID CUT", "TIGHT FREQ", "TIGHT CUT", "DE-HARSH", "GLUE", "THRESHOLD", "RATIO", "ATTACK", "RELEASE",
                               "SC HPF", "RMS / PEAK", "KNEE", "MIX", "WIDTH", "MONO BELOW", "HIGH WIDTH", "HIGH FROM", "DRIVE", "CEILING", "LOOK-AHEAD", "WARN AT", "TARGET" };
        juce::StringArray missing;
        for (auto* l : labels)
        {
            const auto id = r.panel->knobParamIdForTesting (l);
            const bool inBridge = id.isNotEmpty() && r.p.getBridge().find (id) != nullptr;
            const bool inPlan = overrideIds().contains (id) || id == "ceiling" || id == "targetLufs" || id == "warnAt";
            if (! inBridge && ! inPlan) missing.add (juce::String (l) + " (" + id + ")");
        }
        check ("knob -> parameter id", "ADV", juce::String (sizeof (labels) / sizeof (labels[0])) + " knobs", missing.isEmpty(), missing.isEmpty() ? "every knob's id is a processor parameter or a plan id" : "unknown to both: " + missing.joinIntoString (", "));
    }

    // ---- Owner's report 4 Oct: dynamic EQ AUTO solved to 0 dB / 0 cut on dense material, mono below leaving correlation unchanged -----------------------
    {
        // (name, audio, rate, is a finished master by design): "flat" = the synthetic mix without verse / chorus (LRA about 1 LU, the dense path), then corpus files.
        struct Src { juce::String name; Buffer audio; double rate; bool finished; };
        std::vector<Src> srcs;
        srcs.push_back ({ "synthetic flat (dense, LRA small)", musicLike (24.0, false, 5, true), fs, false });
        srcs.push_back ({ "synthetic verse/chorus", copyOf (fx.signal), fs, false });
        srcs.push_back ({ "corpus bag_asis (finished master)", loadCorpus ("bag_asis", 40.0), corpusRate(), true });
        for (auto& src : srcs)
        {
            Rig r (true, "control_audit");
            r.load (src.audio, src.rate, "dyn.wav");
            r.panel->clickSwitchForTesting ("LOW-MID DYN", true); r.panel->turnKnobForTesting ("LO-MID CUT", 1.5);
            r.panel->clickSwitchForTesting ("UPPER-MID DYN", true); r.panel->turnKnobForTesting ("HI-MID CUT", 1.5);
            r.panel->clickSwitchForTesting ("TIGHTEN", true); r.panel->turnKnobForTesting ("TIGHT CUT", 1.5);
            r.panel->clickSwitchForTesting ("COMP IN", true); r.panel->turnKnobForTesting ("GLUE", 1.0);
            r.render(); r.panel->tickForTesting (40);
            auto& s2 = r.session();
            const auto& d = s2.dynEq;
            const bool worked = d.lowMid.loudMeanCutDb > 0.3 && d.upperMid.loudMeanCutDb > 0.3 && d.tightener.loudMeanCutDb > 0.3;
            const bool dense = s2.glue.valid && s2.glue.dense;
            const bool honest = s2.plan.finishedMaster && s2.plan.refused.size() > 0 && r.panel->knobDimmedForTesting ("LO-MID CUT") && r.panel->knobDimmedForTesting ("GLUE");
            check ("dyn EQ + glue AUTO", "ADV", src.name + ": LRA " + f2 (s2.inMeas.lra, 1) + " LU, glue dense " + (dense ? "yes" : "no"), src.finished ? honest : worked,
                   juce::String (src.finished ? "refused by design (finished master), knobs dimmed: " + juce::String (honest ? "yes" : "NO") + ", " : "") + "thr " + f2 (d.lowMid.thresholdDb, 1) + " / " + f2 (d.upperMid.thresholdDb, 1) + " / " + f2 (d.tightener.thresholdDb, 1)
                       + " dB, mean cut on loud part " + f2 (d.lowMid.loudMeanCutDb, 2) + " / " + f2 (d.upperMid.loudMeanCutDb, 2) + " / " + f2 (d.tightener.loudMeanCutDb, 2) + " dB (target 1.5), max "
                       + f2 (d.lowMid.maxCutDb, 1) + " / " + f2 (d.upperMid.maxCutDb, 1) + " / " + f2 (d.tightener.maxCutDb, 1) + "; comp loud-part mean GR " + f2 (s2.glue.loudMeanGrDb, 2) + " dB (target " + f2 (s2.glue.targetGrDb, 2) + ")");
        }
        // MONO BELOW: does the side signal below the frequency go away, on a real file?
        for (int which = 0; which < 2; ++which)
        {
            const bool corpus = which == 1;
            Rig r (true, "control_audit");
            const double rate = corpus ? corpusRate() : fs;
            r.load (corpus ? loadCorpus ("bag_asis", 40.0) : musicLike (24.0, false, 5, true), rate, "mono.wav");
            const juce::String stem = corpus ? "corpus bag_asis (finished master)" : "synthetic flat";
            auto sideLow = [] (const Buffer& b, double rate)
            {
                juce::dsp::IIR::Filter<float> lp; lp.coefficients = juce::dsp::IIR::Coefficients<float>::makeLowPass (rate, 120.0, 0.707f);
                double es = 0.0, em = 0.0;
                std::vector<float> m ((size_t) b.getNumSamples()), sd (m.size());
                for (int i = 0; i < b.getNumSamples(); ++i) { m[(size_t) i] = 0.5f * (b.getSample (0, i) + b.getSample (1, i)); sd[(size_t) i] = 0.5f * (b.getSample (0, i) - b.getSample (1, i)); }
                juce::dsp::IIR::Filter<float> lp2; lp2.coefficients = lp.coefficients;
                for (size_t i = 0; i < m.size(); ++i) { const float a = lp.processSample (m[i]), c = lp2.processSample (sd[i]); em += (double) a * a; es += (double) c * c; }
                return std::make_pair (toDb (std::sqrt (em / (double) m.size())), toDb (std::sqrt (es / (double) m.size())));
            };
            const auto before = copyOf (*r.session().output);
            r.panel->turnKnobForTesting ("MONO BELOW", 120.0); r.render();
            const auto after = copyOf (*r.session().output);
            const auto a = sideLow (before, rate), b = sideLow (after, rate);
            check ("MONO BELOW 120", "ADV", stem + ": side below 120 Hz", corpus ? (r.session().plan.finishedMaster && r.panel->knobDimmedForTesting ("MONO BELOW")) : (b.second < a.second - 6.0 || a.second < -70.0),
                   "finished master " + juce::String ((int) r.session().plan.finishedMaster) + ", width in " + juce::String ((int) r.session().settings.width.enabled) + ", mono below " + f2 (r.session().settings.width.monoBelowHz, 0) + "; low side " + f2 (a.second, 1) + " -> " + f2 (b.second, 1) + " dBFS (low mid " + f2 (a.first, 1) + " -> " + f2 (b.first, 1) + "), broadband correlation " + f2 (analyse (before, rate, false).correlation, 2) + " -> " + f2 (analyse (after, rate, false).correlation, 2));
        }
    }

    // ---- knob travel vs the plug-in parameter's range (a value the knob offers must be a value the parameter holds) -------------------------------
    {
        Rig r (true, "control_audit");
        r.load (fx.signal, fs, "music.wav");
        r.panel->clickSwitchForTesting ("3-BAND", false);
        const char* labels[] { "TRIM", "BALANCE", "LOW CUT", "LOW SHELF", "SHELF FREQ", "SHELF FREQ#2", "BELL 1", "BELL 1 FREQ", "BELL 1 Q", "AIR", "TILT", "HIGH SHELF", "BELL 2", "BELL 2 FREQ", "BELL 2 Q",
                               "LO-MID FREQ", "LO-MID CUT", "HI-MID FREQ", "HI-MID CUT", "TIGHT FREQ", "TIGHT CUT", "DE-HARSH", "GLUE", "THRESHOLD", "RATIO", "ATTACK", "RELEASE", "RELEASE#2",
                               "SC HPF", "RMS / PEAK", "KNEE", "MIX", "WIDTH", "MONO BELOW", "HIGH WIDTH", "HIGH FROM", "DRIVE", "CEILING", "LOOK-AHEAD", "WARN AT", "TARGET" };
        juce::StringArray bad;
        int n = 0;
        for (auto* l : labels)
        {
            double mn = 0, mx = 0, sc = 1;
            const auto id = r.panel->knobParamIdForTesting (l);
            if (! r.panel->knobRangeForTesting (l, mn, mx, sc) || id.isEmpty()) continue;
            for (const auto& d : mihenk::params::definitions())
                if (d.id == id && d.kind == mihenk::params::Def::real)
                {
                    ++n;
                    const double lo = mn / sc, hi = mx / sc;
                    if (lo < d.minimum - 1.0e-6 || hi > d.maximum + 1.0e-6)
                        bad.add (juce::String (l) + ": knob " + f2 (lo, 2) + ".." + f2 (hi, 2) + " > parameter " + f2 (d.minimum, 2) + ".." + f2 (d.maximum, 2));
                }
        }
        check ("knob range vs parameter range", "ADV", juce::String (n) + " continuous knobs", bad.isEmpty(), bad.isEmpty() ? "no knob offers a value its plug-in parameter cannot hold" : bad.joinIntoString ("; "));
    }

    // ---- table out ------------------------------------------------------------------------------------------------------------------------
    juce::String tsv = "result\tcontrol\tface\tcontext\tdelta_dB_rms\tnote\n";
    int pass = 0;
    for (auto& r : rows) { tsv << (r.pass ? "PASS" : "FAIL") << "\t" << r.control << "\t" << r.face << "\t" << r.context << "\t" << f2 (r.delta, 1) << "\t" << r.note << "\n"; if (r.pass) ++pass; }
    outDir().getChildFile ("control_audit.tsv").replaceWithText (tsv);
    std::printf ("\ncontrol_audit: %d rows, PASS %d, FAIL %d  (table: %s)\n", (int) rows.size(), pass, fails, outDir().getChildFile ("control_audit.tsv").getFullPathName().toRawUTF8());
    (void) dummy;
    return fails;
}
