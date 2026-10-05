// TUTKAL engine: a drum and mix-bus compressor. Plain C++, no JUCE, so the prototype tool,
// the measurements and the plugin all run this one file.
//
// Feed-forward in the log domain: level -> static curve -> gain reduction -> attack/release
// on the gain reduction -> gain. Nothing is fed back from the output, so a transient cannot
// make the gain reduction overshoot. See SPEC.md.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#ifndef M_PI   // MSVC does not define it without _USE_MATH_DEFINES (Windows CI build, 2026-10-05)
#define M_PI 3.14159265358979323846
#endif

namespace tutkal
{
enum Voicing : int { kCore = 0, kGlue = 1, kLa = 2, kPump = 3 };

struct Params
{
    float thresholdDb = -18.0f;   // -40 .. 0
    float ratio       = 4.0f;     // 1.5 .. 20
    float attackMs    = 10.0f;    // 0.1 .. 30, time to 63 %
    float releaseMs   = 300.0f;   // 100 .. 1200, time to 63 %
    bool  autoRelease = false;    // two-path release with memory
    float makeupDb    = 0.0f;     // 0 .. 20
    float mix         = 1.0f;     // 0 .. 1
    float scHpfHz     = -1.0f;    // below 0: the voicing's own default (GLUE 200 Hz, every other voicing off); 0 .. 19: off; 20 .. 300: that cutoff
    bool  ext         = false;    // the detector hears only the sidechain
    float drive       = 0.0f;     // 0 .. 1, output transformer
    bool  in          = true;     // false: bypass (the input, delayed by the latency)

