#!/usr/local/bin/python3
"""Harshness (owner 24 Oct on the stronger version: "biraz hars duyuyorum, biraz yatıştırabiliriz").
Measured as the 2.5-5 kHz band re the 0.2-2.5 kHz body (dB), output minus input, on active 100 ms frames (95th percentile:
the harsh moments) and the mean; per setting and with one stage held off at a time (testoff bits).

  harsh.py
"""
import os, subprocess, sys
import numpy as np, soundfile as sf
from scipy import signal
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import measure as M, knobs as K

def harsh(y, sr):
    m = y.mean(axis=1)
    hb = signal.sosfilt(signal.butter(4, [2500, 5000], "bandpass", fs=sr, output="sos"), m)
    bb = signal.sosfilt(signal.butter(4, [200, 2500], "bandpass", fs=sr, output="sos"), m)
    n = int(0.1 * sr); k = len(m) // n
    ph = (hb[: k * n] ** 2).reshape(k, n).mean(1); pb = (bb[: k * n] ** 2).reshape(k, n).mean(1)
    act = pb > pb.max() * 10 ** (-20 / 10)
    r = 10 * np.log10(ph[act] / pb[act] + 1e-20)
    return np.mean(r), np.percentile(r, 95)

CASES = [("LIFT 6", ["lift=6"]), ("LIFT 10", ["lift=10"]), ("LIFT 10 PUSH", ["lift=10", "mood=1"]),
         ("LIFT 10 no layer", ["lift=10", "testoff=16"]), ("LIFT 10 no exciter", ["lift=10", "testoff=64"]),
         ("LIFT 10 no comp", ["lift=10", "testoff=2"]), ("LIFT 10 no de-ess", ["lift=10", "testoff=8"])]

for path, skip in ((K.LIVE, 0), (K.OWNER, 10)):
    x, sr = sf.read(path, dtype="float32", always_2d=True); x = x[skip * sr:]
    x = np.tile(x, (int(np.ceil(24 * sr / len(x))), 1))[: 24 * sr]
    if x.shape[1] == 1: x = np.repeat(x, 2, 1)
    fi, fo = os.path.join(M.TMP, "h_in.f32"), os.path.join(M.TMP, "h_out.f32"); x.tofile(fi)
    a0, p0 = harsh(x[10 * sr:], sr)
    print(os.path.basename(path), f"input: mean {a0:+.1f}, 95th {p0:+.1f} dB")
    for name, p in CASES:
        subprocess.run([M.RAW, fi, fo, str(sr), "--listen", "8"] + p, check=True)
        a, q = harsh(np.fromfile(fo, dtype=np.float32).reshape(-1, 2)[10 * sr:], sr)
        print(f"  {name:20} harsh mean {a - a0:+5.1f}  95th {q - p0:+5.1f} dB re input")
