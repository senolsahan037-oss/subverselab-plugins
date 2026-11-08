// plugin_check: the real DyneqAudioProcessor, offline, against the raw engine and on the demo material.
//   plugin_check [demo_material_dir]      (default tools/demo_material, relative to the project)
//   plugin_check --controlproof [options]  every knob in every variation (tests/SvlControlProof.h), through the real processor
// Exit code 0 only if every check passed.
#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"
#include "../Source/ParamText.h"
#include "../Source/PluginEditor.h"
#if JUCE_MAC
 #include <mach/mach.h>
 #include <malloc/malloc.h>
#endif
#include "SvlUndoTests.h"
#include "SvlControlProof.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <chrono>
#include <random>

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

static Audio slice (const Audio& a, int from, int len)
{
    Audio o (a.size());
    for (size_t c = 0; c < a.size(); ++c) o[c].assign (a[c].begin() + from, a[c].begin() + from + juce::jmin (len, (int) a[c].size() - from));
    return o;
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

static float rawOf (juce::AudioProcessorValueTreeState& s, const char* id) { return s.getRawParameterValue (id)->load(); }

struct TestPlayHead : juce::AudioPlayHead
{
    bool playing = true;
    juce::Optional<PositionInfo> getPosition() const override { PositionInfo i; i.setBpm (120.0); i.setIsPlaying (playing); return i; }
};

// A processor in a host-like setup: stereo (or mono) main, optional stereo sidechain bus, a play-head.
struct Rig
{
    DyneqAudioProcessor proc;
    TestPlayHead ph;
    int mainCh; bool sc; double sr;
    Rig (int mainChannels, bool scEnabled, double sampleRate, int prepBlock, const Settings& st = {}, bool withPlayHead = true)
        : mainCh (mainChannels), sc (scEnabled), sr (sampleRate)
    {
        const auto mainSet = mainCh == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();
        auto layout = proc.getBusesLayout();
        layout.inputBuses.set (0, mainSet); layout.outputBuses.set (0, mainSet);
        layout.inputBuses.set (1, sc ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::disabled());
        if (! proc.setBusesLayout (layout)) { std::printf ("setBusesLayout refused\n"); std::exit (2); }
        proc.setRateAndBufferSizeDetails (sr, prepBlock);
        proc.prepareToPlay (sr, prepBlock);
        for (auto& kv : st) setParam (proc.apvts, kv.first, kv.second);
        if (withPlayHead) proc.setPlayHead (&ph);
    }
    // the whole signal in host blocks of `block`; key (stereo) goes on the sidechain bus when the rig has one
    Audio render (const Audio& in, const Audio* key, int block, bool bypassed = false)
    {
        const int n = (int) in[0].size();
        const int total = juce::jmax (proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
        Audio out ((size_t) mainCh, std::vector<float> ((size_t) n));
        juce::AudioBuffer<float> buf (total, block);
        juce::MidiBuffer midi;
        for (int pos = 0; pos < n; pos += block)
        {
            const int len = juce::jmin (block, n - pos);
            buf.setSize (total, len, false, false, true);
            buf.clear();
            for (int c = 0; c < mainCh; ++c) std::copy (in[(size_t) c].begin() + pos, in[(size_t) c].begin() + pos + len, buf.getWritePointer (c));
            if (sc && key)
                for (int c = 0; c < 2 && mainCh + c < total; ++c)
                    std::copy ((*key)[(size_t) c].begin() + pos, (*key)[(size_t) c].begin() + pos + len, buf.getWritePointer (mainCh + c));
            if (bypassed) proc.processBlockBypassed (buf, midi); else proc.processBlock (buf, midi);
            for (int c = 0; c < mainCh; ++c) std::copy (buf.getReadPointer (c), buf.getReadPointer (c) + len, out[(size_t) c].begin() + pos);
        }
        return out;
    }
};

static Audio runPlugin (const Audio& in, const Audio* key, bool scEnabled, int block, const Settings& st, double sr = 48000.0, bool bypassed = false)
{
    Rig r ((int) in.size(), scEnabled, sr, block, st);
    return r.render (in, key, block, bypassed);
}

// The same parameters the plug-in has right now, as the engine's Params (an independent reading of the mapping in readParams()).
static dyneq::Params paramsOf (juce::AudioProcessorValueTreeState& s)
{
    dyneq::Params p;
    auto r = [&] (const juce::String& id) { return s.getRawParameterValue (id)->load(); };
    p.mode = juce::roundToInt (r ("mode")); p.depthDb = r ("depth"); p.marginDb = r ("margin");
    if (p.mode == dyneq::kCollide) p.marginDb = std::min (p.marginDb, 9.0f);
    p.focusLowHz = r ("focusLow"); p.focusHighHz = r ("focusHigh"); p.speed = juce::roundToInt (r ("speed")); p.mix = 1.0f; p.delta = false; p.tiling = r ("tiling") > 0.5f;
    p.targetInst = juce::roundToInt (r ("targetInst")); p.targetPart = r ("targetPart"); p.targetAmount = r ("targetAmount"); p.targetWidth = r ("targetWidth"); p.targetSweet = r ("targetSweet"); p.reverbReady = r ("reverbReady") > 0.5f; p.targetSlope = dyneq::kSlopeDb[juce::roundToInt (r ("targetSlope"))];
    for (int i = 0; i < dyneq::kFreeBands; ++i)
    {
        const juce::String n = "b" + juce::String (i + 1) + "_"; auto& b = p.band[i];
        b.on = r (n + "on") > 0.5f; b.type = juce::roundToInt (r (n + "type")); b.freqHz = r (n + "freq"); b.gainDb = r (n + "gain"); b.q = r (n + "q");
        b.slopeDb = dyneq::freeSlopeDbFromIndex (juce::roundToInt (r (n + "slope"))); b.dyn = r (n + "dyn") > 0.5f; b.thrDb = r (n + "thr"); b.rangeDb = r (n + "range");
        b.attackMs = r (n + "att"); b.releaseMs = r (n + "rel"); b.trigger = r (n + "trig") > 0.5f ? dyneq::kTrigKey : dyneq::kTrigSelf;
    }
    return p;
}

// The raw engine, block by block, same Params, no plug-in layer.
static Audio runRaw (const Audio& in, const Audio* keyIn, int block, const dyneq::Params& p, double sr = 48000.0, dyneq::Engine* keep = nullptr, bool bake = false)
{
    auto e = std::make_unique<dyneq::Engine>();
    dyneq::Engine& eng = keep ? *keep : *e;
    eng.prepare (sr, block);
    eng.setParams (p);
    eng.bakeReset(); eng.bakeCollect (bake);
    Audio out = in;
    const int nch = (int) in.size(), n = (int) in[0].size();
    for (int pos = 0; pos < n; pos += block)
    {
        const int len = juce::jmin (block, n - pos);
        float* io[2]; const float* key[2];
        for (int c = 0; c < nch; ++c) io[c] = out[(size_t) c].data() + pos;
        if (keyIn) for (int c = 0; c < 2; ++c) key[c] = (*keyIn)[(size_t) c].data() + pos;
        eng.process (io, keyIn ? key : nullptr, nch, len);
    }
    return out;
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
static double peakOf (const Audio& a) { double m = 0; for (auto& c : a) for (float v : c) m = std::max (m, (double) std::abs (v)); return m; }
static double rmsDb (const Audio& a, int from, int to)
{
    double s = 0; long n = 0;
    for (auto& c : a) for (int i = from; i < to; ++i) { s += (double) c[(size_t) i] * c[(size_t) i]; ++n; }
    return 10.0 * std::log10 (s / (double) std::max (n, 1L) + 1e-30);
}

static Audio sineAudio (double f, double levelDb, double secs, double sr = 48000.0)
{
    const int n = (int) (secs * sr);
    Audio a (2, std::vector<float> ((size_t) n));
    for (int i = 0; i < n; ++i) { const float v = (float) (std::pow (10.0, levelDb / 20.0) * std::sin (2.0 * juce::MathConstants<double>::pi * f * (double) i / sr)); a[0][(size_t) i] = a[1][(size_t) i] = v; }
    return a;
}

// every parameter, normalised and raw, bit patterns: for "restores the bands bit-exact"
struct Snap { std::vector<uint32_t> norm, raw; };
static uint32_t bitsOf (float f) { uint32_t u; std::memcpy (&u, &f, 4); return u; }
static Snap snapOf (DyneqAudioProcessor& p)
{
    Snap s;
    for (auto* ap : p.getParameters())
        if (auto* id = dynamic_cast<juce::AudioProcessorParameterWithID*> (ap))
        {
            s.norm.push_back (bitsOf (ap->getValue()));
            s.raw.push_back (bitsOf (p.apvts.getRawParameterValue (id->paramID)->load()));
        }
    return s;
}
static int snapDiff (const Snap& a, const Snap& b, DyneqAudioProcessor* names = nullptr)
{
    int n = 0;
    for (size_t i = 0; i < a.norm.size(); ++i)
        if (a.norm[i] != b.norm[i] || a.raw[i] != b.raw[i])
        {
            ++n;
            if (names != nullptr)
                if (auto* id = dynamic_cast<juce::AudioProcessorParameterWithID*> (names->getParameters()[(int) i]))
                    std::printf ("   differs: %s norm %08x/%08x raw %08x/%08x\n", id->paramID.toRawUTF8(), a.norm[i], b.norm[i], a.raw[i], b.raw[i]);
        }
    return n;
}

// ---- --controlproof: every knob in every variation (SvlControlProof.h), through the real processor ----------------
static int runControlProof (int argc, char** argv)
{
    namespace cp = svl::controlproof;
    using cp::feat::outRmsDb; using cp::feat::reductionDb; using cp::feat::wetRmsDb; using cp::feat::bandDb;
    cp::Config c;
    c.name = "ADALET";
    c.make = [] { return std::make_unique<DyneqAudioProcessor>(); };
    c.scBus = 1;                 // the sidechain bus is enabled and fed with the kick; corners also run with it disabled
    c.tailSeconds = 1.0;         // documented tail 0 (zero latency, minimum-phase bells); 1.0 s is tested
    // P1j: the designed gain of a state = the sum, over the bands that are on, of the highest point of that band's own response (RBJ biquad, the formula A11 holds the engine to): a boost of G dB
    // with a high Q overshoots G (a resonant shelf or bell peaks above its gain), so a plain "G" undercounts it; stacked linear bands cannot exceed the sum of their peaks. A dynamic band adds RANGE up to +12.
    auto bandPeakDb = [] (int type, double f0, double gDb, double q)
    {
        if (type > 2 || std::abs (gDb) < 1.0e-6) return std::max (0.0, gDb);   // a notch / cut never boosts; a flat band is 0 dB
        const double sr = 44100.0, A = std::pow (10.0, gDb / 40.0), w0 = 2.0 * 3.14159265358979 * std::min (f0, 0.45 * sr) / sr, al = std::sin (w0) / (2.0 * q), cs = std::cos (w0), sA = 2.0 * std::sqrt (A) * al;
        double b0, b1, b2, a0, a1, a2;
        if (type == 0) { b0 = 1 + al * A; b1 = -2 * cs; b2 = 1 - al * A; a0 = 1 + al / A; a1 = -2 * cs; a2 = 1 - al / A; }
        else if (type == 1) { b0 = A * ((A + 1) - (A - 1) * cs + sA); b1 = 2 * A * ((A - 1) - (A + 1) * cs); b2 = A * ((A + 1) - (A - 1) * cs - sA); a0 = (A + 1) + (A - 1) * cs + sA; a1 = -2 * ((A - 1) + (A + 1) * cs); a2 = (A + 1) + (A - 1) * cs - sA; }
        else { b0 = A * ((A + 1) + (A - 1) * cs + sA); b1 = -2 * A * ((A - 1) + (A + 1) * cs); b2 = A * ((A + 1) + (A - 1) * cs - sA); a0 = (A + 1) - (A - 1) * cs + sA; a1 = 2 * ((A - 1) - (A + 1) * cs); a2 = (A + 1) - (A - 1) * cs - sA; }
        double best = 0.0;
        for (int k = 0; k <= 600; ++k)
        {
            const double w = 3.14159265358979 * std::pow (10.0, -3.0 + 3.0 * k / 600.0), c1 = std::cos (w), s1 = std::sin (w), c2 = std::cos (2 * w), s2 = std::sin (2 * w);
            const double nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2), dr = a0 + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2);
            best = std::max (best, 10.0 * std::log10 ((nr * nr + ni * ni) / std::max (dr * dr + di * di, 1.0e-30)));
        }
        return best;
    };
    c.designedGainDb = [bandPeakDb] (const cp::StateView& s)
    {
        double g = 0.0;   // free bands boost up to +18 dB each by design (bell / shelves, plus RANGE up to +12 when dynamic)
        for (int i = 1; i <= 6; ++i)
        {
            const juce::String n = "b" + juce::String (i) + "_";
            if (! s.on (n + "on")) continue;
            const double gain = (double) s.raw (n + "gain") + (s.on (n + "dyn") ? juce::jmax (0.0, (double) s.raw (n + "range")) : 0.0);
            g += bandPeakDb (s.choice (n + "type"), (double) s.raw (n + "freq"), gain, (double) s.raw (n + "q"));
        }
        // 2026-10-26 (ControlProof check 7, random 183, chord: DELTA + OUTPUT +12 dB peaked 13.36 dBFS): DELTA plays dry - wet, and |dry - wet| <= |dry| + |wet|, so a cut
        // that shifts the phase (an edge cut, a high cut, TARGET's REVERB-READY) can make the difference louder than the input by up to 20 log10 (1 + 10^(g / 20)) dB
        // (+6 dB at g = 0); the OUTPUT crown then adds its own gain (up to +12 dB). Both are designed gain the bound did not count; the engine was not at fault.
        if (s.on ("delta")) g = 20.0 * std::log10 (1.0 + std::pow (10.0, g / 20.0));
        g += juce::jmax (0.0, (double) s.raw ("output"));
        return g;
    };
    // KEY LISTEN hands out the sidechain signal by design: the key keeps sounding after the main input stops, and a silent main is not a silent output
    // P1j: a free band that resonates (a bell / shelf with a boost or cut of 3 dB or more, or a notch) rings for as long as its poles say. Estimate: -100 dB after 11.5 time constants, a time constant of
    // max (Q, 0.5 / Q) x 10^(|G| / 40) / (pi f0) (the Q term is the resonant pair, the 0.5 / Q term the slow real pole of a low-Q shelf, the gain term the shelf's pole moving down with the boost; checked against
    // the 7 stuck states of the P1i run and the ones of the P1j list). When that is longer than 0.7 s the state sustains by design: the ring is the user's own band, the same biquad in P1h and before;
    // the tail must stay bounded, the decay is not judged.
    c.selfSustains = [] (const cp::StateView& s)
    {
        if (s.on ("keyListen")) return true;
        for (int i = 1; i <= 6; ++i)
        {
            const juce::String n = "b" + juce::String (i) + "_";
            if (! s.on (n + "on")) continue;
            const int type = s.choice (n + "type"); const double gdb = std::abs ((double) s.raw (n + "gain")), q = (double) s.raw (n + "q"), f0 = (double) s.raw (n + "freq");
            if (type == 4 || type == 5)   // P1n: the last stage of a cut follows Q, so a cut with Q above its Butterworth value rings like a resonator (stage Q = the Butterworth value of the slope x Q)
            {
                static const double qLast[5] = { 0.70710678118654752, 1.30656296487637653, 2.56291544774150551, 1.93185165257813657, 3.83064878777013 };   // SLOPE choice index 12 / 24 / 48 / 36 / 72
                const double qs = qLast[juce::jlimit (0, 4, s.choice (n + "slope"))] * q;
                if (11.5 * std::max (qs, 0.5 / qs) / (3.14159265358979 * f0) > 0.7) return true;
                continue;
            }
            if (type > 3 || (type < 3 && gdb < 3.0)) continue;   // a flat bell / shelf does nothing
            if (11.5 * std::max (q, 0.5 / q) * std::pow (10.0, (type < 3 ? gdb : 0.0) / 40.0) / (3.14159265358979 * f0) > 0.7) return true;
        }
        return false;
    };
    c.noiseFloorDb = [] (const cp::StateView& s) { return s.on ("keyListen") ? 6.0 : -90.0; };
    using Force = std::vector<std::pair<juce::String, float>>;
    // normalised values: choice indices are 0, 1/(n-1) ..; the bool "on" is 1.
    auto bandsOff = [] { Force f; for (int i = 1; i <= 6; ++i) f.push_back ({ "b" + juce::String (i) + "_on", 0.0f }); return f; };
    auto D = [] (const char* id, int dir, cp::Feature f, double tol, cp::Cond ap, const char* fname, const char* note, Force force = {}, int mat = cp::kDrums)
    {
        cp::Direction d; d.id = id; d.dir = dir; d.feature = f; d.tol = tol; d.applies = ap; d.featureName = fname; d.note = note; d.material = mat;
        // OUTPUT at 0 dB and ASSIST OFF dark: the trim would turn the mix direction around
        d.force = { { "mix", 1.0f }, { "delta", 0.0f }, { "bypass", 0.0f }, { "keyListen", 0.0f }, { "output", 0.5f }, { "assistOff", 0.0f } }; for (auto& x : force) d.force.push_back (x);
        return d;
    };
    auto cat = [] (Force a, const Force& b) { a.insert (a.end(), b.begin(), b.end()); return a; };
    // COLLIDE with every band off, a wide FOCUS and the largest MARGIN: the kick key against drums that contain that kick
    const Force collide = [&] { Force f = bandsOff(); f.insert (f.end(), { { "mode", 0.0f }, { "margin", 0.75f }, { "depth", 1.0f }, { "focusLow", 0.0f }, { "focusHigh", 1.0f }, { "speed", 0.5f }, { "tiling", 0.0f }, { "bakeArm", 0.0f }, { "targetInst", 0.0f } }); return f; }();   // TARGET off: since P1h it already cuts after the first hit of the 1 s proof material (SPEC 8d 6), and a REVERB-READY band-limit would sit inside the COLLIDE "gain reduction" being judged
    const Force targetOn = [&] { Force f = bandsOff(); f.insert (f.end(), { { "mode", 0.0f }, { "depth", 0.0f }, { "bakeArm", 0.0f }, { "tiling", 0.0f }, { "targetInst", 0.6667f }, { "targetPart", 0.1f }, { "targetAmount", 1.0f }, { "targetWidth", 0.5f }, { "reverbReady", 0.0f } }); return f; }();
    c.notes["mode"] = "two characters (COLLIDE needs the key, SELF hunts resonances), not an ordered scale";
    c.notes["delta"] = "switch: hears what is removed";
    c.notes["bypass"] = "switch: the plug-in's own bypass";
    c.notes["keyListen"] = "switch: solos the sidechain";
    c.notes["tiling"] = "switch: analysis A/B (SPEC 7.3), changes only which bands are cut";
    c.notes["bakeArm"] = "does NOT change the audio by design: it only arms the statistics";
    c.notes["targetInst"] = "seven characters (OFF, VOCAL, BASS, DRUMS, KICK, SNARE, HATS), not an ordered scale";
    c.notes["targetPart"] = "steps through the parts of the chosen instrument (KICK: SUB, BODY, CLICK ...): which band is cut, not an ordered level";
    c.notes["targetWidth"] = "scales the cut width: a wider bell cuts more of the neighbouring bands, but on the drum loop the effect on the rms is not monotone";
    c.notes["reverbReady"] = "switch (SNARE, VOCAL): replaces the part cut by a static band-limit from the learned body (a low corner under it, a high corner over the crack, both Linkwitz-Riley at the SLOPE)";
    c.notes["targetSlope"] = "five steps of the edge cuts' steepness: the crossover (and so the band that is cut) moves with it, not an ordered level of the whole signal";
    c.notes["targetSweet"] = "the release of the cut after each hit: changes the shape in time, not an ordered level";
    c.directions = {
        D ("mode", 0, nullptr, 0, nullptr, "", "two characters, not an ordered scale"),
        D ("depth", +1, reductionDb(), 0.25, nullptr, "gain reduction (in rms - out rms, dB)", "the largest cut in any band", cat (collide, { { "depth", 0.0f } })),
        // 26 Oct (SPEC 10): the proof's key is a kick. Its fundamental is ducked by the low-end rule by DEPTH whoever leads, which MARGIN does not change by design
        // (span 0.24 dB with FOCUS open), and above 245 Hz the kick holds almost nothing (span 0.04 dB). MARGIN's direction is measured by tools/measure.py A3m
        // (a 1 kHz overlap at MARGIN 0 / 3 / 6), as TARGET AMOUNT's is by section 3b.
        D ("margin", 0, nullptr, 0, nullptr, "", "the key must lead by MARGIN dB; with the proof's kick key the low-end rule does not depend on it: direction checked by tools/measure.py A3m"),
        D ("focusLow", -1, reductionDb(), 0.25, nullptr, "gain reduction", "bands below FOCUS LOW are never cut", cat (collide, { { "focusLow", 0.0f } })),
        D ("focusHigh", +1, reductionDb(), 0.25, nullptr, "gain reduction", "bands above FOCUS HIGH are never cut", cat (collide, { { "focusHigh", 1.0f } })),
        D ("speed", 0, nullptr, 0, nullptr, "", "release 40 / 120 / 300 ms of the analysis and of the cut: changes the shape in time, not an ordered level"),
        D ("mix", +1, wetRmsDb(), 0.25, nullptr, "wet part = output - input (rms dB)", "0 is the input exactly", collide),
        D ("delta", 0, nullptr, 0, nullptr, "", "switch"), D ("bypass", 0, nullptr, 0, nullptr, "", "switch"), D ("keyListen", 0, nullptr, 0, nullptr, "", "switch"),
        D ("tiling", 0, nullptr, 0, nullptr, "", "switch"), D ("bakeArm", 0, nullptr, 0, nullptr, "", "does not change the audio"),
        D ("targetInst", 0, nullptr, 0, nullptr, "", "seven characters, not an ordered scale"),
        D ("targetPart", 0, nullptr, 0, nullptr, "", "steps through the parts of the instrument, not an ordered level"),
        D ("targetAmount", 0, nullptr, 0, nullptr, "", "the cut needs 3 learned hits, the proof's material is 1 s long (2 kicks): direction checked in plugin_check section 3b on the 16 s beat instead"),
        D ("targetWidth", 0, nullptr, 0, nullptr, "", "scales the cut width (Q / width); not an ordered level on the drum loop"),
        D ("targetSweet", 0, nullptr, 0, nullptr, "", "the release of the cut: changes the shape in time, not an ordered level"),
        D ("reverbReady", 0, nullptr, 0, nullptr, "", "switch: the part cut becomes a static band-limit (SNARE, VOCAL)"),
        D ("output", +1, outRmsDb(), 0.25, nullptr, "output rms (dB)", "the OUTPUT crown: a gain after everything, -12 .. +12 dB", collide),
        D ("assistOff", 0, nullptr, 0, nullptr, "", "switch: neither COLLIDE nor SELF cuts (their lamp is dark)"),
        D ("levelMatch", 0, nullptr, 0, nullptr, "", "switch: BYPASS plays the dry signal at the loudness of the processed one (it acts only while BYPASS is on)"),
        D ("analyzer", 0, nullptr, 0, nullptr, "", "the display's analyzer (OFF / PRE / POST / PRE+POST): saved, not automatable, never changes the audio (plugin_check 12 proves the bit-identity)"),
        D ("targetSlope", 0, nullptr, 0, nullptr, "", "five steps of the same cut (12 / 24 / 36 / 48 / 72 dB per octave of the SUB, CLICK and REVERB-READY edges): the cut needs learned hits like AMOUNT, direction and exactness are measured in tools/measure.py T22 / T23"),
    };
    for (int i = 1; i <= 6; ++i)
    {
        const juce::String n = "b" + juce::String (i) + "_";
        // This band alone: every other band off, the bank off (DEPTH 0), MODE COLLIDE, static bell at 1 kHz unless a declaration says otherwise.
        auto alone = [&] (Force more)
        {
            Force f = bandsOff();
            f.insert (f.end(), { { n + "on", 1.0f }, { "mode", 0.0f }, { "depth", 0.0f }, { "bakeArm", 0.0f }, { "tiling", 0.0f }, { n + "dyn", 0.0f }, { n + "type", 0.0f }, { n + "freq", 0.667f }, { n + "gain", 0.8333f }, { n + "q", 0.5f } });
            f.insert (f.end(), more.begin(), more.end());
            return f;
        };
        // normalised: freq 0.667 ~ 1 kHz (skew centre 1 kHz sits at 0.5, so use the plain value through "alone" below); gain 0.8333 = +6 dB; Q 0.5 = 1
        auto add = [&] (const char* name, int dir, cp::Feature f, double tol, const char* fname, const char* note, Force force, int mat, double floorValue = -1.0e9)
        {
            auto d = D ((n + name).toRawUTF8(), dir, f, tol, nullptr, fname, note, alone (force), mat); d.floorValue = floorValue; c.directions.push_back (d);
        };
        const Force dynOn = { { n + "dyn", 1.0f } };
        c.directions.push_back (D ((n + "on").toRawUTF8(), 0, nullptr, 0, nullptr, "", "switch: a band that is off does not touch the audio"));
        c.directions.push_back (D ((n + "type").toRawUTF8(), 0, nullptr, 0, nullptr, "", "six filter shapes, not an ordered scale"));
        c.directions.push_back (D ((n + "dyn").toRawUTF8(), 0, nullptr, 0, nullptr, "", "switch: the dynamic section on / off (A12 and A12b in tools/measure.py prove what it does)"));
        c.directions.push_back (D ((n + "trig").toRawUTF8(), 0, nullptr, 0, nullptr, "", "switch: the band's own input or the sidechain key as the dynamic trigger (A12 proves both)"));
        // FREQ: a high cut that moves up lets more of the pink noise through
        add ("freq", +1, outRmsDb(), 0.25, "output rms (dB), type HIGH CUT 12 dB/oct", "a high cut moved up passes more", { { n + "type", 1.0f }, { n + "gain", 0.5f } }, cp::kPink);
        // GAIN: a bell at 1 kHz, the band power around it follows the gain
        add ("gain", +1, bandDb (500.0, 2000.0), 0.25, "band power 0.5-2 kHz", "bell gain", {}, cp::kPink);
        // Q: a +12 dB bell gets narrower with Q, so the power in a wide band around it falls
        add ("q", -1, bandDb (300.0, 3000.0), 0.25, "band power 0.3-3 kHz, bell +12 dB", "higher Q = narrower bell = less boosted power", { { n + "gain", 1.0f }, { n + "q", 0.0f } }, cp::kPink);
        // SLOPE: five choices in the order 12, 24, 48, 36, 72 dB/oct (the first three keep their saved indices, P1n appended 36 and 72), so the knob is NOT an ordered scale: no direction is declared here.
        // The order by steepness is proved elsewhere: tools/measure.py A11 / A11c (every slope's attenuation and corner) and plugin_check 11l (power below the corner falls 12 > 24 > 36 > 48 > 72).
        c.directions.push_back (D ((n + "slope").toRawUTF8(), 0, nullptr, 0, nullptr, "", "five choices 12 / 24 / 48 / 36 / 72 dB/oct in saved-index order, not steepness order (A11, A11c and plugin_check 11l prove the steepness order)"));
        // THRESHOLD / RANGE (dynamic bell at 1 kHz, gain 0, trigger = own input on pink)
        add ("thr", +1, bandDb (500.0, 2000.0), 0.25, "band power 0.5-2 kHz, RANGE -12", "a higher threshold lets less of the cut in", cat (dynOn, { { n + "range", 0.0f }, { n + "gain", 0.5f } }), cp::kPink);
        add ("range", +1, bandDb (500.0, 2000.0), 0.25, "band power 0.5-2 kHz, THRESHOLD -60", "RANGE is added to the gain once the trigger is over the threshold", cat (dynOn, { { n + "thr", 0.0f }, { n + "gain", 0.5f } }), cp::kPink);
        // ATTACK / RELEASE on drum hits: slower attack lets more of each hit through; slower release keeps the cut longer
        add ("att", +1, outRmsDb(), 0.05, "output rms (dB), drums", "a slower attack lets more of each hit through before the cut arrives (a small effect on the rms: about 0.2 dB over the whole knob on the drum loop)", cat (dynOn, { { n + "range", 0.0f }, { n + "thr", 0.3f }, { n + "gain", 0.5f }, { n + "freq", 0.2f }, { n + "rel", 0.3f } }), cp::kDrums);
        add ("rel", -1, outRmsDb(), 0.05, "output rms (dB), drums", "a longer release keeps the cut in longer", cat (dynOn, { { n + "range", 0.0f }, { n + "thr", 0.3f }, { n + "gain", 0.5f }, { n + "freq", 0.2f }, { n + "att", 0.1f } }), cp::kDrums);
    }
    return cp::main (c, argc, argv);
}

#include "editor_checks.h"
#include "variation_rig.h"
#include "bake_tester_checks.h"


// ---- 13. stress: sample rates, random blocks, automation fuzz, clicks, extremes, NaN / Inf, denormals, lifecycle, state ------------
// One block of `in` (from pos, len) through the rig; `key` (stereo) goes on the sidechain bus when the rig has one.
static void stressBlock (Rig& r, const Audio& in, const Audio* key, Audio& out, int pos, int len, juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi)
{
    const int total = juce::jmax (r.proc.getTotalNumInputChannels(), r.proc.getTotalNumOutputChannels());
    buf.setSize (total, len, false, false, true);
    buf.clear();
    for (int c = 0; c < r.mainCh; ++c) std::copy (in[(size_t) c].begin() + pos, in[(size_t) c].begin() + pos + len, buf.getWritePointer (c));
    if (r.sc && key)
        for (int c = 0; c < 2 && r.mainCh + c < total; ++c)
            std::copy ((*key)[(size_t) c].begin() + pos, (*key)[(size_t) c].begin() + pos + len, buf.getWritePointer (r.mainCh + c));
    r.proc.processBlock (buf, midi);
    for (int c = 0; c < r.mainCh; ++c) std::copy (buf.getReadPointer (c), buf.getReadPointer (c) + len, out[(size_t) c].begin() + pos);
}

static std::vector<juce::AudioProcessorParameter*> stressParams (DyneqAudioProcessor& p)
{
    std::vector<juce::AudioProcessorParameter*> v;
    for (auto* ap : p.getParameters()) if (dynamic_cast<juce::AudioProcessorParameterWithID*> (ap) != nullptr) v.push_back (ap);
    return v;
}

struct FuzzStats { bool finite = true; double peak = 0.0; int blocks = 0, changes = 0; };
// 20000 blocks of 256, 1..3 random parameters set to a random (or extreme) normalised value at 30 % of the blocks
static std::unique_ptr<Rig> stressFuzz (const Audio& music, const Audio& key, FuzzStats& fs)
{
    auto rig = std::make_unique<Rig> (2, true, 48000.0, 256);
    auto ps = stressParams (rig->proc);
    std::mt19937 rng (20261011u);
    std::uniform_real_distribution<float> u01 (0.0f, 1.0f);
    const int n = (int) music[0].size(), B = 256;
    Audio in (2, std::vector<float> ((size_t) B)), ky (2, std::vector<float> ((size_t) B)), out (2, std::vector<float> ((size_t) B));
    juce::AudioBuffer<float> buf; juce::MidiBuffer midi;
    for (int blk = 0; blk < 20000; ++blk)
    {
        if (u01 (rng) < 0.3f)
        {
            const int k = 1 + (int) (u01 (rng) * 2.999f);
            for (int j = 0; j < k; ++j)
            {
                auto* p = ps[(size_t) juce::jmin ((int) ps.size() - 1, (int) (u01 (rng) * (float) ps.size()))];
                const float r = u01 (rng);
                p->setValueNotifyingHost (r < 0.1f ? 0.0f : r < 0.2f ? 1.0f : u01 (rng));
                ++fs.changes;
            }
        }
        for (int i = 0; i < B; ++i) for (int c = 0; c < 2; ++c) { in[(size_t) c][(size_t) i] = music[(size_t) c][(size_t) ((blk * B + i) % n)]; ky[(size_t) c][(size_t) i] = key[(size_t) c][(size_t) ((blk * B + i) % n)]; }
        stressBlock (*rig, in, &ky, out, 0, B, buf, midi);
        fs.finite = fs.finite && allFinite (out);
        fs.peak = std::max (fs.peak, peakOf (out));
        ++fs.blocks;
    }
    return rig;
}

static void runStressChecks (const Audio& beat, const Audio& vocal)
{
    std::printf ("\n-- 13 stress --\n");
    const Audio b2 = slice (beat, 0, 96000), v2 = slice (vocal, 0, 96000);

    // 13.1 sample rates beyond 44.1 / 48 / 96 (section 6): bands + COLLIDE + SELF + a TARGET entry, latency 0
    for (double sr : { 22050.0, 88200.0, 176400.0, 192000.0 })
    {
        const Settings st = { { "depth", 12 }, { "margin", 0 }, { "b1_on", 1 }, { "b1_gain", 12 }, { "b2_on", 1 }, { "b2_type", 5 }, { "b2_freq", 18000 }, { "b2_slope", 2 },
                              { "b3_on", 1 }, { "b3_dyn", 1 }, { "b3_range", -12 }, { "b3_thr", -60 } };
        Rig a (2, true, sr, 512, st), s (2, false, sr, 480, { { "mode", 1 }, { "depth", 12 } }), t (2, false, sr, 512, { { "targetInst", 3 }, { "targetPart", 60 }, { "targetAmount", 12 }, { "targetWidth", 1.4f } });
        const auto oa = a.render (b2, &v2, 512), os = s.render (b2, nullptr, 480), ot = t.render (b2, nullptr, 512);
        const bool lat = a.proc.getLatencySamples() == 0 && s.proc.getLatencySamples() == 0 && t.proc.getLatencySamples() == 0;
        check (allFinite (oa) && allFinite (os) && allFinite (ot) && peakOf (oa) < 20.0 && peakOf (os) < 20.0 && peakOf (ot) < 20.0 && lat,
               "13.1 sample rate " + juce::String (sr, 0) + ": bands + COLLIDE, SELF, TARGET run finite, peak < 20, latency 0",
               juce::String::formatted ("peaks %.3f / %.3f / %.3f, latency %d", peakOf (oa), peakOf (os), peakOf (ot), a.proc.getLatencySamples()));
    }

    // 13.2 a random block size on every call == fixed 512, bit-identical
    {
        const Audio b4 = slice (beat, 0, 48000 * 4), v4 = slice (vocal, 0, 48000 * 4);
        const Settings st = { { "depth", 10 }, { "margin", 1 }, { "mix", 70 }, { "b1_on", 1 }, { "b1_gain", 5 }, { "b1_freq", 3000 },
                              { "b2_on", 1 }, { "b2_type", 2 }, { "b2_freq", 5000 }, { "b2_dyn", 1 }, { "b2_thr", -40 }, { "b2_range", -8 }, { "b2_trig", 1 }, { "bakeArm", 1 },
                              { "targetInst", 3 }, { "targetPart", 60 }, { "targetAmount", 12 }, { "targetWidth", 1.4f }, { "reverbReady", 1 } };
        Rig fixedRig (2, true, 48000.0, 512, st), rndRig (2, true, 48000.0, 512, st);
        const Audio ref = fixedRig.render (b4, &v4, 512);
        std::mt19937 rng (777u);
        Audio out (2, std::vector<float> (b4[0].size()));
        juce::AudioBuffer<float> buf; juce::MidiBuffer midi;
        int calls = 0, mn = 99999, mx = 0;
        for (int pos = 0; pos < (int) b4[0].size();)
        {
            const int len = juce::jmin (1 + (int) (rng() % 4096u), (int) b4[0].size() - pos);
            mn = std::min (mn, len); mx = std::max (mx, len); ++calls;
            stressBlock (rndRig, b4, &v4, out, pos, len, buf, midi);
            pos += len;
        }
        check (maxDiff (out, ref) == 0.0, "13.2 random block size (1..4096, seed 777) on every call == fixed 512, bit-identical", juce::String::formatted ("%d calls, sizes %d..%d, maxAbsDiff=%.3e", calls, mn, mx, maxDiff (out, ref)));
    }

    // 13.3 automation fuzz: 20000 blocks of 256, every parameter at random; then defaults == a fresh instance
    FuzzStats fs; auto fz = stressFuzz (beat, vocal, fs);
    const int nParams = (int) stressParams (fz->proc).size();
    check (fs.finite && fs.peak < 200.0, "13.3a automation fuzz (" + juce::String (nParams) + " parameters, 20000 x 256, seed 20261011): output finite, peak < 200 (input peak 0.6; six random bells, up to +18 dB each, and OUTPUT +12 dB can stack to +40 dB legitimately)",
           juce::String::formatted ("%d blocks, %d parameter changes, peak %.3f", fs.blocks, fs.changes, fs.peak));
    juce::MemoryBlock fuzzState; fz->proc.getStateInformation (fuzzState);
    {
        for (auto* p : stressParams (fz->proc)) p->setValueNotifyingHost (p->getDefaultValue());
        Rig fresh (2, true, 48000.0, 256);
        const Audio m6 = beat, k6 = vocal;
        const Audio of = fz->render (m6, &k6, 256), on = fresh.render (m6, &k6, 256);
        double settled = 0.0, whole = maxDiff (of, on);
        settled = maxDiff (slice (of, 96000, 192000), slice (on, 96000, 192000));
        for (int sec = 0; sec < 6; ++sec) std::printf ("   info: fuzzed vs fresh, second %d: maxAbsDiff %.3e\n", sec, maxDiff (slice (of, sec * 48000, 48000), slice (on, sec * 48000, 48000)));
        {   // where does the 2e-5 live? the same fuzzed instance, engine re-prepared, then the same music again
            fz->proc.prepareToPlay (48000.0, 256);
            Rig fresh2 (2, true, 48000.0, 256);
            const Audio o2 = fz->render (m6, &k6, 256), n2 = fresh2.render (m6, &k6, 256);
            std::printf ("   info: same fuzzed instance after prepareToPlay (parameters still default): maxAbsDiff 1..6 s = %.3e\n", maxDiff (slice (o2, 48000, 240000), slice (n2, 48000, 240000)));
        }
        {   // 13.3b a longer run (beat x4, 24 s): the fuzzed instance with defaults becomes bit-identical to a fresh one (no stuck state)
            FuzzStats f4; auto z = stressFuzz (beat, vocal, f4);
            for (auto* p : stressParams (z->proc)) p->setValueNotifyingHost (p->getDefaultValue());
            Audio m4 (2), k4 (2);
            for (int r = 0; r < 4; ++r) for (int c = 0; c < 2; ++c) { m4[(size_t) c].insert (m4[(size_t) c].end(), beat[(size_t) c].begin(), beat[(size_t) c].end()); k4[(size_t) c].insert (k4[(size_t) c].end(), vocal[(size_t) c].begin(), vocal[(size_t) c].end()); }
            Rig fr4 (2, true, 48000.0, 256);
            const Audio a4 = z->render (m4, &k4, 256), b4x = fr4.render (m4, &k4, 256);
            const int secs = (int) a4[0].size() / 48000;
            int exactFrom = -1;
            for (int sec = secs - 1; sec >= 0; --sec) { if (maxDiff (slice (a4, sec * 48000, 48000), slice (b4x, sec * 48000, 48000)) != 0.0) break; exactFrom = sec; }
            const double tail = maxDiff (slice (a4, 12 * 48000, (int) a4[0].size() - 12 * 48000), slice (b4x, 12 * 48000, (int) b4x[0].size() - 12 * 48000));
            check (allFinite (a4) && tail == 0.0, "13.3b after the fuzz, parameters back at defaults: the output becomes bit-identical to a fresh instance (no stuck state; checked from 12 s of a 24 s run)",
                   juce::String::formatted ("identical from second %d on; maxAbsDiff 12..24 s = %.3e; seconds 2..5 still differ by ~2e-5 while the slow analysis settles", exactFrom, tail));
        }
        {   // 13.3c a host reset() after the fuzz (defaults set): the output equals a fresh instance from the first sample
            fz->proc.reset();
            Rig fresh3 (2, true, 48000.0, 256);
            const Audio o3 = fz->render (m6, &k6, 256), n3 = fresh3.render (m6, &k6, 256);
            check (maxDiff (o3, n3) == 0.0, "13.3c host reset() after the fuzz: the output equals a fresh instance bit for bit (the engine and the processor's ramps start cold)", juce::String::formatted ("maxAbsDiff=%.3e", maxDiff (o3, n3)));
        }
        std::printf ("   info: 6 s run: maxAbsDiff 2..6 s = %.3e, whole 6 s = %.3e (the analysis memory is still settling)\n", settled, whole);
    }

    // 13.4 click test. A pure 1 kHz sine y satisfies y[n+1] + y[n-1] - 2 cos(w) y[n] = 0; a slow parameter move leaves that residual tiny, a step
    //      (zipper, click) in level or phase does not. Worst 1 ms window of the residual against the sine's RMS, parameter updated every 512
    //      samples (a host block) and every 8 samples (fine reference). Music: the level of (512-step minus 8-step) is printed for information.
    {
        struct Sweep { const char* name; const char* id; double a, b; bool geo; Settings base; };
        const std::vector<Sweep> sweeps = {
            { "b1_freq 100->10k", "b1_freq", 100.0, 10000.0, true,  { { "b1_on", 1 }, { "b1_gain", 12 }, { "b1_q", 3 } } },
            { "b1_gain -18->+18", "b1_gain", -18.0, 18.0, false,    { { "b1_on", 1 }, { "b1_freq", 1000 }, { "b1_q", 2 } } },
            { "b1_q 0.3->12",     "b1_q",    0.3, 12.0, true,       { { "b1_on", 1 }, { "b1_freq", 1000 }, { "b1_gain", 12 } } },
            { "output -12->+12",  "output",  -12.0, 12.0, false,    {} } };
        const Audio mus = slice (beat, 0, 96000);
        const Audio sine1k = sineAudio (1000.0, -20.0, 2.0);
        const int start = 12000, sweepLen = 48000;
        for (auto& sw : sweeps)
        {
            auto run = [&] (const Audio& in, int step)
            {
                Rig r (2, false, 48000.0, 512, sw.base);
                Audio out (2, std::vector<float> (in[0].size()));
                juce::AudioBuffer<float> buf; juce::MidiBuffer midi;
                for (int pos = 0; pos < (int) in[0].size(); pos += step)
                {
                    const double t = juce::jlimit (0.0, 1.0, (double) (pos - start) / sweepLen);
                    const double v = sw.geo ? sw.a * std::pow (sw.b / sw.a, t) : sw.a + (sw.b - sw.a) * t;
                    setParam (r.proc.apvts, sw.id, (float) v);
                    stressBlock (r, in, nullptr, out, pos, juce::jmin (step, (int) in[0].size() - pos), buf, midi);
                }
                return out;
            };
            auto residualDb = [&] (const Audio& y)
            {
                const double c2 = 2.0 * std::cos (2.0 * juce::MathConstants<double>::pi * 1000.0 / 48000.0), w = 48;
                const double ref = rmsDb (sine1k, 0, 48000);
                double worst = -999.0;
                for (int p = start + 1; p + (int) w < start + sweepLen + 4800; p += 24)
                {
                    double e = 0; long n = 0;
                    for (size_t c = 0; c < 2; ++c) for (int i = p; i < p + (int) w; ++i) { const double r = (double) y[c][(size_t) i + 1] + y[c][(size_t) i - 1] - c2 * y[c][(size_t) i]; e += r * r; ++n; }
                    worst = std::max (worst, 10.0 * std::log10 (e / (double) n + 1e-30) - ref);
                }
                return worst;
            };
            const double r512 = residualDb (run (sine1k, 512)), r8 = residualDb (run (sine1k, 8));
            const Audio m512 = run (mus, 512), m8 = run (mus, 8);
            double wA = -999.0; const double prog = rmsDb (m8, start, start + sweepLen);
            for (int p = start; p + 48 <= start + sweepLen; p += 24)
            { double e = 0; long n = 0; for (size_t c = 0; c < 2; ++c) for (int i = p; i < p + 48; ++i) { const double d = (double) m512[c][(size_t) i] - m8[c][(size_t) i]; e += d * d; ++n; } wA = std::max (wA, 10.0 * std::log10 (e / (double) n + 1e-30) - prog); }
            check (r512 < -40.0, juce::String ("13.4 click test ") + sw.name + ": sine, worst 1 ms window of the stationarity residual vs the sine's RMS, 512-sample parameter steps, < -40 dB",
                   juce::String::formatted ("512-step %.1f dB, 8-step %.1f dB; music (512-step minus 8-step) vs programme %.1f dB (info)", r512, r8, wA));
        }
    }

    // 13.5 all six bands at extremes
    {
        Rig r (2, false, 48000.0, 512);
        const Audio in = slice (beat, 0, 48000);
        auto ps = [&] (const juce::String& id) { return r.proc.apvts.getParameter (id); };
        const int nTypes = ps ("b1_type")->getNumSteps();
        int runs = 0; bool fin = true; double pk = 0.0; juce::String worstDesc;
        for (int variant = 0; variant < 2; ++variant)   // 0: static, 1: dynamic with RANGE at its extreme
            for (int t = 0; t < nTypes; ++t)
                for (int combo = 0; combo < 8; ++combo)
                {
                    for (int b = 1; b <= 6; ++b)
                    {
                        const juce::String n = "b" + juce::String (b) + "_";
                        ps (n + "on")->setValueNotifyingHost (1.0f);
                        ps (n + "type")->setValueNotifyingHost ((float) t / (float) (nTypes - 1));
                        ps (n + "freq")->setValueNotifyingHost ((combo & 1) ? 1.0f : 0.0f);
                        ps (n + "gain")->setValueNotifyingHost ((combo & 2) ? 1.0f : 0.0f);
                        ps (n + "q")->setValueNotifyingHost ((combo & 4) ? 1.0f : 0.0f);
                        ps (n + "slope")->setValueNotifyingHost (1.0f);
                        ps (n + "dyn")->setValueNotifyingHost (variant == 1 ? 1.0f : 0.0f);
                        ps (n + "range")->setValueNotifyingHost ((combo & 2) ? 1.0f : 0.0f);
                        ps (n + "thr")->setValueNotifyingHost (0.0f);
                        ps (n + "att")->setValueNotifyingHost (0.0f);
                        ps (n + "rel")->setValueNotifyingHost ((combo & 4) ? 0.0f : 1.0f);
                    }
                    const auto o = r.render (in, nullptr, 512);
                    ++runs; fin = fin && allFinite (o);
                    if (peakOf (o) > pk) { pk = peakOf (o); worstDesc = juce::String::formatted ("variant %d type %d combo %d", variant, t, combo); }
                }
        {   // where does the worst case come from? same config, 1..6 bands, then the static twin
            auto cfg = [&] (int nb, bool dyn) {
                for (int b = 1; b <= 6; ++b)
                {
                    const juce::String n = "b" + juce::String (b) + "_";
                    ps (n + "on")->setValueNotifyingHost (b <= nb ? 1.0f : 0.0f); ps (n + "type")->setValueNotifyingHost (2.0f / (float) (nTypes - 1));
                    ps (n + "freq")->setValueNotifyingHost (0.0f); ps (n + "gain")->setValueNotifyingHost (1.0f); ps (n + "q")->setValueNotifyingHost (1.0f);
                    ps (n + "slope")->setValueNotifyingHost (1.0f); ps (n + "dyn")->setValueNotifyingHost (dyn ? 1.0f : 0.0f); ps (n + "range")->setValueNotifyingHost (1.0f);
                    ps (n + "thr")->setValueNotifyingHost (0.0f); ps (n + "att")->setValueNotifyingHost (0.0f); ps (n + "rel")->setValueNotifyingHost (1.0f);
                } };
            for (bool dyn : { false, true })
            {
                juce::String line;
                for (int nb = 1; nb <= 6; ++nb) { cfg (nb, dyn); line += juce::String::formatted (" %d band%s: %.4g;", nb, nb > 1 ? "s" : "", peakOf (r.render (in, nullptr, 512))); }
                std::printf ("   info: type 2, FREQ min, GAIN max, Q max, %s, input peak %.3f:%s\n", dyn ? "dynamic (RANGE max, THR min)" : "static", peakOf (in), line.toRawUTF8());
            }
            std::printf ("   info: b1 type 2 range %.1f..%.1f dB gain %.1f..%.1f dB q %.2f..%.2f freq %.1f..%.1f\n",
                         (double) ps ("b1_range")->convertFrom0to1 (0), (double) ps ("b1_range")->convertFrom0to1 (1), (double) ps ("b1_gain")->convertFrom0to1 (0), (double) ps ("b1_gain")->convertFrom0to1 (1),
                         (double) ps ("b1_q")->convertFrom0to1 (0), (double) ps ("b1_q")->convertFrom0to1 (1), (double) ps ("b1_freq")->convertFrom0to1 (0), (double) ps ("b1_freq")->convertFrom0to1 (1));
        }
        {   // one band alone at every extreme: its peak must stay bounded (+-18 dB gain, +-12 dB range: no filter may ring up beyond 80 dB)
            double worst1 = 0.0; juce::String wdesc; int nOver = 0;
            for (int variant = 0; variant < 2; ++variant)
                for (int t = 0; t < nTypes; ++t)
                    for (int combo = 0; combo < 8; ++combo)
                    {
                        Rig r1 (2, false, 48000.0, 512);   // a fresh instance per run: no ring-down of an earlier extreme
                        auto ps = [&] (const juce::String& id) { return r1.proc.apvts.getParameter (id); };
                        for (int b = 1; b <= 6; ++b)
                        {
                            const juce::String n = "b" + juce::String (b) + "_";
                            ps (n + "on")->setValueNotifyingHost (b == 1 ? 1.0f : 0.0f);
                            ps (n + "type")->setValueNotifyingHost ((float) t / (float) (nTypes - 1));
                            ps (n + "freq")->setValueNotifyingHost ((combo & 1) ? 1.0f : 0.0f); ps (n + "gain")->setValueNotifyingHost ((combo & 2) ? 1.0f : 0.0f); ps (n + "q")->setValueNotifyingHost ((combo & 4) ? 1.0f : 0.0f);
                            ps (n + "slope")->setValueNotifyingHost (1.0f); ps (n + "dyn")->setValueNotifyingHost (variant == 1 ? 1.0f : 0.0f);
                            ps (n + "range")->setValueNotifyingHost ((combo & 2) ? 1.0f : 0.0f); ps (n + "thr")->setValueNotifyingHost (0.0f); ps (n + "att")->setValueNotifyingHost (0.0f); ps (n + "rel")->setValueNotifyingHost ((combo & 4) ? 0.0f : 1.0f);
                        }
                        const double pkv = peakOf (r1.render (in, nullptr, 512));
                        if (pkv > 1.0e4) { ++nOver; std::printf ("   info: ONE band over 1e4: %s type %d, FREQ %s, GAIN %s, Q %s, SLOPE 72: peak %.4g\n", variant ? "dynamic (RANGE max, THR min, ATT min)" : "static", t, (combo & 1) ? "max" : "min", (combo & 2) ? "max" : "min", (combo & 4) ? "max" : "min", pkv); }
                        if (pkv > worst1) { worst1 = pkv; wdesc = juce::String::formatted ("%s type %d combo %d", variant ? "dynamic" : "static", t, combo); }
                    }
            check (worst1 < 1.0e4, "13.5c ONE band alone at every extreme (types x FREQ x GAIN x Q x static / dynamic, 96 runs): peak < 1e4 (input peak 0.56, i.e. < +85 dB)", juce::String::formatted ("worst %.4g (%s), %d of 96 runs over 1e4", worst1, wdesc.toRawUTF8(), nOver));
        }
        // and a spread of the six bands over the spectrum at max gain / max Q
        for (int b = 1; b <= 6; ++b)
        {
            const juce::String n = "b" + juce::String (b) + "_";
            ps (n + "dyn")->setValueNotifyingHost (0.0f); ps (n + "type")->setValueNotifyingHost (0.0f);
            ps (n + "freq")->setValueNotifyingHost ((float) (b - 1) / 5.0f); ps (n + "gain")->setValueNotifyingHost (1.0f); ps (n + "q")->setValueNotifyingHost (1.0f);
        }
        const auto osp = r.render (in, nullptr, 512);
        fin = fin && allFinite (osp);
        for (int b = 1; b <= 6; ++b) ps ("b" + juce::String (b) + "_on")->setValueNotifyingHost (0.0f);
        const auto rec = r.render (in, nullptr, 512);
        const double recDiff = maxDiff (slice (rec, 24000, 24000), slice (in, 24000, 24000));
        check (fin, "13.5a six bands at extremes (" + juce::String (runs) + " runs of 1 s: types x freq min/max x gain min/max x Q min/max x static/dynamic, slope 72): every sample finite (the peak is the legitimate stack of six extreme bands)",
               juce::String::formatted ("%d types, peak %.4g (%s), spread max gain/Q peak %.4g", nTypes, pk, worstDesc.toRawUTF8(), peakOf (osp)));
        check (allFinite (rec) && recDiff < 1e-3, "13.5b after the extremes, all bands off again: the second half of the next second equals the input", juce::String::formatted ("maxAbsDiff=%.3e", recDiff));
    }

    // 13.6 NaN / Inf in one input block: output finite again within 2 blocks, engine recovers
    {
        const Audio in = slice (beat, 0, 48000 * 6), key = slice (vocal, 0, 48000 * 6);
        const Settings st = { { "depth", 10 }, { "margin", 1 }, { "b1_on", 1 }, { "b1_gain", 5 }, { "b1_freq", 3000 }, { "b2_on", 1 }, { "b2_type", 2 }, { "b2_freq", 5000 }, { "b2_dyn", 1 }, { "b2_thr", -40 }, { "b2_range", -8 }, { "b2_trig", 1 } };
        const int bad = 48000, B = 512;   // block starting at 1 s
        struct Case { const char* name; float v; bool inKey; bool wholeBlock; };
        for (const Case& cs : { Case { "NaN (whole block)", std::nanf (""), false, true }, Case { "Inf (whole block)", INFINITY, false, true }, Case { "one NaN sample", std::nanf (""), false, false }, Case { "NaN on the sidechain", std::nanf (""), true, true } })
        {
            Audio inB = in, keyB = key, inZ = in, keyZ = key;
            for (int c = 0; c < 2; ++c)
                for (int i = bad; i < bad + (cs.wholeBlock ? B : 1); ++i)
                {
                    (cs.inKey ? keyB : inB)[(size_t) c][(size_t) i] = cs.v;
                    if (cs.wholeBlock) (cs.inKey ? keyZ : inZ)[(size_t) c][(size_t) i] = 0.0f;
                    else (cs.inKey ? keyZ : inZ)[(size_t) c][(size_t) i] = 0.0f;
                }
            Rig rb (2, true, 48000.0, B, st), rz (2, true, 48000.0, B, st);
            const Audio ob = rb.render (inB, &keyB, B), oz = rz.render (inZ, &keyZ, B);
            int firstFinite = -1;   // blocks after the bad one until one is finite and stays finite
            for (int blk = bad / B + 1; blk * B < (int) ob[0].size(); ++blk)
            {
                bool ok = true;
                for (int c = 0; c < 2; ++c) for (int i = blk * B; i < std::min ((blk + 1) * B, (int) ob[0].size()); ++i) ok = ok && std::isfinite (ob[(size_t) c][(size_t) i]);
                if (ok) { firstFinite = blk - bad / B; break; }
            }
            // finite for the whole remainder after that point?
            bool tailFinite = firstFinite >= 0 && allFinite (slice (ob, bad / B * B + (firstFinite) * B, (int) ob[0].size() - (bad / B * B + firstFinite * B)));
            double rec = maxDiff (slice (ob, 144000, 144000), slice (oz, 144000, 144000));   // from 3 s (2 s after the bad block)
            long nanTail = 0;
            for (int c = 0; c < 2; ++c) for (int i = 144000; i < (int) ob[0].size(); ++i) nanTail += ! std::isfinite (ob[(size_t) c][(size_t) i]);
            if (nanTail > 0) rec = 1e9;
            check (firstFinite >= 0 && firstFinite <= 2 && tailFinite && rec < 1e-3, juce::String ("13.6 ") + cs.name + " in one block: finite again within 2 blocks, and 2 s later equal to a run with that block silent",
                   juce::String::formatted ("first finite block after the bad one: %d (-1 = never), tail finite %d, non-finite samples from 3 s: %ld of %d, maxAbsDiff from 3 s = %.3e", firstFinite, (int) tailFinite, nanTail, 2 * ((int) ob[0].size() - 144000), rec));
        }
    }

    // 13.7 denormals: 60 s of silence after music must not cost more than 1.5x the music (CPU per block, std::chrono)
    {
        const Settings st = { { "depth", 10 }, { "margin", 1 }, { "b1_on", 1 }, { "b1_gain", 5 }, { "b1_freq", 3000 }, { "b2_on", 1 }, { "b2_type", 2 }, { "b2_freq", 5000 }, { "b2_dyn", 1 }, { "b2_thr", -40 }, { "b2_range", -8 }, { "b2_trig", 1 },
                              { "b3_on", 1 }, { "b3_type", 4 }, { "b3_freq", 60 }, { "b3_slope", 4 }, { "targetInst", 3 }, { "targetPart", 60 }, { "targetAmount", 12 } };
        Rig r (2, true, 48000.0, 512, st);
        const int B = 512;
        Audio in (2, std::vector<float> ((size_t) B)), ky (2, std::vector<float> ((size_t) B)), out (2, std::vector<float> ((size_t) B));
        juce::AudioBuffer<float> buf; juce::MidiBuffer midi;
        using clk = std::chrono::steady_clock;
        auto timeBlocks = [&] (const Audio& mus, const Audio& key, int nBlocks, bool silent, double& perBlockUs)
        {
            double tot = 0.0;
            const int n = (int) mus[0].size();
            for (int blk = 0; blk < nBlocks; ++blk)
            {
                for (int i = 0; i < B; ++i) for (int c = 0; c < 2; ++c) { in[(size_t) c][(size_t) i] = silent ? 0.0f : mus[(size_t) c][(size_t) ((blk * B + i) % n)]; ky[(size_t) c][(size_t) i] = silent ? 0.0f : key[(size_t) c][(size_t) ((blk * B + i) % n)]; }
                const int total = juce::jmax (r.proc.getTotalNumInputChannels(), r.proc.getTotalNumOutputChannels());
                buf.setSize (total, B, false, false, true); buf.clear();
                for (int c = 0; c < 2; ++c) std::copy (in[(size_t) c].begin(), in[(size_t) c].end(), buf.getWritePointer (c));
                for (int c = 0; c < 2 && 2 + c < total; ++c) std::copy (ky[(size_t) c].begin(), ky[(size_t) c].end(), buf.getWritePointer (2 + c));
                const auto t0 = clk::now();
                r.proc.processBlock (buf, midi);
                tot += std::chrono::duration<double, std::micro> (clk::now() - t0).count();
            }
            perBlockUs = tot / nBlocks;
        };
        double warm = 0, music = 1e9, music2 = 0;
        timeBlocks (beat, vocal, 563, false, warm);                                    // 6 s warm-up (learns TARGET, fills the filters)
        for (int rep = 0; rep < 3; ++rep) { timeBlocks (beat, vocal, 563, false, music2); music = std::min (music, music2); }
        double silFirst = 0, silAll = 0, silMid = 0;
        timeBlocks (beat, vocal, 469, true, silFirst);                                 // first 5 s of silence: the tails decay here
        const int blocks60 = 60 * 48000 / B - 469;
        timeBlocks (beat, vocal, blocks60, true, silMid);
        silAll = (silFirst * 469 + silMid * blocks60) / (469 + blocks60);
        check (silFirst <= 1.5 * music && silAll <= 1.5 * music, "13.7 denormals: 60 s of silence after music costs <= 1.5x the CPU per block of the music",
               juce::String::formatted ("music %.1f us/block (best of 3), silence first 5 s %.1f, rest %.1f, whole 60 s %.1f us/block, ratio %.2f / %.2f", music, silFirst, silMid, silAll, silFirst / music, silAll / music));
    }

    // 13.8 lifecycle: prepareToPlay twice, reset() mid-stream, sample-rate change mid-session
    {
        const Audio in = slice (beat, 0, 48000 * 2), key = slice (vocal, 0, 48000 * 2);
        const Settings st = { { "depth", 10 }, { "margin", 1 }, { "b1_on", 1 }, { "b1_gain", 5 }, { "b1_freq", 3000 }, { "b2_on", 1 }, { "b2_type", 2 }, { "b2_freq", 5000 }, { "b2_dyn", 1 }, { "b2_thr", -40 }, { "b2_range", -8 }, { "b2_trig", 1 } };
        Rig fresh (2, true, 48000.0, 512, st), twice (2, true, 48000.0, 512, st);
        twice.proc.prepareToPlay (48000.0, 512); twice.proc.prepareToPlay (48000.0, 512);
        const Audio of = fresh.render (in, &key, 512), ot = twice.render (in, &key, 512);
        check (allFinite (ot) && twice.proc.getLatencySamples() == 0 && maxDiff (of, ot) == 0.0, "13.8a prepareToPlay called three times before the first block == a fresh instance, bit-identical, latency 0", juce::String::formatted ("maxAbsDiff=%.3e", maxDiff (of, ot)));

        Rig m (2, true, 48000.0, 512, st);
        Audio out (2, std::vector<float> (in[0].size()));
        juce::AudioBuffer<float> buf; juce::MidiBuffer midi;
        bool fin = true;
        for (int pos = 0; pos + 512 <= (int) in[0].size(); pos += 512)
        {
            if (pos == 20 * 512 || pos == 60 * 512) m.proc.reset();
            if (pos == 40 * 512) { m.proc.releaseResources(); m.proc.prepareToPlay (48000.0, 512); }
            stressBlock (m, in, &key, out, pos, 512, buf, midi);
        }
        fin = allFinite (out);
        // the same session through the 48 -> 96 -> 44.1 kHz route
        Rig s (2, true, 48000.0, 512, st);
        bool finSr = true; int latSr = 0; double pk = 0.0;
        for (double sr : { 48000.0, 96000.0, 44100.0, 48000.0 })
        {
            s.proc.setRateAndBufferSizeDetails (sr, 512);
            s.proc.prepareToPlay (sr, 512);
            const Audio o = s.render (in, &key, 512);
            finSr = finSr && allFinite (o); pk = std::max (pk, peakOf (o)); latSr = std::max (latSr, s.proc.getLatencySamples());
        }
        check (fin && finSr && latSr == 0 && pk < 20.0, "13.8b reset() x2 and releaseResources + prepareToPlay mid-stream, then 48 -> 96 -> 44.1 -> 48 kHz mid-session: finite, peak < 20, latency 0",
               juce::String::formatted ("peak %.3f, latency %d, reset-run finite %d", pk, latSr, (int) fin));
    }

    // 13.9 state after the fuzz: getStateInformation -> fresh instance -> render 2 s == the fuzzed instance continuing
    {
        FuzzStats fs2; auto a = stressFuzz (beat, vocal, fs2);
        juce::MemoryBlock st; a->proc.getStateInformation (st);
        check (st.getSize() == fuzzState.getSize() && std::memcmp (st.getData(), fuzzState.getData(), st.getSize()) == 0 && fs2.peak == fs.peak,
               "13.9a the fuzz is deterministic (two runs: identical state blob and peak)", juce::String::formatted ("%zu bytes, peak %.6f / %.6f", st.getSize(), fs.peak, fs2.peak));
        Rig b (2, true, 48000.0, 256);
        b.proc.setStateInformation (st.getData(), (int) st.getSize());
        const Snap sa = snapOf (a->proc), sb = snapOf (b.proc);
        const int pd = snapDiff (sa, sb);
        const Audio in = slice (beat, 0, 96000), key = slice (vocal, 0, 96000);
        // hard reset of both engines (prepareToPlay): the engines start equal, the parameters are the saved ones
        a->proc.prepareToPlay (48000.0, 256); b.proc.prepareToPlay (48000.0, 256);
        const Audio oa = a->render (in, &key, 256), ob = b.render (in, &key, 256);
        check (pd == 0 && maxDiff (oa, ob) == 0.0, "13.9b state of the fuzzed instance into a fresh one, both prepared again, 2 s rendered: parameters and output bit-identical",
               juce::String::formatted ("%d parameters differ, maxAbsDiff=%.3e", pd, maxDiff (oa, ob)));
        // the same without the re-prepare: reset() only (the processor does not override it)
        FuzzStats fs3; auto a3 = stressFuzz (beat, vocal, fs3);
        Rig b3 (2, true, 48000.0, 256);
        b3.proc.setStateInformation (st.getData(), (int) st.getSize());
        a3->proc.reset(); b3.proc.reset();
        const Audio o3a = a3->render (in, &key, 256), o3b = b3.render (in, &key, 256);
        std::printf ("   info: with reset() only (no prepareToPlay): maxAbsDiff 0..2 s = %.3e, 1..2 s = %.3e\n", maxDiff (o3a, o3b), maxDiff (slice (o3a, 48000, 48000), slice (o3b, 48000, 48000)));
    }
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    for (int i = 1; i < argc; ++i)
        if (juce::String (argv[i]).startsWith ("--controlproof")) return runControlProof (argc, argv);
    for (int i = 1; i < argc; ++i)
        if (juce::String (argv[i]) == "--rig") return runVariationRig (argc, argv);
    juce::File dir = argc > 1 ? juce::File (argv[1]) : juce::File::getCurrentWorkingDirectory().getChildFile ("tools/demo_material");
    if (! dir.isDirectory()) { std::printf ("demo material dir not found: %s\n", dir.getFullPathName().toRawUTF8()); return 2; }
    const auto beatFull = readWav (dir.getChildFile ("beat.wav"));
    const auto vocalFull = readWav (dir.getChildFile ("vocal.wav"));
    const Audio beat = slice (beatFull, 0, 48000 * 6), vocal = slice (vocalFull, 0, 48000 * 6);   // 6 s each, 48 kHz stereo
    std::printf ("files: beat %zu ch x %zu, vocal %zu ch x %zu (first 6 s used)\n", beat.size(), beat[0].size(), vocal.size(), vocal[0].size());

    // 1. identity, buses
    {
        DyneqAudioProcessor a;
        check (a.getName() == "ADALET" && a.getBusCount (true) == 2 && a.getBus (true, 1)->getName() == "Sidechain" && ! a.getBus (true, 1)->isEnabled(),
               "identity DYNEQ, sidechain bus 'Sidechain' exists, disabled by default");
        auto l = a.getBusesLayout();
        auto mono = l; mono.inputBuses.set (0, juce::AudioChannelSet::mono()); mono.outputBuses.set (0, juce::AudioChannelSet::mono());
        auto bad = l; bad.inputBuses.set (0, juce::AudioChannelSet::mono());
        auto monoSc = mono; monoSc.inputBuses.set (1, juce::AudioChannelSet::mono());
        check (a.checkBusesLayoutSupported (mono) && ! a.checkBusesLayoutSupported (bad) && a.checkBusesLayoutSupported (monoSc), "mono/mono accepted, mono key accepted, mono-in/stereo-out refused");
        Rig r (2, true, 48000.0, 512);
        check (r.proc.getLatencySamples() == 0 && r.proc.getTailLengthSeconds() == 0.0, "latency reported 0, tail 0", juce::String::formatted ("latency=%d", r.proc.getLatencySamples()));
        check (a.getBypassParameter() != nullptr && a.getBypassParameter()->getName (32) == "BYPASS", "the host's bypass parameter is the plug-in's BYPASS");
        int n = 0; for (auto* ap : a.getParameters()) { (void) ap; ++n; }
        check (n == 12 + 6 * 12 + 7 + 4, "95 parameters: 12 global + 6 bands x 12 + 7 TARGET + 4 panel (OUTPUT, ASSIST OFF, BYPASS LEVEL MATCH, ANALYZER; FACE removed with ORBIT, 8 Nov 2026)", "count=" + juce::String (n));
    }

    // 2. bit-exact: no sidechain, silent key, bypass (own and host's), SELF on silence
    {
        auto o1 = runPlugin (beat, nullptr, false, 512, {});
        check (maxDiff (o1, beat) == 0.0, "COLLIDE, no sidechain bus: output == input bit for bit (6 s of beat)", juce::String::formatted ("maxAbsDiff=%.3e", maxDiff (o1, beat)));
        Audio silentKey (2, std::vector<float> (beat[0].size(), 0.0f));
        auto o2 = runPlugin (beat, &silentKey, true, 333, {});
        check (maxDiff (o2, beat) == 0.0, "COLLIDE, sidechain bus enabled but silent: output == input bit for bit", juce::String::formatted ("maxAbsDiff=%.3e", maxDiff (o2, beat)));
        auto o3 = runPlugin (beat, nullptr, false, 512, { { "mode", 1 } });
        auto raw3 = runRaw (beat, nullptr, 512, [&] { dyneq::Params p; p.mode = dyneq::kSelf; return p; }());
        check (maxDiff (o3, raw3) == 0.0, "SELF (no key needed) equals the raw engine bit for bit", juce::String::formatted ("maxAbsDiff=%.3e", maxDiff (o3, raw3)));
        Audio zeros (2, std::vector<float> (48000, 0.0f));
        check (maxDiff (runPlugin (zeros, nullptr, false, 512, { { "mode", 1 } }), zeros) == 0.0, "SELF on silence == silence bit for bit");
        // bypass: the plug-in's own parameter and the host's call, with a live collision, live bands and the key present
        const Settings busy = { { "depth", 12 }, { "margin", 0 }, { "b1_on", 1 }, { "b1_gain", 9 }, { "b2_on", 1 }, { "b2_type", 4 }, { "b2_freq", 300 } };
        for (int block : { 512, 77 })
        {
            Settings st = busy; st.push_back ({ "bypass", 1 }); st.push_back ({ "levelMatch", 0 });   // the true bypass: the level match is a separate switch (section 11)
            auto own = runPlugin (beat, &vocal, true, block, st);
            auto host = runPlugin (beat, &vocal, true, block, busy, 48000.0, true);
            auto live = runPlugin (beat, &vocal, true, block, busy);
            check (maxDiff (own, beat) == 0.0 && maxDiff (host, beat) == 0.0 && maxDiff (live, beat) > 0.01,
                   "bypass (own parameter and host call) == input bit for bit, the same settings change the audio when not bypassed, block " + juce::String (block),
                   juce::String::formatted ("own=%.3e host=%.3e liveChange=%.3f", maxDiff (own, beat), maxDiff (host, beat), maxDiff (live, beat)));
        }
        // a bypass that ends (P1n): the engine ran through the bypass on the input it was given, so once the 20 ms fade is over the output is bit for bit what an instance that
        // was never bypassed gives (the old behaviour, engine.reset() and a click, is gone)
        {
            Rig r (2, true, 48000.0, 512, busy);
            auto first = r.render (slice (beat, 0, 24000), &vocal, 512, true);
            Audio rest = slice (beat, 24000, 48000), restKey = slice (vocal, 24000, 48000);
            auto a = r.render (rest, &restKey, 512);
            auto whole = runPlugin (slice (beat, 0, 72000), &vocal, true, 512, busy);
            const Audio wholeTail = slice (whole, 24000 + 1536, 48000 - 1536);
            check (maxDiff (first, slice (beat, 0, 24000)) == 0.0 && maxDiff (slice (a, 1536, 48000 - 1536), wholeTail) == 0.0, "after a host bypass the engine is warm: from 32 ms after the release the output == an instance that was never bypassed, bit for bit", juce::String::formatted ("diff=%.3e", maxDiff (slice (a, 1536, 48000 - 1536), wholeTail)));
        }
    }

    // 3. the plug-in equals the raw engine (bank + three live bands + a key); defaults equal the engine's defaults
    {
        auto run = [&] (const Settings& st, bool key, const char* name)
        {
            Rig r (2, key, 48000.0, 512, st);
            auto out = r.render (beat, key ? &vocal : nullptr, 512);
            auto raw = runRaw (beat, key ? &vocal : nullptr, 512, paramsOf (r.proc.apvts));
            check (allFinite (out) && maxDiff (out, raw) == 0.0, juce::String (name) + ": plug-in == raw engine bit for bit", juce::String::formatted ("maxAbsDiff=%.3e peak=%.3f", maxDiff (out, raw), peakOf (out)));
        };
        run ({}, true, "defaults, beat + vocal key");
        const Settings st = { { "depth", 9 }, { "margin", 2 }, { "focusLow", 90 }, { "speed", 2 }, { "b1_on", 1 }, { "b1_gain", 4 }, { "b1_freq", 2500 }, { "b1_q", 1.7f },
                              { "b2_on", 1 }, { "b2_type", 2 }, { "b2_freq", 6000 }, { "b2_gain", -5 }, { "b2_dyn", 1 }, { "b2_thr", -35 }, { "b2_range", -6 }, { "b2_trig", 1 },
                              { "b3_on", 1 }, { "b3_type", 4 }, { "b3_freq", 40 }, { "b3_slope", 2 } };
        run (st, true, "bank + three live bands (bell, dynamic key-triggered shelf, 48 dB/oct low cut)");
        Settings tiling = st; tiling.push_back ({ "tiling", 1 });
        run (tiling, true, "the same with TILING on");
        run ({ { "mode", 1 }, { "depth", 12 }, { "margin", 3 } }, false, "SELF, no key");
        // mono main layout
        Audio mono { beat[0] };
        Rig rm (1, false, 48000.0, 512, { { "mode", 1 }, { "depth", 12 } });
        auto om = rm.render (mono, nullptr, 512);
        auto rawm = runRaw (mono, nullptr, 512, paramsOf (rm.proc.apvts));
        check (allFinite (om) && maxDiff (om, rawm) == 0.0, "mono main layout (SELF) == raw engine, 1 channel", juce::String::formatted ("maxAbsDiff=%.3e", maxDiff (om, rawm)));
        // mono key on a stereo main
        Rig rk (2, true, 48000.0, 512, { { "depth", 12 }, { "margin", 0 } });
        Audio monoKey = vocal; monoKey[1] = monoKey[0];
        auto ok2 = rk.render (beat, &monoKey, 512);
        check (allFinite (ok2) && maxDiff (ok2, beat) > 0.0, "stereo main with a stereo sidechain carrying the vocal collides (the output moves)", juce::String::formatted ("maxAbsDiff=%.3f", maxDiff (ok2, beat)));
    }

    // 3b. TARGET (SPEC 8) through the processor
    {
        const Settings off = { { "targetInst", 0 }, { "targetPart", 20 }, { "targetAmount", 18 }, { "targetWidth", 2 }, { "reverbReady", 1 } };
        const Settings busy = { { "depth", 10 }, { "margin", 1 } };
        Settings busyOff = busy; busyOff.insert (busyOff.end(), off.begin(), off.end());
        const auto a = runPlugin (beat, &vocal, true, 512, busy), b = runPlugin (beat, &vocal, true, 512, busyOff);
        check (maxDiff (a, b) == 0.0 && maxDiff (runPlugin (beat, nullptr, false, 512, off), beat) == 0.0, "TARGET OFF with every other TARGET setting moved: output bit-identical to the run without it (and == input with no other cut)", juce::String::formatted ("maxAbsDiff=%.3e", maxDiff (a, b)));
        const char* names[] = { "VOCAL", "BASS", "DRUMS", "KICK", "SNARE", "HATS" };
        for (int inst = 1; inst <= 6; ++inst)
        {
            const Settings st = { { "targetInst", (float) inst }, { "targetPart", 70 }, { "targetAmount", 15 }, { "targetWidth", 0.8f }, { "reverbReady", inst % 2 } };
            Rig r (2, false, 48000.0, 512, st);
            const auto out = r.render (beat, nullptr, 512);
            const auto raw = runRaw (beat, nullptr, 512, paramsOf (r.proc.apvts));
            check (allFinite (out) && maxDiff (out, raw) == 0.0 && maxDiff (out, beat) > 0.0, juce::String ("TARGET ") + names[inst - 1] + (inst % 2 ? " + REVERB-READY" : "") + ": plug-in == raw engine bit for bit, and the layer cut something (output differs from the input)",
                   juce::String::formatted ("maxAbsDiff plug-in vs raw=%.3e, change=%.3f", maxDiff (out, raw), maxDiff (out, beat)));
        }
        {   // P1i SLOPE (SPEC 8e.2): every step through the processor == the raw engine bit for bit, and every step is a different cut
            Audio prev; juce::String lines; bool ok = true;
            for (int sl = 0; sl < dyneq::kSlopeSteps; ++sl)
            {
                const Settings st = { { "targetInst", 4 }, { "targetPart", 16 }, { "targetAmount", 12 }, { "targetSlope", (float) sl } };   // KICK, SUB
                Rig r (2, false, 48000.0, 512, st); const auto out = r.render (beat, nullptr, 512); const auto raw = runRaw (beat, nullptr, 512, paramsOf (r.proc.apvts));
                const bool good = allFinite (out) && maxDiff (out, raw) == 0.0 && maxDiff (out, beat) > 0.0 && (sl == 0 || maxDiff (out, prev) > 0.0) && paramsOf (r.proc.apvts).targetSlope == dyneq::kSlopeDb[sl];
                ok = ok && good; lines << (sl ? ", " : "") << dyneq::kSlopeDb[sl] << ": " << juce::String::formatted ("%.3e", maxDiff (out, raw)); prev = out;
            }
            check (ok, "TARGET SLOPE 12 / 24 / 36 / 48 / 72 (KICK, SUB, AMOUNT 12): plug-in == raw engine bit for bit at every step, and every step differs from the one before", "plug-in vs raw maxAbsDiff " + lines);
            Settings off = { { "targetInst", 0 }, { "targetSlope", 4 }, { "targetAmount", 60 }, { "reverbReady", 1 } };
            check (maxDiff (runPlugin (beat, nullptr, false, 512, off), beat) == 0.0, "TARGET OFF with SLOPE 72 and AMOUNT 60: the output is the input, bit for bit");
        }
        {   // AMOUNT direction on a long loop (the control proof's 1 s material cannot teach the fish memory 3 hits)
            auto rmsOf = [] (const Audio& a) { double e = 0; size_t n = 0; for (auto& c : a) for (float v : c) { e += (double) v * v; ++n; } return std::sqrt (e / (double) std::max<size_t> (n, 1)); };
            double prev = rmsOf (beat); bool mono = true; juce::String vals;
            for (float amt : { 3.0f, 9.0f, 18.0f })
            {
                const Settings st = { { "targetInst", 4 }, { "targetPart", 10 }, { "targetAmount", amt }, { "targetWidth", 1 }, { "reverbReady", 0 } };
                const double r = rmsOf (runPlugin (beat, nullptr, false, 512, st)); mono = mono && r < prev; prev = r; vals += juce::String (20.0 * std::log10 (r / rmsOf (beat)), 2) + " ";
            }
            check (mono, "TARGET AMOUNT 3 / 9 / 18 dB (KICK, SUB) on the demo beat: output rms falls with every step", "rms change dB: " + vals);
        }
        Audio mono { beat[0] };
        Rig rm (1, false, 48000.0, 512, { { "targetInst", 3 }, { "targetAmount", 12 } });
        const auto om = rm.render (mono, nullptr, 512);
        check (allFinite (om) && maxDiff (om, runRaw (mono, nullptr, 512, paramsOf (rm.proc.apvts))) == 0.0, "mono main layout (TARGET DRUMS) == raw engine, 1 channel");
    }

    // 4. MIX / DELTA / KEY LISTEN
    {
        const Settings base = { { "depth", 12 }, { "margin", 0 } };
        auto wet = runPlugin (beat, &vocal, true, 512, base);
        check (maxDiff (wet, beat) > 0.01, "the collision test setting does cut (so the next three tests mean something)", juce::String::formatted ("change=%.3f", maxDiff (wet, beat)));
        Settings m0 = base; m0.push_back ({ "mix", 0 });
        check (maxDiff (runPlugin (beat, &vocal, true, 512, m0), beat) == 0.0, "MIX 0 % == input bit for bit while the bank is cutting");
        Settings m50 = base; m50.push_back ({ "mix", 50 });
        auto h = runPlugin (beat, &vocal, true, 512, m50);
        double worst = 0; for (size_t c = 0; c < 2; ++c) for (size_t i = 0; i < beat[c].size(); ++i) worst = std::max (worst, (double) std::abs (h[c][i] - (beat[c][i] + 0.5f * (wet[c][i] - beat[c][i]))));
        check (worst < 1e-6, "MIX 50 % == input + 0.5 x (wet - input)", juce::String::formatted ("maxAbsDiff=%.3e", worst));
        Settings dl = base; dl.push_back ({ "delta", 1 });
        auto d = runPlugin (beat, &vocal, true, 512, dl);
        worst = 0; for (size_t c = 0; c < 2; ++c) for (size_t i = 0; i < beat[c].size(); ++i) worst = std::max (worst, (double) std::abs (d[c][i] - (beat[c][i] - wet[c][i])));
        check (worst < 1e-6, "DELTA == input - wet", juce::String::formatted ("maxAbsDiff=%.3e", worst));
        Settings kl = base; kl.push_back ({ "keyListen", 1 });
        auto k = runPlugin (beat, &vocal, true, 512, kl);
        check (maxDiff (k, vocal) < 1e-6, "KEY LISTEN == the sidechain signal", juce::String::formatted ("maxAbsDiff=%.3e", maxDiff (k, vocal)));
        auto kn = runPlugin (beat, nullptr, false, 512, kl);
        check (maxDiff (kn, beat) == 0.0, "KEY LISTEN with no sidechain bus changes nothing");
        // a MIX move mid-stream is a ramp (20 ms), not a step
        Rig r (2, true, 48000.0, 512, base);
        auto a = r.render (slice (beat, 0, 24000), &vocal, 512);
        setParam (r.proc.apvts, "mix", 0.0f);
        Audio rest = slice (beat, 24000, 4096), restKey = slice (vocal, 24000, 4096);
        auto b = r.render (rest, &restKey, 512);
        int settled = 0; for (int i = 0; i < 4096; ++i) if (b[0][(size_t) i] == rest[0][(size_t) i]) { settled = i; break; }
        check (settled > 800 && settled < 1100, "MIX 100 -> 0 % moves over about 20 ms (960 samples), not in one step", "samples until the output equals the input: " + juce::String (settled));
    }

    // 5. block sizes 1 ... 4096 (and a host block larger than the processor's scratch): bit-identical
    {
        const Audio b4 = slice (beat, 0, 48000 * 4), v4 = slice (vocal, 0, 48000 * 4);
        const Settings st = { { "depth", 10 }, { "margin", 1 }, { "mix", 70 }, { "b1_on", 1 }, { "b1_gain", 5 }, { "b1_freq", 3000 },
                              { "b2_on", 1 }, { "b2_type", 2 }, { "b2_freq", 5000 }, { "b2_dyn", 1 }, { "b2_thr", -40 }, { "b2_range", -8 }, { "b2_trig", 1 }, { "bakeArm", 1 },
                              { "targetInst", 3 }, { "targetPart", 60 }, { "targetAmount", 12 }, { "targetWidth", 1.4f }, { "reverbReady", 1 } };
        const Audio ref = runPlugin (b4, &v4, true, 512, st);
        check (maxDiff (ref, b4) > 0.01, "block-size test signal is processed (the reference differs from the input)");
        for (int block : { 1, 7, 32, 33, 64, 333, 4096, 5000 })
        {
            const auto o = runPlugin (b4, &v4, true, block, st);
            check (maxDiff (o, ref) == 0.0, "block size " + juce::String (block) + " == block size 512, bit-identical", juce::String::formatted ("maxAbsDiff=%.3e", maxDiff (o, ref)));
        }
        Rig r (2, true, 48000.0, 512, st);   // processed in two different block layouts, the BAKE statistics agree too
        r.render (b4, &v4, 512);
        Rig q (2, true, 48000.0, 512, st);
        q.render (b4, &v4, 97);
        const auto sa = r.proc.bakeSuggest(), sb = q.proc.bakeSuggest();
        bool same = sa.count == sb.count && sa.seconds == sb.seconds;
        for (int i = 0; same && i < sa.count; ++i) same = sa.text[i] == sb.text[i];
        check (same && sa.seconds > 3.9, "BAKE statistics are block-size independent (512 vs 97)", "counted " + juce::String (sa.seconds, 3) + " s vs " + juce::String (sb.seconds, 3) + " s, " + juce::String (sa.count) + " bells");
    }

    // 6. sample rates
    for (double sr : { 44100.0, 48000.0, 96000.0 })
    {
        const Settings st = { { "depth", 12 }, { "margin", 0 }, { "b1_on", 1 }, { "b1_gain", 12 }, { "b2_on", 1 }, { "b2_type", 5 }, { "b2_freq", 18000 }, { "b2_slope", 2 },
                              { "b3_on", 1 }, { "b3_dyn", 1 }, { "b3_range", -12 }, { "b3_thr", -60 } };
        const Audio b2 = slice (beat, 0, 96000), v2 = slice (vocal, 0, 96000);
        auto o = runPlugin (b2, &v2, true, 512, st, sr);
        auto s = runPlugin (b2, nullptr, false, 480, { { "mode", 1 }, { "depth", 12 } }, sr);
        check (allFinite (o) && allFinite (s) && peakOf (o) < 20.0 && peakOf (s) < 20.0, "sample rate " + juce::String (sr, 0) + ": COLLIDE + bands and SELF run without NaN / blow-up", juce::String::formatted ("peaks %.3f / %.3f", peakOf (o), peakOf (s)));
    }

    // 7. parameter round trip and state
    {
        const std::pair<const char*, float> vals[] = {
            { "mode", 1 }, { "depth", 7.3f }, { "margin", 10.5f }, { "focusLow", 211.0f }, { "focusHigh", 5300.0f }, { "speed", 2 }, { "mix", 42.0f }, { "delta", 1 }, { "bypass", 1 },
            { "keyListen", 1 }, { "tiling", 1 }, { "bakeArm", 1 },
            { "targetInst", 5 }, { "targetPart", 83.5f }, { "targetAmount", 13.5f }, { "targetWidth", 1.7f }, { "targetSweet", 237.0f }, { "reverbReady", 1 }, { "targetSlope", 4 },
            { "b1_on", 1 }, { "b1_type", 3 }, { "b1_freq", 333.33f }, { "b1_gain", -7.25f }, { "b1_q", 4.5f }, { "b1_slope", 1 }, { "b1_dyn", 1 }, { "b1_thr", -41.5f }, { "b1_range", -8.5f },
            { "b1_att", 12.5f }, { "b1_rel", 640.0f }, { "b1_trig", 1 },
            { "b6_on", 1 }, { "b6_type", 5 }, { "b6_freq", 17000.0f }, { "b6_gain", 11.0f }, { "b6_q", 0.35f }, { "b6_slope", 2 }, { "b6_dyn", 1 }, { "b6_thr", -3.0f }, { "b6_range", 4.0f },
            { "b6_att", 2.0f }, { "b6_rel", 20.0f }, { "b6_trig", 1 } };
        DyneqAudioProcessor a, b;
        for (auto& kv : vals) setParam (a.apvts, kv.first, kv.second);
        juce::MemoryBlock mb; a.getStateInformation (mb);
        b.setStateInformation (mb.getData(), (int) mb.getSize());
        int bad = 0; double worst = 0.0; const auto sa = snapOf (a), sb = snapOf (b);
        for (auto& kv : vals) { const float va = rawOf (a.apvts, kv.first), vb = rawOf (b.apvts, kv.first); if (va != vb) ++bad; worst = std::max (worst, (double) std::abs (va - vb)); }
        DyneqAudioProcessor fresh; int changed = 0;
        for (auto& kv : vals) if (rawOf (fresh.apvts, kv.first) != rawOf (a.apvts, kv.first)) ++changed;
        check (bad == 0 && snapDiff (sa, sb) == 0 && changed == (int) (sizeof (vals) / sizeof (vals[0])), "state round trip (" + juce::String ((int) (sizeof (vals) / sizeof (vals[0]))) + " parameters set, all compared bit for bit)",
               juce::String::formatted ("mismatches=%d  maxDiff=%.1e  differing from defaults before restore=%d  state bytes=%zu", bad, worst, changed, mb.getSize()));
        // set a value, read the same value back (within the parameter's own snapping)
        int offs = 0;
        for (auto& kv : vals)
        {
            auto* rp = dynamic_cast<juce::RangedAudioParameter*> (fresh.apvts.getParameter (kv.first));
            const float back = rp->convertFrom0to1 (rp->convertTo0to1 (kv.second));
            if (std::abs (back - kv.second) > 0.0051f * std::max (1.0f, std::abs (kv.second)) && ! rp->isDiscrete()) ++offs;
        }
        check (offs == 0, "every set value reads back inside the parameter's step", "outside=" + juce::String (offs));
        // defaults are the engine's defaults (nothing here decides a sound)
        const dyneq::Params d; const auto pd = paramsOf (fresh.apvts);
        auto close = [] (float x, float y) { return std::abs (x - y) <= 1e-5f * std::max (1.0f, std::abs (y)); };
        bool same = pd.mode == d.mode && close (pd.depthDb, d.depthDb) && close (pd.marginDb, d.marginDb) && close (pd.focusLowHz, d.focusLowHz) && close (pd.focusHighHz, d.focusHighHz) && pd.speed == d.speed
                    && pd.tiling == d.tiling && [&] { static const float hz[6] { 80.0f, 200.0f, 500.0f, 1200.0f, 3000.0f, 8000.0f }; for (int i = 0; i < 6; ++i) if (! close (pd.band[i].freqHz, hz[i])) return false; return true; }() && close (pd.band[0].q, d.band[0].q) && close (pd.band[0].thrDb, d.band[0].thrDb) && close (pd.band[0].attackMs, d.band[0].attackMs)
                    && close (pd.band[0].releaseMs, d.band[0].releaseMs) && pd.band[0].on == d.band[0].on && pd.band[0].dyn == d.band[0].dyn && close (pd.band[0].rangeDb, d.band[0].rangeDb) && close (pd.band[0].gainDb, d.band[0].gainDb)
                    && pd.band[0].type == d.band[0].type && pd.band[0].slopeDb == d.band[0].slopeDb && pd.band[0].trigger == d.band[0].trigger
                    && pd.targetInst == d.targetInst && close (pd.targetPart, d.targetPart) && close (pd.targetAmount, d.targetAmount) && close (pd.targetWidth, d.targetWidth) && close (pd.targetSweet, d.targetSweet) && pd.reverbReady == d.reverbReady && pd.targetSlope == d.targetSlope;
        if (! same) std::printf ("   mode %d/%d depth %.9g/%.9g margin %.9g/%.9g low %.9g/%.9g high %.9g/%.9g speed %d/%d freq %.9g/%.9g q %.9g/%.9g thr %.9g/%.9g att %.9g/%.9g rel %.9g/%.9g range %.9g/%.9g\n", pd.mode, d.mode, (double) pd.depthDb, (double) d.depthDb,
            (double) pd.marginDb, (double) d.marginDb, (double) pd.focusLowHz, (double) d.focusLowHz, (double) pd.focusHighHz, (double) d.focusHighHz, pd.speed, d.speed, (double) pd.band[0].freqHz, (double) d.band[0].freqHz, (double) pd.band[0].q, (double) d.band[0].q,
            (double) pd.band[0].thrDb, (double) d.band[0].thrDb, (double) pd.band[0].attackMs, (double) d.band[0].attackMs, (double) pd.band[0].releaseMs, (double) d.band[0].releaseMs, (double) pd.band[0].rangeDb, (double) d.band[0].rangeDb);
        check (same, "parameter defaults == the engine's Params / FreeBand defaults (within 1e-5 relative: the skew round trip); the six band frequencies start at 80 / 200 / 500 / 1200 / 3000 / 8000 Hz (UX redesign 11 Oct; a band is off by default, so this decides no sound)");
    }

    {   // 7b. TARGET memory (SPEC 8d): saved with the project, never in a preset, a fresh instance is empty, a loaded project cuts from its first hit
        auto kickTrack = [] (int hits, double f0, double sr)
        {
            const double spacing = 0.5, lead = 0.5; const int n = (int) ((lead + spacing * hits + 0.6) * sr);
            Audio a (2, std::vector<float> ((size_t) n, 0.0f)); uint32_t rnd = 12345u;
            for (int h = 0; h < hits; ++h)
            {
                const int at = (int) ((lead + spacing * h) * sr);
                for (int i = 0; i < (int) (0.6 * sr) && at + i < n; ++i)
                {
                    const double t = (double) i / sr; rnd = rnd * 1664525u + 1013904223u;
                    const double noise = ((double) (rnd >> 8) / 8388608.0 - 1.0);
                    const double v = 0.55 * std::sin (2.0 * M_PI * f0 * t) * std::exp (-t / 0.12) + (t < 0.004 ? 0.5 * noise * std::exp (-t / 0.0007) : 0.0);
                    a[0][(size_t) (at + i)] += (float) v; a[1][(size_t) (at + i)] += (float) v;
                }
            }
            return a;
        };
        auto subDb = [] (const Audio& out, const Audio& in, int hit, double sr)   // the energy 10-60 ms after the hit's onset, output against input (the boom is the only thing there)
        {
            const int a0 = (int) ((0.5 + 0.5 * hit + 0.010) * sr), a1 = (int) ((0.5 + 0.5 * hit + 0.060) * sr); double eo = 0.0, ei = 0.0;
            for (int i = a0; i < a1; ++i) { eo += (double) out[0][(size_t) i] * out[0][(size_t) i]; ei += (double) in[0][(size_t) i] * in[0][(size_t) i]; }
            return 10.0 * std::log10 ((eo + 1e-30) / (ei + 1e-30));
        };
        const Settings kickSub = { { "targetInst", 4 }, { "targetPart", 16.0f }, { "targetAmount", 9.0f } };   // KICK, part 1 of 3 (SUB)
        const Audio kicks = kickTrack (12, 55.0, 48000.0);
        DyneqAudioProcessor freshOne;
        auto stateHas = [] (juce::MemoryBlock& mb) { auto xml = juce::AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize()); return xml != nullptr && xml->hasAttribute ("targetMemory"); };
        juce::MemoryBlock freshState; freshOne.getStateInformation (freshState);
        check (freshOne.targetMemoryEmpty() && ! stateHas (freshState), "TARGET memory: a fresh instance is empty and its state holds no learned points", "state bytes=" + juce::String ((int) freshState.getSize()));
        Rig r1 (2, false, 48000.0, 512, kickSub);
        const Audio o1 = r1.render (kicks, nullptr, 512);
        juce::MemoryBlock st1; r1.proc.getStateInformation (st1);
        check (! r1.proc.targetMemoryEmpty() && stateHas (st1), "TARGET memory: after 12 kicks the instance holds a memory and writes it into its state", "state bytes=" + juce::String ((int) st1.getSize()));
        check (! r1.proc.apvts.copyState().hasProperty ("targetMemory"), "TARGET memory: not part of the parameter tree (a preset is the parameters only)");
        DyneqAudioProcessor presetTarget; presetTarget.apvts.replaceState (r1.proc.apvts.copyState());
        juce::MemoryBlock stP; presetTarget.getStateInformation (stP);
        check (presetTarget.targetMemoryEmpty() && ! stateHas (stP) && rawOf (presetTarget.apvts, "targetInst") == 4.0f, "TARGET memory: the parameters of that instance applied as a preset leave the new instance empty (settings arrive, memory does not)");
        DyneqAudioProcessor mid; mid.setStateInformation (st1.getData(), (int) st1.getSize()); juce::MemoryBlock st2; mid.getStateInformation (st2);
        auto memText = [] (juce::MemoryBlock& mb) { auto xml = juce::AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize()); return xml != nullptr ? xml->getStringAttribute ("targetMemory") : juce::String(); };
        check (memText (st1).isNotEmpty() && memText (st1) == memText (st2), "TARGET memory: state round trip carries the memory unchanged (save, load, save: the same text)", "text length " + juce::String (memText (st1).length()));
        Rig r2 (2, false, 48000.0, 512);
        r2.proc.setStateInformation (st1.getData(), (int) st1.getSize());
        const Audio k4 = kickTrack (4, 55.0, 48000.0); const Audio o2 = r2.render (k4, nullptr, 512);
        const double ref = subDb (o1, kicks, 8, 48000.0), first = subDb (o2, k4, 0, 48000.0);
        check (first <= -5.0 && std::abs (first - ref) <= 0.5, "TARGET memory: a fresh instance that loaded the state cuts the sub on its very first hit like the original does on its 9th", juce::String::formatted ("first hit %.2f dB, original hit 9 %.2f dB, difference %.2f dB (want <= -5 and within 0.5)", first, ref, std::abs (first - ref)));
        Rig r3 (2, false, 48000.0, 512, kickSub);
        const Audio o3 = r3.render (k4, nullptr, 512); const double empty1 = subDb (o3, k4, 0, 48000.0), empty4 = subDb (o3, k4, 3, 48000.0);
        check (std::abs (empty1) <= 0.1 && empty4 <= -5.0, "TARGET memory: an empty instance does not cut the first hit and has learned by the 4th (the other side of the test above)", juce::String::formatted ("hit 1 %.2f dB, hit 4 %.2f dB", empty1, empty4));
    }

    {   // 8. value text: what the host shows is the string the panel shows, and reads back
        DyneqAudioProcessor a;
        auto txt = [&] (const char* id, float plain) { auto* p = dynamic_cast<juce::RangedAudioParameter*> (a.apvts.getParameter (id)); return p->getText (p->convertTo0to1 (plain), 64); };
        auto fromText = [&] (const char* id, const juce::String& t) { auto* p = dynamic_cast<juce::RangedAudioParameter*> (a.apvts.getParameter (id)); return p->convertFrom0to1 (p->getValueForText (t)); };
        struct Ex { const char* id; float plain; const char* text; };
        const Ex shown[] = { { "depth", 6.0f, "6.0 dB" }, { "margin", 3.0f, "3.0 dB" }, { "focusLow", 120.0f, "120 Hz" }, { "focusHigh", 8000.0f, "8.00 kHz" }, { "mix", 100.0f, "100 %" }, { "mix", 47.5f, "47.5 %" },
                             { "b1_freq", 1000.0f, "1.00 kHz" }, { "b1_freq", 20.0f, "20.0 Hz" }, { "b2_gain", -6.0f, "-6.0 dB" }, { "b3_q", 1.0f, "1.00" }, { "b4_thr", -30.0f, "-30.0 dB" },
                             { "b5_att", 10.0f, "10.0 ms" }, { "b5_rel", 100.0f, "100 ms" }, { "b5_rel", 1000.0f, "1.00 s" }, { "mode", 1.0f, "SELF" }, { "speed", 1.0f, "MEDIUM" },
                             { "targetInst", 4.0f, "KICK" }, { "targetPart", 50.0f, "50 %" }, { "targetAmount", 9.0f, "9.0 dB" }, { "targetWidth", 1.0f, "1.00" }, { "targetSweet", 120.0f, "120 ms" }, { "targetSlope", 1.0f, "24 dB/oct" }, { "targetSlope", 4.0f, "72 dB/oct" }, { "targetAmount", 60.0f, "60.0 dB" }, { "b1_type", 4.0f, "LOW CUT" }, { "b1_slope", 2.0f, "48 dB/oct" }, { "b1_trig", 1.0f, "KEY" } };
        for (const auto& e : shown) { const auto got = txt (e.id, e.plain); check (got == e.text, juce::String ("host text ") + e.id + " " + juce::String (e.plain), "\"" + got + "\" want \"" + e.text + "\""); }
        int bad = 0, total = 0;
        for (auto* ap : a.getParameters())
        {
            auto* rp = dynamic_cast<juce::RangedAudioParameter*> (ap); auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (ap);
            if (rp == nullptr || withId == nullptr || rp->isBoolean()) continue;
            for (float n : { 0.0f, rp->getDefaultValue(), 1.0f })
            {
                const auto t = rp->getText (n, 64); const float back = rp->getValueForText (t); const float tol = rp->isDiscrete() ? 1.0e-6f : 0.006f;
                ++total; if (! (t.isNotEmpty() && ! t.containsIgnoreCase ("nan") && std::abs (back - n) <= tol)) { ++bad; std::printf ("   text round trip %s @%.3f: \"%s\" -> %.4f\n", withId->paramID.toRawUTF8(), (double) n, t.toRawUTF8(), (double) back); }
            }
        }
        check (bad == 0, "text round trip at min / default / max of every non-switch parameter", juce::String (total) + " values, bad=" + juce::String (bad));
        auto near = [] (float x, float y, float tol = 1.0e-3f) { return std::abs (x - y) <= tol; };
        check (near (fromText ("depth", "9 dB"), 9.0f) && near (fromText ("depth", "9"), 9.0f) && near (fromText ("depth", juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92" "3"))), 0.0f) && near (fromText ("depth", "99"), 12.0f),
               "typed DEPTH: 9 dB / 9 / minus sign clamps to 0 / 99 clamps to 12");
        check (near (fromText ("b1_freq", "2.4 kHz"), 2400.0f, 0.5f) && near (fromText ("b1_freq", "2400"), 2400.0f, 0.5f) && near (fromText ("b1_freq", "1k"), 1000.0f, 0.5f), "typed FREQ: 2.4 kHz / 2400 / 1k");
        check (near (fromText ("b1_att", "0.02 s"), 20.0f, 0.2f) && near (fromText ("b1_att", "5"), 5.0f, 0.2f) && near (fromText ("b1_rel", "0.5 s"), 500.0f, 1.0f), "typed ATTACK / RELEASE: 0.02 s, bare = ms, 0.5 s");
        check (near (fromText ("b1_type", "low cut"), 4.0f) && near (fromText ("b1_type", "notch"), 3.0f) && near (fromText ("b1_slope", "24"), 1.0f) && near (fromText ("mode", "self"), 1.0f) && near (fromText ("b2_trig", "key"), 1.0f),
               "typed choices: low cut, notch, slope 24, mode self, trigger key");
        float junk = 0; int ji = 0;
        check (! dyneq::ptext::parse ("depth", "banana", junk) && ! dyneq::ptext::parse ("depth", "12 apples", junk) && ! dyneq::ptext::parse ("mix", "50 ms", junk) && ! dyneq::ptext::parse ("b1_q", "2 Hz", junk)
                   && ! dyneq::ptext::parse ("b1_freq", "", junk) && ! dyneq::ptext::parseChoice ("mode", "x", ji) && ! dyneq::ptext::parseChoice ("b1_type", "l", ji),
               "garbage is refused by the strict parser (words, wrong unit, empty, ambiguous prefix)");
        int labels = 0; for (auto* ap : a.getParameters()) if (ap->getLabel().isNotEmpty()) ++labels;
        check (labels == 0, "no separate unit label on any parameter (the unit is in the text)");
    }

    // 9. BAKE through the processor (SPEC 1 C, item 14): statistics, suggestion, ONE undoable step, bit-exact undo
    {
        const double secs = 16.0, sr = 48000.0; const int n = (int) (secs * sr);
        const Audio main1k = sineAudio (1000.0, -12.0, secs);
        Audio key1k = sineAudio (1000.0, -18.0, secs);   // on for the first 1.6 s of every 4 s (the cut then lasts about half the time, see tools/measure.py A15)
        for (int i = 0; i < n; ++i) if ((i % (4 * 48000)) >= (int) (1.6 * 48000)) key1k[0][(size_t) i] = key1k[1][(size_t) i] = 0.0f;
        const Settings col = { { "depth", 12 }, { "margin", 3 }, { "speed", 0 } };
        auto armed = [&] { Settings s = col; s.push_back ({ "bakeArm", 1 }); return s; }();

        Rig r (2, true, sr, 333, armed);
        r.render (main1k, &key1k, 333);
        const auto s = r.proc.bakeSuggest();
        // the same statistics from the raw engine (what dyneqraw --bake and measure.py A15 use)
        dyneq::Engine eng; const auto praw = paramsOf (r.proc.apvts);
        runRaw (main1k, &key1k, 333, praw, sr, &eng, true);
        float mean[dyneq::kBands], pct[dyneq::kBands]; dyneq::BakeBell rb[dyneq::kBakeMaxBells];
        for (int b = 0; b < dyneq::kBands; ++b) { mean[b] = eng.bakeMeanCutDb (b); pct[b] = eng.bakePercentCut (b); }
        const int nrb = dyneq::bakeFit (mean, pct, eng.bandCentresHz(), sr, 6, rb);
        bool same = s.count == nrb && s.count >= 1;
        for (int i = 0; same && i < s.count; ++i) same = s.bells[i].freqHz == rb[i].freqHz && s.bells[i].gainDb == rb[i].gainDb && s.bells[i].q == rb[i].q;
        check (same, "BAKE suggestion of the plug-in == the raw engine's (frequency, gain, Q bit for bit)", juce::String (s.count) + " bell(s): " + (s.count ? juce::String (s.text[0]) : juce::String ("-")));
        float peakMean = 0; for (int b = 0; b < dyneq::kBands; ++b) peakMean = std::max (peakMean, mean[b]);
        check (s.count >= 1 && std::abs (s.bells[0].freqHz - 1000.0f) < 90.0f && std::abs (-s.bells[0].gainDb - peakMean) <= 1.0f && std::abs (s.seconds - secs) < 0.01,
               "first bell within one band of 1 kHz, gain within 1 dB of the largest mean cut, counted time == the run",
               juce::String::formatted ("f=%.0f Hz gain=%.2f dB largest mean cut=%.2f dB counted=%.3f s", s.count ? (double) s.bells[0].freqHz : 0.0, s.count ? (double) s.bells[0].gainDb : 0.0, (double) peakMean, s.seconds));

        // apply = ONE undo step; undo restores every parameter bit for bit; redo reapplies; the bands that were not empty are not touched
        const auto before = snapOf (r.proc);
        const int stepsBefore = r.proc.undo.undoSteps();
        const int written = r.proc.bakeApply();
        const auto applied = snapOf (r.proc);
        bool slotsOk = written == s.count;
        for (int i = 0; i < written; ++i)
        {
            const juce::String nm = "b" + juce::String (s.slot[i] + 1) + "_";
            slotsOk = slotsOk && rawOf (r.proc.apvts, (nm + "on").toRawUTF8()) == 1.0f && rawOf (r.proc.apvts, (nm + "type").toRawUTF8()) == 0.0f
                      && std::abs (rawOf (r.proc.apvts, (nm + "freq").toRawUTF8()) - s.bells[i].freqHz) < 0.011f && std::abs (rawOf (r.proc.apvts, (nm + "gain").toRawUTF8()) - s.bells[i].gainDb) < 0.011f
                      && rawOf (r.proc.apvts, (nm + "dyn").toRawUTF8()) == 0.0f;
        }
        check (slotsOk && r.proc.undo.undoSteps() == stepsBefore + 1 && snapDiff (before, applied) > 0, "BAKE APPLY writes the bells into the empty bands as ONE undo step", "bells written=" + juce::String (written) + ", steps " + juce::String (stepsBefore) + " -> " + juce::String (r.proc.undo.undoSteps()));
        r.proc.undo.undo();
        check (snapDiff (before, snapOf (r.proc), &r.proc) == 0, "undo restores every parameter bit for bit (normalised and raw, all 83)");
        r.proc.undo.redo();
        check (snapDiff (applied, snapOf (r.proc)) == 0, "redo reapplies the baked bells bit for bit");
        r.proc.undo.undo();

        // the baked static curve through the real processor: a 1 kHz tone, no key, bank off, against the analytic curve of the suggestion
        {
            r.proc.bakeApply();
            Rig st (2, false, sr, 512, {});
            for (int i = 0; i < s.count; ++i)
            {
                const juce::String nm = "b" + juce::String (s.slot[i] + 1) + "_";
                setParam (st.proc.apvts, (nm + "on").toRawUTF8(), 1); setParam (st.proc.apvts, (nm + "freq").toRawUTF8(), rawOf (r.proc.apvts, (nm + "freq").toRawUTF8()));
                setParam (st.proc.apvts, (nm + "gain").toRawUTF8(), rawOf (r.proc.apvts, (nm + "gain").toRawUTF8())); setParam (st.proc.apvts, (nm + "q").toRawUTF8(), rawOf (r.proc.apvts, (nm + "q").toRawUTF8()));
            }
            setParam (st.proc.apvts, "depth", 0);
            double worstDev = 0; juce::String lines;
            for (double f : { 800.0, 923.0, 1000.0, 1086.0, 1272.0 })
            {
                const Audio tone = sineAudio (f, -12.0, 1.0);
                Rig one (2, false, sr, 512, {});
                for (int i = 0; i < s.count; ++i)
                {
                    const juce::String nm = "b" + juce::String (s.slot[i] + 1) + "_";
                    setParam (one.proc.apvts, (nm + "on").toRawUTF8(), 1); setParam (one.proc.apvts, (nm + "freq").toRawUTF8(), rawOf (r.proc.apvts, (nm + "freq").toRawUTF8()));
                    setParam (one.proc.apvts, (nm + "gain").toRawUTF8(), rawOf (r.proc.apvts, (nm + "gain").toRawUTF8())); setParam (one.proc.apvts, (nm + "q").toRawUTF8(), rawOf (r.proc.apvts, (nm + "q").toRawUTF8()));
                }
                setParam (one.proc.apvts, "depth", 0);
                const auto o = one.render (tone, nullptr, 512);
                const double measured = rmsDb (o, 24000, 48000) - rmsDb (tone, 24000, 48000);
                double expect = 0; for (int i = 0; i < s.count; ++i) { const juce::String nm = "b" + juce::String (s.slot[i] + 1) + "_";
                    expect += dyneq::bellResponseDb (rawOf (r.proc.apvts, (nm + "freq").toRawUTF8()), rawOf (r.proc.apvts, (nm + "gain").toRawUTF8()), rawOf (r.proc.apvts, (nm + "q").toRawUTF8()), f, sr); }
                worstDev = std::max (worstDev, std::abs (measured - expect)); lines << juce::String::formatted (" %.0f Hz %.2f/%.2f", f, measured, expect);
            }
            check (worstDev < 0.05, "the baked bells through the processor match their analytic curve (measured/expected dB)", juce::String::formatted ("worst %.3f dB;", worstDev) + lines);
            r.proc.undo.undo();
        }

        // only empty bands are written
        {
            Rig q (2, true, sr, 333, armed);
            setParam (q.proc.apvts, "b1_on", 1); setParam (q.proc.apvts, "b1_freq", 3000); setParam (q.proc.apvts, "b1_gain", 3);
            q.render (main1k, &key1k, 333);
            const auto sq = q.proc.bakeSuggest();
            const auto pre = snapOf (q.proc);
            const float f1 = rawOf (q.proc.apvts, "b1_freq");
            q.proc.bakeApply();
            check (sq.slot[0] == 1 && sq.emptyBands == 5 && rawOf (q.proc.apvts, "b1_freq") == f1 && std::abs (rawOf (q.proc.apvts, "b1_gain") - 3.0f) < 0.011f && rawOf (q.proc.apvts, "b2_on") == 1.0f,
                   "with band 1 already on, BAKE fills band 2 and leaves band 1 alone", "first slot used: band " + juce::String (sq.slot[0] + 1) + ", empty bands " + juce::String (sq.emptyBands));
            q.proc.undo.undo();
            check (snapDiff (pre, snapOf (q.proc)) == 0, "... and its undo is bit-exact too");
            for (int i = 1; i <= 6; ++i) setParam (q.proc.apvts, ("b" + juce::String (i) + "_on").toRawUTF8(), 1);
            const int steps = q.proc.undo.undoSteps();
            check (q.proc.bakeSuggest().count == 0 && q.proc.bakeApply() == 0 && q.proc.undo.undoSteps() == steps, "all six bands on: no suggestion, APPLY writes nothing and adds no undo step");
        }

        // when statistics are collected: armed, transport running, COLLIDE only; arming resets
        {
            auto counted = [&] (Rig& rr) { return rr.proc.bakeSuggest().seconds; };
            const Audio m2 = slice (main1k, 0, 48000 * 2); Audio k2 = slice (key1k, 0, 48000 * 2);
            Rig notArmed (2, true, sr, 512, col); notArmed.render (m2, &k2, 512);
            Rig stopped (2, true, sr, 512, armed); stopped.ph.playing = false; stopped.render (m2, &k2, 512);
            Rig selfMode (2, true, sr, 512, armed); setParam (selfMode.proc.apvts, "mode", 1); selfMode.render (m2, &k2, 512);
            Rig running (2, true, sr, 512, armed); running.render (m2, &k2, 512);
            const double c1 = counted (running);
            setParam (running.proc.apvts, "bakeArm", 0); running.render (m2, &k2, 512);
            const double c2 = counted (running);       // disarmed: statistics stand still
            setParam (running.proc.apvts, "bakeArm", 1); running.render (slice (m2, 0, 4800), &k2, 512);
            const double c3 = counted (running);       // armed again: reset, then 0.1 s
            running.proc.bakeResetStats(); running.render (slice (m2, 0, 480), &k2, 480);
            const double c4 = counted (running);       // reset request, then 10 ms
            check (counted (notArmed) == 0.0 && counted (stopped) == 0.0 && counted (selfMode) == 0.0 && std::abs (c1 - 2.0) < 0.01 && c2 == c1 && std::abs (c3 - 0.1) < 0.01 && std::abs (c4 - 0.01) < 0.002,
                   "statistics: nothing when not armed / transport stopped / SELF; 2 s counted when running; frozen when disarmed; re-arming resets; RESET STATS resets",
                   juce::String::formatted ("notArmed %.3f stopped %.3f self %.3f running %.3f -> disarmed %.3f -> rearmed %.3f -> reset %.3f s", counted (notArmed), counted (stopped), counted (selfMode), c1, c2, c3, c4));
        }
        // no play-head at all (a standalone or a test host): the transport counts as running
        {
            Rig np (2, true, sr, 512, armed, false);
            np.render (slice (main1k, 0, 48000), &key1k, 512);
            check (np.proc.bakeSuggest().seconds > 0.99, "no play-head: statistics are collected (the transport counts as running)", "counted " + juce::String (np.proc.bakeSuggest().seconds, 3) + " s");
        }
    }

    {   // 10. Cmd+Z / Cmd+Shift+Z (SvlUndoTests.h). The "recall" is a Group around several writes.
        const auto u = svl::undotest::run<DyneqAudioProcessor> ("DYNEQ", [] { return std::make_unique<DyneqAudioProcessor>(); },
                                                                { { "depth", 9.5f }, { "margin", 5.0f }, { "b1_freq", 2500.0f }, { "targetSlope", 4.0f }, { "targetAmount", 33.0f } },
                                                                [] (DyneqAudioProcessor& p)
                                                                {
                                                                    svl::ParamUndo::Group oneStep (p.undo, "Test recall");
                                                                    setParam (p.apvts, "mode", 1.0f);
                                                                    setParam (p.apvts, "b2_on", 1.0f);
                                                                    setParam (p.apvts, "b2_gain", -4.0f);
                                                                    setParam (p.apvts, "speed", 2.0f);
                                                                });
        gPass += u.pass;
        gFail += u.fail;
        check (u.fail == 0, "undo_redo: " + juce::String (u.pass) + " of " + juce::String (u.pass + u.fail) + " undo checks passed");
    }

    // =====================================================================================================================
    // 11. P4: the panel (LAB; the ORBIT face was removed on 8 Nov 2026) and the processor features behind it
    {
        // ---- mouse events by hand (the editor is not on a screen)
        auto mk = [] (juce::Component& c, juce::Point<float> pos, juce::Point<float> downPos, int clicks = 1, bool dragged = false, juce::ModifierKeys m = {})
        {
            auto src = juce::Desktop::getInstance().getMainMouseSource();
            return juce::MouseEvent (src, pos, m, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, juce::Time::getCurrentTime(), downPos, juce::Time::getCurrentTime(), clicks, dragged);
        };
        auto residentMB = [] {
#if JUCE_MAC
            malloc_statistics_t st {}; malloc_zone_statistics (nullptr, &st); return (double) st.size_in_use / 1048576.0;
#else
            return 0.0;   // Windows CI: no heap statistics here, so 11a's memory part reads 0 growth; the Mac run is the gate
#endif
        };   // bytes the program holds on the heap right now (freed memory the allocator keeps is not counted)
        auto findCtl = [] (DyneqEditor& ed, const juce::String& action, const juce::String& param = {}) -> juce::Component*
        {
            juce::Component* found = nullptr;
            ed.forEachControl ([&] (juce::Component& c, PanelControl& pc)
            {
                if (found != nullptr) return;
                if (action.isNotEmpty() && pc.uiAction() == action) found = &c;
                else if (param.isNotEmpty() && pc.boundParams().size() == 1 && pc.boundParams()[0] == param) found = &c;
            });
            return found;
        };
        auto settle = [] (DyneqEditor& ed) { ed.refreshFace(); ed.tickForSnapshot(); };

        // 11a. open / close the editor 4 x 20 times: no object and no memory left behind
        {
            DyneqAudioProcessor proc;
            const int before = DyneqEditor::liveObjects();
            for (int i = 0; i < 5; ++i) { DyneqEditor ed (proc); ed.tickForSnapshot(); }   // warm-up: allocator and glyph caches
            double win[4]; double last = residentMB();
            for (int w = 0; w < 4; ++w)
            {
                for (int i = 0; i < 20; ++i) { DyneqEditor ed (proc); ed.tickForSnapshot(); ed.tickForSnapshot(); (void) ed.createComponentSnapshot (ed.getLocalBounds(), true, 1.0f); }
                const double now = residentMB(); win[w] = now - last; last = now;
            }
            { const double h0 = residentMB(); juce::ImageCache::releaseUnusedImages(); const double h1 = residentMB(); juce::LookAndFeel::setDefaultLookAndFeel (nullptr); std::printf ("   (heap after the windows %.1f MB; ImageCache::releaseUnusedImages frees %.1f MB)\n", h0, h0 - h1); }
            check (DyneqEditor::liveObjects() == before && win[3] < 1.0, "editor open/close, 4 x 20 cycles: every panel object released; heap in use grew " + juce::String (win[0], 2) + " / " + juce::String (win[1], 2) + " / " + juce::String (win[2], 2) + " / " + juce::String (win[3], 2) + " MB per 20 cycles (the last must stay under 1)",
                   "live objects " + juce::String (before) + " -> " + juce::String (DyneqEditor::liveObjects()));
        }

        // 11b. the "face" parameter (LAB / ORBIT) is gone (8 Nov 2026). A state saved by a tester build still holds it, as a PARAM child and in exactNorm:
        // it loads with "face" ignored and every other parameter bit-identical, and a later save no longer carries it.
        {
            DyneqAudioProcessor a1; juce::Random rnd (811);
            for (auto* ap : a1.getParameters()) ap->setValueNotifyingHost (rnd.nextFloat());
            a1.editorScalePct = 85; a1.uiMode = 3;
            juce::MemoryBlock mb; a1.getStateInformation (mb);
            auto xml = juce::AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize());
            bool injected = false;
            if (xml != nullptr)
            {
                auto* face = xml->createNewChildElement ("PARAM"); face->setAttribute ("id", "face"); face->setAttribute ("value", 1.0);   // <PARAM id="face" value="1"/>: ORBIT, as a tester build saved it
                xml->setAttribute ("exactNorm", xml->getStringAttribute ("exactNorm") + "face=3f800000,3f800000;");
                injected = xml->getChildByAttribute ("id", "face") != nullptr;
            }
            juce::MemoryBlock old; if (xml != nullptr) juce::AudioProcessor::copyXmlToBinary (*xml, old);
            DyneqAudioProcessor a2; a2.setStateInformation (old.getData(), (int) old.getSize());
            const int diffs = snapDiff (snapOf (a1), snapOf (a2), &a2);
            juce::MemoryBlock again; a2.getStateInformation (again);
            const auto xml2 = juce::AudioProcessor::getXmlFromBinary (again.getData(), (int) again.getSize());
            const bool gone = xml2 != nullptr && xml2->getChildByAttribute ("id", "face") == nullptr && ! xml2->getStringAttribute ("exactNorm").contains ("face=");
            DyneqEditor ed (a2); settle (ed);
            check (injected && a2.apvts.getParameter ("face") == nullptr && diffs == 0 && a2.editorScalePct.load() == 85 && a2.uiMode.load() == 3 && gone && ed.getWidth() == DyneqEditor::kW,
                   "a tester-build state with <PARAM id=\"face\" value=\"1\"/> (ORBIT) loads: \"face\" is ignored, every one of the " + juce::String ((int) a2.getParameters().size()) + " parameters is bit-identical (" + juce::String (diffs) + " different), size 85 % and mode kept, the editor opens, and a new save no longer holds \"face\"");
        }

        // 11c. every visible control is bound
        {
            DyneqAudioProcessor proc; DyneqEditor ed (proc); settle (ed);
            int n = 0, unbound = 0, badId = 0, textBoxes = 0;
            for (int mode : { 0, 4, 6, 7, 8 })
            {
                ed.ctl().setMode (mode); ed.refreshFace();
                ed.forEachControl ([&] (juce::Component&, PanelControl& pc)
                {
                    ++n;
                    if (pc.boundParams().isEmpty() && pc.uiAction().isEmpty()) ++unbound;
                    for (auto& id : pc.boundParams()) if (proc.apvts.getParameter (id) == nullptr) ++badId;
                });
                std::function<void (juce::Component&)> walk = [&] (juce::Component& c) { for (auto* ch : c.getChildren()) { if (ch->isVisible() && dynamic_cast<juce::TextEditor*> (ch) != nullptr) ++textBoxes; if (ch->isVisible()) walk (*ch); } };
                walk (ed);
            }
            check (n > 0 && unbound == 0 && badId == 0 && textBoxes == 0, "LAB: every visible control is bound to a parameter or declares what it does (" + juce::String (n) + " control instances over 5 modes, " + juce::String (unbound) + " unbound, " + juce::String (badId) + " unknown ids); no text box open until one is clicked");
        }

        // 11d. the SubverseLab mark, bottom right of the display: icon only, a pointing hand over it, a click calls the link to subverselab.com (caught, no browser)
        {
            DyneqAudioProcessor proc; DyneqEditor ed (proc); settle (ed);
            auto& scr = ed.labScreen();
            const auto area = ed.testLockupArea(); const auto local = area - scr.getPosition().toFloat();
            int calls = 0; ed.setLockupLinkForTest ([&calls] { ++calls; });
            const auto c0 = local.getCentre();
            scr.mouseMove (mk (scr, c0, c0));
            const bool hand = scr.getMouseCursor() == juce::MouseCursor (juce::MouseCursor::PointingHandCursor);
            scr.mouseDown (mk (scr, c0, c0)); scr.mouseUp (mk (scr, c0, c0));
            const int afterClick = calls;
            const auto away = juce::Point<float> (local.getX() - 60.0f, local.getCentreY());   // the frequency scale left of the mark: no link
            scr.mouseDown (mk (scr, away, away)); scr.mouseUp (mk (scr, away, away));
            ed.setLockupLinkForTest ({});
            const float aspect = area.getWidth() / juce::jmax (1.0f, area.getHeight());
            check (! area.isEmpty() && aspect < 1.6f && area.getRight() <= (float) (scr.getRight()) && area.getBottom() <= (float) scr.getBottom() && hand && afterClick == 1 && calls == 1,
                   "the SubverseLab mark: icon only (" + juce::String (area.getWidth(), 1) + " x " + juce::String (area.getHeight(), 1) + " px at " + juce::String (area.getX(), 0) + ", " + juce::String (area.getY(), 0) + ", bottom right of the display), a pointing hand over it, a click calls the subverselab.com link once, a click beside it does not",
                   "hand " + juce::String ((int) hand) + ", calls " + juce::String (afterClick) + " / " + juce::String (calls));
        }

        // 11e. the shape / slope lists and the TARGET list write the real parameters, each pick as one undo step
        {
            DyneqAudioProcessor proc; DyneqEditor ed (proc); settle (ed); auto& c = ed.ctl();
            auto pickFrom = [&] (std::vector<panel::ChooserItem> items, const juce::String& text)
            {
                ed.openItems (std::move (items), { 300, 700 }); int idx = -1;
                for (int i = 0; i < ed.chooserCount(); ++i) if (ed.chooserText (i) == text && ! ed.chooserHeading (i)) idx = i;
                const int steps = proc.undo.undoSteps();
                if (idx >= 0) ed.chooserPick (idx);
                return idx < 0 ? -1 : proc.undo.undoSteps() - steps;
            };
            auto shape = [&] (const juce::String& t) { return pickFrom (c.shapeItems (0), t); };
            auto slope = [&] (const juce::String& t) { return pickFrom (c.slopeItems (0), t); };
            c.setMode (0); setParam (proc.apvts, "b1_on", 0);
            const int s1 = shape ("BELL");
            const bool bell = c.on ("b1_on") && c.bandType (0) == dyneq::kBell && std::abs (c.raw ("b1_gain") - 3.0f) < 1e-4f;
            const int s2 = shape ("LOW SHELF"); const bool ls = c.bandType (0) == dyneq::kLowShelf && std::abs (c.raw ("b1_gain") - 3.0f) < 1e-4f;   // had a gain: kept
            const int s3 = shape ("LOW CUT"); const bool lc = c.bandType (0) == dyneq::kLowCut;
            const int s4 = shape ("HIGH SHELF"); const bool hs = c.bandType (0) == dyneq::kHighShelf && std::abs (c.raw ("b1_gain") - 2.0f) < 1e-4f;   // from a no-gain type: +2
            shape ("NOTCH"); const int s5 = shape ("LOW SHELF"); const bool ls2 = std::abs (c.raw ("b1_gain") + 3.0f) < 1e-4f;                          // from a notch: -3
            const int s6 = shape ("HIGH CUT"); const bool hc = c.bandType (0) == dyneq::kHighCut;
            check (s1 == 1 && s2 == 1 && s3 == 1 && s4 == 1 && s5 == 1 && s6 == 1 && bell && ls && lc && hs && ls2 && hc, "SHAPE list: BELL / LOW SHELF / HIGH SHELF from a type without gain start at +3 / -3 / +2 dB, LOW CUT and HIGH CUT set the type, each pick is one undo step");
            int u[5]; bool ok = true; int k = 0;
            shape ("LOW CUT");
            for (int db : { 24, 36, 48, 72, 12 })
            {
                u[k++] = slope (juce::String (db) + " dB/oct");
                ok = ok && c.bandSlopeDb (0) == db && juce::roundToInt (c.raw ("b1_slope")) == dyneq::freeSlopeIndexFromDb (db);
            }
            check (ok && u[0] == 1 && u[1] == 1 && u[2] == 1 && u[3] == 1 && u[4] == 1, "SLOPE list: 12 / 24 / 36 / 48 / 72 dB/oct write the SLOPE choice (12 24 48 36 72 order), each pick is one undo step");
            proc.undo.undo(); check (c.bandSlopeDb (0) == 72, "one undo goes back one slope pick (12 -> 72)");
            // OFF band: a shape pick switches it on
            setParam (proc.apvts, "b2_on", 0); pickFrom (c.shapeItems (1), "NOTCH"); check (c.on ("b2_on") && c.bandType (1) == dyneq::kNotch, "a shape pick on an off band switches it on");
            // TARGET list
            c.setMode (panel::kTarget);
            ed.openChooser ({ 300, 700 });
            bool names = ed.chooserCount() == 26 && ed.chooserText (0) == "OFF";
            for (const char* t : { "KICK - SUB", "KICK - BODY", "KICK - MUD", "KICK - BOX", "KICK - CLICK", "SNARE - BODY", "SNARE - BOX", "SNARE - CRACK", "SNARE - SNAP/AIR", "SNARE - REVERB-READY", "HATS - CHICK", "HATS - BITE", "HATS - SHIMMER", "BASS - SUB", "BASS - BODY", "BASS - MUD", "BASS - TOP",
                                   "VOCAL - BOX", "VOCAL - NASAL", "VOCAL - HARSH", "VOCAL - S", "VOCAL - REVERB-READY", "DRUMS - KICK BODY", "DRUMS - SNARE BODY", "DRUMS - HATS SHIMMER" })
            { bool f = false; for (int i = 0; i < ed.chooserCount(); ++i) f = f || ed.chooserText (i) == t; names = names && f; }
            check (names, "TARGET list: OFF + every instrument and part of the engine (KICK 5, SNARE 4 + REVERB-READY, HATS 3, BASS 4, VOCAL 4 + REVERB-READY, DRUMS 3) = 26 rows (12 Oct: MUD / BOX / BITE added from the published ranges)");
            auto pickT = [&] (const juce::String& t) { ed.openChooser ({ 300, 700 }); for (int i = 0; i < ed.chooserCount(); ++i) if (ed.chooserText (i) == t) ed.chooserPick (i); };
            pickT ("KICK - CLICK"); const bool kk = c.raw ("targetInst") == (float) dyneq::kTgtKick && dyneq::partIndex (dyneq::kTgtKick, c.raw ("targetPart")) == 4 && ! c.on ("reverbReady");
            pickT ("SNARE - REVERB-READY"); const bool sn = c.raw ("targetInst") == (float) dyneq::kTgtSnare && c.on ("reverbReady");
            pickT ("VOCAL - S"); const bool v = c.raw ("targetInst") == (float) dyneq::kTgtVocal && dyneq::partIndex (dyneq::kTgtVocal, c.raw ("targetPart")) == 3 && ! c.on ("reverbReady");
            pickT ("BASS - TOP"); const bool bt = c.raw ("targetInst") == (float) dyneq::kTgtBass && dyneq::partIndex (dyneq::kTgtBass, c.raw ("targetPart")) == 3;
            pickT ("HATS - CHICK"); const bool hch = c.raw ("targetInst") == (float) dyneq::kTgtHats && dyneq::partIndex (dyneq::kTgtHats, c.raw ("targetPart")) == 0;
            pickT ("OFF"); const bool o = c.raw ("targetInst") == 0.0f && ! c.layerOn (panel::kTarget);
            c.toggleLayer (panel::kTarget); const bool back = c.layerOn (panel::kTarget);
            check (kk && sn && v && bt && hch && o && back, "TARGET list maps to targetInst + targetPart + reverbReady (KICK CLICK, SNARE REVERB-READY, VOCAL S, BASS TOP, HATS CHICK, OFF); the TARGET lamp turns the last instrument back on");
        }

        // 11f. the curves are the engine's filters: measured through the processor against Engine::freeBandMagDb, all six band types
        {
            struct T { const char* name; int type; float f0, g, q; int slope; };
            const T tests[] = { { "BELL +6 dB Q 1 at 1 kHz", dyneq::kBell, 1000, 6, 1.0f, 0 }, { "LOW SHELF -6 dB at 300 Hz", dyneq::kLowShelf, 300, -6, 0.9f, 0 }, { "HIGH SHELF +6 dB at 3 kHz", dyneq::kHighShelf, 3000, 6, 0.9f, 0 },
                                { "NOTCH Q 4 at 1.5 kHz", dyneq::kNotch, 1500, 0, 4.0f, 0 }, { "LOW CUT 24 at 800 Hz", dyneq::kLowCut, 800, 0, 1.0f, 1 }, { "HIGH CUT 48 at 4 kHz", dyneq::kHighCut, 4000, 0, 1.0f, 2 }, { "LOW CUT 12 at 200 Hz", dyneq::kLowCut, 200, 0, 1.0f, 0 },
                                { "LOW CUT 72 at 600 Hz Q 4", dyneq::kLowCut, 600, 0, 4.0f, 4 }, { "HIGH CUT 36 at 3 kHz", dyneq::kHighCut, 3000, 0, 1.0f, 3 }, { "HIGH CUT 72 at 5 kHz Q 3", dyneq::kHighCut, 5000, 0, 3.0f, 4 }, { "LOW CUT 12 at 700 Hz Q 6", dyneq::kLowCut, 700, 0, 6.0f, 0 } };
            double worst = 0.0; int points = 0;
            for (auto& t : tests)
                for (double f : { 120.0, 250.0, 500.0, 1000.0, 1700.0, 3000.0, 6000.0 })
                {
                    const Audio in = sineAudio (f, -20.0, 1.2);
                    const Settings st = { { "b1_on", 1 }, { "b1_type", (float) t.type }, { "b1_freq", t.f0 }, { "b1_gain", t.g }, { "b1_q", t.q }, { "b1_slope", (float) t.slope } };
                    const auto out = runPlugin (in, nullptr, false, 512, st);
                    const double measured = rmsDb (out, 36000, 57600) - rmsDb (in, 36000, 57600);
                    const double model = dyneq::Engine::freeBandMagDb (t.type, t.f0, t.g, t.q, dyneq::freeSlopeDbFromIndex (t.slope), f, 48000.0);
                    if (model > -60.0) { worst = std::max (worst, std::abs (measured - model)); ++points; }
                }
            check (worst < 0.1, "panel curves: Engine::freeBandMagDb vs the real plug-in on sine tones, 11 band shapes x 7 frequencies: worst difference " + juce::String (worst, 3) + " dB over " + juce::String (points) + " points (limit 0.1 dB)");
        }

        // 11g. the layer curves of the panel equal the engine's own bandConflictDb / bandSelfDb / bandTargetDb (read through the processor)
        {
            auto compare = [&] (const char* what, const Settings& st, bool useKey, const Audio& in, const Audio* key, int which)
            {
                Rig r (2, useKey, 48000.0, 512, st);
                r.render (in, key, 512);
                DyneqEditor ed (r.proc); ed.tickForSnapshot();
                auto& cv = ed.curves(); double worst = 0.0, maxAbs = 0.0;
                for (int i = 0; i < 6; ++i)
                {
                    const auto& l = r.proc.getBandLayers (i); const double hz = (double) cv.band[i].freq;
                    const double model = which == 0 ? cv.collideDb (hz) : which == 1 ? cv.selfDb (hz) : cv.targetDb (hz);
                    const double engine = which == 0 ? l.conflict.load() : which == 1 ? l.self.load() : l.target.load();
                    worst = std::max (worst, std::abs (model - engine)); maxAbs = std::max (maxAbs, std::abs (engine));
                }
                check (worst < 0.1 && maxAbs > 0.3, juce::String ("panel layer curve ") + what + " == the engine's accessor at the six band frequencies: worst difference " + juce::String (worst, 4) + " dB, largest engine value " + juce::String (maxAbs, 2) + " dB (limit 0.1, and the layer must be doing something)");
            };
            Settings bands = { { "b1_on", 1 }, { "b1_freq", 150 }, { "b2_on", 1 }, { "b2_freq", 300 }, { "b3_on", 1 }, { "b3_freq", 600 }, { "b4_on", 1 }, { "b4_freq", 1200 }, { "b5_on", 1 }, { "b5_freq", 2400 }, { "b6_on", 1 }, { "b6_freq", 4800 } };
            Settings col = bands; col.push_back ({ "depth", 8 }); col.push_back ({ "margin", 0 });
            compare ("COLLIDE", col, true, beat, &vocal, 0);
            Settings sf = bands; sf.push_back ({ "mode", 1 }); sf.push_back ({ "depth", 12 }); sf.push_back ({ "margin", 0 });
            const Audio rich = runPlugin (beat, nullptr, false, 512, { }); (void) rich;
            Audio res (2, std::vector<float> (beat[0].size())); { auto pn = sineAudio (2400.0, -24.0, 6.0); for (int c = 0; c < 2; ++c) for (size_t i = 0; i < res[0].size() && i < pn[0].size(); ++i) res[(size_t) c][i] = beat[(size_t) c][i] + pn[(size_t) c][i]; }
            compare ("SELF", sf, false, res, nullptr, 1);
            Settings tg = bands; tg.push_back ({ "targetInst", (float) dyneq::kTgtKick }); tg.push_back ({ "targetPart", dyneq::sliderForPart (dyneq::kTgtKick, 0) }); tg.push_back ({ "targetAmount", 12 }); tg.push_back ({ "b1_freq", 60 }); tg.push_back ({ "b2_freq", 90 });
            const Audio longBeat = readWav (dir.getChildFile ("beat.wav"));
            compare ("TARGET", tg, false, slice (longBeat, 0, 48000 * 12), nullptr, 2);
        }

        // 11h. the node read-out and the teal ring use the engine's numbers
        // 2026-10-26: the boost sits at 1272 Hz (was 292 Hz). The signal law (SPEC 10) cuts only where the key has energy of its own; the demo vocal sits 34 dB under
        // the beat at 292 Hz (0.9.1 cut there through its presence knee, 1.7 dB on average), at 1272 Hz vocal and beat collide (dyneqraw: -5.7 dB at the six read-outs)
        {
            Settings st = { { "depth", 12 }, { "margin", 0 }, { "b1_on", 1 }, { "b1_freq", 1272 }, { "b1_gain", 4 }, { "b1_q", 1.2f } };
            Rig r (2, true, 48000.0, 512, st);
            const Audio b = slice (beat, 0, 48000 * 6), v = slice (vocal, 0, 48000 * 6);
            double carvedSeen = 0, readoutOk = 0, blocks = 0;
            DyneqEditor ed (r.proc);
            for (int pos = 0; pos < 48000 * 6; pos += 24000)
            {
                r.render (slice (b, pos, 24000), nullptr, 512); (void) v;
            }
            Audio v2 = slice (vocal, 0, 48000 * 2); Audio b2 = slice (beat, 0, 48000 * 2);
            for (int k = 0; k < 6; ++k)
            {
                r.render (b2, &v2, 512); ed.tickForSnapshot(); ++blocks;
                const auto& l = r.proc.getBandLayers (0);
                const juce::String t = ed.testReadout (0);
                const bool carved = ed.testCarved (0);
                if (carved) ++carvedSeen;
                if (t.contains ("net " + panel::sgn (l.net.load())) && t.contains (panel::sgn (4.0f) + " band") && (std::abs (l.conflict.load()) < 0.05f || t.contains (panel::sgn (l.conflict.load()) + " COLLIDE (KEY)"))) ++readoutOk;
                if (k == 5) std::printf ("   read-out: %s   (teal ring: %s)\n", t.toRawUTF8(), carved ? "yes" : "no");
            }
            check (readoutOk == blocks && carvedSeen > 0, "node read-out is '+x band / -y COLLIDE (KEY) / net z' built from the engine's bandConflictDb / bandNetDb, and the dashed teal ring appears while a boost is carved", juce::String ((int) readoutOk) + " of " + juce::String ((int) blocks) + " read-outs match, ring drawn in " + juce::String ((int) carvedSeen));
        }

        // 11i. the processor features: OUTPUT, ASSIST OFF, level-matched BYPASS, meters, the state
        {
            const Audio tone = sineAudio (1000.0, -20.0, 3.0);
            const auto o6 = runPlugin (tone, nullptr, false, 512, { { "output", 6.0f } });
            const double g6 = rmsDb (o6, 100000, 140000) - rmsDb (tone, 100000, 140000);
            const auto o0 = runPlugin (tone, nullptr, false, 512, {});
            check (std::abs (g6 - 6.0) < 0.02 && maxDiff (o0, tone) == 0.0, "OUTPUT: +6 dB is +6.00 dB on a 1 kHz tone (measured " + juce::String (g6, 3) + " dB), 0 dB leaves the signal bit for bit");
            const Settings busy = { { "depth", 12 }, { "margin", 0 } };
            Settings off = busy; off.push_back ({ "assistOff", 1 });
            auto cut = runPlugin (beat, &vocal, true, 512, busy); auto none = runPlugin (beat, &vocal, true, 512, off);
            Settings offSelf = off; offSelf.push_back ({ "mode", 1 });
            auto none2 = runPlugin (beat, &vocal, true, 512, offSelf);
            check (maxDiff (cut, beat) > 0.01 && maxDiff (none, beat) == 0.0 && maxDiff (none2, beat) == 0.0, "ASSIST OFF (the layer lamp dark): COLLIDE and SELF cut nothing and the output is the input bit for bit; with the lamp lit the same settings cut", juce::String::formatted ("lit change %.3f, dark %.1e / %.1e", maxDiff (cut, beat), maxDiff (none, beat), maxDiff (none2, beat)));
            // level-matched bypass: the dry signal at the loudness of the processed one
            Settings proc2 = { { "depth", 12 }, { "margin", 0 }, { "b1_on", 1 }, { "b1_gain", -9 }, { "b1_freq", 300 }, { "b2_on", 1 }, { "b2_type", 4 }, { "b2_freq", 200 }, { "b2_slope", 2 } };
            const Audio bL = slice (readWav (dir.getChildFile ("beat.wav")), 0, 48000 * 14), vL = slice (readWav (dir.getChildFile ("vocal.wav")), 0, 48000 * 14);
            const auto live = runPlugin (bL, &vL, true, 512, proc2);
            Settings byp = proc2; byp.push_back ({ "bypass", 1 }); byp.push_back ({ "levelMatch", 1 });
            const auto matched = runPlugin (bL, &vL, true, 512, byp);
            Settings bypTrue = proc2; bypTrue.push_back ({ "bypass", 1 }); bypTrue.push_back ({ "levelMatch", 0 });
            const auto trueByp = runPlugin (bL, &vL, true, 512, bypTrue);
            const int a = 48000 * 8, b = 48000 * 14;
            const double lLive = rmsDb (live, a, b), lMatch = rmsDb (matched, a, b), lDry = rmsDb (bL, a, b);
            check (std::abs (lLive - lMatch) < 0.5 && std::abs (lLive - lDry) > 1.0 && maxDiff (trueByp, bL) == 0.0, "BYPASS level matched: the bypassed signal is within 0.5 dB of the processed loudness (processed " + juce::String (lLive, 2) + " dB, matched bypass " + juce::String (lMatch, 2) + " dB, plain dry " + juce::String (lDry, 2) + " dB); level match off = the true bypass, bit for bit");
            // the meters follow the real signal, the trim included
            Rig mr (2, false, 48000.0, 512, { { "output", 6.0f } });
            const Audio tn = sineAudio (1000.0, -20.0, 2.0); mr.render (tn, nullptr, 512);
            const double expIn = -20.0 - 3.0103;   // a sine of amplitude 0.1 has a mean square of A^2 / 2
            check (std::abs (mr.proc.getInDb() - expIn) < 0.2 && std::abs (mr.proc.getOutDb() - (expIn + 6.0)) < 0.2, "IN / OUT meters: IN reads the input and OUT follows the real output including the +6 dB trim", juce::String::formatted ("IN %.2f (want %.2f), OUT %.2f (want %.2f)", (double) mr.proc.getInDb(), expIn, (double) mr.proc.getOutDb(), expIn + 6.0));
            // state: the panel's size and mode are saved
            DyneqAudioProcessor a1, a2; a1.editorScalePct = 85; a1.uiMode = 7;
            juce::MemoryBlock mb; a1.getStateInformation (mb); a2.setStateInformation (mb.getData(), (int) mb.getSize());
            check (a2.editorScalePct.load() == 85 && a2.uiMode.load() == 7, "state keeps the panel's size (85 %) and mode");
        }

        // 11j. the spectrum on the LAB screen is an FFT of the real input; DELTA lights; the scale factors
        {
            Rig r (2, false, 48000.0, 512, {});
            setParam (r.proc.apvts, "analyzer", 3.0f);   // this check reads the PRE trace (the default is POST only)
            r.render (sineAudio (1000.0, -20.0, 1.0), nullptr, 512);
            DyneqEditor ed (r.proc); settle (ed); ed.tickForSnapshot(); ed.tickForSnapshot(); ed.tickForSnapshot();
            const auto& sp = ed.spectrum(); int top = 0; for (int i = 0; i < panel::kCurveN; ++i) if (sp.db[i] > sp.db[top]) top = i;
            const double fTop = ed.curves().f[top]; const int i4k = (int) std::round (std::log (4000.0 / 20.0) / std::log (1000.0) * (panel::kCurveN - 1));
            check (std::abs (std::log2 (fTop / 1000.0)) < 0.2 && sp.db[top] - sp.db[i4k] > 20.0f, "LAB spectrum: a 1 kHz tone shows its peak at " + juce::String (fTop, 0) + " Hz and stands " + juce::String (sp.db[top] - sp.db[i4k], 1) + " dB above 4 kHz");
            juce::Component* dl = nullptr; ed.forEachControl ([&] (juce::Component& c, PanelControl& pc) { if (pc.boundParams().size() == 1 && pc.boundParams()[0] == "delta") dl = &c; });
            const juce::Point<float> p (30.0f, 30.0f);
            if (dl != nullptr) dl->mouseDown (mk (*dl, p, p));
            const bool lit = ed.ctl().delta();
            if (dl != nullptr) dl->mouseUp (mk (*dl, p, p));
            check (dl != nullptr && lit && ! ed.ctl().delta(), "DELTA is a hold: pressed = delta on (the lamp follows the parameter), released = off");
            double s75 = 0, s85 = 0, s100 = 0;
            ed.setScalePercent (75); s75 = ed.getTransform().mat00; ed.setScalePercent (85); s85 = ed.getTransform().mat00; ed.setScalePercent (100); s100 = ed.getTransform().mat00;
            check (std::abs (s75 - 0.75) < 1e-3 && std::abs (s85 - 0.85) < 1e-3 && std::abs (s100 - 1.0) < 1e-3, "scaling offers 75 / 85 / 100 %: 1200 x 540 becomes 900 x 405, 1020 x 459, 1200 x 540 (85 %: " + juce::String (juce::roundToInt (1200 * s85)) + " x " + juce::String (juce::roundToInt (540 * s85)) + ")");
        }
    }

    {   // 11k. a dynamic band's RANGE still moves the output (a regression probe for the processor loop)
        Audio noise (2, std::vector<float> (48000 * 2)); juce::Random rnd (7); for (auto& c : noise) for (auto& v : c) v = 0.2f * (rnd.nextFloat() - 0.5f);
        auto run = [&] (float range) { return runPlugin (noise, nullptr, false, 512, { { "b1_on", 1 }, { "b1_freq", 1000.0f }, { "b1_dyn", 1 }, { "b1_thr", -60.0f }, { "b1_range", range }, { "b1_gain", 0.0f }, { "b1_q", 0.5f } }); };
        const double lo = rmsDb (run (-12.0f), 48000, 96000), hi = rmsDb (run (12.0f), 48000, 96000);
        {
            auto rs = [&] (bool sc, int block) { Rig r (2, sc, 48000.0, block, { { "b1_on", 1 }, { "b1_type", 0 }, { "b1_freq", 1000 }, { "b1_gain", 9 }, { "b1_q", 1 }, { "depth", 0 }, { "mix", 100 } }); return rmsDb (r.render (noise, &vocal, block), 48000, 96000); };
            const Audio v2 = slice (vocal, 0, 96000); (void) v2;
            std::printf ("   probe: noise rms %.2f dB; bell +9 dB through the plug-in: no key bus %.2f / key bus %.2f (block 512), %.2f (block 64)\n", rmsDb (noise, 48000, 96000), rs (false, 512), rs (true, 512), rs (true, 64));
        }
        check (hi - lo > 3.0, "a dynamic band: RANGE -12 vs +12 dB moves the output rms by " + juce::String (hi - lo, 2) + " dB (must be > 3)");
    }

    {   // 11l. TASK_P1n through the processor: the BYPASS fade (own parameter without level match, and the host's call), legacy state, the new slopes
        auto hpPeakDbc = [] (const Audio& a, int from, int to, double ref)   // 8th-order 4 kHz high pass (RBJ Butterworth), peak of the output over [from, to), in dB re ref
        {
            const double Q[4] = { 0.50979557910415917, 0.60134488693504529, 0.89997622313641570, 2.5629154477415055 };
            double peak = 0.0; const double w0 = 2.0 * M_PI * 4000.0 / 48000.0, cw = std::cos (w0);
            double s1[4] {}, s2[4] {};
            for (int n = 0; n < to; ++n)
            {
                double x = a[0][(size_t) n];
                for (int k = 0; k < 4; ++k)
                {
                    const double al = std::sin (w0) / (2.0 * Q[k]), a0 = 1.0 + al, b0 = 0.5 * (1.0 + cw) / a0, b1 = -(1.0 + cw) / a0, a1 = -2.0 * cw / a0, a2 = (1.0 - al) / a0;
                    const double y = b0 * x + s1[k]; s1[k] = b1 * x - a1 * y + s2[k]; s2[k] = b0 * x - a2 * y; x = y;
                }
                if (n >= from) peak = std::max (peak, std::abs (x));
            }
            return 20.0 * std::log10 (peak / ref + 1e-30);
        };
        auto maxStep = [] (const Audio& a, int from, int to) { double m = 0; for (int n = from + 1; n < to; ++n) m = std::max (m, (double) std::abs (a[0][(size_t) n] - a[0][(size_t) n - 1])); return m; };
        Audio tone (2, std::vector<float> (48000 * 3));
        for (int n = 0; n < 48000 * 3; ++n) { const float v = (float) (0.25 * std::cos (2.0 * M_PI * 1000.0 * n / 48000.0)); tone[0][(size_t) n] = tone[1][(size_t) n] = v; }
        const double ownStep = 0.25 * 2.0 * M_PI * 1000.0 / 48000.0;   // the sine's own largest step
        struct Row { const char* name; Settings st; };
        const Row rows[] = { { "bell +12 dB at 1 kHz", { { "b1_on", 1 }, { "b1_gain", 12 } } }, { "high cut 500 Hz 12 dB/oct", { { "b1_on", 1 }, { "b1_type", 5 }, { "b1_freq", 500 }, { "b1_slope", 0 } } },
                             { "high shelf 3 kHz +6", { { "b1_on", 1 }, { "b1_type", 2 }, { "b1_freq", 3000 }, { "b1_gain", 6 }, { "b1_q", 0.71f } } }, { "low cut 2 kHz 72 dB/oct", { { "b1_on", 1 }, { "b1_type", 4 }, { "b1_freq", 2000 }, { "b1_slope", 4 } } } };
        double worstStep = 0.0, worstHf = -300.0; bool exactDry = true, exactBack = true; int nTrans = 0;
        for (const auto& row : rows)
            for (int host = 0; host < 2; ++host)
            {
                Settings st = row.st; st.push_back ({ "levelMatch", 0 });
                // never bypassed: the reference for the tail
                const auto ref = runPlugin (tone, nullptr, false, 64, st);
                Rig r (2, false, 48000.0, 64, st);
                Audio out (2, std::vector<float> ((size_t) 48000 * 3));
                auto seg = [&] (int a, int b, bool bypassed, bool viaHost)
                {
                    const Audio in = slice (tone, a, b - a);
                    if (bypassed && ! viaHost) setParam (r.proc.apvts, "bypass", 1.0f); else setParam (r.proc.apvts, "bypass", 0.0f);
                    const auto o = r.render (in, nullptr, 64, bypassed && viaHost);
                    for (int c = 0; c < 2; ++c) std::copy (o[(size_t) c].begin(), o[(size_t) c].end(), out[(size_t) c].begin() + a);
                };
                seg (0, 48000, false, false); seg (48000, 96000, true, host == 1); seg (96000, 144000, false, false);
                // BYPASS on at 1.0 s (a waveform peak), off at 2.0 s: the largest step 5 ms around each switch, and the HF residue over 100 ms
                for (int sw : { 48000, 96000 })
                {
                    const double wetStep = maxStep (ref, sw - 240, sw + 240), dryStep = ownStep, bigger = std::max (wetStep, dryStep);
                    worstStep = std::max (worstStep, maxStep (out, sw - 240, sw + 1200) / bigger);
                    worstHf = std::max (worstHf, hpPeakDbc (out, sw - 2400, sw + 4800, 0.25)); ++nTrans;
                }
                exactDry = exactDry && maxDiff (slice (out, 48000 + 1536, 46000 - 1536), slice (tone, 48000 + 1536, 46000 - 1536)) == 0.0;
                exactBack = exactBack && maxDiff (slice (out, 96000 + 1536, 48000 - 1536), slice (ref, 96000 + 1536, 48000 - 1536)) == 0.0;
            }
        check (worstStep <= 1.5 && worstHf <= -60.0 && exactDry && exactBack,
               "BYPASS fade (own parameter without level match, and the host's call; 4 shapes x 2 paths x on/off = " + juce::String (nTrans) + " switches at a waveform peak, block 64): largest step " + juce::String (worstStep, 2) + " x the louder signal's own step (limit 1.5), HF above 4 kHz " + juce::String (worstHf, 1) + " dBc (limit -60); bypassed output == the input bit for bit once the fade is over; released output == a never-bypassed instance bit for bit",
               juce::String::formatted ("exactDry=%d exactBack=%d", (int) exactDry, (int) exactBack));

        // legacy state: the saved SLOPE values 0 / 1 / 2 mean 12 / 24 / 48 dB per octave, as before; the new 3 / 4 mean 36 / 72
        {
            DyneqAudioProcessor a, b;
            const float legacy[3] = { 0, 1, 2 };
            for (int i = 0; i < 3; ++i) { const juce::String n = "b" + juce::String (i + 1) + "_"; setParam (a.apvts, (n + "on").toRawUTF8(), 1); setParam (a.apvts, (n + "type").toRawUTF8(), 4); setParam (a.apvts, (n + "slope").toRawUTF8(), legacy[i]); }
            setParam (a.apvts, "b4_on", 1); setParam (a.apvts, "b4_type", 5); setParam (a.apvts, "b4_slope", 3); setParam (a.apvts, "b5_on", 1); setParam (a.apvts, "b5_type", 5); setParam (a.apvts, "b5_slope", 4);
            juce::MemoryBlock mb; a.getStateInformation (mb); b.setStateInformation (mb.getData(), (int) mb.getSize());
            const auto pb = paramsOf (b.apvts);
            // a state blob exactly as an older build wrote it: the same XML, slope values 0 / 1 / 2 only
            juce::String xmlText = "<DYNEQ><PARAM id=\"b1_on\" value=\"1.0\"/><PARAM id=\"b1_type\" value=\"4.0\"/><PARAM id=\"b1_slope\" value=\"2.0\"/><PARAM id=\"b2_on\" value=\"1.0\"/><PARAM id=\"b2_type\" value=\"5.0\"/><PARAM id=\"b2_slope\" value=\"1.0\"/></DYNEQ>";
            juce::MemoryBlock old; if (auto xml = juce::parseXML (xmlText)) juce::AudioProcessor::copyXmlToBinary (*xml, old);
            DyneqAudioProcessor c; c.setStateInformation (old.getData(), (int) old.getSize());
            const auto pc = paramsOf (c.apvts);
            check (pb.band[0].slopeDb == 12 && pb.band[1].slopeDb == 24 && pb.band[2].slopeDb == 48 && pb.band[3].slopeDb == 36 && pb.band[4].slopeDb == 72 && pc.band[0].slopeDb == 48 && pc.band[1].slopeDb == 24 && pc.band[0].type == dyneq::kLowCut && pc.band[1].type == dyneq::kHighCut,
                   "legacy state: SLOPE 0 / 1 / 2 still load as 12 / 24 / 48 dB/oct (a state written by an older build included), the appended 3 / 4 as 36 / 72", juce::String::formatted ("%d %d %d %d %d | old blob %d %d", pb.band[0].slopeDb, pb.band[1].slopeDb, pb.band[2].slopeDb, pb.band[3].slopeDb, pb.band[4].slopeDb, pc.band[0].slopeDb, pc.band[1].slopeDb));
            check (a.apvts.getParameter ("b1_slope")->getNumSteps() == 5 && dyneq::ptext::choiceNames ("b1_slope").size() == 5 && dyneq::ptext::choiceNames ("b1_slope")[3] == "36 dB/oct" && dyneq::ptext::choiceNames ("b1_slope")[4] == "72 dB/oct", "SLOPE has 5 choices: 12, 24, 48 (the old ones, same indices), then 36 and 72");
        }
        // the new cuts through the processor: LC 72 at 1 kHz, 48 kHz: slope between 500 and 250 Hz, and Q 4 puts a peak of at least +6 dB at the corner
        {
            auto gainAt = [&] (const Settings& st, double f) { const Audio in = sineAudio (f, -20.0, 1.2); const auto out = runPlugin (in, nullptr, false, 512, st); return rmsDb (out, 36000, 57600) - rmsDb (in, 36000, 57600); };
            const Settings lc72 = { { "b1_on", 1 }, { "b1_type", 4 }, { "b1_freq", 1000 }, { "b1_slope", 4 } };
            const double g500 = gainAt (lc72, 500.0), g250 = gainAt (lc72, 250.0), g1k = gainAt (lc72, 1000.0);
            Settings res = lc72; res.push_back ({ "b1_q", 4.0f });
            const double peak = std::max ({ gainAt (res, 900.0), gainAt (res, 1000.0), gainAt (res, 1100.0) });
            check (std::abs ((g250 - g500) + 72.0) < 1.5 && std::abs (g1k + 3.01) < 0.1 && peak >= 6.0, "LOW CUT 72 at 1 kHz through the plug-in: second octave " + juce::String (g250 - g500, 2) + " dB (nominal -72 +-1.5), corner " + juce::String (g1k, 2) + " dB (-3.01), Q 4 peak " + juce::String (peak, 2) + " dB (>= +6)");
        }
        // SLOPE steepness order: a low cut at 2 kHz, a 1 kHz tone one octave below its corner, slope indices in steepness order 0 (12), 1 (24), 3 (36), 2 (48), 4 (72): the tone falls by 12.3 / 24.1 / 36.1 / 48.2 / 72.2 dB
        {
            const Audio tn = sineAudio (1000.0, -20.0, 1.2); double prev = 0.0; bool falling = true; juce::String vals;
            for (int idx : { 0, 1, 3, 2, 4 })
            {
                const auto o = runPlugin (tn, nullptr, false, 512, { { "b1_on", 1 }, { "b1_type", 4 }, { "b1_freq", 2000 }, { "b1_slope", (float) idx } });
                const double att = rmsDb (o, 36000, 57600) - rmsDb (tn, 36000, 57600);
                vals += juce::String (att, 1) + " "; falling = falling && att < prev - 6.0; prev = att;
            }
            check (falling, "SLOPE order by steepness (indices 0, 1, 3, 2, 4 = 12, 24, 36, 48, 72 dB/oct): a 1 kHz tone one octave below a 2 kHz low cut falls by more than 6 dB more at every step", vals + "dB");
        }
        // FREQ ceiling: the panel shows the working frequency, not the request
        {
            DyneqAudioProcessor p; p.setRateAndBufferSizeDetails (44100.0, 512); p.prepareToPlay (44100.0, 512); DyneqEditor edf (p); auto& c = edf.ctl();
            setParam (p.apvts, "b1_on", 1.0f); setParam (p.apvts, "b1_freq", 20000.0f); c.selectLab (0); edf.tickForSnapshot();
            const float shown = edf.curves().band[0].freq; const auto line = edf.testReadout1 (0);
            check (std::abs (c.workingHz (20000.0f) - 19845.0f) < 0.5f && std::abs (shown - 19845.0f) < 0.5f && line.contains (dyneq::ptext::hz (19845.0f)) && std::abs (dyneq::Engine::freeBandMaxFreqHz (44100.0) - 19845.0) < 1e-6 && std::abs (dyneq::Engine::freeBandMaxFreqHz (96000.0) - 20000.0) < 1e-6,
                   "FREQ ceiling: at 44.1 kHz a 20 kHz request shows (node, read-out '" + line + "') as the working " + juce::String (shown, 0) + " Hz (0.45 x sr); at 96 kHz it stays 20 kHz");
        }
    }

    runBakeTesterChecks (beatFull, vocalFull);   // 9b. BAKE as the first tester ran it (25 Oct)

    runStressChecks (beat, vocal);   // 13. stress

    runEditorChecks (beat, vocal, dir);   // 12. P4c: the analyzer, the defaults, the nodes, the panels

    std::printf ("\nplugin_check: %d passed, %d failed\n", gPass, gFail);
    return gFail == 0 ? 0 : 1;
}
