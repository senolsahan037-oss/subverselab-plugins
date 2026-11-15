#pragma once

#include "BoebAnalyser.h"
#include "BoebDsp.h"
#include "BoebReference.h"
#include <algorithm>
#include <cmath>
#include <vector>

/*  BOE-B engine: a rap vocal channel against the dull vocal. Plain C++, no JUCE.

    Chain (SPEC.md "Engine chain"), stereo with linked detectors:
        high-pass -> CLEAN (dynamic mud cut, feed-forward) -> BODY shelf -> COMP (+ average make-up) -> PRESENCE
        (bell + a high-passed saturated parallel layer) -> AIR shelf -> de-esser -> MOOD colour -> RIDE (slow level
        rider on the finished shape) -> EQUAL -> OUTPUT
        + SPACE slap and THROW echo in parallel -> -1 dBFS safety ceiling.

    LIFT is the one control: f = LIFT / 6 up to LIFT 6 (the amount LISTEN found), then up to 1.5 at LIFT 10. Every
    stage scales with f, so LIFT 0 is no processing; settled at LIFT 0 with a non-effect mood (or with the engine
    disabled) the output is the input bit for bit. Zero latency.

    Detectors and gain computers update every kCtl samples (coefficients of the dynamic filters too); audio runs per
    sample. No allocation in process(): delay lines are sized in prepare().
*/
namespace svl::boeb {

class BoebEngine {
public:
    enum Mood { Clean = 0, Push, Nineties, Radio, Phone, Megaphone, kNumMoods };

    static constexpr int   kCtl          = 16;     // control-rate step, samples
    static constexpr float kRideRangeDb  = 14.0f;
    static constexpr float kRideSec      = 0.4f;   // rider gain smoothing
    static constexpr float kRideLevelSec = 1.0f;   // rider level detector: a phrase, not a word (word-level evening is
                                                   // the compressor's; a fast rider without look-ahead lags and adds spread)
    static constexpr float kOwnLevelSec  = 3.0f;   // the vocal's own long-term level (target without a beat)
    static constexpr float kBeatLevelSec = 1.0f;
    static constexpr float kRideGateDb   = -50.0f; // rider holds its gain under this (A-weighted short-term, dBFS) ...
    static constexpr float kRideBelowDb  = 15.0f;  // ... and when the input is this far under its own long-term level
    static constexpr float kCompRatio    = 3.0f;
    static constexpr float kCompKneeDb   = 6.0f;
    static constexpr float kCompAttSec   = 0.005f, kCompRelSec = 0.08f;
#ifndef BOEB_HOLD
#define BOEB_HOLD 0.02f
#endif
#ifndef BOEB_HOLDTHR
#define BOEB_HOLDTHR 5.0f
#endif
    // Detector peak hold for longer than one period of a low voice: without it the gain moved inside every cycle
    // (owner 24 Oct: "amfi göbeği gibi titriyor"). The held detector reads higher, so the threshold goes up with it:
    // +5 dB keeps the mean gain reduction on the owner's takes where it was (tools/lowend.py, STATUS 24 Oct day 4).
    static constexpr float kCompHoldSec   = BOEB_HOLD;
    static constexpr float kCompHoldThrDb = BOEB_HOLDTHR;
    static constexpr float kCompThrTopDb = 14.0f;  // threshold re the vocal's long-term level at f = 0 ...
    static constexpr float kCompThrPerF  = 8.0f;   // ... and how far it comes down per unit of f x compNeed
    static constexpr float kMudHz = 270.0f, kMudQ = 1.4f, kMudMaxDb = 6.0f;
    static constexpr float kPresHz = 1778.0f, kPresQ = 0.7f, kPresMaxDb = 8.0f;
    static constexpr float kAirHz = 10000.0f, kAirMaxDb = 12.0f;
    // LIFT over 6 goes past what LISTEN found: on a vocal LISTEN found little wrong with, 6 -> 10 only scaled that little
    // and was not heard (owner 24 Oct). From 6 to 10 this much presence and air is added on top, whatever LISTEN found.
    static constexpr float kLiftExtraPresDb = 6.0f, kLiftExtraAirDb = 6.0f;
    static constexpr float kLiftExtraCompDb = 8.0f, kLiftExtraLayerDb = 6.0f;   // and this much lower comp threshold, louder saturated layer
#ifndef BOEB_KEEPMAX
#define BOEB_KEEPMAX 12.0f
#endif
    static constexpr float kKeepMaxDb = BOEB_KEEPMAX;   // level keep: at most this much back (the -1 dBFS ceiling)
    static constexpr float kBodyHz = 180.0f;
    static constexpr float kLayerHz = 1000.0f, kLayerDb = -24.0f, kLayerDrive = 4.0f;
    static constexpr float kSibHz = 6000.0f, kSibQ = 0.8f, kSibMaxCutDb = 10.0f;
    static constexpr float kSibGateDb = -12.0f;    // de-esser listens only when SIB is within this of broadband
    static constexpr float kSibAllowDb = 0.0f;
    // SOFTEN: harshness (owner 24 Oct, stronger version: "biraz hars duyuyorum, biraz yatıştırabiliriz"). At LIFT 10 the
    // 2.5-5 kHz band re level rose +9 / +12 dB at its harsh moments (95th percentile; +4 / +5 without the compressor: the
    // harder compression holds the vowels down and lets consonants through). Like the de-esser: a dynamic cut at 3.5 kHz,
    // only while the processed signal's 2.5-5 kHz re level is more than kHarshAllowDb over the input's; plus a fixed dip
    // there that grows with LIFT over 6 (kHarshLiftDipDb at 10), as the extra presence spills into 2.5-5 kHz.
    // tools/harsh.py, LIFT 10, 95th pct: Live loop +8.8 -> +5.6, take 0004 +11.8 -> +8.1 dB; 1-4 kHz band still +2.5 / +2.9.
    static constexpr float kHarshHz = 3500.0f, kHarshQ = 1.0f, kHarshMaxCutDb = 8.0f;
    static constexpr float kHarshAllowDb = 2.0f, kHarshGateDb = -20.0f, kHarshLiftDipDb = 3.0f;     // sibilance may rise this much over the input's, relative to level
    static constexpr float kEqualSec = 4.0f, kEqualRangeDb = 18.0f;
    static constexpr float kCeilingDb = -1.0f, kCeilRelSec = 0.05f;
    static constexpr float kBypassSec = 0.01f;
    static constexpr float kMaxDelaySec = 2.5f;
    static constexpr double kLoopSec = 1.5, kLoopMaxDb = 6.0;

