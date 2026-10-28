#!/usr/bin/env python3
"""Gates for the original's graphic-equaliser engine (Source/Ge1100Engine.h via build/geraw), PASS/FAIL one line each.
The reference is the analog state-space model of the traced circuit (tools/ge_statespace.py), which matches ngspice
within 0.014 dB / 0.05 deg (docs/reference/ge1100/VALUES.md).

  measure_ge.py            every gate; exit code 0 (all pass) or 1
  measure_ge.py G2 G6      only those
"""
import os, subprocess, sys, tempfile, struct
import numpy as np
from scipy.signal import welch, csd

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ge_statespace as ref

RAW = os.environ.get("GERAW", os.path.join(HERE, "..", "build", "geraw"))
TMP = tempfile.mkdtemp(prefix="ge-")
fails = 0
rng = np.random.default_rng(11)

def write_wav(path, x, sr):
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

def run(x, sr, *flags, timing=False):
    i, o = os.path.join(TMP, "in.wav"), os.path.join(TMP, "out.wav")
    write_wav(i, x, sr)
    r = subprocess.run([RAW, i, o] + [str(f) for f in flags] + (["--time-it"] if timing else []), capture_output=True, text=True, check=True)
    y = read_wav(o)
    return (y, r.stdout) if timing else y

def gate(name, ok, detail):
    global fails
    print(f"{'PASS' if ok else 'FAIL'} {name}: {detail}")
    if not ok: fails += 1

db = lambda z: 20 * np.log10(np.maximum(np.abs(z), 1e-20))
posarg = lambda p: ["--pos", ",".join(f"{v:.6f}" for v in p)]

def measured(pos, sr, seconds=6):
    """The engine is linear and time-invariant at fixed settings: its impulse response gives H exactly (the 16 Hz band
    rings for about 2 s, so 6 s of response at 1/6 Hz resolution)."""
    x = np.zeros(sr * seconds); x[100] = 0.05
    y = run(x, sr, *posarg(pos))[:, 0]
    H = np.fft.rfft(y) / np.fft.rfft(x); f = np.fft.rfftfreq(len(x), 1 / sr)
    return f, np.abs(H)

def err(pos, sr, lo=20, hi=20000):
    f, H = measured(pos, sr)
    m = (f >= lo) & (f <= hi)
    return np.max(np.abs(db(H[m]) - db(ref.analog_response(pos, f[m]))))

def G1():
    for sr in (44100, 48000, 96000):
        e = err([0.5] * 12, sr)
        gate(f"G1 flat @ {sr}", e <= 0.3, f"max |engine - analog| 20 Hz-20 kHz = {e:.3f} dB (<= 0.3)")

def G2():
    worst, at = 0, ""
    for k in range(12):
        for x in (0.0, 1.0):
            p = [0.5] * 12; p[k] = x
            e = err(p, 48000)
            if e > worst: worst, at = e, f"band {ref.LABELS[k]} Hz at {'N (boost)' if x == 0 else 'MID (cut)'}"
    gate("G2 every band at both ends @ 48000", worst <= 0.35, f"worst {worst:.3f} dB at {at} (<= 0.35)")

def G3():
    sets = [[0.0 if i % 2 else 1.0 for i in range(12)]] + [list(rng.uniform(0, 1, 12)) for _ in range(5)]
    worst = max(err(p, 48000) for p in sets)
    gate("G3 interaction (zigzag + 5 random)", worst <= 0.5, f"worst {worst:.3f} dB (<= 0.5)")

