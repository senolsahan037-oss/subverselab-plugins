/*  MIHENK console tool. Drives the same engine the app uses.

        render --selftest
            Measurement, chain and export checks against known signals.
            Exit code 0 only if every check passes.

        render --measure <file.wav>
            Prints the input measurement.

        render --master <in.wav> <out.wav> [--target -14] [--ceiling -1] [--bits 24|16]
               [--genre rap|arabesk|electronic] [--reference ref.wav] [--tone 0-100]
               [--linear] [--warn 6] [--no-match] [--drive dB]
            Masters a file: optional genre starting point, optional tone match to a
            reference, MATCH TARGET, then exports the WAV and a report next to it.
*/
#include <JuceHeader.h>
#include "../Source/engine/MasterChain.h"
#include "../Source/engine/AudioIO.h"
#include "../Source/engine/Entitlement.h"
#include "../Source/engine/Preset.h"
#include "../Source/engine/Diagnosis.h"
#include "../Source/engine/RtChain.h"
#include "../Source/engine/Batch.h"
#include "../Source/dsp/Equalizer.h"
#include "../Source/dsp/Spectrum.h"
#include "../Source/dsp/Filters.h"
#include "../Source/app/ValueParse.h"

using namespace mihenk;

namespace
{
int failures = 0;
void check (bool ok, const juce::String& what)
{
    std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8());
    std::fflush (stdout);
    if (! ok) ++failures;
}
void section (const char* name) { std::printf ("\n-- %s\n", name); }

using Buffer = juce::AudioBuffer<float>;

/** Stereo (or mono) sine of the given peak level. */
Buffer sine (double fs, double secs, double dbfs, double freq, int chans = 2, double phase = 0.0)
{
    const int n = (int) std::lround (secs * fs);
    Buffer b (chans, n);
    const double a = dbToGain (dbfs);
    for (int i = 0; i < n; ++i)
    {
        const float v = (float) (a * std::sin (2.0 * kPi * freq * i / fs + phase));
        for (int c = 0; c < chans; ++c) b.setSample (c, i, v);
    }
    return b;
}

/** 20 ms raised-cosine fades, so a test tone has no switch-on ringing (Gibbs) in the true-peak reading. */
Buffer faded (Buffer b, double fs)
{
    const int f = juce::jmin (b.getNumSamples() / 2, (int) (0.02 * fs));
    for (int i = 0; i < f; ++i)
    {
        const float w = (float) (0.5 - 0.5 * std::cos (kPi * i / f));
        for (int c = 0; c < b.getNumChannels(); ++c)
        {
            b.setSample (c, i, b.getSample (c, i) * w);
            b.setSample (c, b.getNumSamples() - 1 - i, b.getSample (c, b.getNumSamples() - 1 - i) * w);
        }
    }
    return b;
}

Buffer concat (std::initializer_list<Buffer> parts)
{
    int n = 0, chans = 0;
    for (auto& p : parts) { n += p.getNumSamples(); chans = p.getNumChannels(); }
    Buffer out (chans, n);
    int pos = 0;
    for (auto& p : parts)
    {
        for (int c = 0; c < chans; ++c) out.copyFrom (c, pos, p, c, 0, p.getNumSamples());
        pos += p.getNumSamples();
    }
    return out;
}

/** Noise with a power spectrum of f^-1 (pink) times a tilt of tiltDbPerOct around 1 kHz, synthesised in the
    frequency domain so the slope is exact. Independent per channel. RMS set to rmsDb. */
Buffer shapedNoise (double fs, double secs, double tiltDbPerOct, double rmsDb, uint32_t seed, int chans = 2)
{
    const int n = (int) std::lround (secs * fs);
    int N = 1;
    while (N < n) N <<= 1;
    Buffer b (chans, n);
    juce::Random rng ((juce::int64) seed);
    for (int c = 0; c < chans; ++c)
    {
        std::vector<std::complex<double>> x ((size_t) N);
        for (int k = 1; k < N / 2; ++k)
        {
            const double f = (double) k * fs / N;
            const double magDb = -10.0 * std::log10 (f / 1000.0) + tiltDbPerOct * std::log2 (juce::jlimit (20.0, 20000.0, f) / 1000.0);
            const double m = f < 10.0 ? 0.0 : std::pow (10.0, magDb / 20.0);
            const double ph = rng.nextDouble() * 2.0 * kPi;
            x[(size_t) k] = std::polar (m, ph);
            x[(size_t) (N - k)] = std::conj (x[(size_t) k]);
        }
        fftInPlace (x, true);
        double ss = 0.0;
        for (int i = 0; i < n; ++i) ss += x[(size_t) i].real() * x[(size_t) i].real();
        const double g = dbToGain (rmsDb) / std::sqrt (ss / n);
        for (int i = 0; i < n; ++i) b.setSample (c, i, (float) (x[(size_t) i].real() * g));
    }
    return b;
}

/** A music-like test signal: kick, bass, hats and a pad, with a quiet verse and a loud chorus. */
Buffer musicLike (double fs, double secs, uint32_t seed)
{
    const int n = (int) std::lround (secs * fs);
    Buffer b (2, n);
    auto pad = shapedNoise (fs, secs, -1.0, -30.0, seed);
    juce::Random rng ((juce::int64) seed + 7);
    const double beat = 0.5;
    for (int i = 0; i < n; ++i)
    {
        const double t = i / fs;
        const double section = std::fmod (t, 16.0) < 8.0 ? 0.35 : 1.0;   // verse / chorus
        const double tb = std::fmod (t, beat);
        const double kick = std::exp (-tb * 18.0) * std::sin (2.0 * kPi * (48.0 * tb + 60.0 * (1.0 - std::exp (-tb * 30.0)) / 30.0));
        const double bass = 0.25 * std::sin (2.0 * kPi * 55.0 * t) * (0.6 + 0.4 * std::sin (2.0 * kPi * 0.5 * t));
        const double th = std::fmod (t + 0.25, beat * 0.5);
        const double hat = std::exp (-th * 60.0) * (rng.nextDouble() * 2.0 - 1.0) * 0.15;
        const double mono = section * (0.55 * kick + bass) + hat * section;
        b.setSample (0, i, (float) (mono + section * pad.getSample (0, i) * 3.0 + hat * 0.3));
        b.setSample (1, i, (float) (mono + section * pad.getSample (1, i) * 3.0 - hat * 0.3));
    }
    return b;
}

double rmsDbOf (const Buffer& b, int ch, int from, int to)
{
    double s = 0.0;
    for (int i = from; i < to; ++i) s += (double) b.getSample (ch, i) * b.getSample (ch, i);
    return 10.0 * std::log10 (s / (to - from) + 1.0e-30);
}

/** Independent 16x true-peak check: a longer, different interpolator than the meter's. */
double truePeak16x (const Buffer& b)
{
    constexpr int P = 16, T = 64;
    static float c[P][T];
    static bool init = false;
    if (! init)
    {
        for (int p = 0; p < P; ++p)
        {
            double sum = 0.0, tmp[T];
            for (int j = 0; j < T; ++j)
            {
                const double t = (double) p / P + T / 2.0 - 1.0 - j;
                const double s = std::abs (t) < 1e-12 ? 1.0 : std::sin (kPi * t) / (kPi * t);
                tmp[j] = s * kaiser (t / (T / 2.0), 9.0);
                sum += tmp[j];
            }
            for (int j = 0; j < T; ++j) c[p][j] = (float) (tmp[j] / sum);
        }
        init = true;
    }
    float peak = 0.0f;
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
    {
        const int n = b.getNumSamples();
        std::vector<float> xp ((size_t) (n + T + 1), 0.0f);
        std::copy (b.getReadPointer (ch), b.getReadPointer (ch) + n, xp.begin() + (T / 2 - 1));
        for (int i = 0; i < n; ++i)
        {
            peak = juce::jmax (peak, std::abs (b.getSample (ch, i)));
            if (std::abs (b.getSample (ch, i)) < peak * 0.5f) continue;   // intersample peaks sit next to large samples
            for (int p = 1; p < P; ++p)
            {
                float acc = 0.0f;
                for (int j = 0; j < T; ++j) acc += c[p][j] * xp[(size_t) (i + j)];
                peak = juce::jmax (peak, std::abs (acc));
            }
        }
    }
    return gainToDb (peak);
}

double eqGainAt (const EqSettings& e, double fs, double f)
{
    auto in = sine (fs, 1.5, -12.0, f, 1);
    auto out = in;
    convolveInPlace (out, designEqFir (e, fs));
    const int a = (int) (0.5 * fs), z = (int) (1.0 * fs);
    return rmsDbOf (out, 0, a, z) - rmsDbOf (in, 0, a, z);
}

bool identical (const Buffer& a, const Buffer& b)
{
    if (a.getNumChannels() != b.getNumChannels() || a.getNumSamples() != b.getNumSamples()) return false;
    for (int c = 0; c < a.getNumChannels(); ++c)
        if (std::memcmp (a.getReadPointer (c), b.getReadPointer (c), sizeof (float) * (size_t) a.getNumSamples()) != 0) return false;
    return true;
}


//==============================================================================
// v2 helpers: a clean mix for the diagnosis and its faulty variants.

struct MixSpec
{
    double secs = 20.0;
    double verseGain = 0.5;          // quiet sections (8 s verse / 8 s chorus)
    bool lowInVerse = true;          // kick and bass play in the verse
    double stereo = 0.5;             // independent share of the pad per channel
    uint32_t seed = 1;
    double verseLowGain = 1.0;       // kick and bass level in the verse (on top of verseGain)
};

/** A mix that no diagnosis rule should flag: kick, moving bass line, hats, a pink-ish stereo pad, verse / chorus,
    sample peak -6 dBFS. */
Buffer cleanMix (double fs, const MixSpec& ms = {})
{
    const int n = (int) std::lround (ms.secs * fs);
    Buffer b (2, n);
    auto common = shapedNoise (fs, ms.secs, -1.0, -24.0, ms.seed, 1);
    auto indep = shapedNoise (fs, ms.secs, -1.0, -24.0, ms.seed + 100, 2);
    juce::Random rng ((juce::int64) ms.seed + 7);
    const double notes[] { 55.0, 49.0, 65.41, 73.42 };
    double bassPhase = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double t = i / fs;
        const bool verse = std::fmod (t, 16.0) < 8.0;
        const double sec = verse ? ms.verseGain : 1.0;
        const double lowOn = verse ? (ms.lowInVerse ? ms.verseLowGain : 0.0) : 1.0;
        const double tb = std::fmod (t, 0.5);
        const double kick = std::exp (-tb * 18.0) * std::sin (2.0 * kPi * (48.0 * tb + 60.0 * (1.0 - std::exp (-tb * 30.0)) / 30.0));
        bassPhase += 2.0 * kPi * notes[(int) (t / 2.0) % 4] / fs;
        const double bass = 0.22 * std::sin (bassPhase);
        const double th = std::fmod (t + 0.125, 0.25);
        const double hat = std::exp (-th * 70.0) * (rng.nextDouble() * 2.0 - 1.0) * 0.08;
        const double mono = sec * (lowOn * (0.5 * kick + bass) + hat + 2.5 * common.getSample (0, i));
        b.setSample (0, i, (float) (mono + sec * (2.5 * ms.stereo * indep.getSample (0, i) + 0.2 * hat)));
        b.setSample (1, i, (float) (mono + sec * (2.5 * ms.stereo * indep.getSample (1, i) - 0.2 * hat)));
    }
    for (int c = 0; c < 2; ++c)
    {
        // The kick's decaying sweep has a small DC component; a real mix bus does not.
        double mean = 0.0;
        for (int i = 0; i < n; ++i) mean += b.getSample (c, i);
        mean /= n;
        for (int i = 0; i < n; ++i) b.setSample (c, i, (float) (b.getSample (c, i) - mean));
    }
    b.applyGain ((float) (dbToGain (-6.0) / b.getMagnitude (0, n)));
    return b;
}

void normalisePeak (Buffer& b, double dbfs) { b.applyGain ((float) (dbToGain (dbfs) / b.getMagnitude (0, b.getNumSamples()))); }

void applyEq (Buffer& b, double fs, const std::function<void (EqSettings&)>& set)
{
    EqSettings e;
    set (e);
    convolveInPlace (b, designEqFir (e, fs));
}

/** Crest factor (sample peak - RMS, dB) of one band of a 3-band split. */
double bandCrest (const Buffer& x, double fs, int band, double f1, double f2)
{
    Buffer lo, mi, hi;
    splitThreeBands (x, fs, f1, f2, lo, mi, hi);
    const Buffer& b = band == 0 ? lo : band == 1 ? mi : hi;
    return analyse (b, fs, false).crest;
}

/** Level (dB) of a sine of frequency f after a buffer went through `process`, relative to before (seconds 1..2). */
double sineGainThrough (double fs, double f, const std::function<void (Buffer&)>& process)
{
    auto in = sine (fs, 3.0, -12.0, f);
    auto out = in;
    process (out);
    return rmsDbOf (out, 0, (int) fs, 2 * (int) fs) - rmsDbOf (in, 0, (int) fs, 2 * (int) fs);
}


/** The synthetic case of each diagnosis scenario (a clean mix with one fault), as the selftest uses it. */
Buffer scenarioSignal (int sc, double fs)
{
    auto mk = [fs] (std::function<void (Buffer&)> f, MixSpec ms = {}) { return [f, ms, fs] { auto b = cleanMix (fs, ms); f (b); return b; }; };
    switch (sc)
    {
        case scTooHot: return mk ([] (Buffer& b) { normalisePeak (b, -0.1); })();
        case scClipped: return mk ([] (Buffer& b) { b.applyGain (4.0f); for (int c = 0; c < 2; ++c) for (int i = 0; i < b.getNumSamples(); ++i) b.setSample (c, i, juce::jlimit (-0.98f, 0.98f, b.getSample (c, i))); })();
        case scTooQuiet: return mk ([] (Buffer& b) { normalisePeak (b, -24.0); })();
        case scPhase: return mk ([] (Buffer& b) { auto lo = zeroPhaseLowPass (b.getReadPointer (1), b.getNumSamples(), 48000.0, 150.0);
                                            for (int i = 0; i < b.getNumSamples(); ++i) b.setSample (1, i, (float) (b.getSample (1, i) - 2.0 * lo[(size_t) i])); })();
        case scTooWide: return mk ([] (Buffer& b) { auto l = b; for (int i = 0; i < b.getNumSamples(); ++i) { const double m = 0.5 * (l.getSample (0, i) + l.getSample (1, i)), sd = 0.5 * (l.getSample (0, i) - l.getSample (1, i));
                                                  b.setSample (0, i, (float) m); b.setSample (1, i, (float) sd); }
                                              auto hs = zeroPhaseLowPass (b.getReadPointer (1), b.getNumSamples(), 48000.0, 3000.0);
                                              for (int i = 0; i < b.getNumSamples(); ++i) { const double sd = b.getSample (1, i) + 3.0 * (b.getSample (1, i) - hs[(size_t) i]), m = b.getSample (0, i);
                                                  b.setSample (0, i, (float) (m + sd)); b.setSample (1, i, (float) (m - sd)); } })();
        case scTooNarrow: return mk ([] (Buffer& b) { for (int i = 0; i < b.getNumSamples(); ++i) { const double m = 0.5 * (b.getSample (0, i) + b.getSample (1, i)), sd = 0.05 * 0.5 * (b.getSample (0, i) - b.getSample (1, i));
                                                  b.setSample (0, i, (float) (m + sd)); b.setSample (1, i, (float) (m - sd)); } })();
        case scImbalance: return mk ([] (Buffer& b) { b.applyGain (1, 0, b.getNumSamples(), (float) dbToGain (-2.5)); })();
        case scDcOffset: return mk ([] (Buffer& b) { for (int c = 0; c < 2; ++c) for (int i = 0; i < b.getNumSamples(); ++i) b.setSample (c, i, b.getSample (c, i) + 0.01f); })();
        case scMuddy: return mk ([] (Buffer& b) { applyEq (b, 48000.0, [] (EqSettings& e) { e.bell1Db = 9.0; e.bell1Hz = 300.0; e.bell1Q = 0.8; }); })();
        case scBoomy: return mk ([] (Buffer& b) { applyEq (b, 48000.0, [] (EqSettings& e) { e.lowShelfDb = 6.0; e.lowShelfHz = 45.0; e.bell1Db = 8.0; e.bell1Hz = 45.0; e.bell1Q = 1.2; }); })();
        case scThin: return mk ([] (Buffer& b) { applyEq (b, 48000.0, [] (EqSettings& e) { e.lowCutOn = true; e.lowCutHz = 200.0; e.lowCutSlope = 12; }); })();
        case scHarsh: return mk ([] (Buffer& b) { applyEq (b, 48000.0, [] (EqSettings& e) { e.bell1Db = 9.0; e.bell1Hz = 3500.0; e.bell1Q = 0.8; }); })();
        case scSibilance: return mk ([] (Buffer& b) {
                  auto nz = shapedNoise (48000.0, 20.0, 0.0, -3.0, 91, 1);     // a centred (vocal) ess: the same in both channels
                  Buffer lo, mi, hi;
                  splitThreeBands (nz, 48000.0, 5000.0, 9000.0, lo, mi, hi);
                  for (int i = 0; i < b.getNumSamples(); ++i)
                      if (std::fmod (i / 48000.0, 0.9) < 0.1)
                          for (int c = 0; c < 2; ++c) b.setSample (c, i, b.getSample (c, i) + mi.getSample (0, i)); })();
        case scDull: return mk ([] (Buffer& b) { applyEq (b, 48000.0, [] (EqSettings& e) { e.highShelfDb = -10.0; e.highShelfHz = 4000.0; e.bell1Db = -10.0; e.bell1Hz = 13000.0; e.bell1Q = 0.7; }); })();
        case scBuriedVocal: return mk ([] (Buffer& b) { applyEq (b, 48000.0, [] (EqSettings& e) { e.bell1Db = -9.0; e.bell1Hz = 2000.0; e.bell1Q = 0.8; }); })();
        case scOverCompressed: return mk ([] (Buffer& b) { auto env = truePeakEnvelope (b, kLimiterOversampling);
                                                     LimiterSettings ls; ls.ceilingDbtp = -1.0; ls.releaseMs = 15.0; ls.lookaheadMs = 0.5; ls.driveDb = 12.0;
                                                     Buffer o; limitWithSafety (b, env, 48000.0, ls, o); b = o; })();
        case scTooDynamic: return mk ([] (Buffer&) {}, MixSpec { 32.0, 0.12, true, 0.5, 1 })();
        case scUnevenLow: return mk ([] (Buffer&) {}, MixSpec { 32.0, 1.0, true, 0.5, 1, 0.12 })();
        case scResonance: return mk ([] (Buffer& b) { auto t = sine (48000.0, 20.0, -24.0, 1200.0); for (int c = 0; c < 2; ++c) b.addFrom (c, 0, t, c, 0, b.getNumSamples()); normalisePeak (b, -6.0); })();
        case scCodecRisk: return mk ([] (Buffer& b) { b.applyGain ((float) (dbToGain (-0.6) / truePeakMax (b))); })();
        default: return cleanMix (fs);
    }
}