    // EXPANDER ("dipler"): gentle downward expansion between words, so the noise the compressor, rider, presence and the
    // exciter lift there goes back down (owner 24 Oct: "dipte hafif hışırtı var"; gaps measured 4-9 dB louder than the take,
    // 5-24 dB more above 7.4 kHz, the exciter up to 12 dB of that). Keyed on the input. The threshold follows the take's own
    // noise floor, not the voice: on the owner's takes the floor is only 22-26 dB under the voice and word ends reach down
    // to it, so a threshold re the voice would drop words (the problem BOE-B is for).
    static constexpr float kExpAboveNoiseDb = 12.0f;  // threshold: this far over the measured noise floor (12: the level keep, 24 Oct) ...
    static constexpr float kExpWordsDb = 12.0f;       // ... and never closer than this under the vocal's own level
    static constexpr float kExpRatio   = 3.0f;        // 1:3 under it
    static constexpr float kExpMaxDb   = 15.0f;
    static constexpr float kExpDetRelSec = 0.03f;     // detector: instant up, falls this fast
    static constexpr float kExpOpenSec = 0.001f, kExpCloseSec = 0.05f;
    static constexpr float kNoiseRiseDbPerSec = 1.0f; // noise floor: the quietest 10 ms block, rising this fast
    static constexpr double kExcRmsSec = 0.002;   // exciter's level normalisation (see the exciter)
    static constexpr float kMudEaseBelowDb = 15.0f, kMudEaseSpanDb = 9.0f;   // mud cut: none 15 dB under, full 6 dB under

    struct Parameters {
        float lift = 0;            // 0 .. 10
        int mood = Clean;
        float bodyDb = 0;          // BODY knob, -9 .. +9
        float airDb = 0;           // AIR knob, 0 .. 12 (LISTEN sets it; the value at LIFT 6); with the exciter it moves the exciter
        float space = 0;           // SPACE knob, 0 .. 1
        bool throwOn = false;      // THROW button (held / automated)
        float outputDb = 0;        // -12 .. +12
        bool equal = false;        // EQUAL: processed output matched to the input's loudness
        bool enabled = true;
        float dryWet = 1;          // DRY/WET 0 .. 1: the whole processed channel (SPACE / THROW included) against the input
        double bpm = 0;            // host tempo, 0 = unknown
        Settings listen;           // from LISTEN (hpf, presence, comp need; airDb is copied into the AIR knob)
        int testOff = 0;           // measurement only: bit mask of stages held off (1 ride, 2 comp, 4 mud, 8 de-ess, 16 layer, 32 tone loop,
                                   // 64 exciter, 128 expander, 256 mud weight held at 1, 512 soften)
    };

    void prepare (double sampleRate, int /*maxBlockSize*/)
    {
        fs = sampleRate > 0 ? sampleRate : 48000.0;
        const int maxDelay = (int) std::ceil (kMaxDelaySec * fs) + 4;
        for (auto& d : dly) d.assign ((size_t) maxDelay, 0.0f);
        cRideLevel = coefFor (kRideLevelSec, fs / kCtl);
        wetStep = (float) (1.0 - coefFor (0.02, fs));
        cInFast    = coefFor (0.05, fs / kCtl);
        frameLenTicks = std::max (1, (int) std::lround (kFrameSec * fs / kCtl));
        cFrameRide = coefFor (kRideLevelSec, 1.0 / kFrameSec);
        cFrameOwn  = coefFor (kOwnLevelSec, 1.0 / kFrameSec);
        cFrameBeat = coefFor (kBeatLevelSec, 1.0 / kFrameSec);
        cFrameMud  = coefFor (3.0, 1.0 / kFrameSec);
        cFrameFast = coefFor (0.3, 1.0 / kFrameSec);
        cOwn       = coefFor (kOwnLevelSec, fs / kCtl);
        cBeat      = coefFor (kBeatLevelSec, fs / kCtl);
        cRide      = coefFor (kRideSec, fs / kCtl);
        cCompAtt   = coefFor (kCompAttSec, fs);
        cCompRel   = coefFor (kCompRelSec, fs);
        compHoldSamples = (int) std::lround (kCompHoldSec * fs);
        cFast      = coefFor (0.005, fs);
        cExcEnv    = coefFor (kExcRmsSec, fs);
        expFallDb  = (float) (8.6859 * (kCtl / fs) / kExpDetRelSec);
        cExpOpen   = coefFor (kExpOpenSec, fs / kCtl);
        cExpClose  = coefFor (kExpCloseSec, fs / kCtl);
        noiseBlockTicks = std::max (1, (int) std::lround (0.01 * fs / kCtl));
        cExpRms    = coefFor (0.005, fs / kCtl);

        cSibRel    = coefFor (0.04, fs);
        cEqual     = coefFor (kEqualSec, fs / kCtl);
        cEqualGain = coefFor (0.3, fs / kCtl);
        cCeilRel   = coefFor (kCeilRelSec, fs);
        cParam     = coefFor (0.03, fs / kCtl);
        bypassStep = 1.0f / std::max (1.0f, (float) (kBypassSec * fs));
        awIn.prepare (fs); awKey.prepare (fs); awPre.prepare (fs);
        for (int c = 0; c < 2; ++c) { kIn[c].prepare (fs); kOut[c].prepare (fs); }
        mudDet.prepare (fs, kMudLo, kMudHi); midDet.prepare (fs, kMidLo, kMidHi);
        presDet.prepare (fs, kPresLo, kPresHi); airDet.prepare (fs, kAirLo, std::min (kAirHi, 0.45 * fs));
        outDet[0].prepare (fs, kMudLo, kMudHi); outDet[1].prepare (fs, kMidLo, kMidHi);
        outDet[2].prepare (fs, kPresLo, kPresHi); outDet[3].prepare (fs, kAirLo, std::min (kAirHi, 0.45 * fs));
        sibIn.prepare (fs, kSibLo, kSibHi);  sibPre.prepare (fs, kSibLo, kSibHi);
        harshIn.prepare (fs, 2500.0, 5000.0); harshPre.prepare (fs, 2500.0, 5000.0);
        slapHp.highPass (fs, 200, 0.707); slapLp.lowPass (fs, 6000, 0.707);
        throwHp.highPass (fs, 300, 0.707); throwLp.lowPass (fs, 4000, 0.707);
        // How much of a filter's centre gain the measured band actually gets (average dB over the band, log
        // frequency): the moving filters are driven by band dB, so their centre gain is band dB / k.
        // Bands are read re MID, and every filter touches MID a little, so the three gains are solved together:
        // E[band][filter] = change of (band re MID) per dB of the filter's centre gain; centre gains = E^-1 x wanted.
        {
            Biquad b[3];
            b[0].peak (fs, kMudHz, kMudQ, -6.0); b[1].peak (fs, kPresHz, kPresQ, 6.0); b[2].highShelf (fs, kAirHz, 0.707, 6.0);
            const double sign[3] = { -6.0, 6.0, 6.0 };
            const double lo[3] = { kMudLo, kPresLo, kAirLo }, hi[3] = { kMudHi, kPresHi, std::min (kAirHi, 0.45 * fs) };
            double E[3][3];
            for (int j = 0; j < 3; ++j)
            {
                const double mid = bandAvgDb (b[j], kMidLo, kMidHi);
                for (int k = 0; k < 3; ++k) E[k][j] = (bandAvgDb (b[j], lo[k], hi[k]) - mid) / sign[j];
            }
            const double det = E[0][0] * (E[1][1] * E[2][2] - E[1][2] * E[2][1]) - E[0][1] * (E[1][0] * E[2][2] - E[1][2] * E[2][0])
                             + E[0][2] * (E[1][0] * E[2][1] - E[1][1] * E[2][0]);
            (void) det;
            for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) eqE[r][c] = E[r][c];
        }
        designed = false;
        reset();
    }

