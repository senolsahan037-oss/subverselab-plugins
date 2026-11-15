/*  boebraw: runs the BOE-B engine (Source/BoebEngine.h, the code the plug-in will use) over raw float32 files.

        boebraw in.f32 out.f32 SR [name=value ...] [--key beat.f32] [--listen SEC] [--report r.json]
                [--block N | --blocks random] [--at SAMPLE name=value]...

    in / out / key: interleaved stereo float32. Parameters: lift mood body air space throw output equal enabled drywet bpm,
    and the LISTEN settings hpf presence compneed. --listen SEC runs the analyser over the first SEC seconds first
    (as the LISTEN button), sets hpf / presence / compneed and, unless air= is given, the AIR knob; the diagnosis
    goes to --report as JSON. --at changes a parameter at a sample position (block edges follow it).
*/
#include "../Source/BoebEngine.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace svl::boeb;

static std::vector<float> readF32 (const char* path)
{
    std::vector<float> v;
    FILE* f = std::fopen (path, "rb");
    if (! f) { std::fprintf (stderr, "boebraw: cannot open %s\n", path); std::exit (2); }
    std::fseek (f, 0, SEEK_END); const long n = std::ftell (f); std::fseek (f, 0, SEEK_SET);
    v.resize ((size_t) n / sizeof (float));
    if (std::fread (v.data(), sizeof (float), v.size(), f) != v.size()) { std::fprintf (stderr, "boebraw: short read\n"); std::exit (2); }
    std::fclose (f);
    return v;
}

static bool setParam (BoebEngine::Parameters& p, const std::string& kv, bool* airGiven = nullptr)
{
    const auto eq = kv.find ('=');
    if (eq == std::string::npos) return false;
    const std::string k = kv.substr (0, eq);
    const double v = std::atof (kv.c_str() + eq + 1);
    if (k == "lift") p.lift = (float) v;
    else if (k == "mood") p.mood = (int) v;
    else if (k == "drywet") p.dryWet = (float) v;
    else if (k == "body") p.bodyDb = (float) v;
    else if (k == "air") { p.airDb = (float) v; if (airGiven) *airGiven = true; }
    else if (k == "space") p.space = (float) v;
    else if (k == "throw") p.throwOn = v != 0;
    else if (k == "output") p.outputDb = (float) v;
    else if (k == "equal") p.equal = v != 0;
    else if (k == "enabled") p.enabled = v != 0;
    else if (k == "bpm") p.bpm = v;
    else if (k == "hpf") p.listen.hpfHz = (float) v;
    else if (k == "presence") p.listen.presenceDb = (float) v;
    else if (k == "compneed") p.listen.compNeed = (float) v;
    else if (k == "testoff") p.testOff = (int) v;
    else if (k == "cut") { p.listen.bandLimited = v > 0; p.listen.cutHz = (float) v; }   // measurement: force the exciter
    else if (k == "excite") p.listen.exciteDb = (float) v;
    else return false;
    return true;
}

