#!/usr/bin/env python3
"""
HAVA, Model HV-72 -- panel concepts (Pillow + numpy only, no network).

    python3 tools/concepts.py            # writes everything into docs/concepts/

A brand-new 1972 studio "air" unit: a top-end lift plus a spring sheen, presented as a colour box.
Engine: ../prototypes/YAY/Source/YayEngine.h (struct yay::Controls), presets: tools/presets.h.

Owner change 2026-09-30: HAVA must never read as a reverb. The front TANK knob is SHEEN (engine
`tank`); SIZE is off the panel (engine default short); no tail/decay readout anywhere.

Front: AIR, SHEEN, OUTPUT knobs; MOOD x5; MASTER SAFE toggle + ceiling lamp; BYPASS; REAR key;
       cream label window (AIR lift meter, SHEEN activity ladders, MID ONLY + auto-gain).
Rear:  TWANG, TENSION, DRIP, LOW CUT.

Every drawing part (plate, rack, knobs, toggles, keys, lamps, 7-segment, VU needle, brackets)
comes from the approved family kit, subverselab-koro/tools/concepts.py, imported unchanged.
Only the H/A/V letters of the stroked wordmark and the HAVA layout are new here.

Outputs (docs/concepts/):
    hava_front.png               1600x640  MOOD "AIR + SHEEN", MASTER SAFE off
    hava_front_master_air.png    1600x640  MOOD "MASTER AIR", MASTER SAFE on (lamps lit)
    hava_hero_4x5.png            1080x1350 Instagram crop: AIR + SHEEN meters and knobs
    hava_rear_plate.png          1200x440  rear plate
    concepts_contact.png         KUBBE | KASET | KORO | (YANKI) | HAVA at one face height
"""

import glob
import importlib.util
import math
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
HAVA = os.path.abspath(os.path.join(HERE, ".."))
OUT = os.path.join(HAVA, "docs", "concepts")
TOOLS = os.path.abspath(os.path.join(HAVA, ".."))

_spec = importlib.util.spec_from_file_location(
    "family_kit", os.path.join(TOOLS, "subverselab-koro", "tools", "concepts.py"))
kit = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(kit)

from_kit = ("Face", "plate", "rack", "groove", "bracket", "knob", "knob_marks", "toggle", "button", "lamp",
            "red_led", "seven", "vu_meter", "screw", "polar", "_quad")
Face, plate, rack, groove, bracket, knob, knob_marks, toggle, button, lamp, red_led, seven, vu_meter, \
    screw, polar, _quad = (getattr(kit, n) for n in from_kit)
K, CREAM, INK, GOLD, MUTED, LED_RED, LED_RED_OFF = kit.K, kit.CREAM, kit.INK, kit.GOLD, kit.MUTED, \
    kit.LED_RED, kit.LED_RED_OFF
THEME = kit.THEME_A
INK2 = (73, 108, 104)

KUBBE_FACE = kit.KUBBE_FACE
KASET_FACE = kit.KASET_FACE
KORO_FACE = os.path.join(TOOLS, "subverselab-koro", "docs", "concepts", "koro_A2_front_label7seg.png")

MOODS = ["AIR LIGHT", "AIR FULL", "AIR + SHEEN", "SHEEN ONLY", "MASTER AIR"]
# tools/presets.h (engine names AIR + TANK / TANK ONLY) -- rear defaults for every preset: SIZE short, TENSION .55, DRIP .5, TWANG .7, LOW CUT 80 Hz
PRESETS = {"AIR LIGHT": (0.35, 0.10, False), "AIR FULL": (0.80, 0.15, False), "AIR + SHEEN": (0.50, 0.50, False),
           "SHEEN ONLY": (0.00, 0.60, False), "MASTER AIR": (0.50, 0.10, True)}


def state_for(mood, live_lift, sheen_act, autogain):
    air, tank, safe = PRESETS[mood]
    return dict(mood=mood, air=air, sheen=tank, safe=safe, output=0.0,
                live_lift=live_lift,        # plugin-layer meter: dB added above 8 kHz (out vs in)
                sheen_act=sheen_act,        # plugin-layer meter: sheen (tank + wash) return, lamps of 16 (L, R)
                autogain=autogain)          # engine autoGainDb() while MASTER SAFE is on


