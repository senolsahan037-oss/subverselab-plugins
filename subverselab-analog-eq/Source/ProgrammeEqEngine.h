// Passive broadcast programme EQ, 1974 (SPEC.md section 2b). Header-only, no JUCE: the plug-in and tools/eqraw.cpp run
// exactly this code.
//
// Path per channel: passive network (linear, session rate) -> x2 oversampling -> make-up amplifier (DRIVE) -> output
// transformer -> /2 -> DC blocker -> OUTPUT. Latency: kLatency samples at the session rate (the half-band FIR pair).
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace peq
{
constexpr double kPi = 3.14159265358979323846;

constexpr std::array<float, 4> kLowFreqs { 40.0f, 60.0f, 100.0f, 150.0f };
constexpr std::array<float, 5> kPresFreqs { 700.0f, 1400.0f, 2800.0f, 4000.0f, 5600.0f };
constexpr std::array<float, 3> kHighFreqs { 5000.0f, 8000.0f, 12000.0f };
// The cut shelf sits 2.5x above the boost corner and is broader: with both up the low end lifts and the octave above
// dips (the passive boost/cut trick). Shapes chosen by a search on the prototype (STATUS 26 Oct): boost +10 / cut 6 at
// 60 Hz gives about +5 dB below 50 Hz and -2.5 dB between 100 and 250 Hz.
constexpr float kLowCutRatio = 2.5f;
constexpr float kLowBoostQ = 0.8f, kLowCutQ = 0.5f, kHighQ = 0.5f;
constexpr float kLowCutMaxDb = 8.0f;   // the passive attenuator is shallower than the boost
inline float presenceQ (float gainDb) { return 0.55f + 0.02f * std::fabs (gainDb); }

constexpr int kTaps = 127;                // half-band FIR, linear phase, run polyphase (only the non-zero taps)
constexpr int kMid = (kTaps - 1) / 2;     // 63, odd: the even taps carry the filter, the odd ones are zero but the centre
constexpr int kEven = (kTaps + 1) / 2;    // 64 even taps
static_assert (kEven % 4 == 0, "HalfBand2x::dot sums in fours");
constexpr int kLatency = kMid;            // up + down = 126 samples at 2x = 63 at the session rate

struct Params
{
    int lowFreq = 1;          // index into kLowFreqs
    float lowBoostDb = 0.0f;  // 0 .. 12
    float lowCutDb = 0.0f;    // 0 .. kLowCutMaxDb (attenuation)
    int presFreq = 2;         // index into kPresFreqs
    float presDb = 0.0f;      // -8 .. 8
    int highFreq = 1;         // index into kHighFreqs
    float highDb = 0.0f;      // -10 .. 10
    float driveDb = 0.0f;     // 0 .. 18
    float outputDb = 0.0f;    // -18 .. 18
    bool in = true;           // false = the dry input, delayed by the latency so a switch does not jump in time
};

// RBJ biquads (bilinear transform with pre-warp), Direct Form II transposed, double state.
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void setNorm (double B0, double B1, double B2, double A0, double A1, double A2)
    { b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0; }
    void lowShelf (double fs, double f, double q, double gainDb)
    {
        const double A = std::pow (10.0, gainDb / 40.0), w = 2 * kPi * f / fs, c = std::cos (w), al = std::sin (w) / (2 * q), s = 2 * std::sqrt (A) * al;
        setNorm (A * ((A + 1) - (A - 1) * c + s), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - s),
                 (A + 1) + (A - 1) * c + s, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - s);
    }
    void highShelf (double fs, double f, double q, double gainDb)
    {
        const double A = std::pow (10.0, gainDb / 40.0), w = 2 * kPi * f / fs, c = std::cos (w), al = std::sin (w) / (2 * q), s = 2 * std::sqrt (A) * al;
        setNorm (A * ((A + 1) + (A - 1) * c + s), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - s),
                 (A + 1) - (A - 1) * c + s, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - s);
    }
    void peak (double fs, double f, double q, double gainDb)
    {
        const double A = std::pow (10.0, gainDb / 40.0), w = 2 * kPi * f / fs, c = std::cos (w), al = std::sin (w) / (2 * q);
        setNorm (1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
    }
    double run (double x)
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() { z1 = z2 = 0; }
};

