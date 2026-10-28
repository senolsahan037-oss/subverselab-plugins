// The original's "REVERBERATION" section (SPEC.md 2d): input limiter -> two 512-stage bucket brigades -> output filter,
// with the filtered output fed back to the input, DEPTH setting the loop gain and the wet level, DELAY TIME the clock.
// Header-only, no JUCE: the plug-in and tools/rvraw.cpp run exactly this. Values from docs/reference/ge1100/VALUES.md;
// what is inferred rather than read is marked (I) here and in the SPEC.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <vector>

namespace gerv
{
constexpr double kPi = 3.14159265358979323846;
constexpr int kStagesPerLine = 512, kLines = 2;
constexpr int kPeriods = kStagesPerLine * kLines / 2;   // a sample moves two stages per clock period: delay = 512 / f_clock
constexpr double kDelayMin = 0.030, kDelayMax = 0.100;  // owner's manual: 30 ... 100 ms
constexpr double kRt60Max = 3.0;                        // owner's manual: reverberation time 0 ... 3 s
constexpr double kVoltsPerUnit = 2.0 * 1.4142135623730951;   // a full-scale sine is 2 V rms (as the equaliser)

// input limiter IC101 (1/2): inverting, R102 10 k in, R101 120 k || C101 470 p || back-to-back silicon diodes
constexpr double kR102 = 10e3, kR101 = 120e3, kC101 = 470e-12;
constexpr double kIs = 2.52e-9, kNVt = 1.752 * 0.025852;   // 1N4148-class diode (D101 / D102 type not printed: I)
constexpr double kInAtten = 0.0707;   // (I) the service level, 150 mV rms, sits ~10 dB under the diodes' knee

// output filter IC104
constexpr double kR128 = 47e3, kC117 = 1.8e-9, kR129 = 6.8e3;              // (1/2): gain 7.9, pole 1.88 kHz
constexpr double kR130 = 20e3, kR131 = 20e3, kC118 = 18e-9, kC119 = 1e-9;   // (2/2): Sallen-Key, f0 1.88 kHz, Q 2.1
constexpr double kPhaseShiftHz = 1000.0;   // (I) Q107's all-pass corner, not read yet
constexpr double kLoopTrim = 1.0726;       // measured 26 Oct at 48 kHz: broadband RT60 = 3.000 s at DELAY 10, DEPTH 10 (peak -1.39 dB per pass)

struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    double run (double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
    void reset() { z1 = z2 = 0; }
    // bilinear transform of (n2 s^2 + n1 s + n0) / (d2 s^2 + d1 s + d0), pre-warped at fw
    void fromAnalog (double n2, double n1, double n0, double d2, double d1, double d0, double fs, double fw)
    {
        const double w = 2 * kPi * fw, k = w / std::tan (w / (2 * fs)), k2 = k * k;
        const double A0 = d2 * k2 + d1 * k + d0;
        b0 = (n2 * k2 + n1 * k + n0) / A0; b1 = (2 * n0 - 2 * n2 * k2) / A0; b2 = (n2 * k2 - n1 * k + n0) / A0;
        a1 = (2 * d0 - 2 * d2 * k2) / A0; a2 = (d2 * k2 - d1 * k + d0) / A0;
    }
};

class Reverb
{
public:
    void prepare (double sampleRate)
    {
        fs = sampleRate;
        // IC104 (1/2): (1 + 6.91 / (1 + s tau)) = (7.91 + s tau) / (1 + s tau), tau = R128 C117
        const double tau = kR128 * kC117, g0 = 1 + kR128 / kR129;
        f1.fromAnalog (0, tau, g0, 0, tau, 1, fs, 1.0 / (2 * kPi * tau));
        // IC104 (2/2): unity-gain Sallen-Key, 1 / (s^2 R130 R131 C118 C119 + s C119 (R130 + R131) + 1)
        const double a2 = kR130 * kR131 * kC118 * kC119, a1 = kC119 * (kR130 + kR131);
        f2.fromAnalog (0, 0, 1, a2, a1, 1, fs, 1.0 / (2 * kPi * std::sqrt (a2)));
        // Q107 phase shifter: first-order all-pass (1 - s / w) / (1 + s / w)
        const double wp = 2 * kPi * kPhaseShiftHz;
        ap.fromAnalog (0, -1 / wp, 1, 0, 1 / wp, 1, fs, kPhaseShiftHz);
        // limiter's C101: trapezoidal companion at fs
        gC = 2 * kC101 * fs;
        line.assign (kPeriods, 0.0);
        calibrate();
        reset();
    }
    void reset()
    {
        f1.reset(); f2.reset(); ap.reset();
        std::fill (line.begin(), line.end(), 0.0);
        pos = 0; acc = 0; held = 0; xPrev = 0; vC = 0; iC = 0; wet = 0;
    }
    void setDelay (double knob010) { delayKnob = std::clamp (knob010, 0.0, 10.0); }
    void setDepth (double knob010) { depthKnob = std::clamp (knob010, 0.0, 10.0); }
    void setOn (bool b) { on = b; }
    // DELAY TIME: the clock astable (Q101 / Q102) runs as a linear VCO, its frequency linear in VR101's wiper voltage
    // (ngspice, 26 Oct: 2.2 kHz per volt, straight within a few % over 8 ... 20 kHz); VR101 is linear (B), so the clock
    // is linear in the knob and the delay, 512 / f, is not. The ends are the manual's 30 / 100 ms.
    double delaySeconds() const
    {
        const double fShort = kPeriods / kDelayMin, fLong = kPeriods / kDelayMax;
        return kPeriods / (fShort + (fLong - fShort) * delayKnob / 10.0);
    }
    double loopGainMax() const { return loopMax; }
    double loopPeakPerPass() const { return loopMax * chainPeak; }   // < 1 or the loop runs away
    void setLoopTrim (double t) { trim = t; calibrate(); }            // calibration only (tools/rvraw --loop-trim)