# The shelf target is exact engine maths: gain at 11 kHz = 6*AIR dB. The live values are
# representative: the selftest measured +2.87 dB at 11 kHz for AIR .5, and an auto-gain of
# -1.38..-1.33 dB for MASTER AIR on pink noise.
STATE_A = state_for("AIR + SHEEN", live_lift=2.9, sheen_act=(11, 10), autogain=None)
STATE_B = state_for("MASTER AIR", live_lift=2.8, sheen_act=(6, 6), autogain=-1.4)


# ---- the stroked wordmark: kit letters K/O/R plus H/A/V in the same stroke --------------------
def wordmark(f, name, x, y, h, col):
    w, t, r, gap = 1.34 * h, 0.19 * h, 0.30 * h, 0.30 * h
    inn = t * 0.5
    paths = []
    cx = x
    for ch in name:
        L, R, T, B, M = cx + inn, cx + w - inn, y + inn, y + h - inn, y + h * 0.5
        if ch == "H":
            paths += [[(L, T - inn), (L, B + inn)], [(R, T - inn), (R, B + inn)], [(L, M + t * 0.1), (R, M + t * 0.1)]]
        elif ch == "A":
            paths.append([(L, B + inn), (L, T + r)] + _quad((L, T + r), (L, T), (L + r, T)) + [(R - r, T)] +
                         _quad((R - r, T), (R, T), (R, T + r)) + [(R, B + inn)])
            paths.append([(L, M + t * 0.35), (R, M + t * 0.35)])
        elif ch == "V":
            paths.append([(L - inn * 0.2, T - inn), ((L + R) / 2, B), (R + inn * 0.2, T - inn)])
        else:  # K, O, R: let the kit draw them
            kit.wordmark(f, ch, cx, y, h, col)
        cx += w + gap
    wpx = max(1, round(t * K))
    for dx, dy, c in ((0, 1.3, (0, 0, 0, 95)), (0, 0, col)):
        for p in paths:
            f.d.line([((px + dx) * K, (py + dy) * K) for px, py in p], fill=c, width=wpx, joint="curve")
    return cx - gap - x


# ---- 7-segment with decimal points -------------------------------------------------------------
def seven_dp(f, x, y, h, s, pitch=None):
    """Kit seven() for the digits; a '.' lights the decimal point of the digit before it."""
    pitch = pitch or 0.78 * h
    digits, dps = "", []
    for ch in s:
        if ch == ".":
            dps.append(len(digits) - 1)
        else:
            digits += ch
    end = seven(f, x, y, h, digits, pitch=pitch)
    for i in range(len(digits)):
        d = 0.12 * h
        dx = x + (i + 1) * pitch - 0.13 * h
        box = [(dx, y + h - d), (dx + d, y + h - d), (dx + d, y + h), (dx, y + h)]
        if i in dps:
            f.glow_polys([box], LED_RED, blur=h * 0.05, strength=0.7)
        else:
            f.poly(box, LED_RED_OFF)
    return end


def red_window(f, box):
    x0, y0, x1, y1 = box
    f.rrect((x0 - 2, y0 - 2, x1 + 2, y1 + 2), 3, fill=(12, 12, 12))
    f.rrect(box, 2, fill=(28, 6, 5))


def ladder(f, x0, x1, y, n, lit_n, hot_from=None):
    red_window(f, (x0 - 5, y - 8, x1 + 5, y + 8))
    cw = (x1 - x0) / n
    lit = []
    for i in range(n):
        b = (x0 + i * cw + 0.9, y - 4.5, x0 + (i + 1) * cw - 0.9, y + 4.5)
        if i < lit_n:
            lit.append([(b[0], b[1]), (b[2], b[1]), (b[2], b[3]), (b[0], b[3])])
        else:
            f.d.rectangle(f.P(*b), fill=LED_RED_OFF)
    if lit:
        f.glow_polys(lit, LED_RED, blur=2.0, strength=0.8)


# ---- display: the cream label window ------------------------------------------------------------
WIN = (450, 34, 1400, 296)
SEC_AIR = (462, 700)
SEC_SHEEN = (720, 944)
SEC_SAFE = (964, 1388)


