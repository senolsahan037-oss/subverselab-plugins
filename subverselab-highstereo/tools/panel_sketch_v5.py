"""HIGH STEREO panel sketch v5: bars are the controls, centred numbers, FILL drawn inside the bars, one FIX key. A digital instrument, not a device. Flat surfaces, no knobs, no bezels, no screws.
Ground: the RU-8 panel grey (sampled from public/plugins/ru8/card-bg.webp, ~#DADCD4). Ink and gold from the brand kit.
Run: python3 tools/panel_sketch_v3.py -> docs/panel_sketch_v5.png
"""
import math, os, random
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
LAUNCH = os.path.abspath(os.path.join(ROOT, "..", ".."))
GREY = (218, 220, 212)        # RU-8 panel grey
GREY_HI = (230, 232, 225)     # raised surface
GREY_LO = (200, 203, 194)     # wells, unlit segments
INK = (3, 55, 51); INK_SOFT = (73, 108, 104); GOLD = (197, 160, 89); CREAM = (242, 239, 231)

S = 2; W, H = 960 * S, 600 * S
F = os.path.join(ROOT, "Assets", "Fonts")
def font(n, px): return ImageFont.truetype(os.path.join(F, n), int(px * S))
MONO, BOLD, REG = "DejaVuSansMono-Bold.ttf", "texgyreheros-bold.otf", "texgyreheros-regular.otf"
img = Image.new("RGB", (W, H), GREY); d = ImageDraw.Draw(img)
def r(*v): return [x * S for x in v]
def T(x, y, s, f, c, a="la"): d.text((x * S, y * S), s, font=f, fill=c, anchor=a)
def rr(b, c, rad=6, outline=None, w=1): d.rounded_rectangle(r(*b), radius=rad * S, fill=c, outline=outline, width=w * S)

# Top bar
wm = Image.open(os.path.join(LAUNCH, "Assets", "Brand", "high-stereo-wordmark.png")).convert("RGB")
wm = wm.resize((96 * S, int(96 * S * wm.height / wm.width)), Image.LANCZOS); img.paste(wm, (24 * S, 18 * S))
T(132, 24, "HS-84", font(BOLD, 13), INK)
T(132, 42, "stereo image  ·  mono safety", font(REG, 11), INK_SOFT)

# LISTEN: one segmented control.
def icon(kind, cx, cy, c):
    if kind == "OFF": d.ellipse(r(cx - 6, cy - 6, cx + 6, cy + 6), outline=c, width=2 * S)
    elif kind == "MONO":
        d.rounded_rectangle(r(cx - 6, cy - 8, cx + 6, cy + 8), radius=2 * S, outline=c, width=2 * S); d.ellipse(r(cx - 3, cy - 1, cx + 3, cy + 5), outline=c, width=S)
    elif kind == "PHONE":
        d.rounded_rectangle(r(cx - 5, cy - 9, cx + 5, cy + 9), radius=2 * S, outline=c, width=2 * S); d.line(r(cx - 2, cy + 5, cx + 2, cy + 5), fill=c, width=S)
    else:
        for dx in (-7, 7): d.rounded_rectangle(r(cx + dx - 4, cy - 7, cx + dx + 4, cy + 7), radius=S, outline=c, width=2 * S)
modes = ["OFF", "MONO", "PHONE", "SPEAKERS"]; act = "PHONE"
lx0, lw = 508, 106
rr((lx0, 18, lx0 + lw * 4, 58), GREY_LO, 20)
for i, m in enumerate(modes):
    x0 = lx0 + i * lw; on = m == act
    if on: rr((x0 + 3, 21, x0 + lw - 3, 55), INK, 17)
    c = CREAM if on else INK_SOFT
    icon(m, x0 + 24, 38, c); T(x0 + 38, 32, m, font(BOLD, 11), c)
T(lx0 + lw * 4, 66, "LISTEN: PHONE  ·  monitor only, not in the bounce", font(REG, 10), GOLD, "ra")