    void reset()
    {
        for (int c = 0; c < 2; ++c)
        {
            hp[c].reset(); mudEq[c].reset(); body[c].reset(); pres[c].reset(); air[c].reset(); sibEq[c].reset(); harshEq[c].reset();
            layerHp[c].reset(); moodA[c].reset(); moodB[c].reset(); moodC[c].reset(); moodD[c].reset(); moodE[c].reset();
            kIn[c].reset(); kOut[c].reset();
        }
        presDet.reset(); airDet.reset(); for (auto& d : outDet) d.reset();
        for (int io = 0; io < 2; ++io) for (int k = 0; k < 4; ++k) { accB[io][k] = frameB[io][k] = bandP[io][k] = 0; }
        loopCorr[0] = loopCorr[1] = loopCorr[2] = 0; loopFrame = false;
        for (auto& b : bandFast) b = 0;
        frameOutMax = -300; haveBands = false;
        excEnv[0] = excEnv[1] = 0; exciteGain = 0;
        for (int c = 0; c < 2; ++c) { excSrc[c].reset(); excOut[c].reset(); }
        awIn.reset(); awKey.reset(); awPre.reset(); mudDet.reset(); midDet.reset(); sibIn.reset(); sibPre.reset(); harshIn.reset(); harshPre.reset();
        slapHp.reset(); slapLp.reset(); throwHp.reset(); throwLp.reset();
        for (auto& d : dly) std::fill (d.begin(), d.end(), 0.0f);
        wpos = 0;
        ctlCount = 0; accA = accKey = accPre = accGr = 0; accInK = accOutK = 0; accN = 0;
        framePre = frameKey = frameIn = 0; frameTicks = 0; frameMaxDb = -300; frameActive = false;
        accMud = accMid = frameMud = frameMid = mudP = midP = 0;
        rideLevel = inLevel = ownIn = ownPre = -300; beatLevel = -300; rideGainDb = 0; makeupDb = 0; haveOwn = false; keepDb = 0; keepGain = 1;
        compEnv = 0; compHeld = 0; compHoldLeft = 0; sibInEnv = sibPreEnv = broadInEnv = broadPreEnv = 0; harshInEnv = harshPreEnv = 0; harshCut = 0;
        mudCut = sibCut = 0; mudAvg = 0; inFast = -300; mudWeight = 1; inLoud = outLoud = -300; equalDb = 0; ceilGain = 1; throwSend = 0;
        expEnvDb = -300; expDb = 0; expGain = 1; noiseDb = 300; noiseAcc = 0; noiseTicks = 0; expPow = 0; excWeight = 1;
        primed = false;
        rep = Report {};
    }

    int getLatencySamples() const { return 0; }

    struct Report {
        float rideDb = 0, compGrDb = 0, mudCutDb = 0, deessDb = 0, equalDb = 0, ceilingDb = 0, outPeak = 0;
        float makeupDb = 0, rideLevelDb = 0, targetDb = 0;   // read-outs for the trace
        float exciteDb = 0, loopAirDb = 0;
        float expanderDb = 0;   // largest expander reduction in the block
        float toneDb[3] = { 0, 0, 0 };   // the solved mud / presence / air filter gains (trace)
    };
    const Report& report() const { return rep; }

    static float liftToF (float lift)
    {
        lift = std::clamp (lift, 0.0f, 10.0f);
        return lift <= 6.0f ? lift / 6.0f : 1.0f + (lift - 6.0f) / 8.0f;
    }

