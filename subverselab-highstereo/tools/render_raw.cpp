// Render a stereo float32 interleaved raw file through the HIGH STEREO engine.
// Usage: render_raw in.f32 out.f32 rate width% spread% [band1% .. band8%]
#include "../Source/HighStereoEngine.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

int main (int argc, char** argv)
{
    if (argc < 6) { std::fprintf (stderr, "usage: render_raw in.f32 out.f32 rate width%% spread%% [band1%%..band8%%]\n"); return 2; }
    FILE* f = std::fopen (argv[1], "rb");
    if (! f) { std::perror ("in"); return 1; }
    std::vector<float> inter;
    float buf[4096];
    size_t got;
    while ((got = std::fread (buf, sizeof (float), 4096, f)) > 0) inter.insert (inter.end(), buf, buf + got);
    std::fclose (f);
    const size_t n = inter.size() / 2;
    std::vector<float> l (n), r (n), ol (n), orr (n);
    for (size_t i = 0; i < n; ++i) { l[i] = inter[2 * i]; r[i] = inter[2 * i + 1]; }

    svl::HighStereoEngine e;
    const double fs = std::atof (argv[3]);
    e.prepare (fs, 512);
    svl::HighStereoEngine::Parameters p;
    p.width = std::atof (argv[4]) / 100.0;
    p.spread = std::atof (argv[5]) / 100.0;
    if (const char* fsc = std::getenv ("HS_FILLSCALE")) p.fillScale = std::atof (fsc);
    for (int k = 0; k < 8 && 6 + k < argc; ++k) p.bandWidth[(size_t) k] = std::atof (argv[6 + k]) / 100.0;

    for (size_t i = 0; i < n; i += 512)
    {
        const int m = (int) std::min ((size_t) 512, n - i);
        const float* ip[2] = { l.data() + i, r.data() + i };
        float* op[2] = { ol.data() + i, orr.data() + i };
        e.process (ip, op, 2, m, p);
    }
    FILE* o = std::fopen (argv[2], "wb");
    for (size_t i = 0; i < n; ++i) { std::fwrite (&ol[i], sizeof (float), 1, o); std::fwrite (&orr[i], sizeof (float), 1, o); }
    std::fclose (o);
    return 0;
}