int selftest()
{
    const double fs = 48000.0;

    section ("Loudness: EBU Tech 3341 (stereo 1 kHz sine, expected values from the Tech 3341 table)");
    {
        struct Case { const char* name; Buffer sig; double expect; };
        Case cases[] = {
            { "Case 1: -23 dBFS, 20 s", sine (fs, 20, -23, 1000), -23.0 },
            { "Case 2: -33 dBFS, 20 s", sine (fs, 20, -33, 1000), -33.0 },
            { "Case 3: -36 / -23 / -36 dBFS, 10 / 60 / 10 s", concat ({ sine (fs, 10, -36, 1000), sine (fs, 60, -23, 1000), sine (fs, 10, -36, 1000) }), -23.0 },
            { "Case 4: -72 / -36 / -23 / -36 / -72 dBFS", concat ({ sine (fs, 10, -72, 1000), sine (fs, 10, -36, 1000), sine (fs, 60, -23, 1000),
                                                                   sine (fs, 10, -36, 1000), sine (fs, 10, -72, 1000) }), -23.0 },
            { "Case 5: -26 / -20 / -26 dBFS, 20 / 20.1 / 20 s", concat ({ sine (fs, 20, -26, 1000), sine (fs, 20.1, -20, 1000), sine (fs, 20, -26, 1000) }), -23.0 },
        };
        for (auto& c : cases)
        {
            const double l = integratedLoudness (c.sig, fs);
            check (std::abs (l - c.expect) <= 0.1, juce::String (c.name) + ": I = " + fmt (l, 2) + " LUFS (expect " + fmt (c.expect) + " +-0.1)");
        }
        // Steady tone: momentary and short-term equal the integrated value (Tech 3341 case 1 lists M, S and I = -23.0).
        const auto r = measureLoudness (cases[0].sig, fs);
        check (std::abs (r.momentaryMax + 23.0) <= 0.1 && std::abs (r.shortTermMax + 23.0) <= 0.1,
               "Case 1: momentary max " + fmt (r.momentaryMax, 2) + ", short-term max " + fmt (r.shortTermMax, 2) + " LUFS (expect -23.0)");
        for (double rate : { 44100.0, 96000.0 })
        {
            const double l = integratedLoudness (sine (rate, 20, -23, 1000), rate);
            check (std::abs (l + 23.0) <= 0.1, "Case 1 at " + fmt (rate / 1000.0, 1) + " kHz: " + fmt (l, 2) + " LUFS");
        }
        // The brief's example, stated correctly: a -20 dBFS stereo sine is -20.0 LUFS (by case 1, +3 dB);
        // -23 LUFS is what a -20 dBFS sine reads as a single (mono) channel.
        const double st = integratedLoudness (sine (fs, 20, -20, 1000), fs);
        const double mo = integratedLoudness (sine (fs, 20, -20, 1000, 1), fs);
        check (std::abs (st + 20.0) <= 0.1, "-20 dBFS stereo sine: " + fmt (st, 2) + " LUFS (expect -20.0)");
        check (std::abs (mo + 23.0) <= 0.1, "-20 dBFS mono sine (one channel, BS.1770 weight 1.0): " + fmt (mo, 2) + " LUFS (expect -23.0)");
        const double quiet = integratedLoudness (sine (fs, 5, -80, 1000), fs);
        check (! std::isfinite (quiet), "Signal under the -70 LUFS absolute gate reads -inf: " + fmt (quiet));
    }

    section ("Loudness range: EBU Tech 3342 (expected LRA +-1 LU)");
    {
        struct Case { const char* name; Buffer sig; double expect; };
        Case cases[] = {
            { "Case 1: -20 / -30 dBFS, 20 s each", concat ({ sine (fs, 20, -20, 1000), sine (fs, 20, -30, 1000) }), 10.0 },
            { "Case 2: -20 / -15 dBFS", concat ({ sine (fs, 20, -20, 1000), sine (fs, 20, -15, 1000) }), 5.0 },
            { "Case 3: -40 / -20 dBFS", concat ({ sine (fs, 20, -40, 1000), sine (fs, 20, -20, 1000) }), 20.0 },
            { "Case 4: -50 / -35 / -20 / -35 / -50 dBFS", concat ({ sine (fs, 20, -50, 1000), sine (fs, 20, -35, 1000), sine (fs, 20, -20, 1000),
                                                                   sine (fs, 20, -35, 1000), sine (fs, 20, -50, 1000) }), 15.0 },
        };
        for (auto& c : cases)
        {
            const double lra = measureLoudness (c.sig, fs).lra;
            check (std::abs (lra - c.expect) <= 1.0, juce::String (c.name) + ": LRA " + fmt (lra, 2) + " LU (expect " + fmt (c.expect, 0) + ")");
        }
    }

    section ("True peak (4x oversampling)");
    {
        // fs/4 sine, 45 degree phase: every sample sits at 0.707 of the peak, the peak is between samples.
        auto s = faded (sine (fs, 2, -6.0206, fs / 4.0, 2, kPi / 4.0), fs);
        const auto m = analyse (s, fs, false);
        check (std::abs (m.samplePeak + 9.03) < 0.05, "fs/4 sine at 45 deg: sample peak " + fmt (m.samplePeak, 2) + " dBFS (expect -9.03)");
        check (std::abs (m.truePeak + 6.02) <= 0.2, "fs/4 sine at 45 deg: true peak " + fmt (m.truePeak, 2) + " dBTP (expect -6.02, BS.1770 tolerance +0.2/-0.4)");
        // Tech 3341 style: 0 dBFS-peak sine at fs/4 with 45 deg reads +0 dBTP although no sample exceeds -3 dBFS.
        auto s2 = faded (sine (fs, 2, 0.0, fs / 4.0, 2, kPi / 4.0), fs);
        const auto m2 = analyse (s2, fs, false);
        check (std::abs (m2.truePeak) <= 0.2 && m2.samplePeak < -2.9, "Full-scale inter-sample peak: sample " + fmt (m2.samplePeak, 2) + " dBFS, true "
               + fmt (m2.truePeak, 2) + " dBTP (expect 0.0)");
        auto s3 = faded (sine (44100.0, 2, -3.0, 997.0), 44100.0);
        const auto m3 = analyse (s3, 44100.0, false);
        check (std::abs (m3.truePeak + 3.0) < 0.05 && m3.truePeak >= m3.samplePeak, "997 Hz sine at -3 dBFS, 44.1 kHz: " + fmt (m3.truePeak, 3) + " dBTP");
    }

    section ("Level, correlation, spectrum");
    {
        const auto m = analyse (sine (fs, 5, -6, 1000), fs);
        check (std::abs (m.rms + 9.01) < 0.02 && std::abs (m.crest - 3.01) < 0.02, "Sine -6 dBFS: RMS " + fmt (m.rms, 2) + " dBFS, crest " + fmt (m.crest, 2) + " dB");
        check (std::abs (m.correlation - 1.0) < 1e-6, "Identical channels: correlation " + fmt (m.correlation, 3));
        auto anti = sine (fs, 5, -6, 1000);
        anti.applyGain (1, 0, anti.getNumSamples(), -1.0f);
        check (std::abs (analyse (anti, fs, false).correlation + 1.0) < 1e-6, "Inverted channel: correlation -1");
        auto pink = shapedNoise (fs, 20, 0.0, -20.0, 42);
        const auto ps = analyse (pink, fs).spectrumDb;
        const double lo = meanInRange (ps, 50.0, 200.0), hi = meanInRange (ps, 3000.0, 12000.0);
        check (std::abs (hi - lo) < 1.0, "Pink noise reads flat: 50-200 Hz " + fmt (lo, 1) + " dB, 3-12 kHz " + fmt (hi, 1) + " dB");
        const auto& grid = spectrumGrid();
        const auto ss = analyse (sine (fs, 5, -6, 1000), fs).spectrumDb;
        size_t k1 = 0;
        for (size_t i = 0; i < grid.size(); ++i) if (std::abs (std::log2 (grid[i] / 1000.0)) < std::abs (std::log2 (grid[k1] / 1000.0))) k1 = i;
        check (std::abs (ss[k1] - (-6.0 - 3.01)) < 0.5, "1 kHz sine -6 dBFS in its band: " + fmt (ss[k1], 2) + " dB (expect -9.03)");
    }

    section ("EQ: response at known frequencies (analogue prototype, no cramping)");
    {
        for (bool lin : { false, true })
        {
            const juce::String mode = lin ? "linear" : "minimum";
            EqSettings e;
            e.linearPhase = lin;
            e.bell1Db = 6.0; e.bell1Hz = 1000.0; e.bell1Q = 1.0;
            bool ok = true;
            juce::String list;
            for (double f : { 250.0, 700.0, 1000.0, 1414.0, 4000.0 })
            {
                const double got = eqGainAt (e, fs, f), want = eqManualDb (e, f);
                ok &= std::abs (got - want) < 0.1;
                list << fmt (f, 0) << " Hz " << fmt (got, 2) << "/" << fmt (want, 2) << "  ";
            }
            check (ok && std::abs (eqGainAt (e, fs, 1000.0) - 6.0) < 0.05, "Bell +6 dB 1 kHz Q1, " + mode + " phase: " + list);

            EqSettings hi;
            hi.linearPhase = lin;
            hi.bell1Db = 6.0; hi.bell1Hz = 16000.0; hi.bell1Q = 2.0;
            const double at16 = eqGainAt (hi, 44100.0, 16000.0), at20 = eqGainAt (hi, 44100.0, 20000.0), want20 = eqManualDb (hi, 20000.0);
            check (std::abs (at16 - 6.0) < 0.15 && std::abs (at20 - want20) < 0.2,
                   "Bell +6 dB 16 kHz Q2 at 44.1 kHz, " + mode + ": 16 kHz " + fmt (at16, 2) + " dB, 20 kHz " + fmt (at20, 2) + " dB (analogue " + fmt (want20, 2) + ")");

            EqSettings lc;
            lc.linearPhase = lin;
            lc.lowCutOn = true; lc.lowCutHz = 100.0; lc.lowCutSlope = 24;
            const double c100 = eqGainAt (lc, fs, 100.0), c50 = eqGainAt (lc, fs, 50.0), c1k = eqGainAt (lc, fs, 1000.0);
            check (std::abs (c100 + 3.01) < 0.15 && std::abs (c50 - eqManualDb (lc, 50.0)) < 0.3 && std::abs (c1k) < 0.05,
                   "Low cut 100 Hz 24 dB/oct, " + mode + ": 100 Hz " + fmt (c100, 2) + ", 50 Hz " + fmt (c50, 2) + " (analogue " + fmt (eqManualDb (lc, 50.0), 2) + "), 1 kHz " + fmt (c1k, 3));

            EqSettings sh;
            sh.linearPhase = lin;
            sh.lowShelfDb = -4.0; sh.lowShelfHz = 100.0; sh.highShelfDb = 3.0; sh.highShelfHz = 8000.0; sh.tiltDbPerOct = 0.5;
            bool okS = true;
            for (double f : { 30.0, 100.0, 1000.0, 8000.0, 18000.0 })
                okS &= std::abs (eqGainAt (sh, fs, f) - eqManualDb (sh, f)) < 0.12;
            check (okS, "Shelves + tilt, " + mode + ": within 0.12 dB of the analogue response at 30 Hz .. 18 kHz");
        }
        // Linear phase keeps the time alignment: an impulse comes out where it went in, symmetric.
        EqSettings e;
        e.linearPhase = true;
        e.bell1Db = 4.0; e.bell1Hz = 2000.0;
        Buffer imp (1, 48000);
        imp.clear();
        imp.setSample (0, 20000, 1.0f);
        convolveInPlace (imp, designEqFir (e, fs));
        int peak = 0;
        for (int i = 0; i < imp.getNumSamples(); ++i) if (std::abs (imp.getSample (0, i)) > std::abs (imp.getSample (0, peak))) peak = i;
        check (peak == 20000 && std::abs (imp.getSample (0, 19990) - imp.getSample (0, 20010)) < 1e-5f, "Linear phase: impulse stays at its sample, symmetric");
    }

    section ("Bypass");
    {
        auto in = musicLike (fs, 6, 3);
        in.applyGain (0.3f);
        MasterSettings s;
        s.eq.bell1Db = 3.0; s.comp.enabled = true; s.limiter.driveDb = 6.0;
        s.bypass = true;
        PreLimiterCache cache;
        auto r = MasterChain::render (in, fs, s, cache);
        check (identical (*r.output, in), "BYPASS: output is the input, bit for bit");
        MasterSettings neutral;   // EQ flat, compressor off, width 100 %, limiter on but nothing over the ceiling, drive 0
        PreLimiterCache c2;
        auto r2 = MasterChain::render (in, fs, neutral, c2);
        check (identical (*r2.output, in) && r2.limiter.gr.maxDb == 0.0, "Neutral chain (limiter in, below ceiling): bit-exact");
        auto mono = sine (fs, 3, -12, 440, 1);
        PreLimiterCache c3;
        auto r3 = MasterChain::render (mono, fs, neutral, c3);
        check (identical (*r3.output, mono), "Neutral chain on a mono file: bit-exact");
    }

    section ("True-peak limiter never exceeds the ceiling");
    {
        for (double rate : { 44100.0, 48000.0, 96000.0 })
        {
            auto in = concat ({ musicLike (rate, 8, 11), sine (rate, 1, -3.0, rate / 4.0, 2, kPi / 4.0), sine (rate, 1, 0.0, rate * 0.23, 2, 0.3) });
            MasterSettings s;
            s.limiter.driveDb = 14.0;
            s.limiter.ceilingDbtp = -1.0;
            PreLimiterCache cache;
            auto r = MasterChain::render (in, rate, s, cache);
            const double tp4 = r.measured.truePeak, tp16 = truePeak16x (*r.output);
            check (tp4 <= -1.0 + 1e-4 && tp16 <= -1.0 + 0.05, fmt (rate / 1000.0, 1) + " kHz, +14 dB drive: true peak " + fmt (tp4, 3) + " dBTP (4x), "
                   + fmt (tp16, 3) + " dBTP (independent 16x), ceiling -1.0; " + juce::String (r.limiter.passes) + " pass(es), trim " + fmt (r.limiter.finalTrimDb, 3) + " dB");
        }
        auto in = musicLike (fs, 8, 5);
        for (double ceil : { -0.3, -2.0 })
        {
            MasterSettings s;
            s.limiter.driveDb = 10.0;
            s.limiter.ceilingDbtp = ceil;
            s.limiter.lookaheadMs = 0.5;
            s.limiter.releaseMs = 20.0;
            PreLimiterCache cache;
            auto r = MasterChain::render (in, fs, s, cache);
            check (r.measured.truePeak <= ceil + 1e-4, "Ceiling " + fmt (ceil, 1) + " dBTP, fast settings: " + fmt (r.measured.truePeak, 3) + " dBTP");
        }
    }

    section ("MATCH TARGET (within 0.2 LU, ceiling held)");
    {
        struct Sig { const char* name; Buffer b; };
        Sig sigs[] = { { "pink noise", shapedNoise (fs, 20, 0.0, -30.0, 9) }, { "music-like", musicLike (fs, 32, 21) } };
        for (auto& sg : sigs)
            for (double target : { -16.0, -14.0, -11.0 })
            {
                MasterSettings s;
                s.targetLufs = target;
                s.comp.enabled = true;
                PreLimiterCache cache;
                RenderResult r;
                const auto m = MasterChain::matchTarget (sg.b, fs, s, cache, &r);
                check (m.reached && std::abs (r.measured.integrated - target) <= 0.2 && r.measured.truePeak <= -1.0 + 1e-4,
                       juce::String (sg.name) + ", target " + fmt (target, 0) + ": " + fmt (r.measured.integrated, 2) + " LUFS, " + fmt (r.measured.truePeak, 2)
                       + " dBTP, drive " + fmt (m.driveDb, 2) + " dB, peak limiting " + fmt (m.maxLimitingDb, 1) + " dB, " + juce::String (m.iterations) + " solver steps"
                       + (r.warnings.isEmpty() ? "" : "  [warns]"));
            }
        // Club loudness is reachable with club settings (fast limiter release).
        {
            MasterSettings s;
            s.targetPreset = TargetPreset::club;
            s.targetLufs = presetLufs (s.targetPreset, 0.0);
            s.comp.enabled = true;
            s.limiter.releaseMs = 30.0;
            s.limiter.lookaheadMs = 1.0;
            PreLimiterCache cache;
            RenderResult r;
            auto sig = shapedNoise (fs, 20, 0.0, -24.0, 10);
            const auto m = MasterChain::matchTarget (sig, fs, s, cache, &r);
            check (m.reached && std::abs (r.measured.integrated + 8.0) <= 0.2 && r.measured.truePeak <= -1.0 + 1e-4,
                   "CLUB -8 on dense noise, release 30 ms: " + fmt (r.measured.integrated, 2) + " LUFS, " + fmt (r.measured.truePeak, 2) + " dBTP, peak limiting "
                   + fmt (m.maxLimitingDb, 1) + " dB; warnings: " + (r.warnings.isEmpty() ? juce::String ("none") : r.warnings.joinIntoString (" | ")));
        }
        // An unreachable target is reported as such, the ceiling still holds, and the heavy limiting is warned about.
        {
            MasterSettings s;
            s.targetLufs = -5.0;
            s.limitWarnDb = 6.0;
            PreLimiterCache cache;
            RenderResult r;
            auto sig = musicLike (fs, 20, 4);
            const auto m = MasterChain::matchTarget (sig, fs, s, cache, &r);
            bool warned = false;
            for (auto& w : r.warnings) warned |= w.contains ("warning level");
            check (! m.reached && m.message.contains ("not reachable") && m.message.contains ("diminishing returns")
                       && m.driveDb < 36.0 && warned && r.measured.truePeak <= -1.0 + 1e-4,
                   "Target -5 LUFS on a dynamic signal: not reached (best " + fmt (r.measured.integrated, 2) + " LUFS), backs off at diminishing returns, warns about "
                   + fmt (r.limiter.gr.maxDb, 1) + " dB of limiting, ceiling held (" + fmt (r.measured.truePeak, 2) + " dBTP)");
        }
    }

    section ("Tone match (pink source, reference tilted +1 dB/oct)");
    {
        auto src = shapedNoise (fs, 20, 0.0, -20.0, 77);
        auto ref = shapedNoise (fs, 20, 1.0, -18.0, 78);
        const auto srcSpec = analyse (src, fs).spectrumDb, refSpec = analyse (ref, fs).spectrumDb;
        auto residual = [&] (const std::vector<float>& outSpec, double& maxAbs)
        {
            auto d = MasterChain::differenceCurve (outSpec, refSpec, nullptr);
            const auto& grid = spectrumGrid();
            double ss = 0.0;
            int n = 0;
            maxAbs = 0.0;
            for (size_t i = 0; i < d.size(); ++i)
                if (grid[i] >= 60.0 && grid[i] <= 12000.0) { ss += d[i] * d[i]; ++n; maxAbs = juce::jmax (maxAbs, (double) std::abs (d[i])); }
            return std::sqrt (ss / n);
        };
        double before = 0.0, beforeMax = 0.0;
        before = residual (srcSpec, beforeMax);
        for (bool lin : { false, true })
        {
            EqSettings e;
            e.linearPhase = lin;
            e.matchCurveDb = MasterChain::toneMatchCurve (srcSpec, refSpec, e, 1.0);
            e.matchAmount = 1.0;
            auto out = src;
            convolveInPlace (out, designEqFir (e, fs));
            double afterMax = 0.0;
            const double after = residual (analyse (out, fs).spectrumDb, afterMax);
            check (before > 2.0 && after < 0.5 && afterMax < 1.5, juce::String (lin ? "Linear" : "Minimum") + " phase: difference to reference (60 Hz-12 kHz) RMS "
                   + fmt (before, 2) + " dB -> " + fmt (after, 2) + " dB, worst " + fmt (afterMax, 2) + " dB");
        }
        // Amount 50 % moves half way; the curve is limited to +-6 dB.
        EqSettings e;
        e.matchCurveDb = MasterChain::toneMatchCurve (srcSpec, refSpec, e, 1.0);
        float mx = 0.0f;
        for (float v : e.matchCurveDb) mx = juce::jmax (mx, std::abs (v));
        e.matchAmount = 0.5;
        const double half = eqTotalDb (e, 8000.0), full = interpGrid (e.matchCurveDb, 8000.0);
        check (mx <= 6.0f && std::abs (half - 0.5 * full) < 1e-9, "Curve limited to +-6 dB (max " + fmt (mx, 2) + "), amount 50 % gives half: " + fmt (half, 2) + " of " + fmt (full, 2) + " dB at 8 kHz");
    }

    section ("Compressor and stereo width");
    {
        auto in = sine (fs, 3, -10.0, 1000);
        CompSettings c;
        c.enabled = true; c.thresholdDb = -20.0; c.ratio = 2.0; c.kneeDb = 0.0; c.rmsBlend = 1.0; c.autoRelease = false; c.attackMs = 5; c.releaseMs = 50;
        GainReductionStats st;
        auto out = in;
        processCompressor (out, fs, c, st);
        const double gr = rmsDbOf (in, 0, (int) fs, 2 * (int) fs) - rmsDbOf (out, 0, (int) fs, 2 * (int) fs);
        check (std::abs (gr - 5.0) < 0.3, "Sine 10 dB over the threshold, 2:1, RMS detector: " + fmt (gr, 2) + " dB reduction (expect 5.0)");
        c.mix = 0.0;
        auto par = in;
        processCompressor (par, fs, c, st);
        check (identical (par, in), "Compressor mix 0 %: dry, bit-exact");

        WidthSettings w;
        w.monoBelowHz = 120.0;
        auto side40 = sine (fs, 3, -10, 40), side1k = sine (fs, 3, -10, 1000);
        side40.applyGain (1, 0, side40.getNumSamples(), -1.0f);
        side1k.applyGain (1, 0, side1k.getNumSamples(), -1.0f);
        auto o40 = side40, o1k = side1k;
        processWidth (o40, fs, w);
        processWidth (o1k, fs, w);
        const int a = (int) fs, z = 2 * (int) fs;
        const double d40 = rmsDbOf (o40, 0, a, z) - rmsDbOf (side40, 0, a, z), d1k = rmsDbOf (o1k, 0, a, z) - rmsDbOf (side1k, 0, a, z);
        check (d40 < -30.0 && std::abs (d1k) < 0.05, "Mono below 120 Hz: side at 40 Hz " + fmt (d40, 1) + " dB, at 1 kHz " + fmt (d1k, 3) + " dB");
        auto mid = sine (fs, 3, -10, 40);
        auto om = mid;
        w.width = 1.6;
        processWidth (om, fs, w);
        check (identical (om, mid), "Width never touches a mono (mid-only) signal: bit-exact");
    }

    section ("Export and dither");
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("mihenk_selftest");
        dir.createDirectory();
        auto sig = sine (fs, 1, -6.0, 440);
        auto read = [] (const juce::File& f, std::vector<std::vector<int>>& out)
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatReader> r (wav.createReaderFor (new juce::FileInputStream (f), true));
            if (r == nullptr) return 0;
            out.assign (r->numChannels, std::vector<int> ((size_t) r->lengthInSamples));
            std::vector<int*> ptrs;
            for (auto& ch : out) ptrs.push_back (ch.data());
            r->read (ptrs.data(), (int) r->numChannels, 0, (int) r->lengthInSamples, false);
            return (int) r->bitsPerSample;
        };
        juce::String err;
        const auto f24 = dir.getChildFile ("t24.wav"), f16 = dir.getChildFile ("t16.wav");
        exportWav (f24, sig, fs, 24, err);
        std::vector<std::vector<int>> back;
        const int bits24 = read (f24, back);
        bool exact = bits24 == 24 && back.size() == 2;
        for (int i = 0; exact && i < sig.getNumSamples(); ++i)
            exact &= (back[0][(size_t) i] >> 8) == (int) std::lround ((double) sig.getSample (0, i) * 8388608.0);
        check (exact, "24-bit export: every sample is the rounded value, no dither");

        Buffer silence (2, 48000);
        silence.clear();
        exportWav (f24, silence, fs, 24, err);
        read (f24, back);
        bool zero = true;
        for (auto& ch : back) for (int v : ch) zero &= v == 0;
        check (zero, "24-bit export of digital silence stays digital silence");

        exportWav (f16, silence, fs, 16, err);
        const int bits16 = read (f16, back);
        double ss = 0.0, mean = 0.0;
        for (int v : back[0]) { const double q = (double) (v >> 16); ss += q * q; mean += q; }
        const double rmsLsb = std::sqrt (ss / back[0].size());
        mean /= (double) back[0].size();
        check (bits16 == 16 && rmsLsb > 0.4 && rmsLsb < 0.6 && std::abs (mean) < 0.02, "16-bit export: TPDF dither present on silence, RMS " + fmt (rmsLsb, 3) + " LSB (expect 0.5), mean " + fmt (mean, 4));

        std::vector<std::vector<int>> q;
        quantise (sig, 16, true, 1, q);
        double errSum = 0.0, errSq = 0.0;
        for (int i = 0; i < sig.getNumSamples(); ++i)
        {
            const double e = q[0][(size_t) i] - (double) sig.getSample (0, i) * 32768.0;
            errSum += e; errSq += e * e;
        }
        const double n = sig.getNumSamples();
        check (std::abs (errSum / n) < 0.02 && std::abs (std::sqrt (errSq / n) - std::sqrt (0.25)) < 0.08,
               "16-bit TPDF error on a sine: mean " + fmt (errSum / n, 4) + " LSB, RMS " + fmt (std::sqrt (errSq / n), 3) + " LSB (TPDF + rounding: 0.5)");

        MasterSettings s;
            applyGenreStartingPoint (s, Genre::arabeskPop);
        s.limiter.driveDb = 7.25;
        const auto preset = dir.getChildFile ("roundtrip.json");
        juce::String presetError;
        MasterSettings loaded;
        const bool saved = savePreset (preset, s, presetError) && loadPreset (preset, loaded, presetError);
        PreLimiterCache ca, cb;
        const auto ra = MasterChain::render (sig, fs, s, ca), rb = MasterChain::render (sig, fs, loaded, cb);
        check (saved && identical (*ra.output, *rb.output), "JSON preset save/load: round-trip render is bit-identical");

        const auto report = dir.getChildFile ("export.mihenk.txt");
        RenderResult rr = ra;
        check (report.replaceWithText (MasterChain::report ("source.wav", "export.wav", 24, s, analyse (sig, fs), ra.measured, rr, nullptr))
                   && report.existsAsFile(), "Export report: .mihenk.txt exists and contains the measurement path");
        dir.deleteRecursively();
    }

    section ("Signal-aware glue: AUTO threshold, dense input, level-neutral make-up, TARGET LOCK");
    {
        const Genre genres[] { Genre::rapTrap, Genre::arabeskPop, Genre::electronicLofi };
        auto dynamic = musicLike (fs, 32, 21);

        // (a) The solver itself, on a raw signal: the threshold it returns gives the target GR over the loud part,
        //     checked independently here (own short-term measurement, own 70th percentile, the compressor's gain curve).
        {
            CompSettings c;
            c.enabled = true; c.ratio = 2.0; c.attackMs = 30.0; c.releaseMs = 250.0; c.autoRelease = true; c.rmsBlend = 0.7; c.scHpfHz = 90.0; c.kneeDb = 6.0;
            std::vector<float> level;
            compressorDetector (dynamic, fs, c, level);
            const auto L = measureLoudness (dynamic, fs);
            std::vector<float> st;
            for (float v : L.shortTerm) if (v > -70.0f) st.push_back (v);
            std::sort (st.begin(), st.end());
            const float p70 = st[(size_t) std::floor (0.7 * (double) (st.size() - 1))];
            const auto mask = loudPartMask (L.shortTerm, dynamic.getNumSamples(), fs, 0.7);
            size_t loud = 0;
            for (auto m : mask) loud += m;
            const double loudShare = (double) loud / (double) mask.size();
            for (double target : { 0.5, 1.0, 1.5, 3.0 })
            {
                int its = 0;
                const double thr = solveGlueThreshold (level, mask, fs, c, target, &its);
                auto cc = c; cc.thresholdDb = thr;
                auto out = dynamic;
                GainReductionStats gst;
                std::vector<float> gr;
                processCompressor (out, fs, cc, gst, &gr);
                // Independent mean over samples whose centred short-term value is >= p70.
                const int block = (int) std::lround (0.1 * fs);
                double sum = 0.0; size_t cnt = 0;
                for (int i = 0; i < dynamic.getNumSamples(); ++i)
                {
                    const int k = juce::jlimit (0, (int) L.shortTerm.size() - 1, i / block - 14);
                    if (L.shortTerm[(size_t) k] >= p70) { sum -= gr[(size_t) i]; ++cnt; }
                }
                const double mean = sum / (double) cnt;
                check (std::abs (mean - target) <= 0.05 && its <= 25,
                       "AUTO solver, glue target " + fmt (target, 1) + " dB: threshold " + fmt (thr, 2) + " dBFS gives " + fmt (mean, 3)
                       + " dB mean GR on the loud part (" + fmt (loudShare * 100.0, 0) + " % of the file), " + juce::String (its) + " detector passes");
            }
            // Signal-derived, not absolute: the same mix 6 dB hotter or quieter moves the solved threshold by the same 6 dB.
            double thr0 = solveGlueThreshold (level, mask, fs, c, 1.2), shift[2];
            for (int k = 0; k < 2; ++k)
            {
                auto g = dynamic;
                g.applyGain ((float) dbToGain (k == 0 ? 6.0 : -6.0));
                std::vector<float> lv;
                compressorDetector (g, fs, c, lv);
                shift[k] = solveGlueThreshold (lv, loudPartMask (measureLoudness (g, fs).shortTerm, g.getNumSamples(), fs), fs, c, 1.2) - thr0;
            }
            check (std::abs (shift[0] - 6.0) < 0.1 && std::abs (shift[1] + 6.0) < 0.1,
                   "AUTO threshold follows the signal: +6 dB input moves it " + fmt (shift[0], 2) + " dB, -6 dB moves it " + fmt (shift[1], 2) + " dB");
        }

        // (b) Dynamic input + each starting point: loud-part GR on the glue target +-0.3 dB, not flagged dense.
        for (auto g : genres)
        {
            MasterSettings s;
            applyGenreStartingPoint (s, g);
            s.limiter.driveDb = 6.0;
            PreLimiterCache cache;
            const auto r = MasterChain::render (dynamic, fs, s, cache);
            bool warned = false;
            for (auto& w : r.warnings) warned |= w.contains ("already dense");
            check (r.glue.valid && ! r.glue.dense && ! warned && std::abs (r.glue.loudMeanGrDb - s.comp.glueDb) <= 0.3,
                   juce::String ("Dynamic input, ") + genreName (g) + ": PLR " + fmt (r.glue.plr) + " LU, crest " + fmt (r.glue.crest) + " dB, LRA " + fmt (r.glue.lra)
                   + " LU -> not dense; AUTO threshold " + fmt (r.glue.thresholdDb) + " dBFS, loud-part GR " + fmt (r.glue.loudMeanGrDb, 2) + " dB (target " + fmt (s.comp.glueDb, 1)
                   + " +-0.3), max " + fmt (r.glue.loudMaxGrDb, 1) + " dB");
        }

        // (c) Dense input: the same music pre-limited to a crest factor of about 7 dB.
        Buffer dense;
        {
            auto env = truePeakEnvelope (dynamic, kLimiterOversampling);
            LimiterSettings ls; ls.ceilingDbtp = -1.0; ls.releaseMs = 15.0; ls.lookaheadMs = 0.5;
            for (double drive = 6.0; drive <= 36.0; drive += 1.0)
            {
                ls.driveDb = drive;
                Buffer out;
                limitWithSafety (dynamic, env, fs, ls, out);
                dense = out;
                if (analyse (out, fs, false).crest <= 7.2) break;
            }
        }
        const auto denseM = analyse (dense, fs, false);
        for (auto g : genres)
        {
            MasterSettings s;
            applyGenreStartingPoint (s, g);
            PreLimiterCache cache;
            const auto r = MasterChain::renderLocked (dense, fs, s, cache);
            juce::String warning;
            for (auto& w : r.warnings) if (w.startsWith ("Input already dense: glue reduced to")) warning = w;
            check (r.glue.dense && r.glue.targetGrDb <= 0.5 && r.glue.ratio <= 1.5 && r.glue.loudMeanGrDb <= 0.6 && warning.isNotEmpty(),
                   juce::String ("Dense input (crest ") + fmt (denseM.crest) + " dB, " + fmt (denseM.integrated) + " LUFS), " + genreName (g) + ": loud-part GR "
                   + fmt (r.glue.loudMeanGrDb, 2) + " dB (<= 0.6), ratio " + fmt (r.glue.ratio, 1) + ", warning \"" + warning + "\"");
        }

        // (d) Level-neutral compressor: pre-limiter loudness moves < 0.5 LU when the compressor is toggled.
        for (double glueDb : { 1.2, 4.0 })
        {
            MasterSettings s;
            applyGenreStartingPoint (s, Genre::rapTrap);
            s.comp.glueDb = glueDb;
            PreLimiterCache a, b;
            MasterChain::renderPreLimiter (dynamic, fs, s, a);
            s.comp.enabled = false;
            MasterChain::renderPreLimiter (dynamic, fs, s, b);
            check (std::abs (a.integratedPre - b.integratedPre) < 0.5,
                   "Compressor make-up, glue " + fmt (glueDb, 1) + " dB: pre-limiter " + fmt (a.integratedPre, 2) + " LUFS with, " + fmt (b.integratedPre, 2)
                   + " LUFS without (make-up " + fmt (a.glue.makeupDb, 2) + " dB)");
        }

        // (e) TARGET LOCK: starting points, EQ and compressor moves never move the output off -14 LUFS.
        {
            MasterSettings s;
            s.targetLock = true;
            s.targetLufs = -14.0;
            PreLimiterCache cache;
            double worst = 0.0;
            juce::StringArray steps;
            auto step = [&] (const juce::String& what)
            {
                const auto r = MasterChain::renderLocked (dynamic, fs, s, cache);
                const double err = r.measured.integrated + 14.0;
                worst = juce::jmax (worst, std::abs (err));
                steps.add (what + " " + fmt (r.measured.integrated, 2));
                return r;
            };
            for (auto g : genres) { applyGenreStartingPoint (s, g); step (genreName (g)); }
            applyGenreStartingPoint (s, Genre::rapTrap); step ("rap again");
            s.eq.bell2Db = 4.0; s.eq.tiltDbPerOct = 0.5; step ("EQ +4 dB bell, tilt");
            s.eq.lowShelfDb = -4.0; step ("EQ low shelf -4");
            s.comp.glueDb = 3.5; step ("glue 3.5");
            s.comp.autoThreshold = false; s.comp.thresholdDb = -32.0; s.comp.ratio = 3.0; step ("manual threshold -32, 3:1");
            s.comp.enabled = false; step ("compressor out");
            s.width.width = 1.4; step ("width 140 %");
            const auto last = step ("final");
            check (worst <= 0.2 && last.measured.truePeak <= s.limiter.ceilingDbtp + 1e-4,
                   "TARGET LOCK: 3 starting points + 7 EQ / compressor / width moves stay within " + fmt (worst, 3) + " LU of -14 (<= 0.2), true peak "
                   + fmt (last.measured.truePeak, 2) + " dBTP");
            // With the lock off the drive is left alone, so the same moves do change the loudness (the old behaviour).
            MasterSettings u;
            u.targetLock = false;
            u.limiter.driveDb = 6.0;
            applyGenreStartingPoint (u, Genre::rapTrap);
            PreLimiterCache c2;
            const double l1 = MasterChain::renderLocked (dynamic, fs, u, c2).measured.integrated;
            u.eq.lowShelfDb = -4.0; u.eq.bell2Db = -4.0;
            const double l2 = MasterChain::renderLocked (dynamic, fs, u, c2).measured.integrated;
            check (std::abs (u.limiter.driveDb - 6.0) < 1e-12 && std::abs (l1 - l2) > 0.2,
                   "TARGET LOCK off: drive stays at +6.00 dB and the EQ move changes the output (" + fmt (l1, 2) + " -> " + fmt (l2, 2) + " LUFS)");
        }
    }


    section ("3-BAND compressor (Linkwitz-Riley 4th order, zero phase)");
    {
        // (a) The split sums back flat, and each band is -6.02 dB at its crossover (LR4).
        double worst = 0.0;
        juce::String list;
        for (double f : { 30.0, 120.0, 500.0, 2000.0, 4000.0, 12000.0 })
        {
            const double g = sineGainThrough (fs, f, [] (Buffer& b)
            {
                Buffer lo, mi, hi;
                splitThreeBands (b, 48000.0, 120.0, 4000.0, lo, mi, hi);
                b.clear();
                for (int c = 0; c < 2; ++c) { b.addFrom (c, 0, lo, c, 0, b.getNumSamples()); b.addFrom (c, 0, mi, c, 0, b.getNumSamples()); b.addFrom (c, 0, hi, c, 0, b.getNumSamples()); }
            });
            worst = juce::jmax (worst, std::abs (g));
            list << fmt (f, 0) << " Hz " << fmt (g, 4) << "  ";
        }
        const double atX1 = sineGainThrough (fs, 120.0, [] (Buffer& b) { Buffer lo, mi, hi; splitThreeBands (b, 48000.0, 120.0, 4000.0, lo, mi, hi); b = lo; });
        const double atX2 = sineGainThrough (fs, 4000.0, [] (Buffer& b) { Buffer lo, mi, hi; splitThreeBands (b, 48000.0, 120.0, 4000.0, lo, mi, hi); b = hi; });
        check (worst <= 0.1 && std::abs (atX1 + 6.02) < 0.2 && std::abs (atX2 + 6.02) < 0.2,
               "Crossovers 120 Hz / 4 kHz: bands sum flat (worst " + fmt (worst, 4) + " dB, limit 0.1): " + list + "; low band at 120 Hz " + fmt (atX1, 2)
               + " dB, high band at 4 kHz " + fmt (atX2, 2) + " dB (LR4 -6.02)");

        // (b) The whole 3-BAND stage with every band neutral (bypassed, gain 0) is flat.
        {
            MasterSettings s;
            s.comp.enabled = true; s.comp.mode = CompMode::threeBand;
            for (auto& b : s.comp.bands) b.bypass = true;
            double w2 = 0.0;
            for (double f : { 40.0, 120.0, 1000.0, 4000.0, 15000.0 })
                w2 = juce::jmax (w2, std::abs (sineGainThrough (fs, f, [&s] (Buffer& b) { PreLimiterCache c; MasterChain::renderPreLimiter (b, 48000.0, s, c); b = *c.buffer; })));
            check (w2 <= 0.1, "3-BAND stage with all bands neutral: flat within " + fmt (w2, 4) + " dB (limit 0.1) at 40 Hz .. 15 kHz");
        }

        // (c) Band independence: a dynamic low part and a dynamic high part with different rhythms.
        const int n = (int) (20.0 * fs);
        Buffer mix (2, n);
        {
            auto hiNoise = shapedNoise (fs, 20.0, 3.0, -20.0, 5);
            Buffer lo, mi, hi;
            splitThreeBands (hiNoise, fs, 120.0, 4000.0, lo, mi, hi);
            for (int i = 0; i < n; ++i)
            {
                const double t = i / fs;
                // Sections (so the mix is not dense: LRA well over 4 LU), then each band with its own rhythm.
                const double sec = std::fmod (t, 8.0) < 4.0 ? 0.25 : 1.0;
                const double envL = 0.2 + 0.8 * std::pow (0.5 + 0.5 * std::sin (2.0 * kPi * 0.7 * t), 3.0);
                const double envH = std::fmod (t, 0.37) < 0.08 ? 1.0 : 0.15;
                const double low = 0.3 * envL * std::sin (2.0 * kPi * 50.0 * t);
                for (int c = 0; c < 2; ++c) mix.setSample (c, i, (float) (sec * (low + 2.0 * envH * hi.getSample (c, i))));
            }
        }
        auto crestOf = [&] (const MasterSettings& s, int band)
        {
            PreLimiterCache c;
            MasterChain::renderPreLimiter (mix, fs, s, c);
            return bandCrest (*c.buffer, fs, band, 120.0, 4000.0);
        };
        const double lowIn = bandCrest (mix, fs, 0, 120.0, 4000.0), highIn = bandCrest (mix, fs, 2, 120.0, 4000.0);
        MasterSettings onlyLow;
        onlyLow.comp.enabled = true; onlyLow.comp.mode = CompMode::threeBand;
        onlyLow.comp.bands[1].bypass = onlyLow.comp.bands[2].bypass = true;
        onlyLow.comp.bands[0].glueDb = 3.0; onlyLow.comp.bands[0].ratio = 4.0; onlyLow.comp.bands[0].attackMs = 5.0;
        const double lowOut = crestOf (onlyLow, 0), highOutL = crestOf (onlyLow, 2);
        check (std::abs (highOutL - highIn) <= 0.3 && lowIn - lowOut >= 1.0,
               "Low band only compressed: high-band crest " + fmt (highIn, 2) + " -> " + fmt (highOutL, 2) + " dB (within 0.3), low-band crest "
               + fmt (lowIn, 2) + " -> " + fmt (lowOut, 2) + " dB (reduced)");
        MasterSettings onlyHigh = onlyLow;
        onlyHigh.comp.bands[0].bypass = true; onlyHigh.comp.bands[2].bypass = false;
        onlyHigh.comp.bands[2].glueDb = 3.0; onlyHigh.comp.bands[2].ratio = 4.0; onlyHigh.comp.bands[2].attackMs = 1.0;
        const double lowOutH = crestOf (onlyHigh, 0), highOut = crestOf (onlyHigh, 2);
        // (Fast bursts through a 1 ms attack: the compressor takes the sustain, so the high band's crest goes up; it must move.)
        check (std::abs (lowOutH - lowIn) <= 0.3 && std::abs (highIn - highOut) >= 0.5,
               "High band only compressed: low-band crest " + fmt (lowIn, 2) + " -> " + fmt (lowOutH, 2) + " dB (within 0.3), high-band crest "
               + fmt (highIn, 2) + " -> " + fmt (highOut, 2) + " dB (changed by the compressor)");

        // (d) AUTO per band: each band's loud-part GR lands on its own glue target.
        auto dynamic = musicLike (fs, 32, 21);
        {
            MasterSettings s;
            s.comp.enabled = true; s.comp.mode = CompMode::threeBand;
            s.comp.bands[0].glueDb = 1.0; s.comp.bands[1].glueDb = 1.5; s.comp.bands[2].glueDb = 2.0;
            PreLimiterCache c;
            MasterChain::renderPreLimiter (dynamic, fs, s, c);
            const auto& g = c.glue;
            bool ok = g.threeBand && ! g.dense;
            for (int b = 0; b < 3; ++b) ok &= std::abs (g.bands[b].loudMeanGrDb - s.comp.bands[b].glueDb) <= 0.1;
            check (ok, "AUTO per band on a dynamic mix: loud-part GR " + fmt (g.bands[0].loudMeanGrDb, 3) + " / " + fmt (g.bands[1].loudMeanGrDb, 3) + " / "
                   + fmt (g.bands[2].loudMeanGrDb, 3) + " dB (targets 1.0 / 1.5 / 2.0 +-0.1), thresholds " + fmt (g.bands[0].thresholdDb) + " / "
                   + fmt (g.bands[1].thresholdDb) + " / " + fmt (g.bands[2].thresholdDb) + " dBFS");
            // (e) Level-neutral: pre-limiter loudness with and without the stage.
            PreLimiterCache c2;
            auto off = s; off.comp.enabled = false;
            MasterChain::renderPreLimiter (dynamic, fs, off, c2);
            check (std::abs (c.integratedPre - c2.integratedPre) < 0.5,
                   "3-BAND make-up: pre-limiter " + fmt (c.integratedPre, 2) + " LUFS with, " + fmt (c2.integratedPre, 2) + " LUFS without (band make-ups "
                   + fmt (g.bands[0].makeupDb, 2) + " / " + fmt (g.bands[1].makeupDb, 2) + " / " + fmt (g.bands[2].makeupDb, 2) + " dB, final " + fmt (g.makeupDb, 2) + " dB)");
            // Solo: with the bands neutral, soloing the low band outputs exactly the low band of the split.
            auto solo = s;
            for (auto& b : solo.comp.bands) b.bypass = true;
            solo.comp.bands[0].solo = true;
            PreLimiterCache c3;
            MasterChain::renderPreLimiter (dynamic, fs, solo, c3);
            Buffer lo, mi, hi;
            splitThreeBands (dynamic, fs, 120.0, 4000.0, lo, mi, hi);
            float err = 0.0f;
            for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < lo.getNumSamples(); ++i) err = juce::jmax (err, std::abs (c3.buffer->getSample (ch, i) - lo.getSample (ch, i)));
            check (gainToDb (err) < -90.0, "Low band SOLO: output is the low band of the split (largest difference " + fmt (gainToDb (err), 1) + " dBFS)");
        }
        // (f) Dense input: every band capped at 0.5 dB and 1.5:1.
        {
            auto env = truePeakEnvelope (dynamic, kLimiterOversampling);
            LimiterSettings ls; ls.ceilingDbtp = -1.0; ls.releaseMs = 15.0; ls.lookaheadMs = 0.5; ls.driveDb = 16.0;
            Buffer dense;
            limitWithSafety (dynamic, env, fs, ls, dense);
            MasterSettings s;
            s.comp.enabled = true; s.comp.mode = CompMode::threeBand;
            for (auto& b : s.comp.bands) { b.glueDb = 2.5; b.ratio = 3.0; }
            PreLimiterCache c;
            const auto r = MasterChain::renderLocked (dense, fs, s, c);
            bool ok = r.glue.dense;
            for (auto& b : r.glue.bands) ok &= b.loudMeanGrDb <= 0.6 && b.ratio <= 1.5;
            bool warned = false;
            for (auto& w : r.warnings) warned |= w.startsWith ("Input already dense");
            check (ok && warned, "Dense input in 3-BAND (" + r.glue.denseReason + "): band GR " + fmt (r.glue.bands[0].loudMeanGrDb, 2) + " / " + fmt (r.glue.bands[1].loudMeanGrDb, 2)
                   + " / " + fmt (r.glue.bands[2].loudMeanGrDb, 2) + " dB (<= 0.6), ratio " + fmt (r.glue.bands[0].ratio, 1) + ", warning " + (warned ? "present" : "missing"));
        }
        // (g) TARGET LOCK still holds with the 3-BAND stage and band moves.
        {
            MasterSettings s;
            s.targetLufs = -14.0;
            applyGenreStartingPoint (s, Genre::rapTrap);
            s.comp.mode = CompMode::threeBand;
            PreLimiterCache c;
            double worstLu = 0.0;
            RenderResult last;
            auto step = [&] { last = MasterChain::renderLocked (dynamic, fs, s, c); worstLu = juce::jmax (worstLu, std::abs (last.measured.integrated + 14.0)); };
            step();
            s.comp.bands[0].glueDb = 3.5; step();
            s.comp.bands[2].gainDb = 3.0; step();
            s.comp.bands[1].bypass = true; step();
            s.comp.xoverLowHz = 250.0; s.comp.xoverHighHz = 6000.0; step();
            s.comp.bands[0].autoThreshold = false; s.comp.bands[0].thresholdDb = -30.0; step();
            check (worstLu <= 0.2 && last.measured.truePeak <= -1.0 + 1e-4,
                   "TARGET LOCK in 3-BAND: 6 band / crossover moves stay within " + fmt (worstLu, 3) + " LU of -14 (<= 0.2), true peak " + fmt (last.measured.truePeak, 2) + " dBTP");
        }
    }

    section ("AIR band and dynamic EQ");
    {
        bool ok = true;
        juce::String list;
        for (double hz : { 10000.0, 15000.0, 20000.0, 40000.0 })
        {
            EqSettings e;
            e.airDb = 6.0; e.airHz = hz;
            double w = 0.0;
            for (double f : { 1000.0, 5000.0, 10000.0, 15000.0, 19000.0 })
                w = juce::jmax (w, std::abs (eqGainAt (e, fs, f) - airShelfDb (f, 6.0, hz)));
            ok &= w <= 0.2;
            list << fmt (hz / 1000.0, 0) << " kHz " << fmt (w, 3) << "  ";
        }
        EqSettings e;
        e.airDb = 6.0; e.airHz = 15000.0; e.linearPhase = true;
        const double lin = std::abs (eqGainAt (e, fs, 12000.0) - airShelfDb (12000.0, 6.0, 15000.0));
        check (ok && lin <= 0.2, "AIR +6 dB: FIR response within 0.2 dB of the design at 1-19 kHz, worst per setting: " + list + "; linear phase " + fmt (lin, 3) + " dB");
        check (std::abs (airShelfDb (15000.0, 6.0, 15000.0) - 20.0 * std::log10 (1.0 + (dbToGain (6.0) - 1.0) * std::sqrt (0.5))) < 1e-9
                   && airShelfDb (1000.0, 6.0, 15000.0) < 0.6 && airShelfDb (20000.0, 6.0, 40000.0) < 3.5,
               "AIR shape is broad and gentle: 15 kHz setting gives " + fmt (airShelfDb (1000.0, 6.0, 15000.0), 2) + " dB at 1 kHz, " + fmt (airShelfDb (15000.0, 6.0, 15000.0), 2)
               + " dB at 15 kHz; 40 kHz setting gives " + fmt (airShelfDb (20000.0, 6.0, 40000.0), 2) + " dB at 20 kHz");

        // Dynamic AIR: pink noise with sibilant 5-9 kHz bursts.
        auto base = shapedNoise (fs, 12.0, -1.0, -24.0, 61);
        auto burst = shapedNoise (fs, 12.0, 0.0, -14.0, 62);
        {
            Buffer lo, mi, hi;
            splitThreeBands (burst, fs, 5000.0, 9000.0, lo, mi, hi);
            burst = mi;
        }
        const int n = base.getNumSamples();
        std::vector<uint8_t> inBurst ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            const bool on = std::fmod (i / fs, 1.1) < 0.09;
            inBurst[(size_t) i] = on;
            for (int c = 0; c < 2; ++c) base.setSample (c, i, base.getSample (c, i) + (on ? burst.getSample (c, i) : 0.0f));
        }
        auto render = [&] (bool dyn)
        {
            MasterSettings s;
            s.eq.airDb = 3.0; s.eq.airHz = 10000.0; s.eq.airDynamic = dyn; s.eq.airDeHarshDb = 4.0;
            PreLimiterCache c;
            MasterChain::renderPreLimiter (base, fs, s, c);
            return std::make_pair (*c.buffer, c.dynEq.air);
        };
        const auto st = render (false), dy = render (true);
        auto bandE = [&] (const Buffer& b, bool on)
        {
            Buffer lo, mi, hi;
            splitThreeBands (b, fs, 5000.0, 9000.0, lo, mi, hi);
            double e = 0.0; size_t k = 0;
            for (int i = (int) fs; i < n - (int) fs; ++i) if ((bool) inBurst[(size_t) i] == on) { e += (double) mi.getSample (0, i) * mi.getSample (0, i); ++k; }
            return 10.0 * std::log10 (e / (double) k);
        };
        const double dOn = bandE (dy.first, true) - bandE (st.first, true), dOff = bandE (dy.first, false) - bandE (st.first, false);
        check (dOn <= -1.5 && std::abs (dOff) <= 0.3 && dy.second.active,
               "Dynamic AIR (+3 dB @ 10 kHz, de-harsh 4 dB): 5-9 kHz in the bursts " + fmt (dOn, 2) + " dB against static AIR (<= -1.5), between bursts "
               + fmt (dOff, 2) + " dB (within 0.3); typical ratio " + fmt (dy.second.typicalRelDb, 1) + " dB, max easing " + fmt (dy.second.maxReductionDb, 1) + " dB");

        // Dynamic bell and tightener: the mean cut over the loud part is the amount, and only their band moves.
        auto music = musicLike (fs, 32, 21);
        {
            MasterSettings s;
            s.eq.dynLowMid.enabled = true; s.eq.dynLowMid.freqHz = 300.0; s.eq.dynLowMid.amountDb = 2.0;
            PreLimiterCache c;
            MasterChain::renderPreLimiter (music, fs, s, c);
            const auto& r = c.dynEq.lowMid;
            const auto inS = analyse (music, fs).spectrumDb, outS = analyse (*c.buffer, fs).spectrumDb;
            const double d300 = interpGrid (outS, 300.0) - interpGrid (inS, 300.0), d5k = interpGrid (outS, 5000.0) - interpGrid (inS, 5000.0);
            check (r.active && std::abs (r.loudMeanCutDb - 2.0) <= 0.1 && d300 < -0.8 && std::abs (d5k) < 0.1,
                   "Dynamic bell 300 Hz, amount 2 dB: loud-part mean cut " + fmt (r.loudMeanCutDb, 3) + " dB (max " + fmt (r.maxCutDb, 1) + "), long-term 300 Hz "
                   + fmt (d300, 2) + " dB, 5 kHz " + fmt (d5k, 3) + " dB");
        }
        {
            MasterSettings s;
            s.eq.tightener.enabled = true; s.eq.tightener.amountDb = 2.0;
            PreLimiterCache c;
            MasterChain::renderPreLimiter (music, fs, s, c);
            const auto& r = c.dynEq.tightener;
            const auto inS = analyse (music, fs).spectrumDb, outS = analyse (*c.buffer, fs).spectrumDb;
            const double d50 = interpGrid (outS, 50.0) - interpGrid (inS, 50.0), d2k = interpGrid (outS, 2000.0) - interpGrid (inS, 2000.0);
            check (r.active && std::abs (r.loudMeanCutDb - 2.0) <= 0.1 && d50 < -1.0 && std::abs (d2k) < 0.1,
                   "Low tightener (dynamic low shelf 90 Hz), amount 2 dB: loud-part mean cut " + fmt (r.loudMeanCutDb, 3) + " dB, long-term 50 Hz " + fmt (d50, 2)
                   + " dB, 2 kHz " + fmt (d2k, 3) + " dB");
        }
    }

    section ("Low end, input stage and headroom assistant");
    {
        // DC filter.
        auto dc = sine (fs, 4.0, -12.0, 1000.0);
        for (int c = 0; c < 2; ++c) for (int i = 0; i < dc.getNumSamples(); ++i) dc.setSample (c, i, dc.getSample (c, i) + 0.01f);
        auto f = dc;
        dcFilter (f, fs);
        double mean = 0.0;
        for (int i = (int) fs; i < 3 * (int) fs; ++i) mean += f.getSample (0, i);
        mean /= 2.0 * fs;
        const double g20 = sineGainThrough (fs, 20.0, [] (Buffer& b) { dcFilter (b, 48000.0); });
        const double g1k = sineGainThrough (fs, 1000.0, [] (Buffer& b) { dcFilter (b, 48000.0); });
        check (gainToDb (std::abs (mean)) < -80.0 && std::abs (g20) < 0.05 && std::abs (g1k) < 0.001,
               "DC filter: offset -40 dBFS -> " + fmt (gainToDb (std::abs (mean)), 1) + " dBFS, 20 Hz " + fmt (g20, 3) + " dB, 1 kHz " + fmt (g1k, 4) + " dB");

        // Phase-safe low mono: anti-phase 50 Hz with an in-phase 1 kHz on top.
        auto a = concat ({ sine (fs, 4.0, -10.0, 50.0) });
        auto hiTone = sine (fs, 4.0, -16.0, 1000.0);
        a.applyGain (1, 0, a.getNumSamples(), -1.0f);
        for (int c = 0; c < 2; ++c) a.addFrom (c, 0, hiTone, c, 0, a.getNumSamples());
        auto lowEnergyDb = [&] (const Buffer& b)
        {
            auto l = zeroPhaseLowPass (b.getReadPointer (0), b.getNumSamples(), fs, 150.0);
            double e = 0.0;
            for (int i = (int) fs; i < 3 * (int) fs; ++i) e += l[(size_t) i] * l[(size_t) i];
            return 10.0 * std::log10 (e / (2.0 * fs) + 1e-30);
        };
        WidthSettings w; w.monoBelowHz = 150.0;
        auto plain = a, safe = a;
        processWidth (plain, fs, w);
        w.phaseSafeLowMono = true;
        double rho = 0.0;
        processWidth (safe, fs, w, &rho);
        const double e0 = lowEnergyDb (a), ePlain = lowEnergyDb (plain), eSafe = lowEnergyDb (safe);
        const double corrAfter = lowBandCorrelation (safe, fs, 150.0);
        const double hiDiff = rmsDbOf (safe, 0, (int) fs, 3 * (int) fs) - rmsDbOf (plain, 0, (int) fs, 3 * (int) fs);
        check (ePlain - e0 < -30.0 && std::abs (eSafe - e0) < 0.5 && rho < -0.99 && corrAfter > 0.99,
               "Phase-safe low mono on anti-phase 50 Hz (low correlation " + fmt (rho, 2) + "): plain mono below 150 Hz loses " + fmt (e0 - ePlain, 1)
               + " dB of the lows, phase-safe keeps them within " + fmt (std::abs (eSafe - e0), 2) + " dB, low correlation after " + fmt (corrAfter, 3)
               + " (level difference incl. 1 kHz " + fmt (hiDiff, 2) + " dB)");

        // High width: side above 4 kHz x 0.5 (-6 dB); side at 500 Hz untouched.
        {
            WidthSettings hw; hw.highWidth = 0.5; hw.highWidthHz = 4000.0;
            auto sideGain = [&] (double freq)
            {
                auto sd = sine (fs, 3.0, -12.0, freq);
                sd.applyGain (1, 0, sd.getNumSamples(), -1.0f);
                auto o = sd;
                processWidth (o, fs, hw);
                return rmsDbOf (o, 0, (int) fs, 2 * (int) fs) - rmsDbOf (sd, 0, (int) fs, 2 * (int) fs);
            };
            const double g12k = sideGain (12000.0), g500 = sideGain (500.0);
            check (std::abs (g12k + 6.02) < 0.1 && std::abs (g500) < 0.05,
                   "High width 50 % above 4 kHz: side at 12 kHz " + fmt (g12k, 2) + " dB (expect -6.02), at 500 Hz " + fmt (g500, 3) + " dB");
        }

        // Headroom assistant: to peak -6 dBFS / short-term max -14 LUFS, whichever binds.
        auto mixHot = cleanMix (fs);
        normalisePeak (mixHot, -0.1);
        auto mixQuiet = cleanMix (fs);
        normalisePeak (mixQuiet, -24.0);
        bool okH = true;
        juce::String txt;
        for (auto* b : { &mixHot, &mixQuiet })
        {
            const auto src = MasterChain::measureSource (*b, fs);
            const auto st = headroomAssistant (src);
            const double peakAfter = src.samplePeakDb + st.autoTrimDb, stAfter = src.shortTermMax + st.autoTrimDb;
            okH &= peakAfter <= -6.0 + 1e-6 && stAfter <= -14.0 + 1e-6 && (std::abs (peakAfter + 6.0) < 1e-6 || std::abs (stAfter + 14.0) < 1e-6);
            txt << "peak " << fmt (src.samplePeakDb, 1) << " / ST max " << fmt (src.shortTermMax, 1) << " -> trim " << fmt (st.autoTrimDb, 2) << " dB (\"" << st.message.substring (0, 40) << "...\")  ";
            MasterSettings s; s.autoTrim = true;
            PreLimiterCache c;
            const auto r = MasterChain::render (*b, fs, s, c);
            okH &= std::abs (r.input.autoTrimDb - st.autoTrimDb) < 1e-9;
        }
        check (okH, "Headroom assistant lands on the working level: " + txt);

        auto imb = cleanMix (fs);
        const double bal0 = rmsDbOf (imb, 0, 0, imb.getNumSamples()) - rmsDbOf (imb, 1, 0, imb.getNumSamples());
        imb.applyGain (0, 0, imb.getNumSamples(), (float) dbToGain (2.0));
        MasterSettings s; s.balanceDb = 2.0;
        PreLimiterCache c;
        MasterChain::renderPreLimiter (imb, fs, s, c);
        const double bal = rmsDbOf (*c.buffer, 0, 0, imb.getNumSamples()) - rmsDbOf (*c.buffer, 1, 0, imb.getNumSamples());
        check (std::abs (bal - bal0) < 0.001, "Balance correction +2.0 dB on a mix with the left 2 dB up: L - R " + fmt (bal, 3) + " dB (the clean mix's own "
               + fmt (bal0, 3) + " dB)");
    }

    section ("Diagnosis: 20 scenarios, measured rules, no false positives, each fix measurably helps");
    {
        const auto clean = cleanMix (fs);
        const auto dClean = diagnose (clean, fs, MasterSettings{});
        juce::StringArray cf;
        for (auto& f : dClean.findings) cf.add (f.id + " (" + fmt (f.value, 2) + ")");
        const auto& cm = dClean.m;
        check (dClean.findings.empty(), "Clean mix: no finding in any of the 20 rules" + (cf.isEmpty() ? juce::String() : ": " + cf.joinIntoString (", "))
               + "  [peak " + fmt (cm.samplePeak, 1) + ", PLR " + fmt (cm.plr, 1) + ", LRA " + fmt (cm.lra, 1) + ", low corr " + fmt (cm.lowCorrelation, 2)
               + ", side/mid " + fmt (cm.sideMidDb, 1) + " / >4k " + fmt (cm.sideMidHighDb, 1) + ", mud " + fmt (cm.mudDb, 1) + ", harsh " + fmt (cm.harshDb, 1)
               + ", presence " + fmt (cm.presenceDb, 1) + ", sub " + fmt (cm.subDb, 1) + ", low " + fmt (cm.lowDb, 1) + ", air " + fmt (cm.airRelDb, 1)
               + ", sib " + fmt (cm.sibPeakDb, 1) + "/" + fmt (cm.sibAbsDb, 1) + ", uneven " + fmt (cm.unevenLowDb, 1) + ", res " + fmt (cm.resonanceDb, 1) + "]");

        struct Case
        {
            int sc;
            std::function<Buffer()> make;
            juce::String metricName;
            std::function<double (const DiagnosisMeasures& in, const RenderResult& r, const DiagnosisMeasures& out)> metric;
            std::function<bool (double before, double after, const RenderResult& rb, const RenderResult& ra)> better;
        };
        auto chainInPeak = [] (const DiagnosisMeasures& in, const RenderResult& r, const DiagnosisMeasures&) { return in.samplePeak + r.input.effectiveTrimDb; };
        const Case cases[] {
            { scTooHot, [&fs] { return scenarioSignal (scTooHot, fs); }, "chain input peak (dBFS) / output TP",
              chainInPeak, [] (double b, double a, const RenderResult&, const RenderResult& ra) { return b > -1.0 && a <= -5.9 && ra.measured.truePeak <= -1.0 + 1e-4; } },
            { scClipped, [&fs] { return scenarioSignal (scClipped, fs); },
              "chain input peak (dBFS); limiter GR", chainInPeak,
              [] (double b, double a, const RenderResult&, const RenderResult& ra) { return b > -1.0 && a <= -5.9 && ra.limiter.gr.maxDb <= 1.0 && ra.measured.truePeak <= -1.0 + 1e-4; } },
            { scTooQuiet, [&fs] { return scenarioSignal (scTooQuiet, fs); }, "chain input peak (dBFS)", chainInPeak,
              [] (double b, double a, const RenderResult&, const RenderResult&) { return b < -20.0 && std::abs (a + 6.0) <= 0.1; } },
            { scPhase, [&fs] { return scenarioSignal (scPhase, fs); },
              "low-band correlation", [] (const DiagnosisMeasures&, const RenderResult&, const DiagnosisMeasures& o) { return o.lowCorrelation; },
              [] (double b, double a, const RenderResult&, const RenderResult&) { return b < 0.0 && a > 0.9; } },
            { scTooWide, [&fs] { return scenarioSignal (scTooWide, fs); },
              "side/mid above 4 kHz (dB)", [] (const DiagnosisMeasures&, const RenderResult&, const DiagnosisMeasures& o) { return o.sideMidHighDb; },
              [] (double b, double a, const RenderResult&, const RenderResult&) { return a <= b - 1.5; } },
            { scTooNarrow, [&fs] { return scenarioSignal (scTooNarrow, fs); },
              "side/mid above 4 kHz (dB)", [] (const DiagnosisMeasures&, const RenderResult&, const DiagnosisMeasures& o) { return o.sideMidHighDb; },
              [] (double b, double a, const RenderResult&, const RenderResult&) { return a >= b + 1.5; } },
            { scImbalance, [&fs] { return scenarioSignal (scImbalance, fs); },
              "|L - R| (dB)", [] (const DiagnosisMeasures&, const RenderResult&, const DiagnosisMeasures& o) { return std::abs (o.balanceDb); },
              [] (double b, double a, const RenderResult&, const RenderResult&) { return b > 2.0 && a < 0.2; } },
            { scDcOffset, [&fs] { return scenarioSignal (scDcOffset, fs); },
              "DC (dBFS)", [] (const DiagnosisMeasures&, const RenderResult&, const DiagnosisMeasures& o) { return o.dcDbfs; },
              [] (double b, double a, const RenderResult&, const RenderResult&) { return a <= b - 20.0; } },
            { scMuddy, [&fs] { return scenarioSignal (scMuddy, fs); },
              "200-500 Hz excess (dB)", [] (const DiagnosisMeasures&, const RenderResult&, const DiagnosisMeasures& o) { return o.mudDb; },
              [] (double b, double a, const RenderResult&, const RenderResult&) { return a <= b - 1.0; } },
            { scBoomy, [&fs] { return scenarioSignal (scBoomy, fs); },
              "25-60 Hz over 100-300 Hz (dB)", [] (const DiagnosisMeasures&, const RenderResult&, const DiagnosisMeasures& o) { return o.subDb; },
              [] (double b, double a, const RenderResult&, const RenderResult&) { return a <= b - 1.5; } },
            { scThin, [&fs] { return scenarioSignal (scThin, fs); },
              "40-120 Hz over 300 Hz-1 kHz (dB)", [] (const DiagnosisMeasures&, const RenderResult&, const DiagnosisMeasures& o) { return o.lowDb; },
              [] (double b, double a, const RenderResult&, const RenderResult&) { return a >= b + 1.0; } },
            { scHarsh, [&fs] { return scenarioSignal (scHarsh, fs); },
              "2-5 kHz excess (dB)", [] (const DiagnosisMeasures&, const RenderResult&, const DiagnosisMeasures& o) { return o.harshDb; },
              [] (double b, double a, const RenderResult&, const RenderResult&) { return a <= b - 1.0; } },
            { scSibilance, [&fs] { return scenarioSignal (scSibilance, fs); },
              "5-9 kHz peaks over typical (dB)", [] (const DiagnosisMeasures&, const RenderResult&, const DiagnosisMeasures& o) { return o.sibPeakDb; },
              [] (double b, double a, const RenderResult&, const RenderResult&) { return a <= b - 1.0; } },
            { scDull, [&fs] { return scenarioSignal (scDull, fs); },
              "10-16 kHz over 1-4 kHz (dB)", [] (const DiagnosisMeasures&, const RenderResult&, const DiagnosisMeasures& o) { return o.airRelDb; },
              [] (double b, double a, const RenderResult&, const RenderResult&) { return a >= b + 1.0; } },
            { scBuriedVocal, [&fs] { return scenarioSignal (scBuriedVocal, fs); },
              "1-4 kHz presence (dB)", [] (const DiagnosisMeasures&, const RenderResult&, const DiagnosisMeasures& o) { return o.presenceDb; },
              [] (double b, double a, const RenderResult&, const RenderResult&) { return a >= b + 1.0; } },
            { scOverCompressed, [&fs] { return scenarioSignal (scOverCompressed, fs); },
              "output PLR (LU); glue GR; limiter GR", [] (const DiagnosisMeasures&, const RenderResult&, const DiagnosisMeasures& o) { return o.plr; },
              [] (double, double a, const RenderResult&, const RenderResult& ra) { return a >= 5.0 && ra.glue.loudMeanGrDb <= 0.55 && ra.limiter.gr.maxDb <= 0.5; } },
            { scTooDynamic, [&fs] { return scenarioSignal (scTooDynamic, fs); },
              "LRA (LU)", [] (const DiagnosisMeasures&, const RenderResult&, const DiagnosisMeasures& o) { return o.lra; },
              [] (double b, double a, const RenderResult&, const RenderResult&) { return a <= b - 1.0; } },
            { scUnevenLow, [&fs] { return scenarioSignal (scUnevenLow, fs); },
              "low-band swing (dB)", [] (const DiagnosisMeasures&, const RenderResult&, const DiagnosisMeasures& o) { return o.unevenLowDb; },
              [] (double b, double a, const RenderResult&, const RenderResult&) { return a <= b - 0.5; } },
            { scResonance, [&fs] { return scenarioSignal (scResonance, fs); },
              "peak prominence (dB)", [] (const DiagnosisMeasures&, const RenderResult&, const DiagnosisMeasures& o) { return o.resonanceDb; },
              [] (double b, double a, const RenderResult&, const RenderResult&) { return a <= b - 3.0; } },
            { scCodecRisk, [&fs] { return scenarioSignal (scCodecRisk, fs); },
              "output true peak (dBTP), ceiling -0.1 before", [] (const DiagnosisMeasures&, const RenderResult& r, const DiagnosisMeasures&) { return r.measured.truePeak; },
              [] (double b, double a, const RenderResult&, const RenderResult&) { return b > -0.5 && a <= -1.0 + 1e-4; } },
        };
        MasterSettings base;
        base.comp.enabled = true;
        for (const auto& cs : cases)
        {
            const auto sig = cs.make();
            const auto d = diagnose (sig, fs, base);
            const auto* f = d.find (cs.sc);
            juce::StringArray others;
            for (auto& x : d.findings) if (x.scenario != cs.sc) others.add (x.id);
            const auto& dm = d.m;
            const juce::String measures = "[mud " + fmt (dm.mudDb, 2) + ", harsh " + fmt (dm.harshDb, 2) + ", presence " + fmt (dm.presenceDb, 2) + ", sib " + fmt (dm.sibPeakDb, 2)
                                          + "/" + fmt (dm.sibAbsDb, 2) + ", air " + fmt (dm.airRelDb, 2) + ", uneven " + fmt (dm.unevenLowDb, 2) + ", LRA " + fmt (dm.lra, 1) + "]";
            check (f != nullptr, juce::String ("Rule ") + juce::String (cs.sc) + " " + scenarioId (cs.sc) + " fires on its synthetic case"
                   + (f != nullptr ? ": \"" + f->reason + "\" -> " + f->delta : ": NOT FIRED " + measures)
                   + (others.isEmpty() ? juce::String() : "  (also: " + others.joinIntoString (", ") + ")"));
            auto sBefore = base;
            if (cs.sc == scCodecRisk) sBefore.limiter.ceilingDbtp = -0.1;      // a ceiling that leaves the codec no room
            auto sAfter = sBefore;
            applyScenario (sAfter, cs.sc, &d.m);
            PreLimiterCache c1, c2;
            const auto rb = MasterChain::renderLocked (sig, fs, sBefore, c1);
            const auto ra = MasterChain::renderLocked (sig, fs, sAfter, c2);
            const auto mb = measureMix (*rb.output, fs), ma = measureMix (*ra.output, fs);
            const double vb = cs.metric (d.m, rb, mb), va = cs.metric (d.m, ra, ma);
            check (cs.better (vb, va, rb, ra), juce::String ("Scenario ") + juce::String (cs.sc) + " " + scenarioId (cs.sc) + " applied: " + cs.metricName + " "
                   + fmt (vb, 2) + " -> " + fmt (va, 2) + " (output " + fmt (ra.measured.integrated, 2) + " LUFS, " + fmt (ra.measured.truePeak, 2) + " dBTP, limiter "
                   + fmt (ra.limiter.gr.maxDb, 1) + " dB, glue " + fmt (ra.glue.loudMeanGrDb, 2) + " dB)");
        }

        // Scenario presets live in the preset system: 20 factory entries in "Scenarios", and an edited copy saves as a user scenario.
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("mihenk_scenario_selftest");
        dir.deleteRecursively();
        dir.createDirectory();
        MasterSettings s;
        applyScenario (s, scMuddy, nullptr);
        s.eq.dynLowMid.amountDb = 2.5;               // the user's edit
        juce::String err;
        const auto file = dir.getChildFile (juce::String ("Muddy low-mids (mine)") + kPresetExtension);
        const bool saved = savePreset (file, s, err, kCategoryScenarios, scMuddy);
        MasterSettings back;
        const bool loaded = loadPreset (file, back, err);
        const auto list = listPresets (dir);
        int factoryScenarios = 0;
        for (auto& e : list) factoryScenarios += (e.factory && e.category == kCategoryScenarios) ? 1 : 0;
        const auto& last = list.back();
        auto sig = musicLike (fs, 8, 31);
        PreLimiterCache ca, cb;
        const auto ra = MasterChain::render (sig, fs, s, ca), rb = MasterChain::render (sig, fs, back, cb);
        check (saved && loaded && factoryScenarios == kNumScenarios && list[3].scenario == 1 && list[22].scenario == 20 && ! last.factory
                   && last.category == kCategoryScenarios && last.scenario == scMuddy && identical (*ra.output, *rb.output) && back.eq.dynLowMid.amountDb == 2.5,
               "Scenario presets: " + juce::String (factoryScenarios) + " factory entries in \"Scenarios\" after the starting points; an edited copy saves as a user scenario (\""
               + last.name + "\", category " + last.category + ", scenario " + scenarioId (last.scenario) + ") and renders identically");
        dir.deleteRecursively();
    }

    section ("Knob value text and presets");
    {
        struct P { const char* text; double lo, hi, expect; bool ok; };
        const P cases[] {
            { "-14.0 LUFS", -24, -6, -14.0, true }, { "3.5 kHz", 40, 16000, 3500.0, true }, { "3.50 KHZ", 40, 16000, 3500.0, true },
            { "2k", 40, 16000, 2000.0, true }, { "120 hz", 0, 300, 120.0, true }, { "+1.5 dB", -6, 6, 1.5, true }, { "-0.5db", -6, 6, -0.5, true },
            { "Q 1.20", 0.3, 6, 1.2, true }, { "2.00:1", 1.5, 4, 2.0, true }, { "70 % RMS", 0, 100, 70.0, true }, { "1,5", 0, 4, 1.5, true },
            { "99 dB", -6, 6, 6.0, true }, { "-80", -60, 0, -60.0, true }, { "40 khz", 40, 16000, 16000.0, true }, { "OFF", 20, 300, 20.0, true },
            { ".5 ms", 0.1, 10, 0.5, true }, { "abc", 0, 1, 0.0, false }, { "", 0, 1, 0.0, false },
        };
        bool all = true;
        juce::StringArray bad;
        for (auto& c : cases)
        {
            double v = -12345.0;
            const bool ok = parseKnobText (c.text, c.lo, c.hi, v);
            const bool pass = ok == c.ok && (! ok || std::abs (v - c.expect) < 1e-9);
            if (! pass) bad.add (juce::String ("\"") + c.text + "\" -> " + (ok ? fmt (v, 3) : juce::String ("none")));
            all &= pass;
        }
        check (all, "Value text: units (LUFS, dB, kHz, k, Hz, Q, :1, %, ms, OFF, decimal comma) and clamping to the knob range, 18 cases"
                    + (bad.isEmpty() ? juce::String() : ": " + bad.joinIntoString ("; ")));

        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("mihenk_presets_selftest");
        dir.deleteRecursively();
        dir.createDirectory();
        MasterSettings s;
        applyGenreStartingPoint (s, Genre::electronicLofi);
        s.comp.autoThreshold = false; s.comp.thresholdDb = -27.5; s.comp.glueDb = 2.25; s.targetLock = false; s.limiter.driveDb = 4.5; s.eq.bell1Db = 2.5;
        juce::String err;
        const bool saved = savePreset (dir.getChildFile (sanitisePresetName ("Zed / loud: v2") + kPresetExtension), s, err)
                           && savePreset (dir.getChildFile (juce::String ("alpha") + kPresetExtension), MasterSettings{}, err);
        MasterSettings back;
        const bool loaded = loadPreset (dir.getChildFile (juce::String ("Zed - loud- v2") + kPresetExtension), back, err);
        auto sig = musicLike (fs, 8, 31);
        PreLimiterCache ca, cb;
        const auto ra = MasterChain::render (sig, fs, s, ca), rb = MasterChain::render (sig, fs, back, cb);
        check (saved && loaded && identical (*ra.output, *rb.output) && ! back.comp.autoThreshold && back.comp.glueDb == 2.25 && ! back.targetLock
                   && settingsFingerprint (back) == settingsFingerprint (s),
               "Preset round trip (.mihenkpreset, AUTO / GLUE / TARGET LOCK stored): identical render, same fingerprint");
        const auto list = listPresets (dir);
        juce::StringArray names;
        for (auto& e : list) names.add ((e.factory ? "F:" : "U:") + e.name);
        // v2: the 20 factory scenarios sit between the starting points and the user presets (list[3..22]).
        juce::StringArray shown;
        for (auto& e : list) if (! (e.factory && e.category == kCategoryScenarios)) shown.add ((e.factory ? "F:" : "U:") + e.name);
        check (list.size() == 25 && list[0].factory && list[0].genre == Genre::rapTrap && list[1].genre == Genre::arabeskPop
                   && list[2].genre == Genre::electronicLofi && list[3].category == kCategoryScenarios && list[22].category == kCategoryScenarios
                   && ! list[23].factory && list[23].name == "alpha" && list[24].name == "Zed - loud- v2",
               "Preset list: factory starting points first, then the 20 scenarios, then user presets by name: " + shown.joinIntoString (", "));
        auto solvedOnly = s;
        solvedOnly.targetLock = true; solvedOnly.comp.autoThreshold = true;
        auto resolved = solvedOnly;
        resolved.limiter.driveDb += 3.0; resolved.comp.thresholdDb -= 5.0;
        auto edited = solvedOnly;
        edited.eq.bell1Db += 0.1;
        check (settingsFingerprint (resolved) == settingsFingerprint (solvedOnly) && settingsFingerprint (edited) != settingsFingerprint (solvedOnly),
               "Preset \"modified\" mark: a re-solved drive / AUTO threshold does not count, a knob move does");
        dir.deleteRecursively();
    }

    section ("Determinism");
    {
        auto in = musicLike (fs, 6, 8);
        MasterSettings s;
            applyGenreStartingPoint (s, Genre::rapTrap);
        s.limiter.driveDb = 8.0;
        PreLimiterCache a, b;
        auto r1 = MasterChain::render (in, fs, s, a), r2 = MasterChain::render (in, fs, s, b);
        bool finite = true;
        for (int c = 0; c < 2; ++c) for (int i = 0; i < r1.output->getNumSamples(); ++i) finite &= std::isfinite (r1.output->getSample (c, i));
        check (identical (*r1.output, *r2.output) && finite, "Same input and settings: identical output, all finite");
        auto r3 = MasterChain::render (in, fs, s, a);   // served from the pre-limiter cache
        check (identical (*r1.output, *r3.output), "Pre-limiter cache gives the same output as a fresh render");
    }


    section ("Real-time chain (the plug-in path): streamed in random host blocks against the offline render");
    {
        // Each case: the offline engine renders (TARGET LOCK solves the drive, AUTO the thresholds), its plan goes to the
        // real-time chain, and the same input is streamed through RtChain in random block sizes (1..1024 samples).
        // After the reported latency the two outputs are compared sample by sample.
        struct RtCase { const char* name; double rate; std::function<void (MasterSettings&)> set; };
        const RtCase cases[] {
            { "HIP-HOP / RAP (WIDE AUTO glue, mono below 120 Hz, min-phase EQ), TARGET LOCK -14", 48000.0,
              [] (MasterSettings& s) { applyGenreStartingPoint (s, Genre::rapTrap); } },
            { "R&B / POP, 3-BAND (per-band glue, gains, crossovers 150 Hz / 5 kHz), linear-phase EQ, 44.1 kHz", 44100.0,
              [] (MasterSettings& s) { applyGenreStartingPoint (s, Genre::arabeskPop); s.eq.linearPhase = true; s.comp.mode = CompMode::threeBand;
                                       s.comp.xoverLowHz = 150.0; s.comp.xoverHighHz = 5000.0; s.comp.bands[0].glueDb = 2.0; s.comp.bands[1].gainDb = 0.5;
                                       s.comp.bands[2].glueDb = 0.5; s.comp.bands[2].ratio = 3.0; } },
            { "ELECTRONIC + dynamic EQ (low-mid, upper-mid, tightener), dynamic AIR, phase-safe low mono, high width, auto trim", 48000.0,
              [] (MasterSettings& s) { applyGenreStartingPoint (s, Genre::electronicLofi); s.autoTrim = true;
                                       s.eq.dynLowMid.enabled = true; s.eq.dynLowMid.freqHz = 280.0; s.eq.dynLowMid.amountDb = 2.0;
                                       s.eq.dynUpperMid.enabled = true; s.eq.dynUpperMid.amountDb = 1.5; s.eq.tightener.enabled = true;
                                       s.eq.airDb = 2.0; s.eq.airHz = 15000.0; s.eq.airDynamic = true; s.eq.airDeHarshDb = 4.0;
                                       s.width.phaseSafeLowMono = true; s.width.highWidth = 1.2; s.width.highWidthHz = 6000.0; } },
            { "Hot drive (+12 dB, target lock off, fast release): limiter safety passes, balance, static AIR", 48000.0,
              [] (MasterSettings& s) { applyGenreStartingPoint (s, Genre::rapTrap); s.targetLock = false; s.limiter.driveDb = 12.0;
                                       s.limiter.releaseMs = 20.0; s.limiter.lookaheadMs = 0.8; s.balanceDb = 0.6; s.eq.airDb = 3.0; s.eq.airHz = 10000.0; } },
            { "WIDE glue, width 110 %, no split switched in (the shortest latency), TARGET LOCK -14", 48000.0,
              [] (MasterSettings& s) { applyGenreStartingPoint (s, Genre::rapTrap); s.width.monoBelowHz = 0.0; s.width.width = 1.1; } },
        };
        int caseNo = 0;
        for (const auto& c : cases)
        {
            ++caseNo;
            const double rate = c.rate;
            auto body = musicLike (rate, 16.0, 40 + (uint32_t) caseNo);
            auto in = concat ({ Buffer (2, (int) (0.3 * rate)), faded (body, rate), Buffer (2, (int) (1.5 * rate)) });
            for (int ch = 0; ch < 2; ++ch) { in.clear (ch, 0, (int) (0.3 * rate)); in.clear (ch, in.getNumSamples() - (int) (1.5 * rate), (int) (1.5 * rate)); }
            MasterSettings s;
            c.set (s);
            if (caseNo == 2)
            {
                // A tone-match curve as well, so the EQ kernel carries it.
                std::vector<float> curve (spectrumGrid().size());
                for (size_t k = 0; k < curve.size(); ++k) curve[k] = (float) (1.5 * std::sin (std::log2 (spectrumGrid()[k] / 20.0)));
                s.eq.matchCurveDb = curve;
                s.eq.matchAmount = 0.7;
            }
            PreLimiterCache cache;
            const auto r = MasterChain::renderLocked (in, rate, s, cache);
            const auto plan = makePlan (s, r, rate);

            RtChain rt;
            rt.prepare (rate, 2, 1024);
            rt.setSynchronousKernels (true);
            rt.setMatchCurve (s.eq.matchCurveDb);
            rt.prime (s);
            rt.setPlan (plan);
            MasterSettings live = s;
            live.eq.matchCurveDb.clear();
            const int lat = rt.latencySamples (s);
            const int n = in.getNumSamples(), total = n + lat;
            Buffer streamed (2, total);
            streamed.clear();
            for (int ch = 0; ch < 2; ++ch) streamed.copyFrom (ch, 0, in, ch, 0, n);
            juce::Random rng (1234 + caseNo);
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            for (int pos = 0; pos < total;)
            {
                const int len = juce::jmin (total - pos, 1 + rng.nextInt (1024));
                float* ptrs[2] { streamed.getWritePointer (0, pos), streamed.getWritePointer (1, pos) };
                rt.process (ptrs, 2, len, live);
                pos += len;
            }
            const double secs = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
            double maxDiff = 0.0, errSq = 0.0, sigSq = 0.0;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < n; ++i)
                {
                    const double a = r.output->getSample (ch, i), b = streamed.getSample (ch, i + lat);
                    maxDiff = juce::jmax (maxDiff, std::abs (a - b));
                    errSq += (a - b) * (a - b);
                    sigSq += a * a;
                }
            const double residualDb = 10.0 * std::log10 (errSq / juce::jmax (1.0e-30, sigSq) + 1.0e-30);
            Buffer rtOut (2, n);
            for (int ch = 0; ch < 2; ++ch) rtOut.copyFrom (ch, 0, streamed, ch, lat, n);
            const double tp = gainToDb (truePeakMax (rtOut, 4));
            check (gainToDb (maxDiff) <= -70.0 && residualDb <= -80.0 && tp <= s.limiter.ceilingDbtp + 0.01,
                   juce::String (c.name) + ": largest sample difference " + fmt (gainToDb (maxDiff), 1) + " dBFS, residual " + fmt (residualDb, 1)
                   + " dB under the signal, true peak " + fmt (tp, 2) + " dBTP; latency " + juce::String (lat) + " samples ("
                   + fmt (1000.0 * lat / rate, 1) + " ms), streamed at " + juce::String (juce::roundToInt ((double) n / rate / secs)) + "x real time");
        }

        // BYPASS: the input, delayed by exactly the reported latency, bit for bit.
        {
            auto in = musicLike (fs, 3.0, 77);
            MasterSettings s;
            applyGenreStartingPoint (s, Genre::rapTrap);
            s.bypass = true;
            RtChain rt;
            rt.prepare (fs, 2, 512);
            rt.setSynchronousKernels (true);
            rt.prime (s);
            const int lat = rt.latencySamples (s), n = in.getNumSamples();
            Buffer b (2, n + lat);
            b.clear();
            for (int ch = 0; ch < 2; ++ch) b.copyFrom (ch, 0, in, ch, 0, n);
            for (int pos = 0; pos < n + lat; pos += 512)
            {
                float* p[2] { b.getWritePointer (0, pos), b.getWritePointer (1, pos) };
                rt.process (p, 2, juce::jmin (512, n + lat - pos), s);
            }
            bool same = true;
            for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < n; ++i) same &= b.getSample (ch, i + lat) == in.getSample (ch, i);
            check (same, "Real-time BYPASS: the input delayed by the reported latency (" + juce::String (lat) + " samples), bit for bit");
        }

        // DC filter: causal in real time (the offline one is zero phase); it still removes the offset.
        {
            auto in = musicLike (fs, 6.0, 78);
            for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < in.getNumSamples(); ++i) in.setSample (ch, i, in.getSample (ch, i) + 0.01f);
            MasterSettings s;
            s.dcFilter = true;
            s.limiter.enabled = false;
            RtChain rt;
            rt.prepare (fs, 2, 512);
            rt.setSynchronousKernels (true);
            rt.prime (s);
            for (int pos = 0; pos < in.getNumSamples(); pos += 512)
            {
                float* p[2] { in.getWritePointer (0, pos), in.getWritePointer (1, pos) };
                rt.process (p, 2, juce::jmin (512, in.getNumSamples() - pos), s);
            }
            double mean = 0.0;
            const int from = in.getNumSamples() - (int) (3.0 * fs);
            for (int i = from; i < in.getNumSamples(); ++i) mean += in.getSample (0, i);
            mean /= (double) (in.getNumSamples() - from);
            check (gainToDb (std::abs (mean)) < -70.0, "Real-time DC filter (causal 5 Hz high-pass): -40 dBFS offset -> " + fmt (gainToDb (std::abs (mean)), 1) + " dBFS after 3 s");
        }
    }

    section ("v2 usability: loudness honesty, KEEP INPUT, album loudness and tone, batch export");
    {
        const double fs = 48000.0;
        // A hot production: the music-like mix limited to -8 LUFS at -0.3 dBTP (what the owner loaded).
        Buffer hot;
        {
            auto raw = musicLike (fs, 30.0, 91);
            MasterSettings h;
            h.targetPreset = TargetPreset::custom; h.targetLufs = -9.0; h.limiter.ceilingDbtp = -0.3; h.limiter.releaseMs = 60.0;
            PreLimiterCache c;
            RenderResult r;
            MasterChain::matchTarget (raw, fs, h, c, &r);
            hot = *r.output;
        }
        const auto hotIn = analyse (hot, fs);
        check (std::abs (hotIn.integrated + 9.0) < 0.1 && hotIn.truePeak > -0.5, "Hot synthetic input: " + fmt (hotIn.integrated, 2) + " LUFS, " + fmt (hotIn.truePeak, 2) + " dBTP");

        // STREAM -14 turns a hot mix DOWN, and says so, with the platform that would do the same.
        {
            MasterSettings s;
            PreLimiterCache c;
            auto r = MasterChain::renderLocked (hot, fs, s, c);
            const auto st = MasterChain::loudnessStory (s, hotIn.integrated, r.measured.integrated, r.limiter.gr.maxDb);
            check (std::abs (r.measured.integrated + 14.0) <= 0.05 && std::abs (st.changeDb + 5.0) < 0.15 && st.sentence.contains ("turns it down 5.0 dB")
                       && st.sentence.contains ("Spotify") && st.shortLine.contains ("TURNED DOWN"),
                   "STREAM -14 on a -9 LUFS mix: " + fmt (r.measured.integrated, 2) + " LUFS; says \"" + st.sentence + "\"");
            const auto rep = MasterChain::report ("hot.wav", "hot_MIHENK.wav", 24, s, hotIn, r.measured, r, nullptr);
            check (rep.contains ("Loudness    Your mix is -9.0 LUFS; STREAM -14 turns it down 5.0 dB"), "The export report states the turn-down in its Loudness line");
        }
        // KEEP INPUT: hold the input's loudness; the limiter only guards the ceiling; never louder than the input.
        {
            MasterSettings s;
            s.targetPreset = TargetPreset::keepInput;
            PreLimiterCache c;
            auto r = MasterChain::renderLocked (hot, fs, s, c);
            const auto st = MasterChain::loudnessStory (s, hotIn.integrated, r.measured.integrated, r.limiter.gr.maxDb);
            // The mix peaks at -0.3 dBTP, the ceiling is -1.0: the ceiling may cost at most those 0.7 dB, never add loudness.
            check (hotIn.integrated - r.measured.integrated <= (hotIn.truePeak - s.limiter.ceilingDbtp) + 0.05 && r.measured.integrated <= hotIn.integrated + 0.05
                       && r.measured.truePeak <= -0.95
                       && std::abs (s.targetLufs - hotIn.integrated) < 0.02 && st.sentence.startsWith ("KEEP INPUT") && (st.sentence.contains ("the ceiling costs") || st.sentence.contains ("holds")),
                   "KEEP INPUT on the -9 LUFS mix: " + fmt (r.measured.integrated, 2) + " LUFS (input " + fmt (hotIn.integrated, 2) + "), TP " + fmt (r.measured.truePeak, 2)
                       + " dBTP, solved target " + fmt (s.targetLufs, 2) + ", limiter " + fmt (r.limiter.gr.maxDb, 1) + " dB: the -1.0 ceiling under the mix's -0.3 dBTP peaks costs "
                       + fmt (hotIn.integrated - r.measured.integrated, 2) + " dB (<= 0.7), never louder");
            // A brick-walled master (LRA < 1 LU, peaks at -0.3): holding it under -1 dBTP is not possible with a little limiting,
            // so KEEP INPUT stops at +1 dB of extra drive, stays under the input and says what the ceiling cost.
            Buffer brick;
            {
                auto raw = musicLike (fs, 20.0, 95);
                LimiterSettings ls; ls.ceilingDbtp = -0.3; ls.releaseMs = 20.0; ls.lookaheadMs = 0.5; ls.driveDb = 24.0;
                limitWithSafety (raw, truePeakEnvelope (raw, kLimiterOversampling), fs, ls, brick);
            }
            const auto bIn = analyse (brick, fs);
            MasterSettings k;
            k.targetPreset = TargetPreset::keepInput;
            PreLimiterCache ck;
            RenderResult rk;
            const auto mk = MasterChain::matchTarget (brick, fs, k, ck, &rk);
            check (rk.measured.integrated <= bIn.integrated + 0.02 && bIn.integrated - rk.measured.integrated < 1.0 && rk.limiter.gr.maxDb < 3.0 && mk.message.startsWith ("KEEP INPUT: holding"),
                   "KEEP INPUT on a brick-walled master (" + fmt (bIn.integrated, 2) + " LUFS, LRA " + fmt (bIn.lra, 1) + "): " + fmt (rk.measured.integrated, 2)
                       + " LUFS, limiter " + fmt (rk.limiter.gr.maxDb, 1) + " dB, never crushed; \"" + mk.message + "\"");
            auto quiet = musicLike (fs, 30.0, 92);
            normalisePeak (quiet, -12.0);
            const auto qIn = analyse (quiet, fs);
            MasterSettings q;
            q.targetPreset = TargetPreset::keepInput;
            applyGenreStartingPoint (q, Genre::rapTrap);
            PreLimiterCache cq;
            auto rq = MasterChain::renderLocked (quiet, fs, q, cq);
            check (std::abs (rq.measured.integrated - qIn.integrated) <= 0.2 && ! rq.warnings.joinIntoString ("|").contains ("target"),
                   "KEEP INPUT on a quiet mix with the HIP-HOP starting point: " + fmt (qIn.integrated, 2) + " -> " + fmt (rq.measured.integrated, 2)
                       + " LUFS (not pushed to -14), no target warning");
            MasterSettings p;
            p.targetPreset = TargetPreset::keepInput;
            MasterSettings p2 = p;
            p2.targetLufs = -3.0;
            check (settingsFingerprint (p) == settingsFingerprint (p2), "KEEP INPUT: the solved target does not mark the preset as modified");
        }
        // MATCH TO REFERENCE (tone): the rendered output moves towards the reference by the level-aligned distance.
        {
            auto mix = shapedNoise (fs, 12.0, -0.8, -20.0, 93);
            auto ref = shapedNoise (fs, 12.0, 0.6, -18.0, 94);
            const auto mi = analyse (mix, fs), rf = analyse (ref, fs);
            MasterSettings s;
            s.eq.matchCurveDb = MasterChain::toneMatchCurve (mi.spectrumDb, rf.spectrumDb, s.eq, s.toneSmoothOct);
            s.targetPreset = TargetPreset::custom; s.targetLufs = rf.integrated;
            PreLimiterCache c;
            auto r = MasterChain::renderLocked (mix, fs, s, c);
            const double before = MasterChain::toneDistanceDb (mi.spectrumDb, rf.spectrumDb), after = MasterChain::toneDistanceDb (r.measured.spectrumDb, rf.spectrumDb);
            check (after < 0.35 * before && std::abs (r.measured.integrated - rf.integrated) <= 0.05,
                   "MATCH TO REFERENCE: tone distance " + fmt (before, 2) + " -> " + fmt (after, 2) + " dB RMS, loudness " + fmt (r.measured.integrated, 2) + " vs ref " + fmt (rf.integrated, 2));
        }

        // Album: four tracks, quiet to loud, with different tilts.
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("mihenk_selftest_batch");
        dir.deleteRecursively();
        dir.createDirectory();
        const double levels[] { -10.0, -12.0, -16.0, -20.0 }, tilts[] { 0.6, -0.4, 0.0, -0.9 };
        std::vector<juce::File> srcFiles;
        for (int i = 0; i < 4; ++i)
        {
            auto b = musicLike (fs, 12.0, 100 + (uint32_t) i);
            applyEq (b, fs, [&] (EqSettings& e) { e.tiltDbPerOct = tilts[i]; });
            const double L = integratedLoudness (b, fs);
            b.applyGain ((float) dbToGain (levels[i] - L));
            if (b.getMagnitude (0, b.getNumSamples()) > 0.99f) { LimiterSettings ls; ls.ceilingDbtp = -0.5; juce::AudioBuffer<float> o; limitWithSafety (b, truePeakEnvelope (b, kLimiterOversampling), fs, ls, o); b = o; }
            const auto f = dir.getChildFile ("song" + juce::String (i + 1) + ".wav");
            juce::String err;
            exportWav (f, b, fs, 24, err);
            srcFiles.push_back (f);
        }
        {
            std::vector<batch::Job> jobs;
            for (auto& f : srcFiles) { batch::Job j; j.file = f; applyGenreStartingPoint (j.settings, Genre::arabeskPop); jobs.push_back (j); }
            batch::Options o;
            o.album = true;
            o.outDir = dir.getChildFile ("album");
            int calls = 0;
            double lastF = -1.0;
            bool monotonic = true;
            const auto res = batch::run (jobs, o, [&] (int, int, double f, const juce::String&) { ++calls; monotonic &= f >= lastF - 1.0e-9; lastF = f; });
            bool onPlan = res.tracks.size() == 4, order = true;
            double worst = 0.0, outMax = -100.0, outMin = 100.0, inMax = -100.0, inMin = 100.0;
            double sIn[4] {}, sOut[4] {};
            juce::String txt;
            for (int i = 0; i < 4 && onPlan; ++i)
            {
                const auto& t = res.tracks[(size_t) i];
                onPlan &= t.ok;
                const double planned = t.in.integrated + t.plannedGainDb;       // v3: the plan's gain, not a drive solved against the render
                // album v3.2 (SPEC_V3 2 REVISED): each master REACHES album target + its offset within 0.2 LU (no +3/+8 caps); the limiter may take what the peaks need
                onPlan &= std::abs (t.out.integrated - (res.plan.targetLufs + res.plan.offsetLu[(size_t) i])) <= 0.2;
                onPlan &= planned - t.out.integrated >= -0.25;                  // the limiter only takes, never adds (plan vs render)
                worst = juce::jmax (worst, planned - t.out.integrated);
                if (i > 0) order &= t.out.integrated <= res.tracks[(size_t) i - 1].out.integrated + 0.05;
                outMax = juce::jmax (outMax, t.out.integrated); outMin = juce::jmin (outMin, t.out.integrated);
                inMax = juce::jmax (inMax, t.in.integrated); inMin = juce::jmin (inMin, t.in.integrated);
                sIn[i] = t.inSlope; sOut[i] = t.outSlope;
                txt << fmt (t.in.integrated, 1) << ">" << fmt (t.out.integrated, 2) << " ";
            }
            auto spread = [] (const double* x) { double lo = x[0], hi = x[0]; for (int i = 1; i < 4; ++i) { lo = juce::jmin (lo, x[i]); hi = juce::jmax (hi, x[i]); } return hi - lo; };
            check (onPlan && order && outMax <= -14.0 + 0.2 && (outMax - outMin) < (inMax - inMin),
                   "ALBUM loudness (album v3.2: each master reaches its album target within 0.2 LU, no +3/+8 caps): inputs spread " + fmt (inMax - inMin, 1) + " LU -> masters " + fmt (outMax - outMin, 2) + " LU (never above -14 + 0.2, quiet songs stay quieter, order kept); "
                       + txt + "LUFS, largest limiter shortfall against the plan " + fmt (worst, 3) + " LU");
            check (onPlan && spread (sOut) <= spread (sIn) + 0.1,
                   "ALBUM tone (v3: the nudge is an override, refused on finished masters, which are level + ceiling only): tilt spread " + fmt (spread (sIn), 2) + " -> " + fmt (spread (sOut), 2)
                       + " dB/oct (album mean " + fmt (res.plan.meanSlope, 2) + ", at most 0.5 dB/oct, never wider)");
            check (calls >= 8 && monotonic && lastF == 1.0, "Batch progress: " + juce::String (calls) + " reports, monotonic, ends at 100 %");
        }
        // Batch export, SAME settings: one folder, a WAV and a report per track, CSV + JSON that agree with the files.
        {
            std::vector<batch::Job> jobs;
            for (int i = 0; i < 3; ++i) { batch::Job j; j.file = srcFiles[(size_t) i]; jobs.push_back (j); }
            batch::Options o;
            o.outDir = dir.getChildFile ("same");
            o.bits = 16;
            const auto res = batch::run (jobs, o);
            bool files = true, agree = true, onTarget = true;
            for (auto& t : res.tracks)
            {
                files &= t.ok && t.output.existsAsFile() && t.output.getSiblingFile (t.output.getFileNameWithoutExtension() + ".mihenk.txt").existsAsFile();
                auto back = loadAudioFile (t.output);
                const auto m = back.ok() ? analyse (back.buffer, back.sampleRate) : Measurement();
                agree &= back.ok() && back.sourceBits == 16 && std::abs (m.integrated - t.out.integrated) <= 0.05;
                onTarget &= t.out.integrated <= -14.0 + 0.2 && (t.in.integrated + t.plannedGainDb) - t.out.integrated >= -0.05 && (t.in.integrated + t.plannedGainDb) - t.out.integrated <= 2.0 && t.out.truePeak <= -0.95;
            }
            const auto csvLines = juce::StringArray::fromLines (res.csv.loadFileAsString().trim());
            const auto js = juce::JSON::parse (res.json);
            const auto* arr = js["tracks"].getArray();
            check (files && res.tracks.size() == 3 && csvLines.size() == 4 && csvLines[0].startsWith ("index,source,output") && arr != nullptr && arr->size() == 3
                       && (bool) (*arr)[0]["ok"] && (*arr)[0]["changes"].hasProperty ("drive_db"),
                   "Batch export: 3 WAVs + 3 reports in one folder, CSV (header + 3 rows) and JSON (3 tracks with their changes)");
            check (agree && onTarget, "Batch export: every exported 16-bit file re-measures within 0.05 LU of its report row, all on the plan (never above -14 LUFS) under -1 dBTP");
            std::atomic<bool> stop { true };
            batch::Options oc;
            oc.outDir = dir.getChildFile ("cancelled");
            const auto rc = batch::run (jobs, oc, {}, &stop);
            int wavs = 0;
            for (const auto& e : juce::RangedDirectoryIterator (oc.outDir, false, "*.wav")) { juce::ignoreUnused (e); ++wavs; }
            check (rc.cancelled && wavs == 0, "Batch cancel before the first track: nothing exported");
            batch::Options od;
            od.outDir = dir;
            const auto a1 = batch::outputFor (juce::File ("/x/a/song.wav"), od, {}), a2 = batch::outputFor (juce::File ("/x/b/song.wav"), od, { a1 });
            check (a1.getFileName() == "song_MIHENK.wav" && a2.getFileName() == "song_MIHENK-2.wav", "Batch output names: two sources called song.wav do not overwrite each other");
        }
        dir.deleteRecursively();
    }

    std::printf ("\n%s\n", failures == 0 ? "ALL PASS" : (juce::String (failures) + " FAILED").toRawUTF8());
    return failures == 0 ? 0 : 1;
}

