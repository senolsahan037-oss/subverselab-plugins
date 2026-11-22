"""Option B in motion, driven by the engine (owner 20 Nov).
- STEREO FIELD: one shape whose edges shimmer like a vertical waveform. Outline = fill of 40 fine bands from an FFT of the
  engine's OUTPUT (what the real panel will do from a sample FIFO), edge = half * fill/200 %, so a 100 % grip sits exactly
  where a 100 % full band ends. Light colour per height: narrow ink green -> wide cream, gold past 100 %.
- Centre number = the band's live fill % (engine BandReadout.liveWidth * 100), gold past 100. Grips fixed at the setting.
- Frequencies fixed; right column = live MONO loss (header follows LISTEN; STEREO reads as MONO).
- 360 scope: per band, dots on that band's ring (bass inside). How many = band level; spread = fill (narrow band = tight
  cluster, full band = sparse, spread out); centre angle = liveBalance; share behind = fill/2 (0 mono, half when fully wide,
  all when out of phase) - the same thing a 360 goniometer shows. Behind dots gold.
Usage: python3 tools/field_motion_b.py frames.csv engine_out.f32 rate label out.mp4 [still_frame.png frame_index]
"""
import os, subprocess, sys, tempfile
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter
import soundfile as sf, pyloudnorm as pyln

csv, aud, rate, label, out = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4], sys.argv[5]
still = sys.argv[6] if len(sys.argv) > 7 else None; still_i = int(sys.argv[7]) if still else 0
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
GREY = (218, 220, 212); GREY_HI = (230, 232, 225); GREY_LO = (200, 203, 194)
INK = (3, 55, 51); INK_SOFT = (73, 108, 104); GOLD = (197, 160, 89); CREAM = (242, 239, 231); LIGHT = (252, 250, 244)
F = os.path.join(ROOT, "Assets", "Fonts"); S = 2
def font(n, px): return ImageFont.truetype(os.path.join(F, n), int(px * S))
MONO, BOLD = "DejaVuSansMono-Bold.ttf", "texgyreheros-bold.otf"
fB12, fB11, fB10, fM12, fM13, fM18 = font(BOLD, 12), font(BOLD, 11), font(BOLD, 10), font(MONO, 12), font(MONO, 13), font(MONO, 18)
def mixc(a, b, t): return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))
def light_colour(w):
    if w <= 1: return mixc(INK_SOFT, LIGHT, float(np.clip(w, 0, 1)))
    return mixc(LIGHT, GOLD, float(min(1.0, (w - 1) * 2)))

SKIP = 30
A = np.loadtxt(csv, delimiter=",")
wid8, lev8, bal8, corr = A[:, 0:24:3], A[:, 1:24:3], A[:, 2:24:3], A[:, 24]
stage = A[:, 25] if A.shape[1] > 25 else np.zeros(len(A))
HAS_LIMIT = A.shape[1] >= 50
if HAS_LIMIT: lim8, pkf8, nos8 = A[:, 26:50:3], A[:, 27:50:3], A[:, 28:50:3]
y = np.fromfile(aud, dtype=np.float32).reshape(-1, 2).astype(np.float64)
# The width panel is fed BEFORE the STAGE rotation (owner: the stage must not move the width panel). FIELD_AUDIO = the same
# render without STAGE; the goniometer and the audio track use the full output.
yfield = np.fromfile(os.environ["FIELD_AUDIO"], dtype=np.float32).reshape(-1, 2).astype(np.float64) if os.environ.get("FIELD_AUDIO") else y
hop = int(rate / 30); NFFT = 4096; win = np.hanning(NFFT)
edges8 = [20, 100, 250, 500, 1000, 2000, 4000, 8000, 20000]
fine = np.geomspace(40, 16000, 41); fc_fine = np.sqrt(fine[:-1] * fine[1:])
freqs = np.fft.rfftfreq(NFFT, 1 / rate)
bins = [(freqs >= lo) & (freqs < hi) for lo, hi in zip(fine[:-1], fine[1:])]

def fine_fill(i):
    c = (i + 1) * hop; a0 = max(0, c - NFFT)
    seg = yfield[a0:a0 + NFFT]
    if len(seg) < NFFT: seg = np.pad(seg, ((0, NFFT - len(seg)), (0, 0)))
    M = np.fft.rfft(win * 0.5 * (seg[:, 0] + seg[:, 1])); Sd = np.fft.rfft(win * 0.5 * (seg[:, 0] - seg[:, 1]))
    pm = np.array([np.sum(np.abs(M[b]) ** 2) for b in bins]); ps = np.array([np.sum(np.abs(Sd[b]) ** 2) for b in bins])
    return pm, ps

