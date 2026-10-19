// plugin_check: the real SedaAudioProcessor against the raw engine, offline, on generated programme (no files needed).
//   plugin_check                       every check below, then the quick control proof (SvlControlProof.h); exit code 0 only if all passed
//   plugin_check --controlproof [...]  the full control proof harness on its own (options: SvlControlProof.h; --quick, --param ID, --no-docs ...)
//
// What is covered (TASK_P3 item 4): parameter round-trip (plain value, host text), state save/load incl. the "touched" mask, bypass and MIX 0
// bit-exact, block sizes 1 ... 4096 bit-identical, 44.1 / 48 / 96 kHz (no NaN, delay time correct in ms), tail, SvlUndo, ControlProof.
#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"
#include "../Source/ParamText.h"
#include "../Source/PluginEditor.h"
#include "SvlUndoTests.h"
#include "SvlControlProof.h"
#include <cmath>
#include <cstdio>
#include <functional>
#include <set>
#if JUCE_MAC
 #include <mach/mach.h>
#endif

using Audio = std::vector<std::vector<float>>;
using Settings = std::vector<std::pair<const char*, float>>;   // parameter id, PLAIN value

static int gPass = 0, gFail = 0;
static void check (bool ok, const juce::String& what, const juce::String& detail = {})
{
    std::printf ("[%s] %s%s%s\n", ok ? "PASS" : "FAIL", what.toRawUTF8(), detail.isEmpty() ? "" : "  ", detail.toRawUTF8());
    (ok ? gPass : gFail)++;
}

// ---------------------------------------------------------------------------------------------------- generated programme
struct Rng
{
    uint64_t s;
    explicit Rng (uint64_t seed) : s (seed * 0x9E3779B97F4A7C15ull + 1) {}
    double uni() { s ^= s >> 12; s ^= s << 25; s ^= s >> 27; return (double) ((s * 0x2545F4914F6CDD1Dull) >> 11) / 9007199254740992.0; }
    double bi() { return 2.0 * uni() - 1.0; }
};

static Audio makeAudio (int ch, int n) { return Audio ((size_t) ch, std::vector<float> ((size_t) n, 0.0f)); }

/** White noise from `from` to `to` seconds (peak `amp`), silence elsewhere. Left and right differ. */
static Audio noiseBurst (double sr, double seconds, double from, double to, double amp, uint64_t seed = 5)
{
    auto a = makeAudio (2, (int) (seconds * sr));
    Rng r (seed);
    for (int c = 0; c < 2; ++c)
        for (int i = (int) (from * sr); i < (int) (to * sr) && i < (int) a[0].size(); ++i) a[(size_t) c][(size_t) i] = (float) (amp * r.bi());
    return a;
}

/** A synth "vocal": phrases of three tone bursts (four harmonics), 1.3 s of silence between phrases. Left and right differ in level and phase. */
static Audio phrases (double sr, double seconds, double level = 0.12, int words = 3)   // words: 3 = a 1.0 s phrase; 5 = 1.7 s (P3e: a throw needs >= 1.2 s of material)
{
    auto a = makeAudio (2, (int) (seconds * sr));
    double t0 = 0.2; int note = 0;
    while (t0 < seconds - 0.4)
    {
        for (int w = 0; w < words; ++w)
        {
            const double f0 = 220.0 * std::pow (2.0, (double) ((note++ * 5) % 12) / 12.0), len = 0.30;
            for (int i = 0; i < (int) (len * sr); ++i)
            {
                const int k = (int) (t0 * sr) + i; if (k >= (int) a[0].size()) break;
                const double t = (double) i / sr, env = std::min (1.0, t / 0.02) * std::min (1.0, (len - t) / 0.04);
                double l = 0.0, r = 0.0;
                for (int h = 1; h <= 4; ++h) { l += std::sin (2.0 * M_PI * f0 * h * t) / h; r += std::sin (2.0 * M_PI * f0 * h * t + 0.7 * h) / h; }
                a[0][(size_t) k] += (float) (level * env * l * 0.5);
                a[1][(size_t) k] += (float) (level * env * r * 0.4);
            }
            t0 += len + 0.05;
        }
        t0 += 1.3;
    }
    return a;
}

/** Decaying tone pluck trains: `rate` plucks per second. */
static Audio plucks (double sr, double seconds, double rate, double level = 0.2)
{
    auto a = makeAudio (2, (int) (seconds * sr));
    for (double t0 = 0.1; t0 < seconds - 0.2; t0 += 1.0 / rate)
        for (int i = 0; i < (int) (0.12 * sr); ++i)
        {
            const int k = (int) (t0 * sr) + i; if (k >= (int) a[0].size()) break;
            const double t = (double) i / sr, env = std::exp (-t * 30.0), v = level * env * std::sin (2.0 * M_PI * 440.0 * t);
            a[0][(size_t) k] += (float) v; a[1][(size_t) k] += (float) (0.8 * v);
        }
    return a;
}

static Audio mixOf (const Audio& x, const Audio& y)
{
    Audio o = x;
    for (size_t c = 0; c < o.size(); ++c) for (size_t i = 0; i < o[c].size() && i < y[c].size(); ++i) o[c][i] += y[c][i];
    return o;
}

static Audio slice (const Audio& a, size_t from, size_t len)
{
    Audio o (a.size());
    for (size_t c = 0; c < a.size(); ++c) o[c].assign (a[c].begin() + (long) from, a[c].begin() + (long) std::min (a[c].size(), from + len));
    return o;
}

static bool allFinite (const Audio& a) { for (auto& c : a) for (float v : c) if (! std::isfinite (v)) return false; return true; }
static double peakOf (const Audio& a, size_t from = 0, size_t to = ~(size_t) 0)
{
    double m = 0; for (auto& c : a) for (size_t i = from; i < std::min (to, c.size()); ++i) m = std::max (m, (double) std::abs (c[i])); return m;
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
static bool bitEqual (const Audio& a, const Audio& b)
{
    if (a.size() != b.size()) return false;
    for (size_t c = 0; c < a.size(); ++c)
        if (a[c].size() != b[c].size() || std::memcmp (a[c].data(), b[c].data(), a[c].size() * sizeof (float)) != 0) return false;
    return true;
}
static double toDb (double x) { return 20.0 * std::log10 (std::max (x, 1e-12)); }

static void setParam (juce::AudioProcessorValueTreeState& s, const char* id, float v)
{
    auto* p = s.getParameter (id);
    jassert (p != nullptr);
    p->setValueNotifyingHost (p->convertTo0to1 (v));
}

struct TestPlayHead : juce::AudioPlayHead
{
    double bpm = 120.0;
    bool hasPpq = false; double ppq = 0.0, barStart = 0.0;   // a host that reports its position (hasPpq): quarter notes at the start of the block, and of the bar
    bool playing = false;   // the transport runs (CHAOS steps on the host's beats only while it plays)
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo i; i.setBpm (bpm);
        if (hasPpq) { i.setPpqPosition (ppq); i.setPpqPositionOfLastBarStart (barStart); i.setTimeSignature (juce::AudioPlayHead::TimeSignature { 4, 4 }); }
        i.setIsPlaying (playing);
        return i;
    }
};

// ---------------------------------------------------------------------------------------------------- the rig: the real processor, driven like a host
struct Rig
{
    SedaAudioProcessor proc;
    TestPlayHead ph;
    int block, nch; double sampleRate;
    juce::AudioBuffer<float> buf;
    juce::MidiBuffer midi;
    bool bypassedCall = false;   // call processBlockBypassed instead (the host's own bypass)

    Rig (double sr, int blockSize, int prepBlock, int channels, const Settings& st, double bpm /* <= 0: no playhead */)
        : block (blockSize), nch (channels), sampleRate (sr)
    {
        const auto set = channels == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();
        auto layout = proc.getBusesLayout();
        layout.inputBuses.set (0, set); layout.outputBuses.set (0, set);
        if (! proc.setBusesLayout (layout)) { std::printf ("setBusesLayout refused\n"); std::exit (2); }
        proc.setRateAndBufferSizeDetails (sr, prepBlock);
        proc.prepareToPlay (sr, prepBlock);
        for (auto& kv : st) setParam (proc.apvts, kv.first, kv.second);
        ph.bpm = bpm;
        if (bpm > 0.0) proc.setPlayHead (&ph);
        buf.setSize (nch, block);
    }

    /** Runs `in` (nch channels) through the processor in blocks of `block`; returns the output. */
    Audio run (const Audio& in)
    {
        const int n = (int) in[0].size();
        Audio out = makeAudio (nch, n);
        for (int pos = 0; pos < n; pos += block)
        {
            const int len = juce::jmin (block, n - pos);
            buf.setSize (nch, len, false, false, true);
            for (int c = 0; c < nch; ++c) std::copy (in[(size_t) c].begin() + pos, in[(size_t) c].begin() + pos + len, buf.getWritePointer (c));
            if (bypassedCall) proc.processBlockBypassed (buf, midi); else proc.processBlock (buf, midi);
            if (ph.hasPpq) { ph.ppq += (double) len * ph.bpm / (60.0 * sampleRate); ph.barStart = std::floor (ph.ppq / 4.0 + 1e-9) * 4.0; }
            for (int c = 0; c < nch; ++c) std::copy (buf.getReadPointer (c), buf.getReadPointer (c) + len, out[(size_t) c].begin() + pos);
        }
        return out;
    }
};

static Audio runPlugin (const Audio& in, double sr, int block, int prepBlock, const Settings& st, double bpm, bool bypassedCall = false)
{
    Rig r (sr, block, prepBlock, (int) in.size(), st, bpm);
    r.bypassedCall = bypassedCall;
    return r.run (in);
}

