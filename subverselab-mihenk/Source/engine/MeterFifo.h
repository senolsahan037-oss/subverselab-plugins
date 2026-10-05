#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <vector>

/*  The real-time metering path: the audio thread writes one frame per sample (four stage taps in stereo and the
    gain reductions), the UI timer drains it. Lock-free (juce::AbstractFifo, single producer / single consumer),
    preallocated, and the writer never waits: when the UI has not drained (window closed, menu open), new frames
    are dropped, never the audio.

    The taps are time-aligned to the output: IN is the input as it entered the chain, EQ after the whole EQ section
    (static, dynamic, AIR), COMP after the compressor, OUT what leaves MIHENK. Gain reductions are positive dB.
*/
namespace mihenk
{
class MeterFifo
{
public:
    enum Slot { inL = 0, inR, eqL, eqR, compL, compR, outL, outR, grComp, grLow, grMid, grHigh, grLim, frameSize };

    explicit MeterFifo (int capacityFrames = 1 << 15)
        : fifo (capacityFrames), data ((size_t) capacityFrames * frameSize, 0.0f) {}

    /** Audio thread. `frames` holds numFrames * frameSize floats. Returns false when it had to drop. */
    bool push (const float* frames, int numFrames) noexcept
    {
        int s1, n1, s2, n2;
        fifo.prepareToWrite (numFrames, s1, n1, s2, n2);
        if (n1 > 0) std::memcpy (data.data() + (size_t) s1 * frameSize, frames, (size_t) n1 * frameSize * sizeof (float));
        if (n2 > 0) std::memcpy (data.data() + (size_t) s2 * frameSize, frames + (size_t) n1 * frameSize, (size_t) n2 * frameSize * sizeof (float));
        fifo.finishedWrite (n1 + n2);
        framesWritten.fetch_add (n1 + n2, std::memory_order_relaxed);
        return n1 + n2 == numFrames;
    }

    /** UI thread. Copies up to maxFrames frames into dest; returns the count. */
    int pull (float* dest, int maxFrames) noexcept
    {
        int s1, n1, s2, n2;
        fifo.prepareToRead (maxFrames, s1, n1, s2, n2);
        if (n1 > 0) std::memcpy (dest, data.data() + (size_t) s1 * frameSize, (size_t) n1 * frameSize * sizeof (float));
        if (n2 > 0) std::memcpy (dest + (size_t) n1 * frameSize, data.data() + (size_t) s2 * frameSize, (size_t) n2 * frameSize * sizeof (float));
        fifo.finishedRead (n1 + n2);
        return n1 + n2;
    }

    int available() const noexcept { return fifo.getNumReady(); }
    void clear() noexcept { fifo.reset(); }
    int capacity() const noexcept { return fifo.getTotalSize(); }

    // Context written by the audio side, read by the UI.
    std::atomic<double> sampleRate { 48000.0 };   // rate of the frames
    std::atomic<bool> running { false };          // audio is moving (playing / host transport rolling)
    std::atomic<bool> monoSource { false };       // the source is one channel (BS.1770 weight: meter it as one)
    std::atomic<long long> framesWritten { 0 };
    std::atomic<long long> position { 0 };        // standalone: playhead of the last frame written (file samples)

private:
    juce::AbstractFifo fifo;
    std::vector<float> data;
};
} // namespace mihenk
