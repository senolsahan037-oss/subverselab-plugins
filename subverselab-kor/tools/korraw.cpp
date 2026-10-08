// korraw <in.f32> <out.f32> <sampleRate> [key=value ...] [--block N | --blocks random] [--mono]
// korraw --latency
// Interleaved stereo float32 in and out (mono with --mono). Runs the engine exactly as the plugin will.
// Keys (engine units): drive (dB, 0..24) character (0..1) tone (dB, -6..6) ceiling (dBFS, -12..0) mix (0..1)
// auto (0/1) in (0/1). A key may glide: name=a:b goes from a to b over the file (a new value at each block).
// --latency prints the engine latency in samples and exits.
#include "../Source/KorEngine.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <chrono>

static std::vector<float> readAll (const char* path)
{
    std::vector<float> v;
    FILE* f = std::fopen (path, "rb");
    if (! f) { std::fprintf (stderr, "cannot open %s\n", path); std::exit (2); }
    float buf[4096]; size_t got;
    while ((got = std::fread (buf, sizeof (float), 4096, f)) > 0) v.insert (v.end(), buf, buf + got);
    std::fclose (f);
    return v;
}

int main (int argc, char** argv)
{
    bool bench = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp (argv[i], "--latency") == 0) { std::printf ("%d\n", kor::Engine::latencySamples()); return 0; }
        else if (std::strcmp (argv[i], "--bench") == 0) bench = true;
    if (argc < 4) { std::fprintf (stderr, "usage: korraw <in.f32> <out.f32> <sr> [key=value|key=a:b ...] [--block N|--blocks random] [--mono]\n       korraw --latency\n"); return 2; }

    const double sr = std::atof (argv[3]);
    kor::Params from, to;
    int block = 512; bool randomBlocks = false, mono = false;
    double benchSeconds = 0.0;

    auto assign = [] (kor::Params& p, const std::string& k, float v)
    {
        if (k == "drive") p.driveDb = v; else if (k == "character") p.character = v; else if (k == "tone") p.toneDb = v;
        else if (k == "ceiling") p.ceilingDb = v; else if (k == "mix") p.mix = v;
        else if (k == "auto") p.autoGain = v > 0.5f; else if (k == "in") p.in = v > 0.5f;
        else { std::fprintf (stderr, "unknown key %s\n", k.c_str()); std::exit (2); }
    };
    for (int i = 4; i < argc; ++i)
    {
        std::string a (argv[i]);
        if (a == "--bench") continue;
        if (a == "--block" && i + 1 < argc) block = std::max (1, std::atoi (argv[++i]));
        else if (a == "--blocks") { randomBlocks = true; ++i; }
        else if (a == "--mono") mono = true;
        else
        {
            const auto eq = a.find ('=');
            if (eq == std::string::npos) { std::fprintf (stderr, "bad argument %s\n", a.c_str()); return 2; }
            const std::string k = a.substr (0, eq), v = a.substr (eq + 1);
            const auto colon = v.find (':');
            assign (from, k, (float) std::atof (v.substr (0, colon).c_str()));
            assign (to, k, (float) std::atof (colon == std::string::npos ? v.c_str() : v.substr (colon + 1).c_str()));
        }
    }

    const int ch = mono ? 1 : 2;
    auto in = readAll (argv[1]);
    const int n = (int) (in.size() / (size_t) ch);
    std::vector<float> l ((size_t) n), r ((size_t) n);
    for (int i = 0; i < n; ++i) { l[(size_t) i] = in[(size_t) (ch * i)]; r[(size_t) i] = in[(size_t) (ch * i + ch - 1)]; }

    static kor::Engine eng;
    eng.prepare (sr, randomBlocks ? 700 : block, ch);
    uint32_t rng = 0x1234567u;
    for (int p = 0; p < n;)
    {
        int m = block;
        if (randomBlocks) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; m = 1 + (int) (rng % 700u); }
        m = std::min (m, n - p);
        const float t = n > 1 ? (float) p / (float) (n - 1) : 0.0f;
        auto lerp = [t] (float a, float b) { return a + (b - a) * t; };
        kor::Params q = from;
        q.driveDb = lerp (from.driveDb, to.driveDb); q.character = lerp (from.character, to.character);
        q.toneDb = lerp (from.toneDb, to.toneDb); q.ceilingDb = lerp (from.ceilingDb, to.ceilingDb);
        q.mix = lerp (from.mix, to.mix);
        eng.setParams (q);
        float* io[2] { l.data() + p, r.data() + p };
        const auto t0 = std::chrono::steady_clock::now();
        eng.process (io, ch, m);
        const auto t1 = std::chrono::steady_clock::now();
        if (bench) benchSeconds += std::chrono::duration<double>(t1 - t0).count();
        p += m;
    }

    if (bench) std::fprintf (stderr, "process_seconds %.9f\n", benchSeconds);
    for (int i = 0; i < n; ++i) { in[(size_t) (ch * i)] = l[(size_t) i]; if (ch == 2) in[(size_t) (2 * i + 1)] = r[(size_t) i]; }
    FILE* o = std::fopen (argv[2], "wb");
    if (! o) { std::fprintf (stderr, "cannot write %s\n", argv[2]); return 2; }
    std::fwrite (in.data(), sizeof (float), in.size(), o);
    std::fclose (o);
    return 0;
}
