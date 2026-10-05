#include "LiveAnalysis.h"
#include "../dsp/Spectrum.h"

namespace mihenk
{
namespace
{
double gatedLufs (const std::vector<double>& blocks)
{
    double sum = 0.0;
    int count = 0;
    for (double z : blocks)
        if (energyToLufs (z) > -70.0) { sum += z; ++count; }
    if (count == 0) return kNegInf;
    const double rel = energyToLufs (sum / count) - 10.0;
    sum = 0.0; count = 0;
    for (double z : blocks)
    {
        const double l = energyToLufs (z);
        if (l > -70.0 && l > rel) { sum += z; ++count; }
    }
    return count > 0 ? energyToLufs (sum / count) : kNegInf;
}
} // namespace

LiveAnalysis::LiveAnalysis (MeterFifo& f) : fifo (f)
{
    scratch.resize ((size_t) f.capacity() * MeterFifo::frameSize);
    cols.resize ((size_t) maxColumns);
    mHist.assign ((size_t) maxSteps, -120.0f);
    sHist.assign ((size_t) maxSteps, -120.0f);
    inSHist.assign ((size_t) maxSteps, -120.0f);
    for (auto& s : spectrum) s.assign (spectrumGrid().size(), -150.0f);
    configure (48000.0);
}

void LiveAnalysis::configure (double rate)
{
    fs = rate;
    for (int io = 0; io < 2; ++io)
        for (int c = 0; c < 2; ++c)
        {
            kw[io][c][0] = Biquad::kWeightingShelf (fs);
            kw[io][c][1] = Biquad::kWeightingHighPass (fs);
        }
    blockLen = juce::jmax (1, (int) std::lround (0.1 * fs));
    for (auto& r : ring) r.assign (30, 0.0);
    fftOrder = fs > 50000.0 ? 13 : 12;
    fftN = 1 << fftOrder;
    fft = std::make_unique<juce::dsp::FFT> (fftOrder);
    for (auto& l : specRing) for (auto& c : l) c.assign ((size_t) fftN, 0.0f);
    window.resize ((size_t) fftN);
    for (int i = 0; i < fftN; ++i) window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * kPi * i / fftN));
    fftIn.resize ((size_t) fftN);
    fftOut.resize ((size_t) fftN);
    colHop = juce::jmax (1, (int) std::lround (0.01 * fs));
    for (auto& q : lowLp) q = Biquad::butterworthLowPass (fs, 150.0);
    corrCoef = std::exp (-1.0 / (0.3 * fs));
    for (auto& w : tpWin) w.assign ((size_t) TruePeakDetector::taps - 1, 0.0f);
    resetRun();
}

void LiveAnalysis::resetRun()
{
    blockPos = 0;
    blockE[0] = blockE[1] = 0.0;
    for (auto& r : ring) std::fill (r.begin(), r.end(), 0.0);
    ringPos = ringFill = 0;
    gating.clear();
    integrated = kNegInf;
    colPos = 0; colHead = 0; colCount = 0;
    acc = {};
    stHead = 0; stCount = 0;
    specPos = 0; sinceFft = 0;
    spectrumValid = false;
    cLR = cLL = cRR = lLR = lLL = lRR = 0.0;
    for (int io = 0; io < 2; ++io) for (int c = 0; c < 2; ++c) for (auto& q : kw[io][c]) q.reset();
    for (auto& q : lowLp) q.reset();
}

void LiveAnalysis::resetMaxHold()
{
    for (auto* b : { &in[0], &in[1], &out[0], &out[1] }) { b->max = -120.0f; b->hold = -120.0f; }
    inTruePeakMax = outTruePeakMax = -120.0f;
    compGrMax = limGrMax = 0.0f;
    gating.clear();
    integrated = kNegInf;
}

