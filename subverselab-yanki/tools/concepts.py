#!/usr/bin/env python3
"""
YANKI, Model YK-74 -- panel concepts (Pillow + numpy only, no network).

    python3 tools/concepts.py            # writes everything into docs/concepts/

Follows PRODUCT_BRIEF.md. One face, the family look (KUBBE green plate, machined knobs,
KASET-style cream label window with red 7-segment digits and LEDs, MOOD keys without numbers,
REAR key, gold plate text). The drawing kit is NOT re-invented: it is imported from the
approved KORO generator (../../subverselab-koro/tools/concepts.py): Face, plate, rack, screw,
groove, bracket, knob, pointer_knob, knob_marks, lamp, toggle, button, seven, model_box, ...
Only the YANKI-specific parts live here (Y/N/I glyphs for the stroked product name, the
concentric TIME control, the FEEDBACK scale with its HOLD mark, the tape-loop display).

Outputs (docs/concepts/):
    yanki_front.png            1600x640  the face in a swell state (DUB SPIRAL, FEEDBACK past HOLD)
    yanki_front_default.png    1600x640  the same face as it opens (ECHO, 1/8 at the host tempo)
    yanki_hero_4x5.png         1080x1350 Instagram crop of yanki_front.png (display + LOOP knobs)
    yanki_rear_plate.png       1200x440  rear plate (set-up controls)
    concepts_contact.png       KUBBE | KASET | KORO A2 | YANKI at the same face height
"""

import importlib.util
import math
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
YANKI = os.path.abspath(os.path.join(HERE, ".."))
OUT = os.path.join(YANKI, "docs", "concepts")
TOOLS = os.path.abspath(os.path.join(YANKI, ".."))
KIT_PATH = os.path.join(TOOLS, "subverselab-koro", "tools", "concepts.py")
KORO_A2 = os.path.join(TOOLS, "subverselab-koro", "docs", "concepts", "koro_A2_front_label7seg.png")

_spec = importlib.util.spec_from_file_location("family_kit", KIT_PATH)
kit = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(kit)

Face, plate, rack, screw, groove, bracket = kit.Face, kit.plate, kit.rack, kit.screw, kit.groove, kit.bracket
knob, pointer_knob, knob_marks, lamp, toggle = kit.knob, kit.pointer_knob, kit.knob_marks, kit.lamp, kit.toggle
button, seven, model_box, polar, _quad = kit.button, kit.seven, kit.model_box, kit.polar, kit._quad
CREAM, INK, GOLD, GOLD_L, GOLD_D, MUTED = kit.CREAM, kit.INK, kit.GOLD, kit.GOLD_L, kit.GOLD_D, kit.MUTED
LED_RED, LED_RED_OFF, FONTS, K = kit.LED_RED, kit.LED_RED_OFF, kit.FONTS, kit.K
THEME = kit.THEME_A
SWELL_RED = (236, 104, 78)   # the swell arc on the green plate (VU red lifted for contrast)
INK2 = (73, 108, 104)        # secondary print on the cream label (KORO A2)

MOODS = ["ECHO", "SLAPBACK", "THREE HEADS", "DUB SPIRAL", "WORN LOOP", "YESILCAM", "TERS"]
SYNC_LABELS = ["1/16", "1/8", "1/8.", "1/4", "1/2", "FREE"]
SYNC_ANGLES = [-125, -75, -25, 25, 75, 125]
# heads that play per MOOD (engine `mode`: 1 = head 1, 12 = heads 1+2+3)
MOOD_HEADS = {"ECHO": (1,), "SLAPBACK": (1,), "THREE HEADS": (1, 2, 3), "DUB SPIRAL": (1, 2, 3),
              "WORN LOOP": (1, 2, 3), "YESILCAM": (1,), "TERS": (1,)}
SWELL_MOODS = ("SLAPBACK", "THREE HEADS", "DUB SPIRAL", "WORN LOOP")   # classic loop (engine moods 1-4)

