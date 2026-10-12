#include "Analysis.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace
{
    // Two analyses, two windows, because tempo and key want opposite things
    // from the same transform.
    //
    // Onsets need time resolution: a beat is an event, and a long window
    // smears it. 4096 samples with a half-window hop gives about 21 frames a
    // second, which is fine enough to place a hit.
    //
    // Pitch needs frequency resolution, and needs it worst exactly where the
    // short window is weakest. A semitone at 65 Hz spans 3.8 Hz; a 4096-sample
    // window has 10.8 Hz bins, so a bass note and its neighbour a semitone away
    // land in the same bin and the key comes out shifted. 16384 samples give
    // 2.7 Hz bins, which resolves a semitone down to the bottom of the range
    // that matters. Measured: with the short window the estimator answered
    // C major, B minor and B minor on the bass, harmony and vocal stems of one
    // track; with the long window, B minor on all three — which is what
    // librosa's constant-Q chroma says about the same audio.
    constexpr int   kOnsetOrder = 12;              // 4096 samples
    constexpr int   kOnsetSize  = 1 << kOnsetOrder;
    constexpr int   kOnsetHop   = kOnsetSize / 2;

    constexpr int   kPitchOrder = 14;              // 16384 samples
    constexpr int   kPitchSize  = 1 << kPitchOrder;
    constexpr int   kPitchHop   = kPitchSize / 4;

    constexpr int   kLowestMidi  = 36;             // C2, about 65 Hz
    constexpr int   kHighestMidi = 84;             // C6, about 1047 Hz
    constexpr double kMinBpm    = 60.0;
    constexpr double kMaxBpm    = 200.0;

    const char* kNoteNames[12] = { "C", "C#", "D", "D#", "E", "F",
                                   "F#", "G", "G#", "A", "A#", "B" };

    // Krumhansl-Schmuckler probe-tone ratings, identical to the web tool's.
    const double kMajorProfile[12] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09,
                                       2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
    const double kMinorProfile[12] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53,
                                       2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };

    juce::String camelotFor (const juce::String& key, const juce::String& scale)
    {
        static const std::pair<const char*, const char*> major[12] = {
            { "C", "8B" }, { "C#", "3B" }, { "D", "10B" }, { "D#", "5B" },
            { "E", "12B" }, { "F", "7B" }, { "F#", "2B" }, { "G", "9B" },
            { "G#", "4B" }, { "A", "11B" }, { "A#", "6B" }, { "B", "1B" } };
        static const std::pair<const char*, const char*> minor[12] = {
            { "C", "5A" }, { "C#", "12A" }, { "D", "7A" }, { "D#", "2A" },
            { "E", "9A" }, { "F", "4A" }, { "F#", "11A" }, { "G", "6A" },
            { "G#", "1A" }, { "A", "8A" }, { "A#", "3A" }, { "B", "10A" } };

        const auto* table = (scale == "min" ? minor : major);
        for (int i = 0; i < 12; ++i)
            if (key == table[i].first)
                return table[i].second;
        return "N/A";
    }

    double correlation (const double* a, const double* b, int n)
    {
        const double meanA = std::accumulate (a, a + n, 0.0) / n;
        const double meanB = std::accumulate (b, b + n, 0.0) / n;

        double covariance = 0.0, varianceA = 0.0, varianceB = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double da = a[i] - meanA, db = b[i] - meanB;
            covariance += da * db;
            varianceA  += da * da;
            varianceB  += db * db;
        }
        // A flat chroma has no variance and correlates with nothing. Returning
        // zero rather than dividing by it is what keeps a silent or purely
        // percussive file from being handed a confident key.
        if (varianceA < 1e-12 || varianceB < 1e-12)
            return 0.0;
        return covariance / std::sqrt (varianceA * varianceB);
    }
}

