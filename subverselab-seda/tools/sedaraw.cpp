// sedaraw in.wav out.wav [--time ms | --note 1/4d --bpm 120] [--fb 0..1.1] [--mix 0..1] [--tone -1..1] [--rate Hz] [--depth 0..1]
//         [--runaway 0..1] [--chaos 0..1] [--age new|old] [--comp 0..1] [--stereo mono|wide|pingpong] [--width ms] [--block N] [--time-it]
//         extras: [--shape sine|tri] [--bypass] [--tail sec] [--time-to ms --at sec --over ms]   (TIME automation ramp, applied once per block)
//         automation (P1d): [--fb-at sec value]... FEEDBACK steps (repeatable; the engine smooths them over 20 ms), [--time-at sec ms]... TIME breakpoints (linear between them, repeatable; first one holds before it, last one after it)
//         LISTEN (P1b): [--listen] [--duck 0..1] [--throw dB 0..12] [--note auto] [--wethp Hz] [--source off|vocal|drums|bass|keys] [--beats-per-bar N] [--print-effective] [--eventlog [file]]
//         --note auto = NOTE AUTO (sync on, note follows the density of the source at bar lines). --source loads the SPEC 2.5 starting point for the knobs that no flag touched (the SOURCE selector). Every flag given counts as "touched".
//         --gaintrace file (P3e): CSV time_s,duck_db,throw_lift_db after every block (what LISTEN applies to the wet: the duck, and the throw lift above it)
//         --eventlog prints the internal moves as CSV (time_s,event,a,b) to stdout, or to the file if one is given:
//         duck (one per state change: a = 1 in / 0 out, b = depth dB), throw_on (a = THROW dB, b = seconds of material), throw_off, note (a = note index, b = seconds), guard_on (a = wet dB), guard_off (a = guard dB)
// Offline renderer: runs Source/SedaEngine.h exactly as the plug-in will. WAV in: PCM 16/24/32 or float32, mono or stereo.
// WAV out: float32 stereo-or-mono as the input, optionally extended by --tail seconds of silence so the repeats can ring out.
// --time-it prints "audio_s=... proc_s=... realtime_x=..." on stdout (processing only).
#include "../Source/SedaEngine.h"
#include "wavio.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

