#!/usr/bin/env python3
"""Gates for the original's spectrum analyser (Source/Ge1100Analyser.h via build/flraw), PASS/FAIL per line.
The references are the schematic's values (page 10): the band-pass filters, the threshold ladder and the hold / scan
timing, computed here independently of the C++.

  measure_fl.py            every gate; exit code 0 (all pass) or 1
  measure_fl.py F1 F5      only those
"""
import math, os, re, subprocess, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
RAW = os.environ.get("FLRAW", os.path.join(HERE, "..", "build", "flraw"))
fails = 0
BANDS = [(16, 62e3, 1.1e3, 0.47e-6, 390e3), (32, 47e3, 750, 0.33e-6, 300e3), (64, 36e3, 560, 0.22e-6, 220e3),
         (125, 47e3, 820, 0.082e-6, 300e3), (250, 51e3, 820, 0.039e-6, 330e3), (500, 43e3, 750, 0.022e-6, 270e3),
         (1000, 56e3, 1e3, 8.2e-9, 360e3), (2000, 62e3, 1.2e3, 3.3e-9, 470e3), (4000, 36e3, 750, 2.7e-9, 270e3),
         (8000, 36e3, 820, 1.2e-9, 330e3), (16000, 30e3, 750, 680e-12, 270e3), (32000, 30e3, 1.2e3, 220e-12, 390e3)]
f0 = lambda b: math.sqrt((b[1] + b[2]) / (b[1] * b[2] * b[4])) / (2 * math.pi * b[3])

def gate(name, ok, detail):
    global fails
    print(f"{'PASS' if ok else 'FAIL'} {name}: {detail}")
    if not ok: fails += 1

def run(*args):
    return subprocess.run([RAW] + [str(a) for a in args], capture_output=True, text=True, check=True).stdout

def buses(out):
    return [float(m) for m in re.findall(r"bus ([\d.]+) V", out)]

def segs(out):
    return [int(m) for m in re.findall(r"segments (\d+)", out)]

def tone(sr, hz, dbfs, secs=0.6, level=0.2):
    return run("--sr", sr, "--level", level, "--tone", hz, dbfs, secs)

def F1():
    """Each band's peak lies at the circuit's centre frequency (21 tones over +-15 %, the best within 3 %)."""
    worst = 0; rows = []
    for i, b in enumerate(BANDS):
        fc = f0(b); sr = 96000 if fc > 20000 else 48000
        secs = max(0.6, 40 / fc * 10)           # Q 10: let the filter settle
        fr = fc * np.linspace(0.85, 1.15, 21)
        v = [buses(tone(sr, f, -40, secs))[i] for f in fr]
        fpk = fr[int(np.argmax(v))]; err = abs(fpk / fc - 1); worst = max(worst, err)
        rows.append(f"{b[0]}:{fpk:.0f}/{fc:.0f}")
    gate("F1 centres", worst <= 0.03, f"worst {100 * worst:.1f} % (<= 3 %); peak/circuit Hz " + " ".join(rows))

def F2():
    """Selectivity: a tone one octave away reads under a fifth of the centre's bus voltage (Q about 10). Steady state:
    the hold keeps the onset's ring for a while (tau 0.26 s), so the tones run 3 s."""
    worst = 0
    for i, b in enumerate(BANDS[1:11], start=1):
        fc = f0(b); secs = 3.0
        sr = 96000 if fc * 2 > 0.45 * 48000 else 48000   # an octave above 16 kHz does not exist at 48 kHz (it would fold)
        on = buses(tone(sr, fc, -30, secs))[i]
        off = max(buses(tone(sr, fc * 2, -30, secs))[i], buses(tone(sr, fc / 2, -30, secs))[i])
        worst = max(worst, off / on)
    gate("F2 selectivity", worst <= 0.2, f"worst octave-off / centre bus ratio {worst:.3f} (<= 0.2), bands 32 Hz ... 16 kHz")

def F3():
    R = [10e3, 12e3, 18e3, 11e3, 9.1e3, 7.5e3, 3.9e3, 3e3, 2.7e3, 430]
    want = [13 * sum(R[k:]) / sum(R) for k in range(10)][::-1]
    got = [float(x) for x in re.findall(r": ([\d.]+) V", run("--taps"))]
    printed = [0.08, 0.53, 1.0, 1.7, 3.0, 4.5, 6.3, 9.3, 11.3, 13.0]   # the schematic's own DC labels
    err = max(abs(g - w) for g, w in zip(got, want)); perr = max(abs(g - p) / p for g, p in zip(got, printed))
    gate("F3 ladder", err < 1e-3 and perr < 0.12, f"taps {', '.join(f'{g:.2f}' for g in got)} V; vs resistors {err:.1e} V, vs printed labels worst {100 * perr:.0f} %")

