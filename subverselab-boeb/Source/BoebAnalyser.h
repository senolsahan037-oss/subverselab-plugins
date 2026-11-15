#pragma once

#include "BoebDsp.h"
#include "BoebReference.h"
#include <algorithm>
#include <vector>

/*  BOE-B five-cause analyser (LISTEN). Plain C++, no JUCE.

    Fed the vocal (and the beat on the side input when there is one) for a few seconds, it measures the five causes
    of a dull vocal against BoebReference.h and works out the settings the engine needs:

        1 LEVEL     vocal minus beat, A-weighted, active frames (needs the beat); the paper's method
        2 DROPS     spread of the 400 ms short-term A-weighted level, 90th minus 10th percentile, active frames
        3 PRESENCE  PRES re MID
        4 MUD       MUD re MID
        5 AIR       AIR re MID
    plus LOW re MID for the high-pass and SIB re MID for the read-out.

    Frames are 100 ms; a frame is active when its A-weighted level is above kActiveFloorDb and within
    kActiveBelowMaxDb of the loudest frame so far (decided at the end, over all frames). Allocation happens in
    prepare() (frame store for up to maxSeconds); feed() is real-time safe.
*/
namespace svl::boeb {

struct Diagnosis {
    bool valid = false;          // enough active frames to say anything
    int activeFrames = 0;
    bool hasBeat = false;
    float larDb = 0;             // 1: vocal minus beat (A-weighted), only with a beat
    float spreadDb = 0;          // 2
    float presDb = 0, mudDb = 0, airDb = 0, sibDb = 0, lowDb = 0;   // 3,4,5 + read-outs, dB re MID
    float vocalLevelDb = -300;   // A-weighted power mean of active frames, dBFS
    float cutHz = 0;             // highest frequency within kCutBelowMidDb of MID's density (averaged spectrum)
    float exciteSrcDb = -300;    // power of the octave under the cut, dB re MID
    float exciteGenDb = -300;    // AIR the exciter makes at unity gain, dB re MID (measured on the listened audio)
    double sampleRate = 48000;
    float beatLevelDb = -300;
};

struct Settings {
    float hpfHz = 80;            // high-pass
    float presenceDb = 0;        // presence bell at LIFT 6
    float mudCutDb = 0;          // what the dynamic mud cut will take at LIFT 6 (read-out; the cut itself is dynamic)
    float airDb = 0;             // air shelf at LIFT 6 (the AIR knob starts here)
    float compNeed = 1;          // 0.5 .. 1.6, scales the compressor
    bool bandLimited = false;    // nothing above the cut: the exciter makes AIR
    float cutHz = 0;             // 8000 or 11220
    float exciteDb = 0;          // the exciter's starting gain (the engine's loop takes it from there)
};

class BoebAnalyser {
public:
    void prepare (double sampleRate, double maxSeconds = 30.0)
    {
        m_fs = sampleRate > 0 ? sampleRate : 48000.0;
        m_frameLen = std::max (1, (int) std::lround (kFrameSec * m_fs));
        m_frames.assign ((size_t) std::ceil (maxSeconds / kFrameSec) + 1, Frame {});
        m_mono.assign (m_frames.size() * (size_t) m_frameLen, 0.0f);   // the listened audio, for the exciter's calibration
        aw.prepare (m_fs); awKey.prepare (m_fs);
        // Averaged spectrum for the band limit: Hann, 4096 points, no overlap (IIR probes next to a steep edge leaked
        // 14-20 dB and read an empty band as full).
        m_fftIn.assign ((size_t) kFftN, 0.0); m_re.assign ((size_t) kFftN, 0.0); m_im.assign ((size_t) kFftN, 0.0);
        m_spec.assign ((size_t) kFftN / 2 + 1, 0.0); m_win.resize ((size_t) kFftN);
        for (int i = 0; i < kFftN; ++i) m_win[(size_t) i] = 0.5 - 0.5 * std::cos (2 * kPi * i / kFftN);
        low.prepare (m_fs, kLowLo, kLowHi);   mud.prepare (m_fs, kMudLo, kMudHi);   mid.prepare (m_fs, kMidLo, kMidHi);
        pres.prepare (m_fs, kPresLo, kPresHi); sib.prepare (m_fs, kSibLo, kSibHi);  air.prepare (m_fs, kAirLo, kAirHi);
        reset();
    }

