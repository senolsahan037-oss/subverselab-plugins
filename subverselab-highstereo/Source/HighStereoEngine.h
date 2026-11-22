// HIGH STEREO engine. Plain C++17, header only, no JUCE. Zero latency.
//
// Signal flow, per sample:
//   M = (L+R)/2, S = (L-R)/2
//   St = width*S
//   SPREAD (fill, owner 2026-11-20): the hole between the centre and the edges is filled from the edges.
//     Per band k, w_k = min(1, 2 * Ps_k / (Pm_k + Ps_k)) (300 ms): a centre band has w = 0, a hard-panned or wide band ~1.
//     eL = sum w_k * L_k, eR = sum w_k * R_k (the edge content only); fL = decorrelate(HP150(eL)), fR likewise with a
//     different allpass chain; fL is placed at pan -0.45, fR at +0.45 (constant power), gain spread * fillScale.
//     Mono gains the decorrelated fill (no comb), so SPREAD is the one control that is not mono-exact.
//   BEHIND (owner 2026-11-20, "sesler arkadan geliyormus hissi, yuksek ayarlarda efektif"): above WIDTH 150 % the side
//     takes on the rear direction bands (Blauert): per-band side gains from the same band tree, ramped in from 150 to 200 %.
//     Side only, so mono is unchanged; band tree, so the phase-matched dry path still never combs at any MIX.
//   MONO BASS:  St -> LR4 high-pass at fm, M -> matching 2nd-order allpass (same phase)
//   8-band LR4 tree (100 .. 8k) on St and on M, with allpass compensation: sum of bands = allpass of input
//   FIX k:  S band k scaled so its power never exceeds the M band's (band correlation >= 0)
//   L = M + S, R = M - S; MIX; OUTPUT
//   STAGE (owner 2026-11-20, the puck dragged inside the 360 stage): the finished (M,S) rotated by a = stage/2, so the
//     whole image turns by 'stage' degrees on the 360 goniometer (+90 left, -90 right, +-180 behind). Exact 2x2 rotation,
//     power kept; mono is NOT kept once it is off 0. After the band readouts on purpose: the stage (left screen) does not
//     move the width panel (owner: "biri sahne, digeri genislik").
//   MATCH (K-weighted loudness held to the input's; includes OUTPUT, so a preset's trim never breaks a level A/B)
//   PEAK GUARD (zero latency): output peaks held to the input's own recent peak (-1 .. 0 dBFS)
//   IN / "nothing to do" crossfade to the untouched input (bit-identical when settled)
//   LISTEN (monitoring only, after everything): MONO, PHONE, SPEAKERS, HEADPHONES (earbud model, stereo kept)
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace svl {

namespace hs {

constexpr double kPi = 3.14159265358979323846;

struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;

    inline double process (double x) noexcept
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() noexcept { z1 = z2 = 0; }
    /** |H| at frequency f (for readouts only). */
    double magnitudeAt (double fs, double f) const noexcept
    {
        const double w = 2.0 * kPi * f / fs, c1 = std::cos (w), s1 = std::sin (w), c2 = std::cos (2 * w), s2 = std::sin (2 * w);
        const double nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2);
        const double dr = 1 + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2);
        return std::sqrt ((nr * nr + ni * ni) / std::max (1e-30, dr * dr + di * di));
    }

    // RBJ cookbook. All share the same prewarp, so LP4 + HP4 == AP2 holds digitally.
    void setLowPass (double fs, double f, double q)  { design (fs, f, q, 0); }
    void setHighPass (double fs, double f, double q) { design (fs, f, q, 1); }
    void setAllPass (double fs, double f, double q)  { design (fs, f, q, 2); }
    void setPeak (double fs, double f, double q, double gainDb) { design (fs, f, q, 3, gainDb); }
    void setHighShelf (double fs, double f, double q, double gainDb) { design (fs, f, q, 4, gainDb); }

    void design (double fs, double f, double q, int type, double gainDb = 0)
    {
        f = std::min (f, fs * 0.49);
        const double w0 = 2.0 * kPi * f / fs, c = std::cos (w0), s = std::sin (w0);
        const double alpha = s / (2.0 * q);
        const double A = std::pow (10.0, gainDb / 40.0);
        double nb0 = 1, nb1 = 0, nb2 = 0, na0 = 1, na1 = 0, na2 = 0;
        switch (type)
        {
            case 0: nb0 = (1 - c) / 2; nb1 = 1 - c; nb2 = (1 - c) / 2; na0 = 1 + alpha; na1 = -2 * c; na2 = 1 - alpha; break;
            case 1: nb0 = (1 + c) / 2; nb1 = -(1 + c); nb2 = (1 + c) / 2; na0 = 1 + alpha; na1 = -2 * c; na2 = 1 - alpha; break;
            case 2: nb0 = 1 - alpha; nb1 = -2 * c; nb2 = 1 + alpha; na0 = 1 + alpha; na1 = -2 * c; na2 = 1 - alpha; break;
            case 3: nb0 = 1 + alpha * A; nb1 = -2 * c; nb2 = 1 - alpha * A; na0 = 1 + alpha / A; na1 = -2 * c; na2 = 1 - alpha / A; break;
            default:
            {
                const double sq = 2.0 * std::sqrt (A) * alpha;
                nb0 = A * ((A + 1) + (A - 1) * c + sq);
                nb1 = -2 * A * ((A - 1) + (A + 1) * c);
                nb2 = A * ((A + 1) + (A - 1) * c - sq);
                na0 = (A + 1) - (A - 1) * c + sq;
                na1 = 2 * ((A - 1) - (A + 1) * c);
                na2 = (A + 1) - (A - 1) * c - sq;
            }
        }
        b0 = nb0 / na0; b1 = nb1 / na0; b2 = nb2 / na0; a1 = na1 / na0; a2 = na2 / na0;
    }
};

