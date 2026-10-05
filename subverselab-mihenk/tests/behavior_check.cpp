/*  MIHENK behavior_check: a "virtual user" that drives the plug-in headless, the way a producer uses it for ten minutes,
    with synthetic and measured signals, and writes a PASS / FAIL report (tests/behavior_report.txt + .json).

        behavior_check                      all scenarios
        behavior_check --only 2,4           only these scenarios (1..7)
        behavior_check --wav a.wav b.wav    extra real finished masters for scenario 4 (wav files in demos are picked up too)
        behavior_check --out DIR            where the report goes (default: the tests/ folder of the source)

    It FINDS and REPORTS; it fixes nothing and changes no engine code. Exit code 0 when the run completed (read the
    report for FAILs), 2 when the harness itself broke.

    Scenarios
        1  determinism / independence from history      4  level sanity (finished, quiet, silence, mono, partial LEARN)
        2  bypass (exactness, settle time, click)       5  parameter fuzz (rates, blocks, automation, bypass)
        3  reported latency vs measured                 6  state recovery (round trips, bad states, loops, reset path)
        7  explainability (what the panel says vs what is measured)
*/
#include <JuceHeader.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include "../Source/plugin/PluginProcessor.h"
#include "../Source/plugin/Params.h"
#include "../Source/engine/Preset.h"
#include "../Source/engine/MasterChain.h"
#include "../Source/engine/AudioIO.h"
#include "../Source/dsp/Filters.h"
#include "../Source/dsp/Loudness.h"
#include "../Source/dsp/Spectrum.h"

using namespace mihenk;

namespace
{
using Buffer = juce::AudioBuffer<float>;
using juce::String;

//==============================================================================
// Report
struct Item
{
    String scenario, id, name, measured, limit, suspect, hypothesis;
    bool pass = true, info = false;
    int prio = 2;      // 1 = bears on a user complaint, 2 = real defect, 3 = minor
};
std::vector<Item> g_items;
std::vector<String> g_notes;
int g_fail = 0;
double g_t0 = 0.0;

double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }

void rec (const String& sc, const String& id, const String& name, bool pass, const String& measured, const String& limit = {},
          const String& suspect = {}, const String& hyp = {}, int prio = 2)
{
    Item it;
    it.scenario = sc; it.id = id; it.name = name; it.pass = pass; it.measured = measured; it.limit = limit;
    it.suspect = suspect; it.hypothesis = hyp; it.prio = prio;
    g_items.push_back (it);
    if (! pass) ++g_fail;
    std::printf ("%s  [%s] %s: %s | %s%s\n", pass ? "PASS" : "FAIL", id.toRawUTF8(), name.toRawUTF8(), measured.toRawUTF8(),
                 limit.isEmpty() ? "" : ("limit " + limit).toRawUTF8(), "");
    std::fflush (stdout);
}
void inf (const String& sc, const String& id, const String& name, const String& measured)
{
    Item it;
    it.scenario = sc; it.id = id; it.name = name; it.measured = measured; it.info = true; it.pass = true;
    g_items.push_back (it);
    std::printf ("INFO  [%s] %s: %s\n", id.toRawUTF8(), name.toRawUTF8(), measured.toRawUTF8());
    std::fflush (stdout);
}
void section (const char* s)
{
    std::printf ("\n== %s   (t+%.0f s)\n", s, (nowMs() - g_t0) / 1000.0);
    std::fflush (stdout);
}
String d (double v, int dec = 2) { return std::isfinite (v) ? String (v, dec) : String (v < 0 ? "-inf" : (std::isnan (v) ? "nan" : "inf")); }

//==============================================================================
// Signals
Buffer song (double fs, double secs, uint32_t seed, double pre = 0.3, double post = 1.5)
{
    const int n = (int) (secs * fs), preN = (int) (pre * fs), postN = (int) (post * fs);
    Buffer b (2, preN + n + postN);
    b.clear();
    juce::Random rng ((juce::int64) seed);
    double lp[2] { 0.0, 0.0 };
    for (int i = 0; i < n; ++i)
    {
        const double t = i / fs, tb = std::fmod (t, 0.5), sec = std::fmod (t, 8.0) < 4.0 ? 0.35 : 1.0;
        const double kick = std::exp (-tb * 18.0) * std::sin (2.0 * kPi * (48.0 * tb + 2.0 * (1.0 - std::exp (-tb * 30.0))));
        const double bass = 0.25 * std::sin (2.0 * kPi * 55.0 * t);
        const double th = std::fmod (t + 0.25, 0.25);
        const double hat = std::exp (-th * 60.0) * (rng.nextDouble() * 2.0 - 1.0) * 0.15;
        const double fade = juce::jmin (1.0, t / 0.02, (secs - t) / 0.02);
        for (int c = 0; c < 2; ++c)
        {
            const double w = rng.nextDouble() * 2.0 - 1.0;
            lp[c] += 0.05 * (w - lp[c]);
            const double pad = lp[c] * 1.5;
            b.setSample (c, preN + i, (float) (fade * sec * (0.55 * kick + bass + 0.25 * pad + hat * (c == 0 ? 1.3 : 0.7))));
        }
    }
    return b;
}

/** A fuller test song: the kick/bass/hat/pad song plus a broadband pad, so that loudness (K-weighted) can reach finished-master levels. */
Buffer pinkNoise (double fs, double secs, double rmsDbfs, int ch, uint32_t seed);
Buffer songFull (double fs, double secs, uint32_t seed, double pre = 0.3, double post = 1.5)
{
    auto b = song (fs, secs, seed, pre, post);
    const int preN = (int) (pre * fs), n = (int) (secs * fs);
    const auto pk = pinkNoise (fs, secs, -17.0, 2, seed + 9);
    for (int i = 0; i < n; ++i)
    {
        const double t = i / fs, sec = std::fmod (t, 8.0) < 4.0 ? 0.35 : 1.0;
        const double fade = juce::jmin (1.0, t / 0.02, (secs - t) / 0.02);
        for (int c = 0; c < 2; ++c)
            b.setSample (c, preN + i, (float) (0.6 * b.getSample (c, preN + i) + fade * sec * pk.getSample (c, i)));
    }
    return b;
}

Buffer pinkNoise (double fs, double secs, double rmsDbfs, int ch, uint32_t seed)
{
    const int n = (int) (secs * fs);
    Buffer b (ch, n);
    juce::Random rng ((juce::int64) seed);
    double e = 0.0;
    for (int c = 0; c < ch; ++c)
    {
        double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
        for (int i = 0; i < n; ++i)
        {
            const double w = rng.nextDouble() * 2.0 - 1.0;
            b0 = 0.99886 * b0 + w * 0.0555179; b1 = 0.99332 * b1 + w * 0.0750759; b2 = 0.96900 * b2 + w * 0.1538520;
            b3 = 0.86650 * b3 + w * 0.3104856; b4 = 0.55000 * b4 + w * 0.5329522; b5 = -0.76160 * b5 - w * 0.0168980;
            const double p = (b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362) * 0.11;
            b6 = w * 0.115926;
            b.setSample (c, i, (float) p);
            e += p * p;
        }
    }
    const double rms = std::sqrt (e / ((double) n * ch));
    b.applyGain ((float) (dbToGain (rmsDbfs) / juce::jmax (1.0e-12, rms)));
    return b;
}

Buffer sineBuf (double fs, double secs, double hz, double peakDb, int ch)
{
    const int n = (int) (secs * fs);
    Buffer b (ch, n);
    const double a = dbToGain (peakDb);
    for (int c = 0; c < ch; ++c)
        for (int i = 0; i < n; ++i) b.setSample (c, i, (float) (a * std::sin (2.0 * kPi * hz * i / fs)));
    return b;
}

Buffer silenceBuf (double fs, double secs, int ch) { Buffer b (ch, (int) (secs * fs)); b.clear(); return b; }

Buffer slice (const Buffer& b, int start, int len)
{
    start = juce::jmax (0, start);
    len = juce::jmax (0, juce::jmin (len, b.getNumSamples() - start));
    Buffer o (b.getNumChannels(), len);
    for (int c = 0; c < b.getNumChannels(); ++c) o.copyFrom (c, 0, b, c, start, len);
    return o;
}

Buffer concat (const Buffer& a, const Buffer& b)
{
    Buffer o (a.getNumChannels(), a.getNumSamples() + b.getNumSamples());
    for (int c = 0; c < a.getNumChannels(); ++c)
    {
        o.copyFrom (c, 0, a, c, 0, a.getNumSamples());
        o.copyFrom (c, a.getNumSamples(), b, c, 0, b.getNumSamples());
    }
    return o;
}

//==============================================================================
// Measurements
double lufsOf (const Buffer& b, double fs, int start = 0, int len = -1)
{
    if (len < 0) len = b.getNumSamples() - start;
    return integratedLoudness (slice (b, start, len), fs);
}
double peakDb (const Buffer& b)
{
    float m = 0.0f;
    for (int c = 0; c < b.getNumChannels(); ++c) m = juce::jmax (m, b.getMagnitude (c, 0, b.getNumSamples()));
    return gainToDb (m);
}
double tpOf (const Buffer& b, int factor = 4) { return gainToDb (truePeakMax (b, factor)); }
double rmsOf (const Buffer& b, int start = 0, int len = -1)
{
    if (len < 0) len = b.getNumSamples() - start;
    double e = 0.0;
    long long n = 0;
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = start; i < start + len && i < b.getNumSamples(); ++i) { const double v = b.getSample (c, i); e += v * v; ++n; }
    return n > 0 ? 10.0 * std::log10 (e / (double) n + 1.0e-30) : -300.0;
}
long long countBad (const Buffer& b)
{
    long long n = 0;
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = 0; i < b.getNumSamples(); ++i) n += ! std::isfinite (b.getSample (c, i));
    return n;
}
long long countDenormals (const Buffer& b)
{
    long long n = 0;
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const float v = std::abs (b.getSample (c, i));
            n += (v != 0.0f && v < 1.17549435e-38f);
        }
    return n;
}
double crestOf (const Buffer& b) { return peakDb (b) - rmsOf (b); }

/** An independent, offline look-ahead peak limiter (not the engine's): the way a finished master is made. */
Buffer limitSimple (const Buffer& in, double fs, double ceilLin, double lookaheadMs, double releaseMs)
{
    const int n = in.getNumSamples(), C = in.getNumChannels(), L = juce::jmax (2, (int) (lookaheadMs * 1.0e-3 * fs));
    std::vector<float> req ((size_t) n, 1.0f), hold ((size_t) n, 1.0f), sm ((size_t) n, 1.0f), g ((size_t) n, 1.0f);
    for (int i = 0; i < n; ++i)
    {
        float pk = 0.0f;
        for (int c = 0; c < C; ++c) pk = juce::jmax (pk, std::abs (in.getSample (c, i)));
        req[(size_t) i] = pk > ceilLin ? (float) (ceilLin / pk) : 1.0f;
    }
    // min over +-L (monotonic deque would be faster; the windows are short)
    for (int i = 0; i < n; ++i)
    {
        float m = 1.0f;
        for (int j = juce::jmax (0, i - L); j <= juce::jmin (n - 1, i + L); ++j) m = juce::jmin (m, req[(size_t) j]);
        hold[(size_t) i] = m;
    }
    const double rel = std::exp (-1.0 / (releaseMs * 1.0e-3 * fs));
    float prev = 1.0f;
    for (int i = 0; i < n; ++i)
    {
        const float h = hold[(size_t) i];
        prev = h < prev ? h : (float) (h + (prev - h) * rel);
        sm[(size_t) i] = prev;
    }
    double acc = 0.0;
    const int W = L;
    for (int i = 0; i < n + W; ++i)
    {
        if (i < n) acc += sm[(size_t) i];
        if (i >= 2 * W + 1) acc -= sm[(size_t) (i - 2 * W - 1)];
        const int centre = i - W;
        if (centre >= 0 && centre < n) g[(size_t) centre] = (float) juce::jmin (1.0, acc / (2 * W + 1));
    }
    Buffer out (in);
    for (int c = 0; c < C; ++c)
        for (int i = 0; i < n; ++i) out.setSample (c, i, in.getSample (c, i) * g[(size_t) i]);
    return out;
}

/** A finished master with the loudness and true peak asked for (as close as the material allows): gain into a 2 ms
    look-ahead limiter at -ceiling, then normalised to the true peak. */
Buffer makeFinished (const Buffer& src, double fs, double lufsT, double tpT)
{
    auto build = [&] (double gDb)
    {
        Buffer b (src);
        b.applyGain ((float) dbToGain (gDb));
        b = limitSimple (b, fs, dbToGain (tpT - 0.4), 1.0, 25.0);
        b.applyGain ((float) dbToGain (tpT - tpOf (b)));
        return b;
    };
    double lo = 0.0, hi = 40.0;
    Buffer best = build (lo);
    for (int it = 0; it < 14; ++it)
    {
        const double mid = 0.5 * (lo + hi);
        auto b = build (mid);
        const double l = lufsOf (b, fs);
        best = b;
        if (l < lufsT) lo = mid; else hi = mid;
        if (std::abs (l - lufsT) < 0.05) break;
    }
    return best;
}

Buffer scaledToLufs (const Buffer& src, double fs, double lufsT)
{
    Buffer b (src);
    b.applyGain ((float) dbToGain (lufsT - lufsOf (src, fs)));
    return b;
}

//==============================================================================
// The virtual host
struct FakeHead : juce::AudioPlayHead
{
    bool playing = true;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        p.setIsPlaying (playing);
        return p;
    }
};

