// geraw in.wav out.wav [--pos p1,...,p12 | --marks d1,...,d12] [--band i p] [--sweep i from_dB to_dB] [--bypass] [--rev] [--att] [--toggle bypass|rev|att t] [--block N]
//       [--time-it] | --latency | --dump-ss p1,...,p12
// Offline renderer for Source/Ge1100Engine.h (the original's 12-band graphic equaliser stage), run exactly as the plug-in
// will. Slide-pot positions are 0 (wiper at the N end, full boost) ... 0.5 (centre tap, flat) ... 1 (MID end, full cut).
// --marks sets the panel marks in dB (-12 ... +12, Ge1100Taper.h). --band i p sets band i's position (0 = 16 Hz ... 11 = 32 kHz).
// --sweep i a b moves band i's mark from a to b dB across the file, updated every 32 samples (the click test).
// --dump-ss prints the continuous A, B, C, D (for the Python check).
// WAV in: PCM 16/24/32 or float32, mono or stereo; WAV out: float32, same channels.
#include "../Source/Ge1100Engine.h"
#include "wavio.h"
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

static std::array<double, 12> parsePos (const std::string& s)
{
    std::array<double, 12> p {}; std::stringstream ss (s); std::string tok; int i = 0;
    while (std::getline (ss, tok, ',') && i < 12) p[(size_t) i++] = std::atof (tok.c_str());
    if (i != 12) { std::fprintf (stderr, "--pos needs 12 values\n"); std::exit (2); }
    return p;
}

int main (int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string a (argv[i]);
        if (a == "--latency") { std::printf ("%d\n", ge::Channel::latency()); return 0; }
        if (a == "--dump-ss" && i + 1 < argc)
        {
            const auto c = ge::buildContinuous (parsePos (argv[i + 1]));
            for (double v : c.A) std::printf ("%.17g\n", v);
            for (double v : c.B) std::printf ("%.17g\n", v);
            for (double v : c.C) std::printf ("%.17g\n", v);
            std::printf ("%.17g\n", c.D);
            return 0;
        }
    }
    if (argc < 3) { std::fprintf (stderr, "usage: geraw in.wav out.wav [--pos p1,...,p12 | --marks d1,...,d12] [--band i p] [--sweep i a b] [--bypass] [--block N] [--time-it] | --latency | --dump-ss p1,...,p12\n"); return 2; }
    std::array<double, 12> pos; pos.fill (0.5);
    bool bypass = false, timing = false, rev = false, att = false; int block = 512, sweepBand = -1; double sweepA = 0, sweepB = 0;
    std::string toggleKind; double toggleAt = -1;   // --toggle bypass|rev|att t: flip that switch at t seconds (bus tests)
    for (int i = 3; i < argc; ++i)
    {
        const std::string a (argv[i]);
        auto next = [&] { if (i + 1 >= argc) { std::fprintf (stderr, "%s needs a value\n", a.c_str()); std::exit (2); } return std::string (argv[++i]); };
        if (a == "--pos") pos = parsePos (next());
        else if (a == "--band") { const int b = std::atoi (next().c_str()); const double v = std::atof (next().c_str()); if (b < 0 || b > 11) return 2; pos[(size_t) b] = v; }
        else if (a == "--bypass") bypass = true;
        else if (a == "--rev") rev = true;
        else if (a == "--att") att = true;
        else if (a == "--marks") { const auto d = parsePos (next()); for (int b = 0; b < 12; ++b) pos[(size_t) b] = ge::positionForMark (b, d[(size_t) b]); }
        else if (a == "--sweep") { sweepBand = std::atoi (next().c_str()); sweepA = std::atof (next().c_str()); sweepB = std::atof (next().c_str()); }
        else if (a == "--block") block = std::atoi (next().c_str());
        else if (a == "--time-it") timing = true;
        else if (a == "--toggle") { toggleKind = next(); toggleAt = std::atof (next().c_str()); }
        else { std::fprintf (stderr, "unknown flag %s\n", a.c_str()); return 2; }
    }
    Wav w = readWav (argv[1]);
    ge::Channel ch[2];
    if (sweepBand >= 0) pos[(size_t) sweepBand] = ge::positionForMark (sweepBand, sweepA);
    for (auto& c : ch) { c.setReverse (rev); c.setAttenuate (att); c.setPositions (pos); c.prepare (w.sr); c.setBypass (bypass); }
    // matrices are built on the first sample: do it before the clock starts
    { float z = 0; ch[0].process (z); ch[1].process (z); ch[0].reset(); ch[1].reset(); }
    const std::clock_t c0 = std::clock();   // process CPU time: steady under other load on the Mac
    const int n = (int) w.l.size();
    for (int s = 0; s < n; s += block)
    {
        const int m = std::min (block, n - s);
        for (int k = 0; k < m; ++k)
        {
            const int idx = s + k;
            if (toggleAt >= 0 && idx == (int) (toggleAt * w.sr))
                for (auto& c : ch)
                {
                    if (toggleKind == "bypass") c.setBypass (! bypass);
                    else if (toggleKind == "rev") c.setReverse (! rev);
                    else if (toggleKind == "att") c.setAttenuate (! att);
                }
            if (sweepBand >= 0 && idx % 32 == 0)
            {
                pos[(size_t) sweepBand] = ge::positionForMark (sweepBand, sweepA + (sweepB - sweepA) * idx / std::max (1, n - 1));
                ch[0].setPositions (pos); ch[1].setPositions (pos);
            }
            w.l[(size_t) idx] = ch[0].process (w.l[(size_t) idx]);
        }
        if (w.ch == 2) for (int k = 0; k < m; ++k) w.r[(size_t) (s + k)] = ch[1].process (w.r[(size_t) (s + k)]);
    }
    const double proc = (double) (std::clock() - c0) / CLOCKS_PER_SEC;
    writeWav (argv[2], w.sr, w.ch, w.l, w.r);
    if (timing) { const double au = (double) n / w.sr; std::printf ("audio_s=%.3f proc_s=%.4f realtime_x=%.1f\n", au, proc, au / std::max (proc, 1e-9)); }
    return 0;
}