constexpr double kButterQ = 0.70710678118654752;

/** Linkwitz-Riley 4th order split. low + high == 2nd-order allpass at the same frequency. */
struct LR4
{
    Biquad lp1, lp2, hp1, hp2;
    void set (double fs, double f)
    {
        lp1.setLowPass (fs, f, kButterQ); lp2 = lp1;
        hp1.setHighPass (fs, f, kButterQ); hp2 = hp1;
    }
    void reset() { lp1.reset(); lp2.reset(); hp1.reset(); hp2.reset(); }
    inline void split (double x, double& lo, double& hi) noexcept
    {
        lo = lp2.process (lp1.process (x));
        hi = hp2.process (hp1.process (x));
    }
};

/** Schroeder allpass: flat magnitude, smeared phase. Used to decorrelate the SPREAD fill from the side. */
struct SchroederAP
{
    std::vector<double> buf;
    int pos = 0, delay = 1;
    double g = 0.5;
    void prepare (int d, double gain) { delay = std::max (1, d); buf.assign ((size_t) delay, 0.0); pos = 0; g = gain; }
    void reset() { std::fill (buf.begin(), buf.end(), 0.0); pos = 0; }
    inline double process (double x) noexcept
    {
        const double vd = buf[(size_t) pos];      // v[n-d]
        const double v = x + g * vd;
        const double y = -g * v + vd;
        buf[(size_t) pos] = v;
        if (++pos >= delay) pos = 0;
        return y;
    }
};

struct Smoother
{
    double cur = 0, coeff = 0;
    void prepare (double fs, double ms, double v) { coeff = std::exp (-1.0 / (fs * ms * 0.001)); cur = v; }
    inline double next (double target) noexcept
    {
        cur = target + coeff * (cur - target);
        if (std::abs (cur - target) < 1e-9) cur = target;
        return cur;
    }
};

/** 10 ms raised-cosine crossfade position: no kink at either end, settles exactly on 0 or 1. */
struct Fade
{
    double pos = 0, step = 0.001;
    void prepare (double fs, double ms, double v) { step = 1.0 / std::max (1.0, fs * ms * 0.001); pos = v; }
    inline double next (double target) noexcept
    {
        if (pos < target) pos = std::min (target, pos + step);
        else if (pos > target) pos = std::max (target, pos - step);
        return 0.5 - 0.5 * std::cos (kPi * pos);
    }
};

inline double dbToGain (double db) { return std::pow (10.0, db / 20.0); }
inline double safe (double x) { return std::isfinite (x) ? x : 0.0; }

} // namespace hs

class HighStereoEngine
{
public:
    static constexpr int kBands = 8;
    static constexpr double kCrossovers[kBands - 1] = { 100, 250, 500, 1000, 2000, 4000, 8000 };
    // BEHIND: side gain per band at full depth (WIDTH 200 %). Rear bands up (500 Hz-2 kHz, 8 kHz+), front bands down
    // (250-500 Hz, 4-8 kHz), plus an overall side lift. Values chosen by measurement, STATUS 20 Nov.
    static constexpr double kBehindDb[kBands] = { 0.0, 2.0, -1.0, 7.0, 8.0, 3.0, -3.0, 7.0 };
    static constexpr double kBehindStart = 1.5, kBehindFull = 2.0;
    static double behindDepth (double width) { return std::clamp ((width - kBehindStart) / (kBehindFull - kBehindStart), 0.0, 1.0); }

    enum class Listen { Off = 0, Mono, Phone, Speakers, Headphones };

