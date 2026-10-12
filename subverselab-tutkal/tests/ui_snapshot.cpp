/*  Draws the editor into PNGs without a screen, after real programme has gone through the real processor,
    so the needle, the 7-segment digits and the lamps show what the engine measured (nothing is faked).

        ui_snapshot [outdir] [session_dir]     (default docs/ui, ../prototypes/DINLE/10_TUTKAL_v3/_session)

      front_glue.png : GLUE on DRY_drumbus.wav, threshold -18, 4:1, attack 10, AUTO
      front_core.png : CORE on DRY_drumbus.wav (the GLUE knobs), threshold -18, 4:1, attack 10, release 0.25 s, AUTO off
      front_la.png   : LA on DRY_vocal_in_backing.wav, PEAK REDUCTION 0.5
      front_pump.png : PUMP on DRY_padbass.wav, KEY_kick.wav on the sidechain bus, EXT on, 1/8 EXP depth 10, host 100 bpm
      rear.png       : the rear plate (state of the PUMP run)

    For each programme the audio runs in 512-sample blocks and the editor's 30 Hz tick is called every 1600 samples (the
    needle integrates the real gain reduction over time). The first pass finds the moment of the deepest needle position;
    the second pass stops there, so the picture is a measured moment of the programme, not the end of the file.
    Also checks the editor's keyboard undo of a voicing key and the saved editor width. Exit code 0 only if all of it held.
*/
#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"
#include <cstdio>
#include <map>

