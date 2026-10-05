#!/usr/bin/env python3
"""YANKI v2 face concepts (Pillow + numpy). Three layouts, each shown in three states (short slap, 1/8 dotted, long swell).

    python3 tools/concepts_v2.py        # writes docs/ui/v2_concept_A.png, _B.png, _C.png (1600 px wide)

The rings are drawn from the model the plug-in will use: repeat k sits at radius k * T (time), its level is
L1 + (k-1) * g dB (first repeat, then the decay per repeat; a swell rises until CAP), brightness and stroke width follow
that level, warmth follows TONE and WEAR, and the left/right offset follows SPREAD. Numbers for the three states come from
tools/presets_v2_measured.json where it exists (measured through the real processor), otherwise from the design targets.
"""
import json, math, os, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageChops

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import concepts as C                      # loads the family drawing kit (KORO generator)

kit = C.kit
kit.K = 2.0                               # drawing scale: 3200 px wide, saved at 1600
K = kit.K
C.K = K                                # the stroked wordmark in concepts.py reads its own K
Face, plate, screw, groove, bracket = kit.Face, kit.plate, kit.screw, kit.groove, kit.bracket
knob, pointer_knob, lamp, toggle, button, seven, model_box = kit.knob, kit.pointer_knob, kit.lamp, kit.toggle, kit.button, kit.seven, kit.model_box
polar, CREAM, GOLD, GOLD_L, GOLD_D, MUTED, INK = kit.polar, kit.CREAM, kit.GOLD, kit.GOLD_L, kit.GOLD_D, kit.MUTED, kit.INK
THEME = kit.THEME_A
OUT = os.path.normpath(os.path.join(HERE, "../docs/ui"))

PHOS = (157, 247, 221)      # phosphor (Theme::phosphor)
TEAL = (99, 242, 216)
SCREEN = (2, 21, 18)
AMBER = (236, 184, 96)
RUST = (196, 98, 58)
RED = (255, 64, 40)
SWELL_RED = (236, 104, 78)


def lerp(a, b, t):
    t = max(0.0, min(1.0, t))
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


# ---- the three states ---------------------------------------------------------------------------------------------
def states():
    m = {}
    p = os.path.join(HERE, "presets_v2_measured.json")
    if os.path.exists(p):
        for r in json.load(open(p)):
            m[r["name"]] = r
    sl, dl = m.get("Slap 80 ms", {}), m.get("Dotted eighth wide", {})
    sw = m.get("Swell build", {})
    slap = dict(tag="SHORT SLAP", preset="SLAP 80 MS", group="SLAP", time_s=0.080, time_txt="80 MS", sync="FREE", first=-12.0,
                decay_bars=sl.get("decay_bars_m", 0.5), tone=5200, wear=0.25, spread=0.30, wet=0.25, fb_pos=0.66, view_s=1.3,
                pingpong=False, swell=False, bpm=120, rho=sl.get("rho", 1.0), mood="ECHO")
    dotted = dict(tag="1/8 DOTTED, FEEDBACK", preset="DOTTED EIGHTH WIDE", group="WIDE", time_s=0.375, time_txt="375 MS", sync="1/8.",
                  first=-12.0, decay_bars=dl.get("decay_bars_m", 2.0), tone=3800, wear=0.30, spread=0.90, wet=0.21, fb_pos=0.28, view_s=4.4,
                  pingpong=True, swell=False, bpm=120, rho=dl.get("rho", 0.14), mood="THREE HEADS")
    swell = dict(tag="LONG SWELL", preset="SWELL BUILD", group="SWELL", time_s=0.375, time_txt="375 MS", sync="1/8.", first=-9.0,
                 decay_bars=0, tone=2600, wear=0.8, spread=0.65, wet=0.48, fb_pos=0.74, view_s=9.0, pingpong=True, swell=True, cap=3.0,
                 bpm=120, rho=sw.get("rho", 0.55), mood="DUB SPIRAL", plateau=sw.get("swell_plateau_db", 4.8), reach=sw.get("swell_reach_s", 4.0))
    return slap, dotted, swell