void printMeasurement (const char* title, const Measurement& m)
{
    std::printf ("%s\n  integrated %s LUFS | short-term max %s | momentary max %s | LRA %s LU\n  true peak %s dBTP | sample peak %s dBFS | RMS %s dBFS | crest %s dB | correlation %s\n",
                 title, fmt (m.integrated).toRawUTF8(), fmt (m.shortTermMax).toRawUTF8(), fmt (m.momentaryMax).toRawUTF8(), fmt (m.lra).toRawUTF8(),
                 fmt (m.truePeak, 2).toRawUTF8(), fmt (m.samplePeak, 2).toRawUTF8(), fmt (m.rms).toRawUTF8(), fmt (m.crest).toRawUTF8(), fmt (m.correlation, 2).toRawUTF8());
}

void printDiagnosis (const Diagnosis& d)
{
    const auto& m = d.m;
    std::printf ("MEASURES\n  peak %s dBFS, TP %s dBTP, ISP %s dB, integrated %s LUFS, ST max %s, LRA %s LU, PLR %s LU, crest %s dB, dense %s\n",
                 fmt (m.samplePeak, 2).toRawUTF8(), fmt (m.truePeak, 2).toRawUTF8(), fmt (m.ispDb, 2).toRawUTF8(), fmt (m.integrated).toRawUTF8(), fmt (m.shortTermMax).toRawUTF8(),
                 fmt (m.lra).toRawUTF8(), fmt (m.plr).toRawUTF8(), fmt (m.crest).toRawUTF8(), m.dense ? "yes" : "no");
    std::printf ("  clip runs %d (%s/min), DC %s dBFS, balance %s dB (low %s), corr %s, low corr %s, mono low loss %s dB, side/mid %s dB, side/mid >4k %s dB\n",
                 m.clipRuns, fmt (m.clipRunsPerMin).toRawUTF8(), fmt (m.dcDbfs).toRawUTF8(), fmt (m.balanceDb, 2).toRawUTF8(), fmt (m.lowBalanceDb, 2).toRawUTF8(),
                 fmt (m.correlation, 2).toRawUTF8(), fmt (m.lowCorrelation, 2).toRawUTF8(), fmt (m.monoLowLossDb).toRawUTF8(), fmt (m.sideMidDb).toRawUTF8(), fmt (m.sideMidHighDb).toRawUTF8());
    std::printf ("  line %s dB/oct, mud %s dB @%s Hz, harsh %s dB @%s Hz, presence %s dB, sub %s dB, low crest %s dB, low %s dB, air %s dB\n",
                 fmt (m.lineSlopeDbPerOct, 2).toRawUTF8(), fmt (m.mudDb).toRawUTF8(), fmt (m.mudPeakHz, 0).toRawUTF8(), fmt (m.harshDb).toRawUTF8(), fmt (m.harshPeakHz, 0).toRawUTF8(),
                 fmt (m.presenceDb).toRawUTF8(), fmt (m.subDb).toRawUTF8(), fmt (m.lowCrestDb).toRawUTF8(), fmt (m.lowDb).toRawUTF8(), fmt (m.airRelDb).toRawUTF8());
    std::printf ("  sibilance peak %s dB (abs %s dB), uneven low %s dB, resonance %s dB @%s Hz\n",
                 fmt (m.sibPeakDb).toRawUTF8(), fmt (m.sibAbsDb).toRawUTF8(), fmt (m.unevenLowDb, 2).toRawUTF8(), fmt (m.resonanceDb).toRawUTF8(), fmt (m.resonanceHz, 0).toRawUTF8());
    std::printf ("FINDINGS (%d)\n", (int) d.findings.size());
    const char* sev[] { "info", "WARN", "PROBLEM" };
    for (auto& f : d.findings)
        std::printf ("  [%s] %d %s: %s\n      -> %s\n", sev[(int) f.severity], f.scenario, f.title.toRawUTF8(), f.reason.toRawUTF8(), f.delta.toRawUTF8());
}

