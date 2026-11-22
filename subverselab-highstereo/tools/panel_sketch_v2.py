"""HIGH STEREO panel sketch v2: the screen draws the stereo image itself.
Rows = frequency bands (bass at the bottom), each row's glowing bar is as wide as that band is in stereo; a band that
will vanish in mono spills past the speakers in gold. Left: a goniometer. Colours from Assets/Brand/_kit/brand.py only.
Run: python3 tools/panel_sketch_v2.py -> docs/panel_sketch_v2.png
"""
import math, os, random
from PIL import Image, ImageDraw, ImageFilter, ImageFont, ImageChops

HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
LAUNCH = os.path.abspath(os.path.join(ROOT, "..", ".."))
CREAM = (242, 239, 231); INK = (3, 55, 51); GOLD = (197, 160, 89); INK_SOFT = (73, 108, 104); GROUND = (3, 40, 37)
OFFSEG = (10, 54, 49)

S = 2; W, H = 960 * S, 670 * S
F = os.path.join(ROOT, "Assets", "Fonts")
def font(n, px): return ImageFont.truetype(os.path.join(F, n), int(px * S))
MONO, BOLD = "DejaVuSansMono-Bold.ttf", "texgyreheros-bold.otf"

img = Image.new("RGB", (W, H), INK); d = ImageDraw.Draw(img)
glow = Image.new("RGB", (W, H), (0, 0, 0)); gd = ImageDraw.Draw(glow)
def r(*v): return [x * S for x in v]
def T(x, y, s, f, c, a="la", g=False):
    d.text((x * S, y * S), s, font=f, fill=c, anchor=a)
    if g: gd.text((x * S, y * S), s, font=f, fill=c, anchor=a)
def box(b, c, g=True):
    d.rectangle(r(*b), fill=c)
    if g: gd.rectangle(r(*b), fill=c)

d.rounded_rectangle(r(8, 8, 952, 662), radius=16 * S, outline=INK_SOFT, width=S)

# Header
wm = Image.open(os.path.join(LAUNCH, "Assets", "Brand", "high-stereo-wordmark.png")).convert("RGB")
wm = wm.resize((118 * S, int(118 * S * wm.height / wm.width)), Image.LANCZOS); img.paste(wm, (28 * S, 22 * S))
T(156, 30, "MODEL HS-84", font(BOLD, 12), GOLD)
T(156, 48, "STEREO IMAGE  /  MONO SAFETY", font(BOLD, 10), INK_SOFT)

# LISTEN: four keys with drawn icons.
def icon(kind, cx, cy, c):
    if kind == "OFF":
        d.ellipse(r(cx - 7, cy - 7, cx + 7, cy + 7), outline=c, width=2 * S)
    elif kind == "MONO":   # one speaker
        d.rounded_rectangle(r(cx - 7, cy - 10, cx + 7, cy + 10), radius=2 * S, outline=c, width=2 * S)
        d.ellipse(r(cx - 4, cy - 1, cx + 4, cy + 7), outline=c, width=2 * S)
    elif kind == "PHONE":
        d.rounded_rectangle(r(cx - 6, cy - 11, cx + 6, cy + 11), radius=3 * S, outline=c, width=2 * S)
        d.line(r(cx - 2, cy + 7, cx + 2, cy + 7), fill=c, width=2 * S)
    else:                  # two speakers
        for dx in (-9, 9):
            d.rounded_rectangle(r(cx + dx - 5, cy - 9, cx + dx + 5, cy + 9), radius=2 * S, outline=c, width=2 * S)
            d.ellipse(r(cx + dx - 3, cy, cx + dx + 3, cy + 6), outline=c, width=S)
modes = ["OFF", "MONO", "PHONE", "SPEAKERS"]; act = "PHONE"
kx = 640
T(kx - 14, 36, "LISTEN", font(BOLD, 10), INK_SOFT, "ra")
for i, m in enumerate(modes):
    x0 = kx + i * 74; on = m == act
    d.rounded_rectangle(r(x0, 20, x0 + 66, 74), radius=6 * S, fill=GROUND, outline=GOLD if on else INK_SOFT, width=(2 if on else 1) * S)
    c = GOLD if on else INK_SOFT
    icon(m, x0 + 33, 40, c)
    if on: icon(m, x0 + 33, 40, c); gd.rounded_rectangle(r(x0, 20, x0 + 66, 74), radius=6 * S, outline=GOLD, width=2 * S)
    T(x0 + 33, 58, m, font(MONO, 9), c, "ma", on)
T(936, 82, "PHONE  -  MONITOR ONLY, NOT IN THE BOUNCE", font(MONO, 9), GOLD, "ra", True)

