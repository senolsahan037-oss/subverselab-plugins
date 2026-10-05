/*  audit modes 2: presets, reference, batch, player, state, snap, meters, buttons. See audit_main.cpp for the list. */
#include "audit_common.h"
#include <algorithm>

using namespace audit;

namespace
{
/** Runs the message loop until the session is idle and the master is current (real async load / render path). */
bool pump (Rig& r, int timeoutMs = 120000)
{
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
    while (juce::Time::getMillisecondCounter() < end)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (40);
        if (! r.session().busy() && r.session().outputCurrent()) { r.sync(); return true; }
    }
    return false;
}

Buffer readWav (const juce::File& f, double* rate = nullptr)
{
    auto a = loadAudioFile (f);
    if (rate != nullptr) *rate = a.sampleRate;
    return std::move (a.buffer);
}

juce::File writeCorpusExcerpt (const juce::String& stem, double secs, const juce::File& dir, const juce::String& name = {})
{
    auto b = loadCorpus (stem, secs);
    auto f = dir.getChildFile ((name.isEmpty() ? stem : name) + ".wav");
    juce::String err;
    exportWav (f, b, corpusRate(), 24, err);
    return f;
}

//==============================================================================
int modePresets (int, char**)
{
    auto buf = loadCorpus ("bag_peak-3", 12.0);
    // 1. Round trip of a heavily edited settings object.
    {
        Rig r (true, "presets");
        r.load (buf, corpusRate(), "x.wav");
        juce::Random rnd (77);
        std::vector<juce::Slider*> all, sl;
        collectOf<juce::Slider> (r.panel.get(), all);
        for (auto* s : all) if (chainVisible (s, r.panel.get()) && s->getMaximum() > s->getMinimum()) sl.push_back (s);
        for (int i = 0; i < 40; ++i)
        {
            auto* s = sl[(size_t) rnd.nextInt ((int) sl.size())];
            s->setValue (s->getMinimum() + rnd.nextDouble() * (s->getMaximum() - s->getMinimum()), juce::sendNotificationSync);
        }
        r.session().applyScenario (5);
        r.session().applyScenario (11);
        r.render();
        const auto f = r.tmp.getChildFile ("presets/roundtrip" + juce::String (kPresetExtension));
        f.getParentDirectory().createDirectory();
        juce::String err;
        const bool saved = savePreset (f, r.session().settings, err);
        MasterSettings back;
        const bool loaded = loadPreset (f, back, err);
        const auto a = juce::JSON::toString (settingsToVar (r.session().settings), false), b = juce::JSON::toString (settingsToVar (back), false);
        // key-by-key diff
        juce::StringArray lost;
        auto va = settingsToVar (r.session().settings), vb = settingsToVar (back);
        std::function<void (const juce::var&, const juce::var&, juce::String)> diff = [&] (const juce::var& x, const juce::var& y, juce::String path)
        {
            if (auto* ox = x.getDynamicObject())
            {
                for (auto& p : ox->getProperties()) diff (p.value, y.getProperty (p.name, juce::var()), path + "/" + p.name.toString());
                return;
            }
            if (juce::JSON::toString (x, true) != juce::JSON::toString (y, true)) lost.add (path);
        };
        diff (va, vb, "");
        info ("preset round trip: saved=" + juce::String ((int) saved) + " loaded=" + juce::String ((int) loaded) + ", fields that differ after load: " + juce::String (lost.size()) + " " + lost.joinIntoString (","));
        if (! saved || ! loaded) finding ("S1", "PRESET_ROUNDTRIP_IO", "preset save/load failed: " + err);
        if (lost.size() > 0) finding ("S2", "PRESET_ROUNDTRIP_LOSS", "fields that a saved preset does not restore: " + lost.joinIntoString (", "));
        // not carried by the preset at all (session state that changes the master)
        info ("session-only state that a preset cannot carry: autoSkip, toneByHand, profile locks, abMatched, reference, matchCurve (matchCurve is in the preset JSON: " + juce::String ((int) (va["eq"]["matchCurveDb"].isArray())) + ")");
    }
    // 2. Loading the same preset after a clean start and after a wrecked state must give the same master.
    {
        Rig clean (true, "presets_clean"), wrecked (true, "presets_wrecked");
        clean.load (buf, corpusRate(), "x.wav");
        wrecked.load (buf, corpusRate(), "x.wav");
        // The preset: RAP starting point + KEEP INPUT + 3-BAND
        MasterSettings P;
        applyGenreStartingPoint (P, Genre::rapTrap);
        P.targetPreset = TargetPreset::club;
        P.comp.mode = CompMode::threeBand;
        const auto f = clean.tmp.getChildFile ("presets/p" + juce::String (kPresetExtension));
        f.getParentDirectory().createDirectory();
        juce::String err;
        savePreset (f, P, err);
        // wreck: reference matched, scenarios, a few knobs, bypass cycles
        wrecked.session().setReferenceForTesting (loadCorpus ("dip_asis", 12.0), corpusRate(), "ref.wav");
        wrecked.click ("MATCH TO\nREFERENCE");
        wrecked.session().applyScenario (3);
        wrecked.session().applyScenario (9);
        wrecked.click ("KEEP INPUT");
        wrecked.click ("BYPASS");
        wrecked.click ("BYPASS");
        auto load = [&] (Rig& r)
        {
            MasterSettings loaded = r.session().settings;
            juce::String e;
            loadPreset (f, loaded, e);
            r.session().settings = loaded;
            r.session().noteHandEdit (false);
            r.session().settingsChanged();
            r.render();
        };
        load (clean);
        load (wrecked);
        const auto a = clean.snap(), b = wrecked.snap();
        info ("preset load, clean vs wrecked history: " + f2 (a.outLufs) + " vs " + f2 (b.outLufs) + " LUFS, hash " + (a.hash == b.hash ? "identical" : "DIFFERENT"));
        if (a.hash != b.hash)
        {
            juce::String why;
            if (wrecked.session().reference != nullptr) why += " a reference is still loaded;";
            if (! wrecked.session().appliedScenarios.isEmpty()) why += " scenario chips remain (" + juce::String (wrecked.session().appliedScenarios.size()) + ");";
            finding ("S2", "PRESET_LOAD_HISTORY", "the same preset gives a different master after another history: " + f2 (a.outLufs) + " vs " + f2 (b.outLufs) + " LUFS (clean vs after reference match + 2 scenarios + target change);" + why);
        }
    }
    // 3. Broken files.
    {
        Rig r (false, "presets_bad");
        auto dir = r.tmp.getChildFile ("bad");
        dir.createDirectory();
        struct Case { const char* name; juce::String content; };
        const Case cases[] {
            { "empty", "" }, { "truncated", "{\"eq\":{\"bell1Db\": 3.0, \"bell1Hz\"" }, { "nan", "{\"eq\":{\"bell1Db\": NaN, \"bell1Q\": 0},\"comp\":{\"ratio\":-5},\"limiter\":{\"driveDb\":1e30,\"ceilingDbtp\":12}}" },
            { "wrongtype", "{\"eq\":\"hello\",\"targetLufs\":\"loud\",\"limiter\":[1,2,3]}" }, { "array", "[1,2,3]" },
            { "huge", "{\"targetLufs\": -1e300, \"trimDb\": 1e300, \"limiter\": {\"ceilingDbtp\": 5, \"driveDb\": 99999}}" } };
        for (auto& c : cases)
        {
            const auto f = dir.getChildFile (juce::String (c.name) + kPresetExtension);
            f.replaceWithText (c.content);
            MasterSettings s;
            juce::String err;
            const bool ok = loadPreset (f, s, err);
            bool sane = std::isfinite (s.trimDb) && std::isfinite (s.targetLufs) && s.limiter.ceilingDbtp <= 0.0 && s.limiter.driveDb <= 36.0 && s.limiter.driveDb >= -24.0 && s.comp.ratio >= 1.0 && s.eq.bell1Q > 0.0;
            info (juce::String ("bad preset '") + c.name + "': loadPreset " + (ok ? "accepted" : "rejected") + ", settings " + (sane ? "sane" : "INSANE") + " ceiling " + f2 (s.limiter.ceilingDbtp) + " drive " + f2 (s.limiter.driveDb) + " ratio " + f2 (s.comp.ratio) + " Q " + f2 (s.eq.bell1Q));
            if (ok && ! sane) finding ("S1", juce::String ("PRESET_BAD_") + c.name, juce::String ("a corrupt preset ('") + c.name + "') is accepted with out-of-range values");
        }
    }
    // 4. Every factory scenario: gain, true peak, applied twice, removed again.
    {
        Rig r (true, "scen");
        r.load (buf, corpusRate(), "x.wav");
        const auto base = r.snap();
        int nonIdem = 0, notRemoved = 0, overCap = 0;
        juce::String notIdem, notRem;
        for (int sc = 1; sc <= kNumScenarios; ++sc)
        {
            Rig q (true, "scen_q");
            q.load (buf, corpusRate(), "x.wav");
            q.session().applyScenario (sc);
            q.render();
            const auto once = q.snap();
            q.session().applyScenario (sc);
            q.render();
            const auto twice = q.snap();
            if (once.hash != twice.hash) { ++nonIdem; notIdem << " " << sc << "(" << f2 (twice.outLufs - once.outLufs) << " LU)"; }
            q.session().removeScenario (sc);
            if (q.session().appliedScenarios.contains (sc)) q.session().removeScenario (sc);
            q.render();
            const auto removed = q.snap();
            if (removed.hash != base.hash) { ++notRemoved; notRem << " " << sc; }
            if (once.gainDb > 3.3) ++overCap;
            if (once.tp > once.ceiling + 0.05) finding ("S1", "SCENARIO_TP_" + juce::String (sc), "scenario " + juce::String (sc) + ": true peak " + f2 (once.tp) + " above the ceiling");
        }
        info ("scenarios 1.." + juce::String (kNumScenarios) + ": applied twice differs for " + juce::String (nonIdem) + ":" + notIdem + "; remove does not restore the baseline master for " + juce::String (notRemoved) + ":" + notRem + "; gain > 3.3 dB for " + juce::String (overCap));
        if (nonIdem > 0) finding ("S3", "SCENARIO_NOT_IDEMPOTENT", "scenarios applied twice change the master in " + juce::String (nonIdem) + " of " + juce::String (kNumScenarios) + ":" + notIdem);
        if (notRemoved > 0) finding ("S3", "SCENARIO_REMOVE_INCOMPLETE", "removing a scenario does not return the baseline master for " + juce::String (notRemoved) + " of " + juce::String (kNumScenarios) + ":" + notRem);
    }
    return 0;
}

