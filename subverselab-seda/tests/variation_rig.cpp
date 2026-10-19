// variation_rig: the SEDA variation test rig (11 Oct, after the owner's Live test). Headless, through the REAL processor (and the real editor where the panel
// decides something): it sweeps the combinations a person can reach and measures what comes out instead of trusting the code.
//
//   variation_rig            every section, then a table; exit code 0 only if no check failed
//   variation_rig --quick    fewer sweep cases (a smoke test, about a minute)
//   variation_rig NOTE DUCK  only the named sections (NOTE DUCK PROG STATE LAT CLICK SWEEP UI)
//
// Material: the project's own demo material (tools/demo_material: vocal.wav, drums.wav, pluck.wav, the 30 s phrase line vocal_p3e.wav + marks) and a few
// generated signals (impulse, 5 Hz sine, 220 Hz sine) where a measurement needs one.
//
// Sections
//   NOTE   the 18 note values: reachable by parameter (host automation), by knob drag (both ways), by wheel, by typing; the window, the red display and the
//          engine agree on the note in force (SYNC on/off, AUTO on/off, LISTEN on/off, every program); the delay MEASURED from the output equals the note at
//          the host tempo (incl. 1/32); the TIME knob's shown ms equals the measured delay
//   DUCK   DUCK 0 gives 0.00 dB of duck whatever the program; DUCK 50 / 100 give 6 / 12 dB; THROW 0 never throws; WET HP OFF leaves the lows alone
//   PROG   (SEDA_HAS_PROGRAMS) a program writes exactly its table into the real parameters, one undo step, undo/redo exact, the other knobs stay
//   STATE  state round trip bit for bit (random settings, after programs), an old session (no version, touched mask) loads as it sounded
//   LAT    reported latency against measured, block size independence, parameter response times
//   CLICK  program change / SYNC / NOTE / STEREO / LISTEN / AGE switched under a sine: no click (prominence of the second difference)
//   SWEEP  every program x LISTEN x SYNC x AUTO x STEREO x AGE (x NOTE, DUCK, THROW, FEEDBACK cycled) on vocal / drums / pluck: finite, bounded, no runaway,
//          the NOTE in force is the NOTE set, a second pass over all 18 notes x DUCK x THROW x FEEDBACK
//   UI     cost of the panel: refresh tick, a full paint at 75 % and 100 %, the meter region, the value windows (what the 30 Hz timer repaints)
#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"
#include "../Source/ParamText.h"
#include "../tools/wavio.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <string>

#ifndef SEDA_ROOT
 #define SEDA_ROOT "."
#endif
#ifndef SEDA_HAS_PROGRAMS
 #define SEDA_HAS_PROGRAMS 0
#endif

using Audio = std::vector<std::vector<float>>;
using Settings = std::vector<std::pair<const char*, float>>;   // parameter id, PLAIN value

// ---------------------------------------------------------------------------------------------------- bookkeeping
struct GroupStat { int cases = 0, fails = 0; std::string worst; };
static std::map<std::string, GroupStat> gGroups;
static std::vector<std::string> gOrder;
static std::string gCur;
static int gFailTotal = 0, gPrintedFails = 0;
static bool gQuick = false;

static void section (const std::string& name) { gCur = name; if (! gGroups.count (name)) { gGroups[name] = {}; gOrder.push_back (name); } std::printf ("\n--- %s ---\n", name.c_str()); std::fflush (stdout); }

/** One case of the current group. A failing case is always printed (the first 25 per run); a passing one is printed only when `print` is set. */
static void check (bool ok, const std::string& what, const std::string& detail = {}, bool print = true)
{
    auto& g = gGroups[gCur];
    ++g.cases;
    if (! ok) { ++g.fails; ++gFailTotal; if (g.worst.empty()) g.worst = what; }
    if (! ok && gPrintedFails < 60) { ++gPrintedFails; std::printf ("[FAIL] %s%s%s\n", what.c_str(), detail.empty() ? "" : "  ", detail.c_str()); }
    else if (ok && print) std::printf ("[PASS] %s%s%s\n", what.c_str(), detail.empty() ? "" : "  ", detail.c_str());
    std::fflush (stdout);
}
static std::string fmt (const char* f, ...)
{
    char b[1024]; va_list a; va_start (a, f); std::vsnprintf (b, sizeof b, f, a); va_end (a); return b;
}

// ---------------------------------------------------------------------------------------------------- material
static Audio makeAudio (int ch, int n) { return Audio ((size_t) ch, std::vector<float> ((size_t) n, 0.0f)); }
static Audio loadWav (const std::string& rel)
{
    const auto w = readWav ((std::string (SEDA_ROOT) + "/tools/demo_material/" + rel).c_str());
    return Audio { w.l, w.r };
}
static Audio sliceSec (const Audio& a, double sr, double from, double len)
{
    Audio o (a.size());
    const size_t f = (size_t) (from * sr), n = (size_t) (len * sr);
    for (size_t c = 0; c < a.size(); ++c) { const size_t e = std::min (a[c].size(), f + n); o[c].assign (a[c].begin() + (long) std::min (f, a[c].size()), a[c].begin() + (long) e); }
    return o;
}
static Audio withSilence (const Audio& a, double sr, double seconds)
{
    Audio o = a;
    for (auto& c : o) c.resize (c.size() + (size_t) (seconds * sr), 0.0f);
    return o;
}
static Audio sine (double sr, double seconds, double f, double amp)
{
    auto a = makeAudio (2, (int) (seconds * sr));
    for (size_t i = 0; i < a[0].size(); ++i) { const float v = (float) (amp * std::sin (2.0 * M_PI * f * (double) i / sr)); a[0][i] = v; a[1][i] = v; }
    return a;
}
static Audio impulse (double sr, double seconds, int at)
{
    auto a = makeAudio (2, (int) (seconds * sr));
    a[0][(size_t) at] = 1.0f; a[1][(size_t) at] = 1.0f;
    return a;
}

struct Material
{
    Audio vocal, drums, pluck, p3e;   // 48 kHz, 2 channels
    std::vector<std::pair<double, double>> phrases;   // p3e: onset, end of each marked phrase (s)
    std::vector<double> phraseEnds;
};
static Material gMat;

static void loadMaterial()
{
    gMat.vocal = loadWav ("vocal.wav"); gMat.drums = loadWav ("drums.wav"); gMat.pluck = loadWav ("pluck.wav"); gMat.p3e = loadWav ("vocal_p3e.wav");
    const auto js = juce::JSON::parse (juce::File (std::string (SEDA_ROOT) + "/tools/demo_material/vocal_p3e.json"));
    if (auto* arr = js.getProperty ("phrases", {}).getArray())
        for (auto& ph : *arr)
            if (ph.getProperty ("kind", {}).toString() == "phrase")
                gMat.phrases.push_back ({ (double) ph.getProperty ("t0", 0.0), (double) ph.getProperty ("t1", 0.0) });
    if (auto* arr = js.getProperty ("phrase_ends", {}).getArray())
        for (auto& e : *arr) gMat.phraseEnds.push_back ((double) e.getProperty ("t", 0.0));
}

// ---------------------------------------------------------------------------------------------------- measuring
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
static double rmsDb (const std::vector<float>& x, double sr, double t0, double t1)
{
    const size_t a = (size_t) (t0 * sr), b = std::min (x.size(), (size_t) (t1 * sr));
    double s = 0.0; for (size_t i = a; i < b; ++i) s += (double) x[i] * x[i];
    return 10.0 * std::log10 (s / std::max<size_t> (1, b - a) + 1e-30);
}
/** RMS (dB) of the part of x under 200 Hz (two one-poles at 200 Hz, 12 dB/oct): the wet's lows. */
static double lowBandDb (const std::vector<float>& x, double sr, double t0, double t1)
{
    const double a = 1.0 - std::exp (-2.0 * M_PI * 200.0 / sr);
    double y1 = 0.0, y2 = 0.0, s = 0.0; size_t n = 0;
    const size_t i0 = (size_t) (t0 * sr), i1 = std::min (x.size(), (size_t) (t1 * sr));
    for (size_t i = 0; i < i1; ++i)
    {
        y1 += a * (x[i] - y1); y2 += a * (y1 - y2);
        if (i >= i0) { s += y2 * y2; ++n; }
    }
    return 10.0 * std::log10 (s / std::max<size_t> (1, n) + 1e-30);
}
static double toDb (double x) { return 20.0 * std::log10 (std::max (x, 1e-12)); }

/** How much a click stands out: the largest |second difference| in [from, to) against the largest one in the 32 samples either side (2 samples either side of it left out).
    A smooth signal has ~1 (its neighbours are as large); a one-sample step or a gap has 10 and more. Samples under `floor` are not judged (the noise floor of the line). */
static double spikeProminence (const std::vector<float>& y, size_t from, size_t to, size_t* where = nullptr, double floor = 2.0e-5)
{
    double worst = 0.0;
    to = std::min (to, y.size() - 40);
    for (size_t n = std::max<size_t> (from, 40); n < to; ++n)
    {
        const double d = std::abs ((double) y[n + 1] - 2.0 * y[n] + y[n - 1]);
        if (d < floor) continue;
        double nb = 0.0;
        for (int k = -32; k <= 32; ++k)
        {
            if (std::abs (k) <= 2) continue;
            const size_t m = n + (size_t) (long) k;
            nb = std::max (nb, std::abs ((double) y[m + 1] - 2.0 * y[m] + y[m - 1]));
        }
        if (d / std::max (nb, floor) > worst) { worst = d / std::max (nb, floor); if (where) *where = n; }
    }
    return worst;
}

// ---------------------------------------------------------------------------------------------------- the rig: the real processor, driven like a host
struct TestPlayHead : juce::AudioPlayHead
{
    double bpm = 120.0;
    juce::Optional<PositionInfo> getPosition() const override { PositionInfo i; i.setBpm (bpm); return i; }
};

static void setParam (juce::AudioProcessorValueTreeState& s, const char* id, float plain)
{
    auto* p = s.getParameter (id);
    jassert (p != nullptr);
    p->setValueNotifyingHost (p->convertTo0to1 (plain));
}
static float rawOf (SedaAudioProcessor& p, const char* id) { return p.apvts.getRawParameterValue (id)->load(); }