// Linear-phase half-band low-pass for x2 up/down sampling (windowed sinc, Kaiser beta 8, cutoff at a quarter of the 2x rate).
inline std::array<double, kTaps> halfBand()
{
    auto i0 = [] (double x) { double s = 1, t = 1; for (int k = 1; k < 30; ++k) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; };
    std::array<double, kTaps> h {};
    const double beta = 8.0, mid = (kTaps - 1) / 2.0;
    double sum = 0;
    for (int n = 0; n < kTaps; ++n)
    {
        const double m = n - mid, sinc = m == 0 ? 0.5 : std::sin (kPi * 0.5 * m) / (kPi * m);
        const double r = m / mid, win = i0 (beta * std::sqrt (std::max (0.0, 1 - r * r))) / i0 (beta);
        h[(size_t) n] = sinc * win; sum += h[(size_t) n];
    }
    for (auto& v : h) v /= sum;   // unity DC gain
    return h;
}

// x2 up and down with the half-band, polyphase. Up: even output = 2 * sum(h[2i] x[m-i]), odd output = x[m - (kMid-1)/2]
// (the centre tap is 0.5). Down keeps the even outputs: z[m] = sum(h[2i] e[m-i]) + 0.5 * o[m - (kMid+1)/2].
struct HalfBand2x
{
    std::array<float, kEven> he {};   // float: the half-band's own noise floor is far below -120 dB
    std::array<float, 2 * kEven> xin {}, ev {}, od {};   // doubled ring buffers: a contiguous window, no wrap in the loop
    int pos = 0;
    void init (const std::array<double, kTaps>& h) { for (int i = 0; i < kEven; ++i) he[(size_t) i] = (float) h[(size_t) (2 * i)]; reset(); }
    void reset() { xin.fill (0); ev.fill (0); od.fill (0); pos = 0; }
    static void push (std::array<float, 2 * kEven>& b, int p, double v) { b[(size_t) p] = (float) v; b[(size_t) (p + kEven)] = (float) v; }
    // newest sample at index pos + kEven, older ones below it
    double dot (const std::array<float, 2 * kEven>& b) const
    {
        // four running sums break the add chain (kEven is a multiple of 4)
        const float* x = b.data() + pos + kEven;
        float y0 = 0, y1 = 0, y2 = 0, y3 = 0;
        for (int i = 0; i < kEven; i += 4)
        {
            y0 += he[(size_t) i] * x[-i]; y1 += he[(size_t) i + 1] * x[-i - 1];
            y2 += he[(size_t) i + 2] * x[-i - 2]; y3 += he[(size_t) i + 3] * x[-i - 3];
        }
        return (y0 + y1) + (y2 + y3);
    }
    void up (double x, double& even, double& odd)
    {
        pos = pos + 1 == kEven ? 0 : pos + 1;
        push (xin, pos, x);
        even = 2 * dot (xin);
        odd = xin[(size_t) (pos + kEven - (kMid - 1) / 2)];
    }
    double down (double e, double o)
    {
        push (ev, pos, e); push (od, pos, o);
        return dot (ev) + 0.5 * od[(size_t) (pos + kEven - (kMid + 1) / 2)];
    }
};

