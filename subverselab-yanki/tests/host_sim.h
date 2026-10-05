#pragma once
/*  A host-like harness for the YANKI processor.

    - the audio thread runs processBlock continuously on a stationary noise
      source (what a DAW does), reading the host tempo from a fake playhead;
    - the message thread (the thread that owns the MessageManager) moves
      parameters with setValueNotifyingHost inside begin/endChangeGesture, as
      the editor and a host UI do, and pumps the dispatch loop so the
      AsyncUpdater of the processor runs;
    - an "automation" thread writes parameters from a third thread;
    - probe() measures whether the output still follows MIX: the energy of
      (output - input) in a window after a MIX change, in samples of audio, so
      the result does not depend on how fast the machine is.

    Used by tests/host_sim_main.cpp (long run, prints the trace) and by the
    regression check in render --selftest. */
#include <JuceHeader.h>
#include <atomic>
#include <thread>

namespace hostsim
{
namespace ids
{
    inline constexpr const char* mood = "MOOD", *timeSync = "TIME_SYNC", *timeMs = "TIME_MS", *feedback = "FEEDBACK", *capOn = "CAP_ON",
        *capDb = "CAP_DB", *tone = "TONE", *wear = "WEAR", *mix = "MIX", *output = "OUTPUT", *endMode = "END_MODE",
        *endTrigger = "END_TRIGGER", *freeze = "FREEZE", *bypass = "BYPASS", *darken = "DARKEN", *riseCurve = "RISE_CURVE",
        *lockBars = "LOCK_BARS", *fineMs = "FINE_MS", *spread = "SPREAD";
}
// Parameter access that works on the processor itself (APVTS) and on a plug-in
// hosted through its AU / VST3 wrapper (found by name).
struct Params
{
    virtual ~Params() = default;
    virtual juce::AudioProcessorParameter* get (const char* id) = 0;
    /** normalised position of a plain value (only the ones the probe needs) */
    virtual float norm (const char* id, float plain) = 0;
};

struct Host : juce::AudioPlayHead
{
    std::atomic<double> bpm { 120.0 };
    std::atomic<double> ppq { 0.0 };
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setBpm (bpm.load());
        info.setIsPlaying (true);
        info.setPpqPosition (ppq.load());
        return info;
    }
};

class Rig
{
public:
    explicit Rig (juce::AudioProcessor& proc, Params& prm, double sampleRate = 48000.0, int block = 512, bool paced = true)
                : p (proc), params (prm), sr (sampleRate), blk (block), pace (paced)
    {
        p.setPlayHead (&host);
        p.setRateAndBufferSizeDetails (sr, blk);
        p.prepareToPlay (sr, blk);
    }
    ~Rig() { stop(); p.setPlayHead (nullptr); }

    void start()
    {
        if (running.exchange (true)) return;
        audio = std::thread ([this] { loop(); });
    }
    void stop()
    {
        if (! running.exchange (false)) return;
        if (audio.joinable()) audio.join();
    }

    /** Samples the audio thread has processed. */
    long long samples() const { return done.load(); }
    double sampleRate() const { return sr; }
    int blockSize() const { return blk; }

    /** Waits until the audio thread has processed `n` more samples, pumping
        the message loop meanwhile (the AsyncUpdater needs it). */
    void waitSamples (long long n)
    {
        const auto target = samples() + n;
        const auto t0 = juce::Time::getMillisecondCounter();
        while (samples() < target && juce::Time::getMillisecondCounter() - t0 < 20000)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (1);
    }

    /** Energy of (out - in) per sample, accumulated between mark() and read(). */
    struct Window { double diff = 0, out = 0, in = 0; long long n = 0; };
    Window takeWindow()
    {
        std::lock_guard<std::mutex> l (m);
        Window w = acc;
        acc = {};
        return w;
    }

    /** Silence the input (a gap between phrases re-arms END/swell, as music does). */
    std::atomic<bool> gap { false };
    /** 0 coloured noise, 1 hot (clipped) noise, 2 DC + noise, 3 +-1 impulses, 4 100 Hz full-scale square, 5 1e-20 noise,
        6 noise with one NaN sample, 7 noise with one +inf sample, 8 +-4 (over full scale) noise */
    std::atomic<int> inputKind { 0 };

