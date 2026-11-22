"""SPREAD fill, sources in separate bands so the position estimate is clean (LCR mix, hole in the middle):
vocal 300-1000 Hz at the centre, guitars 1.5-4 kHz at +-0.85, hats 6-12 kHz at +-0.9.
Vocal band: centre share must stay. Guitar / hat bands: the gap ring (0.15..0.6) should fill. Mono change vs no SPREAD."""
import os, subprocess, numpy as np
from scipy import signal
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))); OUT = os.path.join(ROOT, "build", "panmap"); FS = 48000; N = FS * 6
rng = np.random.default_rng(11)
def bn(lo, hi):
    x = signal.sosfilt(signal.butter(6, [lo, hi], btype="band", fs=FS, output="sos"), rng.standard_normal(N)); return x / np.sqrt(np.mean(x ** 2))
def pan(x, p): a = (p + 1) * np.pi / 4; return np.cos(a) * x, np.sin(a) * x
L = np.zeros(N); R = np.zeros(N)
for x, p, g in [(bn(300, 1000), 0, 0.2), (bn(1500, 4000), -0.85, 0.15), (bn(6000, 12000), 0.9, 0.06)]:   # one source per band, so each bin has one position
    l, r = pan(x * g, p); L += l; R += r
src = os.path.join(OUT, "lcr3.f32"); np.stack([L, R], 1).astype(np.float32).tofile(src)
def render(w, sp, fsc):
    dst = os.path.join(OUT, f"lcr3_{w}_{sp}_{fsc}.f32")
    subprocess.run([os.path.join(ROOT, "build", "render_raw"), src, dst, str(FS), str(w), str(sp)], check=True, env={**os.environ, "HS_FILLSCALE": str(fsc)})
    return np.fromfile(dst, dtype=np.float32).reshape(-1, 2)
def shares(y, lo, hi):
    f, _, ZL = signal.stft(y[:, 0], FS, nperseg=2048); _, _, ZR = signal.stft(y[:, 1], FS, nperseg=2048)
    m = (f > lo) & (f < hi); aL, aR = np.abs(ZL[m]), np.abs(ZR[m])
    pos = np.abs((aR - aL) / (aR + aL + 1e-12)); en = aL ** 2 + aR ** 2; t = en.sum()
    return [100 * en[(pos >= a) & (pos < b)].sum() / t for a, b in [(0, .1), (.15, .6), (.6, 2)]]
def line(tag, y, base):
    v = shares(y, 350, 900); g = shares(y, 1700, 3600); h = shares(y, 6500, 11000)
    mono = 10 * np.log10(np.mean(y.mean(1) ** 2) / np.mean(base.mean(1) ** 2))
    print(f"{tag:30s} vocal centre {v[0]:5.1f}% | guitars gap {g[1]:5.1f}% edge {g[2]:5.1f}% | hats gap {h[1]:5.1f}% | mono {mono:+5.2f} dB")
for w in (100, 150):
    base = render(w, 0, 0.6); line(f"WIDTH {w}, no SPREAD", base, base)
    for fsc in (0.7, 1.0, 1.4):
        for sp in (50, 100):
            line(f"WIDTH {w}, fill {fsc:.1f}, SPREAD {sp}", render(w, sp, fsc), base)