def level_series(s, n=80):
    """dB re the dry of repeats 1..n, until they fall under -60 (or, for a swell, until they have held at CAP)."""
    out = []
    if s["swell"]:
        L = s["first"]
        for k in range(1, n + 1):
            out.append(min(L, s["cap"]))
            L += 1.7 if L < s["cap"] else 0.0
        return out
    reps = s["decay_bars"] * 2.0 / s["time_s"]
    g = -(60.0 + s["first"]) / max(reps - 1.0, 1.0) if reps > 1 else -60.0
    for k in range(1, n + 1):
        L = s["first"] + (k - 1) * g
        if L < -60:
            break
        out.append(L)
    return out


# ---- the rings ------------------------------------------------------------------------------------------------------
def draw_rings(f, box, s, style="glow"):
    """Concentric rings in the screen box (base units). style: glow (phosphor on glass), heat (filled), paper (ink on cream)."""
    x0, y0, x1, y1 = box
    W, H = int((x1 - x0) * K), int((y1 - y0) * K)
    cx, cy = (x1 - x0) / 2, (y1 - y0) / 2 + 8
    R = min(cx - 20, cy - 26)
    paper = style == "paper"
    layer = f.img.crop((int(x0 * K), int(y0 * K), int(x0 * K) + W, int(y0 * K) + H)).convert("RGB") if paper else Image.new("RGB", (W, H), (0, 0, 0))
    ld = ImageDraw.Draw(layer, "RGBA")

    def P(x, y):
        return (x * K, y * K)

    def circle(ccx, ccy, r, col, w, alpha=255):
        ld.ellipse([P(ccx - r, ccy - r), P(ccx + r, ccy + r)], outline=(*col, alpha), width=max(1, int(w * K)))

    lv = level_series(s)
    tone_n = (s["tone"] - 1000) / 7000.0
    # guides: bar marks (1/2 bar, 1 bar, ...) and a faint cross
    gcol = (30, 90, 80) if not paper else (150, 140, 118)
    ld.line([P(cx - R - 8, cy), P(cx + R + 8, cy)], fill=(*gcol, 90), width=max(1, int(0.8 * K)))
    ld.line([P(cx, cy - R - 8), P(cx, cy + R + 8)], fill=(*gcol, 90), width=max(1, int(0.8 * K)))
    bar = 2.0
    marks = [(0.5 * bar, "1/2 BAR"), (bar, "1 BAR"), (2 * bar, "2 BARS"), (4 * bar, "4 BARS")]
    for t, lab in marks:
        if t <= s["view_s"] * 1.001:
            r = R * t / s["view_s"]
            for a0 in range(0, 360, 6):
                ld.arc([P(cx - r, cy - r), P(cx + r, cy + r)], a0, a0 + 2.2, fill=(*gcol, 130), width=max(1, int(0.8 * K)))
    circle(cx, cy, R, gcol, 1.0, 110)
    # repeats
    n = len(lv)
    glow = Image.new("RGB", (W, H), (0, 0, 0))
    gd = ImageDraw.Draw(glow, "RGBA")
    ring_pos = []
    for k, L in enumerate(lv, 1):
        r = R * k * s["time_s"] / s["view_s"]
        if r > R:
            break
        a = ((L + 60.0) / 66.0) ** 1.5
        spacing = R * s["time_s"] / s["view_s"]
        w = min(0.9 + 5.2 * a, max(1.0, 0.42 * spacing))
        heat = k * 0.075 * (1.15 - tone_n) + s["wear"] * 0.18
        if paper:
            col = lerp((14, 64, 58), (150, 70, 40), heat * 0.9)
        else:
            col = lerp(TEAL if tone_n > 0.4 else PHOS, AMBER, heat)
            col = lerp(col, RUST, max(0.0, heat - 0.55) * 1.3)
        if s["swell"]:
            col = lerp(col, SWELL_RED, 0.12 * min(1.0, k / 12.0))
        off = s["spread"] * R * 0.16 * ((-1) ** k if s["pingpong"] else -0.8)
        al = int(70 + 185 * a)
        circle(cx + off, cy, r, col, w, al)
        gd.ellipse([P(cx + off - r, cy - r), P(cx + off + r, cy + r)], outline=(*col, int((150 * a + 20) * (0.45 if s["swell"] else 1.0))), width=max(1, int((w + 3) * K)))
        ring_pos.append((k, L, cx + off, r))
    # dry core
    core = (255, 250, 232) if not paper else (14, 64, 58)
    gd.ellipse([P(cx - 11, cy - 11), P(cx + 11, cy + 11)], fill=(*core, 150))
    ld.ellipse([P(cx - 6.5, cy - 6.5), P(cx + 6.5, cy + 6.5)], fill=(*core, 255))
    if not paper:
        glow = glow.filter(ImageFilter.GaussianBlur(5.5 * K))
        layer = ImageChops.add(layer, glow)
        layer = ImageChops.add(layer, glow.filter(ImageFilter.GaussianBlur(5 * K)))
    if style == "heat":
        heatl = Image.new("RGB", (W, H), (0, 0, 0))
        hd = ImageDraw.Draw(heatl, "RGBA")
        for k, L, ox, r in reversed(ring_pos):
            a = ((L + 60.0) / 66.0) ** 1.4
            hd.ellipse([P(ox - r, cy - r), P(ox + r, cy + r)], fill=(*lerp((10, 40, 36), AMBER, a * 0.8), int(60 + 80 * a)))
        heatl = heatl.filter(ImageFilter.GaussianBlur(3 * K))
        layer = ImageChops.add(heatl, layer)
    if s["swell"]:      # the CAP ring: where the loop is held
        rc = R * 0.80
        for a0 in range(0, 360, 10):
            ld2 = ImageDraw.Draw(layer, "RGBA")
            ld2.arc([P(cx - rc, cy - rc), P(cx + rc, cy + rc)], a0, a0 + 5, fill=(*SWELL_RED, 210), width=max(1, int(1.6 * K)))
    px, py = int(x0 * K), int(y0 * K)
    if paper:
        f.img.paste(layer, (px, py))
    else:                                   # light on glass: add the layer to what is there (no black box)
        under = f.img.crop((px, py, px + W, py + H))
        f.img.paste(ImageChops.add(under, layer), (px, py))
    # annotations (drawn on the face, not the layer)
    ink = INK if paper else PHOS
    dim = (73, 108, 104) if paper else (80, 170, 150)
    ox, oy = x0 + cx, y0 + cy
    for t, lab in marks:
        if t <= s["view_s"] * 1.001:
            r = R * t / s["view_s"]
            f.text(ox + r + 4, oy + 11, lab, "mono", 8.0, dim, "lm", 0.6)
    if ring_pos:
        k, L, rx, r = ring_pos[0]
        chip(f, ox + R * 0.30, oy - R * 1.02 + 10, "FIRST REPEAT %+.0f dB" % L, 9.5, ink, "lm", paper)
        k, L, rx, r = ring_pos[-1]
        lab = ("-60 dB · %.1f BARS" % (s["decay_bars"])) if not s["swell"] else "HELD AT CAP +%d dB" % s["cap"]
        chip(f, ox + R * 0.42, oy + R * 1.0 - 4, lab, 9.5, SWELL_RED if s["swell"] else ink, "lm", paper)
    f.text(ox - 2, oy + 24, "DRY", "mono", 8.0, dim, "mm", 1.0)
    return (ox, oy, R)


