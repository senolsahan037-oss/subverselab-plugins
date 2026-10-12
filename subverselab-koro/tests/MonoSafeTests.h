#pragma once
// MONO SAFE tests (family batch, 2026-10-12). Needs "Source/Engine.h" (cifte::) included first.
//  - offHash(): FNV-1a of the engine output for MONO SAFE OFF (doubler only, and doubler + harmony), on fixed noise.
//    kOffHashBefore was taken from the engine as it was BEFORE the MONO SAFE change (same compiler, same flags), so a match
//    proves the OFF path is bit-identical to the old one.
//  - monoSumResidual(): MONO SAFE ON, HARMONY OFF, DOUBLE 10, WIDTH 10, pink noise: the mono sum of the output against the
//    latency-aligned mono input, in dB relative to the input mono RMS.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <vector>

namespace svl::monosafetest
{
inline std::vector<float> pinkNoise (size_t n, uint32_t seed)
{
    std::vector<float> y (n);
    uint32_t s = seed;
    float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
    for (size_t i = 0; i < n; ++i)
    {
        s = s * 1664525u + 1013904223u;
        const float w = (float) ((double) (s >> 8) / 8388608.0 - 1.0);
        b0 = 0.99886f * b0 + w * 0.0555179f;  b1 = 0.99332f * b1 + w * 0.0750759f;
        b2 = 0.96900f * b2 + w * 0.1538520f;  b3 = 0.86650f * b3 + w * 0.3104856f;
        b4 = 0.55000f * b4 + w * 0.5329522f;  b5 = -0.7616f * b5 - w * 0.0168980f;
        y[i] = 0.11f * (b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362f) * 0.5f;
        b6 = w * 0.115926f;
    }
    return y;
}

inline uint64_t fnv (uint64_t h, const std::vector<float>& v)
{
    for (float f : v)
    {
        uint32_t u; std::memcpy (&u, &f, 4);
        for (int k = 0; k < 4; ++k) { h ^= (u >> (8 * k)) & 0xffu; h *= 1099511628211ull; }
    }
    return h;
}

/** Runs the engine in 512-sample blocks; returns the output as two channels. */
inline void runEngine (const cifte::Controls& c, const std::vector<float>& inL, const std::vector<float>& inR,
                       std::vector<float>& outL, std::vector<float>& outR, int& latency)
{
    cifte::CifteEngine e;
    e.prepare (48000.0);
    latency = e.latency (c);
    outL = inL; outR = inR;
    for (size_t p = 0; p < inL.size(); p += 512)
        e.process (outL.data() + p, outR.data() + p, (int) std::min<size_t> (512, inL.size() - p), c);
}

inline uint64_t offHash()
{
    const size_t n = 48000 * 3;
    const auto a = pinkNoise (n, 11u), b = pinkNoise (n, 23u);
    std::vector<float> L (n), R (n);
    for (size_t i = 0; i < n; ++i) { L[i] = a[i]; R[i] = 0.6f * b[i] + 0.4f * a[i]; }
    uint64_t h = 1469598103934665603ull;
    for (int scenario = 0; scenario < 2; ++scenario)
    {
        cifte::Controls c;
        c.monoSafe = false;
        c.voices = 3; c.mix = 0.75f; c.width = scenario == 0 ? 1.0f : 0.38f; c.tone = 0.78f;
        c.harmony = scenario == 0 ? 0.0f : 0.5f;
        c.keyMode = cifte::KeyMode::Manual; c.root = 9; c.scale = 1;
        c.harmonyVoices = scenario == 0 ? 1 : 3;
        std::vector<float> oL, oR; int lat = 0;
        runEngine (c, L, R, oL, oR, lat);
        h = fnv (fnv (h, oL), oR);
    }
    return h;
}

/** dB of (mono sum of the output minus the latency-aligned mono input) relative to the input mono RMS; also the stereo side level. */
inline double monoSumResidualDb (double* sideDb = nullptr)
{
    const size_t n = 48000 * 4;
    const auto a = pinkNoise (n, 5u), b = pinkNoise (n, 77u);
    std::vector<float> L (n), R (n);
    for (size_t i = 0; i < n; ++i) { L[i] = a[i]; R[i] = 0.5f * b[i] + 0.5f * a[i]; }
    cifte::Controls c;
    c.monoSafe = true;
    c.voices = 3; c.mix = 0.75f; c.width = 1.0f; c.tone = 0.78f; c.harmony = 0.0f;
    std::vector<float> oL, oR; int lat = 0;
    runEngine (c, L, R, oL, oR, lat);
    double ref = 0.0, err = 0.0, side = 0.0;
    for (size_t i = (size_t) lat; i < n; ++i)
    {
        const double m = 0.5 * ((double) L[i - (size_t) lat] + R[i - (size_t) lat]);
        const double o = 0.5 * ((double) oL[i] + oR[i]);
        ref += m * m; err += (o - m) * (o - m);
        const double s = 0.5 * ((double) oL[i] - oR[i]);
        side += s * s;
    }
    if (sideDb) *sideDb = 10.0 * std::log10 (side / ref + 1.0e-30);
    return 10.0 * std::log10 (err / ref + 1.0e-30);
}
} // namespace svl::monosafetest
