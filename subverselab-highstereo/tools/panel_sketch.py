"""HIGH STEREO panel sketch for the owner (not the coded panel). Colours from Assets/Brand/_kit/brand.py only.
Run: python3 tools/panel_sketch.py  ->  docs/panel_sketch_v1.png
"""
import math, os
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LAUNCH = os.path.abspath(os.path.join(ROOT, "..", ".."))

CREAM = (242, 239, 231); INK = (3, 55, 51); GOLD = (197, 160, 89)
INK_SOFT = (73, 108, 104); GROUND = (3, 40, 37)
GLOW_DIM = (40, 78, 70)

S = 2                       # draw at 2x, panel is 900 x 610
W, H = 900 * S, 610 * S
F = os.path.join(ROOT, "Assets", "Fonts")
def font(name, px): return ImageFont.truetype(os.path.join(F, name), px * S)
MONO = "DejaVuSansMono-Bold.ttf"; BOLD = "texgyreheros-bold.otf"; REG = "texgyreheros-regular.otf"

img = Image.new("RGB", (W, H), INK)
d = ImageDraw.Draw(img)
glow = Image.new("RGB", (W, H), (0, 0, 0))   # what glows goes here too, blurred and added at the end
gd = ImageDraw.Draw(glow)

def r(x0, y0, x1, y1): return [x0 * S, y0 * S, x1 * S, y1 * S]
def text(x, y, s, f, fill, anchor="la", glowing=False):
    d.text((x * S, y * S), s, font=f, fill=fill, anchor=anchor)
    if glowing: gd.text((x * S, y * S), s, font=f, fill=fill, anchor=anchor)

# Plate: thin cream rule inset, gold hairline under the header.
d.rounded_rectangle(r(10, 10, 890, 600), radius=14 * S, outline=INK_SOFT, width=S)

# Header: the owner's wordmark top left, model under it.
wm = Image.open(os.path.join(LAUNCH, "Assets", "Brand", "high-stereo-wordmark.png")).convert("RGB")
wm = wm.resize((int(130 * S), int(130 * S * wm.height / wm.width)), Image.LANCZOS)
img.paste(wm, (28 * S, 26 * S))
text(28, 26 + 130 * 380 / 783 + 6, "MODEL HS-84", font(BOLD, 11), GOLD)

# LISTEN selector, top right: VFD words, the active one lit.
text(862, 30, "LISTEN", font(BOLD, 10), INK_SOFT, "ra")
modes = ["OFF", "MONO", "PHONE", "SPEAKERS"]; active = "PHONE"
x = 862
for m in reversed(modes):
    f = font(MONO, 15)
    wpx = d.textlength(m, font=f) / S
    x -= wpx
    lit = m == active
    d.rounded_rectangle(r(x - 9, 46, x + wpx + 9, 72), radius=4 * S, fill=GROUND, outline=GOLD if lit else GLOW_DIM, width=S)
    text(x, 52, m, f, GOLD if lit else GLOW_DIM, glowing=lit)
    x -= 26
text(862, 80, "MONITOR ONLY  -  NOT IN YOUR BOUNCE", font(MONO, 9), GOLD, "ra", glowing=True)

# The screen.
sx0, sy0, sx1, sy1 = 28, 112, 872, 410
d.rounded_rectangle(r(sx0, sy0, sx1, sy1), radius=8 * S, fill=GROUND, outline=(1, 25, 23), width=3 * S)
for gx in range(sx0 + 12, sx1, 12):
    for gy in range(sy0 + 12, sy1, 12):
        d.point((gx * S, gy * S), fill=(10, 52, 48))
text(sx0 + 18, sy0 + 12, "PHASE MAP", font(MONO, 11), CREAM, glowing=True)
text(sx1 - 18, sy0 + 12, "MONO LOSS PER BAND", font(MONO, 9), INK_SOFT, "ra")

bands = ["<100", "100", "250", "500", "1k", "2k", "4k", "8k+"]
corr  = [0.95, 0.80, 0.55, 0.30, -0.45, 0.20, 0.10, 0.05]
loss  = [-0.1, -0.5, -1.3, -2.1, -8.6, -2.4, -2.7, -2.8]
fix   = [False, False, False, False, True, False, False, False]
colw = (sx1 - sx0 - 36) / 8
top, mid, bot = sy0 + 40, sy0 + 40 + 70, sy0 + 40 + 140
for k in range(8):
    cx = sx0 + 18 + colw * k + colw / 2
    bad = corr[k] < 0
    col = GOLD if bad else CREAM
    # Segment column: 14 segments from -1 (bottom) to +1 (top); filled from the centre line to the value.
    for i in range(14):
        v = 1 - (i + 0.5) / 7          # +0.93 .. -0.93
        y = top + i * 10
        on = (0 < v <= corr[k]) or (corr[k] <= v < 0)
        box = r(cx - 26, y + 1, cx + 26, y + 8)
        d.rectangle(box, fill=col if on else (12, 56, 51))
        if on: gd.rectangle(box, fill=col)
    d.line(r(cx - 32, mid, cx + 32, mid), fill=INK_SOFT, width=S)
    text(cx, bot + 8, bands[k], font(MONO, 11), CREAM if not bad else GOLD, "ma", glowing=True)
    text(cx, bot + 26, f"{loss[k]:+.1f}", font(MONO, 13), col, "ma", glowing=True)
    # FIX key under each column, on the panel just below the screen edge
    fx0, fy0 = cx - 24, sy1 + 10
    d.rounded_rectangle(r(fx0, fy0, fx0 + 48, fy0 + 20), radius=3 * S, fill=GOLD if fix[k] else (6, 46, 42),
                        outline=GOLD if (fix[k] or bad) else INK_SOFT, width=S)
    text(cx, fy0 + 5, "FIX", font(BOLD, 10), INK if fix[k] else (GOLD if bad else INK_SOFT), "ma")

