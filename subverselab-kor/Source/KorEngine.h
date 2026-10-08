// KOR engine: a clipper and warm saturator for drums and the master bus. Plain C++17, no JUCE, so the
// prototype tool, the measurements and the plugin all run this one file. See SPEC.md, "Engine".
//
// Per channel, per sample at the base rate:
//   x * drive / ceiling -> TONE pre-emphasis (first-order tilt, 1 kHz pivot) -> 8x up (three 2x half-band
//   FIR stages) -> ADAA-1 shaper f = (1-c) tanh + c knee-clip -> 8x down (same FIR) -> exact de-emphasis
//   -> * ceiling -> static auto-gain -> safety hard limit +-ceiling -> MIX against the delayed input
//   -> IN crossfade (20 ms) against the delayed input.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
#include <xmmintrin.h>
#define KOR_SSE 1
#endif
#ifndef M_PI   // MSVC does not define it without _USE_MATH_DEFINES
#define M_PI 3.14159265358979323846
#endif

namespace kor
{
struct Params
{
    float driveDb   = 6.0f;    // 0 .. 24 dB
    float character = 0.35f;   // 0 .. 1 (0 round tanh, 1 firm knee clip)
    float toneDb    = 0.0f;    // -6 .. +6 dB tilt, undone after the shaper
    float ceilingDb = -0.3f;   // -12 .. 0 dBFS
    float mix       = 1.0f;    // 0 .. 1
    bool  autoGain  = true;
    bool  in        = true;
};

// ---------------------------------------------------------------------------------------------
// Half-band FIR, designed by tools/design_halfband.py (scipy 1.17.1 remez, exact half-band construction).
// N = 91 taps (centre 0.5, every second tap zero), group delay 45 samples at the rate it runs at.
// Passband 0 .. 0.2083 fs, stopband 0.2917 .. 0.5 fs (20 / 28 kHz at 96 kHz).
// Stopband attenuation 122.7 dB (float32 taps: 122.5 dB), passband ripple 6.38e-06 dB (float32: 6.52e-06 dB).
static constexpr int kHbTaps = 91;
static constexpr int kHbSide = 46;   // non-zero side taps h[0], h[2], ..., h[N-1]
static constexpr float kHbCoef[kHbSide] = {
     2.307502522e-06f, -8.263886083e-06f,  2.231225678e-05f, -5.095783258e-05f,
     1.041118064e-04f, -1.959602667e-04f,  3.459146517e-04f, -5.795469651e-04f,
     9.294999232e-04f, -1.436413322e-03f,  2.150013698e-03f, -3.130696234e-03f,
     4.452229526e-03f, -6.206780354e-03f,  8.514544373e-03f, -1.154263846e-02f,
     1.554342948e-02f, -2.093704869e-02f,  2.850643386e-02f, -3.992907789e-02f,
     5.958646316e-02f, -1.036103202e-01f,  3.174708070e-01f,  3.174708070e-01f,
    -1.036103202e-01f,  5.958646316e-02f, -3.992907789e-02f,  2.850643386e-02f,
    -2.093704869e-02f,  1.554342948e-02f, -1.154263846e-02f,  8.514544373e-03f,
    -6.206780354e-03f,  4.452229526e-03f, -3.130696234e-03f,  2.150013698e-03f,
    -1.436413322e-03f,  9.294999232e-04f, -5.795469651e-04f,  3.459146517e-04f,
    -1.959602667e-04f,  1.041118064e-04f, -5.095783258e-05f,  2.231225678e-05f,
    -8.263886083e-06f,  2.307502522e-06f,
};
static constexpr int kHb2Taps = 31, kHb2Side = 16;
static constexpr float kHb2Coef[kHb2Side] = {
    -3.463000487e-05f, 3.170923305e-04f, -1.560829405e-03f, 5.446159507e-03f,
    -1.517630813e-02f, 3.679890585e-02f, -8.733184899e-02f, 3.115413860e-01f,
     3.115413860e-01f, -8.733184899e-02f, 3.679890585e-02f, -1.517630813e-02f,
     5.446159507e-03f, -1.560829405e-03f, 3.170923305e-04f, -3.463000487e-05f,
};
static constexpr int kHb3Taps = 31, kHb3Side = 16;
static constexpr float kHb3Coef[kHb3Side] = {
    -5.705126186e-06f, 8.528689793e-05f, -6.230761013e-04f, 2.977761209e-03f,
    -1.058498233e-02f, 3.067429080e-02f, -8.182573042e-02f, 3.093021551e-01f,
     3.093021551e-01f, -8.182573042e-02f, 3.067429080e-02f, -1.058498233e-02f,
     2.977761209e-03f, -6.230761013e-04f, 8.528689793e-05f, -5.705126186e-06f,
};
// ---------------------------------------------------------------------------------------------

static constexpr int kHbDelay = (kHbTaps - 1) / 2;
static_assert (kHbTaps % 4 == 3 && kHbSide * 2 == kHbTaps + 1, "half-band length must be 4m - 1");
static_assert (kHb2Taps % 4 == 3 && kHb2Side * 2 == kHb2Taps + 1, "half-band length must be 4m - 1");
static_assert (kHb3Taps % 4 == 3 && kHb3Side * 2 == kHb3Taps + 1, "half-band length must be 4m - 1");

// Latency is padded to one integer base-rate sample count at every sample rate and setting.
static constexpr double kHeadroom = 0.891250938133746;   // -1.0 dB (was -0.6: drums 5 dB over a -6 dB ceiling gave true peak +0.74 dB, M6)
static constexpr int kLatency = 57;
static_assert (kLatency == 57, "latency must be a whole number of base-rate samples");

// ---------------------------------------------------------------------------------------------
// Static auto-gain, by tools/make_autogain.py: dB to add for (DRIVE, CHARACTER), so that pink noise at
// -18 dBFS RMS keeps the loudness (BS.1770) it has at DRIVE 0 with the same CHARACTER.
static constexpr int   kAgNumDrive = 13;      // DRIVE 0, 2, ..., 24 dB
static constexpr int   kAgNumChar  = 5;       // CHARACTER 0, 0.25, ..., 1
static constexpr float kAgDriveStep = 2.0f, kAgCharStep = 0.25f;
static constexpr float kAutoGainDb[5][13] = {
    { 0.000000f, -1.902293f, -3.755366f, -5.539461f, -7.231381f, -8.806892f, -10.244226f, -11.527513f, -12.648809f, -13.607931f, -14.410184f, -15.064574f, -15.583821f },
    { 0.000000f, -1.926746f, -3.816580f, -5.653385f, -7.411122f, -9.049864f, -10.527722f, -11.820136f, -12.923857f, -13.848164f, -14.607189f, -15.216492f, -15.693412f },
    { 0.000000f, -1.951189f, -3.877739f, -5.766944f, -7.589093f, -9.288057f, -10.803357f, -12.103853f, -13.191229f, -14.082846f, -14.800494f, -15.365943f, -15.801365f },
    { 0.000000f, -1.975610f, -3.938793f, -5.879983f, -7.765012f, -9.521237f, -11.071136f, -12.378751f, -13.450644f, -14.311041f, -14.988640f, -15.511371f, -15.906410f },
    { 0.000000f, -1.999999f, -3.999692f, -5.992366f, -7.938647f, -9.749252f, -11.331089f, -12.644652f, -13.701013f, -14.530751f, -15.169060f, -15.650424f, -16.006608f },
};
// ---------------------------------------------------------------------------------------------

/** Bilinear lookup in kAutoGainDb. */
inline float autoGainDb (float driveDb, float character) noexcept
{
    const float fd = std::clamp (driveDb / kAgDriveStep, 0.0f, (float) (kAgNumDrive - 1));
    const float fc = std::clamp (character / kAgCharStep, 0.0f, (float) (kAgNumChar - 1));
    const int i = std::min ((int) fd, kAgNumDrive - 2), j = std::min ((int) fc, kAgNumChar - 2);
    const float u = fd - (float) i, v = fc - (float) j;
    const float a = kAutoGainDb[j][i] + u * (kAutoGainDb[j][i + 1] - kAutoGainDb[j][i]);
    const float b = kAutoGainDb[j + 1][i] + u * (kAutoGainDb[j + 1][i + 1] - kAutoGainDb[j + 1][i]);
    return a + v * (b - a);
}

// The shaper. c = CHARACTER (0 .. 1). k(x) is a hard clip at +-1 with a quadratic knee of half-width 0.15:
// linear below 0.85, flat (1) above 1.15, k(x) = x - (x - 0.85)^2 / 0.6 between (C1 at both ends).
static constexpr double kKneeW = 0.15;
static constexpr double kKneeLo = 1.0 - kKneeW, kKneeHi = 1.0 + kKneeW;
static constexpr double kKneeTop = kKneeHi * kKneeHi / 2.0 - (2.0 * kKneeW) * (2.0 * kKneeW) * (2.0 * kKneeW) / (12.0 * kKneeW);   // K(1.15)
static constexpr double kLn2 = 0.69314718055994530942;

inline double kneeClip (double x) noexcept
{
    const double a = std::abs (x);
    double y;
    if (a <= kKneeLo)      y = a;
    else if (a >= kKneeHi) y = 1.0;
    else                   { const double t = a - kKneeLo; y = a - t * t / (4.0 * kKneeW); }
    return x < 0.0 ? -y : y;
}
/** Antiderivative of kneeClip, K(0) = 0 (even). */
inline double kneeClipInt (double x) noexcept
{
    const double a = std::abs (x);
    if (a <= kKneeLo) return 0.5 * a * a;
    if (a >= kKneeHi) return kKneeTop + (a - kKneeHi);
    const double t = a - kKneeLo;
    return 0.5 * a * a - t * t * t / (12.0 * kKneeW);
}
/** Antiderivative of tanh: log(cosh(x)), without overflow. */
inline double fastLog1p (double x) noexcept
{
    // x is exp(-2|x|), hence 0 <= x <= 1. The atanh series is accurate to about 1e-7 here
    // and avoids a libm log call at every 8x ADAA sample.
    const double y = x / (2.0 + x), y2 = y * y;
    return 2.0 * y * (1.0 + y2 * (1.0 / 3.0 + y2 * (1.0 / 5.0 + y2 * (1.0 / 7.0 + y2 * (1.0 / 9.0 + y2 / 11.0)))));
}
inline double logCosh (double x) noexcept
{
    // Small |x|: log cosh is ~x^2/2, and a + log1p(e^-2a) - ln2 would lose it to cancellation (the 1e-7
    // error of fastLog1p is 0.2 % of the value at -40 dBFS and showed as THD -79 dB in M4). The Taylor
    // series to x^12 is exact to double precision below 0.25.
    const double a = std::abs (x);
    if (a < 0.25)
    {
        const double q = a * a;
        return q * (1.0 / 2.0 + q * (-1.0 / 12.0 + q * (1.0 / 45.0 + q * (-17.0 / 2520.0 + q * (31.0 / 14175.0 + q * (-691.0 / 935550.0))))));
    }
    return a > 20.0 ? a - kLn2 : a + fastLog1p (std::exp (-2.0 * a)) - kLn2;
}

class Engine
{
public:
    static constexpr int kMaxChannels = 2;

