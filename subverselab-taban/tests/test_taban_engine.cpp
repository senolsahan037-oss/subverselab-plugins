/*  TABAN engine self-test. Plain C++, no JUCE.

        c++ -std=c++17 -O2 -o build/test_taban_engine tests/test_taban_engine.cpp && ./build/test_taban_engine

    Every line prints the measured number and the limit. Exit code = number of failures.
*/
#include "../Source/TabanEngine.h"
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <vector>

namespace
{
constexpr double kFs = 48000.0;
constexpr double kPi = 3.14159265358979323846;
int failures = 0;

void check (bool ok, const char* name, const char* fmt, double a, double b = 0.0, double c = 0.0)
{
    char buf[512];
    std::snprintf (buf, sizeof buf, fmt, a, b, c);
    std::printf ("%s  %-34s %s\n", ok ? "PASS" : "FAIL", name, buf);
    failures += ok ? 0 : 1;
}

float amp (double db) { return (float) std::pow (10.0, db / 20.0); }
double toDb (double a) { return a > 0.0 ? 20.0 * std::log10 (a) : -999.0; }

struct Stereo { std::vector<float> l, r; };

Stereo sine (double hz, double db, double seconds)
{
    const size_t n = (size_t) (seconds * kFs);
    Stereo s { std::vector<float> (n), std::vector<float> (n) };
    for (size_t i = 0; i < n; ++i)
        s.l[i] = s.r[i] = amp (db) * (float) std::sin (2.0 * kPi * hz * (double) i / kFs);
    return s;
}

/** Runs the engine over a whole signal in blocks; params may change per block via the callback. */
Stereo run (const Stereo& in, svl::TabanEngine::Parameters p, int block = 512,
            std::function<void (size_t, svl::TabanEngine::Parameters&)> automate = {}, svl::TabanEngine* keep = nullptr)
{
    svl::TabanEngine local;
    auto& e = keep != nullptr ? *keep : local;
    e.prepare (kFs, block);
    Stereo out { std::vector<float> (in.l.size()), std::vector<float> (in.r.size()) };
    for (size_t pos = 0; pos < in.l.size(); pos += (size_t) block)
    {
        const int n = (int) std::min ((size_t) block, in.l.size() - pos);
        if (automate) automate (pos, p);
        const float* ins[2] { in.l.data() + pos, in.r.data() + pos };
        float* outs[2] { out.l.data() + pos, out.r.data() + pos };
        e.process (ins, outs, 2, n, p);
    }
    return out;
}

double peakDb (const std::vector<float>& x, size_t from, size_t to)
{
    double m = 0.0;
    for (size_t i = from; i < to && i < x.size(); ++i) m = std::max (m, (double) std::abs (x[i]));
    return toDb (m);
}

/** Distortion: residual after removing the best-fit scaled copy of the input, relative to the output. dB. */
double residualDb (const std::vector<float>& in, const std::vector<float>& out, size_t from, size_t to)
{
    double xy = 0.0, xx = 0.0, yy = 0.0;
    for (size_t i = from; i < to; ++i) { xy += (double) in[i] * out[i]; xx += (double) in[i] * in[i]; }
    const double g = xy / xx;
    double rr = 0.0;
    for (size_t i = from; i < to; ++i) { const double r = out[i] - g * in[i]; rr += r * r; yy += (double) out[i] * out[i]; }
    return 10.0 * std::log10 (rr / yy + 1e-30);
}

bool identical (const std::vector<float>& a, const std::vector<float>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (a[i] != b[i]) return false;
    return true;
}
} // namespace

