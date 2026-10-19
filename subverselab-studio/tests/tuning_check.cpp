// Tuning Lab checks (SPEC.md "Tuning Lab"). No JUCE: c++ -std=c++17 -O2 -I Source/plugin/tuning tests/tuning_check.cpp
#include "TuningLearn.h"
#include "TuningBuiltin.h"
#include <cstdio>
#include <random>

static int failures = 0, checks = 0;
static void check (bool ok, const std::string& what)
{
    ++checks;
    if (! ok) ++failures;
    std::printf ("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
}
static bool near (double a, double b, double tol) { return std::fabs (a - b) <= tol; }
static std::string f2 (double v) { char b[32]; std::snprintf (b, sizeof b, "%.2f", v); return b; }

using namespace jflab;

// harmonic tone (5 partials, 1/k), 10 ms attack / release, slight vibrato-free
static void addTone (std::vector<float>& out, double sr, double start, double dur, double hz, std::mt19937& rng, double vibCents = 0)
{
    std::uniform_real_distribution<double> ph (0, 6.283);
    double phase[5];
    for (double& p : phase) p = ph (rng);
    const int s0 = (int) (start * sr), n = (int) (dur * sr);
    if ((int) out.size() < s0 + n) out.resize ((size_t) (s0 + n), 0.0f);
    for (int i = 0; i < n; ++i)
    {
        const double t = i / sr, env = std::min (1.0, std::min (t / 0.01, (dur - t) / 0.01));
        // vibrato: the phase of a sine whose frequency swings +-vibCents at 5.5 Hz
        const double vib = vibCents == 0 ? 0 : (std::pow (2.0, vibCents / 1200.0) - 1.0) * hz / (6.283185307 * 5.5) * std::sin (6.283185307 * 5.5 * t);
        double v = 0;
        for (int k = 1; k <= 5; ++k) v += std::sin (6.283185307 * (hz * t + vib) * k + phase[k - 1]) / k;
        out[(size_t) (s0 + i)] += (float) (0.25 * env * v);
    }
}

int main()
{
    // ---------------------------------------------------------------- Scala
    {
        const std::string scl = "! test.scl\n!\nPythagorean-ish test\n 5\n!\n 9/8\n 81/64 comment\n 701.955\n 27/16\n 2/1\n";
        Tuning t;
        std::string err;
        check (scala::readScl (scl, t, &err), "scl: parse ratios + cents " + err);
        check (t.size() == 5 && near (t.cents[0], 203.910, 0.001) && near (t.cents[2], 701.955, 1e-9) && near (t.period(), 1200, 1e-9),
               "scl: 9/8 = 203.910 ct, 701.955 kept, period 1200");
        Tuning u;
        check (scala::readScl (scala::writeScl (t), u) && u.size() == t.size() && near (u.cents[1], t.cents[1], 1e-4),
               "scl: write -> read round trip within 0.0001 ct");
        Tuning bad;
        check (! scala::readScl ("x\n 3\n 100.0\n", bad) && bad.size() == 12, "scl: short file rejected, tuning untouched");
        check (! scala::readScl ("x\n 2\n 100.0\n -5/4\n", bad), "scl: negative ratio rejected");
    }
    // ---------------------------------------------------------------- tables / kbm
    {
        Tuning t;   // default 12 equal, middle C at 261.63
        const auto tab = t.table();
        bool ok = true;
        for (int k = 0; k < 128; ++k) ok = ok && near (tab[(size_t) k], k, 1e-4);
        check (ok, "table: 12 equal default = MIDI keys (all 128 within 0.0001 st)");

        Tuning e53;
        e53.cents.clear();
        for (int i = 1; i <= 53; ++i) e53.cents.push_back (1200.0 * i / 53);
        const auto t53 = e53.table();
        check (near (t53[61] - t53[60], 1200.0 / 53 / 100, 1e-5) && near (t53[60 + 53], 72.0, 1e-4), "table: 53 equal, one key = 22.642 ct, 53 keys = an octave");

        const std::string kbm = "! 7 of 12\n12\n0\n127\n60\n69\n440.0\n7\n0\nx\n1\nx\n2\n3\nx\n4\nx\n5\nx\n6\n";
        Tuning maj;
        scala::readScl ("major just\n 7\n 9/8\n 5/4\n 4/3\n 3/2\n 5/3\n 15/8\n 2/1\n", maj);
        check (scala::readKbm (kbm, maj), "kbm: parse a white-key mapping");
        const auto tm = maj.table();
        check (near (tm[69], 69.0, 1e-4), "kbm: reference key 69 = 440 Hz");
        check (near ((tm[64] - tm[60]) * 100, 386.314, 0.01) && near ((tm[67] - tm[60]) * 100, 701.955, 0.01), "kbm: E = 5/4 (386.31 ct), G = 3/2 above C");
        check (near (tm[61], 61, 1e-6), "kbm: unmapped black key stays 12 equal");
        check (near (tm[72] - tm[60], 12.0, 1e-4), "kbm: formal octave 7 degrees = one octave on the keys");
        Tuning back = maj;
        check (scala::readKbm (scala::writeKbm (maj), back) && back.mapping == maj.mapping && back.refKey == 69, "kbm: write -> read round trip");
    }
    // ---------------------------------------------------------------- lock
    {
        Lab lab;
        lab.settings.on = true;
        Lab::setUnlockedForTests (false);
        check (! lab.active(), "lock: Lab switched on but locked -> inactive");
        Lab::setUnlockedForTests (true);
        check (lab.active(), "lock: unlocked + on -> active");
        Lab::setUnlockedForTests (false);
    }
    // ---------------------------------------------------------------- morph + direction
    {
        Lab lab;
        Tuning e24;
        e24.cents.clear();
        for (int i = 1; i <= 24; ++i) e24.cents.push_back (50.0 * i);
        lab.a = Tuning();
        lab.b = e24;
        lab.settings.morph = 0; lab.rebuild();
        check (near (lab.risingTable()[62], 62, 1e-4), "morph 0 = tuning A");
        lab.settings.morph = 1; lab.rebuild();
        check (near (lab.risingTable()[62], 61, 1e-4), "morph 1 = tuning B (key 62 = 2 quarter tones = 61)");
        lab.settings.morph = 0.5f; lab.rebuild();
        check (near (lab.risingTable()[62], 61.5, 1e-4), "morph 0.5 = halfway in cents");

        Tuning d;   // 12 equal, but E falls to 360 ct
        d.fall = d.cents;
        d.fall[3] = 360;
        lab.a = d; lab.b = d; lab.settings.morph = 0; lab.rebuild();
        const float up = lab.noteOn (64, 0.0);                     // first note of a phrase: rising
        lab.noteOff (64);
        const float down = (lab.noteOn (67, 0.2), lab.noteOff (67), lab.noteOn (64, 0.4));
        lab.noteOff (64);
        const float rep = lab.noteOn (64, 0.6);
        lab.noteOff (64);
        const float fresh = lab.noteOn (64, 5.0);
        lab.noteOff (64);
        check (near (up, 64, 1e-4), "direction: phrase start plays the rising value");
        check (near (down, 63.6, 1e-4), "direction: G -> E plays the falling value (360 ct = 63.60)");
        check (near (rep, 63.6, 1e-4), "direction: repeated E keeps falling");
        check (near (fresh, 64, 1e-4), "direction: after 1.5 s a new phrase starts rising");
        lab.settings.directional = false;
        lab.noteOn (67, 6.0); lab.noteOff (67);
        check (near (lab.noteOn (64, 6.1), 64, 1e-4), "direction: switched off -> always the rising value");
        lab.allOff();
    }
    // ---------------------------------------------------------------- ratios + beats
    {
        const Ratio r = nearestRatio (701.955);
        check (r.p == 3 && r.q == 2, "ratio: 701.955 ct -> 3/2");
        const Ratio m3 = nearestRatio (300);
        check (m3.p == 6 && m3.q == 5, "ratio: 300 ct -> 6/5 (" + std::to_string (m3.p) + "/" + std::to_string (m3.q) + ")");
        const Ratio oc = nearestRatio (1190);
        check (oc.p == 2 && oc.q == 1, "ratio: 1190 ct -> 2/1 (" + std::to_string (oc.p) + "/" + std::to_string (oc.q) + ")");
        const Ratio tw = nearestRatio (1902);
        check (tw.p == 3 && tw.q == 1, "ratio: 1902 ct -> 3/1 (" + std::to_string (tw.p) + "/" + std::to_string (tw.q) + ")");
        const Ratio n7 = nearestRatio (968.826);
        check (n7.p == 7 && n7.q == 4, "ratio: 968.83 ct -> 7/4");
        // equal fifth A3-E4: 220 and 329.628 Hz -> 2*329.628 - 3*220 = 0.745 Hz
        const double b = beatRate (220.0, 220.0 * std::pow (2.0, 7.0 / 12.0), { 3, 2, 0 });
        check (near (b, 0.7454, 0.001), "beats: equal fifth on A3 beats " + f2 (b) + " Hz (0.745 by hand)");
        const auto iv = intervals ({ { 57, 57.f }, { 64, 64.f }, { 61, 61.f } });
        check (iv.size() == 3 && iv[0].ratio.p == 5 && iv[0].ratio.q == 4 && near (iv[0].deviation, 13.69, 0.01),
               "intervals: A-C# equal major third = 5/4 + 13.69 ct");
    }
    // ---------------------------------------------------------------- adaptive
    {
        Lab lab;
        lab.a = lab.b = Tuning(); lab.rebuild();
        lab.settings.adaptive = true; lab.settings.homePull = 0;
        lab.noteOn (60, 0);
        const float e = lab.noteOn (64, 0);
        const float g = lab.noteOn (67, 0);
        check (near ((e - 60) * 100, 386.314, 0.01) && near ((g - 60) * 100, 701.955, 0.01), "adaptive: C E G -> 5/4 and 3/2 above C");
        check (near (lab.driftCents(), (386.314 - 400 + 701.955 - 700) / 3, 0.01), "adaptive: drift meter = mean (actual - home) = " + f2 (lab.driftCents()) + " ct");
        lab.allOff();
        lab.settings.adaptiveWindowCents = 10;
        lab.noteOn (60, 1);
        check (near (lab.noteOn (64, 1), 64, 1e-4), "adaptive: 13.7 ct off 5/4 is outside a 10 ct window -> tuned pitch kept");
        lab.allOff();
    }
    // ---------------------------------------------------------------- learning
    {
        const double sr = 48000;
        // a seven-degree line on D3 (146.83 Hz): degree 2 rises at 290 ct, falls at 270 ct; others fixed
        const std::vector<double> rise { 0, 180, 290, 500, 700, 830, 1000 };
        std::vector<double> fallV = rise; fallV[2] = 270;
        const double tonicHz = 146.832;
        for (double vibC : { 0.0, 20.0 })
        {
        std::mt19937 rng (7);
        const std::string tag = vibC > 0 ? " [vibrato +-20 ct]" : "";
        std::vector<float> audio;
        double t = 0.1;
        for (int rep = 0; rep < 4; ++rep)
        {
            for (int k = 0; k <= 7; ++k) { const double c = k == 7 ? 1200 : rise[(size_t) k]; addTone (audio, sr, t, 0.35, tonicHz * std::pow (2.0, c / 1200), rng, vibC); t += 0.4; }
            for (int k = 6; k >= 0; --k) { addTone (audio, sr, t, 0.35, tonicHz * std::pow (2.0, fallV[(size_t) k] / 1200), rng, vibC); t += 0.4; }
            addTone (audio, sr, t, 0.9, tonicHz, rng, vibC);   // the tonic held longest
            t += 2.0;                                     // new phrase
        }
        const auto r = learnTuning (audio.data(), (int) audio.size(), sr);
        check (r.ok, "learn: " + r.message + tag);
        if (r.ok)
        {
            const auto& tu = r.tuning;
            check (near (std::fmod (r.tonicPitch, 12.0), std::fmod (hzToPitch (tonicHz), 12.0), 0.02), "learn: tonic pitch class D found (" + f2 (r.tonicPitch) + ", placed by the C4 octave)" + tag);
            bool deg = tu.size() == 7;
            double worst = 0;
            for (int k = 1; deg && k < 7; ++k) worst = std::max (worst, std::fabs (tu.cents[(size_t) k - 1] - rise[(size_t) k]));
            check (deg && worst <= 2.0, "learn: 6 degrees + octave, worst rising error " + f2 (worst) + " ct (<= 2)" + tag);
            check (tu.directional() && near (tu.fall[1], 270, 2.0), "learn: degree 2 falls at " + (tu.directional() ? f2 (tu.fall[1]) : std::string ("-")) + " ct (270)" + tag);
            bool others = tu.directional();
            for (int k = 1; others && k < 7; ++k) if (k != 2) others = near (tu.fall[(size_t) k - 1], tu.cents[(size_t) k - 1], 2.0);
            check (others, "learn: the other degrees are not directional (within 2 ct)" + tag);
            const auto tab = tu.table();
            check (near (tab[(size_t) tu.middleKey], r.tonicPitch, 1e-4), "learn: the table puts degree 0 on the tonic's own pitch" + tag);
        }
        }
        std::vector<float> noise (48000);
        std::mt19937 g2 (3);
        std::normal_distribution<float> nd (0, 0.1f);
        for (auto& v : noise) v = nd (g2);
        const auto rn = learnTuning (noise.data(), (int) noise.size(), sr);
        check (! rn.ok, "learn: noise is refused (" + rn.message + ")");
    }
    // ---------------------------------------------------------------- built-in tunings (AEU, 53 commas)
    {
        // the scale keys from the tonic up to its octave and the steps in commas the sources give (TDV / AEU tetrachords)
        struct Want { const char* id; std::vector<int> keys; std::vector<int> steps; };
        const std::vector<Want> want = {
            { "rast",   { 67, 69, 71, 72, 74, 76, 78, 79 },     { 9, 8, 5, 9, 9, 8, 5 } },        // T K S T + T K S
            { "ussak",  { 69, 71, 72, 74, 76, 77, 79, 81 },     { 8, 5, 9, 9, 4, 9, 9 } },        // ussak 4li + buselik 5li
            { "hicaz",  { 69, 70, 73, 74, 76, 78, 79, 81 },     { 5, 12, 5, 9, 8, 5, 9 } },       // hicaz 4li + rast 5li
            { "huzzam", { 71, 72, 74, 75, 78, 79, 81, 82, 83 }, { 5, 9, 5, 12, 5, 9, 4, 4 } },    // huzzam 5li (S T S A) on segah
            { "saba",   { 69, 71, 72, 73, 76, 77, 79, 80, 81 }, { 8, 5, 5, 12, 5, 9, 5, 4 } },    // saba 4li (18) + hicaz on cargah
            { "segah",  { 71, 72, 74, 76, 78, 79, 81, 82, 83 }, { 5, 9, 8, 9, 5, 9, 4, 4 } },     // segah 5li S T K T
            { "kurdi",  { 69, 70, 72, 74, 76, 77, 79, 81 },     { 4, 9, 9, 9, 4, 9, 9 } },        // kurdi 4li + buselik 5li
        };
        check (builtin::entries().size() == want.size(), "builtin: " + std::to_string (builtin::entries().size()) + " tunings");
        for (auto& w : want)
        {
            Tuning t;
            const bool found = builtin::find (w.id, t);
            check (found, std::string ("builtin: ") + w.id + " found");
            if (! found) continue;
            const auto tab = t.table();
            bool ok = true; std::string got;
            for (size_t i = 0; i + 1 < w.keys.size(); ++i)
            {
                const double c = 100.0 * (tab[(size_t) w.keys[i + 1]] - tab[(size_t) w.keys[i]]) / builtin::kComma;
                got += (i ? " " : "") + f2 (c);
                ok = ok && near (c, w.steps[i], 0.01);
            }
            int sum = 0; for (int st : w.steps) sum += st;
            check (ok && sum == 53, std::string ("builtin: ") + w.id + " steps in commas " + got);
            check (near (tab[69], 69.0, 1e-4), std::string ("builtin: ") + w.id + " A4 = 440 Hz");
            // a key outside the makam is 12-equal against A
            bool et = true;
            for (int k = 48; k < 84; ++k)
                if ([&] { for (auto& en : builtin::entries()) if (w.id == std::string (en.id)) return en.commas[k % 12] < 0; return false; }())
                    et = et && near (tab[(size_t) k], (double) k, 1e-4);
            check (et, std::string ("builtin: ") + w.id + " other keys 12-equal");
            // Scala round trip keeps it
            Tuning r; scala::readScl (scala::writeScl (t), r); scala::readKbm (scala::writeKbm (t), r);
            const auto rt = r.table();
            bool same = true; for (int k = 0; k < 128; ++k) same = same && near (rt[(size_t) k], tab[(size_t) k], 1e-4);
            check (same, std::string ("builtin: ") + w.id + " .scl/.kbm round trip");
        }
        Tuning rast; builtin::find ("rast", rast);
        // G4 = 440 Hz * 2^(-9 commas / 53)
        check (near (pitchToHz (rast.table()[67]), 440.0 * std::pow (2.0, -9.0 / 53.0), 1e-3), "builtin: rast G4 = " + f2 (pitchToHz (rast.table()[67])) + " Hz");
        check (rast.directional() && near (100.0 * (rast.table (false)[78] - rast.table (true)[78]) / builtin::kComma, 4.0, 0.01)
               && near (rast.table (true)[77], rast.table (false)[77], 1e-4),
               "builtin: rast F#4 falls to acem (4 commas lower), the other keys keep their pitch");
    }
    std::printf ("\n%d / %d checks passed\n", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}