# The two states the renders show. Values are engine values (see docs/concepts/README.md).
STATE_SWELL = dict(mood="DUB SPIRAL", sync=2, bpm=120, ms=375, free_ms=0.45,
                   fb=0.62,            # knob position: 0.5 = HOLD, 0.62 = RISE 0.24 (loop just swelling)
                   cap=3,              # dB over the dry phrase; None = OFF
                   tone=0.23, wear=0.80, mix=0.45, out=0.5,
                   end="HOLD", freeze=False, dry_db=-18, loop_db=-15.5)
STATE_ECHO = dict(mood="ECHO", sync=1, bpm=120, ms=250, free_ms=0.45,
                  fb=0.30,             # FEEDBACK 0.35 (below HOLD: decays)
                  cap=None, tone=0.50, wear=0.25, mix=0.30, out=0.5,
                  end="HOLD", freeze=False, dry_db=-18, loop_db=-26)


# ---- product name: KASET's drawProductName glyphs (K, A) + Y, N, I in the same stroke logic --
def wordmark(f, name, x, y, h, col):
    w, t, r, gap = 1.34 * h, 0.19 * h, 0.30 * h, 0.30 * h
    inn = t * 0.5
    paths = []
    cx = x
    for ch in name:
        adv = w
        L, R, T, B, M = cx + inn, cx + w - inn, y + inn, y + h - inn, y + h * 0.5
        if ch == "K":
            paths += [[(L, T - inn), (L, B + inn)], [(L, M + t * 0.2), (R, T - inn * 0.3)],
                      [(L + (R - L) * 0.36, M - h * 0.12), (R + inn * 0.2, B + inn)]]
        elif ch == "A":
            paths.append([(L, B + inn), (L, T + r)] + _quad((L, T + r), (L, T), (L + r, T)) + [(R - r, T)] +
                         _quad((R - r, T), (R, T), (R, T + r)) + [(R, B + inn)])
            paths.append([(L, M + h * 0.06), (R, M + h * 0.06)])
        elif ch == "Y":
            mx = (L + R) / 2
            paths.append([(L, T - inn), (L, M - r)] + _quad((L, M - r), (L, M), (L + r, M)) + [(R - r, M)] +
                         _quad((R - r, M), (R, M), (R, M - r)) + [(R, T - inn)])
            paths.append([(mx, M), (mx, B + inn)])
        elif ch == "N":
            paths += [[(L, B + inn), (L, T - inn)], [(L, T), (R, B)], [(R, B + inn), (R, T - inn)]]
        elif ch == "I":
            adv = t
            paths.append([(cx + inn, T - inn), (cx + inn, B + inn)])
        cx += adv + gap
    wpx = max(1, round(t * K))
    for dx, dy, c in ((0, 1.3, (0, 0, 0, 95)), (0, 0, col)):
        for p in paths:
            f.d.line([((px + dx) * K, (py + dy) * K) for px, py in p], fill=c, width=wpx, joint="curve")
    return cx - gap - x


# ---- front-panel parts -------------------------------------------------------------------------
def name_and_moods(f, s):
    wordmark(f, "YANKI", 72, 34, 50, CREAM)
    f.lt(74, 110, "TAPE ECHO · TEMPO SYNCED", 10.5, 3.0, anchor="lm")
    model_box(f, 74, 138, "MODEL YK-74")
    bracket(f, 72, 440, 172, "MOOD")
    bw, bh, g = 84.5, 36, 10
    for i, m in enumerate(MOODS):
        row = 0 if i < 4 else 1
        col = i if i < 4 else i - 4
        x0 = 72 + col * (bw + g) + (0 if row == 0 else (bw + g) / 2)
        y0 = 190 + row * (bh + 13)
        button(f, (x0, y0, x0 + bw, y0 + bh), m, lit=(m == s["mood"]), style="cream", size=7.4, track=0.9)
    f.lt(256, 296, "OPENS ON ECHO · A MOOD SETS HEADS, DRIVE AND VOICING", 6.8, 1.0, "fine", col=MUTED)