/** The raw engine, block by block, same Params, no plug-in layer. */
static Audio runRaw (const Audio& in, double sr, int block, const seda::Params& p)
{
    static seda::Engine e;
    e.prepare (sr); e.setParams (p); e.reset();
    Audio out = in;
    const int n = (int) in[0].size();
    for (int pos = 0; pos < n; pos += block)
    {
        const int len = juce::jmin (block, n - pos);
        e.setParams (p);
        e.process (out[0].data() + pos, out[1].data() + pos, out[0].data() + pos, out[1].data() + pos, len);
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------- parameters
struct ParamCase { const char* id; float plain; };
// one non-default value for every parameter (the 21 of the layout); BYPASS is left out of the render comparisons (it is tested on its own)
static const ParamCase kAllParams[] = {
    { "voice", 2 }, { "time", 777.7f }, { "sync", 1 }, { "note", 5 }, { "noteAuto", 1 }, { "feedback", 83.3f }, { "mix", 42.5f }, { "age", 1 }, { "comp", 37.5f },
    { "tone", -45.5f }, { "rate", 3.21f }, { "depth", 61.5f }, { "shape", 1 }, { "stereo", 2 }, { "width", 12.3f }, { "listen", 1 }, { "source", 3 },
    { "duck", 71.5f }, { "throw", 8.5f }, { "wetHp", 330.0f }, { "runaway", 37.5f }, { "chaos", 64.5f } };

static Settings settingsOf (std::initializer_list<ParamCase> l) { Settings s; for (auto& c : l) s.push_back ({ c.id, c.plain }); return s; }

// ---------------------------------------------------------------------------------------------------- the checks
static void testParameters()
{
    SedaAudioProcessor a;
    const std::vector<std::pair<const char*, float>> defaults = {
        { "voice", 0 }, { "time", 300.0f }, { "sync", 0 }, { "note", 11 }, { "noteAuto", 0 }, { "feedback", 30.0f }, { "mix", 50.0f }, { "age", 0 }, { "comp", 100.0f },
        { "tone", 0.0f }, { "rate", 0.5f }, { "depth", 0.0f }, { "shape", 0 }, { "stereo", 0 }, { "width", 0.0f }, { "listen", 0 }, { "source", 0 }, { "duck", 50.0f },
        { "throw", 0.0f }, { "wetHp", 0.0f }, { "bypass", 0 }, { "runaway", 0.0f }, { "chaos", 0.0f } };
    check (a.getParameters().size() == (int) defaults.size(), "the layout holds the 23 parameters (every engine parameter + VOICE + BYPASS; RUNAWAY and CHAOS appended at the end)", "found " + juce::String (a.getParameters().size()));
    {   // the order of the parameters is what saved sessions and hosts know: the two new ones are the last two, every earlier index is unchanged
        juce::StringArray ids; for (auto* q : a.getParameters()) if (auto* w = dynamic_cast<juce::AudioProcessorParameterWithID*> (q)) ids.add (w->paramID);
        juce::StringArray want; for (auto& d : defaults) want.add (d.first);
        check (ids == want, "parameter order: the 21 earlier parameters keep their indices, runaway and chaos are indices 21 and 22", ids.joinIntoString (" "));
    }
    int bad = 0;
    juce::String why;
    const seda::Params d;   // the engine's own defaults
    const float engineDefaults[] = { 0, d.timeMs, (float) d.sync, (float) d.note, (float) d.noteAuto, d.feedback * 100, d.mix * 100, (float) d.age, d.comp * 100, d.tone * 100, d.rateHz, d.depth * 100,
                                     (float) d.lfoShape, (float) d.stereo, d.widthMs, (float) d.listen, (float) d.source, d.duck * 100, d.throwDb, d.wetHpHz, 0, d.runaway * 100, d.chaos * 100 };
    for (size_t i = 0; i < defaults.size(); ++i)
    {
        auto* p = a.apvts.getRawParameterValue (defaults[i].first);
        if (p == nullptr) { ++bad; why << defaults[i].first << " missing; "; continue; }
        if (std::abs (p->load() - defaults[i].second) > 1e-4f || std::abs (p->load() - engineDefaults[i]) > 1e-3f) { ++bad; why << defaults[i].first << "=" << p->load() << " "; }
    }
    check (bad == 0, "defaults are the engine's own defaults (nothing here decides a sound)", why);

    // round trip of the plain value through the host-side (normalised) path
    bad = 0; why = {};
    auto all = settingsOf ({ { "bypass", 1 } });
    for (auto& c : kAllParams) all.push_back ({ c.id, c.plain });
    for (auto& kv : all)
    {
        auto* p = dynamic_cast<juce::RangedAudioParameter*> (a.apvts.getParameter (kv.first));
        p->setValueNotifyingHost (p->convertTo0to1 (kv.second));
        const float got = a.apvts.getRawParameterValue (kv.first)->load();
        const float tol = p->isDiscrete() ? 1e-6f : p->getNormalisableRange().interval * 0.5f + 1e-4f;
        if (std::abs (got - kv.second) > tol) { ++bad; why << kv.first << " set " << kv.second << " read " << got << "; "; }
    }
    check (bad == 0, "parameter round trip: all 23 read back what was written (plain value)", why);

    // every parameter at its extremes: finite readback, host text reads back to the same normalised value
    bad = 0; why = {};
    SedaAudioProcessor b;
    for (auto* ap : b.getParameters())
    {
        auto* rp = dynamic_cast<juce::RangedAudioParameter*> (ap);
        auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (ap);
        if (rp == nullptr || withId == nullptr) continue;
        for (float n : { 0.0f, rp->getDefaultValue(), 0.37f, 1.0f })
        {
            const auto t = rp->getText (n, 64);
            const float back = rp->getValueForText (t);
            const float tol = rp->isDiscrete() ? 1.0e-6f : 0.006f;
            if (t.isEmpty() || t.containsIgnoreCase ("nan") || std::abs (back - (rp->isDiscrete() ? rp->convertTo0to1 (rp->convertFrom0to1 (n)) : n)) > tol) { ++bad; why << withId->paramID << " @" << n << " \"" << t << "\"->" << back << "; "; }
        }
        if (! (ap->getLabel().isEmpty())) { ++bad; why << withId->paramID << " has a label; "; }
    }
    check (bad == 0, "host text: every parameter's text at 0 / default / 0.37 / 1 reads back to the same value, no separate unit label", why);

    {   // typed forms and the strict parser
        float v = 0.0f;
        const bool ok = seda::ptext::parse ("time", "0.45 s", v) && std::abs (v - 450.0f) < 1e-3f && seda::ptext::parse ("time", "220", v) && v == 220.0f
                        && seda::ptext::parse ("tone", "-25 %", v) && v == -25.0f && seda::ptext::parse ("wetHp", "off", v) && v == 0.0f
                        && seda::ptext::parse ("wetHp", "250 Hz", v) && v == 250.0f && seda::ptext::parse ("throw", "6 dB", v) && v == 6.0f
                        && seda::ptext::parse ("rate", "1,5", v) && v == 1.5f;
        const bool bad2 = seda::ptext::parse ("time", "banana", v) || seda::ptext::parse ("time", "12 apples", v) || seda::ptext::parse ("mix", "50 ms", v)
                          || seda::ptext::parse ("throw", "-", v) || seda::ptext::parse ("feedback", "", v);
        check (ok && ! bad2, "typed text: 0.45 s, 220, -25 %, off, 250 Hz, 6 dB, 1,5 read; words, wrong unit, lone sign, empty are refused");
        check (seda::ptext::format ("time", 300.0f) == "300.0 ms" && seda::ptext::format ("feedback", 110.0f) == "110.0 %" && seda::ptext::format ("tone", 25.0f) == "+25.0 %"
                   && seda::ptext::format ("tone", -0.01f) == "0.0 %" && seda::ptext::format ("wetHp", 0.0f) == "OFF" && seda::ptext::format ("rate", 0.5f) == "0.50 Hz",
               "host text strings: 300.0 ms, 110.0 %, +25.0 %, 0.0 % (never -0.0), OFF, 0.50 Hz");
    }

    {   // VOICE: BBD only; DIGITAL and CLEAN are reserved values that play as BBD
        auto in = phrases (48000.0, 2.0);
        auto base = runPlugin (in, 48000.0, 512, 512, {}, 120.0);
        auto v1 = runPlugin (in, 48000.0, 512, 512, { { "voice", 1 } }, 120.0);
        auto v2 = runPlugin (in, 48000.0, 512, 512, { { "voice", 2 } }, 120.0);
        SedaAudioProcessor pr;
        auto* vp = dynamic_cast<juce::AudioParameterChoice*> (pr.apvts.getParameter ("voice"));
        check (vp != nullptr && vp->choices == juce::StringArray ({ "BBD", "DIGITAL", "CLEAN" }), "VOICE choices are BBD, DIGITAL, CLEAN (reserved values, order fixed for saved sessions)");
        check (bitEqual (base, v1) && bitEqual (base, v2), "VOICE DIGITAL / CLEAN (reserved) play as BBD: output bit-identical to BBD", juce::String::formatted ("maxDiff %.1e / %.1e", maxDiff (base, v1), maxDiff (base, v2)));
    }
}

static void testAgainstRaw()
{
    // the plug-in layer adds nothing to the engine: same Params, same samples (tolerance covers the percent -> fraction conversion in float)
    struct Case { const char* name; Settings st; seda::Params p; double bpm; int src; };   // src: 0 phrases, 1 plucks
    const double sr = 48000.0;
    std::vector<Case> cases;
    { seda::Params p; cases.push_back ({ "defaults", {}, p, -1.0, 0 }); }
    { seda::Params p; p.age = seda::kOld; p.comp = 0.5f; p.depth = 0.8f; p.rateHz = 2.0f; p.feedback = 0.6f; p.stereo = seda::kWide; p.widthMs = 9.0f; p.mix = 0.4f; p.tone = 0.3f; p.timeMs = 500.0f; p.touched = seda::kTouchNote | seda::kTouchMix | seda::kTouchStereo;
      cases.push_back ({ "OLD, mod, WIDE, TIME 500", { { "age", 1 }, { "comp", 50 }, { "depth", 80 }, { "rate", 2 }, { "feedback", 60 }, { "stereo", 1 }, { "width", 9 }, { "mix", 40 }, { "tone", 30 }, { "time", 500 } }, p, -1.0, 0 }); }
    { seda::Params p; p.sync = true; p.note = 11; p.bpm = 100.0; p.stereo = seda::kPingPong; p.feedback = 0.5f; p.touched = seda::kTouchNote | seda::kTouchStereo;
      cases.push_back ({ "SYNC 1/4d at 100 bpm, PING-PONG", { { "sync", 1 }, { "note", 11 }, { "stereo", 2 }, { "feedback", 50 } }, p, 100.0, 0 }); }
    { seda::Params p; p.listen = true; p.bpm = 90.0; p.throwDb = 6.0f; p.sync = true; p.note = 11; p.wetHpHz = 250.0f; p.duck = 0.6f;
      cases.push_back ({ "LISTEN with the VOCAL program's values in the knobs (SYNC 1/4d, HP 250, DUCK 60, THROW 6)", { { "listen", 1 }, { "sync", 1 }, { "note", 11 }, { "wetHp", 250 }, { "duck", 60 }, { "throw", 6 } }, p, 90.0, 0 }); }
    { seda::Params p; p.listen = true; p.sync = true; p.noteAuto = true; p.bpm = 120.0; p.touched = seda::kTouchNote; p.duck = 0.2f;
      cases.push_back ({ "LISTEN, NOTE AUTO, DUCK 20 % (a busy pluck train)", { { "listen", 1 }, { "sync", 1 }, { "noteAuto", 1 }, { "duck", 20 } }, p, 120.0, 1 }); }
    for (auto& c : cases)
    {
        c.p.touched = 0; c.p.source = 0;   // 11 Oct: the plug-in passes no hidden SOURCE overlay to the engine; a program is written into the knobs
        const auto in = c.src == 0 ? phrases (sr, 6.0) : plucks (sr, 6.0, 8.0);
        for (int block : { 512, 333 })
        {
            auto p = runPlugin (in, sr, block, block, c.st, c.bpm);
            seda::Params rp = c.p; if (c.bpm <= 0.0) rp.bpm = 120.0;
            auto r = runRaw (in, sr, block, rp);
            const double d = maxDiff (p, r);
            check (allFinite (p) && d < 2e-5, juce::String ("plug-in = raw engine: ") + c.name + ", block " + juce::String (block), juce::String::formatted ("maxAbsDiff=%.3e outPeak=%.3f", d, peakOf (p)));
        }
    }
    // a block larger than the prepared one (10000 vs 256): chunked inside, same result
    {
        const auto in = phrases (sr, 3.0);
        auto a = runPlugin (in, sr, 10000, 256, { { "feedback", 70 }, { "depth", 40 } }, 120.0);
        auto b = runPlugin (in, sr, 256, 256, { { "feedback", 70 }, { "depth", 40 } }, 120.0);
        check (bitEqual (a, b), "host block 10000 vs prepared 256 (chunked inside): bit-identical", juce::String::formatted ("maxAbsDiff=%.3e", maxDiff (a, b)));
    }
    // mono bus, STEREO = MONO: the same engine path as the left channel of the stereo bus when both sides carry the same signal
    // (tester finding T1, 17 Oct 2026: with PING-PONG / WIDE the mono bus now runs as MONO too, see testMonoBusMode)
    {
        auto st = phrases (sr, 3.0); st[1] = st[0];
        Audio mono { st[0] };
        auto a = runPlugin (mono, sr, 512, 512, { { "feedback", 60 }, { "depth", 50 }, { "stereo", 0 } }, 120.0);
        auto b = runPlugin (st, sr, 512, 512, { { "feedback", 60 }, { "depth", 50 }, { "stereo", 0 } }, 120.0);
        check (a.size() == 1 && a[0] == b[0] && allFinite (a), "mono bus = left channel of the stereo bus on identical input, STEREO MONO (bit-identical)", juce::String::formatted ("maxAbsDiff=%.3e", maxDiff (a, Audio { b[0] })));
    }
}

// ---------------------------------------------------------------------------------------------------- mono bus: STEREO runs as MONO (tester finding T1, 17 Oct 2026)
static uint64_t fnv1a (const Audio& a)
{
    uint64_t h = 1469598103934665603ull;
    for (auto& c : a) for (float v : c) { uint32_t u; std::memcpy (&u, &v, 4); for (int k = 0; k < 4; ++k) { h ^= (u >> (8 * k)) & 0xFFu; h *= 1099511628211ull; } }
    return h;
}

// ---------------------------------------------------------------------------------------------------- T4: self-oscillation is clean above 100 % FEEDBACK
struct ClickStat { int clicks = 0; double worst = 0.0, gapDb = 0.0; };
/** Clicks = first differences above 8 x their own local RMS (+-512 samples) and above 2e-3; gap = loudest / quietest 10 ms RMS (the burst-train depth). Samples [from, to). */
static ClickStat clickStat (const std::vector<float>& x, size_t from, size_t to, double sr)
{
    ClickStat r; to = std::min (to, x.size());
    const size_t n = to - from, H = 512;
    std::vector<double> d (n, 0.0), cs (n + 1, 0.0);
    for (size_t i = 1; i < n; ++i) d[i] = (double) x[from + i] - (double) x[from + i - 1];
    for (size_t i = 0; i < n; ++i) cs[i + 1] = cs[i] + d[i] * d[i];
    for (size_t i = 1; i < n; ++i)
    {
        const size_t lo = i > H ? i - H : 0, hi = std::min (n, i + H + 1);
        const double loc = std::sqrt ((cs[hi] - cs[lo] - d[i] * d[i]) / (double) (hi - lo - 1) + 1e-30), q = std::abs (d[i]) / loc;
        r.worst = std::max (r.worst, q);
        if (q > 8.0 && std::abs (d[i]) > 2e-3) ++r.clicks;
    }
    const size_t b = (size_t) (0.01 * sr); double mn = 1e9, mx = 0.0;
    for (size_t i = from; i + b <= to; i += b) { double q = 0; for (size_t k = 0; k < b; ++k) q += (double) x[i + k] * x[i + k]; q = std::sqrt (q / (double) b); mn = std::min (mn, q); mx = std::max (mx, q); }
    r.gapDb = toDb ((mx + 1e-12) / (mn + 1e-12));
    return r;
}

static void testSelfOscillation()
{
    const double sr = 48000.0;
    {   // the tester's case: FEEDBACK 110 %, COMPANDER 100 %, TIME 350, MIX 100, a burst, then 60 s of silence in. The old loop collapsed into a burst train (gap 105 dB, 70-180 clicks per 40 s).
        auto in = phrases (sr, 60.0, 0.25, 3);
        std::fill (in[0].begin() + (long) (3.0 * sr), in[0].end(), 0.0f); std::fill (in[1].begin() + (long) (3.0 * sr), in[1].end(), 0.0f);
        for (int block : { 512, 64 })
        {
            auto o = runPlugin (in, sr, block, block, { { "feedback", 110 }, { "comp", 100 }, { "time", 350 }, { "mix", 100 } }, -1.0);
            const auto st = clickStat (o[0], (size_t) (10.0 * sr), o[0].size(), sr), late = clickStat (o[0], (size_t) (40.0 * sr), o[0].size(), sr);
            check (allFinite (o) && st.clicks == 0 && late.gapDb < 20.0, juce::String ("FEEDBACK 110 %, COMPANDER 100 %, 60 s after a burst: no clicks, no pulse train, block ") + juce::String (block),
                   juce::String::formatted ("clicks 10..60 s = %d, worst outlier %.1f x, gap (40..60 s) %.1f dB, peak %.1f dB", st.clicks, st.worst, late.gapDb, toDb (peakOf (o, (size_t) (40.0 * sr)) )));
        }
    }
    {   // FEEDBACK automated 95 % -> 110 % -> 95 % over 4 s (per 64-sample block, as a host does) while a tone plays: no click at the 100 % crossings
        const int n = (int) (4.0 * sr); auto in = makeAudio (2, n);   // 0.3 s fade-in below: the tone's own start is not a click of the sweep
        for (int i = 0; i < n; ++i) { const double t = (double) i / sr; const float v = (float) (std::min (1.0, t / 0.3) * (0.2 * std::sin (2.0 * M_PI * 330.0 * t) + 0.1 * std::sin (2.0 * M_PI * 495.0 * t + 1.0))); in[0][(size_t) i] = v; in[1][(size_t) i] = v; }
        Rig r (sr, 64, 64, 2, { { "time", 350 }, { "mix", 100 }, { "comp", 100 }, { "feedback", 95 } }, -1.0);
        Audio out = makeAudio (2, n);
        for (int pos = 0; pos < n; pos += 64)
        {
            const double t = (double) pos / sr, fb = t < 2.0 ? 95.0 + 15.0 * t / 2.0 : 110.0 - 15.0 * (t - 2.0) / 2.0;
            setParam (r.proc.apvts, "feedback", (float) fb);
            Audio blk = slice (in, (size_t) pos, 64); auto o = r.run (blk);
            for (int c = 0; c < 2; ++c) std::copy (o[(size_t) c].begin(), o[(size_t) c].end(), out[(size_t) c].begin() + pos);
        }
        // crossings at fb = 100 %: t = 0.667 s (up) and 3.333 s (down), fade 101 %: 0.733 s / 3.267 s
        const auto up = clickStat (out[0], (size_t) (0.45 * sr), (size_t) (1.0 * sr), sr), dn = clickStat (out[0], (size_t) (3.0 * sr), (size_t) (3.55 * sr), sr), all = clickStat (out[0], (size_t) (0.1 * sr), (size_t) (3.9 * sr), sr);
        check (allFinite (out) && up.clicks == 0 && dn.clicks == 0 && all.clicks == 0, "FEEDBACK 95 -> 110 -> 95 % over 4 s with a tone playing: no click at the 100 % crossings",
               juce::String::formatted ("clicks up %d, down %d, whole sweep %d; worst outlier up %.1f x, down %.1f x", up.clicks, dn.clicks, all.clicks, up.worst, dn.worst));
        check (up.worst < 8.0 && dn.worst < 8.0, "the sweep's crossings are smooth: worst first-difference outlier under 8 x local RMS", juce::String::formatted ("%.1f x / %.1f x", up.worst, dn.worst));
    }
}

// ---------------------------------------------------------------------------------------------------- RUNAWAY / CHAOS (17 Oct 2026)
static float rawOf (SedaAudioProcessor& p, const char* id);
static void testPerform()
{
    std::printf ("\n--- RUNAWAY / CHAOS ---\n");
    {   // defaults: RUNAWAY 0 and CHAOS 0 written explicitly (moved and put back before playback) render bit-identically to an instance that never saw them.
        // (The golden hashes of testMonoBusMode were recorded on the engine BEFORE RUNAWAY / CHAOS existed and still pass: the engine at 0 / 0 is the old one.)
        const double sr = 48000.0; const auto in = phrases (sr, 5.0);
        const Settings st = { { "feedback", 75 }, { "depth", 40 }, { "comp", 80 }, { "stereo", 2 } };
        Rig a (sr, 512, 512, 2, st, 120.0), b (sr, 512, 512, 2, st, 120.0);
        setParam (b.proc.apvts, "runaway", 80.0f); setParam (b.proc.apvts, "chaos", 90.0f); setParam (b.proc.apvts, "runaway", 0.0f); setParam (b.proc.apvts, "chaos", 0.0f);
        auto oa = a.run (in), ob = b.run (in);
        check (bitEqual (oa, ob), "RUNAWAY 0 / CHAOS 0 (set explicitly) = the plain engine, bit for bit (FB 75, DEPTH 40, COMP 80, PING-PONG)", juce::String::formatted ("maxDiff=%.1e", maxDiff (oa, ob)));
    }
    {   // RUNAWAY 100 %, no input at all: the loop starts from its own hiss and settles at the servo level; bounded, finite. 44.1 / 48 / 96 kHz x blocks 32 / 512.
        for (double sr : { 44100.0, 48000.0, 96000.0 })
            for (int block : { 32, 512 })
            {
                const auto in = makeAudio (2, (int) (24.0 * sr));
                auto o = runPlugin (in, sr, block, block, { { "runaway", 100 }, { "time", 150 }, { "mix", 100 } }, -1.0);
                double sq = 0; const size_t a0 = (size_t) (19.0 * sr); for (size_t i = a0; i < o[0].size(); ++i) sq += 0.5 * ((double) o[0][i] * o[0][i] + (double) o[1][i] * o[1][i]);
                const double rmsDb = toDb (std::sqrt (sq / (double) (o[0].size() - a0))), pkDb = toDb (peakOf (o)), pkLate = toDb (peakOf (o, a0));
                // ceiling: FEEDBACK 110 % settles at -13.7 dB RMS (STATUS T4); RUNAWAY is held by the same servo: RMS <= -12 dB, peak <= -3 dBFS (the wet ceiling), and it must be oscillating (RMS > -20 dB)
                check (allFinite (o) && rmsDb > -20.0 && rmsDb <= -12.0 && pkDb <= -3.0, juce::String::formatted ("RUNAWAY 100 %%, TIME 150, silence in: self-oscillates and stays bounded, %.1f kHz block %d", sr / 1000.0, block),
                       juce::String::formatted ("last 5 s RMS %.2f dB, peak (whole render) %.2f dBFS, peak last 5 s %.2f dBFS", rmsDb, pkDb, pkLate));
            }
        {   // the onset from silence at the default TIME (300 ms): how long the hiss takes to grow into the drone (information, the gate is the line above)
            const double sr = 48000.0; const auto in = makeAudio (2, (int) (30.0 * sr));
            auto o = runPlugin (in, sr, 512, 512, { { "runaway", 100 }, { "mix", 100 } }, -1.0);
            double tUp = -1; for (size_t i = 0; i + 4800 < o[0].size(); i += 4800) { double q = 0; for (size_t k = 0; k < 4800; ++k) q += (double) o[0][i + k] * o[0][i + k]; if (toDb (std::sqrt (q / 4800.0)) > -20.0) { tUp = (double) i / sr; break; } }
            check (tUp > 0.0 && tUp < 25.0, "RUNAWAY 100 %, TIME 300 (default), silence in: the drone reaches -20 dB RMS from the hiss alone", juce::String::formatted ("after %.1f s", tUp));
        }
        {   // RUNAWAY 0 -> 100 -> 0 with programme, and FEEDBACK 110 % for comparison: same level ceiling
            const double sr = 48000.0; auto in = phrases (sr, 30.0, 0.2, 3);
            auto o = runPlugin (in, sr, 512, 512, { { "runaway", 100 }, { "mix", 100 } }, -1.0);
            auto f = runPlugin (in, sr, 512, 512, { { "feedback", 110 }, { "mix", 100 } }, -1.0);
            const size_t a0 = (size_t) (20.0 * sr);
            auto rms = [&] (const Audio& x) { double q = 0; for (size_t i = a0; i < x[0].size(); ++i) q += (double) x[0][i] * x[0][i]; return toDb (std::sqrt (q / (double) (x[0].size() - a0))); };
            check (allFinite (o) && rms (o) <= rms (f) + 1.5 && toDb (peakOf (o)) <= -3.0, "RUNAWAY 100 % on phrases: level within 1.5 dB of FEEDBACK 110 % (the same servo), peak under the -3 dBFS wet ceiling",
                   juce::String::formatted ("RUNAWAY %.2f dB RMS / FEEDBACK 110 %% %.2f dB RMS (last 10 s), peak %.2f dBFS", rms (o), rms (f), toDb (peakOf (o))));
        }
        {   // a live RUNAWAY move under a tone (0 -> 100 -> 0 over 6 s, per 64-sample block as a host automates): no click
            const double sr = 48000.0; const int n = (int) (6.0 * sr); auto in = makeAudio (2, n);
            for (int i = 0; i < n; ++i) { const double t = (double) i / sr; const float v = (float) (std::min (1.0, t / 0.3) * (0.2 * std::sin (2.0 * M_PI * 330.0 * t))); in[0][(size_t) i] = v; in[1][(size_t) i] = v; }
            Rig r (sr, 64, 64, 2, { { "time", 350 }, { "mix", 100 } }, -1.0);
            Audio out = makeAudio (2, n);
            for (int pos = 0; pos < n; pos += 64)
            {
                const double t = (double) pos / sr, v = t < 3.0 ? 100.0 * t / 3.0 : 100.0 * (6.0 - t) / 3.0;
                setParam (r.proc.apvts, "runaway", (float) v);
                Audio blk = slice (in, (size_t) pos, 64); auto o = r.run (blk);
                for (int c = 0; c < 2; ++c) std::copy (o[(size_t) c].begin(), o[(size_t) c].end(), out[(size_t) c].begin() + pos);
            }
            const auto st = clickStat (out[0], (size_t) (0.4 * sr), (size_t) n, sr);
            check (allFinite (out) && st.clicks == 0, "RUNAWAY automated 0 -> 100 -> 0 % over 6 s with a tone playing: no click", juce::String::formatted ("clicks %d, worst outlier %.1f x", st.clicks, st.worst));
        }
        {   // the LED's number: the effective loop gain
            Rig a (48000.0, 512, 512, 2, {}, -1.0), b (48000.0, 512, 512, 2, { { "runaway", 100 } }, -1.0), c (48000.0, 512, 512, 2, { { "feedback", 110 } }, -1.0), d (48000.0, 512, 512, 2, { { "runaway", 60 } }, -1.0);
            const auto in = phrases (48000.0, 0.5);
            a.run (in); b.run (in); c.run (in); d.run (in);
            const float ga = a.proc.getReadouts().loopGain, gb = b.proc.getReadouts().loopGain, gc = c.proc.getReadouts().loopGain, gd = d.proc.getReadouts().loopGain;
            check (ga < 1.0f && gd < 1.0f && gb > 1.15f && gc > 1.05f, "runaway state (the LED): loop gain > 1 at RUNAWAY 100 % and at FEEDBACK 110 %, < 1 at the defaults and at RUNAWAY 60 % (FEEDBACK 30 %)",
                   juce::String::formatted ("defaults %.3f, RUNAWAY 60 %.3f, RUNAWAY 100 %.3f, FEEDBACK 110 %.3f", ga, gd, gb, gc));
        }
    }
    {   // CHAOS steps land on the host's beats (120 bpm, the host reports its position and plays). Block 32: the block in which the step counter moves must hold a beat line.
        const double sr = 48000.0; const int block = 32;
        auto stepRun = [&] (float chaos, bool hostPos, int& steps, int& offBeat, int& beats)
        {
            Rig r (sr, block, block, 2, { { "runaway", 80 }, { "chaos", chaos }, { "time", 250 } }, 120.0);
            r.ph.hasPpq = hostPos; r.ph.playing = hostPos; r.ph.ppq = 0.37; r.ph.barStart = 0.0;
            const auto in = phrases (sr, 12.0, 0.15, 3);
            steps = 0; offBeat = 0; beats = 0; int last = 0;
            for (int pos = 0; pos + block <= (int) in[0].size(); pos += block)
            {
                const double q0 = r.ph.ppq, q1 = q0 + (double) block * 120.0 / (60.0 * sr);
                const bool beatHere = std::floor (q1 - 1e-12) > std::floor (q0 - 1e-12) || std::floor (q0) == q0;
                if (beatHere) ++beats;
                Audio blk = slice (in, (size_t) pos, (size_t) block); r.run (blk);
                const int now = r.proc.getReadouts().chaosSteps;
                if (now != last) { steps += now - last; if (! beatHere) ++offBeat; last = now; }
            }
        };
        int s100, off100, b100, s30, off30, b30, s75, off75, b75, sFree, offFree, bFree;
        stepRun (100.0f, true, s100, off100, b100); stepRun (75.0f, true, s75, off75, b75); stepRun (30.0f, true, s30, off30, b30); stepRun (100.0f, false, sFree, offFree, bFree);
        check (s100 > b100 / 2 && off100 == 0 && s75 > 0 && s75 < s100 && off75 == 0, "CHAOS 100 % / 75 %, host at 120 bpm playing: pitch steps happen, every one in a block that holds a beat line (block 32)",
               juce::String::formatted ("CHAOS 100: %d steps on %d beats, off the beat %d; CHAOS 75: %d steps, off the beat %d", s100, b100, off100, s75, off75));
        check (s30 == 0, "CHAOS 30 %: wobble only, no pitch steps", juce::String::formatted ("%d steps", s30));
        check (sFree > 0 && sFree <= (int) (12.0 / 0.25) + 1, "CHAOS 100 % without a host position: steps once per delay time at most (TIME 250 ms: <= 49 in 12 s)", juce::String::formatted ("%d steps", sFree));
        {   // reproducible: two instances, same settings, same output
            const auto in = phrases (sr, 6.0);
            const Settings st = { { "runaway", 70 }, { "chaos", 90 }, { "sync", 1 }, { "note", 6 } };
            auto a = runPlugin (in, sr, 512, 512, st, 120.0), b = runPlugin (in, sr, 512, 512, st, 120.0), c = runPlugin (in, sr, 128, 128, st, 120.0);
            check (bitEqual (a, b) && bitEqual (a, c) && allFinite (a), "CHAOS 90 % + RUNAWAY 70 %: fixed seed, two instances render bit-identically, also at block 128");
        }
    }
    {   // state: the two new parameters round-trip; a state saved before they existed loads them at 0
        SedaAudioProcessor a; setParam (a.apvts, "runaway", 64.5f); setParam (a.apvts, "chaos", 12.5f); setParam (a.apvts, "feedback", 70.0f);
        juce::MemoryBlock mb; a.getStateInformation (mb);
        SedaAudioProcessor b; b.setStateInformation (mb.getData(), (int) mb.getSize());
        const bool rt = rawOf (b, "runaway") == rawOf (a, "runaway") && rawOf (b, "chaos") == rawOf (a, "chaos");
        auto xml = juce::AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize());
        for (auto* id : { "runaway", "chaos" }) if (auto* e = xml->getChildByAttribute ("id", id)) xml->removeChildElement (e, true);
        juce::MemoryBlock old; juce::AudioProcessor::copyXmlToBinary (*xml, old);
        SedaAudioProcessor c; setParam (c.apvts, "runaway", 90.0f); setParam (c.apvts, "chaos", 90.0f);
        c.setStateInformation (old.getData(), (int) old.getSize());
        const bool oldOk = rawOf (c, "runaway") == 0.0f && rawOf (c, "chaos") == 0.0f && rawOf (c, "feedback") == 70.0f && xml->getChildByAttribute ("id", "runaway") == nullptr;
        check (rt && oldOk, "state: RUNAWAY / CHAOS round-trip; a state without them (saved before they existed) loads them at 0 and keeps the rest",
               juce::String::formatted ("restored %.1f / %.1f; old state -> %.1f / %.1f, FEEDBACK %.1f", rawOf (b, "runaway"), rawOf (b, "chaos"), rawOf (c, "runaway"), rawOf (c, "chaos"), rawOf (c, "feedback")));
        {   // and the old state renders exactly as a fresh instance with its values (no RUNAWAY / CHAOS left over)
            const auto in = phrases (48000.0, 3.0);
            Rig r1 (48000.0, 512, 512, 2, {}, 120.0), r2 (48000.0, 512, 512, 2, { { "feedback", 70 } }, 120.0);
            setParam (r1.proc.apvts, "runaway", 90.0f); setParam (r1.proc.apvts, "chaos", 90.0f);
            r1.proc.setStateInformation (old.getData(), (int) old.getSize());
            auto o1 = r1.run (in), o2 = r2.run (in);
            check (bitEqual (o1, o2), "an old state (no RUNAWAY / CHAOS) renders bit-identically to the same settings on a fresh instance", juce::String::formatted ("maxDiff=%.1e", maxDiff (o1, o2)));
        }
    }
}

static void testMonoBusMode()
{
    const double sr = 48000.0;
    const int at = 1000, T = 4800;   // TIME 100 ms
    const int nRep = 6;   // an impulse comes back about 11 dB lower per repeat through the line and the expander; repeat 7 is at the noise floor (-140 dB)
    auto peakAt = [&] (const std::vector<float>& x, int k) { double m = 0; for (int i = at + k * T - 200; i < at + k * T + 200 && i < (int) x.size(); ++i) m = std::max (m, (double) std::abs (x[(size_t) i])); return m; };
    auto imp = makeAudio (1, (int) (1.3 * sr)); imp[0][(size_t) at] = 1.0f;
    auto impSt = makeAudio (2, (int) (1.3 * sr)); impSt[0][(size_t) at] = 0.5f; impSt[1][(size_t) at] = 0.3f;
    const Settings base = { { "time", 100 }, { "feedback", 70 }, { "mix", 100 } };
    auto with = [&] (std::initializer_list<std::pair<const char*, float>> extra) { Settings s = base; for (auto& e : extra) s.push_back (e); return s; };

    // the reference: a mono bus with STEREO MONO (the plain mono path)
    auto ref = runPlugin (imp, sr, 512, 512, with ({ { "stereo", 0 } }), -1.0);
    for (int block : { 64, 512 })
    {
        auto pp = runPlugin (imp, sr, block, block, with ({ { "stereo", 2 } }), -1.0);
        auto wd = runPlugin (imp, sr, block, block, with ({ { "stereo", 1 }, { "width", 12 } }), -1.0);
        auto rf = runPlugin (imp, sr, block, block, with ({ { "stereo", 0 } }), -1.0);
        check (pp.size() == 1 && allFinite (pp) && bitEqual (pp, rf), juce::String ("mono bus PING-PONG = mono bus MONO (bit-identical), block ") + juce::String (block), juce::String::formatted ("maxAbsDiff=%.3e", maxDiff (pp, rf)));
        check (wd.size() == 1 && allFinite (wd) && bitEqual (wd, rf), juce::String ("mono bus WIDE (12 ms) = mono bus MONO (bit-identical), block ") + juce::String (block), juce::String::formatted ("maxAbsDiff=%.3e", maxDiff (wd, rf)));
    }
    {   // every repeat is there, at the level of the plain mono path, and the repeats decay by about the feedback
        auto pp = runPlugin (imp, sr, 256, 256, with ({ { "stereo", 2 } }), -1.0);
        bool allThere = true, inLevel = true, decays = true; double worstDev = 0, prev = 0, prevRef = 0, worstRatio = 0; juce::String pk, rk;
        for (int k = 1; k <= nRep; ++k)
        {
            const double p = peakAt (pp[0], k), r = peakAt (ref[0], k);
            pk << juce::String::formatted ("%s%.1f", k > 1 ? " " : "", toDb (p)); rk << juce::String::formatted ("%s%.1f", k > 1 ? " " : "", toDb (r));
            if (p < 1e-6) allThere = false;   // the repeats of the old build that went missing were at -140 dB
            worstDev = std::max (worstDev, std::abs (toDb (p) - toDb (r)));
            if (std::abs (toDb (p) - toDb (r)) > 0.5) inLevel = false;
            if (k > 1) { const double d = std::abs (toDb (p / prev) - toDb (r / prevRef)); worstRatio = std::max (worstRatio, d); if (d > 0.5 || p >= prev) decays = false; }
            prev = p; prevRef = r;
        }
        check (allThere, "mono bus PING-PONG, impulse in: repeats 1..6 (T, 2T, ... 6T) are all present (above -120 dB)", "peaks dB: " + pk);
        check (inLevel, "mono bus PING-PONG: every repeat within 0.5 dB of the plain mono path", juce::String::formatted ("worst deviation %.3f dB; plain mono path peaks dB: ", worstDev) + rk);
        check (decays, "mono bus PING-PONG: each repeat is lower than the one before, by the plain mono path's step (within 0.5 dB)", juce::String::formatted ("worst step deviation %.3f dB", worstRatio));
    }
    {   // the stereo bus is untouched: PING-PONG still alternates, and its output is bit-identical to the build before the change
        auto pp = runPlugin (impSt, sr, 512, 512, with ({ { "stereo", 2 } }), -1.0);
        bool alternates = true; juce::String pk;
        for (int k = 1; k <= 4; ++k)
        {
            const double l = peakAt (pp[0], k), r = peakAt (pp[1], k);
            pk << juce::String::formatted ("%s%d:L%.1f/R%.1f", k > 1 ? " " : "", k, toDb (l), toDb (r));
            if (k % 2 == 1 ? ! (l > 100.0 * r) : ! (r > 100.0 * l)) alternates = false;   // the input is summed into the left line: odd repeats on the left, even repeats on the right
        }
        check (alternates, "stereo bus PING-PONG still alternates (odd repeats left, even repeats right, the other side more than 40 dB down)", pk);
    }
    {   // bit-identity of the stereo bus with the build before the mono change (hashes recorded from that build, plugin_check 17 Oct 2026, arm64 Release; re-record on purpose if the engine is ever changed)
        struct Case { const char* name; Audio in; Settings st; double bpm; uint64_t golden; };
        std::vector<Case> cases;
        cases.push_back ({ "impulse, PING-PONG, TIME 100, FB 70, MIX 100", impSt, with ({ { "stereo", 2 } }), -1.0, 0x17719a38ef4586d7ull });
        cases.push_back ({ "phrases, PING-PONG, FB 60, DEPTH 50, SYNC 1/4d", phrases (sr, 5.0), { { "feedback", 60 }, { "depth", 50 }, { "stereo", 2 }, { "sync", 1 }, { "note", 11 } }, 100.0, 0x0c34ae3c63ac4e48ull });
        cases.push_back ({ "phrases, WIDE 12 ms, FB 75, OLD", phrases (sr, 5.0), { { "feedback", 75 }, { "stereo", 1 }, { "width", 12 }, { "age", 1 }, { "depth", 30 } }, -1.0, 0xd74b7e0323b32ec4ull });
        cases.push_back ({ "phrases, PING-PONG, FB 110, TIME 350, MIX 100 (re-recorded 17 Oct: above 100 % the compander fades out, T4)", phrases (sr, 5.0), { { "feedback", 110 }, { "stereo", 2 }, { "time", 350 }, { "mix", 100 } }, -1.0, 0xb73993f3ff959275ull });
        for (auto& c : cases)
        {
            auto o = runPlugin (c.in, sr, 512, 512, c.st, c.bpm);
            const uint64_t h = fnv1a (o);
            check (allFinite (o) && h == c.golden, juce::String ("stereo bus output unchanged (FNV-1a of the float output): ") + c.name, juce::String::formatted ("hash=0x%016llx", (unsigned long long) h));
        }
    }
}

static float rawOf (SedaAudioProcessor& p, const char* id) { return p.apvts.getRawParameterValue (id)->load(); }

static void testState()
{
    const double sr = 48000.0;
    SedaAudioProcessor a, b;
    for (auto& c : kAllParams) setParam (a.apvts, c.id, c.plain);
    juce::MemoryBlock mb; a.getStateInformation (mb);
    b.setStateInformation (mb.getData(), (int) mb.getSize());
    int bad = 0; double worst = 0.0;
    for (auto& c : kAllParams)
    {
        const float va = a.apvts.getRawParameterValue (c.id)->load(), vb = b.apvts.getRawParameterValue (c.id)->load();
        if (std::memcmp (&va, &vb, sizeof va) != 0) ++bad;
        worst = std::max (worst, (double) std::abs (va - vb));
    }
    SedaAudioProcessor fresh;
    int changed = 0;
    for (auto& c : kAllParams) if (fresh.apvts.getRawParameterValue (c.id)->load() != a.apvts.getRawParameterValue (c.id)->load()) ++changed;
    check (bad == 0 && changed == (int) std::size (kAllParams), "state round trip (22 parameters, bit for bit)", juce::String::formatted ("mismatches=%d maxDiff=%.1e parameters differing from defaults before restore=%d state bytes=%zu", bad, worst, changed, mb.getSize()));
    {   // a restored instance renders bit-identically to the original (a program applied, then DUCK moved by hand)
        const Settings st = { { "listen", 1 }, { "feedback", 55 }, { "depth", 30 } };
        const auto in = phrases (sr, 5.0);
        Rig ra (sr, 512, 512, 2, st, 100.0);
        ra.proc.applyProgram (1);
        setParam (ra.proc.apvts, "duck", 20.0f);
        juce::MemoryBlock m2; ra.proc.getStateInformation (m2);
        Rig rb (sr, 512, 512, 2, {}, 100.0);
        rb.proc.setStateInformation (m2.getData(), (int) m2.getSize());
        auto oa = ra.run (in), ob = rb.run (in);
        check (bitEqual (oa, ob), "restored state renders bit-identically (LISTEN, program VOCAL written into the knobs, DUCK 20 by hand)", juce::String::formatted ("maxDiff=%.1e", maxDiff (oa, ob)));
        auto xml = juce::ValueTree::fromXml (*juce::AudioProcessor::getXmlFromBinary (m2.getData(), (int) m2.getSize()));
        check (xml.hasProperty ("stateV") && ! xml.hasProperty ("touched"), "the saved state carries stateV 2 and no touched mask (the knobs are the truth)");
    }

    {   // garbage and foreign state are ignored, no crash, parameters unchanged
        SedaAudioProcessor g;
        const char junk[] = "this is not a state";
        g.setStateInformation (junk, (int) sizeof junk);
        juce::MemoryBlock foreign;
        { juce::XmlElement x ("SOMEONE_ELSE"); x.setAttribute ("foo", 1); g.copyXmlToBinary (x, foreign); }
        g.setStateInformation (foreign.getData(), (int) foreign.getSize());
        g.setStateInformation (nullptr, 0);
        check (g.apvts.getRawParameterValue ("time")->load() == 300.0f , "garbage / foreign / empty state: ignored, nothing changed");
    }

    {   // writing a knob never touches anything but that knob (the old 'touched' bits are gone)
        SedaAudioProcessor p;
        setParam (p.apvts, "duck", 10.0f); setParam (p.apvts, "duck", 50.0f);
        check (rawOf (p, "duck") == 50.0f, "a knob keeps exactly the value written to it, whatever program is selected");
    }
}

/** 11 Oct, the owner in Live: "every sound comes out late". An impulse at sample 1000 through the real processor: the first output sample is sample 1000 (the dry path has no
    delay, no latency is reported), its level is the dry gain of the MIX law (1 - MIX) within 0.1 dB, the first repeat arrives TIME later; also through processBlockBypassed. */
static void testDryPath()
{
    int bad = 0, cases = 0; juce::String first; double worstDb = 0.0;
    for (double sr : { 44100.0, 48000.0 })
        for (int block : { 32, 64, 128, 256, 480, 512, 1024 })
            for (int mode = 0; mode < 2; ++mode)
                for (float mix : { 0.0f, 40.0f, 100.0f })
                {
                    Settings st = { { "mix", mix }, { "feedback", 0 }, { "time", 300 } };
                    auto in = makeAudio (2, (int) sr); const int at = 1000; in[0][(size_t) at] = 0.5f; in[1][(size_t) at] = 0.5f;
                    const auto out = runPlugin (in, sr, block, block, st, 120.0, mode == 1);
                    ++cases;
                    const double g = mode == 1 ? 1.0 : 1.0 - mix / 100.0, got = std::abs (out[0][(size_t) at]);
                    if (g > 0.0)
                    {
                        const double db = toDb (got / (0.5 * g)); worstDb = std::max (worstDb, std::abs (db));
                        int firstIdx = -1; for (int i = 0; i < 20000; ++i) if (std::abs (out[0][(size_t) i]) > 0.1 * g) { firstIdx = i; break; }
                        if (firstIdx != at || std::abs (db) > 0.1) { ++bad; if (first.isEmpty()) first = juce::String::formatted ("%.0f Hz block %d mode %d MIX %.0f: first sample %d (want %d), level %+.2f dB", sr, block, mode, mix, firstIdx, at, db); }
                    }
                    else if (got > 1e-3) { ++bad; if (first.isEmpty()) first = "MIX 100 holds dry"; }
                    if (mode == 0 && mix > 0.0f)
                    {
                        const int want = at + (int) std::lround (0.3 * sr); int pk = -1; double pv = 0;
                        for (int i = want - 3000; i < want + 3000; ++i) if (std::abs (out[0][(size_t) i]) > pv) { pv = std::abs (out[0][(size_t) i]); pk = i; }
                        if (std::abs (pk - want) > (int) (0.005 * 0.3 * sr) + 2) { ++bad; if (first.isEmpty()) first = juce::String::formatted ("first repeat at %d, wanted %d", pk, want); }
                    }
                }
    SedaAudioProcessor p; p.setRateAndBufferSizeDetails (48000.0, 512); p.prepareToPlay (48000.0, 512);
    check (bad == 0 && p.getLatencySamples() == 0, "dry path: impulse in -> first output sample at the same sample, level = (1 - MIX) within 0.1 dB, first repeat at TIME, latency 0; 44.1/48 kHz, blocks 32..1024, processBlock and processBlockBypassed, MIX 0/40/100 (" + juce::String (cases) + " cases)",
           juce::String::formatted ("worst level error %.3f dB ", worstDb) + first);
}

static void testBypassAndMix0()
{
    const double sr = 48000.0;
    const auto in = mixOf (phrases (sr, 3.0), noiseBurst (sr, 3.0, 0.0, 3.0, 0.05, 9));
    const Settings loud = { { "feedback", 80 }, { "depth", 100 }, { "rate", 3 }, { "comp", 100 }, { "stereo", 1 }, { "width", 10 }, { "time", 120 }, { "listen", 1 }, { "source", 1 }, { "throw", 12 } };
    for (int block : { 512, 1, 333 })
    {
        Settings s = loud; s.push_back ({ "mix", 0 });
        auto a = runPlugin (in, sr, block, 512, s, 120.0);
        check (bitEqual (a, in), juce::String ("MIX 0 = the input, bit for bit (FEEDBACK 80 %, DEPTH 100 %, WIDE, LISTEN, THROW 12), block ") + juce::String (block), juce::String::formatted ("maxAbsDiff=%.1e", maxDiff (a, in)));
    }
    for (int block : { 512, 64 })
    {
        Settings s = loud; s.push_back ({ "bypass", 1 });
        auto a = runPlugin (in, sr, block, 512, s, 120.0);
        auto h = runPlugin (in, sr, block, 512, loud, 120.0, true);   // the host's own bypass: processBlockBypassed
        check (bitEqual (a, in) && bitEqual (h, in), juce::String ("BYPASS (parameter and host bypass call) = the input, bit for bit, latency 0, block ") + juce::String (block));
    }
    {   // switching BYPASS on and off mid-stream: no click, bit-exact once the 10 ms fade is over, the old repeats do not come back
        Settings s = loud; s.push_back ({ "mix", 60 });
        Rig r (sr, 512, 512, 2, s, 120.0);
        auto seg = [&] (double a, double b) { return slice (in, (size_t) (a * sr), (size_t) ((b - a) * sr)); };
        auto o1 = r.run (seg (0.0, 1.0));
        setParam (r.proc.apvts, "bypass", 1);
        auto o2 = r.run (seg (1.0, 2.0));
        double worstStep = 0.0;   // largest sample-to-sample step of the output over the fade region, against the input's own
        double inStep = 0.0;
        for (int c = 0; c < 2; ++c) for (int i = 1; i < 1024; ++i) { worstStep = std::max (worstStep, (double) std::abs (o2[(size_t) c][(size_t) i] - o2[(size_t) c][(size_t) i - 1])); inStep = std::max (inStep, (double) std::abs (in[(size_t) c][(size_t) (sr + i)] - in[(size_t) c][(size_t) (sr + i - 1)])); }
        auto after = slice (o2, 520, o2[0].size() - 520), inAfter = slice (seg (1.0, 2.0), 520, o2[0].size() - 520);
        check (bitEqual (after, inAfter), "BYPASS switched on mid-stream: bit-exact from 520 samples (10 ms fade) on");
        check (worstStep <= inStep * 4.0 + 0.05, "BYPASS fade: no click (largest step in the fade " + juce::String (worstStep, 3) + ", the input's own " + juce::String (inStep, 3) + ")");
        // un-bypass on silence: the engine was reset, so nothing of the old line comes back
        setParam (r.proc.apvts, "bypass", 0);
        auto silence = makeAudio (2, (int) (2.0 * sr));
        auto o3 = r.run (silence);
        check (toDb (peakOf (o3)) < -100.0, "BYPASS off again on silent input: output is silent (the old repeats did not come back; the line's own noise floor is under -100 dBFS)", "peak " + juce::String (toDb (peakOf (o3)), 1) + " dBFS");
    }
}

static void testBlockSizes()
{
    const double sr = 48000.0;
    const auto in = mixOf (phrases (sr, 5.0), plucks (sr, 5.0, 6.0));
    const Settings plain = { { "feedback", 65 }, { "time", 211.3f } };
    const Settings full = { { "age", 1 }, { "comp", 80 }, { "feedback", 85 }, { "tone", 40 }, { "depth", 100 }, { "rate", 2.5f }, { "stereo", 2 }, { "width", 7 }, { "mix", 55 },
                            { "sync", 1 }, { "note", 9 }, { "noteAuto", 1 }, { "listen", 1 }, { "source", 1 }, { "duck", 80 }, { "throw", 9 }, { "wetHp", 180 } };
    for (const auto* st : { &plain, &full })
    {
        const auto ref = runPlugin (in, sr, 512, 4096, *st, 100.0);
        std::vector<int> bad;
        for (int block : { 1, 2, 3, 7, 32, 64, 100, 333, 1000, 1024, 2048, 4096 })
            if (! bitEqual (runPlugin (in, sr, block, 4096, *st, 100.0), ref)) bad.push_back (block);
        check (bad.empty() && allFinite (ref), juce::String ("block sizes 1 ... 4096 bit-identical (12 sizes against 512), ") + (st == &plain ? "plain delay" : "every control on, LISTEN on"),
               bad.empty() ? "" : "differ at block " + juce::String (bad[0]));
    }
}

/** Delay of a 5 Hz sine through the wet line (phase delay = DC group delay, least squares over 3 s), samples; the integer number of periods is resolved with `expect`. */
static double sineDelaySamples (double sr, const Settings& st, double bpm, double expectSamples)
{
    const double f = 5.0, extra = 4.5;
    const int n = (int) (sr * (expectSamples / sr + extra));
    auto in = makeAudio (2, n);
    for (int i = 0; i < n; ++i) { const float v = (float) (0.3 * std::sin (2.0 * M_PI * f * (double) i / sr)); in[0][(size_t) i] = v; in[1][(size_t) i] = v; }
    Settings s = st; s.push_back ({ "mix", 100 }); s.push_back ({ "feedback", 0 }); s.push_back ({ "comp", 0 });
    auto out = runPlugin (in, sr, 512, 512, s, bpm);
    const int a = (int) (sr * (expectSamples / sr + 1.5)), m = (int) (3.0 * sr);
    // least squares y = A sin + B cos + C over [a, a + m)
    double M[3][3] = {}, rhs[3] = {};
    for (int i = a; i < a + m; ++i)
    {
        const double t = (double) i / sr, bs[3] = { std::sin (2.0 * M_PI * f * t), std::cos (2.0 * M_PI * f * t), 1.0 }, y = out[0][(size_t) i];
        for (int r = 0; r < 3; ++r) { rhs[r] += bs[r] * y; for (int c = 0; c < 3; ++c) M[r][c] += bs[r] * bs[c]; }
    }
    for (int p = 0; p < 3; ++p)   // Gauss elimination
    {
        for (int r = p + 1; r < 3; ++r) { const double k = M[r][p] / M[p][p]; for (int c = p; c < 3; ++c) M[r][c] -= k * M[p][c]; rhs[r] -= k * rhs[p]; }
    }
    double x[3];
    for (int r = 2; r >= 0; --r) { double v = rhs[r]; for (int c = r + 1; c < 3; ++c) v -= M[r][c] * x[c]; x[r] = v / M[r][r]; }
    const double ph = std::atan2 (x[1], x[0]);
    double d = -ph / (2.0 * M_PI * f) * sr;
    const double per = sr / f;
    d -= per * std::round ((d - expectSamples) / per);
    return d;
}

static void testSampleRates()
{
    for (double sr : { 44100.0, 48000.0, 96000.0 })
    {
        // everything on, LISTEN on, feedback past 100 %: finite and bounded at every rate
        const auto in = mixOf (phrases (sr, 4.0), plucks (sr, 4.0, 7.0));
        auto out = runPlugin (in, sr, 480, 480, { { "age", 1 }, { "feedback", 110 }, { "depth", 100 }, { "rate", 5 }, { "stereo", 2 }, { "mix", 70 }, { "time", 90 }, { "listen", 1 }, { "source", 2 }, { "throw", 12 }, { "comp", 100 } }, 133.0);
        const auto out2 = runPlugin (in, sr, 480, 480, { { "feedback", 80 }, { "sync", 1 }, { "noteAuto", 1 }, { "note", 6 }, { "listen", 1 }, { "wetHp", 300 }, { "duck", 100 } }, 97.0);
        check (allFinite (out) && allFinite (out2) && peakOf (out) < 4.0 && peakOf (out2) < 4.0, juce::String::formatted ("%.0f Hz: no NaN / inf with every control on (FEEDBACK 110 %%, OLD, DEPTH 100 %%, LISTEN)", sr),
               juce::String::formatted ("peaks %.2f / %.2f", peakOf (out), peakOf (out2)));
        // delay time in ms: free TIME 375 ms and SYNC 1/4d at 120 bpm (750 ms) -- measured on the sine's phase, as tools/measure.py D2 / D9 do
        for (const char* age : { "NEW", "OLD" })
        {
            const float a = std::string (age) == "OLD" ? 1.0f : 0.0f;
            const double want = 0.375 * sr, got = sineDelaySamples (sr, { { "time", 375 }, { "age", a } }, -1.0, want);
            const double wantS = 0.750 * sr, gotS = sineDelaySamples (sr, { { "sync", 1 }, { "note", 11 }, { "age", a } }, 120.0, wantS);
            check (std::abs (got - want) <= 0.005 * want, juce::String::formatted ("%.0f Hz %s: TIME 375 ms comes back at %.3f ms (%+.2f%%; limit +-0.5%%)", sr, age, got / sr * 1000.0, 100.0 * (got - want) / want));
            check (std::abs (gotS - wantS) <= 1.0, juce::String::formatted ("%.0f Hz %s: SYNC 1/4d at 120 bpm comes back at %.3f ms (%+.2f samples; limit +-1)", sr, age, gotS / sr * 1000.0, gotS - wantS));
        }
    }
}

static void testTail()
{
    struct Case { const char* name; Settings st; double bpm; bool listen; };
    const double sr = 48000.0;
    // COMPANDER 0 where the loop is deep: with the compander on (the default) the loop regrows from the line noise at FEEDBACK 40-70 % (see regrowthInfo(), STATUS.md P3 finding)
    const Case cases[] = {
        { "FEEDBACK 0, TIME 300 ms (one repeat)", { { "feedback", 0 }, { "time", 300 }, { "mix", 100 } }, -1.0, false },
        { "defaults (FEEDBACK 30 %, 300 ms, COMPANDER on)", {}, -1.0, false },
        { "FEEDBACK 55 %, TIME 100 ms, OLD, COMPANDER on", { { "feedback", 55 }, { "time", 100 }, { "age", 1 } }, -1.0, false },
        { "FEEDBACK 80 %, TIME 300 ms, COMPANDER 0, MOD DEPTH 50 %", { { "feedback", 80 }, { "time", 300 }, { "comp", 0 }, { "depth", 50 } }, -1.0, false },
        { "FEEDBACK 85 %, TIME 150 ms, COMPANDER 0, WIDE", { { "feedback", 85 }, { "time", 150 }, { "comp", 0 }, { "stereo", 1 }, { "width", 15 } }, -1.0, false },
        { "FEEDBACK 60 %, SYNC 1/4d at 90 bpm (1 s), COMPANDER 0", { { "feedback", 60 }, { "sync", 1 }, { "note", 11 }, { "comp", 0 } }, 90.0, false },
        { "FEEDBACK 50 %, 1/8 at 140 bpm, PING-PONG, COMPANDER 0", { { "feedback", 50 }, { "sync", 1 }, { "note", 6 }, { "stereo", 2 }, { "comp", 0 } }, 140.0, false },
        { "LISTEN, THROW 12 (feedback +15 % at a phrase end), FEEDBACK 60 %, COMPANDER 0", { { "feedback", 60 }, { "time", 250 }, { "comp", 0 }, { "listen", 1 }, { "throw", 12 } }, -1.0, true },
        { "LISTEN, SOURCE vocal (turns SYNC on: 1/4d), FEEDBACK 50 %, 80 bpm, COMPANDER 0", { { "feedback", 50 }, { "comp", 0 }, { "listen", 1 }, { "source", 1 } }, 80.0, true },
        { "LISTEN, NOTE AUTO (up to 1/2) at 110 bpm, FEEDBACK 40 %, COMPANDER 0", { { "feedback", 40 }, { "comp", 0 }, { "sync", 1 }, { "noteAuto", 1 }, { "listen", 1 } }, 110.0, true },
    };
    for (auto& c : cases)
    {
        Rig r (sr, 512, 512, 2, c.st, c.bpm);
        auto burst = noiseBurst (sr, 0.5, 0.0, 0.5, 0.25, 17);
        if (c.listen) burst = phrases (sr, 2.6, 0.2, 5);   // a phrase of 1.7 s so that the throw can fire (P3e: >= 1.2 s of material)
        const double burstLen = (double) burst[0].size() / sr;
        auto o1 = r.run (burst);
        const double tail = r.proc.getTailLengthSeconds();   // after the first block the processor knows the host tempo
        const double inPeak = peakOf (burst);
        if (! std::isfinite (tail) || tail > 40.0) { check (false, juce::String ("tail ") + c.name, "reported " + juce::String (tail, 2) + " s, too long to render in the check"); continue; }
        auto o2 = r.run (makeAudio (2, (int) ((tail + 0.6) * sr)));
        const double after = peakOf (o2, (size_t) (tail * sr), o2[0].size());
        const double rel = toDb (after) - toDb (inPeak);
        // how long the repeats were really audible (above -66 dB re input peak): the reported tail should not be far beyond it
        size_t lastAbove = 0; const double thr = inPeak * std::pow (10.0, -66.0 / 20.0);
        for (size_t i = 0; i < o2[0].size(); ++i) if (std::abs (o2[0][i]) > thr || std::abs (o2[1][i]) > thr) lastAbove = i;
        check (allFinite (o2) && rel <= -60.0, juce::String ("tail: ") + c.name,
               juce::String::formatted ("reported %.2f s; peak after it %.1f dB re the input peak (limit <= -60); the repeats were last above -66 dB at %.2f s", tail, rel, (double) lastAbove / sr));
        juce::ignoreUnused (burstLen, o1);
    }
    {   // endless / capped / zero
        SedaAudioProcessor p;
        auto tailOf = [&] (std::initializer_list<ParamCase> l) { SedaAudioProcessor q; for (auto& c : l) setParam (q.apvts, c.id, c.plain); return q.getTailLengthSeconds(); };
        check (std::isinf (tailOf ({ { "feedback", 100 } })) && std::isinf (tailOf ({ { "feedback", 110 } })), "tail: FEEDBACK 100 % or more reports an endless tail (the loop does not decay)");
        check (tailOf ({ { "feedback", 99 }, { "time", 1200 } }) == SedaAudioProcessor::kTailCapSeconds, "tail: capped at 60 s below 100 % (FEEDBACK 99 %, TIME 1.2 s)");
        check (tailOf ({ { "feedback", 0 }, { "time", 20 } }) < 0.2 && tailOf ({ { "feedback", 30 }, { "time", 600 } }) > tailOf ({ { "feedback", 30 }, { "time", 300 } })
                   && tailOf ({ { "feedback", 70 } }) > tailOf ({ { "feedback", 30 } }), "tail grows with TIME and FEEDBACK, FEEDBACK 0 / TIME 20 ms is under 0.2 s");
        check (p.getLatencySamples() == 0, "latency 0 at the default settings, and after prepareToPlay at 44.1 / 96 kHz", "latency " + juce::String (p.getLatencySamples()));
    }
    for (double sr : { 44100.0, 96000.0 })
    {
        Rig r (sr, 512, 512, 2, { { "feedback", 70 }, { "time", 900 }, { "depth", 100 }, { "listen", 1 }, { "throw", 12 } }, 120.0);
        r.run (noiseBurst (sr, 0.2, 0.0, 0.1, 0.1));
        check (r.proc.getLatencySamples() == 0, juce::String::formatted ("latency 0 samples at %.0f Hz with TIME 900 ms, DEPTH 100 %%, LISTEN on", sr));
    }
}

/** Not a pass / fail: the engine's COMPANDER regrowth, measured through the processor. A 0.5 s noise burst, then silence, FEEDBACK 85 %, TIME 300 ms:
    the loop level over 16 s. With the compander on (the default) the loop grows back from the line noise to the -3 dBFS ceiling; with COMPANDER 0 it
    falls to the noise floor. The time of the regrowth depends on the noise, so three bursts are shown. */
static void regrowthInfo()
{
    const double sr = 48000.0;
    for (int comp : { 100, 0 })
        for (uint64_t seed : { 21u, 22u, 23u })
        {
            Rig r (sr, 512, 512, 2, { { "feedback", 85 }, { "time", 300 }, { "mix", 100 }, { "comp", (float) comp } }, -1.0);
            r.run (noiseBurst (sr, 0.5, 0.0, 0.5, 0.25, seed));
            const auto o = r.run (makeAudio (2, (int) (16.0 * sr)));
            juce::String line;
            for (double t : { 1.0, 3.0, 6.0, 9.0, 12.0, 15.0 }) line << juce::String (toDb (peakOf (o, (size_t) (t * sr), (size_t) ((t + 0.4) * sr))), 1) << " ";
            std::printf ("[INFO] COMPANDER regrowth (engine, not a shell fault): FEEDBACK 85 %%, TIME 300 ms, COMPANDER %d %%, burst %d: peak dBFS at 1/3/6/9/12/15 s after the burst: %s\n", comp, (int) seed, line.toRawUTF8());
        }
}

static void testFeedbackOrder()
{
    // FEEDBACK orders the amount of repeats up to 100 %: above it the engine's level servo holds the loop at a fixed level (so the harness declares no direction)
    const double sr = 48000.0;
    const auto in = noiseBurst (sr, 2.0, 0.0, 0.3, 0.2, 33);
    double prev = -1e9; bool rising = true; juce::String vals;
    for (float fb : { 0.0f, 20.0f, 40.0f, 60.0f, 80.0f })
    {
        auto o = runPlugin (in, sr, 512, 512, { { "feedback", fb }, { "time", 120 }, { "mix", 100 }, { "comp", 0 } }, -1.0);
        double e = 0; for (size_t i = (size_t) (0.5 * sr); i < o[0].size(); ++i) e += (double) o[0][i] * o[0][i];
        const double v = 10.0 * std::log10 (e / (double) o[0].size() + 1e-30);
        vals << juce::String (v, 1) << " ";
        rising = rising && v > prev + 0.5; prev = v;
    }
    check (rising, "FEEDBACK 0 / 20 / 40 / 60 / 80 %: the energy of the repeats after the first 0.5 s rises with every step (dB)", vals);
}

/** What each parameter does to the sound, through the real processor, no harness: base render against the render with the knob moved, with the context the knob needs. */
// ---------------------------------------------------------------------------------------------------- P3b: reset() reseeds, NOTE AUTO follows the host bar
static void testResetAndHostBars()
{
    const double sr = 48000.0;
    {   // a render, a state load, the same render: identical (the noise and drift generators start from their fixed seeds again)
        const Settings st = { { "feedback", 60 }, { "mix", 100 }, { "depth", 100 }, { "rate", 1.3f }, { "age", 1 }, { "stereo", 1 }, { "comp", 100 } };
        const auto in = mixOf (plucks (sr, 6.0, 4.0), noiseBurst (sr, 6.0, 1.0, 1.5, 0.1));
        for (int block : { 512, 77 })
        {
            Rig r (sr, block, block, 2, st, 120.0);
            const auto a = r.run (in);
            juce::MemoryBlock mb; r.proc.getStateInformation (mb);
            r.proc.setStateInformation (mb.getData(), (int) mb.getSize());
            const auto b = r.run (in);
            Rig fresh (sr, block, block, 2, st, 120.0);
            const auto c = fresh.run (in);
            check (bitEqual (a, b) && bitEqual (a, c) && peakOf (a) > 0.01, juce::String ("a render, a state load, a render: bit-identical to each other and to a fresh instance (DEPTH 100 %, OLD, noise on), block ") + juce::String (block),
                   juce::String::formatted ("maxDiff load %.1e, fresh %.1e", maxDiff (a, b), maxDiff (a, c)));
        }
    }
    {   // NOTE AUTO: the changes land on the host's bar lines. The fake playhead starts at ppq 2.5 (half way through the first bar of 4/4), 120 bpm: the host's
        // bar lines are at 0.75, 2.75, 4.75 ... s; counting from the start of playback would put them at 2, 4, 6 ... s.
        const int blk = 64;
        auto material = [&] (double secs)
        {
            auto o = plucks (sr, secs, 8.0);                                   // dense for the first 10 s, then sparse, then dense again
            const auto sparse = plucks (sr, secs, 0.8);
            for (size_t c = 0; c < 2; ++c) for (size_t i = (size_t) (10.0 * sr); i < (size_t) (22.0 * sr) && i < o[c].size(); ++i) o[c][i] = sparse[c][i];
            return o;
        };
        const auto in = material (30.0);
        auto changes = [&] (bool hostPos, double ppq0)
        {
            Rig r (sr, blk, blk, 2, { { "sync", 1 }, { "note", 9 }, { "noteAuto", 1 }, { "listen", 1 }, { "feedback", 30 }, { "mix", 50 } }, 120.0);
            r.ph.hasPpq = hostPos; r.ph.ppq = ppq0; r.ph.barStart = std::floor (ppq0 / 4.0) * 4.0;
            std::vector<double> at; int last = r.proc.getReadouts().chosenNote;
            const int n = (int) in[0].size();
            for (int pos = 0; pos + blk <= n; pos += blk)
            {
                r.buf.setSize (2, blk, false, false, true);
                for (int c = 0; c < 2; ++c) std::copy (in[(size_t) c].begin() + pos, in[(size_t) c].begin() + pos + blk, r.buf.getWritePointer (c));
                r.proc.processBlock (r.buf, r.midi);
                if (r.ph.hasPpq) { r.ph.ppq += (double) blk * r.ph.bpm / (60.0 * sr); r.ph.barStart = std::floor (r.ph.ppq / 4.0 + 1e-9) * 4.0; }
                const int now = r.proc.getReadouts().chosenNote;
                if (pos == 0) last = now;   // the first block loads the NOTE knob as the starting note (the read-out held its default before)
                else if (now != last) { at.push_back ((double) (pos + blk) / sr); last = now; }
            }
            return at;
        };
        auto offLine = [&] (const std::vector<double>& at, double firstLine)   // distance (s) of the worst change from a bar line (2 s apart)
        {
            double worst = 0.0;
            for (double t : at) { const double k = std::fmod (t - firstLine, 2.0); worst = std::max (worst, std::min (k < 0 ? k + 2.0 : k, 2.0 - (k < 0 ? k + 2.0 : k))); }
            return worst;
        };
        const auto host = changes (true, 2.5);
        const auto plain = changes (false, 0.0);
        juce::String hs, ps; for (double t : host) hs << juce::String (t, 3) << " "; for (double t : plain) ps << juce::String (t, 3) << " ";
        check (host.size() >= 2 && offLine (host, 0.75) <= 2.0 * blk / sr, "NOTE AUTO with a host playhead starting mid-bar (ppq 2.5): every change lands on a host bar line (0.75 + 2 k s)",
               juce::String::formatted ("%zu changes at s: ", host.size()) + hs + juce::String::formatted ("worst distance %.4f s", offLine (host, 0.75)));
        check (host.size() >= 2 && offLine (host, 2.0) > 0.5, "...and not on the lines counted from the start of playback (0 + 2 k s)", juce::String::formatted ("worst distance %.3f s", offLine (host, 2.0)));
        check (plain.size() >= 2 && offLine (plain, 0.0) <= 2.0 * blk / sr, "no host position (tempo only): the bar lines are counted from the start of playback (0 + 2 k s), as before",
               juce::String::formatted ("%zu changes at s: ", plain.size()) + ps + juce::String::formatted ("worst distance %.4f s", offLine (plain, 0.0)));
    }
}

static void testAudibility()
{
    const double sr = 48000.0;
    struct A { const char* id; Settings ctx; float from, to; int mat; const char* note; };   // mat 0 phrases, 1 plucks, 2 noise, 3 long phrases (1.7 s, for the throw)
    const A rows[] = {
        { "time", {}, 300, 600, 0, "" }, { "sync", { { "note", 9 } }, 0, 1, 0, "" }, { "note", { { "sync", 1 } }, 9, 6, 0, "" },
        { "noteAuto", { { "sync", 1 }, { "listen", 1 } }, 0, 1, 1, "needs a bar line (2 s) to act" },
        { "feedback", {}, 0, 70, 0, "" }, { "mix", {}, 0, 50, 0, "" }, { "age", {}, 0, 1, 0, "" }, { "comp", {}, 0, 100, 0, "" },
        { "tone", { { "feedback", 70 } }, 0, 80, 0, "" }, { "rate", { { "depth", 80 } }, 0.3f, 4.0f, 0, "" }, { "depth", { { "rate", 2 } }, 0, 100, 0, "" },
        { "shape", { { "depth", 100 }, { "rate", 2 } }, 0, 1, 0, "" }, { "stereo", {}, 0, 2, 0, "" }, { "width", { { "stereo", 1 } }, 0, 15, 0, "" },
        { "listen", {}, 0, 1, 0, "" }, { "duck", { { "listen", 1 } }, 0, 100, 0, "" },
        { "throw", { { "listen", 1 }, { "duck", 0 }, { "feedback", 50 } }, 0, 12, 3, "needs a phrase end (>= 1.2 s of material, then a confirmed gap)" }, { "wetHp", { { "listen", 1 }, { "feedback", 50 } }, 0, 500, 2, "" },
        { "bypass", {}, 0, 1, 0, "" },
        { "runaway", {}, 0, 100, 0, "" }, { "chaos", { { "feedback", 60 } }, 0, 40, 0, "" } };
    const Audio mats[4] = { phrases (sr, 8.0), plucks (sr, 8.0, 8.0), noiseBurst (sr, 8.0, 0.0, 8.0, 0.1), phrases (sr, 8.0, 0.12, 5) };
    int silent = 0; juce::String list;
    for (auto& r : rows)
    {
        Settings a = r.ctx, b = r.ctx;
        a.push_back ({ r.id, r.from }); b.push_back ({ r.id, r.to });
        const auto oa = runPlugin (mats[r.mat], sr, 512, 512, a, 120.0), ob = runPlugin (mats[r.mat], sr, 512, 512, b, 120.0);
        const double d = toDb (maxDiff (oa, ob));
        const bool audible = d > -60.0 && allFinite (ob);
        if (! audible) { ++silent; list << r.id << " (" << juce::String (d, 0) << " dB) "; }
        std::printf ("    %-9s %6.1f -> %6.1f : largest change of the output %7.1f dBFS %s\n", r.id, r.from, r.to, d, r.note);
    }
    {   // SOURCE is the label of the last program chosen: a host writing it changes nothing in the sound; the program (applyProgram) writes the knobs, and that is audible
        const auto base = runPlugin (mats[0], sr, 512, 512, { { "listen", 1 } }, 120.0), lab = runPlugin (mats[0], sr, 512, 512, { { "listen", 1 }, { "source", 1 } }, 120.0);
        Rig rg (sr, 512, 512, 2, { { "listen", 1 } }, 120.0); rg.proc.applyProgram (1);
        const auto prog = rg.run (mats[0]);
        const double dl = toDb (maxDiff (base, lab)), dp = toDb (maxDiff (base, prog));
        std::printf ("    source    label only (host write) : change of the output %7.1f dBFS; program P1 via applyProgram : %7.1f dBFS\n", dl, dp);
        if (! (maxDiff (base, lab) == 0.0 && dp > -60.0)) { ++silent; list << "source (label " << juce::String (dl, 0) << " dB, program " << juce::String (dp, 0) << " dB) "; }
    }
    check (silent == 0, "ControlProof (processor level): every parameter except VOICE audibly changes the output (largest change over -60 dBFS)", silent == 0 ? "" : "silent: " + list);
    {   // documented as not doing anything: VOICE's reserved values
        const auto base = runPlugin (mats[0], sr, 512, 512, {}, 120.0), v1 = runPlugin (mats[0], sr, 512, 512, { { "voice", 1 } }, 120.0);
        check (bitEqual (base, v1), "VOICE: documented as not changing the sound yet (DIGITAL / CLEAN are reserved values that play as BBD): output identical");
    }
}


// ---------------------------------------------------------------------------------------------------- P4: the panel (SedaEditor)
static double residentMB()
{
#if JUCE_MAC
    mach_task_basic_info info; mach_msg_type_number_t cnt = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info (mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t) &info, &cnt) == KERN_SUCCESS) return (double) info.resident_size / 1048576.0;
