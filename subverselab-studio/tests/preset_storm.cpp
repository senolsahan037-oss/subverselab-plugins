/*  preset_storm: the owner's Ableton test of 20 Oct 2026, offline. JING FANG on a track, two melodies looping from clip slots,
    the host playing, and the sound switched fast and many times in a row: whole presets (prev / next and the menu), the
    Sequence and Sequence - Lab families, and single panels from other presets (the slots). Bug 1: the channel went silent.

        preset_storm [--changes N] [--seed S] [--verbose] [--threaded]

    --threaded: the blocks run on their own thread (about 4x real time) while the changes come from the main thread, as in
    a host (the audio thread and the message thread at the same time).

    After every change a few host blocks play (0..4, the owner's fast clicking), then every 8 changes a probe: a plain
    reference preset is loaded and one bar of the melody is measured against a fresh processor that only loaded that preset.
    A probe more than 12 dB below the fresh one = silence that a new preset does not bring back (bug 1). Also checked: the
    output stays finite, and the last change of the storm is compared the same way.
    Exit code 0 only if every check held.
*/
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cstdio>
#include <random>
#include <thread>
#include <atomic>
#include <mutex>

namespace
{
int gPass = 0, gFail = 0;
void check (bool ok, const juce::String& what)
{
    std::printf ("[%s] %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8());
    std::fflush (stdout);
    ok ? ++gPass : ++gFail;
}

constexpr int kSr = 44100, kBlock = 512;
constexpr double kBpm = 120.0;

// a host that plays: the transport runs, the ppq moves with every block (Ableton with a clip playing)
struct PlayingHost : juce::AudioPlayHead
{
    double ppq = 0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        p.setIsPlaying (true);
        p.setBpm (kBpm);
        p.setPpqPosition (ppq);
        p.setTimeInSamples ((juce::int64) (ppq * 60.0 / kBpm * kSr));
        p.setTimeSignature (juce::AudioPlayHead::TimeSignature { 4, 4 });
        return p;
    }
};

// two melodies, as two clips taking turns: eighth notes, 0.2 s long
struct Melody
{
    static constexpr int a[8] = { 60, 62, 64, 67, 69, 67, 64, 62 };
    static constexpr int b[8] = { 57, 60, 64, 65, 64, 60, 59, 55 };
    long sample = 0;
    void fill (juce::MidiBuffer& m, int n)
    {
        const long step = (long) (kSr * 60.0 / kBpm / 2.0), len = (long) (0.2 * kSr);
        for (int i = 0; i < n; ++i)
        {
            const long t = sample + i;
            const long k = t / step, in = t % step;
            const int key = ((k / 16) % 2 == 0 ? a : b)[k % 8];
            if (in == 0) m.addEvent (juce::MidiMessage::noteOn (1, key, (juce::uint8) 100), i);
            if (in == len) m.addEvent (juce::MidiMessage::noteOff (1, key), i);
        }
        sample += n;
    }
};

// a VST3 host's echo: a value the plug-in writes on the message thread (performEdit) comes back to the processor 1..3 blocks
// later through the host's parameter queue, applied only if it differs (JUCE's setValueAndNotifyIfChanged)
struct HostEcho : juce::AudioProcessorListener
{
    struct E { int index; float value; long due; };
    std::vector<E> queue;
    long blockNow = 0;
    bool on = false, applying = false;
    std::mt19937 rng { 7 };
    void audioProcessorParameterChanged (juce::AudioProcessor*, int index, float v) override
    {
        if (! on || applying) return;
        // the host keeps the order: a later value never arrives before an earlier one
        const long due = std::max (blockNow + 1 + (long) (rng() % 3), queue.empty() ? 0L : queue.back().due);
        queue.push_back ({ index, v, due });
    }
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}
    void apply (juce::AudioProcessor& p)
    {
        applying = true;
        size_t j = 0;
        for (size_t i = 0; i < queue.size(); ++i)
        {
            const auto& e = queue[i];
            if (e.due <= blockNow)
            {
                auto* param = p.getParameters()[e.index];
                if (! juce::approximatelyEqual (param->getValue(), e.value)) param->setValueNotifyingHost (e.value);
            }
            else queue[j++] = e;
        }
        queue.resize (j);
        applying = false;
        ++blockNow;
    }
};

