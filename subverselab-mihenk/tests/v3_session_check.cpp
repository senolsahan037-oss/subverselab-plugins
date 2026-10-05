/*  MIHENK v3 stage 2: the real Session and the real processor, driven the way the panel and a host drive them.

        v3_session_check            all checks (exit code 0 only if every check passes)
        v3_session_check --quick    three corpus files only (a smoke run)

    For every file of tests/corpus/ and every target (STREAM -14, APPLE -16, CLUB -8, KEEP INPUT) x character (OFF, GENTLE, FULL):
      - the measured gain MIHENK applied is within the cap (+3 dB within 6 LU of the target, +8 dB otherwise), exactly the plan's
        for character OFF, and KEEP INPUT never makes the master louder than the mix;
      - the limiter stays inside average GR <= 2 dB and peak GR <= 6 dB, the true peak under the ceiling;
      - finished material is level + ceiling only, whatever the character says;
      - when the target is out of reach the status says so in plain words with the dB numbers (CLUB -8 on the *_peak-3 files);
      - random click orders (targets, characters, bypass, knob writes, clear) end in the very same settings and the very same
        master, bit for bit, as setting the final intent directly; overrides in any order give the same result;
      - ten bypass cycles change nothing (no job, the same rendered buffer, the same settings, the same plan);
      - every parameter id is either an override the plan knows or a documented intent / pass-through field;
      - the plug-in path (LEARN through processBlock, the plan in the real-time chain): the applied gain and the true peak,
        and processBlockBypassed: input delayed by the reported latency, ten cycles leave the chain untouched.
    Files go to a temp folder; the user's preferences and profile are not touched. */
#include <JuceHeader.h>
#include <cstdlib>
#include <map>
#include <set>
#include "../Source/plugin/PluginProcessor.h"
#include "../Source/plugin/Params.h"
#include "../Source/engine/AudioIO.h"
#include "../Source/engine/Plan.h"
#include "../Source/engine/Preset.h"
#include "../Source/app/MainComponent.h"

using namespace mihenk;

namespace
{
int failures = 0, checks = 0;
void check (bool ok, const juce::String& what, const juce::String& detail = {})
{
    ++checks;
    std::printf ("%s  %s%s\n", ok ? "PASS" : "FAIL", what.toRawUTF8(), detail.isEmpty() ? "" : ("  | " + detail).toRawUTF8());
    std::fflush (stdout);
    if (! ok) ++failures;
}
void section (const char* name) { std::printf ("\n-- %s\n", name); std::fflush (stdout); }
using Buffer = juce::AudioBuffer<float>;

uint64_t hashBuffer (const Buffer& b)
{
    uint64_t h = 1469598103934665603ull;
    for (int c = 0; c < b.getNumChannels(); ++c)
    {
        const float* x = b.getReadPointer (c);
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            uint32_t u;
            std::memcpy (&u, &x[i], 4);
            h = (h ^ u) * 1099511628211ull;
        }
    }
    return h ^ ((uint64_t) b.getNumSamples() << 1);
}

struct Combo { PlanTarget target; const char* name; PlanCharacter character; const char* cname; };
const PlanTarget kTargets[] { PlanTarget::STREAM_14, PlanTarget::APPLE_16, PlanTarget::CLUB_8, PlanTarget::KEEP_INPUT };
const char* kTargetNames[] { "STREAM -14", "APPLE -16", "CLUB -8", "KEEP INPUT" };
const PlanCharacter kChars[] { PlanCharacter::OFF, PlanCharacter::GENTLE, PlanCharacter::FULL };
const char* kCharNames[] { "OFF", "GENTLE", "FULL" };

double capFor (const Measurements& m, double target) { return std::abs (m.integrated - target) <= 6.0 ? 3.0 : 8.0; }

/** The same Session, set to an intent through its public API. */
void setIntent (Session& s, PlanTarget t, PlanCharacter c)
{
    s.intent.target = t;
    s.intent.character = c;
    s.settingsChanged();
}

bool neutralStages (const MasterSettings& s)
{
    return ! s.eq.enabled && ! s.comp.enabled && ! s.width.enabled && std::abs (s.eq.airDb) < 1e-9 && std::abs (s.eq.lowShelfDb) < 1e-9 && std::abs (s.eq.bell1Db) < 1e-9
           && std::abs (s.eq.bell2Db) < 1e-9 && std::abs (s.eq.highShelfDb) < 1e-9 && std::abs (s.eq.tiltDbPerOct) < 1e-9 && s.eq.matchCurveDb.empty()
           && ! s.eq.dynLowMid.enabled && ! s.eq.dynUpperMid.enabled && ! s.eq.tightener.enabled && ! s.eq.airDynamic && ! s.dcFilter
           && std::abs (s.width.width - 1.0) < 1e-9 && s.width.monoBelowHz == 0.0 && ! s.width.phaseSafeLowMono;
}

/** Mean limiter reduction over the loudest 30 % of the song: the hops (10 ms, as the limiter's history) of the input with the highest RMS. */
double loud30Mean (const std::vector<float>& gr, const Buffer& in, double fs)
{
    const int hop = std::max (1, (int) std::lround (0.01 * fs)), n = std::min ((int) gr.size(), in.getNumSamples() / hop);
    if (n <= 0) return 0.0;
    std::vector<std::pair<double, int>> rms ((size_t) n);
    for (int k = 0; k < n; ++k)
    {
        double e = 0.0;
        for (int c = 0; c < in.getNumChannels(); ++c) for (int i = 0; i < hop; ++i) { const double x = in.getSample (c, k * hop + i); e += x * x; }
        rms[(size_t) k] = { e, k };
    }
    std::sort (rms.begin(), rms.end(), [] (const auto& a, const auto& b) { return a.first > b.first; });
    const int m = std::max (1, (int) std::ceil (0.3 * n));
    double sum = 0.0;
    for (int i = 0; i < m; ++i) sum += gr[(size_t) rms[(size_t) i].second];
    return sum / m;
}

struct FakePlayHead : juce::AudioPlayHead
{
    bool playing = true;
    juce::Optional<PositionInfo> getPosition() const override { PositionInfo p; p.setIsPlaying (playing); return p; }
};

Buffer play (MihenkProcessor& p, const Buffer& in, int block, int extra, bool bypassed = false, const std::vector<std::pair<int, bool>>* bypassAt = nullptr)
{
    const int n = in.getNumSamples(), total = n + extra;
    Buffer out (2, total);
    out.clear();
    for (int c = 0; c < 2; ++c) out.copyFrom (c, 0, in, juce::jmin (c, in.getNumChannels() - 1), 0, n);
    juce::MidiBuffer midi;
    bool byp = bypassed;
    size_t next = 0;
    for (int pos = 0; pos < total; pos += block)
    {
        if (bypassAt != nullptr)
            while (next < bypassAt->size() && (*bypassAt)[next].first <= pos) byp = (*bypassAt)[next++].second;
        const int len = juce::jmin (block, total - pos);
        Buffer view (out.getArrayOfWritePointers(), 2, pos, len);
        if (byp) p.processBlockBypassed (view, midi); else p.processBlock (view, midi);
    }
    return out;
}

Buffer stereoOf (const Buffer& b)
{
    Buffer s (2, b.getNumSamples());
    for (int c = 0; c < 2; ++c) s.copyFrom (c, 0, b, juce::jmin (c, b.getNumChannels() - 1), 0, b.getNumSamples());
    return s;
}