    void prepare (double sampleRate, int maxBlock, int channels)
    {
        fs = sampleRate > 0.0 ? sampleRate : 48000.0;
        maxBlockSize = std::max (1, maxBlock);
        numCh = std::clamp (channels, 1, kMaxChannels);
        smoothA = 1.0f - (float) std::exp (-1.0 / (0.020 * fs));
        fadeStep = (float) (1.0 / (0.020 * fs));
        tanW = std::tan (M_PI * 1000.0 / fs);
        reset();
    }

    void reset() noexcept
    {
        chans.reset();
        for (auto& p : postLine) p = {};
        postPos = 0;
        first = true;
        for (int c = 0; c < kMaxChannels; ++c) inPeak[c] = outPeak[c] = 0.0f;
        lamp = false;
    }

    void setParams (const Params& p) noexcept
    {
        tDrive = sane (p.driveDb, 0.0f, 24.0f, 6.0f);
        tChar  = sane (p.character, 0.0f, 1.0f, 0.35f);
        tTone  = sane (p.toneDb, -6.0f, 6.0f, 0.0f);
        tCeil  = sane (p.ceilingDb, -12.0f, 0.0f, -0.3f);
        tMix   = sane (p.mix, 0.0f, 1.0f, 1.0f);
        tAuto  = p.autoGain ? 1.0f : 0.0f;
        tIn    = p.in ? 1.0f : 0.0f;
    }

