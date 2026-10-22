// JING FANG FX rack, after the voices: EQ -> COMP -> DRIVE -> MOD -> ECHO -> REVERB. Plain C++17, no JUCE.
//   EQ     low shelf 120 Hz, a peak (Q 0.9) at 200 Hz..8 kHz, high shelf 6 kHz, +-12 dB each (RBJ biquads, gains smoothed)
//   COMP   stereo-linked feed-forward compressor, soft knee 6 dB, attack / release in the dB domain, makeup
//   MOD    chorus (two voices, 12 ms +- 6 ms), flanger (0.3..4.3 ms, feedback), phaser (six all-passes 200 Hz up to 4 kHz,
//          feedback); a type change fades the stage out and in
//   (the 2.0 layer, work order 20 Oct; each is skipped while it is neutral, so a 1.0 preset is still bit-identical)
//   DRIVE  the studio's hw_tube stage (Faust, faust/jf_drive.dsp): amount = drive 0..24 dB, tone = emphasis + top roll,
//          mix = dry / wet. Small-signal gain 1.
//   ECHO   tempo-synced ping-pong delay (our own, below): the input enters the left line, each line feeds the other; a
//          one-pole damping filter in the loop; feedback up to 0.95; time glides when the tempo changes. Added to the dry.
//   REVERB the studio's room (Faust zita_rev1, faust/jf_reverb.dsp): T60 in seconds, damping, pre-delay, size = low-band
//          decay ratio + width, a 180 Hz high-pass on the send. Added to the dry up to mix 0.5; above it the dry fades (1 = all wet).
// A stage whose mix (and, for the drive, amount) is 0 and has faded out is not run at all, so a preset with all FX off is
// bit-identical to the engine without the rack. prepare() allocates; process() never allocates, locks or does I/O.
#pragma once
#include "SvlDsp.h"
#include "faust/jf_drive.h"
#include "faust/jf_reverb.h"
#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

namespace svl {

class FxRack
{
public:
    void prepare (double sampleRate)
    {
        sr = (float) sampleRate;
        drive = std::make_unique<jf_drive_dsp>(); drive->init ((int) sampleRate);
        reverb = std::make_unique<jf_reverb_dsp>(); reverb->init ((int) sampleRate);
        zDrive = zone (*drive, "drive_db"); zEmph = zone (*drive, "emph_db"); zHf = zone (*drive, "hf_hz");
        zT60 = zone (*reverb, "t60"); zDamp = zone (*reverb, "damp_hz"); zPre = zone (*reverb, "predelay_ms"); zSize = zone (*reverb, "size");
        const int maxDelay = (int) std::ceil (sampleRate * 4.1) + 8;   // 1/2 note at 30 BPM
        int n = 1; while (n < maxDelay) n <<= 1;
        lineL.assign ((size_t) n, 0.0f); lineR.assign ((size_t) n, 0.0f); mask = n - 1; w = 0;
        dampL = dampR = 0;
        // MOD lines: 50 ms is more than the chorus' 18 ms reach
        int m = 1; while (m < (int) std::ceil (sampleRate * 0.05) + 8) m <<= 1;
        modL.assign ((size_t) m, 0.0f); modR.assign ((size_t) m, 0.0f); modMask = m - 1; modW = 0;
        const int ramp = (int) std::lround (0.02 * sampleRate);
        for (auto* s : { &sDriveMix, &sEchoMix, &sRevMix, &sFb, &sEqLow, &sEqMid, &sEqHigh, &sMakeup, &sModMix }) s->setRampLength (ramp);
        echoDelay = -1;
        resetEq(); compEnv = 0; resetMod(); modType = 0; wowPhase = 0;
        snap();
    }