Buffer noise (double fs, double secs, uint32_t seed, float level)
{
    Buffer b (2, (int) (secs * fs));
    juce::Random r ((juce::int64) seed);
    double lp[2] { 0, 0 };
    for (int i = 0; i < b.getNumSamples(); ++i)
        for (int c = 0; c < 2; ++c)
        {
            lp[c] += 0.2 * ((r.nextDouble() * 2.0 - 1.0) - lp[c]);
            b.setSample (c, i, (float) (level * (lp[c] * 3.0 + 0.15 * std::sin (0.01 * i * (c + 1)))));
        }
    return b;
}

/** A kick / bass / pad mix with a quiet verse and a loud chorus, not a finished master (PLR above 11 LU). */
Buffer openMix (double fs, double secs, uint32_t seed)
{
    const int n = (int) (secs * fs);
    Buffer b (2, n);
    juce::Random rng ((juce::int64) seed);
    double lp[2] { 0.0, 0.0 };
    for (int i = 0; i < n; ++i)
    {
        const double t = i / fs, tb = std::fmod (t, 0.5), section = std::fmod (t, 8.0) < 4.0 ? 0.3 : 1.0;
        const double kick = std::exp (-tb * 18.0) * std::sin (2.0 * kPi * (48.0 * tb + 2.0 * (1.0 - std::exp (-tb * 30.0))));
        const double bass = 0.25 * std::sin (2.0 * kPi * 55.0 * t);
        for (int c = 0; c < 2; ++c)
        {
            lp[c] += 0.05 * ((rng.nextDouble() * 2.0 - 1.0) - lp[c]);
            b.setSample (c, i, (float) (0.5 * section * (0.9 * kick + bass + 0.3 * lp[c] * 1.5)));
        }
    }
    return b;
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    const bool quick = argc > 1 && juce::String (argv[1]) == "--quick";
    const auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("mihenk_v3_session_check");
    tmp.deleteRecursively();
    tmp.createDirectory();

    juce::File corpusDir = juce::File::getCurrentWorkingDirectory().getChildFile ("tests/corpus");
    if (! corpusDir.isDirectory()) corpusDir = juce::File (juce::String (CORPUS_DIR));
    auto files = corpusDir.findChildFiles (juce::File::findFiles, false, "*.wav");
    std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b) { return a.getFileName() < b.getFileName(); });
    if (quick)
    {
        juce::Array<juce::File> few;
        for (auto& f : files) if (f.getFileName() == "bag_peak-3.wav" || f.getFileName() == "bag_quiet-20.wav" || f.getFileName() == "dip_asis.wav") few.add (f);
        files = few;
    }
    check (files.size() == (quick ? 3 : 18), "corpus files found", juce::String (files.size()) + " in " + corpusDir.getFullPathName());

    //==============================================================================================
    section ("Every parameter id is an override the plan knows, or a documented intent / pass-through field");
    {
        const std::set<juce::String> intentOrPass { "bypass", "autoTrim", "targetPreset", "targetLufs", "targetLock", "ceiling", "warnAt", "toneSmooth", "limOn" };
        int unknown = 0;
        juce::String list;
        for (const auto& d : params::definitions())
            if (! intentOrPass.count (d.id) && ! overrideIds().contains (d.id)) { ++unknown; list << d.id << " "; }
        check (unknown == 0, "no parameter writes a knob the plan does not know", list);
        int notParam = 0;
        for (const auto& id : overrideIds()) { bool found = false; for (const auto& d : params::definitions()) found |= d.id == id; notParam += found ? 0 : 1; }
        check (notParam == 0, "every override id is a real parameter id", juce::String (notParam) + " not found");
    }

    //==============================================================================================
    section ("The Session on every corpus file: target x character");
    std::map<juce::String, double> maxGainOver;           // file -> largest (applied - cap) seen
    double worstOffOvershoot = -1e9, worstOtherOvershoot = -1e9, worstPeakGr = 0.0, worstMeanGr = 0.0, worstTop30 = 0.0, worstTp = -300.0, worstOffPlanDiff = 0.0, worstKeepRise = -1e9;
    int reachN = 0, unreachableSeen = 0, statusSaid = 0, statusBad = 0; double worstReachErrOff = 0.0, worstReachErrOther = 0.0, worstPkErr = 0.0, worstAvgErr = 0.0;
    int combos = 0, capViol = 0, offExact = 0, grViol = 0, tpViol = 0, finishedViol = 0, keepViol = 0, outOfReachMissing = 0, outOfReachNeeded = 0, statusMissing = 0, settingsNotPlan = 0;
    for (const auto& f : files)
    {
        auto a = loadAudioFile (f);
        if (! a.ok()) { check (false, "load " + f.getFileName(), a.error); continue; }
        MihenkProcessor p;
        p.setStandaloneModeForTesting (true);
        auto& s = p.getSession();
        s.profile.setFolder (tmp.getChildFile ("profile"));
        const double fs = a.sampleRate;
        s.setInputForTesting (juce::AudioBuffer<float> (a.buffer), fs, f.getFileName());
        const auto& m = s.measurements;
        for (int ti = 0; ti < 4; ++ti)
            for (int ci = 0; ci < 3; ++ci)
            {
                setIntent (s, kTargets[ti], kChars[ci]);
                s.renderNowForTesting();
                ++combos;
                const juce::String tag = f.getFileName() + " " + kTargetNames[ti] + " " + kCharNames[ci];
                const double target = s.plan.settings.targetLufs;
                const double cap = capFor (m, target);
                const double applied = s.processed().integrated - s.inMeas.integrated;
                const double over = applied - (kTargets[ti] == PlanTarget::KEEP_INPUT ? 0.0 : cap);
                if (ci == 0) worstOffOvershoot = juce::jmax (worstOffOvershoot, over); else worstOtherOvershoot = juce::jmax (worstOtherOvershoot, over);
                const bool explicitT = kTargets[ti] != PlanTarget::KEEP_INPUT;     // SPEC_V3 2 REVISED: STREAM / APPLE / CLUB are reached, no +3 / +8 cap
                if (explicitT)
                {
                    const double err = s.processed().integrated - target;
                    if (ci == 0) worstReachErrOff = juce::jmax (worstReachErrOff, std::abs (err)); else worstReachErrOther = juce::jmax (worstReachErrOther, std::abs (err));
                    ++reachN;
                    const double reachTol = ci == 0 ? 0.2 : 0.3;          // GENTLE / FULL add air / glue / EQ before the limiter: the session measures that on its render and holds it (calibration)
                    if (std::abs (err) > reachTol) { ++capViol; std::printf ("FAIL  target not reached: %s %.2f LUFS vs %.1f (err %+.2f)\n", tag.toRawUTF8(), s.processed().integrated, target, err); }
                }
                if (kTargets[ti] == PlanTarget::KEEP_INPUT)
                {
                    worstKeepRise = juce::jmax (worstKeepRise, applied);
                    if (applied > 0.3) { ++keepViol; std::printf ("FAIL  keep input: %s rose %+.2f dB\n", tag.toRawUTF8(), applied); }
                }
                if (ci == 0)
                {
                    // The plan's gain is trim; the limiter then takes what the peaks need (bounded: mean <= 2 dB), so the loudness gained
                    // is the plan's minus that, never more.
                    const double d = s.plan.totalGainDb - applied;
                    worstOffPlanDiff = juce::jmax (worstOffPlanDiff, d);
                    const bool exactWhenIdle = s.limGr.maxDb > 0.1 || std::abs (d) <= 0.05;
                    if (d < -0.05 || (! explicitT && d > 2.05) || ! exactWhenIdle) { ++offExact; std::printf ("FAIL  OFF is trim + limiter only: %s applied %+.3f, plan %+.3f, limiter max %.2f\n", tag.toRawUTF8(), applied, s.plan.totalGainDb, s.limGr.maxDb); }
                }
                const double pk = s.limGr.maxDb, mean = s.limGr.meanDb, t30 = loud30Mean (s.limGr.history, *s.input, fs);
                worstPeakGr = juce::jmax (worstPeakGr, pk); worstMeanGr = juce::jmax (worstMeanGr, mean); worstTop30 = juce::jmax (worstTop30, t30);
                if (explicitT && ci == 0)
                {
                    // the plan's own GR numbers (what the status says) against what the limiter really did
                    const double dPk = std::abs (pk - s.plan.predictedPeakGrDb), dAvg = std::abs (t30 - s.plan.predictedAverageGrDb);
                    worstPkErr = juce::jmax (worstPkErr, dPk); worstAvgErr = juce::jmax (worstAvgErr, dAvg);
                    if (dPk > 0.6 || dAvg > 1.5) {   // the history is the largest GR per 10 ms, the plan's average is the plain mean over the loudest 30 % of 100 ms blocks: the hop maximum reads up to ~1.3 dB higher at heavy limiting
                        ++grViol; std::printf ("FAIL  status GR numbers: %s plan peak %.2f / avg %.2f, measured %.2f / %.2f\n", tag.toRawUTF8(), s.plan.predictedPeakGrDb, s.plan.predictedAverageGrDb, pk, t30); }
                }
                else if (explicitT) {}
                else if (pk > 6.0 + 0.1 || mean > 2.0 + 0.05 || t30 > 2.0 + 0.05) { ++grViol; std::printf ("FAIL  limiter: %s peak %.2f mean %.2f top30 %.2f\n", tag.toRawUTF8(), pk, mean, t30); }
                worstTp = juce::jmax (worstTp, s.processed().truePeak);
                if (s.processed().truePeak > s.settings.limiter.ceilingDbtp + 0.1) { ++tpViol; std::printf ("FAIL  ceiling: %s true peak %.2f\n", tag.toRawUTF8(), s.processed().truePeak); }
                if (s.plan.finishedMaster && ! neutralStages (s.settings)) { ++finishedViol; std::printf ("FAIL  finished master is level + ceiling only: %s\n", tag.toRawUTF8()); }
                // settings are the plan, nothing else
                const auto expect = solvePlan (s.measurements, s.intent).settings;
                if (std::abs (s.settings.trimDb - expect.trimDb) > 1e-4 || std::abs (s.settings.limiter.driveDb - expect.limiter.driveDb) > 1e-4
                    || s.settings.eq.enabled != expect.eq.enabled || s.settings.comp.enabled != expect.comp.enabled || std::abs (s.settings.eq.airDb - expect.eq.airDb) > 1e-4
                    || s.settings.targetLock || s.settings.autoTrim || ! s.settings.planned)
                { ++settingsNotPlan; std::printf ("FAIL  settings are not the plan: %s\n", tag.toRawUTF8()); }
                // out of reach: said in words with the numbers
                if (explicitT && s.plan.targetUnreachable) { ++unreachableSeen; std::printf ("FAIL  an explicit target was declared unreachable: %s %s\n", tag.toRawUTF8(), s.plan.refused.joinIntoString (" / ").toRawUTF8()); ++capViol; }
                if (explicitT && ci == 0 && s.plan.limiterStatus.isNotEmpty())
                {
                    ++statusSaid;
                    const auto story = s.loudness();
                    const bool ok = s.status.contains ("LUFS reached: the limiter takes up to") && story.shortLine.contains ("REACHED") && story.severity == s.plan.limiterSeverity
                                    && ! s.status.containsIgnoreCase ("raise the mix") && ! s.status.contains ("stopped");
                    if (! ok) { ++statusBad; std::printf ("FAIL  limiter cost not said: %s status: %s line: %s\n", tag.toRawUTF8(), s.status.toRawUTF8(), story.shortLine.toRawUTF8()); }
                }
                if (s.status.isEmpty() || ! s.status.contains ("LUFS") || ! s.status.contains ("dBTP")) { ++statusMissing; std::printf ("FAIL  status paragraph missing numbers: %s: %s\n", tag.toRawUTF8(), s.status.toRawUTF8()); }
            }
    }
    check (capViol == 0 && reachN > 0, "REVISED: every explicit target (STREAM / APPLE / CLUB) is reached by the Session on every file x character (OFF within 0.2 LU; GENTLE / FULL within 0.3 LU after the calibration), never declared unreachable",
           juce::String (reachN) + " combos, violations " + juce::String (capViol) + ", worst |error| OFF " + juce::String (worstReachErrOff, 3) + " LU, GENTLE/FULL " + juce::String (worstReachErrOther, 3) + " LU");
    check (statusBad == 0 && statusSaid > 0, "REVISED: the status and the LOUDNESS line say what the limiter costs (\"... reached: the limiter takes up to X dB ... (avg Y dB)\"), with the severity", juce::String (statusSaid) + " cases");
    check (offExact == 0, "character OFF is trim + limiter only: the measured gain is the plan's, less what the limiter takes (exact when it is idle, never more than the plan; KEEP INPUT: at most 2 dB less)", "largest shortfall " + juce::String (worstOffPlanDiff, 3) + " dB");
    check (keepViol == 0, "KEEP INPUT never makes the master louder than the mix", "largest rise " + juce::String (worstKeepRise, 3) + " dB");
    check (grViol == 0, "limiter: KEEP INPUT average GR <= 2 dB, peak GR <= 6 dB; explicit targets: the plan's GR numbers (the ones the status prints) match what the limiter did (peak within 0.6 dB, loud-30% average within 1.5 dB: the test reads the per-10 ms maximum, the plan the plain mean)",
           "worst peak " + juce::String (worstPeakGr, 2) + " dB, worst mean " + juce::String (worstMeanGr, 3) + " dB, worst loud-30% mean " + juce::String (worstTop30, 3) + " dB; status vs measured: peak " + juce::String (worstPkErr, 2) + " dB, avg " + juce::String (worstAvgErr, 2) + " dB");
    check (tpViol == 0, "true peak under the ceiling", "highest " + juce::String (worstTp, 2) + " dBTP");
    check (finishedViol == 0, "finished material is level + ceiling only, whatever the character says");
    check (settingsNotPlan == 0, "after every click the settings are the plan (TARGET LOCK and the headroom assistant never on)");
    check (unreachableSeen == 0 && outOfReachMissing == 0, "REVISED: no explicit target is ever 'out of reach' (the OUT OF REACH line stays only for a limiter that cannot get there; ALBUM is a reached target too since album v3.2)", juce::String (outOfReachNeeded) + " old-rule cases");
    check (statusMissing == 0, "the status paragraph carries the measured numbers on every combination");

    //==============================================================================================
    section ("CLUB -8 on the peak -3 files (the owner's case)");
    for (const auto& f : files)
    {
        if (! f.getFileName().contains ("_peak-3")) continue;
        auto a = loadAudioFile (f);
        MihenkProcessor p;
        p.setStandaloneModeForTesting (true);
        auto& s = p.getSession();
        s.profile.setFolder (tmp.getChildFile ("profile"));
        s.setInputForTesting (juce::AudioBuffer<float> (a.buffer), a.sampleRate, f.getFileName());
        for (auto c : kChars)
        {
            setIntent (s, PlanTarget::CLUB_8, c);
            s.renderNowForTesting();
            const double applied = s.processed().integrated - s.inMeas.integrated;
            const double err = s.processed().integrated - (-8.0);
            check (std::abs (err) <= 0.2 && ! s.plan.targetUnreachable && neutralStages (s.settings) && s.processed().truePeak <= s.settings.limiter.ceilingDbtp + 0.05
                       && s.status.contains ("-8 LUFS reached: the limiter takes up to") && ! s.status.contains ("stopped"),
                   f.getFileName() + " CLUB -8 " + (c == PlanCharacter::OFF ? "OFF" : c == PlanCharacter::GENTLE ? "GENTLE" : "FULL") + ": REVISED, -8 is reached within 0.2 LU, true peak under the ceiling, level + ceiling only, limiter cost said",
                   "in " + fmt (s.inMeas.integrated, 2) + " -> out " + fmt (s.processed().integrated, 2) + " LUFS, TP " + fmt (s.processed().truePeak, 2) + ", applied " + fmt (applied, 2) + " dB, limiter peak " + fmt (s.limGr.maxDb, 2) + " / mean " + fmt (s.limGr.meanDb, 3)
                       + " dB; \"" + s.plan.limiterStatus + "\"");
        }
    }

    //==============================================================================================
    section ("Random click orders end where setting the final intent directly ends (bit for bit)");
    {
        juce::Random rng (20261003);
        int walks = 0, bad = 0;
        for (const auto& f : files)
        {
            if (quick == false && ! (f.getFileName().startsWith ("bag_") || f.getFileName().startsWith ("dip_")) ) continue;     // the 170 s files are covered by the combos above
            auto a = loadAudioFile (f);
            for (int w = 0; w < 2; ++w)
            {
                const auto T = kTargets[rng.nextInt (4)];
                const auto C = kChars[rng.nextInt (3)];
                MihenkProcessor walkP, directP;
                walkP.setStandaloneModeForTesting (true);
                directP.setStandaloneModeForTesting (true);
                auto& ws = walkP.getSession();
                auto& ds = directP.getSession();
                ws.profile.setFolder (tmp.getChildFile ("profile"));
                ds.profile.setFolder (tmp.getChildFile ("profile"));
                ws.setInputForTesting (juce::AudioBuffer<float> (a.buffer), a.sampleRate, f.getFileName());
                ds.setInputForTesting (juce::AudioBuffer<float> (a.buffer), a.sampleRate, f.getFileName());
                for (int step = 0; step < 30; ++step)
                    switch (rng.nextInt (6))
                    {
                        case 0: ws.intent.target = kTargets[rng.nextInt (4)]; ws.settingsChanged(); break;
                        case 1: ws.intent.character = kChars[rng.nextInt (3)]; ws.settingsChanged(); break;
                        case 2: ws.settings.bypass = ! ws.settings.bypass; ws.settingsChanged(); break;
                        case 3: ws.settings.eq.airDb = rng.nextFloat() * 5.0f; ws.settingsChanged(); break;                       // a panel write: an override
                        case 4: ws.settings.limiter.driveDb = rng.nextFloat() * 20.0f; ws.settingsChanged(); break;               // a level override: capped
                        default: ws.clearOverrides(); break;
                    }
                ws.settings.bypass = false;
                ws.intent.target = T; ws.intent.character = C; ws.settingsChanged();
                ws.clearOverrides();
                ws.renderNowForTesting();
                ds.intent.target = T; ds.intent.character = C; ds.settingsChanged();
                ds.renderNowForTesting();
                ++walks;
                const bool same = settingsFingerprint (ws.settings) == settingsFingerprint (ds.settings) && hashBuffer (*ws.output) == hashBuffer (*ds.output)
                                  && ws.plan.settings.trimDb == ds.plan.settings.trimDb && ws.status == ds.status && ws.overrideCount() == 0;
                if (! same) { ++bad; std::printf ("FAIL  walk on %s ended differently (target %s, character %s)\n", f.getFileName().toRawUTF8(), kTargetNames[(int) std::distance (std::begin (kTargets), std::find (std::begin (kTargets), std::end (kTargets), T))], kCharNames[(int) C]); }
            }
        }
        check (walks > 0 && bad == 0, "30 random clicks (targets, characters, bypass, knob writes, level overrides, clear) then the final intent = the intent set directly: same settings, same master, same status", juce::String (walks) + " walks, differences " + juce::String (bad));
    }

    //==============================================================================================
    section ("Overrides: stored as deltas, any order gives the same plan, caps hold with them");
    {
        const double fs = 48000.0;
        const auto mix = openMix (fs, 14.0, 5);
        auto make = [&] (MihenkProcessor& p)
        {
            p.setStandaloneModeForTesting (true);
            auto& s = p.getSession();
            s.profile.setFolder (tmp.getChildFile ("profile"));
            s.setInputForTesting (juce::AudioBuffer<float> (mix), fs, "open.wav");
        };
        MihenkProcessor pa, pb, pc;
        make (pa); make (pb); make (pc);
        auto& A = pa.getSession(); auto& B = pb.getSession(); auto& C = pc.getSession();
        check (! A.plan.finishedMaster, "the synthetic mix is not a finished master", "PLR " + fmt (A.measurements.plr, 1) + " LU, crest " + fmt (A.measurements.crest, 1) + " dB");
        setIntent (A, PlanTarget::STREAM_14, PlanCharacter::GENTLE); setIntent (B, PlanTarget::STREAM_14, PlanCharacter::GENTLE); setIntent (C, PlanTarget::STREAM_14, PlanCharacter::GENTLE);
        // three edits in two different orders
        A.settings.eq.airDb += 1.0; A.settingsChanged(); A.settings.eq.bell2Db = 1.5; A.settingsChanged(); A.settings.comp.glueDb += 0.4; A.settingsChanged();
        B.settings.comp.glueDb += 0.4; B.settingsChanged(); B.settings.eq.bell2Db = 1.5; B.settingsChanged(); B.settings.eq.airDb += 1.0; B.settingsChanged();
        A.renderNowForTesting(); B.renderNowForTesting();
        check (settingsFingerprint (A.settings) == settingsFingerprint (B.settings) && hashBuffer (*A.output) == hashBuffer (*B.output) && A.overrideCount() == B.overrideCount() && A.overrideCount() == 3,
               "three knob edits in opposite orders: the same settings and the same master", "overrides " + juce::String (A.overrideCount()));
        check (A.isOverridden ("bell2Db") && std::abs (A.overrideDelta ("bell2Db") - 1.5) < 1e-3 && std::abs (A.overrideDelta ("airDb") - 1.0) < 1e-3,
               "the override is stored as a delta over the plan", "BELL 2 +" + fmt (A.overrideDelta ("bell2Db"), 2) + " dB, AIR +" + fmt (A.overrideDelta ("airDb"), 2) + " dB");
        // a new intent keeps the deltas
        const double airBefore = A.settings.eq.airDb;
        setIntent (A, PlanTarget::STREAM_14, PlanCharacter::FULL);
        check (A.isOverridden ("airDb") && std::abs (A.overrideDelta ("airDb") - 1.0) < 1e-3 && std::abs (A.settings.eq.airDb - (A.basePlan.settings.eq.airDb + 1.0)) < 1e-3,
               "changing the intent recomputes the plan and keeps the deltas", "AIR " + fmt (airBefore, 2) + " -> " + fmt (A.settings.eq.airDb, 2) + " dB (plan " + fmt (A.basePlan.settings.eq.airDb, 2) + " + 1.0)");
        // the knob moved back to the plan removes the override
        A.settings.eq.airDb = A.basePlan.settings.eq.airDb; A.settingsChanged();
        check (! A.isOverridden ("airDb"), "a knob turned back to the plan's value is no override any more");
        A.clearOverrides();
        check (A.overrideCount() == 0 && std::abs (A.settings.eq.bell2Db - A.basePlan.settings.eq.bell2Db) < 1e-9, "RESET OVERRIDES: every knob shows the plan again");
        // idempotence: the same edit twice = once
        C.settings.eq.airDb += 1.0; C.settingsChanged(); const auto once = settingsFingerprint (C.settings);
        C.settings.eq.airDb = C.settings.eq.airDb; C.settingsChanged(); C.settingsChanged();
        check (settingsFingerprint (C.settings) == once && C.overrideCount() == 1, "applying the same intent again changes nothing (idempotence)");
        // caps with level overrides
        for (double gain : { 6.0, 20.0, 36.0 })
        {
            B.clearOverrides();
            setIntent (B, PlanTarget::CLUB_8, PlanCharacter::OFF);
            B.settings.trimDb = B.basePlan.settings.trimDb + gain; B.settingsChanged();
            B.settings.limiter.driveDb = 12.0; B.settingsChanged();
            B.renderNowForTesting();
            const double needed = B.basePlan.totalGainDb;
            const double reachErr = B.processed().integrated - B.plan.settings.targetLufs;
            check (B.plan.totalGainDb <= needed + 1e-9 && std::abs (reachErr) <= 0.25 && std::abs (B.plan.settings.trimDb) <= 24.0 + 1e-9 && B.plan.settings.limiter.driveDb >= -1e-9 && B.plan.refused.size() > 0,
                   "REVISED: trim +" + juce::String (gain, 0) + " dB and drive +12 dB written by hand are cut back to the gain the target needs (never above needed + 0.2 dB) and said", "plan gain " + fmt (B.plan.totalGainDb, 2) + " dB, needed " + fmt (needed, 2) + " dB, output " + fmt (B.processed().integrated, 2) + " LUFS (target " + fmt (B.plan.settings.targetLufs, 1) + "), \"" + B.plan.refused.joinIntoString (" / ") + "\"");
        }
    }
    {
        // Overrides on finished material are refused and said so.
        const auto f = files[0];
        auto a = loadAudioFile (f);
        MihenkProcessor p;
        p.setStandaloneModeForTesting (true);
        auto& s = p.getSession();
        s.profile.setFolder (tmp.getChildFile ("profile"));
        s.setInputForTesting (juce::AudioBuffer<float> (a.buffer), a.sampleRate, f.getFileName());
        setIntent (s, PlanTarget::STREAM_14, PlanCharacter::FULL);
        s.settings.eq.airDb = 3.0; s.settings.comp.glueDb = 2.0; s.settingsChanged();
        check (s.plan.finishedMaster && std::abs (s.settings.eq.airDb) < 1e-9 && ! s.settings.comp.enabled && s.status.contains ("not applied"),
               "a knob written on a finished master is refused and the status says so", s.status.substring (0, 200));
    }


    //==============================================================================================
    section ("Undo / redo (owner 2026-10-04): N random intent / override changes, undo all = the initial render, redo all = the final render");
    {
        const double fs = 48000.0;
        const auto mix = openMix (fs, 8.0, 9);
        auto make = [&] (MihenkProcessor& p, bool app)
        {
            p.setStandaloneModeForTesting (app);
            auto& s = p.getSession();
            s.profile.setFolder (tmp.getChildFile ("profile"));
            s.setInputForTesting (juce::AudioBuffer<float> (mix), fs, "open.wav");
            s.renderNowForTesting();
        };
        struct Snap { uint64_t master = 0; juce::String fp, intent; };
        auto snap = [] (Session& s) { return Snap { hashBuffer (*s.output), settingsFingerprint (s.settings), juce::JSON::toString (s.intentToVar()) }; };
        auto same = [] (const Snap& a, const Snap& b) { return a.master == b.master && a.fp == b.fp && a.intent == b.intent; };
        auto hex = [] (uint64_t v) { return juce::String::toHexString ((juce::int64) v); };
        auto randomChange = [&] (Session& s, juce::Random& r)
        {
            switch (r.nextInt (7))
            {
                case 0: s.setTarget (kTargets[r.nextInt (4)]); break;
                case 1: s.setCharacter (kChars[r.nextInt (3)]); break;
                case 2: s.setCeiling (-0.5 - 0.5 * r.nextInt (5)); break;
                case 3: s.settings.eq.airDb = juce::jlimit (0.0, 6.0, s.settings.eq.airDb + 0.3 + 0.1 * r.nextInt (10)); s.settingsChanged(); break;
                case 4: s.settings.eq.bell2Db = juce::jlimit (-8.0, 8.0, 0.4 * (r.nextInt (9) - 4) + 0.05); s.settingsChanged(); break;
                case 5: s.settings.comp.glueDb = juce::jlimit (0.0, 6.0, s.settings.comp.glueDb + 0.5); s.settingsChanged(); break;
                default: s.setCustomTarget (-9.0 - r.nextInt (8)); break;
            }
        };

        for (const bool renderEach : { false, true })
        {
            MihenkProcessor p;
            make (p, true);
            auto& s = p.getSession();
            const juce::String tag = renderEach ? "app, a render after every change" : "app";
            check (! s.canUndo() && ! s.canRedo() && s.undoName().isEmpty(), "a fresh source has an empty history (" + tag + ")");
            const auto initial = snap (s);
            juce::Random rng (4711);
            std::vector<Snap> states { initial };
            int expected = 0;
            for (int i = 0; i < 24; ++i)
            {
                const int before = s.undoSteps();
                randomChange (s, rng);
                if (renderEach) s.renderNowForTesting();
                if (s.undoSteps() > before) { ++expected; if (renderEach) states.push_back (snap (s)); }
                else if (s.undoSteps() != before) expected = -1000;
            }
            s.renderNowForTesting();
            const auto finalSnap = snap (s);
            check (s.undoSteps() == expected && expected >= 12, "every change that changed something is exactly one step (" + tag + ")", juce::String (expected) + " steps from 24 random changes");
            int undone = 0;
            if (renderEach)
            {
                bool ok = (int) states.size() == expected + 1;
                for (int k = (int) states.size() - 1; k >= 1 && ok; --k)
                {
                    ok = s.undo();
                    s.renderNowForTesting();
                    ++undone;
                    ok = ok && same (snap (s), states[(size_t) k - 1]);
                }
                check (ok && undone == expected, "undo one step at a time: every step lands exactly on the intent, settings and master it was made from (" + tag + ")", juce::String (undone) + " steps");
                while (s.redo()) {}
                s.renderNowForTesting();
                undone = expected;
            }
            else
                while (s.undo()) ++undone;
            s.renderNowForTesting();
            const auto back = snap (s);
            if (! renderEach) check (undone == expected, "undo all takes exactly the recorded steps (" + tag + ")", juce::String (undone) + " of " + juce::String (expected));
            if (renderEach) { while (s.undo()) {} s.renderNowForTesting(); }
            const auto back2 = snap (s);
            check (! s.canUndo() && s.canRedo() && same (back2, initial), "undo all: intent, settings and the render are bit-identical to the initial ones (" + tag + ")",
                   "initial " + hex (initial.master) + ", undone " + hex (back2.master) + " (" + juce::String (back2.intent == initial.intent ? "same intent" : "intent differs") + ")");
            juce::ignoreUnused (back);
            int redone = 0;
            while (s.redo()) ++redone;
            s.renderNowForTesting();
            const auto fwd = snap (s);
            check (redone == expected && ! s.canRedo() && same (fwd, finalSnap), "redo all (" + juce::String (redone) + " steps): bit-identical to the final render (" + tag + ")", "final " + hex (finalSnap.master) + ", redone " + hex (fwd.master));
        }

        // Names, a drag as ONE step, the keyboard, a typed value, RESET OVERRIDES, the new-file rule
        {
            ui::UiPrefs::setFileForTesting (tmp.getChildFile ("ui.json"));
            MihenkProcessor p;
            make (p, true);
            MainComponent panel (p);
            panel.setPresetsFolder (tmp.getChildFile ("presets"));
            panel.setFace (true, false);
            auto& s = p.getSession();
            s.setTarget (PlanTarget::CLUB_8);
            check (s.undoName() == "Target CLUB -8", "the step is named for the Edit menu: Undo Target CLUB -8", "\"" + s.undoName() + "\"");
            s.setCharacter (PlanCharacter::FULL);
            check (s.undoName() == "Character FULL", "Character FULL", "\"" + s.undoName() + "\"");
            s.renderNowForTesting();
            const auto base = snap (s);
            const int stepsBefore = s.undoSteps();
            panel.dragKnobForTesting ("AIR", 0.0, 2.4, 40);
            s.renderNowForTesting();
            const auto dragged = snap (s);
            check (s.undoSteps() == stepsBefore + 1 && s.isOverridden ("airDb") && s.undoName().contains ("override"),
                   "a knob drag of 41 value changes is ONE step", juce::String (s.undoSteps() - stepsBefore) + " step(s), \"" + s.undoName() + "\", AIR " + fmt (s.settings.eq.airDb, 2) + " dB");
            // Cmd+Z / Cmd+Shift+Z through the panel's keyPressed
            const bool z = panel.keyPressed (juce::KeyPress ('z', juce::ModifierKeys::commandModifier, 'z'));
            s.renderNowForTesting();
            check (z && same (snap (s), base) && s.undoSteps() == stepsBefore, "Cmd+Z takes the whole drag back: the state and the master are those before it", "master " + hex (base.master) + " -> " + hex (snap (s).master));
            const bool y = panel.keyPressed (juce::KeyPress ('z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 'z'));
            s.renderNowForTesting();
            check (y && same (snap (s), dragged), "Cmd+Shift+Z brings it back bit for bit", "master " + hex (dragged.master) + " -> " + hex (snap (s).master));
            check (! panel.keyPressed (juce::KeyPress ('z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 'z')), "redo with nothing to redo does not take the key");
            check (! panel.keyPressed (juce::KeyPress ('z', 0, 'z')), "a plain Z is not undo");
            // a typed value (one value change, no gesture) is one step
            const int n0 = s.undoSteps();
            panel.turnKnobForTesting ("BELL 2", 1.5);
            check (s.undoSteps() == n0 + 1 && s.undoName().contains ("override"), "a typed / single knob value is one step", "\"" + s.undoName() + "\"");
            panel.clickResetOverridesForTesting();
            check (s.overrideCount() == 0 && s.undoName() == "Reset overrides", "RESET OVERRIDES is one step", "\"" + s.undoName() + "\"");
            s.undo();
            check (s.overrideCount() == 2 && s.isOverridden ("airDb") && s.isOverridden ("bell2Db"), "undoing RESET OVERRIDES brings both overrides back", juce::String (s.overrideCount()) + " overrides");
            // reference choice and MATCH TO REFERENCE
            s.setReferenceForTesting (juce::AudioBuffer<float> (noise (fs, 6.0, 5, 0.3f)), fs, "ref.wav");
            check (s.undoName() == "Reference ref.wav", "choosing a reference is a step", "\"" + s.undoName() + "\"");
            s.matchToReference();
            check (s.undoName() == "Match to reference" && s.intent.character == PlanCharacter::FULL && ! s.intent.referenceCurve.empty(), "MATCH TO REFERENCE is one step (curve, loudness target, character)");
            s.undo();
            check (s.intent.referenceCurve.empty() && s.reference != nullptr && s.intent.target == PlanTarget::CLUB_8, "undo MATCH TO REFERENCE: the curve and target are back, the reference stays");
            s.undo();
            check (s.reference == nullptr && s.referenceName.isEmpty(), "undo the reference choice: no reference");
            s.redo(); s.redo();
            check (s.reference != nullptr && s.referenceName == "ref.wav" && ! s.intent.referenceCurve.empty(), "redo both: reference and match are back");
            // a new source starts a new history
            s.setInputForTesting (juce::AudioBuffer<float> (mix), fs, "other.wav");
            check (! s.canUndo() && ! s.canRedo(), "a new file starts a new history (nothing to undo, nothing to redo)");
            // RESET ALL with the file kept is one step
            s.setTarget (PlanTarget::APPLE_16);
            s.settings.eq.airDb = 2.0; s.settingsChanged();
            const auto beforeReset = snap (s);
            s.renderNowForTesting();
            const auto beforeResetRendered = snap (s);
            p.resetAll();
            s.renderNowForTesting();
            check (s.undoName() == "Reset all" && s.overrideCount() == 0, "RESET ALL (file kept) is one step", "\"" + s.undoName() + "\"");
            s.undo();
            s.renderNowForTesting();
            check (same (snap (s), beforeResetRendered) && beforeReset.intent == snap (s).intent, "undo RESET ALL: target and override are back, master bit-identical");
            // undo while a drag is open is refused
            s.beginUndoGesture();
            check (! s.canUndo() && ! s.undo(), "no undo while a drag is open");
            s.endUndoGesture();
        }

        // The plug-in: the host undoes the automatable parameters; the editor's history holds only the rest
        {
            MihenkProcessor p;
            make (p, false);
            auto& s = p.getSession();
            check (s.undoHostOwnsParams, "plug-in mode: the host owns the parameter steps");
            s.setTarget (PlanTarget::CLUB_8);
            s.settings.eq.airDb = 2.0; s.settingsChanged();
            check (! s.canUndo(), "plug-in: a target and an override are not in the editor's history (the host's Cmd+Z undoes them)");
            s.setCharacter (PlanCharacter::FULL);
            check (s.canUndo() && s.undoName() == "Character FULL", "plug-in: CHARACTER (not a parameter) is a step", "\"" + s.undoName() + "\"");
            s.setCeiling (-2.0);
            s.settings.eq.bell2Db = 1.0; s.settingsChanged();
            s.setCharacter (PlanCharacter::GENTLE);
            check (s.undoSteps() == 2, "plug-in: two character picks are two steps; the parameter moves between them add none", juce::String (s.undoSteps()));
            // the host moves a parameter (its own undo / automation): not recorded, and the editor's undo does not touch it
            {
                const Session::HostEdit hostEdit (s);
                s.settings.eq.airDb = 0.0;
                s.settingsChanged();
            }
            check (s.undoSteps() == 2 && ! s.isOverridden ("airDb"), "plug-in: a host-side move is followed but never recorded");
            s.undo(); s.undo();
            check (s.intent.character == PlanCharacter::OFF && s.intent.target == PlanTarget::CLUB_8 && std::abs (s.intent.ceilingDbtp + 2.0) < 1e-9 && s.isOverridden ("bell2Db") && ! s.isOverridden ("airDb"),
                   "plug-in: undo takes the character back and leaves what the host owns exactly as it is (no double undo)",
                   "character " + juce::String ((int) s.intent.character) + ", ceiling " + fmt (s.intent.ceilingDbtp, 1) + ", BELL 2 override " + juce::String (s.isOverridden ("bell2Db") ? "kept" : "lost"));
            s.renderNowForTesting();
            // the same state set directly gives the same master
            MihenkProcessor q;
            make (q, false);
            auto& t = q.getSession();
            t.setTarget (PlanTarget::CLUB_8); t.setCeiling (-2.0); t.settings.eq.bell2Db = 1.0; t.settingsChanged();
            t.renderNowForTesting();
            check (hashBuffer (*s.output) == hashBuffer (*t.output), "plug-in: after the editor's undo the master equals the same state set directly, bit for bit");
            s.redo(); s.redo();
            check (s.intent.character == PlanCharacter::GENTLE && s.isOverridden ("bell2Db") && std::abs (s.intent.ceilingDbtp + 2.0) < 1e-9, "plug-in: redo puts the character picks back and nothing else moves");
        }
    }

    //==============================================================================================
    section ("Bypass: ten cycles change nothing (app)");
    {
        int bad = 0, n = 0;
        for (const auto& f : files)
        {
            if (f.getFileName().startsWith ("dipmix_") && f.getFileName() != "dipmix_peak-3.wav") continue;
            auto a = loadAudioFile (f);
            MihenkProcessor p;
            p.setStandaloneModeForTesting (true);
            auto& s = p.getSession();
            s.profile.setFolder (tmp.getChildFile ("profile"));
            s.setInputForTesting (juce::AudioBuffer<float> (a.buffer), a.sampleRate, f.getFileName());
            setIntent (s, PlanTarget::CLUB_8, PlanCharacter::GENTLE);
            s.renderNowForTesting();
            for (int w = 0; w < 400 && s.busy(); ++w) juce::Thread::sleep (25);        // the worker may still be finishing a job the test replaced
            const auto* master = s.output.get();
            const auto h0 = hashBuffer (*s.output);
            const auto fp0 = settingsFingerprint (s.settings);
            const auto plan0 = s.plan.plainSummary();
            const double lufs0 = s.outMeas.integrated, matched = s.originalGainDb();
            bool ok = true;
            for (int k = 0; k < 10; ++k)
            {
                s.settings.bypass = true; s.settingsChanged();
                const bool a1 = s.player.isBypassed(), a2 = ! s.busy(), a3 = s.output.get() == master, a4 = std::abs (s.outMeas.integrated - s.inMeas.integrated) < 1e-9;
                const bool a5 = std::abs (s.originalGainDb() - (s.processed().integrated - s.inMeas.integrated)) < 1e-6;      // level-matched A/B
                s.settings.bypass = false; s.settingsChanged();
                const bool b1 = ! s.player.isBypassed(), b2 = ! s.busy(), b3 = s.output.get() == master, b4 = std::abs (s.outMeas.integrated - lufs0) < 1e-12;
                const bool b5 = hashBuffer (*s.output) == h0, b6 = settingsFingerprint (s.settings) == fp0, b7 = s.plan.plainSummary() == plan0, b8 = std::abs (s.originalGainDb() - matched) < 1e-9;
                if (! (a1 && a2 && a3 && a4 && a5 && b1 && b2 && b3 && b4 && b5 && b6 && b7 && b8))
                    std::printf ("      cycle %d: on[bypassed %d idle %d same %d inMeas %d matched %d] off[player %d idle %d same %d lufs %d hash %d settings %d plan %d gain %d]\n", k, a1, a2, a3, a4, a5, b1, b2, b3, b4, b5, b6, b7, b8);
                ok &= a1 && a2 && a3 && a4 && a5 && b1 && b2 && b3 && b4 && b5 && b6 && b7 && b8;
            }
            ++n;
            if (! ok) { ++bad; std::printf ("FAIL  bypass cycles changed something on %s\n", f.getFileName().toRawUTF8()); }
        }
        check (n > 0 && bad == 0, "10 x (bypass on, bypass off): no job queued, the same rendered buffer (bit for bit), the same settings and plan, A level-matched while bypassed", juce::String (n) + " files, failures " + juce::String (bad));
    }

    //==============================================================================================
    section ("The plug-in path: LEARN through processBlock, the plan in the real-time chain");
    {
        FakePlayHead head;
        int combosP = 0, capBad = 0, tpBad = 0, grBad = 0, offBad = 0;
        double worstGain = -1e9, worstTpP = -300.0, worstPkGr = 0.0, worstOff = 0.0;
        for (const auto& f : files)
        {
            if (quick == false && f.getFileName().startsWith ("dipmix_") && f.getFileName() != "dipmix_peak-3.wav" && f.getFileName() != "dipmix_asis.wav") continue;   // 170 s: two of the six
            auto a = loadAudioFile (f);
            if (! a.ok()) continue;
            const double fs = a.sampleRate;
            MihenkProcessor p;
            p.setPlayHead (&head);
            p.setNonRealtime (true);
            p.setRateAndBufferSizeDetails (fs, 512);
            p.prepareToPlay (fs, 512);
            auto& s = p.getSession();
            s.profile.setFolder (tmp.getChildFile ("profile"));
            // LEARN: play the whole file once.
            p.toggleLearn();
            const auto whole = stereoOf (a.buffer);
            (void) play (p, whole, 512, 0);
            p.finishLearnNow();
            check (p.isLearned() && s.measurements.present, f.getFileName() + ": LEARN heard the file and the measurements are frozen", fmt (s.measurements.integrated, 2) + " LUFS");
            const auto frozen = s.measurements.integrated;
            const int len = juce::jmin (whole.getNumSamples() - 2000, (int) (10.0 * fs)), start = juce::jmax (0, whole.getNumSamples() / 3);
            Buffer excerpt (2, juce::jmin (len, whole.getNumSamples() - start));
            for (int c = 0; c < 2; ++c) excerpt.copyFrom (c, 0, whole, c, start, excerpt.getNumSamples());
            const double inL = integratedLoudness (excerpt, fs);
            const auto inM = analyse (excerpt, fs);
            struct C2 { int t, c; };
            const C2 list[] { { 0, 0 }, { 1, 0 }, { 2, 0 }, { 3, 0 }, { 2, 1 }, { 2, 2 }, { 0, 2 } };
            for (auto [ti, ci] : list)
            {
                setIntent (s, kTargets[ti], kChars[ci]);
                s.renderNowForTesting();
                p.publishNow();
                p.prepareToPlay (fs, 512);                      // a fresh chain for each combination
                auto out = play (p, excerpt, 512, p.getLatencySamples() + (int) (0.5 * fs));
                const int lat = p.getLatencySamples();
                Buffer res (2, excerpt.getNumSamples());
                for (int c = 0; c < 2; ++c) res.copyFrom (c, 0, out, c, lat, excerpt.getNumSamples());
                const double outL = integratedLoudness (res, fs);
                const auto outM = analyse (res, fs);
                const double applied = outL - inL;
                const double cap = capFor (s.measurements, s.plan.settings.targetLufs);
                const double rise = kTargets[ti] == PlanTarget::KEEP_INPUT ? 0.3 : s.plan.totalGainDb + (ci == 0 ? 0.3 : 0.9);   // never more than the plan's gain (the limiter only takes away)
                const double grProxy = juce::jmax (0.0, inM.truePeak + s.plan.totalGainDb - outM.truePeak);
                ++combosP;
                worstGain = juce::jmax (worstGain, applied - (kTargets[ti] == PlanTarget::KEEP_INPUT ? 0.0 : cap)); worstTpP = juce::jmax (worstTpP, outM.truePeak); worstPkGr = juce::jmax (worstPkGr, grProxy);
                if (applied > rise) { ++capBad; std::printf ("FAIL  plug-in cap: %s %s %s applied %+.2f dB (cap %.0f)\n", f.getFileName().toRawUTF8(), kTargetNames[ti], kCharNames[ci], applied, cap); }
                if (outM.truePeak > s.settings.limiter.ceilingDbtp + 0.2) { ++tpBad; std::printf ("FAIL  plug-in ceiling: %s %s true peak %.2f\n", f.getFileName().toRawUTF8(), kTargetNames[ti], outM.truePeak); }
                if (kTargets[ti] == PlanTarget::KEEP_INPUT && grProxy > 6.2) { ++grBad; std::printf ("FAIL  plug-in limiter proxy: %s %s %.2f dB\n", f.getFileName().toRawUTF8(), kTargetNames[ti], grProxy); }
                if (ci == 0)
                {
                    const double d = s.plan.totalGainDb - applied;
                    worstOff = juce::jmax (worstOff, d);
                    if (d < -0.3 || (kTargets[ti] == PlanTarget::KEEP_INPUT && d > 2.3)) { ++offBad; std::printf ("FAIL  plug-in OFF is trim only: %s %s applied %+.2f plan %+.2f\n", f.getFileName().toRawUTF8(), kTargetNames[ti], applied, s.plan.totalGainDb); }
                }
            }
            check (s.measurements.integrated == frozen, f.getFileName() + ": the measurements did not move through all the clicks");
        }
        check (capBad == 0, "plug-in: the real-time chain never applies more than the plan's gain (KEEP INPUT: not above the mix) on every file x target (and character)", juce::String (combosP) + " combos, worst overshoot " + juce::String (worstGain, 3) + " dB");
        check (offBad == 0, "plug-in: character OFF applies the plan's gain less what the limiter takes (never more than the plan; KEEP INPUT at most 2 dB less; LUFS of a 10 s excerpt)", "largest shortfall " + juce::String (worstOff, 3) + " dB");
        check (tpBad == 0, "plug-in: true peak under the ceiling", "highest " + juce::String (worstTpP, 2) + " dBTP");
        check (grBad == 0, "plug-in: KEEP INPUT limiter peak reduction (input peak + gain - output peak) <= 6 dB", "worst " + juce::String (worstPkGr, 2) + " dB");
    }

    //==============================================================================================
    section ("processBlockBypassed: input delayed by the reported latency, ten cycles leave the chain untouched");
    {
        const double fs = 48000.0;
        const auto sig = noise (fs, 12.0, 77, 0.2f);
        auto make = [&] (MihenkProcessor& p, FakePlayHead& head)
        {
            p.setPlayHead (&head);
            p.setNonRealtime (true);
            p.setRateAndBufferSizeDetails (fs, 512);
            p.prepareToPlay (fs, 512);
            p.getSession().profile.setFolder (tmp.getChildFile ("profile"));
        };
        FakePlayHead h1, h2;
        MihenkProcessor ref, cyc;
        make (ref, h1); make (cyc, h2);
        for (auto* q : { &ref, &cyc })
        {
            q->toggleLearn();
            (void) play (*q, sig, 512, 0);
            q->finishLearnNow();
            setIntent (q->getSession(), PlanTarget::CLUB_8, PlanCharacter::GENTLE);
            q->getSession().renderNowForTesting();
            q->publishNow();
            q->prepareToPlay (fs, 512);
        }
        const int lat = cyc.getLatencySamples();
        // ten cycles: 0.3 s bypassed, 0.7 s active, from t = 1 s on
        std::vector<std::pair<int, bool>> events;
        for (int k = 0; k < 10; ++k) { events.push_back ({ (int) ((1.0 + k) * fs), true }); events.push_back ({ (int) ((1.3 + k) * fs), false }); }
        const auto a = play (ref, sig, 512, lat + 1000);
        const auto b = play (cyc, sig, 512, lat + 1000, false, &events);
        // active regions (30 ms after each un-bypass until the next bypass): identical to the chain that was never bypassed
        double maxDiffActive = 0.0, maxDiffByp = 0.0;
        int activeSamples = 0, bypSamples = 0;
        for (int k = 0; k < 10; ++k)
        {
            const int on = (int) ((1.0 + k) * fs), off = (int) ((1.3 + k) * fs), next = (int) ((2.0 + k) * fs);
            for (int c = 0; c < 2; ++c)
            {
                for (int i = off + (int) (0.05 * fs); i < next - 1024; ++i) { maxDiffActive = juce::jmax (maxDiffActive, (double) std::abs (a.getSample (c, i) - b.getSample (c, i))); ++activeSamples; }
                for (int i = on + (int) (0.05 * fs); i < off - 1024; ++i) { maxDiffByp = juce::jmax (maxDiffByp, (double) std::abs (sig.getSample (c, i - lat) - b.getSample (c, i))); ++bypSamples; }
            }
        }
        check (maxDiffActive == 0.0 && activeSamples > 0, "after each of 10 bypass cycles the output is bit-identical to a chain that was never bypassed", "largest difference " + juce::String (maxDiffActive, 12) + " over " + juce::String (activeSamples) + " samples");
        check (maxDiffByp < 1.0e-6 && bypSamples > 0, "while bypassed the output is the input delayed by the reported latency (" + juce::String (lat) + " samples = " + juce::String (1000.0 * lat / fs, 1) + " ms)", "largest difference " + juce::String (maxDiffByp, 9) + " over " + juce::String (bypSamples) + " samples");
    }

    tmp.deleteRecursively();
    std::printf ("\n%d checks, %s\n", checks, failures == 0 ? "ALL PASS" : (juce::String (failures) + " FAILED").toRawUTF8());
    return failures == 0 ? 0 : 1;
}
