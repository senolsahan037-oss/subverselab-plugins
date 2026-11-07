// dyneqraw main.wav key.wav out.wav [--mode collide|self] [--depth 6] [--margin 3 (= SENS in self)] [--low 120] [--high 8000] [--keylaw signal|091]
//          [--speed fast|medium|slow] [--mix 1] [--delta 0|1] [--rawbells] [--tiling] [--block 32] [--cutlog cuts.csv] [--bandlog bands.csv] [--reallog real.csv] [--time]
//          [--band1 type=bell,f=1000,g=6,q=1[,dyn=1,thr=-30,range=-6,att=10,rel=100,trig=self|key]] ... [--band6 ...]
//          Free bands (SPEC 1b-A). type = bell|lowshelf|highshelf|notch|lowcut|highcut; lowcut / highcut also take slope=12|24|48;
//          on=0 keeps the settings but switches the band off. A band named on the command line is on.
//          --after SEC --aband1 ... --aband6 ...   at the first block starting at or after SEC the named bands take the new settings ("off" = same settings, on=0)
// Offline renderer: runs the engine exactly as the plug-in will. key.wav may be "-" (no key).
// WAV in: PCM 16/24/32 or float32, mono or stereo (the first two channels). WAV out: float32, same channel count as main.
// Cut log: one row per engine block (32 samples when --block is a multiple of 32), 32 target cuts in dB, positive = cut.
// In SELF mode the row continues with the 4 hunter bells: h1_hz,h1_db,h1_q, ... h4_q (gain dB, negative = cut; 0 = idle).
// The last six columns are the free bands' current gains b1_db ... b6_db (dB, negative = cut, 0 = off / idle).
// --target inst=kick,part=body,amount=9,width=1[,sweet=120][,reverb=1][,slope=24][,part_end=100]   the TARGET layer (SPEC 8b / 8c). inst = off|vocal|bass|drums|kick|snare|hats;
//          part = a part name of that instrument (sub, body, mud, box, click, crack, snap/air, chick, bite, shimmer, top, nasal, harsh, s; drums: kickbody, snarebody, hatsshimmer) or the slider 0..100;
//          amount 0..60 dB (a bell is capped at 18); slope 12|24|36|48|72 dB/oct (the edge cuts: SUB, CLICK, REVERB-READY); width 0.5..2; sweet 20..400 ms; part_end: the PART slider moves linearly from part to part_end over the whole file.
//          --after SEC --atarget inst=...   at the first block starting at or after SEC the TARGET settings are replaced (the keys not named keep their defaults).
//          The cut log's last column target_db is the deepest of the three TARGET bells (dB, positive = cut).
//          --switch SEC:inst=...   (repeatable, ascending SEC) the same, any number of times: one more TARGET setting from SEC on (the engine's memory is never touched by a switch).
// --mem-in FILE / --mem-out FILE / --mem-at SEC FILE (repeatable)   SPEC 8d memory as raw float32: load it before the first sample (a state load), save it after the last, save it at the first block starting at or after SEC.
// --no-listen   switches the always-on analysis off (CPU measurement only: the cost of listening)
// --listen-until SEC   switches the analysis off from the first block at or after SEC (the memory stays as it is: a frozen memory for a steady probe)
// --blocktimes   times every process() call (use with --block 512): prints "block_us n=.. mean=.. p99=.. p999=.. max=.." on stdout
// --latency   prints "evidence_latency records=.. max_samples=.. last_samples=..": how long the evidence records took from the start of their job to their last slot (P1l)
// --edge-in   keeps the edge splits in the path at AMOUNT 0 (measures the split's all-pass sum; measurement only)
// --bake [--bake-from SEC]   collect the BAKE statistics (SPEC 1 C) from SEC (default 0) to the end, fit the mean cut into the EMPTY free bands
//          (the ones not named on the command line) and print, on stdout, lines starting with "BAKE ": counts, mean cut and % of time per band,
//          the bells ("BAKE bell i: f= g= q= ..."), ready-made "--bandN" specs, the baked static curve and the residual at the 32 centres, and
//          the undo self-check. The render itself is the normal one (the bells are NOT applied to out.wav; run again with the printed specs).
// --time prints "audio_s=... proc_s=... realtime_x=..." on stdout (processing only; use it without --cutlog, the log is written inside the loop).
#include "../Source/DyneqEngine.h"
#include "../Source/DyneqBake.h"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

struct Wav { int sr = 0; int ch = 0; std::vector<float> l, r; };

static uint32_t rd32 (const unsigned char* p) { return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24); }
static uint16_t rd16 (const unsigned char* p) { return (uint16_t) (p[0] | (p[1] << 8)); }

