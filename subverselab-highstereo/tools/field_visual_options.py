"""STEREO FIELD without bars (owner 20 Nov): two proposals, drawn from one real moment of the owner's track.
Fixed: frequency labels (never change colour), grips (draggable, at the setting), the number = the band's stereo %.
Live: the light (width + level), the loss column. The column header is the LISTEN mode (STEREO reads as MONO).
A "light ribbons": each band is a soft ribbon of light from the centre, no track.
B "field shape": the bands join into one smooth shape, its edges are the live widths, filled with light.
Usage: python3 tools/field_visual_options.py frames.csv frame_index listen_label out.png
"""
import os, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter

csv, fi, listen, out = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
GREY = (218, 220, 212); GREY_HI = (230, 232, 225); GREY_LO = (200, 203, 194)
INK = (3, 55, 51); INK_SOFT = (73, 108, 104); GOLD = (197, 160, 89); CREAM = (242, 239, 231); LIGHT = (252, 250, 244)
F = os.path.join(ROOT, "Assets", "Fonts"); S = 2
def font(n, px): return ImageFont.truetype(os.path.join(F, n), int(px * S))
MONO, BOLD = "DejaVuSansMono-Bold.ttf", "texgyreheros-bold.otf"
def mixc(a, b, t): return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))

a = np.loadtxt(csv, delimiter=",")
row = a[fi]; wid = np.maximum(0, row[0:16:2]); lev = row[1:16:2]
loss = [10 * np.log10(max(1e-6, 1 - w / 2)) if w < 2 else -60.0 for w in wid]   # MONO column (STEREO reads as MONO)
names = ["8k+", "4k", "2k", "1k", "500", "250", "100", "<100"]; order = list(range(7, -1, -1))
setting = [100] * 8

def light_colour(w):
    if w <= 1: return mixc(INK_SOFT, LIGHT, float(w))
    return mixc(LIGHT, GOLD, float(min(1.0, (w - 1) * 2)))

W, H = 640, 352
def base(title):
    img = Image.new("RGB", (W * S, H * S), GREY); d = ImageDraw.Draw(img)
    d.rounded_rectangle([8 * S, 8 * S, (W - 8) * S, (H - 8) * S], radius=10 * S, fill=GREY_HI)
    return img, d

def labels(d, fx0, fx1, y0, rh, header):
    T = lambda x, y, s, f, c, an="la": d.text((x * S, y * S), s, font=f, fill=c, anchor=an)
    T(fx0, 18, "L", font(BOLD, 11), INK_SOFT); T(fx1, 18, "R", font(BOLD, 11), INK_SOFT, "ra")
    T(fx1 + 26, 18, header, font(BOLD, 10), INK_SOFT)
    for r_, k in enumerate(order):
        yc = y0 + r_ * rh + rh / 2
        T(fx0 - 16, yc - 7, names[r_], font(BOLD, 12), INK, "ra")                       # fixed, never changes colour
        T(fx1 + 26, yc - 8, f"{loss[k]:+.1f}", font(MONO, 13), GOLD if wid[k] > 1 else INK_SOFT)

def grips_and_numbers(d, fc, half, y0, rh):
    for r_, k in enumerate(order[:6]):
        yc = y0 + r_ * rh + rh / 2; sh = setting[k] / 200 * half
        for gx in (fc - sh, fc + sh):
            d.ellipse([(gx - 7) * S, (yc - 7) * S, (gx + 7) * S, (yc + 7) * S], fill=CREAM, outline=INK, width=2 * S)
        d.text((fc * S, (yc - 7) * S), f"{setting[k]}", font=font(MONO, 12), fill=INK, anchor="ma")

