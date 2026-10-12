#!/usr/local/bin/python3
"""Assembles the ALEV demo videos from ui_video's frames and audio.

  make_video.py <framedir> <outdir>
Writes ALEV_reel_1080x1920.mp4 (Instagram Reels / YouTube Shorts) and ALEV_1920x1080.mp4 (YouTube).
Audio: the plug-in's output, loudness-normalised to -14 LUFS (owner rule for video), true peak kept under -1 dBTP.
"""
import os, sys, glob, subprocess
import numpy as np, soundfile as sf
from PIL import Image, ImageDraw, ImageFont
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
os.environ.setdefault("KORRAW", os.path.join(HERE, "..", "build", "korraw_mb"))
import measure as m

FR, OUT = sys.argv[1], sys.argv[2]
FB = os.path.join(HERE, "..", "Assets", "Fonts", "texgyreheros-bold.otf")
FRG = os.path.join(HERE, "..", "Assets", "Fonts", "texgyreheros-regular.otf")
G = (3, 55, 51); DEEP = (2, 32, 30); CREAM = (242, 239, 231); GOLD = (197, 160, 89); MUTED = (150, 180, 175)
# Scene captions (start s, headline, small line) — match tests/ui_video.cpp's script.
SCENES = [(0.0, "DRY", "the master as it is"), (4.0, "DRY → WET", "the curved fader on the left"),
          (8.0, "LOW  30 → 60", "weight and roundness below 200 Hz"),
          (13.0, "LINK", "the heat spreads to the neighbouring bands"),
          (14.0, "HIGH ↑ · LINK", "MID follows at 70 %, LOW at 49 %"),
          (20.5, "GANG", "one ring moves all three"), (21.5, "MID ↓ · GANG", "all three rings cool together"),
          (27.5, "FREE", "loudness-matched: you hear heat, not level"), (32.0, "FREE · FOREVER", "subverselab.com/plugins/alev")]

def font(p, s): return ImageFont.truetype(p, s)
def spaced(d, cx, y, text, f, fill, track):
    ws = [d.textlength(c, font=f) for c in text]; tot = sum(ws) + track * (len(text) - 1); x = cx - tot / 2
    for c, w in zip(text, ws): d.text((x, y), c, font=f, fill=fill); x += w + track
def scene(t):
    cur = SCENES[0]
    for s in SCENES:
        if t >= s[0]: cur = s
    return cur

def build(W, H, place, name):
    frames = sorted(glob.glob(os.path.join(FR, "f*.png")))
    tmp = os.path.join(FR, f"_{name}"); os.makedirs(tmp, exist_ok=True)
    fT, fS, fH, fL = font(FB, 120 if H > W else 100), font(FB, 34), font(FB, 54 if H > W else 50), font(FRG, 34)
    for i, fp in enumerate(frames):
        t = i / 30.0; s = scene(t)
        c = Image.new("RGB", (W, H), G); d = ImageDraw.Draw(c)
        p = Image.open(fp).convert("RGB")
        place(c, d, p, s, (fT, fS, fH, fL))
        c.save(os.path.join(tmp, f"c{i:05d}.png"))
    return tmp, len(frames)

def reel(c, d, p, s, f):
    fT, fS, fH, fL = f; W = c.width
    spaced(d, W / 2, 130, "ALEV", fT, CREAM, 48)
    spaced(d, W / 2, 285, "FREE 3-BAND SATURATOR", fS, GOLD, 10)
    c.paste(p.resize((1080, 990), Image.LANCZOS), (0, 420))
    spaced(d, W / 2, 1500, s[1], fH, CREAM, 6)
    d.text(((W - d.textlength(s[2], font=fL)) / 2, 1585), s[2], font=fL, fill=MUTED)
    spaced(d, W / 2, 1760, "SUBVERSELAB", fS, GOLD, 12)

def wide(c, d, p, s, f):
    fT, fS, fH, fL = f
    c.paste(p.resize((1080, 990), Image.LANCZOS), (60, 45))
    x = 1180
    d.text((x, 200), "ALEV", font=fT, fill=CREAM)
    d.text((x, 330), "FREE 3-BAND SATURATOR", font=fS, fill=GOLD)
    d.text((x, 560), s[1], font=fH, fill=CREAM)
    d.text((x, 640), s[2], font=fL, fill=MUTED)
    d.text((x, 920), "subverselab.com/plugins/alev", font=fL, fill=GOLD)

def audio():
    y, sr = sf.read(os.path.join(FR, "alev_out.wav"), dtype="float64", always_2d=True)
    n = len(y); fi, fo = int(0.05 * sr), int(1.5 * sr)          # short fade in, 1.5 s fade out (the excerpt is cut from a song)
    y[:fi] *= np.linspace(0, 1, fi)[:, None]; y[n - fo:] *= np.linspace(1, 0, fo)[:, None]
    g = 10 ** ((-14.0 - m.loudness(y, sr)) / 20); y = y * g
    tp = m.true_peak_db(y)
    if tp > -1.0: y = y * 10 ** ((-1.0 - tp) / 20)
    path = os.path.join(FR, "_audio.wav"); sf.write(path, y.astype(np.float32), sr, subtype="FLOAT")
    print("audio LUFS %.2f TP %.2f" % (m.loudness(y, sr), m.true_peak_db(y)))
    return path

if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True); a = audio()
    for (W, H, place, name) in ((1080, 1920, reel, "ALEV_reel_1080x1920"), (1920, 1080, wide, "ALEV_1920x1080")):
        tmp, n = build(W, H, place, name)
        mp4 = os.path.join(OUT, name + ".mp4")
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-framerate", "30", "-i", os.path.join(tmp, "c%05d.png"), "-i", a,
                        "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "18", "-preset", "medium", "-c:a", "aac", "-b:a", "256k",
                        "-shortest", "-movflags", "+faststart", mp4], check=True)
        print(mp4, n, "frames")
