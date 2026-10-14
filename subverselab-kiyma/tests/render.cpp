/*  Drives the KIYMA v6 engine and the real plug-in processor with a fake host clock (tempo and beat position).

        render --selftest
            The v6 acceptance checks (docs/DSP_V6_CHOPPER.md): the map against chopper.js, the read position against the Max device,
            null, finite, block-size invariance, presets, undo. Exit code 0 only if every check passes.

        render --controlproof [options]
            The shared control harness (tests/SvlControlProof.h) through the real processor.

        render <in.wav> <out.wav> [glitch=0..1] [brutal=0..1] [offset=0..1] [envelope=0..1] [attack=0..10]
               [release=0..1] [mix=0..1] [crank=0..1] [output=-24..6] [bpm=120] [block=512] [tail=0]
            Renders the file through the plug-in with those parameters (the rest at their defaults) and writes a 24-bit WAV.
            The ids are the v5 ones; their meanings are DENSITY, REACH, PATTERN, LENGTH, EDGE, STEPS, MIX, SHUFFLE, OUTPUT.
*/
#include <complex>
#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"
#include "../Source/KnobDrag.h"
#include "SvlUndoTests.h"
#include "SvlControlProof.h"

namespace
{
using Setup = std::vector<std::pair<const char*, float>>;   // parameter id -> plain value

struct FakeHost : juce::AudioPlayHead
{
    double bpm = 120.0, ppq = 0.0;
    bool playing = true;

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setBpm (bpm);
        info.setPpqPosition (ppq);
        info.setIsPlaying (playing);
        return info;
    }
};

void setParam (KiymaProcessor& p, const char* id, float plain)
{
    auto* par = p.apvts.getParameter (id);
    par->setValueNotifyingHost (par->convertTo0to1 (plain));
}

/** Runs a whole buffer through `proc` in fixed blocks. useHost = a playhead with tempo and position. */
juce::AudioBuffer<float> run (KiymaProcessor& proc, const juce::AudioBuffer<float>& input, double sr, const Setup& setup,
                              int block = 512, bool useHost = false, double bpm = 120.0,
                              std::function<void (KiymaProcessor&, int blockStart)> perBlock = {})
{
    FakeHost host;
    host.bpm = bpm;
    if (useHost)
        proc.setPlayHead (&host);
    juce::AudioProcessor::BusesLayout layout;
    const auto set = input.getNumChannels() == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();
    layout.inputBuses.add (set);
    layout.outputBuses.add (set);
    proc.setBusesLayout (layout);
    proc.setRateAndBufferSizeDetails (sr, block);
    proc.prepareToPlay (sr, block);
    for (const auto& s : setup)
        setParam (proc, s.first, s.second);

    juce::AudioBuffer<float> buf (input);
    const double spb = sr * 60.0 / bpm;
    juce::MidiBuffer midi;
    for (int pos = 0; pos < buf.getNumSamples(); pos += block)
    {
        const int n = juce::jmin (block, buf.getNumSamples() - pos);
        host.ppq = pos / spb;
        if (perBlock)
            perBlock (proc, pos);
        juce::AudioBuffer<float> view (buf.getArrayOfWritePointers(), buf.getNumChannels(), pos, n);
        proc.processBlock (view, midi);
    }
    return buf;
}


juce::AudioBuffer<float> noise (int channels, int samples, float amp, uint32_t seed = 12345u)
{
    juce::AudioBuffer<float> b (channels, samples);
    uint32_t s = seed;   // a fixed LCG: the same input on every run and every machine
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < samples; ++i)
        {
            s = s * 1664525u + 1013904223u;
            b.setSample (c, i, ((float) (s >> 8) / 16777216.0f - 0.5f) * 2.0f * amp);
        }
    return b;
}


juce::AudioBuffer<float> sine (int channels, int samples, double hz, float amp, double sr)
{
    juce::AudioBuffer<float> b (channels, samples);
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < samples; ++i)
            b.setSample (c, i, amp * (float) std::sin (2.0 * juce::MathConstants<double>::pi * hz * i / sr));
    return b;
}

/** Decaying noise bursts, one per `period` samples: a stand-in for drum hits. */

