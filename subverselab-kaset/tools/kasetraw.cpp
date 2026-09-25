// kasetraw <in.f32> <out.f32> <sr> <generation> <wow> <flutter> <drive> <hiss> <wear>
// Stereo interleaved float32 in and out.
#include "../Source/KasetEngine.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

int main (int argc, char** argv)
{
    if (argc < 10) { std::fprintf (stderr, "usage: kasetraw in out sr gen wow flutter drive hiss wear\n"); return 2; }
    FILE* f = std::fopen (argv[1], "rb");
    std::vector<float> in;
    float buf[4096]; size_t got;
    while ((got = std::fread (buf, sizeof (float), 4096, f)) > 0) in.insert (in.end(), buf, buf + got);
    std::fclose (f);
    const int n = (int) (in.size() / 2);
    std::vector<float> l (n), r (n);
    for (int i = 0; i < n; ++i) { l[i] = in[2 * i]; r[i] = in[2 * i + 1]; }

    kaset::Controls c;
    c.generation = std::atoi (argv[4]); c.wow = (float) std::atof (argv[5]); c.flutter = (float) std::atof (argv[6]);
    c.drive = (float) std::atof (argv[7]); c.hiss = (float) std::atof (argv[8]); c.wear = (float) std::atof (argv[9]);
    static kaset::KasetEngine eng;
    eng.prepare (std::atof (argv[3]));
    for (int p = 0; p < n; p += 512) eng.process (&l[p], &r[p], std::min (512, n - p), c);
    // Remove the transport latency so the demo lines up with the dry file.
    const int lat = eng.latency();
    for (int i = 0; i < n; ++i) { in[2 * i] = i + lat < n ? l[i + lat] : 0.0f; in[2 * i + 1] = i + lat < n ? r[i + lat] : 0.0f; }
    FILE* o = std::fopen (argv[2], "wb");
    std::fwrite (in.data(), sizeof (float), in.size(), o);
    std::fclose (o);
    return 0;
}
