// HAVA (YAY) renderer + selftest.
//   render --selftest
//   render in.wav out.wav [--preset "AIR LIGHT"] [--param k=v ...]
//     (k: air sheen output(dB) bypass(0/1) tension drip twang lowcut(Hz) safe(0/1); legacy: kick presence)
#include "../Source/YayEngine.h"
#include "wav.h"
#include "presets.h"
#include "pluck.h"
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using Buf = std::vector<float>;  // interleaved stereo

// PRIMARY reference: Erkin Koray "Cemalim" (1974), 15-45 s, measured by tools/measure_reference.py
// (see STATUS.md). The Fender '63 tank demo is only a secondary sanity check (tank delay, chirp).
static constexpr double kRefCentroidHz = 1666.0;
static constexpr double kRefTwangHz = 1011.0;
static constexpr double kRefTiltDbOct = -3.21;
static constexpr double kRefHumpHz = 1462.0;  // tail max in 1.2-4 kHz
// Cemalim 15-45 s mean ATTACK (5-60 ms) spectrum, 1/3-octave bands 100 Hz .. 10.16 kHz, dB re max
static const double kCemalimAttackDb[21] = {-1.6, -3.9, 0.0, -4.5, -7.3, -11.3, -15.4, -13.8, -15.6, -18.2, -17.4,
                                            -23.8, -22.5, -26.4, -28.6, -28.4, -31.6, -32.6, -31.8, -32.4, -27.0};

static double rmsOf(const Buf& x, size_t a = 0, size_t b = SIZE_MAX) {
    b = std::min(b, x.size());
    double s = 0;
    for (size_t i = a; i < b; ++i) s += double(x[i]) * x[i];
    return std::sqrt(s / double(std::max<size_t>(1, b - a)));
}
static double db(double v) { return 20.0 * std::log10(std::max(1e-12, v)); }

static yay::YayEngine gEngine;  // large object: keep off the stack
static double gAgMin = 1e9, gAgMax = -1e9;  // MASTER auto-gain range after the first second (dB)

static Buf renderBuf(const Buf& in, int rate, const yay::Controls& c, bool clank = true, float* clankPeak = nullptr, bool comp = false) {
    gEngine.prepare(rate);
    const size_t lat = comp ? size_t(gEngine.latency(c.masterSafe)) : 0;  // latency compensation (MASTER look-ahead)
    Buf o = in, l(in.size() / 2 + lat, 0.0f), r(in.size() / 2 + lat, 0.0f);
    for (size_t i = 0; i < in.size() / 2; i++) { l[i] = in[2 * i]; r[i] = in[2 * i + 1]; }
    gEngine.setClankEnabled(clank);
    const int block = 512;
    for (size_t p = 0; p < l.size(); p += block) {
        const int n = int(std::min<size_t>(block, l.size() - p));
        gEngine.process(l.data() + p, r.data() + p, n, c);
        if (p >= size_t(rate)) { gAgMin = std::min(gAgMin, double(gEngine.autoGainDb())); gAgMax = std::max(gAgMax, double(gEngine.autoGainDb())); }
    }
    if (clankPeak) *clankPeak = gEngine.clankPeak();
    gEngine.setClankEnabled(true);
    for (size_t i = 0; i < o.size() / 2; i++) { o[2 * i] = l[i + lat]; o[2 * i + 1] = r[i + lat]; }
    return o;
}
static Buf renderC(const Buf& in, int rate, const yay::Controls& c) { return renderBuf(in, rate, c, true, nullptr, true); }

static bool finiteBelow(const Buf& x) {
    for (float v : x) if (!std::isfinite(v) || std::fabs(v) >= 2.0f) return false;  // +6 dBFS
    return true;
}

static bool check(bool ok, const std::string& name, const std::string& v) {
    std::cout << (ok ? "PASS " : "FAIL ") << name << " " << v << "\n";
    return ok;
}

// ---------- analysis helpers ----------
using Cplx = std::vector<std::complex<double>>;
static void fft(Cplx& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * 3.141592653589793 / double(len);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1);
            for (size_t k = 0; k < len / 2; ++k) {
                auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v; a[i + k + len / 2] = u - v; w *= wl;
            }
        }
    }
}
static double hann(size_t i, size_t len) { return 0.5 - 0.5 * std::cos(2 * 3.141592653589793 * double(i) / double(len - 1)); }

// power spectrum of x[a..b) (Hann), zero-padded to n
static std::vector<double> pspec(const std::vector<double>& x, size_t a, size_t b, size_t n) {
    Cplx f(n, 0.0);
    const size_t len = b - a;
    for (size_t i = 0; i < len && a + i < x.size(); ++i) f[i] = x[a + i] * hann(i, len);
    fft(f);
    std::vector<double> p(n / 2 + 1);
    for (size_t k = 0; k <= n / 2; ++k) p[k] = std::norm(f[k]);
    return p;
}
static double bandPower(const std::vector<double>& p, size_t n, int rate, double lo, double hi) {
    double s = 0;
    for (size_t k = 0; k < p.size(); ++k) { const double hz = double(k) * rate / double(n); if (hz >= lo && hz <= hi) s += p[k]; }
    return s;
}
// 1/6-octave smoothing (same definition as measure_reference.py)
static std::vector<double> smoothOct(const std::vector<double>& p, size_t n, int rate) {
    std::vector<double> c(p.size() + 1, 0.0), o(p.size());
    for (size_t k = 0; k < p.size(); ++k) c[k + 1] = c[k] + p[k];
    const double df = double(rate) / double(n);
    for (size_t k = 0; k < p.size(); ++k) {
        if (k == 0) { o[k] = p[k]; continue; }
        const double f = k * df;
        const long lo = long(std::ceil(f * std::pow(2.0, -0.5 / 6) / df)), hi = long(std::floor(f * std::pow(2.0, 0.5 / 6) / df));
        const long a = std::max(0L, lo), b = std::min(long(p.size()) - 1, hi);
        o[k] = b >= a ? (c[size_t(b) + 1] - c[size_t(a)]) / double(b - a + 1) : p[k];
    }
    return o;
}

// wet-only (BLEND 1) mono impulse response
static std::vector<double> wetIR(yay::Controls c, int rate, double seconds) {
    const size_t n = size_t(seconds * rate);
    Buf in(n * 2, 0.0f);
    in[0] = in[1] = 1.0f;
    c.sheen = 1.0f; c.air = 0.0f;
    Buf o = renderBuf(in, rate, c);
    std::vector<double> w(n);  // wet only: output minus the (unity) dry
    for (size_t i = 0; i < n; ++i) w[i] = 0.5 * ((double(o[2 * i]) - in[2 * i]) + (double(o[2 * i + 1]) - in[2 * i + 1]));
    return w;
}
// wet-only render (output minus input), SHEEN path only (AIR 0)
static Buf renderWet(const Buf& in, int rate, yay::Controls c) {
    c.air = 0.0f;
    Buf o = renderBuf(in, rate, c);
    for (size_t i = 0; i < o.size(); ++i) o[i] -= in[i];
    return o;
}