    /** key: the beat on the side input (nullptr or keyCh 0 = none). */
    void process (const float* const* in, float* const* out, int numCh, const float* const* key, int keyCh, int n,
                  const Parameters& p)
    {
        if (numCh <= 0 || n <= 0) return;
        const int nIn = std::min (numCh, 2);
        const float* inL = in[0];
        const float* inR = nIn > 1 ? in[1] : in[0];
        const bool effectMood = p.mood >= Radio;
        const float fTarget = p.enabled ? liftToF (p.lift) : 0.0f;
        const float mixTarget = p.enabled && (fTarget > 0.0f || effectMood) ? 1.0f : 0.0f;

        const float wetTarget = std::clamp (p.dryWet, 0.0f, 1.0f);
        if (! primed) { wetNow = wetTarget; f = fTarget; mix = mixTarget; airNow = p.airDb; bodyNow = p.bodyDb; primed = true; }
        if (! designed || hpHz != p.listen.hpfHz || moodNow != p.mood || cutNow != (p.listen.bandLimited ? p.listen.cutHz : 0.0f))
        {
            designStatic (p);
            excDb = p.listen.exciteDb;
        }

        Report r {};
        for (int i = 0; i < n; ++i)
        {
            float xL = inL[i], xR = inR[i];
            if (! std::isfinite (xL) || std::abs (xL) > 1.0e6f) xL = 0;
            if (! std::isfinite (xR) || std::abs (xR) > 1.0e6f) xR = 0;

            // Detectors that run on the input whatever the state.
            const double xm = 0.5 * ((double) xL + xR);
            double km = 0;
            if (key != nullptr && keyCh > 0)
            {
                for (int c = 0; c < std::min (keyCh, 2); ++c) { const float k = key[c][i]; km += std::isfinite (k) ? k : 0.0f; }
                km /= std::min (keyCh, 2);
            }
            { const double a = awIn.process (xm), ak = awKey.process (km); accA += a * a; accKey += ak * ak; }
            { const double kl = kIn[0].process (xL), kr = kIn[1].process (xR); accInK += kl * kl + kr * kr; }
            ++accN;

            if (++ctlCount >= kCtl) { control (p); ctlCount = 0; }

            // Bypass crossfade.
            if (mix < mixTarget) mix = std::min (mixTarget, mix + bypassStep);
            else if (mix > mixTarget) mix = std::max (mixTarget, mix - bypassStep);
            if (mix <= 0.0f && mixTarget <= 0.0f)
            {
                // Settled bypass: the input, bit for bit. Effects' tails are dropped.
                out[0][i] = inL[i];
                if (numCh > 1) out[1][i] = nIn > 1 ? inR[i] : inL[i];
                continue;
            }

            double y[2] = { xL, xR };
            const double sibInSample = sibIn.process (xm);
            sibInEnv  = follow (sibInEnv, sibInSample * sibInSample);
            const double harshInSample = harshIn.process (xm);
            harshInEnv = follow (harshInEnv, harshInSample * harshInSample);
            broadInEnv = follow (broadInEnv, xm * xm);

            for (int c = 0; c < 2; ++c) y[c] = hp[c].process (y[c]);
            // Mud / mid detectors before the cut (feed-forward: the cut follows the excess, not its own result).
            {
                const double ym = 0.5 * (y[0] + y[1]);
                const double mu = mudDet.process (ym), mi = midDet.process (ym);
                const double pi = presDet.process (ym), ai = airDet.process (ym);
                accMud += mu * mu; accMid += mi * mi; accB[0][2] += pi * pi; accB[0][3] += ai * ai;
            }
            for (int c = 0; c < 2; ++c) y[c] = body[c].process (mudEq[c].process (y[c]));
            // Compressor: linked, peak-ish detector; the average gain reduction comes back as make-up.
            {
                // Peak hold for longer than one period of a low voice (20 ms = 50 Hz) before the release: without it the
                // envelope sagged between the waveform's peaks, the gain moved inside every cycle and the low end came out
                // distorted (owner 24 Oct: "amfi göbeği gibi titriyor"; 110 Hz tone, LIFT 10: harmonics -40.8 dB against -44.8 in).
                const double a = std::max (std::abs (y[0]), std::abs (y[1]));
                if (a >= compHeld) { compHeld = a; compHoldLeft = compHoldSamples; }
                else if (compHoldLeft > 0) --compHoldLeft;
                else compHeld = a;
                const double d = std::max (a, compHeld);
                compEnv = d > compEnv ? cCompAtt * compEnv + (1 - cCompAtt) * d : cCompRel * compEnv + (1 - cCompRel) * d;
                const float envDb = compEnv > 1e-12 ? (float) (20 * std::log10 (compEnv)) : -240.0f;
                const float over = envDb - compThrDb;
                float gr = 0;
                if (2 * over > kCompKneeDb) gr = over * (1 - 1 / kCompRatio);
                else if (2 * over > -kCompKneeDb) { const float t = over + kCompKneeDb / 2; gr = (1 - 1 / kCompRatio) * t * t / (2 * kCompKneeDb); }
                gr *= compAmount;
                r.compGrDb = std::max (r.compGrDb, gr);
                accGr += gr;
                const double g = std::pow (10.0, (makeupDb - gr) / 20.0);
                y[0] *= g; y[1] *= g;
            }
            for (int c = 0; c < 2; ++c)
            {
                double v = pres[c].process (y[c]);
                const double h = layerHp[c].process (y[c]);
                v += layerGain * std::tanh (kLayerDrive * h) / kLayerDrive;
                v = air[c].process (v);
                // EXCITER (band-limited sources only): the octave under the cut (before the tone EQ), squared and divided by its own RMS
                // (level-linear, only the 2nd harmonic: nothing folds back as long as twice the cut is under
                // Nyquist), then high-passed at the cut so the original band is not touched.
                if (exciteOn)
                {
                    // Source taken before the tone EQ, as LISTEN's calibration hears it (the presence lift reaches
                    // into the octave under the cut and made the exciter 4-10 dB hotter than calibrated).
                    const double src = excSrc[c].process (y[c]);
                    excEnv[c] = cExcEnv * excEnv[c] + (1 - cExcEnv) * src * src;
                    // RMS over 2 ms: short enough that a sudden onset (a plosive) peaks at about 2x the source instead
                    // of 4x with 10 ms, smooth enough that the octave stays pure (made band -60 dB inharmonic on a
                    // clean harmonic voice). A hard floor under the divisor (tried) rectified noise-like input 4.6 %
                    // of the time and folded back: -24 dB inharmonic, heard as fizz.
                    const double gen = src * src / std::sqrt (excEnv[c] + 1.0e-20);
                    v += exciteGain * excOut[c].process (gen);
                }
                y[c] = v;
            }
            // De-esser: relative sibilance of the processed signal against the input's.
            {
                const double ym = 0.5 * (y[0] + y[1]);
                const double s = sibPre.process (ym);
                sibPreEnv = follow (sibPreEnv, s * s);
                broadPreEnv = follow (broadPreEnv, ym * ym);
                const double hs = harshPre.process (ym);
                harshPreEnv = follow (harshPreEnv, hs * hs);
                for (int c = 0; c < 2; ++c) y[c] = harshEq[c].process (sibEq[c].process (y[c]));
                // What the tone stages achieved, measured on their output (closes the loop below).
                const double yo = 0.5 * (y[0] + y[1]);
                double b;
                b = outDet[0].process (yo); accB[1][0] += b * b;
                b = outDet[1].process (yo); accB[1][1] += b * b;
                b = outDet[2].process (yo); accB[1][2] += b * b;
                b = outDet[3].process (yo); accB[1][3] += b * b;
            }
            // Mood colour.
            for (int c = 0; c < 2; ++c) y[c] = moodColour (c, y[c]);
            // RIDE: it measures what it is about to move (the finished shape), then moves it.
            { const double a = awPre.process (0.5 * (y[0] + y[1])); accPre += a * a; }
            // EQUAL measures before the rider: the rider's own gain is taken back exactly, the rest (tone, compressor)
            // is matched over a long window.
            { const double kl = kOut[0].process (y[0]), kr = kOut[1].process (y[1]); accOutK += kl * kl + kr * kr; }
            y[0] *= rideGain * keepGain * expGain; y[1] *= rideGain * keepGain * expGain;
            r.expanderDb = std::max (r.expanderDb, expDb);
            {
                const double g = outGain * (p.equal ? std::pow (10.0, equalDb / 20.0) : 1.0);
                y[0] *= g; y[1] *= g;
            }
            // SPACE slap + THROW echo, from the processed mono.
            {
                const double ym = 0.5 * (y[0] + y[1]);
                const float throwTarget = p.throwOn ? 1.0f : 0.0f;
                throwSend += (throwTarget - throwSend) * 0.002f;
                const size_t len = dly[0].size();
                const auto tap = [&] (const std::vector<float>& d, double samples) {
                    const double rp = (double) wpos - samples;
                    const double rr = rp < 0 ? rp + (double) len : rp;
                    const size_t i0 = (size_t) rr % len, i1 = (i0 + 1) % len;
                    const double fr = rr - std::floor (rr);
                    return (1 - fr) * d[i0] + fr * d[i1];
                };
                const double slapL = tap (dly[0], slapSamples), slapR = tap (dly[0], slapSamples + 0.007 * fs);
                const double thrL = tap (dly[1], throwSamples), thrR = tap (dly[2], throwSamples);
                const double slapIn = slapLp.process (slapHp.process (ym + 0.12 * slapL));
                const double thrIn = throwLp.process (throwHp.process (ym * throwSend));
                dly[0][wpos] = (float) slapIn;
                dly[1][wpos] = (float) (thrIn + 0.45 * thrR);   // ping-pong: L feeds from R, R from L
                dly[2][wpos] = (float) (0.45 * thrL);
                wpos = (wpos + 1) % len;
                const double sp = std::clamp (p.space, 0.0f, 1.0f) * 0.35 * std::min (1.0f, f + (effectMood ? 1.0f : 0.0f));
                y[0] += sp * slapL + 0.5 * thrL;
                y[1] += sp * slapR + 0.5 * thrR;
            }
            // Bypass crossfade against the dry input, then the ceiling.
            double o[2];
            wetNow += (wetTarget - wetNow) * wetStep;   // DRY/WET glides (~20 ms), no zipper
            const float blend = mix * wetNow;
            for (int c = 0; c < 2; ++c) o[c] = blend >= 1.0f ? y[c] : (c ? xR : xL) + blend * (y[c] - (c ? xR : xL));
            const double pk = std::max (std::abs (o[0]), std::abs (o[1]));
            const double lim = std::pow (10.0, kCeilingDb / 20.0);
            // Instant attack on the sample peak, release towards 1 but never above what this sample allows.
            const double allowed = pk > lim ? lim / pk : 1.0;
            ceilGain = std::min (allowed, cCeilRel * ceilGain + (1 - cCeilRel));
            o[0] *= ceilGain; o[1] *= ceilGain;
            r.ceilingDb = std::max (r.ceilingDb, (float) (-20 * std::log10 (ceilGain)));
            out[0][i] = (float) o[0];
            if (numCh > 1) out[1][i] = (float) (nIn > 1 ? o[1] : o[0]);
            r.outPeak = std::max (r.outPeak, (float) pk * (float) ceilGain);
        }
        for (int ch = 2; ch < numCh; ++ch)
            if (in[ch] != out[ch]) std::copy (in[ch], in[ch] + n, out[ch]);

        r.rideDb = rideGainDb; r.mudCutDb = mudCut; r.deessDb = sibCut; r.equalDb = equalDb;
        r.exciteDb = (float) excDb; r.loopAirDb = (float) (powToDb (bandFast[3]) - powToDb (bandFast[1]));
        for (int k = 0; k < 3; ++k) r.toneDb[k] = toneNow[k];
        r.makeupDb = makeupDb; r.rideLevelDb = rideLevel; r.targetDb = beatLevel > -90.0f ? beatLevel + kRapLarDb : ownPre;
        rep = r;
    }

private:
    double bandAvgDb (const Biquad& b, double lo, double hi) const
    {
        double sum = 0; const int n = 48;
        for (int k = 0; k < n; ++k)
        {
            const double fr = lo * std::pow (hi / lo, (k + 0.5) / n), w = 2 * kPi * fr / fs;
            const double c1 = std::cos (w), s1 = -std::sin (w), c2 = std::cos (2 * w), s2 = -std::sin (2 * w);
            const double nr = b.b0 + b.b1 * c1 + b.b2 * c2, ni = b.b1 * s1 + b.b2 * s2;
            const double dr = 1 + b.a1 * c1 + b.a2 * c2, di = b.a1 * s1 + b.a2 * s2;
            sum += 10 * std::log10 ((nr * nr + ni * ni) / (dr * dr + di * di));
        }
        return sum / n;
    }