#endif
    return 0.0;
}

static void pump (int ms = 30) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

// ---------------------------------------------------------------------------------------------------- UI signature (UI_SIGNATURE_STANDARD.md 2 and 3)
static juce::MouseEvent clickAt (juce::Component& c, juce::Point<float> pos)
{
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), pos, juce::ModifierKeys(), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                             &c, &c, now, pos, now, 1, false);
}

static void testSignature()
{
    std::printf ("\n--- UI signature: double-click default, lockup link ---\n");
    SedaAudioProcessor proc;
    proc.setRateAndBufferSizeDetails (48000.0, 512);
    proc.prepareToPlay (48000.0, 512);
    SedaEditor ed (proc);
    auto plain = [&proc] (const char* id) { return proc.apvts.getRawParameterValue (id)->load(); };
    auto dbl = [&ed] (const char* id) { if (auto* k = ed.knob (id)) if (k->onDoubleClick) k->onDoubleClick(); };

    check (plain ("runaway") == 0.0f && plain ("chaos") == 0.0f, "RUNAWAY and CHAOS open at 0 (off) in a fresh instance",
           "runaway " + juce::String (plain ("runaway")) + " chaos " + juce::String (plain ("chaos")));

    {   // no program: double-click returns to the factory default
        setParam (proc.apvts, "duck", 10.0f); setParam (proc.apvts, "runaway", 55.0f); setParam (proc.apvts, "chaos", 80.0f); setParam (proc.apvts, "time", 900.0f);
        dbl ("duck"); dbl ("runaway"); dbl ("chaos"); dbl ("time");
        check (std::abs (plain ("duck") - 50.0f) < 0.01f && plain ("runaway") == 0.0f && plain ("chaos") == 0.0f && std::abs (plain ("time") - 300.0f) < 0.5f,
               "double-click with MANUAL loaded: DUCK 50, RUNAWAY 0, CHAOS 0, TIME 300 (factory defaults)",
               juce::String (plain ("duck")) + " " + juce::String (plain ("runaway")) + " " + juce::String (plain ("chaos")) + " " + juce::String (plain ("time")));
    }
    {   // a program loaded: double-click returns to the program's value, asked at click time, and the drawing glides there
        proc.applyProgram (1);   // VOCAL: DUCK 60 %, WET HP 250 Hz
        for (int i = 0; i < 12; ++i) ed.tickForSnapshot();
        const float vocalDuck = plain ("duck"), vocalHp = plain ("wetHp");
        setParam (proc.apvts, "duck", 5.0f); setParam (proc.apvts, "wetHp", 20.0f);
        auto* k = ed.knob ("duck");
        dbl ("duck");
        const float atOnce = plain ("duck");
        ed.tickForSnapshot();
        const float eff1 = k != nullptr ? (float) k->getProperties().getWithDefault ("effPos", -1.0f) : -1.0f;
        const float to01 = proc.apvts.getParameter ("duck")->getValue(), from01 = proc.apvts.getParameter ("duck")->convertTo0to1 (5.0f);
        for (int i = 0; i < 10; ++i) ed.tickForSnapshot();
        const float effEnd = k != nullptr ? (float) k->getProperties().getWithDefault ("effPos", -1.0f) : 0.0f;
        dbl ("wetHp");
        check (std::abs (atOnce - vocalDuck) < 0.01f && std::abs (plain ("wetHp") - vocalHp) < 0.5f && std::abs (vocalDuck - 50.0f) > 1.0f,
               "double-click with VOCAL loaded: DUCK " + juce::String (atOnce, 1) + " = the program's " + juce::String (vocalDuck, 1) + " (factory 50), WET HP " + juce::String (plain ("wetHp"), 1) + " = " + juce::String (vocalHp, 1) + "; the parameter changes at once");
        check (eff1 > juce::jmin (from01, to01) && eff1 < juce::jmax (from01, to01) && effEnd < 0.0f,
               "the knob drawing glides: after one tick at " + juce::String (eff1, 3) + " between " + juce::String (from01, 3) + " and " + juce::String (to01, 3) + ", settled after 11 ticks");
        proc.applyProgram (0);
        setParam (proc.apvts, "duck", 5.0f);
        dbl ("duck");
        check (std::abs (plain ("duck") - 50.0f) < 0.01f, "after switching back to MANUAL the same double-click gives the factory 50 (asked at click time)", juce::String (plain ("duck")));
    }
    {   // the lockup: a click on it opens https://subverselab.com, a click elsewhere does not
        int opened = 0;
        ed.openLockupLink_forTest = [&opened] { ++opened; };
        const auto area = ed.lockupArea();
        ed.mouseUp (clickAt (ed, area.getCentre()));
        const int onLogo = opened;
        ed.mouseUp (clickAt (ed, { 800.0f, 300.0f }));
        check (! area.isEmpty() && area.getRight() <= (float) SedaEditor::kW && area.getY() > (float) SedaEditor::kH * 0.8f && onLogo == 1 && opened == 1
                   && juce::String (SedaEditor::kLockupUrl) == "https://subverselab.com",
               "lockup bottom right (" + area.toString() + "): a click opens " + juce::String (SedaEditor::kLockupUrl) + ", a click on the panel does not");
    }
}