    // VOICING (SPEC.md, CHARACTER). kCore is the plain measured engine (M1-M18); the shell defaults to kGlue.
    int   voicing       = kGlue;
    // LA: PEAK REDUCTION replaces THRESHOLD, GAIN is makeupDb, COMP/LIMIT is `limit`. RATIO/ATTACK/RELEASE/AUTO/SC are ignored.
    float peakReduction = 0.5f;   // 0 .. 1  (threshold -45 dB * value)
    bool  limit         = false;  // false COMPRESS (~3:1), true LIMIT (~12:1)
    // PUMP: THRESHOLD and EXT as usual, RATIO limited to 10 .. 20, ATTACK to <= 1 ms, RELEASE replaced by the note below.
    float bpm           = 120.0f; // host tempo
    float pumpNote      = 0.125f; // recovery length as a fraction of a whole note: 1/16 .. 1/2
    int   pumpShape     = 0;      // 0 exp, 1 linear, 2 swell
    float depthDb       = 12.0f;  // 0 .. 24, the duck never goes deeper than this
};

constexpr int   kLatency  = 63;       // samples, at every sample rate
constexpr float kMaxGrDb  = 60.0f;    // a ceiling for the envelope states

// Voicing constants. Every number here is placed by a measurement in tools/measure.py (G*, L*, P*).
constexpr float kGlueRmsMs = 5.0f, kGlueSlowRmsMs = 100.0f;                      // GLUE detector: two mean-square windows
constexpr float kGlueWPeak = 0.05f, kGlueWRms = 0.10f;                              // ... blended with the peak; the rest (0.85) is the 100 ms mean square
constexpr float kGlueCapDb = 1.5f;                                                  // the fast stage may add at most this much gain reduction over the slow one
constexpr float kGlueAutoFastS = 0.4f, kGlueSlowAttackS = 0.8f, kGlueSlowReleaseS = 1.6f, kGlueSlowReleaseK = 4.0f;   // stage times (tau, s)
constexpr float kGlueDetDb = 11.0f;                                                 // GLUE detector level offset: the RMS-weighted, SC-HPF'd level reads ~11 dB under a peak bus compressor's; with it THRESHOLD/RATIO give the GR of a classic bus comp at the same settings (owner 2026-10-04: "Glue at -18 compresses, TUTKAL barely does")
constexpr float kGlueHpfHz = 200.0f;                                                 // GLUE's default sidechain high-pass
constexpr float kGlueC2 = 0.0034f, kGlueC3 = 0.0072f;                                 // GLUE colour per 4 dB of gain reduction
constexpr float kLaRmsMs = 6.0f, kLaAttackMs = 12.0f, kLaFastMs = 55.0f;         // LA detector, slowest attack, fast release stage (tau)
constexpr float kLaChargeS = 0.25f, kLaMemS = 3.0f, kLaForgetS = 30.0f;          // slow stage fills, press memory fills, press memory fades
constexpr float kLaSlowMinS = 1.0f, kLaSlowSpanS = 11.0f;                        // slow stage tau = min + span * memory
constexpr float kLaPeakRedDb = 45.0f, kLaCompRatio = 3.0f, kLaLimitRatio = 12.0f, kLaCompKnee = 18.0f, kLaLimitKnee = 10.0f;
constexpr float kLaC2 = 0.011f, kLaC3 = 0.0024f;                                     // LA colour per 6 dB of gain reduction
constexpr float kColourFloor = 0.02f, kColourEnvMs = 60.0f;                      // colour reference: recent peak, never below -34 dBFS
constexpr float kPumpDecayMs = 2.0f, kPumpHoldMs = 15.0f, kPumpAttackMs = 0.7f;   // PUMP detector, attack ceiling                        // PUMP detector

/** Knee width for a ratio: wide at gentle ratios, tight at steep ones. */
inline float kneeFor (float ratio) noexcept
{
    return std::clamp (10.0f * std::pow (2.0f / ratio, 0.7f), 1.5f, 12.0f);
}

/** Gain reduction in dB for a level `over` dB above the threshold. */
inline float staticGr (float over, float ratio, float knee) noexcept
{
    const float slope = 1.0f - 1.0f / ratio;
    if (2.0f * over < -knee) return 0.0f;
    if (2.0f * over <= knee) { const float t = over + 0.5f * knee; return slope * t * t / (2.0f * knee); }
    return slope * over;
}

/** Linear-phase FIR used by the 4x oversampler: a Kaiser-windowed half-band low-pass. */
class HalfBand
{
public:
    void design (int taps, double beta, double gain)
    {
        h.assign ((size_t) taps, 0.0f);
        const int mid = (taps - 1) / 2;
        auto bessel = [] (double x) { double s = 1.0, t = 1.0; for (int k = 1; k < 40; ++k) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; } return s; };
        double sum = 0.0;
        std::vector<double> d ((size_t) taps);
        for (int n = 0; n < taps; ++n)
        {
            const double m = n - mid;
            const double sinc = m == 0 ? 0.5 : std::sin (0.5 * M_PI * m) / (M_PI * m);
            const double r = (double) m / mid;
            d[(size_t) n] = sinc * bessel (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / bessel (beta);
            sum += d[(size_t) n];
        }
        for (int n = 0; n < taps; ++n) h[(size_t) n] = (float) (d[(size_t) n] / sum * gain);
        z.assign ((size_t) taps, 0.0f);
        pos = 0;
    }
    void reset() noexcept { std::fill (z.begin(), z.end(), 0.0f); pos = 0; }
    inline void push (float x) noexcept { z[(size_t) pos] = x; if (++pos == (int) z.size()) pos = 0; }
    inline float out() const noexcept
    {
        // z[pos] is the oldest sample.
        const int n = (int) z.size();
        float acc = 0.0f;
        int k = pos;
        for (int i = 0; i < n; ++i) { acc += h[(size_t) (n - 1 - i)] * z[(size_t) k]; if (++k == n) k = 0; }
        return acc;
    }

private:
    std::vector<float> h, z;
    int pos = 0;
};

/** One channel of the output stage: low shelf -> 4x oversampled tanh -> inverse shelf.
    Its latency is kLatency samples whatever the sample rate. */
class DriveStage
{
public:
    void prepare (double fs)
    {
        // Counted in samples at 4x: stage A (1x <-> 2x, 115 taps) delays 114 going up and 114 coming
        // down; stage B (2x <-> 4x, 23 taps) 11 and 11. Each decimator hands over its newest output,
        // which is one sample early at its own rate (1 and 2 at 4x). 114 + 11 + 11 - 1 + 114 - 2 = 247;
        // five more at 4x make 252 = 63 samples at 1x.
        upA.design (115, 9.0, 2.0); downA.design (115, 9.0, 1.0);
        upB.design (23, 9.0, 2.0);  downB.design (23, 9.0, 1.0);
        shelfA = 1.0f - (float) std::exp (-2.0 * M_PI * 150.0 / fs);
        meanA  = 1.0f - (float) std::exp (-2.0 * M_PI * 8.0 / (4.0 * fs));
        reset();
    }
    void reset() noexcept
    {
        upA.reset(); downA.reset(); upB.reset(); downB.reset();
        pre = post = eMean = 0.0f;
        std::fill (std::begin (pad), std::end (pad), 0.0f);
        std::fill (std::begin (preLine), std::end (preLine), 0.0f);
        prePos = 0;
    }