void LiveAnalysis::frame (const float* f)
{
    // Peaks of this tick.
    for (int c = 0; c < 2; ++c)
    {
        tickPeak[c] = juce::jmax (tickPeak[c], std::abs (f[MeterFifo::inL + c]));
        tickPeak[2 + c] = juce::jmax (tickPeak[2 + c], std::abs (f[MeterFifo::outL + c]));
    }
    tickComp = juce::jmax (tickComp, f[MeterFifo::grComp]);
    for (int b = 0; b < 3; ++b) tickBand[b] = juce::jmax (tickBand[b], f[MeterFifo::grLow + b]);
    tickLim = juce::jmax (tickLim, f[MeterFifo::grLim]);

    // Loudness: K-weighted energy of IN and OUT (a mono source counts as its one channel).
    for (int io = 0; io < 2; ++io)
    {
        const int base = io == 0 ? MeterFifo::inL : MeterFifo::outL;
        double e = 0.0;
        for (int c = 0; c < 2; ++c)
        {
            const double y = kw[io][c][1].process (kw[io][c][0].process ((double) f[base + c]));
            e += y * y;
        }
        blockE[io] += mono ? 0.5 * e : e;
    }
    if (++blockPos >= blockLen)
    {
        for (int io = 0; io < 2; ++io) ring[io][(size_t) ringPos] = blockE[io];
        ringPos = (ringPos + 1) % 30;
        ringFill = juce::jmin (30, ringFill + 1);
        auto meanOf = [&] (int io, int blocks)
        {
            double s = 0.0;
            const int k = juce::jmin (blocks, ringFill);
            for (int j = 1; j <= k; ++j) s += ring[io][(size_t) ((ringPos - j + 30) % 30)];
            return k > 0 ? s / ((double) k * blockLen) : 0.0;
        };
        const double mOut = meanOf (1, 4), sOut = meanOf (1, 30), mIn = meanOf (0, 4), sIn = meanOf (0, 30);
        momentary = ringFill >= 4 ? (float) juce::jmax (-120.0, energyToLufs (mOut)) : -120.0f;
        shortTerm = ringFill >= 30 ? (float) juce::jmax (-120.0, energyToLufs (sOut)) : -120.0f;
        inMomentary = ringFill >= 4 ? (float) juce::jmax (-120.0, energyToLufs (mIn)) : -120.0f;
        inShortTerm = ringFill >= 30 ? (float) juce::jmax (-120.0, energyToLufs (sIn)) : -120.0f;
        if (ringFill >= 4) gating.push_back (mOut);
        integrated = gatedLufs (gating);
        mHist[(size_t) stHead] = momentary;
        sHist[(size_t) stHead] = shortTerm;
        inSHist[(size_t) stHead] = inShortTerm;
        stHead = (stHead + 1) % maxSteps;
        stCount = juce::jmin (maxSteps, stCount + 1);
        blockPos = 0;
        blockE[0] = blockE[1] = 0.0;
    }

    // Spectrum rings.
    for (int l = 0; l < layers; ++l)
    {
        const int base = l == 0 ? MeterFifo::inL : l == 1 ? MeterFifo::eqL : l == 2 ? MeterFifo::compL : MeterFifo::outL;
        specRing[l][0][(size_t) specPos] = f[base];
        specRing[l][1][(size_t) specPos] = f[base + 1];
    }
    specPos = (specPos + 1) % fftN;
    ++sinceFft;

    // Columns.
    const float inL = f[MeterFifo::inL], inR = f[MeterFifo::inR], oL = f[MeterFifo::outL], oR = f[MeterFifo::outR];
    if (colPos == 0) { acc = {}; acc.inMin = acc.inMax = inL; acc.outMin = acc.outMax = oL; }
    acc.inMin = juce::jmin (acc.inMin, inL, inR);
    acc.inMax = juce::jmax (acc.inMax, inL, inR);
    acc.outMin = juce::jmin (acc.outMin, oL, oR);
    acc.outMax = juce::jmax (acc.outMax, oL, oR);
    acc.comp = juce::jmax (acc.comp, f[MeterFifo::grComp]);
    acc.lim = juce::jmax (acc.lim, f[MeterFifo::grLim]);
    for (int b = 0; b < 3; ++b) acc.band[b] = juce::jmax (acc.band[b], f[MeterFifo::grLow + b]);
    if (++colPos >= colHop)
    {
        cols[(size_t) colHead] = acc;
        colHead = (colHead + 1) % maxColumns;
        colCount = juce::jmin (maxColumns, colCount + 1);
        colPos = 0;
    }

    // Correlation of OUT.
    cLR = corrCoef * cLR + (double) oL * oR;
    cLL = corrCoef * cLL + (double) oL * oL;
    cRR = corrCoef * cRR + (double) oR * oR;
    const double lL = lowLp[0].process (oL), lR = lowLp[1].process (oR);
    lLR = corrCoef * lLR + lL * lR;
    lLL = corrCoef * lLL + lL * lL;
    lRR = corrCoef * lRR + lR * lR;
}

void LiveAnalysis::computeSpectra()
{
    const auto& grid = spectrumGrid();
    double winSq = 0.0;
    for (float w : window) winSq += (double) w * w;
    const double norm = 2.0 / ((double) fftN * winSq * 2.0);
    const double df = fs / fftN;
    std::vector<double> power ((size_t) fftN / 2 + 1);
    for (int l = 0; l < layers; ++l)
    {
        for (int i = 0; i < fftN; ++i)
        {
            const int k = (specPos + i) % fftN;       // oldest first
            fftIn[(size_t) i] = { specRing[l][0][(size_t) k] * window[(size_t) i], specRing[l][1][(size_t) k] * window[(size_t) i] };
        }
        fft->perform (fftIn.data(), fftOut.data(), false);
        for (int k = 1; k <= fftN / 2; ++k)
        {
            const auto zk = fftOut[(size_t) k], zn = std::conj (fftOut[(size_t) ((fftN - k) % fftN)]);
            const auto xl = (zk + zn) * 0.5f;
            const auto xr = (zk - zn) * std::complex<float> (0.0f, -0.5f);
            power[(size_t) k] = ((double) std::norm (xl) + (double) std::norm (xr)) * norm;
        }
        auto& out = spectrum[l];
        for (size_t g = 0; g < grid.size(); ++g)
        {
            const double lo = grid[g] * std::pow (2.0, -1.0 / 12.0), hi = grid[g] * std::pow (2.0, 1.0 / 12.0);
            double sum = 0.0;
            const int k0 = juce::jmax (1, (int) std::floor (lo / df - 0.5)), k1 = juce::jmin (fftN / 2, (int) std::ceil (hi / df + 0.5));
            for (int k = k0; k <= k1; ++k)
            {
                const double a = juce::jmax (lo, (k - 0.5) * df), z = juce::jmin (hi, (k + 0.5) * df);
                if (z > a) sum += power[(size_t) k] * (z - a) / df;
            }
            const float db = (float) (sum > 0.0 ? juce::jmax (-150.0, 10.0 * std::log10 (sum)) : -150.0);
            float& v = out[g];
            if (! spectrumValid) v = db;
            else v += (db - v) * (db > v ? 0.6f : 0.16f);
        }
    }
    spectrumValid = true;
}

