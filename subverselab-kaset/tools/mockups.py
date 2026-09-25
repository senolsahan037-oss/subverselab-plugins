#!/usr/bin/env python3
"""
SubverseLab plugin mockup generator.

Two usages:

  Single product (kept for backward compatibility):
    python3 mockups.py <ui.png> <PRODUCT NAME> <MODEL> "<tagline>" <out_dir>
  Produces <slug>_hero.png, <slug>_box_left.png, <slug>_box_right.png,
  <slug>_social.png. (The old laptop mockup has been removed.)

  Full one-month Instagram content plan for both KUBBE and KASET:
    python3 mockups.py all <out_dir>
  Reads fixed inputs under subverselab-reverb/ and subverselab-kaset/ and
  writes the exact file set the October content plan is built against into
  <out_dir> (feed 4:5, carousels, stories 9:16, plus a contact sheet).

Only PIL and numpy are used (no Blender, no network). Everything runs
sequentially, one image at a time -- safe on a loaded machine.
"""

import os
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

# --------------------------------------------------------------------------
# Brand
# --------------------------------------------------------------------------

CREAM = (242, 239, 231)
DARK_GREEN = (3, 55, 51)
GOLD = (197, 160, 89)
TEAL = (42, 157, 143)
CORAL = (217, 87, 63)
NEAR_BLACK = (14, 16, 15)

FONT_PATH = "/System/Library/Fonts/HelveticaNeue.ttc"
FONT_BOLD_INDEX = 1

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
BRAND_ROOT = os.path.abspath(os.path.join(TOOLS_DIR, "..", "..", "..", "Assets", "Brand", "logo"))
MARK_CREAM = os.path.join(BRAND_ROOT, "mark-only-cream.png")
MARK_INK = os.path.join(BRAND_ROOT, "mark-only-ink.png")
WORDMARK_CREAM = os.path.join(BRAND_ROOT, "wordmark-only-cream.png")
WORDMARK_INK = os.path.join(BRAND_ROOT, "wordmark-only-ink.png")

# subverselab-kaset/tools -> 01_AI_Tools
AI_TOOLS_ROOT = os.path.abspath(os.path.join(TOOLS_DIR, "..", ".."))
REVERB_ROOT = os.path.join(AI_TOOLS_ROOT, "subverselab-reverb")
KASET_ROOT = os.path.join(AI_TOOLS_ROOT, "subverselab-kaset")

KUBBE_UI = os.path.join(REVERB_ROOT, "demos", "kubbe_typo_v2.png")
KUBBE_PANELS = [os.path.join(REVERB_ROOT, "demos", "instagram", f"panel_{i}.png") for i in range(3)]
KUBBE_SNAPSHOT_BIN = os.path.join(REVERB_ROOT, "build", "ui_snapshot_artefacts", "Release", "ui_snapshot")

KASET_UI = os.path.join(KASET_ROOT, "demos", "kaset_ui_v5.png")
KASET_SNAPSHOT_BIN = os.path.join(KASET_ROOT, "build", "ui_snapshot_artefacts", "Release", "ui_snapshot")

CANVAS_W, CANVAS_H = 2400, 1600  # legacy single-product canvas (hero)
SOCIAL_SIZE = 1080

FEED_W, FEED_H = 1080, 1350
STORY_W, STORY_H = 1080, 1920

KUBBE_PLACES = [
    ("HAMMAM", "Marble dome. Dense, bright, a flutter between the walls."),
    ("CISTERN", "Underground stone and water. Late column slaps, a long dark tail."),
    ("VALLEY", "Open air. The far slopes answer one by one, locked to your tempo."),
]
KASET_GENS = [
    ("MASTER", "One trip onto tape."),
    ("2ND DUB", "A copy of a copy. The mixtape sweet spot."),
    ("4TH DUB", "Four copies deep. Worn, warm, far away."),
]


# --------------------------------------------------------------------------
# Small helpers
# --------------------------------------------------------------------------

def font(size, bold=True):
    return ImageFont.truetype(FONT_PATH, size, index=FONT_BOLD_INDEX if bold else 0)


def load_rgba(path):
    return Image.open(path).convert("RGBA")


def fit_logo(img, target_h):
    w, h = img.size
    scale = target_h / h
    return img.resize((max(1, int(w * scale)), target_h), Image.LANCZOS)


def draw_tracked_text(draw, xy, text, fnt, fill, tracking=0, anchor_center_x=None):
    """Draw text with extra letter spacing (tracking, in px). If
    anchor_center_x is given, the whole tracked string is horizontally
    centered on that x and xy's x is ignored (xy's y is still used)."""
    x, y = xy
    widths = [draw.textlength(ch, font=fnt) for ch in text]
    total = sum(widths) + tracking * max(0, len(text) - 1)
    if anchor_center_x is not None:
        x = anchor_center_x - total / 2
    cx = x
    for ch, w in zip(text, widths):
        draw.text((cx, y), ch, font=fnt, fill=fill)
        cx += w + tracking
    return total


def tracked_text_width(draw, text, fnt, tracking=0):
    widths = [draw.textlength(ch, font=fnt) for ch in text]
    return sum(widths) + tracking * max(0, len(text) - 1)


def wrap_text(draw, text, fnt, max_width):
    words = text.split()
    lines, cur = [], ""
    for w in words:
        trial = (cur + " " + w).strip()
        if draw.textlength(trial, font=fnt) <= max_width or not cur:
            cur = trial
        else:
            lines.append(cur)
            cur = w
    if cur:
        lines.append(cur)
    return lines


def draw_centered_wrapped(draw, text, fnt, fill, center_x, top_y, max_width, line_gap=1.3):
    lines = wrap_text(draw, text, fnt, max_width)
    ascent, descent = fnt.getmetrics()
    line_h = (ascent + descent) * line_gap
    y = top_y
    for line in lines:
        w = draw.textlength(line, font=fnt)
        draw.text((center_x - w / 2, y), line, font=fnt, fill=fill)
        y += line_h
    return y


def vertical_gradient(size, top, bottom):
    w, h = size
    top_a = np.array(top, dtype=np.float32)
    bot_a = np.array(bottom, dtype=np.float32)
    t = np.linspace(0.0, 1.0, h, dtype=np.float32).reshape(h, 1)
    row = top_a * (1 - t) + bot_a * t  # (h, 3)
    arr = np.repeat(row.reshape(h, 1, 3), w, axis=1).astype(np.uint8)
    return Image.fromarray(arr, mode="RGB")


def diagonal_gradient(size, tl, br, angle_bias=0.5):
    """Gradient from top-left color to bottom-right color, used for subtle
    top-left key-light shading on flat faces."""
    w, h = size
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    t = (xx / max(1, w - 1)) * angle_bias + (yy / max(1, h - 1)) * (1 - angle_bias)
    t = np.clip(t, 0, 1)[..., None]
    top_a = np.array(tl, dtype=np.float32)
    bot_a = np.array(br, dtype=np.float32)
    arr = (top_a * (1 - t) + bot_a * t).astype(np.uint8)
    return Image.fromarray(arr, mode="RGB")


def radial_vignette(size, strength=0.35):
    """Return an RGBA black vignette overlay to darken canvas corners."""
    w, h = size
    yy, xx = np.mgrid[0:h, 0:w]
    cx, cy = w / 2.0, h / 2.0
    dist = np.sqrt(((xx - cx) / (w / 2.0)) ** 2 + ((yy - cy) / (h / 2.0)) ** 2)
    dist = np.clip(dist, 0, 1.4) / 1.4
    alpha = np.clip((dist - 0.35) / 0.65, 0, 1) ** 1.6 * strength * 255
    rgba = np.zeros((h, w, 4), dtype=np.uint8)
    rgba[..., 3] = alpha.astype(np.uint8)
    return Image.fromarray(rgba, mode="RGBA")