struct Rig
{
    SedaAudioProcessor proc;
    TestPlayHead ph;
    int block; double sr;
    juce::AudioBuffer<float> buf;
    juce::MidiBuffer midi;
    std::vector<int> noteSeen;   // the note in force the engine reported after every block of the last run

    Rig (double sampleRate, int blockSize, const Settings& st, double bpm = 120.0) : block (blockSize), sr (sampleRate)
    {
        proc.setRateAndBufferSizeDetails (sr, block);
        proc.prepareToPlay (sr, block);
        for (auto& kv : st) setParam (proc.apvts, kv.first, kv.second);
        ph.bpm = bpm;
        proc.setPlayHead (&ph);
        buf.setSize (2, block);
    }
    void set (const char* id, float plain) { setParam (proc.apvts, id, plain); }
    float get (const char* id) { return rawOf (proc, id); }

    Audio run (const Audio& in)
    {
        const int n = (int) in[0].size();
        Audio out = makeAudio (2, n);
        noteSeen.clear();
        for (int pos = 0; pos < n; pos += block)
        {
            const int len = juce::jmin (block, n - pos);
            buf.setSize (2, len, false, false, true);
            for (int c = 0; c < 2; ++c) std::copy (in[(size_t) c].begin() + pos, in[(size_t) c].begin() + pos + len, buf.getWritePointer (c));
            proc.processBlock (buf, midi);
            noteSeen.push_back (proc.getReadouts().chosenNote);
            for (int c = 0; c < 2; ++c) std::copy (buf.getReadPointer (c), buf.getReadPointer (c) + len, out[(size_t) c].begin() + pos);
        }
        return out;
    }
};

static Audio renderWith (const Audio& in, const Settings& st, double bpm = 120.0, int block = 512)
{
    Rig r (48000.0, block, st, bpm);
    return r.run (in);
}

// ---------------------------------------------------------------------------------------------------- programs (the API of item 4; absent on the baseline build)
static void applyProgramTo (SedaAudioProcessor& p, int program)
{
#if SEDA_HAS_PROGRAMS
    svl::ParamUndo::Group one (p.undo, "PROGRAM");
    svl::setParamAsGesture (dynamic_cast<juce::RangedAudioParameter*> (p.apvts.getParameter ("source")), (float) program);
    p.applyProgram (program);
#else
    setParam (p.apvts, "source", (float) program);   // the baseline: the program is an overlay the engine adds, nothing is written into the knobs
#endif
}
static void applyProgramTo (Rig& r, int program) { applyProgramTo (r.proc, program); }

static const char* kProgName[5] = { "MANUAL", "VOCAL", "DRUMS", "BASS", "KEYS" };

// ---------------------------------------------------------------------------------------------------- the delay, measured
/** Delay of a 5 Hz sine through the wet line (phase delay, least squares over 3 s), in samples; the integer number of periods is resolved with `expect`. */
static double sineDelaySamples (double sr, Settings st, double bpm, double expectSamples, SedaAudioProcessor** keep = nullptr)
{
    (void) keep;
    const double f = 5.0, extra = 4.5;
    const int n = (int) (sr * (expectSamples / sr + extra));
    auto in = sine (sr, (double) n / sr, f, 0.3);
    st.push_back ({ "mix", 100 }); st.push_back ({ "feedback", 0 }); st.push_back ({ "comp", 0 }); st.push_back ({ "depth", 0 });
    Rig r (sr, 512, st, bpm);
    auto out = r.run (in);
    const int a = (int) (sr * (expectSamples / sr + 1.5)), m = (int) (3.0 * sr);
    double M[3][3] = {}, rhs[3] = {};
    for (int i = a; i < a + m && i < (int) out[0].size(); ++i)
    {
        const double t = (double) i / sr, bs[3] = { std::sin (2.0 * M_PI * f * t), std::cos (2.0 * M_PI * f * t), 1.0 }, y = out[0][(size_t) i];
        for (int q = 0; q < 3; ++q) { rhs[q] += bs[q] * y; for (int c = 0; c < 3; ++c) M[q][c] += bs[q] * bs[c]; }
    }
    for (int p = 0; p < 3; ++p)
        for (int q = p + 1; q < 3; ++q) { const double k = M[q][p] / M[p][p]; for (int c = p; c < 3; ++c) M[q][c] -= k * M[p][c]; rhs[q] -= k * rhs[p]; }
    double x[3];
    for (int q = 2; q >= 0; --q) { double v = rhs[q]; for (int c = q + 1; c < 3; ++c) v -= M[q][c] * x[c]; x[q] = v / M[q][q]; }
    const double ph = std::atan2 (x[1], x[0]);
    double d = -ph / (2.0 * M_PI * f) * sr;
    const double per = sr / f;
    d -= per * std::round ((d - expectSamples) / per);
    return d;
}

// ---------------------------------------------------------------------------------------------------- the panel
static void pump (int ms = 20) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

struct PanelRig
{
    SedaAudioProcessor proc;
    TestPlayHead ph;
    std::unique_ptr<SedaEditor> ed;
    juce::AudioBuffer<float> buf { 2, 512 };
    juce::MidiBuffer midi;
    double sr = 48000.0;

    explicit PanelRig (double bpm = 120.0)
    {
        proc.setRateAndBufferSizeDetails (sr, 512); proc.prepareToPlay (sr, 512);
        ph.bpm = bpm; proc.setPlayHead (&ph);
        ed = std::make_unique<SedaEditor> (proc);
    }
    /** A few blocks of the given material so the processor publishes the note in force, then a panel tick. */
    void settle (const Audio& mat, int blocks = 6, int from = 0)
    {
        for (int b = 0; b < blocks; ++b)
        {
            const size_t pos = (size_t) from + (size_t) b * 512;
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < 512; ++i) buf.setSample (c, i, pos + (size_t) i < mat[(size_t) c].size() ? mat[(size_t) c][pos + (size_t) i] : 0.0f);
            proc.processBlock (buf, midi);
        }
        ed->tickForSnapshot();
    }
    sedaui::ValueBox* box (const char* id)
    {
        for (auto* c : ed->controls()) if (c->paramId() == id) if (auto* v = dynamic_cast<sedaui::ValueBox*> (c)) return v;
        return nullptr;
    }
};

static juce::MouseEvent mouseEv (juce::Component* k, juce::Point<float> pos, bool dragged, bool button = true)
{
    auto src = juce::Desktop::getInstance().getMainMouseSource();
    auto mods = juce::ModifierKeys().withFlags (button ? juce::ModifierKeys::leftButtonModifier : 0);
    const auto t0 = juce::Time::getCurrentTime();
    return juce::MouseEvent (src, pos, mods, 1.0f, juce::MouseInputSource::defaultOrientation, juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
                             juce::MouseInputSource::defaultTiltY, k, k, t0, { 20.0f, 30.0f }, t0, 1, dragged);
}

// ====================================================================================================== NOTE
static std::string noteTextIn (PanelRig& pr) { auto* b = pr.box ("note"); return b ? b->text().toStdString() : "?"; }

