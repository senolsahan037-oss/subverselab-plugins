// crackle_probe: long self-oscillation probe (tester finding T4, SEDA 0.9.0-test1). Runs Source/SedaEngine.h exactly as the plug-in does, feeds a short burst
// (vocal-like or noise), then silence, and prints one CSV row per window: RMS, peak, crest, share of energy above 8 kHz, click count / worst first-difference outlier,
// and the engine's internal state (servo gain, fbEnv, compander gains, held value, S&H phase, line count). Engine members are read through `#define private public`.
//   crackle_probe [--sr 48000] [--block 512] [--time 350] [--fb 1.10] [--comp 1] [--mix 1] [--age new|old] [--burst vocal|noise] [--burst-s 2.5]
//                 [--clicks N (print the first N clicks after 10 s with the state around them)] [--total-s 90] [--win 1] [--depth 0] [--wav out.wav] [--no-header] [--tag name]
//   17 Oct (RUNAWAY / CHAOS): [--runaway 0..1] [--chaos 0..1] [--note 1/8 --bpm 120 (SYNC)] [--burst none] [--move id:t0:t1:v0:v1]... (linear live ramp of runaway | chaos | time | fb,
//                 applied per block as a host automates) [--summary] (one line: clicks outside CHAOS steps, clicks inside a step window, steps, step glide check, level)
// Build: c++ -O3 -std=c++17 tools/crackle_probe.cpp -o <dir>/crackle_probe
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#define private public
#include "../Source/SedaEngine.h"
#undef private
#include "wavio.h"

struct Hp2 { double s1 = 0, s2 = 0, g = 0, k = 1.41421356, a1 = 0, a2 = 0, a3 = 0;   // TPT SVF high-pass, 2nd order
    void set (double fc, double sr) { g = std::tan (M_PI * fc / sr); a1 = 1.0 / (1.0 + g * (g + k)); a2 = g * a1; a3 = g * a2; }
    double tick (double x) { const double v3 = x - s2, v1 = a1 * s1 + a2 * v3, v2 = s2 + a2 * s1 + a3 * v3; s1 = 2 * v1 - s1; s2 = 2 * v2 - s2; return x - k * v1 - v2; } };

