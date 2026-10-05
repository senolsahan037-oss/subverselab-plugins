// tutkalraw <in.f32> <out.f32> <sampleRate> [key=value ...] [--key key.f32] [--gr gr.f32] [--block N | --blocks random] [--mono]
// Interleaved stereo float32 in and out (mono with --mono). Runs the engine exactly as the plugin will.
// Keys: threshold ratio attack release auto makeup mix hpf ext drive in voicing(0 core,1 glue,2 la,3 pump) pr limit bpm note shape depth. A key may be automated:
//   name=a:b            glides from a to b over the file (a new value at each block)
//   --at N name=value   jumps at absolute sample N, whatever the block size (the block is split there)
#include "../Source/TutkalEngine.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

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
    if (argc < 4) { std::fprintf (stderr, "usage: tutkalraw <in.f32> <out.f32> <sr> [key=value ...] [--key f] [--gr f] [--block N|--blocks random] [--mono]\n"); return 2; }
    const double sr = std::atof (argv[3]);
    tutkal::Params from, to;
    const char* keyPath = nullptr; const char* grPath = nullptr;
    int block = 512; bool randomBlocks = false, mono = false;
    std::vector<std::pair<int, std::pair<std::string, float>>> events;

    auto assign = [] (tutkal::Params& p, const std::string& k, float v)
    {
        if (k == "threshold") p.thresholdDb = v; else if (k == "ratio") p.ratio = v; else if (k == "attack") p.attackMs = v;
        else if (k == "release") p.releaseMs = v; else if (k == "auto") p.autoRelease = v > 0.5f; else if (k == "makeup") p.makeupDb = v;
        else if (k == "mix") p.mix = v; else if (k == "hpf") p.scHpfHz = v; else if (k == "ext") p.ext = v > 0.5f;
        else if (k == "drive") p.drive = v; else if (k == "in") p.in = v > 0.5f;
        else if (k == "voicing") p.voicing = (int) std::lround (v); else if (k == "pr") p.peakReduction = v; else if (k == "limit") p.limit = v > 0.5f;
        else if (k == "bpm") p.bpm = v; else if (k == "note") p.pumpNote = v; else if (k == "shape") p.pumpShape = (int) std::lround (v);
        else if (k == "depth") p.depthDb = v;
        else { std::fprintf (stderr, "unknown key %s\n", k.c_str()); std::exit (2); }
    };
    for (int i = 4; i < argc; ++i)
    {
        std::string a (argv[i]);
        if (a == "--key" && i + 1 < argc) keyPath = argv[++i];
        else if (a == "--gr" && i + 1 < argc) grPath = argv[++i];
        else if (a == "--block" && i + 1 < argc) block = std::atoi (argv[++i]);
        else if (a == "--blocks") { randomBlocks = true; ++i; }
        else if (a == "--mono") mono = true;
        else if (a == "--at" && i + 2 < argc)
        {
            const int at = std::atoi (argv[++i]);
            const std::string kv (argv[++i]);
            const auto eq = kv.find ('=');
            events.push_back ({ at, { kv.substr (0, eq), (float) std::atof (kv.substr (eq + 1).c_str()) } });
        }
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
    std::vector<float> l ((size_t) n), r ((size_t) n), kl, kr, gr ((size_t) n);
    for (int i = 0; i < n; ++i) { l[(size_t) i] = in[(size_t) (ch * i)]; r[(size_t) i] = in[(size_t) (ch * i + ch - 1)]; }
    if (keyPath)
    {
        auto k = readAll (keyPath);
        kl.resize ((size_t) n, 0.0f); kr.resize ((size_t) n, 0.0f);
        for (int i = 0; i < n && (size_t) (2 * i + 1) < k.size(); ++i) { kl[(size_t) i] = k[(size_t) (2 * i)]; kr[(size_t) i] = k[(size_t) (2 * i + 1)]; }
    }

    static tutkal::Engine eng;
    eng.prepare (sr);
    uint32_t rng = 0x1234567u;
    for (int p = 0; p < n;)
    {
        int m = block;
        if (randomBlocks) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; m = 1 + (int) (rng % 700u); }
        m = std::min (m, n - p);
        for (auto& e : events)
        {
            if (e.first == p) { assign (from, e.second.first, e.second.second); assign (to, e.second.first, e.second.second); }
            else if (e.first > p && e.first < p + m) m = e.first - p;
        }
        const float t = n > 1 ? (float) p / (float) (n - 1) : 0.0f;
        tutkal::Params q = from;
        q.autoRelease = from.autoRelease; q.ext = from.ext; q.in = from.in; q.voicing = from.voicing; q.limit = from.limit; q.pumpShape = from.pumpShape;
        auto lerp = [t] (float a, float b) { return a + (b - a) * t; };
        q.thresholdDb = lerp (from.thresholdDb, to.thresholdDb); q.ratio = lerp (from.ratio, to.ratio);
        q.attackMs = lerp (from.attackMs, to.attackMs); q.releaseMs = lerp (from.releaseMs, to.releaseMs);
        q.makeupDb = lerp (from.makeupDb, to.makeupDb); q.mix = lerp (from.mix, to.mix);
        q.scHpfHz = lerp (from.scHpfHz, to.scHpfHz); q.drive = lerp (from.drive, to.drive);
        q.peakReduction = lerp (from.peakReduction, to.peakReduction); q.bpm = lerp (from.bpm, to.bpm); q.pumpNote = lerp (from.pumpNote, to.pumpNote); q.depthDb = lerp (from.depthDb, to.depthDb);
        float* io[2] { l.data() + p, r.data() + p };
        const float* key[2] { keyPath ? kl.data() + p : nullptr, keyPath ? kr.data() + p : nullptr };
        eng.process (io, ch, keyPath ? key : nullptr, keyPath ? 2 : 0, m, q, gr.data() + p);
        p += m;
    }

    for (int i = 0; i < n; ++i) { in[(size_t) (ch * i)] = l[(size_t) i]; if (ch == 2) in[(size_t) (2 * i + 1)] = r[(size_t) i]; }
    FILE* o = std::fopen (argv[2], "wb");
    std::fwrite (in.data(), sizeof (float), in.size(), o);
    std::fclose (o);
    if (grPath) { FILE* g = std::fopen (grPath, "wb"); std::fwrite (gr.data(), sizeof (float), gr.size(), g); std::fclose (g); }
    return 0;
}
