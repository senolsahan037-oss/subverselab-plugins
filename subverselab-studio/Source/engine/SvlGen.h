// JING FANG GENERATE (docs/GENERATE_PLAN.md §8): rule-based, seeded pattern writer for the sequencer and the drum machine.
// Plain C++17, no JUCE; runs on the message thread (never in the audio callback). It writes what the units already play:
// sequencer steps (on, note, row, vel, gate, slide, accent, prob, ratchet, chord notes) and drum steps (off / on / accent).
//
// Seeds. Two per roll, 0..9999 each: RHYTHM (which steps play, lengths, accents, ratchets, drum hits) and NOTES (scale, phrase
// form, motif, pitches, progression, micro pitch classes). A pattern is a function of (role, seeds, settings, scale): the same
// inputs always give the same steps. Steps are ranked once per RHYTHM seed and DENSITY takes the top k, so turning DENSITY
// only adds or removes notes of the same pattern.
//
// Repetition. distance() compares two patterns (0 = equal, 1 = nothing shared); the plug-in skips a seed whose pattern is
// closer than kMinDistance to one of the last rolls. Inside a pattern: no three equal notes in a row, at least three
// different pitches, the second half never a copy of the first.
#pragma once
#include "SvlParams.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace svl::gen
{
enum class Role : int { melody, bass, chords, arpRhythm, beat, count };
inline const char* const kRoleNames[] = { "MELODY", "BASS", "CHORDS", "ARP RHYTHM", "BEAT" };

// gen.scale: 0 = from the NOTES seed
inline const char* const kScaleNames[] = { "auto", "minor", "dorian", "phrygian", "major", "mixolydian", "harmonic minor", "minor pentatonic", "major pentatonic" };
constexpr int kNumScales = 8;
inline const std::vector<int>& scaleDegrees (int s)   // s = 1..kNumScales
{
    static const std::vector<int> t[kNumScales] = { { 0, 2, 3, 5, 7, 8, 10 }, { 0, 2, 3, 5, 7, 9, 10 }, { 0, 1, 3, 5, 7, 8, 10 }, { 0, 2, 4, 5, 7, 9, 11 },
                                                    { 0, 2, 4, 5, 7, 9, 10 }, { 0, 2, 3, 5, 7, 8, 11 }, { 0, 3, 5, 7, 10 }, { 0, 2, 4, 7, 9 } };
    return t[std::max (1, std::min (kNumScales, s)) - 1];
}

struct Scale
{
    int period = 12;          // keys per period (12; a Lab tuning: its own size)
    std::vector<int> deg;     // the keys inside one period that are degrees, ascending, deg[0] = 0
    int id = 0;               // 1..kNumScales, 0 = the Lab's
    int size() const { return (int) deg.size(); }
    // key of degree index d (any integer; negative = below the root)
    int key (int d) const
    {
        const int n = size(); if (n == 0) return d;
        int q = d / n, r = d % n; if (r < 0) { r += n; --q; }
        return q * period + deg[(size_t) r];
    }
    // nearest degree index of a key (ties go down)
    int degreeOf (int k) const
    {
        int best = 0, bd = 1 << 30;
        const int n = size();
        for (int d = -4 * n; d <= 4 * n; ++d) { const int dd = std::abs (key (d) - k); if (dd < bd) { bd = dd; best = d; } }
        return best;
    }
};
inline Scale scale12 (int s) { Scale sc; sc.period = 12; sc.deg = scaleDegrees (s); sc.id = s; return sc; }

struct Settings
{
    float density = 0.5f, complexity = 0.4f, range = 12, micro = 0;   // range: keys the line may span
    bool microAllowed = false;     // only with the keyboard on MICRO (NORMAL never writes a raised / lowered step)
    int scale = 0;                 // gen.scale; ignored when the Lab gives the scale
    int length = 16;
};

struct Step
{
    bool on = false; int note = 0, row = 1; float vel = 0.8f, gate = 0.6f; bool slide = false, accent = false;
    float prob = 1; int ratchet = 1; std::array<int, 3> chord { { 0, 0, 0 } };
    bool operator== (const Step& o) const
    { return on == o.on && note == o.note && row == o.row && vel == o.vel && gate == o.gate && slide == o.slide && accent == o.accent
             && prob == o.prob && ratchet == o.ratchet && chord == o.chord; }
};
struct SeqPattern { int length = 16; std::array<Step, kSeqSteps> s {}; int scaleId = 0; bool operator== (const SeqPattern& o) const { return length == o.length && s == o.s; } };
struct DrumPattern { int length = 16; std::array<std::array<int, kDrumSteps>, kDrumTracks> s {}; int style = 0; bool operator== (const DrumPattern& o) const { return length == o.length && s == o.s; } };

enum class Drum : int { kick, snare, clap, hat, openHat, tom, perc, shaker, count };
inline const char* const kDrumStyleNames[] = { "four on the floor", "backbeat", "breakbeat", "half time", "three-three-two", "sparse" };
constexpr int kNumDrumStyles = 6;

constexpr float kMinDistance = 0.25f;
constexpr int kHistory = 16, kMaxTries = 256;   // a roll costs a few microseconds, 256 tries < 1 ms

// ---------------------------------------------------------------- random
struct Rng
{
    uint64_t s;
    explicit Rng (uint64_t seed) : s (seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull) {}
    uint64_t next() { uint64_t z = (s += 0x9E3779B97F4A7C15ull); z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; return z ^ (z >> 31); }
    double uni() { return (double) (next() >> 11) * (1.0 / 9007199254740992.0); }
    int below (int n) { return n <= 1 ? 0 : (int) (next() % (uint64_t) n); }
    bool chance (double p) { return uni() < p; }
    template <size_t N> int pick (const std::array<float, N>& w) { double t = 0; for (float x : w) t += x; double u = uni() * t; for (size_t i = 0; i < N; ++i) { u -= w[i]; if (u < 0) return (int) i; } return (int) N - 1; }
};
inline uint64_t seedFor (int seed, uint64_t salt) { return (uint64_t) (uint32_t) seed * 0x100000001B3ull ^ (salt * 0xD6E8FEB86659FD93ull); }
inline uint64_t hash2 (uint64_t a, uint64_t b) { Rng r (a ^ (b * 0xA0761D6478BD642Full)); return r.next(); }

// ---------------------------------------------------------------- rhythm
// metric weight of a step in a 16-step bar (1 = the downbeat)
inline float metric (int p) { p &= 15; return p == 0 ? 1.0f : (p == 8 ? 0.85f : (p % 4 == 0 ? 0.65f : (p % 2 == 0 ? 0.4f : 0.2f))); }

// four rhythm feels per role, picked by the RHYTHM seed; weights per step on top of the metric
inline float feel (Role role, int v, int p)
{
    p &= 15;
    auto in = [p] (std::initializer_list<int> l) { for (int x : l) if (x == p) return 1.0f; return 0.0f; };
    switch (role)
    {
        case Role::bass:
            if (v == 1) return 0.6f * in ({ 2, 6, 10, 14 });                  // off-beat
            if (v == 2) return 0.7f * in ({ 0, 3, 6, 8, 11, 14 });            // three-three-two
            if (v == 3) return 0.6f * in ({ 0, 3, 7, 10, 12 });               // syncopated
            if (v == 4) return 0.6f * in ({ 0, 1, 4, 8, 9, 12 });             // doubled hits
            if (v == 5) return 0.6f * in ({ 0, 6, 10, 13 });                  // sparse, late
            return 0.4f * in ({ 0, 4, 8, 12 });                               // on the beat
        case Role::chords:
            if (v == 1) return 0.7f * in ({ 0, 7, 15, 10 });                  // pushed
            if (v == 2) return 0.8f * in ({ 0, 8 });                          // halves
            if (v == 3) return 0.6f * in ({ 2, 6, 10, 14 });                  // off-beat stabs
            if (v == 4) return 0.6f * in ({ 0, 3, 6, 8, 11, 14 });            // three-three-two stabs
            if (v == 5) return 0.6f * in ({ 0, 2, 8, 10, 12 });              // short-long
            return 0.5f * in ({ 0, 4, 8, 12 });
        case Role::melody:
            if (v == 1) return 0.5f * in ({ 0, 3, 6, 10, 13 });               // syncopated
            if (v == 2) return p < 8 ? 0.35f : -0.15f;                        // call (busy) and answer (sparse)
            if (v == 3) return 0.45f * in ({ 2, 6, 10, 14, 7, 15 });          // off-beat
            if (v == 4) return 0.45f * in ({ 0, 1, 2, 8, 9, 10 });            // bursts
            if (v == 5) return p >= 8 ? 0.35f : -0.15f;                       // late answer
            return 0.3f * in ({ 0, 2, 4, 8, 12 });
        default:
            if (v == 1) return 0.5f * in ({ 0, 3, 6, 8, 11, 14 });
            if (v == 2) return 0.4f * (p % 2 == 0 ? 1.0f : 0.0f);
            if (v == 3) return 0.5f * in ({ 0, 2, 3, 6, 8, 10, 11, 14 });
            if (v == 4) return 0.5f * in ({ 0, 4, 5, 8, 12, 13 });
            if (v == 5) return 0.5f * (p % 3 == 0 ? 1.0f : 0.0f);
            return 0.2f;
    }
}

// the steps that play: every step ranked once by (metric + feel + noise); DENSITY takes the top k
inline std::vector<int> rankSteps (Role role, int rseed, float complexity, int len, int& feelOut, std::vector<int> forced = {})
{
    Rng r (seedFor (rseed, 0x5254 + (uint64_t) role));
    const int v = r.below (6);
    feelOut = v;
    std::vector<std::pair<float, int>> sc;
    for (int p = 0; p < len; ++p)
    {
        const float noise = (float) r.uni();
        float s = (0.85f - 0.45f * complexity) * (metric (p) + feel (role, v, p)) + (0.45f + 0.55f * complexity) * noise;
        for (int f : forced) if (f == p) s += 10.0f;
        sc.push_back ({ s, p });
    }
    std::stable_sort (sc.begin(), sc.end(), [] (auto& a, auto& b) { return a.first > b.first; });
    std::vector<int> order; for (auto& x : sc) order.push_back (x.second);
    return order;
}

inline int hitsFor (Role role, float density, int len)
{
    const float d = std::max (0.0f, std::min (1.0f, density));
    float lo = 3, hi = (float) len * 0.85f;
    if (role == Role::bass) { lo = 2; hi = (float) len * 0.9f; }
    if (role == Role::chords) { lo = 1; hi = (float) len * 0.6f; }
    if (role == Role::arpRhythm) { lo = 3; hi = (float) len; }
    return std::max (1, std::min (len, (int) std::lround (lo + (hi - lo) * d)));
}

// ---------------------------------------------------------------- notes
inline int pickScale (const Settings& st, int nseed)
{
    if (st.scale >= 1 && st.scale <= kNumScales) return st.scale;
    Rng r (seedFor (nseed, 0x5343));
    const std::array<float, kNumScales> w { { 25, 15, 8, 18, 8, 8, 12, 6 } };
    return 1 + r.pick (w);
}

// a progression of scale-degree roots (0-based), 2 or 4 chords; minor-ish scales and major-ish scales have their own tables
inline std::vector<int> progression (const Scale& sc, int nseed, float density)
{
    Rng r (seedFor (nseed, 0x5052));
    const bool minorish = sc.size() >= 3 && (sc.deg[2] == 3 || (sc.size() == 5 && sc.deg[1] == 3));
    static const std::vector<std::vector<int>> mi = { { 0, 5, 2, 6 }, { 0, 3, 4, 0 }, { 0, 5, 3, 4 }, { 0, 6, 5, 6 }, { 0, 3, 6, 2 }, { 0, 2, 3, 4 },
                                                      { 0, 6, 5, 4 }, { 0, 3, 5, 4 }, { 5, 6, 0, 0 }, { 0, 4, 5, 3 }, { 3, 4, 0, 5 }, { 0, 2, 5, 6 } };
    static const std::vector<std::vector<int>> ma = { { 0, 4, 5, 3 }, { 0, 5, 3, 4 }, { 1, 4, 0, 0 }, { 0, 3, 0, 4 }, { 0, 2, 3, 4 }, { 5, 3, 0, 4 },
                                                      { 0, 3, 5, 4 }, { 0, 5, 1, 4 }, { 3, 0, 4, 5 }, { 0, 2, 5, 3 }, { 0, 4, 1, 3 }, { 0, 6, 3, 0 } };
    static const std::vector<std::vector<int>> two = { { 0, 3 }, { 0, 4 }, { 0, 5 }, { 0, 6 }, { 0, 2 }, { 5, 0 } };
    if (density < 0.35f || r.chance (0.25)) { auto p = two[(size_t) r.below ((int) two.size())]; for (auto& x : p) x %= std::max (1, sc.size()); return p; }
    const auto& t = minorish ? mi : ma;
    auto p = t[(size_t) r.below ((int) t.size())];
    for (auto& x : p) x %= std::max (1, sc.size());
    return p;
}

// chord tones (keys) on a root degree: every other degree, 3 notes or 4 with COMPLEX
inline std::vector<int> chordTones (const Scale& sc, int rootDeg, bool seventh)
{
    std::vector<int> t { sc.key (rootDeg), sc.key (rootDeg + 2), sc.key (rootDeg + 4) };
    if (seventh) t.push_back (sc.key (rootDeg + 6));
    return t;
}

// the voicing nearest to the previous chord (lowest note within [lo, lo + period)): inversions x octave
inline std::vector<int> voiceNear (std::vector<int> tones, const std::vector<int>& prev, int period, int lo)
{
    std::sort (tones.begin(), tones.end());
    std::vector<int> best; long bestCost = 1L << 40;
    const int n = (int) tones.size();
    for (int inv = 0; inv < n; ++inv)
        for (int sh = -2; sh <= 2; ++sh)
        {
            std::vector<int> v;
            for (int i = 0; i < n; ++i) v.push_back (tones[(size_t) ((i + inv) % n)] + (i + inv >= n ? period : 0) + sh * period);
            std::sort (v.begin(), v.end());
            if (v.front() < lo || v.front() >= lo + period || v.back() - v.front() > 24) continue;
            long cost = 0;
            if (prev.empty()) cost = std::abs (v.front() - lo - 2) * 2 + inv * 3;
            else for (int x : v) { int d = 1 << 20; for (int y : prev) d = std::min (d, std::abs (x - y)); cost += d; }
            if (cost < bestCost) { bestCost = cost; best = v; }
        }
    if (best.empty()) best = tones;
    return best;
}

// ---------------------------------------------------------------- the sequencer
inline SeqPattern makeSeq (Role role, const Scale& scIn, int rseed, int nseed, const Settings& st)
{
    SeqPattern out; const int len = std::max (1, std::min (kSeqSteps, st.length)); out.length = len;
    const float cx = std::max (0.0f, std::min (1.0f, st.complexity));
    Scale sc = scIn;
    if (sc.deg.empty()) sc = scale12 (pickScale (st, nseed));
    out.scaleId = sc.id;
    const int P = sc.period, n = sc.size();
    Rng rr (seedFor (rseed, 0x5257)), nr (seedFor (nseed, 0x4E4F));

    // chord changes (also the bass's harmony): 2 or 4 equal parts, a pushed feel moves a change one step early
    const auto prog = progression (sc, nseed, st.density);
    int feelV = 0;
    std::vector<int> changes;
    for (size_t i = 0; i < prog.size(); ++i) changes.push_back ((int) (i * (size_t) len / prog.size()));
    std::vector<int> forced;
    if (role == Role::chords) forced = changes;
    if (role == Role::bass) forced = { 0 };
    const auto order = rankSteps (role, rseed, cx, len, feelV, forced);
    if (role == Role::chords && feelV == 1) for (size_t i = 1; i < changes.size(); ++i) changes[i] = std::max (changes[i - 1] + 1, changes[i] - 1);
    int k = hitsFor (role, st.density, len);
    if (role == Role::chords) k = std::max (k, (int) changes.size());
    std::vector<bool> on ((size_t) len, false);
    for (int i = 0; i < k; ++i) on[(size_t) order[(size_t) i]] = true;
    if (role == Role::chords) for (int c : changes) on[(size_t) c] = true;
    auto chordAt = [&] (int p) { int c = 0; for (size_t i = 0; i < changes.size(); ++i) if (p >= changes[i]) c = (int) i; return c; };

    // rhythm details from the RHYTHM seed
    for (int p = 0; p < len; ++p)
    {
        Step& s = out.s[(size_t) p];
        s.on = on[(size_t) p];
        const double u1 = rr.uni(), u2 = rr.uni(), u3 = rr.uni(), u4 = rr.uni();
        if (! s.on) { s.vel = 0.8f; s.gate = 0.6f; continue; }
        int d = 1; while (d < len && ! on[(size_t) ((p + d) % len)]) ++d;
        if (role == Role::chords)
        {
            int toChange = len - p; for (int c : changes) if (c > p) { toChange = c - p; break; }
            s.gate = (float) std::min (4.0, std::max (0.5, std::min (d, toChange) * (0.6 + 0.35 * (1.0 - cx * u1))));
        }
        else if (role == Role::melody) s.gate = (float) std::min (4.0, std::max (0.3, d * (0.55 + 0.45 * u1 - 0.25 * cx * u2)));
        else if (role == Role::bass) s.gate = (float) std::min (4.0, std::max (0.3, d * (0.45 + 0.4 * u1)));
        else s.gate = (float) (0.35 + 0.3 * u1);
        s.vel = std::min (1.0f, 0.62f + 0.25f * metric (p) + (float) (0.12 * u3 * (0.4 + cx)));
        s.accent = (role == Role::bass || role == Role::arpRhythm) && metric (p) >= 0.65f && u2 < 0.25 + 0.4 * cx;
        if ((role == Role::melody || role == Role::arpRhythm) && cx > 0.5f && metric (p) < 0.5f && u4 < (cx - 0.5) * 0.5) s.ratchet = u3 < 0.6 ? 2 : 3;
        if (cx > 0.6f && metric (p) < 0.45f && role != Role::chords && u2 > 0.65) s.prob = 0.6f + 0.3f * (float) u1;
        if (role == Role::bass && d == 1 && u4 < 0.12 + 0.45 * cx) s.slide = true;
        if (role == Role::melody && d == 1 && u4 < 0.12 * cx) s.slide = true;
    }

    // notes from the NOTES seed
    const int lo = role == Role::bass ? -2 : (role == Role::melody ? -3 : -7);
    const int span = (int) std::lround (std::max (5.0f, std::min (24.0f, st.range)));
    const int hi = role == Role::bass ? std::max (7, std::min (14, span)) : (role == Role::melody ? span : 16);
    // into [lo, hi] by octaves; a window narrower than an octave can leave no octave of the key inside: then the scale's key
    // inside the window nearest to it (RANGE 5 = a five-key line)
    auto fold = [&] (int key)
    {
        int k = key; while (k > hi) k -= P; while (k < lo) k += P;
        if (k <= hi) return k;
        int best = lo, bd = 1 << 20;
        for (int c = lo; c <= hi; ++c)
        {
            const int r = ((c % P) + P) % P;
            if (std::find (sc.deg.begin(), sc.deg.end(), r) == sc.deg.end()) continue;
            const int d = std::min (std::abs (c - key), std::abs (c - k));
            if (d < bd) { bd = d; best = c; }
        }
        return best;
    };
    std::vector<int> sounding;
    for (int p = 0; p < len; ++p) if (out.s[(size_t) p].on) sounding.push_back (p);

    if (role == Role::arpRhythm)
    {
        for (int p : sounding) out.s[(size_t) p].note = 0;
    }
    else if (role == Role::chords)
    {
        const bool sev = cx > 0.45f && n >= 7 && nr.chance (0.35 + 0.5 * cx);
        std::vector<std::vector<int>> voiced; std::vector<int> prev;
        for (size_t i = 0; i < prog.size(); ++i) { auto v = voiceNear (chordTones (sc, prog[i], sev), prev, P, -5); voiced.push_back (v); prev = v; }
        for (int p : sounding)
        {
            const auto& v = voiced[(size_t) chordAt (p)];
            Step& s = out.s[(size_t) p];
            s.note = v[0];
            for (size_t i = 1; i < v.size() && i <= 3; ++i) s.chord[i - 1] = v[i] - v[0];
        }
    }
    else if (role == Role::bass)
    {
        int last = 0;
        for (size_t i = 0; i < sounding.size(); ++i)
        {
            const int p = sounding[i];
            const int c = chordAt (p);
            const int root = sc.key (prog[(size_t) c]);
            int key = root;
            const bool strong = metric (p) >= 0.65f || p == changes[(size_t) c];
            const int nextP = i + 1 < sounding.size() ? sounding[i + 1] : len;
            const bool beforeChange = nextP < len && chordAt (nextP) != c;
            const double u = nr.uni();
            if (! strong)
            {
                if (beforeChange && u < 0.35 + 0.4 * cx) key = sc.key (prog[(size_t) chordAt (nextP)] + (nr.chance (0.5) ? 1 : -1));   // approach
                else if (u < 0.3) key = root + P;                                                           // octave
                else if (u < 0.55) key = sc.key (prog[(size_t) c] + 4);                                     // fifth
                else if (u < 0.55 + 0.3 * cx) key = sc.key (prog[(size_t) c] + (nr.chance (0.5) ? 2 : 1));
            }
            key = fold (key);
            if (std::abs (key - last) > 12) key += key > last ? -P : P;
            out.s[(size_t) p].note = fold (key); last = out.s[(size_t) p].note;
        }
    }
    else
    {   // melody: a phrase form of 4-step segments, a motif per letter, chord tones on strong steps, a cadence at the end
        static const char* forms[] = { "AaBa", "ABAb", "AABC", "ABAc" };
        const char* form = forms[nr.below (4)];
        const int segs = (len + 3) / 4;
        std::array<std::array<int, 4>, 4> motif {};
        std::array<bool, 4> made {};
        auto makeMotif = [&] (int startDeg, int dir)
        {
            std::array<int, 4> m {}; int d = startDeg;
            for (int i = 0; i < 4; ++i)
            {
                m[(size_t) i] = d;
                const double u = nr.uni();
                int stepSz = u < 0.55 - 0.3 * cx ? 1 : (u < 0.85 - 0.2 * cx ? 2 : (u < 0.95 ? 3 : 4));
                if (nr.chance (0.22)) stepSz = 0;
                d += (nr.chance (0.7) ? dir : -dir) * stepSz;
            }
            return m;
        };
        const int topDeg = sc.degreeOf (hi), lowDeg = std::max (sc.degreeOf (lo), 0);
        auto startChord = [&] (int which) { const int base[3] = { 0, 2, 4 }; return base[which % 3] + (nr.chance (0.3) ? n : 0); };
        int prevDeg = 0;
        std::vector<int> degAt ((size_t) len, 0);
        for (int sg = 0; sg < segs; ++sg)
        {
            const char ch = form[sg % 4];
            const int L = (std::toupper (ch) - 'A') & 3;
            if (! made[(size_t) L]) { motif[(size_t) L] = makeMotif (startChord (L == 0 ? nr.below (3) : (L == 1 ? 1 + nr.below (2) : 2 + L)), nr.chance (0.5) ? 1 : -1); made[(size_t) L] = true; }
            auto m = motif[(size_t) L];
            if (ch >= 'a' && ch <= 'z') { m[3] += nr.chance (0.5) ? 1 : -1; if (nr.chance (0.5)) m[2] += nr.chance (0.5) ? 1 : -2; }   // a variant: a new ending
            for (int i = 0; i < 4; ++i) { const int p = sg * 4 + i; if (p < len) degAt[(size_t) p] = m[(size_t) i]; }
        }
        for (int p : sounding)
        {
            int d = degAt[(size_t) p];
            if (metric (p) >= 0.65f && n >= 6) { const int r = ((d % n) + n) % n; if (r != 0 && r != 2 && r != 4) d += (r == 1 || r == 3 || r == 5) ? -1 : 1; }
            while (d > topDeg) d -= n;
            while (d < lowDeg - 1 && d + n <= topDeg) d += n;
            degAt[(size_t) p] = d; prevDeg = d;
        }
        if (! sounding.empty())
        {   // cadence: the last note on degree 1 (or 5), in the octave nearest the note before
            const int p = sounding.back(); const int ref = sounding.size() > 1 ? degAt[(size_t) sounding[sounding.size() - 2]] : 0;
            const int target = nr.chance (0.7) ? 0 : 4; int best = target, bd = 1 << 20;
            for (int o = -2; o <= 2; ++o) { const int c = target + o * n; if (sc.key (c) < lo || sc.key (c) > hi) continue; if (std::abs (c - ref) < bd) { bd = std::abs (c - ref); best = c; } }
            degAt[(size_t) p] = best;
        }
        (void) prevDeg;
        for (int p : sounding) out.s[(size_t) p].note = fold (sc.key (degAt[(size_t) p]));
        // repetition inside the pattern: no three equal notes in a row, three different pitches, the halves differ
        for (size_t i = 2; i < sounding.size(); ++i)
        {
            Step& c = out.s[(size_t) sounding[i]];
            if (c.note == out.s[(size_t) sounding[i - 1]].note && c.note == out.s[(size_t) sounding[i - 2]].note)
            {   // the nearest other degree that stays inside the range (a narrow RANGE can fold a move back onto the same key)
                const int first = nr.chance (0.5) ? 1 : -1;
                for (int mv : { first, -first, 2 * first, -2 * first, 3, -3 })
                    if (const int k = fold (sc.key (sc.degreeOf (c.note) + mv)); k != c.note) { c.note = k; break; }
            }
        }
        auto distinct = [&] { std::vector<int> u; for (int p : sounding) if (std::find (u.begin(), u.end(), out.s[(size_t) p].note) == u.end()) u.push_back (out.s[(size_t) p].note); return (int) u.size(); };
        for (size_t i = 1, guard = 0; distinct() < std::min<int> (3, (int) sounding.size()) && guard < 32; ++guard, i = 1 + (i % std::max<size_t> (1, sounding.size() - 1)))
        {
            Step& c = out.s[(size_t) sounding[i]];
            c.note = fold (sc.key (sc.degreeOf (c.note) + (int) (guard % 2 == 0 ? 2 : -1)));
        }
        {   // no pitch takes more than 40 % of the notes (at least 2): the extra ones (never the first or the last) go to a neighbour
            const int cnt = (int) sounding.size(), cap = std::max (2, (cnt * 2 + 4) / 5);
            for (int pass = 0; pass < 3; ++pass)
                for (size_t i = 1; i + 1 < sounding.size(); ++i)
                {
                    Step& c = out.s[(size_t) sounding[i]];
                    int same = 0; for (int p : sounding) same += out.s[(size_t) p].note == c.note;
                    if (same <= cap) continue;
                    const int before = out.s[(size_t) sounding[i - 1]].note, after = out.s[(size_t) sounding[i + 1]].note;
                    for (int mv : { 1, -1, 2, -2, 3 })
                    {
                        const int k = fold (sc.key (sc.degreeOf (c.note) + mv));
                        int kc = 0; for (int p : sounding) kc += out.s[(size_t) p].note == k;
                        if (k != c.note && kc < cap && ! (k == before && k == after)) { c.note = k; break; }
                    }
                }
            // a move above can make three in a row with the notes two away: once more, with the share kept
            for (size_t i = 2; i < sounding.size(); ++i)
            {
                Step& c = out.s[(size_t) sounding[i]];
                if (c.note != out.s[(size_t) sounding[i - 1]].note || c.note != out.s[(size_t) sounding[i - 2]].note) continue;
                const int nextNote = i + 1 < sounding.size() ? out.s[(size_t) sounding[i + 1]].note : 1 << 20;
                for (int mv : { 1, -1, 2, -2, 3, -3 })
                {
                    const int k = fold (sc.key (sc.degreeOf (c.note) + mv));
                    int kc = 0; for (int p : sounding) kc += out.s[(size_t) p].note == k;
                    if (k != c.note && kc < cap && k != nextNote) { c.note = k; break; }
                }
            }
        }
        if (len >= 8)
        {
            const int h = len / 2; bool same = true;
            for (int p = 0; p < h && same; ++p) same = out.s[(size_t) p].on == out.s[(size_t) (p + h)].on && (! out.s[(size_t) p].on || out.s[(size_t) p].note == out.s[(size_t) (p + h)].note);
            if (same)
                for (int p = len - 1; p >= h; --p)
                    if (out.s[(size_t) p].on && p != sounding.back()) { Step& c = out.s[(size_t) p]; c.note = fold (sc.key (sc.degreeOf (c.note) + 1)); break; }
        }
    }

    // micro: one or two pitch classes of the scale sit on the low (or high) row, like a tuning's own colour; NORMAL never
    if (st.microAllowed && st.micro > 0.0f && (role == Role::melody || role == Role::bass) && P == 12 && n >= 5)
    {
        Rng mr (seedFor (nseed, 0x4D49));
        const int cands[4] = { 1, 2, 5, 6 };
        std::vector<std::pair<int, int>> pcs;   // pitch class, row
        const int count = 1 + (st.micro > 0.6f && mr.chance (0.6) ? 1 : 0);
        for (int i = 0; i < count; ++i)
        {
            const int d = cands[mr.below (4)] % n;
            pcs.push_back ({ ((sc.deg[(size_t) d] % 12) + 12) % 12, mr.chance (0.75) ? 0 : 2 });
        }
        for (int p : sounding)
        {
            Step& s = out.s[(size_t) p];
            const int pc = ((s.note % 12) + 12) % 12;
            for (auto& [c, row] : pcs)
                if (pc == c && mr.uni() < 0.35 + 0.65 * st.micro) s.row = row;
        }
    }
    for (int p = len; p < kSeqSteps; ++p) out.s[(size_t) p] = Step {};
    return out;
}

// ---------------------------------------------------------------- the drum machine
// per style and drum, a 16-step weight (1 = always at any density, 0 = never); COMPLEX adds noise, ghosts and a fill
inline float drumWeight (int style, Drum d, int p)
{
    p &= 15;
    auto in = [p] (std::initializer_list<int> l) { for (int x : l) if (x == p) return true; return false; };
    const bool q = p % 4 == 0, e = p % 2 == 0;
    switch (d)
    {
        case Drum::kick:
            if (style == 0) return q ? 1.0f : (in ({ 14, 7 }) ? 0.25f : 0.05f);
            if (style == 1) return in ({ 0, 10 }) ? 1.0f : (in ({ 7, 13, 3 }) ? 0.45f : 0.05f);
            if (style == 2) return in ({ 0, 10 }) ? 1.0f : (in ({ 2, 7, 11, 13 }) ? 0.5f : 0.05f);
            if (style == 3) return p == 0 ? 1.0f : (in ({ 3, 7, 11, 14 }) ? 0.4f : 0.05f);
            if (style == 4) return in ({ 0, 6, 12 }) ? 1.0f : (in ({ 3, 9 }) ? 0.4f : 0.05f);
            return p == 0 ? 1.0f : (p == 10 ? 0.5f : 0.02f);
        case Drum::snare:
            if (style == 3) return p == 8 ? 1.0f : (in ({ 15, 13 }) ? 0.25f : 0.03f);
            if (style == 5) return p == 12 ? 0.9f : 0.02f;
            return in ({ 4, 12 }) ? 1.0f : (in ({ 7, 9, 15, 14 }) ? 0.3f : 0.03f);
        case Drum::clap:
            if (style == 0) return in ({ 4, 12 }) ? 0.8f : 0.02f;
            if (style == 3) return p == 8 ? 0.7f : 0.02f;
            return in ({ 4, 12 }) ? 0.4f : 0.02f;
        case Drum::hat:
            if (style == 0) return in ({ 2, 6, 10, 14 }) ? 0.95f : (e ? 0.45f : 0.3f);
            if (style == 3) return e ? 0.8f : 0.45f;
            if (style == 5) return in ({ 2, 6, 10, 14 }) ? 0.6f : 0.05f;
            return e ? 0.9f : 0.35f;
        case Drum::openHat:
            if (style == 0) return in ({ 2, 6, 10, 14 }) ? 0.55f : 0.02f;
            return in ({ 14, 6 }) ? 0.35f : 0.02f;
        case Drum::tom: return in ({ 13, 14, 15 }) ? 0.25f : 0.02f;
        case Drum::perc:
            if (style == 4) return in ({ 0, 3, 6, 10, 12 }) ? 0.8f : 0.05f;
            return in ({ 3, 6, 11, 14 }) ? 0.35f : 0.05f;
        case Drum::shaker: default: return e ? 0.45f : 0.35f;
    }
}
inline bool drumStrong (int style, Drum d, int p) { return drumWeight (style, d, p) >= 0.95f && (d == Drum::kick || d == Drum::snare); }

inline DrumPattern makeDrums (const std::array<Drum, kDrumTracks>& kit, int rseed, const Settings& st)
{
    DrumPattern out; const int len = std::max (1, std::min (kDrumSteps, st.length)); out.length = len;
    Rng r (seedFor (rseed, 0x4452));
    const std::array<float, kNumDrumStyles> w { { 22, 26, 18, 16, 12, 6 } };
    const int style = r.pick (w); out.style = style;
    const float cx = std::max (0.0f, std::min (1.0f, st.complexity)), den = std::max (0.0f, std::min (1.0f, st.density));
    const bool fill = cx > 0.55f && r.chance (0.35 + 0.5 * (cx - 0.55));
    for (int t = 0; t < kDrumTracks; ++t)
    {
        Rng tr (seedFor (rseed, 0x5452 + (uint64_t) t * 977 + (uint64_t) kit[(size_t) t] * 31));
        const float base = (kit[(size_t) t] == Drum::kick || kit[(size_t) t] == Drum::snare) ? 0.5f : 0.75f;
        const float thr = base - 0.55f * den;   // DENSITY lowers every track's threshold; one score per step, so it nests
        for (int p = 0; p < len; ++p)
        {
            const float wgt = drumWeight (style, kit[(size_t) t], p);
            float s = wgt + (0.1f + 0.45f * cx) * ((float) tr.uni() - 0.5f);
            if (fill && p >= len - 3 && (kit[(size_t) t] == Drum::tom || kit[(size_t) t] == Drum::snare)) s += 0.6f;
            if (wgt >= 0.999f && kit[(size_t) t] == Drum::kick && p == 0) s = 2.0f;
            int v = s > thr ? 1 : 0;
            if (v && drumStrong (style, kit[(size_t) t], p)) v = 2;
            out.s[(size_t) t][(size_t) p] = v;
        }
    }
    return out;
}

// ---------------------------------------------------------------- distance, mutate
inline float distance (const SeqPattern& a, const SeqPattern& b)
{
    const int len = std::max (a.length, b.length);
    int uni = 0, diff = 0, both = 0; float nd = 0; bool pitched = false;
    for (int p = 0; p < len; ++p)
    {
        const Step& x = a.s[(size_t) p]; const Step& y = b.s[(size_t) p];
        if (x.on || y.on) ++uni;
        if (x.on != y.on) ++diff;
        if (x.on && y.on)
        {
            ++both;
            float d = std::min (1.0f, (float) std::abs (x.note - y.note) / 7.0f) + (x.row != y.row ? 0.3f : 0.0f);
            for (int c = 0; c < 3; ++c) d += x.chord[(size_t) c] != y.chord[(size_t) c] ? 0.15f : 0.0f;
            d += (x.accent != y.accent ? 0.25f : 0.0f) + (x.ratchet != y.ratchet ? 0.25f : 0.0f) + (std::fabs (x.gate - y.gate) > 0.3f ? 0.15f : 0.0f);
            nd += std::min (1.0f, d);
            pitched |= x.note != 0 || y.note != 0 || x.chord[0] != 0 || y.chord[0] != 0;
        }
    }
    const float rd = uni == 0 ? 0.0f : (float) diff / (float) uni;
    const float noted = both == 0 ? 1.0f : nd / (float) both;
    if (! pitched) return 0.7f * rd + 0.3f * noted;   // rhythm only (ARP RHYTHM): the steps and their accents / ratchets / lengths carry it
    return 0.5f * rd + 0.5f * noted;
}
inline float distance (const DrumPattern& a, const DrumPattern& b)
{
    int uni = 0, diff = 0;
    for (int t = 0; t < kDrumTracks; ++t) for (int p = 0; p < kDrumSteps; ++p)
    {
        const int x = a.s[(size_t) t][(size_t) p], y = b.s[(size_t) t][(size_t) p];
        if (x || y) ++uni;
        if ((x != 0) != (y != 0)) ++diff;
    }
    return uni == 0 ? 0.0f : (float) diff / (float) uni;
}

// MUTATE: a share m of the steps comes from the new pattern, the rest stays (the same steps for the same seed)
inline SeqPattern mutate (const SeqPattern& old, const SeqPattern& fresh, float m, int seed)
{
    if (m >= 1.0f) return fresh;
    SeqPattern o = old; o.length = fresh.length; o.scaleId = fresh.scaleId;
    for (int p = 0; p < kSeqSteps; ++p) if ((double) (hash2 ((uint64_t) seed, (uint64_t) p) >> 11) * (1.0 / 9007199254740992.0) < m) o.s[(size_t) p] = fresh.s[(size_t) p];
    return o;
}
inline DrumPattern mutate (const DrumPattern& old, const DrumPattern& fresh, float m, int seed)
{
    if (m >= 1.0f) return fresh;
    DrumPattern o = old; o.length = fresh.length;
    for (int t = 0; t < kDrumTracks; ++t) for (int p = 0; p < kDrumSteps; ++p)
        if ((double) (hash2 ((uint64_t) seed, (uint64_t) (t * 64 + p)) >> 11) * (1.0 / 9007199254740992.0) < m) o.s[(size_t) t][(size_t) p] = fresh.s[(size_t) t][(size_t) p];
    return o;
}

// the first candidate (from next(), a stream of seeds) at least kMinDistance from every pattern in history; after kMaxTries the
// farthest one seen. Returns how many seeds were skipped.
template <typename Pattern, typename Make, typename Next>
int pickNovel (const std::vector<Pattern>& history, Make make, Next next, int& seedOut, Pattern& out)
{
    float bestD = -1; int skipped = 0;
    for (int tryNo = 0; tryNo < kMaxTries; ++tryNo)
    {
        const int s = next();
        const Pattern p = make (s);
        float dmin = 1.0f;
        for (const auto& h : history) dmin = std::min (dmin, distance (p, h));
        if (dmin > bestD) { bestD = dmin; seedOut = s; out = p; }
        if (dmin >= kMinDistance) return skipped;
        ++skipped;
    }
    return skipped;
}

// the role a preset family asks for (GENERATE_PLAN §7); arp on: the arp gives the notes, the seq the rhythm
inline Role roleForFamily (const std::string& family, bool arpOn, bool monoVoice, float lowestKeyHint)
{
    if (arpOn) return Role::arpRhythm;
    auto has = [&family] (const char* s) { return family.find (s) != std::string::npos; };
    if (has ("Drum")) return Role::beat;
    if (family == "Init") return Role::melody;
    if (has ("Bass") || has ("Acid")) return Role::bass;
    for (const char* c : { "Pad", "Choir", "Strings", "Organ", "Piano", "E-Piano", "Brass", "Guitar", "Pulse", "Arp - Chord" }) if (has (c)) return Role::chords;
    for (const char* m : { "Lead", "Pluck", "Bell", "Mallet", "Winds", "FX", "Bansuri", "Dizi", "Erhu", "Kanun", "Sitar", "Guzheng", "Zurna", "Melodic", "Lab", "Micro", "Arp" })
        if (has (m)) return Role::melody;
    if (monoVoice) return lowestKeyHint < 48.0f ? Role::bass : Role::melody;
    return Role::chords;
}

// a drum preset's role on a track, from its family / name ("Drums - Kick/Thump Kick", "Drums - Hat/Open Hat")
inline Drum drumForName (const std::string& familyAndName)
{
    std::string s; for (char c : familyAndName) s += (char) std::tolower ((unsigned char) c);
    auto has = [&s] (const char* x) { return s.find (x) != std::string::npos; };
    if (has ("kick") || has ("808") || has ("bd")) return Drum::kick;
    if (has ("open")) return Drum::openHat;
    if (has ("hat") || has ("hh")) return Drum::hat;
    if (has ("clap")) return Drum::clap;
    if (has ("snare") || has ("rim")) return Drum::snare;
    if (has ("tom")) return Drum::tom;
    if (has ("shak") || has ("cabasa") || has ("tamb")) return Drum::shaker;
    return Drum::perc;
}
} // namespace svl::gen