# Correlation strip and MATCH read-out along the screen's lower edge.
cy = sy1 - 26
text(sx0 + 18, cy - 6, "CORR", font(MONO, 10), INK_SOFT)
bx0, bx1 = sx0 + 70, sx0 + 470
for i in range(40):
    v = -1 + (i + 0.5) / 20
    x = bx0 + i * (bx1 - bx0) / 40
    on = 0 <= v <= 0.42
    box = r(x, cy - 6, x + (bx1 - bx0) / 40 - 2, cy + 6)
    d.rectangle(box, fill=CREAM if on else (12, 56, 51))
    if on: gd.rectangle(box, fill=CREAM)
text(bx0, cy + 9, "-1", font(MONO, 8), INK_SOFT, "ma"); text((bx0 + bx1) / 2, cy + 9, "0", font(MONO, 8), INK_SOFT, "ma"); text(bx1, cy + 9, "+1", font(MONO, 8), INK_SOFT, "ma")
text(bx1 + 30, cy - 8, "+0.42", font(MONO, 15), CREAM, glowing=True)
text(sx1 - 18, cy - 8, "MATCH  -1.4 dB", font(MONO, 15), CREAM, "ra", glowing=True)

# Controls row.
def knob(cx, cy, label, value, frac):
    R = 30
    d.ellipse(r(cx - R - 4, cy - R - 4, cx + R + 4, cy + R + 4), fill=(2, 33, 31))
    d.ellipse(r(cx - R, cy - R, cx + R, cy + R), fill=(14, 66, 60), outline=INK_SOFT, width=S)
    for i in range(11):
        a = math.radians(225 - 270 * i / 10)
        d.line(r(cx + math.cos(a) * (R + 8), cy - math.sin(a) * (R + 8), cx + math.cos(a) * (R + 12), cy - math.sin(a) * (R + 12)),
               fill=INK_SOFT, width=S)
    a = math.radians(225 - 270 * frac)
    d.line(r(cx + math.cos(a) * 8, cy - math.sin(a) * 8, cx + math.cos(a) * (R - 5), cy - math.sin(a) * (R - 5)), fill=GOLD, width=3 * S)
    text(cx, cy + R + 16, label, font(BOLD, 11), CREAM, "ma")
    d.rounded_rectangle(r(cx - 34, cy + R + 32, cx + 34, cy + R + 50), radius=3 * S, fill=GROUND)
    text(cx, cy + R + 35, value, font(MONO, 11), CREAM, "ma", glowing=True)

ky = 492
knob(90, ky, "WIDTH", "140 %", 0.70)
knob(220, ky, "SPREAD", "35 %", 0.35)
knob(350, ky, "MONO BASS", "120 Hz", 0.52)
knob(480, ky, "OUTPUT", "0.0 dB", 0.5)

def switch(x, y, label, on):
    d.rounded_rectangle(r(x, y, x + 70, y + 30), radius=4 * S, fill=(6, 46, 42), outline=INK_SOFT, width=S)
    d.ellipse(r(x + 10, y + 10, x + 20, y + 20), fill=GOLD if on else (12, 56, 51))
    if on: gd.ellipse(r(x + 10, y + 10, x + 20, y + 20), fill=GOLD)
    text(x + 28, y + 9, label, font(BOLD, 11), CREAM)
switch(600, 470, "IN", True)
switch(690, 470, "MATCH", True)

# SubverseLab icon, bottom right (links to the manual in the real panel).
mark = Image.open(os.path.join(ROOT, "Assets", "mark_cream.png")).convert("RGBA")
ms = 34 * S
mark = mark.resize((ms, int(ms * mark.height / mark.width)), Image.LANCZOS)
img.paste(mark, (W - 30 * S - ms, H - 26 * S - mark.height), mark)

# VFD glow.
halo = glow.filter(ImageFilter.GaussianBlur(5 * S))
img = Image.blend(img, Image.composite(halo, img, halo.convert("L").point(lambda v: 255 if v > 0 else 0)), 0.0)
from PIL import ImageChops
img = ImageChops.add(img, halo.point(lambda v: int(v * 0.35)))

out = os.path.join(ROOT, "docs", "panel_sketch_v1.png")
os.makedirs(os.path.dirname(out), exist_ok=True)
img.resize((W // S * 2 // 2, H // S * 2 // 2), Image.LANCZOS).save(out) if False else img.save(out)
print(out, img.size)