def chip(f, x, y, text, size, col, anchor="lm", paper=False):
    """A label on a small dark (or cream) backing so it stays readable over the rings."""
    w = f.tlen(text, "mono", size, 0.5) if hasattr(f, "tlen") else len(text) * size * 0.62
    x0 = x if anchor[0] == "l" else x - w
    f.rrect((x0 - 4, y - size * 0.8, x0 + w + 4, y + size * 0.8), 3, fill=((245, 241, 229, 225) if paper else (1, 14, 12, 225)))
    f.text(x, y, text, "mono", size, col, anchor, 0.5)


def handles(f, box, ring, paper=False):
    ox, oy, R = ring
    gcol = GOLD_D if paper else GOLD_L
    for ang, lab, dx, dy, anc in ((-40, "TIME", 12, -10, "lm"), (118, "FEEDBACK", -12, 10, "rm"), (200, "SPREAD", -12, 0, "rm")):
        px, py = polar(ox, oy, R * (0.50 if lab != "FEEDBACK" else 0.88), ang)
        f.ellipse((px - 5.5, py - 5.5, px + 5.5, py + 5.5), outline=(*gcol, 255), width=1.6)
        f.ellipse((px - 2, py - 2, px + 2, py + 2), fill=gcol)
        f.text(px + dx, py + dy, lab, "legend", 7.2, gcol, anc, 1.2)


