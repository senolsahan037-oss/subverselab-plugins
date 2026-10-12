/*  MIHENK v2 usability checks: the panel and the processor as the owner uses them (no screen, no audio device).

        ui_check            all checks (exit code 0 only if every check passes)

    - SIMPLE / ADVANCED face: SIMPLE for a new user, the choice saved on this computer and read back.
    - PLAN dials (v3, SPEC_V3): the knobs show the plan (trim and drive "PLAN", threshold / release AUTO); a hand turn is an
      override (gold ring, a delta over the plan) that RESET OVERRIDES drops; the lamps for level are displays; the SIMPLE macros
      are read-only; the CHARACTER caps change the intent.
    - Reference: its controls are hidden until one is loaded; MATCH TO REFERENCE sets tone and loudness in one step.
    - A / B: level-matched by default (the original at the master's loudness), raw levels when switched off; the
      plug-in path and its saved state; no allocation in processBlock with A / B.
    - KEEP INPUT through the plug-in parameters.
    - The batch queue runs on its own thread while the panel keeps working, SAME and PER TRACK.
    Every file goes to a temp folder: the user's preferences, presets and profile are not touched.
*/
#include <JuceHeader.h>
#include <atomic>
#include <cstdlib>
#include <new>
#include "../Source/app/MainComponent.h"
#include "../Source/plugin/PluginProcessor.h"
#include "../Source/plugin/Params.h"
#include "../Source/engine/AudioIO.h"
#include "../Source/dsp/Filters.h"
#include "../Source/dsp/Spectrum.h"
#include "../Source/engine/Preset.h"

using namespace mihenk;

