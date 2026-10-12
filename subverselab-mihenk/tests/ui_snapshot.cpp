/*  Draws the MIHENK panel into a PNG without a screen or an audio device: synthetic audio is loaded, the chain renders
    for real, and for the "live" variants the processor itself plays it (processBlock), so the meters, the live
    spectrum / wave / loudness and the gain reduction come through the real metering path.

        ui_snapshot <out.png> <variant> [scale]

    variants:
      stopped         app, HIP-HOP / RAP, whole-track spectrum (stopped)
      live-spectrum   app, playing: layered live spectrum
      live-wave       app, playing: scrolling wave with GR band and overview
      live-loudness   app, playing: live loudness
      threeband       app, playing, 3-BAND with per-band settings, MID band selected
      diagnosis       app, a mix with several problems: findings, two scenarios applied (chips)
      dense           app, a pre-limited master: dense warning, glue reduced, DENSE IN
      plugin          plug-in: LEARN through processBlock, then playing (header LEARN, latency)
      edit            app, the GLUE value in its in-place editor
      profile-panel   app, personal profile on: the profile panel (what was learned, a note to write)
      profile-note    app, the profile panel with tags picked, a note typed and an override pending
      profile-lock    app, knobs locked from the notes, the AIR lock's tooltip
      v10-simple      app, SIMPLE face: a hot (-9 LUFS) mix turned down to STREAM -14, macros, chain readout
      v10-advanced    app, ADVANCED face, the same mix, KEEP INPUT, no reference (the module shows its hint)
      v10-auto        app, ADVANCED, AUTO trim / threshold / release / drive lamps lit, solved values read AUTO
      v10-reference   app, ADVANCED, a reference loaded and MATCH TO REFERENCE applied (difference graph, summary)
      v10-batch       app, the BATCH / ALBUM panel after an album batch of five tracks
    The v8 / v9 variants draw the ADVANCED face (the panel they were made for).
*/
#include <JuceHeader.h>
#include "../Source/app/MainComponent.h"
#include "../Source/plugin/PluginProcessor.h"
#include "../Source/dsp/Filters.h"
#include "../Source/dsp/Equalizer.h"
#include "../Source/engine/Diagnosis.h"
#include "../Source/engine/Profile.h"
#include "../Source/engine/AudioIO.h"

using namespace mihenk;

static juce::AudioBuffer<float> synth (double fs, double secs, double tilt, uint32_t seed)
{
    // A kick / bass / noise-pad mix with a quiet verse and a loud chorus.
    const int n = (int) (secs * fs);
    juce::AudioBuffer<float> b (2, n);
    juce::Random rng ((juce::int64) seed);
    double lp[2] { 0.0, 0.0 }, hp[2] { 0.0, 0.0 };
    for (int i = 0; i < n; ++i)
    {
        const double t = i / fs, tb = std::fmod (t, 0.5), section = std::fmod (t, 12.0) < 6.0 ? 0.35 : 1.0;
        const double kick = std::exp (-tb * 16.0) * std::sin (2.0 * kPi * (50.0 * tb + 2.0 * (1.0 - std::exp (-tb * 30.0))));
        const double bass = 0.2 * std::sin (2.0 * kPi * 55.0 * t);
        const double th = std::fmod (t + 0.25, 0.25);
        const double hat = std::exp (-th * 70.0) * (rng.nextDouble() * 2.0 - 1.0) * 0.12;
        for (int c = 0; c < 2; ++c)
        {
            const double w = rng.nextDouble() * 2.0 - 1.0;
            lp[c] += 0.08 * (w - lp[c]);
            hp[c] = w - lp[c];
            const double pad = (1.0 - tilt) * lp[c] * 0.8 + tilt * hp[c] * 0.25;
            b.setSample (c, i, (float) (section * (0.5 * kick + bass + 0.3 * pad + hat * (c == 0 ? 1.2 : 0.8))));
        }
    }
    return b;
}