# ---- glass screen -------------------------------------------------------------------------------------------------
def glass(f, box, paper=False):
    x0, y0, x1, y1 = box
    f.soft((x0 - 8, y0 - 8, x1 + 8, y1 + 8), 5, opacity=0.65, offset=(1, 5), shape="rect", radius=14)
    f.grad((x0 - 9, y0 - 9, x1 + 9, y1 + 9), (36, 40, 39), (4, 5, 5), 90, "rect", 12)
    if paper:
        f.grad(box, (245, 241, 229), (222, 214, 193), 90, "rect", 5)
    else:
        f.grad(box, (4, 30, 26), (1, 12, 10), 90, "rect", 6)
        f.rrect((x0 + 1, y0 + 1, x1 - 1, y1 - 1), 6, outline=(60, 120, 108, 60), width=1.0)


def readout(f, x, y, rows, paper=False, w=215):
    col = INK if paper else PHOS
    dim = (73, 108, 104) if paper else (92, 168, 152)
    for i, (k, v, hot) in enumerate(rows):
        yy = y + i * 19
        f.text(x, yy, k, "mono", 10.0, dim, "lm", 0.4)
        f.text(x + w, yy, v, "mono", 10.5, (SWELL_RED if hot else col), "rm", 0.4)


def decay_graph(f, box, s, paper=False):
    """Level of each repeat against time (the same series the rings use)."""
    x0, y0, x1, y1 = box
    col = INK if paper else PHOS
    dim = (73, 108, 104) if paper else (60, 130, 116)
    f.line((x0, y1), (x1, y1), 0.9, dim)
    f.line((x0, y0), (x0, y1), 0.9, dim)
    for db in (0, -20, -40, -60):
        yy = y0 + (-db) / 66.0 * (y1 - y0) * 1.0
        f.line((x0, yy), (x1, yy), 0.5, (*dim, 110))
        f.text(x0 - 5, yy, str(db), "mono", 7.0, dim, "rm", 0.2)
    lv = level_series(s)
    pts = []
    for k, L in enumerate(lv, 1):
        t = k * s["time_s"]
        if t > s["view_s"]: break
        pts.append((x0 + (x1 - x0) * t / s["view_s"], y0 + (-min(L, 3.0)) / 66.0 * (y1 - y0) * 1.0 + 0.0))
    for a, b in zip(pts, pts[1:]):
        f.line(a, b, 1.2, dim)
    for px, py in pts:
        f.ellipse((px - 2.6, py - 2.6, px + 2.6, py + 2.6), fill=col)
    f.text(x0, y0 - 11, "LEVEL OF EACH REPEAT (dB re DRY) / TIME", "mono", 7.4, dim, "lm", 0.5)


# ---- common frame ----------------------------------------------------------------------------------------------------
def frame(f, h):
    plate(f, (26, 70, 66), (9, 42, 39), streak=9, grain=3.2)
    f.d.rectangle(f.P(0, 0, 1600, h), outline=GOLD_D, width=round(3 * K))
    f.d.rectangle(f.P(1.5, 1.5, 1598.5, h - 1.5), outline=(*GOLD_L, 150), width=round(1 * K))
    for x, y in ((24, 24), (1576, 24), (24, h - 24), (1576, h - 24)):
        screw(f, x, y, 8.5)
    C.wordmark(f, "YANKI", 72, 34, 46, CREAM)
    f.lt(74, 104, "ECHO · MODEL YK-74", 10.0, 3.0, anchor="lm")


