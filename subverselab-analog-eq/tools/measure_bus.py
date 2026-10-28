#!/usr/bin/env python3
"""Mix-bus / master-bus gates for TORNA's EQ engine (Source/Ge1100Engine.h via build/geraw), PASS/FAIL one line each.
Owner 26 Oct: no listening sessions; TORNA is a mix and master bus EQ, so it is tested for what a bus needs: a clean
EQ OUT path, no clicks when a switch is pressed, headroom, aliasing when it is driven, every sample rate a host offers,
CPU at high rates, no CPU rise in silence, DC. The circuit's own behaviour (flat insertion loss, the rails) is kept and
reported, not corrected. SPEC §6.

  measure_bus.py            every gate; exit code 0 (all pass) or 1
  measure_bus.py B1 B6      only those
"""
import os, subprocess, sys, tempfile, struct
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ge_statespace as ref

RAW = os.environ.get("GERAW", os.path.join(HERE, "..", "build", "geraw"))
TMP = tempfile.mkdtemp(prefix="bus-")
LAT = 64
fails = 0
SMILE = [3, 4, 3, 1, -1, -2, -2, -1, 1, 3, 4, 3]

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

def run(x, sr, *flags):
    i, o = os.path.join(TMP, "in.wav"), os.path.join(TMP, "out.wav")
    write_wav(i, x, sr)
    r = subprocess.run([RAW, i, o] + [str(f) for f in flags], capture_output=True, text=True, check=True)
    return read_wav(o), r.stdout

def marks(m): return ["--marks", ",".join(str(v) for v in m)]
def gate(name, ok, detail):
    global fails
    print(f"{'PASS' if ok else 'FAIL'} {name}: {detail}")
    if not ok: fails += 1
db = lambda v: 20 * np.log10(np.maximum(np.abs(v), 1e-20))
def sine(sr, f, dbfs, secs): t = np.arange(int(sr * secs)) / sr; return 10 ** (dbfs / 20) * np.sin(2 * np.pi * f * t)

def thd(y, sr, f0):
    """harmonic distortion of a steady sine: harmonics 2..10 below Nyquist against the fundamental (Blackman-Harris)"""
    n = len(y); w = np.blackman(n); Y = np.abs(np.fft.rfft(y * w)); fr = np.fft.rfftfreq(n, 1 / sr)
    pk = lambda f: Y[max(0, np.argmin(np.abs(fr - f)) - 3): np.argmin(np.abs(fr - f)) + 4].max()
    h = [pk(k * f0) for k in range(2, 11) if k * f0 < sr / 2 * 0.99]
    return np.sqrt(sum(v * v for v in h)) / pk(f0)

def B1():
    """EQ OUT is transparent: the path is the oversampler alone; magnitude 20 Hz-20 kHz within 0.01 dB at every rate."""
    rows, worst = [], 0
    for sr in (44100, 48000, 96000):
        x = np.zeros(sr); x[1000] = 0.5
        y, _ = run(x, sr, "--bypass")
        H = np.fft.rfft(y[:, 0]) / np.fft.rfft(x); f = np.fft.rfftfreq(len(x), 1 / sr); m = (f >= 20) & (f <= 20000)
        e = np.max(np.abs(db(H[m]))); worst = max(worst, e); rows.append(f"{sr}: {e:.4f} dB")
    gate("B1 EQ OUT transparent", worst <= 0.01, "max |gain| 20 Hz-20 kHz " + ", ".join(rows) + " (<= 0.01)")

def B2():
    """At flat the circuit has its own small insertion loss and ripple (the original's, kept); report it and gate the shape."""
    sr = 48000; f = np.geomspace(20, 20000, 400)
    H = db(ref.analog_response([0.5] * 12, f)); at1k = db(ref.analog_response([0.5] * 12, [1000.0]))[0]
    shape = H.max() - H.min()
    gate("B2 flat insertion", shape <= 0.2, f"at 1 kHz {at1k:+.3f} dB (the original's insertion loss; OUTPUT trims it), ripple 20 Hz-20 kHz {shape:.3f} dB peak-to-peak (<= 0.2)")