float peakOf (const juce::AudioBuffer<float>& b, int from = 0)
{
    float m = 0.0f;
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = from; i < b.getNumSamples(); ++i)
            m = std::max (m, std::abs (b.getSample (c, i)));
    return m;
}

double toDb (double x) { return 20.0 * std::log10 (std::max (x, 1.0e-12)); }

int countNonFinite (const juce::AudioBuffer<float>& b)
{
    int n = 0;
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = 0; i < b.getNumSamples(); ++i)
            n += std::isfinite (b.getSample (c, i)) ? 0 : 1;
    return n;
}

float maxStep (const juce::AudioBuffer<float>& b, int from)
{
    float m = 0.0f;
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = std::max (1, from); i < b.getNumSamples(); ++i)
            m = std::max (m, std::abs (b.getSample (c, i) - b.getSample (c, i - 1)));
    return m;
}

bool identical (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    if (a.getNumSamples() != b.getNumSamples() || a.getNumChannels() != b.getNumChannels())
        return false;
    for (int c = 0; c < a.getNumChannels(); ++c)
        if (std::memcmp (a.getReadPointer (c), b.getReadPointer (c), sizeof (float) * (size_t) a.getNumSamples()) != 0)
            return false;
    return true;
}
int failures = 0, checks = 0;
void check (bool ok, const juce::String& what)
{
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << "\n";
    ++checks;
    if (! ok) ++failures;
}



//==============================================================================

//==============================================================================
// v6 helpers
kiyma::Params P6 (float density, float reachKnob, float pattern, float length, float edgeMs = 0.0f, float steps = 0.5f, float mix = 1.0f, float outDb = 0.0f, float crank = 0.0f)
{
    kiyma::Params p;
    p.glitch = density; p.brutal = reachKnob; p.offset = pattern; p.envelope = length; p.attackMs = edgeMs; p.releaseMs = steps;
    p.mix = mix; p.outputDb = outDb; p.crank = crank;
    return p;
}

/** The engine alone, in blocks, with the transport at ppq0 + k / spb. */
juce::AudioBuffer<float> engineRun (const juce::AudioBuffer<float>& in, double sr, const kiyma::Params& p, int block = 512, double bpm = 120.0,
                                    double ppq0 = 0.0, kiyma::Engine::Probe* probe = nullptr)
{
    kiyma::Engine e;
    e.prepare (sr, in.getNumChannels());
    e.setProbe (probe);
    juce::AudioBuffer<float> b (in);
    kiyma::Transport t;
    t.bpm = bpm;
    const double spb = sr * 60.0 / bpm;
    for (int pos = 0; pos < b.getNumSamples(); pos += block)
    {
        const int n = juce::jmin (block, b.getNumSamples() - pos);
        t.ppq = ppq0 + pos / spb;
        float* ch[2] { b.getWritePointer (0, pos), b.getNumChannels() > 1 ? b.getWritePointer (1, pos) : nullptr };
        e.process (ch, b.getNumChannels(), n, p, t);
    }
    return b;
}

std::vector<int> mapOf (int steps, int density, int reach, int seed)
{
    std::vector<int> m ((size_t) steps);
    kiyma::buildMap (m.data(), steps, density, reach, seed);
    return m;
}

juce::String join (const std::vector<int>& v)
{
    juce::StringArray s;
    for (int x : v) s.add (juce::String (x));
    return s.joinIntoString (",");
}

/** Dominant frequency by zero crossings of channel 0 over [from, to). */
double zcHz (const juce::AudioBuffer<float>& b, int from, int to, double sr)
{
    int z = 0;
    const float* d = b.getReadPointer (0);
    for (int i = from + 1; i < to; ++i)
        if ((d[i - 1] < 0.0f) != (d[i] < 0.0f)) ++z;
    return 0.5 * z * sr / (to - from);
}

/** A 2-bar "file": a decaying chirp-ish tone plus a click every beat, so a wrong slice shows. */
juce::AudioBuffer<float> phraseFile (double sr, double bpm, double bars)
{
    const int n = (int) std::llround (sr * 60.0 / bpm * 4.0 * bars);
    juce::AudioBuffer<float> f (2, n);
    for (int i = 0; i < n; ++i)
    {
        const double t = i / sr, beat = std::fmod (i / (sr * 60.0 / bpm), 1.0);
        const float v = (float) (0.4 * std::sin (2.0 * kiyma::kPi * (220.0 + 180.0 * i / n) * t) * (0.4 + 0.6 * std::exp (-beat * 6.0)));
        f.setSample (0, i, v);
        f.setSample (1, i, -0.7f * v);
    }
    return f;
}

