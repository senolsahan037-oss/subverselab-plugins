#!/usr/bin/env python3
"""SubverseLab desktop-app icon, one standard for every standalone tool (docs/APP_ICON_STANDARD.md).

    python3 make_app_icon.py --name ELEK --out <project>/Resources/AppIcon

writes
    <out>/AppIcon.icon/           Icon Composer bundle: the green gradient as the fill, the cream mark + name as one layer
    <out>/AppIcon_1024.png        the same artwork flattened, full bleed (JUCE ICON_BIG; the plug-in formats and old macOS)
    <out>/compiled/Assets.car     actool output: the icon macOS 26+ draws without the grey "squircle" plate
    <out>/compiled/AppIcon.icns   actool's fallback icns for older macOS

Why: macOS 26 and later put every app icon that is not a compiled asset-catalog icon (a plain .icns, even a full-bleed one) on a
light grey squircle, scaled down (seen in the owner's Dock with ELEK, 12 Oct 2026). Only Assets.car + CFBundleIconName avoid it.

The layout never changes between tools, only the NAME: the brand mark at the top, the product name under it in TeX Gyre Heros Bold
(the family's embedded face), cream on the family green. One look in the Dock for all SubverseLab apps; the name tells them apart.
"""
import argparse, json, os, shutil, subprocess, sys
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
S = 1024
TOP, BOT = (0x0d, 0x3b, 0x33), (0x02, 0x15, 0x12)      # the family green, top -> bottom
CREAM = (0xF2, 0xEF, 0xE7, 255)
MARK_W = 0.62                                           # mark width, share of the side
MARK_Y = 200                                            # mark top, px of 1024
NAME_PX_MAX, NAME_W_MAX = 170, 0.74                     # name: at most 170 px, and never wider than 74 % of the side
GAP = 40                                                # mark bottom -> name top


def foreground(name):
    fg = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(fg)
    mark = Image.open(os.path.join(HERE, "mark_cream.png")).convert("RGBA")
    w = int(S * MARK_W); h = int(mark.height * w / mark.width)
    fg.alpha_composite(mark.resize((w, h), Image.LANCZOS), ((S - w) // 2 + 10, MARK_Y))
    px = NAME_PX_MAX
    while True:                                         # long names step the size down instead of running off the edge
        font = ImageFont.truetype(os.path.join(HERE, "texgyreheros-bold.otf"), px)
        track = px * 0.14
        widths = [d.textlength(c, font=font) for c in name]
        total = sum(widths) + track * (len(name) - 1)
        if total <= S * NAME_W_MAX or px <= 60:
            break
        px -= 4
    x = (S - total) / 2; y = MARK_Y + h + GAP
    for c, cw in zip(name, widths):
        d.text((x, y), c, font=font, fill=CREAM)
        x += cw + track
    return fg


def gradient():
    g = Image.new("RGBA", (S, S))
    d = ImageDraw.Draw(g)
    for y in range(S):
        t = y / (S - 1)
        d.line([(0, y), (S, y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(TOP, BOT)) + (255,))
    return g


def srgb(c):
    return "srgb:%.5f,%.5f,%.5f,1.00000" % tuple(v / 255 for v in c)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", required=True, help="product name, ASCII, as it reads in the Dock (ELEK, MIHENK, ...)")
    ap.add_argument("--out", required=True, help="output folder, e.g. <project>/Resources/AppIcon")
    a = ap.parse_args()
    if not a.name.isascii():
        sys.exit("product names are ASCII (root CLAUDE.md)")

    out = os.path.abspath(a.out)
    bundle = os.path.join(out, "AppIcon.icon")
    shutil.rmtree(bundle, ignore_errors=True)
    os.makedirs(os.path.join(bundle, "Assets"))
    fg = foreground(a.name)
    fg.save(os.path.join(bundle, "Assets", "foreground.png"))
    with open(os.path.join(bundle, "icon.json"), "w") as f:
        json.dump({
            "fill": {"linear-gradient": [srgb(TOP), srgb(BOT)]},
            "groups": [{"layers": [{"image-name": "foreground.png", "name": "foreground"}],
                        "shadow": {"kind": "neutral", "opacity": 0.5},
                        "translucency": {"enabled": False, "value": 0.5}}],
            "supported-platforms": {"squares": "shared"},
        }, f, indent=2)

    flat = gradient(); flat.alpha_composite(fg)
    flat.save(os.path.join(out, "AppIcon_1024.png"))

    comp = os.path.join(out, "compiled")
    shutil.rmtree(comp, ignore_errors=True); os.makedirs(comp)
    r = subprocess.run(["xcrun", "actool", bundle, "--compile", comp, "--platform", "macosx", "--minimum-deployment-target", "11.0",
                        "--app-icon", "AppIcon", "--output-partial-info-plist", os.path.join(comp, "partial.plist")],
                       capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(os.path.join(comp, "Assets.car")):
        sys.exit("actool failed:\n" + r.stdout + r.stderr)
    print("wrote", bundle, os.path.join(out, "AppIcon_1024.png"), os.path.join(comp, "Assets.car"))


if __name__ == "__main__":
    main()