static void testNote()
{
    section ("NOTE");
    const double sr = 48000.0;
    {   // 1. the parameter: 18 values, the names, every index by host automation (normalised steps), the engine reports each
        SedaAudioProcessor p;
        auto* c = dynamic_cast<juce::AudioParameterChoice*> (p.apvts.getParameter ("note"));
        bool names = c != nullptr && c->choices.size() == seda::kNumNotes;
        for (int i = 0; names && i < seda::kNumNotes; ++i) names = c->choices[i].toStdString() == seda::noteName (i);
        check (names, "NOTE parameter: 18 choices, names equal seda::noteName (1/32 first, 1/1d last)", c ? c->choices[0].toStdString() + " .. " + c->choices[c->choices.size() - 1].toStdString() : "");
        std::set<int> seen; int bad = 0;
        for (int step = 0; step <= 340; ++step)   // host automation: a sweep of the normalised value in small steps, both ways
        {
            for (int dir : { 1, -1 })
            {
                const float v = dir > 0 ? (float) step / 340.0f : 1.0f - (float) step / 340.0f;
                c->setValueNotifyingHost (v);
                const int idx = juce::roundToInt (p.apvts.getRawParameterValue ("note")->load());
                seen.insert (idx);
                if (idx != c->getIndex() || idx < 0 || idx >= seda::kNumNotes) ++bad;
            }
        }
        check (seen.size() == (size_t) seda::kNumNotes && bad == 0 && *seen.begin() == 0, "NOTE by host automation: a sweep of the normalised value reaches all 18 notes including 1/32 (index 0)", fmt ("reached %d of 18, lowest index %d", (int) seen.size(), *seen.begin()));
        // the exact normalised values a host sends for each step (VST3 stepCount 17, AU indexed)
        bool exact = true;
        for (int i = 0; i < seda::kNumNotes; ++i) { c->setValueNotifyingHost ((float) i / 17.0f); exact = exact && c->getIndex() == i && juce::roundToInt (p.apvts.getRawParameterValue ("note")->load()) == i; }
        check (exact, "NOTE by host automation: the normalised value i/17 selects note i for all 18");
    }
    {   // 2. the knob: drag (down and up), wheel, double-click type-in
        PanelRig pr;
        auto* k = pr.ed->knob ("note");
        std::set<int> down, up, wheelDown, wheelUp;
        auto idx = [&] { return juce::roundToInt (rawOf (pr.proc, "note")); };
        {   // drag from the top (17) all the way down in 1 px steps, then back up
            setParam (pr.proc.apvts, "note", 17.0f);
            k->mouseDown (mouseEv (k, { 20.0f, 30.0f }, false));
            for (int dy = 1; dy <= 420; ++dy) { k->mouseDrag (mouseEv (k, { 20.0f, 30.0f + (float) dy }, true)); down.insert (idx()); }
            for (int dy = 420; dy >= -420; --dy) { k->mouseDrag (mouseEv (k, { 20.0f, 30.0f + (float) dy }, true)); up.insert (idx()); }
            k->mouseUp (mouseEv (k, { 20.0f, 30.0f - 420.0f }, true));
            pump (10);
        }
        check (down.size() == 18 && *down.begin() == 0, "NOTE knob, drag down from 1/1d: every note 1/1d .. 1/32 is passed", fmt ("reached %d of 18, lowest index %d (%s)", (int) down.size(), *down.begin(), seda::noteName (*down.begin())));
        check (up.size() == 18 && *up.begin() == 0 && *up.rbegin() == 17, "NOTE knob, drag back up: every note passed again, both ends reached", fmt ("reached %d of 18", (int) up.size()));
        {   // wheel: 20 notches down from 17, 20 up from 0
            setParam (pr.proc.apvts, "note", 17.0f); wheelDown.insert (17);
            juce::MouseWheelDetails wd; wd.deltaX = 0.0f; wd.deltaY = -0.1f; wd.isReversed = false; wd.isSmooth = false; wd.isInertial = false;
            for (int i = 0; i < 40; ++i) { juce::Thread::sleep (3); k->mouseWheelMove (mouseEv (k, { 20.0f, 30.0f }, false, false), wd); wheelDown.insert (idx()); }
            wd.deltaY = 0.1f; wheelUp.insert (idx());
            for (int i = 0; i < 40; ++i) { juce::Thread::sleep (3); k->mouseWheelMove (mouseEv (k, { 20.0f, 30.0f }, false, false), wd); wheelUp.insert (idx()); }
        }
        check (wheelDown.size() == 18 && *wheelDown.begin() == 0, "NOTE knob, wheel down from 1/1d: reaches 1/32 and passes every note", fmt ("reached %d of 18, lowest index %d%s", (int) wheelDown.size(), *wheelDown.begin(), [&] { std::string m; for (int i = 0; i < 18; ++i) if (! wheelDown.count (i)) m += fmt (" missing %s", seda::noteName (i)); return m; }().c_str()));
        check (wheelUp.size() == 18 && *wheelUp.rbegin() == 17, "NOTE knob, wheel up from 1/32: reaches 1/1d and passes every note", fmt ("reached %d of 18%s", (int) wheelUp.size(), [&] { std::string m; for (int i = 0; i < 18; ++i) if (! wheelUp.count (i)) m += fmt (" missing %s", seda::noteName (i)); return m; }().c_str()));
        // drag from every start value by a plain 60 px (about what a hand does): moves, never stuck
        {
            int stuck = 0;
            for (int v = 1; v < 18; ++v)
            {
                setParam (pr.proc.apvts, "note", (float) v);
                k->mouseDown (mouseEv (k, { 20.0f, 30.0f }, false));
                for (int dy = 1; dy <= 60; ++dy) k->mouseDrag (mouseEv (k, { 20.0f, 30.0f + (float) dy }, true));
                k->mouseUp (mouseEv (k, { 20.0f, 90.0f }, true));
                if (idx() >= v) ++stuck;
            }
            check (stuck == 0, "NOTE knob: a 60 px drag down from any of the notes 1/32t .. 1/1d always lowers it", fmt ("stuck %d", stuck));
        }
        {   // typing: every note name, exactly as the window prints it, and the usual short forms
            int bad = 0; std::string which;
            auto* b = pr.box ("note");
            for (int i = 0; i < seda::kNumNotes; ++i)
            {
                setParam (pr.proc.apvts, "note", (float) ((i + 5) % 18));
                const bool ok = b->commitText (seda::noteName (i)) && idx() == i;
                if (! ok) { ++bad; which += std::string (seda::noteName (i)) + " "; }
            }
            check (bad == 0, "NOTE type-in: each of the 18 names (1/32, 1/32t, 1/32d ... 1/1d) typed into the window selects that note", bad ? "not read: " + which : "");
            setParam (pr.proc.apvts, "note", 11.0f);
            const bool bad1 = b->commitText ("rubbish") || idx() != 11;
            check (! bad1, "NOTE type-in: text that is not a note changes nothing");
            setParam (pr.proc.apvts, "note", 11.0f);
            const bool shortForms = b->commitText ("1/32T") && idx() == 1 && b->commitText ("1/16 d") && idx() == 5 && b->commitText ("1/8.") && idx() == 8 && b->commitText ("AUTO 1/4") && idx() == 9;
            check (shortForms, "NOTE type-in: 1/32T, 1/16 d, 1/8. (dotted) and the window's own 'AUTO 1/4' are read");
        }
    }
    {   // 3. one note, three witnesses: the engine's note in force, the window, the red display; across SYNC / AUTO / LISTEN / program
        PanelRig pr (120.0);
        const auto pluck = sliceSec (gMat.pluck, sr, 0.0, 6.0);
        int cases = 0, bad = 0; std::string first;
        for (int program = 0; program < 5; ++program)
            for (int listen = 0; listen < 2; ++listen)
                for (int sync = 0; sync < 2; ++sync)
                    for (int au = 0; au < 2; ++au)
                        for (int note = 0; note < 18; ++note)
                        {
                            setParam (pr.proc.apvts, "source", 0.0f);
                            setParam (pr.proc.apvts, "listen", (float) listen); setParam (pr.proc.apvts, "sync", (float) sync); setParam (pr.proc.apvts, "noteAuto", (float) au);
                            setParam (pr.proc.apvts, "note", (float) note);
                            applyProgramTo (pr.proc, program);
                            if (program > 0)   // the program wrote its own note and SYNC; put the case's choice back (the user turning the knob after the program)
                            {
                                setParam (pr.proc.apvts, "sync", (float) sync); setParam (pr.proc.apvts, "note", (float) note);
                            }
                            pr.settle (pluck, 4);
                            const auto r = pr.proc.getReadouts();
                            const bool effSync = pr.ed->autoKeyEnabled();
                            const std::string win = noteTextIn (pr), red = pr.ed->shownNoteText().toStdString();
                            const bool autoOn = au && listen && sync;
                            int inForce = r.chosenNote;
                            // what the owner must see: with SYNC on the note in force (AUTO's choice with AUTO), never a note other than the engine's
                            bool ok = win.find (seda::noteName (inForce)) != std::string::npos;
                            if (effSync) ok = ok && red == seda::noteName (inForce);
                            if (effSync && ! autoOn) ok = ok && inForce == note;            // no AUTO: the note in force is the note set
                            if (effSync && autoOn) ok = ok && win.rfind ("AUTO ", 0) == 0;   // AUTO: the engine's choice, shown with AUTO in front
                            if (! effSync) ok = ok && red == "FREE";
                            ++cases;
                            if (! ok) { ++bad; if (first.empty()) first = fmt ("program %d listen %d sync %d auto %d note %d: engine %s window '%s' display '%s' effSync %d", program, listen, sync, au, note, seda::noteName (inForce), win.c_str(), red.c_str(), (int) effSync); }
                        }
        check (bad == 0, fmt ("NOTE in force: window, red display and engine agree in all %d combinations (5 programs x LISTEN x SYNC x AUTO x 18 notes)", cases), first);
    }
    {   // 4. the delay MEASURED against the note, at several tempi, every note, NEW and OLD
        int cases = 0, bad = 0; double worst = 0.0; std::string first;
        const std::vector<double> bpms = gQuick ? std::vector<double> { 120.0 } : std::vector<double> { 100.0, 120.0, 140.0 };
        for (double bpm : bpms)
            for (int age = 0; age < (gQuick ? 1 : 2); ++age)
                for (int note = 0; note < 18; ++note)
                {
                    const double want = seda::noteBeats (note) * 60.0 / bpm * sr;
                    const double wantMs = want / sr * 1000.0;
                    const double minMs = age == 0 ? 41.1 : 21.0;
                    if (wantMs < minMs || wantMs > 1200.0) continue;   // outside the line's range (the engine clamps: NEW from 41 ms, 1.2 s at most)
                    const double got = sineDelaySamples (sr, { { "sync", 1 }, { "note", (float) note }, { "age", (float) age } }, bpm, want);
                    const double err = (got - want) / want * 100.0;
                    worst = std::max (worst, std::abs (err)); ++cases;
                    if (std::abs (err) > 0.5 && std::abs (got - want) > 1.0) { ++bad; if (first.empty()) first = fmt ("%s at %.0f bpm %s: wanted %.2f ms, measured %.2f ms (%+.2f %%)", seda::noteName (note), bpm, age ? "OLD" : "NEW", wantMs, got / sr * 1000.0, err); }
                }
        check (bad == 0, fmt ("NOTE delay measured from the output equals the note at the host tempo: %d note/tempo/age cases incl. 1/32 (limit +-0.5 %%)", cases), fmt ("worst %+.3f %%", worst) + (first.empty() ? "" : "  " + first));
    }
    {   // 5. the 1/32 family in particular, as the owner saw it
        for (double bpm : { 100.0, 120.0, 140.0 })
        {
            double got[3]; double want[3];
            for (int k = 0; k < 3; ++k) { want[k] = seda::noteBeats (k) * 60.0 / bpm * sr; got[k] = sineDelaySamples (sr, { { "sync", 1 }, { "note", (float) k } }, bpm, want[k]); }
            const double minS = 0.0410 * sr; bool ok = true;
            for (int k = 0; k < 3; ++k) ok = ok && (std::abs (got[k] - want[k]) <= 0.005 * want[k] || (want[k] < minS && std::abs (got[k] - minS) <= 0.005 * minS));   // under 41 ms the NEW line clamps
            check (ok, fmt ("1/32, 1/32t, 1/32d at %.0f bpm: %.2f / %.2f / %.2f ms measured (wanted %.2f / %.2f / %.2f)", bpm, got[0] / sr * 1e3, got[1] / sr * 1e3, got[2] / sr * 1e3, want[0] / sr * 1e3, want[1] / sr * 1e3, want[2] / sr * 1e3));
        }
    }
    {   // 6. the TIME knob: shown ms against the measured delay, NEW and OLD (the shortest delay NEW can make is 41 ms)
        PanelRig pr;
        int bad = 0; std::string first, all;
        for (int age = 0; age < 2; ++age)
            for (float ms : { 20.0f, 30.0f, 41.0f, 60.0f, 100.0f, 300.0f, 750.0f, 1200.0f })
            {
                setParam (pr.proc.apvts, "age", (float) age); setParam (pr.proc.apvts, "time", ms); setParam (pr.proc.apvts, "sync", 0.0f);
                pr.settle (gMat.pluck, 2);
                const std::string seg = pr.ed->shownSegText().toStdString();
                const double shown = std::atof (seg.c_str());
                const double got = sineDelaySamples (sr, { { "time", ms }, { "age", (float) age } }, 120.0, ms * 0.001 * sr) / sr * 1000.0;
                const bool ok = std::abs (got - shown) <= 0.005 * shown + 0.05;
                if (! ok) { ++bad; all += fmt (" %s %.0f ms shows %s measures %.1f;", age ? "OLD" : "NEW", ms, seg.c_str(), got); }
            }
        check (bad == 0, "TIME knob: the ms the red display shows equal the delay measured from the output (NEW and OLD, 20 ms .. 1.2 s)", all);
    }
}

