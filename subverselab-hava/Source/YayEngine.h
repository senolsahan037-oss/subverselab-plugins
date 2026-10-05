#pragma once
// HAVA ("air"; prototype folder YAY) - air + spring sheen, header-only C++17.
// (History: this engine grew out of the AMFI amp rounds; the amp preamp/saturation code is kept but is
// not on the panel and is off by default - KICK = 0 means it never runs.)
//
// Front: AIR, SHEEN, OUTPUT, BYPASS. Rear: TWANG, TENSION, DRIP, LOW CUT, MASTER SAFE.
// HAVA is "glue, not a room": the tails are short springs (SHEEN tank T60 0.63 s at an amount capped at
// 0.30, AIR wash T60 0.15 s) and the dry is never reduced.
//   AIR:   a high shelf calibrated to 0..+6 dB at 11 kHz plus a very short, bright 2-lane spring wash
//          (T60 0.15 s, amount 0..0.15).
//   SHEEN: the round-4 short spring tank (Cemalim voicing: falling chirp = drip, 1.5 kHz twang, 29-35 ms
//          lanes, T60 0.63 s), tank amount 0.30*SHEEN (colour gain 0.6*SHEEN), added on top of a unity dry.
//          SHEEN 0.5 = tank 0.15 = the round-4 AIR FULL. LOW CUT high-passes the SHEEN send.
//   MASTER SAFE off: plain stereo, zero latency.
//   MASTER SAFE on:  MID-only processing (SIDE passes clean), K-weighted auto-gain on MID (loudness
//          held equal to the input), and a true-peak look-ahead ceiling at -0.3 dBTP. Latency 1.5 ms + 8
//          samples, constant while on (also in BYPASS).
// Bypass-equivalence: AIR 0 + SHEEN 0 (+ OUTPUT 0, KICK 0) with MASTER SAFE off returns without
// touching the buffers (bit-exact). BYPASS on does the same (MASTER SAFE on: delayed dry).

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>

namespace yay {

struct Controls {
    // front panel
    float air = 0.0f;        // 0 .. 1, AIR: high shelf 0..+6 dB at 11 kHz + short bright spring sheen (0..0.15)
    float sheen = 0.0f;      // 0 .. 1, SHEEN: the short spring tank (T60 0.63 s), tank amount 0..0.30; dry stays at unity
    float output = 0.0f;     // dB, -12 .. +12
    bool bypass = false;     // BYPASS
    // rear / advanced
    float tension = 0.55f;   // 0 .. 1, dispersion = chirp length
    float drip = 0.5f;       // 0 .. 1, transient drip emphasis into the sheen
    float twang = 0.7f;      // 0 .. 1, metallic 1-3 kHz resonance (0..+10 dB at 1.5 kHz)
    float lowcut = 80.0f;    // Hz 20 .. 300, high-pass on the SHEEN send (the dry is never filtered)
    bool masterSafe = false; // on: MID-only processing + auto-gain + -0.3 dBTP ceiling (adds latency)
    // legacy, NOT on the panel (AMFI amp rounds): preamp drive and presence. Default 0 = code not run.
    float kick = 0.0f;
    float presence = 0.5f;
};

// The state the plugin opens in: AIR FULL (AIR .8, SHEEN .25, LOW CUT 300, DRIP .5, MASTER SAFE off) - the owner's colour.
// (A default-constructed Controls is neutral: AIR 0 + SHEEN 0 = bit-exact dry.)
inline Controls defaultControls() {
    Controls c;
    c.air = 0.8f;
    c.sheen = 0.25f;      // 2026-10-05: AIR FULL = D (SHEEN .25, LOW CUT 300, DRIP .5), owner's pick
    c.drip = 0.5f;
    c.lowcut = 300.0f;
    c.masterSafe = false;
    return c;
}

namespace detail {
static constexpr double kPi = 3.141592653589793;

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float s1[2]{}, s2[2]{};
    static void lpCoeffs(double f, double fs, double q, double& b0, double& b1, double& b2, double& a1, double& a2) {
        const double w = 2.0 * kPi * f / fs, cw = std::cos(w), al = std::sin(w) / (2.0 * q);
        const double a0 = 1.0 + al;
        b0 = (1.0 - cw) * 0.5 / a0; b1 = (1.0 - cw) / a0; b2 = b0; a1 = -2.0 * cw / a0; a2 = (1.0 - al) / a0;
    }
    void setLP(double f, double fs, double q = 0.7071067811865476) { lpCoeffs(f, fs, q, b0, b1, b2, a1, a2); }
    void setHP(double f, double fs, double q = 0.7071067811865476) {
        const double w = 2.0 * kPi * f / fs, cw = std::cos(w), al = std::sin(w) / (2.0 * q);
        const double a0 = 1.0 + al;
        b0 = (1.0 + cw) * 0.5 / a0; b1 = -(1.0 + cw) / a0; b2 = b0; a1 = -2.0 * cw / a0; a2 = (1.0 - al) / a0;
    }
    void setBP(double f, double q, double fs) {  // constant 0 dB peak gain
        const double w = 2.0 * kPi * f / fs, cw = std::cos(w), al = std::sin(w) / (2.0 * q);
        const double a0 = 1.0 + al;
        b0 = al / a0; b1 = 0.0; b2 = -al / a0; a1 = -2.0 * cw / a0; a2 = (1.0 - al) / a0;
    }
    void setPeak(double f, double q, double dB, double fs) {
        const double A = std::pow(10.0, dB / 40.0), w = 2.0 * kPi * f / fs;
        const double cw = std::cos(w), al = std::sin(w) / (2.0 * q), a0 = 1.0 + al / A;
        b0 = (1.0 + al * A) / a0; b1 = -2.0 * cw / a0; b2 = (1.0 - al * A) / a0; a1 = b1; a2 = (1.0 - al / A) / a0;
    }
    void setHighShelf(double f, double dB, double fs) {  // RBJ, slope S = 1
        const double A = std::pow(10.0, dB / 40.0), w = 2.0 * kPi * f / fs;
        const double cw = std::cos(w), al = std::sin(w) / 2.0 * std::sqrt(2.0), sA = 2.0 * std::sqrt(A) * al;
        const double a0 = (A + 1) - (A - 1) * cw + sA;
        b0 = A * ((A + 1) + (A - 1) * cw + sA) / a0;
        b1 = -2 * A * ((A - 1) + (A + 1) * cw) / a0;
        b2 = A * ((A + 1) + (A - 1) * cw - sA) / a0;
        a1 = 2 * ((A - 1) - (A + 1) * cw) / a0;
        a2 = ((A + 1) - (A - 1) * cw - sA) / a0;
    }
    double magDb(double f, double fs) const {
        const std::complex<double> z1 = std::polar(1.0, -2.0 * kPi * f / fs), z2 = z1 * z1;
        return 20.0 * std::log10(std::abs((b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2)));
    }
    float run(float x, int ch = 0) {
        const float y = float(b0) * x + s1[ch];
        s1[ch] = float(b1) * x - float(a1) * y + s2[ch];
        s2[ch] = float(b2) * x - float(a2) * y;
        return y;
    }
};

static inline float clampf(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }
}  // namespace detail