namespace
{
thread_local bool inAudio = false;
std::atomic<long> audioAllocs { 0 };
}
void* operator new (std::size_t n)
{
    if (inAudio) audioAllocs.fetch_add (1);
    if (void* p = std::malloc (n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}
void* operator new[] (std::size_t n)
{
    if (inAudio) audioAllocs.fetch_add (1);
    if (void* p = std::malloc (n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}
void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }

namespace
{
int failures = 0;
void check (bool ok, const juce::String& what)
{
    std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8());
    std::fflush (stdout);
    if (! ok) ++failures;
}
void section (const char* name) { std::printf ("\n-- %s\n", name); }
using Buffer = juce::AudioBuffer<float>;

/** A kick / bass / noise mix with a quiet verse and a loud chorus. */
Buffer mixSignal (double fs, double secs, uint32_t seed, double tilt = 0.3)
{
    const int n = (int) (secs * fs);
    Buffer b (2, n);
    juce::Random rng ((juce::int64) seed);
    double lp[2] { 0.0, 0.0 };
    for (int i = 0; i < n; ++i)
    {
        const double t = i / fs, tb = std::fmod (t, 0.5), section = std::fmod (t, 8.0) < 4.0 ? 0.35 : 1.0;
        const double kick = std::exp (-tb * 16.0) * std::sin (2.0 * kPi * (50.0 * tb + 2.0 * (1.0 - std::exp (-tb * 30.0))));
        const double bass = 0.2 * std::sin (2.0 * kPi * 55.0 * t);
        for (int c = 0; c < 2; ++c)
        {
            const double w = rng.nextDouble() * 2.0 - 1.0;
            lp[c] += 0.08 * (w - lp[c]);
            const double pad = (1.0 - tilt) * lp[c] * 0.8 + tilt * (w - lp[c]) * 0.25;
            b.setSample (c, i, (float) (section * (0.5 * kick + bass + 0.3 * pad)));
        }
    }
    return b;
}

/** An unmastered mix (peaks far above the average loudness, not a finished master), quiet: about -24 LUFS. */
Buffer openSignal (double fs, double secs, uint32_t seed)
{
    auto b = mixSignal (fs, secs, seed);
    b.applyGain (0.35f);
    return b;
}

/** A loud production: limited to about -9 LUFS at -0.3 dBTP. */
Buffer hotMix (double fs, double secs, uint32_t seed)
{
    auto raw = mixSignal (fs, secs, seed);
    MasterSettings h;
    h.targetPreset = TargetPreset::custom; h.targetLufs = -9.0; h.limiter.ceilingDbtp = -0.3; h.limiter.releaseMs = 60.0;
    PreLimiterCache c;
    RenderResult r;
    MasterChain::matchTarget (raw, fs, h, c, &r);
    return *r.output;
}

/** Plays `secs` of the player (A or B) at the file rate and measures what comes out. */
double playedLoudness (Player& p, double fs, double secs)
{
    p.setPosition (0);
    p.play();
    const int block = 512, n = (int) (secs * fs);
    Buffer out (2, n);
    out.clear();   // the tail past the last whole block is never written
    Buffer b (2, block);
    for (int pos = 0; pos + block <= n; pos += block)
    {
        juce::AudioSourceChannelInfo info (&b, 0, block);
        p.getNextAudioBlock (info);
        for (int c = 0; c < 2; ++c) out.copyFrom (c, pos, b, c, 0, block);
    }
    p.stop();
    return integratedLoudness (out, fs);
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    const double fs = 48000.0;
    const auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("mihenk_ui_check");
    tmp.deleteRecursively();
    tmp.createDirectory();
    const auto prefs = tmp.getChildFile ("ui.json");
    ui::UiPrefs::setFileForTesting (prefs);

    section ("SIMPLE / ADVANCED face");
    {
        MihenkProcessor p;
        p.setStandaloneModeForTesting (true);
        p.getSession().profile.setFolder (tmp.getChildFile ("profile"));
        {
            MainComponent panel (p);
            panel.setPresetsFolder (tmp.getChildFile ("presets"));
            check (! panel.isAdvancedFace() && panel.knobVisibleForTesting ("LOW END") && panel.knobVisibleForTesting ("TONE") && panel.knobVisibleForTesting ("GLUE")
                       && panel.knobVisibleForTesting ("WIDTH") && panel.knobVisibleForTesting ("AIR") && ! panel.knobVisibleForTesting ("BELL 1")
                       && ! panel.knobVisibleForTesting ("LOOK-AHEAD") && panel.isComponentVisibleForTesting ("chain") && ! panel.isComponentVisibleForTesting ("compThreeBand"),
                   "A new user gets the SIMPLE face: five macros (LOW END, TONE, AIR, GLUE, WIDTH) and the chain readout; the module controls are hidden");
            check (panel.knobVisibleForTesting ("TARGET") && panel.knobVisibleForTesting ("LEVEL") && panel.knobVisibleForTesting ("CEILING")
                       && panel.isComponentVisibleForTesting ("keep") && panel.isComponentVisibleForTesting ("abMatch"),
                   "SIMPLE keeps the target (with KEEP INPUT), the planned LEVEL, the ceiling and A/B MATCHED");
            panel.setFace (true);
            check (panel.isAdvancedFace() && panel.knobVisibleForTesting ("BELL 1") && panel.knobVisibleForTesting ("LOOK-AHEAD") && ! panel.knobVisibleForTesting ("LOW END")
                       && panel.isComponentVisibleForTesting ("compThreeBand") && ! panel.isComponentVisibleForTesting ("chain") && prefs.loadFileAsString().contains ("advanced"),
                   "ADVANCED shows every module control and the choice is written to the preferences file");
        }
        MainComponent again (p);
        check (again.isAdvancedFace(), "A new panel (next launch, another plug-in window) opens on the saved ADVANCED face");
        again.setFace (false);
        MainComponent third (p);
        check (! third.isAdvancedFace(), "... and on SIMPLE once SIMPLE is chosen again");
    }

    section ("PLAN dials: the knobs show the plan, a hand turn is an override with a ring, RESET OVERRIDES drops it");
    {
        MihenkProcessor p;
        p.setStandaloneModeForTesting (true);
        auto& s = p.getSession();
        s.profile.setFolder (tmp.getChildFile ("profile2"));
        MainComponent panel (p);
        panel.setPresetsFolder (tmp.getChildFile ("presets"));
        panel.setFace (true, false);
        const double before = panel.knobValueForTesting ("TRIM");
        s.setInputForTesting (openSignal (fs, 24.0, 7), fs, "open.wav");
        panel.clickCharacterForTesting (PlanCharacter::GENTLE);
        s.renderNowForTesting();
        panel.setFace (true, false);      // the change message's resync, synchronously
        const double plannedTrim = s.plan.settings.trimDb;
        panel.tickForTesting (1);
        const double mid = panel.knobValueForTesting ("TRIM");
        panel.tickForTesting (40);
        const double settled = panel.knobValueForTesting ("TRIM");
        check (! s.plan.finishedMaster && std::abs (plannedTrim) > 1.0 && std::abs (before) < 1e-9 && std::abs (settled - plannedTrim) <= 0.05,
               "TRIM glides to the planned trim: 0.0 -> " + juce::String (mid, 2) + " after one tick -> " + juce::String (settled, 2) + " dB (plan " + juce::String (plannedTrim, 2) + ")");
        check (panel.autoLampForTesting ("TRIM") && panel.knobTextForTesting ("TRIM").endsWith ("PLAN") && panel.autoLampForTesting ("DRIVE") && panel.knobTextForTesting ("DRIVE").endsWith ("PLAN"),
               "TRIM and DRIVE are plan displays: lamp lit, readouts \"" + panel.knobTextForTesting ("TRIM") + "\" / \"" + panel.knobTextForTesting ("DRIVE") + "\"");
        check (panel.autoLampForTesting ("THRESHOLD") && std::abs (panel.knobValueForTesting ("THRESHOLD") - s.glue.thresholdDb) <= 0.05 && panel.knobTextForTesting ("THRESHOLD").endsWith ("DB AUTO"),
               "THRESHOLD shows the solved threshold: \"" + panel.knobTextForTesting ("THRESHOLD") + "\" (solved " + juce::String (s.glue.thresholdDb, 2) + " dBFS)");
        check (panel.autoLampForTesting ("RELEASE") && panel.knobTextForTesting ("RELEASE").endsWith ("MS AUTO"), "RELEASE: AUTO REL lamp lit, \"" + panel.knobTextForTesting ("RELEASE") + "\"");
        check (std::abs (s.outMeas.integrated - s.plan.settings.targetLufs) <= 0.3 || s.plan.targetUnreachable,
               "The master lands on the plan: " + fmt (s.outMeas.integrated, 2) + " LUFS for the target " + fmt (s.plan.settings.targetLufs, 1));

        // A hand turn: an override over the plan, with a ring; the plan itself does not move.
        panel.turnKnobForTesting ("TRIM", std::round ((plannedTrim - 1.5) * 10.0) / 10.0);   // the knob steps in 0.1 dB
        check (s.isOverridden ("trim") && panel.overrideRingForTesting ("TRIM") && std::abs (s.overrideDelta ("trim") + 1.5) < 0.06 && std::abs (s.basePlan.settings.trimDb - plannedTrim) < 1e-9
                   && ! panel.knobTextForTesting ("TRIM").contains ("PLAN"),
               "Turning TRIM by hand: an override of " + fmt (s.overrideDelta ("trim"), 2) + " dB over the plan, ring on, the plan's own trim unchanged");
        panel.turnKnobForTesting ("THRESHOLD", -20.0);
        panel.turnKnobForTesting ("RELEASE", 300.0);
        check (s.isOverridden ("compAuto") && s.isOverridden ("autoRel") && ! s.settings.comp.autoThreshold && ! s.settings.comp.autoRelease && ! panel.autoLampForTesting ("THRESHOLD")
                   && panel.knobTextForTesting ("THRESHOLD").endsWith ("DBFS"),
               "Turning THRESHOLD / RELEASE by hand overrides their AUTO (\"" + panel.knobTextForTesting ("THRESHOLD") + "\", \"" + panel.knobTextForTesting ("RELEASE") + "\")");
        panel.clickAutoLampForTesting ("DRIVE");
        panel.clickAutoLampForTesting ("TRIM");
        check (s.isOverridden ("trim") && ! s.settings.targetLock && ! s.settings.autoTrim && s.settings.planned,
               "The level lamps are displays: clicking them switches nothing on or off");
        panel.clickResetOverridesForTesting();
        s.renderNowForTesting();
        panel.setFace (true, false);
        panel.tickForTesting (40);
        check (s.overrideCount() == 0 && ! panel.overrideRingForTesting ("TRIM") && std::abs (panel.knobValueForTesting ("TRIM") - plannedTrim) <= 0.05 && panel.autoLampForTesting ("THRESHOLD") && s.settings.comp.autoThreshold,
               "RESET OVERRIDES: no ring, TRIM shows the plan " + juce::String (plannedTrim, 2) + " dB again, THRESHOLD is AUTO again");

        // The CHARACTER caps are intent: switching them recomputes the plan and keeps the deltas.
        panel.turnKnobForTesting ("AIR", 1.0);
        const bool airOver = s.isOverridden ("airDb");
        panel.clickCharacterForTesting (PlanCharacter::OFF);
        panel.clickCharacterForTesting (PlanCharacter::FULL);
        check (airOver && s.intent.character == PlanCharacter::FULL && s.isOverridden ("airDb") && std::abs (s.settings.eq.airDb - (s.basePlan.settings.eq.airDb + s.overrideDelta ("airDb"))) < 1e-3,
               "CHARACTER OFF then FULL: the AIR delta is kept over the new plan (" + fmt (s.settings.eq.airDb, 2) + " dB = plan " + fmt (s.basePlan.settings.eq.airDb, 2) + " + " + fmt (s.overrideDelta ("airDb"), 2) + ")");
        panel.clickResetOverridesForTesting();

        // 3-BAND: the per-band AUTO threshold.
        s.settings.comp.mode = CompMode::threeBand;
        s.settings.comp.enabled = true;
        s.settingsChanged();
        panel.selectBand (2);
        s.renderNowForTesting();
        panel.setFace (true, false);
        panel.tickForTesting (40);
        const double hiThr = s.glue.bands[2].thresholdDb;
        check (panel.autoLampForTesting ("THRESHOLD") && std::abs (panel.knobValueForTesting ("THRESHOLD") - hiThr) <= 0.05,
               "3-BAND, HIGH band selected: THRESHOLD shows that band's solved " + juce::String (hiThr, 2) + " dBFS with its AUTO lamp");
        panel.turnKnobForTesting ("THRESHOLD", -30.0);
        check (s.isOverridden ("b3Auto") && ! s.isOverridden ("b1Auto") && ! s.isOverridden ("b2Auto") && ! s.settings.comp.bands[2].autoThreshold && s.settings.comp.bands[0].autoThreshold && s.settings.comp.bands[1].autoThreshold
                   && ! panel.autoLampForTesting ("THRESHOLD"),
               "Turning it overrides AUTO for the HIGH band only (LOW and MID stay AUTO)");
        panel.selectBand (0);
        check (panel.autoLampForTesting ("THRESHOLD"), "Selecting the LOW band shows its AUTO lamp lit");
        panel.clickResetOverridesForTesting();

        // SIMPLE: the macros are plan displays, read-only.
        panel.setFace (false, false);
        panel.tickForTesting (40);
        const int ovBefore = s.overrideCount();
        panel.turnKnobForTesting ("GLUE", 3.0);
        panel.turnKnobForTesting ("AIR", 5.0);
        panel.turnKnobForTesting ("LEVEL", -9.0);
        panel.setFace (false, false);       // resync: a disabled dial the test forced shows the plan again
        panel.tickForTesting (40);
        check (s.overrideCount() == ovBefore && std::abs (panel.knobValueForTesting ("LEVEL") - (s.settings.trimDb + s.settings.limiter.driveDb)) <= 0.05,
               "SIMPLE face: the five macros and LEVEL are read-only plan displays; a turn writes nothing (LEVEL shows " + juce::String (panel.knobValueForTesting ("LEVEL"), 2) + " dB)");
    }

    section ("Reference: hidden until loaded, MATCH TO REFERENCE in one step");
    {
        MihenkProcessor p;
        p.setStandaloneModeForTesting (true);
        auto& s = p.getSession();
        s.profile.setFolder (tmp.getChildFile ("profile3"));
        MainComponent panel (p);
        panel.setPresetsFolder (tmp.getChildFile ("presets"));
        panel.setFace (true, false);
        s.setInputForTesting (openSignal (fs, 16.0, 11), fs, "mix.wav");
        s.renderNowForTesting();
        panel.setFace (true, false);
        check (! panel.isComponentVisibleForTesting ("matchRef") && ! panel.isComponentVisibleForTesting ("clearRef") && ! panel.knobVisibleForTesting ("TONE AMOUNT")
                   && ! panel.knobVisibleForTesting ("SMOOTH") && panel.isComponentVisibleForTesting ("referencePanel"),
               "No reference: MATCH TO REFERENCE, CLEAR, TONE AMOUNT and SMOOTH are hidden (the module shows only its hint)");
        auto ref = mixSignal (fs, 16.0, 12, 0.7);
        ref.applyGain (2.0f);
        s.setReferenceForTesting (std::move (ref), fs, "ref.wav");
        panel.setFace (true, false);
        check (panel.isComponentVisibleForTesting ("matchRef") && panel.isComponentVisibleForTesting ("clearRef") && panel.knobVisibleForTesting ("TONE AMOUNT"),
               "With a reference: MATCH TO REFERENCE, CLEAR and the tone knobs appear");
        s.matchToReference();
        s.renderNowForTesting();
        const double before = s.toneDistanceBefore(), now = s.toneDistanceNow();
        check (! s.settings.eq.matchCurveDb.empty() && s.intent.character == PlanCharacter::FULL && s.settings.targetPreset == TargetPreset::custom
                   && std::abs (s.plan.settings.targetLufs - std::round (s.refMeas.integrated * 10.0) / 10.0) <= 0.06
                   && (std::abs (s.outMeas.integrated - s.refMeas.integrated) <= 0.3 || s.plan.targetUnreachable)
                   && now < 0.8 * before && ui::ReferencePanel::summary (s).startsWith ("TONE "),
               "MATCH TO REFERENCE: tone " + juce::String (before, 2) + " -> " + juce::String (now, 2) + " dB RMS and loudness " + fmt (s.outMeas.integrated, 2) + " vs ref "
                   + fmt (s.refMeas.integrated, 2) + " in one step; summary \"" + ui::ReferencePanel::summary (s) + "\"");
        s.clearReference();
        panel.setFace (true, false);
        check (! panel.isComponentVisibleForTesting ("matchRef") && ! panel.knobVisibleForTesting ("TONE AMOUNT"), "CLEAR hides the reference controls again");
    }

    section ("A / B level-matched by default; raw on request");
    {
        MihenkProcessor p;
        p.setStandaloneModeForTesting (true);
        auto& s = p.getSession();
        s.profile.setFolder (tmp.getChildFile ("profile4"));
        s.setInputForTesting (hotMix (fs, 16.0, 21), fs, "hot.wav");
        s.renderNowForTesting();
        auto& pl = s.player;
        pl.setListenOriginal (false);
        const double b = playedLoudness (pl, fs, 14.0);
        pl.setListenOriginal (true);
        const double a = playedLoudness (pl, fs, 14.0);
        check (s.abLevelMatched() && std::abs (a - b) <= 0.1,
               "LEVEL-MATCHED (default): the original plays at " + fmt (a, 2) + " LUFS, the master at " + fmt (b, 2) + " (input " + fmt (s.inMeas.integrated, 2) + ")");
        p.setAbLevelMatched (false);
        const double aRaw = playedLoudness (pl, fs, 14.0);
        check (! s.abLevelMatched() && aRaw - b > 4.0 && std::abs ((aRaw - b) - (s.inMeas.integrated - s.outMeas.integrated)) < 0.2,
               "Raw levels: the original plays at its own " + fmt (aRaw, 2) + " LUFS, " + fmt (aRaw - b, 1) + " dB louder than the master");
        p.setAbLevelMatched (true);
        const double aBack = playedLoudness (pl, fs, 14.0);
        check (std::abs (aBack - b) <= 0.1, "Back to LEVEL-MATCHED: " + fmt (aBack, 2) + " LUFS");
    }
    {
        // Plug-in: A = the input at the learned output's loudness; the switch is saved with the project.
        MihenkProcessor p;
        p.setStandaloneModeForTesting (false);
        p.setRateAndBufferSizeDetails (fs, 512);
        p.prepareToPlay (fs, 512);
        auto& s = p.getSession();
        s.profile.setFolder (tmp.getChildFile ("profile5"));
        s.setInputForTesting (hotMix (fs, 12.0, 31), fs, "hot.wav");
        s.renderNowForTesting();
        p.publishNow();
        const float matched = p.effectiveListenGain();
        p.setAbLevelMatched (false);
        const float raw = p.effectiveListenGain();
        juce::MemoryBlock state;
        p.getStateInformation (state);
        MihenkProcessor q;
        q.setStandaloneModeForTesting (false);
        q.getSession().profile.setFolder (tmp.getChildFile ("profile5"));
        q.setStateInformation (state.getData(), (int) state.getSize());
        check (gainToDb (matched) < -4.0 && raw == 1.0f && ! q.isAbLevelMatched(),
               "Plug-in A/B: matched gain " + juce::String (gainToDb (matched), 2) + " dB, raw 1.0; the RAW choice comes back with the saved state");
        // processBlock with A selected, both ways: no C++ heap allocation on the audio thread.
        p.setListenOriginal (true);
        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer midi;
        auto src = mixSignal (fs, 4.0, 41);
        audioAllocs = 0;
        for (int k = 0; k < 200; ++k)
        {
            for (int c = 0; c < 2; ++c) buf.copyFrom (c, 0, src, c, (k * 512) % (src.getNumSamples() - 512), 512);
            if (k == 100) p.setAbLevelMatched (true);
            inAudio = true;
            p.processBlock (buf, midi);
            inAudio = false;
        }
        check (audioAllocs.load() == 0, "processBlock with A / B switched between raw and matched: " + juce::String (audioAllocs.load()) + " allocations");
        // KEEP INPUT reaches the plug-in's parameters and comes back.
        s.settings.targetPreset = TargetPreset::keepInput;
        p.getBridge().write (s.settings, false);
        MasterSettings back;
        p.getBridge().read (back);
        check (back.targetPreset == TargetPreset::keepInput, "KEEP INPUT is a value of the plug-in's automatable Target parameter (written and read back)");
    }

    section ("Batch queue: its own thread, the panel stays usable");
    {
        MihenkProcessor p;
        p.setStandaloneModeForTesting (true);
        auto& s = p.getSession();
        s.profile.setFolder (tmp.getChildFile ("profile6"));
        MainComponent panel (p);
        panel.setPresetsFolder (tmp.getChildFile ("presets"));
        juce::Array<juce::File> files;
        for (int i = 0; i < 3; ++i)
        {
            auto b = mixSignal (fs, 10.0, 50 + (uint32_t) i, 0.2 + 0.2 * i);
            b.applyGain (0.35f * (float) dbToGain (-2.0 * i));       // unmastered, so the plan may do more than level and ceiling
            const auto f = tmp.getChildFile ("q" + juce::String (i + 1) + ".wav");
            juce::String err;
            exportWav (f, b, fs, 24, err);
            files.add (f);
        }
        auto& bp = panel.getBatchPanel();
        panel.showBatchPanel (true);
        bp.addFiles (files);
        bp.addFiles (files);      // the same files again are not queued twice
        const bool measured = bp.waitForTesting (60000);
        bp.setOutputFolder (tmp.getChildFile ("out_same"));
        bp.setModes (false, false);
        bp.start();
        const bool runningNow = bp.isRunning();
        // Meanwhile the panel's own source renders and a knob moves.
        s.setInputForTesting (openSignal (fs, 8.0, 60), fs, "meanwhile.wav");
        s.renderNowForTesting();
        panel.setFace (true, false);
        panel.turnKnobForTesting ("AIR", 1.5);
        const bool usable = std::abs (s.settings.eq.airDb - 1.5) < 1e-6 && s.outMeas.valid && s.outMeas.integrated <= -14.0 + 0.2;
        const bool done = bp.waitForTesting (300000);
        const auto& r = bp.lastResult();
        int ok = 0;
        for (auto& t : r.tracks) ok += t.ok && t.output.existsAsFile() ? 1 : 0;
        check (measured && bp.numRows() == 3 && runningNow && usable && done && ok == 3 && r.csv.existsAsFile() && r.json.existsAsFile(),
               "SAME FOR ALL: 3 queued (no duplicates), started in the background, the panel rendered and took a knob move meanwhile, 3 masters + CSV/JSON written");
        // PER TRACK: track 1 keeps stored settings, the others get the panel's settings plus their own diagnosis.
        s.settings.eq.airDb = 3.0;
        s.settingsChanged();
        bp.setModes (true, false);
        bp.selectRow (0);
        bp.storePanelSettingsInSelected();
        s.settings.eq.airDb = 0.0;
        s.settingsChanged();
        bp.setOutputFolder (tmp.getChildFile ("out_per_track"));
        bp.start();
        bp.waitForTesting (300000);
        const auto& r2 = bp.lastResult();
        const auto rep1 = r2.tracks.size() == 3 ? r2.tracks[0].output.getSiblingFile (r2.tracks[0].output.getFileNameWithoutExtension() + ".mihenk.txt").loadFileAsString() : juce::String();
        check (r2.tracks.size() == 3 && r2.tracks[0].settingsMode == "own" && r2.tracks[1].settingsMode == "panel + diagnosis" && rep1.contains ("AIR 3.0 dB"),
               "PER TRACK: track 1 used its stored settings (AIR +3.0 dB in its report), tracks 2-3 the panel's plus their own diagnosis");
    }

    section ("Behaviour fixes: standalone BYPASS without a render, RESET ALL, the automatic skip for finished input");
    {
        MihenkProcessor p;
        p.setStandaloneModeForTesting (true);
        auto& s = p.getSession();
        s.profile.setFolder (tmp.getChildFile ("profile7"));
        s.setInputForTesting (mixSignal (fs, 12.0, 71, 0.4), fs, "bypass.wav");
        s.renderNowForTesting();
        const auto* master = s.output.get();
        const double procLufs = s.outMeas.integrated;
        s.settings.bypass = true;
        s.settingsChanged();
        const bool onNow = s.player.isBypassed() && ! s.busy() && s.output.get() == master && std::abs (s.outMeas.integrated - s.inMeas.integrated) < 1.0e-9;
        s.settings.bypass = false;
        s.settingsChanged();
        const bool offNow = ! s.player.isBypassed() && ! s.busy() && s.output.get() == master && std::abs (s.outMeas.integrated - procLufs) < 1.0e-9;
        check (onNow && offNow, "Standalone BYPASS is a switch in the player: no job queued, the rendered master stays loaded, the table shows the input while bypassed and the master again after");

        // RESET ALL: the intent, every override, the tone-match curve and the extras are those of a new instance; the parameters follow.
        s.intent.target = PlanTarget::CLUB_8;
        s.intent.character = PlanCharacter::FULL;
        s.settings.eq.matchCurveDb.assign (spectrumGrid().size(), 1.0f);       // a write the plan does not own: dropped by the next plan
        s.settings.eq.bell2Db = 4.0;
        s.settingsChanged();
        const bool wasChanged = s.overrideCount() > 0 && s.intent.target == PlanTarget::CLUB_8;
        p.resetAll();
        MasterSettings back;
        p.getBridge().read (back);
        const auto& e = s.settings.eq;
        const bool settingsFresh = e.matchCurveDb.empty() && e.bell2Db == 0.0 && s.overrideCount() == 0 && s.intent.target == PlanTarget::STREAM_14 && s.intent.character == PlanCharacter::OFF
                                   && s.settings.limiter.maxDriveDb == std::max (0.0, s.settings.limiter.driveDb) && s.settings.planned && ! s.settings.targetLock;
        check (wasChanged && settingsFresh && std::abs (back.eq.bell2Db) < 1e-9 && s.input != nullptr,
               "RESET ALL: intent, overrides and the extras outside the parameters are a new instance's, the parameters follow, the loaded file stays in the app");
    }
    {
        // A finished master (a loud, limited mix): level and ceiling only, whatever CHARACTER says; the status says why.
        MihenkProcessor p;
        p.setStandaloneModeForTesting (true);
        auto& s = p.getSession();
        s.profile.setFolder (tmp.getChildFile ("profile8"));
        s.setInputForTesting (hotMix (fs, 14.0, 31), fs, "finished.wav");
        s.intent.character = PlanCharacter::FULL;
        s.settingsChanged();
        s.renderNowForTesting();
        const bool level = s.plan.finishedMaster && ! s.settings.eq.enabled && ! s.settings.comp.enabled && ! s.settings.width.enabled && s.settings.eq.matchCurveDb.empty();
        s.settings.eq.bell2Db = 3.0;
        s.settingsChanged();                                      // a hand turn on a finished master is refused
        check (s.inMeas.integrated >= -14.3 && level && std::abs (s.settings.eq.bell2Db) < 1e-9 && s.status.contains ("finished master"),
               "Input already at the target (" + fmt (s.inMeas.integrated, 1) + " LUFS): level + ceiling only with CHARACTER FULL, a hand turn is refused, the status says why: \"" + s.status.substring (0, 160) + "\"");
    }

    section ("Double-click on a knob goes to the parameter's default (plug-in and app use the same Def)");
    {
        MihenkProcessor p;
        p.setStandaloneModeForTesting (true);
        p.getSession().profile.setFolder (tmp.getChildFile ("profile3"));
        MainComponent panel (p);
        panel.setPresetsFolder (tmp.getChildFile ("presets"));
        panel.setFace (true, false);
        int withId = 0, noId = 0, bad = 0, changed = 0;
        juce::String badList, changedList;
        for (int i = 0; i < panel.knobCountForTesting(); ++i)
        {
            juce::String label, id; double scale = 1, fixedDef = 0, target = 0; bool stepped = false;
            if (! panel.knobDoubleClickInfoForTesting (i, label, id, scale, fixedDef, target, stepped)) continue;
            const auto* d = id.isNotEmpty() ? mihenk::params::findDef (id) : nullptr;
            if (d == nullptr) { ++noId; continue; }
            ++withId;
            const double dv = d->defaultValue();
            const double expect = stepped ? (double) juce::roundToInt (dv) + 0.5 : dv * scale;
            if (std::abs (target - expect) > 1.0e-9) { ++bad; badList << " " << label << "(" << id << ")"; }
            if (std::abs (target - fixedDef) > 1.0e-9)
            {
                ++changed;
                changedList << "\n     " << label << " [" << id << "]  " << juce::String (fixedDef, 3) << " -> " << juce::String (target, 3);
            }
        }
        check (withId > 50 && bad == 0, "Every knob with a parameter id (" + juce::String (withId) + ") double-clicks to that parameter's default; wrong:" + (badList.isEmpty() ? " none" : badList)
                                          + "; no id (keeps its own value): " + juce::String (noId));
        std::printf ("     knobs whose double-click value changed (old -> new, display units): %d%s\n", changed, changedList.toRawUTF8());
        // The real path: turn a knob away, double-click it, it is back on the default.
        panel.turnKnobForTesting ("TONE AMOUNT", 50.0);
        panel.doubleClickKnobForTesting ("TONE AMOUNT");
        check (std::abs (panel.knobValueForTesting ("TONE AMOUNT") - 0.0) < 1.0e-6, "TONE AMOUNT: turned to 50, double-click -> " + fmt (panel.knobValueForTesting ("TONE AMOUNT"), 2) + " (parameter default 0)");
        panel.turnKnobForTesting ("BELL 1 Q", 3.0);
        panel.doubleClickKnobForTesting ("BELL 1 Q");
        check (std::abs (panel.knobValueForTesting ("BELL 1 Q") - mihenk::params::findDef ("bell1Q")->defaultValue()) < 1.0e-4, "BELL 1 Q: double-click -> " + fmt (panel.knobValueForTesting ("BELL 1 Q"), 3));
    }

    tmp.deleteRecursively();
    std::printf ("\n%s\n", failures == 0 ? "ALL PASS" : (juce::String (failures) + " FAILED").toRawUTF8());
    return failures == 0 ? 0 : 1;
}