def preset_bar(f, s, x0=470, x1=1130, y=36):
    """The preset browser in the top bar: previous / name + group / next / list."""
    f.soft((x0 - 4, y - 4, x1 + 4, y + 60), 4, opacity=0.55, offset=(1, 3), shape="rect", radius=12)
    f.grad((x0 - 5, y - 5, x1 + 5, y + 61), (36, 40, 39), (4, 5, 5), 90, "rect", 10)
    f.grad((x0, y, x1, y + 56), (4, 30, 26), (1, 12, 10), 90, "rect", 7)
    button(f, (x0 + 12, y + 12, x0 + 52, y + 44), "<", style="dark", size=13)
    button(f, (x1 - 52, y + 12, x1 - 12, y + 44), ">", style="dark", size=13)
    f.text(x0 + 70, y + 14, "PRESET", "mono", 8.0, (92, 168, 152), "lm", 1.2)
    f.glow_text((x0 + x1) / 2, y + 30, s["preset"], "mono", 21, GOLD_L, "mm", blur=2.6, strength=0.55)
    f.text(x1 - 70, y + 14, s["group"], "mono", 8.5, PHOS, "rm", 1.2)
    f.text(x1 - 70, y + 44, "1 OF 10", "mono", 7.5, (92, 168, 152), "rm", 1.0)
    f.text(x0 + 70, y + 44, "STARTING POINT", "mono", 7.5, (92, 168, 152), "lm", 1.0)


def top_right(f, s, x=1190):
    f.text(x + 30, 54, "BPM", "legend", 8, CREAM, "lm", 1.8)
    f.rrect((x + 62, 36, x + 62 + 100, 36 + 46), 3, fill=(12, 12, 12))
    f.rrect((x + 64, 38, x + 62 + 98, 36 + 44), 2, fill=(28, 6, 5))
    seven(f, x + 76, 44, 30, "%3d" % s["bpm"], pitch=26)
    lamp(f, x + 30, 74, 3.2, True, (255, 64, 40), (80, 18, 12))
    f.text(x + 40, 74.5, "HOST", "legend", 7, CREAM, "lm", 1.2)
    toggle(f, x + 215, 64, False, None, s=0.6)
    f.text(x + 215, 106, "BYPASS", "legend", 7.5, CREAM, "mm", 1.4)


def knob_label(f, cx, y, name, value, sub=None):
    f.text(cx, y, name, "legend", 10, CREAM, "mm", 2.2)
    f.text(cx, y + 15, value, "mono", 9, PHOS, "mm", 0.5)


def swell_section(f, x0, x1, y0, s, vertical=True):
    """SWELL as a section of its own: the on/off key, RISE, CAP, END and FREEZE."""
    on = s["swell"]
    bracket(f, x0, x1, y0, "SWELL")
    cx = (x0 + x1) / 2
    button(f, (cx - 62, y0 + 24, cx + 62, y0 + 58), "SWELL", lit=on, style="cream", size=11, track=2.2)
    f.text(cx, y0 + 72, "LOOP SWELLS PAST HOLD" if on else "OFF · REPEATS DECAY", "fine", 6.8, MUTED, "mm", 0.8)
    kx = [cx - 50, cx + 50]
    knob(f, kx[0], y0 + 130, 30, 0.45 if on else 0.2, "RISE", nums=("SLOW", "", "FAST"), lsize=9.5)
    knob(f, kx[1], y0 + 130, 30, 0.55 if on else 0.0, "CAP", nums=("OFF", "", "+12"), lsize=9.5)
    for i, t in enumerate(("HOLD", "CUT", "DUMP")):
        bx = cx - 78 + i * 53
        button(f, (bx, y0 + 214, bx + 48, y0 + 240), t, lit=(i == 0), style="cream", size=7.4, track=0.8)
    f.text(cx, y0 + 252, "END", "legend", 8.5, CREAM, "mm", 2.0)
    button(f, (cx - 62, y0 + 266, cx + 62, y0 + 294), "FREEZE", style="dark", size=8.6, track=1.4)