def time_control(f, cx, cy, s):
    """Concentric TIME: outer bakelite ring = SYNC (host tempo divisions or FREE), inner machined
    cap = free milliseconds (acts when the ring is on FREE; the display shows the resolved ms)."""
    R = 52
    pointer_knob(f, cx, cy, R, SYNC_LABELS, SYNC_ANGLES, s["sync"], label=None, tsize=8.4, ring=1.58)
    knob(f, cx, cy, 21, s["free_ms"], label=None, nums=None, ticks=False)
    f.lt(cx, cy + R * 1.46 + 5, "TIME", 10.5, 2.4)
    f.lt(cx, cy + R * 1.46 + 5 + 13, "RING: SYNC  ·  CAP: FREE MS", 7.5, 1.4, "fine", col=MUTED)


def feedback_knob(f, cx, cy, r, val):
    """0 .. HOLD (12 o'clock) = repeats decay; HOLD .. MAX = the loop swells (red arc)."""
    pts = [polar(cx, cy, r * 1.50, a) for a in np.linspace(2, 135, 40)]
    f.d.line([(x * K, y * K) for x, y in pts], fill=SWELL_RED, width=max(1, round(3.2 * K)))
    knob(f, cx, cy, r, val, label=None, nums=None, ticks=True)
    knob_marks(f, cx, cy, r, [(-135, "0"), (135, "MAX")], size=8)
    f.line(polar(cx, cy, r * 1.18, 0), polar(cx, cy, r * 1.62, 0), 2.2, CREAM)
    f.text(cx, cy - r * 1.86, "HOLD", "legend", 8.4, CREAM, "mm", 1.2)
    px, py = polar(cx, cy, r * 1.86, 88)
    f.text(px + 6, py, "SWELL", "legend", 7.6, SWELL_RED, "lm", 1.0)
    f.lt(cx, cy + r * 1.98, "FEEDBACK", 10.5, 2.4)
    f.lt(cx, cy + r * 1.98 + 13, "PAST HOLD THE LOOP SWELLS", 7.5, 1.4, "fine", col=MUTED)


def cap_knob(f, cx, cy, r, cap_db):
    def ang(db):
        return -100 + 235 * (db + 12) / 24.0
    a = -135 if cap_db is None else ang(cap_db)
    knob(f, cx, cy, r, (a + 135) / 270.0, label=None, nums=None, ticks=True)
    knob_marks(f, cx, cy, r, [(-135, "OFF"), (ang(-12), "−12"), (ang(0), "0"), (135, "+12")], size=8)
    f.lt(cx, cy + r * 1.98, "CAP", 10.5, 2.4)
    f.lt(cx, cy + r * 1.98 + 13, "LOOP CEILING · dB OVER DRY", 7.5, 1.4, "fine", col=MUTED)


def small_key(f, box, text, lit, style="cream", size=8.2):
    button(f, box, text, lit=lit, style=style, size=size, track=1.4)


def end_and_freeze(f, s):
    bracket(f, 1040, 1318, 52, "END")
    for i, e in enumerate(["HOLD", "CUT", "DUMP"]):
        x0 = 1044 + i * 93
        small_key(f, (x0, 72, x0 + 82, 108), e, e == s["end"])
    f.lt(1179, 128, "CUT AND DUMP FIRE ON PRESS · A NEW PHRASE RE-ARMS", 6.8, 1.0, "fine", col=MUTED)
    bracket(f, 1360, 1536, 52, "FREEZE")
    small_key(f, (1384, 72, 1512, 108), "FREEZE", s["freeze"], style="dark", size=8.6)
    f.lt(1448, 128, "TAP: LATCH · HOLD: MOMENTARY", 6.8, 1.0, "fine", col=MUTED)
    groove(f, 150, 1040, 1536)
    toggle(f, 1150, 214, True, None)
    f.lt(1150, 268, "BYPASS", 10.5, 2.4)
    f.lt(1150, 281, "UP = EFFECT ON", 6.8, 1.0, "fine", col=MUTED)
    lamp(f, 1290, 196, 5.5, True)
    f.lt(1290, 220, "BEAT", 8, 1.6, "fine")
    f.lt(1290, 233, "FLASHES ON THE HOST QUARTER", 6.8, 1.0, "fine", col=MUTED)
    button(f, (1404, 196, 1492, 230), "REAR", lit=False, style="cream", size=8.2, track=1.6)
    f.lt(1448, 244, "SET-UP PANEL", 7, 1.2, "fine", col=MUTED)


