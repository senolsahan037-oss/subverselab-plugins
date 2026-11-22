"""Does SPREAD fill the field or open a hole? A mono source (centre) through SPREAD, pan distribution of the result.
HOLE = deepest dip of the energy-vs-position curve between the centre and the outer edge of the spread, relative to the
smaller of the two sides of that dip (dB). Below -3 dB = a visible hole (two lobes). Near 0 or above = filled.
Usage: python3 tools/pan_fill.py [tag]   (renders with build/render_raw)
"""
import os, subprocess, sys
import numpy as np
from scipy import signal

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build", "panmap"); os.makedirs(OUT, exist_ok=True)
FS = 48000; N = FS * 6
rng = np.random.default_rng(5)
sos = signal.butter(4, [200, 8000], btype="band", fs=FS, output="sos")
x = signal.sosfilt(sos, rng.standard_normal(N)); x = 0.2 * x / np.sqrt(np.mean(x ** 2))
src = os.path.join(OUT, "mono.f32"); np.stack([x, x], 1).astype(np.float32).tofile(src)
NB = 41
tag = sys.argv[1] if len(sys.argv) > 1 else ""
for sp in (25, 50, 100):
    dst = os.path.join(OUT, f"mono_sp{sp}.f32")
    subprocess.run([os.path.join(ROOT, "build", "render_raw"), src, dst, str(FS), "100", str(sp)], check=True)
    y = np.fromfile(dst, dtype=np.float32).reshape(-1, 2)
    f, _, ZL = signal.stft(y[:, 0], FS, nperseg=2048); _, _, ZR = signal.stft(y[:, 1], FS, nperseg=2048)
    aL, aR = np.abs(ZL), np.abs(ZR)
    m = (f > 300) & (f < 6000)
    pos = ((aR - aL) / (aR + aL + 1e-12))[m].ravel(); en = (aL ** 2 + aR ** 2)[m].ravel()
    h, e = np.histogram(np.abs(pos), bins=20, range=(0, 1), weights=en); h = h / h.sum()
    # smooth a little, then find the hole: min between the centre bin and the outer peak
    hs = np.convolve(h, [0.25, 0.5, 0.25], mode="same")
    outer = int(np.argmax(hs[3:])) + 3
    if outer <= 1: hole = 0.0
    else:
        inner_min = hs[1:outer].min()
        hole = 10 * np.log10(inner_min / min(hs[0], hs[outer]) + 1e-12) if inner_min < min(hs[0], hs[outer]) else 0.0
    mono = 10 * np.log10(np.mean((y.mean(1) - x) ** 2) / np.mean(x ** 2) + 1e-30)
    shape = " ".join(f"{v * 100:3.0f}" for v in h[:16])
    print(f"{tag} SPREAD {sp:3d}%  hole {hole:6.1f} dB   mono null {mono:7.1f} dB   |pan| 0..0.8 energy %: {shape}")