# fine outline with light ballistics (still shimmering), 8-band readouts smoothed for the numbers
# smooth the POWERS (over time and over neighbouring fine bands), then take the ratio: low bands have few FFT bins and
# a per-frame ratio there is noise; this keeps the shimmer but drops the spikes.
nF = len(A); fw = np.zeros((nF, 40)); fl = np.zeros((nF, 40)); sw8 = np.zeros_like(wid8)
sm = np.array([0.25, 0.5, 0.25]); spm = sps = None
for i in range(nF):
    pm, ps = fine_fill(i)
    pm = np.convolve(np.pad(pm, 1, mode="edge"), sm, mode="valid"); ps = np.convolve(np.pad(ps, 1, mode="edge"), sm, mode="valid")
    if spm is None: spm, sps = pm, ps
    else: spm = spm + (pm - spm) * 0.45; sps = sps + (ps - sps) * 0.45
    tot = spm + sps
    fw[i] = np.where(tot > 1e-12, 2 * sps / np.maximum(tot, 1e-30), 0.0)
    fl[i] = 10 * np.log10(np.maximum(tot / NFFT, 1e-12))
    sw8[i] = wid8[i] if i == 0 else sw8[i - 1] + (wid8[i] - sw8[i - 1]) * np.where(wid8[i] > sw8[i - 1], 0.6, 0.25)
lvl_ref = np.percentile(fl[SKIP:], 97)
# PEAK HOLD (owner 20 Nov): the 6 bands' grips and centre numbers sit at the highest fill reached so far (from play start).
# Band level gate: a band below -60 dB does not set a peak (silence noise).
peak8 = np.zeros_like(sw8); run_ = np.zeros(8)
for i_ in range(len(sw8)):
    if i_ >= SKIP:
        ok = lev8[i_] > -60
        run_ = np.where(ok, np.maximum(run_, np.maximum(0.0, sw8[i_])), run_)
    peak8[i_] = run_
AMP_REF = np.percentile(np.hypot(0.5 * (y[:, 0] + y[:, 1]), 0.5 * (y[:, 0] - y[:, 1])), 99.7)

W, H = 960, 352
names = ["8k+", "4k", "2k", "1k", "500", "250", "100", "<100"]
SX0, SX1 = 8, 300; FX0, FX1 = 308, W - 8
fx0, fx1 = FX0 + 62, FX1 - 80; fcx = (fx0 + fx1) / 2; half = (fx1 - fx0) / 2
y0, rh = 40, 37
def ypos(f):   # band rows top (8k+) .. bottom (<100); inside a row, higher frequency higher up
    k = int(np.clip(np.searchsorted(edges8, f) - 1, 0, 7)); lo, hi = edges8[k], edges8[k + 1]
    frac = (np.log(f) - np.log(lo)) / (np.log(hi) - np.log(lo))
    return y0 + (7 - k) * rh + rh * (1 - frac)

YF = np.array([ypos(f) for f in fc_fine])
rng = np.random.default_rng(1)

