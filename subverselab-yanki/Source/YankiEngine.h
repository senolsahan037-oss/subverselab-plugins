#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#ifndef LOCK_AIM
#define LOCK_AIM 0.25
#endif
#ifndef LOCK_WOW_PHASE
#define LOCK_WOW_PHASE 1.0
#endif
#ifndef CAP_AIM
#define CAP_AIM 0.0
#endif
#ifndef CLEAN_SERVO
#define CLEAN_SERVO 1.0
#endif
#ifndef CLEAN_TAPER
#define CLEAN_TAPER 1.5
#endif
#ifndef CLEAN_HOLD
#define CLEAN_HOLD 1.0145
#endif
#ifndef ECHO_WET
#define ECHO_WET 0.70
#endif
namespace yanki {

struct Controls {
    double timeMs = 360.0;
    int sync = 0;              // TIME division at bpm: 0=free (timeMs), 1=1/8, 2=1/4, 3=dotted 1/8, 4=1/16, 5=1/2
    bool cleanVoice = false;   // ECHO voicing: the clean (unity-record) loop with its own wet level
    int mood = 0;              // informational: which MOOD the voicing came from (moodControls); the engine does not read it
    double bpm = 120.0;
    int mode = 12;
    double feedback = 0.55;
    double toneHz = 4200.0;
    double drive = 0.25;
    double wear = 0.20;
    double mix = 0.35;
    double width = 0.65;
    double drama = 0.0;       // 0..1
    bool freeze = false;
    double spread = 0.0;      // 0..1
    double reverse = 0.0;     // 0..1

    // ---- Swell controls (classic loop only; 2026-09-30) ----
    // With all of them at their defaults the classic loop runs the exact
    // original arithmetic, so the four original presets stay bit-identical.
    double rise = -1.0;       // <0 = off (preset's own loop gain); 0..1 = target build rate
    bool capOn = false;       // false = no cap (original behaviour)
    double capDb = 0.0;       // -12..+12 dB relative to the dry phrase RMS
    double darken = 0.0;      // 0..1 extra per-pass high-cut
    int endMode = 0;          // 0 HOLD, 1 CUT, 2 DUMP, 3 FREEZE
    bool endTrigger = false;  // rising edge fires END
    double lockBars = 0.0;    // >0 = tempo lock: loop reaches CAP lockBars bars (bpm) after the phrase onset
    double endBars = 0.0;     // >0 = END fires endBars bars (bpm) after the phrase onset
};

enum EndMode { kEndHold = 0, kEndCut = 1, kEndDump = 2, kEndFreeze = 3 };

class YankiEngine {
public:
    static constexpr double kMaxEffectiveFeedback = 0.92;   // classic loop
    static constexpr double kStageMaxFeedback = 0.90;       // stage loop (DRAMA/SPREAD/REVERSE)
    static constexpr double kStageWetLevel = 0.44;          // head 1 level at MIX 0.5 (about -7 dB)
    static constexpr double kEchoWetLevel = ECHO_WET;
    static constexpr double kCleanHoldGain = CLEAN_HOLD;
    static constexpr double kCleanTaperDb = CLEAN_TAPER;
    static constexpr double kCleanServoDb = CLEAN_SERVO;    // servo target re CAP (the servo reads the heads before the playback tone filter)    // clean loop: RISE -> hold taper zone under CAP    // clean/stage loop gain that neither grows nor decays (measured)       // ECHO: first repeat about -7.5 dB under the dry at MIX 0.3

    void prepare(double sr) {
        sr_ = sr;
        // Tape: the classic 2 s loop length (the original moods run exactly this
        // modulus, bit-identical); long divisions (e.g. 1/2 note: head 3 at 3x)
        // switch to a 6.2 s loop (and back to 2 s when the time shortens again).
        // The switch keeps what is on the tape (ensureTape); only a tape that has
        // not recorded anything yet starts empty, as before.
        shortLen_ = static_cast<size_t>(std::ceil(sr_ * 2.0)) + 8U;
        longLen_ = static_cast<size_t>(std::ceil(sr_ * 6.2)) + 8U;
        buffer_.assign(longLen_, 0.0f);
        scratch_.assign(longLen_, 0.0f);
        tapeLen_ = shortLen_;
        hist_.assign(2U * (static_cast<size_t>(std::ceil(sr_ * 2.0)) + 8U), 0.0f);
        const size_t lim = static_cast<size_t>(std::ceil((kDetectSec + kAttackSec) * sr_)) + 4U;
        dqIdx_.assign(lim + 2U, 0); dqVal_.assign(lim + 2U, 0.0); boxBuf_.assign(lim + 2U, 0.0); levBuf_.assign(lim + 2U, 0.0);
        detN_ = std::max<size_t>(1U, static_cast<size_t>(std::lround(kDetectSec * sr_)));
        detBuf_.assign(detN_ + static_cast<size_t>(std::ceil(kAttackSec * sr_)) + 8U, 0.0);   // + room for the leveler's delayed window
        reset();
    }

    void reset() {
        std::fill(buffer_.begin(), buffer_.end(), 0.0f);
        tapeLen_ = shortLen_;
        writePos_ = 0;
        sample_ = 0;
        prevInput_ = 0.0f;
        hpX_ = hpY_ = lpY_ = 0.0f;
        wowPhase_ = 0.0;
        flutterPhase_ = 0.0;
        freezeActive_ = false;
        freezeSamples_ = 0;
        sHpX_ = sHpY_ = sShelfLp_ = 0.0;
        sToneL_ = sToneR_ = 0.0;
        for (int h = 0; h < 3; ++h) sHeadL_[h] = sHeadR_[h] = 0.0;
        revPhase_ = 0;
        revOldPhase_ = 0;
        revXfade_ = 0;
        revQuiet_ = static_cast<uint64_t>(sr_);   // start "quiet": the first onset locks the window
        revEnv_ = 0.0;
        // swell state
        std::fill(hist_.begin(), hist_.end(), 0.0f);
        histPos_ = 0;
        dkY_ = 0.0f;
        onEnv_ = 0.0;
        onQuiet_ = static_cast<uint64_t>(sr_ * kPhraseGapSec);   // armed: the first onset starts a phrase
        phraseStart_ = 0;
        phraseSeen_ = false;
        accMs_ = 0.0; accN_ = 0; refMs_ = 0.0;
        detMs_ = 0.0; gainDb_ = 0.0; detSum_ = 0.0; detPos_ = 0; levWinSum_ = 0.0; levMs_ = 0.0;
        std::fill(detBuf_.begin(), detBuf_.end(), 0.0);
        limWin_ = 0; boxWin_ = 0; dqHead_ = dqTail_ = 0; boxSum_ = 0.0; boxN_ = 0; limIdx_ = 0;
        std::fill(boxBuf_.begin(), boxBuf_.end(), 0.0);
        endGain_ = 1.0; endTarget_ = 1.0;
        endFired_ = false; endFreeze_ = false; lastTrig_ = false;
        dumpActive_ = false; dumpJ_ = 0; dumpW_ = 0; dumpFrom_ = 0;
        lastCeilDb_ = 0.0; lastGainDb_ = 0.0; lockFade_ = 1.0; wowGlide_ = 0.0; loopMs_ = 0.0; sDkS_ = 0.0; mDry_ = mLoop_ = 0.0; dryDb_ = loopDb_ = -240.0; swelling_ = false;
        std::fill(levBuf_.begin(), levBuf_.end(), 0.0); levSum_ = 0.0; levN_ = 0; levIdx_ = 0;
    }