// ---------- AMFI helpers ----------
// power of harmonics 1..10 of f0 in the mono sum, window 0.5..1.5 s (Hann, N = 65536), band +-bw Hz
static std::vector<double> harmonics(const Buf& o, int rate, double f0, double bw) {
    std::vector<double> m(o.size() / 2);
    for (size_t i = 0; i < m.size(); ++i) m[i] = 0.5 * (double(o[2 * i]) + o[2 * i + 1]);
    const size_t N = 65536;
    auto p = pspec(m, size_t(0.5 * rate), size_t(1.5 * rate), N);
    std::vector<double> h(11, 0.0);
    for (int k = 1; k <= 10; ++k) if (f0 * k < 0.45 * rate) h[size_t(k)] = bandPower(p, N, rate, f0 * k - bw, f0 * k + bw);
    return h;
}
static double thdPct(const std::vector<double>& h) { double s = 0; for (int k = 2; k <= 10; ++k) s += h[size_t(k)]; return 100.0 * std::sqrt(s / h[1]); }
static Buf sineSt(int rate, double f, double amp, double secs) {
    Buf b(size_t(secs * rate) * 2);
    for (size_t i = 0; i < b.size() / 2; ++i) b[2 * i] = b[2 * i + 1] = float(amp * std::sin(2 * 3.141592653589793 * f * double(i) / rate));
    return b;
}
// K-weighted mean power of a stereo buffer (sum of both channels), BS.1770 pre-filter, no gating
static double kPower(const Buf& x, int rate) {
    yay::KWeight k; k.setup(rate); k.reset();
    double s = 0;
    for (size_t i = 0; i < x.size() / 2; ++i) { const double a = k.run(x[2 * i], 0), b = k.run(x[2 * i + 1], 1); s += a * a + b * b; }
    return s / double(std::max<size_t>(1, x.size() / 2));
}
static double kLU(const Buf& out, const Buf& in, int rate) { return 10.0 * std::log10(kPower(out, rate) / kPower(in, rate)); }
// stereo pink noise (Kellet filter), common part + 30 % decorrelated part, scaled to rmsDb
static Buf pinkSt(int rate, double secs, double rmsDb, uint32_t seed) {
    std::mt19937 g(seed); std::uniform_real_distribution<double> u(-1, 1);
    const size_t n = size_t(secs * rate);
    Buf b(n * 2);
    double s[2][7] = {};
    auto pk = [&](double w, double* q) {
        q[0] = 0.99886 * q[0] + w * 0.0555179; q[1] = 0.99332 * q[1] + w * 0.0750759; q[2] = 0.96900 * q[2] + w * 0.1538520;
        q[3] = 0.86650 * q[3] + w * 0.3104856; q[4] = 0.55000 * q[4] + w * 0.5329522; q[5] = -0.7616 * q[5] - w * 0.0168980;
        const double y = q[0] + q[1] + q[2] + q[3] + q[4] + q[5] + q[6] + w * 0.5362; q[6] = w * 0.115926; return y;
    };
    for (size_t i = 0; i < n; ++i) { const double a = pk(u(g), s[0]), d = pk(u(g), s[1]); b[2 * i] = float(a + 0.3 * d); b[2 * i + 1] = float(a - 0.3 * d); }
    const double r = rmsOf(b), gn = std::pow(10.0, rmsDb / 20.0) / r;
    for (float& v : b) v = float(v * gn);
    return b;
}
// music-like stereo mix: pluck melody (left), kick every beat (centre), hats (right), wide saw pad; peak -1 dBFS
static Buf musicSt(int rate, double secs) {
    const size_t n = size_t(secs * rate);
    std::vector<float> pl(n, 0.0f);
    for (double t0 = 0.2; t0 + 5.0 < secs; t0 += 4.8) pluck::melody(pl, size_t(t0 * rate), rate);
    Buf b(n * 2, 0.0f);
    std::mt19937 g(99); std::uniform_real_distribution<double> u(-1, 1);
    double hpPrev = 0, padPh[3][2] = {};
    const double padF[3] = {73.42, 110.0, 146.83};
    for (size_t i = 0; i < n; ++i) {
        const double t = double(i) / rate, beat = std::fmod(t, 0.6);
        const double kick = beat < 0.3 ? 0.8 * std::exp(-beat / 0.08) * std::sin(2 * 3.141592653589793 * (50.0 * beat + 40.0 * 0.03 * (1 - std::exp(-beat / 0.03)))) : 0.0;
        const double hb = std::fmod(t + 0.3, 0.6);
        const double w = u(g), hp = w - hpPrev; hpPrev = w;
        const double hat = 0.12 * hp * std::exp(-hb / 0.02);
        double pad[2] = {0, 0};
        for (int k = 0; k < 3; ++k) for (int ch = 0; ch < 2; ++ch) {
            padPh[k][ch] += padF[k] * (ch ? 1.003 : 0.997) / rate; padPh[k][ch] -= std::floor(padPh[k][ch]);
            pad[ch] += 0.05 * (2.0 * padPh[k][ch] - 1.0);
        }
        b[2 * i] = float(0.75 * pl[i] + kick + 0.4 * hat + pad[0]);
        b[2 * i + 1] = float(0.35 * pl[i] + kick + 1.0 * hat + pad[1]);
    }
    float pk = 0; for (float v : b) pk = std::max(pk, std::fabs(v));
    for (float& v : b) v *= float(std::pow(10.0, -1.0 / 20.0) / pk);
    return b;
}

// true peak (dBTP) of a stereo buffer: 4x oversampling, windowed-sinc interpolation, 64 taps per phase
static double truePeakDb(const Buf& x) {
    const int H = 32;
    double h[4][2 * H];
    for (int k = 0; k < 4; ++k) {
        double sum = 0;
        for (int j = 0; j < 2 * H; ++j) {
            const double t = double(j - (H - 1)) - k / 4.0;
            const double sc = std::fabs(t) < 1e-12 ? 1.0 : std::sin(3.141592653589793 * t) / (3.141592653589793 * t);
            h[k][j] = sc * (0.5 + 0.5 * std::cos(3.141592653589793 * t / (H + 0.5)));
            sum += h[k][j];
        }
        for (int j = 0; j < 2 * H; ++j) h[k][j] /= sum;
    }
    const long n = long(x.size() / 2);
    double pk = 0;
    for (int ch = 0; ch < 2; ++ch)
        for (long p = 0; p < n; ++p) {
            pk = std::max(pk, double(std::fabs(x[2 * size_t(p) + size_t(ch)])));
            for (int k = 1; k < 4; ++k) {
                double a = 0;
                for (int j = 0; j < 2 * H; ++j) { const long q = p - (H - 1) + j; if (q >= 0 && q < n) a += h[k][j] * x[2 * size_t(q) + size_t(ch)]; }
                pk = std::max(pk, std::fabs(a));
            }
        }
    return 20.0 * std::log10(std::max(1e-12, pk));
}

// synthetic vocal-like phrase into st[0 .. P) (stereo, identical channels), peak -3 dBFS
static void detail_vox(Buf& st, size_t P, int rate) {
    std::vector<double> x(P, 0.0);
    double ph = 0; uint32_t rs = 0xBEEFu;
    struct Res { double b0, a1, a2, y1 = 0, y2 = 0; };
    auto mk = [&](double f, double bw) { const double r = std::exp(-3.141592653589793 * bw / rate), w = 2 * 3.141592653589793 * f / rate; return Res{1.0 - r, -2 * r * std::cos(w), r * r}; };
    Res fo[3] = {mk(700, 110), mk(1220, 120), mk(2600, 160)};
    double hpPrev = 0;
    for (size_t i = 0; i < P; ++i) {
        const double t = double(i) / rate, u = t / 1.5;
        const double f0 = 180.0 + 40.0 * std::sin(3.141592653589793 * u) - 20.0 * u;
        ph += f0 / rate; if (ph >= 1.0) ph -= 1.0;
        const double pulse = std::pow(ph, 3.0) - 0.25;                       // glottal-like asymmetric pulse
        const double syl = std::fmod(t, 0.375) / 0.375;                        // 4 syllables
        const double env = std::sin(3.141592653589793 * std::min(1.0, syl * 1.15));
        double v = 0;
        for (auto& r : fo) { const double y = r.b0 * pulse - r.a1 * r.y1 - r.a2 * r.y2; r.y2 = r.y1; r.y1 = y; v += y; }
        rs = rs * 1664525u + 1013904223u; const double w = double(int32_t(rs)) / 2147483648.0, hp = w - hpPrev; hpPrev = w;
        const double fric = syl < 0.12 ? 0.25 * hp * (1.0 - syl / 0.12) : 0.0;  // "s"-like onset
        x[i] = env * v + fric;
    }
    double pk = 0; for (double v : x) pk = std::max(pk, std::fabs(v));
    const size_t F = size_t(0.005 * rate);
    for (size_t i = 0; i < P; ++i) {
        double g = std::pow(10.0, -3.0 / 20.0) / pk;
        if (i < F) g *= double(i) / F;
        if (i >= P - F) g *= double(P - 1 - i) / F;
        st[2 * i] = st[2 * i + 1] = float(x[i] * g);
    }
}