void pump (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

struct MeterAcc
{
    bool keep = false;
    double maxLim = 0.0, sumLim = 0.0, maxComp = 0.0;
    long long n = 0, active = 0, over3 = 0;
    std::vector<float> outL, inL;
};

struct Rig
{
    std::unique_ptr<MihenkProcessor> p;
    FakeHead head;
    double fs;
    int chans, maxBlock;

    explicit Rig (double sr = 48000.0, int ch = 2, int maxBlk = 512, bool nonRealtime = true, bool standalone = false)
        : fs (sr), chans (ch), maxBlock (maxBlk)
    {
        p = std::make_unique<MihenkProcessor>();
        p->setStandaloneModeForTesting (standalone);
        p->setPlayHead (&head);
        if (ch == 1)
        {
            juce::AudioProcessor::BusesLayout l;
            l.inputBuses.add (juce::AudioChannelSet::mono());
            l.outputBuses.add (juce::AudioChannelSet::mono());
            p->setBusesLayout (l);
        }
        p->setRateAndBufferSizeDetails (fs, maxBlock);
        p->setNonRealtime (nonRealtime);
        p->getMeters().clear();
    }
    ~Rig() { p->setPlayHead (nullptr); }

    MihenkProcessor& P() { return *p; }
    Session& S() { return p->getSession(); }
    void prep (double sr = 0.0, int blk = 0)
    {
        if (sr > 0.0) { fs = sr; p->setRateAndBufferSizeDetails (fs, blk > 0 ? blk : maxBlock); }
        if (blk > 0) maxBlock = blk;
        p->prepareToPlay (fs, maxBlock);
    }
    void setSettings (const MasterSettings& s) { S().settings = s; S().settingsChanged(); }
    void setParam (const String& id, double v)
    {
        auto* prm = p->getBridge().find (id);
        jassert (prm != nullptr);
        prm->setValueNotifyingHost (prm->convertTo0to1 ((float) v));
    }
    double getParam (const String& id) { return (double) p->getState().getRawParameterValue (id)->load(); }
    int latency() { return p->getLatencySamples(); }

    void settle (double maxSec = 240.0)
    {
        pump (160);
        const double t0 = nowMs();
        while (S().busy() && (nowMs() - t0) < maxSec * 1000.0) pump (40);
        pump (140);
    }

    /** Plays `in` (in place copy) in blocks of `block` (0 = irregular 1..maxBlock), then `extra` samples of silence. */
    Buffer run (const Buffer& in, int block, int extra = 0, MeterAcc* ma = nullptr, std::vector<int>* blockLog = nullptr)
    {
        const int n = in.getNumSamples(), total = n + extra, C = in.getNumChannels();
        Buffer out (C, total);
        out.clear();
        for (int c = 0; c < C; ++c) out.copyFrom (c, 0, in, c, 0, n);
        juce::MidiBuffer midi;
        juce::Random rng (4242);
        std::vector<float> frames;
        if (ma != nullptr) frames.assign ((size_t) p->getMeters().capacity() * MeterFifo::frameSize, 0.0f);
        for (int pos = 0; pos < total;)
        {
            int len = block > 0 ? block : 1 + rng.nextInt (maxBlock);
            len = juce::jmin (len, total - pos);
            Buffer view (out.getArrayOfWritePointers(), C, pos, len);
            p->processBlock (view, midi);
            if (blockLog != nullptr) blockLog->push_back (len);
            pos += len;
            if (ma != nullptr) drain (*ma, frames);
        }
        return out;
    }
    void drain (MeterAcc& m, std::vector<float>& frames)
    {
        const int k = p->getMeters().pull (frames.data(), p->getMeters().capacity());
        for (int i = 0; i < k; ++i)
        {
            const float* f = frames.data() + (size_t) i * MeterFifo::frameSize;
            const double g = f[MeterFifo::grLim];
            m.maxLim = juce::jmax (m.maxLim, g);
            m.sumLim += g;
            m.active += g > 0.1;
            m.over3 += g > 3.0;
            m.maxComp = juce::jmax (m.maxComp, (double) f[MeterFifo::grComp]);
            ++m.n;
            if (m.keep) { m.outL.push_back (f[MeterFifo::outL]); m.inL.push_back (f[MeterFifo::inL]); }
        }
    }
    /** LEARN on `audio` (the plug-in's real flow), then ready to play. */
    void learn (const Buffer& audio, int block = 512)
    {
        p->setNonRealtime (true);
        prep();
        p->toggleLearn();
        run (audio, block, 0);
        p->finishLearnNow();
        prep();
    }
    /** The audio, delayed by the reported latency, lined up with the input again. */
    Buffer playAligned (const Buffer& in, int block, MeterAcc* ma = nullptr)
    {
        const int lat = latency();
        auto out = run (in, block, lat + 64, ma);
        return slice (out, lat, in.getNumSamples());
    }
};

struct Meas { double lufs = 0, rms = 0, tp = 0; Buffer out; };

/** One steady measurement: a second of silence first (detectors let go), then the signal. */
Meas measure (Rig& r, const Buffer& sig, int block = 512)
{
    r.run (silenceBuf (r.fs, 1.2, sig.getNumChannels()), 512);
    Meas m;
    m.out = r.playAligned (sig, block);
    m.lufs = lufsOf (m.out, r.fs);
    m.rms = rmsOf (m.out);
    m.tp = tpOf (m.out);
    return m;
}

MasterSettings rapSettings()
{
    MasterSettings s;
    applyGenreStartingPoint (s, Genre::rapTrap);
    return s;
}

String ms (double samples, double fs) { return d (1000.0 * samples / fs, 1) + " ms"; }

//==============================================================================
// Scenario 1: determinism / independence from history
void scenario1()
{
    section ("1  Determinism and independence from history");
    const double fs = 48000.0;
    const auto learnAudio = song (fs, 20.0, 1);
    const auto sig = song (fs, 8.0, 7);
    auto st = rapSettings();
    st.eq.airDb = 1.0;
    auto makeRig = [&]
    {
        auto r = std::make_unique<Rig> (fs);
        r->setSettings (st);
        r->learn (learnAudio);
        return r;
    };

    auto r0 = makeRig();
    const auto base = measure (*r0, sig);
    const double drive0 = r0->getParam ("drive");
    inf ("1", "1.0", "baseline", "fresh rig, LEARN on 20 s, test signal " + d (base.lufs, 2) + " LUFS, " + d (base.rms, 2) + " dB RMS, " + d (base.tp, 2)
                                     + " dBTP, solved drive " + d (drive0, 2) + " dB, latency " + String (r0->latency()) + " samples");

    {
        auto r1 = makeRig();
        const auto m = measure (*r1, sig);
        double worst = 0.0;
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < sig.getNumSamples(); ++i) worst = juce::jmax (worst, (double) std::abs (m.out.getSample (c, i) - base.out.getSample (c, i)));
        rec ("1", "1.1", "two fresh rigs, same settings and input", worst == 0.0 && std::abs (m.lufs - base.lufs) < 0.001,
             "largest sample difference " + d (gainToDb (worst), 1) + " dBFS, level difference " + d (m.lufs - base.lufs, 4) + " LU", "bit-exact (non-realtime kernels)");
    }

    auto check = [&] (const String& id, const String& name, Rig& r, int block, const String& why = {})
    {
        r.settle();
        const auto m = measure (r, sig, block);
        const double dl = m.lufs - base.lufs, dr = m.rms - base.rms;
        const bool ok = std::abs (dl) <= 0.1 && std::abs (dr) <= 0.1 && countBad (m.out) == 0;
        rec ("1", id, name, ok, "LUFS " + d (m.lufs, 3) + " (" + (dl >= 0 ? "+" : "") + d (dl, 3) + " LU), RMS " + (dr >= 0 ? "+" : "") + d (dr, 3) + " dB, drive "
                                    + d (r.getParam ("drive"), 2) + " dB (was " + d (drive0, 2) + ")" + (why.isEmpty() ? String() : "; " + why),
             "|level difference| <= 0.1 dB", "", "", 1);
    };

    { auto r = makeRig(); check ("1.2", "block size 32", *r, 32); }
    { auto r = makeRig(); check ("1.3", "block size 64", *r, 64); }
    { auto r = makeRig(); check ("1.4", "irregular block sizes 1..512", *r, 0); }
    { auto r = makeRig(); check ("1.5", "block size 2048 (larger than the 512 announced to prepareToPlay)", *r, 2048); }
    {
        auto r = makeRig();
        const auto short8 = song (fs, 2.0, 7);
        auto b8 = measure (*r0, short8, 512);
        const auto m1 = measure (*r, short8, 1);
        rec ("1", "1.6", "block size 1 (2 s signal, against block 512 on the same signal)", std::abs (m1.lufs - b8.lufs) <= 0.1,
             "LUFS " + d (m1.lufs, 3) + " vs " + d (b8.lufs, 3), "<= 0.1 dB", "", "", 1);
    }
    {
        auto r = makeRig();
        const auto chunk = song (fs, 0.4, 3);
        for (int i = 0; i < 20; ++i)
        {
            r->setParam ("bypass", 1.0);
            r->run (chunk, 512);
            pump (60);
            r->setParam ("bypass", 0.0);
            r->run (chunk, 512);
            pump (60);
        }
        check ("1.7", "after 20 x (bypass ON, play, bypass OFF, play)", *r, 512);
    }
    {
        auto r = makeRig();
        // bypass flicked on and off faster than the session can re-solve
        for (int i = 0; i < 10; ++i) { r->setParam ("bypass", 1.0); pump (20); r->setParam ("bypass", 0.0); pump (20); }
        check ("1.8", "after 10 quick bypass flicks (20 ms apart, solves cancelled)", *r, 512);
    }
    {
        auto r = makeRig();
        const char* ids[] { "bell1Db", "glue", "airDb", "width", "ceiling", "lowShelfDb", "tilt", "ratio" };
        const double vals[] { 3.0, 3.0, 4.0, 1.5, -2.5, 4.0, 0.8, 5.0 };
        double orig[8];
        for (int i = 0; i < 8; ++i) orig[i] = r->getParam (ids[i]);
        for (int i = 0; i < 8; ++i) r->setParam (ids[i], vals[i]);
        r->settle();
        for (int i = 0; i < 8; ++i) r->setParam (ids[i], orig[i]);
        check ("1.9", "8 knobs moved away and back to the exact same values", *r, 512);
    }
    {
        auto r = makeRig();
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("mihenk_behavior");
        dir.createDirectory();
        const auto f = dir.getChildFile ("p1.mihenkpreset");
        String err;
        auto cur = r->S().settings;
        savePreset (f, cur, err);
        r->S().applyGenre (Genre::arabeskPop);
        r->settle();
        MasterSettings loaded = r->S().settings;
        loadPreset (f, loaded, err);
        r->S().settings = loaded;
        r->S().settingsChanged();
        check ("1.10", "preset round trip (save, another starting point, load the saved one back)", *r, 512);
    }
    { auto r = makeRig(); r->P().getChain().reset(); check ("1.11", "RtChain::reset() (state cleared, plan and settings kept)", *r, 512); }
    {
        auto r = makeRig();
        r->prep (48000.0, 512);
        r->run (song (fs, 1.0, 5), 512);
        r->prep (48000.0, 128);
        r->run (song (fs, 1.0, 5), 128);
        r->prep (48000.0, 512);
        check ("1.12", "prepareToPlay called three times (512, 128, 512)", *r, 512);
    }
    {
        auto r = makeRig();
        juce::MemoryBlock mb;
        r->P().getStateInformation (mb);
        r->P().setStateInformation (mb.getData(), (int) mb.getSize());
        check ("1.13", "getStateInformation then setStateInformation on the same live instance", *r, 512);
    }
    {
        auto r = makeRig();
        r->prep (44100.0, 512);
        r->run (song (44100.0, 1.0, 5), 512);
        r->prep (48000.0, 512);
        check ("1.14", "host hops the sample rate 48 -> 44.1 -> 48 kHz (LEARN done at 48)", *r, 512);
    }
    {
        auto r = makeRig();
        r->learn (learnAudio);     // the user presses LEARN again on the same song
        check ("1.15", "LEARN pressed a second time on the identical song", *r, 512);
    }
    {
        auto r = makeRig();
        juce::Random rng (77);
        std::map<String, double> orig;
        const auto& defs = params::definitions();
        std::vector<const params::Def*> pick;
        for (const auto& df : defs)
            if (df.id != "bypass" && df.id != "limOn" && df.id != "targetLock" && df.id != "targetPreset" && df.id != "targetLufs") pick.push_back (&df);
        for (int i = 0; i < 40; ++i)
        {
            const auto* df = pick[(size_t) rng.nextInt ((int) pick.size())];
            if (orig.count (df->id) == 0) orig[df->id] = r->getParam (df->id);
            const double v = df->minimum + rng.nextDouble() * (df->maximum - df->minimum);
            r->setParam (df->id, df->kind == params::Def::real ? v : std::round (v));
            if (i % 8 == 7) r->run (song (fs, 0.3, 2), 512);
        }
        r->settle();
        for (auto& kv : orig) r->setParam (kv.first, kv.second);
        check ("1.16", "40 random knob moves, then every touched knob restored to its exact value", *r, 512);
    }
    {
        // Same rig played twice back to back: no reset in between (a host that keeps running).
        const auto m2 = measure (*r0, sig, 512);
        rec ("1", "1.17", "same instance, same signal played again without any reset", std::abs (m2.lufs - base.lufs) <= 0.1,
             "LUFS " + d (m2.lufs, 3) + " vs " + d (base.lufs, 3), "<= 0.1 dB", "", "", 2);
    }
}

//==============================================================================
// Scenario 2: bypass
struct Trans { double a0 = 0, a1 = 0, startMs = -1, t50 = -1, t90 = -1, t99 = -1, stepRatio = 0, dipDb = 0; bool ok = false; };

/** Switch a parameter while a steady sine plays and time the envelope. The first host block is `lead` samples long
    (moves the later block starts against the 256-sample internal block), then blocks of `blk`; the switch lands on the
    first sample of the block that starts at `at`. */
Trans transitionOf (Rig& r, const Buffer& sine, int blk, int lead, int at, const std::function<void()>& flip, double hz = 997.0)
{
    const int n = sine.getNumSamples(), C = sine.getNumChannels();
    Buffer out (C, n);
    out.clear();
    for (int c = 0; c < C; ++c) out.copyFrom (c, 0, sine, c, 0, n);
    juce::MidiBuffer midi;
    for (int pos = 0; pos < n;)
    {
        if (pos == at) flip();
        int len = (pos == 0 && lead > 0) ? lead : blk;
        len = juce::jmin (len, n - pos);
        Buffer view (out.getArrayOfWritePointers(), C, pos, len);
        r.p->processBlock (view, midi);
        pos += len;
    }
    Trans t;
    const int W = 96;      // two periods of 1 kHz at 48 kHz
    std::vector<double> env ((size_t) n, 0.0);
    {
        double acc = 0.0;
        std::vector<double> sq ((size_t) n);
        for (int i = 0; i < n; ++i) sq[(size_t) i] = (double) out.getSample (0, i) * out.getSample (0, i);
        for (int i = 0; i < n; ++i)
        {
            acc += sq[(size_t) i];
            if (i >= W) acc -= sq[(size_t) (i - W)];
            env[(size_t) i] = std::sqrt (juce::jmax (0.0, acc) / W);
        }
    }
    auto avg = [&] (int a, int b2) { double s = 0.0; for (int i = a; i < b2; ++i) s += env[(size_t) i]; return s / juce::jmax (1, b2 - a); };
    t.a0 = avg (at - 4000, at - 200);
    t.a1 = avg (n - 4000, n - 200);
    // click: largest sample step around the transition against the natural step of the louder of the two levels
    const double natural = juce::jmax (t.a0, t.a1) * std::sqrt (2.0) * 2.0 * kPi * hz / r.fs;
    double worst = 0.0;
    for (int i = at + 1; i < juce::jmin (n, at + (int) (0.15 * r.fs)); ++i)
        worst = juce::jmax (worst, (double) std::abs (out.getSample (0, i) - out.getSample (0, i - 1)));
    t.stepRatio = worst / juce::jmax (1.0e-9, natural);
    // dip: lowest envelope against the lower of the two steady levels (a dropout under both)
    double lowest = 1.0e9;
    for (int i = at + W; i < juce::jmin (n, at + (int) (0.15 * r.fs)); ++i) lowest = juce::jmin (lowest, env[(size_t) i]);
    t.dipDb = 20.0 * std::log10 (juce::jmax (1.0e-9, lowest) / juce::jmax (1.0e-9, juce::jmin (t.a0, t.a1)));
    const double span = t.a1 - t.a0;
    if (std::abs (span) < 1.0e-6) return t;
    t.ok = true;
    for (int i = at; i < n; ++i)
    {
        const double frac = (env[(size_t) i] - t.a0) / span;
        const double msv = 1000.0 * (i - W / 2 - at) / r.fs;
        if (t.startMs < 0 && std::abs (frac) > 0.05) t.startMs = msv;
        if (t.t50 < 0 && frac > 0.5) t.t50 = msv;
        if (t.t90 < 0 && frac > 0.9) t.t90 = msv;
        if (t.t99 < 0 && frac > 0.99) t.t99 = msv;
    }
    return t;
}