    struct Parameters
    {
        bool   enabled  = true;
        double width    = 1.0;    // 0 .. 2
        double spread   = 0.0;    // 0 .. 1  (fill from the edges toward the centre)
        double fillScale = 1.0;   // fill gain at SPREAD 100 %; chosen by measurement (STATUS 20 Nov, tools/fill_tune3.py), not a user control
        double monoBassHz = 0.0;  // 0 = OFF, else 40 .. 300
        std::array<double, kBands> bandWidth { 1, 1, 1, 1, 1, 1, 1, 1 };   // 0 .. 2 per band, on top of WIDTH
        bool   fix      = false;   // one key: no band may go past correlation 0 (vanish in mono)
        bool   match    = false;
        double outputDb = 0.0;
        double mix      = 1.0;    // dry/wet 0 .. 1; the dry path is phase-matched to the wet one (no comb at any MIX)
        double stageDeg = 0.0;    // -180 .. 180: where the centre of the image sits on the 360 stage (0 front)
        Listen listen   = Listen::Off;
        bool   peakGuard = true;   // always on in the plug-in; the self-test switches it off to check the exact math
    };

    struct BandReadout { float correlation = 1.0f; float monoLossDb = 0.0f; float fixGainDb = 0.0f; float levelDb = -120.0f;
                         bool overflow = false;      // overflow: before FIX this band's side is louder than its mid
                         // Live, for the moving STEREO FIELD (owner 20 Nov: bands move with the sound). 50 ms, output side.
                         float liveWidth = 0.0f;     // 2*Ps/(Pm+Ps): 0 mono, 1 fully wide, > 1 out of phase (vanishes in mono)
                         float liveLevelDb = -120.0f;
                         // What this band loses right now in the selected LISTEN mode (STEREO reads as MONO).
                         float liveLossDb = 0.0f;
                         // Left/right balance of the band's energy, -1 right .. +1 left (2*<M*S>/(Pm+Ps)), 50 ms. Drives the
                         // 360 scope's dots together with liveWidth (spread, share behind) and liveLevelDb (how many).
                         float liveBalance = 0.0f;
                         // LIMIT (SPEC 7): learned from the music since the last resetPeaks(). limit = the band setting
                         // (0 .. 2) at which this band's loudest 300 ms reaches fill 100 %; peakFill at the current settings.
                         float limit = 2.0f;          // > setting: room to widen; < setting: narrow to it
                         float peakFill = 0.0f;       // 2r/(1+r) of the held peak, at the current settings (1 = 100 %)
                         bool  noSide = true;         // almost no side to widen (even 200 % stays under ~2 % fill)
                         bool  learned = false; };    // at least one block above the level gate since the reset

    void prepare (double sampleRate, int maxBlock)
    {
        fs = sampleRate;
        tapL.assign ((size_t) std::max (maxBlock, 4096), 0.0f); tapR = tapL; tapCount = 0;
        for (int k = 0; k < kBands - 1; ++k)
        {
            xS[k].set (fs, kCrossovers[k]);
            xM[k].set (fs, kCrossovers[k]);
        }
        // Compensation: band k (k < 7) passes through the allpasses of every later crossover.
        for (int k = 0; k < kBands - 1; ++k)
            for (int j = k + 1; j < kBands - 1; ++j)
            {
                apS[k][j].setAllPass (fs, kCrossovers[j], hs::kButterQ);
                apM[k][j].setAllPass (fs, kCrossovers[j], hs::kButterQ);
            }

        for (int j = 0; j < kBands - 1; ++j) dsTree[(size_t) j].setAllPass (fs, kCrossovers[j], hs::kButterQ);
        dHp.setHighPass (fs, 150.0, hs::kButterQ);
        const double delaysMs[3] = { 2.3, 3.7, 5.9 };
        for (int i = 0; i < 3; ++i) dAp[i].prepare ((int) std::lround (delaysMs[i] * 0.001 * fs), 0.5);
        const double delaysMsR[3] = { 1.9, 3.1, 5.3 };
        for (int i = 0; i < 3; ++i) dApR[i].prepare ((int) std::lround (delaysMsR[i] * 0.001 * fs), 0.5);
        dHpR = dHp;
        edgeCoeff = std::exp (-1.0 / (fs * 0.3));

        lastMonoBassHz = -1.0;
        updateMonoBass (150.0);

        // K-weighting, BS.1770 stage 1 (shelf) and stage 2 (high-pass), designed for this rate.
        for (int c = 0; c < 2; ++c)
        {
            kIn[c][0].setHighShelf (fs, 1681.974450955533, hs::kButterQ, 3.999843853973347);
            kIn[c][1].setHighPass (fs, 38.13547087602444, 0.5003270373238773);
            kOut[c][0] = kIn[c][0]; kOut[c][1] = kIn[c][1];
        }
        powCoeff  = std::exp (-1.0 / (fs * 1.5));
        bandCoeff = std::exp (-1.0 / (fs * 0.3));
        fixCoeff  = std::exp (-1.0 / (fs * 0.15));
        liveCoeff = std::exp (-1.0 / (fs * 0.05));

        // PHONE: one small speaker. Values are a model, NOT measured yet (STATUS: measure a real phone).
        phHp1.setHighPass (fs, 550.0, 0.5412); phHp2.setHighPass (fs, 550.0, 1.3066);
        phPeak.setPeak (fs, 900.0, 1.5, 4.0);
        phLp.setLowPass (fs, 10000.0, hs::kButterQ);
        // HEADPHONES: typical earbuds. Model, NOT measured yet (like PHONE): bass roll-off, presence bump, soft top.
        for (int c = 0; c < 2; ++c)
        {
            hpHp[c].setHighPass (fs, 55.0, 0.7);
            hpPk[c].setPeak (fs, 2800.0, 1.2, 4.0);
            hpSh[c].setHighShelf (fs, 9000.0, hs::kButterQ, -4.0);
        }
        // SPEAKERS: bs2b default level, 700 Hz / 4.5 dB.
        sfCoeff = std::exp (-2.0 * hs::kPi * 700.0 / fs);

        mixS.prepare (fs, 20, 1.0);
        stageS.prepare (fs, 40, 0.0); lastStage = 0.0; stC = 1.0; stSn = 0.0;
        stFade.prepare (fs, 10.0, 0.0); lastStageHz = -1.0; updateStageSplit (150.0);
        widthS.prepare (fs, 20, 1.0); spreadS.prepare (fs, 20, 0.0); outS.prepare (fs, 20, 1.0);
        activeS.prepare (fs, 10.0, 0.0);
        filtS.prepare (fs, 10.0, 0.0);
        mbFade.prepare (fs, 10.0, 0.0);
        matchS.prepare (fs, 500, 1.0);
        guardRel = std::exp (-1.0 / (0.050 * fs));     // 50 ms release
        guardInRel = std::exp (-1.0 / (1.5 * fs));     // input peak follower falls over ~1.5 s
        for (auto& g : fixS) g.prepare (fs, 20, 1.0);
        for (auto& g : bwS) g.prepare (fs, 20, 1.0);
        reset();
    }