struct CliOptions
{
    juce::StringArray scenarios;
    int bits = 24;
    bool doMatch = true, album = false, perTrack = false;
    juce::String refPath;
    double tone = -1.0;
};

/** The mastering options shared by --master and --batch. Returns false (with a message) on an unknown option. */
bool parseMasterOptions (int argc, char** argv, int start, MasterSettings& s, CliOptions& o)
{
    for (int i = start; i < argc; ++i)
    {
        const juce::String a (argv[i]);
        auto next = [&] { return i + 1 < argc ? juce::String::fromUTF8 (argv[++i]) : juce::String(); };
        if (a == "--target")
        {
            const auto t = next();
            if (t.equalsIgnoreCase ("keep")) s.targetPreset = TargetPreset::keepInput;
            else { s.targetLufs = t.getDoubleValue(); s.targetPreset = TargetPreset::custom; }
        }
        else if (a == "--ceiling") s.limiter.ceilingDbtp = next().getDoubleValue();
        else if (a == "--bits") o.bits = next().getIntValue();
        else if (a == "--warn") s.limitWarnDb = next().getDoubleValue();
        else if (a == "--drive") { s.limiter.driveDb = next().getDoubleValue(); o.doMatch = false; s.targetLock = false; }
        else if (a == "--linear") s.eq.linearPhase = true;
        else if (a == "--no-match") { o.doMatch = false; s.targetLock = false; }
        else if (a == "--reference") o.refPath = next();
        else if (a == "--tone") o.tone = next().getDoubleValue();
        else if (a == "--album") o.album = true;
        else if (a == "--per-track") o.perTrack = true;
        else if (a == "--genre")
        {
            const auto g = next().toLowerCase();
            applyGenreStartingPoint (s, g.startsWith ("rap") || g == "trap" || g.startsWith ("hip") ? Genre::rapTrap
                                        : g.startsWith ("arab") || g == "pop" || g.startsWith ("r&b") || g == "rnb" ? Genre::arabeskPop : Genre::electronicLofi);
        }
        else if (a == "--glue") { s.comp.enabled = true; s.comp.autoThreshold = true; s.comp.glueDb = next().getDoubleValue(); }
        else if (a == "--threshold") { s.comp.enabled = true; s.comp.autoThreshold = false; s.comp.thresholdDb = next().getDoubleValue(); }
        else if (a == "--mode") { s.comp.enabled = true; s.comp.mode = next().containsIgnoreCase ("3") ? CompMode::threeBand : CompMode::wide; }
        else if (a == "--air") { s.eq.airDb = next().getDoubleValue(); }
        else if (a == "--auto-trim") s.autoTrim = true;
        else if (a == "--scenario") o.scenarios.add (next());
        else { std::fprintf (stderr, "Unknown option %s\n", a.toRawUTF8()); return false; }
    }
    return true;
}