void LiveAnalysis::finishTick (int n)
{
    const double now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    // A tick that found no frames while the audio is still moving (the UI outran the audio callback) holds everything.
    if (n == 0 && isRunning) return;
    // True peak of the drained chunk: the 4x envelope on [63 samples of history | chunk], edges left out.
    auto chunkTp = [&] (int slot, int ch) -> float
    {
        auto& w = tpWin[slot * 2 + ch];
        const int hist = TruePeakDetector::taps - 1;
        w.resize ((size_t) (hist + n));
        const int base = (slot == 0 ? MeterFifo::inL : MeterFifo::outL) + ch;
        for (int i = 0; i < n; ++i) w[(size_t) (hist + i)] = scratch[(size_t) i * MeterFifo::frameSize + (size_t) base];
        tpEnv.resize (w.size());
        tp.envelope (w.data(), (int) w.size(), tpEnv.data());
        float m = 0.0f;
        for (int i = hist / 2; i < hist / 2 + n; ++i) m = juce::jmax (m, tpEnv[(size_t) i]);
        std::copy (w.end() - hist, w.end(), w.begin());
        w.resize ((size_t) hist);
        return m;
    };
    if (n > 0)
    {
        inTruePeak = (float) gainToDb (juce::jmax (chunkTp (0, 0), chunkTp (0, 1)), -120.0);
        outTruePeak = (float) gainToDb (juce::jmax (chunkTp (1, 0), chunkTp (1, 1)), -120.0);
        inTruePeakMax = juce::jmax (inTruePeakMax, inTruePeak);
        outTruePeakMax = juce::jmax (outTruePeakMax, outTruePeak);
        if (outTruePeak >= outTruePeakHold || now - outTpHoldAt > 1.5) { outTruePeakHold = outTruePeak; outTpHoldAt = now; }
    }
    Bar* bars[4] { &in[0], &in[1], &out[0], &out[1] };
    for (int k = 0; k < 4; ++k)
    {
        auto& b = *bars[k];
        b.level = n > 0 ? (float) gainToDb (tickPeak[k], -120.0) : -120.0f;
        b.shown = b.level > b.shown ? b.level : juce::jmax (b.level, b.shown - 1.2f);   // about 36 dB/s fall at 30 fps
        if (b.level >= b.hold || now - b.holdAt > 3.0) { b.hold = b.level; b.holdAt = now; }
        b.max = juce::jmax (b.max, b.level);
        tickPeak[k] = 0.0f;
    }
    auto fall = [] (float& shown, float target) { shown = target > shown ? target : juce::jmax (target, shown - 0.5f); };
    fall (compGr, tickComp);
    for (int b = 0; b < 3; ++b) fall (bandGr[b], tickBand[b]);
    fall (limGr, tickLim);
    compGrMax = juce::jmax (compGrMax, tickComp);
    limGrMax = juce::jmax (limGrMax, tickLim);
    tickComp = tickLim = 0.0f;
    for (auto& t : tickBand) t = 0.0f;
    correlation = (cLL > 1.0e-12 && cRR > 1.0e-12) ? (float) (cLR / std::sqrt (cLL * cRR)) : 1.0f;
    lowCorrelation = (lLL > 1.0e-12 && lRR > 1.0e-12) ? (float) (lLR / std::sqrt (lLL * lRR)) : 1.0f;
    if (sinceFft >= fftN / 4 && totalFrames >= fftN)
    {
        computeSpectra();
        sinceFft = 0;
    }
}

bool LiveAnalysis::update()
{
    const double rate = fifo.sampleRate.load();
    if (rate > 0.0 && std::abs (rate - fs) > 0.5) configure (rate);
    mono = fifo.monoSource.load();
    const bool wasRunning = isRunning;
    const int n = fifo.pull (scratch.data(), fifo.capacity());
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (n > 0) lastAudioMs = now;
    isRunning = n > 0 || now - lastAudioMs < 400.0;
    if (isRunning && ! wasRunning) resetRun();
    for (int i = 0; i < n; ++i) frame (scratch.data() + (size_t) i * MeterFifo::frameSize);
    totalFrames += n;
    finishTick (n);
    return n > 0;
}
} // namespace mihenk
