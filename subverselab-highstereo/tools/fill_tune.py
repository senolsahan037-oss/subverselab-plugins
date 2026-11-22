"""Pick the SPREAD fill gain by measurement. Mix from pan_map.py (build/panmap/mix.f32), WIDTH 180, 250 Hz..8 kHz.
Prints centre / gap ring / edge energy shares and the mono level change vs WIDTH 180 without SPREAD."""
import os, subprocess, numpy as np
from scipy import signal
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))); OUT = os.path.join(ROOT, "build", "panmap"); FS = 48000
src = os.path.join(OUT, "mix.f32")
def render(sp, fsc, w=180):
    dst = os.path.join(OUT, f"tune_{w}_{sp}_{fsc}.f32")
    subprocess.run([os.path.join(ROOT, "build", "render_raw"), src, dst, str(FS), str(w), str(sp)], check=True, env={**os.environ, "HS_FILLSCALE": str(fsc)})
    return np.fromfile(dst, dtype=np.float32).reshape(-1, 2)
def shares(y):
    f, _, ZL = signal.stft(y[:, 0], FS, nperseg=2048); _, _, ZR = signal.stft(y[:, 1], FS, nperseg=2048)
    m = (f > 250) & (f < 8000); aL, aR = np.abs(ZL[m]), np.abs(ZR[m])
    pos = np.abs((aR - aL) / (aR + aL + 1e-12)); en = aL ** 2 + aR ** 2; t = en.sum()
    return 100 * en[pos < 0.1].sum() / t, 100 * en[(pos >= 0.15) & (pos < 0.6)].sum() / t, 100 * en[pos >= 0.6].sum() / t
def monodb(y, ref): return 10 * np.log10(np.mean(y.mean(1) ** 2) / np.mean(ref.mean(1) ** 2))
orig = render(0, 0.6, 100); base = render(0, 0.6)
c, g, e = shares(orig); print(f"original              centre {c:4.1f}%  gap {g:4.1f}%  edge {e:4.1f}%")
c, g, e = shares(base); print(f"WIDTH 180             centre {c:4.1f}%  gap {g:4.1f}%  edge {e:4.1f}%")
for fsc in (0.4, 0.6, 0.8, 1.0):
    for sp in (50, 100):
        y = render(sp, fsc); c, g, e = shares(y)
        print(f"fill {fsc:.1f} SPREAD {sp:3d}  centre {c:4.1f}%  gap {g:4.1f}%  edge {e:4.1f}%  mono {monodb(y, base):+5.2f} dB")
