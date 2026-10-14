// KIYMA engine v6: the ChopperLab step engine, reading the channel instead of a dropped file. Plain C++, no JUCE.
// Specification: docs/DSP_V6_CHOPPER.md. The sound is the owner's first ChopperLab Max device (2026-08-26, recovered in
// recovered_chopperlab/): the material is squeezed into one bar on the transport grid, the bar is cut into steps, and a fixed map says
// which slice each step reads. The map comes from chopper.js's generator (mulberry32, density, reach, seed), so seed 1 here is seed 1 there.
//
//   in -> ring (the channel's own past)
//   for each output sample at bar phase phi:  step = floor (phi * S), frac = phi * S - step
//       read = blockStart + (map[step] + frac) / S * W          W = LENGTH bars of material, so the speed (and the pitch) is LENGTH x;
//                                                               blockStart = the start of the last complete LENGTH-bar block on the grid
//   wet = read (hard cuts at the bar line and at every displaced step, optionally rounded by EDGE)
//   out = dry (1 - mix) + wet mix -> output gain
//
// The material is always the LENGTH bars that ended at the current bar line, so everything read has already arrived: no lookahead and no
// latency. Until that much of the channel has been heard the wet path passes the dry signal through. There is no randomness in the audio
// path: the map is a pure function of (density, reach, pattern, steps) and the position is a pure function of the transport.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace kiyma
{
constexpr double kPi = 3.14159265358979323846;
constexpr int kLatency = 0;                  // reads only the past: nothing to report
constexpr int kMaxSteps = 32;
constexpr double kRingSeconds = 48.0;        // 4 bars at 20 BPM in 4/4; longer windows are clamped to the ring

struct Params
{
    float glitch = 0.40f;      // DENSITY 0..1 = 0..100 % of the steps displaced
    float brutal = 1.0f / 3.0f;// REACH 0..1 -> 1..16 steps (round (1 + 15 b)); 1/3 = 6
    float offset = 0.0f;       // PATTERN stepped 0..1 -> seed 1..16
    float envelope = 2.0f / 3.0f; // LENGTH stepped 0..1 -> 1/2, 1, 2, 4 bars of material per bar
    float attackMs = 0.0f;     // EDGE 0..10 ms: rounds the cuts; 0 = hard, as the Max device
    float releaseMs = 0.5f;    // STEPS stepped 0..1 -> 8, 16, 32
    float mix = 1.0f;          // 0..1 linear dry / wet
    float crank = 0.0f;        // SHUFFLE 0..1 -> +0..15 on the pattern number: turning the crank re-rolls
    float outputDb = 0.0f;     // -24..+6
};

struct Transport
{
    double bpm = 120.0;
    double ppq = -1.0;           // quarter-note position at the start of the block; < 0 when unknown
    double beatsPerBar = 4.0;
    bool playing = true;
};

// ---- the selectors ---------------------------------------------------------------------------------------------------------------------
inline int lengthIndex (float v) noexcept { return std::clamp ((int) std::lround (v * 3.0f), 0, 3); }
inline const char* lengthName (int index) noexcept
{
    static const char* const names[4] { "1/2 BAR", "1 BAR", "2 BARS", "4 BARS" };
    return names[std::clamp (index, 0, 3)];
}
inline double lengthBars (int index) noexcept
{
    static const double bars[4] { 0.5, 1.0, 2.0, 4.0 };
    return bars[std::clamp (index, 0, 3)];
}
inline int patternIndex (float v) noexcept { return std::clamp ((int) std::lround (v * 15.0f), 0, 15); }   // seed = index + 1
inline int shuffleOf (float crank) noexcept { return std::clamp ((int) std::floor (crank * 15.999f), 0, 15); }
inline int seedOf (float pattern, float crank) noexcept { return (patternIndex (pattern) + shuffleOf (crank)) % 16 + 1; }
inline int stepsIndex (float v) noexcept { return std::clamp ((int) std::lround (v * 2.0f), 0, 2); }
inline int stepsOf (int index) noexcept { return 8 << std::clamp (index, 0, 2); }
inline int reachOf (float b) noexcept { return std::clamp ((int) std::lround (1.0f + 15.0f * b), 1, 16); }
inline int densityOf (float g) noexcept { return std::clamp ((int) std::lround (g * 100.0f), 0, 100); }

// ---- the map: chopper.js generate(), line for line -------------------------------------------------------------------------------------
struct Mulberry32
{
    uint32_t state;
    explicit Mulberry32 (int32_t seed) : state ((uint32_t) seed) {}
    double next() noexcept
    {
        state += 0x6D2B79F5u;
        uint32_t t = state;
        t = (t ^ (t >> 15)) * (1u | t);
        t = (t + ((t ^ (t >> 7)) * (61u | t))) ^ t;
        return (double) (t ^ (t >> 14)) / 4294967296.0;
    }
};

/** For each step, the step to read instead. density 0..100, reach >= 1, any seed; steps <= kMaxSteps. */
inline void buildMap (int* map, int steps, int density, int reach, int seed) noexcept
{
    Mulberry32 random (seed);
    for (int step = 0; step < steps; ++step)
    {
        int target = step;
        if (random.next() * 100.0 < (double) density)
        {
            const int jump = (int) std::floor ((random.next() * 2.0 - 1.0) * (double) reach + 0.5);   // JS Math.round
            target = ((step + jump) % steps + steps) % steps;
        }
        map[step] = target;
    }
}

class Engine
{
public:
    static constexpr int kMaxChannels = 2;

    struct Probe
    {
        std::vector<double> read;     // the read position (absolute input sample) of each output sample; < 0 = dry passthrough
        std::vector<int> step;        // the step each output sample is in
    };

    void prepare (double sampleRate, int numChannels)
    {
        sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        channels = std::clamp (numChannels, 1, kMaxChannels);
        size = (int64_t) std::ceil (sr * kRingSeconds) + 8;
        for (auto& r : ring)
            r.assign ((size_t) size, 0.0f);
        reset();
    }

    void reset() noexcept
    {
        for (auto& r : ring)
            std::fill (r.begin(), r.end(), 0.0f);
        written = 0;
        kPos = 0;
        mapKey = -1;
        cutLevel = 0.0f;
        needSnap = true;
    }

    void setProbe (Probe* p) noexcept { probe = p; }
    float envelopeLevel() const noexcept { return cutLevel; }   // 1 at every cut, decaying: the editor's pulse
    const int* currentMap() const noexcept { return map.data(); }

    void process (float* const* io, int numChannels, int numSamples, const Params& p, const Transport& t)
    {
        const int nch = std::min (std::min (numChannels, channels), kMaxChannels);
        if (numSamples <= 0 || nch <= 0)
            return;

        // The map, rebuilt only when one of its four inputs changes.
        const int nSteps = stepsOf (stepsIndex (clamp01 (p.releaseMs)));
        const int density = densityOf (clamp01 (p.glitch));
        const int reach = std::min (reachOf (clamp01 (p.brutal)), nSteps);
        const int seed = seedOf (clamp01 (p.offset), clamp01 (p.crank));
        const int64_t key = (((int64_t) nSteps * 101 + density) * 17 + reach) * 17 + seed;
        if (key != mapKey)
        {
            buildMap (map.data(), nSteps, density, reach, seed);
            steps = nSteps;
            mapKey = key;
        }

        const double lenBars = lengthBars (lengthIndex (clamp01 (p.envelope)));
        const double edgeMs = std::clamp (finite (p.attackMs), 0.0f, 10.0f);
        mixS.tgt = clamp01 (p.mix);
        gainS.tgt = std::pow (10.0, (double) std::clamp (finite (p.outputDb), -24.0f, 6.0f) / 20.0);
        if (needSnap)
        {
            mixS.v = mixS.tgt;
            gainS.v = gainS.tgt;
            needSnap = false;
        }
        const double smoothK = 1.0 - std::exp (-1.0 / (0.02 * sr));

        // The position: sample k of the transport is k / spb beats, k an integer, so the grid is the same at any block size. A stopped
        // transport (or none) free-runs from the last position.
        const double bpm = std::clamp (t.bpm > 0.0 && std::isfinite (t.bpm) ? t.bpm : 120.0, 20.0, 999.0);
        const double spb = sr * 60.0 / bpm;
        const double beatsPerBar = t.beatsPerBar > 0.0 && std::isfinite (t.beatsPerBar) ? t.beatsPerBar : 4.0;
        const double barLen = beatsPerBar * spb;   // samples
        if (t.playing && t.ppq >= 0.0 && std::isfinite (t.ppq))
            kPos = (int64_t) std::llround (t.ppq * spb);
        const double window = std::min (lenBars * barLen, (double) (size - 8));
        const double stepLen = barLen / (double) steps;
        const double edge = edgeMs * 0.001 * sr;
        const float cutDecay = (float) std::exp (-1.0 / (0.08 * sr));

        for (int i = 0; i < numSamples; ++i)
        {
            const int64_t n = written;
            for (int c = 0; c < nch; ++c)
                ring[(size_t) c][(size_t) (n % size)] = finite (io[c][i]);
            ++written;

            const int64_t k = kPos + i;
            const double barIdx = std::floor ((double) k / barLen);
            const double inBar = (double) k - barIdx * barLen;           // samples since the bar line
            const double barStart = (double) n - inBar;                  // the bar line, in input samples
            // The material is the last complete LENGTH-bar block on the bar grid (blocks start at bar 0, LENGTH bars apart), so like the
            // dropped file it always starts at the top of a phrase: with 2 BARS, bars 2j and 2j+1 both play bars 2j-2 .. 2j.
            const double endBar = lenBars >= 1.0 ? std::floor (barIdx / lenBars) * lenBars : barIdx;
            const double windowStart = barStart - (barIdx - endBar) * barLen - window;
            const int step = std::min ((int) (inBar / stepLen), steps - 1);
            const double frac = (inBar - step * stepLen) / stepLen;
            const double read = windowStart + ((double) map[(size_t) step] + frac) / (double) steps * window;

            // EDGE: a raised-cosine dip either side of a cut (the bar line, or a step boundary where the read position jumps).
            float g = 1.0f;
            if (edge > 0.5)
            {
                const double toStart = inBar - step * stepLen, toEnd = (step + 1) * stepLen - inBar;
                const bool cutIn = step == 0 || map[(size_t) step] != map[(size_t) step - 1] + 1;
                const bool cutOut = step == steps - 1 || map[(size_t) step + 1] != map[(size_t) step] + 1;
                double d = 1e9;
                if (cutIn) d = std::min (d, toStart);
                if (cutOut) d = std::min (d, toEnd);
                if (d < edge)
                    g = (float) (0.5 - 0.5 * std::cos (kPi * d / edge));
            }
            if (inBar - step * stepLen < 1.0 && (step == 0 || map[(size_t) step] != map[(size_t) step - 1] + 1))
                cutLevel = 1.0f;
            else
                cutLevel *= cutDecay;

            const bool valid = read >= (double) firstValid() && read <= (double) n;
            mixS.v += (mixS.tgt - mixS.v) * smoothK;
            gainS.v += (gainS.tgt - gainS.v) * smoothK;
            const float m = (float) mixS.v, gn = (float) gainS.v;
            for (int c = 0; c < nch; ++c)
            {
                const float dry = io[c][i];
                float wet = dry;
                if (valid)
                {
                    const int64_t r0 = (int64_t) std::floor (read);
                    const float a = (float) (read - (double) r0);
                    const float s0 = ring[(size_t) c][(size_t) (r0 % size)];
                    const float s1 = r0 + 1 <= n ? ring[(size_t) c][(size_t) ((r0 + 1) % size)] : s0;
                    wet = (s0 + (s1 - s0) * a) * g;
                }
                io[c][i] = (dry * (1.0f - m) + wet * m) * gn;
            }
            if (probe != nullptr)
            {
                probe->read.push_back (valid ? read : -1.0);
                probe->step.push_back (step);
            }
        }
        kPos += numSamples;
    }

private:
    struct Smooth { double v = 0.0, tgt = 0.0; };
    static float finite (float v) noexcept { return std::isfinite (v) ? v : 0.0f; }
    static float clamp01 (float v) noexcept { return std::clamp (finite (v), 0.0f, 1.0f); }
    int64_t firstValid() const noexcept { return std::max<int64_t> (0, written - size + 8); }

    double sr = 48000.0;
    int channels = 2;
    int64_t size = 0, written = 0, kPos = 0, mapKey = -1;
    std::array<std::vector<float>, kMaxChannels> ring;
    std::array<int, kMaxSteps> map {};
    int steps = 16;
    Smooth mixS, gainS;
    float cutLevel = 0.0f;
    bool needSnap = true;
    Probe* probe = nullptr;
};
} // namespace kiyma
