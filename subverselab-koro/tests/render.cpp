/*  KORO console tool.

        render --selftest
            1. the engine's own selftest (tools/engine_render.cpp, verbatim from CIFTE),
            2. the plugin checks below (latency, bypass, state, moods, read-outs, ...).
            Run it from the subverselab-koro folder: the engine's female-vocal checks read
            _kaynak/Kadin_vokal_20s.wav (a link to ../prototypes/_kaynak).

        render in.wav out.wav [--mood "CHOIR"] [--param id=value]...
            renders through the whole plugin (KoroProcessor), latency removed, 24-bit.
*/
#include <JuceHeader.h>
#include "EngineCli.h"
#include "../Source/PluginProcessor.h"
#include "SvlUndoTests.h"
#include "SvlControlProof.h"

namespace
{
int checks = 0;
bool check (const juce::String& name, bool ok, const juce::String& detail)
{
    ++checks;
    std::printf ("%s %s %s\n", ok ? "PASS" : "FAIL", name.toRawUTF8(), detail.toRawUTF8());
    std::fflush (stdout);
    return ok;
}

void setParam (KoroProcessor& p, const juce::String& id, float value)
{
    auto* prm = p.apvts.getParameter (id);
    jassert (prm != nullptr);
    prm->setValueNotifyingHost (prm->convertTo0to1 (value));
}

using Block = std::function<void (KoroProcessor&, int64_t blockStart)>;

/** Runs a stereo signal through the processor in blocks; returns the output (interleaved L/R). */
std::vector<float> run (KoroProcessor& p, const std::vector<float>& left, const std::vector<float>& right, int block = 512, Block perBlock = {})
{
    std::vector<float> out (left.size() * 2);
    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer midi;
    for (size_t pos = 0; pos < left.size(); pos += (size_t) block)
    {
        const int n = (int) std::min<size_t> ((size_t) block, left.size() - pos);
        if (perBlock) perBlock (p, (int64_t) pos);
        buf.setSize (2, n, false, false, true);
        for (int i = 0; i < n; ++i)
        {
            buf.setSample (0, i, left[pos + (size_t) i]);
            buf.setSample (1, i, right[pos + (size_t) i]);
        }
        p.processBlock (buf, midi);
        for (int i = 0; i < n; ++i)
        {
            out[2 * (pos + (size_t) i)] = buf.getSample (0, i);
            out[2 * (pos + (size_t) i) + 1] = buf.getSample (1, i);
        }
    }
    return out;
}

std::vector<float> channel (const std::vector<float>& inter, int c)
{
    std::vector<float> y (inter.size() / 2);
    for (size_t i = 0; i < y.size(); ++i) y[i] = inter[2 * i + (size_t) c];
    return y;
}

double rmsDiff (const std::vector<float>& a, const std::vector<float>& b)
{
    double s = 0.0;
    const size_t n = std::min (a.size(), b.size());
    for (size_t i = 0; i < n; ++i) s += (double) (a[i] - b[i]) * (a[i] - b[i]);
    return std::sqrt (s / (double) std::max<size_t> (1, n));
}

std::unique_ptr<KoroProcessor> fresh (double sr, int mood = -1, int block = 512)
{
    auto p = std::make_unique<KoroProcessor>();
    if (mood >= 0) p->applyMood (mood);
    p->prepareToPlay (sr, block);
    return p;
}

std::vector<float> melody (int rate, int root, int scale, double seconds, uint32_t seed, std::vector<double>* f0 = nullptr)
{
    return enginecli::scaleMelody (rate, { { seconds, root, scale } }, seed, f0);
}

bool pluginChecks()
{
    bool all = true;

    {   // 1. latency reported = the engine's fixed latency, scaled with the rate
        juce::String d;
        bool ok = true;
        for (auto [sr, want] : { std::pair<double, int> { 44100.0, 2386 }, { 48000.0, 2596 }, { 96000.0, 5179 } })
        {
            auto p = fresh (sr);
            cifte::CifteEngine e; e.prepare (sr);
            ok &= p->getLatencySamples() == want && e.latency() == want;
            d << juce::String ((int) sr) << ":" << p->getLatencySamples() << " ";
        }
        all &= check ("plugin_latency_reported", ok, d + "(want 2386 / 2596 / 5179)");
    }
    {   // 2. latency measured on an impulse = reported, doubler mood and harmony mood
        juce::String d;
        bool ok = true;
        for (int mood : { 0, 3, 5 })
        {
            auto p = fresh (44100.0, mood);
            std::vector<float> x (44100, 0.0f);
            x[1000] = 0.8f;
            const auto y = channel (run (*p, x, x), 0);
            const int at = (int) (std::max_element (y.begin(), y.end(), [] (float a, float b) { return std::abs (a) < std::abs (b); }) - y.begin()) - 1000;
            ok &= at == p->getLatencySamples();
            d << KoroProcessor::moodNames[mood] << "=" << at << " ";
        }
        all &= check ("plugin_latency_measured", ok, d + "reported=2386");
    }
    {   // 3. BYPASS (parameter and host bypass) = the dry input delayed by the latency, sample exact
        std::vector<float> l (48000), r (48000);
        juce::Random rng (5);
        for (size_t i = 0; i < l.size(); ++i) { l[i] = rng.nextFloat() - 0.5f; r[i] = rng.nextFloat() - 0.5f; }
        auto p = std::make_unique<KoroProcessor>();
        p->applyMood (5);
        setParam (*p, "bypass", 1.0f);
        p->prepareToPlay (48000.0, 512);
        const auto y = run (*p, l, r);
        const int D = p->getLatencySamples();
        float err = 0.0f;
        for (size_t i = 0; i < l.size(); ++i)
        {
            const float wl = i >= (size_t) D ? l[i - (size_t) D] : 0.0f, wr = i >= (size_t) D ? r[i - (size_t) D] : 0.0f;
            err = std::max ({ err, std::abs (y[2 * i] - wl), std::abs (y[2 * i + 1] - wr) });
        }
        // Host bypass path.
        auto q = fresh (48000.0, 5);
        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer midi;
        float err2 = 0.0f;
        for (size_t pos = 0; pos + 512 <= l.size(); pos += 512)
        {
            for (int i = 0; i < 512; ++i) { buf.setSample (0, i, l[pos + (size_t) i]); buf.setSample (1, i, r[pos + (size_t) i]); }
            q->processBlockBypassed (buf, midi);
            for (int i = 0; i < 512; ++i)
            {
                const size_t k = pos + (size_t) i;
                const float wl = k >= (size_t) D ? l[k - (size_t) D] : 0.0f;
                err2 = std::max (err2, std::abs (buf.getSample (0, i) - wl));
            }
        }
        all &= check ("plugin_bypass_is_delayed_dry", err == 0.0f && err2 == 0.0f,
                      "param_bypass_max_err=" + juce::String (err) + " host_bypass_max_err=" + juce::String (err2) + " delay=" + juce::String (D));
    }
    {   // 4. DOUBLE 0 + HARMONY 0 = the input delayed by the latency (the engine is transparent there)
        auto p = std::make_unique<KoroProcessor>();
        setParam (*p, "double", 0.0f);
        setParam (*p, "harmony", 0.0f);
        p->prepareToPlay (44100.0, 256);
        const auto x = melody (44100, 0, 0, 3.0, 3u);
        const auto y = channel (run (*p, x, x, 256), 0);
        const int D = p->getLatencySamples();
        float err = 0.0f;
        for (size_t i = (size_t) D; i < x.size(); ++i) err = std::max (err, std::abs (y[i] - x[i - (size_t) D]));
        all &= check ("plugin_transparent_at_zero", err < 1.0e-6f, "max_err=" + juce::String (err, 9));
    }
    {   // 5. BYPASS toggled mid-signal: a 20 ms crossfade, no click
        auto p = fresh (48000.0, 0);
        std::vector<float> x (96000);
        for (size_t i = 0; i < x.size(); ++i) x[i] = 0.5f * std::sin (2.0f * 3.14159265f * 220.0f * (float) i / 48000.0f);
        const auto y = channel (run (*p, x, x, 512, [] (KoroProcessor& q, int64_t pos)
                                     { if (pos == 30720) setParam (q, "bypass", 1.0f); if (pos == 61440) setParam (q, "bypass", 0.0f); }), 0);
        float jy = 0.0f, jx = 0.0f;
        for (size_t i = 5000; i < y.size(); ++i) jy = std::max (jy, std::abs (y[i] - y[i - 1]));
        for (size_t i = 1; i < x.size(); ++i) jx = std::max (jx, std::abs (x[i] - x[i - 1]));
        all &= check ("plugin_bypass_toggle_no_click", jy < 1.5f * jx, "max_jump=" + juce::String (jy, 5) + " dry_max_jump=" + juce::String (jx, 5));
    }
    {   // 6. state save / restore: every parameter and the editor size come back
        auto a = std::make_unique<KoroProcessor>();
        juce::Random rng (77);
        for (auto* prm : a->getParameters())
            if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (prm))   // a legal value (bools and choices snapped)
                rp->setValueNotifyingHost (rp->convertTo0to1 (rp->convertFrom0to1 (rng.nextFloat())));
        a->editorWidth = 1440;
        juce::MemoryBlock mb;
        a->getStateInformation (mb);
        auto b = std::make_unique<KoroProcessor>();
        b->setStateInformation (mb.getData(), (int) mb.getSize());
        float worst = 0.0f;
        const auto pa = a->getParameters(), pb = b->getParameters();
        juce::String bad;
        for (int i = 0; i < pa.size(); ++i)
        {
            const float e = std::abs (pa[i]->getValue() - pb[i]->getValue());
            if (e > 1.0e-6f) bad << pa[i]->getName (32) << " ";
            worst = std::max (worst, e);
        }
        all &= check ("plugin_state_roundtrip", worst < 1.0e-6f && b->editorWidth.load() == 1440 && pa.size() == 20,
                      juce::String (pa.size()) + " parameters, max_err=" + juce::String (worst, 8) + " editorWidth=" + juce::String (b->editorWidth.load()) + (bad.isEmpty() ? juce::String() : " differ: " + bad));
    }
    {   // 7. MOOD recall: the face + rear values it writes give back the engine's mood Controls
        juce::String d;
        bool ok = true;
        for (int m = 0; m < 6; ++m)
        {
            auto p = std::make_unique<KoroProcessor>();
            setParam (*p, "double", 1.0f);  setParam (*p, "width", 1.0f);  setParam (*p, "color", 0.9f);   // disturb first
            p->applyMood (m);
            cifte::Controls want;
            cifte::moods::get (cifte::moods::kNames[m], want);
            const auto c = p->targetControls();
            auto near = [] (float a, float b) { return std::abs (a - b) < 1.0e-4f; };
            bool mok = near (c.mix, want.mix) && near (c.width, want.width) && near (c.tone, want.tone) && near (c.harmony, want.harmony)
                    && near (c.color, want.color) && near (c.spread, want.spread) && near (c.tight, want.tight) && c.harmonyVoices == want.harmonyVoices
                    && near (c.glide, want.glide) && near (c.timing, want.timing) && c.monoSafe == want.monoSafe && c.keyMode == cifte::KeyMode::Auto;
            for (size_t v = 0; v < 3; ++v)
                mok = mok && near (c.voiceLevel[v], want.voiceLevel[v]) && near (c.voicePan[v], want.voicePan[v]);
            if (want.mix > 0.0f) mok = mok && c.voices == want.voices;   // the DOUBLE macro reproduces the voice count
            ok &= mok && (int) p->apvts.getRawParameterValue ("mood")->load() == m;
            d << KoroProcessor::moodNames[m].replace (" ", "_") << (mok ? ":ok " : ":BAD ");
        }
        all &= check ("plugin_mood_recall", ok, d);
    }
    {   // 8. every control changes the output (CHOIR mood, synthetic C major singer)
        const auto x = melody (44100, 0, 0, 6.0, 8u);
        auto render = [&] (std::function<void (KoroProcessor&)> tweak)
        {
            auto p = std::make_unique<KoroProcessor>();
            p->applyMood (5);
            if (tweak) tweak (*p);
            p->prepareToPlay (44100.0, 512);
            return channel (run (*p, x, x), 0);
        };
        const auto base = render ({});
        const std::pair<const char*, float> tweaks[] {
            { "double", 9.0f }, { "width", 1.0f }, { "tone", 0.1f }, { "harmony", 9.0f }, { "color", 0.9f }, { "output", -6.0f },
            { "level1", 2.0f }, { "pan1", 1.0f }, { "level3", 0.0f }, { "glide", 120.0f }, { "timing", 30.0f }, { "monoSafe", 1.0f },
            { "keyOverride", 3.0f }, { "scaleOverride", 4.0f }, { "mood", 0.0f } };
        juce::String d;
        bool ok = true;
        for (auto [id, v] : tweaks)
        {
            const double delta = rmsDiff (base, render ([id = id, v = v] (KoroProcessor& p) { setParam (p, id, v); }));
            ok &= delta > 1.0e-4;
            d << id << "=" << juce::String (delta, 4) << " ";
        }
        all &= check ("plugin_controls_change_output", ok, d);
    }
    {   // 9. read-outs: key, scale, confidence, pitch IN / OUT, meter (AUTO, CHOIR, synthetic C major)
        std::vector<double> f0;
        const auto x = melody (48000, 0, 0, 8.0, 11u, &f0);
        auto p = fresh (48000.0, 5, 480);
        int voicedBlocks = 0, pitchHits = 0, outBlocks = 0;
        run (*p, x, x, 480, [&] (KoroProcessor& q, int64_t pos)
        {
            if (pos < 48000 * 3 || ! q.ui.pitchValid.load()) return;
            ++voicedBlocks;
            // The IN lamp's pitch must be the sung pitch of the last ~60 ms (octave errors count as misses).
            const float in = q.ui.inCents.load();
            double best = 1.0e9;
            for (int64_t k = std::max<int64_t> (0, pos - 2880); k < pos; k += 48)
                best = std::min (best, std::abs (in - (6900.0 + 1200.0 * std::log2 (f0[(size_t) k] / 440.0))));
            pitchHits += best < 50.0;
            outBlocks += q.ui.outValid.load();
        });
        const bool keyOk = p->ui.hasKey.load() && p->ui.root.load() == 0 && p->ui.scale.load() == 0 && p->ui.confidence.load() > 0.0f;
        const double hit = 100.0 * pitchHits / juce::jmax (1, voicedBlocks);
        all &= check ("plugin_readouts", keyOk && hit >= 90.0 && outBlocks > 0 && p->ui.rmsL.load() > 0.0f,
                      "key=" + juce::String (p->ui.root.load()) + "/" + juce::String (p->ui.scale.load()) + " (want C Major)"
                      + " confidence=" + juce::String (p->ui.confidence.load(), 3) + " pitch_in_within_50c=" + juce::String (hit, 1) + "%"
                      + " of " + juce::String (voicedBlocks) + " voiced blocks, out_valid_blocks=" + juce::String (outBlocks)
                      + " meter_rms=" + juce::String (p->ui.rmsL.load(), 4));
    }
    {   // 10. LOCK holds the key: C major, then D Hicaz; locked at 6 s it stays C major, unlocked it moves
        const auto x = enginecli::scaleMelody (44100, { { 6.0, 0, 0 }, { 12.0, 2, 3 } }, 31u);
        auto keyAtEnd = [&] (bool lock)
        {
            auto p = fresh (44100.0, 3);
            run (*p, x, x, 512, [lock] (KoroProcessor& q, int64_t pos) { if (lock && pos >= 6 * 44100 && pos < 6 * 44100 + 512) setParam (q, "lock", 1.0f); });
            return std::make_tuple (p->ui.root.load(), p->ui.scale.load(), p->ui.locked.load());
        };
        const auto [r1, s1, l1] = keyAtEnd (true);
        const auto [r0, s0, l0] = keyAtEnd (false);
        all &= check ("plugin_lock_holds_key", r1 == 0 && s1 == 0 && l1 && r0 == 2 && s0 == 3 && ! l0,
                      "locked_end=" + juce::String (r1) + "/" + juce::String (s1) + " unlocked_end=" + juce::String (r0) + "/" + juce::String (s0) + " (want 0/0 and 2/3)");
    }
    {   // 11. KEY / SCALE override on the rear: MANUAL, shown on the display
        auto p = std::make_unique<KoroProcessor>();
        setParam (*p, "keyOverride", 3.0f);    // D
        setParam (*p, "scaleOverride", 4.0f);  // HICAZ
        p->prepareToPlay (44100.0, 512);
        const auto x = melody (44100, 0, 0, 1.0, 2u);
        run (*p, x, x);
        const auto c = p->targetControls();
        all &= check ("plugin_manual_override", p->ui.manual.load() && p->ui.root.load() == 2 && p->ui.scale.load() == 3 && p->ui.confidence.load() == 1.0f
                                                  && c.keyMode == cifte::KeyMode::Manual && c.root == 2 && c.scale == 3,
                      "root=" + juce::String (p->ui.root.load()) + " scale=" + juce::String (p->ui.scale.load()) + " manual=" + juce::String ((int) p->ui.manual.load()));
    }
    {   // 12. SAFE flag reaches the display on the real female vocal (THIRD mood, AUTO)
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (juce::File::getCurrentWorkingDirectory().getChildFile ("_kaynak/Kadin_vokal_20s.wav")));
        if (rd == nullptr)
            all &= check ("plugin_safe_flag", false, "_kaynak/Kadin_vokal_20s.wav not found (run from subverselab-koro)");
        else
        {
            juce::AudioBuffer<float> src ((int) rd->numChannels, (int) rd->lengthInSamples);
            rd->read (&src, 0, (int) rd->lengthInSamples, 0, true, true);
            std::vector<float> l ((size_t) src.getNumSamples()), r (l.size());
            for (size_t i = 0; i < l.size(); ++i) { l[i] = src.getSample (0, (int) i); r[i] = src.getSample (juce::jmin (1, src.getNumChannels() - 1), (int) i); }
            auto p = fresh (rd->sampleRate, 3);
            int blocks = 0, safe = 0, keyed = 0;
            run (*p, l, r, 512, [&] (KoroProcessor& q, int64_t) { ++blocks; safe += q.ui.safe.load(); keyed += q.ui.hasKey.load(); });
            all &= check ("plugin_safe_flag", safe > 0 && keyed > 0,
                          "safe_blocks=" + juce::String (100.0 * safe / blocks, 1) + "% key_blocks=" + juce::String (100.0 * keyed / blocks, 1) + "% final_key="
                          + juce::String (p->ui.root.load()) + "/" + juce::String (p->ui.scale.load()));
        }
    }
    {   // 13. random automation of every parameter, every block: finite, bounded
        auto p = fresh (48000.0, 5);
        const auto x = melody (48000, 7, 1, 10.0, 99u);
        juce::Random rng (123);
        const auto y = run (*p, x, x, 256, [&] (KoroProcessor& q, int64_t)
        {
            auto params = q.getParameters();
            auto* prm = params[rng.nextInt (params.size())];
            if (prm != q.getBypassParameter()) prm->setValueNotifyingHost (rng.nextFloat());
        });
        bool finite = true;
        float pk = 0.0f;
        for (float v : y) { finite &= std::isfinite (v); pk = std::max (pk, std::abs (v)); }
        all &= check ("plugin_automation_finite", finite && pk < 6.0f, "finite=" + juce::String ((int) finite) + " peak=" + juce::String (pk, 3));
    }
    {   // 14. mono in / stereo out and mono / mono
        bool ok = true;
        juce::String d;
        for (auto [in, out] : { std::pair<int, int> { 1, 2 }, { 1, 1 } })
        {
            KoroProcessor p;
            juce::AudioProcessor::BusesLayout lay;
            lay.inputBuses.add (in == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo());
            lay.outputBuses.add (out == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo());
            ok &= p.setBusesLayout (lay);
            p.prepareToPlay (44100.0, 512);
            juce::AudioBuffer<float> buf (juce::jmax (in, out), 512);
            juce::MidiBuffer midi;
            const auto x = melody (44100, 0, 0, 2.0, 4u);
            float pk = 0.0f;
            for (size_t pos = 0; pos + 512 <= x.size(); pos += 512)
            {
                buf.clear();
                for (int i = 0; i < 512; ++i) buf.setSample (0, i, x[pos + (size_t) i]);
                p.processBlock (buf, midi);
                for (int c = 0; c < out; ++c) for (int i = 0; i < 512; ++i) { const float v = buf.getSample (c, i); ok &= std::isfinite (v); pk = std::max (pk, std::abs (v)); }
            }
            ok &= pk > 0.01f;
            d << in << "in/" << out << "out peak=" << juce::String (pk, 3) << " ";
        }
        all &= check ("plugin_mono_layouts", ok, d);
    }
    {   // 15. CPU: 60 s of CHOIR (double + three harmony voices) at 48 kHz
        auto p = fresh (48000.0, 5);
        const auto x = melody (48000, 9, 1, 60.0, 5u);
        const auto t0 = std::chrono::steady_clock::now();
        run (*p, x, x);
        const double s = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        all &= check ("plugin_cpu_60s", s < 6.0, "seconds=" + juce::String (s, 3) + " (CHOIR, 48 kHz, 512-sample blocks)");
    }
    {   // 16. undo / redo: editor edits are steps, host automation is not (SvlUndoTests.h; the checks are counted too)
        const auto u = svl::undotest::run<KoroProcessor> ("KORO", [] { return std::make_unique<KoroProcessor>(); },
                                                          { { "double", 7.3f }, { "output", 5.5f }, { "tone", 0.9f } },
                                                          [] (KoroProcessor& p) { p.applyMood (3); });
        checks += u.pass + u.fail;
        all &= check ("plugin_undo_redo", u.fail == 0, juce::String (u.pass) + " of " + juce::String (u.pass + u.fail) + " undo checks passed");
    }
    return all;
}

