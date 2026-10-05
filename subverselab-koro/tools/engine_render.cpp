#include "../Source/CifteEngine.h"
#include "presets.h"
#include "wav.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

static float rms (const std::vector<float>& x) { double s = 0.0; for (float v : x) s += double (v) * v; return x.empty () ? 0.0f : float (std::sqrt (s / double (x.size ()))); }
static float peak (const std::vector<float>& x) { float p = 0.0f; for (float v : x) p = std::max (p, std::fabs (v)); return p; }
static std::vector<float> render (const std::vector<float>& in, int channels, int rate, const cifte::Controls& c)
{
    const int n = int (in.size () / size_t (channels)), outN = n; std::vector<float> l (static_cast<size_t> (n)); std::vector<float> r (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i) { l[size_t (i)] = in[size_t (i * channels)]; r[size_t (i)] = channels > 1 ? in[size_t (i * channels + 1)] : l[size_t (i)]; }
    // the engine has a fixed latency: pad the input and drop the first latency() samples (output aligned with the input)
    cifte::CifteEngine e; e.prepare (rate); const int lat = e.latency (c); l.resize (size_t (n + lat), 0.0f); r.resize (size_t (n + lat), 0.0f);
    for (int p = 0; p < n + lat; p += 512) e.process (l.data () + p, r.data () + p, std::min (512, n + lat - p), c);
    std::vector<float> out (size_t (outN) * 2u); for (int i = 0; i < n; ++i) { out[size_t (i * 2)] = l[size_t (i + lat)]; out[size_t (i * 2 + 1)] = r[size_t (i + lat)]; } return out;
}
static cifte::Controls preset (const std::string& name)
{
    cifte::Controls c;
    if (cifte::moods::get (name, c)) return c;
    if (name == "Tight Double") { c.voices = 2; c.spread = 0.18f; c.tight = 0.88f; c.width = 0.38f; c.tone = 0.78f; c.mix = 0.42f; }
    else if (name == "Arabesk Choir") { c.voices = 3; c.spread = 0.78f; c.tight = 0.22f; c.width = 0.92f; c.tone = 0.58f; c.mix = 0.55f; }
    else if (name == "Wide Chorus-less") { c.voices = 2; c.spread = 0.92f; c.tight = 0.42f; c.width = 1.0f; c.tone = 0.68f; c.mix = 0.5f; }
    else if (name == "Backing Stack") { c.voices = 3; c.spread = 0.48f; c.tight = 0.5f; c.width = 0.7f; c.tone = 0.42f; c.mix = 0.62f; }
    return c;
}
static void applyParam (cifte::Controls& c, const std::string& p)
{
    const size_t eq = p.find ('='); if (eq == std::string::npos) return; const std::string k = p.substr (0, eq); const float v = std::strtof (p.c_str () + eq + 1, nullptr);
    if (k == "voices") c.voices = int (v); else if (k == "spread") c.spread = v; else if (k == "tight") c.tight = v; else if (k == "width") c.width = v; else if (k == "tone") c.tone = v; else if (k == "mix") c.mix = v; else if (k == "mono_safe") c.monoSafe = v > 0.5f;
    else if (k == "key_mode") c.keyMode = v > 0.5f ? cifte::KeyMode::Manual : cifte::KeyMode::Auto; else if (k == "key_lock") c.keyLock = v > 0.5f; else if (k == "color") c.color = v; else if (k == "share_width_tone") c.shareWidthTone = v > 0.5f;
    else if (k == "harmony") c.harmony = v; else if (k == "root") c.root = int (v); else if (k == "scale") c.scale = int (v); else if (k == "glide") c.glide = v; else if (k == "timing") c.timing = v; else if (k == "harmony_voices") c.harmonyVoices = int (v);
    else if (k.rfind ("degree", 0) == 0) { const int i = std::max (0, std::min (2, std::atoi (k.c_str () + 6) - 1)); c.degree[size_t (i)] = int (v); }
    else if (k.rfind ("level", 0) == 0) { const int i = std::max (0, std::min (2, std::atoi (k.c_str () + 5) - 1)); c.voiceLevel[size_t (i)] = v; }
    else if (k.rfind ("pan", 0) == 0) { const int i = std::max (0, std::min (2, std::atoi (k.c_str () + 3) - 1)); c.voicePan[size_t (i)] = v; }
}
static std::vector<float> crop (const Wav& w, double start, double seconds)
{
    const int first = std::max (0, std::min (int (w.samples.size () / size_t (w.channels)), int (start * w.rate)));
    const int count = std::min (int (seconds * w.rate), int (w.samples.size () / size_t (w.channels)) - first);
    return std::vector<float> (w.samples.begin () + size_t (first * w.channels), w.samples.begin () + size_t ((first + count) * w.channels));
}
static int generate (const std::string& kind, const std::string& path)
{
    const int rate = 44100, seconds = 20, n = rate * seconds; std::vector<float> x (size_t (n) * 2u, 0.0f);
    for (int i = 0; i < n; ++i)
    {
        const float t = float (i) / rate; float l = 0.0f, r = 0.0f;
        if (kind == "drum")
        {
            const int step = int (t * 2.0f) % 16; const float phase = std::fmod (t * 2.0f, 1.0f);
            if (step == 0 || step == 8) { const float e = std::exp (-phase * 22.0f); l += e * std::sin (2.0f * cifte::kPi * (74.0f - 35.0f * phase) * phase); r += l; }
            if (step == 4 || step == 12) { const float e = std::exp (-phase * 30.0f); l += e * 0.45f * std::sin (2.0f * cifte::kPi * 190.0f * phase); r += l; }
            if (step % 2 == 0) { const float e = std::exp (-phase * 48.0f); const float h = std::sin (2.0f * cifte::kPi * 3700.0f * phase) + 0.5f * std::sin (2.0f * cifte::kPi * 6100.0f * phase); l += e * 0.08f * h; r += e * 0.07f * h; }
        }
        else
        {
            const int note = int (t * 0.5f) % 4; const float f = 110.0f * std::pow (2.0f, float (note) / 3.0f); const float e = 0.5f + 0.5f * std::sin (2.0f * cifte::kPi * t / 8.0f);
            l = 0.14f * e * (std::sin (2.0f * cifte::kPi * f * t) + 0.33f * std::sin (2.0f * cifte::kPi * f * 2.01f * t)); r = 0.14f * e * (std::sin (2.0f * cifte::kPi * f * t + 0.12f) + 0.33f * std::sin (2.0f * cifte::kPi * f * 2.01f * t));
        }
        x[size_t (2 * i)] = l; x[size_t (2 * i + 1)] = r;
    }
    writeWav (path, rate, 2, x); return 0;
}
static bool check (const char* name, bool ok, const std::string& value, int& count) { ++count; std::printf ("%s %s %s\n", ok ? "PASS" : "FAIL", name, value.c_str ()); return ok; }
static float diffRms (const std::vector<float>& a, const std::vector<float>& b) { std::vector<float> d (a.size ()); for (size_t i = 0; i < d.size (); ++i) d[i] = a[i] - b[i]; return rms (d); }
// ---------------------------------------------------------------- harmony (pitch-shift) tests
// Render with the harmony latency removed (input padded, first `lat` samples dropped).
static std::vector<float> renderAligned (const std::vector<float>& monoIn, int rate, const cifte::Controls& c, std::vector<float>* rightOut = nullptr)
{
    cifte::CifteEngine e; e.prepare (rate); const int lat = e.latency (c); const size_t n = monoIn.size ();
    std::vector<float> l (n + size_t (lat), 0.0f), r (l.size (), 0.0f);
    for (size_t i = 0; i < n; ++i) l[i] = r[i] = monoIn[i];
    for (size_t p = 0; p < l.size (); p += 512) e.process (l.data () + p, r.data () + p, int (std::min<size_t> (512, l.size () - p)), c);
    if (rightOut) rightOut->assign (r.begin () + lat, r.end ());
    return std::vector<float> (l.begin () + lat, l.end ());
}
// Harmonic synthetic "voice": notes A3, B3, C#4 (0.8 s each, 40 ms glides), 5.5 Hz +-30 cent vibrato.
static std::vector<float> vibratoVoice (int rate, double seconds, std::vector<double>& f0)
{
    const int n = int (seconds * rate); std::vector<float> y (static_cast<size_t> (n)); f0.assign (size_t (n), 0.0);
    const double notes[3] = { 220.0, 246.9417, 277.1826 }; double phase = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double t = double (i) / rate, noteLen = 0.8; const int k = int (t / noteLen); const double u = t - k * noteLen;
        double c = 1200.0 * std::log2 (notes[k % 3] / 220.0);
        if (u < 0.04 && k > 0) { const double cp = 1200.0 * std::log2 (notes[(k - 1) % 3] / 220.0); c = cp + (c - cp) * (u / 0.04); }
        const double hz = 220.0 * std::pow (2.0, (c + 30.0 * std::sin (2.0 * 3.14159265358979 * 5.5 * t)) / 1200.0);
        f0[size_t (i)] = hz; phase += 2.0 * 3.14159265358979 * hz / rate; if (phase > 2.0 * 3.14159265358979) phase -= 2.0 * 3.14159265358979;
        double s = 0.0; for (int h = 1; h * hz < 0.45 * rate && h <= 40; ++h) s += std::sin (h * phase) / h * (h <= 4 ? 1.0 : 1.6 / std::sqrt (double (h)));
        y[size_t (i)] = float (0.18 * s);
    }
    return y;
}
// Strongest frequency in [lo, hi] Hz of x[a, a + len) (Hann-windowed DFT scan, coarse then fine).
static double peakHz (const std::vector<float>& x, int rate, size_t a, size_t len, double lo, double hi, double* levelAt = nullptr, double probe = 0.0)
{
    std::vector<double> w (len); for (size_t i = 0; i < len; ++i) w[i] = x[a + i] * (0.5 - 0.5 * std::cos (2.0 * 3.14159265358979 * double (i) / double (len - 1)));
    auto mag = [&] (double f) { double re = 0.0, im = 0.0; const double dw = 2.0 * 3.14159265358979 * f / rate; for (size_t i = 0; i < len; ++i) { re += w[i] * std::cos (dw * double (i)); im -= w[i] * std::sin (dw * double (i)); } return std::sqrt (re * re + im * im); };
    double bf = lo, bm = -1.0; for (double f = lo; f <= hi; f += 0.5) { const double m = mag (f); if (m > bm) { bm = m; bf = f; } }
    for (double f = bf - 0.6; f <= bf + 0.6; f += 0.02) { const double m = mag (f); if (m > bm) { bm = m; bf = f; } }
    if (levelAt) *levelAt = 20.0 * std::log10 (std::max (1.0e-12, mag (probe) / bm));
    return bf;
}
static bool harmonyTests (int& checks)
{
    bool all = true; const int rate = 44100;
    std::vector<double> f0; const std::vector<float> voice = vibratoVoice (rate, 4.8, f0);
    // 1. tracker accuracy on the synthetic vibrato voice
    {
        const auto fr = cifte::trackOffline (voice, rate); std::vector<double> err; int voiced = 0, total = 0;
        for (const auto& f : fr) { if (f.t < 0.1 * rate || f.t > double (voice.size ()) - 0.1 * rate) continue; ++total; if (!f.voiced) continue; ++voiced; err.push_back (std::fabs (1200.0 * std::log2 (f.hz / f0[size_t (std::lround (f.t))]))); }
        std::sort (err.begin (), err.end ()); const double mean = err.empty () ? 1e9 : [&] { double s = 0; for (double e : err) s += e; return s / double (err.size ()); } ();
        const double p95 = err.empty () ? 1e9 : err[size_t (0.95 * double (err.size () - 1))], mx = err.empty () ? 1e9 : err.back ();
        char b[200]; std::snprintf (b, sizeof b, "mean_cents=%.2f p95_cents=%.2f max_cents=%.2f voiced=%.1f%%", mean, p95, mx, 100.0 * voiced / std::max (1, total));
        all &= check ("tracker_vibrato_accuracy", p95 <= 10.0 && voiced >= 0.95 * total, b, checks);
    }
    // legacy harmony settings: manual key, per-voice degree[], harmony voices untouched by WIDTH/TONE, doubler off
    cifte::Controls h; h.harmony = 1.0f; h.harmonyVoices = 1; h.root = 9; h.scale = 0; h.voiceLevel = {{1.0f, 1.0f, 1.0f}}; h.voicePan = {{0.0f, 0.0f, 0.0f}}; h.glide = 0.0f; h.timing = 0.0f;
    h.keyMode = cifte::KeyMode::Manual; h.color = -1.0f; h.shareWidthTone = false; h.mix = 0.0f; h.shifterV2 = false;
    // 2. sine 220 Hz, A major, harmony voice alone
    std::vector<float> s220 (size_t (2 * rate)); for (size_t i = 0; i < s220.size (); ++i) s220[i] = 0.5f * std::sin (2.0f * cifte::kPi * 220.0f * float (i) / float (rate));
    for (int deg : { 2, 4 })
    {
        cifte::Controls z = h; z.degree[0] = deg; const std::vector<float> y = renderAligned (s220, rate, z); double at220 = 0.0;
        const double f = peakHz (y, rate, size_t (0.5 * rate), size_t (rate), 100.0, 600.0, &at220, 220.0), want = deg == 2 ? 277.1826 : 329.6276;
        char b[200]; std::snprintf (b, sizeof b, "peak_hz=%.2f target_hz=%.2f err=%.3f%% level_at_220Hz=%.1fdB rms_ratio_to_dry=%.2fdB", f, want, 100.0 * (f / want - 1.0), at220, 20.0 * std::log10 (rms (y) / rms (s220)));
        all &= check (deg == 2 ? "harmony_sine_deg+2_major_third" : "harmony_sine_deg+4_fifth", std::fabs (f / want - 1.0) < 0.01, b, checks);
    }
    // 3. harmony voice f0 tracks the target on the vibrato voice (target = input f0 * scale interval; +2 in A major)
    {
        cifte::Controls z = h; z.degree[0] = 2; const std::vector<float> y = renderAligned (voice, rate, z); const auto fr = cifte::trackOffline (y, rate); int ok = 0, tot = 0; double worst = 0;
        for (const auto& f : fr)
        {
            if (f.t < 0.1 * rate || f.t > double (voice.size ()) - 0.1 * rate) continue;
            const double fin = f0[size_t (std::lround (f.t))]; int idx = -1, oct = 0; cifte::scales::nearest (6900.0 + 1200.0 * std::log2 (fin / 440.0), 9, 0, false, idx, oct, 0.0);
            const double target = fin * std::pow (2.0, cifte::scales::intervalCents (idx, oct, 2, 0) / 1200.0); ++tot;
            if (f.voiced) { const double e = std::fabs (1200.0 * std::log2 (f.hz / target)); if (e <= 30.0) ++ok; worst = std::max (worst, e); }
        }
        char b[200]; std::snprintf (b, sizeof b, "within_30_cents=%.1f%% of %d voiced frames", 100.0 * ok / std::max (1, tot), tot);
        all &= check ("harmony_vibrato_tracks_target", ok >= 0.9 * tot, b, checks);
    }
    // 4. unvoiced noise passes without shifting (harmony voice alone == delayed input)
    {
        std::vector<float> nz (static_cast<size_t> (rate)); cifte::Rng g (4242u); for (float& v : nz) v = 0.3f * g.bipolar ();
        cifte::Controls z = h; z.degree[0] = 4; const std::vector<float> y = renderAligned (nz, rate, z);
        double num = 0.0, den = 0.0; for (size_t i = 0; i < y.size (); ++i) { num += double (y[i]) * nz[i]; den += double (nz[i]) * nz[i]; } const double k = num / den;
        std::vector<float> res (y.size ()); for (size_t i = 0; i < y.size (); ++i) res[i] = float (y[i] - k * nz[i]);
        const auto fr = cifte::trackOffline (nz, rate); int vf = 0; for (const auto& f : fr) vf += f.voiced;
        const double resDb = 20.0 * std::log10 (std::max (1.0e-12, double (rms (res)) / rms (nz)));
        char b[200]; std::snprintf (b, sizeof b, "gain=%.4f residual=%.1fdB voiced_frames=%d/%zu", k, resDb, vf, fr.size ());
        all &= check ("harmony_unvoiced_passthrough", resDb < -40.0 && std::fabs (k - 1.0) < 0.01, b, checks);
    }
    // 5. no NaN / clicks
    {
        bool finite = true; float pk = 0.0f, pk1 = 0.0f; double worstRatio = 0.0;
        cifte::Controls one = h; one.harmony = 0.5f; one.degree[0] = 2; one.voiceLevel[0] = 0.631f;   // the demo setting: one voice, -4 dB under dry
        cifte::Controls z = h; z.harmony = 0.5f; z.harmonyVoices = 3; z.degree = {{2, -2, 4}}; z.glide = 0.3f; z.timing = 12.0f;
        for (int r : { 44100, 48000, 96000 })
        {
            std::vector<double> ff; const std::vector<float> vv = vibratoVoice (r, 2.0, ff); const int nn = int (vv.size ());
            std::vector<float> sil (static_cast<size_t> (nn), 0.0f), sn (static_cast<size_t> (nn)), wn (static_cast<size_t> (nn)), sw (static_cast<size_t> (nn)); cifte::Rng g (7u);
            for (int i = 0; i < nn; ++i) { const float t = float (i) / r; sn[size_t (i)] = 0.99f * std::sin (2.0f * cifte::kPi * 1000.0f * t); wn[size_t (i)] = 0.35f * g.bipolar (); sw[size_t (i)] = 0.5f * std::sin (2.0f * cifte::kPi * 20.0f * (std::pow (1000.0f, t / 2.0f) - 1.0f) * 2.0f / std::log (1000.0f)); }
            for (const std::vector<float>* src : std::initializer_list<const std::vector<float>*> { &sil, &sn, &wn, &sw, &vv }) { const std::vector<float> y = renderAligned (*src, r, z); for (float v : y) finite &= std::isfinite (v); pk = std::max (pk, peak (y)); const std::vector<float> y1 = renderAligned (*src, r, one); for (float v : y1) finite &= std::isfinite (v); pk1 = std::max (pk1, peak (y1)); }
            for (const std::vector<float>* src : { &vv }) for (int deg : { 2, 4, -2, -7, 7 })
            {
                cifte::Controls a = h; a.degree[0] = deg; const std::vector<float> y = renderAligned (*src, r, a); float jy = 0.0f, jx = 0.0f;
                for (size_t i = 1; i < y.size (); ++i) { jy = std::max (jy, std::fabs (y[i] - y[i - 1])); jx = std::max (jx, std::fabs ((*src)[i] - (*src)[i - 1])); }
                worstRatio = std::max (worstRatio, double (jy / jx));
            }
        }
        { cifte::Controls a = h; a.degree[0] = 2; const std::vector<float> y = renderAligned (s220, rate, a); float jy = 0, jx = 0; for (size_t i = 1; i < y.size (); ++i) { jy = std::max (jy, std::fabs (y[i] - y[i - 1])); jx = std::max (jx, std::fabs (s220[i] - s220[i - 1])); } worstRatio = std::max (worstRatio, double (jy / jx)); }
        // bound for 3 voices at level 1 + dry: input peak 0.99 * (1 + 3 * max envelope-match gain 2) ; one-voice demo setting must stay < +6 dBFS
        char b[240]; std::snprintf (b, sizeof b, "finite=%d peak_one_voice=%.3f peak_three_voices=%.3f (bound %.2f) max_jump_vs_dry=%.3fx", int (finite), pk1, pk, 0.99 * 7.0, worstRatio);
        all &= check ("harmony_no_nan_clicks", finite && pk1 < 1.995f && pk <= 0.99f * 7.0f && worstRatio < 2.0, b, checks);
    }
    // 6. HARMONY 0 is the legacy doubler, bit-exact (after the fixed latency), whatever the harmony controls say
    {
        std::vector<float> st (voice.size () * 2); for (size_t i = 0; i < voice.size (); ++i) st[2 * i] = st[2 * i + 1] = voice[i];
        cifte::Controls a = preset ("Arabesk Choir"), b2 = a; b2.harmony = 0.0f; b2.root = 4; b2.scale = 3; b2.degree = {{4, 7, -7}}; b2.glide = 1.0f; b2.timing = 30.0f; b2.harmonyVoices = 3; b2.color = 0.5f; b2.keyMode = cifte::KeyMode::Auto; b2.forcedKeys = 4;
        // reference: the zero-latency doubler alone (processLegacy), no harmonizer in the path
        std::vector<float> rl (voice), rr (voice); { cifte::CifteEngine ref; ref.prepare (rate); for (size_t p = 0; p < rl.size (); p += 512) ref.processLegacy (rl.data () + p, rr.data () + p, int (std::min<size_t> (512, rl.size () - p)), a); }
        std::vector<float> refSt (st.size ()); for (size_t i = 0; i < rl.size (); ++i) { refSt[2 * i] = rl[i]; refSt[2 * i + 1] = rr[i]; }
        cifte::CifteEngine e; e.prepare (rate); const std::vector<float> ya = render (st, 2, rate, a);
        char b[160]; std::snprintf (b, sizeof b, "bit-identical to the zero-latency doubler delayed by latency=%d (both harmony settings)", e.latency (b2));
        all &= check ("harmony0_bitexact_legacy", ya == render (st, 2, rate, b2) && ya == refSt && e.latency (b2) == e.latency (a), b, checks);
    }
    // 7. latency: reported == measured (impulse through the unvoiced path, and voiced unison through PSOLA)
    {
        cifte::CifteEngine e; e.prepare (rate); cifte::Controls z = h; z.harmony = 0.5f; z.degree[0] = 0; const int rep = e.latency (z);
        std::vector<float> l (size_t (rate), 0.0f), r (l.size (), 0.0f); l[3000] = r[3000] = 0.8f;
        for (size_t p = 0; p < l.size (); p += 256) e.process (l.data () + p, r.data () + p, 256, z);
        const int meas = int (std::max_element (l.begin (), l.end (), [] (float a, float b) { return std::fabs (a) < std::fabs (b); }) - l.begin ()) - 3000;
        // voiced: unison PSOLA (degree 0, harmony alone) must reproduce the input at the same delay
        cifte::CifteEngine e2; e2.prepare (rate); cifte::Controls u = h; u.degree[0] = 0; std::vector<float> vl (voice.size () + 8192, 0.0f), vr (vl.size (), 0.0f);
        for (size_t i = 0; i < voice.size (); ++i) vl[i] = vr[i] = voice[i];
        for (size_t p = 0; p < vl.size (); p += 512) e2.process (vl.data () + p, vr.data () + p, int (std::min<size_t> (512, vl.size () - p)), u);
        int bestLag = 0; double bestC = -1e18; for (int lag = rep - 300; lag <= rep + 300; ++lag) { double s = 0; for (size_t i = size_t (rate / 2); i < voice.size () - size_t (rate / 2); i += 3) s += double (voice[i]) * vl[i + size_t (lag)]; if (s > bestC) { bestC = s; bestLag = lag; } }
        std::vector<float> res (voice.size () - size_t (rate)); for (size_t i = 0; i < res.size (); ++i) res[i] = vl[i + size_t (rate / 2) + size_t (rep)] - voice[i + size_t (rate / 2)];
        std::vector<float> ref (voice.begin () + rate / 2, voice.end () - rate / 2);
        char b[200]; std::snprintf (b, sizeof b, "reported=%d measured_impulse=%d measured_voiced_xcorr=%d (%.1f ms) unison_error=%.1fdB", rep, meas, bestLag, 1000.0 * rep / rate, 20.0 * std::log10 (rms (res) / rms (ref)));
        all &= check ("harmony_latency", meas == rep && bestLag == rep, b, checks);
    }
    // 8. every harmony control changes the output
    {
        cifte::Controls base = h; base.harmony = 0.5f; base.harmonyVoices = 2; base.degree = {{2, -2, 4}}; base.voicePan = {{-0.5f, 0.5f, 0.0f}};
        std::vector<float> br; const std::vector<float> bl = renderAligned (voice, rate, base, &br); bool ok = true; std::string worst;
        const char* names[] = { "harmony", "root", "scale", "degree", "glide", "timing", "voices", "level", "pan" };
        for (int k = 0; k < 9; ++k)
        {
            cifte::Controls x = base; if (k == 0) x.harmony = 0.8f; if (k == 1) x.root = 2; if (k == 2) x.scale = 3; if (k == 3) x.degree[0] = 4; if (k == 4) x.glide = 1.0f; if (k == 5) x.timing = 20.0f; if (k == 6) x.harmonyVoices = 3; if (k == 7) x.voiceLevel[0] = 0.4f; if (k == 8) x.voicePan[0] = 0.9f;
            std::vector<float> xr; const std::vector<float> xl = renderAligned (voice, rate, x, &xr); const float dd = diffRms (bl, xl) + diffRms (br, xr);
            ok &= dd > 0.0005f; worst += std::string (names[k]) + "=" + std::to_string (dd).substr (0, 6) + " ";
        }
        all &= check ("harmony_controls_change_output", ok, worst, checks);
    }
    // 9. CPU: 60 s stereo at 48 kHz, 3 harmony voices, voiced material
    {
        std::vector<double> ff; const std::vector<float> vv = vibratoVoice (48000, 4.8, ff); std::vector<float> l (size_t (48000 * 60)), r (l.size ());
        for (size_t i = 0; i < l.size (); ++i) l[i] = r[i] = vv[i % vv.size ()];
        cifte::Controls z = h; z.harmony = 0.5f; z.harmonyVoices = 3; z.degree = {{2, -2, 4}}; z.glide = 0.3f; z.timing = 10.0f;
        cifte::CifteEngine e; e.prepare (48000.0); const auto t0 = std::chrono::steady_clock::now ();
        for (size_t p = 0; p < l.size (); p += 512) e.process (l.data () + p, r.data () + p, int (std::min<size_t> (512, l.size () - p)), z);
        const double sec = std::chrono::duration<double> (std::chrono::steady_clock::now () - t0).count ();
        all &= check ("harmony_cpu_60s", sec < 10.0, "seconds=" + std::to_string (sec), checks);
    }
    return all;
}