class Channel
{
public:
    void prepare (double sampleRate)
    {
        fs = sampleRate;
        const auto h = halfBand();
        hb.init (h);
        dryDelay.assign ((size_t) kLatency + 1, 0.0);
        // transformer flux: one-pole low-pass at 30 Hz, at the 2x rate
        fluxA = 1.0 - std::exp (-2 * kPi * 30.0 / (2 * fs));
        // DC blocker at the session rate, 3 Hz
        dcR = std::exp (-2 * kPi * 3.0 / fs);
        reset();
    }
    void reset()
    {
        lowB.reset(); lowC.reset(); pres.reset(); high.reset(); hb.reset();
        std::fill (dryDelay.begin(), dryDelay.end(), 0.0); dPos = 0; flux = 0; dcX = dcY = 0; prevX = 0.0625; prevR = satInt (prevX) - 0.5 * satSlope (prevX) * prevX * prevX;
    }
    void setParams (const Params& p)
    {
        const auto clampi = [] (int v, int hi) { return v < 0 ? 0 : v > hi ? hi : v; };
        const float fl = kLowFreqs[(size_t) clampi (p.lowFreq, (int) kLowFreqs.size() - 1)];
        lowB.lowShelf (fs, fl, kLowBoostQ, p.lowBoostDb);
        lowC.lowShelf (fs, fl * kLowCutRatio, kLowCutQ, -std::min (p.lowCutDb, kLowCutMaxDb));
        pres.peak (fs, kPresFreqs[(size_t) clampi (p.presFreq, (int) kPresFreqs.size() - 1)], presenceQ (p.presDb), p.presDb);
        high.highShelf (fs, kHighFreqs[(size_t) clampi (p.highFreq, (int) kHighFreqs.size() - 1)], kHighQ, p.highDb);
        drive = std::pow (10.0, p.driveDb / 20.0);
        out = std::pow (10.0, p.outputDb / 20.0);
        in = p.in;
    }
    float process (float xIn)
    {
        const double x = xIn;
        // dry path, delayed by the latency (IN off)
        dryDelay[(size_t) dPos] = x;
        const int rp = dPos + 1 == (int) dryDelay.size() ? 0 : dPos + 1;
        const double dry = dryDelay[(size_t) rp];
        dPos = rp;

        double v = high.run (pres.run (lowC.run (lowB.run (x))));
        // x2: two samples through the nonlinear stages, back down to one
        double e, o;
        hb.up (v, e, o);
        const double ye = nonlinear (e), yo = nonlinear (o);
        const double y = hb.down (ye, yo);
        // DC blocker
        const double d = y - dcX + dcR * dcY; dcX = y; dcY = d;
        return (float) ((in ? d : dry) * out);
    }

private:
    double nonlinear (double u)
    {
        // make-up amplifier: a biased soft stage, so the 2nd harmonic leads at moderate levels and the 3rd joins when
        // pushed; kHead keeps DRIVE 0 clean. The level is divided by the drive again: DRIVE colours, it does not raise.
        // The curve is sat() (a Pade tanh that lands flat at |x| = 3). Its linear part passes straight; only the residual
        // r(x) = sat(x) - s1 x goes through first-order antiderivative anti-aliasing (ADAA): (R(x1) - R(x0)) / (x1 - x0),
        // so pushed highs fold back far less and the clean signal keeps its top end.
        constexpr double k = 0.25, bias = 0.25, kHead = 0.5, xb = k * bias;
        static const double t0 = sat (xb), s1 = satSlope (xb), slope0 = k * s1;
        const double g = drive * kHead, xk = k * (u * g + bias);
        const double Rk = satInt (xk) - 0.5 * s1 * xk * xk, dx = xk - prevX;
        const double r = std::fabs (dx) > 1e-6 ? (Rk - prevR) / dx : sat (0.5 * (xk + prevX)) - s1 * 0.5 * (xk + prevX);
        prevX = xk; prevR = Rk;
        const double a = (s1 * xk + r - t0) / slope0 / g;
        // output transformer: flux = low-pass of the voltage; the core's error grows as the frequency falls (odd harmonics).
        // kCore scales the flux (a smaller core saturates earlier); the error is scaled back to the signal.
        constexpr double kT = 0.6, kCore = 2.5;
        flux += fluxA * (a * g * kCore - flux);
        const double err = flux - sat (flux);
        return a - kT * err / (g * kCore);
    }

    // Pade tanh, x (27 + x^2) / (27 + 9 x^2) = x/9 + 24x / (9x^2 + 27); exactly 1 with zero slope at |x| = 3, flat beyond.
    static double sat (double x)
    {
        if (x >= 3) return 1; if (x <= -3) return -1;
        return x * (27 + x * x) / (27 + 9 * x * x);
    }
    static double satSlope (double x) { const double d = 9 * x * x + 27; return 1.0 / 9 + 24 * (27 - 9 * x * x) / (d * d); }
    // antiderivative of sat: x^2/18 + (4/3) ln(9x^2 + 27); beyond |x| = 3 it grows as |x|
    static double satInt (double x)
    {
        static const double f3 = 0.5 + (4.0 / 3) * std::log (108.0);
        const double ax = std::fabs (x);
        if (ax >= 3) return f3 + (ax - 3);
        return x * x / 18 + (4.0 / 3) * std::log (9 * x * x + 27);
    }

    double fs = 48000, prevX = 0.0625, prevR = 0;   // 0.0625 = k * bias, the idle input (prevR is set in reset)
    Biquad lowB, lowC, pres, high;
    HalfBand2x hb;
    std::vector<double> dryDelay;
    int dPos = 0;
    double fluxA = 0, flux = 0, dcR = 0, dcX = 0, dcY = 0, drive = 1, out = 1;
    bool in = true;
};

class Engine
{
public:
    void prepare (double sampleRate) { for (auto& c : ch) c.prepare (sampleRate); }
    void reset() { for (auto& c : ch) c.reset(); }
    void setParams (const Params& p) { for (auto& c : ch) c.setParams (p); }
    void process (float* l, float* r, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            l[i] = ch[0].process (l[i]);
            if (r != nullptr) r[i] = ch[1].process (r[i]);
        }
    }
    static constexpr int latency() { return kLatency; }

private:
    std::array<Channel, 2> ch;
};
} // namespace peq
