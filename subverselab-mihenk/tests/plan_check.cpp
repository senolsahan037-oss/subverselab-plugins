// plan_check: the acceptance test of the pure plan solver (Source/engine/Plan.{h,cpp}).
// Run from the project root (it reads tests/corpus/*.wav; pass another corpus folder as argv[1]).
// Every check prints PASS/FAIL with the numbers it saw and the process exits non-zero on any FAIL.
#include <JuceHeader.h>
#include "../Source/engine/Plan.h"
#include "../Source/engine/AudioIO.h"
#include "../Source/engine/Preset.h"
#include "../Source/engine/Batch.h"
#include <cmath>
#include <cstdio>
#include <functional>
using namespace mihenk;

static int passes = 0, fails = 0;
static void check (bool ok, const juce::String& name, const juce::String& detail = {})
{
    std::printf ("%s  %s%s%s\n", ok ? "PASS" : "FAIL", name.toRawUTF8(), detail.isEmpty() ? "" : "  | ", detail.toRawUTF8());
    ok ? ++passes : ++fails;
}

// An open (unfinished) mix: the peak sits `plr` above the loudness, so the finished rule does not mask what is under test.
static Measurements M (double l, double plr = 14.0, double crest = 12.0, double songS = 180.0, double covS = 180.0)
{
    Measurements x; x.integrated = l; x.samplePeak = l + plr; x.truePeak = x.samplePeak; x.plr = plr; x.crest = crest;
    x.shortTermMax = l + 2; x.momentaryMax = l + 4; x.lra = 6; x.loud30Percent = l + 1.5; x.coverageSeconds = covS; x.songSeconds = songS;
    return x;
}
static double targetOf (const Measurements& m, const Intent& i)
{
    switch (i.target)
    {
        case PlanTarget::STREAM_14: return -14; case PlanTarget::APPLE_16: return -16; case PlanTarget::CLUB_8: return -8;
        case PlanTarget::KEEP_INPUT: return m.integrated; case PlanTarget::ALBUM: return i.albumTarget + i.relativeDb;
        default: return i.customLufs;
    }
}
static bool finishedRule (const Measurements& m, double t)
{ return m.plr < 11.0 || m.crest < 9.0 || (m.samplePeak > -3.0 && std::abs (m.integrated - t) <= 3.0); }
static double gainOf (const Plan& p) { return p.settings.trimDb + p.settings.limiter.driveDb; }

