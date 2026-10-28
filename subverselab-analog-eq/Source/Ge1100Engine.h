// The graphic equaliser stage of the original (one channel = 12 slide pots, 16 Hz ... 32 kHz), modelled from its traced
// schematic (docs/reference/ge1100/VALUES.md). Header-only, no JUCE: the plug-in and tools/geraw.cpp run exactly this.
//
// Model: the linear circuit with ideal op-amps -> MNA with every capacitor as a voltage source -> continuous state space
// (28 capacitor voltages) -> first-order-hold discretisation (matrix exponential) at 4x the session rate -> run between a
// 129-tap linear-phase polyphase FIR up and down. Same construction as tools/ge_statespace.py, which matches ngspice
// within 0.014 dB / 0.05 deg. Latency: kLatency samples at the session rate.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <string>
#include <utility>
#include <vector>
#include "Ge1100Taper.h"

namespace ge
{
constexpr double kPi = 3.14159265358979323846;

struct BandParts { double cs, cg, rb, rs; };   // series C, gyrator C, gyrator R (to ground), gyrator series R
// VALUES.md, read from the service manual; band 12 R_s: 390 ohm (left-channel drawing)
constexpr std::array<BandParts, 12> kBands { {
    { 10e-6, 0.22e-6, 100e3, 470 }, { 4.7e-6, 0.12e-6, 91e3, 470 }, { 2.2e-6, 0.068e-6, 91e3, 470 },
    { 1.22e-6, 0.033e-6, 82e3, 470 }, { 0.57e-6, 0.018e-6, 82e3, 470 }, { 0.25e-6, 0.012e-6, 68e3, 470 },
    { 0.15e-6, 0.0068e-6, 51e3, 470 }, { 0.068e-6, 0.0047e-6, 43e3, 470 }, { 0.039e-6, 0.0022e-6, 39e3, 470 },
    { 0.018e-6, 0.0022e-6, 24e3, 430 }, { 0.01e-6, 0.0012e-6, 20e3, 430 }, { 0.0056e-6, 680e-12, 16e3, 390 } } };
constexpr std::array<int, 12> kLabels { 16, 32, 64, 125, 250, 500, 1000, 2000, 4000, 8000, 16000, 32000 };
constexpr int kStates = 4 + 2 * 12;           // C225, C226, C227, C228 + (series C, gyrator C) per band
constexpr int kOs = 4;
// 4x interpolation / decimation FIR: cutoff at the session Nyquist, flat to 20 kHz even at 44.1 kHz (transition
// 20 ... 24.1 kHz). 129 taps rolled off 0.95 dB at 20 kHz at 44.1 kHz (26 Oct).
constexpr int kTaps = 257;
constexpr int kLatency = (kTaps - 1) / kOs;   // up + down = 256 samples at 4x = 64 at the session rate
constexpr double kPot = 100e3, kRmin = 1.0;   // slide pot; 1 ohm at an end or at the centre tap
// Levels. A full-scale sine (peak 1.0) is 2 V rms, a CD player's full-scale output, the source this hi-fi unit was built
// for. The op-amps run on +-15 V; the owner's manual gives a maximum output of 9 V rms at 1 kHz with 0.01 % THD
// (12.7 V peak), so the output stage stays linear to 12.7 V and clips at 13.3 V peak with a short rounded knee.
constexpr double kVoltsPerUnit = 2.0 * 1.4142135623730951;
constexpr double kRailV = 13.3, kKneeV = 0.5;   // linear up to kRailV - kKneeV, flat at kRailV

using Mat = std::vector<double>;              // row-major, square or n x m as noted

// ---------------------------------------------------------------- small dense linear algebra (control rate only)
inline bool luSolve (Mat& M, int n, Mat& rhs, int nrhs)   // M n x n, rhs n x nrhs (row-major); rhs <- M^-1 rhs
{
    std::vector<int> piv ((size_t) n);
    for (int k = 0; k < n; ++k)
    {
        int p = k; double best = std::fabs (M[(size_t) (k * n + k)]);
        for (int i = k + 1; i < n; ++i) { const double v = std::fabs (M[(size_t) (i * n + k)]); if (v > best) { best = v; p = i; } }
        if (best == 0) return false;
        if (p != k)
        {
            for (int j = 0; j < n; ++j) std::swap (M[(size_t) (k * n + j)], M[(size_t) (p * n + j)]);
            for (int j = 0; j < nrhs; ++j) std::swap (rhs[(size_t) (k * nrhs + j)], rhs[(size_t) (p * nrhs + j)]);
        }
        const double d = M[(size_t) (k * n + k)];
        for (int i = k + 1; i < n; ++i)
        {
            const double f = M[(size_t) (i * n + k)] / d;
            if (f == 0) continue;
            for (int j = k; j < n; ++j) M[(size_t) (i * n + j)] -= f * M[(size_t) (k * n + j)];
            for (int j = 0; j < nrhs; ++j) rhs[(size_t) (i * nrhs + j)] -= f * rhs[(size_t) (k * nrhs + j)];
        }
    }
    for (int k = n - 1; k >= 0; --k)
        for (int j = 0; j < nrhs; ++j)
        {
            double s = rhs[(size_t) (k * nrhs + j)];
            for (int i = k + 1; i < n; ++i) s -= M[(size_t) (k * n + i)] * rhs[(size_t) (i * nrhs + j)];
            rhs[(size_t) (k * nrhs + j)] = s / M[(size_t) (k * n + k)];
        }
    return true;
}

inline Mat matMul (const Mat& a, const Mat& b, int n)
{
    Mat c ((size_t) (n * n), 0.0);
    for (int i = 0; i < n; ++i)
        for (int k = 0; k < n; ++k)
        {
            const double v = a[(size_t) (i * n + k)];
            if (v == 0) continue;
            for (int j = 0; j < n; ++j) c[(size_t) (i * n + j)] += v * b[(size_t) (k * n + j)];
        }
    return c;
}

// expm by scaling and squaring with a [6/6] Pade approximant (||A / 2^s||_1 <= 0.5)
inline Mat expm (Mat A, int n)
{
    double norm = 0;
    for (int j = 0; j < n; ++j) { double s = 0; for (int i = 0; i < n; ++i) s += std::fabs (A[(size_t) (i * n + j)]); norm = std::max (norm, s); }
    int sq = 0;
    while (norm > 0.5) { norm *= 0.5; ++sq; }
    const double scale = std::ldexp (1.0, -sq);
    for (auto& v : A) v *= scale;
    static const double c[] = { 1.0, 0.5, 5.0 / 44, 1.0 / 66, 1.0 / 792, 1.0 / 15840, 1.0 / 665280 };
    Mat N ((size_t) (n * n), 0.0), D ((size_t) (n * n), 0.0), P ((size_t) (n * n), 0.0);
    for (int i = 0; i < n; ++i) P[(size_t) (i * n + i)] = 1;
    for (int k = 0; k <= 6; ++k)
    {
        if (k > 0) P = matMul (P, A, n);
        const double sgn = (k % 2) ? -1.0 : 1.0;
        for (size_t i = 0; i < P.size(); ++i) { N[i] += c[k] * P[i]; D[i] += sgn * c[k] * P[i]; }
    }
    luSolve (D, n, N, n);   // N <- D^-1 N
    for (int s = 0; s < sq; ++s) N = matMul (N, N, n);
    return N;
}

// ---------------------------------------------------------------- the circuit -> continuous state space
struct Continuous { Mat A; std::vector<double> B, C; double D = 0; };

// pos[i] in [0, 1]: wiper from the N end (0, full boost) to the MID end (1, full cut); 0.5 = the centre tap (flat).
inline Continuous buildContinuous (const std::array<double, 12>& pos)
{
    // nodes: 0 ground; fixed 1..10; per band w, g, p, q
    enum { SRC = 1, A_ = 2, P = 3, N = 4, OUT1 = 5, MID = 6, OUT2 = 7, N2 = 8, O3 = 9, OUT = 10 };
    const int nFixed = 10, nNodes = nFixed + 4 * 12;
    auto w = [] (int b) { return 11 + 4 * b; };
    auto g = [] (int b) { return 12 + 4 * b; };
    auto p = [] (int b) { return 13 + 4 * b; };
    auto q = [] (int b) { return 14 + 4 * b; };
    struct E { int a, b; double v; };
    std::vector<E> R, C;
    std::vector<std::array<int, 3>> OA;   // output, +, -
    R.insert (R.end(), { { A_, P, 30e3 }, { P, 0, 56e3 }, { OUT1, N, 2.7e3 }, { OUT1, MID, 2.7e3 },
                         { OUT2, N2, 20e3 }, { N2, 0, 36e3 }, { O3, OUT, 620.0 }, { OUT, 0, 18e3 } });
    C.insert (C.end(), { { SRC, A_, 6.8e-6 }, { OUT1, N, 470e-12 }, { OUT2, N2, 68e-12 }, { OUT2, O3, 4.7e-6 } });
    OA.push_back ({ OUT1, P, N }); OA.push_back ({ OUT2, MID, N2 });
    for (int b = 0; b < 12; ++b)
    {
        const double x = std::clamp (pos[(size_t) b], 0.0, 1.0);
        if (x <= 0.5) R.insert (R.end(), { { N, w (b), std::max (x * kPot, kRmin) }, { w (b), 0, std::max ((0.5 - x) * kPot, kRmin) }, { 0, MID, 0.5 * kPot } });
        else          R.insert (R.end(), { { N, 0, 0.5 * kPot }, { 0, w (b), std::max ((x - 0.5) * kPot, kRmin) }, { w (b), MID, std::max ((1 - x) * kPot, kRmin) } });
        const auto& k = kBands[(size_t) b];
        C.insert (C.end(), { { w (b), g (b), k.cs }, { g (b), p (b), k.cg } });
        R.insert (R.end(), { { p (b), 0, k.rb }, { q (b), g (b), k.rs } });
        OA.push_back ({ q (b), p (b), q (b) });
    }
    const int nc = (int) C.size(), no = (int) OA.size();
    const int size = nNodes + nc + no + 1;
    auto at = [size] (int r, int c) { return (size_t) (r * size + c); };
    Mat M ((size_t) (size * size), 0.0);
    auto node = [] (int nd) { return nd - 1; };   // ground (0) has no row
    for (const auto& e : R)
    {
        const double y = 1.0 / e.v;
        if (e.a) M[at (node (e.a), node (e.a))] += y;
        if (e.b) M[at (node (e.b), node (e.b))] += y;
        if (e.a && e.b) { M[at (node (e.a), node (e.b))] -= y; M[at (node (e.b), node (e.a))] -= y; }
    }
    auto stampSource = [&] (int a, int b, int row)
    {
        if (a) { M[at (node (a), row)] += 1; M[at (row, node (a))] += 1; }
        if (b) { M[at (node (b), row)] -= 1; M[at (row, node (b))] -= 1; }
    };
    for (int k = 0; k < nc; ++k) stampSource (C[(size_t) k].a, C[(size_t) k].b, nNodes + k);
    stampSource (SRC, 0, nNodes + nc + no);
    for (int k = 0; k < no; ++k)
    {
        const int col = nNodes + nc + k;
        M[at (node (OA[(size_t) k][0]), col)] += 1;
        M[at (col, node (OA[(size_t) k][1]))] += 1;
        M[at (col, node (OA[(size_t) k][2]))] -= 1;
    }
    // one solve with nc + 1 right-hand sides: unit capacitor voltage each, then the unit input
    const int nr = nc + 1;
    Mat rhs ((size_t) (size * nr), 0.0);
    for (int k = 0; k < nc; ++k) rhs[(size_t) ((nNodes + k) * nr + k)] = 1;
    rhs[(size_t) ((nNodes + nc + no) * nr + nc)] = 1;
    luSolve (M, size, rhs, nr);
    Continuous s;
    s.A.assign ((size_t) (nc * nc), 0.0); s.B.assign ((size_t) nc, 0.0); s.C.assign ((size_t) nc, 0.0);
    for (int j = 0; j < nr; ++j)
    {
        for (int k = 0; k < nc; ++k)
        {
            const double dv = rhs[(size_t) ((nNodes + k) * nr + j)] / C[(size_t) k].v;   // capacitor current / C
            if (j < nc) s.A[(size_t) (k * nc + j)] = dv; else s.B[(size_t) k] = dv;
        }
        const double y = rhs[(size_t) (node (OUT) * nr + j)];
        if (j < nc) s.C[(size_t) j] = y; else s.D = y;
    }
    return s;
}

// first-order hold (same as scipy cont2discrete 'foh'): em = expm([[A T, B T, 0], [0, 0, 1], [0, 0, 0]])
// The circuit's response in dB at the given frequencies, H(jw) = C (jw I - A)^-1 B + D, solved directly (complex Gaussian
// elimination with partial pivoting). For the panel's EQUALIZER GRAPHIC: control rate, message thread, never audio.
inline std::vector<double> responseDb (const Continuous& s, const std::vector<double>& freqs)
{
    using c = std::complex<double>;
    const int n = kStates;
    std::vector<double> out;
    out.reserve (freqs.size());
    std::vector<c> M ((size_t) (n * n)), x ((size_t) n);
    for (double f : freqs)
    {
        const c jw (0.0, 2.0 * kPi * f);
        for (int i = 0; i < n; ++i)
        {
            for (int j = 0; j < n; ++j) M[(size_t) (i * n + j)] = -s.A[(size_t) (i * n + j)];
            M[(size_t) (i * n + i)] += jw;
            x[(size_t) i] = s.B[(size_t) i];
        }
        for (int k = 0; k < n; ++k)
        {
            int piv = k;
            for (int i = k + 1; i < n; ++i) if (std::abs (M[(size_t) (i * n + k)]) > std::abs (M[(size_t) (piv * n + k)])) piv = i;
            if (piv != k) { for (int j = 0; j < n; ++j) std::swap (M[(size_t) (k * n + j)], M[(size_t) (piv * n + j)]); std::swap (x[(size_t) k], x[(size_t) piv]); }
            const c d = M[(size_t) (k * n + k)];
            if (std::abs (d) < 1e-300) break;
            for (int i = k + 1; i < n; ++i)
            {
                const c m = M[(size_t) (i * n + k)] / d;
                if (m == c (0.0)) continue;
                for (int j = k; j < n; ++j) M[(size_t) (i * n + j)] -= m * M[(size_t) (k * n + j)];
                x[(size_t) i] -= m * x[(size_t) k];
            }
        }
        for (int i = n - 1; i >= 0; --i)
        {
            c acc = x[(size_t) i];
            for (int j = i + 1; j < n; ++j) acc -= M[(size_t) (i * n + j)] * x[(size_t) j];
            x[(size_t) i] = acc / M[(size_t) (i * n + i)];
        }
        c h = s.D;
        for (int i = 0; i < n; ++i) h += s.C[(size_t) i] * x[(size_t) i];
        out.push_back (20.0 * std::log10 (std::max (1e-12, std::abs (h))));
    }
    return out;
}

struct Discrete { Mat Ad; std::vector<double> Bd, Cd; double Dd = 0; };
inline Discrete discretizeFoh (const Continuous& s, double T)
{
    const int n = kStates, m = n + 2;
    Mat em ((size_t) (m * m), 0.0);
    for (int i = 0; i < n; ++i) { for (int j = 0; j < n; ++j) em[(size_t) (i * m + j)] = s.A[(size_t) (i * n + j)] * T; em[(size_t) (i * m + n)] = s.B[(size_t) i] * T; }
    em[(size_t) (n * m + n + 1)] = 1;
    const Mat e = expm (em, m);
    Discrete d;
    d.Ad.assign ((size_t) (n * n), 0.0); d.Bd.assign ((size_t) n, 0.0); d.Cd = s.C;
    std::vector<double> ms13 ((size_t) n);
    for (int i = 0; i < n; ++i) { ms13[(size_t) i] = e[(size_t) (i * m + n + 1)]; for (int j = 0; j < n; ++j) d.Ad[(size_t) (i * n + j)] = e[(size_t) (i * m + j)]; }
    for (int i = 0; i < n; ++i)
    {
        double acc = e[(size_t) (i * m + n)] - ms13[(size_t) i];
        for (int j = 0; j < n; ++j) acc += d.Ad[(size_t) (i * n + j)] * ms13[(size_t) j];
        d.Bd[(size_t) i] = acc;
    }
    d.Dd = s.D;
    for (int j = 0; j < n; ++j) d.Dd += s.C[(size_t) j] * ms13[(size_t) j];
    return d;
}

// ---------------------------------------------------------------- modal form (control rate)
// A = V diag(lambda) V^-1 by Householder Hessenberg reduction and a shifted complex QR (Givens) to Schur form, then
// eigenvectors from the triangular factor. Each mode is discretised with the same first-order hold as discretizeFoh, in
// closed form, so the per-sample work is one complex multiply-add per mode instead of a 28 x 28 matrix-vector product.
using cd = std::complex<double>;
// per mode: x <- a x + b u, y = Re(sum c x) + d u. V / Vinv: eigenvectors and their inverse; m13: the first-order-hold
// offset, physical modal coordinate = x + m13 u. Both are used to hand the state over when the sliders move.
struct Modal { std::vector<cd> a, b, c, m13, V, Vinv; double d = 0; bool ok = false; };

inline bool schurComplex (std::vector<cd>& H, std::vector<cd>& Q, int n)
{
    auto at = [n] (int r, int c) { return (size_t) (r * n + c); };
    int hi = n - 1, iter = 0, total = 0;
    while (hi > 0)
    {
        int l = hi;
        while (l > 0)
        {
            const double s = std::abs (H[at (l - 1, l - 1)]) + std::abs (H[at (l, l)]);
            if (std::abs (H[at (l, l - 1)]) <= 1e-15 * (s > 0 ? s : 1.0)) { H[at (l, l - 1)] = 0; break; }
            --l;
        }
        if (l == hi) { --hi; iter = 0; continue; }
        if (++iter > 200 || ++total > 100 * n) return false;
        const cd a = H[at (hi - 1, hi - 1)], b = H[at (hi - 1, hi)], c = H[at (hi, hi - 1)], dd = H[at (hi, hi)];
        const cd tr = a + dd, det = a * dd - b * c, disc = std::sqrt (tr * tr * 0.25 - det);
        const cd m1 = tr * 0.5 + disc, m2 = tr * 0.5 - disc;
        cd mu = std::abs (m1 - dd) < std::abs (m2 - dd) ? m1 : m2;
        if (iter % 11 == 10) mu += std::abs (H[at (hi, hi - 1)]);   // exceptional shift
        for (int k = l; k <= hi; ++k) H[at (k, k)] -= mu;
        std::vector<double> cs ((size_t) n); std::vector<cd> sn ((size_t) n);
        for (int k = l; k < hi; ++k)
        {
            const cd x = H[at (k, k)], y = H[at (k + 1, k)];
            const double r = std::sqrt (std::norm (x) + std::norm (y));
            double cc; cd ss;
            if (r == 0) { cc = 1; ss = 0; }
            else if (std::abs (x) == 0) { cc = 0; ss = std::conj (y) / r; }
            else { const cd al = x / std::abs (x); cc = std::abs (x) / r; ss = al * std::conj (y) / r; }
            cs[(size_t) k] = cc; sn[(size_t) k] = ss;
            for (int j = k; j < n; ++j)
            {
                const cd u = H[at (k, j)], v = H[at (k + 1, j)];
                H[at (k, j)] = cc * u + ss * v; H[at (k + 1, j)] = -std::conj (ss) * u + cc * v;
            }
        }
        for (int k = l; k < hi; ++k)
        {
            const double cc = cs[(size_t) k]; const cd ss = sn[(size_t) k];
            for (int i = 0; i <= std::min (k + 2, hi); ++i)
            {
                const cd u = H[at (i, k)], v = H[at (i, k + 1)];
                H[at (i, k)] = cc * u + std::conj (ss) * v; H[at (i, k + 1)] = -ss * u + cc * v;
            }
            for (int i = 0; i < n; ++i)
            {
                const cd u = Q[at (i, k)], v = Q[at (i, k + 1)];
                Q[at (i, k)] = cc * u + std::conj (ss) * v; Q[at (i, k + 1)] = -ss * u + cc * v;
            }
        }
        for (int k = l; k <= hi; ++k) H[at (k, k)] += mu;
    }
    return true;
}

inline Modal modalFoh (const Continuous& s, double T)
{
    const int n = kStates;
    auto at = [] (int r, int c) { return (size_t) (r * kStates + c); };
    Mat A = s.A, Q0 ((size_t) (n * n), 0.0);
    for (int i = 0; i < n; ++i) Q0[at (i, i)] = 1;
    for (int k = 0; k < n - 2; ++k)   // Householder to upper Hessenberg, A = Q0 H Q0^T
    {
        double norm = 0; for (int i = k + 1; i < n; ++i) norm += A[at (i, k)] * A[at (i, k)];
        norm = std::sqrt (norm);
        if (norm == 0) continue;
        std::vector<double> v ((size_t) n, 0.0);
        for (int i = k + 1; i < n; ++i) v[(size_t) i] = A[at (i, k)];
        v[(size_t) (k + 1)] += (A[at (k + 1, k)] >= 0 ? norm : -norm);
        double vn = 0; for (int i = k + 1; i < n; ++i) vn += v[(size_t) i] * v[(size_t) i];
        if (vn == 0) continue;
        for (int j = 0; j < n; ++j)   // A = P A
        {
            double dot = 0; for (int i = k + 1; i < n; ++i) dot += v[(size_t) i] * A[at (i, j)];
            const double f = 2 * dot / vn; for (int i = k + 1; i < n; ++i) A[at (i, j)] -= f * v[(size_t) i];
        }
        for (int i = 0; i < n; ++i)   // A = A P, Q0 = Q0 P
        {
            double dot = 0, dq = 0;
            for (int j = k + 1; j < n; ++j) { dot += A[at (i, j)] * v[(size_t) j]; dq += Q0[at (i, j)] * v[(size_t) j]; }
            const double f = 2 * dot / vn, fq = 2 * dq / vn;
            for (int j = k + 1; j < n; ++j) { A[at (i, j)] -= f * v[(size_t) j]; Q0[at (i, j)] -= fq * v[(size_t) j]; }
        }
    }
    std::vector<cd> H (A.begin(), A.end()), Q (Q0.begin(), Q0.end());
    Modal m;
    if (! schurComplex (H, Q, n)) return m;
    // eigenvectors of the triangular factor, then V = Q Y
    std::vector<cd> V ((size_t) (n * n), 0.0);
    for (int i = 0; i < n; ++i)
    {
        std::vector<cd> y ((size_t) n, 0.0); y[(size_t) i] = 1;
        const cd lam = H[at (i, i)];
        for (int j = i - 1; j >= 0; --j)
        {
            cd acc = 0; for (int k = j + 1; k <= i; ++k) acc += H[at (j, k)] * y[(size_t) k];
            cd den = H[at (j, j)] - lam; if (std::abs (den) < 1e-14 * (std::abs (lam) + 1)) den = 1e-14 * (std::abs (lam) + 1);
            y[(size_t) j] = -acc / den;
        }
        for (int r = 0; r < n; ++r) { cd acc = 0; for (int k = 0; k <= i; ++k) acc += Q[at (r, k)] * y[(size_t) k]; V[at (r, i)] = acc; }
    }
    // [beta | Vinv] = V^-1 [B | I] (complex LU with partial pivoting), gamma = C V
    const int w = n + 1;
    std::vector<cd> M = V, R ((size_t) (n * w), 0.0);
    for (int i = 0; i < n; ++i) { R[(size_t) (i * w)] = s.B[(size_t) i]; R[(size_t) (i * w + 1 + i)] = 1.0; }
    for (int k = 0; k < n; ++k)
    {
        int p = k; for (int i = k + 1; i < n; ++i) if (std::abs (M[at (i, k)]) > std::abs (M[at (p, k)])) p = i;
        if (std::abs (M[at (p, k)]) == 0) return m;
        if (p != k) { for (int j = 0; j < n; ++j) std::swap (M[at (k, j)], M[at (p, j)]); for (int j = 0; j < w; ++j) std::swap (R[(size_t) (k * w + j)], R[(size_t) (p * w + j)]); }
        for (int i = k + 1; i < n; ++i)
        {
            const cd f = M[at (i, k)] / M[at (k, k)];
            for (int j = k; j < n; ++j) M[at (i, j)] -= f * M[at (k, j)];
            for (int j = 0; j < w; ++j) R[(size_t) (i * w + j)] -= f * R[(size_t) (k * w + j)];
        }
    }
    for (int k = n - 1; k >= 0; --k)
        for (int j = 0; j < w; ++j)
        {
            cd acc = R[(size_t) (k * w + j)];
            for (int i = k + 1; i < n; ++i) acc -= M[at (k, i)] * R[(size_t) (i * w + j)];
            R[(size_t) (k * w + j)] = acc / M[at (k, k)];
        }
    std::vector<cd> beta ((size_t) n);
    m.Vinv.assign ((size_t) (n * n), 0.0);
    for (int i = 0; i < n; ++i) { beta[(size_t) i] = R[(size_t) (i * w)]; for (int j = 0; j < n; ++j) m.Vinv[at (i, j)] = R[(size_t) (i * w + 1 + j)]; }
    m.V = V;
    m.a.resize ((size_t) n); m.b.resize ((size_t) n); m.c.resize ((size_t) n); m.m13.resize ((size_t) n); m.d = s.D;
    cd dsum = 0;
    for (int i = 0; i < n; ++i)
    {
        cd gam = 0; for (int r = 0; r < n; ++r) gam += s.C[(size_t) r] * V[at (r, i)];
        const cd lam = H[at (i, i)], z = lam * T, e = std::exp (z);
        // first-order hold, closed form: m12 = beta (e - 1) / lam, m13 = beta (e - 1 - lam T) / (lam^2 T)
        cd f12, f13;
        if (std::abs (z) < 1e-4) { f12 = T * (1.0 + z * 0.5 + z * z / 6.0); f13 = T * (0.5 + z / 6.0 + z * z / 24.0); }
        else { f12 = (e - 1.0) / lam; f13 = (e - 1.0 - z) / (lam * z); }
        const cd m12 = beta[(size_t) i] * f12, m13 = beta[(size_t) i] * f13;
        m.a[(size_t) i] = e; m.b[(size_t) i] = m12 - m13 + e * m13; m.c[(size_t) i] = gam; m.m13[(size_t) i] = m13;
        dsum += gam * m13;
    }
    m.d += dsum.real();
    m.ok = true;
    return m;
}

// ---------------------------------------------------------------- 4x linear-phase polyphase FIR
inline std::array<double, kTaps> lowpass4x()
{
    auto i0 = [] (double x) { double s = 1, t = 1; for (int k = 1; k < 40; ++k) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; };
    std::array<double, kTaps> h {};
    const double beta = 8.0, mid = (kTaps - 1) / 2.0, fc = 0.5 / kOs;   // cutoff at the session Nyquist
    double sum = 0;
    for (int n = 0; n < kTaps; ++n)
    {
        const double t = n - mid, sinc = t == 0 ? 2 * fc : std::sin (2 * kPi * fc * t) / (kPi * t);
        const double r = t / mid, win = i0 (beta * std::sqrt (std::max (0.0, 1 - r * r))) / i0 (beta);
        h[(size_t) n] = sinc * win; sum += h[(size_t) n];
    }
    for (auto& v : h) v /= sum;
    return h;
}

// One channel: 12 slide pots. Settings come as panel marks in dB (setMarks, Ge1100Taper.h) or as raw pot positions.
// When a setting changes, the new model is built (0.39 ms measured, 26 Oct) at most once every kRebuildEvery samples, the
// circuit's state (capacitor voltages) is handed from the old modes to the new ones, and the two outputs are crossfaded
// over kFade samples, so a moving slider neither clicks nor zips.
class Channel
{
public:
    static constexpr int kRebuildEvery = 256;   // session samples
    static constexpr int kFade = 64;            // session samples

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        const auto hd = lowpass4x();
        // per-phase coefficient rows for the interpolator (phase k: taps k, k + 4, ...), zero-padded to kPh; the full
        // filter for the decimator; buffers are written backwards so both loops run forward over contiguous floats
        for (int ph = 0; ph < kOs; ++ph) for (int i = 0; i < kPh; ++i) { const int t = ph + kOs * i; hp[(size_t) (ph * kPh + i)] = t < kTaps ? (float) (hd[(size_t) t] * kOs) : 0.0f; }
        for (int t = 0; t < kTapsPad; ++t) hr[(size_t) t] = t < kTaps ? (float) hd[(size_t) t] : 0.0f;
        built = target; pending = false; att = attTarget; byMix = bypassTarget;
        slot[0] = makeSlot (built); slot[1] = slot[0];
        cur = 0; fadeLeft = 0; sinceRebuild = kRebuildEvery;
        reset();
    }
    void reset()
    {
        for (auto& sl : slot) { sl.xr.fill (0); sl.xi.fill (0); }
        inBuf.fill (0); osBuf.fill (0); inPos = osPos = 0; lastU = 0; prevV = 0;
    }
    void setPositions (const std::array<double, 12>& p) { user = p; applyReverse(); }
    void setMarks (const std::array<double, 12>& db) { std::array<double, 12> p; for (int b = 0; b < 12; ++b) p[(size_t) b] = positionForMark (b, db[(size_t) b]); setPositions (p); }
    // EQ IN/OUT: the model keeps running while out, and the switch is a kBypassFade-sample crossfade between the two
    // paths (both carry the same latency), so a press on a bus does not click (measure_bus.py B5).
    void setBypass (bool b) { bypassTarget = b ? 1.0 : 0.0; }
    // NOR/REV (S10): the switch exchanges the slider network's two bus ends at the EQ amp, so every pot works from the
    // other end: position p becomes 1 - p (boost <-> cut, with the circuit's own +12.5 / -12.9 dB asymmetry kept). (I)
    // the exchanged nets are read from the switch block's pins, not simulated as a separate netlist.
    void setReverse (bool r) { reverse = r; applyReverse(); }
    // ATT -6 dB (S11): R184 33 k in series and R185 62 k to ground ahead of C225; loaded by the EQ amp's 30 k + 56 k
    // input it is a flat -5.648 dB (ngspice, docs/reference/ge1100/eq_att.cir: -5.648 dB from 10 Hz to 20 kHz, phase
    // within 0.005 deg). Applied to the input, also at EQ OUT (I); ramped over kFade samples instead of the switch's click.
    void setAttenuate (bool a) { attTarget = a ? kAttGain : 1.0; }
    float process (float in)
    {
        if (att != attTarget) { const double st = (kAttGain - 1.0) / kFade; att = attTarget < att ? std::max (attTarget, att + st) : std::min (attTarget, att - st); }
        in = (float) (in * att);
        if (sinceRebuild < kRebuildEvery) ++sinceRebuild;
        else if (pending && fadeLeft == 0) rebuild();
        if (--inPos < 0) inPos = kPh - 1;
        inBuf[(size_t) inPos] = in; inBuf[(size_t) (inPos + kPh)] = in;
        const float* xp = inBuf.data() + inPos;
        float z = 0;
        const double g = fadeLeft > 0 ? (double) fadeLeft / kFade : 0.0;   // weight of the old model
        for (int ph = 0; ph < kOs; ++ph)
        {
            const double u = dot (hp.data() + ph * kPh, xp, kPh);
            double y = step (slot[(size_t) cur], u);
            if (fadeLeft > 0) y = g * step (slot[(size_t) (1 - cur)], u) + (1 - g) * y;
            y = railAdaa (y * kVoltsPerUnit) / kVoltsPerUnit;   // at 4x, before the decimator
            if (byMix > 0) y = byMix * u + (1 - byMix) * y;
            lastU = u;
            if (--osPos < 0) osPos = kTapsPad - 1;
            osBuf[(size_t) osPos] = (float) y; osBuf[(size_t) (osPos + kTapsPad)] = (float) y;
            if (ph == 0)
            {
                // down: keep the phase-0 sample of every four, so the total delay is a whole number of session samples
                // (an impulse at 4m peaks at 4m + 2 * 128 after both filters = session sample m + 64)
                z = dot (hr.data(), osBuf.data() + osPos, kTapsPad);
            }
        }
        if (fadeLeft > 0) --fadeLeft;
        if (byMix != bypassTarget) byMix = bypassTarget > byMix ? std::min (1.0, byMix + 1.0 / kBypassFade) : std::max (0.0, byMix - 1.0 / kBypassFade);
        return z;
    }
    static constexpr int latency() { return kLatency; }
    const std::array<double, 12>& positions() const { return built; }

private:
    // modes as separate real / imaginary arrays, so the per-sample loop has no complex type and vectorises
    struct Slot { Modal m; std::array<double, kStates> ar {}, ai {}, br {}, bi {}, cr {}, ci {}, xr {}, xi {}; double d = 0; };