int renderFile (int argc, char** argv)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (juce::File::getCurrentWorkingDirectory().getChildFile (argv[1])));
    if (rd == nullptr) { std::fprintf (stderr, "cannot read %s\n", argv[1]); return 1; }
    KoroProcessor p;
    for (int i = 3; i < argc; ++i)
    {
        const juce::String a (argv[i]);
        if (a == "--mood" && i + 1 < argc)
        {
            const int m = KoroProcessor::moodNames.indexOf (juce::String (argv[++i]).toUpperCase());
            if (m < 0) { std::fprintf (stderr, "unknown mood\n"); return 2; }
            p.applyMood (m);
        }
        else if (a == "--param" && i + 1 < argc)
        {
            const juce::String kv (argv[++i]);
            setParam (p, kv.upToFirstOccurrenceOf ("=", false, false), kv.fromFirstOccurrenceOf ("=", false, false).getFloatValue());
        }
    }
    p.prepareToPlay (rd->sampleRate, 512);
    const int len = (int) rd->lengthInSamples, D = p.getLatencySamples();
    juce::AudioBuffer<float> src ((int) rd->numChannels, len);
    rd->read (&src, 0, len, 0, true, true);
    std::vector<float> l ((size_t) (len + D), 0.0f), r (l.size(), 0.0f);
    for (int i = 0; i < len; ++i) { l[(size_t) i] = src.getSample (0, i); r[(size_t) i] = src.getSample (juce::jmin (1, src.getNumChannels() - 1), i); }
    const auto y = run (p, l, r);
    juce::AudioBuffer<float> out (2, len);
    for (int i = 0; i < len; ++i) { out.setSample (0, i, y[2 * (size_t) (i + D)]); out.setSample (1, i, y[2 * (size_t) (i + D) + 1]); }
    const juce::File f = juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]);
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> os (f.createOutputStream().release());
    auto opts = juce::AudioFormatWriterOptions().withSampleRate (rd->sampleRate).withNumChannels (2).withBitsPerSample (24);
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os, opts));
    if (w == nullptr) return 1;
    w->writeFromAudioSampleBuffer (out, 0, len);
    std::printf ("wrote %s (%d samples, latency %d removed)\n", argv[2], len, D);
    return 0;
}
} // namespace


