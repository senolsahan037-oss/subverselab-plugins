"""SPREAD fill on a mix that HAS a hole: vocal in the centre, guitars and hats panned near the edges (LCR mixing).
Energy shares 250 Hz..8 kHz by |pan|: centre (<0.1), gap (0.15..0.6), edge (>=0.6); mono level change vs no SPREAD."""
import os, subprocess, numpy as np
from scipy import signal
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))); OUT = os.path.join(ROOT, "build", "panmap"); FS = 48000; N = FS * 6
rng = np.random.default_rng(9)
def bn(lo, hi):
    x = signal.sosfilt(signal.butter(4, [lo, hi], btype="band", fs=FS, output="sos"), rng.standard_normal(N)); return x / np.sqrt(np.mean(x ** 2))
def pan(x, p): a = (p + 1) * np.pi / 4; return np.cos(a) * x, np.sin(a) * x
L = np.zeros(N); R = np.zeros(N)
for x, p, g in [(bn(300, 3000), 0, 0.20), (bn(200, 5000), -0.85, 0.15), (bn(200, 5000), 0.85, 0.15), (bn(6000, 14000), -0.9, 0.05), (bn(6000, 14000), 0.9, 0.05)]:
    l, r = pan(x * g, p); L += l; R += r
src = os.path.join(OUT, "lcr.f32"); np.stack([L, R], 1).astype(np.float32).tofile(src)
def render(w, sp, fsc):
    dst = os.path.join(OUT, f"lcr_{w}_{sp}_{fsc}.f32")
    subprocess.run([os.path.join(ROOT, "build", "render_raw"), src, dst, str(FS), str(w), str(sp)], check=True, env={**os.environ, "HS_FILLSCALE": str(fsc)})
    return np.fromfile(dst, dtype=np.float32).reshape(-1, 2)
def shares(y):
    f, _, ZL = signal.stft(y[:, 0], FS, nperseg=2048); _, _, ZR = signal.stft(y[:, 1], FS, nperseg=2048)
    m = (f > 250) & (f < 8000); aL, aR = np.abs(ZL[m]), np.abs(ZR[m])
    pos = np.abs((aR - aL) / (aR + aL + 1e-12)); en = aL ** 2 + aR ** 2; t = en.sum()
    return [100 * en[(pos >= a) & (pos < b)].sum() / t for a, b in [(0, .1), (.15, .6), (.6, 2)]]
for w in (100, 150):
    base = render(w, 0, 0.6)
    c, g, e = shares(base); print(f"WIDTH {w}  no SPREAD          centre {c:4.1f}%  gap {g:4.1f}%  edge {e:4.1f}%")
    for fsc in (0.6, 1.0):
        for sp in (50, 100):
            y = render(w, sp, fsc); c, g, e = shares(y)
            mono = 10 * np.log10(np.mean(y.mean(1) ** 2) / np.mean(base.mean(1) ** 2))
            print(f"WIDTH {w}  fill {fsc:.1f} SPREAD {sp:3d}  centre {c:4.1f}%  gap {g:4.1f}%  edge {e:4.1f}%  mono {mono:+5.2f} dB")