    // ---- meters (updated once per process() block) ----
    // Loop return = the echo part of the output (output minus the dry part), stereo RMS in dBFS.
    double loopLevelDb() const { return loopDb_; }
    // Dry = the input, stereo RMS in dBFS.
    double dryLevelDb() const { return dryDb_; }
    // True when the loop swells: RISE set (RISE 0 = the hold point) or the loop gain >= the hold point.
    bool swelling() const { return swelling_; }
    static bool swellingNow(const Controls& c) {
        if (c.rise >= 0.0 || c.lockBars > 0.0) return true;
        if (stageVoice(c)) return std::min(kStageMaxFeedback, std::clamp(c.feedback, 0.0, 1.1)) >= kCleanHoldGain;
        return criticalMul(c) <= 1.0;   // the preset's own loop gain is at/above the hold point
    }
    // FEEDBACK value at which the loop neither grows nor decays (the hold point).
    static double holdFeedback(const Controls& c) {
        if (stageVoice(c)) return kCleanHoldGain;
        return std::min(kMaxEffectiveFeedback, std::clamp(c.feedback, 0.0, 1.1)) * criticalMul(c);
    }
    // One FEEDBACK knob for every mood: 0..0.7 -> feedback 0..hold point (decaying
    // repeats, RISE off); 0.7..1 -> RISE 0..1 (swelling loop; RISE 0 = hold).
    static constexpr double kKnobHold = 0.7;
    static void feedbackKnob(Controls& c, double knob) {
        const double k = std::clamp(knob, 0.0, 1.0), hold = holdFeedback(c);
        if (k < kKnobHold) { c.rise = -1.0; c.feedback = hold * k / kKnobHold; }
        else { c.feedback = hold; c.rise = (k - kKnobHold) / (1.0 - kKnobHold); }
    }

    // Diagnostics for the selftest / CLI (last processed sample).
    double lastGainDb() const { return lastGainDb_; }
    double refRmsDb() const { return 10.0 * std::log10(std::max(refMs_, 1e-24)); }

    int latency() const { return 0; }

    // ---- Swell helpers (public so the CLI can print the mappings) ----
    static bool swellVoice(const Controls& c) {
        return c.rise >= 0.0 || c.capOn || c.darken > 0.0 || c.endMode != kEndHold || c.endTrigger
            || c.lockBars > 0.0 || c.endBars > 0.0;
    }
    // RISE -> loop gain: q = 2^(2 RISE) times the critical loop gain of the
    // preset (q = 1: the loop just holds; q > 1: it swells). RISE off keeps
    // the preset's own loop gain. The dB/s a RISE produces on the reference
    // vocal is measured (kRiseRateTable, selftest prints it) -- the model is
    // only used to find the critical gain.
    static constexpr double kRiseOctaves = 2.0;
    // loop limiter (CAP)
    static constexpr double kAttackSec = 0.200, kReleaseSec = 0.800, kDetectSec = 0.400;   // main detector = 400 ms moving-window RMS (about one tape pass)
    static constexpr double kCapAimDb = CAP_AIM;   // 400 ms window target re CAP (measured max: selftest cap_*)
    // Tempo lock: the loop runs at a RISE that outgrows the arrival line and
    // is held at CAP by the in-loop limiter; an arrival leveler OUTSIDE the
    // loop makes the audible per-pass level follow a line (linear in dB) that
    // lands on CAP exactly on the downbeat of bar N and holds it there.
    static constexpr double kLockRampDb = 12.0;        // body of the arrival line: 12 dB over the lock time ...
    static constexpr double kLockBodyMin = 3.0;        // ... but never flatter than 3 dB/s
    static constexpr double kLockFinalSlope = 12.0;    // last 0.5 s before the downbeat: 12 dB/s (sharp arrival)
    static constexpr double kLockFinalSec = 0.5;
    static constexpr double kLockDepthMax = 24.0;      // dB, the line never starts lower than CAP - 24
    static constexpr double kLockAimDb = LOCK_AIM;        // the line lands 0.1 dB under CAP
    static constexpr double kLockBoostDb = 6.0;        // leveler may lift the loop up to +6 dB ...
    static constexpr double kLockBoostHoldSec = 0.3;
    static constexpr double kLockLiftCeilDbFS = -12.0; // the lift never raises the loop above -12 dBFS RMS   // ... until 0.3 s after the downbeat, then the lift fades out over 1 s
    static constexpr double kLockMargin = 1.3;         // the loop builds 1.3x faster than the line body
    static double lockSlope(const Controls& c) { const double T = lockSeconds(c); return T > 0.0 ? std::max(kLockRampDb / T, kLockBodyMin) : 0.0; }
    // Arrival line (dB rel. CAP) dT seconds before the downbeat.
    static double lockLineDb(double slopeBody, double dT) {
        dT = std::max(0.0, dT);
        return -std::min(kLockDepthMax, kLockFinalSlope * std::min(dT, kLockFinalSec) + slopeBody * std::max(0.0, dT - kLockFinalSec));
    }
    // The held loop's level follows the 0.5 Hz wow; at 120 bpm a bar is
    // exactly one wow cycle. In tempo lock the wow is re-phased at the phrase
    // onset so it passes pi on the target downbeat (jump if the tape is
    // silent, else a glide at <= 2x wow speed). Target phase chosen by scan
    // (0, 0.5, 1, 1.5 pi; 3 moods x 5 tempos, see STATUS.md). The earlier
    // 0.5 pi choice was measured with the lookahead bug (fixed 2026-09-30).
    static constexpr double kLockWowPhase = LOCK_WOW_PHASE * 3.141592653589793;
    static double riseToOverGain(double rise) { return std::pow(2.0, kRiseOctaves * std::clamp(rise, 0.0, 1.0)); }
    // Measured build (dB/s from 0 to +6 dB rel. the dry phrase, 0.5 s RMS) on
    // the 3 s reference vocal at RISE 0, 0.1 .. 1.0: the SLOWEST of DubSpiral /
    // WornLoop / ThreeHeads, made non-decreasing (conservative for the tempo
    // lock). Re-measured and printed by the selftest (INFO rise_build_rate).
    static constexpr double kRiseRate[11] = {0.00, 1.00, 1.60, 2.86, 5.22, 6.00, 7.06, 7.06, 7.50, 7.50, 8.00};
    static double riseRateMeasured(double rise) {
        const double* t = kRiseRate;
        const double x = std::clamp(rise, 0.0, 1.0) * 10.0; const int i = std::min(9, static_cast<int>(x));
        return t[i] + (t[i + 1] - t[i]) * (x - i);
    }
    static double rateToRise(double r) {
        for (int k = 0; k < 1000; ++k) { const double x = k / 999.0; if (riseRateMeasured(x) >= r) return x; }
        return 1.0;
    }
    // Small-signal loop model of the classic loop: every active head returns
    // a * (count) * x^k per pass of the base delay, a = the per-head loop gain
    // (0.5 head average * limiter slope * FEEDBACK*0.45 * record drive * the
    // HP/LP magnitude at the loop's dominant frequency). The loop grows by
    // -20 log10(x) dB per base delay where a * sum(w_k x^k) = 1.
    static void headWeights(int mode, double w[3]) {
        w[0] = w[1] = w[2] = 0.0;
        if (mode == 1 || mode == 4 || mode == 7 || mode == 10 || mode == 12) w[0] += 1;
        if (mode == 2 || mode == 4 || mode == 5 || mode == 7 || mode == 9 || mode == 11 || mode == 12) w[1] += 1;
        if (mode == 3 || mode == 5 || mode == 6 || mode == 7 || mode == 8 || mode == 10 || mode == 11 || mode == 12) w[2] += 1;
        if (mode == 8 || mode == 9 || mode == 11) w[0] += 1;
        if (mode == 10) w[1] += 1;
    }
    // TIME in ms: free, or a note division at the host tempo.
    static double syncMs(const Controls& c) {
        if (c.sync == 0) return c.timeMs;
        const double quarter = 60000.0 / std::max(20.0, c.bpm);
        if (c.sync == 1) return quarter * 0.5;     // 1/8
        if (c.sync == 2) return quarter;           // 1/4
        if (c.sync == 4) return quarter * 0.25;    // 1/16
        if (c.sync == 5) return quarter * 2.0;     // 1/2
        return quarter * 0.75;                     // 3 = dotted 1/8
    }
    static double baseSeconds(const Controls& c) { return std::clamp(syncMs(c) * 0.001, 1.0 / 48000.0, 2.0); }
    static double perHeadLoopGain(const Controls& c) {
        const double drv = std::clamp(c.drive, 0.0, 1.0);
        const double fb = std::min(kMaxEffectiveFeedback, std::clamp(c.feedback, 0.0, 1.1));
        return 0.5 * (1.0 + 1.5 * drv) * fb * 0.45 * (1.0 + 7.0 * drv) * kModelFilterLoss;
    }
    // Model growth rate (dB/s) for a loop-gain multiplier (0 at the critical gain).
    static double modelRate(const Controls& c, double mul) {
        double w[3]; headWeights(c.mode, w);
        const double a = perHeadLoopGain(c) * mul;
        double lo = 1e-4, hi = 1e4;   // bisection on x
        for (int it = 0; it < 200; ++it) {
            const double x = std::sqrt(lo * hi);
            const double s = a * (w[0] * x + w[1] * x * x + w[2] * x * x * x);
            if (s > 1.0) hi = x; else lo = x;
        }
        return -20.0 * std::log10(std::sqrt(lo * hi)) / baseSeconds(c);
    }
    // Loop-gain multiplier at which the model loop neither grows nor decays.
    static double criticalMul(const Controls& c) {
        double w[3]; headWeights(c.mode, w);
        return 1.0 / (perHeadLoopGain(c) * std::max(w[0] + w[1] + w[2], 1e-9));
    }
    static double riseToLoopMul(const Controls& c, double rise) { return rise >= 0.0 ? riseToOverGain(rise) * criticalMul(c) : 1.0; }
    static double lockSeconds(const Controls& c) { return c.lockBars * 4.0 * 60.0 / std::max(20.0, c.bpm); }
    // RISE used by the tempo lock: the loop must outgrow the ceiling ramp.
    static double lockRise(const Controls& c) {
        const double T = lockSeconds(c);
        return T > 0.0 ? rateToRise(kLockMargin * lockSlope(c)) : c.rise;
    }