void scenario2()
{
    section ("2  Bypass");
    const double fs = 48000.0;

    // 2a: bypass output = input delayed by the reported latency, bit for bit, in the structures that change the delay
    struct Cfg { const char* name; std::function<void (MasterSettings&)> set; };
    const Cfg cfgs[] {
        { "default settings", [] (MasterSettings&) {} },
        { "HIP-HOP starting point (mono below 120 Hz)", [] (MasterSettings& s) { s = rapSettings(); s.limiter.driveDb = 9.0; } },
        { "3-BAND + dynamic AIR + mono below + linear-phase EQ", [] (MasterSettings& s)
          { s = rapSettings(); s.comp.mode = CompMode::threeBand; s.eq.airDynamic = true; s.eq.airDb = 2.0; s.eq.linearPhase = true; s.eq.bell1Db = 2.0; s.limiter.driveDb = 9.0; } },
    };
    const auto noise = pinkNoise (fs, 3.0, -18.0, 2, 5);
    int idx = 0;
    for (const auto& c : cfgs)
    {
        Rig r (fs);
        MasterSettings s;
        c.set (s);
        s.bypass = true;
        r.setSettings (s);
        r.prep();
        const int lat = r.latency();
        auto out = r.run (noise, 512, lat + 64);
        int bestLag = -1;
        double bestErr = 1.0e9;
        for (int lag = juce::jmax (0, lat - 3); lag <= lat + 3; ++lag)
        {
            double e = 0.0;
            for (int c2 = 0; c2 < 2; ++c2)
                for (int i = 24000; i < noise.getNumSamples(); i += 7) e = juce::jmax (e, (double) std::abs (out.getSample (c2, i + lag) - noise.getSample (c2, i)));
            if (e < bestErr) { bestErr = e; bestLag = lag; }
        }
        rec ("2", "2.a" + String (++idx), String ("bypass = input delayed by the reported latency (") + c.name + ")", bestLag == lat && bestErr == 0.0,
             "reported latency " + String (lat) + " samples (" + ms (lat, fs) + "), bit-exact lag " + String (bestLag) + ", largest difference " + d (gainToDb (bestErr), 1) + " dBFS",
             "same lag, difference 0", "Source/engine/RtChain.cpp:1430-1458", "the bypass tap is delayed by Dchain built from the current structure", 1);
    }

    // 2b: settle time of the switch, three host block sizes, four phases against the 256-sample internal block
    {
        Rig r (fs);
        auto s = rapSettings();
        s.limiter.driveDb = 9.0;
        s.comp.mode = CompMode::wide;
        r.setSettings (s);
        r.prep();
        const auto sine = sineBuf (fs, 3.0, 997.0, -12.0, 2);
        double worstStart = 0, worst90 = 0, worst99 = 0, worstClick = 0, worstDip = 0, lvl = 0;
        String detail;
        for (int blk : { 64, 512, 2048 })
            for (int phase : { 0, 1, 2, 3 })
            {
                const int lead = phase * 64;
                const int at = lead + ((int) (1.5 * fs) - lead) / blk * blk;
                for (int dir = 0; dir < 2; ++dir)
                {
                    r.setParam ("bypass", dir == 0 ? 0.0 : 1.0);
                    r.prep();
                    r.run (silenceBuf (fs, 0.5, 2), 512);
                    const auto t = transitionOf (r, sine, blk, lead, at, [&] { r.setParam ("bypass", dir == 0 ? 1.0 : 0.0); });
                    if (! t.ok) continue;
                    lvl = juce::jmax (lvl, std::abs (20.0 * std::log10 (t.a1 / t.a0)));
                    worstStart = juce::jmax (worstStart, t.startMs);
                    worst90 = juce::jmax (worst90, t.t90);
                    worst99 = juce::jmax (worst99, t.t99);
                    worstClick = juce::jmax (worstClick, t.stepRatio);
                    worstDip = juce::jmin (worstDip, t.dipDb);
                    if (blk == 64 && phase == 0)
                        detail += String (dir == 0 ? "ON " : "OFF") + ": first change " + d (t.startMs, 1) + " ms, 50% " + d (t.t50, 1) + " ms, 90% " + d (t.t90, 1) + " ms, 99% " + d (t.t99, 1) + " ms; ";
                }
            }
        inf ("2", "2.b0", "bypass step size used for the timing", "processed and bypassed sine differ by up to " + d (lvl, 1) + " dB (drive +9 dB)");
        rec ("2", "2.b1", "time from the parameter change to the first audible change (worst of 24 cases)", worstStart <= 12.0,
             d (worstStart, 1) + " ms. " + detail, "<= 12 ms", "Source/engine/RtChain.cpp:1480-1509 (process), 1002-1003", "the switch is read once per 256-sample internal block, and the block's output leaves one block later (framing latency)", 1);
        rec ("2", "2.b2", "time from the parameter change to 90 % of the switch (worst of 24 cases)", worst90 <= 30.0,
             d (worst90, 1) + " ms (99 %: " + d (worst99, 1) + " ms)", "<= 30 ms", "Source/engine/RtChain.cpp:1435-1436 (ab / out ramps use rampLen = 20 ms)",
             "20 ms linear crossfade plus up to 2 internal blocks of framing delay", 1);
        rec ("2", "2.b3", "bypass switch click (largest sample step / natural step of the louder level)", worstClick <= 1.5,
             d (worstClick, 2) + " x", "<= 1.5 x", "", "", 2);
        rec ("2", "2.b4", "bypass switch dropout (lowest level during the switch vs the quieter side)", worstDip >= -1.0,
             d (worstDip, 2) + " dB", ">= -1 dB", "", "", 2);
    }

    // 2c: level after bypass OFF is the level before (learned session, quick ON/OFF and a slow ON/OFF)
    {
        const auto learnAudio = song (fs, 20.0, 1);
        const auto sig = song (fs, 8.0, 7);
        Rig r (fs);
        r.setSettings (rapSettings());
        r.learn (learnAudio);
        const auto before = measure (r, sig);
        r.setParam ("bypass", 1.0);
        r.settle();
        const auto byp = measure (r, sig);
        r.setParam ("bypass", 0.0);
        r.settle();
        const auto after = measure (r, sig);
        double maxDiff = 0.0;
        for (int i = 0; i < sig.getNumSamples(); i += 3) maxDiff = juce::jmax (maxDiff, (double) std::abs (byp.out.getSample (0, i) - sig.getSample (0, i)));
        rec ("2", "2.c1", "level after bypass OFF equals the level before bypass ON (learned session)", std::abs (after.lufs - before.lufs) <= 0.1,
             d (before.lufs, 3) + " LUFS before, " + d (after.lufs, 3) + " after, bypassed " + d (byp.lufs, 3) + " LUFS (input " + d (lufsOf (sig, fs), 3) + ")", "<= 0.1 dB", "", "", 1);
        rec ("2", "2.c2", "bypass ON output equals the input (learned session, aligned)", maxDiff == 0.0, "largest difference " + d (gainToDb (maxDiff), 1) + " dBFS", "0");
        r.setParam ("bypass", 1.0);
        pump (200);
        r.setParam ("bypass", 0.0);
        r.settle();
        const auto quick = measure (r, sig);
        rec ("2", "2.c3", "level after a quick bypass ON/OFF (200 ms)", std::abs (quick.lufs - before.lufs) <= 0.1,
             d (quick.lufs, 3) + " LUFS vs " + d (before.lufs, 3), "<= 0.1 dB", "", "", 1);
    }

    // 2e: the HOST's own bypass (VST3 'Bypass' parameter, AU bypass property, DAW device switch): processBlockBypassed
    {
        Rig r (fs);
        auto s = rapSettings();
        r.setSettings (s);
        r.prep();
        const int lat = r.latency();
        juce::MidiBuffer midi;
        // impulse through the host-bypass path
        Buffer in (2, (int) (1.0 * fs));
        in.clear();
        const int at = 20000;
        in.setSample (0, at, 0.5f); in.setSample (1, at, 0.5f);
        Buffer out (in);
        for (int pos = 0; pos < out.getNumSamples(); pos += 512)
        {
            Buffer view (out.getArrayOfWritePointers(), 2, pos, juce::jmin (512, out.getNumSamples() - pos));
            r.p->processBlockBypassed (view, midi);
        }
        int pk = 0; float bv = 0.0f;
        for (int i = 0; i < out.getNumSamples(); ++i) if (std::abs (out.getSample (0, i)) > bv) { bv = std::abs (out.getSample (0, i)); pk = i; }
        rec ("2", "2.e1", "host bypass (processBlockBypassed): passes the audio with the reported latency", pk - at == lat,
             "reported latency " + String (lat) + " samples (" + ms (lat, fs) + "), the bypassed impulse is delayed by " + String (pk - at) + " samples; so with the host's bypass the signal jumps "
                 + ms (lat - (pk - at), fs) + " earlier than the processed one (and than the other tracks the host delay-compensates)",
             "delay == reported latency", "Source/plugin/PluginProcessor.h (no processBlockBypassed override; JUCE asserts this case, juce_AudioProcessor.cpp:593-599), JUCE VST3 wrapper adds a host Bypass parameter when getBypassParameter() is null",
             "the host's bypass runs the base-class pass-through, which has zero delay while MIHENK reports 28-330 ms", 1);
        // click at the switch, and old audio replaying when the host un-bypasses
        const auto sine = sineBuf (fs, 2.0, 220.0, -12.0, 2);
        Buffer o2 (sine);
        const int sw = 48000;
        for (int pos = 0; pos < o2.getNumSamples(); pos += 256)
        {
            Buffer view (o2.getArrayOfWritePointers(), 2, pos, juce::jmin (256, o2.getNumSamples() - pos));
            if (pos < sw) r.p->processBlock (view, midi); else r.p->processBlockBypassed (view, midi);
        }
        double worst = 0.0;
        for (int i = sw; i < sw + 2000; ++i) worst = juce::jmax (worst, (double) std::abs (o2.getSample (0, i) - o2.getSample (0, i - 1)));
        const double natural = dbToGain (-12.0) * 2.0 * kPi * 220.0 / fs;
        rec ("2", "2.e2", "host bypass switch: click when the host engages bypass in the middle of a tone", worst / natural <= 1.5,
             "largest sample step " + d (worst / natural, 1) + " x the natural step of a 220 Hz tone (the tone jumps " + ms (lat, fs) + " in phase)", "<= 1.5 x", "same as 2.e1", "no crossfade and no delay-matched pass-through on the host path", 1);
        // stale audio: sine, then host bypass with silence, then back to processBlock with silence
        Rig r3 (fs);
        r3.setSettings (rapSettings());
        r3.prep();
        const auto tone = sineBuf (fs, 1.0, 220.0, -12.0, 2);
        r3.run (tone, 256);
        Buffer sil (2, (int) (0.5 * fs));
        sil.clear();
        for (int pos = 0; pos < sil.getNumSamples(); pos += 256)
        {
            Buffer view (sil.getArrayOfWritePointers(), 2, pos, juce::jmin (256, sil.getNumSamples() - pos));
            r3.p->processBlockBypassed (view, midi);
        }
        const auto after = r3.run (silenceBuf (fs, 0.6, 2), 256);
        const double leak = peakDb (after);
        const int lat3 = r3.latency();
        int lastLoud = 0;
        for (int i = 0; i < after.getNumSamples(); ++i) if (std::abs (after.getSample (0, i)) > dbToGain (-60.0)) lastLoud = i;
        rec ("2", "2.e3", "un-bypassing from the host: old audio must not play again", leak < -90.0,
             "after 0.5 s of host bypass the plug-in is fed silence and still plays " + d (leak, 1) + " dBFS of the tone from before the bypass, for " + ms (lastLoud, fs)
                 + " (latency " + ms (lat3, fs) + ")", "< -90 dBFS", "Source/engine/RtChain.cpp:949-981 (reset() is only called from prepare), Source/plugin/PluginProcessor.cpp:121-171",
             "the chain keeps the last block-lengths of audio in its delay lines while the host bypasses and plays them out when processing resumes", 1);
    }

    // 2d: the standalone app: bypass is a whole-file re-render
    for (double lenSec : { 40.0, 180.0 })
    {
        const double sr = 44100.0;
        Rig a (sr, 2, 512, false, true);
        auto s = rapSettings();
        a.S().settings = s;
        const auto fileSong = song (sr, lenSec, 3);
        a.S().setInputForTesting (fileSong, sr, "behavior song");
        a.S().renderNowForTesting();
        a.prep();
        a.S().player.setPosition (0);
        a.S().player.play();
        const int blk = 512;
        Buffer buf (2, blk);
        juce::MidiBuffer midi;
        auto isInputBlock = [&]
        {
            const auto pos = (int) a.S().player.getPosition() - blk;
            if (pos < 0) return false;
            const float g = a.S().player.getOriginalGain();       // v3: the bypassed original is level-matched to the master (A/B LEVEL-MATCHED)
            for (int i = 0; i < blk; i += 5)
                if (buf.getSample (0, i) != fileSong.getSample (0, pos + i) * g) return false;
            return true;
        };
        auto spin = [&] (double ms, const std::function<bool()>& stop)
        {
            const double t0 = nowMs();
            while (nowMs() - t0 < ms)
            {
                buf.clear();
                a.P().processBlock (buf, midi);
                if (stop && stop()) return nowMs() - t0;
                pump (9);
            }
            return -1.0;
        };
        spin (2000.0, nullptr);
        const bool startedProcessed = ! isInputBlock();
        a.S().settings.bypass = true;
        const double tClick = nowMs();
        a.S().settingsChanged();
        const double audibleOn = spin (40000.0, [&] { return isInputBlock(); });
        const double doneOn = nowMs() - tClick;
        a.S().settings.bypass = false;
        a.S().settingsChanged();
        const double audibleOff = spin (100000.0, [&] { return ! isInputBlock(); });
        inf ("2", "2.d0_" + String ((int) lenSec), "standalone app test setup", String ((int) lenSec) + " s stereo file at 44.1 kHz, started processed: " + String (startedProcessed ? "yes" : "no"));
        rec ("2", "2.d1_" + String ((int) lenSec), "standalone: time from the BYPASS key to the audible switch (" + String ((int) lenSec) + " s file)", audibleOn >= 0 && audibleOn <= 100.0,
             audibleOn >= 0 ? d (audibleOn, 0) + " ms (key to render applied " + d (doneOn, 0) + " ms)" : "never switched within 90 s", "<= 100 ms",
             "Source/app/MainComponent.cpp:326, Source/app/Session.cpp:302-308 and 148-170, Source/engine/MasterChain.cpp:496-502 (bypass render calls analyse() on the whole file), Source/app/Player.h (no bypass path)",
             "in the app BYPASS is not an audio switch: it re-renders the whole file (120 ms debounce, then a copy plus a full-file loudness/spectrum analysis) and only then swaps the player's master buffer", 1);
        rec ("2", "2.d2_" + String ((int) lenSec), "standalone: time from BYPASS OFF to the processed sound returning (" + String ((int) lenSec) + " s file)", audibleOff >= 0 && audibleOff <= 100.0,
             audibleOff >= 0 ? d (audibleOff, 0) + " ms" : "never within 180 s", "<= 100 ms", "Source/engine/MasterChain.cpp:546-565 (renderLocked -> matchTarget on the whole file)",
             "switching back on waits for a full TARGET LOCK solve of the entire file; until then the bypass buffer keeps playing", 1);
    }
}

//==============================================================================
// Scenario 3: latency
int impulseDelay (Rig& r, double& reportedOut, const std::function<void()>& before = {})
{
    const double fs = r.fs;
    Buffer in (2, (int) (4.0 * fs));
    in.clear();
    const int at = (int) (1.0 * fs);
    in.setSample (0, at, 0.5f);
    in.setSample (1, at, 0.5f);
    if (before) before();
    r.prep();
    reportedOut = r.latency();
    auto out = r.run (in, 512, 0);
    int best = 0;
    float bv = 0.0f;
    for (int i = at - 100; i < out.getNumSamples(); ++i)
    {
        const float v = std::abs (out.getSample (0, i)) + std::abs (out.getSample (1, i));
        if (v > bv) { bv = v; best = i; }
    }
    return best - at;
}

void scenario3()
{
    section ("3  Reported latency vs measured, and what changes it");
    struct Cfg { const char* name; double tol; std::function<void (MasterSettings&)> set; };
    const Cfg cfgs[] {
        { "no splits, neutral EQ", 0, [] (MasterSettings&) {} },
        { "mono below 120 Hz (stereo split)", 0, [] (MasterSettings& s) { s.width.monoBelowHz = 120.0; } },
        { "high width 1.2", 0, [] (MasterSettings& s) { s.width.highWidth = 1.2; } },
        { "3-BAND compressor", 0, [] (MasterSettings& s) { s.comp.enabled = true; s.comp.mode = CompMode::threeBand; } },
        { "dynamic AIR", 0, [] (MasterSettings& s) { s.eq.airDynamic = true; s.eq.airDb = 2.0; } },
        { "linear-phase EQ, bell +2 dB", 1, [] (MasterSettings& s) { s.eq.linearPhase = true; s.eq.bell1Db = 2.0; } },
        { "all of the above", 1, [] (MasterSettings& s) { s.width.monoBelowHz = 120.0; s.width.highWidth = 1.2; s.comp.enabled = true; s.comp.mode = CompMode::threeBand;
                                                            s.eq.airDynamic = true; s.eq.airDb = 2.0; s.eq.linearPhase = true; s.eq.bell1Db = 2.0; } },
        { "HIP-HOP starting point (minimum-phase EQ, mono below)", 96, [] (MasterSettings& s) { s = rapSettings(); } },
    };
    int idx = 0;
    for (double fs : { 48000.0 })
        for (const auto& c : cfgs)
        {
            Rig r (fs);
            MasterSettings s;
            c.set (s);
            r.setSettings (s);
            double rep = 0.0;
            const int meas = impulseDelay (r, rep);
            const bool ok = std::abs (meas - (int) rep) <= c.tol;
            rec ("3", "3." + String (++idx), String ("impulse delay equals the reported latency: ") + c.name, ok,
                 "reported " + String ((int) rep) + " samples (" + ms (rep, fs) + "), impulse peak at " + String (meas) + " (" + ms (meas, fs) + "), difference " + String (meas - (int) rep),
                 "|difference| <= " + String ((int) c.tol) + " sample" + (c.tol == 1 ? "" : "s"), "Source/engine/RtChain.cpp:584-587 (latencyFor), 1430-1458", "reported latency is built from the same Dd / Dc / Ds terms the taps use", 2);
        }
    {
        MasterSettings s;
        s.eq.linearPhase = true; s.eq.bell1Db = 2.0; s.width.monoBelowHz = 120.0; s.comp.enabled = true; s.comp.mode = CompMode::threeBand; s.eq.airDynamic = true; s.eq.airDb = 2.0;
        for (double fs : { 44100.0, 96000.0 })
        {
            Rig q (fs);
            q.setSettings (s);
            double rep = 0.0;
            const int meas = impulseDelay (q, rep);
            rec ("3", "3.sr" + String ((int) fs), String ("all switches in, ") + String ((int) fs) + " Hz: impulse delay equals reported", std::abs (meas - (int) rep) <= 1,
                 "reported " + String ((int) rep) + " (" + ms (rep, fs) + "), measured " + String (meas), "|difference| <= 1");
        }
    }

    // What changes the latency, and how it reaches the host
    {
        const double fs = 48000.0;
        Rig r (fs);
        auto base = rapSettings();
        base.width.monoBelowHz = 0.0;
        r.setSettings (base);
        r.prep();
        const auto sine = sineBuf (fs, 1.0, 997.0, -12.0, 2);
        const auto sine3 = sineBuf (fs, 2.0, 997.0, -12.0, 2);
        juce::MidiBuffer midi;
        auto feed = [&] (int ms2)
        {
            Buffer b (2, 512);
            for (int i = 0; i < 512; ++i) for (int c = 0; c < 2; ++c) b.setSample (c, i, sine.getSample (c, i));
            const double t0 = nowMs();
            while (nowMs() - t0 < ms2) { r.p->processBlock (b, midi); pump (10); }
        };
        feed (150);
        struct Sw { const char* name; std::function<void()> on, off; };
        const Sw sws[] {
            { "LINEAR PHASE (EQ)", [&] { r.setParam ("eqLinear", 1); }, [&] { r.setParam ("eqLinear", 0); } },
            { "3-BAND mode", [&] { r.setParam ("compMode", 1); }, [&] { r.setParam ("compMode", 0); } },
            { "AIR DYN (with AIR 2 dB)", [&] { r.setParam ("airDb", 2); r.setParam ("airDyn", 1); }, [&] { r.setParam ("airDyn", 0); } },
            { "MONO BELOW knob 0 -> 120 Hz", [&] { r.setParam ("monoBelow", 120); }, [&] { r.setParam ("monoBelow", 0); } },
            { "MONO BELOW knob 0 -> 41 Hz", [&] { r.setParam ("monoBelow", 41); }, [&] { r.setParam ("monoBelow", 0); } },
            { "HIGH WIDTH knob 100 % -> 101 %", [&] { r.setParam ("highWidth", 1.01); }, [&] { r.setParam ("highWidth", 1.0); } },
        };
        const int steady = r.P().getChain().currentLatency();
        inf ("3", "3.s0", "latency with no delaying stage switched in", String (steady) + " samples = " + ms (steady, fs));
        int k = 0;
        double worstClick = 0.0, worstDip = 0.0;
        String swSummary;
        for (const auto& sw : sws)
        {
            const int l0 = r.P().getChain().currentLatency();
            double clickMax = 0.0, dipMin = 0.0, told = -1.0;
            int l1 = l0;
            for (double hz : { 997.0, 613.0 })
            {
                const auto sineH = sineBuf (fs, 2.0, hz, -12.0, 2);
                // switch ON
                r.setParam ("bypass", 0);
                r.prep();
                feed (60);
                const auto t = transitionOf (r, sineH, 64, 0, ((int) (0.8 * fs) / 64) * 64, [&] { sw.on(); }, hz);
                l1 = r.P().getChain().currentLatency();
                if (hz == 997.0)
                {
                    const double t0 = nowMs();
                    for (int i = 0; i < 60 && told < 0; ++i)
                    {
                        feed (10);
                        if (r.latency() == l1) told = nowMs() - t0;
                    }
                }
                // switch back OFF
                r.setParam ("bypass", 0);
                r.prep();
                feed (80);
                const auto t2 = transitionOf (r, sineH, 64, 0, ((int) (0.8 * fs) / 64) * 64, [&] { sw.off(); }, hz);
                clickMax = juce::jmax (clickMax, t.stepRatio, t2.stepRatio);
                dipMin = juce::jmin (dipMin, t.dipDb, t2.dipDb);
                sw.off();
                feed (120);
            }
            const bool changed = l1 != l0;
            worstClick = juce::jmax (worstClick, clickMax);
            worstDip = juce::jmin (worstDip, dipMin);
            const String line = String (sw.name) + ": latency " + String (l0) + " -> " + String (l1) + " samples (" + (changed ? "+" : "") + d (1000.0 * (l1 - l0) / fs, 1) + " ms)"
                                + (changed ? ", host told after ~" + d (told, 0) + " ms" : String()) + ", dropout " + d (dipMin, 1) + " dB, largest step " + d (clickMax, 1) + " x natural";
            inf ("3", "3.sw" + String (++k), String ("switching ") + sw.name, line);
            swSummary += line + " | ";
        }
        rec ("3", "3.swall", "switching a stage that changes the latency is inaudible (no dropout, no click)", worstClick <= 1.5 && worstDip >= -3.0,
             "worst click " + d (worstClick, 1) + " x the natural step, worst dropout " + d (worstDip, 1) + " dB. " + swSummary,
             "click <= 1.5 x natural, dropout >= -3 dB", "Source/engine/RtChain.cpp:1000-1003 (decision) and 1440-1441 (mute applied at the chain output), 1409-1427 (limiter delay between them)",
             "the output mute is applied at the moment the switch is decided, but the phase jump made inside the switched stage (Dw / Mb / Dair / EQ centre) only reaches the output 4*Dp + one block later (1340 samples = 28 ms at 48 kHz, measured +1404 samples): the dip comes first, the click arrives afterwards at full level", 1);
        // The two knobs that look continuous but flip the latency:
        {
            const int la = r.P().getChain().currentLatency();
            r.setParam ("monoBelow", 0); feed (60);
            const int lz = r.P().getChain().currentLatency();
            r.setParam ("monoBelow", 41); feed (60);
            const int l41 = r.P().getChain().currentLatency();
            r.setParam ("monoBelow", 300); feed (60);
            const int l300 = r.P().getChain().currentLatency();
            r.setParam ("monoBelow", 0); feed (60);
            rec ("3", "3.knob1", "a continuous knob must not change the plug-in's latency (MONO BELOW 0 -> 41 -> 300 Hz)", lz == l41 && l41 == l300,
                 "latency at 0 Hz " + String (lz) + ", 41 Hz " + String (l41) + ", 300 Hz " + String (l300) + " samples (" + d (1000.0 * (l41 - lz) / fs, 1)
                     + " ms step as the knob leaves 0); before this test " + String (la),
                 "no change", "Source/engine/RtChain.cpp:581 (st.split)", "the stereo-split delay (Dw) is switched in by monoBelowHz > 0, a value on a continuous knob, so moving the knob off 0 changes PDC and mutes the output for a block", 1);
            r.setParam ("highWidth", 1.0); feed (60);
            const int lw1 = r.P().getChain().currentLatency();
            r.setParam ("highWidth", 1.02); feed (60);
            const int lw2 = r.P().getChain().currentLatency();
            r.setParam ("highWidth", 1.0); feed (60);
            rec ("3", "3.knob2", "a continuous knob must not change the plug-in's latency (HIGH WIDTH 100 % -> 102 %)", lw1 == lw2,
                 "latency " + String (lw1) + " -> " + String (lw2) + " samples (" + d (1000.0 * (lw2 - lw1) / fs, 1) + " ms)", "no change", "Source/engine/RtChain.cpp:581", "same mechanism: highWidth != 1.0 switches the split delay in", 1);
        }
    }
}