    static constexpr int latencySamples() noexcept { return kLatency; }

    float lastInPeak (int ch) const noexcept  { return ch >= 0 && ch < kMaxChannels ? inPeak[ch] : 0.0f; }
    float lastOutPeak (int ch) const noexcept { return ch >= 0 && ch < kMaxChannels ? outPeak[ch] : 0.0f; }
    bool clipLamp() const noexcept { return lamp; }

    void process (float* const* ch, int numChannels, int numSamples) noexcept
    {
        const FlushDenormals ftz;
        numChannels = std::clamp (numChannels, 1, kMaxChannels);
        if (ch == nullptr || numSamples <= 0) return;

        if (first)
        {
            sDrive = tDrive; sChar = tChar; sTone = tTone; sCeil = tCeil; sMix = tMix; sAuto = tAuto; sIn = tIn;
            derive();
            for (auto& p : postLine) p = cur;
            first = false;
        }

        float pkIn[kMaxChannels] {}, pkOut[kMaxChannels] {}, pkShaper[kMaxChannels] {};
        for (int i = 0; i < numSamples; ++i)
        {
            bool moved = false;
            moved |= glide (sDrive, tDrive, 1.0e-4f);
            moved |= glide (sChar,  tChar,  1.0e-6f);
            moved |= glide (sTone,  tTone,  1.0e-4f);
            moved |= glide (sCeil,  tCeil,  1.0e-5f);
            moved |= glide (sAuto,  tAuto,  1.0e-6f);
            glide (sMix, tMix, 1.0e-6f);
            if (sIn != tIn) sIn = tIn > sIn ? std::min (tIn, sIn + fadeStep) : std::max (tIn, sIn - fadeStep);
            if (moved) derive();

            const Post post = postLine[postPos];
            postLine[postPos] = cur;
            const double c = (double) sChar;
            const double invC = 1.0 - c;
            const float wetMult = post.ceil * post.gainAuto * (float) kHeadroom;

            if (numChannels == 2)
            {
                float v0 = ch[0][i];
                float v1 = ch[1][i];
                v0 = std::isfinite (v0) ? std::clamp (v0, -100.0f, 100.0f) : 0.0f;
                v1 = std::isfinite (v1) ? std::clamp (v1, -100.0f, 100.0f) : 0.0f;
                pkIn[0] = std::max (pkIn[0], std::abs (v0));
                pkIn[1] = std::max (pkIn[1], std::abs (v1));

                const float x0 = v0 * cur.gainIn;
                const float x1 = v1 * cur.gainIn;
                const float e0 = cur.b0 * x0 + (cur.b1 * chans.preX1[0] - cur.a1 * chans.preY1[0]);
                const float e1 = cur.b0 * x1 + (cur.b1 * chans.preX1[1] - cur.a1 * chans.preY1[1]);
                chans.preX1[0] = x0; chans.preY1[0] = flush (e0);
                chans.preX1[1] = x1; chans.preY1[1] = flush (e1);

                float a0_0, a1_0, a0_1, a1_1;
                chans.up1.push2 (chans.preY1[0], chans.preY1[1], a0_0, a1_0, a0_1, a1_1);

                float w0[2], w1[2];
                for (int h = 0; h < 2; ++h)
                {
                    float b0_0, b1_0, b0_1, b1_1;
                    chans.up2.push2 (h == 0 ? a0_0 : a1_0, h == 0 ? a0_1 : a1_1, b0_0, b1_0, b0_1, b1_1);

                    float d0_0, d1_0, d0_1, d1_1;
                    chans.up3.push2 (b0_0, b0_1, d0_0, d1_0, d0_1, d1_1);
                    pkShaper[0] = std::max (pkShaper[0], std::max (std::abs (d0_0), std::abs (d1_0)));
                    pkShaper[1] = std::max (pkShaper[1], std::max (std::abs (d0_1), std::abs (d1_1)));

                    float e0_0, e1_0, e0_1, e1_1;
                    chans.up3.push2 (b1_0, b1_1, e0_0, e1_0, e0_1, e1_1);

                    const float t0_0 = chans.shape (0, d0_0, c, invC), t1_0 = chans.shape (0, d1_0, c, invC);
                    const float t0_1 = chans.shape (1, d0_1, c, invC), t1_1 = chans.shape (1, d1_1, c, invC);
                    
                    const float t2_0 = chans.shape (0, e0_0, c, invC), t3_0 = chans.shape (0, e1_0, c, invC);
                    const float t2_1 = chans.shape (1, e0_1, c, invC), t3_1 = chans.shape (1, e1_1, c, invC);

                    float q0_0, q0_1, q1_0, q1_1;
                    chans.down3.push2 (t0_0, t1_0, t0_1, t1_1, q0_0, q0_1);
                    chans.down3.push2 (t2_0, t3_0, t2_1, t3_1, q1_0, q1_1);

                    chans.down2.push2 (q0_0, q1_0, q0_1, q1_1, w0[h], w1[h]);
                }

                float z0, z1;
                chans.down1.push2 (chans.pad[0], w0[0], chans.pad[1], w1[0], z0, z1);
                chans.pad[0] = w0[1];
                chans.pad[1] = w1[1];

                const float d0 = (z0 - (post.b1 * chans.deX1[0] - post.a1 * chans.deZ1[0])) * post.invB0;
                const float d1 = (z1 - (post.b1 * chans.deX1[1] - post.a1 * chans.deZ1[1])) * post.invB0;
                chans.deX1[0] = flush (d0); chans.deZ1[0] = z0;
                chans.deX1[1] = flush (d1); chans.deZ1[1] = z1;

                float wet0 = chans.deX1[0] * wetMult;
                float wet1 = chans.deX1[1] * wetMult;
                wet0 = std::clamp (wet0, -post.ceil, post.ceil);
                wet1 = std::clamp (wet1, -post.ceil, post.ceil);

                float& slot0 = chans.dry[0][postPos];
                float& slot1 = chans.dry[1][postPos];
                const float dry0 = slot0;
                const float dry1 = slot1;
                slot0 = v0;
                slot1 = v1;

                const float mixed0 = sMix >= 1.0f ? wet0 : (sMix <= 0.0f ? dry0 : dry0 + sMix * (wet0 - dry0));
                const float mixed1 = sMix >= 1.0f ? wet1 : (sMix <= 0.0f ? dry1 : dry1 + sMix * (wet1 - dry1));
                const float out0 = sIn >= 1.0f ? mixed0 : (sIn <= 0.0f ? dry0 : dry0 + sIn * (mixed0 - dry0));
                const float out1 = sIn >= 1.0f ? mixed1 : (sIn <= 0.0f ? dry1 : dry1 + sIn * (mixed1 - dry1));
                ch[0][i] = out0;
                ch[1][i] = out1;
                pkOut[0] = std::max (pkOut[0], std::abs (out0));
                pkOut[1] = std::max (pkOut[1], std::abs (out1));
            }
            else
            {
                for (int k = 0; k < numChannels; ++k)
                {
                    float v = ch[k][i];
                    v = std::isfinite (v) ? std::clamp (v, -100.0f, 100.0f) : 0.0f;
                    pkIn[k] = std::max (pkIn[k], std::abs (v));

                    const float x = v * cur.gainIn;
                    const float e = cur.b0 * x + (cur.b1 * chans.preX1[k] - cur.a1 * chans.preY1[k]);
                    chans.preX1[k] = x; chans.preY1[k] = flush (e);

                    float a0, a1;
                    chans.up1.push (k, chans.preY1[k], a0, a1);
                    float w[2];
                    for (int h = 0; h < 2; ++h)
                    {
                        float b0, b1;
                        chans.up2.push (k, h == 0 ? a0 : a1, b0, b1);
                        float d0, d1;
                        chans.up3.push (k, b0, d0, d1);
                        pkShaper[k] = std::max (pkShaper[k], std::max (std::abs (d0), std::abs (d1)));
                        float e0, e1;
                        chans.up3.push (k, b1, e0, e1);
                        const float t0 = chans.shape (k, d0, c, invC), t1 = chans.shape (k, d1, c, invC);
                        const float t2 = chans.shape (k, e0, c, invC), t3 = chans.shape (k, e1, c, invC);
                        const float q0 = chans.down3.push (k, t0, t1);
                        const float q1 = chans.down3.push (k, t2, t3);
                        w[h] = chans.down2.push (k, q0, q1);
                    }
                    const float z = chans.down1.push (k, chans.pad[k], w[0]);
                    chans.pad[k] = w[1];

                    const float d = (z - (post.b1 * chans.deX1[k] - post.a1 * chans.deZ1[k])) * post.invB0;
                    chans.deX1[k] = flush (d); chans.deZ1[k] = z;

                    float wet = chans.deX1[k] * wetMult;
                    wet = std::clamp (wet, -post.ceil, post.ceil);

                    float& slot = chans.dry[k][postPos];
                    const float dry = slot;
                    slot = v;
                    const float mixed = sMix >= 1.0f ? wet : (sMix <= 0.0f ? dry : dry + sMix * (wet - dry));
                    const float out = sIn >= 1.0f ? mixed : (sIn <= 0.0f ? dry : dry + sIn * (mixed - dry));
                    ch[k][i] = out;
                    pkOut[k] = std::max (pkOut[k], std::abs (out));
                }
            }
            if (++postPos == kLatency) postPos = 0;
        }

        bool lit = false;
        for (int k = 0; k < kMaxChannels; ++k)
        {
            inPeak[k] = k < numChannels ? pkIn[k] : 0.0f;
            outPeak[k] = k < numChannels ? pkOut[k] : 0.0f;
            if (k < numChannels && pkShaper[k] > 1.0e-6f)
            {
                const double x = pkShaper[k], c = sChar;
                const double y = (1.0 - c) * std::tanh (x) + c * kneeClip (x);
                lit |= y < x * 0.89125093813;
            }
        }
        lamp = lit;
    }

private:
    struct FlushDenormals
    {
#if defined(__aarch64__)
        uint64_t saved = 0;
        FlushDenormals() noexcept
        {
            asm volatile ("mrs %0, fpcr" : "=r" (saved));
            const uint64_t fz = saved | (uint64_t (1) << 24);
            asm volatile ("msr fpcr, %0" : : "r" (fz));
        }
        ~FlushDenormals() noexcept { asm volatile ("msr fpcr, %0" : : "r" (saved)); }
#elif defined(KOR_SSE)
        unsigned int saved = 0;
        FlushDenormals() noexcept { saved = _mm_getcsr(); _mm_setcsr (saved | 0x8040u); }
        ~FlushDenormals() noexcept { _mm_setcsr (saved); }
#else
        FlushDenormals() noexcept {}
#endif
    };

