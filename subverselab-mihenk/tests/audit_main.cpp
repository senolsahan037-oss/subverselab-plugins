/*  MIHENK audit driver (2026-10-03). Finds and reports; fixes nothing; never edits Source/.

        audit <mode> [args]

    modes (each prints FINDING lines and writes TSV tables to $AUDIT_OUT, default build-audit/audit_out):
      targets [secs]        every corpus file x every target button x {default, HIP-HOP}: gain, true peak, limiter GR, drive
      bypass                BYPASS / A-B cycles through the real player: level jump, bit-exactness, clicks, drift
      random <seed0> <n> [steps] [secs]   n seeded random click / knob sequences on a real panel; invariants after every step
      perm                  order independence of the intent buttons (all ordered triples of 8 actions)
      accum                 A-B-A-B x10 of button pairs: does the result drift
      presets               preset save / load round trips and what a preset does not carry
      reference             reference load / MATCH TO REFERENCE / CLEAR, odd references
      batch                 batch / album through the real BatchPanel: against the panel's own master, bypass, solo, folders
      player                standalone player: file rate vs device rate, loop, end of file, mono
      state                 plug-in state restore across sample rates, latency on knobs
    Formats / lengths are separate (audit_formats.cpp).
*/
#include "audit_common.h"
#include <algorithm>
#include <set>

using namespace audit;

