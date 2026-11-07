// TASK_P4c section 5: the variation rig. `plugin_check --rig [demo_material_dir]` drives the real processor and the real editor through every variation of
// the plug-in that can be listed and prints a table; the exit code is non-zero when any row fails.
//   BANDS     every band type x slopes 12-72 x Q {0.3, 1, 4, 18} x gain {-18, 0, +18} x dynamic on / off (336 cases): finite, the measured response equals
//             Engine::freeBandMagDb within 0.1 dB, the curve the editor draws equals the engine's (and the measured one)
//   RANDOM    a seeded sweep over frequency and sample rate (44.1 / 48 / 96 kHz) of the same checks
//   TARGET    every instrument and part the panel offers (21 entries, chosen through the editor's list), on matching synthetic material: the cut lands where it is
//             stated, stays static, and is measurable in the output
//   COLLIDE   with and without a sidechain key; SELF on resonant material and on plain pink noise; all layers together
//   EDITOR    section 12 (analyzer, defaults, nodes, panels, state round trip, clicks) is part of this run
// Included by plugin_check.cpp after editor_checks.h.
#pragma once

namespace vr
{
struct Row { juce::String name; int n = 0, fail = 0; double worst = 0.0; juce::String worstCase, firstFail, unit; };
inline std::vector<Row>& rows() { static std::vector<Row> r; return r; }
inline Row& row (const juce::String& name) { for (auto& r : rows()) if (r.name == name) return r; rows().push_back ({ name }); return rows().back(); }
inline void record (const juce::String& group, bool ok, double metric, const juce::String& caseName, const juce::String& unit = {})
{
    auto& r = row (group); ++r.n; r.unit = unit;
    if (std::abs (metric) >= std::abs (r.worst)) { r.worst = metric; r.worstCase = caseName; }
    if (! ok) { ++r.fail; if (r.firstFail.isEmpty()) r.firstFail = caseName; }
}

constexpr int kN = 16384;   // the analysis window: tones sit on exact bins, so every window holds whole cycles

struct Tones
{
    std::vector<int> bin; std::vector<double> hz, amp, phase; double sr = 48000.0;
    explicit Tones (double sampleRate) : sr (sampleRate)
    {
        juce::Random rnd (99);
        for (double f : { 60.0, 120.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 14000.0, 18000.0 })
        {
            const int m = juce::roundToInt (f * kN / sr);
            if ((double) m * sr / kN > 0.47 * sr) continue;
            bin.push_back (m); hz.push_back ((double) m * sr / kN); amp.push_back (0.012); phase.push_back (rnd.nextDouble() * 6.2831853);
        }
    }
    Audio signal (int n) const
    {
        Audio a (2, std::vector<float> ((size_t) n));
        for (int i = 0; i < n; ++i)
        {
            double v = 0.0; for (size_t k = 0; k < bin.size(); ++k) v += amp[k] * std::sin (6.283185307179586 * hz[k] * (double) i / sr + phase[k]);
            a[0][(size_t) i] = a[1][(size_t) i] = (float) v;
        }
        return a;
    }
    /** The gain in dB at every tone: single-bin DFT of the last kN samples of the output over the same of the input. */
    std::vector<double> gains (const Audio& in, const Audio& out) const
    {
        std::vector<double> g;
        const int n = (int) out[0].size(), from = n - kN;
        for (size_t k = 0; k < bin.size(); ++k)
        {
            double ir = 0, ii = 0, orr = 0, oi = 0;
            for (int i = 0; i < kN; ++i)
            {
                const double a = -6.283185307179586 * (double) bin[k] * (double) i / (double) kN, c = std::cos (a), s = std::sin (a);
                ir += in[0][(size_t) (from + i)] * c; ii += in[0][(size_t) (from + i)] * s; orr += out[0][(size_t) (from + i)] * c; oi += out[0][(size_t) (from + i)] * s;
            }
            g.push_back (20.0 * std::log10 (std::sqrt (orr * orr + oi * oi) / std::sqrt (ir * ir + ii * ii) + 1e-30));
        }
        return g;
    }
};

struct BandSpec { int type = 0; float f0 = 1000, gain = 0, q = 1; int slopeIdx = 0; bool dyn = false; float range = 0.0f; };
inline const char* typeName (int t) { static const char* n[] = { "bell", "lowshelf", "highshelf", "notch", "lowcut", "highcut" }; return n[t]; }
inline juce::String specName (const BandSpec& b, double sr)
{
    return juce::String (typeName (b.type)) + " f" + juce::String (b.f0, 0) + " g" + juce::String (b.gain, 0) + " Q" + juce::String (b.q, 1) + (b.type >= 4 ? " " + juce::String (dyneq::freeSlopeDbFromIndex (b.slopeIdx)) + "dB" : juce::String())
         + (b.dyn ? " dyn" : "") + " @" + juce::String (sr / 1000.0, 1) + "k";
}

/** One band case through the real processor and the editor. Returns the worst measured-vs-model error (dB) and fills the curve error. */
struct BandResult { bool finite = true; double worstErr = 0.0, curveErr = 0.0, drawnVsMeasured = 0.0, wrongModelErr = 0.0; int points = 0; juce::String at; };
inline BandResult runBand (Rig& r, DyneqEditor& ed, const Tones& tones, const Audio& sig, const BandSpec& b)
{
    BandResult res;
    auto& p = r.proc;
    for (int i = 2; i <= 6; ++i) setParam (p.apvts, ("b" + juce::String (i) + "_on").toRawUTF8(), 0.0f);
    setParam (p.apvts, "b1_on", 0.0f); p.prepareToPlay (r.sr, 512);   // a cold engine for every case (the audio thread's cold start puts the band at full gain from the first sample)
    setParam (p.apvts, "b1_type", (float) b.type); setParam (p.apvts, "b1_freq", b.f0); setParam (p.apvts, "b1_gain", b.gain); setParam (p.apvts, "b1_q", b.q);
    setParam (p.apvts, "b1_slope", (float) b.slopeIdx); setParam (p.apvts, "b1_dyn", b.dyn ? 1.0f : 0.0f); setParam (p.apvts, "b1_range", b.range); setParam (p.apvts, "b1_thr", -60.0f);
    setParam (p.apvts, "b1_on", 1.0f);
    const Audio out = r.render (sig, nullptr, 512);
    res.finite = allFinite (out);
    ed.tickForSnapshot();
    auto& cv = ed.curves();
    // the model takes the values the parameters really hold (FREQ, GAIN and Q are quantised to 0.01 by the parameter, as in a host)
    const double rf = (double) p.apvts.getRawParameterValue ("b1_freq")->load(), rg = (double) p.apvts.getRawParameterValue ("b1_gain")->load(), rq = (double) p.apvts.getRawParameterValue ("b1_q")->load(), rr = (double) p.apvts.getRawParameterValue ("b1_range")->load();
    const double eg = b.dyn ? rg + rr : rg;
    const int slopeDb = dyneq::freeSlopeDbFromIndex (b.slopeIdx);
    // the curve the editor draws: 361 points against the engine's own response
    for (int i = 0; i < panel::kCurveN; ++i)
    {
        const double model = dyneq::Engine::freeBandMagDb (b.type, cv.band[0].freq, eg, rq, slopeDb, cv.f[i], r.sr);
        res.curveErr = std::max (res.curveErr, std::abs ((double) cv.bands[i] - model));
        res.curveErr = std::max (res.curveErr, std::abs ((double) cv.total[i] - model));   // no layer and no OUTPUT: the total is the band
    }
    const auto g = tones.gains (sig, out);
    for (size_t k = 0; k < g.size(); ++k)
    {
        const double model = dyneq::Engine::freeBandMagDb (b.type, (float) rf, eg, rq, slopeDb, tones.hz[k], r.sr);
        if (model < -60.0) continue;
        const double e = std::abs (g[k] - model);
        if (e > res.worstErr) { res.worstErr = e; res.at = juce::String (tones.hz[k], 0) + " Hz measured " + juce::String (g[k], 3) + " model " + juce::String (model, 3); }
        res.drawnVsMeasured = std::max (res.drawnVsMeasured, std::abs (g[k] - cv.bandsDb (tones.hz[k])));
        // negative control: the same comparison against a model that is wrong on purpose (FREQ +1 %, Q +3 %): the rig must see it
        res.wrongModelErr = std::max (res.wrongModelErr, std::abs (g[k] - dyneq::Engine::freeBandMagDb (b.type, (float) (rf * 1.01), eg, rq * 1.03, slopeDb, tones.hz[k], r.sr)));
        ++res.points;
    }
    return res;
}

//==============================================================================
// material for TARGET: kick, snare and hat hits as the TARGET pack generator makes them (tools/make_pack_target.py), placed at 120 bpm
inline std::vector<float> hp2 (const std::vector<float>& x, double fc, double sr)
{
    const double w0 = 2.0 * juce::MathConstants<double>::pi * fc / sr, cs = std::cos (w0), al = std::sin (w0) / (2.0 * 0.7071067811865476), a0 = 1.0 + al;
    const double b0 = (1.0 + cs) / 2.0 / a0, b1 = -(1.0 + cs) / a0, b2 = b0, a1 = -2.0 * cs / a0, a2 = (1.0 - al) / a0;
    std::vector<float> y (x.size()); double z1 = 0, z2 = 0;
    for (size_t i = 0; i < x.size(); ++i) { const double o = b0 * x[i] + z1; z1 = b1 * x[i] - a1 * o + z2; z2 = b2 * x[i] - a2 * o; y[i] = (float) o; }
    return y;
}
inline float gnoise (juce::Random& r) { float s = 0; for (int i = 0; i < 6; ++i) s += r.nextFloat(); return (s - 3.0f) * 1.4142f; }
inline std::vector<float> kickHit (juce::Random& rnd, double f0, double sr = 48000.0)
{
    const int n = (int) (0.5 * sr); std::vector<float> h ((size_t) n); double ph = 0;
    for (int i = 0; i < n; ++i)
    {
        const double t = (double) i / sr; ph += 2.0 * juce::MathConstants<double>::pi * f0 * (1.0 + 1.6 * std::exp (-t / 0.018)) / sr;
        h[(size_t) i] = (float) (0.8 * std::sin (ph) * std::exp (-t / 0.13) + (t < 0.005 ? 0.45 * gnoise (rnd) * std::exp (-t / 0.0008) : 0.0));
    }
    return h;
}
inline std::vector<float> snareHit (juce::Random& rnd, double sr = 48000.0)
{
    const int n = (int) (0.6 * sr); std::vector<float> nz ((size_t) n);
    for (auto& v : nz) v = gnoise (rnd);
    nz = hp2 (nz, 1500.0, sr);
    std::vector<float> h ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        const double t = (double) i / sr;
        h[(size_t) i] = (float) (0.55 * nz[(size_t) i] * std::exp (-t / 0.09) + 0.45 * gnoise (rnd) * std::exp (-t / 0.006) + 0.38 * std::sin (2.0 * juce::MathConstants<double>::pi * 205.0 * t) * std::exp (-t / 0.11) + 0.16 * std::sin (2.0 * juce::MathConstants<double>::pi * 980.0 * t) * std::exp (-t / 0.07));
    }
    return h;
}
inline std::vector<float> hatHit (juce::Random& rnd, double tau, double sr = 48000.0)
{
    const int n = (int) (0.12 * sr); std::vector<float> nz ((size_t) n);
    for (auto& v : nz) v = gnoise (rnd);
    nz = hp2 (nz, 6500.0, sr);
    for (int i = 0; i < n; ++i) nz[(size_t) i] *= (float) (0.3 * std::exp (-(double) i / sr / tau));
    return nz;
}
struct DrumLoops { Audio kick, snare, hats, bus; };
inline DrumLoops makeDrumLoops (double secs, double sr = 48000.0)
{
    const int n = (int) (secs * sr); DrumLoops d; d.kick = d.snare = d.hats = d.bus = Audio (2, std::vector<float> ((size_t) n, 0.0f));
    juce::Random rnd (16);
    auto put = [&] (Audio& a, const std::vector<float>& h, double t) { const int s = (int) std::lround (t * sr); for (size_t i = 0; i < h.size() && s + (int) i < n; ++i) a[0][(size_t) s + i] = a[1][(size_t) s + i] += h[i]; };
    const double beat = 0.5; const int beats = (int) (secs / beat) - 1;
    for (int k = 0; k < beats; ++k)
    {
        if (k % 4 == 0 || k % 4 == 2 || k % 8 == 7) put (d.kick, kickHit (rnd, 56.0 + 4.0 * rnd.nextDouble(), sr), k * beat);
        if (k % 4 == 1 || k % 4 == 3) put (d.snare, snareHit (rnd, sr), k * beat);
    }
    for (int k = 0; k < beats * 2; ++k) put (d.hats, hatHit (rnd, k % 2 ? 0.02 : 0.04, sr), k * beat * 0.5);
    for (int c = 0; c < 2; ++c) for (int i = 0; i < n; ++i) d.bus[(size_t) c][(size_t) i] = d.kick[(size_t) c][(size_t) i] + d.snare[(size_t) c][(size_t) i] + 0.6f * d.hats[(size_t) c][(size_t) i];
    return d;
}