    struct Settings
    {
        float driveAmount = 0, driveTone = 0.5f, driveMix = 1;
        int echoTime = 2; float echoFb = 0.35f, echoDamp = 0.5f, echoMix = 0;
        float revSize = 0.5f, revDecay = 2.0f, revPre = 15, revDamp = 0.5f, revMix = 0;
        float eqLow = 0, eqMid = 0, eqMidF = 1000, eqHigh = 0;
        float compThresh = -12, compRatio = 1, compAttack = 0.01f, compRelease = 0.15f, compMakeup = 0;
        int modType = 0; float modRate = 0.6f, modDepth = 0.5f, modFb = 0.3f, modMix = 0.5f;
        int echoType = 0;   // 0 ping-pong, 1 stereo, 2 mono, 3 tape
    };
    void set (const Settings& s, float bpm, bool snapNow)
    {
        st = s; tempo = bpm;
        // the drive fades in over its first 0.05 of amount, so switching it on does not click
        const float dmix = s.driveMix * std::fmin (1.0f, s.driveAmount / 0.05f);
        auto apply = [&] (Smoother& sm, float v) { if (snapNow) sm.reset (v); else sm.set (v); };
        apply (sDriveMix, dmix); apply (sEchoMix, s.echoMix); apply (sRevMix, s.revMix); apply (sFb, s.echoFb);
        if (zDrive) { *zDrive = 24.0f * s.driveAmount; *zEmph = 8.0f * s.driveTone; *zHf = 5000.0f * std::pow (4.0f, s.driveTone); }
        if (zT60) { *zT60 = s.revDecay; *zDamp = 18000.0f * std::pow (1200.0f / 18000.0f, s.revDamp); *zPre = s.revPre; *zSize = s.revSize; }
        echoTarget = std::fmin ((float) (lineL.size() - 4), sr * 60.0f / std::fmax (20.0f, tempo) * kEchoBeats[juce_clamp (s.echoTime)]);
        if (echoDelay < 0 || snapNow) echoDelay = echoTarget;
        echoDampA = 1.0f - std::exp (-2.0f * (float) kPi * (18000.0f * std::pow (900.0f / 18000.0f, s.echoDamp)) / sr);
        apply (sEqLow, s.eqLow); apply (sEqMid, s.eqMid); apply (sEqHigh, s.eqHigh);
        apply (sMakeup, s.compMakeup);
        compOn = s.compRatio > 1.0001f || s.compMakeup > 0.0f;
        compAtkA = std::exp (-1.0f / (std::fmax (0.0002f, s.compAttack) * sr));
        compRelA = std::exp (-1.0f / (std::fmax (0.005f, s.compRelease) * sr));
        // MOD: the wanted type plays at the set mix; another type first fades the stage out (process switches at silence)
        const float modWant = s.modType > 0 ? s.modMix : 0.0f;
        if (snapNow) { modType = s.modType; sModMix.reset (modWant); resetMod(); }
        else apply (sModMix, s.modType == modType || modType == 0 ? modWant : 0.0f);
        if (modType == 0 && s.modType > 0 && sModMix.cur <= 0.0f) { modType = s.modType; resetMod(); apply (sModMix, modWant); }
        eqDirty = true;
    }
    void snap() { set (st, tempo, true); }