// ---------- selftest ----------
static int selftest() {
    int checks = 0, fails = 0;
    auto CK = [&](bool ok, const std::string& n, const std::string& v) { ++checks; if (!check(ok, n, v)) ++fails; };
    char s[400];

    Buf input(48000 * 5 * 2);
    for (size_t i = 0; i < input.size() / 2; i++) { float t = float(i) / 48000.0f; input[2 * i] = input[2 * i + 1] = 0.3f * std::sin(2 * 3.14159265f * 1000 * t); }
    Buf gNoise5(48000 * 5 * 2);
    { std::mt19937 g(3); std::uniform_real_distribution<float> u(-.3f, .3f); for (float& v : gNoise5) v = u(g); }
    const yay::Controls c = preset("AIR + SHEEN");  // base for control / sheen checks (AIR .5, SHEEN .6)

    // determinism
    { auto a = renderBuf(gNoise5, 48000, preset("AIR FULL")), b = renderBuf(gNoise5, 48000, preset("AIR FULL")); CK(a == b, "determinism", "bit-identical"); }
    // default bypass-equivalence: a default-constructed Controls (AIR 0, SHEEN 0, KICK 0, OUTPUT 0, MASTER SAFE off)
    { yay::Controls d; auto o = renderBuf(gNoise5, 48000, d); CK(o == gNoise5 && d.kick == 0.0f && !d.masterSafe, "default_bypass_equivalence", "default controls (AIR 0, SHEEN 0, legacy KICK 0, MASTER SAFE off): bit-exact dry"); }
    // AIR 0 + SHEEN 0 with MASTER SAFE off, other rear controls moved: still bit-exact
    { yay::Controls d = preset("AIR FULL"); d.air = 0; d.sheen = 0; d.tension = 0; d.drip = 1; d.twang = 1; d.lowcut = 300;
      auto o = renderBuf(gNoise5, 48000, d); CK(o == gNoise5, "bypass_controls", "AIR 0 + SHEEN 0, TENSION/DRIP/TWANG/LOW CUT moved: bit-exact dry"); }
    // BYPASS button, MASTER SAFE off / on
    for (int sf : {0, 1}) {
        yay::Controls d = preset("AIR + SHEEN"); d.bypass = true; d.masterSafe = sf != 0;
        auto o = renderC(gNoise5, 48000, d);
        CK(o == gNoise5, std::string("bypass_button_") + (sf ? "safe_on" : "safe_off"), sf ? "BYPASS: bit-exact after latency compensation" : "BYPASS: bit-exact");
    }
    // MASTER SAFE on, AIR 0 + SHEEN 0: delayed dry, bit-exact after compensation
    { yay::Controls d; d.masterSafe = true; auto o = renderC(gNoise5, 48000, d); CK(o == gNoise5, "bypass_exact_safe_on", "AIR 0 + SHEEN 0, MASTER SAFE on: bit-exact after latency compensation"); }
    // latency: 0 with MASTER SAFE off; constant (80 samples at 48 kHz) with it on, across settings
    {
        Buf imp(48000 * 2, 0.0f); imp[2 * 100] = imp[2 * 100 + 1] = 0.5f;
        auto meas = [&](yay::Controls q) {
            auto o = renderBuf(imp, 48000, q);
            int first = -1; for (size_t i = 0; i < o.size() / 2; ++i) if (std::fabs(o[2 * i]) > 1e-6f) { first = int(i); break; }
            return first - 100;
        };
        int m0[3], m1[3];
        yay::Controls qs[3] = {yay::Controls{}, preset("AIR FULL"), preset("AIR + SHEEN")};
        for (int k = 0; k < 3; ++k) { yay::Controls q = qs[k]; q.masterSafe = false; if (k == 0) q.air = 0.3f; m0[k] = meas(q); q.masterSafe = true; if (k == 0) q.air = 0; m1[k] = meas(q); }
        std::snprintf(s, sizeof s, "MASTER SAFE off: reported=%d measured=%d/%d/%d samples", gEngine.latency(false), m0[0], m0[1], m0[2]);
        CK(gEngine.latency(false) == 0 && m0[0] == 0 && m0[1] == 0 && m0[2] == 0, "latency_safe_off", s);
        std::snprintf(s, sizeof s, "MASTER SAFE on: reported=%d measured=%d/%d/%d samples (AIR0+SHEEN0 / AIR FULL / AIR+SHEEN), constant", gEngine.latency(true), m1[0], m1[1], m1[2]);
        CK(gEngine.latency(true) == 80 && m1[0] == 80 && m1[1] == 80 && m1[2] == 80, "latency_safe_on", s);
    }
    // finite / bounded: 3 rates x 4 signals x MASTER SAFE off/on, AIR 1 SHEEN 1
    for (int rate : {44100, 48000, 96000}) {
        Buf z(size_t(rate) * 2, 0), si = z, no = z, sw = z;
        std::mt19937 ng(11 + rate); std::uniform_real_distribution<float> nu(-.8f, .8f);
        for (int i = 0; i < rate; i++) {
            float v = .999f * std::sin(2 * 3.14159265f * 1000 * i / rate); si[2 * i] = si[2 * i + 1] = v;
            float nv = nu(ng); no[2 * i] = no[2 * i + 1] = nv;
            double ph = 2 * 3.141592653589793 * 20.0 * (rate / std::log(1000.0)) * (std::pow(1000.0, double(i) / rate) - 1.0) / rate;
            sw[2 * i] = sw[2 * i + 1] = float(.8 * std::sin(ph));
        }
        const char* nm[] = {"silence", "fullscale_sine", "white_noise", "20hz_20khz_sweep"};
        const Buf* src[] = {&z, &si, &no, &sw};
        for (int sf : {0, 1}) {
            yay::Controls q; q.air = 1; q.sheen = 1; q.masterSafe = sf != 0;
            for (int k = 0; k < 4; ++k) {
                auto o = renderBuf(*src[k], rate, q);
                float pk = 0; for (float v : o) pk = std::max(pk, std::fabs(v));
                std::snprintf(s, sizeof s, "peak=%.2f dBFS (<+6, finite)", db(pk));
                CK(finiteBelow(o), std::string("finite_") + (sf ? "safe_" : "") + nm[k] + "_" + std::to_string(rate), s);
            }
        }
    }
    // every panel control changes the output (effect on)
    {
        const char* names[] = {"air", "sheen", "output", "bypass", "tension", "drip", "twang", "lowcut", "master_safe"};
        auto ob = renderC(gNoise5, 48000, c);
        for (int k = 0; k < 9; k++) {
            yay::Controls q = c;
            if (k == 0) q.air = .9f; if (k == 1) q.sheen = .1f; if (k == 2) q.output = -3.0f; if (k == 3) q.bypass = true;
            if (k == 4) q.tension = .05f; if (k == 5) q.drip = 0.8f; if (k == 6) q.twang = 0.0f;
            if (k == 7) q.lowcut = 80.0f; if (k == 8) q.masterSafe = true;
            auto oq = renderC(gNoise5, 48000, q);
            Buf diff(oq.size()); for (size_t i = 0; i < diff.size(); i++) diff[i] = oq[i] - ob[i];
            const double d = db(rmsOf(diff));
            std::snprintf(s, sizeof s, "%.2f dB", d);
            CK(d > -60, std::string("control_") + names[k], s);
        }
    }

    const int R = 44100;
    // tank on the default path is clean (legacy KICK 0): full-scale 1 kHz sine, SHEEN 1 (colour only)
    {
        const int FR = 48000;
        const Buf sine = sineSt(FR, 1000.0, 1.0, 1.6);
        yay::Controls q = c; q.air = 0; q.sheen = 1.0f;
        float cp = 0;
        auto h = harmonics(renderBuf(sine, FR, q, true, &cp), FR, 1000.0, 40.0);
        std::snprintf(s, sizeof s, "SHEEN 1, full-scale 1 kHz sine: THD=%.3f %% (need <0.5), clank env peak=%.3f (need 0)", thdPct(h), cp);
        CK(thdPct(h) < 0.5 && cp == 0.0f, "tank_clean_default", s);
    }
    // (b) TWANG: the wet's 1-3 kHz band rises 4-8 dB from TWANG 0 to 1 (white noise, KICK 0, colour only)
    {
        const int FR = 48000;
        Buf nz(size_t(FR) * 2 * 2);
        std::mt19937 g(5); std::uniform_real_distribution<float> u(-.25f, .25f);
        for (size_t i = 0; i < nz.size() / 2; ++i) nz[2 * i] = nz[2 * i + 1] = u(g);
        auto band = [&](float tw) {
            yay::Controls q = c; q.kick = 0; q.twang = tw; q.sheen = 1;
            Buf o = renderWet(nz, FR, q);
            std::vector<double> m(o.size() / 2);
            for (size_t i = 0; i < m.size(); ++i) m[i] = 0.5 * (double(o[2 * i]) + o[2 * i + 1]);
            const size_t N = 131072;
            auto p = pspec(m, size_t(0.5 * FR), size_t(2.0 * FR), N);
            return 10.0 * std::log10(bandPower(p, N, FR, 1000, 3000));
        };
        const double rise = band(1.0f) - band(0.0f);
        std::snprintf(s, sizeof s, "wet 1-3 kHz band TWANG 0->1 = %+.2f dB (need +4..+8)", rise);
        CK(rise >= 4.0 && rise <= 8.0, "twang_band_rise", s);
    }
    // (c) tank delay from IR autocorrelation (first 300 ms): the round-4 short tank
    {
        yay::Controls q = c; q.kick = 0;
        auto ir = wetIR(q, R, 0.3);
        const size_t N = ir.size();
        auto ac = [&](size_t lag) { double a = 0; for (size_t i = 0; i + lag < N; ++i) a += ir[i] * ir[i + lag]; return a; };
        const double r0 = ac(0);
        size_t best = 0; double bestV = -1e30; std::vector<double> absr;
        for (size_t lag = size_t(0.020 * R); lag <= size_t(0.060 * R); ++lag) { double v = ac(lag) / r0; absr.push_back(std::fabs(v)); if (v > bestV) { bestV = v; best = lag; } }
        std::nth_element(absr.begin(), absr.begin() + absr.size() / 2, absr.end());
        const double med = absr[absr.size() / 2], lagMs = 1000.0 * best / R;
        std::snprintf(s, sizeof s, "lag=%.2f ms peak_ratio=%.3f prominence=%.1fx (need 25-45 ms, ratio>=0.2, prominence>=4)", lagMs, bestV, bestV / med);
        CK(lagMs >= 25 && lagMs <= 45 && bestV >= 0.2 && bestV / med >= 4, "tank_delay_short", s);
    }
    // (d) SHEEN wet T60 <= 0.15 s at every SHEEN value (impulse, out - in, AIR 0, Schroeder T30 fit)
    for (float sv : {0.15f, 0.5f, 1.0f}) {
        yay::Controls q; q.sheen = sv;
        const int FR = 48000;
        Buf imp(size_t(FR) * 2, 0.0f); imp[0] = imp[1] = 1.0f;
        const Buf o = renderBuf(imp, FR, q);
        const size_t n = o.size() / 2;
        std::vector<double> edc(n); double acc = 0;
        for (size_t i = n; i-- > 0;) { const double w = 0.5 * ((double(o[2 * i]) - imp[2 * i]) + (double(o[2 * i + 1]) - imp[2 * i + 1])); acc += w * w; edc[i] = acc; }
        double sx = 0, sy = 0, sxx = 0, sxy = 0; int m = 0;
        for (size_t i = 0; i < n; ++i) { const double d = 10.0 * std::log10(std::max(1e-300, edc[i] / edc[0])); if (d <= -5.0 && d >= -35.0) { const double t = double(i) / FR; sx += t; sy += d; sxx += t * t; sxy += t * d; ++m; } }
        const double t60 = -60.0 / ((m * sxy - sx * sy) / (m * sxx - sx * sx));
        std::snprintf(s, sizeof s, "SHEEN %.2f wet T60 = %.3f s (round-4 tank 0.63 s; glue bound: need <= 0.7)", sv, t60);
        char nm[48]; std::snprintf(nm, sizeof nm, "sheen_t60_%.2f", sv);
        CK(t60 > 0 && t60 <= 0.7, nm, s);
    }
    // (d1) SHEEN is ADDED colour: the dry component in the output stays at 0 dB (correlation with the dry),
    //      white noise, SHEEN 1, AIR 0. MASTER SAFE on: SIDE dry exactly 0 dB, MID dry = the auto-gain.
    {
        const int FR = 48000;
        Buf nz(size_t(FR) * 4 * 2);
        { std::mt19937 g(77); std::uniform_real_distribution<float> u(-.3f, .3f); for (size_t i = 0; i < nz.size() / 2; ++i) { const float a = u(g), b = u(g); nz[2 * i] = a + 0.3f * b; nz[2 * i + 1] = a - 0.3f * b; } }
        auto dryDb = [&](const Buf& o, int part) {  // part 0: L+R stereo, 1: MID, 2: SIDE
            double xy = 0, xx = 0;
            for (size_t i = 0; i < o.size() / 2; ++i) {
                double x, y;
                if (part == 0) { xy += double(o[2 * i]) * nz[2 * i] + double(o[2 * i + 1]) * nz[2 * i + 1]; xx += double(nz[2 * i]) * nz[2 * i] + double(nz[2 * i + 1]) * nz[2 * i + 1]; continue; }
                if (part == 1) { x = 0.5 * (double(nz[2 * i]) + nz[2 * i + 1]); y = 0.5 * (double(o[2 * i]) + o[2 * i + 1]); }
                else { x = 0.5 * (double(nz[2 * i]) - nz[2 * i + 1]); y = 0.5 * (double(o[2 * i]) - o[2 * i + 1]); }
                xy += x * y; xx += x * x;
            }
            return 20 * std::log10(xy / xx);
        };
        for (float sv : {0.2f, 1.0f}) {
            yay::Controls q; q.sheen = sv;
            const double d = dryDb(renderC(nz, FR, q), 0);
            std::snprintf(s, sizeof s, "SHEEN %.1f, MASTER SAFE off: dry component %+.3f dB (need 0 +-0.1)", sv, d);
            char nm[48]; std::snprintf(nm, sizeof nm, "sheen_dry_unity_%.1f", sv);
            CK(std::fabs(d) <= 0.1, nm, s);
        }
        {
            yay::Controls q; q.sheen = 1.0f; q.masterSafe = true;
            gAgMin = 1e9; gAgMax = -1e9;
            const Buf o = renderC(nz, FR, q);
            const double dm = dryDb(o, 1), ds = dryDb(o, 2), ag = 0.5 * (gAgMin + gAgMax);
            std::snprintf(s, sizeof s, "SHEEN 1, MASTER SAFE on: SIDE dry %+.4f dB (need 0 +-0.01), MID dry %+.3f dB vs auto-gain %+.3f dB (range %+.2f..%+.2f; need within 0.1): no crossfade, only the auto-gain",
                          ds, dm, ag, gAgMin, gAgMax);
            CK(std::fabs(ds) <= 0.01 && std::fabs(dm - ag) <= 0.1, "sheen_dry_unity_safe", s);
        }
    }
    // defaults and MASTER AIR
    {
        auto same = [](const yay::Controls& a, const yay::Controls& b, bool ignoreSafe) {
            return a.air == b.air && a.sheen == b.sheen && a.output == b.output && a.bypass == b.bypass && a.tension == b.tension && a.drip == b.drip &&
                   a.twang == b.twang && a.lowcut == b.lowcut && (ignoreSafe || a.masterSafe == b.masterSafe) && a.kick == b.kick && a.presence == b.presence;
        };
        const yay::Controls dc = yay::defaultControls(), af = preset("AIR FULL");
        const bool bit = renderBuf(gNoise5, 48000, dc) == renderBuf(gNoise5, 48000, af);
        CK(same(dc, af, false) && bit, "defaults_are_air_full", "defaultControls() == AIR FULL (AIR .8, SHEEN .25, LOW CUT 300, DRIP .5, MASTER SAFE off): all fields equal, renders bit-identical");
        yay::Controls ma = preset("MASTER AIR");   // 2026-10-04: the master colour has no spring (SHEEN 0); otherwise AIR FULL + SAFE
        yay::Controls afNoSheen = af; afNoSheen.sheen = 0.0f; afNoSheen.drip = ma.drip;   // DRIP acts only on the spring
        const bool onlySafe = same(ma, afNoSheen, true) && ma.masterSafe && !af.masterSafe && ma.sheen == 0.0f;
        yay::Controls maNoSafe = ma; maNoSafe.masterSafe = false;
        const bool bit2 = renderC(gNoise5, 48000, maNoSafe) == renderC(gNoise5, 48000, afNoSheen);
        CK(onlySafe && bit2, "master_air_is_air_full_no_sheen_plus_safe", "MASTER AIR = AIR FULL with SHEEN 0 + MASTER SAFE; with the SAFE stage off the renders are bit-identical");
    }
    // meter taps (printed values + sanity gates): last-block values after a 5 s noise render
    {
        yay::Controls q; auto o0 = renderBuf(gNoise5, 48000, q);
        const float a0 = gEngine.airLiftDb(), s00 = gEngine.sheenLevelDb(0);
        q = preset("AIR FULL"); renderBuf(gNoise5, 48000, q);
        const float a1 = gEngine.airLiftDb(), s1l = gEngine.sheenLevelDb(0), s1r = gEngine.sheenLevelDb(1);
        q = preset("MASTER AIR"); renderC(gNoise5, 48000, q);
        const float a2 = gEngine.airLiftDb(), ag = gEngine.autoGainDb();
        std::snprintf(s, sizeof s, "neutral: airLift %.2f dB sheen %.1f dBFS | AIR FULL: airLift %+.2f dB, sheen L/R %.1f/%.1f dBFS | MASTER AIR: airLift %+.2f dB, autoGain %+.2f dB",
                      a0, s00, a1, s1l, s1r, a2, ag);
        CK(a0 == 0.0f && s00 == -120.0f && a1 > 1.0f && a1 < 8.0f && s1l > -60.0f && s1l < 0.0f && a2 > 1.0f && a2 < 8.0f && std::isfinite(ag), "meter_taps", s);
    }
    // (d2) GLUE, NOT A ROOM (owner chose the round-4 tail): gated at SHEEN 1.0 (AIR 0), AIR 1 SHEEN 1 and its
    //   MASTER SAFE variant; every preset printed. Impulse: wet (out - in) energy in 300-320 ms after the hit
    //   <= -25 dB re the first 20 ms of the wet (from its onset: the tank's first arrival is ~29 ms after the
    //   hit, so a window fixed at the hit would hold no tank energy). The wet T60 bound (<= 0.7 s) is
    //   sheen_t60_* above. Vocal-like phrase tail (300-600 ms after the phrase) printed only.
    {
        const int FR = 48000;
        // synthetic vocal phrase: glottal pulse train (f0 glide 180-220-160 Hz), 3 formants, 4 syllables,
        // fricative noise at syllable starts, ends with a 5 ms fade, then 1.2 s silence
        Buf voc(size_t(2.7 * FR) * 2, 0.0f);
        {
            const size_t P = size_t(1.5 * FR);
            detail_vox(voc, P, FR);
        }
        const double vocRms = rmsOf(voc, 0, 2 * size_t(1.5 * FR));
        std::printf("INFO vocal-like phrase: 1.5 s, RMS %.2f dBFS\n", db(vocRms));
        // real vocal phrase (owner's source, read-only) if present: printed only
        Buf real; bool haveReal = false;
        { Wav w; if (readWav("../_kaynak/OG_vokal_20s.wav", w) && w.rate == 44100) { const size_t a0 = size_t(5.78 * 44100), P = size_t(1.5 * 44100);
            if ((a0 + P) * 2 <= w.data.size()) { real.assign(size_t(2.7 * 44100) * 2, 0.0f); for (size_t i = 0; i < P; ++i) { double g = std::min(1.0, std::min(double(i), double(P - 1 - i)) / (0.005 * 44100)); real[2 * i] = float(w.data[2 * (a0 + i)] * g); real[2 * i + 1] = float(w.data[2 * (a0 + i) + 1] * g); } haveReal = true; } } }
        std::vector<std::pair<std::string, yay::Controls>> cfg;
        for (const char* pn : kPresetNames) cfg.push_back({pn, preset(pn)});
        { yay::Controls q; q.air = 1; q.sheen = 1; cfg.push_back({"AIR 1 SHEEN 1", q}); q.masterSafe = true; cfg.push_back({"AIR 1 SHEEN 1 SAFE", q}); }
        std::printf("TABLE GLUE-NOT-A-ROOM: setting | wet onset | impulse wet 300-320 ms re first 20 ms of wet | vocal wet 300-600 ms after phrase re phrase | real vocal\n");
        for (auto& [nm, q] : cfg) {
            Buf imp(size_t(FR) * 2, 0.0f); const size_t hit = size_t(0.1 * FR); imp[2 * hit] = imp[2 * hit + 1] = 0.5f;
            const Buf oi = renderC(imp, FR, q);
            auto eWin = [&](const Buf& o, const Buf& in, double t0, double t1, int rate) {
                double e = 0; for (size_t i = size_t(t0 * rate); i < size_t(t1 * rate) && 2 * i + 1 < o.size(); ++i) { const double a = double(o[2 * i]) - in[2 * i], b = double(o[2 * i + 1]) - in[2 * i + 1]; e += a * a + b * b; }
                return e;
            };
            double pkw = 0; for (size_t i = hit; i < oi.size() / 2; ++i) pkw = std::max(pkw, std::fabs(double(oi[2 * i]) - imp[2 * i]) + std::fabs(double(oi[2 * i + 1]) - imp[2 * i + 1]));
            size_t on = hit; while (on < oi.size() / 2 && std::fabs(double(oi[2 * on]) - imp[2 * on]) + std::fabs(double(oi[2 * on + 1]) - imp[2 * on + 1]) < 0.01 * pkw) ++on;
            const double t0 = double(on) / FR;
            const double e0 = eWin(oi, imp, t0, t0 + 0.02, FR), e300 = eWin(oi, imp, 0.1 + 0.3, 0.1 + 0.32, FR);
            const double impDb = 10 * std::log10(std::max(1e-300, e300) / e0);
            const Buf ov = renderC(voc, FR, q);
            const double tailRms = std::sqrt(eWin(ov, voc, 1.8, 2.1, FR) / (2.0 * 0.3 * FR));
            const double vocDb = 20 * std::log10(std::max(1e-30, tailRms) / vocRms);
            double realDb = 0;
            if (haveReal) { const Buf orl = renderC(real, 44100, q); const double tr = std::sqrt(eWin(orl, real, 1.8, 2.1, 44100) / (2.0 * 0.3 * 44100)); realDb = 20 * std::log10(std::max(1e-30, tr) / rmsOf(real, 0, 2 * size_t(1.5 * 44100))); }
            std::printf("TABLE   %-20s | %5.1f ms | %8.1f dB | %8.1f dB | %s\n", nm.c_str(), 1000.0 * (t0 - 0.1), impDb, vocDb, haveReal ? (std::to_string(int(std::lround(realDb))) + " dB").c_str() : "n/a");
            const bool gated = nm == "SHEEN ONLY" || nm.rfind("AIR 1 SHEEN 1", 0) == 0;
            if (gated) {
                std::snprintf(s, sizeof s, "%s: impulse tail 300 ms %.1f dB re first 20 ms of the wet (need <= -25)", nm.c_str(), impDb);
                CK(impDb <= -25.0, "glue_not_a_room_" + nm, s);
            }
        }
    }
    // (e) chirp: instantaneous frequency falls over time in the first echo, TENSION 0.5 and 1
    for (float ten : {0.5f, 1.0f}) {
        yay::Controls q = c; q.kick = 0; q.tension = ten; q.drip = 0;
        auto ir = wetIR(q, R, 0.25);
        double mx = 0; for (double v : ir) mx = std::max(mx, std::fabs(v));
        size_t t0 = 0; while (t0 < ir.size() && std::fabs(ir[t0]) < 0.05 * mx) ++t0;
        const size_t a = t0 > 44 ? t0 - 44 : 0, b = t0 + size_t(0.030 * R);
        const int F = 256, H = 16;
        std::vector<double> tt, cc, ww;
        for (size_t p = a; p + F <= b + F / 2; p += H) {
            Cplx f(F, 0.0);
            double en = 0;
            for (int i = 0; i < F; ++i) { const double v = (p + i < ir.size() ? ir[p + i] : 0) * hann(size_t(i), F); f[i] = v; en += v * v; }
            fft(f);
            double num = 0, den = 0;
            for (int k = 1; k < F / 2; ++k) { const double hz = double(k) * R / F; if (hz < 200 || hz > 8000) continue; const double m = std::norm(f[k]); num += hz * m; den += m; }
            tt.push_back(1000.0 * (double(p) + F / 2 - double(t0)) / R); cc.push_back(num / std::max(1e-300, den)); ww.push_back(en);
        }
        const double wm = *std::max_element(ww.begin(), ww.end());
        double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (size_t i = 0; i < tt.size(); ++i) { if (ww[i] < 1e-3 * wm) continue; const double w = ww[i]; sw += w; sx += w * tt[i]; sy += w * cc[i]; sxx += w * tt[i] * tt[i]; sxy += w * tt[i] * cc[i]; }
        const double slope = (sw * sxy - sx * sy) / (sw * sxx - sx * sx);
        auto bandArrival = [&](double lo, double hi) {
            double num = 0, den = 0;
            for (size_t p = a; p + F <= b + F / 2; p += H) {
                Cplx f(F, 0.0);
                for (int i = 0; i < F; ++i) f[i] = (p + i < ir.size() ? ir[p + i] : 0) * hann(size_t(i), F);
                fft(f);
                double e = 0; for (int k = 1; k < F / 2; ++k) { const double hz = double(k) * R / F; if (hz >= lo && hz < hi) e += std::norm(f[k]); }
                const double t = 1000.0 * (double(p) + F / 2 - double(t0)) / R; num += t * e; den += e;
            }
            return num / std::max(1e-300, den);
        };
        const double tHi = bandArrival(3000, 5000), tLo = bandArrival(300, 700);
        std::snprintf(s, sizeof s, "TENSION %.1f slope=%.1f Hz/ms  arrival 3-5k=%.1f ms 300-700=%.1f ms  sweep=%.1f ms (need slope<0, sweep 10-40 ms)", ten, slope, tHi, tLo, tLo - tHi);
        CK(slope < 0 && (tLo - tHi) >= 10 && (tLo - tHi) <= 40, std::string("chirp_falls_tension_") + (ten < 0.9f ? "0.5" : "1.0"), s);
    }
    // (h) reference match vs Cemalim. Excitation = 8 noise bursts (55 ms, 0.6 s apart) whose spectrum is
    //     Cemalim's measured mean ATTACK spectrum, so the device gets the same spectral input as the
    //     reference and the tail it makes can be compared with the reference tail. 5-60 ms attack /
    //     60-300 ms tail windows after each onset, same definitions as tools/measure_reference.py.
    //     Gated: wet-only tail centroid and twang peak within +-25 % of Cemalim.
    //     Printed: the same for the pluck melody (source-dominated), mix values, tilt and 1.2-4 kHz hump.
    {
        struct Res { double cen, tw, hump, tilt, r250, r1k, r2k, r4k; };
        auto analyse = [&](const Buf& o, const std::vector<size_t>& on) {
            std::vector<double> m(o.size() / 2);
            for (size_t i = 0; i < m.size(); ++i) m[i] = 0.5 * (double(o[2 * i]) + o[2 * i + 1]);
            const size_t N = 16384;
            std::vector<double> T(N / 2 + 1, 0.0), A(N / 2 + 1, 0.0);
            for (size_t o0 : on) {
                auto p = pspec(m, o0 + size_t(0.060 * R), o0 + size_t(0.300 * R), N);
                auto q = pspec(m, o0 + size_t(0.005 * R), o0 + size_t(0.060 * R), N);
                for (size_t k = 0; k < T.size(); ++k) { T[k] += p[k] / (0.240 * R); A[k] += q[k] / (0.055 * R); }
            }
            Res r{};
            double num = 0, den = 0;
            for (size_t k = 0; k < T.size(); ++k) { const double hz = double(k) * R / N; if (hz < 100 || hz > 10000) continue; const double mg = std::sqrt(T[k]); num += hz * mg; den += mg; }
            r.cen = num / den;
            auto S = smoothOct(T, N, R), SA = smoothOct(A, N, R);
            double best = -1, bh = -1, sx = 0, sy = 0, sxx = 0, sxy = 0; int cnt = 0;
            for (size_t k = 1; k < S.size(); ++k) {
                const double hz = double(k) * R / N;
                if (hz >= 800 && hz <= 4000 && S[k] > best) { best = S[k]; r.tw = hz; }
                if (hz >= 1200 && hz <= 4000 && S[k] > bh) { bh = S[k]; r.hump = hz; }
                if (hz >= 200 && hz <= 8000) { const double x = std::log2(hz), y = 10 * std::log10(S[k] / SA[k]); sx += x; sy += y; sxx += x * x; sxy += x * y; ++cnt; }
            }
            r.tilt = (cnt * sxy - sx * sy) / (cnt * sxx - sx * sx);
            auto ratioAt = [&](double hz) { const size_t k = size_t(std::lround(hz * N / R)); return 10 * std::log10(S[k] / SA[k]); };
            r.r250 = ratioAt(250); r.r1k = ratioAt(1000); r.r2k = ratioAt(2000); r.r4k = ratioAt(4000);
            return r;
        };
        // source A: Cemalim-attack-shaped bursts
        std::vector<float> bm(size_t(5.4 * R), 0.0f);
        std::vector<size_t> bOn;
        {
            const size_t NB = 4096;
            std::mt19937 g(1974);
            std::uniform_real_distribution<double> ph(0.0, 2 * 3.141592653589793);
            for (int b = 0; b < 8; ++b) {
                Cplx X(NB, 0.0);
                for (size_t k = 1; k < NB / 2; ++k) {
                    const double hz = double(k) * R / NB;
                    const double pos = std::log2(std::max(hz, 1.0) / 100.0) * 3.0;  // band index
                    double dB;
                    if (pos <= 0) dB = kCemalimAttackDb[0] + 12.0 * pos / 3.0;       // -12 dB/oct below 100 Hz
                    else if (pos >= 20) dB = kCemalimAttackDb[20] - 12.0 * (pos - 20) / 3.0;
                    else { const int i = int(pos); const double f = pos - i; dB = kCemalimAttackDb[i] * (1 - f) + kCemalimAttackDb[i + 1] * f; }
                    const double mag = std::pow(10.0, dB / 20.0), p = ph(g);
                    X[k] = std::polar(mag, p);
                    X[NB - k] = std::conj(X[k]);
                }
                for (auto& v : X) v = std::conj(v);
                fft(X);  // inverse via conjugation (scale irrelevant, normalised below)
                const size_t at = size_t(0.2 * R) + size_t(b) * size_t(0.6 * R);
                bOn.push_back(at);
                const size_t len = size_t(0.055 * R);
                for (size_t i = 0; i < len; ++i) {
                    const double env = std::min(1.0, double(i) / (0.001 * R)) * std::exp(-double(i) / (0.015 * R)) * std::min(1.0, double(len - i) / (0.003 * R));
                    bm[at + i] = float(X[i].real() * env);
                }
            }
            float pk = 0; for (float v : bm) pk = std::max(pk, std::fabs(v));
            for (float& v : bm) v *= 0.5f / pk;
        }
        // source B: the pluck melody used in the demos
        std::vector<float> pm(size_t(6.5 * R), 0.0f);
        auto pOn = pluck::melody(pm, size_t(0.2 * R), R);
        auto toSt = [](const std::vector<float>& mo) { Buf st(mo.size() * 2); for (size_t i = 0; i < mo.size(); ++i) st[2 * i] = st[2 * i + 1] = mo[i]; return st; };
        const Buf bst = toSt(bm), pst = toSt(pm);
        const Res pd = analyse(pst, pOn);
        std::printf("INFO dry pluck melody alone: tail centroid %.0f Hz, twang %.0f Hz, hump %.0f Hz, tilt %+.2f dB/oct\n", pd.cen, pd.tw, pd.hump, pd.tilt);
        const char* bestName = ""; double bestScore = 1e30;
        const char* cands[1] = {"SHEEN (Cemalim voicing)"};
        for (const char* pn : cands) {
            yay::Controls q = preset("SHEEN ONLY");
            q.sheen = 1.0f;
            const Res w = analyse(renderWet(bst, R, q), bOn), wp = analyse(renderWet(pst, R, q), pOn);
            q.sheen = 0.5f;
            const Res mx = analyse(renderBuf(bst, R, q), bOn);
            const double ec = 100.0 * (w.cen - kRefCentroidHz) / kRefCentroidHz, et = 100.0 * (w.tw - kRefTwangHz) / kRefTwangHz;
            std::snprintf(s, sizeof s, "%s wet tail centroid=%.0f Hz vs Cemalim %.0f Hz (%+.1f %%), twang peak=%.0f Hz vs %.0f Hz (%+.1f %%) (need both +-25 %%)",
                          pn, w.cen, kRefCentroidHz, ec, w.tw, kRefTwangHz, et);
            CK(std::fabs(ec) <= 25.0 && std::fabs(et) <= 25.0, std::string("ref_match_cemalim_") + pn, s);
            // match score: mean |% error| of centroid, twang, hump, tank delay (Cemalim 29.4 ms), tilt (dB/oct as %)
            const double tank = 29.4;  // short lanes (the only size now)
            const double sc = (std::fabs(ec) + std::fabs(et) + 100.0 * std::fabs(w.hump - kRefHumpHz) / kRefHumpHz
                               + 100.0 * std::fabs(tank - 29.44) / 29.44 + 100.0 * std::fabs(mx.tilt - kRefTiltDbOct) / std::fabs(kRefTiltDbOct)) / 5.0;
            if (sc < bestScore) { bestScore = sc; bestName = pn; }
            std::printf("INFO %s Cemalim match score (mean |%% err| of centroid, twang, hump, tank delay, tilt) = %.1f\n", pn, sc);
            std::printf("INFO %s: hump 1.2-4k wet %.0f Hz vs Cemalim %.0f Hz; mix(SHEEN .5) centroid %.0f Hz twang %.0f Hz tilt %+.2f dB/oct vs Cemalim %+.2f (tail/attack @250/1k/2k/4k %+.1f/%+.1f/%+.1f/%+.1f dB, Cemalim -2.9/-0.7/-3.0/-8.8); "
                        "pluck-driven wet: centroid %.0f Hz twang %.0f Hz hump %.0f Hz\n",
                        pn, w.hump, kRefHumpHz, mx.cen, mx.tw, mx.tilt, kRefTiltDbOct, mx.r250, mx.r1k, mx.r2k, mx.r4k, wp.cen, wp.tw, wp.hump);
        }
        std::printf("INFO best Cemalim match: %s (score %.1f)\n", bestName, bestScore);
    }

    // ================= AIR, MASTER SAFE =================
    {
        const int FR = 48000;
        // AIR: +3 dB at 11 kHz at AIR 0.5 (SHEEN 0, MASTER SAFE off), wash included
        {
            auto gainAt = [&](double f, float airv, bool wash) {
                yay::Controls q; q.air = airv;
                const Buf in = sineSt(FR, f, 0.25, 1.6);
                gEngine.setAirWashEnabled(wash);
                const Buf o = renderBuf(in, FR, q);
                gEngine.setAirWashEnabled(true);
                return db(rmsOf(o, 2 * size_t(0.5 * FR), 2 * size_t(1.5 * FR)) / rmsOf(in, 2 * size_t(0.5 * FR), 2 * size_t(1.5 * FR)));
            };
            const double g11 = gainAt(11000, 0.5f, true), g11s = gainAt(11000, 0.5f, false), g1k = gainAt(1000, 0.5f, true);
            std::printf("INFO AIR response (SHEEN 0), gain re dry: ");
            for (float av : {0.25f, 0.5f, 1.0f}) std::printf("AIR %.2f: 1k %+.2f 5k %+.2f 8k %+.2f 11k %+.2f 16k %+.2f dB; ", av, gainAt(1000, av, true), gainAt(5000, av, true), gainAt(8000, av, true), gainAt(11000, av, true), gainAt(16000, av, true));
            std::printf("\n");
            std::snprintf(s, sizeof s, "AIR 0.5: 11 kHz %+.2f dB (shelf only %+.2f), 1 kHz %+.2f dB (need 11k +2.5..+3.5)", g11, g11s, g1k);
            CK(g11 >= 2.5 && g11 <= 3.5, "air_11k_at_0.5", s);
        }
        // AIR wash tail T60 (IR of wash on - wash off, AIR 0.7, SHEEN 0)
        {
            yay::Controls q; q.air = 0.7f;
            Buf imp(size_t(FR) * 2, 0.0f); imp[0] = imp[1] = 1.0f;
            const Buf on = renderBuf(imp, FR, q);
            gEngine.setAirWashEnabled(false);
            const Buf off = renderBuf(imp, FR, q);
            gEngine.setAirWashEnabled(true);
            const size_t n = on.size() / 2;
            std::vector<double> edc(n);
            double acc = 0;
            for (size_t i = n; i-- > 0;) { const double w = 0.5 * ((double(on[2 * i]) - off[2 * i]) + (double(on[2 * i + 1]) - off[2 * i + 1])); acc += w * w; edc[i] = acc; }
            double sx = 0, sy = 0, sxx = 0, sxy = 0; int m = 0;
            for (size_t i = 0; i < n; ++i) {
                const double d = 10.0 * std::log10(std::max(1e-300, edc[i] / edc[0]));
                if (d <= -5.0 && d >= -35.0) { const double t = double(i) / FR; sx += t; sy += d; sxx += t * t; sxy += t * d; ++m; }
            }
            const double t60 = -60.0 / ((m * sxy - sx * sy) / (m * sxx - sx * sx));
            std::snprintf(s, sizeof s, "AIR 0.7 wash T60 = %.3f s (need < 0.3)", t60);
            CK(t60 > 0 && t60 < 0.3, "air_wash_t60", s);
        }
        // MASTER SAFE auto-gain: K-weighted loudness out vs in, pink noise and a music-like mix
        const Buf pink = pinkSt(FR, 10.0, -16.0, 21), mus = musicSt(FR, 10.0);
        std::printf("INFO test signals: pink RMS %.2f dBFS; music-like RMS %.2f dBFS peak -1.00 dBFS\n", db(rmsOf(pink)), db(rmsOf(mus)));
        for (int sg = 0; sg < 2; ++sg) {
            const Buf& src = sg ? mus : pink;
            for (int k = 0; k < 3; ++k) {
                yay::Controls q = preset(k == 0 ? "MASTER AIR" : k == 1 ? "AIR FULL" : "AIR + SHEEN"); q.masterSafe = true;
                gAgMin = 1e9; gAgMax = -1e9;
                const double lu = kLU(renderC(src, FR, q), src, FR);
                std::snprintf(s, sizeof s, "%s %s + MASTER SAFE: out-in = %+.3f LU, auto-gain %+.2f..%+.2f dB after 1 s (need +-0.5 LU)", sg ? "music" : "pink", kPresetNames[k == 0 ? 4 : k == 1 ? 1 : 2], lu, gAgMin, gAgMax);
                char nmx[80]; std::snprintf(nmx, sizeof nmx, "safe_autogain_%s_%s", sg ? "music" : "pink", k == 0 ? "master_air" : k == 1 ? "air_full" : "air_sheen");
                CK(std::fabs(lu) <= 0.5, nmx, s);
            }
        }
        // MASTER SAFE ceiling: song-like signal peaking at -0.1 dBFS stays <= -0.3 dBTP (independent 4x measurement)
        {
            Buf hot = mus;
            { float pk = 0; for (float v : hot) pk = std::max(pk, std::fabs(v)); for (float& v : hot) v *= float(std::pow(10.0, -0.1 / 20.0) / pk); }
            std::printf("INFO ceiling test input: sample peak -0.10 dBFS, true peak %.2f dBTP\n", truePeakDb(hot));
            for (int k = 0; k < 2; ++k) {
                yay::Controls q = k == 0 ? preset("MASTER AIR") : yay::Controls{}; if (k == 1) { q.air = 1; q.sheen = 1; q.masterSafe = true; }
                const Buf o = renderC(hot, FR, q);
                const double tp = truePeakDb(o), lu = kLU(o, hot, FR);
                std::snprintf(s, sizeof s, "%s: out true peak %.2f dBTP (need <= -0.3), out-in %+.3f LU (need +-0.5)", k == 0 ? "MASTER AIR" : "AIR 1 SHEEN 1 SAFE", tp, lu);
                CK(tp <= -0.3 && std::fabs(lu) <= 0.5, std::string("safe_ceiling_") + (k == 0 ? "master_air" : "air1_tank1"), s);
            }
            // without MASTER SAFE the same material is not limited (printed, for contrast)
            yay::Controls q = preset("MASTER AIR"); q.masterSafe = false;
            std::printf("INFO same input, MASTER AIR with MASTER SAFE off: out true peak %.2f dBTP (no ceiling)\n", truePeakDb(renderBuf(hot, FR, q)));
        }
        // MASTER SAFE: SIDE passes clean
        for (int k = 0; k < 2; ++k) {
            yay::Controls q = preset("MASTER AIR");
            if (k == 0) { q.air = 1; q.sheen = 1; }
            const Buf o = renderC(mus, FR, q);
            double se = 0, sr2 = 0;
            for (size_t i = 0; i < o.size() / 2; ++i) {
                const double si = 0.5 * (double(mus[2 * i]) - mus[2 * i + 1]), so = 0.5 * (double(o[2 * i]) - o[2 * i + 1]);
                se += si * si; sr2 += (so - si) * (so - si);
            }
            const double rr = 10 * std::log10(std::max(1e-30, sr2) / se);
            std::snprintf(s, sizeof s, "%s: side residual = %.1f dB re input side (need < -80)", k ? "MASTER AIR" : "AIR 1 SHEEN 1 SAFE", rr);
            CK(rr < -80.0, std::string("safe_side_clean_") + (k ? "master_air" : "worst"), s);
        }
        // residual vs dry of each preset on the music-like signal (printed)
        std::printf("INFO residual (out-dry re dry) on the music-like signal:");
        for (const char* pn : kPresetNames) {
            const Buf o = renderC(mus, FR, preset(pn));
            double sw = 0, sd = 0;
            for (size_t i = 0; i < o.size(); ++i) { const double d = double(o[i]) - mus[i]; sw += d * d; sd += double(mus[i]) * mus[i]; }
            std::printf(" %s %+.2f dB;", pn, 10 * std::log10(sw / sd));
        }
        std::printf("\n");

        // legacy AMFI preamp (KICK, not on the panel): printed only
        {
            const Buf s6 = sineSt(FR, 1000.0, 0.5, 1.6);
            std::printf("LEGACY INSTRUMENT preamp (KICK), 1 kHz sine -6 dBFS peak, SHEEN 0 AIR 0: KICK THD%% H2dBc H3dBc:");
            for (float kk : {0.1f, 0.3f, 0.5f, 1.0f}) {
                yay::Controls q; q.kick = kk;
                auto h = harmonics(renderBuf(s6, FR, q), FR, 1000.0, 40.0);
                std::printf(" %.1f: %.3f %.1f %.1f;", kk, thdPct(h), 10 * std::log10(h[2] / h[1]), 10 * std::log10(h[3] / h[1]));
            }
            const Buf sNom = sineSt(FR, 1000.0, std::pow(10.0, -18.0 / 20.0) * std::sqrt(2.0), 1.6);
            std::printf("\nLEGACY MASTER SAFE saturator (KICK), 1 kHz -18 dBFS RMS: KICK THD%% H2dBc H3dBc:");
            for (float kk : {0.1f, 0.5f, 1.0f}) {
                yay::Controls q; q.kick = kk; q.masterSafe = true;
                auto h = harmonics(renderC(sNom, FR, q), FR, 1000.0, 40.0);
                std::printf(" %.1f: %.3f %.1f %.1f;", kk, thdPct(h), 10 * std::log10(h[2] / h[1]), 10 * std::log10(h[3] / h[1]));
            }
            yay::Controls q; q.kick = 1.0f; q.sheen = 1.0f;
            float cp = 0;
            auto h = harmonics(renderBuf(sineSt(FR, 1000.0, 1.0, 1.6), FR, q, true, &cp), FR, 1000.0, 40.0);
            std::printf("\nLEGACY KICK 1 into the tank (SHEEN 1): THD %.1f %%, clank env peak %.3f\n", thdPct(h), cp);
        }
    }

    // stability: everything at max, 60 s noise, MASTER SAFE off / on
    Buf noise(size_t(48000) * 60 * 2);
    { std::mt19937 g(7); std::uniform_real_distribution<float> u(-.5f, .5f); for (float& v : noise) v = u(g); }
    for (int sf : {0, 1}) {
        yay::Controls mx; mx.air = 1; mx.sheen = 1; mx.tension = 1; mx.drip = 1; mx.twang = 1; mx.lowcut = 20; mx.masterSafe = sf != 0;
        auto no = renderBuf(noise, 48000, mx);
        float pk = 0; for (float v : no) pk = std::max(pk, std::fabs(v));
        std::snprintf(s, sizeof s, "60 s noise, all controls max, peak=%.2f dBFS", db(pk));
        CK(finiteBelow(no), std::string("stability_all_max") + (sf ? "_safe" : ""), s);
    }
    // CPU: AIR + SHEEN (heaviest), MASTER SAFE off / on
    for (int sf : {0, 1}) {
        yay::Controls q = preset("AIR + SHEEN"); q.masterSafe = sf != 0;
        auto t0 = std::chrono::steady_clock::now();
        auto o = renderBuf(noise, 48000, q);
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::snprintf(s, sizeof s, "%.3f s for 60 s stereo @48k, AIR + SHEEN (<3 s)", sec);
        CK(sec < 3.0 && finiteBelow(o), std::string("cpu_60s") + (sf ? "_safe" : ""), s);
    }
    // artefacts: finite, max sample jump and wet 2nd-difference crest on a 0.3 1 kHz sine, every preset
    for (const char* pn : kPresetNames) {
        yay::Controls q = preset(pn);
        auto z = renderC(input, 48000, q);
        float jump = 0; for (size_t i = 2; i < z.size(); i += 2) jump = std::max(jump, std::fabs(z[i] - z[i - 2]));
        std::vector<double> w(z.size() / 2);
        for (size_t i = 0; i < w.size(); ++i) w[i] = double(z[2 * i]) - input[2 * i];
        double pk = 0, ss = 0; size_t cnt = 0;
        for (size_t i = 48000 / 2; i + 1 < w.size(); ++i) { const double d2 = w[i + 1] - 2 * w[i] + w[i - 1]; pk = std::max(pk, std::fabs(d2)); ss += d2 * d2; ++cnt; }
        const double crest = pk / std::sqrt(ss / cnt);
        float zp = 0; for (float v : z) zp = std::max(zp, std::fabs(v));
        std::snprintf(s, sizeof s, "peak=%.2f dBFS jump=%.4f (<0.8) wet_d2_crest=%.2f (<6)", db(zp), jump, crest);
        CK(finiteBelow(z) && jump < 0.8f && crest < 6.0, std::string("artefact_free_") + pn, s);
    }
    std::cout << (fails ? "FAIL" : "ALL PASS") << " (" << checks << " checks)\n";
    return fails ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc >= 2 && !std::strcmp(argv[1], "--selftest")) return selftest();
    if (argc < 3) { std::cerr << "usage: render in.wav out.wav [--preset P] [--param k=v]\n"; return 2; }
    Wav w;
    if (!readWav(argv[1], w)) { std::cerr << "cannot read input\n"; return 2; }
    yay::Controls c;
    for (int i = 3; i < argc; i++) {
        if (!std::strcmp(argv[i], "--preset") && i + 1 < argc) c = preset(argv[++i]);
        else if (!std::strncmp(argv[i], "--param", 7) && i + 1 < argc) {
            const char* p = argv[++i]; char k[32]; float v;
            if (std::sscanf(p, "%31[^=]=%f", k, &v) == 2) {
                if (!std::strcmp(k, "air")) c.air = v; else if (!std::strcmp(k, "sheen")) c.sheen = v;
                else if (!std::strcmp(k, "output")) c.output = v; else if (!std::strcmp(k, "bypass")) c.bypass = v != 0;
                else if (!std::strcmp(k, "tension")) c.tension = v;
                else if (!std::strcmp(k, "drip")) c.drip = v; else if (!std::strcmp(k, "twang")) c.twang = v;
                else if (!std::strcmp(k, "lowcut")) c.lowcut = v; else if (!std::strcmp(k, "safe")) c.masterSafe = v != 0;
                else if (!std::strcmp(k, "kick")) c.kick = v; else if (!std::strcmp(k, "wash")) gEngine.setAirWashEnabled(v != 0);  // analysis only: AIR wash on/off else if (!std::strcmp(k, "presence")) c.presence = v;
            }
        }
    }
    w.data = renderC(w.data, w.rate, c);  // latency-compensated file render
    return writeWav24(argv[2], w.rate, w.data) ? 0 : 3;
}