# ---- display: KASET cassette-label window, red 7-segment, LEDs ---------------------------------
DISPLAY = (480, 34, 1000, 296)


def red_led(f, cx, cy, r, on):
    lamp(f, cx, cy, r, on, LED_RED, (80, 18, 12))


def seg_window(f, x, y, w, h):
    f.rrect((x - 2, y - 2, x + w + 2, y + h + 2), 3, fill=(12, 12, 12))
    f.rrect((x, y, x + w, y + h), 2, fill=(28, 6, 5))


def tape_loop(f, box, s):
    """The tape loop seen from above: fixed heads, the tape moves. The oxide marks travel one head
    gap per TIME (in the plugin they are animated from the resolved ms; here drawn with a streak)."""
    x0, y0, x1, y1 = box
    rr = (y1 - y0) / 2
    brown, brown_d = (104, 66, 38), (70, 42, 24)
    f.rrect(box, rr, outline=brown_d, width=6.5)
    f.rrect(box, rr, outline=brown, width=4.2)
    # guide rollers at both ends
    for cx in (x0 + rr, x1 - rr):
        cy = y0 + rr
        f.ellipse((cx - 8, cy - 8, cx + 8, cy + 8), fill=(176, 170, 156), outline=(60, 60, 58), width=0.8)
        f.ellipse((cx - 2.2, cy - 2.2, cx + 2.2, cy + 2.2), fill=(60, 60, 58))
    # capstan + pinch roller on the right bend
    cx, cy = x1 - 3, (y0 + y1) / 2
    f.ellipse((cx - 5, cy - 5, cx + 5, cy + 5), fill=(40, 40, 40))
    f.ellipse((cx + 4, cy - 7, cx + 18, cy + 7), fill=(28, 28, 28))
    f.text(x1 + 10, cy + 22, "CAPSTAN", "legend", 6.0, INK2, "mm", 0.8)
    # direction, inside the loop
    ax, ay = x0 + rr + 22, cy
    f.line((ax, ay), (ax + 40, ay), 1.1, INK)
    f.poly([(ax + 46, ay), (ax + 38, ay - 3.5), (ax + 38, ay + 3.5)], INK)
    f.text(ax + 52, ay + 0.5, "TAPE", "legend", 6.8, INK, "lm", 1.2)
    # oxide marks on the top run with a motion streak (the top run moves right-to-left)
    for k in range(6):
        mx = x0 + rr + 6 + k * 28
        f.d.rectangle(f.P(mx, y0 - 2.5, mx + 3, y0 + 2.5), fill=(236, 226, 200))
        for j in range(1, 6):
            f.d.rectangle(f.P(mx + 3 + j * 2.2, y0 - 2.0, mx + 5 + j * 2.2, y0 + 2.0),
                          fill=(236, 226, 200, int(150 - 26 * j)))
    # heads under the bottom run: ERASE, REC, then PLAY 1 / 2 / 3 one TIME apart
    hx = [x0 + rr + 4, x0 + rr + 36, x0 + rr + 78, x0 + rr + 114, x0 + rr + 150]
    tags = ["ERASE", "REC", "1", "2", "3"]
    heads = MOOD_HEADS[s["mood"]]
    for i, (x, tg) in enumerate(zip(hx, tags)):
        f.rrect((x - 9, y1 + 4, x + 9, y1 + 18), 2, fill=(46, 48, 47))
        f.rrect((x - 6, y1 + 1, x + 6, y1 + 5), 1, fill=(196, 198, 192))
        f.text(x, y1 + 28, tg, "legend", 6.6, INK, "mm", 0.6)
        if i >= 2:
            red_led(f, x, y1 + 40, 2.8, int(tg) in heads)
    f.text(hx[4] + 20, y1 + 40.5, "PLAYING", "legend", 6.4, INK2, "lm", 0.8)


