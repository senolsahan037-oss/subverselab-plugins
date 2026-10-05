#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <memory>
#include "../dsp/Filters.h"
#include "../engine/MeterFifo.h"

/*  Standalone playback of the rendered master (B) or the original (A), at the file's sample rate. A is played with
    a gain that matches its integrated loudness to the master, so the comparison is not "louder wins". Switching
    A/B crossfades over 20 ms at the same position. Loop region optional.

    While it plays it feeds the real-time metering path: for every sample it writes one MeterFifo frame from the
    render cache at the playhead (the source, the signal after the EQ section and after the compressor, what is
    heard, and the gain reductions from the render's 10 ms histories). The stage taps are kept as 16-bit copies
    scaled to their own peak (96 dB of range is plenty for meters and spectra, and a 4-minute song costs 46 MB a
    tap instead of 92).
*/
namespace mihenk
{
/** A render stage kept for the meters: 16-bit, scaled to its own peak. */
struct TapBuffer
{
    std::vector<int16_t> data;         // interleaved
    int channels = 0, length = 0;
    float scale = 1.0f;
    static std::shared_ptr<const TapBuffer> make (const juce::AudioBuffer<float>& b);
    inline float get (int ch, int i) const noexcept { return (float) data[(size_t) i * (size_t) channels + (size_t) juce::jmin (ch, channels - 1)] * scale; }
};

/** Gain reduction histories of a render, one value per `hop` samples (positive dB). */
struct GrHistories
{
    std::vector<float> comp, band[3], lim;
    int hop = 480;
};

class Player : public juce::AudioSource
{
public:
    using BufferPtr = std::shared_ptr<const juce::AudioBuffer<float>>;
    using TapPtr = std::shared_ptr<const TapBuffer>;
    using GrPtr = std::shared_ptr<const GrHistories>;

    void setSources (BufferPtr original, BufferPtr master, double fileRate);
    void setMaster (BufferPtr master);
    void setTaps (TapPtr afterEq, TapPtr afterComp, GrPtr gr);
    void setOriginalGain (float g) { originalGain.store (g); }
    float getOriginalGain() const { return originalGain.load(); }
    void setMeterFifo (MeterFifo* f) { meters = f; }

    /** BYPASS (standalone): the original at its own level, raw, crossfaded in 20 ms; the master stays loaded and no render runs. */
    void setBypass (bool b) { bypass.store (b); }
    bool isBypassed() const { return bypass.load(); }

    void setListenOriginal (bool a) { listenOriginal.store (a); }
    bool isListeningOriginal() const { return listenOriginal.load(); }

    void play()  { if (length.load() > 0) playing.store (true); }
    void pause() { playing.store (false); }
    void stop();
    bool isPlaying() const { return playing.load(); }

    void setPosition (juce::int64 samples);
    juce::int64 getPosition() const { return position.load(); }
    juce::int64 getLength() const { return length.load(); }
    double getFileRate() const { return fileRate.load(); }

    void setLoop (bool on) { looping.store (on); }
    bool isLooping() const { return looping.load(); }
    void setLoopRegion (juce::int64 start, juce::int64 end);
    juce::int64 getLoopStart() const { return loopStart.load(); }
    juce::int64 getLoopEnd() const { return loopEnd.load(); }

    void prepareToPlay (int samplesPerBlockExpected, double sampleRate) override;
    void releaseResources() override {}
    void getNextAudioBlock (const juce::AudioSourceChannelInfo&) override;

private:
    juce::SpinLock lock;
    BufferPtr original, master;
    TapPtr tapEq, tapComp;
    GrPtr gr;
    MeterFifo* meters = nullptr;
    std::atomic<double> fileRate { 48000.0 };
    std::atomic<float> originalGain { 1.0f };
    std::atomic<bool> bypass { false }, listenOriginal { false }, playing { false }, looping { false };
    std::atomic<juce::int64> position { 0 }, length { 0 }, loopStart { 0 }, loopEnd { 0 };
    float mixA = 0.0f;   // audio thread: 0 = master, 1 = original (level-matched A/B)
    float mixBy = 0.0f;  // audio thread: 0 = what A/B selects, 1 = the original, raw (BYPASS)
    std::array<float, 256 * MeterFifo::frameSize> frames {};
};
} // namespace mihenk
