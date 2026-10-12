#!/usr/bin/env python3
"""
KORO, Model KR-77 -- panel concepts (Pillow + numpy only, no network).

    python3 tools/concepts.py            # writes everything into docs/concepts/

Follows PRODUCT_BRIEF.md incl. the owner REVISION of 2026-09-30 ("drop it on the vocal, done"):
front = DOUBLE, WIDTH, TONE, HARMONY, COLOR, OUTPUT + LOCK, BYPASS + 6 MOOD keys; everything
else lives on a rear plate. Concept A (KUBBE family, anodised green) in two display variants.

Outputs (docs/concepts/):
    koro_A1_front_vfd.png          1600x640  VFD key display
    koro_A2_front_label7seg.png    1600x640  same face, KASET-style cream label window + red 7-segment
    koro_A1_hero_4x5.png           1080x1350 Instagram crop of A1
    koro_A2_hero_4x5.png           1080x1350 Instagram crop of A2
    koro_rear_plate.png            1200x440  advanced controls (rear plate)
    concepts_contact.png           KUBBE | KASET | KORO A1 | KORO A2 at the same face height

Faces are drawn in base units and rendered at K=4.22 (supersampled), then reduced: 1600x640
for the face (KUBBE's 2240x880 face has the same 2.5:1 proportion), and a 514-unit slice of
the full height at 2.11x for the 4:5 hero (no upscaling).

Family parts are re-drawn from subverselab-reverb/Source/PluginEditor.cpp (machined knob,
bakelite pointer knob, toggles, place buttons, section brackets, stroked Microgramma-style
product name) and Theme.h (palette). Fonts: Helvetica Neue, Menlo (macOS), DIN Alternate
(macOS Supplemental; needs a licence on Windows).
"""

import math
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
KORO = os.path.abspath(os.path.join(HERE, ".."))
OUT = os.path.join(KORO, "docs", "concepts")
TOOLS = os.path.abspath(os.path.join(KORO, ".."))
LOGO = os.path.abspath(os.path.join(TOOLS, "..", "Assets", "Brand", "logo"))
KUBBE_FACE = os.path.join(TOOLS, "subverselab-reverb", "docs", "manual", "images", "kubbe_typo_v2.png")
KASET_FACE = os.path.join(TOOLS, "subverselab-kaset", "docs", "manual", "images", "kaset_ui_v5.png")

W, H = 1600, 640
K = 4.22

# ---- palette (Theme.h / Assets/Brand/_kit/brand.py) ----------------------------------------
CREAM = (242, 239, 231)
INK = (3, 55, 51)
GOLD = (197, 160, 89)
GOLD_L = (223, 191, 122)
GOLD_D = (140, 111, 53)
LAMP = (246, 213, 142)
VFD = (99, 242, 216)
VFD_OFF = (14, 42, 39)
SCREEN = (2, 21, 18)
MUTED = (143, 170, 165)
CAP_CREAM = (230, 223, 204)
LED_RED = (255, 64, 40)
LED_RED_OFF = (70, 14, 10)

FONTS = {
    "legend": ("/System/Library/Fonts/HelveticaNeue.ttc", 1),     # Helvetica Neue Bold
    "fine": ("/System/Library/Fonts/HelveticaNeue.ttc", 10),      # Helvetica Neue Medium
    "cond": ("/System/Library/Fonts/HelveticaNeue.ttc", 4),       # Helvetica Neue Condensed Bold
    "din": ("/System/Library/Fonts/Supplemental/DIN Alternate Bold.ttf", 0),
    "dinc": ("/System/Library/Fonts/Supplemental/DIN Condensed Bold.ttf", 0),
    "mono": ("/System/Library/Fonts/Menlo.ttc", 1),
}

# The state every render shows: preset KOR, B Hicaz, harmony on, 2 doubler voices.
# The state every render shows: MOOD "KOR", detected B Hicaz at 3 of 4 confidence, COLOR on the
# high third; the singer is on F#4 (+6 cents), the harmony voice lands on A#4 (Hicaz 1088 cents).
STATE = dict(mood="CHOIR", key="B", scale="HICAZ", conf_dots=3, color="HIGH 3RD",
             pitch_cents=708, pitch_note="F#4", pitch_dev=+6, harm_cents=1088, harm_note="A#4")

MOODS = ["TIGHT DOUBLE", "ARABESK CHOIR", "BACKING STACK", "THIRD", "FIFTH", "CHOIR"]
DETECT_SCALES = ["MAJOR", "MINOR", "NIHAVENT", "HICAZ", "RAST"]
KEYS12 = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


def polar(cx, cy, r, deg):
    a = math.radians(deg)
    return cx + r * math.sin(a), cy - r * math.cos(a)