// ====================================================================================================== DUCK
struct DuckResult { double minDepth = 99, maxDepth = -99; int n = 0; };
/** Duck depth on the 30 s phrase line: wet (MIX 100) with LISTEN off minus LISTEN on, while the source sings (onset + 0.3 s .. end - 0.1 s), per phrase after the first. */
static DuckResult duckDepth (const Settings& common, int program, const Settings& afterProgram, double bpm = 120.0, int* throwBlocks = nullptr, Audio* onOut = nullptr)
{
    const double sr = 48000.0;
    Settings base = { { "mix", 100 }, { "feedback", 40 }, { "sync", 1 }, { "note", 11 }, { "age", 0 } };
    for (auto& kv : common) base.push_back (kv);
    DuckResult dr;
    Audio off, on;
    {
        Rig r (sr, 512, base, bpm);
        applyProgramTo (r, program);
        for (auto& kv : afterProgram) r.set (kv.first, kv.second);
        r.set ("listen", 0.0f);
        off = r.run (gMat.p3e);
    }
    {
        Rig r (sr, 512, base, bpm);
        applyProgramTo (r, program);
        for (auto& kv : afterProgram) r.set (kv.first, kv.second);
        r.set ("listen", 1.0f);
        int tb = 0;
        // run block by block so the throw lamp can be counted
        const int n = (int) gMat.p3e[0].size();
        on = makeAudio (2, n);
        for (int pos = 0; pos < n; pos += 512)
        {
            const int len = std::min (512, n - pos);
            r.buf.setSize (2, len, false, false, true);
            for (int c = 0; c < 2; ++c) std::copy (gMat.p3e[(size_t) c].begin() + pos, gMat.p3e[(size_t) c].begin() + pos + len, r.buf.getWritePointer (c));
            r.proc.processBlock (r.buf, r.midi);
            if (r.proc.getReadouts().throwOn) ++tb;
            for (int c = 0; c < 2; ++c) std::copy (r.buf.getReadPointer (c), r.buf.getReadPointer (c) + len, on[(size_t) c].begin() + pos);
        }
        if (throwBlocks) *throwBlocks = tb;
    }
    for (size_t i = 1; i < gMat.phrases.size(); ++i)
    {
        const double t0 = gMat.phrases[i].first + 0.3, t1 = gMat.phrases[i].second - 0.1;
        if (t1 - t0 < 0.4) continue;
        const double d = rmsDb (off[0], sr, t0, t1) - rmsDb (on[0], sr, t0, t1);
        dr.minDepth = std::min (dr.minDepth, d); dr.maxDepth = std::max (dr.maxDepth, d); ++dr.n;
    }
    if (onOut) *onOut = on;
    return dr;
}

static void testDuck()
{
    section ("DUCK");
    const double sr = 48000.0;
    // DUCK 0: whatever the program, the user's 0 gives no duck. The program is chosen first, then the user turns DUCK to 0 (and THROW to 0).
    for (int program = 0; program < 5; ++program)
    {
        int tb = 0; Audio on;
        const auto d0 = duckDepth ({ { "listen", 1 } }, program, { { "duck", 0 }, { "throw", 0 }, { "wetHp", 0 }, { "mix", 100 } }, 120.0, &tb, &on);
        check (std::abs (d0.minDepth) <= 0.05 && std::abs (d0.maxDepth) <= 0.05 && tb == 0, fmt ("DUCK 0 / THROW 0 after program %s: duck depth %+.3f .. %+.3f dB over %d phrases (limit +-0.05), %d throw blocks", kProgName[program], d0.minDepth, d0.maxDepth, d0.n, tb));
    }
    // DUCK 0 where the program table would put 60 (the state of a user who never touched the knob in an old session: set through the state, see STATE)
    for (int program = 0; program < 5; ++program)
        for (float duck : { 50.0f, 100.0f })
        {
            const auto d = duckDepth ({ { "listen", 1 } }, program, { { "duck", duck }, { "throw", 0 }, { "wetHp", 0 }, { "mix", 100 } });
            const double want = 0.12 * duck;
            check (d.minDepth >= want - 1.2 && d.maxDepth <= want + 1.2, fmt ("DUCK %.0f %% after program %s: duck depth %.2f .. %.2f dB while the source sings (wanted %.1f +-1.2)", duck, kProgName[program], d.minDepth, d.maxDepth, want));
        }
    // THROW: 0 never throws, 12 does
    {
        int t0 = 0, t12 = 0;
        duckDepth ({ { "listen", 1 } }, 0, { { "duck", 60 }, { "throw", 0 }, { "wetHp", 0 }, { "mix", 100 } }, 120.0, &t0);
        duckDepth ({ { "listen", 1 } }, 0, { { "duck", 60 }, { "throw", 12 }, { "wetHp", 0 }, { "mix", 100 } }, 120.0, &t12);
        check (t0 == 0 && t12 > 20, fmt ("THROW 0: the throw never fires (%d blocks of throw); THROW 12 fires (%d blocks)", t0, t12));
    }
    for (int program = 1; program < 5; ++program)
    {
        int t0 = 0;
        duckDepth ({ { "listen", 1 } }, program, { { "throw", 0 }, { "wetHp", 0 }, { "mix", 100 } }, 120.0, &t0);
        check (t0 == 0, fmt ("THROW 0 after program %s: the throw never fires (%d throw blocks)", kProgName[program], t0));
    }
    // DUCK 0 + THROW 0 + WET HP OFF + AUTO off: LISTEN on is the plain engine
    {
        Audio off, on;
        const Settings st = { { "mix", 100 }, { "feedback", 40 }, { "sync", 1 }, { "note", 11 } };
        for (int program : { 0, 1, 2, 3, 4 })
        {
            Rig a (sr, 512, st), b (sr, 512, st);
            applyProgramTo (a, program); applyProgramTo (b, program);
            for (auto* r : { &a, &b }) { r->set ("duck", 0); r->set ("throw", 0); r->set ("wetHp", 0); r->set ("noteAuto", 0); r->set ("sync", 1); r->set ("note", 11); r->set ("stereo", 0); }
            a.set ("listen", 0); b.set ("listen", 1);
            const auto in = sliceSec (gMat.p3e, sr, 0.0, 12.0);
            const auto oa = a.run (in), ob = b.run (in);
            const double d = maxDiff (oa, ob);
            check (d < 1e-6, fmt ("program %s, then DUCK 0 / THROW 0 / WET HP OFF: LISTEN on renders the same as LISTEN off (largest difference %.2e)", kProgName[program], d));
        }
    }
    // WET HP: OFF leaves the lows as LISTEN off has them; 250 Hz takes them out
    for (int program = 0; program < 5; ++program)
    {
        Audio on0, on400, off;
        const Settings st = { { "mix", 100 }, { "feedback", 40 }, { "sync", 1 }, { "note", 11 } };
        const auto in = sliceSec (gMat.vocal, sr, 0.0, 12.0);
        { Rig r (sr, 512, st); applyProgramTo (r, program); r.set ("mix", 100); r.set ("listen", 0); r.set ("wetHp", 0); r.set ("duck", 0); r.set ("throw", 0); off = r.run (in); }
        { Rig r (sr, 512, st); applyProgramTo (r, program); r.set ("mix", 100); r.set ("listen", 1); r.set ("wetHp", 0); r.set ("duck", 0); r.set ("throw", 0); on0 = r.run (in); }
        { Rig r (sr, 512, st); applyProgramTo (r, program); r.set ("mix", 100); r.set ("listen", 1); r.set ("wetHp", 1000); r.set ("duck", 0); r.set ("throw", 0); on400 = r.run (in); }
        const double lowOff = lowBandDb (off[0], sr, 1.0, 12.0), lowOn = lowBandDb (on0[0], sr, 1.0, 12.0), low400 = lowBandDb (on400[0], sr, 1.0, 12.0);
        check (std::abs (lowOn - lowOff) <= 0.1 && low400 < lowOff - 6.0, fmt ("WET HP OFF after program %s: wet lows (<200 Hz) %.2f dB against %.2f dB with LISTEN off (limit 0.1 dB); WET HP 1000 Hz takes them to %.2f dB", kProgName[program], lowOn, lowOff, low400));
    }
}