    // The stage voicing (DRAMA / SPREAD / REVERSE) runs its own loop. The
    // classic loop below is kept verbatim so the original presets stay
    // bit-identical.
    static bool stageVoice(const Controls& c) {
        return c.drama > 0.0 || c.spread > 0.0 || c.reverse > 0.0 || c.cleanVoice;
    }

    void process(float* left, float* right, int n, const Controls& c) {
        if (stageVoice(c)) { processStage(left, right, n, c); return; }
        const double baseMs = syncedTime(c);
        const double base = std::clamp(baseMs * 0.001 * sr_, 1.0, sr_ * 2.0 - 4.0);
        const double wowDepth = c.wear * 0.0018 * sr_;
        ensureTape(3.0 * base + std::abs(wowDepth) * (1.0 + 1.8 * std::clamp(c.drama, 0.0, 1.0)) + 8.0);
        const double drive = 1.0 + std::clamp(c.drive, 0.0, 1.0) * 7.0;
        const double fb = std::clamp(c.feedback, 0.0, 1.1);
        const double tone = std::clamp(c.toneHz, 1000.0, 8000.0);
        const double lpCoeff = 1.0 - std::exp(-2.0 * 3.141592653589793 * tone / sr_);
        const double hpCoeff = std::exp(-2.0 * 3.141592653589793 * 120.0 / sr_);
        const double wowStep = 2.0 * 3.141592653589793 * 0.5 / sr_;
        const double flutterStep = 2.0 * 3.141592653589793 * 6.0 / sr_;
        const double mix = std::clamp(c.mix, 0.0, 1.0);
        const double drama = std::clamp(c.drama, 0.0, 1.0);
        const bool wantFreeze = c.freeze || endFreeze_;
        if (wantFreeze && !freezeActive_) { freezeActive_ = true; freezeSamples_ = 0; }
        if (!wantFreeze) freezeActive_ = false;
        const double width = std::clamp(c.width, 0.0, 1.0);
        // ---- swell block constants (unused when swell is off) ----
        const bool swell = swellVoice(c);
        SwellBlock sw{};
        if (swell) setupSwell(c, base, mix, std::abs(wowDepth) * (1.0 + 1.8 * drama), sw);
        for (int i = 0; i < n; ++i) {
            const float inL = left[i];
            const float inR = right[i];
            const float in = 0.5f * (inL + inR);
            const double pressure = 1.0 + drama * 0.08 * std::sin (2.0 * 3.141592653589793 * 0.1 * double (sample_) / sr_);
            const double wobble = wowDepth * (1.0 + drama * 1.8) * (0.72 * std::sin(wowPhase_) + 0.28 * std::sin(flutterPhase_));
            wowPhase_ += wowStep;
            flutterPhase_ += flutterStep;
            if (wowPhase_ > 6.283185307) wowPhase_ -= 6.283185307;
            if (flutterPhase_ > 6.283185307) flutterPhase_ -= 6.283185307;

            const float h1 = readCubic(static_cast<double>(base) + wobble);
            const float h2 = readCubic(static_cast<double>(base * 2.0) + wobble);
            const float h3 = readCubic(static_cast<double>(base * 3.0) + wobble);
            float eL = 0.0f;
            float eR = 0.0f;
            const auto addHead = [&](float v, double pan) {
                const double p = std::clamp(pan * width, -1.0, 1.0);
                const float gl = static_cast<float>(0.5 * (1.0 - p));
                const float gr = static_cast<float>(0.5 * (1.0 + p));
                eL += v * gl;
                eR += v * gr;
            };
            if (c.mode == 1 || c.mode == 4 || c.mode == 7 || c.mode == 10 || c.mode == 12) addHead(h1, -1.0);
            if (c.mode == 2 || c.mode == 4 || c.mode == 5 || c.mode == 7 || c.mode == 9 || c.mode == 11 || c.mode == 12) addHead(h2, 1.0);
            if (c.mode == 3 || c.mode == 5 || c.mode == 6 || c.mode == 7 || c.mode == 8 || c.mode == 10 || c.mode == 11 || c.mode == 12) addHead(h3, -1.0);
            if (c.mode == 8) addHead(h1, -1.0);
            if (c.mode == 9) addHead(h1, -1.0);
            if (c.mode == 10) addHead(h2, 1.0);
            if (c.mode == 11) addHead(h1, -1.0);
            float revL = 0.0f, revR = 0.0f;
            if (swell) {
                double pw = 0.0;
                if (sw.capActive) {
                    const double d = sw.look;
                    const double f1 = readCubic(base - d + wobble), f2 = readCubic(2.0 * base - d + wobble), f3 = readCubic(3.0 * base - d + wobble);
                    const double gl1 = 0.5 * (1.0 + width), gr1 = 0.5 * (1.0 - width);   // heads 1/3 (pan -width)
                    const double fL = sw.w[0] * f1 * gl1 + sw.w[1] * f2 * gr1 + sw.w[2] * f3 * gl1;
                    const double fR = sw.w[0] * f1 * gr1 + sw.w[1] * f2 * gl1 + sw.w[2] * f3 * gr1;
                    pw = sw.mix2 * 0.5 * (fL * fL + fR * fR);
                }
                const float g = swellGain(c, sw, inL, inR, pw);
                eL *= g; eR *= g;
                double rl = 0.0, rr = 0.0;
                swellHistory(eL, eR, rl, rr);
                revL = static_cast<float>(rl); revR = static_cast<float>(rr);
            }
            const float feedbackSignal = 0.5f * (eL + eR);
            const float hp = feedbackSignal - hpX_ + static_cast<float>(hpCoeff * hpY_);
            hpX_ = feedbackSignal;
            hpY_ = hp;
            lpY_ += static_cast<float>(lpCoeff) * (hp - lpY_);
            float loopSig = lpY_;
            if (swell && sw.darkCoeff > 0.0f) { dkY_ += sw.darkCoeff * (lpY_ - dkY_); loopSig = dkY_; }
            const float limited = static_cast<float>(std::tanh(static_cast<double>(loopSig) * (1.0 + 1.5 * c.drive)));
            const float pre = freezeActive_ ? 0.0f : in - 0.78f * prevInput_;
            prevInput_ = in;
            // DRAMA may swell the requested value, but the normal tape loop is
            // always contractive. FREEZE is intentionally a bounded special case.
            const double effectiveFb = std::min(kMaxEffectiveFeedback, fb * pressure);
            const double loopGain0 = freezeActive_ ? 0.985 : effectiveFb * 0.45;
            const double loopGain = (swell && !freezeActive_) ? loopGain0 * sw.riseMul : loopGain0;
            const float record = static_cast<float>(std::tanh(static_cast<double>(pre + limited * loopGain) * drive));
            buffer_[writePos_] = record;
            writePos_ = (writePos_ + 1U) % tapeLen_;
            if (swell) { eL = static_cast<float>(eL * lockFade_) + revL; eR = static_cast<float>(eR * lockFade_) + revR; }
            const float outL = inL + eL;
            const float outR = inR + eR;
            left[i] = static_cast<float>(inL * (1.0 - mix) + outL * mix);
            right[i] = static_cast<float>(inR * (1.0 - mix) + outR * mix);
            meterAdd(inL, inR, static_cast<double>(left[i]) - inL, static_cast<double>(right[i]) - inR);
            ++sample_;
        }
        meterEnd(n, swellingNow(c));
    }

private:
    // ======================= Swell (classic loop only) =======================
    // The loop limiter sits on the head sum, i.e. INSIDE the loop: the gain
    // feeds both the output and the feedback path, so the tape keeps being
    // re-recorded through the per-pass high-cut (the tone keeps darkening)
    // while the level holds. Its detector reads the heads LOOKAHEAD seconds
    // early: that audio is already on the tape (lookahead < TIME), so the
    // 200 ms attack is aligned with the audio and the cap is not overshot.
    static constexpr double kModelFilterLoss = 0.59;   // 0.95 (HP/LP near 600 Hz) x 0.62 tape-saturation loss, measured: see STATUS.md
    static constexpr double kPhraseGapSec = 1.0;       // quiet needed before a new phrase onset
    static constexpr double kEndRampSec = 0.030;
    static constexpr double kTwoPi = 6.283185307179586;
    struct SwellBlock {
        double riseMul = 1.0;
        bool riseOn = false;       // RISE set (or tempo lock)
        double cleanQ = 1.0;       // clean/stage loop gain per pass when riseOn
        float darkCoeff = 0.0f;
        bool capActive = false;
        double capDb = 0.0;
        double lockT = 0.0;        // seconds, 0 = no tempo lock
        double lockSlope = 0.0;    // dB/s
        double endAfter = 0.0;     // seconds after onset, 0 = none
        double look = 0.0;         // samples (sliding-min span)
        double box = 0.0;          // samples (attack ramp)
        size_t levOff = 0;         // samples: leveler window ends this much before the limiter window
        double w[3] = {0, 0, 0};
        double mix2 = 0.0;
        double aAtk = 0, aRel = 0, endStep = 0;
        double dumpW = 0.0;        // samples
        double wowStep = 0.0;
    };