// BS.1770 K-weighting pre-filter (high shelf + RLB high-pass), any sample rate.
struct KWeight {
    detail::Biquad sh, hp;
    void setup(double fs) {
        {
            const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
            const double K = std::tan(detail::kPi * f0 / fs), Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
            const double a0 = 1.0 + K / Q + K * K;
            sh.b0 = (Vh + Vb * K / Q + K * K) / a0; sh.b1 = 2.0 * (K * K - Vh) / a0; sh.b2 = (Vh - Vb * K / Q + K * K) / a0;
            sh.a1 = 2.0 * (K * K - 1.0) / a0; sh.a2 = (1.0 - K / Q + K * K) / a0;
        }
        {
            const double f0 = 38.13547087602444, Q = 0.5003270373238773;
            const double K = std::tan(detail::kPi * f0 / fs), a0 = 1.0 + K / Q + K * K;
            hp.b0 = 1.0; hp.b1 = -2.0; hp.b2 = 1.0; hp.a1 = 2.0 * (K * K - 1.0) / a0; hp.a2 = (1.0 - K / Q + K * K) / a0;
        }
    }
    void reset() { for (int c = 0; c < 2; ++c) { sh.s1[c] = sh.s2[c] = hp.s1[c] = hp.s2[c] = 0.0f; } }
    float run(float x, int ch = 0) { return hp.run(sh.run(x, ch), ch); }
};

// ---------------------------------------------------------------------------------------------
// SHEEN engine = the round-4 short spring tank (Cemalim voicing): 4 lanes x 160 dispersive sections, short
// lanes 29.3-34.6 ms, T60 0.6 s design (0.63 s measured), loop output only. Mono in, stereo out.
// (Round 5 tried a direct-chirp 0.12 s version; the owner chose this tail: "the snare sticks to the floor
// from the air, like two glued sheets of paper being peeled apart".)
class SpringTank {
public:
    static constexpr int kLanes = 4;
    static constexpr int kSections = 160;

    void reset() {
        std::memset(buf_, 0, sizeof(buf_));
        std::memset(ap_, 0, sizeof(ap_));
        std::memset(lpS1_, 0, sizeof(lpS1_));
        std::memset(lpS2_, 0, sizeof(lpS2_));
        std::memset(dcX_, 0, sizeof(dcX_));
        std::memset(dcY_, 0, sizeof(dcY_));
        std::memset(fb_, 0, sizeof(fb_));
        for (auto& b : inBq_) b = detail::Biquad{};
        for (auto& b : outBq_) b = detail::Biquad{};
        shelf_ = detail::Biquad{};
        clankBp_ = detail::Biquad{};
        pos_ = 0;
        phase_ = 0.0;
        dripLp_ = dripFast_ = dripSlow_ = dripGate_ = 0.0f;
        kFast_ = kSlow_ = clankEnv_ = 0.0f;
        clankPeak_ = 0.0f;
        rng_ = 0x9E3779B9u;
    }
    float clankPeak() const { return clankPeak_; }
    void setClankEnabled(bool on) { clankOn_ = on; }

    // block-constant setup; colG = colour gain (0..1)
    void setup(const Controls& c, double sr, float colG) {
        using detail::clampf;
        sr_ = sr;
        const float fs = float(sr);
        const float kick = clampf(c.kick, 0.0f, 1.0f);
        const float tension = clampf(c.tension, 0.0f, 1.0f);
        drip_ = clampf(c.drip, 0.0f, 1.0f);
        const float twang = clampf(c.twang, 0.0f, 1.0f);
        const float a44 = 0.64f + 0.22f * tension;
        for (int s = 0; s < kLanes; ++s) {
            const float mag = 1.0f - (1.0f - a44 * kLaneA[s]) * (44100.0f / fs);
            a_[s] = -clampf(mag, 0.0f, 0.995f);
        }
        inBq_[0].setHP(70.0, sr);
        inBq_[1].setLP(std::min(6000.0, 0.45 * sr), sr);
        shelf_.setHighShelf(2500.0, 3.0 * drip_, sr);
        clankBp_.setBP(std::min(3000.0, 0.45 * sr), 1.8, sr);
        outBq_[0].setHP(80.0, sr);
        outBq_[1].setLP(std::min(kWetLpHz, 0.45 * sr), sr);
        outBq_[2].setHP(40.0, sr);
        outBq_[3].setPeak(std::min(kTwangHz, 0.45 * sr), kTwangQ, kTwangDb * twang, sr);
        detail::Biquad::lpCoeffs(std::min(kLoopLpHz, 0.45 * sr), sr, 0.7071067811865476, lb0_, lb1_, lb2_, la1_, la2_);
        dcR_ = std::exp(-2.0f * 3.14159265f * 30.0f / fs);
        const float t60 = kSheenT60;           // the round-4 short tank (Cemalim voicing)
        const float* laneMs = kLaneMsShort;
        for (int s = 0; s < kLanes; ++s) {
            D_[s] = laneMs[s] * 0.001f * fs;
            g_[s] = std::pow(10.0f, -3.0f * (D_[s] + 1.5f) / (t60 * kDecayTrim * fs));
        }
        const float gainNorm = std::sqrt(std::max(1e-6f, 1.0f - g_[0] * g_[0]));
        wet_ = colG * kWetGain * gainNorm;
        modDepth_ = 0.00010f * fs;
        pre_ = 0.05f * std::pow(160.0f, kick);
        bias_ = 0.35f * kick;
        tb_ = std::tanh(bias_);
        satNorm_ = 1.0f / (pre_ * (1.0f - tb_ * tb_));
        makeup_ = 1.0f + 0.6f * kick;
        knee_ = 0.95f - 0.55f * kick;
        auto coef = [fs](float ms) { return 1.0f - std::exp(-1.0f / (0.001f * ms * fs)); };
        cDripLp_ = 1.0f - std::exp(-2.0f * 3.14159265f * 1500.0f / fs);
        cFastA_ = coef(0.5f); cFastR_ = coef(8.0f); cSlowA_ = coef(40.0f); cSlowR_ = coef(250.0f);
        clankDecay_ = std::exp(-1.0f / (0.012f * fs));
        dPhase_ = 1.0 / sr;
    }

