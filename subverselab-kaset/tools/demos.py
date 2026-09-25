"""Measures the three cassette presets and renders the comparison demos.

    python3 tools/demos.py <kasetraw binary> <scratch dir>
"""
import os, subprocess, sys
import numpy as np, soundfile as sf, scipy.signal as sg

BIN, TMP = sys.argv[1], sys.argv[2]
SR = 44100
PRESETS = [
    ('g2_first_dub',  dict(gen=2, wow=.40, flutter=.40, drive=.50, hiss=.45, wear=.10)),
    ('g3_second_dub', dict(gen=3, wow=.50, flutter=.50, drive=.55, hiss=.50, wear=.25)),   # the approved 'copy from a friend'
    ('g5_fourth_dub', dict(gen=5, wow=.55, flutter=.50, drive=.55, hiss=.50, wear=.30)),
]

def run(x, p):
    a, b = os.path.join(TMP, 'k_in.f32'), os.path.join(TMP, 'k_out.f32')
    x.astype(np.float32).tofile(a)
    subprocess.run([BIN, a, b, str(SR)] + [str(p[k]) for k in ('gen', 'wow', 'flutter', 'drive', 'hiss', 'wear')], check=True)
    return np.fromfile(b, dtype=np.float32).reshape(-1, 2).astype(np.float64)

def db(v): return 20 * np.log10(v + 1e-20)
def rms(v): return np.sqrt(np.mean(v ** 2))

print('%-20s %9s %9s %10s %9s %8s' % ('preset', 'W&F %pk', 'top -3dB', 'hiss dBFS', 'THD %', 'finite'))
for name, p in PRESETS:
    # Wow & flutter: a 1 kHz tone (3150 Hz falls in the lost top of a bootleg),
    # instantaneous frequency from the analytic signal, no hiss or dropouts.
    t = np.arange(12 * SR) / SR
    tone = 0.25 * np.sin(2 * np.pi * 1000 * t)
    y = run(np.stack([tone, tone], 1), dict(p, hiss=0, wear=0))[:, 0][2 * SR:-SR]   # the tail is zero after removing latency
    ph = np.unwrap(np.angle(sg.hilbert(y)))
    inst = np.diff(ph) * SR / (2 * np.pi)
    inst = sg.medfilt(inst, 101)
    wf = (np.percentile(inst, 99.5) - np.percentile(inst, 0.5)) / 2 / 1000 * 100
    # Frequency response: white noise, ratio of spectra; the -3 dB point above 1 kHz.
    rng = np.random.default_rng(3)
    nz = rng.normal(0, 0.05, (8 * SR, 2))
    p0 = dict(p, hiss=0, wear=0)
    yo = run(nz, p0)
    f, Pin = sg.welch(nz[:, 0], SR, nperseg=8192)
    _, Pout = sg.welch(yo[:, 0], SR, nperseg=8192)
    resp = 10 * np.log10(Pout / Pin)
    ref = np.mean(resp[(f > 500) & (f < 1500)])
    top = f[(f > 1000) & (resp < ref - 3)][0]
    # Hiss: silent input.
    h = run(np.zeros((6 * SR, 2)), dict(p, wear=0))[SR:]
    # THD: 1 kHz at -6 dBFS.
    s1 = 0.5 * np.sin(2 * np.pi * 1000 * t[:4 * SR])
    y1 = run(np.stack([s1, s1], 1), dict(p, wow=0, flutter=0, hiss=0, wear=0))[SR:, 0]
    F = np.abs(np.fft.rfft(y1 * np.hanning(len(y1)))); fr = np.fft.rfftfreq(len(y1), 1 / SR)
    amp = lambda hz: F[np.argmin(np.abs(fr - hz))]
    thd = np.sqrt(sum(amp(1000 * k) ** 2 for k in range(2, 6))) / amp(1000) * 100
    print('%-20s %9.3f %8.0f Hz %10.1f %9.2f %8s' % (name, wf, top, db(rms(h)), thd, np.isfinite(yo).all()))

# Demos: 20 s of Diplomat, dry reference plus the three tapes.
x, sr = sf.read(os.path.expanduser('~/Desktop/Diplomat.wav'), start=int(2.4 * SR), frames=20 * SR)
f = int(0.02 * SR); x[:f] *= np.linspace(0, 1, f)[:, None]; x[-f:] *= np.linspace(1, 0, f)[:, None]
x /= np.abs(x).max() / 0.85
os.makedirs('demos', exist_ok=True)
sf.write('demos/0_kuru_diplomat.wav', x, SR, subtype='PCM_24')
for name, p in PRESETS:
    y = run(x, p)
    y *= rms(x) / rms(y)                 # same loudness as the dry file, so only the colour differs
    y /= max(1.0, np.abs(y).max() / 0.98)
    sf.write(f'demos/{name}.wav', y, SR, subtype='PCM_24')
    print('wrote demos/%s.wav' % name)