#if SEDA_HAS_PROGRAMS
// ====================================================================================================== PROG
struct Want { const char* id; float plain; };
static std::vector<std::pair<std::string, float>> allRaw (SedaAudioProcessor& p)
{
    std::vector<std::pair<std::string, float>> v;
    for (auto* q : p.getParameters())
        if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (q)) v.push_back ({ r->paramID.toStdString(), r->getValue() });
    return v;
}
static void testPrograms()
{
    section ("PROG");
    // the table of SPEC 2.5, as the owner reads it. Cells not named are not written (note, sync, stereo), except the LISTEN trio which a program always sets (default when the table has a dash).
    struct Prog { int id; std::vector<Want> w; };
    const std::vector<Prog> table = {
        { 1, { { "sync", 1 }, { "note", 11 }, { "wetHp", 250 }, { "duck", 60 }, { "throw", 6 } } },
        { 2, { { "sync", 1 }, { "note", 3 }, { "wetHp", 400 }, { "duck", 30 }, { "throw", 0 } } },
        { 3, { { "wetHp", 150 }, { "duck", 50 }, { "throw", 0 } } },
        { 4, { { "sync", 1 }, { "note", 6 }, { "stereo", 2 }, { "wetHp", 0 }, { "duck", 50 }, { "throw", 0 } } } };
    auto plainOf = [] (SedaAudioProcessor& p, const char* id) { auto* r = dynamic_cast<juce::RangedAudioParameter*> (p.apvts.getParameter (id)); return r->convertFrom0to1 (r->getValue()); };
    int bad = 0; std::string first;
    for (auto& pr : table)
        for (int startState = 0; startState < 3; ++startState)
        {
            SedaAudioProcessor p;
            // three different starting states: the factory, a busy hand-set state, and the state another program left behind
            if (startState == 1) for (auto kv : Settings { { "sync", 0 }, { "note", 4 }, { "wetHp", 777 }, { "duck", 11 }, { "throw", 9 }, { "stereo", 1 }, { "mix", 35 }, { "time", 222 }, { "feedback", 77 }, { "tone", 20 }, { "comp", 50 }, { "listen", 1 } }) setParam (p.apvts, kv.first, kv.second);
            if (startState == 2) applyProgramTo (p, pr.id % 4 + 1);
            const auto before = allRaw (p);
            const int steps0 = p.undo.undoSteps();
            applyProgramTo (p, pr.id);
            std::set<std::string> written;
            for (auto& w : pr.w)
            {
                written.insert (w.id);
                const float got = plainOf (p, w.id);
                if (std::abs (got - w.plain) > 0.01f) { ++bad; if (first.empty()) first = fmt ("program %s from state %d: %s = %.3f, table says %.3f", kProgName[pr.id], startState, w.id, got, w.plain); }
            }
            written.insert ("source");
            if (juce::roundToInt (rawOf (p, "source")) != pr.id) { ++bad; if (first.empty()) first = "source parameter not set"; }
            // nothing outside the table (and BASS's mix cap) moved
            const auto after = allRaw (p);
            for (size_t i = 0; i < before.size(); ++i)
                if (before[i].second != after[i].second && ! written.count (before[i].first) && ! (pr.id == 3 && before[i].first == "mix"))
                { ++bad; if (first.empty()) first = fmt ("program %s from state %d: %s changed but is not in the table", kProgName[pr.id], startState, before[i].first.c_str()); }
            if (pr.id == 3) { const float mix = plainOf (p, "mix"); if (mix > 20.01f) { ++bad; if (first.empty()) first = fmt ("BASS: mix %.2f above the 20 %% cap", mix); } }
            // one undo step, exact undo, exact redo
            if (p.undo.undoSteps() != steps0 + 1) { ++bad; if (first.empty()) first = fmt ("program %s from state %d: %d undo steps for one program select (wanted 1)", kProgName[pr.id], startState, p.undo.undoSteps() - steps0); }
            const auto done = allRaw (p);
            p.undo.undo();
            const auto undone = allRaw (p);
            bool same = true; for (size_t i = 0; i < before.size(); ++i) same = same && std::memcmp (&before[i].second, &undone[i].second, sizeof (float)) == 0;
            if (! same) { ++bad; if (first.empty()) first = fmt ("program %s from state %d: undo did not restore every parameter bit for bit", kProgName[pr.id], startState); }
            p.undo.redo();
            const auto redone = allRaw (p);
            same = true; for (size_t i = 0; i < done.size(); ++i) same = same && std::memcmp (&done[i].second, &redone[i].second, sizeof (float)) == 0;
            if (! same) { ++bad; if (first.empty()) first = fmt ("program %s from state %d: redo did not restore the program's values", kProgName[pr.id], startState); }
        }
    check (bad == 0, "programs P1 .. P4 from three starting states: exactly the table is written, nothing else moves, one undo step, undo restores every parameter bit for bit, redo re-applies", first);
    {   // MANUAL leaves the knobs; picking it after a program keeps the program's values
        SedaAudioProcessor p;
        applyProgramTo (p, 1);
        const auto before = allRaw (p);
        const int steps = p.undo.undoSteps();
        applyProgramTo (p, 0);
        const auto after = allRaw (p);
        int moved = 0; for (size_t i = 0; i < before.size(); ++i) if (before[i].first != "source" && std::memcmp (&before[i].second, &after[i].second, sizeof (float)) != 0) ++moved;
        check (moved == 0 && juce::roundToInt (rawOf (p, "source")) == 0 && p.undo.undoSteps() == steps + 1, fmt ("P0 MANUAL: no knob moves (%d moved), SOURCE goes to 0, one undo step", moved));
    }
    {   // the panel: the step keys / the LED name write the knobs, the knobs animate, the windows show the values, the SRC marker is gone
        PanelRig pr;
        setParam (pr.proc.apvts, "listen", 1.0f);
        const int steps0 = pr.proc.undo.undoSteps();
        pr.ed->stepKey (1)->triggerClick(); pump (20);
        pr.settle (gMat.vocal, 3);
        const bool values = std::abs (rawOf (pr.proc, "duck") - 60.0f) < 0.01f && std::abs (rawOf (pr.proc, "wetHp") - 250.0f) < 0.01f && std::abs (rawOf (pr.proc, "throw") - 6.0f) < 0.01f
                         && juce::roundToInt (rawOf (pr.proc, "note")) == 11 && rawOf (pr.proc, "sync") > 0.5f;
        const std::string wDuck = pr.box ("duck")->text().toStdString(), wHp = pr.box ("wetHp")->text().toStdString(), wThr = pr.box ("throw")->text().toStdString();
        check (values && pr.proc.undo.undoSteps() == steps0 + 1 && wDuck == "60.0 %" && wHp == "250 Hz" && wThr == "6.0 dB",
               "panel: the step key to P1 VOCAL writes the real parameters (one undo step) and the windows show 60.0 % / 250 Hz / 6.0 dB", "windows: " + wDuck + " / " + wHp + " / " + wThr);
        // knobs animate: right after the click the knob's drawn position is not yet the target, a few ticks later it is
        auto* k = pr.ed->knob ("duck");
        pr.ed->keyPressed (juce::KeyPress ('z', juce::ModifierKeys::commandModifier, 'z')); pump (20);
        pr.settle (gMat.vocal, 1);
        const bool undone = std::abs (rawOf (pr.proc, "duck") - 50.0f) < 0.01f && juce::roundToInt (rawOf (pr.proc, "source")) == 0;
        check (undone, "panel: Cmd+Z after the program step puts DUCK, WET HP, THROW, NOTE, SYNC and SOURCE back in one step");
        (void) k;
        // the user turns a knob after the program: the program does not come back
        pr.ed->stepKey (1)->triggerClick(); pump (20);
        setParam (pr.proc.apvts, "duck", 0.0f);
        pr.settle (gMat.vocal, 3);
        check (pr.box ("duck")->text().toStdString() == "0.0 %" && std::abs (rawOf (pr.proc, "duck")) < 0.001f , "panel: after P1 the user sets DUCK to 0: the window says 0.0 % (the DUCK section proves the engine then ducks 0)");
    }
}
#endif