def sheen_ladders(f, x0, x1, y, act, n=16):
    """Two LED ladders (L, R): the sheen return (spring + wash) re the input, -30..0 dB."""
    for i, (tag, lit_n) in enumerate(zip(("L", "R"), act)):
        yy = y + i * 22
        f.text(x0 - 12, yy + 0.5, tag, "legend", 8, INK, "mm")
        ladder(f, x0, x1, yy, n, lit_n)
    for k, db in enumerate(("−30", "−24", "−18", "−12", "−6", "0")):
        xx = x0 + (x1 - x0) * k / 5
        f.line((xx, y + 31), (xx, y + 36), 1.0, INK)
        f.text(xx, y + 44, db, "legend", 7.5, INK)


def label_window(f, st):
    x0, y0, x1, y1 = WIN
    f.soft((x0 - 6, y0 - 6, x1 + 6, y1 + 6), 4, opacity=0.6, offset=(1, 4), shape="rect", radius=10)
    f.grad((x0 - 7, y0 - 7, x1 + 7, y1 + 7), (30, 33, 32), (4, 5, 5), 90, "rect", 10)
    f.grad(WIN, (245, 241, 229), (222, 214, 193), 90, "rect", 4)
    # headings + gold rule (KASET cassette-label setting)
    f.text(SEC_AIR[0] + 4, y0 + 16, "AIR · TOP-BAND LIFT", "legend", 8, INK, "lm", 1.8)
    f.text(SEC_SHEEN[0] + 4, y0 + 16, "SHEEN · ACTIVITY", "legend", 8, INK, "lm", 1.8)
    f.text(SEC_SAFE[0] + 4, y0 + 16, "MASTER SAFE", "legend", 8, INK, "lm", 1.8)
    f.text(x1 - 14, y0 + 16, "SUBVERSELAB", "legend", 7.5, INK, "rm", 1.8)
    f.d.rectangle(f.P(x0 + 12, y0 + 27, x1 - 12, y0 + 30), fill=GOLD)
    for xs in (SEC_SHEEN[0] - 10, SEC_SAFE[0] - 10, SEC_SAFE[0] + 212):
        f.d.rectangle(f.P(xs, y0 + 40, xs + 1.0, y1 - 26), fill=(*GOLD, 150))

    # ---- AIR: shelf target in 7-seg (6*AIR dB at 11 kHz), live lift on a 0..6 dB ladder
    ax = SEC_AIR[0] + 4
    red_window(f, (ax, y0 + 42, ax + 118, y0 + 118))
    target = 6.0 * st["air"]
    seven_dp(f, ax + 12, y0 + 52, 56, f"{target:.1f}", pitch=46)
    f.text(ax + 128, y0 + 58, "dB", "legend", 12, INK, "lm", 1.2)
    f.text(ax + 128, y0 + 78, "AT 11 kHz", "legend", 7.5, INK, "lm", 1.0)
    f.text(ax + 128, y0 + 92, "SET BY AIR", "fine", 7, INK2, "lm", 0.8)
    red_led(f, ax + 134, y0 + 111, 3, st["air"] > 0)
    f.text(ax + 143, y0 + 111.5, "LIFT", "legend", 7.5, INK if st["air"] > 0 else INK2, "lm", 1.2)
    lx0, lx1, ly = ax + 6, SEC_AIR[1] - 10, y0 + 170
    f.text(ax, ly - 22, "LIVE · dB ADDED ABOVE 8 kHz", "legend", 7.5, INK, "lm", 1.2)
    ladder(f, lx0, lx1, ly, 12, int(st["live_lift"] / 0.5 + 1e-6))
    for db in range(7):
        xx = lx0 + (lx1 - lx0) * db / 6
        f.line((xx, ly + 9), (xx, ly + 14), 1.0, INK)
        f.text(xx, ly + 22, str(db), "legend", 7.5, INK)
    f.text(ax, y1 - 14, "AIR 10 = +6 dB SHELF AT 11 kHz", "fine", 7, INK2, "lm", 0.8)

    # ---- SHEEN: activity ladders (no time, no decay read-out: this is a colour, not a room)
    sx0, sx1 = SEC_SHEEN
    on = st["sheen"] > 0 or st["air"] > 0
    red_led(f, sx0 + 10, y0 + 50, 4.2, on)
    f.text(sx0 + 22, y0 + 50.5, "SHEEN ON", "legend", 9, INK if on else INK2, "lm", 1.6)
    f.text(sx0 + 4, y0 + 74, "CHIRP · TWANG · SPARKLE", "legend", 7.5, INK, "lm", 1.2)
    f.text(sx0 + 4, y0 + 88, "SPRING RETURN RE INPUT, dB", "fine", 7, INK2, "lm", 0.8)
    sheen_ladders(f, sx0 + 24, sx1 - 12, y0 + 116, st["sheen_act"])
    f.text(sx0 + 4, y0 + 214, "LIGHTS ON TRANSIENTS,", "fine", 7, INK2, "lm", 0.8)
    f.text(sx0 + 4, y0 + 226, "FALLS AWAY AT ONCE", "fine", 7, INK2, "lm", 0.8)
    f.text(sx0 + 4, y1 - 14, "SHEEN · SHORT SPRING GLUE · NOT A ROOM", "fine", 7, INK2, "lm", 0.8)

    # ---- MASTER SAFE: MID ONLY lamp, ceiling lamp, auto-gain read-out
    mx = SEC_SAFE[0] + 4
    on = st["safe"]
    red_led(f, mx + 8, y0 + 50, 4.2, on)
    f.text(mx + 20, y0 + 50.5, "MID ONLY", "legend", 9, INK if on else INK2, "lm", 1.6)
    f.text(mx + 20, y0 + 64, "SIDE PASSES UNTOUCHED", "fine", 6.8, INK2, "lm", 0.6)
    red_led(f, mx + 8, y0 + 92, 4.2, on)
    f.text(mx + 20, y0 + 92.5, "CEILING −0.3 dBTP", "legend", 9, INK if on else INK2, "lm", 1.4)
    f.text(mx + 20, y0 + 106, "TRUE-PEAK, 4× ESTIMATE", "fine", 6.8, INK2, "lm", 0.6)
    red_led(f, mx + 8, y0 + 134, 4.2, on)
    f.text(mx + 20, y0 + 134.5, "LATENCY 80 SMP", "legend", 9, INK if on else INK2, "lm", 1.4)
    f.text(mx + 20, y0 + 148, "AT 48 kHz · 74 AT 44.1 kHz", "fine", 6.8, INK2, "lm", 0.6)
    f.text(mx + 4, y1 - 14, "OFF: FULL STEREO, 0 LATENCY", "fine", 7, INK2, "lm", 0.8)
    gx = SEC_SAFE[0] + 222
    f.text(gx + 4, y0 + 50.5, "AUTO-GAIN", "legend", 9, INK if on else INK2, "lm", 1.6)
    f.text(gx + 4, y0 + 64, "K-WEIGHTED, HOLDS LOUDNESS", "fine", 6.8, INK2, "lm", 0.6)
    red_window(f, (gx + 4, y0 + 82, gx + 142, y0 + 150))
    ag = st["autogain"]
    seven_dp(f, gx + 14, y0 + 91, 48, (f"{ag:+.1f}".replace("+", " ") if ag is not None else "---"), pitch=38)
    f.text(gx + 150, y0 + 104, "dB", "legend", 12, INK, "lm", 1.0)
    f.text(gx + 4, y0 + 176, "±12 dB RANGE, 1.5 S WINDOW", "fine", 6.8, INK2, "lm", 0.6)
    f.text(gx + 4, y0 + 188, "DARK WHEN MASTER SAFE IS OFF", "fine", 6.8, INK2, "lm", 0.6)