static Wav readWav (const char* path)
{
    Wav w;
    FILE* f = std::fopen (path, "rb");
    if (! f) { std::fprintf (stderr, "cannot open %s\n", path); std::exit (2); }
    std::vector<unsigned char> d;
    unsigned char buf[65536]; size_t got;
    while ((got = std::fread (buf, 1, sizeof (buf), f)) > 0) d.insert (d.end(), buf, buf + got);
    std::fclose (f);
    if (d.size() < 12 || std::memcmp (d.data(), "RIFF", 4) != 0 || std::memcmp (d.data() + 8, "WAVE", 4) != 0) { std::fprintf (stderr, "%s: not a WAV\n", path); std::exit (2); }
    int fmt = 0, bits = 0; size_t pos = 12; const unsigned char* data = nullptr; size_t dataLen = 0;
    while (pos + 8 <= d.size())
    {
        const uint32_t len = rd32 (d.data() + pos + 4); const unsigned char* p = d.data() + pos + 8;
        if (std::memcmp (d.data() + pos, "fmt ", 4) == 0)
        {
            fmt = rd16 (p); w.ch = rd16 (p + 2); w.sr = (int) rd32 (p + 4); bits = rd16 (p + 14);
            if (fmt == 0xFFFE && len >= 26) fmt = rd16 (p + 24);   // WAVE_FORMAT_EXTENSIBLE: sub-format GUID's first two bytes
        }
        else if (std::memcmp (d.data() + pos, "data", 4) == 0) { data = p; dataLen = std::min ((size_t) len, d.size() - pos - 8); }
        pos += 8 + len + (len & 1);
    }
    if (! data || w.ch < 1 || w.sr < 1) { std::fprintf (stderr, "%s: no data/fmt\n", path); std::exit (2); }
    const int bps = bits / 8; const size_t frames = dataLen / (size_t) (bps * w.ch);
    w.l.resize (frames); w.r.resize (frames);
    for (size_t i = 0; i < frames; ++i)
        for (int c = 0; c < std::min (w.ch, 2); ++c)
        {
            const unsigned char* s = data + (i * (size_t) w.ch + (size_t) c) * (size_t) bps; float v = 0.0f;
            if (fmt == 3 && bits == 32) { float x; std::memcpy (&x, s, 4); v = x; }
            else if (fmt == 1 && bits == 16) v = (float) (int16_t) rd16 (s) / 32768.0f;
            else if (fmt == 1 && bits == 24) v = (float) ((int32_t) (((uint32_t) s[0] << 8) | ((uint32_t) s[1] << 16) | ((uint32_t) s[2] << 24)) >> 8) / 8388608.0f;
            else if (fmt == 1 && bits == 32) v = (float) (int32_t) rd32 (s) / 2147483648.0f;
            else { std::fprintf (stderr, "%s: unsupported format %d / %d bit\n", path, fmt, bits); std::exit (2); }
            (c == 0 ? w.l : w.r)[i] = v;
        }
    if (w.ch == 1) w.r = w.l;
    return w;
}

static bool parseBand (const std::string& spec, dyneq::FreeBand& b)
{
    b = dyneq::FreeBand(); b.on = true;
    size_t pos = 0;
    while (pos <= spec.size())
    {
        size_t end = spec.find (',', pos); if (end == std::string::npos) end = spec.size();
        const std::string kv = spec.substr (pos, end - pos); pos = end + 1;
        if (kv.empty()) continue;
        const size_t eq = kv.find ('=');
        if (eq == std::string::npos) { std::fprintf (stderr, "band option '%s' is not key=value\n", kv.c_str()); return false; }
        const std::string k = kv.substr (0, eq), v = kv.substr (eq + 1); const float f = (float) std::atof (v.c_str());
        if (k == "type")
        {
            if (v == "bell") b.type = dyneq::kBell; else if (v == "lowshelf") b.type = dyneq::kLowShelf; else if (v == "highshelf") b.type = dyneq::kHighShelf;
            else if (v == "notch") b.type = dyneq::kNotch; else if (v == "lowcut") b.type = dyneq::kLowCut; else if (v == "highcut") b.type = dyneq::kHighCut;
            else { std::fprintf (stderr, "unknown band type %s\n", v.c_str()); return false; }
        }
        else if (k == "f") b.freqHz = f;
        else if (k == "g") b.gainDb = f;
        else if (k == "q") b.q = f;
        else if (k == "slope") b.slopeDb = (int) f;
        else if (k == "on") b.on = f != 0.0f;
        else if (k == "dyn") b.dyn = f != 0.0f;
        else if (k == "thr") b.thrDb = f;
        else if (k == "range") b.rangeDb = f;
        else if (k == "att") b.attackMs = f;
        else if (k == "rel") b.releaseMs = f;
        else if (k == "trig") b.trigger = v == "key" ? dyneq::kTrigKey : dyneq::kTrigSelf;
        else { std::fprintf (stderr, "unknown band key %s\n", k.c_str()); return false; }
    }
    return true;
}

static std::string squash (const std::string& v)   // lower case, letters and digits only: "SNARE BODY" -> "snarebody"
{
    std::string o; for (char c : v) if (std::isalnum ((unsigned char) c)) o += (char) std::tolower ((unsigned char) c); return o;
}

