#pragma once
// The swell test/demo material: the loudest 3 s phrase of the reference vocal
// (10 ms search grid, 5 ms edge fades) followed by silence.
#include "wav.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

inline double spanRmsSt(const std::vector<float>& x, int sr, double t0, double t1) {
    const size_t a = static_cast<size_t>(std::max(0.0, t0) * sr) * 2, b = std::min(x.size(), static_cast<size_t>(std::max(0.0, t1) * sr) * 2);
    double s = 0; size_t n = 0; for (size_t i = a; i < b; ++i) { s += double(x[i]) * x[i]; ++n; }
    return std::sqrt(s / std::max<size_t>(1, n));
}

struct SwellSource { int sr = 0; std::vector<float> src, dry; double phraseStart = 0, phraseRms = 0; std::vector<double> per2s; bool ok = false; };

inline SwellSource loadSwellSource(const std::string& path, double phraseSec = 3.0, double silenceSec = 9.0) {
    SwellSource S; WavData w;
    if (!readWav(path, w) || w.channels < 1) return S;
    S.sr = w.sampleRate; const size_t fr = w.samples.size() / w.channels;
    S.src.resize(fr * 2);
    for (size_t i = 0; i < fr; ++i) { S.src[2 * i] = w.samples[i * w.channels]; S.src[2 * i + 1] = w.samples[i * w.channels + (w.channels > 1 ? 1 : 0)]; }
    for (double t = 0; t + 1.0 < double(fr) / S.sr; t += 2.0) S.per2s.push_back(20.0 * std::log10(std::max(1e-12, spanRmsSt(S.src, S.sr, t, t + 2.0))));
    const size_t P = static_cast<size_t>(phraseSec * S.sr), hop = static_cast<size_t>(0.01 * S.sr);
    size_t best = 0; double br = -1;
    for (size_t a = 0; a + P <= fr; a += hop) { const double r = spanRmsSt(S.src, S.sr, double(a) / S.sr, double(a + P) / S.sr); if (r > br) { br = r; best = a; } }
    S.phraseStart = double(best) / S.sr;
    S.dry.assign(static_cast<size_t>((phraseSec + silenceSec) * S.sr) * 2, 0.0f);
    const size_t fadeN = static_cast<size_t>(0.005 * S.sr);
    for (size_t i = 0; i < P; ++i) {
        double g = 1.0; if (i < fadeN) g = double(i) / fadeN; if (i >= P - fadeN) g = double(P - 1 - i) / fadeN;
        S.dry[2 * i] = static_cast<float>(S.src[2 * (best + i)] * g); S.dry[2 * i + 1] = static_cast<float>(S.src[2 * (best + i) + 1] * g);
    }
    S.phraseRms = spanRmsSt(S.dry, S.sr, 0.0, phraseSec); S.ok = true;
    return S;
}
