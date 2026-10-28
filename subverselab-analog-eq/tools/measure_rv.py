#!/usr/bin/env python3
"""Gates for the original's reverberation section (Source/Ge1100Reverb.h via build/rvraw), SPEC.md 2d, PASS/FAIL per line.
The references are the owner's manual (delay 30-100 ms, reverberation time 0-3 s) and the schematic's filter values.

  measure_rv.py            every gate; exit code 0 (all pass) or 1
  measure_rv.py R1 R3      only those
"""
import os, subprocess, sys, tempfile, struct
import numpy as np
from scipy.signal import hilbert

HERE = os.path.dirname(os.path.abspath(__file__))
RAW = os.environ.get("RVRAW", os.path.join(HERE, "..", "build", "rvraw"))
TMP = tempfile.mkdtemp(prefix="rv-")
SR = 48000
fails = 0

def write_wav(path, x, sr=None):
    sr = sr or SR   # read SR at call time (a default bound at definition time wrote 48 kHz headers for other rates, 26 Oct)
    x = np.asarray(x, dtype=np.float32)
    if x.ndim == 1: x = x[:, None]
    ch = x.shape[1]; data = x.astype("<f4").tobytes()
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 3, ch, sr, sr * 4 * ch, 4 * ch, 32))
        f.write(b"data" + struct.pack("<I", len(data)) + data)

def read_wav(path):
    with open(path, "rb") as f: d = f.read()
    pos = 12; ch = 1; data = None
    while pos + 8 <= len(d):
        cid = d[pos:pos + 4]; n = struct.unpack("<I", d[pos + 4:pos + 8])[0]
        if cid == b"fmt ": ch = struct.unpack("<H", d[pos + 10:pos + 12])[0]
        if cid == b"data": data = np.frombuffer(d[pos + 8:pos + 8 + n], dtype="<f4")
        pos += 8 + n + (n & 1)
    return data.reshape(-1, ch)

def run(x, *flags, timing=False):
    i, o = os.path.join(TMP, "in.wav"), os.path.join(TMP, "out.wav")
    write_wav(i, x)
    r = subprocess.run([RAW, i, o] + [str(f) for f in flags] + (["--time-it"] if timing else []), capture_output=True, text=True, check=True)
    y = read_wav(o)
    return (y, r.stdout) if timing else y

def gate(name, ok, detail):
    global fails
    print(f"{'PASS' if ok else 'FAIL'} {name}: {detail}")
    if not ok: fails += 1

db = lambda v: 20 * np.log10(np.maximum(np.abs(v), 1e-20))
IMP_AT = 1000

def wet_impulse(delay, depth, seconds):
    x = np.zeros(int(SR * seconds)); x[IMP_AT] = 0.02          # small: the limiter stays linear
    y = run(x, "--delay", delay, "--depth", depth)[:, 0]
    return y - x                                                 # left channel wet only (dry is unity)

def R1():
    res = []
    for knob, want in ((0, 0.030), (5, 512 / (512 / 0.030 + (512 / 0.100 - 512 / 0.030) * 0.5)), (10, 0.100)):
        w = wet_impulse(knob, 3, want * 2.2)
        env = np.abs(hilbert(w))
        lo, hi = int(IMP_AT + want * 0.5 * SR), int(IMP_AT + want * 1.5 * SR)
        t = (lo + np.argmax(env[lo:hi]) - IMP_AT) / SR
        res.append((knob, want, t))
    ok = all(abs(t - want) <= 0.02 * want for _, want, t in res)
    gate("R1 delay", ok, "; ".join(f"DELAY {k}: first repeat at {1000 * t:.2f} ms ({'VCO law' if k == 5 else 'manual'} {1000 * w:.1f} ms, ±2 %)" for k, w, t in res))

def R2():
    w = wet_impulse(5, 3, 0.2)
    d = 0.065; seg = w[int(IMP_AT + d * 0.5 * SR):int(IMP_AT + d * 1.5 * SR)]
    seg = seg * np.hanning(len(seg)); S = np.abs(np.fft.rfft(seg, 1 << 16)); f = np.fft.rfftfreq(1 << 16, 1 / SR)
    m = (f > 100) & (f < 10000); fpk = f[m][np.argmax(S[m])]; pk = S[m].max()
    at6k = S[np.argmin(np.abs(f - 6000))]
    ok = 1500 <= fpk <= 2500 and db(at6k / pk) <= -20
    gate("R2 tone", ok, f"first repeat peaks at {fpk:.0f} Hz (1.5-2.5 kHz: the Q 2.1 filter), 6 kHz is {db(at6k / pk):.1f} dB under the peak (<= -20)")

def R3():
    """Reverberation time is an energy decay: the energy of each 100 ms repeat window (the peaky filter spreads every pass
    a little in time, so the peak falls faster than the energy and would understate it)."""
    w = wet_impulse(10, 10, 4.5)
    pts = []
    for k in range(1, 44):
        a = IMP_AT + int((k - 0.5) * 0.100 * SR); b = a + int(0.100 * SR)
        if b >= len(w): break
        pts.append((k * 0.100, 10 * np.log10(np.sum(w[a:b] ** 2) + 1e-30)))
    t = np.array([p[0] for p in pts]); lv = np.array([p[1] for p in pts])
    use = (lv > lv[0] - 50) & (t >= 0.2)                         # skip the first repeat's build-up; stay above float noise
    slope = np.polyfit(t[use], lv[use], 1)[0]
    rt60 = -60 / slope
    gate("R3 decay", abs(rt60 - 3.0) <= 0.3, f"DELAY 10, DEPTH 10: {slope:.1f} dB/s (energy per repeat) -> RT60 {rt60:.2f} s (manual max 3 s, ±10 %)")

def R4():
    x = np.random.default_rng(3).standard_normal(SR) * 0.2
    y = run(x, "--delay", 5, "--depth", 0)
    e = max(np.max(np.abs(y[:, 0] - x)), np.max(np.abs(y[:, 1] - x)))
    gate("R4 DEPTH 0", e < 1e-5, f"max |out - dry| = {e:.1e} ({db(e / 0.2):.0f} dB under the signal; float output)")

def R5():
    t = np.arange(SR * 3) / SR; sq = np.sign(np.sin(2 * np.pi * 110 * t))
    y = run(sq, "--delay", 10, "--depth", 10)
    finite = bool(np.all(np.isfinite(y))); pk = np.max(np.abs(y))
    z = run(np.zeros(SR), "--delay", 10, "--depth", 10); sil = np.max(np.abs(z))
    gate("R5 safety", finite and pk < 4 and sil == 0, f"full-scale square, DEPTH 10: finite {finite}, peak {pk:.2f} (< 4, the limiter holds the loop); silence -> {sil:.1e}")

def R6():
    x = np.random.default_rng(4).standard_normal((SR * 10, 2)) * 0.1
    _, out = run(x, "--delay", 7, "--depth", 6, timing=True)
    rt = float(out.split("realtime_x=")[1])
    gate("R6 speed", rt > 50, f"stereo 48 kHz: {rt:.0f}x real time (> 50)")

GATES = dict(R1=R1, R2=R2, R3=R3, R4=R4, R5=R5, R6=R6)
if __name__ == "__main__":
    for g in (sys.argv[1:] or list(GATES)): GATES[g]()
    print("ALL PASS" if fails == 0 else f"{fails} FAIL")
    sys.exit(1 if fails else 0)
