/*  Drives the real HavaProcessor, the code path a DAW runs, and checks the
    plugin layer around the (separately self-tested) engine:

        plugin_check        exit code 0 only if every check passes

    defaults = AIR FULL, plugin output = engine output, bit-exact BYPASS (MASTER
    SAFE off) and bit-exact delayed BYPASS (on), latency 0 / 80 @ 48k / 74 @ 44.1k
    re-reported on the toggle, no click on the MASTER SAFE crossfade, mood recall,
    state round trip without a mood recall over saved values.
*/
#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"
#include "SvlUndoTests.h"
#include "SvlControlProof.h"
#include "SvlControlProof.h"

namespace
{
int failures = 0, checks = 0;

void report (bool ok, const juce::String& name, const juce::String& detail)
{
    ++checks;
    if (! ok) ++failures;
    std::printf ("%s %s  %s\n", ok ? "PASS" : "FAIL", name.toRawUTF8(), detail.toRawUTF8());
}

void set (HavaProcessor& p, const char* id, float value)
{
    auto* param = p.apvts.getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (value));
}

float get (HavaProcessor& p, const char* id) { return p.apvts.getRawParameterValue (id)->load(); }

/** Program material: noise bursts over a low sine, stereo, deterministic. */
juce::AudioBuffer<float> material (double sr, double seconds)
{
    const int n = (int) (sr * seconds);
    juce::AudioBuffer<float> b (2, n);
    juce::Random rng (72);
    for (int i = 0; i < n; ++i)
    {
        const double t = (double) i / sr;
        const double ph = std::fmod (t, 0.25);
        const float burst = (float) std::exp (-ph / 0.04) * 0.4f;
        const float s = 0.2f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 110.0 * t);
        b.setSample (0, i, s + burst * (rng.nextFloat() * 2.0f - 1.0f));
        b.setSample (1, i, 0.9f * s + burst * (rng.nextFloat() * 2.0f - 1.0f));
    }
    return b;
}

void run (HavaProcessor& p, juce::AudioBuffer<float>& buf, int block = 512, int from = 0, int to = -1)
{
    juce::MidiBuffer midi;
    if (to < 0) to = buf.getNumSamples();
    for (int pos = from; pos < to; pos += block)
    {
        const int n = juce::jmin (block, to - pos);
        juce::AudioBuffer<float> view (buf.getArrayOfWritePointers(), buf.getNumChannels(), pos, n);
        p.processBlock (view, midi);
    }
}

std::unique_ptr<HavaProcessor> make (double sr, std::function<void (HavaProcessor&)> setup = {})
{
    auto p = std::make_unique<HavaProcessor>();
#if SVL_LICENSING
    p->license.setLicensedForTesting (true);
#endif
    if (setup) setup (*p);
    p->setRateAndBufferSizeDetails (sr, 512);
    p->prepareToPlay (sr, 512);
    return p;
}

bool identical (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b, int from, int to, int shift = 0)
{
    for (int c = 0; c < 2; ++c)
        for (int i = from; i < to; ++i)
            if (std::memcmp (a.getReadPointer (c) + i, b.getReadPointer (c) + i - shift, sizeof (float)) != 0)
                return false;
    return true;
}
} // namespace