# Screen
sx0, sy0, sx1, sy1 = 28, 100, 932, 468
d.rounded_rectangle(r(sx0, sy0, sx1, sy1), radius=10 * S, fill=GROUND, outline=(1, 24, 22), width=4 * S)

# Goniometer (left)
gx, gy, gR = 170, 300, 124
for k in range(1, 4):
    d.arc(r(gx - gR * k / 3, gy - gR * k / 3, gx + gR * k / 3, gy + gR * k / 3), 180, 360, fill=OFFSEG, width=S)
d.line(r(gx - gR, gy, gx + gR, gy), fill=INK_SOFT, width=S)
d.line(r(gx, gy, gx, gy - gR), fill=OFFSEG, width=S)
d.line(r(gx, gy, gx - gR * 0.707, gy - gR * 0.707), fill=OFFSEG, width=S)
d.line(r(gx, gy, gx + gR * 0.707, gy - gR * 0.707), fill=OFFSEG, width=S)
T(gx - gR * 0.75, gy - gR * 0.80, "L", font(MONO, 10), INK_SOFT, "ma"); T(gx + gR * 0.75, gy - gR * 0.80, "R", font(MONO, 10), INK_SOFT, "ma")
T(gx, gy - gR - 16, "M", font(MONO, 10), INK_SOFT, "ma")
random.seed(4)
for _ in range(1300):
    m = random.gauss(0, 0.42); s = random.gauss(0, 0.20)
    if random.random() < 0.10: s = random.gauss(0, 0.55); m = random.gauss(0, 0.12)   # the out-of-phase band
    a = abs(m); px = gx + s * gR; py = gy - a * gR
    if math.hypot(px - gx, py - gy) < gR - 2:
        c = GOLD if abs(s) > a else CREAM
        d.point((px * S, py * S), fill=c); gd.point((px * S, py * S), fill=c)
        d.point((px * S + 1, py * S), fill=c)
T(gx, gy + 14, "CORRELATION", font(MONO, 9), INK_SOFT, "ma")
cb0, cb1, cy = gx - 108, gx + 108, gy + 32
for i in range(30):
    v = -1 + (i + 0.5) / 15; x = cb0 + i * (cb1 - cb0) / 30
    box((x, cy, x + (cb1 - cb0) / 30 - 2, cy + 10), CREAM if 0 <= v <= 0.42 else OFFSEG, 0 <= v <= 0.42)
T(cb0, cy + 13, "-1", font(MONO, 8), INK_SOFT, "ma"); T(gx, cy + 13, "0", font(MONO, 8), INK_SOFT, "ma"); T(cb1, cy + 13, "+1", font(MONO, 8), INK_SOFT, "ma")
T(gx, cy + 26, "+0.42", font(MONO, 20), CREAM, "ma", True)

d.line(r(318, sy0 + 18, 318, sy1 - 18), fill=OFFSEG, width=S)

# Stereo field (right): one row per band, bass at the bottom.
fx0, fx1 = 410, 770; fc = (fx0 + fx1) / 2; half = (fx1 - fx0) / 2
T(fx0, sy0 + 14, "L", font(MONO, 11), INK_SOFT); T(fx1, sy0 + 14, "R", font(MONO, 11), INK_SOFT, "ra")
T(fc, sy0 + 14, "STEREO FIELD", font(MONO, 11), CREAM, "ma", True)
for x in (fx0, fx1):   # speaker posts
    d.line(r(x, sy0 + 34, x, sy1 - 26), fill=INK_SOFT, width=S)
d.line(r(fc, sy0 + 34, fc, sy1 - 26), fill=OFFSEG, width=S)
bands = ["8k+", "4k", "2k", "1k", "500", "250", "100", "<100"]
width = [0.80, 0.74, 0.62, 1.22, 0.50, 0.38, 0.06, 0.0]          # 1.0 = speaker to speaker
loss  = [-2.8, -2.7, -2.4, -8.6, -2.1, -1.3, -0.1, 0.0]
rows_y0, rh = sy0 + 44, 39
for i, b in enumerate(bands):
    y = rows_y0 + i * rh; yc = y + rh / 2
    bad = width[i] > 1.0
    T(fx0 - 46, yc - 6, b, font(MONO, 11), GOLD if bad else CREAM, "ra", True)
    # segments across the row, lit where the band is
    n = 40; segw = (fx1 - fx0) / n
    for j in range(n):
        x = fx0 + j * segw; dist = abs((x + segw / 2) - fc) / half
        on = dist <= max(width[i], 0.012)
        box((x + 1, yc - 9, x + segw - 1, yc + 9), (CREAM if not bad else GOLD) if on else OFFSEG, on)
    if bad:   # spill past the speakers
        for side in (-1, 1):
            for j in range(2):
                x = (fx1 + 6 + j * segw) if side > 0 else (fx0 - 6 - (j + 1) * segw)
                box((x + 1, yc - 9, x + segw - 1, yc + 9), GOLD, True)
    T(fx1 + 44, yc - 7, f"{loss[i]:+.1f}" if loss[i] else " 0.0", font(MONO, 12), GOLD if bad else INK_SOFT, "la", bad)
    # FIX key at the row's end
    kx0 = sx1 - 52
    on = bad
    d.rounded_rectangle(r(kx0, yc - 11, kx0 + 40, yc + 11), radius=3 * S, fill=GOLD if on else (6, 46, 42), outline=GOLD if on else INK_SOFT, width=S)
    T(kx0 + 20, yc - 6, "FIX", font(BOLD, 10), INK if on else INK_SOFT, "ma")