def wet_hero(f, cx, cy, r, s, title=True):
    f.soft((cx - r * 1.2, cy - r * 1.2, cx + r * 1.2, cy + r * 1.2), r * 0.3, GOLD, 0.10)
    knob(f, cx, cy, r, s["wet"], None, nums=("DRY", "", "WET"), ticks=True)
    if title:
        f.text(cx, cy + r * 1.62 + 14, "WET", "legend", 17, CREAM, "mm", 5.0)
    f.text(cx, cy + r * 1.62 + (36 if title else 12), "MIX %d %%  ·  FIRST REPEAT %+.0f dB" % (round(s["wet"] * 100), s["first"]), "mono", 9.5, PHOS, "mm", 0.5)


def knob_row(f, xs, y, s, r=33):
    sync = ["1/16", "1/8", "1/8.", "1/4", "1/2", "FREE"]
    idx = sync.index(s["sync"]) if s["sync"] in sync else 5
    pointer_knob(f, xs[0], y, 46, sync, [-125, -75, -25, 25, 75, 125], idx, label=None, tsize=7.6, ring=1.58)
    knob(f, xs[0], y, 18, 0.45, label=None, nums=None, ticks=False)
    knob_label(f, xs[0], y + 77, "TIME", ("%s · %s" % (s["sync"], s["time_txt"])))
    knob(f, xs[1], y, r, s["fb_pos"], None, nums=("0", "", "MAX"))
    knob_label(f, xs[1], y + 62, "FEEDBACK", "%.1f BARS" % s["decay_bars"] if not s["swell"] else "PAST HOLD")
    knob(f, xs[2], y, r, (s["tone"] - 1000) / 7000.0, None, nums=("1k", "", "8k"))
    knob_label(f, xs[2], y + 62, "TONE", "%.1f kHz" % (s["tone"] / 1000.0))
    knob(f, xs[3], y, r, s["wear"], None)
    knob_label(f, xs[3], y + 62, "WEAR", "%.1f" % (s["wear"] * 10))
    knob(f, xs[4], y, r, s["spread"], None, nums=("MONO", "", "WIDE"))
    knob_label(f, xs[4], y + 62, "SPREAD", "%d %%" % round(s["spread"] * 100))
    knob(f, xs[5], y, r, 0.5, None, nums=("-12", "0", "+12"))
    knob_label(f, xs[5], y + 62, "OUTPUT", "0.0 dB")


def footer(f, h):
    t = "YANKI · MODEL YK-74 · ECHO"
    tw = f.tlen(t, "legend", 9.5, 2.6)
    f.rrect((800 - tw / 2 - 18, h - 43, 800 + tw / 2 + 18, h - 17), 1, outline=GOLD, width=1.3)
    f.text(800, h - 29.5, t, "legend", 9.5, GOLD, "mm", 2.6)
    f.lt(72, h - 29, "MEASURED, NOT GUESSED", 7.5, 1.8, "fine", "lm", MUTED)
    wlen = f.logo("wordmark-only-cream.png", 1530, h - 29, 17, "rm")
    f.logo("mark-only-cream.png", 1530 - wlen - 8, h - 30, 26, "rm")


def drag_legend(f, x, y, paper=False):
    col = (73, 108, 104) if paper else (92, 168, 152)
    f.text(x, y, "DRAG A RING OUT / IN = TIME  ·  UP / DOWN = FEEDBACK  ·  SIDEWAYS = SPREAD", "mono", 7.6, col, "lm", 0.5)


def rows_for(s):
    rep = level_series(s)
    return [("TIME", "%s %s" % (s["sync"], s["time_txt"]), False),
            ("REPEATS VISIBLE", "%d" % len(rep), False),
            ("FIRST REPEAT", "%+.0f dB" % s["first"], False),
            ("DECAY -60 dB", ("%.1f BARS" % s["decay_bars"]) if not s["swell"] else "HOLDS AT CAP", s["swell"]),
            ("TONE / WEAR", "%.1fk / %.1f" % (s["tone"] / 1000.0, s["wear"] * 10), False),
            ("WIDTH (L/R CORR)", "%.2f" % s["rho"], False)]