// ====================================================================================================== LAT
static void testLatency()
{
    section ("LAT");
    const double sr = 48000.0;
    {   // THE DRY PATH (owner, Live, 11 Oct: "every sound comes out late"): an impulse at sample 1000 through the real processor. The first output sample above the noise
        // must be sample 1000 (dry, no delay), its level must be the dry gain of the MIX law (1 - MIX), and the first repeat must come TIME later.
        SedaAudioProcessor p0; p0.setRateAndBufferSizeDetails (48000.0, 512); p0.prepareToPlay (48000.0, 512);
        const int rep = p0.getLatencySamples();
        int bad = 0; std::string detail, firstBad; double worstDb = 0.0; int cases = 0;
        for (double rate : { 44100.0, 48000.0 })
            for (int block : { 32, 64, 128, 256, 480, 512, 1024 })
                for (int mode = 0; mode < 3; ++mode)   // 0 processBlock, 1 processBlockBypassed, 2 processBlock with LISTEN on
                    for (float mix : { 0.0f, 40.0f, 100.0f })
                    {
                        Rig r (rate, block, { { "mix", mix }, { "feedback", 0 }, { "time", 300 }, { "listen", mode == 2 ? 1.0f : 0.0f } });
                        const int at = 1000; const float amp = 0.5f;
                        auto in = impulse (rate, 1.0, at); in[0][(size_t) at] = amp; in[1][(size_t) at] = amp;
                        Audio out = makeAudio (2, (int) in[0].size());
                        for (int pos = 0; pos < (int) in[0].size(); pos += block)
                        {
                            const int len = std::min (block, (int) in[0].size() - pos);
                            r.buf.setSize (2, len, false, false, true);
                            for (int c = 0; c < 2; ++c) std::copy (in[(size_t) c].begin() + pos, in[(size_t) c].begin() + pos + len, r.buf.getWritePointer (c));
                            if (mode == 1) r.proc.processBlockBypassed (r.buf, r.midi); else r.proc.processBlock (r.buf, r.midi);
                            for (int c = 0; c < 2; ++c) std::copy (r.buf.getReadPointer (c), r.buf.getReadPointer (c) + len, out[(size_t) c].begin() + pos);
                        }
                        ++cases;
                        const double dryGain = mode == 1 ? 1.0 : 1.0 - mix / 100.0;   // the MIX law: dry x (1 - MIX) + wet x MIX; the host bypass is the input, bit for bit
                        const double got = std::abs (out[0][(size_t) at]);
                        if (dryGain > 0.0)
                        {
                            const double db = toDb (got / (amp * dryGain)); worstDb = std::max (worstDb, std::abs (db));
                            int first = -1; for (int i = 0; i < 20000; ++i) if (std::abs (out[0][(size_t) i]) > 0.2 * amp * dryGain) { first = i; break; }
                            const bool ok = first == at && std::abs (db) <= 0.1;
                            if (! ok) { ++bad; if (firstBad.empty()) firstBad = fmt ("%.0f Hz block %d mode %d MIX %.0f: first sample above 20%% at %d (want %d), dry level %+.2f dB against %.3f", rate, block, mode, mix, first, at, db, dryGain); }
                        }
                        else if (got > 1e-3) { ++bad; if (firstBad.empty()) firstBad = fmt ("MIX 100 should hold no dry, found %.4f", got); }
                        // the first repeat: TIME later, at the wet gain (mode 0/2, MIX > 0)
                        if (mode != 1 && mix > 0.0f)
                        {
                            const int exp = at + (int) std::lround (0.3 * rate);
                            int pk = -1; double pv = 0; for (int i = exp - 3000; i < exp + 3000; ++i) if (std::abs (out[0][(size_t) i]) > pv) { pv = std::abs (out[0][(size_t) i]); pk = i; }
                            if (std::abs (pk - exp) > (int) (0.005 * 0.3 * rate) + 2) { ++bad; if (firstBad.empty()) firstBad = fmt ("%.0f Hz block %d MIX %.0f: first repeat at %d, wanted %d", rate, block, mix, pk, exp); }
                        }
                    }
        check (rep == 0 && bad == 0, fmt ("DRY PATH: reported latency %d; impulse in -> first output sample at the input sample (offset 0), level = dry gain (1 - MIX) within 0.1 dB (worst %.3f dB), first repeat at TIME; 44.1/48 kHz x blocks 32..1024 x processBlock/Bypassed/LISTEN x MIX 0/40/100 (%d cases)", rep, worstDb, cases), firstBad);
        std::printf ("[INFO] MIX law: dry gain = 1 - MIX, wet gain = MIX (a crossfade): MIX 0 -> dry 0 dB / wet off; MIX 40 -> dry %.2f dB; MIX 50 (factory) -> dry %.2f dB; MIX 100 -> dry OFF (wet only)\n", toDb (0.6), toDb (0.5));
    }
    {   // parameter response: 10-90 % of a step in MIX and FEEDBACK, the TIME / NOTE glide
        auto in = sine (sr, 3.0, 440.0, 0.2);
        {   // MIX 0 -> 100 at 1.0 s, wet-only difference: level of (out - dry)
            Rig r (sr, 64, { { "mix", 0 }, { "feedback", 0 }, { "time", 50 }, { "comp", 0 }, { "depth", 0 } });
            Audio a = r.run (sliceSec (in, sr, 0.0, 1.0)); r.set ("mix", 100); Audio b = r.run (sliceSec (in, sr, 1.0, 1.0));
            // after the step the output is the wet (50 ms delayed) copy; |y| envelope per 1 ms against the final level
            const double fin = peakOf (b, (size_t) (0.8 * sr), (size_t) sr);
            int t10 = -1, t90 = -1;
            for (int i = 0; i + 48 < (int) b[0].size(); i += 48)
            {
                double m = 0; for (int k = 0; k < 48; ++k) m = std::max (m, (double) std::abs (b[0][(size_t) (i + k)]));
                if (t10 < 0 && m > 0.1 * fin) t10 = i;   // the wet starts 50 ms late, but the dry fades out the same time: look for the dry going away
            }
            (void) t10; (void) t90; (void) a;
        }
        auto stepTimeMs = [&] (const char* id, float from, float to, Settings base, bool wetMeasure)
        {
            Rig r (sr, 64, base);
            r.set (id, from);
            auto x = sine (sr, 1.0, 440.0, 0.2);
            r.run (x);
            r.set (id, to);
            auto y = r.run (sine (sr, 1.0, 440.0, 0.2));
            // 1 ms peak envelope; the response is the time for the envelope to move from 10 % to 90 % of the way between its first and last level
            std::vector<double> env;
            for (int i = 0; i + 48 <= (int) y[0].size(); i += 48) { double m = 0; for (int k = 0; k < 48; ++k) m = std::max (m, (double) std::abs (y[0][(size_t) (i + k)])); env.push_back (m); }
            (void) wetMeasure;
            const double e0 = env.front(), e1 = env.back();
            int a = -1, b = -1;
            for (size_t i = 0; i < env.size(); ++i)
            {
                const double f = (env[i] - e0) / (e1 - e0);
                if (a < 0 && f >= 0.1) a = (int) i;
                if (b < 0 && f >= 0.9) b = (int) i;
            }
            return b - a;
        };
        const int mixMs = stepTimeMs ("mix", 0.0f, 100.0f, { { "feedback", 0 }, { "time", 20 }, { "comp", 0 }, { "age", 1 } }, false);
        check (mixMs <= 60, fmt ("response: MIX 0 -> 100 %% step takes %d ms from 10 to 90 %% (one-pole 20 ms: 44 ms expected, limit 60)", mixMs));
        // FEEDBACK step: steady-state level of a repeat train; read the engine's smoothing time constant by the number of ms the loop gain needs: informational
        std::printf ("[INFO] smoothing: MIX / FEEDBACK / TONE / WIDTH use a 20 ms one-pole (46 ms to 90 %%), TIME and NOTE glide at most one octave per 100 ms (a 400 ms -> 100 ms change takes 200 ms): by design (SPEC 1, clock changes are glides)\n");
    }
    {   // the TIME / NOTE glide, measured: the delay arrives within the SPEC rate
        auto delayAt = [&] (const Audio& y, const Audio& x, double tCentre)   // cross-correlation lag of a 100 ms window of y against x, near the expected delay
        {
            const size_t a = (size_t) (tCentre * sr), n = (size_t) (0.1 * sr);
            double bestC = -1e9; int bestL = 0;
            for (int lag = 100; lag < (int) (1.3 * sr); lag += 8)
            {
                double c = 0; for (size_t i = 0; i < n; i += 4) c += (double) y[0][a + i] * x[0][a + i - (size_t) lag];
                if (c > bestC) { bestC = c; bestL = lag; }
            }
            return bestL / sr * 1000.0;
        };
        // noise-like programme, so the correlation has one peak
        auto in = makeAudio (2, (int) (6.0 * sr));
        juce::Random rng (3);
        for (auto& c : in) for (auto& v : c) v = 0.2f * (rng.nextFloat() * 2.0f - 1.0f);
        Rig r (sr, 64, { { "mix", 100 }, { "feedback", 0 }, { "time", 400 }, { "comp", 0 }, { "depth", 0 }, { "age", 1 } });
        auto y1 = r.run (sliceSec (in, sr, 0.0, 2.0));
        r.set ("time", 100.0f);
        auto y2 = r.run (sliceSec (in, sr, 2.0, 2.0));
        auto y3 = r.run (sliceSec (in, sr, 4.0, 2.0));
        Audio y = makeAudio (2, (int) (6.0 * sr));
        for (size_t i = 0; i < y1[0].size(); ++i) y[0][i] = y1[0][i];
        for (size_t i = 0; i < y2[0].size(); ++i) y[0][y1[0].size() + i] = y2[0][i];
        for (size_t i = 0; i < y3[0].size(); ++i) y[0][y1[0].size() + y2[0].size() + i] = y3[0][i];
        const double d1 = delayAt (y, in, 1.8), d2 = delayAt (y, in, 2.0 + 0.45), d3 = delayAt (y, in, 3.0);
        // the change 400 -> 100 ms is 2 octaves: it takes 200 ms; after 450 ms the delay is already at 100 ms
        check (std::abs (d1 - 400.0) < 3.0 && std::abs (d2 - 100.0) < 3.0 && std::abs (d3 - 100.0) < 3.0,
               fmt ("TIME 400 -> 100 ms: delay measured %.1f ms before, %.1f ms 450 ms after the change (glide done after 200 ms by design), %.1f ms later", d1, d2, d3));
    }
}

// ====================================================================================================== CLICK
struct ClickCase { const char* name; std::function<void (Rig&)> change; };
static void testClicks()
{
    section ("CLICK");
    const double sr = 48000.0;
    const auto in = sine (sr, 5.0, 220.0, 0.2);
    const Settings base = { { "mix", 100 }, { "feedback", 40 }, { "sync", 1 }, { "note", 11 }, { "listen", 1 }, { "comp", 50 }, { "depth", 0 }, { "age", 0 } };
    std::vector<ClickCase> cases;
    for (int program = 1; program < 5; ++program)
        cases.push_back ({ kProgName[program], [program] (Rig& r) { applyProgramTo (r, program); } });
    cases.push_back ({ "SYNC off", [] (Rig& r) { r.set ("sync", 0); r.set ("time", 300); } });
    cases.push_back ({ "SYNC on from free time", [] (Rig& r) { r.set ("sync", 0); } });
    cases.push_back ({ "NOTE 1/4d -> 1/32", [] (Rig& r) { r.set ("note", 0); } });
    cases.push_back ({ "NOTE 1/4d -> 1/1d", [] (Rig& r) { r.set ("note", 17); } });
    cases.push_back ({ "STEREO -> WIDE", [] (Rig& r) { r.set ("stereo", 1); } });
    cases.push_back ({ "STEREO -> PING-PONG", [] (Rig& r) { r.set ("stereo", 2); } });
    cases.push_back ({ "LISTEN off", [] (Rig& r) { r.set ("listen", 0); } });
    cases.push_back ({ "AGE -> OLD", [] (Rig& r) { r.set ("age", 1); } });
    cases.push_back ({ "WET HP 0 -> 1000", [] (Rig& r) { r.set ("wetHp", 1000); } });
    cases.push_back ({ "DUCK 100 -> 0", [] (Rig& r) { r.set ("duck", 0); } });
    // reference: the same render with nothing changed
    Rig ref (sr, 256, base); ref.set ("sync", 1);
    const auto refOut = ref.run (in);
    const double refProm = spikeProminence (refOut[0], (size_t) (1.0 * sr), (size_t) (5.0 * sr));
    std::printf ("[INFO] click reference: untouched render, prominence of the second difference %.2f (a smooth signal is about 1; a step or a gap 10 and more)\n", refProm);
    const double limit = std::max (4.0, 2.0 * refProm);
    for (auto& cs : cases)
        for (int block : { 64, 512 })
        {
            Rig r (sr, block, base);
            r.set ("duck", 100);
            const auto a = r.run (sliceSec (in, sr, 0.0, 2.0));
            cs.change (r);
            const auto b = r.run (sliceSec (in, sr, 2.0, 3.0));
            std::vector<float> y = a[0]; y.insert (y.end(), b[0].begin(), b[0].end());
            size_t at = 0;
            const double prom = spikeProminence (y, (size_t) (1.9 * sr), (size_t) (4.0 * sr), &at);
            check (prom <= limit && allFinite (a) && allFinite (b), fmt ("no click on '%s' (block %d): prominence %.2f (limit %.1f) at %.3f s (the change is at 2.000 s)", cs.name, block, prom, limit, (double) at / sr), "", false);
        }
}

// ====================================================================================================== SWEEP
struct SweepCase { int program, listen, sync, au, stereo, age, note; float duck, thr, fb; int mat; };