//==============================================================================
int modeReference (int, char**)
{
    auto buf = loadCorpus ("bag_asis", 15.0);
    auto refBuf = loadCorpus ("dip_asis", 15.0);
    struct RefCase { juce::String name; Buffer b; double rate; };
    std::vector<RefCase> refs;
    refs.push_back ({ "dip_asis", refBuf, corpusRate() });
    refs.push_back ({ "dip_brick+6", loadCorpus ("dip_brick+6", 15.0), corpusRate() });
    refs.push_back ({ "dip_quiet-20", loadCorpus ("dip_quiet-20", 15.0), corpusRate() });
    {
        Buffer mono (1, refBuf.getNumSamples());
        mono.copyFrom (0, 0, refBuf, 0, 0, mono.getNumSamples());
        refs.push_back ({ "mono", std::move (mono), corpusRate() });
    }
    refs.push_back ({ "silence", Buffer (2, 44100 * 5), corpusRate() });
    refs.back().b.clear();
    refs.push_back ({ "0.3s", loadCorpus ("dip_asis", 0.3), corpusRate() });
    refs.push_back ({ "96k", synthMix (96000.0, 8.0, 3), 96000.0 });
    refs.push_back ({ "8k", synthMix (8000.0, 8.0, 4), 8000.0 });
    for (auto& rc : refs)
    {
        Rig r (true, "ref");
        r.load (buf, corpusRate(), "bag_asis.wav");
        const auto base = r.snap();
        r.session().setReferenceForTesting (rc.b, rc.rate, rc.name);
        r.sync();
        r.click ("MATCH TO\nREFERENCE");
        const auto m = r.snap();
        auto& s = r.session();
        const double refL = s.refMeas.integrated;
        info ("ref " + rc.name + ": ref " + f2 (refL) + " LUFS; master " + f2 (m.outLufs) + " LUFS (target " + f2 (m.target) + "), gain " + f2 (m.gainDb) + " dB, TP " + f2 (m.tp) + ", limiter GR max " + f2 (m.grMax) + ", tone distance " + f2 (s.toneDistanceBefore(), 2) + " -> " + f2 (s.toneDistanceNow(), 2) + ", curve points " + juce::String ((int) s.settings.eq.matchCurveDb.size()) + " | " + s.status.substring (0, 90));
        if (m.nonFinite) finding ("S1", "REF_NONFINITE_" + rc.name, "reference " + rc.name + ": non-finite output after MATCH TO REFERENCE");
        if (m.gainDb > capFor3 (m) + 0.3 && std::isfinite (m.gainDb)) finding ("S1", "REF_GAIN_" + rc.name, "MATCH TO REFERENCE to '" + rc.name + "' applies +" + f2 (m.gainDb) + " dB (limiter GR max " + f2 (m.grMax) + " dB)");
        if (std::abs (m.outLufs - refL) > 0.7 && std::isfinite (refL) && std::isfinite (m.outLufs) && s.warnings.isEmpty())
            info ("   note: master " + f2 (m.outLufs) + " vs reference " + f2 (refL) + " LUFS with no warning");
        // CLEAR
        r.click ("CLEAR");
        const auto c = r.snap();
        const bool curveKept = ! s.settings.eq.matchCurveDb.empty();
        info ("   after CLEAR: reference " + juce::String (s.reference == nullptr ? "gone" : "still loaded") + ", tone-match curve " + (curveKept ? "STILL APPLIED (" + juce::String ((int) s.settings.eq.matchCurveDb.size()) + " points)" : "gone") + ", master " + f2 (c.outLufs) + " LUFS, hash vs before-reference " + (c.hash == base.hash ? "identical" : "different") + ", target " + juce::String (presetName (s.settings.targetPreset)) + " " + f2 (s.settings.targetLufs));
        if (curveKept && rc.name == "dip_asis")
        {
            std::vector<juce::Slider*> sl;
            collectOf<juce::Slider> (r.panel.get(), sl);
            bool toneAmountVisible = false;
            for (auto* x : sl) if (x->getName() == "TONE AMOUNT" && chainVisible (x, r.panel.get())) toneAmountVisible = true;
            finding ("S2", "REF_CLEAR_KEEPS_CURVE", "after CLEAR the reference is gone but its tone-match curve (" + juce::String ((int) s.settings.eq.matchCurveDb.size()) + " points) still shapes the master (tone distance now " + f2 (s.toneDistanceNow(), 2) + ", output differs from the pre-reference master by " + f2 (c.outLufs - base.outLufs, 2) + " LU, spectrum changed); TONE AMOUNT knob visible=" + juce::String ((int) toneAmountVisible) + ", the only way back is RESET ALL or hunting the EQ");
        }
        if (c.hash != base.hash && rc.name == "dip_asis")
        {
            // Then pick STREAM: does the master return?
            r.click ("STREAM -14");
            const auto st = r.snap();
            info ("   after STREAM -14: hash vs never-referenced " + juce::String (st.hash == base.hash ? "identical" : "different") + ", " + f2 (st.outLufs - base.outLufs, 2) + " LU");
        }
    }
    // A second reference replaces the first: the old curve must not be blended.
    {
        Rig r (true, "ref2");
        r.load (buf, corpusRate(), "bag_asis.wav");
        r.session().setReferenceForTesting (refBuf, corpusRate(), "a");
        r.click ("MATCH TO\nREFERENCE");
        const auto a = r.snap();
        r.session().setReferenceForTesting (loadCorpus ("dip_asis", 15.0), corpusRate(), "a2");
        r.click ("MATCH TO\nREFERENCE");
        const auto b = r.snap();
        info ("match twice with the identical reference: " + juce::String (a.hash == b.hash ? "identical" : "different") + " (" + f2 (b.outLufs - a.outLufs, 3) + " LU)");
        if (a.hash != b.hash) finding ("S2", "REF_MATCH_NOT_IDEMPOTENT", "pressing MATCH TO REFERENCE twice with the same reference changes the master by " + f2 (b.outLufs - a.outLufs, 3) + " LU (the second match is computed on top of the first curve)");
    }
    return 0;
}

