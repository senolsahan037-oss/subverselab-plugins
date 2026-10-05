#pragma once
// Karplus-Strong plucked-string melody (guitar / baglama-like), deterministic.
// 8 notes, 100 bpm, one note per beat (2 bars of 4/4 = 4.8 s); notes 1,3,5,7 (0-based odd) are
// palm-muted (shorter decay, darker loop filter). Used by make_demos (source 2) and by
// render --selftest (h), so the reference-match check runs on the same material the owner hears.
#include <cmath>
#include <cstdint>
#include <vector>

namespace pluck {

// Hicaz-flavoured line in guitar range (Hz): D3 Eb3 F#3 G3 A3 G3 F#3 D3
static const double kNotes[8] = {146.83, 155.56, 185.00, 196.00, 220.00, 196.00, 185.00, 146.83};
static constexpr double kBeat = 0.6;  // 100 bpm

// one plucked note, mono, `len` samples
static inline void note(std::vector<float>& out, size_t at, size_t len, double f0, bool mute, int rate, uint32_t seed) {
    const double period = rate / f0;
    // loop: y[n] = x[n] + rho * lp(y[n-N]) ; fractional part via first-order allpass
    const int N = int(std::floor(period - 0.5));      // the averaging filter adds 0.5 sample
    const double frac = period - 0.5 - N;
    const double apc = (1.0 - frac) / (1.0 + frac);
    const double t60 = mute ? 0.18 : 2.2;             // seconds
    const double rho = std::pow(10.0, -3.0 / (t60 * f0));
    const double bright = mute ? 0.30 : 0.5;          // loop LP mix (0.5 = plain 2-tap average)
    std::vector<double> dl(size_t(N), 0.0);
    // excitation: triangular string displacement plucked at 1/5 of the length (harmonics fall
    // ~12 dB/oct like a real pluck) plus a little low-passed pick noise
    uint32_t s = seed;
    std::vector<double> ex(size_t(N), 0.0);
    const int apex = std::max(1, N / 5);
    double lp = 0.0, mean = 0.0;
    for (int i = 0; i < N; ++i) {
        s = s * 1664525u + 1013904223u;
        const double nz = double(int32_t(s)) / 2147483648.0;
        lp += (mute ? 0.15 : 0.3) * (nz - lp);
        const double tri = i < apex ? double(i) / apex : double(N - i) / double(N - apex);
        ex[size_t(i)] = tri + (mute ? 0.10 : 0.20) * lp;
        mean += ex[size_t(i)];
    }
    mean /= N;
    for (auto& v : ex) v -= mean;
    size_t w = 0;
    double prev = 0.0, apx = 0.0, apy = 0.0;
    for (size_t n = 0; n < len && at + n < out.size(); ++n) {
        const double x = n < size_t(N) ? ex[n] : 0.0;
        const double d0 = dl[w];                         // y[n-N]
        const double avg = (1.0 - bright) * prev + bright * d0;
        prev = d0;
        const double ap = apc * avg + apx - apc * apy;   // fractional-delay allpass
        apx = avg; apy = ap;
        const double y = x + rho * ap;
        dl[w] = y;
        w = (w + 1) % size_t(N);
        const double fade = (len - n < 256) ? double(len - n) / 256.0 : 1.0;
        out[at + n] += float(0.55 * y * fade);
    }
}

// the melody, mono, starting at `start` samples, returns the onset sample positions
static inline std::vector<size_t> melody(std::vector<float>& out, size_t start, int rate) {
    std::vector<size_t> on;
    const size_t beat = size_t(kBeat * rate);
    for (int k = 0; k < 8; ++k) {
        const size_t at = start + size_t(k) * beat;
        note(out, at, size_t(1.6 * rate), kNotes[k], (k & 1) != 0, rate, 0x1234u + 977u * uint32_t(k));
        on.push_back(at);
    }
    return on;
}

}  // namespace pluck
