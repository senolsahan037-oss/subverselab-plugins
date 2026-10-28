// eqraw in.wav out.wav [--lowfreq 40|60|100|150] [--lowboost dB] [--lowcut dB] [--presfreq 700|1400|2800|4000|5600]
//       [--pres dB] [--highfreq 5000|8000|12000] [--high dB] [--drive dB] [--out dB] [--bypass] [--block N] [--time-it]
//       [--latency]   prints the engine latency in samples and exits
// Offline renderer: runs Source/ProgrammeEqEngine.h exactly as the plug-in will. WAV in: PCM 16/24/32 or float32, mono or
// stereo; WAV out: float32, same channels. --time-it prints "audio_s=... proc_s=... realtime_x=..." (processing only).
#include "../Source/ProgrammeEqEngine.h"
#include "wavio.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

template <size_t N>
static int indexOf (const std::array<float, N>& a, float v, const char* what)
{
    for (size_t i = 0; i < N; ++i) if (std::fabs (a[i] - v) < 0.5f) return (int) i;
    std::fprintf (stderr, "%s: %g is not a stepped position\n", what, (double) v); std::exit (2);
}

int main (int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) if (std::string (argv[i]) == "--latency") { std::printf ("%d\n", peq::Engine::latency()); return 0; }
    if (argc < 3) { std::fprintf (stderr, "usage: eqraw in.wav out.wav [--lowfreq Hz] [--lowboost dB] [--lowcut dB] [--presfreq Hz] [--pres dB] [--highfreq Hz] [--high dB] [--drive dB] [--out dB] [--bypass] [--block N] [--time-it] | --latency\n"); return 2; }
    peq::Params p; int block = 512; bool timing = false;
    for (int i = 3; i < argc; ++i)
    {
        const std::string a (argv[i]);
        auto next = [&] { if (i + 1 >= argc) { std::fprintf (stderr, "%s needs a value\n", a.c_str()); std::exit (2); } return (float) std::atof (argv[++i]); };
        if (a == "--lowfreq") p.lowFreq = indexOf (peq::kLowFreqs, next(), "--lowfreq");
        else if (a == "--lowboost") p.lowBoostDb = next();
        else if (a == "--lowcut") p.lowCutDb = next();
        else if (a == "--presfreq") p.presFreq = indexOf (peq::kPresFreqs, next(), "--presfreq");
        else if (a == "--pres") p.presDb = next();
        else if (a == "--highfreq") p.highFreq = indexOf (peq::kHighFreqs, next(), "--highfreq");
        else if (a == "--high") p.highDb = next();
        else if (a == "--drive") p.driveDb = next();
        else if (a == "--out") p.outputDb = next();
        else if (a == "--bypass") p.in = false;
        else if (a == "--block") block = (int) next();
        else if (a == "--time-it") timing = true;
        else { std::fprintf (stderr, "unknown flag %s\n", a.c_str()); return 2; }
    }
    Wav w = readWav (argv[1]);
    peq::Engine e; e.prepare (w.sr); e.setParams (p);
    const auto t0 = std::chrono::steady_clock::now();
    const int n = (int) w.l.size();
    for (int s = 0; s < n; s += block)
    {
        const int m = std::min (block, n - s);
        e.process (w.l.data() + s, w.ch == 2 ? w.r.data() + s : nullptr, m);
    }
    const double proc = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
    writeWav (argv[2], w.sr, w.ch, w.l, w.r);
    if (timing) { const double au = (double) n / w.sr; std::printf ("audio_s=%.3f proc_s=%.4f realtime_x=%.1f\n", au, proc, au / std::max (proc, 1e-9)); }
    return 0;
}