static bool parseTarget (const std::string& spec, dyneq::Params& p, float& partEnd)
{
    // keys are collected first: part=NAME needs the instrument, whatever the order
    std::string partV, instV; partEnd = -1.0f; bool havePart = false;
    size_t pos = 0;
    p.targetInst = dyneq::kTgtOff;
    while (pos <= spec.size())
    {
        size_t end = spec.find (',', pos); if (end == std::string::npos) end = spec.size();
        const std::string kv = spec.substr (pos, end - pos); pos = end + 1;
        if (kv.empty()) continue;
        const size_t eq = kv.find ('=');
        if (eq == std::string::npos) { std::fprintf (stderr, "target option '%s' is not key=value\n", kv.c_str()); return false; }
        const std::string k = kv.substr (0, eq), v = kv.substr (eq + 1); const float f = (float) std::atof (v.c_str());
        if (k == "inst") instV = v;
        else if (k == "part") { partV = v; havePart = true; }
        else if (k == "part_end") partEnd = f;
        else if (k == "amount") p.targetAmount = f;
        else if (k == "width") p.targetWidth = f;
        else if (k == "sweet") p.targetSweet = f;
        else if (k == "reverb") p.reverbReady = f != 0.0f;
        else if (k == "slope") p.targetSlope = (int) f;
        else { std::fprintf (stderr, "unknown target key %s\n", k.c_str()); return false; }
    }
    if (! instV.empty())
    {
        int hit = -1; for (int i = 0; i < dyneq::kTargetInsts; ++i) if (squash (instV) == squash (dyneq::kInst[i].name)) hit = i;
        if (hit < 0) { std::fprintf (stderr, "unknown target inst %s\n", instV.c_str()); return false; }
        p.targetInst = hit;
    }
    if (havePart)   // a number is the slider (0..100); a name picks that part of the instrument
    {
        if (! partV.empty() && (std::isdigit ((unsigned char) partV[0]) || partV[0] == '.')) p.targetPart = (float) std::atof (partV.c_str());
        else
        {
            int hit = -1; for (int i = 0; i < dyneq::kInst[p.targetInst].n; ++i) if (squash (partV) == squash (dyneq::partName (p.targetInst, i))) hit = i;
            if (hit < 0) { std::fprintf (stderr, "unknown part %s for %s\n", partV.c_str(), dyneq::kInst[p.targetInst].name); return false; }
            p.targetPart = dyneq::sliderForPart (p.targetInst, hit);
        }
    }
    return true;
}

static bool sameBand (const dyneq::FreeBand& a, const dyneq::FreeBand& b)
{
    return a.on == b.on && a.type == b.type && a.freqHz == b.freqHz && a.gainDb == b.gainDb && a.q == b.q && a.slopeDb == b.slopeDb && a.dyn == b.dyn
        && a.thrDb == b.thrDb && a.rangeDb == b.rangeDb && a.attackMs == b.attackMs && a.releaseMs == b.releaseMs && a.trigger == b.trigger;
}

static void writeWav (const char* path, int sr, int ch, const std::vector<float>& l, const std::vector<float>& r)
{
    FILE* f = std::fopen (path, "wb");
    if (! f) { std::fprintf (stderr, "cannot write %s\n", path); std::exit (2); }
    const uint32_t n = (uint32_t) l.size(), dataLen = n * 4u * (uint32_t) ch;
    auto w32 = [f] (uint32_t v) { unsigned char b[4] { (unsigned char) v, (unsigned char) (v >> 8), (unsigned char) (v >> 16), (unsigned char) (v >> 24) }; std::fwrite (b, 1, 4, f); };
    auto w16 = [f] (uint16_t v) { unsigned char b[2] { (unsigned char) v, (unsigned char) (v >> 8) }; std::fwrite (b, 1, 2, f); };
    std::fwrite ("RIFF", 1, 4, f); w32 (36 + dataLen); std::fwrite ("WAVE", 1, 4, f);
    std::fwrite ("fmt ", 1, 4, f); w32 (16); w16 (3); w16 ((uint16_t) ch); w32 ((uint32_t) sr); w32 ((uint32_t) sr * 4u * (uint32_t) ch); w16 ((uint16_t) (4 * ch)); w16 (32);
    std::fwrite ("data", 1, 4, f); w32 (dataLen);
    std::vector<float> inter ((size_t) n * (size_t) ch);
    for (uint32_t i = 0; i < n; ++i) { inter[(size_t) i * (size_t) ch] = l[i]; if (ch == 2) inter[(size_t) i * 2 + 1] = r[i]; }
    std::fwrite (inter.data(), 4, inter.size(), f);
    std::fclose (f);
}

