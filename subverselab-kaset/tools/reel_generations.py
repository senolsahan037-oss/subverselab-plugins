"""Builds the Kaset "one song, four generations" Reel (1080x1920).

    python3 tools/reel_generations.py <scratch dir> <out.mp4> <audio dir>

Needs in the scratch dir: ui_dry.png, ui_g1.png, ui_g2.png, ui_g4.png from
ui_snapshot (BYPASS, 1st, 2nd and 4th dub). Audio comes from demos/: the dry
Diplomat excerpt and the same excerpt through three dubs, loudness-matched by
tools/demos.py, so only the colour changes. The song runs without a break;
each section switches source on the running track.
"""
import os, subprocess, sys
import numpy as np, soundfile as sf
from PIL import Image, ImageDraw, ImageFont, ImageFilter

TMP, OUT = sys.argv[1], sys.argv[2]
AUD = sys.argv[3] if len(sys.argv) > 3 else 'demos'   # dir with 0_dry.wav, g2.wav, g3.wav, g5.wav, or the Diplomat demos
W, H, SR, FPS = 1080, 1920, 44100, 30

DEEP = (2, 30, 28)
PANEL = (3, 55, 51)
INK = (242, 239, 231)
GOLD = (197, 160, 89)
LAMP = (246, 213, 142)
MUTED = (143, 170, 165)
HN = '/System/Library/Fonts/HelveticaNeue.ttc'


def font(size, bold=True):
    return ImageFont.truetype(HN, size, index=1 if bold else 0)


def tracked(d, cx, y, text, f, fill, spacing):
    w = sum(d.textlength(c, font=f) for c in text) + spacing * (len(text) - 1)
    x = cx - w / 2
    for c in text:
        d.text((x, y), c, font=f, fill=fill)
        x += d.textlength(c, font=f) + spacing


def background():
    y = np.linspace(0, 1, H)[:, None]
    x = np.linspace(-1, 1, W)[None, :]
    glow = np.exp(-((x ** 2) / 0.5 + ((y - 0.42) ** 2) / 0.08))
    base = np.array(DEEP)[None, None, :] + (np.array(PANEL) - np.array(DEEP))[None, None, :] * glow[..., None]
    return Image.fromarray(base.clip(0, 255).astype(np.uint8))


BG = background()

MODE = sys.argv[4] if len(sys.argv) > 4 else 'dubs'   # 'dubs': copies of copies; 'tapes': one pass, three tape types

DUB_SECTIONS = [
    ('ui_dry', 'DRY', 'The mix, straight from the DAW.', '0_dry.wav'),
    ('ui_g1', '1ST DUB', 'One pass through the tape.', 'g2.wav'),
    ('ui_g2', '2ND DUB', 'A copy of the copy.', 'g3.wav'),
    ('ui_g4', '4TH DUB', 'The bootleg from the market stall.', 'g5.wav'),
]
TAPE_SECTIONS = [
    ('ui_dry', 'DRY', 'The master, straight from the DAW.', '0_dry.wav'),
    ('ui_normal', 'NORMAL', 'One pass. Warmth, nothing lost.', 'normal.wav'),
    ('ui_chrome', 'CHROME', 'Warm, with the top end open.', 'chrome.wav'),
    ('ui_metal', 'METAL', 'The brightest tape. Your call.', 'metal.wav'),
]
SECTIONS = TAPE_SECTIONS if MODE == 'tapes' else DUB_SECTIONS


