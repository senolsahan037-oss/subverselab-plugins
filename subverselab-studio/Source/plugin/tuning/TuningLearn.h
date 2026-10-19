// Learn a tuning from a recording of a solo melodic line (SPEC.md "Tuning Lab" 2). Plain C++17, no JUCE.
//
// 1. YIN pitch track (de Cheveigné & Kawahara 2002: difference function, cumulative mean normalisation, absolute
//    threshold, parabolic interpolation) on the input decimated to ~16 kHz, frame 1024 (64 ms), hop 128, 55..1500 Hz;
//    frames 30 dB under the peak are unvoiced.
// 2. Stable notes: the track is smoothed over ~180 ms (one vibrato cycle); runs whose smoothed pitch stays within +-12 ct
//    of the run's running median for >= 70 ms are notes; pitch = mean of the smoothed frames whose window lies inside the note.
// 3. Tonic: the pitch class (cents mod 1200) held longest (duration-weighted histogram, 1-ct bins, 7-ct smoothing),
//    unless the caller gives one.
// 4. Degrees: peaks of the same histogram of the notes relative to the tonic (>= 1.5 % of the held time or >= 3 notes,
//    >= 35 ct apart).
// 5. Direction: a note reached from a lower note (inside one phrase, gap <= 1.5 s) counts as rising, from a higher note as
//    falling. Each degree gets the duration-weighted mean of its rising notes and of its falling notes within +-30 ct;
//    when both have >= 2 notes and differ by >= 8 ct the tuning is directional.
#pragma once
#include "TuningLab.h"
#include <map>