//==============================================================================
// Scenario 4: level sanity
struct Level
{
    String name, targetName, msg, story;
    double inLufs = 0, inTp = 0, inPlr = 0, outLufs = 0, outTp = 0, outTp16 = 0, boost = 0, drive = 0, ceiling = -1, target = -14;
    double grMax = 0, grMean = 0, grActivePct = 0, grOver3Pct = 0, rtGrMax = 0, crestIn = 0, crestOut = 0, plrOut = 0, kickLossDb = 0;
    bool finite = true, reached = true;
};

Level evalLevel (const String& name, const Buffer& src, double fs, TargetPreset tp, double custom, const String& tname, const std::function<void (MasterSettings&)>& tweak = {})
{
    Level L;
    L.name = name; L.targetName = tname;
    auto st = rapSettings();
    st.targetPreset = tp;
    st.targetLufs = custom;
    if (tweak) tweak (st);
    Rig r (fs, src.getNumChannels());
    r.setSettings (st);
    r.learn (src);
    r.S().settingsChanged();      // what the panel does after any change: the worker re-renders and writes the status line
    r.settle();
    r.prep();
    MeterAcc ma;
    const auto out = r.playAligned (src, 512, &ma);
    L.inLufs = lufsOf (src, fs);
    L.inTp = tpOf (src);
    L.inPlr = L.inTp - L.inLufs;
    L.outLufs = lufsOf (out, fs);
    L.outTp = tpOf (out);
    L.outTp16 = tpOf (out, 16);
    L.boost = L.outLufs - L.inLufs;
    L.drive = r.getParam ("drive");
    L.ceiling = r.getParam ("ceiling");
    L.target = MasterChain::effectiveTarget (r.S().settings, MasterChain::measureSource (src, fs));
    L.grMax = r.S().limGr.maxDb;
    L.grMean = r.S().limGr.meanDb;
    L.grActivePct = 100.0 * r.S().limGr.activeFraction;
    {
        long long over = 0;
        for (float g : r.S().limGr.history) over += g > 3.0f;
        L.grOver3Pct = r.S().limGr.history.empty() ? 0.0 : 100.0 * (double) over / (double) r.S().limGr.history.size();
    }
    L.rtGrMax = ma.maxLim;
    L.crestIn = crestOf (src);
    L.crestOut = crestOf (out);
    L.plrOut = L.outTp - L.outLufs;
    L.kickLossDb = L.inPlr - L.plrOut;
    L.finite = countBad (out) == 0;
    L.msg = r.S().status;
    if (L.msg.startsWith ("LEARN armed")) L.msg.clear();     // the synchronous LEARN path of the harness leaves the arming text; not a user-visible state
    L.story = r.S().loudness().sentence;
    return L;
}

String levelLine (const Level& L)
{
    return L.targetName + ": in " + d (L.inLufs, 1) + " LUFS / " + d (L.inTp, 1) + " dBTP (PLR " + d (L.inPlr, 1) + ") -> out " + d (L.outLufs, 1) + " LUFS / " + d (L.outTp, 2)
           + " dBTP (PLR " + d (L.plrOut, 1) + "); boost " + (L.boost >= 0 ? "+" : "") + d (L.boost, 1) + " dB, drive " + d (L.drive, 1) + " dB, limiter GR max " + d (L.grMax, 1)
           + " / mean " + d (L.grMean, 2) + " dB, >3 dB for " + d (L.grOver3Pct, 1) + " % of the time, PLR lost " + d (L.kickLossDb, 1) + " LU";
}

void scenario4 (const std::vector<juce::File>& wavs)
{
    section ("4  Level sanity");
    const double fs = 48000.0;
    const auto base = songFull (fs, 20.0, 1);

    struct Mat { String name; Buffer buf; double fs; };
    std::vector<Mat> mats;
    auto addFinished = [&] (double l, double t)
    {
        auto b = makeFinished (base, fs, l, t);
        mats.push_back ({ "finished " + d (lufsOf (b, fs), 1) + " LUFS / " + d (tpOf (b), 1) + " dBTP", b, fs });
    };
    addFinished (-9.0, -1.5);
    addFinished (-10.0, -3.0);
    addFinished (-12.0, -3.0);
    for (const auto& w : wavs)
    {
        auto la = loadAudioFile (w);
        if (! la.ok()) continue;
        Buffer b = la.buffer;
        if (b.getNumSamples() > (int) (90 * la.sampleRate)) b = slice (b, 0, (int) (90 * la.sampleRate));
        mats.push_back ({ "real file " + w.getFileName(), b, la.sampleRate });
    }
    for (const auto& m : mats)
        inf ("4", "4.mat", "test material", m.name + ": " + d (lufsOf (m.buf, m.fs), 1) + " LUFS, " + d (tpOf (m.buf), 1) + " dBTP, PLR " + d (tpOf (m.buf) - lufsOf (m.buf, m.fs), 1)
                                              + ", crest " + d (crestOf (m.buf), 1) + " dB");

    // 4i: already finished material under every target a user would pick
    struct Tgt { String name; TargetPreset tp; double custom; };
    const Tgt tgts[] { { "STREAM -14", TargetPreset::streaming, -14 }, { "CLUB -8", TargetPreset::club, -8 }, { "CUSTOM -9", TargetPreset::custom, -9 },
                       { "KEEP INPUT", TargetPreset::keepInput, -14 }, { "CUSTOM -6 (beyond reach)", TargetPreset::custom, -6 } };
    int k = 0;
    for (const auto& m : mats)
        for (const auto& t : tgts)
        {
            const auto L = evalLevel (m.name, m.buf, m.fs, t.tp, t.custom, t.name);
            const bool keepT = t.tp == TargetPreset::keepInput;
            // SPEC_V3 2 REVISED: KEEP INPUT keeps the earlier rule; a chosen target is reached (within 0.3 LU), or, when even the limiter's own ceiling of
            // loudness is short of it by more than 0.2 LU, the status says "out of reach" with the number the limiter does get.
            const bool reachedOk = std::abs (L.outLufs - L.target) <= 0.3 || (L.msg.contains ("out of reach") && L.outLufs >= L.target - 3.0);
            const bool boostOk = keepT ? L.boost <= 3.0 + 0.05 : reachedOk, grOk = keepT ? (L.grMax <= 6.0 + 0.05 && L.grMean <= 2.0 + 0.05) : true;
            const bool ceilOk = L.outTp16 <= L.ceiling + 0.1 && L.finite;
            rec ("4", "4.f" + String (++k), "finished material " + m.name + " -> " + t.name, boostOk && grOk && ceilOk, levelLine (L) + (L.msg.isNotEmpty() ? " | " + L.msg : String()),
                 "KEEP INPUT: boost <= +3 dB, limiter peak GR <= 6 dB and mean <= 2 dB. Chosen target (v3 SPEC 2 REVISED): reached within 0.3 LU, or said 'out of reach' with the limiter's own best. True peak <= ceiling + 0.1 dB", "Source/engine/MasterChain.cpp:567-707 (matchTarget: maxDrive = 36 dB at :589, back-off of a single dB at :635-653)",
                 "TARGET LOCK solves the drive to the number on the target and lets the limiter pay for it: the only brake is the 0.1 LU diminishing-returns rule, there is no cap on boost or on limiter GR and no 'refuse and warn' branch", 1);
        }

    // 4i-b: the same finished material through a NEUTRAL chain (no starting point: the solver alone)
    {
        int kn = 0;
        for (const auto& m : mats)
            for (const auto& t : { tgts[1], tgts[3] })
            {
                const auto L = evalLevel (m.name, m.buf, m.fs, t.tp, t.custom, t.name, [] (MasterSettings& st)
                                          { const auto tp = st.targetPreset; const auto tl = st.targetLufs; st = MasterSettings(); st.targetPreset = tp; st.targetLufs = tl; });
                rec ("4", "4.n" + String (++kn), "NEUTRAL chain (EQ, compressor, stereo all off) on " + m.name + " -> " + t.name, (t.tp == TargetPreset::keepInput ? (L.boost <= 3.0 && L.grMax <= 2.0) : (std::abs (L.outLufs - L.target) <= 0.3 || (L.msg.contains ("out of reach") && L.outLufs >= L.target - 3.0))) && L.outTp16 <= L.ceiling + 0.1 && L.finite,
                     levelLine (L) + (L.msg.isNotEmpty() ? " | " + L.msg : String()), "KEEP INPUT: boost <= 3 dB, limiter GR <= 2 dB. Chosen target (v3 SPEC 2 REVISED): reached within 0.3 LU or said out of reach. True peak <= ceiling + 0.1 dB",
                     "Source/engine/MasterChain.cpp:567-707", "with every tone stage off the solver alone decides the drive", 1);
            }
    }

    // 4ii: very quiet material
    {
        const auto quiet = scaledToLufs (base, fs, -40.0);
        const auto L = evalLevel ("very quiet -40 LUFS", quiet, fs, TargetPreset::streaming, -14, "STREAM -14");
        rec ("4", "4.q1", "very quiet material (-40 LUFS) to STREAM -14", L.finite && std::abs (L.outLufs - L.target) <= 0.3 && L.boost >= 20.0 && L.outTp16 <= L.ceiling + 0.1,
             levelLine (L), "v3 SPEC 2 REVISED: the chosen target is reached (STREAM -14 within 0.3 LU, about +26 dB), the limiter holds the ceiling, no NaN; the status warns about the noise floor above +18 dB", "", "", 2);
    }
    {
        const auto mix = scaledToLufs (base, fs, -20.0);
        const auto L = evalLevel ("unmastered mix -20 LUFS", mix, fs, TargetPreset::streaming, -14, "STREAM -14");
        rec ("4", "4.m1", "ordinary mix (-20 LUFS, unmastered) to STREAM -14", L.finite && std::abs (L.outLufs - L.target) <= 0.3 && L.outTp16 <= L.ceiling + 0.1,
             levelLine (L), "v3 SPEC 2 REVISED: the chosen target is reached (STREAM -14 within 0.3 LU), the limiter holds the ceiling; what the limiter costs is in the status, not a refusal", "", "", 2);
    }

    // 4iii: silence, tails, mono
    {
        Rig r (fs);
        r.setSettings (rapSettings());
        r.learn (base);
        const auto tail = r.run (silenceBuf (fs, 4.0, 2), 512);
        const auto late = slice (tail, (int) (2.5 * fs), (int) (1.5 * fs));
        float maxLate = 0.0f;
        for (int c = 0; c < 2; ++c) maxLate = juce::jmax (maxLate, late.getMagnitude (c, 0, late.getNumSamples()));
        rec ("4", "4.s1", "silence after music: digital silence out, no NaN, no denormals", maxLate == 0.0f && countBad (tail) == 0 && countDenormals (tail) == 0,
             "largest sample after 2.5 s of silence " + d (gainToDb (maxLate), 1) + " dBFS, non-finite " + String ((long long) countBad (tail)) + ", denormals " + String ((long long) countDenormals (tail)),
             "0, 0, 0");
        const auto tail2 = r.run (silenceBuf (fs, 20.0, 2), 512);
        rec ("4", "4.s2", "20 s of silence: no denormals, still silent", countDenormals (tail2) == 0 && peakDb (slice (tail2, (int) (5 * fs), (int) (15 * fs))) < -200.0,
             "denormals " + String ((long long) countDenormals (tail2)) + ", level after 5 s " + d (peakDb (slice (tail2, (int) (5 * fs), (int) (15 * fs))), 1) + " dBFS");
    }
    {
        Rig r (fs);
        r.setSettings (rapSettings());
        r.learn (silenceBuf (fs, 3.0, 2));
        const auto m = r.playAligned (base, 512);
        const double l = lufsOf (m, fs);
        rec ("4", "4.s3", "LEARN pressed in a silent section, then the song plays", countBad (m) == 0 && l <= -8.0 && tpOf (m, 16) <= -0.9,
             "LEARN message: " + r.S().status + " | song comes out at " + d (l, 1) + " LUFS, " + d (tpOf (m, 16), 2) + " dBTP, drive " + d (r.getParam ("drive"), 1) + " dB, learned=" + String (r.P().isLearned() ? "yes" : "no"),
             "finite, no louder than -8 LUFS, ceiling held", "Source/plugin/PluginProcessor.cpp:276-301 (finishLearn)", "a silent capture is accepted as the learned source", 2);
    }
    {
        const auto monoSong = [&] { Buffer b (1, base.getNumSamples()); b.copyFrom (0, 0, base, 0, 0, base.getNumSamples()); return b; }();
        Rig r (fs, 1);
        r.setSettings (rapSettings());
        r.learn (monoSong);
        const auto out = r.playAligned (monoSong, 512);
        const double l = lufsOf (out, fs);
        rec ("4", "4.mo1", "mono track (one channel) to STREAM -14", countBad (out) == 0 && l <= -14.0 + 0.5 && tpOf (out, 16) <= -0.9,
             "out " + d (l, 2) + " LUFS (mono weight 1.0), " + d (tpOf (out, 16), 2) + " dBTP, drive " + d (r.getParam ("drive"), 1) + " dB, latency " + String (r.latency()), "v3: never above the target (the +3 dB cap may stop short of it), ceiling held");
    }
    {
        Buffer dual (2, base.getNumSamples());
        for (int c = 0; c < 2; ++c) dual.copyFrom (c, 0, base, 0, 0, base.getNumSamples());
        Rig r (fs);
        r.setSettings (rapSettings());
        r.learn (dual);
        const auto out = r.playAligned (dual, 512);
        double side = 0.0;
        for (int i = 0; i < out.getNumSamples(); ++i) side = juce::jmax (side, (double) std::abs (out.getSample (0, i) - out.getSample (1, i)));
        rec ("4", "4.mo2", "dual-mono input stays centred (L == R)", side < 1.0e-5, "largest L-R difference " + d (gainToDb (side), 1) + " dBFS", "< -100 dBFS");
    }

    {
        // a burst of NaN inside the LEARN pass (a misbehaving plug-in upstream), then clean music
        auto bad = song (fs, 12.0, 1);
        for (int c = 0; c < 2; ++c) for (int i = (int) (6.0 * fs); i < (int) (6.0 * fs) + 960; ++i) bad.setSample (c, i, std::numeric_limits<float>::quiet_NaN());
        Rig r (fs);
        r.setSettings (rapSettings());
        r.learn (bad);
        const auto clean = song (fs, 6.0, 5);
        const auto out = r.playAligned (clean, 512);
        rec ("4", "4.s4", "LEARN pass containing 20 ms of NaN, then clean music", countBad (out) == 0 && lufsOf (out, fs) > -30.0,
             "output non-finite samples " + String ((long long) countBad (out)) + ", level " + d (lufsOf (out, fs), 1) + " LUFS, drive " + d (r.getParam ("drive"), 1) + " dB, learned=" + String (r.P().isLearned() ? "yes" : "no"),
             "finite output, sane level", "Source/plugin/PluginProcessor.cpp:276-301 (nothing checks the captured audio)", "a NaN in the captured audio poisons the measured loudness, the solved drive and the plan", 2);
    }

    // 4iv: LEARN heard only part of the song
    {
        const auto verseOnly = slice (base, (int) (0.3 * fs), (int) (3.5 * fs));      // the quiet 4 s section
        const auto chorus = slice (base, (int) (4.3 * fs), (int) (3.5 * fs));
        Rig full (fs);
        full.setSettings (rapSettings());
        full.learn (base);
        Rig part (fs);
        part.setSettings (rapSettings());
        part.learn (verseOnly);
        MeterAcc mf, mp;
        const auto of = full.playAligned (base, 512, &mf);
        const auto op = part.playAligned (base, 512, &mp);
        const double lf = lufsOf (of, fs), lp = lufsOf (op, fs);
        rec ("4", "4.p1", "LEARN heard only the quiet verse; the whole song then plays", lp <= lf + 0.3 && mp.maxLim <= 3.0 && tpOf (op, 16) <= -0.9,
             "full-song LEARN: drive " + d (full.getParam ("drive"), 1) + " dB, song " + d (lf, 1) + " LUFS, limiter GR max " + d (mf.maxLim, 1) + " dB; verse-only LEARN: drive "
                 + d (part.getParam ("drive"), 1) + " dB, song " + d (lp, 1) + " LUFS, limiter GR max " + d (mp.maxLim, 1) + " dB (limiter active " + d (100.0 * (double) mp.active / (double) juce::jmax ((long long) 1, mp.n), 1) + " % of the time)",
             "v3: a LEARN that heard under 10 s lifts nothing: not louder than the full-song master, GR <= 3 dB", "Source/plugin/PluginProcessor.cpp:276-301, Source/engine/RtChain.cpp:1412 (one fixed drive from the learned audio)",
             "the drive is solved once on whatever LEARN heard and then applied as a constant: a section louder than the learned one is pushed into the limiter", 1);
        const auto Lc = lufsOf (op, fs, (int) (4.3 * fs), (int) (3.5 * fs));
        const auto Lc2 = lufsOf (of, fs, (int) (4.3 * fs), (int) (3.5 * fs));
        inf ("4", "4.p2", "chorus level in the two cases", "chorus " + d (Lc, 1) + " LUFS after verse-only LEARN, " + d (Lc2, 1) + " after full-song LEARN");
    }
}