//==============================================================================
double lufsOfFile (const juce::File& f) { double r = 0; auto b = readWav (f, &r); return integratedLoudness (b, r); }

int modeBatch (int, char**)
{
    auto dir = tmpDir ("batch");
    auto src = dir.getChildFile ("src");
    src.createDirectory();
    const juce::StringArray stems { "bag_asis", "dip_peak-3", "bag_quiet-20", "dip_brick+6" };
    juce::Array<juce::File> files;
    for (auto& s : stems) files.add (writeCorpusExcerpt (s, 12.0, src));
    auto runBatch = [&] (Rig& r, const juce::String& outName, bool album, bool perTrack, bool* completed = nullptr)
    {
        auto& bp = r.panel->getBatchPanel();
        auto out = dir.getChildFile (outName);
        out.deleteRecursively();
        bp.setOutputFolder (out);
        bp.setModes (perTrack, album);
        bp.start();
        const bool ok = bp.waitForTesting (600000);
        if (completed != nullptr) *completed = ok;
        return out;
    };
    auto setUp = [&] (Rig& r)
    {
        auto& bp = r.panel->getBatchPanel();
        r.panel->showBatchPanel (true);
        bp.addFiles (files);
        bp.waitForTesting (60000);
    };
    // 0. UI facts
    {
        Rig r (false, "batch_ui");
        r.panel->showBatchPanel (true);
        std::vector<juce::Button*> bs;
        collectOf<juce::Button> (&r.panel->getBatchPanel(), bs);
        juce::StringArray names;
        for (auto* b : bs) names.add (b->getButtonText().replace ("\n", " "));
        info ("batch panel buttons: " + names.joinIntoString (" | "));
        bool hasPlay = false;
        for (auto& n : names) if (n.containsIgnoreCase ("play") || n.containsIgnoreCase ("pause") || n.containsIgnoreCase ("A/B") || n.containsIgnoreCase ("up") || n.containsIgnoreCase ("down")) hasPlay = true;
        if (! hasPlay) finding ("S2", "BATCH_NO_PLAYER", "BATCH panel has no play / pause / A-B / reorder control and no waveform or spectrum column; the buttons are: " + names.joinIntoString (", ") + " (owner: tracks cannot be played; SPEC_V3 4b asks for a media player; stage 4 / needs its own work)");
    }
    // 1. SAME FOR ALL, default panel state: each exported file against the panel's own master of that file.
    {
        Rig r (false, "batch_a");
        r.load (loadCorpus ("bag_asis", 12.0), corpusRate(), "bag_asis.wav");
        setUp (r);
        bool done = false;
        auto out = runBatch (r, "out_same", false, false, &done);
        const auto& res = r.panel->getBatchPanel().lastResult();
        info ("batch SAME: completed " + juce::String ((int) done) + ", " + res.message);
        for (size_t i = 0; i < stems.size(); ++i)
        {
            const auto exported = res.tracks[i].output;
            Rig p (false, "batch_panelref");
            p.load (loadCorpus (stems[(int) i], 12.0), corpusRate(), stems[(int) i] + ".wav");
            const auto pm = p.snap();
            const double l = exported.existsAsFile() ? lufsOfFile (exported) : kNegInf;
            double rate = 0;
            auto eb = readWav (exported, &rate);
            double md = 1.0e9;
            if (eb.getNumSamples() > 0 && p.session().output != nullptr) md = maxDiff (eb, *p.session().output);
            info ("  " + stems[(int) i] + ": exported " + f2 (l) + " LUFS vs panel master " + f2 (pm.outLufs) + ", max sample difference " + f2 (md, 7) + (md > 2.0e-6 ? "  <-- differs" : ""));
            if (md > 2.0e-6) finding ("S1", "BATCH_NE_PANEL_" + stems[(int) i], stems[(int) i] + ": the batch export differs from the panel master of the same file (max sample difference " + f2 (md, 6) + ", " + f2 (l) + " vs " + f2 (pm.outLufs) + " LUFS)");
        }
    }
    // 2. The panel last showed a FINISHED file, and the user had set a starting point on a quiet one before: batch inherits the skip.
    {
        Rig r (false, "batch_b");
        r.load (loadCorpus ("bag_quiet-20", 12.0), corpusRate(), "bag_quiet-20.wav");
        r.click ("HIP-HOP / RAP");
        r.click ("STREAM -14");
        // reference: what the quiet file gets with RAP when it is the file on the panel
        const auto quietRef = r.snap();
        auto quietBuf = *r.session().output;
        // now the user loads a finished file (the real async path resets toneByHand)
        r.session().loadInput (writeCorpusExcerpt ("dip_brick+6", 12.0, dir, "brick_panel"));
        pump (r);
        const bool skipActive = r.session().settings.autoSkip.active;
        const bool eqOn = r.session().settings.eq.enabled, compOn = r.session().settings.comp.enabled, widthOn = r.session().settings.width.enabled;
        info ("panel after loading a finished file: autoSkip " + juce::String ((int) skipActive) + ", EQ " + juce::String ((int) eqOn) + ", comp " + juce::String ((int) compOn) + ", width " + juce::String ((int) widthOn));
        setUp (r);
        auto out = runBatch (r, "out_skip", false, false);
        const auto& res = r.panel->getBatchPanel().lastResult();
        for (size_t i = 0; i < stems.size(); ++i)
        {
            if (stems[(int) i] != "bag_quiet-20") continue;
            double rate = 0;
            auto eb = readWav (res.tracks[i].output, &rate);
            Rig p (false, "batch_panelref2");
            p.load (loadCorpus ("bag_quiet-20", 12.0), corpusRate(), "q.wav");
            p.click ("HIP-HOP / RAP");
            p.click ("STREAM -14");
            const double md = eb.getNumSamples() > 0 ? maxDiff (eb, *p.session().output) : 1e9;
            info ("  quiet file in the batch (panel shows a finished file, RAP chosen earlier): differs from its own panel master by " + f2 (md, 6) + " (batch " + f2 (integratedLoudness (eb, rate)) + " vs panel " + f2 (quietRef.outLufs) + " LUFS)");
            if (skipActive && md > 2.0e-6)
                finding ("S1", "BATCH_INHERITS_AUTOSKIP", "the batch uses the panel's settings as the panel last had them: with a finished file on the panel the 'already finished' auto-skip had switched EQ/compressor/stereo off, so every quiet track in the batch loses the chosen starting point (RAP) its own panel master has (max sample difference " + f2 (md, 6) + ")");
        }
    }
    // 3. BYPASS on while the batch starts.
    {
        Rig r (false, "batch_c");
        r.load (loadCorpus ("bag_quiet-20", 12.0), corpusRate(), "bag_quiet-20.wav");
        r.click ("HIP-HOP / RAP");
        r.click ("BYPASS");
        setUp (r);
        auto out = runBatch (r, "out_bypass", false, false);
        const auto& res = r.panel->getBatchPanel().lastResult();
        for (size_t i = 0; i < stems.size(); ++i)
        {
            double rate = 0;
            auto eb = readWav (res.tracks[i].output, &rate);
            auto ib = readWav (files[(int) i]);
            const double md = (eb.getNumSamples() > 0 && ib.getNumSamples() > 0) ? maxDiff (eb, ib) : 1e9;
            info ("  BYPASS on at START: " + stems[(int) i] + " exported vs source: max difference " + f2 (md, 7) + ", " + f2 (integratedLoudness (eb, rate)) + " vs " + f2 (integratedLoudness (ib, rate)) + " LUFS");
            if (md < 2.0e-6) finding ("S1", "BATCH_BYPASS_" + stems[(int) i], stems[(int) i] + ": with BYPASS engaged on the panel, BATCH exports the unprocessed input as if it were the master (report says 'mastered')");
        }
    }
    // 4. Band SOLO left on.
    {
        Rig r (true, "batch_d");
        r.load (loadCorpus ("bag_asis", 12.0), corpusRate(), "bag_asis.wav");
        r.click ("3-BAND");
        r.click ("SOLO");
        const auto s = r.snap();
        const double exportLufs = [&]
        {
            juce::String msg;
            const auto f = dir.getChildFile ("solo_export.wav");
            r.session().exportMaster (f, 24, msg);
            return lufsOfFile (f);
        }();
        info ("band SOLO on: the exported master reads " + f2 (exportLufs) + " LUFS against " + f2 (r.session().inMeas.integrated) + " LUFS input (settings.comp.bands[0].solo=" + juce::String ((int) r.session().settings.comp.bands[0].solo) + ")");
        if (r.session().settings.comp.bands[0].solo && exportLufs < r.session().inMeas.integrated - 6.0)
            finding ("S1", "EXPORT_WITH_SOLO", "EXPORT (and BATCH) render the soloed band only: with LOW band SOLO left on the exported file is " + f2 (exportLufs) + " LUFS against a " + f2 (r.session().inMeas.integrated) + " LUFS input; nothing warns or resets SOLO for export");
    }
    // 5. Album mode.
    {
        Rig r (false, "batch_e");
        r.load (loadCorpus ("bag_asis", 12.0), corpusRate(), "bag_asis.wav");
        setUp (r);
        auto out = runBatch (r, "out_album", true, false);
        const auto& res = r.panel->getBatchPanel().lastResult();
        double lo = 1e9, hi = -1e9;
        juce::String line;
        for (size_t i = 0; i < stems.size(); ++i)
        {
            const double l = res.tracks[i].ok ? res.tracks[i].out.integrated : kNegInf;
            if (std::isfinite (l)) { lo = juce::jmin (lo, l); hi = juce::jmax (hi, l); }
            line << stems[(int) i] << " in " << f2 (res.tracks[i].in.integrated) << " -> out " << f2 (l) << " (target " << f2 (res.tracks[i].targetLufs) << ", GR " << f2 (res.tracks[i].limiterGrMaxDb) << ", trim " << f2 (res.tracks[i].trimDb) << ", drive " << f2 (res.tracks[i].driveDb) << ") ; ";
        }
        info ("album: " + line);
        info ("album output spread " + f2 (hi - lo) + " LU");
        if (hi - lo > 1.0)
            finding ("S2", "ALBUM_NOT_EQUAL_LOUDNESS", "ALBUM mode leaves the four songs " + f2 (hi - lo) + " LU apart (" + f2 (lo) + " .. " + f2 (hi) + " LUFS) by design (target_i = T + max(-3, 0.5 x (L_i - L_max))); SPEC_V3 section 4 promises equal loudness across songs");
        // album v3.2 (SPEC_V3 2 REVISED): the +8 dB cap is gone; a track gets what its album target needs. Report it as information, not a finding.
        for (size_t i = 0; i < stems.size(); ++i)
            if (res.tracks[i].ok && res.tracks[i].out.integrated - res.tracks[i].in.integrated > 8.3)
                info ("ALBUM raises " + stems[(int) i] + " by " + f2 (res.tracks[i].out.integrated - res.tracks[i].in.integrated) + " dB to reach its album target (limiter GR max " + f2 (res.tracks[i].limiterGrMaxDb) + " dB)");
    }
    // 6. Overwrite and reprocessing: a second batch into the same folder; a folder that already holds exports.
    {
        Rig r (false, "batch_f");
        r.load (loadCorpus ("bag_asis", 12.0), corpusRate(), "bag_asis.wav");
        setUp (r);
        auto out = runBatch (r, "out_twice", false, false);
        const auto before = out.getChildFile ("bag_asis_MIHENK.wav").getLastModificationTime();
        juce::Thread::sleep (1100);
        auto& bp = r.panel->getBatchPanel();
        bp.setOutputFolder (out);
        bp.start();
        bp.waitForTesting (600000);
        const auto after = out.getChildFile ("bag_asis_MIHENK.wav").getLastModificationTime();
        info ("second batch into the same folder: bag_asis_MIHENK.wav " + juce::String (after > before ? "overwritten silently" : "kept"));
        if (after > before) finding ("S3", "BATCH_OVERWRITES", "a second batch into the same folder overwrites the earlier exports and reports without asking or numbering");
        Rig r2 (false, "batch_g");
        r2.load (loadCorpus ("bag_asis", 12.0), corpusRate(), "bag_asis.wav");
        auto& bp2 = r2.panel->getBatchPanel();
        bp2.addFiles ({ out });
        bp2.waitForTesting (60000);
        info ("adding the export folder as a source folder gives " + juce::String (bp2.numRows()) + " rows (the already mastered _MIHENK files are included)");
        if (bp2.numRows() > 0)
            finding ("S3", "BATCH_REPROCESS_OUTPUTS", "choosing a folder that already contains *_MIHENK.wav exports queues them as sources again (" + juce::String (bp2.numRows()) + " rows: no filter on the export suffix)");
    }
    // 7. Odd files in a queue: a corrupt wav, a zero-length wav, and a good one after them.
    {
        auto odd = dir.getChildFile ("odd");
        odd.createDirectory();
        odd.getChildFile ("a_corrupt.wav").replaceWithData ("RIFFxxxxWAVEfmt garbage garbage garbage", 40);
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::FileOutputStream> os (odd.getChildFile ("b_empty.wav").createOutputStream());
            if (os != nullptr) { std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), 44100.0, 2, 24, {}, 0)); if (w != nullptr) os.release(); }
        }
        writeCorpusExcerpt ("bag_asis", 5.0, odd, "c_good");
        Rig r (false, "batch_h");
        r.load (loadCorpus ("bag_asis", 12.0), corpusRate(), "bag_asis.wav");
        auto& bp = r.panel->getBatchPanel();
        bp.addFiles ({ odd });
        bp.waitForTesting (60000);
        auto out = dir.getChildFile ("out_odd");
        out.deleteRecursively();
        bp.setOutputFolder (out);
        bp.setModes (false, true);
        bp.start();
        const bool done = bp.waitForTesting (600000);
        const auto& res = bp.lastResult();
        int ok = 0;
        juce::String errs;
        for (auto& t : res.tracks) { ok += t.ok ? 1 : 0; if (! t.ok) errs << t.source.getFileName() << ": " << t.error << " ; "; }
        info ("odd queue (album mode): completed " + juce::String ((int) done) + ", ok " + juce::String (ok) + " of " + juce::String ((int) res.tracks.size()) + ", errors: " + errs + " | " + res.message);
        if (ok != 1) finding ("S2", "BATCH_ODD_FILES", "a corrupt and an empty file in the queue break the good one: " + juce::String (ok) + " of " + juce::String ((int) res.tracks.size()) + " exported; " + errs);
    }
    return 0;
}