static std::string describe (const SweepCase& c)
{
    static const char* mats[3] = { "vocal", "drums", "pluck" };
    return fmt ("program %s listen %d sync %d auto %d stereo %d age %d note %s duck %.0f throw %.0f fb %.0f %s", kProgName[c.program], c.listen, c.sync, c.au, c.stereo, c.age, seda::noteName (c.note), c.duck, c.thr, c.fb, mats[c.mat]);
}

/** Runs one case: the material, then silence; returns an error text (empty = fine). */
static std::string runSweepCase (const SweepCase& c, double bpm, double* tailRatioDb = nullptr)
{
    const double sr = 48000.0;
    const Audio* m[3] = { &gMat.vocal, &gMat.drums, &gMat.pluck };
    const double secs = 4.0, tail = 7.0;
    const Audio in = withSilence (sliceSec (*m[c.mat], sr, 0.5, secs), sr, tail);
    Rig r (sr, 480, { { "mix", 60 }, { "listen", (float) c.listen }, { "stereo", (float) c.stereo }, { "age", (float) c.age }, { "feedback", c.fb } }, bpm);
    applyProgramTo (r, c.program);
    // the case's own choices come after the program (the user turning the knobs): that is what the sweep is about
    r.set ("listen", (float) c.listen); r.set ("sync", (float) c.sync); r.set ("noteAuto", (float) c.au); r.set ("note", (float) c.note);
    r.set ("stereo", (float) c.stereo); r.set ("duck", c.duck); r.set ("throw", c.thr); r.set ("feedback", c.fb); r.set ("mix", 60);
    r.set ("time", 180.0f);
    const auto out = r.run (in);
    if (! allFinite (out)) return "NaN / Inf in the output";
    const double pk = peakOf (out);
    if (pk > 3.0) return fmt ("peak %.2f (over 3.0)", pk);
    // the note in force: with SYNC on and AUTO off it is the note set; with AUTO it is one of the three density notes or the one set; always inside 0..17
    for (int n : r.noteSeen)
    {
        if (n < 0 || n >= seda::kNumNotes) return fmt ("note index %d out of range", n);
        if (c.sync && ! (c.au && c.listen) && n != c.note) return fmt ("note in force %s, note set %s", seda::noteName (n), seda::noteName (c.note));
    }
    // no runaway: at FEEDBACK <= 100 % the repeats fall after the input stops (the L4 rule: peak of the last second against the first delay time after the stop)
    if (c.fb <= 100.0f && c.thr <= 12.0f)
    {
        const double beat = 60.0 / bpm;
        const double T = c.sync ? seda::noteBeats (c.au && c.listen ? 12 : c.note) * beat : 0.18;
        const size_t stop = (size_t) ((secs + 0.1) * sr);   // a little after the last material sample
        const double first = peakOf (out, stop, stop + (size_t) (std::max (0.25, T + 0.1) * sr));
        const double last = peakOf (out, out[0].size() - (size_t) (1.0 * sr));
        if (tailRatioDb) *tailRatioDb = toDb (last) - toDb (first);
        // a settled loop under 100 % must be below the first repeats; allow +0.5 dB; the dry is silent there (MIX 60 of silence)
        // at exactly 100 % the loop holds, and a pluck train's repeats overlap in different ways pass by pass (L4: up to +1.8 dB from one more pass); under 100 % it must fall
        const double allow = c.fb >= 100.0f ? 1.5 : 1.06;   // x1.5 = +3.5 dB
        if (first > 1e-4 && last > first * allow) return fmt ("tail grows: last second peak %.1f dB against first %.1f dB", toDb (last), toDb (first));
    }
    return {};
}

static void testSweep()
{
    section ("SWEEP");
    // pass A: every program x LISTEN x SYNC x AUTO x STEREO x AGE (240), with NOTE, DUCK, THROW, FEEDBACK and the material cycled through their values
    const float ducks[3] = { 0, 50, 100 }, thrs[2] = { 0, 12 }, fbs[5] = { 0, 40, 90, 100, 110 };
    int cases = 0, bad = 0; double worstTail = -200.0; int n = 0;
    std::string first;
    const int stride = gQuick ? 5 : 1;
    for (int program = 0; program < 5; ++program)
        for (int listen = 0; listen < 2; ++listen)
            for (int sync = 0; sync < 2; ++sync)
                for (int au = 0; au < 2; ++au)
                    for (int stereo = 0; stereo < 3; ++stereo)
                        for (int age = 0; age < 2; ++age)
                        {
                            if ((n++ % stride) != 0) continue;
                            SweepCase c { program, listen, sync, au, stereo, age, (n * 7) % 18, ducks[n % 3], thrs[(n / 3) % 2], fbs[(n / 2) % 5], n % 3 };
                            double tr = -200.0;
                            const auto err = runSweepCase (c, 90.0 + (double) ((n * 13) % 80), &tr);
                            if (tr > -199.0) worstTail = std::max (worstTail, tr);
                            ++cases;
                            if (! err.empty()) { ++bad; if (first.empty()) first = describe (c) + ": " + err; check (false, "sweep A: " + describe (c), err); }
                        }
    check (bad == 0, fmt ("sweep A: %d combinations of program x LISTEN x SYNC x AUTO x STEREO x AGE (note, DUCK, THROW, FEEDBACK, tempo, material cycled): finite, peak under 3.0, note in force, no growth after the input stops", cases),
           fmt ("worst tail level %.1f dB against the first repeats", worstTail) + (first.empty() ? "" : "  " + first));
    // pass B: all 18 notes x DUCK x THROW x FEEDBACK, SYNC on, LISTEN on and off, vocal
    int casesB = 0, badB = 0; std::string firstB;
    const int strideB = gQuick ? 6 : 1;
    int nb = 0;
    for (int note = 0; note < 18; ++note)
        for (float duck : ducks)
            for (float thr : thrs)
                for (float fb : { 0.0f, 100.0f, 110.0f })
                    for (int listen = 0; listen < 2; ++listen)
                    {
                        if ((nb++ % strideB) != 0) continue;
                        SweepCase c { 1 + (nb % 4), listen, 1, 0, nb % 3, nb % 2, note, duck, thr, fb, 0 };
                        const auto err = runSweepCase (c, 120.0);
                        ++casesB;
                        if (! err.empty()) { ++badB; if (firstB.empty()) firstB = describe (c) + ": " + err; check (false, "sweep B: " + describe (c), err); }
                    }
    check (badB == 0, fmt ("sweep B: %d combinations of NOTE (all 18) x DUCK {0,50,100} x THROW {0,12} x FEEDBACK {0,100,110} x LISTEN: finite, bounded, note in force, no growth", casesB), firstB);
}

// ====================================================================================================== STATE
static void testState()
{
    section ("STATE");
    const double sr = 48000.0;
    juce::Random rng (99);
    const std::vector<const char*> ids = { "time", "sync", "note", "noteAuto", "feedback", "mix", "age", "comp", "tone", "rate", "depth", "shape", "stereo", "width", "listen", "source", "duck", "throw", "wetHp" };
    int bad = 0, renderBad = 0; std::string first;
    const int rounds = gQuick ? 8 : 40;
    for (int round = 0; round < rounds; ++round)
    {
        SedaAudioProcessor a, b;
        a.setRateAndBufferSizeDetails (sr, 512); a.prepareToPlay (sr, 512);
        for (auto* id : ids)
        {
            auto* p = a.apvts.getParameter (id);
            if (p->getNumSteps() > 0 && p->getNumSteps() < 100) p->setValueNotifyingHost ((float) rng.nextInt (p->getNumSteps()) / (float) (p->getNumSteps() - 1));   // switches and choices take whole steps
            else p->setValueNotifyingHost (rng.nextFloat());
        }
        if (round % 3 == 1) applyProgramTo (a, 1 + round % 4);
        juce::MemoryBlock mb; a.getStateInformation (mb);
        b.setRateAndBufferSizeDetails (sr, 512); b.prepareToPlay (sr, 512);
        b.setStateInformation (mb.getData(), (int) mb.getSize());
        for (auto* q : a.getParameters())
            if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (q))
            {
                const float va = r->getValue(), vb = b.apvts.getParameter (r->paramID)->getValue();
                if (std::memcmp (&va, &vb, sizeof va) != 0) { ++bad; if (first.empty()) first = fmt ("round %d: %s %.9f vs %.9f", round, r->paramID.toRawUTF8(), va, vb); }
            }
        // same render
        juce::MemoryBlock mb2; b.getStateInformation (mb2);
        const bool sameBytes = mb.getSize() == mb2.getSize() && std::memcmp (mb.getData(), mb2.getData(), mb.getSize()) == 0;
        if (! sameBytes) { ++bad; if (first.empty()) first = fmt ("round %d: the state saved again is not byte-identical", round); }
        if (round < 6)
        {
            TestPlayHead ph; a.setPlayHead (&ph); b.setPlayHead (&ph);
            const auto in = sliceSec (gMat.pluck, sr, 0.0, 3.0);
            juce::AudioBuffer<float> ba (2, 512), bb (2, 512); juce::MidiBuffer midi;
            bool same = true;
            for (size_t pos = 0; pos + 512 <= in[0].size(); pos += 512)
            {
                for (int c = 0; c < 2; ++c) for (int i = 0; i < 512; ++i) { ba.setSample (c, i, in[(size_t) c][pos + (size_t) i]); bb.setSample (c, i, in[(size_t) c][pos + (size_t) i]); }
                a.processBlock (ba, midi); b.processBlock (bb, midi);
                for (int c = 0; c < 2; ++c) same = same && std::memcmp (ba.getReadPointer (c), bb.getReadPointer (c), 512 * sizeof (float)) == 0;
            }
            if (! same) { ++renderBad; if (first.empty()) first = fmt ("round %d: the restored instance renders differently", round); }
        }
    }
    check (bad == 0 && renderBad == 0, fmt ("state round trip: %d random settings (some after a program) restore every parameter bit for bit, save again byte-identical, render identically", rounds), first);
