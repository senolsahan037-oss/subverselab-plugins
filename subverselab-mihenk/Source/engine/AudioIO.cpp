#include "AudioIO.h"

namespace mihenk
{
LoadedAudio loadAudioFile (const juce::File& file)
{
    LoadedAudio a;
    juce::AudioFormatManager fm;
    fm.registerFormat (new juce::WavAudioFormat(), true);
    fm.registerFormat (new juce::AiffAudioFormat(), false);
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
    if (reader == nullptr)
    {
        a.error = "Not a readable WAV or AIFF file: " + file.getFileName();
        return a;
    }
    if (reader->lengthInSamples <= 0 || reader->lengthInSamples > (juce::int64) std::numeric_limits<int>::max() / 2)
    {
        a.error = "Unsupported length";
        return a;
    }
    const int chans = (int) juce::jmin<unsigned int> (2, reader->numChannels);
    if (reader->numChannels > 2) a.note = juce::String ((int) reader->numChannels) + " channels in the file; using the first two.";
    const double sr = reader->sampleRate;
    if (! (sr == 44100.0 || sr == 48000.0 || sr == 88200.0 || sr == 96000.0))
        a.note << (a.note.isEmpty() ? "" : " ") << "Sample rate " << juce::String (sr, 0) << " Hz is outside the tested set (44.1 / 48 / 96 kHz).";
    const int n = (int) reader->lengthInSamples;
    a.buffer.setSize (chans, n);
    reader->read (&a.buffer, 0, n, 0, true, chans > 1);
    a.sampleRate = sr;
    a.sourceBits = (int) reader->bitsPerSample;
    return a;
}

int quantise (const juce::AudioBuffer<float>& in, int bits, bool tpdf, uint32_t seed, std::vector<std::vector<int>>& out)
{
    const double full = std::ldexp (1.0, bits - 1);
    const int lo = -(int) full, hi = (int) full - 1;
    uint32_t state = seed != 0 ? seed : 0x9E3779B9u;
    auto uniform = [&state]
    {
        // xorshift32: fast, deterministic, plenty for dither.
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        return (double) state / 4294967296.0;
    };
    int clipped = 0;
    out.assign ((size_t) in.getNumChannels(), std::vector<int> ((size_t) in.getNumSamples()));
    for (int i = 0; i < in.getNumSamples(); ++i)
        for (int c = 0; c < in.getNumChannels(); ++c)
        {
            double v = (double) in.getSample (c, i) * full;
            if (tpdf) v += uniform() - uniform();
            long q = std::lround (v);
            if (q < lo) { q = lo; ++clipped; }
            if (q > hi) { q = hi; ++clipped; }
            out[(size_t) c][(size_t) i] = (int) q;
        }
    return clipped;
}

bool exportWav (const juce::File& file, const juce::AudioBuffer<float>& buffer, double sr, int bits, juce::String& error, int* clippedOut)
{
    if (bits != 16 && bits != 24)
    {
        error = "Only 16 and 24 bit export";
        return false;
    }
    std::vector<std::vector<int>> q;
    const int clipped = quantise (buffer, bits, bits == 16, 0x4D48u /* "MH" */, q);
    if (clippedOut != nullptr) *clippedOut = clipped;

    // The writer takes left-justified 32-bit integers for integer formats.
    const int shift = 32 - bits;
    for (auto& ch : q)
        for (auto& v : ch) v = (int) ((unsigned int) v << shift);

    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
    if (! static_cast<juce::FileOutputStream*> (stream.get())->openedOk())
    {
        error = "Cannot write " + file.getFullPathName();
        return false;
    }
    juce::WavAudioFormat wav;
    const auto options = juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (buffer.getNumChannels())
                             .withBitsPerSample (bits).withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::integral);
    auto w = wav.createWriterFor (stream, options);
    if (w == nullptr)
    {
        error = "WAV writer refused the format";
        return false;
    }
    std::vector<const int*> ptrs;
    for (auto& ch : q) ptrs.push_back (ch.data());
    ptrs.push_back (nullptr);
    if (! w->write (ptrs.data(), buffer.getNumSamples()))
    {
        error = "Write failed";
        return false;
    }
    return true;
}
} // namespace mihenk