def frame_section(i):
    key, label, line, _ = SECTIONS[i]
    im = BG.copy()
    d = ImageDraw.Draw(im)
    tracked(d, W / 2, 250, f'{i + 1} / 4', font(30), GOLD, 8)
    tracked(d, W / 2, 310, label, font(96), INK, 14)
    d.text((W / 2, 460), line, font=font(40, False), fill=INK, anchor='mm')
    ui = Image.open(os.path.join(TMP, key + '.png')).convert('RGB')
    uw = W - 80
    ui = ui.resize((uw, round(ui.height * uw / ui.width)), Image.LANCZOS)
    shadow = Image.new('RGBA', (ui.width + 60, ui.height + 60), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle((30, 40, ui.width + 30, ui.height + 30), 14, fill=(0, 0, 0, 150))
    shadow = shadow.filter(ImageFilter.GaussianBlur(18))
    y0 = 620
    im.paste(shadow, (40 - 30, y0 - 30), shadow)
    im.paste(ui, (40, y0))
    # generation ladder under the panel: where this section sits
    if MODE == 'tapes':
        steps, here = ['DRY', 'NORMAL', 'CHROME', 'METAL'], i
    else:
        steps = ['DRY', 'MASTER', '1ST', '2ND', '3RD', '4TH']
        here = {0: 0, 1: 2, 2: 3, 3: 5}[i]
    ly = y0 + ui.height + 120
    gap = (W - 160) / (len(steps) - 1)
    for k, s in enumerate(steps):
        cx = 80 + k * gap
        on = k == here
        r = 16 if on else 9
        d.ellipse((cx - r, ly - r, cx + r, ly + r), fill=LAMP if on else MUTED)
        d.text((cx, ly + 50), s, font=font(26, on), fill=LAMP if on else MUTED, anchor='mm')
    d.line((80, ly, W - 80, ly), fill=MUTED, width=2)
    for k in range(len(steps)):
        cx = 80 + k * gap
        on = k == here
        r = 16 if on else 9
        d.ellipse((cx - r, ly - r, cx + r, ly + r), fill=LAMP if on else MUTED)
    tracked(d, W / 2, 1760, 'SUBVERSELAB', font(28), MUTED, 12)
    return im


def frame_intro():
    im = BG.copy()
    d = ImageDraw.Draw(im)
    tracked(d, W / 2, 700, 'KASET', font(170), INK, 34)
    tracked(d, W / 2, 900, 'MODEL KS-83', font(34), GOLD, 10)
    d.text((W / 2, 1040), 'One song, three tapes.' if MODE == 'tapes' else 'One song, four generations.', font=font(50, False), fill=INK, anchor='mm')
    d.text((W / 2, 1110), 'Headphones on.', font=font(40, False), fill=MUTED, anchor='mm')
    tracked(d, W / 2, 1760, 'SUBVERSELAB', font(28), MUTED, 12)
    return im


def frame_outro():
    im = BG.copy()
    d = ImageDraw.Draw(im)
    tracked(d, W / 2, 560, 'KASET', font(130), INK, 26)
    d.text((W / 2, 740), 'Warmth or wear: every knob is yours.' if MODE == 'tapes' else 'Cassette colour for your mix bus.',
           font=font(46, False), fill=INK, anchor='mm')
    tracked(d, W / 2, 880, 'OUT OCTOBER 1', font(48), LAMP, 10)
    d.rounded_rectangle((W / 2 - 390, 1000, W / 2 + 390, 1130), 20, outline=GOLD, width=4)
    d.text((W / 2, 1065), 'Free for the first 1,000 — DM “KASET”', font=font(40), fill=INK, anchor='mm')
    d.text((W / 2, 1230), 'macOS · Windows  ·  subverselab.com/plugins', font=font(34, False), fill=MUTED, anchor='mm')
    tracked(d, W / 2, 1760, 'SUBVERSELAB', font(28), MUTED, 12)
    return im


INTRO_S, SEG_S, OUTRO_S = 2.5, 3.5, 3.5   # 20 s: the length of the demo excerpts
shots = [('intro', frame_intro(), INTRO_S)]
for i in range(4):
    shots.append((f's{i}', frame_section(i), SEG_S))
shots.append(('outro', frame_outro(), OUTRO_S))

concat = os.path.join(TMP, 'shots.txt')
with open(concat, 'w') as fh:
    for name, img, dur in shots:
        p = os.path.join(TMP, name + '.png')
        img.save(p)
        fh.write(f"file '{p}'\nduration {dur}\n")
    fh.write(f"file '{os.path.join(TMP, shots[-1][0] + '.png')}'\n")

# Audio: the song keeps running; each section switches the source.
srcs = [sf.read(os.path.join(AUD, s[3]))[0] for s in SECTIONS]
n = lambda s: int(round(s * SR))
total = n(INTRO_S + 4 * SEG_S + OUTRO_S)
L = min(len(s) for s in srcs)
assert L >= total, 'demo excerpts are shorter than the Reel'
cuts = [(0.0, 0)] + [(INTRO_S + i * SEG_S, i) for i in range(4)] + [(INTRO_S + 4 * SEG_S, 3)]
audio = np.zeros((total, 2))
xf = n(0.04)
prev = None
for j, (start, k) in enumerate(cuts):
    a0 = n(start)
    a1 = n(cuts[j + 1][0]) if j + 1 < len(cuts) else total
    audio[a0:a1] = srcs[k][a0:a1]
    if prev is not None and prev != k:
        ramp = np.linspace(0, 1, xf)[:, None]
        audio[a0:a0 + xf] = srcs[prev][a0:a0 + xf] * (1 - ramp) + srcs[k][a0:a0 + xf] * ramp
    prev = k
audio[-n(1.5):] *= np.linspace(1, 0, n(1.5))[:, None]
audio /= max(1.0, np.abs(audio).max() / 0.95)
wav = os.path.join(TMP, 'reel.wav')
sf.write(wav, audio, SR, subtype='PCM_16')

subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-f', 'concat', '-safe', '0', '-i', concat, '-i', wav,
                '-vf', f'fps={FPS},format=yuv420p', '-c:v', 'libx264', '-preset', 'medium', '-crf', '18',
                '-c:a', 'aac', '-b:a', '256k', '-shortest', '-movflags', '+faststart', OUT], check=True)
print('wrote', OUT, round(total / SR, 2), 's')
