// track_regress: TRACK off must be bit-exact with the engine of 1.1.0.
//
// Links two copies of the engine into one program: tests/regress/KasetEngine_baseline.h is Source/KasetEngine.h exactly as it was before the
// TRACK switch (copied unchanged, namespace renamed here by a macro), Source/KasetEngine.h is the new one. The same input and settings go through
// both, in blocks of 512 and of 97, and every output sample is compared bit for bit.
//
//   build:  clang++ -std=c++17 -O2 tools/track_regress.cpp -o build/track_regress      (or the track_regress CMake target)
//   run:    build/track_regress            exit code 0 only if the largest difference is exactly 0 everywhere
//
// Also checked: a new engine that was switched to TRACK and back to MIX before it ran gives the same bits as the baseline.
#define kaset kaset_old
#include "../tests/regress/KasetEngine_baseline.h"
#undef kaset
#include "../Source/KasetEngine.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
struct Preset { const char* name; int gen; float wow, flutter, drive, hiss, wear; int tape; };

std::vector<float> makeInput (int n, double sr, int channel)
{
    // Drum-like: noise bed, a decaying 60 Hz kick every 0.5 s, a noise burst every 0.5 s offset, and a 1 kHz tone for the speed errors.
    std::vector<float> x ((size_t) n);
    uint32_t s = 24680u + (uint32_t) channel * 977u;
    for (int i = 0; i < n; ++i)
    {
        s = s * 1664525u + 1013904223u;
        const double t = (double) i / sr, ph = std::fmod (t, 0.5);
        double v = 0.05 * ((double) (s >> 8) / 16777216.0 * 2.0 - 1.0);
        v += 0.6 * std::exp (-ph * 18.0) * std::sin (2.0 * 3.14159265358979 * 60.0 * ph);
        const double sn = std::fmod (t + 0.25, 0.5);
        v += 0.3 * std::exp (-sn * 40.0) * ((double) (s >> 9) / 8388608.0 * 2.0 - 1.0);
        v += 0.2 * std::sin (2.0 * 3.14159265358979 * 1000.0 * t);
        x[(size_t) i] = (float) v;
    }
    return x;
}
}

int main()
{
    const Preset presets[] = {
        { "G1 master, default",         1, 0.50f, 0.50f, 0.55f, 0.50f, 0.25f, 0 },
        { "G2 first dub, chrome",       2, 0.40f, 0.40f, 0.50f, 0.45f, 0.10f, 1 },
        { "G3 second dub (the demo)",   3, 0.50f, 0.50f, 0.55f, 0.50f, 0.25f, 0 },
        { "G4 third dub, metal",        4, 0.80f, 0.60f, 0.70f, 0.30f, 0.50f, 2 },
        { "G5 fourth dub",              5, 0.55f, 0.50f, 0.55f, 0.50f, 0.30f, 0 },
        { "G5 all max",                 5, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 2 },
        { "G5 all zero",                5, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0 },
        { "G2 wow max only",            2, 1.00f, 0.00f, 0.50f, 0.00f, 0.00f, 0 },
    };
    long compared = 0, different = 0;
    double maxDiff = 0.0;
    bool latencyOk = true;
    int cases = 0;

    for (double sr : { 44100.0, 48000.0, 96000.0 })
        for (const auto& p : presets)
            for (int block : { 512, 97 })
                for (int viaTrack = 0; viaTrack < 2; ++viaTrack)
                {
                    const int n = (int) (3.0 * sr);
                    auto inL = makeInput (n, sr, 0), inR = makeInput (n, sr, 1);
                    auto oldL = inL, oldR = inR, newL = inL, newR = inR;

                    kaset::Controls c;
                    c.generation = p.gen; c.wow = p.wow; c.flutter = p.flutter; c.drive = p.drive; c.hiss = p.hiss; c.wear = p.wear;
                    c.tape = (kaset::TapeType) p.tape;
                    kaset_old::Controls co;
                    co.generation = p.gen; co.wow = p.wow; co.flutter = p.flutter; co.drive = p.drive; co.hiss = p.hiss; co.wear = p.wear;
                    co.tape = (kaset_old::TapeType) p.tape;

                    static kaset_old::KasetEngine oldEng;
                    static kaset::KasetEngine newEng;
                    oldEng.prepare (sr);
                    newEng.prepare (sr);
                    if (viaTrack) { newEng.setTrack (true); newEng.setTrack (false); }   // on and off again before it runs: must leave nothing behind
                    latencyOk &= oldEng.latency() == newEng.latency() && newEng.latency() == kaset::KasetEngine::latencyFor (sr, false);

                    for (int pos = 0; pos < n; pos += block)
                    {
                        const int m = std::min (block, n - pos);
                        oldEng.process (&oldL[(size_t) pos], &oldR[(size_t) pos], m, co);
                        newEng.process (&newL[(size_t) pos], &newR[(size_t) pos], m, c);
                    }
                    long diffs = 0; double worst = 0.0;
                    for (int i = 0; i < n; ++i)
                    {
                        if (std::memcmp (&oldL[(size_t) i], &newL[(size_t) i], sizeof (float)) != 0) ++diffs;
                        if (std::memcmp (&oldR[(size_t) i], &newR[(size_t) i], sizeof (float)) != 0) ++diffs;
                        worst = std::max (worst, (double) std::fabs (oldL[(size_t) i] - newL[(size_t) i]));
                        worst = std::max (worst, (double) std::fabs (oldR[(size_t) i] - newR[(size_t) i]));
                    }
                    compared += 2L * n; different += diffs; maxDiff = std::max (maxDiff, worst);
                    ++cases;
                    if (diffs != 0)
                        std::printf ("DIFF  %-28s %6.0f Hz block %3d %s: %ld samples differ, max |diff| %g\n", p.name, sr, block, viaTrack ? "(via TRACK on/off)" : "", diffs, worst);
                }

    std::printf ("%d cases (8 presets x 3 rates x 2 block sizes x 2 start states), %ld samples compared, %ld differ, max |diff| = %g, latency %s\n",
                cases, compared, different, maxDiff, latencyOk ? "unchanged" : "CHANGED");
    const bool ok = different == 0 && maxDiff == 0.0 && latencyOk;
    std::printf("%s\n", ok ? "PASS  TRACK off is bit-exact with the engine of 1.1.0" : "FAIL  TRACK off differs from the engine of 1.1.0");
    return ok ? 0 : 1;
}