    /** k: drive (0 = linear). c2, c3: the voicing colour, a 2nd and a 3rd order term (0 = none), shaped on the
        signal normalised by `ref` (its recent peak), so the harmonics are a fixed share of the signal at any level.
        Always run, so the path is warm when DRIVE leaves zero. */
    inline float process (float x, float k, float c2 = 0.0f, float c3 = 0.0f, float ref = 1.0f) noexcept
    {
        // Transformer cores saturate on flux, which grows as the frequency falls: lift the lows
        // into the nonlinearity and take the same lift out after it.
        pre += shelfA * (x - pre);
        if (std::abs (pre) < 1.0e-20f) pre = 0.0f;            // silence in must become exact silence out
        const float u = x + shelfE * pre;
        const float preDelayed = preLine[prePos];            // `pre`, kLatency samples ago: lines up with the oversampled output
        preLine[prePos] = pre;
        if (++prePos == kLatency) prePos = 0;
        const bool colour = c2 != 0.0f || c3 != 0.0f;

        float y = 0.0f;
        for (int a = 0; a < 2; ++a)                 // 1x -> 2x
        {
            upA.push (a == 0 ? u : 0.0f);
            const float a2 = upA.out();
            for (int b = 0; b < 2; ++b)             // 2x -> 4x
            {
                upB.push (b == 0 ? a2 : 0.0f);
                const float v = upB.out();
                float s = k > 1.0e-4f ? std::tanh (k * v) / k : v;
                if (colour)
                {
                    // The colour is taken from the signal without the low lift, so it does not run away on bass.
                    const float q = std::clamp ((v - shelfE * preDelayed) / ref, -2.0f, 2.0f);
                    const float e = ref * c2 * q * q;
                    eMean += meanA * (e - eMean);            // the square has a DC part; only its 2nd harmonic stays
                    if (std::abs (eMean) < 1.0e-20f) eMean = 0.0f;
                    s += (e - eMean) + ref * c3 * q * q * q;
                }
                else
                {
                    eMean += meanA * (0.0f - eMean);
                    if (std::abs (eMean) < 1.0e-20f) eMean = 0.0f;
                }
                downB.push (pad[kPad - 1]);
                for (int i = kPad - 1; i > 0; --i) pad[i] = pad[i - 1];
                pad[0] = s;
            }
            downA.push (downB.out());               // 4x -> 2x, every second sample
        }
        y = downA.out();                            // 2x -> 1x

        // Exact inverse of the shelf, with no delay: y = w + e lp(w)  =>  w.
        const float w = (y - shelfE * (1.0f - shelfA) * post) / (1.0f + shelfE * shelfA);
        post += shelfA * (w - post);
        if (std::abs (post) < 1.0e-20f) post = 0.0f;
        return w;
    }

private:
    static constexpr float shelfE = 2.0f;           // lows enter the core 3x (+9.5 dB) hotter
    HalfBand upA, downA, upB, downB;
    static constexpr int kPad = 5;
    float shelfA = 0.0f, meanA = 0.0f, pre = 0.0f, post = 0.0f, eMean = 0.0f, pad[kPad] {};
    float preLine[kLatency] {};
    int prePos = 0;
};

class Engine
{
public:
    void prepare (double sampleRate)
    {
        fs = sampleRate > 0.0 ? sampleRate : 48000.0;
        for (auto& d : drive) d.prepare (fs);
        smooth = 1.0f - (float) std::exp (-1.0 / (0.020 * fs));
        fade   = 1.0f - (float) std::exp (-1.0 / (0.005 * fs));
        aAutoFast   = (float) std::exp (-1.0 / (0.090 * fs));
        aAutoCharge = (float) std::exp (-1.0 / (0.800 * fs));
        aAutoSlow   = (float) std::exp (-1.0 / (1.600 * fs));
        cMsGlue  = 1.0f - (float) std::exp (-1.0 / (kGlueRmsMs * 0.001 * fs));
        cMsLa    = 1.0f - (float) std::exp (-1.0 / (kLaRmsMs * 0.001 * fs));
        cMsGlueSlow = 1.0f - (float) std::exp (-1.0 / (kGlueSlowRmsMs * 0.001 * fs));
        aGlueSlowA = (float) std::exp (-1.0 / (kGlueSlowAttackS * fs));
        aGlueFastAuto = (float) std::exp (-1.0 / (kGlueAutoFastS * fs));
        aGlueSlowAuto = (float) std::exp (-1.0 / (kGlueSlowReleaseS * fs));
        aLaFast  = (float) std::exp (-1.0 / (kLaFastMs * 0.001 * fs));
        cLaCharge = 1.0f - (float) std::exp (-1.0 / (kLaChargeS * fs));
        cLaMem   = 1.0f - (float) std::exp (-1.0 / (kLaMemS * fs));
        cLaMemDown = 1.0f - (float) std::exp (-1.0 / (kLaForgetS * fs));
        aPumpDec = (float) std::exp (-1.0 / (kPumpDecayMs * 0.001 * fs));
        pumpHoldN = (int) (kPumpHoldMs * 0.001 * fs);
        aEnvC = (float) std::exp (-1.0 / (kColourEnvMs * 0.001 * fs));
        reset();
    }