int main (int argc, char** argv)
{
    if (argc < 4) { std::fprintf (stderr, "usage: boebraw in.f32 out.f32 SR [name=value ...] [--key f] [--listen s] [--report f] [--block N|--blocks random] [--at N k=v]\n"); return 2; }
    const char* inPath = argv[1]; const char* outPath = argv[2];
    const double fs = std::atof (argv[3]);
    BoebEngine::Parameters p;
    const char* keyPath = nullptr; const char* reportPath = nullptr; const char* tracePath = nullptr;
    double listenSec = 0; int block = 512; bool randomBlocks = false; bool airGiven = false;
    struct At { long pos; std::string kv; };
    std::vector<At> ats;
    for (int a = 4; a < argc; ++a)
    {
        const std::string s = argv[a];
        if (s == "--key" && a + 1 < argc) keyPath = argv[++a];
        else if (s == "--listen" && a + 1 < argc) listenSec = std::atof (argv[++a]);
        else if (s == "--report" && a + 1 < argc) reportPath = argv[++a];
        else if (s == "--trace" && a + 1 < argc) tracePath = argv[++a];
        else if (s == "--block" && a + 1 < argc) block = std::max (1, std::atoi (argv[++a]));
        else if (s == "--blocks" && a + 1 < argc) { randomBlocks = std::strcmp (argv[++a], "random") == 0; }
        else if (s == "--at" && a + 2 < argc) { ats.push_back ({ std::atol (argv[a + 1]), argv[a + 2] }); a += 2; }
        else if (! setParam (p, s, &airGiven)) { std::fprintf (stderr, "boebraw: unknown argument %s\n", s.c_str()); return 2; }
    }

    const std::vector<float> in = readF32 (inPath);
    const size_t frames = in.size() / 2;
    std::vector<float> key;
    if (keyPath) { key = readF32 (keyPath); key.resize (frames * 2, 0.0f); }
    std::vector<float> L (frames), R (frames), KL, KR, OL (frames), OR (frames);
    for (size_t i = 0; i < frames; ++i) { L[i] = in[2 * i]; R[i] = in[2 * i + 1]; }
    if (keyPath) { KL.resize (frames); KR.resize (frames); for (size_t i = 0; i < frames; ++i) { KL[i] = key[2 * i]; KR[i] = key[2 * i + 1]; } }

    Diagnosis dg;
    if (listenSec > 0)
    {
        BoebAnalyser an;
        an.prepare (fs, listenSec + 1);
        const size_t nL = std::min (frames, (size_t) (listenSec * fs));
        const float* v[2] = { L.data(), R.data() };
        const float* k[2] = { keyPath ? KL.data() : nullptr, keyPath ? KR.data() : nullptr };
        an.feed (v, 2, keyPath ? k : nullptr, keyPath ? 2 : 0, (int) nL);
        dg = an.diagnose();
        Settings st = BoebAnalyser::settingsFor (dg);
        if (st.bandLimited)   // as the plug-in will do when LISTEN ends
        {
            std::vector<float> m (nL);
            for (size_t i = 0; i < nL; ++i) m[i] = 0.5f * (L[i] + R[i]);
            calibrateExciter (st, m.data(), nL, fs);
        }
        p.listen = st;
        if (! airGiven) p.airDb = st.airDb;
        for (int a = 4; a < argc; ++a)   // explicit hpf= / presence= / compneed= win over LISTEN
        {
            const std::string s = argv[a];
            if (s.rfind ("hpf=", 0) == 0 || s.rfind ("presence=", 0) == 0 || s.rfind ("compneed=", 0) == 0
                || s.rfind ("cut=", 0) == 0 || s.rfind ("excite=", 0) == 0) setParam (p, s);
        }
    }

    BoebEngine eng;
    eng.prepare (fs, 4096);
    std::mt19937 rng (12345);
    std::uniform_int_distribution<int> dist (1, 4096);
    size_t pos = 0, nextAt = 0;
    std::sort (ats.begin(), ats.end(), [] (const At& a, const At& b) { return a.pos < b.pos; });
    FILE* ft = tracePath ? std::fopen (tracePath, "w") : nullptr;
    float maxGr = 0, maxRide = -100, minRide = 100, maxDeess = 0, maxMud = 0, maxCeil = 0;
    while (pos < frames)
    {
        while (nextAt < ats.size() && (size_t) ats[nextAt].pos <= pos) setParam (p, ats[nextAt++].kv);
        size_t n = (size_t) (randomBlocks ? dist (rng) : block);
        n = std::min (n, frames - pos);
        if (nextAt < ats.size()) n = std::min (n, (size_t) ats[nextAt].pos - pos);
        const float* ip[2] = { L.data() + pos, R.data() + pos };
        float* op[2] = { OL.data() + pos, OR.data() + pos };
        const float* kp[2] = { keyPath ? KL.data() + pos : nullptr, keyPath ? KR.data() + pos : nullptr };
        eng.process (ip, op, 2, keyPath ? kp : nullptr, keyPath ? 2 : 0, (int) n, p);
        const auto& r = eng.report();
        maxGr = std::max (maxGr, r.compGrDb); maxRide = std::max (maxRide, r.rideDb); minRide = std::min (minRide, r.rideDb);
        maxDeess = std::max (maxDeess, r.deessDb); maxMud = std::max (maxMud, r.mudCutDb); maxCeil = std::max (maxCeil, r.ceilingDb);
        if (ft) std::fprintf (ft, "%.3f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f\n", pos / fs, r.rideDb, r.compGrDb, r.makeupDb, r.rideLevelDb, r.targetDb, r.mudCutDb, r.exciteDb, r.loopAirDb, r.toneDb[0], r.toneDb[1], r.toneDb[2]);
        pos += n;
    }
    if (ft) std::fclose (ft);
    std::vector<float> outI (frames * 2);
    for (size_t i = 0; i < frames; ++i) { outI[2 * i] = OL[i]; outI[2 * i + 1] = OR[i]; }
    FILE* fo = std::fopen (outPath, "wb");
    if (! fo) { std::fprintf (stderr, "boebraw: cannot write %s\n", outPath); return 2; }
    std::fwrite (outI.data(), sizeof (float), outI.size(), fo);
    std::fclose (fo);

    if (reportPath)
    {
        FILE* fr = std::fopen (reportPath, "w");
        if (fr)
        {
            std::fprintf (fr, "{\"valid\":%s,\"active\":%d,\"hasBeat\":%s,\"lar\":%.3f,\"spread\":%.3f,\"pres\":%.3f,\"mud\":%.3f,"
                              "\"air\":%.3f,\"sib\":%.3f,\"low\":%.3f,\"vocal\":%.3f,\"beat\":%.3f,"
                              "\"hpf\":%.1f,\"presence\":%.3f,\"mudcut\":%.3f,\"airset\":%.3f,\"compneed\":%.3f,"
                              "\"bandLimited\":%s,\"cut\":%.0f,\"excite\":%.2f,\"cutMeasured\":%.0f,\"srcDb\":%.2f,\"genDb\":%.2f,"
                              "\"maxGr\":%.3f,\"rideMax\":%.3f,\"rideMin\":%.3f,\"deessMax\":%.3f,\"mudMax\":%.3f,\"ceilMax\":%.3f}\n",
                         dg.valid ? "true" : "false", dg.activeFrames, dg.hasBeat ? "true" : "false", dg.larDb, dg.spreadDb,
                         dg.presDb, dg.mudDb, dg.airDb, dg.sibDb, dg.lowDb, dg.vocalLevelDb, dg.beatLevelDb,
                         p.listen.hpfHz, p.listen.presenceDb, p.listen.mudCutDb, p.airDb, p.listen.compNeed,
                         p.listen.bandLimited ? "true" : "false", p.listen.cutHz, p.listen.exciteDb, dg.cutHz, dg.exciteSrcDb, dg.exciteGenDb,
                         maxGr, maxRide, minRide, maxDeess, maxMud, maxCeil);
            std::fclose (fr);
        }
    }
    return 0;
}
