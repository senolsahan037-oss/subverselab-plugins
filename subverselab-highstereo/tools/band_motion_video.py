"""Moving STEREO FIELD preview (owner 20 Nov: "bantlar sesle birlikte hareket etsin").
Input: frames.csv from tools/band_motion.cpp (real engine, 30 frames/s) and the engine's output audio.
Bar half-width = sqrt(liveWidth / 2) of the track: centre = mono, the faint tick = fully wide (liveWidth 1),
the edge = fully out of phase. Past the tick the bar turns gold (that part vanishes in mono).
Bar strength follows the band level (-48 .. -16 dB). Grips ride the bar ends; the number is the band's setting.
Usage: python3 tools/band_motion_video.py frames.csv out_audio.f32 rate width_label out.mp4 [skip_frames]
"""
import os, subprocess, sys, tempfile
from PIL import ImageFilter
import numpy as np
from PIL import Image, ImageDraw, ImageFont
import soundfile as sf, pyloudnorm as pyln

csv, aud, rate, label, out = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4], sys.argv[5]
skip = int(sys.argv[6]) if len(sys.argv) > 6 else 30
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
GREY = (218, 220, 212); GREY_HI = (230, 232, 225); GREY_LO = (200, 203, 194)
INK = (3, 55, 51); INK_SOFT = (73, 108, 104); GOLD = (197, 160, 89); CREAM = (242, 239, 231)
F = os.path.join(ROOT, "Assets", "Fonts")
S = 2
def font(n, px): return ImageFont.truetype(os.path.join(F, n), int(px * S))
MONO, BOLD = "DejaVuSansMono-Bold.ttf", "texgyreheros-bold.otf"
fB12, fB11, fM12, fM13 = font(BOLD, 12), font(BOLD, 11), font(MONO, 12), font(MONO, 13)

a = np.loadtxt(csv, delimiter=",")[skip:]
wid, lev, cor = a[:, 0:16:2], a[:, 1:16:2], a[:, 16]
# UI ballistics at 30 Hz: fast up, slower down (what the panel will do on top of the 50 ms engine readout)
sw = np.zeros_like(wid); sl = np.zeros_like(lev)
for i in range(len(a)):
    for arr, src in ((sw, wid), (sl, lev)):
        prev = arr[i - 1] if i else src[0]
        up = src[i] > prev
        arr[i] = prev + (src[i] - prev) * np.where(up, 0.6, 0.25)

W, H = 640, 352
PALETTE = os.environ.get("HS_GLOW", "cream")
CREAM_LIGHT = (252, 250, 244)   # brand cream pushed toward white so it reads as light on the grey track
bands = ["8k+", "4k", "2k", "1k", "500", "250", "100", "<100"]   # top to bottom; csv order is <100 .. 8k+
def mixc(a_, b_, t): return tuple(int(a_[i] + (b_[i] - a_[i]) * t) for i in range(3))

