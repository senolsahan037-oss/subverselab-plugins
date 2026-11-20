#include <limits>
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
    // 1024 samples, hop 256: 172 onset frames a second. (Was 4096 / 2048 = 21.5 frames a second: a beat period could only be a
    // whole number of 46 ms frames, so tempo came out as one of ~17 values, 98 read 99.4 and 120 read 117.4.)
    constexpr int   kOnsetOrder = 10;
    constexpr int   kOnsetSize  = 1 << kOnsetOrder;
    constexpr int   kOnsetHop   = kOnsetSize / 4;

    constexpr int   kPitchOrder = 14;              // 16384 samples
    constexpr int   kPitchSize  = 1 << kPitchOrder;
    constexpr int   kPitchHop   = kPitchSize / 4;

    constexpr int   kLowestMidi  = 36;             // C2, about 65 Hz
    constexpr int   kHighestMidi = 84;             // C6, about 1047 Hz
    constexpr double kMinBpm    = 60.0;
    constexpr double kMaxBpm    = 200.0;

    const char* kNoteNames[12] = { "C", "C#", "D", "D#", "E", "F",
                                   "F#", "G", "G#", "A", "A#", "B" };

    // KEY PROFILES (20 Nov 2026, owner: "kullanıcının bilgisayarında ton yanlış çalışmamalı"): the Albrecht-Shanahan corpus profile
    // plus 1.44 x the EDMA profile (Faraldo et al., electronic dance music), summed. Measured with the harmonic-only chroma below on
    // 199 pack mixes + 24 labelled major/minor loops (01_AI_Tools/elek2/work/keytest): 70 % / 100 % exact, confident readings 85 %
    // (was Krumhansl-Schmuckler on the full chroma: 61 % / 67 %, confident 80 %).
    const double kMajorProfile[12] = { 0.238 + 1.44 * 0.16519551, 0.006 + 1.44 * 0.04749026, 0.111 + 1.44 * 0.08293076,
                                       0.006 + 1.44 * 0.06687112, 0.137 + 1.44 * 0.09994645, 0.094 + 1.44 * 0.09274123,
                                       0.016 + 1.44 * 0.05294487, 0.214 + 1.44 * 0.13159476, 0.009 + 1.44 * 0.05218986,
                                       0.080 + 1.44 * 0.07443653, 0.008 + 1.44 * 0.06940723, 0.081 + 1.44 * 0.0642515 };
    const double kMinorProfile[12] = { 0.220 + 1.44 * 0.17235348, 0.006 + 1.44 * 0.04,       0.104 + 1.44 * 0.0761009,
                                       0.123 + 1.44 * 0.12112053, 0.019 + 1.44 * 0.05751457, 0.103 + 1.44 * 0.08571564,
                                       0.012 + 1.44 * 0.0539885,  0.214 + 1.44 * 0.13529572, 0.062 + 1.44 * 0.07455507,
                                       0.022 + 1.44 * 0.05090036, 0.061 + 1.44 * 0.06784019, 0.052 + 1.44 * 0.06223468 };
    constexpr int kHpssSpan = 17;     // median length (frames along time, bins along frequency) of the harmonic/percussive split

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

            // The score of every lag from one below the range to one above it, so the best one has both neighbours.
            auto scoreAt = [&] (int lag)
            {
                if (lag < 1 || lag >= frameCount) return 0.0;
                double sum = 0.0;
                for (int i = 0; i + lag < frameCount; ++i)
                    sum += onsetEnvelope[(size_t) i] * onsetEnvelope[(size_t) (i + lag)];
                return sum / (double) (frameCount - lag);   // longer lags have fewer terms
            };
            double bestScore = -std::numeric_limits<double>::infinity();     // (was -1.0: an unnormalised score can stay under it, and "0 BPM" came out as a valid reading)
            int bestLag = 0;
            // A lag is a beat when its multiples line up too: two beats and the bar. A dotted or shuffled figure repeats at 3/4 of
            // a beat and scored highest on its own (a 98 BPM break read 130.7); its multiples (1.5 and 3 beats) do not line up.
            // The weight prefers tempi around 120 BPM, one octave wide: of 72 and 144 it picks the one nearer the middle.
            auto support = [&] (int lag)
            {
                const double bpm = 60.0 * framesPerSecond / (double) lag;
                const double octaves = std::log2 (bpm / 120.0);
                const double weight = std::exp (-0.5 * octaves * octaves);
                return (scoreAt (lag) + 0.5 * scoreAt (2 * lag) + 0.25 * scoreAt (4 * lag)) * weight;
            };
            for (int lag = minLag; lag <= maxLag; ++lag)
            {
                const double sum = support (lag);
                if (sum > bestScore)
                {
                    bestScore = sum;
                    bestLag = lag;
                }
            }

            if (bestLag > 0)
            {
                // The hop is 2048 samples (21.5 frames a second), so whole lags can only say 99.4, 107.7, 117.4, 129.2, 143.6 ...
                // BPM (a 98 BPM loop read 99, a 120 BPM track 117). A parabola through the best lag and its neighbours places
                // the peak between frames.
                double lagExact = (double) bestLag;
                const double a = scoreAt (bestLag - 1), b = scoreAt (bestLag), c = scoreAt (bestLag + 1);
                const double curve = a - 2.0 * b + c;
                if (curve < 0.0)
                    lagExact += juce::jlimit (-0.5, 0.5, 0.5 * (a - c) / curve);
                double raw = 60.0 * framesPerSecond / lagExact;

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

        // HARMONIC ONLY (20 Nov 2026): drums smear every pitch class; a median along time keeps what holds a pitch, a median along
        // frequency keeps what is broadband, and each bin is weighted by H^2 / (H^2 + P^2) before the semitone bands are summed.
        int topBin = 0;
        for (const auto& band : bands) topBin = std::max (topBin, band.lastBin);
        const int nb = std::min (kPitchSize / 2, topBin + kHpssSpan);
        std::vector<float> mag ((size_t) std::max (0, frameCount) * (size_t) nb, 0.0f);
        for (int f = 0; f < frameCount; ++f)
        {
            std::fill (frame.begin(), frame.end(), 0.0f);
            std::copy (mono.begin() + (size_t) f * kPitchHop,
                       mono.begin() + (size_t) f * kPitchHop + kPitchSize,
                       frame.begin());
            window.multiplyWithWindowingTable (frame.data(), (size_t) kPitchSize);
            fft.performFrequencyOnlyForwardTransform (frame.data());
            std::copy (frame.begin(), frame.begin() + nb, mag.begin() + (size_t) f * (size_t) nb);
        }
        auto at = [&] (int f, int b) { return mag[(size_t) f * (size_t) nb + (size_t) b]; };
        auto reflect = [] (int i, int n) { if (n <= 1) return 0; while (i < 0 || i >= n) i = i < 0 ? -i - 1 : 2 * n - i - 1; return i; };
        std::vector<float> win ((size_t) kHpssSpan);
        auto median = [&win]() { std::nth_element (win.begin(), win.begin() + kHpssSpan / 2, win.end()); return win[(size_t) kHpssSpan / 2]; };

        for (int f = 0; f < frameCount; ++f)
        {
            std::fill (frameChroma.begin(), frameChroma.end(), 0.0);
            for (const auto& band : bands)
            {
                double energy = 0.0;
                for (int bin = band.firstBin; bin <= band.lastBin; ++bin)
                {
                    for (int k = 0; k < kHpssSpan; ++k) win[(size_t) k] = at (reflect (f + k - kHpssSpan / 2, frameCount), bin);
                    const double h = median();
                    for (int k = 0; k < kHpssSpan; ++k) win[(size_t) k] = at (f, reflect (bin + k - kHpssSpan / 2, nb));
                    const double p = median();
                    const double mask = (h * h) / (h * h + p * p + 1e-12);
                    energy += at (f, bin) * mask;
                }
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