    double follow (double env, double sq) const
    {
        return sq > env ? cFast * env + (1 - cFast) * sq : cSibRel * env + (1 - cSibRel) * sq;
    }

    void designStatic (const Parameters& p)
    {
        hpHz = p.listen.hpfHz; moodNow = p.mood; cutNow = p.listen.bandLimited ? p.listen.cutHz : 0.0f;
        exciteOn = cutNow > 0 && 2.0 * cutNow < 0.49 * fs && p.mood < Radio && p.mood != Nineties;
        if (exciteOn)
            for (int c = 0; c < 2; ++c)
            {
                excSrc[c].prepare (fs, cutNow * 0.5, cutNow * 0.97);
                excOut[c].prepare (fs, cutNow, std::min (1.94 * cutNow, 0.45 * fs));   // 2 x the source band's top
                excEnv[c] = 0;
            }
        for (int c = 0; c < 2; ++c)
        {
            hp[c].highPass (fs, std::clamp (hpHz, 40.0f, 200.0f), 0.70710678);
            layerHp[c].highPass (fs, kLayerHz, 0.70710678);
            moodA[c].identity(); moodB[c].identity(); moodC[c].identity(); moodD[c].identity(); moodE[c].identity();
            switch (p.mood)
            {
                case Nineties:  moodA[c].lowPass (fs, 13000, 0.707); break;
                case Radio:     moodA[c].highPass (fs, 300, 0.707); moodB[c].highPass (fs, 300, 0.707);
                                moodC[c].lowPass (fs, 3400, 0.707); moodD[c].lowPass (fs, 3400, 0.707); break;
                case Phone:     moodA[c].highPass (fs, 450, 0.707); moodB[c].highPass (fs, 450, 0.707);
                                moodC[c].lowPass (fs, 2800, 0.707); moodD[c].lowPass (fs, 2800, 0.707); break;
                case Megaphone: moodA[c].highPass (fs, 550, 0.707); moodB[c].highPass (fs, 550, 0.707);
                                moodC[c].lowPass (fs, 3000, 0.707); moodD[c].lowPass (fs, 3000, 0.707);
                                moodE[c].peak (fs, 1600, 2.0, 8.0); break;
                default: break;
            }
        }
        designed = true;
    }

    double moodColour (int c, double v)
    {
        switch (moodNow)
        {
            case Nineties:  v = moodA[c].process (v); return std::tanh (1.5 * v) / 1.5;
            case Radio:     v = moodD[c].process (moodC[c].process (moodB[c].process (moodA[c].process (v)))); return std::tanh (2.5 * v) / 2.5;
            case Phone:     v = moodD[c].process (moodC[c].process (moodB[c].process (moodA[c].process (v)))); return std::clamp (4.0 * v, -1.0, 1.0) / 4.0;
            case Megaphone: v = moodE[c].process (moodD[c].process (moodC[c].process (moodB[c].process (moodA[c].process (v))))); return std::clamp (6.0 * v, -1.0, 1.0) / 6.0;
            default: return v;
        }
    }

