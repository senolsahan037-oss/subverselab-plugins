#!/usr/bin/env python3
"""KIYMA's own box: an old cast-iron grinder carton, in the plug-in's own look.

    python3 tools/box_mockup.py <grinder_2x.png> <bank_2x.png> <ui_2x.png> <out_dir>

The three inputs come from build/ui_snapshot_artefacts/Release/ui_snapshot:
    --preset 1 --t 6 --fake-audio --mix 1 --scale 2 --crop 290,70,620,470   (grinder)
    --preset 1 --t 3 --scale 2 --crop 900,40,270,232                         (switch bank)
    --preset 1 --t 3 --fake-audio --mix 1 --scale 2                          (whole face)

Faces are drawn flat, then warped into the box with the family generator's
perspective helpers (subverselab-kaset/tools/mockups.py), so the box sits in
the same shot as the other SubverseLab boxes while wearing KIYMA's colours.
"""

import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "..", "subverselab-kaset", "tools"))
import mockups as fam  # noqa: E402  (perspective, shadow and background helpers)

ASSETS = os.path.join(ROOT, "Assets")
FONTS = os.path.join(ASSETS, "Fonts")

# Source/Theme.h
BACKDROP = (18, 17, 18)
CREAM = (239, 230, 210)
OXBLOOD = (122, 23, 18)
MUSTARD = (212, 154, 42)
COPPER = (184, 103, 63)
IRON = (27, 26, 26)


def shrikhand(size):
    return ImageFont.truetype(os.path.join(FONTS, "Shrikhand-Regular.ttf"), size)


def oswald(size, weight=600):
    f = ImageFont.truetype(os.path.join(FONTS, "Oswald-Variable.ttf"), size)
    try:
        f.set_variation_by_axes([weight])
    except Exception:
        pass
    return f


def tracked(draw, cx, y, text, fnt, fill, tracking):
    w = sum(draw.textlength(c, font=fnt) for c in text) + tracking * (len(text) - 1)
    x = cx - w / 2
    for c in text:
        draw.text((x, y), c, font=fnt, fill=fill)
        x += draw.textlength(c, font=fnt) + tracking
    return w


def feather(img, px):
    """Soft alpha edge so a snapshot crop melts into the same backdrop."""
    w, h = img.size
    m = Image.new("L", (w, h), 0)
    ImageDraw.Draw(m).rectangle([px, px, w - px, h - px], fill=255)
    m = m.filter(ImageFilter.GaussianBlur(px / 2))
    out = img.convert("RGBA")
    out.putalpha(m)
    return out


