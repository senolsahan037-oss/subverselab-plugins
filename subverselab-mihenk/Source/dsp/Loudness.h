#pragma once

#include <JuceHeader.h>
#include <vector>
#include <limits>

/*  Loudness and level measurement.

    - K-weighting, 400 ms momentary blocks with 75 % overlap, absolute gate
      -70 LUFS and relative gate -10 LU: ITU-R BS.1770-4.
    - Short-term loudness: 3 s window, evaluated every 100 ms (EBU Tech 3341).
    - Loudness range: short-term values, absolute gate -70 LUFS, relative gate
      -20 LU, 10th to 95th percentile (EBU Tech 3342).
    - True peak: 4x oversampling with a windowed-sinc interpolator
      (BS.1770-4 Annex 2 method with a longer filter than the Annex example:
      64 taps per phase, Kaiser beta 9, flat to about 0.45 fs; phase 0 is the
      original sample, so the true peak is never below the sample peak).
    - Channel weights: L and R 1.0. A mono file is one channel with weight 1.0,
      as BS.1770 defines it (it reads 3 dB lower than the same signal as
      dual-mono stereo).
*/
namespace mihenk
{
constexpr double kNegInf = -std::numeric_limits<double>::infinity();

inline double energyToLufs (double meanSquare) { return meanSquare > 0.0 ? -0.691 + 10.0 * std::log10 (meanSquare) : kNegInf; }

struct LoudnessResult
{
    double integrated = kNegInf;
    double momentaryMax = kNegInf;
    double shortTermMax = kNegInf;
    double lra = 0.0;
    std::vector<float> shortTerm;   // LUFS every 100 ms (entry i covers [i*0.1 s, i*0.1 s + 3 s))
    std::vector<float> momentary;   // LUFS every 100 ms
};

/** Full BS.1770 / Tech 3342 loudness measurement. */
LoudnessResult measureLoudness (const juce::AudioBuffer<float>& buffer, double sampleRate);

/** Integrated loudness only (the fast path used by the target solver). */
double integratedLoudness (const juce::AudioBuffer<float>& buffer, double sampleRate);

/** The 100 ms segment length the measurement works in, samples. */
int loudnessSegmentLength (double sampleRate);
/** K-weighted energy (sum of squares over the samples and the channels) of consecutive blocks of blockLen samples. */
std::vector<double> kWeightedBlockEnergies (const juce::AudioBuffer<float>& buffer, double sampleRate, int blockLen);
/** Integrated loudness (BS.1770 gating) from 100 ms segment energies of the kind kWeightedBlockEnergies gives with blockLen = loudnessSegmentLength. */
double integratedFromSegments (const std::vector<double>& segmentEnergies, int segmentLength);

/** Oversampling true-peak detector (4x for the meter, 8x inside the limiter). */
class TruePeakDetector
{
public:
    static constexpr int taps = 64;   // taps per phase (Kaiser beta 9, flat to about 0.45 fs)
    explicit TruePeakDetector (int factor = 4);

    /** out[i] = max over p = 0..factor-1 of |y(i + p/factor)|: the true-peak envelope of one channel. */
    void envelope (const float* x, int n, float* out) const;

    /** Largest true-peak value (linear) of one channel. */
    float maxPeak (const float* x, int n) const;

    int getFactor() const { return factor; }

private:
    int factor;
    std::vector<float> coeff;   // factor x taps
};

/** True-peak envelope of a buffer, the maximum over all channels, one value per sample. */
std::vector<float> truePeakEnvelope (const juce::AudioBuffer<float>& buffer, int factor = 4);

/** Largest true peak over all channels, linear. */
float truePeakMax (const juce::AudioBuffer<float>& buffer, int factor = 4);

struct Measurement
{
    bool valid = false;
    double sampleRate = 0.0;
    int channels = 0;
    double seconds = 0.0;

    double integrated = kNegInf, momentaryMax = kNegInf, shortTermMax = kNegInf, lra = 0.0;
    double truePeak = kNegInf;      // dBTP
    double samplePeak = kNegInf;    // dBFS
    double rms = kNegInf;           // dBFS, sine peak 0 dBFS reads -3.01 dB
    double crest = 0.0;             // sample peak minus RMS, dB
    double correlation = 1.0;       // Pearson, whole file; 1 for mono
    long long truePeakAt = -1;      // sample index of the highest true peak (4x meter)
    long long samplePeakAt = -1;    // sample index of the highest sample peak

    std::vector<float> shortTermHistory;
    std::vector<float> spectrumDb;  // on spectrumGrid(), 1/6-octave band power
};

Measurement analyse (const juce::AudioBuffer<float>& buffer, double sampleRate, bool withSpectrum = true);

/** "-14.0" style text; "-inf" for silence. */
juce::String fmt (double value, int decimals = 1);
} // namespace mihenk