# ---- layouts --------------------------------------------------------------------------------------------------------
def layout_A(s, h=900):
    f = Face(THEME, 1600, h)
    frame(f, h)
    preset_bar(f, s)
    top_right(f, s)
    box = (360, 128, 1240, 640)
    glass(f, box)
    f.text(box[0] + 18, box[1] + 20, "ECHO FIELD", "mono", 9.5, (92, 168, 152), "lm", 2.0)
    ring = draw_rings(f, box, s, "glow")
    handles(f, box, ring)
    readout(f, box[0] + 20, box[1] + 56, rows_for(s))
    drag_legend(f, box[0] + 20, box[3] - 16)
    f.text(box[2] - 18, box[1] + 20, "VIEW %.1f S" % s["view_s"], "mono", 8.5, (92, 168, 152), "rm", 1.0)
    bracket(f, 72, 330, 150, "WET")
    wet_hero(f, 200, 340, 76, s)
    swell_section(f, 1272, 1530, 150, s)
    groove(f, 668)
    knob_row(f, [432, 600, 750, 890, 1030, 1170], 735, s)
    footer(f, h)
    return f, box


def layout_B(s, h=960):
    f = Face(THEME, 1600, h)
    frame(f, h)
    preset_bar(f, s)
    top_right(f, s)
    box = (72, 128, 1528, 556)
    glass(f, box)
    f.text(box[0] + 18, box[1] + 20, "ECHO FIELD", "mono", 9.5, (92, 168, 152), "lm", 2.0)
    ring = draw_rings(f, (440, 128, 1160, 556), s, "heat")
    handles(f, box, ring)
    readout(f, box[0] + 24, box[1] + 60, rows_for(s), w=250)
    decay_graph(f, (1210, 214, 1500, 330), s)
    f.text(1210, 380, "VIEW %.1f S  ·  1 BAR = 2.0 S AT 120 BPM" % s["view_s"], "mono", 7.6, (92, 168, 152), "lm", 0.4)
    drag_legend(f, 96, 536)
    # swell strip
    bracket(f, 72, 1528, 588, "SWELL")
    for i, (t, lit) in enumerate((("SWELL", s["swell"]),)):
        button(f, (96, 604, 214, 640), t, lit=lit, style="cream", size=11, track=2.2)
    f.text(160, 654, "PAST HOLD THE LOOP SWELLS" if s["swell"] else "OFF · REPEATS DECAY", "fine", 6.6, MUTED, "mm", 0.6)
    for i, t in enumerate(("SLOW", "MED", "FAST")):
        button(f, (300 + i * 60, 612, 354 + i * 60, 638), t, lit=(i == 1 and s["swell"]), style="cream", size=7.4, track=0.8)
    f.text(390, 650, "RISE", "legend", 8.5, CREAM, "mm", 2.0)
    knob(f, 560, 626, 24, 0.55 if s["swell"] else 0.0, None, nums=None, ticks=True)
    f.text(560, 662, "CAP  " + ("+3 dB" if s["swell"] else "OFF"), "legend", 8.5, CREAM, "mm", 1.6)
    for i, t in enumerate(("HOLD", "CUT", "DUMP")):
        button(f, (700 + i * 58, 612, 754 + i * 58, 638), t, lit=(i == 0), style="cream", size=7.4, track=0.8)
    f.text(786, 650, "END", "legend", 8.5, CREAM, "mm", 2.0)
    for i, t in enumerate(("OFF", "1", "2", "4")):
        button(f, (960 + i * 50, 612, 1006 + i * 50, 638), t, lit=(i == 0), style="cream", size=7.4, track=0.8)
    f.text(1060, 650, "LOCK  BARS", "legend", 8.5, CREAM, "mm", 1.8)
    button(f, (1300, 604, 1420, 640), "FREEZE", style="dark", size=9, track=1.4)
    groove(f, 684)
    knob_row(f, [190, 330, 470, 1130, 1270, 1410], 780, s, r=31)
    # the hero WET, centre
    wet_hero(f, 800, 772, 66, s, title=False)
    bracket(f, 610, 990, 706, "WET")
    footer(f, h)
    return f, box


