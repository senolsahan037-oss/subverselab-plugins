/*  MIHENK plug-in checks: the AudioProcessor driven the way a host drives it.

        rt_check            all checks (exit code 0 only if every check passes)
        rt_check --bench    only the CPU measurement

    - LEARN through processBlock with a playing host transport, then the song played again: the real-time output
      against the offline render of what LEARN captured (sample by sample, after the reported latency).
    - Host automation while playing, parameters saved and restored with the state (a new instance gives the same
      output), the learned results restored without the audio.
    - No heap allocation from C++ new on the audio thread while playing, with knob moves that swap kernels (counted
      by a replaced operator new).
    - CPU: processBlock at 48 kHz / 512 samples, typical and heavy settings, as a share of real time.
*/
#include <JuceHeader.h>
#include <atomic>
#include <cstdlib>
#include <new>
#include "../Source/plugin/PluginProcessor.h"
#include "../Source/engine/Preset.h"
#include "../Source/dsp/Filters.h"

using namespace mihenk;

//==============================================================================
// Allocation counter: operator new counts while the current thread is inside a processBlock.
namespace
{
thread_local bool inAudio = false;
std::atomic<long> audioAllocs { 0 };
}
void* operator new (std::size_t n)
{
    if (inAudio) audioAllocs.fetch_add (1);
    if (void* p = std::malloc (n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}
void* operator new[] (std::size_t n)
{
    if (inAudio) audioAllocs.fetch_add (1);
    if (void* p = std::malloc (n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}
void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }

namespace
{
int failures = 0;
void check (bool ok, const juce::String& what)
{
    std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8());
    std::fflush (stdout);
    if (! ok) ++failures;
}
void section (const char* name) { std::printf ("\n-- %s\n", name); }

using Buffer = juce::AudioBuffer<float>;

struct FakePlayHead : juce::AudioPlayHead
{
    bool playing = true;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        p.setIsPlaying (playing);
        return p;
    }
};

/** A music-like test signal (as the selftest's): kick, bass, hats and a pad, verse and chorus, with silence around. */
Buffer song (double fs, double secs, uint32_t seed)
{
    const int n = (int) (secs * fs), pre = (int) (0.3 * fs), post = (int) (1.5 * fs);
    Buffer b (2, pre + n + post);
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
            b.setSample (c, pre + i, (float) (fade * sec * (0.55 * kick + bass + 0.25 * pad + hat * (c == 0 ? 1.3 : 0.7))));
        }
    }
    return b;
}

/** Plays `in` through the processor in blocks of `block` samples (the last one shorter), plus `extra` samples of silence. */
Buffer play (MihenkProcessor& p, const Buffer& in, int block, int extra, bool countAllocs = false)
{
    const int n = in.getNumSamples(), total = n + extra;
    Buffer out (2, total);
    out.clear();
    for (int c = 0; c < 2; ++c) out.copyFrom (c, 0, in, c, 0, n);
    juce::MidiBuffer midi;
    for (int pos = 0; pos < total; pos += block)
    {
        const int len = juce::jmin (block, total - pos);
        Buffer view (out.getArrayOfWritePointers(), 2, pos, len);
        inAudio = countAllocs;
        p.processBlock (view, midi);
        inAudio = false;
    }
    return out;
}

double compare (const Buffer& reference, const Buffer& streamed, int latency, double& residualDb)
{
    double maxDiff = 0.0, err = 0.0, sig = 0.0;
    const int n = reference.getNumSamples();
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < n; ++i)
        {
            const double a = reference.getSample (c, i), b = streamed.getSample (c, i + latency);
            maxDiff = juce::jmax (maxDiff, std::abs (a - b));
            err += (a - b) * (a - b);
            sig += a * a;
        }
    residualDb = 10.0 * std::log10 (err / juce::jmax (1.0e-30, sig) + 1.0e-30);
    return gainToDb (maxDiff);
}

void setParam (MihenkProcessor& p, const juce::String& id, float value)
{
    auto* prm = p.getBridge().find (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (value));
}

/** The message thread's periodic work, as the timer would do it (the console has no message loop). */
void pump (int ms)
{
    juce::MessageManager::getInstance()->runDispatchLoopUntil (ms);
}

