// Reviewer's adversarial audit of Source/engine/Plan.{h,cpp}. Read-only use of the engine. Not part of the product.
#include <JuceHeader.h>
#include "../Source/engine/Plan.h"
#include "../Source/engine/AudioIO.h"
#include "../Source/engine/Preset.h"
#include <cstdio>
#include <cmath>
using namespace mihenk;

static int nPass = 0, nFail = 0;
static void R (bool ok, const juce::String& id, const juce::String& detail = {})
{ std::printf ("%s  %s  %s\n", ok ? "PASS" : "FAIL", id.toRawUTF8(), detail.toRawUTF8()); ok ? ++nPass : ++nFail; }

static juce::String fp (const Plan& p)
{
    juce::String s = juce::JSON::toString (settingsToVar (p.settings), true);
    s << "|tl" << (int) p.settings.targetLock << "|md" << p.settings.limiter.maxDriveDb << "|ce" << p.settings.limiter.ceilingDbtp
      << "|dyn" << (int) p.settings.eq.dynLowMid.enabled << (int) p.settings.eq.dynUpperMid.enabled << (int) p.settings.eq.tightener.enabled << (int) p.settings.eq.airDynamic
      << "|mc" << (int) p.settings.eq.matchCurveDb.size() << "|ma" << p.settings.eq.matchAmount << "|tg" << p.settings.targetLufs
      << "|G" << juce::String (p.totalGainDb, 9) << "|ag" << p.predictedAverageGrDb << "|pg" << p.predictedPeakGrDb << "|U" << (int) p.targetUnreachable;
    for (auto& a : p.explain) s << "|E:" << a.action << "=" << juce::String (a.value, 9) << ":" << a.reason;
    for (auto& r : p.refused) s << "|R:" << r;
    for (auto& w : p.warnings) s << "|W:" << w;
    return s;
}
static Measurements M (double l, double plr = 14.0, double crest = 12.0, double songS = 180, double covS = 180)
{ Measurements x; x.integrated = l; x.samplePeak = l + plr; x.truePeak = x.samplePeak; x.plr = plr; x.crest = crest;
  x.shortTermMax = l + 2; x.momentaryMax = l + 4; x.lra = 6; x.loud30Percent = l + 1.5; x.coverageSeconds = covS; x.songSeconds = songS; return x; }
static double tgt (const Measurements& m, const Intent& i)
{ switch (i.target) { case PlanTarget::STREAM_14: return -14; case PlanTarget::APPLE_16: return -16; case PlanTarget::CLUB_8: return -8;
    case PlanTarget::KEEP_INPUT: return m.integrated; case PlanTarget::ALBUM: return i.albumTarget + i.relativeDb; default: return i.customLufs; } }