    // stereo in place: one delay line fed by the mono sum, as the original (one pair of BBDs)
    void process (float& l, float& r)
    {
        const double d = depthKnob / 10.0;
        if (! on || d <= 0) { feed (0.5 * (l + r), 0.0); return; }
        const double w = feed (0.5 * (l + r), d * loopMax);
        const double wetGain = d * wetCal;
        l = (float) (l + wetGain * w);
        r = (float) (r + wetGain * ap.run (w));
    }

private:
    // one session sample through limiter -> BBD -> filter; returns the filtered output (volts, scaled back to units)
    double feed (double inUnits, double loop)
    {
        const double x = kInAtten * inUnits * kVoltsPerUnit + loop * wet * kVoltsPerUnit;   // (I) the loop sums at R102
        const double v = limiter (x);                                                      // limiter output, volts (inverted)
        // BBD: sample v at every clock edge (linear interpolation inside the session sample), hold the output
        const double fClk = kPeriods / delaySeconds();
        acc += fClk / fs;
        while (acc >= 1.0)
        {
            acc -= 1.0;
            const double frac = 1.0 - acc / (fClk / fs);   // where in this sample the edge fell
            const double s = xPrev + (v - xPrev) * std::clamp (frac, 0.0, 1.0);
            held = line[(size_t) pos];                       // the value that entered kPeriods clock periods ago
            line[(size_t) pos] = s;
            if (++pos == kPeriods) pos = 0;
        }
        xPrev = v;
        const double y = f2.run (f1.run (-held));          // re-invert, IC104 (1/2) then (2/2)
        wet = y / kVoltsPerUnit;
        return wet;
    }
    // inverting stage with R101 || C101 || diodes in the feedback: x / R102 + v / R101 + iC(v) + id(v) = 0, v = output
    double limiter (double x)
    {
        double v = vC;
        for (int it = 0; it < 6; ++it)
        {
            const double e = v / kNVt, sh = std::sinh (std::clamp (e, -60.0, 60.0)), ch = std::cosh (std::clamp (e, -60.0, 60.0));
            const double f = x / kR102 + v / kR101 + (gC * v - (gC * vC + iC)) + 2 * kIs * sh;
            const double df = 1 / kR101 + gC + 2 * kIs * ch / kNVt;
            const double dv = f / df; v -= dv;
            if (std::fabs (dv) < 1e-9) break;
        }
        iC = gC * (v - vC) - iC; vC = v;
        return v;
    }
    // loop gain at DEPTH 10: the peak of |limiter (small signal) x BBD hold x filter| at DELAY 10 is set to the gain that
    // decays 60 dB in 3 s with 100 ms repeats (-2 dB per pass); wet level: the first repeat at 400 Hz equals the dry (I)
    void calibrate()
    {
        const double fClk = kPeriods / kDelayMax;
        double peak = 0, at400 = 0;
        for (int i = 0; i < 2000; ++i)
        {
            const double f = 20.0 * std::pow (1000.0, i / 1999.0), w = 2 * kPi * f;
            const std::complex<double> s (0, w);
            const std::complex<double> lim = -(kR101 / kR102) / (1.0 + s * kR101 * kC101);
            const double xw = kPi * f / fClk, zoh = xw > 0 ? std::fabs (std::sin (xw) / xw) : 1.0;
            const double tau = kR128 * kC117;
            const std::complex<double> h1 = (1.0 + kR128 / kR129 + s * tau) / (1.0 + s * tau);
            const std::complex<double> h2 = 1.0 / (s * s * (kR130 * kR131 * kC118 * kC119) + s * (kC119 * (kR130 + kR131)) + 1.0);
            const double g = std::abs (lim) * zoh * std::abs (h1 * h2);
            peak = std::max (peak, g);
            if (at400 == 0 && f >= 400) at400 = g;
        }
        // the peak's per-pass gain for a 3 s decay; kLoopTrim (measured, tools/measure_rv.py R3) corrects for the broadband
        // decay being faster than the peak's alone (the off-peak frequencies die sooner)
        const double perPass = std::pow (10.0, -60.0 / 20.0 * kDelayMax / kRt60Max);
        chainPeak = peak;
        loopMax = perPass / peak * trim;
        wetCal = 1.0 / (kInAtten * at400);
    }

    double fs = 48000, delayKnob = 5, depthKnob = 0, loopMax = 0, wetCal = 1, chainPeak = 1, trim = kLoopTrim;
    Biquad f1, f2, ap;
    std::vector<double> line;
    int pos = 0;
    double acc = 0, held = 0, xPrev = 0, vC = 0, iC = 0, gC = 0, wet = 0;
    bool on = true;
};
} // namespace gerv