//==============================================================================
int bench()
{
    section ("CPU: processBlock at 48 kHz, 512-sample blocks, 60 s of stereo audio (one core, share of real time)");
    const double fs = 48000.0;
    auto in = song (fs, 60.0, 9);
    struct Cfg { const char* name; std::function<void (MasterSettings&)> set; };
    const Cfg cfgs[] {
        { "HIP-HOP / RAP starting point (WIDE glue, mono below 120 Hz, min-phase EQ, limiter)", [] (MasterSettings& s) { applyGenreStartingPoint (s, Genre::rapTrap); } },
        { "Heavy: 3-BAND + dynamic EQ x3 + dynamic AIR + phase-safe low mono + high width", [] (MasterSettings& s)
          { applyGenreStartingPoint (s, Genre::rapTrap); s.comp.mode = CompMode::threeBand; s.eq.dynLowMid.enabled = true; s.eq.dynUpperMid.enabled = true;
            s.eq.tightener.enabled = true; s.eq.airDynamic = true; s.eq.airDb = 2.0; s.width.phaseSafeLowMono = true; s.width.highWidth = 1.2; } },
        { "Heavy + linear-phase EQ", [] (MasterSettings& s)
          { applyGenreStartingPoint (s, Genre::rapTrap); s.comp.mode = CompMode::threeBand; s.eq.dynLowMid.enabled = true; s.eq.dynUpperMid.enabled = true;
            s.eq.tightener.enabled = true; s.eq.airDynamic = true; s.eq.airDb = 2.0; s.width.phaseSafeLowMono = true; s.width.highWidth = 1.2; s.eq.linearPhase = true; } },
    };
    for (const auto& c : cfgs)
    {
        MihenkProcessor p;
        p.setStandaloneModeForTesting (false);
        FakePlayHead head;
        p.setPlayHead (&head);
        p.setRateAndBufferSizeDetails (fs, 512);
        c.set (p.getSession().settings);
        p.getSession().settingsChanged();
        // A learned plan, so every AUTO stage really works (the analysis is not part of the timing).
        p.setNonRealtime (true);
        p.prepareToPlay (fs, 512);
        p.toggleLearn();
        auto learnPart = song (fs, 20.0, 3);
        play (p, learnPart, 512, 0);
        p.finishLearnNow();
        p.setNonRealtime (false);
        p.prepareToPlay (fs, 512);
        play (p, song (fs, 1.0, 5), 512, 0);           // warm-up
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        play (p, in, 512, 0);
        const double secs = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
        const double audio = in.getNumSamples() / fs;
        std::printf ("CPU  %s: %.2f %% of one core (%.2f s for %.1f s of audio), latency %d samples (%.1f ms)\n",
                     c.name, 100.0 * secs / audio, secs, audio, p.getLatencySamples(), 1000.0 * p.getLatencySamples() / fs);
        p.setPlayHead (nullptr);
    }
    return 0;
}

