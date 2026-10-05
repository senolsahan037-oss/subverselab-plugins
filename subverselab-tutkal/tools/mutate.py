#!/usr/local/bin/python3
"""Puts each known fault back into a copy of the engine and checks that the measurement guarding
against it fails (Rules/09_FAILURE_LOG.md L2: a guard is trusted only after it was seen to fail).

  mutate.py        every mutation; exit code = number of guards that did NOT catch their fault
"""
import os, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.join(HERE, "..")
ENGINE = os.path.join(ROOT, "Source", "TutkalEngine.h")

# (name, the line as it is, the fault, the check that must fail)
MUTATIONS = [
    ("overshooting attack (second-order envelope)", "yL = aAtt * yL + (1.0f - aAtt) * y1;\n                if (yL < 1.0e-5f && y1 < 1.0e-5f) { yL = 0.0f; if",
     "{ static float v = 0.0f; v = 0.97f * v + (1.0f - aAtt) * (y1 - yL); yL += v; }\n                if (yL < 1.0e-5f && y1 < 1.0e-5f) { yL = 0.0f; if", "M4"),
    ("knee formula wrong", "return slope * t * t / (2.0f * knee);", "return slope * t * t / (1.5f * knee);", "M1"),
    ("attack is 10-90 % instead of 63 %", "aAtt = (float) std::exp (-1.0 / (0.001 * sAtt * fs));", "aAtt = (float) std::exp (-2.2 / (0.001 * sAtt * fs));", "M2"),
    ("release time doubled", "aRel = (float) std::exp (-1.0 / (0.001 * sRel * fs));", "aRel = (float) std::exp (-1.0 / (0.002 * sRel * fs));", "M3"),
    ("AUTO slow path charges at once (no memory)", "aAutoCharge = (float) std::exp (-1.0 / (0.800 * fs));", "aAutoCharge = (float) std::exp (-1.0 / (0.005 * fs));", "M5"),
    ("release left out between peaks (ripple)", "y1 = std::max (target, aRel * y1 + (1.0f - aRel) * target);", "y1 = target;", "M6"),
    ("drive not level-compensated", "float s = k > 1.0e-4f ? std::tanh (k * v) / k : v;", "float s = k > 1.0e-4f ? std::tanh (k * v) : v;", "M8"),
    ("no oversampling filter worth the name", "upA.design (115, 9.0, 2.0); downA.design (115, 9.0, 1.0);", "upA.design (115, 0.5, 2.0); downA.design (115, 0.5, 1.0);", "M9"),
    ("dry path not delayed", "const float dry = dd;", "const float dry = x[c];", "M10"),
    ("drive path a quarter sample short", "static constexpr int kPad = 5;", "static constexpr int kPad = 4;", "M11"),
    ("coefficients refreshed per block, not per sample count", "if ((count++ & 15) == 0)", "if (i == 0)", "M12"),
    ("time constants tied to 48 kHz", "aAtt = (float) std::exp (-1.0 / (0.001 * sAtt * fs));", "aAtt = (float) std::exp (-1.0 / (0.001 * sAtt * 48000.0));", "M13"),
    ("makeup not smoothed", "sMake  += smooth * (tMake - sMake);", "sMake = tMake;", "M14"),
    ("input not sanitised", "x[c] = std::isfinite (v) ? std::clamp (v, -100.0f, 100.0f) : 0.0f;", "x[c] = v;", "M15"),
    ("EXT off still hears the sidechain", "if (p.ext)\n", "if (p.ext || keyChannels > 0)\n", "M16"),
    ("EXT on falls back to the programme", "d[0] = keyChannels > 0 ? sane (key[0][i]) : 0.0f;", "d[0] = keyChannels > 0 ? sane (key[0][i]) : x[0];", "M16"),
    ("gain reduction not limited", "const float level = pk;\n                const float levelDb = 20.0f * std::log10 (std::max (level, 1.0e-7f));\n                const float target = std::min (staticGr (levelDb - sThr, sRatio, knee), kMaxGrDb);",
     "const float level = pk;\n                const float levelDb = 20.0f * std::log10 (std::max (level, 1.0e-7f));\n                const float target = staticGr (levelDb - sThr, sRatio, knee);", "M15"),
    # ---- voicing layer (G = GLUE, L = LA, P = PUMP, V = every voicing)
    ("GLUE colour: no 3rd harmonic", "kGlueC3 = 0.0072f;", "kGlueC3 = 0.0f;", "G1"),
    ("GLUE colour: no 2nd harmonic", "kGlueC2 = 0.0034f,", "kGlueC2 = 0.0f,", "G1"),
    ("GLUE colour does not follow the gain reduction", "const float g = std::clamp (yL / 4.0f, 0.0f, 2.5f);", "const float g = 1.0f;", "G1"),
    ("GLUE colour three times too strong", "kGlueC2 = 0.0034f, kGlueC3 = 0.0072f;", "kGlueC2 = 0.0102f, kGlueC3 = 0.0216f;", "G1"),
    ("GLUE colour follows the signal level, not the gain reduction", "std::max (envC[c], kColourFloor)", "1.0f", "G1"),
    ("GLUE detector is a plain peak (no RMS blend)", "kGlueWPeak = 0.05f, kGlueWRms = 0.10f;", "kGlueWPeak = 1.0f, kGlueWRms = 0.0f;", "G3"),
    ("GLUE detector has only the short window (hits drive it one by one)", "kGlueRmsMs = 5.0f, kGlueSlowRmsMs = 100.0f;", "kGlueRmsMs = 5.0f, kGlueSlowRmsMs = 5.0f;", "G5"),
    ("GLUE gain reduction not limited (slow stage + cap passes 60 dB)", "kMaxGrDb - kGlueCapDb);", "1000.0f);", "V3"),
    ("GLUE fast stage has no cap (a per-hit duck, the v2 shadow)", "kGlueCapDb = 1.5f;", "kGlueCapDb = 20.0f;", "G4"),
    ("GLUE fast stage has no cap (a per-hit duck): continuity", "kGlueCapDb = 1.5f;", "kGlueCapDb = 20.0f;", "G5"),
    ("GLUE slow stage releases in 90 ms (the gain reduction swings back to zero between hits)", "kGlueSlowReleaseS = 1.6f, kGlueSlowReleaseK = 4.0f;", "kGlueSlowReleaseS = 0.09f, kGlueSlowReleaseK = 0.1f;", "G5"),
    ("GLUE slow stage attacks in 10 ms (it chases every hit)", "kGlueSlowAttackS = 0.8f,", "kGlueSlowAttackS = 0.01f,", "G4"),
    ("GLUE passes the burst whole (fast stage cap 0)", "kGlueCapDb = 1.5f;", "kGlueCapDb = 0.001f;", "G2"),
    ("GLUE SC HPF default is off", "(p.voicing == kGlue ? kGlueHpfHz : 0.0f)", "0.0f", "G6"),
    ("GLUE SC HPF default is 60 Hz (the kick still drives it)", "kGlueHpfHz = 200.0f;", "kGlueHpfHz = 60.0f;", "G6"),
    ("GLUE manual release ignores the knob in the slow stage", "const float aSlowRel = p.autoRelease ? aGlueSlowAuto : aGlueSlowKnob;", "const float aSlowRel = aGlueSlowAuto;", "G7"),
    ("LA slow stage has no press memory (always 1 s)", "kLaSlowSpanS = 11.0f;", "kLaSlowSpanS = 0.0f;", "L1"),
    ("LA fast stage ten times slower", "kLaFastMs = 55.0f;", "kLaFastMs = 550.0f;", "L1"),
    ("LA slow stage fills at once", "if (half > laSlow) laSlow += cLaCharge * (half - laSlow);", "if (half > laSlow) laSlow = half;", "L1"),
    ("LA attack is not program dependent", "const float tauA = std::clamp (kLaAttackMs / (1.0f + y1 / 6.0f), 3.0f, kLaAttackMs);", "const float tauA = 10.0f;", "L2"),
    ("LIMIT has the COMPRESS ratio", "kLaLimitRatio = 12.0f,", "kLaLimitRatio = 3.0f,", "L2"),
    ("PEAK REDUCTION does nothing", "lvlDb + kLaPeakRedDb * sPr", "lvlDb + kLaPeakRedDb * 0.5f", "L2"),
    ("LA colour is odd instead of even", "kLaC2 = 0.011f, kLaC3 = 0.0024f;", "kLaC2 = 0.0f, kLaC3 = 0.04f;", "L3"),
    ("LA colour missing", "kLaC2 = 0.011f, kLaC3 = 0.0024f;", "kLaC2 = 0.0f, kLaC3 = 0.0f;", "L3"),
    ("PUMP recovery ignores the tempo", "rNote * 240.0 / rBpm", "rNote * 240.0 / 120.0", "P1"),
    ("PUMP linear shape ends at one note, not 90 % at one note", "sh = std::max (0.0f, 1.0f - 0.9f * pumpP);", "sh = std::max (0.0f, 1.0f - pumpP);", "P1"),
    ("PUMP has only the exponential shape", "pumpShape = std::clamp (p.pumpShape, 0, 2);", "pumpShape = 0;", "P1"),
    ("PUMP depth not limited", "sDepth);\n\n                // Recovery on a clock", "1000.0f);\n\n                // Recovery on a clock", "P2"),
    ("PUMP attack not held to its ceiling", "std::min (sAtt, kPumpAttackMs)", "sAtt", "P2"),
    ("PUMP with EXT off still hears the sidechain", "if (p.ext)\n", "if (p.ext || keyChannels > 0)\n", "P2"),
    ("dry path not delayed (every voicing)", "const float dry = dd;", "const float dry = x[c];", "V1"),
    ("coefficients refreshed per block (voicing coefficients too)", "if ((count++ & 15) == 0)", "if (i == 0)", "V2"),
    ("colour DC tracker not flushed (denormal tail)", "                    if (std::abs (eMean) < 1.0e-20f) eMean = 0.0f;\n                    s += (e - eMean)", "                    s += (e - eMean)", "V3"),
    ("LA costs ten times the CPU", "const float aA = std::exp (-1.0f / (0.001f * tauA * (float) fs));", "float aA = std::exp (-1.0f / (0.001f * tauA * (float) fs)); for (int q = 0; q < 600; ++q) aA += 1.0e-30f * std::exp ((float) q * 0.01f + aA);", "V4"),
    ("voicing switch resets the gain", "pumpP = 0.0f; laMem = 0.0f; voicingPrev = p.voicing;", "pumpP = 0.0f; laMem = 0.0f; voicingPrev = p.voicing; yL = 0.0f;", "V5"),
]