    void setupSwell(const Controls& c, double base, double mix, double wobbleMax, SwellBlock& sw) const {
        double rise = c.rise;
        if (c.lockBars > 0.0) rise = lockRise(c);
        sw.riseMul = riseToLoopMul(c, rise);
        // clean/stage loop: RISE = the same measured build rate (dB/s) as the
        // classic table; the loop gain per pass = 10^(rate * TIME / 20) x the hold gain
        sw.riseOn = rise >= 0.0;
        sw.cleanQ = kCleanHoldGain * std::pow(10.0, riseRateMeasured(std::max(0.0, rise)) * (base / sr_) / 20.0);
        const double dk = std::clamp(c.darken, 0.0, 1.0);
        sw.darkCoeff = dk > 0.0 ? static_cast<float>(1.0 - std::exp(-2.0 * 3.141592653589793 * std::min(0.45 * sr_, 16000.0 * std::pow(2.0, -5.0 * dk)) / sr_)) : 0.0f;
        sw.capActive = c.capOn || c.lockBars > 0.0;
        sw.capDb = std::clamp(c.capDb, -12.0, 12.0);
        sw.lockT = c.lockBars > 0.0 ? lockSeconds(c) : 0.0;
        sw.lockSlope = lockSlope(c);
        sw.endAfter = c.endBars > 0.0 ? c.endBars * 4.0 * 60.0 / std::max(20.0, c.bpm) : 0.0;
        // detector window + attack must be inside the lookahead for a strict cap
        // the heads move by up to wobbleMax (wow + flutter): the lookahead tap
        // must stay behind the write head at every wow phase
        sw.look = std::max(1.0, std::floor(std::min((kDetectSec + kAttackSec) * sr_, base - 4.0 - std::ceil(wobbleMax))));
        sw.box = std::max(1.0, std::floor(std::min(kAttackSec * sr_, sw.look)));
        {   // leveler lookahead = (window + box) / 2, limited by the tape
            const double want = std::floor(0.5 * (kDetectSec * sr_ + sw.box));
            sw.levOff = static_cast<size_t>(std::max(0.0, sw.look - std::min(want, sw.look)));
        }
        headWeights(c.mode, sw.w);
        sw.mix2 = mix * mix;
        sw.aAtk = 1.0 - std::exp(-1.0 / (kAttackSec * sr_));
        sw.aRel = 1.0 - std::exp(-1.0 / (kReleaseSec * sr_));
        sw.endStep = 1.0 / (kEndRampSec * sr_);
        const double longest = sw.w[2] > 0 ? 3.0 : sw.w[1] > 0 ? 2.0 : 1.0;
        sw.dumpW = std::min(longest * base, 1.9 * sr_);
        sw.wowStep = kTwoPi * 0.5 / sr_;
    }

    void fireEnd(const Controls& c, const SwellBlock& sw) {
        endFired_ = true;
        if (c.endMode == kEndCut) endTarget_ = 0.0;
        else if (c.endMode == kEndDump) {
            endTarget_ = 0.0;
            dumpActive_ = true; dumpJ_ = 0; dumpW_ = static_cast<uint64_t>(sw.dumpW); dumpFrom_ = histPos_;
        } else if (c.endMode == kEndFreeze) endFreeze_ = true;
    }

