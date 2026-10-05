#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <memory>

/*  What LEARN hears: the plug-in's input while the host plays, in 5-second chunks. The message thread keeps a few
    empty chunks ready ahead of the writer (ensureAhead), so the audio thread only copies samples. In a
    non-realtime render (offline bounce) the audio thread may allocate a chunk itself when it runs out; in real time
    it stops capturing instead and says so (overflowed()). Up to 20 minutes.
*/
namespace mihenk
{
class Capture
{
public:
    static constexpr int chunkSeconds = 5, maxMinutes = 20;

    Capture() : slots (std::make_unique<std::atomic<juce::AudioBuffer<float>*>[]> (maxChunks)) { for (int i = 0; i < maxChunks; ++i) slots[i] = nullptr; }
    ~Capture() { freeAll(); }

    /** Message thread: forget everything and set the format for the next capture. */
    void start (double sampleRate, int numChannels)
    {
        freeAll();
        rate = sampleRate;
        chans = juce::jlimit (1, 2, numChannels);
        chunkLen = juce::jmax (1, (int) (chunkSeconds * rate));
        written = 0;
        full = false;
        overflow = false;
        ensureAhead (3);
    }

    /** Message thread: make sure `count` chunks beyond the writer exist. */
    void ensureAhead (int count)
    {
        const int want = juce::jmin (maxChunks, (int) (written.load() / juce::jmax (1, chunkLen)) + 1 + count);
        for (int i = 0; i < want; ++i)
            if (slots[i].load() == nullptr)
                slots[i].store (new juce::AudioBuffer<float> (chans, chunkLen));
    }

    /** Audio thread. */
    void write (const float* const* x, int numChannels, int n, bool mayAllocate) noexcept
    {
        if (full.load() || chunkLen <= 0) return;
        juce::int64 w = written.load();
        int done = 0;
        while (done < n)
        {
            const int chunk = (int) (w / chunkLen), off = (int) (w % chunkLen);
            if (chunk >= maxChunks) { full = true; break; }
            auto* b = slots[chunk].load();
            if (b == nullptr)
            {
                if (! mayAllocate) { overflow = true; break; }
                b = new juce::AudioBuffer<float> (chans, chunkLen);
                slots[chunk].store (b);
            }
            const int take = juce::jmin (n - done, chunkLen - off);
            for (int c = 0; c < chans; ++c)
                b->copyFrom (c, off, x[juce::jmin (c, numChannels - 1)] + done, take);
            done += take;
            w += take;
        }
        written.store (w);
    }

    juce::int64 samples() const noexcept { return written.load(); }
    double seconds() const noexcept { return rate > 0.0 ? (double) written.load() / rate : 0.0; }
    double sampleRate() const noexcept { return rate; }
    bool overflowed() const noexcept { return overflow.load(); }
    bool isFull() const noexcept { return full.load(); }

    /** Message thread (after the audio thread stopped writing): the captured audio in one buffer. */
    std::shared_ptr<juce::AudioBuffer<float>> assemble() const
    {
        const auto n = written.load();
        auto out = std::make_shared<juce::AudioBuffer<float>> (chans, (int) n);
        juce::int64 pos = 0;
        for (int i = 0; i < maxChunks && pos < n; ++i)
        {
            auto* b = slots[i].load();
            if (b == nullptr) break;
            const int take = (int) juce::jmin ((juce::int64) chunkLen, n - pos);
            for (int c = 0; c < chans; ++c) out->copyFrom (c, (int) pos, *b, c, 0, take);
            pos += take;
        }
        return out;
    }

    void freeAll()
    {
        for (int i = 0; i < maxChunks; ++i) delete slots[i].exchange (nullptr);
        written = 0;
    }

private:
    static constexpr int maxChunks = maxMinutes * 60 / chunkSeconds;
    std::unique_ptr<std::atomic<juce::AudioBuffer<float>*>[]> slots;
    double rate = 48000.0;
    int chans = 2, chunkLen = 0;
    std::atomic<juce::int64> written { 0 };
    std::atomic<bool> full { false }, overflow { false };
};
} // namespace mihenk
