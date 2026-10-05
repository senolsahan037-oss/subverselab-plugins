#pragma once
// Swell selftests (RISE / CAP / DARKEN / END / tempo lock). Included by
// render.cpp after its helpers (process, line, fmt, preset). Run from the
// YANKI folder: needs ../_kaynak/OG_vokal_20s.wav and tools/drum_loop.wav.
#include "md5.h"
#include "swell_source.h"

namespace swelltest {

static const char* kVocal = "../_kaynak/OG_vokal_20s.wav";
static const char* kDrum = "tools/drum_loop.wav";

// Renders in 512-sample blocks; END trigger raised exactly at endAt seconds.
inline std::vector<float> render(const std::vector<float>& in, int sr, yanki::Controls c, double endAt = -1.0) {
    std::vector<float> l(in.size() / 2), r(l.size());
    for (size_t i = 0; i < l.size(); ++i) { l[i] = in[2 * i]; r[i] = in[2 * i + 1]; }
    yanki::YankiEngine e; e.prepare(sr);
    const size_t trig = endAt >= 0.0 ? static_cast<size_t>(endAt * sr) : l.size() + 1;
    size_t p = 0;
    while (p < l.size()) {
        size_t n = std::min<size_t>(512, l.size() - p);
        if (p < trig && p + n > trig) n = trig - p;
        c.endTrigger = p >= trig;
        e.process(l.data() + p, r.data() + p, static_cast<int>(n), c);
        p += n;
    }
    std::vector<float> out(in.size());
    for (size_t i = 0; i < l.size(); ++i) { out[2 * i] = l[i]; out[2 * i + 1] = r[i]; }
    return out;
}
inline std::vector<float> loopPart(const std::vector<float>& out, const std::vector<float>& dry) {
    std::vector<float> y(out.size()); for (size_t i = 0; i < y.size(); ++i) y[i] = out[i] - dry[i]; return y;
}
inline double db(double v) { return 20.0 * std::log10(std::max(v, 1e-12)); }
// Loop level (dB rel. dry phrase RMS) = RMS over the limiter's own detector
// window (400 ms, about one tape pass), hop 10 ms, reported at the window
// CENTRE (the unbiased level at t; a trailing meter reads W/2 = 200 ms late).
static const double kWin = yanki::YankiEngine::kDetectSec;
inline std::vector<double> levels100(const std::vector<float>& y, int sr, double ref) {
    std::vector<double> v; const double len = double(y.size() / 2) / sr;
    for (double t = 0.0; t + kWin <= len + 1e-9; t += 0.01) v.push_back(db(spanRmsSt(y, sr, t, t + kWin) / ref));
    return v;   // v[k] = window [0.01 k, 0.01 k + kWin]
}
inline double firstReach(const std::vector<double>& v, double thrDb, bool centred = true) { for (size_t k = 0; k < v.size(); ++k) if (v[k] >= thrDb) return (centred ? 0.5 * kWin : kWin) + 0.01 * double(k); return -1.0; }
inline bool finiteBounded(const std::vector<float>& x, double lim = 2.0) { for (float v : x) if (!std::isfinite(v) || std::abs(v) >= lim) return false; return true; }
inline double maxJump(const std::vector<float>& y, int sr, double t0, double t1) {
    double m = 0; const size_t a = static_cast<size_t>(std::max(0.0, t0) * sr) + 1, b = std::min(y.size() / 2, static_cast<size_t>(t1 * sr));
    for (size_t i = a; i < b; ++i) for (int ch = 0; ch < 2; ++ch) m = std::max(m, std::abs(double(y[2 * i + ch]) - y[2 * (i - 1) + ch]));
    return m;
}
// Build rate (dB/s) from the first 0.5 s-RMS crossing of 0 dB to that of +6 dB (0 if +6 dB is never reached).
inline double buildRate(const std::vector<float>& y, int sr, double ref) {
    double tA = -1, tB = -1; const double len = double(y.size() / 2) / sr;
    for (double t = 0; t + 0.5 <= len; t += 0.05) { const double d = db(spanRmsSt(y, sr, t, t + 0.5) / ref); if (tA < 0 && d >= 0) tA = t; if (tB < 0 && d >= 6) tB = t; }
    return (tA >= 0 && tB > tA) ? 6.0 / (tB - tA) : 0.0;
}

inline bool run(int& checks) {
    bool all = true;
    const auto check = [&](const std::string& name, bool ok, const std::string& v) { line(name, ok, v); all = all && ok; ++checks; };

    // (1) the four original presets, bit-identical at their defaults: same path as `tools/render in out --preset P`
    struct Ref { const char* p; const char* vocal; const char* drum; };
    // md5 of the renders made BEFORE this change (original binary): the 4 original
    // moods plus the 2 stage moods (Yesilcam = "Yesilcam Sahne", Ters = "Ters Yanki")
    const Ref refs[6] = {{"Slapback", "f481e4e0d478bac24ec1dfbb640be824", "704e276d3dda0044a5658aa344c3c6e3"},
                         {"ThreeHeads", "be03461633645101ab33e351f3bd925d", "84d4a77675f6a1eb65c21e8da566aaa7"},
                         {"DubSpiral", "d984c355b6b31a3706a9cbf6807598c9", "4e66d755902c26d2e71f88788c8a84c6"},
                         {"WornLoop", "8676cd472f67f45bd9969b792eb366d0", "eb90629160f23034c626b58f5a2b3375"},
                         {"Yesilcam Sahne", "597e9666f426aa6bd393d7e2a3152c2d", "e72547afa654e7ca173a863578db3302"},
                         {"Ters Yanki", "d25750c9b86520bae5a3cb5dd1c887c1", "097098475e815bbeeac1b75e940b8dde"}};
    for (const Ref& R : refs) {
        std::string got[2];
        for (int k = 0; k < 2; ++k) {
            WavData w; if (!readWav(k == 0 ? kVocal : kDrum, w)) { check(std::string("original_md5_") + R.p, false, "cannot read source"); return false; }
            std::vector<float> x(w.samples.begin(), w.samples.end());
            if (w.channels == 1) { std::vector<float> s(x.size() * 2); for (size_t i = 0; i < x.size(); ++i) s[i * 2] = s[i * 2 + 1] = x[i]; x.swap(s); }
            process(x, w.sampleRate, preset(R.p));
            got[k] = md5hex(encodeWav24(w.sampleRate, x));
        }
        check(std::string("mood_md5_") + R.p, got[0] == R.vocal && got[1] == R.drum,
              std::string("vocal ") + R.vocal + "->" + got[0] + " drum " + R.drum + "->" + got[1]);
    }

    // TIME sync: impulse, first repeat (head 1) = argmax |wet| near the expected delay, 120 bpm
    {
        const int sr = 48000; const struct { int sync; const char* name; double ms; } divs[5] = {{4, "1/16", 125.0}, {1, "1/8", 250.0}, {3, "1/8d", 375.0}, {2, "1/4", 500.0}, {5, "1/2", 1000.0}};
        const auto measure = [&](yanki::Controls c, double wantMs) {
            std::vector<float> x(static_cast<size_t>(sr) * 3 * 2, 0.0f); x[0] = x[1] = 0.5f;
            const auto y = render(x, sr, c);
            const size_t a = static_cast<size_t>(0.5 * wantMs * 0.001 * sr), b = static_cast<size_t>(1.5 * wantMs * 0.001 * sr);
            size_t at = a; double pk = 0; for (size_t i = a; i < b; ++i) { const double v = std::abs(double(y[2 * i]) - x[2 * i]) + std::abs(double(y[2 * i + 1]) - x[2 * i + 1]); if (v > pk) { pk = v; at = i; } }
            return 1000.0 * double(at) / sr;
        };
        for (int mood : {3, 2, 5}) {
            std::string row, wowRow; bool ok = true;
            for (const auto& d : divs) {
                yanki::Controls c = yanki::moodControls(mood); c.sync = d.sync; c.bpm = 120.0; c.mix = 1.0;
                const double withWow = measure(c, d.ms);
                c.wear = 0.0; const double ms = measure(c, d.ms);
                ok = ok && std::abs(ms - d.ms) <= 1.0;
                row += std::string(" ") + d.name + "=" + fmt(ms, 3) + "ms(want " + fmt(d.ms, 0) + ")";
                wowRow += std::string(" ") + d.name + "=" + fmt(withWow, 3);
            }
            check(std::string("time_sync_120bpm_mood") + std::to_string(mood), ok, std::string(yanki::kMoodNames[mood]) + " WEAR0" + row + " tol=1ms");
            std::cout << "INFO time_sync_with_mood_wow mood" << mood << " WEAR=" << fmt(yanki::moodControls(mood).wear, 2) << wowRow << " ms (tape wow moves the heads)\n";
        }
    }
    const SwellSource S = loadSwellSource(kVocal);
    if (!S.ok) { check("swell_source", false, "cannot read vocal"); return false; }
    const int sr = S.sr; const double ref = S.phraseRms;
    { bool ok = true; std::string v; for (double d : S.per2s) { v += " " + fmt(d); ok = ok && d > -35.0; }
      check("swell_source_rms_per_2s", ok, "dBFS" + v + " (limit > -35) phrase_start_s=" + fmt(S.phraseStart) + " phrase_rms_dbfs=" + fmt(db(ref))); }
    bool fin = true;   // (6) collected over every swell render below

    // (2) CAP -3 dB: loop never above dry phrase RMS -3 dB (+0.5 dB tolerance) over 12 s
    {
        yanki::Controls c = preset("DubSpiral"); c.capOn = true; c.capDb = -3.0;
        const auto out = render(S.dry, sr, c); fin = fin && finiteBounded(out);
        const auto v = levels100(loopPart(out, S.dry), sr, ref);
        double mx = -300; double at = 0; for (size_t k = 0; k < v.size(); ++k) if (v[k] > mx) { mx = v[k]; at = 0.5 * kWin + 0.01 * double(k); }
        yanki::Controls o = preset("DubSpiral"); const auto vo = levels100(loopPart(render(S.dry, sr, o), S.dry), sr, ref);
        double mo = -300; for (double d : vo) mo = std::max(mo, d);
        check("cap_minus3", mx <= -2.5, "DubSpiral max_loop_db_rel_dry(400ms RMS win)=" + fmt(mx) + " at_s=" + fmt(at) + " limit=-2.50 (uncapped max=" + fmt(mo) + ")");
    }
    // CAP must also hold at the fastest RISE (ThreeHeads builds ~20 dB/s at RISE 1)
    for (const char* p : {"DubSpiral", "ThreeHeads"}) {
        yanki::Controls c = preset(p); c.capOn = true; c.capDb = 3.0; c.rise = 1.0;
        const auto out = render(S.dry, sr, c); fin = fin && finiteBounded(out);
        const auto v = levels100(loopPart(out, S.dry), sr, ref);
        double mx = -300, at = 0; for (size_t k = 0; k < v.size(); ++k) if (v[k] > mx) { mx = v[k]; at = 0.5 * kWin + 0.01 * double(k); }
        check(std::string("cap_plus3_rise1_") + p, mx <= 3.5, "max_loop_db_rel_dry(400ms RMS win)=" + fmt(mx) + " at_s=" + fmt(at) + " limit=+3.50");
    }
    // (3) RISE: time to reach 95 % of CAP at RISE 0.2 vs 0.8. Pass criterion at
    // CAP +6 dB: the phrase's own echo already reaches about +2..+3 dB during
    // the phrase whatever the RISE, so a lower CAP measures the echo, not the
    // swell. All four CAPs are printed.
    {
        bool ok3 = false;
        for (double cp : {0.0, 3.0, 6.0, 9.0}) {
            double t[2] = {0, 0}; const double rs[2] = {0.2, 0.8};
            for (int k = 0; k < 2; ++k) {
                yanki::Controls c = preset("DubSpiral"); c.capOn = true; c.capDb = cp; c.rise = rs[k];
                const auto out = render(S.dry, sr, c); fin = fin && finiteBounded(out);
                t[k] = firstReach(levels100(loopPart(out, S.dry), sr, ref), cp + 20.0 * std::log10(0.95));
            }
            const bool ok = t[0] > 0 && t[1] > 0 && t[0] >= 2.0 * t[1];
            const std::string v = "DubSpiral CAP" + std::string(cp >= 0 ? "+" : "") + fmt(cp, 0) + " time_to_95pct_s RISE0.2=" + fmt(t[0]) + " RISE0.8=" + fmt(t[1]) + " ratio=" + fmt(t[1] > 0 ? t[0] / t[1] : 0) + " limit>=2";
            if (cp == 6.0) { check("rise_time_to_cap", ok, v); ok3 = ok; }
            else std::cout << "INFO rise_time_to_cap " << v << (t[0] < 0 ? " (RISE 0.2 never reaches it in 12 s)" : "") << "\n";
        }
        (void)ok3;
    }
    // measured build-rate table (the tempo lock uses it)
    {
        std::vector<float> longDry = S.dry; longDry.resize(static_cast<size_t>(24.0 * sr) * 2, 0.0f);
        std::string tab; bool match = true; double runMin = 1e9; double table[11];
        for (int k = 10; k >= 0; --k) {
            double mn = 1e9; std::string per;
            for (const char* p : {"DubSpiral", "WornLoop", "ThreeHeads"}) {
                yanki::Controls c = preset(p); c.rise = 0.1 * k;
                const auto out = render(longDry, sr, c); fin = fin && finiteBounded(out);
                const double r = buildRate(loopPart(out, longDry), sr, ref); mn = std::min(mn, r); per += " " + fmt(r);
            }
            runMin = std::min(runMin, mn); table[k] = runMin;
            std::cout << "INFO rise_build_rate RISE " << fmt(0.1 * k, 1) << " x" << fmt(yanki::YankiEngine::riseToOverGain(0.1 * k), 3)
                      << " critical  DubSpiral/WornLoop/ThreeHeads dB/s:" << per << "  slowest(non-decreasing)=" << fmt(runMin) << "\n";
        }
        for (int k = 0; k <= 10; ++k) { tab += " " + fmt(table[k]); match = match && std::abs(table[k] - yanki::YankiEngine::kRiseRate[k]) < 0.01; }
        check("rise_table_matches_measurement", match, "measured" + tab);
    }
    // (4) END CUT / END DUMP (trigger at 6.0 s, CAP +3, loop holding)
    const double t0 = 6.0;
    {
        yanki::Controls c = preset("DubSpiral"); c.capOn = true; c.capDb = 3.0; c.endMode = yanki::kEndCut;
        const auto out = render(S.dry, sr, c, t0); fin = fin && finiteBounded(out);
        const auto y = loopPart(out, S.dry);
        const double before = db(spanRmsSt(y, sr, t0 - 0.5, t0) / ref), after = db(spanRmsSt(y, sr, t0 + 0.05, t0 + 0.15) / ref);
        double pk = 0; for (size_t i = static_cast<size_t>((t0 + 0.05) * sr) * 2; i < y.size(); ++i) pk = std::max(pk, std::abs(double(y[i])));
        const double jNear = maxJump(y, sr, t0 - 0.05, t0 + 0.1), jRef = maxJump(y, sr, t0 - 1.0, t0 - 0.05);
        check("end_cut", after < -60.0 && db(pk / ref) < -60.0,
              "loop_db_rel_dry before=" + fmt(before) + " 50-150ms_after=" + fmt(after) + " peak_after_50ms_db=" + fmt(db(pk / ref)) + " limit<-60");
        check("end_cut_no_click", jNear <= jRef, "max_step near_trigger=" + fmt(jNear, 5) + " <= in_loop_before=" + fmt(jRef, 5));
    }
    {
        yanki::Controls c = preset("DubSpiral"); c.capOn = true; c.capDb = 3.0; c.endMode = yanki::kEndDump;
        const auto out = render(S.dry, sr, c, t0); fin = fin && finiteBounded(out);
        const auto y = loopPart(out, S.dry);
        // correlation of the loop after the trigger with the time-reversed loop before it
        const size_t T0 = static_cast<size_t>(t0 * sr), j0 = static_cast<size_t>(0.03 * sr), j1 = static_cast<size_t>(0.6 * sr);
        double sxy = 0, sxx = 0, syy = 0;
        for (size_t j = j0; j < j1; ++j) for (int ch = 0; ch < 2; ++ch) {
            const double a = y[2 * (T0 + j) + ch], b = y[2 * (T0 - 1 - j) + ch]; sxy += a * b; sxx += a * a; syy += b * b; }
        const double corr = sxy / std::sqrt(std::max(1e-30, sxx * syy));
        double fwd = 0; { double f = 0, g = 0, h = 0; for (size_t j = j0; j < j1; ++j) for (int ch = 0; ch < 2; ++ch) { const double a = y[2 * (T0 + j) + ch], b = y[2 * (T0 - j1 + j) + ch]; f += a * b; g += a * a; h += b * b; } fwd = f / std::sqrt(std::max(1e-30, g * h)); }
        const double tail = db(spanRmsSt(y, sr, t0 + 2.9, t0 + 3.0) / ref);
        double dieAt = -1; { const auto v = levels100(y, sr, ref); for (size_t k = static_cast<size_t>((t0 - 0.05) / 0.01); k < v.size(); ++k) if (v[k] < -50.0) { bool stays = true; for (size_t q = k; q < v.size(); ++q) if (v[q] >= -50.0) stays = false; if (stays) { dieAt = 0.5 * kWin + 0.01 * double(k) - t0; break; } } }
        check("end_dump_reversed", corr > 0.5, "corr_with_time_reversed_loop(30-600ms)=" + fmt(corr, 3) + " (same window vs forward loop=" + fmt(fwd, 3) + ") limit>0.5");
        check("end_dump_dies", tail < -50.0 && dieAt >= 0.0 && dieAt <= 3.0, "loop_db_rel_dry 2.9-3.0s_after=" + fmt(tail) + " stays_below_-50dB_from_s=" + fmt(dieAt) + " limit<=3.0");
        const double jNear = maxJump(y, sr, t0 - 0.05, t0 + 0.1), jRef = maxJump(y, sr, t0 - 1.0, t0 - 0.05);
        check("end_dump_no_click", jNear <= jRef, "max_step near_trigger=" + fmt(jNear, 5) + " <= in_loop_before=" + fmt(jRef, 5));
    }
    {
        yanki::Controls c = preset("DubSpiral"); c.capOn = true; c.capDb = 3.0; c.endMode = yanki::kEndFreeze;
        const auto out = render(S.dry, sr, c, t0); fin = fin && finiteBounded(out);
        const auto y = loopPart(out, S.dry);
        const double last = db(spanRmsSt(y, sr, 11.0, 12.0) / ref);
        const double jNear = maxJump(y, sr, t0 - 0.05, t0 + 0.1), jRef = maxJump(y, sr, t0 - 1.0, t0 - 0.05);
        check("end_freeze_sustains", last > -20.0 && jNear <= jRef, "loop_db_rel_dry 11-12s=" + fmt(last) + " max_step near/before=" + fmt(jNear, 5) + "/" + fmt(jRef, 5));
    }
    // (5) tempo lock: 120 bpm, 2 bars -> CAP (+3 dB) on the downbeat at 4.0 s
    {
        yanki::Controls c = preset("DubSpiral"); c.capOn = true; c.capDb = 3.0; c.bpm = 120.0; c.lockBars = 2.0;
        const auto out = render(S.dry, sr, c); fin = fin && finiteBounded(out);
        const auto v = levels100(loopPart(out, S.dry), sr, ref);
        const double t95 = firstReach(v, 3.0 + 20.0 * std::log10(0.95)), t95m = firstReach(v, 3.0 + 20.0 * std::log10(0.95), false);
        double mx = -300; for (double d : v) mx = std::max(mx, d);
        check("tempo_lock_120bpm_2bars", t95 > 0 && std::abs(t95 - 4.0) <= 0.12,
              "required_RISE=" + fmt(yanki::YankiEngine::rateToRise(yanki::YankiEngine::lockSlope(c)), 3) + " used_RISE=" + fmt(yanki::YankiEngine::lockRise(c), 3)
              + " line_body_db_per_s=" + fmt(yanki::YankiEngine::lockSlope(c)) + " final_0.5s=12dB/s" + " t_95pct_cap_s(400ms window centred)=" + fmt(t95, 3) + " (trailing meter: " + fmt(t95m, 3) + ") target=4.000+-0.120 max_db=" + fmt(mx));
    }
    // tempo-lock robustness (INFO, not a pass criterion): CAP +3, 3 presets x 5 tempo/bar settings
    {
        std::vector<float> longDry = S.dry; longDry.resize(static_cast<size_t>(14.0 * sr) * 2, 0.0f);
        const double tb[5][2] = {{120, 2}, {90, 2}, {128, 2}, {100, 3}, {140, 4}};
        for (const char* p : {"DubSpiral", "WornLoop", "ThreeHeads"}) {
            std::string row; int ok = 0; double worstOver = -99;
            for (const auto& b : tb) {
                yanki::Controls c = preset(p); c.capOn = true; c.capDb = 3.0; c.bpm = b[0]; c.lockBars = b[1];
                const auto out = render(longDry, sr, c); fin = fin && finiteBounded(out);
                const auto v = levels100(loopPart(out, longDry), sr, ref);
                const double T = yanki::YankiEngine::lockSeconds(c), t = firstReach(v, 3.0 + 20.0 * std::log10(0.95));
                double mx = -300; for (double d : v) mx = std::max(mx, d);
                worstOver = std::max(worstOver, mx - 3.0); if (t > 0 && std::abs(t - T) <= 0.12) ++ok;
                row += " " + fmt(b[0], 0) + "bpm/" + fmt(b[1], 0) + "bars:" + (t > 0 ? fmt(t - T, 3) : std::string("never"));
            }
            std::cout << "INFO tempo_lock_scan " << p << " CAP+3 arrival_error_s" << row << " within_0.12=" << ok << "/5 max_over_cap_db=" << fmt(worstOver) << "\n";
        }
    }
    // DARKEN: darker loop at the same CAP (spectral centroid of the held loop 8-12 s)
    {
        double cen[2] = {0, 0};
        for (int k = 0; k < 2; ++k) {
            yanki::Controls c = preset("DubSpiral"); c.capOn = true; c.capDb = 0.0; c.darken = k ? 0.6 : 0.0;
            const auto out = render(S.dry, sr, c); fin = fin && finiteBounded(out);
            cen[k] = centroidSpan(loopPart(out, S.dry), sr, 8.0, 12.0);
        }
        check("darken_lowers_centroid", cen[1] < 0.8 * cen[0], "loop_centroid_hz_8-12s DARKEN0=" + fmt(cen[0], 0) + " DARKEN0.6=" + fmt(cen[1], 0) + " limit<0.8x");
    }
    // every swell control changes the output; at MIX 0 none does
    {
        const yanki::Controls base = preset("DubSpiral");
        const auto a = render(S.dry, sr, base);
        std::string bad; bool ok = true, byp = true;
        for (int k = 0; k < 7; ++k) {
            yanki::Controls q = base; double endAt = -1;
            if (k == 0) q.rise = 0.3; if (k == 1) { q.capOn = true; q.capDb = 0; } if (k == 2) q.darken = 0.5;
            if (k == 3) { q.endMode = yanki::kEndCut; endAt = 6.0; } if (k == 4) { q.endMode = yanki::kEndDump; endAt = 6.0; }
            if (k == 5) { q.lockBars = 3; q.capOn = true; } if (k == 6) { q.endBars = 4; q.endMode = yanki::kEndCut; q.bpm = 120; }
            const auto b = render(S.dry, sr, q, endAt); fin = fin && finiteBounded(b);
            double d = 0; for (size_t i = 0; i < a.size(); ++i) d += (double(a[i]) - b[i]) * (double(a[i]) - b[i]);
            const double rel = db(std::sqrt(d / a.size()) / ref); if (!(rel > -60.0)) { ok = false; bad += " k" + std::to_string(k) + "=" + fmt(rel); }
            yanki::Controls z = q; z.mix = 0.0; byp = byp && render(S.dry, sr, z, endAt) == S.dry;
        }
        check("swell_controls_audible", ok, ok ? "rise cap darken end_cut end_dump lock endbars all changed (>-60 dB)" : "unchanged:" + bad);
        check("swell_controls_mix0_dry", byp, "bit-exact");
    }
    // CPU: 60 s stereo at 48 kHz, every swell block active (CAP + RISE + DARKEN + tempo lock)
    {
        yanki::YankiEngine e; e.prepare(48000); yanki::Controls c = preset("DubSpiral"); c.capOn = true; c.capDb = 3; c.rise = 0.6; c.darken = 0.3; c.lockBars = 4; c.bpm = 120;
        std::vector<float> l(48000 * 60), r(48000 * 60); for (size_t i = 0; i < l.size(); ++i) l[i] = r[i] = (i % 96000) < 48000 ? 0.1f * float(std::sin(6.283185307 * 220.0 * i / 48000.0)) : 0.0f;
        const auto a = std::chrono::steady_clock::now();
        for (size_t p = 0; p < l.size(); p += 512) e.process(l.data() + p, r.data() + p, static_cast<int>(std::min<size_t>(512, l.size() - p)), c);
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count();
        check("cpu_60s_swell", sec < 3.0, "seconds=" + fmt(sec, 3) + " limit<3");
    }
    // (6) no NaN / Inf, |x| < 2 on every swell render above, plus noise/sine/silence at 3 rates
    {
        bool rates = true;
        for (int rate : {44100, 48000, 96000}) {
            std::vector<float> z(static_cast<size_t>(rate) * 6 * 2, 0.0f); uint32_t seed = 1;
            for (size_t i = 0; i < static_cast<size_t>(rate) * 2; ++i) { seed = seed * 1664525u + 1013904223u; const float n = (float(seed >> 8) / 16777216.0f - 0.5f); z[2 * i] = z[2 * i + 1] = 0.5f * n + 0.4f * float(std::sin(6.283185307 * 1000.0 * i / rate)); }
            for (int m = 0; m < 4; ++m) { yanki::Controls c = preset("WornLoop"); c.capOn = true; c.capDb = 12; c.rise = 1.0; c.darken = 1.0; c.endMode = m; c.lockBars = m == 3 ? 1 : 0; rates = rates && finiteBounded(render(z, rate, c, 4.0)); }
        }
        check("swell_finite_bounded", fin && rates, "all swell renders finite, |x|<2; noise+sine 44.1/48/96 kHz x 4 END modes");
    }
    return all;
}


// ---- clean path (ECHO / Yesilcam / Ters): swell block, one-knob FEEDBACK, meters ----
inline bool runClean(int& checks) {
    bool all = true;
    const auto check = [&](const std::string& name, bool ok, const std::string& v) { line(name, ok, v); all = all && ok; ++checks; };
    // ECHO default unchanged by the clean-path swell work (md5 of the renders made before it)
    {
        const char* want[2] = {"c3c49c2aed66396ca113dd9d8692c9e9", "8aeaf367c4ad3c63efd7c1bfaae2d6cb"}; std::string got[2];
        for (int k = 0; k < 2; ++k) {
            WavData w; if (!readWav(k == 0 ? kVocal : kDrum, w)) { check("echo_md5_default", false, "cannot read"); return false; }
            std::vector<float> x(w.samples.begin(), w.samples.end());
            if (w.channels == 1) { std::vector<float> s2(x.size() * 2); for (size_t i = 0; i < x.size(); ++i) s2[i * 2] = s2[i * 2 + 1] = x[i]; x.swap(s2); }
            process(x, w.sampleRate, yanki::defaultControls()); got[k] = md5hex(encodeWav24(w.sampleRate, x));
        }
        check("echo_md5_default", got[0] == want[0] && got[1] == want[1], std::string("vocal ") + want[0] + "->" + got[0] + " drum " + want[1] + "->" + got[1]);
    }
    const SwellSource S = loadSwellSource(kVocal);
    const int sr = S.sr; const double ref = S.phraseRms;
    bool fin = true;
    const auto perSecondPeaks = [&](const std::vector<double>& v, int s0, int s1, double& lo, double& hi, std::string& row) {
        lo = 1e9; hi = -1e9;
        for (int s = s0; s < s1; ++s) { double p = -300; for (size_t k = 0; k < v.size(); ++k) { const double t = 0.5 * kWin + 0.01 * double(k); if (t >= s && t < s + 1) p = std::max(p, v[k]); } lo = std::min(lo, p); hi = std::max(hi, p); row += " " + fmt(p); }
    };
    // ECHO + RISE 0.5 + CAP +3: swells, then holds at +3 +-0.5 dB
    {
        yanki::Controls c = yanki::defaultControls(); c.rise = 0.5; c.capOn = true; c.capDb = 3.0;
        const auto out = render(S.dry, sr, c); fin = fin && finiteBounded(out);
        const auto v = levels100(loopPart(out, S.dry), sr, ref);
        double mx = -300; for (double d : v) mx = std::max(mx, d);
        double lo, hi; std::string row; perSecondPeaks(v, 5, 12, lo, hi, row);
        const double early = v[static_cast<size_t>((0.5 - 0.5 * kWin) / 0.01)];
        check("echo_swell_cap_plus3", mx <= 3.5 && lo >= 2.5 && hi <= 3.5 && early < lo - 6.0,
              "ECHO RISE0.5 CAP+3 loop_db_rel_dry at_0.5s=" + fmt(early) + " per-second peaks 5-12s:" + row + " max=" + fmt(mx) + " hold window [2.5,3.5]");
    }
    // END CUT / DUMP on ECHO (RISE 0.5, CAP +3, trigger 6.0 s)
    const double t0 = 6.0;
    {
        yanki::Controls c = yanki::defaultControls(); c.rise = 0.5; c.capOn = true; c.capDb = 3.0; c.endMode = yanki::kEndCut;
        const auto out = render(S.dry, sr, c, t0); fin = fin && finiteBounded(out);
        const auto y = loopPart(out, S.dry);
        const double before = db(spanRmsSt(y, sr, t0 - 0.5, t0) / ref), after = db(spanRmsSt(y, sr, t0 + 0.05, t0 + 0.15) / ref);
        const double jNear = maxJump(y, sr, t0 - 0.05, t0 + 0.1), jRef = maxJump(y, sr, t0 - 1.0, t0 - 0.05);
        check("echo_end_cut", after < -60.0 && jNear <= jRef, "loop_db_rel_dry before=" + fmt(before) + " 50-150ms_after=" + fmt(after) + " limit<-60; max_step near/before=" + fmt(jNear, 5) + "/" + fmt(jRef, 5));
    }
    {
        yanki::Controls c = yanki::defaultControls(); c.rise = 0.5; c.capOn = true; c.capDb = 3.0; c.endMode = yanki::kEndDump;
        const auto out = render(S.dry, sr, c, t0); fin = fin && finiteBounded(out);
        const auto y = loopPart(out, S.dry);
        const size_t T0 = static_cast<size_t>(t0 * sr), j0 = static_cast<size_t>(0.03 * sr), j1 = static_cast<size_t>(0.2 * sr);
        double sxy = 0, sxx = 0, syy = 0;
        for (size_t j = j0; j < j1; ++j) for (int ch = 0; ch < 2; ++ch) { const double a = y[2 * (T0 + j) + ch], b = y[2 * (T0 - 1 - j) + ch]; sxy += a * b; sxx += a * a; syy += b * b; }
        const double corr = sxy / std::sqrt(std::max(1e-30, sxx * syy));
        const double tail = db(spanRmsSt(y, sr, t0 + 2.9, t0 + 3.0) / ref);
        const double jNear = maxJump(y, sr, t0 - 0.05, t0 + 0.1), jRef = maxJump(y, sr, t0 - 1.0, t0 - 0.05);
        check("echo_end_dump", corr > 0.5 && tail < -50.0 && jNear <= jRef, "corr_with_time_reversed_loop(30-200ms; dump length = 1 head = 250ms)=" + fmt(corr, 3) + " limit>0.5; loop_db 2.9-3.0s_after=" + fmt(tail) + " limit<-50; max_step near/before=" + fmt(jNear, 5) + "/" + fmt(jRef, 5));
    }
    // every swell control changes ECHO's output; MIX 0 stays bit-exact dry
    {
        const yanki::Controls base = yanki::defaultControls(); const auto a = render(S.dry, sr, base);
        bool ok = true, byp = true; std::string bad;
        for (int k = 0; k < 6; ++k) {
            yanki::Controls q = base; double endAt = -1;
            if (k == 0) q.rise = 0.3; if (k == 1) { q.capOn = true; q.capDb = -12; q.rise = 0.3; } if (k == 2) { q.darken = 0.6; q.rise = 0.2; }
            if (k == 3) { q.rise = 0.3; q.endMode = yanki::kEndCut; endAt = 4.0; } if (k == 4) { q.rise = 0.3; q.endMode = yanki::kEndDump; endAt = 4.0; } if (k == 5) { q.lockBars = 3; q.capOn = true; }
            const auto b = render(S.dry, sr, q, endAt); fin = fin && finiteBounded(b);
            double d = 0; for (size_t i = 0; i < a.size(); ++i) d += (double(a[i]) - b[i]) * (double(a[i]) - b[i]);
            const double rel = db(std::sqrt(d / a.size()) / ref); if (!(rel > -60.0)) { ok = false; bad += " k" + std::to_string(k) + "=" + fmt(rel); }
            yanki::Controls z = q; z.mix = 0.0; const auto zz = render(S.dry, sr, z, endAt);
            // clean-path MIX law: dry = min(1, 2(1-MIX)) = 1 at MIX 0, wet = 0
            byp = byp && zz == S.dry;
        }
        check("echo_swell_controls_audible", ok, ok ? "rise cap darken end_cut end_dump lock all changed (>-60 dB)" : "unchanged:" + bad);
        check("echo_swell_mix0_dry", byp, "bit-exact");
    }
    // one FEEDBACK knob: mapping, and 0.7 = the hold point
    {
        std::string row; bool map = true;
        for (int m = 0; m <= 6; ++m) {
            yanki::Controls c = yanki::moodControls(m); const double hold = yanki::YankiEngine::holdFeedback(c);
            yanki::Controls a = c, b = c, h = c, d = c;
            yanki::YankiEngine::feedbackKnob(a, 0.0); yanki::YankiEngine::feedbackKnob(b, 0.35); yanki::YankiEngine::feedbackKnob(h, 0.7); yanki::YankiEngine::feedbackKnob(d, 1.0);
            map = map && a.feedback == 0.0 && a.rise < 0.0 && std::abs(b.feedback - 0.5 * hold) < 1e-12 && b.rise < 0.0 && h.rise == 0.0 && h.feedback == hold && d.rise == 1.0;
            row += std::string(" ") + yanki::kMoodNames[m] + "=" + fmt(hold, 3);
        }
        check("feedback_knob_mapping", map, "knob 0->fb 0, 0.35->fb hold/2 (RISE off), 0.7->RISE 0 at the hold point, 1->RISE 1; hold FEEDBACK per mood:" + row);
        const auto holdRun = [&](int mood, double scale, double& a, double& b) {
            std::vector<float> x = S.dry; for (float& v : x) v = static_cast<float>(v * scale);
            yanki::Controls c = yanki::moodControls(mood); yanki::YankiEngine::feedbackKnob(c, 0.7);
            const auto y = loopPart(render(x, sr, c), x);
            a = db(spanRmsSt(y, sr, 3.5, 4.5) / (ref * scale)); b = db(spanRmsSt(y, sr, 7.5, 8.5) / (ref * scale));
        };
        double a, b; holdRun(0, 0.1, a, b);
        check("feedback_knob_0.7_holds_echo", std::abs(b - a) <= 1.0, "ECHO knob 0.7, phrase at -20 dB (small signal): loop_db_rel_dry 3.5-4.5s=" + fmt(a) + " 7.5-8.5s=" + fmt(b) + " change=" + fmt(b - a) + " limit +-1 over 4 s");
        std::string info;
        for (int m : {0, 5, 6, 3}) for (double sc : {1.0, 0.1}) { holdRun(m, sc, a, b); if (m == 0 && sc == 0.1) continue; info += std::string(" ") + yanki::kMoodNames[m] + (sc == 1.0 ? "@full" : "@-20dB") + "=" + fmt(b - a); }
        std::cout << "INFO feedback_knob_0.7_change_over_4s_db" << info << " (full level: tape saturation pulls a loud loop down; classic moods: the model hold point is approximate)\n";
    }
    // meters + swelling()
    {
        yanki::YankiEngine e; e.prepare(sr); const yanki::Controls c = yanki::defaultControls();
        std::vector<float> l(S.dry.size() / 2), r(l.size()); for (size_t i = 0; i < l.size(); ++i) { l[i] = S.dry[2 * i]; r[i] = S.dry[2 * i + 1]; }
        double worst = 0; int blocks = 0;
        for (size_t p = 0; p + 512 <= l.size() && p < static_cast<size_t>(4.0 * sr); p += 512) {
            std::vector<float> il(l.begin() + static_cast<long>(p), l.begin() + static_cast<long>(p + 512)), ir(r.begin() + static_cast<long>(p), r.begin() + static_cast<long>(p + 512));
            e.process(l.data() + p, r.data() + p, 512, c);
            double sd = 0, sl = 0; for (int i = 0; i < 512; ++i) { sd += 0.5 * (double(il[i]) * il[i] + double(ir[i]) * ir[i]); const double dl = double(l[p + i]) - il[i], dr = double(r[p + i]) - ir[i]; sl += 0.5 * (dl * dl + dr * dr); }
            const double dDb = 10.0 * std::log10(std::max(sd / 512.0, 1e-24)), lDb = 10.0 * std::log10(std::max(sl / 512.0, 1e-24));
            if (sd > 1e-12 && sl > 1e-12) { worst = std::max(worst, std::max(std::abs(dDb - e.dryLevelDb()), std::abs(lDb - e.loopLevelDb()))); ++blocks; }
        }
        check("meters_dry_loop", blocks > 100 && worst < 0.05, "ECHO 512-sample blocks=" + std::to_string(blocks) + " max |getter - measured| dB=" + fmt(worst, 4) + " limit 0.05 (loop = output minus the dry part)");
        const auto sw = [&](yanki::Controls q) { yanki::YankiEngine z; z.prepare(sr); std::vector<float> a(512, 0.0f), b(512, 0.0f); z.process(a.data(), b.data(), 512, q); return z.swelling(); };
        yanki::Controls k85 = yanki::defaultControls(); yanki::YankiEngine::feedbackKnob(k85, 0.85);
        yanki::Controls k50 = yanki::defaultControls(); yanki::YankiEngine::feedbackKnob(k50, 0.5);
        const bool s0 = sw(yanki::defaultControls()), s1 = sw(k85), s2 = sw(k50), s3 = sw(yanki::moodControls(3)), s4 = sw(yanki::moodControls(1)), s5 = sw(yanki::moodControls(5));
        check("swelling_flag", !s0 && s1 && !s2 && s3 && !s4 && !s5, std::string("ECHO default=") + (s0 ? "1" : "0") + " ECHO knob0.85=" + (s1 ? "1" : "0") + " ECHO knob0.5=" + (s2 ? "1" : "0")
              + " DubSpiral=" + (s3 ? "1" : "0") + " Slapback=" + (s4 ? "1" : "0") + " Yesilcam=" + (s5 ? "1" : "0") + " want 0 1 0 1 0 0");
    }
    check("clean_swell_finite_bounded", fin, "all clean-path swell renders finite, |x|<2");
    return all;
}

}  // namespace swelltest