    void tick(float xin, float& outL, float& outR) {
        float x = inBq_[0].run(xin);
        x = inBq_[1].run(x);
        x = shelf_.run(x);
        {
            dripLp_ += cDripLp_ * (x - dripLp_);
            const float hi = x - dripLp_;
            const float ax = std::fabs(x);
            dripFast_ += (ax > dripFast_ ? cFastA_ : cFastR_) * (ax - dripFast_);
            dripSlow_ += (ax > dripSlow_ ? cSlowA_ : cSlowR_) * (ax - dripSlow_);
            const float tr = detail::clampf(dripFast_ / (dripSlow_ + 1e-4f) - 1.2f, 0.0f, 3.0f);
            dripGate_ += cFastR_ * (tr - dripGate_);
            x += drip_ * 1.0f * dripGate_ * hi;
        }
        const float u = pre_ * x;
        float y = (std::tanh(u + bias_) - tb_) * satNorm_ * makeup_;
        {
            const float au = std::fabs(u);
            kFast_ += (au > kFast_ ? cFastA_ : cFastR_) * (au - kFast_);
            kSlow_ += (au > kSlow_ ? cSlowA_ : cSlowR_) * (au - kSlow_);
            const float trig = detail::clampf((kFast_ - std::max(kClankThr, 1.4f * kSlow_)) * 0.25f, 0.0f, 1.0f);
            clankEnv_ = std::max(clankEnv_ * clankDecay_, trig);
            clankPeak_ = std::max(clankPeak_, clankEnv_);
            if (clankOn_ && clankEnv_ > 1e-6f) {
                rng_ = rng_ * 1664525u + 1013904223u;
                const float nz = float(int32_t(rng_)) * (1.0f / 2147483648.0f);
                y += kClankGain * clankBp_.run(nz * clankEnv_);
            }
        }
        const float e = y + 1e-20f;
        float v[kLanes];
        for (int s = 0; s < kLanes; ++s) v[s] = e;
        for (int k = 0; k < kSections; ++k) {
            float* st = ap_[k];
            for (int s = 0; s < kLanes; ++s) {
                const float yy = a_[s] * v[s] + st[s];
                st[s] = v[s] - a_[s] * yy;
                v[s] = yy;
            }
        }
        const double t = phase_;
        phase_ += dPhase_;
        if (phase_ > 1000.0) phase_ -= 1000.0;
        float rd[kLanes];
        for (int s = 0; s < kLanes; ++s) {
            buf_[s][pos_] = v[s] + fb_[s];
            const float m = modDepth_ * float(0.6 * std::sin(6.283185307179586 * (kLfoA[s] * t) + s)
                                              + 0.4 * std::sin(6.283185307179586 * (kLfoB[s] * t) + 2.0 * s));
            rd[s] = readLagrange(buf_[s], kBufMask, pos_, D_[s] + m);
        }
        pos_ = (pos_ + 1) & kBufMask;
        float tap[kLanes];
        for (int s = 0; s < kLanes; ++s) {
            const float xi = rd[s];
            const float yy = float(lb0_) * xi + lpS1_[s];
            lpS1_[s] = float(lb1_) * xi - float(la1_) * yy + lpS2_[s];
            lpS2_[s] = float(lb2_) * xi - float(la2_) * yy;
            const float dc = yy - dcX_[s] + dcR_ * dcY_[s];
            dcX_[s] = yy;
            dcY_[s] = dc;
            tap[s] = dc;
            fb_[s] = g_[s] * dc;
        }
        float wl = 0.80f * tap[0] - 0.70f * tap[1] + 0.75f * tap[2];
        float wr = 0.80f * tap[3] + 0.70f * tap[1] - 0.75f * tap[2];
        const float mid = 0.5f * (wl + wr), side = 0.5f * (wl - wr) * kWidth;
        wl = mid + side;
        wr = mid - side;
        for (int b = 0; b < 4; ++b) { wl = outBq_[b].run(wl, 0); wr = outBq_[b].run(wr, 1); }
        outL = retStage(wet_ * wl, knee_);
        outR = retStage(wet_ * wr, knee_);
    }

private:
    static constexpr int kBufSize = 16384, kBufMask = kBufSize - 1;
    static constexpr float kLaneMsShort[kLanes] = {29.3f, 32.2f, 34.6f, 30.7f};
    static constexpr float kSheenT60 = 0.6f;  // round-4 short tank: design 0.6 s, measured 0.63 s
    static constexpr float kLaneA[kLanes] = {1.000f, 0.990f, 1.008f, 0.995f};
    static constexpr double kLfoA[kLanes] = {0.37, 0.53, 0.29, 0.44};
    static constexpr double kLfoB[kLanes] = {0.83, 1.17, 0.97, 1.31};
    static constexpr float kWetGain = 0.55f;
    static constexpr float kRetCeil = 0.98f;
    static constexpr double kLoopLpHz = 8500.0;
    static constexpr double kWetLpHz = 7000.0;
    static constexpr double kTwangHz = 1500.0, kTwangQ = 1.2;
    static constexpr float kWidth = 0.7f;
    static constexpr float kTwangDb = 10.0f;
    static constexpr float kClankThr = 1.0f;
    static constexpr float kClankGain = 0.9f;
    static constexpr float kDecayTrim = 1.06f;

