// svl: raw render and selftest for the SVL engine (Source/engine), the same code the plug-in will run.
//
//   svl render --preset p.xml --notes n.txt --out x.wav [--sr 44100] [--block 512] [--tail 4] [--tempo 120] [--length s]
//     --tempo sets the BPM for the arp / seq, the synced LFOs and the echo (no host transport: own clock from 0);
//     --length renders at least that many seconds; when the notes and that length are over, the arp / seq stop and the tail rings
//   svl render --preset p.xml --note 48 [--vel 100] [--start 0] [--len 1] --out x.wav
//   svl write-init <path.xml>
//   svl --selftest [--out-dir out/selftest]
//
// Notes file: one note per line, "pitch velocity start length" (velocity 1..127, start and length in seconds), '#' comments.
// An integer pitch is a MIDI key and goes through the engine's tuning table; a fractional pitch (60.5) is played as is.
// Output: 24-bit PCM stereo WAV. Samples beyond full scale are clipped in the file and counted on stdout.
#include "../Source/engine/SvlEngine.h"
#include "../Source/engine/SvlPreset.h"
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <chrono>
#include <complex>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <new>
#include <string>
#include <vector>

// ---------------------------------------------------------------- allocation counter (selftest: none inside process())
static std::atomic<bool> gCountAllocs { false };
static std::atomic<long> gAllocs { 0 };
void* operator new (std::size_t n)
{
    if (gCountAllocs.load (std::memory_order_relaxed)) gAllocs.fetch_add (1, std::memory_order_relaxed);
    if (void* p = std::malloc (n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void* operator new[] (std::size_t n) { return operator new (n); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }

using Buf = std::vector<float>;
struct Stereo { Buf l, r; };

// ---------------------------------------------------------------- WAV
static long writeWav24 (const std::string& path, const Stereo& s, int sr)
{
    FILE* f = std::fopen (path.c_str(), "wb");
    if (! f) { std::fprintf (stderr, "cannot write %s\n", path.c_str()); return -1; }
    const uint32_t n = (uint32_t) s.l.size(), data = n * 2 * 3;
    auto u32 = [&] (uint32_t v) { std::fwrite (&v, 4, 1, f); };
    auto u16 = [&] (uint16_t v) { std::fwrite (&v, 2, 1, f); };
    std::fwrite ("RIFF", 1, 4, f); u32 (36 + data); std::fwrite ("WAVEfmt ", 1, 8, f);
    u32 (16); u16 (1); u16 (2); u32 ((uint32_t) sr); u32 ((uint32_t) sr * 6); u16 (6); u16 (24);
    std::fwrite ("data", 1, 4, f); u32 (data);
    long clipped = 0;
    for (uint32_t i = 0; i < n; ++i)
        for (const Buf* b : { &s.l, &s.r })
        {
            double x = (*b)[i];
            if (x > 1.0 || x < -1.0) { ++clipped; x = x > 0 ? 1.0 : -1.0; }
            int32_t v = (int32_t) std::lround (x * 8388607.0);
            unsigned char c[3] = { (unsigned char) (v & 0xFF), (unsigned char) ((v >> 8) & 0xFF), (unsigned char) ((v >> 16) & 0xFF) };
            std::fwrite (c, 1, 3, f);
        }
    std::fclose (f);
    return clipped;
}

// ---------------------------------------------------------------- notes + render
struct Note { float pitch, vel; double start, len; };

static bool loadNotes (const std::string& path, std::vector<Note>& out)
{
    FILE* f = std::fopen (path.c_str(), "r");
    if (! f) return false;
    char line[512];
    while (std::fgets (line, sizeof line, f))
    {
        if (char* h = std::strchr (line, '#')) *h = 0;
        Note n {};
        if (std::sscanf (line, "%f %f %lf %lf", &n.pitch, &n.vel, &n.start, &n.len) == 4) out.push_back (n);
    }
    std::fclose (f);
    return true;
}

struct RenderOpts { int sr = 44100, block = 512; double tail = 4.0; double minLen = 0; double maxExtra = 0; double tempo = 120; };   // maxExtra: keep rendering past the tail until silent (FX tails)

static Stereo render (const svl::ParamSet& ps, const std::vector<Note>& notes, const RenderOpts& o, svl::Engine* use = nullptr)
{
    svl::Engine local;
    svl::Engine& e = use ? *use : local;
    e.setParams (ps, true);
    e.setTempo (o.tempo);
    e.prepare (o.sr);
    struct Ev { long at; bool on; float pitch, vel; int id; bool frac; };
    std::vector<Ev> evs;
    long lastOff = 0;
    for (size_t i = 0; i < notes.size(); ++i)
    {
        const auto& n = notes[i];
        const bool frac = n.pitch != std::floor (n.pitch);
        const int id = frac ? 1000 + (int) i : (int) n.pitch;
        const long on = std::lround (n.start * o.sr), off = std::lround ((n.start + n.len) * o.sr);
        evs.push_back ({ on, true, n.pitch, std::fmin (1.0f, n.vel / 127.0f), id, frac });
        evs.push_back ({ off, false, n.pitch, 0, id, frac });
        lastOff = std::max (lastOff, off);
    }
    std::stable_sort (evs.begin(), evs.end(), [] (const Ev& a, const Ev& b) { return a.at < b.at; });
    const long total = std::max ((long) std::lround (o.minLen * o.sr), lastOff + (long) std::lround (o.tail * o.sr));
    Stereo s; s.l.reserve ((size_t) (total + o.maxExtra * o.sr + o.block)); s.r.reserve (s.l.capacity());
    s.l.assign ((size_t) total, 0); s.r.assign ((size_t) total, 0);
    // "silent" = rounds to 0 in a 24-bit file (below half an LSB)
    const float quiet = 0.5f / 8388607.0f;
    const long hold = (long) std::lround (0.5 * o.sr), cap = total + (long) std::lround (o.maxExtra * o.sr);
    long silentRun = 0, pos = 0;
    size_t k = 0;
    for (int n = 0; pos < total || (pos < cap && silentRun < hold); pos += n)   // += n: the block cut at `total` must not skip samples
    {
        n = pos < total ? (int) std::min ((long) o.block, total - pos) : o.block;
        if (pos == total) { e.setParam (svl::ARP_ON, 0); e.setParam (svl::SEQ_ON, 0); e.setParam (svl::DRUMS_ON, 0); }   // patterns stop; tails ring out
        if ((long) s.l.size() < pos + n) { s.l.resize ((size_t) (pos + n), 0); s.r.resize ((size_t) (pos + n), 0); }
        while (k < evs.size() && evs[k].at < pos + n)
        {
            const Ev& v = evs[k++];
            const int off = (int) (v.at - pos);
            if (v.on) { if (v.frac) e.noteOnPitch (v.pitch, v.vel, v.id, off); else e.noteOn ((int) v.pitch, v.vel, off); }
            else e.noteOff (v.id, off);
        }
        e.process (s.l.data() + pos, s.r.data() + pos, n);
        for (int i = 0; i < n; ++i)
            silentRun = (std::fabs (s.l[(size_t) (pos + i)]) < quiet && std::fabs (s.r[(size_t) (pos + i)]) < quiet) ? silentRun + 1 : 0;
    }
    // trim trailing silence (keep 50 ms)
    const long have = (long) s.l.size();
    long end = have;
    while (end > 0 && std::fabs (s.l[(size_t) end - 1]) < quiet && std::fabs (s.r[(size_t) end - 1]) < quiet) --end;
    end = std::min (have, end + (long) (0.05 * o.sr));
    end = std::max (end, std::min (have, (long) std::lround (o.minLen * o.sr)));
    s.l.resize ((size_t) end); s.r.resize ((size_t) end);
    return s;
}

// ---------------------------------------------------------------- analysis
static void fft (std::vector<std::complex<double>>& a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap (a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = -2 * svl::kPi / (double) len;
        const std::complex<double> wl (std::cos (ang), std::sin (ang));
        for (size_t i = 0; i < n; i += len)
        {
            std::complex<double> w (1);
            for (size_t j = 0; j < len / 2; ++j)
            {
                const auto u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v; a[i + j + len / 2] = u - v; w *= wl;
            }
        }
    }
}

static std::vector<double> powerSpectrum (const float* x, size_t n)   // Blackman-Harris 4-term
{
    std::vector<std::complex<double>> a (n);
    for (size_t i = 0; i < n; ++i)
    {
        const double t = 2 * svl::kPi * (double) i / (double) (n - 1);
        const double w = 0.35875 - 0.48829 * std::cos (t) + 0.14128 * std::cos (2 * t) - 0.01168 * std::cos (3 * t);
        a[i] = x[i] * w;
    }
    fft (a);
    std::vector<double> p (n / 2);
    for (size_t i = 0; i < n / 2; ++i) p[i] = std::norm (a[i]);
    return p;
}

struct AliasResult { double ratioDb, worstDbc; };

// Power off the harmonic series (±5 bins around each k·f0), relative to the power on it; and the largest off-series bin
// relative to the fundamental's peak bin. 20 Hz .. Nyquist.
static AliasResult aliasMeasure (const float* x, size_t n, double f0, double sr, double upper = 1e9)
{
    const auto p = powerSpectrum (x, n);
    const double bin = sr / (double) n;
    std::vector<char> harm (p.size(), 0);
    for (double f = f0; f < sr * 0.5; f += f0)
    {
        const long c = std::lround (f / bin);
        for (long b = c - 5; b <= c + 5; ++b) if (b >= 0 && b < (long) p.size()) harm[(size_t) b] = 1;
    }
    double hp = 0, ap = 0, worst = 0, fund = 0;
    const long c1 = std::lround (f0 / bin);
    for (long b = c1 - 5; b <= c1 + 5; ++b) if (b >= 0 && b < (long) p.size()) fund = std::max (fund, p[(size_t) b]);
    const size_t top = std::min (p.size(), (size_t) (upper / bin));
    for (size_t b = (size_t) std::ceil (20 / bin); b < top; ++b)
    {
        if (harm[b]) hp += p[b];
        else { ap += p[b]; worst = std::max (worst, p[b]); }
    }
    return { 10 * std::log10 (ap / hp + 1e-30), 10 * std::log10 (worst / fund + 1e-30) };
}

// Frequency of the strongest bin within ±1 semitone of `expect`, refined by a parabola on the log magnitudes.
static double peakFreq (const float* x, size_t n, double sr, double expect)
{
    const auto p = powerSpectrum (x, n);
    const double bin = sr / (double) n;
    const long lo = (long) std::floor (expect / 1.06 / bin), hi = (long) std::ceil (expect * 1.06 / bin);
    long best = lo;
    for (long b = lo; b <= hi; ++b) if (p[(size_t) b] > p[(size_t) best]) best = b;
    const double a = std::log (p[(size_t) best - 1] + 1e-30), b0 = std::log (p[(size_t) best] + 1e-30), c = std::log (p[(size_t) best + 1] + 1e-30);
    const double d = 0.5 * (a - c) / (a - 2 * b0 + c);
    return ((double) best + d) * bin;
}

static double cents (double f, double ref) { return 1200.0 * std::log2 (f / ref); }

static double peakDb (const Stereo& s)
{
    float m = 0;
    for (size_t i = 0; i < s.l.size(); ++i) m = std::max (m, std::max (std::fabs (s.l[i]), std::fabs (s.r[i])));
    return 20 * std::log10 (m + 1e-30);
}

// ITU-R BS.1770-4 integrated loudness (K-weighting, 400 ms blocks, 75 % overlap, -70 LUFS and -10 LU gates).
static double lufs (const Stereo& s, double sr)
{
    struct Bq { double b0, b1, b2, a1, a2, z1 = 0, z2 = 0; double run (double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; } };
    auto shelf = [&]
    {
        const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        const double K = std::tan (svl::kPi * f0 / sr), Vh = std::pow (10.0, G / 20), Vb = std::pow (Vh, 0.4996667741545416);
        const double a0 = 1 + K / Q + K * K;
        return Bq { (Vh + Vb * K / Q + K * K) / a0, 2 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0, 2 * (K * K - 1) / a0, (1 - K / Q + K * K) / a0 };
    };
    auto hpf = [&]
    {
        const double f0 = 38.13547087602444, Q = 0.5003270373238773, K = std::tan (svl::kPi * f0 / sr);
        const double a0 = 1 + K / Q + K * K;
        return Bq { 1, -2, 1, 2 * (K * K - 1) / a0, (1 - K / Q + K * K) / a0 };
    };
    std::vector<double> zl (s.l.size()), zr (s.r.size());
    Bq s1 = shelf(), h1 = hpf(), s2 = shelf(), h2 = hpf();
    for (size_t i = 0; i < s.l.size(); ++i) { zl[i] = h1.run (s1.run (s.l[i])); zr[i] = h2.run (s2.run (s.r[i])); }
    const size_t blk = (size_t) (0.4 * sr), hop = blk / 4;
    std::vector<double> e;
    for (size_t st = 0; st + blk <= zl.size(); st += hop)
    {
        double a = 0, b = 0;
        for (size_t i = st; i < st + blk; ++i) { a += zl[i] * zl[i]; b += zr[i] * zr[i]; }
        e.push_back ((a + b) / (double) blk);
    }
    auto L = [] (double z) { return -0.691 + 10 * std::log10 (z + 1e-30); };
    double sum = 0; int cnt = 0;
    for (double z : e) if (L (z) > -70) { sum += z; ++cnt; }
    if (! cnt) return -200;
    const double rel = L (sum / cnt) - 10;
    sum = 0; cnt = 0;
    for (double z : e) if (L (z) > -70 && L (z) > rel) { sum += z; ++cnt; }
    return cnt ? L (sum / cnt) : -200;
}

static double meanDb (const Buf& b)
{
    double m = 0; for (float x : b) m += x;
    m /= (double) std::max<size_t> (1, b.size());
    return 20 * std::log10 (std::fabs (m) + 1e-30);
}

// ---------------------------------------------------------------- selftest
#if defined (__GNUC__) || defined (__clang__)
 #define SVL_PRINTF_LIKE(f, a) __attribute__ ((format (printf, f, a)))
#else
 #define SVL_PRINTF_LIKE(f, a)   // MSVC has no format attribute (Windows CI)
#endif
struct Report
{
    FILE* f = nullptr; int pass = 0, fail = 0;
    void line (const char* fmt, ...) SVL_PRINTF_LIKE (2, 3)
    {
        char b[1024]; va_list ap; va_start (ap, fmt); std::vsnprintf (b, sizeof b, fmt, ap); va_end (ap);
        std::printf ("%s\n", b); if (f) std::fprintf (f, "%s\n", b);
    }
    void check (bool ok, const char* what) { ok ? ++pass : ++fail; line ("  [%s] %s", ok ? "PASS" : "FAIL", what); }
};

static svl::ParamSet initParams() { return svl::ParamSet(); }

static std::string gRoot = ".";

static svl::ParamSet presetParams (const std::string& rel)
{
    svl::Preset p;
    if (! svl::loadPreset (gRoot + "/" + rel, p)) std::fprintf (stderr, "missing preset %s\n", rel.c_str());
    for (auto& w : p.warnings) std::fprintf (stderr, "%s: %s\n", rel.c_str(), w.c_str());
    return p.params;
}

// the preset's drum kit (empty tracks: the default kit), resolved from presets/ as the plug-in does
static void applyKit (svl::Engine& e, const svl::Preset& p)
{
    for (int t = 0; t < svl::kDrumTracks; ++t)
    {
        const std::string name = p.drumKit[(size_t) t].empty() ? svl::kDefaultDrumKit[t] : p.drumKit[(size_t) t];
        svl::Preset d;
        if (svl::loadPreset (gRoot + "/presets/" + name + ".xml", d)) e.setDrumTrackSound (t, d.params, svl::drumGateSeconds (d.meta.family));
        else { e.clearDrumTrackSound (t); std::fprintf (stderr, "drum kit: missing preset %s\n", name.c_str()); }
    }
}

static const char* kPresetFiles[] = { "presets/Init/Init.xml", "presets/Bass/Saw Bass.xml", "presets/Pluck/Glass Pluck.xml",
                                      "presets/Pad/Slow Pad.xml", "presets/Guitar/KS String.xml" };

static int selftest (const std::string& outDir)
{
    std::string cmd = "mkdir -p '" + outDir + "'";
    if (std::system (cmd.c_str()) != 0) return 2;
    Report R; R.f = std::fopen ((outDir + "/selftest_report.txt").c_str(), "w");
    static constexpr int SR = 44100;   // static constexpr: MSVC lets captureless lambdas below read it
    R.line ("SVL engine selftest — sample rate %d Hz", SR);

    // 1. aliasing through the engine's own voice path (VA at 2x -> drive off -> decimator -> SVF high-pass at 20 Hz, Q 0.5),
    //    stepped high sweep 1 kHz .. 12 kHz in 24 log steps; baseline: the same oscillator naive (no polyBLEP) at 1x
    {
        R.line ("\n1. Aliasing (16384-pt Blackman-Harris FFT; ratio = off-harmonic power / harmonic power, full band 20 Hz..22.05 kHz"
                "\n   and audible band 20 Hz..16 kHz; worst = largest single off-harmonic bin vs the fundamental)");
        struct Case { const char* name; svl::OscType t; float pw; };
        const Case cases[] = { { "saw", svl::OscType::saw, 0.5f }, { "square pw .5", svl::OscType::square, 0.5f },
                               { "square pw .2", svl::OscType::square, 0.2f }, { "triangle", svl::OscType::triangle, 0.5f } };
        const size_t N = 16384;
        Buf x (N);
        auto med = [] (std::vector<double> v) { std::sort (v.begin(), v.end()); return v[v.size() / 2]; };
        auto mx = [] (const std::vector<double>& v) { return *std::max_element (v.begin(), v.end()); };
        for (const auto& c : cases)
        {
            std::vector<double> full, aud, worstAud, naiveFull;
            for (int s = 0; s < 24; ++s)
            {
                const double f0 = 1000.0 * std::pow (12.0, s / 23.0);
                svl::ParamSet p = initParams();
                p[svl::oscParam (0, svl::O_TYPE)] = (float) c.t; p[svl::oscParam (0, svl::O_PW)] = c.pw;
                p[svl::FLT_MODE] = (float) svl::FilterMode::hp; p[svl::FLT_CUTOFF] = 20; p[svl::FLT_RESO] = 0;
                p[svl::AENV_A] = 0;
                const float pitch = (float) (69.0 + 12.0 * std::log2 (f0 / 440.0));
                svl::Engine e; e.setParams (p, true); e.prepare (SR);
                e.noteOnPitch (pitch, 1.0f, 1, 0);
                Buf L (N + 4096), Rr (N + 4096);
                e.process (L.data(), Rr.data(), (int) L.size());
                const double fe = svl::pitchToHz (pitch);
                full.push_back (aliasMeasure (L.data() + 4096, N, fe, SR).ratioDb);
                const auto au = aliasMeasure (L.data() + 4096, N, fe, SR, 16000);
                aud.push_back (au.ratioDb); worstAud.push_back (au.worstDbc);
                svl::VaOsc o; const float dt = (float) (fe / SR);
                for (size_t i = 0; i < 4096; ++i) o.next (c.t, dt, c.pw, true);
                for (size_t i = 0; i < N; ++i) x[i] = o.next (c.t, dt, c.pw, true);
                naiveFull.push_back (aliasMeasure (x.data(), N, fe, SR).ratioDb);
            }
            R.line ("  %-13s full band: median %6.1f, worst %6.1f dB | audible band: median %6.1f, worst %6.1f dB, worst single %6.1f dBc | naive 1x full band median %6.1f dB",
                    c.name, med (full), mx (full), med (aud), mx (aud), mx (worstAud), med (naiveFull));
            char what[128]; std::snprintf (what, sizeof what, "%s: full-band median at least 10 dB below naive", c.name);
            R.check (med (full) < med (naiveFull) - 10, what);
        }
        // the listening file: one saw gliding 987 Hz -> 12.5 kHz in 4 s through the whole engine (filter open)
        svl::ParamSet p = initParams();
        p[svl::FLT_CUTOFF] = 20000; p[svl::V_MODE] = (float) svl::VoiceMode::legato; p[svl::V_GLIDE] = 4.0f;
        p[svl::AENV_R] = 0.05f;
        std::vector<Note> nn = { { 83, 100, 0.0, 4.6 }, { 127, 100, 0.5, 4.1 } };
        RenderOpts o; o.tail = 0.3;
        auto s = render (p, nn, o);
        writeWav24 (outDir + "/alias_saw_sweep_987Hz_to_12k5.wav", s, SR);
        R.line ("  wrote alias_saw_sweep_987Hz_to_12k5.wav (legato glide, peak %.1f dBFS)", peakDb (s));
    }

    // 2–3. DC and level for every preset's demo phrase
    {
        R.line ("\n2-3. DC offset, peak and loudness per preset (phrase: notes/<preset>.txt; peak = sample peak)");
        const char* notesFor[] = { "notes/init.txt", "notes/saw_bass.txt", "notes/pluck.txt", "notes/pad.txt", "notes/ks_string.txt" };
        double worstDc = -999;
        for (int i = 0; i < 5; ++i)
        {
            std::vector<Note> nn;
            if (! loadNotes (gRoot + "/" + notesFor[i], nn)) { R.line ("  missing %s", notesFor[i]); ++R.fail; continue; }
            auto s = render (presetParams (kPresetFiles[i]), nn, RenderOpts());
            const double dcl = meanDb (s.l), dcr = meanDb (s.r);
            worstDc = std::max (worstDc, std::max (dcl, dcr));
            R.line ("  %-28s len %5.2f s  peak %6.1f dBFS  %6.1f LUFS-I  DC L %6.1f R %6.1f dBFS",
                    kPresetFiles[i], s.l.size() / (double) SR, peakDb (s), lufs (s, SR), dcl, dcr);
        }
        // square at PW 0.1 held 3 s (largest duty-cycle DC before correction), DC over the held part
        svl::ParamSet p = initParams();
        p[svl::oscParam (0, svl::O_TYPE)] = (float) svl::OscType::square; p[svl::oscParam (0, svl::O_PW)] = 0.1f;
        auto s = render (p, { { 36, 127, 0.0, 3.0 } }, RenderOpts());
        Buf held (s.l.begin() + SR / 2, s.l.begin() + 3 * SR);
        R.line ("  square pw 0.1, C2 held: DC %.1f dBFS (0.5..3 s)", meanDb (held));
        worstDc = std::max (worstDc, meanDb (held));
        R.check (worstDc < -60, "DC below -60 dBFS everywhere");

        // worst case: 16 voices of Init at velocity 127, 4 octaves
        std::vector<Note> chord;
        for (int k = 0; k < 16; ++k) chord.push_back ({ (float) (36 + k * 3), 127, 0.0, 2.0 });
        auto c = render (initParams(), chord, RenderOpts());
        R.line ("  Init, 16 voices at velocity 127 (C2..A5 in minor thirds): peak %.1f dBFS, %.1f LUFS-I", peakDb (c), lufs (c, SR));
    }

    // 4. denormals / NaN
    {
        R.line ("\n4. Denormals and NaN");
        svl::ParamSet p = initParams();
        p[svl::FLT_RESO] = 1.0f; p[svl::FLT_CUTOFF] = 800;
        RenderOpts o; o.tail = 20; o.minLen = 22;
        auto t0 = std::chrono::steady_clock::now();
        auto s = render (p, { { 48, 127, 0.0, 0.5 } }, o);
        const double el = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        long sub = 0, nonfin = 0, nonzeroTail = 0;
        for (size_t i = 0; i < s.l.size(); ++i)
            for (float v : { s.l[i], s.r[i] })
            {
                if (! std::isfinite (v)) ++nonfin;
                else if (v != 0 && std::fabs (v) < 1.17549435e-38f) ++sub;
                if (i > (size_t) (2 * SR) && v != 0) ++nonzeroTail;
            }
        R.line ("  reso 1.0 note + 21.5 s tail: %ld subnormal, %ld non-finite samples; non-zero samples after 2 s: %ld; render %.3f s for %.1f s audio",
                sub, nonfin, nonzeroTail, el, s.l.size() / (double) SR);
        R.check (sub == 0 && nonfin == 0, "no subnormal or non-finite output in the resonant tail");

        // random extremes: 200 parameter sets, each 0.5 s of a 4-note chord at random pitches 0..127
        svl::Rng rng; rng.seed (7);
        long bad = 0; double worstPeak = -999; uint64_t resets = 0;
        const auto& defs = svl::paramDefs();
        for (int t = 0; t < 200; ++t)
        {
            svl::ParamSet q;
            for (int i = 0; i < svl::kNumParams; ++i)
            {
                const auto& d = defs[(size_t) i];
                const float u = 0.5f + 0.5f * rng.uni();
                q[i] = d.min + (d.max - d.min) * (rng.uni() > 0.4f ? u : (rng.uni() > 0 ? 1.0f : 0.0f));
            }
            q[svl::AENV_R] = std::fmin (q[svl::AENV_R], 0.5f); q[svl::MASTER] = -6;
            std::vector<Note> nn;
            for (int k = 0; k < 4; ++k) nn.push_back ({ std::floor (64 + 63.9f * rng.uni()), 127, 0.0, 0.4 });
            svl::Engine e;
            RenderOpts ro; ro.tail = 0.1;
            auto s2 = render (q, nn, ro, &e);
            for (size_t i = 0; i < s2.l.size(); ++i) if (! std::isfinite (s2.l[i]) || ! std::isfinite (s2.r[i])) ++bad;
            worstPeak = std::max (worstPeak, peakDb (s2));
            resets += e.stats.nonFiniteResets;
        }
        R.line ("  200 random extreme parameter sets: %ld non-finite output samples, %llu voice resets by the guard, worst peak %.1f dBFS",
                bad, (unsigned long long) resets, worstPeak);
        R.check (bad == 0, "no non-finite output with random extreme parameters");
    }

    // 5. mono / legato / glide
    {
        R.line ("\n5. Mono, legato, glide (engine state read per sample)");
        const int B = 64;
        float l[B], r[B];
        auto run = [&] (svl::Engine& e, int samples, auto&& probe)
        {
            for (int done = 0; done < samples; done += B) { e.process (l, r, B); probe(); }
        };
        {   // legato, glide 0.1 s
            svl::Engine e; svl::ParamSet p = initParams();
            p[svl::V_MODE] = (float) svl::VoiceMode::legato; p[svl::V_GLIDE] = 0.1f;
            p[svl::oscParam (0, svl::O_TYPE)] = (float) svl::OscType::sine;
            e.setParams (p, true); e.prepare (SR);
            e.noteOn (60, 0.8f, 0);
            int maxVoices = 0; run (e, SR / 2, [&] { maxVoices = std::max (maxVoices, e.activeVoices (true)); });
            const int stageBefore = e.voiceInfo (0).ampStage;
            e.noteOn (67, 0.8f, 0);
            long reach = -1; long n = 0; bool retrig = false; float mid = 0;
            for (int blk = 0; blk < SR / B; ++blk)
            {
                e.process (l, r, B); n += B;
                const auto v = e.voiceInfo (0);
                if (v.ampStage == svl::Adsr::attack) retrig = true;
                if (n == 2240) mid = v.pitch;
                if (reach < 0 && std::fabs (v.pitch - 67) < 1e-4f) reach = n;
                maxVoices = std::max (maxVoices, e.activeVoices (true));
            }
            R.line ("  legato: amp stage before second key %d (3 = sustain); retriggered: %s; pitch at 2240 samples %.3f (linear target %.3f); within 1e-4 st of 67 after %ld samples (glide = %d, block %d); max voices %d",
                    stageBefore, retrig ? "yes" : "no", mid, 60 + 7 * 2240.0 / (0.1 * SR), reach, (int) std::lround (0.1 * SR), B, maxVoices);
            R.check (! retrig && reach > 0 && std::abs (reach - (long) std::lround (0.1 * SR)) <= B && maxVoices == 1, "legato: no retrigger, glide lands on time, one voice");
            e.noteOff (67, 0);
            run (e, SR / 4, [] {});
            const auto back = e.voiceInfo (0);
            R.line ("  legato: release top key while 60 held -> pitch %.4f, gate %d", back.pitch, (int) back.gate);
            R.check (std::fabs (back.pitch - 60) < 1e-4f && back.gate, "legato: returns to the held key");
            // audio check: frequency after the return, sine osc
            Buf mono (16384);
            for (size_t i = 0; i < mono.size(); i += B) { e.process (l, r, B); for (int k = 0; k < B; ++k) mono[i + (size_t) k] = l[k]; }
            const double f = peakFreq (mono.data(), mono.size(), SR, 261.6256);
            R.line ("  legato: measured %.3f Hz after return, %.2f cents from C4", f, cents (f, 261.6256));
            R.check (std::fabs (cents (f, 261.6256)) < 1.0, "legato: audio pitch within 1 cent");
            e.noteOff (60, 0);
            run (e, SR, [] {});
            R.check (e.activeVoices (true) == 0, "legato: voice ends after release");
        }
        {   // mono, no glide: retrigger, pitch jumps
            svl::Engine e; svl::ParamSet p = initParams();
            p[svl::V_MODE] = (float) svl::VoiceMode::mono;
            e.setParams (p, true); e.prepare (SR);
            e.noteOn (60, 0.8f, 0); run (e, SR / 2, [] {});
            e.noteOn (64, 0.8f, 0); e.process (l, r, B);
            const auto v = e.voiceInfo (0);
            R.line ("  mono: after second key stage %d (1 = attack), pitch %.3f, voices %d", v.ampStage, v.pitch, e.activeVoices (true));
            R.check (v.ampStage == svl::Adsr::attack && std::fabs (v.pitch - 64) < 1e-6f && e.activeVoices (true) == 1, "mono: retrigger, no glide at 0 s, one voice");
        }
        {   // mono with glide: glides even when retriggering
            svl::Engine e; svl::ParamSet p = initParams();
            p[svl::V_MODE] = (float) svl::VoiceMode::mono; p[svl::V_GLIDE] = 0.05f;
            e.setParams (p, true); e.prepare (SR);
            e.noteOn (48, 0.8f, 0); run (e, SR / 4, [] {});
            e.noteOn (60, 0.8f, 0);
            long reach = -1, n = 0;
            for (int blk = 0; blk < SR / 4 / B; ++blk)
            { e.process (l, r, B); n += B; if (reach < 0 && std::fabs (e.voiceInfo (0).pitch - 60) < 1e-4f) reach = n; }
            R.line ("  mono glide 0.05 s: reached 60 after %ld samples (expected %ld ± %d)", reach, std::lround (0.05 * SR), B);
            R.check (reach > 0 && std::abs (reach - std::lround (0.05 * SR)) <= B, "mono: glide time");
        }
    }

    // 6. voice stealing
    {
        R.line ("\n6. Voice stealing");
        auto stealRun = [&] (int poly, int notes, const char* label)
        {
            svl::Engine e; svl::ParamSet p = initParams();
            p[svl::V_POLY] = (float) poly; p[svl::oscParam (0, svl::O_TYPE)] = (float) svl::OscType::sine;
            p[svl::AENV_A] = 0.001f;
            e.setParams (p, true); e.prepare (SR);
            Buf L (SR * 2), Rr (SR * 2);
            const int B = 256; int maxV = 0;
            for (int i = 0, k = 0; i < SR * 2; i += B)
            {
                if (k < notes && i >= k * (SR / 100)) { e.noteOn (40 + k, 0.9f, 0); ++k; }   // one new key every 10 ms, all held
                e.process (L.data() + i, Rr.data() + i, B);
                maxV = std::max (maxV, e.activeVoices (false));
            }
            std::vector<int> ids;
            for (int v = 0; v < svl::kVoicePool; ++v) { auto vi = e.voiceInfo (v); if (vi.active && ! vi.fading) ids.push_back (vi.id); }
            std::sort (ids.begin(), ids.end());
            const bool oldestGone = ! ids.empty() && ids.front() == 40 + (notes - poly);
            // largest sample step anywhere vs. in the steady part
            float stepAll = 0, stepSteady = 0;
            for (size_t i = 1; i < L.size(); ++i)
            {
                const float d = std::fabs (L[i] - L[i - 1]);
                stepAll = std::max (stepAll, d);
                if (i > (size_t) SR) stepSteady = std::max (stepSteady, d);
            }
            R.line ("  %s: %d keys into %d voices -> max %d playing, %llu steals, %llu hard steals; lowest surviving key %d (expected %d); largest sample step %.4f (steady chord %.4f)",
                    label, notes, poly, maxV, (unsigned long long) e.stats.steals, (unsigned long long) e.stats.hardSteals,
                    ids.empty() ? -1 : ids.front(), 40 + notes - poly, stepAll, stepSteady);
            char what[160]; std::snprintf (what, sizeof what, "%s: never more than %d voices, %d steals, oldest keys stolen", label, poly, notes - poly);
            R.check (maxV <= poly && (int) e.stats.steals == notes - poly && oldestGone, what);
        };
        stealRun (16, 20, "poly 16");
        stealRun (4, 8, "poly 4");
        // pool exhaustion: 40 keys in one block
        svl::Engine e; svl::ParamSet p = initParams(); e.setParams (p, true); e.prepare (SR);
        for (int k = 0; k < 40; ++k) e.noteOn (30 + k, 1, 0);
        float l[64], r[64]; e.process (l, r, 64);
        R.line ("  40 keys on one sample: %d playing, %d sounding incl. fading, %llu steals, %llu hard steals",
                e.activeVoices (false), e.activeVoices (true), (unsigned long long) e.stats.steals, (unsigned long long) e.stats.hardSteals);
        R.check (e.activeVoices (false) <= 16, "40 simultaneous keys: never more than 16 playing");
    }

    // 7. block-size independence + sample-accurate start
    {
        R.line ("\n7. Block size and note timing");
        std::vector<Note> nn; loadNotes (gRoot + "/notes/pluck.txt", nn);
        const auto ps = presetParams (kPresetFiles[2]);
        RenderOpts o; o.block = 512;
        auto ref = render (ps, nn, o);
        double worst = 0;
        for (int b : { 1, 7, 64, 333, 4096 })
        {
            o.block = b; auto s = render (ps, nn, o);
            double d = s.l.size() == ref.l.size() ? 0 : 1;
            for (size_t i = 0; i < std::min (s.l.size(), ref.l.size()); ++i) d = std::max (d, (double) std::fabs (s.l[i] - ref.l[i]) + std::fabs (s.r[i] - ref.r[i]));
            R.line ("  block %4d vs 512: max difference %.3g", b, d);
            worst = std::max (worst, d);
        }
        R.check (worst == 0, "output identical for every block size");
        // the same with drift, noise floor, pitch envelope, click, per-osc decay and choke
        for (const char* pf : { "presets/Pluck/Saw Pluck.xml", "presets/Drums - Snare/Crack Snare.xml" })
        {
            const auto p2 = presetParams (pf);
            o.block = 512; o.tail = 4; auto r2 = render (p2, nn, o);
            double w2 = 0;
            for (int b : { 1, 77, 4096 })
            {
                o.block = b; auto s = render (p2, nn, o);
                double d = s.l.size() == r2.l.size() ? 0 : 1;
                for (size_t i = 0; i < std::min (s.l.size(), r2.l.size()); ++i) d = std::max (d, (double) std::fabs (s.l[i] - r2.l[i]) + std::fabs (s.r[i] - r2.r[i]));
                w2 = std::max (w2, d);
            }
            R.line ("  %s, blocks 1 / 77 / 4096 vs 512: max difference %.3g", pf, w2);
            R.check (w2 == 0, "drum / drift features identical for every block size");
        }
        {   // choke: a second hit cuts the first within the steal fade
            svl::Engine e; e.setParams (presetParams ("presets/Drums - Hat/Crisp Hat.xml"), true); e.prepare (SR);
            float l[64], r[64];
            e.noteOn (60, 1, 0); e.process (l, r, 64);
            e.noteOn (61, 1, 0);
            int maxV = 0; for (int b = 0; b < 10; ++b) { e.process (l, r, 64); maxV = std::max (maxV, e.activeVoices (true)); }
            R.line ("  choke: after the second hit and 640 samples, %d voice(s) sounding (fade %d samples)", e.activeVoices (true), (int) std::lround (svl::kStealFadeSec * SR));
            R.check (e.activeVoices (true) == 1, "choke cuts the previous hit");
        }
        svl::ParamSet p = initParams(); p[svl::oscParam (0, svl::O_TYPE)] = (float) svl::OscType::noiseWhite;
        for (int b : { 1, 512, 4096 })
        {
            o.block = b; o.tail = 0.1;
            auto s = render (p, { { 60, 100, 12345.0 / SR, 0.2 } }, o);
            long first = -1; for (size_t i = 0; i < s.l.size(); ++i) if (s.l[i] != 0) { first = (long) i; break; }
            R.line ("  note at sample 12345, block %4d: first non-zero sample %ld", b, first);
            R.check (first == 12345, "note starts on its exact sample");
        }
    }

    // 8. tuning: VA, fractional pitch, tuning table, string
    {
        R.line ("\n8. Pitch");
        auto toneAt = [&] (svl::ParamSet p, float pitch, double expect, double at)
        {
            RenderOpts o; o.tail = 0.1;
            auto s = render (p, { { pitch, 100, 0.0, at + 1.0 } }, o);
            return peakFreq (s.l.data() + (size_t) (at * SR), 32768, SR, expect);
        };
        svl::ParamSet sine = initParams(); sine[svl::oscParam (0, svl::O_TYPE)] = (float) svl::OscType::sine;
        const double fa = toneAt (sine, 69, 440, 0.2), fq = toneAt (sine, 60.5f, svl::pitchToHz (60.5), 0.2);
        R.line ("  sine A4: %.4f Hz (%.3f ct); pitch 60.5: %.4f Hz (%.3f ct from %.4f)", fa, cents (fa, 440), fq, cents (fq, svl::pitchToHz (60.5)), (double) svl::pitchToHz (60.5));
        R.check (std::fabs (cents (fa, 440)) < 0.5 && std::fabs (cents (fq, svl::pitchToHz (60.5))) < 0.5, "sine and fractional pitch within 0.5 cent");
        {   // tuning table: key 64 -> 63.86 (a lowered third), key 71 -> 70.84
            svl::Engine e; float tab[128]; for (int i = 0; i < 128; ++i) tab[i] = (float) i;
            tab[64] = 63.86f;
            e.setParams (sine, true); e.prepare (SR); e.setTuning (tab);
            Buf L (SR), Rr (SR); e.noteOn (64, 0.8f, 0); e.process (L.data(), Rr.data(), SR);
            const double f = peakFreq (L.data() + 8192, 32768, SR, svl::pitchToHz (63.86));
            R.line ("  tuning table key 64 -> 63.86: %.4f Hz (%.3f ct)", f, cents (f, svl::pitchToHz (63.86)));
            R.check (std::fabs (cents (f, svl::pitchToHz (63.86))) < 0.5, "tuning table applied within 0.5 cent");
        }
        svl::ParamSet ks = initParams();
        ks[svl::oscParam (0, svl::O_TYPE)] = (float) svl::OscType::string; ks[svl::FLT_CUTOFF] = 20000;
        ks[svl::oscParam (0, svl::O_KS_DECAY)] = 8; ks[svl::AENV_S] = 1;
        double worstKs = 0;
        std::string lineKs = "  string (tone 0.5):";
        for (int k : { 40, 52, 64, 76, 88 })
        {
            const double expect = svl::pitchToHz (k), f = toneAt (ks, (float) k, expect, 0.3);
            char b[64]; std::snprintf (b, sizeof b, " %d: %+.2f ct", k, cents (f, expect)); lineKs += b;
            worstKs = std::max (worstKs, std::fabs (cents (f, expect)));
        }
        R.line ("%s", lineKs.c_str());
        R.check (worstKs < 5, "string pitch within 5 cents (keys 40..88)");
    }

    // 8b. tuning map: normal / micro, every grid, keyswitches, reference
    {
        R.line ("\n8b. Tuning map");
        svl::ParamSet sine = initParams(); sine[svl::oscParam (0, svl::O_TYPE)] = (float) svl::OscType::sine;
        sine[svl::V_POLY] = 16;
        auto voicePitch = [&] (svl::Engine& e, int key)
        {
            float l[16], r[16];
            e.allNotesOff (0); e.process (l, r, 16);
            for (int b = 0; b < 2000; ++b) { e.process (l, r, 16); if (e.activeVoices (true) == 0) break; }
            e.noteOn (key, 0.8f, 0); e.process (l, r, 16);
            float p = -1; for (int v = 0; v < svl::kVoicePool; ++v) { auto vi = e.voiceInfo (v); if (vi.active && vi.gate) p = vi.pitch; }
            return p;
        };
        {   // normal mode ignores the rows
            svl::ParamSet p = sine; for (int pc = 0; pc < 12; ++pc) p[svl::TUNE_ROW0 + pc] = 0;
            svl::Engine e; e.setParams (p, true); e.prepare (SR);
            double worst = 0; for (int k = 21; k <= 108; ++k) worst = std::max (worst, std::fabs (voicePitch (e, k) - (double) k) * 100);
            R.line ("  normal mode, every row set low: keys 21..108 off their 12-TET pitch by at most %.4f ct", worst);
            R.check (worst < 0.1, "normal mode plays the usual notes");
        }
        double worstAll = 0;
        for (int edo : { 12, 24, 31, 41, 53, 72 })
        {
            double worst = 0;
            for (int rowSel : { 0, 2 })
            {
                svl::ParamSet p = sine; p[svl::TUNE_MODE] = 1; p[svl::TUNE_EDO] = (float) edo; p[svl::ROW_UP] = 2; p[svl::ROW_DOWN] = 1;
                for (int pc = 0; pc < 12; ++pc) p[svl::TUNE_ROW0 + pc] = (float) rowSel;
                svl::Engine e; e.setParams (p, true); e.prepare (SR);
                for (int k = 36; k <= 96; ++k)
                {
                    const double expect = k + (rowSel == 0 ? -1.0 : 2.0) * 12.0 / edo;
                    worst = std::max (worst, std::fabs (voicePitch (e, k) - expect) * 100);
                }
            }
            worstAll = std::max (worstAll, worst);
            R.line ("  micro, edo %2d, rows low (-1 step) / high (+2 steps), keys 36..96: max error %.4f ct", edo, worst);
        }
        R.check (worstAll < 0.1, "every key of every grid within 0.1 ct of its formula (engine pitch)");
        {   // audio: E4 on the low row of 53, and A4 at reference 432 Hz
            svl::ParamSet p = sine; p[svl::TUNE_MODE] = 1; p[svl::TUNE_ROW0 + 4] = 0;
            svl::Engine e; e.setParams (p, true); e.prepare (SR);
            Buf L (SR), Rr (SR); e.noteOn (64, 0.8f, 0); e.process (L.data(), Rr.data(), SR);
            const double fe = svl::pitchToHz (64.0 - 12.0 / 53.0), f = peakFreq (L.data() + 8192, 32768, SR, fe);
            p = sine; p[svl::TUNE_REF] = 432;
            svl::Engine e2; e2.setParams (p, true); e2.prepare (SR);
            e2.noteOn (69, 0.8f, 0); e2.process (L.data(), Rr.data(), SR);
            const double f2 = peakFreq (L.data() + 8192, 32768, SR, 432);
            R.line ("  audio: E4 one 53-step low %.4f Hz (%.3f ct off %.4f); A4 at reference 432: %.4f Hz (%.3f ct)", f, cents (f, fe), fe, f2, cents (f2, 432));
            R.check (std::fabs (cents (f, fe)) < 0.5 && std::fabs (cents (f2, 432)) < 0.5, "micro row and reference audible within 0.5 ct");
        }
        {   // keyswitches from a normal keyboard: key 28 (E in the "low" octave) toggles E low; 40 (E, "high" octave) toggles high
            svl::ParamSet p = sine; p[svl::TUNE_MODE] = 1; p[svl::KSW_MODE] = 1; p[svl::KSW_LOW] = 24;
            svl::Engine e; e.setParams (p, true); e.prepare (SR);
            float l[16], r[16];
            e.noteOn (28, 1, 0); e.process (l, r, 16);
            const int sounding = e.activeVoices (true);
            const float lowE = voicePitch (e, 64);
            e.noteOn (28, 1, 0); e.process (l, r, 16);
            const float midE = voicePitch (e, 64);
            e.noteOn (40, 1, 0); e.process (l, r, 16);
            const float highE = voicePitch (e, 64), highF = voicePitch (e, 65);
            R.line ("  keyswitch: key 28 -> voices sounding %d, E4 %.4f; again -> %.4f; key 40 -> E4 %.4f, F4 %.4f (unchanged)",
                    sounding, lowE, midE, highE, highF);
            R.check (sounding == 0 && std::fabs (lowE - (64 - 12.0f / 53)) < 1e-3f && std::fabs (midE - 64) < 1e-3f
                     && std::fabs (highE - (64 + 12.0f / 53)) < 1e-3f && std::fabs (highF - 65) < 1e-3f && e.tuningChangedByKeyswitch(),
                     "keyswitches move one pitch class and never sound");
        }
    }

    // 8c. unison, FM, LFO + modulation matrix
    {
        R.line ("\n8c. Unison, FM, LFOs and the modulation matrix");
        auto band = [&] (const Buf& x, size_t off, double f, double sr)   // dB of the strongest bin within +-3 % of f
        {
            const size_t N = 32768;
            auto P = powerSpectrum (x.data() + off, N);
            const double bin = sr / N; double m = 0;
            for (long b = (long) (f * 0.97 / bin); b <= (long) (f * 1.03 / bin); ++b) m = std::max (m, P[(size_t) b]);
            return 10 * std::log10 (m + 1e-30);
        };
        auto renderHeld = [&] (const svl::ParamSet& p, int key, int len, float wheel = 0)
        {
            svl::Engine e; e.setParams (p, true); e.prepare (SR); e.setModWheel (wheel);
            Stereo s; s.l.assign ((size_t) len, 0); s.r.assign ((size_t) len, 0);
            e.noteOn (key, 1.0f, 0);
            for (int i = 0; i < len; i += 256) e.process (s.l.data() + i, s.r.data() + i, std::min (256, len - i));
            return s;
        };
        svl::ParamSet sine = initParams(); sine[svl::oscParam (0, svl::O_TYPE)] = (float) svl::OscType::sine;
        sine[svl::FLT_CUTOFF] = 20000;
        {   // unison: 3 sine copies, detune 100 ct (the maximum) -> copies at -50 / 0 / +50 ct; level and width
            svl::ParamSet p = sine; p[svl::oscParam (0, svl::O_UNI)] = 3; p[svl::oscParam (0, svl::O_DETUNE)] = 100; p[svl::oscParam (0, svl::O_SPREAD)] = 0;   // spread 0: equal copies in each channel
            auto s = renderHeld (p, 69, 2 * SR);
            auto narrowPeak = [&] (double f)   // strongest bin within +-0.5 %, parabola-refined
            {
                const size_t N = 65536; auto P = powerSpectrum (s.l.data() + 8192, N); const double bin = (double) SR / N;
                long best = (long) (f * 0.995 / bin);
                for (long b = best; b <= (long) (f * 1.005 / bin); ++b) if (P[(size_t) b] > P[(size_t) best]) best = b;
                const double a = std::log (P[(size_t) best - 1] + 1e-30), c0 = std::log (P[(size_t) best] + 1e-30), c = std::log (P[(size_t) best + 1] + 1e-30);
                return std::make_pair (((double) best + 0.5 * (a - c) / (a - 2 * c0 + c)) * bin, 10 * std::log10 (P[(size_t) best] + 1e-30));
            };
            const auto lo = narrowPeak (440 * std::pow (2.0, -50 / 1200.0)), mid = narrowPeak (440), hi = narrowPeak (440 * std::pow (2.0, 50 / 1200.0));
            R.line ("  unison 3, detune 100 ct: copies at %.2f / %.2f / %.2f Hz (%.2f / %.2f / %.2f ct), levels %.1f / %.1f / %.1f dB",
                    lo.first, mid.first, hi.first, cents (lo.first, 440), cents (mid.first, 440), cents (hi.first, 440), lo.second, mid.second, hi.second);
            R.check (std::fabs (cents (lo.first, 440) + 50) < 0.5 && std::fabs (cents (mid.first, 440)) < 0.5 && std::fabs (cents (hi.first, 440) - 50) < 0.5
                     && std::fabs (lo.second - hi.second) < 1, "unison copies sit at +-half the detune, outer copies equal");
            svl::ParamSet q = initParams(); q[svl::FLT_CUTOFF] = 20000;
            auto one = renderHeld (q, 48, 4 * SR);
            q[svl::oscParam (0, svl::O_UNI)] = 7; q[svl::oscParam (0, svl::O_DETUNE)] = 40; q[svl::oscParam (0, svl::O_SPREAD)] = 1;
            auto seven = renderHeld (q, 48, 4 * SR);
            auto rmsDb = [] (const Buf& b) { double a = 0; for (size_t i = SR / 4; i < b.size(); ++i) a += b[i] * b[i]; return 10 * std::log10 (a / (b.size() - SR / 4)); };
            auto corr = [] (const Stereo& x) { double a = 0, b = 0, c = 0; for (size_t i = SR / 4; i < x.l.size(); ++i) { a += x.l[i] * x.r[i]; b += x.l[i] * x.l[i]; c += x.r[i] * x.r[i]; } return a / std::sqrt (b * c + 1e-30); };
            const double dl = 0.5 * (rmsDb (seven.l) + rmsDb (seven.r) - rmsDb (one.l) - rmsDb (one.r));
            R.line ("  saw C3, unison 7 / detune 40 / spread 1 vs unison 1 (0.25..4 s): level %+.2f dB, L/R correlation %.3f vs %.3f", dl, corr (seven), corr (one));
            R.check (std::fabs (dl) < 3 && corr (seven) < 0.9 && corr (one) > 0.999, "unison keeps the level within 3 dB and widens the image");
        }
        {   // FM: sine carrier A3 (220 Hz), osc 2 sine modulator two octaves up, index 0.5 -> J1/J0 = -11.76 dB at 5 f
            svl::ParamSet p = sine; p[svl::oscParam (1, svl::O_TYPE)] = (float) svl::OscType::sine; p[svl::oscParam (1, svl::O_LEVEL)] = 0;
            p[svl::oscParam (1, svl::O_OCT)] = 2; p[svl::FM_AMOUNT] = 0.5f;
            auto s = renderHeld (p, 57, SR);
            const double c0 = band (s.l, 8192, 220, SR), c1 = band (s.l, 8192, 1100, SR), c3 = band (s.l, 8192, 660, SR);
            R.line ("  FM index 0.5, modulator 4 x carrier: 5f %.2f dB, 3f %.2f dB re carrier (Bessel J1/J0 = -11.76 dB)", c1 - c0, c3 - c0);
            R.check (std::fabs (c1 - c0 + 11.76) < 1.0 && std::fabs (c3 - c0 + 11.76) < 1.0, "FM sidebands within 1 dB of the Bessel values");
            p[svl::FM_AMOUNT] = 0;
            auto z = renderHeld (p, 57, SR);
            R.line ("  FM index 0: 5f %.1f dB re carrier", band (z.l, 8192, 1100, SR) - band (z.l, 8192, 220, SR));
            R.check (band (z.l, 8192, 1100, SR) - band (z.l, 8192, 220, SR) < -80, "FM off leaves the carrier clean");
        }
        {   // LFO 1 square, synced 1/8 at 120 BPM (4 Hz) -> amp: level alternates every 125 ms by x0.5 / x1.5
            svl::ParamSet p = sine; p[svl::lfoParam (0, svl::L_SHAPE)] = (float) svl::LfoShape::square; p[svl::lfoParam (0, svl::L_SYNC)] = 6;
            p[svl::modParam (0, svl::M_SRC)] = (float) svl::ModSrc::lfo1; p[svl::modParam (0, svl::M_DST)] = (float) svl::ModDst::amp;
            p[svl::modParam (0, svl::M_AMT)] = 0.5f;
            auto s = renderHeld (p, 69, SR);
            auto win = [&] (double t0) { double a = 0; size_t i0 = (size_t) (t0 * SR); for (size_t i = i0; i < i0 + 2205; ++i) a += s.l[i] * s.l[i]; return 10 * std::log10 (a); };
            const double hi = win (0.03), lo = win (0.155), hi2 = win (0.28), lo2 = win (0.405);
            R.line ("  LFO square 1/8 @120 BPM -> amp 0.5: windows at 30/155/280/405 ms: %.2f / %.2f / %.2f / %.2f dB (step %.2f dB, expected 9.54)",
                    hi, lo, hi2, lo2, hi - lo);
            R.check (std::fabs (hi - lo - 9.54) < 0.3 && std::fabs (hi2 - lo2 - 9.54) < 0.3, "synced LFO rate and amount as set");
        }
        {   // mod wheel -> cutoff +4 oct: brighter with the wheel up
            svl::ParamSet p = initParams(); p[svl::FLT_CUTOFF] = 300;
            p[svl::modParam (3, svl::M_SRC)] = (float) svl::ModSrc::modWheel; p[svl::modParam (3, svl::M_DST)] = (float) svl::ModDst::cutoff;
            p[svl::modParam (3, svl::M_AMT)] = 0.5f;
            auto a = renderHeld (p, 48, SR, 0), b = renderHeld (p, 48, SR, 1);   // >= 4096 + 32768 samples: band() reads that far (was SR / 2: read past the end)
            const double h0 = band (a.l, 4096, 130.8 * 8, SR), h1 = band (b.l, 4096, 130.8 * 8, SR);
            R.line ("  mod wheel 0 -> 1 into cutoff (+4 oct): 8th harmonic %.1f -> %.1f dB", h0, h1);
            R.check (h1 - h0 > 20, "mod wheel opens the filter");
        }
        {   // block-size identity with unison, FM, LFOs, S&H, matrix
            svl::ParamSet p = initParams();
            p[svl::oscParam (0, svl::O_UNI)] = 5; p[svl::oscParam (0, svl::O_DETUNE)] = 30;
            p[svl::oscParam (1, svl::O_TYPE)] = (float) svl::OscType::sine; p[svl::FM_AMOUNT] = 1.5f; p[svl::FM_TARGET] = 2;
            p[svl::oscParam (2, svl::O_TYPE)] = (float) svl::OscType::triangle;
            p[svl::lfoParam (1, svl::L_SHAPE)] = (float) svl::LfoShape::sampleHold; p[svl::lfoParam (1, svl::L_RATE)] = 7;
            p[svl::lfoParam (2, svl::L_RETRIG)] = 0; p[svl::lfoParam (2, svl::L_FADE)] = 0.3f;
            const float slots[][3] = { { 1, 11, 0.3f }, { 2, 8, 0.5f }, { 3, 1, 0.02f }, { 10, 15, 0.8f }, { 5, 14, 0.4f } };
            for (int m = 0; m < 5; ++m) { p[svl::modParam (m, svl::M_SRC)] = slots[m][0]; p[svl::modParam (m, svl::M_DST)] = slots[m][1]; p[svl::modParam (m, svl::M_AMT)] = slots[m][2]; }
            std::vector<Note> nn; loadNotes (gRoot + "/notes/pluck.txt", nn);
            RenderOpts o; o.block = 512; o.tail = 2; auto ref = render (p, nn, o);
            double w = 0;
            for (int b : { 1, 77, 4096 })
            {
                o.block = b; auto x = render (p, nn, o);
                double d = x.l.size() == ref.l.size() ? 0 : 1;
                for (size_t i = 0; i < std::min (x.l.size(), ref.l.size()); ++i) d = std::max (d, (double) std::fabs (x.l[i] - ref.l[i]) + std::fabs (x.r[i] - ref.r[i]));
                w = std::max (w, d);
            }
            long bad = 0; for (float x : ref.l) if (! std::isfinite (x)) ++bad;
            R.line ("  unison + FM + LFOs + 5 matrix slots, blocks 1 / 77 / 4096 vs 512: max difference %.3g; non-finite %ld; peak %.1f dBFS", w, bad, peakDb (ref));
            R.check (w == 0 && bad == 0, "modulation output identical for every block size");
        }
    }

    // 8d. drum one-shot, velocity into click and pitch envelope, LFO up to 100 Hz
    {
        R.line ("\n8d. Drum one-shot, velocity to click / pitch envelope, fast LFO");
        auto hit = [&] (svl::ParamSet p, float vel, double len, double total)
        {
            RenderOpts o; o.tail = 0; o.minLen = total;
            return render (p, { { 60, vel * 127.0f, 0.0, len } }, o);
        };
        svl::ParamSet d = initParams();
        d[svl::oscParam (0, svl::O_TYPE)] = (float) svl::OscType::sine; d[svl::DRUM_MODE] = 1; d[svl::DRUM_PITCH] = 45;
        d[svl::AENV_A] = 0; d[svl::AENV_D] = 0.6f; d[svl::AENV_S] = 1; d[svl::AENV_R] = 0.02f;   // sustain 1: without one-shot it would hold
        auto lastSound = [&] (const Stereo& s) { long e = -1; for (size_t i = 0; i < s.l.size(); ++i) if (std::fabs (s.l[i]) > 1e-4f) e = (long) i; return e / (double) SR; };
        const double noOne = lastSound (hit (d, 1, 0.05, 2.0));
        d[svl::DRUM_ONESHOT] = 1;
        const double one = lastSound (hit (d, 1, 0.05, 2.0)), oneLong = lastSound (hit (d, 1, 1.5, 2.0));
        R.line ("  50 ms note, sustain 1: ends at %.3f s without one-shot, %.3f s with it (1.5 s note: %.3f s)", noOne, one, oneLong);
        R.check (noOne < 0.3 && one > 0.5 && std::fabs (one - oneLong) < 0.01, "one-shot ignores note-off and skips sustain (end incl. the DC blocker's tail)");
        // click: velocity 0.25 vs 1, first 4 ms, with click.velocity 1 and 0
        svl::ParamSet c = initParams(); c[svl::oscParam (0, svl::O_TYPE)] = (float) svl::OscType::off; c[svl::CLICK_LEVEL] = 1; c[svl::V_VELAMP] = 0;
        auto e4 = [&] (const Stereo& s) { double a = 0; for (int i = 0; i < SR / 250; ++i) a += s.l[(size_t) i] * s.l[(size_t) i]; return 10 * std::log10 (a + 1e-30); };
        c[svl::CLICK_VEL] = 1;
        const double dv1 = e4 (hit (c, 0.25f, 0.1, 0.1)) - e4 (hit (c, 1, 0.1, 0.1));
        c[svl::CLICK_VEL] = 0;
        const double dv0 = e4 (hit (c, 0.25f, 0.1, 0.1)) - e4 (hit (c, 1, 0.1, 0.1));
        R.line ("  click energy, velocity 0.25 vs 1: %.2f dB with click.velocity 1 (expected -12.04), %.2f dB with 0", dv1, dv0);
        R.check (std::fabs (dv1 + 12.04) < 0.5 && std::fabs (dv0) < 0.1, "click follows velocity as set");
        // pitch envelope: +24 st falling in 50 ms; zero crossings in the first 10 ms
        svl::ParamSet pe = d; pe[svl::DRUM_ONESHOT] = 0; pe[svl::PENV_AMT] = 24; pe[svl::PENV_DECAY] = 0.05f; pe[svl::PENV_VEL] = 1;
        auto zc = [&] (const Stereo& s) { int z = 0; for (int i = 1; i < SR / 100; ++i) if ((s.l[(size_t) i - 1] < 0) != (s.l[(size_t) i] < 0)) ++z; return z; };
        const int zSoft = zc (hit (pe, 0.2f, 0.2, 0.2)), zHard = zc (hit (pe, 1, 0.2, 0.2));
        pe[svl::PENV_VEL] = 0;
        const int zSoft0 = zc (hit (pe, 0.2f, 0.2, 0.2));
        R.line ("  pitch env +24 st, zero crossings in 10 ms: soft %d / hard %d with pitch_env.velocity 1; soft %d with 0", zSoft, zHard, zSoft0);
        R.check (zSoft < zHard && zSoft0 == zHard, "velocity scales the pitch envelope only when asked");
        // LFO at 100 Hz into amp
        svl::ParamSet lf = initParams(); lf[svl::oscParam (0, svl::O_TYPE)] = (float) svl::OscType::sine; lf[svl::lfoParam (0, svl::L_RATE)] = 100;
        lf[svl::lfoParam (0, svl::L_SHAPE)] = (float) svl::LfoShape::square;
        lf[svl::modParam (0, svl::M_SRC)] = 1; lf[svl::modParam (0, svl::M_DST)] = (float) svl::ModDst::amp; lf[svl::modParam (0, svl::M_AMT)] = 1;
        svl::Engine e; e.setParams (lf, true); e.prepare (SR);
        R.line ("  lfo1.rate set to 100 Hz -> engine value %.1f Hz", e.getParams()[svl::lfoParam (0, svl::L_RATE)]);
        R.check (std::fabs (e.getParams()[svl::lfoParam (0, svl::L_RATE)] - 100) < 1e-3f, "LFO reaches 100 Hz");
    }

    // 8e. FX rack: drive -> echo -> reverb
    {
        R.line ("\n8e. FX rack (drive -> echo -> reverb)");
        std::vector<Note> nn; loadNotes (gRoot + "/notes/pluck.txt", nn);
        RenderOpts o; o.tail = 2;
        // 1. bypass: with every mix 0 (and drive amount 0) the other FX knobs change nothing, bit for bit
        {
            svl::ParamSet a = presetParams (kPresetFiles[2]);
            a[svl::FXD_AMOUNT] = 0; a[svl::FXE_MIX] = 0; a[svl::FXR_MIX] = 0;   // the factory preset carries FX now
            svl::ParamSet b = a;
            b[svl::FXD_TONE] = 0.9f; b[svl::FXE_TIME] = 5; b[svl::FXE_FB] = 0.9f; b[svl::FXR_DECAY] = 9; b[svl::FXR_SIZE] = 1; b[svl::FXR_PRE] = 120;
            auto x = render (a, nn, o), y = render (b, nn, o);
            double d = x.l.size() == y.l.size() ? 0 : 1;
            for (size_t i = 0; i < std::min (x.l.size(), y.l.size()); ++i) d = std::max (d, (double) std::fabs (x.l[i] - y.l[i]) + std::fabs (x.r[i] - y.r[i]));
            R.line ("  all mixes 0, other FX knobs moved: max difference %.3g", d);
            R.check (d == 0, "FX rack off = bit-identical dry path");
        }
        // 2. echo timing: one click, 1/8 dotted at 120 BPM = 0.375 s; ping-pong: left first, right one step later
        {
            svl::ParamSet p = initParams(); p[svl::oscParam (0, svl::O_TYPE)] = (float) svl::OscType::off; p[svl::CLICK_LEVEL] = 1;
            p[svl::CLICK_DECAY] = 0.0005f; p[svl::FXE_MIX] = 1; p[svl::FXE_FB] = 0.5f; p[svl::FXE_DAMP] = 0;
            RenderOpts e; e.tail = 0; e.minLen = 1.2;
            auto s = render (p, { { 60, 127, 0.0, 0.01 } }, e);
            auto peakAt = [&] (const Buf& b, double t0, double t1) { size_t best = (size_t) (t0 * SR); for (size_t i = best; i < (size_t) (t1 * SR); ++i) if (std::fabs (b[i]) > std::fabs (b[best])) best = i; return (double) best / SR; };
            const double l1 = peakAt (s.l, 0.2, 0.55), r1 = peakAt (s.r, 0.55, 0.9);
            R.line ("  echo 1/8D @ 120 BPM: first left echo at %.4f s, first right echo at %.4f s (expected 0.375 / 0.750)", l1, r1);
            R.check (std::fabs (l1 - 0.375) < 0.002 && std::fabs (r1 - 0.750) < 0.002, "echo lands on the tempo grid, ping-pong");
        }
        // 3. stability: random FX settings incl. feedback 0.95 on noise bursts, 20 s; no NaN, no subnormal, the tail never grows
        {
            svl::Rng rng; rng.seed (11);
            long bad = 0, sub = 0; double worstGrowth = -999, worstPeak = -999;
            for (int t = 0; t < 60; ++t)
            {
                svl::ParamSet p = initParams(); p[svl::oscParam (0, svl::O_TYPE)] = (float) svl::OscType::noiseWhite;
                p[svl::AENV_R] = 0.05f;
                auto u = [&] { return 0.5f + 0.5f * rng.uni(); };
                p[svl::FXD_AMOUNT] = u(); p[svl::FXD_TONE] = u(); p[svl::FXD_MIX] = u();
                p[svl::FXE_TIME] = std::floor (u() * 5.99f); p[svl::FXE_FB] = t < 20 ? 0.95f : 0.95f * u(); p[svl::FXE_DAMP] = u(); p[svl::FXE_MIX] = u();
                p[svl::FXR_SIZE] = u(); p[svl::FXR_DECAY] = 0.2f + 11.8f * u(); p[svl::FXR_PRE] = 200 * u(); p[svl::FXR_DAMP] = u(); p[svl::FXR_MIX] = u();
                RenderOpts e; e.tail = 0; e.minLen = 20;
                auto s = render (p, { { 60, 127, 0.0, 0.3 }, { 67, 100, 0.5, 0.3 } }, e);
                float pk02 = 0, pkLate = 0;
                for (size_t i = 0; i < s.l.size(); ++i)
                    for (float v : { s.l[i], s.r[i] })
                    {
                        if (! std::isfinite (v)) ++bad; else if (v != 0 && std::fabs (v) < 1.17549435e-38f) ++sub;
                        if (i < (size_t) (2 * SR)) pk02 = std::max (pk02, std::fabs (v)); else if (i > (size_t) (10 * SR)) pkLate = std::max (pkLate, std::fabs (v));
                    }
                worstGrowth = std::max (worstGrowth, 20 * std::log10 ((pkLate + 1e-12) / (pk02 + 1e-12)));
                worstPeak = std::max (worstPeak, 20 * std::log10 (pk02 + 1e-12));
            }
            R.line ("  60 random FX settings (20 with feedback 0.95), 20 s each: %ld non-finite, %ld subnormal; worst tail (10-20 s) vs first 2 s: %+.1f dB; worst peak %.1f dBFS",
                    bad, sub, worstGrowth, worstPeak);
            R.check (bad == 0 && sub == 0 && worstGrowth < 0, "FX stable at feedback 0.95, no NaN or denormals");
        }
        // 4. reverb T60: impulse into the reverb only (mix 1), Schroeder curve of the wet, -5 .. -25 dB fit
        {
            for (float t60 : { 1.0f, 2.0f, 6.0f })
            {
                svl::ParamSet p = initParams(); p[svl::oscParam (0, svl::O_TYPE)] = (float) svl::OscType::off; p[svl::CLICK_LEVEL] = 1;
                p[svl::CLICK_DECAY] = 0.002f; p[svl::CLICK_HP] = 1000; p[svl::FXR_MIX] = 1; p[svl::FXR_DECAY] = t60; p[svl::FXR_PRE] = 0;
                RenderOpts e; e.tail = 0; e.minLen = t60 * 2.0 + 1.0;
                auto s = render (p, { { 60, 127, 0.0, 0.01 } }, e);
                // the knob is the mid band's T60 (damping shortens the top on purpose): measure in the 1 kHz band
                svl::Svf bp; bp.setCoeffs (1000.0f, 0.3f, (float) SR);
                Buf band (s.l.size());
                for (size_t i = 0; i < s.l.size(); ++i) band[i] = bp.process (0, 0.5f * (s.l[i] + s.r[i]), svl::FilterMode::bp);
                std::vector<double> sch (s.l.size());
                double acc = 0; for (size_t i = band.size(); i-- > 0;) { acc += (double) band[i] * band[i]; sch[i] = acc; }
                auto tAt = [&] (double db) { for (size_t i = 0; i < sch.size(); ++i) if (10 * std::log10 (sch[i] / sch[0]) < db) return (double) i / SR; return -1.0; };
                const double t5 = tAt (-5), t25 = tAt (-25), est = (t25 - t5) * 3.0;
                R.line ("  reverb decay %.1f s: measured T60 %.2f s in the 1 kHz band (from the -5..-25 dB slope)", t60, est);
                R.check (est > t60 * 0.7 && est < t60 * 1.35, "reverb T60 within -30 / +35 % of the knob");
            }
        }
        // 5. drive: small-signal gain about 1, harmonics grow with level
        {
            for (float lvl : { -40.0f, -6.0f })
            {
                svl::ParamSet p = initParams(); p[svl::oscParam (0, svl::O_TYPE)] = (float) svl::OscType::sine; p[svl::FLT_CUTOFF] = 20000;
                p[svl::MASTER] = lvl + 3.0f; p[svl::V_VELAMP] = 0;
                RenderOpts e; e.tail = 0; e.minLen = 1.0;
                auto dry = render (p, { { 57, 127, 0.0, 1.0 } }, e);
                p[svl::FXD_AMOUNT] = 0.5f;
                auto wet = render (p, { { 57, 127, 0.0, 1.0 } }, e);
                const auto P = powerSpectrum (wet.l.data() + 8192, 16384);
                const auto D = powerSpectrum (dry.l.data() + 8192, 16384);
                const double bin = (double) SR / 16384;
                auto at = [&] (const std::vector<double>& q, double f) { double m = 0; for (long b = (long) (f / bin) - 3; b <= (long) (f / bin) + 3; ++b) m = std::max (m, q[(size_t) b]); return 10 * std::log10 (m + 1e-30); };
                R.line ("  drive 0.5 (12 dB) at %.0f dBFS: fundamental %+.2f dB vs dry, 2nd harmonic %.1f dBc, 3rd %.1f dBc",
                        lvl, at (P, 220) - at (D, 220), at (P, 440) - at (P, 220), at (P, 660) - at (P, 220));
                if (lvl < -20) R.check (std::fabs (at (P, 220) - at (D, 220)) < 1.0, "drive keeps the small-signal level (within 1 dB)");
            }
            // a light drive mixed with the dry must not cancel it: the wet's phase has to stay close to the dry's (18 Oct: the
            // 3rd-order shelves turned 60 Hz by -75 deg and a 50 % mix lost 5 dB on every bass)
            double worst = 0;
            for (float f : { 40.0f, 60.0f, 110.0f, 250.0f, 1000.0f })   // above that the drive's top roll (by design) lowers the level
            {
                svl::ParamSet p = initParams(); p[svl::oscParam (0, svl::O_TYPE)] = (float) svl::OscType::sine; p[svl::FLT_CUTOFF] = 20000;
                p[svl::MASTER] = -20; p[svl::V_VELAMP] = 0;
                const float key = 69.0f + 12.0f * std::log2 (f / 440.0f);
                RenderOpts e; e.tail = 0; e.minLen = 1.0;
                auto dry = render (p, { { key, 127, 0.0, 1.0 } }, e);
                p[svl::FXD_AMOUNT] = 0.05f; p[svl::FXD_MIX] = 0.5f;
                auto wet = render (p, { { key, 127, 0.0, 1.0 } }, e);
                double a = 0, b = 0;
                for (int i = 16384; i < 32768; ++i) { a += dry.l[(size_t) i] * dry.l[(size_t) i]; b += wet.l[(size_t) i] * wet.l[(size_t) i]; }
                worst = std::max (worst, std::fabs (10 * std::log10 (b / a)));
                R.line ("  drive 0.05 at mix 0.5, %5.0f Hz: %+.2f dB", f, 10 * std::log10 (b / a));
            }
            R.check (worst < 0.5, "a half-mixed light drive keeps the level (no phase cancellation, within 0.5 dB)");
        }
        // 5b. a reverb / echo tail rendered past the notes is the same for every block size (18 Oct: the block cut at the end of
        //     the notes skipped up to block - 1 samples, so the tail differed from the plug-in's)
        {
            svl::ParamSet p = initParams(); p[svl::FXR_MIX] = 0.3f; p[svl::FXR_DECAY] = 3; p[svl::FXE_MIX] = 0.2f;
            RenderOpts a; a.tail = 0.3; a.maxExtra = 20; a.block = 512;
            RenderOpts b = a; b.block = 37;
            RenderOpts c = a; c.block = 1;
            const std::vector<Note> nt = { { 60, 100, 0.0, 0.3 }, { 67, 100, 0.13, 0.2 } };
            const auto x = render (p, nt, a), y = render (p, nt, b), z = render (p, nt, c);
            long diff = 0;
            for (size_t i = 0; i < std::min ({ x.l.size(), y.l.size(), z.l.size() }); ++i) diff += x.l[i] != y.l[i] || x.l[i] != z.l[i];
            R.line ("  FX tail past the notes, blocks 512 / 37 / 1: lengths %zu %zu %zu, %ld samples differ", x.l.size(), y.l.size(), z.l.size(), diff);
            R.check (diff == 0 && x.l.size() == y.l.size() && x.l.size() == z.l.size() && x.l.size() > 44100, "FX tail identical for every block size");
        }
        // 6. CPU of the rack: the same 16-voice saw chord with the rack off and with each stage on
        {
            auto cost = [&] (int which)
            {
                svl::ParamSet p = initParams();
                if (which == 1) p[svl::FXD_AMOUNT] = 0.3f;
                if (which == 2) p[svl::FXE_MIX] = 0.3f;
                if (which == 3) p[svl::FXR_MIX] = 0.3f;
                svl::Engine e; e.setParams (p, true); e.prepare (SR);
                Buf L (512), Rr (512);
                for (int k = 0; k < 16; ++k) e.noteOn (36 + k * 3, 0.8f, 0);
                const int blocks = 10 * SR / 512;
                auto t0 = std::chrono::steady_clock::now();
                for (int b = 0; b < blocks; ++b) e.process (L.data(), Rr.data(), 512);
                return 100.0 * std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count() / (blocks * 512.0 / SR);
            };
            const double off = cost (0), d = cost (1), ec = cost (2), rv = cost (3);
            R.line ("  CPU (one core, 16 voices): rack off %.2f %%, + drive %.2f %%, + echo %.2f %%, + reverb %.2f %% (stage alone: %.2f / %.2f / %.2f points)",
                    off, d, ec, rv, d - off, ec - off, rv - off);
        }
    }

    // 8f. arpeggiator + step sequencer (SvlArpSeq.h)
    {
        R.line ("\n8f. Arpeggiator + step sequencer");
        // the generator alone, driven as the engine drives it: pop what is due, advance to the next due sample (or the block end)
        struct Act { long at; int type; int key; float vel; };   // type 0 on, 1 off, 2 all off
        struct Rec { long t; svl::GenNote g; };
        auto runGen = [] (svl::ParamSet p, double bpm, long total, int block, std::vector<Act> acts,
                          bool host = false, double ppq0 = 0)
        {
            svl::NoteGen g; g.prepare (44100); g.setTempo (bpm); g.setParams (p);
            std::vector<Rec> out; size_t a = 0; long pos = 0;
            const double spb = 44100.0 * 60.0 / bpm;
            while (pos < total)
            {
                const long blockEnd = std::min (total, (pos / block + 1) * block);
                if (pos % block == 0 && host) g.setTransport (true, true, ppq0 + pos / spb);
                while (a < acts.size() && acts[a].at <= pos)
                {
                    const Act& x = acts[a++];
                    if (x.type == 0) g.keyOn (x.key, x.key, 0, x.vel); else if (x.type == 1) g.keyOff (x.key); else g.allOff();
                }
                svl::GenNote n;
                while (g.pop (n)) out.push_back ({ pos, n });
                long end = std::min (blockEnd, pos + std::max (1, g.samplesToNext()));
                if (a < acts.size()) end = std::min (end, std::max (pos + 1, acts[a].at));
                g.advance ((int) (end - pos)); pos = end;
            }
            return out;
        };
        auto seqAll = [] (int rate, float gate = 0.6f)
        {
            svl::ParamSet p = initParams(); p[svl::SEQ_ON] = 1; p[svl::SEQ_MODE] = 1; p[svl::SEQ_RATE] = (float) rate;
            for (int s = 0; s < 16; ++s) p[svl::stepParam (s, svl::S_GATE)] = gate;
            return p;
        };
        // 1. onsets on the grid, every rate, 120 and 97 BPM, 4 bars, several block sizes
        {
            long worst = 0; int bad = 0, total = 0;
            for (double bpm : { 120.0, 97.0 })
                for (int rate = 0; rate < 6; ++rate)
                    for (int block : { 1, 64, 512, 4096 })
                    {
                        const double spb = 44100.0 * 60.0 / bpm, step = svl::kStepRateBeats[rate];
                        const long len = (long) std::ceil (16 * spb);
                        auto ev = runGen (seqAll (rate), bpm, len, block, {});
                        long k = 0;
                        for (auto& r : ev)
                            if (r.g.kind != svl::GenNote::off)
                            {
                                const long want = (long) std::ceil (k * step * spb - 1e-6);
                                worst = std::max (worst, std::labs (r.t - want)); ++k; ++total;
                            }
                        if (k != (long) std::ceil (16.0 / step - 1e-9)) ++bad;
                    }
            R.line ("  6 rates x 120 / 97 BPM x blocks 1..4096: %d onsets, worst %ld samples off the grid, %d runs with a wrong count",
                    total, worst, bad);
            R.check (worst == 0 && bad == 0, "every step on its sample at every rate and tempo, any block size");
        }
        // 2. swing and gate
        {
            svl::ParamSet p = seqAll (3, 0.5f); p[svl::SEQ_SWING] = 0.5f;
            const double spb = 44100.0 * 60.0 / 120.0, step = 0.25;
            auto ev = runGen (p, 120, (long) (4 * spb), 512, {});
            long k = 0, worstOn = 0, worstGate = 0;
            std::map<int, long> onAt;
            for (auto& r : ev)
            {
                if (r.g.kind != svl::GenNote::off)
                {
                    const double b = k * step + ((k & 1) ? 0.5 * 0.5 * step : 0.0);
                    worstOn = std::max (worstOn, std::labs (r.t - (long) std::ceil (b * spb - 1e-6)));
                    onAt[r.g.id] = r.t; ++k;
                }
                else if (onAt.count (r.g.id))
                {   // gate 0.5 of the step's own length (even steps 1.25, odd 0.75 x the step)
                    const long on = onAt[r.g.id];
                    const long kk = std::distance (onAt.begin(), onAt.find (r.g.id));
                    const double dur = step * ((kk & 1) ? 0.75 : 1.25);
                    worstGate = std::max (worstGate, std::labs ((r.t - on) - std::llround (0.5 * dur * spb)));
                }
            }
            R.line ("  swing 0.5 at 1/16: odd steps a quarter step late, worst %ld samples; gate 0.5 of the swung step: worst %ld samples",
                    worstOn, worstGate);
            R.check (worstOn == 0 && worstGate <= 1, "swing and gate as set");
        }
        // 3. arp order (keys 60 64 67, 2 octaves), latch, hold transposition
        {
            auto arpKeys = [&] (int mode, int steps, bool latch = false, std::vector<Act> extra = {})
            {
                svl::ParamSet p = initParams(); p[svl::ARP_ON] = 1; p[svl::ARP_MODE] = (float) mode; p[svl::ARP_OCT] = 2;
                p[svl::ARP_LATCH] = latch ? 1.0f : 0.0f;
                std::vector<Act> acts = { { 0, 0, 64, 0.8f }, { 0, 0, 60, 0.8f }, { 0, 0, 67, 0.8f } };
                for (auto& x : extra) acts.push_back (x);
                auto ev = runGen (p, 120, (long) std::floor (steps * 0.25 * 22050), 256, acts);
                std::string s;
                for (auto& r : ev) if (r.g.kind != svl::GenNote::off) s += std::to_string (r.g.key + (int) std::lround (r.g.add)) + " ";   // octaves ride in add
                return s;
            };
            const std::string up = arpKeys (0, 7), down = arpKeys (1, 7), ud = arpKeys (2, 11), du = arpKeys (3, 11), pl = arpKeys (6, 7),
                              ch = arpKeys (5, 2);
            R.line ("  up: %s| down: %s| updown: %s| downup: %s| played: %s| chord: %s", up.c_str(), down.c_str(), ud.c_str(), du.c_str(),
                    pl.c_str(), ch.c_str());
            R.check (up == "60 64 67 72 76 79 60 " && down == "79 76 72 67 64 60 79 " && ud == "60 64 67 72 76 79 76 72 67 64 60 "
                     && du == "79 76 72 67 64 60 64 67 72 76 79 " && pl == "64 60 67 76 72 79 64 " && ch == "60 64 67 72 76 79 ",
                     "arp order: up, down, updown, downup, played, chord over 2 octaves");
            // latch: release everything after 3 steps, the arp goes on; a new key after that replaces the chord
            auto at = [] (int k) { return (long) std::ceil (k * 5512.5 - 1e-6); };   // step k of 1/16 at 120 BPM (5512.5 samples)
            const std::string lat = arpKeys (0, 9, true, { { at (2), 1, 60, 0 }, { at (2), 1, 64, 0 }, { at (2), 1, 67, 0 }, { at (6), 0, 62, 0.8f } });
            const std::string nolat = arpKeys (0, 9, false, { { at (2), 1, 60, 0 }, { at (2), 1, 64, 0 }, { at (2), 1, 67, 0 } });
            R.line ("  latch: %s| no latch: %s", lat.c_str(), nolat.c_str());
            R.check (lat == "60 64 67 72 76 79 62 74 62 " && nolat == "60 64 ", "latch keeps the keys; a new key after release starts a new set");
            // seq hold: the pattern 0 +3 +7 +12 follows the last key held
            svl::ParamSet p = initParams(); p[svl::SEQ_ON] = 1; p[svl::SEQ_LEN] = 4;
            const int notes[4] = { 0, 3, 7, 12 };
            for (int s = 0; s < 4; ++s) p[svl::stepParam (s, svl::S_NOTE)] = (float) notes[s];
            p[svl::stepParam (1, svl::S_ACCENT)] = 1; p[svl::stepParam (2, svl::S_VEL)] = 0.5f;
            auto ev = runGen (p, 120, (long) std::floor (8 * 5512.5), 256, { { 0, 0, 48, 1 }, { at (4), 0, 55, 1 }, { at (6), 1, 55, 0 } });
            std::string s; float v0 = 0, v1 = 0, v2 = 0; int i = 0;
            for (auto& r : ev)
                if (r.g.kind != svl::GenNote::off) { s += std::to_string (r.g.key) + " "; (i == 0 ? v0 : i == 1 ? v1 : i == 2 ? v2 : v0) = i < 3 ? r.g.vel : v0; ++i; }
            R.line ("  seq hold, key 48 then 55 (released after 2 steps): %s; velocity 0.8 / accent / 0.5: %.2f %.2f %.2f", s.c_str(), v0, v1, v2);
            R.check (s == "48 51 55 60 55 58 55 60 ", "seq hold: transposed by the last key held, back when it is released");
            R.check (std::fabs (v0 - 0.8f) < 1e-6f && std::fabs (v1 - 1.0f) < 1e-6f && std::fabs (v2 - 0.5f) < 1e-6f, "step velocity and accent (+0.25, at most 1)");
        }
        // 3b. own clock and detached keys: within a bar of the last stop the pattern keeps its place (an off-beat pattern
        //     played with short notes still sounds); after a longer pause the first step plays at the key
        {
            svl::ParamSet p = initParams(); p[svl::SEQ_ON] = 1;
            for (int s = 0; s < 16; ++s) p[svl::stepParam (s, svl::S_ON)] = s % 4 == 2 ? 1.0f : 0.0f;   // the off-beats only
            std::vector<Act> acts;
            for (int i = 0; i < 8; ++i) { acts.push_back ({ (long) (i * 11025), 0, 48, 1 }); acts.push_back ({ (long) (i * 11025 + 8820), 1, 48, 0 }); }
            int offbeats = 0;
            for (auto& r : runGen (p, 120, 8 * 11025, 512, acts)) if (r.g.kind != svl::GenNote::off) ++offbeats;
            svl::ParamSet q = seqAll (3); q[svl::SEQ_MODE] = 0;   // every step on, hold
            const long late = 51597 + 92610 + 37;   // 2.1 s (+37 samples, off the grid) after the second key went up: more than a bar
            auto ev = runGen (q, 120, 4 * 44100, 512, { { 0, 0, 48, 1 }, { 4410, 1, 48, 0 }, { 47187, 0, 48, 1 }, { 47187 + 4410, 1, 48, 0 },
                                                         { late, 0, 50, 1 }, { late + 4410, 1, 50, 0 } });
            long withinBar = -1, afterPause = -1;
            for (auto& r : ev)
                if (r.g.kind != svl::GenNote::off)
                {
                    if (withinBar < 0 && r.t >= 47187) withinBar = r.t;
                    if (r.t >= late && r.g.key == 50 && afterPause < 0) afterPause = r.t;
                }
            R.line ("  off-beat pattern, eight 0.2 s notes every 0.25 s: %d notes (4 off-beats; 0 before 18 Oct); key back at 1.07 s: first step at %ld (grid 49613),"
                    " key after a 2.1 s pause: first step at %ld (key at %ld)", offbeats, withinBar, afterPause, late);
            R.check (offbeats == 4 && withinBar == 49613 && afterPause == late, "detached keys keep the bar position; a long pause restarts at the key");
        }
        // 3d. arp.chord: one held key plays the chord shape, in the mode's order, over the octaves
        {
            auto chordKeys = [&] (int chord, int mode, int oct, int steps)
            {
                svl::ParamSet p = initParams(); p[svl::ARP_ON] = 1; p[svl::ARP_CHORD] = (float) chord; p[svl::ARP_MODE] = (float) mode;
                p[svl::ARP_OCT] = (float) oct;
                auto ev = runGen (p, 120, (long) std::floor (steps * 5512.5), 256, { { 0, 0, 60, 0.8f } });
                std::string s;
                for (auto& r : ev) if (r.g.kind != svl::GenNote::off) s += std::to_string (r.g.key + (int) std::lround (r.g.add)) + " ";
                return s;
            };
            const std::string maj = chordKeys (3, 0, 1, 4), min7 = chordKeys (8, 1, 1, 5), sus4 = chordKeys (6, 2, 2, 10), off = chordKeys (0, 0, 1, 3),
                              ch = chordKeys (3, 5, 1, 1);
            R.line ("  key 60: maj up %s| min7 down %s| sus4 updown 2 oct %s| off %s| maj chord mode %s", maj.c_str(), min7.c_str(), sus4.c_str(), off.c_str(), ch.c_str());
            R.check (maj == "60 64 67 60 " && min7 == "70 67 63 60 70 " && sus4 == "60 65 67 72 77 79 77 72 67 65 " && off == "60 60 60 " && ch == "60 64 67 ",
                     "arp.chord: one key plays the chord shape in the mode's order");
        }
        // 3c. arp octaves are octaves in any tuning table (Tuning Lab): a 24-step table, key 62 = 61.0, 2 octaves -> 61.0 then 73.0
        {
            svl::ParamSet p = initParams(); p[svl::ARP_ON] = 1; p[svl::ARP_OCT] = 2; p[svl::ARP_RATE] = 3; p[svl::AENV_R] = 0.01f;
            svl::Engine e; e.setParams (p, true); e.prepare (SR);
            float table[128]; for (int k = 0; k < 128; ++k) table[k] = 60.0f + 0.5f * (float) (k - 60);
            e.setTuning (table);
            Buf L (64), Rr (64);
            e.noteOn (62, 0.8f, 0);
            std::vector<float> seen;
            for (long t = 0; t < 4 * 5513; t += 64)
            {
                e.process (L.data(), Rr.data(), 64);
                for (int v = 0; v < svl::kVoicePool; ++v)
                {
                    const auto vi = e.voiceInfo (v);
                    if (vi.gate && (seen.empty() || std::fabs (seen.back() - vi.pitch) > 1e-4f)) seen.push_back (vi.pitch);
                }
            }
            std::string sp; for (float x : seen) { char b[16]; std::snprintf (b, sizeof b, "%.3f ", x); sp += b; }
            R.line ("  24-step table, key 62, arp 2 octaves: pitches %s", sp.c_str());
            R.check (seen.size() >= 3 && std::fabs (seen[0] - 61) < 1e-4f && std::fabs (seen[1] - 73) < 1e-4f && std::fabs (seen[2] - 61) < 1e-4f,
                     "arp octaves are 12 semitones in any tuning table");
        }
        // 4. slide: the note holds into the next and the voice glides (engine), rows add grid steps
        {
            struct Out { bool retrig; int maxV; float pitchMid, pitchEnd; };
            auto run = [&] (bool slide)
            {
                svl::ParamSet p = initParams(); p[svl::SEQ_ON] = 1; p[svl::SEQ_MODE] = 1; p[svl::SEQ_LEN] = 2;
                p[svl::stepParam (0, svl::S_SLIDE)] = slide ? 1.0f : 0.0f; p[svl::stepParam (1, svl::S_NOTE)] = 7;
                p[svl::stepParam (1, svl::S_ROW)] = 2; p[svl::AENV_A] = 0.01f;
                p[svl::V_MODE] = (float) svl::VoiceMode::mono;
                svl::Engine e; e.setParams (p, true); e.prepare (SR);
                Buf L (64), Rr (64);
                Out o { false, 0, 0, 0 };
                const long step = 5513;
                for (long t = 0; t < 2 * step - 200; t += 64)
                {
                    e.process (L.data(), Rr.data(), 64);
                    o.maxV = std::max (o.maxV, e.activeVoices (false));
                    const auto v = e.voiceInfo (0);
                    if (t > step + 64 && t < step + 400 && v.ampStage == svl::Adsr::attack) o.retrig = true;
                    if (t >= step + 640 && o.pitchMid == 0) o.pitchMid = v.pitch;   // 15 ms into the 60 ms glide
                    o.pitchEnd = v.pitch;
                }
                return o;
            };
            const Out a = run (true), b = run (false);
            const float want = 67.0f + 1.0f * 12.0f / 53.0f;
            R.line ("  slide into step 2 (+7, high row = +1 of 53): mid-glide %.3f, end %.4f (want %.4f), voices %d, retrigger %s;"
                    " without slide: retrigger %s, %.3f at the same moment", a.pitchMid, a.pitchEnd, want, a.maxV, a.retrig ? "yes" : "no",
                    b.retrig ? "yes" : "no", b.pitchMid);
            R.check (std::fabs (a.pitchEnd - want) < 1e-3f && a.maxV == 1 && ! a.retrig && a.pitchMid > 60.5f && a.pitchMid < want - 0.5f
                     && b.retrig && std::fabs (b.pitchMid - want) < 1e-3f, "slide glides without a retrigger (a plain step retriggers); the row adds its grid step");
        }
        // 5. host grid: the transport starts at beat 3.9, steps sit on the bar grid; a jump back (loop) re-aligns
        {
            svl::ParamSet p = seqAll (1);
            const double spb = 22050;
            auto ev = runGen (p, 120, (long) (2 * spb), 512, {}, true, 3.9);
            long worst = 0; int n = 0;
            for (auto& r : ev)
                if (r.g.kind != svl::GenNote::off)
                {
                    const double b = 3.9 + r.t / spb;
                    worst = std::max (worst, (long) std::lround (std::fabs (b - std::round (b * 2) / 2) * spb)); ++n;
                }
            R.line ("  host transport from beat 3.9, 1/8: %d onsets, worst %ld samples off the host's grid, first at beat %.4f", n, worst,
                    ev.empty() ? -1.0 : 3.9 + ev[0].t / spb);
            R.check (n == 4 && worst <= 1 && ! ev.empty() && std::fabs (3.9 + ev[0].t / spb - 4.0) < 1e-4, "steps on the host's bar grid");
        }
        // 6. no stuck notes: random keys, switches, rates, transport jumps, all-off; then everything must end
        {
            svl::Engine e; svl::ParamSet p = initParams(); p[svl::AENV_R] = 0.05f;
            e.setParams (p, true); e.prepare (SR);
            Buf L (256), Rr (256);
            uint32_t r = 12345; auto rnd = [&] (int n) { r = r * 1664525u + 1013904223u; return (int) ((r >> 8) % (uint32_t) n); };
            double ppq = 0; bool playing = false;
            gAllocs = 0; gCountAllocs = true;
            for (int b = 0; b < 4000; ++b)
            {
                const int what = rnd (100);
                if (what < 20) e.noteOn (40 + rnd (40), 0.8f, rnd (256));
                else if (what < 40) e.noteOff (40 + rnd (40), rnd (256));
                else if (what < 44) e.setParam (svl::ARP_ON, (float) rnd (2));
                else if (what < 48) e.setParam (svl::SEQ_ON, (float) rnd (2));
                else if (what < 50) e.setParam (svl::SEQ_MODE, (float) rnd (2));
                else if (what < 52) e.setParam (svl::ARP_LATCH, (float) rnd (2));
                else if (what < 55) e.setParam (svl::ARP_MODE, (float) rnd (7));
                else if (what < 58) { e.setParam (svl::ARP_RATE, (float) rnd (6)); e.setParam (svl::SEQ_RATE, (float) rnd (6)); }
                else if (what < 60) e.setParam (svl::stepParam (rnd (16), svl::S_GATE), 0.05f + 3.0f * (float) rnd (100) / 100.0f);
                else if (what < 62) e.setParam (svl::stepParam (rnd (16), svl::S_SLIDE), (float) rnd (2));
                else if (what < 64) e.setParam (svl::V_MODE, (float) rnd (3));
                else if (what < 66) { playing = ! playing; }
                else if (what < 67) ppq = rnd (32);
                else if (what < 68) e.allNotesOff (rnd (256));
                else if (what < 69) e.setTempo (60 + rnd (140));
                e.setTransport (true, playing, ppq);
                e.process (L.data(), Rr.data(), 256);
                ppq += 256.0 / 22050.0;
            }
            gCountAllocs = false;
            const long allocs = gAllocs.load();
            for (int k = 0; k < 128; ++k) e.noteOff (k, 0);
            e.setParam (svl::ARP_LATCH, 0); e.setParam (svl::SEQ_MODE, 0);
            for (int b = 0; b < 400; ++b) { e.setTransport (true, false, ppq); e.process (L.data(), Rr.data(), 256); }
            const int left1 = e.activeVoices (true), gen1 = e.noteGen().soundingNotes();
            e.setParam (svl::ARP_ON, 1); e.setParam (svl::SEQ_ON, 1);
            for (int b = 0; b < 400; ++b) e.process (L.data(), Rr.data(), 256);
            e.setParam (svl::ARP_ON, 0); e.setParam (svl::SEQ_ON, 0);
            for (int b = 0; b < 400; ++b) e.process (L.data(), Rr.data(), 256);
            const int left2 = e.activeVoices (true), gen2 = e.noteGen().soundingNotes();
            R.line ("  4000 random blocks, then all keys up: %d voices / %d generated notes left; after switching both on and off: %d / %d;"
                    " heap allocations during the random blocks: %ld", left1, gen1, left2, gen2, allocs);
            R.check (left1 == 0 && gen1 == 0 && left2 == 0 && gen2 == 0, "no stuck notes");
            R.check (allocs == 0, "arp / seq: no heap allocation on the audio thread");
        }
        // 7. the engine's output with the seq does not depend on the block size; CPU of the generator
        {
            svl::ParamSet p = presetParams ("presets/Bass/Saw Bass.xml");
            p[svl::SEQ_ON] = 1; p[svl::SEQ_MODE] = 1; p[svl::SEQ_SWING] = 0.3f;
            for (int s = 0; s < 16; ++s)
            {
                p[svl::stepParam (s, svl::S_NOTE)] = (float) ((s * 5) % 13 - 6); p[svl::stepParam (s, svl::S_ON)] = s % 5 != 3 ? 1.0f : 0.0f;
                p[svl::stepParam (s, svl::S_SLIDE)] = s % 4 == 1 ? 1.0f : 0.0f; p[svl::stepParam (s, svl::S_GATE)] = s % 3 == 0 ? 1.5f : 0.4f;
            }
            p[svl::V_MODE] = (float) svl::VoiceMode::legato;
            RenderOpts o1; o1.tail = 0.5; o1.minLen = 3; o1.block = 512; o1.tempo = 97;
            RenderOpts o2 = o1; o2.block = 37;
            RenderOpts o3 = o1; o3.block = 1;
            const auto a = render (p, {}, o1), b = render (p, {}, o2), c = render (p, {}, o3);
            long diff = 0;
            for (size_t i = 0; i < std::min ({ a.l.size(), b.l.size(), c.l.size() }); ++i) diff += a.l[i] != b.l[i] || a.l[i] != c.l[i];
            double rms = 0; for (float x : a.l) rms += (double) x * x;
            R.line ("  free seq, swing, slides, ties, legato, 97 BPM: blocks 512 / 37 / 1 differ in %ld samples (lengths %zu %zu %zu), rms %.1f dBFS",
                    diff, a.l.size(), b.l.size(), c.l.size(), 10 * std::log10 (rms / (double) a.l.size() + 1e-30));
            R.check (diff == 0 && a.l.size() == b.l.size() && a.l.size() == c.l.size() && rms > 0, "seq output identical for every block size");
            svl::NoteGen g; g.prepare (44100); svl::ParamSet q = seqAll (5, 0.3f); q[svl::ARP_ON] = 1; g.setParams (q);
            for (int k = 0; k < 8; ++k) g.keyOn (50 + k, 50 + k, 0, 0.8f);
            auto t0 = std::chrono::steady_clock::now();
            long notes = 0; svl::GenNote n;
            for (long pos = 0; pos < 60L * 44100; pos += 32) { while (g.pop (n)) ++notes; g.advance (32); }
            const double sec = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
            R.line ("  generator alone, 1/32 arp + seq, 8 keys, 60 s of audio: %.3f %% of one core (%ld events)", 100.0 * sec / 60.0, notes);
            R.check (sec / 60.0 < 0.001, "generator under 0.1 % of a core");
        }
    }

    // 8g. drum machine (SvlDrums.h): 8 tracks of drum presets, the default kit from presets/
    {
        R.line ("\n8g. Drum machine");
        const svl::Preset noKit;   // empty kit = the default kit
        auto drumParams = [] (int rate = 3)
        {
            svl::ParamSet p = initParams(); p[svl::DRUMS_ON] = 1; p[svl::DRUMS_RATE] = (float) rate; p[svl::DRUMS_MODE] = 1;   // free
            p[svl::oscParam (0, svl::O_TYPE)] = 0;   // no synth voice: only the drums sound
            return p;
        };
        // run an engine for len samples in blocks, optional host transport from ppq0
        auto run = [&] (const svl::ParamSet& p, long len, int block, double bpm = 120, bool host = false, double ppq0 = 0,
                        std::function<void (svl::Engine&, long)> each = nullptr)
        {
            svl::Engine e; applyKit (e, noKit); e.setParams (p, true); e.setTempo (bpm); e.prepare (SR);
            Stereo s; s.l.assign ((size_t) len, 0); s.r.assign ((size_t) len, 0);
            const double spb = SR * 60.0 / bpm;
            for (long i = 0; i < len; i += block)
            {
                const int n = (int) std::min ((long) block, len - i);
                if (host) e.setTransport (true, true, ppq0 + i / spb);
                if (each) each (e, i);
                e.process (s.l.data() + i, s.r.data() + i, n);
            }
            return s;
        };
        // onsets: the first sample above -60 dBFS after 2 ms below it, when the next 64 samples reach -40 dBFS
        auto onsets = [] (const Buf& x)
        {
            std::vector<long> o; long quiet = 89;
            for (size_t i = 0; i < x.size(); ++i)
            {
                const float a = std::fabs (x[i]);
                if (a >= 1e-3f && quiet > 88)
                {
                    float m = 0; for (size_t j = i; j < std::min (x.size(), i + 64); ++j) m = std::max (m, std::fabs (x[j]));
                    if (m > 1e-2f) o.push_back ((long) i);
                }
                quiet = a < 1e-3f ? quiet + 1 : 0;
            }
            return o;
        };
        // 1. hits on the grid: track 1 (kick) on steps 1 5 9 13, 120 and 97 BPM, blocks 512 and 1
        {
            long worst = 0; int bad = 0; long first120 = -1, first97 = -1;
            for (double bpm : { 120.0, 97.0 })
                for (int block : { 512, 1 })
                {
                    svl::ParamSet p = drumParams (3);   // track 4: the tight hat (short, so every hit starts from silence)
                    for (int s = 0; s < 16; s += 4) p[svl::drumStepParam (3, s)] = 1;
                    const double spb = SR * 60.0 / bpm;
                    auto out = run (p, (long) (8 * spb), block, bpm);
                    auto o = onsets (out.l);
                    if (o.size() != 8) { ++bad; continue; }
                    for (size_t k = 0; k < o.size(); ++k)
                        worst = std::max (worst, std::labs ((o[k] - o[0]) - (long) std::ceil (k * spb - 1e-6)));
                    (bpm == 120 ? first120 : first97) = o[0];
                }
            R.line ("  hat on every beat, 120 / 97 BPM, blocks 512 / 1: worst %ld samples off the beat grid, first hit at %ld / %ld, %d runs with a wrong count",
                    worst, first120, first97, bad);
            R.check (worst <= 1 && bad == 0 && first120 == first97 && first120 >= 0 && first120 < 32, "drum hits on the beat grid (onset detection within 1 sample)");
        }
        // 2. a full pattern with swing and the FX on is identical for every block size
        {
            svl::ParamSet p = drumParams (3); p[svl::DRUMS_SWING] = 0.4f; p[svl::FXR_MIX] = 0.2f; p[svl::FXE_MIX] = 0.1f;
            for (int t = 0; t < 8; ++t) for (int s = 0; s < 16; ++s) p[svl::drumStepParam (t, s)] = (float) (((s * (t + 3)) % 5 == 0) ? 1 + (s % 4 == 0) : 0);
            const long len = 3 * SR;
            const auto a = run (p, len, 512), b = run (p, len, 37), c = run (p, len, 1);
            long diff = 0; double rms = 0;
            for (long i = 0; i < len; ++i) { diff += a.l[(size_t) i] != b.l[(size_t) i] || a.l[(size_t) i] != c.l[(size_t) i] || a.r[(size_t) i] != c.r[(size_t) i]; rms += (double) a.l[(size_t) i] * a.l[(size_t) i]; }
            R.line ("  8 tracks, swing 0.4, echo + reverb, blocks 512 / 37 / 1: %ld samples differ, rms %.1f dBFS", diff, 10 * std::log10 (rms / len + 1e-30));
            R.check (diff == 0 && rms > 0, "drum machine output identical for every block size");
        }
        // 3. accent, mute, pan, decay, level
        {
            auto peakOf = [] (const Buf& x, long from, long to) { float m = 0; for (long i = from; i < to && i < (long) x.size(); ++i) m = std::max (m, std::fabs (x[(size_t) i])); return 20 * std::log10 (m + 1e-30); };
            svl::ParamSet p = drumParams (3); p[svl::drumStepParam (1, 0)] = 1; p[svl::drumStepParam (1, 4)] = 2;   // snare: normal, accent
            auto a = run (p, SR, 512);
            const double pn = peakOf (a.l, 0, 5000), pa = peakOf (a.l, 22050, 27000);   // step 5 = beat 2
            p[svl::drumTrackParam (1, svl::DT_MUTE)] = 1;
            auto m = run (p, SR, 512);
            const double pm = peakOf (m.l, 0, SR);
            svl::ParamSet q = drumParams (3); q[svl::drumStepParam (3, 0)] = 1; q[svl::drumTrackParam (3, svl::DT_PAN)] = -1;   // hat hard left
            auto h = run (q, SR / 2, 512);
            const double hl = peakOf (h.l, 0, SR / 2), hr = peakOf (h.r, 0, SR / 2);
            q[svl::drumTrackParam (3, svl::DT_PAN)] = 0; q[svl::drumTrackParam (3, svl::DT_LEVEL)] = -6;
            auto h6 = run (q, SR / 2, 512);
            q[svl::drumTrackParam (3, svl::DT_LEVEL)] = 0;
            auto h0 = run (q, SR / 2, 512);
            const double d6 = peakOf (h6.l, 0, SR / 2) - peakOf (h0.l, 0, SR / 2);
            auto tailLen = [] (const Buf& x) { long last = 0; for (size_t i = 0; i < x.size(); ++i) if (std::fabs (x[i]) > 1e-3f) last = (long) i; return last; };
            svl::ParamSet k = drumParams (3); k[svl::drumStepParam (0, 0)] = 1;
            const long t1 = tailLen (run (k, SR, 512).l);
            k[svl::drumTrackParam (0, svl::DT_DECAY)] = 2;
            const long t2 = tailLen (run (k, SR, 512).l);
            R.line ("  snare normal / accent peak %.1f / %.1f dBFS; muted: %.1f dBFS; hat pan -1: L %.1f R %.1f dBFS; level -6: %+.2f dB;"
                    " kick to -60 dB: %ld samples, decay x2: %ld", pn, pa, pm, hl, hr, d6, t1, t2);
            R.check (pa > pn + 1 && pm < -200 && hr < hl - 60 && std::fabs (d6 + 6) < 0.05 && t2 > t1 * 3 / 2, "accent, mute, pan, level and decay as set");
        }
        // 3b. drums.mode hold: nothing without a key; the first key starts at step 1 and does not play the synth; the pattern
        //     stops with the last key; a key back within a bar keeps the bar position
        {
            svl::ParamSet p = drumParams (3); p[svl::DRUMS_MODE] = 0; p[svl::oscParam (0, svl::O_TYPE)] = (float) svl::OscType::sine;
            p[svl::oscParam (0, svl::O_LEVEL)] = 0.0001f;   // the synth plays the key, quietly
            for (int s = 0; s < 16; ++s) p[svl::drumStepParam (3, s)] = 1;   // the hat on every 1/16
            int synth = 0;
            const long k1 = 10000, up1 = 10000 + 3 * 5513, k2 = up1 + 2 * 5513 + 100, up2 = k2 + 2 * 5513;
            auto out = run (p, 3 * SR, 1, 120, false, 0, [&] (svl::Engine& e, long i)
            {
                if (i == k1) e.noteOn (60, 0.8f, 0);
                if (i == up1) e.noteOff (60, 0);
                if (i == k2) e.noteOn (62, 0.8f, 0);
                if (i == up2) e.noteOff (62, 0);
                synth = std::max (synth, e.activeVoices (true));
            });
            auto o = onsets (out.l);
            long before = 0; for (long v : o) before += v < k1;
            std::string sp; for (long v : o) sp += std::to_string (v) + " ";
            // own clock from k1: steps at k1 + n x 5512.5; the key back after 2 steps (and 100 samples) keeps the place: the
            // step that began 100 samples ago plays at once (catch), then the grid continues
            const long g = k1 + (long) std::ceil (5 * 5512.5 - 1e-6);
            const bool ok = before == 0 && ! o.empty() && o[0] == k1 && synth == 0 && o.size() == 7
                            && std::find (o.begin(), o.end(), k2) != o.end() && std::find (o.begin(), o.end(), g + 0) == o.end();
            R.line ("  hold: hat hits %s(key at %ld, up %ld, back %ld, up %ld); synth voices %d", sp.c_str(), k1, up1, k2, up2, synth);
            R.check (ok, "drums.mode hold: silent without a key, starts at the key, stops with it, keeps the bar position, the synth stays silent");
        }
        // 3c. drums.octave moves the whole kit by octaves: a drum-mode sine kick and a plain-pitched sound, +1 / -1
        {
            auto pitchOf = [&] (const svl::ParamSet& snd, float oct)
            {
                svl::Engine e; e.setDrumTrackSound (0, snd, 2.0f);   // held through the 1 s window
                svl::ParamSet p = drumParams (3); p[svl::drumStepParam (0, 0)] = 1; p[svl::DRUMS_OCT] = oct;
                e.setParams (p, true); e.prepare (SR);
                Buf L (SR), Rr (SR);
                for (int i = 0; i < SR; i += 512) e.process (L.data() + i, Rr.data() + i, std::min (512, SR - i));
                // first and last upward zero crossing after the pitch settles: cycles / time
                long first = -1, last = -1; int cycles = -1;
                for (int i = 4001; i < SR - 100; ++i) if (L[(size_t) i - 1] < 0 && L[(size_t) i] >= 0) { if (first < 0) first = i; last = i; ++cycles; }
                return cycles > 0 ? cycles / ((double) (last - first) / SR) : 0.0;
            };
            svl::ParamSet kick = initParams(); kick[svl::oscParam (0, svl::O_TYPE)] = (float) svl::OscType::sine; kick[svl::DRUM_MODE] = 1;
            kick[svl::DRUM_PITCH] = 45; kick[svl::FLT_CUTOFF] = 20000; kick[svl::AENV_D] = 5; kick[svl::AENV_S] = 1;
            svl::ParamSet tone = kick; tone[svl::DRUM_MODE] = 0;   // the note moves (60)
            const double k0 = pitchOf (kick, 0), kp = pitchOf (kick, 1), km = pitchOf (kick, -1), t0 = pitchOf (tone, 0), tp = pitchOf (tone, 1);
            R.line ("  drums.octave, drum-mode sine at drum.pitch 45: %.1f Hz, +1 %.1f Hz, -1 %.1f Hz; plain sound (note 60): %.1f Hz, +1 %.1f Hz",
                    k0, kp, km, t0, tp);
            R.check (std::fabs (kp / k0 - 2) < 0.01 && std::fabs (k0 / km - 2) < 0.01 && std::fabs (tp / t0 - 2) < 0.01, "drums.octave moves the kit by whole octaves");
        }
        // 4. drums.keys: MIDI 36..43 hit the tracks and do not play the synth; host grid; everything ends when switched off
        {
            svl::ParamSet p = drumParams (3); p[svl::DRUMS_KEYS] = 1; p[svl::oscParam (0, svl::O_TYPE)] = (float) svl::OscType::saw;
            int synthVoices = 0;
            auto out = run (p, SR / 2, 256, 120, false, 0, [&] (svl::Engine& e, long i) { if (i == 2560) e.noteOn (37, 1.0f, 0); if (i == 7680) e.noteOff (37, 0); synthVoices = std::max (synthVoices, e.activeVoices (true)); });
            auto o = onsets (out.l);
            svl::ParamSet h = drumParams (1); for (int s = 0; s < 16; ++s) h[svl::drumStepParam (3, s)] = 1;   // 1/8, hat on every step
            auto ho = onsets (run (h, SR, 512, 120, true, 3.9).l);
            const double firstBeat = ho.empty() ? -1 : 3.9 + ho[0] / 22050.0;
            svl::Engine e; applyKit (e, noKit);
            svl::ParamSet f = drumParams (5); for (int t = 0; t < 8; ++t) for (int s = 0; s < 16; ++s) f[svl::drumStepParam (t, s)] = 1;
            for (int t = 0; t < 8; ++t) f[svl::drumTrackParam (t, svl::DT_DECAY)] = 4;
            e.setParams (f, true); e.prepare (SR);
            Buf L (256), Rr (256);
            gAllocs = 0; gCountAllocs = true;
            for (int b = 0; b < 200; ++b) e.process (L.data(), Rr.data(), 256);
            gCountAllocs = false;
            const long allocs = gAllocs.load();
            const int during = e.drumMachine()->soundingTracks();
            auto t0 = std::chrono::steady_clock::now();
            for (int b = 0; b < 1000; ++b) e.process (L.data(), Rr.data(), 256);
            const double cpu = 100.0 * std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count() / (1000 * 256.0 / SR);
            e.setParam (svl::DRUMS_ON, 0);
            for (int b = 0; b < 2000; ++b) e.process (L.data(), Rr.data(), 256);
            const int after = e.drumMachine()->soundingTracks();
            R.line ("  key 37 -> track 2: %zu hit(s) at %ld, synth voices %d; host from beat 3.9 at 1/8: first hit at beat %.4f, %zu hits in 2 beats;"
                    " all 8 tracks on every 1/16T step, decay x4: %d sounding, %.2f %% of one core, %ld heap allocations; switched off: %d sounding",
                    o.size(), o.empty() ? -1L : o[0], synthVoices, firstBeat, ho.size(), during, cpu, allocs, after);
            R.check (o.size() == 1 && o[0] >= 2560 && o[0] < 2600 && synthVoices == 0, "drums.keys: MIDI 36..43 hit tracks 1..8, the synth stays silent");
            R.check (std::fabs (firstBeat - 4.0) < 1e-3 && ho.size() == 4, "drum steps on the host's bar grid");
            R.check (allocs == 0 && after == 0 && during > 0, "drum machine: no heap allocation, nothing left sounding when switched off");
        }
    }

    // 8h. no hanging notes when a mode changes while keys are held (code sweep, 19 Oct)
    {
        R.line ("\n8h. Mode changes with keys held");
        auto left = [&] (std::function<void (svl::Engine&)> before, int key, std::function<void (svl::Engine&)> change, std::vector<int> keys)
        {
            svl::ParamSet p = initParams(); p[svl::AENV_R] = 0.05f;
            svl::Engine e; e.setParams (p, true); e.prepare (SR);
            if (before) before (e);
            Buf L (256), Rr (256);
            for (int k : keys) e.noteOn (k, 0.8f, 0);
            if (key >= 0) e.noteOn (key, 0.8f, 0);
            for (int b = 0; b < 20; ++b) e.process (L.data(), Rr.data(), 256);
            const int sounding = e.activeVoices (true);
            change (e);
            for (int k : keys) e.noteOff (k, 0);
            if (key >= 0) e.noteOff (key, 0);
            for (int b = 0; b < 200; ++b) e.process (L.data(), Rr.data(), 256);
            return std::make_pair (sounding, e.activeVoices (true));
        };
        const auto ks = left ([] (svl::Engine& e) { e.setParam (svl::TUNE_MODE, 1); }, 30,
                              [] (svl::Engine& e) { e.setParam (svl::KSW_MODE, 1); e.setParam (svl::KSW_LOW, 24); }, {});
        const auto dk = left (nullptr, 37, [] (svl::Engine& e) { e.setParam (svl::DRUMS_ON, 1); e.setParam (svl::DRUMS_KEYS, 1); }, {});
        const auto pm = left (nullptr, -1, [] (svl::Engine& e) { e.setParam (svl::V_MODE, (float) svl::VoiceMode::mono); }, { 60, 64, 67 });
        const auto pl = left (nullptr, -1, [] (svl::Engine& e) { e.setParam (svl::V_MODE, (float) svl::VoiceMode::legato); }, { 60, 64, 67 });
        const auto mp = left ([] (svl::Engine& e) { e.setParam (svl::V_MODE, (float) svl::VoiceMode::mono); }, -1,
                              [] (svl::Engine& e) { e.setParam (svl::V_MODE, (float) svl::VoiceMode::poly); }, { 60, 64 });
        R.line ("  sounding -> left after the keys go up: keyswitch range switched on %d -> %d; drums.keys switched on %d -> %d;"
                " poly -> mono %d -> %d; poly -> legato %d -> %d; mono -> poly %d -> %d",
                ks.first, ks.second, dk.first, dk.second, pm.first, pm.second, pl.first, pl.second, mp.first, mp.second);
        R.check (ks.first == 1 && ks.second == 0 && dk.first == 1 && dk.second == 0, "a note-off is never swallowed by a keyswitch range or drums.keys");
        R.check (pm.first == 3 && pm.second == 0 && pl.first == 3 && pl.second == 0 && mp.first >= 1 && mp.second == 0,
                 "changing voice.mode with keys held leaves no voice hanging");
    }

    // 9. no allocation in process()
    {
        R.line ("\n9. Real time");
        svl::Engine e; e.setParams (presetParams (kPresetFiles[4]), true); e.prepare (SR);
        Buf L (512), Rr (512);
        gAllocs = 0; gCountAllocs = true;
        for (int b = 0; b < 400; ++b)
        {
            if (b % 10 == 0) { e.noteOn (40 + b % 37, 0.8f, b % 512); }
            if (b % 10 == 5) e.noteOff (40 + (b - 5) % 37, 100);
            if (b % 50 == 0) e.setParam (svl::FLT_CUTOFF, 300.0f + 50.0f * (float) b);
            e.process (L.data(), Rr.data(), 512);
        }
        gCountAllocs = false;
        R.line ("  heap allocations during 400 process() blocks with notes and parameter changes: %ld", gAllocs.load());
        R.check (gAllocs.load() == 0, "no heap allocation on the audio thread");
    }

    // 10. CPU per voice
    {
        R.line ("\n10. CPU per voice (16 voices held 10 s, block 512, %d Hz; %% = share of one core in real time)", SR);
        struct C { const char* name; svl::OscType t; int oscs; float drive; };
        const C cs[] = { { "1 saw", svl::OscType::saw, 1, 0 }, { "3 saw + drive 12 dB", svl::OscType::saw, 3, 12 },
                         { "3 square", svl::OscType::square, 3, 0 }, { "3 triangle", svl::OscType::triangle, 3, 0 },
                         { "3 pink noise", svl::OscType::noisePink, 3, 0 }, { "3 string", svl::OscType::string, 3, 0 }, { "3 saw x unison 7", svl::OscType::saw, 3, 0 } };
        for (const auto& c : cs)
        {
            svl::ParamSet p = initParams();
            for (int o = 0; o < 3; ++o) p[svl::oscParam (o, svl::O_TYPE)] = (float) (o < c.oscs ? c.t : svl::OscType::off);
            p[svl::FLT_DRIVE] = c.drive; p[svl::FLT_ENVAMT] = 2; p[svl::FENV_S] = 0.5f;
            if (std::strstr (c.name, "unison")) for (int o = 0; o < 3; ++o) p[svl::oscParam (o, svl::O_UNI)] = 7;
            svl::Engine e; e.setParams (p, true); e.prepare (SR);
            Buf L (512), Rr (512);
            for (int k = 0; k < 16; ++k) e.noteOn (36 + k * 3, 0.8f, 0);
            const int blocks = 10 * SR / 512;
            auto t0 = std::chrono::steady_clock::now();
            for (int b = 0; b < blocks; ++b) e.process (L.data(), Rr.data(), 512);
            const double el = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
            const double audio = blocks * 512.0 / SR;
            R.line ("  %-22s %6.3f %% per voice, %5.2f %% for 16 voices (%d playing)", c.name, 100 * el / audio / 16, 100 * el / audio, e.activeVoices (false));
        }
    }

    // 11. every preset under presets/ reads back
    {
        R.line ("\n11. Presets (every .xml under presets/)");
        int ok = 0, total = 0;
        std::vector<std::string> files;
        for (auto& de : std::filesystem::recursive_directory_iterator (gRoot + "/presets"))
            if (de.is_regular_file() && de.path().extension() == ".xml") files.push_back (de.path().string());
        std::sort (files.begin(), files.end());
        for (const auto& f : files)
        {
            ++total;
            svl::Preset a; svl::loadPreset (f, a);
            svl::Preset b; svl::presetFromXml (svl::presetToXml (a), b);
            bool same = a.warnings.empty() && b.warnings.empty() && a.meta.author == "SubverseLab";
            for (int i = 0; i < svl::kNumParams; ++i) same &= a.params[i] == b.params[i];
            same &= a.drumKit == b.drumKit;
            ok += same;
            R.line ("  %-44s \"%s\" (%s): %s", f.substr (gRoot.size() + 1).c_str(), a.meta.name.c_str(), a.meta.family.c_str(), same ? "round trip exact" : "MISMATCH");
        }
        char what[96]; std::snprintf (what, sizeof what, "%d/%d presets load without warnings, author SubverseLab, exact round trip", ok, total);
        R.check (ok == total && total > 0, what);
    }

    // 12. arp / sequencer presets (presets/Arp - */, presets/Sequence - */): the pattern is on, it plays with the held input of
    //     tools/make_pattern_presets.py for 4 s at 120 BPM, stays under -1 dBFS, and leaves no note sounding
    {
        R.line ("\n12. Arp, sequencer and drum machine presets");
        int ok = 0, total = 0, plain = 0, plainTotal = 0;
        std::vector<std::string> files;
        for (auto& de : std::filesystem::recursive_directory_iterator (gRoot + "/presets"))
            if (de.is_regular_file() && de.path().extension() == ".xml") files.push_back (de.path().string());
        std::sort (files.begin(), files.end());
        for (const auto& f : files)
        {
            svl::Preset pr; svl::loadPreset (f, pr);
            const bool arpFam = pr.meta.family.rfind ("Arp - ", 0) == 0, seqFam = pr.meta.family.rfind ("Sequence - ", 0) == 0;
            const bool drumFam = pr.meta.family == "Drum Machine";
            const bool moves = pr.params[svl::ARP_ON] > 0.5f || pr.params[svl::SEQ_ON] > 0.5f || (drumFam && pr.params[svl::DRUMS_ON] > 0.5f);
            if (! arpFam && ! seqFam && ! drumFam) { ++plainTotal; plain += ! moves && pr.params[svl::DRUMS_ON] < 0.5f; continue; }   // sound presets: none
            bool kitOk = true;
            if (drumFam)
                for (auto& k : pr.drumKit) kitOk &= ! k.empty() && std::filesystem::exists (gRoot + "/presets/" + k + ".xml");
            ++total;
            const std::string sub = pr.meta.family.substr (pr.meta.family.find (" - ") + 3);
            std::vector<Note> keys;
            if (drumFam) keys = { { 48, 100, 0.0, 4.0 } };   // drums.mode hold: the beat runs while the key is held
            else if (seqFam && sub != "Pulse") keys = { { (sub == "Bass" || sub == "Acid") ? 36.0f : 60.0f, 100, 0.0, 4.0 } };
            else if (sub == "Wide") keys = { { 60, 100, 0.0, 4.0 } };
            else keys = { { 48, 100, 0.0, 4.0 } };   // one key: the arp.chord shape makes the combination
            svl::Engine e; applyKit (e, pr);
            RenderOpts o; o.tail = 1.0; o.tempo = 120; o.maxExtra = 30; o.minLen = 4;   // ringing sounds: render until silent
            const auto out = render (pr.params, keys, o, &e);
            double rms = 0; float peak = 0; bool finite = true;
            for (size_t i = 0; i < out.l.size(); ++i)
            {
                rms += (double) out.l[i] * out.l[i]; peak = std::max ({ peak, std::fabs (out.l[i]), std::fabs (out.r[i]) });
                finite &= std::isfinite (out.l[i]) && std::isfinite (out.r[i]);
            }
            rms = 10 * std::log10 (rms / (double) std::max<size_t> (1, out.l.size()) + 1e-30);
            const double pk = 20 * std::log10 (peak + 1e-30);
            const bool good = moves && kitOk && pr.warnings.empty() && pr.meta.author == "SubverseLab" && finite && rms > -45 && pk < -1
                              && e.activeVoices (true) == 0 && e.noteGen().soundingNotes() == 0;
            ok += good;
            R.line ("  %-58s rms %5.1f peak %5.1f dBFS%s", f.substr (gRoot.size() + 1).c_str(), rms, pk, good ? "" : "  BAD");
        }
        char what[160]; std::snprintf (what, sizeof what, "%d/%d arp / sequencer / drum machine presets play their pattern, peak under -1 dBFS, no note left", ok, total);
        R.check (ok == total && total > 0, what);
        std::snprintf (what, sizeof what, "%d/%d sound presets carry no arp / seq / drums", plain, plainTotal);
        R.check (plain == plainTotal && plainTotal > 0, what);
    }

    R.line ("\nselftest: %d passed, %d failed", R.pass, R.fail);
    if (R.f) std::fclose (R.f);
    return R.fail == 0 ? 0 : 1;
}

// ---------------------------------------------------------------- main
int main (int argc, char** argv)
{
    std::map<std::string, std::string> a;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i)
    {
        std::string s = argv[i];
        if (s.rfind ("--", 0) == 0)
        {
            if (s == "--selftest") { a[s] = "1"; continue; }
            a[s] = (i + 1 < argc) ? argv[++i] : "";
        }
        else pos.push_back (s);
    }
    if (a.count ("--root")) gRoot = a["--root"];
    if (a.count ("--selftest")) return selftest (a.count ("--out-dir") ? a["--out-dir"] : gRoot + "/out/selftest");
    if (! pos.empty() && pos[0] == "write-init" && pos.size() > 1)
    {
        svl::Preset p; p.meta.tags = "init";
        return svl::savePreset (pos[1], p) ? 0 : 1;
    }
    if (! pos.empty() && pos[0] == "render")
    {
        if (! a.count ("--out")) { std::fprintf (stderr, "--out required\n"); return 2; }
        svl::Preset pr;
        if (a.count ("--preset") && ! svl::loadPreset (a["--preset"], pr)) { std::fprintf (stderr, "cannot read preset\n"); return 2; }
        for (auto& w : pr.warnings) std::fprintf (stderr, "preset: %s\n", w.c_str());
        std::vector<Note> notes;
        if (a.count ("--notes")) { if (! loadNotes (a["--notes"], notes)) { std::fprintf (stderr, "cannot read notes\n"); return 2; } }
        else if (a.count ("--note"))
            notes.push_back ({ std::stof (a["--note"]), a.count ("--vel") ? std::stof (a["--vel"]) : 100.0f,
                               a.count ("--start") ? std::stod (a["--start"]) : 0.0, a.count ("--len") ? std::stod (a["--len"]) : 1.0 });
        RenderOpts o;
        if (a.count ("--sr")) o.sr = std::stoi (a["--sr"]);
        if (a.count ("--block")) o.block = std::max (1, std::min (8192, std::stoi (a["--block"])));
        if (a.count ("--tail")) o.tail = std::stod (a["--tail"]);
        o.maxExtra = a.count ("--max-extra") ? std::stod (a["--max-extra"]) : 60.0;
        if (a.count ("--tempo")) o.tempo = std::stod (a["--tempo"]);
        if (a.count ("--length")) o.minLen = std::stod (a["--length"]);   // FX tails: render on until silent
        svl::Engine e;
        if (pr.params[svl::DRUMS_ON] > 0.5f) applyKit (e, pr);
        auto s = render (pr.params, notes, o, &e);
        const long clipped = writeWav24 (a["--out"], s, o.sr);
        if (clipped < 0) return 1;
        std::printf ("%s: %.2f s, peak %.1f dBFS, %.1f LUFS-I, %ld clipped samples\n", a["--out"].c_str(), s.l.size() / (double) o.sr,
                     peakDb (s), lufs (s, o.sr), clipped);
        return 0;
    }
    std::fprintf (stderr, "usage: svl render --preset p.xml (--notes n.txt | --note 60 [--vel 100 --start 0 --len 1]) --out x.wav [--sr 44100 --block 512 --tail 4]\n"
                          "       svl write-init path.xml\n       svl --selftest [--root dir] [--out-dir dir]\n");
    return 2;
}