    void control (const Parameters& p)
    {
        // Smoothed control values.
        const float fTarget = p.enabled ? liftToF (p.lift) : 0.0f;
        f = (float) (fTarget + (f - fTarget) * cParam); if (std::abs (f - fTarget) < 1e-4f) f = fTarget;
        airNow  = (float) (p.airDb + (airNow - p.airDb) * cParam);
        bodyNow = (float) (p.bodyDb + (bodyNow - p.bodyDb) * cParam);
        const float fe = std::min (f, 1.5f);
        const bool voiceMood = moodNow >= Radio;

        // Levels over the last kCtl samples.
        const double inv = 1.0 / std::max (1, accN);
        const float aDb = powToDb (accA * inv), keyDb = powToDb (accKey * inv), preDb = powToDb (accPre * inv);
        const float inK = powToDb (accInK * inv * 0.5), outK = powToDb (accOutK * inv * 0.5);
        const float grMean = (float) (accGr * inv);
        accA = accKey = accInK = accOutK = accPre = accGr = 0; accN = 0;

        // RIDE: level towards the beat + the rap LAR, or towards the vocal's own long-term level.
        const auto smoothDb = [] (float cur, float x, double c) {
            const double pc = std::pow (10.0, cur / 10.0), px = std::pow (10.0, x / 10.0);
            return powToDb (c * pc + (1 - c) * px);
        };
        inLevel = smoothDb (inLevel, aDb, cRideLevel);
        // Mud cut weight: full on the voice's loud frames, easing to none 15 dB under its long-term level. The quiet
        // frames of real takes are the mud-heavy ones (bleed, hum: MUD +11 dB re MID in the quietest 10 % of one take);
        // a full cut there made them quieter still and widened the level spread by 3-4 dB.
        inFast = smoothDb (inFast, aDb, cInFast);
        {
            const float wT = haveOwn ? std::clamp ((inFast - (ownIn - kMudEaseBelowDb)) / kMudEaseSpanDb, 0.0f, 1.0f) : 1.0f;
            mudWeight = (p.testOff & 256) ? 1.0f : (float) (wT + (mudWeight - wT) * cParam);   // 256: measurement, weight held
        }
        const bool active = inLevel > kRideGateDb && (! haveOwn || inLevel > ownIn - kRideBelowDb);
        if (active) makeupDb = (float) (grMean + (makeupDb - grMean) * cOwn);   // average gain reduction while the vocal sounds
        // 100 ms frames: a frame counts when the input is within 20 dB of its recent loudest frame and above the gate;
        // only those frames move the levels (as the measurement the target comes from).
        frameMud += accMud; frameMid += accMid; accMud = accMid = 0;
        accB[0][0] = 0; accB[0][1] = 0;   // (input MUD / MID are the frameMud / frameMid above)
        for (int io = 0; io < 2; ++io) for (int k = 0; k < 4; ++k) { frameB[io][k] += accB[io][k]; accB[io][k] = 0; }
        framePre += std::pow (10.0, preDb / 10.0); frameKey += std::pow (10.0, keyDb / 10.0); frameIn += std::pow (10.0, aDb / 10.0);
        if (++frameTicks >= frameLenTicks)
        {
            const float fIn = powToDb (frameIn / frameTicks), fPre = powToDb (framePre / frameTicks), fKey = powToDb (frameKey / frameTicks);
            frameMaxDb = std::max (fIn, frameMaxDb - 0.02f);           // falls 0.2 dB a second: about the session's loudest
            frameActive = fIn > kRideGateDb && fIn > frameMaxDb - kActiveBelowMaxDb;
            if (frameActive)
            {
                rideLevel = haveOwn ? smoothDb (rideLevel, fPre, cFrameRide) : fPre;
                ownIn  = haveOwn ? smoothDb (ownIn, fIn, cFrameOwn) : fIn;
                ownPre = haveOwn ? smoothDb (ownPre, fPre, cFrameOwn) : fPre;
                beatLevel = haveOwn ? smoothDb (beatLevel, fKey, cFrameBeat) : fKey;
                mudP = haveOwn ? cFrameMud * mudP + (1 - cFrameMud) * frameMud : frameMud;
                midP = haveOwn ? cFrameMud * midP + (1 - cFrameMud) * frameMid : frameMid;
                haveOwn = true;
            }
            // The tone loop picks its frames by the OUTPUT level (what the listener hears, and what is measured): a
            // quiet bleed frame the rider lifted counts, as it does in the measurement.
            const float fOut = fPre + rideGainDb;
            frameOutMax = std::max (fOut, frameOutMax - 0.02f);
            if (haveOwn && fOut > kRideGateDb && fOut > frameOutMax - kActiveBelowMaxDb)
            {
                frameB[0][0] = frameMud; frameB[0][1] = frameMid;
                for (int io = 0; io < 2; ++io)
                    for (int k = 0; k < 4; ++k)
                        bandP[io][k] = haveBands ? cFrameMud * bandP[io][k] + (1 - cFrameMud) * frameB[io][k] : frameB[io][k];
                for (int k = 1; k < 4; k += 2)   // output MID and AIR, fast: the exciter's loop must see faster than it moves
                    bandFast[k] = haveBands ? cFrameFast * bandFast[k] + (1 - cFrameFast) * frameB[1][k] : frameB[1][k];
                haveBands = true;
                loopFrame = true;
            }
            framePre = frameKey = frameIn = frameMud = frameMid = 0; frameTicks = 0;
            for (int io = 0; io < 2; ++io) for (int k = 0; k < 4; ++k) frameB[io][k] = 0;
        }
        const bool beat = beatLevel > -90.0f;
        const float target = beat ? beatLevel + kRapLarDb : ownPre;
        // The rider moves only while the current frame is active vocal; in pauses it holds.
        float want = frameActive && haveOwn ? std::clamp (target - rideLevel, -kRideRangeDb, kRideRangeDb) * std::min (fe, 1.0f) : rideGainDb;
        if (voiceMood && f <= 0.0f) want = 0;
        if (p.testOff & 1) want = 0;
        rideGainDb = (float) (want + (rideGainDb - want) * cRide);
        rideGain = std::pow (10.0, rideGainDb / 20.0);
        // LEVEL KEEP (no beat): the tone stages take level away (mud cut, tone loop: -6 dB at LIFT 6 on a polished sample;
        // owner 24 Oct "knobları sonuna kadar çeviriyorum ama duyulur bir fark yaratmıyor"): heard as quieter, not clearer.
        // A slow, even gain gives the input's long-term level back, words and gaps alike (the rider, which moves only on
        // active frames, lifted the gaps relative to the words when it did this).
        {
            const float keepT = (! beat && haveOwn && ! (p.testOff & 1)) ? std::clamp (ownIn - ownPre, -12.0f, kKeepMaxDb) * std::min (fe, 1.0f) : 0.0f;
            keepDb = (float) (keepT + (keepDb - keepT) * cEqualGain);
            keepGain = std::pow (10.0, keepDb / 20.0);
        }

        // EXPANDER: input level, instant up, falling kExpDetRelSec; under the vocal's own level - kExpBelowDb it expands 1:2.
        // 5 ms RMS first: per control step (0.3 ms) noise has peaks 5-10 dB over its mean that held the detector open.
        expPow = cExpRms * expPow + (1 - cExpRms) * std::pow (10.0, aDb / 10.0);
        expEnvDb = std::max (powToDb (expPow), expEnvDb - expFallDb);
        noiseAcc += std::pow (10.0, aDb / 10.0);
        if (++noiseTicks >= noiseBlockTicks)
        {
            const float b = powToDb (noiseAcc / noiseTicks);
            if (b > -90.0f) noiseDb = b < noiseDb ? b : noiseDb + kNoiseRiseDbPerSec * (float) (noiseTicks * kCtl / fs);
            noiseAcc = 0; noiseTicks = 0;
        }
        {
            float t = 0.0f;
            if (haveOwn && noiseDb > -200.0f && ! (p.testOff & 128) && ! voiceMood)
            {
                const float thr = std::min (noiseDb + kExpAboveNoiseDb, ownIn - kExpWordsDb);
                t = std::clamp ((thr - expEnvDb) * (kExpRatio - 1.0f), 0.0f, kExpMaxDb) * std::min (fe, 1.0f);
            }
            expDb = (float) (t + (expDb - t) * (t < expDb ? cExpOpen : cExpClose));
            expGain = std::pow (10.0, -expDb / 20.0);
        }

        // The exciter's voice weight, from the expander's fast detector (5 ms RMS, 30 ms fall): none 15 dB under the voice's
        // long-term level, full 6 dB under. The mud weight (50 ms power smoothing) fell only ~9 dB in a 100 ms gap between
        // words, so the exciter kept squaring the gap's noise into hiss above the cut (+6..+13 dB there on the owner's takes).
        {
            const float wT = haveOwn ? std::clamp ((expEnvDb - (ownIn - kMudEaseBelowDb)) / kMudEaseSpanDb, 0.0f, 1.0f) : 1.0f;
            excWeight = (float) (wT + (excWeight - wT) * (wT > excWeight ? cExpOpen : cExpClose));
        }

        // COMP: threshold re the vocal's own long-term input level.
        const float need = std::clamp (p.listen.compNeed, 0.5f, 1.6f) * (moodNow == Push ? 1.2f : 1.0f);
        const float liftOver = std::clamp ((p.lift - 6.0f) / 4.0f, 0.0f, 1.0f);   // LIFT 6 -> 10: harder, denser (owner 24 Oct)
        compThrDb = (haveOwn ? ownIn : -20.0f) + kCompThrTopDb + kCompHoldThrDb - kCompThrPerF * fe * need - kLiftExtraCompDb * liftOver;
        compAmount = (p.testOff & 2) ? 0.0f : std::min (1.0f, fe * 2.0f);

        // CLEAN: dynamic mud cut, the MUD excess over the reference at this moment.
        // MUD re MID over the active frames of the last few seconds (the same measurement as LISTEN's).
        const float exc = midP > 0 ? powToDb (mudP) - powToDb (midP) - kRefMud : 0.0f;
        const float cutT = std::min (std::clamp (exc, 0.0f, kMudMaxDb) * fe, kMudMaxDb);   // band dB to take away
        mudCut = (p.testOff & 4) ? 0.0f : (float) (cutT + (mudCut - cutT) * cParam);
        mudAvg = mudCut;

        // De-esser: sibilance relative to level, processed against input; only on sibilant moments.
        const float relPre = powToDb (sibPreEnv) - powToDb (broadPreEnv);
        const float relIn  = powToDb (sibInEnv) - powToDb (broadInEnv);
        float sc = 0;
        if (relPre > kSibGateDb) sc = std::clamp (relPre - relIn - kSibAllowDb, 0.0f, kSibMaxCutDb);
        sibCut = (p.testOff & 8) ? 0.0f : sc > sibCut ? sc : (float) (sc + (sibCut - sc) * 0.9);
        {
            const float hPre = powToDb (harshPreEnv) - powToDb (broadPreEnv);
            const float hIn  = powToDb (harshInEnv) - powToDb (broadInEnv);
            const float hc = hPre > kHarshGateDb ? std::clamp (hPre - hIn - kHarshAllowDb, 0.0f, kHarshMaxCutDb) : 0.0f;
            harshCut = (p.testOff & 512) ? 0.0f : hc > harshCut ? hc : (float) (hc + (harshCut - hc) * 0.9);
        }

        // Coefficients of the moving filters.
        const float extra = std::clamp ((p.lift - 6.0f) / 4.0f, 0.0f, 1.0f);
        const float presDb = voiceMood ? 0.0f : std::min (p.listen.presenceDb * fe + (moodNow == Push ? 1.0f * fe : 0.0f), kPresMaxDb);
        const float airDb = (voiceMood || exciteOn) ? 0.0f : std::min (p.listen.airDb * fe, kAirMaxDb);
        // What the user adds (the AIR knob over LISTEN's value, LIFT over 6) goes on top of the solved gains, outside the
        // loop: inside it, the loop read every band re MID and, after a deep mud cut had lowered MID, took the added air
        // and presence back (AIR 6 gave +2 dB at the top, LIFT 10 nothing over LIFT 6; owner 24 Oct).
        const float userAirDb  = (voiceMood || exciteOn) ? 0.0f : (airNow - p.listen.airDb) * std::min (1.0f, f * 4.0f) + kLiftExtraAirDb * extra;
        const float userPresDb = voiceMood ? 0.0f : kLiftExtraPresDb * extra;
        // Solved with the slow average of the mud cut (seconds): proximity and mic mud are a property of the take.
        // A cut that followed syllables or phrases deepened the quiet, mud-heavy ones and widened the level spread
        // by 3-5 dB; driving presence from it moved the level with every syllable.
        const double want3[3] = { -mudAvg, presDb, airDb };
        // Closed loop: band changes actually achieved (output re MID minus input re MID, active frames, a few seconds)
        // against those wanted; the difference is integrated into a correction (time constant kLoopSec, +-kLoopMaxDb).
        // The open-loop model (average dB over a band) does not know how the voice's energy sits inside the band.
        if (loopFrame)
        {
            loopFrame = false;
            const bool run = f > 0.0f && ! voiceMood && bandP[0][1] > 0 && bandP[1][1] > 0 && ! (p.testOff & 32);
            static constexpr int band[3] = { 0, 2, 3 };   // MUD, PRES, AIR in bandP
            for (int k = 0; k < 3; ++k)
            {
                if (! run) { loopCorr[k] *= 0.98; continue; }
                const double got = (powToDb (bandP[1][band[k]]) - powToDb (bandP[1][1])) - (powToDb (bandP[0][band[k]]) - powToDb (bandP[0][1]));
                if (k == 0 && mudAvg <= 0.0f) { loopCorr[k] *= 0.98; continue; }   // mud: only while a cut is wanted
                if (k == 2 && exciteOn) { loopCorr[k] = 0; continue; }   // AIR comes from the exciter at LISTEN's gain
                loopCorr[k] = std::clamp (loopCorr[k] + (want3[k] - got) * (kFrameSec / kLoopSec), -kLoopMaxDb, kLoopMaxDb);
            }
        }
        const double wantC[3] = { want3[0] + loopCorr[0], want3[1] + loopCorr[1], want3[2] + loopCorr[2] };
        // Projected Gauss-Seidel: each gain within its range (mud only cuts, presence and air only lift).
        // Presence may dip a little (-3) to hold the balance when a deep mud cut has lowered MID under it.
        static constexpr double gLo[3] = { -15.0, -3.0, 0.0 }, gHi[3] = { 0.0, 12.0, 10.0 };
        double g3[3] = { 0, 0, 0 };
        for (int it = 0; it < 12; ++it)
            for (int k = 0; k < 3; ++k)
            {
                const double got = eqE[k][0] * g3[0] + eqE[k][1] * g3[1] + eqE[k][2] * g3[2];
                g3[k] = std::clamp (g3[k] + (wantC[k] - got) / eqE[k][k], gLo[k], gHi[k]);
            }
        for (int k = 0; k < 3; ++k) toneNow[k] = (float) g3[k];
        for (int c = 0; c < 2; ++c)
        {
            mudEq[c].peak (fs, kMudHz, kMudQ, g3[0] * mudWeight);
            body[c].lowShelf (fs, kBodyHz, 0.707, std::clamp (bodyNow, -9.0f, 9.0f) * std::min (1.0f, f * 4.0f));
            pres[c].peak (fs, kPresHz, kPresQ, std::min (g3[1] + userPresDb, 12.0));
            air[c].highShelf (fs, kAirHz, 0.707, std::clamp (g3[2] + userAirDb, -6.0, 12.0));
            sibEq[c].peak (fs, kSibHz, kSibQ, -sibCut);
            harshEq[c].peak (fs, kHarshHz, kHarshQ, -harshCut - kHarshLiftDipDb * userPresDb / kLiftExtraPresDb);
        }
        // The exciter follows the voice (excWeight, above: none 15 dB under the voice's long-term level), so bleed and room
        // noise between words are not turned into hiss (they were, and a lifted quiet passage then carried 6 dB more AIR
        // than LISTEN calibrated).
        // Fixed at LISTEN's calibrated gain, scaled with LIFT (LIFT 6 = the target). No loop: the AIR the exciter makes
        // swings 15-18 dB between passages of one take (sibilant passages make far more), and a loop chased those
        // swings and overshot the long-term level by up to 10 dB.
        const double liftDb = fe <= 1.0f ? 20.0 * std::log10 (std::max (fe, 1.0e-3f)) * 0.5 : 4.0 * (fe - 1.0) / 0.5;
        exciteGain = (exciteOn && f > 0.0f && ! (p.testOff & 64)) ? std::pow (10.0, (excDb + liftDb + (airNow - p.listen.airDb)) / 20.0) * excWeight : 0.0;
        layerGain = (voiceMood || (p.testOff & 16)) ? 0.0 : dbToGain (kLayerDb + (moodNow == Push ? 6.0f : 0.0f) + kLiftExtraLayerDb * extra) * fe;

        // EQUAL: K-weighted loudness of input and processed output, only while the vocal sounds.
        if (active)
        {
            inLoud = smoothDb (inLoud, inK, cEqual);
            outLoud = smoothDb (outLoud, outK, cEqual);
        }
        const float eqT = (inLoud > -200 && outLoud > -200) ? std::clamp (inLoud - outLoud - rideGainDb, -kEqualRangeDb, kEqualRangeDb) : 0.0f;
        equalDb = (float) (eqT + (equalDb - eqT) * cEqualGain);
        outGain = dbToGain (std::clamp (p.outputDb, -12.0f, 12.0f));

        // Delay times.
        const double beatSec = p.bpm > 30 ? 60.0 / p.bpm : 0.0;
        slapSamples = std::clamp ((beatSec > 0 ? beatSec / 4 : 0.09), 0.04, 0.2) * fs;
        throwSamples = std::clamp ((beatSec > 0 ? beatSec : 0.375), 0.1, 2.0) * fs;
    }

