"""Builds the Kubbe announcement Reel (1080x1920) and a 4:5 cover post.

    python3 tools/instagram.py <scratch dir>

Needs, in the scratch dir: song.wav (the dry excerpt, at least 31 s) and
w0/w1/w2.wav (the same excerpt through Hammam, Cistern, Valley, rendered by
build/.../render). Panel images
come from ui_snapshot into demos/instagram/panel_<n>.png.
"""
import os, subprocess, sys
import numpy as np, soundfile as sf
from PIL import Image, ImageDraw, ImageFont, ImageFilter

TMP = sys.argv[1]
OUT = 'demos/instagram'
W, H = 1080, 1920
SR = 44100

PANEL = (3, 55, 51)
DEEP = (2, 30, 28)
INK = (242, 239, 231)
GOLD = (197, 160, 89)
LAMP = (246, 213, 142)
MUTED = (143, 170, 165)

HN = '/System/Library/Fonts/HelveticaNeue.ttc'
def font(size, bold=True):
    return ImageFont.truetype(HN, size, index=1 if bold else 0)

PLACES = [
    ('HAMMAM', 'Marble dome. Dense, bright,', 'a flutter between the walls.'),
    ('CISTERN', 'Underground stone and water.', 'Late column slaps, a long dark tail.'),
    ('VALLEY', 'Open air. The far slopes answer', 'one by one, locked to your tempo.'),
]

def background():
    """Brushed anodized green, like the panel itself."""
    rng = np.random.default_rng(1979)
    top, bot = np.array(PANEL, float) * 1.12, np.array(DEEP, float)
    t = np.linspace(0, 1, H)[:, None]
    base = top * (1 - t) + bot * t
    img = np.repeat(base[:, None, :], W, axis=1)
    img += rng.normal(0, 2.2, (H, 1, 1))
    for _ in range(1400):
        y, x = rng.integers(H), rng.integers(W)
        ln = rng.integers(30, 260)
        img[y, x:x + ln] += rng.choice([-5, 5])
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8))

BG = background()

def tracked(d, xy, text, f, fill, spacing, anchor='mm'):
    """Wide-tracked caps, as on the panel. Centred on xy for anchor 'mm'."""
    widths = [d.textlength(ch, font=f) for ch in text]
    total = sum(widths) + spacing * (len(text) - 1)
    x = xy[0] - total / 2 if anchor == 'mm' else xy[0]
    for ch, w in zip(text, widths):
        d.text((x, xy[1]), ch, font=f, fill=fill, anchor='lm')
        x += w + spacing

def paste_with_shadow(canvas, im, xy):
    sh = Image.new('RGBA', (im.width + 60, im.height + 60), (0, 0, 0, 0))
    ImageDraw.Draw(sh).rectangle((30, 38, 30 + im.width, 38 + im.height), fill=(0, 0, 0, 150))
    sh = sh.filter(ImageFilter.GaussianBlur(18))
    canvas.paste(sh, (xy[0] - 30, xy[1] - 30), sh)
    canvas.paste(im, xy)

panels = [Image.open(f'{OUT}/panel_{i}.png').convert('RGB') for i in range(3)]
wordmark = panels[0].crop((112, 52, 640, 150))          # the KUBBE letters, drawn by the plugin

def lamp(im, d, cx, cy, on, label):
    r = 13
    if on:
        # Glow on its own layer, blurred and composited, so it sits behind the text.
        g = Image.new('RGBA', im.size, (0, 0, 0, 0))
        ImageDraw.Draw(g).ellipse((cx - 34, cy - 34, cx + 34, cy + 34), fill=(246, 213, 142, 150))
        im.alpha_composite (g.filter(ImageFilter.GaussianBlur(16)))
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=LAMP if on else (60, 48, 22))
    tracked(d, (cx + 30, cy), label, font(30), INK if on else MUTED, 6, anchor='lm')

