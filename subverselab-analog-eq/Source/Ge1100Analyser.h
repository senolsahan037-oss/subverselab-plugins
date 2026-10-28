// The original's "SPECTRUM ANALIZER" (service manual page 10; values in docs/reference/ge1100/VALUES.md): L + R summed,
// LEVEL, twelve multiple-feedback band-pass filters (Q about 10), a diode peak detector per band on 10 uF, read one band at
// a time by the scan (16 slots of ~0.97 ms), each read bleeding the capacitor into 1.6 k, and the bus voltage compared
// with a resistor ladder of ten thresholds (the FL segments). Header-only, no JUCE; it only draws, it never touches audio.
// What is inferred rather than read is marked (I).
#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace gefl
{
constexpr double kPi = 3.14159265358979323846;
constexpr int kBands = 12, kSegments = 10, kSlots = 16;
constexpr double kVoltsPerUnit = 2.0 * 1.4142135623730951;   // a full-scale sine is 2 V rms (as the equaliser)

// band-pass: R1 in, R2 shunt, two equal C, R3 feedback (R10 ... R45, C6 ... C29)
struct BandValues { double label, r1, r2, c, r3; };
constexpr std::array<BandValues, kBands> kBandValues { {
    { 16, 62e3, 1.1e3, 0.47e-6, 390e3 },  { 32, 47e3, 750, 0.33e-6, 300e3 },   { 64, 36e3, 560, 0.22e-6, 220e3 },
    { 125, 47e3, 820, 0.082e-6, 300e3 },  { 250, 51e3, 820, 0.039e-6, 330e3 }, { 500, 43e3, 750, 0.022e-6, 270e3 },
    { 1000, 56e3, 1e3, 8.2e-9, 360e3 },   { 2000, 62e3, 1.2e3, 3.3e-9, 470e3 }, { 4000, 36e3, 750, 2.7e-9, 270e3 },
    { 8000, 36e3, 820, 1.2e-9, 330e3 },   { 16000, 30e3, 750, 680e-12, 270e3 }, { 32000, 30e3, 1.2e3, 220e-12, 390e3 } } };

// threshold ladder: R74 24 k from the supply, then R75 ... R84 to ground; the top tap reads 13 V (printed)
constexpr std::array<double, kSegments> kLadderR { 10e3, 12e3, 18e3, 11e3, 9.1e3, 7.5e3, 3.9e3, 3e3, 2.7e3, 430 };
constexpr double kLadderTopV = 13.0;

constexpr double kMixGainMax = 1.0 + 51e3 / 1e3;   // IC2: 1 + R8 / R7, after LEVEL VR1 100 k (B)
// TOTAL column: IC1 (2/2), no filter, gain 1 + R48 160 k / R47 5.1 k, its own D25 / C42 / Q15 on scan slot 13. (I) its
// input is the L + R sum before LEVEL (IC1 1/2): then the manual's "140 mV for 0 dB (TOTAL), both channels" lands
// 2.1 dB above the 0 dB tap (110 mV computed; 140 mV reads +3), and LEVEL does not move it.
constexpr double kTotalGain = 1.0 + 160e3 / 5.1e3;
constexpr int kColumns = kBands + 1;              // the twelve bands and TOTAL
constexpr double kRailV = 13.5;                   // NJM2058 output swing at +-15 V (I)
constexpr double kDiodeV = 0.6;                   // D1 ... D23 (1S1588)
constexpr double kReadOffsetV = 0.17;             // (I) Darlington + D2 path, fitted: the ladder's -15 ... 0 dB taps lie on bus = 10.08 A - 0.77 V
constexpr double kHoldC = 10e-6, kBleedR = 1.6e3; // C30 ... C41, R49
constexpr double kSlotS = 0.97e-3;                // scan astable Q1 / Q2: 100 k, 6.8 nF, 16.8 V rail -> ~1.03 kHz, one slot per cycle
constexpr double kChargeVPerS = 2000.0;           // (I) ~20 mA op-amp output current into 10 uF

class Analyser
{
public:
    void prepare (double sampleRate)
    {
        fs = sampleRate;
        os = fs < 80000 ? 2 : 1;                  // the 32 kHz band needs a rate above ~80 kHz
        const double f = fs * os;
        for (int b = 0; b < kBands; ++b)
        {
            const auto& v = kBandValues[(size_t) b];
            // H(s) = -(s / (R1 C)) / (s^2 + s 2 / (R3 C) + (R1 + R2) / (R1 R2 R3 C^2)), bilinear pre-warped at f0
            const double w0 = std::sqrt ((v.r1 + v.r2) / (v.r1 * v.r2 * v.r3)) / v.c;
            const double k = w0 / std::tan (w0 / (2 * f)), k2 = k * k;
            const double n1 = -1.0 / (v.r1 * v.c), d1 = 2.0 / (v.r3 * v.c), d0 = w0 * w0;
            const double a0 = k2 + d1 * k + d0;
            bp[(size_t) b] = { n1 * k / a0, 0.0, -n1 * k / a0, (2 * d0 - 2 * k2) / a0, (k2 - d1 * k + d0) / a0 };
        }
        // ladder taps, ascending
        double below = 0, total = 0;
        for (double r : kLadderR) total += r;
        for (int i = 0; i < kSegments; ++i) { below += kLadderR[(size_t) (kSegments - 1 - i)]; tap[(size_t) i] = kLadderTopV * below / total; }
        bleedPerSample = std::exp (-1.0 / (f * kBleedR * kHoldC));
        slotSamples = std::max (1, (int) std::lround (kSlotS * f));
        chargePerSample = kChargeVPerS / f;
        reset();
    }
    void reset()
    {
        for (auto& s : st) s = {};
        hold.fill (0); bus.fill (0); upHist.fill (0); upPos = 0; slotCount = 0; slot = 0;
    }
    void setLevel (double wiper01) { level = std::clamp (wiper01, 0.0, 1.0); }   // VR1, linear (B)
    double tapVolts (int i) const { return tap[(size_t) i]; }

    void process (float l, float r)
    {
        const double x = (double) (l + r) * kVoltsPerUnit;   // the sum, volts, before LEVEL
        if (os == 1) { step (x); return; }
        // 2x: zero-stuffed, half-band low-pass (31 taps, Blackman-windowed sinc)
        upHist[(size_t) upPos] = x;
        for (int ph = 0; ph < 2; ++ph)
        {
            double y = 0;
            for (int j = ph; j < kUpTaps; j += 2) y += kUp()[(size_t) j] * upHist[(size_t) ((upPos - j / 2 + kUpLen) % kUpLen)];
            step (2 * y);
        }
        upPos = (upPos + 1) % kUpLen;
    }
    // FL state: per column (0 ... 11 the bands, 12 TOTAL), the number of lit segments (0 ... 10) and the last read bus voltage
    int segments (int band) const
    {
        int n = 0;
        while (n < kSegments && bus[(size_t) band] >= tap[(size_t) n]) ++n;
        return n;
    }
    double busVolts (int band) const { return bus[(size_t) band]; }

private:
    static constexpr int kUpTaps = 31, kUpLen = 16;
    static const std::array<double, kUpTaps>& kUp()
    {
        static const std::array<double, kUpTaps> h = [] {
            std::array<double, kUpTaps> c {};
            for (int n = 0; n < kUpTaps; ++n)
            {
                const double m = n - (kUpTaps - 1) / 2.0, s = m == 0 ? 0.5 : std::sin (kPi * m / 2) / (kPi * m);
                const double w = 0.42 - 0.5 * std::cos (2 * kPi * n / (kUpTaps - 1)) + 0.08 * std::cos (4 * kPi * n / (kUpTaps - 1));
                c[(size_t) n] = s * w;
            }
            return c;
        }();
        return h;
    }
    void step (double sum)
    {
        const double x = sum * level * kMixGainMax;
        for (int b = 0; b < kBands; ++b)
        {
            auto& s = st[(size_t) b]; const auto& c = bp[(size_t) b];
            double y = c[0] * x + s.z1; s.z1 = c[1] * x - c[3] * y + s.z2; s.z2 = c[2] * x - c[4] * y;
            y = std::clamp (y, -kRailV, kRailV);
            const double target = y - kDiodeV;                      // half-wave: the diode only charges upward
            auto& h = hold[(size_t) b];
            if (target > h) h = std::min (target, h + chargePerSample);
        }
        {
            const double y = std::clamp (sum * kTotalGain, -kRailV, kRailV), target = y - kDiodeV;
            auto& h = hold[(size_t) kBands];
            if (target > h) h = std::min (target, h + chargePerSample);
        }
        // scan: slot k (0 ... 11) reads band k, slot 12 TOTAL; the read bleeds the capacitor into R49; 13 ... 15 read
        // nothing (I: the slot order)
        if (slot < kColumns)
        {
            auto& h = hold[(size_t) slot];
            if (slotCount == 0) bus[(size_t) slot] = std::max (0.0, h - kReadOffsetV);   // the comparators light from the slot's start, before this read bleeds it
            h = kReadOffsetV + (h - kReadOffsetV) * bleedPerSample;
            if (h < 0) h = 0;
        }
        if (++slotCount >= slotSamples) { slotCount = 0; slot = (slot + 1) % kSlots; }
    }

    struct State { double z1 = 0, z2 = 0; };
    double fs = 48000, level = 1, bleedPerSample = 1, chargePerSample = 1;
    int os = 1, slotSamples = 47, slotCount = 0, slot = 0, upPos = 0;
    std::array<std::array<double, 5>, kBands> bp {};
    std::array<State, kBands> st {};
    std::array<double, kColumns> hold {}, bus {};
    std::array<double, kSegments> tap {};
    std::array<double, kUpLen> upHist {};
};
} // namespace gefl