// Whole plan as text: every setting (settingsToVar) plus the chain-control flags, the numbers, and every explain/refused/warning string.
static juce::String fingerprint (const Plan& p)
{
    juce::String s = juce::JSON::toString (settingsToVar (p.settings), true);
    s << "|tl" << (int) p.settings.targetLock << "|at" << (int) p.settings.autoTrim << "|md" << p.settings.limiter.maxDriveDb << "|ce" << p.settings.limiter.ceilingDbtp
      << "|dyn" << (int) p.settings.eq.dynLowMid.enabled << (int) p.settings.eq.dynUpperMid.enabled << (int) p.settings.eq.tightener.enabled << (int) p.settings.eq.airDynamic
      << "|mc" << (int) p.settings.eq.matchCurveDb.size() << "|ma" << p.settings.eq.matchAmount << "|ps" << (int) p.settings.width.phaseSafeLowMono
      << "|G" << juce::String (p.totalGainDb, 9) << "|ag" << p.predictedAverageGrDb << "|pg" << p.predictedPeakGrDb << "|U" << (int) p.targetUnreachable << "|F" << (int) p.finishedMaster;
    for (auto& a : p.explain) s << "|E:" << a.action << "=" << juce::String (a.value, 9) << ":" << a.reason;
    for (auto& r : p.refused) s << "|R:" << r;
    for (auto& w : p.warnings) s << "|W:" << w;
    return s;
}
static bool stereoNeutral (const MasterSettings& s)
{ return ! s.width.enabled && s.width.width == 1.0 && s.width.highWidth == 1.0 && s.width.monoBelowHz == 0.0 && ! s.width.phaseSafeLowMono; }
static bool levelOnly (const MasterSettings& s)
{
    return ! s.eq.enabled && ! s.comp.enabled && stereoNeutral (s) && s.eq.matchCurveDb.empty() && s.eq.matchAmount == 0.0 && s.eq.airDb == 0 && s.eq.bell1Db == 0 && s.eq.bell2Db == 0
           && s.eq.tiltDbPerOct == 0 && s.eq.lowShelfDb == 0 && s.eq.highShelfDb == 0 && ! s.eq.lowCutOn && ! s.eq.dynLowMid.enabled && ! s.eq.dynUpperMid.enabled
           && ! s.eq.tightener.enabled && ! s.eq.airDynamic;
}
static juce::String allText (const Plan& p, bool withDetail)
{
    juce::String t = p.summary() + " ## ";
    for (auto& a : p.explain) if (withDetail || a.action != "detail") t << a.action << " " << a.reason << " ## ";
    for (auto& r : p.refused) t << r << " ## ";
    return t;
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::File corpus (argc > 1 ? argv[1] : "tests/corpus");
    const PlanTarget targets[] = { PlanTarget::STREAM_14, PlanTarget::APPLE_16, PlanTarget::CLUB_8, PlanTarget::KEEP_INPUT, PlanTarget::ALBUM, PlanTarget::CUSTOM };

    // ---------------------------------------------------------------- 1. idempotence: the whole plan, 108 cases
    {
        int n = 0, bad = 0; size_t minLen = 100000;
        for (double l : { -30.0, -23.2, -16.0, -12.0, -9.0, -6.0 }) for (int ch = 0; ch < 3; ++ch) for (int t = 0; t < 6; ++t)
        {
            auto m = M (l, 15, 13); m.songSeconds = 169; m.coverageSeconds = (t == 2 ? 30 : 169);
            Intent q; q.target = targets[t]; q.character = (PlanCharacter) ch; q.relativeDb = -2; q.customLufs = -11; q.ceilingDbtp = -0.5;
            q.overrides["bell1Db"] = 1.2; q.overrides["airDb"] = 2.0;
            const auto a = fingerprint (solvePlan (m, q)), b = fingerprint (solvePlan (m, q));
            ++n; if (a != b) ++bad; minLen = std::min ((size_t) minLen, (size_t) a.length());
        }
        check (bad == 0 && minLen > 1000, "1 idempotence: whole plan (settings, explain, refused, warnings) identical on repeat", juce::String (n) + " cases, differences " + juce::String (bad) + ", shortest fingerprint " + juce::String ((int) minLen) + " chars");
    }

    // ---------------------------------------------------------------- 2. order independence: six real assignment orders (with wrong values written first)
    {
        int n = 0, bad = 0;
        for (double l : { -30.0, -16.0, -12.0, -6.0 }) for (int ch = 0; ch < 3; ++ch) for (int t = 0; t < 6; ++t)
        {
            auto m = M (l, 15, 13);
            const PlanTarget T = targets[t]; const PlanCharacter C = (PlanCharacter) ch;
            const std::vector<float> ref (100, 2.0f);
            auto fill = [&] (Intent& q, int k)
            {
                std::function<void()> steps[6] = { [&] { q.target = T; }, [&] { q.character = C; }, [&] { q.ceilingDbtp = -0.5; },
                                                   [&] { q.relativeDb = -2.0; q.albumTarget = -13.0; q.customLufs = -11.0; }, [&] { q.overrides["bell1Db"] = 1.2; q.overrides["airDb"] = 2.0; },
                                                   [&] { q.referenceCurve = ref; q.referenceLoudness = -9.0; } };
                static const int orders[6][6] = { { 0,1,2,3,4,5 }, { 5,4,3,2,1,0 }, { 2,0,4,1,5,3 }, { 3,5,1,0,2,4 }, { 1,3,5,4,0,2 }, { 4,2,0,5,3,1 } };
                // decoys: write the wrong values first, then the real ones in this order
                q.target = PlanTarget::CUSTOM; q.character = PlanCharacter::FULL; q.ceilingDbtp = -6.0; q.customLufs = -20.0; q.overrides["trim"] = 9.0;
                q.overrides.clear(); q.referenceCurve.clear();
                for (int s = 0; s < 6; ++s) steps[orders[k][s]]();
            };
            Intent ref0; fill (ref0, 0); const auto want = fingerprint (solvePlan (m, ref0));
            for (int k = 1; k < 6; ++k) { Intent q; fill (q, k); ++n; if (fingerprint (solvePlan (m, q)) != want) ++bad; }
        }
        check (bad == 0, "2 order independence: six assignment orders give the identical plan", juce::String (n) + " intents compared, differences " + juce::String (bad));
        // click history: solving other intents in between changes nothing
        auto m = M (-18, 14, 13); Intent q; q.character = PlanCharacter::FULL; const auto base = fingerprint (solvePlan (m, q));
        for (int k = 0; k < 20; ++k) { Intent z; z.target = targets[k % 6]; z.character = (PlanCharacter) (k % 3); (void) solvePlan (m, z); }
        check (fingerprint (solvePlan (m, q)) == base, "2 plan unchanged after 20 intervening solves (no hidden state)");
    }

    // ---------------------------------------------------------------- 3. caps: target x character x overrides sweep
    {
        long expN = 0, expRefMismatch = 0, expOver = 0, expMiss = 0, sevBad = 0; long n = 0, capViol = 0, refMismatch = 0, unrMismatch = 0, grBound = 0, grUnder = 0, drift = 0, lockOn = 0, ovN = 0, ovViol = 0, ovUnreported = 0, textBad = 0;
        juce::String firstRef, firstCap;
        for (double l = -30; l <= -4; l += 1.0) for (double plr = 6; plr <= 20; plr += 2) for (double crest = 5; crest <= 18; crest += 3)
        for (int ti = 0; ti < 6; ++ti) for (int ch = 0; ch < 3; ++ch) for (int ovk = 0; ovk < 4; ++ovk)
        {
            Intent q; q.target = targets[ti]; q.character = (PlanCharacter) ch; q.albumTarget = -14; q.relativeDb = (ch == 1 ? -2.0 : 3.0); q.customLufs = -10;
            if (ovk == 1) q.overrides["drive"] = 20.0;
            if (ovk == 2) q.overrides["trim"] = 20.0;
            if (ovk == 3) { q.overrides["drive"] = 12.0; q.overrides["trim"] = 6.0; }
            const auto m = M (l, plr, crest); const auto p = solvePlan (m, q); ++n;
            const double target = targetOf (m, q);
            const bool legacyT = q.target == PlanTarget::KEEP_INPUT;   // SPEC_V3 2 REVISED: only KEEP INPUT keeps the +3 / +8 rule; explicit targets AND ALBUM are reached (album v3.2, 2026-10-03)
            const double cap = std::abs (m.integrated - target) <= 6.0 ? 3.0 : 8.0;
            const double g = gainOf (p);
            if (p.settings.targetLock || p.settings.autoTrim) ++lockOn;
            if (ovk == 0 && legacyT)
            {
                double desired = target - m.integrated; if (q.target == PlanTarget::KEEP_INPUT) desired = std::min (0.0, desired);
                const bool binds = desired > cap + 1e-9;
                if (g > cap + 1e-9) { ++capViol; if (firstCap.isEmpty()) firstCap << "int " << l << " plr " << plr << " t" << ti << " ch" << ch << " gain " << g; }
                if (binds != ! p.refused.isEmpty()) { ++refMismatch; if (firstRef.isEmpty()) firstRef << "int " << l << " plr " << plr << " t" << ti << " desired " << desired << " cap " << cap << " refused " << p.refused.size(); }
                if (binds != p.targetUnreachable) ++unrMismatch;
                if (std::abs (g - p.totalGainDb) > 1e-9) ++drift;
                const double needPk = std::max (0.0, std::max (m.samplePeak, m.truePeak) + g - q.ceilingDbtp);
                if (p.predictedPeakGrDb > 6.0 + 1e-9 || p.predictedAverageGrDb > 2.0 + 1e-9) ++grBound;
                if (p.predictedPeakGrDb + 1e-9 < needPk) ++grUnder;
                if (! p.refused.isEmpty() && ! p.refused[0].contains ("dB")) ++textBad;
            }
            else if (ovk == 0)
            {
                // explicit target: the plan reaches it; refused / unreachable only when even the limiter cannot get there
                ++expN;
                if (p.refused.isEmpty() == p.targetUnreachable) ++expRefMismatch;
                if (g > p.desiredGainDb + 0.2 + 1e-9 && p.desiredGainDb > -24.0) ++expOver;
                if (std::abs (g - p.totalGainDb) > 1e-9) ++drift;
                const double needPk = std::max (0.0, std::max (m.samplePeak, m.truePeak) + g - q.ceilingDbtp);
                if (p.predictedPeakGrDb + 1e-9 < needPk) ++grUnder;
                const double ceilingReach = m.integrated + g - p.predictedAverageGrDb;     // the model's own loudness after the limiter
                if (! p.targetUnreachable && std::abs (ceilingReach - target) > 0.2 && g > -23.9) ++expMiss;
                if (p.limiterSeverity != (p.predictedAverageGrDb > 4.0 ? 2 : p.predictedAverageGrDb > 2.0 ? 1 : 0) && p.totalGainDb <= 18.0) ++sevBad;
            }
            else
            {
                ++ovN;
                Intent q0 = q; q0.overrides.clear();
                const double limit = legacyT ? cap : gainOf (solvePlan (m, q0));
                if (g > limit + 1e-9) ++ovViol;
                bool reported = false; for (auto& e : p.explain) if (e.action.startsWith ("override")) reported = true;
                if (! reported) ++ovUnreported;
            }
        }
        check (capViol == 0, "3 KEEP INPUT: total gain (trim + drive) never above +3 / +8", juce::String (n) + " plans, violations " + juce::String (capViol) + " " + firstCap);
        check (refMismatch == 0 && unrMismatch == 0, "3 KEEP INPUT: refused / targetUnreachable set exactly when the gain cap binds", "refused mismatches " + juce::String (refMismatch) + ", flag mismatches " + juce::String (unrMismatch) + " " + firstRef);
        check (grBound == 0, "3 KEEP INPUT: predicted limiter GR within 2 dB average / 6 dB peak", "violations " + juce::String (grBound));
        check (expN > 0 && expRefMismatch == 0, "3 REVISED explicit targets + ALBUM: refused exactly when unreachable, never 'raise the mix'", juce::String (expN) + " plans, mismatches " + juce::String (expRefMismatch));
        check (expOver == 0, "3 REVISED explicit targets + ALBUM: applied gain <= needed gain + 0.2 dB", "violations " + juce::String (expOver));
        check (expMiss == 0, "3 REVISED explicit targets + ALBUM: the model's loudness after the limiter lands on the target within 0.2 LU", "misses " + juce::String (expMiss));
        check (sevBad == 0, "3 REVISED limiter severity follows average GR (<= 2 fine, 2..4 warn, > 4 strong)", "mismatches " + juce::String (sevBad));
        check (grUnder == 0, "3 predicted peak GR never below the independent estimate (peak + gain - ceiling)", "plans under-reporting " + juce::String (grUnder));
        check (drift == 0, "3 totalGainDb equals settings.trimDb + limiter.driveDb", "mismatches " + juce::String (drift));
        check (lockOn == 0, "3 plan never leaves TARGET LOCK / headroom assistant on (the chain applies exactly the plan)", "plans with either on " + juce::String (lockOn));
        check (textBad == 0, "3 refused text states the dB needed", "entries without 'dB' " + juce::String (textBad));
        check (ovViol == 0, "3 overrides never lift the total above the plan's needed gain (explicit) / the cap (KEEP INPUT)", juce::String (ovN) + " override plans, over " + juce::String (ovViol));
        check (ovUnreported == 0, "3 every honoured override is reported in explain", "unreported " + juce::String (ovUnreported));
        const auto p = solvePlan (M (-23.2), Intent{});
        check (p.refused.isEmpty() && ! p.targetUnreachable && gainOf (p) > 9.0 && p.desiredGainDb > 9.0, "3 REVISED: -23.2 LUFS to STREAM -14 is reached (no +8 stop, nothing refused), gain " + juce::String (gainOf (p), 2), p.limiterStatus);
        check (p.limiterStatus.contains ("-14 LUFS reached: the limiter takes up to") && p.limiterStatus.contains ("avg"), "3 REVISED status says what the limiter costs", p.limiterStatus);
        const auto q = solvePlan (M (-13.0), Intent{});
        check (q.refused.isEmpty() && ! q.targetUnreachable, "3 nothing refused when the target is reachable");
    }

    // ---------------------------------------------------------------- 4. finished-material rule
    {
        long n = 0, bad = 0, noWhy = 0; juce::String first;
        struct C { const char* name; Measurements m; };
        const C cs[] = { { "plr 9.5", M (-14, 9.5, 12) }, { "crest 8", M (-14, 14, 8) }, { "peak -2 near target", M (-15, 13, 12) } };
        for (auto& c : cs) for (int ti : { 0, 1, 2, 3 }) for (int ch = 0; ch < 3; ++ch)
        {
            Intent q; q.target = targets[ti]; q.character = (PlanCharacter) ch; q.referenceCurve.assign (100, 3.0f); q.referenceLoudness = -9;
            Finding f; f.id = "mud"; f.scenario = scMuddy; f.severity = Severity::problem;
            auto m = c.m; m.findings.push_back (f); m.spectrum.assign (100, -50.f); m.dcOffset = -20; m.lowCorrelation = -0.5; m.correlation = -0.5;
            const auto p = solvePlan (m, q); ++n;
            if (! finishedRule (m, targetOf (m, q))) continue;
            if (! levelOnly (p.settings) || p.settings.dcFilter) { ++bad; if (first.isEmpty()) first << c.name << " t" << ti << " ch" << ch; }
            if (! p.finishedMaster || ! allText (p, true).contains ("Finished master detected")) ++noWhy;
        }
        check (bad == 0, "4 finished material: EQ, compression, stereo, tone match, dynamic bands, DC all off (3 triggers x 4 targets x 3 characters)", juce::String (n) + " runs, non-neutral " + juce::String (bad) + " " + first);
        check (noWhy == 0, "4 finished plan says 'Finished master detected'", "missing " + juce::String (noWhy));
        struct B { const char* n; Measurements m; bool fin; };
        const B bs[] = { { "plr 10.99", M (-20, 10.99, 12), true }, { "plr 11.0", M (-20, 11.0, 12), false }, { "crest 8.99", M (-20, 14, 8.99), true }, { "crest 9.0", M (-20, 14, 9.0), false },
                         { "peak -3.0, int -15", M (-15, 12.0, 12), false }, { "peak -2.99, int -15", M (-15, 12.01, 12), true }, { "peak -2, int -17.01", M (-17.01, 15.01, 12), false }, { "peak -2, int -17.0", M (-17.0, 15.0, 12), true } };
        int bb = 0; for (auto& b : bs) { Intent q; q.character = PlanCharacter::FULL; const auto p = solvePlan (b.m, q); if (p.finishedMaster != b.fin || (p.finishedMaster && ! levelOnly (p.settings))) ++bb; }
        check (bb == 0, "4 finished rule boundaries (8 edge cases)", "wrong " + juce::String (bb));
    }

    // ---------------------------------------------------------------- 5. characters follow the measurements
    {
        const auto open = M (-18, 14, 13);
        Intent o, g, f; o.character = PlanCharacter::OFF; g.character = PlanCharacter::GENTLE; f.character = PlanCharacter::FULL;
        const auto po = solvePlan (open, o), pg = solvePlan (open, g), pf = solvePlan (open, f);
        check (levelOnly (po.settings) && po.settings.limiter.enabled && ! po.settings.dcFilter, "5 OFF: trim + limiter only on open material");
        check (pg.settings.comp.enabled && pg.settings.comp.glueDb <= 1.0 + 1e-9 && pg.settings.eq.airDb <= 1.5 + 1e-9, "5 GENTLE: compression <= 1 dB, AIR <= 1.5 dB", "glue " + juce::String (pg.settings.comp.glueDb) + " air " + juce::String (pg.settings.eq.airDb));
        auto airRich = open; airRich.spectrum.assign (100, -40.f);
        auto airPoor = open; airPoor.spectrum.assign (100, -40.f); for (int k = 80; k < 100; ++k) airPoor.spectrum[k] = -80.f;
        const double a1 = solvePlan (airRich, g).settings.eq.airDb, a2 = solvePlan (airPoor, g).settings.eq.airDb;
        check (a1 == 0.0 && a2 > 0.0 && a2 <= 1.5, "5 GENTLE adds AIR only when the spectrum is air-deficient", "air-rich " + juce::String (a1) + ", air-deficit " + juce::String (a2));
        auto dense = M (-18, 12, 10); dense.lra = 3;
        const auto pd = solvePlan (dense, f);
        check (pd.settings.comp.glueDb <= 0.5 + 1e-9 && pd.settings.comp.ratio <= 1.5 + 1e-9 && pf.settings.comp.glueDb > pd.settings.comp.glueDb, "5 FULL glue follows the measurements: dense input held at 0.5 dB / 1.5:1, open input more", "open " + juce::String (pf.settings.comp.glueDb) + " dense " + juce::String (pd.settings.comp.glueDb));
        check (solvePlan (airPoor, f).settings.eq.airDb >= solvePlan (airPoor, g).settings.eq.airDb, "5 FULL AIR is at least GENTLE's");
        auto changes = [&] (const char* what, const std::function<void (Measurements&)>& edit) { auto m = open; edit (m); const bool c = fingerprint (solvePlan (m, f)) != fingerprint (solvePlan (open, f)); check (c, juce::String ("5 FULL plan reacts to ") + what); };
        changes ("shortTermMax", [] (Measurements& m) { m.shortTermMax = -2; });
        changes ("momentaryMax", [] (Measurements& m) { m.momentaryMax = 0; });
        changes ("lra", [] (Measurements& m) { m.lra = 1; });
        changes ("loud30Percent", [] (Measurements& m) { m.loud30Percent = -40; });
        changes ("correlation", [] (Measurements& m) { m.correlation = -0.5; });
        changes ("lowCorrelation (phase-safe mono below)", [] (Measurements& m) { m.lowCorrelation = -0.5; });
        changes ("dcOffset (DC filter)", [] (Measurements& m) { m.dcOffset = -20; });
        changes ("spectrum", [] (Measurements& m) { m.spectrum.assign (100, -80.f); for (int k = 0; k < 20; ++k) m.spectrum[k] = -30.f; });
        changes ("findings", [] (Measurements& m) { Finding x; x.id = "harsh"; x.scenario = scHarsh; x.severity = Severity::problem; m.findings.push_back (x); });
        { auto m = open; m.lowCorrelation = -0.5; const auto p = solvePlan (m, f); check (p.settings.width.phaseSafeLowMono && p.settings.width.monoBelowHz > 0, "5 low-band anti-phase gives phase-safe mono below 150 Hz"); }
        { auto m = open; m.dcOffset = -20; check (solvePlan (m, f).settings.dcFilter && solvePlan (m, g).settings.dcFilter && ! solvePlan (m, o).settings.dcFilter, "5 DC offset switches the DC filter on for GENTLE / FULL, not OFF"); }
        { Intent z = f; z.referenceCurve.assign (100, 4.0f); z.referenceLoudness = -9; const auto p = solvePlan (open, z); check (! p.settings.eq.matchCurveDb.empty() && p.settings.eq.matchAmount > 0, "5 reference curve gives tone match in FULL"); }
        { auto m = M (-18, 14, 13, 169, 30); Intent z; auto m2 = m; m2.shortTermMax = -3;
          check (solvePlan (m, z).warnings.joinIntoString ("").contains ("Heard only 30") && fingerprint (solvePlan (m, z)) != fingerprint (solvePlan (m2, z)), "5 low coverage: warning and level solved on short-term max"); }
    }

    // ---------------------------------------------------------------- 6. KEEP_INPUT and ALBUM
    {
        int keepBad = 0, unexplained = 0, albumBad = 0, n = 0;
        for (double l = -30; l <= -4; l += 1) for (double plr : { 8.0, 14.0, 20.0 })
        {
            Intent k; k.target = PlanTarget::KEEP_INPUT; const auto m = M (l, plr, 13); const auto p = solvePlan (m, k); ++n;
            if (gainOf (p) > 1e-9) ++keepBad;
            const double cost = std::max (0.0, m.samplePeak + gainOf (p) - k.ceilingDbtp);
            if (cost > 1.0) { bool said = false; for (auto& e : p.explain) if (e.reason.containsIgnoreCase ("ceiling")) said = true; if (! said) ++unexplained; }
            Intent a; a.target = PlanTarget::ALBUM; a.albumTarget = -13.3; a.relativeDb = -1.7; const auto pa = solvePlan (m, a);
            // album v3.2 (SPEC_V3 2 REVISED): ALBUM is reached like any chosen target: no +3/+8 cap, refused only when even the limiter cannot get there,
            // and the model's loudness after the limiter lands on album target + offset within 0.2 LU. Same gain as the equal CUSTOM target.
            Intent cu; cu.target = PlanTarget::CUSTOM; cu.customLufs = -15.0; const auto pc = solvePlan (m, cu);
            const double reached = m.integrated + gainOf (pa) - pa.predictedAverageGrDb;
            if (std::abs (pa.settings.targetLufs + 15.0) > 1e-9 || std::abs (gainOf (pa) - gainOf (pc)) > 1e-9
                || (! pa.targetUnreachable && gainOf (pa) > -23.9 && std::abs (reached + 15.0) > 0.2) || (pa.targetUnreachable != pa.refused.size() > 0)) ++albumBad;
        }
        check (keepBad == 0, "6 KEEP_INPUT never raises the level", juce::String (n) + " cases, raised " + juce::String (keepBad));
        check (unexplained == 0, "6 KEEP_INPUT explains a ceiling cost above 1 dB", "unexplained " + juce::String (unexplained));
        check (albumBad == 0, "6 ALBUM (album v3.2): target = album target + relative offset, reached within 0.2 LU like a chosen target, same gain as CUSTOM at that value, no +3/+8 cap", "bad " + juce::String (albumBad));
    }

    // ---------------------------------------------------------------- 7. overrides
    {
        const auto m12 = M (-12, 12, 12);   // peak 0 dBFS: realistic headroom
        const auto base = solvePlan (m12, Intent{});
        check (base.refused.isEmpty() && std::abs (gainOf (base) - (-2.0)) < 1e-9, "7 baseline at -12 LUFS: reachable, nothing refused, gain -2 dB", "gain " + juce::String (gainOf (base), 2));
        Intent o; o.overrides["drive"] = 20; const auto p = solvePlan (m12, o);
        check (std::abs (gainOf (p) - gainOf (base)) < 1e-9 && ! p.refused.isEmpty(), "7 REVISED: drive +20 override is cut back to the gain the target needs (-2 dB here) and reported", "total " + juce::String (gainOf (p), 2) + " refused: " + p.refused.joinIntoString (" // "));
        Intent t; t.overrides["trim"] = 20; const auto pt = solvePlan (m12, t);
        check (std::abs (gainOf (pt) - gainOf (base)) < 1e-9 && ! pt.refused.isEmpty(), "7 REVISED: trim +20 override is cut to the needed gain and reported", "total " + juce::String (gainOf (pt), 2));
        Intent q; q.overrides["drive"] = 20; { const auto pq = solvePlan (M (-30), q); check (gainOf (pq) <= pq.desiredGainDb + 0.2 && gainOf (pq) < 20.0, "7 REVISED: drive +20 at -30 LUFS is cut to what STREAM -14 needs (never above needed + 0.2 dB)", "total " + juce::String (gainOf (pq), 2) + " needed " + juce::String (pq.desiredGainDb, 2)); }
        // bell1Db changes nothing else
        auto m = M (-18, 14, 13); Intent b; b.character = PlanCharacter::FULL; const auto p0 = solvePlan (m, b); b.overrides["bell1Db"] = 2.0; auto pb = solvePlan (m, b);
        auto a = pb, c = p0; a.settings.eq.bell1Db = c.settings.eq.bell1Db; a.explain.clear(); c.explain.clear();
        check (juce::JSON::toString (settingsToVar (a.settings)) == juce::JSON::toString (settingsToVar (c.settings)) && pb.settings.eq.bell1Db == 2.0 && pb.totalGainDb == p0.totalGainDb && pb.refused == p0.refused,
               "7 bell1Db override changes bell1Db and nothing else of the plan");
        bool reported = false; for (auto& e : pb.explain) if (e.action == "override bell1Db" && e.reason.contains ("over plan")) reported = true;
        check (reported, "7 override reported as a delta over the plan");
        Intent tl; tl.overrides["tilt"] = 3; tl.overrides["nonsense"] = 1; const auto ptl = solvePlan (m, tl);
        check (ptl.settings.eq.tiltDbPerOct == 3.0 && ptl.warnings.joinIntoString ("").contains ("Unknown override: nonsense"), "7 any known parameter id is honoured, an unknown one is warned about");
        Intent fo; fo.overrides["airDb"] = 4; fo.overrides["glueDb"] = 3; fo.overrides["drive"] = 0; const auto pfin = solvePlan (M (-14, 9.5, 12), fo);
        check (pfin.settings.eq.airDb == 0 && ! pfin.settings.comp.enabled && pfin.refused.joinIntoString ("").contains ("Finished master: airDb not applied"), "7 overrides cannot re-enable stages on finished material");
    }

    // ---------------------------------------------------------------- 8. plain words
    {
        // the -3 dBFS case, rendered from the real file measurement below; here a synthetic version first
        auto m = M (-12.8, 9.8, 10.5); m.samplePeak = -3.0;
        const auto p = solvePlan (m, Intent{});
        const auto s = p.summary(), all = allText (p, false);
        for (auto j : { "glue", "PLR", "LRA", "crest", "sidechain" }) check (! all.containsIgnoreCase (j), juce::String ("8 no '") + j + "' in the summary or the plan text (-3 dBFS case)");
        check (s.contains ("dB") && s.containsIgnoreCase ("ceiling"), "8 summary has dB and ceiling", s);
        check (s.containsIgnoreCase ("nothing else") && s.containsIgnoreCase ("finished"), "8 summary says why and 'nothing else'", s);
        const auto q = solvePlan (M (-23.2), Intent{});
        check (q.summary().contains ("-14 LUFS reached") && q.summary().contains ("limiter takes up to") && ! q.summary().containsIgnoreCase ("raise the mix"), "8 REVISED summary says the target is reached and what the limiter costs", q.summary());
        { Intent a; a.target = PlanTarget::ALBUM; a.albumTarget = -13.3; a.relativeDb = -1.7; const auto pa = solvePlan (M (-23.2), a);
          check (pa.refused.isEmpty() && ! pa.targetUnreachable && std::abs (gainOf (pa) - 8.2) < 0.05 && ! pa.summary().containsIgnoreCase ("not done") && ! pa.summary().containsIgnoreCase ("raise the mix"), "8 ALBUM (album v3.2): -23.2 to album -15 needs +8.2 dB, reached (peaks stay under the ceiling, so no limiter cost line), nothing refused", pa.summary()); }
    }

    // ---------------------------------------------------------------- 9. real corpus, measured per file with the engine
    std::printf ("\n--- corpus %s ---\n", corpus.getFullPathName().toRawUTF8());
    {
        juce::Array<juce::File> files; corpus.findChildFiles (files, juce::File::findFiles, false, "*.wav"); files.sort();
        check (files.size() == 18, "9 corpus has 18 files", juce::String (files.size()));
        int checked = 0;
        for (auto& f : files)
        {
            auto la = loadAudioFile (f);
            if (! la.ok()) { check (false, "9 load " + f.getFileName(), la.error); continue; }
            const auto me = analyse (la.buffer, la.sampleRate, true);
            const auto dm = measureMix (la.buffer, la.sampleRate);
            auto m = measurementsFrom (me, &dm);
            const auto name = f.getFileNameWithoutExtension();
            if (name.endsWith ("verse30s")) m.songSeconds = 169.0;     // 30 s heard of a 169 s song
            Intent q; q.target = PlanTarget::STREAM_14; q.character = PlanCharacter::OFF;
            const auto p = solvePlan (m, q);
            const double g = gainOf (p);
            const juce::String nums = "int " + juce::String (me.integrated, 2) + " peak " + juce::String (me.samplePeak, 2) + " gain " + juce::String (g, 2) + " predGR " + juce::String (p.predictedAverageGrDb, 2) + "/" + juce::String (p.predictedPeakGrDb, 2);
            if (name.endsWith ("peak-3"))
            {
                check (levelOnly (p.settings) && g <= 1e-9 && p.predictedAverageGrDb == 0.0 && p.predictedPeakGrDb == 0.0 && ! p.targetUnreachable, "9 " + name + ": level only, turned down, limiter predicted idle", nums); ++checked;
                const auto s = p.summary(), all = allText (p, false);
                bool jargon = false; for (auto j : { "glue", "PLR", "LRA", "crest", "sidechain" }) if (all.containsIgnoreCase (j)) jargon = true;
                check (! jargon && s.contains ("dB") && s.containsIgnoreCase ("ceiling"), "9 " + name + ": plain summary (no jargon, has dB and ceiling)", s);
            }
            else if (name.endsWith ("brick+6")) { check (std::abs (g + 9.0) < 1.5 && levelOnly (p.settings), "9 " + name + ": about -9 dB, no EQ / compression", nums); ++checked; }
            else if (name.endsWith ("quiet-20")) { check (g > 14.0 && ! p.targetUnreachable && p.refused.isEmpty(), "9 REVISED " + name + ": STREAM -14 is reached (about +15 dB), nothing refused, no 'raise the mix'", nums); ++checked; }
            else if (name.endsWith ("verse30s")) { check (p.warnings.joinIntoString ("").contains ("Heard only 30"), "9 " + name + ": low-coverage warning", p.warnings.joinIntoString (" // ")); ++checked; }
            if (name == "bag_asis")
            {
                check (m.loud30Percent != 0.0 && m.loud30Percent >= me.integrated - 0.01 && m.loud30Percent <= me.shortTermMax + 0.01, "9 measurementsFrom: loud30Percent between integrated and short-term max", juce::String (m.loud30Percent, 2));
                check (! m.findings.empty() && m.bandsValid, "9 measurementsFrom: diagnosis findings and band energies filled", "findings " + juce::String ((int) m.findings.size()));
            }
        }
        check (checked == 12, "9 all 12 named corpus expectations were asserted", juce::String (checked));
    }

    // ---------------------------------------------------------------- 10. through the real chain: the settings of the plan must give exactly the plan
    {
        for (auto nm : { "bag_quiet-20", "dipmix_quiet-20", "bag_peak-3", "dipmix_peak-3", "dip_peak-3", "bag_brick+6" })
        {
            auto la = loadAudioFile (corpus.getChildFile (juce::String (nm) + ".wav"));
            if (! la.ok()) { check (false, juce::String ("10 load ") + nm, la.error); continue; }
            const auto me = analyse (la.buffer, la.sampleRate, false);
            const auto m = measurementsFrom (me, nullptr);
            Intent q; q.character = PlanCharacter::OFF;
            const auto p = solvePlan (m, q);
            MasterSettings s = p.settings; PreLimiterCache cache; MatchResult mr;
            const auto rr = MasterChain::renderLocked (la.buffer, la.sampleRate, s, cache, nullptr, &mr);
            const double applied = rr.measured.integrated - me.integrated;
            const juce::String nums = "in " + juce::String (me.integrated, 2) + " out " + juce::String (rr.measured.integrated, 2) + " applied " + juce::String (applied, 3)
                                      + " dB (plan " + juce::String (p.totalGainDb, 3) + "), limiter GR max " + juce::String (rr.limiter.gr.maxDb, 3);
            const juce::String n (nm);
            check (std::abs (applied - p.totalGainDb) < 0.05, "10 " + n + ": the chain applies exactly the plan's gain", nums);
            if (n.endsWith ("quiet-20")) check (applied > 14.0 && std::abs (rr.measured.integrated + 14.0) <= 0.2, "10 REVISED " + n + ": through the chain the quiet file lands on -14 LUFS", nums);
            if (n.endsWith ("peak-3"))
                check (rr.limiter.gr.maxDb < 0.001 && levelOnly (s) && ! rr.glue.valid && applied <= 0.0, "10 " + n + ": limiter does 0 dB, no EQ / compression, turned down", nums);
        }
    }

    // ---------------------------------------------------------------- 11. the stacked-gain bug must stay impossible: nothing but the measurements sets the needed gain
    {
        auto sweep = [&] (const juce::String& label, const Measurements& m)
        {
            for (auto tgt : { PlanTarget::STREAM_14, PlanTarget::APPLE_16, PlanTarget::CLUB_8, PlanTarget::CUSTOM })
            {
                Intent base; base.target = tgt; base.customLufs = -10.0;
                const auto p0 = solvePlan (m, base);
                const double needed = p0.desiredGainDb;
                long n = 0, over = 0, moved = 0; double worst = -1e9;
                // every other intent field, in every order of application (the order is the click order of the owner's UI)
                std::vector<std::function<void (Intent&)>> clicks = {
                    [] (Intent& i) { i.character = PlanCharacter::FULL; },
                    [] (Intent& i) { i.character = PlanCharacter::GENTLE; },
                    [] (Intent& i) { i.character = PlanCharacter::OFF; },
                    [] (Intent& i) { i.referenceCurve.assign (121, 3.0f); i.referenceLoudness = -6.0; },
                    [] (Intent& i) { i.albumTarget = -9.0; i.relativeDb = 2.0; },
                    [] (Intent& i) { i.overrides["bell1Db"] = 3.0; i.overrides["airDb"] = 4.0; i.overrides["glue"] = 3.0; },
                    [] (Intent& i) { i.overrides["lowMidOn"] = 1.0; i.overrides["compOn"] = 1.0; i.overrides["widthOn"] = 1.0; i.overrides["tilt"] = 2.0; },
                    [] (Intent& i) { i.overrides["trim"] = 30.0; },
                    [] (Intent& i) { i.overrides["drive"] = 30.0; },
                    [] (Intent& i) { i.overrides["trim"] = 10.0; i.overrides["drive"] = 10.0; } };
                std::vector<int> order (clicks.size());
                for (size_t k = 0; k < order.size(); ++k) order[k] = (int) k;
                juce::Random rnd (1234);
                for (int rep = 0; rep < 400; ++rep)
                {
                    for (size_t k = order.size() - 1; k > 0; --k) std::swap (order[k], order[(size_t) rnd.nextInt ((int) k + 1)]);
                    Intent q = base;
                    const int count = 1 + rnd.nextInt ((int) clicks.size());
                    for (int k = 0; k < count; ++k) clicks[(size_t) order[(size_t) k]] (q);
                    const auto p = solvePlan (m, q); ++n;
                    const double g = gainOf (p);
                    worst = std::max (worst, g - needed);
                    if (g > needed + 0.2 + 1e-9 || p.totalGainDb > needed + 0.2 + 1e-9) ++over;
                    if (std::abs (p.desiredGainDb - needed) > 1e-9) ++moved;     // the needed gain itself never depends on anything but the measurements, target and ceiling
                }
                check (over == 0 && moved == 0, "11 " + label + " target " + juce::String ((int) tgt) + ": applied gain never above needed + 0.2 dB over 400 random click orders / intents",
                       "needed " + juce::String (needed, 2) + " dB, worst applied - needed " + juce::String (worst, 3) + ", needed changed " + juce::String (moved));
            }
        };
        sweep ("synthetic -12.5 LUFS", M (-12.5, 12.0, 11.0));
        sweep ("synthetic -30 LUFS", M (-30.0, 14.0, 12.0));
        for (auto nm : { "bag_peak-3", "dip_quiet-20" })
        {
            auto la = loadAudioFile (corpus.getChildFile (juce::String (nm) + ".wav"));
            if (! la.ok()) { check (false, juce::String ("11 load ") + nm, la.error); continue; }
            auto m = measurementsFrom (analyse (la.buffer, la.sampleRate, false), nullptr);
            measureLimiterResponse (la.buffer, la.sampleRate, m);
            sweep (juce::String ("corpus ") + nm + " (measured limiter)", m);
        }
    }

    // ---------------------------------------------------------------- 12. SPEC_V3 2 REVISED on the real corpus, through the real chain
    {
        std::printf ("\n--- 12. every corpus file x CLUB -8 / STREAM -14 / APPLE -16, plan from measurements (+ measured limiter response), rendered by the chain ---\n");
        std::printf ("%-18s %-7s %9s %9s %8s %8s %8s %8s %8s %9s\n", "file", "target", "in LUFS", "out LUFS", "err LU", "TP dBTP", "gain dB", "avgGR", "peakGR", "chainPkGR");
        juce::Array<juce::File> files; corpus.findChildFiles (files, juce::File::findFiles, false, "*.wav"); files.sort();
        int reached = 0, total = 0, tpOk = 0, finishedLevelOnly = 0;
        for (auto& f : files)
        {
            auto la = loadAudioFile (f);
            if (! la.ok()) continue;
            const auto name = f.getFileNameWithoutExtension();
            const auto me = analyse (la.buffer, la.sampleRate, true);
            const auto dm = measureMix (la.buffer, la.sampleRate);
            auto m = measurementsFrom (me, &dm);
            measureLimiterResponse (la.buffer, la.sampleRate, m);
            if (name.endsWith ("verse30s")) m.songSeconds = 0.0;                      // the whole file is the song here
            for (auto tgt : { PlanTarget::CLUB_8, PlanTarget::STREAM_14, PlanTarget::APPLE_16 })
            {
                Intent q; q.target = tgt; q.character = PlanCharacter::OFF;
                const auto p = solvePlan (m, q);
                MasterSettings s = p.settings; PreLimiterCache cache; MatchResult mr;
                const auto rr = MasterChain::renderLocked (la.buffer, la.sampleRate, s, cache, nullptr, &mr);
                const double tgtL = tgt == PlanTarget::CLUB_8 ? -8.0 : tgt == PlanTarget::STREAM_14 ? -14.0 : -16.0;
                const double err = rr.measured.integrated - tgtL, tpOut = rr.measured.truePeak;
                std::printf ("%-18s %-7s %9.2f %9.2f %+8.3f %8.2f %+8.2f %8.2f %8.2f %9.2f\n", name.toRawUTF8(), tgt == PlanTarget::CLUB_8 ? "CLUB" : tgt == PlanTarget::STREAM_14 ? "STREAM" : "APPLE",
                            me.integrated, rr.measured.integrated, err, tpOut, p.totalGainDb, p.predictedAverageGrDb, p.predictedPeakGrDb, rr.limiter.gr.maxDb);
                ++total;
                const bool ok = std::abs (err) <= 0.2, tpGood = tpOut <= -1.0 + 0.05;
                if (ok) ++reached;
                if (tpGood) ++tpOk;
                if (! ok || ! tpGood || p.targetUnreachable) check (false, "12 " + name + " " + juce::String ((int) tgt), "err " + juce::String (err, 3) + " LU, TP " + juce::String (tpOut, 2) + ", unreachable " + juce::String ((int) p.targetUnreachable));
                if (name.endsWith ("quiet-20") && tgt == PlanTarget::CLUB_8)
                    check (p.totalGainDb > 18.0 && p.warnings.joinIntoString (" ").contains ("noise floor") && p.limiterStatus.contains ("noise floor"), "12 " + name + " CLUB: reached with the noise-floor warning", p.limiterStatus);
                if (levelOnly (s)) ++finishedLevelOnly;
            }
        }
        check (total == 54 && reached == total, "12 all 18 corpus files x CLUB / STREAM / APPLE land within +-0.2 LU of the target through the chain", juce::String (reached) + " of " + juce::String (total));
        check (tpOk == total, "12 true peak <= the -1.0 dBTP ceiling (+0.05 dB meter tolerance) on every one", juce::String (tpOk) + " of " + juce::String (total));
        check (finishedLevelOnly == total, "12 character OFF stays level + ceiling only (no EQ / compressor / stereo)", juce::String (finishedLevelOnly) + " of " + juce::String (total));
    }

    // ---------------------------------------------------------------- 13. album v3.2 (SPEC_V3 2 REVISED): every track reaches album target + its offset
    {
        std::printf ("\n--- 13. ALBUM on three different corpus files, through batch::run (measure all, plan each, render, calibrate) ---\n");
        const char* names[] = { "bag_asis", "dipmix_asis", "dip_quiet-20" };
        for (double albumT : { -14.0, -9.0 })
        {
            std::vector<batch::Job> jobs;
            for (auto nm : names) { batch::Job j; j.file = corpus.getChildFile (juce::String (nm) + ".wav"); jobs.push_back (j); }
            batch::Options o; o.album = true; o.outDir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("mihenk_plan_check_album_" + juce::String ((int) -albumT));
            o.outDir.deleteRecursively();
            Intent ai; ai.target = PlanTarget::ALBUM; ai.albumTarget = albumT; ai.relativeDb = 0.0;       // the batch derives each track's offset
            for (auto& j : jobs) { j.hasIntent = true; j.intent = ai; }
            const auto res = batch::run (jobs, o, {});
            const juce::String tag = "13 ALBUM " + juce::String (albumT, 0) + " LUFS: ";
            bool allOk = res.tracks.size() == 3 && res.plan.valid;
            double worstErr = 0.0, worstTp = -100.0, worstOffErr = 0.0, worstGain = -100.0;
            juce::String line;
            for (size_t i = 0; allOk && i < 3; ++i)
            {
                const auto& t = res.tracks[i];
                allOk &= t.ok;
                if (! t.ok) break;
                const double want = res.plan.targetLufs + res.plan.offsetLu[i];                  // album target + the track's relative offset
                worstErr = std::max (worstErr, std::abs (t.out.integrated - want));
                worstTp = std::max (worstTp, t.out.truePeak);
                worstOffErr = std::max (worstOffErr, std::abs (t.albumOffsetLu - res.plan.offsetLu[i]) + std::abs (t.targetLufs - want));   // offsets carried exactly into the plan
                worstGain = std::max (worstGain, t.plannedGainDb);
                line << names[i] << " in " << juce::String (t.in.integrated, 2) << " > out " << juce::String (t.out.integrated, 2) << " want " << juce::String (want, 2)
                     << " (off " << juce::String (res.plan.offsetLu[i], 2) << ", gain " << juce::String (t.plannedGainDb, 1) << ", TP " << juce::String (t.out.truePeak, 2) << ")  ";
            }
            std::printf ("%s\n", line.toRawUTF8());
            check (allOk && worstErr <= 0.2, tag + "every track within +-0.2 LU of its album target (album target + relative offset)", "worst |error| " + juce::String (worstErr, 3) + " LU");
            check (allOk && worstTp <= -1.0 + 0.05, tag + "true peak <= the -1.0 dBTP ceiling (+0.05 dB meter tolerance)", "highest " + juce::String (worstTp, 2) + " dBTP");
            check (allOk && worstOffErr < 1e-9, tag + "relative offsets between tracks are the plan's, exactly (track target = album target + offset)", "difference " + juce::String (worstOffErr, 12));
            if (allOk)
            {
                double pairErr = 0.0;       // the spacing between masters equals the spacing between targets
                for (size_t a = 0; a < 3; ++a) for (size_t b = a + 1; b < 3; ++b)
                    pairErr = std::max (pairErr, std::abs ((res.tracks[a].out.integrated - res.tracks[b].out.integrated) - (res.plan.offsetLu[a] - res.plan.offsetLu[b])));
                check (pairErr <= 0.4, tag + "spacing between masters = spacing between planned offsets (two tracks, each within 0.2 LU)", "worst pair deviation " + juce::String (pairErr, 3) + " LU");
                check (worstGain > 8.0, tag + "no +3/+8 gain cap (the quiet-20 track gets what its album target needs, over +8 dB)", "largest planned gain " + juce::String (worstGain, 1) + " dB");
            }
            o.outDir.deleteRecursively();
        }
    }

    std::printf ("\nPASS totals: %d\nFAIL totals: %d\n", passes, fails);
    return fails == 0 ? 0 : 1;
}