    float buf_[kLanes][kBufSize]{};
    alignas(16) float ap_[kSections][kLanes]{};
    float lpS1_[kLanes]{}, lpS2_[kLanes]{}, dcX_[kLanes]{}, dcY_[kLanes]{}, fb_[kLanes]{};
    detail::Biquad inBq_[2], outBq_[4], shelf_, clankBp_;
    int pos_ = 0;
    double phase_ = 0.0;
    float dripLp_ = 0, dripFast_ = 0, dripSlow_ = 0, dripGate_ = 0;
    float kFast_ = 0, kSlow_ = 0, clankEnv_ = 0, clankPeak_ = 0;
    bool clankOn_ = true;
    uint32_t rng_ = 0x9E3779B9u;
    double sr_ = 48000.0;
    // block-constant
    float a_[kLanes]{}, D_[kLanes]{}, g_[kLanes]{};
    double lb0_ = 1, lb1_ = 0, lb2_ = 0, la1_ = 0, la2_ = 0;
    float dcR_ = 0, wet_ = 0, modDepth_ = 0, pre_ = 1, bias_ = 0, tb_ = 0, satNorm_ = 1, makeup_ = 1, knee_ = 0.95f;
    float drip_ = 0, cDripLp_ = 0, cFastA_ = 0, cFastR_ = 0, cSlowA_ = 0, cSlowR_ = 0, clankDecay_ = 0;
    double dPhase_ = 1.0 / 48000.0;

    static float retStage(float x, float knee) {
        const float ax = std::fabs(x);
        if (ax <= knee) return x;
        const float room = kRetCeil - knee;
        const float y = knee + room * std::tanh((ax - knee) / room);
        return x < 0.0f ? -y : y;
    }
    static float readLagrange(const float* b, int mask, int p, float d) {
        d = std::max(1.0f, d);
        const int i = int(d);
        const float f = d - float(i);
        const float xm1 = b[(p - i + 1) & mask], x0 = b[(p - i) & mask];
        const float x1 = b[(p - i - 1) & mask], x2 = b[(p - i - 2) & mask];
        const float hm1 = -f * (f - 1.0f) * (f - 2.0f) * (1.0f / 6.0f);
        const float h0 = (f + 1.0f) * (f - 1.0f) * (f - 2.0f) * 0.5f;
        const float h1 = -(f + 1.0f) * f * (f - 2.0f) * 0.5f;
        const float h2 = (f + 1.0f) * f * (f - 1.0f) * (1.0f / 6.0f);
        return hm1 * xm1 + h0 * x0 + h1 * x1 + h2 * x2;
    }
};

// ---------------------------------------------------------------------------------------------
// AIR wash: a tiny, bright 2-lane spring (HP 3 kHz in, 24 dispersion sections, 7.3 / 9.1 ms loops,
// T60 0.15 s, LP 13 kHz in the loop). Mono in, 2 lane taps out.
class AirWash {
public:
    static constexpr int kL = 2, kS = 24;
    void reset() {
        std::memset(buf_, 0, sizeof(buf_));
        std::memset(ap_, 0, sizeof(ap_));
        std::memset(fb_, 0, sizeof(fb_));
        hp_ = detail::Biquad{};
        lp_ = detail::Biquad{};
        pos_ = 0;
    }
    void setup(double sr) {
        const float fs = float(sr);
        hp_.setHP(std::min(3000.0, 0.45 * sr), sr);
        lp_.setLP(std::min(13000.0, 0.45 * sr), sr);
        for (int s = 0; s < kL; ++s) {
            const float mag = 1.0f - (1.0f - kA[s]) * (44100.0f / fs);
            a_[s] = -detail::clampf(mag, 0.0f, 0.995f);
            D_[s] = std::max(4, int(std::lround(kMs[s] * 0.001f * fs)));
            g_[s] = std::pow(10.0f, -3.0f * float(D_[s]) / (kT60 * fs));
        }
        norm_ = std::sqrt(std::max(1e-6f, 1.0f - g_[0] * g_[0]));
    }
    void tick(float x, float& o0, float& o1) {
        const float h = hp_.run(x) + 1e-20f;
        float v[kL] = {h, h};
        for (int k = 0; k < kS; ++k)
            for (int s = 0; s < kL; ++s) {
                const float yy = a_[s] * v[s] + ap_[k][s];
                ap_[k][s] = v[s] - a_[s] * yy;
                v[s] = yy;
            }
        float t[kL];
        for (int s = 0; s < kL; ++s) {
            buf_[s][pos_] = v[s] + fb_[s];
            const float rd = buf_[s][(pos_ - D_[s]) & kMask];
            t[s] = lp_.run(rd, s);
            fb_[s] = g_[s] * t[s];
        }
        pos_ = (pos_ + 1) & kMask;
        o0 = norm_ * t[0];
        o1 = norm_ * t[1];
    }

private:
    static constexpr int kSize = 4096, kMask = kSize - 1;  // 21 ms at 192 kHz
    static constexpr float kMs[kL] = {7.3f, 9.1f};
    static constexpr float kA[kL] = {0.50f, 0.46f};
    static constexpr float kT60 = 0.15f;
    float buf_[kL][kSize]{};
    float ap_[kS][kL]{};
    float fb_[kL]{};
    float a_[kL]{}, g_[kL]{}, norm_ = 1.0f;
    int D_[kL]{4, 4};
    detail::Biquad hp_, lp_;
    int pos_ = 0;
};