    static inline float sane (float v, float lo, float hi, float fallback) noexcept
    {
        return std::isfinite (v) ? std::clamp (v, lo, hi) : fallback;
    }
    static inline float flush (float v) noexcept { return std::abs (v) < 1.0e-30f ? 0.0f : v; }

    inline bool glide (float& s, float t, float eps) const noexcept
    {
        if (s == t) return false;
        s += smoothA * (t - s);
        if (std::abs (t - s) < eps) s = t;
        return true;
    }

    void derive() noexcept
    {
        const double ceil = std::pow (10.0, (double) sCeil / 20.0);
        cur.gainIn = (float) (std::pow (10.0, (double) sDrive / 20.0) / (ceil * kHeadroom));
        cur.ceil = (float) ceil;
        const double ag = (double) (autoGainDb (sDrive, sChar) * sAuto);
        cur.gainAuto = ag == 0.0 ? 1.0f : (float) std::pow (10.0, ag / 20.0);
        const double G = std::pow (10.0, (double) sTone / 40.0), W = tanW;
        const double n = 1.0 + G * W;
        const double b0 = (G + W) / n, b1 = (W - G) / n, a1 = (G * W - 1.0) / n;
        cur.b0 = (float) b0; cur.b1 = (float) b1; cur.a1 = (float) a1;
        cur.invB0 = (float) (1.0 / b0);
        if (sTone == 0.0f) { cur.b0 = 1.0f; cur.invB0 = 1.0f; cur.a1 = cur.b1; }
    }