namespace
{
using Audio = std::vector<std::vector<float>>;
int gFail = 0;

void check (bool ok, const juce::String& what)
{
    std::printf ("[%s] %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8());
    if (! ok) ++gFail;
}

Audio readWav (const juce::File& f)
{
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
    if (! r) { std::printf ("cannot read %s\n", f.getFullPathName().toRawUTF8()); std::exit (2); }
    juce::AudioBuffer<float> b ((int) r->numChannels, (int) r->lengthInSamples);
    r->read (&b, 0, (int) r->lengthInSamples, 0, true, true);
    Audio a ((size_t) b.getNumChannels());
    for (int c = 0; c < b.getNumChannels(); ++c) a[(size_t) c].assign (b.getReadPointer (c), b.getReadPointer (c) + b.getNumSamples());
    return a;
}

struct TestPlayHead : juce::AudioPlayHead
{
    double bpm = 100.0;
    juce::Optional<PositionInfo> getPosition() const override { PositionInfo i; i.setBpm (bpm); return i; }
};

void setParam (juce::AudioProcessorValueTreeState& s, const char* id, float v)
{
    auto* p = s.getParameter (id);
    p->setValueNotifyingHost (p->convertTo0to1 (v));
}

using Settings = std::vector<std::pair<const char*, float>>;

struct Scenario
{
    const char* name;
    Audio in;
    const Audio* key;          // on the sidechain bus, or nullptr
    Settings settings;
    double bpm;                // <= 0: no play-head
};

constexpr int kBlock = 512, kTickEvery = 1600;

/** Runs the programme through a fresh processor and editor up to `stopAt` samples, then hands both to `done`. */
void run (const Scenario& sc, int stopAt, const std::function<void (TutkalAudioProcessor&, TutkalEditor&)>& done)
{
    TutkalAudioProcessor proc;
    const int mainCh = (int) sc.in.size();
    auto layout = proc.getBusesLayout();
    layout.inputBuses.set (1, sc.key != nullptr ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::disabled());
    if (! proc.setBusesLayout (layout)) { std::printf ("setBusesLayout refused\n"); std::exit (2); }
    proc.setRateAndBufferSizeDetails (48000.0, kBlock);
    proc.prepareToPlay (48000.0, kBlock);
    for (auto& kv : sc.settings) setParam (proc.apvts, kv.first, kv.second);
    TestPlayHead ph; ph.bpm = sc.bpm;
    if (sc.bpm > 0.0) proc.setPlayHead (&ph);

    std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
    auto& editor = *static_cast<TutkalEditor*> (ed.get());
    editor.setSize (TutkalEditor::kW, TutkalEditor::kH);

    const int total = juce::jmax (proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
    const int n = (int) sc.in[0].size();
    const int end = juce::jmin (n, stopAt);
    juce::AudioBuffer<float> buf (total, kBlock);
    juce::MidiBuffer midi;
    int sinceTick = 0;
    for (int pos = 0; pos < end; pos += kBlock)
    {
        const int len = juce::jmin (kBlock, end - pos);
        buf.setSize (total, len, false, false, true);
        buf.clear();
        for (int c = 0; c < mainCh; ++c) std::copy (sc.in[(size_t) c].begin() + pos, sc.in[(size_t) c].begin() + pos + len, buf.getWritePointer (c));
        if (sc.key != nullptr)
            for (int c = 0; c < 2 && mainCh + c < total; ++c)
                std::copy ((*sc.key)[(size_t) c].begin() + pos, (*sc.key)[(size_t) c].begin() + pos + len, buf.getWritePointer (mainCh + c));
        proc.processBlock (buf, midi);
        sinceTick += len;
        if (sinceTick >= kTickEvery)
        {
            sinceTick -= kTickEvery;
            editor.frontPanel().displayWindow().setSnapshotClock (1000.0 * (double) (pos + len) / 48000.0);   // deterministic leaf clock
            editor.tickForSnapshot();
        }
    }
    done (proc, editor);
}
} // namespace

// --ornaments: renders the three dial ornaments (LA voicing with programme, as front_la) to ornament_N_name.png and a 2x
// zoom of the meter box (vector re-render at scale 2, not a bitmap upscale) to ornament_N_name_zoom.png.
// ORNAMENT_ZOOM=4 in the environment renders the zoom crops at 4x for close inspection (default 2x).
static float kOrnamentZoom = 2.0f;
static bool renderOrnaments (const juce::File& dir, const juce::File& session, bool peaks = false, bool leaves = false)
{
    const Audio vocal = readWav (session.getChildFile ("DRY_vocal_in_backing.wav"));
    const Scenario sc { "front_la", vocal, nullptr, { { "voicing", 1 }, { "peakReduction", 0.5f } }, -1.0 };
    const char* names[3] { "ornament_1_tespih", "ornament_2_rumi", "ornament_3_kandil" };
    bool ok = true;
    int stopAt = 0;
    {
        // the deepest-needle moment, found with the plain dial (same ticks)
        TutkalAudioProcessor proc;
        auto layout = proc.getBusesLayout();
        layout.inputBuses.set (1, juce::AudioChannelSet::disabled());
        proc.setBusesLayout (layout);
        proc.setRateAndBufferSizeDetails (48000.0, kBlock);
        proc.prepareToPlay (48000.0, kBlock);
        for (auto& kv : sc.settings) setParam (proc.apvts, kv.first, kv.second);
        std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
        auto& editor = *static_cast<TutkalEditor*> (ed.get());
        editor.setSize (TutkalEditor::kW, TutkalEditor::kH);
        const int total = juce::jmax (proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
        const int n = (int) sc.in[0].size(), mainCh = (int) sc.in.size();
        juce::AudioBuffer<float> buf (total, kBlock);
        juce::MidiBuffer midi;
        int sinceTick = 0;
        float bestNeedle = -1.0f;
        for (int pos = 0; pos < n; pos += kBlock)
        {
            const int len = juce::jmin (kBlock, n - pos);
            buf.setSize (total, len, false, false, true);
            buf.clear();
            for (int c = 0; c < mainCh; ++c) std::copy (sc.in[(size_t) c].begin() + pos, sc.in[(size_t) c].begin() + pos + len, buf.getWritePointer (c));
            proc.processBlock (buf, midi);
            sinceTick += len;
            if (sinceTick >= kTickEvery)
            {
                sinceTick -= kTickEvery;
                editor.tickForSnapshot();
                const float nd = editor.frontPanel().needleDb();
                if (pos > 96000 && nd > bestNeedle) { bestNeedle = nd; stopAt = pos + len; }
            }
        }
    }
    const char* peakNames[3] { "peak_1_damla", "peak_2_lale", "peak_3_gul" };
    for (int v = 1; v <= 3; ++v)
    {
        if (leaves) TutkalUi::peakLeaf = v; else if (peaks) TutkalUi::peakMarker = v; else TutkalUi::dialOrnament = v;
        run (sc, stopAt, [&] (TutkalAudioProcessor& proc, TutkalEditor& editor)
        {
            auto save = [&] (const juce::Image& image, const juce::String& file)
            {
                const auto f = dir.getChildFile (file);
                f.deleteFile();
                juce::FileOutputStream stream (f);
                const bool w = stream.openedOk() && juce::PNGImageFormat().writeImageToStream (image, stream);
                std::printf ("  %s %s %dx%d\n", w ? "wrote" : "FAILED", f.getFullPathName().toRawUTF8(), image.getWidth(), image.getHeight());
                ok &= w;
            };
            if (leaves)
            {
                // --leaves: held-peak frames (2x meter box, 4x close-up) and the release animation, 24 frames driven by the
                // editor's own clock (set here, so the result does not depend on the machine's speed)
                const char* ln[3] { "leaf_1_yaprak", "leaf_2_sogut", "leaf_3_lale" };
                auto& dw = editor.frontPanel().displayWindow();
                const juce::Rectangle<int> meter (484, 58, 434, 226);
                std::printf ("leaf %d, peak %.2f dB, needle %.2f dB\n", v, (double) editor.frontPanel().peakDb(), (double) editor.frontPanel().needleDb());
                save (editor.createComponentSnapshot (meter, true, 2.0f), juce::String (ln[v - 1]) + "_zoom.png");
                const auto c = dw.lastLeafCentre;
                save (editor.createComponentSnapshot (juce::Rectangle<int> ((int) std::lround (c.x) - 20, (int) std::lround (c.y) - 22, 40, 44), true, 4.0f), juce::String (ln[v - 1]) + "_close.png");
                const char* fe = std::getenv ("LEAF_FRAMES_DIR");
                const juce::File fdir = fe != nullptr ? juce::File (fe) : dir.getChildFile ("leaf_frames");
                fdir.createDirectory();
                double clock = dw.leafClockMs();
                const int total = juce::jmax (proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
                juce::AudioBuffer<float> quiet (total, kBlock);
                juce::MidiBuffer midi;
                auto stepTo = [&] (double to)           // silence through the real processor, ticked every 33 ms
                {
                    while (clock < to - 1e-6)
                    {
                        const double dt = juce::jmin (1000.0 / 30.0, to - clock);
                        for (int left = (int) std::lround (dt * 48.0); left > 0; left -= kBlock)
                        {
                            quiet.setSize (total, juce::jmin (kBlock, left), false, false, true);
                            quiet.clear();
                            proc.processBlock (quiet, midi);
                        }
                        clock += dt;
                        dw.setSnapshotClock (clock);
                        editor.tickForSnapshot();
                    }
                };
                int guard = 0;
                while (! dw.leafFalling() && guard++ < 400) stepTo (clock + 1000.0 / 30.0);   // wait for the hold to end
                const double fs = dw.leafFallStartMs();
                const double span = 1000.0 * (double) dw.leafFallSeconds() + (v == 3 ? 300.0 : 0.0);
                std::printf ("  fall starts at %.0f ms, span %.0f ms, needle %.2f dB\n", fs, span, (double) editor.frontPanel().needleDb());
                for (int i = 0; i < 24; ++i)
                {
                    stepTo (fs + span * (double) i / 23.0 + (i == 23 ? 1.0 : 0.0));
                    char nm[64];
                    std::snprintf (nm, sizeof nm, "%s_%02d.png", ln[v - 1], i);
                    const auto f = fdir.getChildFile (nm);
                    f.deleteFile();
                    juce::FileOutputStream stream (f);
                    ok &= stream.openedOk() && juce::PNGImageFormat().writeImageToStream (editor.createComponentSnapshot (meter, true, 2.0f), stream);
                }
                std::printf ("  frames: %s  step %.1f ms\n", fdir.getFullPathName().toRawUTF8(), span / 23.0);
                return;
            }
            if (peaks)
            {
                // marker area: tip at R-2 on the peak angle; meter box (498,73,402,196), pivot 6% above its bottom, R = 0.76 h
                const float pk = juce::jmin (editor.frontPanel().peakDb(), 20.0f);
                const float a = (-52.0f + 104.0f * pk / 20.0f) * 3.14159265f / 180.0f;
                const float cx = 498.0f + 201.0f, py = 73.0f + 196.0f - 196.0f * 0.06f, R = 196.0f * 0.76f;
                const int mx = (int) std::lround (cx + std::sin (a) * (R - 9.0f)), my = (int) std::lround (py - std::cos (a) * (R - 9.0f));
                std::printf ("peak marker %d, peak %.2f dB, needle %.2f dB\n", v, (double) pk, (double) editor.frontPanel().needleDb());
                save (editor.createComponentSnapshot (juce::Rectangle<int> (484, 58, 434, 226), true, 2.0f), juce::String (peakNames[v - 1]) + "_zoom.png");
                save (editor.createComponentSnapshot (juce::Rectangle<int> (mx - 28, my - 20, 56, 40), true, 4.0f), juce::String (peakNames[v - 1]) + "_close.png");
                if (v == 1) save (editor.createComponentSnapshot (editor.getLocalBounds(), true, 1.0f), "peak_1_damla_1x.png");
                if (v == 2) save (editor.createComponentSnapshot (editor.getLocalBounds(), true, 1.0f), "peak_2_lale_1x.png");
                if (v == 3) save (editor.createComponentSnapshot (editor.getLocalBounds(), true, 1.0f), "peak_3_gul_1x.png");
                return;
            }
            std::printf ("ornament %d, needle %.2f dB\n", v, (double) editor.frontPanel().needleDb());
            save (editor.createComponentSnapshot (editor.getLocalBounds(), true, 1.0f), juce::String (names[v - 1]) + ".png");
            save (editor.createComponentSnapshot (juce::Rectangle<int> (484, 58, 434, 226), true, kOrnamentZoom), juce::String (names[v - 1]) + "_zoom.png");
        });
    }
    TutkalUi::dialOrnament = TutkalUi::kDialOrnament;
    TutkalUi::peakMarker = TutkalUi::kPeakMarker;
    return ok;
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    bool ornamentsOnly = false, peaksOnly = false, leavesOnly = false;
    if (const char* z = std::getenv ("ORNAMENT_ZOOM")) kOrnamentZoom = juce::jlimit (1.0f, 6.0f, (float) std::atof (z));
    for (int i = 1; i < argc;)
    {
        if (juce::String (argv[i]) == "--leaves")
        {
            leavesOnly = true;
            for (int j = i; j + 1 < argc; ++j) argv[j] = argv[j + 1];
            --argc;
        }
        else if (juce::String (argv[i]) == "--peaks")
        {
            peaksOnly = true;
            for (int j = i; j + 1 < argc; ++j) argv[j] = argv[j + 1];
            --argc;
        }
        else if (juce::String (argv[i]) == "--ornaments")
        {
            ornamentsOnly = true;
            for (int j = i; j + 1 < argc; ++j) argv[j] = argv[j + 1];
            --argc;
        }
        else ++i;
    }
    const juce::File cwd = juce::File::getCurrentWorkingDirectory();
    const juce::File dir = cwd.getChildFile (argc > 1 ? argv[1] : "docs/ui");
    const juce::File session = cwd.getChildFile (argc > 2 ? argv[2] : "../prototypes/DINLE/10_TUTKAL_v3/_session");
    dir.createDirectory();
    if (! session.isDirectory()) { std::printf ("session dir not found: %s\n", session.getFullPathName().toRawUTF8()); return 2; }
    if (leavesOnly) return renderOrnaments (dir, session, true, true) ? 0 : 1;
    if (peaksOnly) return renderOrnaments (dir, session, true) ? 0 : 1;
    if (ornamentsOnly) return renderOrnaments (dir, session) ? 0 : 1;

    const Audio drum = readWav (session.getChildFile ("DRY_drumbus.wav"));
    const Audio vocal = readWav (session.getChildFile ("DRY_vocal_in_backing.wav"));
    const Audio pad = readWav (session.getChildFile ("DRY_padbass.wav"));
    const Audio kick = readWav (session.getChildFile ("KEY_kick.wav"));

    const Scenario scenarios[] = {
        { "front_glue", drum, nullptr,
          { { "voicing", 0 }, { "threshold", -18.0f }, { "ratio", 4.0f }, { "attack", 10.0f }, { "autoRelease", 1 } }, -1.0 },
        { "front_core", drum, nullptr,
          { { "voicing", 3 }, { "threshold", -18.0f }, { "ratio", 4.0f }, { "attack", 10.0f }, { "release", 250.0f }, { "autoRelease", 0 } }, -1.0 },
        { "front_la", vocal, nullptr, { { "voicing", 1 }, { "peakReduction", 0.5f } }, -1.0 },
        { "front_pump", pad, &kick,
          { { "voicing", 2 }, { "ratio", 10.0f }, { "attack", 0.5f }, { "threshold", -30.0f }, { "ext", 1 },
            { "pumpNote", 1 }, { "pumpShape", 0 }, { "depth", 10.0f } }, 100.0 } };

    // The moment of the deepest needle: a first pass that only measures (same editor code, same 30 Hz ticks).
    auto deepestMoment = [] (const Scenario& sc)
    {
        TutkalAudioProcessor proc;
        const int mainCh = (int) sc.in.size();
        auto layout = proc.getBusesLayout();
        layout.inputBuses.set (1, sc.key != nullptr ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::disabled());
        proc.setBusesLayout (layout);
        proc.setRateAndBufferSizeDetails (48000.0, kBlock);
        proc.prepareToPlay (48000.0, kBlock);
        for (auto& kv : sc.settings) setParam (proc.apvts, kv.first, kv.second);
        TestPlayHead ph; ph.bpm = sc.bpm;
        if (sc.bpm > 0.0) proc.setPlayHead (&ph);
        std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
        auto& editor = *static_cast<TutkalEditor*> (ed.get());
        editor.setSize (TutkalEditor::kW, TutkalEditor::kH);
        const int total = juce::jmax (proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
        const int n = (int) sc.in[0].size();
        juce::AudioBuffer<float> buf (total, kBlock);
        juce::MidiBuffer midi;
        int sinceTick = 0, best = 0;
        float bestNeedle = -1.0f;
        for (int pos = 0; pos < n; pos += kBlock)
        {
            const int len = juce::jmin (kBlock, n - pos);
            buf.setSize (total, len, false, false, true);
            buf.clear();
            for (int c = 0; c < mainCh; ++c) std::copy (sc.in[(size_t) c].begin() + pos, sc.in[(size_t) c].begin() + pos + len, buf.getWritePointer (c));
            if (sc.key != nullptr)
                for (int c = 0; c < 2 && mainCh + c < total; ++c)
                    std::copy ((*sc.key)[(size_t) c].begin() + pos, (*sc.key)[(size_t) c].begin() + pos + len, buf.getWritePointer (mainCh + c));
            proc.processBlock (buf, midi);
            sinceTick += len;
            if (sinceTick >= kTickEvery)
            {
                sinceTick -= kTickEvery;
                editor.tickForSnapshot();
                const float nd = editor.frontPanel().needleDb();
                if (pos > 96000 && nd > bestNeedle) { bestNeedle = nd; best = pos + len; }
            }
        }
        std::printf ("  deepest needle %.2f dB at %.2f s\n", (double) bestNeedle, best / 48000.0);
        return best;
    };

    // Demo hook (env, unset = unchanged behaviour): TUTKAL_ONLY=<name> runs ONE scenario and writes <name>.png only.
    //   TUTKAL_IN = wav inside the session dir, TUTKAL_KEY = wav on the sidechain bus (optional), TUTKAL_BPM host tempo,
    //   TUTKAL_SETTINGS = "voicing=0,threshold=-18,ratio=4,..." (parameter ids, plain values), TUTKAL_AT=<seconds>: stop there
    //   instead of at the deepest needle moment.
    if (const char* onlyName = std::getenv ("TUTKAL_ONLY"))
    {
        static std::vector<std::string> keep;
        static Settings st;
        keep.clear(); st.clear();
        std::vector<std::pair<std::string, float>> parsed;
        for (auto& tok : juce::StringArray::fromTokens (juce::String (std::getenv ("TUTKAL_SETTINGS") != nullptr ? std::getenv ("TUTKAL_SETTINGS") : ""), ",", ""))
            if (tok.contains ("=")) parsed.push_back ({ tok.upToFirstOccurrenceOf ("=", false, false).toStdString(), tok.fromFirstOccurrenceOf ("=", false, false).getFloatValue() });
        keep.reserve (parsed.size());
        for (auto& kv : parsed) { keep.push_back (kv.first); st.push_back ({ keep.back().c_str(), kv.second }); }
        static Audio keyAudio;
        const char* kf = std::getenv ("TUTKAL_KEY");
        if (kf != nullptr) keyAudio = readWav (session.getChildFile (kf));
        const double bpm = std::getenv ("TUTKAL_BPM") != nullptr ? std::atof (std::getenv ("TUTKAL_BPM")) : -1.0;
        const Scenario sc { onlyName, readWav (session.getChildFile (std::getenv ("TUTKAL_IN") != nullptr ? std::getenv ("TUTKAL_IN") : "DRY_drumbus.wav")),
                            kf != nullptr ? &keyAudio : nullptr, st, bpm };
        const int stopAt = std::getenv ("TUTKAL_AT") != nullptr ? (int) (48000.0 * std::atof (std::getenv ("TUTKAL_AT"))) : deepestMoment (sc);
        bool good = false;
        run (sc, stopAt, [&] (TutkalAudioProcessor& proc, TutkalEditor& editor)
        {
            std::printf ("  state: meter GR %.2f dB, needle %.2f dB, peak hold %.2f dB, host bpm %.1f, voicing %d\n", (double) proc.getGainReductionDb(),
                         (double) editor.frontPanel().needleDb(), (double) editor.frontPanel().peakDb(), (double) proc.getHostBpm(),
                         (int) std::lround (proc.apvts.getRawParameterValue ("voicing")->load()));
            const auto image = editor.createComponentSnapshot (editor.getLocalBounds(), true, 1.0f);
            const auto f = dir.getChildFile (juce::String (sc.name) + ".png");
            f.deleteFile();
            juce::FileOutputStream stream (f);
            good = stream.openedOk() && juce::PNGImageFormat().writeImageToStream (image, stream);
            std::printf ("  %s %s %dx%d\n", good ? "wrote" : "FAILED", f.getFullPathName().toRawUTF8(), image.getWidth(), image.getHeight());
        });
        return good ? 0 : 1;
    }

    bool ok = true;
    juce::Image lastRearImage;
    for (const auto& sc : scenarios)
    {
        std::printf ("%s\n", sc.name);
        const int stopAt = deepestMoment (sc);
        run (sc, stopAt, [&] (TutkalAudioProcessor& proc, TutkalEditor& editor)
        {
            std::printf ("  state: meter GR %.2f dB, needle %.2f dB, host bpm %.1f, sidechain %d, voicing %d\n",
                         (double) proc.getGainReductionDb(), (double) editor.frontPanel().needleDb(), (double) proc.getHostBpm(),
                         (int) proc.isSidechainConnected(), (int) std::lround (proc.apvts.getRawParameterValue ("voicing")->load()));
            const auto image = editor.createComponentSnapshot (editor.getLocalBounds(), true, 1.0f);
            const auto f = dir.getChildFile (juce::String (sc.name) + ".png");
            f.deleteFile();
            juce::FileOutputStream stream (f);
            const bool w = stream.openedOk() && juce::PNGImageFormat().writeImageToStream (image, stream);
            std::printf ("  %s %s %dx%d\n", w ? "wrote" : "FAILED", f.getFullPathName().toRawUTF8(), image.getWidth(), image.getHeight());
            ok &= w;
            if (juce::String (sc.name) == "front_pump")
            {
                editor.showRear (true);
                const auto rimage = editor.createComponentSnapshot (editor.getLocalBounds(), true, 1.0f);
                const auto rf = dir.getChildFile ("rear.png");
                rf.deleteFile();
                juce::FileOutputStream rs (rf);
                const bool rw = rs.openedOk() && juce::PNGImageFormat().writeImageToStream (rimage, rs);
                std::printf ("  %s %s %dx%d\n", rw ? "wrote" : "FAILED", rf.getFullPathName().toRawUTF8(), rimage.getWidth(), rimage.getHeight());
                ok &= rw;
                editor.showRear (false);
            }
        });
    }

    // A voicing key is one undo step, and Cmd+Z / Cmd+Shift+Z reach it through the editor.
    {
        TutkalAudioProcessor proc;
        proc.prepareToPlay (48000.0, kBlock);
        std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
        auto& editor = *static_cast<TutkalEditor*> (ed.get());
        auto voicing = [&] { return (int) std::lround (proc.apvts.getRawParameterValue ("voicing")->load()); };
        const int steps0 = proc.undo.undoSteps();
        check (voicing() == 0, "starts on GLUE");
        editor.frontPanel().voicingKey (2).onClick();
        check (voicing() == 2 && proc.undo.undoSteps() == steps0 + 1, "PUMP key: voicing is PUMP, exactly one undo step");
        editor.frontPanel().voicingKey (1).onClick();
        check (voicing() == 1 && proc.undo.undoSteps() == steps0 + 2, "LA key: one more undo step");
        const auto cmdZ = juce::KeyPress ('z', juce::ModifierKeys::commandModifier, 0);
        const auto cmdShiftZ = juce::KeyPress ('z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0);
        check (editor.keyPressed (cmdZ) && voicing() == 2, "Cmd+Z in the editor: back to PUMP");
        check (editor.keyPressed (cmdZ) && voicing() == 0, "Cmd+Z again: back to GLUE");
        check (editor.keyPressed (cmdShiftZ) && voicing() == 2, "Cmd+Shift+Z: PUMP again");
        editor.frontPanel().voicingKey (2).onClick();
        check (proc.undo.undoSteps() == steps0 + 1, "clicking the lit key again adds no step");
    }
    // The editor's width is kept in the state, between 800 and 2400.
    {
        TutkalAudioProcessor proc;
        {
            std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
            check (ed->getWidth() == 1280 && ed->getHeight() == 512, "opens at 1280 x 512 (5:2): " + juce::String (ed->getWidth()) + " x " + juce::String (ed->getHeight()));
            ed->setSize (1000, 400);
            check (proc.editorWidth.load() == 1000, "resize writes the width");
        }
        juce::MemoryBlock mb;
        proc.getStateInformation (mb);
        TutkalAudioProcessor other;
        other.setStateInformation (mb.getData(), (int) mb.getSize());
        check (other.editorWidth.load() == 1000, "state restores the editor width (1000)");
        std::unique_ptr<juce::AudioProcessorEditor> ed2 (other.createEditor());
        check (ed2->getWidth() == 1000 && ed2->getHeight() == 400, "a new editor opens at the saved size: " + juce::String (ed2->getWidth()) + " x " + juce::String (ed2->getHeight()));
    }
    // Value windows and type-in (the owner could not type values, and could not read them).
    {
        struct Counter : juce::AudioProcessorListener
        {
            std::map<int, int> changes;
            void audioProcessorParameterChanged (juce::AudioProcessor*, int i, float) override { ++changes[i]; }
            void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}
        };
        TutkalAudioProcessor proc;
        proc.prepareToPlay (48000.0, kBlock);
        Counter counter;
        proc.addListener (&counter);
        std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
        auto& editor = *static_cast<TutkalEditor*> (ed.get());
        auto& front = editor.frontPanel();
        auto indexOf = [&] (const char* id) { return proc.apvts.getParameter (id)->getParameterIndex(); };
        auto raw = [&] (const char* id) { return proc.apvts.getRawParameterValue (id)->load(); };
        auto typeIn = [&] (Knob* k, const juce::String& t)
        {
            k->box->beginEdit();
            if (auto* f = k->box->textField()) { f->setText (t, false); if (f->onReturnKey) f->onReturnKey(); }
        };
        auto mouse = [&] (juce::Component& c, juce::ModifierKeys m)
        {
            return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), juce::Point<float> (10.0f, 10.0f), m,
                                     juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation, juce::MouseInputSource::defaultRotation,
                                     juce::MouseInputSource::defaultTiltX, juce::MouseInputSource::defaultTiltY, &c, &c, juce::Time::getCurrentTime(),
                                     juce::Point<float> (10.0f, 10.0f), juce::Time::getCurrentTime(), 1, false);
        };

        // 1. What is shown.
        auto* thr = front.findKnob ("threshold");
        check (thr != nullptr, "GLUE face: a THRESHOLD knob with a value window");
        if (thr != nullptr)
        {
            check (thr->box->text() == "-18.0 dB" || thr->box->text() == "-12.0 dB", "THRESHOLD window reads a plain dB value: \"" + thr->box->text() + "\"");
            check (front.findKnob ("ratio")->box->text() == "4.0 : 1", "RATIO window: " + front.findKnob ("ratio")->box->text());
            check (front.findKnob ("attack")->box->text() == "10.0 ms", "ATTACK window: " + front.findKnob ("attack")->box->text());
            check (front.findKnob ("release")->box->text() == "0.30 s", "RELEASE window: " + front.findKnob ("release")->box->text());
            check (front.findKnob ("mix")->box->text() == "100 %", "MIX window: " + front.findKnob ("mix")->box->text());
            check (front.findKnob ("makeup")->box->text() == "0.0 dB", "MAKEUP window: " + front.findKnob ("makeup")->box->text());
            const float oldH = Theme::monoEm (8.5f).getHeight(), newH = Theme::monoEm (22.0f).getHeight();
            check (newH >= 2.5f * oldH, "value text is " + juce::String (newH / oldH, 2) + " x the old read-out height (" + juce::String (oldH, 1) + " -> " + juce::String (newH, 1) + " units)");
        }

        // 2. Type-in: "-22 dB" is exactly -22, one undo step, the host hears it.
        {
            const int steps0 = proc.undo.undoSteps();
            const float before = raw ("threshold");
            counter.changes.clear();
            typeIn (thr, "-22 dB");
            check (std::abs (raw ("threshold") + 22.0f) < 1.0e-4f, "type \"-22 dB\" into THRESHOLD: parameter = " + juce::String (raw ("threshold"), 6) + " (was " + juce::String (before, 3) + ")");
            check (proc.undo.undoSteps() == steps0 + 1, "  ... exactly one undo step (" + juce::String (proc.undo.undoSteps() - steps0) + ")");
            check (counter.changes[indexOf ("threshold")] >= 1, "  ... the host was told (setValueNotifyingHost, " + juce::String (counter.changes[indexOf ("threshold")]) + " notification)");
            check (! thr->box->isEditing(), "  ... the field is closed after Enter");
            check (thr->box->text() == "-22.0 dB", "  ... the window now reads " + thr->box->text());
            check (proc.undo.undo() && std::abs (raw ("threshold") - before) < 1.0e-4f, "  ... one Cmd+Z puts the old value back (" + juce::String (raw ("threshold"), 3) + ")");
            proc.undo.redo();
        }
        // 3. Garbage, Esc, clamping, units.
        {
            const int steps0 = proc.undo.undoSteps();
            const float v0 = raw ("threshold");
            counter.changes.clear();
            typeIn (thr, "banana");
            check (raw ("threshold") == v0 && proc.undo.undoSteps() == steps0 && counter.changes.empty(), "type garbage (banana): no change, no undo step, no host message");
            typeIn (thr, "12 apples");
            check (raw ("threshold") == v0 && proc.undo.undoSteps() == steps0, "type \"12 apples\": no change");
            typeIn (thr, "");
            check (raw ("threshold") == v0 && proc.undo.undoSteps() == steps0, "type nothing + Enter: no change");
            thr->box->beginEdit();
            thr->box->textField()->setText ("-30", false);
            const bool openBefore = thr->box->isEditing();
            thr->box->textField()->onEscapeKey();
            check (openBefore && ! thr->box->isEditing() && raw ("threshold") == v0 && proc.undo.undoSteps() == steps0 && counter.changes.empty(), "Esc: field closes, no change, no step, no host message");
            thr->box->beginEdit();
            thr->box->textField()->setText ("-30", false);
            thr->box->textField()->onFocusLost();
            check (! thr->box->isEditing() && raw ("threshold") == v0, "focus lost (click elsewhere): cancels");
            typeIn (thr, "-99");
            check (std::abs (raw ("threshold") + 40.0f) < 1.0e-4f, "-99 is clamped to the range: " + juce::String (raw ("threshold"), 2));
            typeIn (thr, "5");
            check (std::abs (raw ("threshold")) < 1.0e-4f, "+5 is clamped to the range: " + juce::String (raw ("threshold"), 2));
            typeIn (thr, "-18");
            check (std::abs (raw ("threshold") + 18.0f) < 1.0e-4f, "plain number \"-18\"");

            auto* ratio = front.findKnob ("ratio");
            typeIn (ratio, "6:1");
            check (std::abs (raw ("ratio") - 6.0f) < 1.0e-3f, "type \"6:1\" into RATIO: " + juce::String (raw ("ratio"), 4));
            typeIn (ratio, "6");
            check (std::abs (raw ("ratio") - 6.0f) < 1.0e-3f, "type \"6\" into RATIO: " + juce::String (raw ("ratio"), 4));
            auto* attack = front.findKnob ("attack");
            typeIn (attack, "25 ms");
            check (std::abs (raw ("attack") - 25.0f) < 1.0e-3f, "type \"25 ms\" into ATTACK: " + juce::String (raw ("attack"), 4));
            typeIn (attack, "0.002 s");
            check (std::abs (raw ("attack") - 2.0f) < 1.0e-3f, "type \"0.002 s\" into ATTACK: " + juce::String (raw ("attack"), 4));
            auto* release = front.findKnob ("release");
            typeIn (release, "0.4 s");
            check (std::abs (raw ("release") - 400.0f) < 1.0e-2f, "type \"0.4 s\" into RELEASE: " + juce::String (raw ("release"), 3));
            typeIn (release, "650 ms");
            check (std::abs (raw ("release") - 650.0f) < 1.0e-2f, "type \"650 ms\" into RELEASE: " + juce::String (raw ("release"), 3));
            typeIn (release, "400 ms");
            check (std::abs (raw ("release") - 400.0f) < 1.0e-2f && release->box->text() == "0.40 s", "type \"400 ms\" into RELEASE: window " + release->box->text());
            auto* mix = front.findKnob ("mix");
            typeIn (mix, "50 %");
            check (std::abs (raw ("mix") - 50.0f) < 1.0e-3f && mix->box->text() == "50 %", "type \"50 %\" into MIX: window " + mix->box->text());
        }
        // 4. Option-click and Cmd-click: back to the default, one undo step each.
        {
            auto resetTest = [&] (int mods, const juce::String& label)
            {
                typeIn (thr, "-27");
                const int steps0 = proc.undo.undoSteps();
                counter.changes.clear();
                const auto down = mouse (thr->slider, juce::ModifierKeys (mods | juce::ModifierKeys::leftButtonModifier));
                thr->slider.mouseDown (down);
                thr->slider.mouseUp (down);
                const tutkal::Params d;
                check (std::abs (raw ("threshold") - d.thresholdDb) < 1.0e-4f, label + "-click on THRESHOLD: back to the default " + juce::String (d.thresholdDb, 1) + " (is " + juce::String (raw ("threshold"), 3) + ")");
                check (proc.undo.undoSteps() == steps0 + 1 && counter.changes[indexOf ("threshold")] >= 1, "  ... one undo step, host told");
            };
            resetTest (juce::ModifierKeys::altModifier, "Option");
            resetTest (juce::ModifierKeys::commandModifier, "Cmd");
            // A plain click on the knob does not reset and adds no step.
            typeIn (thr, "-27");
            const int steps0 = proc.undo.undoSteps();
            const auto down = mouse (thr->slider, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier));
            thr->slider.mouseDown (down);
            thr->slider.mouseUp (down);
            check (std::abs (raw ("threshold") + 27.0f) < 1.0e-4f && proc.undo.undoSteps() == steps0, "a plain click on the knob changes nothing and adds no step");
            // Double-click on the knob: back to the default, one undo step (owner 2026-10-12). On the window: the type-in field.
            thr->slider.mouseDown (down); thr->slider.mouseUp (down); thr->slider.mouseDown (down);
            thr->slider.mouseDoubleClick (down);
            thr->slider.mouseUp (down);
            check (std::abs (raw ("threshold") + 18.0f) < 1.0e-4f && ! thr->box->isEditing() && proc.undo.undoSteps() == steps0 + 1,
                   "double-click on the knob: THRESHOLD back to -18.0 (was -27), no field, one undo step");
            typeIn (thr, "-27");
            const int steps1 = proc.undo.undoSteps();
            thr->box->mouseDoubleClick (down);
            check (thr->box->isEditing(), "double-click on the value window opens the type-in field");
            thr->box->textField()->onEscapeKey();
            check (proc.undo.undoSteps() == steps1, "opening and closing the field adds no step");
        }
        // 5. LA / PUMP faces: PEAK REDUCTION (0 .. 10), choices.
        {
            front.voicingKey (2).onClick();
            front.tick();
            auto* note = front.findKnob ("pumpNote");
            auto* shape = front.findKnob ("pumpShape");
            auto* depth = front.findKnob ("depth");
            check (note && shape && depth, "PUMP face: DEPTH, NOTE, SHAPE value windows");
            if (note && shape && depth)
            {
                typeIn (note, "1/4");
                check (std::lround (raw ("pumpNote")) == 2 && note->box->text() == "1/4", "type \"1/4\" into NOTE: window " + note->box->text());
                typeIn (note, "1/16");
                check (std::lround (raw ("pumpNote")) == 0, "type \"1/16\" into NOTE");
                const int steps0 = proc.undo.undoSteps();
                typeIn (note, "1/3");
                check (std::lround (raw ("pumpNote")) == 0 && proc.undo.undoSteps() == steps0, "type \"1/3\" into NOTE: no change");
                typeIn (shape, "swell");
                check (std::lround (raw ("pumpShape")) == 2 && shape->box->text() == "SWELL", "type \"swell\" into SHAPE: window " + shape->box->text());
                typeIn (shape, "exp");
                check (std::lround (raw ("pumpShape")) == 0 && shape->box->text() == "EXP", "type \"exp\" into SHAPE: window " + shape->box->text());
                typeIn (depth, "14 dB");
                check (std::abs (raw ("depth") - 14.0f) < 1.0e-3f && depth->box->text() == "14.0 dB", "type \"14 dB\" into DEPTH: window " + depth->box->text());
            }
            front.voicingKey (1).onClick();
            front.tick();
            auto* pr = front.findKnob ("peakReduction");
            check (pr != nullptr, "LA face: PEAK REDUCTION value window");
            if (pr != nullptr)
            {
                typeIn (pr, "7");
                check (std::abs (raw ("peakReduction") - 0.7f) < 1.0e-3f && pr->box->text() == "7.0", "type \"7\" into PEAK REDUCTION (scale 0..10): parameter " + juce::String (raw ("peakReduction"), 3) + ", window " + pr->box->text());
            }
            auto* gain = front.findKnob ("makeup");
            typeIn (gain, "6");
            check (gain && std::abs (raw ("makeup") - 6.0f) < 1.0e-3f && gain->box->text() == "6.0 dB", "type \"6\" into GAIN: window " + gain->box->text());
            front.voicingKey (0).onClick();
            front.tick();
        }
        // 6. Rear plate.
        {
            editor.showRear (true);
            auto& rear = editor.rearPanel();
            auto* hpf = rear.findKnob ("scHpf");
            auto* drive = rear.findKnob ("drive");
            check (hpf && drive, "rear plate: SC HPF and DRIVE value windows");
            if (hpf && drive)
            {
                check (hpf->box->text() == "OFF" && drive->box->text() == "0 %", "rear windows: " + hpf->box->text() + " / " + drive->box->text());
                typeIn (hpf, "150 Hz");
                check (std::abs (raw ("scHpf") - 150.0f) < 1.0e-3f && hpf->box->text() == "150 Hz", "type \"150 Hz\" into SC HPF: window " + hpf->box->text());
                typeIn (hpf, "off");
                check (hpf->box->text() == "OFF", "type \"off\" into SC HPF: window " + hpf->box->text());
                typeIn (hpf, "auto");
                check (raw ("scHpf") == 0.0f && hpf->box->text() == "OFF", "type \"auto\" into SC HPF: window " + hpf->box->text());
                typeIn (drive, "35");
                check (std::abs (raw ("drive") - 35.0f) < 1.0e-3f && drive->box->text() == "35 %", "type \"35\" into DRIVE: window " + drive->box->text());
                const int steps0 = proc.undo.undoSteps();
                typeIn (hpf, "loud");
                check (raw ("scHpf") == 0.0f && proc.undo.undoSteps() == steps0, "type garbage into SC HPF: no change");
            }
            editor.showRear (false);
        }
        // 7. Nothing crowds: on every face the value windows stay inside the panel, apart from each other and clear of the footer plaque.
        {
            auto spread = [&] (const char* what, juce::Component& panel, const std::vector<Knob*>& knobs)
            {
                bool ok = true;
                juce::String why;
                for (size_t i = 0; i < knobs.size(); ++i)
                {
                    const auto bi = knobs[i]->box->getBounds().reduced (3);   // the glass, without the component margin
                    if (! panel.getLocalBounds().contains (bi)) { ok = false; why << " outside:" << knobs[i]->id; }
                    for (size_t j = i + 1; j < knobs.size(); ++j)
                        if (bi.intersects (knobs[j]->box->getBounds().reduced (3))) { ok = false; why << " overlap:" << knobs[i]->id << "/" << knobs[j]->id; }
                    // the knob's own slider and the next knob's slider stay clear of the window
                    for (auto* k : knobs)
                        if (k != knobs[i] && bi.intersects (k->slider.getBounds().reduced (6))) { ok = false; why << " on knob:" << knobs[i]->id << "/" << k->id; }
                }
                check (ok, juce::String (what) + ": " + juce::String ((int) knobs.size()) + " value windows, no overlap" + why);
            };
            for (int v = 0; v < 3; ++v)
            {
                front.voicingKey (v).onClick();
                front.tick();
                std::vector<Knob*> ks;
                for (const char* id : { "threshold", "ratio", "attack", "release", "makeup", "mix", "peakReduction", "pumpNote", "pumpShape", "depth" })
                    if (auto* k = front.findKnob (id)) ks.push_back (k);
                const char* names[3] { "GLUE face", "LA face", "PUMP face" };
                spread (names[v], front, ks);
                int lowest = 0;
                for (auto* k : ks) lowest = juce::jmax (lowest, k->box->getBottom() - 3);
                check (lowest <= 590, juce::String (names[v]) + ": lowest value window ends at " + juce::String (lowest) + " (footer plaque starts at 597)");
            }
            front.voicingKey (0).onClick();
            front.tick();
            std::vector<Knob*> rk { editor.rearPanel().findKnob ("scHpf"), editor.rearPanel().findKnob ("drive") };
            const auto b0 = rk[0]->box->getBounds().reduced (3), b1 = rk[1]->box->getBounds().reduced (3);
            check (! b0.intersects (b1) && b0.getBottom() <= 349 && b1.getBottom() <= 349, "rear plate: value windows apart, end at " + juce::String (b0.getBottom()) + " (the notes start at 352)");
        }
        proc.removeListener (&counter);
    }
    return (ok && gFail == 0) ? 0 : 1;
}
