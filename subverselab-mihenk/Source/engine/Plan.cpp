#include "Plan.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace mihenk
{
namespace
{
double targetOf (const Measurements& m, const Intent& i)
{
    switch (i.target)
    {
        case PlanTarget::STREAM_14:  return -14.0;
        case PlanTarget::APPLE_16:   return -16.0;
        case PlanTarget::CLUB_8:     return -8.0;
        case PlanTarget::KEEP_INPUT: return m.integrated;
        case PlanTarget::ALBUM:      return i.albumTarget + i.relativeDb;
        case PlanTarget::CUSTOM:     return i.customLufs;
    }
    return i.customLufs;
}

// SPEC_V3 section 2: finished material is level and ceiling only.
bool isFinished (const Measurements& m, double target)
{
    return m.plr < kDensePlrLu || m.crest < kDenseCrestDb || (m.samplePeak > -3.0 && std::abs (m.integrated - target) <= 3.0);
}

juce::String num (double v)   // "8" for 8.0, "9.2" otherwise
{
    return std::abs (v - std::round (v)) < 0.05 ? juce::String ((int) std::lround (v)) : juce::String (v, 1);
}
juce::String sgn (double v, int d = 1) { return (v >= 0.0 ? "+" : "") + juce::String (v, d); }

//------------------------------------------------------------------------------------------ spectrum -> band energies
double freqAt (size_t k, size_t n) { return n < 2 ? 1000.0 : 20.0 * std::pow (2.0, 10.0 * (double) k / (double) (n - 1)); }

double meanIn (const std::vector<float>& sp, double lo, double hi, bool& ok)
{
    double s = 0.0; int c = 0;
    for (size_t k = 0; k < sp.size(); ++k) { const double f = freqAt (k, sp.size()); if (f >= lo && f <= hi) { s += sp[k]; ++c; } }
    if (c == 0) { ok = false; return 0.0; }
    return s / c;
}

// Mean distance above the straight line (dB against log2 f, 100 Hz..10 kHz) fitted without the region under test +-1/2 octave.
double lineDev (const std::vector<float>& sp, double lo, double hi, bool& ok)
{
    double sx = 0, sy = 0, sxx = 0, sxy = 0; int n = 0;
    for (size_t k = 0; k < sp.size(); ++k)
    {
        const double f = freqAt (k, sp.size());
        if (f < 100.0 || f > 10000.0 || (f >= lo / 1.4142 && f <= hi * 1.4142)) continue;
        const double x = std::log2 (f), y = sp[k];
        sx += x; sy += y; sxx += x * x; sxy += x * y; ++n;
    }
    const double den = n * sxx - sx * sx;
    if (n < 2 || std::abs (den) < 1.0e-9) { ok = false; return 0.0; }
    const double a = (n * sxy - sx * sy) / den, b = (sy - a * sx) / n;
    double d = 0.0; int c = 0;
    for (size_t k = 0; k < sp.size(); ++k)
    {
        const double f = freqAt (k, sp.size());
        if (f >= lo && f <= hi) { d += sp[k] - (a * std::log2 (f) + b); ++c; }
    }
    if (c == 0) { ok = false; return 0.0; }
    return d / c;
}

Measurements::BandEnergies bandsOf (const Measurements& m)
{
    if (m.bandsValid) return m.bands;
    Measurements::BandEnergies b;
    const auto& sp = m.spectrum;
    if (sp.size() < 8) return b;
    bool ok = true;
    auto g = [&] (double v) { return ok ? v : 0.0; };
    ok = true; { const double a = meanIn (sp, 25, 60, ok), c = meanIn (sp, 100, 300, ok); b.sub = g (a - c); }
    ok = true; { const double a = meanIn (sp, 40, 120, ok), c = meanIn (sp, 300, 1000, ok); b.low = g (a - c); }
    ok = true; { const double a = lineDev (sp, 200, 500, ok); b.lowMid = g (a); }
    ok = true; { const double a = lineDev (sp, 1000, 4000, ok), c = lineDev (sp, 300, 1000, ok); b.mid = g (a - c); }
    ok = true; { const double a = lineDev (sp, 2000, 5000, ok); b.presence = g (a); }
    ok = true; { const double a = meanIn (sp, 10000, 16000, ok), c = meanIn (sp, 1000, 4000, ok); b.air = g (a - c); }
    return b;
}

const Finding* findingFor (const Measurements& m, int scenario, std::initializer_list<const char*> aliases)
{
    for (const auto& f : m.findings)
    {
        if (f.scenario == scenario) return &f;
        for (auto a : aliases) if (f.id == a) return &f;
    }
    return nullptr;
}
double severityFloor (const Finding* f, double warnValue, double problemValue)
{
    if (f == nullptr) return 0.0;
    return f->severity == Severity::problem ? problemValue : warnValue;
}

//------------------------------------------------------------------------------------------ overrides
// An override is a parameter id and an absolute value (switches 0 / 1, choices by index). The ids are the panel's and the
// plug-in's parameter ids; the older long names stay as aliases. Every override is applied on top of the plan.
enum class Stage { level, eq, comp, width };
struct Slot
{
    std::function<double()> get;
    std::function<void (double)> set;
    Stage stage = Stage::level;
    bool isOn = false;            // a stage switch (eqOn / compOn / widthOn): an explicit "in" or "out" for its stage
    bool isGain = false;          // trim or drive
};

bool slotFor (MasterSettings& s, const juce::String& id, Slot& out)
{
    auto num = [&] (double& v, Stage st) { out.get = [&v] { return v; }; out.set = [&v] (double x) { v = x; }; out.stage = st; return true; };
    auto flag = [&] (bool& v, Stage st, bool on = false) { out.get = [&v] { return v ? 1.0 : 0.0; }; out.set = [&v] (double x) { v = x > 0.5; }; out.stage = st; out.isOn = on; return true; };
    static constexpr double kAir[] { 10000.0, 15000.0, 20000.0, 40000.0 };
    auto& e = s.eq; auto& c = s.comp; auto& w = s.width;
    if (id == "drive")  { num (s.limiter.driveDb, Stage::level); out.isGain = true; return true; }
    if (id == "trim")   { num (s.trimDb, Stage::level); out.isGain = true; return true; }
    if (id == "ceilingDbtp" || id == "ceiling") return num (s.limiter.ceilingDbtp, Stage::level);
    if (id == "limRelease") return num (s.limiter.releaseMs, Stage::level);
    if (id == "lookahead")  return num (s.limiter.lookaheadMs, Stage::level);
    if (id == "balance") return num (s.balanceDb, Stage::eq);
    if (id == "dc") return flag (s.dcFilter, Stage::eq);
    // EQ
    if (id == "eqOn") return flag (e.enabled, Stage::eq, true);
    if (id == "eqLinear") return flag (e.linearPhase, Stage::eq);
    if (id == "lowCutOn") return flag (e.lowCutOn, Stage::eq);
    if (id == "lowCutHz") return num (e.lowCutHz, Stage::eq);
    if (id == "lowCutSteep") { out.get = [&e] { return e.lowCutSlope >= 24 ? 1.0 : 0.0; }; out.set = [&e] (double x) { e.lowCutSlope = x > 0.5 ? 24 : 12; }; out.stage = Stage::eq; return true; }
    if (id == "lowShelfDb") return num (e.lowShelfDb, Stage::eq);
    if (id == "lowShelfHz") return num (e.lowShelfHz, Stage::eq);
    if (id == "tiltDbPerOct" || id == "tilt") return num (e.tiltDbPerOct, Stage::eq);
    if (id == "highShelfDb") return num (e.highShelfDb, Stage::eq);
    if (id == "highShelfHz") return num (e.highShelfHz, Stage::eq);
    if (id == "bell1Db") return num (e.bell1Db, Stage::eq);
    if (id == "bell1Hz") return num (e.bell1Hz, Stage::eq);
    if (id == "bell1Q")  return num (e.bell1Q, Stage::eq);
    if (id == "bell2Db") return num (e.bell2Db, Stage::eq);
    if (id == "bell2Hz") return num (e.bell2Hz, Stage::eq);
    if (id == "bell2Q")  return num (e.bell2Q, Stage::eq);
    if (id == "airDb")   return num (e.airDb, Stage::eq);
    if (id == "airHz")   // Hz (10000 .. 40000) or the step index 0..3 of the plug-in's choice parameter
    {
        out.get = [&e] { int b = 0; for (int i = 1; i < 4; ++i) if (std::abs (kAir[i] - e.airHz) < std::abs (kAir[b] - e.airHz)) b = i; return (double) b; };
        out.set = [&e] (double x) { e.airHz = x >= 1000.0 ? x : kAir[std::clamp ((int) std::lround (x), 0, 3)]; };
        out.stage = Stage::eq; return true;
    }
    if (id == "airDyn") return flag (e.airDynamic, Stage::eq);
    if (id == "airDeHarsh") return num (e.airDeHarshDb, Stage::eq);
    if (id == "lowMidOn") return flag (e.dynLowMid.enabled, Stage::eq);
    if (id == "lowMidHz") return num (e.dynLowMid.freqHz, Stage::eq);
    if (id == "lowMidAmount") return num (e.dynLowMid.amountDb, Stage::eq);
    if (id == "upperMidOn") return flag (e.dynUpperMid.enabled, Stage::eq);
    if (id == "upperMidHz") return num (e.dynUpperMid.freqHz, Stage::eq);
    if (id == "upperMidAmount") return num (e.dynUpperMid.amountDb, Stage::eq);
    if (id == "tightOn") return flag (e.tightener.enabled, Stage::eq);
    if (id == "tightHz") return num (e.tightener.freqHz, Stage::eq);
    if (id == "tightAmount" || id == "tightDb") return num (e.tightener.amountDb, Stage::eq);
    if (id == "matchAmount" || id == "toneAmount") return num (e.matchAmount, Stage::eq);
    // Compressor
    if (id == "compOn") return flag (c.enabled, Stage::comp, true);
    if (id == "compMode") { out.get = [&c] { return c.mode == CompMode::threeBand ? 1.0 : 0.0; }; out.set = [&c] (double x) { c.mode = x > 0.5 ? CompMode::threeBand : CompMode::wide; }; out.stage = Stage::comp; return true; }
    if (id == "compAuto") return flag (c.autoThreshold, Stage::comp);
    if (id == "glueDb" || id == "glue") return num (c.glueDb, Stage::comp);
    if (id == "threshold") return num (c.thresholdDb, Stage::comp);
    if (id == "ratio")   return num (c.ratio, Stage::comp);
    if (id == "attackMs" || id == "attack") return num (c.attackMs, Stage::comp);
    if (id == "releaseMs" || id == "release") return num (c.releaseMs, Stage::comp);
    if (id == "autoRel") return flag (c.autoRelease, Stage::comp);
    if (id == "rms") return num (c.rmsBlend, Stage::comp);
    if (id == "scHpf") return num (c.scHpfHz, Stage::comp);
    if (id == "mix")     return num (c.mix, Stage::comp);
    if (id == "kneeDb" || id == "knee") return num (c.kneeDb, Stage::comp);
    if (id == "xoverLow") return num (c.xoverLowHz, Stage::comp);
    if (id == "xoverHigh") return num (c.xoverHighHz, Stage::comp);
    if (id.length() >= 3 && id[0] == 'b' && id[1] >= '1' && id[1] <= '3')
    {
        auto& b = c.bands[id[1] - '1'];
        const auto rest = id.substring (2).toLowerCase();
        if (rest == "bypass") return flag (b.bypass, Stage::comp);
        if (rest == "solo") return flag (b.solo, Stage::comp);
        if (rest == "auto") return flag (b.autoThreshold, Stage::comp);
        if (rest == "glue") return num (b.glueDb, Stage::comp);
        if (rest == "threshold") return num (b.thresholdDb, Stage::comp);
        if (rest == "ratio") return num (b.ratio, Stage::comp);
        if (rest == "attack") return num (b.attackMs, Stage::comp);
        if (rest == "release") return num (b.releaseMs, Stage::comp);
        if (rest == "gain") return num (b.gainDb, Stage::comp);
    }
    // Stereo and low end
    if (id == "widthOn") return flag (w.enabled, Stage::width, true);
    if (id == "width")   return num (w.width, Stage::width);
    if (id == "highWidth") return num (w.highWidth, Stage::width);
    if (id == "highWidthHz") return num (w.highWidthHz, Stage::width);
    if (id == "monoBelowHz" || id == "monoBelow") return num (w.monoBelowHz, Stage::width);
    if (id == "phaseSafe") return flag (w.phaseSafeLowMono, Stage::width);
    return false;
}

void neutralise (MasterSettings& s)
{
    s.dcFilter = false; s.balanceDb = 0.0;
    auto& e = s.eq;
    e.enabled = false; e.lowCutOn = false;
    e.lowShelfDb = e.tiltDbPerOct = e.highShelfDb = e.bell1Db = e.bell2Db = e.airDb = 0.0;
    e.airDynamic = false; e.dynLowMid.enabled = e.dynUpperMid.enabled = e.tightener.enabled = false;
    e.matchCurveDb.clear(); e.matchAmount = 0.0;
    s.comp.enabled = false;
    s.width.enabled = false; s.width.width = s.width.highWidth = 1.0; s.width.monoBelowHz = 0.0; s.width.phaseSafeLowMono = false;
}
} // namespace

//================================================================================================== solvePlan
Plan solvePlan (const Measurements& m, const Intent& i)
{
    Plan p;
    auto& s = p.settings;
    const double target = targetOf (m, i);
    const bool unmeasured = ! m.present;                       // nothing analysed yet (a plug-in before LEARN): the level stays, the knobs are the user's
    const bool silent = unmeasured || ! (m.integrated > -70.0);
    const bool lowCoverage = ! unmeasured && ((m.songSeconds > 0.0 && m.coverageSeconds < 60.0 && m.coverageSeconds < 0.5 * m.songSeconds)
                                              || (m.coverageSeconds > 0.0 && m.coverageSeconds < kShortLearnSeconds));
    const bool done = ! unmeasured && isFinished (m, target);
    const bool gentle = i.character == PlanCharacter::GENTLE, full = i.character == PlanCharacter::FULL;

    // The chain must apply exactly this plan: nothing may re-solve level behind it.
    s = MasterSettings();
    neutralise (s);
    s.targetPreset = TargetPreset::custom; s.targetLufs = target; s.targetLock = false; s.autoTrim = false;
    s.toneByHand = false; s.autoSkip = {}; s.planned = true;
    s.limiter.enabled = true; s.limiter.ceilingDbtp = i.ceilingDbtp; s.limiter.driveDb = 0.0;

    //---------------------------------------------------------------- level: the gain the target needs, solved once from the measurements
    // SPEC_V3 section 2 REVISED: an explicitly chosen target (STREAM, APPLE, CLUB, CUSTOM) is REACHED by trim + the true-peak limiter.
    // The needed gain is computed here, once, from the measurements (the limiter's measured response when the analysis pass made one,
    // a model otherwise); nothing else adds to it. KEEP INPUT keeps the earlier rule (caps, limiter limit); ALBUM (album target + its relative offset) is reached like any chosen target (SPEC_V3 2 REVISED).
    const bool legacy = i.target == PlanTarget::KEEP_INPUT;
    const double reference = (lowCoverage && m.shortTermMax > -299.0) ? m.shortTermMax : m.integrated;
    const double tp = std::max (m.truePeak, m.samplePeak);
    const double plrEff = m.plr > 0.0 ? m.plr : (m.samplePeak - m.integrated);
    const double loudRef = m.loud30Percent != 0.0 ? m.loud30Percent : m.integrated;
    const double typicalPeak = std::min (tp, loudRef + plrEff - 2.0);   // a typical peak of the loud part sits ~2 dB under the highest
    auto modelGr = [&] (double g, double ceiling, double& avg, double& pk)
    {
        pk  = std::max (0.0, tp + g - ceiling);
        avg = 0.65 * std::max (0.0, typicalPeak + g - ceiling);         // mean GR is about 0.65 of the typical peak's excess (measured: 0.59 on the synthetic finished masters at +6 dB; 0.5 under-reported)
    };
    const auto& lr = m.limiter;
    // loss: how much quieter the limited signal is than trim alone would make it; avg / pk: gain reduction, dB
    auto response = [&] (double g, double ceiling, double& loss, double& avg, double& pk)
    {
        if (! lr.valid()) { modelGr (g, ceiling, avg, pk); loss = avg; return; }
        const double x = g - ceiling;                                   // the limiter is scale-free: only the drive over the ceiling matters
        if (x <= lr.x.front()) { loss = avg = pk = 0.0; return; }
        size_t k = 1;
        while (k + 1 < lr.x.size() && lr.x[k] < x) ++k;
        const double t = std::clamp ((x - lr.x[k - 1]) / std::max (1.0e-9, (double) (lr.x[k] - lr.x[k - 1])), 0.0, 1.0);
        auto at = [&] (const std::vector<float>& v) { return (double) v[k - 1] + t * ((double) v[k] - (double) v[k - 1]); };
        loss = at (lr.loss); avg = at (lr.avgGr); pk = at (lr.maxGr);
    };
    auto predict = [&] (double g, double ceiling, double& avg, double& pk)
    {
        if (legacy) { modelGr (g, ceiling, avg, pk); return; }
        double loss; response (g, ceiling, loss, avg, pk);
    };
    auto limiterGainLimit = [&] (double ceiling) { return std::min (ceiling + 6.0 - tp, ceiling + 3.0 - typicalPeak); };

    double desired = silent ? 0.0 : target - reference, cap = 1.0e9, gain = 0.0;
    bool capBinds = false, limiterBinds = false, reachFailed = false;
    double reachedLufs = target;
    if (legacy)
    {
        if (i.target == PlanTarget::KEEP_INPUT) desired = std::min (0.0, desired);
        cap = (! unmeasured && std::abs (m.integrated - target) <= 6.0) ? 3.0 : 8.0;
        capBinds = desired > cap + 1.0e-9;
        gain = std::clamp (desired, -24.0, cap);
        if (! silent)
        {
            const double gLim = limiterGainLimit (i.ceilingDbtp);
            if (gain > gLim + 1.0e-9) { gain = std::max (-24.0, gLim); limiterBinds = true; }
        }
    }
    else if (! silent)
    {
        auto reach = [&] (double g) { double loss, a, k; response (g, i.ceilingDbtp, loss, a, k); return reference + m.stageDeltaDb + g - loss; };
        const double gMax = lr.valid() ? (double) lr.x.back() + i.ceilingDbtp : 36.0;
        double lo = -60.0, hi = gMax;      // out of reach = even the most the limiter can give (table end, or +36 dB) is more than 0.2 LU short
        if (reach (hi) < target - 0.2) { desired = hi; reachFailed = true; reachedLufs = reach (hi); }
        else
        {
            for (int it = 0; it < 80; ++it) { const double mid = 0.5 * (lo + hi); (reach (mid) < target ? lo : hi) = mid; }
            desired = 0.5 * (lo + hi);
        }
        gain = std::clamp (desired, -24.0, gMax);
    }
    // A LEARN that heard under 10 s may be a quiet part only: what it says about the level must not push the loud parts. No lift until the whole song is heard.
    const bool shortLearn = ! silent && m.songSeconds <= 0.0 && m.coverageSeconds > 0.0 && m.coverageSeconds < kShortLearnSeconds;
    if (shortLearn && gain > 0.0) gain = 0.0;
    const double neededGain = gain;                                     // the only level the plan ever asks for; overrides can lower it, never raise it
    s.trimDb = std::clamp (gain, -24.0, 24.0);
    s.limiter.driveDb = gain - s.trimDb;                                // above +24 dB the rest goes through the drive
    p.desiredGainDb = desired;

    p.finishedMaster = done;
    if (done)
    {
        if (m.plr < kDensePlrLu)        p.finishedWhy = "its peaks are only " + juce::String (m.plr, 1) + " dB above the average loudness";
        else if (m.crest < kDenseCrestDb) p.finishedWhy = "its peaks are only " + juce::String (m.crest, 1) + " dB above its average level";
        else                            p.finishedWhy = "its peaks are at " + juce::String (m.samplePeak, 1) + " dBFS, already within 3 dB of the target";
    }

    //---------------------------------------------------------------- explain: level
    if (unmeasured)
        p.explain.push_back ({ "level", 0.0, "Nothing measured yet: the level is left alone until the song is analysed" });
    else if (silent)
        p.explain.push_back ({ "level", 0.0, "The signal is silent (below the -70 LUFS gate): nothing to set" });
    else if (i.target == PlanTarget::KEEP_INPUT)
        p.explain.push_back ({ "level", gain, "Input level kept at " + juce::String (m.integrated, 1) + " LUFS; the master is never made louder than the mix" });
    else if (gain < -0.05)
        p.explain.push_back ({ "level", gain, "Turned down so it sits at " + juce::String (target, 1) + " LUFS (the mix is " + juce::String (m.integrated, 1) + " LUFS)" });
    else if (gain > 0.05)
        p.explain.push_back ({ "level", gain, (legacy ? "Turned up to move towards " : "Turned up to reach ") + juce::String (target, 1) + " LUFS (the mix is " + juce::String (m.integrated, 1) + " LUFS)" });
    else
        p.explain.push_back ({ "level", gain, "Level kept: the mix is already at " + juce::String (m.integrated, 1) + " LUFS" });

    //---------------------------------------------------------------- finished material: level and ceiling only
    if (done)
    {
        p.explain.push_back ({ "finished", gain, "Finished master detected: " + p.finishedWhy + ". Level and ceiling only" });
        if (! m.findings.empty())   // the diagnosis ran: keep its technical readout next to the plain line (never shown in summary())
            p.explain.push_back ({ "detail", m.plr, "PLR " + juce::String (m.plr, 1) + " LU, crest " + juce::String (m.crest, 1) + " dB, peak " + juce::String (m.samplePeak, 1) + " dBFS" });
        if (m.dcOffset > diag::dcDbfs)
            p.warnings.add ("DC offset " + juce::String (m.dcOffset, 0) + " dBFS is present; it is left alone on a finished master");
    }
    else if (i.character != PlanCharacter::OFF && ! unmeasured)
    {
        const auto b = bandsOf (m);
        const bool dense = m.plr < kDensePlrLu || m.crest < kDenseCrestDb || m.lra < kDenseLraLu;
        auto& e = s.eq; auto& c = s.comp; auto& w = s.width;

        // -- compression: how much the mix can take, from how dynamic it is
        double glue = 1.0;
        juce::String why = dense ? "the mix is already dense, so the amount is held down" : "gentle overall compression";
        if (full && ! dense)
        {
            const double excess = m.shortTermMax > -299.0 ? m.shortTermMax - m.integrated : 0.0;
            const double spread = m.loud30Percent != 0.0 ? m.loud30Percent - m.integrated : excess;
            if (m.lra >= 8.0) glue += 0.3;
            if (excess >= 4.0) glue += 0.2;
            if (spread < 0.5) glue -= 0.2;
            if (findingFor (m, scTooDynamic, { "dynamic" }) != nullptr) glue += 0.3;
            glue = std::clamp (glue, 0.4, 1.5);
            why = "compression follows the music: loudness range " + juce::String (m.lra, 1) + " LU, loudest part " + juce::String (excess, 1) + " dB above the average";
            if (m.momentaryMax > -299.0 && m.shortTermMax > -299.0 && m.momentaryMax - m.shortTermMax >= 3.0)
            { c.attackMs = 40.0; why += "; slower attack to let the hits through"; }
        }
        c.enabled = true; c.glueDb = dense ? std::min (glue, kDenseGlueDb) : glue; c.ratio = dense ? kDenseRatio : 2.0;
        p.explain.push_back ({ "compression", c.glueDb, "Gentle compression " + juce::String (c.glueDb, 1) + " dB: " + why });

        // -- AIR
        const bool airDeficit = b.air < diag::dullDb;
        const double deficit = std::max (0.0, diag::dullDb - b.air);
        double air = 0.0;
        if (gentle && airDeficit && m.plr >= kDensePlrLu && m.lra >= 3.0) air = std::min (1.5, 0.5 + 0.25 * deficit);
        if (full)
        {
            const Finding* dullF = findingFor (m, scDull, { "dull", "air" });
            const double d2 = std::max (deficit, severityFloor (dullF, 1.0, 6.0));
            if (airDeficit || dullF != nullptr) air = std::min (3.0, 1.0 + 0.25 * d2);
        }
        if (air > 0.0) { e.airDb = air; p.explain.push_back ({ "air", air, "AIR +" + juce::String (air, 1) + " dB: the top octave sits " + juce::String (-b.air, 0) + " dB under the mids" }); }

        if (full)
        {
            // -- tone, from the band energies and the diagnosis findings (each bounded)
            const Finding* mudF = findingFor (m, scMuddy, { "mud" });
            const double mud = std::max (b.lowMid, severityFloor (mudF, diag::mudDb, diag::mudProblemDb));
            if (mud >= diag::mudDb || mudF != nullptr)
            { e.dynLowMid.enabled = true; e.dynLowMid.amountDb = std::clamp (0.6 * mud, 1.0, 4.0);
              p.explain.push_back ({ "low-mid", -e.dynLowMid.amountDb, "Low-mid build-up (" + juce::String (mud, 1) + " dB over the line): cut " + juce::String (e.dynLowMid.amountDb, 1) + " dB, only while it is loud" }); }
            const Finding* harshF = findingFor (m, scHarsh, { "harsh" });
            const double harsh = std::max (b.presence, severityFloor (harshF, diag::harshDb, diag::harshProblemDb));
            if (harsh >= diag::harshDb || harshF != nullptr)
            { e.dynUpperMid.enabled = true; e.dynUpperMid.amountDb = std::clamp (0.6 * harsh, 1.0, 4.0);
              p.explain.push_back ({ "upper-mid", -e.dynUpperMid.amountDb, "Harsh upper mids (" + juce::String (harsh, 1) + " dB over the line): cut " + juce::String (e.dynUpperMid.amountDb, 1) + " dB, only while it is loud" }); }
            const Finding* boomF = findingFor (m, scBoomy, { "boomy" });
            const double boom = std::max (b.sub, severityFloor (boomF, diag::boomySubDb, diag::boomySubAloneDb));
            if (boom >= diag::boomySubDb || boomF != nullptr)
            { e.tightener.enabled = true; e.tightener.amountDb = std::clamp (boom / 3.0, 1.0, 4.0);
              p.explain.push_back ({ "low end", -e.tightener.amountDb, "Boomy sub (" + juce::String (boom, 1) + " dB over the low mids): tightened by up to " + juce::String (e.tightener.amountDb, 1) + " dB" }); }
            const Finding* thinF = findingFor (m, scThin, { "thin" });
            const double thin = std::max (-b.low, severityFloor (thinF, -diag::thinLowDb, -diag::thinProblemDb));
            if (thin >= -diag::thinLowDb || thinF != nullptr)
            { e.lowShelfDb = std::clamp (0.25 * thin, 0.5, 2.0);
              p.explain.push_back ({ "low shelf", e.lowShelfDb, "Thin low end (" + juce::String (thin, 1) + " dB under the mids): low shelf +" + juce::String (e.lowShelfDb, 1) + " dB" }); }
            const double dip = -b.mid;
            if (dip >= -diag::presenceDipDb)
            { e.bell2Db = std::clamp (0.3 * dip, 0.5, 1.5);
              p.explain.push_back ({ "presence", e.bell2Db, "Vocal presence dips " + juce::String (dip, 1) + " dB: +" + juce::String (e.bell2Db, 1) + " dB around 3 kHz" }); }
            const Finding* sibF = findingFor (m, scSibilance, { "sibilance" });
            const double sib = std::max (b.sibilance, severityFloor (sibF, diag::sibPeakDb, diag::sibPeakDb + 4.0));
            if (sib >= diag::sibPeakDb || sibF != nullptr)
            { e.airDynamic = true; e.airDeHarshDb = std::clamp (sib / 4.0, 2.0, 4.0);
              p.explain.push_back ({ "sibilance", -e.airDeHarshDb, "Hiss and 's' peaks stand out: the top eases off by up to " + juce::String (e.airDeHarshDb, 1) + " dB when they do" }); }
            if (! i.referenceCurve.empty())
            { e.matchCurveDb = i.referenceCurve; e.matchAmount = 0.5; p.explain.push_back ({ "tone match", 0.5, "Tone moved halfway towards the reference" }); }
            e.enabled = true;   // FULL keeps the stages in; neutral ones are skipped by the chain, and an override finds them on

            // -- stereo, from the phase measurements
            const bool phaseF = findingFor (m, scPhase, { "phase" }) != nullptr;
            if (m.lowCorrelation < diag::phaseLowCorr || m.correlation < diag::phaseCorr || phaseF)
            { w.monoBelowHz = 150.0; w.phaseSafeLowMono = true; w.enabled = true;
              p.explain.push_back ({ "stereo", 150.0, "Left and right lows work against each other (match " + juce::String (m.lowCorrelation, 2) + "): lows made mono below 150 Hz, keeping their level" }); }
            if (m.correlation < diag::phaseCorr)
            { w.width = 0.8; w.enabled = true; p.explain.push_back ({ "width", 0.8, "Left and right partly cancel overall: width 0.80" }); }
            else if (findingFor (m, scTooWide, { "wide" }) != nullptr)
            { w.width = 0.9; w.enabled = true; p.explain.push_back ({ "width", 0.9, "The mix is very wide: width 0.90" }); }
            w.enabled = true;
        }
        else
            e.enabled = e.airDb != 0.0;

        // -- DC offset
        if (m.dcOffset > diag::dcDbfs || findingFor (m, scDcOffset, { "dc" }) != nullptr)
        { s.dcFilter = true; p.explain.push_back ({ "dc", m.dcOffset, "DC offset (" + juce::String (m.dcOffset, 0) + " dBFS) removed so it does not eat headroom" }); }
    }
    else if (m.dcOffset > diag::dcDbfs)
        p.warnings.add ("DC offset " + juce::String (m.dcOffset, 0) + " dBFS is present; Gentle or Full would remove it");

    //---------------------------------------------------------------- refusals and warnings
    if (capBinds)
    {
        p.targetUnreachable = true;
        const bool nearTarget = std::abs (m.integrated - target) <= 6.0;
        // Only a legacy rule can land here (KEEP INPUT never asks for more than 0 dB, so in practice never). Chosen targets and ALBUM are reached by the limiter.
        p.refused.add ("Target " + num (target) + " needs +" + juce::String (desired, 1) + " dB; applied +" + num (gain) + " and stopped"
                       + (nearTarget ? ": more would push the peaks into the limiter" : ""));
        p.warnings.add ("targetUnreachable");
    }
    if (reachFailed)
    {
        p.targetUnreachable = true;
        p.refused.add ("Target " + num (target) + " is out of reach even with +" + juce::String (gain, 1) + " dB: the limiter gets it to " + juce::String (reachedLufs, 1) + " LUFS");
        p.warnings.add ("targetUnreachable");
    }
    if (limiterBinds)
    {
        const juce::String t = "Peaks would need more than 6 dB of limiting at the target: level held at " + sgn (gain) + " dB";
        p.warnings.add (t);
        p.explain.push_back ({ "limiter", gain, t });
    }
    if (lowCoverage)
        p.warnings.add ("Heard only " + juce::String (m.coverageSeconds, 0) + " s: solved conservatively" + juce::String (shortLearn ? " (the level is not raised)" : "") + "; play the whole song and LEARN again");
    if (unmeasured)
        p.warnings.add ("Not measured yet: load a file, or press LEARN and play the song");
    else if (silent)
        p.warnings.add ("Silent input: nothing was changed");

    //---------------------------------------------------------------- overrides: applied last, never past the caps, reported as deltas
    struct Applied { juce::String id; double before; Slot slot; };
    std::vector<Applied> applied;
    bool trimOverridden = false, driveOverridden = false, eqOnSet = false, compOnSet = false, widthOnSet = false;
    for (const auto& ov : i.overrides)
    {
        Slot sl;
        if (! slotFor (s, ov.first, sl)) { p.warnings.add ("Unknown override: " + ov.first); continue; }
        if (done && sl.stage != Stage::level) { p.refused.add ("Finished master: " + ov.first + " not applied"); continue; }
        applied.push_back ({ ov.first, sl.get(), sl });
        sl.set (ov.second);
        if (ov.first == "trim") trimOverridden = true;
        if (ov.first == "drive") driveOverridden = true;
        if (sl.isOn) { if (sl.stage == Stage::eq) eqOnSet = true; else if (sl.stage == Stage::comp) compOnSet = true; else widthOnSet = true; }
    }
    // A knob moved by hand switches its stage in (unless the user also set that stage's own switch).
    for (const auto& a : applied)
    {
        if (a.slot.isOn) continue;
        if (a.slot.stage == Stage::eq && ! eqOnSet) s.eq.enabled = true;
        else if (a.slot.stage == Stage::comp && ! compOnSet) s.comp.enabled = true;
        else if (a.slot.stage == Stage::width && ! widthOnSet) s.width.enabled = true;
    }
    {
        // explicit targets: a hand-made level never goes above the gain the target needs; with nothing measured there is no such gain, and the earlier +8 dB limit stays
        double limit = legacy ? std::min (cap, limiterGainLimit (s.limiter.ceilingDbtp)) : (unmeasured ? 8.0 : neededGain);
        if (i.target == PlanTarget::KEEP_INPUT) limit = std::min (limit, 0.0);
        if (silent && ! unmeasured) limit = 0.0;
        const double total = s.trimDb + s.limiter.driveDb;
        if (total > limit + 1.0e-9)
        {
            // The capped total is carried the way the plan carries it (trim up to +-24 dB, the rest in the drive): a hand-made trim of +36 dB
            // must not leave the chain with trim 36 and drive -10.
            const juce::String what = driveOverridden ? "Override drive capped" : trimOverridden ? "Override trim capped" : "Override of the ceiling lowered the level";
            s.trimDb = std::clamp (limit, -24.0, 24.0);
            s.limiter.driveDb = limit - s.trimDb;
            p.refused.add (what + ": total gain is limited to " + sgn (limit) + " dB");
        }
    }
    for (const auto& a : applied)
    {
        const double now = a.slot.get(), d = now - a.before;
        p.explain.push_back ({ "override " + a.id, now, d >= 0.0 ? sgn (d) + " over plan" : juce::String (-d, 1) + " under plan" });
    }

    //---------------------------------------------------------------- the plan as the chain will run it
    p.totalGainDb = s.trimDb + s.limiter.driveDb;
    s.limiter.maxDriveDb = std::max (0.0, s.limiter.driveDb);     // the chain cannot add drive of its own
    predict (p.totalGainDb, s.limiter.ceilingDbtp, p.predictedAverageGrDb, p.predictedPeakGrDb);
    if (silent) p.predictedAverageGrDb = p.predictedPeakGrDb = 0.0;
    if (! legacy && ! silent && ! unmeasured)
    {
        // What reaching the target costs, in plain words (SPEC_V3 section 2 REVISED). The target is reached either way; the colour only says how hard the limiter works.
        const double avg = p.predictedAverageGrDb, pk = p.predictedPeakGrDb;
        p.limiterSeverity = avg > 4.0 ? 2 : avg > 2.0 ? 1 : 0;
        if (pk > 0.05 && ! reachFailed)
        {
            p.limiterStatus = num (target) + " LUFS reached: the limiter takes up to " + juce::String (pk, 1) + " dB on the loudest peaks (avg " + juce::String (avg, 1) + " dB).";
            if (p.limiterSeverity == 2)      p.limiterStatus += " Strong limiting: expect clearly less punch on the drums and a flatter, denser sound.";
            else if (pk >= 1.0)              p.limiterStatus += " Expect less punch on the drums.";
        }
        if (p.totalGainDb > 18.0)
        {
            const juce::String w = "The master is turned up " + juce::String (p.totalGainDb, 1) + " dB: the noise floor of the mix rises with it";
            p.warnings.add (w);
            p.limiterStatus = (p.limiterStatus.isEmpty() ? juce::String() : p.limiterStatus + " ") + w + ".";
            p.limiterSeverity = std::max (p.limiterSeverity, 1);
        }
    }
    if (p.predictedPeakGrDb > 0.05 && i.target != PlanTarget::KEEP_INPUT)
        p.explain.push_back ({ "ceiling", p.predictedPeakGrDb, "Peaks reach the " + juce::String (s.limiter.ceilingDbtp, 1) + " dBTP ceiling: the limiter turns them down by up to " + juce::String (p.predictedPeakGrDb, 1) + " dB" });
    if (i.target == PlanTarget::KEEP_INPUT)
    {
        const double cost = std::max (0.0, tp + p.totalGainDb - s.limiter.ceilingDbtp);
        if (cost > 1.0)
            p.explain.push_back ({ "ceiling", cost, "The ceiling may turn peaks down by up to " + juce::String (cost, 1) + " dB; the input level is preserved" });
    }
    return p;
}

//================================================================================================== ids
const juce::StringArray& overrideIds()
{
    static const juce::StringArray ids {
        "trim", "drive", "limRelease", "lookahead", "balance", "dc",
        "eqOn", "eqLinear", "lowCutOn", "lowCutHz", "lowCutSteep", "lowShelfDb", "lowShelfHz", "tilt", "highShelfDb", "highShelfHz",
        "bell1Db", "bell1Hz", "bell1Q", "bell2Db", "bell2Hz", "bell2Q", "airDb", "airHz", "airDyn", "airDeHarsh",
        "lowMidOn", "lowMidHz", "lowMidAmount", "upperMidOn", "upperMidHz", "upperMidAmount", "tightOn", "tightHz", "tightAmount", "toneAmount",
        "compOn", "compMode", "compAuto", "glue", "threshold", "ratio", "attack", "release", "autoRel", "rms", "scHpf", "mix", "knee", "xoverLow", "xoverHigh",
        "b1Bypass", "b1Solo", "b1Auto", "b1Glue", "b1Threshold", "b1Ratio", "b1Attack", "b1Release", "b1Gain",
        "b2Bypass", "b2Solo", "b2Auto", "b2Glue", "b2Threshold", "b2Ratio", "b2Attack", "b2Release", "b2Gain",
        "b3Bypass", "b3Solo", "b3Auto", "b3Glue", "b3Threshold", "b3Ratio", "b3Attack", "b3Release", "b3Gain",
        "widthOn", "width", "monoBelow", "phaseSafe", "highWidth", "highWidthHz" };
    return ids;
}

double settingValue (const MasterSettings& s, const juce::String& id)
{
    Slot sl;
    auto copy = s;
    return slotFor (copy, id, sl) ? sl.get() : std::numeric_limits<double>::quiet_NaN();
}

bool setSettingValue (MasterSettings& s, const juce::String& id, double v)
{
    Slot sl;
    if (! slotFor (s, id, sl)) return false;
    sl.set (v);
    return true;
}

int overrideStage (const juce::String& id, bool* stageSwitch)
{
    MasterSettings scratch;
    Slot sl;
    if (stageSwitch != nullptr) *stageSwitch = false;
    if (! slotFor (scratch, id, sl)) return 0;
    if (stageSwitch != nullptr) *stageSwitch = sl.isOn;
    return sl.stage == Stage::eq ? 1 : sl.stage == Stage::comp ? 2 : sl.stage == Stage::width ? 3 : 0;
}

bool overrideIsLevel (const juce::String& id) { return id == "trim" || id == "drive" || id == "limRelease" || id == "lookahead" || id == "ceiling"; }

//================================================================================================== summary
juce::String Plan::plainSummary() const
{
    juce::StringArray parts;
    const double t = settings.targetLufs, g = totalGainDb;
    if (std::abs (g) <= 0.05) parts.add ("Level kept at " + juce::String (t, 1) + " LUFS");
    else if (g < 0.0)         parts.add ("Turned down " + juce::String (-g, 1) + " dB so it sits at " + juce::String (t, 1) + " LUFS");
    else                      parts.add ("Turned up " + juce::String (g, 1) + " dB towards " + juce::String (t, 1) + " LUFS");
    parts.add ("ceiling " + juce::String (settings.limiter.ceilingDbtp, 1) + " dBTP ("
               + (predictedPeakGrDb > 0.05 ? "peaks touched " + juce::String (predictedPeakGrDb, 1) + " dB" : juce::String ("peaks stay under it")) + ")");
    int others = 0;
    for (const auto& a : explain)
        if (a.action == "compression" || a.action == "air" || a.action == "low-mid" || a.action == "upper-mid" || a.action == "low end" || a.action == "low shelf"
            || a.action == "presence" || a.action == "sibilance" || a.action == "tone match" || a.action == "stereo" || a.action == "width" || a.action == "dc")
        { parts.add (a.reason); ++others; }
    if (others == 0)
        parts.add (finishedMaster ? "nothing else, because it is already a finished master (" + finishedWhy + ")" : juce::String ("nothing else"));
    if (limiterStatus.isNotEmpty()) parts.add (limiterStatus.trimCharactersAtEnd ("."));
    for (const auto& r : refused) parts.add ("not done: " + r);
    for (const auto& a : explain)
        if (a.action == "limiter") parts.add ("held back: " + a.reason);
    return parts.joinIntoString (" \xc2\xb7 ");
}
juce::String Plan::summary() const { return plainSummary(); }

//================================================================================================== measurements
Measurements measurementsFrom (const Measurement& x, const DiagnosisMeasures* d, double songSeconds)
{
    Measurements m;
    m.integrated = x.integrated; m.shortTermMax = x.shortTermMax; m.momentaryMax = x.momentaryMax; m.lra = x.lra;
    m.samplePeak = x.samplePeak; m.truePeak = x.truePeak; m.crest = x.crest; m.correlation = x.correlation;
    m.spectrum = x.spectrumDb; m.plr = x.samplePeak - x.integrated;

    // Loudest 30 % of the short-term history, power-averaged (gated values left out).
    std::vector<double> st;
    for (auto v : x.shortTermHistory) if (std::isfinite (v) && v > -70.0f) st.push_back (v);
    if (st.empty()) m.loud30Percent = x.shortTermMax;
    else
    {
        std::sort (st.begin(), st.end(), std::greater<double>());
        const size_t n = std::max<size_t> (1, (size_t) std::ceil (0.3 * (double) st.size()));
        double sum = 0.0; for (size_t k = 0; k < n; ++k) sum += std::pow (10.0, st[k] / 10.0);
        m.loud30Percent = 10.0 * std::log10 (sum / (double) n);
    }

    if (d != nullptr)
    {
        m.lowCorrelation = d->lowCorrelation; m.dcOffset = d->dcDbfs;
        m.bands = { d->subDb, d->lowDb, d->mudDb, d->presenceDb, d->harshDb, d->sibPeakDb, d->airRelDb };
        m.bandsValid = true;
        m.findings = diagnose (*d, MasterSettings()).findings;
    }
    m.coverageSeconds = x.seconds;
    m.songSeconds = songSeconds > 0.0 ? songSeconds : x.seconds;   // unknown song length: the measured part is taken as the whole
    return m;
}

//================================================================================================== limiter response
void measureLimiterResponse (const juce::AudioBuffer<float>& in, double fs, Measurements& m)
{
    m.limiter = {};
    const int n = in.getNumSamples();
    if (n < 64 || fs <= 0.0 || ! (m.integrated > -70.0)) return;
    const double tpDb = std::max (m.truePeak, m.samplePeak);
    if (! std::isfinite (tpDb) || tpDb < -150.0) return;
    const auto env = truePeakEnvelope (in, kLimiterOversampling);
    // The loudest 30 % of the 100 ms blocks (by the signal's own energy) is where the average gain reduction is read.
    const int blk = std::max (1, (int) std::lround (0.1 * fs)), nb = (n + blk - 1) / blk;
    std::vector<double> power ((size_t) nb, 0.0);
    for (int c = 0; c < in.getNumChannels(); ++c)
    {
        const float* x = in.getReadPointer (c);
        for (int k = 0; k < n; ++k) power[(size_t) (k / blk)] += (double) x[k] * (double) x[k];
    }
    std::vector<int> order ((size_t) nb);
    for (int k = 0; k < nb; ++k) order[(size_t) k] = k;
    std::sort (order.begin(), order.end(), [&] (int a, int b) { return power[(size_t) a] > power[(size_t) b]; });
    const int loud = std::max (1, (int) std::ceil (0.3 * nb));
    LimiterSettings ls;                                           // default release / look-ahead; the ceiling is 0 dBTP, the table is drive over the ceiling
    ls.ceilingDbtp = 0.0;
    auto& r = m.limiter;
    const double x0 = -tpDb;                                      // the drive at which the highest peak just touches the ceiling
    r.x.push_back ((float) x0); r.loss.push_back (0.0f); r.avgGr.push_back (0.0f); r.maxGr.push_back (0.0f);
    juce::AudioBuffer<float> out;
    std::vector<float> g;
    std::vector<char> isLoud ((size_t) nb, 0);
    for (int j = 0; j < loud; ++j) isLoud[(size_t) order[(size_t) j]] = 1;
    double lastReached = m.integrated + x0, lastOff = 0.0;
    // 1.5 dB steps over the first 6 dB (where the limiter starts to bite and the loss curve bends most), 3 dB after that
    std::vector<double> offsets { 1.5, 3.0, 4.5, 6.0 };
    for (int j = 1; j <= 11; ++j) offsets.push_back (6.0 + 3.0 * j);
    for (size_t k = 0; k < offsets.size(); ++k)
    {
        const double x = x0 + offsets[k];
        applyDriveAndLimit (in, env, fs, ls, x, out, g);
        const double lufs = integratedLoudness (out, fs);
        double mx = 0.0, sum = 0.0; long cnt = 0;
        for (int j = 0; j < n; ++j)
        {
            const double gr = g[(size_t) j] >= 1.0f ? 0.0 : -20.0 * std::log10 (std::max (1.0e-9f, g[(size_t) j]));
            mx = std::max (mx, gr);
            if (isLoud[(size_t) (j / blk)]) { sum += gr; ++cnt; }
        }
        // loss: what trim alone (integrated + x) would give, minus what the limiter really leaves
        r.x.push_back ((float) x);
        r.loss.push_back ((float) std::max (0.0, m.integrated + x - lufs));
        r.avgGr.push_back ((float) (cnt > 0 ? sum / (double) cnt : 0.0));
        r.maxGr.push_back ((float) mx);
        const double reached = m.integrated + x - r.loss.back();
        if ((reached - lastReached) * 3.0 / (offsets[k] - lastOff) < 0.2 && k >= 2) break;   // 3 dB more drive buys < 0.2 LU: the limiter has flattened the loudness curve
        lastReached = reached; lastOff = offsets[k];
        if (reached > -1.0) break;       // past -1 LUFS at a 0 dBTP ceiling: no target (<= -4 LUFS, ceiling >= -3 dBTP) lives above it
    }
}
}