def frame_place(i, wet):
    im = BG.copy().convert('RGBA')
    d = ImageDraw.Draw(im, 'RGBA')
    name, l1, l2 = PLACES[i]
    tracked(d, (W / 2, 200), f'{i + 1} / 3', font(30), GOLD, 10)
    tracked(d, (W / 2, 300), name, font(96), INK, 22)
    d.text((W / 2, 400), l1, font=font(40, False), fill=INK, anchor='mm')
    d.text((W / 2, 452), l2, font=font(40, False), fill=INK, anchor='mm')

    p = panels[i]
    full = p.resize((1000, round(p.height * 1000 / p.width)), Image.LANCZOS)
    zoom = p.crop((100, 40, 1560, 480))
    zoom = zoom.resize((1000, round(zoom.height * 1000 / zoom.width)), Image.LANCZOS)
    paste_with_shadow(im, zoom, (40, 560))
    paste_with_shadow(im, full, (40, 560 + zoom.height + 50))

    y = 560 + zoom.height + 50 + full.height + 110
    lamp(im, d, W / 2 - 250, y, not wet, 'DRY')
    lamp(im, d, W / 2 + 60, y, wet, 'KUBBE ON')
    return im.convert('RGB')

def frame_intro():
    im = BG.copy().convert('RGBA')
    d = ImageDraw.Draw(im, 'RGBA')
    wm = wordmark.resize((wordmark.width * 17 // 10, wordmark.height * 17 // 10), Image.LANCZOS)
    mask = Image.fromarray((np.asarray(wm.convert('L')) > 150).astype(np.uint8) * 255).filter(ImageFilter.GaussianBlur(0.6))
    ink = Image.new('RGB', wm.size, INK)
    im.paste(ink, ((W - wm.width) // 2, 640), mask)
    tracked(d, (W / 2, 860), 'DIGITAL REVERBERATION SYSTEM', font(30), INK, 9)
    d.rectangle((W / 2 - 150, 920, W / 2 + 150, 980), outline=GOLD, width=3)
    tracked(d, (W / 2, 950), 'MODEL KB-79', font(30), GOLD, 8)
    d.text((W / 2, 1120), 'A reverb with three rooms.', font=font(44, False), fill=INK, anchor='mm')
    tracked(d, (W / 2, 1200), 'HAMMAM  ·  CISTERN  ·  VALLEY', font(32), GOLD, 5)
    tracked(d, (W / 2, 1720), 'SUBVERSELAB', font(28), MUTED, 12)
    return im.convert('RGB')

def frame_outro():
    im = BG.copy().convert('RGBA')
    d = ImageDraw.Draw(im, 'RGBA')
    wm = wordmark.resize((wordmark.width * 12 // 10, wordmark.height * 12 // 10), Image.LANCZOS)
    mask = Image.fromarray((np.asarray(wm.convert('L')) > 150).astype(np.uint8) * 255).filter(ImageFilter.GaussianBlur(0.6))
    im.paste(Image.new('RGB', wm.size, INK), ((W - wm.width) // 2, 420), mask)
    tracked(d, (W / 2, 640), 'FREE', font(150), LAMP, 20)
    d.text((W / 2, 790), 'for the first 1,000 users', font=font(56, False), fill=INK, anchor='mm')
    d.rounded_rectangle((W / 2 - 360, 930, W / 2 + 360, 1060), 20, outline=GOLD, width=4)
    d.text((W / 2, 995), 'DM  “KUBBE”  for your code', font=font(46), fill=INK, anchor='mm')
    tracked(d, (W / 2, 1170), 'OUT NOW', font(44), LAMP, 10)
    d.text((W / 2, 1245), 'macOS · Windows', font=font(36, False), fill=MUTED, anchor='mm')
    tracked(d, (W / 2, 1720), 'SUBVERSELAB', font(28), MUTED, 12)
    return im.convert('RGB')

def frame_cover():
    """4:5 feed post: the panel is the picture."""
    cw, ch = 1080, 1350
    im = BG.crop((0, 0, cw, ch)).convert('RGBA')
    d = ImageDraw.Draw(im, 'RGBA')
    wm = wordmark.resize((wordmark.width * 13 // 10, wordmark.height * 13 // 10), Image.LANCZOS)
    mask = Image.fromarray((np.asarray(wm.convert('L')) > 150).astype(np.uint8) * 255).filter(ImageFilter.GaussianBlur(0.6))
    im.paste(Image.new('RGB', wm.size, INK), ((cw - wm.width) // 2, 150), mask)
    tracked(d, (cw / 2, 330), 'A REVERB WITH THREE ROOMS', font(28), INK, 7)
    p = panels[0]
    full = p.resize((1000, round(p.height * 1000 / p.width)), Image.LANCZOS)
    paste_with_shadow(im, full, (40, 440))
    tracked(d, (cw / 2, 1010), 'FREE FOR THE FIRST 1,000 USERS', font(40), LAMP, 6)
    d.text((cw / 2, 1080), 'DM “KUBBE” for your code  ·  Out now', font=font(34, False), fill=INK, anchor='mm')
    tracked(d, (cw / 2, 1250), 'SUBVERSELAB  ·  MODEL KB-79', font(24), MUTED, 8)
    return im.convert('RGB')

# ---------------------------------------------------------------------------
# Stills and timing

DRY_S, SEG_S, INTRO_S, OUTRO_S = 2.5, 8.0, 3.0, 4.0
shots = [('intro', frame_intro(), INTRO_S)]
for i in range(3):
    shots.append((f'p{i}_dry', frame_place(i, False), DRY_S))
    shots.append((f'p{i}_wet', frame_place(i, True), SEG_S - DRY_S))
shots.append(('outro', frame_outro(), OUTRO_S))
frame_cover().save(f'{OUT}/kubbe_cover_4x5.png')

concat = os.path.join(TMP, 'shots.txt')
with open(concat, 'w') as fh:
    for name, img, dur in shots:
        path = os.path.join(TMP, f'{name}.png')
        img.save(path)
        fh.write(f"file '{path}'\nduration {dur}\n")
    fh.write(f"file '{os.path.join(TMP, shots[-1][0] + '.png')}'\n")

# ---------------------------------------------------------------------------
# Audio: the song runs without a break under the whole Reel. Each place was
# rendered over the full excerpt, so switching to it mid-song sounds like
# engaging the plugin on a running track: dry for 2.5 s, then that place.

dry, _ = sf.read(os.path.join(TMP, 'song.wav'))
wets = [sf.read(os.path.join(TMP, f'w{i}.wav'))[0][:len(dry)] for i in range(3)]
n = lambda s: int(round(s * SR))

# (start, source) in seconds: None is dry.
cuts = [(0.0, None)]
t = INTRO_S
for i in range(3):
    cuts += [(t, None), (t + DRY_S, i)]
    t += SEG_S
total = n(t + OUTRO_S)
src = lambda k: dry if k is None else wets[k]

audio = np.zeros((total, 2))
xf = n(0.04)
for j, (start, k) in enumerate(cuts):
    a0 = n(start)
    a1 = n(cuts[j + 1][0]) if j + 1 < len(cuts) else total
    audio[a0:a1] = src(k)[a0:a1]
    if j > 0 and cuts[j - 1][1] != k:
        ramp = np.linspace(0, 1, xf)[:, None]
        audio[a0:a0 + xf] = src(cuts[j - 1][1])[a0:a0 + xf] * (1 - ramp) + src(k)[a0:a0 + xf] * ramp
audio[:n(0.02)] *= np.linspace(0, 1, n(0.02))[:, None]
audio[-n(1.2):] *= np.linspace(1, 0, n(1.2))[:, None]
audio /= np.abs(audio).max() / 0.89
wav = os.path.join(TMP, 'reel.wav')
sf.write(wav, audio, SR, subtype='PCM_16')

out = f'{OUT}/kubbe_reel_1080x1920.mp4'
subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-f', 'concat', '-safe', '0', '-i', concat, '-i', wav,
                '-vf', 'fps=30,format=yuv420p', '-c:v', 'libx264', '-preset', 'medium', '-crf', '18',
                '-c:a', 'aac', '-b:a', '256k', '-shortest', '-movflags', '+faststart', out], check=True)
print('wrote', out, round(len(audio) / SR, 2), 's')