    void reset()
    {
        for (auto& x : xS) x.reset();
        for (auto& x : xM) x.reset();
        for (auto& r : apS) for (auto& a : r) a.reset();
        for (auto& r : apM) for (auto& a : r) a.reset();
        mbS.reset(); mbM.reset(); dHp.reset(); dsMb.reset(); stSplitM.reset(); stSplitS.reset();
        for (auto& a : dsTree) a.reset();
        for (auto& a : dAp) a.reset();
        for (auto& a : dApR) a.reset();
        dHpR.reset(); fPmAll = fPsAll = 0;
        for (auto& c : kIn) for (auto& b : c) b.reset();
        for (auto& c : kOut) for (auto& b : c) b.reset();
        phHp1.reset(); phHp2.reset(); phPeak.reset(); phLp.reset();
        for (int c = 0; c < 2; ++c) { hpHp[c].reset(); hpPk[c].reset(); hpSh[c].reset(); }
        sfL = sfR = 0;
        pIn = pOut = 0; guardIn = 0; guardEnv = 1.0; guardMin = 1.0;
        for (int k = 0; k < kBands; ++k) { bPm[k] = bPs[k] = bPms[k] = 0; fPm[k] = fPs[k] = 0; lPm[k] = lPs[k] = lPms[k] = 0; cPm[k] = cPs[k] = 0; }
        resetPeaks();
        oLL = oRR = oLR = 0;
    }