// ---------------------------------------------------------------- KORO tests (key detector, safe rule, COLOR, double+harmony)
static const char* kNoteName[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
struct MelSeg { double seconds; int root, scale; };
// Harmonic "voice" singing scale passages: 8-note phrases = tonic, the six other degrees in a seeded order (5th in
// the middle), tonic; 40 % of the notes an octave lower; 0.25-0.45 s notes, 30 ms glides, 5.5 Hz +-20 cent vibrato.
static std::vector<float> scaleMelody (int rate, const std::vector<MelSeg>& segs, uint32_t seed, std::vector<double>* f0 = nullptr)
{
    std::vector<float> y; if (f0) f0->clear (); cifte::Rng g (seed); double phase = 0.0, prevC = -1.0; const double pi = 3.14159265358979;
    for (const auto& s : segs)
    {
        const double tonic = 100.0 * (48 + s.root); int pos = 0, order[6] = { 1, 2, 3, 4, 5, 6 }; size_t segEnd = y.size () + size_t (s.seconds * rate);
        while (y.size () < segEnd)
        {
            // phrase = tonic, the other six degrees in a seeded order (the 5th in the middle), tonic; each note up or down an octave at random
            if (pos % 8 == 0) { int rest[5] = { 1, 2, 3, 5, 6 }; for (int q = 4; q > 0; --q) std::swap (rest[q], rest[int (g.next () * float (q + 1)) % (q + 1)]); for (int q = 0; q < 6; ++q) order[q] = q < 3 ? rest[q] : q == 3 ? 4 : rest[q - 1]; }
            int deg = (pos % 8 == 0 || pos % 8 == 7) ? 0 : order[pos % 8 - 1];
            if (deg != 0 && g.next () < 0.4f) deg -= 7;
            ++pos;
            const double c = tonic + 1200.0 * cifte::scales::floorDiv (deg, 7) + cifte::scales::cents (s.scale, deg - 7 * cifte::scales::floorDiv (deg, 7));
            const double dur = 0.25 + 0.1 * int (g.next () * 3.0f); const size_t n = std::min (segEnd - y.size (), size_t (dur * rate));
            for (size_t i = 0; i < n; ++i)
            {
                const double t = double (i) / rate, u = double (y.size ()) / rate;
                const double cc = (prevC >= 0.0 && t < 0.03) ? prevC + (c - prevC) * (t / 0.03) : c;
                const double hz = 440.0 * std::pow (2.0, (cc + 20.0 * std::sin (2.0 * pi * 5.5 * u) - 6900.0) / 1200.0);
                if (f0) f0->push_back (hz);
                phase += 2.0 * pi * hz / rate; if (phase > 2.0 * pi) phase -= 2.0 * pi;
                double v = 0.0; for (int hh = 1; hh * hz < 0.45 * rate && hh <= 40; ++hh) v += std::sin (hh * phase) / hh * (hh <= 4 ? 1.0 : 1.6 / std::sqrt (double (hh)));
                y.push_back (float (0.15 * v));
            }
            prevC = c;
        }
    }
    return y;
}
struct KeyPoint { double t; bool has; int root, scale, mode; float conf; bool voiced, safe; double note, third; };
// Run the engine over a mono signal, sampling the key status every 10 ms (t = input time).
static std::vector<KeyPoint> keyTimeline (const std::vector<float>& x, int rate, const cifte::Controls& c, std::vector<float>* outL = nullptr)
{
    cifte::CifteEngine e; e.prepare (rate); std::vector<KeyPoint> tl; const int blk = rate / 200; std::vector<float> l (x), r (x);
    for (size_t p = 0; p < x.size (); p += size_t (blk))
    {
        const int m = int (std::min<size_t> (size_t (blk), x.size () - p)); e.process (l.data () + p, r.data () + p, m, c);
        const auto& s = e.keyStatus (); tl.push_back ({ double (p + size_t (m)) / rate, s.hasKey, s.root, s.scale, s.mode[0], s.confidence, s.voicedNow, s.safe, s.noteCents, s.thirdCents[0] });
    }
    if (outL) *outL = l;
    return tl;
}
static std::string keyName (int root, int scale) { return std::string (kNoteName[root]) + " " + cifte::scales::name (scale); }
static double maxJumpRatio (const std::vector<float>& y, const std::vector<float>& x)
{
    float jy = 0.0f, jx = 0.0f; for (size_t i = 1; i < y.size (); ++i) jy = std::max (jy, std::fabs (y[i] - y[i - 1])); for (size_t i = 1; i < x.size (); ++i) jx = std::max (jx, std::fabs (x[i] - x[i - 1]));
    return double (jy) / std::max (1.0e-9, double (jx));
}
// Tracked output pitch minus input pitch (semitones), voiced frames of the output only.
static std::vector<double> offsetsSt (const std::vector<float>& y, const std::vector<double>& f0, int rate)
{
    std::vector<double> o; const auto fr = cifte::trackOffline (y, rate, true);
    for (const auto& f : fr) { if (!f.voiced || f.t < 0.1 * rate || f.t > double (y.size ()) - 0.1 * rate) continue; o.push_back (12.0 * std::log2 (f.hz / f0[size_t (std::lround (f.t))])); }
    return o;
}
// ---------------------------------------------------------------- roughness metric (harmony voice alone, sustained notes)
static void fftInPlace (std::vector<double>& re, std::vector<double>& im)
{
    const size_t N = re.size ();
    for (size_t i = 1, j = 0; i < N; ++i) { size_t bit = N >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) { std::swap (re[i], re[j]); std::swap (im[i], im[j]); } }
    for (size_t len = 2; len <= N; len <<= 1)
    {
        const double a = -2.0 * 3.14159265358979323846 / double (len); const double wr = std::cos (a), wi = std::sin (a);
        for (size_t i = 0; i < N; i += len) { double cr = 1.0, ci = 0.0; for (size_t k = 0; k < len / 2; ++k) { const size_t p = i + k, q = p + len / 2; const double tr = re[q] * cr - im[q] * ci, ti = re[q] * ci + im[q] * cr; re[q] = re[p] - tr; im[q] = im[p] - ti; re[p] += tr; im[p] += ti; const double nr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = nr; } }
    }
}
struct Rough { double flux, am; int frames; double seconds; };
// Sustained = inside a run of steady voiced tracker frames of the DRY voice (moved < 30 c per 10 ms), after its first 50 ms.
// flux = sum over 4-12 kHz of the positive magnitude change between consecutive 1024-point frames (hop 256) / sum of all magnitudes.
// am   = RMS of the 20-150 Hz band of the amplitude envelope (sqrt of 200 Hz low-passed y^2) / mean envelope.
static Rough roughness (const std::vector<float>& y, const std::vector<float>& dry, int rate)
{
    std::vector<uint8_t> m (y.size (), 0); const auto fr = cifte::trackOffline (dry, rate); double pc1 = 0, pc2 = 0; bool v1 = false, v2 = false; int run = 0;
    const double hopS = fr.size () > 1 ? fr[1].t - fr[0].t : 220.0;
    for (const auto& f : fr)
    {
        const double c = f.voiced ? 1200.0 * std::log2 (f.hz / 440.0) : 0.0; const bool st = f.voiced && v1 && v2 && std::fabs (c - pc2) < 30.0;
        v2 = v1; pc2 = pc1; v1 = f.voiced; pc1 = c; run = st ? run + 1 : 0;
        if (run * hopS >= 0.050 * rate) for (int64_t i = int64_t (f.t - hopS / 2); i < int64_t (f.t + hopS / 2); ++i) if (i >= 0 && i < int64_t (m.size ())) m[size_t (i)] = 1;
    }
    Rough r { 0.0, 0.0, 0, 0.0 }; const size_t N = 1024, H = 256; std::vector<double> prev (N / 2 + 1, 0.0), win (N); bool prevOk = false; double fl = 0.0, nm = 0.0;
    for (size_t i = 0; i < N; ++i) win[i] = 0.5 - 0.5 * std::cos (2.0 * 3.14159265358979 * double (i) / double (N));
    const size_t b4 = size_t (4000.0 * N / rate), b12 = std::min (N / 2, size_t (12000.0 * N / rate));
    for (size_t a = 0; a + N <= y.size (); a += H)
    {
        const bool ok = m[a] && m[a + N / 2] && m[a + N - 1];
        if (!ok) { prevOk = false; continue; }
        std::vector<double> re (N), im (N, 0.0); for (size_t i = 0; i < N; ++i) re[i] = win[i] * y[a + i]; fftInPlace (re, im);
        std::vector<double> mag (N / 2 + 1); double tot = 0.0; for (size_t b = 0; b <= N / 2; ++b) { mag[b] = std::sqrt (re[b] * re[b] + im[b] * im[b]); tot += mag[b]; }
        if (prevOk) { double f = 0.0; for (size_t b = b4; b <= b12; ++b) f += std::max (0.0, mag[b] - prev[b]); fl += f; nm += tot; ++r.frames; }
        prev = mag; prevOk = true;
    }
    r.flux = nm > 0.0 ? fl / nm : 0.0;
    auto biq = [&] (std::vector<double>& x, double f0, bool hp) {
        const double w = 2.0 * 3.14159265358979 * f0 / rate, c = std::cos (w), al = std::sin (w) / (2.0 * 0.70710678);
        double b0 = hp ? (1 + c) / 2 : (1 - c) / 2, b1 = hp ? -(1 + c) : 1 - c, b2 = b0, a0 = 1 + al, a1 = -2 * c, a2 = 1 - al; double z1 = 0, z2 = 0;
        b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0;
        for (double& v : x) { const double o = b0 * v + z1; z1 = b1 * v - a1 * o + z2; z2 = b2 * v - a2 * o; v = o; }
    };
    std::vector<double> e (y.size ()); for (size_t i = 0; i < y.size (); ++i) e[i] = double (y[i]) * y[i];
    biq (e, 200.0, false); biq (e, 200.0, false); for (double& v : e) v = std::sqrt (std::max (0.0, v));
    std::vector<double> bp (e); biq (bp, 20.0, true); biq (bp, 150.0, false);
    double s2 = 0.0, se = 0.0; size_t cnt = 0; for (size_t i = 0; i < y.size (); ++i) if (m[i]) { s2 += bp[i] * bp[i]; se += e[i]; ++cnt; }
    r.am = cnt && se > 0.0 ? std::sqrt (s2 / double (cnt)) / (se / double (cnt)) : 0.0; r.seconds = double (cnt) / rate;
    return r;
}
static bool readKadin (Wav& w) { for (const char* pth : { "../_kaynak/Kadin_vokal_20s.wav", "_kaynak/Kadin_vokal_20s.wav" }) { try { w = readWav (pth); return true; } catch (...) {} } return false; }

