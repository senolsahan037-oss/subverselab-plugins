// GENERATE checks for `svl --selftest` and the variation report `svl gen-stats` (docs/GENERATE_PLAN.md §8).
// Included by tools/svl.cpp after Report is defined.
#pragma once
#include "../Source/engine/SvlGen.h"
#include <chrono>
#include <map>
#include <set>

namespace gencheck
{
using namespace svl::gen;
static const Role kSeqRoles[] = { Role::melody, Role::bass, Role::chords, Role::arpRhythm };
static const std::array<Drum, svl::kDrumTracks> kDefaultKit { { Drum::kick, Drum::snare, Drum::clap, Drum::hat, Drum::openHat, Drum::tom, Drum::perc, Drum::shaker } };

static std::string sig (const SeqPattern& p)
{
    std::string s;
    for (int i = 0; i < p.length; ++i)
    {
        const auto& x = p.s[(size_t) i];
        s += x.on ? std::to_string (x.note) + "/" + std::to_string (x.row) + "/" + std::to_string (x.chord[0]) + "." + std::to_string (x.chord[1]) + "." + std::to_string (x.chord[2]) + "," : "-,";
    }
    return s;
}
static std::string rhythmSig (const SeqPattern& p) { std::string s; for (int i = 0; i < p.length; ++i) s += p.s[(size_t) i].on ? 'x' : '.'; return s; }
static std::string sig (const DrumPattern& p) { std::string s; for (auto& t : p.s) { for (int v : t) s += (char) ('0' + v); s += '|'; } return s; }

static bool onScale (const Scale& sc, int key) { const int r = ((key % sc.period) + sc.period) % sc.period; return std::find (sc.deg.begin(), sc.deg.end(), r) != sc.deg.end(); }

// a 7-degree "Lab" tuning with a 7-key period (linear mapping: consecutive keys are consecutive degrees)
static Scale labLike() { Scale s; s.period = 7; s.deg = { 0, 1, 2, 3, 4, 5, 6 }; s.id = 0; return s; }

template <typename R>
static void selftest (R& Rp)
{
    Rp.line ("\n[GENERATE] seeded pattern writer (Source/engine/SvlGen.h)");
    Settings st;
    // 1. determinism
    {
        bool same = true;
        for (auto role : kSeqRoles) for (int s = 0; s < 1000; ++s) same &= makeSeq (role, {}, s, (s * 7919) % 10000, st) == makeSeq (role, {}, s, (s * 7919) % 10000, st);
        for (int s = 0; s < 1000; ++s) same &= makeDrums (kDefaultKit, s, st) == makeDrums (kDefaultKit, s, st);
        Rp.check (same, "the same seeds and settings give the same steps (4 seq roles + drums x 1000 seeds)");
    }
    // 2. NORMAL never writes a raised / lowered step; MICRO does
    {
        Settings n = st; n.micro = 1.0f; n.microAllowed = false;
        Settings m = n; m.microAllowed = true;
        int rowsNormal = 0, seedsWithMicro = 0;
        for (auto role : { Role::melody, Role::bass })
            for (int s = 0; s < 1000; ++s)
            {
                const auto a = makeSeq (role, {}, s, 9999 - s, n), b = makeSeq (role, {}, s, 9999 - s, m);
                bool any = false;
                for (int i = 0; i < 16; ++i) { rowsNormal += a.s[(size_t) i].on && a.s[(size_t) i].row != 1; any |= b.s[(size_t) i].on && b.s[(size_t) i].row != 1; }
                seedsWithMicro += any;
            }
        char b[200]; std::snprintf (b, sizeof b, "NORMAL: 0 micro steps in 2000 melody / bass rolls at MICRO 1 (found %d)", rowsNormal); Rp.check (rowsNormal == 0, b);
        std::snprintf (b, sizeof b, "MICRO at 1: micro steps in %d of 2000 rolls (want > 1000)", seedsWithMicro); Rp.check (seedsWithMicro > 1000, b);
    }
    // 3. every note and chord note on the scale (8 scales at 12 keys, and a 7-key Lab tuning)
    {
        int off = 0, total = 0;
        for (int sid = 1; sid <= kNumScales; ++sid)
            for (auto role : { Role::melody, Role::bass, Role::chords })
                for (int s = 0; s < 200; ++s)
                {
                    const Scale sc = scale12 (sid);
                    const auto p = makeSeq (role, sc, s, s + 17, st);
                    for (int i = 0; i < 16; ++i)
                    {
                        const auto& x = p.s[(size_t) i]; if (! x.on) continue;
                        ++total; off += ! onScale (sc, x.note);
                        for (int c : x.chord) if (c > 0) { ++total; off += ! onScale (sc, x.note + c); }
                    }
                }
        int offLab = 0, totLab = 0;
        for (auto role : { Role::melody, Role::bass, Role::chords })
            for (int s = 0; s < 500; ++s)
            {
                const auto p = makeSeq (role, labLike(), s, s * 3 + 1, st);
                for (int i = 0; i < 16; ++i) if (p.s[(size_t) i].on) { ++totLab; offLab += ! onScale (labLike(), p.s[(size_t) i].note); }
            }
        char b[200]; std::snprintf (b, sizeof b, "notes on the scale: %d of %d off (8 scales x 3 roles x 200 seeds); Lab 7-key tuning %d of %d off", off, total, offLab, totLab);
        Rp.check (off == 0 && offLab == 0 && total > 10000, b);
    }
    // 4. DENSITY only adds steps of the same pattern; the hit count follows it
    {
        bool nested = true, counts = true;
        for (auto role : kSeqRoles)
            for (int s = 0; s < 500; ++s)
            {
                Settings a = st, b = st, c = st; a.density = 0.2f; b.density = 0.5f; c.density = 0.9f;
                const auto pa = makeSeq (role, {}, s, 42, a), pb = makeSeq (role, {}, s, 42, b), pc = makeSeq (role, {}, s, 42, c);
                int na = 0, nb = 0, nc = 0;
                for (int i = 0; i < 16; ++i)
                {
                    const bool x = pa.s[(size_t) i].on, y = pb.s[(size_t) i].on, z = pc.s[(size_t) i].on;
                    if ((x && ! y) || (y && ! z)) nested = false;
                    na += x; nb += y; nc += z;
                }
                if (role != Role::chords && (na != hitsFor (role, 0.2f, 16) || nb != hitsFor (role, 0.5f, 16) || nc != hitsFor (role, 0.9f, 16))) counts = false;
            }
        bool dn = true;
        for (int s = 0; s < 500; ++s)
        {
            Settings a = st, b = st; a.density = 0.2f; b.density = 0.8f;
            const auto x = makeDrums (kDefaultKit, s, a), y = makeDrums (kDefaultKit, s, b);
            for (int t = 0; t < 8; ++t) for (int i = 0; i < 16; ++i) if (x.s[(size_t) t][(size_t) i] && ! y.s[(size_t) t][(size_t) i]) dn = false;
        }
        Rp.check (nested && counts && dn, "DENSITY 0.2 -> 0.5 -> 0.9: every step of the lower density stays on (seq 4 roles x 500, drums x 500); hit counts = hitsFor");
    }
    // 5. ranges, repetition inside a pattern
    {
        int outRange = 0, triple = 0, fewPitches = 0, halvesEqual = 0, n = 0, overUsed = 0;
        for (int s = 0; s < 2000; ++s)
        {
            Settings r = st; r.range = (float) (5 + s % 20);
            const auto p = makeSeq (Role::melody, {}, s, (s * 31) % 10000, r);
            std::vector<int> notes; for (int i = 0; i < 16; ++i) if (p.s[(size_t) i].on) { notes.push_back (p.s[(size_t) i].note); outRange += p.s[(size_t) i].note < -3 || p.s[(size_t) i].note > (int) r.range; }
            for (size_t i = 2; i < notes.size(); ++i) triple += notes[i] == notes[i - 1] && notes[i] == notes[i - 2];
            std::set<int> u (notes.begin(), notes.end()); fewPitches += notes.size() >= 3 && u.size() < 3;
            { std::map<int, int> c; for (int x : notes) c[x]++; int mx = 0; for (auto& [k2, v2] : c) mx = std::max (mx, v2);
              overUsed += notes.size() >= 5 && mx > std::max (2, ((int) notes.size() * 2 + 4) / 5); }
            bool eq = true; for (int i = 0; i < 8 && eq; ++i) eq = p.s[(size_t) i].on == p.s[(size_t) i + 8].on && (! p.s[(size_t) i].on || p.s[(size_t) i].note == p.s[(size_t) i + 8].note);
            halvesEqual += eq; ++n;
        }
        int bassOut = 0;
        for (int s = 0; s < 2000; ++s) { const auto p = makeSeq (Role::bass, {}, s, s + 5, st); for (int i = 0; i < 16; ++i) if (p.s[(size_t) i].on) bassOut += p.s[(size_t) i].note < -2 || p.s[(size_t) i].note > 14; }
        char b[300]; std::snprintf (b, sizeof b, "melody x %d (RANGE 5..24): %d notes out of range, %d three-in-a-row, %d with < 3 pitches, %d with equal halves, %d with one pitch > 40 %%; bass: %d out of -2..14",
                                    n, outRange, triple, fewPitches, halvesEqual, overUsed, bassOut);
        Rp.check (outRange == 0 && triple == 0 && fewPitches == 0 && halvesEqual == 0 && overUsed == 0 && bassOut == 0, b);
    }
    // 6. chords: 3 or 4 notes, voice leading
    {
        int bad = 0, moves = 0; long moveSum = 0; int chordsSeen = 0;
        for (int s = 0; s < 1000; ++s)
        {
            const auto p = makeSeq (Role::chords, {}, s, (s * 13) % 10000, st);
            std::vector<int> prev;
            for (int i = 0; i < 16; ++i)
            {
                const auto& x = p.s[(size_t) i]; if (! x.on) continue;
                std::vector<int> v { x.note }; for (int c : x.chord) if (c > 0) v.push_back (x.note + c);
                ++chordsSeen; bad += v.size() < 3 || v.size() > 4;
                if (! prev.empty() && v != prev && v.size() == prev.size()) { for (size_t k = 0; k < v.size(); ++k) moveSum += std::abs (v[k] - prev[k]); moves += (int) v.size(); }
                prev = v;
            }
        }
        char b[200]; std::snprintf (b, sizeof b, "chords: %d of %d steps not 3-4 notes; mean voice move between chords %.2f keys (want <= 3)", bad, chordsSeen, moves ? (double) moveSum / moves : 0.0);
        Rp.check (bad == 0 && moves > 0 && (double) moveSum / moves <= 3.0, b);
    }
    // 7. drums: the kick on step 1 always; accents only on kick / snare
    {
        int noKick = 0;
        for (int s = 0; s < 2000; ++s) { Settings d = st; d.density = (float) (s % 11) / 10.0f; noKick += makeDrums (kDefaultKit, s, d).s[0][0] == 0; }
        char b[160]; std::snprintf (b, sizeof b, "drums: the kick on step 1 in every beat (missing in %d of 2000, density 0..1)", noKick); Rp.check (noKick == 0, b);
    }
    // 8. MUTATE
    {
        bool ends = true; double frac = 0; int n = 0;
        for (int s = 0; s < 500; ++s)
        {
            const auto a = makeSeq (Role::melody, {}, s, 1, st), b = makeSeq (Role::melody, {}, s + 5000, 2, st);
            ends &= mutate (a, b, 0.0f, s) == a && mutate (a, b, 1.0f, s) == b;
            const auto m = mutate (a, b, 0.2f, s); int ch = 0; for (int i = 0; i < 16; ++i) ch += ! (m.s[(size_t) i] == a.s[(size_t) i]) || ! (m.s[(size_t) i] == b.s[(size_t) i]) ? (m.s[(size_t) i] == b.s[(size_t) i] && ! (a.s[(size_t) i] == b.s[(size_t) i])) : 0;
            int differ = 0; for (int i = 0; i < 16; ++i) differ += ! (a.s[(size_t) i] == b.s[(size_t) i]);
            if (differ > 0) { frac += (double) ch / differ; ++n; }
        }
        char b[160]; std::snprintf (b, sizeof b, "MUTATE 0 keeps, 1 replaces; 0.2 takes %.0f %% of the differing steps from the new roll (want 10..30)", 100.0 * frac / std::max (1, n));
        Rp.check (ends && frac / n > 0.1 && frac / n < 0.3, b);
    }
    // 9. repetition between rolls: 1000 rolls per role through the filter, each at least kMinDistance from the 16 before
    {
        bool ok = true; int skippedAll = 0; float worst = 1.0f;
        for (auto role : kSeqRoles)
        {
            Rng src (role == Role::melody ? 11 : 12 + (int) role);
            std::vector<SeqPattern> hist;
            for (int roll = 0; roll < 1000; ++roll)
            {
                int seed = 0; SeqPattern p; const int fixedN = (int) src.below (10000);
                skippedAll += pickNovel (hist, [&] (int r) { return makeSeq (role, {}, r, (r * 7 + fixedN) % 10000, st); }, [&] { return src.below (10000); }, seed, p);
                float d = 1.0f; for (auto& h : hist) d = std::min (d, distance (p, h));
                if (! hist.empty()) worst = std::min (worst, d);
                ok &= hist.empty() || d >= kMinDistance;
                hist.push_back (p); if ((int) hist.size() > kHistory) hist.erase (hist.begin());
            }
        }
        char b[200]; std::snprintf (b, sizeof b, "repetition filter: 4 roles x 1000 rolls, closest to the 16 before %.3f (want >= %.2f), %d seeds skipped", worst, kMinDistance, skippedAll);
        Rp.check (ok, b);
    }
    // 10. the sequencer plays a step's chord notes with its note
    {
        svl::ParamSet ps; ps[svl::SEQ_ON] = 1; ps[svl::SEQ_MODE] = 1; ps[svl::SEQ_LEN] = 1;
        ps[svl::stepParam (0, svl::S_ON)] = 1; ps[svl::stepParam (0, svl::S_NOTE)] = -5;
        ps[svl::stepChordParam (0, 0)] = 3; ps[svl::stepChordParam (0, 1)] = 7; ps[svl::stepChordParam (0, 2)] = 10;
        svl::NoteGen g; g.prepare (48000); g.setTempo (120); g.setParams (ps); g.setTransport (false, false, 0);
        std::vector<int> keys; svl::GenNote n;
        for (int guard = 0; guard < 10 && keys.size() < 4; ++guard) { while (g.pop (n)) if (n.kind != svl::GenNote::off) keys.push_back (n.key); g.advance (std::max (1, g.samplesToNext())); }
        std::sort (keys.begin(), keys.end());
        Rp.check (keys == std::vector<int> ({ 55, 58, 62, 65 }), "a step with chord notes +3 +7 +10 on G3 plays G3 Bb3 D4 F4 together");
    }
    // 11. speed
    {
        const auto t0 = std::chrono::steady_clock::now(); int n = 0;
        for (int s = 0; s < 2000; ++s) for (auto role : kSeqRoles) { volatile auto p = makeSeq (role, {}, s, s, st).s[0].note; (void) p; ++n; }
        const double us = std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - t0).count() / n;
        char b[120]; std::snprintf (b, sizeof b, "one roll takes %.1f us on average (want < 1000)", us); Rp.check (us < 1000.0, b);
    }
}