// ---- --controlproof: every knob in every variation (SvlControlProof.h), through the real processor ----------------
static int runControlProof (int argc, char** argv)
{
    namespace cp = svl::controlproof;
    using cp::feat::bandGainDb; using cp::feat::outRmsDb; using cp::feat::wetRmsDb; using cp::feat::wetBandDb;
    cp::Config c;
    c.name = "HAVA";
    c.make = [] { return std::make_unique<HavaProcessor>(); };
    c.tailSeconds = 1.0;                       // getTailLengthSeconds() = 1.0 (spring tank T60 0.63 s)
    c.applyFirst = { "mood" };                 // a MOOD write recalls air, sheen, masterSafe, twang, tension, drip, lowCut
    c.recallParams = { "mood" };
    c.latencyVariants = { { { "masterSafe", 1.0f } } };    // latency is 0 with MASTER SAFE off, 80 samples at 48 kHz with it on
    c.designedGainDb = [] (const cp::StateView& s) { return (double) juce::jmax (0.0f, s.raw ("output")) + 6.0; };   // OUTPUT up to +12 dB, AIR lifts the top by up to 6 dB
    c.notes["mood"] = "recall: writes air, sheen, masterSafe, twang, tension, drip, lowCut; swept on its own it is written last so the recall is the variation";
    cp::Cond live = [] (const cp::StateView& s) { return ! s.on ("bypass"); };
    auto D = [] (const char* id, int dir, cp::Feature f, double tol, cp::Cond ap, const char* fname, const char* note, std::vector<std::pair<juce::String, float>> force = {}, int mat = cp::kPink)
    { cp::Direction d; d.id = id; d.dir = dir; d.feature = f; d.tol = tol; d.applies = ap; d.featureName = fname; d.note = note; d.force = force; d.material = mat; d.force.push_back ({ "bypass", 0.0f }); if (juce::String (id) != "output") d.force.push_back ({ "output", 0.5f }); return d; };
    c.directions = {
        D ("air", +1, bandGainDb (8000.0, 16000.0), 0.1, live, "output minus input power 8-16 kHz (dB)", "AIR lifts the top end", { { "sheen", 0.0f } }),
        D ("sheen", +1, wetRmsDb(), 0.25, live, "wet part = output - aligned input (rms dB), AIR 0", "SHEEN is the spring return level", { { "air", 0.0f } }),
        D ("output", +1, outRmsDb(), 0.25, live, "output rms (dB)", "output gain"),
        D ("bypass", 0, nullptr, 0, nullptr, "", "switch"),
        D ("masterSafe", 0, nullptr, 0, nullptr, "", "switch: ceiling, latency and auto gain; no ordering"),
        D ("mood", 0, nullptr, 0, nullptr, "", "five recall slots"),
        D ("twang", 2, wetRmsDb(), 0, nullptr, "wet rms (dB), AIR 0, SHEEN 1", "character of the spring; no direction implied by the brief", { { "air", 0.0f }, { "sheen", 1.0f } }),
        D ("tension", 2, wetRmsDb(), 0, nullptr, "wet rms (dB), AIR 0, SHEEN 1", "character of the spring; no direction implied by the brief", { { "air", 0.0f }, { "sheen", 1.0f } }),
        D ("drip", 2, wetRmsDb(), 0, nullptr, "wet rms (dB), AIR 0, SHEEN 1", "character of the spring; no direction implied by the brief", { { "air", 0.0f }, { "sheen", 1.0f } }),
        D ("lowCut", -1, wetBandDb (20.0, 400.0), 0.25, live, "wet band 20-400 Hz power (dB), AIR 0, SHEEN 1", "LOW CUT filters the sheen send: less energy below the cutoff", { { "air", 0.0f }, { "sheen", 1.0f } }),
    };
    return cp::main (c, argc, argv);
}