    // Phrase tracking, END triggers, CAP limiter, END ramps, tempo-lock leveler.
    // Returns the gain for the loop (head) signal; futurePw = power of the
    // audible loop LOOK samples ahead (0 when CAP is off).
    float swellGain(const Controls& c, const SwellBlock& sw, double inL, double inR, double futurePw) {
        // 1) phrase tracking on the dry input (onset, reference RMS)
        const double a = std::max(std::abs(static_cast<double>(inL)), std::abs(static_cast<double>(inR)));
        onEnv_ = a > onEnv_ ? a : onEnv_ * std::exp(-1.0 / (0.05 * sr_));
        const uint64_t gapN = static_cast<uint64_t>(kPhraseGapSec * sr_);
        if (onEnv_ < 0.00316) { if (onQuiet_ < gapN) ++onQuiet_; }
        else if (onEnv_ > 0.01) {
            if (onQuiet_ >= gapN) {
                // new phrase after >= 1 s of quiet: re-arm the swell
                phraseStart_ = sample_; phraseSeen_ = true; accMs_ = 0.0; accN_ = 0;
                endFired_ = false; endFreeze_ = false; dumpActive_ = false; endTarget_ = 1.0;
                if (sw.lockT > 0.0) {
                    // wowPhase_ already points at the next sample
                    double want = std::fmod(kLockWowPhase - sw.wowStep * (sw.lockT * sr_ - 1.0), kTwoPi);
                    if (want < 0.0) want += kTwoPi;
                    double dlt = std::fmod(want - wowPhase_, kTwoPi);
                    if (dlt > kTwoPi * 0.5) dlt -= kTwoPi;
                    if (dlt < -kTwoPi * 0.5) dlt += kTwoPi;
                    if (loopMs_ < 1e-14) { wowPhase_ = want; wowGlide_ = 0.0; } else wowGlide_ = dlt;
                }
            }
            onQuiet_ = 0;
        }   // between -50 and -40 dBFS: hold (hysteresis)
        if (onEnv_ >= 0.00316) {
            accMs_ += 0.5 * (static_cast<double>(inL) * inL + static_cast<double>(inR) * inR); ++accN_;
            if (accN_ >= static_cast<uint64_t>(0.05 * sr_)) refMs_ = accMs_ / static_cast<double>(accN_);
        }
        if (wowGlide_ != 0.0) {
            const double st = std::clamp(wowGlide_, -sw.wowStep, sw.wowStep);
            wowPhase_ += st; wowGlide_ -= st;
            if (wowPhase_ < 0.0) wowPhase_ += kTwoPi;
            if (wowPhase_ > kTwoPi) wowPhase_ -= kTwoPi;
        }
        loopMs_ += 0.001 * (futurePw - loopMs_);   // tape-silence check for the wow re-phase
        const double tSince = phraseSeen_ ? static_cast<double>(sample_ - phraseStart_) / sr_ : -1.0;

        // 2) END triggers: rising edge of endTrigger, or endBars after the onset
        if (c.endTrigger && !lastTrig_ && !endFired_) fireEnd(c, sw);
        lastTrig_ = c.endTrigger;
        if (sw.endAfter > 0.0 && tSince >= sw.endAfter && !endFired_) fireEnd(c, sw);

        // 3) loop limiter with tape lookahead
        double target = 0.0;
        if (sw.capActive && refMs_ > 0.0 && sw.mix2 > 0.0) {
            const double pw = futurePw;
            const size_t rn = detBuf_.size(), dn = detN_;
            detBuf_[detPos_] = pw;
            // newest window: the dn samples ending at t+LOOK
            detSum_ += pw - detBuf_[(detPos_ + rn - dn) % rn];
            // leveler window: the dn samples ending levOff samples earlier
            const size_t off = sw.levOff;
            levWinSum_ += detBuf_[(detPos_ + rn - off) % rn] - detBuf_[(detPos_ + 2U * rn - off - dn) % rn];
            detPos_ = (detPos_ + 1U) % rn;
            detMs_ = std::max(0.0, detSum_) / static_cast<double>(dn);
            levMs_ = std::max(0.0, levWinSum_) / static_cast<double>(dn);
            const double ceilDb = sw.capDb + kCapAimDb;
            lastCeilDb_ = ceilDb;
            const double needDb = ceilDb + 10.0 * std::log10(refMs_) - 10.0 * std::log10(std::max(detMs_, 1e-30));
            if (needDb < 0.0) target = needDb;
        }
        // Lookahead limiter: target = the gain the 400 ms RMS window ending
        // LOOK samples ahead needs. Sliding minimum over LOOK+1 samples, then
        // a 200 ms box average (the attack ramp). A strict guarantee needs
        // LOOK >= window + attack = 600 ms; the tape only holds TIME-4 samples
        // of future head-1 audio (416 ms for DubSpiral), so the window is
        // partly covered (measured max: selftest cap_*). The lookahead keeps a
        // full wow depth behind the write head (2026-09-30 fix: before, the tap
        // crossed the write head twice per wow cycle and read stale tape).
        // Release: 800 ms one-pole, upward only.
        const size_t win = static_cast<size_t>(sw.look) + 1U, bwin = static_cast<size_t>(sw.box) + 1U;
        if (win != limWin_ || bwin != boxWin_) {
            limWin_ = win; boxWin_ = bwin; dqHead_ = dqTail_ = 0; boxSum_ = 0.0; boxN_ = 0; limIdx_ = 0; std::fill(boxBuf_.begin(), boxBuf_.end(), 0.0);
        }
        const size_t cap = dqIdx_.size();
        while (dqTail_ != dqHead_ && dqVal_[(dqTail_ + cap - 1U) % cap] >= target) dqTail_ = (dqTail_ + cap - 1U) % cap;
        dqIdx_[dqTail_] = limIdx_; dqVal_[dqTail_] = target; dqTail_ = (dqTail_ + 1U) % cap;
        while (dqIdx_[dqHead_] + win <= limIdx_) dqHead_ = (dqHead_ + 1U) % cap;
        const double q = dqVal_[dqHead_];
        const size_t bpos = static_cast<size_t>(limIdx_ % bwin);
        if (boxN_ >= bwin) boxSum_ -= boxBuf_[bpos]; else ++boxN_;
        boxBuf_[bpos] = q; boxSum_ += q;
        ++limIdx_;
        const double boxed = std::min(0.0, boxSum_ / static_cast<double>(bwin));   // missing history counts as 0 dB
        gainDb_ = boxed < gainDb_ ? boxed : gainDb_ + sw.aRel * (boxed - gainDb_);
        lastGainDb_ = gainDb_;

        // 4) END ramps (CUT / DUMP mute the forward loop over 30 ms)
        if (endGain_ < endTarget_) endGain_ = std::min(endTarget_, endGain_ + sw.endStep);
        else if (endGain_ > endTarget_) endGain_ = std::max(endTarget_, endGain_ - sw.endStep);
        const float g = static_cast<float>(std::pow(10.0, gainDb_ / 20.0) * endGain_);
        // Tempo-lock arrival leveler (OUTPUT only, after the feedback tap: the
        // loop itself is never starved). Target: the arrival line (lands on
        // CAP-0.1 dB at the downbeat). The loop's 400 ms level centred 100 ms
        // ahead is read from the tape (same lookahead taps) times the gain the
        // in-loop limiter plans; the required output gain (-30 dB .. +6 dB
        // lift, the lift fading out 0.3-1.3 s after the downbeat so the natural
        // wow movement returns and the output can never exceed the in-loop
        // limited loop) is box-averaged over 200 ms.
        lockFade_ = 1.0;
        if (sw.lockT > 0.0) {
            double req = -30.0;
            if (tSince >= 0.0 && refMs_ > 0.0) {
                // levMs_ = 400 ms window ending levLook ahead = centred A/2 ahead;
                // the 200 ms backward box brings the centre back to "now".
                const double tC = tSince + 0.5 * sw.box / sr_;
                const double tgt = sw.capDb + kLockAimDb + lockLineDb(sw.lockSlope, sw.lockT - tC);
                // in-loop gain those samples will get: the limiter's planned minimum
                const double gPlan = std::min(0.0, dqVal_[dqHead_]);
                const double innerAbs = 10.0 * std::log10(std::max(levMs_ * std::pow(10.0, gPlan / 10.0), 1e-30));
                const double innerDb = innerAbs - 10.0 * std::log10(refMs_);
                const double lift = std::min(kLockBoostDb * std::clamp(1.0 - (tSince - sw.lockT - kLockBoostHoldSec), 0.0, 1.0),
                                             std::max(0.0, kLockLiftCeilDbFS - innerAbs));   // never lift the loop above -12 dBFS RMS
                req = std::clamp(tgt - innerDb, -30.0, lift);
            }
            // 200 ms box average of the required gain (no sliding minimum: the
            // leveler must be able to fill wow dips shorter than its lookahead)
            const size_t lb = static_cast<size_t>(sw.box) + 1U;
            const size_t lp = static_cast<size_t>(levIdx_ % lb);
            if (levN_ >= lb) levSum_ -= levBuf_[lp]; else ++levN_;
            levBuf_[lp] = req; levSum_ += req; ++levIdx_;
            lockFade_ = std::pow(10.0, (levSum_ / static_cast<double>(levN_)) / 20.0);
        }
        return g;
    }