int main (int argc, char** argv)
{
    if (argc < 3) { std::fprintf (stderr, "usage: sedaraw in.wav out.wav [--time ms|--note 1/4d --bpm 120] [--fb 0..1.1] [--mix] [--tone] [--rate] [--depth] [--age new|old] [--comp 0..1] [--stereo mono|wide|pingpong] [--width ms] [--block N] [--time-it] [--shape sine|tri] [--bypass] [--tail sec] [--time-to ms --at sec --over ms]\n"); return 2; }
    seda::Params p; int block = 512; bool timing = false; bool evlog = false, printEff = false; std::string evPath; std::string tracePath; std::vector<double> trT, trDuck, trLift; double tail = 0.0, toMs = -1.0, atS = 0.0, overMs = 0.0; std::vector<std::pair<double, double>> fbBp, timeBp;
    for (int i = 3; i < argc; ++i)
    {
        const std::string a (argv[i]);
        auto next = [&] { if (i + 1 >= argc) { std::fprintf (stderr, "%s needs a value\n", a.c_str()); std::exit (2); } return std::string (argv[++i]); };
        if (a == "--time") { p.timeMs = (float) std::atof (next().c_str()); p.sync = false; p.touched |= seda::kTouchNote; }
        else if (a == "--note")
        {
            const auto v = next(); p.sync = true; p.touched |= seda::kTouchNote;
            if (v == "auto") p.noteAuto = true;
            else { p.note = seda::noteIndex (v.c_str()); if (p.note < 0) { std::fprintf (stderr, "unknown note %s\n", v.c_str()); return 2; } }
        }
        else if (a == "--note-init") { const auto v = next(); p.note = seda::noteIndex (v.c_str()); if (p.note < 0) { std::fprintf (stderr, "unknown note %s\n", v.c_str()); return 2; } }   // starting note of the NOTE knob without turning sync on
        else if (a == "--listen") p.listen = true;
        else if (a == "--duck") { p.duck = (float) std::atof (next().c_str()); p.touched |= seda::kTouchDuck; }
        else if (a == "--throw") { p.throwDb = (float) std::atof (next().c_str()); p.touched |= seda::kTouchThrow; }
        else if (a == "--wethp") { p.wetHpHz = (float) std::atof (next().c_str()); p.touched |= seda::kTouchWetHp; }
        else if (a == "--nosense") {}   // accepted and ignored: SourceSense detection is gone from LISTEN (P1d)
        else if (a == "--source") { const auto v = next(); p.source = seda::sourceIndex (v.c_str()); if (p.source < 0) { std::fprintf (stderr, "unknown source %s\n", v.c_str()); return 2; } }
        else if (a == "--print-effective") printEff = true;
        else if (a == "--beats-per-bar") p.beatsPerBar = std::max (1, std::atoi (next().c_str()));
        else if (a == "--gaintrace") tracePath = next();
        else if (a == "--eventlog") { evlog = true; if (i + 1 < argc && std::string (argv[i + 1]).rfind ("--", 0) != 0) evPath = argv[++i]; }
        else if (a == "--bpm") p.bpm = std::atof (next().c_str());
        else if (a == "--fb") p.feedback = (float) std::atof (next().c_str());
        else if (a == "--mix") { p.mix = (float) std::atof (next().c_str()); p.touched |= seda::kTouchMix; }
        else if (a == "--tone") p.tone = (float) std::atof (next().c_str());
        else if (a == "--rate") p.rateHz = (float) std::atof (next().c_str());
        else if (a == "--depth") p.depth = (float) std::atof (next().c_str());
        else if (a == "--runaway") p.runaway = (float) std::atof (next().c_str());   // 17 Oct: RUNAWAY 0..1
        else if (a == "--chaos") p.chaos = (float) std::atof (next().c_str());       // 17 Oct: CHAOS 0..1 (steps every delay time: no host position offline)
        else if (a == "--age") p.age = next() == "old" ? seda::kOld : seda::kNew;
        else if (a == "--comp") p.comp = (float) std::atof (next().c_str());
        else if (a == "--stereo") { const auto v = next(); p.stereo = v == "wide" ? seda::kWide : v == "pingpong" ? seda::kPingPong : seda::kMono; p.touched |= seda::kTouchStereo; }
        else if (a == "--width") p.widthMs = (float) std::atof (next().c_str());
        else if (a == "--shape") p.lfoShape = next() == "tri" ? seda::kTriangle : seda::kSine;
        else if (a == "--bypass") p.bypass = true;
        else if (a == "--tail") tail = std::atof (next().c_str());
        else if (a == "--fb-at") { const double t = std::atof (next().c_str()); fbBp.emplace_back (t, std::atof (next().c_str())); }
        else if (a == "--time-at") { const double t = std::atof (next().c_str()); timeBp.emplace_back (t, std::atof (next().c_str())); p.sync = false; p.touched |= seda::kTouchNote; }
        else if (a == "--time-to") toMs = std::atof (next().c_str());
        else if (a == "--at") atS = std::atof (next().c_str());
        else if (a == "--over") overMs = std::atof (next().c_str());
        else if (a == "--block") block = std::max (1, std::atoi (next().c_str()));
        else if (a == "--time-it") timing = true;
        else { std::fprintf (stderr, "unknown argument %s\n", a.c_str()); return 2; }
    }

    Wav w = readWav (argv[1]);
    const int ch = w.ch >= 2 ? 2 : 1;
    const size_t n = w.l.size() + (size_t) (tail * w.sr);
    w.l.resize (n, 0.0f); w.r.resize (n, 0.0f);
    static seda::Engine eng;
    eng.prepare ((double) w.sr);
    const double t0ms = p.timeMs;
    eng.setParams (p);
    eng.reset();
    eng.setEventLog (evlog);
    std::vector<seda::Event> events;

    const auto t0 = std::chrono::steady_clock::now();
    for (size_t pos = 0; pos < n; pos += (size_t) block)
    {
        const int m = (int) std::min ((size_t) block, n - pos);
        if (toMs > 0.0)
        {
            const double t = (double) pos / w.sr, u = overMs > 0.0 ? std::min (1.0, std::max (0.0, (t - atS) / (overMs * 0.001))) : (t >= atS ? 1.0 : 0.0);
            p.timeMs = (float) (t0ms + (toMs - t0ms) * u); eng.setParams (p);
        }
        if (! fbBp.empty() || ! timeBp.empty())
        {
            const double t = (double) pos / w.sr;
            for (const auto& b : fbBp) if (t >= b.first) p.feedback = (float) b.second;   // breakpoints in time order: the last one passed wins
            if (! timeBp.empty())
            {
                double v = timeBp.front().second;
                for (size_t k = 0; k < timeBp.size(); ++k)
                {
                    if (t >= timeBp[k].first) v = timeBp[k].second;
                    if (k + 1 < timeBp.size() && t >= timeBp[k].first && t < timeBp[k + 1].first) v = timeBp[k].second + (timeBp[k + 1].second - timeBp[k].second) * (t - timeBp[k].first) / (timeBp[k + 1].first - timeBp[k].first);
                }
                p.timeMs = (float) v;
            }
            eng.setParams (p);
        }
        eng.process (w.l.data() + pos, w.r.data() + pos, w.l.data() + pos, w.r.data() + pos, m);
        if (! tracePath.empty()) { trT.push_back ((double) (pos + (size_t) m) / w.sr); trDuck.push_back (eng.duckDb()); trLift.push_back (eng.throwLiftDb()); }   // after each block: the duck and the throw lift LISTEN is applying
        { seda::Event ev; while (evlog && eng.popEvent (ev)) events.push_back (ev); }
    }
    const double procS = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
    if (timing) std::printf ("audio_s=%.4f proc_s=%.4f realtime_x=%.1f\n", (double) n / w.sr, procS, ((double) n / w.sr) / std::max (procS, 1e-9));

    writeWav (argv[2], w.sr, ch, w.l, w.r);
    if (printEff)   // what LISTEN ended up using: the user's values, or the SOURCE selector's starting point where the knob was untouched
    {
        const auto e = eng.effective();
        std::printf ("effective: source=%s sync=%d note=%s wethp=%.0f duck=%.2f throw=%.1f mixmax=%.2f stereo=%d\n", seda::sourceName (p.source),
                     e.sync ? 1 : 0, seda::noteName (e.note), (double) e.wetHpHz, (double) e.duck, (double) e.throwDb, (double) e.mixMax, e.stereo);
    }
    if (! tracePath.empty())
    {
        FILE* f = std::fopen (tracePath.c_str(), "w"); if (! f) { std::fprintf (stderr, "cannot write %s\n", tracePath.c_str()); return 2; }
        std::fprintf (f, "time_s,duck_db,throw_lift_db\n");
        for (size_t i = 0; i < trT.size(); ++i) std::fprintf (f, "%.5f,%.4f,%.4f\n", trT[i], trDuck[i], trLift[i]);
        std::fclose (f);
    }
    if (evlog)
    {
        FILE* f = evPath.empty() ? stdout : std::fopen (evPath.c_str(), "w");
        if (! f) { std::fprintf (stderr, "cannot write %s\n", evPath.c_str()); return 2; }
        std::fprintf (f, "time_s,event,a,b\n");
        for (const auto& e : events) std::fprintf (f, "%.5f,%s,%.4f,%.4f\n", (double) e.frame / w.sr, seda::eventName (e.type), e.a, e.b);
        if (f != stdout) std::fclose (f);
    }
    return 0;
}
