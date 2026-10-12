/*  Drives the real plugin processor, so the tests go through the same code
    path a DAW does.

        render --selftest
            Latency, generation, auto gain, bypass and determinism checks, and the TRACK switch.
            Exit code 0 only if every check passes.

        render <in.wav> <out.wav> [generation 0-4] [tape 0-2] [mix]
*/
#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"
#include "SvlUndoTests.h"
#include "SvlParamTextTests.h"
#include "SvlControlProof.h"

namespace
{
void set (KasetProcessor& p, const char* id, float value)
{
    auto* param = p.apvts.getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (value));
}

using Setup = std::function<void (KasetProcessor&)>;

juce::AudioBuffer<float> run (const juce::AudioBuffer<float>& input, double sr, Setup setup, int block = 512)
{
    KasetProcessor proc;
#if SVL_LICENSING
    proc.license.setLicensedForTesting (true);
#endif
    setup (proc);
    proc.setRateAndBufferSizeDetails (sr, block);
    proc.prepareToPlay (sr, block);
    juce::AudioBuffer<float> buf (input);
    juce::MidiBuffer midi;
    for (int pos = 0; pos < buf.getNumSamples(); pos += block)
    {
        const int n = juce::jmin (block, buf.getNumSamples() - pos);
        juce::AudioBuffer<float> view (buf.getArrayOfWritePointers(), buf.getNumChannels(), pos, n);
        proc.processBlock (view, midi);
    }
    return buf;
}

juce::AudioBuffer<float> noise (int samples, float amp)
{
    juce::AudioBuffer<float> b (2, samples);
    uint32_t s = 12345u;
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < samples; ++i)
        {
            s = s * 1664525u + 1013904223u;
            b.setSample (c, i, ((float) (s >> 8) / 16777216.0f - 0.5f) * 2.0f * amp);
        }
    return b;
}

double rmsDb (const juce::AudioBuffer<float>& b, int from, int to)
{
    double e = 0.0;
    for (int c = 0; c < 2; ++c)
        for (int i = from; i < to; ++i)
            e += (double) b.getSample (c, i) * b.getSample (c, i);
    return 10.0 * std::log10 (e / (2.0 * (to - from)) + 1.0e-30);
}

/** Where a click comes out: the index of the largest sample. */
int peakAt (const juce::AudioBuffer<float>& b)
{
    int best = 0;
    for (int i = 0; i < b.getNumSamples(); ++i)
        if (std::abs (b.getSample (0, i)) > std::abs (b.getSample (0, best))) best = i;
    return best;
}

/** run() with the TRACK switch flipped at block boundaries: events are { sample position, value }, applied at the first block that starts at or after it. */
juce::AudioBuffer<float> runSwitching (const juce::AudioBuffer<float>& input, double sr, Setup setup,
                                       std::vector<std::pair<int, float>> events, int block = 512)
{
    KasetProcessor proc;
#if SVL_LICENSING
    proc.license.setLicensedForTesting (true);
#endif
    setup (proc);
    proc.setRateAndBufferSizeDetails (sr, block);
    proc.prepareToPlay (sr, block);
    juce::AudioBuffer<float> buf (input);
    juce::MidiBuffer midi;
    size_t next = 0;
    for (int pos = 0; pos < buf.getNumSamples(); pos += block)
    {
        while (next < events.size() && pos >= events[next].first)
            set (proc, "track", events[next++].second);
        const int n = juce::jmin (block, buf.getNumSamples() - pos);
        juce::AudioBuffer<float> view (buf.getArrayOfWritePointers(), buf.getNumChannels(), pos, n);
        proc.processBlock (view, midi);
    }
    return buf;
}

/** Largest sample-to-sample step of channel 0 in [from, to). */
float maxStep (const juce::AudioBuffer<float>& b, int from, int to)
{
    float m = 0.0f;
    for (int i = juce::jmax (1, from); i < to; ++i)
        m = juce::jmax (m, std::abs (b.getSample (0, i) - b.getSample (0, i - 1)));
    return m;
}

juce::AudioBuffer<float> sine (int samples, double sr, float hz, float amp)
{
    juce::AudioBuffer<float> b (2, samples);
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < samples; ++i)
            b.setSample (c, i, amp * (float) std::sin (2.0 * juce::MathConstants<double>::pi * hz * (double) i / sr));
    return b;
}

bool sameBits (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    if (a.getNumSamples() != b.getNumSamples()) return false;
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < a.getNumSamples(); ++i)
            if (std::memcmp (a.getReadPointer (c) + i, b.getReadPointer (c) + i, sizeof (float)) != 0) return false;
    return true;
}

int failures = 0;
void check (bool ok, const juce::String& what)
{
    std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8());
    if (! ok) ++failures;
}