    template <int Start, int End>
    struct DotTree {
        template <const float* Coef>
        static inline void run2(const float buf[][2], int pos, float& s0, float& s1) noexcept {
            constexpr int Mid = Start + (End - Start) / 2;
            float l0, l1, r0, r1;
            DotTree<Start, Mid>::template run2<Coef>(buf, pos, l0, l1);
            DotTree<Mid, End>::template run2<Coef>(buf, pos, r0, r1);
            s0 = l0 + r0;
            s1 = l1 + r1;
        }
        template <const float* Coef>
        static inline void run1(const float buf[][2], int pos, int k, float& s) noexcept {
            constexpr int Mid = Start + (End - Start) / 2;
            float l, r;
            DotTree<Start, Mid>::template run1<Coef>(buf, pos, k, l);
            DotTree<Mid, End>::template run1<Coef>(buf, pos, k, r);
            s = l + r;
        }
    };

    template <int Start>
    struct DotTree<Start, Start + 1> {
        template <const float* Coef>
        static inline void run2(const float buf[][2], int pos, float& s0, float& s1) noexcept {
            s0 = Coef[Start] * buf[pos + Start][0];
            s1 = Coef[Start] * buf[pos + Start][1];
        }
        template <const float* Coef>
        static inline void run1(const float buf[][2], int pos, int k, float& s) noexcept {
            s = Coef[Start] * buf[pos + Start][k];
        }
    };