# Two flat canvases.
cy0, cy1 = 92, 438
rr((24, cy0, 300, cy1), GREY_HI, 10)
rr((312, cy0, 936, cy1), GREY_HI, 10)

# Goniometer (particles stay: this is a digital tool).
gx, gy, gR = 162, 300, 118
d.line(r(gx - gR, gy, gx + gR, gy), fill=GREY_LO, width=S)
for a in (45, 90, 135): d.line(r(gx, gy, gx + math.cos(math.radians(a)) * gR, gy - math.sin(math.radians(a)) * gR), fill=GREY_LO, width=S)
d.arc(r(gx - gR, gy - gR, gx + gR, gy + gR), 180, 360, fill=GREY_LO, width=S)
T(gx - 92, gy - 98, "L", font(BOLD, 11), INK_SOFT, "ma"); T(gx + 92, gy - 98, "R", font(BOLD, 11), INK_SOFT, "ma"); T(gx, gy - gR - 16, "M", font(BOLD, 11), INK_SOFT, "ma")
random.seed(4)
for _ in range(1500):
    m = random.gauss(0, 0.42); s = random.gauss(0, 0.20)
    if random.random() < 0.10: s = random.gauss(0, 0.55); m = random.gauss(0, 0.12)
    px, py = gx + s * gR, gy - abs(m) * gR
    if math.hypot(px - gx, py - gy) < gR - 2:
        c = GOLD if abs(s) > abs(m) else INK
        d.ellipse((px * S - 1.6, py * S - 1.6, px * S + 1.6, py * S + 1.6), fill=c)
T(gx, cy0 + 16, "CORRELATION", font(BOLD, 10), INK_SOFT, "ma")
T(gx, cy0 + 32, "+0.42", font(MONO, 30), INK, "ma")
# correlation strip under the scope
bx0, bx1, by = 50, 274, 340
rr((bx0, by, bx1, by + 8), GREY_LO, 4)
rr(((bx0 + bx1) / 2, by, (bx0 + bx1) / 2 + (bx1 - bx0) / 2 * 0.42, by + 8), INK, 4)
d.line(r((bx0 + bx1) / 2, by - 4, (bx0 + bx1) / 2, by + 12), fill=INK_SOFT, width=S)
T(bx0, by + 14, "−1", font(REG, 10), INK_SOFT); T(bx1, by + 14, "+1", font(REG, 10), INK_SOFT, "ra")
T(gx, 392, "gold dots: out of phase", font(REG, 10), INK_SOFT, "ma")

# Stereo field: every bar is a control. Drag its ends out to widen that band, in to narrow it; double-click = 100 %.
fx0, fx1 = 420, 800; fc = (fx0 + fx1) / 2; half = (fx1 - fx0) / 2
T(fx0, cy0 + 14, "L", font(BOLD, 11), INK_SOFT); T(fx1, cy0 + 14, "R", font(BOLD, 11), INK_SOFT, "ra")
T(fc, cy0 + 14, "STEREO FIELD  ·  drag to widen", font(BOLD, 11), INK, "ma")
T(fx1 + 34, cy0 + 14, "MONO", font(BOLD, 10), INK_SOFT)
bands = ["8k+", "4k", "2k", "1k", "500", "250", "100", "<100"]
core  = [0.70, 0.64, 0.52, 1.22, 0.38, 0.28, 0.06, 0.0]      # the band's own width
fill  = [0.10, 0.10, 0.10, 0.00, 0.12, 0.10, 0.0, 0.0]       # what FILL adds (mono-exact)
pct   = [110, 105, 100, 160, 100, 90, None, None]            # band width setting
loss  = [-2.8, -2.7, -2.4, -8.6, -2.1, -1.3, -0.1, 0.0]
FILL_INK, FILL_GOLD = (128, 156, 149), (224, 199, 146)
y0, rh = cy0 + 40, 37
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
    if bad:
        rr((fx0 - 14, yc - 9, fx0 - 4, yc + 9), GOLD, 4); rr((fx1 + 4, yc - 9, fx1 + 14, yc + 9), GOLD, 4)
    if i < 6:
        ex = max(wf, 10)
        for gx_ in (fc - ex, fc + ex):
            d.ellipse(r(gx_ - 8, yc - 8, gx_ + 8, yc + 8), fill=CREAM, outline=col, width=2 * S)
        # the number sits in the middle of the bar, no box: gold on green, green on gold
        T(fc, yc - 7, f"{pct[i]}", font(MONO, 12), INK if bad else GOLD, "ma")
    T(fx1 + 34, yc - 8, f"{loss[i]:+.1f}" if loss[i] else " 0.0", font(MONO, 13), GOLD if bad else INK_SOFT)