int selftest()
{
    const double sr = 48000.0;
    KasetProcessor probe;
    probe.prepareToPlay (sr, 512);
    const int lat = probe.getLatencySamples();

    // 1. The same latency for every generation, and it is where the audio really is.
    for (int gen = 0; gen < 5; ++gen)
    {
        juce::AudioBuffer<float> click (2, (int) sr);
        click.clear();
        click.setSample (0, 1000, 0.8f);
        click.setSample (1, 1000, 0.8f);
        auto out = run (click, sr, [gen] (KasetProcessor& p) {
            set (p, "generation", (float) gen); set (p, "wow", 0.0f); set (p, "flutter", 0.0f); set (p, "hiss", 0.0f); set (p, "wear", 0.0f); set (p, "autoGain", 0.0f); });
        const int off = peakAt (out) - 1000 - lat;
        check (std::abs (off) <= 6, "Generation " + juce::String (gen + 1) + ": click lands " + juce::String (off) + " samples from the reported latency (" + juce::String (lat) + ")");
    }

    // 2. Auto gain keeps the level within 1.5 dB of the input, every generation and tape.
    for (int gen : { 0, 2, 4 })
        for (int tape = 0; tape < 3; ++tape)
        {
            auto in = noise ((int) (8.0 * sr), 0.15f);
            auto out = run (in, sr, [gen, tape] (KasetProcessor& p) { set (p, "generation", (float) gen); set (p, "tape", (float) tape); });
            const double d = rmsDb (out, (int) (5.0 * sr), (int) (8.0 * sr)) - rmsDb (in, (int) (5.0 * sr), (int) (8.0 * sr));
            check (std::abs (d) < 1.5, "Auto gain, gen " + juce::String (gen + 1) + " tape " + KasetProcessor::tapeNames[tape] + ": " + juce::String (d, 2) + " dB");
        }

    // Every control changes the sound when the tape is in, and nothing does in BYPASS.
    {
        auto in = noise ((int) (3.0 * sr), 0.15f);
        auto base = run (in, sr, [] (KasetProcessor&) {});
        auto diffDb = [&] (const juce::AudioBuffer<float>& a)
        {
            juce::AudioBuffer<float> d (a);
            for (int c = 0; c < 2; ++c) d.addFrom (c, 0, base, c, 0, base.getNumSamples(), -1.0f);
            return rmsDb (d, (int) sr, 3 * (int) sr) - rmsDb (base, (int) sr, 3 * (int) sr);
        };
        const std::pair<const char*, float> moves[] { { "input", 12.0f }, { "drive", 1.0f }, { "wow", 1.0f }, { "flutter", 1.0f }, { "hiss", 1.0f },
                                                      { "wear", 1.0f }, { "mix", 0.3f }, { "output", 6.0f }, { "generation", 0.0f }, { "tape", 2.0f } };
        for (auto [id, v] : moves)
        {
            auto on = run (in, sr, [id = id, v = v] (KasetProcessor& p) { set (p, id, v); });
            auto off = run (in, sr, [id = id, v = v] (KasetProcessor& p) { set (p, "play", 0.0f); set (p, id, v); });
            auto offBase = run (in, sr, [] (KasetProcessor& p) { set (p, "play", 0.0f); });
            float offWorst = 0.0f;
            for (int i = 0; i < off.getNumSamples(); ++i)
                offWorst = juce::jmax (offWorst, std::abs (off.getSample (0, i) - offBase.getSample (0, i)));
            const double d = diffDb (on);
            check (d > -45.0 && offWorst < 1.0e-6f, juce::String (id).toUpperCase() + ": changes the sound by " + juce::String (d, 1)
                   + " dB against the default; in BYPASS the change is " + juce::String (offWorst, 7));
        }
    }

    // 3. More generations, less top: energy above 6 kHz falls step by step.
    {
        double last = 1.0e9;
        bool falling = true;
        juce::String list;
        for (int gen = 0; gen < 5; ++gen)
        {
            auto in = noise ((int) (4.0 * sr), 0.1f);
            auto out = run (in, sr, [gen] (KasetProcessor& p) { set (p, "generation", (float) gen); set (p, "hiss", 0.0f); set (p, "autoGain", 0.0f); });
            juce::dsp::IIR::Filter<float> hp;
            hp.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighPass (sr, 6000.0);
            double e = 0.0;
            for (int i = 0; i < out.getNumSamples(); ++i)
            {
                const float y = hp.processSample (out.getSample (0, i));
                if (i > (int) sr) e += (double) y * y;
            }
            const double db = 10.0 * std::log10 (e + 1.0e-30);
            falling &= db < last;
            last = db;
            list << juce::String (db, 1) << " ";
        }
        check (falling, "Top end above 6 kHz falls with every generation: " + list);
    }

    // 4. BYPASS is a true bypass: the dry signal, delayed by the latency, bit-exact after the fade.
    {
        auto in = noise ((int) sr, 0.2f);
        auto out = run (in, sr, [] (KasetProcessor& p) { set (p, "play", 0.0f); });
        float worst = 0.0f;
        for (int i = lat + 2000; i < out.getNumSamples(); ++i)
            worst = juce::jmax (worst, std::abs (out.getSample (0, i) - in.getSample (0, i - lat)));
        check (worst < 1.0e-6f, "BYPASS passes the dry signal through, largest difference " + juce::String (worst, 8));
    }

    // 5. Determinism: identical bits across runs and block sizes.
    {
        auto in = noise ((int) (2.0 * sr), 0.2f);
        auto setup = [] (KasetProcessor& p) { set (p, "generation", 4.0f); set (p, "wear", 1.0f); };
        auto a = run (in, sr, setup, 512), b = run (in, sr, setup, 97), c = run (in, sr, setup, 512);
        bool same = true, finite = true;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < a.getNumSamples(); ++i)
            {
                same &= a.getSample (ch, i) == b.getSample (ch, i) && a.getSample (ch, i) == c.getSample (ch, i);
                finite &= std::isfinite (a.getSample (ch, i));
            }
        check (same && finite, "4th dub, full wear: identical across runs and block sizes 512 / 97, all finite");
    }


    // ---- TRACK (low-latency) switch ----------------------------------------------------------------------------------------------
    // Written for the November family update; see STATUS.md "PENDING BUILD". Latency numbers are the spec's: 66 / 72 samples with TRACK on and
    // 660 / 720 off at 44.1 / 48 kHz.
    {
        // 1. Reported latency per mode, set before prepare, and updated when the switch flips afterwards.
        for (auto [rate, onExpect, offExpect] : std::initializer_list<std::tuple<double, int, int>> { { 44100.0, 66, 660 }, { 48000.0, 72, 720 } })
        {
            auto latOf = [rate = rate] (bool trackOn, bool flipAfterPrepare)
            {
                KasetProcessor p;
                if (! flipAfterPrepare) set (p, "track", trackOn ? 1.0f : 0.0f);
                p.setRateAndBufferSizeDetails (rate, 512);
                p.prepareToPlay (rate, 512);
                if (flipAfterPrepare) set (p, "track", trackOn ? 1.0f : 0.0f);
                return p.getLatencySamples();
            };
            check (latOf (true, false) == onExpect && latOf (false, false) == offExpect,
                   "TRACK latency at " + juce::String (rate, 0) + " Hz: on " + juce::String (latOf (true, false)) + " (want " + juce::String (onExpect) + "), off "
                   + juce::String (latOf (false, false)) + " (want " + juce::String (offExpect) + ")");
            check (latOf (true, true) == onExpect && latOf (false, true) == offExpect,
                   "TRACK latency follows the switch after prepareToPlay at " + juce::String (rate, 0) + " Hz: " + juce::String (latOf (true, true)) + " / "
                   + juce::String (latOf (false, true)));
        }

        // 2. The click really lands at the reported latency in TRACK (wet path, MIX 100 %), at three rates.
        for (double rate : { 44100.0, 48000.0, 96000.0 })
        {
            juce::AudioBuffer<float> click (2, (int) rate);
            click.clear();
            click.setSample (0, 1000, 0.8f);
            click.setSample (1, 1000, 0.8f);
            auto out = run (click, rate, [] (KasetProcessor& p) {
                set (p, "track", 1.0f); set (p, "generation", 4.0f); set (p, "wow", 0.0f); set (p, "flutter", 0.0f); set (p, "hiss", 0.0f); set (p, "wear", 0.0f); set (p, "autoGain", 0.0f); });
            const int expect = kaset::KasetEngine::latencyFor (rate, true);
            const int off = peakAt (out) - 1000 - expect;
            // The head itself sits at the reported centre (1st dub below: 2 / 2 / 5 samples = one trip's filter delay). What is left at the 4th dub is the group delay
            // of the five trips' gap filters (two low-passes per trip), which is a property of the tone, not of the head, and is the same in MIX.
            // Measured on the biquads at 1 kHz (5 trips): 146 / 159 / 208 us at 44.1 / 48 / 96 kHz = 6.4 / 7.6 / 20 samples (the second pole's corner is
            // 1.7 x the first and is clamped at 0.47 x sr at 44.1 / 48 kHz, so it is less delayed there). So the tolerance is a TIME: 0.25 ms.
            const int tol = (int) std::ceil (0.00025 * rate);
            check (std::abs (off) <= tol, "TRACK, 4th dub, " + juce::String (rate, 0) + " Hz: the click lands " + juce::String (off) + " samples from the reported " + juce::String (expect)
                   + " (limit " + juce::String (tol) + " = 0.25 ms)");
        }

        // 2b. The head itself: with one trip (1st dub) the click lands within 0.08 ms of the reported latency at every rate (one trip's own gap-filter delay: 2 / 2 / 5 samples = 45 / 42 / 52 us).
        for (double rate : { 44100.0, 48000.0, 96000.0 })
        {
            juce::AudioBuffer<float> click (2, (int) rate);
            click.clear();
            click.setSample (0, 1000, 0.8f);
            click.setSample (1, 1000, 0.8f);
            auto out = run (click, rate, [] (KasetProcessor& p) {
                set (p, "track", 1.0f); set (p, "generation", 0.0f); set (p, "wow", 0.0f); set (p, "flutter", 0.0f); set (p, "hiss", 0.0f); set (p, "wear", 0.0f); set (p, "autoGain", 0.0f); });
            const int expect = kaset::KasetEngine::latencyFor (rate, true);
            const int off = peakAt (out) - 1000 - expect;
            check (std::abs (off) <= (int) std::ceil (0.00008 * rate), "TRACK, 1st dub, " + juce::String (rate, 0) + " Hz: the head sits where it is reported (click " + juce::String (off) + " samples from " + juce::String (expect) + ")");
        }

        // 3. TRACK stays finite and bounded at WOW max, GEN max, FLUTTER max, every other control at its worst, at 44.1 / 48 / 96 kHz.
        for (double rate : { 44100.0, 48000.0, 96000.0 })
        {
            auto in = noise ((int) (4.0 * rate), 0.5f);
            auto out = run (in, rate, [] (KasetProcessor& p) {
                set (p, "track", 1.0f); set (p, "generation", 4.0f); set (p, "tape", 2.0f); set (p, "wow", 1.0f); set (p, "flutter", 1.0f); set (p, "drive", 1.0f);
                set (p, "hiss", 1.0f); set (p, "wear", 1.0f); set (p, "input", 12.0f); set (p, "output", 12.0f); });
            bool finite = true;
            float peak = 0.0f;
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < out.getNumSamples(); ++i)
                {
                    finite &= std::isfinite (out.getSample (c, i));
                    peak = juce::jmax (peak, std::abs (out.getSample (c, i)));
                }
            check (finite && peak < 50.0f, "TRACK finite at WOW / FLUTTER / GEN / everything max, " + juce::String (rate, 0) + " Hz (peak " + juce::String (peak, 2) + ")");
        }

        // 4. Head bounds: the single head never leaves [3, 2 * centre - 3] (the cubic reader needs >= 2), and the WOW cap alone keeps it there:
        //    the hard clamp must not have had to act. Engine level, 20 s of noise per case; the WOW caps are printed for STATUS.md.
        for (double rate : { 44100.0, 48000.0, 96000.0 })
        {
            bool inside = true, noClamp = true, finite = true;
            float lowest = 1.0e9f, highest = -1.0e9f;
            juce::String caps;
            for (int gen = 1; gen <= 5; ++gen)
                for (float wow : { 0.5f, 1.0f })
                {
                    kaset::KasetEngine eng;
                    eng.prepare (rate);
                    eng.setTrack (true);
                    kaset::Controls c;
                    c.generation = gen; c.wow = wow; c.flutter = 1.0f; c.wear = 1.0f; c.hiss = 1.0f;
                    if (wow == 1.0f) caps << "G" << gen << " " << juce::String (eng.trackWowCap (gen, 1.0f), 3) << "  ";
                    auto in = noise ((int) (20.0 * rate), 0.4f);
                    for (int pos = 0; pos < in.getNumSamples(); pos += 512)
                    {
                        const int n = juce::jmin (512, in.getNumSamples() - pos);
                        eng.process (in.getWritePointer (0) + pos, in.getWritePointer (1) + pos, n, c);
                    }
                    for (int i = 0; i < in.getNumSamples(); ++i) finite &= std::isfinite (in.getSample (0, i)) && std::isfinite (in.getSample (1, i));
                    inside &= eng.trackHeadMin() >= eng.trackHeadLowLimit() && eng.trackHeadMax() <= eng.trackHeadHighLimit();
                    noClamp &= eng.trackClampHits() == 0;
                    lowest = juce::jmin (lowest, eng.trackHeadMin());
                    highest = juce::jmax (highest, eng.trackHeadMax());
                }
            check (inside && noClamp && finite && lowest >= 2.0f,
                   "TRACK head bounds at " + juce::String (rate, 0) + " Hz, 20 s x 10 cases: head " + juce::String (lowest, 2) + " .. " + juce::String (highest, 2)
                   + " samples, no clamp hit; WOW cap at FLUTTER max: " + caps);
        }

        // 5. TRACK off is MIX: a switch that went on and off again before prepare leaves nothing behind (bit-identical to a plain render).
        {
            auto in = noise ((int) (2.0 * sr), 0.3f);
            auto a = run (in, sr, [] (KasetProcessor& p) { set (p, "generation", 3.0f); set (p, "wear", 0.6f); });
            auto b = run (in, sr, [] (KasetProcessor& p) { set (p, "track", 1.0f); set (p, "track", 0.0f); set (p, "generation", 3.0f); set (p, "wear", 0.6f); });
            check (sameBits (a, b), "TRACK flipped on and off before prepare: the MIX render is bit-identical to one that never touched it");
        }

        // 6. Switching in the middle of a signal does not click: the largest step stays within 2x the signal's own, ON and back OFF, wet and half-dry.
        for (float mixAmt : { 1.0f, 0.5f })
        {
            auto in = sine ((int) (3.0 * sr), sr, 440.0f, 0.4f);
            auto setup = [mixAmt] (KasetProcessor& p) {
                set (p, "wow", 0.0f); set (p, "flutter", 0.0f); set (p, "hiss", 0.0f); set (p, "wear", 0.0f); set (p, "autoGain", 0.0f); set (p, "mix", mixAmt); };
            auto mixOnly = run (in, sr, setup);
            auto trackOnly = run (in, sr, [setup] (KasetProcessor& p) { setup (p); set (p, "track", 1.0f); });
            const int from = (int) (0.5 * sr), to = (int) (3.0 * sr);
            const float own = juce::jmax (maxStep (mixOnly, from, to), maxStep (trackOnly, from, to));
            auto sw = runSwitching (in, sr, setup, { { (int) (1.0 * sr), 1.0f }, { (int) (2.0 * sr), 0.0f } });
            const float step = maxStep (sw, from, to);
            bool finite = true;
            for (int i = 0; i < sw.getNumSamples(); ++i) finite &= std::isfinite (sw.getSample (0, i));
            check (finite && step <= 2.0f * own, "TRACK switched on at 1 s and off at 2 s in a 440 Hz sine, MIX " + juce::String (mixAmt * 100.0f, 0) + " %: largest step "
                   + juce::String (step, 5) + " against the signal's own " + juce::String (own, 5) + " (limit 2x)");
        }

        // 7. The fade is a short dip, not a hole: the signal is back at full level 100 ms after the switch.
        {
            auto in = sine ((int) (3.0 * sr), sr, 440.0f, 0.4f);
            auto setup = [] (KasetProcessor& p) { set (p, "wow", 0.0f); set (p, "flutter", 0.0f); set (p, "hiss", 0.0f); set (p, "wear", 0.0f); set (p, "autoGain", 0.0f); };
            auto steady = run (in, sr, [setup] (KasetProcessor& p) { setup (p); set (p, "track", 1.0f); });
            auto sw = runSwitching (in, sr, setup, { { (int) (1.0 * sr), 1.0f } });
            const int a = (int) (1.1 * sr), b = (int) (1.5 * sr);
            check (std::abs (rmsDb (sw, a, b) - rmsDb (steady, a, b)) < 0.5,
                   "TRACK switch: 100 ms after the switch the level is within 0.5 dB of a render that started in TRACK (" + juce::String (rmsDb (sw, a, b) - rmsDb (steady, a, b), 3) + " dB)");
        }

        // 8. Saved with the session, not automatable, undoable.
        {
            KasetProcessor a;
            check (a.apvts.getParameter ("track") != nullptr && ! a.apvts.getParameter ("track")->isAutomatable(), "TRACK is a parameter and is not automatable");
            set (a, "track", 1.0f);
            juce::MemoryBlock data;
            a.getStateInformation (data);
            KasetProcessor b;
            b.setStateInformation (data.getData(), (int) data.getSize());
            b.setRateAndBufferSizeDetails (44100.0, 512);
            b.prepareToPlay (44100.0, 512);
            check (b.apvts.getRawParameterValue ("track")->load() > 0.5f && b.getLatencySamples() == 66, "TRACK is saved in the session state and comes back on, with latency 66 at 44.1 kHz");

            KasetProcessor u;
            svl::setParamAsGesture (u.apvts.getParameter ("track"), 1.0f);
            const bool on = u.apvts.getRawParameterValue ("track")->load() > 0.5f;
            const bool undone = u.undo.undo();
            const bool off = u.apvts.getRawParameterValue ("track")->load() < 0.5f;
            const bool redone = u.undo.redo();
            const bool backOn = u.apvts.getRawParameterValue ("track")->load() > 0.5f;
            check (on && undone && off && redone && backOn, "TRACK: one click = one undo step; undo turns it off, redo turns it on");
        }
    }