def option_a():
    img, d = base("A")
    fx0, fx1 = 70, W - 92; fc = (fx0 + fx1) / 2; half = (fx1 - fx0) / 2; y0, rh = 40, 37
    tint = Image.new("RGB", img.size, GREY_HI); td = ImageDraw.Draw(tint)
    glow = Image.new("L", img.size, 0); gd = ImageDraw.Draw(glow)
    core = Image.new("L", img.size, 0); cd = ImageDraw.Draw(core)
    for r_, k in enumerate(order):
        yc = y0 + r_ * rh + rh / 2; hw = max(6, min(1.0, np.sqrt(wid[k] / 2)) * half)
        st = float(np.clip((lev[k] + 48) / 32, 0, 1))
        td.rectangle([0, (yc - rh / 2) * S, W * S, (yc + rh / 2) * S], fill=light_colour(wid[k]))
        gd.ellipse([(fc - hw) * S, (yc - 11) * S, (fc + hw) * S, (yc + 11) * S], fill=int(255 * (0.3 + 0.7 * st)))
        cd.ellipse([(fc - hw * 0.8) * S, (yc - 3) * S, (fc + hw * 0.8) * S, (yc + 3) * S], fill=int(255 * (0.2 + 0.8 * st)))
    img = Image.composite(tint, img, glow.filter(ImageFilter.GaussianBlur(6 * S)))
    img = Image.composite(tint, img, core.filter(ImageFilter.GaussianBlur(2 * S)))
    d = ImageDraw.Draw(img)
    d.line([fc * S, (y0 - 4) * S, fc * S, (y0 + 8 * rh) * S], fill=GREY_LO, width=S)
    grips_and_numbers(d, fc, half, y0, rh); labels(d, fx0, fx1, y0, rh, listen)
    return img

def option_b():
    img, d = base("B")
    fx0, fx1 = 70, W - 92; fc = (fx0 + fx1) / 2; half = (fx1 - fx0) / 2; y0, rh = 40, 37
    ys = np.array([y0 + r_ * rh + rh / 2 for r_ in range(8)])
    hws = np.array([max(5, min(1.0, np.sqrt(wid[k] / 2)) * half) for k in order])
    yy = np.linspace(ys[0] - 10, ys[-1] + 10, 260)
    hh = np.interp(yy, ys, hws)
    kern = np.hanning(25); kern /= kern.sum(); hh = np.convolve(np.pad(hh, 12, mode="edge"), kern, mode="valid")   # smooth outline
    # colour per height from the nearest band; one smooth filled shape, lit, plus a crisp thin edge line
    tint = Image.new("RGB", img.size, GREY_HI); td = ImageDraw.Draw(tint)
    cols = np.array([light_colour(wid[k]) for k in order], dtype=float)
    for y_, h_ in zip(yy, hh):   # colour blended between neighbouring bands, no steps
        c = tuple(int(np.interp(y_, ys, cols[:, j])) for j in range(3))
        td.line([0, y_ * S, W * S, y_ * S], fill=c, width=2 * S)
    mask = Image.new("L", img.size, 0); md = ImageDraw.Draw(mask)
    poly = [((fc - h_) * S, y_ * S) for y_, h_ in zip(yy, hh)] + [((fc + h_) * S, y_ * S) for y_, h_ in zip(yy[::-1], hh[::-1])]
    md.polygon(poly, fill=235)
    img = Image.composite(tint, img, mask.filter(ImageFilter.GaussianBlur(5 * S)))
    d = ImageDraw.Draw(img)
    for side in (-1, 1):
        pts = [((fc + side * h_) * S, y_ * S) for y_, h_ in zip(yy, hh)]
        d.line(pts, fill=mixc(GREY_HI, INK_SOFT, 0.55), width=S)
    d.line([fc * S, (y0 - 4) * S, fc * S, (y0 + 8 * rh) * S], fill=GREY_LO, width=S)
    for r_ in range(8):   # faint row guides so a grip has a lane
        yc = ys[r_]; d.line([fx0 * S, yc * S, fx1 * S, yc * S], fill=mixc(GREY_HI, GREY_LO, 0.6), width=S)
    grips_and_numbers(d, fc, half, y0, rh); labels(d, fx0, fx1, y0, rh, listen)
    return img

A, B = option_a(), option_b()
o = Image.new("RGB", (W * S * 2 + 20 * S, H * S + 40 * S), GREY); od = ImageDraw.Draw(o)
od.text((16 * S, 8 * S), "A  light ribbons", font=font(BOLD, 15), fill=INK)
od.text(((W + 26) * S, 8 * S), "B  field shape", font=font(BOLD, 15), fill=INK)
o.paste(A, (0, 40 * S)); o.paste(B, ((W + 20) * S, 40 * S))
o = o.resize((o.width // 2, o.height // 2), Image.LANCZOS); o.save(out); print(out)