// ---------------------------------------------------------------------------------------------
// 2x oversampler for a memoryless nonlinearity, applied to the NONLINEAR PART ONLY:
//   residual(x) = down( f(up(x)) ),  f(v) = shaper(v) - linearGain * v
// The linear part stays at the base rate with no filter, so the dry/linear path keeps zero latency
// and zero phase shift; only the distortion products pass the up/down filters (8th-order
// Butterworth LP at 0.42 fs_base, run at 2 fs), which removes the images and most of the aliasing.
struct Os2 {
    detail::Biquad up[4], dn[4];
    void setup(double fsBase) {
        const double f = 0.42 * fsBase, fs2 = 2.0 * fsBase;
        for (int k = 0; k < 4; ++k) {
            const double q = 1.0 / (2.0 * std::sin((2.0 * k + 1.0) * detail::kPi / 16.0));
            up[k].setLP(f, fs2, q);
            dn[k].setLP(f, fs2, q);
        }
    }
    void reset() { for (int k = 0; k < 4; ++k) for (int c = 0; c < 2; ++c) up[k].s1[c] = up[k].s2[c] = dn[k].s1[c] = dn[k].s2[c] = 0.0f; }
    template <class Fn> float residual(float x, int ch, Fn&& f) {
        float v0 = 2.0f * x, v1 = 0.0f;
        for (int k = 0; k < 4; ++k) { v0 = up[k].run(v0, ch); v1 = up[k].run(v1, ch); }
        float r0 = f(v0), r1 = f(v1);
        for (int k = 0; k < 4; ++k) { r0 = dn[k].run(r0, ch); r1 = dn[k].run(r1, ch); }
        (void)r0;
        return r1;
    }
};

// ---------------------------------------------------------------------------------------------
// MASTER output ceiling: true-peak-aware look-ahead limiter, gain on MID (side only in an emergency).
//   - true-peak estimate: 4x polyphase windowed-sinc interpolation (16 taps per phase) of M and S
//   - per interpolated point: side gain gS = min(1, 0.9c/|S|) (only when the side alone is near the
//     ceiling), mid gain gM = min(1, (c - gS|S|)/|M|)  ->  |gM M +- gS S| <= c
//   - window minimum over L+1 samples (L = look-ahead 1.5 ms), release 60 ms (exponential toward 1,
//     snapped to exactly 1), box average over L samples -> smooth gain that is <= the required gain
//     at every peak. Audio delay D = L + 8 (8 = interpolator centre). latency() = D.
//   - when both gains are exactly 1 the output is the delayed pre-limit L/R bit-for-bit.
class TpLimiter {
public:
    static constexpr int kHalf = 8, kTaps = 16, kRing = 1024;
    void setup(double sr, float ceilDb) {
        L_ = std::max(1, int(std::lround(0.0015 * sr)));
        D_ = L_ + kHalf;
        c_ = std::pow(10.0f, ceilDb / 20.0f);
        cRel_ = 1.0f - std::exp(-1.0f / (0.060f * float(sr)));
        for (int k = 1; k < 4; ++k) {
            double sum = 0;
            for (int j = 0; j < kTaps; ++j) {
                const double x = double(j - (kHalf - 1)) - k / 4.0;  // tap offsets -7..8 re the centre sample
                const double sc = std::fabs(x) < 1e-12 ? 1.0 : std::sin(detail::kPi * x) / (detail::kPi * x);
                const double w = 0.5 + 0.5 * std::cos(detail::kPi * x / (kHalf + 0.5));
                h_[k][j] = sc * w;
                sum += h_[k][j];
            }
            for (int j = 0; j < kTaps; ++j) h_[k][j] /= sum;
        }
    }
    void reset() {
        std::memset(dl_, 0, sizeof(dl_));
        std::memset(hm_, 0, sizeof(hm_));
        std::memset(hs_, 0, sizeof(hs_));
        for (int i = 0; i < kRing; ++i) { envRingM_[i] = envRingS_[i] = 1.0f; }
        n_ = 0; qmH_ = qmT_ = qsH_ = qsT_ = 0;
        envM_ = envS_ = 1.0f; sumM_ = sumS_ = 0.0; nonOneM_ = nonOneS_ = 0;
    }
    int latency() const { return D_; }
    float lastGainM() const { return gMout_; }
    // one frame in (pre-limit L/R and its M/S), one delayed frame out
    void tick(float Lp, float Rp, float M, float S, bool active, float& oL, float& oR) {
        const int w = int(n_ & (kRing - 1));
        dl_[0][w] = Lp; dl_[1][w] = Rp; dl_[2][w] = M; dl_[3][w] = S;
        hm_[n_ & 31] = M; hs_[n_ & 31] = S;
        float gtM = 1.0f, gtS = 1.0f;
        if (active && n_ >= kTaps) {
            const int64_t p = n_ - kHalf;  // centre sample
            for (int k = 0; k < 4; ++k) {
                float mi, si;
                if (k == 0) { mi = hm_[p & 31]; si = hs_[p & 31]; }
                else {
                    double am = 0, as = 0;
                    for (int j = 0; j < kTaps; ++j) { const int64_t q = p - (kHalf - 1) + j; am += h_[k][j] * hm_[q & 31]; as += h_[k][j] * hs_[q & 31]; }
                    mi = float(am); si = float(as);
                }
                const float as_ = std::fabs(si), am_ = std::fabs(mi);
                float gs = 1.0f;
                if (as_ > 0.9f * c_) gs = 0.9f * c_ / as_;
                const float room = c_ - gs * as_;
                float gm = 1.0f;
                if (am_ > room) gm = room / am_;
                gtM = std::min(gtM, gm);
                gtS = std::min(gtS, gs);
            }
        }
        // window minimum over the last L+1 targets (monotonic deques)
        const float hM = pushMin(qmI_, qmV_, qmH_, qmT_, gtM), hS = pushMin(qsI_, qsV_, qsH_, qsT_, gtS);
        envM_ = std::min(hM, envM_ + cRel_ * (1.0f - envM_)); if (1.0f - envM_ < 1e-6f) envM_ = std::min(hM, 1.0f);
        envS_ = std::min(hS, envS_ + cRel_ * (1.0f - envS_)); if (1.0f - envS_ < 1e-6f) envS_ = std::min(hS, 1.0f);
        // box average over L samples
        const int b = int(n_ % L_);
        box(envRingM_, b, envM_, sumM_, nonOneM_);
        box(envRingS_, b, envS_, sumS_, nonOneS_);
        const float gM = nonOneM_ ? float(sumM_ / L_) : 1.0f, gS = nonOneS_ ? float(sumS_ / L_) : 1.0f;
        gMout_ = gM;
        const int r = int((n_ - D_) & (kRing - 1));
        if (gM == 1.0f && gS == 1.0f) { oL = dl_[0][r]; oR = dl_[1][r]; }
        else { const float m = gM * dl_[2][r], sd = gS * dl_[3][r]; oL = m + sd; oR = m - sd; }
        ++n_;
    }

private:
    int L_ = 72, D_ = 80;
    float c_ = 0.95f, cRel_ = 0.0003f;
    double h_[4][kTaps]{};
    float dl_[4][kRing]{};
    float hm_[32]{}, hs_[32]{};
    int64_t n_ = 0;
    int64_t qmI_[kRing]{}, qsI_[kRing]{};
    float qmV_[kRing]{}, qsV_[kRing]{};
    int qmH_ = 0, qmT_ = 0, qsH_ = 0, qsT_ = 0;
    float envM_ = 1, envS_ = 1, gMout_ = 1;
    float envRingM_[kRing]{}, envRingS_[kRing]{};
    double sumM_ = 0, sumS_ = 0;
    int nonOneM_ = 0, nonOneS_ = 0;