int main (int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
        if (juce::String (argv[i]).startsWith ("--controlproof"))
        {
            juce::ScopedJuceInitialiser_GUI init;
            return runControlProof (argc, argv);
        }
    juce::ScopedJuceInitialiser_GUI init;
    const double sr = 48000.0;
    const auto src = material (sr, 2.0);
    const int N = src.getNumSamples();

    // 1. The state the plugin opens in is the engine's defaultControls() (AIR FULL), mood AIR FULL.
    {
        HavaProcessor p;
        const auto d = yay::defaultControls();
        const bool ok = juce::exactlyEqual (get (p, "air"), d.air) && juce::exactlyEqual (get (p, "sheen"), d.sheen)
                     && juce::exactlyEqual (get (p, "output"), d.output) && (get (p, "masterSafe") > 0.5f) == d.masterSafe
                     && juce::exactlyEqual (get (p, "twang"), d.twang) && juce::exactlyEqual (get (p, "tension"), d.tension)
                     && juce::exactlyEqual (get (p, "drip"), d.drip) && juce::exactlyEqual (get (p, "lowCut"), d.lowcut)
                     && (int) get (p, "mood") == 1 && get (p, "bypass") < 0.5f;
        report (ok, "defaults_are_air_full", "AIR " + juce::String (get (p, "air"), 2) + " SHEEN " + juce::String (get (p, "sheen"), 2)
                                                  + " mood " + HavaProcessor::moodNames[(int) get (p, "mood")]);
    }

    // 2. Plugin output = engine output (steady controls, both MASTER SAFE modes), bit for bit.
    for (int safe = 0; safe < 2; ++safe)
    {
        auto p = make (sr, [safe] (HavaProcessor& q) { set (q, "masterSafe", (float) safe); });
        auto viaPlugin = src;
        run (*p, viaPlugin);
        yay::YayEngine e;
        e.prepare (sr);
        auto c = yay::defaultControls();
        c.masterSafe = safe == 1;
        auto direct = src;
        for (int pos = 0; pos < N; pos += 512)
            e.process (direct.getWritePointer (0) + pos, direct.getWritePointer (1) + pos, juce::jmin (512, N - pos), c);
        report (identical (viaPlugin, direct, 0, N), safe ? "plugin_equals_engine_safe" : "plugin_equals_engine",
                "AIR FULL" + juce::String (safe ? " + MASTER SAFE" : "") + ", 2 s program, 512-sample blocks");
    }

    // 3. Latency: 0 off; engine latency(true) on, re-reported when MASTER SAFE toggles.
    for (double rate : { 48000.0, 44100.0 })
    {
        auto p = make (rate);
        yay::YayEngine e;
        e.prepare (rate);
        const int off0 = p->getLatencySamples();
        auto buf = material (rate, 0.2);
        set (*p, "masterSafe", 1.0f);
        run (*p, buf);
        const int on = p->getLatencySamples();
        set (*p, "masterSafe", 0.0f);
        run (*p, buf);
        const int off1 = p->getLatencySamples();
        const int want = rate > 46000.0 ? 80 : 74;
        report (off0 == 0 && on == want && on == e.latency (true) && off1 == 0, "latency_" + juce::String ((int) rate),
                "off " + juce::String (off0) + " -> on " + juce::String (on) + " (engine " + juce::String (e.latency (true))
                    + ", need " + juce::String (want) + ") -> off " + juce::String (off1));
    }

    // 4. BYPASS, MASTER SAFE off: bit-exact, both from the start and after the 20 ms fade has landed.
    {
        auto p = make (sr, [] (HavaProcessor& q) { set (q, "bypass", 1.0f); });
        auto out = src;
        run (*p, out);
        const bool a = identical (out, src, 0, N);
        auto q = make (sr);
        auto out2 = src;
        run (*q, out2, 512, 0, 24000);
        set (*q, "bypass", 1.0f);
        run (*q, out2, 512, 24000, N);
        const bool b = identical (out2, src, 24000 + 2048, N);
        report (a && b, "bypass_bit_exact", juce::String ("from start ") + (a ? "exact" : "DIFFERS") + ", after the fade " + (b ? "exact" : "DIFFERS"));
    }

    // 5. BYPASS, MASTER SAFE on: the input delayed by exactly the reported latency.
    {
        auto p = make (sr, [] (HavaProcessor& q) { set (q, "bypass", 1.0f); set (q, "masterSafe", 1.0f); });
        auto out = src;
        run (*p, out);
        const int lat = p->getLatencySamples();
        report (identical (out, src, lat, N, lat), "bypass_safe_delayed_exact", "output[n] == input[n - " + juce::String (lat) + "] bit for bit");
    }

    // 6. Neutral controls (AIR 0, SHEEN 0, OUTPUT 0, MASTER SAFE off): bit-exact, also after smoothing a knob down.
    {
        auto p = make (sr, [] (HavaProcessor& q) { set (q, "air", 0.0f); set (q, "sheen", 0.0f); });
        auto out = src;
        run (*p, out);
        const bool a = identical (out, src, 0, N);
        auto q = make (sr);
        auto out2 = src;
        run (*q, out2, 512, 0, 24000);
        set (*q, "air", 0.0f);
        set (*q, "sheen", 0.0f);
        run (*q, out2, 512, 24000, N);
        // once the 50 ms knob ramp has landed on 0 the engine returns the buffer untouched
        const bool b = identical (out2, src, 24000 + 4800, N);
        report (a && b, "neutral_bit_exact",
                juce::String ("AIR 0 SHEEN 0 OUTPUT 0: from start ") + (a ? "exact" : "DIFFERS") + ", after turning down " + (b ? "exact" : "DIFFERS"));
    }

    // 7. MASTER SAFE toggle: a 30 ms crossfade, no click (largest sample step during the switch vs steady state).
    {
        auto p = make (sr);
        const int n = (int) sr * 2;
        juce::AudioBuffer<float> buf (2, n);
        for (int i = 0; i < n; ++i)
            for (int c = 0; c < 2; ++c)
                buf.setSample (c, i, 0.25f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * i / sr));
        run (*p, buf, 512, 0, n / 2);
        set (*p, "masterSafe", 1.0f);
        run (*p, buf, 512, n / 2, n);
        auto maxStep = [&] (int from, int to)
        {
            float m = 0.0f;
            for (int c = 0; c < 2; ++c)
                for (int i = from + 1; i < to; ++i)
                    m = juce::jmax (m, std::abs (buf.getSample (c, i) - buf.getSample (c, i - 1)));
            return m;
        };
        const float steady = maxStep (n / 4, n / 2), sw = maxStep (n / 2, n / 2 + 4800);
        report (sw < 1.5f * steady, "safe_toggle_no_click",
                "max step " + juce::String (sw, 4) + " during the switch vs " + juce::String (steady, 4) + " steady (need < 1.5x)");
    }

    // 8. Mood recall: AIR, SHEEN, MASTER SAFE and the rear defaults; OUTPUT stays; the same key recalls again.
    {
        HavaProcessor p;
        set (p, "output", 3.0f);
        set (p, "twang", 0.2f);
        p.selectMood (3);   // SHEEN ONLY
        const auto c3 = preset ("SHEEN ONLY");
        bool ok = juce::exactlyEqual (get (p, "air"), c3.air) && juce::exactlyEqual (get (p, "sheen"), c3.sheen)
               && juce::approximatelyEqual (get (p, "twang"), 0.7f) && juce::approximatelyEqual (get (p, "output"), 3.0f);
        set (p, "tension", 0.9f);
        p.selectMood (3);   // again
        ok = ok && juce::approximatelyEqual (get (p, "tension"), 0.55f);
        set (p, "mood", 4.0f);   // as a host would automate it
        ok = ok && get (p, "masterSafe") > 0.5f && juce::approximatelyEqual (get (p, "air"), 0.8f) && juce::approximatelyEqual (get (p, "sheen"), 0.0f)
                 && juce::approximatelyEqual (get (p, "drip"), 0.0f) && std::abs (get (p, "lowCut") - 300.0f) < 0.01f;   // 2026-10-04 moods
        report (ok, "mood_recall", "SHEEN ONLY -> AIR " + juce::String (c3.air, 2) + " SHEEN " + juce::String (c3.sheen, 2)
                                       + ", rear defaults back, OUTPUT kept at +3 dB; MASTER AIR turns MASTER SAFE on");
    }

    // 9. State round trip: saved values come back as saved, not overwritten by the saved mood's preset.
    {
        HavaProcessor a;
        a.selectMood (2);   // AIR + SHEEN
        set (a, "air", 0.33f);
        set (a, "lowCut", 150.0f);
        set (a, "bypass", 1.0f);
        juce::MemoryBlock mb;
        a.getStateInformation (mb);
        HavaProcessor b;
        b.setStateInformation (mb.getData(), (int) mb.getSize());
        bool ok = true;
        for (auto* id : { "air", "sheen", "output", "bypass", "masterSafe", "mood", "twang", "tension", "drip", "lowCut" })
            ok = ok && juce::approximatelyEqual (get (a, id), get (b, id));
        report (ok && juce::approximatelyEqual (get (b, "air"), 0.33f), "state_round_trip",
                "mood " + HavaProcessor::moodNames[(int) get (b, "mood")] + ", AIR " + juce::String (get (b, "air"), 2)
                    + " (edited after the mood), LOW CUT " + juce::String (get (b, "lowCut"), 0) + " Hz");
    }

    // 10. Knob smoothing: a jump of OUTPUT by 12 dB ramps (no step bigger than the program's own).
    {
        auto p = make (sr);
        const int n = (int) sr;
        juce::AudioBuffer<float> buf (2, n);
        for (int i = 0; i < n; ++i)
            for (int c = 0; c < 2; ++c)
                buf.setSample (c, i, 0.1f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * i / sr));
        run (*p, buf, 512, 0, n / 2);
        set (*p, "output", 12.0f);
        run (*p, buf, 512, n / 2, n);
        float before = 0.0f, after = 0.0f, worst = 0.0f;
        for (int i = n / 2 - 2400; i < n / 2; ++i) before = juce::jmax (before, std::abs (buf.getSample (0, i)));
        for (int i = n - 2400; i < n; ++i) after = juce::jmax (after, std::abs (buf.getSample (0, i)));
        for (int i = n / 2 + 1; i < n / 2 + 4800; ++i) worst = juce::jmax (worst, std::abs (buf.getSample (0, i) - buf.getSample (0, i - 1)));
        const float natural = 2.0f * juce::MathConstants<float>::pi * 220.0f / (float) sr * after;
        report (worst < 1.2f * natural && after > 3.5f * before, "output_smoothed",
                "peak " + juce::String (before, 3) + " -> " + juce::String (after, 3) + ", max step " + juce::String (worst, 4)
                    + " vs sine slope " + juce::String (natural, 4));
    }

    // 13. Cmd+Z / Cmd+Shift+Z: editor edits are steps, a MOOD key press is one step, host automation is none (SvlUndoTests.h).
    {
        const auto u = svl::undotest::run<HavaProcessor> ("HAVA", [] { return std::make_unique<HavaProcessor>(); },
                                                          { { "air", 0.21f }, { "sheen", 0.64f }, { "output", 4.0f } },
                                                          [] (HavaProcessor& p) { p.selectMood (3); });
        checks += u.pass + u.fail;
        failures += u.fail;
        std::printf ("%s undo_redo  %d of %d undo checks passed\n", u.fail == 0 ? "PASS" : "FAIL", u.pass, u.pass + u.fail);
    }

    std::printf ("%s (%d checks)\n", failures == 0 ? "ALL PASS" : "FAILED", checks);
    return failures == 0 ? 0 : 1;
}