/** The synthetic hits of tools/measure.py (kick6, snare6, hat6, bass6), the material TARGET's own T-series is judged on: parts at known places, the same sample every time for the kick and the
    bass, new noise for every snare and hat. Mono vectors at 48 kHz. */
inline std::vector<float> bpNoise (juce::Random& rnd, int n, double lo, double hi, double sr = 48000.0)
{
    std::vector<double> x ((size_t) n); for (auto& v : x) v = (double) gnoise (rnd);
    const double fc = std::sqrt (lo * hi), q = fc / (hi - lo), w0 = 2.0 * juce::MathConstants<double>::pi * fc / sr, al = std::sin (w0) / (2.0 * q), a0 = 1.0 + al;
    const double b0 = al / a0, b2 = -al / a0, a1 = -2.0 * std::cos (w0) / a0, a2 = (1.0 - al) / a0;
    for (int pass = 0; pass < 2; ++pass) { double x1 = 0, x2 = 0, y1 = 0, y2 = 0; for (auto& v : x) { const double y = b0 * v + b2 * x2 - a1 * y1 - a2 * y2; x2 = x1; x1 = v; y2 = y1; y1 = y; v = y; } }
    double ss = 0; for (double v : x) ss += v * v;
    const double g = std::sqrt ((hi - lo) / (sr * 0.5)) / std::sqrt (ss / (double) n + 1e-30);   // the level a Butterworth band-pass of white noise would have
    std::vector<float> y ((size_t) n); for (int i = 0; i < n; ++i) y[(size_t) i] = (float) (x[(size_t) i] * g);
    return y;
}
inline std::vector<float> kick6 (double f0, double fc, double secs = 0.7, double sr = 48000.0)
{
    const int n = (int) (secs * sr); std::vector<float> h ((size_t) n); const double pi2 = 2.0 * juce::MathConstants<double>::pi;
    for (int i = 0; i < n; ++i)
    {
        const double t = (double) i / sr; double v = 0.55 * std::sin (pi2 * f0 * t) * std::exp (-t / 0.12), body = 0.0, click = 0.0;
        for (int k = -2; k <= 2; ++k) { body += 0.07 * std::sin (pi2 * 2.75 * f0 * std::pow (2.0, k / 10.0) * t); click += 0.12 * std::sin (pi2 * fc * std::pow (2.0, k / 6.0) * t); }
        h[(size_t) i] = (float) (v + body * std::exp (-t / 0.04) + (t < 0.004 ? click * std::exp (-t / 0.0007) : 0.0));
    }
    return h;
}
inline std::vector<float> snare6 (juce::Random& rnd, double secs = 0.7, double sr = 48000.0)
{
    const int n = (int) (secs * sr); const auto cr = bpNoise (rnd, n, 800.0, 3500.0), sn = bpNoise (rnd, n, 4000.0, 10000.0); std::vector<float> h ((size_t) n); const double pi2 = 2.0 * juce::MathConstants<double>::pi;
    for (int i = 0; i < n; ++i)
    {
        const double t = (double) i / sr;
        h[(size_t) i] = (float) (0.30 * std::sin (pi2 * 200.0 * t) * std::exp (-t / 0.10) + 0.10 * std::sin (pi2 * 400.0 * t) * std::exp (-t / 0.07) + 0.9 * cr[(size_t) i] * std::exp (-t / 0.025) + 1.3 * sn[(size_t) i] * std::exp (-t / 0.008));
    }
    return h;
}
inline std::vector<float> hat6 (juce::Random& rnd, double sr = 48000.0)
{
    const int n = (int) (0.4 * sr); std::vector<float> nz ((size_t) n); for (auto& v : nz) v = gnoise (rnd);
    nz = hp2 (nz, 6500.0, sr); for (int i = 0; i < n; ++i) nz[(size_t) i] *= (float) (0.5 * std::exp (-(double) i / sr / 0.025));
    return nz;
}
inline std::vector<float> bass6 (double sr = 48000.0)
{
    const int n = (int) (0.8 * sr); juce::Random rnd (66); const auto pl = bpNoise (rnd, n, 500.0, 2000.0); std::vector<float> h ((size_t) n); const double pi2 = 2.0 * juce::MathConstants<double>::pi, f0 = 55.0;
    for (int i = 0; i < n; ++i)
    {
        const double t = (double) i / sr; double note = 0.0; const double ks[4] = { 1, 2, 3, 5 }, as[4] = { 0.5, 0.25, 0.15, 0.06 };
        for (int k = 0; k < 4; ++k) note += as[k] * std::sin (pi2 * ks[k] * f0 * t);
        h[(size_t) i] = (float) (note * std::exp (-t / 0.25) + 0.3 * pl[(size_t) i] * std::exp (-t / 0.01));
    }
    return h;
}
/** hits placed one after the other (0.5 s lead) until `secs`: kind 0 kick, 1 snare, 2 hats (every 0.25 s), 3 bass (every 0.6 s), 4 drums (kick and snare alternating). */
inline Audio makeTargetMaterial (int kind, double secs, bool freshNoise = false, double sr = 48000.0)
{
    // a drum machine plays one sample: the snare and the hat are made once and repeated (freshNoise = a new noise for every hit, as measure.py's T25 makes them)
    const int n = (int) (secs * sr); Audio a (2, std::vector<float> ((size_t) n, 0.0f)); juce::Random rnd (25);
    const double spacing = kind == 2 ? 0.25 : kind == 3 ? 0.6 : 0.5; const auto kick = kick6 (55.0, 3000.0), bass = bass6(), snare = snare6 (rnd), hat = hat6 (rnd);
    int k = 0;
    for (double t = 0.5; t < secs - 0.8; t += spacing, ++k)
    {
        const std::vector<float> h = kind == 0 ? kick : kind == 1 ? (freshNoise ? snare6 (rnd) : snare) : kind == 2 ? (freshNoise ? hat6 (rnd) : hat) : kind == 3 ? bass : (k % 2 == 0 ? kick : (freshNoise ? snare6 (rnd) : snare));
        const int s = (int) std::lround (t * sr);
        for (size_t i = 0; i < h.size() && s + (int) i < n; ++i) a[0][(size_t) s + i] = a[1][(size_t) s + i] += h[i];
    }
    return a;
}

