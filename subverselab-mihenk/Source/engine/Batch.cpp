#include "Batch.h"
#include "AudioIO.h"
#include "Entitlement.h"

namespace mihenk::batch
{
Intent intentFromSettings (const MasterSettings& s)
{
    Intent i;
    switch (s.targetPreset)
    {
        case TargetPreset::streaming: i.target = PlanTarget::STREAM_14; break;
        case TargetPreset::apple:     i.target = PlanTarget::APPLE_16; break;
        case TargetPreset::club:      i.target = PlanTarget::CLUB_8; break;
        case TargetPreset::keepInput: i.target = PlanTarget::KEEP_INPUT; break;
        case TargetPreset::custom:    i.target = PlanTarget::CUSTOM; break;
    }
    if (std::isfinite (s.targetLufs)) i.customLufs = juce::jlimit (-30.0, -4.0, s.targetLufs);
    if (std::isfinite (s.limiter.ceilingDbtp)) i.ceilingDbtp = juce::jlimit (-3.0, -0.1, s.limiter.ceilingDbtp);
    return i;
}

namespace
{
double targetLufsOf (const Intent& i)
{
    switch (i.target)
    {
        case PlanTarget::STREAM_14: return -14.0;
        case PlanTarget::APPLE_16:  return -16.0;
        case PlanTarget::CLUB_8:    return -8.0;
        case PlanTarget::ALBUM:     return i.albumTarget + i.relativeDb;
        default:                    return i.customLufs;
    }
}
}

AlbumPlan planAlbum (const std::vector<double>& L, const std::vector<double>& slope, const MasterSettings& shared)
{
    return planAlbum (L, slope, intentFromSettings (shared));
}

AlbumPlan planAlbum (const std::vector<double>& L, const std::vector<double>& slope, const Intent& shared)
{
    AlbumPlan p;
    const size_t n = L.size();
    p.offsetLu.assign (n, 0.0);
    p.tiltNudge.assign (n, 0.0);
    p.keepInput = shared.target == PlanTarget::KEEP_INPUT;
    p.targetLufs = targetLufsOf (shared);
    int valid = 0;
    double sumSlope = 0.0;
    for (size_t i = 0; i < n; ++i)
        if (std::isfinite (L[i]))
        {
            p.loudestLufs = juce::jmax (p.loudestLufs, L[i]);
            sumSlope += i < slope.size() ? slope[i] : 0.0;
            ++valid;
        }
    if (valid == 0) return p;
    p.valid = true;
    p.meanSlope = sumSlope / valid;
    for (size_t i = 0; i < n; ++i)
    {
        if (! std::isfinite (L[i])) continue;
        if (! p.keepInput) p.offsetLu[i] = juce::jmax (-kAlbumMaxDropLu, kAlbumKeep * (L[i] - p.loudestLufs));
        const double s = i < slope.size() ? slope[i] : p.meanSlope;
        p.tiltNudge[i] = juce::jlimit (-kAlbumMaxTiltNudge, kAlbumMaxTiltNudge, kAlbumTiltKeep * (p.meanSlope - s));
    }
    return p;
}

juce::File outputFor (const juce::File& source, const Options& o, const std::vector<juce::File>& taken)
{
    const auto stem = source.getFileNameWithoutExtension() + o.suffix;
    auto f = o.outDir.getChildFile (stem + ".wav");
    for (int k = 2; std::find (taken.begin(), taken.end(), f) != taken.end(); ++k)
        f = o.outDir.getChildFile (stem + "-" + juce::String (k) + ".wav");
    return f;
}

Result run (const std::vector<Job>& jobs, const Options& o, ProgressFn progress, const std::atomic<bool>* cancel)
{
    Result res;
    const int n = (int) jobs.size();
    res.tracks.resize ((size_t) n);
    auto cancelled = [cancel] { return cancel != nullptr && cancel->load(); };
    auto report = [&] (int i, double f, const juce::String& stage) { if (progress) progress (i, n, juce::jlimit (0.0, 1.0, f), stage); };
    if (n == 0) { res.message = "The queue is empty."; return res; }
    if (! Entitlement::mayBatchMaster()) { res.message = "Batch mastering is not licensed."; return res; }
    if (! o.outDir.isDirectory() && ! o.outDir.createDirectory()) { res.message = "Cannot create " + o.outDir.getFullPathName(); return res; }

    // Pass 1 (the album needs every track measured before any is rendered; PER TRACK diagnosis uses it too).
    const double measureShare = o.album ? 0.25 : 0.0;
    std::vector<DiagnosisMeasures> dm ((size_t) n);
    std::vector<bool> measured ((size_t) n, false);
    const bool needMeasures = o.album || std::any_of (jobs.begin(), jobs.end(), [] (const Job& j) { return j.applyDiagnosis; });
    if (needMeasures)
        for (int i = 0; i < n; ++i)
        {
            if (cancelled()) { res.cancelled = true; res.message = "Cancelled."; return res; }
            if (! o.album && ! jobs[(size_t) i].applyDiagnosis) continue;
            report (i, measureShare * i / n, "Measuring " + jobs[(size_t) i].file.getFileName());
            auto a = loadAudioFile (jobs[(size_t) i].file);
            if (! a.ok()) { res.tracks[(size_t) i].error = a.error; continue; }
            dm[(size_t) i] = measureMix (a.buffer, a.sampleRate);
            measured[(size_t) i] = dm[(size_t) i].valid;
        }
    if (o.album)
    {
        std::vector<double> L ((size_t) n), S ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            L[(size_t) i] = measured[(size_t) i] && dm[(size_t) i].integrated > -70.0 ? dm[(size_t) i].integrated : kNegInf;
            S[(size_t) i] = dm[(size_t) i].lineSlopeDbPerOct;
        }
        res.plan = planAlbum (L, S, jobs.front().hasIntent ? jobs.front().intent : intentFromSettings (jobs.front().settings));
    }