# ---- face parts ---------------------------------------------------------------------------------
def model_box(f, x, y, text="MODEL HV-72", size=10):
    tw = f.tlen(text, "legend", size, 2.4)
    f.rrect((x, y - 11, x + tw + 22, y + 11), 1, outline=GOLD, width=1.3)
    f.text(x + 11, y + 0.5, text, "legend", size, GOLD, "lm", 2.4)


def footer(f):
    t = "HAVA · MODEL HV-72 · STUDIO AIR UNIT"
    tw = f.tlen(t, "legend", 9.5, 2.6)
    cx = 800
    f.rrect((cx - tw / 2 - 18, 597, cx + tw / 2 + 18, 623), 1, outline=GOLD, width=1.3)
    f.text(cx, 610.5, t, "legend", 9.5, GOLD, "mm", 2.6)
    f.lt(72, 611, "MEASURED, NOT GUESSED", 7.5, 1.8, "fine", "lm", MUTED)
    wlen = f.logo("wordmark-only-cream.png", 1530, 611, 17, "rm")
    f.logo("mark-only-cream.png", 1530 - wlen - 8, 610, 26, "rm")


def name_and_moods(f, st):
    wordmark(f, "HAVA", 72, 34, 50, CREAM)
    f.lt(74, 110, "TOP-END AIR · SPRING SHEEN", 10.5, 3.0, anchor="lm")
    model_box(f, 74, 138)
    bracket(f, 72, 420, 172, "MOOD")
    bw, bh, g = 106, 38, 13
    for i, m in enumerate(MOODS):
        row = i // 3
        col = i % 3
        n_in_row = 3 if row == 0 else 2
        x_start = 73 + (348 - (n_in_row * bw + (n_in_row - 1) * g)) / 2
        x0 = x_start + col * (bw + g)
        y0 = 190 + row * (bh + 14)
        button(f, (x0, y0, x0 + bw, y0 + bh), m, lit=(m == st["mood"]), style="cream", size=8.2, track=1.0)


