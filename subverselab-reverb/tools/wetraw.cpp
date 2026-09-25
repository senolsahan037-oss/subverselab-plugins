// wetraw <hamam|sarnic|yayla> <in.f32> <out.f32> <sampleRate>
// Stereo interleaved float32 in, stereo interleaved wet-only float32 out.
#include "../Source/ReverbEngine.h"
#include <cstdio>
#include <cstring>
#include <vector>

int main (int argc, char** argv)
{
    if (argc < 5) { std::fprintf (stderr, "usage: wetraw <character> <in> <out> <sr>\n"); return 2; }
    const svrb::Character* ch = !std::strcmp (argv[1], "hammam") ? &svrb::hammam()
                              : !std::strcmp (argv[1], "cistern") ? &svrb::cistern()
                              : !std::strcmp (argv[1], "valley") ? &svrb::valley() : nullptr;
    if (! ch) { std::fprintf (stderr, "unknown character\n"); return 2; }

    FILE* f = std::fopen (argv[2], "rb");
    std::vector<float> in;
    float buf[4096];
    size_t got;
    while ((got = std::fread (buf, sizeof (float), 4096, f)) > 0) in.insert (in.end(), buf, buf + got);
    std::fclose (f);

    const int n = (int) (in.size() / 2);
    std::vector<float> l (n), r (n);
    for (int i = 0; i < n; ++i) { l[i] = in[2 * i]; r[i] = in[2 * i + 1]; }

    static svrb::ReverbEngine eng;
    eng.prepare (std::atof (argv[4]), *ch);
    for (int p = 0; p < n; p += 512)
    {
        const int m = n - p < 512 ? n - p : 512;
        eng.process (&l[p], &r[p], &l[p], &r[p], m);
    }
    for (int i = 0; i < n; ++i) { in[2 * i] = l[i]; in[2 * i + 1] = r[i]; }
    FILE* o = std::fopen (argv[3], "wb");
    std::fwrite (in.data(), sizeof (float), in.size(), o);
    std::fclose (o);
    return 0;
}