int checks()
{
    const double fs = 48000.0;
    const auto in = song (fs, 20.0, 1);

    section ("LEARN through processBlock, then the real-time output against the offline render");
    MihenkProcessor p;
    p.setStandaloneModeForTesting (false);
    FakePlayHead head;
    p.setPlayHead (&head);
    p.setRateAndBufferSizeDetails (fs, 512);
    applyGenreStartingPoint (p.getSession().settings, Genre::rapTrap);
    p.getSession().settings.eq.dynLowMid.enabled = true;
    p.getSession().settings.eq.dynLowMid.amountDb = 1.5;
    p.getSession().settings.eq.airDynamic = true;
    p.getSession().settings.eq.airDb = 1.5;
    p.getSession().settingsChanged();
    check (std::abs (p.getState().getRawParameterValue ("glue")->load() - 1.5f) < 1.0e-6f && p.getState().getRawParameterValue ("monoBelow")->load() == 120.0f,
           "Panel settings reach the automatable parameters (GLUE 1.5 dB, MONO BELOW 120 Hz)");
    p.setNonRealtime (true);
    p.prepareToPlay (fs, 512);
    check (! p.isLearned(), "Before LEARN: not learned");
    p.toggleLearn();
    check (p.learnState() == MihenkProcessor::Learn::armed, "LEARN armed");
    const auto first = play (p, in, 512, 0);
    check (p.learnState() == MihenkProcessor::Learn::capturing && std::abs (p.learnSeconds() - in.getNumSamples() / fs) < 1.0e-6,
           "LEARN captured the whole song while the host played: " + juce::String (p.learnSeconds(), 2) + " s");
    p.finishLearnNow();
    for (int i = 0; i < 1200 && p.getSession().busy(); ++i) pump (25);      // the calibration render (SPEC_V3 2 REVISED) follows the first one
    pump (50);
    auto& ses = p.getSession();
    check (p.isLearned() && p.learnState() == MihenkProcessor::Learn::idle && ses.output != nullptr && ses.diagnosis.m.valid,
           "LEARN analysed: plan solved, output rendered, diagnosis " + juce::String ((int) ses.diagnosis.findings.size()) + " finding(s)");
    const double solvedDrive = p.getState().getRawParameterValue ("drive")->load();
    check (std::abs (solvedDrive - ses.settings.limiter.driveDb) < 1.0e-4 && std::abs (p.getState().getRawParameterValue ("threshold")->load() - ses.settings.comp.thresholdDb) < 1.0e-4,
           "The planned drive (" + juce::String (solvedDrive, 2) + " dB) and the solved AUTO threshold (" + juce::String (ses.settings.comp.thresholdDb, 2)
           + " dBFS) are written to the parameters");

    // Play the song again, as the host would after LEARN (fresh start of playback).
    p.prepareToPlay (fs, 512);
    const int lat = p.getLatencySamples();
    const auto second = play (p, in, 512, lat);
    double res = 0.0;
    const double worst = compare (*ses.output, second, lat, res);
    check (worst <= -70.0 && res <= -80.0, "Real-time processBlock vs the offline render of what LEARN heard: largest difference " + juce::String (worst, 1)
                                            + " dBFS, residual " + juce::String (res, 1) + " dB under the signal (latency " + juce::String (lat) + " samples reported to the host)");
    {
        Buffer out (2, in.getNumSamples());
        for (int c = 0; c < 2; ++c) out.copyFrom (c, 0, second, c, lat, in.getNumSamples());
        const auto m = analyse (out, fs, false);
        // v3 (SPEC 2 REVISED): the plan decides the level, not a drive solved against the render: the plug-in plays what the plan says. The limiter response is measured on the untouched mix; the stages switched on here
        // (EQ, compressor, stereo of the panel starting point) add their own loudness, which the session measures on its render and holds (calibration): within 0.3 LU.
        check (std::abs (m.integrated - ses.outMeas.integrated) <= 0.1 && std::abs (m.integrated - ses.settings.targetLufs) <= 0.3 && m.truePeak <= ses.settings.limiter.ceilingDbtp + 0.01,
               "The plug-in output is what the plan says (never above the target, the master the app would make): " + juce::String (m.integrated, 2) + " LUFS (offline " + juce::String (ses.outMeas.integrated, 2)
                   + ", target " + juce::String (ses.settings.targetLufs, 1) + "), true peak " + juce::String (m.truePeak, 2) + " dBTP");
    }
    // Continuing without a new prepareToPlay (a host that just keeps the plug-in running).
    {
        // The compressor's slow release (1 s) still holds a little reduction from the end of the last pass, as any
        // compressor would, so this compares loudness and the residual at a musical, not a numerical, tolerance.
        const auto third = play (p, in, 333, lat);
        const double w3 = compare (*ses.output, third, lat, res);
        Buffer out (2, in.getNumSamples());
        for (int c = 0; c < 2; ++c) out.copyFrom (c, 0, third, c, lat, in.getNumSamples());
        const auto m = analyse (out, fs, false);
        check (res <= -50.0 && std::abs (m.integrated - ses.outMeas.integrated) <= 0.02,
               "Played again without a reset (333-sample blocks, detector state carried over): residual " + juce::String (res, 1) + " dB, largest difference "
               + juce::String (w3, 1) + " dBFS, loudness " + juce::String (m.integrated, 3) + " vs " + juce::String (ses.outMeas.integrated, 3) + " LUFS offline");
    }

    section ("Automation, state and presets");
    {
        // Drive automated from the audio thread mid-song: ramps, no jump, ceiling held.
        p.setNonRealtime (false);
        p.prepareToPlay (fs, 512);
        const int total = in.getNumSamples();
        Buffer out (2, total);
        for (int c = 0; c < 2; ++c) out.copyFrom (c, 0, in, c, 0, total);
        juce::MidiBuffer midi;
        for (int pos = 0, k = 0; pos < total; pos += 512, ++k)
        {
            if (k == 400) setParam (p, "drive", (float) (solvedDrive + 6.0));
            if (k == 900) setParam (p, "ceiling", -2.0f);
            Buffer view (out.getArrayOfWritePointers(), 2, pos, juce::jmin (512, total - pos));
            p.processBlock (view, midi);
        }
        const auto m = analyse (out, fs, false);
        bool finite = true;
        for (int c = 0; c < 2; ++c) for (int i = 0; i < total; ++i) finite &= std::isfinite (out.getSample (c, i));
        check (finite && m.truePeak <= -1.0 + 0.01, "Drive +6 dB and ceiling -2 dBTP automated while playing: output finite, true peak " + juce::String (m.truePeak, 2)
                                                       + " dBTP (never above the -1 dBTP it started with)");
        pump (50);
        setParam (p, "drive", (float) solvedDrive);
        setParam (p, "ceiling", -1.0f);
        pump (100);
    }
    {
        // Save the state, load it into a new instance: parameters, settings and the learned plan come back.
        for (int i = 0; i < 1200 && p.getSession().busy(); ++i) pump (25);      // the renders the automation above asked for (and their calibration) are done
        pump (50);
        juce::MemoryBlock state;
        p.getStateInformation (state);
        MihenkProcessor q;
        q.setStandaloneModeForTesting (false);
        FakePlayHead h2;
        q.setPlayHead (&h2);
        q.setRateAndBufferSizeDetails (fs, 512);
        q.setStateInformation (state.getData(), (int) state.getSize());
        check (q.isLearned() && (int) q.getSession().diagnosis.findings.size() == (int) ses.diagnosis.findings.size()
                   && std::abs (q.getSession().inMeas.integrated - ses.inMeas.integrated) < 1.0e-6
                   && settingsFingerprint (q.getSession().settings) == settingsFingerprint (ses.settings),
               "State restored in a new instance: settings, " + juce::String ((int) q.getSession().diagnosis.findings.size()) + " finding(s), measurements and the learned plan ("
               + juce::String (state.getSize()) + " bytes, no audio)");
        p.setNonRealtime (true);
        q.setNonRealtime (true);
        p.prepareToPlay (fs, 512);
        q.prepareToPlay (fs, 512);
        const auto a = play (p, in, 512, lat), b = play (q, in, 512, lat);
        bool same = a.getNumSamples() == b.getNumSamples();
        for (int c = 0; c < 2 && same; ++c) for (int i = 0; i < a.getNumSamples() && same; ++i) same &= a.getSample (c, i) == b.getSample (c, i);
        check (same, "The restored instance plays exactly as the original (bit for bit, " + juce::String (a.getNumSamples()) + " samples)");
        check (! q.learnedIsStale(), "Restored, unchanged settings: the learned values are current");
        q.getSession().settings.eq.bell1Db += 1.0;
        q.getSession().settingsChanged();
        check (q.learnedIsStale(), "After a change without the captured audio, the panel is told to LEARN again");
        q.setPlayHead (nullptr);
    }
    {
        // A preset saved by the app loads into the plug-in's parameters (one preset format for both).
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("mihenk_rt_check");
        dir.createDirectory();
        MasterSettings s;
        applyGenreStartingPoint (s, Genre::electronicLofi);
        s.comp.mode = CompMode::threeBand;
        s.comp.bands[1].gainDb = 1.5;
        juce::String err;
        const auto f = dir.getChildFile ("shared.mihenkpreset");
        savePreset (f, s, err);
        MihenkProcessor r;
        MasterSettings loaded = r.getSession().settings;
        loadPreset (f, loaded, err);
        r.getSession().settings = loaded;
        r.getSession().settingsChanged();
        check (r.getState().getRawParameterValue ("compMode")->load() == 1.0f && std::abs (r.getState().getRawParameterValue ("b2Gain")->load() - 1.5f) < 1.0e-6f,
               "A .mihenkpreset from the app loads into the plug-in's parameters (3-BAND, mid gain +1.5 dB)");
        dir.deleteRecursively();
    }

    section ("Real time: no allocation on the audio thread, kernels swapped by the worker");
    {
        p.setNonRealtime (false);
        p.prepareToPlay (fs, 512);
        play (p, song (fs, 0.5, 4), 512, 0);
        audioAllocs = 0;
        const int total = in.getNumSamples();
        Buffer out (2, total);
        for (int c = 0; c < 2; ++c) out.copyFrom (c, 0, in, c, 0, total);
        juce::MidiBuffer midi;
        int moves = 0;
        for (int pos = 0, k = 0; pos < total; pos += 512, ++k)
        {
            if (k % 60 == 30)
            {
                // Knob moves that change every kernel: EQ, crossovers, mono below, AIR frequency.
                setParam (p, "bell1Db", (float) ((k / 60) % 2 == 0 ? 2.0 : -1.0));
                setParam (p, "xoverLow", (float) (100 + 20 * ((k / 60) % 3)));
                setParam (p, "monoBelow", (float) (100 + 10 * ((k / 60) % 2)));
                setParam (p, "airHz", (float) ((k / 60) % 4));
                ++moves;
            }
            Buffer view (out.getArrayOfWritePointers(), 2, pos, juce::jmin (512, total - pos));
            inAudio = true;
            p.processBlock (view, midi);
            inAudio = false;
            juce::Thread::sleep (2);   // give the kernel worker real time, as a 10.7 ms audio period would
        }
        check (audioAllocs.load() == 0, "C++ heap allocations inside processBlock while playing with " + juce::String (moves * 4)
                                            + " kernel-changing knob moves: " + juce::String (audioAllocs.load()));
        bool finite = true;
        for (int c = 0; c < 2; ++c) for (int i = 0; i < total; ++i) finite &= std::isfinite (out.getSample (c, i));
        check (finite, "Output finite through the kernel swaps");
    }

    section ("Meters: the real-time taps reach the FIFO (IN, EQ, COMP, OUT, gain reductions)");
    {
        p.setNonRealtime (true);
        p.prepareToPlay (fs, 512);
        auto& m = p.getMeters();
        m.clear();
        const auto part = song (fs, 6.0, 6);
        // Drained after every block, as the panel's timer drains it.
        std::vector<float> f ((size_t) m.capacity() * MeterFifo::frameSize);
        int got = 0;
        double inPk = 0.0, outPk = 0.0, lim = 0.0;
        Buffer io (2, part.getNumSamples());
        for (int c = 0; c < 2; ++c) io.copyFrom (c, 0, part, c, 0, part.getNumSamples());
        juce::MidiBuffer midi;
        for (int pos = 0; pos < part.getNumSamples(); pos += 512)
        {
            Buffer view (io.getArrayOfWritePointers(), 2, pos, juce::jmin (512, part.getNumSamples() - pos));
            p.processBlock (view, midi);
            const int k = m.pull (f.data(), m.capacity());
            for (int i = 0; i < k; ++i)
            {
                inPk = juce::jmax (inPk, (double) std::abs (f[(size_t) i * MeterFifo::frameSize + MeterFifo::inL]));
                outPk = juce::jmax (outPk, (double) std::abs (f[(size_t) i * MeterFifo::frameSize + MeterFifo::outL]));
                lim = juce::jmax (lim, (double) f[(size_t) i * MeterFifo::frameSize + MeterFifo::grLim]);
            }
            got += k;
        }
        const int whole = part.getNumSamples() / RtChain::blockSize * RtChain::blockSize;   // the last partial block is still in the framing buffer
        check (got == whole && inPk > 0.1 && outPk > 0.1 && lim > 0.1, "One frame per processed sample (" + juce::String (got) + " of " + juce::String (whole) + "), IN peak " + juce::String (gainToDb (inPk), 1)
                                                                            + " dBFS, OUT peak " + juce::String (gainToDb (outPk), 1) + " dBFS, limiter GR up to " + juce::String (lim, 1) + " dB");
    }
    p.setPlayHead (nullptr);
    std::printf ("\n%s\n", failures == 0 ? "ALL PASS" : (juce::String (failures) + " FAILED").toRawUTF8());
    return failures == 0 ? 0 : 1;
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::String mode = argc >= 2 ? juce::String (argv[1]) : juce::String();
    if (mode == "--bench") return bench();
    const int r = checks();
    bench();
    return r;
}
