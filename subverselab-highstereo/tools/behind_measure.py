"""BEHIND measurement: old engine (before 20 Nov BEHIND) vs new, through the real engine binaries.
Usage: python3 tools/behind_measure.py <render_old> <render_new> <workdir>
Test mix = tools/pan_map.py sources + a decorrelated room (what real mixes carry at the sides).
Prints per setting: correlation, rear share (STFT energy where |S| > |M|, i.e. the lower half of a 360 scope / what a
matrix decoder sends to the rear), mono error vs the input's mono, and side level per band vs the input.
"""
import os, subprocess, sys
import numpy as np
from scipy import signal

old_bin, new_bin, work = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(work, exist_ok=True)
FS = 48000; N = FS * 8
rng = np.random.default_rng(3)

def bandnoise(lo, hi):
    sos = signal.butter(4, [lo, hi], btype="band", fs=FS, output="sos")
    x = signal.sosfilt(sos, rng.standard_normal(N)); return x / np.sqrt(np.mean(x ** 2))
def pan(x, p):
    a = (p + 1) * np.pi / 4; return np.cos(a) * x, np.sin(a) * x

srcs = [
    (np.sin(2 * np.pi * 55 * np.arange(N) / FS) * np.sqrt(2), 0.0, 0.30),
    (bandnoise(300, 3000), 0.0, 0.20),
    (bandnoise(200, 2500), -0.35, 0.12), (bandnoise(200, 2500), 0.35, 0.12),
    (bandnoise(1500, 6000), -0.6, 0.08), (bandnoise(1500, 6000), 0.6, 0.08),
    (bandnoise(6000, 14000), -0.7, 0.05), (bandnoise(6000, 14000), 0.7, 0.05),
    (bandnoise(5000, 14000), 0.0, 0.05),
]
L = np.zeros(N); R = np.zeros(N)
for x, p, g in srcs:
    l, r = pan(x * g, p); L += l; R += r
L += 0.05 * bandnoise(200, 10000); R += 0.05 * bandnoise(200, 10000)   # room: decorrelated
inp = np.stack([L, R], 1).astype(np.float32)
src = os.path.join(work, "mix.f32"); inp.tofile(src)

def render(binary, width):
    out = os.path.join(work, f"o_{os.path.basename(binary)}_{width}.f32")
    subprocess.run([binary, src, out, str(FS), str(width), "0"], check=True)
    y = np.fromfile(out, dtype=np.float32).reshape(-1, 2).astype(np.float64)
    return y[FS:], inp[FS:].astype(np.float64)   # skip 1 s settle

edges = [0, 100, 250, 500, 1000, 2000, 4000, 8000, FS / 2]
names = ["<100", "100", "250", "500", "1k", "2k", "4k", "8k+"]

def metrics(y, x):
    l, r = y[:, 0], y[:, 1]
    corr = np.sum(l * r) / np.sqrt(np.sum(l * l) * np.sum(r * r))
    m, s = 0.5 * (l + r), 0.5 * (l - r)
    f, t, M = signal.stft(m, FS, nperseg=2048); _, _, S = signal.stft(s, FS, nperseg=2048)
    e = np.abs(M) ** 2 + np.abs(S) ** 2
    rear = np.sum(e[np.abs(S) > np.abs(M)]) / np.sum(e)
    xm, xs = 0.5 * (x[:, 0] + x[:, 1]), 0.5 * (x[:, 0] - x[:, 1])
    # mono through the band tree is an allpass of the input's mono (phase only), so compare magnitude spectra:
    _, Pm_in = signal.welch(xm, FS, nperseg=8192); fm_, Pm_out = signal.welch(m, FS, nperseg=8192)
    k = (fm_ > 30) & (fm_ < 16000) & (Pm_in > Pm_in.max() * 1e-7)
    monoErr = np.max(np.abs(10 * np.log10(Pm_out[k] / Pm_in[k])))   # worst bin, dB
    _, Sx = signal.welch(xs, FS, nperseg=4096); fw, Sy = signal.welch(s, FS, nperseg=4096)
    side = []
    for lo, hi in zip(edges[:-1], edges[1:]):
        k = (fw >= lo) & (fw < hi)
        side.append(10 * np.log10(np.sum(Sy[k]) / np.sum(Sx[k])))
    lufsish = 10 * np.log10(np.mean(l * l + r * r))
    return corr, rear, monoErr, side, lufsish

print(f"{'setting':<16}{'corr':>7}{'rear%':>7}{'mono max|dB|':>13}{'level dB':>10}   side dB per band " + " ".join(f"{n:>5}" for n in names))
for w in (100, 150, 175, 200):
    for tag, b in (("old", old_bin), ("new", new_bin)):
        if w == 100 and tag == "new": continue
        y, x = render(b, w)
        c, rr, me, side, lv = metrics(y, x)
        print(f"{tag} WIDTH {w:<6}{c:7.3f}{rr * 100:7.1f}{me:13.3f}{lv:10.2f}   {' ' * 17}" + " ".join(f"{v:5.1f}" for v in side))