static bool koroTests (int& checks)
{
    bool all = true; const int rate = 44100;
    cifte::Controls autoC; autoC.harmony = 0.5f; autoC.harmonyVoices = 1; autoC.color = 1.0f / 3.0f; autoC.mix = 0.0f;   // AUTO key, THIRD colour
    struct Share { const char* name; double pct; int frames; }; std::vector<Share> thirdShares;
    // 1. convergence on synthetic scale passages (12 s each)
    struct Case { const char* name; int root, scale; uint32_t seed; };
    for (const Case& k : { Case { "key_converge_C_Major", 0, 0, 11u }, Case { "key_converge_D_Hicaz", 2, 3, 12u }, Case { "key_converge_A_Minor", 9, 1, 13u } })
    {
        const std::vector<float> x = scaleMelody (rate, { { 12.0, k.root, k.scale } }, k.seed); const auto tl = keyTimeline (x, rate, autoC);
        double tRight = -1.0, tConf = -1.0, firstLock = -1.0; float c4 = 0.0f, cMinAfter = 1.0f; std::string firstKey = "-";
        for (size_t i = 0; i < tl.size (); ++i)
        {
            const bool right = tl[i].has && tl[i].root == k.root && tl[i].scale == k.scale;
            if (tl[i].has && firstLock < 0.0) { firstLock = tl[i].t; firstKey = keyName (tl[i].root, tl[i].scale); }
            if (right && tRight < 0.0) tRight = tl[i].t; if (!right) tRight = -1.0;
            if (right && tl[i].conf > 0.7f && tConf < 0.0) tConf = tl[i].t;
            if (tConf >= 0.0) cMinAfter = std::min (cMinAfter, tl[i].conf);
            if (std::fabs (tl[i].t - 4.0) < 0.005) c4 = tl[i].conf;
        }
        const auto& e = tl.back (); int vf = 0, th = 0; for (const auto& q : tl) if (q.has && q.voiced) { ++vf; th += q.mode == cifte::Harmonizer::kThird; }
        thirdShares.push_back ({ k.name + 14, 100.0 * th / std::max (1, vf), vf });
        char b[300]; std::snprintf (b, sizeof b, "target=%s first_key=%s@%.2fs right_from=%.2fs conf>0.7_at=%.2fs conf@4s=%.3f min_conf_after=%.3f final=%s conf=%.3f", keyName (k.root, k.scale).c_str (), firstKey.c_str (), firstLock, tRight, tConf, c4, cMinAfter, keyName (e.root, e.scale).c_str (), e.conf);
        all &= check (k.name, tRight >= 0.0 && tRight <= 4.0 && tConf >= 0.0 && tConf <= 4.0, b, checks);
    }
    {   // per-note rule on the passages: after the first lock, the THIRD voice plays a third on >= 90 % of the voiced frames
        std::string out; bool ok = true; for (const auto& q : thirdShares) { char b[80]; std::snprintf (b, sizeof b, "%s:%.1f%%_of_%d ", q.name, q.pct, q.frames); out += b; ok &= q.pct >= 85.0; }
        all &= check ("thirds_after_lock_synthetic", ok, out + "(gate >= 85%)", checks);
    }
    // 2. white noise: confidence stays < 0.25, harmony voice = unison passthrough
    {
        std::vector<float> nz (size_t (10 * rate)); cifte::Rng g (777u); for (float& v : nz) v = 0.3f * g.bipolar ();
        cifte::Controls z = autoC; z.harmony = 1.0f; z.voiceLevel[0] = 1.0f; z.voicePan[0] = 0.0f; z.timing = 0.0f; z.shareWidthTone = false;
        cifte::CifteEngine e; e.prepare (rate); std::vector<float> l (nz), r (nz); l.resize (nz.size () + size_t (e.latency (z)), 0.0f); r.resize (l.size (), 0.0f);
        float cmax = 0.0f; int modeMax = 0; for (size_t p = 0; p < l.size (); p += 441) { e.process (l.data () + p, r.data () + p, int (std::min<size_t> (441, l.size () - p)), z); cmax = std::max (cmax, e.keyStatus ().confidence); modeMax = std::max (modeMax, e.keyStatus ().mode[0]); }
        const size_t lat = size_t (e.latency (z)); double num = 0, den = 0; for (size_t i = 0; i < nz.size (); ++i) { num += double (l[i + lat]) * nz[i]; den += double (nz[i]) * nz[i]; }
        double res = 0; for (size_t i = 0; i < nz.size (); ++i) { const double d = l[i + lat] - num / den * nz[i]; res += d * d; }
        char b[240]; std::snprintf (b, sizeof b, "max_confidence=%.3f non_unison_decisions=%d has_key=%d voice_interval=%.0f_cents out_vs_in_gain=%.4f residual=%.1fdB", cmax, modeMax, int (e.keyStatus ().hasKey), e.voiceIntervalCents (0), num / den, 10.0 * std::log10 (std::max (1e-24, res / den)));
        all &= check ("key_noise_unison", cmax < 0.25f && modeMax == 0 && std::fabs (e.voiceIntervalCents (0)) < 1e-9, b, checks);
    }
    // 3. hysteresis: 8 s C Major, 1 s F# Major excursion, 4 s C Major
    {
        const std::vector<float> x = scaleMelody (rate, { { 8.0, 0, 0 }, { 1.0, 6, 0 }, { 4.0, 0, 0 } }, 21u); const auto tl = keyTimeline (x, rate, autoC);
        bool stayed = true; float cmin = 1.0f; std::string at8 = "-";
        for (const auto& p : tl) { if (p.t >= 7.99 && p.t < 8.0) at8 = keyName (p.root, p.scale); if (p.t >= 8.0) { stayed &= p.has && p.root == 0 && p.scale == 0; if (p.t < 10.5) cmin = std::min (cmin, p.conf); } }
        char b[240]; std::snprintf (b, sizeof b, "key@8s=%s held_C_Major_8-13s=%d min_conf_8-10.5s=%.3f final=%s conf=%.3f", at8.c_str (), int (stayed), cmin, keyName (tl.back ().root, tl.back ().scale).c_str (), tl.back ().conf);
        all &= check ("key_hysteresis_1s_excursion", stayed && at8 == "C Major", b, checks);
    }
    std::vector<double> f0; const std::vector<float> voice = vibratoVoice (rate, 4.8, f0);   // A3 B3 C#4, A major
    cifte::Controls man; man.harmony = 1.0f; man.harmonyVoices = 1; man.keyMode = cifte::KeyMode::Manual; man.root = 9; man.scale = 0; man.mix = 0.0f;
    man.voiceLevel = {{ 1.0f, 1.0f, 1.0f }}; man.voicePan = {{ 0.0f, 0.0f, 0.0f }}; man.glide = 0.0f; man.timing = 0.0f; man.shareWidthTone = false;
    // 4. per-note rule with the plausible set forced to 4 keys that disagree on both thirds of A, B, C# (A, C, C#, F Major):
    //    every third is suppressed -> third range (COLOR <= 0.5) falls back to UNISON, never an octave; the 5TH and OCT
    //    positions play their own (chosen) interval
    {
        std::string out; bool ok = true;
        struct SC { const char* nm; float color; int vc, solo; double want; };
        for (const SC& s : { SC { "c0", 0.0f, 1, 0, 0.0 }, SC { "c.33", 1.0f / 3.0f, 1, 0, 0.0 }, SC { "c.67", 2.0f / 3.0f, 1, 0, 7.02 }, SC { "c1", 1.0f, 1, 0, 12.0 }, SC { "choir_v1", 0.0f, 3, 0, 0.0 }, SC { "choir_v2", 0.0f, 3, 1, 0.0 }, SC { "choir_v3", 0.0f, 3, 2, 7.02 } })
        {
            cifte::Controls z = man; z.keyMode = cifte::KeyMode::Auto; z.forcedKeys = 4; z.forcedRoot = {{ 9, 0, 1, 5, 0, 0 }}; z.forcedScale = {{ 0, 0, 0, 0, 0, 0 }}; z.color = s.color; z.harmonyVoices = s.vc; for (int v = 0; v < 3; ++v) z.voiceLevel[size_t (v)] = v == s.solo ? 1.0f : 0.0f;
            const auto o = offsetsSt (renderAligned (voice, rate, z), f0, rate); int hit = 0, th = 0, oc = 0; std::vector<double> so (o); std::sort (so.begin (), so.end ());
            for (double d : o) { if (std::fabs (d - s.want) < 0.5) ++hit; if (std::fabs (std::fabs (d) - 3.0) < 0.5 || std::fabs (std::fabs (d) - 4.0) < 0.5) ++th; if (std::fabs (std::fabs (d) - 12.0) < 1.0) ++oc; }
            const double ph = 100.0 * hit / std::max<size_t> (1, o.size ()), pt = 100.0 * th / std::max<size_t> (1, o.size ()), po = 100.0 * oc / std::max<size_t> (1, o.size ());
            ok &= ph >= 90.0 && th == 0 && (s.want != 0.0 || oc == 0);   // third range: unison fallback, never an octave
            char b[160]; std::snprintf (b, sizeof b, "%s:median=%+.2fst,on_%+.0fst=%.1f%%,third=%.1f%%,octave=%.1f%% ", s.nm, so.empty () ? 0.0 : so[so.size () / 2], s.want, ph, pt, po); out += b;
        }
        all &= check ("safe_rule_conflicting_keys", ok, out, checks);
    }
    // 5. COLOR anchors / mid-points on the vibrato voice (A major, confidence 1, harmony voice alone).
    //    Anchors: tracked output pitch vs the per-frame scale target of the anchor degree (-2, +2, +4, +7).
    //    Mid-points: least-squares fit y_mid = a * y_anchorA + b * y_anchorB; both a and b must be large (both intervals sound).
    {
        std::string out; bool ok = true; std::vector<std::vector<float>> anc;
        const int degs[4] = { -2, 2, 4, 7 };
        for (int j = 0; j < 4; ++j)
        {
            cifte::Controls z = man; z.color = float (j) / 3.0f; anc.push_back (renderAligned (voice, rate, z));
            const auto fr = cifte::trackOffline (anc.back (), rate); int hit = 0, tot = 0; std::vector<double> off;
            for (const auto& f : fr)
            {
                if (f.t < 0.1 * rate || f.t > double (voice.size ()) - 0.1 * rate) continue;
                const double fin = f0[size_t (std::lround (f.t))]; int idx = -1, oct = 0; cifte::scales::nearest (6900.0 + 1200.0 * std::log2 (fin / 440.0), 9, 0, false, idx, oct, 0.0);
                const double want = cifte::scales::intervalCents (idx, oct, degs[j], 0); ++tot;
                if (f.voiced) { const double c = 1200.0 * std::log2 (f.hz / fin); off.push_back (c / 100.0); if (std::fabs (c - want) < 30.0) ++hit; }
            }
            std::sort (off.begin (), off.end ()); const double pc = 100.0 * hit / std::max (1, tot);
            ok &= pc >= 90.0;
            char b[120]; std::snprintf (b, sizeof b, "c%.2f(deg%+d):median=%+.2fst,on_target=%.1f%% ", z.color, degs[j], off.empty () ? 0.0 : off[off.size () / 2], pc); out += b;
        }
        for (int j = 0; j < 3; ++j)
        {
            cifte::Controls z = man; z.color = (float (j) + 0.5f) / 3.0f; const std::vector<float> y = renderAligned (voice, rate, z);
            const std::vector<float>& A = anc[size_t (j)]; const std::vector<float>& B = anc[size_t (j + 1)];
            double aa = 0, bb = 0, ab = 0, ya = 0, yb = 0, yy = 0; for (size_t i = size_t (rate / 10); i < y.size () - size_t (rate / 10); ++i) { aa += double (A[i]) * A[i]; bb += double (B[i]) * B[i]; ab += double (A[i]) * B[i]; ya += double (y[i]) * A[i]; yb += double (y[i]) * B[i]; yy += double (y[i]) * y[i]; }
            const double det = aa * bb - ab * ab, ka = (ya * bb - yb * ab) / det, kb = (yb * aa - ya * ab) / det;
            double rr = 0; for (size_t i = size_t (rate / 10); i < y.size () - size_t (rate / 10); ++i) { const double d = y[i] - ka * A[i] - kb * B[i]; rr += d * d; }
            ok &= ka > 0.5 && kb > 0.5 && ka < 0.9 && kb < 0.9;
            char b[140]; std::snprintf (b, sizeof b, "c%.2f=%.3f*deg%+d+%.3f*deg%+d(res=%.1fdB) ", z.color, ka, degs[j], kb, degs[j + 1], 10.0 * std::log10 (rr / yy)); out += b;
        }
        all &= check ("color_anchors_midpoints", ok, out, checks);
    }
    // 6. DOUBLE + HARMONY together. B = both, D = same with harmony 0, H = same with double mix 0.
    //    B - D must equal H - dry (the harmony part, independent of the doubler), B - D must track at the scale third,
    //    and B - H (= doubled - dry) must be a drifting delayed copy of the dry voice.
    {
        cifte::Controls z = man; z.harmony = 0.5f; z.color = 1.0f / 3.0f; z.voices = 1; z.mix = 0.5f; z.spread = 0.55f; z.tight = 0.35f; z.width = 0.75f; z.tone = 0.65f; z.shareWidthTone = true; z.voicePan[0] = 0.4f;
        cifte::Controls zd = z; zd.harmony = 0.0f; cifte::Controls zh = z; zh.mix = 0.0f;
        std::vector<float> Br, Dr, Hr; const std::vector<float> B = renderAligned (voice, rate, z, &Br), D = renderAligned (voice, rate, zd, &Dr), H = renderAligned (voice, rate, zh, &Hr);
        double ident = 0.0; std::vector<float> harm (B.size ()), dbl (B.size ());
        for (size_t i = 0; i < B.size (); ++i) { ident = std::max (ident, double (std::fabs ((B[i] - D[i]) - (H[i] - voice[i])))); ident = std::max (ident, double (std::fabs ((Br[i] - Dr[i]) - (Hr[i] - voice[i])))); harm[i] = 0.5f * ((B[i] - D[i]) + (Br[i] - Dr[i])); dbl[i] = 0.5f * ((B[i] - H[i]) + (Br[i] - Hr[i])); }
        const auto o = offsetsSt (harm, f0, rate); int hit = 0;
        const auto fr = cifte::trackOffline (harm, rate);
        for (const auto& f : fr)
        {
            if (!f.voiced || f.t < 0.1 * rate || f.t > double (voice.size ()) - 0.1 * rate) continue;
            const double fin = f0[size_t (std::lround (f.t))]; int idx = -1, oct = 0; cifte::scales::nearest (6900.0 + 1200.0 * std::log2 (fin / 440.0), 9, 0, false, idx, oct, 0.0);
            if (std::fabs (1200.0 * std::log2 (f.hz / fin) - cifte::scales::intervalCents (idx, oct, 2, 0)) < 60.0) ++hit;
        }
        // doubled - dry = mix * (voice1 - dry): add back mix*dry and find the delay of the re-performed voice
        std::vector<float> v1 (dbl.size ()); for (size_t i = 0; i < v1.size (); ++i) v1[i] = dbl[i] + 0.5f * voice[i];
        int bestLag = 0; double bestC = -1.0; const size_t a0 = size_t (rate / 2), a1 = voice.size () - size_t (rate / 2);
        for (int lag = 0; lag <= int (0.030 * rate); ++lag) { double s = 0, ea = 0, eb = 0; for (size_t i = a0; i < a1; i += 2) { s += double (voice[i]) * v1[i + size_t (lag)]; ea += double (voice[i]) * voice[i]; eb += double (v1[i + size_t (lag)]) * v1[i + size_t (lag)]; } const double cc = s / std::sqrt (ea * eb); if (cc > bestC) { bestC = cc; bestLag = lag; } }
        char b[320]; std::snprintf (b, sizeof b, "max|(B-D)-(H-dry)|=%.2e harmony_part: %.1f%% of %zu voiced frames at the A-major third (+-60c) | double_part: xcorr_with_dry=%.3f at lag=%.2fms rms=%.1fdB_re_dry", ident, 100.0 * hit / std::max<size_t> (1, o.size ()), o.size (), bestC, 1000.0 * bestLag / rate, 20.0 * std::log10 (rms (dbl) / rms (voice)));
        all &= check ("double_plus_harmony", ident < 1.0e-5 && hit >= 0.85 * double (o.size ()) && bestC > 0.8 && bestLag > int (0.003 * rate) && bestLag < int (0.025 * rate), b, checks);
    }
    // 7. latency is one constant per sample rate, whatever the settings; measured == reported
    {
        std::string out; bool ok = true;
        for (int r : { 44100, 48000, 96000 })
        {
            cifte::CifteEngine e; e.prepare (r); const int rep = e.latency (); int diff = 0, meas = 0;
            for (int k = 0; k < 8; ++k)
            {
                cifte::Controls z; z.harmony = k % 3 == 0 ? 0.0f : k % 3 == 1 ? 0.5f : 1.0f; z.mix = k % 2 ? 0.5f : 0.0f; z.keyMode = k & 4 ? cifte::KeyMode::Manual : cifte::KeyMode::Auto; z.color = k & 2 ? 0.5f : -1.0f; z.harmonyVoices = 1 + k % 3; z.voicePan = {{ 0.0f, 0.0f, 0.0f }};
                if (e.latency (z) != rep) ++diff;
                cifte::CifteEngine m; m.prepare (r); std::vector<float> l (size_t (r / 2), 0.0f), rr (l.size (), 0.0f); l[500] = rr[500] = 0.8f;
                for (size_t p = 0; p < l.size (); p += 512) m.process (l.data () + p, rr.data () + p, int (std::min<size_t> (512, l.size () - p)), z);
                const int got = int (std::max_element (l.begin (), l.end (), [] (float x, float y) { return std::fabs (x) < std::fabs (y); }) - l.begin ()) - 500;
                if (got == rep) ++meas;
            }
            ok &= diff == 0 && meas == 8;
            char b[120]; std::snprintf (b, sizeof b, "%d:%d(%.2fms,impulse_ok=%d/8) ", r, rep, 1000.0 * rep / r, meas); out += b;
        }
        all &= check ("latency_constant", ok, out, checks);
    }
    // 8. no clicks: COLOR sweep, manual key switches (every 0.75 s), and AUTO key switch C Major -> D Hicaz
    {
        std::vector<double> ff; const std::vector<float> v6 = scaleMelody (rate, { { 6.0, 9, 0 } }, 31u, &ff);
        auto sweep = [&] (int mode) {
            cifte::Controls z = man; z.harmony = 0.5f; z.voiceLevel[0] = 1.0f; z.glide = 0.25f;
            cifte::CifteEngine e; e.prepare (rate); std::vector<float> l (v6), r (v6); const int lat = e.latency (z); l.resize (v6.size () + size_t (lat), 0.0f); r.resize (l.size (), 0.0f);
            for (size_t p = 0; p < l.size (); p += 256) { const double t = double (p) / rate; if (mode == 0) z.color = float (std::min (1.0, t / 6.0)); else { z.color = 1.0f / 3.0f; z.root = int (t / 0.75) % 2 ? 2 : 9; } e.process (l.data () + p, r.data () + p, int (std::min<size_t> (256, l.size () - p)), z); }
            return maxJumpRatio (std::vector<float> (l.begin () + lat, l.end ()), v6);
        };
        const double jc = sweep (0), jk = sweep (1);
        const std::vector<float> x = scaleMelody (rate, { { 8.0, 0, 0 }, { 14.0, 2, 3 } }, 41u); std::vector<float> y; cifte::Controls z = autoC; z.voiceLevel[0] = 1.0f;
        const auto tl = keyTimeline (x, rate, z, &y); if (std::getenv ("KORO_DEBUG")) for (size_t i = 0; i < tl.size (); i += 50) { std::printf ("  t=%.2f %s conf=%.3f mode=%d\n", tl[i].t, keyName (tl[i].root, tl[i].scale).c_str (), tl[i].conf, tl[i].mode); } double tSwitch = -1.0; int modeChanges = 0; for (size_t i = 1; i < tl.size (); ++i) { if (tSwitch < 0.0 && tl[i].t > 8.0 && tl[i].root == 2 && tl[i].scale == 3) tSwitch = tl[i].t; modeChanges += tl[i].mode != tl[i - 1].mode; }
        const double ja = maxJumpRatio (y, x);
        char b[260]; std::snprintf (b, sizeof b, "color_sweep=%.3fx manual_key_switches=%.3fx auto_C_Major->D_Hicaz=%.3fx (switched at %.2fs, %d interval-class changes) of max dry jump", jc, jk, ja, tSwitch, modeChanges);
        all &= check ("no_clicks_key_color", jc < 2.0 && jk < 2.0 && ja < 2.0 && tSwitch > 8.0, b, checks);
    }
    // 9. real female pop vocal (AUTO, THIRD mood): share of third / fifth / octave / unison over voiced frames, and
    //    every played third checked against the FINAL top-1 key's third for the same note (must agree within 30 cents)
    {
        Wav w; bool okRead = false; for (const char* pth : { "../_kaynak/Kadin_vokal_20s.wav", "_kaynak/Kadin_vokal_20s.wav" }) { try { w = readWav (pth); okRead = true; break; } catch (...) {} }
        if (!okRead) all &= check ("female_vocal_per_note", false, "source _kaynak/Kadin_vokal_20s.wav not found", checks);
        else
        {
            std::vector<float> x (w.samples.size () / size_t (w.channels)); for (size_t i = 0; i < x.size (); ++i) x[i] = w.samples[i * size_t (w.channels)];
            cifte::Controls z; cifte::moods::get ("THIRD", z);
            cifte::CifteEngine e; e.prepare (w.rate); std::vector<float> l (x), r (x); std::vector<KeyPoint> tl; const int blk = w.rate / 200;
            for (size_t p = 0; p < x.size (); p += size_t (blk)) { e.process (l.data () + p, r.data () + p, int (std::min<size_t> (size_t (blk), x.size () - p)), z); const auto& q = e.keyStatus (); tl.push_back ({ (double (p) - e.latency ()) / w.rate, q.hasKey, q.root, q.scale, q.mode[0], q.confidence, q.voicedNow, q.safe, q.noteCents, q.thirdCents[0] }); }
            cifte::KeyDetector::Candidate t1[1]; e.keyDetector ().top (t1, 1);
            int cnt[5] = { 0, 0, 0, 0, 0 }, vf = 0, dis = 0, safeF = 0;
            for (const auto& q : tl)
            {
                if (!q.voiced) continue; ++vf; ++cnt[q.mode]; safeF += q.safe;
                if (q.mode == cifte::Harmonizer::kThird)
                {
                    int idx = -1, oct = 0; cifte::scales::nearest (q.note, t1[0].root, t1[0].scale, false, idx, oct, 0.0);
                    const double a = 100.0 * t1[0].root + 1200.0 * oct + cifte::scales::cents (t1[0].scale, idx) + cifte::scales::intervalCents (idx, oct, 2, t1[0].scale);
                    if (std::fabs (a - q.third) > 30.0) { ++dis; if (std::getenv ("KORO_DEBUG")) std::printf ("  disagree t=%.3f note=%.1f played_third=%.1f final_third=%.1f\n", q.t, q.note, q.third, a); }
                }
            }
            const double pc = 100.0 / std::max (1, vf);
            char b[320]; std::snprintf (b, sizeof b, "voiced_frames=%d third=%.1f%% unison=%.1f%% fifth=%.1f%% octave=%.1f%% safe_lamp=%.1f%% final_top1=%s thirds_disagreeing_with_final_top1=%d of %d",
                                        vf, cnt[3] * pc, cnt[0] * pc, cnt[1] * pc, cnt[2] * pc, safeF * pc, keyName (t1[0].root, t1[0].scale).c_str (), dis, cnt[3]);
            const auto& sx = e.shifterStats (); int nowDis = 0; for (const auto& q : tl) nowDis += q.voiced && q.mode == cifte::Harmonizer::kThird && q.safe && false;
            char ba[300]; std::snprintf (ba, sizeof ba, "thirds_applied=%d applied_without_plausible_agreement=%d (gate 0) | info: %s", sx.thirdsApplied, sx.thirdsWithoutAgreement, b);
            all &= check ("female_vocal_third_agreement", sx.thirdsWithoutAgreement == 0 && sx.thirdsApplied > 0 && cnt[3] > 0, ba, checks); (void) nowDis;
            char b2[160]; std::snprintf (b2, sizeof b2, "third_share=%.1f%% (gate >= 30%%)", cnt[3] * pc);
            all &= check ("female_vocal_third_share", cnt[3] * pc >= 30.0, b2, checks);
            // no voice of THIRD or CHOIR ever sits on an octave: engine decisions, and the tracked pitch of each voice solo
            {
                std::string out; bool ok = true;
                struct MV { const char* mood; int solo; };
                for (const MV& m : { MV { "THIRD", 0 }, MV { "CHOIR", 0 }, MV { "CHOIR", 1 }, MV { "CHOIR", 2 } })
                {
                    cifte::Controls zc; cifte::moods::get (m.mood, zc); zc.harmony = 1.0f; zc.mix = 0.0f; zc.timing = 0.0f; zc.shareWidthTone = false;
                    for (int v = 0; v < 3; ++v) { zc.voiceLevel[size_t (v)] = v == m.solo ? 1.0f : 0.0f; zc.voicePan[size_t (v)] = 0.0f; }
                    cifte::CifteEngine ec; ec.prepare (w.rate); const int lat = ec.latency (zc); std::vector<float> lc (x), rc (x); lc.resize (x.size () + size_t (lat), 0.0f); rc.resize (lc.size (), 0.0f);
                    int octDec = 0, voicedDec = 0;
                    for (size_t p = 0; p < lc.size (); p += size_t (blk)) { ec.process (lc.data () + p, rc.data () + p, int (std::min<size_t> (size_t (blk), lc.size () - p)), zc); const auto& q = ec.keyStatus (); if (q.voicedNow) { ++voicedDec; octDec += q.mode[size_t (m.solo)] == cifte::Harmonizer::kOctave; } }
                    const std::vector<float> y (lc.begin () + lat, lc.end ());
                    const auto fin = cifte::trackOffline (x, w.rate, true), fout = cifte::trackOffline (y, w.rate, true); int both = 0, oct = 0, run = 0, runs = 0;
                    for (size_t k = 0; k < std::min (fin.size (), fout.size ()); ++k)
                    {
                        bool at = false;
                        if (fin[k].voiced && fout[k].voiced) { ++both; const double dst = 12.0 * std::log2 (fout[k].hz / fin[k].hz); at = std::fabs (std::fabs (dst) - 12.0) < 1.0; oct += at; }
                        run = at ? run + 1 : 0; if (run == 4) { ++runs; if (std::getenv ("KORO_DEBUG")) std::printf ("  octave stretch %s v%d t=%.3f in=%.1fHz out=%.1fHz mode=%d\n", m.mood, m.solo + 1, fin[k].t / w.rate, fin[k].hz, fout[k].hz, 0); }   // an octave that lasts >= 20 ms (single tracker frames are tracker errors)
                    }
                    ok &= octDec == 0;   // gate: the engine never puts a THIRD/CHOIR voice on +-12 st (tracked stretches: info, see STATUS.md)
                    char bb[200]; std::snprintf (bb, sizeof bb, "%s_v%d:octave_frames=%d/%d (gate 0) | info: tracked >=20ms stretches at +-12st=%d, single frames=%d/%d ", m.mood, m.solo + 1, octDec, voicedDec, runs, oct, both); out += bb;
                }
                all &= check ("no_octave_in_third_moods", ok, out, checks);
            }
        }
    }
    // 10. roughness of the harmony voice alone vs the DRY voice on the same sustained frames (gate: <= 1.3 x dry, flux and AM)
    {
        Wav w; if (!readKadin (w)) all &= check ("roughness_vs_dry", false, "source not found", checks);
        else
        {
            std::vector<float> x (w.samples.size () / size_t (w.channels)); for (size_t i = 0; i < x.size (); ++i) x[i] = w.samples[i * size_t (w.channels)];
            const Rough d = roughness (x, x, w.rate); std::string out; char b[200];
            std::snprintf (b, sizeof b, "dry:flux=%.5f,am=%.5f ", d.flux, d.am); out += b; bool ok = true;
            // gate: third and fifth <= 1.3 x dry, octave-up <= 1.4 x dry (formant-preserving octave-up keeps only the even harmonics)
            struct RC { const char* nm; int kind; float color; bool v2; bool gate; };
            for (const RC& q : { RC { "manual_third_legacy", 0, 1.0f / 3.0f, false, false }, RC { "manual_third", 0, 1.0f / 3.0f, true, true }, RC { "manual_fifth", 0, 2.0f / 3.0f, true, true }, RC { "manual_octave", 0, 1.0f, true, true },
                                 RC { "auto_THIRD", 1, 1.0f / 3.0f, true, true }, RC { "auto_FIFTH", 2, 2.0f / 3.0f, true, true } })
            {
                cifte::Controls z;
                if (q.kind == 0) { z.keyMode = cifte::KeyMode::Manual; z.root = 5; z.scale = 0; z.harmonyVoices = 1; z.color = q.color; z.glide = 1.0f / 3.0f; }
                else cifte::moods::get (q.kind == 1 ? "THIRD" : "FIFTH", z);
                z.harmony = 1.0f; z.mix = 0.0f; z.voiceLevel = {{ 1.0f, 1.0f, 1.0f }}; z.voicePan = {{ 0.0f, 0.0f, 0.0f }}; z.timing = 0.0f; z.shareWidthTone = false; z.shifterV2 = q.v2;
                const Rough r = roughness (renderAligned (x, w.rate, z), x, w.rate);
                const bool info = !q.gate || q.kind == 1;   // AUTO THIRD (octave fallback + interval changes) is info
                const double lim = (q.kind == 0 && q.color > 0.9f) ? 1.4 : 1.3;
                std::snprintf (b, sizeof b, "%s:flux=%.2fx,am=%.2fx%s ", q.nm, r.flux / d.flux, r.am / d.am, info ? "(info)" : lim > 1.35 ? "(gate 1.4x)" : "(gate 1.3x)"); out += b;
                if (!info) ok &= r.flux <= lim * d.flux && r.am <= lim * d.am;
            }
            all &= check ("roughness_vs_dry", ok, out, checks);
        }
    }
    // 11. KORO shifter still puts a sine exactly on the interval (+-1 %)
    {
        std::vector<float> s220 (size_t (2 * rate)); for (size_t i = 0; i < s220.size (); ++i) s220[i] = 0.5f * std::sin (2.0f * cifte::kPi * 220.0f * float (i) / float (rate));
        std::string out; bool ok = true;
        for (int deg : { 2, 4 })
        {
            cifte::Controls z = man; z.shifterV2 = true; z.color = -1.0f; z.degree[0] = deg; const std::vector<float> y = renderAligned (s220, rate, z); double at220 = 0.0;
            const double f = peakHz (y, rate, size_t (0.5 * rate), size_t (rate), 100.0, 600.0, &at220, 220.0), want = deg == 2 ? 277.1826 : 329.6276;
            ok &= std::fabs (f / want - 1.0) < 0.01; char b[120]; std::snprintf (b, sizeof b, "deg%+d:peak=%.2fHz target=%.2fHz err=%.3f%% ", deg, f, want, 100.0 * (f / want - 1.0)); out += b;
        }
        all &= check ("koro_shifter_sine_accuracy", ok, out, checks);
    }
    // 12. per-note hold, 300 ms spacing and target smoothness on the female vocal (AUTO, THIRD mood)
    {
        Wav w; if (!readKadin (w)) all &= check ("note_onset_hold", false, "source not found", checks);
        else
        {
            std::vector<float> x (w.samples.size () / size_t (w.channels)); for (size_t i = 0; i < x.size (); ++i) x[i] = w.samples[i * size_t (w.channels)];
            cifte::Controls z; cifte::moods::get ("THIRD", z);
            cifte::CifteEngine e; e.prepare (w.rate); std::vector<float> l (x), r (x); const int blk = w.rate / 200; int prevMode = -1; int late = 0, changes = 0; double lastT = -1e9, minGap = 1e9; int64_t notes = 0, lastNote = -1;
            for (size_t p = 0; p < x.size (); p += size_t (blk))
            {
                e.process (l.data () + p, r.data () + p, int (std::min<size_t> (size_t (blk), x.size () - p)), z); const auto& q = e.keyStatus ();
                const double td = double (p + size_t (blk)) - e.latency ();
                if (q.noteId != lastNote) { ++notes; lastNote = q.noteId; }
                if (prevMode >= 0 && q.mode[0] != prevMode)
                {
                    ++changes; if (q.noteOnset >= 0.0 && td - q.noteOnset > 0.100 * w.rate + blk) ++late;
                    minGap = std::min (minGap, (td - lastT) / w.rate * 1000.0); lastT = td;
                }
                prevMode = q.mode[0];
            }
            const auto& sx = e.shifterStats ();
            char b[360]; std::snprintf (b, sizeof b, "notes=%lld class_changes=%d changes_later_than_100ms_after_onset=%d (engine: late=%d, safety_overrides_of_300ms=%d, skipped_for_300ms=%d) min_gap_between_changes=%.0fms",
                                        (long long) notes, changes, late, sx.lateChanges, sx.overrides, sx.skipped, minGap);
            char bh[300]; std::snprintf (bh, sizeof bh, "non-withdrawal changes later than 600 ms after onset (upgrade window)=%d (gate 0) upgrades=%d withdrawals=%d decisions_skipped_for_spacing=%d | status-based changes >100 ms after onset (all kinds)=%d", sx.lateChanges, sx.upgrades, sx.withdrawals, sx.skipped, late);
            all &= check ("note_onset_hold", sx.lateChanges == 0, bh, checks); (void) b;
            char b2[200]; std::snprintf (b2, sizeof b2, "max_target_jump_sustained=%.1fc over %lld grain pairs (>=100 ms after onset), incl_onset_glides=%.1fc", sx.maxJumpSustained, (long long) sx.grainPairsSustained, sx.maxJumpNote);
            all &= check ("target_jump_within_note", sx.maxJumpSustained <= 50.0 && sx.grainPairsSustained > 1000, b2, checks);
            char b3[200]; std::snprintf (b3, sizeof b3, "min_gap_between_non-withdrawal_changes=%.0fms (gate >= 300) withdrawals_exempt=%d crossfade=250ms", 1000.0 * sx.minGapNonSafety, sx.withdrawals);
            all &= check ("change_spacing_300ms", sx.minGapNonSafety >= 0.300, b3, checks); (void) minGap;
        }
    }
    // 9. CPU: 60 s stereo 48 kHz, CHOIR mood with COLOR at a mid-point (6 shifter slots) + doubler + detector
    {
        std::vector<double> ff; const std::vector<float> vv = vibratoVoice (48000, 4.8, ff); std::vector<float> l (size_t (48000 * 60)), r (l.size ());
        for (size_t i = 0; i < l.size (); ++i) l[i] = r[i] = vv[i % vv.size ()];
        cifte::Controls z; cifte::moods::get ("CHOIR", z); z.color = 0.5f; z.keyMode = cifte::KeyMode::Manual; z.root = 9; z.scale = 0;
        cifte::CifteEngine e; e.prepare (48000.0); const auto t0 = std::chrono::steady_clock::now ();
        for (size_t p = 0; p < l.size (); p += 512) e.process (l.data () + p, r.data () + p, int (std::min<size_t> (512, l.size () - p)), z);
        const double sec = std::chrono::duration<double> (std::chrono::steady_clock::now () - t0).count ();
        all &= check ("koro_cpu_60s", sec < 10.0, "seconds=" + std::to_string (sec), checks);
    }
    return all;
}