    Slot makeSlot (const std::array<double, 12>& p) const
    {
        Slot sl;
        sl.m = modalFoh (buildContinuous (p), 1.0 / (fs * kOs));
        if (! sl.m.ok) return sl;   // keeps zero coefficients: silence rather than a wrong model (not seen in tests)
        for (int i = 0; i < kStates; ++i)
        {
            sl.ar[(size_t) i] = sl.m.a[(size_t) i].real(); sl.ai[(size_t) i] = sl.m.a[(size_t) i].imag();
            sl.br[(size_t) i] = sl.m.b[(size_t) i].real(); sl.bi[(size_t) i] = sl.m.b[(size_t) i].imag();
            sl.cr[(size_t) i] = sl.m.c[(size_t) i].real(); sl.ci[(size_t) i] = sl.m.c[(size_t) i].imag();
        }
        sl.d = sl.m.d;
        return sl;
    }
    void rebuild()
    {
        Slot next = makeSlot (target);
        if (! next.m.ok) { built = target; pending = false; return; }
        // hand the capacitor voltages over: x_phys = V_old (x_old + m13_old u), x_new = V_new^-1 x_phys - m13_new u
        const Slot& old = slot[(size_t) cur];
        std::array<cd, kStates> q {}, xp {};
        for (int i = 0; i < kStates; ++i) q[(size_t) i] = cd (old.xr[(size_t) i], old.xi[(size_t) i]) + old.m.m13[(size_t) i] * lastU;
        for (int r = 0; r < kStates; ++r) { cd acc = 0; for (int k = 0; k < kStates; ++k) acc += old.m.V[(size_t) (r * kStates + k)] * q[(size_t) k]; xp[(size_t) r] = acc.real(); }
        for (int r = 0; r < kStates; ++r)
        {
            cd acc = 0; for (int k = 0; k < kStates; ++k) acc += next.m.Vinv[(size_t) (r * kStates + k)] * xp[(size_t) k];
            const cd v = acc - next.m.m13[(size_t) r] * lastU;
            next.xr[(size_t) r] = v.real(); next.xi[(size_t) r] = v.imag();
        }
        cur = 1 - cur;
        slot[(size_t) cur] = std::move (next);
        built = target; pending = false; fadeLeft = kFade; sinceRebuild = 0;
    }
    static double step (Slot& sl, double u)
    {
        // y = Re(sum c x) + d u (modes of a real system come in conjugate pairs; the imaginary parts cancel)
        double y0 = 0, y1 = 0;
        for (int i = 0; i < kStates; i += 2)   // kStates is even; two running sums
        {
            y0 += sl.cr[(size_t) i] * sl.xr[(size_t) i] - sl.ci[(size_t) i] * sl.xi[(size_t) i];
            y1 += sl.cr[(size_t) i + 1] * sl.xr[(size_t) i + 1] - sl.ci[(size_t) i + 1] * sl.xi[(size_t) i + 1];
        }
        for (int i = 0; i < kStates; ++i)
        {
            const double xr = sl.xr[(size_t) i], xi = sl.xi[(size_t) i];
            sl.xr[(size_t) i] = sl.ar[(size_t) i] * xr - sl.ai[(size_t) i] * xi + sl.br[(size_t) i] * u;
            sl.xi[(size_t) i] = sl.ar[(size_t) i] * xi + sl.ai[(size_t) i] * xr + sl.bi[(size_t) i] * u;
        }
        return sl.d * u + y0 + y1;
    }
    // the output op-amp's swing: linear below kRailV - kKneeV, a quadratic knee to kRailV, then flat (slope continuous)
    // The rails, with first-order antiderivative anti-aliasing on what they ADD to the signal (the residual
    // rail(v) - v, zero below the knee): below 12.8 V the output is the input, sample for sample; driven into the rails,
    // the clip's high harmonics no longer fold back from the 4x rate (measure_bus.py B4).
    double railAdaa (double v)
    {
        const double p = prevV; prevV = v;
        const double dv = v - p;
        const double r = std::fabs (dv) > 1e-6 ? (residualIntegral (v) - residualIntegral (p)) / dv : rail (0.5 * (v + p)) - 0.5 * (v + p);
        return v + r;
    }
    static double residualIntegral (double v)   // the integral of rail(u) - u from 0 to v (even in v)
    {
        const double a = std::fabs (v), lo = kRailV - kKneeV, c = lo + 2 * kKneeV;
        if (a <= lo) return 0.0;
        if (a <= c) { const double t = a - lo; return -t * t * t / (12 * kKneeV); }
        return -2.0 * kKneeV * kKneeV / 3.0 + kRailV * (a - c) - 0.5 * (a * a - c * c);
    }
    static double rail (double v)
    {
        const double a = std::fabs (v), lo = kRailV - kKneeV;
        if (a <= lo) return v;
        const double t = std::min (a - lo, 2 * kKneeV);
        const double out = lo + t - t * t / (4 * kKneeV);
        return v < 0 ? -out : out;
    }
    static float dot (const float* a, const float* b, int n)   // n is a multiple of 4; four running sums
    {
        float s0 = 0, s1 = 0, s2 = 0, s3 = 0;
        for (int i = 0; i < n; i += 4) { s0 += a[i] * b[i]; s1 += a[i + 1] * b[i + 1]; s2 += a[i + 2] * b[i + 2]; s3 += a[i + 3] * b[i + 3]; }
        return (s0 + s1) + (s2 + s3);
    }
    static constexpr int kPh = ((kTaps + kOs - 1) / kOs + 3) / 4 * 4;   // taps per phase, rounded up to a multiple of 4
    static constexpr int kTapsPad = (kTaps + 3) / 4 * 4;

    double fs = 48000, lastU = 0;
    std::array<double, 12> target { 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5 }, built = target;
    std::array<Slot, 2> slot;
    int cur = 0, fadeLeft = 0, sinceRebuild = 0;
    std::array<float, kOs * kPh> hp {};
    std::array<float, kTapsPad> hr {};
    std::array<float, 2 * kPh> inBuf {};
    std::array<float, 2 * kTapsPad> osBuf {};
    int inPos = 0, osPos = 0;
    void applyReverse()
    {
        std::array<double, 12> p = user;
        if (reverse) for (auto& v : p) v = 1.0 - v;
        if (p != target) { target = p; pending = true; }
    }
    static constexpr double kAttGain = 0.52186;   // 10^(-5.648 / 20)
    std::array<double, 12> user { 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5 };
    double att = 1.0, attTarget = 1.0, prevV = 0, byMix = 0.0, bypassTarget = 0.0;
    static constexpr int kBypassFade = 256;   // session samples (5.3 ms at 48 kHz)
    bool reverse = false;
    bool pending = false;
};
} // namespace ge