/** render --batch <dir|file...> <outdir> [--album] [--per-track] [mastering options]: the app's batch queue from the command line. */
int batchCli (int argc, char** argv)
{
    if (argc < 4) return 2;
    const auto cwd = juce::File::getCurrentWorkingDirectory();
    const juce::File src (cwd.getChildFile (juce::String::fromUTF8 (argv[2])));
    const juce::File outDir (cwd.getChildFile (juce::String::fromUTF8 (argv[3])));
    MasterSettings s;
    CliOptions o;
    if (! parseMasterOptions (argc, argv, 4, s, o)) return 2;
    if (o.refPath.isNotEmpty() || ! o.scenarios.isEmpty()) { std::fprintf (stderr, "--batch: --reference and --scenario are per-file options; use --per-track for each file's own diagnosis\n"); return 2; }
    juce::Array<juce::File> files;
    if (src.isDirectory())
    {
        for (const auto& e : juce::RangedDirectoryIterator (src, false, "*.wav;*.aif;*.aiff", juce::File::findFiles)) files.add (e.getFile());
        files.sort();
    }
    else if (src.existsAsFile()) files.add (src);
    if (files.isEmpty()) { std::fprintf (stderr, "No WAV / AIFF files in %s\n", src.getFullPathName().toRawUTF8()); return 1; }
    std::vector<batch::Job> jobs;
    for (auto& f : files) { batch::Job j; j.file = f; j.settings = s; j.applyDiagnosis = o.perTrack; jobs.push_back (j); }
    batch::Options bo;
    bo.album = o.album;
    bo.outDir = outDir;
    bo.bits = o.bits;
    int lastPct = -1;
    const auto res = batch::run (jobs, bo, [&lastPct] (int i, int n, double f, const juce::String& stage)
    {
        const int pct = (int) std::floor (f * 100.0);
        if (pct / 5 != lastPct / 5 || i >= n) { std::printf ("[%3d%%] %s\n", pct, stage.toRawUTF8()); std::fflush (stdout); lastPct = pct; }
    });
    if (o.album && res.plan.valid)
        std::printf ("ALBUM: target %s LUFS, loudest input %s LUFS, mean tilt %s dB/oct\n", fmt (res.plan.targetLufs).toRawUTF8(),
                     fmt (res.plan.loudestLufs, 2).toRawUTF8(), fmt (res.plan.meanSlope, 2).toRawUTF8());
    std::printf ("%-34s %8s %8s %8s %7s %7s %7s %7s\n", "TRACK", "IN LUFS", "OUT", "TARGET", "TP", "LRA", "TILT>", "NUDGE");
    int bad = 0;
    for (auto& t : res.tracks)
    {
        if (! t.ok) { std::printf ("%-34s ERROR %s\n", t.source.getFileName().substring (0, 34).toRawUTF8(), t.error.toRawUTF8()); ++bad; continue; }
        std::printf ("%-34s %8s %8s %8s %7s %7s %7s %7s\n", t.source.getFileName().substring (0, 34).toRawUTF8(), fmt (t.in.integrated, 2).toRawUTF8(),
                     fmt (t.out.integrated, 2).toRawUTF8(), fmt (t.targetLufs, 2).toRawUTF8(), fmt (t.out.truePeak, 2).toRawUTF8(), fmt (t.out.lra, 1).toRawUTF8(),
                     fmt (t.outSlope, 2).toRawUTF8(), fmt (t.tiltNudge, 2).toRawUTF8());
    }
    std::printf ("%s\nReport: %s, %s\n", res.message.toRawUTF8(), res.csv.getFullPathName().toRawUTF8(), res.json.getFileName().toRawUTF8());
    return bad == 0 && ! res.cancelled ? 0 : 1;
}

