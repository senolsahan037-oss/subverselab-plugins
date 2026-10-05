// plugin_check: the real TutkalAudioProcessor against the raw engine, offline, on the listening pack's own files.
//   plugin_check [session_dir]      (default ../prototypes/DINLE/10_TUTKAL_v3/_session, relative to the project)
// Exit code 0 only if every check passed.
#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"
#include "../Source/ParamText.h"
#include "SvlUndoTests.h"
#include "SvlControlProof.h"
#include <cmath>
#include <cstdio>
#include <functional>

using Audio = std::vector<std::vector<float>>;
using Settings = std::vector<std::pair<const char*, float>>;

static int gPass = 0, gFail = 0;
static void check (bool ok, const juce::String& what, const juce::String& detail = {})
{
    std::printf ("[%s] %s%s%s\n", ok ? "PASS" : "FAIL", what.toRawUTF8(), detail.isEmpty() ? "" : "  ", detail.toRawUTF8());
    (ok ? gPass : gFail)++;
}

static Audio readWav (const juce::File& f)
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

static bool allFinite (const Audio& a)
{
    for (auto& c : a) for (float v : c) if (! std::isfinite (v)) return false;
    return true;
}

static void setParam (juce::AudioProcessorValueTreeState& s, const char* id, float v)
{
    auto* p = s.getParameter (id);
    jassert (p != nullptr);
    p->setValueNotifyingHost (p->convertTo0to1 (v));
}

struct TestPlayHead : juce::AudioPlayHead
{
    double bpm = 120.0;
    juce::Optional<PositionInfo> getPosition() const override { PositionInfo i; i.setBpm (bpm); return i; }
};

struct PluginRun { Audio out; std::vector<float> grBlock; int latency = 0; double maxGr = 0.0; };

