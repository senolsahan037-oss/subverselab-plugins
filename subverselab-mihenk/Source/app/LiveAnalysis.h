#pragma once

#include <JuceHeader.h>
#include <vector>
#include "../engine/MeterFifo.h"
#include "../dsp/Filters.h"
#include "../dsp/Loudness.h"

/*  The live views' numbers, computed on the message thread (the panel's 30 fps timer) from the frames the audio
    thread leaves in the MeterFifo. Nothing here runs on the audio thread.

      - Sample peak per channel (IN, OUT) and the 4x true peak of each drained chunk (BS.1770 meter, the same
        interpolator as the file measurement), a 3 s peak hold and a max hold until reset.
      - BS.1770 momentary (400 ms) and short-term (3 s) loudness of OUT and IN, and the gated integrated loudness of
        OUT since playback started.
      - Layered spectra of IN, EQ, COMP and OUT: Hann FFT of the last ~85 ms, integrated into the same 1/6-octave
        band power on the 1/12-octave grid as the whole-track spectrum, so live and long-term curves share one
        absolute scale; smoothed with a fast rise and a ~200 ms fall.
      - Scrolling history: 10 ms columns (IN / OUT min-max, compressor and limiter reduction) and 100 ms loudness steps.
      - Correlation of OUT, whole band and below 150 Hz, over about 300 ms.
*/
namespace mihenk
{
class LiveAnalysis
{
public:
    explicit LiveAnalysis (MeterFifo& fifo);

    /** Drains the FIFO; returns true when new audio arrived. */
    bool update();
    /** Max holds (and the integrated loudness) back to empty. */
    void resetMaxHold();
    /** A new playback run: histories and the integrated loudness restart. */
    void resetRun();

    /** Audio is moving (frames arrived in the last 0.4 s). */
    bool running() const noexcept { return isRunning; }
    double rate() const noexcept { return fs; }

    struct Bar
    {
        float level = -120.0f;       // this tick's peak, dB (displayed with a fall)
        float shown = -120.0f;
        float hold = -120.0f;        // 3 s peak hold
        double holdAt = 0.0;
        float max = -120.0f;         // until reset
    };
    Bar in[2], out[2];               // sample peak, dBFS
    float inTruePeak = -120.0f, outTruePeak = -120.0f;          // this tick, dBTP
    float inTruePeakMax = -120.0f, outTruePeakMax = -120.0f;    // since reset
    float outTruePeakHold = -120.0f;                            // highest of the last 1.5 s
    double outTpHoldAt = 0.0;
    float momentary = -120.0f, shortTerm = -120.0f, inMomentary = -120.0f, inShortTerm = -120.0f;
    double integrated = kNegInf;     // OUT, gated, since the run started
    float compGr = 0.0f, bandGr[3] { 0.0f, 0.0f, 0.0f }, limGr = 0.0f;       // shown (with a fall)
    float compGrMax = 0.0f, limGrMax = 0.0f;
    float correlation = 1.0f, lowCorrelation = 1.0f;

    // Spectra on spectrumGrid(), dB band power; layer 0 IN, 1 EQ, 2 COMP, 3 OUT.
    static constexpr int layers = 4;
    std::vector<float> spectrum[layers];
    bool spectrumValid = false;

    // 10 ms columns, oldest first when read through column(i) for i in [0, columnCount()).
    struct Column { float inMin = 0, inMax = 0, outMin = 0, outMax = 0, comp = 0, lim = 0, band[3] { 0, 0, 0 }; };
    static constexpr int maxColumns = 1000;          // 10 s
    int columnCount() const noexcept { return colCount; }
    const Column& column (int i) const noexcept { return cols[(size_t) ((colHead - colCount + i + maxColumns) % maxColumns)]; }

    // 100 ms loudness steps (OUT momentary, OUT short-term, IN short-term), oldest first.
    static constexpr int maxSteps = 600;             // 60 s
    int stepCount() const noexcept { return stCount; }
    float stepMomentary (int i) const noexcept { return mHist[(size_t) idx (i)]; }
    float stepShortTerm (int i) const noexcept { return sHist[(size_t) idx (i)]; }
    float stepInShortTerm (int i) const noexcept { return inSHist[(size_t) idx (i)]; }

    long long framesSeen() const noexcept { return totalFrames; }

private:
    int idx (int i) const noexcept { return (stHead - stCount + i + maxSteps) % maxSteps; }
    void configure (double rate);
    void frame (const float* f);
    void finishTick (int n);
    void computeSpectra();

    MeterFifo& fifo;
    double fs = 0.0;
    bool isRunning = false;
    double lastAudioMs = 0.0;
    std::vector<float> scratch;

    // true peak
    TruePeakDetector tp { 4 };
    std::vector<float> tpWin[4], tpEnv;          // IN L/R, OUT L/R: 63 samples of history + the chunk
    // loudness
    Biquad kw[2][2][2];                          // [in/out][ch][stage]
    int blockLen = 4800, blockPos = 0;
    double blockE[2] { 0.0, 0.0 };               // [in/out]
    std::vector<double> ring[2];                 // last 30 100-ms block energies
    int ringPos = 0, ringFill = 0;
    std::vector<double> gating;                  // 400 ms energies (100 ms hop) of OUT since the run started
    // spectrum
    int fftOrder = 12, fftN = 4096, specPos = 0, sinceFft = 0;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> specRing[layers][2], window;
    std::vector<std::complex<float>> fftIn, fftOut;
    // columns
    int colHop = 480, colPos = 0, colHead = 0, colCount = 0;
    Column acc;
    std::vector<Column> cols;
    // steps
    std::vector<float> mHist, sHist, inSHist;
    int stHead = 0, stCount = 0;
    // correlation
    Biquad lowLp[2];
    double cLR = 0.0, cLL = 0.0, cRR = 0.0, lLR = 0.0, lLL = 0.0, lRR = 0.0, corrCoef = 0.999;
    // tick
    float tickPeak[4] { 0, 0, 0, 0 }, tickComp = 0, tickBand[3] { 0, 0, 0 }, tickLim = 0;
    long long totalFrames = 0;
    bool mono = false;
};
} // namespace mihenk