//==============================================================================
/** Least-squares sine fit at a known frequency: returns the residual relative to the signal in dB (SNR). */
double sineFitSnr (const std::vector<float>& x, double fs, double f, int from, int n)
{
    double sc = 0, ss = 0, xx = 0, cc = 0, cs = 0, sss = 0;
    for (int i = 0; i < n; ++i)
    {
        const double t = (from + i) / fs, c = std::cos (2 * 3.14159265358979 * f * t), s = std::sin (2 * 3.14159265358979 * f * t), v = x[(size_t) (from + i)];
        sc += v * c; ss += v * s; xx += v * v; cc += c * c; cs += c * s; sss += s * s;
    }
    const double det = cc * sss - cs * cs;
    const double a = (sc * sss - ss * cs) / det, b = (ss * cc - sc * cs) / det;
    const double fit = a * a * cc + 2 * a * b * cs + b * b * sss;
    const double resid = juce::jmax (1.0e-30, xx - fit);
    return 10.0 * std::log10 (fit / resid);
}

int modePlayer (int, char**)
{
    // Device rate vs file rate through the real processor path (DeviceStage).
    struct Case { double fileRate, devRate, freq; };
    for (auto c : { Case { 44100, 44100, 1000 }, Case { 44100, 48000, 1000 }, Case { 44100, 48000, 15000 }, Case { 48000, 44100, 15000 }, Case { 96000, 44100, 1000 }, Case { 96000, 44100, 30000 }, Case { 44100, 96000, 15000 } })
    {
        Rig r (false, "player");
        const int n = (int) (c.fileRate * 4.0);
        Buffer tone (2, n);
        for (int i = 0; i < n; ++i)
            for (int ch = 0; ch < 2; ++ch) tone.setSample (ch, i, (float) (0.25 * std::sin (2 * 3.14159265358979 * c.freq * i / c.fileRate)));
        r.p.prepareToPlay (c.devRate, 512);
        r.load (tone, c.fileRate, "tone.wav");
        r.click ("BYPASS");                       // play the original, raw: the resampler is what is measured
        auto& pl = r.session().player;
        pl.setPosition (0);
        pl.play();
        const int devN = (int) (c.devRate * 2.0);
        std::vector<float> out ((size_t) devN);
        juce::MidiBuffer midi;
        Buffer blk (2, 512);
        for (int pos = 0; pos < devN; pos += 512)
        {
            const int len = juce::jmin (512, devN - pos);
            Buffer view (blk.getArrayOfWritePointers(), 2, 0, len);
            blk.clear();
            r.p.processBlock (blk, midi);
            for (int i = 0; i < len; ++i) out[(size_t) (pos + i)] = blk.getSample (0, i);
        }
        pl.stop();
        // The tone as the device plays it: expected frequency is unchanged (a pitch-correct resample). Alias-free when freq < devRate / 2.
        const double nyq = c.devRate / 2.0;
        const bool below = c.freq < nyq;
        const double snr = below ? sineFitSnr (out, c.devRate, c.freq, (int) (0.3 * c.devRate), (int) (1.0 * c.devRate)) : kNegInf;
        double peak = 0;
        for (int i = (int) (0.3 * c.devRate); i < (int) (1.3 * c.devRate); ++i) peak = juce::jmax (peak, (double) std::abs (out[(size_t) i]));
        info ("file " + f2 (c.fileRate / 1000, 1) + " kHz tone " + f2 (c.freq / 1000, 1) + " kHz on a " + f2 (c.devRate / 1000, 1) + " kHz device: " + (below ? "sine-fit SNR " + f2 (snr, 1) + " dB" : "tone above the device Nyquist, residual peak " + f2 (toDb (peak), 1) + " dBFS (an alias-free resampler would give silence)") + ", level " + f2 (toDb (peak), 2) + " dBFS (0.25 = -12.04)");
        if (below && snr < 60.0) finding ("S3", "PLAYER_RESAMPLE_" + juce::String ((int) c.fileRate) + "_" + juce::String ((int) c.devRate) + "_" + juce::String ((int) c.freq), "device-rate mismatch (" + f2 (c.fileRate / 1000, 1) + " kHz file on " + f2 (c.devRate / 1000, 1) + " kHz device): the monitor path is a plain ResamplingAudioSource, a " + f2 (c.freq / 1000, 1) + " kHz tone comes out with only " + f2 (snr, 1) + " dB signal-to-distortion (a mastering monitor should be > 90 dB); what you hear is not what you export");
        if (! below && toDb (peak) > -60.0) finding ("S3", "PLAYER_ALIAS_" + juce::String ((int) c.fileRate) + "_" + juce::String ((int) c.devRate), f2 (c.freq / 1000, 1) + " kHz content of a " + f2 (c.fileRate / 1000, 1) + " kHz file folds back to " + f2 (toDb (peak), 1) + " dBFS on a " + f2 (c.devRate / 1000, 1) + " kHz device (no anti-alias filter)");
    }
    // End of file, loop, pause, mono.
    {
        Rig r (false, "player2");
        r.p.prepareToPlay (44100.0, 512);
        Buffer mono = synthMix (44100.0, 3.0, 5, 1);
        r.load (mono, 44100.0, "mono.wav");
        auto& pl = r.session().player;
        pl.setPosition (0);
        pl.play();
        Buffer blk (2, 512);
        juce::MidiBuffer midi;
        double asym = 0;
        int blocks = 0;
        while (pl.isPlaying() && blocks < 400)
        {
            blk.clear();
            r.p.processBlock (blk, midi);
            for (int i = 0; i < 512; ++i) asym = juce::jmax (asym, (double) std::abs (blk.getSample (0, i) - blk.getSample (1, i)));
            ++blocks;
        }
        info ("mono file: playback stopped by itself after " + juce::String (blocks) + " blocks (3 s = 259), L/R difference max " + f2 (asym, 8) + ", position " + juce::String ((long long) pl.getPosition()));
        if (pl.isPlaying()) finding ("S2", "PLAYER_NO_STOP_AT_END", "mono playback does not stop at the end of the file");
        if (asym > 0.0) finding ("S3", "PLAYER_MONO_LR", "mono file does not play identical on L and R (diff " + f2 (asym, 8) + ")");
        // tiny files
        for (double secs : { 0.2, 0.05, 0.001 })
        {
            Rig q (false, "player3");
            q.p.prepareToPlay (44100.0, 512);
            q.load (synthMix (44100.0, juce::jmax (secs, 0.001), 5), 44100.0, "tiny.wav");
            auto& pp = q.session().player;
            pp.setPosition (0);
            pp.play();
            for (int k = 0; k < 6 && pp.isPlaying(); ++k) { blk.clear(); q.p.processBlock (blk, midi); }
            info ("tiny file " + f2 (secs, 3) + " s: loaded " + juce::String ((long long) pp.getLength()) + " samples, output finite " + juce::String ((int) ! hasNonFinite (blk)));
        }
    }
    return 0;
}

