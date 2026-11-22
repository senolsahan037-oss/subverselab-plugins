"""Hole metric on the mix renders from pan_map.py: per band, energy vs |pan| (0..1, 20 bins), smoothed;
centre share (|pan|<0.1), and the deepest dip between the centre and the outer peak, relative to the smaller side (dB)."""
import os, sys, numpy as np
from scipy import signal
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))); OUT = os.path.join(ROOT, "build", "panmap"); FS = 48000
edges = [100, 250, 500, 1000, 2000, 4000, 8000, 24000]; names = ["100", "250", "500", "1k", "2k", "4k", "8k+"]
for fn in sys.argv[1:]:
    y = np.fromfile(os.path.join(OUT, fn), dtype=np.float32).reshape(-1, 2)
    f, _, ZL = signal.stft(y[:, 0], FS, nperseg=2048); _, _, ZR = signal.stft(y[:, 1], FS, nperseg=2048)
    aL, aR = np.abs(ZL), np.abs(ZR); pos = np.abs((aR - aL) / (aR + aL + 1e-12)); en = aL ** 2 + aR ** 2
    row = []
    for k in range(7):
        m = (f >= edges[k]) & (f < edges[k + 1])
        h, _ = np.histogram(pos[m].ravel(), bins=20, range=(0, 1), weights=en[m].ravel()); h = h / h.sum()
        hs = np.convolve(h, [0.25, 0.5, 0.25], mode="same"); hs[0] = h[0] * 0.75 + h[1] * 0.25
        outer = int(np.argmax(hs[2:])) + 2
        inner = hs[1:outer].min() if outer > 1 else hs[0]
        ref = min(hs[0], hs[outer]); hole = 10 * np.log10(inner / ref) if inner < ref else 0.0
        row.append(f"{names[k]}: c{100 * (h[0] + h[1]):3.0f}% hole{hole:5.1f}")
    print(f"{fn:22s} " + " | ".join(row))
