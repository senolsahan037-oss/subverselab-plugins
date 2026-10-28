// flraw --sr 48000 [--level 0..1] --tone hz dbfs seconds [--then-silence seconds] [--trace band] [--time-it]
// flraw --sr 48000 [--level 0..1] --wav in.wav [--trace band] [--time-it]
// Offline runner for Source/Ge1100Analyser.h (the original's spectrum analyser). Feeds a sine (both channels) or a WAV,
// then prints, per band, the last read bus voltage and the lit segments. --trace prints that band's bus voltage every
// 10 ms (t_s bus_v segments). --taps prints the ladder.
#include "../Source/Ge1100Analyser.h"
#include "wavio.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

int main (int argc, char** argv)
{
    double sr = 48000, level = 1, hz = 1000, dbfs = -20, secs = 1, silence = 0; int trace = -1; bool timing = false, taps = false;
    const char* wav = nullptr;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a (argv[i]);
        auto next = [&] { if (i + 1 >= argc) { std::fprintf (stderr, "%s needs a value\n", a.c_str()); std::exit (2); } return argv[++i]; };
        if (a == "--sr") sr = std::atof (next());
        else if (a == "--level") level = std::atof (next());
        else if (a == "--tone") { hz = std::atof (next()); dbfs = std::atof (next()); secs = std::atof (next()); }
        else if (a == "--then-silence") silence = std::atof (next());
        else if (a == "--trace") trace = std::atoi (next());
        else if (a == "--wav") wav = next();
        else if (a == "--time-it") timing = true;
        else if (a == "--taps") taps = true;
        else { std::fprintf (stderr, "unknown flag %s\n", a.c_str()); return 2; }
    }
    gefl::Analyser an;
    Wav w;
    if (wav) { w = readWav (wav); sr = w.sr; if (w.ch == 1) w.r = w.l; }
    an.prepare (sr); an.setLevel (level);
    if (taps) { for (int i = 0; i < gefl::kSegments; ++i) std::printf ("tap %d: %.4f V\n", i + 1, an.tapVolts (i)); return 0; }
    const size_t n = wav ? w.l.size() : (size_t) ((secs + silence) * sr), on = wav ? n : (size_t) (secs * sr);
    const double a = std::pow (10.0, dbfs / 20.0), every = 0.010 * sr;
    double nextTrace = 0;
    const std::clock_t c0 = std::clock();
    for (size_t i = 0; i < n; ++i)
    {
        float l, r;
        if (wav) { l = w.l[i]; r = w.r[i]; }
        else { l = r = i < on ? (float) (a * std::sin (2 * gefl::kPi * hz * (double) i / sr)) : 0.0f; }
        an.process (l, r);
        if (trace >= 0 && (double) i >= nextTrace) { std::printf ("%.3f %.4f %d\n", (double) i / sr, an.busVolts (trace), an.segments (trace)); nextTrace += every; }
    }
    const double proc = (double) (std::clock() - c0) / CLOCKS_PER_SEC;
    if (trace < 0)
    {
        for (int b = 0; b < gefl::kBands; ++b) std::printf ("band %2d %6.0f Hz: bus %.4f V, segments %d\n", b, gefl::kBandValues[(size_t) b].label, an.busVolts (b), an.segments (b));
        std::printf ("TOTAL: bus %.4f V, segments %d\n", an.busVolts (gefl::kBands), an.segments (gefl::kBands));
    }
    if (timing) std::printf ("realtime_x=%.1f\n", ((double) n / sr) / std::max (proc, 1e-9));
    return 0;
}