# =============================================================================================
class Face:
    """A drawing surface in base units (1600x640), rendered at K."""

    def __init__(self, theme, w=W, h=H):
        self.t, self.w, self.h = theme, w, h
        self.img = Image.new("RGB", (round(w * K), round(h * K)), (0, 0, 0))
        self.d = ImageDraw.Draw(self.img, "RGBA")
        self._fc = {}

    # ---- basics ---------------------------------------------------------------------------
    def font(self, key, size):
        k = (key, size)
        if k not in self._fc:
            path, idx = FONTS[key]
            self._fc[k] = ImageFont.truetype(path, max(1, round(size * K)), index=idx)
        return self._fc[k]

    def P(self, *v):
        return [x * K for x in v]

    def line(self, p0, p1, width, fill):
        self.d.line([p0[0] * K, p0[1] * K, p1[0] * K, p1[1] * K], fill=fill, width=max(1, round(width * K)))

    def poly(self, pts, fill):
        self.d.polygon([(x * K, y * K) for x, y in pts], fill=fill)

    def rrect(self, box, r, fill=None, outline=None, width=1):
        self.d.rounded_rectangle(self.P(*box), radius=r * K, fill=fill, outline=outline,
                                 width=max(1, round(width * K)))

    def ellipse(self, box, fill=None, outline=None, width=1):
        self.d.ellipse(self.P(*box), fill=fill, outline=outline, width=max(1, round(width * K)))

    def text(self, x, y, s, fkey, size, fill, anchor="mm", track=0.0, engrave=None):
        if engrave:
            self.text(x + 0.6, y + 0.7, s, fkey, size, engrave, anchor, track)
        f = self.font(fkey, size)
        if not track:
            self.d.text((x * K, y * K), s, font=f, fill=fill, anchor=anchor)
            return self.d.textlength(s, font=f) / K
        ws = [self.d.textlength(ch, font=f) for ch in s]
        tot = sum(ws) + track * K * (len(s) - 1)
        x0 = x * K - (tot / 2 if anchor[0] == "m" else tot if anchor[0] == "r" else 0)
        for ch, w_ in zip(s, ws):
            self.d.text((x0, y * K), ch, font=f, fill=fill, anchor="l" + anchor[1])
            x0 += w_ + track * K
        return tot / K

    def tlen(self, s, fkey, size, track=0.0):
        f = self.font(fkey, size)
        return (sum(self.d.textlength(c, font=f) for c in s) + track * K * (len(s) - 1)) / K if track else \
            self.d.textlength(s, font=f) / K

    def lt(self, x, y, s, size=10, track=2.0, key="legend", anchor="mm", col=None):
        """Panel legend in the theme's silkscreen colour."""
        return self.text(x, y, s, key, size, col or self.t["legend"], anchor, track, self.t.get("engrave"))

    # ---- soft light and shadow -------------------------------------------------------------
    def soft(self, box, blur, colour=(0, 0, 0), opacity=0.5, offset=(0, 0), shape="ellipse", radius=0):
        x0, y0, x1, y1 = box
        m = blur * 3
        w, h = int((x1 - x0 + 2 * m) * K), int((y1 - y0 + 2 * m) * K)
        mask = Image.new("L", (w, h), 0)
        md = ImageDraw.Draw(mask)
        b = [m * K, m * K, (x1 - x0 + m) * K, (y1 - y0 + m) * K]
        if shape == "ellipse":
            md.ellipse(b, fill=int(255 * opacity))
        else:
            md.rounded_rectangle(b, radius=radius * K, fill=int(255 * opacity))
        mask = mask.filter(ImageFilter.GaussianBlur(blur * K))
        self.img.paste(Image.new("RGB", (w, h), colour),
                       (int((x0 - m + offset[0]) * K), int((y0 - m + offset[1]) * K)), mask)

    def grad(self, box, c0, c1, angle=90, shape="ellipse", radius=0):
        x0, y0, x1, y1 = [v * K for v in box]
        w, h = max(1, int(round(x1 - x0))), max(1, int(round(y1 - y0)))
        yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
        a = math.radians(angle)
        proj = (xx / w - 0.5) * math.cos(a) + (yy / h - 0.5) * math.sin(a)
        t = ((proj - proj.min()) / (proj.max() - proj.min() + 1e-6))[..., None]
        arr = np.array(c0, np.float32) * (1 - t) + np.array(c1, np.float32) * t
        mask = Image.new("L", (w, h), 0)
        md = ImageDraw.Draw(mask)
        if shape == "ellipse":
            md.ellipse([0, 0, w - 1, h - 1], fill=255)
        else:
            md.rounded_rectangle([0, 0, w - 1, h - 1], radius=radius * K, fill=255)
        self.img.paste(Image.fromarray(arr.astype(np.uint8), "RGB"), (int(round(x0)), int(round(y0))), mask)

    def glow_text(self, x, y, s, fkey, size, col, anchor="mm", blur=3.0, strength=0.9):
        f = self.font(fkey, size)
        bb = [int(v) for v in self.d.textbbox((x * K, y * K), s, font=f, anchor=anchor)]
        m = int(blur * K * 3)
        w, h = bb[2] - bb[0] + 2 * m, bb[3] - bb[1] + 2 * m
        mask = Image.new("L", (w, h), 0)
        ImageDraw.Draw(mask).text((x * K - bb[0] + m, y * K - bb[1] + m), s, font=f, fill=int(255 * strength),
                                  anchor=anchor)
        mask = mask.filter(ImageFilter.GaussianBlur(blur * K))
        self.img.paste(Image.new("RGB", (w, h), col), (bb[0] - m, bb[1] - m), mask)
        self.d.text((x * K, y * K), s, font=f, fill=col, anchor=anchor)

    def glow_polys(self, polys, col, blur=2.5, strength=0.8, box=None):
        xs = [p[0] for pl in polys for p in pl]
        ys = [p[1] for pl in polys for p in pl]
        m = blur * 3
        x0, y0 = min(xs) - m, min(ys) - m
        w, h = int((max(xs) - x0 + m) * K), int((max(ys) - y0 + m) * K)
        mask = Image.new("L", (w, h), 0)
        md = ImageDraw.Draw(mask)
        for pl in polys:
            md.polygon([((px - x0) * K, (py - y0) * K) for px, py in pl], fill=int(255 * strength))
        mask = mask.filter(ImageFilter.GaussianBlur(blur * K))
        self.img.paste(Image.new("RGB", (w, h), col), (int(x0 * K), int(y0 * K)), mask)
        for pl in polys:
            self.poly(pl, col)

    def logo(self, name, x, y, h, anchor="lm"):
        im = Image.open(os.path.join(LOGO, name)).convert("RGBA")
        hh = int(h * K)
        im = im.resize((max(1, int(im.width * hh / im.height)), hh), Image.LANCZOS)
        px = x * K - (im.width if anchor[0] == "r" else im.width / 2 if anchor[0] == "m" else 0)
        py = y * K - hh / 2
        self.img.paste(im.convert("RGB"), (int(px), int(py)), im.split()[3])
        return im.width / K

    # ---- outputs --------------------------------------------------------------------------
    def face(self):
        return self.img.resize((self.w, self.h), Image.LANCZOS)

    def hero(self, x0):
        """4:5 crop: a 514-unit-wide slice of the full face height at 2.11x."""
        sw = 1080 / (1350 / H)
        box = (int(x0 * K), 0, int((x0 + sw) * K), self.img.height)
        return self.img.crop(box).resize((1080, 1350), Image.LANCZOS)