// Renders `in` (1 or 2 channels) through the real processor. key: stereo sidechain, enabled only when scEnabled.
static PluginRun runPlugin (const Audio& in, const Audio* key, bool scEnabled, int block, int prepBlock,
                            const Settings& st, double bpm /* <= 0: no playhead */, bool bypassed = false)
{
    TutkalAudioProcessor proc;
    const int mainCh = (int) in.size();
    const auto mainSet = mainCh == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();
    auto layout = proc.getBusesLayout();
    layout.inputBuses.set (0, mainSet); layout.outputBuses.set (0, mainSet);
    layout.inputBuses.set (1, scEnabled ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::disabled());
    if (! proc.setBusesLayout (layout)) { std::printf ("setBusesLayout refused\n"); std::exit (2); }
    proc.setRateAndBufferSizeDetails (48000.0, prepBlock);
    proc.prepareToPlay (48000.0, prepBlock);
    for (auto& kv : st) setParam (proc.apvts, kv.first, kv.second);
    TestPlayHead ph; ph.bpm = bpm;
    if (bpm > 0.0) proc.setPlayHead (&ph);

    PluginRun r;
    r.latency = proc.getLatencySamples();
    const int n = (int) in[0].size();
    const int total = juce::jmax (proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
    r.out.assign ((size_t) mainCh, std::vector<float> ((size_t) n));
    juce::AudioBuffer<float> buf (total, block);
    juce::MidiBuffer midi;
    for (int pos = 0; pos < n; pos += block)
    {
        const int len = juce::jmin (block, n - pos);
        buf.setSize (total, len, false, false, true);
        buf.clear();
        for (int c = 0; c < mainCh; ++c) std::copy (in[(size_t) c].begin() + pos, in[(size_t) c].begin() + pos + len, buf.getWritePointer (c));
        if (scEnabled && key)
            for (int c = 0; c < 2 && mainCh + c < total; ++c)
                std::copy ((*key)[(size_t) c].begin() + pos, (*key)[(size_t) c].begin() + pos + len, buf.getWritePointer (mainCh + c));
        if (bypassed) proc.processBlockBypassed (buf, midi); else proc.processBlock (buf, midi);
        for (int c = 0; c < mainCh; ++c) std::copy (buf.getReadPointer (c), buf.getReadPointer (c) + len, r.out[(size_t) c].begin() + pos);
        r.grBlock.push_back (proc.getGainReductionDb());
        r.maxGr = juce::jmax (r.maxGr, (double) proc.getGainReductionDb());
    }
    proc.releaseResources();
    return r;
}

// The raw engine, block by block, same Params, no plug-in layer.
static PluginRun runRaw (const Audio& in, const Audio* keyIn, int block, const tutkal::Params& p)
{
    tutkal::Engine e; e.prepare (48000.0);
    PluginRun r; r.latency = tutkal::Engine::latency();
    r.out = in;
    const int nch = (int) in.size(), n = (int) in[0].size();
    std::vector<float> gr ((size_t) block);
    for (int pos = 0; pos < n; pos += block)
    {
        const int len = juce::jmin (block, n - pos);
        float* io[2]; const float* key[2];
        for (int c = 0; c < nch; ++c) io[c] = r.out[(size_t) c].data() + pos;
        if (keyIn) for (int c = 0; c < 2; ++c) key[c] = (*keyIn)[(size_t) c].data() + pos;
        e.process (io, nch, keyIn ? key : nullptr, keyIn ? 2 : 0, len, p, gr.data());
        float m = 0.0f; for (int i = 0; i < len; ++i) m = std::max (m, gr[(size_t) i]);
        r.grBlock.push_back (m);
        r.maxGr = std::max (r.maxGr, (double) m);
    }
    return r;
}

static double maxDiff (const Audio& a, const Audio& b)
{
    double m = 0.0;
    if (a.size() != b.size()) return 1e9;
    for (size_t c = 0; c < a.size(); ++c)
    {
        if (a[c].size() != b[c].size()) return 1e9;
        for (size_t i = 0; i < a[c].size(); ++i) m = std::max (m, (double) std::abs (a[c][i] - b[c][i]));
    }
    return m;
}
static double maxGrDiff (const std::vector<float>& a, const std::vector<float>& b)
{
    double m = a.size() == b.size() ? 0.0 : 1e9;
    for (size_t i = 0; i < std::min (a.size(), b.size()); ++i) m = std::max (m, (double) std::abs (a[i] - b[i]));
    return m;
}
static double peakOf (const Audio& a) { double m = 0; for (auto& c : a) for (float v : c) m = std::max (m, (double) std::abs (v)); return m; }

static void compareCase (const juce::String& name, const Audio& in, const Audio* key, bool scEnabled, const Settings& st,
                         const tutkal::Params& raw, double bpm, double minGr, double maxGrBound = 60.0)
{
    for (int block : { 512, 333 })
    {
        auto p = runPlugin (in, key, scEnabled, block, block, st, bpm);
        auto r = runRaw (in, scEnabled ? key : nullptr, block, raw);
        const double d = maxDiff (p.out, r.out), dg = maxGrDiff (p.grBlock, r.grBlock);
        check (allFinite (p.out) && d < 1e-5 && dg < 1e-5 && p.maxGr >= minGr && p.maxGr <= maxGrBound,
               name + " block " + juce::String (block),
               juce::String::formatted ("maxAbsDiff=%.3e  grMeterVsEngine=%.3e  meterMaxGR=%.2f dB  outPeak=%.3f  latency=%d",
                                        d, dg, p.maxGr, peakOf (p.out), p.latency));
    }
}


// ---- --controlproof: every knob in every variation (SvlControlProof.h), through the real processor ----------------
static int runControlProof (int argc, char** argv)
{
    namespace cp = svl::controlproof;
    using cp::feat::outPeakDb; using cp::feat::reductionDb; using cp::feat::outRmsDb; using cp::feat::wetRmsDb; using cp::feat::bandDb;
    cp::Config c;
    c.name = "TUTKAL";
    c.make = [] { return std::make_unique<TutkalAudioProcessor>(); };
    c.scBus = 1;                 // the sidechain bus is enabled and fed with the kick; corners also run with it disabled
    c.tailSeconds = 1.0;         // documented tail is 0 (63 samples of latency); 1.0 s is tested
    c.designedGainDb = [] (const cp::StateView& s) { return (double) s.raw ("makeup"); };   // MAKEUP adds up to +20 dB by design
    // voicing choice index: GLUE 0, LA 1, PUMP 2, CORE 3 (normalised 0, 1/3, 2/3, 1)
    const float kGlue = 0.0f, kLa = 1.0f / 3.0f, kPump = 2.0f / 3.0f, kCore = 1.0f;
    auto voicingIs = [] (std::initializer_list<int> v) { std::vector<int> vv (v); return cp::Cond ([vv] (const cp::StateView& s) { const int c = s.choice ("voicing"); for (int x : vv) if (x == c) return true; return false; }); };
    using Force = std::vector<std::pair<juce::String, float>>;
    // every declaration runs with IN on and MIX 100 % (otherwise the knob has nothing to act on) and, where a voicing owns the knob, with that voicing forced
    auto D = [] (const char* id, int dir, cp::Feature f, double tol, cp::Cond ap, const char* fname, const char* note, Force force = {}, int mat = cp::kDrums)
    {
        cp::Direction d; d.id = id; d.dir = dir; d.feature = f; d.tol = tol; d.applies = ap; d.featureName = fname; d.note = note; d.material = mat;
        d.force = { { "in", 1.0f }, { "mix", 1.0f } }; for (auto& x : force) d.force.push_back (x);
        return d;
    };
    c.notes["attack"] = "GLUE: ~no effect by v3 design (<= 0.2 dB peak over the knob); PUMP/LA: ignored by design";
    c.notes["ratio"] = "LA ignores it; PUMP clamps 10..20";
    c.directions = {
        D ("voicing", 0, nullptr, 0, nullptr, "", "four characters, not an ordered scale"),
        D ("threshold", -1, reductionDb(), 0.25, voicingIs ({ 0, 2, 3 }), "gain reduction (in rms - out rms, dB)", "threshold up = less reduction; LA ignores it (PEAK REDUCTION replaces it)"),
        D ("ratio", +1, reductionDb(), 0.25, voicingIs ({ 0, 2, 3 }), "gain reduction", "LA ignores RATIO; PUMP clamps it to 10..20 so it is flat below 10"),
        D ("attack", +1, outPeakDb(), 0.1, nullptr, "output peak (dB), CORE, DRIVE 0, MAKEUP 0", "slower attack lets more transient through, so the peak rises; tested in CORE (GLUE: the 0.8 s slow stage dominates, measured peak change <= 0.2 dB over the whole knob; PUMP clamps to 0.7 ms; LA ignores it)",
           { { "voicing", kCore }, { "threshold", 0.2f }, { "ratio", 0.7f }, { "autoRelease", 0.0f }, { "drive", 0.0f }, { "makeup", 0.0f } }),
        D ("release", +1, reductionDb(), 0.25, nullptr, "gain reduction, CORE, AUTO off", "longer release holds the reduction longer; AUTO replaces it, PUMP uses the note, LA ignores it",
           { { "voicing", kCore }, { "autoRelease", 0.0f } }),
        D ("release", +1, reductionDb(), 0.25, nullptr, "gain reduction, GLUE, AUTO off", "same in GLUE (fast stage release = the knob, slow stage max(1.6 s, 4 x knob))",
           { { "voicing", kGlue }, { "autoRelease", 0.0f } }),
        D ("autoRelease", 0, nullptr, 0, nullptr, "", "switch that changes the release law; no ordering"),
        D ("makeup", +1, outRmsDb(), 0.25, nullptr, "output rms (dB)", "output gain of the compressed path"),
        D ("mix", +1, wetRmsDb(), 0.25, nullptr, "wet part = output - delayed input (rms dB)", "0 is the delayed dry signal exactly"),
        D ("scHpf", -1, reductionDb(), 0.25, nullptr, "gain reduction, CORE", "tested in CORE only: in GLUE the AUTO end (200 Hz) sits below OFF so it is not monotone there by design; LA ignores it",
           { { "voicing", kCore }, { "threshold", 0.3f }, { "ratio", 0.7f } }),
        D ("ext", 0, nullptr, 0, nullptr, "", "switch: detector source"),
        D ("drive", +1, bandDb (400.0, 3000.0), 0.25, nullptr, "harmonic band 0.4-3 kHz power on the chord, no gain reduction", "output transformer saturation adds harmonics",
           { { "threshold", 1.0f }, { "peakReduction", 0.0f }, { "makeup", 0.0f } }, cp::kChord),
        D ("in", 0, nullptr, 0, nullptr, "", "bypass switch"),
        D ("peakReduction", +1, reductionDb(), 0.25, nullptr, "gain reduction, LA", "LA threshold = -45 dB x value", { { "voicing", kLa } }),
        D ("limit", +1, reductionDb(), 0.25, nullptr, "gain reduction, LA", "LIMIT = 12:1 against COMPRESS 3:1", { { "voicing", kLa }, { "peakReduction", 0.8f } }),
        D ("pumpNote", +1, reductionDb(), 0.25, nullptr, "gain reduction, PUMP", "a longer note value is a longer recovery, so more average reduction", { { "voicing", kPump }, { "threshold", 0.4f } }),
        D ("pumpShape", +1, reductionDb(), 0.25, nullptr, "gain reduction, PUMP", "choice order EXP < LINEAR < SWELL by the recovery formulas in SPEC.md (remaining duck at half a note: 0.32 / 0.55 / 0.78)", { { "voicing", kPump }, { "threshold", 0.4f } }),
        D ("depth", +1, reductionDb(), 0.25, nullptr, "gain reduction, PUMP", "the duck never goes deeper than DEPTH", { { "voicing", kPump }, { "threshold", 0.4f } }),
    };
    return cp::main (c, argc, argv);
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    for (int i = 1; i < argc; ++i)
        if (juce::String (argv[i]).startsWith ("--controlproof")) return runControlProof (argc, argv);
    juce::File dir = argc > 1 ? juce::File (argv[1])
                              : juce::File::getCurrentWorkingDirectory().getChildFile ("../prototypes/DINLE/10_TUTKAL_v3/_session");
    if (! dir.isDirectory()) { std::printf ("session dir not found: %s\n", dir.getFullPathName().toRawUTF8()); return 2; }
    auto drum = readWav (dir.getChildFile ("DRY_drumbus.wav"));
    auto vocal = readWav (dir.getChildFile ("DRY_vocal_in_backing.wav"));
    auto pad = readWav (dir.getChildFile ("DRY_padbass.wav"));
    auto kick = readWav (dir.getChildFile ("KEY_kick.wav"));
    std::printf ("files: drumbus %zu ch x %zu, vocal %zu ch x %zu, padbass %zu ch x %zu, kick %zu ch x %zu\n",
                 drum.size(), drum[0].size(), vocal.size(), vocal[0].size(), pad.size(), pad[0].size(), kick.size(), kick[0].size());

    // 1. GLUE on the drum bus: threshold -32.2, 4:1, attack 10, AUTO.
    {
        tutkal::Params p; p.voicing = tutkal::kGlue; p.thresholdDb = -32.2f; p.ratio = 4.0f; p.attackMs = 10.0f; p.autoRelease = true;
        compareCase ("GLUE drumbus", drum, nullptr, false,
                     { { "voicing", 0 }, { "threshold", -32.2f }, { "ratio", 4.0f }, { "attack", 10.0f }, { "autoRelease", 1 } },
                     p, -1.0, 1.0);
    }
    // 2. LA on the vocal-in-backing: runs, finite, compresses, equals the raw engine.
    {
        tutkal::Params p; p.voicing = tutkal::kLa; p.peakReduction = 0.5f;
        compareCase ("LA vocal_in_backing", vocal, nullptr, false, { { "voicing", 1 }, { "peakReduction", 0.5f } }, p, -1.0, 0.5);
    }
    // 3. PUMP on pad+bass keyed by the kick on the sidechain bus: 1/8, EXP, depth 10, 100 bpm from the host.
    {
        tutkal::Params p; p.voicing = tutkal::kPump; p.ratio = 10.0f; p.attackMs = 0.5f; p.thresholdDb = -30.0f; p.ext = true;
        p.bpm = 100.0f; p.pumpNote = 0.125f; p.pumpShape = 0; p.depthDb = 10.0f;
        compareCase ("PUMP padbass keyed by KEY_kick (sidechain bus, host 100 bpm)", pad, &kick, true,
                     { { "voicing", 2 }, { "ratio", 10.0f }, { "attack", 0.5f }, { "threshold", -30.0f }, { "ext", 1 },
                       { "pumpNote", 1 }, { "pumpShape", 0 }, { "depth", 10.0f } },
                     p, 100.0, 8.0, 10.2);
        // sidechain bus disabled, EXT on: the detector falls back to the programme (documented shell behaviour) == raw with ext=false.
        tutkal::Params q = p; q.ext = false;
        compareCase ("PUMP, EXT on but sidechain bus disabled -> keys from main input", pad, nullptr, false,
                     { { "voicing", 2 }, { "ratio", 10.0f }, { "attack", 0.5f }, { "threshold", -30.0f }, { "ext", 1 },
                       { "pumpNote", 1 }, { "pumpShape", 0 }, { "depth", 10.0f } },
                     q, 100.0, 0.0, 10.2);
        // no play-head at all: tempo falls back to 120
        tutkal::Params f = p; f.bpm = 120.0f;
        compareCase ("PUMP, no play-head -> 120 bpm", pad, &kick, true,
                     { { "voicing", 2 }, { "ratio", 10.0f }, { "attack", 0.5f }, { "threshold", -30.0f }, { "ext", 1 },
                       { "pumpNote", 1 }, { "pumpShape", 0 }, { "depth", 10.0f } },
                     f, -1.0, 8.0, 10.2);
    }
    // 4. Defaults of the plug-in (GLUE 4:1, 10 ms, AUTO) and the CORE voicing run finite.
    {
        auto d = runPlugin (drum, nullptr, false, 512, 512, {}, -1.0);
        check (allFinite (d.out) && d.latency == 63, "default settings: finite, latency 63", juce::String::formatted ("latency=%d meterMaxGR=%.2f dB", d.latency, d.maxGr));
        auto c = runPlugin (drum, nullptr, false, 512, 512, { { "voicing", 3 }, { "threshold", -30.0f }, { "drive", 50.0f }, { "scHpf", 100.0f } }, -1.0);
        tutkal::Params p; p.voicing = tutkal::kCore; p.thresholdDb = -30.0f; p.drive = 0.5f; p.scHpfHz = 100.0f; p.autoRelease = false;
        // default plug-in AUTO is on, so set raw the same
        p.autoRelease = true;
        auto r = runRaw (drum, nullptr, 512, p);
        check (allFinite (c.out) && maxDiff (c.out, r.out) < 1e-5, "CORE + DRIVE 50 % + SC HPF 100 Hz equals raw", juce::String::formatted ("maxAbsDiff=%.3e", maxDiff (c.out, r.out)));
    }
    // 5. A host block (10000) larger than the prepared block (256): chunked inside, same result.
    {
        tutkal::Params p; p.voicing = tutkal::kGlue; p.thresholdDb = -32.2f; p.ratio = 4.0f; p.attackMs = 10.0f; p.autoRelease = true;
        auto a = runPlugin (drum, nullptr, false, 10000, 256, { { "voicing", 0 }, { "threshold", -32.2f }, { "ratio", 4.0f }, { "attack", 10.0f }, { "autoRelease", 1 } }, -1.0);
        auto r = runRaw (drum, nullptr, 512, p);
        check (allFinite (a.out) && maxDiff (a.out, r.out) < 1e-5, "host block 10000 vs prepared 256 (chunked)", juce::String::formatted ("maxAbsDiff=%.3e", maxDiff (a.out, r.out)));
    }
    // 6. Mono main layout.
    {
        Audio mono { drum[0] };
        tutkal::Params p; p.voicing = tutkal::kGlue; p.thresholdDb = -32.2f; p.ratio = 4.0f; p.attackMs = 10.0f; p.autoRelease = true;
        auto a = runPlugin (mono, nullptr, false, 512, 512, { { "voicing", 0 }, { "threshold", -32.2f }, { "ratio", 4.0f }, { "attack", 10.0f }, { "autoRelease", 1 } }, -1.0);
        auto r = runRaw (mono, nullptr, 512, p);
        check (allFinite (a.out) && maxDiff (a.out, r.out) < 1e-5, "mono main layout equals raw (1 channel)", juce::String::formatted ("maxAbsDiff=%.3e", maxDiff (a.out, r.out)));
    }
    // 7. Bypass (host bypass): output == input delayed by 63 samples, exactly.
    for (int block : { 512, 333 })
    {
        auto b = runPlugin (drum, nullptr, false, block, block, {}, -1.0, true);
        double worst = 0.0; bool headZero = true;
        for (size_t c = 0; c < drum.size(); ++c)
            for (size_t i = 0; i < drum[c].size(); ++i)
            {
                const float expect = i >= 63 ? drum[c][i - 63] : 0.0f;
                worst = std::max (worst, (double) std::abs (b.out[c][i] - expect));
                if (i < 63 && b.out[c][i] != 0.0f) headZero = false;
            }
        check (worst == 0.0 && headZero, "bypass = input delayed 63 samples, block " + juce::String (block), juce::String::formatted ("maxAbsDiff=%.3e", worst));
    }
    // 7b. IN switch off (engine bypass) also reproduces the delayed input once its 5 ms fade has settled.
    {
        auto b = runPlugin (drum, nullptr, false, 512, 512, { { "in", 0 } }, -1.0);
        double worst = 0.0;
        for (size_t c = 0; c < drum.size(); ++c)
            for (size_t i = 48000; i < drum[c].size(); ++i) worst = std::max (worst, (double) std::abs (b.out[c][i] - drum[c][i - 63]));
        check (worst < 1e-6, "IN off = input delayed 63 samples (after the fade, from 1 s)", juce::String::formatted ("maxAbsDiff=%.3e", worst));
    }
    // 8. State round trip through getStateInformation / setStateInformation.
    {
        const std::pair<const char*, float> vals[] = { { "voicing", 2 }, { "threshold", -27.3f }, { "ratio", 7.7f }, { "attack", 0.8f }, { "release", 777.0f },
                                                       { "autoRelease", 0 }, { "makeup", 6.5f }, { "mix", 42.0f }, { "scHpf", 135.0f }, { "ext", 1 },
                                                       { "drive", 33.0f }, { "in", 0 }, { "peakReduction", 0.731f }, { "limit", 1 }, { "pumpNote", 3 },
                                                       { "pumpShape", 2 }, { "depth", 17.5f } };
        TutkalAudioProcessor a, b;
        for (auto& kv : vals) setParam (a.apvts, kv.first, kv.second);
        juce::MemoryBlock mb; a.getStateInformation (mb);
        b.setStateInformation (mb.getData(), (int) mb.getSize());
        int bad = 0; double worst = 0.0;
        for (auto& kv : vals)
        {
            const float va = a.apvts.getRawParameterValue (kv.first)->load(), vb = b.apvts.getRawParameterValue (kv.first)->load();
            if (va != vb) ++bad;
            worst = std::max (worst, (double) std::abs (va - vb));
        }
        TutkalAudioProcessor fresh;
        int changed = 0;
        for (auto& kv : vals) if (fresh.apvts.getRawParameterValue (kv.first)->load() != a.apvts.getRawParameterValue (kv.first)->load()) ++changed;
        check (bad == 0 && changed == 17, "state round trip (17 parameters)", juce::String::formatted ("mismatches=%d  maxDiff=%.1e  parameters differing from defaults before restore=%d  state bytes=%zu", bad, worst, changed, mb.getSize()));
    }
    // 9. Layouts.
    {
        TutkalAudioProcessor a;
        auto l = a.getBusesLayout();
        const bool defaultSc = ! a.getBus (true, 1)->isEnabled();
        check (defaultSc && a.getBusCount (true) == 2 && a.getBus (true, 1)->getName() == "Sidechain", "sidechain bus 'Sidechain' exists, disabled by default");
        auto mono = l; mono.inputBuses.set (0, juce::AudioChannelSet::mono()); mono.outputBuses.set (0, juce::AudioChannelSet::mono());
        auto bad = l; bad.inputBuses.set (0, juce::AudioChannelSet::mono());   // mono in, stereo out
        check (a.checkBusesLayoutSupported (mono) && ! a.checkBusesLayoutSupported (bad), "mono/mono accepted, mono-in/stereo-out refused");
    }

    {   // 10. Value text: what the host shows (Live's device view, automation) is the string the panel shows, and reads back.
        TutkalAudioProcessor a;
        auto txt = [&] (const char* id, float plain)
        {
            auto* p = dynamic_cast<juce::RangedAudioParameter*> (a.apvts.getParameter (id));
            return p->getText (p->convertTo0to1 (plain), 64);
        };
        auto fromText = [&] (const char* id, const juce::String& t)
        {
            auto* p = dynamic_cast<juce::RangedAudioParameter*> (a.apvts.getParameter (id));
            return p->convertFrom0to1 (p->getValueForText (t));
        };
        struct Ex { const char* id; float plain; const char* text; };
        const Ex shown[] = {
            { "threshold", -18.0f, "-18.0 dB" }, { "threshold", -40.0f, "-40.0 dB" }, { "threshold", 0.0f, "0.0 dB" },
            { "ratio", 4.0f, "4.0 : 1" }, { "ratio", 20.0f, "20.0 : 1" }, { "ratio", 1.5f, "1.5 : 1" },
            { "attack", 10.0f, "10.0 ms" }, { "attack", 0.1f, "0.10 ms" }, { "attack", 30.0f, "30.0 ms" },
            { "release", 300.0f, "0.30 s" }, { "release", 1200.0f, "1.20 s" }, { "release", 100.0f, "0.10 s" },
            { "makeup", 6.0f, "6.0 dB" }, { "depth", 10.0f, "10.0 dB" },
            { "mix", 100.0f, "100 %" }, { "mix", 47.5f, "47.5 %" }, { "drive", 0.0f, "0 %" },
            { "peakReduction", 0.5f, "5.0" }, { "peakReduction", 1.0f, "10.0" },
            { "scHpf", 0.0f, "AUTO" }, { "scHpf", 10.0f, "OFF" }, { "scHpf", 200.0f, "200 Hz" },
            { "pumpNote", 1.0f, "1/8" }, { "pumpNote", 3.0f, "1/2" }, { "pumpShape", 2.0f, "SWELL" }, { "pumpShape", 1.0f, "LINEAR" } };
        for (const auto& e : shown)
        {
            const auto got = txt (e.id, e.plain);
            check (got == e.text, juce::String ("host text ") + e.id + " " + juce::String (e.plain), "\"" + got + "\" want \"" + e.text + "\"");
        }
        // Every numeric parameter: the host's text at min / default / max reads back to the same normalised value (within the display's own rounding).
        for (auto* ap : a.getParameters())
        {
            auto* rp = dynamic_cast<juce::RangedAudioParameter*> (ap);
            auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (ap);
            if (rp == nullptr || withId == nullptr || rp->isBoolean()) continue;
            if (withId->paramID == "voicing") continue;
            for (float n : { 0.0f, rp->getDefaultValue(), 1.0f })
            {
                const auto t = rp->getText (n, 64);
                const float back = rp->getValueForText (t);
                const float tol = rp->isDiscrete() ? 1.0e-6f : 0.006f;
                check (t.isNotEmpty() && ! t.containsIgnoreCase ("nan") && std::abs (back - n) <= tol,
                       "text round trip " + withId->paramID + " @" + juce::String (n, 3), "\"" + t + "\" -> " + juce::String (back, 4));
            }
        }
        // Typed forms the panel and the host accept.
        auto near = [] (float a, float b, float tol = 1.0e-3f) { return std::abs (a - b) <= tol; };
        check (near (fromText ("threshold", "-22 dB"), -22.0f) && near (fromText ("threshold", "-22"), -22.0f)
                   && near (fromText ("threshold", juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92" "22 dB"))), -22.0f),
               "typed threshold: -22 dB / -22 / minus-sign -22 dB");
        check (near (fromText ("threshold", "-99"), -40.0f) && near (fromText ("threshold", "12"), 0.0f), "typed threshold is clamped to the range");
        check (near (fromText ("ratio", "6:1"), 6.0f) && near (fromText ("ratio", "6"), 6.0f) && near (fromText ("ratio", "6 : 1"), 6.0f), "typed ratio: 6:1 / 6 / 6 : 1");
        check (near (fromText ("attack", "25 ms"), 25.0f) && near (fromText ("attack", "0.002 s"), 2.0f) && near (fromText ("attack", "1.5"), 1.5f), "typed attack: ms, s, bare = ms");
        check (near (fromText ("release", "0.4 s"), 400.0f) && near (fromText ("release", "400 ms"), 400.0f) && near (fromText ("release", "0.4"), 400.0f) && near (fromText ("release", "400"), 400.0f),
               "typed release: 0.4 s / 400 ms / 0.4 / 400");
        check (near (fromText ("scHpf", "150 Hz"), 150.0f) && near (fromText ("scHpf", "auto"), 0.0f) && near (fromText ("scHpf", "OFF"), 10.0f) && near (fromText ("scHpf", "200"), 200.0f),
               "typed SC HPF: 150 Hz / auto / OFF / 200");
        check (near (fromText ("peakReduction", "7"), 0.7f) && near (fromText ("mix", "50 %"), 50.0f) && near (fromText ("drive", "25"), 25.0f), "typed PEAK REDUCTION 7 (= 0.7), MIX 50 %, DRIVE 25");
        check (near (fromText ("pumpNote", "1/4"), 2.0f) && near (fromText ("pumpNote", "1/16"), 0.0f) && near (fromText ("pumpShape", "swell"), 2.0f) && near (fromText ("pumpShape", "lin"), 1.0f),
               "typed NOTE 1/4, 1/16, SHAPE swell, lin");
        float junk = 0.0f;
        int ji = 0;
        check (! tutkal::ptext::parse ("threshold", "banana", junk) && ! tutkal::ptext::parse ("threshold", "12 apples", junk)
                   && ! tutkal::ptext::parse ("ratio", "", junk) && ! tutkal::ptext::parse ("attack", "-", junk) && ! tutkal::ptext::parse ("mix", "50 ms", junk)
                   && ! tutkal::ptext::parseChoice ("pumpNote", "1/3", ji) && ! tutkal::ptext::parseChoice ("pumpShape", "zzz", ji),
               "garbage is refused by the strict parser (words, wrong unit, empty, lone sign)");
        for (auto* ap : a.getParameters())
            if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (ap))
                check (ap->getLabel().isEmpty() || withId->paramID == "voicing", "no separate unit label on " + withId->paramID + " (the unit is in the text; a host would print it twice)");
    }

    {   // Cmd+Z / Cmd+Shift+Z (SvlUndoTests.h). TUTKAL has no preset / mood recall, so the "recall" is a Group around several writes.
        const auto u = svl::undotest::run<TutkalAudioProcessor> ("TUTKAL", [] { return std::make_unique<TutkalAudioProcessor>(); },
                                                                 { { "threshold", -22.5f }, { "ratio", 8.0f }, { "makeup", 6.5f } },
                                                                 [] (TutkalAudioProcessor& p)
                                                                 {
                                                                     svl::ParamUndo::Group oneStep (p.undo, "Test recall");
                                                                     setParam (p.apvts, "voicing", 2.0f);
                                                                     setParam (p.apvts, "attack", 2.5f);
                                                                     setParam (p.apvts, "release", 640.0f);
                                                                     setParam (p.apvts, "depth", 17.0f);
                                                                 });
        gPass += u.pass;
        gFail += u.fail;
        check (u.fail == 0, "undo_redo: " + juce::String (u.pass) + " of " + juce::String (u.pass + u.fail) + " undo checks passed");
    }

    std::printf ("\nplugin_check: %d passed, %d failed\n", gPass, gFail);
    return gFail == 0 ? 0 : 1;
}
