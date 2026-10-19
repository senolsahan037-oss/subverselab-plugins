// Tuning Lab (SPEC.md "Tuning Lab"): plain C++17, no JUCE, so tests/tuning_check.cpp runs it without the plug-in.
//
// One unit: pitch in MIDI semitones (69 = A4 = 440 Hz, before the engine's tuning.reference offset), as the engine's table.
// A Tuning is a Scala-style degree list: cents of degrees 1..n, the last one is the period (1200 for an octave). It may carry
// a second list of falling values (same length): the pitch a degree plays when the melody descends onto it.
//
// Real time: Lab::notePitch() (audio thread) reads two prebuilt 128-key tables and a fixed array of held notes; it never
// allocates. Tables are rebuilt on the message thread (rebuild()) and published by swapping an atomic index.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace jflab
{
inline double centsToSemis (double c) { return c / 100.0; }
inline double hzToPitch (double hz) { return 69.0 + 12.0 * std::log2 (hz / 440.0); }
inline double pitchToHz (double p) { return 440.0 * std::pow (2.0, (p - 69.0) / 12.0); }

// ----------------------------------------------------------------------------------------------------------------- Tuning
struct Tuning
{
    std::string name = "12 equal";
    std::vector<double> cents { 100, 200, 300, 400, 500, 600, 700, 800, 900, 1000, 1100, 1200 };
    std::vector<double> fall;           // empty, or one falling value per degree (the period's entry is ignored)
    std::vector<float> confidence;      // learned tunings: 0..1 per degree (degree 0 first), else empty

    // mapping (Scala .kbm; the default puts degree 0 on middle C at its 12-equal frequency)
    int mapSize = 0;                    // 0 = linear: consecutive keys are consecutive degrees
    std::vector<int> mapping;           // degree per key in the pattern, -1 = unmapped ('x', key is silent / 12-equal)
    int firstKey = 0, lastKey = 127, middleKey = 60, refKey = 60;
    double refHz = 261.6255653;
    int formalOctave = 0;               // degrees per mapping repeat (0 = scale size)

    int size() const { return (int) cents.size(); }
    double period() const { return cents.empty() ? 1200.0 : cents.back(); }
    bool directional() const { return fall.size() == cents.size() && ! cents.empty(); }

    // cents of a (possibly negative or > n) degree, rising or falling
    double degreeCents (long d, bool falling = false) const
    {
        const long n = (long) cents.size();
        if (n == 0) return (double) d * 100.0;
        long q = d / n, r = d % n;
        if (r < 0) { r += n; --q; }
        const auto& list = (falling && directional()) ? fall : cents;
        return (double) q * period() + (r == 0 ? 0.0 : list[(size_t) r - 1]);
    }

    // degree of a MIDI key, or false when the key is unmapped
    bool keyDegree (int key, long& degree) const
    {
        if (key < firstKey || key > lastKey) return false;
        if (mapSize <= 0) { degree = key - middleKey; return true; }
        long rel = key - middleKey, q = rel / mapSize, r = rel % mapSize;
        if (r < 0) { r += mapSize; --q; }
        const int m = r < (long) mapping.size() ? mapping[(size_t) r] : -1;
        if (m < 0) return false;
        const long oct = formalOctave > 0 ? formalOctave : (long) cents.size();
        degree = q * oct + m;
        return true;
    }

    // the 128-key table (semitones), unmapped keys stay 12-equal
    std::array<float, 128> table (bool falling = false) const
    {
        std::array<float, 128> t {};
        long refDeg = 0;
        const bool refOk = keyDegree (refKey, refDeg);
        const double refC = refOk ? degreeCents (refDeg, falling) : 0.0;
        const double refP = hzToPitch (refHz);
        for (int k = 0; k < 128; ++k)
        {
            long d;
            if (keyDegree (k, d)) t[(size_t) k] = (float) (refP + centsToSemis (degreeCents (d, falling) - refC));
            else t[(size_t) k] = (float) k;
        }
        return t;
    }
};

// ------------------------------------------------------------------------------------------------------------------ Scala
namespace scala
{
inline std::string trim (const std::string& s)
{
    const auto a = s.find_first_not_of (" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr (a, s.find_last_not_of (" \t\r\n") - a + 1);
}
inline std::vector<std::string> lines (const std::string& text)
{
    std::vector<std::string> out;
    std::stringstream ss (text);
    std::string l;
    while (std::getline (ss, l)) if (! l.empty() && l[0] == '!') continue; else out.push_back (l);
    return out;
}
// one pitch line: "701.955" (cents, has a dot), "3/2" or "2" (ratio); text after the number is ignored
inline bool parsePitch (const std::string& raw, double& c)
{
    std::string s = trim (raw);
    const auto sp = s.find_first_of (" \t");
    if (sp != std::string::npos) s = s.substr (0, sp);
    if (s.empty()) return false;
    try
    {
        if (s.find ('.') != std::string::npos) { c = std::stod (s); return true; }
        const auto sl = s.find ('/');
        const double num = std::stod (s.substr (0, sl)), den = sl == std::string::npos ? 1.0 : std::stod (s.substr (sl + 1));
        if (num <= 0 || den <= 0) return false;
        c = 1200.0 * std::log2 (num / den);
        return true;
    }
    catch (...) { return false; }
}

// .scl -> tuning (mapping untouched); returns false and leaves t alone on a malformed file
inline bool readScl (const std::string& text, Tuning& t, std::string* error = nullptr)
{
    const auto ls = lines (text);
    if (ls.size() < 2) { if (error) *error = "too short"; return false; }
    int n = 0;
    try { n = std::stoi (trim (ls[1])); } catch (...) { if (error) *error = "no note count"; return false; }
    if (n < 1 || n > 1024 || (int) ls.size() < 2 + n) { if (error) *error = "note count does not match"; return false; }
    std::vector<double> c;
    for (int i = 0; i < n; ++i)
    {
        double v;
        if (! parsePitch (ls[(size_t) (2 + i)], v)) { if (error) *error = "bad pitch on line " + std::to_string (i + 1); return false; }
        c.push_back (v);
    }
    if (c.back() <= 0) { if (error) *error = "period must be positive"; return false; }
    t.name = trim (ls[0]).empty() ? "Imported" : trim (ls[0]);
    t.cents = c;
    t.fall.clear();
    t.confidence.clear();
    return true;
}

inline std::string writeScl (const Tuning& t, bool falling = false)
{
    std::ostringstream o;
    o.setf (std::ios::fixed);
    o.precision (5);
    o << "! " << t.name << (falling ? " (falling)" : "") << ".scl\n!\n" << t.name << (falling ? " (falling)" : "") << "\n "
      << t.size() << "\n!\n";
    for (int i = 0; i < t.size(); ++i)
    {
        const bool last = i == t.size() - 1;
        o << " " << ((falling && t.directional() && ! last) ? t.fall[(size_t) i] : t.cents[(size_t) i]) << "\n";
    }
    return o.str();
}

// .kbm -> mapping of t
inline bool readKbm (const std::string& text, Tuning& t, std::string* error = nullptr)
{
    const auto ls = lines (text);
    std::vector<std::string> v;
    for (auto& l : ls) if (! trim (l).empty()) v.push_back (trim (l));
    if (v.size() < 7) { if (error) *error = "too short"; return false; }
    try
    {
        const int size = std::stoi (v[0]), first = std::stoi (v[1]), last = std::stoi (v[2]), mid = std::stoi (v[3]),
                  ref = std::stoi (v[4]);
        const double hz = std::stod (v[5]);
        const int oct = std::stoi (v[6]);
        if (size < 0 || size > 128 || hz <= 0) { if (error) *error = "bad header"; return false; }
        std::vector<int> map;
        for (int i = 0; i < size; ++i)
        {
            if (7 + i >= (int) v.size()) { map.push_back (-1); continue; }   // missing entries count as unmapped (Scala does too)
            const std::string e = v[(size_t) (7 + i)];
            map.push_back (e[0] == 'x' || e[0] == 'X' ? -1 : std::stoi (e));
        }
        t.mapSize = size; t.mapping = map; t.firstKey = first; t.lastKey = last; t.middleKey = mid; t.refKey = ref;
        t.refHz = hz; t.formalOctave = oct;
        return true;
    }
    catch (...) { if (error) *error = "bad number"; return false; }
}

inline std::string writeKbm (const Tuning& t)
{
    std::ostringstream o;
    o.setf (std::ios::fixed);
    o.precision (6);
    o << "! " << t.name << ".kbm\n" << t.mapSize << "\n" << t.firstKey << "\n" << t.lastKey << "\n" << t.middleKey << "\n"
      << t.refKey << "\n" << t.refHz << "\n" << t.formalOctave << "\n";
    for (int m : t.mapping) { if (m < 0) o << "x\n"; else o << m << "\n"; }
    return o.str();
}
} // namespace scala

// -------------------------------------------------------------------------------------------------------- interval view
struct Ratio { int p = 1, q = 1; double cents = 0; };

inline int oddPart (int x) { while (x > 0 && x % 2 == 0) x /= 2; return x; }

// the ratio a musician reads for an interval (any size >= 0): among the ratios of odd limit `limit` within `window` cents,
// the simplest (lowest Tenney height log2(p*q)); when none is that close, the nearest one
inline Ratio nearestRatio (double cents, int limit = 15, double window = 18.0)
{
    auto gcd = [] (int x, int y) { while (y) { const int t = x % y; x = y; y = t; } return x; };
    const double c = std::fabs (cents);
    const int oct = (int) std::floor (c / 1200.0);
    const double red = c - 1200.0 * oct;
    Ratio nearest, simplest;
    double nearErr = 1e9, simpleH = 1e9;
    for (int a = 1; a <= limit; a += 2)
        for (int b = 1; b <= limit; b += 2)
        {
            int p = a, q = b;                        // octave-reduce a/b into [1, 2)
            while (p < q) p *= 2;
            while (p >= 2 * q) q *= 2;
            for (int up = 0; up <= 1; ++up)          // also one octave up, so 1190 ct finds 2/1
            {
                int pp = up ? p * 2 : p, qq = q;
                const int g = gcd (pp, qq);
                pp /= g; qq /= g;
                const double rc = 1200.0 * std::log2 ((double) pp / qq);
                const double err = std::fabs (rc - red), h = std::log2 ((double) pp * qq);
                if (err < nearErr - 1e-9) { nearErr = err; nearest = { pp, qq, rc }; }
                if (err <= window && h < simpleH - 1e-9) { simpleH = h; simplest = { pp, qq, rc }; }
            }
        }
    Ratio best = simpleH < 1e8 ? simplest : nearest;
    for (int i = 0; i < oct; ++i) best.p *= 2;
    const int g = gcd (best.p, best.q);
    best.p /= g; best.q /= g;
    best.cents += 1200.0 * oct;
    return best;
}

// beat rate (Hz) between the nearest coinciding partials of two tones a ratio p/q apart: q-th partial of the upper note
// against the p-th partial of the lower note
inline double beatRate (double lowHz, double highHz, const Ratio& r) { return std::fabs ((double) r.q * highHz - (double) r.p * lowHz); }

struct IntervalInfo { int lowKey, highKey; double cents; Ratio ratio; double deviation, beatHz; };

// --------------------------------------------------------------------------------------------------- adaptive just
// 5-limit ratios inside an octave, used by the adaptive mode
inline const std::array<Ratio, 13>& fiveLimit()
{
    static const std::array<Ratio, 13> r = []
    {
        const int pq[13][2] = { {1,1},{16,15},{9,8},{6,5},{5,4},{4,3},{45,32},{3,2},{8,5},{5,3},{9,5},{15,8},{2,1} };
        std::array<Ratio, 13> a {};
        for (int i = 0; i < 13; ++i) a[(size_t) i] = { pq[i][0], pq[i][1], 1200.0 * std::log2 ((double) pq[i][0] / pq[i][1]) };
        return a;
    }();
    return r;
}

// ------------------------------------------------------------------------------------------------------------------ Lab
struct LabSettings
{
    bool on = false;                // the Lab drives the pitches (else the engine's own table / rows)
    bool directional = true;        // use falling values when the tuning has them
    bool adaptive = false;
    float morph = 0;                // 0 = tuning A, 1 = tuning B
    float adaptiveWindowCents = 20;
    float homePull = 0.5f;
    float phraseSeconds = 1.5f;     // a note further apart than this starts a new phrase (direction resets to rising)
};

class Lab
{
public:
    static constexpr int kMaxHeld = 32;

    // the unlock flag (no manual licence key). Owner 18 Oct morning: "kilitli sürümü şu an kaldıralım, tüm kontrollere bakmak
    // istiyorum" -> unlocked; set kUnlockedByDefault = false to ship it locked again (the unlock path is PARKED for the owner)
    static constexpr bool kUnlockedByDefault = true;
    static bool unlocked() noexcept { return unlockedFlag().load(); }
    static void setUnlockedForTests (bool u) noexcept { unlockedFlag().store (u); }

    bool active() const noexcept { return unlocked() && settings.on; }

    LabSettings settings;
    Tuning a, b;

    // message thread: rebuild the morphed rising / falling tables and publish them
    void rebuild()
    {
        const int w = (published.load() + 1) % kBuffers;   // three buffers: two rebuilds inside one audio block never touch the one it reads
        const auto ar = a.table (false), af = a.table (true), br = b.table (false), bf = b.table (true);
        const float m = std::clamp (settings.morph, 0.0f, 1.0f);
        for (int k = 0; k < 128; ++k)
        {
            rise[(size_t) w][(size_t) k] = ar[(size_t) k] + m * (br[(size_t) k] - ar[(size_t) k]);
            falls[(size_t) w][(size_t) k] = af[(size_t) k] + m * (bf[(size_t) k] - af[(size_t) k]);
        }
        hasFall[(size_t) w] = a.directional() || b.directional();
        published.store (w);
        version.fetch_add (1);
    }
    int tableVersion() const noexcept { return version.load(); }
    const std::array<float, 128>& risingTable() const noexcept { return rise[(size_t) published.load()]; }

    // audio thread: the pitch a note-on plays. timeSec = the host's running time of the event.
    float noteOn (int key, double timeSec) noexcept
    {
        key = std::clamp (key, 0, 127);
        const int t = published.load();
        // direction: against the previous note of the phrase; a repeat keeps the last direction
        if (lastKey < 0 || timeSec - lastTime > settings.phraseSeconds) dir = +1;
        else if (key > lastKey) dir = +1;
        else if (key < lastKey) dir = -1;
        lastKey = key; lastTime = timeSec;
        const bool falling = settings.directional && hasFall[(size_t) t] && dir < 0;
        const float home = falling ? falls[(size_t) t][(size_t) key] : rise[(size_t) t][(size_t) key];
        float actual = home;
        if (settings.adaptive) actual = adapt (home);
        addHeld (key, home, actual);
        return actual;
    }
    void noteOff (int key) noexcept
    {
        for (int i = 0; i < numHeld; ++i)
            if (held[(size_t) i].key == key) { held[(size_t) i] = held[(size_t) --numHeld]; break; }
    }
    void allOff() noexcept { numHeld = 0; }
    int direction() const noexcept { return dir; }
    // drift meter: mean (actual - home) in cents over the held notes
    float driftCents() const noexcept
    {
        if (numHeld == 0) return 0;
        double s = 0;
        for (int i = 0; i < numHeld; ++i) s += held[(size_t) i].actual - held[(size_t) i].home;
        return (float) (100.0 * s / numHeld);
    }

    struct Held { int key; float home, actual; };
    int heldCount() const noexcept { return numHeld; }
    const Held& heldAt (int i) const noexcept { return held[(size_t) i]; }

private:
    static std::atomic<bool>& unlockedFlag() noexcept { static std::atomic<bool> f { kUnlockedByDefault }; return f; }

    float adapt (float home) noexcept
    {
        if (numHeld == 0) return home;
        int lo = 0;
        for (int i = 1; i < numHeld; ++i) if (held[(size_t) i].actual < held[(size_t) lo].actual) lo = i;
        const auto& L = held[(size_t) lo];
        const double ref = L.actual - settings.homePull * (L.actual - L.home);   // pulled back towards home
        const double c = 100.0 * (home - ref);
        const double sign = c < 0 ? -1.0 : 1.0, ac = std::fabs (c);
        const int oct = (int) std::floor (ac / 1200.0);
        const double red = ac - 1200.0 * oct;
        double best = red, bestErr = 1e9;
        for (const auto& r : fiveLimit())
            if (std::fabs (r.cents - red) < bestErr) { bestErr = std::fabs (r.cents - red); best = r.cents; }
        if (bestErr > settings.adaptiveWindowCents) return home;
        return (float) (ref + sign * (1200.0 * oct + best) / 100.0);
    }
    void addHeld (int key, float home, float actual) noexcept
    {
        noteOff (key);
        if (numHeld < kMaxHeld) held[(size_t) numHeld++] = { key, home, actual };
    }

    static constexpr int kBuffers = 3;
    std::array<std::array<float, 128>, kBuffers> rise {}, falls {};
    std::array<bool, kBuffers> hasFall { false, false, false };
    std::atomic<int> published { 0 }, version { 0 };
    int lastKey = -1, dir = +1;
    double lastTime = -1e9;
    std::array<Held, kMaxHeld> held {};
    int numHeld = 0;
};

// interval view for a set of sounding pitches (semitones), lowest first
inline std::vector<IntervalInfo> intervals (const std::vector<std::pair<int, float>>& keyPitch, int limit = 15)
{
    auto s = keyPitch;
    std::sort (s.begin(), s.end(), [] (auto& x, auto& y) { return x.second < y.second; });
    std::vector<IntervalInfo> out;
    for (size_t i = 0; i < s.size(); ++i)
        for (size_t j = i + 1; j < s.size(); ++j)
        {
            const double c = 100.0 * (s[j].second - s[i].second);
            const Ratio r = nearestRatio (c, limit);
            out.push_back ({ s[i].first, s[j].first, c, r, c - r.cents, beatRate (pitchToHz (s[i].second), pitchToHz (s[j].second), r) });
        }
    return out;
}
} // namespace jflab