    void meterAdd(double inL, double inR, double loopL, double loopR) {
        mDry_ += 0.5 * (inL * inL + inR * inR);
        mLoop_ += 0.5 * (loopL * loopL + loopR * loopR);
    }
    void meterEnd(int n, bool swellingFlag) {
        const double k = 1.0 / std::max(1, n);
        dryDb_ = 10.0 * std::log10(std::max(mDry_ * k, 1e-24));
        loopDb_ = 10.0 * std::log10(std::max(mLoop_ * k, 1e-24));
        mDry_ = mLoop_ = 0.0; swelling_ = swellingFlag;
    }

    // Loop history (for END DUMP) and the reversed playback.
    void swellHistory(double eL, double eR, double& revL, double& revR) {
        const size_t hsz = hist_.size() / 2U;
        if (!dumpActive_) {
            hist_[2U * histPos_] = static_cast<float>(eL); hist_[2U * histPos_ + 1U] = static_cast<float>(eR);
            histPos_ = (histPos_ + 1U) % hsz;
        } else if (dumpJ_ < dumpW_) {
            const size_t idx = (dumpFrom_ + hsz * 2U - 1U - static_cast<size_t>(dumpJ_)) % hsz;
            const double j = static_cast<double>(dumpJ_), W = static_cast<double>(dumpW_), xf = kEndRampSec * sr_;
            const double env = std::min(1.0, j / xf) * std::min(1.0, (W - j) / xf) * std::pow(10.0, -24.0 * (j / W) / 20.0);
            revL = hist_[2U * idx] * env;
            revR = hist_[2U * idx + 1U] * env;
            ++dumpJ_;
        }
    }