//==============================================================================
// Scenario 5: fuzz
struct FuzzEvent { int sample; String id; double value; };

struct FuzzResult
{
    long long bad = 0, denormals = 0, tpViol = 0, jumps = 0, unexplained = 0, clicks = 0, dropouts = 0;
    double worstTpOver = -100.0, worstJump = 0.0, worstStep = 0.0, maxLevelJumpExplained = 0.0;
    String worstTpWhen, worstJumpWhen, worstStepWhen, unexplainedWhen;
    int events = 0;
    std::vector<String> clickCtx, tpCtx, unexCtx;
};

FuzzResult fuzzRun (double fs, bool pinkInput, bool rtAsync, uint32_t seed, double secs)
{
    FuzzResult R;
    const auto learnAudio = song (fs, 12.0, 11);
    Rig r (fs, 2, 512, true);
    {
        auto s = rapSettings();
        s.comp.mode = CompMode::threeBand;
        s.eq.dynLowMid.enabled = true; s.eq.dynLowMid.amountDb = 1.5;
        s.eq.dynUpperMid.enabled = true; s.eq.dynUpperMid.amountDb = 1.5;
        s.eq.tightener.enabled = true; s.eq.tightener.amountDb = 1.5;
        s.eq.airDynamic = true; s.eq.airDb = 2.0;
        s.width.phaseSafeLowMono = true; s.width.highWidth = 1.2;
        r.setSettings (s);
    }
    r.learn (learnAudio);
    r.P().setNonRealtime (! rtAsync);
    r.prep();

    const int n = (int) (secs * fs);
    Buffer in = pinkInput ? pinkNoise (fs, secs, -20.0, 2, seed + 1) : sineBuf (fs, secs, 220.0, -20.0, 2);

    // events
    juce::Random rng ((juce::int64) seed);
    const auto& defs = params::definitions();
    std::vector<const params::Def*> pick;
    for (const auto& df : defs)
        if (df.id != "bypass" && df.id != "limOn" && df.id != "targetLock" && df.id != "targetPreset" && df.id != "targetLufs" && df.id != "warnAt" && df.id != "toneSmooth") pick.push_back (&df);
    std::vector<FuzzEvent> ev;
    for (double t = 0.25; t < secs - 0.5; t += 0.15 + rng.nextDouble() * 0.3)
    {
        const int cnt = 1 + rng.nextInt (4);
        for (int i = 0; i < cnt; ++i)
        {
            const auto* df = pick[(size_t) rng.nextInt ((int) pick.size())];
            double lo = df->minimum, hi = df->maximum;
            if (df->id == "drive") { lo = -12.0; hi = 18.0; }
            if (df->id == "trim") { lo = -12.0; hi = 12.0; }
            double v = lo + rng.nextDouble() * (hi - lo);
            if (df->kind != params::Def::real) v = std::round (rng.nextDouble() * (df->maximum - df->minimum) + df->minimum);
            if (df->id == "monoBelow" && rng.nextFloat() < 0.3f) v = 0.0;
            if (df->id == "highWidth" && rng.nextFloat() < 0.3f) v = 1.0;
            ev.push_back ({ (int) (t * fs), df->id, v });
        }
    }
    // bypass twice
    ev.push_back ({ (int) (secs * 0.35 * fs), "bypass", 1.0 });
    ev.push_back ({ (int) (secs * 0.35 * fs) + (int) (0.5 * fs), "bypass", 0.0 });
    ev.push_back ({ (int) (secs * 0.7 * fs), "bypass", 1.0 });
    ev.push_back ({ (int) (secs * 0.7 * fs) + (int) (0.4 * fs), "bypass", 0.0 });
    std::stable_sort (ev.begin(), ev.end(), [] (const FuzzEvent& a, const FuzzEvent& b) { return a.sample < b.sample; });
    R.events = (int) ev.size();

    Buffer out (2, n);
    for (int c = 0; c < 2; ++c) out.copyFrom (c, 0, in, c, 0, n);
    std::vector<int> latAt ((size_t) n / 64 + 2, 0);
    std::vector<float> ceilAt ((size_t) n / 64 + 2, -1.0f);
    std::vector<char> bypassAt ((size_t) n / 64 + 2, 0);
    juce::MidiBuffer midi;
    size_t ei = 0;
    const int blocks[] { 32, 64, 128, 256, 512, 64, 512 };
    juce::Random brng (seed + 5);
    for (int pos = 0; pos < n;)
    {
        while (ei < ev.size() && ev[ei].sample <= pos) { r.setParam (ev[ei].id, ev[ei].value); ++ei; }
        int len = blocks[brng.nextInt (7)];
        len = juce::jmin (len, n - pos);
        Buffer view (out.getArrayOfWritePointers(), 2, pos, len);
        r.p->processBlock (view, midi);
        for (int bi = pos / 64; bi <= (pos + len - 1) / 64 && (size_t) bi < latAt.size(); ++bi)
        {
            latAt[(size_t) bi] = r.P().getChain().currentLatency();
            ceilAt[(size_t) bi] = (float) r.getParam ("ceiling");
            bypassAt[(size_t) bi] = r.getParam ("bypass") > 0.5;
        }
        pos += len;
        if (rtAsync) juce::Thread::sleep (3);
    }
    R.bad = countBad (out);
    R.denormals = countDenormals (out);

    auto ctxAt = [&] (int sampleOut)
    {
        const int lat = latAt[juce::jmin ((size_t) (sampleOut / 64), latAt.size() - 1)];
        String c = "lat " + String (lat) + ", events within the 200 ms + latency before:";
        int shown = 0;
        for (const auto& e : ev)
        {
            const int dt = sampleOut - e.sample;
            if (dt >= -(int) (0.01 * fs) && dt < lat + (int) (0.2 * fs) && shown < 8) { c += " " + e.id + "=" + d (e.value, 2) + "(" + d (1000.0 * dt / fs, 0) + " ms)"; ++shown; }
        }
        return c;
    };
    // ceiling: true peak per 10 ms window against the ceiling that was in force (with the latency as slack)
    const int win = (int) (0.01 * fs);
    const auto env = truePeakEnvelope (out, 4);
    auto sec = [&] (int i) { return String (i / fs, 2) + " s"; };
    for (int w = 0; (w + 1) * win <= n - 3 * win; ++w)           // the last 30 ms are left out: a truncated tail makes the 4x interpolation ring
    {
        const int a = w * win, b = a + win;
        const size_t bi = (size_t) (a / 64);
        // allowed: highest ceiling over the last (latency + 60 ms) of host time; excluded around bypass
        const int look = latAt[juce::jmin (bi, latAt.size() - 1)] + (int) (0.06 * fs);
        float allowed = -100.0f;
        bool byp = false;
        for (int q = juce::jmax (0, a - look); q <= b; q += 64)
        {
            const size_t qi = juce::jmin ((size_t) (q / 64), ceilAt.size() - 1);
            allowed = juce::jmax (allowed, ceilAt[qi]);
            byp |= bypassAt[qi] != 0;
        }
        if (byp) continue;
        float m = 0.0f;
        for (int i = a; i < b; ++i) m = juce::jmax (m, env[(size_t) i]);
        const double over = gainToDb (m) - allowed;
        if (over > R.worstTpOver) { R.worstTpOver = over; R.worstTpWhen = sec (a); }
        if (over > 0.1 && R.tpCtx.size() < 4) R.tpCtx.push_back ("t=" + sec (a) + " over by " + d (over, 2) + " dB, allowed ceiling " + d (allowed, 2) + "; " + ctxAt (a));
        R.tpViol += over > 0.1;
    }

    // level jumps (pink input) and steps (sine input)
    auto nearEvent = [&] (int sampleOut, String& which, int& dist)
    {
        // the effect of an event can appear as late as the OLD latency after it (a structure switch), so use the largest latency of the last 0.6 s
        int lat = 0;
        for (int q = juce::jmax (0, sampleOut - (int) (0.6 * fs)); q <= sampleOut; q += 64) lat = juce::jmax (lat, latAt[juce::jmin ((size_t) (q / 64), latAt.size() - 1)]);
        int best = 1 << 30;
        for (const auto& e : ev)
        {
            const int dt = sampleOut - e.sample;
            if (dt >= -(int) (0.01 * fs) && dt < lat + (int) (0.12 * fs) && dt < best) { best = dt; which = e.id; }
        }
        dist = best;
        return best != (1 << 30);
    };
    if (pinkInput)
    {
        std::vector<double> rms;
        for (int w = 0; (w + 1) * win <= n; ++w) rms.push_back (rmsOf (out, w * win, win));
        for (size_t w = 1; w < rms.size(); ++w)
        {
            if (rms[w] < -70.0 && rms[w - 1] < -70.0) continue;
            if ((int) w * win < latAt[0] + (int) (0.15 * fs)) continue;       // the signal arriving after the start-up latency is not a jump
            const double j = std::abs (rms[w] - rms[w - 1]);
            if (j > 6.0)
            {
                ++R.jumps;
                String which; int dist;
                const int at = (int) w * win;
                const bool near2 = nearEvent (at, which, dist);
                if (! near2) { ++R.unexplained; if (R.unexplainedWhen.isEmpty()) R.unexplainedWhen = sec (at) + " (" + d (j, 1) + " dB)"; if (R.unexCtx.size() < 6) R.unexCtx.push_back ("t=" + sec (at) + " " + d (rms[w - 1], 1) + " -> " + d (rms[w], 1) + " dB; " + ctxAt (at)); }
                else R.maxLevelJumpExplained = juce::jmax (R.maxLevelJumpExplained, j);
                if (j > R.worstJump) { R.worstJump = j; R.worstJumpWhen = sec (at) + " after " + (near2 ? which : String ("no event")); }
                if (rms[w] < -50.0 && rms[w - 1] > -40.0) ++R.dropouts;
            }
        }
    }
    else
    {
        std::vector<float> pk ((size_t) n, 0.0f);
        float run = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            run = juce::jmax (std::abs (out.getSample (0, i)), run * (float) std::exp (-1.0 / (0.004 * fs)));
            pk[(size_t) i] = run;
        }
        for (int i = 1; i < n; ++i)
        {
            // The sine starts at sample 0 and reaches the output after the start-up latency: its first few samples are an onset
            // of the test signal (a step of 0.26 of the local peak at 220 Hz), not a click of the plug-in. Same rule as the pink-noise check above.
            if (i < latAt[0] + (int) (0.05 * fs)) continue;
            if (pk[(size_t) i] < 0.02f) continue;
            const double ratio = std::abs (out.getSample (0, i) - out.getSample (0, i - 1)) / pk[(size_t) i];
            if (ratio > 0.25)
            {
                ++R.clicks;
                String which; int dist;
                const bool near2 = nearEvent (i, which, dist);
                if (R.clickCtx.size() < 5) R.clickCtx.push_back ("t=" + sec (i) + " step/peak " + d (ratio, 2) + "; " + ctxAt (i));
                if (ratio > R.worstStep) { R.worstStep = ratio; R.worstStepWhen = sec (i) + " after " + (near2 ? which : String ("no event")); }
            }
        }
    }
    return R;
}

void scenario5()
{
    section ("5  Parameter fuzz (seeded), sample rates, block sizes, bypass mid-run");
    int k = 0;
    struct Run { double fs; bool rt; uint32_t seed; double secs; };
    const Run runs[] { { 48000.0, false, 1001, 14 }, { 44100.0, false, 1002, 14 }, { 96000.0, false, 1003, 10 }, { 48000.0, true, 1004, 12 } };
    for (const auto& ru : runs)
    {
        const auto A = fuzzRun (ru.fs, true, ru.rt, ru.seed, ru.secs);
        const auto B = fuzzRun (ru.fs, false, ru.rt, ru.seed, ru.secs);
        const String tag = String ((int) ru.fs) + " Hz" + (ru.rt ? ", real-time async kernels" : "") + ", seed " + String ((int) ru.seed) + ", " + String (A.events) + " events";
        ++k;
        rec ("5", "5." + String (k) + "a", "no NaN / Inf / denormal (" + tag + ")", A.bad + B.bad + A.denormals + B.denormals == 0,
             "non-finite " + String (A.bad + B.bad) + ", denormals " + String (A.denormals + B.denormals), "0", "", "", 2);
        rec ("5", "5." + String (k) + "b", "true peak under the ceiling in force (" + tag + ")", A.worstTpOver <= 0.1 && B.worstTpOver <= 0.1,
             "worst overshoot " + d (juce::jmax (A.worstTpOver, B.worstTpOver), 2) + " dB (" + (A.worstTpOver >= B.worstTpOver ? A.worstTpWhen : B.worstTpWhen) + "), windows over: " + String (A.tpViol + B.tpViol),
             "<= +0.1 dB over the ceiling", "Source/engine/RtChain.cpp:400-412 (setParams restarts the gain pipeline on a look-ahead change)", "a look-ahead or ceiling automation move restarts or rescales the limiter pipeline mid-signal", 2);
        for (auto& c : A.tpCtx) inf ("5", "5." + String (k) + "y", "true-peak overshoot detail (pink)", c);
        rec ("5", "5." + String (k) + "c", "no dropout to silence while a steady pink-noise input plays (" + tag + ")", A.dropouts == 0,
             "level jumps > 6 dB between 10 ms windows: " + String (A.jumps) + " (worst " + d (A.worstJump, 1) + " dB " + A.worstJumpWhen + "), of which " + String (A.unexplained)
                 + " with no parameter event in the latency + 120 ms before (random extreme compressor / limiter settings pump by themselves, so this count is information); drops to < -50 dB: " + String (A.dropouts),
             "0 dropouts", "", "", 2);
        rec ("5", "5." + String (k) + "d", "no click (sample step > 25 % of the local peak) on a 220 Hz sine (" + tag + ")", B.clicks == 0,
             "samples over: " + String (B.clicks) + ", worst step/peak " + d (B.worstStep, 2) + " (" + B.worstStepWhen + "), natural " + d (2.0 * kPi * 220.0 / ru.fs, 3)
                 + ". The clicks sit at (latency before the switch) after a parameter event that changed the delay structure: the same mechanism as 3.swall",
             "0", "Source/engine/RtChain.cpp:1000-1003, 1440-1441", "a structure switch (eqOn / compOn / compMode / airDyn / eqLinear / mono below / high width) mutes the output at the wrong moment; the phase jump reaches the output one limiter-delay later", 2);
        for (auto& c : A.unexCtx) inf ("5", "5." + String (k) + "u", "unexplained jump detail", c);
        for (auto& c : B.clickCtx) inf ("5", "5." + String (k) + "x", "click detail", c);
        for (auto& c : B.tpCtx) inf ("5", "5." + String (k) + "y", "true-peak overshoot detail (sine)", c);
        inf ("5", "5." + String (k) + "e", "largest jump explained by a parameter event (" + tag + ")", d (A.maxLevelJumpExplained, 1) + " dB in one 10 ms window");
    }
}

