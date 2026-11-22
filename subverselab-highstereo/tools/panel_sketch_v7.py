"""HIGH STEREO panel sketch v7 (owner 20 Nov): no model line or subtitle, no helper text, blocks closed up,
360-degree sound-field scope (front at the top, behind at the bottom), hover summary in the bottom-left corner
(new SubverseLab rule). Shown state: pointer on WIDTH at 180 %, so BEHIND is on (gold part of the WIDTH ring = past 150 %).
Run: python3 tools/panel_sketch_v7.py -> docs/panel_sketch_v7.png
"""
import math, os, random
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
LAUNCH = os.path.abspath(os.path.join(ROOT, "..", ".."))
GREY = (218, 220, 212); GREY_HI = (230, 232, 225); GREY_LO = (200, 203, 194)
INK = (3, 55, 51); INK_SOFT = (73, 108, 104); GOLD = (197, 160, 89); CREAM = (242, 239, 231)
FILL_INK, FILL_GOLD = (128, 156, 149), (224, 199, 146)

S = 2; W, H = 960 * S, 520 * S
F = os.path.join(ROOT, "Assets", "Fonts")
def font(n, px): return ImageFont.truetype(os.path.join(F, n), int(px * S))
MONO, BOLD, REG = "DejaVuSansMono-Bold.ttf", "texgyreheros-bold.otf", "texgyreheros-regular.otf"
img = Image.new("RGB", (W, H), GREY); d = ImageDraw.Draw(img)
def r(*v): return [x * S for x in v]
def T(x, y, s, f, c, a="la"): d.text((x * S, y * S), s, font=f, fill=c, anchor=a)
def rr(b, c, rad=6, outline=None, w=1): d.rounded_rectangle(r(*b), radius=rad * S, fill=c, outline=outline, width=w * S)
def mixc(a, b, t): return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))

M_ = 14; GAP = 8   # outer margin, gap between blocks

# Top bar: wordmark only, preset bar, LISTEN.
wm = Image.open(os.path.join(LAUNCH, "Assets", "Brand", "high-stereo-wordmark.png")).convert("RGB")
wm = wm.resize((84 * S, int(84 * S * wm.height / wm.width)), Image.LANCZOS); img.paste(wm, (M_ * S, 12 * S))
px = 300
rr((px, 16, px + 24, 42), GREY_LO, 6); T(px + 12, 21, "<", font(BOLD, 13), INK, "ma")
rr((px + 28, 16, px + 168, 42), GREY_HI, 6, INK_SOFT, 1); T(px + 38, 22, "Default", font(BOLD, 12), INK); T(px + 158, 21, "v", font(BOLD, 11), INK_SOFT, "ra")
rr((px + 172, 16, px + 196, 42), GREY_LO, 6); T(px + 184, 21, ">", font(BOLD, 13), INK, "ma")
rr((px + 202, 16, px + 244, 42), INK, 6); T(px + 223, 22, "SAVE", font(BOLD, 10), CREAM, "ma")

def icon(kind, cx, cy, c):
    if kind == "STEREO":
        for dx in (-5, 5): d.ellipse(r(cx + dx - 5, cy - 5, cx + dx + 5, cy + 5), outline=c, width=2 * S)
    elif kind == "MONO":
        d.rounded_rectangle(r(cx - 6, cy - 8, cx + 6, cy + 8), radius=2 * S, outline=c, width=2 * S); d.ellipse(r(cx - 3, cy - 1, cx + 3, cy + 5), outline=c, width=S)
    elif kind == "PHONE":
        d.rounded_rectangle(r(cx - 5, cy - 9, cx + 5, cy + 9), radius=2 * S, outline=c, width=2 * S); d.line(r(cx - 2, cy + 5, cx + 2, cy + 5), fill=c, width=S)
    else:
        for dx in (-7, 7): d.rounded_rectangle(r(cx + dx - 4, cy - 7, cx + dx + 4, cy + 7), radius=S, outline=c, width=2 * S)
