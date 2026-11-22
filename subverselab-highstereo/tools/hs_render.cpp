// Render a stereo float32 interleaved raw file through the HIGH STEREO engine with every parameter (preset measurements).
// Usage: hs_render in.f32 out.f32 rate [key=value ...]
//   keys (plug-in units): width spread monobass(Hz, <40 = OFF) b1..b8 (band %, b1 = <100 Hz .. b8 = 8k+) fix match mix output stage guard(1/0)
#include "../Source/HighStereoEngine.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

int main (int argc, char** argv)
{
    if (argc < 4) { std::fprintf (stderr, "usage: hs_render in.f32 out.f32 rate [key=value ...]\n"); return 2; }
    FILE* f = std::fopen (argv[1], "rb");
    if (! f) { std::perror ("in"); return 1; }
    std::vector<float> inter; float buf[4096]; size_t got;
    while ((got = std::fread (buf, sizeof (float), 4096, f)) > 0) inter.insert (inter.end(), buf, buf + got);
    std::fclose (f);
    const size_t n = inter.size() / 2;
    std::vector<float> l (n), r (n), ol (n), orr (n);
    for (size_t i = 0; i < n; ++i) { l[i] = inter[2 * i]; r[i] = inter[2 * i + 1]; }

    svl::HighStereoEngine::Parameters p;
    for (int a = 4; a < argc; ++a)
    {
        std::string kv (argv[a]); const auto eq = kv.find ('=');
        if (eq == std::string::npos) { std::fprintf (stderr, "bad arg %s\n", argv[a]); return 2; }
        const std::string k = kv.substr (0, eq); const double v = std::atof (kv.c_str() + eq + 1);
        if (k == "width") p.width = v / 100.0;
        else if (k == "spread") p.spread = v / 100.0;
        else if (k == "monobass") p.monoBassHz = v < 40.0 ? 0.0 : v;
        else if (k.size() == 2 && k[0] == 'b' && k[1] >= '1' && k[1] <= '8') p.bandWidth[(size_t) (k[1] - '1')] = v / 100.0;
        else if (k == "fix") p.fix = v > 0.5;
        else if (k == "match") p.match = v > 0.5;
        else if (k == "mix") p.mix = v / 100.0;
        else if (k == "output") p.outputDb = v;
        else if (k == "stage") p.stageDeg = v;
        else if (k == "guard") p.peakGuard = v > 0.5;
        else { std::fprintf (stderr, "unknown key %s\n", k.c_str()); return 2; }
    }

    svl::HighStereoEngine e;
    e.prepare (std::atof (argv[3]), 512);
    for (size_t i = 0; i < n; i += 512)
    {
        const int m = (int) std::min ((size_t) 512, n - i);
        const float* ip[2] = { l.data() + i, r.data() + i };
        float* op[2] = { ol.data() + i, orr.data() + i };
        e.process (ip, op, 2, m, p);
    }
    FILE* o = std::fopen (argv[2], "wb");
    if (! o) { std::perror ("out"); return 1; }
    std::vector<float> out (2 * n);
    for (size_t i = 0; i < n; ++i) { out[2 * i] = ol[i]; out[2 * i + 1] = orr[i]; }
    std::fwrite (out.data(), sizeof (float), out.size(), o);
    std::fclose (o);
    return 0;
}