    template <int Start>
    struct DotTree<Start, Start + 2> {
        template <const float* Coef>
        static inline void run2(const float buf[][2], int pos, float& s0, float& s1) noexcept {
            s0 = Coef[Start] * buf[pos + Start][0] + Coef[Start + 1] * buf[pos + Start + 1][0];
            s1 = Coef[Start] * buf[pos + Start][1] + Coef[Start + 1] * buf[pos + Start + 1][1];
        }
        template <const float* Coef>
        static inline void run1(const float buf[][2], int pos, int k, float& s) noexcept {
            s = Coef[Start] * buf[pos + Start][k] + Coef[Start + 1] * buf[pos + Start + 1][k];
        }
    };

    template <int Start>
    struct DotTree<Start, Start + 3> {
        template <const float* Coef>
        static inline void run2(const float buf[][2], int pos, float& s0, float& s1) noexcept {
            s0 = (Coef[Start] * buf[pos + Start][0] + Coef[Start + 1] * buf[pos + Start + 1][0]) + Coef[Start + 2] * buf[pos + Start + 2][0];
            s1 = (Coef[Start] * buf[pos + Start][1] + Coef[Start + 1] * buf[pos + Start + 1][1]) + Coef[Start + 2] * buf[pos + Start + 2][1];
        }
        template <const float* Coef>
        static inline void run1(const float buf[][2], int pos, int k, float& s) noexcept {
            s = (Coef[Start] * buf[pos + Start][k] + Coef[Start + 1] * buf[pos + Start + 1][k]) + Coef[Start + 2] * buf[pos + Start + 2][k];
        }
    };