def soft_shadow_layer(canvas_size, box, radius=40, blur=40, opacity=120):
    """A blurred rounded-rect / ellipse shadow on a transparent layer."""
    layer = Image.new("RGBA", canvas_size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    d.rounded_rectangle(box, radius=radius, fill=(0, 0, 0, opacity))
    return layer.filter(ImageFilter.GaussianBlur(blur))


def poly_shadow_layer(canvas_size, poly, blur=50, opacity=120, expand=0):
    layer = Image.new("RGBA", canvas_size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    if expand:
        cx = sum(p[0] for p in poly) / len(poly)
        cy = sum(p[1] for p in poly) / len(poly)
        poly = [(cx + (x - cx) * (1 + expand), cy + (y - cy) * (1 + expand)) for x, y in poly]
    d.polygon(poly, fill=(0, 0, 0, opacity))
    return layer.filter(ImageFilter.GaussianBlur(blur))


def find_coeffs(pa, pb):
    """Coeffs for PIL PERSPECTIVE transform such that output pixel at pb[i]
    samples source pixel pa[i] (pa = source-space corners, pb = output-space
    corners)."""
    matrix = []
    for p1, p2 in zip(pa, pb):
        matrix.append([p2[0], p2[1], 1, 0, 0, 0, -p1[0] * p2[0], -p1[0] * p2[1]])
        matrix.append([0, 0, 0, p2[0], p2[1], 1, -p1[1] * p2[0], -p1[1] * p2[1]])
    A = np.array(matrix, dtype=np.float64)
    B = np.array(pa, dtype=np.float64).reshape(8)
    res = np.linalg.solve(A, B)
    return res


def warp_into_quad(source_rgba, canvas_size, rect, quad):
    """Place `source_rgba` (resized to fit `rect` inside a canvas_size
    transparent layer) then perspective-warp that layer so `rect`'s four
    corners land on `quad`'s four corners. Returns an RGBA layer the size of
    canvas_size."""
    x0, y0, x1, y1 = rect
    resized = source_rgba.resize((int(x1 - x0), int(y1 - y0)), Image.LANCZOS)
    layer = Image.new("RGBA", canvas_size, (0, 0, 0, 0))
    layer.paste(resized, (int(x0), int(y0)), resized)
    rect_corners = [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]
    coeffs = find_coeffs(rect_corners, quad)
    return layer.transform(canvas_size, Image.PERSPECTIVE, coeffs, Image.BICUBIC)


def lerp_pt(a, b, t):
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)


def tint(rgb, target, amount):
    """Blend rgb toward target by amount (0..1)."""
    return tuple(int(rgb[i] * (1 - amount) + target[i] * amount) for i in range(3))


def paste_contain(bg, img, box, pad=0):
    """Fit img (RGBA) inside box=(x0,y0,x1,y1), centered, preserving aspect."""
    x0, y0, x1, y1 = box
    bw, bh = (x1 - x0 - pad * 2), (y1 - y0 - pad * 2)
    scale = min(bw / img.width, bh / img.height)
    w, h = max(1, int(img.width * scale)), max(1, int(img.height * scale))
    fitted = img.resize((w, h), Image.LANCZOS)
    px = x0 + pad + (bw - w) // 2
    py = y0 + pad + (bh - h) // 2
    bg.paste(fitted, (int(px), int(py)), fitted)
    return px, py, w, h


def run_snapshot(binary, out_path, *args):
    cmd = [binary, out_path] + [str(a) for a in args]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    return load_rgba(out_path)


# --------------------------------------------------------------------------
# Canvas backgrounds
# --------------------------------------------------------------------------

def bg_dark(size, glow_pos="lower-left", vign=0.42):
    W, H = size
    bg = vertical_gradient(size, (7, 46, 43), DARK_GREEN).convert("RGBA")
    glow = Image.new("RGBA", size, (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    if glow_pos == "lower-left":
        gd.ellipse([-W * 0.35, H * 0.55, W * 0.75, H * 1.25], fill=(*TEAL, 60))
    elif glow_pos == "upper":
        gd.ellipse([-W * 0.15, -H * 0.2, W * 0.9, H * 0.35], fill=(*TEAL, 65))
    else:
        gd.ellipse([-W * 0.2, -H * 0.2, W * 0.8, H * 0.5], fill=(*TEAL, 55))
    glow = glow.filter(ImageFilter.GaussianBlur(int(W * 0.16)))
    bg = Image.alpha_composite(bg, glow)
    bg = Image.alpha_composite(bg, radial_vignette(size, vign))
    return bg


def bg_cream(size):
    W, H = size
    bg = Image.new("RGBA", size, (*CREAM, 255))
    wash = diagonal_gradient(size, (255, 255, 255), (225, 220, 205)).convert("RGBA")
    wash.putalpha(90)
    bg = Image.alpha_composite(bg, wash)
    return bg


def brand_footer(bg, mark_variant="cream", y_from_bottom=90, center_x=None, h=44):
    W, H = bg.size
    center_x = center_x if center_x is not None else W / 2
    mark = fit_logo(load_rgba(MARK_CREAM if mark_variant == "cream" else MARK_INK), h)
    bg.paste(mark, (int(center_x - mark.width / 2), H - y_from_bottom), mark)


# --------------------------------------------------------------------------
# 3D box render (front + side + top, real vanishing perspective)
# --------------------------------------------------------------------------

def render_box(ui, product_name, model, tagline, direction, size=(FEED_W, FEED_H)):
    """direction: -1 = box seen from the left (side/top recede to the left),
    +1 = box seen from the right (side/top recede to the right)."""
    W, H = size
    bg = vertical_gradient(size, (26, 28, 27), (8, 9, 9)).convert("RGBA")
    bg = Image.alpha_composite(bg, radial_vignette(size, 0.5))

    # --- box footprint --- (kept smaller than the frame so the box "breathes",
    # and sat lower so there is real floor beneath it for the shadow/reflection)
    front_w, front_h = int(W * 0.50), int(H * 0.48)
    fx0 = int(W * 0.5 - front_w / 2) - int(direction * W * 0.045)
    fy0 = int(H * 0.27)
    fx1, fy1 = fx0 + front_w, fy0 + front_h
    TL, TR, BR, BL = (fx0, fy0), (fx1, fy0), (fx1, fy1), (fx0, fy1)

    # Real (bounded) 2-point-style perspective: the top and side faces recede
    # toward a back edge that is both shifted (up + toward `direction`) and
    # narrower than the front edge -- lines genuinely converge rather than
    # running parallel (an isometric shear), but the amount is controlled
    # directly in box-relative units so it can never blow up off-canvas.
    # depth_x is the on-screen width of the side face (~32% of the front
    # face's width) and depth_y_top is the on-screen height of the top face
    # (~20% of the front face's height) -- both clearly readable as depth,
    # not a flat rectangle plus a slanted strip.
    depth_x = front_w * 0.32
    depth_y_top = front_h * 0.20
    converge = 0.78  # back edge is 78% of the corresponding front edge

    back_w = front_w * converge
    back_cx = (TL[0] + TR[0]) / 2 + direction * depth_x
    back_y_top = fy0 - depth_y_top
    back_TL = (back_cx - back_w / 2, back_y_top)
    back_TR = (back_cx + back_w / 2, back_y_top)
    top_quad = [TL, TR, back_TR, back_TL]

    back_h = front_h * converge
    if direction > 0:
        back_side_top = back_TR
        back_side_bottom = (back_TR[0], back_TR[1] + back_h)
        side_quad = [TR, back_TR, back_side_bottom, BR]
        side_near, side_far = TR, back_TR
    else:
        back_side_top = back_TL
        back_side_bottom = (back_TL[0], back_TL[1] + back_h)
        side_quad = [TL, back_TL, back_side_bottom, BL]
        side_near, side_far = TL, back_TL

    # ---------- floor reflection (drawn first, faint, below the box) ----------
    front_col = tint(CREAM, (0, 0, 0), 0.08)
    side_col = tint(DARK_GREEN, (0, 0, 0), 0.35)
    tmp = Image.new("RGBA", size, (0, 0, 0, 0))
    td = ImageDraw.Draw(tmp)
    td.polygon([TL, TR, BR, BL], fill=(*front_col, 255))
    td.polygon(side_quad, fill=(*side_col, 255))
    tmp = tmp.transpose(Image.FLIP_TOP_BOTTOM)
    dy = 2 * fy1  # mirror the box across the line y = fy1 (its base) to the floor below
    refl = Image.new("RGBA", size, (0, 0, 0, 0))
    refl.paste(tmp, (0, dy - H), tmp)
    # fade the reflection out with a downward alpha ramp
    fade = np.zeros((H, W), dtype=np.uint8)
    fade_top = fy1
    fade_h = int(H * 0.30)
    yy = np.arange(H)
    a = np.clip(1.0 - (yy - fade_top) / max(1, fade_h), 0, 1) * 95
    fade[:, :] = a[:, None].astype(np.uint8)
    r, g, b, orig_a = refl.split()
    new_a = Image.fromarray((np.array(orig_a).astype(np.float32) / 255.0 * fade).astype(np.uint8))
    refl = Image.merge("RGBA", (r, g, b, new_a))
    refl = refl.filter(ImageFilter.GaussianBlur(4))
    bg = Image.alpha_composite(bg, refl)

    # ---------- long soft shadow (light from top-left -> shadow falls lower-right) ----------
    shadow_poly = [
        BL, BR,
        (BR[0] + front_w * 0.55, BR[1] + front_h * 0.30),
        (BL[0] + front_w * 0.32, BL[1] + front_h * 0.30),
    ]
    bg = Image.alpha_composite(bg, poly_shadow_layer(size, shadow_poly, blur=55, opacity=125))
    # tight, dark contact shadow right under the box
    contact = poly_shadow_layer(size, [
        (BL[0] + 6, BL[1] - 2), (BR[0] - 6, BR[1] - 2),
        (BR[0] + 20, BR[1] + 50), (BL[0] - 20, BL[1] + 50),
    ], blur=16, opacity=225)
    bg = Image.alpha_composite(bg, contact)

    # ---------- top face (brightest: catches the top-left key light) ----------
    top_layer = Image.new("RGBA", size, (0, 0, 0, 0))
    td2 = ImageDraw.Draw(top_layer)
    top_col = tint(CREAM, (255, 255, 255), 0.45)
    td2.polygon(top_quad, fill=(*top_col, 255))
    bg = Image.alpha_composite(bg, top_layer)
    # subtle shading across the top so it isn't a flat card: brighter near the
    # front-left, cooling toward the back-right
    top_shade = Image.new("RGBA", size, (0, 0, 0, 0))
    tsd = ImageDraw.Draw(top_shade)
    tsd.polygon(top_quad, fill=(0, 0, 0, 26))
    top_shade = top_shade.filter(ImageFilter.GaussianBlur(3))
    bg = Image.alpha_composite(bg, top_shade)

    # ---------- side face (darkest: away from the key light) ----------
    side_layer = Image.new("RGBA", size, (0, 0, 0, 0))
    sd2 = ImageDraw.Draw(side_layer)
    side_shadow_amt = 0.65 if direction > 0 else 0.50
    side_col2 = tint(DARK_GREEN, (0, 0, 0), side_shadow_amt)
    sd2.polygon(side_quad, fill=(*side_col2, 255))
    bg = Image.alpha_composite(bg, side_layer)
    # AO gradient: darker toward the back (far) edge of the side face, using a
    # horizontal luminance ramp masked to the side-face polygon only.
    mask = Image.new("L", size, 0)
    md = ImageDraw.Draw(mask)
    md.polygon(side_quad, fill=255)
    ramp = diagonal_gradient(size, (0, 0, 0), (100, 100, 100), angle_bias=1.0).convert("L")
    if direction < 0:
        ramp = ramp.transpose(Image.FLIP_LEFT_RIGHT)
    side_ao = Image.new("RGBA", size, (0, 0, 0, 0))
    side_ao.putalpha(Image.composite(ramp, Image.new("L", size, 0), mask))
    bg = Image.alpha_composite(bg, side_ao)

    # model number on the side face, rotated to sit inside the parallelogram
    side_txt = Image.new("RGBA", (560, 150), (0, 0, 0, 0))
    std = ImageDraw.Draw(side_txt)
    f_side = font(44)
    draw_tracked_text(std, (0, 46), model.upper(), f_side, CREAM, tracking=10)
    side_txt = side_txt.rotate(-90 if direction > 0 else 90, expand=True)
    scx = sum(p[0] for p in side_quad) / 4
    scy = sum(p[1] for p in side_quad) / 4
    bg.paste(side_txt, (int(scx - side_txt.width / 2), int(scy - side_txt.height / 2)), side_txt)

    # wordmark on the top face
    wm = fit_logo(load_rgba(WORDMARK_INK), 30)
    wcx = (TL[0] + TR[0] + back_TL[0] + back_TR[0]) / 4
    wcy = (TL[1] + TR[1] + back_TL[1] + back_TR[1]) / 4 - 6
    bg.paste(wm, (int(wcx - wm.width / 2), int(wcy - wm.height / 2)), wm)

    # ---------- front face ----------
    front_layer = Image.new("RGBA", size, (0, 0, 0, 0))
    fdraw = ImageDraw.Draw(front_layer)
    fdraw.rectangle([fx0, fy0, fx1, fy1], fill=(*CREAM, 255))
    bg = Image.alpha_composite(bg, front_layer)
    # front-face key light: soft brighten top-left corner, darken bottom-right
    front_shade = diagonal_gradient((front_w, front_h), (255, 255, 255), (210, 205, 190)).convert("RGBA")
    front_shade.putalpha(50)
    bg.paste(front_shade, (fx0, fy0), front_shade)

    draw = ImageDraw.Draw(bg)
    draw.rectangle([fx0 + 20, fy0 + 20, fx1 - 20, fy1 - 20], outline=(*DARK_GREEN, 255), width=3)

    # brand mark + model tag on the front face
    mark = fit_logo(load_rgba(MARK_INK), 46)
    bg.paste(mark, (fx0 + 44, fy0 + 40), mark)
    f_model = font(22)
    model_txt = model.upper()
    mw = tracked_text_width(draw, model_txt, f_model, tracking=5)
    draw_tracked_text(draw, (fx1 - 44 - mw, fy0 + 50), model_txt, f_model, DARK_GREEN, tracking=5)

    # UI screenshot "window" on the front face, like a period product photo
    win_margin_x = 64
    win_y0 = fy0 + 120
    win_w = front_w - win_margin_x * 2
    win_h = int(win_w * ui.height / ui.width)
    max_win_h = int(front_h * 0.42)
    if win_h > max_win_h:
        win_h = max_win_h
        win_w = int(win_h * ui.width / ui.height)
    win_x0 = fx0 + (front_w - win_w) // 2
    ui_fit = ui.resize((win_w, win_h), Image.LANCZOS)
    bez = 12
    draw.rounded_rectangle(
        [win_x0 - bez, win_y0 - bez, win_x0 + win_w + bez, win_y0 + win_h + bez],
        radius=14, fill=NEAR_BLACK,
    )
    bg.paste(ui_fit, (win_x0, win_y0), ui_fit)

    fd = ImageDraw.Draw(bg)
    f_name = font(58)
    name_y = win_y0 + win_h + 34
    draw_tracked_text(
        fd, (0, name_y), product_name.upper(), f_name, DARK_GREEN,
        tracking=10, anchor_center_x=fx0 + front_w / 2,
    )
    f_tag = font(20, bold=False)
    tw = fd.textlength(tagline, font=f_tag)
    if tw > front_w - 100:
        # shrink slightly rather than clip
        f_tag = font(17, bold=False)
        tw = fd.textlength(tagline, font=f_tag)
    fd.text((fx0 + front_w / 2 - tw / 2, name_y + 68), tagline, font=f_tag, fill=DARK_GREEN)

    # wordmark sits ABOVE the rule, with clear margin from the inner frame
    # line below it (the rule), so it is never clipped by that border.
    wordmark = fit_logo(load_rgba(WORDMARK_INK), 22)
    rule_y = fy1 - 46
    wm_y = rule_y - 14 - wordmark.height
    bg.paste(wordmark, (fx0 + 44, wm_y), wordmark)
    draw.line([fx0 + 44, rule_y, fx1 - 44, rule_y], fill=GOLD, width=2)

    # ---------- bevel / edge highlights (catch the light) ----------
    hl = Image.new("RGBA", size, (0, 0, 0, 0))
    hd = ImageDraw.Draw(hl)
    hd.line([TL, TR], fill=(255, 255, 255, 130), width=3)
    hd.line([side_near, side_far], fill=(255, 255, 255, 70), width=2)
    hd.line([BL, BR], fill=(0, 0, 0, 90), width=2)
    hl = hl.filter(ImageFilter.GaussianBlur(0.6))
    bg = Image.alpha_composite(bg, hl)

    return bg


# --------------------------------------------------------------------------
# Hero shot (floating UI, perspective tilt, thickness edge, long shadow)
# --------------------------------------------------------------------------

def render_hero(ui, product_name, tagline, size=(FEED_W, FEED_H)):
    W, H = size
    bg = bg_dark(size, glow_pos="lower-left", vign=0.4)

    rect_w = int(W * 0.86)
    rect_h = int(rect_w * ui.height / ui.width)
    rx0 = (W - rect_w) // 2
    ry0 = int(H * 0.30)
    rx1, ry1 = rx0 + rect_w, ry0 + rect_h

    depth = 16  # panel thickness, in px, for the extra "device edge"

    # long soft shadow cast toward lower-right
    shadow_quad = [
        (rx0 + 40, ry0 + 30 + depth), (rx1 + 70, ry0 + 6 + depth),
        (rx1 + 150, ry1 + 130 + depth), (rx0 + 70, ry1 + 150 + depth),
    ]
    shadow_layer = Image.new("RGBA", size, (0, 0, 0, 0))
    sd = ImageDraw.Draw(shadow_layer)
    sd.polygon(shadow_quad, fill=(0, 0, 0, 130))
    shadow_layer = shadow_layer.filter(ImageFilter.GaussianBlur(38))
    bg = Image.alpha_composite(bg, shadow_layer)
    contact = poly_shadow_layer(size, [
        (rx0 + 60, ry1 - 10), (rx1 - 20, ry1 - 30),
        (rx1 + 10, ry1 + 40), (rx0 + 20, ry1 + 55),
    ], blur=16, opacity=150)
    bg = Image.alpha_composite(bg, contact)

    quad_top = [
        (rx0, ry0 - 8),
        (rx1 - 6, ry0 - 46),
        (rx1 - 36, ry1 - 26),
        (rx0 + 24, ry1 + 18),
    ]
    # panel "edge" quad, offset down by `depth` -> gives the tilted panel a
    # visible thickness, like a real enclosure rather than a flat sticker
    quad_edge = [(x, y + depth) for x, y in quad_top]

    edge_layer = Image.new("RGBA", size, (0, 0, 0, 0))
    ed = ImageDraw.Draw(edge_layer)
    ed.polygon([quad_top[3], quad_top[2], quad_edge[2], quad_edge[3]], fill=(*tint(GOLD, (0, 0, 0), 0.35), 255))
    ed.polygon([quad_top[2], quad_top[1], quad_edge[1], quad_edge[2]], fill=(*tint(GOLD, (0, 0, 0), 0.15), 255))
    bg = Image.alpha_composite(bg, edge_layer)

    edge_pad = 8
    frame = Image.new("RGBA", (rect_w + edge_pad * 2, rect_h + edge_pad * 2), (0, 0, 0, 0))
    fr = ImageDraw.Draw(frame)
    fr.rounded_rectangle([0, 0, frame.width, frame.height], radius=20, fill=(*GOLD, 255))
    panel = Image.new("RGBA", (rect_w, rect_h), (0, 0, 0, 0))
    panel.paste(frame, (-edge_pad, -edge_pad), frame)
    ui_fit = ui.resize((rect_w - 20, rect_h - 20), Image.LANCZOS)
    panel.paste(ui_fit, (10, 10), ui_fit)

    warped = warp_into_quad(panel, size, (0, 0, rect_w, rect_h), quad_top)
    bg = Image.alpha_composite(bg, warped)

    draw = ImageDraw.Draw(bg)
    mark = fit_logo(load_rgba(MARK_CREAM), 52)
    bg.paste(mark, (W // 2 - mark.width // 2, 64), mark)

    f_name = font(78)
    name_y = int(H * 0.855)
    draw_tracked_text(draw, (0, name_y), product_name.upper(), f_name, CREAM, tracking=14, anchor_center_x=W / 2)

    f_tag = font(24, bold=False)
    tw = draw.textlength(tagline, font=f_tag)
    draw.text((W / 2 - tw / 2, name_y + 78), tagline, font=f_tag, fill=(*GOLD, 255))

    return bg


# --------------------------------------------------------------------------
# Legacy social card (kept for the single-product CLI)
# --------------------------------------------------------------------------

def make_social_card(ui, product_name, tagline, out_path):
    S = SOCIAL_SIZE
    bg = bg_dark((S, S), glow_pos="upper", vign=0.4)
    draw = ImageDraw.Draw(bg)

    win_w = S - 160
    win_h = int(win_w * ui.height / ui.width)
    max_h = 560
    if win_h > max_h:
        win_h = max_h
        win_w = int(win_h * ui.width / ui.height)
    win_x0 = (S - win_w) // 2
    win_y0 = 130

    shadow = soft_shadow_layer(
        (S, S), (win_x0 + 20, win_y0 + 30, win_x0 + win_w + 20, win_y0 + win_h + 40),
        radius=30, blur=35, opacity=140,
    )
    bg = Image.alpha_composite(bg, shadow)
    draw = ImageDraw.Draw(bg)

    edge_pad = 8
    draw.rounded_rectangle(
        [win_x0 - edge_pad, win_y0 - edge_pad, win_x0 + win_w + edge_pad, win_y0 + win_h + edge_pad],
        radius=16, fill=GOLD,
    )
    ui_fit = ui.resize((win_w, win_h), Image.LANCZOS)
    bg.paste(ui_fit, (win_x0, win_y0), ui_fit)

    f_name = font(84)
    name_y = win_y0 + win_h + 70
    draw_tracked_text(draw, (0, name_y), product_name.upper(), f_name, CREAM, tracking=14, anchor_center_x=S / 2)

    f_tag = font(30, bold=False)
    tw = draw.textlength(tagline, font=f_tag)
    draw.text((S / 2 - tw / 2, name_y + 100), tagline, font=f_tag, fill=(*GOLD, 255))

    brand_footer(bg, "cream", y_from_bottom=120, h=50)
    bg.convert("RGB").save(out_path, quality=95)


# --------------------------------------------------------------------------
# Feed cards: place / generation panels, detail crops, spec sheets, etc.
# --------------------------------------------------------------------------

def render_image_caption_card(image, title, body, accent=GOLD, size=(FEED_W, FEED_H), badge=None):
    W, H = size
    bg = bg_dark(size, glow_pos="upper", vign=0.42)
    draw = ImageDraw.Draw(bg)

    img_box = (int(W * 0.06), int(H * 0.07), int(W * 0.94), int(H * 0.62))
    shadow = soft_shadow_layer(size, (img_box[0] + 14, img_box[1] + 22, img_box[2] + 14, img_box[3] + 30),
                                radius=26, blur=32, opacity=130)
    bg = Image.alpha_composite(bg, shadow)
    draw = ImageDraw.Draw(bg)
    px, py, pw, ph = paste_contain(bg, image, img_box)
    draw.rounded_rectangle([px - 6, py - 6, px + pw + 6, py + ph + 6], radius=10, outline=(*accent, 255), width=3)

    f_title = font(74)
    title_y = int(H * 0.68)
    draw_tracked_text(draw, (0, title_y), title.upper(), f_title, CREAM, tracking=10, anchor_center_x=W / 2)

    f_body = font(28, bold=False)
    draw_centered_wrapped(draw, body, f_body, (*accent, 255), W / 2, title_y + 96, W * 0.8, line_gap=1.25)

    if badge:
        f_b = font(24)
        bw = tracked_text_width(draw, badge, f_b, tracking=4)
        pad = 16
        bx0, by0 = W - bw - pad * 2 - 40, 40
        draw.rounded_rectangle([bx0, by0, bx0 + bw + pad * 2, by0 + 46], radius=23, fill=(*NEAR_BLACK, 200))
        draw_tracked_text(draw, (bx0 + pad, by0 + 10), badge, f_b, GOLD, tracking=4)

    brand_footer(bg, "cream", y_from_bottom=64, h=32)
    return bg


def crop_region(img, box_frac, pad_frac=0.0):
    w, h = img.size
    x0, y0, x1, y1 = box_frac
    px, py = (x1 - x0) * pad_frac * w, (y1 - y0) * pad_frac * h
    box = (max(0, int(x0 * w - px)), max(0, int(y0 * h - py)),
           min(w, int(x1 * w + px)), min(h, int(y1 * h + py)))
    return img.crop(box)


def render_detail_card(ui, box_frac, title, caption, size=(FEED_W, FEED_H), upscale=1.35, pad_frac=0.03):
    crop = crop_region(ui, box_frac, pad_frac=pad_frac)
    crop = crop.resize((int(crop.width * upscale), int(crop.height * upscale)), Image.LANCZOS)

    W, H = size
    bg = bg_dark(size, glow_pos="upper", vign=0.45)
    draw = ImageDraw.Draw(bg)

    img_box = (int(W * 0.05), int(H * 0.10), int(W * 0.95), int(H * 0.60))
    shadow = soft_shadow_layer(size, (img_box[0] + 14, img_box[1] + 20, img_box[2] + 14, img_box[3] + 28),
                                radius=22, blur=30, opacity=140)
    bg = Image.alpha_composite(bg, shadow)
    draw = ImageDraw.Draw(bg)
    px, py, pw, ph = paste_contain(bg, crop, img_box)
    draw.rounded_rectangle([px - 5, py - 5, px + pw + 5, py + ph + 5], radius=10, outline=(*GOLD, 255), width=3)
    # small "zoom" corner ticks to sell "this is a close-up"
    tick = 26
    for cx, cy, dx, dy in [(px, py, 1, 1), (px + pw, py, -1, 1), (px, py + ph, 1, -1), (px + pw, py + ph, -1, -1)]:
        draw.line([(cx, cy), (cx + tick * dx, cy)], fill=(*TEAL, 220), width=4)
        draw.line([(cx, cy), (cx, cy + tick * dy)], fill=(*TEAL, 220), width=4)

    title_size, tracking = 46, 8
    f_title = font(title_size)
    while tracked_text_width(draw, title.upper(), f_title, tracking=tracking) > W * 0.92 and title_size > 26:
        title_size -= 2
        f_title = font(title_size)
    title_y = int(H * 0.655)
    draw_tracked_text(draw, (0, title_y), title.upper(), f_title, CREAM, tracking=tracking, anchor_center_x=W / 2)

    f_cap = font(27, bold=False)
    draw_centered_wrapped(draw, caption, f_cap, (*GOLD, 255), W / 2, title_y + 62, W * 0.82, line_gap=1.3)

    brand_footer(bg, "cream", y_from_bottom=64, h=32)
    return bg


def render_spec_card(product_name, model, tagline, specs, size=(FEED_W, FEED_H)):
    W, H = size
    bg = bg_cream(size)
    draw = ImageDraw.Draw(bg)

    margin = int(W * 0.08)
    draw.rectangle([margin, margin, W - margin, H - margin], outline=(*DARK_GREEN, 255), width=3)
    draw.rectangle([margin + 10, margin + 10, W - margin - 10, H - margin - 10], outline=(*GOLD, 255), width=1)

    mark = fit_logo(load_rgba(MARK_INK), 54)
    bg.paste(mark, (margin + 34, margin + 34), mark)

    f_model = font(24)
    model_txt = model.upper()
    mw = tracked_text_width(draw, model_txt, f_model, tracking=5)
    draw_tracked_text(draw, (W - margin - 34 - mw, margin + 50), model_txt, f_model, DARK_GREEN, tracking=5)

    f_name = font(80)
    name_y = margin + 120
    draw_tracked_text(draw, (0, name_y), product_name.upper(), f_name, DARK_GREEN, tracking=12, anchor_center_x=W / 2)

    f_tag = font(26, bold=False)
    tw = draw.textlength(tagline, font=f_tag)
    draw.text((W / 2 - tw / 2, name_y + 96), tagline, font=f_tag, fill=(*GOLD, 255))

    rule_y = name_y + 150
    draw.line([margin + 40, rule_y, W - margin - 40, rule_y], fill=GOLD, width=2)

    f_label = font(26)
    f_value = font(26, bold=False)
    row_y = rule_y + 44
    row_h = (H - margin - 90 - row_y) / max(1, len(specs))
    for label, value in specs:
        draw.text((margin + 40, row_y), label.upper(), font=f_label, fill=DARK_GREEN)
        vw = draw.textlength(value, font=f_value)
        draw.text((W - margin - 40 - vw, row_y), value, font=f_value, fill=tint(DARK_GREEN, (0, 0, 0), 0.2))
        draw.line([margin + 40, row_y + row_h - 14, W - margin - 40, row_y + row_h - 14],
                   fill=(*DARK_GREEN, 60), width=1)
        row_y += row_h

    f_slogan = font(20, bold=False)
    sw = draw.textlength("MEASURED, NOT GUESSED.", font=f_slogan)
    draw.text((W / 2 - sw / 2, H - margin - 60), "MEASURED, NOT GUESSED.", font=f_slogan, fill=(*DARK_GREEN, 180))

    return bg


# --------------------------------------------------------------------------
# Duo family / quote / CTA cards
# --------------------------------------------------------------------------

def flat_front_card(ui, name, model, width=420):
    """A small flat 'product card' (mark, UI window sized to its own aspect
    ratio, name, wordmark). Height is derived from content, so there is never
    an oversized empty bezel around a wide-and-short UI screenshot."""
    w = width
    win_w = w - 48
    win_h = int(win_w * ui.height / ui.width)
    win_y0 = 78
    name_y = win_y0 + win_h + 40
    rule_y = name_y + 84
    h = rule_y + 56
    size = (w, h)
    card = Image.new("RGBA", size, (*CREAM, 255))
    d = ImageDraw.Draw(card)
    shade = diagonal_gradient(size, (255, 255, 255), (215, 210, 196)).convert("RGBA")
    shade.putalpha(60)
    card.paste(shade, (0, 0), shade)
    d.rectangle([0, 0, w - 1, h - 1], outline=(*DARK_GREEN, 255), width=3)
    d.rectangle([14, 14, w - 15, h - 15], outline=(*GOLD, 255), width=1)

    mark = fit_logo(load_rgba(MARK_INK), 34)
    card.paste(mark, (26, 24), mark)
    f_model = font(15)
    mw = tracked_text_width(d, model.upper(), f_model, tracking=3)
    draw_tracked_text(d, (w - 26 - mw, 32), model.upper(), f_model, DARK_GREEN, tracking=3)

    win_box = (24, win_y0, w - 24, win_y0 + win_h)
    d.rounded_rectangle([win_box[0] - 6, win_box[1] - 6, win_box[2] + 6, win_box[3] + 6],
                         radius=10, fill=NEAR_BLACK)
    paste_contain(card, ui, win_box, pad=2)

    f_name = font(46)
    draw_tracked_text(d, (0, name_y), name.upper(), f_name, DARK_GREEN, tracking=8, anchor_center_x=w / 2)
    d.line([26, rule_y, w - 26, rule_y], fill=GOLD, width=2)
    wm = fit_logo(load_rgba(WORDMARK_INK), 16)
    card.paste(wm, (w // 2 - wm.width // 2, h - 44), wm)
    return card


def render_duo_family(kubbe_ui, kaset_ui, size=(FEED_W, FEED_H)):
    W, H = size
    bg = bg_dark(size, glow_pos="upper", vign=0.4)
    draw = ImageDraw.Draw(bg)

    mark = fit_logo(load_rgba(MARK_CREAM), 60)
    bg.paste(mark, (W // 2 - mark.width // 2, 70), mark)

    f_head = font(34)
    head_y = 150
    draw_tracked_text(draw, (0, head_y), "SUBVERSE-LAB HARDWARE", f_head, CREAM, tracking=8, anchor_center_x=W / 2)

    card_w = int(W * 0.40)
    gap = int(W * 0.04)
    total_w = card_w * 2 + gap
    x0 = (W - total_w) // 2
    y0 = head_y + 90

    kubbe_card = flat_front_card(kubbe_ui, "KUBBE", "KB-79", width=card_w)
    kaset_card = flat_front_card(kaset_ui, "KASET", "KS-83", width=card_w)
    card_h = max(kubbe_card.height, kaset_card.height)

    for card, x in [(kubbe_card, x0), (kaset_card, x0 + card_w + gap)]:
        shadow = soft_shadow_layer(size, (x + 10, y0 + 18, x + card_w + 10, y0 + card.height + 26),
                                    radius=20, blur=26, opacity=130)
        bg2 = Image.alpha_composite(bg, shadow)
        bg.paste(bg2, (0, 0))
        bg.paste(card, (x, y0), card)

    draw = ImageDraw.Draw(bg)
    f_tag = font(26, bold=False)
    tag = "Two machines. Built the same way."
    tw = draw.textlength(tag, font=f_tag)
    draw.text((W / 2 - tw / 2, y0 + card_h + 46), tag, font=f_tag, fill=(*GOLD, 255))

    return bg


def render_quote_card(text, sub, size=(FEED_W, FEED_H)):
    W, H = size
    bg = bg_dark(size, glow_pos="all", vign=0.42)
    draw = ImageDraw.Draw(bg)

    # decorative CRT-style decay curve, faint, behind the quote
    curve = Image.new("RGBA", size, (0, 0, 0, 0))
    cd = ImageDraw.Draw(curve)
    grid_y0, grid_y1 = int(H * 0.30), int(H * 0.60)
    grid_x0, grid_x1 = int(W * 0.10), int(W * 0.90)
    for gx in range(grid_x0, grid_x1, 60):
        cd.line([(gx, grid_y0), (gx, grid_y1)], fill=(*TEAL, 24), width=1)
    for gy in range(grid_y0, grid_y1, 40):
        cd.line([(grid_x0, gy), (grid_x1, gy)], fill=(*TEAL, 24), width=1)
    pts = []
    n = 120
    for i in range(n):
        x = grid_x0 + (grid_x1 - grid_x0) * i / (n - 1)
        tnorm = i / (n - 1)
        decay = np.exp(-tnorm * 4.2)
        y = grid_y1 - decay * (grid_y1 - grid_y0) * 0.92
        pts.append((x, y))
    cd.line(pts, fill=(*TEAL, 210), width=4, joint="curve")
    curve = curve.filter(ImageFilter.GaussianBlur(0.4))
    bg = Image.alpha_composite(bg, curve)
    draw = ImageDraw.Draw(bg)

    f_q = font(66)
    qy = int(H * 0.66)
    draw_centered_wrapped(draw, text.upper(), f_q, CREAM, W / 2, qy, W * 0.84, line_gap=1.15)

    f_sub = font(26, bold=False)
    sw = draw.textlength(sub, font=f_sub)
    draw.text((W / 2 - sw / 2, qy + 200), sub, font=f_sub, fill=(*GOLD, 255))

    brand_footer(bg, "cream", y_from_bottom=90, h=42)
    return bg


def render_text_card(headline, lines, size=(FEED_W, FEED_H), accent=GOLD, ui_thumb=None, glow="upper"):
    W, H = size
    bg = bg_dark(size, glow_pos=glow, vign=0.42)
    draw = ImageDraw.Draw(bg)

    y = int(H * 0.10)
    mark = fit_logo(load_rgba(MARK_CREAM), 54)
    bg.paste(mark, (W // 2 - mark.width // 2, y), mark)
    y += 90

    if ui_thumb is not None:
        box = (int(W * 0.14), y, int(W * 0.86), y + int(H * 0.30))
        shadow = soft_shadow_layer(size, (box[0] + 10, box[1] + 16, box[2] + 10, box[3] + 22),
                                    radius=20, blur=26, opacity=130)
        bg = Image.alpha_composite(bg, shadow)
        draw = ImageDraw.Draw(bg)
        px, py, pw, ph = paste_contain(bg, ui_thumb, box)
        draw.rounded_rectangle([px - 5, py - 5, px + pw + 5, py + ph + 5], radius=10, outline=(*accent, 255), width=3)
        y = py + ph + 60
    else:
        y += 40

    f_head = font(84)
    draw_tracked_text(draw, (0, y), headline.upper(), f_head, CREAM, tracking=10, anchor_center_x=W / 2)
    y += 108

    f_line = font(30, bold=False)
    for line in lines:
        y = draw_centered_wrapped(draw, line, f_line, (*accent, 255), W / 2, y, W * 0.82, line_gap=1.3) + 14

    brand_footer(bg, "cream", y_from_bottom=70, h=34)
    return bg


# --------------------------------------------------------------------------
# Carousel chrome (page pill + progress dots)
# --------------------------------------------------------------------------

def add_carousel_chrome(base, index, total):
    img = base.convert("RGBA").copy()
    W, H = img.size
    draw = ImageDraw.Draw(img)

    f = font(24)
    label = f"{index}/{total}"
    lw = draw.textlength(label, font=f)
    pad = 14
    # top-left, so it never collides with an existing top-right badge
    # (PLACE x/3, GENERATION) already baked into the source card.
    bx0, by0 = 34, 34
    draw.rounded_rectangle([bx0, by0, bx0 + lw + pad * 2, by0 + 42], radius=21, fill=(*NEAR_BLACK, 190))
    draw.text((bx0 + pad, by0 + 8), label, font=f, fill=CREAM)

    dot_r = 6
    gap = 20
    total_w = (total - 1) * gap
    cx0 = W / 2 - total_w / 2
    cy = H - 30
    for i in range(total):
        cx = cx0 + i * gap
        col = GOLD if i == index - 1 else (*CREAM, 110)
        draw.ellipse([cx - dot_r, cy - dot_r, cx + dot_r, cy + dot_r], fill=col if i == index - 1 else None,
                     outline=(*CREAM, 160), width=1)
    return img


# --------------------------------------------------------------------------
# Story cards (9:16)
# --------------------------------------------------------------------------

def render_countdown_story(number, unit_label, deadline_label, size=(STORY_W, STORY_H)):
    W, H = size
    bg = bg_dark(size, glow_pos="all", vign=0.35)
    draw = ImageDraw.Draw(bg)

    mark = fit_logo(load_rgba(MARK_CREAM), 64)
    bg.paste(mark, (W // 2 - mark.width // 2, int(H * 0.12)), mark)

    f_num = font(560)
    num_y = int(H * 0.30)
    draw_tracked_text(draw, (0, num_y), number, f_num, GOLD, tracking=0, anchor_center_x=W / 2)

    f_unit = font(46)
    draw_tracked_text(draw, (0, int(H * 0.62)), unit_label.upper(), f_unit, CREAM, tracking=10, anchor_center_x=W / 2)

    f_sub = font(30, bold=False)
    sw = draw.textlength(deadline_label, font=f_sub)
    draw.text((W / 2 - sw / 2, int(H * 0.68)), deadline_label, font=f_sub, fill=(*GOLD, 255))

    f_name = font(46)
    draw_tracked_text(draw, (0, int(H * 0.84)), "KUBBE · KASET — OCTOBER 1", f_name, CREAM,
                       tracking=6, anchor_center_x=W / 2)

    return bg


def render_out_now_story(ui, size=(STORY_W, STORY_H)):
    W, H = size
    bg = bg_dark(size, glow_pos="upper", vign=0.35)
    draw = ImageDraw.Draw(bg)

    mark = fit_logo(load_rgba(MARK_CREAM), 60)
    bg.paste(mark, (W // 2 - mark.width // 2, int(H * 0.08)), mark)

    box = (int(W * 0.08), int(H * 0.16), int(W * 0.92), int(H * 0.55))
    shadow = soft_shadow_layer(size, (box[0] + 10, box[1] + 18, box[2] + 10, box[3] + 26),
                                radius=24, blur=30, opacity=140)
    bg = Image.alpha_composite(bg, shadow)
    draw = ImageDraw.Draw(bg)
    px, py, pw, ph = paste_contain(bg, ui, box)
    draw.rounded_rectangle([px - 6, py - 6, px + pw + 6, py + ph + 6], radius=12, outline=(*GOLD, 255), width=3)

    f_head = font(88)
    y = py + ph + 90
    draw_tracked_text(draw, (0, y), "OUT NOW", f_head, CREAM, tracking=14, anchor_center_x=W / 2)
    y += 116
    f_sub = font(40)
    draw_tracked_text(draw, (0, y), "KUBBE — KB-79", f_sub, GOLD, tracking=6, anchor_center_x=W / 2)
    y += 70
    f_tag = font(30, bold=False)
    tag = "A reverb built from three real places."
    tw = draw.textlength(tag, font=f_tag)
    draw.text((W / 2 - tw / 2, y), tag, font=f_tag, fill=(*CREAM, 230))

    return bg


def render_dm_story(size=(STORY_W, STORY_H)):
    W, H = size
    bg = bg_dark(size, glow_pos="all", vign=0.35)
    draw = ImageDraw.Draw(bg)

    mark = fit_logo(load_rgba(MARK_CREAM), 64)
    bg.paste(mark, (W // 2 - mark.width // 2, int(H * 0.14)), mark)

    f_head = font(70)
    y = int(H * 0.34)
    y = draw_centered_wrapped(draw, "FREE FOR THE FIRST 1,000 USERS", f_head, CREAM, W / 2, y, W * 0.84, line_gap=1.2)

    y += 60
    pill_txt = 'DM "KUBBE"'
    f_pill = font(52)
    pw = tracked_text_width(draw, pill_txt.upper(), f_pill, tracking=6)
    pad = 34
    px0 = W / 2 - pw / 2 - pad
    draw.rounded_rectangle([px0, y, px0 + pw + pad * 2, y + 100], radius=50, fill=GOLD)
    draw_tracked_text(draw, (0, y + 22), pill_txt.upper(), f_pill, DARK_GREEN, tracking=6, anchor_center_x=W / 2)

    y += 160
    f_sub = font(32, bold=False)
    sub = "KUBBE — KB-79. Out October 1."
    sw = draw.textlength(sub, font=f_sub)
    draw.text((W / 2 - sw / 2, y), sub, font=f_sub, fill=(*GOLD, 255))

    # a simple downward arrow hinting at the DM action
    ax = W / 2
    ay0, ay1 = y + 70, y + 150
    draw.line([(ax, ay0), (ax, ay1)], fill=(*CREAM, 200), width=4)
    draw.polygon([(ax - 18, ay1 - 18), (ax + 18, ay1 - 18), (ax, ay1 + 14)], fill=(*CREAM, 200))

    return bg


def render_kaset_teaser_story(ui, size=(STORY_W, STORY_H)):
    W, H = size
    bg = bg_dark(size, glow_pos="all", vign=0.55)
    draw = ImageDraw.Draw(bg)

    box = (int(W * 0.10), int(H * 0.22), int(W * 0.90), int(H * 0.58))
    px, py, pw, ph = paste_contain(bg, ui, box)
    # dim + blur a copy of the UI to keep it a "teaser", not a full reveal
    reveal = bg.crop((px, py, px + pw, py + ph)).filter(ImageFilter.GaussianBlur(6))
    dark = Image.new("RGBA", reveal.size, (0, 0, 0, 110))
    reveal = Image.alpha_composite(reveal, dark)
    bg.paste(reveal, (px, py))
    draw = ImageDraw.Draw(bg)
    draw.rounded_rectangle([px - 6, py - 6, px + pw + 6, py + ph + 6], radius=12, outline=(*GOLD, 200), width=2)

    f_head = font(72)
    y = py + ph + 90
    draw_tracked_text(draw, (0, y), "SOMETHING'S COMING", f_head, CREAM, tracking=8, anchor_center_x=W / 2)
    y += 110
    f_name = font(60)
    draw_tracked_text(draw, (0, y), "KASET", f_name, GOLD, tracking=18, anchor_center_x=W / 2)
    y += 76
    f_tag = font(30, bold=False)
    tag = "Mixtape color for your mix bus."
    y = draw_centered_wrapped(draw, tag, f_tag, (*CREAM, 220), W / 2, y, W * 0.78, line_gap=1.25)
    y += 18
    f_date = font(36)
    draw_tracked_text(draw, (0, y), "OUT OCTOBER 1", f_date, GOLD, tracking=8, anchor_center_x=W / 2)

    return bg


def render_double_launch_story(kubbe_ui, kaset_ui, size=(STORY_W, STORY_H)):
    W, H = size
    bg = bg_dark(size, glow_pos="all", vign=0.35)
    draw = ImageDraw.Draw(bg)

    mark = fit_logo(load_rgba(MARK_CREAM), 60)
    bg.paste(mark, (W // 2 - mark.width // 2, int(H * 0.06)), mark)

    f_head = font(70)
    y = int(H * 0.145)
    draw_tracked_text(draw, (0, y), "OUT NOW", f_head, CREAM, tracking=14, anchor_center_x=W / 2)
    y += 90
    f_date = font(34)
    draw_tracked_text(draw, (0, y), "OCTOBER 1", f_date, GOLD, tracking=10, anchor_center_x=W / 2)

    gap = int(W * 0.06)
    card_w = int((W - gap * 3) / 2)
    y0 = y + 60
    x0 = gap
    kubbe_card = flat_front_card(kubbe_ui, "KUBBE", "KB-79", width=card_w)
    kaset_card = flat_front_card(kaset_ui, "KASET", "KS-83", width=card_w)
    card_h = max(kubbe_card.height, kaset_card.height)
    for card, x in [(kubbe_card, x0), (kaset_card, x0 + card_w + gap)]:
        shadow = soft_shadow_layer(size, (x + 8, y0 + 14, x + card_w + 8, y0 + card.height + 20),
                                    radius=18, blur=24, opacity=140)
        bg2 = Image.alpha_composite(bg, shadow)
        bg.paste(bg2, (0, 0))
        bg.paste(card, (x, y0), card)

    draw = ImageDraw.Draw(bg)
    f_tag = font(30, bold=False)
    tag = "Two machines. Built the same way."
    y2 = y0 + card_h + 70
    tw = draw.textlength(tag, font=f_tag)
    draw.text((W / 2 - tw / 2, y2), tag, font=f_tag, fill=(*GOLD, 255))

    return bg


def render_kaset_poll_story(size=(STORY_W, STORY_H)):
    W, H = size
    bg = bg_dark(size, glow_pos="upper", vign=0.35)
    draw = ImageDraw.Draw(bg)

    mark = fit_logo(load_rgba(MARK_CREAM), 56)
    bg.paste(mark, (W // 2 - mark.width // 2, int(H * 0.10)), mark)

    f_q = font(58)
    y = int(H * 0.24)
    y = draw_centered_wrapped(draw, "WHICH COPY DO YOU HEAR?", f_q, CREAM, W / 2, y, W * 0.82, line_gap=1.2)

    y += 50
    f_opt = font(40)
    for label in ["MASTER", "4TH DUB"]:
        lw = tracked_text_width(draw, label, f_opt, tracking=6)
        pad = 30
        x0 = W / 2 - lw / 2 - pad
        draw.rounded_rectangle([x0, y, x0 + lw + pad * 2, y + 84], radius=16, outline=(*GOLD, 255), width=3)
        draw_tracked_text(draw, (0, y + 18), label, f_opt, GOLD, tracking=6, anchor_center_x=W / 2)
        y += 110

    f_sub = font(26, bold=False)
    sub = "KASET — KS-83. Mixtape color for your mix bus."
    sw = draw.textlength(sub, font=f_sub)
    draw.text((W / 2 - sw / 2, y + 20), sub, font=f_sub, fill=(*CREAM, 200))

    # lower third left clear for Instagram's native poll sticker
    return bg


# --------------------------------------------------------------------------
# Contact sheet
# --------------------------------------------------------------------------

def render_contact_sheet(entries, cols=6):
    """entries: list of (filename, PIL.Image RGBA)."""
    thumb_w = 300
    label_h = 34
    pad = 14
    rows = (len(entries) + cols - 1) // cols
    cell_h_max = max(int(thumb_w * im.height / im.width) for _, im in entries)
    cell_w = thumb_w + pad * 2
    cell_h = cell_h_max + label_h + pad * 2

    W = cell_w * cols
    H = cell_h * rows + 120
    sheet = bg_dark((W, H), glow_pos="all", vign=0.25)
    draw = ImageDraw.Draw(sheet)

    f_head = font(40)
    draw_tracked_text(draw, (0, 30), "SUBVERSE-LAB — OCTOBER INSTAGRAM PLAN — CONTACT SHEET", f_head, CREAM,
                       tracking=4, anchor_center_x=W / 2)

    f_label = font(14, bold=False)
    for i, (name, im) in enumerate(entries):
        col, row = i % cols, i // cols
        x0 = col * cell_w + pad
        y0 = 100 + row * cell_h + pad
        th = int(thumb_w * im.height / im.width)
        thumb = im.convert("RGB").resize((thumb_w, th), Image.LANCZOS)
        draw.rectangle([x0 - 3, y0 - 3, x0 + thumb_w + 3, y0 + th + 3], outline=(*GOLD, 180), width=1)
        sheet.paste(thumb, (x0, y0 + (cell_h_max - th) // 2))
        label = name if len(name) <= 26 else name[:23] + "..."
        lw = draw.textlength(label, font=f_label)
        draw.text((x0 + thumb_w / 2 - lw / 2, y0 + cell_h_max + 8), label, font=f_label, fill=(*CREAM, 220))

    return sheet


# --------------------------------------------------------------------------
# Legacy single-product pipeline (laptop mockup removed per brand request)
# --------------------------------------------------------------------------

def run_single(ui_path, product_name, model, tagline, out_dir):
    ui = load_rgba(ui_path)
    os.makedirs(out_dir, exist_ok=True)
    slug = product_name.strip().lower().replace(" ", "_")

    hero = render_hero(ui, product_name, tagline, size=(CANVAS_W, CANVAS_H))
    p = os.path.join(out_dir, f"{slug}_hero.png")
    hero.convert("RGB").save(p, quality=95)
    print("wrote", p)

    for direction, tag in [(-1, "left"), (1, "right")]:
        box = render_box(ui, product_name, model, tagline, direction, size=(CANVAS_W, CANVAS_H))
        p = os.path.join(out_dir, f"{slug}_box_{tag}.png")
        box.convert("RGB").save(p, quality=95)
        print("wrote", p)

    p = os.path.join(out_dir, f"{slug}_social.png")
    make_social_card(ui, product_name, tagline, p)
    print("wrote", p)


# --------------------------------------------------------------------------
# Full content-plan pipeline
# --------------------------------------------------------------------------

def save(img, out_dir, name, registry):
    path = os.path.join(out_dir, name)
    img.convert("RGB").save(path, quality=95)
    registry.append((name, img))
    print("wrote", path)
    return img


def run_all(out_dir):
    os.makedirs(out_dir, exist_ok=True)
    registry = []  # (filename, RGBA image) for the contact sheet

    kubbe_ui = load_rgba(KUBBE_UI)
    kaset_ui = load_rgba(KASET_UI)

    # ---------------- KUBBE ----------------
    save(render_box(kubbe_ui, "KUBBE", "KB-79", "A reverb built from three real places.", -1),
         out_dir, "kubbe_box_left.png", registry)
    save(render_box(kubbe_ui, "KUBBE", "KB-79", "A reverb built from three real places.", 1),
         out_dir, "kubbe_box_right.png", registry)
    save(render_hero(kubbe_ui, "KUBBE", "A reverb built from three real places."),
         out_dir, "kubbe_hero.png", registry)

    place_imgs = []
    for i, (label, desc) in enumerate(KUBBE_PLACES):
        panel = load_rgba(KUBBE_PANELS[i])
        img = render_image_caption_card(panel, label, desc, accent=GOLD, badge=f"PLACE {i + 1}/3")
        fname = f"kubbe_place_{label.lower()}.png"
        save(img, out_dir, fname, registry)
        place_imgs.append(img)

    save(render_detail_card(kubbe_ui, (0.310, 0.057, 0.692, 0.598), "CRT DECAY DISPLAY",
                             "The decay you see is measured, not a nominal number.", pad_frac=0.008),
         out_dir, "kubbe_detail_crt.png", registry)
    save(render_detail_card(kubbe_ui, (0.700, 0.057, 0.985, 0.598), "VFD + DUCK METER",
                             "A live readout: exact T60, and how much the tail is ducking under the mix.", pad_frac=0.008),
         out_dir, "kubbe_detail_vfd.png", registry)
    save(render_detail_card(kubbe_ui, (0.453, 0.545, 0.745, 0.960), "MACHINED KNOBS",
                             "Mix, decay, distance, tone — every stage under your hand.", pad_frac=0.01),
         out_dir, "kubbe_detail_knobs.png", registry)

    save(render_spec_card("KUBBE", "KB-79", "A reverb built from three real places.", [
        ("Spaces", "Hammam · Cistern · Valley"),
        ("T60 range", "0.4s – 7.2s"),
        ("Formats", "VST3 / AU / macOS"),
        ("Freeze", "Infinite hold, any space"),
        ("Echo Sync", "Valley — tempo-locked slaps"),
        ("Ducking", "Sidechains the tail to your mix"),
    ]), out_dir, "kubbe_spec.png", registry)

    # ---------------- KASET ----------------
    save(render_box(kaset_ui, "KASET", "KS-83", "Mixtape color for your mix bus.", -1),
         out_dir, "kaset_box_left.png", registry)
    save(render_box(kaset_ui, "KASET", "KS-83", "Mixtape color for your mix bus.", 1),
         out_dir, "kaset_box_right.png", registry)
    save(render_hero(kaset_ui, "KASET", "Mixtape color for your mix bus."),
         out_dir, "kaset_hero.png", registry)

    gen_specs = [(0, "master"), (2, "2nddub"), (4, "4thdub")]
    gen_imgs = []
    for (gen_idx, slug), (label, desc) in zip(gen_specs, KASET_GENS):
        snap_path = os.path.join(out_dir, f"_tmp_kaset_gen_{gen_idx}.png")
        gen_ui = run_snapshot(KASET_SNAPSHOT_BIN, snap_path, gen_idx, 0, 0.15, 1)
        img = render_image_caption_card(gen_ui, label, desc, accent=TEAL, badge="GENERATION")
        fname = f"kaset_gen_{slug}.png"
        save(img, out_dir, fname, registry)
        gen_imgs.append(img)
        os.remove(snap_path)

    tape_renders = []
    for tape_idx in (0, 1, 2):
        snap_path = os.path.join(out_dir, f"_tmp_kaset_tape_{tape_idx}.png")
        tape_renders.append(run_snapshot(KASET_SNAPSHOT_BIN, snap_path, 1, tape_idx, 0.15, 1))
        os.remove(snap_path)
    save(render_tape_types_card(tape_renders), out_dir, "kaset_tape_types.png", registry)

    save(render_detail_card(kaset_ui, (0.415, 0.560, 0.590, 0.800), "TAPE COUNTER",
                             "A mechanical count of every pass the signal has made through tape."),
         out_dir, "kaset_detail_dial.png", registry)
    save(render_detail_card(kaset_ui, (0.030, 0.485, 0.360, 0.870), "TWO-DECK TRANSPORT",
                             "Record deck to playback deck — the dub path, modeled pass by pass."),
         out_dir, "kaset_detail_deck.png", registry)
    save(render_detail_card(kaset_ui, (0.045, 0.265, 0.520, 0.530), 'RECORD · TRANSPORT · TAPE · OUTPUT',
                             "Input, saturation, wow, flutter, hiss, wear, mix, output.", pad_frac=0.01),
         out_dir, "kaset_detail_knobs.png", registry)

    save(render_spec_card("KASET", "KS-83", "Mixtape color for your mix bus.", [
        ("Generations", "Master · 1st · 2nd · 3rd · 4th Dub"),
        ("Tape types", "Normal · Chrome · Metal"),
        ("Formats", "VST3 / AU / macOS"),
        ("Wow & flutter", "Varies per generation"),
        ("Auto gain", "Keeps level honest through the dubs"),
        ("Character", "Hiss, wear and saturation compound with age"),
    ]), out_dir, "kaset_spec.png", registry)

    # ---------------- shared feed pieces ----------------
    save(render_duo_family(kubbe_ui, kaset_ui), out_dir, "duo_family.png", registry)
    save(render_quote_card("Measured, not guessed.", "The SubverseLab standard."),
         out_dir, "quote_measured.png", registry)
    save(render_text_card("KUBBE", [
        "Free for the first 1,000 users.",
        'DM "KUBBE".',
        "Out October 1.",
    ], accent=GOLD, ui_thumb=kubbe_ui), out_dir, "cta_kubbe_oct1.png", registry)
    kaset_cta_img = render_text_card("KASET", [
        "Mixtape color for your mix bus.",
        'DM "KASET" to reserve yours.',
        "Out October 1.",
    ], accent=TEAL, ui_thumb=kaset_ui, glow="lower-left")
    save(kaset_cta_img, out_dir, "cta_kaset_soon.png", registry)

    # ---------------- carousels ----------------
    kubbe_hero_img = [im for n, im in registry if n == "kubbe_hero.png"][0]
    kubbe_cta_img = [im for n, im in registry if n == "cta_kubbe_oct1.png"][0]
    kubbe_carousel_sources = [kubbe_hero_img] + place_imgs + [kubbe_cta_img]
    for i, src in enumerate(kubbe_carousel_sources, start=1):
        save(add_carousel_chrome(src, i, 5), out_dir, f"kubbe_carousel_{i}.png", registry)

    kaset_hero_img = [im for n, im in registry if n == "kaset_hero.png"][0]
    kaset_carousel_sources = [kaset_hero_img] + gen_imgs + [kaset_cta_img]
    for i, src in enumerate(kaset_carousel_sources, start=1):
        save(add_carousel_chrome(src, i, 5), out_dir, f"kaset_carousel_{i}.png", registry)

    # ---------------- stories ----------------
    save(render_countdown_story("3", "days to launch", "Out October 1"), out_dir, "story_kubbe_countdown_3.png", registry)
    save(render_countdown_story("2", "days to launch", "Out October 1"), out_dir, "story_kubbe_countdown_2.png", registry)
    save(render_countdown_story("1", "day to launch", "Out October 1"), out_dir, "story_kubbe_countdown_1.png", registry)
    save(render_out_now_story(kubbe_ui), out_dir, "story_kubbe_out_now.png", registry)
    save(render_dm_story(), out_dir, "story_kubbe_dm.png", registry)
    save(render_kaset_teaser_story(kaset_ui), out_dir, "story_kaset_teaser.png", registry)
    save(render_kaset_poll_story(), out_dir, "story_kaset_poll.png", registry)
    save(render_double_launch_story(kubbe_ui, kaset_ui), out_dir, "story_double_launch.png", registry)

    # ---------------- contact sheet ----------------
    sheet = render_contact_sheet(registry)
    sheet.convert("RGB").save(os.path.join(out_dir, "contact_sheet.png"), quality=92)
    print("wrote", os.path.join(out_dir, "contact_sheet.png"))
    print(f"\n{len(registry)} assets + 1 contact sheet written to {out_dir}")


def render_tape_types_card(renders, size=(FEED_W, FEED_H)):
    labels = ["NORMAL", "CHROME", "METAL"]
    W, H = size
    bg = bg_dark(size, glow_pos="upper", vign=0.42)
    draw = ImageDraw.Draw(bg)

    f_title = font(58)
    draw_tracked_text(draw, (0, int(H * 0.06)), "TAPE SELECTOR", f_title, CREAM, tracking=10, anchor_center_x=W / 2)

    # crop just the cassette-shell region from each render for a tight 3-up
    crop_box = (0.030, 0.485, 0.360, 0.870)
    crops = [crop_region(r, crop_box, pad_frac=0.02) for r in renders]

    gap = 24
    cell_w = int((W * 0.90 - gap * 2) / 3)
    x0 = int(W * 0.05)
    y0 = int(H * 0.16)
    cell_h = int(H * 0.42)
    for i, (crop, label) in enumerate(zip(crops, labels)):
        cx0 = x0 + i * (cell_w + gap)
        box = (cx0, y0, cx0 + cell_w, y0 + cell_h)
        shadow = soft_shadow_layer(size, (box[0] + 8, box[1] + 12, box[2] + 8, box[3] + 18),
                                    radius=16, blur=20, opacity=130)
        bg2 = Image.alpha_composite(bg, shadow)
        bg.paste(bg2, (0, 0))
        draw = ImageDraw.Draw(bg)
        px, py, pw, ph = paste_contain(bg, crop, box)
        draw.rounded_rectangle([px - 4, py - 4, px + pw + 4, py + ph + 4], radius=8, outline=(*GOLD, 255), width=2)
        f_lbl = font(30)
        draw_tracked_text(draw, (0, y0 + cell_h + 26), label, f_lbl, GOLD, tracking=5,
                           anchor_center_x=cx0 + cell_w / 2)

    f_body = font(28, bold=False)
    draw_centered_wrapped(draw, "Three tape formulations, three colors of saturation.", f_body, (*CREAM, 220),
                           W / 2, y0 + cell_h + 78, W * 0.8, line_gap=1.25)

    brand_footer(bg, "cream", y_from_bottom=64, h=32)
    return bg


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

def main():
    if len(sys.argv) == 3 and sys.argv[1] == "all":
        run_all(sys.argv[2])
        return
    if len(sys.argv) == 6:
        ui_path, product_name, model, tagline, out_dir = sys.argv[1:6]
        run_single(ui_path, product_name, model, tagline, out_dir)
        return
    print(__doc__)
    sys.exit(1)


if __name__ == "__main__":
    main()