struct Rig
{
    JingFangProcessor proc;
    PlayingHost host;
    Melody mel;
    HostEcho echo;
    juce::AudioBuffer<float> buf { 2, kBlock };
    bool finite = true;
    Rig()
    {
        proc.addListener (&echo);
        proc.setPlayHead (&host);
        proc.setRateAndBufferSizeDetails (kSr, kBlock);
        proc.prepareToPlay (kSr, kBlock);
    }
    ~Rig() { proc.removeListener (&echo); }
    double block (bool messageWork = true)   // one host block; the sum of squares
    {
        juce::MidiBuffer m;
        mel.fill (m, kBlock);
        echo.apply (proc);
        proc.processBlock (buf, m);
        host.ppq += kBlock / (double) kSr * kBpm / 60.0;
        double s = 0;
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < kBlock; ++i)
            {
                const float x = buf.getSample (c, i);
                if (! std::isfinite (x)) { finite = false; continue; }
                s += (double) x * x;
            }
        if (messageWork) proc.runMessageWorkForTest();   // the 30 Hz timer, about once per block
        return s;
    }
    double rmsDb (double seconds)
    {
        const int blocks = (int) std::ceil (seconds * kSr / kBlock);
        double s = 0;
        for (int b = 0; b < blocks; ++b) s += block();
        return 10.0 * std::log10 (s / (2.0 * blocks * kBlock) + 1.0e-20);
    }
};