def level_bar(f, x0, x1, y, db, label, cap_db=None, dry_db=None):
    f.text(x0 - 48, y, label, "legend", 8, INK, "lm", 1.8)
    f.rrect((x0 - 5, y - 7.5, x1 + 5, y + 7.5), 2, fill=(28, 6, 5))
    n = 40                                      # 1 dB per lamp, -40 .. 0 dBFS
    cw = (x1 - x0) / n
    lit_n = int(round(db + 40))
    lit = []
    for i in range(n):
        box = (x0 + i * cw + 0.8, y - 4.2, x0 + (i + 1) * cw - 0.8, y + 4.2)
        if i < lit_n:
            lit.append([(box[0], box[1]), (box[2], box[1]), (box[2], box[3]), (box[0], box[3])])
        else:
            f.d.rectangle(f.P(*box), fill=LED_RED_OFF)
    if lit:
        f.glow_polys(lit, LED_RED, blur=1.6, strength=0.6)
    if cap_db is not None and dry_db is not None:
        xc = x0 + (dry_db + cap_db + 40) * cw
        f.poly([(xc - 4, y - 15), (xc + 4, y - 15), (xc, y - 9.5)], INK)
        f.text(xc + 7, y - 13, "CAP +%d dB" % cap_db if cap_db >= 0 else "CAP %d dB" % cap_db, "legend", 6.2, INK, "lm", 0.6)


def label_display(f, s):
    x0, y0, x1, y1 = DISPLAY
    f.soft((x0 - 6, y0 - 6, x1 + 6, y1 + 6), 4, opacity=0.6, offset=(1, 4), shape="rect", radius=10)
    f.grad((x0 - 7, y0 - 7, x1 + 7, y1 + 7), (30, 33, 32), (4, 5, 5), 90, "rect", 10)
    f.grad(DISPLAY, (245, 241, 229), (222, 214, 193), 90, "rect", 4)
    f.text(x0 + 16, y0 + 16, "TAPE LOOP", "legend", 8, INK, "lm", 1.8)
    f.text(x0 + 318, y0 + 16, "TEMPO", "legend", 8, INK, "lm", 1.8)
    f.text(x1 - 16, y0 + 16, "SUBVERSELAB", "legend", 7.5, INK, "rm", 1.8)
    f.d.rectangle(f.P(x0 + 12, y0 + 27, x1 - 12, y0 + 30), fill=GOLD)
    tape_loop(f, (x0 + 26, y0 + 48, x0 + 272, y0 + 118), s)
    # tempo: host BPM and the resolved TIME in ms, 7-segment
    bx = x0 + 318
    seg_window(f, bx, y0 + 44, 104, 50)
    seven(f, bx + 12, y0 + 52, 34, f"{s['bpm']:>3d}", pitch=28)
    f.text(bx + 116, y0 + 58, "BPM", "legend", 8, INK, "lm", 1.6)
    red_led(f, bx + 119, y0 + 78, 3.0, True)
    f.text(bx + 128, y0 + 78.5, "HOST", "legend", 7, INK, "lm", 1.2)
    seg_window(f, bx, y0 + 104, 104, 50)
    seven(f, bx + 12, y0 + 112, 34, f"{s['ms']:>3d}", pitch=28)
    f.text(bx + 116, y0 + 118, "MS", "legend", 8, INK, "lm", 1.6)
    synced = s["sync"] != 5
    red_led(f, bx + 119, y0 + 138, 3.0, synced)
    f.text(bx + 128, y0 + 138.5, "SYNC", "legend", 7, INK if synced else INK2, "lm", 1.2)
    red_led(f, bx + 162, y0 + 138, 3.0, not synced)
    f.text(bx + 171, y0 + 138.5, "FREE", "legend", 7, INK2 if synced else INK, "lm", 1.2)
    f.d.rectangle(f.P(x0 + 12, y0 + 178, x1 - 12, y0 + 179.2), fill=(*GOLD, 200))
    # loop vs dry: the swell is visible as the LOOP bar passing the DRY bar
    lx0, lx1 = x0 + 66, x1 - 96
    level_bar(f, lx0, lx1, y0 + 198, s["dry_db"], "DRY")
    level_bar(f, lx0, lx1, y0 + 232, s["loop_db"], "LOOP", s["cap"], s["dry_db"])
    for db in (-40, -30, -20, -10, 0):
        xx = lx0 + (db + 40) * (lx1 - lx0) / 40
        f.line((xx, y0 + 241), (xx, y0 + 245), 0.9, INK)
        f.text(xx, y0 + 252, "−" + str(-db) if db else "0", "legend", 6.6, INK)
    f.text(lx1 + 10, y0 + 252, "dBFS", "legend", 6.4, INK2, "lm", 0.6)
    swell = s["mood"] in SWELL_MOODS and s["fb"] > 0.5
    red_led(f, x1 - 50, y0 + 204, 5.5, swell)
    f.text(x1 - 50, y0 + 226, "SWELL", "legend", 8, INK if swell else INK2, "mm", 1.6)
    f.text(x1 - 50, y0 + 238, "PAST HOLD", "fine", 6.4, INK2, "mm", 0.6)


