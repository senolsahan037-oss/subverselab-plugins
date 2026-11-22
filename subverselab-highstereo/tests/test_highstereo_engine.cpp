// HIGH STEREO engine self-test. Plain C++17. Prints every number it checks.
#include "../Source/HighStereoEngine.h"
#include <cstdio>
#include <random>
#include <string>

using svl::HighStereoEngine;
using P = HighStereoEngine::Parameters;
// The peak guard is checked in its own block (8b); every other check measures the exact math with it off.
static bool gGuard = false;
static P withGuard (P p) { p.peakGuard = gGuard; return p; }

static int failures = 0, checks = 0;
static void check (bool ok, const std::string& what, double value)
{
    ++checks;
    if (! ok) ++failures;
    std::printf ("[%s] %-62s %.4f\n", ok ? "PASS" : "FAIL", what.c_str(), value);
}

struct Buf
{
    std::vector<float> l, r;
    explicit Buf (size_t n = 0) : l (n), r (n) {}
    size_t size() const { return l.size(); }
};

static Buf run (double fs, const Buf& in, const P& p, int block = 512, HighStereoEngine* eng = nullptr)
{
    HighStereoEngine local;
    HighStereoEngine& e = eng ? *eng : local;
    if (! eng) e.prepare (fs, block);
    Buf out (in.size());
    for (size_t i = 0; i < in.size(); i += (size_t) block)
    {
        const int n = (int) std::min ((size_t) block, in.size() - i);
        const float* ip[2] = { in.l.data() + i, in.r.data() + i };
        float* op[2] = { out.l.data() + i, out.r.data() + i };
        e.process (ip, op, 2, n, withGuard (p));
    }
    return out;
}

static Buf noise (size_t n, unsigned seed, double amp = 0.25, bool pink = false)
{
    std::mt19937 rng (seed);
    std::normal_distribution<double> nd (0.0, 1.0);
    Buf b (n);
    double pl[7] {}, pr[7] {};
    for (size_t i = 0; i < n; ++i)
    {
        double a = nd (rng), c = nd (rng);
        if (pink)   // Paul Kellet's economy pink filter
        {
            auto pk = [] (double* s, double w) {
                s[0] = 0.99886 * s[0] + w * 0.0555179; s[1] = 0.99332 * s[1] + w * 0.0750759;
                s[2] = 0.96900 * s[2] + w * 0.1538520; s[3] = 0.86650 * s[3] + w * 0.3104856;
                s[4] = 0.55000 * s[4] + w * 0.5329522; s[5] = -0.7616 * s[5] - w * 0.0168980;
                const double y = s[0] + s[1] + s[2] + s[3] + s[4] + s[5] + s[6] + w * 0.5362; s[6] = w * 0.115926; return y * 0.11; };
            a = pk (pl, a); c = pk (pr, c);
        }
        // partly correlated stereo
        b.l[i] = (float) (amp * (0.7 * a + 0.3 * c));
        b.r[i] = (float) (amp * (0.3 * a + 0.7 * c));
    }
    return b;
}

static double db (double x) { return 20.0 * std::log10 (std::max (1e-30, x)); }

static double rms (const std::vector<float>& v, size_t from, size_t to)
{
    double s = 0; for (size_t i = from; i < to; ++i) s += (double) v[i] * v[i];
    return std::sqrt (s / (double) (to - from));
}

static double corr (const Buf& b, size_t from, size_t to)
{
    double ll = 0, rr = 0, lr = 0;
    for (size_t i = from; i < to; ++i) { ll += (double) b.l[i] * b.l[i]; rr += (double) b.r[i] * b.r[i]; lr += (double) b.l[i] * b.r[i]; }
    return lr / std::sqrt (ll * rr);
}

static double kLoudness (double fs, const Buf& b, size_t from, size_t to)
{
    svl::hs::Biquad f[2][2];
    for (auto& c : f) { c[0].setHighShelf (fs, 1681.974450955533, svl::hs::kButterQ, 3.999843853973347); c[1].setHighPass (fs, 38.13547087602444, 0.5003270373238773); }
    double s = 0;
    for (size_t i = 0; i < to; ++i)
    {
        const double l = f[0][1].process (f[0][0].process (b.l[i]));
        const double r = f[1][1].process (f[1][0].process (b.r[i]));
        if (i >= from) s += l * l + r * r;
    }
    return -0.691 + 10.0 * std::log10 (s / (double) (to - from));
}

static Buf sines (double fs, size_t n, double fM, double aM, double fS, double aS)
{
    Buf b (n);
    for (size_t i = 0; i < n; ++i)
    {
        const double t = (double) i / fs;
        const double m = aM * std::sin (2 * svl::hs::kPi * fM * t), s = aS * std::sin (2 * svl::hs::kPi * fS * t + 0.3);
        b.l[i] = (float) (m + s); b.r[i] = (float) (m - s);
    }
    return b;
}