modes = ["STEREO", "MONO", "PHONE", "SPEAKERS"]; act = "STEREO"
lw = 91; lx0 = 960 - M_ - lw * 4
rr((lx0, 10, lx0 + lw * 4, 48), GREY_LO, 19)
for i, m in enumerate(modes):
    x0 = lx0 + i * lw; on = m == act
    if on: rr((x0 + 3, 13, x0 + lw - 3, 45), INK, 16)
    c = CREAM if on else INK_SOFT
    icon(m, x0 + 18, 29, c); T(x0 + 31, 23, m, font(BOLD, 10), c)

# Blocks, closed up: scope | field, then the control strip, all GAP apart.
top, bot = 58, 388
sx1 = M_ + 300
rr((M_, top, sx1, bot), GREY_HI, 10)
rr((sx1 + GAP, top, 960 - M_, bot), GREY_HI, 10)

# 360 scope: where each sound sits around the listener. Front (in phase) in the top half, behind (out of phase) in
# the bottom half; left/right from the level ratio. Dots behind are gold.
gx, gy, gR = (M_ + sx1) / 2, top + 178, 132
d.ellipse(r(gx - gR, gy - gR, gx + gR, gy + gR), outline=GREY_LO, width=S)
d.ellipse(r(gx - gR * 0.5, gy - gR * 0.5, gx + gR * 0.5, gy + gR * 0.5), outline=GREY_LO, width=S)
for a in range(0, 360, 45):
    d.line(r(gx, gy, gx + math.cos(math.radians(a)) * gR, gy - math.sin(math.radians(a)) * gR), fill=GREY_LO, width=S)
d.line(r(gx - gR, gy, gx + gR, gy), fill=INK_SOFT, width=S)   # the ear line: front above, behind below
T(gx - gR - 4, gy - 7, "L", font(BOLD, 11), INK_SOFT, "ra"); T(gx + gR + 4, gy - 7, "R", font(BOLD, 11), INK_SOFT, "la")
random.seed(7)
def dot(theta_deg, rad, c, sz=1.6):
    x = gx + math.sin(math.radians(theta_deg)) * rad; y = gy - math.cos(math.radians(theta_deg)) * rad
    d.ellipse((x * S - sz, y * S - sz, x * S + sz, y * S + sz), fill=c)
for _ in range(700):    # front: vocal/bass in the middle, instruments to the sides
    th = random.gauss(0, 14) if random.random() < 0.55 else random.choice((-1, 1)) * random.gauss(55, 14)
    th = max(-85, min(85, th)); dot(th, min(abs(random.gauss(0.62, 0.17)), 0.96) * gR, INK)
for _ in range(520):    # behind: the widened sides at WIDTH 180 %
    side = random.choice((-1, 1)); th = side * random.gauss(128, 20)
    dot(th, min(abs(random.gauss(0.6, 0.18)), 0.97) * gR, GOLD)
T(M_ + 14, top + 12, "CORRELATION", font(BOLD, 9), INK_SOFT)
T(M_ + 14, top + 24, "+0.31", font(MONO, 18), INK)