    float pushMin(int64_t* qi, float* qv, int& h, int& t, float v) {
        while (t != h && qv[(t - 1) & (kRing - 1)] >= v) --t;
        qi[t & (kRing - 1)] = n_; qv[t & (kRing - 1)] = v; ++t;
        while (qi[h & (kRing - 1)] < n_ - L_) ++h;
        if (h >= (1 << 30)) { h -= (1 << 29); t -= (1 << 29); }  // keep the counters bounded (ring size divides 2^29)
        return qv[h & (kRing - 1)];
    }
    void box(float* ring, int b, float v, double& sum, int& nonOne) {
        const float old = ring[b];
        if (old != 1.0f) { --nonOne; sum -= old; }
        ring[b] = v;
        if (v != 1.0f) { ++nonOne; sum += v; }
        if (nonOne == 0) sum = 0.0;  // no drift once back at unity
    }
};

// ---------------------------------------------------------------------------------------------
class YayEngine {
public:
    void prepare(double sr) {
        sr_ = sr;
        kwIn_.setup(sr);
        kwOut_.setup(sr);
        wash_.setup(sr);
        osAmp_.setup(sr);
        osMst_.setup(sr);
        lim_.setup(sr, kCeilDb);
        airCalAir_ = -1.0f;
        reset();
    }

    void reset() {
        tank_.reset();
        wash_.reset();
        airLiftDb_ = 0.0f; sheenDb_[0] = sheenDb_[1] = -120.0f;
        osAmp_.reset();
        osMst_.reset();
        lim_.reset();
        for (auto* b : {&ampHp_, &presShelf_, &spkLp_, &mHp_[0], &mHp_[1], &mLp_, &mPres_, &mWarm_, &mTop_, &airShelf_, &tankHp_, &mtrPre_, &mtrPost_}) { b->s1[0] = b->s1[1] = b->s2[0] = b->s2[1] = 0.0f; }
        kwIn_.reset();
        kwOut_.reset();
        ampDcX_[0] = ampDcX_[1] = ampDcY_[0] = ampDcY_[1] = 0.0f;
        mDcX_ = mDcY_ = 0.0f;
        pIn_ = pOut_ = kAgPrior;
        agG_ = 1.0f;
    }

    // MASTER SAFE off: 0. On: look-ahead 1.5 ms + 8 samples (true-peak interpolator), in every state
    // including BYPASS, so it is constant while MASTER SAFE is on. latency() = the last processed setting.
    int latency(bool masterSafe) const { return masterSafe ? lim_.latency() : 0; }
    int latency() const { return latency(lastSafe_); }

    // test hooks (not controls)
    float clankPeak() const { return tank_.clankPeak(); }
    void setClankEnabled(bool on) { tank_.setClankEnabled(on); }
    void setAirWashEnabled(bool on) { washOn_ = on; }
    float autoGainDb() const { return 20.0f * std::log10(std::max(1e-9f, agG_)); }

    // meter taps for the plugin face (updated once per process() block, cheap):
    //   airLiftDb():     dB the AIR stage actually added above 8 kHz (HP 8 kHz energy after / before AIR,
    //                    block RMS, mono/MID); 0 when AIR is off or the block is silent
    //   sheenLevelDb(ch): RMS of the SHEEN return per channel (dBFS), -120 when SHEEN is off
    //   autoGainDb():    MASTER SAFE auto-gain (dB)
    float airLiftDb() const { return airLiftDb_; }
    float sheenLevelDb(int ch) const { return sheenDb_[ch & 1]; }