    void process (const float* const* in, float* const* out, int numCh, int n, const Parameters& p)
    {
        if (numCh <= 0) return;
        if (p.monoBassHz > 0) updateMonoBass (p.monoBassHz);
        updateStageSplit (p.monoBassHz > 0 ? p.monoBassHz : 120.0);

        for (int k = 0; k < kBands; ++k) lastBandSetting[k] = std::max (1e-6, p.bandWidth[(size_t) k]);
        bool bandsMoved = false;
        for (double b : p.bandWidth) bandsMoved = bandsMoved || std::abs (b - 1.0) > 1e-9;
        const bool behind = behindDepth (p.width) > 0.0;
        const bool filtered = p.fix || bandsMoved || p.monoBassHz > 0 || behind;
        const bool noEffect = (std::abs (p.width - 1.0) < 1e-9 && p.spread <= 0.0) || p.mix <= 0.0;
        // Thresholds in plug-in units (a host's 0..1 float round trip leaves ~1e-6 behind; one control step is far larger).
        const bool staged = std::abs (p.stageDeg) > 0.05 || std::abs (stageS.cur) > 0.05;
        const bool nothingToDo = noEffect && ! filtered && ! staged && std::abs (p.outputDb) < 0.005 && ! p.match;
        const double activeTarget = (! p.enabled || nothingToDo) ? 0.0 : 1.0;
        const double outTarget = hs::dbToGain (p.outputDb);
        const bool stereo = numCh >= 2;
        double peakOut = 0;

        for (int i = 0; i < n; ++i)
        {
            const double inL = hs::safe (in[0][i]);
            const double inR = stereo ? hs::safe (in[1][i]) : inL;

            const double w  = widthS.next (p.width);
            const double bd = behindDepth (w);
            if (bd != lastBehind)
            {
                lastBehind = bd;
                for (int k = 0; k < kBands; ++k) behindGain[k] = std::pow (10.0, bd * kBehindDb[k] / 20.0);
            }
            const double sp = spreadS.next (p.spread);
            const double og = outS.next (outTarget);
            const double act = activeS.next (activeTarget);
            const double flt = filtS.next (filtered ? 1.0 : 0.0);
            const double mx  = mixS.next (p.mix);

            const double M = 0.5 * (inL + inR);
            const double S = 0.5 * (inL - inR);

            const double St = w * S;

            // Filtered path (always running so its state is warm when switched in).
            double sLo, sHi, mAp;
            mbS.split (St, sLo, sHi);
            // MONO BASS in/out fades too, so switching it while a FIX holds the filtered path cannot click.
            const double mbf = mbFade.next (p.monoBassHz > 0 ? 1.0 : 0.0);
            const double mApF = mbM.process (M);
            const double sMb = mbf * sHi + (1.0 - mbf) * St;
            mAp = mbf * mApF + (1.0 - mbf) * M;

            double restS = sMb, restM = mAp, sumS = 0, sumM = 0, eL = 0, eR = 0;
            std::array<double, kBands> bS {}, bM {};
            for (int k = 0; k < kBands - 1; ++k)
            {
                double loS, hiS, loM, hiM;
                xS[k].split (restS, loS, hiS);
                xM[k].split (restM, loM, hiM);
                for (int j = k + 1; j < kBands - 1; ++j) { loS = apS[k][j].process (loS); loM = apM[k][j].process (loM); }
                bS[(size_t) k] = loS; bM[(size_t) k] = loM;
                restS = hiS; restM = hiM;
            }
            bS[kBands - 1] = restS; bM[kBands - 1] = restM;

            for (int k = 0; k < kBands; ++k)
            {
                 const double bg = behindGain[k];
                const double ms = bM[(size_t) k], ss = bS[(size_t) k] * bwS[k].next (p.bandWidth[(size_t) k]) * bg;
                // LIMIT learns the source's own side/mid (divided by WIDTH here; band and BEHIND gains are applied after
                // bS), so moving a control never fakes a peak. WIDTH near 0: hold.
                if (w > 1e-4)
                {
                    const double su = bS[(size_t) k] / w;
                    cPm[k] = ms * ms + bandCoeff * (cPm[k] - ms * ms);
                    cPs[k] = su * su + bandCoeff * (cPs[k] - su * su);
                }
                fPm[k] = ms * ms + fixCoeff * (fPm[k] - ms * ms);
                fPs[k] = ss * ss + fixCoeff * (fPs[k] - ss * ss);
                double gTarget = 1.0;
                if (p.fix && fPs[k] > 1e-20)
                    gTarget = std::min (1.0, std::sqrt (fPm[k] / fPs[k]));
                const double g = fixS[k].next (gTarget);
                const double so = ss * g;
                sumS += so; sumM += ms;
                const double share = (bPm[k] + bPs[k]) > 1e-20 ? bPs[k] / (bPm[k] + bPs[k]) : 0.0;
                const double wk = std::min (1.0, 2.0 * share);
                eL += wk * (ms + so); eR += wk * (ms - so);
                bPm[k]  = ms * ms + bandCoeff * (bPm[k] - ms * ms);
                bPs[k]  = so * so + bandCoeff * (bPs[k] - so * so);
                bPms[k] = ms * so + bandCoeff * (bPms[k] - ms * so);
                lPm[k] = ms * ms + liveCoeff * (lPm[k] - ms * ms);
                lPs[k] = so * so + liveCoeff * (lPs[k] - so * so);
                lPms[k] = ms * so + liveCoeff * (lPms[k] - ms * so);
                curFixGain[k] = g;
            }

            double mOut = flt * sumM + (1.0 - flt) * M;
            const double sOut = flt * sumS + (1.0 - flt) * St;

            const double preL = mOut + sOut, preR = mOut - sOut;

            // Dry for MIX: the input's side through the same allpasses the wet side went through, so dry + wet never comb.
            double dS = mbf * dsMb.process (S) + (1.0 - mbf) * S;
            for (auto& a : dsTree) dS = a.process (dS);
            const double drySide = flt * dS + (1.0 - flt) * S;
            const double dryL = mOut + drySide, dryR = mOut - drySide;
            // SPREAD: decorrelated copy of each side, placed between the centre and that edge. Always computed (warm state).
            double fL = dHp.process (eL), fR = dHpR.process (eR);
            for (auto& a : dAp) fL = a.process (fL);
            for (auto& a : dApR) fR = a.process (fR);
            const double kf = sp * p.fillScale;
            constexpr double near = 0.90796, far = 0.41909;   // constant-power pan at 0.45 from centre
            const double addL = kf * (near * fL + far * fR), addR = kf * (far * fL + near * fR);
            const double wetL = preL + addL, wetR = preR + addR;
            double L = (dryL + mx * (wetL - dryL)) * og, R = (dryR + mx * (wetR - dryR)) * og;   // OUTPUT before MATCH (owner 21 Nov)
            // Tap for the WIDTH screen: the signal before STAGE (SPEC 7: the stage never moves the width screen).
            if ((size_t) i < tapL.size()) { tapL[(size_t) i] = (float) L; tapR[(size_t) i] = (float) R; }
            // STAGE. The bass below MONO BASS (120 Hz while it is off) is not rotated: it stays centred and in phase.
            // The split always runs (warm); the staged path fades in/out over 10 ms; at 0 the signal is untouched.
            const double stg = stageS.next (std::clamp (p.stageDeg, -180.0, 180.0));
            {
                if (stg != lastStage) { lastStage = stg; const double a = stg * hs::kPi / 360.0; stC = std::cos (a); stSn = std::sin (a); }
                const double mo = 0.5 * (L + R), so = 0.5 * (L - R);
                double loM, hiM, loS, hiS;
                stSplitM.split (mo, loM, hiM); stSplitS.split (so, loS, hiS);
                const double sf = stFade.next (stg != 0.0 || std::abs (p.stageDeg) > 1e-9 ? 1.0 : 0.0);
                if (sf > 0.0)
                {
                    const double m2 = loM + hiM * stC - hiS * stSn, s2 = loS + hiM * stSn + hiS * stC;
                    const double mr = mo + sf * (m2 - mo), sr = so + sf * (s2 - so);
                    L = mr + sr; R = mr - sr;
                }
            }

            // MATCH: K-weighted loudness of output held to the input's.
            const double kl = kIn[0][1].process (kIn[0][0].process (inL));
            const double kr = kIn[1][1].process (kIn[1][0].process (inR));
            pIn = (kl * kl + kr * kr) + powCoeff * (pIn - (kl * kl + kr * kr));
            const double ol = kOut[0][1].process (kOut[0][0].process (L));
            const double orr = kOut[1][1].process (kOut[1][0].process (R));
            pOut = (ol * ol + orr * orr) + powCoeff * (pOut - (ol * ol + orr * orr));
            double mTarget = 1.0;
            if (p.match && pIn > 1e-9 && pOut > 1e-12)   // gate: input above about -90 dB
                mTarget = std::clamp (std::sqrt (pIn / pOut), 0.5011872336, 1.995262315);
            else if (p.match)
                mTarget = matchS.cur;   // hold through silence
            const double mg = matchS.next (mTarget);
            // MATCH includes OUTPUT on purpose (owner 21 Nov: "MATCH kismayi hesaba katsin"): with MATCH on, IN on/off stays
            // level even when a preset trims its output; OUTPUT changes the level only while MATCH is off.
            L *= mg; R *= mg;

            // PEAK GUARD (owner 21 Nov): the band tree rotates phase, so peaks can rise while loudness does not. Zero latency,
            // stereo-linked: instant attack, 50 ms release. Ceiling = the input's own recent peak, never below -1 dBFS and
            // never above 0 dBFS: it only takes away peak the processing added, and quiet material is never touched.
            {
                const double ip = std::max (std::abs (inL), std::abs (inR));
                guardIn = ip > guardIn ? ip : ip + guardInRel * (guardIn - ip);
                const double ceil = std::clamp (guardIn, kGuardFloor, 1.0);
                const double op = std::max (std::abs (L), std::abs (R));
                const double g = op > ceil ? ceil / op : 1.0;
                guardEnv = g < guardEnv ? g : g + guardRel * (guardEnv - g);
                if (p.peakGuard) { L *= guardEnv; R *= guardEnv; } else guardEnv = 1.0;
                guardMin = std::min (guardMin, 1.0 - act * (1.0 - guardEnv));   // readout = what reaches the output (0 in bypass)
            }

            // IN / nothing-to-do crossfade to the untouched input.
            if (act <= 0.0) { L = inL; R = inR; }
            else if (act < 1.0) { L = inL + act * (L - inL); R = inR + act * (R - inR); }

            oLL = L * L + bandCoeff * (oLL - L * L);
            oRR = R * R + bandCoeff * (oRR - R * R);
            oLR = L * R + bandCoeff * (oLR - L * R);

            // LISTEN (monitoring only).
            switch (p.listen)
            {
                case Listen::Mono:  { const double m = 0.5 * (L + R); L = R = m; break; }
                case Listen::Phone:
                {
                    double m = 0.5 * (L + R);
                    m = phLp.process (phPeak.process (phHp2.process (phHp1.process (m))));
                    m = std::tanh (m * 1.5) / 1.5;
                    L = R = m; break;
                }
                case Listen::Speakers:
                {
                    // bs2b-style: cross = k * LP700(other), direct = x - (1 - G) * LP700(x); G/k = 4.5 dB, G + k = 1.
                    constexpr double k = 0.37331, G = 0.62669;
                    sfL = L + sfCoeff * (sfL - L);
                    sfR = R + sfCoeff * (sfR - R);
                    const double nl = L - (1.0 - G) * sfL + k * sfR;
                    const double nr = R - (1.0 - G) * sfR + k * sfL;
                    L = nl; R = nr; break;
                }
                case Listen::Headphones:
                {
                    L = hpSh[0].process (hpPk[0].process (hpHp[0].process (L)));
                    R = hpSh[1].process (hpPk[1].process (hpHp[1].process (R)));
                    break;
                }
                case Listen::Off: default: break;
            }

            if (stereo)
            {
                out[0][i] = (float) L; out[1][i] = (float) R;
                for (int c = 2; c < numCh; ++c) out[c][i] = in[c][i];
            }
            else
                out[0][i] = (act <= 0.0 && p.listen == Listen::Off) ? in[0][i] : (float) (0.5 * (L + R));

            peakOut = std::max (peakOut, std::max (std::abs (L), std::abs (R)));
        }

        lastPeak = peakOut;
        lastListen = p.listen;
        tapCount = std::min (n, (int) tapL.size());
        // LIMIT learning, once per block: the band's side/mid ratio normalised to unit gain (WIDTH x band x BEHIND), so the
        // held peak stays right when the user moves a control; peak held until resetPeaks().
        {
            const double wNow = widthS.cur;
            for (int k = 0; k < kBands; ++k)
            {
                const double g = std::max (1e-6, wNow * bwS[k].cur * behindGain[k]);
                curGain[k] = g;
                if (cPm[k] + cPs[k] < 1e-6 || cPm[k] < 1e-12) continue;   // band under about -60 dB: does not count
                const double rUnit = cPs[k] / cPm[k];
                rPk[k] = std::max (rPk[k], rUnit); learnedK[k] = true;
            }
        }
        lastMatchDb = 20.0 * std::log10 (std::max (1e-9, matchS.cur));
        lastGuardDb = 20.0 * std::log10 (std::max (1e-9, guardMin)); guardMin = 1.0;
    }