# Stereo field (bands are the controls).
fx0, fx1 = sx1 + GAP + 62, 960 - M_ - 74; fc = (fx0 + fx1) / 2; half = (fx1 - fx0) / 2
T(fx0, top + 12, "L", font(BOLD, 11), INK_SOFT); T(fx1, top + 12, "R", font(BOLD, 11), INK_SOFT, "ra")
T(fx1 + 30, top + 12, "MONO", font(BOLD, 10), INK_SOFT)
bands = ["8k+", "4k", "2k", "1k", "500", "250", "100", "<100"]
core = [0.82, 0.70, 0.74, 1.12, 0.55, 0.36, 0.06, 0.0]
fill = [0.08, 0.08, 0.08, 0.00, 0.10, 0.08, 0.0, 0.0]
pct = [100, 100, 100, 130, 100, 100, None, None]
loss = [-3.4, -2.9, -3.6, -8.1, -2.6, -1.6, -0.1, 0.0]
y0, rh = top + 34, 37
d.line(r(fc, y0 - 4, fc, y0 + 8 * rh), fill=GREY_LO, width=S)
for i, b in enumerate(bands):
    yc = y0 + i * rh + rh / 2; bad = core[i] > 1
    T(fx0 - 22, yc - 7, b, font(BOLD, 12), GOLD if bad else INK, "ra")
    rr((fx0, yc - 9, fx1, yc + 9), GREY_LO, 9)
    col = GOLD if bad else INK
    wc = min(core[i], 1.0) * half; wf = min(core[i] + fill[i], 1.0) * half
    if wf > wc + 1: rr((fc - wf, yc - 9, fc + wf, yc + 9), FILL_GOLD if bad else FILL_INK, 9)
    if wc > 2: rr((fc - wc, yc - 9, fc + wc, yc + 9), col, 9)
    elif i >= 6: rr((fc - 2, yc - 9, fc + 2, yc + 9), INK, 2)
    if bad: rr((fx0 - 14, yc - 9, fx0 - 4, yc + 9), GOLD, 4); rr((fx1 + 4, yc - 9, fx1 + 14, yc + 9), GOLD, 4)
    if i < 6:
        ex = max(wf, 10)
        for gx_ in (fc - ex, fc + ex): d.ellipse(r(gx_ - 8, yc - 8, gx_ + 8, yc + 8), fill=CREAM, outline=col, width=2 * S)
        T(fc, yc - 7, f"{pct[i]}", font(MONO, 12), INK if bad else GOLD, "ma")
    T(fx1 + 30, yc - 8, f"{loss[i]:+.1f}" if loss[i] else " 0.0", font(MONO, 13), GOLD if bad else INK_SOFT)
mbY = y0 + 6 * rh - 1
d.line(r(fx0, mbY, fx1, mbY), fill=GOLD, width=2 * S)

# Control strip directly under the blocks.
ct, cb = bot + GAP, H / S - M_
rr((M_, ct, 960 - M_, cb), GREY_HI, 10)
def round_ctrl(cx, cy, name, val, e, frac, gold_from=None, hover=False):
    r0 = 30; grow = 13 * e
    for k, t in ((3, 0.10), (2, 0.18), (1, 0.30)):
        if e > 0.02:
            rad = r0 + grow * (0.55 + 0.15 * k)
            d.ellipse(r(cx - rad, cy - rad, cx + rad, cy + rad), outline=mixc(GREY_HI, INK, t), width=S)
    if e > 0.02:
        rad = r0 + grow * 0.5; d.ellipse(r(cx - rad, cy - rad, cx + rad, cy + rad), fill=mixc(GREY_HI, INK, 0.16))
    d.ellipse(r(cx - r0, cy - r0, cx + r0, cy + r0), fill=CREAM if hover else GREY_HI, outline=INK, width=(3 if hover else 2) * S)
    if frac > 0.001:
        box = r(cx - r0 + 3, cy - r0 + 3, cx + r0 - 3, cy + r0 - 3)
        if gold_from is not None and frac > gold_from:
            d.arc(box, -90, -90 + 360 * gold_from, fill=INK_SOFT, width=3 * S)
            d.arc(box, -90 + 360 * gold_from, -90 + 360 * frac, fill=GOLD, width=3 * S)
        else:
            d.arc(box, -90, -90 + 360 * frac, fill=GOLD, width=3 * S)
    T(cx, cy - 14, name, font(BOLD, 7 if len(name) > 6 else 10), INK_SOFT, "ma")
    T(cx, cy - 1, val, font(MONO, 13), INK, "ma")