static void testRate (double fs)
{
    const std::string tag = " @" + std::to_string ((int) fs);
    const size_t n = (size_t) (fs * 2);
    const Buf in = noise (n, 7);

    // 1. Defaults: bit-identical.
    {
        const Buf out = run (fs, in, P {});
        size_t diff = 0; for (size_t i = 0; i < n; ++i) diff += (out.l[i] != in.l[i]) + (out.r[i] != in.r[i]);
        check (diff == 0, "defaults are bit-identical (differing samples)" + tag, (double) diff);
    }
    // 2. IN off with everything turned: bit-identical.
    {
        P p; p.enabled = false; p.width = 2; p.spread = 1; p.monoBassHz = 120; p.fix = true; p.bandWidth.fill (2.0); p.match = true; p.outputDb = 6;
        const Buf out = run (fs, in, p);
        size_t diff = 0; for (size_t i = 0; i < n; ++i) diff += (out.l[i] != in.l[i]) + (out.r[i] != in.r[i]);
        check (diff == 0, "IN off is bit-identical (differing samples)" + tag, (double) diff);
    }
    // 3. SPREAD fills from the edges (owner 2026-11-20). A centre-only source has nothing to fill from: untouched.
    {
        Buf mono = noise (n, 11); mono.r = mono.l;
        P p; p.spread = 1.0;
        const Buf out = run (fs, mono, p);
        double err = 0; for (size_t i = 0; i < n; ++i) err = std::max (err, std::abs ((double) out.l[i] - mono.l[i]) + std::abs ((double) out.r[i] - mono.r[i]));
        check (err < 1e-6, "SPREAD 100% on a centre-only source: max deviation" + tag, err);
    }
    // 3b. An edge source (noise panned to -0.85): SPREAD brings part of it toward the middle (R rises vs L).
    //     Mono level change is reported: SPREAD is the one control that is not mono-exact.
    {
        Buf edge (n);
        { std::mt19937 rng (13); std::normal_distribution<double> nd (0, 1);
          svl::hs::Biquad b1, b2; b1.setHighPass (fs, 1500, svl::hs::kButterQ); b2.setLowPass (fs, 4000, svl::hs::kButterQ);
          const double a = (1.0 - 0.85) * svl::hs::kPi / 4;   // constant-power pan at -0.85
          for (size_t i = 0; i < n; ++i) { const double x = 0.3 * b2.process (b1.process (nd (rng))); edge.l[i] = (float) (std::cos (a) * x); edge.r[i] = (float) (std::sin (a) * x); } }
        auto balance = [&] (const Buf& b) { return db (rms (b.r, n / 2, n) / rms (b.l, n / 2, n)); };
        auto monoLevel = [&] (const Buf& b) { std::vector<float> m (n); for (size_t i = 0; i < n; ++i) m[i] = 0.5f * (b.l[i] + b.r[i]); return rms (m, n / 2, n); };
        P off; off.width = 1.0001;   // processed path, no fill
        const Buf o0 = run (fs, edge, off);
        P p; p.spread = 1.0;
        const Buf o1 = run (fs, edge, p);
        check (balance (o0) < -15, "edge source R/L balance without SPREAD (dB)" + tag, balance (o0));
        check (balance (o1) > balance (o0) + 6, "edge source R/L balance with SPREAD 100% (dB)" + tag, balance (o1));
        // SPREAD adds energy (MATCH is there for a fair A/B). What matters for mono safety is that mono does not
        // jump against stereo: mono change minus stereo change.
        auto stLevel = [&] (const Buf& b) { return std::sqrt (0.5 * (rms (b.l, n / 2, n) * rms (b.l, n / 2, n) + rms (b.r, n / 2, n) * rms (b.r, n / 2, n))); };
        const double dm = db (monoLevel (o1) / monoLevel (o0)), ds = db (stLevel (o1) / stLevel (o0));
        check (true, "SPREAD 100% on the edge source: stereo level change (dB)" + tag, ds);
        check (dm < 3.5, "SPREAD 100% on the edge source: mono level change (dB)" + tag, dm);
        check (std::abs (dm - ds) < 1.5, "SPREAD 100% on the edge source: mono minus stereo change (dB)" + tag, dm - ds);
    }
    // 4. WIDTH 0: L == R exactly after settling.
    {
        P p; p.width = 0;
        const Buf out = run (fs, in, p);
        size_t diff = 0; for (size_t i = (size_t) (fs * 0.5); i < n; ++i) diff += out.l[i] != out.r[i];
        check (diff == 0, "WIDTH 0 gives L == R (differing samples)" + tag, (double) diff);
    }
    // 5. MONO BASS 120 Hz: side at 60 Hz and 30 Hz removed, side at 1 kHz kept, mid at 60 Hz kept.
    {
        P p; p.monoBassHz = 120;
        for (double f : { 60.0, 30.0 })
        {
            const Buf in2 = sines (fs, n, 0, 0, f, 0.5);
            const Buf out = run (fs, in2, p);
            Buf side (n); for (size_t i = 0; i < n; ++i) side.l[i] = 0.5f * (out.l[i] - out.r[i]);
            const double red = db (rms (side.l, n / 2, n) / (0.5 / std::sqrt (2.0)));
            const double need = f == 60 ? -24.0 : -48.0;
            check (red < need, "MONO BASS 120: side at " + std::to_string ((int) f) + " Hz (dB)" + tag, red);
        }
        const Buf in3 = sines (fs, n, 0, 0, 1000, 0.5);
        const Buf o3 = run (fs, in3, p);
        Buf s3 (n); for (size_t i = 0; i < n; ++i) s3.l[i] = 0.5f * (o3.l[i] - o3.r[i]);
        const double keep = db (rms (s3.l, n / 2, n) / (0.5 / std::sqrt (2.0)));
        check (std::abs (keep) < 0.1, "MONO BASS 120: side at 1 kHz (dB)" + tag, keep);
        const Buf in4 = sines (fs, n, 60, 0.5, 0, 0);
        const Buf o4 = run (fs, in4, p);
        const double mk = db (rms (o4.l, n / 2, n) / (0.5 / std::sqrt (2.0)));
        check (std::abs (mk) < 0.05, "MONO BASS 120: mid at 60 Hz (dB)" + tag, mk);
    }
    // 6. Band tree is flat: FIX on everywhere, mono input, magnitude unchanged.
    {
        P p; p.fix = true;
        double worst = 0;
        for (double f : { 40.0, 100.0, 180.0, 250.0, 700.0, 1000.0, 3000.0, 8000.0, 15000.0 })
        {
            const Buf in2 = sines (fs, n, f, 0.5, 0, 0);
            const Buf out = run (fs, in2, p);
            worst = std::max (worst, std::abs (db (rms (out.l, n / 2, n) / (0.5 / std::sqrt (2.0)))));
        }
        check (worst < 0.05, "band tree magnitude, worst deviation 40 Hz..15 kHz (dB)" + tag, worst);
    }
    // 7. FIX as a user does it: run, read the PHASE MAP, press FIX on every band that reads below 0, run again.
    {
        const Buf in2 = sines (fs, n, 1300, 0.2, 1500, 0.4);
        HighStereoEngine probe; probe.prepare (fs, 512);
        const double before = corr (run (fs, in2, P {}, 512, &probe), n / 2, n);
        P p; p.fix = true; int lit = 0;
        for (int k = 0; k < HighStereoEngine::kBands; ++k)
            if (probe.band (k).overflow) ++lit;
        HighStereoEngine e; e.prepare (fs, 512);
        const Buf out = run (fs, in2, p, 512, &e);
        const double after = corr (out, n / 2, n);
        // Bands more than 30 dB under the loudest band read as "no signal" on the panel.
        double top = -200; for (int k = 0; k < HighStereoEngine::kBands; ++k) top = std::max (top, (double) e.band (k).levelDb);
        double worstBand = 1;
        for (int k = 0; k < HighStereoEngine::kBands; ++k)
            if (e.band (k).levelDb > top - 30) worstBand = std::min (worstBand, (double) e.band (k).correlation);
        check (before < -0.5, "FIX test signal correlation before" + tag, before);
        check (lit >= 1 && probe.band (4).overflow, "overflow flag lights band 5 (1k-2k); bands lit" + tag, lit);
        check (after > -0.05, "one FIX key: output correlation after" + tag, after);
        check (worstBand > -0.05, "one FIX key: worst band correlation (bands within 30 dB)" + tag, worstBand);
    }
    // 7b. Band widths. LR4 bands overlap, so one band at 200 % lifts a tone at its centre (2828 Hz) by less than 6.02 dB;
    //     with its neighbours also at 200 % it is the full 6.02 dB. A tone two bands away (500 Hz) is untouched.
    {
        auto sideGain = [&] (const P& p, double f) {
            const Buf in2 = sines (fs, n, 0, 0, f, 0.2);
            const Buf out = run (fs, in2, p);
            Buf sd (n); for (size_t i = 0; i < n; ++i) sd.l[i] = 0.5f * (out.l[i] - out.r[i]);
            return db (rms (sd.l, n / 2, n) / (0.2 / std::sqrt (2.0)));
        };
        P one; one.bandWidth[5] = 2.0;
        P three; three.bandWidth[4] = three.bandWidth[5] = three.bandWidth[6] = 2.0;
        const double g1 = sideGain (one, 2828.0), g3 = sideGain (three, 2828.0), g0 = sideGain (one, 500.0);
        check (g1 > 3.5 && g1 < 6.1, "band 2k-4k at 200 %: side at 2828 Hz (dB)" + tag, g1);
        check (std::abs (g3 - 6.0206) < 0.1, "bands 1k-8k at 200 %: side at 2828 Hz (dB)" + tag, g3);
        check (std::abs (g0) < 0.05, "band 2k-4k at 200 %: side at 500 Hz (dB)" + tag, g0);
    }
    // 7c. Band widths never change the mono sum: random band widths vs widths at 100 % (both on the filtered path).
    {
        P a; std::mt19937 rng (21); std::uniform_real_distribution<double> u (0.0, 2.0);
        for (auto& b : a.bandWidth) b = u (rng);
        P b; b.fix = true;   // filtered path, band widths at 100 %; FIX touches only the side
        const Buf oa = run (fs, in, a), ob = run (fs, in, b);
        double e = 0, s2 = 0;
        for (size_t i = 0; i < n; ++i)
        {
            const double ma = 0.5 * ((double) oa.l[i] + oa.r[i]), mb = 0.5 * ((double) ob.l[i] + ob.r[i]);
            e += (ma - mb) * (ma - mb); s2 += mb * mb;
        }
        const double nullDb = 10 * std::log10 (std::max (1e-30, e) / s2);
        check (nullDb < -120, "random band widths: mono sum null (dB)" + tag, nullDb);
    }
    // 7d. Over-widened band: lights without FIX; the one FIX key holds it at correlation >= 0.
    {
        const Buf in2 = sines (fs, n, 3000, 0.2, 3300, 0.15);   // side 2.5 dB under mid
        P p; p.bandWidth[5] = 2.0;                               // pushed 3.5 dB over mid
        HighStereoEngine e1; e1.prepare (fs, 512);
        const double c1 = corr (run (fs, in2, p, 512, &e1), n / 2, n);
        p.fix = true;
        HighStereoEngine e2; e2.prepare (fs, 512);
        const double c2 = corr (run (fs, in2, p, 512, &e2), n / 2, n);
        check (e1.band (5).overflow && c1 < -0.1, "over-widened band lights; correlation without FIX" + tag, c1);
        check (c2 > -0.05, "over-widened band with FIX: correlation" + tag, c2);
    }
    // 7e. MIX: dry is phase-matched, so 50 % never combs. Side-only tone exactly at a crossover (1 kHz) and between
    //     (1414 Hz), filtered path on (a band at 100.01 %): side level at MIX 50 % within 0.05 dB of the input.
    //     MIX 0 % with the plain path: bit-identical.
    {
        // WIDTH 150 % = the last width before BEHIND starts (20 Nov), so the side is a plain 1.5x here.
        P p; p.bandWidth[0] = 1.0001; p.width = 1.5; p.mix = 0.5;
        P q = p; q.width = 1.0;   // reference: same path, no widening -> side 0 dB
        double worst = 0;
        for (double f : { 1000.0, 1414.0, 250.0, 8000.0 })
        {
            const Buf in2 = sines (fs, n, 0, 0, f, 0.2);
            const Buf out = run (fs, in2, p);
            Buf sd (n); for (size_t i = 0; i < n; ++i) sd.l[i] = 0.5f * (out.l[i] - out.r[i]);
            const double g = db (rms (sd.l, n / 2, n) / (0.2 / std::sqrt (2.0)));
            worst = std::max (worst, std::abs (g - db (1.25)));   // 50 % of 1.0 and 1.5 -> 1.25 in phase
        }
        check (worst < 0.05, "MIX 50% WIDTH 150%: side = 1.25x at 250/1k/1414/8k, worst error (dB)" + tag, worst);
        // BEHIND on (WIDTH 200 %): MIX 50 % side = in-phase average of dry (1x) and the wet side measured at MIX 100 %.
        double worstB = 0;
        for (double f : { 1000.0, 1414.0, 250.0, 5657.0, 11000.0 })
        {
            const Buf in2 = sines (fs, n, 0, 0, f, 0.2);
            P w100; w100.width = 2.0;  P w50 = w100; w50.mix = 0.5;
            const Buf o1 = run (fs, in2, w100), o5 = run (fs, in2, w50);
            Buf s1 (n), s5 (n);
            for (size_t i = 0; i < n; ++i) { s1.l[i] = 0.5f * (o1.l[i] - o1.r[i]); s5.l[i] = 0.5f * (o5.l[i] - o5.r[i]); }
            const double ref = 0.2 / std::sqrt (2.0);
            const double gw = rms (s1.l, n / 2, n) / ref, g5 = rms (s5.l, n / 2, n) / ref;
            worstB = std::max (worstB, std::abs (db (g5) - db (0.5 + 0.5 * gw)));
        }
        check (worstB < 0.05, "MIX 50% WIDTH 200% (BEHIND): side = in-phase dry/wet average, worst error (dB)" + tag, worstB);
        P z; z.width = 1.4; z.spread = 0.6; z.mix = 0.0;
        const Buf o = run (fs, in, z);
        size_t diff = 0; for (size_t i = 0; i < n; ++i) diff += (o.l[i] != in.l[i]) + (o.r[i] != in.r[i]);
        check (diff == 0, "MIX 0% on the plain path is bit-identical (differing samples)" + tag, (double) diff);
    }
    // 7f. BEHIND (20 Nov): above WIDTH 150 % the side takes the rear bands; mono never changes.
    {
        // mono at WIDTH 200 % is sample-identical to mono through the same band tree at WIDTH 100 %
        P a; a.width = 2.0;  P b; b.width = 1.0; b.bandWidth[0] = 1.0001;
        const Buf oa = run (fs, in, a), ob = run (fs, in, b);
        double md = 0;
        for (size_t i = 0; i < n; ++i) md = std::max (md, std::abs (0.5 * ((double) oa.l[i] + oa.r[i]) - 0.5 * ((double) ob.l[i] + ob.r[i])));
        check (md < 1e-6, "BEHIND: WIDTH 200% mono vs band-tree mono, max sample difference" + tag, md);
        // side shape at full depth: rear band (1414 Hz) well above the plain 2x, front band (5657 Hz) near it
        auto sideDb = [&] (double f, double width)
        {
            const Buf in2 = sines (fs, n, 0, 0, f, 0.2);
            P q; q.width = width;
            const Buf o = run (fs, in2, q);
            Buf sd (n); for (size_t i = 0; i < n; ++i) sd.l[i] = 0.5f * (o.l[i] - o.r[i]);
            return db (rms (sd.l, n / 2, n) / (0.2 / std::sqrt (2.0)));
        };
        const double rear = sideDb (1414.0, 2.0) - db (2.0), front = sideDb (5657.0, 2.0) - db (2.0);
        check (rear > 6.0, "BEHIND: WIDTH 200% side at 1414 Hz over plain 2x (dB)" + tag, rear);
        check (std::abs (front) < 1.5, "BEHIND: WIDTH 200% side at 5657 Hz vs plain 2x (dB)" + tag, front);
        check (std::abs (sideDb (1414.0, 1.5) - db (1.5)) < 0.01, "BEHIND: off at WIDTH 150% (side at 1414 Hz vs 1.5x, dB)" + tag,
               sideDb (1414.0, 1.5) - db (1.5));
    }
    // 8. MATCH at WIDTH 200 % on pink noise: loudness within 0.5 LU of the input after 5 s.
    {
        const size_t m = (size_t) (fs * 7);
        const Buf pn = noise (m, 5, 0.5, true);
        P p; p.width = 2; p.match = true;
        const Buf out = run (fs, pn, p);
        const double li = kLoudness (fs, pn, (size_t) (fs * 5), m), lo = kLoudness (fs, out, (size_t) (fs * 5), m);
        P q; q.width = 2;
        const double raw = kLoudness (fs, run (fs, pn, q), (size_t) (fs * 5), m) - li;
        check (raw > 1.0, "WIDTH 200% without MATCH is louder (LU)" + tag, raw);
        check (std::abs (lo - li) < 0.5, "MATCH: output minus input loudness (LU)" + tag, lo - li);
        // MATCH includes OUTPUT (owner 21 Nov): a preset's trim does not break the level A/B while MATCH is on ...
        P r = p; r.outputDb = -6.0;
        const double lt = kLoudness (fs, run (fs, pn, r), (size_t) (fs * 5), m);
        check (std::abs (lt - li) < 0.5, "MATCH on + OUTPUT -6 dB: still level with the input (LU)" + tag, lt - li);
        // ... and OUTPUT works while MATCH is off
        P o; o.width = 2; o.outputDb = -6.0;
        const double lo6 = kLoudness (fs, run (fs, pn, o), (size_t) (fs * 5), m) - li;
        check (std::abs (lo6 - (raw - 6.0)) < 0.05, "MATCH off: OUTPUT -6 dB = 6 dB down (LU)" + tag, lo6 - raw);
    }
    // 8b. PEAK GUARD (owner 21 Nov): output peaks never above the input's own peak (floor -1 dBFS, ceiling 0 dBFS).
    {
        gGuard = true;
        auto peakOf = [] (const Buf& b, size_t from) { double pk = 0; for (size_t i = from; i < b.size(); ++i) pk = std::max ({ pk, (double) std::abs (b.l[i]), (double) std::abs (b.r[i]) }); return pk; };
        const size_t m = (size_t) (fs * 4);
        // a stereo mix with transients (pink noise + clicks), normalised to a -0.3 dBFS peak
        Buf x = noise (m, 11, 0.2, true);
        // side-heavy transients (a wide snare): WIDTH 160 % makes their peaks grow
        for (size_t i = (size_t) (fs * 0.1); i + 200 < m; i += (size_t) (fs * 0.25))
            for (int k = 0; k < 200; ++k) { const float v = 0.6f * std::exp (-k / 40.0f) * (k % 2 ? 1.0f : -1.0f); x.l[i + k] += v; x.r[i + k] -= 0.8f * v; }
        const double pin = peakOf (x, 0), norm = svl::hs::dbToGain (-0.3) / pin;
        for (size_t i = 0; i < m; ++i) { x.l[i] *= (float) norm; x.r[i] *= (float) norm; }
        P p; p.monoBassHz = 150; p.bandWidth[0] = 0; p.bandWidth[1] = 0.6; p.width = 1.6;
        const Buf y = run (fs, x, p);
        const double pout = 20 * std::log10 (peakOf (y, 0)), pinDb = 20 * std::log10 (peakOf (x, 0));
        check (pout <= pinDb + 0.001, "GUARD: hot input (-0.3 dBFS), output peak not above it (dB over)" + tag, pout - pinDb);
        // the same without the guard would be over: measure what the processing adds (quiet copy, guard idle)
        Buf q = x; for (size_t i = 0; i < m; ++i) { q.l[i] *= 0.1f; q.r[i] *= 0.1f; }
        const double rise = 20 * std::log10 (peakOf (run (fs, q, p), 0) / peakOf (q, 0));
        check (rise > 0.5, "GUARD: the processing alone raises the peak (dB, so the guard had work)" + tag, rise);
        // quiet material: the guard never touches it (output of the quiet copy = 1/10 of ... not comparable; check the readout)
        HighStereoEngine e; e.prepare (fs, 512);
        double deepest = 0;
        for (size_t i = 0; i < m; i += 512)
        {
            const int n = (int) std::min ((size_t) 512, m - i); Buf o (512);
            const float* ip[2] = { q.l.data() + i, q.r.data() + i }; float* op[2] = { o.l.data(), o.r.data() };
            e.process (ip, op, 2, n, withGuard (p)); deepest = std::min (deepest, (double) e.guardDb());
        }
        check (deepest == 0.0, "GUARD: material peaking at -20 dBFS is never touched (deepest dB)" + tag, deepest);
        // the readout shows nothing while the output is the untouched input (Default / IN off), even on a hot input
        {
            HighStereoEngine z; z.prepare (fs, 512); double dz = 0;
            P off = p; off.enabled = false; P def;
            for (const P* pp : { &off, &def })
                for (size_t i = 0; i < m; i += 512)
                {
                    const int n = (int) std::min ((size_t) 512, m - i); Buf o (512);
                    const float* ip[2] = { x.l.data() + i, x.r.data() + i }; float* op[2] = { o.l.data(), o.r.data() };
                    z.process (ip, op, 2, n, withGuard (*pp)); if (i > (size_t) (fs * 0.05)) dz = std::min (dz, (double) z.guardDb());
                }
            check (dz == 0.0, "GUARD: readout 0 while bypassed / Default on a hot input (deepest dB)" + tag, dz);
        }
        // floor: input at -6 dBFS may rise up to -1 dBFS untouched, never above
        Buf h = x; for (size_t i = 0; i < m; ++i) { h.l[i] *= 0.5f; h.r[i] *= 0.5f; }
        P big = p; big.width = 2.0; big.outputDb = 6.0;
        const double ph = 20 * std::log10 (peakOf (run (fs, h, big), 0));
        check (ph <= -1.0 + 0.001 && ph > -1.5, "GUARD: -6 dBFS input pushed +6 dB stops at the -1 dBFS floor (dBFS)" + tag, ph);
        // after one over the guard lets go: back within 0.05 dB 300 ms later (its loudness cost is measured on the packs)
        {
            Buf one = noise (m, 12, 0.05, true);
            const size_t at = (size_t) (fs * 1.0);
            for (int k = 0; k < 200; ++k) { const float v = 0.9f * std::exp (-k / 40.0f) * (k % 2 ? 1.0f : -1.0f); one.l[at + k] += v; one.r[at + k] -= 0.8f * v; }
            HighStereoEngine g; g.prepare (fs, 256);
            double during = 0, after = -99;
            for (size_t i = 0; i < m; i += 256)
            {
                const int n = (int) std::min ((size_t) 256, m - i); Buf o (256);
                const float* ip[2] = { one.l.data() + i, one.r.data() + i }; float* op[2] = { o.l.data(), o.r.data() };
                g.process (ip, op, 2, n, withGuard (p));
                if (i >= at && i < at + 512) during = std::min (during, (double) g.guardDb());
                if (i >= at + (size_t) (fs * 0.3) && i < at + (size_t) (fs * 0.3) + 256) after = g.guardDb();
            }
            check (during < -1.0 && after > -0.05, "GUARD: one over pulls down, 300 ms later back (dB after)" + tag, after);
        }
        gGuard = false;
    }
    // 9. Block sizes give the same output.
    {
        P p; p.width = 1.6; p.spread = 0.5; p.monoBassHz = 90; p.fix = true; p.bandWidth[2] = 0.4; p.bandWidth[6] = 1.8; p.match = true; p.outputDb = -2;
        const Buf ref = run (fs, in, p, 512);
        size_t worst = 0;
        for (int bs : { 1, 7, 64, 333, 4096 })
        {
            const Buf o = run (fs, in, p, bs);
            size_t d = 0; for (size_t i = 0; i < n; ++i) d += (o.l[i] != ref.l[i]) + (o.r[i] != ref.r[i]);
            worst = std::max (worst, d);
        }
        check (worst == 0, "block sizes 1..4096 identical (worst differing samples)" + tag, (double) worst);
    }
    // 10. No click on a WIDTH jump 0 -> 2 and an IN toggle.
    {
        const Buf in2 = sines (fs, n, 200, 0.3, 230, 0.3);
        HighStereoEngine e; e.prepare (fs, 256);
        Buf out (n);
        double steady = 0, worst = 0;
        for (size_t i = 0; i < n; i += 256)
        {
            P p; p.width = i < n / 2 ? 0.0 : 2.0; p.enabled = ! (i > n * 3 / 4 && i < n * 7 / 8);
            const float* ip[2] = { in2.l.data() + i, in2.r.data() + i };
            float* op[2] = { out.l.data() + i, out.r.data() + i };
            e.process (ip, op, 2, 256, withGuard (p));
        }
        // A click is broadband: high-pass the output at 5 kHz (LR4); a 200-230 Hz signal has nothing there.
        svl::hs::Biquad h1, h2; h1.setHighPass (fs, 5000, svl::hs::kButterQ); h2 = h1;
        for (size_t i = 0; i < n; ++i)
        {
            const double y = h2.process (h1.process (out.l[i]));
            if (i > (size_t) (fs * 0.1)) worst = std::max (worst, std::abs (y));
        }
        (void) steady;
        check (db (worst / 0.6) < -60, "no click on WIDTH jump / IN toggle (energy above 5 kHz, dB)" + tag, db (worst / 0.6));
        // Sanity: the same measure on a hard switch must see it.
        Buf hard (n); for (size_t i = 0; i < n; ++i) hard.l[i] = i < n / 2 ? in2.l[i] : 2.0f * in2.l[i];
        svl::hs::Biquad g1, g2; g1.setHighPass (fs, 5000, svl::hs::kButterQ); g2 = g1;
        double hw = 0; for (size_t i = 0; i < n; ++i) { const double y = g2.process (g1.process (hard.l[i])); if (i > (size_t) (fs * 0.1)) hw = std::max (hw, std::abs (y)); }
        check (db (hw / 0.6) > -40, "click measure sees a hard switch (dB)" + tag, db (hw / 0.6));
    }
    // 11. NaN/Inf input does not poison the state.
    {
        Buf bad = noise (n, 3);
        bad.l[1000] = NAN; bad.r[1001] = INFINITY;
        P p; p.width = 1.5; p.spread = 0.5; p.fix = true; p.bandWidth[5] = 2.0; p.match = true;
        const Buf out = run (fs, bad, p);
        size_t nonFinite = 0; for (size_t i = 0; i < n; ++i) nonFinite += ! std::isfinite (out.l[i]) || ! std::isfinite (out.r[i]);
        check (nonFinite == 0, "NaN/Inf input: non-finite output samples" + tag, (double) nonFinite);
    }
    // 12. LISTEN SPEAKERS keeps a mono signal's level (100 Hz and 8 kHz).
    {
        P p; p.listen = HighStereoEngine::Listen::Speakers;
        double worst = 0;
        for (double f : { 100.0, 8000.0 })
        {
            const Buf in2 = sines (fs, n, f, 0.5, 0, 0);
            worst = std::max (worst, std::abs (db (rms (run (fs, in2, p).l, n / 2, n) / (0.5 / std::sqrt (2.0)))));
        }
        check (worst < 0.5, "SPEAKERS: mono level change at 100 Hz / 8 kHz (dB)" + tag, worst);
    }
    // 13. LISTEN PHONE: bass 100 Hz at least 20 dB down vs 2 kHz, output mono.
    {
        P p; p.listen = HighStereoEngine::Listen::Phone;
        const double lo = rms (run (fs, sines (fs, n, 100, 0.1, 0, 0), p).l, n / 2, n);
        const Buf hiOut = run (fs, sines (fs, n, 2000, 0.1, 300, 0.1), p);
        const double hi = rms (hiOut.l, n / 2, n);
        size_t diff = 0; for (size_t i = 0; i < n; ++i) diff += hiOut.l[i] != hiOut.r[i];
        check (db (lo / hi) < -20, "PHONE: 100 Hz vs 2 kHz (dB)" + tag, db (lo / hi));
        check (diff == 0, "PHONE: output is mono (differing samples)" + tag, (double) diff);
    }
    // 14. LISTEN HEADPHONES (earbud model): stereo kept (no crossfeed), bass rolled off, presence up.
    {
        P p; p.listen = HighStereoEngine::Listen::Headphones;
        const Buf sOut = run (fs, sines (fs, n, 0, 0, 1000, 0.2), p);      // side-only tone
        double leak = 0; for (size_t i = n / 2; i < n; ++i) leak = std::max (leak, std::abs ((double) sOut.l[i] + sOut.r[i]));
        check (leak < 1e-6, "HEADPHONES: side-only tone stays side (max |L+R|)" + tag, leak);
        auto lvl = [&] (double f) { return rms (run (fs, sines (fs, n, f, 0.1, 0, 0), p).l, n / 2, n); };
        const double k1 = lvl (1000);
        check (db (lvl (40) / k1) < -6, "HEADPHONES: 40 Hz vs 1 kHz (dB)" + tag, db (lvl (40) / k1));
        check (db (lvl (2800) / k1) > 2, "HEADPHONES: 2.8 kHz vs 1 kHz (dB)" + tag, db (lvl (2800) / k1));
    }
    // 15. Live loss column follows the LISTEN mode: equal mid and side at 1414 Hz -> MONO -3.0 dB; side-only -> floor;
    //     HEADPHONES reads the earbud curve, so a 60 Hz band shows a loss.
    {
        auto lossAt = [&] (const Buf& b, HighStereoEngine::Listen l, int band)
        {
            HighStereoEngine e; e.prepare (fs, 512);
            P q; q.bandWidth[0] = 1.0001; q.listen = l;
            run (fs, b, q, 512, &e);
            return (double) e.band (band).liveLossDb;
        };
        const double half = lossAt (sines (fs, n, 1414, 0.1, 1414, 0.1), HighStereoEngine::Listen::Mono, 4);
        check (std::abs (half + 3.0103) < 0.3, "loss column MONO: equal mid/side at 1414 Hz (dB)" + tag, half);
        const double gone = lossAt (sines (fs, n, 0, 0, 1414, 0.1), HighStereoEngine::Listen::Mono, 4);
        check (gone < -30, "loss column MONO: side-only band (dB)" + tag, gone);
        const double stereoAsMono = lossAt (sines (fs, n, 1414, 0.1, 1414, 0.1), HighStereoEngine::Listen::Off, 4);
        check (std::abs (stereoAsMono - half) < 1e-6, "loss column STEREO reads as MONO (difference dB)" + tag, stereoAsMono - half);
        const double ear = lossAt (sines (fs, n, 60, 0.1, 0, 0), HighStereoEngine::Listen::Headphones, 0);
        P hp; hp.listen = HighStereoEngine::Listen::Headphones;
        const double heard = db (rms (run (fs, sines (fs, n, 60, 0.1, 0, 0), hp).l, n / 2, n) / (0.1 / std::sqrt (2.0)));
        check (ear < -1 && std::abs (ear - heard) < 0.5, "loss column HEADPHONES 60 Hz minus the level heard (dB)" + tag, ear - heard);
        // balance: a 1414 Hz tone hard left -> +1, centre -> 0, hard right -> -1
        auto balAt = [&] (double gl, double gr)
        {
            Buf b (n); for (size_t i = 0; i < n; ++i) { const double v = 0.2 * std::sin (2 * 3.14159265358979 * 1414 * (double) i / fs); b.l[i] = (float) (gl * v); b.r[i] = (float) (gr * v); }
            HighStereoEngine e; e.prepare (fs, 512); P q; q.bandWidth[0] = 1.0001; run (fs, b, q, 512, &e);
            return (double) e.band (4).liveBalance;
        };
        const double bl = balAt (1, 0), bc = balAt (1, 1), br = balAt (0, 1);
        check (bl > 0.99 && std::abs (bc) < 1e-3 && br < -0.99, "balance readout: hard left / centre / hard right worst error" + tag,
               std::max ({ 1 - bl, std::abs (bc), 1 + br }));
    }
    // 16. STAGE (the puck in the 360 stage): mono tone at +90 -> hard left, -90 -> hard right, 180 -> anti-phase;
    //     power kept on a stereo mix; back at 0 the output is the input again; a jump 0 -> 180 does not click.
    {
        auto stageRun = [&] (const Buf& b, double deg) { P q; q.stageDeg = deg; return run (fs, b, q); };
        const Buf mono = sines (fs, n, 1000, 0.2, 0, 0);
        const Buf l90 = stageRun (mono, 90), r90 = stageRun (mono, -90), b180 = stageRun (mono, 180);
        const double ref = rms (mono.l, n / 2, n);
        const double leftR = db (rms (l90.r, n / 2, n) / ref + 1e-12), rightL = db (rms (r90.l, n / 2, n) / ref + 1e-12);
        // 1 kHz is above the bass split (120 Hz): only the split's leftover low part (LR4, ~ -73 dB at 1 kHz) stays unrotated
        Buf sum180 (n); for (size_t i = 0; i < n; ++i) sum180.l[i] = 0.5f * (b180.l[i] + b180.r[i]);
        const double anti = db (rms (sum180.l, n / 2, n) / ref + 1e-12);
        check (leftR < -60 && rightL < -60, "STAGE +90/-90: 1 kHz mono tone, the far channel (dB, worst)" + tag, std::max (leftR, rightL));
        check (anti < -60, "STAGE 180: 1 kHz mono tone anti-phase, what is left in mono (dB)" + tag, anti);
        // the bass stays out: a 50 Hz mono tone at STAGE 180 keeps its mono (side under mid by >= 20 dB)
        {
            const Buf bass = sines (fs, n, 50, 0.2, 0, 0); const Buf ob = stageRun (bass, 180);
            Buf md (n), sd (n); for (size_t i = 0; i < n; ++i) { md.l[i] = 0.5f * (ob.l[i] + ob.r[i]); sd.l[i] = 0.5f * (ob.l[i] - ob.r[i]); }
            const double sm = db (rms (sd.l, n / 2, n) / rms (md.l, n / 2, n));
            check (sm < -20, "STAGE 180: 50 Hz bass stays centred (side vs mid, dB)" + tag, sm);
        }
        const Buf mix = noise (n, 21);
        const Buf o = stageRun (mix, 63);
        double pi = 0, po = 0; for (size_t i = n / 2; i < n; ++i) { pi += (double) mix.l[i] * mix.l[i] + (double) mix.r[i] * mix.r[i]; po += (double) o.l[i] * o.l[i] + (double) o.r[i] * o.r[i]; }
        check (std::abs (db (std::sqrt (po / pi))) < 0.1, "STAGE 63: stereo power kept (dB)" + tag, db (std::sqrt (po / pi)));
        P z; z.stageDeg = 0;   // default: untouched
        const Buf o0 = run (fs, mix, z);
        size_t diff = 0; for (size_t i = 0; i < n; ++i) diff += (o0.l[i] != mix.l[i]) + (o0.r[i] != mix.r[i]);
        check (diff == 0, "STAGE 0: bit-identical (differing samples)" + tag, (double) diff);
        // jump 0 -> 180 in the middle of a 200 Hz mono tone: energy above 5 kHz stays far below a hard switch
        const Buf lowTone = sines (fs, n, 200, 0.3, 0, 0);
        HighStereoEngine e; e.prepare (fs, 512);
        Buf oj (n);
        for (size_t i = 0; i < n; i += 512)
        {
            const int m = (int) std::min ((size_t) 512, n - i);
            P q; q.stageDeg = i < n / 2 ? 0.0 : 180.0;
            const float* ip[2] = { lowTone.l.data() + i, lowTone.r.data() + i }; float* op[2] = { oj.l.data() + i, oj.r.data() + i };
            e.process (ip, op, 2, m, withGuard (q));
        }
        double hf = 0, tot = 0; { svl::hs::Biquad h1, h2; h1.setHighPass (fs, 5000, 0.707); h2 = h1;
            for (size_t i = n / 4; i < n; ++i) { const double v = h2.process (h1.process (oj.r[i])); hf += v * v; tot += (double) oj.r[i] * oj.r[i]; } }
        check (10 * std::log10 (hf / tot + 1e-30) < -60, "STAGE jump 0 -> 180: energy above 5 kHz vs total (dB)" + tag, 10 * std::log10 (hf / tot + 1e-30));
        // the stage does not move the width panel: band readouts at STAGE 120 equal those at STAGE 0
        auto readouts = [&] (double deg)
        {
            HighStereoEngine e2; e2.prepare (fs, 512); P q; q.width = 1.4; q.stageDeg = deg; run (fs, mix, q, 512, &e2);
            std::array<double, 8> w {}; for (int k = 0; k < 8; ++k) w[(size_t) k] = e2.band (k).liveWidth; return w;
        };
        const auto w0 = readouts (0), w120 = readouts (120);
        double dw = 0; for (int k = 0; k < 8; ++k) dw = std::max (dw, std::abs (w0[(size_t) k] - w120[(size_t) k]));
        check (dw < 1e-9, "STAGE 120 vs 0: width panel readouts, max difference" + tag, dw);
    }
    // 17. LIMIT (SPEC 7): learned per band from the music. Mid 0.2 + side a at 1414 Hz (band 4): r = a^2/0.04, limit = 1/sqrt(r).
    {
        auto lim = [&] (double side, double setting, double width = 1.0)
        {
            HighStereoEngine e; e.prepare (fs, 512); P q; q.bandWidth[4] = setting; q.width = width;
            run (fs, sines (fs, n, 1414, 0.2, 1414, side), q, 512, &e);
            return e.band (4);
        };
        double worst = 0;
        for (double a : { 0.1, 0.2, 0.3 })
        {
            const double expect = std::min (2.0, 0.2 / a);
            worst = std::max (worst, std::abs (lim (a, 1.0).limit - expect) / expect);
        }
        check (worst < 0.03, "LIMIT: side 0.1/0.2/0.3 vs mid 0.2 -> limit 200/100/67 %, worst relative error" + tag, worst);
        // the limit does not depend on where the grip is now, or on WIDTH (it is in band-setting units at that WIDTH)
        const double l1 = lim (0.3, 1.0).limit, l2 = lim (0.3, 0.5).limit, l3 = lim (0.3, 1.0, 1.2).limit;
        check (std::abs (l1 - l2) < 0.01, "LIMIT: same with the grip at 100 % and 50 % (difference)" + tag, l1 - l2);
        check (std::abs (l3 - l1 / 1.2) < 0.01, "LIMIT at WIDTH 120 %: limit scales by 1/1.2 (error)" + tag, l3 - l1 / 1.2);
        // setting the band to its limit puts the peak fill at 100 %
        const double lset = lim (0.3, 1.0).limit;
        const auto atLimit = lim (0.3, lset);
        check (std::abs (atLimit.peakFill - 1.0) < 0.03, "LIMIT: band set to its limit -> peak fill (1 = 100 %)" + tag, atLimit.peakFill);
        // a mid-only band has no side to widen; reset forgets
        const auto mid = lim (0.0, 1.0);
        check (mid.noSide && mid.learned, "LIMIT: mid-only band is flagged 'no side'" + tag, mid.noSide ? 1.0 : 0.0);
        HighStereoEngine e; e.prepare (fs, 512); P q; run (fs, sines (fs, n, 1414, 0.2, 1414, 0.2), q, 512, &e);
        e.resetPeaks();
        check (! e.band (4).learned && e.band (4).limit == 2.0f, "LIMIT: resetPeaks forgets (learned, limit)" + tag, e.band (4).limit);
        // the peak is held: a loud wide burst, then narrow -> limit stays at the burst's value
        {
            Buf b (n);
            for (size_t i = 0; i < n; ++i)
            {
                const double t = 2 * 3.14159265358979 * 1414 * (double) i / fs, sa = i < n / 3 ? 0.3 : 0.05;
                const double m_ = 0.2 * std::sin (t), s_ = sa * std::sin (t + 1.0);
                b.l[i] = (float) (m_ + s_); b.r[i] = (float) (m_ - s_);
            }
            HighStereoEngine e2; e2.prepare (fs, 512); P q2; run (fs, b, q2, 512, &e2);
            check (std::abs (e2.band (4).limit - 0.2 / 0.3) < 0.03, "LIMIT: held at the burst after it ends (limit)" + tag, e2.band (4).limit);
        }
    }
}

int main()
{
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
        testRate (fs);

    // Mono track (1 channel) at defaults: identical; with SPREAD: unchanged (mono sum is the input).
    {
        HighStereoEngine e; e.prepare (48000, 512);
        const Buf in = noise (48000, 9);
        std::vector<float> out (in.size());
        P p; p.spread = 1.0;
        for (size_t i = 0; i < in.size(); i += 512)
        {
            const float* ip[1] = { in.l.data() + i };
            float* op[1] = { out.data() + i };
            e.process (ip, op, 1, 512, withGuard (p));
        }
        double err = 0; for (size_t i = 0; i < in.size(); ++i) err = std::max (err, std::abs ((double) out[i] - in.l[i]));
        check (err < 1e-6, "mono track with SPREAD 100%: max deviation", err);
    }

    std::printf ("\n%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