    // Readouts for the panel (call after process).
    BandReadout band (int k) const
    {
        BandReadout r;
        const double pm = bPm[k], ps = bPs[k], pms = bPms[k];
        const double pl = pm + ps + 2 * pms, pr = pm + ps - 2 * pms;
        r.correlation = (pl > 1e-14 && pr > 1e-14) ? (float) std::clamp ((pm - ps) / std::sqrt (pl * pr), -1.0, 1.0) : 1.0f;
        const double avg = 0.5 * (pl + pr);
        r.monoLossDb = (avg > 1e-14) ? (float) std::max (-60.0, 10.0 * std::log10 (std::max (1e-14, pm) / avg)) : 0.0f;
        r.fixGainDb = (float) (20.0 * std::log10 (std::max (1e-6, curFixGain[k])));
        r.levelDb = (float) (10.0 * std::log10 (std::max (1e-12, avg)));
        r.overflow = fPs[k] > fPm[k] * 1.0233 && fPs[k] + fPm[k] > 1e-7;   // side > 0.1 dB over mid, band above about -70 dB
        const double lt = lPm[k] + lPs[k];
        r.liveWidth = lt > 1e-14 ? (float) (2.0 * lPs[k] / lt) : 0.0f;
        r.liveLevelDb = (float) (10.0 * std::log10 (std::max (1e-12, lt)));
        r.liveLossDb = (float) listenLossDb (k, lPm[k], lPs[k]);
        r.liveBalance = lt > 1e-14 ? (float) std::clamp (2.0 * lPms[k] / lt, -1.0, 1.0) : 0.0f;
        r.learned = learnedK[k];
        if (learnedK[k] && rPk[k] > 0.0)
        {
            const double bandOnly = curGain[k] / std::max (1e-9, lastBandSetting[k]);    // WIDTH x BEHIND part
            r.limit = (float) std::clamp (1.0 / (bandOnly * std::sqrt (rPk[k])), 0.0, 2.0);
            const double rNow = rPk[k] * curGain[k] * curGain[k];
            r.peakFill = (float) (2.0 * rNow / (1.0 + rNow));
            r.noSide = rPk[k] * (bandOnly * 2.0) * (bandOnly * 2.0) < 0.01;              // even 200 % under ~2 % fill
        }
        return r;
    }
    /** The last block's signal before STAGE (for the WIDTH screen's FFT). */
    const float* preStage (int ch) const noexcept { return ch == 0 ? tapL.data() : tapR.data(); }
    int preStageCount() const noexcept { return tapCount; }

