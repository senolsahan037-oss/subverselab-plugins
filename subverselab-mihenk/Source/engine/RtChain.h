#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <memory>
#include "Settings.h"
#include "MasterChain.h"
#include "MeterFifo.h"

/*  The real-time chain: the same processing as MasterChain, streamed sample by sample, for the DAW (VST3 / AU)
    and for anything that has to run while the audio plays.

        DC filter -> balance -> trim (+ learned headroom assistant) -> EQ FIR (partitioned FFT convolution)
        -> dynamic EQ (low-mid, upper-mid, tightener, dynamic AIR) -> compressor (WIDE | 3-BAND)
        -> stereo (width, mono below / phase-safe, high width) -> drive -> true-peak limiter (4 look-ahead passes)

    How it matches the offline engine:
      - Every causal stage (compressor detector and ballistics, dynamic bands, gains) is the same arithmetic.
      - The offline engine's zero-phase filters (forward-backward Butterworth: the 3-band split, mono below,
        high width, the de-harsh detector) are run here as their exact impulse response (the autocorrelation of
        the Butterworth response), truncated where its tail is below 1e-6 of its area, as a linear-phase FIR with
        a fixed delay. The EQ FIR and the AIR shaping FIR are the offline kernels themselves.
      - The limiter is the offline gain curve (look-ahead minimum, release, box average) computed as a stream,
        and the offline safety passes are three more cascaded passes (each gives unity gain wherever the previous
        one held the ceiling, as the offline passes do).
      - What the offline engine solves from the whole song (AUTO thresholds of the dynamic bands, band make-ups,
        the headroom assistant, dynamic AIR's typical level, the phase-safe polarity) comes in an RtPlan, filled
        from an offline render: the standalone renders the file, the plug-in renders what LEARN captured.
      - The DC filter is the one exception: offline it is zero phase, here a causal 5 Hz high-pass (its zero-phase
        kernel would be about a second long).
    Latency = framing block + limiter look-ahead + the delays of the stages that are switched in (LINEAR PHASE,
    AIR DYN, 3-BAND, the stereo split). Switching one of them fades out for a block, switches and fades back in.
    Nothing on the audio thread allocates or locks: kernels are designed on a worker thread (or inline in
    non-realtime mode) and swapped with a crossfade; the meters leave through a lock-free FIFO.
*/
namespace mihenk
{
/** What the real-time chain needs from a whole-song analysis. Plain values only. */
struct RtPlan
{
    bool valid = false;
    double sampleRate = 0.0;
    double autoTrimDb = 0.0;                       // headroom assistant (applies when autoTrim is on)
    bool sourceValid = false, denseWide = false, densePeak = false;   // the dense rule on the source mix
    double wideMakeupDb = 0.0;                     // WIDE: level-neutral make-up
    bool bandActive[3] { false, false, false };
    double bandMakeupDb[3] { 0.0, 0.0, 0.0 };      // 3-BAND: per band, then the final correction
    double bandFinalMakeupDb = 0.0;
    bool dynActive[3] { false, false, false };     // tightener, low-mid, upper-mid (as the offline order)
    double dynThresholdDb[3] { 0.0, 0.0, 0.0 };
    bool airActive = false;
    double airTypicalRelDb = 0.0;
    double safeSign = 1.0, safeGain = 1.0;         // phase-safe low mono
    juce::int64 learnedSamples = 0;                // length of the analysed audio
};

/** The plan an offline render implies (`solved` = the settings after the render wrote its solved values back). */
RtPlan makePlan (const MasterSettings& solved, const RenderResult& r, double sampleRate);
/** Every number of a plan finite and inside a sane range (a plan read from a saved project is not trusted). */
void sanitisePlan (RtPlan&) noexcept;

class RtChain
{
public:
    static constexpr int blockSize = 256;          // internal processing block (the framing latency)
    static constexpr int stages = 4;               // meter taps: IN, EQ, COMP, OUT

    RtChain();
    ~RtChain();

    /** Message thread: allocates everything. maxHostBlock is the largest block process() will get. */
    void prepare (double sampleRate, int numChannels, int maxHostBlock);
    /** Clears all state (delay lines, detectors, limiter); parameter ramps snap to the next settings. */
    void reset();
    /** Message thread (or any non-audio thread): design every kernel for these settings now and install them. */
    void prime (const MasterSettings& s);

    double getSampleRate() const noexcept;
    int getNumChannels() const noexcept;
    /** Total latency in samples with these settings. Only the switches of the delaying stages count: LINEAR PHASE,
        AIR DYN, 3-BAND, and the stereo split (MONO BELOW on or HIGH WIDTH away from 100 %); knob values never do. */
    int latencySamples (const MasterSettings& s) const noexcept;
    /** Latency with every delaying stage switched in: the longest delay the host can ever be told. */
    int maxLatencySamples() const noexcept;
    /** Latency of the structure the audio thread is running now (the host is told on the message thread). */
    int currentLatency() const noexcept;

    /** Tone-match curve (dB on spectrumGrid()) used with eq.matchAmount. Message thread. */
    void setMatchCurve (const std::vector<float>& curveDb);

    /** Non-realtime rendering (offline bounce, tests): kernels are designed inline, so a render is deterministic. */
    void setSynchronousKernels (bool shouldDesignInline) noexcept;

    /** Audio thread. The plan is copied (plain values). */
    void setPlan (const RtPlan& plan) noexcept;
    const RtPlan& getPlan() const noexcept;

    /** A/B: listen to the input (delayed like the output) with this gain, crossfaded. */
    void setListenInput (bool listenInput, float inputGain) noexcept;

    /** Audio thread: process in place, any block size up to maxHostBlock. `s` must not carry a match curve
        (it is never read here; the curve comes from setMatchCurve). */
    void process (float* const* channels, int numChannels, int numSamples, const MasterSettings& s) noexcept;

    /** Meter taps go here (nullptr: none). */
    void setMeterFifo (MeterFifo* fifo) noexcept;

    /** True while a kernel is being designed or crossfaded (tests wait for this to settle). */
    bool kernelsSettling() const noexcept;

    // Last block, for tests and the UI: largest gain reductions (positive dB).
    float lastCompGrDb() const noexcept;
    float lastLimiterGrDb() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace mihenk