static void testEditor()
{
    std::printf ("\n--- P4: the panel ---\n");
    const double sr = 48000.0;
    SedaAudioProcessor proc;
    proc.setRateAndBufferSizeDetails (sr, 512);
    proc.prepareToPlay (sr, 512);

    {   // 1. open / close 20 times: no editor left behind, the resident memory does not climb (the plate image alone is 4 MB)
        double after5 = 0.0, after20 = 0.0;
        int leftBehind = 0;
        for (int i = 0; i < 20; ++i)
        {
            std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
            ed->setVisible (false);
            auto* se = dynamic_cast<SedaEditor*> (ed.get());
            if (se != nullptr) se->tickForSnapshot();
            pump (5);
            ed.reset();
            pump (5);
            if (proc.getActiveEditor() != nullptr) ++leftBehind;
            if (i == 4) after5 = residentMB();
            if (i == 19) after20 = residentMB();
        }
        setParam (proc.apvts, "time", 321.0f); setParam (proc.apvts, "duck", 40.0f);   // parameters still change with no editor (no dangling attachment)
        check (leftBehind == 0 && (after5 == 0.0 || after20 - after5 < 8.0), "editor open/close x20: none left behind, resident memory after cycle 5 " + juce::String (after5, 1) + " MB, after cycle 20 " + juce::String (after20, 1) + " MB (limit +8 MB), parameters still change afterwards");
    }

    SedaEditor ed (proc);
    {   // 2. every control is bound, every parameter (except VOICE) has a control
        std::set<juce::String> ids; int unbound = 0, n = 0;
        juce::String missing;
        for (auto* c : ed.controls())
        {
            const auto id = c->paramId();
            if (id.isEmpty()) continue;   // the SIZE chip: a panel setting, kept in the state, not a parameter
            ++n;
            if (proc.apvts.getParameter (id) == nullptr) { ++unbound; missing << id << " "; }
            ids.insert (id);
        }
        juce::String noControl;
        for (auto* p : proc.getParameters())
            if (auto* pid = dynamic_cast<juce::AudioProcessorParameterWithID*> (p))
                if (pid->paramID != "voice" && ids.count (pid->paramID) == 0) noControl << pid->paramID << " ";
        check (unbound == 0 && noControl.isEmpty() && ids.count ("voice") == 0, "panel: " + juce::String (n) + " bound controls over " + juce::String ((int) ids.size()) + " parameters; no control without a parameter, no parameter without a control except VOICE (BBD is pinned, no selector)",
               unbound == 0 && noControl.isEmpty() ? "" : "unbound: " + missing + " | no control: " + noControl);
    }
    {   // 3. a knob drag changes only its own parameter
        int bad = 0, tested = 0; juce::String list;
        for (const char* id : { "time", "feedback", "tone", "comp", "rate", "depth", "width", "wetHp", "duck", "throw", "mix", "runaway", "chaos" })
        {
            auto* k = ed.knob (id);
            if (k == nullptr) { ++bad; list << id << "(missing) "; continue; }
            std::map<juce::String, float> before;
            for (auto* p : proc.getParameters()) if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (p)) before[r->paramID] = r->getValue();
            auto src = juce::Desktop::getInstance().getMainMouseSource();
            const auto mods = juce::ModifierKeys().withFlags (juce::ModifierKeys::leftButtonModifier);
            const auto t0 = juce::Time::getCurrentTime();
            auto ev = [&] (juce::Point<float> pos, bool dragged) { return juce::MouseEvent (src, pos, mods, 1.0f, juce::MouseInputSource::defaultOrientation, juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX, juce::MouseInputSource::defaultTiltY, k, k, t0, { 20.0f, 30.0f }, t0, 1, dragged); };
            const float dir = before[id] > 0.5f ? 1.0f : -1.0f;   // up from the lower half, down from the upper half (COMPANDER starts at its maximum)
            k->mouseDown (ev ({ 20.0f, 30.0f }, false));
            for (int s = 1; s <= 6; ++s) k->mouseDrag (ev ({ 20.0f, 30.0f + dir * 8.0f * (float) s }, true));
            k->mouseUp (ev ({ 20.0f, 30.0f + dir * 48.0f }, true));
            pump (20);
            int changed = 0; juce::String which;
            for (auto* p : proc.getParameters())
                if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (p))
                    if (r->getValue() != before[r->paramID]) { ++changed; which << r->paramID << " "; }
            ++tested;
            if (changed != 1 || ! which.startsWith (id)) { ++bad; list << id << "(" << which << ") "; }
            svl::setParamAsGesture (dynamic_cast<juce::RangedAudioParameter*> (proc.apvts.getParameter (id)), dynamic_cast<juce::RangedAudioParameter*> (proc.apvts.getParameter (id))->convertFrom0to1 (before[id]));
        }
        check (bad == 0 && tested == 13, "knob drag: each of the 13 knobs (RUNAWAY and CHAOS included), dragged, changes its own parameter and no other", bad == 0 ? "" : "bad: " + list);
    }
    {   // 4. keys: each key writes its parameter and nothing else; AUTO needs SYNC; WIDTH dims in MONO; the dead-looking cases are dimmed, not silent
        setParam (proc.apvts, "sync", 0.0f); setParam (proc.apvts, "noteAuto", 0.0f); setParam (proc.apvts, "stereo", 0.0f); setParam (proc.apvts, "listen", 0.0f);
        ed.tickForSnapshot();
        const bool autoOffWithoutSync = ! ed.autoKeyEnabled() && ! ed.key ("noteAuto")->isEnabled();
        ed.key ("sync")->triggerClick(); pump (20); ed.tickForSnapshot();
        const bool syncOn = proc.apvts.getRawParameterValue ("sync")->load() > 0.5f, autoOn = ed.key ("noteAuto")->isEnabled();
        ed.key ("noteAuto")->triggerClick(); pump (20);
        const bool autoSet = proc.apvts.getRawParameterValue ("noteAuto")->load() > 0.5f;
        ed.key ("stereo", 2)->triggerClick(); pump (20); ed.tickForSnapshot();
        const bool ping = juce::roundToInt (proc.apvts.getRawParameterValue ("stereo")->load()) == 2 && ! ed.widthDimmed();
        ed.key ("stereo", 0)->triggerClick(); pump (20); ed.tickForSnapshot();
        const bool mono = ed.widthDimmed();
        ed.toggle ("listen")->triggerClick(); pump (20);
        const bool listenOn = proc.apvts.getRawParameterValue ("listen")->load() > 0.5f;
        for (int i = 0; i < 3; ++i) { ed.stepKey (1)->triggerClick(); pump (20); }
        const bool bass = juce::roundToInt (proc.apvts.getRawParameterValue ("source")->load()) == 3;
        ed.key ("shape", 1)->triggerClick(); pump (20);
        const bool tri = juce::roundToInt (proc.apvts.getRawParameterValue ("shape")->load()) == 1;
        check (autoOffWithoutSync && syncOn && autoOn && autoSet && ping && mono && listenOn && bass && tri,
               "keys: AUTO disabled without SYNC, enabled with it; PING-PONG / MONO / LISTEN / legend step P3 BASS (three step keys) / SHAPE TRI write their parameters; WIDTH dims in MONO only");
        for (auto* id : { "sync", "noteAuto", "stereo", "listen", "source", "shape" }) setParam (proc.apvts, id, 0.0f);
    }
    {   // 4b. the VFD program: step keys and the program line write `source`, one undo step each, wrap round, dim with a visible reason
        setParam (proc.apvts, "source", 0.0f); setParam (proc.apvts, "listen", 0.0f); ed.tickForSnapshot();
        auto src = [&] { return juce::roundToInt (proc.apvts.getRawParameterValue ("source")->load()); };
        const bool reasonShown = ed.programIndex() == 0 && ed.stepKey (1) != nullptr && ed.programLine() != nullptr;
        const int s0 = proc.undo.undoSteps();
        ed.stepKey (1)->triggerClick(); pump (20);
        const bool one = src() == 1 && proc.undo.undoSteps() == s0 + 1;
        ed.programLine()->step (1); pump (20);
        const bool line = src() == 2 && proc.undo.undoSteps() == s0 + 2;
        ed.programLine()->step (1); pump (20);
        const bool picked = src() == 3 && proc.undo.undoSteps() == s0 + 3;
        ed.programLine()->step (-1); pump (20);
        ed.programLine()->step (-1); pump (20); ed.stepKey (-1)->triggerClick(); pump (20);
        const bool back = src() == 0 && proc.undo.undoSteps() == s0 + 6;
        ed.stepKey (-1)->triggerClick(); pump (20);
        const bool wrap = src() == 4;
        ed.tickForSnapshot();
        const bool name = ed.programIndex() == 4;
        ed.keyPressed (juce::KeyPress ('z', juce::ModifierKeys::commandModifier, 'z')); pump (20);
        const bool undone = src() == 0;
        check (reasonShown && one && line && picked && back && wrap && name && undone, "LED program name: click / wheel (step) and the step keys write SOURCE, one undo step per step (wraps P00 -> P04), the LED name shows P4, Cmd+Z takes a step back");
        setParam (proc.apvts, "source", 0.0f);
    }
    {   // 5. the movement display follows the engine's accessor, block by block
        SedaAudioProcessor p2;
        p2.setRateAndBufferSizeDetails (sr, 512); p2.prepareToPlay (sr, 512);
        setParam (p2.apvts, "listen", 1.0f); p2.applyProgram (1); setParam (p2.apvts, "feedback", 55.0f); setParam (p2.apvts, "mix", 50.0f);
        SedaEditor e2 (p2);
        const auto in = phrases (sr, 12.0, 0.12, 5);
        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer midi;
        int blocks = 0, mismatches = 0, deep = 0, back = 0, throwSeen = 0, throwLampSeen = 0;
        float deepest = 0.0f, worst = 0.0f;
        for (int pos = 0; pos + 512 <= (int) in[0].size(); pos += 512, ++blocks)
        {
            for (int c = 0; c < 2; ++c) std::copy (in[(size_t) c].begin() + pos, in[(size_t) c].begin() + pos + 512, buf.getWritePointer (c));
            p2.processBlock (buf, midi);
            e2.tickForSnapshot();
            const auto r = p2.getReadouts();
            const float want = std::min (0.0f, r.duckDb), got = e2.shownDuckDb();
            worst = std::max (worst, std::abs (got - want));
            if (got != want) ++mismatches;
            if (got < -3.0f) ++deep;
            if (got > -0.5f && deep > 0) ++back;
            deepest = std::min (deepest, got);
            if (r.throwOn) { ++throwSeen; if (e2.throwLampLit()) ++throwLampSeen; }
        }
        check (mismatches == 0 && deep > 20 && back > 20 && deepest < -6.0f && throwSeen > 0 && throwLampSeen == throwSeen,
               "movement display: the analog duck bar shows the engine's duckDb on all " + juce::String (blocks) + " blocks (largest difference " + juce::String (worst, 6) + " dB), deepest " + juce::String (deepest, 1) + " dB, in the duck " + juce::String (deep) + " blocks, back to 0 dB " + juce::String (back) + " blocks; the throw lamp lit on " + juce::String (throwLampSeen) + " of " + juce::String (throwSeen) + " throw blocks");
        setParam (p2.apvts, "listen", 0.0f);
        for (int i = 0; i < 40; ++i) { p2.processBlock (buf, midi); e2.tickForSnapshot(); }
        check (e2.shownDuckDb() == 0.0f && ! e2.throwLampLit(), "movement display: LISTEN off -> 0 dB and no throw lamp");
    }
    {   // 5b. the ECHO needle follows the engine's wet level, the red needle follows the engine's duck (a second, identical processor is the reference)
        SedaAudioProcessor pa, pb;
        for (auto* q : { &pa, &pb }) { q->setRateAndBufferSizeDetails (sr, 512); q->prepareToPlay (sr, 512); setParam (q->apvts, "listen", 1.0f); q->applyProgram (1); setParam (q->apvts, "feedback", 55.0f); setParam (q->apvts, "mix", 50.0f); }
        SedaEditor ea (pa);
        const auto in = phrases (sr, 12.0, 0.12, 5);
        juce::AudioBuffer<float> ba (2, 512), bb (2, 512);
        juce::MidiBuffer midi;
        int blocks = 0, wetMismatch = 0, duckMismatch = 0, moved = 0; float worstWet = 0.0f, lo = 90.0f, hi = -90.0f, lastTarget = -90.0f;
        for (int pos = 0; pos + 512 <= (int) in[0].size(); pos += 512, ++blocks)
        {
            for (int c = 0; c < 2; ++c) { std::copy (in[(size_t) c].begin() + pos, in[(size_t) c].begin() + pos + 512, ba.getWritePointer (c)); std::copy (in[(size_t) c].begin() + pos, in[(size_t) c].begin() + pos + 512, bb.getWritePointer (c)); }
            pa.processBlock (ba, midi); pb.processBlock (bb, midi);
            const float pk = pb.takeWetPeak(), refDb = pk > 1.0e-5f ? 20.0f * std::log10 (pk) : -90.0f;
            ea.tickForSnapshot();
            worstWet = std::max (worstWet, std::abs (ea.echoTargetDb() - refDb));
            if (std::abs (ea.echoTargetDb() - refDb) > 1.0e-4f) ++wetMismatch;
            if (ea.shownDuckDb() != std::min (0.0f, pb.getReadouts().duckDb)) ++duckMismatch;
            if (ea.shownEchoDb() > -36.0f) { lo = std::min (lo, ea.shownEchoDb()); hi = std::max (hi, ea.shownEchoDb()); }
            if (std::abs (ea.echoTargetDb() - lastTarget) > 3.0f) ++moved;
            lastTarget = ea.echoTargetDb();
        }
        for (int i = 0; i < 12; ++i) { ba.clear(); pa.processBlock (ba, midi); }   // silence: the needle falls
        for (int i = 0; i < 40; ++i) ea.tickForSnapshot();
        check (wetMismatch == 0 && duckMismatch == 0 && hi - lo > 6.0f && moved > 5 && ea.shownEchoDb() < -35.0f,
               "ECHO needle: its target equals the engine's wet peak after the duck on all " + juce::String (blocks) + " blocks (largest difference " + juce::String (worstWet, 6) + " dB), the analog duck bar equals duckDb on all of them; the needle moved over " + juce::String (hi - lo, 1) + " dB (" + juce::String (lo, 1) + " .. " + juce::String (hi, 1) + " dB) and falls back to " + juce::String (ea.shownEchoDb(), 1) + " dB in silence");
    }
    {   // 6. SIZE: 75 / 85 / 100 % is kept in the state
        const bool def75 = SedaAudioProcessor().editorScalePct.load() == 75;
        ed.setScalePercent (60);
        juce::MemoryBlock mb; proc.getStateInformation (mb);
        SedaAudioProcessor p3; p3.setStateInformation (mb.getData(), (int) mb.getSize());
        const bool ok60 = p3.editorScalePct.load() == 60;
        ed.setScalePercent (100);
        const bool ok100 = proc.editorScalePct.load() == 100 && ed.getWidth() == SedaEditor::kW && ed.getHeight() == SedaEditor::kH;
        ed.setScalePercent (75);
        check (def75 && ok60 && ok100 && proc.editorScalePct.load() == 75, "SIZE chip: default 75 % (1200 x 480 on screen), 60 % is saved in the state and comes back, 100 % keeps the 1600 x 640 design size");
    }
    {   // 7. the RUNAWAY LED follows the engine's loop gain (> 1.0): off at the defaults, on with RUNAWAY 100 %, on with FEEDBACK 110 %, off again when RUNAWAY is back at 0
        SedaAudioProcessor p4; p4.setRateAndBufferSizeDetails (sr, 512); p4.prepareToPlay (sr, 512);
        SedaEditor e4 (p4);
        juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
        auto blocks = [&] (int k) { for (int i = 0; i < k; ++i) { buf.clear(); p4.processBlock (buf, midi); } e4.tickForSnapshot(); };
        blocks (10); const bool offDef = ! e4.runawayLampLit();
        setParam (p4.apvts, "runaway", 100.0f); blocks (10); const bool onRun = e4.runawayLampLit();
        setParam (p4.apvts, "runaway", 0.0f); blocks (10); const bool offAgain = ! e4.runawayLampLit();
        setParam (p4.apvts, "feedback", 110.0f); blocks (10); const bool onFb = e4.runawayLampLit();
        check (offDef && onRun && offAgain && onFb, "RUNAWAY LED: off at the defaults, lit at RUNAWAY 100 %, off at RUNAWAY 0 again, lit at FEEDBACK 110 %");
        auto* k = e4.knob ("runaway");
        check (k != nullptr && e4.knob ("chaos") != nullptr && e4.knobLabelText ("runaway") == "RUNAWAY" && e4.knobLabelText ("chaos") == "CHAOS", "RUNAWAY and CHAOS knobs on the panel, labelled RUNAWAY and CHAOS");
    }
}