//==============================================================================
int modeState (int, char**)
{
    // Plug-in: a LEARNed state restored in a new instance at another sample rate. Same material, same loudness expected.
    struct Result { double lufs, tp; int latency; };
    auto learnAndPlay = [] (double fs, juce::MemoryBlock* saveTo, const juce::MemoryBlock* restore, bool club) -> Result
    {
        MihenkProcessor p;
        p.setStandaloneModeForTesting (false);
        p.setNonRealtime (true);
        p.setRateAndBufferSizeDetails (fs, 512);
        p.prepareToPlay (fs, 512);
        const auto mat = synthMix (fs, 16.0, 11, 2, -6.0);
        struct PH : juce::AudioPlayHead { juce::Optional<PositionInfo> getPosition() const override { PositionInfo i; i.setIsPlaying (true); return i; } } ph;
        p.setPlayHead (&ph);
        auto run = [&] (const Buffer& in)
        {
            Buffer out (2, in.getNumSamples());
            for (int c = 0; c < 2; ++c) out.copyFrom (c, 0, in, c, 0, in.getNumSamples());
            juce::MidiBuffer midi;
            for (int pos = 0; pos < out.getNumSamples(); pos += 512)
            {
                Buffer view (out.getArrayOfWritePointers(), 2, pos, juce::jmin (512, out.getNumSamples() - pos));
                p.processBlock (view, midi);
            }
            return out;
        };
        if (restore != nullptr) p.setStateInformation (restore->getData(), (int) restore->getSize());
        else
        {
            if (club) { p.getSession().settings.targetPreset = TargetPreset::club; }
            p.toggleLearn();
            run (mat);
            p.finishLearnNow();
            p.prepareToPlay (fs, 512);
        }
        if (saveTo != nullptr) p.getStateInformation (*saveTo);
        p.prepareToPlay (fs, 512);
        run (synthMix (fs, 1.5, 12, 2, -6.0));          // let the detectors settle
        auto out = run (mat);
        Result r;
        r.latency = p.getLatencySamples();
        const int lat = r.latency;
        Buffer al (2, out.getNumSamples() - lat);
        for (int c = 0; c < 2; ++c) al.copyFrom (c, 0, out, c, lat, al.getNumSamples());
        r.lufs = integratedLoudness (al, fs);
        r.tp = truePeakDb (al);
        p.setPlayHead (nullptr);
        return r;
    };
    for (bool club : { false, true })
    {
        juce::MemoryBlock state;
        const auto a = learnAndPlay (48000.0, &state, nullptr, club);
        info (juce::String (club ? "CLUB" : "STREAM") + ": learned at 48 kHz: " + f2 (a.lufs) + " LUFS, " + f2 (a.tp) + " dBTP, latency " + juce::String (a.latency));
        for (double fs : { 44100.0, 96000.0, 48000.0 })
        {
            const auto b = learnAndPlay (fs, nullptr, &state, club);
            info ("   state restored in a new instance at " + f2 (fs / 1000, 1) + " kHz: " + f2 (b.lufs) + " LUFS, " + f2 (b.tp) + " dBTP, latency " + juce::String (b.latency) + " (" + f2 (b.lufs - a.lufs, 2) + " LU)");
            if (std::abs (b.lufs - a.lufs) > 0.5) finding ("S2", juce::String ("STATE_SR_") + juce::String ((int) fs) + (club ? "_club" : "_stream"), "plug-in state learned at 48 kHz and restored at " + f2 (fs / 1000, 1) + " kHz plays " + f2 (b.lufs - a.lufs, 2) + " LU different (" + f2 (a.lufs) + " -> " + f2 (b.lufs) + " LUFS)");
            if (b.tp > -0.9) finding ("S1", juce::String ("STATE_TP_") + juce::String ((int) fs), "restored state at " + f2 (fs / 1000, 1) + " kHz: true peak " + f2 (b.tp) + " dBTP above the -1 dBTP ceiling");
        }
    }
    return 0;
}