def footer(f):
    t = "YANKI · MODEL YK-74 · TAPE ECHO"
    tw = f.tlen(t, "legend", 9.5, 2.6)
    f.rrect((800 - tw / 2 - 18, 597, 800 + tw / 2 + 18, 623), 1, outline=GOLD, width=1.3)
    f.text(800, 610.5, t, "legend", 9.5, GOLD, "mm", 2.6)
    f.lt(72, 611, "MEASURED, NOT GUESSED", 7.5, 1.8, "fine", "lm", MUTED)
    wlen = f.logo("wordmark-only-cream.png", 1530, 611, 17, "rm")
    f.logo("mark-only-cream.png", 1530 - wlen - 8, 610, 26, "rm")


def front(s):
    f = Face(THEME)
    plate(f, (26, 70, 66), (9, 42, 39), streak=9, grain=3.2)
    rack(f, gold_edge=False)
    name_and_moods(f, s)
    label_display(f, s)
    end_and_freeze(f, s)
    groove(f, 318)
    cy = 450
    bracket(f, 72, 440, 348, "TIME")
    time_control(f, 256, cy - 4, s)
    bracket(f, 500, 980, 348, "LOOP")
    feedback_knob(f, 625, cy, 41, s["fb"])
    cap_knob(f, 860, cy, 41, s["cap"])
    bracket(f, 1012, 1256, 348, "TAPE")
    knob(f, 1072, cy, 37, s["tone"], "TONE", nums=("1k", "", "8k"), sub="HIGH-CUT · Hz")
    knob(f, 1196, cy, 37, s["wear"], "WEAR", sub="WOW · FLUTTER")
    bracket(f, 1290, 1536, 348, "OUT")
    knob(f, 1350, cy, 37, s["mix"], "MIX", nums=("DRY", "", "WET"))
    knob(f, 1474, cy, 37, s["out"], "OUTPUT", nums=("−12", "0", "+12"), sub="dB")
    footer(f)
    return f