    juce::AudioProcessor& p;
    Params& params;
    Host host;

private:
    void loop()
    {
        juce::AudioBuffer<float> buf (2, blk), in (2, blk);
        juce::MidiBuffer midi;
        juce::Random rnd (7);
        double lp = 0.0;
        long long sampleCount = 0;
        while (running.load())
        {
            // A stationary, coloured noise at about -20 dBFS: a signal that is always in the tape.
            for (int i = 0; i < blk; ++i)
            {
                lp += 0.35 * ((double) rnd.nextFloat() * 2.0 - 1.0 - lp);
                float v = gap.load() ? 0.0f : (float) (0.18 * lp);
                switch (inputKind.load())
                {
                    case 1: v = juce::jlimit (-1.0f, 1.0f, v * 8.0f); break;
                    case 2: v = v + 0.5f; break;
                    case 3: v = ((sampleCount + i) % 14400 == 0) ? 1.0f : (((sampleCount + i) % 14400 == 7200) ? -1.0f : 0.0f); break;
                    case 4: v = (((sampleCount + i) / 240) % 2) ? 1.0f : -1.0f; break;
                    case 5: v = v * 1.0e-19f; break;
                    case 6: if ((sampleCount + i) % 100000 == 777) v = std::numeric_limits<float>::quiet_NaN(); break;
                    case 7: if ((sampleCount + i) % 100000 == 777) v = std::numeric_limits<float>::infinity(); break;
                    case 8: v = v * 25.0f; break;
                    default: break;
                }
                in.setSample (0, i, v);
                in.setSample (1, i, v);
            }
            buf.makeCopyOf (in);
            p.processBlock (buf, midi);
            Window w;
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < blk; ++i)
                {
                    const double o = buf.getSample (c, i), x = in.getSample (c, i);
                    w.diff += (o - x) * (o - x);
                    w.out += o * o;
                    w.in += x * x;
                }
            w.n = 2 * blk;
            {
                std::lock_guard<std::mutex> l (m);
                acc.diff += w.diff; acc.out += w.out; acc.in += w.in; acc.n += w.n;
            }
            sampleCount += blk;
            host.ppq.store (host.ppq.load() + blk / sr * host.bpm.load() / 60.0);
            done.fetch_add (blk);
            if (pace) std::this_thread::sleep_for (std::chrono::microseconds (700));
        }
    }

    double sr;
    int blk;
    bool pace;
    std::atomic<bool> running { false };
    std::atomic<long long> done { 0 };
    std::thread audio;
    std::mutex m;
    Window acc;
};

inline void move (Rig& rig, const char* id, float normalised)
{
    if (auto* param = rig.params.get (id))
    {
        param->beginChangeGesture();
        param->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, normalised));
        param->endChangeGesture();
    }
}

inline float norm (Rig& rig, const char* id, float plain) { return rig.params.norm (id, plain); }

struct ProbeResult
{
    double diffDbAt0 = -200, diffDbAtMix = -200;   // energy of (out - in), dB re the input energy
    double riseDb() const { return diffDbAtMix - diffDbAt0; }
    bool follows() const { return diffDbAtMix > -30.0 && riseDb() > 12.0; }
};

/** Puts the "state" parameters into a plain position (bypass off, freeze off,
    END trigger off, OUTPUT 0 dB) and checks that MIX still drives the output:
    MIX 0 -> the output is the input (diff tiny); MIX 0.3 -> an audible wet
    part within 75 ms of audio. TIME is left where the run put it. */
inline ProbeResult probe (Rig& rig, bool rearmGap = true)
{
    const long long ms = (long long) (rig.sampleRate() / 1000.0);   // samples per ms
    move (rig, ids::bypass, 0.0f);
    move (rig, ids::freeze, 0.0f);
    move (rig, ids::endTrigger, 0.0f);
    if (rearmGap) move (rig, ids::endMode, 0.0f);   // HOLD: a CUT/DUMP that fired keeps the loop shut until a new phrase (by design)
    move (rig, ids::output, norm (rig, ids::output, 0.0f));
    move (rig, ids::mix, 0.0f);
    if (rearmGap)
    {
        rig.gap = true;                      // a gap between phrases: END / swell re-arm as they do for music
        rig.waitSamples (1500 * ms);
        rig.gap = false;
    }
    rig.waitSamples (5000 * ms);             // settle: TIME glide, the tape refilling after a 1/2 note switch (up to 3 s), smoothers
    rig.takeWindow();
    rig.waitSamples (100 * ms);
    auto w0 = rig.takeWindow();
    move (rig, ids::mix, norm (rig, ids::mix, 0.3f));
    rig.waitSamples (25 * ms);
    rig.takeWindow();                        // the first 25 ms of the ramp is not scored
    rig.waitSamples (50 * ms);
    auto w1 = rig.takeWindow();              // 25..75 ms after the move
    ProbeResult r;
    r.diffDbAt0 = 10.0 * std::log10 (std::max (w0.diff, 1e-30) / std::max (w0.in, 1e-30));
    r.diffDbAtMix = 10.0 * std::log10 (std::max (w1.diff, 1e-30) / std::max (w1.in, 1e-30));
    return r;
}
} // namespace hostsim