#if SVL_LICENSING
    // Licensing: the answers the licence server can give, parsed without a network.
    {
        svl::LicenseConfig cfg;
        cfg.product = "TEST";
        cfg.storeId = 111;
        cfg.productIds = { 222, 333 };
        const juce::String good = R"({"activated":true,"error":null,"license_key":{"status":"active"},"instance":{"id":"inst-1"},"meta":{"store_id":111,"product_id":333}})";
        const juce::String other = R"({"activated":true,"error":null,"license_key":{"status":"active"},"instance":{"id":"inst-2"},"meta":{"store_id":111,"product_id":999}})";
        const juce::String limit = R"({"activated":false,"error":"This license key has reached the activation limit.","license_key":{"status":"active"},"meta":{}})";
        const auto a = svl::License::parseActivate (good, cfg);
        const auto b = svl::License::parseActivate (other, cfg);
        const auto c = svl::License::parseActivate (limit, cfg);
        const auto d = svl::License::parseActivate ("", cfg);
        check (a.ok && a.instanceId == "inst-1", "Licence: a key for our product (or the bundle) activates");
        check (! b.ok && b.message.contains ("different product"), "Licence: a key for another product is refused");
        check (! c.ok && c.message.contains ("activation limit"), "Licence: the server's own error is shown (activation limit)");
        check (! d.ok && ! d.definitelyRevoked, "Licence: no network is not a refusal");
        const auto v1 = svl::License::parseValidate (R"({"valid":false,"error":"license_key not found","license_key":{"status":"disabled"},"meta":{}})", cfg);
        const auto v2 = svl::License::parseValidate ("", cfg);
        check (v1.definitelyRevoked && ! v2.definitelyRevoked, "Licence: a disabled key is revoked on re-check, an offline re-check keeps the licence");
    }

    // Launch keys: SVL- keys go to our own server and must be for KASET or the bundle; other keys stay on Lemon Squeezy.
    {
        using B = svl::License::Backend;
        const auto cfg = kasetLicenseConfig();
        check (svl::License::apiBaseFor ("SVL-AB12-CD34-EF56-GH78", cfg) == "https://subverselab.com/api/license/"
                   && svl::License::apiBaseFor ("  svl-ab12-cd34-ef56-gh78 ", cfg) == "https://subverselab.com/api/license/"
                   && svl::License::apiBaseFor ("38b1460a-5104-4067-a91d-77b872934d51", cfg) == "https://api.lemonsqueezy.com/v1/licenses/",
               "Licence: SVL- keys (any case) go to subverselab.com, other keys to Lemon Squeezy");
        auto svlAnswer = [] (int store, int product)
        {
            return R"({"activated":true,"error":null,"license_key":{"status":"active"},"instance":{"id":"svl-inst"},"meta":{"store_id":)"
                   + juce::String (store) + R"(,"product_id":)" + juce::String (product) + "}}";
        };
        const auto bundle = svl::License::parseActivate (svlAnswer (1, 9003), cfg, B::svl);
        const auto own = svl::License::parseActivate (svlAnswer (1, 9002), cfg, B::svl);
        const auto sibling = svl::License::parseActivate (svlAnswer (1, 9001), cfg, B::svl);
        const auto foreign = svl::License::parseActivate (svlAnswer (2, 9003), cfg, B::svl);
        check (bundle.ok && bundle.instanceId == "svl-inst" && own.ok, "Licence: an SVL key for KASET or the launch bundle activates");
        check (! sibling.ok && sibling.definitelyRevoked && sibling.message.contains ("different product"),
               "Licence: an SVL key for KUBBE alone (9001) is refused by KASET");
        check (! foreign.ok, "Licence: an SVL answer from another store is refused");
        const auto used = svl::License::parseActivate (R"({"activated":false,"error":"This license key has reached the activation limit.","meta":{}})", cfg, B::svl);
        const auto gone = svl::License::parseValidate (R"({"valid":false,"error":"license_key not found","meta":{}})", cfg, B::svl);
        const auto fine = svl::License::parseValidate (R"({"valid":true,"error":null,"license_key":{"status":"active"},"meta":{"store_id":1,"product_id":9003}})", cfg, B::svl);
        const auto offline = svl::License::parseValidate ("", cfg, B::svl);
        check (! used.ok && used.message.contains ("activation limit") && gone.definitelyRevoked && fine.ok && ! offline.definitelyRevoked,
               "Licence: our server's refusals, re-checks and silence are read like Lemon Squeezy's");
        // A Lemon Squeezy key is never judged by the SVL rule, and our store id means nothing to it.
        // A config with unset store / product ids (as before the store existed) must refuse every Lemon Squeezy key.
        auto unset = cfg;
        unset.storeId = 0;
        unset.productIds = {};
        const auto lsPlaceholder = svl::License::parseActivate (svlAnswer (555, 12345), unset, B::lemonSqueezy);
        const auto lsPlaceholderVal = svl::License::parseValidate (R"({"valid":true,"error":null,"license_key":{"status":"active"},"meta":{"store_id":555,"product_id":12345}})", unset, B::lemonSqueezy);
        auto lsCfg = cfg;
        lsCfg.storeId = 111;
        lsCfg.productIds = { 222 };
        const auto lsOurs = svl::License::parseActivate (svlAnswer (111, 222), lsCfg);
        const auto lsWrongStore = svl::License::parseActivate (svlAnswer (112, 222), lsCfg);
        const auto lsWrongProduct = svl::License::parseActivate (svlAnswer (111, 223), lsCfg);
        const auto lsSvlIds = svl::License::parseActivate (svlAnswer (1, 9003), lsCfg);
        auto storeOnly = unset;
        storeOnly.storeId = 111;   // store set, products still empty
        const auto lsStoreOnly = svl::License::parseActivate (svlAnswer (111, 222), storeOnly);
        check (! lsPlaceholder.ok && ! lsPlaceholderVal.ok && ! lsStoreOnly.ok,
               "Licence: fails closed - with unset Lemon Squeezy ids every Lemon Squeezy key is refused (an empty id is not 'accept all')");
        // The shipped config carries the real store and the product plus the bundle.
        check (cfg.storeId == 486764 && cfg.productIds.size() == 2 && cfg.productIds.contains (1402289),
               "Licence: the shipped config names the SubverseLab store, this product and the KUBBE + KASET bundle");
        check (lsOurs.ok && ! lsWrongStore.ok && ! lsWrongProduct.ok && ! lsSvlIds.ok, "Licence: Lemon Squeezy keys keep the store / product rule once the ids are set");
        // The licence file remembers its server; files from before SVL keys are Lemon Squeezy.
        const auto back = svl::License::decodeStored (svl::License::encodeStored ({ "SVL-AB12-CD34-EF56-GH78", "svl-inst", 1234, B::svl }));
        const auto ls = svl::License::decodeStored (svl::License::encodeStored ({ "38b1460a", "ls-inst", 99, B::lemonSqueezy }));
        const auto old = svl::License::decodeStored (R"({"key":"38b1460a","instance":"ls-inst","validated":99})");
        check (back.backend == B::svl && back.key == "SVL-AB12-CD34-EF56-GH78" && back.instanceId == "svl-inst" && back.validated == 1234
                   && ls.backend == B::lemonSqueezy && old.backend == B::lemonSqueezy && old.instanceId == "ls-inst",
               "Licence: the stored server survives a save / load; an old file without it is Lemon Squeezy");
    }

    // Without a licence the audio passes through bit for bit (latency-aligned, like BYPASS); with one, it is processed.
    {
        KasetProcessor probeLatency;
        probeLatency.prepareToPlay (sr, 512);
        const int latency = probeLatency.getLatencySamples();
        auto in = noise ((int) sr, 0.2f);
        auto out = run (in, sr, [] (KasetProcessor& p) { p.license.setLicensedForTesting (false); });
        int differing = 0;
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < out.getNumSamples(); ++i)
            {
                const float expected = i >= latency ? in.getSample (c, i - latency) : 0.0f;
                differing += out.getSample (c, i) != expected ? 1 : 0;
            }
        check (differing == 0, "No licence: the audio passes through bit for bit (dry == in, delayed by the " + juce::String (latency) + " sample latency), "
                                   + juce::String (differing) + " samples differ");

        auto lic = run (in, sr, [] (KasetProcessor& p) { p.license.setLicensedForTesting (true); set (p, "generation", 2.0f); set (p, "mix", 1.0f); });
        float diff = 0.0f;
        for (int i = latency + 2000; i < lic.getNumSamples(); ++i)
            diff = juce::jmax (diff, std::abs (lic.getSample (0, i) - in.getSample (0, i - latency)));
        check (diff > 1.0e-4f, "Licensed: the same instance processes the audio, difference " + juce::String (diff, 6));
    }

    // The licence file: written when a key is activated, read back on the next start, and never lost to a network error.
    {
        using B = svl::License::Backend;
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("svl-selftest-" + juce::String (juce::Time::currentTimeMillis()));
        dir.createDirectory();
        auto cfg = kasetLicenseConfig();
        cfg.licenseFolder = dir;                 // never the user's real Licenses folder
        const auto file = dir.getChildFile ("kaset.json");
        const juce::String key = "SVL-AB12-CD34-EF56-GH78";

        bool emptyDir = false, wrote = false, readBack = false;
        { svl::License l (cfg); emptyDir = ! l.isLicensed(); }
        { svl::License l (cfg); l.adoptActivation (key, "inst-1", B::svl); wrote = l.isLicensed() && file.existsAsFile(); }
        {
            svl::License l (cfg);
            const auto stored = svl::License::decodeStored (file.loadFileAsString());
            readBack = l.isLicensed() && l.keyHint() == "GH78" && stored.key == key && stored.instanceId == "inst-1"
                       && stored.backend == B::svl && stored.validated > 0;
        }
        check (emptyDir && wrote && readBack, "Licence file: none = unlicensed; an activation is written; the next start reads it back, licensed, offline");

        file.replaceWithText ("not json");
        bool junk = false, noInstance = false;
        { svl::License l (cfg); junk = ! l.isLicensed(); }
        file.replaceWithText (svl::License::encodeStored ({ key, "", 5, B::svl }));
        { svl::License l (cfg); noInstance = ! l.isLicensed(); }
        check (junk && noInstance, "Licence file: an unreadable file, or one without an instance id, is not a licence");

        // Network error: the weekly re-check is due (validated long ago) and the server cannot be reached.
        file.replaceWithText (svl::License::encodeStored ({ key, "inst-1", 1000, B::svl }));
        cfg.svlApiBase = "http://127.0.0.1:9/api/license/";   // nothing listens: connection refused
        {
            svl::License l (cfg);
            const bool loaded = l.isLicensed();
            const auto until = juce::Time::getMillisecondCounter() + 30000;
            while (l.revalidationsFinished() == 0 && juce::Time::getMillisecondCounter() < until)
                juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
            const auto stored = svl::License::decodeStored (file.loadFileAsString());
            check (loaded && l.revalidationsFinished() == 1 && l.isLicensed() && file.existsAsFile() && stored.key == key && stored.instanceId == "inst-1",
                   "Network error: the due weekly re-check could not reach the server and the activation (memory and file) stayed");
        }
        const auto throttle = svl::License::parseValidate (R"({"message":"Too Many Attempts."})", cfg, B::lemonSqueezy);
        const auto html = svl::License::parseValidate ("<html>502 Bad Gateway</html>", cfg, B::svl);
        check (! throttle.definitelyRevoked && ! html.definitelyRevoked && ! throttle.ok && ! html.ok,
               "Network error: a throttle notice or a gateway page is 'could not ask', not a revocation");
        dir.deleteRecursively();
    }