    // Pass 2: render and export each track.
    std::vector<juce::File> taken;
    for (int i = 0; i < n; ++i)
    {
        auto& t = res.tracks[(size_t) i];
        const auto& job = jobs[(size_t) i];
        t.source = job.file;
        t.settingsMode = job.ownSettings ? "own" : job.applyDiagnosis ? "panel + diagnosis" : "panel";
        const double base = measureShare + (1.0 - measureShare) * i / n, span = (1.0 - measureShare) / n;
        if (cancelled()) { res.cancelled = true; break; }
        if (t.error.isNotEmpty()) continue;
        report (i, base, "Loading " + job.file.getFileName());
        auto a = loadAudioFile (job.file);
        if (! a.ok()) { t.error = a.error; continue; }
        // v3: the track is measured once and planned: solvePlan (measurements, intent). Nothing is solved against the render.
        t.in = analyse (a.buffer, a.sampleRate);
        if (! measured[(size_t) i]) { dm[(size_t) i] = measureMix (a.buffer, a.sampleRate); measured[(size_t) i] = dm[(size_t) i].valid; }
        t.inSlope = measured[(size_t) i] ? dm[(size_t) i].lineSlopeDbPerOct : 0.0;
        Intent intent = job.hasIntent ? job.intent : intentFromSettings (job.settings);
        if (job.applyDiagnosis) intent.character = PlanCharacter::FULL;
        intent.referenceCurve.clear();                  // a panel's tone-match curve belongs to the panel's source
        intent.overrides.erase ("trim");                // level is the plan's
        intent.overrides.erase ("drive");
        if (o.album && res.plan.valid && std::isfinite (res.plan.loudestLufs) && measured[(size_t) i])
        {
            t.albumOffsetLu = res.plan.offsetLu[(size_t) i];
            t.tiltNudge = res.plan.tiltNudge[(size_t) i];
            if (! res.plan.keepInput)
            {
                intent.target = PlanTarget::ALBUM;
                intent.albumTarget = res.plan.targetLufs;
                intent.relativeDb = t.albumOffsetLu;
            }
            if (std::abs (t.tiltNudge) > 1.0e-6) intent.overrides["tilt"] = intent.overrides.count ("tilt") ? intent.overrides["tilt"] + t.tiltNudge : t.tiltNudge;
        }
        auto meas = measurementsFrom (t.in, measured[(size_t) i] ? &dm[(size_t) i] : nullptr, (double) a.buffer.getNumSamples() / a.sampleRate);
        measureLimiterResponse (a.buffer, a.sampleRate, meas);
        auto plan = solvePlan (meas, intent);
        for (const auto& f : meas.findings) if (job.applyDiagnosis) t.scenarios.add (f.id);
        auto s = plan.settings;
        report (i, base + span * 0.15, "Mastering " + job.file.getFileName());
        PreLimiterCache cache;
        MatchResult m;
        auto r = MasterChain::renderLocked (a.buffer, a.sampleRate, s, cache, cancel, &m);
        if (r.cancelled || r.output == nullptr) { if (cancelled()) { res.cancelled = true; break; } t.error = "Render failed."; continue; }
        // SPEC_V3 2 REVISED, calibration: the limiter table is measured on the untouched track; the stages the intent switches on add
        // loudness before the limiter. The miss against the target is measured on the render and held in the measurements (see Session::calibrationStep).
        for (int round = 0; round < 3 && intent.target != PlanTarget::KEEP_INPUT && ! plan.targetUnreachable; ++round)
        {
            const double miss = r.measured.integrated - plan.settings.targetLufs;
            if (! (std::abs (miss) > 0.15 && std::abs (miss) <= 6.0) || plan.totalGainDb <= -23.9) break;
            meas.stageDeltaDb = juce::jlimit (-6.0, 6.0, meas.stageDeltaDb + miss);
            plan = solvePlan (meas, intent);
            s = plan.settings;
            cache = PreLimiterCache();
            r = MasterChain::renderLocked (a.buffer, a.sampleRate, s, cache, cancel, &m);
            if (r.cancelled || r.output == nullptr) break;
        }
        if (r.cancelled || r.output == nullptr) { if (cancelled()) { res.cancelled = true; break; } t.error = "Render failed."; continue; }
        report (i, base + span * 0.8, "Exporting " + job.file.getFileName());
        t.output = outputFor (job.file, o, taken);
        taken.push_back (t.output);
        juce::String err;
        int clipped = 0;
        if (! exportWav (t.output, *r.output, a.sampleRate, o.bits, err, &clipped)) { t.error = err; continue; }
        if (clipped > 0) r.warnings.add (juce::String (clipped) + " samples clipped in quantisation.");
        t.out = r.measured;
        t.outSlope = measureMix (*r.output, a.sampleRate).lineSlopeDbPerOct;
        t.targetLufs = s.targetLufs;
        t.trimDb = s.trimDb;
        t.driveDb = s.limiter.driveDb;
        t.plannedGainDb = plan.totalGainDb;
        t.neededGainDb = plan.desiredGainDb;
        t.targetUnreachable = plan.targetUnreachable;
        for (const auto& e : plan.explain) if (e.action != "detail") t.planLines.add (e.reason);
        t.glueGrDb = r.glue.valid ? r.glue.loudMeanGrDb : 0.0;
        t.limiterGrMaxDb = r.limiter.gr.maxDb;
        t.warnings = r.warnings;
        for (const auto& w : plan.refused) t.warnings.add (w);
        for (const auto& w : plan.warnings) if (w != "targetUnreachable") t.warnings.add (w);
        t.loudness = plan.plainSummary();
        auto reportText = MasterChain::report (job.file.getFileName(), t.output.getFileName(), o.bits, s, t.in, t.out, r, nullptr);
        reportText << "\nPlan        " << plan.plainSummary() << "\n";
        for (const auto& l : t.planLines) reportText << "            - " << l << "\n";
        if (o.album)
            reportText << "\nAlbum       target " << fmt (t.targetLufs) << " LUFS (album offset " << fmt (t.albumOffsetLu, 2) << " LU), tilt nudge "
                       << fmt (t.tiltNudge, 2) << " dB/oct towards the album mean " << fmt (res.plan.meanSlope, 2) << " dB/oct\n";
        t.output.getSiblingFile (t.output.getFileNameWithoutExtension() + ".mihenk.txt").replaceWithText (reportText);
        t.ok = true;
        report (i, base + span, "Done " + job.file.getFileName());
    }
    juce::String err;
    if (! writeReports (res, o, err)) res.message = err;
    int ok = 0;
    for (auto& t : res.tracks) ok += t.ok ? 1 : 0;
    if (res.message.isEmpty())
        res.message = (res.cancelled ? "Cancelled after " : "Batch done: ") + juce::String (ok) + " of " + juce::String (n) + " track(s) exported to "
                      + o.outDir.getFullPathName() + (o.album ? " (album)" : "");
    report (n, 1.0, res.message);
    return res;
}