ky = ct + 44
round_ctrl(70, ky, "WIDTH", "180%", 0.62, 0.90, gold_from=0.75, hover=True)   # gold past 150 % = BEHIND
round_ctrl(170, ky, "SPREAD", "20%", 0.2, 0.20)
round_ctrl(270, ky, "MONO BASS", "120", 0.33, 0.33)
round_ctrl(370, ky, "OUTPUT", "0.0", 0.0, 0.5)
round_ctrl(470, ky, "MIX", "100%", 1.0, 1.0)

# IN, MATCH and FIX are the same switch (owner 20 Nov): click -> the knob slides right = on, click -> slides left = off.
# FIX only adds: while it is off and a band is over, its label turns gold.
def toggle(x, y, label, on, sub=None, warn=False):
    rr((x, y, x + 38, y + 21), INK if on else GREY_LO, 10)
    d.ellipse(r(x + (20 if on else 3), y + 3, x + (35 if on else 18), y + 18), fill=GOLD if on else CREAM)
    T(x + 46, y + 3, label, font(BOLD, 12), GOLD if warn else INK)
    if sub: T(x + 46, y + 22, sub, font(MONO, 10), GOLD)
# Side by side, on the knobs' own 100 px step (owner 20 Nov): unit centres 570 / 670 / 770, after MIX at 470.
def toggle_c(cx, y, label, on, sub=None, warn=False):
    unit = 38 + 8 + font(BOLD, 12).getlength(label) / S
    toggle(cx - unit / 2, y, label, on, sub, warn)
ty = ky - 22
toggle_c(570, ty, "IN", True)
toggle_c(670, ty, "MATCH", True, "−1.1 dB")
toggle_c(770, ty, "FIX", False, warn=True)   # off, 1k is over -> gold label
# Phase (correlation) bar, centred under the switch row: -1 .. +1, filled from the centre to the reading.
pc, pw, py_ = 670, 230, ty + 44
corr_v = 0.31
rr((pc - pw / 2, py_, pc + pw / 2, py_ + 8), GREY_LO, 4)
if corr_v >= 0: rr((pc, py_, pc + pw / 2 * corr_v, py_ + 8), INK, 4)
else: rr((pc + pw / 2 * corr_v, py_, pc, py_ + 8), GOLD, 4)
d.line(r(pc, py_ - 4, pc, py_ + 12), fill=INK_SOFT, width=S)
T(pc - pw / 2 - 6, py_ - 3, "−1", font(REG, 10), INK_SOFT, "ra"); T(pc + pw / 2 + 6, py_ - 3, "+1", font(REG, 10), INK_SOFT)

# Hover summary, bottom-left corner (SubverseLab rule, 20 Nov): what the control under the pointer does, with its value.
T(M_ + 14, cb - 17, "WIDTH 180 %  —  widens the sides; past 150 % they move behind you. Mono stays the same.", font(REG, 11), INK)

# SubverseLab icon bottom right.
mark = Image.open(os.path.join(ROOT, "Assets", "mark_cream.png")).convert("RGBA")
ms = 28 * S; mark = mark.resize((ms, int(ms * mark.height / mark.width)), Image.LANCZOS)
alpha = mark.split()[3]; inkmark = Image.new("RGBA", mark.size, INK + (255,)); inkmark.putalpha(alpha)
img.paste(inkmark, (W - (M_ + 12) * S - ms, int((cb - 10) * S) - mark.height), inkmark)

out = os.path.join(ROOT, "docs", "panel_sketch_v7.png"); img.save(out); print(out, img.size)

# States strip: the one switch, off / on, and FIX off with a band over.
img = Image.new("RGB", (330 * S, 70 * S), GREY_HI); d = ImageDraw.Draw(img)
toggle(24, 24, "FIX", False); toggle(124, 24, "FIX", True); toggle(224, 24, "FIX", False, warn=True)
out3 = os.path.join(ROOT, "docs", "panel_sketch_v7_fix.png"); img.save(out3); print(out3)