    /** LIMIT: forget the learned peaks (click on the field, preset load, transport start). */
    void resetPeaks() noexcept { for (int k = 0; k < kBands; ++k) { rPk[k] = 0.0; learnedK[k] = false; } }

    float correlation() const
    {
        return (oLL > 1e-14 && oRR > 1e-14) ? (float) std::clamp (oLR / std::sqrt (oLL * oRR), -1.0, 1.0) : 1.0f;
    }
    float matchDb() const { return (float) lastMatchDb; }
    /** Deepest peak-guard gain reduction in the last block, dB (0 = untouched). */
    float guardDb() const { return (float) lastGuardDb; }
    float outputPeak() const { return (float) lastPeak; }

    /** Loss of band k in the current LISTEN mode from its mid / side power (dB, <= 0 mostly). */
    double listenLossDb (int k, double pm, double ps) const
    {
        static constexpr double kCentre[kBands] = { 60, 158, 354, 707, 1414, 2828, 5657, 11314 };
        const double f = kCentre[k], tot = pm + ps;
        if (tot < 1e-14) return 0.0;
        const double monoDb = 10.0 * std::log10 (std::max (1e-6, pm / tot));
        switch (lastListen)
        {
            case Listen::Phone:
                return monoDb + 20.0 * std::log10 (std::max (1e-6, phHp1.magnitudeAt (fs, f) * phHp2.magnitudeAt (fs, f)
                                                                   * phPeak.magnitudeAt (fs, f) * phLp.magnitudeAt (fs, f)));
            case Listen::Speakers:
            {
                const double h = 1.0 / std::sqrt (1.0 + (f / 700.0) * (f / 700.0));   // one-pole 700 Hz
                const double gs = 1.0 - 2.0 * 0.37331 * h;                              // side gain (mid stays 1)
                return 10.0 * std::log10 (std::max (1e-6, (pm + gs * gs * ps) / tot));
            }
            case Listen::Headphones:
                return 20.0 * std::log10 (std::max (1e-6, hpHp[0].magnitudeAt (fs, f) * hpPk[0].magnitudeAt (fs, f) * hpSh[0].magnitudeAt (fs, f)));
            case Listen::Off: case Listen::Mono: default:
                return monoDb;
        }
    }

private:
    void updateStageSplit (double hz)
    {
        if (hz == lastStageHz) return;
        lastStageHz = hz; stSplitM.set (fs, hz); stSplitS.set (fs, hz);   // coefficients only, state kept
    }
    void updateMonoBass (double hz)
    {
        if (hz == lastMonoBassHz) return;
        lastMonoBassHz = hz;
        mbS.set (fs, hz);                      // coefficients only; state kept, so a sweep does not click
        mbM.setAllPass (fs, hz, hs::kButterQ);
        dsMb.setAllPass (fs, hz, hs::kButterQ);
    }