def front(st):
    f = Face(THEME)
    plate(f, (26, 70, 66), (9, 42, 39), streak=9, grain=3.2)
    rack(f, gold_edge=False)
    name_and_moods(f, st)
    label_window(f, st)
    # right column: BYPASS (top), REAR key (below)
    toggle(f, 1478, 118, False, "BYPASS")
    button(f, (1434, 246, 1522, 280), "REAR", lit=False, style="cream", size=8.2, track=1.6)
    f.lt(1478, 292, "SET-UP PANEL", 7, 1.2, "fine", col=MUTED)
    groove(f, 318)
    # big knobs, each under its own meter
    cy, r = 458, 58
    bracket(f, SEC_AIR[0] + 6, SEC_AIR[1] - 6, 334, "AIR")
    knob(f, 581, cy, r, st["air"], "AIR", lsize=12, label_dy=r * 1.86, sub="TOP-END LIFT")
    bracket(f, SEC_SHEEN[0] + 6, SEC_SHEEN[1] - 6, 334, "SHEEN")
    knob(f, 832, cy, r, st["sheen"], "SHEEN", lsize=12, label_dy=r * 1.86, sub="SPRING COLOR")
    bracket(f, SEC_SAFE[0] + 60, SEC_SAFE[1] - 60, 334, "OUT")
    knob(f, 1176, cy, r, 0.5 + st["output"] / 24.0, "OUTPUT", nums=("−12", "0", "+12"), lsize=12,
         label_dy=r * 1.86, sub="dB")
    # bottom left: MASTER SAFE with its ceiling lamp
    bracket(f, 72, 420, 334, "MASTER SAFE")
    toggle(f, 124, 440, st["safe"], None)
    f.lt(124, 500, "MASTER SAFE", 10.5, 2.4)
    lamp(f, 204, 404, 5.5, st["safe"], LED_RED, (80, 18, 12))
    f.lt(218, 404, "CEILING −0.3 dBTP · 80 SMP", 8, 1.2, anchor="lm")
    for i, s in enumerate(("MID ONLY + AUTO-GAIN.", "FOR THE MIX BUS AND MASTER.", "80 SAMPLES AT 48 kHz,",
                           "74 AT 44.1 kHz; OFF = 0.")):
        f.lt(200, 432 + i * 13, s, 7, 1.0, "fine", "lm", MUTED)
    # bottom right: nothing decorative -- the REAR key sits above; leave the plate clean
    footer(f)
    return f