    void reset() noexcept
    {
        for (auto& d : drive) d.reset();
        for (auto& l : dryLine) std::fill (std::begin (l), std::end (l), 0.0f);
        for (auto& l : wetLine) std::fill (std::begin (l), std::end (l), 0.0f);
        linePos = 0;
        for (auto& s : hp) s = {};
        y1 = yFast = ySlow = yL = 0.0f;
        msGlue = msGlueSlow = msLa = laFast = laSlow = laMem = 0.0f; gFast = gSlow = 0.0f;
        pkEnv = pumpY = pumpY0 = pumpP = 0.0f; pumpHold = 0; envC[0] = envC[1] = 0.0f;
        voicingPrev = -1;
        first = true;
        count = 0;
        grDb = inPeak = outPeak = 0.0f;
    }

    static constexpr int latency() noexcept { return kLatency; }

    /** In place. `io` holds 1 or 2 channels; `key` (may be null) holds 0, 1 or 2 channels of
        sidechain. `grOut` (may be null) receives the gain reduction in dB for every sample. */
    void process (float* const* io, int numChannels, const float* const* key, int keyChannels,
                  int numSamples, const Params& p, float* grOut = nullptr) noexcept
    {
        numChannels = std::clamp (numChannels, 1, 2);
        if (key == nullptr) keyChannels = 0;
        keyChannels = std::clamp (keyChannels, 0, 2);

        const float tThr   = std::clamp (p.thresholdDb, -60.0f, 0.0f);
        const float tRatio = std::clamp (p.ratio, 1.0f, 100.0f);
        const float tMake  = std::clamp (p.makeupDb, -24.0f, 24.0f);
        const float tMix   = std::clamp (p.mix, 0.0f, 1.0f);
        const float tDrive = std::clamp (p.drive, 0.0f, 1.0f);
        const float tIn    = p.in ? 1.0f : 0.0f;
        const float tAtt   = std::clamp (p.attackMs, 0.02f, 200.0f);
        const float tRel   = std::clamp (p.releaseMs, 20.0f, 5000.0f);
        const float hpfReq = p.scHpfHz < 0.0f ? (p.voicing == kGlue ? kGlueHpfHz : 0.0f) : p.scHpfHz;
        const float tHpf   = hpfReq < 20.0f ? 0.0f : std::min (hpfReq, 1000.0f);
        const float tPr    = std::clamp (p.peakReduction, 0.0f, 1.0f);
        const float tDepth = std::clamp (p.depthDb, 0.0f, 48.0f);
        rBpm = std::clamp (p.bpm, 20.0f, 300.0f); rNote = std::clamp (p.pumpNote, 0.01f, 2.0f);
        pumpShape = std::clamp (p.pumpShape, 0, 2);

        if (first)
        {
            sThr = tThr; sRatio = tRatio; sMake = tMake; sMix = tMix; sDrive = tDrive; sIn = tIn;
            sAtt = tAtt; sRel = tRel; sHpf = tHpf; sPr = tPr; sDepth = tDepth;
            first = false;
            refresh();
        }

        float peakIn = 0.0f, peakOut = 0.0f;
        for (int i = 0; i < numSamples; ++i)
        {
            // Controls glide; everything is keyed to the sample count, never to the block, so
            // any block size renders the same bits.
            sThr   += smooth * (tThr - sThr);
            sRatio += smooth * (tRatio - sRatio);
            sMake  += smooth * (tMake - sMake);
            sMix   += smooth * (tMix - sMix);
            sDrive += smooth * (tDrive - sDrive);
            sIn    += fade * (tIn - sIn);
            sAtt   += smooth * (tAtt - sAtt);
            sRel   += smooth * (tRel - sRel);
            sHpf   += smooth * (tHpf - sHpf);
            sPr    += smooth * (tPr - sPr);
            sDepth += smooth * (tDepth - sDepth);
            if ((count++ & 15) == 0)
                refresh();

            float x[2];
            for (int c = 0; c < numChannels; ++c)
            {
                const float v = io[c][i];
                x[c] = std::isfinite (v) ? std::clamp (v, -100.0f, 100.0f) : 0.0f;   // a host may send anything
                peakIn = std::max (peakIn, std::abs (x[c]));
            }
            if (numChannels == 1) x[1] = x[0];

            // Detector: the programme, or with EXT only the sidechain (nothing routed: silence).
            float d[2] { x[0], x[1] };
            if (p.ext)
            {
                d[0] = keyChannels > 0 ? sane (key[0][i]) : 0.0f;
                d[1] = keyChannels > 1 ? sane (key[1][i]) : d[0];
            }
            if (hpOn)
                for (int c = 0; c < 2; ++c)
                    d[c] = hp[c].tick (d[c], hpB0, hpB1, hpB2, hpA1, hpA2);
            const float pk = std::max (std::abs (d[0]), std::abs (d[1]));

            // A voicing change hands the gain reduction over, so the gain does not jump.
            if (p.voicing != voicingPrev)
            {
                y1 = yFast = ySlow = laFast = laSlow = pumpY = pumpY0 = gFast = gSlow = yL;
                pumpP = 0.0f; laMem = 0.0f; voicingPrev = p.voicing;
            }
            const int voicing = std::clamp (p.voicing, 0, 3);

            // Mean-square trackers run always, so a voicing switch finds them warm.
            msGlue += cMsGlue * (pk * pk - msGlue);
            msLa   += cMsLa   * (pk * pk - msLa);
            msGlueSlow += cMsGlueSlow * (pk * pk - msGlueSlow);
            if (msGlue < 1.0e-30f) msGlue = 0.0f;
            if (msGlueSlow < 1.0e-30f) msGlueSlow = 0.0f;
            if (msLa   < 1.0e-30f) msLa   = 0.0f;

            float c2 = 0.0f, c3 = 0.0f;       // voicing colour for this sample

            if (voicing == kLa)
            {
                // Opto cell. PEAK REDUCTION sets how hard the cell is driven; ratio is fixed by COMP/LIMIT, the knee is very soft.
                const float lvl = std::sqrt (2.0f * msLa);
                const float lvlDb = 20.0f * std::log10 (std::max (lvl, 1.0e-7f));
                const float ratio = p.limit ? kLaLimitRatio : kLaCompRatio;
                const float kn = p.limit ? kLaLimitKnee : kLaCompKnee;
                const float target = std::min (staticGr (lvlDb + kLaPeakRedDb * sPr, ratio, kn), kMaxGrDb);

                // Two-stage release with memory: a fast stage (about 60 ms to half) and a slow stage that
                // fills while the cell is pressed and then lets go with a time constant of 1 .. 12 s by how long and how hard.
                laFast = std::max (target, aLaFast * laFast + (1.0f - aLaFast) * target);
                const float half = 0.5f * target;
                if (half > laSlow) laSlow += cLaCharge * (half - laSlow);
                else               laSlow = half + aLaSlow * (laSlow - half);
                if (target > 0.5f) laMem += cLaMem * std::min (1.0f, target / 6.0f) * (1.0f - laMem);
                else               laMem -= cLaMemDown * laMem;
                y1 = std::max (laFast, laSlow);
                if (y1 < 1.0e-5f && target <= 0.0f) { laFast = laSlow = y1 = 0.0f; }
                // Program-dependent attack: about 12 ms for a gentle push, a few ms for a hard one.
                const float tauA = std::clamp (kLaAttackMs / (1.0f + y1 / 6.0f), 3.0f, kLaAttackMs);
                const float aA = std::exp (-1.0f / (0.001f * tauA * (float) fs));
                yL = aA * yL + (1.0f - aA) * y1;
                yFast = ySlow = y1;
                if (yL < 1.0e-5f && y1 < 1.0e-5f) yL = 0.0f;
                const float g = std::clamp (yL / 6.0f, 0.0f, 3.0f);
                c2 = kLaC2 * g; c3 = kLaC3 * g;
            }
            else if (voicing == kPump)
            {
                // Peak hold keeps a kick's own cycles from re-triggering the recovery; then a fast drop.
                if (pk >= pkEnv) { pkEnv = pk; pumpHold = pumpHoldN; }
                else if (pumpHold > 0) --pumpHold;
                else pkEnv *= aPumpDec;
                if (pkEnv < 1.0e-9f) pkEnv = 0.0f;
                const float lvlDb = 20.0f * std::log10 (std::max (pkEnv, 1.0e-7f));
                const float ratio = std::clamp (sRatio, 10.0f, 20.0f);
                const float target = std::min (staticGr (lvlDb - sThr, ratio, kneeFor (ratio)), sDepth);

                // Recovery on a clock: from the moment the key lets go, the duck falls on the chosen shape and is 90 % gone
                // after exactly one note value.
                if (target >= pumpY) { pumpY = target; pumpY0 = target; pumpP = 0.0f; }
                else
                {
                    pumpP += pumpStep;
                    float sh;
                    if (pumpShape == 1)      sh = std::max (0.0f, 1.0f - 0.9f * pumpP);
                    else if (pumpShape == 2) sh = std::max (0.0f, 1.0f - 0.9f * pumpP * pumpP);
                    else                     sh = std::exp (-2.30258509f * pumpP);
                    pumpY = std::max (target, pumpY0 * sh);
                    if (pumpY < 1.0e-4f && target <= 0.0f) pumpY = pumpY0 = 0.0f;
                }
                yL = aAttPump * yL + (1.0f - aAttPump) * pumpY;
                y1 = yFast = ySlow = pumpY;
                if (yL < 1.0e-5f && pumpY < 1.0e-5f) yL = 0.0f;
            }
            else if (voicing == kGlue)
            {
                // Two stages on one detector. The detector is mostly a 100 ms mean square (the programme, not the single hit), with a
                // little 5 ms mean square and peak. The SLOW stage follows the programme (attack 0.8 s, release 1.6 s or 4 x the knob):
                // it is the glue, a gain reduction that does not swing back to zero between hits. The FAST stage (attack knob,
                // release knob, AUTO 0.4 s) may add at most kGlueCapDb over the slow one: the hit's own grab, bounded,
                // so a hit never leaves its own ducked shadow. Both rise and fall toward the target (attack before release).
                const float level = std::sqrt (kGlueWPeak * pk * pk + kGlueWRms * 2.0f * msGlue + (1.0f - kGlueWPeak - kGlueWRms) * 2.0f * msGlueSlow);
                const float levelDb = 20.0f * std::log10 (std::max (level, 1.0e-7f)) + kGlueDetDb;
                const float target = std::min (staticGr (levelDb - sThr, sRatio, knee), kMaxGrDb - kGlueCapDb);   // slow + cap never passes kMaxGrDb
                const float aFastRel = p.autoRelease ? aGlueFastAuto : aRel;
                const float aSlowRel = p.autoRelease ? aGlueSlowAuto : aGlueSlowKnob;
                gFast += (1.0f - (target > gFast ? aAtt : aFastRel)) * (target - gFast);
                gSlow += (1.0f - (target > gSlow ? aGlueSlowA : aSlowRel)) * (target - gSlow);
                const float ex = std::max (0.0f, gFast - gSlow);
                y1 = yFast = ySlow = gSlow + kGlueCapDb * std::tanh (ex / kGlueCapDb);
                yL = y1;
                if (yL < 1.0e-5f && target <= 0.0f) { yL = y1 = yFast = ySlow = gFast = gSlow = 0.0f; }   // no denormal tails
                const float g = std::clamp (yL / 4.0f, 0.0f, 2.5f);
                c2 = kGlueC2 * g; c3 = kGlueC3 * g;
            }
            else
            {
                // CORE: the plain measured engine (M1-M18).
                const float level = pk;
                const float levelDb = 20.0f * std::log10 (std::max (level, 1.0e-7f));
                const float target = std::min (staticGr (levelDb - sThr, sRatio, knee), kMaxGrDb);

                // Release, then attack, both on the gain reduction (smooth decoupled detector).
                if (p.autoRelease)
                {
                    yFast = std::max (target, aAutoFast * yFast + (1.0f - aAutoFast) * target);
                    const float a = target > ySlow ? aAutoCharge : aAutoSlow;
                    ySlow = a * ySlow + (1.0f - a) * target;
                    y1 = std::max (yFast, ySlow);
                }
                else
                {
                    y1 = std::max (target, aRel * y1 + (1.0f - aRel) * target);
                    yFast = ySlow = y1;       // AUTO takes over from where the knob left off
                }
                yL = aAtt * yL + (1.0f - aAtt) * y1;
                if (yL < 1.0e-5f && y1 < 1.0e-5f) { yL = 0.0f; if (target <= 0.0f) y1 = yFast = ySlow = 0.0f; }   // no denormal tails
            }

            const float gain = yL > 0.0f ? std::exp2 (-yL * kDbToLog2) : 1.0f;
            const float make = sMake != 0.0f ? std::exp2 (sMake * kDbToLog2) : 1.0f;
            const float k = sDrive > 1.0e-4f ? sDrive * kDriveMax : 0.0f;
            const float wetMix = sMix * sIn;
            const bool coloured = c2 != 0.0f || c3 != 0.0f;

            for (int c = 0; c < numChannels; ++c)
            {
                const float comp = x[c] * gain * make;
                envC[c] = std::max (std::abs (comp), aEnvC * envC[c]);
                if (envC[c] < 1.0e-20f) envC[c] = 0.0f;
                const float driven = drive[c].process (comp, k, c2, c3, std::max (envC[c], kColourFloor));
                float& wd = wetLine[c][linePos];
                const float delayed = wd;                 // comp, kLatency samples ago
                wd = comp;
                float& dd = dryLine[c][linePos];
                const float dry = dd;
                dd = x[c];

                const float wet = (k > 0.0f || coloured) ? driven : delayed;
                const float out = wetMix >= 1.0f ? wet : (wetMix <= 0.0f ? dry : dry + wetMix * (wet - dry));
                io[c][i] = out;
                peakOut = std::max (peakOut, std::abs (out));
            }
            if (++linePos == kLatency) linePos = 0;
            if (grOut != nullptr) grOut[i] = yL;
        }
        grDb = yL;
        inPeak = peakIn;
        outPeak = peakOut;
    }