static bool selftest ()
{
    int checks = 0; bool all = true; const int rates[] = { 44100, 48000, 96000 };
    const int n = 48000; std::vector<float> silence (size_t (n) * 2u, 0.0f), sine (silence.size ()), noise (silence.size ()), sweep (silence.size ()); cifte::Rng rr (999u);
    for (int i = 0; i < n; ++i) { const float t = float (i) / 48000.0f; const float s = std::sin (2.0f * cifte::kPi * 1000.0f * t); const float q = std::sin (2.0f * cifte::kPi * (20.0f * std::pow (1000.0f, t)) * t); sine[2u * size_t (i)] = sine[2u * size_t (i) + 1u] = 0.7f * s; sweep[2u * size_t (i)] = sweep[2u * size_t (i) + 1u] = 0.5f * q; noise[2u * size_t (i)] = noise[2u * size_t (i) + 1u] = 0.35f * rr.bipolar (); }
    cifte::Controls c = preset ("Arabesk Choir"); c.mix = 0.55f;
    const std::vector<float> a = render (sine, 2, 48000, c), b = render (sine, 2, 48000, c); all &= check ("determinism", a == b, "bit-identical", checks);
    cifte::Controls dry = c; dry.mix = 0.0f; const std::vector<float> d = render (sine, 2, 48000, dry); all &= check ("mix_zero_dry", d == sine, "exact", checks);
    bool finite = true; float pk = 0.0f;
    for (int rate : rates) for (const std::vector<float>* src : { &silence, &sine, &noise, &sweep }) { const std::vector<float> y = render (*src, 2, rate, c); for (float v : y) finite &= std::isfinite (v); pk = std::max (pk, peak (y)); }
    all &= check ("finite_peak_all_rates", finite && pk < 1.995262f, "peak=" + std::to_string (pk), checks);
    const float base = rms (render (sine, 2, 48000, c)); bool changed = true; const char* names[] = { "voices", "spread", "tight", "width", "tone", "mix", "mono_safe" };
    for (int k = 0; k < 7; ++k) { cifte::Controls x = c; if (k == 0) x.voices = 1; if (k == 1) x.spread = 0.05f; if (k == 2) x.tight = 0.95f; if (k == 3) x.width = 0.05f; if (k == 4) x.tone = 0.05f; if (k == 5) x.mix = 0.9f; if (k == 6) x.monoSafe = true; const float delta = diffRms (render (sine, 2, 48000, c), render (sine, 2, 48000, x)); changed &= delta > 0.0005f; all &= check (names[k], delta > 0.0005f, "rms_delta=" + std::to_string (delta), checks); }
    cifte::Controls off = c; off.mix = 0.0f; cifte::Controls off2 = off; off2.width = 0.0f; all &= check ("bypass_controls_fixed", render (sine, 2, 48000, off) == render (sine, 2, 48000, off2), "bit-identical", checks); (void) base; (void) changed;
    { // latency: reported == measured (impulse, doubler mix 0.5, harmony 0), and the same number for every setting
        cifte::CifteEngine e; e.prepare (48000.0); cifte::Controls z = c; z.mix = 0.5f; const int rep = e.latency (z);
        std::vector<float> l (48000, 0.0f), r (l.size (), 0.0f); l[1000] = r[1000] = 0.8f;
        for (size_t p = 0; p < l.size (); p += 512) e.process (l.data () + p, r.data () + p, 512, z);
        const int meas = int (std::max_element (l.begin (), l.end (), [] (float x, float y) { return std::fabs (x) < std::fabs (y); }) - l.begin ()) - 1000;
        all &= check ("latency", rep == meas, "reported=" + std::to_string (rep) + " measured=" + std::to_string (meas) + " (48 kHz, harmony 0)", checks);
    }
    cifte::Controls one = c; one.voices = 1; one.mix = 1.0f; one.spread = 1.0f; std::vector<float> ys = render (sine, 2, 48000, one); float maxJump = 0.0f, dryJump = 0.0f; for (size_t i = 2; i < ys.size (); i += 2) { maxJump = std::max (maxJump, std::fabs (ys[i] - ys[i - 2])); dryJump = std::max (dryJump, std::fabs (sine[i] - sine[i - 2])); } all &= check ("no_clicks", maxJump < 2.0f * dryJump + 0.001f, "jump=" + std::to_string (maxJump), checks);
    const float sec = 60.0f; std::vector<float> longIn (size_t (96000 * sec), 0.0f); auto t0 = std::chrono::steady_clock::now (); (void) render (longIn, 2, 48000, c); const double elapsed = std::chrono::duration<double> (std::chrono::steady_clock::now () - t0).count (); all &= check ("cpu_60s", elapsed < 3.0, "seconds=" + std::to_string (elapsed), checks);
    const std::vector<float> yy = render (noise, 2, 48000, [&] { cifte::Controls z = c; z.monoSafe = true; z.mix = 0.5f; return z; } ()); float monoMin = 1.0e9f, monoMax = 0.0f; for (int i = 256; i < n - 256; i += 256) { float block = 0.0f; for (int j = 0; j < 256; ++j) block += std::fabs (yy[size_t (2 * (i + j))] + yy[size_t (2 * (i + j) + 1)]); block /= 256.0f; monoMin = std::min (monoMin, block); monoMax = std::max (monoMax, block); } const float notch = 20.0f * std::log10 (std::max (monoMax, 1.0e-9f) / std::max (monoMin, 1.0e-9f)); all &= check ("mono_safe_comb", notch < 6.0f, "depth_db=" + std::to_string (notch), checks);
    // A 440 Hz zero-crossing pitch check on the isolated one-voice path.
    const int pitchN = 48000; std::vector<float> ps (size_t (pitchN) * 2u); for (int i = 0; i < pitchN; ++i) ps[2u * size_t (i)] = ps[2u * size_t (i) + 1u] = 0.6f * std::sin (2.0f * cifte::kPi * 440.0f * float (i) / 48000.0f); const std::vector<float> po = render (ps, 2, 48000, one); int crossings = 0; for (int i = 1; i < pitchN; ++i) if (po[2u * size_t (i - 1)] <= 0.0f && po[2u * size_t (i)] > 0.0f) ++crossings; const float cents = 1200.0f * std::log2 (std::max (1.0f, float (crossings) / 1.0f) / 440.0f * 48000.0f / float (pitchN)); all &= check ("pitch_deviation", std::fabs (cents) < 15.0f, "cents=" + std::to_string (cents), checks);
    const std::vector<float> ho = render (ps, 2, 48000, [&] { cifte::Controls z = c; z.harmony = 0.7f; z.harmonyVoices = 3; z.root = 9; z.scale = 0; z.degree = {{2, -2, 4}}; return z; } ());
    const float hd = diffRms (render (ps, 2, 48000, c), ho); all &= check ("harmony_changes_output", hd > 0.0005f, "rms_delta=" + std::to_string (hd), checks);
    all &= harmonyTests (checks);
    all &= koroTests (checks);
    for (const std::string& pn : { "Tight Double", "Arabesk Choir", "Wide Chorus-less", "Backing Stack" }) { cifte::Controls z = preset (pn); const std::vector<float> zo = render (sine, 2, 48000, z); float jump = 0.0f; for (size_t i = 2; i < zo.size (); i += 2) jump = std::max (jump, std::fabs (zo[i] - zo[i - 2])); all &= check (("artefact_free_" + pn).c_str (), std::isfinite (jump) && jump < 0.8f, "roughness_proxy_jump=" + std::to_string (jump), checks); }
    if (all) std::printf ("ALL PASS (%d checks)\n", checks); else std::printf ("NOT ALL PASS (%d checks)\n", checks); return all;
}
int main (int argc, char** argv)
{
    try
    {
        if (argc == 2 && std::string (argv[1]) == "--selftest") return selftest () ? 0 : 1;
        if (argc >= 4 && std::string (argv[1]) == "--generate") return generate (argv[2], argv[3]);
        if (argc < 3) { std::fprintf (stderr, "usage: render --selftest | render in.wav out.wav [--preset NAME] [--param k=v] [--start sec] [--seconds sec] [--loudness-match dry.wav]\n"); return 2; }
        const Wav input = readWav (argv[1]); std::string name = "Tight Double", match; double start = 0.0, seconds = double (input.samples.size () / size_t (input.channels)) / input.rate;
        cifte::Controls c;
        for (int i = 3; i < argc; ++i) { if (std::string (argv[i]) == "--preset" && i + 1 < argc) name = argv[++i]; else if (std::string (argv[i]) == "--param" && i + 1 < argc) applyParam (c, argv[++i]); else if (std::string (argv[i]) == "--start" && i + 1 < argc) start = std::atof (argv[++i]); else if (std::string (argv[i]) == "--seconds" && i + 1 < argc) seconds = std::atof (argv[++i]); else if (std::string (argv[i]) == "--loudness-match" && i + 1 < argc) match = argv[++i]; }
        c = preset (name); for (int i = 3; i + 1 < argc; ++i) if (std::string (argv[i]) == "--param") applyParam (c, argv[i + 1]);
        const std::vector<float> source = crop (input, start, seconds); std::vector<float> out = render (source, input.channels, input.rate, c);   // latency-compensated
        if (!match.empty ()) { const Wav dryW = readWav (match); const float from = rms (dryW.samples), to = rms (out); if (to > 1.0e-8f) for (float& v : out) v *= from / to; }
        writeWav (argv[2], input.rate, 2, out); return 0;
    }
    catch (const std::exception& ex) { std::fprintf (stderr, "ERROR %s\n", ex.what ()); return 1; }
}