namespace
{
const char* targetButtons[] { "STREAM -14", "APPLE -16", "CLUB -8", "KEEP INPUT", "CUSTOM" };

double capFor (const Snap& s)
{
    const double tgt = s.target;
    return std::abs (s.inLufs - tgt) <= 6.0 ? 3.0 : 8.0;
}

/** What the listener hears from the player (A, B or BYPASS), `secs` of it, as one buffer at the file rate. */
Buffer hear (Rig& r, double secs, double from = 0.0)
{
    auto& pl = r.session().player;
    const double fs = pl.getFileRate();
    const int n = (int) (secs * fs), block = 512;
    Buffer out (2, n);
    out.clear();
    Buffer b (2, block);
    pl.setPosition ((juce::int64) (from * fs));
    pl.play();
    for (int pos = 0; pos + block <= n; pos += block)
    {
        b.clear();
        juce::AudioSourceChannelInfo info (&b, 0, block);
        pl.getNextAudioBlock (info);
        for (int c = 0; c < 2; ++c) out.copyFrom (c, pos, b, c, 0, block);
    }
    pl.stop();
    return out;
}

/** Largest sample-to-sample step relative to the local peak of a 2 ms neighbourhood (click detector). */
double worstStep (const Buffer& b, int from, int to, double fs)
{
    const int w = juce::jmax (8, (int) (0.002 * fs));
    double worst = 0.0;
    for (int i = juce::jmax (1, from); i < juce::jmin (to, b.getNumSamples()); ++i)
        for (int c = 0; c < b.getNumChannels(); ++c)
        {
            float local = 1.0e-4f;
            for (int k = juce::jmax (0, i - w); k < juce::jmin (b.getNumSamples(), i + w); k += 4) local = juce::jmax (local, std::abs (b.getSample (c, k)));
            worst = juce::jmax (worst, (double) std::abs (b.getSample (c, i) - b.getSample (c, i - 1)) / local);
        }
    return worst;
}

//==============================================================================
int modeTargets (int argc, char** argv)
{
    const double secs = argc > 2 ? std::atof (argv[2]) : 40.0;
    juce::File tsv = outDir().getChildFile ("targets.tsv");
    tsv.replaceWithText ("file\tvariant\ttarget\tinLufs\toutLufs\tgainDb\ttpDbtp\tsamplePeak\tlimGrMax\tlimGrMean\tdriveDb\ttrimDb\ttargetLufs\tstatus\n");
    struct Row { juce::String file, variant, target; Snap s; };
    std::vector<Row> rows;
    int historyMismatch = 0;
    for (auto& stem : corpusStems())
    {
        if (stem.startsWith ("dipmix") && ! (stem.endsWith ("asis") || stem.endsWith ("peak-3") || stem.endsWith ("quiet-20"))) continue;
        auto buf = loadCorpus (stem, secs);
        for (const char* variant : { "default", "rap" })
        {
            Rig r (false, "targets");
            r.load (buf, corpusRate(), stem + ".wav");
            if (juce::String (variant) == "rap") r.click ("HIP-HOP / RAP");
            std::map<juce::String, uint64_t> first;
            auto runOrder = [&] (bool reversed, bool record)
            {
                const int n = 5;
                for (int k = 0; k < n; ++k)
                {
                    const int i = reversed ? n - 1 - k : k;
                    const juce::String name = targetButtons[i];
                    r.click (name);
                    if (name == "CUSTOM") r.panel->turnKnobForTesting ("TARGET", -9.0), r.render();
                    auto s = r.snap();
                    if (record)
                    {
                        first[name] = s.hash;
                        rows.push_back ({ stem, variant, name, s });
                        tsv.appendText (stem + "\t" + variant + "\t" + name + "\t" + f2 (s.inLufs) + "\t" + f2 (s.outLufs) + "\t" + f2 (s.gainDb) + "\t" + f2 (s.tp) + "\t"
                                        + f2 (s.sp) + "\t" + f2 (s.grMax) + "\t" + f2 (s.grMean) + "\t" + f2 (s.drive) + "\t" + f2 (s.trim) + "\t" + f2 (s.target) + "\t"
                                        + s.status.replaceCharacter ('\t', ' ') + "\n");
                    }
                    else if (first[name] != s.hash)
                    {
                        ++historyMismatch;
                        info ("history: " + stem + " " + variant + " " + name + " differs on the second visit (reverse order): " + f2 (s.outLufs) + " LUFS");
                    }
                }
            };
            runOrder (false, true);
            runOrder (true, false);
        }
    }
    // Summaries
    for (const char* t : targetButtons)
    {
        double worstGain = -1e9, worstTp = -1e9, worstGr = 0.0;
        juce::String gainAt, tpAt, grAt;
        int overCap = 0, n = 0;
        for (auto& row : rows)
        {
            if (row.target != t) continue;
            ++n;
            if (row.s.gainDb > capFor (row.s) + 0.3) ++overCap;
            if (row.s.gainDb > worstGain) { worstGain = row.s.gainDb; gainAt = row.file + "/" + row.variant; }
            const double over = row.s.tp - row.s.ceiling;
            if (over > worstTp) { worstTp = over; tpAt = row.file + "/" + row.variant; }
            if (row.s.grMax > worstGr) { worstGr = row.s.grMax; grAt = row.file + "/" + row.variant; }
        }
        info (juce::String (t) + ": " + juce::String (n) + " renders, worst gain " + f2 (worstGain) + " dB at " + gainAt + ", gain over cap in " + juce::String (overCap) + ", worst TP over ceiling " + f2 (worstTp) + " dB at " + tpAt + ", worst limiter GR " + f2 (worstGr) + " dB at " + grAt);
        if (overCap > 0) finding ("S1", juce::String ("GAIN_CAP_") + t, juce::String (t) + ": applied loudness gain exceeds the SPEC_V3 cap in " + juce::String (overCap) + " of " + juce::String (n) + " renders; worst +" + f2 (worstGain) + " dB (" + gainAt + "), limiter GR up to " + f2 (worstGr) + " dB (" + grAt + ")");
        if (worstTp > 0.05) finding ("S1", juce::String ("TP_OVER_") + t, juce::String (t) + ": true peak above the ceiling by " + f2 (worstTp) + " dB at " + tpAt);
    }
    if (historyMismatch > 0) finding ("S2", "TARGET_HISTORY", "Visiting the same target a second time (after the other targets) gave a different master in " + juce::String (historyMismatch) + " cases");
    info ("targets table: " + tsv.getFullPathName());
    return 0;
}

//==============================================================================
int modeBypass (int, char**)
{
    for (const char* stem : { "bag_peak-3", "dip_asis", "bag_quiet-20", "dip_brick+6", "bag_peak-0.3" })
    {
        auto buf = loadCorpus (stem, 20.0);
        Rig r (false, "bypass");
        r.load (buf, corpusRate(), juce::String (stem) + ".wav");
        auto& s = r.session();
        const auto base = r.snap();
        // B, A (matched), BYPASS
        const double secs = 12.0;
        auto B = hear (r, secs);
        const double lufsB = integratedLoudness (B, corpusRate());
        r.p.setListenOriginal (true);
        auto A = hear (r, secs);
        const double lufsA = integratedLoudness (A, corpusRate());
        r.p.setListenOriginal (false);
        r.click ("BYPASS");
        auto By = hear (r, secs);
        const double lufsBy = integratedLoudness (By, corpusRate());
        const double inLufs = integratedLoudness (buf, corpusRate());
        info (juce::String (stem) + ": heard B " + f2 (lufsB) + " LUFS, A matched " + f2 (lufsA) + ", BYPASS " + f2 (lufsBy) + " (input " + f2 (inLufs) + ", master gain " + f2 (base.gainDb) + " dB)");
        if (std::abs (lufsBy - lufsB) > 0.5)
            finding ("S2", "BYPASS_NOT_LEVEL_MATCHED", juce::String (stem) + ": switching BYPASS changes the heard loudness by " + f2 (lufsBy - lufsB) + " LU (B " + f2 (lufsB) + " -> bypass " + f2 (lufsBy) + " LUFS); A/B MATCHED does not apply to the BYPASS key (Player::getNextAudioBlock plays the original raw)");
        if (std::abs (lufsA - lufsB) > 0.3)
            finding ("S2", "AB_MATCH_OFF", juce::String (stem) + ": A/B MATCHED left " + f2 (lufsA - lufsB) + " LU between A and B");
        // bypass = original bit for bit?
        Buffer ref (2, By.getNumSamples());
        for (int c = 0; c < 2; ++c) ref.copyFrom (c, 0, buf, c, 0, ref.getNumSamples());
        const double md = maxDiff (ref, By);
        if (md > 1.0e-7) finding ("S3", "BYPASS_NOT_BITEXACT", juce::String (stem) + ": bypass playback differs from the input file by " + f2 (md, 8) + " (max sample difference)");
        // 10 cycles
        r.click ("BYPASS"); // off
        const auto hashB0 = hashBuffer (hear (r, 4.0));
        int drift = 0;
        double worstClick = 0.0;
        for (int k = 0; k < 10; ++k)
        {
            r.click ("BYPASS");
            hear (r, 1.0);
            r.click ("BYPASS");
            if (hashBuffer (hear (r, 4.0)) != hashB0) ++drift;
            if (r.snap().hash != base.hash) ++drift;
        }
        info (juce::String (stem) + ": 10 bypass cycles, drifted plays " + juce::String (drift));
        if (drift > 0) finding ("S1", "BYPASS_CYCLE_DRIFT", juce::String (stem) + ": master differs after bypass cycles (" + juce::String (drift) + " of 20 comparisons)");
        // click at the switch: run a 1 s stream and toggle mid-way, in the loud part
        auto& pl = s.player;
        const int block = 512, total = 48 * block;
        Buffer stream (2, total);
        Buffer b (2, block);
        pl.setPosition (8 * 44100);
        pl.play();
        int switchAt = -1;
        for (int pos = 0, blk = 0; pos + block <= total; pos += block, ++blk)
        {
            if (blk == 20) { pl.setBypass (true); switchAt = pos; }
            b.clear();
            juce::AudioSourceChannelInfo info2 (&b, 0, block);
            pl.getNextAudioBlock (info2);
            for (int c = 0; c < 2; ++c) stream.copyFrom (c, pos, b, c, 0, block);
        }
        pl.stop();
        pl.setBypass (false);
        worstClick = worstStep (stream, switchAt - 64, switchAt + 2000, corpusRate());
        info (juce::String (stem) + ": worst step/local-peak at the bypass switch " + f2 (worstClick, 3));
        if (worstClick > 0.5) finding ("S3", "BYPASS_CLICK", juce::String (stem) + ": step of " + f2 (worstClick, 2) + " x the local peak while switching to BYPASS");
    }
    return 0;
}

//==============================================================================
struct Act
{
    juce::String name;
    int weight;
    std::function<juce::String (Rig&, juce::Random&)> run;   // returns the detail text
};

juce::StringArray switchTexts() { return { "IN", "LOW CUT", "24 DB", "LINEAR", "AIR DYN", "LOW-MID DYN", "UPPER-MID DYN", "TIGHTEN", "DC FILTER", "3-BAND", "SOLO", "BYPASS", "PHASE-SAFE", "A/B MATCHED" }; }

std::vector<Act> makeActions (const std::vector<juce::File>& refs)
{
    std::vector<Act> a;
    for (const char* t : targetButtons)
        a.push_back ({ juce::String ("target ") + t, 2, [t] (Rig& r, juce::Random& rnd) { r.click (t); if (juce::String (t) == "CUSTOM" && rnd.nextBool()) { r.panel->turnKnobForTesting ("TARGET", -20.0 + 14.0 * rnd.nextDouble()); r.render(); } return juce::String(); } });
    for (const char* g : { "HIP-HOP / RAP", "R&B / POP", "ELECTRONIC", "OFF", "GENTLE", "FULL", "RESET OVERRIDES" })
        a.push_back ({ juce::String ("genre ") + g, 2, [g] (Rig& r, juce::Random&) { r.click (g); return juce::String(); } });
    a.push_back ({ "bypass key", 2, [] (Rig& r, juce::Random&) { r.click ("BYPASS"); return juce::String (r.session().settings.bypass ? "on" : "off"); } });
    a.push_back ({ "face toggle", 1, [] (Rig& r, juce::Random&) { r.face (! r.advanced); return juce::String (r.advanced ? "advanced" : "simple"); } });
    a.push_back ({ "scenario", 2, [] (Rig& r, juce::Random& rnd) { const int sc = 1 + rnd.nextInt (kNumScenarios); r.session().applyScenario (sc); r.render(); return juce::String (sc); } });
    a.push_back ({ "switch", 5, [] (Rig& r, juce::Random& rnd)
    {
        std::vector<juce::Button*> all, pick;
        collectOf<juce::Button> (r.panel.get(), all);
        const auto texts = switchTexts();
        for (auto* b : all)
            if (b->isEnabled() && chainVisible (b, r.panel.get()) && dynamic_cast<ui::CapButton*> (b) != nullptr && texts.contains (b->getButtonText()) && b->getClickingTogglesState())
                pick.push_back (b);
        if (pick.empty()) return juce::String ("none");
        auto* b = pick[(size_t) rnd.nextInt ((int) pick.size())];
        const auto name = b->getButtonText();
        fire (b);
        r.render();
        return name + (b->getToggleState() ? " on" : " off");
    } });
    a.push_back ({ "knob", 10, [] (Rig& r, juce::Random& rnd)
    {
        std::vector<juce::Slider*> all, pick;
        collectOf<juce::Slider> (r.panel.get(), all);
        for (auto* s : all) if (s->isEnabled() && chainVisible (s, r.panel.get()) && s->getMaximum() > s->getMinimum()) pick.push_back (s);
        if (pick.empty()) return juce::String ("none");
        auto* s = pick[(size_t) rnd.nextInt ((int) pick.size())];
        const double v = s->getMinimum() + rnd.nextDouble() * (s->getMaximum() - s->getMinimum());
        s->setValue (v, juce::sendNotificationSync);
        r.render();
        return s->getName() + "=" + f2 (s->getValue());
    } });
    a.push_back ({ "auto lamp", 2, [] (Rig& r, juce::Random& rnd)
    {
        const char* l[] { "TRIM", "THRESHOLD", "DRIVE", "RELEASE" };
        const char* pick = l[rnd.nextInt (4)];
        r.panel->clickAutoLampForTesting (pick);
        r.render();
        return juce::String (pick);
    } });
    a.push_back ({ "reset all", 1, [] (Rig& r, juce::Random&) { r.p.resetAll(); r.render(); return juce::String(); } });
    a.push_back ({ "preset save+load", 1, [] (Rig& r, juce::Random& rnd)
    {
        const auto folder = r.tmp.getChildFile ("presets");
        folder.createDirectory();
        const auto f = folder.getChildFile ("slot" + juce::String (rnd.nextInt (3)) + kPresetExtension);
        juce::String err;
        if (rnd.nextBool() || ! f.existsAsFile())
        {
            savePreset (f, r.session().settings, err);
            return "saved " + f.getFileName();
        }
        MasterSettings loaded = r.session().settings;
        if (! loadPreset (f, loaded, err)) return "load failed " + err;
        r.session().settings = loaded;
        r.session().noteHandEdit (false);
        r.session().settingsChanged();
        r.render();
        return "loaded " + f.getFileName();
    } });
    if (! refs.empty())
    {
        a.push_back ({ "match to reference", 2, [] (Rig& r, juce::Random&) { if (r.session().reference == nullptr) return juce::String ("no ref"); r.click ("MATCH TO\nREFERENCE"); return juce::String(); } });
        a.push_back ({ "clear reference", 1, [] (Rig& r, juce::Random&) { r.click ("CLEAR"); return juce::String(); } });
    }
    return a;
}

int modeRandom (int argc, char** argv)
{
    const int seed0 = argc > 2 ? std::atoi (argv[2]) : 1, nSeq = argc > 3 ? std::atoi (argv[3]) : 20, steps = argc > 4 ? std::atoi (argv[4]) : 12;
    const double secs = argc > 5 ? std::atof (argv[5]) : 12.0;
    auto stems = corpusStems();
    juce::File tsv = outDir().getChildFile ("random_steps.tsv");
    if (seed0 == 1) tsv.replaceWithText ("seed\tfile\tstep\taction\tdetail\tinLufs\toutLufs\tgainDb\ttp\tceiling\tlimGrMax\tdrive\ttrim\ttarget\tbypass\tflags\thash\n");
    auto refBuf = loadCorpus ("dipmix_asis", 15.0);
    std::map<juce::String, int> flagCounts;
    std::map<juce::String, juce::String> firstRepro;
    int resetDirty = 0, total = 0;
    std::map<juce::String, std::pair<int, double>> worstGainByAction;
    for (int q = 0; q < nSeq; ++q)
    {
        const int seed = seed0 + q;
        juce::Random rnd (seed);
        const auto stem = stems[rnd.nextInt (stems.size())];
        auto buf = loadCorpus (stem, secs);
        Rig r (rnd.nextBool(), "random");
        r.load (buf, corpusRate(), stem + ".wav");
        const bool withRef = rnd.nextInt (3) == 0;
        if (withRef) r.session().setReferenceForTesting (refBuf, corpusRate(), "ref.wav");
        const auto actions = makeActions (withRef ? std::vector<juce::File> { juce::File() } : std::vector<juce::File>());
        int totalW = 0;
        for (auto& a : actions) totalW += a.weight;
        const auto base = r.snap();
        uint64_t freshHash = 0;
        {
            Rig fresh (false, "fresh");
            fresh.load (buf, corpusRate(), stem + ".wav");
            freshHash = fresh.snap().hash;
        }
        for (int st = 0; st < steps; ++st)
        {
            int pick = rnd.nextInt (totalW);
            const Act* act = &actions[0];
            for (auto& a : actions) { if (pick < a.weight) { act = &a; break; } pick -= a.weight; }
            const auto detail = act->run (r, rnd);
            auto s = r.snap();
            ++total;
            juce::StringArray flags;
            const auto& set = r.session().settings;
            if (s.nonFinite || ! std::isfinite (s.outLufs) && std::isfinite (s.inLufs) && ! set.comp.bands[0].solo) flags.add ("NONFINITE");
            if (s.limiterOn && ! s.bypass && s.tp > s.ceiling + 0.05) flags.add ("TP_OVER");
            if (s.gainDb > capFor (s) + 0.3 && ! s.bypass) flags.add ("GAIN_OVER_CAP");
            if (s.grMax > 6.0) flags.add ("LIM_GR_GT6");
            if (r.session().settings.bypass != r.session().player.isBypassed()) flags.add ("BYPASS_FLAG_MISMATCH");
            const bool anySolo = set.comp.bands[0].solo || set.comp.bands[1].solo || set.comp.bands[2].solo;
            if (anySolo) flags.add ("BAND_SOLO_IN_MASTER");
            if (s.targetLock && s.limiterOn && ! set.bypass && std::abs (s.outLufs - s.target) > 1.0 && std::isfinite (s.outLufs) && r.session().warnings.isEmpty() && ! anySolo)
                flags.add ("OFF_TARGET_NO_WARNING");
            for (auto& f : flags)
            {
                ++flagCounts[f];
                if (! firstRepro.count (f)) firstRepro[f] = "seed " + juce::String (seed) + " step " + juce::String (st) + " (" + stem + ", " + act->name + " " + detail + ")";
                if (f == "GAIN_OVER_CAP") { auto& w = worstGainByAction[act->name]; w.first++; w.second = juce::jmax (w.second, s.gainDb); }
            }
            tsv.appendText (juce::String (seed) + "\t" + stem + "\t" + juce::String (st) + "\t" + act->name + "\t" + detail + "\t" + f2 (s.inLufs) + "\t" + f2 (s.outLufs) + "\t" + f2 (s.gainDb) + "\t" + f2 (s.tp) + "\t" + f2 (s.ceiling) + "\t" + f2 (s.grMax) + "\t" + f2 (s.drive) + "\t" + f2 (s.trim) + "\t" + f2 (s.target) + "\t" + (s.bypass ? "1" : "0") + "\t" + flags.joinIntoString (",") + "\t" + juce::String::toHexString ((juce::int64) s.hash) + "\n");
        }
        // After the walk: RESET ALL must give the fresh master back.
        r.p.resetAll();
        r.render();
        const auto after = r.snap();
        if (after.hash != freshHash)
        {
            ++resetDirty;
            if (resetDirty <= 5) info ("seed " + juce::String (seed) + " (" + stem + "): RESET ALL did not return the fresh master: " + f2 (after.outLufs) + " vs " + f2 (base.outLufs) + " LUFS");
        }
        if ((q + 1) % 5 == 0) info ("... " + juce::String (q + 1) + " sequences, " + juce::String (total) + " steps");
    }
    info ("random: " + juce::String (nSeq) + " sequences, " + juce::String (total) + " steps, seeds " + juce::String (seed0) + ".." + juce::String (seed0 + nSeq - 1));
    for (auto& [f, n] : flagCounts)
    {
        info ("  " + f + ": " + juce::String (n) + " steps; first at " + firstRepro[f]);
    }
    for (auto& [act, wp] : worstGainByAction) info ("  gain over cap after '" + act + "': " + juce::String (wp.first) + " times, worst +" + f2 (wp.second) + " dB");
    for (auto& [f, n] : flagCounts)
    {
        if (f == "NONFINITE") finding ("S1", "RANDOM_NONFINITE", juce::String (n) + " steps produced a NaN/Inf or non-finite loudness; first " + firstRepro[f]);
        if (f == "TP_OVER") finding ("S1", "RANDOM_TP_OVER", juce::String (n) + " steps with true peak above the ceiling; first " + firstRepro[f]);
        if (f == "GAIN_OVER_CAP") finding ("S1", "RANDOM_GAIN_OVER_CAP", juce::String (n) + " of " + juce::String (total) + " steps applied more loudness gain than the SPEC_V3 cap; first " + firstRepro[f]);
        if (f == "BYPASS_FLAG_MISMATCH") finding ("S2", "RANDOM_BYPASS_MISMATCH", juce::String (n) + " steps where settings.bypass and the player disagree; first " + firstRepro[f]);
        if (f == "BAND_SOLO_IN_MASTER") finding ("S2", "RANDOM_SOLO_IN_MASTER", juce::String (n) + " steps where a band SOLO was part of the rendered master (the file that would be exported); first " + firstRepro[f]);
        if (f == "OFF_TARGET_NO_WARNING") finding ("S2", "RANDOM_OFF_TARGET_SILENT", juce::String (n) + " steps with TARGET LOCK on and the output more than 1 LU off the target without any warning; first " + firstRepro[f]);
        if (f == "LIM_GR_GT6") finding ("S2", "RANDOM_LIM_GR", juce::String (n) + " steps with limiter peak GR > 6 dB; first " + firstRepro[f]);
    }
    if (resetDirty > 0) finding ("S2", "RESET_ALL_NOT_CLEAN", "after a random walk, RESET ALL (keep file) did not reproduce the fresh master in " + juce::String (resetDirty) + " of " + juce::String (nSeq) + " sequences");
    return 0;
}

//==============================================================================
/** Order independence: every ordered triple of intent actions, grouped by the intent they leave behind. */
int modePerm (int, char**)
{
    struct A { juce::String name; std::function<void (Rig&)> run; int cat; int val; };
    std::vector<A> acts {
        { "STREAM", [] (Rig& r) { r.click ("STREAM -14"); }, 0, 0 },
        { "APPLE", [] (Rig& r) { r.click ("APPLE -16"); }, 0, 1 },
        { "CLUB", [] (Rig& r) { r.click ("CLUB -8"); }, 0, 2 },
        { "KEEP", [] (Rig& r) { r.click ("KEEP INPUT"); }, 0, 3 },
        { "RAP", [] (Rig& r) { r.click ("HIP-HOP / RAP"); }, 1, 0 },
        { "POP", [] (Rig& r) { r.click ("R&B / POP"); }, 1, 1 },
        { "BYP", [] (Rig& r) { r.click ("BYPASS"); }, 2, 1 },
        { "ADV", [] (Rig& r) { r.face (true); }, 3, 1 } };
    for (const char* stem : { "bag_peak-3", "dip_asis" })
    {
        auto buf = loadCorpus (stem, 10.0);
        std::map<juce::String, std::map<uint64_t, juce::StringArray>> groups;
        std::map<juce::String, Snap> keySnap;
        const int n = (int) acts.size();
        int runs = 0;
        for (int i = 0; i < n; ++i) for (int j = 0; j < n; ++j) for (int k = 0; k < n; ++k)
        {
            if (i == j || j == k || i == k) continue;
            const int seq[3] { i, j, k };
            Rig r (false, "perm");
            r.load (buf, corpusRate(), juce::String (stem) + ".wav");
            int last[4] { -1, -1, 0, 0 };
            for (int x : seq) { acts[(size_t) x].run (r); last[acts[(size_t) x].cat] = acts[(size_t) x].val; }
            // The intent left behind (only the render matters; bypass / face do not touch the render in the app).
            const juce::String key = "target" + juce::String (last[0]) + "_genre" + juce::String (last[1]);
            auto s = r.snap();
            groups[key][s.hash].add (acts[(size_t) i].name + ">" + acts[(size_t) j].name + ">" + acts[(size_t) k].name);
            keySnap[key + "/" + juce::String::toHexString ((juce::int64) s.hash)] = s;
            ++runs;
        }
        int badGroups = 0;
        for (auto& [key, hs] : groups)
        {
            if (hs.size() > 1)
            {
                ++badGroups;
                juce::String detail;
                for (auto& [h, seqs] : hs)
                {
                    const auto& s = keySnap[key + "/" + juce::String::toHexString ((juce::int64) h)];
                    detail << " [" << seqs[0] << " (+" << juce::String (seqs.size() - 1) << " more): " << f2 (s.outLufs) << " LUFS, drive " << f2 (s.drive) << ", gain " << f2 (s.gainDb) << "]";
                }
                info (juce::String (stem) + " " + key + ": " + juce::String ((int) hs.size()) + " distinct masters for the same final intent:" + detail);
            }
        }
        info (juce::String (stem) + ": " + juce::String (runs) + " orderings, " + juce::String ((int) groups.size()) + " distinct final intents, " + juce::String (badGroups) + " intents with order-dependent output");
        if (badGroups > 0)
            finding ("S1", juce::String ("ORDER_DEPENDENCE_") + stem, juce::String (badGroups) + " of " + juce::String ((int) groups.size()) + " final intents (target + starting point) give different masters depending on click order (" + juce::String (runs) + " orderings of 3 of {STREAM, APPLE, CLUB, KEEP, RAP, POP, BYPASS, ADVANCED})");
    }
    return 0;
}

//==============================================================================
int modeAccum (int, char**)
{
    auto buf = loadCorpus ("bag_peak-3", 12.0);
    struct Pair { const char* a; const char* b; };
    for (auto p : { Pair { "STREAM -14", "CLUB -8" }, Pair { "HIP-HOP / RAP", "R&B / POP" }, Pair { "STREAM -14", "KEEP INPUT" }, Pair { "APPLE -16", "STREAM -14" } })
    {
        Rig r (false, "accum");
        r.load (buf, corpusRate(), "bag_peak-3.wav");
        r.click (p.a);
        const auto first = r.snap();
        int diff = 0;
        double maxDelta = 0.0;
        for (int k = 0; k < 10; ++k)
        {
            r.click (p.b);
            r.click (p.a);
            const auto s = r.snap();
            if (s.hash != first.hash) { ++diff; maxDelta = juce::jmax (maxDelta, std::abs (s.outLufs - first.outLufs)); }
        }
        info (juce::String (p.a) + " <-> " + p.b + " x10: A-state master differed from the first A in " + juce::String (diff) + " of 10 returns, max " + f2 (maxDelta) + " LU");
        if (diff > 0) finding ("S1", juce::String ("ACCUM_") + p.a + "_" + p.b, juce::String (p.a) + " -> " + p.b + " -> " + p.a + " repeated: the A master drifted in " + juce::String (diff) + " of 10 returns (max " + f2 (maxDelta) + " LU)");
    }
    // Knob out and back to the same value.
    {
        Rig r (true, "accum");
        r.load (buf, corpusRate(), "bag_peak-3.wav");
        r.click ("HIP-HOP / RAP");
        const auto first = r.snap();
        for (int k = 0; k < 10; ++k)
        {
            const double v = r.panel->knobValueForTesting ("BELL 1");
            r.panel->turnKnobForTesting ("BELL 1", 4.0);
            r.render();
            r.panel->turnKnobForTesting ("BELL 1", v);
            r.render();
        }
        const auto s = r.snap();
        info ("BELL 1 away and back x10: hash " + juce::String (s.hash == first.hash ? "identical" : "different") + ", " + f2 (s.outLufs - first.outLufs, 3) + " LU");
        if (s.hash != first.hash) finding ("S2", "KNOB_ROUNDTRIP", "BELL 1 moved to +4 dB and back x10 does not restore the master (" + f2 (s.outLufs - first.outLufs, 3) + " LU)");
    }
    return 0;
}
} // namespace

// other modes live in audit_modes2.cpp
int auditModes2 (const juce::String& mode, int argc, char** argv);
int auditFormats (int argc, char** argv);

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc < 2) { std::printf ("audit <mode> ...\n"); return 1; }
    const juce::String mode (argv[1]);
    if (mode == "targets") return modeTargets (argc, argv);
    if (mode == "bypass") return modeBypass (argc, argv);
    if (mode == "random") return modeRandom (argc, argv);
    if (mode == "perm") return modePerm (argc, argv);
    if (mode == "accum") return modeAccum (argc, argv);
    if (mode == "formats") return auditFormats (argc, argv);
    return auditModes2 (mode, argc, argv);
}