    double fs = 48000;
    // Static and moving filters.
    Biquad hp[2], mudEq[2], body[2], pres[2], air[2], sibEq[2], harshEq[2], layerHp[2];
    Biquad moodA[2], moodB[2], moodC[2], moodD[2], moodE[2];
    Biquad slapHp, slapLp, throwHp, throwLp;
    AWeighting awIn, awKey, awPre;
    KWeighting kIn[2], kOut[2];
    BandFilter mudDet, midDet, presDet, airDet, sibIn, sibPre, harshIn, harshPre, outDet[4];
    double accB[2][4] = {}, frameB[2][4] = {}, bandP[2][4] = {}, loopCorr[3] = {}, bandFast[4] = {}, cFrameFast = 0;
    float frameOutMax = -300; bool haveBands = false;
    bool loopFrame = false;
    BandFilter excSrc[2], excOut[2];
    double excEnv[2] = { 0, 0 }, excDb = 0, exciteGain = 0, cExcEnv = 0;
    float expEnvDb = -300, expDb = 0, expFallDb = 0; double expGain = 1, cExpOpen = 0, cExpClose = 0;
    float excWeight = 1;
    float noiseDb = 300; double noiseAcc = 0, expPow = 0, cExpRms = 0; int noiseTicks = 0, noiseBlockTicks = 30;
    float cutNow = -1; bool exciteOn = false;
    std::vector<float> dly[3];
    size_t wpos = 0;
    // Control state.
    int ctlCount = 0, accN = 0;
    double accA = 0, accKey = 0, accInK = 0, accOutK = 0, accPre = 0, accGr = 0;
    float rideLevel = -300, inLevel = -300, ownIn = -300, ownPre = -300, beatLevel = -300, rideGainDb = 0, makeupDb = 0;
    float toneNow[3] = { 0, 0, 0 };
    double eqE[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    // Rider levels from 100 ms frames, active frames only (the LAR paper's method).
    double framePre = 0, frameKey = 0, frameIn = 0; int frameTicks = 0; float frameMaxDb = -300; bool frameActive = false;
    bool haveOwn = false;
    double rideGain = 1, keepGain = 1, compEnv = 0, layerGain = 0, outGain = 1;
    float keepDb = 0;
    float compThrDb = 0, compAmount = 0;
    double compHeld = 0; int compHoldLeft = 0, compHoldSamples = 960;
    double sibInEnv = 0, sibPreEnv = 0, broadInEnv = 0, broadPreEnv = 0, harshInEnv = 0, harshPreEnv = 0;
    float harshCut = 0;
    float mudCut = 0, sibCut = 0, inLoud = -300, outLoud = -300, equalDb = 0;
    double ceilGain = 1, slapSamples = 4320, throwSamples = 18000;
    float throwSend = 0;
    float f = 0, mix = 0, airNow = 0, bodyNow = 0, hpHz = 0;
    int moodNow = -1;
    bool primed = false, designed = false;
    float bypassStep = 0.01f, wetNow = 1, wetStep = 0.001f;
    double cRideLevel = 0, cOwn = 0, cBeat = 0, cRide = 0, cCompAtt = 0, cCompRel = 0, cFast = 0, cSibRel = 0;
    double cFrameMud = 0, accMud = 0, accMid = 0, frameMud = 0, frameMid = 0, mudP = 0, midP = 0;
    float mudAvg = 0, inFast = -300, mudWeight = 1;
    double cInFast = 0;
    double cEqual = 0, cEqualGain = 0, cCeilRel = 0, cParam = 0, cFrameRide = 0, cFrameOwn = 0, cFrameBeat = 0;
    int frameLenTicks = 300;
    Report rep;
};

/** Exciter calibration, run once after LISTEN (message thread, not the audio thread): the listened audio goes through
    a whole engine at LIFT 6 with the exciter at 0 dB and its loop held, and the AIR that comes out (the analyser's own
    measurement: AIR re MID on active frames) sets the exciter's gain for the target. Every other stage adds a little
    in the AIR band (the presence layer's saturation, the compressor), so the exciter alone could not be calibrated.
    mono: the listened audio; returns false when nothing was changed. */
inline bool calibrateExciter (Settings& s, const float* mono, size_t n, double fs)
{
    if (! s.bandLimited || n < (size_t) (2 * fs)) return false;
    BoebEngine eng;
    eng.prepare (fs, 1024);
    BoebEngine::Parameters p;
    p.lift = 6; p.listen = s; p.listen.exciteDb = 0; p.airDb = s.airDb;
    BoebAnalyser an;
    an.prepare (fs, (double) n / fs + 1);
    std::vector<float> outL (1024), outR (1024);
    // Two passes: the first lets the tone loop settle (it moves MID by a dB or two), the second is measured.
    for (int pass = 0; pass < 2; ++pass)
        for (size_t pos = 0; pos < n; pos += 1024)
        {
            const int len = (int) std::min ((size_t) 1024, n - pos);
            const float* in[2] = { mono + pos, mono + pos };
            float* out[2] = { outL.data(), outR.data() };
            eng.process (in, out, 2, nullptr, 0, len, p);
            const float* o[2] = { outL.data(), outR.data() };
            if (pass == 1) an.feed (o, 2, nullptr, 0, len);
        }
    const Diagnosis d = an.diagnose();
    if (! d.valid) return false;
    s.exciteDb = std::clamp ((kRefAir - kExciteTargetBelowRefDb) - d.airDb, -20.0f, 40.0f);
    return true;
}

} // namespace svl::boeb
