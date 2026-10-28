#!/usr/bin/env python3
"""Programme EQ engine measurements: SPEC.md section 2b, gates E1-E9, PASS/FAIL one line each, ALL PASS at the end.
Runs build/eqraw, the engine as the plug-in will run it. Every test signal is generated here.

  measure.py            every gate; exit code 0 (all pass) or 1
  measure.py E2 E6      only those
  EQRAW=/path/to/eqraw measure.py
"""
import os, subprocess, sys, tempfile, struct
import numpy as np
from scipy.signal import welch, csd, freqz

HERE = os.path.dirname(os.path.abspath(__file__))
RAW = os.environ.get("EQRAW", os.path.join(HERE, "..", "build", "eqraw"))
TMP = tempfile.mkdtemp(prefix="peq-")
SR = 48000
LOWS, PRES, HIGHS = [40, 60, 100, 150], [700, 1400, 2800, 4000, 5600], [5000, 8000, 12000]
fails = 0
rng = np.random.default_rng(7)

def write_wav(path, x, sr=SR):
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
    args = [RAW, i, o] + [str(f) for f in flags] + (["--time-it"] if timing else [])
    r = subprocess.run(args, capture_output=True, text=True, check=True)
    y = read_wav(o)
    return (y, r.stdout) if timing else y

def gate(name, ok, detail):
    global fails
    print(f"{'PASS' if ok else 'FAIL'} {name}: {detail}")
    if not ok: fails += 1

def db(x): return 20 * np.log10(np.maximum(np.abs(x), 1e-20))

# ------------------------------------------------------------------------------------------- reference (the prototype)
def rbj(kind, f, q, g, fs=SR):
    A = 10 ** (g / 40); w = 2 * np.pi * f / fs; c = np.cos(w); al = np.sin(w) / (2 * q); s = 2 * np.sqrt(A) * al
    if kind == "low":
        b = [A * ((A + 1) - (A - 1) * c + s), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - s)]
        a = [(A + 1) + (A - 1) * c + s, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - s]
    elif kind == "high":
        b = [A * ((A + 1) + (A - 1) * c + s), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - s)]
        a = [(A + 1) - (A - 1) * c + s, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - s]
    else:
        b = [1 + al * A, -2 * c, 1 - al * A]; a = [1 + al / A, -2 * c, 1 - al / A]
    return np.array(b) / a[0], np.array(a) / a[0]

def proto(freqs, lowfreq=60, lowboost=0, lowcut=0, presfreq=2800, pres=0, highfreq=8000, high=0):
    H = np.ones_like(freqs, dtype=complex)
    for kind, f, q, g in [("low", lowfreq, 0.8, lowboost), ("low", lowfreq * 2.5, 0.5, -lowcut),
                          ("peak", presfreq, 0.55 + 0.02 * abs(pres), pres), ("high", highfreq, 0.5, high)]:
        b, a = rbj(kind, f, q, g); H *= freqz(b, a, worN=freqs, fs=SR)[1]
    return H

def response(flags, seconds=6, level_db=-30):
    x = rng.standard_normal(SR * seconds) * 10 ** (level_db / 20) / 1.0
    y = run(x, *flags)[:, 0]
    f, pxx = welch(x, SR, nperseg=1 << 15); _, pxy = csd(x, y, SR, nperseg=1 << 15)
    return f, np.abs(pxy / pxx)

def band(f, lo, hi): return (f >= lo) & (f <= hi)

def harmonics(f0, level_db, *flags, n=7):
    t = np.arange(SR * 3) / SR
    x = np.sin(2 * np.pi * f0 * t) * 10 ** (level_db / 20)
    y = run(x, *flags)[SR:, 0]
    w = np.blackman(len(y)); Y = np.abs(np.fft.rfft(y * w)); fr = np.fft.rfftfreq(len(y), 1 / SR)
    def amp(fq):
        k = int(round(fq * len(y) / SR)); return Y[max(0, k - 3):k + 4].max()
    a1 = amp(f0)
    hs = {k: db(amp(k * f0) / a1) for k in range(2, n + 1) if k * f0 < SR / 2 - 200}
    return a1, hs, fr, Y

# ------------------------------------------------------------------------------------------- gates
def E1():
    f, H = response([])
    m = band(f, 20, 20000); dev = np.max(np.abs(db(H[m])))
    gate("E1 flat", dev <= 0.1, f"max |dB| 20 Hz-20 kHz = {dev:.3f} (gate 0.1)")

def E2():
    worst = (0, "")
    cases = []
    for lf in LOWS:
        cases += [dict(lowfreq=lf, lowboost=12), dict(lowfreq=lf, lowcut=8)]
    for pf in PRES:
        cases += [dict(presfreq=pf, pres=8), dict(presfreq=pf, pres=-8)]
    for hf in HIGHS:
        cases += [dict(highfreq=hf, high=10), dict(highfreq=hf, high=-10)]
    for c in cases:
        flags = sum([[f"--{k}", v] for k, v in c.items()], [])
        f, H = response(flags)
        m = band(f, 20, 18000)
        err = np.max(np.abs(db(H[m]) - db(proto(f[m], **c))))
        if err > worst[0]: worst = (err, str(c))
    gate("E2 curves", worst[0] <= 0.3, f"{len(cases)} positions, worst error {worst[0]:.3f} dB at {worst[1]} (gate 0.3)")