int main (int argc, char** argv)
{
    if (argc < 4) { std::fprintf (stderr, "usage: dyneqraw main.wav key.wav|- out.wav [--mode collide|self] [--depth dB] [--margin dB] [--low Hz] [--high Hz] [--speed fast|medium|slow] [--mix 0..1] [--delta 0|1] [--rawbells] [--tiling] [--block N] [--cutlog f.csv] [--bandlog f.csv] [--time] [--bake] [--bake-from SEC] [--target inst=kick,part=body,amount=9,width=1[,sweet=120][,reverb=1]] [--learnlog f.csv] [--after SEC --atarget ...] [--band1 ... --band6 ...]\n"); return 2; }
    int aMask = 0; std::string aSpec[dyneq::kFreeBands];
    dyneq::Params p, p2; bool haveAfter = false; double afterS = 0.0; int block = dyneq::kBlock; const char* cutlog = nullptr; const char* bandlog = nullptr; const char* reallog = nullptr; const char* levellog = nullptr; const char* learnlog = nullptr; bool timing = false; bool bake = false; double bakeFrom = 0.0; std::string aTarget; float partEnd = -1.0f, partStart = 50.0f;
    std::vector<std::pair<double, std::string>> switchSpecs; const char* memIn = nullptr; double listenUntil = -1.0; bool edgeIn = false, blockTimes = false, latency = false; const char* memOut = nullptr; std::vector<std::pair<double, std::string>> memAts; bool noListen = false;
    for (int i = 4; i < argc; ++i)
    {
        const std::string a (argv[i]);
        auto next = [&] { if (i + 1 >= argc) { std::fprintf (stderr, "%s needs a value\n", a.c_str()); std::exit (2); } return std::string (argv[++i]); };
        if (a == "--mode") { const auto v = next(); p.mode = v == "self" ? dyneq::kSelf : dyneq::kCollide; }
        else if (a == "--depth") p.depthDb = (float) std::atof (next().c_str());
        else if (a == "--margin") p.marginDb = (float) std::atof (next().c_str());
        else if (a == "--low") p.focusLowHz = (float) std::atof (next().c_str());
        else if (a == "--keylaw") { const auto v = next(); p.keyLawMode = v == "091" ? dyneq::kLaw091 : dyneq::kLawSignal; }   // COLLIDE law: without the flag the plug-in default (Params::keyLawMode, 0.9.1-test2: 091); signal | 091
        else if (a == "--high") p.focusHighHz = (float) std::atof (next().c_str());
        else if (a == "--speed") { const auto v = next(); p.speed = v == "fast" ? dyneq::kFast : v == "slow" ? dyneq::kSlow : dyneq::kMedium; }
        else if (a == "--mix") p.mix = (float) std::atof (next().c_str());
        else if (a == "--delta") p.delta = std::atoi (next().c_str()) != 0;
        else if (a == "--rawbells") p.solveBells = false;
        else if (a == "--tiling") p.tiling = true;
        else if (a == "--block") block = std::max (1, std::atoi (next().c_str()));
        else if (a == "--cutlog") cutlog = argv[++i];
        else if (a == "--levellog") levellog = argv[++i];   // per block: the 32 COLLIDE main / key levels (SPEC 1 followers) m1..m32, k1..k32, the signal law's stage-1 levels mf, kf, its presences pk, pm and need (dB)
        else if (a == "--bandlog") bandlog = argv[++i];
        else if (a == "--reallog") reallog = argv[++i];   // per block: the realised cut (dB, positive = cut) at the 32 band centres of the COLLIDE bank c1..c32, the SELF hunters s1..s32, TARGET t1..t32, the six free bands' sum u1..u32, after the 32 bell gains g1..g32 (0 = bell skipped) and the 4 hunters (hz, cut dB, q) (tools/bench)
        else if (a == "--learnlog") learnlog = argv[++i];
        else if (a == "--time") timing = true;
        else if (a == "--bake") bake = true;
        else if (a == "--bake-from") bakeFrom = std::atof (next().c_str());
        else if (a == "--target") { if (! parseTarget (next(), p, partEnd)) return 2; partStart = p.targetPart; }
        else if (a == "--atarget") { aTarget = next(); haveAfter = true; }
        else if (a == "--switch") { const std::string v = next(); const size_t c = v.find (':'); if (c == std::string::npos) { std::fprintf (stderr, "--switch needs SEC:spec\n"); return 2; } switchSpecs.push_back ({ std::atof (v.substr (0, c).c_str()), v.substr (c + 1) }); }
        else if (a == "--mem-in") memIn = argv[++i];
        else if (a == "--mem-out") memOut = argv[++i];
        else if (a == "--mem-at") { const double sec = std::atof (next().c_str()); memAts.push_back ({ sec, std::string (argv[++i]) }); }
        else if (a == "--no-listen") noListen = true;
        else if (a == "--listen-until") listenUntil = std::atof (next().c_str());
        else if (a == "--edge-in") edgeIn = true;
        else if (a == "--latency") latency = true;
        else if (a == "--blocktimes") blockTimes = true;
        else if (a == "--after") afterS = std::atof (next().c_str());
        else if (a.size() == 8 && a.compare (0, 7, "--aband") == 0 && a[7] >= '1' && a[7] <= '6')
        {
            aSpec[a[7] - '1'] = next(); haveAfter = true; aMask |= 1 << (a[7] - '1');
        }
        else if (a.size() == 7 && a.compare (0, 6, "--band") == 0 && a[6] >= '1' && a[6] <= '6')
        {
            if (! parseBand (next(), p.band[a[6] - '1'])) return 2;
        }
        else { std::fprintf (stderr, "unknown argument %s\n", a.c_str()); return 2; }
    }

    Wav main = readWav (argv[1]);
    const bool hasKey = std::strcmp (argv[2], "-") != 0;
    Wav key; if (hasKey) key = readWav (argv[2]);
    const size_t n = main.l.size();
    if (hasKey)
    {
        if (key.sr != main.sr) { std::fprintf (stderr, "sample rates differ (%d vs %d)\n", main.sr, key.sr); return 2; }
        key.l.resize (n, 0.0f); key.r.resize (n, 0.0f);
    }
    const int ch = main.ch >= 2 ? 2 : 1;

    if (haveAfter)
    {
        p2 = p;
        if (! aTarget.empty()) { p2.targetPart = 50.0f; p2.targetAmount = 6.0f; p2.targetWidth = 1.0f; p2.targetSweet = 120.0f; p2.reverbReady = false; p2.targetSlope = 24; float le; if (! parseTarget (aTarget, p2, le)) return 2; }
        for (int i = 0; i < dyneq::kFreeBands; ++i)
            if (aMask & (1 << i))
            {
                if (aSpec[i] == "off") p2.band[i].on = false;
                else if (! parseBand (aSpec[i], p2.band[i])) return 2;
            }
    }
    std::vector<dyneq::Params> switchParams;
    for (const auto& sw : switchSpecs)
    {
        dyneq::Params q = p; q.targetPart = 50.0f; q.targetAmount = 6.0f; q.targetWidth = 1.0f; q.targetSweet = 120.0f; q.reverbReady = false; q.targetSlope = 24; float le;
        if (! parseTarget (sw.second, q, le)) return 2;
        switchParams.push_back (q);
    }
    size_t nextSwitch = 0;
    static dyneq::Engine eng;
    eng.prepare (main.sr, block);
    eng.setParams (p);
    auto saveMem = [&] (const char* path) { std::vector<float> m; eng.memoryExport (m); FILE* f = std::fopen (path, "wb"); if (! f) return; std::fwrite (m.data(), 4, m.size(), f); std::fclose (f); };
    if (memIn)
    {
        FILE* f = std::fopen (memIn, "rb"); std::vector<float> m;
        if (f) { float v; while (std::fread (&v, 4, 1, f) == 1) m.push_back (v); std::fclose (f); }
        if (! eng.memoryImport (m)) { std::fprintf (stderr, "memory file %s is not a memory\n", memIn); return 2; }
    }
    if (noListen) eng.setListening (false);
    if (edgeIn) eng.setEdgeAlwaysIn (true);
    size_t nextMemAt = 0;
    FILE* log = cutlog ? std::fopen (cutlog, "w") : nullptr;
    if (log)
    {
        std::fprintf (log, "block");
        for (int b = 0; b < dyneq::kBands; ++b) std::fprintf (log, ",%.1f", eng.bandCentresHz()[b]);
        if (p.mode == dyneq::kSelf) for (int s = 0; s < eng.numHunters(); ++s) std::fprintf (log, ",h%d_hz,h%d_db,h%d_q", s + 1, s + 1, s + 1);
        for (int i = 0; i < eng.numFreeBands(); ++i) std::fprintf (log, ",b%d_db", i + 1);
        std::fprintf (log, ",target_db\n");
    }
    FILE* blog = bandlog ? std::fopen (bandlog, "w") : nullptr;   // SPEC 8h: what the other layers do at each free band's frequency, per block (kept out of --cutlog, whose columns measure.py reads by position)
    if (blog)
    {
        std::fprintf (blog, "block");
        for (int i = 0; i < eng.numFreeBands(); ++i) std::fprintf (blog, ",b%d_hz,b%d_gain_db,b%d_conflict_db,b%d_self_db,b%d_target_db,b%d_net_db", i + 1, i + 1, i + 1, i + 1, i + 1, i + 1);
        for (int b = 0; b < dyneq::kBands; ++b) std::fprintf (blog, ",g%d", b + 1);   // the 32 COLLIDE bell gains (dB, positive = cut) the conflict is the sum of
        std::fprintf (blog, "\n");
    }

    FILE* rlog = reallog ? std::fopen (reallog, "w") : nullptr;
    if (rlog)
    {
        std::fprintf (rlog, "block");
        for (const char* L : { "g", "c", "s", "t", "u" }) for (int b = 0; b < dyneq::kBands; ++b) std::fprintf (rlog, ",%s%d", L, b + 1);
        for (int h = 0; h < eng.numHunters(); ++h) std::fprintf (rlog, ",h%d_hz,h%d_db,h%d_q", h + 1, h + 1, h + 1);
        std::fprintf (rlog, "\n");
    }
    FILE* vlog = levellog ? std::fopen (levellog, "w") : nullptr;
    if (vlog)
    {
        std::fprintf (vlog, "block");
        for (int b = 0; b < dyneq::kBands; ++b) std::fprintf (vlog, ",m%d", b + 1);
        for (int b = 0; b < dyneq::kBands; ++b) std::fprintf (vlog, ",k%d", b + 1);
        for (int b = 0; b < dyneq::kBands; ++b) std::fprintf (vlog, ",mf%d", b + 1);   // the signal law's fast presence levels (main, key)
        for (int b = 0; b < dyneq::kBands; ++b) std::fprintf (vlog, ",kf%d", b + 1);
        for (const char* L : { "pk", "pm", "need" }) for (int b = 0; b < dyneq::kBands; ++b) std::fprintf (vlog, ",%s%d", L, b + 1);   // the signal law's presence and need
        std::fprintf (vlog, "\n");
    }
    FILE* llog = learnlog ? std::fopen (learnlog, "w") : nullptr;
    if (llog)
    {
        std::fprintf (llog, "t_s,onsets,learned,hits,ready,conf,f0,trans_end_ms,voice,part,bell_hz,bell_db,rev_low,rev_high,rev_mix,rej_early,rej_late,rej_f0,rej_none");
        for (int k = 0; k < dyneq::kMaxParts; ++k) std::fprintf (llog, ",p%d_ok,p%d_c,p%d_lo,p%d_hi", k, k, k, k);
        std::fprintf (llog, ",relearns,edge_lo_hz,edge_lo_db,edge_lo_mix,edge_hi_hz,edge_hi_db,edge_hi_mix,rule_lo_hz,rule_hi_hz,prot_lo_db,prot_hi_db,ach_lo_db,ach_hi_db,gentle\n");
    }
    std::vector<double> bt; if (blockTimes) bt.reserve (n / (size_t) std::max (block, 1) + 8);
    const auto t0 = std::chrono::steady_clock::now();
    size_t blk = 0; bool targetSwitched = false;
    for (size_t pos = 0; pos < n; pos += (size_t) block, ++blk)
    {
        const int m = (int) std::min ((size_t) block, n - pos);
        if (bake && ! eng.bakeCollecting() && (double) pos >= bakeFrom * main.sr) { eng.bakeReset(); eng.bakeCollect (true); }
        while (nextSwitch < switchSpecs.size() && (double) pos >= switchSpecs[nextSwitch].first * main.sr) { eng.setParams (switchParams[nextSwitch]); ++nextSwitch; }
        if (listenUntil >= 0.0 && (double) pos >= listenUntil * main.sr) { eng.setListening (false); listenUntil = -1.0; }
        while (nextMemAt < memAts.size() && (double) pos >= memAts[nextMemAt].first * main.sr) { saveMem (memAts[nextMemAt].second.c_str()); ++nextMemAt; }
        if (haveAfter && (double) pos >= afterS * main.sr) { eng.setParams (p2); haveAfter = false; targetSwitched = true; }
        if (partEnd >= 0.0f && ! targetSwitched)   // PART sweeps linearly over the file until a --atarget switch
        {
            dyneq::Params q = p; q.targetPart = partStart + (partEnd - partStart) * (float) ((double) pos / (double) n);
            eng.setParams (q);
        }
        float* io[2] { main.l.data() + pos, main.r.data() + pos };
        const float* k[2] { hasKey ? key.l.data() + pos : nullptr, hasKey ? key.r.data() + pos : nullptr };
        if (blockTimes) { const auto b0 = std::chrono::steady_clock::now(); eng.process (io, hasKey ? k : nullptr, ch, m); bt.push_back (std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now() - b0).count()); }
        else eng.process (io, hasKey ? k : nullptr, ch, m);
        if (log)
        {
            std::fprintf (log, "%zu", blk);
            const float* c = eng.currentCutsDb();
            for (int b = 0; b < dyneq::kBands; ++b) std::fprintf (log, ",%.4f", c[b]);
            if (p.mode == dyneq::kSelf)
                for (int s = 0; s < eng.numHunters(); ++s)
                    std::fprintf (log, ",%.2f,%.4f,%.3f", eng.hunterFreqHz (s), eng.hunterGainDb (s), eng.hunterQ (s));
            for (int i = 0; i < eng.numFreeBands(); ++i) std::fprintf (log, ",%.4f", eng.freeBandGainDb (i));
            std::fprintf (log, ",%.4f\n", eng.targetCutDb());
        }
        if (rlog)
        {
            std::fprintf (rlog, "%zu", blk);
            const float* fcs = eng.bandCentresHz();
            for (int b = 0; b < dyneq::kBands; ++b) std::fprintf (rlog, ",%.5f", (double) (eng.bellActive (b) ? eng.bellGainsDb()[b] : 0.0f));
            for (int b = 0; b < dyneq::kBands; ++b) std::fprintf (rlog, ",%.4f", -(double) eng.collideDbAt ((double) fcs[b]));
            for (int b = 0; b < dyneq::kBands; ++b) std::fprintf (rlog, ",%.4f", -(double) eng.selfDbAt ((double) fcs[b]));
            for (int b = 0; b < dyneq::kBands; ++b) std::fprintf (rlog, ",%.4f", -(double) eng.targetDbAt ((double) fcs[b]));
            for (int b = 0; b < dyneq::kBands; ++b)
            {
                double u = 0.0;
                for (int i = 0; i < dyneq::kFreeBands; ++i)
                    if (p.band[i].on) u += dyneq::Engine::freeBandMagDb (p.band[i].type, (double) eng.freeBandFreqHz (i), (double) eng.freeBandGainDb (i), (double) eng.freeBandQ (i), p.band[i].slopeDb, (double) fcs[b], (double) main.sr);
                std::fprintf (rlog, ",%.4f", -u);
            }
            for (int h = 0; h < eng.numHunters(); ++h) std::fprintf (rlog, ",%.3f,%.5f,%.4f", (double) eng.hunterFreqHz (h), (double) ((eng.hunterActive (h) ? -eng.hunterGainDb (h) : 0.0f)), (double) eng.hunterQ (h));
            std::fprintf (rlog, "\n");
        }
        if (vlog)
        {
            std::fprintf (vlog, "%zu", blk);
            for (int b = 0; b < dyneq::kBands; ++b) std::fprintf (vlog, ",%.2f", (double) std::max (eng.mainLevelsDb()[b], -200.0f));
            for (int b = 0; b < dyneq::kBands; ++b) std::fprintf (vlog, ",%.2f", (double) std::max (eng.keyLevelsDb()[b], -200.0f));
            for (int b = 0; b < dyneq::kBands; ++b) std::fprintf (vlog, ",%.2f", (double) std::max (eng.fastMainLevelsDb()[b], -200.0f));
            for (int b = 0; b < dyneq::kBands; ++b) std::fprintf (vlog, ",%.2f", (double) std::max (eng.fastKeyLevelsDb()[b], -200.0f));
            for (int b = 0; b < dyneq::kBands; ++b) std::fprintf (vlog, ",%.3f", (double) eng.sigPresenceKey()[b]);
            for (int b = 0; b < dyneq::kBands; ++b) std::fprintf (vlog, ",%.3f", (double) eng.sigPresenceMain()[b]);
            for (int b = 0; b < dyneq::kBands; ++b) std::fprintf (vlog, ",%.2f", (double) eng.sigNeedDb()[b]);
            std::fprintf (vlog, "\n");
        }
        if (blog)
        {
            std::fprintf (blog, "%zu", blk);
            for (int i = 0; i < eng.numFreeBands(); ++i) std::fprintf (blog, ",%.3f,%.4f,%.4f,%.4f,%.4f,%.4f", (double) eng.bandFreqForLayersHz (i), (double) eng.freeBandGainDb (i), (double) eng.bandConflictDb (i), (double) eng.bandSelfDb (i), (double) eng.bandTargetDb (i), (double) eng.bandNetDb (i));
            for (int b = 0; b < dyneq::kBands; ++b) std::fprintf (blog, ",%.5f", (double) (eng.bellGainsDb()[b]));
            std::fprintf (blog, "\n");
        }
        if (llog)
        {
            const auto& ln = eng.targetLearned();
            std::fprintf (llog, "%.5f,%d,%d,%d,%d,%.3f,%.3f,%.3f,%d,%d,%.2f,%.4f,%.2f,%.2f,%.4f", (double) (pos + (size_t) m) / main.sr, eng.targetOnsets(), eng.targetHitsLearned(), ln.hits, ln.ready ? 1 : 0, (double) ln.confidence,
                          (double) ln.f0, (double) ln.transEndMs, eng.targetVoice(), eng.targetPartNow(), (double) eng.targetBellFreqHz (dyneq::kTargetBells / 2), (double) eng.targetBellCutDb (dyneq::kTargetBells / 2), (double) eng.targetRevLowHz(), (double) eng.targetRevHighHz(), (double) eng.targetRevMix());
            std::fprintf (llog, ",%d,%d,%d,%d", eng.targetRejected (0), eng.targetRejected (1), eng.targetRejected (2), eng.targetRejected (3));
            for (int k = 0; k < dyneq::kMaxParts; ++k) std::fprintf (llog, ",%d,%.2f,%.2f,%.2f", ln.ok[k] ? 1 : 0, (double) ln.centre[k], (double) ln.lo[k], (double) ln.hi[k]);
            std::fprintf (llog, ",%d,%.3f,%.4f,%.4f,%.3f,%.4f,%.4f", eng.targetRelearns(), (double) eng.targetEdgeHz (0), (double) eng.targetEdgeCutDb (0), (double) eng.targetEdgeMix (0), (double) eng.targetEdgeHz (1), (double) eng.targetEdgeCutDb (1), (double) eng.targetEdgeMix (1));
            std::fprintf (llog, ",%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d\n", (double) eng.targetEdgeRuleHz (0), (double) eng.targetEdgeRuleHz (1), (double) eng.targetProtectLossDb (0), (double) eng.targetProtectLossDb (1),
                          (double) eng.targetAchievedCutDb (0), (double) eng.targetAchievedCutDb (1), eng.targetSlopeTooGentle() ? 1 : 0);
        }
    }
    if (memOut) saveMem (memOut);
    const double procS = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
    if (log) std::fclose (log);
    if (vlog) std::fclose (vlog);
    if (rlog) std::fclose (rlog);
    if (blog) std::fclose (blog);
    if (llog) std::fclose (llog);
    if (blockTimes && ! bt.empty())
    {
        std::vector<double> v = bt; std::sort (v.begin(), v.end()); double sum = 0.0; for (double x : v) sum += x;
        std::printf ("block_us n=%zu mean=%.1f p99=%.1f p999=%.1f max=%.1f\n", v.size(), sum / (double) v.size(), v[(size_t) ((double) (v.size() - 1) * 0.99)], v[(size_t) ((double) (v.size() - 1) * 0.999)], v.back());
    }