def F4():
    """Scale: for each segment from -18 to 0 dB, the 1 kHz level where it first lights, relative to the 0 dB segment's,
    lies on its panel mark within 0.5 dB (the ladder against a linear detector minus its drops: residuals up to 0.35 dB)."""
    def threshold(seg):
        lo, hi = -90.0, 0.0
        for _ in range(26):
            mid = (lo + hi) / 2
            if segs(tone(48000, 1032, mid))[6] >= seg: hi = mid
            else: lo = mid
        return hi
    zero = threshold(8)
    marks = {2: -18, 3: -15, 4: -12, 5: -9, 6: -6, 7: -3}
    rel = {k: threshold(k) - zero for k in marks}
    worst = max(abs(rel[k] - m) for k, m in marks.items())
    gate("F4 scale", worst <= 0.5, f"0 dB segment at {zero:.2f} dBFS (LEVEL 0.2); " + ", ".join(f"{m}: {rel[k]:+.2f}" for k, m in marks.items()) + f" dB; worst {worst:.2f} dB off its mark (<= 0.5)")

def F5():
    out = run("--sr", 48000, "--level", 0.2, "--tone", 1032, -30, 0.5, "--then-silence", 1.5, "--trace", 6)
    t, v = np.array([[float(x) for x in l.split()[:2]] for l in out.strip().splitlines()]).T
    m = (t > 0.55) & (v > 0.05)
    tau = -1 / np.polyfit(t[m], np.log(v[m]), 1)[0]
    want = kSlotTau = 1.6e3 * 10e-6 * 16 * 0.97e-3 / 0.97e-3   # 16 ms per read, one read per 16 slots
    gate("F5 release", abs(tau - want) / want < 0.1, f"bus decays with tau {1000 * tau:.0f} ms (circuit: 1.6 k x 10 uF per read, 1 read in 16 slots -> {1000 * want:.0f} ms, +-10 %)")

def F6():
    out = run("--sr", 48000, "--level", 0.2, "--tone", 1032, -30, 0.5, "--trace", 6)
    t, v = np.array([[float(x) for x in l.split()[:2]] for l in out.strip().splitlines()]).T
    t90 = t[np.argmax(v >= 0.9 * v[-1])]
    gate("F6 attack", t90 <= 0.06, f"1 kHz band reaches 90 % of its reading in {1000 * t90:.0f} ms (<= 60: the Q 10 filter's rise plus a scan)")

def F7():
    rng = np.random.default_rng(5)
    out = run("--sr", 48000, "--tone", 440, -20, 20, "--time-it")
    rt = float(out.split("realtime_x=")[1])
    gate("F7 speed", rt > 50, f"48 kHz (2x inside): {rt:.0f}x real time (> 50)")

def F8():
    """TOTAL: unfiltered, before LEVEL. LEVEL does not move it; 0 dB is at 110 mV per channel (computed from IC1 2/2's
    gain 32.4 and the detector law), the manual's 140 mV lights +3."""
    def tot(mv, level):
        out = run("--sr", 48000, "--level", level, "--tone", 1000, 20 * math.log10(mv / 1000 / 2), 1)
        return float(re.search(r"TOTAL: bus ([\d.]+) V, segments (\d+)", out).group(1)), int(re.search(r"TOTAL: bus [\d.]+ V, segments (\d+)", out).group(1))
    a, b = tot(110, 0.2), tot(110, 1.0); c = tot(140, 0.2); z = tot(112, 0.2); u = tot(108, 0.2)
    ok = a == b and u[1] == 7 and z[1] == 8 and c[1] == 9
    gate("F8 TOTAL", ok, f"110 mV: bus {a[0]:.2f} V at LEVEL 0.2 and {b[0]:.2f} V at 1.0; 108 / 112 mV light {u[1]} / {z[1]} (0 dB between); 140 mV lights {c[1]} (+3)")

GATES = dict(F1=F1, F2=F2, F3=F3, F4=F4, F5=F5, F6=F6, F7=F7, F8=F8)
if __name__ == "__main__":
    for g in (sys.argv[1:] or list(GATES)): GATES[g]()
    print("ALL PASS" if fails == 0 else f"{fails} FAIL")
    sys.exit(1 if fails else 0)