int findPreset (const JingFangProcessor& p, const juce::String& family, const juce::String& name)
{
    for (size_t i = 0; i < p.presets().size(); ++i)
        if (p.presets()[i].family == family && p.presets()[i].name == name) return (int) i;
    return -1;
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    int changes = 3000; unsigned seed = 20261020; bool verbose = false, threaded = false, compare = false, survey = false, echo = false, withState = false, withEditor = false, slotsOnly = false;
    for (int i = 1; i < argc; ++i)
    {
        const juce::String a (argv[i]);
        if (a == "--changes" && i + 1 < argc) changes = juce::String (argv[++i]).getIntValue();
        else if (a == "--seed" && i + 1 < argc) seed = (unsigned) juce::String (argv[++i]).getLargeIntValue();
        else if (a == "--verbose") verbose = true;
        else if (a == "--threaded") threaded = true;
        else if (a == "--compare") compare = true;
        else if (a == "--survey") survey = true;
        else if (a == "--echo") echo = true;
        else if (a == "--slots") slotsOnly = true;
        else if (a == "--state") withState = true;
        else if (a == "--editor") { withEditor = true; threaded = true; }
    }
    const auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("jf_preset_storm");
    tmp.deleteRecursively(); tmp.createDirectory();
    JingFangProcessor::userFolderOverrideForTests() = tmp;   // never the user's presets

    Rig rig;
    auto& p = rig.proc;
    const int count = (int) p.presets().size();
    const int refIndex = findPreset (p, "Lead", "VA Lead") >= 0 ? findPreset (p, "Lead", "VA Lead") : findPreset (p, "Init", "Init");
    check (refIndex >= 0, "reference preset found (" + p.presets()[(size_t) juce::jmax (0, refIndex)].family + " / " + p.presets()[(size_t) juce::jmax (0, refIndex)].name + ")");
    std::vector<int> seqLab, seqAll;
    for (int i = 0; i < count; ++i)
    {
        if (p.presets()[(size_t) i].family == "Sequence - Lab") seqLab.push_back (i);
        if (p.presets()[(size_t) i].family.startsWith ("Sequence")) seqAll.push_back (i);
    }
    std::printf ("preset_storm: %d presets (%d Sequence, %d Sequence - Lab), %d changes, seed %u%s\n", count, (int) seqAll.size(), (int) seqLab.size(), changes, seed, threaded ? ", threaded" : "");

    // the fresh level of the probe: a new processor, the reference preset, one bar
    double freshDb = 0;
    {
        Rig f;
        f.proc.loadPresetIndex (refIndex);
        f.block();
        freshDb = f.rmsDb (2.0);
        check (freshDb > -40.0, "fresh reference level " + juce::String (freshDb, 1) + " dBFS RMS");
    }

    // ---- the preset list's structure (work order item 2): categories in order, every factory family in one, list sorted by them
    {
        const juce::StringArray want { "Init", "Keys", "Pads", "Leads", "Bass", "Plucks", "Arps", "Sequences", "Maqam", "Drums", "FX" };
        int orphans = 0, backwards = 0;
        for (const auto& e : p.presets()) if (e.factory && JingFangProcessor::categoryOf (e.family) == "Yours") ++orphans;
        for (size_t i = 1; i < p.presets().size(); ++i)
            backwards += want.indexOf (JingFangProcessor::categoryOf (p.presets()[i].family)) < want.indexOf (JingFangProcessor::categoryOf (p.presets()[i - 1].family));
        check (p.categories() == want && orphans == 0 && backwards == 0,
               "preset list: " + p.categories().joinIntoString (", ") + "; factory presets outside a category " + juce::String (orphans)
                   + ", out of order " + juce::String (backwards));
    }

    // ---- two defects found while looking for bug 1 (each failed before its fix)
    {
        // 1. a key released while the arp / seq is off stays "held" inside the note generator: the next arp preset then runs
        //    on that phantom key with no key down (fast switching between an arp preset and a plain one while a clip plays)
        JingFangProcessor q;
        PlayingHost h;
        q.setPlayHead (&h);
        q.setRateAndBufferSizeDetails (kSr, kBlock); q.prepareToPlay (kSr, kBlock);
        juce::AudioBuffer<float> b (2, kBlock);
        auto run = [&] (juce::MidiBuffer m, int blocks)
        {
            double e = 0;
            for (int k = 0; k < blocks; ++k)
            {
                q.processBlock (b, m); m.clear();
                h.ppq += kBlock / (double) kSr * kBpm / 60.0;
                for (int c = 0; c < 2; ++c) e += b.getRMSLevel (c, 0, kBlock) * b.getRMSLevel (c, 0, kBlock);
            }
            return 10.0 * std::log10 (e / (2.0 * blocks) + 1.0e-20);
        };
        const int arp = findPreset (q, "Arp - Classic", "As Played"), plain = refIndex;
        juce::MidiBuffer on, off;
        on.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
        off.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
        q.loadPresetIndex (arp); run ({}, 2);
        run (on, 20);                                          // the key goes down on the arp preset
        q.loadPresetIndex (plain); run ({}, 2);
        run (off, 2);                                          // ... and up on the plain one
        q.loadPresetIndex (arp); run ({}, 260);                // let any tail die (3 s)
        const double phantom = run ({}, 90);                   // about 1 s with no key down
        const bool running = q.engineForTest().noteGen().isRunning();
        check (arp >= 0 && ! running && phantom < -90.0, "no phantom key: an arp preset with no key down after a key left on a plain preset: arp "
                                             + juce::String (running ? "running" : "waiting") + ", " + juce::String (phantom, 1) + " dBFS (silent below -90)");

        // 2. the host prepares again (sample rate / buffer change, the track re-enabled) while a Lab preset plays: the engine's
        //    tuning table went back to 12-equal and the Lab table was never sent again
        JingFangProcessor l;
        l.setPlayHead (&h);
        l.setRateAndBufferSizeDetails (kSr, kBlock); l.prepareToPlay (kSr, kBlock);
        const int lab = findPreset (l, "Sequence - Lab", "Neutral Third on G");
        l.loadPresetIndex (lab);
        juce::MidiBuffer none;
        l.processBlock (b, none);
        std::array<float, 128> before {};
        for (int k = 0; k < 128; ++k) before[(size_t) k] = l.engineForTest().keyPitch (k);
        int moved = 0;
        for (int k = 0; k < 128; ++k) moved += std::abs (before[(size_t) k] - (float) k) > 1.0e-3f;
        l.prepareToPlay (48000, 256);
        l.processBlock (b, none);
        float worstCt = 0;
        for (int k = 0; k < 128; ++k) worstCt = std::max (worstCt, 100.0f * std::abs (l.engineForTest().keyPitch (k) - before[(size_t) k]));
        check (lab >= 0 && l.labActive() && moved > 0 && worstCt < 0.01f,
               "Lab tuning survives a new prepare: " + juce::String (moved) + " keys off 12-equal, largest change after prepare " + juce::String (worstCt, 2) + " ct");
    }

    std::mt19937 rng (seed);
    auto pick = [&] (int n) { return (int) (rng() % (unsigned) n); };
    juce::StringArray history;
    int firstSilent = -1, probes = 0, silentProbes = 0;
    double worst = 0;
    std::vector<juce::MemoryBlock> savedStates;
    rig.echo.on = echo;
    auto change = [&] (int c)
    {
        juce::String what;
        std::vector<juce::MemoryBlock>& saved = savedStates;
        switch (pick (withState ? 10 : 8))
        {
            case 8: { juce::MemoryBlock mb; p.getStateInformation (mb); saved.push_back (mb); what = "host saves the state"; break; }
            case 9: if (! saved.empty()) { const auto& mb = saved[(size_t) pick ((int) saved.size())]; p.setStateInformation (mb.getData(), (int) mb.getSize());
                                           what = "host restores a saved state"; } else what = "nothing"; break;
            case 0: p.stepPreset (1); what = "next"; break;
            case 1: p.stepPreset (-1); what = "prev"; break;
            case 2: case 3: { const int i = pick (count); p.loadPresetIndex (i); what = "menu " + p.presets()[(size_t) i].name; break; }
            case 4: { const int i = seqAll[(size_t) pick ((int) seqAll.size())]; p.loadPresetIndex (i); what = "set " + p.presets()[(size_t) i].name; break; }
            case 5: { const int i = seqLab[(size_t) pick ((int) seqLab.size())]; p.loadPresetIndex (i); what = "lab set " + p.presets()[(size_t) i].name; break; }
            case 6: { const int s = pick (JingFangProcessor::kNumSections), i = pick (count);
                      p.loadSection (s, i); what = juce::String ("slot ") + JingFangProcessor::sectionName (s) + " <- " + p.presets()[(size_t) i].name; break; }
            default: { const int s = pick (JingFangProcessor::kNumSections); p.stepSection (s, pick (2) ? 1 : -1);
                       what = juce::String ("slot step ") + JingFangProcessor::sectionName (s); break; }
        }
        history.add (juce::String (c) + " " + what);
    };
    auto judge = [&] (int c, double db)
    {
        ++probes;
        const double drop = freshDb - db;
        worst = juce::jmax (worst, drop);
        if (verbose) std::printf ("  probe after change %d: %.1f dB (fresh %.1f)\n", c, db, freshDb);
        if (drop <= 12.0) return;
        ++silentProbes;
        if (firstSilent >= 0) return;
        firstSilent = c;
        std::printf ("  SILENT after change %d: %.1f dBFS vs fresh %.1f; the last changes:\n", c, db, freshDb);
        for (int h = juce::jmax (0, history.size() - 16); h < history.size(); ++h) std::printf ("    %s\n", history[h].toRawUTF8());
        const auto& e = p.engineForTest();
        std::printf ("    engine: %d voices active, steals %llu, non-finite resets %llu, dropped events %llu, lab %s\n",
                     e.activeVoices (true), (unsigned long long) e.stats.steals, (unsigned long long) e.stats.nonFiniteResets,
                     (unsigned long long) e.stats.droppedEvents, p.labActive() ? "on" : "off");
        const auto& ps = e.getParams();
        for (int i = 0; i < svl::kNumParams; ++i)
            if (verbose && std::abs (ps[i] - p.exactValue (i)) > 1.0e-6f)
                std::printf ("      engine %s = %g, processor %g\n", svl::paramDefs()[(size_t) i].id.c_str(), (double) ps[i], (double) p.exactValue (i));
    };

    if (survey)
    {   // every preset fresh with the melody and a playing host: which ones are silent on their own
        int quiet = 0;
        for (int i = 0; i < count; ++i)
        {
            Rig f;
            f.proc.loadPresetIndex (i);
            const double db = f.rmsDb (3.0);
            if (db < -50.0) { ++quiet; std::printf ("  quiet fresh: %-24s %-28s %.1f dBFS\n", p.presets()[(size_t) i].family.toRawUTF8(), p.presets()[(size_t) i].name.toRawUTF8(), db); }
        }
        std::printf ("  %d of %d presets below -50 dBFS fresh\n", quiet, count);
        return 0;
    }
    if (slotsOnly)
    {   // only the panels' own steps (the left / right third of a section title): how often the mix itself is silent
        int silent = 0, n = 0;
        p.loadPresetIndex (refIndex);
        for (int c = 1; c <= changes; ++c)
        {
            const int s = pick (JingFangProcessor::kNumSections);
            p.stepSection (s, pick (2) ? 1 : -1);
            const double db = rig.rmsDb (1.0);
            ++n;
            if (db < -60.0)
            {
                ++silent;
                if (silent <= 12)
                {
                    std::printf ("  silent mix after step %d (%s): %.1f dBFS; slots:", c, JingFangProcessor::sectionName (s), db);
                    for (int k = 0; k < JingFangProcessor::kNumSections; ++k)
                        std::printf (" %s=%s", JingFangProcessor::sectionName (k), p.presets()[(size_t) juce::jmax (0, p.sectionSlot (k))].name.toRawUTF8());
                    std::printf ("\n    master %.1f dB, amp A %.3f S %.2f, osc levels %.2f %.2f %.2f types %g %g %g, cutoff %.0f mode %g, seq %g arp %g drums %g/%g, poly %g\n",
                                 p.exactValue (svl::MASTER), p.exactValue (svl::AENV_A), p.exactValue (svl::AENV_S),
                                 p.exactValue (svl::oscParam (0, svl::O_LEVEL)), p.exactValue (svl::oscParam (1, svl::O_LEVEL)), p.exactValue (svl::oscParam (2, svl::O_LEVEL)),
                                 p.exactValue (svl::oscParam (0, svl::O_TYPE)), p.exactValue (svl::oscParam (1, svl::O_TYPE)), p.exactValue (svl::oscParam (2, svl::O_TYPE)),
                                 p.exactValue (svl::FLT_CUTOFF), p.exactValue (svl::FLT_MODE), p.exactValue (svl::SEQ_ON), p.exactValue (svl::ARP_ON),
                                 p.exactValue (svl::DRUMS_ON), p.exactValue (svl::DRUMS_MODE), p.exactValue (svl::V_POLY));
                }
            }
        }
        std::printf ("  slot steps: %d, silent mixes %d (%.1f %%)\n", n, silent, 100.0 * silent / juce::jmax (1, n));
        return 0;
    }
    if (compare)
    {
        // after a whole-preset change: does that preset sound as it does in a fresh processor (same melody, same host time)?
        int compared = 0;
        for (int c = 1; c <= changes; ++c)
        {
            change (c);
            const int blocks = pick (5);
            for (int b = 0; b < blocks; ++b) rig.block();
            const int idx = p.currentPresetIndex();
            if (idx < 0 || pick (4) != 0) continue;
            Rig f;
            f.mel.sample = rig.mel.sample; f.host.ppq = rig.host.ppq;
            f.proc.loadPresetIndex (idx);
            const double fresh = f.rmsDb (1.5), here = rig.rmsDb (1.5);
            ++compared;
            if (fresh < -60.0) continue;                         // a preset this melody does not play (a drum key, a waiting sequence)
            if (fresh - here > 12.0)
            {
                ++silentProbes;
                if (firstSilent < 0)
                {
                    firstSilent = c;
                    std::printf ("  SILENT after change %d (%s / %s): %.1f dBFS vs fresh %.1f; the last changes:\n", c,
                                 p.presets()[(size_t) idx].family.toRawUTF8(), p.presets()[(size_t) idx].name.toRawUTF8(), here, fresh);
                    for (int h = juce::jmax (0, history.size() - 16); h < history.size(); ++h) std::printf ("    %s\n", history[h].toRawUTF8());
                    const auto& ps = p.engineForTest().getParams();
                    const auto& fs = f.proc.engineForTest().getParams();
                    for (int i = 0; i < svl::kNumParams; ++i)
                        if (std::abs (ps[i] - fs[i]) > 1.0e-6f)
                            std::printf ("      %s: here %g, fresh %g\n", svl::paramDefs()[(size_t) i].id.c_str(), (double) ps[i], (double) fs[i]);
                    std::printf ("      lab here %s, fresh %s\n", p.labActive() ? "on" : "off", f.proc.labActive() ? "on" : "off");
                }
            }
            worst = juce::jmax (worst, fresh - here);
        }
        probes = compared;
    }
    else if (! threaded)
    {
        for (int c = 1; c <= changes; ++c)
        {
            change (c);
            const int blocks = pick (5);
            for (int b = 0; b < blocks; ++b) rig.block();
            if (c % 8 == 0 || c == changes)
            {   // probe: the reference preset, as a user would load "a sound that surely plays"
                p.loadPresetIndex (refIndex);
                rig.block();
                judge (c, rig.rmsDb (2.0));
            }
        }
    }
    else
    {
        // the audio thread: blocks at about 4x real time; each block's sum of squares into a running total
        std::atomic<bool> stop { false };
        std::atomic<long> blocksDone { 0 };
        std::mutex energyLock;
        std::vector<double> energy;
        energy.reserve (1 << 22);
        std::thread audio ([&]
        {
            while (! stop.load())
            {
                const double e = rig.block (false);
                { std::lock_guard<std::mutex> g (energyLock); energy.push_back (e); }
                blocksDone.fetch_add (1);
                std::this_thread::sleep_for (std::chrono::microseconds (2900));
            }
        });
        auto waitBlocks = [&] (long n) { const long until = blocksDone.load() + n; while (blocksDone.load() < until) std::this_thread::sleep_for (std::chrono::milliseconds (1)); };
        auto measure = [&] (double seconds)
        {
            const long nb = (long) std::ceil (seconds * kSr / kBlock);
            const long from = blocksDone.load();
            waitBlocks (nb);
            std::lock_guard<std::mutex> g (energyLock);
            double s = 0;
            for (long b = from; b < from + nb; ++b) s += energy[(size_t) b];
            return 10.0 * std::log10 (s / (2.0 * nb * kBlock) + 1.0e-20);
        };
        // --editor: the panel is open and the message loop runs (its timers, the processor's 30 Hz timer), as in a host
        std::unique_ptr<juce::AudioProcessorEditor> ed;
        if (withEditor) { ed.reset (p.createEditor()); ed->setSize (ed->getWidth(), ed->getHeight()); }
        auto idle = [&] (int ms)
        {
            if (withEditor) juce::MessageManager::getInstance()->runDispatchLoopUntil (juce::jmax (1, ms));
            else { p.runMessageWorkForTest(); std::this_thread::sleep_for (std::chrono::milliseconds (ms)); }
        };
        for (int c = 1; c <= changes; ++c)
        {
            change (c);
            idle (pick (40));   // 0..40 ms between clicks
            if (c % 8 == 0 || c == changes)
            {
                p.loadPresetIndex (refIndex);
                waitBlocks (2);
                judge (c, measure (2.0));
            }
        }
        stop.store (true);
        audio.join();
        ed.reset();
    }
    check (rig.finite, "the output stayed finite through the storm");
    check (silentProbes == 0, juce::String (probes) + " probes, " + juce::String (silentProbes) + " silent (worst drop "
                                  + juce::String (worst, 1) + " dB" + (firstSilent > 0 ? ", first after change " + juce::String (firstSilent) : juce::String()) + ")");
    tmp.deleteRecursively();
    std::printf ("preset_storm: %d passed, %d failed\n", gPass, gFail);
    return gFail == 0 ? 0 : 1;
}