#ifdef DYNEQ_PROFILE
    std::printf ("prof_call n=%d max_us=%.1f mean_us=%.2f max_units=%.0f | evidence records=%lld latency_max_samples=%lld\n", eng.profCalls, eng.profCallMax, eng.profCalls ? eng.profCallSum / eng.profCalls : 0.0, eng.profCallUnitsMax, (long long) eng.evidenceRecords(), (long long) eng.evidenceLatencyMaxSamples());
    for (int i = 0; i < 8; ++i) std::printf ("prof stage %d n=%d max_us=%.1f mean_us=%.2f units_per_us=%.2f\n", i, eng.profN[i], eng.profMax[i], eng.profN[i] ? eng.profSum[i] / eng.profN[i] : 0.0, eng.profSum[i] > 0 ? eng.profUnits[i] / eng.profSum[i] / 1000.0 : 0.0);
#endif
#ifdef DYNEQ_PROFILE
    { const char* nm[9] = { "copy", "f0", "transEnd", "removeSine", "psd window", "psd bitrev", "psd butterflies", "psd power", "hpWindow" }; for (int i = 0; i < 9; ++i) std::printf ("prim %-16s %.0f us, %.0f units, %.2f units/ns\n", nm[i], eng.pPrimUs[i], eng.pPrimUnits[i], eng.pPrimUs[i] > 0 ? eng.pPrimUnits[i] / (eng.pPrimUs[i] * 1000.0) : 0.0); }