AudioAnalysis AudioAnalysis::analyse (const juce::AudioBuffer<float>& audio, double sampleRate)
{
    AudioAnalysis result;

    const int totalSamples = audio.getNumSamples();
    if (totalSamples < kPitchSize * 2 || sampleRate <= 0.0)
        return result;                       // too short to measure anything

    // Mono sum. Key and tempo are properties of the arrangement, not of the
    // stereo image, and summing halves the work.
    std::vector<float> mono ((size_t) totalSamples, 0.0f);
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
    {
        const float* data = audio.getReadPointer (channel);
        for (int i = 0; i < totalSamples; ++i)
            mono[(size_t) i] += data[i];
    }
    const float monoScale = 1.0f / (float) juce::jmax (1, audio.getNumChannels());
    for (auto& sample : mono)
        sample *= monoScale;

    // ── Pass 1: onsets, on the short window ──────────────────────────────
    {
        juce::dsp::FFT fft (kOnsetOrder);
        juce::dsp::WindowingFunction<float> window ((size_t) kOnsetSize,
                                                   juce::dsp::WindowingFunction<float>::hann);

        const int frameCount = 1 + (totalSamples - kOnsetSize) / kOnsetHop;
        if (frameCount >= 8)
        {
            std::vector<float> frame ((size_t) kOnsetSize * 2, 0.0f);
            std::vector<float> previous ((size_t) kOnsetSize / 2, 0.0f);
            std::vector<double> onsetEnvelope ((size_t) frameCount, 0.0);

            for (int f = 0; f < frameCount; ++f)
            {
                std::fill (frame.begin(), frame.end(), 0.0f);
                std::copy (mono.begin() + (size_t) f * kOnsetHop,
                           mono.begin() + (size_t) f * kOnsetHop + kOnsetSize,
                           frame.begin());
                window.multiplyWithWindowingTable (frame.data(), (size_t) kOnsetSize);
                fft.performFrequencyOnlyForwardTransform (frame.data());

                double flux = 0.0;
                for (int bin = 1; bin < kOnsetSize / 2; ++bin)
                {
                    const float magnitude = frame[(size_t) bin];
                    // Spectral flux: only energy that *arrived* counts as an
                    // onset, so the difference is half-wave rectified. Decay
                    // is not a beat.
                    const float rise = magnitude - previous[(size_t) bin];
                    if (rise > 0.0f)
                        flux += rise;
                    previous[(size_t) bin] = magnitude;
                }
                onsetEnvelope[(size_t) f] = flux;
            }

            // Autocorrelate. The lag that best explains the spacing of the
            // onsets is the beat period.
            const double mean = std::accumulate (onsetEnvelope.begin(), onsetEnvelope.end(), 0.0)
                                / (double) frameCount;
            for (auto& value : onsetEnvelope)
                value -= mean;               // remove DC or lag 0 wins outright

            const double framesPerSecond = sampleRate / kOnsetHop;
            const int minLag = juce::jmax (1, (int) std::floor (framesPerSecond * 60.0 / kMaxBpm));
            const int maxLag = juce::jmin (frameCount / 2, (int) std::ceil (framesPerSecond * 60.0 / kMinBpm));

            double bestScore = -1.0;
            int bestLag = 0;
            for (int lag = minLag; lag <= maxLag; ++lag)
            {
                double sum = 0.0;
                for (int i = 0; i + lag < frameCount; ++i)
                    sum += onsetEnvelope[(size_t) i] * onsetEnvelope[(size_t) (i + lag)];
                sum /= (double) (frameCount - lag);   // longer lags have fewer terms
                if (sum > bestScore)
                {
                    bestScore = sum;
                    bestLag = lag;
                }
            }

            if (bestLag > 0)
            {
                double raw = 60.0 * framesPerSecond / bestLag;

                // Fold into the band most material sits in, and keep the other
                // reading rather than hiding it. A tracker reporting 75 for a
                // 150 BPM track is not wrong, it is ambiguous, and the reader
                // can settle it by ear.
                double alternate = raw < 100.0 ? raw * 2.0 : raw / 2.0;
                if (raw < 70.0)       { alternate = raw; raw *= 2.0; }
                else if (raw > 180.0) { alternate = raw; raw /= 2.0; }

                result.bpm = raw;
                result.bpmAlternate = alternate;
            }
        }
    }

    // ── Pass 2: chroma, on the long window ───────────────────────────────
    std::vector<double> chroma (12, 0.0);
    {
        juce::dsp::FFT fft (kPitchOrder);
        juce::dsp::WindowingFunction<float> window ((size_t) kPitchSize,
                                                   juce::dsp::WindowingFunction<float>::hann);

        // A semitone filterbank, not a bin-to-pitch-class mapping. Each
        // semitone owns the bins within fifty cents of its centre, so every
        // pitch class is measured over its own bandwidth rather than being
        // assigned whichever bin happened to round to it — and the top octave
        // no longer outvotes the bottom four simply for having more bins.
        struct Band { int firstBin, lastBin, pitchClass; };
        std::vector<Band> bands;
        for (int midi = kLowestMidi; midi <= kHighestMidi; ++midi)
        {
            const double centre = 440.0 * std::pow (2.0, (midi - 69) / 12.0);
            const double low  = centre * std::pow (2.0, -0.5 / 12.0);
            const double high = centre * std::pow (2.0,  0.5 / 12.0);
            const int firstBin = (int) std::ceil  (low  * kPitchSize / sampleRate);
            const int lastBin  = (int) std::floor (high * kPitchSize / sampleRate);
            if (lastBin >= firstBin && lastBin < kPitchSize / 2)
                bands.push_back ({ firstBin, lastBin, ((midi % 12) + 12) % 12 });
        }

        const int frameCount = 1 + (totalSamples - kPitchSize) / kPitchHop;
        std::vector<float> frame ((size_t) kPitchSize * 2, 0.0f);
        std::vector<double> frameChroma (12, 0.0);

        for (int f = 0; f < frameCount; ++f)
        {
            std::fill (frame.begin(), frame.end(), 0.0f);
            std::copy (mono.begin() + (size_t) f * kPitchHop,
                       mono.begin() + (size_t) f * kPitchHop + kPitchSize,
                       frame.begin());
            window.multiplyWithWindowingTable (frame.data(), (size_t) kPitchSize);
            fft.performFrequencyOnlyForwardTransform (frame.data());

            std::fill (frameChroma.begin(), frameChroma.end(), 0.0);
            for (const auto& band : bands)
            {
                double energy = 0.0;
                for (int bin = band.firstBin; bin <= band.lastBin; ++bin)
                    energy += frame[(size_t) bin];
                frameChroma[(size_t) band.pitchClass] += energy;
            }

            // Normalise each frame before adding it in, so a loud chorus does
            // not outvote a quiet verse in a piece that stays in one key.
            const double total = std::accumulate (frameChroma.begin(), frameChroma.end(), 0.0);
            if (total > 1e-12)
                for (int c = 0; c < 12; ++c)
                    chroma[(size_t) c] += frameChroma[(size_t) c] / total;
        }
    }

    // ── Key ──────────────────────────────────────────────────────────────
    double scores[24];
    for (int rotation = 0; rotation < 12; ++rotation)
    {
        double rotatedMajor[12], rotatedMinor[12];
        for (int i = 0; i < 12; ++i)
        {
            rotatedMajor[i] = kMajorProfile[(i - rotation + 12) % 12];
            rotatedMinor[i] = kMinorProfile[(i - rotation + 12) % 12];
        }
        scores[rotation]      = correlation (chroma.data(), rotatedMajor, 12);
        scores[rotation + 12] = correlation (chroma.data(), rotatedMinor, 12);
    }

    const int best = (int) std::distance (scores, std::max_element (scores, scores + 24));

    double sorted[24];
    std::copy (scores, scores + 24, sorted);
    std::sort (sorted, sorted + 24, std::greater<double>());

    // How far clear the winner is of the runner-up, on the same scale the web
    // tool uses, so a confidence figure means one thing across both products.
    result.keyConfidence = (float) juce::jlimit (0.0, 1.0, (sorted[0] - sorted[1]) / 0.15);
    result.key = kNoteNames[best % 12];
    result.scale = best < 12 ? "maj" : "min";
    result.camelot = camelotFor (result.key, result.scale);
    result.valid = true;

    return result;
}