bool writeReports (Result& res, const Options& o, juce::String& error)
{
    res.csv = o.outDir.getChildFile ("MIHENK_batch_report.csv");
    res.json = o.outDir.getChildFile ("MIHENK_batch_report.json");
    auto num = [] (double v, int dec) { return std::isfinite (v) ? juce::String (v, dec) : juce::String(); };
    auto quote = [] (juce::String x) { return "\"" + x.replace ("\"", "\"\"") + "\""; };
    juce::String csv = "index,source,output,status,settings,album,target_lufs,album_offset_lu,tilt_nudge_db_oct,in_lufs,out_lufs,change_db,"
                       "in_true_peak_dbtp,out_true_peak_dbtp,in_lra_lu,out_lra_lu,in_tilt_db_oct,out_tilt_db_oct,trim_db,drive_db,glue_gr_db,"
                       "limiter_gr_max_db,scenarios,warnings,loudness\n";
    juce::Array<juce::var> arr;
    for (size_t i = 0; i < res.tracks.size(); ++i)
    {
        const auto& t = res.tracks[i];
        const double change = t.ok ? t.out.integrated - t.in.integrated : kNegInf;
        csv << (int) i + 1 << "," << quote (t.source.getFileName()) << "," << quote (t.output.getFileName()) << "," << (t.ok ? "ok" : quote ("error: " + t.error)) << ","
            << quote (t.settingsMode) << "," << (o.album ? "yes" : "no") << "," << num (t.targetLufs, 2) << "," << num (t.albumOffsetLu, 2) << "," << num (t.tiltNudge, 3) << ","
            << num (t.in.integrated, 2) << "," << num (t.ok ? t.out.integrated : kNegInf, 2) << "," << num (change, 2) << ","
            << num (t.in.truePeak, 2) << "," << num (t.ok ? t.out.truePeak : kNegInf, 2) << "," << num (t.in.lra, 1) << "," << num (t.ok ? t.out.lra : kNegInf, 1) << ","
            << num (t.inSlope, 3) << "," << num (t.ok ? t.outSlope : kNegInf, 3) << "," << num (t.trimDb, 2) << "," << num (t.driveDb, 2) << ","
            << num (t.glueGrDb, 2) << "," << num (t.limiterGrMaxDb, 2) << "," << quote (t.scenarios.joinIntoString (" ")) << ","
            << quote (t.warnings.joinIntoString (" | ")) << "," << quote (t.loudness) << "\n";
        auto* x = new juce::DynamicObject();
        x->setProperty ("index", (int) i + 1);
        x->setProperty ("source", t.source.getFullPathName());
        x->setProperty ("output", t.output.getFullPathName());
        x->setProperty ("ok", t.ok);
        if (! t.ok) x->setProperty ("error", t.error);
        x->setProperty ("settings", t.settingsMode);
        auto meas = [&] (const Measurement& m, double slope)
        {
            auto* y = new juce::DynamicObject();
            y->setProperty ("integrated_lufs", std::isfinite (m.integrated) ? juce::var (m.integrated) : juce::var());
            y->setProperty ("true_peak_dbtp", std::isfinite (m.truePeak) ? juce::var (m.truePeak) : juce::var());
            y->setProperty ("sample_peak_dbfs", std::isfinite (m.samplePeak) ? juce::var (m.samplePeak) : juce::var());
            y->setProperty ("lra_lu", m.lra);
            y->setProperty ("short_term_max_lufs", std::isfinite (m.shortTermMax) ? juce::var (m.shortTermMax) : juce::var());
            y->setProperty ("tilt_db_per_oct", slope);
            return juce::var (y);
        };
        x->setProperty ("input", meas (t.in, t.inSlope));
        if (t.ok) x->setProperty ("result", meas (t.out, t.outSlope));
        auto* c = new juce::DynamicObject();
        c->setProperty ("target_lufs", std::isfinite (t.targetLufs) ? juce::var (t.targetLufs) : juce::var());
        c->setProperty ("album_offset_lu", t.albumOffsetLu);
        c->setProperty ("tilt_nudge_db_per_oct", t.tiltNudge);
        c->setProperty ("loudness_change_db", std::isfinite (change) ? juce::var (change) : juce::var());
        c->setProperty ("trim_db", t.trimDb);
        c->setProperty ("drive_db", t.driveDb);
        c->setProperty ("glue_gr_db", t.glueGrDb);
        c->setProperty ("limiter_gr_max_db", t.limiterGrMaxDb);
        juce::Array<juce::var> sc;
        for (auto& id : t.scenarios) sc.add (id);
        c->setProperty ("scenarios", sc);
        x->setProperty ("changes", juce::var (c));
        juce::Array<juce::var> w;
        for (auto& ww : t.warnings) w.add (ww);
        x->setProperty ("warnings", w);
        x->setProperty ("loudness", t.loudness);
        arr.add (juce::var (x));
    }
    auto* root = new juce::DynamicObject();
    root->setProperty ("format", "MIHENK batch report");
    root->setProperty ("version", 1);
    root->setProperty ("date", juce::Time::getCurrentTime().toISO8601 (true));
    root->setProperty ("album", o.album);
    root->setProperty ("bits", o.bits);
    if (o.album && res.plan.valid)
    {
        auto* p = new juce::DynamicObject();
        p->setProperty ("target_lufs", res.plan.targetLufs);
        p->setProperty ("keep_input", res.plan.keepInput);
        p->setProperty ("loudest_input_lufs", res.plan.loudestLufs);
        p->setProperty ("mean_tilt_db_per_oct", res.plan.meanSlope);
        p->setProperty ("rule", "target_i = T + max(-" + juce::String (kAlbumMaxDropLu, 1) + ", " + juce::String (kAlbumKeep, 2)
                                + " * (L_i - L_loudest)); tilt nudge = clamp(" + juce::String (kAlbumTiltKeep, 2) + " * (mean - tilt_i), +-"
                                + juce::String (kAlbumMaxTiltNudge, 2) + ") dB/oct");
        root->setProperty ("album_plan", juce::var (p));
    }
    root->setProperty ("tracks", arr);
    if (! res.csv.replaceWithText (csv) || ! res.json.replaceWithText (juce::JSON::toString (juce::var (root), false)))
    {
        error = "Could not write the batch report in " + o.outDir.getFullPathName();
        return false;
    }
    return true;
}
} // namespace mihenk::batch