    // Stage voicing: a louder, darker, more theatrical tape echo.
    //  - heads are replayed at unity (x kStageWetLevel), not through the
    //    pre-emphasised record signal, so the first repeat sits about -7 dB
    //    under the dry signal at MIX 0.5;
    //  - MIX law: dry = min(1, 2(1-mix)), wet = min(1, 2 mix); MIX 0.5 keeps
    //    the dry at unity and the wet at full level, MIX 1 is wet only;
    //  - every pass goes through a 60 Hz low-cut and a high shelf (corner at
    //    TONE, top gain falling with DRAMA), so each repeat is darker;
    //  - only the longest active head feeds the loop (one path per repeat, so
    //    every repeat is exactly one pass darker than the one before); the
    //    loop gain is capped at kStageMaxFeedback, so it is contractive
    //    (FREEZE excepted: first active head at unity, tanh-bounded);
    //  - REVERSE crossfades to a reversed-window reader (window = TIME) with
    //    a rising swell envelope; SPREAD widens heads and delays the right
    //    channel reads (Haas) per head.
    void processStage(float* left, float* right, int n, const Controls& c) {
        constexpr double twoPi = 6.283185307179586;
        ensureTape(3.0 * syncedTime(c) * 0.001 * sr_ + 0.02 * sr_);
        const double maxBase = std::floor((static_cast<double>(tapeLen_) - 16.0 - 0.02 * sr_) / 3.0);
        const double base = std::clamp(syncedTime(c) * 0.001 * sr_, 16.0, maxBase);
        const double drama = std::clamp(c.drama, 0.0, 1.0);
        const double spread = std::clamp(c.spread, 0.0, 1.0);
        const double rev = std::clamp(c.reverse, 0.0, 1.0);
        const double width = std::clamp(c.width, 0.0, 1.0);
        const double wowDepth = std::clamp(c.wear, 0.0, 1.0) * 0.0012 * sr_ * (1.0 + 1.2 * drama);
        const double driveK = 1.0 + 1.2 * std::clamp(c.drive, 0.0, 1.0);
        const double fb = std::clamp(c.feedback, 0.0, 1.1);
        const double tone = std::clamp(c.toneHz, 1000.0, 8000.0);
        const double mix = std::clamp(c.mix, 0.0, 1.0);
        const double dryG = std::min(1.0, 2.0 * (1.0 - mix));
        const double wetG = std::min(1.0, 2.0 * mix) * (c.cleanVoice ? kEchoWetLevel : kStageWetLevel);
        const bool wantFreeze = c.freeze || endFreeze_;
        if (wantFreeze && !freezeActive_) { freezeActive_ = true; freezeSamples_ = 0; }
        if (!wantFreeze) freezeActive_ = false;
        const bool frozen = freezeActive_;
        // ---- swell block on the clean/stage loop (unused when swell is off) ----
        const bool swell = swellVoice(c);
        SwellBlock sw{};
        if (swell) setupSwell(c, base, wetG, std::abs(wowDepth), sw);
        const double hpCoeff = std::exp(-twoPi * (frozen ? 25.0 : 60.0) / sr_);
        const double shelfCoeff = 1.0 - std::exp(-twoPi * tone / sr_);
        const double shelfTop = frozen ? 1.0 : 0.66 - 0.20 * drama;   // per-pass top gain
        const double toneCoeff = 1.0 - std::exp(-twoPi * std::min(tone * 1.4, 0.45 * sr_) / sr_);
        // Later heads sit further along the worn tape: extra playback loss.
        const double headLp[3] = {1.0,
                                  1.0 - std::exp(-twoPi * std::min(tone * 1.6, 0.45 * sr_) / sr_),
                                  1.0 - std::exp(-twoPi * std::min(tone * 1.0, 0.45 * sr_) / sr_)};
        const double wowStep = twoPi * 0.5 / sr_;
        const double flutterStep = twoPi * 6.0 / sr_;

        // Head activity follows the classic MODE table.
        const int m = c.mode;
        const bool a1 = m == 1 || m == 4 || m == 7 || m == 8 || m == 9 || m == 10 || m == 11 || m == 12;
        const bool a2 = m == 2 || m == 4 || m == 5 || m == 7 || m == 9 || m == 10 || m == 11 || m == 12;
        const bool a3 = m == 3 || m == 5 || m == 6 || m == 7 || m == 8 || m == 10 || m == 11 || m == 12;
        const double hg[3] = {a1 ? 1.0 : 0.0, a2 ? 0.75 : 0.0, a3 ? 0.55 : 0.0};
        const double pan[3] = {-width, width, -width * (1.0 - spread)};   // SPREAD: L / R / centre
        const double haas[3] = {spread * 0.007 * sr_, spread * 0.013 * sr_, spread * 0.019 * sr_};
        double gl[3], gr[3];
        for (int h = 0; h < 3; ++h) {
            const double p = std::clamp(pan[h], -1.0, 1.0);
            gl[h] = std::sqrt(1.0 - p);   // unity at centre, constant power
            gr[h] = std::sqrt(1.0 + p);
        }
        const uint64_t revLen = static_cast<uint64_t>(base);
        const double revRelease = std::exp(-1.0 / (0.05 * sr_));
        const uint64_t quietNeed = static_cast<uint64_t>(0.12 * sr_);
        const uint64_t xfadeN = std::max<uint64_t>(1, static_cast<uint64_t>(0.01 * sr_));

        for (int i = 0; i < n; ++i) {
            const double inL = left[i];
            const double inR = right[i];
            const double in = 0.5 * (inL + inR);
            const double pressure = 1.0 + drama * 0.12 * std::sin(twoPi * 0.1 * static_cast<double>(sample_) / sr_);
            const double wobble = wowDepth * (0.72 * std::sin(wowPhase_) + 0.28 * std::sin(flutterPhase_));
            wowPhase_ += wowStep;
            flutterPhase_ += flutterStep;
            if (wowPhase_ > twoPi) wowPhase_ -= twoPi;
            if (flutterPhase_ > twoPi) flutterPhase_ -= twoPi;

            double fwdL = 0.0, fwdR = 0.0, fbLast = 0.0, holdRaw = 0.0;
            bool holdSet = false;
            for (int h = 0; h < 3; ++h) {
                if (hg[h] == 0.0) continue;
                const double d = base * static_cast<double>(h + 1) + wobble;
                const double vL = readCubic(d);
                const double vR = spread > 0.0 ? readCubic(d + haas[h]) : vL;
                if (!holdSet) { holdRaw = vL; holdSet = true; }
                double oL = vL, oR = vR;
                if (h > 0) {
                    sHeadL_[h] += headLp[h] * (vL - sHeadL_[h]);
                    sHeadR_[h] += headLp[h] * (vR - sHeadR_[h]);
                    oL = sHeadL_[h];
                    oR = sHeadR_[h];
                }
                fbLast = oL;   // the longest active head (with its own loss) feeds the loop
                fwdL += oL * hg[h] * gl[h];
                fwdR += oR * hg[h] * gr[h];
            }
            double revS_L = 0.0, revS_R = 0.0;
            if (swell) {
                double pw = 0.0;
                if (sw.capActive) {   // audible loop LOOK samples ahead (per-head playback loss ignored)
                    double fL = 0.0, fR = 0.0;
                    for (int h = 0; h < 3; ++h) {
                        if (hg[h] == 0.0) continue;
                        const double d = base * static_cast<double>(h + 1) + wobble - sw.look;
                        const double vL = readCubic(d), vR = spread > 0.0 ? readCubic(d + haas[h]) : vL;
                        fL += vL * hg[h] * gl[h]; fR += vR * hg[h] * gr[h];
                    }
                    pw = sw.mix2 * 0.5 * (fL * fL + fR * fR);
                }
                const double g = swellGain(c, sw, inL, inR, pw);
                fwdL *= g; fwdR *= g; fbLast *= g; holdRaw *= g;
                swellHistory(fwdL, fwdR, revS_L, revS_R);
            }

            double wL = fwdL, wR = fwdR;
            if (rev > 0.0) {
                // Phrase lock: after >= 120 ms of quiet, a new onset restarts
                // the reverse window, so the first reversed repeat is the
                // phrase opening played backwards. The old window fades out
                // over 10 ms (no click).
                const double a = std::abs(in);
                revEnv_ = a > revEnv_ ? a : revEnv_ * revRelease;
                if (revEnv_ < 0.003) { if (revQuiet_ < quietNeed) ++revQuiet_; }
                else if (revEnv_ > 0.01) {   // between the two thresholds: hold
                    if (revQuiet_ >= quietNeed) { revOldPhase_ = revPhase_; revXfade_ = xfadeN; revPhase_ = 0; }
                    revQuiet_ = 0;
                }
                if (revPhase_ >= revLen) revPhase_ = 0;
                double rv = reverseTap(revPhase_, revLen, wobble + wowDepth);
                if (revXfade_ > 0) {
                    const double k = static_cast<double>(revXfade_) / static_cast<double>(xfadeN);
                    if (revOldPhase_ < revLen) rv += k * reverseTap(revOldPhase_, revLen, wobble + wowDepth);
                    ++revOldPhase_;
                    --revXfade_;
                }
                wL = (1.0 - rev) * fwdL + rev * rv;
                wR = (1.0 - rev) * fwdR + rev * rv;
            }
            ++revPhase_;
            if (swell) { wL += revS_L; wR += revS_R; }

            // Playback head tone.
            sToneL_ += toneCoeff * (wL - sToneL_);
            sToneR_ += toneCoeff * (wR - sToneR_);

            // Feedback: normalised head sum -> low-cut -> per-pass high shelf.
            // FREEZE holds the first active head only: a single loop at unity
            // gain sustains, a head average would comb-filter itself away.
            const double fbSig = frozen ? holdRaw : fbLast;
            const double hp = fbSig - sHpX_ + hpCoeff * sHpY_;
            sHpX_ = fbSig;
            sHpY_ = hp;
            sShelfLp_ += shelfCoeff * (hp - sShelfLp_);
            double shelved = sShelfLp_ + shelfTop * (hp - sShelfLp_);
            if (swell && sw.darkCoeff > 0.0f) { sDkS_ += sw.darkCoeff * (shelved - sDkS_); shelved = sDkS_; }
            double riseQ = sw.cleanQ;
            if (swell && sw.riseOn && sw.capActive && refMs_ > 0.0 && detMs_ > 0.0) {
                // soft servo: within CLEAN_TAPER dB under CAP the loop gain tapers
                // from the RISE gain to the hold gain, so the loop parks at CAP
                // (the in-loop limiter above stays as the safety)
                const double lv = 10.0 * std::log10(detMs_ / refMs_);
                const double x = std::clamp((sw.capDb + kCleanServoDb - lv) / kCleanTaperDb, 0.0, 1.0);
                riseQ = kCleanHoldGain * std::pow(sw.cleanQ / kCleanHoldGain, x);
            }
            const double loopGain = frozen ? 1.0 : (swell && sw.riseOn) ? riseQ : std::min(kStageMaxFeedback, fb * pressure);
            const double rec = (frozen ? 0.0 : in) + shelved * loopGain;
            buffer_[writePos_] = static_cast<float>(std::tanh(rec * driveK) / driveK);
            writePos_ = (writePos_ + 1U) % tapeLen_;

            if (!swell) {
                left[i] = static_cast<float>(inL * dryG + sToneL_ * wetG);
                right[i] = static_cast<float>(inR * dryG + sToneR_ * wetG);
            } else {
                left[i] = static_cast<float>(inL * dryG + sToneL_ * wetG * lockFade_);
                right[i] = static_cast<float>(inR * dryG + sToneR_ * wetG * lockFade_);
            }
            meterAdd(inL, inR, left[i] - inL * dryG, right[i] - inR * dryG);
            ++sample_;
        }
        meterEnd(n, swellingNow(c));
    }

    // Reversed window reader with a rising swell envelope.
    double reverseTap(uint64_t phase, uint64_t len, double wobbleOffset) const {
        const double x = static_cast<double>(phase) / static_cast<double>(len);
        const double env = std::pow(x, 1.5) * std::min(1.0, (1.0 - x) / 0.08);
        return readCubic(2.0 * static_cast<double>(phase) + 2.0 + wobbleOffset) * env * 1.9;
    }

    double syncedTime(const Controls& c) const { return syncMs(c); }