//==============================================================================
int selftest()
{
    const double sr = 48000.0;
    std::cout << "KIYMA v6 selftest (the ChopperLab step engine on the channel)\n";

    std::cout << "The map (chopper.js generate(), recovered_chopperlab/chopper_c.js)\n";
    {
        // Reference values printed by node from the original chopper.js (density 40, reach 6, seeds 1..8) and from the Aug 27/28 versions.
        const char* ref[8] { "0,1,2,3,5,5,6,7,8,9,9,7,12,12,14,11", "0,14,2,3,4,5,9,10,7,9,10,13,2,2,10,9", "0,0,5,3,4,2,6,2,8,9,10,11,14,15,14,15",
                             "0,14,15,3,10,5,6,7,11,9,10,11,12,13,14,15", "0,1,3,4,4,5,6,10,8,9,10,0,15,13,8,15", "0,3,3,13,4,5,11,1,7,9,10,11,12,3,14,4",
                             "11,1,2,3,4,5,7,7,4,9,10,9,12,3,3,11", "2,3,2,3,4,5,6,8,4,9,10,7,13,13,14,15" };
        bool all = true;
        for (int s = 1; s <= 8; ++s)
            all = all && join (mapOf (16, 40, 6, s)) == ref[s - 1];
        check (all, "density 40, reach 6, seeds 1..8: identical to the original chopper.js output (seed 1 = " + join (mapOf (16, 40, 6, 1)) + ")");
        check (join (mapOf (16, 50, 8, 1)) == "0,1,2,3,6,5,14,7,6,3,3,15,7,12,14,15", "density 50, reach 8, seed 1: identical to the Aug 28 plain device");
        bool identity = true;
        for (int s : { 8, 16, 32 })
        {
            const auto m = mapOf (s, 0, 6, 3);
            for (int i = 0; i < s; ++i) identity = identity && m[(size_t) i] == i;
        }
        check (identity, "density 0: the identity map at 8, 16 and 32 steps (the sample plays straight through)");
        const kiyma::Params d = P6 (0.40f, 5.0f / 15.0f, 0.0f, 2.0f / 3.0f);
        check (kiyma::densityOf (d.glitch) == 40 && kiyma::reachOf (d.brutal) == 6 && kiyma::seedOf (d.offset, 0.0f) == 1 && kiyma::lengthBars (kiyma::lengthIndex (d.envelope)) == 2.0
                   && kiyma::stepsOf (kiyma::stepsIndex (0.5f)) == 16,
               "the default knobs are the device's defaults: density 40, reach 6, seed 1, 16 steps, and 2 bars of material (the Haram file)");
        check (kiyma::seedOf (0.0f, 0.0f) == 1 && kiyma::seedOf (0.0f, 1.0f) == 16 && kiyma::seedOf (15.0f / 15.0f, 1.0f / 15.0f + 0.001f) == 1 && kiyma::seedOf (2.0f / 15.0f, 0.0f) == 3,
               "PATTERN + SHUFFLE: seeds 1..16, the crank adds 0..15 and wraps");
    }

    std::cout << "The read position (the Max device: phasor~ 1n @lock 1 -> step map -> play~)\n";
    {
        // A looped 2-bar phrase, bar-aligned, is exactly the dropped 2-bar file: from bar 2 on, every output bar must be the device's output
        // for that file, sample for sample (linear interpolation, the same squeeze).
        const double bpm = 96.0;   // a whole number of samples per bar (120000), so the looped phrase stays on the grid
        const auto file = phraseFile (sr, bpm, 2.0);
        const int fl = file.getNumSamples();
        const int total = fl * 5;
        juce::AudioBuffer<float> in (2, total);
        for (int i = 0; i < total; ++i)
            for (int c = 0; c < 2; ++c) in.setSample (c, i, file.getSample (c, i % fl));
        const auto out = engineRun (in, sr, P6 (0.40f, 5.0f / 15.0f, 0.0f, 2.0f / 3.0f), 512, bpm);
        const auto m = mapOf (16, 40, 6, 1);
        const double bar = sr * 60.0 / bpm * 4.0;
        double worst = 0.0;
        for (int i = fl; i < total; ++i)
        {
            const double ph = std::fmod ((double) i, bar) / bar * 16.0;
            const int st = std::min (15, (int) ph);
            const double pos = (m[(size_t) st] + (ph - st)) * fl / 16.0;
            const int i0 = (int) pos;
            const double a = pos - i0;
            for (int c = 0; c < 2; ++c)
            {
                const double want = file.getSample (c, i0) * (1.0 - a) + file.getSample (c, (i0 + 1) % fl) * a;
                worst = std::max (worst, std::abs (want - out.getSample (c, i)));
            }
        }
        check (worst < 2.0e-3, "a looped 2-bar phrase at 96 BPM, seed 1: every bar from bar 2 on is the Max device's output for that file (worst " + juce::String (worst, 6) + ")");
        // the same with STEPS 16 and density 0: the plain D device (squeeze only)
        const auto plain = engineRun (in, sr, P6 (0.0f, 5.0f / 15.0f, 0.0f, 2.0f / 3.0f), 512, bpm);
        double w2 = 0.0;
        for (int i = fl; i < total; ++i)
        {
            const double pos = std::fmod ((double) i, bar) / bar * fl;
            const int i0 = (int) pos;
            const double a = pos - i0;
            w2 = std::max (w2, std::abs (file.getSample (0, i0) * (1.0 - a) + file.getSample (0, (i0 + 1) % fl) * a - plain.getSample (0, i)));
        }
        check (w2 < 2.0e-3, "density 0: the Haram device (the whole phrase squeezed into each bar), worst " + juce::String (w2, 6));
    }

    std::cout << "LENGTH is speed (and pitch)\n";
    {
        const auto in = sine (1, (int) (sr * 20), 500.0, 0.5f, sr);
        const int a = (int) (sr * 16), b = (int) (sr * 20);   // after 8 bars: every LENGTH has a whole block of material
        const double hz[4] { zcHz (engineRun (in, sr, P6 (0.0f, 0.3f, 0.0f, 0.0f / 3.0f)), a, b, sr), zcHz (engineRun (in, sr, P6 (0.0f, 0.3f, 0.0f, 1.0f / 3.0f)), a, b, sr),
                             zcHz (engineRun (in, sr, P6 (0.0f, 0.3f, 0.0f, 2.0f / 3.0f)), a, b, sr), zcHz (engineRun (in, sr, P6 (0.0f, 0.3f, 0.0f, 3.0f / 3.0f)), a, b, sr) };
        check (std::abs (hz[0] - 250.0) < 6.0 && std::abs (hz[1] - 500.0) < 6.0 && std::abs (hz[2] - 1000.0) < 8.0 && std::abs (hz[3] - 2000.0) < 12.0,
               "a 500 Hz sine through LENGTH 1/2, 1, 2, 4 bars comes out at " + juce::String (hz[0], 1) + ", " + juce::String (hz[1], 1) + ", " + juce::String (hz[2], 1) + ", " + juce::String (hz[3], 1) + " Hz");
        // LENGTH 1 bar, density 0: the input exactly one bar late (integer read positions at 120 BPM / 48 kHz)
        const auto nz = noise (2, (int) (sr * 6), 0.3f);
        const auto o = engineRun (nz, sr, P6 (0.0f, 0.3f, 0.0f, 1.0f / 3.0f));
        const int bar = (int) (sr * 2);
        double w = 0.0;
        for (int i = bar; i < nz.getNumSamples(); ++i)
            for (int c = 0; c < 2; ++c) w = std::max (w, (double) std::abs (o.getSample (c, i) - nz.getSample (c, i - bar)));
        check (w < 1.0e-6, "LENGTH 1 bar, density 0: the channel exactly one bar later (worst " + juce::String (w, 9) + ")");
        // before a whole block of material has arrived, the dry signal passes
        double w0 = 0.0;
        const auto o2 = engineRun (nz, sr, P6 (0.4f, 0.3f, 0.0f, 3.0f / 3.0f));
        for (int i = 0; i < (int) (sr * 8) && i < nz.getNumSamples(); ++i)
            w0 = std::max (w0, (double) std::abs (o2.getSample (0, i) - nz.getSample (0, i)));
        check (w0 < 1.0e-7, "LENGTH 4 bars: until 4 bars of the channel have been heard, the dry passes (worst " + juce::String (w0, 9) + ")");
    }

    std::cout << "Null, finite, determinism\n";
    {
        const auto nz = noise (2, (int) (sr * 8), 0.5f);
        check (identical (engineRun (nz, sr, P6 (0.4f, 0.3f, 0.0f, 2.0f / 3.0f, 0.0f, 0.5f, 0.0f)), nz), "mix 0: the input, bit for bit (no latency)");
        bool fin = true;
        float pk = 0.0f;
        for (float d : { 0.0f, 0.4f, 1.0f })
            for (float r : { 0.0f, 0.5f, 1.0f })
                for (float l : { 0.0f, 1.0f / 3.0f, 2.0f / 3.0f, 1.0f })
                    for (float st : { 0.0f, 0.5f, 1.0f })
                    {
                        const auto o = engineRun (nz, sr, P6 (d, r, 0.4f, l, 3.0f, st, 1.0f, 6.0f, 0.9f));
                        fin = fin && countNonFinite (o) == 0;
                        pk = std::max (pk, peakOf (o));
                    }
        check (fin, "108 settings (density, reach, length, steps; edge 3 ms, +6 dB, crank 0.9): no NaN/Inf; peak " + juce::String (toDb (pk), 2) + " dBFS (input peak -6 dBFS + 6 dB)");
        auto bad = nz;
        bad.setSample (0, 1000, std::numeric_limits<float>::quiet_NaN());
        bad.setSample (1, 50000, std::numeric_limits<float>::infinity());
        check (countNonFinite (engineRun (bad, sr, P6 (0.4f, 0.3f, 0.0f, 1.0f / 3.0f))) <= 2, "NaN / Inf in: never more than the injected samples out (the ring stores 0 for them)");
        const auto a = engineRun (nz, sr, P6 (0.4f, 0.3f, 0.2f, 2.0f / 3.0f, 2.0f), 64), b = engineRun (nz, sr, P6 (0.4f, 0.3f, 0.2f, 2.0f / 3.0f, 2.0f), 1013);
        double w = 0.0;
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < nz.getNumSamples(); ++i) w = std::max (w, (double) std::abs (a.getSample (c, i) - b.getSample (c, i)));
        check (w == 0.0, "block size 64 vs 1013: identical (worst " + juce::String (w, 9) + ")");
        check (identical (engineRun (nz, sr, P6 (0.4f, 0.3f, 0.2f, 2.0f / 3.0f)), engineRun (nz, sr, P6 (0.4f, 0.3f, 0.2f, 2.0f / 3.0f))), "run vs run: identical (no randomness)");
        check (! identical (engineRun (nz, sr, P6 (0.4f, 0.3f, 0.0f, 2.0f / 3.0f)), engineRun (nz, sr, P6 (0.4f, 0.3f, 0.0f, 2.0f / 3.0f, 0.0f, 0.5f, 1.0f, 0.0f, 0.5f))),
               "the crank (SHUFFLE) changes the pattern");
        // 313.7 Hz: no whole or half number of cycles per slice, so the cuts are real discontinuities
        const auto hard = engineRun (sine (2, (int) (sr * 8), 313.7, 0.5f, sr), sr, P6 (0.6f, 0.5f, 0.0f, 2.0f / 3.0f, 0.0f));
        const auto soft = engineRun (sine (2, (int) (sr * 8), 313.7, 0.5f, sr), sr, P6 (0.6f, 0.5f, 0.0f, 2.0f / 3.0f, 5.0f));
        const int from = (int) (sr * 5);   // after the one-time switch from the dry (no material yet) to the wet at bar 2
        check (maxStep (soft, from) < 0.5f * maxStep (hard, from), "EDGE 5 ms rounds the cuts: largest sample step " + juce::String (maxStep (soft, from), 4) + " vs "
                   + juce::String (maxStep (hard, from), 4) + " hard");
    }

    std::cout << "The processor\n";
    {
        KiymaProcessor proc;
        const auto nz = noise (2, (int) (sr * 3), 0.3f);
        const auto out = run (proc, nz, sr, { { kiymaIds::mix, 0.0f } }, 512, true, 120.0);
        check (proc.getLatencySamples() == 0 && identical (out, nz), "the plug-in at mix 0: latency 0, the input bit for bit");
        KiymaProcessor fresh;
        check (fresh.apvts.getParameter (kiymaIds::brutal)->getNumSteps() == 16 && fresh.apvts.getParameter (kiymaIds::offset)->getNumSteps() == 16
                   && fresh.apvts.getParameter (kiymaIds::envelope)->getNumSteps() == 4 && fresh.apvts.getParameter (kiymaIds::release)->getNumSteps() == 3,
               "REACH and PATTERN have 16 steps, LENGTH 4, STEPS 3");
        check (fresh.apvts.getParameter (kiymaIds::offset)->getCurrentValueAsText() == "1" && fresh.apvts.getParameter (kiymaIds::envelope)->getCurrentValueAsText() == "2 BARS"
                   && fresh.apvts.getParameter (kiymaIds::brutal)->getCurrentValueAsText() == "6 steps" && fresh.apvts.getParameter (kiymaIds::attack)->getCurrentValueAsText() == "HARD",
               "defaults read as the device: pattern 1, 2 BARS, reach 6 steps, edge HARD");
    }

    std::cout << "Presets\n";
    {
        check (kiyma::presetTrips (0.50f, 0.52f) && ! kiyma::presetTrips (0.50f, 0.505f) && kiyma::presetTrips (0.52f, 0.50f) && ! kiyma::presetTrips (0.4f, 0.4f),
               "auto-trip rule: a deviation of 0.02 trips, 0.005 does not, either way");
        const auto nz = noise (2, (int) (sr * 6), 0.3f);
        bool ok = true;
        for (const auto& pr : kiyma::kPresets)
        {
            const auto o = engineRun (nz, sr, P6 (pr.v[0], pr.v[1], pr.v[2], pr.v[3], pr.v[4], pr.v[5], pr.v[6], pr.v[7]));
            ok = ok && countNonFinite (o) == 0 && identical (o, engineRun (nz, sr, P6 (pr.v[0], pr.v[1], pr.v[2], pr.v[3], pr.v[4], pr.v[5], pr.v[6], pr.v[7])));
        }
        check (ok, "each mood: finite, and identical when rendered twice");
        const auto& mi = kiyma::kPresets[1];
        check (juce::String (mi.name) == "MINCE" && kiyma::densityOf (mi.v[0]) == 40 && kiyma::reachOf (mi.v[1]) == 6 && kiyma::seedOf (mi.v[2], 0.0f) == 1
                   && kiyma::lengthIndex (mi.v[3]) == 2 && mi.v[4] == 0.0f && kiyma::stepsIndex (mi.v[5]) == 1,
               "MINCE is the owner's ChopperLab sound: density 40, reach 6, seed 1, 2 bars, hard cuts, 16 steps");

        KiymaProcessor a;
        a.setRateAndBufferSizeDetails (sr, 512);
        a.prepareToPlay (sr, 512);
        setParam (a, kiymaIds::crank, 0.7f);
        setParam (a, kiymaIds::glitch, 0.9f);
        const int stepsBefore = a.undo.undoSteps();
        a.recallPreset (1);
        bool values = true;
        for (int slot = 0; slot < 8; ++slot)
            values = values && std::abs (a.apvts.getParameter (kiyma::kPresetParamIds[slot])->getValue() - a.presetNormalised (1, slot)) < 1.0e-4f;
        check (a.getActivePreset() == 1 && values && a.apvts.getRawParameterValue (kiymaIds::crank)->load() == 0.0f,
               "recall MINCE: it is active, all 8 values written, CRANK set to 0, and the recall itself did not trip the switch");
        check (a.undo.undoSteps() - stepsBefore == 1, "the recall is ONE undo step (" + juce::String (a.undo.undoSteps() - stepsBefore) + ")");
        setParam (a, kiymaIds::glitch, 0.405f);
        check (a.getActivePreset() == 1, "density moved by 0.005: the switch stays up");
        setParam (a, kiymaIds::glitch, 0.42f);
        check (a.getActivePreset() == -1, "density moved by 0.02: the switch trips down by itself");
        setParam (a, kiymaIds::glitch, 0.40f);
        check (a.getActivePreset() == -1, "moving it back does not raise the switch again");
        a.recallPreset (3);
        a.undo.undo();
        check (a.getActivePreset() == -1 && std::abs (a.apvts.getRawParameterValue (kiymaIds::glitch)->load() - 0.40f) < 1.0e-4f, "undo of a recall restores the old values in one step, and the switch is down");
        check (a.apvts.getParameter ("activePreset") == nullptr && a.getParameters().size() == 9, "activePreset is not a parameter (9 parameters, none called activePreset)");
        a.recallPreset (2);
        juce::MemoryBlock blob;
        a.getStateInformation (blob);
        KiymaProcessor b;
        b.setStateInformation (blob.getData(), (int) blob.getSize());
        check (b.getActivePreset() == 2, "the active preset is saved with the session and restored (CHUNK)");
        KiymaProcessor c2;
        setParam (c2, kiymaIds::glitch, 0.31f);
        c2.forceActivePresetForTest (2);
        juce::MemoryBlock blob2;
        c2.getStateInformation (blob2);
        KiymaProcessor d;
        d.setStateInformation (blob2.getData(), (int) blob2.getSize());
        check (d.getActivePreset() == -1, "a state whose values no longer match its preset restores with no switch up");
    }

    std::cout << "Knob drag and wheel\n";
    {
        check (kiyma::isStepped (16) && kiyma::isStepped (4) && kiyma::isStepped (3) && ! kiyma::isStepped (0x7fffffff), "the selectors count as stepped for the wheel");
    }

    std::cout << "Undo\n";
    {
        const auto u = svl::undotest::run<KiymaProcessor> ("KIYMA", [] { return std::make_unique<KiymaProcessor>(); },
                                                           { { kiymaIds::glitch, 0.8f }, { kiymaIds::brutal, 0.6f }, { kiymaIds::mix, 0.9f } },
                                                           [] (KiymaProcessor& p)
                                                           {
                                                               svl::ParamUndo::Group oneStep (p.undo, "Test recall");
                                                               setParam (p, kiymaIds::glitch, 0.9f);
                                                               setParam (p, kiymaIds::offset, 0.4f);
                                                               setParam (p, kiymaIds::crank, 0.3f);
                                                               setParam (p, kiymaIds::output, -6.0f);
                                                           });
        check (u.fail == 0, "undo / redo (SvlUndoTests.h): " + juce::String (u.pass) + " of " + juce::String (u.pass + u.fail) + " undo checks passed");
        checks += u.pass + u.fail - 1;
    }

    std::cout << "\n" << (failures == 0 ? "ALL PASS" : (juce::String (failures) + " FAILED")) << " (" << checks << " checks)\n";
    return failures == 0 ? 0 : 1;
}