def E3():
    f, H = response(["--lowfreq", 60, "--lowboost", 10, "--lowcut", 6])
    lift = np.max(db(H[band(f, 20, 50)])); dip = np.min(db(H[band(f, 100, 250)]))
    gate("E3 passive trick", lift >= 3 and dip <= -1.5, f"lift below 50 Hz {lift:+.2f} dB (>= +3), dip 100-250 Hz {dip:+.2f} dB (<= -1.5)")

def E4():
    _, h12, _, _ = harmonics(1000, -12, "--drive", 12)
    a1, h0, fr, Y = harmonics(1000, -18, "--drive", 0)
    thd = 100 * np.sqrt(sum(10 ** (v / 10) for v in h0.values()))
    gate("E4 amplifier", h12[2] > h12[3] and thd < 0.1, f"DRIVE +12, 1 kHz -12 dBFS: H2 {h12[2]:.1f} dBc, H3 {h12[3]:.1f} dBc; DRIVE 0, -18 dBFS THD {thd:.4f} % (< 0.1)")

def E5():
    _, lo, _, _ = harmonics(40, -12, "--drive", 12)
    _, hi, _, _ = harmonics(1000, -12, "--drive", 12)
    gate("E5 transformer", lo[3] - hi[3] >= 10, f"DRIVE +12, -12 dBFS: H3 at 40 Hz {lo[3]:.1f} dBc, at 1 kHz {hi[3]:.1f} dBc, difference {lo[3] - hi[3]:.1f} dB (>= 10)")

def E6():
    f0 = 15000
    a1, _, fr, Y = harmonics(f0, -6, "--drive", 18)
    mask = np.ones_like(fr, dtype=bool)
    for k in range(1, 4):
        mask &= np.abs(fr - k * f0) > 60
    mask &= fr > 30
    worst = db(Y[mask].max() / a1); at = fr[mask][np.argmax(Y[mask])]
    gate("E6 aliasing", worst <= -70, f"15 kHz -6 dBFS DRIVE +18: worst non-harmonic {worst:.1f} dBc at {at:.0f} Hz (<= -70)")

def pink(n):
    X = np.fft.rfft(rng.standard_normal(n)); f = np.fft.rfftfreq(n, 1 / SR); X[1:] /= np.sqrt(f[1:]); X[0] = 0
    x = np.fft.irfft(X, n); return x / np.sqrt(np.mean(x ** 2))

def E7():
    x = pink(SR * 6) * 10 ** (-18 / 20)
    r0 = np.sqrt(np.mean(run(x, "--drive", 0)[SR:, 0] ** 2)); r18 = np.sqrt(np.mean(run(x, "--drive", 18)[SR:, 0] ** 2))
    d = db(r18 / r0)
    gate("E7 level", abs(d) < 1.5, f"pink -18 dBFS RMS, DRIVE 0 -> +18: {d:+.2f} dB (|.| < 1.5)")

def E8():
    z = run(np.zeros((SR, 2)), "--drive", 18, "--lowboost", 12)
    silent = np.max(np.abs(z)) < 1e-7
    t = np.arange(SR) / SR; sq = np.sign(np.sin(2 * np.pi * 100 * t))
    y = run(np.stack([sq, sq], 1), "--drive", 18, "--lowboost", 12, "--high", 10)
    finite = np.all(np.isfinite(y)); same = np.array_equal(y[:, 0], y[:, 1])
    imp = np.zeros(SR // 4); imp[1000] = 0.1
    yi = run(imp)[:, 0]; lat_meas = int(np.argmax(np.abs(yi))) - 1000
    lat_rep = int(subprocess.run([RAW, "--latency"], capture_output=True, text=True).stdout.strip())
    yb = run(imp, "--bypass")[:, 0]; lat_byp = int(np.argmax(np.abs(yb))) - 1000
    ok = silent and finite and same and lat_meas == lat_rep == lat_byp
    gate("E8 safety", ok, f"silence max {np.max(np.abs(z)):.1e}; square DRIVE +18 finite {finite}, peak {np.max(np.abs(y)):.2f}; L==R {same}; latency measured {lat_meas}, bypass {lat_byp}, reported {lat_rep}")

def E9():
    x = rng.standard_normal((SR * 20, 2)) * 0.1
    _, out = run(x, "--drive", 12, "--lowboost", 6, "--pres", 3, "--high", 2, timing=True)
    rt = float(out.split("realtime_x=")[1])
    gate("E9 speed", rt > 100, f"stereo 48 kHz: {rt:.0f}x real time (> 100)")

GATES = dict(E1=E1, E2=E2, E3=E3, E4=E4, E5=E5, E6=E6, E7=E7, E8=E8, E9=E9)
if __name__ == "__main__":
    pick = sys.argv[1:] or list(GATES)
    for g in pick: GATES[g]()
    print("ALL PASS" if fails == 0 else f"{fails} FAIL")
    sys.exit(1 if fails else 0)