def frame(i):
    """Owner 20 Nov: the bar and its grips stay fixed at the setting (they are controls); the sound is a gold light
    that glows through the bar and spills past it as far as the band is wide right now, brighter when louder."""
    img = Image.new("RGB", (W * S, H * S), GREY); d = ImageDraw.Draw(img)
    def r(*v): return [x * S for x in v]
    def T(dd, x, y, s, f, c, an="la"): dd.text((x * S, y * S), s, font=f, fill=c, anchor=an)
    def rr(dd, b, c, rad=9): dd.rounded_rectangle(r(*b), radius=rad * S, fill=c)
    rr(d, (8, 8, W - 8, H - 8), GREY_HI, 10)
    fx0, fx1 = 70, W - 86; fc = (fx0 + fx1) / 2; half = (fx1 - fx0) / 2
    T(d, fx0, 18, "L", fB11, INK_SOFT); T(d, fx1, 18, "R", fB11, INK_SOFT, "ra"); T(d, fx1 + 26, 18, "MONO", font(BOLD, 10), INK_SOFT)
    T(d, fc, 18, label, font(BOLD, 10), INK_SOFT, "ma")
    y0, rh = 40, 37
    d.line(r(fc, y0 - 4, fc, y0 + 8 * rh), fill=GREY_LO, width=S)
    setting_hw = 0.5 * half                       # 100 % setting; 200 % reaches the edge
    # Owner 20 Nov: the light is UNDER the bar and its grips (they are draggable controls); it never shows through them,
    # it leaks out around them. Colour = how wide the band is now: narrow -> ink green, wide -> gold. Brightness = level.
    tint = Image.new("RGB", img.size, GREY_HI); td = ImageDraw.Draw(tint)
    glow = Image.new("L", img.size, 0); gd = ImageDraw.Draw(glow)
    rows = []
    for row, name in enumerate(bands):
        k = 7 - row; yc = y0 + row * rh + rh / 2
        w_, lv = max(0.0, sw[i, k]), sl[i, k]
        live_hw = min(1.0, np.sqrt(w_ / 2.0)) * half
        strength = float(np.clip((lv + 48) / 32, 0.0, 1.0))
        over = w_ > 1.0
        t = float(np.clip(w_, 0.0, 1.0))
        if PALETTE == "gold": c = mixc(INK_SOFT, GOLD, t)
        else:   # "cream": narrow ink green -> wide cream light; gold only while the band is past fully wide (vanishes in mono)
            c = mixc(INK_SOFT, CREAM_LIGHT, t) if not over else mixc(CREAM_LIGHT, GOLD, float(np.clip(w_ - 1.0, 0.0, 0.5) * 2))
        td.rectangle(r(fx0 - 30, yc - rh / 2, fx1 + 30, yc + rh / 2), fill=c)
        rr(d, (fx0, yc - 9, fx1, yc + 9), GREY_LO)
        a_ = int(255 * (0.2 + 0.75 * strength))
        if row < 6:
            hw = max(live_hw, setting_hw + 6)
            gd.rounded_rectangle(r(fc - hw, yc - 11, fc + hw, yc + 11), radius=11 * S, fill=a_)
            for gx in (fc - setting_hw, fc + setting_hw): gd.ellipse(r(gx - 12, yc - 12, gx + 12, yc + 12), fill=a_)
        else:
            hw = max(live_hw, 9)
            gd.rounded_rectangle(r(fc - hw, yc - 10, fc + hw, yc + 10), radius=10 * S, fill=a_)
        rows.append((row, name, yc, w_, over))
    img = Image.composite(tint, img, glow.filter(ImageFilter.GaussianBlur(5 * S)))
    d = ImageDraw.Draw(img)
    for row, name, yc, w_, over in rows:   # the fixed bar sits in front of the light (back-lit)
        if row < 6: rr(d, (fc - setting_hw, yc - 9, fc + setting_hw, yc + 9), INK)
        else: rr(d, (fc - 3, yc - 9, fc + 3, yc + 9), INK, 2)
        T(d, fx0 - 16, yc - 7, name, fB12, GOLD if over else INK, "ra")
        if row < 6:
            for gx in (fc - setting_hw, fc + setting_hw):                                  # grips stay put
                d.ellipse(r(gx - 7, yc - 7, gx + 7, yc + 7), fill=CREAM, outline=INK, width=2 * S)
            T(d, fc, yc - 7, "100", fM12, CREAM, "ma")
        ml = 10 * np.log10(max(1e-6, 1 - w_ / 2)) if w_ < 2 else -60.0
        T(d, fx1 + 26, yc - 8, f"{ml:+.1f}", fM13, GOLD if over else INK_SOFT)
    return img

tmp = tempfile.mkdtemp()
for i in range(len(a)):
    frame(i).save(os.path.join(tmp, f"f{i:05d}.png"))
    if i % 150 == 0: print(f"frame {i}/{len(a)}", flush=True)

y = np.fromfile(aud, dtype=np.float32).reshape(-1, 2)[int(skip * rate / 30):]
L = pyln.Meter(rate).integrated_loudness(y); y = y * 10 ** ((-16 - L) / 20)   # -16 LUFS, plenty of peak room
wav = os.path.join(tmp, "a.wav"); sf.write(wav, y, rate, subtype="PCM_24")
subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-framerate", "30", "-i", os.path.join(tmp, "f%05d.png"), "-i", wav,
                "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "20", "-c:a", "aac", "-b:a", "192k", "-shortest", out], check=True)
print(out, f"{len(a)} frames, audio {L:.1f} LUFS -> -16")