int main()
{
    using P = svl::TabanEngine::Parameters;
    std::printf ("TABAN engine self-test, %.0f Hz\n", kFs);

    // 1. FLOOR at OFF: bit-identical to the input.
    {
        const auto in = sine (440.0, -30.0, 1.0);
        P p; p.floorDb = -60.0f;
        const auto out = run (in, p);
        check (identical (in.l, out.l) && identical (in.r, out.r), "off: FLOOR -60 passes through", "bit-identical", 0);
    }
    // 2. IN off: bit-identical to the input, whatever FLOOR and OUTPUT say.
    {
        const auto in = sine (440.0, -40.0, 1.0);
        P p; p.floorDb = -10.0f; p.outputDb = 6.0f; p.enabled = false;
        const auto out = run (in, p);
        check (identical (in.l, out.l), "bypass: IN off passes through", "bit-identical", 0);
    }
    // 3. Above the floor: gain exactly 1.
    {
        const auto in = sine (440.0, -6.0, 1.0);
        P p; p.floorDb = -18.0f;
        const auto out = run (in, p);
        // The sine starts at zero: its first samples are quiet and are lifted. From 10 ms on it is above the floor.
        const std::vector<float> a (in.l.begin() + 480, in.l.end()), b (out.l.begin() + 480, out.l.end());
        check (identical (a, b), "above floor: untouched", "-6 dBFS sine, FLOOR -18: bit-identical after the first 10 ms", 0);
    }
    // 4. Under the floor: lifted to the floor (sample peak), at 40 Hz, 100 Hz, 1 kHz, 10 kHz.
    for (double hz : { 40.0, 100.0, 1000.0, 10000.0 })
    {
        const auto in = sine (hz, -36.0, 2.0);
        P p; p.floorDb = -18.0f;
        const auto out = run (in, p);
        const double pk = peakDb (out.l, (size_t) kFs, out.l.size());
        const double thd = residualDb (in.l, out.l, (size_t) kFs, out.l.size());
        char name[64]; std::snprintf (name, sizeof name, "lift %.0f Hz -36 -> FLOOR -18", hz);
        check (std::abs (pk + 18.0) < 0.25 && thd < -80.0, name, "peak %.2f dBFS (want -18 +-0.25), distortion %.1f dB (want < -80)", pk, thd);
    }
    // 5. The cap: -70 dBFS, FLOOR 0 -> lifted by 24 dB only.
    {
        const auto in = sine (1000.0, -70.0, 1.0);
        P p; p.floorDb = 0.0f;
        const auto out = run (in, p);
        const double pk = peakDb (out.l, (size_t) (0.5 * kFs), out.l.size());
        check (std::abs (pk + 46.0) < 0.25, "cap: lift at most 24 dB", "-70 dBFS, FLOOR 0: out %.2f dBFS (want -46 +-0.25)", pk);
    }
    // 6. Near silence: no lift below -96 dBFS; half the cap at -88.
    {
        const auto in = sine (1000.0, -100.0, 1.0);
        P p; p.floorDb = -10.0f;
        const auto out = run (in, p);
        check (identical (in.l, out.l), "gate: -100 dBFS left alone", "bit-identical", 0);
        const auto in2 = sine (1000.0, -88.0, 1.0);
        const auto out2 = run (in2, p);
        const double lift = peakDb (out2.l, (size_t) (0.5 * kFs), out2.l.size()) + 88.0;
        check (std::abs (lift - 12.0) < 0.3, "gate: -88 dBFS gets half the cap", "lift %.2f dB (want 12 +-0.3)", lift);
    }
    // 7. Silence in, silence out.
    {
        Stereo in { std::vector<float> (48000, 0.0f), std::vector<float> (48000, 0.0f) };
        P p; p.floorDb = 0.0f;
        const auto out = run (in, p);
        check (identical (in.l, out.l), "silence stays silence", "FLOOR 0: all zero", 0);
    }
    // 8. Never over the floor: |out| <= max(|in|, floor) on every sample. Quiet -> loud bursts, noise, sweeps.
    {
        std::mt19937 rng (7);
        std::normal_distribution<float> nd (0.0f, 1.0f);
        const size_t n = (size_t) (6 * kFs);
        Stereo in { std::vector<float> (n), std::vector<float> (n) };
        for (size_t i = 0; i < n; ++i)
        {
            const double t = (double) i / kFs;
            const bool loud = std::fmod (t, 0.5) > 0.4;                          // 100 ms bursts every 500 ms
            const double lvl = loud ? -3.0 : -45.0 + 15.0 * std::sin (t * 1.3);
            const double f = 50.0 * std::pow (2.0, std::fmod (t, 3.0) * 3.0);  // 50 Hz .. 400 Hz glides
            in.l[i] = amp (lvl) * (float) (0.7 * std::sin (2.0 * kPi * f * t) + 0.1 * nd (rng));
            in.r[i] = amp (lvl) * (float) (0.7 * std::sin (2.0 * kPi * f * 1.01 * t) + 0.1 * nd (rng));
        }
        double worst = -999.0;
        for (float floorDb : { -40.0f, -24.0f, -12.0f, -6.0f, 0.0f })
        {
            P p; p.floorDb = floorDb;
            const auto out = run (in, p);
            const float fa = amp (floorDb);
            for (size_t i = 0; i < n; ++i)
                for (int c = 0; c < 2; ++c)
                {
                    const float x = c == 0 ? in.l[i] : in.r[i], y = c == 0 ? out.l[i] : out.r[i];
                    worst = std::max (worst, toDb (std::abs (y) / std::max (std::abs (x), fa)));
                }
        }
        check (worst <= 0.001, "never over the floor", "worst |out| / max(|in|, floor) = %+.4f dB (want <= +0.001)", worst);
    }
    // 9. Recovery: -6 dBFS for 0.5 s, then -40 dBFS, FLOOR -20. Time until the output is within 1 dB of the floor.
    {
        Stereo a = sine (200.0, -6.0, 0.5), b = sine (200.0, -40.0, 1.0);
        a.l.insert (a.l.end(), b.l.begin(), b.l.end()); a.r = a.l;
        P p; p.floorDb = -20.0f;
        const auto out = run (a, p);
        const size_t start = (size_t) (0.5 * kFs), win = (size_t) (kFs / 200.0);   // one 200 Hz period
        double ms = -1.0;
        for (size_t i = start; i + win < out.l.size(); i += 24)
            if (std::abs (peakDb (out.l, i, i + win) + 20.0) < 1.0) { ms = (double) (i + win - start) / kFs * 1000.0; break; }
        check (ms > 0.0 && ms < 400.0, "recovery after a loud passage", "within 1 dB of the floor after %.0f ms (want < 400)", ms);
    }
    // 10. Block size does not change the result (control changes on multiples of 4096; 37 with fixed controls).
    {
        auto in = sine (110.0, -30.0, 2.0);
        for (size_t i = 0; i < in.l.size(); ++i) in.l[i] *= (float) (0.2 + std::abs (std::sin (i * 0.0003))), in.r[i] = in.l[i] * 0.5f;
        auto automate = [] (size_t pos, P& p) { p.floorDb = pos < 49152 ? -30.0f : -12.0f; p.outputDb = pos < 65536 ? 0.0f : -3.0f; p.enabled = pos < 81920; };
        P p;
        const auto ref = run (in, p, 512, automate);
        bool same = true;
        for (int bs : { 1, 64, 4096 })
        {
            const auto o = run (in, p, bs, automate);
            same = same && identical (ref.l, o.l) && identical (ref.r, o.r);
        }
        P q; q.floorDb = -20.0f;
        const auto ref37 = run (in, q, 512), o37 = run (in, q, 37);
        same = same && identical (ref37.l, o37.l);
        check (same, "block sizes 1/37/64/512/4096", "bit-identical", 0);
    }
    // 11. No clicks: FLOOR -60 -> -10, OUTPUT 0 -> +12, IN off -> on, in one go, on a -40 dBFS 100 Hz tone.
    {
        const auto in = sine (100.0, -40.0, 2.0);
        P p; p.floorDb = -60.0f; p.enabled = false;
        const auto out = run (in, p, 256, [] (size_t pos, P& q) { if (pos >= 48000) { q.floorDb = -10.0f; q.outputDb = 12.0f; q.enabled = true; } });
        // The largest sample-to-sample step, against the step of a clean tone at the final level (-10 + 12 = +2 dBFS... clipped by nothing).
        double worstStep = 0.0;
        for (size_t i = 1; i < out.l.size(); ++i) worstStep = std::max (worstStep, (double) std::abs (out.l[i] - out.l[i - 1]));
        const double cleanStep = amp (2.0) * 2.0 * std::sin (kPi * 100.0 / kFs);
        check (worstStep <= cleanStep * 1.05, "no click on control jumps", "largest step %.5f, clean tone at the end level %.5f (want <= 1.05x)", worstStep, cleanStep);
    }
    // 12. NaN / Inf in: the state is not poisoned.
    {
        auto in = sine (440.0, -30.0, 1.0);
        in.l[1000] = std::nanf (""); in.r[1001] = INFINITY; in.l[1002] = -INFINITY;
        P p; p.floorDb = -12.0f;
        const auto out = run (in, p);
        bool finiteAfter = true;
        for (size_t i = 2000; i < out.l.size(); ++i) finiteAfter = finiteAfter && std::isfinite (out.l[i]) && std::isfinite (out.r[i]);
        const double pk = peakDb (out.l, 24000, out.l.size());
        check (finiteAfter && std::abs (pk + 12.0) < 0.25, "NaN/Inf in does not poison the state", "finite afterwards, peak %.2f dBFS (want -12)", pk);
    }
    // 13. Mono, and the sample rates.
    for (double fs : { 44100.0, 96000.0, 192000.0 })
    {
        svl::TabanEngine e; e.prepare (fs, 512);
        std::vector<float> x ((size_t) fs), y ((size_t) fs);
        for (size_t i = 0; i < x.size(); ++i) x[i] = amp (-36.0) * (float) std::sin (2.0 * kPi * 60.0 * (double) i / fs);
        P p; p.floorDb = -18.0f;
        for (size_t pos = 0; pos < x.size(); pos += 512)
        {
            const int n = (int) std::min<size_t> (512, x.size() - pos);
            const float* ins[1] { x.data() + pos }; float* outs[1] { y.data() + pos };
            e.process (ins, outs, 1, n, p);
        }
        double m = 0.0; for (size_t i = x.size() / 2; i < x.size(); ++i) m = std::max (m, (double) std::abs (y[i]));
        const double thd = residualDb (x, y, x.size() / 2, x.size());
        char name[64]; std::snprintf (name, sizeof name, "mono 60 Hz at %.1f kHz", fs / 1000.0);
        check (std::abs (toDb (m) + 18.0) < 0.25 && thd < -80.0, name, "peak %.2f dBFS, distortion %.1f dB", toDb (m), thd);
    }

    std::printf (failures == 0 ? "ALL PASS\n" : "FAILURES: %d\n", failures);
    return failures;
}
