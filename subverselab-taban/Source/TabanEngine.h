#pragma once

#include <algorithm>
#include <cmath>

/*  TABAN engine: a floor leveller, the mirror image of a limiter. Plain C++, no JUCE.

    A peak detector (instant attack, 20 ms hold, 60 ms release, stereo linked) measures the level. Where the
    level is under FLOOR, the signal is lifted to FLOOR; where it is at or above FLOOR the gain is exactly 1.
    The lift is worked out from the detector of the current sample, and the detector never reads below |x|,
    so |out| <= max(|in|, floor) on every sample: the lift can never push a sudden loud sound over the floor.

    The hold is longer than half a period of 25 Hz, so a steady tone sees a constant gain (no waveshaping).
    The lift is capped at kMaxLiftDb, and it fades out between kGateTopDb and kGateBottomDb so a fading tail,
    room tone or dither is not dragged up to the floor: silence stays silence.

    FLOOR and OUTPUT are smoothed per sample; IN crossfades. Zero latency. Settled with FLOOR at OFF and
    OUTPUT at 0 dB, or with IN off, the output is the input bit for bit.
*/
namespace svl {

class TabanEngine {
public:
    static constexpr float kOffDb        = -60.0f;  // the bottom of the fader: leveller off
    static constexpr float kMaxLiftDb    = 24.0f;   // most the leveller will ever lift
    static constexpr float kGateTopDb    = -80.0f;  // full lift allowed above this level
    static constexpr float kGateBottomDb = -96.0f;  // no lift below this level
    static constexpr float kHoldMs       = 20.0f;
    static constexpr float kReleaseMs    = 60.0f;
    static constexpr float kParamMs      = 20.0f;   // FLOOR and OUTPUT smoothing
    static constexpr float kBypassMs     = 10.0f;   // IN crossfade

    struct Parameters {
        float floorDb = kOffDb;  // -60 (off) .. 0 dBFS
        float outputDb = 0.0f;   // -12 .. +12 dB
        bool enabled = true;
    };

    void prepare (double sampleRate, int /*maxBlockSize*/)
    {
        m_sampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
        const float fs = (float) m_sampleRate;
        m_holdSamples = (int) std::lround (kHoldMs * 0.001f * fs);
        m_releaseCoef = std::exp (-1.0f / (kReleaseMs * 0.001f * fs));
        m_paramCoef   = std::exp (-1.0f / (kParamMs * 0.001f * fs));
        m_bypassStep  = 1.0f / std::max (1.0f, kBypassMs * 0.001f * fs);
        reset();
    }

    /** Clears the detector; the next block starts with its parameters already in place (no fade-in). */
    void reset()
    {
        m_env = 0.0f;
        m_hold = 0;
        m_primed = false;
        m_liftDb = 0.0f;
        m_envDb = -200.0f;
        m_outPeak = 0.0f;
    }

    int getLatencySamples() const { return 0; }

    /** Largest lift applied during the last block, dB (0 when nothing was lifted). */
    float getLiftDb() const { return m_liftDb; }
    /** Detector level at the end of the last block, dBFS. */
    float getDetectorDb() const { return m_envDb; }
    /** Sample peak of the output during the last block (linear). */
    float getOutputPeak() const { return m_outPeak; }