# ---- rear plate --------------------------------------------------------------------------------
def rear():
    f = Face(THEME, 1200, 440)
    plate(f, (16, 52, 49), (6, 32, 30), streak=6, grain=3.0, seed=11)
    f.d.rectangle(f.P(0, 0, 1200, 440), outline=(0, 0, 0), width=round(3 * K))
    for x, y in ((18, 18), (1182, 18), (18, 422), (1182, 422)):
        screw(f, x, y, 7.5)
    f.lt(44, 40, "YANKI · MODEL YK-74 · REAR PANEL", 11, 3.0, anchor="lm")
    f.lt(44, 58, "SET-UP. A MOOD RECALLS HEADS, DRIVE, VOICING AND SPREAD; DARKEN, RISE CURVE, TEMPO LOCK AND FINE STAY WHERE YOU LEAVE THEM.",
         7.5, 1.2, "fine", "lm", MUTED)
    wl = f.logo("wordmark-only-cream.png", 1156, 42, 14, "rm")
    f.logo("mark-only-cream.png", 1156 - wl - 8, 41, 22, "rm")
    groove(f, 76, 30, 1170)
    cy, r = 206, 30
    bracket(f, 44, 440, 104, "SWELL")
    knob(f, 140, cy, r, 0.40, "DARKEN", lsize=9.5, sub="EXTRA HIGH-CUT PER PASS")
    pointer_knob(f, 334, cy, r * 0.95, ["SLOW", "MED", "FAST"], [-60, 0, 60], 1, label="RISE CURVE",
                 tsize=7.6, ring=1.75, lsize=9.5)
    f.lt(334, cy + r * 2.0 + 13, "HOW FAST THE LOOP BUILDS PAST HOLD", 7.5 * 0.9, 1.2, "fine", col=MUTED)
    bracket(f, 480, 740, 104, "TEMPO LOCK")
    pointer_knob(f, 610, cy, r * 0.95, ["OFF", "1", "2", "4"], [-90, -30, 30, 90], 2, label="BARS",
                 tsize=8.4, ring=1.7, lsize=9.5)
    f.lt(610, cy + r * 2.0 + 13, "SWELL LANDS ON CAP ON THE DOWNBEAT", 6.8, 1.2, "fine", col=MUTED)
    bracket(f, 780, 960, 104, "STEREO")
    knob(f, 870, cy, r, 0.30, "SPREAD", nums=("MONO", "", "WIDE"), lsize=9.5, sub="HEAD 2 OPPOSITE 1 AND 3")
    bracket(f, 1000, 1156, 104, "FREE TIME")
    knob(f, 1078, cy, r, 0.50, "FINE", nums=("−10", "0", "+10"), lsize=9.5, sub="MS · WHEN TIME IS ON FREE")
    groove(f, 318, 30, 1170)
    f.lt(44, 346, "SWELL AND TEMPO LOCK ACT IN THE SWELLING MOODS: SLAPBACK · THREE HEADS · DUB SPIRAL · WORN LOOP.",
         7.5, 1.2, "fine", "lm", MUTED)
    f.lt(44, 362, "TEMPO LOCK NEEDS CAP (FRONT). THE HOST TEMPO IS READ CONTINUOUSLY; WITHOUT A HOST TEMPO THE UNIT RUNS AT 120 BPM.",
         7.5, 1.2, "fine", "lm", MUTED)
    return f


# ---- contact sheet -----------------------------------------------------------------------------
def contact_sheet(yk):
    fh = 520
    items = [("KUBBE  ·  KB-79  (shipping)", Image.open(kit.KUBBE_FACE).convert("RGB")),
             ("KASET  ·  KS-83  (shipping)", Image.open(kit.KASET_FACE).convert("RGB")),
             ("KORO  ·  KR-77  A2", Image.open(KORO_A2).convert("RGB")),
             ("YANKI  ·  YK-74", yk)]
    ims = [(t, im.resize((int(im.width * fh / im.height), fh), Image.LANCZOS)) for t, im in items]
    gap, top = 44, 110
    Wc = sum(im.width for _, im in ims) + gap * (len(ims) + 1)
    sheet = Image.new("RGB", (Wc, top + fh + 80), (16, 19, 18))
    d = ImageDraw.Draw(sheet)
    ft = ImageFont.truetype(FONTS["legend"][0], 34, index=1)
    fl = ImageFont.truetype(FONTS["legend"][0], 24, index=1)
    d.text((gap, 40), "SUBVERSELAB HARDWARE LINE  —  same face height, one family", font=ft, fill=CREAM)
    x = gap
    for t, im in ims:
        sheet.paste(im, (x, top))
        d.text((x, top + fh + 24), t, font=fl, fill=GOLD if "YANKI" in t else MUTED)
        x += im.width + gap
    return sheet


HERO_X0 = 484


def main():
    os.makedirs(OUT, exist_ok=True)
    fs = front(STATE_SWELL)
    face = fs.face()
    face.save(os.path.join(OUT, "yanki_front.png"))
    fs.hero(HERO_X0).save(os.path.join(OUT, "yanki_hero_4x5.png"))
    del fs
    fd = front(STATE_ECHO)
    fd.face().save(os.path.join(OUT, "yanki_front_default.png"))
    del fd
    rear().face().save(os.path.join(OUT, "yanki_rear_plate.png"))
    contact_sheet(face).save(os.path.join(OUT, "concepts_contact.png"))
    for n in sorted(os.listdir(OUT)):
        if n.endswith(".png"):
            print(n, Image.open(os.path.join(OUT, n)).size)


if __name__ == "__main__":
    main()