    void ensureTape(double needSamples) {
        // 50 ms of hysteresis on the way back to the short tape, so a TIME that hovers at the limit cannot re-lay the tape every sample.
        const double shortCap = static_cast<double>(shortLen_) - (tapeLen_ == longLen_ ? 0.05 * sr_ : 0.0);
        const size_t want = needSamples + 16.0 > shortCap ? longLen_ : shortLen_;
        if (want == tapeLen_) return;
        if (sample_ == 0) { tapeLen_ = want; std::fill(buffer_.begin(), buffer_.end(), 0.0f); writePos_ = 0; return; }   // nothing recorded yet: as before
        // A live change of TIME (a preset, a sync division, the TIME knob) can need the other tape length. The echo that is already on the tape
        // must keep ringing: take the newest min(old, new) samples in time order, put them at the start of the new ring, and continue writing
        // right after them. Every head then reads the same samples at the same distance as one sample earlier (distances beyond what was kept
        // read silence, which is what a tape that has not been recorded that far yet holds). No allocation: scratch_ was sized in prepare().
        const size_t keep = std::min(tapeLen_, want);
        for (size_t k = 0; k < keep; ++k) scratch_[k] = buffer_[(writePos_ + tapeLen_ - keep + k) % tapeLen_];
        std::fill(buffer_.begin(), buffer_.end(), 0.0f);
        std::copy(scratch_.begin(), scratch_.begin() + static_cast<std::ptrdiff_t>(keep), buffer_.begin());
        tapeLen_ = want;
        writePos_ = keep % tapeLen_;
    }

    float readCubic(double delay) const {
        const double pos = static_cast<double>(writePos_) - delay;
        const double size = static_cast<double>(tapeLen_);
        double wrapped = std::fmod(pos, size);
        if (wrapped < 0.0) wrapped += size;
        if (wrapped >= size) wrapped -= size;   // 2026-10-12: -tiny + size rounds to size -> buffer_[tapeLen_] was read one past the end
        const size_t i1 = static_cast<size_t>(wrapped);
        const double f = wrapped - static_cast<double>(i1);
        const size_t im1 = (i1 + tapeLen_ - 1U) % tapeLen_;
        const size_t i2 = (i1 + 1U) % tapeLen_;
        const size_t i3 = (i1 + 2U) % tapeLen_;
        const double a = buffer_[im1], b = buffer_[i1], d = buffer_[i2], e = buffer_[i3];
        return static_cast<float>(b + 0.5 * f * (d - a + f * (2.0 * a - 5.0 * b + 4.0 * d - e + f * (3.0 * (b - d) + e - a))));
    }

    double sr_ = 48000.0;
    std::vector<float> buffer_, scratch_;   // scratch_: the tape being re-laid when its length changes
    size_t shortLen_ = 1, longLen_ = 1, tapeLen_ = 1;
    size_t writePos_ = 0;
    uint64_t sample_ = 0;
    float prevInput_ = 0.0f;
    float hpX_ = 0.0f, hpY_ = 0.0f, lpY_ = 0.0f;
    double wowPhase_ = 0.0, flutterPhase_ = 0.0;
    bool freezeActive_ = false;
    uint64_t freezeSamples_ = 0;
    double sHpX_ = 0.0, sHpY_ = 0.0, sShelfLp_ = 0.0;
    double sToneL_ = 0.0, sToneR_ = 0.0;
    double sHeadL_[3] = {0.0, 0.0, 0.0}, sHeadR_[3] = {0.0, 0.0, 0.0};
    uint64_t revPhase_ = 0, revOldPhase_ = 0, revXfade_ = 0, revQuiet_ = 0;
    double revEnv_ = 0.0;
    // swell state
    std::vector<float> hist_;
    size_t histPos_ = 0;
    float dkY_ = 0.0f;
    double onEnv_ = 0.0;
    uint64_t onQuiet_ = 0, phraseStart_ = 0;
    bool phraseSeen_ = false;
    double accMs_ = 0.0; uint64_t accN_ = 0; double refMs_ = 0.0;
    double detMs_ = 0.0, gainDb_ = 0.0;
    double endGain_ = 1.0, endTarget_ = 1.0;
    bool endFired_ = false, endFreeze_ = false, lastTrig_ = false;
    bool dumpActive_ = false; uint64_t dumpJ_ = 0, dumpW_ = 0; size_t dumpFrom_ = 0;
    double lastCeilDb_ = 0.0, lastGainDb_ = 0.0, lockFade_ = 1.0;
    double wowGlide_ = 0.0, loopMs_ = 0.0;
    double sDkS_ = 0.0;                          // DARKEN state, clean/stage loop
    double mDry_ = 0.0, mLoop_ = 0.0;            // meter accumulators (per block)
    double dryDb_ = -240.0, loopDb_ = -240.0; bool swelling_ = false;
    std::vector<double> levBuf_; double levSum_ = 0.0; size_t levN_ = 0; uint64_t levIdx_ = 0;
    std::vector<uint64_t> dqIdx_; std::vector<double> dqVal_, boxBuf_, detBuf_;
    double detSum_ = 0.0; size_t detPos_ = 0, detN_ = 1;
    double levWinSum_ = 0.0, levMs_ = 0.0;
    size_t limWin_ = 0, boxWin_ = 0, dqHead_ = 0, dqTail_ = 0, boxN_ = 0; double boxSum_ = 0.0; uint64_t limIdx_ = 0;
};


// ---- MOODs: one device, six voicings (2026-09-30) ----
// 1-4 are the original presets (values unchanged -> bit-identical renders);
// 5-6 are the stage voicings. The swell controls (RISE / CAP / DARKEN / END /
// tempo lock) act on moods 1-4 (classic loop); 5-6 are decaying voicings.
static const char* const kMoodNames[7] = {"Echo", "Slapback", "Three Heads", "Dub Spiral", "Worn Loop", "Yesilcam", "Ters"};
// Shared base the original presets were written on top of (kept for their exact values).
inline Controls moodBase() {
    Controls c;
    c.mix = 0.55; c.feedback = 0.68; c.timeMs = 520; c.toneHz = 2200; c.wear = 0.8; c.drive = 0.4;
    return c;
}
// MOOD 0 "ECHO": the device's default, a plain tempo-synced echo that decays.
// TIME 1/8 at the host tempo (120 bpm fallback = 250 ms), single head, clean
// loop (unity record, per-pass high shelf at TONE), FEEDBACK 0.35 (loop gain
// 0.35 < the 0.90 cap, decaying), TONE 4.5 kHz, light wow, MIX 0.3.
inline Controls echoControls() {
    Controls c;
    c.sync = 1; c.bpm = 120.0; c.timeMs = 250.0;
    c.cleanVoice = true; c.mode = 1; c.feedback = 0.35; c.toneHz = 4500.0; c.drive = 0.25; c.wear = 0.25; c.mix = 0.3; c.width = 0.3;
    c.mood = 0;
    return c;
}
inline Controls moodControls(int mood) {
    if (mood == 0) return echoControls();
    Controls c = moodBase();
    if (mood == 1) { c.timeMs = 90; c.mode = 1; c.feedback = 0.22; c.toneHz = 5200; c.mix = 0.38; }
    else if (mood == 2) { c.timeMs = 280; c.mode = 12; c.feedback = 0.58; c.toneHz = 3800; c.width = 0.9; c.mix = 0.44; }
    else if (mood == 3) { c.timeMs = 420; c.mode = 12; c.feedback = 0.88; c.toneHz = 2600; c.drive = 0.45; c.mix = 0.48; }
    else if (mood == 4) { c.timeMs = 360; c.mode = 12; c.feedback = 0.82; c.toneHz = 2100; c.drive = 0.5; c.wear = 0.8; c.mix = 0.46; }
    else if (mood == 5) { c.timeMs = 420; c.mode = 1; c.feedback = 0.50; c.toneHz = 3000; c.drive = 0.35; c.wear = 0.85; c.drama = 1.0; c.width = 0.4; c.mix = 0.5; }
    else if (mood == 6) { c.timeMs = 450; c.mode = 1; c.feedback = 0.48; c.toneHz = 3400; c.drive = 0.3; c.wear = 0.5; c.drama = 0.4; c.reverse = 1.0; c.width = 0.3; c.mix = 0.5; }
    c.mood = mood;
    return c;
}
// The plugin's initial state.
inline Controls defaultControls() { return moodControls(0); }

}
