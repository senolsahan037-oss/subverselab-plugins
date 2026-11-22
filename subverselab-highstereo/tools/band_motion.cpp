// Live band readouts at 30 frames/s through the real engine, for the moving STEREO FIELD preview.
// Usage: band_motion in.f32 out.f32 rate width% frames.csv [stage_tour]   (stereo float32 interleaved in/out)
// stage_tour: the STAGE puck goes front -> left -> behind -> front -> right -> behind -> front (time from 1 s in).
// Then the stage angle, then per band LIMIT: limit, peakFill, noSide.
// frames.csv: one line per frame: for each of the 8 bands liveWidth,liveLevelDb,liveBalance; then output correlation.
#include "../Source/HighStereoEngine.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

int main (int argc, char** argv)
{
    if (argc < 6) { std::fprintf (stderr, "usage: band_motion in.f32 out.f32 rate width%% frames.csv\n"); return 2; }
    FILE* f = std::fopen (argv[1], "rb"); if (! f) { std::perror ("in"); return 1; }
    std::vector<float> inter; float buf[4096]; size_t got;
    while ((got = std::fread (buf, sizeof (float), 4096, f)) > 0) inter.insert (inter.end(), buf, buf + got);
    std::fclose (f);
    const size_t n = inter.size() / 2;
    std::vector<float> l (n), r (n), ol (n), orr (n);
    for (size_t i = 0; i < n; ++i) { l[i] = inter[2 * i]; r[i] = inter[2 * i + 1]; }
    const double fs = std::atof (argv[3]);
    svl::HighStereoEngine e; e.prepare (fs, 4096);
    svl::HighStereoEngine::Parameters p; p.width = std::atof (argv[4]) / 100.0;
    p.bandWidth[0] = 1.0001;   // band tree on, so the readouts run even at WIDTH <= 150 %
    FILE* csv = std::fopen (argv[5], "w");
    const size_t hop = (size_t) (fs / 30.0);
    const bool tour = argc > 6;
    auto stageAt = [] (double t)   // seconds from 1 s in (the preview skips the first second)
    {
        const double k[][2] = { {0,0},{3,0},{8,90},{13,175},{15,175},{19,0},{23,-90},{27,-175},{28,-175},{29.5,0},{99,0} };
        for (int j = 0; j < 10; ++j)
            if (t <= k[j + 1][0]) return k[j][1] + (k[j + 1][1] - k[j][1]) * (t - k[j][0]) / (k[j + 1][0] - k[j][0]);
        return 0.0;
    };
    for (size_t i = 0; i < n; i += hop)
    {
        if (tour) p.stageDeg = stageAt ((double) i / fs - 1.0 < 0 ? 0.0 : (double) i / fs - 1.0);
        const int m = (int) std::min (hop, n - i);
        const float* ip[2] = { l.data() + i, r.data() + i };
        float* op[2] = { ol.data() + i, orr.data() + i };
        e.process (ip, op, 2, m, p);
        for (int k = 0; k < 8; ++k) { const auto b = e.band (k); std::fprintf (csv, "%.4f,%.2f,%.4f,", b.liveWidth, b.liveLevelDb, b.liveBalance); }
        std::fprintf (csv, "%.4f,%.2f", e.correlation(), p.stageDeg);
        for (int k = 0; k < 8; ++k) { const auto b = e.band (k); std::fprintf (csv, ",%.4f,%.4f,%d", b.limit, b.peakFill, (int) b.noSide); }
        std::fprintf (csv, "\n");
    }
    std::fclose (csv);
    FILE* o = std::fopen (argv[2], "wb");
    for (size_t i = 0; i < n; ++i) { std::fwrite (&ol[i], 4, 1, o); std::fwrite (&orr[i], 4, 1, o); }
    std::fclose (o);
    return 0;
}