def G4():
    imp = np.zeros(48000 // 2); imp[2000] = 0.5
    y = run(imp, 48000)[:, 0]; meas = int(np.argmax(np.abs(y))) - 2000
    rep = int(subprocess.run([RAW, "--latency"], capture_output=True, text=True).stdout)
    gate("G4 latency", meas == rep, f"measured {meas}, reported {rep} samples")

def G5():
    z = run(np.zeros((48000, 2)), 48000, *posarg([0.0] * 12)); silent = np.max(np.abs(z)) < 1e-9
    t = np.arange(48000) / 48000; sq = 0.25 * np.sign(np.sin(2 * np.pi * 50 * t))
    y = run(np.stack([sq, sq], 1), 48000, *posarg([0.0] * 12))
    finite = bool(np.all(np.isfinite(y))); same = bool(np.array_equal(y[:, 0], y[:, 1]))
    tail = run(np.r_[np.ones(4800) * 0.1, np.zeros(48000 * 3)], 48000, *posarg([0.0] * 12))[-4800:, 0]
    # the slowest pole is the input coupling, C225 6.8 uF into 30 k + 56 k: tau 0.585 s; a 0.1 step leaves at most
    # 0.1 * exp(-3 / 0.585) = 5.9e-4 after 3 s (the analog circuit's own tail)
    decays = np.max(np.abs(tail)) < 0.1 * np.exp(-3 / 0.585)
    gate("G5 safety", silent and finite and same and decays, f"silence {np.max(np.abs(z)):.1e}; square, all bands boost: finite {finite}, L==R {same}; tail 3 s after a step {np.max(np.abs(tail)):.1e} (< 5.9e-4, the circuit's tau)")

def G6():
    x = rng.standard_normal((48000 * 10, 2)) * 0.05
    _, out = run(x, 48000, *posarg(list(rng.uniform(0, 1, 12))), timing=True)
    rt = float(out.split("realtime_x=")[1])
    gate("G6 speed", rt > 30, f"stereo 48 kHz: {rt:.0f}x real time (> 30)")

def taper():
    path = os.path.join(HERE, "..", "Source", "Ge1100Taper.h")
    import re
    rows = [l for l in open(path) if l.strip().startswith("{ 0.") or l.strip().startswith("{ 1.")]
    return [[float(v) for v in re.findall(r"[-+]?\d+\.\d+", r)] for r in rows[:12]]

def G7():
    """Panel marks: the table is monotonic, 0 dB is the centre tap, and the engine's gain at each band's centre for marks
    +-12 / +-6 is the mark scaled to that end's gain (Ge1100Taper.h rule), measured on the engine."""
    T = taper(); mono = all(all(r[k] <= r[k - 1] for k in range(1, len(r))) for r in T) and all(abs(r[24] - 0.5) < 1e-12 for r in T)
    worst, at = 0.0, ""
    f0, Hflat = measured([0.5] * 12, 48000)
    for b in (0, 3, 6, 9, 10):
        for mark in (-12, -6, 6, 12):
            marks = [0.0] * 12; marks[b] = mark
            x = np.zeros(48000 * 6); x[100] = 0.05
            y = run(x, 48000, "--marks", ",".join(str(v) for v in marks))[:, 0]
            H = np.fft.rfft(y) / np.fft.rfft(x)
            k = int(np.argmin(np.abs(f0 - ref.LABELS[b]))) if ref.LABELS[b] < 24000 else None
            if k is None: continue
            got = db(H[k]) - db(Hflat[k])
            pos_end = 0.0 if mark > 0 else 1.0
            p = [0.5] * 12; p[b] = pos_end
            end = db(ref.analog_response(p, [ref.LABELS[b]])[0]) - db(ref.analog_response([0.5] * 12, [ref.LABELS[b]])[0])
            want = abs(mark) / 12 * end
            e = abs(got - want)
            if e > worst: worst, at = e, f"band {ref.LABELS[b]} Hz mark {mark:+d}: engine {got:+.2f}, rule {want:+.2f}"
    gate("G7 panel marks", mono and worst <= 0.3, f"table monotonic and 0 dB = centre tap: {mono}; worst {worst:.3f} dB ({at}) (<= 0.3)")

def G8():
    """A slider dragged from -12 to +12 over 2 s while a sine plays: no clicks or zipper noise (everything outside the sine's
    own neighbourhood stays below -70 dBc), at 1 kHz and at 32 Hz (where the circuit's state matters most)."""
    worst, at = -999.0, ""
    for band, f in ((6, 1000.0), (1, 50.0)):
        t = np.arange(48000 * 2) / 48000; x = 0.25 * np.sin(2 * np.pi * f * t)
        y = run(x, 48000, "--sweep", band, -12, 12)[:, 0][4800:-4800]
        w = np.hanning(len(y)); Y = np.abs(np.fft.rfft(y * w)); fr = np.fft.rfftfreq(len(y), 1 / 48000)
        guard = max(300.0, 3 * f)
        fund = Y[np.abs(fr - f) < 20].max()
        out = Y[(np.abs(fr - f) > guard) & (fr > 20)].max()
        r = db(out / fund)
        if r > worst: worst, at = r, f"band {ref.LABELS[band]} Hz, {f:.0f} Hz sine"
    gate("G8 slider drag", worst <= -70, f"worst energy away from the sine {worst:.1f} dBc ({at}) (<= -70)")

V_PER_UNIT = 2.0 * np.sqrt(2)   # Ge1100Engine.h kVoltsPerUnit: a full-scale sine is 2 V rms

def thd_and_peak(peak_units, f0=1000.0):
    t = np.arange(48000 * 2) / 48000
    y = run(peak_units * np.sin(2 * np.pi * f0 * t), 48000)[:, 0][24000:]
    w = np.blackman(len(y)); Y = np.abs(np.fft.rfft(y * w)); k0 = int(round(f0 * len(y) / 48000))
    a = lambda k: Y[max(0, k - 3):k + 4].max()
    h = [a(k * k0) for k in range(2, 12) if k * f0 < 23500]
    return 100 * np.sqrt(sum(v * v for v in h)) / a(k0), np.max(np.abs(y)) * V_PER_UNIT, np.sqrt(np.mean(y * y)) * V_PER_UNIT

def G9():
    """The owner's manual: maximum output 9 V rms at 1 kHz with 0.01 % THD. Below it the engine is linear; above it the
    output op-amp clips and never passes 13.3 V peak."""
    thd9, pk9, rms9 = thd_and_peak(9.0 * np.sqrt(2) / V_PER_UNIT / 10 ** (-0.18 / 20))   # flat gain is -0.18 dB
    thd_hot, pk_hot, _ = thd_and_peak(17.0 / V_PER_UNIT)
    # The clip is exact (13.3 V) at 4x; band-limiting the clipped wave in the decimator overshoots its corners slightly
    # (Gibbs, 0.15 % measured 26 Oct). Since 26 Oct the rails carry ADAA on their residual (aliasing -53 -> -78.5 dBc,
    # measure_bus.py B4), whose half-sample lag adds to it: 1.3 % measured. The analog unit has no overshoot; 2 % allowed.
    ok = thd9 < 0.01 and pk_hot <= 13.3 * 1.02 and thd_hot > 1.0
    gate("G9 output swing", ok, f"at {rms9:.2f} V rms out THD {thd9:.4f} % (< 0.01); driven to 17 V peak: output peak {pk_hot:.2f} V (<= 13.3 + 2 %: decimator and ADAA overshoot), THD {thd_hot:.1f} % (> 1, it clips)")

def G10():
    """ATT -6 dB: the engine's level change equals the ngspice run of S11 (eq_att.cir vs eq_flat.cir: -5.648 dB, flat)."""
    sr = 48000; x = np.zeros(sr * 2); x[100] = 0.05
    pos = [0.5] * 12
    a = run(x, sr, *posarg(pos))[:, 0]; b = run(x, sr, *posarg(pos), "--att")[:, 0]
    f = np.fft.rfftfreq(len(x), 1 / sr); m = (f >= 20) & (f <= 20000)
    diff = db(np.fft.rfft(b)[m]) - db(np.fft.rfft(a)[m])
    gate("G10 ATT", np.max(np.abs(diff + 5.648)) < 0.01, f"ATT changes the level by {diff.min():+.3f} ... {diff.max():+.3f} dB over 20 Hz-20 kHz (ngspice -5.648, within 0.01)")

def G11():
    """NOR/REV: with REV the engine plays exactly the curve of the mirrored pot positions (1 - p), and that curve is the
    analog model's for those positions."""
    sr = 48000; x = rng.standard_normal(sr) * 0.05
    pos = [0.1, 0.2, 0.35, 0.5, 0.65, 0.8, 0.9, 0.4, 0.6, 0.3, 0.7, 0.55]
    a = run(x, sr, *posarg(pos), "--rev")[:, 0]; b = run(x, sr, *posarg([1 - p for p in pos]))[:, 0]
    e = np.max(np.abs(a - b)); e2 = err([1 - p for p in pos], sr)
    gate("G11 NOR/REV", e == 0 and e2 < 0.3, f"REV vs mirrored positions: max difference {e:.1e} (bit-exact); mirrored curve vs analog model {e2:.3f} dB (< 0.3)")

GATES = dict(G1=G1, G2=G2, G3=G3, G4=G4, G5=G5, G6=G6, G7=G7, G8=G8, G9=G9, G10=G10, G11=G11)
if __name__ == "__main__":
    for g in (sys.argv[1:] or list(GATES)): GATES[g]()
    print("ALL PASS" if fails == 0 else f"{fails} FAIL")
    sys.exit(1 if fails else 0)