// ---- --controlproof: every knob in every variation (SvlControlProof.h), through the real processor ----------------
int runControlProof (int argc, char** argv)
{
    namespace cp = svl::controlproof;
    using cp::feat::outRmsDb; using cp::feat::wetRmsDb; using cp::feat::wetBandDb; using cp::feat::stereoWidthDb; using cp::feat::wetBalanceDb; using cp::feat::wetCentroidHz;
    cp::Config c;
    c.name = "KORO";
    c.make = [] { return std::make_unique<KoroProcessor>(); };
    c.tailSeconds = 0.3;          // getTailLengthSeconds() = 0.2 s, plus 54 ms of latency and a margin
    c.designedGainDb = [] (const cp::StateView& s) { return (double) juce::jmax (0.0f, s.raw ("output")) + 6.0; };   // OUTPUT up to +12 dB; doubled voices add to the dry
    c.customName = "voice";
    // a voiced, mono, vowel-like tone (the tracker needs a pitch): 196 Hz for half a second, then 220 Hz, 5.5 Hz vibrato of +-12 cents, peak -6 dBFS
    c.customMaterial = [] (cp::Sig& s, double sr, int n)
    {
        s.zeros (n); double ph = 0;
        for (int i = 0; i < n; ++i)
        {
            const double t = (double) i / sr, f0 = (t < 0.5 ? 196.0 : 220.0) * std::pow (2.0, 12.0 * std::sin (6.283185307179586 * 5.5 * t) / 1200.0);
            ph += 6.283185307179586 * f0 / sr;
            double v = 0; for (int h = 1; h <= 20; ++h) v += std::sin (h * ph) / std::pow ((double) h, 1.2);
            const float x = (float) (v * std::min (1.0, t / 0.02));
            s.c[0][(size_t) i] = s.c[1][(size_t) i] = x;
        }
        const double g = 0.5011872336 / std::max (s.peak(), 1e-9);
        for (auto& ch : s.c) for (float& x : ch) x = (float) (x * g);
    };
    using Force = std::vector<std::pair<juce::String, float>>;
    const float kChoir = 1.0f, keyC = 1.0f / 12.0f, scaleMajor = 1.0f / 6.0f;
    auto D = [] (const char* id, int dir, cp::Feature f, double tol, const char* fname, const char* note, Force force = {}, int mat = cp::kCustom)
    {
        cp::Direction d; d.id = id; d.dir = dir; d.feature = f; d.tol = tol; d.featureName = fname; d.note = note; d.material = mat;
        d.force = { { "bypass", 0.0f } }; if (juce::String (id) != "output") d.force.push_back ({ "output", 0.5f });   // OUTPUT 0 dB, so the wet part is only what the knob under test makes
        for (auto& x : force) d.force.push_back (x);
        return d;
    };
    const Force manualHarmony = { { "mood", kChoir }, { "harmony", 1.0f }, { "double", 0.0f }, { "keyOverride", keyC }, { "scaleOverride", scaleMajor }, { "lock", 0.0f } };
    auto vf = [&] (Force f = {}) { Force r = manualHarmony; for (auto& x : f) r.push_back (x); return r; };
    c.directions = {
        D ("double", +1, wetRmsDb(), 0.25, "wet part (rms dB), HARMONY off", "more doubling = more of the doubled voices", { { "harmony", 0.0f } }),
        D ("width", +1, stereoWidthDb(), 0.25, "side / mid power (dB), HARMONY off, DOUBLE 5", "WIDTH spreads the doubled voices", { { "harmony", 0.0f }, { "double", 0.5f } }),
        D ("tone", +1, wetBandDb (8000.0, 16000.0), 0.25, "wet band 8-16 kHz power (dB) on pink, HARMONY off, DOUBLE 5", "TONE is the high cut of the voices, 3..16 kHz", { { "harmony", 0.0f }, { "double", 0.5f } }, cp::kPink),
        D ("harmony", +1, wetRmsDb(), 0.25, "wet part (rms dB), DOUBLE off, mood CHOIR", "more harmony = more harmony voice (at 10 the dry goes away, the wet part still grows)", { { "mood", kChoir }, { "double", 0.0f }, { "keyOverride", keyC }, { "scaleOverride", scaleMajor } }),
        D ("color", +1, wetCentroidHz(), 20.0, "spectral centroid of the wet part (Hz), key C major set by hand, mood CHOIR", "COLOR morphs the interval LOW 3RD > HIGH 3RD > 5TH > OCT, upward in pitch", vf()),
        D ("output", +1, outRmsDb(), 0.25, "output rms (dB)", "output gain"),
        D ("lock", 0, nullptr, 0, "", "freezes the detected key; no ordering"),
        D ("bypass", 0, nullptr, 0, "", "switch"),
        D ("mood", 0, nullptr, 0, "", "six interval / voice-count sets"),
        D ("level1", +1, wetRmsDb(), 0.25, "wet part (rms dB), HARMONY 10, mood CHOIR, key set by hand", "voice 1 level", vf()),
        D ("level2", +1, wetRmsDb(), 0.25, "wet part (rms dB), HARMONY 10, mood CHOIR, key set by hand", "voice 2 level", vf()),
        D ("level3", +1, wetRmsDb(), 0.25, "wet part (rms dB), HARMONY 10, mood CHOIR, key set by hand", "voice 3 level", vf()),
        D ("pan1", +1, wetBalanceDb(), 0.25, "right / left power of the wet part (dB), HARMONY 10, mood CHOIR, key set by hand", "voice 1 position", vf()),
        D ("pan2", +1, wetBalanceDb(), 0.25, "right / left power of the wet part (dB), HARMONY 10, mood CHOIR, key set by hand", "voice 2 position", vf()),
        D ("pan3", +1, wetBalanceDb(), 0.25, "right / left power of the wet part (dB), HARMONY 10, mood CHOIR, key set by hand", "voice 3 position", vf()),
        D ("glide", 2, wetRmsDb(), 0, "wet rms (dB), HARMONY 10, key set by hand", "portamento time of the harmony interval; a level feature cannot show an order", vf()),
        D ("timing", 2, wetRmsDb(), 0, "wet rms (dB), HARMONY 10, key set by hand", "late offset of the harmony voices; no ordering of level or colour implied", vf()),
        D ("monoSafe", 2, stereoWidthDb(), 0, "side / mid power (dB), HARMONY off, DOUBLE 5", "keeps the doubler mono compatible; the brief gives no direction", { { "harmony", 0.0f }, { "double", 0.5f } }),
        D ("keyOverride", 0, nullptr, 0, "", "manual key list; no ordering"),
        D ("scaleOverride", 0, nullptr, 0, "", "manual scale list; no ordering"),
    };
    return cp::main (c, argc, argv);
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    for (int i = 1; i < argc; ++i)
        if (juce::String (argv[i]).startsWith ("--controlproof")) return runControlProof (argc, argv);
    if (argc == 2 && juce::String (argv[1]) == "--selftest")
    {
        std::printf ("== ENGINE selftest (tools/engine_render.cpp, verbatim; engine md5 %s) ==\n", KORO_ENGINE_MD5);
        std::fflush (stdout);
        const bool engineOk = enginecli::selftest();
        std::printf ("== PLUGIN checks (KoroProcessor) ==\n");
        const bool pluginOk = pluginChecks();
        std::printf ("%s (engine %s, plugin %d checks)\n", engineOk && pluginOk ? "ALL PASS" : "NOT ALL PASS",
                     engineOk ? "ALL PASS" : "FAILED", checks);
        return engineOk && pluginOk ? 0 : 1;
    }
    if (argc >= 3)
        return renderFile (argc, argv);
    std::fprintf (stderr, "usage: render --selftest | render in.wav out.wav [--mood NAME] [--param id=value]...\n");
    return 2;
}