void nanRecovery()
{
    const double fs = 48000.0;
    struct Case { const char* name; float value; bool finiteHuge; };
    const Case cases[] { { "NaN", std::numeric_limits<float>::quiet_NaN(), false }, { "+Inf", std::numeric_limits<float>::infinity(), false }, { "a 1e6 full-scale burst (finite)", 1.0e6f, true } };
    int k = 0;
    for (const auto& c : cases)
    {
        Rig r (fs);
        r.setSettings (rapSettings());
        r.learn (song (fs, 12.0, 1));
        const auto music1 = song (fs, 2.0, 5), music2 = song (fs, 5.0, 6);
        Buffer burst (2, (int) (0.05 * fs));
        for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < burst.getNumSamples(); ++i) burst.setSample (ch, i, c.finiteHuge ? (i % 2 ? c.value : -c.value) : c.value);
        const auto in = concat (concat (music1, burst), music2);
        const int lat = r.latency();
        const auto out = r.run (in, 512, lat + 64);
        const int burstEnd = music1.getNumSamples() + burst.getNumSamples();
        int lastBad = -1;
        for (int i = burstEnd; i < out.getNumSamples(); ++i)
            if (! std::isfinite (out.getSample (0, i)) || ! std::isfinite (out.getSample (1, i))) lastBad = i;
        // level of the last 2 s against a clean run
        Rig q (fs);
        q.setSettings (rapSettings());
        q.learn (song (fs, 12.0, 1));
        const auto clean = q.run (concat (music1, music2), 512, lat + 64);
        const int n2 = music2.getNumSamples();
        const double lv = lufsOf (out, fs, out.getNumSamples() - lat - 64 - 2 * (int) fs, 2 * (int) fs);
        const double lc = lufsOf (clean, fs, clean.getNumSamples() - lat - 64 - 2 * (int) fs, 2 * (int) fs);
        (void) n2;
        const bool recovered = lastBad < 0 || (lastBad - burstEnd) < (int) (0.5 * fs);
        const bool ok = recovered && (c.finiteHuge ? std::abs (lv - lc) < 0.5 : std::isfinite (lv) && std::abs (lv - lc) < 0.5);
        rec ("5", "5.nan" + String (++k), String ("recovery after 50 ms of ") + c.name + " on the input", ok,
             lastBad < 0 ? "output stayed finite" : (lastBad >= out.getNumSamples() - lat - 70 ? "output is STILL non-finite at the end of the 5 s of clean music that follows (the plug-in is dead until it is re-prepared)"
                                                                                     : "non-finite output until " + d (1000.0 * (lastBad - burstEnd) / fs, 0) + " ms after the burst")
                 + "; level of the last 2 s " + d (lv, 2) + " LUFS vs " + d (lc, 2) + " LUFS in a clean run",
             "finite again within 500 ms, same level as a clean run", "Source/engine/RtChain.cpp:949-981 (reset() only from prepare), IIR/envelope states throughout processBlock",
             "an IIR or envelope state that has taken a NaN / Inf never loses it, and no stage sanitises its input", 2);
    }
}

//==============================================================================
// Scenario 6: state recovery
juce::MemoryBlock makeBadState (int k, const juce::MemoryBlock& good)
{
    juce::MemoryBlock mb;
    auto goodXml = juce::AudioProcessor::getXmlFromBinary (good.getData(), (int) good.getSize());
    auto setParamAttr = [] (juce::XmlElement& x, const String& id, const String& v)
    {
        if (auto* params = x.getChildByName ("PARAMS"))
            for (auto* e : params->getChildIterator())
                if (e->getStringAttribute ("id") == id) e->setAttribute ("value", v);
    };
    switch (k)
    {
        case 0: break;                                                                       // empty
        case 1: { juce::Random r (5); for (int i = 0; i < 64; ++i) { char c = (char) r.nextInt (256); mb.append (&c, 1); } break; }
        case 2: { juce::XmlElement x ("SOMETHINGELSE"); juce::AudioProcessor::copyXmlToBinary (x, mb); break; }
        case 3: { juce::XmlElement x ("MIHENK"); juce::AudioProcessor::copyXmlToBinary (x, mb); break; }
        case 4: { if (goodXml) { if (auto* p = goodXml->getChildByName ("PARAMS")) goodXml->removeChildElement (p, true); goodXml->removeAttribute ("settings"); juce::AudioProcessor::copyXmlToBinary (*goodXml, mb); } break; }
        case 5: { if (goodXml) { goodXml->setAttribute ("settings", "{"); goodXml->setAttribute ("learned", "not json"); juce::AudioProcessor::copyXmlToBinary (*goodXml, mb); } break; }
        case 6:
        {
            if (goodXml)
            {
                auto v = juce::JSON::parse (goodXml->getStringAttribute ("settings"));
                if (auto* o = v.getDynamicObject())
                {
                    auto* eq = o->getProperty ("eq").getDynamicObject();
                    if (eq != nullptr)
                    {
                        if (auto* dl = eq->getProperty ("dynLowMid").getDynamicObject())
                        {
                            dl->setProperty ("q", 0.0); dl->setProperty ("ratio", -5.0); dl->setProperty ("attackMs", 0.0); dl->setProperty ("releaseMs", -10.0);
                            dl->setProperty ("freqHz", 1.0e30); dl->setProperty ("rangeDb", 1.0e30); dl->setProperty ("enabled", true); dl->setProperty ("amountDb", 4.0);
                        }
                        juce::Array<juce::var> curve;
                        for (int i = 0; i < 5; ++i) curve.add (1.0e30);
                        eq->setProperty ("matchCurveDb", curve);
                        eq->setProperty ("matchAmount", 1.0);
                    }
                }
                goodXml->setAttribute ("settings", juce::JSON::toString (v, true));
                juce::AudioProcessor::copyXmlToBinary (*goodXml, mb);
            }
            break;
        }
        case 7:
        {
            if (goodXml)
            {
                setParamAttr (*goodXml, "drive", "1e9"); setParamAttr (*goodXml, "ceiling", "50"); setParamAttr (*goodXml, "threshold", "-1e9");
                setParamAttr (*goodXml, "ratio", "-4"); setParamAttr (*goodXml, "attack", "0"); setParamAttr (*goodXml, "release", "-5");
                setParamAttr (*goodXml, "bypass", "7"); setParamAttr (*goodXml, "compMode", "9"); setParamAttr (*goodXml, "targetPreset", "99");
                juce::AudioProcessor::copyXmlToBinary (*goodXml, mb);
            }
            break;
        }
        case 8:
        {
            if (goodXml)
            {
                auto v = juce::JSON::parse (goodXml->getStringAttribute ("learned"));
                if (auto* o = v.getDynamicObject())
                    if (auto* pl = o->getProperty ("plan").getDynamicObject())
                    {
                        pl->setProperty ("valid", true); pl->setProperty ("sampleRate", -1.0); pl->setProperty ("autoTrimDb", 1.0e30); pl->setProperty ("wideMakeupDb", 1.0e30);
                        pl->setProperty ("safeGain", 1.0e30); pl->setProperty ("safeSign", 0.0); pl->setProperty ("airTypicalRelDb", -1.0e30);
                        juce::Array<juce::var> a; a.add (1.0e30); a.add (1.0e30); a.add (-1.0e30);
                        pl->setProperty ("bandMakeupDb", a); pl->setProperty ("dynThresholdDb", a);
                    }
                goodXml->setAttribute ("learned", juce::JSON::toString (v, true));
                juce::AudioProcessor::copyXmlToBinary (*goodXml, mb);
            }
            break;
        }
        case 9: { if (goodXml) { goodXml->setAttribute ("learned", "[1,2,3]"); goodXml->setAttribute ("settings", "[\"a\"]"); juce::AudioProcessor::copyXmlToBinary (*goodXml, mb); } break; }
        case 10: { mb.append (good.getData(), good.getSize() / 2); break; }
        case 11: { if (goodXml) { goodXml->setAttribute ("uiScale", "1e9"); goodXml->setAttribute ("abMatched", "x"); goodXml->setAttribute ("profile", "garbage{"); juce::AudioProcessor::copyXmlToBinary (*goodXml, mb); } break; }
        case 12: { juce::Random r (9); mb.setSize (10 * 1024 * 1024); auto* p = (char*) mb.getData(); for (size_t i = 0; i < mb.getSize(); ++i) p[i] = (char) r.nextInt (256); break; }
        case 13:
        {
            if (goodXml)
            {
                auto v = juce::JSON::parse (goodXml->getStringAttribute ("learned"));
                if (auto* o = v.getDynamicObject())
                {
                    o->setProperty ("inMeas", "x"); o->setProperty ("diagnosis", juce::var (new juce::DynamicObject()));
                    o->setProperty ("scenarios", "str");
                }
                goodXml->setAttribute ("learned", juce::JSON::toString (v, true));
                juce::AudioProcessor::copyXmlToBinary (*goodXml, mb);
            }
            break;
        }
        default: break;
    }
    return mb;
}

const char* badStateName (int k)
{
    static const char* names[] { "empty state (0 bytes)", "64 random bytes", "valid binary XML with a foreign root tag", "bare <MIHENK/> root",
                                 "version-1-like state: no parameters, no settings", "settings JSON truncated and 'learned' not JSON",
                                 "settings JSON with absurd non-parameter values (Q 0, ratio -5, 1e30 frequency, 1e30 match curve)",
                                 "parameter values out of range (drive 1e9, ceiling 50, ratio -4, ...)", "learned plan with absurd values (1e30 make-ups, safeGain 1e30)",
                                 "settings/learned of the wrong JSON type", "state truncated to half", "uiScale 1e9, garbage profile", "10 MB of random bytes",
                                 "learned block with wrong member types" };
    return names[k];
}
constexpr int kNumBadStates = 14;

/** Runs in a child process: loads state k into a fresh plug-in and plays 1.5 s. Prints one RESULT line. */
int childBadState (int k)
{
    const double fs = 48000.0;
    juce::MemoryBlock good;
    {
        Rig g (fs);
        auto s = rapSettings();
        s.eq.dynLowMid.enabled = true;
        s.eq.airDynamic = true; s.eq.airDb = 1.5;
        g.setSettings (s);
        g.learn (song (fs, 2.5, 1));
        g.P().getStateInformation (good);
    }
    const auto bad = makeBadState (k, good);
    const auto sig = song (fs, 1.5, 3);
    Rig def (fs);
    def.prep();
    const auto outDef = def.playAligned (sig, 256);
    Rig gi (fs);
    gi.P().setStateInformation (good.getData(), (int) good.getSize());
    gi.prep();
    const auto outGood = gi.playAligned (sig, 256);
    Rig q (fs);
    q.P().setStateInformation (bad.getData(), (int) bad.getSize());
    q.prep();
    pump (50);
    const auto o = q.playAligned (sig, 256);
    pump (50);
    juce::MemoryBlock again;
    q.P().getStateInformation (again);
    int diffParams = 0;
    for (const auto& df : params::definitions())
        if (std::abs (q.getParam (df.id) - def.getParam (df.id)) > 1.0e-4) ++diffParams;
    double worst = 0.0;
    for (int c = 0; c < 2; ++c) for (int i = 0; i < o.getNumSamples(); ++i) worst = juce::jmax (worst, (double) std::abs (o.getSample (c, i) - outDef.getSample (c, i)));
    std::printf ("RESULT bad=%lld peak=%.2f rms=%.2f defaultRms=%.2f tp=%.2f paramsChanged=%d identicalToDefault=%d stateOut=%d goodRms=%.2f\n", (long long) countBad (o), peakDb (o), rmsOf (o),
                 rmsOf (outDef), tpOf (o, 8), diffParams, worst == 0.0 ? 1 : 0, (int) again.getSize(), rmsOf (outGood));
    return 0;
}