    // in place, n <= the caller's chunk
    void process (float* L, float* R, int n)
    {
        // EQ
        if (sEqLow.cur != 0 || sEqLow.target != 0 || sEqMid.cur != 0 || sEqMid.target != 0 || sEqHigh.cur != 0 || sEqHigh.target != 0)
        {
            float gl = 0, gm = 0, gh = 0;
            for (int i = 0; i < n; ++i) { gl = sEqLow.next(); gm = sEqMid.next(); gh = sEqHigh.next(); }   // coefficients once per chunk
            if (eqDirty || gl != eqGl || gm != eqGm || gh != eqGh) { setEq (gl, gm, gh); eqDirty = false; }
            for (int i = 0; i < n; ++i)
            {
                float l = L[i], r = R[i];
                for (int b = 0; b < 3; ++b) { l = eq[b].process (0, l); r = eq[b].process (1, r); }
                L[i] = l; R[i] = r;
            }
        }
        else if (eqGl != 0 || eqGm != 0 || eqGh != 0) { resetEq(); }
        // COMP
        if (compOn || compEnv < -1.0e-3f || sMakeup.cur > 0)
        {
            const float T = st.compThresh, ratio = std::fmax (1.0f, st.compRatio), W = 6.0f;
            for (int i = 0; i < n; ++i)
            {
                const float x = 20.0f * std::log10 (std::fmax (1.0e-9f, std::fmax (std::fabs (L[i]), std::fabs (R[i]))));
                float y = x;
                if (2.0f * (x - T) > W) y = T + (x - T) / ratio;
                else if (2.0f * (x - T) > -W) { const float d = x - T + 0.5f * W; y = x + (1.0f / ratio - 1.0f) * d * d / (2.0f * W); }
                const float gr = compOn ? y - x : 0.0f;
                const float a = gr < compEnv ? compAtkA : compRelA;
                compEnv = a * compEnv + (1.0f - a) * gr;
                const float g = std::pow (10.0f, (compEnv + sMakeup.next()) / 20.0f);
                L[i] *= g; R[i] *= g;
            }
        }
        // DRIVE
        if (sDriveMix.cur > 0 || sDriveMix.target > 0)
        {
            float dl[kMax], dr[kMax];
            std::memcpy (dl, L, sizeof (float) * (size_t) n); std::memcpy (dr, R, sizeof (float) * (size_t) n);
            float* in[2] = { dl, dr }; float* out[2] = { dl, dr };
            drive->compute (n, in, out);
            for (int i = 0; i < n; ++i)
            {
                const float m = sDriveMix.next();
                L[i] += m * (dl[i] - L[i]); R[i] += m * (dr[i] - R[i]);
            }
        }
        // MOD
        if (sModMix.cur > 0 || sModMix.target > 0)
        {
            const float rate = st.modRate, depth = st.modDepth, fb = std::fmin (0.9f, st.modFb);
            for (int i = 0; i < n; ++i)
            {
                const float m = sModMix.next();
                modPhase += rate / sr; if (modPhase >= 1.0f) modPhase -= 1.0f;
                const float sl = std::sin (2.0f * (float) kPi * modPhase), sr_ = std::sin (2.0f * (float) kPi * (modPhase + 0.25f));
                const float dl = L[i], dr = R[i];
                float wl = dl, wr = dr;
                if (modType == 1 || modType == 2)
                {   // chorus: 12 ms +- 6 ms x depth; flanger: 0.3..4.3 ms with feedback
                    const float base = modType == 1 ? 0.012f : 0.0023f, swing = modType == 1 ? 0.006f : 0.002f;
                    const float tl = (base + swing * depth * sl) * sr, tr = (base + swing * depth * sr_) * sr;
                    wl = readMod (modL, tl); wr = readMod (modR, tr);
                    const float k = modType == 2 ? fb : 0.0f;
                    modL[(size_t) modW] = dl + k * wl; modR[(size_t) modW] = dr + k * wr;
                    modW = (modW + 1) & modMask;
                }
                else if (modType == 3)
                {   // phaser: six first-order all-passes swept 200 Hz .. 200 + 3800 x depth Hz (exponential), feedback from the last
                    const float top = 200.0f + 3800.0f * depth;
                    const float fL = 200.0f * std::pow (top / 200.0f, 0.5f + 0.5f * sl), fR = 200.0f * std::pow (top / 200.0f, 0.5f + 0.5f * sr_);
                    const float tl = std::tan ((float) kPi * std::fmin (fL, 0.45f * sr) / sr), trr = std::tan ((float) kPi * std::fmin (fR, 0.45f * sr) / sr);
                    const float al = (tl - 1.0f) / (tl + 1.0f), ar = (trr - 1.0f) / (trr + 1.0f);
                    float xl = dl + fb * phFbL, xr = dr + fb * phFbR;
                    for (int s2 = 0; s2 < 6; ++s2)
                    {
                        const float yl = al * xl + phZL[s2]; phZL[s2] = xl - al * yl; xl = yl;
                        const float yr = ar * xr + phZR[s2]; phZR[s2] = xr - ar * yr; xr = yr;
                    }
                    phFbL = xl; phFbR = xr; wl = xl; wr = xr;
                }
                L[i] = dl + m * 0.5f * (wl - dl); R[i] = dr + m * 0.5f * (wr - dr);
            }
            if (sModMix.cur <= 0 && sModMix.target <= 0 && modType != st.modType)
            {   // faded out for a type change: switch at silence
                modType = st.modType; resetMod();
                if (modType > 0) sModMix.set (st.modMix);
            }
        }
        // ECHO
        if (sEchoMix.cur > 0 || sEchoMix.target > 0)
        {
            const float glide = 1.0f - std::exp (-1.0f / (0.08f * sr));   // 80 ms to a new tempo
            const int type = st.echoType;
            const float dampA = type == 3 ? echoDampA * 0.55f : echoDampA;   // tape: a darker loop
            for (int i = 0; i < n; ++i)
            {
                echoDelay += glide * (echoTarget - echoDelay);
                const float fb = sFb.next(), m = sEchoMix.next();
                float d = echoDelay;
                if (type == 3)
                {   // tape: wow, 0.6 Hz, +-0.35 % of the time
                    wowPhase += 0.6f / sr; if (wowPhase >= 1.0f) wowPhase -= 1.0f;
                    d *= 1.0f + 0.0035f * std::sin (2.0f * (float) kPi * wowPhase);
                }
                const float ol = read (lineL, d), orr = read (lineR, d);
                dampL += dampA * (ol - dampL); dampR += dampA * (orr - dampR);
                if (type == 0)
                {
                    lineL[(size_t) w] = 0.5f * (L[i] + R[i]) + fb * dampR;   // ping: the input and the right line into the left
                    lineR[(size_t) w] = fb * dampL;                         // pong: the left line into the right
                    w = (w + 1) & mask;
                    L[i] += m * ol; R[i] += m * orr;
                }
                else if (type == 1)
                {   // stereo: each side its own line
                    lineL[(size_t) w] = L[i] + fb * dampL;
                    lineR[(size_t) w] = R[i] + fb * dampR;
                    w = (w + 1) & mask;
                    L[i] += m * ol; R[i] += m * orr;
                }
                else
                {   // mono / tape: one line, in the middle
                    lineL[(size_t) w] = 0.5f * (L[i] + R[i]) + fb * dampL;
                    lineR[(size_t) w] = 0.0f;
                    w = (w + 1) & mask;
                    L[i] += m * ol; R[i] += m * ol;
                }
            }
        }
        // REVERB
        if (sRevMix.cur > 0 || sRevMix.target > 0)
        {
            float wl[kMax], wr[kMax];
            float* in[2] = { L, R }; float* out[2] = { wl, wr };
            reverb->compute (n, in, out);
            for (int i = 0; i < n; ++i)
            {
                // up to mix 0.5 the dry stays at full level and the room is added; above it the dry fades (1 = all wet)
                const float m = sRevMix.next(), dry = std::fmin (1.0f, 2.0f * (1.0f - m));
                L[i] = dry * L[i] + m * wl[i]; R[i] = dry * R[i] + m * wr[i];
            }
        }
    }