def iron_texture(size, darken=0.0):
    """The plug-in's own backdrop, cropped to fill `size`."""
    bd = Image.open(os.path.join(ASSETS, "backdrop@2x.jpg")).convert("RGB")
    W, H = size
    s = max(W / bd.width, H / bd.height)
    bd = bd.resize((int(bd.width * s) + 1, int(bd.height * s) + 1), Image.LANCZOS)
    bd = bd.crop(((bd.width - W) // 2, (bd.height - H) // 2, (bd.width - W) // 2 + W, (bd.height - H) // 2 + H))
    if darken:
        bd = Image.blend(bd, Image.new("RGB", size, (0, 0, 0)), darken)
    return bd


def matched_texture(size, crop):
    """Backdrop darkened so its level matches the border of the snapshot crop."""
    a = np.asarray(crop.convert("RGB"), dtype=np.float32)
    edge = np.concatenate([a[:40].reshape(-1, 3), a[-40:].reshape(-1, 3), a[:, :40].reshape(-1, 3), a[:, -40:].reshape(-1, 3)])
    tex = iron_texture(size)
    t = np.asarray(tex, dtype=np.float32)
    gain = edge.mean(0) / np.maximum(t.mean(axis=(0, 1)), 1)
    return Image.fromarray(np.clip(t * gain, 0, 255).astype(np.uint8))


def frame(draw, w, h, inset, colour, width):
    draw.rectangle([inset, inset, w - 1 - inset, h - 1 - inset], outline=colour, width=width)


def corner_studs(img, inset, r):
    d = ImageDraw.Draw(img)
    w, h = img.size
    for x, y in ((inset, inset), (w - inset, inset), (inset, h - inset), (w - inset, h - inset)):
        d.ellipse([x - r, y - r, x + r, y + r], fill=(96, 66, 30))
        d.ellipse([x - r + 3, y - r + 3, x + r - 3, y + r - 3], fill=MUSTARD)
        d.ellipse([x - r * 0.35 - 2, y - r * 0.35 - 2, x - r * 0.35 + 3, y - r * 0.35 + 3], fill=(255, 226, 150))


def front_face(grinder, size=(1600, 1900)):
    W, H = size
    face = matched_texture(size, grinder).convert("RGBA")
    d = ImageDraw.Draw(face)
    frame(d, W, H, 0, OXBLOOD, 40)   # flush to the edge: no texture strip outside the border

    # seal, top centre
    seal = Image.open(os.path.join(ASSETS, "seal@2x.png")).convert("RGBA")
    seal = seal.resize((300, 300), Image.LANCZOS)
    face.alpha_composite(seal, ((W - 300) // 2, 96))

    # the grinder, straight out of the plug-in
    g = feather(grinder, 110)
    gw = W - 220
    g = g.resize((gw, int(g.height * gw / g.width)), Image.LANCZOS)
    gy = 330
    face.alpha_composite(g, ((W - gw) // 2, gy))

    d = ImageDraw.Draw(face)
    # name, in the plug-in's own lettering: mustard over an oxblood drop
    f = shrikhand(250)
    ty = gy + g.height - 120
    tw = d.textlength("KIYMA", font=f)
    for dx, dy in ((10, 12), (7, 9), (4, 6)):
        d.text(((W - tw) / 2 + dx, ty + dy), "KIYMA", font=f, fill=OXBLOOD)
    d.text(((W - tw) / 2, ty), "KIYMA", font=f, fill=MUSTARD)

    tracked(d, W / 2, ty + 300, "AUDIO MINCER", oswald(64, 500), CREAM, 26)

    # ribbon: No. 001 · FREE EDITION
    ry = ty + 410
    rw, rh = 620, 74
    d.rounded_rectangle([(W - rw) / 2, ry, (W + rw) / 2, ry + rh], radius=rh // 2, fill=(70, 46, 18), outline=MUSTARD, width=4)
    tracked(d, W / 2, ry + 12, "No. 001  ·  FREE EDITION", oswald(38, 600), MUSTARD, 6)

    # foot line
    fy = H - 150
    d.line([120, fy, W - 120, fy], fill=MUSTARD, width=3)
    tracked(d, W / 2, fy + 22, "SUBVERSELAB  ·  BABASULTAN EDITION", oswald(40, 500), CREAM, 12)
    return face


def side_face(bank, ui, size=(900, 1700)):
    W, H = size
    face = Image.new("RGBA", size, (*OXBLOOD, 255))
    tex = iron_texture(size, 0.0).convert("L").point(lambda v: int(v * 0.6))
    face = Image.composite(Image.new("RGBA", size, (90, 14, 10, 255)), face, tex)  # worn enamel
    d = ImageDraw.Draw(face)
    frame(d, W, H, 0, IRON, 38)

    # vertical name
    f = shrikhand(210)
    txt = Image.new("RGBA", (1400, 330), (0, 0, 0, 0))
    td = ImageDraw.Draw(txt)
    tw = td.textlength("KIYMA", font=f)
    td.text(((1400 - tw) / 2 + 7, 18 + 8), "KIYMA", font=f, fill=IRON)
    td.text(((1400 - tw) / 2, 18), "KIYMA", font=f, fill=MUSTARD)
    txt = txt.rotate(90, expand=True)
    face.alpha_composite(txt, (60, (H - txt.height) // 2 + 60))

    # what is inside: the switch bank and the whole face
    b = bank.convert("RGBA").resize((460, int(bank.height * 460 / bank.width)), Image.LANCZOS)
    bx = W - 460 - 90
    face.alpha_composite(b, (bx, 120))
    tracked(d, bx + 230, 120 + b.height + 20, "FIVE SWEET SPOTS", oswald(34, 600), CREAM, 8)

    u = ui.convert("RGBA").resize((460, int(ui.height * 460 / ui.width)), Image.LANCZOS)
    uy = H - 120 - u.height - 70
    d.rectangle([bx - 8, uy - 8, bx + 468, uy + u.height + 8], fill=IRON)
    face.alpha_composite(u, (bx, uy))
    tracked(d, bx + 230, uy + u.height + 22, "VST3  ·  AU  ·  macOS", oswald(34, 600), CREAM, 8)
    return face


def top_face(size=(1600, 900)):
    W, H = size
    face = iron_texture(size, 0.35).convert("RGBA")
    d = ImageDraw.Draw(face)
    frame(d, W, H, 0, OXBLOOD, 34)
    B = Image.open(os.path.join(ASSETS, "babasultan_B@2x.png")).convert("RGBA")
    gold = Image.new("RGBA", B.size, (*MUSTARD, 255))
    gold.putalpha(B.split()[3])
    gold = gold.resize((int(B.width * 2.2), int(B.height * 2.2)), Image.LANCZOS)
    face.alpha_composite(gold, ((W - gold.width) // 2, (H - gold.height) // 2 - 40))
    tracked(d, W / 2, H - 170, "SUBVERSELAB", oswald(70, 600), CREAM, 30)
    return face


def render(front, side, top, direction, size=(2400, 1600)):
    W, H = size
    bg = fam.vertical_gradient(size, (30, 22, 18), (8, 7, 7)).convert("RGBA")
    bg = Image.alpha_composite(bg, fam.radial_vignette(size, 0.55))

    front_h = int(H * 0.56)
    front_w = int(front_h * front.width / front.height)
    fx0 = int(W * 0.5 - front_w / 2) - int(direction * W * 0.06)
    fy0 = int(H * 0.25)
    fx1, fy1 = fx0 + front_w, fy0 + front_h
    TL, TR, BR, BL = (fx0, fy0), (fx1, fy0), (fx1, fy1), (fx0, fy1)

    depth_x = front_w * 0.42
    depth_y = front_h * 0.13
    converge = 0.82
    back_w = front_w * converge
    back_cx = (TL[0] + TR[0]) / 2 + direction * depth_x
    back_TL = (back_cx - back_w / 2, fy0 - depth_y)
    back_TR = (back_cx + back_w / 2, fy0 - depth_y)
    back_h = front_h * converge
    top_quad = [back_TL, back_TR, TR, TL]
    if direction > 0:
        side_quad = [TR, back_TR, (back_TR[0], back_TR[1] + back_h), BR]
    else:
        side_quad = [back_TL, TL, BL, (back_TL[0], back_TL[1] + back_h)]

    # floor: shadow and contact
    sh = [BL, BR, (BR[0] + front_w * 0.5 * (1 if direction < 0 else 0.3), BR[1] + front_h * 0.22),
          (BL[0] - front_w * 0.3 * (1 if direction > 0 else 0.2), BL[1] + front_h * 0.22)]
    bg = Image.alpha_composite(bg, fam.poly_shadow_layer(size, sh, blur=60, opacity=150))
    bg = Image.alpha_composite(bg, fam.poly_shadow_layer(size, [(BL[0], BL[1] - 4), (BR[0], BR[1] - 4),
                                                                (BR[0] + 24, BR[1] + 40), (BL[0] - 24, BL[1] + 40)],
                                                         blur=14, opacity=230))

    def put(face, quad, shade):
        # warp_into_quad pastes the face onto a canvas-sized layer first, so it must fit inside it
        k = min(1.0, (W - 2) / face.width, (H - 2) / face.height)
        rect = (0, 0, int(face.width * k), int(face.height * k))
        layer = fam.warp_into_quad(face, size, rect, quad)
        if shade:
            r, g, b, a = layer.split()
            dark = Image.merge("RGBA", (r.point(lambda v: int(v * (1 - shade))),
                                        g.point(lambda v: int(v * (1 - shade))),
                                        b.point(lambda v: int(v * (1 - shade))), a))
            layer = dark
        return Image.alpha_composite(bg, layer)

    bg = put(top, top_quad, 0.05)
    s = side if direction > 0 else side.transpose(Image.FLIP_LEFT_RIGHT).transpose(Image.FLIP_LEFT_RIGHT)
    bg = put(s, side_quad, 0.38)
    bg = put(front, [TL, TR, BR, BL], 0.0)

    # key light from top-left on the front, bevels
    hl = Image.new("RGBA", size, (0, 0, 0, 0))
    hd = ImageDraw.Draw(hl)
    hd.line([TL, TR], fill=(255, 220, 160, 120), width=3)
    near = TR if direction > 0 else TL
    far = back_TR if direction > 0 else back_TL
    hd.line([near, far], fill=(255, 220, 160, 70), width=2)
    hd.line([BL, BR], fill=(0, 0, 0, 120), width=3)
    bg = Image.alpha_composite(bg, hl.filter(ImageFilter.GaussianBlur(0.7)))
    return bg.convert("RGB")


def main():
    grinder_p, bank_p, ui_p, out = sys.argv[1:5]
    os.makedirs(out, exist_ok=True)
    grinder = Image.open(grinder_p).convert("RGBA")
    bank = Image.open(bank_p).convert("RGBA")
    ui = Image.open(ui_p).convert("RGBA")
    front, side, top = front_face(grinder), side_face(bank, ui), top_face()
    front.convert("RGB").save(os.path.join(out, "kiyma_box_front_flat.png"))
    for name, direction in (("left", -1), ("right", 1)):
        img = render(front, side, top, direction)
        p = os.path.join(out, f"kiyma_box_{name}.png")
        img.save(p)
        print("wrote", p, img.size)
    # 4:5 feed crop of the right-hand shot
    r = Image.open(os.path.join(out, "kiyma_box_right.png"))
    cw = int(r.height * 4 / 5)
    feed = r.crop(((r.width - cw) // 2, 0, (r.width + cw) // 2, r.height)).resize((1080, 1350), Image.LANCZOS)
    feed.save(os.path.join(out, "kiyma_box_feed_4x5.png"))
    print("wrote", os.path.join(out, "kiyma_box_feed_4x5.png"), feed.size)


if __name__ == "__main__":
    main()
