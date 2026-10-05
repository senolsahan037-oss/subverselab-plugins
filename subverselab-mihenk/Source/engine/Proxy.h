#pragma once

#include <JuceHeader.h>
#include "Settings.h"
#include "../dsp/Loudness.h"

/*  What the plug-in keeps of the audio LEARN heard, small enough to live in the DAW project (about 20 KB per minute):
    for every 10 ms block of the signal that enters the drive, its K-weighted energy and its true peak. The audio itself
    is dropped after the analysis, so a reopened project cannot re-run the limiter on it; this is what is left to solve
    the drive again when the target or a tone control moves.

    It is a model, not the render: the limiter is simulated per block (instant attack, the release as set), the loudness is
    the BS.1770 gated integral of the energies, and the model is calibrated on the real render it was made with (the
    loudness the real limiter gave at the baseline drive). A tone change is estimated from the long-term spectrum of the
    source. The answer is close, not exact: LEARN again for the exact solve.
*/
namespace mihenk
{
struct LoudnessProxy
{
    bool valid = false;
    double sampleRate = 0.0;
    int hop = 0;                            // samples per block: a tenth of a 100 ms loudness segment
    std::vector<float> energyDb, peakDb;    // per block: K-weighted energy (10 log10 of the sum of squares) and true peak (dBFS)
    static constexpr int numBands = 19;     // half-octave bands, 22 Hz upwards
    std::vector<float> shareDb;             // per 100 ms segment and band: the band's share of the segment's K-weighted energy (dB, <= 0)
    double baselineDriveDb = 0.0, baselineLufs = kNegInf;   // what the real render gave
    double ceilingDb = -1.0, releaseMs = 150.0;
    bool limiterOn = true;
    double recovery = 0.0;                  // fitted on one real render at a higher drive: how far the real limiter's gain recovers between peaks inside a block

    // derived from the dB vectors by prepare()
    std::vector<double> energyLin, peakLin;
    void prepare();
    /** Edges of the bands (numBands + 1 values, Hz). */
    static double bandEdgeHz (int i) { return 22.0 * std::pow (2.0, 0.5 * i); }
};

/** A level change before the drive: per 100 ms segment (power factor, from an EQ change weighted by that segment's spectrum) and one trim. */
struct ToneShift
{
    std::vector<double> segmentPower;       // empty = none: the energy factor
    std::vector<double> segmentPeak;        // the peak (amplitude) factor: the bands add coherently at a peak, so it moves less than the energy
    double trimDb = 0.0;
};

/** The limiter as it is set now (the proxy itself holds the one its baseline render used). */
struct ProxyLimiter
{
    bool on = true;
    double ceilingDb = -1.0, releaseMs = 150.0;
};

struct ProxyLevel
{
    double lufs = kNegInf, grMaxDb = 0.0, grMeanDb = 0.0;
};

/** The proxy of the signal entering the drive (`truePeakEnv`: its per-sample true-peak envelope, may be empty with no limiter). */
LoudnessProxy makeLoudnessProxy (const juce::AudioBuffer<float>& preLimiter, const std::vector<float>& truePeakEnv, double sampleRate);

/** Modelled loudness and limiting at this drive (dB), calibrated on the baseline. `shift` = a level change before the drive (a tone change). */
ProxyLevel proxyLevel (const LoudnessProxy&, double driveDb, const ToneShift& shift = {}, const ProxyLimiter* limiterNow = nullptr);

/** Level change of the signal entering the drive between two settings: the trim, and the EQ response (the change between the two
    settings) weighted by each segment's own spectrum. Compressor and stereo changes are not estimated. */
ToneShift estimateToneShift (const LoudnessProxy&, const MasterSettings& base, double autoTrimBaseDb, const MasterSettings& now, double autoTrimNowDb);

/** Fits `recovery` so the model gives `probeLufs` (a real render) at `probeDriveDb`. Call once the baseline fields are set. */
void fitProxyRecovery (LoudnessProxy&, double probeDriveDb, double probeLufs);

struct ProxySolve
{
    bool ok = false;
    double driveDb = 0.0, lufs = kNegInf, targetLufs = kNegInf;
    juce::String message;
};

/** The drive that holds s.targetLufs (TARGET LOCK) on the modelled signal, with the same limits as MasterChain::matchTarget
    (the drive cap, KEEP INPUT, the finished-master guard). */
ProxySolve solveDriveFromProxy (const LoudnessProxy&, const MasterSettings& s, const ToneShift& shift, const Measurement& source);
} // namespace mihenk