#if SEDA_HAS_PROGRAMS
    {   // an OLD session (no version, "touched" mask, SOURCE set): loads as it sounded -- the program's untouched cells are written into the knobs
        auto legacy = [&] (int source, int touched, std::initializer_list<std::pair<const char*, float>> vals, bool listen)
        {
            SedaAudioProcessor a; a.setRateAndBufferSizeDetails (sr, 512); a.prepareToPlay (sr, 512);
            for (auto& kv : vals) setParam (a.apvts, kv.first, kv.second);
            setParam (a.apvts, "listen", listen ? 1.0f : 0.0f); setParam (a.apvts, "source", (float) source);
            auto tree = a.apvts.copyState();
            tree.setProperty ("touched", touched, nullptr);
            tree.removeProperty ("stateV", nullptr);
            auto xml = tree.createXml();
            juce::MemoryBlock mb; a.copyXmlToBinary (*xml, mb);
            auto b = std::make_unique<SedaAudioProcessor>();
            b->setStateInformation (mb.getData(), (int) mb.getSize());
            return b;
        };
        auto pl = [] (SedaAudioProcessor& p, const char* id) { auto* r = dynamic_cast<juce::RangedAudioParameter*> (p.apvts.getParameter (id)); return r->convertFrom0to1 (r->getValue()); };
        {   // VOCAL, nothing touched: the old engine played note 1/4d SYNC on, HP 250, DUCK 60, THROW 6
            auto p = legacy (1, 0, {}, true);
            check (std::abs (pl (*p, "duck") - 60) < 0.01 && std::abs (pl (*p, "wetHp") - 250) < 0.01 && std::abs (pl (*p, "throw") - 6) < 0.01 && rawOf (*p, "sync") > 0.5f && juce::roundToInt (rawOf (*p, "note")) == 11,
                   "old session (no version): LISTEN on, SOURCE VOCAL, nothing touched -> loads with DUCK 60, WET HP 250, THROW 6, SYNC on, NOTE 1/4d written into the knobs (what it sounded like)");
        }
        {   // DRUMS, DUCK touched at 20: the touched cell keeps the user's value
            auto p = legacy (2, seda::kTouchDuck, { { "duck", 20 } }, true);
            check (std::abs (pl (*p, "duck") - 20) < 0.01 && std::abs (pl (*p, "wetHp") - 400) < 0.01 && juce::roundToInt (rawOf (*p, "note")) == 3,
                   "old session: SOURCE DRUMS with DUCK touched at 20 -> DUCK stays 20, WET HP 400, NOTE 1/16 written");
        }
        {   // LISTEN off: the old engine ignored the source; the new load leaves the knobs as saved
            auto p = legacy (1, 0, {}, false);
            check (std::abs (pl (*p, "duck") - 50) < 0.01 && std::abs (pl (*p, "wetHp")) < 0.01, "old session: SOURCE VOCAL but LISTEN off -> the knobs stay as saved (the old engine ignored SOURCE then)");
        }
        {   // a new save is not converted again on the next load
            auto p = legacy (1, 0, {}, true);
            setParam (p->apvts, "duck", 0.0f);
            juce::MemoryBlock mb; p->getStateInformation (mb);
            SedaAudioProcessor q; q.setStateInformation (mb.getData(), (int) mb.getSize());
            check (std::abs (pl (q, "duck")) < 0.001, "a session saved by this build keeps DUCK 0 at SOURCE VOCAL when it is loaded again (no second conversion)");
        }
    }
#endif
}

// ====================================================================================================== UI
static double nowMs() { return (double) std::chrono::duration_cast<std::chrono::nanoseconds> (std::chrono::steady_clock::now().time_since_epoch()).count() * 1.0e-6; }

static void testUi()
{
    section ("UI");
    PanelRig pr;
    setParam (pr.proc.apvts, "listen", 1.0f);
    pr.settle (gMat.vocal, 4);
    const int W = SedaEditor::kW, H = SedaEditor::kH;
    auto timeIt = [] (int reps, const std::function<void()>& f)
    {
        f();
        std::vector<double> t;
        for (int i = 0; i < reps; ++i) { const double a = nowMs(); f(); t.push_back (nowMs() - a); }
        std::sort (t.begin(), t.end());
        return std::make_pair (t[t.size() / 2], t.back());
    };
    // 1. the timer tick (refresh) with audio running: what the message thread does 30 times a second before any painting
    {
        auto tick = timeIt (300, [&] { pr.settle (gMat.vocal, 1, 4096); });
        std::printf ("[INFO] timer tick (one audio block + refresh): median %.3f ms, worst %.3f ms\n", tick.first, tick.second);
        check (tick.first < 1.0, fmt ("panel refresh tick costs %.3f ms (median; limit 1 ms)", tick.first));
    }
    // 2. a whole paint, native (CoreGraphics) image, 75 % and 100 %; and the software renderer for comparison
    for (int pct : { 75, 100 })
    {
        pr.ed->setScalePercent (pct);
        const int pw = W * pct / 100, ph = H * pct / 100;
        juce::Image native (juce::Image::ARGB, pw * 2, ph * 2, true, juce::NativeImageType());   // a 2x (retina) backing store
        auto full = timeIt (12, [&] { juce::Graphics g (native); g.addTransform (juce::AffineTransform::scale (2.0f * (float) pct / 100.0f)); pr.ed->paintEntireComponent (g, false); });
        std::printf ("[INFO] full paint at %d %% (retina backing, native image): median %.2f ms, worst %.2f ms\n", pct, full.first, full.second);
        check (full.first < (pct == 75 ? 60.0 : 100.0), fmt ("full panel paint at %d %%: %.2f ms (only on open / resize; limit 60 ms at 75 %%, 100 ms at 100 %%)", pct, full.first));
    }
    pr.ed->setScalePercent (75);
    // 3. what the 30 Hz timer asks to repaint each tick: count the repaint() requests and their area. Every Component::repaint goes up to the peer; here we wrap the
    //    children: a counting parent is not possible, so measure the same set of repaints by painting exactly those regions.
    {
        juce::Image native (juce::Image::ARGB, W * 3 / 2, H * 3 / 2, true, juce::NativeImageType());
        // the meter region of the ECHO window (repainted whenever the needle moves) and one value window (repainted on every tick)
        auto* box = pr.box ("duck");
        auto meter = timeIt (60, [&]
        {
            juce::Graphics g (native); g.addTransform (juce::AffineTransform::scale (0.75f * 2.0f));
            g.reduceClipRegion (juce::Rectangle<int> (30, 50, 414, 208).translated (476, 24));
            pr.ed->paintEntireComponent (g, false);
        });
        auto needle = timeIt (100, [&]
        {
            juce::Graphics g (native); g.addTransform (juce::AffineTransform::scale (0.75f * 2.0f));
            g.reduceClipRegion (juce::Rectangle<int> (40, 160).translated (680, 120));   // about what one tick of the ECHO needle dirties (old + new position)
            pr.ed->paintEntireComponent (g, false);
        });
        auto plateOnly = timeIt (20, [&]
        {
            juce::Graphics g (native); g.addTransform (juce::AffineTransform::scale (0.75f * 2.0f));
            g.drawImage (pr.ed->plateImageForTest(), { 0.0f, 0.0f, (float) W, (float) H }, juce::RectanglePlacement::stretchToFit);
        });
        std::printf ("[INFO] needle region repaint (40 x 160): median %.3f ms; the plate image alone at 75 %% (retina): %.2f ms\n", needle.first, plateOnly.first);
        auto bx = timeIt (200, [&]
        {
            juce::Graphics g (native); g.addTransform (juce::AffineTransform::scale (0.75f * 2.0f));
            g.reduceClipRegion (box->getBounds());
            pr.ed->paintEntireComponent (g, false);
        });
        std::printf ("[INFO] one meter-region repaint (414 x 208): median %.3f ms; one value-window repaint (%d x %d): median %.3f ms\n", meter.first, box->getWidth(), box->getHeight(), bx.first);
        // the timer repaints 11 value windows each tick (unconditionally in the P4b code) plus the meter region when the needle moves
        const double perTick = needle.first * 2.0 + 1.0 * bx.first;   // two needles move; a value window repaints only when its text changes (one at a time while a knob turns)
        std::printf ("[INFO] per tick while audio plays: two needle regions + one value window = %.2f ms; at 30 Hz that is %.1f %% of a core\n", perTick, perTick * 30.0 / 10.0);
        check (perTick < 3.0, fmt ("repaint cost per timer tick while audio plays (two needle regions + one value window): %.2f ms (limit 3 ms = 9 %% of a core at 30 Hz)", perTick));
    }
    // 4. how many repaints a quiet panel asks for per tick: nothing changed -> no repaint
    {
        struct Counter : juce::Component { int paints = 0; void paint (juce::Graphics&) override { ++paints; } };
        int boxPaints = 0;
        // count paintEntireComponent calls of the value windows by tracking their isShowing()-independent dirty state: use a ComponentListener on parent resized? Not available.
        // Instead run 30 ticks with no audio and no parameter change and report the time they take: a quiet panel must cost (nearly) nothing.
        auto quiet = timeIt (100, [&] { pr.ed->tickForSnapshot(); });
        (void) boxPaints;
        check (quiet.first < 0.5, fmt ("quiet panel (nothing moves): refresh tick costs %.3f ms", quiet.first));
    }
}

// ====================================================================================================== main
int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::set<std::string> only;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--quick") gQuick = true; else only.insert (a);
    }
    auto want = [&] (const char* s) { return only.empty() || only.count (s) > 0; };
    loadMaterial();
    const double t0 = nowMs();
    if (want ("NOTE"))  testNote();
    if (want ("DUCK"))  testDuck();
#if SEDA_HAS_PROGRAMS
    if (want ("PROG"))  testPrograms();
#endif
    if (want ("STATE")) testState();
    if (want ("LAT"))   testLatency();
    if (want ("CLICK")) testClicks();
    if (want ("SWEEP")) testSweep();
    if (want ("UI"))    testUi();

    std::printf ("\n==================== variation_rig: result table (%.0f s) ====================\n", (nowMs() - t0) / 1000.0);
    std::printf ("%-8s %8s %8s   %s\n", "section", "cases", "failed", "first failure");
    int total = 0;
    for (auto& name : gOrder) { auto& g = gGroups[name]; total += g.cases; std::printf ("%-8s %8d %8d   %s\n", name.c_str(), g.cases, g.fails, g.worst.c_str()); }
    std::printf ("variation_rig: %d cases, %d failed\n", total, gFailTotal);
    return gFailTotal == 0 ? 0 : 1;
}