def main():
    only = sys.argv[1:]
    muts = [m for m in MUTATIONS if not only or any(o.lower() in m[0].lower() or o == m[3] for o in only)]
    src = open(ENGINE).read(); missed = 0
    tmp = tempfile.mkdtemp(prefix="tutkal-mut-")
    os.makedirs(os.path.join(tmp, "Source")); os.makedirs(os.path.join(tmp, "tools"))
    shutil.copy(os.path.join(ROOT, "tools", "tutkalraw.cpp"), os.path.join(tmp, "tools"))
    for name, old, new, check in muts:
        if src.count(old) != 1:
            print(f"STALE  {name}: the line to mutate was not found exactly once"); missed += 1; continue
        open(os.path.join(tmp, "Source", "TutkalEngine.h"), "w").write(src.replace(old, new))
        raw = os.path.join(tmp, "tutkalraw")
        b = subprocess.run(["clang++", "-std=c++17", "-O2", "-o", raw, os.path.join(tmp, "tools", "tutkalraw.cpp")], capture_output=True, text=True)
        if b.returncode != 0:
            print(f"BUILD  {name}: {b.stderr.splitlines()[0] if b.stderr else ''}"); missed += 1; continue
        r = subprocess.run([sys.executable, os.path.join(HERE, "measure.py"), check], capture_output=True, text=True, env={**os.environ, "TUTKALRAW": raw})
        line = next((l for l in r.stdout.splitlines() if l.startswith(check + " ")), r.stdout.strip()[-200:] or r.stderr.strip()[-200:])
        caught = r.returncode != 0 and " FAIL " in line
        print(f"{'CAUGHT' if caught else 'MISSED'} {name} -> {line[:170]}")
        if not caught: missed += 1
    print(f"\n{len(muts) - missed}/{len(muts)} faults caught")
    return missed

if __name__ == "__main__":
    sys.exit(main())