/** Plays the loaded source through the processor (standalone: the player; plug-in: the host input) for `secs`. */
static void playFor (MihenkProcessor& p, MainComponent& panel, double secs, const juce::AudioBuffer<float>* hostInput = nullptr, double start = 0.0)
{
    const double fs = 48000.0;
    const int block = 512;
    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer midi;
    const int blocks = (int) (secs * fs / block);
    int pos = (int) (start * fs);
    for (int k = 0; k < blocks; ++k)
    {
        buf.clear();
        if (hostInput != nullptr)
            for (int c = 0; c < 2; ++c)
                buf.copyFrom (c, 0, *hostInput, c, pos % (hostInput->getNumSamples() - block), block);
        p.processBlock (buf, midi);
        pos += block;
        if (k % 2 == 0) panel.getLive().update();     // the panel's 30 fps timer drains the FIFO
    }
    panel.getLive().update();
}

struct PlayHead : juce::AudioPlayHead
{
    juce::Optional<PositionInfo> getPosition() const override { PositionInfo i; i.setIsPlaying (true); return i; }
};

int main (int argc, char** argv)
{
    if (argc < 3) { std::printf ("ui_snapshot <out.png> <variant> [scale]\n"); return 1; }
    juce::ScopedJuceInitialiser_GUI init;
    const juce::String variant (argv[2]);
    const float scale = argc > 3 ? (float) std::atof (argv[3]) : 2.0f;
    const double fs = 48000.0;
    const bool plugin = variant == "plugin";

    // Never read or write the user's panel preferences.
    const auto prefs = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("mihenk_snapshot_ui.json");
    prefs.deleteFile();
    ui::UiPrefs::setFileForTesting (prefs);
    const bool v10 = variant.startsWith ("v10");
    MihenkProcessor processor;
    processor.setStandaloneModeForTesting (! plugin);
    Session& session = processor.getSession();
    PlayHead head;
    processor.setPlayHead (&head);
    processor.setRateAndBufferSizeDetails (fs, 512);

    applyGenreStartingPoint (session.settings, variant == "diagnosis" ? Genre::arabeskPop : Genre::rapTrap);
    if (variant == "threeband")
    {
        auto& c = session.settings.comp;
        c.mode = CompMode::threeBand;
        c.xoverLowHz = 140.0; c.xoverHighHz = 4500.0;
        c.bands[0].glueDb = 2.0; c.bands[0].ratio = 3.0; c.bands[0].attackMs = 12.0;
        c.bands[1].glueDb = 1.0; c.bands[1].gainDb = 0.5;
        c.bands[2].glueDb = 0.6; c.bands[2].ratio = 2.5;
    }
    session.settingsChanged();

    auto mix = synth (fs, 30.0, 0.3, 1);
    juce::String name = "demo_mix.wav";
    // Demo hook (env, unset = unchanged behaviour): MIHENK_WAV = 48 kHz stereo file used as the input instead of the synthetic mix.
    bool realWav = false;
    if (const char* w = std::getenv ("MIHENK_WAV"))
    {
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (juce::File (w)));
        if (rd == nullptr) { std::printf ("cannot read %s\n", w); return 2; }
        juce::AudioBuffer<float> b (2, (int) rd->lengthInSamples);
        rd->read (&b, 0, (int) rd->lengthInSamples, 0, true, true);
        if (rd->numChannels == 1) b.copyFrom (1, 0, b, 0, 0, b.getNumSamples());
        mix = b; name = juce::File (w).getFileName(); realWav = true;
    }
    if (v10 && variant != "v10-batch" && ! realWav)
    {
        // A loud production: limited to about -9 LUFS at -0.3 dBTP before it reaches MIHENK.
        MasterSettings h;
        h.targetPreset = TargetPreset::custom; h.targetLufs = -9.0; h.limiter.ceilingDbtp = -0.3; h.limiter.releaseMs = 60.0;
        PreLimiterCache c;
        RenderResult r;
        MasterChain::matchTarget (mix, fs, h, c, &r);
        mix = *r.output;
        name = "loud_production.wav";
    }
    if (variant == "v10-advanced") session.settings.targetPreset = TargetPreset::keepInput;
    if (variant == "v10-auto")
    {
        session.settings.autoTrim = true;
        session.settings.comp.autoRelease = true;
    }
    if (variant == "dense")
    {
        LimiterSettings ls; ls.ceilingDbtp = -1.0; ls.releaseMs = 20.0; ls.lookaheadMs = 0.5; ls.driveDb = 14.0;
        juce::AudioBuffer<float> out;
        limitWithSafety (mix, truePeakEnvelope (mix, kLimiterOversampling), fs, ls, out);
        mix = out;
        name = "loud_master.wav";
    }
    if (variant == "diagnosis")
    {
        // A problem mix: muddy 200-500 Hz, harsh 2-5 kHz, and hot (peaks near 0 dBFS).
        EqSettings e;
        e.bell1Db = 12.0; e.bell1Hz = 300.0; e.bell1Q = 0.8;
        e.bell2Db = 10.0; e.bell2Hz = 3300.0; e.bell2Q = 0.9;
        e.highShelfDb = -8.0; e.highShelfHz = 9000.0;
        convolveInPlace (mix, designEqFir (e, fs));
        // Too wide on top and leaning left, with a DC offset.
        for (int i = 0; i < mix.getNumSamples(); ++i)
        {
            const float l = mix.getSample (0, i), r = mix.getSample (1, i);
            const float m = 0.5f * (l + r), sd = 0.5f * (l - r) * 2.2f;
            mix.setSample (0, i, (m + sd) * 1.25f + 0.004f);
            mix.setSample (1, i, (m - sd) + 0.004f);
        }
        mix.applyGain ((float) (dbToGain (-0.3) / mix.getMagnitude (0, mix.getNumSamples())));
        name = "problem_mix.wav";
    }

    const bool profileVariant = variant.startsWith ("profile");
    if (profileVariant)
    {
        // A profile with history: notes on tracks like this one, and on a different kind of track.
        namespace pf = mihenk::profile;
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("mihenk_snapshot_profile");
        dir.deleteRecursively();
        session.profile.setFolder (dir);
        const auto m = measureMix (mix, fs);
        juce::Random rng (12);
        auto near = [&] (double a) { auto x = m; x.plr += a * rng.nextDouble(); x.airRelDb += a * rng.nextDouble(); return x; };
        auto far = m;
        far.plr -= 8.0; far.lra -= 6.0; far.sideMidDb += 10.0; far.airRelDb -= 9.0;
        juce::Time t (2026, 8, 20, 21, 10);
        juce::String err;
        auto add = [&] (const DiagnosisMeasures& mm, std::function<void (MasterSettings&)> set, juce::StringArray tags, const juce::String& text, std::vector<pf::Override> ov = {})
        {
            MasterSettings x = session.settings;
            set (x);
            pf::Context c;
            c.measures = &mm; c.settings = &x; c.source = "app"; c.track = "session_" + juce::String (rng.nextInt (90) + 10) + ".wav";
            c.startingPoint = genreName (Genre::rapTrap); c.overrides = std::move (ov);
            session.profile.add (pf::makeNote (tags, text, c, t), err);
            t = t + juce::RelativeTime::hours (19);
        };
        using pf::tagId;
        add (near (0.6), [] (MasterSettings& x) { x.eq.airDb = 1.0; }, { tagId (pf::tNoAir), tagId (pf::tTooWide) }, "top is closed");
        add (near (0.6), [] (MasterSettings& x) { x.eq.airDb = 1.0; }, { tagId (pf::tNoAir), tagId (pf::tTooWide), tagId (pf::tVocalBuried) }, "");
        add (far, [] (MasterSettings& x) { x.comp.glueDb = 1.2; x.eq.airDb = 0.0; }, { tagId (pf::tSquashed) }, "club mix, kick flattened");
        add (near (0.6), [] (MasterSettings& x) { x.eq.airDb = 1.5; x.width.width = 0.95; }, { tagId (pf::tNoAir) }, "");
        add (far, [] (MasterSettings& x) { x.comp.glueDb = 0.9; x.eq.airDb = 0.5; }, { tagId (pf::tSquashed), tagId (pf::tNoAir) }, "");
        add (near (0.6), [] (MasterSettings& x) { x.eq.airDb = 1.5; x.width.width = 0.95; }, { tagId (pf::tPerfect) }, "this is the sound", { { pf::pAir, 1.0, 1.5 } });
        add (far, [] (MasterSettings& x) { x.comp.glueDb = 0.6; x.eq.airDb = 1.0; }, { tagId (pf::tHarsh) }, "");
        add (near (0.6), [] (MasterSettings& x) { x.eq.airDb = 1.5; x.width.width = 0.95; }, { tagId (pf::tVocalBuried) }, "vocal sits behind the snare");
        add (far, [] (MasterSettings& x) { x.comp.glueDb = 0.6; x.eq.airDb = 0.0; }, { tagId (pf::tPerfect) }, "");
        session.setProfileEnabled (true);
    }

    if (plugin)
    {
        processor.setNonRealtime (true);
        processor.prepareToPlay (fs, 512);
        processor.toggleLearn();
        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer midi;
        for (int pos = 0; pos + 512 <= mix.getNumSamples(); pos += 512)
        {
            for (int c = 0; c < 2; ++c) buf.copyFrom (c, 0, mix, c, pos, 512);
            processor.processBlock (buf, midi);
        }
        processor.finishLearnNow();
        session.status = "Learned 0:30.0 at 48 kHz; the plug-in follows the host in real time.";
        processor.prepareToPlay (fs, 512);
    }
    else
    {
        session.setInputForTesting (juce::AudioBuffer<float> (mix), fs, name);
        if (! v10 || variant == "v10-reference")
        {
            auto ref = synth (fs, 30.0, 0.6, 2);
            if (v10) ref.applyGain ((float) (dbToGain (-1.0) / ref.getMagnitude (0, ref.getNumSamples())));
            session.setReferenceForTesting (std::move (ref), fs, v10 ? "reference_master.wav" : "reference.wav");
        }
        if (! v10)
        {
            session.settings.eq.matchCurveDb = MasterChain::toneMatchCurve (session.inMeas.spectrumDb, session.refMeas.spectrumDb, session.settings.eq, 1.0);
            session.settings.eq.matchAmount = 0.6;
        }
        if (variant == "v10-reference") { session.matchToReference(); }
        session.renderNowForTesting();
        session.status = "Loaded " + name + "; diagnosis: " + juce::String ((int) session.diagnosis.findings.size()) + " finding(s).";
        if (session.diagnosis.findings.empty()) session.status = "Target -14.0 LUFS: achieved -14.00 LUFS, true peak -1.00 dBTP (TARGET LOCK)";
        if (variant == "v10-reference") session.status = "Matched to reference_master.wav: tone and loudness " + fmt (session.settings.targetLufs) + " LUFS.";
        processor.prepareToPlay (fs, 512);
    }

    MainComponent panel (processor);
    panel.setFace (variant == "v10-simple" || variant == "v10-batch" ? false : true, false);
    panel.setPresetsFolder (juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("mihenk_snapshot_presets"));
    panel.setBounds (0, 0, MainComponent::panelWidth, MainComponent::panelHeight);
    panel.presetLoadedForSnapshot (variant == "diagnosis" ? genreName (Genre::arabeskPop) : genreName (Genre::rapTrap), true);

    if (variant == "diagnosis")
    {
        const auto d = session.currentDiagnosis();
        std::printf ("findings: %d\n", (int) d.findings.size());
        for (auto& f : d.findings) std::printf ("  %s: %s\n", f.title.toRawUTF8(), f.reason.toRawUTF8());
        int applied = 0;
        for (auto& f : d.findings)
            if (applied < 3 && (f.scenario == scMuddy || f.scenario == scTooHot || f.scenario == scHarsh || f.scenario == scDcOffset)) { session.applyScenario (f.scenario); ++applied; }
        session.renderNowForTesting();
    }
    ui::CrtScope::Mode mode = ui::CrtScope::Mode::spectrum;
    if (variant == "live-wave" || variant == "threeband") mode = ui::CrtScope::Mode::wave;
    if (variant == "live-loudness") mode = ui::CrtScope::Mode::loudness;
    panel.setScopeMode (mode);
    if (variant == "threeband") panel.selectBand (1);

    const bool playing = variant.startsWith ("live") || variant == "threeband" || plugin || variant == "diagnosis" || variant == "dense";
    if (playing)
    {
        if (! plugin)
        {
            // Stop in the chorus (18-24 s), where the compressor and the limiter work.
            session.player.setPosition ((juce::int64) (fs * (mode == ui::CrtScope::Mode::wave ? 15.0 : mode == ui::CrtScope::Mode::loudness ? 9.0 : 13.0)));
            session.player.play();
        }
        playFor (processor, panel, variant == "live-loudness" ? 14.0 : 8.0, plugin ? &mix : nullptr, 13.0);
        panel.setForceLive (true);
    }
    if (! plugin && variant == "stopped")
    {
        session.player.setPosition ((juce::int64) (fs * 17.3));
        session.player.setLoopRegion ((juce::int64) (fs * 12.0), (juce::int64) (fs * 24.0));
        session.player.setLoop (true);
    }
    panel.settleForSnapshot();
    if (variant == "edit") panel.openValueEditorForSnapshot ("GLUE", "1.5 db");
    if (variant == "profile-panel") panel.openProfileForSnapshot ({}, {});
    if (variant == "profile-note")
    {
        session.unlock (mihenk::profile::pWidth);
        session.settings.width.width = 1.0;
        session.settingsChanged();
        session.renderNowForTesting();
        panel.openProfileForSnapshot ({ mihenk::profile::tagId (mihenk::profile::tNoAir), mihenk::profile::tagId (mihenk::profile::tVocalBuried) },
                                      "Hats are fine now, the top still closes in the hook. Vocal a touch behind.");
    }
    if (variant == "profile-lock") panel.showTooltipForSnapshot ("AIR");
    if (variant == "v10-batch")
    {
        // Five mixes of an EP, quiet to loud, through the real batch panel (album mode) into a temp folder.
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("mihenk_snapshot_batch");
        dir.deleteRecursively();
        dir.createDirectory();
        juce::Array<juce::File> files;
        const char* names[] { "01 intro.wav", "02 bir araya.wav", "03 gece yarisi.wav", "04 son tur.wav", "05 outro.wav" };
        const double levels[] { -21.0, -11.5, -13.0, -9.5, -18.0 }, tilts[] { 0.2, 0.35, 0.3, 0.45, 0.15 };
        for (int i = 0; i < 5; ++i)
        {
            auto b = synth (fs, 14.0, tilts[i], 20 + (uint32_t) i);
            b.applyGain ((float) dbToGain (levels[i] - integratedLoudness (b, fs)));
            if (b.getMagnitude (0, b.getNumSamples()) > 0.95f) { LimiterSettings ls; ls.ceilingDbtp = -0.3; juce::AudioBuffer<float> o; limitWithSafety (b, truePeakEnvelope (b, kLimiterOversampling), fs, ls, o); b = o; }
            juce::String err;
            exportWav (dir.getChildFile (names[i]), b, fs, 24, err);
            files.add (dir.getChildFile (names[i]));
        }
        panel.showBatchPanel (true);
        auto& bp = panel.getBatchPanel();
        bp.setOutputFolder (dir.getChildFile ("MIHENK masters"));
        bp.setModes (false, true);
        bp.addFiles (files);
        bp.waitForTesting (60000);
        bp.start();
        bp.waitForTesting (300000);
        bp.selectRow (1);
        session.status = bp.lastResult().message;
    }
    const auto image = panel.createComponentSnapshot (panel.getLocalBounds(), true, scale);
    juce::File out { juce::File::getCurrentWorkingDirectory().getChildFile (juce::String (argv[1])) };
    out.deleteFile();
    juce::FileOutputStream stream (out);
    juce::PNGImageFormat().writeImageToStream (image, stream);
    std::printf ("%s  (%s)\n", out.getFullPathName().toRawUTF8(), session.status.toRawUTF8());
    processor.setPlayHead (nullptr);
    return 0;
}