T(fx1 + 44, sy0 + 14, "MONO dB", font(MONO, 9), INK_SOFT)
# MONO BASS line across the two lowest rows
mbY = rows_y0 + 6 * rh + 4
d.line(r(fx0 - 4, mbY, fx1 + 4, mbY), fill=GOLD, width=S); gd.line(r(fx0 - 4, mbY, fx1 + 4, mbY), fill=GOLD, width=S)
T(fc, mbY - 13, "MONO BELOW 120 Hz", font(MONO, 9), GOLD, "ma", True)

# Controls: four big knobs with a value ring, IN and MATCH.
def knob(cx, cy, label, val, frac):
    R = 36
    d.ellipse(r(cx - R - 12, cy - R - 12, cx + R + 12, cy + R + 12), fill=GROUND)
    n = 27
    for i in range(n):
        a = math.radians(225 - 270 * i / (n - 1))
        on = i / (n - 1) <= frac
        p0 = (cx + math.cos(a) * (R + 4), cy - math.sin(a) * (R + 4)); p1 = (cx + math.cos(a) * (R + 10), cy - math.sin(a) * (R + 10))
        d.line(r(*p0, *p1), fill=CREAM if on else OFFSEG, width=3 * S)
        if on: gd.line(r(*p0, *p1), fill=CREAM, width=3 * S)
    d.ellipse(r(cx - R + 4, cy - R + 4, cx + R - 4, cy + R - 4), fill=(14, 66, 60), outline=INK_SOFT, width=S)
    a = math.radians(225 - 270 * frac)
    d.line(r(cx + math.cos(a) * 10, cy - math.sin(a) * 10, cx + math.cos(a) * (R - 10), cy - math.sin(a) * (R - 10)), fill=GOLD, width=3 * S)
    T(cx, cy + R + 18, label, font(BOLD, 12), CREAM, "ma")
    d.rounded_rectangle(r(cx - 36, cy + R + 36, cx + 36, cy + R + 54), radius=3 * S, fill=GROUND)
    T(cx, cy + R + 39, val, font(MONO, 11), CREAM, "ma", True)
ky = 548
for i, (lab, v, f) in enumerate([("WIDTH", "140 %", 0.70), ("SPREAD", "35 %", 0.35), ("MONO BASS", "120 Hz", 0.52), ("OUTPUT", "0.0 dB", 0.5)]):
    knob(100 + i * 150, ky, lab, v, f)

def switch(x, y, label, on, sub=None):
    d.rounded_rectangle(r(x, y, x + 110, y + 40), radius=5 * S, fill=(6, 46, 42), outline=GOLD if on else INK_SOFT, width=S)
    d.ellipse(r(x + 12, y + 14, x + 24, y + 26), fill=GOLD if on else OFFSEG)
    if on: gd.ellipse(r(x + 12, y + 14, x + 24, y + 26), fill=GOLD)
    T(x + 34, y + 8 if sub else y + 13, label, font(BOLD, 12), CREAM)
    if sub: T(x + 34, y + 24, sub, font(MONO, 9), GOLD, "la", True)
switch(700, 506, "IN", True)
switch(700, 558, "MATCH", True, "-1.4 dB")

mark = Image.open(os.path.join(ROOT, "Assets", "mark_cream.png")).convert("RGBA")
ms = 36 * S; mark = mark.resize((ms, int(ms * mark.height / mark.width)), Image.LANCZOS)
img.paste(mark, (W - 34 * S - ms, H - 30 * S - mark.height), mark)

halo = glow.filter(ImageFilter.GaussianBlur(5 * S))
img = ImageChops.add(img, halo.point(lambda v: int(v * 0.38)))
out = os.path.join(ROOT, "docs", "panel_sketch_v2.png"); img.save(out); print(out, img.size)
