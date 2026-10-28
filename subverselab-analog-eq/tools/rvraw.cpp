// rvraw in.wav out.wav [--delay 0..10] [--depth 0..10] [--off] [--tail sec] [--loop-trim x] [--time-it] | --info
// --loop-trim scales the loop gain (calibration only).
// Offline renderer for Source/Ge1100Reverb.h (the original's reverberation section), run exactly as the plug-in will.
// Mono input is processed as both channels; the output is stereo. --tail appends silence so the repeats can ring out.
// --info prints the loop gain at DEPTH 10 and the delay for each DELAY mark.
#include "../Source/Ge1100Reverb.h"
#include "wavio.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

int main (int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
        if (std::string (argv[i]) == "--info")
        {
            gerv::Reverb r; r.prepare (48000);
            std::printf ("loop gain at DEPTH 10: %.6f, peak per pass %.4f (%.2f dB)\n", r.loopGainMax(), r.loopPeakPerPass(), 20 * std::log10 (r.loopPeakPerPass()));
            for (int k = 0; k <= 10; ++k) { r.setDelay (k); std::printf ("DELAY %2d: %.1f ms\n", k, 1000 * r.delaySeconds()); }
            return 0;
        }
    if (argc < 3) { std::fprintf (stderr, "usage: rvraw in.wav out.wav [--delay 0..10] [--depth 0..10] [--off] [--tail sec] [--loop-trim x] [--time-it] | --info\n"); return 2; }
    double delay = 5, depth = 5, tail = 0, trim = -1; bool off = false, timing = false;
    for (int i = 3; i < argc; ++i)
    {
        const std::string a (argv[i]);
        auto next = [&] { if (i + 1 >= argc) { std::fprintf (stderr, "%s needs a value\n", a.c_str()); std::exit (2); } return std::atof (argv[++i]); };
        if (a == "--delay") delay = next();
        else if (a == "--depth") depth = next();
        else if (a == "--tail") tail = next();
        else if (a == "--off") off = true;
        else if (a == "--loop-trim") trim = next();
        else if (a == "--time-it") timing = true;
        else { std::fprintf (stderr, "unknown flag %s\n", a.c_str()); return 2; }
    }
    Wav w = readWav (argv[1]);
    if (w.ch == 1) w.r = w.l;
    const size_t extra = (size_t) (tail * w.sr);
    w.l.resize (w.l.size() + extra, 0.0f); w.r.resize (w.l.size(), 0.0f);
    gerv::Reverb rv; rv.prepare (w.sr); rv.setDelay (delay); rv.setDepth (depth); rv.setOn (! off);
    if (trim > 0) rv.setLoopTrim (trim);
    const std::clock_t c0 = std::clock();
    for (size_t i = 0; i < w.l.size(); ++i) rv.process (w.l[i], w.r[i]);
    const double proc = (double) (std::clock() - c0) / CLOCKS_PER_SEC;
    writeWav (argv[2], w.sr, 2, w.l, w.r);
    if (timing) { const double au = (double) w.l.size() / w.sr; std::printf ("audio_s=%.3f proc_s=%.4f realtime_x=%.1f\n", au, proc, au / std::max (proc, 1e-9)); }
    return 0;
}
