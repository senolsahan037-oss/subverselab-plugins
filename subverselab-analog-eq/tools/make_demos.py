#!/usr/bin/env python3
"""Listening pairs for the owner (P1): each setting as A (EQ out) and B (EQ in), both at -16 LUFS so the colour is heard,
not the volume. Material: SEDA's demo stems (synth-made, ../subverselab-seda/tools/demo_material). Output: ../demos/.

  make_demos.py      writes demos/<name>_A_off.wav and demos/<name>_B_eq.wav, prints LUFS and peak of each
"""
import os, subprocess, tempfile
import numpy as np, soundfile as sf, pyloudnorm as pyln

HERE = os.path.dirname(os.path.abspath(__file__))
RAW = os.path.join(HERE, "..", "build", "eqraw")
MAT = os.path.join(HERE, "..", "..", "subverselab-seda", "tools", "demo_material")
OUT = os.path.join(HERE, "..", "demos")
SR = 48000
TARGET = -16.0
meter = pyln.Meter(SR)

def load(name):
    x, sr = sf.read(os.path.join(MAT, name), always_2d=True); assert sr == SR
    return x

def fit(x, n):
    reps = int(np.ceil(n / len(x))); return np.tile(x, (reps, 1))[:n]

n = 16 * SR
drums, pluck, lead, vocal = fit(load("drums.wav"), n), load("pluck.wav")[:n], load("lead.wav")[:n], load("vocal.wav")[:n]
mix = drums + 0.5 * pluck + 0.5 * lead + 0.7 * vocal

SETTINGS = {
    # name: (material, eqraw flags)
    "mixbus": (mix, ["--lowfreq", 60, "--lowboost", 5, "--lowcut", 3, "--presfreq", 2800, "--pres", 1.5, "--highfreq", 8000, "--high", 2, "--drive", 6]),
    "master": (mix, ["--lowfreq", 40, "--lowboost", 3, "--lowcut", 2, "--highfreq", 12000, "--high", 1.5, "--drive", 3]),
    "drumbus": (fit(load("drums.wav"), n), ["--lowfreq", 60, "--lowboost", 8, "--lowcut", 5, "--presfreq", 1400, "--pres", -2, "--highfreq", 8000, "--high", 3, "--drive", 12]),
    "vocal": (vocal, ["--lowfreq", 150, "--lowcut", 6, "--presfreq", 4000, "--pres", 3, "--highfreq", 12000, "--high", 2, "--drive", 6]),
    "drive_only": (mix, ["--drive", 18]),
}

def norm(x):
    x = x / max(1e-9, 10 ** ((meter.integrated_loudness(x) - TARGET) / 20))
    return x

os.makedirs(OUT, exist_ok=True)
tmp = tempfile.mkdtemp(prefix="peq-demo-")
for name, (x, flags) in SETTINGS.items():
    x = norm(x) * 10 ** (-6 / 20)   # into the EQ 6 dB under the target, as on a channel
    src = os.path.join(tmp, "in.wav"); sf.write(src, x.astype(np.float32), SR, subtype="FLOAT")
    dst = os.path.join(tmp, "out.wav")
    subprocess.run([RAW, src, dst] + [str(f) for f in flags], check=True)
    y, _ = sf.read(dst, always_2d=True)
    lat = 63; x_al = np.vstack([np.zeros((lat, 2)), x])[:len(y)]   # A delayed by the latency, so A/B line up
    for tag, sig in (("A_off", x_al), ("B_eq", y)):
        s = norm(sig)
        sf.write(os.path.join(OUT, f"{name}_{tag}.wav"), s.astype(np.float32), SR, subtype="FLOAT")
        print(f"{name}_{tag}: {meter.integrated_loudness(s):.1f} LUFS, peak {20 * np.log10(np.max(np.abs(s))):.1f} dBFS")