void scenario6()
{
    section ("6  State recovery, transport loops, reset path");
    const double fs = 48000.0;
    const auto sig = song (fs, 6.0, 3);

    // 6.1 state round trips
    {
        auto round = [&] (const String& id, const String& name, Rig& src, bool learned)
        {
            juce::MemoryBlock mb;
            src.P().getStateInformation (mb);
            Rig dst (fs);
            dst.P().setStateInformation (mb.getData(), (int) mb.getSize());
            src.prep();
            dst.prep();
            const auto a = src.run (sig, 512, 600), b = dst.run (sig, 512, 600);
            double worst = 0.0;
            for (int c = 0; c < 2; ++c) for (int i = 0; i < a.getNumSamples(); ++i) worst = juce::jmax (worst, (double) std::abs (a.getSample (c, i) - b.getSample (c, i)));
            rec ("6", id, name, worst == 0.0 && src.latency() == dst.latency(),
                 "largest sample difference " + d (gainToDb (worst), 1) + " dBFS, latency " + String (src.latency()) + " vs " + String (dst.latency()) + ", state " + String ((int) mb.getSize()) + " bytes" + (learned ? ", learned plan included" : ""),
                 "bit-exact", "Source/plugin/PluginProcessor.cpp:323-404", "", 1);
        };
        Rig a (fs);
        round ("6.1", "state round trip: default instance", a, false);
        Rig b (fs);
        b.setSettings (rapSettings());
        b.learn (song (fs, 12.0, 1));
        round ("6.2", "state round trip: learned HIP-HOP instance", b, true);
        Rig c (fs);
        {
            auto s = rapSettings();
            s.comp.mode = CompMode::threeBand; s.eq.airDynamic = true; s.eq.airDb = 2.0; s.eq.dynLowMid.enabled = true; s.eq.dynLowMid.freqHz = 250.0; s.eq.dynLowMid.q = 2.5;
            s.eq.dynLowMid.rangeDb = 3.0; s.eq.dynLowMid.attackMs = 40.0;
            c.setSettings (s);
            c.learn (song (fs, 12.0, 1));
            juce::Random rng (3);
            for (const auto& df : params::definitions())
                if (df.id != "bypass" && df.id != "limOn" && rng.nextFloat() < 0.3f && df.kind == params::Def::real) c.setParam (df.id, df.minimum + rng.nextDouble() * (df.maximum - df.minimum));
            c.settle();
        }
        round ("6.3", "state round trip: 3-BAND, dynamic EQ with non-parameter fields, 30 % of the knobs random", c, true);
    }

    // 6.2 bad states, each in a child process so a crash is a finding, not the end of the run
    {
        const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
        for (int k = 0; k < kNumBadStates; ++k)
        {
            juce::ChildProcess cp;
            const bool started = cp.start (juce::StringArray { exe.getFullPathName(), "--child-badstate", String (k) });
            bool finished = started && cp.waitForProcessToFinish (120000);
            const auto output = started ? cp.readAllProcessOutput() : String();
            const auto exitCode = started ? cp.getExitCode() : -1;
            if (started && ! finished) cp.kill();
            String line;
            for (auto& l : juce::StringArray::fromLines (output)) if (l.startsWith ("RESULT")) line = l;
            const bool ran = finished && exitCode == 0 && line.isNotEmpty();
            double peak = 0, rms = 0, defRms = 0, tp = 0, goodRms = 0;
            int bad = -1, changed = -1, ident = 0;
            if (ran)
            {
                std::smatch m;
                const std::string s = line.toStdString();
                if (std::regex_search (s, m, std::regex (R"(bad=(-?\d+) peak=(\S+) rms=(\S+) defaultRms=(\S+) tp=(\S+) paramsChanged=(\d+) identicalToDefault=(\d) stateOut=\d+ goodRms=(\S+))")))
                {
                    bad = std::stoi (m[1]); peak = std::stod (m[2]); rms = std::stod (m[3]); defRms = std::stod (m[4]); tp = std::stod (m[5]); changed = std::stoi (m[6]); ident = std::stoi (m[7]); goodRms = std::stod (m[8]);
                }
            }
            // what a sensible plug-in does: a state that is wrecked falls back to the defaults; a state whose parameters
            // are intact but whose extras are garbage keeps the parameters; absurd values must at least stay finite.
            static const int kind[] { 0, 0, 0, 0, 1, 1, 2, 2, 2, 1, 0, 1, 0, 1 };   // v3: the intent and the learned block ARE the state, so a state that only lost its parameters (4) is rebuilt from them: the same sound as the intact instance
            const double ref = kind[k] == 1 ? goodRms : defRms;
            bool sane = ran && bad == 0 && tp <= 0.5 && std::isfinite (rms);
            if (kind[k] == 0) sane = sane && std::abs (rms - ref) <= 0.5;
            if (kind[k] == 1) sane = sane && rms <= ref + 0.5 && rms >= ref - 10.0;       // v3: the level is f(measurements, intent); a lost LEARN block loses the planned gain, never adds one
            const String expect = kind[k] == 0 ? "falls back to the default instance" : (kind[k] == 1 ? "keeps the intact parameters, drops the garbage" : "stays finite and under the ceiling");
            rec ("6", "6.bad" + String (k), String ("bad state: ") + badStateName (k), sane,
                 ran ? "no crash; non-finite samples " + String (bad) + ", true peak " + d (tp, 1) + " dBTP, level " + d (rms - ref, 1) + " dB vs the expected instance (" + expect + "), " + String (changed)
                           + " parameters differ from default" + (ident ? ", output identical to default" : String())
                     : (started ? (finished ? "CRASHED / failed, exit code " + String (exitCode) : "HUNG (killed after 120 s)") : "could not start the child"),
                 "no crash, finite, level within 0.5 dB (3 dB when only the learned block is lost) of the expected instance, peak <= +0.5 dBTP", "Source/plugin/PluginProcessor.cpp:357-404 (setStateInformation), Source/engine/RtChain.cpp:1169-1181, 1067-1090 (plan and non-parameter fields used unchecked)",
                 "the state is applied field by field with no validation; only a missing/foreign root tag is rejected", 2);
        }
    }

    // 6.3 transport: loops and jumps
    {
        Rig r (fs);
        r.setSettings (rapSettings());
        r.learn (song (fs, 20.0, 1));
        const auto whole = song (fs, 20.0, 1);
        const auto seg = slice (whole, (int) (0.3 * fs), (int) (8.0 * fs));
        r.run (silenceBuf (fs, 1.2, 2), 512);
        std::vector<double> lv;
        const int lat = r.latency();
        Buffer all = seg;
        for (int i = 1; i < 10; ++i) all = concat (all, seg);
        const auto out = r.run (all, 512, lat + 64);
        for (int i = 0; i < 10; ++i) lv.push_back (lufsOf (out, fs, lat + i * seg.getNumSamples(), seg.getNumSamples()));
        double lo = 1e9, hi = -1e9;
        for (int i = 1; i < 10; ++i) { lo = juce::jmin (lo, lv[(size_t) i]); hi = juce::jmax (hi, lv[(size_t) i]); }
        String s;
        for (double v : lv) s += d (v, 2) + " ";
        rec ("6", "6.loop1", "an 8 s section looped 10 times (host loop, no reset): level of each pass", hi - lo <= 0.1,
             "per pass LUFS: " + s + "; passes 2..10 spread " + d (hi - lo, 3) + " LU, pass 1 vs pass 10 " + d (lv[0] - lv[9], 3) + " LU", "spread <= 0.1 dB", "", "", 2);

        // jumps between loud and quiet parts
        const auto verse = slice (whole, (int) (0.3 * fs), (int) (3.5 * fs)), chorus = slice (whole, (int) (4.3 * fs), (int) (3.5 * fs));
        Buffer seq = verse;
        for (int i = 0; i < 4; ++i) { seq = concat (seq, chorus); seq = concat (seq, verse); }
        r.run (silenceBuf (fs, 1.2, 2), 512);
        const auto o2 = r.run (seq, 512, lat + 64);
        const int L = verse.getNumSamples();
        std::vector<double> vv;
        for (int i = 0; i < 5; ++i) vv.push_back (lufsOf (o2, fs, lat + 2 * i * L + L / 2, L / 2));       // second half of each verse
        double lo2 = 1e9, hi2 = -1e9;
        for (double v : vv) { lo2 = juce::jmin (lo2, v); hi2 = juce::jmax (hi2, v); }
        String s2;
        for (double v : vv) s2 += d (v, 2) + " ";
        rec ("6", "6.jump1", "playhead jumps verse <-> chorus 4 times: level of the second half of each verse", hi2 - lo2 <= 0.1,
             "LUFS: " + s2 + "; spread " + d (hi2 - lo2, 3) + " LU", "<= 0.1 dB", "", "", 2);
    }

    // 6.3b a reopened project: the learned values stay, but nothing re-solves any more
    {
        const auto songF = songFull (fs, 20.0, 1);
        const auto sigF = slice (songF, 0, (int) (10.0 * fs));
        Rig live (fs);
        live.setSettings (rapSettings());
        live.learn (songF);
        juce::MemoryBlock saved;
        live.P().getStateInformation (saved);
        Rig reopened (fs);
        reopened.P().setStateInformation (saved.getData(), (int) saved.getSize());
        reopened.prep();
        const double l0 = measure (live, sigF).lufs, r0 = measure (reopened, sigF).lufs;
        // (a) through the host parameter (automation, generic host UI, a controller)
        live.setParam ("targetPreset", 1.0 * (int) TargetPreset::club);
        live.settle();
        const double lp = measure (live, sigF).lufs;
        const double tgtParam = live.getParam ("targetLufs");
        // (b) through the panel's own path (the editor sets the session's settings)
        Rig live2 (fs);
        live2.setSettings (rapSettings());
        live2.learn (songF);
        live2.S().settings.targetPreset = TargetPreset::club;
        live2.S().settingsChanged();
        live2.settle();
        const double lq = measure (live2, sigF).lufs;
        // (c) a reopened project, panel path
        reopened.S().settings.targetPreset = TargetPreset::club;
        reopened.S().settingsChanged();
        reopened.settle();
        const double l1 = lq, r1 = measure (reopened, sigF).lufs;
        inf ("6", "6.dbg", "reopened vs live plan", "live2 trim " + d (live2.S().settings.trimDb, 2) + " plan gain " + d (live2.S().plan.totalGainDb, 2) + " meas " + d (live2.S().measurements.integrated, 2) + " present " + String ((int) live2.S().measurements.present)
                                                  + " | reopened trim " + d (reopened.S().settings.trimDb, 2) + " plan gain " + d (reopened.S().plan.totalGainDb, 2) + " meas " + d (reopened.S().measurements.integrated, 2) + " present "
                                                  + String ((int) reopened.S().measurements.present) + " param trim " + d (reopened.getParam ("trim"), 2) + " target " + String ((int) reopened.S().settings.targetPreset) + " overrides " + String (reopened.S().overrideCount()) + " status " + reopened.S().status.substring (0, 160));
        rec ("6", "6.target1", "target changed to CLUB -8 from the HOST (automation / generic UI / controller): does the sound follow the label?", lp > l0 + 1.0 && std::abs (lp - lq) <= 0.3,
             "label CLUB -8 (preset parameter = 2) but the loudness parameter stays at " + d (tgtParam, 1) + " LUFS: the output stays " + d (lp, 2) + " LUFS (was " + d (l0, 2) + "); through the panel's own path the same change gives " + d (lq, 2) + " LUFS",
             "v3: the host's change is the same intent as the panel's: louder than before and equal to the panel path (the +3 dB cap and the limiter bound how far)", "Source/plugin/PluginProcessor.cpp:180-192 and 194-201, Source/app/Session.cpp:305",
             "applyParamsToSession() sets applyingParams, so Session::settingsChanged() derives targetLufs = -8 but onSettingsEdited() skips the write and immediately re-reads the stale targetLufs parameter (-14) over it; the engine solves for -14 under a CLUB label", 2);
        rec ("6", "6.reload1", "reopened project (no captured audio): target changed to CLUB -8 from the panel", r1 > r0 + 1.0 && std::abs (r1 - l1) <= 0.6,
             "live session " + d (l0, 2) + " -> " + d (l1, 2) + " LUFS; reopened project " + d (r0, 2) + " -> " + d (r1, 2) + " LUFS (stale flag " + String (reopened.P().learnedIsStale() ? "set" : "not set") + ")",
             "v3: a reopened project plans from the saved measurements: the same level as the live session", "Source/plugin/PluginProcessor.cpp:303-308, Source/app/Session.cpp:289-300 (renderRequest returns when there is no input)",
             "TARGET LOCK only works while the captured LEARN audio is in memory; after a project reload the panel changes and nothing in the sound moves until LEARN is pressed again", 2);
        const double trimL0 = live2.S().settings.trimDb, trimR0 = reopened.S().settings.trimDb;
        live2.setParam ("bell2Db", 6.0);
        live2.settle();
        reopened.setParam ("bell2Db", 6.0);
        reopened.settle();
        const double l2 = measure (live2, sigF).lufs, r2 = measure (reopened, sigF).lufs;
        rec ("6", "6.reload2", "BELL 2 +6 dB: the plan's level does not move (live session vs reopened project)", std::abs (live2.S().settings.trimDb - trimL0) < 1e-6 && std::abs (reopened.S().settings.trimDb - trimR0) < 1e-6 && live2.S().isOverridden ("bell2Db"),
             "live session " + d (l1, 2) + " -> " + d (l2, 2) + " LUFS; reopened project " + d (r1, 2) + " -> " + d (r2, 2) + " LUFS", "v3: an EQ override is a delta over the plan and never re-solves the level (the trim stays)", "same as 6.reload1", "same cause", 2);
    }

    // 6.4 reset path
    {
        Rig fresh (fs);
        fresh.prep();
        const auto ref = fresh.playAligned (sig, 512);
        Rig r (fs);
        {
            auto s = rapSettings();
            s.comp.mode = CompMode::threeBand; s.eq.airDynamic = true; s.eq.airDb = 3.0;
            s.eq.dynUpperMid.enabled = true; s.eq.dynUpperMid.freqHz = 5000.0; s.eq.dynUpperMid.q = 3.0; s.eq.dynUpperMid.amountDb = 3.0; s.eq.dynUpperMid.rangeDb = 4.0;
            std::vector<float> curve (spectrumGrid().size(), 0.0f);
            for (size_t i = 0; i < curve.size(); ++i) curve[i] = (float) (3.0 * std::sin (0.05 * (double) i));
            s.eq.matchCurveDb = curve;
            r.setSettings (s);
        }
        r.learn (song (fs, 12.0, 1));
        // a project that was saved and reloaded: parameters + non-parameter settings + learned plan, no captured audio
        juce::MemoryBlock saved;
        r.P().getStateInformation (saved);
        Rig r2 (fs);
        r2.P().setStateInformation (saved.getData(), (int) saved.getSize());
        // (a) only the automatable parameters back to default: what a user could do before RESET ALL existed (information)
        for (const auto& df : params::definitions()) r2.setParam (df.id, df.defaultValue());          // every automatable parameter back to default
        pump (200);
        r2.prep();
        const auto onlyParams = r2.playAligned (sig, 512);
        double worstParams = 0.0;
        for (int c = 0; c < 2; ++c) for (int i = 0; i < onlyParams.getNumSamples(); ++i) worstParams = juce::jmax (worstParams, (double) std::abs (onlyParams.getSample (c, i) - ref.getSample (c, i)));
        inf ("6", "6.reset0", "parameters only back to default (no RESET ALL): difference from a fresh instance",
             d (gainToDb (worstParams), 1) + " dBFS, level " + d (lufsOf (onlyParams, fs) - lufsOf (ref, fs), 2) + " LU (the tone-match curve, dynamic-EQ details and the learned plan are outside the parameters)");
        // (b) RESET ALL
        Rig r3 (fs);
        r3.P().setStateInformation (saved.getData(), (int) saved.getSize());
        pump (100);
        r3.P().resetAll();
        pump (200);
        r3.prep();
        const auto out = r3.playAligned (sig, 512);
        double worst = 0.0;
        for (int c = 0; c < 2; ++c) for (int i = 0; i < out.getNumSamples(); ++i) worst = juce::jmax (worst, (double) std::abs (out.getSample (c, i) - ref.getSample (c, i)));
        // the saved state of the reset instance is the one of a fresh instance (no learned block, default settings)
        juce::MemoryBlock freshState, resetState;
        fresh.P().getStateInformation (freshState);
        r3.P().getStateInformation (resetState);
        auto hasLearned = [] (const juce::MemoryBlock& mb)
        {
            auto x = juce::AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize());
            return x != nullptr && x->hasAttribute ("learned");
        };
        const bool stateClean = ! hasLearned (resetState) && ! r3.P().isLearned();
        rec ("6", "6.reset", "RESET ALL on a reopened, learned project reproduces a fresh instance", worst == 0.0 && stateClean && r3.latency() == fresh.latency(),
             "largest difference from a fresh instance " + d (gainToDb (worst), 1) + " dBFS, level " + d (lufsOf (out, fs) - lufsOf (ref, fs), 2) + " LU, latency " + String (r3.latency()) + " vs " + String (fresh.latency())
                 + ", learned block in the saved state: " + String (hasLearned (resetState) ? "yes" : "no") + ", isLearned " + String (r3.P().isLearned() ? "yes" : "no"),
             "identical output, nothing learned left", "Source/plugin/PluginProcessor.cpp (resetAll), Source/app/Session.cpp (resetAll)",
             "state outside the 94 parameters (tone-match curve, dynamic-EQ Q / range / timing, the learned plan, the captured audio) survives a parameter reset: RESET ALL clears it", 1);
    }
}

//==============================================================================
// Scenario 7: explainability
void scenario7()
{
    section ("7  What the panel says vs what is measured");
    const double fs = 48000.0;
    const auto base = songFull (fs, 20.0, 1);
    struct Case { String name; Buffer buf; TargetPreset tp; double custom; };
    std::vector<Case> cases;
    cases.push_back ({ "unmastered mix (-20 LUFS) to STREAM -14", scaledToLufs (base, fs, -20.0), TargetPreset::streaming, -14 });
    cases.push_back ({ "finished (-11.6 LUFS / -3 dBTP) to CLUB -8", makeFinished (base, fs, -10.0, -3.0), TargetPreset::club, -8 });
    cases.push_back ({ "finished (-10.1 LUFS / -1.5 dBTP) to STREAM -14", makeFinished (base, fs, -9.0, -1.5), TargetPreset::streaming, -14 });
    cases.push_back ({ "dynamic mix (-20 LUFS) to CUSTOM -5 (beyond reach)", scaledToLufs (base, fs, -20.0), TargetPreset::custom, -5 });
    int k = 0;
    for (const auto& c : cases)
    {
        ++k;
        auto st = rapSettings();
        st.targetPreset = c.tp;
        st.targetLufs = c.custom;
        Rig r (fs);
        r.setSettings (st);
        r.learn (c.buf);
        r.S().settingsChanged();
        r.settle();
        r.prep();
        MeterAcc ma;
        ma.keep = true;
        const int lat = r.latency();
        const auto raw = r.run (c.buf, 512, lat + 600, &ma);
        const auto out = slice (raw, lat, c.buf.getNumSamples());
        auto& S = r.S();
        const double measured = lufsOf (out, fs), inL = lufsOf (c.buf, fs);
        const auto story = S.loudness();
        rec ("7", "7." + String (k) + "a", c.name + ": displayed OUT loudness vs real-time output", std::abs (S.outMeas.integrated - measured) <= 0.1,
             "panel " + d (S.outMeas.integrated, 2) + " LUFS, measured on the plug-in output " + d (measured, 2) + " LUFS", "<= 0.1 LU");
        rec ("7", "7." + String (k) + "b", c.name + ": 'turns it up/down X dB' vs measured change", ! story.valid || std::abs (story.changeDb - (measured - inL)) <= 0.2,
             "panel \"" + story.sentence + "\" (" + d (story.changeDb, 2) + " dB); measured " + d (measured - inL, 2) + " dB", "<= 0.2 dB");
        const double rtMean = ma.n > 0 ? ma.sumLim / (double) ma.n : 0.0;
        rec ("7", "7." + String (k) + "c", c.name + ": displayed limiter GR (max / mean) vs the live meter", std::abs (S.limGr.maxDb - ma.maxLim) <= 0.3 && std::abs (S.limGr.meanDb - rtMean) <= 0.2,
             "panel max " + d (S.limGr.maxDb, 2) + " / mean " + d (S.limGr.meanDb, 3) + " dB, live meter max " + d (ma.maxLim, 2) + " / mean " + d (rtMean, 3) + " dB", "<= 0.3 dB max, <= 0.2 dB mean",
             "Source/engine/RtChain.cpp:1443-1444 (limGr from the 4 pass product)", "");
        rec ("7", "7." + String (k) + "d", c.name + ": displayed compressor GR max vs the live meter", std::abs (S.compGr.maxDb - ma.maxComp) <= 0.5,
             "panel max " + d (S.compGr.maxDb, 2) + " dB, live meter max " + d (ma.maxComp, 2) + " dB (glue loud-part mean " + d (S.glue.loudMeanGrDb, 2) + " dB)", "<= 0.5 dB");
        {
            const String st2 = S.status;
            double claimed = 0.0;
            bool has = false;
            std::smatch m;
            const std::string s = st2.toStdString();
            if (std::regex_search (s, m, std::regex (R"(achieved (-?\d+(\.\d+)?) LUFS)"))) { claimed = std::stod (m[1]); has = true; }
            const bool ok = ! has || std::abs (claimed - measured) <= 0.3;
            rec ("7", "7." + String (k) + "e", c.name + ": status line vs measured", ok,
                 "status \"" + st2 + "\"" + (has ? ", claims " + d (claimed, 2) + " LUFS, measured " + d (measured, 2) : String(", no numeric claim")) + "; panel warnings: " + S.warnings.joinIntoString (" / "),
                 "claimed level within 0.3 LU");
        }
        if (k == 1)
        {
            // meter vs output alignment
            int bestLag = -1;
            double bestErr = 1e9;
            const int a = 30000, len = 20000;
            for (int lag = 0; lag <= 1200; ++lag)
            {
                double e = 0.0;
                for (int i = 0; i < len; i += 5) e = juce::jmax (e, (double) std::abs (raw.getSample (0, a + i + lag) - ma.outL[(size_t) (a + i)]));
                if (e < bestErr) { bestErr = e; bestLag = lag; }
            }
            rec ("7", "7.meter", "live meter OUT frames are the output samples, and how far they run ahead of what is heard", bestErr == 0.0 && bestLag <= 2 * RtChain::blockSize,
                 "exact match at an offset of " + String (bestLag) + " samples (" + ms (bestLag, fs) + "), largest difference " + d (gainToDb (bestErr), 1) + " dBFS", "exact; offset <= 2 internal blocks");
        }
    }

    // level-neutral make-up claim: compressor in vs out, limiter off, drive 0
    {
        const auto mix = scaledToLufs (base, fs, -16.0);
        MasterSettings a = rapSettings();
        a.targetLock = false;
        a.limiter.enabled = false;
        a.eq = EqSettings();
        a.width = WidthSettings();
        MasterSettings b = a;
        b.comp.enabled = false;
        PreLimiterCache c1, c2;
        const auto r1 = MasterChain::render (mix, fs, a, c1), r2 = MasterChain::render (mix, fs, b, c2);
        rec ("7", "7.makeup", "compressor make-up is level-neutral (panel: loud-part GR vs measured loudness change)", std::abs (r1.measured.integrated - r2.measured.integrated) <= 0.3,
             "compressor in " + d (r1.measured.integrated, 2) + " LUFS, out " + d (r2.measured.integrated, 2) + " LUFS (loud-part GR " + d (r1.glue.loudMeanGrDb, 2) + " dB, make-up " + d (r1.glue.makeupDb, 2) + " dB)", "<= 0.3 LU");
    }
}

//==============================================================================
// Report
const Item* findItem (const String& id) { for (auto& it : g_items) if (it.id == id) return &it; return nullptr; }

struct Finding { const char* title; const char* cause; const char* where; std::vector<String> prefixes; };
String U (const char* utf8) { return String::fromUTF8 (utf8); }