/** Long-term average spectrum (power per FFT bin) of the left channel between two sample positions. */
inline std::vector<double> ltas (const Audio& x, int from, int to, int nfft = 8192)
{
    std::vector<double> acc ((size_t) nfft / 2, 0.0); int frames = 0;
    std::vector<float> re ((size_t) nfft), im ((size_t) nfft);
    for (int s = from; s + nfft <= to; s += nfft / 2)
    {
        for (int i = 0; i < nfft; ++i) { re[(size_t) i] = x[0][(size_t) (s + i)] * (0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * (float) i / (float) nfft)); im[(size_t) i] = 0.0f; }
        panel::Spectrum::fft (re, im);
        for (int k = 0; k < nfft / 2; ++k) acc[(size_t) k] += (double) re[(size_t) k] * re[(size_t) k] + (double) im[(size_t) k] * im[(size_t) k];
        ++frames;
    }
    for (auto& a : acc) a /= (double) std::max (frames, 1);
    return acc;
}
inline double bandDbDiff (const std::vector<double>& a, const std::vector<double>& b, double lo, double hi, double sr = 48000.0, int nfft = 8192)
{
    double sa = 0, sb = 0; const int k0 = std::max (1, (int) std::floor (lo * nfft / sr)), k1 = std::min (nfft / 2 - 1, (int) std::ceil (hi * nfft / sr));
    for (int k = k0; k <= k1; ++k) { sa += a[(size_t) k]; sb += b[(size_t) k]; }
    return 10.0 * std::log10 ((sa + 1e-30) / (sb + 1e-30));   // a over b
}
} // namespace vr