int main (int argc, char** argv)
{
    double sr = 48000, timeMs = 350, fb = 1.10, comp = 1.0, mix = 1.0, burstS = 2.5, totalS = 90, win = 1.0, depth = 0.0;
    int block = 512, age = seda::kNew; std::string burst = "vocal", wavOut, tag; bool header = true; int showClicks = 0;
    double runaway = 0.0, chaos = 0.0, bpm = 120.0; int note = -1; bool summary = false;
    struct Move { std::string id; double t0, t1, v0, v1; }; std::vector<Move> moves;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a (argv[i]); auto nx = [&] { return std::string (argv[++i]); };
        if (a == "--sr") sr = std::atof (nx().c_str()); else if (a == "--block") block = std::atoi (nx().c_str());
        else if (a == "--time") timeMs = std::atof (nx().c_str()); else if (a == "--fb") fb = std::atof (nx().c_str());
        else if (a == "--comp") comp = std::atof (nx().c_str()); else if (a == "--mix") mix = std::atof (nx().c_str());
        else if (a == "--age") age = nx() == "old" ? seda::kOld : seda::kNew; else if (a == "--burst") burst = nx();
        else if (a == "--burst-s") burstS = std::atof (nx().c_str()); else if (a == "--total-s") totalS = std::atof (nx().c_str());
        else if (a == "--win") win = std::atof (nx().c_str()); else if (a == "--depth") depth = std::atof (nx().c_str());
        else if (a == "--runaway") runaway = std::atof (nx().c_str()); else if (a == "--chaos") chaos = std::atof (nx().c_str());
        else if (a == "--bpm") bpm = std::atof (nx().c_str()); else if (a == "--note") note = seda::noteIndex (nx().c_str());
        else if (a == "--summary") summary = true;
        else if (a == "--move") { const std::string v = nx(); Move m; char id[32] = {}; if (std::sscanf (v.c_str(), "%31[^:]:%lf:%lf:%lf:%lf", id, &m.t0, &m.t1, &m.v0, &m.v1) != 5) { std::fprintf (stderr, "bad --move %s\n", v.c_str()); return 2; } m.id = id; moves.push_back (m); }
        else if (a == "--wav") wavOut = nx(); else if (a == "--tag") tag = nx(); else if (a == "--no-header") header = false; else if (a == "--clicks") showClicks = std::atoi (nx().c_str());
        else { std::fprintf (stderr, "unknown %s\n", a.c_str()); return 2; }
    }
    const size_t n = (size_t) (totalS * sr), nb = (size_t) (burstS * sr);
    std::vector<float> in (n, 0.0f);
    uint64_t rs = 12345; auto rnd = [&] { rs ^= rs >> 12; rs ^= rs << 25; rs ^= rs >> 27; return (double) (int64_t) (rs * 0x2545F4914F6CDD1Dull) / 9223372036854775808.0; };
    if (burst == "none") {}
    else if (burst == "noise") for (size_t i = 0; i < nb; ++i) in[i] = (float) (0.25 * rnd());
    else   // vocal-like: harmonic series 220 Hz with 5.5 Hz vibrato, formant-ish roll-off, two syllables, a little breath noise
    {
        double ph = 0;
        for (size_t i = 0; i < nb; ++i)
        {
            const double t = (double) i / sr, f0 = 220.0 * (1.0 + 0.012 * std::sin (2 * M_PI * 5.5 * t)) * (t > 1.2 ? 1.26 : 1.0);
            ph += f0 / sr; double v = 0; for (int h = 1; h <= 18; ++h) v += std::sin (2 * M_PI * ph * h) / (double) h * std::exp (-std::pow ((h * f0 - 900.0) / 1400.0, 2.0) * 0.5);
            const double env = std::min (1.0, t / 0.03) * std::min (1.0, (burstS - t) / 0.05) * (0.7 + 0.3 * std::sin (2 * M_PI * 1.6 * t));
            in[i] = (float) (0.25 * env * v + 0.01 * rnd());
        }
    }
    seda::Params p; p.timeMs = (float) timeMs; p.feedback = (float) fb; p.comp = (float) comp; p.mix = (float) mix; p.age = age; p.depth = (float) depth;
    p.runaway = (float) runaway; p.chaos = (float) chaos; p.bpm = bpm; if (note >= 0) { p.sync = true; p.note = note; }
    static seda::Engine eng; eng.prepare (sr); eng.setParams (p); eng.reset(); eng.setEventLog (true);
    std::vector<double> stepT; std::vector<float> lnClk (n, 0.0f);   // CHAOS: step times; log of the clock factor per sample (block 1 only would be exact; per block here)
    if (summary) header = false;
    std::vector<float> out (n, 0.0f), outR (n, 0.0f), inR = in;
    const size_t W = (size_t) (win * sr);
    Hp2 hp; hp.set (8000.0, sr);
    if (header) std::printf ("tag,sr,block,t_s,rms_db,peak,crest,hf8k_pct,clicks,worst_d_ratio,gap_db,servo,fbEnv_rms,gEnc,gDec,gHeld,lines,mStages,phase,held,msEnc,msDec,acY1,toneLp\n");
    size_t winStart = 0; double eHf = 0, eTot = 0;
    std::vector<float> sGe, sGd, sGh, sSv, sHeld; if (showClicks) { sGe.assign (n, 0); sGd.assign (n, 0); sGh.assign (n, 0); sSv.assign (n, 0); sHeld.assign (n, 0); }
    if (showClicks) block = 1;   // state per sample
    for (size_t pos = 0; pos < n; pos += (size_t) block)
    {
        const int m = (int) std::min ((size_t) block, n - pos);
        for (auto& mv : moves)
        {
            const double t = (double) pos / sr; if (t < mv.t0) continue;
            const double v = t >= mv.t1 ? mv.v1 : mv.v0 + (mv.v1 - mv.v0) * (t - mv.t0) / std::max (1e-9, mv.t1 - mv.t0);
            if (mv.id == "runaway") p.runaway = (float) v; else if (mv.id == "chaos") p.chaos = (float) v; else if (mv.id == "time") p.timeMs = (float) v; else if (mv.id == "fb") p.feedback = (float) v;
        }
        eng.setParams (p);
        eng.process (in.data() + pos, inR.data() + pos, out.data() + pos, outR.data() + pos, m);
        if (showClicks) { const auto& c = eng.vo[eng.mainV].ch[0]; sGe[pos] = (float) c.gEnc; sGd[pos] = (float) c.gDec; sGh[pos] = (float) c.gHeld; sSv[pos] = (float) c.servo; sHeld[pos] = (float) c.held; }
        for (int i = 0; i < m; ++i) { const double y = out[pos + (size_t) i], h = hp.tick (y); eTot += y * y; eHf += h * h; }
        { seda::Event e; while (eng.popEvent (e)) if (e.type == seda::kEvChaosStep) stepT.push_back ((double) e.frame / sr); }
        lnClk[pos] = (float) std::log (eng.chaosClockRatio());
        const size_t endPos = pos + (size_t) m;
        if (! summary && (endPos - winStart >= W || endPos >= n))
        {
            double sq = 0, pk = 0; const size_t a0 = winStart, a1 = endPos;
            for (size_t i = a0; i < a1; ++i) { sq += (double) out[i] * out[i]; pk = std::max (pk, (double) std::fabs (out[i])); }
            const double rms = std::sqrt (sq / (double) (a1 - a0));
            // click detector: first difference against its own local RMS (+-512 samples, prefix sums)
            std::vector<double> d (a1 - a0, 0.0), cs (a1 - a0 + 1, 0.0);
            for (size_t i = 1; i < d.size(); ++i) d[i] = (double) out[a0 + i] - (double) out[a0 + i - 1];
            for (size_t i = 0; i < d.size(); ++i) cs[i + 1] = cs[i] + d[i] * d[i];
            int clicks = 0; double worst = 0; const size_t H = 512;
            for (size_t i = 1; i < d.size(); ++i)
            {
                const size_t lo = i > H ? i - H : 0, hi = std::min (d.size(), i + H + 1);
                const double loc = std::sqrt ((cs[hi] - cs[lo] - d[i] * d[i]) / (double) (hi - lo - 1) + 1e-30);
                const double r = std::fabs (d[i]) / loc; worst = std::max (worst, r);
                if (r > 8.0 && std::fabs (d[i]) > 2e-3) ++clicks;
            }
            double gapDb = 0; { const size_t b = (size_t) (0.01 * sr); double mn = 1e9, mxv = 0; for (size_t i = a0; i + b <= a1; i += b) { double q = 0; for (size_t k = 0; k < b; ++k) q += (double) out[i + k] * out[i + k]; q = std::sqrt (q / (double) b); mn = std::min (mn, q); mxv = std::max (mxv, q); } gapDb = 20 * std::log10 ((mxv + 1e-12) / (mn + 1e-12)); }   // loudest / quietest 10 ms in the window: the pumping depth
            const auto& c = eng.vo[eng.mainV].ch[0];
            std::printf ("%s,%.0f,%d,%.1f,%.2f,%.4f,%.2f,%.4f,%d,%.1f,%.1f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%d,%.3f,%.5f,%.3e,%.3e,%.5f,%.5f\n", tag.c_str(), sr, block, (double) a1 / sr,
                         20 * std::log10 (rms + 1e-12), pk, pk / (rms + 1e-12), 100.0 * eHf / (eTot + 1e-30), clicks, worst, gapDb, c.servo, std::sqrt (c.fbEnv), c.gEnc, c.gDec, c.gHeld, c.lines, c.mStages, c.phase, c.held, c.msEnc, c.msDec, c.acY1, c.toneLp.s);
            std::fflush (stdout);
            winStart = endPos; eHf = eTot = 0;
        }
    }
    if (showClicks)
    {
        int shown = 0; const size_t H = 512;
        for (size_t i = (size_t) (10 * sr); i + 2 < n && shown < showClicks; ++i)
        {
            const double dd = (double) out[i] - (double) out[i - 1]; double sq = 0; for (size_t k = i - H; k < i + H; ++k) if (k != i) { const double e = (double) out[k] - (double) out[k - 1]; sq += e * e; }
            const double loc = std::sqrt (sq / (2.0 * H - 1)); if (std::fabs (dd) / (loc + 1e-30) > 8.0 && std::fabs (dd) > 2e-3)
            {
                ++shown; std::printf ("CLICK t=%.4f d=%.4f localRms=%.4f\n", (double) i / sr, dd, loc);
                for (size_t k = i - 3; k <= i + 2; ++k) std::printf ("   y=%.4f  gEnc=%.4f gDec=%.4f gHeld=%.4f servo=%.4f held=%.4f\n", out[k], sGe[k], sGd[k], sGh[k], sSv[k], sHeld[k]);
                i += 200;
            }
        }
    }
    if (summary)
    {   // whole render after 1 s: clicks outside / inside CHAOS step windows (step - 2 ms .. step + glide + 20 ms), worst first-difference ratio of each, level of the last 10 s
        const size_t H = 512, a0 = (size_t) (1.0 * sr);
        std::vector<double> d (n, 0.0), cs (n + 1, 0.0);
        for (size_t i = 1; i < n; ++i) d[i] = (double) out[i] - (double) out[i - 1];
        for (size_t i = 0; i < n; ++i) cs[i + 1] = cs[i] + d[i] * d[i];
        std::vector<char> inStep (n, 0);
        for (double t : stepT) { const size_t s0 = (size_t) std::max (0.0, (t - 0.002) * sr), s1 = std::min (n, (size_t) ((t + seda::perform::kChaosGlideSec + 0.020) * sr)); for (size_t i = s0; i < s1; ++i) inStep[i] = 1; }
        int cOut = 0, cIn = 0; double wOut = 0, wIn = 0; bool nan = false;
        for (size_t i = 1; i < n; ++i)
        {
            if (! std::isfinite (out[i])) nan = true;
            if (i < a0) continue;
            const size_t lo = i > H ? i - H : 0, hi = std::min (n, i + H + 1);
            const double loc = std::sqrt ((cs[hi] - cs[lo] - d[i] * d[i]) / (double) (hi - lo - 1) + 1e-30), r = std::fabs (d[i]) / loc;
            const bool c = r > 8.0 && std::fabs (d[i]) > 2e-3;
            if (inStep[i]) { wIn = std::max (wIn, r); cIn += c; } else { wOut = std::max (wOut, r); cOut += c; }
        }
        double sq = 0, pk = 0; const size_t l0 = n > (size_t) (10 * sr) ? n - (size_t) (10 * sr) : 0;
        for (size_t i = l0; i < n; ++i) { sq += (double) out[i] * out[i]; pk = std::max (pk, (double) std::fabs (out[i])); }
        std::printf ("%s sr=%.0f block=%d clicks_outside_steps=%d worst_ratio_outside=%.1f steps=%zu clicks_in_step_windows=%d worst_ratio_in_steps=%.1f last10s_rms_db=%.2f last10s_peak_dbfs=%.2f nan=%d\n",
                     tag.c_str(), sr, block, cOut, wOut, stepT.size(), cIn, wIn, 20 * std::log10 (std::sqrt (sq / (double) std::max<size_t> (1, n - l0)) + 1e-12), 20 * std::log10 (pk + 1e-12), (int) nan);
    }
    if (! wavOut.empty()) writeWav (wavOut.c_str(), (int) sr, 1, out, out);
    return 0;
}