# ---- rear plate --------------------------------------------------------------------------------
def rear():
    f = Face(THEME, 1200, 440)
    plate(f, (16, 52, 49), (6, 32, 30), streak=6, grain=3.0, seed=11)
    f.d.rectangle(f.P(0, 0, 1200, 440), outline=(0, 0, 0), width=round(3 * K))
    for x, y in ((18, 18), (1182, 18), (18, 422), (1182, 422)):
        screw(f, x, y, 7.5)
    f.lt(44, 40, "HAVA · MODEL HV-72 · REAR PANEL", 11, 3.0, anchor="lm")
    f.lt(44, 58, "SET-UP FOR THE SPRING SHEEN. EVERY MOOD LOADS THE MARKED DEFAULTS; CHANGE THEM ONLY TO OVERRIDE.",
         7.5, 1.2, "fine", "lm", MUTED)
    wl = f.logo("wordmark-only-cream.png", 1156, 42, 14, "rm")
    f.logo("mark-only-cream.png", 1156 - wl - 8, 41, 22, "rm")
    groove(f, 76, 30, 1170)
    bracket(f, 44, 760, 110, "SPRING SHEEN")
    r, cy = 40, 228
    knob(f, 150, cy, r, 0.70, "TWANG", lsize=10.5, label_dy=r * 2.0, sub="+10 dB AT 1.5 kHz")
    knob(f, 400, cy, r, 0.55, "TENSION", lsize=10.5, label_dy=r * 2.0, sub="CHIRP LENGTH")
    knob(f, 650, cy, r, 0.50, "DRIP", lsize=10.5, label_dy=r * 2.0, sub="TRANSIENT DRIP")
    bracket(f, 840, 1156, 110, "SHEEN SEND")
    lc = math.log(80 / 20) / math.log(300 / 20)
    knob(f, 998, cy, r, lc, "LOW CUT", nums=("20", "80", "300"), lsize=10.5, label_dy=r * 2.0, sub="Hz")
    # default marks (small gold dots where every MOOD puts the control)
    for cx, v in ((150, 0.70), (400, 0.55), (650, 0.50), (998, lc)):
        px, py = polar(cx, cy, r * 1.2, -135 + 270 * v)
        f.ellipse((px - 2.6, py - 2.6, px + 2.6, py + 2.6), fill=GOLD)
    f.lt(998, 342, "FILTERS THE SHEEN SEND ONLY.", 7, 1.0, "fine", col=MUTED)
    f.lt(998, 354, "THE DRY IS NEVER FILTERED.", 7, 1.0, "fine", col=MUTED)
    f.lt(600, 396, "● = DEFAULT, LOADED BY EVERY MOOD (TWANG 7 · TENSION 5.5 · DRIP 5 · LOW CUT 80 Hz)", 7.5, 1.2,
         "fine", col=MUTED)
    return f


# ---- hero: 4:5 slice through AIR + SHEEN----------------------------------------------------------
HERO_X0 = 444


# ---- contact sheet --------------------------------------------------------------------------------
def contact_sheet(hava):
    fh = 520
    items = [("KUBBE  ·  KB-79", KUBBE_FACE), ("KASET  ·  KS-83", KASET_FACE), ("KORO  ·  KR-77", KORO_FACE)]
    yanki = sorted(glob.glob(os.path.join(TOOLS, "subverselab-yanki", "docs", "concepts", "*front*.png")))
    if yanki:
        items.append(("YANKI  ·  YK-74", yanki[0]))
    ims = [(t, Image.open(p).convert("RGB")) for t, p in items if os.path.exists(p)]
    ims.append(("HAVA  ·  HV-72", hava))
    ims = [(t, im.resize((int(im.width * fh / im.height), fh), Image.LANCZOS)) for t, im in ims]
    gap, top = 44, 110
    Wc = sum(im.width for _, im in ims) + gap * (len(ims) + 1)
    sheet = Image.new("RGB", (Wc, top + fh + 80), (16, 19, 18))
    d = ImageDraw.Draw(sheet)
    ft = ImageFont.truetype(kit.FONTS["legend"][0], 34, index=1)
    fl = ImageFont.truetype(kit.FONTS["legend"][0], 24, index=1)
    d.text((gap, 40), "SUBVERSELAB HARDWARE LINE  —  same face height, one family", font=ft, fill=CREAM)
    x = gap
    for t, im in ims:
        sheet.paste(im, (x, top))
        d.text((x, top + fh + 24), t, font=fl, fill=GOLD if "HAVA" in t else MUTED)
        x += im.width + gap
    return sheet, [t for t, _ in ims]


def main():
    os.makedirs(OUT, exist_ok=True)
    fa = front(STATE_A)
    a = fa.face()
    a.save(os.path.join(OUT, "hava_front.png"))
    fa.hero(HERO_X0).save(os.path.join(OUT, "hava_hero_4x5.png"))
    del fa
    front(STATE_B).face().save(os.path.join(OUT, "hava_front_master_air.png"))
    rear().face().save(os.path.join(OUT, "hava_rear_plate.png"))
    sheet, names = contact_sheet(a)
    sheet.save(os.path.join(OUT, "concepts_contact.png"))
    print("contact sheet:", " | ".join(names))
    for n in sorted(os.listdir(OUT)):
        if n.endswith(".png"):
            print(n, Image.open(os.path.join(OUT, n)).size)


if __name__ == "__main__":
    main()