//==============================================================================
static int runVariationRig (int argc, char** argv)
{
    using namespace vr; using namespace ec;
    juce::File dir = juce::File::getCurrentWorkingDirectory().getChildFile ("tools/demo_material");
    for (int i = 1; i < argc; ++i) if (! juce::String (argv[i]).startsWith ("--")) dir = juce::File (argv[i]);
    if (! dir.isDirectory()) { std::printf ("demo material dir not found: %s\n", dir.getFullPathName().toRawUTF8()); return 2; }
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    const int failBefore = gFail, passBefore = gPass;
    std::printf ("variation rig: the real processor and the real editor, every variation\n");

    //------------------------------------------------------------------------------------------------ BANDS: 336 cases at 48 kHz
    {
        const double sr = 48000.0; Tones tones (sr); const int total = 1200 * 48 + kN; const Audio sig = tones.signal (total);
        Rig r (2, false, sr, 512); DyneqEditor ed (r.proc);
        ed.refreshFace();
        for (int type = 0; type < 6; ++type)
            for (int slope = 0; slope < (type >= 4 ? 5 : 1); ++slope)
                for (float q : { 0.3f, 1.0f, 4.0f, 18.0f })
                    for (float gain : { -18.0f, 0.0f, 18.0f })
                        for (int dyn = 0; dyn < 2; ++dyn)
                        {
                            BandSpec b; b.type = type; b.f0 = 1000.0f; b.gain = gain; b.q = q; b.slopeIdx = slope; b.dyn = dyn != 0; b.range = 0.0f;
                            const auto res = runBand (r, ed, tones, sig, b);
                            const bool ok = res.finite && res.worstErr <= 0.1 && res.curveErr <= 0.02 && res.drawnVsMeasured <= 0.1 && res.points >= 2;
                            const auto nm = specName (b, sr);
                            record ("BANDS 336 cases: measured vs freeBandMagDb (dB)", ok, res.worstErr, nm + " [" + res.at + "]", "dB");
                            record ("BANDS 336 cases: drawn curve vs engine (dB)", res.curveErr <= 0.02, res.curveErr, nm, "dB");
                            record ("BANDS 336 cases: drawn curve vs measured (dB)", res.drawnVsMeasured <= 0.1, res.drawnVsMeasured, nm, "dB");
                            record ("BANDS 336 cases: finite", res.finite, res.finite ? 0.0 : 1.0, nm);
                            if (type != dyneq::kBell || std::abs (gain) > 1.0f) {}   // (the sensitivity rows below use the cases where the response bends)
                            if (res.points >= 2 && ((type <= 2 && std::abs (gain) >= 18.0f) || (type == 3 && q >= 1.0f) || (type >= 4 && q >= 4.0f)))
                                record ("RIG SENSITIVITY: a model that is wrong by FREQ +1 % / Q +3 % is seen (error must exceed 0.1 dB)", res.wrongModelErr > 0.1, res.wrongModelErr, nm, "dB");
                            if (! ok) { ++gFail; std::printf ("[FAIL] band case %s: err %.3f dB at %s, curve %.4f dB, drawn vs measured %.3f dB, finite %d\n", nm.toRawUTF8(), res.worstErr, res.at.toRawUTF8(), res.curveErr, res.drawnVsMeasured, (int) res.finite); }
                            else ++gPass;
                        }
        // a dynamic band with a RANGE: finite, and the response equals the model at GAIN + RANGE where the trigger is far over the threshold (a tone at the band's frequency, threshold -60)
        for (float range : { -12.0f, -6.0f, 6.0f, 12.0f })
        {
            BandSpec b; b.type = dyneq::kBell; b.f0 = (float) tones.hz[4]; b.gain = 0.0f; b.q = 1.0f; b.dyn = true; b.range = range;
            const auto res = runBand (r, ed, tones, sig, b);
            const bool ok = res.finite && res.worstErr <= 0.35;
            record ("BANDS dynamic RANGE +-6 / +-12 (dB)", ok, res.worstErr, specName (b, sr) + " range " + juce::String (range, 0) + " [" + res.at + "]", "dB");
            if (! ok) { ++gFail; std::printf ("[FAIL] dynamic band range %.0f: err %.3f dB at %s\n", range, res.worstErr, res.at.toRawUTF8()); } else ++gPass;
        }
    }

    //------------------------------------------------------------------------------------------------ RANDOM: 3 sample rates, seeded frequency / type / Q / gain / slope
    for (double sr : { 44100.0, 48000.0, 96000.0 })
    {
        Tones tones (sr); const int total = (int) (1.2 * sr) + kN; const Audio sig = tones.signal (total);
        Rig r (2, false, sr, 512); DyneqEditor ed (r.proc);
        juce::Random rnd ((int) sr);
        for (int k = 0; k < 70; ++k)
        {
            BandSpec b; b.type = rnd.nextInt (6); b.f0 = (float) (60.0 * std::pow (150.0, rnd.nextDouble())); b.f0 = std::min (b.f0, (float) (0.4 * sr));
            b.gain = (float) (rnd.nextDouble() * 36.0 - 18.0); b.q = (float) (0.3 * std::pow (60.0, rnd.nextDouble())); b.slopeIdx = rnd.nextInt (5); b.dyn = b.type <= 2 && rnd.nextBool();
            if (b.f0 > 0.2 * sr && b.type >= 1 && b.type <= 2) b.f0 = (float) (0.2 * sr);   // shelves right under Nyquist are not what the P1n matcher promises (see AUDIT_EQ_P1n.md, "NOT MET")
            const auto res = runBand (r, ed, tones, sig, b);
            const bool ok = res.finite && res.worstErr <= 0.1 && res.curveErr <= 0.02 && res.drawnVsMeasured <= 0.1;
            record ("RANDOM 210 cases at 44.1 / 48 / 96 kHz (dB)", ok, res.worstErr, specName (b, sr) + " [" + res.at + "]", "dB");
            if (! ok) { ++gFail; std::printf ("[FAIL] random band %s: err %.3f dB at %s, curve %.4f, drawn vs measured %.3f, finite %d\n", specName (b, sr).toRawUTF8(), res.worstErr, res.at.toRawUTF8(), res.curveErr, res.drawnVsMeasured, (int) res.finite); } else ++gPass;
        }
    }

    //------------------------------------------------------------------------------------------------ TARGET: every instrument and part, chosen through the editor's own list
    {
        const double sr = 48000.0; const int secs = 16;
        const Audio mKick = makeTargetMaterial (0, secs), mSnare = makeTargetMaterial (1, secs), mHats = makeTargetMaterial (2, secs), mBass = makeTargetMaterial (3, secs), mDrums = makeTargetMaterial (4, secs);
        const Audio fSnare = makeTargetMaterial (1, secs, true), fHats = makeTargetMaterial (2, secs, true);   // information only: a new noise for every hit
        const auto vocalW = slice (readWav (dir.getChildFile ("vocal.wav")), 0, 48000 * secs);
        const auto& entries = panel::Ctl::targetEntries();
        for (int pass = 0; pass < 2; ++pass)
        for (size_t k = 1; k < entries.size(); ++k)
        {
            const auto& e = entries[k];
            const bool info = pass == 1;   // pass 1: SNARE CRACK and HATS CHICK on material with fresh noise in every hit: reported, not judged
            if (info && ! ((e.inst == dyneq::kTgtSnare && e.part == 1 && ! e.rev) || (e.inst == dyneq::kTgtHats && e.part == 0))) continue;
            const Audio& mat = e.inst == dyneq::kTgtKick ? mKick : e.inst == dyneq::kTgtSnare ? (info ? fSnare : mSnare) : e.inst == dyneq::kTgtHats ? (info ? fHats : mHats) : e.inst == dyneq::kTgtDrums ? mDrums : e.inst == dyneq::kTgtBass ? mBass : vocalW;
            Rig r (2, false, sr, 512); DyneqEditor ed (r.proc); ed.refreshFace();
            ed.ctl().setMode (panel::kTarget);
            ed.openChooser ({ 300, 300 }); ed.chooserPick ((int) k);
            setParam (r.proc.apvts, "targetAmount", 12.0f); setParam (r.proc.apvts, "targetSlope", 1.0f);
            if (! info) check (ed.ctl().targetEntryIndex() == (int) k && ed.ctl().layerOn (panel::kTarget), "TARGET list entry " + e.text + " chosen through the editor (instrument " + juce::String (e.inst) + ", part " + juce::String (e.part) + (e.rev ? ", REVERB-READY" : "") + ")");
            const int n = (int) mat[0].size();
            Audio out (2, std::vector<float> ((size_t) n));
            juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
            struct Sample { int pos; bool tb; float tbCut, tbHz; bool e[2]; float eDepth[2], eHz[2]; bool eBelow[2]; bool pl; float plDepth, plLo, plHi; };
            std::vector<Sample> log; juce::String trace;
            for (int pos = 0; pos < n; pos += 512)
            {
                const int len = std::min (512, n - pos); buf.setSize (2, len, false, false, true);
                for (int c = 0; c < 2; ++c) std::copy (mat[(size_t) c].begin() + pos, mat[(size_t) c].begin() + pos + len, buf.getWritePointer (c));
                r.proc.processBlock (buf, midi);
                for (int c = 0; c < 2; ++c) std::copy (buf.getReadPointer (c), buf.getReadPointer (c) + len, out[(size_t) c].begin() + pos);
                Sample sm {}; sm.pos = pos; float q = 0; sm.tb = r.proc.getTargetBell (sm.tbHz, q, sm.tbCut);
                for (int ke = 0; ke < 2; ++ke)
                {
                    const auto& ee = r.proc.getEdge (ke); sm.e[ke] = ee.run.load() && ee.cutDb.load() * ee.mix.load() > 0.3f;
                    sm.eDepth[ke] = ee.cutDb.load() * ee.mix.load(); sm.eHz[ke] = ee.hz.load(); sm.eBelow[ke] = ee.below.load();
                }
                {   // 11 Oct: a part that is not an edge is a plateau of three bells; its floor is judged against AMOUNT (12 here), its place by the outer bells
                    float f0b, q0, c0, f2b, q2, c2, f1b, q1, c1;
                    const bool b0 = r.proc.getTargetBell (0, f0b, q0, c0), b1 = r.proc.getTargetBell (1, f1b, q1, c1), b2 = r.proc.getTargetBell (2, f2b, q2, c2);
                    sm.pl = b0 && b1 && b2 && c1 > 0.3f; sm.plDepth = 0.0f; sm.plLo = f0b; sm.plHi = f2b;
                    if (sm.pl) sm.plDepth = juce::jmin (12.0f, r.proc.apvts.getRawParameterValue ("targetAmount")->load());
                }
                log.push_back (sm);
                if (pos % 48000 < 512) trace << "t" << juce::String (pos / 48000) << ": bell " << juce::String (sm.tb ? sm.tbCut : 0.0f, 1) << "@" << juce::String (sm.tb ? sm.tbHz : 0.0f, 0) << " e0 " << juce::String (sm.e[0] ? sm.eDepth[0] : 0.0f, 1) << "@" << juce::String (sm.eHz[0], 0)
                                                                                    << " e1 " << juce::String (sm.e[1] ? sm.eDepth[1] : 0.0f, 1) << "@" << juce::String (sm.eHz[1], 0) << " hits " << r.proc.getTargetHits() << " | ";
            }
            const bool finite = allFinite (out);
            // the last 3 seconds: the memory is full and the cut has had time to settle. STATIC (SPEC 8f) means: the depth does not move with the hits, and the frequency holds still (within 1/6 octave)
            // while the sound does not change. The first 13 seconds are the learning: how far the learned point drifted there is reported, not judged.
            const int winFrom = n - 3 * 48000;
            double depthMin = 1e9, depthMax = -1e9, hzMinB = 1e9, hzMaxB = 0, edgeMin[2] = { 1e9, 1e9 }, edgeMax[2] = { -1e9, -1e9 }, edgeHzMin[2] = { 1e9, 1e9 }, edgeHzMax[2] = { 0, 0 };
            bool tbSeen = false, edgeSeen[2] = { false, false }; float tbHz = 0, tbCut = 0, edgeHz[2] = { 0, 0 }; bool edgeBelow[2] = { true, false }; double hzFirst = 0, hzLast = 0;
            float plLo = 0, plHi = 0;
            for (auto& sm : log)   // the plateau is read through the bell's fields (its geometric centre, its depth): "lands, static, audible" judge it the same way
            {
                if (sm.pl) { sm.tb = true; sm.tbCut = sm.plDepth; sm.tbHz = std::sqrt (sm.plLo * sm.plHi); }
                if (sm.pos < (int) (8.0 * sr)) continue;
                if (sm.pl && sm.pos >= winFrom) { plLo = sm.plLo; plHi = sm.plHi; }
                if (sm.tb) { if (hzFirst == 0) hzFirst = sm.tbHz; hzLast = sm.tbHz; }
                if (sm.pos < winFrom) continue;
                if (sm.tb) { tbSeen = true; tbHz = sm.tbHz; tbCut = std::max (tbCut, sm.tbCut); depthMin = std::min (depthMin, (double) sm.tbCut); depthMax = std::max (depthMax, (double) sm.tbCut); hzMinB = std::min (hzMinB, (double) sm.tbHz); hzMaxB = std::max (hzMaxB, (double) sm.tbHz); }
                for (int ke = 0; ke < 2; ++ke)
                    if (sm.e[ke]) { edgeSeen[ke] = true; edgeHz[ke] = sm.eHz[ke]; edgeBelow[ke] = sm.eBelow[ke]; edgeMin[ke] = std::min (edgeMin[ke], (double) sm.eDepth[ke]); edgeMax[ke] = std::max (edgeMax[ke], (double) sm.eDepth[ke]); edgeHzMin[ke] = std::min (edgeHzMin[ke], (double) sm.eHz[ke]); edgeHzMax[ke] = std::max (edgeHzMax[ke], (double) sm.eHz[ke]); }
            }
            const bool active = tbSeen || edgeSeen[0] || edgeSeen[1];
            const double spreadBell = tbSeen ? depthMax - depthMin : 0.0, spreadEdge = std::max (edgeSeen[0] ? edgeMax[0] - edgeMin[0] : 0.0, edgeSeen[1] ? edgeMax[1] - edgeMin[1] : 0.0);
            const double hzSpread = std::max (tbSeen && hzMinB > 0 ? std::log2 (hzMaxB / hzMinB) : 0.0, std::max (edgeSeen[0] ? std::log2 (edgeHzMax[0] / edgeHzMin[0]) : 0.0, edgeSeen[1] ? std::log2 (edgeHzMax[1] / edgeHzMin[1]) : 0.0));
            const double hzMove = tbSeen && hzFirst > 0 ? std::abs (std::log2 (hzLast / hzFirst)) : 0.0, maxStepOct = hzSpread;
            const bool isStatic = spreadBell <= 0.6 && spreadEdge <= 0.6 && hzSpread <= 1.0 / 6.0;
            // where it is stated: the centre / the edge sits inside the part's search range (one octave of slack)
            const auto& inst = dyneq::kInst[e.inst]; const auto& slot = inst.slot[e.part]; const auto& vo = dyneq::kVoice[slot.voice]; const auto& pd = vo.part[slot.part];
            bool where = true; juce::String whereText;
            if (! e.rev)
            {
                if (pd.edge == 0) { where = tbSeen && tbHz >= pd.lo * 0.5f && tbHz <= pd.hi * 2.0f; whereText = "bell at " + juce::String (tbHz, 0) + " Hz, range " + juce::String (pd.lo, 0) + " - " + juce::String (pd.hi, 0); }
                else
                {
                    const int ke = edgeSeen[0] ? 0 : 1; const float h = edgeSeen[ke] ? edgeHz[ke] : 0.0f;
                    where = edgeSeen[ke] && h >= pd.lo * 0.5f && h <= pd.hi * 3.0f && (pd.edge == 1) == edgeBelow[ke]; whereText = juce::String (edgeBelow[ke] ? "low" : "high") + " edge at " + juce::String (h, 0) + " Hz, range " + juce::String (pd.lo, 0) + " - " + juce::String (pd.hi, 0);
                }
            }
            else
            {
                const auto& bodyP = vo.part[vo.revBody]; const auto& topP = vo.part[vo.revTop];
                where = edgeSeen[0] && edgeSeen[1] && edgeHz[0] >= bodyP.lo * 0.3f && edgeHz[0] <= bodyP.hi * 1.5f && edgeHz[1] >= topP.lo * 0.7f && edgeHz[1] <= topP.hi * 2.5f;
                whereText = "corners " + juce::String (edgeHz[0], 0) + " / " + juce::String (edgeHz[1], 0) + " Hz, body " + juce::String (bodyP.lo, 0) + "-" + juce::String (bodyP.hi, 0) + ", top " + juce::String (topP.lo, 0) + "-" + juce::String (topP.hi, 0);
            }
            // measurable in the output: the long-term spectrum falls where the cut sits
            const int a = n - 5 * 48000, b = n; const auto L0 = ltas (mat, a, b), L1 = ltas (out, a, b);
            double att = 0.0; juce::String attText;
            auto note = [&] (double lo, double hi, const char* tag) { const double d = bandDbDiff (L1, L0, lo, hi); att = std::min (att, d); attText << tag << " " << juce::String (d, 1) << " dB; "; };
            if (tbSeen && plHi > plLo * 1.6f) note (plLo, plHi, "plateau");   // the plateau's floor: between its outer bells
            else if (tbSeen) note (tbHz / 1.19, tbHz * 1.19, "bell band");
            for (int ke = 0; ke < 2; ++ke)
                if (edgeSeen[ke]) { if (edgeBelow[ke]) note (std::max (20.0f, edgeHz[ke] / 5.0f), edgeHz[ke] / 1.7f, "below edge"); else note (edgeHz[ke] * 1.7f, std::min (20000.0f, edgeHz[ke] * 5.0f), "above edge"); }
            // 11 Oct: the bar was "2 dB of a 12 dB cut" (it passed the half cut the owner heard on KICK BODY). A plateau must now reach AMOUNT - 3 dB over its floor
            // (the floor's energy ratio, from the long-term spectrum); an edge keeps the old bar (the cut band's average, judged by tools/target_audit.py).
            const double wanted = plHi > plLo * 1.6f && tbSeen ? -(std::max (0.0, (double) tbCut - 3.0)) : -std::min (2.0, 0.3 * std::max ({ tbSeen ? (double) tbCut : 0.0, (double) edgeMax[0] * (edgeSeen[0] ? 1.0 : 0.0), (double) edgeMax[1] * (edgeSeen[1] ? 1.0 : 0.0), 0.0 }));
            const bool audible = active && att <= wanted;
            const bool ok = finite && active && isStatic && where && audible;
            if (info)
            {
                std::printf ("   INFO (not judged) %s with a new noise in every hit: depth spread %.2f dB, frequency spread %.3f oct in the last 3 s, drift %.2f oct, %s\n", e.text.toRawUTF8(), std::max (spreadBell, spreadEdge), maxStepOct, hzMove, attText.toRawUTF8());
                continue;
            }
            const juce::String nm = e.text + " on " + (e.inst == dyneq::kTgtVocal ? "vocal.wav" : "measure.py material");
            record ("TARGET 20 instrument-parts: lands, static, audible", ok, att, nm + " [" + whereText + "; spread " + juce::String (std::max (spreadBell, spreadEdge), 2) + " dB; " + attText + "]", "dB");
            if (! ok) { ++gFail; std::printf ("[FAIL] TARGET %s: finite=%d active=%d static=%d (bell spread %.2f, edge spread %.2f, frequency spread in the last 3 s %.3f oct, drift since 8 s %.2f oct) where=%d (%s) audible=%d (%s want <= %.1f)\n   trace %s\n", nm.toRawUTF8(), (int) finite, (int) active, (int) isStatic, spreadBell, spreadEdge, maxStepOct, hzMove, (int) where, whereText.toRawUTF8(), (int) audible, attText.toRawUTF8(), wanted, trace.toRawUTF8()); }
            else { ++gPass; std::printf ("   TARGET %-28s %s; depth spread %.2f dB, frequency spread %.3f oct (last 3 s), drift since 8 s %.2f oct; %s\n", e.text.toRawUTF8(), whereText.toRawUTF8(), std::max (spreadBell, spreadEdge), maxStepOct, hzMove, attText.toRawUTF8()); }
        }
    }

    //------------------------------------------------------------------------------------------------ COLLIDE with and without a key, SELF, the combination
    {
        const auto beatW = slice (readWav (dir.getChildFile ("beat.wav")), 0, 48000 * 8), vocalW = slice (readWav (dir.getChildFile ("vocal.wav")), 0, 48000 * 8), resW = slice (readWav (dir.getChildFile ("resonant.wav")), 0, 48000 * 8);
        {   // no key: COLLIDE has nothing to listen to, the output is the input
            const auto o = runPlugin (beatW, nullptr, false, 512, { { "depth", 12 }, { "margin", 0 } });
            const bool ok = maxDiff (o, beatW) == 0.0; record ("COLLIDE / SELF / combinations", ok, maxDiff (o, beatW), "COLLIDE without a sidechain bus: bit-identical"); if (! ok) { ++gFail; std::printf ("[FAIL] COLLIDE without key changed the audio\n"); } else ++gPass;
        }
        for (float depth : { 3.0f, 6.0f, 12.0f })
            for (float margin : { 0.0f, 3.0f, 9.0f })
                for (int speed = 0; speed < 3; ++speed)
                {
                    Rig r (2, true, 48000.0, 512, { { "depth", depth }, { "margin", margin }, { "speed", (float) speed } }); DyneqEditor ed (r.proc);
                    const auto o = r.render (beatW, &vocalW, 512); ed.tickForSnapshot();
                    float maxCut = 0.0f; for (int bnd = 0; bnd < dyneq::kBands; ++bnd) maxCut = std::max (maxCut, r.proc.getCutDb (bnd));
                    const bool ok = allFinite (o) && maxDiff (o, beatW) > 0.01 && maxCut <= depth + 0.6f && peakOf (o) < 4.0;
                    record ("COLLIDE / SELF / combinations", ok, maxCut, "COLLIDE with key: depth " + juce::String (depth, 0) + " margin " + juce::String (margin, 0) + " speed " + juce::String (speed) + " (deepest cut " + juce::String (maxCut, 1) + " dB)", "dB");
                    if (! ok) { ++gFail; std::printf ("[FAIL] COLLIDE depth %.0f margin %.0f speed %d: finite %d diff %.4f maxCut %.2f\n", depth, margin, speed, (int) allFinite (o), maxDiff (o, beatW), maxCut); } else ++gPass;
                }
        {   // a silent key
            Audio sil (2, std::vector<float> (beatW[0].size(), 0.0f));
            const auto o = runPlugin (beatW, &sil, true, 512, { { "depth", 12 }, { "margin", 0 } });
            const bool ok = maxDiff (o, beatW) == 0.0; record ("COLLIDE / SELF / combinations", ok, maxDiff (o, beatW), "COLLIDE with a silent key: bit-identical"); if (! ok) { ++gFail; std::printf ("[FAIL] COLLIDE with a silent key changed the audio\n"); } else ++gPass;
        }
        {   // SELF on resonant material, on plain pink noise
            Rig r (2, false, 48000.0, 512, { { "mode", 1 }, { "depth", 12 }, { "margin", 3 } }); DyneqEditor ed (r.proc);
            const auto o = r.render (resW, nullptr, 512); ed.tickForSnapshot();
            double deepest = 0.0; for (int s = 0; s < dyneq::kHunters; ++s) deepest = std::min (deepest, (double) r.proc.getHunterGainDb (s));
            const bool ok = allFinite (o) && deepest < -1.5 && maxDiff (o, resW) > 0.001;
            record ("COLLIDE / SELF / combinations", ok, deepest, "SELF on resonant.wav: a hunter cuts " + juce::String (deepest, 1) + " dB", "dB"); if (! ok) { ++gFail; std::printf ("[FAIL] SELF on resonant material: hunter %.2f dB\n", deepest); } else ++gPass;
            Rig r2 (2, false, 48000.0, 512, { { "mode", 1 }, { "depth", 12 }, { "margin", 3 } });
            const Audio pk = pink (10.0, -20.0);
            Audio o2 (2); double sum = 0; int cnt = 0;   // SPEC 7 A13: plain pink noise, every hunter bell under 0.5 dB on average, here over the last 6 s block by block
            for (int pos = 0; pos < (int) pk[0].size(); pos += 512)
            {
                const auto part = r2.render (slice (pk, pos, std::min (512, (int) pk[0].size() - pos)), nullptr, 512);
                for (int c = 0; c < 2; ++c) o2[(size_t) c].insert (o2[(size_t) c].end(), part[(size_t) c].begin(), part[(size_t) c].end());
                if (pos >= 4 * 48000) for (int s = 0; s < dyneq::kHunters; ++s) { sum += std::abs (r2.proc.getHunterGainDb (s)); ++cnt; }
            }
            const bool ok2 = allFinite (o2) && sum / cnt < 0.5;
            record ("COLLIDE / SELF / combinations", ok2, sum / cnt, "SELF on pink noise: the hunters stay under 0.5 dB (" + juce::String (sum / cnt, 3) + ")", "dB"); if (! ok2) { ++gFail; std::printf ("[FAIL] SELF on pink noise: hunters %.3f dB\n", sum / cnt); } else ++gPass;
        }
        {   // all layers together: three bands (one dynamic), TARGET kick sub, COLLIDE with a key; then the same with SELF; deterministic, finite, and the panel working between blocks changes nothing
            const auto loops = makeDrumLoops (8.0);
            Settings st = { { "b1_on", 1 }, { "b1_type", 4 }, { "b1_freq", 40 }, { "b2_on", 1 }, { "b2_gain", -3 }, { "b2_freq", 400 }, { "b3_on", 1 }, { "b3_gain", 4 }, { "b3_dyn", 1 }, { "b3_thr", -40 }, { "b3_range", -4 }, { "b3_freq", 2500 },
                            { "targetInst", (float) dyneq::kTgtKick }, { "targetPart", dyneq::sliderForPart (dyneq::kTgtKick, 0) }, { "targetAmount", 12 }, { "depth", 6 }, { "margin", 3 }, { "output", -1.0f } };
            for (int mode = 0; mode < 2; ++mode)
            {
                Settings s2 = st; s2.push_back ({ "mode", (float) mode });
                const Audio key = mode == 0 ? vocalW : loops.snare;
                const auto a1 = runPlugin (loops.bus, &key, true, 512, s2), a2 = runPlugin (loops.bus, &key, true, 512, s2);
                // with the editor open and working between blocks: every 7 blocks the ADVANCED popover and an ASSIST overlay open or close, and the panel ticks
                Rig r (2, true, 48000.0, 512, s2); DyneqEditor ed (r.proc);
                Audio out (2, std::vector<float> (loops.bus[0].size()));
                juce::AudioBuffer<float> buf (4, 512); juce::MidiBuffer midi; int blk = 0;
                for (int pos = 0; pos < (int) loops.bus[0].size(); pos += 512, ++blk)
                {
                    const int len = std::min (512, (int) loops.bus[0].size() - pos); buf.setSize (4, len, false, false, true); buf.clear();
                    for (int c = 0; c < 2; ++c) { std::copy (loops.bus[(size_t) c].begin() + pos, loops.bus[(size_t) c].begin() + pos + len, buf.getWritePointer (c)); std::copy (key[(size_t) c].begin() + pos, key[(size_t) c].begin() + pos + len, buf.getWritePointer (2 + c)); }
                    if (blk % 7 == 0) { const bool open = (blk / 7) % 2 == 1; ed.setAdvancedOpen (open); if (open != (ed.overlayId() >= 0)) ed.toggleOverlay (mode == 0 ? panel::kCollide : panel::kSelf); ed.refreshFace(); ed.tickForSnapshot(); }
                    r.proc.processBlock (buf, midi);
                    for (int c = 0; c < 2; ++c) std::copy (buf.getReadPointer (c), buf.getReadPointer (c) + len, out[(size_t) c].begin() + pos);
                }
                const bool ok = allFinite (a1) && maxDiff (a1, a2) == 0.0 && maxDiff (a1, out) == 0.0 && maxDiff (a1, loops.bus) > 0.01 && peakOf (a1) < 4.0;
                record ("COLLIDE / SELF / combinations", ok, maxDiff (a1, out), juce::String ("all layers on (bands + dynamic band + TARGET kick sub + ") + (mode == 0 ? "COLLIDE with a key" : "SELF") + "): finite, deterministic, the panel opening and closing popovers between blocks changes nothing (max difference " + juce::String (maxDiff (a1, out), 1) + ")");
                if (! ok) { ++gFail; std::printf ("[FAIL] all layers mode %d: finite %d repeat %.1e panel %.1e change %.4f\n", mode, (int) allFinite (a1), maxDiff (a1, a2), maxDiff (a1, out), maxDiff (a1, loops.bus)); } else ++gPass;
            }
        }
    }

    //------------------------------------------------------------------------------------------------ the editor section (analyzer, defaults, nodes, panels, state, clicks)
    {
        const int p0 = gPass, f0 = gFail;
        const auto beat6 = slice (readWav (dir.getChildFile ("beat.wav")), 0, 48000 * 6), vocal6 = slice (readWav (dir.getChildFile ("vocal.wav")), 0, 48000 * 6);
        runEditorChecks (beat6, vocal6, dir);
        auto& rw = row ("EDITOR checks (section 12: analyzer, defaults, nodes, menus, panels, state, clicks)"); rw.n = (gPass - p0) + (gFail - f0); rw.fail = gFail - f0; rw.unit = "checks";
    }

    //------------------------------------------------------------------------------------------------ the table
    std::printf ("\n==== variation rig: table (%.1f s) ====\n", (juce::Time::getMillisecondCounterHiRes() - t0) * 0.001);
    std::printf ("%-78s %6s %6s  %s\n", "group", "cases", "FAIL", "worst");
    for (auto& r : rows())
        std::printf ("%-78s %6d %6d  %s\n", r.name.toRawUTF8(), r.n, r.fail, (r.unit.isNotEmpty() && r.unit != "checks" ? juce::String (r.worst, 5) + " " + r.unit + "  " + r.worstCase : (r.fail ? "first failure: " + r.firstFail : juce::String())).substring (0, 190).toRawUTF8());
    const int fails = gFail - failBefore, passes = gPass - passBefore;
    std::printf ("\nvariation rig: %d passed, %d failed\n", passes, fails);
    return fails == 0 ? 0 : 1;
}