# =============================================================================================
# Plate
def plate(f, top, bottom, streak=8.0, grain=3.0, seed=77, brushed=True):
    Wp, Hp = f.img.size
    rng = np.random.default_rng(seed)
    y = np.linspace(0, 1, Hp, dtype=np.float32)[:, None, None]
    base = np.array(top, np.float32) * (1 - y) + np.array(bottom, np.float32) * y
    if brushed:
        cols = max(8, Wp // 90)
        low = rng.standard_normal((Hp, cols)).astype(np.float32)
        low = np.array(Image.fromarray(low, "F").resize((Wp, Hp), Image.BILINEAR))
        rows = rng.standard_normal((Hp, 1)).astype(np.float32)
        v = low * streak * 0.55 + rows * streak * 0.6
    else:
        v = np.zeros((Hp, Wp), np.float32)
    v += rng.standard_normal((Hp, Wp)).astype(np.float32) * grain
    arr = np.clip(base + v[..., None], 0, 255).astype(np.uint8)
    f.img.paste(Image.fromarray(arr, "RGB"))
    f.d = ImageDraw.Draw(f.img, "RGBA")


def screw(f, cx, cy, r=9):
    f.soft((cx - r, cy - r, cx + r, cy + r), 1.6, opacity=0.6, offset=(1, 2))
    f.grad((cx - r, cy - r, cx + r, cy + r), (236, 237, 233), (120, 124, 121), 45)
    f.ellipse((cx - r, cy - r, cx + r, cy + r), outline=(0, 0, 0, 90), width=0.8)
    for a in (45, 135):
        p0, p1 = polar(cx, cy, r * 0.62, a), polar(cx, cy, r * 0.62, a + 180)
        f.line(p0, p1, 2.2, (40, 42, 41, 230))


def rack(f, ear_col=(0, 0, 0, 60), gold_edge=True):
    """Rack ears with slotted holes, a machined bevel, and the gold frame edge of the family."""
    for x0, x1 in ((0, 44), (1556, 1600)):
        f.d.rectangle(f.P(x0, 0, x1, H), fill=ear_col)
        cx = (x0 + x1) / 2
        for cy in (62, 578):
            f.rrect((cx - 12, cy - 17, cx + 12, cy + 17), 12, fill=(4, 8, 7, 235))
            screw(f, cx, cy, 8.5)
    f.line((44, 0), (44, H), 1.2, (0, 0, 0, 110))
    f.line((45.5, 0), (45.5, H), 0.8, (255, 255, 255, 30))
    f.line((1556, 0), (1556, H), 1.2, (0, 0, 0, 110))
    f.line((1557.5, 0), (1557.5, H), 0.8, (255, 255, 255, 30))
    if gold_edge:
        f.d.rectangle(f.P(0, 0, W, H), outline=GOLD_D, width=round(3 * K))
        f.d.rectangle(f.P(1.5, 1.5, W - 1.5, H - 1.5), outline=(*GOLD_L, 160), width=round(1 * K))


def groove(f, y, x0=64, x1=1536):
    f.line((x0, y), (x1, y), 1.0, (0, 0, 0, 120))
    f.line((x0, y + 1.2), (x1, y + 1.2), 0.8, (255, 255, 255, 34))


def bracket(f, x0, x1, y, title, size=10.5):
    col = f.t["legend"]
    cx = (x0 + x1) / 2
    tw = f.tlen(title, "legend", size, 3.0)
    g = 10
    f.line((x0, y), (cx - tw / 2 - g, y), 1.1, col)
    f.line((cx + tw / 2 + g, y), (x1, y), 1.1, col)
    f.line((x0, y), (x0, y + 7), 1.1, col)
    f.line((x1, y), (x1, y + 7), 1.1, col)
    f.lt(cx, y, title, size, 3.0)


# ---- product name: the stroked Microgramma-style letters of KUBBE/KASET ----------------------
def _quad(p0, p1, p2, n=12):
    return [((1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * p1[0] + t * t * p2[0],
             (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * p1[1] + t * t * p2[1]) for t in np.linspace(0, 1, n)]


def wordmark(f, name, x, y, h, col):
    w, t, r, gap = 1.34 * h, 0.19 * h, 0.30 * h, 0.30 * h
    inn = t * 0.5
    paths = []
    cx = x
    for ch in name:
        L, R, T, B, M = cx + inn, cx + w - inn, y + inn, y + h - inn, y + h * 0.5
        if ch == "K":
            paths += [[(L, T - inn), (L, B + inn)], [(L, M + t * 0.2), (R, T - inn * 0.3)],
                      [(L + (R - L) * 0.36, M - h * 0.12), (R + inn * 0.2, B + inn)]]
        elif ch == "O":
            p = [((L + R) / 2, T), (R - r, T)] + _quad((R - r, T), (R, T), (R, T + r)) + [(R, B - r)] + \
                _quad((R, B - r), (R, B), (R - r, B)) + [(L + r, B)] + _quad((L + r, B), (L, B), (L, B - r)) + \
                [(L, T + r)] + _quad((L, T + r), (L, T), (L + r, T)) + [((L + R) / 2, T)]
            paths.append(p)
        elif ch == "R":
            rb = r * 0.85
            paths.append([(L, B + inn), (L, T)])
            paths.append([(L, T), (R - rb, T)] + _quad((R - rb, T), (R, T), (R, T + rb)) + [(R, M - rb)] +
                         _quad((R, M - rb), (R, M), (R - rb, M)) + [(L, M)])
            paths.append([(L + (R - L) * 0.46, M), (R + inn * 0.2, B + inn)])
        cx += w + gap
    wpx = max(1, round(t * K))
    for dx, dy, c in ((0, 1.3, (0, 0, 0, 95)), (0, 0, col)):
        for p in paths:
            f.d.line([((px + dx) * K, (py + dy) * K) for px, py in p], fill=c, width=wpx, joint="curve")
    return cx - gap - x


# ---- knobs -----------------------------------------------------------------------------------
def knob(f, cx, cy, r, val, label=None, nums=("0", "5", "10"), ticks=True, lsize=10.5, sub=None,
         label_dy=None):
    col = f.t["legend"]
    if ticks:
        for i in range(11):
            a = -135 + 27 * i
            major = i in (0, 5, 10)
            f.line(polar(cx, cy, r * 1.20, a), polar(cx, cy, r * (1.42 if major else 1.31), a),
                   1.5 if major else 1.0, col)
    if nums:
        for a, s in zip((-135, 0, 135), nums):
            ns = max(7.0, r * 0.3) if len(s) <= 3 else max(7.0, r * 0.22)
            if s:
                px, py = polar(cx, cy, r * 1.66, a)
                f.text(px, py + (0 if a == 0 else 1), s, "legend", ns, col)
    f.soft((cx - r, cy - r, cx + r, cy + r), r * 0.10, opacity=0.55, offset=(r * 0.06, r * 0.13))
    f.grad((cx - r, cy - r, cx + r, cy + r), (43, 48, 46), (11, 13, 12), 90)
    for i in range(60):
        a = 360.0 * i / 60
        light = 0.5 + 0.5 * math.cos(math.radians(a) + 0.8)
        f.line(polar(cx, cy, r * 0.86, a), polar(cx, cy, r * 0.985, a), 0.55,
               (255, 255, 255, int(255 * (0.05 + 0.15 * light))))
    cr = r * 0.74
    f.grad((cx - cr, cy - cr, cx + cr, cy + cr), (222, 223, 218), (110, 116, 113), 45)
    rr = cr * 0.12
    step = max(0.9, cr / 26)
    i = 0
    while rr < cr:
        c = (255, 255, 255, 20) if i % 2 else (0, 0, 0, 14)
        f.ellipse((cx - rr, cy - rr, cx + rr, cy + rr), outline=c, width=0.45)
        rr += step
        i += 1
    for a0, a1 in ((144, 201), (-36, 21)):
        f.d.pieslice(f.P(cx - cr, cy - cr, cx + cr, cy + cr), a0, a1, fill=(255, 255, 255, 50))
    f.ellipse((cx - cr, cy - cr, cx + cr, cy + cr), outline=(0, 0, 0, 90), width=0.8)
    ang = -135 + 270 * val
    f.line(polar(cx, cy, cr * 0.28, ang), polar(cx, cy, r * 0.97, ang), max(2.0, r * 0.13), (0, 0, 0, 128))
    f.line(polar(cx, cy, cr * 0.28, ang), polar(cx, cy, r * 0.97, ang), max(1.3, r * 0.08), GOLD)
    if label:
        f.lt(cx, cy + (label_dy if label_dy else r * 1.98), label, lsize, 2.4)
    if sub:
        f.lt(cx, cy + (label_dy if label_dy else r * 1.98) + lsize * 1.25, sub, lsize * 0.72, 1.5, "fine",
             col=f.t["muted"])


def pointer_knob(f, cx, cy, r, labels, angles, index, label=None, lsize=10.5, tsize=8.0, ring=1.62):
    col = f.t["legend"]
    for s, a in zip(labels, angles):
        f.line(polar(cx, cy, r * 1.02, a), polar(cx, cy, r * 1.18, a), 1.0, col)
        px, py = polar(cx, cy, r * ring, a)
        f.text(px, py, s, "legend", tsize, col)
    ang = angles[index]
    f.soft((cx - r * 0.8, cy - r * 0.8, cx + r * 0.8, cy + r * 0.8), r * 0.1, opacity=0.6,
           offset=(r * 0.06, r * 0.14))
    nose = [polar(cx, cy, r * 0.55, ang - 50), polar(cx, cy, r * 1.02, ang), polar(cx, cy, r * 0.55, ang + 50)]
    f.poly(nose, (20, 23, 22))
    f.grad((cx - r * 0.78, cy - r * 0.78, cx + r * 0.78, cy + r * 0.78), (44, 49, 47), (7, 9, 9), 45)
    tip = polar(cx, cy, r * 0.93, ang)
    f.ellipse((tip[0] - r * 0.13, tip[1] - r * 0.13, tip[0] + r * 0.13, tip[1] + r * 0.13), fill=(22, 25, 24))
    f.ellipse((cx - r * 0.78, cy - r * 0.78, cx + r * 0.78, cy + r * 0.78), outline=(255, 255, 255, 26), width=0.7)
    f.line(polar(cx, cy, r * 0.15, ang), polar(cx, cy, r * 0.9, ang), max(1.4, r * 0.09), CREAM)
    if label:
        f.lt(cx, cy + r * 2.1, label, lsize, 2.4)


# ---- switches, lamps, buttons -----------------------------------------------------------------
def lamp(f, cx, cy, r, on, col=LAMP, off=(70, 52, 22)):
    f.ellipse((cx - r * 1.4, cy - r * 1.4, cx + r * 1.4, cy + r * 1.4), fill=(12, 14, 13))
    if on:
        f.soft((cx - r * 3, cy - r * 3, cx + r * 3, cy + r * 3), r * 1.2, col, 0.55)
        f.grad((cx - r, cy - r, cx + r, cy + r), (255, 246, 222), col, 60)
    else:
        f.grad((cx - r, cy - r, cx + r, cy + r), off, tuple(int(c * 0.4) for c in off), 60)
    f.ellipse((cx - r * 0.55, cy - r * 0.7, cx - r * 0.05, cy - r * 0.25), fill=(255, 255, 255, 70))


def toggle(f, cx, cy, on, label=None, s=1.0, on_txt="ON", off_txt="OFF", sub=None):
    col = f.t["legend"]
    f.soft((cx - 12 * s, cy - 12 * s, cx + 12 * s, cy + 12 * s), 2 * s, opacity=0.55, offset=(1.5, 3))
    f.grad((cx - 12 * s, cy - 12 * s, cx + 12 * s, cy + 12 * s), (232, 233, 229), (96, 100, 98), 45)
    f.ellipse((cx - 7 * s, cy - 7 * s, cx + 7 * s, cy + 7 * s), fill=(40, 43, 42))
    dirn = -1 if on else 1
    tipy = cy + dirn * 30 * s
    lever_sh = [(cx - 3.4 * s + 3, cy + 3), (cx + 3.4 * s + 3, cy + 3), (cx + 2.2 * s + 3, tipy + 3),
                (cx - 2.2 * s + 3, tipy + 3)]
    f.poly(lever_sh, (0, 0, 0, 70))
    f.poly([(cx - 3.4 * s, cy), (cx + 3.4 * s, cy), (cx + 2.2 * s, tipy), (cx - 2.2 * s, tipy)], (200, 202, 198))
    f.poly([(cx - 3.4 * s, cy), (cx - 0.6 * s, cy), (cx - 0.4 * s, tipy), (cx - 2.2 * s, tipy)], (245, 246, 242))
    f.soft((cx - 5.5 * s, tipy - 5.5 * s, cx + 5.5 * s, tipy + 5.5 * s), 1.2, opacity=0.5, offset=(2, 3))
    f.grad((cx - 5.5 * s, tipy - 5.5 * s, cx + 5.5 * s, tipy + 5.5 * s), (250, 250, 247), (120, 124, 121), 45)
    f.text(cx + 20 * s, cy - 26 * s, on_txt, "fine", 7.5 * s, col)
    f.text(cx + 20 * s, cy + 26 * s, off_txt, "fine", 7.5 * s, col)
    if label:
        f.lt(cx, cy + 50 * s, label, 10.5 * s, 2.4)
    if sub:
        f.lt(cx, cy + 50 * s + 13 * s, sub, 7.5 * s, 1.2, "fine", col=f.t["muted"])


def button(f, box, text, lit=False, style="cream", size=9.5, num=None, track=1.6):
    x0, y0, x1, y1 = box
    f.rrect((x0 - 3.5, y0 - 3.5, x1 + 3.5, y1 + 3.5), 5, fill=(8, 10, 9))
    f.soft((x0, y0, x1, y1), 1.5, opacity=0.5, offset=(1, 2.5), shape="rect", radius=3)
    if lit:
        f.soft((x0 - 6, y0 - 6, x1 + 6, y1 + 6), 7, GOLD_L, 0.35, shape="rect", radius=6)
        f.grad(box, (240, 212, 146), (190, 152, 82), 90, "rect", 3)
        tc = INK
    elif style == "cream":
        f.grad(box, (242, 238, 226), (206, 199, 181), 90, "rect", 3)
        tc = INK
    elif style == "dark":
        f.grad(box, (22, 74, 68), (4, 40, 37), 90, "rect", 3)
        tc = CREAM
    else:
        f.grad(box, (52, 55, 54), (14, 15, 15), 90, "rect", 3)
        tc = CREAM
    f.line((x0 + 3, y0 + 1), (x1 - 3, y0 + 1), 0.8, (255, 255, 255, 110 if style == "cream" or lit else 40))
    f.text((x0 + x1) / 2, (y0 + y1) / 2 + 0.5, text, "legend", size, tc, "mm", track)
    if num:
        f.text(x0 + 6, y0 + 7, num, "fine", 6.5, (*tc, 170) if len(tc) == 3 else tc, "lm")


def window(f, box, glass=SCREEN, r=6):
    x0, y0, x1, y1 = box
    f.soft((x0 - 6, y0 - 6, x1 + 6, y1 + 6), 4, opacity=0.6, offset=(1, 4), shape="rect", radius=r + 4)
    f.grad((x0 - 7, y0 - 7, x1 + 7, y1 + 7), (30, 33, 32), (4, 5, 5), 90, "rect", r + 4)
    f.rrect(box, r, fill=glass)


def glass_sheen(f, box):
    x0, y0, x1, y1 = box
    f.poly([(x0 + 4, y0 + 3), (x0 + (x1 - x0) * 0.42, y0 + 3), (x0 + (x1 - x0) * 0.24, y1 - 3), (x0 + 4, y1 - 3)],
           (255, 255, 255, 9))


# ---- 7-segment -------------------------------------------------------------------------------
SEG = {"0": "abcdef", "1": "bc", "2": "abged", "3": "abgcd", "4": "fgbc", "5": "afgcd", "6": "afgedc",
       "7": "abc", "8": "abcdefg", "9": "abcdfg", "A": "abcefg", "b": "fgcde", "C": "afed", "c": "ged",
       "d": "bcdeg", "E": "afged", "F": "afge", "G": "afedc", "H": "fbgec", "I": "ef", "J": "bcde",
       "L": "fed", "n": "ceg", "o": "cdeg", "P": "abefg", "r": "eg", "S": "afgcd", "t": "fged",
       "U": "bcdef", "y": "fgbcd", "Z": "abged", "-": "g", " ": ""}


def seg_polys(x, y, h, ch):
    w, th = 0.56 * h, 0.13 * h
    g = th * 0.16
    sl = math.tan(math.radians(7))

    def hs(xa, xb, yc):
        return [(xa, yc), (xa + th / 2, yc - th / 2), (xb - th / 2, yc - th / 2), (xb, yc), (xb - th / 2, yc + th / 2),
                (xa + th / 2, yc + th / 2)]

    def vs(xc, ya, yb):
        return [(xc, ya), (xc + th / 2, ya + th / 2), (xc + th / 2, yb - th / 2), (xc, yb), (xc - th / 2, yb - th / 2),
                (xc - th / 2, ya + th / 2)]

    L, R, T, M, B = th / 2, w - th / 2, th / 2, h / 2, h - th / 2
    segs = {"a": hs(L + g, R - g, T), "b": vs(R, T + g, M - g), "c": vs(R, M + g, B - g), "d": hs(L + g, R - g, B),
            "e": vs(L, M + g, B - g), "f": vs(L, T + g, M - g), "g": hs(L + g, R - g, M)}
    out = {}
    for k_, pl in segs.items():
        out[k_] = [(x + px - (py - h / 2) * sl, y + py) for px, py in pl]
    return out


def seven(f, x, y, h, s, on=LED_RED, off=LED_RED_OFF, pitch=None):
    pitch = pitch or 0.78 * h
    lit = []
    for i, ch in enumerate(s):
        ps = seg_polys(x + i * pitch, y, h, ch)
        for k_, pl in ps.items():
            if k_ in SEG.get(ch, ""):
                lit.append(pl)
            else:
                f.poly(pl, off)
    if lit:
        f.glow_polys(lit, on, blur=h * 0.05, strength=0.7)
    return x + len(s) * pitch



# =============================================================================================
# Revision 2026-09-30: "drop it on the vocal, done". Front = 6 knobs + LOCK + BYPASS + MOOD.
def vu_meter(f, box, needles):
    """Stereo needle meter, VU scale; needles = [(fraction, colour, tag)]."""
    x0, y0, x1, y1 = box
    hw, hh = x1 - x0, y1 - y0
    f.soft((x0 - 6, y0 - 6, x1 + 6, y1 + 6), 4, opacity=0.6, offset=(1, 4), shape="rect", radius=8)
    f.grad((x0 - 7, y0 - 7, x1 + 7, y1 + 7), (30, 33, 32), (4, 5, 5), 90, "rect", 8)
    f.grad(box, (247, 241, 224), (224, 214, 188), 90, "rect", 2)
    cx, py = (x0 + x1) / 2, y1 - hh * 0.08
    R = hh * 0.66
    span = 47
    marks = [(-20, 0.0), (-10, 0.2), (-7, 0.31), (-5, 0.41), (-3, 0.53), (-2, 0.60), (-1, 0.68), (0, 0.76),
             (1, 0.84), (2, 0.92), (3, 1.0)]

    def ang(fr):
        return -span + 2 * span * fr

    dark, red = (32, 32, 30), (190, 55, 38)
    pts = [polar(cx, py, R, ang(t)) for t in np.linspace(0, 0.76, 50)]
    f.d.line([(x * K, y * K) for x, y in pts], fill=dark, width=max(1, round(1.2 * K)))
    pts = [polar(cx, py, R + 1.5, ang(t)) for t in np.linspace(0.76, 1.0, 20)]
    f.d.line([(x * K, y * K) for x, y in pts], fill=red, width=max(1, round(4.0 * K)))
    for db, fr in marks:
        a = ang(fr)
        c = red if db > 0 else dark
        major = db in (-20, -10, -7, -5, -3, 0, 3)
        f.line(polar(cx, py, R, a), polar(cx, py, R + (8 if major else 5), a), 1.2 if major else 0.9, c)
        if db in (-20, -10, -7, -5, -3, -2, -1, 0, 1, 2, 3):
            tx, ty = polar(cx, py, R + 17, a)
            f.text(tx, ty, str(abs(db)) if db != 0 else "0", "legend", 7.5, c)
    f.text(x0 + hw * 0.2, py - R * 0.34, "VU", "legend", 13, dark, "mm", 2)
    f.text(cx - R * 0.95, py - R * 0.28, "−", "legend", 11, dark)
    f.text(cx + R * 0.95, py - R * 0.28, "+", "legend", 11, red)
    for fr, c, tag in needles:
        a = ang(fr)
        f.line((cx + 1.5, py + 2), polar(cx + 1.5, py + 2, R + 6, a), 1.6, (0, 0, 0, 40))
    for fr, c, tag in needles:
        f.line((cx, py), polar(cx, py, R + 6, ang(fr)), 1.2, c)
    hy = y1 - hh * 0.14
    f.d.rectangle(f.P(x0, hy, x1, y1), fill=(22, 24, 23))
    f.line((x0, hy), (x1, hy), 0.8, (0, 0, 0, 220))
    for i, (fr, c, tag) in enumerate(needles):
        tx = x0 + 14 + i * (hw - 28)
        f.line((tx - 6 if i == 0 else tx - 12, (hy + y1) / 2), (tx + 12 if i == 0 else tx + 6, (hy + y1) / 2), 1.6,
               c if c != (20, 20, 20) else (230, 226, 214))
        f.text(tx + (18 if i == 0 else -18), (hy + y1) / 2 + 0.5, tag, "legend", 8, CREAM)
    f.poly([(x0, y0), (x0 + hw * 0.42, y0), (x0 + hw * 0.18, hy), (x0, hy)], (255, 255, 255, 20))


def knob_marks(f, cx, cy, r, marks, size=7.5):
    col = f.t["legend"]
    for a, s in marks:
        f.line(polar(cx, cy, r * 1.20, a), polar(cx, cy, r * 1.44, a), 1.5, col)
        px, py = polar(cx, cy, r * 1.78, a)
        f.text(px, py, s, "legend", size, col, "mm", 0.8)


def model_box(f, x, y, text="MODEL KR-77", size=10):
    tw = f.tlen(text, "legend", size, 2.4)
    f.rrect((x, y - 11, x + tw + 22, y + 11), 1, outline=GOLD, width=1.3)
    f.text(x + 11, y + 0.5, text, "legend", size, GOLD, "lm", 2.4)


def footer(f):
    t = "KORO · MODEL KR-77 · VOCAL DOUBLER · HARMONIZER"
    tw = f.tlen(t, "legend", 9.5, 2.6)
    cx = 800
    f.rrect((cx - tw / 2 - 18, 597, cx + tw / 2 + 18, 623), 1, outline=GOLD, width=1.3)
    f.text(cx, 610.5, t, "legend", 9.5, GOLD, "mm", 2.6)
    f.lt(72, 611, "MEASURED, NOT GUESSED", 7.5, 1.8, "fine", "lm", f.t["muted"])
    wlen = f.logo("wordmark-only-cream.png", 1530, 611, 17, "rm")
    f.logo("mark-only-cream.png", 1530 - wlen - 8, 610, 26, "rm")


def name_and_moods(f):
    wordmark(f, "KORO", 72, 34, 50, CREAM)
    f.lt(74, 110, "VOCAL DOUBLER · HARMONIZER", 10.5, 3.0, anchor="lm")
    model_box(f, 74, 138)
    f.lt(430, 138, "MOODS", 7.5, 2.0, anchor="rm")
    bracket(f, 72, 430, 172, "MOOD")
    bw, bh, g = 110.7, 38, 13
    for i, m in enumerate(MOODS):
        col, row = i % 3, i // 3
        x0 = 73 + col * (bw + g)
        y0 = 190 + row * (bh + 14)
        button(f, (x0, y0, x0 + bw, y0 + bh), m, lit=(m == STATE["mood"]), style="cream", size=8.2,
               num=None, track=1.0)


# ---- A1: VFD ---------------------------------------------------------------------------------
DISPLAY = (480, 34, 990, 296)
VFD_DIM = (38, 108, 96)
VFD_GHOST = (16, 52, 47)


def vfd_annunciator(f, x0, y0, x1, y1, text, on, size=8):
    col = VFD if on else VFD_GHOST
    f.rrect((x0, y0, x1, y1), 2, outline=col, width=0.9)
    if on:
        f.soft((x0, y0, x1, y1), 2.5, VFD, 0.18, shape="rect", radius=3)
        f.glow_text((x0 + x1) / 2, (y0 + y1) / 2 + 0.5, text, "mono", size, VFD, "mm", blur=1.2, strength=0.5)
    else:
        f.text((x0 + x1) / 2, (y0 + y1) / 2 + 0.5, text, "mono", size, col, "mm")


def pitch_ruler(f, x0, x1, y, lit, ghost, dim, glow_marker):
    """One octave from the detected root; scale steps labelled; tracked pitch and harmony note marked."""
    names = ["B", "C", "D#", "E", "F#", "G", "A#", "B"]
    cents = [0, 112, 384, 498, 702, 792, 1088, 1200]

    def X(c):
        return x0 + (x1 - x0) * c / 1200.0

    f.line((x0, y), (x1, y), 0.8, dim)
    for c in range(0, 1201, 100):
        f.line((X(c), y - 3), (X(c), y + 3), 0.6, ghost)
    for n, c in zip(names, cents):
        f.line((X(c), y - 7), (X(c), y + 7), 1.1, dim)
        f.text(X(c), y + 16, n, "mono", 8, dim)
    glow_marker(X(STATE["pitch_cents"]), y, "IN")
    glow_marker(X(STATE["harm_cents"]), y, "OUT")


def vfd_display(f):
    x0, y0, x1, y1 = DISPLAY
    window(f, DISPLAY, SCREEN, r=8)
    for yy in np.arange(y0 + 3, y1 - 2, 2.3):
        f.line((x0 + 4, yy), (x1 - 4, yy), 0.25, (0, 0, 0, 55))
    f.text(x0 + 18, y0 + 20, "KEY · SCALE   DETECTED ON THE VOICE", "mono", 8, VFD_DIM, "lm")
    vfd_annunciator(f, x1 - 118, y0 + 11, x1 - 70, y0 + 29, "AUTO", True)
    vfd_annunciator(f, x1 - 62, y0 + 11, x1 - 16, y0 + 29, "LOCK", False)
    # big key + scale, with unlit cells behind
    f.glow_text(x0 + 16, y0 + 80, STATE["key"], "din", 72, VFD, "lm", blur=3.5)
    f.glow_text(x0 + 78, y0 + 80, STATE["scale"], "din", 72, VFD, "lm", blur=3.5)
    # confidence: four dots, e.g. "B HICAZ  ●●●○"
    f.text(x1 - 18, y0 + 55, "CONFIDENCE", "mono", 7.5, VFD_DIM, "rm")
    n_lit = STATE["conf_dots"]
    for i in range(4):
        cx, cy = x1 - 110 + i * 26, y0 + 84
        if i < n_lit:
            f.soft((cx - 12, cy - 12, cx + 12, cy + 12), 3.5, VFD, 0.45)
            f.ellipse((cx - 8, cy - 8, cx + 8, cy + 8), fill=VFD)
        else:
            f.ellipse((cx - 8, cy - 8, cx + 8, cy + 8), outline=VFD_GHOST, width=1.4)
    f.line((x0 + 16, y0 + 124), (x1 - 16, y0 + 124), 0.6, (38, 108, 96, 150))
    # COLOR: which interval the harmony is using right now, and the safe fallback
    f.text(x0 + 18, y0 + 146, "COLOR", "mono", 8, VFD_DIM, "lm")
    bx = x0 + 70
    for i, s in enumerate(["LOW 3RD", "HIGH 3RD", "5TH", "OCT"]):
        vfd_annunciator(f, bx + i * 82, y0 + 136, bx + i * 82 + 74, y0 + 156, s, s == STATE["color"], 8)
    vfd_annunciator(f, x1 - 90, y0 + 136, x1 - 16, y0 + 156, "SAFE", False, 8)
    f.text(x0 + 18, y0 + 176, f"IN  {STATE['pitch_note']} {STATE['pitch_dev']:+d}c", "mono", 9.5, VFD, "lm")
    f.text(x0 + 150, y0 + 176, f"OUT {STATE['harm_note']}", "mono", 9.5, VFD, "lm")
    f.text(x1 - 18, y0 + 176, "3RD ABOVE IN B HICAZ", "mono", 8, VFD_DIM, "rm")

    def marker(x, y, tag):
        if tag == "IN":
            f.soft((x - 6, y - 12, x + 6, y + 12), 3, VFD, 0.5, shape="rect")
            f.d.rectangle(f.P(x - 1.6, y - 11, x + 1.6, y + 11), fill=VFD)
            f.poly([(x - 5, y - 18), (x + 5, y - 18), (x, y - 12)], VFD)
        else:
            f.d.rectangle(f.P(x - 5, y - 9, x + 5, y + 9), outline=VFD, width=round(1.3 * K))
        f.text(x, y - 26, tag, "mono", 7, VFD if tag == "IN" else VFD_DIM)

    pitch_ruler(f, x0 + 60, x1 - 30, y0 + 222, VFD, VFD_GHOST, VFD_DIM, marker)
    f.text(x0 + 18, y0 + 222, "PITCH", "mono", 8, VFD_DIM, "lm")
    glass_sheen(f, DISPLAY)


# ---- A2: KASET-style cream label window with red 7-segment -------------------------------------
def red_led(f, cx, cy, r, on):
    lamp(f, cx, cy, r, on, LED_RED, (80, 18, 12))


def label_display(f):
    x0, y0, x1, y1 = DISPLAY
    f.soft((x0 - 6, y0 - 6, x1 + 6, y1 + 6), 4, opacity=0.6, offset=(1, 4), shape="rect", radius=10)
    f.grad((x0 - 7, y0 - 7, x1 + 7, y1 + 7), (30, 33, 32), (4, 5, 5), 90, "rect", 10)
    f.grad(DISPLAY, (245, 241, 229), (222, 214, 193), 90, "rect", 4)
    ink2 = (73, 108, 104)
    # headings and gold rule, set like the KASET cassette label
    f.text(x0 + 16, y0 + 16, "KEY", "legend", 8, INK, "lm", 1.8)
    f.text(x0 + 108, y0 + 16, "SCALE · AUTO (READ-OUT)", "legend", 8, INK, "lm", 1.8)
    f.text(x0 + 262, y0 + 16, "CONFIDENCE", "legend", 8, INK, "lm", 1.8)
    f.text(x1 - 16, y0 + 16, "SUBVERSELAB", "legend", 7.5, INK, "rm", 1.8)
    f.d.rectangle(f.P(x0 + 12, y0 + 27, x1 - 12, y0 + 30), fill=GOLD)
    # key: red filter window, 7-segment digit + sharp LED
    wx0, wy0, wx1, wy1 = x0 + 14, y0 + 40, x0 + 92, y0 + 124
    f.rrect((wx0 - 2, wy0 - 2, wx1 + 2, wy1 + 2), 3, fill=(12, 12, 12))
    f.rrect((wx0, wy0, wx1, wy1), 2, fill=(28, 6, 5))
    seven(f, wx0 + 12, wy0 + 10, 64, "b")
    red_led(f, wx1 - 9, wy0 + 11, 2.4, False)
    f.text(wx1 - 9, wy0 + 22, "#", "legend", 7, (190, 120, 110))
    # scale: printed names, one LED lit (7-segment cannot spell MAJOR / MINOR / NIHAVENT)
    for i, s in enumerate(DETECT_SCALES):
        yy = y0 + 46 + i * 17
        on = s == STATE["scale"]
        red_led(f, x0 + 114, yy, 3, on)
        f.text(x0 + 126, yy + 0.5, s, "legend", 8.5, INK if on else ink2, "lm", 1.4)
    f.text(x0 + 114, y0 + 46 + 5 * 17 - 4, "TO OVERRIDE: REAR ▸ KEY / SCALE", "fine", 6.8, ink2, "lm", 0.8)
    # confidence: four LEDs + AUTO / LOCK
    for i in range(4):
        red_led(f, x0 + 272 + i * 30, y0 + 58, 5.5, i < STATE["conf_dots"])
    red_led(f, x0 + 272, y0 + 100, 3.2, True)
    f.text(x0 + 282, y0 + 100.5, "AUTO", "legend", 8, INK, "lm", 1.4)
    red_led(f, x0 + 336, y0 + 100, 3.2, False)
    f.text(x0 + 346, y0 + 100.5, "LOCK", "legend", 8, ink2, "lm", 1.4)
    # latency (static print is true: 54 ms whenever HARMONY is above 0)
    red_led(f, x1 - 92, y0 + 58, 3.2, True)
    f.text(x1 - 82, y0 + 58.5, "HARMONY", "legend", 8, INK, "lm", 1.2)
    f.text(x1 - 82, y0 + 72, "LATENCY 54 MS", "fine", 7, ink2, "lm", 0.8)
    f.d.rectangle(f.P(x0 + 12, y0 + 134, x1 - 12, y0 + 135.2), fill=(*GOLD, 200))
    # COLOR row
    f.text(x0 + 16, y0 + 154, "COLOR", "legend", 8, INK, "lm", 1.8)
    for i, s in enumerate(["LOW 3RD", "HIGH 3RD", "5TH", "OCT"]):
        cx = x0 + 88 + i * 82
        red_led(f, cx, y0 + 154, 3.2, s == STATE["color"])
        f.text(cx + 9, y0 + 154.5, s, "legend", 8, INK if s == STATE["color"] else ink2, "lm", 1.2)
    red_led(f, x1 - 92, y0 + 154, 3.2, False)
    f.text(x1 - 82, y0 + 154.5, "SAFE UNISON", "legend", 7.5, ink2, "lm", 0.8)
    # pitch: 48-segment LED bar over one octave from the root (25 cents per segment)
    bx0, bx1, by = x0 + 64, x1 - 24, y0 + 206
    f.text(x0 + 16, by, "PITCH", "legend", 8, INK, "lm", 1.8)
    f.rrect((bx0 - 5, by - 8, bx1 + 5, by + 8), 2, fill=(28, 6, 5))
    n = 48
    cw = (bx1 - bx0) / n
    pin = int(STATE["pitch_cents"] / 25)
    pout = int(STATE["harm_cents"] / 25)
    lit = []
    for i in range(n):
        box = (bx0 + i * cw + 0.8, by - 4.5, bx0 + (i + 1) * cw - 0.8, by + 4.5)
        if i == pin or i == pout:
            lit.append([(box[0], box[1]), (box[2], box[1]), (box[2], box[3]), (box[0], box[3])])
        else:
            f.d.rectangle(f.P(*box), fill=LED_RED_OFF if abs(i - pin) > 1 else (140, 30, 20))
    f.glow_polys(lit, LED_RED, blur=2.0, strength=0.8)
    for nme, c in zip(["B", "C", "D#", "E", "F#", "G", "A#", "B"], [0, 112, 384, 498, 702, 792, 1088, 1200]):
        xx = bx0 + (bx1 - bx0) * c / 1200.0
        f.line((xx, by + 9), (xx, by + 14), 1.0, INK)
        f.text(xx, by + 22, nme, "legend", 7.5, INK)
    f.text(bx0 + (bx1 - bx0) * STATE["pitch_cents"] / 1200.0, by - 16, "IN", "legend", 7, INK)
    f.text(bx0 + (bx1 - bx0) * STATE["harm_cents"] / 1200.0, by - 16, "OUT", "legend", 7, INK)
    f.text(x0 + 16, y1 - 14, "ONE OCTAVE FROM THE DETECTED KEY  ·  25 CENTS PER LAMP", "fine", 7, ink2, "lm", 0.8)


# ---- the face ---------------------------------------------------------------------------------
THEME_A = dict(legend=CREAM, muted=MUTED)


def front(display):
    f = Face(THEME_A)
    plate(f, (26, 70, 66), (9, 42, 39), streak=9, grain=3.2)
    rack(f, gold_edge=False)
    name_and_moods(f)
    display(f)
    vu_meter(f, (1036, 50, 1336, 246), [(0.60, (20, 20, 20), "L"), (0.55, (170, 40, 30), "R")])
    lamp(f, 1448, 66, 5.5, False)
    toggle(f, 1448, 146, False, "LOCK", sub="HOLDS THE KEY")
    groove(f, 318)
    cy, r = 450, 41
    bracket(f, 72, 540, 348, "DOUBLE")
    knob(f, 140, cy, r, 0.45, "DOUBLE", sub="0 = OFF")
    knob(f, 305, cy, r, 0.75, "WIDTH")
    knob(f, 470, cy, r, 0.62, "TONE", nums=("DARK", "", "BRIGHT"))
    bracket(f, 598, 992, 348, "HARMONY")
    knob(f, 698, cy, r, 0.55, "HARMONY", sub="0 = OFF")
    knob(f, 890, cy, r, 0.34, "COLOR", nums=None, ticks=True, sub="INTERVAL")
    knob_marks(f, 890, cy, r, [(-135, "LOW 3RD"), (-45, "HIGH 3RD"), (45, "5TH"), (135, "OCT")])
    bracket(f, 1052, 1536, 348, "OUT")
    knob(f, 1150, cy, r, 0.50, "OUTPUT", nums=("−12", "0", "+12"), sub="dB")
    toggle(f, 1300, 446, False, "BYPASS")
    button(f, (1404, 246, 1492, 280), "REAR", lit=False, style="cream", size=8.2, track=1.6)
    f.lt(1448, 292, "SET-UP PANEL", 7, 1.2, "fine", col=MUTED)
    lamp(f, 1448, 426, 5.5, True)
    f.lt(1448, 450, "LATENCY", 8, 1.6, "fine")
    f.lt(1448, 463, "54 MS", 8, 1.6, "fine")
    f.lt(1448, 480, "HARMONY UP", 7, 1.2, "fine", col=MUTED)
    footer(f)
    return f


# ---- rear plate (advanced) ----------------------------------------------------------------------
def rear():
    f = Face(THEME_A, 1200, 440)
    plate(f, (16, 52, 49), (6, 32, 30), streak=6, grain=3.0, seed=11)
    f.d.rectangle(f.P(0, 0, 1200, 440), outline=(0, 0, 0), width=round(3 * K))
    for x, y in ((18, 18), (1182, 18), (18, 422), (1182, 422)):
        screw(f, x, y, 7.5)
    f.lt(44, 40, "KORO · MODEL KR-77 · REAR PANEL", 11, 3.0, anchor="lm")
    f.lt(44, 58, "SET-UP. THE MOOD ON THE FRONT WRITES THESE; CHANGE THEM ONLY TO OVERRIDE.", 7.5, 1.2, "fine",
         "lm", MUTED)
    wl = f.logo("wordmark-only-cream.png", 1156, 42, 14, "rm")
    f.logo("mark-only-cream.png", 1156 - wl - 8, 41, 22, "rm")
    groove(f, 76, 30, 1170)
    # harmony voices
    bracket(f, 44, 560, 104, "HARMONY VOICES")
    lv, pn = [0.80, 0.70, 0.62], [0.18, 0.82, 0.5]
    for i, x in enumerate((130, 300, 470)):
        f.lt(x, 128, f"VOICE {i + 1}", 8, 1.8, "fine", col=MUTED)
        knob(f, x, 184, 24, lv[i], "LEVEL", lsize=9)
        knob(f, x, 318, 24, pn[i], "PAN", nums=("L", "C", "R"), lsize=9)
    bracket(f, 600, 780, 104, "MOTION")
    knob(f, 690, 184, 24, 0.30, "GLIDE", nums=("0", "", "120"), sub="MS", lsize=9)
    knob(f, 690, 318, 24, 0.40, "TIMING", nums=("0", "", "30"), sub="MS", lsize=9)
    bracket(f, 820, 1156, 104, "KEY OVERRIDE")
    klabels = ["AUTO"] + KEYS12
    kang = [-150 + i * (300 / 12) for i in range(13)]
    pointer_knob(f, 910, 196, 25, klabels, kang, 0, label="KEY", tsize=6.8, ring=1.9, lsize=9)
    slabels = ["AUTO", "MAJ", "MIN", "NIHAV", "HICAZ", "RAST", "CHROM"]
    sang = [-135 + i * 45 for i in range(7)]
    pointer_knob(f, 1072, 196, 25, slabels, sang, 0, label="SCALE / MAKAM", tsize=6.8, ring=2.05, lsize=9)
    bracket(f, 820, 1156, 286, "DOUBLE")
    toggle(f, 910, 342, False, None, s=0.8)
    f.lt(910, 390, "MONO SAFE", 9, 2.0)
    f.lt(1072, 332, "DOUBLES GO IN", 7, 1.0, "fine", col=MUTED)
    f.lt(1072, 344, "THE SIDE ONLY:", 7, 1.0, "fine", col=MUTED)
    f.lt(1072, 356, "MONO = INPUT", 7, 1.0, "fine", col=MUTED)
    return f


# =============================================================================================
def contact_sheet(a1, a2):
    fh = 520
    items = [("KUBBE  ·  KB-79  (shipping)", Image.open(KUBBE_FACE).convert("RGB")),
             ("KASET  ·  KS-83  (shipping)", Image.open(KASET_FACE).convert("RGB")),
             ("KORO  ·  KR-77  A1  VFD", a1),
             ("KORO  ·  KR-77  A2  CREAM LABEL + 7-SEG", a2)]
    ims = [(t, im.resize((int(im.width * fh / im.height), fh), Image.LANCZOS)) for t, im in items]
    gap, top = 44, 110
    Wc = sum(im.width for _, im in ims) + gap * (len(ims) + 1)
    Hc = top + fh + 80
    sheet = Image.new("RGB", (Wc, Hc), (16, 19, 18))
    d = ImageDraw.Draw(sheet)
    ft = ImageFont.truetype(FONTS["legend"][0], 34, index=1)
    fl = ImageFont.truetype(FONTS["legend"][0], 24, index=1)
    d.text((gap, 40), "SUBVERSELAB HARDWARE LINE  —  same face height, one family", font=ft, fill=CREAM)
    x = gap
    for t, im in ims:
        sheet.paste(im, (x, top))
        d.text((x, top + fh + 24), t, font=fl, fill=GOLD if "KORO" in t else MUTED)
        x += im.width + gap
    return sheet


def main():
    os.makedirs(OUT, exist_ok=True)
    fa1 = front(vfd_display)
    a1 = fa1.face()
    a1.save(os.path.join(OUT, "koro_A1_front_vfd.png"))
    fa1.hero(HERO_X0).save(os.path.join(OUT, "koro_A1_hero_4x5.png"))
    del fa1
    fa2 = front(label_display)
    a2 = fa2.face()
    a2.save(os.path.join(OUT, "koro_A2_front_label7seg.png"))
    fa2.hero(HERO_X0).save(os.path.join(OUT, "koro_A2_hero_4x5.png"))
    del fa2
    rear().face().save(os.path.join(OUT, "koro_rear_plate.png"))
    contact_sheet(a1, a2).save(os.path.join(OUT, "concepts_contact.png"))
    for n in sorted(os.listdir(OUT)):
        if n.endswith(".png"):
            print(n, Image.open(os.path.join(OUT, n)).size)


HERO_X0 = 478

if __name__ == "__main__":
    main()
