"""Renders the character comparison demos and measures each impulse response.

    python3 tools/demos.py <wetraw binary> <scratch dir>
"""
import subprocess, sys, os
import numpy as np, soundfile as sf

BIN, TMP = sys.argv[1], sys.argv[2]
SR = 44100
STEMS = os.path.expanduser('~/Desktop/solo/SampleChopVol1/Bir Araya Gelemeyiz Project/Backup/mdx_extra_q/Bir Araya Gelemeyiz')
CHARS = ['hammam', 'cistern', 'valley']

def wet(ch, x):
    src, dst = os.path.join(TMP, 'in.f32'), os.path.join(TMP, 'out.f32')
    x.astype(np.float32).tofile(src)
    subprocess.run([BIN, ch, src, dst, str(SR)], check=True)
    return np.fromfile(dst, dtype=np.float32).reshape(-1, 2).astype(np.float64)

def rms(x): return np.sqrt(np.mean(x ** 2) + 1e-20)

# Impulse response: T60 from Schroeder integration, peak, stability.
print('%-7s %8s %8s %8s %10s' % ('char', 'T30->60', 'EDT', 'peak', 'tail@6s dB'))
for ch in CHARS:
    imp = np.zeros((SR * 10, 2)); imp[100] = 1.0
    ir = wet(ch, imp).mean(1)
    e = np.cumsum((ir ** 2)[::-1])[::-1]; edc = 10 * np.log10(e / e[0] + 1e-30)
    def t(db): return np.argmax(edc < db) / SR
    t30 = (t(-35) - t(-5)) * 2
    edt = t(-10) * 6
    tail = 20 * np.log10(rms(ir[SR * 6:SR * 6 + 4410]) / rms(ir[:SR]) + 1e-12)
    print('%-7s %8.2f %8.2f %8.3f %10.1f' % (ch, t30, edt, np.abs(ir).max(), tail))

def excerpt(name, start, dur):
    x, sr = sf.read(os.path.join(STEMS, name + '.wav'), start=int(start * SR), frames=int(dur * SR))
    fade = int(0.02 * SR); x[:fade] *= np.linspace(0, 1, fade)[:, None]; x[-fade:] *= np.linspace(1, 0, fade)[:, None]
    return x

voc = excerpt('vocals', 118.5, 10.0)
drm = excerpt('drums', 118.5, 6.0)

def with_tail(x, secs): return np.vstack([x, np.zeros((int(secs * SR), 2))])

def render(ch, x, level):
    x = with_tail(x, 4.0)
    w = wet(ch, x)
    w *= rms(x) * level / rms(w)          # every character at the same wet loudness
    return x + w

gap = np.zeros((SR, 2))
os.makedirs('demos', exist_ok=True)
dry = np.vstack([with_tail(voc, 4.0), gap, with_tail(drm, 4.0)])
sf.write('demos/0_kuru_referans.wav', dry / max(1.0, np.abs(dry).max() / 0.95), SR, subtype='PCM_24')
for i, ch in enumerate(CHARS, 1):
    out = np.vstack([render(ch, voc, 0.55), gap, render(ch, drm, 0.45)])
    out /= max(1.0, np.abs(out).max() / 0.95)
    sf.write('demos/%d_%s.wav' % (i, ch), out, SR, subtype='PCM_24')
    print('wrote demos/%d_%s.wav' % (i, ch))