//==============================================================================
void savePng (const juce::Image& img, const juce::File& f)
{
    f.deleteFile();
    juce::FileOutputStream os (f);
    juce::PNGImageFormat().writeImageToStream (img, os);
}

/** Plays `secs` through the processor so the live meters get real frames. */
void playLive (Rig& r, double secs)
{
    auto& p = r.p;
    Buffer b (2, 512);
    juce::MidiBuffer midi;
    const double fs = r.session().sampleRate;
    const int blocks = (int) (secs * fs / 512);
    for (int k = 0; k < blocks; ++k)
    {
        b.clear();
        p.processBlock (b, midi);
        if (k % 2 == 0) r.panel->getLive().update();
    }
    r.panel->getLive().update();
}

int modeSnap (int argc, char** argv)
{
    const float scale = argc > 2 ? (float) std::atof (argv[2]) : 1.1f;
    auto dir = outDir().getChildFile ("snaps");
    dir.createDirectory();
    auto loud = loadCorpus ("dip_asis", 30.0);
    struct V { juce::String name; bool adv; std::function<void (Rig&)> setup; };
    std::vector<V> vs {
        { "simple_stream", false, [] (Rig& r) { r.click ("STREAM -14"); } },
        { "simple_club", false, [] (Rig& r) { r.click ("CLUB -8"); r.click ("HIP-HOP / RAP"); } },
        { "advanced", true, [] (Rig& r) { r.click ("HIP-HOP / RAP"); } },
        { "advanced_3band", true, [] (Rig& r) { r.click ("HIP-HOP / RAP"); r.click ("3-BAND"); } },
        { "advanced_spectrum", true, [] (Rig& r) { r.click ("SPECTRUM"); } },
        { "advanced_loudness", true, [] (Rig& r) { r.click ("LOUDNESS"); } } };
    for (auto& v : vs)
    {
        Rig r (v.adv, "snap");
        r.p.prepareToPlay (44100.0, 512);
        r.load (loud, corpusRate(), "dip_asis.wav");
        v.setup (r);
        r.panel->settleForSnapshot();
        savePng (r.panel->createComponentSnapshot (r.panel->getLocalBounds(), true, scale), dir.getChildFile (v.name + "_stopped.png"));
        // playing: 2 s of audio, then a snapshot
        r.session().player.setPosition (corpusRate() * 8);
        r.session().player.play();
        playLive (r, 2.0);
        savePng (r.panel->createComponentSnapshot (r.panel->getLocalBounds(), true, scale), dir.getChildFile (v.name + "_playing.png"));
        r.session().player.stop();
        info ("snap " + v.name);
    }
    // batch panel with rows after an album batch
    {
        Rig r (false, "snap_b");
        auto src = tmpDir ("snap_src");
        juce::Array<juce::File> files;
        for (auto* s : { "bag_asis", "dip_peak-3", "bag_quiet-20" }) files.add (writeCorpusExcerpt (s, 8.0, src));
        r.load (loadCorpus ("bag_asis", 8.0), corpusRate(), "bag_asis.wav");
        r.panel->showBatchPanel (true);
        auto& bp = r.panel->getBatchPanel();
        bp.addFiles (files);
        bp.waitForTesting (60000);
        bp.setOutputFolder (tmpDir ("snap_out"));
        bp.setModes (false, true);
        bp.start();
        bp.waitForTesting (600000);
        savePng (r.panel->createComponentSnapshot (r.panel->getLocalBounds(), true, scale), dir.getChildFile ("batch.png"));
        info ("snap batch");
    }
    return 0;
}

