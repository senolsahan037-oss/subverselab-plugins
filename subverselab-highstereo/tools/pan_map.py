"""Measure where the sound sits from left to right, per band, through the real engine.
Makes a test mix (sources panned at fixed places), renders it through build/render_raw with three settings, and
prints per band how much energy sits in the "gap" ring (0.2 < |pan| < 0.6) between the centre and the edges.
Also writes docs/pan_map_measured.png (one strip per band per setting). Run from the project root.
"""
import os, subprocess, sys
import numpy as np
from scipy import signal

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build", "panmap"); os.makedirs(OUT, exist_ok=True)
FS = 48000; N = FS * 8
rng = np.random.default_rng(3)

def bandnoise(lo, hi):
    sos = signal.butter(4, [lo, hi], btype="band", fs=FS, output="sos")
    x = signal.sosfilt(sos, rng.standard_normal(N)); return x / np.sqrt(np.mean(x ** 2))
def pan(x, p):   # constant power, p in -1..1
    a = (p + 1) * np.pi / 4; return np.cos(a) * x, np.sin(a) * x

srcs = [  # (signal, pan, level)
    (np.sin(2 * np.pi * 55 * np.arange(N) / FS) * np.sqrt(2), 0.0, 0.30),   # bass, centre
    (bandnoise(300, 3000), 0.0, 0.20),                                        # vocal, centre
    (bandnoise(200, 2500), -0.35, 0.12), (bandnoise(200, 2500), 0.35, 0.12), # pads, inside
    (bandnoise(1500, 6000), -0.6, 0.08), (bandnoise(1500, 6000), 0.6, 0.08), # guitars, wider
    (bandnoise(6000, 14000), -0.7, 0.05), (bandnoise(6000, 14000), 0.7, 0.05), # hats
    (bandnoise(5000, 14000), 0.0, 0.05),                                      # air on the vocal
]
L = np.zeros(N); R = np.zeros(N)
for x, p, g in srcs:
    l, r = pan(x * g, p); L += l; R += r
mix = np.stack([L, R], 1).astype(np.float32)
src = os.path.join(OUT, "mix.f32"); mix.tofile(src)

settings = [("ORIGINAL", 100, 0), ("WIDTH 180", 180, 0), ("WIDTH 180 + SPREAD 50", 180, 50), ("WIDTH 180 + SPREAD 100", 180, 100)]
if len(sys.argv) > 1 and sys.argv[1] == "--only-render": pass
edges = [0, 100, 250, 500, 1000, 2000, 4000, 8000, FS / 2]
names = ["<100", "100", "250", "500", "1k", "2k", "4k", "8k+"]
NB = 41
results = {}
for label, w, sp in settings:
    dst = os.path.join(OUT, f"out_{w}_{sp}.f32")
    subprocess.run([os.path.join(ROOT, "build", "render_raw"), src, dst, str(FS), str(w), str(sp)], check=True)
    y = np.fromfile(dst, dtype=np.float32).reshape(-1, 2)
    f, t, ZL = signal.stft(y[:, 0], FS, nperseg=2048); _, _, ZR = signal.stft(y[:, 1], FS, nperseg=2048)
    aL, aR = np.abs(ZL), np.abs(ZR)
    # position: magnitude ratio, and content whose L and R are opposite in sign goes past the speakers
    pos = (aR - aL) / (aR + aL + 1e-12)
    anti = np.real(ZL * np.conj(ZR)) < 0
    pos = np.where(anti, np.sign(pos + 1e-12) * np.minimum(1.25, np.abs(pos) + 0.25), pos)
    en = aL ** 2 + aR ** 2
    hist = []
    for k in range(8):
        m = (f >= edges[k]) & (f < edges[k + 1])
        h, _ = np.histogram(pos[m].ravel(), bins=NB, range=(-1.25, 1.25), weights=en[m].ravel())
        hist.append(h / (h.sum() + 1e-20))
    results[label] = np.array(hist)
    # gap ring: 0.2 < |pan| < 0.6 ; edge: |pan| >= 0.6
    centers = np.linspace(-1.25, 1.25, NB + 1); centers = (centers[:-1] + centers[1:]) / 2
    ring = (np.abs(centers) > 0.2) & (np.abs(centers) < 0.6)
    print(f"{label:24s} gap-ring share per band: " + "  ".join(f"{names[k]}:{100 * results[label][k][ring].sum():4.0f}%" for k in range(8)))
    # mono sum check against the input mid
    mono_out = y.mean(1); mono_in = mix.mean(1)
    err = 10 * np.log10(np.mean((mono_out - mono_in) ** 2) / np.mean(mono_in ** 2) + 1e-30)
    print(f"{'':24s} mono sum vs input: {err:.1f} dB")

np.save(os.path.join(OUT, "hist.npy"), {k: v for k, v in results.items()}, allow_pickle=True)

# Picture: one block per setting, rows = bands (high at top), brightness = energy at that position.
from PIL import Image, ImageDraw, ImageFont
F = os.path.join(ROOT, "Assets", "Fonts")
fnt = ImageFont.truetype(os.path.join(F, "texgyreheros-bold.otf"), 22)
GREY = (218, 220, 212); INK = (3, 55, 51); GOLD = (197, 160, 89)
cw, rh, pad = 520, 26, 40
img = Image.new("RGB", (pad + len(settings) * (cw + pad), 110 + 8 * (rh + 6) + 40), GREY); d = ImageDraw.Draw(img)
for si, (label, w, sp) in enumerate(settings):
    x0 = pad + si * (cw + pad)
    d.text((x0, 30), label, font=fnt, fill=INK)
    H = results[label]
    for k in range(8):
        row = 7 - k
        y = 80 + row * (rh + 6)
        hv = H[k] / (H[k].max() + 1e-20)
        for b in range(NB):
            v = hv[b] ** 0.6
            c = tuple(int(GREY[i] + (INK[i] - GREY[i]) * v) for i in range(3))
            if abs(np.linspace(-1.25, 1.25, NB)[b]) > 1.0 and v > 0.05: c = tuple(int(GREY[i] + (GOLD[i] - GREY[i]) * v) for i in range(3))
            d.rectangle([x0 + b * cw / NB, y, x0 + (b + 1) * cw / NB - 1, y + rh], fill=c)
        d.text((x0 - 36, y + 2), names[k], font=ImageFont.truetype(os.path.join(F, "texgyreheros-regular.otf"), 16), fill=INK)
    for xx in (x0 + cw * 0.25 / 2.5, x0 + cw * 2.25 / 2.5): d.line([xx, 75, xx, 80 + 8 * (rh + 6)], fill=INK, width=1)
out = os.path.join(ROOT, "docs", "pan_map_measured.png"); img.save(out); print(out)