    void process(float* l, float* r, int n, const Controls& c) {
        using detail::clampf;
        const bool safe = c.masterSafe;
        lastSafe_ = safe;
        const float kick = clampf(c.kick, 0.0f, 1.0f);   // legacy preamp (not on the panel), default 0
        const float tankAmt = kSheenToTank * clampf(c.sheen, 0.0f, 1.0f);  // SHEEN 0..1 -> tank 0..0.30
        const float air = clampf(c.air, 0.0f, 1.0f);
        const float outDb = clampf(c.output, -12.0f, 12.0f);
        const bool colour = !c.bypass && (kick > 0.0f || tankAmt > 0.0f || air > 0.0f);
        if (!colour) { airLiftDb_ = 0.0f; sheenDb_[0] = sheenDb_[1] = -120.0f; }
        if (!colour && (c.bypass || outDb == 0.0f)) {
            if (!safe) return;  // bit-exact dry, zero latency
            // MASTER SAFE: delayed dry (constant latency), bit-exact after latency compensation
            for (int i = 0; i < n; ++i) { const float L = l[i], R = r[i]; lim_.tick(L, R, 0.5f * (L + R), 0.5f * (L - R), false, l[i], r[i]); }
            return;
        }
        const float og = std::pow(10.0f, outDb / 20.0f);
        if (!colour) {  // OUTPUT only
            for (int i = 0; i < n; ++i) {
                if (!safe) { l[i] *= og; r[i] *= og; continue; }
                const float L = og * l[i], R = og * r[i];
                lim_.tick(L, R, 0.5f * (L + R), 0.5f * (L - R), true, l[i], r[i]);
            }
            return;
        }
        const float fs = float(sr_);
        // legacy amp engage: smoothstep over KICK 0 .. 0.1 (KICK 0 = the amp code is not run at all)
        const float ek = clampf(kick / 0.1f, 0.0f, 1.0f);
        const float e = ek * ek * (3.0f - 2.0f * ek);
        const bool ampOn = kick > 0.0f;
        const float presence = clampf(c.presence, 0.0f, 1.0f);

        // SHEEN: tank amount t = 0.30*SHEEN, colour gain 2t (<= 0.6); the dry is ALWAYS at unity.
        const float dryG = 1.0f;
        const float colG = tankAmt >= 0.5f ? 1.0f : 2.0f * tankAmt;
        const bool tankOn = tankAmt > 0.0f;
        if (tankOn) { tank_.setup(c, sr_, colG); tankHp_.setHP(clampf(c.lowcut, 20.0f, 300.0f), sr_); }

        // AIR
        const bool airOn = air > 0.0f;
        if (airOn) calibrateAir(air);
        const float washG = washOn_ ? kWashMax * air * kWashGain : 0.0f;
        meterBegin(airOn);

        if (!safe) {
            // legacy amp input stage (only when KICK > 0)
            const float pre = 0.2f * std::pow(40.0f, kick);
            const float tb = std::tanh(kTubeBias);
            const float lin = std::sqrt(std::max(1.0f, pre));          // small-signal gain
            const float norm = lin / (pre * (1.0f - tb * tb));
            const float dcR = std::exp(-2.0f * 3.14159265f * 7.0f / fs);
            auto shaper = [&](float v) { return (std::tanh(pre * v + kTubeBias) - tb) * norm - lin * v; };
            if (ampOn) {
                ampHp_.setHP(kAmpLowCutHz, sr_);
                presShelf_.setHighShelf(std::min(3200.0, 0.45 * sr_), 6.0 * presence, sr_);
                spkLp_.setLP(std::min(5000.0, 0.45 * sr_), sr_);
            }
            for (int i = 0; i < n; ++i) {
                float a[2] = {l[i], r[i]};
                if (ampOn)
                    for (int ch = 0; ch < 2; ++ch) {
                        const float x = a[ch];
                        float y = ampHp_.run(x, ch);
                        y = lin * y + osAmp_.residual(y, ch, shaper);  // 2x-oversampled nonlinear part
                        const float d = y - ampDcX_[ch] + dcR * ampDcY_[ch];
                        ampDcX_[ch] = y;
                        ampDcY_[ch] = d;
                        y = spkLp_.run(presShelf_.run(d, ch), ch);
                        a[ch] = x + e * (y - x);
                    }
                float o0 = a[0], o1 = a[1];
                if (tankOn) {
                    float wl, wr;
                    tank_.tick(tankHp_.run(0.5f * (a[0] + a[1])), wl, wr);
                    o0 = dryG * a[0] + wl;
                    o1 = dryG * a[1] + wr;
                    mES_[0] += double(wl) * wl; mES_[1] += double(wr) * wr;
                }
                if (airOn) {
                    float w0, w1;
                    const float pm = mtrPre_.run(0.5f * (o0 + o1));
                    wash_.tick(0.5f * (o0 + o1), w0, w1);
                    o0 = airShelf_.run(o0, 0) + washG * w0;
                    o1 = airShelf_.run(o1, 1) + washG * w1;
                    const float qm = mtrPost_.run(0.5f * (o0 + o1));
                    mEPre_ += double(pm) * pm; mEPost_ += double(qm) * qm;
                }
                l[i] = og * guard(o0);
                r[i] = og * guard(o1);
            }
            meterEnd(n, airOn, tankOn);
            return;
        }

        // MASTER SAFE: mid/side, processing on MID only, auto-gain, -0.3 dBTP ceiling
        if (ampOn) {  // legacy band saturation + voicing (only when KICK > 0)
            mHp_[0].setHP(120.0, sr_, 0.5411961001461970);
            mHp_[1].setHP(120.0, sr_, 1.3065629648763766);
            mLp_.setLP(std::min(8000.0, 0.45 * sr_), sr_);
            mPres_.setPeak(std::min(3500.0, 0.45 * sr_), 0.8, 3.0 * presence * e, sr_);
            mWarm_.setPeak(kWarmHz, kWarmQ, kWarmDb * kick * e, sr_);
            mTop_.setHighShelf(std::min(kTopHz, 0.45 * sr_), -kTopDb * kick * e, sr_);
        }
        const float pre = std::max(0.01f, kMasterDrive * kick);
        const float tb = std::tanh(kMasterBias);
        const float norm = 1.0f / (pre * (1.0f - tb * tb));
        auto shaper = [&](float v) { return (std::tanh(pre * v + kMasterBias) - tb) * norm - v; };
        const float dcR = std::exp(-2.0f * 3.14159265f * 5.0f / fs);
        const float cAg = 1.0f - std::exp(-1.0f / (kAgSeconds * fs));
        const float cG = 1.0f - std::exp(-1.0f / (0.020f * fs));
        for (int i = 0; i < n; ++i) {
            const float L = l[i], R = r[i];
            const float M = 0.5f * (L + R), S = 0.5f * (L - R);
            float m = M;
            if (ampOn) {
                const float band = mLp_.run(mHp_[1].run(mHp_[0].run(M)));
                const float dd = osMst_.residual(band, 0, shaper);  // sat(band) - band, 2x oversampled
                const float d = dd - mDcX_ + dcR * mDcY_;
                mDcX_ = dd;
                mDcY_ = d;
                m = mTop_.run(mWarm_.run(mPres_.run(M + e * d)));
            }
            if (tankOn) {
                float wl, wr;
                tank_.tick(tankHp_.run(m), wl, wr);
                m = dryG * m + 0.5f * (wl + wr);
                mES_[0] += double(wl) * wl; mES_[1] += double(wr) * wr;
            }
            if (airOn) {
                float w0, w1;
                const float pm = mtrPre_.run(m);
                wash_.tick(m, w0, w1);
                m = airShelf_.run(m) + washG * 0.5f * (w0 + w1);
                const float qm = mtrPost_.run(m);
                mEPre_ += double(pm) * pm; mEPost_ += double(qm) * qm;
            }
            // auto-gain: K-weighted power of the processed MID follows the input MID
            const float ki = kwIn_.run(M), ko = kwOut_.run(m);
            pIn_ += cAg * (ki * ki - pIn_);
            pOut_ += cAg * (ko * ko - pOut_);
            const float gt = clampf(std::sqrt((pIn_ + 1e-12f) / (pOut_ + 1e-12f)), 0.25f, 4.0f);
            agG_ += cG * (gt - agG_);
            m *= agG_;
            // OUTPUT, then the true-peak ceiling (last, so nothing leaves MASTER SAFE above -0.3 dBTP)
            const float mo = og * m, so = og * S;
            lim_.tick(mo + so, mo - so, mo, so, true, l[i], r[i]);
        }
        meterEnd(n, airOn, tankOn);
    }

private:
    static constexpr float kTubeBias = 0.2f;        // preamp grid bias (asymmetry -> 2nd harmonic)
    static constexpr float kMasterDrive = 1.0f;     // MASTER pre-gain at KICK 1
    static constexpr float kMasterBias = 0.6f;      // MASTER bias shift: H2 leads H3 (warmth, not grit)
    static constexpr double kWarmHz = 220.0, kWarmQ = 0.7;
    static constexpr double kWarmDb = 4.0;          // warmth peak gain at KICK 1
    static constexpr double kTopHz = 7000.0;
    static constexpr double kTopDb = 4.0;           // top softening at KICK 1
    static constexpr double kAmpLowCutHz = 80.0;    // legacy amp stage input HP
    static constexpr float kSheenToTank = 0.30f;    // SHEEN 0.5 = tank 0.15 = the round-4 AIR FULL
    static constexpr float kWashMax = 0.15f;        // AIR wash amount at AIR 1
    static constexpr float kWashGain = 1.5f;
    static constexpr float kAirDbAt11k = 6.0f;      // AIR 1 = +6 dB at 11 kHz
    static constexpr double kAirCornerHz = 9000.0;
    static constexpr float kAgSeconds = 1.5f;       // auto-gain loudness window
    static constexpr float kAgPrior = 1e-5f;        // start the follower at unity gain
    static constexpr float kCeilDb = -0.4f;         // MASTER ceiling: -0.3 dBTP spec, 0.1 dB estimator margin