#else
    // SVL_LICENSING=0: no gate at all, a fresh instance processes immediately.
    {
        KasetProcessor probeLatency;
        probeLatency.prepareToPlay (sr, 512);
        const int latency = probeLatency.getLatencySamples();
        auto in = noise ((int) sr, 0.2f);
        auto out = run (in, sr, [] (KasetProcessor& p) { set (p, "generation", 2.0f); set (p, "mix", 1.0f); });
        float diff = 0.0f;
        for (int i = latency + 2000; i < out.getNumSamples(); ++i)
            diff = juce::jmax (diff, std::abs (out.getSample (0, i) - in.getSample (0, i - latency)));
        check (diff > 1.0e-4f, "SVL_LICENSING=0: a fresh instance processes with no licence check at all, difference " + juce::String (diff, 6));
    }
#endif

    {   // Cmd+Z / Cmd+Shift+Z (SvlUndoTests.h). KASET has no preset / mood recall, so the "recall" is a Group around several writes.
        const auto u = svl::undotest::run<KasetProcessor> ("KASET", [] { return std::make_unique<KasetProcessor>(); },
                                                           { { "input", 4.5f }, { "wow", 0.8f }, { "hiss", 0.1f }, { "track", 1.0f } },
                                                           [] (KasetProcessor& p)
                                                           {
                                                               svl::ParamUndo::Group oneStep (p.undo, "Test recall");
                                                               set (p, "generation", 4.0f);
                                                               set (p, "tape", 1.0f);
                                                               set (p, "drive", 0.2f);
                                                               set (p, "wear", 0.7f);
                                                           });
        check (u.fail == 0, "undo / redo (SvlUndoTests.h): " + juce::String (u.pass) + " of " + juce::String (u.pass + u.fail) + " undo checks passed");
    }

    {   // Type-in text of every float parameter (SvlParamTextTests.h), plus the issues found by the 2026-10-12 audit as literal cases.
        KasetProcessor proc;
        const auto t = svl::paramtexttest::run (proc, { "input", "drive", "wow", "flutter", "hiss", "wear", "mix", "output" });
        for (auto& f : t.failures) std::printf ("  paramtext: %s\n", f.toRawUTF8());
        check (t.fail == 0, "type-in text: " + juce::String (t.pass) + " of " + juce::String (t.pass + t.fail) + " checks passed (8 float parameters)");
        auto txt = [&] (const char* id, const char* s) { auto* q = proc.apvts.getParameter (id); return q->convertFrom0to1 (q->getValueForText (s)); };
        check (std::abs (txt ("mix", "25 %") - 0.25f) < 0.005f && std::abs (txt ("mix", "25") - 0.25f) < 0.005f, "type-in: Mix \"25 %\" and \"25\" are 25 %");
        check (std::abs (txt ("drive", "40 %") - 0.4f) < 0.005f && std::abs (txt ("wear", "75%") - 0.75f) < 0.005f && txt ("hiss", "500") == 1.0f, "type-in: Saturation \"40 %\", Wear \"75%\", Hiss 500 clamps to 100 %");
        check (std::abs (txt ("input", "+3.0 dB") - 3.0f) < 0.005f && std::abs (txt ("input", "-6") - (-6.0f)) < 0.005f && txt ("output", "99") == 12.0f, "type-in: Input \"+3.0 dB\" and \"-6\" are dB, Output 99 clamps to +12");
    }

    std::printf ("\n%s\n", failures == 0 ? "ALL PASS" : (juce::String (failures) + " FAILED").toRawUTF8());
    return failures == 0 ? 0 : 1;
}
} // namespace