    template <int Start>
    struct DotTree<Start, Start + 4> {
        template <const float* Coef>
        static inline void run2(const float buf[][2], int pos, float& s0, float& s1) noexcept {
            s0 = (Coef[Start] * buf[pos + Start][0] + Coef[Start + 1] * buf[pos + Start + 1][0]) + (Coef[Start + 2] * buf[pos + Start + 2][0] + Coef[Start + 3] * buf[pos + Start + 3][0]);
            s1 = (Coef[Start] * buf[pos + Start][1] + Coef[Start + 1] * buf[pos + Start + 1][1]) + (Coef[Start + 2] * buf[pos + Start + 2][1] + Coef[Start + 3] * buf[pos + Start + 3][1]);
        }
        template <const float* Coef>
        static inline void run1(const float buf[][2], int pos, int k, float& s) noexcept {
            s = (Coef[Start] * buf[pos + Start][k] + Coef[Start + 1] * buf[pos + Start + 1][k]) + (Coef[Start + 2] * buf[pos + Start + 2][k] + Coef[Start + 3] * buf[pos + Start + 3][k]);
        }
    };

    template<int Side, int Half, const float* Coef>
    struct HbUp
    {
        float buf[2 * Side][2] {};
        int pos = 0;
        void reset() noexcept { std::fill (&buf[0][0], &buf[2 * Side][0], 0.0f); pos = 0; }
        inline void push (int k, float x, float& even, float& odd) noexcept
        {
            if (k == 0) pos = (pos == 0 ? Side : pos) - 1;
            buf[pos][k] = buf[pos + Side][k] = x;
            float s;
            DotTree<0, Side>::template run1<Coef>(buf, pos, k, s);
            even = 2.0f * s;
            odd = buf[pos + Half - 1][k];
        }
        inline void push2 (float x0, float x1, float& even0, float& odd0, float& even1, float& odd1) noexcept
        {
            pos = (pos == 0 ? Side : pos) - 1;
            buf[pos][0] = buf[pos + Side][0] = x0;
            buf[pos][1] = buf[pos + Side][1] = x1;
            float s0, s1;
            DotTree<0, Side>::template run2<Coef>(buf, pos, s0, s1);
            even0 = 2.0f * s0;
            even1 = 2.0f * s1;
            odd0 = buf[pos + Half - 1][0];
            odd1 = buf[pos + Half - 1][1];
        }
    };

    template<int Side, int Half, const float* Coef>
    struct HbDown
    {
        float ev[2 * Side][2] {};
        float od[Half][2] {};
        int pos = 0, opos = 0;
        void reset() noexcept
        {
            std::fill (&ev[0][0], &ev[2 * Side][0], 0.0f);
            std::fill (&od[0][0], &od[Half][0], 0.0f);
            pos = opos = 0;
        }
        inline float push (int k, float uEven, float uOdd) noexcept
        {
            if (k == 0) pos = (pos == 0 ? Side : pos) - 1;
            ev[pos][k] = ev[pos + Side][k] = uEven;
            const float oldOdd = od[opos][k];
            od[opos][k] = uOdd;
            if (k == 1 || kMaxChannels == 1) { if (++opos == Half) opos = 0; }
            float s;
            DotTree<0, Side>::template run1<Coef>(ev, pos, k, s);
            return s + 0.5f * oldOdd;
        }
        inline void push2 (float uEven0, float uOdd0, float uEven1, float uOdd1, float& out0, float& out1) noexcept
        {
            pos = (pos == 0 ? Side : pos) - 1;
            ev[pos][0] = ev[pos + Side][0] = uEven0;
            ev[pos][1] = ev[pos + Side][1] = uEven1;
            const float oldOdd0 = od[opos][0];
            const float oldOdd1 = od[opos][1];
            od[opos][0] = uOdd0;
            od[opos][1] = uOdd1;
            if (++opos == Half) opos = 0;
            float s0, s1;
            DotTree<0, Side>::template run2<Coef>(ev, pos, s0, s1);
            out0 = s0 + 0.5f * oldOdd0;
            out1 = s1 + 0.5f * oldOdd1;
        }
    };