    double fs = 48000;
    std::array<hs::LR4, kBands - 1> xS, xM;
    std::array<std::array<hs::Biquad, kBands - 1>, kBands - 1> apS, apM;
    hs::LR4 mbS; hs::Biquad mbM; double lastMonoBassHz = -1;
    hs::Biquad dHp, dHpR; std::array<hs::SchroederAP, 3> dAp, dApR;
    double fPmAll = 0, fPsAll = 0, edgeCoeff = 0;
    hs::Biquad kIn[2][2], kOut[2][2];
    hs::Biquad phHp1, phHp2, phPeak, phLp;
    hs::Biquad hpHp[2], hpPk[2], hpSh[2];
    Listen lastListen = Listen::Off;
    double sfCoeff = 0, sfL = 0, sfR = 0;
    double powCoeff = 0, bandCoeff = 0, fixCoeff = 0, liveCoeff = 0;
    double lPm[kBands] {}, lPs[kBands] {}, lPms[kBands] {};
    double cPm[kBands] {}, cPs[kBands] {}, rPk[kBands] {}, curGain[kBands] { 1, 1, 1, 1, 1, 1, 1, 1 };
    double lastBandSetting[kBands] { 1, 1, 1, 1, 1, 1, 1, 1 }; bool learnedK[kBands] {};
    double pIn = 0, pOut = 0;
    double bPm[kBands] {}, bPs[kBands] {}, bPms[kBands] {}, fPm[kBands] {}, fPs[kBands] {};
    double curFixGain[kBands] { 1, 1, 1, 1, 1, 1, 1, 1 };
    double oLL = 0, oRR = 0, oLR = 0;
    hs::Smoother widthS, spreadS, outS, matchS, mixS, stageS;
    double lastStage = 0.0, stC = 1.0, stSn = 0.0, lastStageHz = -1.0;
    hs::LR4 stSplitM, stSplitS; hs::Fade stFade;
    std::vector<float> tapL, tapR; int tapCount = 0;
    hs::Biquad dsMb; std::array<hs::Biquad, kBands - 1> dsTree;
    hs::Fade activeS, filtS, mbFade;
    std::array<hs::Smoother, kBands> fixS, bwS;
    double lastPeak = 0, lastMatchDb = 0, lastGuardDb = 0;
    static constexpr double kGuardFloor = 0.89125093813;   // -1 dBFS
    double guardIn = 0, guardEnv = 1.0, guardMin = 1.0, guardRel = 0.0, guardInRel = 0.0;
    double lastBehind = 0, behindGain[kBands] { 1, 1, 1, 1, 1, 1, 1, 1 };
};

} // namespace svl