// ---- --controlproof: every knob in every variation (SvlControlProof.h), through the real processor ----------------
int runControlProof (int argc, char** argv)
{
    namespace cp = svl::controlproof;
    using cp::feat::outRmsDb; using cp::feat::wetRmsDb; using cp::feat::bandRatioDb; using cp::feat::brightnessDb; using cp::feat::devFromMinDb;
    cp::Config c;
    c.name = "KASET";
    c.make = [] { return std::make_unique<KasetProcessor>(); };
    c.afterCreate = [] (juce::AudioProcessor& p)
    {
#if SVL_LICENSING
        static_cast<KasetProcessor&> (p).license.setLicensedForTesting (true);   // as every test in this file does: without a licence the processor passes the dry signal
#else
        juce::ignoreUnused (p);
#endif
    };
    c.tailSeconds = 0.3;          // getTailLengthSeconds() = 0.05 s, plus 15 ms of latency and a margin
    c.automationForce = { { "autoGain", 0.0f } };   // check 3 only: AUTO GAIN follows the loudness over 1.5 s, so a fast INPUT sweep makes a level excursion that is not a click
    c.latencyForce = { { "mix", 0.0f } };
    c.latencyVariants = { { { "mix", 0.0f }, { "track", 1.0f } } };   // TRACK reports 66 / 72 / 132 samples: the impulse must come back exactly that late   // the dry path is the latency-aligned one: the impulse must come back exactly `latency` samples late
    c.designedGainDb = [] (const cp::StateView& s) { return (double) juce::jmax (0.0f, s.raw ("input")) + (double) juce::jmax (0.0f, s.raw ("output")) + 6.0; };   // INPUT and OUTPUT up to +12 dB each; tape saturation and auto gain
    // HISS is noise by design: "about -62 dB at full per pass" (KasetEngine.h), up to 5 passes; the silence-in peak must stay under -35 dBFS then, and under -90 dBFS with HISS 0
    c.noiseFloorDb = [] (const cp::StateView& s) { return s.raw ("hiss") > 0.0f ? -35.0 : -90.0; };
    c.selfSustains = [] (const cp::StateView& s) { return s.raw ("hiss") > 0.0f; };
    using Force = std::vector<std::pair<juce::String, float>>;
    // every declaration: tape in (play 1), mix 100 %, auto gain off (it follows the loudness over 1.5 s and would hide a level knob)
    auto D = [] (const char* id, int dir, cp::Feature f, double tol, const char* fname, const char* note, Force force = {}, int mat = cp::kDrums)
    {
        cp::Direction d; d.id = id; d.dir = dir; d.feature = f; d.tol = tol; d.featureName = fname; d.note = note; d.material = mat;
        d.force = { { "play", 1.0f }, { "mix", 1.0f }, { "autoGain", 0.0f } };
        if (juce::String (id) != "output") d.force.push_back ({ "output", 0.5f });   // OUTPUT 0 dB: it scales the dry path too, so it would swamp a wet-part measurement
        for (auto& x : force) d.force.push_back (x);
        return d;
    };
    const Force clean = { { "hiss", 0.0f }, { "wow", 0.0f }, { "flutter", 0.0f }, { "wear", 0.0f } };
    auto with = [&] (Force extra) { Force r = clean; for (auto& x : extra) r.push_back (x); return r; };
    c.directions = {
        D ("generation", -1, brightnessDb (4000.0), 0.25, "output power above 4 kHz re total (dB) on pink, HISS / WOW / FLUTTER / WEAR 0", "each dub 'loses a little more top' (KasetEngine.h header)", clean, cp::kPink),
        D ("generation", +1, outRmsDb(), 0.25, "output rms on silence (dB), HISS full", "each dub 'adds its own hiss' (KasetEngine.h header)", { { "hiss", 1.0f } }, cp::kSilence),
        D ("tape", 0, nullptr, 0, "", "three tape types, no ordering"),
        D ("input", +1, outRmsDb(), 0.25, "output rms (dB)", "level into the tape"),
        D ("drive", +1, bandRatioDb (400.0, 3000.0, 150.0, 400.0), 0.25, "harmonic band 0.4-3 kHz over the chord band 0.15-0.4 kHz (dB), HISS / WOW / FLUTTER / WEAR 0", "SATURATION = how hard the tape is hit: more harmonics against the fundamentals (a plain band power is not used: the saturator is divided by the drive, so the level falls at hot input)", clean, cp::kChord),
        D ("wow", +1, devFromMinDb(), 0.25, "rms of (output - the WOW 0 render) on the chord, FLUTTER / WEAR 0", "WOW = slow speed drift", with ({ { "flutter", 0.0f }, { "wear", 0.0f } }), cp::kChord),
        D ("flutter", +1, devFromMinDb(), 0.25, "rms of (output - the FLUTTER 0 render) on the chord, WOW / WEAR 0", "FLUTTER = fast speed wobble", with ({ { "wow", 0.0f }, { "wear", 0.0f } }), cp::kChord),
        D ("hiss", +1, outRmsDb(), 0.25, "output rms on silence (dB)", "HISS = noise added on the way out", {}, cp::kSilence),
        D ("wear", +1, devFromMinDb(), 0.25, "rms of (output - the WEAR 0 render) on the chord, WOW / FLUTTER 0, values under -60 dB not judged", "WEAR = dropouts and worn oxide", with ({ { "wow", 0.0f }, { "flutter", 0.0f } }), cp::kChord),
        D ("mix", +1, wetRmsDb(), 0.25, "wet part = output - latency-aligned input (rms dB)", "0 is the latency-aligned dry signal"),
        D ("output", +1, outRmsDb(), 0.25, "output rms (dB)", "output gain"),
        D ("autoGain", 0, nullptr, 0, "", "switch: follows the loudness over 1.5 s, so a 1 s render cannot show it"),
        D ("play", 0, nullptr, 0, "", "tape in / bypass switch"),
        D ("track", 0, nullptr, 0, "", "switch: one low-latency head (66 samples at 44.1 kHz) instead of 660; the output dips for about 20 ms when it flips, so no ramp or direction applies"),
    };
    for (auto& d : c.directions) if (d.id == "wear") d.floorValue = -60.0;
    return cp::main (c, argc, argv);
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    for (int i = 1; i < argc; ++i)
        if (juce::String (argv[i]).startsWith ("--controlproof")) return runControlProof (argc, argv);
    if (argc >= 2 && juce::String (argv[1]) == "--selftest")
        return selftest();
    if (argc < 3)
    {
        std::printf ("render --selftest\nrender <in.wav> <out.wav> [generation 0-4] [tape 0-2] [mix]\n");
        return 2;
    }
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (juce::File (juce::String (argv[1]))));
    if (! reader) return 2;
    const double sr = reader->sampleRate;
    juce::AudioBuffer<float> in (2, (int) reader->lengthInSamples);
    reader->read (&in, 0, (int) reader->lengthInSamples, 0, true, true);
    auto arg = [&] (int i, float d) { return argc > i ? (float) std::atof (argv[i]) : d; };
    auto out = run (in, sr, [&] (KasetProcessor& p) { set (p, "generation", arg (3, 2)); set (p, "tape", arg (4, 0)); set (p, "mix", arg (5, 1.0f)); });
    juce::File f { juce::String (argv[2]) };
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (f), sr, 2, 24, {}, 0));
    w->writeFromAudioSampleBuffer (out, 0, out.getNumSamples());
    return 0;
}