/** Do the meters and graphs move while audio plays? Pixel change per component between snapshots. */
int modeMeters (int, char**)
{
    for (const char* view : { "WAVE", "SPECTRUM", "LOUDNESS" })
    {
        Rig r (true, "meters");
        r.p.prepareToPlay (44100.0, 512);
        r.load (loadCorpus ("dip_asis", 40.0), corpusRate(), "dip_asis.wav");
        r.click ("HIP-HOP / RAP");
        r.click (view);
        auto& pl = r.session().player;
        pl.setPosition (corpusRate() * 6);
        pl.play();
        std::vector<juce::Image> frames;
        for (int k = 0; k < 6; ++k)
        {
            playLive (r, 0.25);
            r.panel->tickForTesting (1);
            frames.push_back (r.panel->createComponentSnapshot (r.panel->getLocalBounds(), true, 1.0f));
        }
        pl.stop();
        struct Region { juce::String name; juce::Rectangle<int> b; };
        std::vector<Region> regions;
        auto add = [&] (const juce::String& n, juce::Component* c) { if (c != nullptr && c->isVisible()) regions.push_back ({ n, r.panel->getLocalArea (c->getParentComponent(), c->getBounds()) }); };
        auto addType = [&] (const juce::String& n, auto* typeTag)
        {
            using T = std::remove_pointer_t<decltype (typeTag)>;
            std::vector<T*> v;
            collectOf<T> (r.panel.get(), v);
            for (auto* c : v) if (chainVisible (c, r.panel.get())) add (n, c);
        };
        addType ("CrtScope", (ui::CrtScope*) nullptr);
        addType ("OutputMeters", (ui::OutputMeters*) nullptr);
        addType ("VfdTable", (ui::VfdTable*) nullptr);
        addType ("InfoDisplay", (ui::InfoDisplay*) nullptr);
        addType ("GrReadout", (ui::GrReadout*) nullptr);
        addType ("GrTrace", (ui::GrTrace*) nullptr);
        addType ("CorrelationMeter", (ui::CorrelationMeter*) nullptr);
        addType ("LimiterReadout", (ui::LimiterReadout*) nullptr);
        addType ("LoudnessVfd", (ui::LoudnessVfd*) nullptr);
        addType ("DiagnosisPanel", (ui::DiagnosisPanel*) nullptr);
        info (juce::String ("view ") + view + ": fraction of pixels that changed between consecutive 250 ms frames (5 intervals)");
        for (auto& reg : regions)
        {
            juce::String line = "   " + reg.name + " [" + reg.b.toString() + "]";
            double total = 0;
            for (size_t k = 1; k < frames.size(); ++k)
            {
                int changed = 0, n = 0;
                for (int y = reg.b.getY(); y < reg.b.getBottom() && y < frames[k].getHeight(); ++y)
                    for (int x = reg.b.getX(); x < reg.b.getRight() && x < frames[k].getWidth(); ++x)
                    {
                        const auto a = frames[k - 1].getPixelAt (x, y), b = frames[k].getPixelAt (x, y);
                        const int d = std::abs ((int) a.getRed() - (int) b.getRed()) + std::abs ((int) a.getGreen() - (int) b.getGreen()) + std::abs ((int) a.getBlue() - (int) b.getBlue());
                        changed += d > 24 ? 1 : 0;
                        ++n;
                    }
                const double fr = n > 0 ? 100.0 * changed / n : 0.0;
                total += fr;
                line << " " << f2 (fr, 1) << "%";
            }
            info (line);
            if (total / 5.0 < 0.05 && (reg.name == "CrtScope" || reg.name == "OutputMeters" || reg.name == "GrReadout" || reg.name == "LimiterReadout"))
                finding ("S2", juce::String ("STATIC_") + reg.name + "_" + view, reg.name + " does not change while 1.25 s of audio plays (view " + view + "): mean " + f2 (total / 5.0, 3) + " % of pixels per 250 ms");
        }
        savePng (frames.back(), outDir().getChildFile ("snaps/meters_" + juce::String (view) + ".png"));
    }
    return 0;
}