    SpringTank tank_;
    AirWash wash_;
    KWeight kwIn_, kwOut_;
    detail::Biquad ampHp_, presShelf_, spkLp_, mHp_[2], mLp_, mPres_, mWarm_, mTop_, airShelf_, tankHp_, mtrPre_, mtrPost_;
    float airLiftDb_ = 0.0f, sheenDb_[2] = {-120.0f, -120.0f};
    double mEPre_ = 0, mEPost_ = 0, mES_[2] = {0, 0};
    void meterBegin(bool airOn) {
        mEPre_ = mEPost_ = 0; mES_[0] = mES_[1] = 0;
        if (airOn) { mtrPre_.setHP(std::min(8000.0, 0.45 * sr_), sr_); mtrPost_.setHP(std::min(8000.0, 0.45 * sr_), sr_); }
    }
    void meterEnd(int n, bool airOn, bool sheenOn) {
        airLiftDb_ = (airOn && mEPre_ > 1e-12) ? float(10.0 * std::log10(std::max(1e-30, mEPost_) / mEPre_)) : 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            sheenDb_[ch] = (sheenOn && n > 0) ? float(std::max(-120.0, 10.0 * std::log10(std::max(1e-30, mES_[ch] / n)))) : -120.0f;
    }
    Os2 osAmp_, osMst_;
    TpLimiter lim_;
    bool lastSafe_ = false;
    float ampDcX_[2]{}, ampDcY_[2]{}, mDcX_ = 0, mDcY_ = 0;
    float pIn_ = kAgPrior, pOut_ = kAgPrior, agG_ = 1.0f;
    float airCalAir_ = -1.0f;
    bool washOn_ = true;
    double sr_ = 48000.0;

    // shelf plateau chosen so the measured shelf gain at 11 kHz is kAirDbAt11k * AIR
    void calibrateAir(float air) {
        if (air == airCalAir_) return;
        airCalAir_ = air;
        const double target = kAirDbAt11k * air;
        const double f0 = std::min(kAirCornerHz, 0.4 * sr_), fm = std::min(11000.0, 0.45 * sr_);
        double G = target;
        detail::Biquad t;
        for (int it = 0; it < 4; ++it) {
            t.setHighShelf(f0, G, sr_);
            const double m = t.magDb(fm, sr_);
            if (m > 1e-9) G *= target / m;
        }
        airShelf_.setHighShelf(f0, G, sr_);  // coefficients only; filter state is kept
    }
    static float guard(float x) {  // output safety: linear to 1.5, soft to a 1.98 ceiling (< +6 dBFS)
        const float ax = std::fabs(x);
        if (ax <= 1.5f) return x;
        const float y = 1.5f + 0.48f * std::tanh((ax - 1.5f) / 0.48f);
        return x < 0.0f ? -y : y;
    }
};

}  // namespace yay
