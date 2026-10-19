// JING FANG FX rack, after the voices: DRIVE -> ECHO -> REVERB. Plain C++17, no JUCE.
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
        const int ramp = (int) std::lround (0.02 * sampleRate);
        for (auto* s : { &sDriveMix, &sEchoMix, &sRevMix, &sFb }) s->setRampLength (ramp);
        echoDelay = -1;
        snap();
    }

    struct Settings
    {
        float driveAmount = 0, driveTone = 0.5f, driveMix = 1;
        int echoTime = 2; float echoFb = 0.35f, echoDamp = 0.5f, echoMix = 0;
        float revSize = 0.5f, revDecay = 2.0f, revPre = 15, revDamp = 0.5f, revMix = 0;
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
    }
    void snap() { set (st, tempo, true); }

    // in place, n <= the caller's chunk
    void process (float* L, float* R, int n)
    {
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
        // ECHO
        if (sEchoMix.cur > 0 || sEchoMix.target > 0)
        {
            const float glide = 1.0f - std::exp (-1.0f / (0.08f * sr));   // 80 ms to a new tempo
            for (int i = 0; i < n; ++i)
            {
                echoDelay += glide * (echoTarget - echoDelay);
                const float fb = sFb.next(), m = sEchoMix.next();
                const float ol = read (lineL, echoDelay), orr = read (lineR, echoDelay);
                dampL += echoDampA * (ol - dampL); dampR += echoDampA * (orr - dampR);
                lineL[(size_t) w] = 0.5f * (L[i] + R[i]) + fb * dampR;   // ping: the input and the right line into the left
                lineR[(size_t) w] = fb * dampL;                         // pong: the left line into the right
                w = (w + 1) & mask;
                L[i] += m * ol; R[i] += m * orr;
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

    static constexpr int kMax = 64;   // the engine's chunk (32) fits
    static constexpr float kEchoBeats[6] = { 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f };

private:
    static int juce_clamp (int i) { return i < 0 ? 0 : (i > 5 ? 5 : i); }
    template <class D> static FAUSTFLOAT* zone (D& d, const char* label)
    {
        ZoneFinder f; f.want = label; d.buildUserInterface (&f); return f.found;
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
    Smoother sDriveMix, sEchoMix, sRevMix, sFb;
};

} // namespace svl