    // Meters, as of the last block.
    float gainReductionDb() const noexcept { return grDb; }
    float inputPeak() const noexcept       { return inPeak; }
    float outputPeak() const noexcept      { return outPeak; }

private:
    static constexpr float kDbToLog2 = 0.16609640474f;   // log2(10) / 20
    static constexpr float kDriveMax = 1.5f;

    struct Biquad
    {
        float z1 = 0.0f, z2 = 0.0f;
        inline float tick (float x, float b0, float b1, float b2, float a1, float a2) noexcept
        {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            if (! (std::abs (z1) < 1.0e6f) || std::abs (z1) < 1.0e-20f) z1 = 0.0f;   // NaN, runaway, denormal
            if (! (std::abs (z2) < 1.0e6f) || std::abs (z2) < 1.0e-20f) z2 = 0.0f;
            return y;
        }
    };

    static inline float sane (float v) noexcept { return std::isfinite (v) ? std::clamp (v, -100.0f, 100.0f) : 0.0f; }

    /** Coefficients that follow the smoothed controls; called every 16 samples. */
    void refresh() noexcept
    {
        knee = kneeFor (sRatio);
        aAtt = (float) std::exp (-1.0 / (0.001 * sAtt * fs));
        aRel = (float) std::exp (-1.0 / (0.001 * sRel * fs));
        aGlueSlowKnob = (float) std::exp (-1.0 / (0.001 * std::max ((double) kGlueSlowReleaseS * 1000.0, (double) kGlueSlowReleaseK * sRel) * fs));
        aAttPump = (float) std::exp (-1.0 / (0.001 * std::min (sAtt, kPumpAttackMs) * fs));
        aLaSlow  = (float) std::exp (-1.0 / ((kLaSlowMinS + kLaSlowSpanS * laMem) * fs));
        pumpStep = (float) (1.0 / (std::clamp ((double) rNote * 240.0 / rBpm, 0.02, 8.0) * fs));
        hpOn = sHpf >= 19.0f;
        if (hpOn)
        {
            const double w = 2.0 * M_PI * std::min ((double) sHpf, 0.45 * fs) / fs;
            const double c = std::cos (w), alpha = std::sin (w) / (2.0 * 0.70710678);
            const double a0 = 1.0 + alpha;
            hpB0 = (float) ((1.0 + c) * 0.5 / a0); hpB1 = (float) (-(1.0 + c) / a0); hpB2 = hpB0;
            hpA1 = (float) (-2.0 * c / a0); hpA2 = (float) ((1.0 - alpha) / a0);
        }
    }