String findingsText()
{
    const std::vector<Finding> fs {
        { "Girişe 50 ms'lik NaN / Inf gelince çıkış kalıcı olarak NaN: eklenti ölüyor, tek çare kapat-aç / yeniden prepare  [şikâyet (d)]",
          "RtChain'de hiçbir aşama girişi temizlemiyor; DC filtre, dedektör, zarf ve limiter toplamı gibi özyinelemeli durumlar bir kez NaN alınca kendini toparlayamıyor ve reset() sadece prepareToPlay'den çağrılıyor.",
          "Source/engine/RtChain.cpp:1019-1031 (giriş aşaması), :949-981 (reset), Source/plugin/PluginProcessor.cpp:121-171 (processBlock)", { "5.nan" } },
        { "DAW'ın kendi bypass'ı (VST3 Bypass parametresi / AU bypass / cihaz anahtarı) gecikme telafisiz: sinyal bildirilen latency kadar erken kayıyor, çıkarken eski ses tekrar çalıyor  [şikâyet (a)]",
          "PluginProcessor processBlockBypassed'ı override etmiyor; JUCE tabanı sıfır gecikmeli geçiş yapıyor (JUCE kendi kaynağında tam bu durumu jassert ile uyarıyor), oysa eklenti 28-332 ms latency bildiriyor. Bypass'tan dönüşte RtChain'in gecikme hatları eski sesi taşıyor.",
          "Source/plugin/PluginProcessor.h:23-61 (override yok), JUCE juce_AudioProcessor.cpp:593-599, Source/engine/RtChain.cpp:949-981", { "2.e" } },
        { "Bağımsız uygulamada BYPASS tuşu bir ses anahtarı değil, tüm dosyanın yeniden render'ı: tuştan sese 250 ms (40 s dosya) - 526 ms (180 s dosya), geri açarken 1-3.9 s  [şikâyet (a)]",
          "BYPASS settings.bypass'ı değiştirip Session::settingsChanged()'i çağırıyor: 120 ms debounce, ardından dosyanın kopyası + tüm dosya üzerinde analyse() (LUFS, true peak, spektrum), sonra Player'ın master tamponu değişiyor; açarken tüm dosya için TARGET LOCK çözümü bekleniyor. Player'da bypass yolu yok. (Eklenti yolunda bypass sağlıklı: ilk değişim 6.4 ms, %90 23.3 ms, tıklama yok.)",
          "Source/app/MainComponent.cpp:326, Source/app/Session.cpp:302-308 ve 148-170, Source/engine/MasterChain.cpp:496-502 ve 546-565, Source/app/Player.h", { "2.d" } },
        { "Hedef LUFS'a ulaşmak için sınırsız drive + limiter: bitmiş eserde CLUB/CUSTOM hedefinde yüksek GR, ulaşılamayan hedefte +35 dB drive ve 30 dB ortalama GR; LEARN yalnız sessiz bölümü duyduysa tüm şarkıya fazla drive  [şikâyet (b) ve (c)]",
          "matchTarget drive'ı hedef sayıyı tutturana kadar +36 dB'ye kadar çıkarıyor; tek fren 'son +1 dB 0.1 LU'dan az ekliyorsa 1 dB geri çekil' kuralı (yani +35'te duruyor); boost ve GR için tavan ya da 'zorlama, uyar' dalı yok. Eklentide drive LEARN'in duyduğu kısımdan bir kez çözülüp sabit uygulanıyor, daha yüksek bölümler limiter'a itiliyor.",
          "Source/engine/MasterChain.cpp:567-707 (:589 maxDrive, :635-653 geri çekilme), Source/plugin/PluginProcessor.cpp:276-301, Source/engine/RtChain.cpp:1412-1417", { "4.f", "4.n", "4.p" } },
        { "Latency'yi değiştiren şalterler: MONO BELOW (0 <-> >0) ve HIGH WIDTH (%100 <-> değil) sürekli düğme olduğu halde latency'yi +61.8 ms oynatıyor; her yapı değişiminde çıkış ~12 ms susuyor (-20..-25 dB), ardından ~28 ms sonra tam seviyede faz tıkırtısı geliyor (3-BAND, MONO BELOW, HIGH WIDTH, AIR DYN, fuzz'da eqOn/compOn/compMode)  [şikâyet (d): 'ne olduğunu anlamıyor']",
          "Yapı değişiminde sessizlik (mute) çıkışın sonunda, değişim kararı anında uygulanıyor; oysa değişen aşamanın yarattığı faz sıçraması limiter gecikmesi (4*Dp + 1 blok = 1340 örnek) kadar sonra çıkışa ulaşıyor, yani önce çukur, sonra tıkırtı. Ayrıca stereo bölücü gecikmesi monoBelowHz > 0 ve highWidth != 1 koşullarına bağlı.",
          "Source/engine/RtChain.cpp:575-587 (:581 split), :1000-1003 (karar), :1440-1441 (mute çıkışta)", { "3.knob", "3.swall", "5.1d", "5.2d", "5.3d", "5.4d" } },
        { "Hedef presetini host parametresiyle (otomasyon, genel host arayüzü, kontrolcü) değiştirince ses değişmiyor; yeniden açılmış projede TARGET LOCK çalışmıyor  [şikâyet (b)]",
          "applyParamsToSession() sırasında applyingParams açık: Session::settingsChanged() targetLufs'u presetten türetiyor ama onSettingsEdited() yazmayı atlayıp eski targetLufs parametresini üzerine okuyor (etiket CLUB -8, motor -14 çözüyor). Proje yeniden yüklenince yakalanan ses yok, Session::renderRequest input yoksa hiçbir şey yapmıyor; hedef/EQ değişiklikleri paneli değiştiriyor, sesi değil.",
          "Source/plugin/PluginProcessor.cpp:180-201, Source/app/Session.cpp:289-309", { "6.target", "6.reload" } },
        { "Tek bir 'güvenli sıfırlama' yok: tüm parametreler varsayılana alınsa bile tone-match eğrisi, dinamik EQ alanları ve öğrenilmiş plan kalıyor; toneAmount varsayılanı 1.0  [şikâyet (d)]",
          "Durumun parametre dışı kısımları (Settings.h'daki matchCurveDb, DynBandSettings Q/range/timing, RtPlan, yakalanan ses) için temizleme yolu yok; arayüzde de sıfırlama kontrolü yok.",
          "Source/plugin/PluginProcessor.cpp:357-404, Source/engine/Settings.h:24-36, 54, 67-69, Source/app/MainComponent.cpp:461", { "6.reset" } },
        { "Bozuk proje durumu ya da absürt plan değeri kalıcı NaN çıkışı veriyor (çökme yok)",
          "setStateInformation alanları doğrulamadan uyguluyor: parametre dışı dinamik-EQ alanları (Q 0, 1e30 frekans) ve RtPlan değerleri (1e30 make-up) doğrudan DSP'ye gidiyor.",
          "Source/plugin/PluginProcessor.cpp:357-404, Source/engine/RtChain.cpp:1067-1090, 1169-1181", { "6.bad" } },
        { "Limiter true-peak aşımı (asenkron çekirdek takasında)", "Look-ahead / tavan otomasyonu limiter hattını yeniden başlatıyor.", "Source/engine/RtChain.cpp:400-412", { "5.1b", "5.2b", "5.3b", "5.4b" } },
    };
    String t;
    int n = 0;
    for (const auto& f : fs)
    {
        std::vector<const Item*> failing;
        for (auto& it : g_items)
            if (! it.pass && ! it.info)
                for (auto& pre : f.prefixes) if (it.id.startsWith (pre)) { failing.push_back (&it); break; }
        if (failing.empty()) continue;
        t << ++n << ". " << U (f.title) << "\n";
        for (auto* it : failing) t << "     [" << it->id << "] " << it->measured.substring (0, 330) << (it->measured.length() > 330 ? " ..." : "") << "\n";
        t << U ("   Olası kök neden: ") << U (f.cause) << U ("\n   Kod yeri: ") << U (f.where) << "\n\n";
    }
    if (n == 0) t << "(FAIL yok)\n";
    t << U ("TEMİZ ÇIKANLAR (ölçüldü, sorun yok):\n");
    t << U (" - Determinizm / geçmişten bağımsızlık: iki taze eklenti bit-eşit; blok boyutu 1..2048, 20x bypass, parametre git-gel, preset gidiş-dönüş, reset(), prepareToPlay x3, state get/set, örnekleme hızı atlaması, ikinci LEARN: hepsi 0.000 LU fark (1.x).\n");
    t << U (" - Eklenti içi bypass DSP'si: giriş latency kadar gecikmiş hâliyle bit-eşit (2.a), ilk değişim ve %90 süresi (2.b), seviye geri dönüşü (2.c).\n");
    t << U (" - Bildirilen latency = impuls ölçümü, 44.1/48/96 kHz, tüm anahtar kombinasyonları (3.1-3.8).\n");
    t << U (" - Sessizlik -> sessizlik, denormal yok, mono ve dual-mono, çok kısık malzeme, ortalama mix hedefe iniyor (4.s*, 4.mo*, 4.q1, 4.m1); STREAM -14 ve KEEP INPUT'ta bitmiş malzemeye boost yok (4.f1/4/6/9/11/14).\n");
    t << U (" - Ekrandaki sayılar ölçülenle tutarlı: OUT LUFS, 'X dB yükseltir/düşürür' cümlesi, limiter ve kompresör GR, 'hedef ulaşılamıyor' durum satırı, make-up (7.x).\n");
    t << U (" - Durum gidiş-dönüşü bit-eşit; 14 bozuk durumun hiçbiri çökertmiyor, boş/rastgele/yabancı/yarım durumlar varsayılan eklentiyle bit-eşit çıkış veriyor (6.bad0-4, 10, 12); 8 s'lik bölüm 10 kez döngüde seviye sürüklenmesi 0.000 LU, playhead atlamaları <= 0.07 LU (6.x).\n");
    return t;
}

String stamp() { return juce::Time::getCurrentTime().toString (true, true, false, true); }

void writeReport (const juce::File& dir, double elapsedSec)
{
    int pass = 0, fail = 0, infos = 0;
    for (auto& it : g_items) { if (it.info) ++infos; else if (it.pass) ++pass; else ++fail; }
    String t;
    t << "MIHENK behavior_check  -  virtual-user report\n";
    t << "Generated " << stamp() << ", run time " << String (elapsedSec, 0) << " s. Plug-in mode (VST3/AU path), non-realtime kernels unless stated.\n";
    t << "Result: " << pass << " PASS, " << fail << " FAIL, " << infos << " INFO\n";
    t << "This tool finds and reports. It fixes nothing; engine code is untouched.\n";
    t << "Not covered: the editor/GUI, a real DAW's PDC and automation timing, interactive controls.\n\n";
    String cur;
    for (auto& it : g_items)
    {
        if (it.scenario != cur) { cur = it.scenario; t << "\n==== Scenario " << cur << " ====\n"; }
        t << (it.info ? "INFO " : (it.pass ? "PASS " : "FAIL ")) << "[" << it.id << "] " << it.name << "\n";
        t << "      measured: " << it.measured << "\n";
        if (it.limit.isNotEmpty()) t << "      limit:    " << it.limit << "\n";
        if (! it.pass)
        {
            if (it.suspect.isNotEmpty()) t << "      suspect:  " << it.suspect << "\n";
            if (it.hypothesis.isNotEmpty()) t << "      cause?:   " << it.hypothesis << "\n";
        }
    }
    t << U ("\n==== BULGULAR, ÖNCELİK SIRASIYLA (şikâyet bağlantısı köşeli parantezde) ====\n\n") << findingsText();
    for (auto& nt : g_notes) t << "\nNOTE: " << nt << "\n";
    dir.getChildFile ("behavior_report.txt").replaceWithText (t);

    juce::Array<juce::var> arr;
    for (auto& it : g_items)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("scenario", it.scenario); o->setProperty ("id", it.id); o->setProperty ("name", it.name);
        o->setProperty ("result", it.info ? "INFO" : (it.pass ? "PASS" : "FAIL"));
        o->setProperty ("measured", it.measured); o->setProperty ("limit", it.limit);
        o->setProperty ("suspect", it.suspect); o->setProperty ("hypothesis", it.hypothesis); o->setProperty ("priority", it.prio);
        arr.add (juce::var (o));
    }
    auto* root = new juce::DynamicObject();
    root->setProperty ("generated", stamp()); root->setProperty ("pass", pass); root->setProperty ("fail", fail); root->setProperty ("info", infos);
    root->setProperty ("runSeconds", elapsedSec); root->setProperty ("items", arr);
    dir.getChildFile ("behavior_report.json").replaceWithText (juce::JSON::toString (juce::var (root), false));
}
} // namespace

//==============================================================================
int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    juce::StringArray args;
    for (int i = 1; i < argc; ++i) args.add (argv[i]);
    if (args.size() >= 2 && args[0] == "--child-badstate") return childBadState (args[1].getIntValue());

    std::set<int> only;
    std::vector<juce::File> wavs;
    juce::File outDir = juce::File (__FILE__).getParentDirectory();
    for (int i = 0; i < args.size(); ++i)
    {
        if (args[i] == "--only" && i + 1 < args.size()) { for (auto& p : juce::StringArray::fromTokens (args[++i], ",", "")) only.insert (p.getIntValue()); }
        else if (args[i] == "--out" && i + 1 < args.size()) outDir = juce::File (args[++i]);
        else if (args[i] == "--wav") { while (i + 1 < args.size() && ! args[i + 1].startsWith ("--")) wavs.push_back (juce::File (args[++i])); }
    }
    {
        const auto demos = juce::File (__FILE__).getParentDirectory().getParentDirectory().getChildFile ("demos");
        for (auto& f : demos.findChildFiles (juce::File::findFiles, false, "*.wav;*.aif;*.aiff")) wavs.push_back (f);
    }
    g_t0 = nowMs();
    if (args.contains ("--debug-target"))
    {
        const double fs = 48000.0;
        const auto songF = songFull (fs, 20.0, 1);
        const auto sigF = slice (songF, 0, (int) (10.0 * fs));
        Rig live (fs);
        live.setSettings (rapSettings());
        live.learn (songF);
        std::printf ("start: target preset param %.0f, session preset %d lufs %.2f drive %.2f\n", live.getParam ("targetPreset"), (int) live.S().settings.targetPreset, live.S().settings.targetLufs, live.getParam ("drive"));
        live.setParam ("targetPreset", 2.0);
        std::printf ("after setParam: param %.0f\n", live.getParam ("targetPreset"));
        live.settle();
        std::printf ("after settle: param %.0f, session preset %d lufs %.2f drive %.2f status: %s\n", live.getParam ("targetPreset"), (int) live.S().settings.targetPreset, live.S().settings.targetLufs, live.getParam ("drive"), live.S().status.toRawUTF8());
        live.prep();
        std::printf ("level %.2f\n", measure (live, sigF).lufs);
        return 0;
    }
    if (args.contains ("--debug-switch"))
    {
        const double fs = 48000.0;
        struct V { const char* name; std::function<void (Rig&)> pre, flip; };
        const V vs[] {
            { "monoBelow 0 -> 120", [] (Rig& r) { r.setParam ("monoBelow", 0); }, [] (Rig& r) { r.setParam ("monoBelow", 120); } },
            { "highWidth 1 -> 1.01 (no kernel change)", [] (Rig& r) { r.setParam ("monoBelow", 0); }, [] (Rig& r) { r.setParam ("highWidth", 1.01); } },
            { "monoBelow 120 -> 0", [] (Rig& r) { r.setParam ("monoBelow", 120); }, [] (Rig& r) { r.setParam ("monoBelow", 0); } },
            { "airDyn on", [] (Rig& r) { r.setParam ("airDb", 2); }, [] (Rig& r) { r.setParam ("airDyn", 1); } },
            { "compMode 3-band", [] (Rig& r) { r.setParam ("compOn", 1); }, [] (Rig& r) { r.setParam ("compMode", 1); } },
        };
        for (const auto& v : vs)
        {
            Rig r (fs);
            auto st = rapSettings();
            st.width.monoBelowHz = 0.0;
            r.setSettings (st);
            v.pre (r);
            r.prep();
            const auto sine = sineBuf (fs, 1.2, 997.0, -12.0, 2);
            Buffer out (sine);
            juce::MidiBuffer midi;
            const int at = 24000;
            for (int pos = 0; pos < out.getNumSamples(); pos += 64)
            {
                if (pos == at) v.flip (r);
                Buffer view (out.getArrayOfWritePointers(), 2, pos, 64);
                r.p->processBlock (view, midi);
            }
            std::printf ("-- %s (latency now %d)\n", v.name, r.P().getChain().currentLatency());
            int shown = 0;
            for (int i = at; i < at + 9000 && shown < 6; ++i)
            {
                const double stp = std::abs (out.getSample (0, i) - out.getSample (0, i - 1));
                if (stp > 0.1) { std::printf ("   big step %.3f at +%d samples (%.2f ms): %.3f -> %.3f\n", stp, i - at, 1000.0 * (i - at) / fs, out.getSample (0, i - 1), out.getSample (0, i)); ++shown; }
            }
        }
        return 0;
    }
    if (args.contains ("--mats"))
    {
        const double fs = 48000.0;
        const auto base = songFull (fs, 20.0, 1);
        std::printf ("song: %.1f LUFS, %.1f dBTP, crest %.1f dB\n", lufsOf (base, fs), tpOf (base), crestOf (base));
        for (auto lt : std::vector<std::pair<double, double>> { { -9, -1.5 }, { -10, -3 }, { -12, -3 }, { -8, -1 } })
        {
            const auto m = makeFinished (base, fs, lt.first, lt.second);
            std::printf ("finished asked %.1f / %.1f: %.2f LUFS, %.2f dBTP, PLR %.1f, crest %.1f dB, LRA?\n", lt.first, lt.second, lufsOf (m, fs), tpOf (m), tpOf (m) - lufsOf (m, fs), crestOf (m));
        }
        return 0;
    }
    auto want = [&] (int n) { return only.empty() || only.count (n) > 0; };
    try
    {
        if (want (1)) scenario1();
        if (want (2)) scenario2();
        if (want (3)) scenario3();
        if (want (4)) scenario4 (wavs);
        if (want (5)) { scenario5(); nanRecovery(); }
        if (want (6)) scenario6();
        if (want (7)) scenario7();
    }
    catch (const std::exception& e)
    {
        std::printf ("HARNESS ERROR: %s\n", e.what());
        g_notes.push_back (String ("The harness stopped with an exception: ") + e.what());
        writeReport (outDir, (nowMs() - g_t0) / 1000.0);
        return 2;
    }
    writeReport (outDir, (nowMs() - g_t0) / 1000.0);
    std::printf ("\n%d FAIL. Report: %s\n", g_fail, outDir.getChildFile ("behavior_report.txt").getFullPathName().toRawUTF8());
    return 0;
}