    float reductionDb() const { return -compEnv; }   // the compressor's gain reduction now (dB, >= 0), for the panel
    static constexpr int kMax = 64;   // the engine's chunk (32) fits
    static constexpr float kEchoBeats[6] = { 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f };

private:
    static int juce_clamp (int i) { return i < 0 ? 0 : (i > 5 ? 5 : i); }
    template <class D> static FAUSTFLOAT* zone (D& d, const char* label)
    {
        ZoneFinder f; f.want = label; d.buildUserInterface (&f); return f.found;
    }
    struct Biquad
    {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1[2] {}, z2[2] {};
        float process (int ch, float x) { const float y = b0 * x + z1[ch]; z1[ch] = b1 * x - a1 * y + z2[ch]; z2[ch] = b2 * x - a2 * y; return y; }
        void reset() { z1[0] = z1[1] = z2[0] = z2[1] = 0; }
        void set (float B0, float B1, float B2, float A0, float A1, float A2) { b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0; }
    };
    void resetEq() { for (auto& b : eq) { b.reset(); b.set (1, 0, 0, 1, 0, 0); } eqGl = eqGm = eqGh = 0; eqDirty = true; }
    void setEq (float gl, float gm, float gh)
    {   // RBJ cookbook: shelves with S = 1, the peak with Q 0.9
        eqGl = gl; eqGm = gm; eqGh = gh;
        {
            const float A = std::pow (10.0f, gl / 40.0f), w0 = 2.0f * (float) kPi * 120.0f / sr, c = std::cos (w0), al = std::sin (w0) * 0.5f * std::sqrt (2.0f), sa = 2.0f * std::sqrt (A) * al;
            eq[0].set (A * ((A + 1) - (A - 1) * c + sa), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sa),
                       (A + 1) + (A - 1) * c + sa, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sa);
        }
        {
            const float f = std::fmin (st.eqMidF, 0.45f * sr);
            const float A = std::pow (10.0f, gm / 40.0f), w0 = 2.0f * (float) kPi * f / sr, c = std::cos (w0), al = std::sin (w0) / (2.0f * 0.9f);
            eq[1].set (1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
        }
        {
            const float A = std::pow (10.0f, gh / 40.0f), w0 = 2.0f * (float) kPi * std::fmin (6000.0f, 0.45f * sr) / sr, c = std::cos (w0), al = std::sin (w0) * 0.5f * std::sqrt (2.0f), sa = 2.0f * std::sqrt (A) * al;
            eq[2].set (A * ((A + 1) + (A - 1) * c + sa), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sa),
                       (A + 1) - (A - 1) * c + sa, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sa);
        }
    }
    void resetMod()
    {
        std::fill (modL.begin(), modL.end(), 0.0f); std::fill (modR.begin(), modR.end(), 0.0f); modW = 0; modPhase = 0;
        for (int i = 0; i < 6; ++i) phZL[i] = phZR[i] = 0;
        phFbL = phFbR = 0;
    }
    float readMod (const std::vector<float>& b, float d) const
    {
        const int di = (int) d; const float fr = d - (float) di;
        const float a = b[(size_t) ((modW - di) & modMask)], c = b[(size_t) ((modW - di - 1) & modMask)];
        return a + fr * (c - a);
    }
    float read (const std::vector<float>& b, float d) const
    {
        const int di = (int) d; const float fr = d - (float) di;
        const float a = b[(size_t) ((w - di) & mask)], c = b[(size_t) ((w - di - 1) & mask)];
        return a + fr * (c - a);
    }

    float sr = 44100, tempo = 120;
    Settings st;
    std::unique_ptr<jf_drive_dsp> drive;
    std::unique_ptr<jf_reverb_dsp> reverb;
    FAUSTFLOAT *zDrive = nullptr, *zEmph = nullptr, *zHf = nullptr, *zT60 = nullptr, *zDamp = nullptr, *zPre = nullptr, *zSize = nullptr;
    std::vector<float> lineL, lineR;
    int mask = 0, w = 0;
    float echoDelay = -1, echoTarget = 0, echoDampA = 0.5f, dampL = 0, dampR = 0;
    Smoother sDriveMix, sEchoMix, sRevMix, sFb, sEqLow, sEqMid, sEqHigh, sMakeup, sModMix;
    Biquad eq[3];
    float eqGl = 0, eqGm = 0, eqGh = 0; bool eqDirty = true;
    bool compOn = false; float compEnv = 0, compAtkA = 0.99f, compRelA = 0.999f;
    std::vector<float> modL, modR; int modMask = 0, modW = 0, modType = 0;
    float modPhase = 0, phZL[6] {}, phZR[6] {}, phFbL = 0, phFbR = 0, wowPhase = 0;
};

} // namespace svl