def frame(i):
    img = Image.new("RGB", (W * S, H * S), GREY); d = ImageDraw.Draw(img)
    def r(*v): return [x * S for x in v]
    def T(x, y_, s, f, c, an="la"): d.text((x * S, y_ * S), s, font=f, fill=c, anchor=an)
    d.rounded_rectangle(r(SX0, 8, SX1, H - 8), radius=10 * S, fill=GREY_HI)
    d.rounded_rectangle(r(FX0, 8, FX1, H - 8), radius=10 * S, fill=GREY_HI)

    # ---- 360 scope
    gx, gy, gR = (SX0 + SX1) / 2, 192, 124
    d.ellipse(r(gx - gR, gy - gR, gx + gR, gy + gR), outline=GREY_LO, width=S)
    for a_ in range(0, 360, 45):
        d.line(r(gx, gy, gx + np.cos(np.radians(a_)) * gR, gy - np.sin(np.radians(a_)) * gR), fill=GREY_LO, width=S)
    d.line(r(gx - gR, gy, gx + gR, gy), fill=INK_SOFT, width=S)
    T(gx - gR - 3, gy - 7, "L", fB11, INK_SOFT, "ra"); T(gx + gR + 3, gy - 7, "R", fB11, INK_SOFT)
    T(SX0 + 12, 14, "CORRELATION", font(BOLD, 9), INK_SOFT); T(SX0 + 12, 26, f"{corr[i]:+.2f}", fM18, INK)
    # Real goniometer as a 360 stage (owner 20 Nov): every output sample pair is one dot. M/S folded to M >= 0, stage angle
    # = 2 * atan2(S, M): mono -> front (top), hard left -> left, hard right -> right, anti-phase -> behind (bottom, gold).
    # Radius = sample amplitude (square-root scale). This frame's samples bright, the previous frame's faint (persistence).
    # Colours (owner 20 Nov): mixed by stage angle, no hard line (front ink green -> sides ink-soft -> behind gold), and each
    # dot follows the signal's strength as it decays: a fresh, loud sample is full colour; quieter and older samples fade
    # into the ground. Trail = the last 8 frames (~270 ms), oldest drawn first.
    arr = np.asarray(img).astype(np.float32)
    bg = np.array(GREY_HI, np.float32); c0 = np.array(INK, np.float32); c1 = np.array(INK_SOFT, np.float32); c2 = np.array(GOLD, np.float32)
    for age in range(7, -1, -1):
        fr = i - age
        if fr < 0: continue
        seg = y[fr * hop:(fr + 1) * hop]
        if len(seg) == 0: continue
        if age > 0: seg = seg[::2]                                   # thinner trail
        m = 0.5 * (seg[:, 0] + seg[:, 1]); sd = 0.5 * (seg[:, 0] - seg[:, 1])
        neg = m < 0; m = np.where(neg, -m, m); sd = np.where(neg, -sd, sd)
        th = 2 * np.arctan2(sd, m); amp = np.minimum(1.0, np.sqrt(np.hypot(m, sd) / AMP_REF))
        rad = amp * (gR - 3)
        px = ((gx - np.sin(th) * rad) * S).astype(int); py = ((gy - np.cos(th) * rad) * S).astype(int)
        t = (np.abs(th) / np.pi)[:, None]                            # 0 front .. 1 behind
        hue = np.where(t < 0.5, c0 + (c1 - c0) * (t / 0.5), c1 + (c2 - c1) * ((t - 0.5) / 0.5))
        strength = np.clip(0.4 + 1.0 * amp, 0, 1) * np.exp(-age / 2.6)
        col = bg + (hue - bg) * strength[:, None]
        size = 3 if age == 0 else 2
        for dy in range(size):
            for dx in range(size):
                yy_ = np.clip(py + dy - 1, 0, arr.shape[0] - 1); xx_ = np.clip(px + dx - 1, 0, arr.shape[1] - 1)
                arr[yy_, xx_] = col
    img = Image.fromarray(arr.astype(np.uint8)); d = ImageDraw.Draw(img)
    T(gx, gy - gR - 4, "FRONT", font(BOLD, 8), INK_SOFT, "md"); T(gx, gy + gR + 3, "BEHIND", font(BOLD, 8), INK_SOFT, "ma")
    # STAGE puck: grab it anywhere in the stage and drag; it sits where the centre of the image now is.
    st_ = np.radians(stage[i]); pr = gR * 0.86
    pxp, pyp = gx - np.sin(st_) * pr, gy - np.cos(st_) * pr
    d.line(r(gx, gy, pxp, pyp), fill=mixc(GREY_HI, INK_SOFT, 0.5), width=S)
    d.ellipse(r(pxp - 9, pyp - 9, pxp + 9, pyp + 9), fill=CREAM, outline=INK, width=2 * S)
    T(SX1 - 12, 14, "STAGE", font(BOLD, 9), INK_SOFT, "ra"); T(SX1 - 12, 26, f"{stage[i]:+.0f}°", fM18, INK, "ra")

    # ---- STEREO FIELD, option B
    T(fx0, 18, "L", fB11, INK_SOFT); T(fx1, 18, "R", fB11, INK_SOFT, "ra"); T(fx1 + 26, 18, "MONO", fB10, INK_SOFT)
    T(fcx, 18, label, fB10, INK_SOFT, "ma")
    ys = np.linspace(y0 + 2, y0 + 8 * rh - 2, 300)
    order = np.argsort(YF); YFo = YF[order]
    wv = np.interp(ys, YFo, fw[i][order]); lvv = np.interp(ys, YFo, fl[i][order])
    hh = half * np.clip(wv / 2, 0.012, 1.0)
    strength = np.clip((lvv - (lvl_ref - 45)) / 40, 0.0, 1.0)
    tint = Image.new("RGB", img.size, GREY_HI); td = ImageDraw.Draw(tint)
    for y_, w_ in zip(ys, wv): td.line([0, y_ * S, W * S, y_ * S], fill=light_colour(w_), width=2 * S)
    mask = Image.new("L", img.size, 0); md = ImageDraw.Draw(mask)
    for y_, h_, s_ in zip(ys, hh, strength):
        md.line([((fcx - h_) * S, y_ * S), ((fcx + h_) * S, y_ * S)], fill=int(255 * (0.35 + 0.6 * s_)), width=2 * S)
    img = Image.composite(tint, img, mask.filter(ImageFilter.GaussianBlur(2 * S)))
    d = ImageDraw.Draw(img)
    for side in (-1, 1):
        d.line([((fcx + side * h_) * S, y_ * S) for y_, h_ in zip(ys, hh)], fill=mixc(GREY_HI, INK_SOFT, 0.6), width=S)
    d.line(r(fcx, y0 - 4, fcx, y0 + 8 * rh), fill=GREY_LO, width=S)
    for row in range(8):
        yc = y0 + row * rh + rh / 2; k = 7 - row
        d.line(r(fx0, yc, fx1, yc), fill=mixc(GREY_HI, GREY_LO, 0.5), width=S)
        T(fx0 - 16, yc - 7, names[row], fB12, INK, "ra")                                    # fixed
        w = max(0.0, sw8[i, k])
        ml = 10 * np.log10(max(1e-6, 1 - w / 2)) if w < 2 else -60.0
        T(fx1 + 26, yc - 8, f"{ml:+.1f}", fM13, GOLD if w > 1 else INK_SOFT)
        pk = pkf8[i, k] if HAS_LIMIT else peak8[i, k]
        if HAS_LIMIT:   # SPEC 7: grips = the setting (100 %), gold notches = LIMIT learned from the music
            lim = lim8[i, k]; nos = nos8[i, k] > 0.5
            lx = half * min(1.0, lim / 2.0)
            if lim < 1.999:
                for gxx in (fcx - lx, fcx + lx):
                    d.rounded_rectangle(r(gxx - 2, yc - 11, gxx + 2, yc + 11), radius=2 * S, fill=GOLD)
            sh = half * 0.5
            gcol = mixc(GREY_HI, INK, 0.35) if nos else INK
            for gxx in (fcx - sh, fcx + sh):
                d.ellipse(r(gxx - 7, yc - 7, gxx + 7, yc + 7), fill=CREAM, outline=gcol, width=2 * S)
        else:
            sh = half * min(1.0, pk / 2.0)
            for gxx in (fcx - sh, fcx + sh):
                d.ellipse(r(gxx - 7, yc - 7, gxx + 7, yc + 7), fill=CREAM, outline=GOLD if pk > 1 else INK, width=2 * S)
        fill = int(round(min(pk, 2.0) * 100))
        bw = font(MONO, 12).getlength(f"{fill}") / S
        d.rounded_rectangle(r(fcx - bw / 2 - 4, yc - 8, fcx + bw / 2 + 4, yc + 8), radius=4 * S, fill=GREY_HI)
        T(fcx, yc - 7, f"{fill}", fM12, GOLD if pk > 1 else INK, "ma")                     # peak fill % so far
    return img

if still:
    frame(still_i).resize((W, H), Image.LANCZOS).save(still); print(still); sys.exit()

tmp = tempfile.mkdtemp()
for j, i in enumerate(range(SKIP, nF)):
    frame(i).save(os.path.join(tmp, f"f{j:05d}.png"))
    if j % 150 == 0: print(f"frame {j}/{nF - SKIP}", flush=True)
yy = y[SKIP * hop:].astype(np.float32)
Lu = pyln.Meter(rate).integrated_loudness(yy); yy = yy * 10 ** ((-16 - Lu) / 20)
wav = os.path.join(tmp, "a.wav"); sf.write(wav, yy, rate, subtype="PCM_24")
subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-framerate", "30", "-i", os.path.join(tmp, "f%05d.png"), "-i", wav,
                "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "20", "-c:a", "aac", "-b:a", "192k", "-shortest", out], check=True)
print(out)