//==============================================================================
int controlProof (int argc, char** argv)
{
    namespace cp = svl::controlproof;
    cp::Config c;
    c.name = "KIYMA";
    c.make = [] { return std::make_unique<KiymaProcessor>(); };
    c.tailSeconds = 8.0;    // the wet path replays up to 4 bars of the past
    c.designedGainDb = [] (const cp::StateView& s) { return (double) std::max (0.0f, s.raw (kiymaIds::output)); };   // OUTPUT adds up to +6 dB by design
    auto D = [] (const char* id, int dir, cp::Feature f, double tol, const char* fname, const char* note)
    { cp::Direction d; d.id = id; d.dir = dir; d.feature = f; d.tol = tol; d.featureName = fname; d.note = note; return d; };
    const char* chop = "rebuilds the step map: the cut is a hard edge by design (the ChopperLab device), so the automation step check does not apply";
    c.directions = {
        D (kiymaIds::glitch, 0, nullptr, 0, "", "DENSITY: how many steps are displaced"),
        D (kiymaIds::brutal, 0, nullptr, 0, "", "REACH: how far a displaced step jumps"),
        D (kiymaIds::offset, 0, nullptr, 0, "", "PATTERN: which map (seed)"),
        D (kiymaIds::envelope, 0, nullptr, 0, "", "LENGTH: bars of the channel per bar (speed and pitch)"),
        D (kiymaIds::attack, 0, nullptr, 0, "", "EDGE: rounds the cuts"),
        D (kiymaIds::release, 0, nullptr, 0, "", chop),
        D (kiymaIds::mix, 0, nullptr, 0, "", "linear dry/wet"),
        D (kiymaIds::crank, 0, nullptr, 0, "", chop),
        D (kiymaIds::output, +1, cp::feat::outRmsDb(), 0.25, "output rms (dB)", "output gain") };
    return cp::main (c, argc, argv);
}