int master (int argc, char** argv)
{
    if (argc < 4 || ! Entitlement::mayBatchMaster()) return 2;
    const juce::File inFile (juce::File::getCurrentWorkingDirectory().getChildFile (juce::String::fromUTF8 (argv[2])));
    const juce::File outFile (juce::File::getCurrentWorkingDirectory().getChildFile (juce::String::fromUTF8 (argv[3])));
    MasterSettings s;
    CliOptions o;
    if (! parseMasterOptions (argc, argv, 4, s, o)) return 2;
    auto& scenarios = o.scenarios;
    const int bits = o.bits;
    const bool doMatch = o.doMatch;
    const auto refPath = o.refPath;
    const double tone = o.tone;
    auto in = loadAudioFile (inFile);
    if (! in.ok()) { std::fprintf (stderr, "%s\n", in.error.toRawUTF8()); return 1; }
    if (in.note.isNotEmpty()) std::printf ("Note: %s\n", in.note.toRawUTF8());
    const auto before = analyse (in.buffer, in.sampleRate);
    printMeasurement ("INPUT", before);
    if (! scenarios.isEmpty())
    {
        // --scenario <id|number|found>: apply scenarios with the amounts measured on this file ("found" = every finding).
        const auto dm = measureMix (in.buffer, in.sampleRate);
        const auto dg = diagnose (dm, s);
        for (auto& id : scenarios)
        {
            if (id == "found") { for (auto& f : dg.findings) { applyScenario (s, f.scenario, &dm); std::printf ("SCENARIO %s\n", f.id.toRawUTF8()); } continue; }
            const int sc = id.containsOnly ("0123456789") ? id.getIntValue() : scenarioFromId (id);
            if (sc < 1 || sc > kNumScenarios) { std::fprintf (stderr, "Unknown scenario %s\n", id.toRawUTF8()); return 2; }
            applyScenario (s, sc, &dm);
            std::printf ("SCENARIO %s\n", scenarioId (sc));
        }
    }

    Measurement refM;
    if (refPath.isNotEmpty())
    {
        auto ref = loadAudioFile (juce::File::getCurrentWorkingDirectory().getChildFile (refPath));
        if (! ref.ok()) { std::fprintf (stderr, "Reference: %s\n", ref.error.toRawUTF8()); return 1; }
        refM = analyse (ref.buffer, ref.sampleRate);
        printMeasurement ("REFERENCE", refM);
        if (tone > 0.0)
        {
            s.eq.matchCurveDb = MasterChain::toneMatchCurve (before.spectrumDb, refM.spectrumDb, s.eq, s.toneSmoothOct);
            s.eq.matchAmount = juce::jlimit (0.0, 1.0, tone / 100.0);
        }
    }

    PreLimiterCache cache;
    RenderResult r;
    if (doMatch)
    {
        const auto m = MasterChain::matchTarget (in.buffer, in.sampleRate, s, cache, &r);
        std::printf ("MATCH TARGET: %s\n", m.message.toRawUTF8());
        MasterChain::storeSolvedThresholds (s, r.glue);
    }
    else
        r = MasterChain::render (in.buffer, in.sampleRate, s, cache);
    if (r.glue.valid)
        std::printf ("GLUE: input PLR %s LU, crest %s dB, LRA %s LU -> %s; %s threshold %s dBFS, ratio %s:1, target %s dB, loud-part GR avg %s dB / max %s dB, make-up %s dB\n",
                     fmt (r.glue.plr).toRawUTF8(), fmt (r.glue.crest).toRawUTF8(), fmt (r.glue.lra).toRawUTF8(),
                     r.glue.dense ? ("DENSE (" + r.glue.denseReason + ")").toRawUTF8() : "not dense",
                     r.glue.autoThreshold ? "AUTO" : "manual", fmt (r.glue.thresholdDb, 2).toRawUTF8(), fmt (r.glue.ratio, 2).toRawUTF8(),
                     fmt (r.glue.targetGrDb, 2).toRawUTF8(), fmt (r.glue.loudMeanGrDb, 2).toRawUTF8(), fmt (r.glue.loudMaxGrDb, 2).toRawUTF8(),
                     fmt (r.glue.makeupDb, 2).toRawUTF8());
    printMeasurement ("OUTPUT", r.measured);
    std::printf ("LOUDNESS: %s\n", MasterChain::loudnessStory (s, before.integrated, r.measured.integrated, r.limiter.gr.maxDb).sentence.toRawUTF8());
    for (auto& w : r.warnings) std::printf ("WARNING: %s\n", w.toRawUTF8());

    juce::String err;
    int clipped = 0;
    if (! exportWav (outFile, *r.output, in.sampleRate, bits, err, &clipped)) { std::fprintf (stderr, "%s\n", err.toRawUTF8()); return 1; }
    const auto reportFile = outFile.getSiblingFile (outFile.getFileNameWithoutExtension() + ".mihenk.txt");
    reportFile.replaceWithText (MasterChain::report (inFile.getFileName(), outFile.getFileName(), bits, s, before, r.measured, r,
                                                     refM.valid ? &refM : nullptr));
    std::printf ("Wrote %s and %s\n", outFile.getFullPathName().toRawUTF8(), reportFile.getFileName().toRawUTF8());
    return 0;
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::String mode = argc >= 2 ? juce::String (argv[1]) : juce::String();
    if (mode == "--selftest") return selftest();
    if (mode == "--master") return master (argc, argv);
    if (mode == "--batch" || mode == "batch") return batchCli (argc, argv);
    if (mode == "--synth" && argc >= 4)
    {
        // render --synth <scenario id|0 for the clean mix> out.wav : writes the selftest's synthetic case (48 kHz, float).
        const juce::String id = juce::String::fromUTF8 (argv[2]);
        const int sc = id.containsOnly ("0123456789") ? id.getIntValue() : scenarioFromId (id);
        const auto b = scenarioSignal (sc, 48000.0);
        juce::String err;
        if (! exportWav (juce::File::getCurrentWorkingDirectory().getChildFile (juce::String::fromUTF8 (argv[3])), b, 48000.0, 24, err)) { std::fprintf (stderr, "%s\n", err.toRawUTF8()); return 1; }
        return 0;
    }
    if (mode == "--diagnose" && argc >= 3)
    {
        for (int i = 2; i < argc; ++i)
        {
            auto in = loadAudioFile (juce::File::getCurrentWorkingDirectory().getChildFile (juce::String::fromUTF8 (argv[i])));
            if (! in.ok()) { std::fprintf (stderr, "%s: %s\n", argv[i], in.error.toRawUTF8()); continue; }
            std::printf ("\n== %s (%.1f s, %.0f Hz, %d ch)\n", argv[i], in.buffer.getNumSamples() / in.sampleRate, in.sampleRate, in.buffer.getNumChannels());
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            const auto d = diagnose (in.buffer, in.sampleRate, MasterSettings{});
            printDiagnosis (d);
            std::printf ("  (diagnosis %.2f s)\n", (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0);
        }
        return 0;
    }
    if (mode == "--measure" && argc >= 3)
    {
        auto in = loadAudioFile (juce::File::getCurrentWorkingDirectory().getChildFile (juce::String::fromUTF8 (argv[2])));
        if (! in.ok()) { std::fprintf (stderr, "%s\n", in.error.toRawUTF8()); return 1; }
        printMeasurement (argv[2], analyse (in.buffer, in.sampleRate));
        return 0;
    }
    std::printf ("render --selftest\nrender --measure <file>\nrender --master <in.wav> <out.wav> [--target -14|keep] [--ceiling -1] [--bits 24|16]\n"
                 "       [--genre rap|pop|electronic] [--glue dB | --threshold dBFS] [--reference ref.wav] [--tone 0-100] [--linear] [--warn 6] [--no-match] [--drive dB]\n"
                 "       [--mode wide|3band] [--air dB] [--auto-trim] [--scenario id|1-20|found]...\n"
                 "render --batch <dir|file> <outdir> [--album] [--per-track] [mastering options as --master]\nrender --diagnose <file> [file...]\n");
    return 2;
}