// ---- the shared harness (SvlControlProof.h), through the real processor ----------------------------------------------------------------
static int runControlProof (int argc, char** argv)
{
    namespace cp = svl::controlproof;
    using cp::feat::wetRmsDb; using cp::feat::devFromMinDb; using cp::feat::wetBandDb; using cp::feat::wetCentroidHz; using cp::feat::stereoWidthDb;
    cp::Config c;
    c.name = "SEDA";
    c.make = [] { return std::make_unique<SedaAudioProcessor>(); };
    c.scBus = -1;
    c.tailSeconds = 8.0;
    static SedaAudioProcessor ref;   // to turn a plain value into the normalised one the harness forces
    auto N = [] (const char* id, float plain) { auto* p = ref.apvts.getParameter (id); return p->convertTo0to1 (plain); };
    c.skip = {};
    // "sustains by design": a state whose tail (66 dB, plus margin for the -90 dBFS ceiling) is longer than the harness waits, or that has FEEDBACK over 100 %.
    // P3b: no regrowth excuse any more (the 'FEEDBACK x (1 + TONE) over 45 %' clause is gone): the tail test covers the whole parameter range.
    c.selfSustains = [] (const cp::StateView& s)
    {
        double fb = s.raw ("feedback") * 0.01 + (s.on ("listen") ? 0.15 * s.raw ("throw") / 12.0 : 0.0);
        if (const double r = juce::jlimit (0.0, 1.0, s.raw ("runaway") * 0.01); r > 0.0) fb += std::max (0.0, seda::perform::kRunMaxGain - fb) * r * r * (3.0 - 2.0 * r);   // RUNAWAY's loop gain
        if (fb >= 0.99) return true;
        const double timeSec = std::max (s.raw ("time") * 0.001, s.on ("sync") || s.on ("listen") ? 3.0 : 0.0);   // SYNC / LISTEN note values may be longer than the TIME knob: be generous
        const double repeats = fb < 1e-3 ? 0.0 : std::ceil (84.0 / std::max (0.01, -20.0 * std::log10 (fb)));
        return (repeats + 1.0) * timeSec > 7.0;
    };
    // BBD line noise (the clock-noise floor of the model): quiet with the compander, audible without it. Peak of a silent input, by design.
    c.noiseFloorDb = [] (const cp::StateView& s)
    {
        // RUNAWAY (17 Oct 2026): over a loop gain of 1 the loop starts from its own hiss by design and settles at the servo level (about -14 dB RMS, peak under the -3 dBFS wet ceiling)
        const double r = juce::jlimit (0.0, 1.0, s.raw ("runaway") * 0.01), fb = s.raw ("feedback") * 0.01;
        if (r > 0.0 && fb + std::max (0.0, seda::perform::kRunMaxGain - fb) * r * r * (3.0 - 2.0 * r) > 0.99) return -3.0;
        return (s.raw ("comp") < 99.0 ? -55.0 : -75.0) + (s.choice ("age") == 1 ? 8.0 : 0.0) + (s.raw ("feedback") > 90.0 || r > 0.5 ? 15.0 : 0.0) + 20.0 * std::log10 (1.0 + seda::perform::kRunNoise * r);
    };
    c.latencyForce = {};
    using Force = std::vector<std::pair<juce::String, float>>;
    auto D = [] (const char* id, int dir, cp::Feature f, double tol, cp::Cond ap, const char* fname, const char* note, Force force = {}, int mat = cp::kDrums)
    {
        cp::Direction d; d.id = id; d.dir = dir; d.feature = f; d.tol = tol; d.applies = ap; d.featureName = fname; d.note = note; d.material = mat; d.force = std::move (force);
        return d;
    };
    c.notes["voice"] = "BBD only; DIGITAL and CLEAN are reserved values that play as BBD (no change of the sound by design, plugin_check proves the output identical)";
    c.notes["time"] = "moves the echo in time: not an ordered scale for level or colour; the delay in ms is measured per sample rate by plugin_check (0.5 %)";
    c.notes["note"] = "note values; the delay time they give is measured by plugin_check (SYNC 1/4d at 120 bpm = 750 ms)";
    c.notes["noteAuto"] = "acts at a bar line (2 s at 120 bpm) after the density of the source is read; shown by plugin_check's audibility table on a pluck train (the harness renders 1 s)";
    c.notes["rate"] = "an LFO speed, not an ordered scale for level or colour; with DEPTH it changes the render (plugin_check audibility table)";
    c.notes["shape"] = "SINE / TRIANGLE: two characters, not ordered";
    c.notes["rate"] = "LFO speed: not ordered; audible with DEPTH over 0 (plugin_check audibility table)";
    c.notes["bypass"] = "BYPASS 1 = the input, bit-exact (plugin_check)";
    c.notes["runaway"] = "pushes the loop gain towards 1.2 over FEEDBACK; from about 70 % (FEEDBACK 30 %) the loop self-oscillates and the level servo holds it (plugin_check: level, onset from silence, 44.1 / 48 / 96 kHz)";
    c.notes["chaos"] = "random clock wobble (0-50 %) and random pitch steps on beats (50-100 %): random by design (fixed seed), not an ordered scale; plugin_check proves the steps land on the host's beats";
    c.directions = {
        D ("voice", 0, nullptr, 0, nullptr, "", "reserved values, same sound by design"),
        D ("time", 0, nullptr, 0, nullptr, "", "delay time: measured by plugin_check"),
        D ("sync", 0, nullptr, 0, nullptr, "", "switch: tempo or TIME"),
        D ("note", 0, nullptr, 0, nullptr, "", "note values, not a level scale"),
        D ("noteAuto", 0, nullptr, 0, nullptr, "", "acts at bar lines"),
        D ("feedback", 0, nullptr, 0, nullptr, "", "orders the repeats up to 100 % (plugin_check, FEEDBACK 0 / 20 / 40 / 60 / 80 %); above it the engine's level servo holds the loop level, so the 110 % end is lower than 100 %: no single direction over the whole travel"),
        D ("mix", +1, wetRmsDb(), 0.25, nullptr, "wet part = output - input (rms dB)", "0 is the dry input exactly", { { "sync", 0.0f }, { "time", N ("time", 120.0f) } }),
        D ("age", 0, nullptr, 0, nullptr, "", "NEW / OLD: two lines, not ordered"),
        D ("comp", +1, devFromMinDb(), 0.25, nullptr, "distance from the COMPANDER 0 render (rms dB)", "the compander's colour grows with the knob", { { "sync", 0.0f }, { "time", N ("time", 120.0f) }, { "mix", N ("mix", 100.0f) } }),
        D ("tone", 0, nullptr, 0, nullptr, "", "feedback tilt: the colour of the repeats; audible (plugin_check audibility table), not ordered in one level feature"),
        D ("rate", 0, nullptr, 0, nullptr, "", "LFO speed"),
        D ("depth", +1, devFromMinDb(), 0.5, nullptr, "distance from the DEPTH 0 render (rms dB)", "more modulation, further from the unmodulated render (tolerance 0.5 dB: once the delay swing passes the period of the programme the difference saturates)", { { "sync", 0.0f }, { "time", N ("time", 150.0f) }, { "mix", N ("mix", 100.0f) }, { "rate", N ("rate", 3.0f) }, { "feedback", N ("feedback", 0.0f) } }),
        D ("shape", 0, nullptr, 0, nullptr, "", "two LFO shapes"),
        D ("stereo", 0, nullptr, 0, nullptr, "", "MONO / WIDE / PING-PONG: three behaviours"),
        D ("width", 0, nullptr, 0, nullptr, "", "right side delayed by WIDTH 0-20 ms: the difference from the WIDTH 0 render is comb-shaped (it rises, then falls and rises again as the shift passes the programme's own repeat length), so no ordered feature; audible in plugin_check's table"),
        D ("listen", 0, nullptr, 0, nullptr, "", "switch: the interactive layer"),
        D ("source", 0, nullptr, 0, nullptr, "", "starting points per source, not ordered"),
        D ("duck", +1, devFromMinDb(), 0.25, nullptr, "distance from the DUCK 0 render (rms dB)", "deeper duck of the repeats while the source plays", { { "sync", 0.0f }, { "listen", 1.0f }, { "mix", N ("mix", 100.0f) }, { "time", N ("time", 150.0f) } }),
        D ("throw", 0, nullptr, 0, nullptr, "", "acts at a phrase end (P3e: >= 1.2 s of material, then a gap of max(250 ms, half the repeat time)): audible in plugin_check's audibility table on phrases, not on the harness's 1 s drums"),
        D ("wetHp", -1, wetBandDb (40.0, 200.0), 0.25, nullptr, "wet band power 40-200 Hz (dB)", "the high-pass takes the lows out of the repeats", { { "sync", 0.0f }, { "listen", 1.0f }, { "mix", N ("mix", 100.0f) }, { "time", N ("time", 150.0f) } }, cp::kPink),
        D ("bypass", -1, wetRmsDb(), 0.25, nullptr, "wet part = output - input (rms dB)", "BYPASS 1 is the input exactly", { { "sync", 0.0f }, { "time", N ("time", 120.0f) } }),
        D ("runaway", 0, nullptr, 0, nullptr, "", "loop gain over FEEDBACK: the repeats bloom, then the loop self-oscillates and the level servo holds it at a fixed level (like FEEDBACK, no single direction over the whole travel)"),
        D ("chaos", 0, nullptr, 0, nullptr, "", "random clock wobble and random pitch steps: not ordered by design"),
    };
    return cp::main (c, argc, argv);
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    for (int i = 1; i < argc; ++i)
        if (juce::String (argv[i]).startsWith ("--controlproof")) return runControlProof (argc, argv);

    testParameters();
    testAgainstRaw();
    testMonoBusMode();
    testSelfOscillation();
    testPerform();
    testState();
    testDryPath();
    testBypassAndMix0();
    testBlockSizes();
    testSampleRates();
    testTail();
    regrowthInfo();
    testFeedbackOrder();
    testResetAndHostBars();
    testAudibility();
    testEditor();
    testSignature();

    {   // Cmd+Z / Cmd+Shift+Z (SvlUndoTests.h): SEDA has no presets, so the "recall" is a Group around several writes, as a SOURCE change would do.
        const auto u = svl::undotest::run<SedaAudioProcessor> ("SEDA", [] { return std::make_unique<SedaAudioProcessor>(); },
                                                               { { "time", 412.5f }, { "feedback", 66.0f }, { "tone", -30.0f }, { "runaway", 40.0f }, { "chaos", 62.5f } },
                                                               [] (SedaAudioProcessor& p)
                                                               {
                                                                   svl::ParamUndo::Group oneStep (p.undo, "Test recall");
                                                                   setParam (p.apvts, "listen", 1.0f); setParam (p.apvts, "source", 2.0f);
                                                                   setParam (p.apvts, "duck", 33.0f); setParam (p.apvts, "wetHp", 400.0f);
                                                               });
        gPass += u.pass;
        gFail += u.fail;
        check (u.fail == 0, "undo_redo: " + juce::String (u.pass) + " of " + juce::String (u.pass + u.fail) + " undo checks passed");
    }

    std::printf ("\n--- control proof (SvlControlProof.h, --quick) ---\n");
    std::fflush (stdout);
    char arg0[] = "plugin_check", a1[] = "--quick", a2[] = "--no-docs";
    char* cpArgs[] = { arg0, a1, a2 };
    const int cpRc = runControlProof (3, cpArgs);
    check (cpRc == 0, "ControlProof (the shared harness, --quick, every parameter through the real processor)", "exit code " + juce::String (cpRc));

    std::printf ("\nplugin_check: %d passed, %d failed\n", gPass, gFail);
    return gFail == 0 ? 0 : 1;
}