//==============================================================================
int renderFile (int argc, char** argv)
{
    juce::File inFile = juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]);
    juce::File outFile = juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]);
    Setup setup;
    std::vector<juce::String> names;   // keeps the id strings alive
    double bpm = 120.0, tail = 0.0;
    int block = 512;
    const char* known[] { kiymaIds::glitch, kiymaIds::brutal, kiymaIds::offset, kiymaIds::envelope, kiymaIds::attack, kiymaIds::release, kiymaIds::mix, kiymaIds::crank, kiymaIds::output };
    for (int i = 3; i < argc; ++i)
    {
        const juce::String a (argv[i]);
        const auto key = a.upToFirstOccurrenceOf ("=", false, false).trim();
        const auto val = a.fromFirstOccurrenceOf ("=", false, false).getFloatValue();
        if (key == "bpm") bpm = val;
        else if (key == "block") block = juce::jmax (1, (int) val);
        else if (key == "tail") tail = val;
        else
        {
            bool found = false;
            for (auto* k : known)
                if (key == k) { setup.push_back ({ k, val }); found = true; }
            if (! found) { std::cout << "unknown parameter: " << key << "\n"; return 2; }
        }
    }

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (inFile));
    if (reader == nullptr) { std::cout << "cannot read " << inFile.getFullPathName() << "\n"; return 2; }
    const double sr = reader->sampleRate;
    const int lat = kiyma::kLatency;
    const int inLen = (int) reader->lengthInSamples;
    const int total = inLen + lat + (int) std::lround (tail * sr);
    juce::AudioBuffer<float> in (2, total);
    in.clear();
    juce::AudioBuffer<float> file ((int) reader->numChannels, inLen);
    reader->read (&file, 0, inLen, 0, true, true);
    for (int c = 0; c < 2; ++c)
        in.copyFrom (c, 0, file, juce::jmin (c, file.getNumChannels() - 1), 0, inLen);

    KiymaProcessor proc;
    auto out = run (proc, in, sr, setup, block, true, bpm);
    juce::AudioBuffer<float> aligned (2, total - lat);
    for (int c = 0; c < 2; ++c)
        aligned.copyFrom (c, 0, out, c, lat, total - lat);

    outFile.deleteFile();
    std::unique_ptr<juce::OutputStream> stream (outFile.createOutputStream());
    if (stream == nullptr) { std::cout << "cannot write " << outFile.getFullPathName() << "\n"; return 2; }
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (2).withBitsPerSample (24)).release());
    if (writer == nullptr) { std::cout << "cannot create the wav writer\n"; return 2; }
    writer->writeFromAudioSampleBuffer (aligned, 0, aligned.getNumSamples());
    writer.reset();

    double rms = 0.0;
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < aligned.getNumSamples(); ++i)
            rms += (double) aligned.getSample (c, i) * aligned.getSample (c, i);
    rms = std::sqrt (rms / (2.0 * aligned.getNumSamples()));
    std::cout << "wrote " << outFile.getFullPathName() << ": " << aligned.getNumSamples() << " samples at " << sr << " Hz, latency " << lat << " removed, peak "
              << juce::String (toDb (peakOf (aligned)), 2) << " dBFS, rms " << juce::String (toDb (rms), 2) << " dBFS\n";
    return 0;
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    for (int i = 1; i < argc; ++i)
        if (juce::String (argv[i]) == "--selftest") return selftest();
    for (int i = 1; i < argc; ++i)
        if (juce::String (argv[i]) == "--controlproof") return controlProof (argc, argv);
    if (argc >= 3 && juce::String (argv[1]).isNotEmpty() && juce::String (argv[1])[0] != '-')
        return renderFile (argc, argv);
    std::cout << "usage: render --selftest | render --controlproof [options] | render <in.wav> <out.wav> [name=value ...]\n";
    return 1;
}