    void reset()
    {
        m_count = 0; m_pos = 0; m_acc = Frame {}; m_monoPos = 0;
        aw.reset(); awKey.reset();
        std::fill (m_spec.begin(), m_spec.end(), 0.0); m_specFrames = 0; m_fftPos = 0;
        low.reset(); mud.reset(); mid.reset(); pres.reset(); sib.reset(); air.reset();
    }

    /** vocal: numCh channels (summed to mono); key: beat channels or nullptr. */
    void feed (const float* const* vocal, int numCh, const float* const* key, int keyCh, int n)
    {
        if (numCh <= 0) return;
        for (int i = 0; i < n; ++i)
        {
            double x = 0;
            for (int c = 0; c < numCh; ++c) x += vocal[c][i];
            x /= numCh;
            if (! std::isfinite (x)) x = 0;
            double k = 0;
            if (key != nullptr && keyCh > 0)
            {
                for (int c = 0; c < keyCh; ++c) k += key[c][i];
                k /= keyCh;
                if (! std::isfinite (k)) k = 0;
            }
            const double a = aw.process (x), ak = awKey.process (k);
            m_acc.a += a * a;  m_acc.key += ak * ak;
            double v;
            v = low.process (x);  m_acc.low  += v * v;
            v = mud.process (x);  m_acc.mud  += v * v;
            v = mid.process (x);  m_acc.mid  += v * v;
            v = pres.process (x); m_acc.pres += v * v;
            v = sib.process (x);  m_acc.sib  += v * v;
            v = air.process (x);  m_acc.air  += v * v;
            m_fftIn[(size_t) m_fftPos++] = x;
            if (m_monoPos < m_mono.size()) m_mono[m_monoPos++] = (float) x;
            if (m_fftPos >= kFftN) addSpectrum();
            if (++m_pos >= m_frameLen)
            {
                if (m_count < m_frames.size())
                {
                    Frame f = m_acc;
                    const double inv = 1.0 / m_frameLen;
                    f.a *= inv; f.key *= inv; f.low *= inv; f.mud *= inv; f.mid *= inv; f.pres *= inv; f.sib *= inv; f.air *= inv;

                    m_frames[m_count++] = f;
                }
                m_acc = Frame {}; m_pos = 0;
            }
        }
    }

    double secondsFed() const { return m_count * kFrameSec; }