def B3():
    """Headroom: 0 dBFS sines stay clean at flat and under a +12 dB band (the rails are 13.3 V, full scale 2.83 V peak)."""
    sr = 48000; rows, ok = [], True
    for name, m, f0 in (("flat 1 kHz", [0] * 12, 1000), ("1 kHz band +12, 1 kHz", [0] * 6 + [12] + [0] * 5, 1032), ("smile, 100 Hz", SMILE, 100)):
        y, _ = run(sine(sr, f0, 0, 2), sr, *marks(m))
        d = thd(y[sr // 2: sr // 2 + 32768, 0], sr, f0) * 100; rows.append(f"{name}: THD {d:.4f} %"); ok &= d < 0.01
    gate("B3 headroom at 0 dBFS", ok, "; ".join(rows) + " (< 0.01 %)")

def B4():
    """Driven into the rails it clips (the circuit's character) without aliasing: inharmonic energy 20 Hz-20 kHz <= -70 dBc."""
    sr = 44100; f0 = 7000
    y, _ = run(sine(sr, f0, 6, 2), sr, *marks([0] * 9 + [12, 0, 0]))
    seg = y[sr // 2: sr // 2 + 65536, 0]; n = len(seg); Y = np.abs(np.fft.rfft(seg * np.blackman(n))) ** 2; fr = np.fft.rfftfreq(n, 1 / sr)
    harm = np.zeros_like(fr, dtype=bool)
    for k in range(1, 10): harm |= np.abs(fr - k * f0) < 30
    band = (fr > 20) & (fr < 20000)
    fund = Y[np.abs(fr - f0) < 30].sum(); alias = Y[band & ~harm].sum()
    clip = thd(seg, sr, f0) * 100
    gate("B4 aliasing when driven", 10 * np.log10(alias / fund) <= -70, f"7 kHz at +6 dBFS into the 8 kHz band at +12 (clips: THD {clip:.1f} %), 44.1 kHz: inharmonic {10 * np.log10(alias / fund):.1f} dBc (<= -70)")

def click(kind, sr=48000):
    """a 1 kHz sine through the smile curve, the switch flipped at 1.0 s; the click is the energy above 4 kHz in 30 ms
    around the flip (the steady parts have none: the curve is linear, the sine pure), against the sine's level"""
    x = sine(sr, 1000, -6, 2)
    y, _ = run(x, sr, *marks(SMILE), "--toggle", kind, 1.0)
    from scipy.signal import butter, sosfiltfilt
    hp = sosfiltfilt(butter(8, 4000, "hp", fs=sr, output="sos"), y[:, 0])
    a = int((1.0 - 0.015) * sr) + LAT; b = a + int(0.03 * sr)
    ref_rms = np.sqrt(np.mean(y[int(0.5 * sr): int(0.9 * sr), 0] ** 2))
    return 20 * np.log10(np.sqrt(np.mean(hp[a:b] ** 2)) / ref_rms + 1e-12)

def B5():
    rows = {k: click(k) for k in ("bypass", "rev", "att")}
    gate("B5 switch clicks", all(v <= -60 for v in rows.values()), "energy above 4 kHz around the press, 1 kHz sine, smile curve: " + ", ".join(f"EQ IN/OUT {rows['bypass']:.1f}" if k == "bypass" else f"{k.upper()} {v:.1f}" for k, v in rows.items()) + " dBc (<= -60)")

def B6():
    """Every rate a host offers: the response follows the analog model and the latency stays 64 samples."""
    rows, worst, lat_ok = [], 0, True
    pos = [0.2, 0.8, 0.35, 0.65, 0.5, 0.3, 0.7, 0.45, 0.55, 0.25, 0.75, 0.4]
    for sr in (44100, 48000, 88200, 96000, 176400, 192000):
        x = np.zeros(int(sr * 6)); x[100] = 0.05
        y, _ = run(x, sr, "--pos", ",".join(f"{v:.6f}" for v in pos))
        H = np.fft.rfft(y[:, 0]) / np.fft.rfft(x); f = np.fft.rfftfreq(len(x), 1 / sr); m = (f >= 20) & (f <= 20000)
        e = np.max(np.abs(db(H[m]) - db(ref.analog_response(pos, f[m])))); worst = max(worst, e)
        imp = np.zeros(sr // 2); imp[1000] = 0.5; z, _ = run(imp, sr, "--bypass"); lat = int(np.argmax(np.abs(z[:, 0]))) - 1000
        lat_ok &= lat == LAT; rows.append(f"{sr}: {e:.3f} dB / {lat} smp")
    gate("B6 sample rates", worst <= 0.3 and lat_ok, ", ".join(rows) + " (curve vs analog <= 0.3 dB, latency 64)")

def B7():
    """CPU for a stereo instance (process time, the Mac's other load excluded)."""
    rows, ok = [], True
    for sr, need in ((44100, 30), (96000, 12), (192000, 6)):
        x = np.random.default_rng(1).standard_normal((sr * 10, 2)) * 0.1
        _, out = run(x, sr, *marks(SMILE), "--time-it")
        rt = float(out.split("realtime_x=")[1]); ok &= rt > need; rows.append(f"{sr}: {rt:.0f}x (> {need})")
    gate("B7 CPU", ok, ", ".join(rows))

def B8():
    """No CPU rise in silence (denormals): 20 s of silence after a burst costs no more than 20 s of noise."""
    sr = 48000; x = np.zeros((sr * 20, 2)); x[:sr] = np.random.default_rng(2).standard_normal((sr, 2)) * 0.3
    _, a = run(x, sr, *marks(SMILE), "--time-it")
    _, b = run(np.random.default_rng(3).standard_normal((sr * 20, 2)) * 0.1, sr, *marks(SMILE), "--time-it")
    ra, rb = float(a.split("realtime_x=")[1]), float(b.split("realtime_x=")[1])
    gate("B8 silence CPU", ra >= 0.8 * rb, f"burst then silence {ra:.0f}x, noise {rb:.0f}x real time (silence not slower than 0.8 x noise)")

def B9():
    """DC: an offset at the input is removed (the circuit's coupling capacitors); none left after 6 s."""
    sr = 48000; x = np.full(sr * 8, 0.1)
    y, _ = run(x, sr, *marks([0] * 12))
    dc = abs(np.mean(y[sr * 6:, 0]))
    gate("B9 DC", 20 * np.log10(dc / 0.1 + 1e-12) <= -60, f"0.1 DC in: mean output after 6 s {dc:.2e} ({20 * np.log10(dc / 0.1 + 1e-12):.0f} dB, <= -60)")

GATES = dict(B1=B1, B2=B2, B3=B3, B4=B4, B5=B5, B6=B6, B7=B7, B8=B8, B9=B9)
if __name__ == "__main__":
    for g in (sys.argv[1:] or list(GATES)): GATES[g]()
    print("ALL PASS" if fails == 0 else f"{fails} FAIL")
    sys.exit(1 if fails else 0)