//==============================================================================
/** Level sweep: the same song scaled from loud to quiet; the master must be monotonic in the input level (a quieter input never ends louder). */
int modeSweep (int, char**)
{
    const auto base = loadCorpus ("bag_peak-0.3", 14.0);
    struct Cfg { const char* name; const char* target; const char* character; };
    for (auto cfg : { Cfg { "STREAM OFF", "STREAM -14", "OFF" }, Cfg { "STREAM FULL", "STREAM -14", "FULL" }, Cfg { "CLUB OFF", "CLUB -8", "OFF" } })
    {
        Rig r (false, "sweep");
        juce::String table = juce::String (cfg.name) + ": input dB scaled -> in LUFS / out LUFS / gain / limGR / finished?\n";
        double prevIn = 0, prevOut = 0;
        bool have = false;
        int violations = 0;
        juce::String worst;
        double worstDrop = 0;
        for (double db = 0.0; db >= -30.01; db -= 0.5)
        {
            Buffer b (base.getNumChannels(), base.getNumSamples());
            for (int c = 0; c < b.getNumChannels(); ++c) b.copyFrom (c, 0, base, c, 0, b.getNumSamples());
            b.applyGain ((float) std::pow (10.0, db / 20.0));
            r.load (b, corpusRate(), "sweep.wav");
            r.click (cfg.target);
            r.click (cfg.character);
            const auto sn = r.snap();
            table << "  " << f2 (db, 1) << " -> " << f2 (sn.inLufs) << " / " << f2 (sn.outLufs) << " / " << f2 (sn.gainDb) << " / " << f2 (sn.grMax) << " / " << juce::String ((int) r.session().plan.finishedMaster) << "\n";
            if (have && sn.inLufs < prevIn - 0.2 && sn.outLufs > prevOut + 0.3)
            {
                ++violations;
                if (sn.outLufs - prevOut > worstDrop) { worstDrop = sn.outLufs - prevOut; worst = f2 (prevIn) + " LUFS in -> " + f2 (prevOut) + " out, but " + f2 (sn.inLufs) + " in -> " + f2 (sn.outLufs) + " out"; }
            }
            prevIn = sn.inLufs; prevOut = sn.outLufs; have = true;
        }
        info (table);
        if (violations > 0) finding ("S2", juce::String ("NONMONOTONIC_") + cfg.name, juce::String (cfg.name) + ": a quieter input ends up LOUDER than a louder one in " + juce::String (violations) + " steps of a 0.5 dB sweep (worst: " + worst + "); the +3 dB / +8 dB cap switches at 6 LU from the target, a cliff of up to " + f2 (worstDrop) + " LU");
    }
    return 0;
}
} // namespace

int auditModes2 (const juce::String& mode, int argc, char** argv)
{
    if (mode == "presets") return modePresets (argc, argv);
    if (mode == "reference") return modeReference (argc, argv);
    if (mode == "batch") return modeBatch (argc, argv);
    if (mode == "player") return modePlayer (argc, argv);
    if (mode == "state") return modeState (argc, argv);
    if (mode == "snap") return modeSnap (argc, argv);
    if (mode == "meters") return modeMeters (argc, argv);
    if (mode == "sweep") return modeSweep (argc, argv);
    if (mode == "dbg2")
    {
        Rig r (false, "dbg2");
        r.load (loadCorpus ("bag_asis", 12.0), corpusRate(), "x.wav");
        for (const char* t : { "CUSTOM", "KEEP INPUT", "CLUB -8", "APPLE -16", "STREAM -14", "APPLE -16", "STREAM -14" })
        {
            r.click (t);
            const auto sn = r.snap();
            info (juce::String (t) + ": intent " + juce::String ((int) r.session().intent.target) + " plan target " + f2 (r.session().plan.settings.targetLufs) + " settings.target " + f2 (r.session().settings.targetLufs) + " preset " + presetName (r.session().settings.targetPreset) + " out " + f2 (sn.outLufs));
        }
        return 0;
    }
    if (mode == "dbg")
    {
        Rig r (false, "dbg");
        r.load (loadCorpus ("bag_peak-3", 8.0), corpusRate(), "x.wav");
        auto* b = findButton (*r.panel, "CLUB -8");
        info ("button found " + juce::String ((int) (b != nullptr)));
        if (b != nullptr)
        {
            fire (b);
            info ("after triggerClick: preset " + juce::String (presetName (r.session().settings.targetPreset)) + " intent.target " + juce::String ((int) r.session().intent.target) + " target " + f2 (r.session().settings.targetLufs));
            r.render();
            info ("after render: preset " + juce::String (presetName (r.session().settings.targetPreset)) + " intent.target " + juce::String ((int) r.session().intent.target) + " lufs " + f2 (r.snap().outLufs) + " status " + r.session().status);
        }
        return 0;
    }
    if (mode == "buttons")
    {
        for (bool adv : { false, true })
        {
            Rig r (adv, "buttons");
            r.load (synthMix (44100, 5, 1), 44100.0);
            std::vector<juce::Button*> all;
            collectOf<juce::Button> (r.panel.get(), all);
            info (juce::String (adv ? "ADVANCED" : "SIMPLE") + " buttons: " + juce::String ((int) all.size()));
            for (auto* b : all)
                info ("  '" + b->getButtonText().replace ("\n", "\\n") + "' vis=" + juce::String ((int) chainVisible (b, r.panel.get())) + " en=" + juce::String ((int) b->isEnabled()) + " toggles=" + juce::String ((int) b->getClickingTogglesState()) + " bounds=" + b->getBounds().toString());
        }
        return 0;
    }
    info ("unknown mode " + mode);
    return 1;
}