// `svl gen-stats`: how much variation, in numbers
static int stats()
{
    Settings st;
    std::printf ("GENERATE variation report (default settings: density %.1f, complexity %.1f, range %.0f keys)\n", st.density, st.complexity, st.range);
    for (auto role : kSeqRoles)
    {
        Rng src (777 + (int) role);
        std::set<std::string> all, rh; std::vector<SeqPattern> ps; std::map<int, int> scales;
        for (int i = 0; i < 1000; ++i) { const auto p = makeSeq (role, {}, src.below (10000), src.below (10000), st); all.insert (sig (p)); rh.insert (rhythmSig (p)); ps.push_back (p); scales[p.scaleId]++; }
        std::vector<float> d; for (int i = 0; i < 2000; ++i) d.push_back (distance (ps[(size_t) src.below (1000)], ps[(size_t) src.below (1000)]));
        std::sort (d.begin(), d.end());
        // a session through the repetition filter
        std::vector<SeqPattern> hist; int skipped = 0; std::vector<float> dprev;
        for (int roll = 0; roll < 1000; ++roll)
        {
            int seed = 0; SeqPattern p; const int fixedN = src.below (10000);
            skipped += pickNovel (hist, [&] (int r) { return makeSeq (role, {}, r, (r * 7 + fixedN) % 10000, st); }, [&] { return src.below (10000); }, seed, p);
            if (! hist.empty()) dprev.push_back (distance (p, hist.back()));
            hist.push_back (p); if ((int) hist.size() > kHistory) hist.erase (hist.begin());
        }
        std::sort (dprev.begin(), dprev.end());
        std::printf ("\n%s: 1000 random rolls -> %zu different patterns, %zu different rhythms\n", kRoleNames[(int) role], all.size(), rh.size());
        std::printf ("  distance between two random rolls: median %.2f, 5th percentile %.2f, smallest %.2f (2000 pairs)\n", d[1000], d[100], d[0]);
        std::printf ("  a 1000-roll session through the filter: distance to the roll before median %.2f, smallest %.2f; seeds skipped %d\n", dprev[dprev.size() / 2], dprev.front(), skipped);
        if (role != Role::arpRhythm) { std::printf ("  scales:"); for (auto& [k, v] : scales) std::printf (" %s %d", kScaleNames[k], v); std::printf ("\n"); }
    }
    {
        Rng src (999);
        std::set<std::string> all; std::vector<DrumPattern> ps; std::map<int, int> styles;
        for (int i = 0; i < 1000; ++i) { const auto p = makeDrums (kDefaultKit, src.below (10000), st); all.insert (sig (p)); ps.push_back (p); styles[p.style]++; }
        std::vector<float> d; for (int i = 0; i < 2000; ++i) d.push_back (distance (ps[(size_t) src.below (1000)], ps[(size_t) src.below (1000)]));
        std::sort (d.begin(), d.end());
        std::printf ("\nBEAT: 1000 random rolls -> %zu different beats; distance median %.2f, 5th percentile %.2f, smallest %.2f\n  styles:", all.size(), d[1000], d[100], d[0]);
        for (auto& [k, v] : styles) std::printf (" %s %d", kDrumStyleNames[k], v);
        std::printf ("\n");
    }
    return 0;
}
} // namespace gencheck