    Diagnosis diagnose() const
    {
        Diagnosis d;
        d.sampleRate = m_fs;
        if (m_count < (size_t) kShortTermFrames) return d;
        double maxDb = -300;
        for (size_t i = 0; i < m_count; ++i) maxDb = std::max (maxDb, (double) powToDb (m_frames[i].a));
        const double floorDb = std::max ((double) kActiveFloorDb, maxDb - kActiveBelowMaxDb);

        // Tone sums weight each frame by the inverse of its phrase level (1.1 s around it): the reference values are read
        // on the finished vocal, whose phrases the rider and compressor have evened out. A plain power sum let a short
        // loud event in the listened seconds (a 2 s shout before a verse 6 dB quieter) set the tone and the exciter's
        // gain for the whole take (AIR 4-5 dB over the target on the verse). Level and LAR stay plain power means.
        std::vector<double> w (m_count, 1.0);
        for (size_t i = 0; i < m_count; ++i)
        {
            double p = 0; int n = 0;
            for (size_t j = i >= (size_t) kPhraseHalfFrames ? i - (size_t) kPhraseHalfFrames : 0; j <= std::min (m_count - 1, i + (size_t) kPhraseHalfFrames); ++j)
                if (powToDb (m_frames[j].a) >= floorDb) { p += m_frames[j].a; ++n; }
            w[i] = n > 0 && p > 0 ? n / p : 1.0;
        }
        Frame sum {};
        int active = 0;
        double keyActive = 0;
        std::vector<float> st;   // short-term levels of active frames (only at LISTEN time, not in the audio thread)
        st.reserve (m_count);
        for (size_t i = 0; i < m_count; ++i)
        {
            const Frame& f = m_frames[i];
            if (powToDb (f.a) < floorDb) continue;
            ++active;
            sum.a += f.a; sum.key += f.key;
            const double wi = w[i];
            sum.low += wi * f.low; sum.mud += wi * f.mud; sum.mid += wi * f.mid;
            sum.pres += wi * f.pres; sum.sib += wi * f.sib; sum.air += wi * f.air;

            if (i + 1 >= (size_t) kShortTermFrames)
            {
                double p = 0;
                for (int j = 0; j < kShortTermFrames; ++j) p += m_frames[i - (size_t) j].a;
                st.push_back (powToDb (p / kShortTermFrames));
            }
            keyActive += f.key;
        }
        d.activeFrames = active;
        if (active < 10 || sum.mid <= 0) return d;
        d.valid = true;
        d.vocalLevelDb = powToDb (sum.a / active);
        d.lowDb  = powToDb (sum.low)  - powToDb (sum.mid);
        d.mudDb  = powToDb (sum.mud)  - powToDb (sum.mid);
        d.presDb = powToDb (sum.pres) - powToDb (sum.mid);
        d.sibDb  = powToDb (sum.sib)  - powToDb (sum.mid);
        d.airDb  = powToDb (sum.air)  - powToDb (sum.mid);
        if (m_specFrames > 0)
        {
            const double binHz = m_fs / kFftN;
            const auto bandMean = [&] (double lo, double hi) {
                double sm = 0; int n = 0;
                for (int k = (int) std::ceil (lo / binHz); k <= (int) std::floor (hi / binHz) && k <= kFftN / 2; ++k) { sm += m_spec[(size_t) k]; ++n; }
                return n > 0 ? sm / n : 0.0;
            };
            const double midD = bandMean (kMidLo, kMidHi);
            const double floorD = midD * std::pow (10.0, -kCutBelowMidDb / 10.0);
            int top = 0;
            for (int k = kFftN / 2; k > 0; --k) if (m_spec[(size_t) k] > floorD) { top = k; break; }
            d.cutHz = (float) (top * binHz);
            const double srcLo = 0.5 * d.cutHz, srcHi = 0.97 * d.cutHz;
            d.exciteSrcDb = powToDb (bandMean (srcLo, srcHi) * (srcHi - srcLo)) - powToDb (midD * (kMidHi - kMidLo));
        }
        // Exciter calibration: run the engine's exciter (same filters, same 2 ms RMS) at unity gain over the listened
        // audio and measure what it makes in the AIR band against MID, on the active frames. x^2 of speech depends on
        // the voice (sibilants make far more than vowels), so no formula stands in for this.
        if (d.cutHz > 2000.0f && 2.0 * d.cutHz < 0.49 * m_fs)
        {
            BandFilter src, out, airB, midB;
            src.prepare (m_fs, d.cutHz * 0.5, d.cutHz * 0.97);
            out.prepare (m_fs, d.cutHz, std::min (1.94 * d.cutHz, 0.45 * m_fs));
            airB.prepare (m_fs, kAirLo, std::min (kAirHi, 0.45 * m_fs));
            midB.prepare (m_fs, kMidLo, kMidHi);
            const double cEnv = coefFor (0.002, m_fs);   // == BoebEngine::kExcRmsSec
            double env = 0, genSum = 0, midSum = 0;
            for (size_t i = 0; i < m_count; ++i)
            {
                double g = 0, mm = 0;
                for (int j = 0; j < m_frameLen; ++j)
                {
                    const double x = m_mono[i * (size_t) m_frameLen + (size_t) j];
                    const double sv = src.process (x);
                    env = cEnv * env + (1 - cEnv) * sv * sv;
                    const double a = airB.process (out.process (sv * sv / std::sqrt (env + 1.0e-20)));
                    const double m = midB.process (x);
                    g += a * a; mm += m * m;
                }
                if (powToDb (m_frames[i].a) >= floorDb) { genSum += w[i] * g; midSum += w[i] * mm; }
            }
            d.exciteGenDb = powToDb (genSum) - powToDb (midSum);
        }
        if (keyActive / active > 1.0e-9)   // beat above -90 dBFS A-weighted
        {
            d.hasBeat = true;
            d.beatLevelDb = powToDb (keyActive / active);
            d.larDb = d.vocalLevelDb - d.beatLevelDb;
        }
        if (st.size() >= 4)
        {
            std::sort (st.begin(), st.end());
            const auto pct = [&] (double q) { return st[(size_t) std::lround (q * (double) (st.size() - 1))]; };
            d.spreadDb = pct (0.9) - pct (0.1);
        }
        return d;
    }