    struct Channels
    {
        using Up1 = HbUp<kHbSide, kHbSide / 2, kHbCoef>;
        using Up2 = HbUp<kHb2Side, kHb2Side / 2, kHb2Coef>;
        using Up3 = HbUp<kHb3Side, kHb3Side / 2, kHb3Coef>;
        using Down1 = HbDown<kHbSide, kHbSide / 2, kHbCoef>;
        using Down2 = HbDown<kHb2Side, kHb2Side / 2, kHb2Coef>;
        using Down3 = HbDown<kHb3Side, kHb3Side / 2, kHb3Coef>;
        Up1 up1; Up2 up2; Up3 up3;
        Down3 down3; Down2 down2; Down1 down1;
        
        float pad[kMaxChannels] {};
        float preX1[kMaxChannels] {}, preY1[kMaxChannels] {}, deX1[kMaxChannels] {}, deZ1[kMaxChannels] {};
        double x1[kMaxChannels] {}, lc1[kMaxChannels] {}, k1[kMaxChannels] {};
        float dry[kMaxChannels][kLatency] {};

        void reset() noexcept
        {
            up1.reset(); up2.reset(); up3.reset(); down3.reset(); down2.reset(); down1.reset();
            for (int k = 0; k < kMaxChannels; ++k) {
                pad[k] = preX1[k] = preY1[k] = deX1[k] = deZ1[k] = 0.0f;
                x1[k] = lc1[k] = k1[k] = 0.0;
                std::fill(std::begin(dry[k]), std::end(dry[k]), 0.0f);
            }
        }

        inline float shape (int k, float in, double c, double invC) noexcept
        {
            const double x = in;
            const double dx = x - x1[k];

            double lc = 0.0, kk = 0.0;
            double e_val = -1.0;

            if (c != 1.0)
            {
                const double a = std::abs (x);
                if (a < 0.25)
                {
                    const double q = a * a;
                    lc = q * (1.0 / 2.0 + q * (-1.0 / 12.0 + q * (1.0 / 45.0 + q * (-17.0 / 2520.0 + q * (31.0 / 14175.0 + q * (-691.0 / 935550.0))))));
                }
                else if (a > 20.0)
                {
                    lc = a - kLn2;
                }
                else
                {
                    e_val = std::exp (-2.0 * a);
                    lc = a + fastLog1p (e_val) - kLn2;
                }
            }

            if (c != 0.0) kk = kneeClipInt (x);

            double y;
            if (std::abs (dx) < 1.0e-5)
            {
                const double m = 0.5 * (x + x1[k]);
                double th = 0.0, knee = 0.0;
                if (c != 1.0)
                {
                    const double a = std::abs (m);
                    if (a < 0.25) {
                        const double q = m * m;
                        th = m * (1.0 + q * (-1.0 / 3.0 + q * (2.0 / 15.0 + q * (-17.0 / 315.0 + q * (62.0 / 2835.0 + q * (-1382.0 / 155925.0))))));
                    }
                    else if (a > 20.0) th = m < 0.0 ? -1.0 : 1.0;
                    else {
                        double eval_th = (e_val >= 0.0) ? e_val : std::exp(-2.0 * a);
                        const double ta = (1.0 - eval_th) / (1.0 + eval_th);
                        th = m < 0.0 ? -ta : ta;
                    }
                }
                if (c != 0.0) knee = kneeClip (m);
                y = invC * th + c * knee;
            }
            else
            {
                y = (invC * (lc - lc1[k]) + c * (kk - k1[k])) / dx;
            }
            
            x1[k] = x; lc1[k] = lc; k1[k] = kk;
            return (float) y;
        }
    };

    struct Post
    {
        float gainIn = 1.0f, ceil = 1.0f, gainAuto = 1.0f;
        float b0 = 1.0f, b1 = 0.0f, a1 = 0.0f, invB0 = 1.0f;
    };

    double fs = 48000.0, tanW = 0.0;
    int maxBlockSize = 512, numCh = 2;
    float smoothA = 0.0f, fadeStep = 0.0f;
    float tDrive = 6.0f, tChar = 0.35f, tTone = 0.0f, tCeil = -0.3f, tMix = 1.0f, tAuto = 1.0f, tIn = 1.0f;
    float sDrive = 6.0f, sChar = 0.35f, sTone = 0.0f, sCeil = -0.3f, sMix = 1.0f, sAuto = 1.0f, sIn = 1.0f;
    bool first = true, lamp = false;
    Post cur;
    Post postLine[kLatency] {};
    int postPos = 0;
    Channels chans;
    float inPeak[kMaxChannels] {}, outPeak[kMaxChannels] {};
};
} // namespace kor