namespace jflab
{
struct LearnResult
{
    bool ok = false;
    std::string message;
    Tuning tuning;
    double tonicPitch = 0;           // semitones (MIDI scale) of the learned degree 0
    int notes = 0;                   // stable notes found
    double heldSeconds = 0;
};

namespace learn
{
// YIN on one frame; returns Hz or 0 (unvoiced)
inline double yin (const float* x, int n, double sr, double fmin, double fmax, std::vector<double>& d, double threshold = 0.12)
{
    const int maxLag = std::min (n / 2, (int) (sr / fmin) + 2), minLag = std::max (2, (int) (sr / fmax));
    const int w = n - maxLag;
    if (w < 64 || maxLag <= minLag) return 0;
    d.assign ((size_t) maxLag + 1, 0.0);
    for (int tau = 1; tau <= maxLag; ++tau)
    {
        double s = 0;
        for (int i = 0; i < w; ++i) { const double e = (double) x[i] - x[i + tau]; s += e * e; }
        d[(size_t) tau] = s;
    }
    // cumulative mean normalised difference
    double run = 0;
    d[0] = 1;
    for (int tau = 1; tau <= maxLag; ++tau) { run += d[(size_t) tau]; d[(size_t) tau] = run > 0 ? d[(size_t) tau] * tau / run : 1; }
    int tau = -1;
    for (int t = minLag; t < maxLag; ++t)
        if (d[(size_t) t] < threshold)
        {
            while (t + 1 < maxLag && d[(size_t) t + 1] < d[(size_t) t]) ++t;
            tau = t;
            break;
        }
    if (tau < 0) return 0;
    // parabolic interpolation
    const double a = d[(size_t) tau - 1], b = d[(size_t) tau], c = d[(size_t) tau + 1];
    const double den = a - 2 * b + c;
    const double shift = std::fabs (den) > 1e-12 ? 0.5 * (a - c) / den : 0.0;
    return sr / ((double) tau + std::clamp (shift, -1.0, 1.0));
}

struct Note { double start, dur, pitch; };   // seconds, seconds, semitones

inline double median (std::vector<double> v)
{
    if (v.empty()) return 0;
    std::nth_element (v.begin(), v.begin() + (long) v.size() / 2, v.end());
    return v[v.size() / 2];
}

// decimate to about 16 kHz (windowed-sinc low-pass, 63 taps): YIN needs nothing above 1.5 kHz and runs ~9x faster
inline std::vector<float> decimate (const float* x, int n, double sr, double& outSr)
{
    const int f = std::max (1, (int) std::floor (sr / 16000.0));
    outSr = sr / f;
    if (f == 1) return std::vector<float> (x, x + n);
    const int taps = 63, mid = taps / 2;
    const double fc = 0.45 / f;
    std::vector<double> h ((size_t) taps);
    double sum = 0;
    for (int i = 0; i < taps; ++i)
    {
        const double t = i - mid, sinc = t == 0 ? 2 * fc : std::sin (2 * 3.14159265358979 * fc * t) / (3.14159265358979 * t);
        h[(size_t) i] = sinc * (0.54 - 0.46 * std::cos (2 * 3.14159265358979 * i / (taps - 1)));
        sum += h[(size_t) i];
    }
    std::vector<float> y;
    y.reserve ((size_t) (n / f + 1));
    for (int o = 0; o < n; o += f)
    {
        double acc = 0;
        for (int i = 0; i < taps; ++i) { const int k = o + i - mid; if (k >= 0 && k < n) acc += h[(size_t) i] * x[k]; }
        y.push_back ((float) (acc / sum));
    }
    return y;
}

inline std::vector<Note> notes (const float* mono, int n, double sr, const std::atomic<bool>* cancel = nullptr)
{
    const int frame = sr < 24000 ? 1024 : 2048, hop = frame / 8;
    std::vector<double> d, pitch;
    double peak = 0;
    for (int i = 0; i < n; ++i) peak = std::max (peak, (double) std::fabs (mono[i]));
    const double gate = peak * 0.03;                        // -30 dB under the peak
    for (int s = 0; s + frame <= n; s += hop)
    {
        if (cancel != nullptr && cancel->load() && (s / hop) % 64 == 0) return {};
        double rms = 0;
        for (int i = 0; i < frame; ++i) rms += (double) mono[s + i] * mono[s + i];
        rms = std::sqrt (rms / frame);
        const double hz = rms > gate * 0.5 ? yin (mono + s, frame, sr, 55, 1500, d) : 0;
        pitch.push_back (hz > 0 ? hzToPitch (hz) : -1);
    }
    const double frameSec = hop / sr, minDur = 0.07;
    // smoothed track: centred mean over ~180 ms of voiced frames (one cycle of a 5.5 Hz vibrato), so vibrato does not
    // split a note; segment boundaries come from unvoiced frames and from jumps of the smoothed track
    const int half = std::max (1, (int) std::lround (0.09 / frameSec));
    std::vector<double> smooth (pitch.size(), -1);
    for (int f = 0; f < (int) pitch.size(); ++f)
    {
        if (pitch[(size_t) f] < 0) continue;
        double s = 0; int c = 0;
        for (int k = f - half; k <= f + half; ++k)
        {
            if (k < 0 || k >= (int) pitch.size()) continue;
            if (pitch[(size_t) k] < 0) continue;
            if (std::fabs (pitch[(size_t) k] - pitch[(size_t) f]) > 1.0) continue;   // another note: not mixed in
            s += pitch[(size_t) k]; ++c;
        }
        smooth[(size_t) f] = s / c;
    }
    std::vector<Note> out;
    std::vector<double> run, raw;
    int runStart = 0;
    auto flush = [&] (int endFrame)
    {
        if ((double) run.size() * frameSec >= minDur)
        {
            // pitch: the smoothed frames whose whole window lies inside the note (each is a mean over one vibrato
            // cycle); a note shorter than a window falls back to the raw frames without the first / last 30 ms
            double s = 0; int c = 0;
            for (int i = half; i < (int) run.size() - half; ++i) { s += run[(size_t) i]; ++c; }
            if (c == 0)
            {
                const int edge = std::min ((int) raw.size() / 4, (int) std::lround (0.03 / frameSec));
                for (int i = edge; i < (int) raw.size() - edge; ++i) { s += raw[(size_t) i]; ++c; }
            }
            out.push_back ({ runStart * frameSec, (double) run.size() * frameSec, c > 0 ? s / c : median (raw) });
        }
        run.clear(); raw.clear();
        runStart = endFrame;
    };
    for (int f = 0; f < (int) pitch.size(); ++f)
    {
        const double p = smooth[(size_t) f];
        if (p < 0) { flush (f + 1); continue; }
        if (! run.empty())
        {
            const double m = median (std::vector<double> (run.end() - std::min<long> ((long) run.size(), 9), run.end()));
            if (std::fabs (p - m) * 100.0 > 12.0) flush (f);
        }
        if (run.empty()) runStart = f;
        run.push_back (p);
        raw.push_back (pitch[(size_t) f]);
    }
    flush ((int) pitch.size());
    // trim the onset / release frames of each note (YIN is least sure there): median already ignores most of it
    return out;
}

// duration-weighted circular histogram (1-ct bins over 1200), Gaussian smoothing sigma 7 ct
inline std::vector<double> histogram (const std::vector<std::pair<double, double>>& centsWeight, double sigma = 7.0)
{
    std::vector<double> h (1200, 0.0), out (1200, 0.0);
    for (auto& [c, w] : centsWeight)
    {
        double m = std::fmod (c, 1200.0);
        if (m < 0) m += 1200;
        h[(size_t) std::lround (m) % 1200] += w;
    }
    const int r = (int) std::ceil (3 * sigma);
    for (int i = 0; i < 1200; ++i)
    {
        if (h[(size_t) i] == 0) continue;
        for (int k = -r; k <= r; ++k)
            out[(size_t) ((i + k + 1200) % 1200)] += h[(size_t) i] * std::exp (-0.5 * k * k / (sigma * sigma));
    }
    return out;
}

// weighted circular mean of the values within +-win cents of `around`
inline bool meanNear (const std::vector<std::pair<double, double>>& cw, double around, double win, double& mean, double& weight, int* count = nullptr)
{
    double s = 0, w = 0;
    int n = 0;
    for (auto& [c, wt] : cw)
    {
        double diff = std::fmod (c - around, 1200.0);
        if (diff > 600) diff -= 1200;
        if (diff < -600) diff += 1200;
        if (std::fabs (diff) <= win) { s += (around + diff) * wt; w += wt; ++n; }
    }
    if (count) *count = n;
    if (w <= 0) return false;
    mean = s / w; weight = w;
    return true;
}
} // namespace learn

// tonicPitchClassCents < 0: find the tonic; else cents (mod 1200, relative to C) of the tonic
inline LearnResult learnTuning (const float* mono, int n, double sr, double tonicPitchClassCents = -1, const std::string& name = "Learned",
                                const std::atomic<bool>* cancel = nullptr)
{
    LearnResult res;
    if (n < (int) (sr * 0.5)) { res.message = "The recording is shorter than half a second."; return res; }
    double dsr = sr;
    const auto dec = learn::decimate (mono, n, sr, dsr);
    const auto ns = learn::notes (dec.data(), (int) dec.size(), dsr, cancel);
    res.notes = (int) ns.size();
    if (ns.size() < 8) { res.message = "Fewer than 8 steady notes were found; a solo line with held notes works best."; return res; }

    // cents relative to C (pitch 60 -> 0)
    std::vector<std::pair<double, double>> all;
    for (auto& nt : ns) { all.push_back ({ (nt.pitch - 60.0) * 100.0, nt.dur }); res.heldSeconds += nt.dur; }
    double tonic = tonicPitchClassCents;
    if (tonic < 0)
    {
        const auto h = learn::histogram (all);
        tonic = (double) (std::max_element (h.begin(), h.end()) - h.begin());
        double m, w;
        if (learn::meanNear (all, tonic, 25, m, w)) tonic = m;   // refine to the notes themselves, not the bin
    }
    tonic = std::fmod (tonic + 1200.0, 1200.0);

    // relative to the tonic, with direction
    std::vector<std::pair<double, double>> rel, up, down;
    for (size_t i = 0; i < ns.size(); ++i)
    {
        const double c = std::fmod ((ns[i].pitch - 60.0) * 100.0 - tonic + 120000.0, 1200.0);
        rel.push_back ({ c, ns[i].dur });
        if (i == 0) continue;
        const auto& prev = ns[i - 1];
        if (ns[i].start - (prev.start + prev.dur) > 1.5) continue;
        const double step = (ns[i].pitch - prev.pitch) * 100.0;
        if (step > 15) up.push_back ({ c, ns[i].dur });
        else if (step < -15) down.push_back ({ c, ns[i].dur });
    }
    const auto h = learn::histogram (rel);
    // peaks (local maxima on the circle), strongest first
    std::vector<std::pair<double, int>> peaks;
    for (int i = 0; i < 1200; ++i)
    {
        const double v = h[(size_t) i], l = h[(size_t) ((i + 1199) % 1200)], r = h[(size_t) ((i + 1) % 1200)];
        if (v > 0 && v >= l && v > r) peaks.push_back ({ v, i });
    }
    std::sort (peaks.begin(), peaks.end(), [] (auto& x, auto& y) { return x.first > y.first; });
    // a degree: a peak holding >= 1.5 % of the held time, or reached by >= 3 notes (passing steps are short but they are
    // steps; measured 18 Oct on a Hicaz line: neva = 6 notes, 0.56 s of 21 s). The smoothed peak height of one note of
    // weight w is w (kernel peak = 1).
    const double minPeak = 0.015 * res.heldSeconds;
    std::vector<int> chosen;
    for (auto& [v, i] : peaks)
    {
        double mm, ww; int cnt = 0;
        learn::meanNear (rel, i, 25, mm, ww, &cnt);
        if (v < minPeak && cnt < 3) continue;
        bool far = true;
        for (int c : chosen) { int dd = std::abs (c - i); dd = std::min (dd, 1200 - dd); if (dd < 35) far = false; }
        if (far) chosen.push_back (i);
    }
    // degree 0 is the tonic: make sure it is in (the bin nearest 0)
    std::vector<double> degs;
    for (int i : chosen)
    {
        double m, w;
        if (! learn::meanNear (rel, i, 30, m, w)) continue;
        m = std::fmod (m + 1200.0, 1200.0);
        if (m > 1200 - 17.5 || m < 17.5) continue;   // the tonic itself
        degs.push_back (m);
    }
    std::sort (degs.begin(), degs.end());
    if (degs.size() < 2) { res.message = "Fewer than 3 scale degrees were clear enough."; return res; }

    Tuning t;
    t.name = name;
    t.cents = degs;
    t.cents.push_back (1200.0);
    std::vector<double> fall = degs;
    bool anyDir = false;
    t.confidence.push_back (1.0f);
    for (size_t k = 0; k < degs.size(); ++k)
    {
        double mu, wu, md, wd, m, w;
        learn::meanNear (rel, degs[k], 30, m, w);
        t.confidence.push_back ((float) std::min (1.0, w / (0.10 * res.heldSeconds)));
        int nu = 0, nd = 0;
        const bool hu = learn::meanNear (up, degs[k], 30, mu, wu, &nu), hd = learn::meanNear (down, degs[k], 30, md, wd, &nd);
        // directional only on evidence: >= 2 notes each way and >= 8 ct apart (a synthetic line with no direction showed
        // 3-7 ct between rising and falling notes from onset glides alone; the steps that do differ differ by about a comma)
        if (hu && hd && nu >= 2 && nd >= 2 && std::fabs (mu - md) >= 8.0)
        {
            t.cents[k] = std::fmod (mu + 1200.0, 1200.0);
            fall[k] = std::fmod (md + 1200.0, 1200.0);
            anyDir = true;
        }
    }
    if (anyDir) { fall.push_back (1200.0); t.fall = fall; }

    // mapping: linear, degree 0 on the MIDI key nearest the tonic around middle C, at the tonic's own frequency
    const double tonicPitch = 60.0 + tonic / 100.0;
    t.mapSize = 0;
    t.middleKey = t.refKey = (int) std::lround (tonicPitch);
    t.refHz = pitchToHz (tonicPitch);
    res.tuning = t;
    res.tonicPitch = tonicPitch;
    res.ok = true;
    res.message = std::to_string (t.size()) + " degrees from " + std::to_string (res.notes) + " notes" + (anyDir ? ", directional" : "");
    return res;
}
} // namespace jflab