    static double fsOf (const Diagnosis& d) { return d.sampleRate; }

    static Settings settingsFor (const Diagnosis& d)
    {
        Settings s;
        if (! d.valid) return s;
        s.presenceDb = std::clamp (kRefPres - d.presDb, 0.0f, 6.0f);
        s.mudCutDb   = std::clamp (d.mudDb - kRefMud, 0.0f, 6.0f);
        s.airDb      = std::clamp (kRefAir - d.airDb, 0.0f, 4.0f);
        // LOW re MID in natural speech is about -10 dB (Byrne 1994); more low end means proximity / rumble.
        const float lowEx = d.lowDb - kLtassLow;
        s.hpfHz = lowEx > 6.0f ? 120.0f : lowEx > 3.0f ? 100.0f : lowEx < -6.0f ? 70.0f : 80.0f;
        // 1 at 3 dB under the reference spread, +0.5 per 6 dB more (max 1.6). It starts early because the tone moves
        // widen the spread themselves: in real takes the quiet frames are the mud-heavy ones (bleed, hum, nasals: MUD
        // +11 dB re MID in the quietest 10 % of one take against +3 dB in the loudest), so the mud cut deepens them.
        s.compNeed = std::clamp (1.0f + (d.spreadDb - (kRefSpreadDb - 3.0f)) / 12.0f, 0.6f, 1.6f);
        // Band limit: empty above 11 kHz -> limited; empty from 8 kHz too -> the cut is 8 kHz. The exciter squares the
        // octave under the cut and divides by its RMS: a sine comes out as its octave 3 dB under it, so the starting
        // gain puts AIR at the target from the probe under the cut.
        if (d.cutHz > 2000.0f && d.cutHz < std::min (kFullBandHz, 0.9 * 0.5 * fsOf (d)))
        {
            s.bandLimited = true;
            s.cutHz = d.cutHz;
            s.exciteDb = std::clamp ((kRefAir - kExciteTargetBelowRefDb) - d.exciteGenDb, -20.0f, 40.0f);
            s.airDb = 0.0f;      // the shelf has nothing to lift
        }
        return s;
    }

private:
    struct Frame { double a = 0, key = 0, low = 0, mud = 0, mid = 0, pres = 0, sib = 0, air = 0; };

    static constexpr int kFftN = 4096;
    static constexpr int kPhraseHalfFrames = 5;   // phrase level for the tone weights: 11 frames, 1.1 s
    void addSpectrum()
    {
        for (int i = 0; i < kFftN; ++i) { m_re[(size_t) i] = m_fftIn[(size_t) i] * m_win[(size_t) i]; m_im[(size_t) i] = 0; }
        fftInPlace (m_re.data(), m_im.data(), kFftN);
        for (int k = 0; k <= kFftN / 2; ++k) m_spec[(size_t) k] += m_re[(size_t) k] * m_re[(size_t) k] + m_im[(size_t) k] * m_im[(size_t) k];
        ++m_specFrames; m_fftPos = 0;
    }
    std::vector<double> m_fftIn, m_re, m_im, m_spec, m_win;
    int m_fftPos = 0, m_specFrames = 0;
    std::vector<float> m_mono;
    size_t m_monoPos = 0;

    double m_fs = 48000;
    int m_frameLen = 4800, m_pos = 0;
    size_t m_count = 0;
    std::vector<Frame> m_frames;
    Frame m_acc;
    AWeighting aw, awKey;
    BandFilter low, mud, mid, pres, sib, air;
};

} // namespace svl::boeb
