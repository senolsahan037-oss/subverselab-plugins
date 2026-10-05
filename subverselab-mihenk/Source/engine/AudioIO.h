#pragma once

#include <JuceHeader.h>
#include <vector>

namespace mihenk
{
struct LoadedAudio
{
    juce::AudioBuffer<float> buffer;
    double sampleRate = 0.0;
    int sourceBits = 0;
    juce::String error, note;
    bool ok() const { return error.isEmpty() && buffer.getNumSamples() > 0; }
};

/** WAV or AIFF, mono or stereo (more channels: the first two are used and a note says so). */
LoadedAudio loadAudioFile (const juce::File& file);

/*  Quantise to integers of `bits` (16 or 24), full scale 2^(bits-1).
    With `tpdf`, triangular dither of +-1 LSB (two independent uniform values)
    is added before rounding. No noise shaping. Deterministic for a given seed.
    Returns the number of samples that had to be clipped. */
int quantise (const juce::AudioBuffer<float>& in, int bits, bool tpdf, uint32_t seed, std::vector<std::vector<int>>& out);

/** Write a WAV at the buffer's rate. 16-bit gets TPDF dither, 24-bit is rounded without dither. */
bool exportWav (const juce::File& file, const juce::AudioBuffer<float>& buffer, double sampleRate, int bits,
                juce::String& error, int* clippedSamples = nullptr);
} // namespace mihenk