    double fs = 48000.0;
    DriveStage drive[2];
    float dryLine[2][kLatency] {}, wetLine[2][kLatency] {};
    int linePos = 0;
    Biquad hp[2];
    float hpB0 = 1.0f, hpB1 = 0.0f, hpB2 = 0.0f, hpA1 = 0.0f, hpA2 = 0.0f;
    bool hpOn = false, first = true;
    uint64_t count = 0;

    float smooth = 0.0f, fade = 0.0f, aAutoFast = 0.0f, aAutoCharge = 0.0f, aAutoSlow = 0.0f;
    float aAtt = 0.0f, aRel = 0.0f, knee = 6.0f;
    float aGlueSlowKnob = 0.0f, aAttPump = 0.0f, aLaSlow = 0.0f, pumpStep = 0.0f, rBpm = 120.0f, rNote = 0.125f;
    float cMsGlue = 0.0f, cMsLa = 0.0f, aLaFast = 0.0f, cLaCharge = 0.0f, cLaMem = 0.0f, cLaMemDown = 0.0f, aPumpDec = 0.0f;
    float sPr = 0.5f, sDepth = 12.0f, aEnvC = 0.0f, envC[2] {};
    float msGlueSlow = 0.0f, gFast = 0.0f, gSlow = 0.0f, cMsGlueSlow = 0.0f, aGlueSlowA = 0.0f, aGlueFastAuto = 0.0f, aGlueSlowAuto = 0.0f;
    float msGlue = 0.0f, msLa = 0.0f, laFast = 0.0f, laSlow = 0.0f, laMem = 0.0f, pkEnv = 0.0f, pumpY = 0.0f, pumpY0 = 0.0f, pumpP = 0.0f;
    int pumpHoldN = 0, pumpHold = 0, pumpShape = 0, voicingPrev = -1;
    float sThr = -18.0f, sRatio = 4.0f, sMake = 0.0f, sMix = 1.0f, sDrive = 0.0f, sIn = 1.0f;
    float sAtt = 10.0f, sRel = 300.0f, sHpf = 0.0f;
    float y1 = 0.0f, yFast = 0.0f, ySlow = 0.0f, yL = 0.0f;
    float grDb = 0.0f, inPeak = 0.0f, outPeak = 0.0f;
};
} // namespace tutkal