#endif
#ifdef DYNEQ_RECLOG
    if (const char* rf = std::getenv ("DYNEQ_RECLOG_FILE"))
        if (FILE* f = std::fopen (rf, "a"))
        {
            std::fprintf (f, "run\n");
            for (const auto& r : eng.recLog)
            {
                std::fprintf (f, "slot %d ok %d why %d key %a f0 %a tEnd %a", r.slot, r.ok, r.why, (double) r.h.key, (double) r.h.f0, (double) r.h.tEnd);
                for (int p = 0; p < dyneq::kMaxParts; ++p) std::fprintf (f, " | %d %a %a %a", (int) r.h.ok[p], (double) r.h.c[p], (double) r.h.lo[p], (double) r.h.hi[p]);
                std::fprintf (f, "\n");
            }
            std::fclose (f);
        }
#endif
    if (latency) std::printf ("evidence_latency records=%lld max_samples=%lld last_samples=%lld\n", (long long) eng.evidenceRecords(), (long long) eng.evidenceLatencyMaxSamples(), (long long) eng.evidenceLatencyLastSamples());
    if (timing) std::printf ("audio_s=%.4f proc_s=%.4f realtime_x=%.1f\n", (double) n / main.sr, procS, ((double) n / main.sr) / std::max (procS, 1e-9));

    if (bake)
    {
        float mean[dyneq::kBands], pct[dyneq::kBands]; double resid[dyneq::kBands];
        for (int b = 0; b < dyneq::kBands; ++b) { mean[b] = eng.bakeMeanCutDb (b); pct[b] = eng.bakePercentCut (b); }
        int empty = 0; for (int i = 0; i < dyneq::kFreeBands; ++i) if (! p.band[i].on) ++empty;
        dyneq::BakeBell bells[dyneq::kBakeMaxBells];
        const int nb = dyneq::bakeFit (mean, pct, eng.bandCentresHz(), main.sr, empty, bells, resid);
        std::printf ("BAKE counted=%llu seen=%llu empty_bands=%d bells=%d\n", (unsigned long long) eng.bakeBlocksCounted(), (unsigned long long) eng.bakeBlocksSeen(), empty, nb);
        std::printf ("BAKE centres_hz"); for (int b = 0; b < dyneq::kBands; ++b) std::printf (" %.2f", (double) eng.bandCentresHz()[b]); std::printf ("\n");
        std::printf ("BAKE mean_cut_db"); for (int b = 0; b < dyneq::kBands; ++b) std::printf (" %.5f", (double) mean[b]); std::printf ("\n");
        std::printf ("BAKE percent_cut"); for (int b = 0; b < dyneq::kBands; ++b) std::printf (" %.3f", (double) pct[b]); std::printf ("\n");
        for (int i = 0; i < nb; ++i)
        {
            char txt[160]; dyneq::bakeBellText (bells[i], txt, sizeof (txt));
            std::printf ("BAKE bell %d: band=%d f=%.3f g=%.4f q=%.4f mean_cut_db=%.4f percent_cut=%.2f  | %s\n", i + 1, bells[i].band, (double) bells[i].freqHz, (double) bells[i].gainDb, (double) bells[i].q, (double) bells[i].meanCutDb, (double) bells[i].percentCut, txt);
        }
        std::printf ("BAKE specs"); for (int i = 0; i < nb; ++i) std::printf (" type=bell,f=%.3f,g=%.4f,q=%.4f", (double) bells[i].freqHz, (double) bells[i].gainDb, (double) bells[i].q); std::printf ("\n");
        std::printf ("BAKE curve_db"); for (int b = 0; b < dyneq::kBands; ++b) std::printf (" %.5f", dyneq::bakedCurveDb (bells, nb, (double) eng.bandCentresHz()[b], main.sr)); std::printf ("\n");
        std::printf ("BAKE residual_db"); for (int b = 0; b < dyneq::kBands; ++b) std::printf (" %.5f", resid[b]); std::printf ("\n");
        // undo self-check at the Params level: apply into the empty bands, put the saved copy back, every band field must be identical
        const dyneq::Params before = p; dyneq::Params applied = p;
        const int written = dyneq::bakeApplyToParams (applied, bells, nb);
        dyneq::Params restored = applied; for (int i = 0; i < dyneq::kFreeBands; ++i) restored.band[i] = before.band[i];
        bool same = true; for (int i = 0; i < dyneq::kFreeBands; ++i) same = same && sameBand (restored.band[i], before.band[i]);
        int changed = 0; for (int i = 0; i < dyneq::kFreeBands; ++i) if (! sameBand (applied.band[i], before.band[i])) ++changed;
        std::printf ("BAKE undo_params written=%d bands_changed=%d restored_bit_exact=%d\n", written, changed, same ? 1 : 0);
    }
    writeWav (argv[3], main.sr, ch, main.l, main.r);
    return 0;
}