# legend under the field
T(fc, y0 + 8 * rh - 4, "dark: the band   ·   light: FILL   ·   number: width %", font(REG, 9), INK_SOFT, "ma")
mbY = y0 + 6 * rh - 1
d.line(r(fx0, mbY, fx1, mbY), fill=GOLD, width=2 * S)
T(fx0 + 6, mbY + 2, "mono below 120 Hz", font(REG, 9), GOLD)

# Controls: flat value bars, drag or type; no knobs.
def bar(x0, y, wdt, label, val, frac, centre=None):
    T(x0, y, label, font(BOLD, 11), INK_SOFT)
    T(x0 + wdt, y - 4, val, font(MONO, 16), INK, "ra")
    rr((x0, y + 22, x0 + wdt, y + 34), GREY_LO, 6)
    if centre is None: rr((x0, y + 22, x0 + wdt * frac, y + 34), INK, 6)
    else:
        a, b = sorted((x0 + wdt * centre, x0 + wdt * frac)); rr((a, y + 22, b, y + 34), INK, 6)
        d.line(r(x0 + wdt * centre, y + 18, x0 + wdt * centre, y + 38), fill=INK_SOFT, width=S)
    d.ellipse(r(x0 + wdt * frac - 9, y + 19, x0 + wdt * frac + 9, y + 37), fill=CREAM, outline=INK, width=2 * S)
by = 470
bar(24, by, 150, "WIDTH", "140 %", 0.70, centre=0.5)
bar(196, by, 150, "FILL", "35 %", 0.35)
bar(368, by, 150, "MONO BASS", "120 Hz", 0.40)
bar(540, by, 120, "OUTPUT", "0.0 dB", 0.5, centre=0.5)

def toggle(x, y, label, on, sub=None):
    rr((x, y, x + 40, y + 22), INK if on else GREY_LO, 11)
    d.ellipse(r(x + (21 if on else 3), y + 3, x + (37 if on else 19), y + 19), fill=GOLD if on else CREAM)
    T(x + 50, y + 3, label, font(BOLD, 12), INK)
    if sub: T(x + 50, y + 22, sub, font(MONO, 10), GOLD)
toggle(836, 466, "IN", True)
toggle(836, 500, "MATCH", True, "−1.4 dB")
# The one FIX key: idle it is a grey outline; here it glows gold because a band is over.
fx_, fy_ = 690, 464
rr((fx_, fy_, fx_ + 120, fy_ + 46), GREY_HI, 23, GOLD, 3)
T(fx_ + 60, fy_ + 9, "FIX", font(BOLD, 22), GOLD, "ma")
T(fx_ + 60, fy_ + 54, "1k is over", font(REG, 11), GOLD, "ma")

# SubverseLab icon bottom right, ink version drawn from the cream mark.
mark = Image.open(os.path.join(ROOT, "Assets", "mark_cream.png")).convert("RGBA")
ms = 30 * S; mark = mark.resize((ms, int(ms * mark.height / mark.width)), Image.LANCZOS)
alpha = mark.split()[3]; inkmark = Image.new("RGBA", mark.size, INK + (255,)); inkmark.putalpha(alpha)
img.paste(inkmark, (W - 26 * S - ms, H - 22 * S - mark.height), inkmark)

out = os.path.join(ROOT, "docs", "panel_sketch_v5.png"); img.save(out); print(out, img.size)