    void process (const float* const* inputs, float* const* outputs, int numChannels, int numSamples, const Parameters& p)
    {
        if (numChannels <= 0 || numSamples <= 0)
            return;

        const float floorTarget = p.floorDb <= kOffDb + 0.05f ? -200.0f : std::min (p.floorDb, 0.0f);
        const float outTarget   = std::pow (10.0f, std::clamp (p.outputDb, -12.0f, 12.0f) * 0.05f);
        const float mixTarget   = p.enabled ? 1.0f : 0.0f;
        if (! m_primed)
        {
            m_floorDb = floorTarget;
            m_outGain = outTarget;
            m_mix = mixTarget;
            m_primed = true;
        }

        const int nIn = std::min (numChannels, 2);
        const float* inL = inputs[0];
        const float* inR = nIn > 1 ? inputs[1] : inputs[0];
        float* outL = outputs[0];
        float* outR = numChannels > 1 ? outputs[1] : nullptr;

        float liftMax = 0.0f, outPeak = 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            const float xL = inL[i], xR = inR[i];

            // Parameter smoothing: one-pole, snapped once close so a settled state is exact.
            m_floorDb = smoothTo (m_floorDb, floorTarget, m_paramCoef, 1.0e-3f);
            m_outGain = smoothTo (m_outGain, outTarget, m_paramCoef, 1.0e-6f);
            if (m_mix < mixTarget || m_mix > mixTarget)
                m_mix = mixTarget > m_mix ? std::min (mixTarget, m_mix + m_bypassStep) : std::max (mixTarget, m_mix - m_bypassStep);

            // Detector: stereo linked, instant attack, hold, then release. NaN/Inf never reach it.
            float a = std::max (std::abs (xL), std::abs (xR));
            if (! (a < 1.0e6f)) a = 0.0f;
            if (a >= m_env)       { m_env = a; m_hold = m_holdSamples; }
            else if (m_hold > 0)  { --m_hold; }
            else                  { m_env = std::max (a, m_env * m_releaseCoef); if (m_env < 1.0e-20f) m_env = 0.0f; }

            // Lift: up to the floor, capped, faded out near silence.
            float gain = 1.0f;
            if (m_floorDb > -150.0f && m_env > 0.0f)
            {
                const float envDb = 20.0f * std::log10 (m_env);
                if (envDb < m_floorDb && envDb > kGateBottomDb)
                {
                    // Linear, not via dB: floor / env times |x| <= floor survives float rounding.
                    const float allowDb = kMaxLiftDb * std::clamp ((envDb - kGateBottomDb) / (kGateTopDb - kGateBottomDb), 0.0f, 1.0f);
                    gain = std::min (std::pow (10.0f, m_floorDb * 0.05f) / m_env, std::pow (10.0f, allowDb * 0.05f));
                    if (gain > 1.0f)
                        liftMax = std::max (liftMax, std::min (m_floorDb - envDb, allowDb));
                    else
                        gain = 1.0f;
                }
            }

            const float wet = gain * m_outGain;
            float yL, yR;
            if (m_mix >= 1.0f)      { yL = xL * wet; yR = xR * wet; }
            else if (m_mix <= 0.0f) { yL = xL; yR = xR; }
            else
            {
                const float k = 1.0f + m_mix * (wet - 1.0f);
                yL = xL * k; yR = xR * k;
            }

            outL[i] = yL;
            if (outR != nullptr) outR[i] = nIn > 1 ? yR : yL;
            outPeak = std::max (outPeak, std::max (std::abs (yL), std::abs (yR)));
        }

        // Channels past the second are passed through untouched.
        for (int ch = 2; ch < numChannels; ++ch)
            if (inputs[ch] != outputs[ch])
                std::copy (inputs[ch], inputs[ch] + numSamples, outputs[ch]);

        m_liftDb = liftMax;
        m_envDb = m_env > 0.0f ? 20.0f * std::log10 (m_env) : -200.0f;
        m_outPeak = outPeak;
    }

private:
    static float smoothTo (float v, float target, float coef, float snap)
    {
        if (! (v < target || v > target)) return v;   // already there (also keeps -Wfloat-equal quiet)
        v = target + (v - target) * coef;
        return std::abs (v - target) < snap ? target : v;
    }

    double m_sampleRate = 44100.0;
    int m_holdSamples = 0, m_hold = 0;
    float m_releaseCoef = 0.0f, m_paramCoef = 0.0f, m_bypassStep = 1.0f;
    float m_env = 0.0f;
    float m_floorDb = -200.0f, m_outGain = 1.0f, m_mix = 1.0f;
    bool m_primed = false;
    float m_liftDb = 0.0f, m_envDb = -200.0f, m_outPeak = 0.0f;
};

} // namespace svl