static bool isFinished (const Measurements& m, double t)
{ return m.plr < 11.0 || m.crest < 9.0 || (m.samplePeak > -3.0 && std::abs (m.integrated - t) <= 3.0); }
static double gainOf (const Plan& p) { return p.settings.trimDb + p.settings.limiter.driveDb; }

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::File corpus (argc > 1 ? argv[1] : "tests/corpus");

    // ---------------------------------------------------------------- 3. caps sweep
    {
        long n = 0, capViol = 0, repViol = 0, grViol = 0, grHonest = 0, refusedMismatch = 0, unreachMismatch = 0, ovCapViol = 0, ovRep = 0, ovN = 0, textBad = 0, unreachNeeded = 0, unreachMissing = 0, explN = 0, explOver = 0, explRefMis = 0;
        juce::String firstCap, firstRef, firstGr, firstOv, firstHon;
        const PlanTarget targets[] = { PlanTarget::STREAM_14, PlanTarget::APPLE_16, PlanTarget::CLUB_8, PlanTarget::KEEP_INPUT, PlanTarget::ALBUM, PlanTarget::CUSTOM };
        for (double l = -30; l <= -4; l += 1.0) for (double plr = 6; plr <= 20; plr += 2) for (double crest = 5; crest <= 18; crest += 3)
        for (auto t : targets) for (int ch = 0; ch < 3; ++ch) for (int ovk = 0; ovk < 4; ++ovk)
        {
            Intent q; q.target = t; q.character = (PlanCharacter) ch; q.albumTarget = -14; q.relativeDb = (ch == 1 ? -2.0 : 3.0); q.customLufs = -10;
            if (ovk == 1) q.overrides["drive"] = 20.0;
            if (ovk == 2) q.overrides["trim"] = 20.0;
            if (ovk == 3) { q.overrides["drive"] = 12.0; q.overrides["trim"] = 6.0; }
            const auto m = M (l, plr, crest); const auto p = solvePlan (m, q); ++n;
            const double target = tgt (m, q), desired = (t == PlanTarget::KEEP_INPUT) ? std::min (0.0, target - m.integrated) : target - m.integrated;
            const double cap = std::abs (m.integrated - target) <= 6.0 ? 3.0 : 8.0;
            const double real = gainOf (p);
            const bool legacyT = t == PlanTarget::KEEP_INPUT;      // SPEC_V3 2 REVISED: explicit targets and ALBUM (album v3.2) are reached, only KEEP INPUT keeps the +3 / +8 rule
            if (! legacyT)
            {
                if (ovk == 0)
                {
                    ++explN;
                    if (real > p.desiredGainDb + 0.2 + 1e-9 && p.desiredGainDb > -24.0) ++explOver;
                    if (p.refused.isEmpty() == p.targetUnreachable) ++explRefMis;
                    if (std::abs (real - p.totalGainDb) > 1e-9) ++repViol;
                    const double needPk = std::max (0.0, m.samplePeak + real - q.ceilingDbtp);
                    if (p.predictedPeakGrDb + 1e-6 < needPk) ++grHonest;
                }
                else { ++ovN; Intent q0 = q; q0.overrides.clear(); if (real > gainOf (solvePlan (m, q0)) + 1e-9) ++ovCapViol; }
            }
            else if (ovk == 0)
            {
                if (real > cap + 1e-9 || p.totalGainDb > cap + 1e-9) { ++capViol; if (firstCap.isEmpty()) firstCap << "int " << l << " plr " << plr << " t " << (int) t << " gain " << real; }
                if (std::abs (real - p.totalGainDb) > 1e-9) ++repViol;
                const bool bound = desired > cap + 1e-9;
                if (bound != (! p.refused.isEmpty())) { ++refusedMismatch; if (firstRef.isEmpty()) firstRef << "int " << l << " t " << (int) t << " desired " << desired << " cap " << cap << " refused " << p.refused.size(); }
                if (bound != p.targetUnreachable) ++unreachMismatch;
                // independent physical estimate of peak GR at the ceiling: peak + gain - ceiling
                const double needPk = std::max (0.0, m.samplePeak + real - q.ceilingDbtp);
                if (p.predictedPeakGrDb > 6.0 + 1e-9 || p.predictedAverageGrDb > 2.0 + 1e-9) ++grViol;
                if (p.predictedPeakGrDb + 1e-6 < needPk) { ++grHonest; if (firstHon.isEmpty()) firstHon << "int " << l << " plr " << plr << " peak " << m.samplePeak << " gain " << real << " needs " << needPk << " reported " << p.predictedPeakGrDb; }
                if (needPk > 6.0 + 1e-9) { ++unreachNeeded; if (! p.targetUnreachable) { ++unreachMissing; } }
                if (! p.refused.isEmpty() && ! (p.refused[0].contains ("dB"))) ++textBad;
            }
            else
            {
                ++ovN;
                if (real > cap + 1e-9) { ++ovCapViol; if (firstOv.isEmpty()) firstOv << "ovk " << ovk << " int " << l << " t " << (int) t << " trim " << p.settings.trimDb << " drive " << p.settings.limiter.driveDb << " total " << real; }
                if (real > 0 && ! (p.refused.size() > 0)) ++ovRep;
            }
        }
        R (explN > 0 && explOver == 0, "3a REVISED explicit targets + ALBUM: applied gain <= needed gain + 0.2 dB (no +3/+8 cap)", juce::String (explN) + " plans, over " + juce::String (explOver));
        R (explRefMis == 0, "3c REVISED explicit targets + ALBUM: refused/targetUnreachable only when even the limiter cannot reach it", "mismatches " + juce::String (explRefMis));
        R (capViol == 0, "3a KEEP_INPUT gain cap (trim+drive <= +3/+8), no overrides", juce::String (n) + " plans, violations " + juce::String (capViol) + " " + firstCap);
        R (repViol == 0, "3b totalGainDb == settings.trimDb + limiter.driveDb", "mismatches " + juce::String (repViol));
        R (refusedMismatch == 0 && unreachMismatch == 0, "3c KEEP_INPUT refused/targetUnreachable set exactly when cap binds", "refused mismatches " + juce::String (refusedMismatch) + ", unreachable mismatches " + juce::String (unreachMismatch) + " " + firstRef);
        R (grViol == 0, "3d KEEP_INPUT reported predicted GR within 2/6", "violations " + juce::String (grViol));
        R (grHonest == 0, "3e reported peak GR >= independent estimate (peak+gain-ceiling)", "plans where plan under-reports: " + juce::String (grHonest) + " " + firstHon);
        R (unreachMissing == 0, "3f KEEP_INPUT limiter peak GR > 6 dB => targetUnreachable", juce::String (unreachNeeded) + " plans need >6 dB peak GR; flagged unreachable in " + juce::String (unreachNeeded - unreachMissing));
        R (ovCapViol == 0, "3g overrides never lift the total above needed gain (explicit) / the cap (KEEP_INPUT)", juce::String (ovN) + " plans, over-cap " + juce::String (ovCapViol) + " " + firstOv);
        R (textBad == 0, "3h refused text states the dB needed", "refused entries without 'dB': " + juce::String (textBad));
        // the exact brief sentence
        auto p = solvePlan (M (-23.2), Intent{});
        std::printf ("INFO  refused text for -23.2 integrated, STREAM_14: %s\n", p.refused.joinIntoString (" // ").toRawUTF8());
        R (p.refused.isEmpty() && ! p.targetUnreachable && gainOf (p) > 9.0 && p.limiterStatus.contains ("-14 LUFS reached"), "3i REVISED -23.2 to STREAM -14 is reached, not refused", p.limiterStatus);
        { Intent a; a.target = PlanTarget::ALBUM; a.albumTarget = -13.3; a.relativeDb = -1.7; auto pa = solvePlan (M (-23.2), a);
          R (pa.refused.isEmpty() && ! pa.targetUnreachable && std::abs (gainOf (pa) - 8.2) < 0.05, "3i ALBUM (album v3.2): -23.2 to album -15 needs +8.2 dB and is reached (peaks stay under the ceiling), nothing refused", juce::String (gainOf (pa), 2)); }
    }

    // ---------------------------------------------------------------- 4. finished material rule
    {
        long n = 0, bad = 0, noWhy = 0, wrongDetect = 0, extraBad = 0; juce::String first, firstX;
        struct C { const char* name; Measurements m; };
        std::vector<C> cs;
        cs.push_back ({ "PLR 9.5", M (-14, 9.5, 12) });
        cs.push_back ({ "crest 8", M (-14, 14, 8) });
        cs.push_back ({ "peak -2 & |int-t|<=3", M (-15, 13, 12) });
        const PlanTarget targets[] = { PlanTarget::STREAM_14, PlanTarget::APPLE_16, PlanTarget::CLUB_8, PlanTarget::KEEP_INPUT };
        for (auto& c : cs) for (auto t : targets) for (int ch = 0; ch < 3; ++ch)
        {
            Intent q; q.target = t; q.character = (PlanCharacter) ch;
            Finding f; f.id = "mud"; f.scenario = 3; f.severity = Severity::problem; q.referenceCurve.assign (100, 3.0f); q.referenceLoudness = -9;
            auto m = c.m; m.findings.push_back (f); m.spectrum.assign (100, -50.f);
            auto p = solvePlan (m, q); ++n;
            const auto& s = p.settings; const bool fin = isFinished (m, tgt (m, q));
            if (! fin) continue;
            const bool neutral = ! s.eq.enabled && ! s.comp.enabled && ! s.width.enabled && s.width.width == 1.0 && s.width.highWidth == 1.0 && s.width.monoBelowHz == 0.0
                                 && s.eq.matchCurveDb.empty() && s.eq.matchAmount == 0.0 && s.eq.airDb == 0 && s.eq.bell1Db == 0 && s.eq.tiltDbPerOct == 0 && ! s.eq.lowCutOn
                                 && ! s.eq.dynLowMid.enabled && ! s.eq.dynUpperMid.enabled && ! s.eq.tightener.enabled && ! s.eq.airDynamic && ! s.width.phaseSafeLowMono;
            if (! neutral) { ++bad; if (first.isEmpty()) first << c.name << " t" << (int) t << " ch" << ch; }
            juce::String all; for (auto& a : p.explain) all << a.reason << " ; ";
            if (! all.contains ("Finished master detected")) ++noWhy;
            if (! all.contains ("PLR") && c.name[0] == 'P') ++extraBad; // brief wants "(PLR 9.5 LU)" in explain
        }
        R (bad == 0, "4a finished => EQ/comp/stereo/tone-match/dyn all off+neutral (3 chars x 4 targets x 3 triggers)", juce::String (n) + " runs, non-neutral " + juce::String (bad) + " " + first);
        R (noWhy == 0, "4b explain says Finished master detected", "missing " + juce::String (noWhy));
        R (extraBad == 0, "4c explain quotes the trigger value (brief: 'Finished master detected (PLR 9.5 LU)')", "runs without the value " + juce::String (extraBad));
        // boundaries
        struct B { const char* n; Measurements m; bool exp; };
        B bs[] = { { "plr 10.99", M (-20, 10.99, 12), true }, { "plr 11.0", M (-20, 11.0, 12), false }, { "crest 8.99", M (-20, 14, 8.99), true }, { "crest 9.0", M (-20, 14, 9.0), false },
                   { "peak -3.0 exactly, int -15", M (-15, 12.0, 12), false }, { "peak -2.99 int -15", M (-15, 12.01, 12), true }, { "peak -2 int -17.01", M (-17.01, 15.01, 12), false }, { "peak -2 int -17.0", M (-17.0, 15.0, 12), true } };
        for (auto& b : bs)
        {
            Intent q; q.character = PlanCharacter::FULL; auto p = solvePlan (b.m, q);
            const bool got = ! p.settings.eq.enabled && ! p.settings.comp.enabled;
            R (got == b.exp, juce::String ("4d boundary ") + b.n, juce::String ("expected finished=") + (b.exp ? "1" : "0") + " got neutral=" + (got ? "1" : "0"));
        }
    }

    // ---------------------------------------------------------------- 2. which Measurements fields move the plan
    {
        Intent q; q.character = PlanCharacter::FULL; q.target = PlanTarget::STREAM_14;
        auto base = M (-18, 14, 13); const auto fb = fp (solvePlan (base, q));
        auto test = [&] (const char* name, Measurements m, Intent qi = {}) { Intent z = q; z = qi.target == PlanTarget::STREAM_14 ? q : qi; return fp (solvePlan (m, z)) != fb; };
        struct F { const char* n; bool used; };
        std::vector<F> fs;
        { auto m = base; m.shortTermMax = -2; fs.push_back ({ "shortTermMax (full coverage)", test ("", m) }); }
        { auto m = base; m.momentaryMax = 0; fs.push_back ({ "momentaryMax", test ("", m) }); }
        { auto m = base; m.lra = 1; fs.push_back ({ "lra", test ("", m) }); }
        { auto m = base; m.truePeak = 2; fs.push_back ({ "truePeak", test ("", m) }); }
        { auto m = base; m.loud30Percent = -40; fs.push_back ({ "loud30Percent", test ("", m) }); }
        { auto m = base; m.correlation = -0.5; fs.push_back ({ "correlation", test ("", m) }); }
        { auto m = base; m.lowCorrelation = -0.5; fs.push_back ({ "lowCorrelation", test ("", m) }); }
        { auto m = base; m.dcOffset = -20; fs.push_back ({ "dcOffset", test ("", m) }); }
        { auto m = base; m.spectrum.assign (100, -80.f); for (int k = 0; k < 20; ++k) m.spectrum[k] = -30.f; fs.push_back ({ "spectrum / band energies", test ("", m) }); }
        { auto m = base; Finding f; f.id = "air"; f.scenario = 9; f.severity = Severity::problem; m.findings.push_back (f); Finding g = f; g.id = "mud"; g.scenario = 3; m.findings.push_back (g); fs.push_back ({ "findings (diagnosis ids/severities)", test ("", m) }); }
        for (auto& f : fs) R (f.used, juce::String ("2 field used by solvePlan: ") + f.n, f.used ? "plan changes" : "NO EFFECT on the plan (character FULL)");
        { Intent z = q; z.referenceCurve.assign (100, 4.0f); z.referenceLoudness = -9; auto p = solvePlan (base, z); R (! p.settings.eq.matchCurveDb.empty(), "2 Intent.referenceCurve produces tone match in FULL", "matchCurveDb size " + juce::String ((int) p.settings.eq.matchCurveDb.size())); }
        { // coverage short-term
          auto m = M (-18, 14, 13, 169, 30); Intent z; auto p = solvePlan (m, z); auto m2 = m; m2.shortTermMax = -3; auto p2 = solvePlan (m2, z);
          R (p.warnings.joinIntoString ("").contains ("Heard only 30") && fp (p) != fp (p2), "2 low coverage: warning AND drive solved on short-term max", "warning=" + juce::String (p.warnings.joinIntoString ("//")) + " ; plan changes with shortTermMax: " + (fp (p) != fp (p2) ? "yes" : "no")); }
    }

    // ---------------------------------------------------------------- character behaviour
    {
        auto m = M (-18, 14, 13); Intent a, b, c; a.character = PlanCharacter::OFF; b.character = PlanCharacter::GENTLE; c.character = PlanCharacter::FULL;
        auto pa = solvePlan (m, a), pb = solvePlan (m, b), pc = solvePlan (m, c);
        R (! pa.settings.eq.enabled && ! pa.settings.comp.enabled && ! pa.settings.width.enabled && pa.settings.limiter.enabled, "1 OFF: trim + limiter only on open material (eq, comp, width all off)", "width " + juce::String ((int) pa.settings.width.enabled) + " eq " + juce::String ((int) pa.settings.eq.enabled) + " comp " + juce::String ((int) pa.settings.comp.enabled));
        R (pb.settings.comp.glueDb <= 1.0 + 1e-9 && pb.settings.eq.airDb <= 1.5 + 1e-9, "1 GENTLE: glue<=1 and air<=1.5", "glue " + juce::String (pb.settings.comp.glueDb) + " air " + juce::String (pb.settings.eq.airDb));
        // GENTLE "only when the measurements ask": air-rich vs air-poor must differ
        auto mAir = m; mAir.spectrum.assign (100, -40.f); auto mDull = m; mDull.spectrum.assign (100, -40.f); for (int k = 80; k < 100; ++k) mDull.spectrum[k] = -80.f;
        R (fp (solvePlan (mAir, b)) != fp (solvePlan (mDull, b)), "1 GENTLE adds AIR only when measurements ask (air-rich vs air-deficit spectrum differ)", "air-rich " + juce::String (solvePlan (mAir, b).settings.eq.airDb) + " dull " + juce::String (solvePlan (mDull, b).settings.eq.airDb));
        auto mDense = M (-18, 12, 10); mDense.lra = 3;
        R (solvePlan (m, c).settings.comp.glueDb != solvePlan (mDense, c).settings.comp.glueDb, "1 FULL glue follows measurements (open vs dense input)", "open " + juce::String (pc.settings.comp.glueDb) + " dense " + juce::String (solvePlan (mDense, c).settings.comp.glueDb));
        R (pc.settings.eq.airDb >= pb.settings.eq.airDb, "1 FULL is a superset of GENTLE (air)", "GENTLE air " + juce::String (pb.settings.eq.airDb) + " FULL air " + juce::String (pc.settings.eq.airDb));
        R (pc.settings.limiter.maxDriveDb <= 8.0 + 1e-9, "1 plan sets limiter.maxDriveDb to the cap (so the chain cannot exceed it)", "maxDriveDb " + juce::String (pc.settings.limiter.maxDriveDb));
        R (! pc.settings.targetLock, "1 plan does not leave TARGET LOCK on (chain would re-solve drive, ignoring the cap)", "targetLock " + juce::String ((int) pc.settings.targetLock));
        R (pc.settings.dcFilter || true, "-");
        --nPass; // (placeholder line removed from totals)
    }

    // ---------------------------------------------------------------- 6. idempotence / order independence, field by field
    {
        bool idem = true, ord = true; juce::String first;
        for (double l : { -30.0, -23.2, -16.0, -12.0, -9.0, -6.0 }) for (int ch = 0; ch < 3; ++ch) for (int ti = 0; ti < 6; ++ti)
        {
            auto m = M (l, 15, 13); m.songSeconds = 169; m.coverageSeconds = (ti == 2 ? 30 : 169);
            Intent a; a.target = (PlanTarget) ti; a.character = (PlanCharacter) ch; a.relativeDb = -2; a.customLufs = -11; a.ceilingDbtp = -0.5; a.overrides["bell1Db"] = 1.2; a.overrides["airDb"] = 2;
            const auto f1 = fp (solvePlan (m, a)), f2 = fp (solvePlan (m, a));
            if (f1 != f2) { idem = false; first << "idem"; }
            // six construction orders
            Intent o[6];
            o[0].target = a.target; o[0].character = a.character; o[0].ceilingDbtp = a.ceilingDbtp; o[0].relativeDb = a.relativeDb; o[0].customLufs = a.customLufs; o[0].overrides = a.overrides;
            o[1].overrides = a.overrides; o[1].customLufs = a.customLufs; o[1].relativeDb = a.relativeDb; o[1].ceilingDbtp = a.ceilingDbtp; o[1].character = a.character; o[1].target = a.target;
            o[2].character = a.character; o[2].target = a.target; o[2].overrides = a.overrides; o[2].relativeDb = a.relativeDb; o[2].ceilingDbtp = a.ceilingDbtp; o[2].customLufs = a.customLufs;
            o[3].target = PlanTarget::CUSTOM; o[3].character = PlanCharacter::FULL; o[3].target = a.target; o[3].character = a.character; o[3].ceilingDbtp = a.ceilingDbtp; o[3].relativeDb = a.relativeDb; o[3].customLufs = a.customLufs; o[3].overrides = a.overrides;
            o[4].overrides["airDb"] = 2; o[4].overrides["bell1Db"] = 1.2; o[4].customLufs = a.customLufs; o[4].relativeDb = a.relativeDb; o[4].ceilingDbtp = a.ceilingDbtp; o[4].target = a.target; o[4].character = a.character;
            o[5].ceilingDbtp = a.ceilingDbtp; o[5].customLufs = a.customLufs; o[5].character = a.character; o[5].relativeDb = a.relativeDb; o[5].target = a.target; o[5].overrides = a.overrides;
            for (auto& x : o) if (fp (solvePlan (m, x)) != f1) { ord = false; first << "ord"; }
        }
        R (idem, "6a idempotence field-by-field (settings JSON + explain + refused + warnings), 108 cases", first);
        R (ord, "6b order independence field-by-field, 6 orders x 108 cases", first);
        // intent-only dependence: click sequence then return to the same intent
        auto m = M (-18, 14, 13); Intent q; q.character = PlanCharacter::FULL; auto base = fp (solvePlan (m, q));
        for (int k = 0; k < 20; ++k) { Intent z = q; z.target = (PlanTarget)(k % 6); z.character = (PlanCharacter)(k % 3); (void) solvePlan (m, z); }
        R (fp (solvePlan (m, q)) == base, "6c plan unchanged after 20 intervening solves (no hidden state)");
        // non-trivial content check so equality is meaningful
        auto p = solvePlan (m, q);
        R (fp (p).length() > 1500, "6d fingerprint is substantive", "length " + juce::String (fp (p).length()) + ", explain entries " + juce::String ((int) p.explain.size()));
    }

    // ---------------------------------------------------------------- 7. KEEP_INPUT / ALBUM / overrides
    {
        bool keepOk = true, albumOk = true; juce::String f1, f2;
        for (double l = -30; l <= -4; l += 1) for (double plr : { 8.0, 14.0 })
        {
            Intent k; k.target = PlanTarget::KEEP_INPUT; auto m = M (l, plr, 13); auto p = solvePlan (m, k);
            const double predicted = m.integrated + p.totalGainDb; if (predicted > m.integrated + 1e-9) { keepOk = false; f1 << l; }
            const double ceilingCost = std::max (0.0, m.samplePeak + p.totalGainDb - k.ceilingDbtp);
            if (ceilingCost > 1.0 + 1e-9) { /* ceiling cost >1 dB should be explained */ juce::String all; for (auto& a : p.explain) all << a.reason; if (! all.containsIgnoreCase ("ceiling")) { keepOk = false; f1 << "unexplained@" << l; } }
            Intent a; a.target = PlanTarget::ALBUM; a.albumTarget = -13.3; a.relativeDb = -1.7; auto pa = solvePlan (m, a);
            if (std::abs (pa.settings.targetLufs - (-15.0)) > 1e-9) { albumOk = false; f2 << "tl " << pa.settings.targetLufs; }
            Intent cu; cu.target = PlanTarget::CUSTOM; cu.customLufs = -15.0; auto pc = solvePlan (m, cu);      // album v3.2: reached like any chosen target
            if (std::abs (pa.totalGainDb - pc.totalGainDb) > 1e-6) { albumOk = false; f2 << " gain@" << l << "=" << pa.totalGainDb << "!=" << pc.totalGainDb; }
            if (! pa.targetUnreachable && pa.totalGainDb > -23.9 && std::abs (m.integrated + pa.totalGainDb - pa.predictedAverageGrDb + 15.0) > 0.2) { albumOk = false; f2 << " miss@" << l; }
        }
        R (keepOk, "7a KEEP_INPUT never louder than input; >1 dB ceiling cost explained", f1);
        R (albumOk, "7b ALBUM (album v3.2): target = albumTarget + relativeDb, reached like a chosen target (gain = CUSTOM at that value, model within 0.2 LU, no cap)", f2.substring (0, 200));
        { Intent o; o.overrides["drive"] = 20; auto p = solvePlan (M (-12), o);
          R (gainOf (p) <= 3.0 + 1e-9 && ! p.refused.isEmpty(), "7c drive +20 override capped and reported (int -12 -> cap 3)", "trim " + juce::String (p.settings.trimDb) + " drive " + juce::String (p.settings.limiter.driveDb) + " refused " + p.refused.joinIntoString ("//"));
          auto p2 = solvePlan (M (-12), Intent{});
          R (gainOf (p) <= 3.0 + 1e-9, "7c' ... total gain with drive override", "total " + juce::String (gainOf (p)) + " base total " + juce::String (gainOf (p2))); }
        { Intent o; o.overrides["drive"] = 20; auto p = solvePlan (M (-30), o);
          R (gainOf (p) <= p.desiredGainDb + 0.2 && gainOf (p) < 20.0, "7c'' REVISED drive +20 at int -30 is cut to the gain STREAM -14 needs", "total " + juce::String (gainOf (p)) + " needed " + juce::String (p.desiredGainDb)); }
        { Intent o; o.overrides["trim"] = 20; auto p = solvePlan (M (-12), o); R (gainOf (p) <= 3.0 + 1e-9, "7d trim +20 override cannot pass cap", "trim " + juce::String (p.settings.trimDb) + " total " + juce::String (gainOf (p)) + " refused " + juce::String (p.refused.size())); }
        { Intent o; o.overrides["bell1Db"] = 2; o.character = PlanCharacter::FULL; auto m = M (-18, 14, 13); auto pb = solvePlan (m, o); Intent n = o; n.overrides.clear(); auto p0 = solvePlan (m, n);
          auto a = pb; a.settings.eq.bell1Db = p0.settings.eq.bell1Db; a.explain.clear(); auto b = p0; b.explain.clear();
          juce::String sa = juce::JSON::toString (settingsToVar (a.settings)), sb = juce::JSON::toString (settingsToVar (b.settings));
          R (sa == sb && pb.settings.eq.bell1Db == 2 && pb.totalGainDb == p0.totalGainDb, "7e bell1Db override leaves rest of settings identical (FULL)", "bell " + juce::String (pb.settings.eq.bell1Db));
          R (pb.explain.size() > p0.explain.size() || true, "-"); --nPass;
          bool reported = false; for (auto& e : pb.explain) if (e.action.containsIgnoreCase ("override") || e.reason.containsIgnoreCase ("over plan")) reported = true;
          R (reported, "7f overrides reported as deltas over the plan (explain)", "explain entries " + juce::String ((int) pb.explain.size())); }
        { Intent o; o.overrides["glueDb"] = 3; o.overrides["tilt"] = 3; o.overrides["nonsense"] = 1; auto p = solvePlan (M (-18, 14, 13), o);
          R (p.settings.eq.tiltDbPerOct == 3.0, "7g overrides of any parameter id (tilt) honoured (brief: sparse map parameterId->value)", "tilt " + juce::String (p.settings.eq.tiltDbPerOct) + " (only drive/trim/bell1Db/airDb/glueDb are handled in applyOverride)"); }
        { // override on finished material: does it break level-only?
          Intent o; o.overrides["glueDb"] = 3; o.overrides["airDb"] = 4; auto p = solvePlan (M (-14, 9.5, 12), o);
          R (p.settings.eq.airDb == 0 && ! p.settings.comp.enabled, "7h overrides cannot re-enable stages on finished material (level-only is absolute)", "airDb " + juce::String (p.settings.eq.airDb)); }
    }

    // ---------------------------------------------------------------- 8. plain words
    {
        auto p = solvePlan (M (-12.8, 9.5, 10.5), Intent{}); // -3 dBFS peak case style: peak -3.3? use peak -3
        auto m = M (-12.8, 9.8, 10.5); m.samplePeak = -3.0; m.plr = 9.8; p = solvePlan (m, Intent{});
        juce::String all; for (auto& a : p.explain) all << a.action << " " << juce::String (a.value, 1) << " dB: " << a.reason << ". ";
        const auto s = p.summary();
        std::printf ("INFO  summary(): %s\nINFO  explain text: %s\n", s.toRawUTF8(), all.toRawUTF8());
        for (auto j : { "glue", "PLR", "LRA", "crest", "sidechain" }) R (! s.containsIgnoreCase (j) && ! all.containsIgnoreCase (j), juce::String ("8a no jargon '") + j + "' in summary()/explain (-3 dBFS case)");
        R (s.contains ("dB") && s.containsIgnoreCase ("ceiling"), "8b summary() contains dB and ceiling", s);
        R (s.contains ("-1.0") && ! s.contains ("level 0.0") , "8c summary is a readable sentence per action (SPEC 4b: 'Turned down 2.4 dB so it sits ...')", s);
        R (s.containsIgnoreCase ("finished") || s.containsIgnoreCase ("untouched") || s.containsIgnoreCase ("nothing else") , "8d summary() says WHY (refused / nothing else)", s);
    }

    // ---------------------------------------------------------------- 5. REAL CORPUS
    std::printf ("\n--- corpus (%s) ---\n", corpus.getFullPathName().toRawUTF8());
    {
        juce::Array<juce::File> files; corpus.findChildFiles (files, juce::File::findFiles, false, "*.wav"); files.sort();
        R (files.size() == 18, "5.0 corpus has 18 wavs", juce::String (files.size()));
        int checked = 0;
        for (auto& f : files)
        {
            auto la = loadAudioFile (f); if (! la.ok()) { R (false, "5 load " + f.getFileName(), la.error); continue; }
            auto me = analyse (la.buffer, la.sampleRate, true);
            auto dm = measureMix (la.buffer, la.sampleRate);
            auto m = measurementsFrom (me, &dm);
            const auto name = f.getFileNameWithoutExtension();
            const double songS = 169.0;
            if (name.endsWith ("verse30s")) m.songSeconds = songS;
            Intent q; q.target = PlanTarget::STREAM_14; q.character = PlanCharacter::OFF;
            auto p = solvePlan (m, q);
            std::printf ("DATA  %-18s int %7.2f stMax %7.2f peak %6.2f tp %6.2f plr %5.2f crest %5.2f lra %5.2f sec %6.1f | gain %+6.2f trim %+6.2f drive %+5.2f eq %d comp %d width %d unreach %d predAvgGR %.2f predPkGR %.2f refused %d warn %s\n",
                         name.toRawUTF8(), me.integrated, me.shortTermMax, me.samplePeak, me.truePeak, m.plr, me.crest, me.lra, me.seconds, p.totalGainDb, p.settings.trimDb, p.settings.limiter.driveDb,
                         (int) p.settings.eq.enabled, (int) p.settings.comp.enabled, (int) p.settings.width.enabled, (int) p.targetUnreachable, p.predictedAverageGrDb, p.predictedPeakGrDb, p.refused.size(), p.warnings.joinIntoString ("//").toRawUTF8());
            const bool levelOnly = ! p.settings.eq.enabled && ! p.settings.comp.enabled && ! p.settings.width.enabled;
            if (name.endsWith ("peak-3"))
            { R (levelOnly && p.totalGainDb <= 1e-9 && p.predictedAverageGrDb == 0.0 && p.predictedPeakGrDb == 0.0, "5 " + name + ": level-only, gain<=0, predicted GR 0", "gain " + juce::String (p.totalGainDb, 2) + " int " + juce::String (me.integrated, 2) + " levelOnly " + juce::String ((int) levelOnly)); ++checked; }
            else if (name.endsWith ("brick+6"))
            { R (std::abs (p.totalGainDb + 9.0) < 1.5 && levelOnly, "5 " + name + ": gain ~ -9 dB, no EQ/comp", "gain " + juce::String (p.totalGainDb, 2) + " (target-int = " + juce::String (-14 - me.integrated, 2) + ")"); ++checked; }
            else if (name.endsWith ("quiet-20"))
            { R (p.totalGainDb > 14.0 && ! p.targetUnreachable && p.refused.isEmpty(), "5 REVISED " + name + ": STREAM -14 reached (about +15 dB), not refused", "gain " + juce::String (p.totalGainDb, 2) + " int " + juce::String (me.integrated, 2) + " unreach " + juce::String ((int) p.targetUnreachable)); ++checked; }
            else if (name.endsWith ("verse30s"))
            { R (p.warnings.joinIntoString ("").contains ("Heard only"), "5 " + name + ": low-coverage warning (30 s of 169 s)", p.warnings.joinIntoString ("//")); ++checked; }
            // measurement completeness through measurementsFrom
            if (name == "bag_asis")
            {
                R (m.loud30Percent != 0.0, "5m measurementsFrom fills loud30Percent", juce::String (m.loud30Percent));
                R (! m.findings.empty() || true, "-"); --nPass;
                R (m.findings.size() > 0, "5m measurementsFrom fills findings (diagnosis ids/severities)", "count " + juce::String ((int) m.findings.size()));
                R (std::abs (m.truePeak - me.truePeak) < 1e-9, "5m measurementsFrom fills truePeak", "plan " + juce::String (m.truePeak) + " engine " + juce::String (me.truePeak));
                R (std::abs (m.dcOffset - dm.dcDbfs) < 1e-9, "5m dcOffset from diagnosis measures", juce::String (m.dcOffset));
                R (m.songSeconds != m.coverageSeconds || true, "-"); --nPass;
            }
        }
        R (checked == 12, "5.1 all 12 named corpus expectations asserted", juce::String (checked));
    }

    // ---------------------------------------------------------------- 5e. END TO END: does the chain, run on plan.settings, obey the caps?
    {
        for (auto nm : { "bag_quiet-20", "bag_peak-3", "bag_brick+6", "dipmix_quiet-20", "dip_peak-3" })
        {
            auto la = loadAudioFile (corpus.getChildFile (juce::String (nm) + ".wav")); if (! la.ok()) continue;
            auto me = analyse (la.buffer, la.sampleRate, false); auto m = measurementsFrom (me, nullptr);
            Intent q; q.character = PlanCharacter::OFF; auto p = solvePlan (m, q);
            MasterSettings s = p.settings; PreLimiterCache cache; MatchResult mr;
            auto rr = MasterChain::renderLocked (la.buffer, la.sampleRate, s, cache, nullptr, &mr);
            const double outInt = rr.measured.integrated, applied = outInt - me.integrated;
            std::printf ("E2E   %-16s in %.2f LUFS -> out %.2f LUFS applied %+.2f dB (plan said %+.2f) | solved drive %+.2f, trim %+.2f | limiter maxGR %.2f dB | out sample peak %.2f tp %.2f\n",
                         nm, me.integrated, outInt, applied, p.totalGainDb, s.limiter.driveDb, s.trimDb, rr.limiter.gr.maxDb, rr.measured.samplePeak, rr.measured.truePeak);
            const double cap = std::abs (me.integrated + 14) <= 6 ? 3.0 : 8.0;
            R (juce::String (nm).endsWith ("quiet-20") ? std::abs (outInt + 14.0) <= 0.2 : applied <= cap + 0.25, juce::String ("5e REVISED chain on plan.settings: quiet files land on -14 LUFS, the others stay within the cap ") + nm, "applied " + juce::String (applied, 2) + " cap " + juce::String (cap) + " plan " + juce::String (p.totalGainDb, 2));
            R (std::abs (applied - p.totalGainDb) < 0.5, juce::String ("5e chain applied gain == plan.totalGainDb ") + nm, "diff " + juce::String (applied - p.totalGainDb, 2));
            R (rr.limiter.gr.maxDb <= 6.0 + 0.01, juce::String ("5e limiter peak GR <= 6 dB ") + nm, juce::String (rr.limiter.gr.maxDb, 2));
            if (juce::String (nm) == "bag_peak-3")
            { R (rr.limiter.gr.maxDb < 0.05, "5e peak-3: limiter does nothing (predicted GR 0)", juce::String (rr.limiter.gr.maxDb, 3));
              auto r2 = MasterChain::renderLocked (la.buffer, la.sampleRate, s, cache, nullptr, &mr);
              bool same = r2.output->getNumSamples() == rr.output->getNumSamples(); for (int c = 0; same && c < rr.output->getNumChannels(); ++c) same = std::memcmp (r2.output->getReadPointer (c), rr.output->getReadPointer (c), sizeof (float) * (size_t) rr.output->getNumSamples()) == 0;
              R (same, "5e peak-3: second render of same settings bit-exact"); }
        }
    }

    std::printf ("\nAUDIT PASS %d  FAIL %d\n", nPass, nFail);
    return nFail ? 1 : 0;
}