def layout_C(s, h=900):
    f = Face(THEME, 1600, h)
    frame(f, h)
    preset_bar(f, s)
    top_right(f, s)
    box = (380, 128, 1220, 640)
    glass(f, box, paper=True)
    f.text(box[0] + 18, box[1] + 20, "ECHO FIELD", "legend", 8.5, INK, "lm", 1.8)
    f.text(box[2] - 18, box[1] + 20, "SUBVERSELAB", "legend", 7.5, INK, "rm", 1.8)
    f.d.rectangle(f.P(box[0] + 12, box[1] + 30, box[2] - 12, box[1] + 33), fill=GOLD)
    ring = draw_rings(f, (box[0], box[1] + 10, box[2], box[3]), s, "paper")
    handles(f, box, ring, paper=True)
    readout(f, box[0] + 22, box[1] + 62, rows_for(s), paper=True, w=206)
    drag_legend(f, box[0] + 20, box[3] - 16, paper=True)
    # left: swell; right: the WET hero
    swell_section(f, 72, 340, 150, s)
    bracket(f, 1262, 1530, 150, "WET")
    wet_hero(f, 1396, 340, 76, s)
    groove(f, 668)
    knob_row(f, [452, 612, 758, 898, 1038, 1178], 735, s)
    footer(f, h)
    return f, box


def sheet(name, layout, st):
    slap, dotted, swell = st
    faces = {}
    for key, s in (("slap", slap), ("dotted", dotted), ("swell", swell)):
        faces[key] = layout(s)
    main, mbox = faces["dotted"]
    W = 1600
    img_main = main.img.resize((W, int(main.h)), Image.LANCZOS)
    cap_h = 70
    strip_h = 360 + cap_h
    out = Image.new("RGB", (W, img_main.height + strip_h + 24), (6, 22, 20))
    out.paste(img_main, (0, 0))
    d = ImageDraw.Draw(out)
    from PIL import ImageFont
    font = ImageFont.truetype("/System/Library/Fonts/Menlo.ttc", 15, index=1)
    fsm = ImageFont.truetype("/System/Library/Fonts/Menlo.ttc", 12, index=0)
    y0 = img_main.height + 16
    d.text((24, y0 - 2), "THE SAME SCREEN IN THREE STATES (main view above: 1/8 dotted with feedback)", font=fsm, fill=(143, 170, 165))
    y0 += 22
    cw = (W - 24 * 4) // 3
    for i, (key, s) in enumerate((("slap", slap), ("dotted", dotted), ("swell", swell))):
        f, box = faces[key]
        sc = f.img.size[0] / 1600.0
        pad = 14
        crop = f.img.crop((int((box[0] - pad) * K), int((box[1] - pad) * K), int((box[2] + pad) * K), int((box[3] + pad) * K)))
        crop = crop.resize((cw, int(crop.height * cw / crop.width)), Image.LANCZOS)
        x = 24 + i * (cw + 24)
        out.paste(crop, (x, y0))
        d.text((x, y0 + crop.height + 8), s["tag"], font=font, fill=(223, 191, 122))
        sub = "%s · %s · %s" % (s["preset"], s["sync"] + " " + s["time_txt"], ("decay %.1f bars" % s["decay_bars"]) if not s["swell"] else "holds at CAP +%d dB" % s["cap"])
        d.text((x, y0 + crop.height + 28), sub, font=fsm, fill=(143, 170, 165))
    out = out.crop((0, 0, W, y0 + crop.height + 56))
    path = os.path.join(OUT, name)
    out.save(path)
    print(path, out.size)


def main():
    os.makedirs(OUT, exist_ok=True)
    st = states()
    which = sys.argv[1:] or ["A", "B", "C"]
    for key, layout in (("A", layout_A), ("B", layout_B), ("C", layout_C)):
        if key in which:
            sheet("v2_concept_%s.png" % key, layout, st)


if __name__ == "__main__":
    main()
