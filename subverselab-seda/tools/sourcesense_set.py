#!/usr/bin/env python3
"""Labelled synthetic clips for SourceSense v0 (TASK_P1b).

  sourcesense_set.py --out DIR [--set A|B|C] [--per-class 10]

8 classes (vocal, drums, kick, bass, keys, pad, fullmix, unsure) x per-class clips, 11 s, 48 kHz, 16-bit stereo, peak level -30 .. -6 dBFS
(except the "quiet room" unsure clips). Everything varies per clip: tempo, key/register, timbre, pattern, level, stereo.
Sets A, B, C use different seeds. A is the development set (the rules were read off it); B and C are held-out sets: B was evaluated once
before any rule change that came after looking at it, see STATUS.md. Writes DIR/<class>_<set><NN>.wav and DIR/labels.json.
"""
import argparse, json, os, struct
import numpy as np
from scipy.signal import lfilter, butter, sosfilt

SR = 48000
DUR = 11.0
N = int(SR * DUR)
CLASSES = ["vocal", "drums", "kick", "bass", "keys", "pad", "fullmix", "unsure"]
SET_BASE = {"A": 1, "B": 2, "C": 3}


# ---------------------------------------------------------------------------------------------- helpers
def mtof(m): return 440.0 * 2.0 ** ((np.asarray(m, float) - 69.0) / 12.0)

def place(buf, sig, start, pan=0.0):
    """Add mono `sig` into the stereo buffer at sample `start`; pan -1 .. 1 (constant power)."""
    start = int(start)
    if start >= buf.shape[0] or start + len(sig) <= 0: return
    a = max(0, -start); b = min(len(sig), buf.shape[0] - start)
    th = (pan + 1.0) * np.pi / 4.0
    buf[start + a:start + b, 0] += sig[a:b] * np.cos(th)
    buf[start + a:start + b, 1] += sig[a:b] * np.sin(th)

def ar_env(n, a, r):
    e = np.ones(n); na = min(n, max(1, int(a * SR))); nr = min(n, max(1, int(r * SR)))
    e[:na] *= np.linspace(0, 1, na) ** 1.5
    e[n - nr:] *= np.linspace(1, 0, nr)
    return e

def reson(x, f, bw):
    r = np.exp(-np.pi * bw / SR); a1 = -2 * r * np.cos(2 * np.pi * f / SR)
    return lfilter([1 - r], [1, a1, r * r], x)

def lp(x, fc, order=2): return sosfilt(butter(order, fc / (SR / 2), "low", output="sos"), x)
def hp(x, fc, order=2): return sosfilt(butter(order, fc / (SR / 2), "high", output="sos"), x)
def bp(x, f1, f2, order=2): return sosfilt(butter(order, [f1 / (SR / 2), f2 / (SR / 2)], "band", output="sos"), x)

def rms(x): return float(np.sqrt(np.mean(np.asarray(x, float) ** 2) + 1e-30))


# ---------------------------------------------------------------------------------------------- sources
VOWELS = [(730, 1090, 2440), (270, 2290, 3010), (300, 870, 2240), (660, 1700, 2400), (570, 840, 2410), (530, 1840, 2480)]

def gen_vocal(rng, out=None, level=1.0):
    out = np.zeros((N, 2)) if out is None else out
    high = rng.random() < 0.5
    f0c = rng.uniform(170, 300) if high else rng.uniform(85, 150)
    fs = rng.uniform(1.1, 1.2) if high else 1.0
    tilt = rng.uniform(0.55, 0.97); breath = rng.uniform(0.005, 0.08)       # brightness and breathiness of the source
    sylmax = rng.uniform(0.3, 0.9); gapmax = rng.uniform(0.5, 1.8); gapmin = rng.uniform(0.1, 0.45)   # syllable length, pause between phrases
    t = rng.uniform(0.1, 0.6)
    while t < DUR:
        for _ in range(int(rng.integers(2, 7))):
            dur = rng.uniform(0.1, sylmax); n = int(dur * SR)
            f0 = f0c * 2 ** (rng.integers(-5, 6) / 12.0) * (1 + rng.uniform(0, 0.05) * np.linspace(0, 1, n))
            if dur > 0.25: f0 = f0 * (1 + 0.006 * np.sin(2 * np.pi * rng.uniform(4.5, 6) * np.arange(n) / SR))
            src = (np.cumsum(f0 / SR) % 1.0) * 2 - 1
            src = lfilter([1.0], [1, -tilt], src) * 0.1 + rng.standard_normal(n) * breath * 0.1
            v = VOWELS[int(rng.integers(0, len(VOWELS)))]
            y = sum(reson(src, fr * fs * rng.uniform(0.95, 1.05), bw) for fr, bw in zip(v, (70, 100, 140)))
            y = y / rms(y) * rng.uniform(0.7, 1.0) * ar_env(n, rng.uniform(0.012, 0.04), rng.uniform(0.03, 0.08))
            place(out, y * level, (t) * SR, 0.0)
            t += dur + rng.uniform(0.0, 0.05)
        t += rng.uniform(gapmin, gapmax)
    return out

def kick_hit(rng, f_end, decay, f_start=None):
    n = int(min(0.9, decay * 6) * SR); tt = np.arange(n) / SR
    f_start = f_start or f_end * rng.uniform(2.0, 3.2)
    f = f_end + (f_start - f_end) * np.exp(-tt / rng.uniform(0.02, 0.04))
    y = np.sin(2 * np.pi * np.cumsum(f) / SR) * np.exp(-tt / decay)
    y += 0.3 * np.exp(-tt / 0.004) * rng.standard_normal(n) * rng.uniform(0.3, 1.0)
    return y

def snare_hit(rng):
    n = int(0.35 * SR); tt = np.arange(n) / SR
    y = bp(rng.standard_normal(n), 1200, 6500) * np.exp(-tt / rng.uniform(0.07, 0.14)) * 0.9
    y += np.sin(2 * np.pi * rng.uniform(170, 240) * tt) * np.exp(-tt / 0.06) * 0.6
    return y

def hat_hit(rng, open_=False):
    n = int((0.3 if open_ else 0.1) * SR); tt = np.arange(n) / SR
    return hp(rng.standard_normal(n), rng.uniform(6000, 8500)) * np.exp(-tt / (0.12 if open_ else rng.uniform(0.02, 0.05))) * 0.5

def gen_drums(rng, out=None, level=1.0):
    out = np.zeros((N, 2)) if out is None else out
    bpm = rng.uniform(70, 170); step = 60.0 / bpm / 4; kd = rng.uniform(0.08, 0.2); kf = rng.uniform(45, 65)
    kp = [[0, 8], [0, 6, 8, 14], [0, 3, 8, 10], [0, 4, 8, 12], [0, 7, 10]][int(rng.integers(0, 5))]
    sp = [[4, 12], [4, 12, 15], [12], [4, 10, 12]][int(rng.integers(0, 4))]
    hs = [1, 2][int(rng.integers(0, 2))]
    t0 = rng.uniform(0, 0.2); k = 0
    hat_pan = rng.uniform(0.1, 0.5) * (1 if rng.random() < 0.5 else -1)
    while t0 + k * step < DUR:
        s = k % 16; t = (t0 + k * step) * SR
        if s in kp: place(out, kick_hit(rng, kf, kd) * rng.uniform(0.8, 1.0) * 1.2, t, 0)
        if s in sp: place(out, snare_hit(rng) * rng.uniform(0.7, 1.0), t, rng.uniform(-0.1, 0.1))
        if s % hs == 0: place(out, hat_hit(rng, rng.random() < 0.06) * rng.uniform(0.4, 1.0), t, hat_pan if (s // hs) % 2 else -hat_pan)
        if k % 64 == 60 and rng.random() < 0.5: place(out, snare_hit(rng) * 0.8, t, 0)   # fill
        k += 1
    return out * level

def gen_kick(rng, out=None, level=1.0):
    out = np.zeros((N, 2)) if out is None else out
    bpm = rng.uniform(80, 150); beat = 60.0 / bpm; style = int(rng.integers(0, 3))
    kd = rng.uniform(0.08, 0.5); kf = rng.uniform(38, 70); t = rng.uniform(0.0, 0.3); k = 0
    while t < DUR:
        pat = [0, 1, 2, 3] if style == 0 else ([0, 2.5] if style == 1 else [0, 1.5, 2, 3.5])
        for b in pat: place(out, kick_hit(rng, kf, kd) * rng.uniform(0.85, 1.0), (t + b * beat) * SR, 0)
        t += 4 * beat; k += 1
    return out * level

def gen_bass(rng, out=None, level=1.0):
    out = np.zeros((N, 2)) if out is None else out
    bpm = rng.uniform(70, 150); beat = 60.0 / bpm; root = int(rng.integers(26, 46))   # D1 .. A2
    scale = [0, 3, 5, 7, 10, 12] if rng.random() < 0.5 else [0, 2, 4, 7, 9, 12]
    wave = int(rng.integers(0, 3)); cutoff = rng.uniform(250, 900); sustained = rng.random() < 0.45
    grid = [0.5, 1.0][int(rng.integers(0, 2))] * beat; t = rng.uniform(0, 0.3); k = 0
    while t < DUR:
        if rng.random() < 0.8:
            f = mtof(root + scale[int(rng.integers(0, len(scale)))])
            dur = grid * (rng.choice([1, 2, 4]) if sustained else 1) * 0.95; n = int(dur * SR + 0.3 * SR); tt = np.arange(n) / SR
            ph = (f * tt) % 1.0
            y = {0: ph * 2 - 1, 1: np.sign(ph - 0.5), 2: np.sin(2 * np.pi * f * tt) + 0.4 * np.sin(4 * np.pi * f * tt) + 0.2 * np.sin(6 * np.pi * f * tt)}[wave]
            y = lp(y, cutoff) + 0.6 * np.sin(2 * np.pi * f * tt)
            env = np.exp(-tt / rng.uniform(0.15, 0.5)) if not sustained else ar_env(n, 0.01, 0.12) * (0.7 + 0.3 * np.exp(-tt / 0.2))
            y = y * env; y = y / rms(y) * 0.5
            place(out, y, t * SR, 0)
        t += grid * (rng.choice([1, 1, 2]) if sustained else 1); k += 1
    return out * level

def gen_keys(rng, out=None, level=1.0):
    out = np.zeros((N, 2)) if out is None else out
    bpm = rng.uniform(70, 140); beat = 60.0 / bpm; kind = int(rng.integers(0, 3)); mode = int(rng.integers(0, 3))
    key = int(rng.integers(38, 62)); scale = [0, 2, 3, 5, 7, 8, 10] if rng.random() < 0.5 else [0, 2, 4, 5, 7, 9, 11]
    width = rng.uniform(0, 0.4)
    def note(m, dur, vel):
        f = mtof(m); n = int(dur * SR); tt = np.arange(n) / SR
        if kind == 0:     # pluck / guitar: harmonics with their own decays
            tau = rng.uniform(0.25, 0.9); y = sum(h ** -rng.uniform(0.7, 1.2) * np.sin(2 * np.pi * f * h * (1 + 0.0003 * h * h) * tt) * np.exp(-tt / (tau / h ** 0.4)) for h in range(1, 14) if f * h < 9000)
            y = y + 0.4 * lp(rng.standard_normal(n), 4000) * np.exp(-tt / 0.004)
        elif kind == 1:   # electric piano: 2-op FM with decaying index
            idx = rng.uniform(1.0, 3.0) * np.exp(-tt / 0.4); y = np.sin(2 * np.pi * f * tt + idx * np.sin(2 * np.pi * f * tt)) * np.exp(-tt / rng.uniform(0.5, 1.2))
            y += 0.25 * np.sin(2 * np.pi * f * 14 * tt) * np.exp(-tt / 0.03)
        else:             # felt piano: a few partials, hammer thump
            tau = rng.uniform(0.6, 1.6); y = sum(a * np.sin(2 * np.pi * f * h * tt) * np.exp(-tt / (tau / h ** 0.5)) for h, a in ((1, 1.0), (2, 0.5), (3, 0.3), (4, 0.15), (5, 0.1)))
            y = y + 0.5 * lp(rng.standard_normal(n), 800) * np.exp(-tt / 0.01)
        y = y / (rms(y) + 1e-9) * 0.5 * vel
        return y * ar_env(n, 0.002, 0.05)
    t = rng.uniform(0, 0.3); step = {0: beat / 2, 1: beat / 3, 2: beat / 4}[mode]
    chord = [0, 2, 4]; k = 0
    while t < DUR:
        if mode == 0 and k % 6 == 0:      # chords with a strum
            base = key + scale[int(rng.integers(0, 7))]
            for i, o in enumerate((0, 4, 7, 12)): place(out, note(base + o, 1.6, rng.uniform(0.7, 1.0)), (t + i * 0.012) * SR, rng.uniform(-width, width))
        elif mode > 0:                    # arpeggio
            m = key + scale[(chord[k % 3] + (k // 3) * 2) % 7] + 12 * (k % 2)
            place(out, note(m, 1.2, rng.uniform(0.7, 1.0)), t * SR, rng.uniform(-width, width))
        t += step; k += 1
        if k % 12 == 0: chord = [(c + int(rng.integers(0, 4))) % 7 for c in chord]
    return out * level

def gen_pad(rng, out=None, level=1.0):
    out = np.zeros((N, 2)) if out is None else out
    key = int(rng.integers(40, 56)); scale = [0, 3, 7, 10, 14] if rng.random() < 0.5 else [0, 4, 7, 11, 14]
    cut = rng.uniform(800, 3500); att = rng.uniform(0.4, 1.4); lead = rng.random() < 0.3
    t = rng.uniform(0, 0.5)
    while t < DUR:
        dur = rng.uniform(3.0, 6.0)
        notes = [key + 12 * int(rng.integers(0, 2)) + scale[i] for i in (rng.permutation(5)[:int(rng.integers(3, 5))])] if not lead else [key + 12 + scale[int(rng.integers(0, 5))]]
        for m in notes:
            f = mtof(m); n = int((dur + 1.5) * SR); tt = np.arange(n) / SR; voices = int(rng.integers(3, 7)); y = np.zeros(n)
            for vi in range(voices):
                ph = (f * 2 ** (rng.uniform(-9, 9) / 1200.0) * tt + rng.random()) % 1.0
                y += ph * 2 - 1
            y = lp(y, cut) * (1 + 0.15 * np.sin(2 * np.pi * rng.uniform(0.1, 0.4) * tt))
            if lead: y *= 1 + 0.08 * np.sin(2 * np.pi * 5.0 * tt)
            y = y / rms(y) * 0.35 * ar_env(n, att, rng.uniform(0.8, 1.5))
            place(out, y, t * SR, rng.uniform(-0.6, 0.6) if not lead else 0.0)
        t += dur * rng.uniform(0.8, 1.0)
    return out * level

def gen_fullmix(rng, out=None, level=1.0):
    out = np.zeros((N, 2))
    parts = [(gen_drums, rng.uniform(0.7, 1.0)), (gen_bass, rng.uniform(0.6, 1.0)),
             ((gen_keys if rng.random() < 0.5 else gen_pad), rng.uniform(0.5, 0.9))]
    if rng.random() < 0.8: parts.append((gen_vocal, rng.uniform(0.5, 0.9)))
    if rng.random() < 0.4: parts.append((gen_pad, rng.uniform(0.3, 0.6)))
    for g, a in parts:
        y = g(rng); y = y / (rms(y) + 1e-9) * 0.15 * a
        # widen: a short random delay on one side for the tonal parts
        if g in (gen_keys, gen_pad, gen_vocal) and rng.random() < 0.7:
            d = int(rng.uniform(0.004, 0.02) * SR); y2 = np.roll(y[:, 1], d); y = np.stack([y[:, 0], 0.8 * y2 + 0.2 * y[:, 1]], 1)
        out += y
    return out * level

def gen_unsure(rng, kind):
    n = N; tt = np.arange(n) / SR
    if kind == 0: x = rng.standard_normal((n, 2)) * 0.2                                      # white noise, uncorrelated sides
    elif kind == 1:                                                                         # pink noise
        w = rng.standard_normal(n); x = lfilter([0.049922035, -0.095993537, 0.050612699, -0.004408786], [1, -2.494956002, 2.017265875, -0.522189400], w)[:, None] * np.ones((1, 2))
    elif kind == 2: f = rng.uniform(150, 2500); x = np.sin(2 * np.pi * f * tt)[:, None] * np.ones((1, 2)) * 0.3   # steady sine
    elif kind == 3: f = 400 + 300 * np.sin(2 * np.pi * rng.uniform(0.2, 0.6) * tt) + 100 * tt; x = np.sin(2 * np.pi * np.cumsum(f) / SR)[:, None] * np.ones((1, 2)) * 0.3   # siren
    elif kind == 4: x = lp(rng.standard_normal((n, 2)), 3000) * 10 ** (rng.uniform(-80, -70) / 20)   # quiet room hiss (below the gate)
    elif kind == 5:                                                                         # random sample-and-hold bleeps, 10 per s
        f = np.repeat(rng.uniform(300, 2000, int(DUR * 10) + 1), int(SR / 10))[:n]; x = (np.sin(2 * np.pi * np.cumsum(f) / SR) * (0.5 + 0.5 * np.sin(2 * np.pi * 10 * tt - 1.5)) ** 2)[:, None] * np.ones((1, 2)) * 0.3
    elif kind == 6:                                                                         # wind: band-passed noise whose centre sweeps 200 -> 3000 Hz
        w = rng.standard_normal(n); x = np.zeros(n); seg = SR // 2
        for i0 in range(0, n, seg):
            fc = 200 * (15.0 ** (i0 / n)); x[i0:i0 + seg] = bp(w[i0:i0 + seg], fc * 0.8, fc * 1.25)[:len(x[i0:i0 + seg])]
        x = x[:, None] * np.ones((1, 2)) * 1.0
    elif kind == 7: x = (lp(rng.standard_normal(n), 1500) * (0.6 + 0.4 * np.sin(2 * np.pi * 0.4 * tt)))[:, None] * np.ones((1, 2)) * 0.3   # slowly pulsing low-passed noise
    elif kind == 8: x = (bp(rng.standard_normal(n), 400, 600) * (0.5 + 0.5 * np.sin(2 * np.pi * 0.25 * tt)))[:, None] * np.ones((1, 2)) * 0.4   # narrow noise swell
    else: f = rng.choice([50.0, 60.0]); x = sum(a * np.sin(2 * np.pi * f * h * tt) for h, a in ((1, 1), (2, 0.5), (3, 0.7), (5, 0.3)))[:, None] * np.ones((1, 2)) * 0.2   # mains hum
    return x


GEN = {"vocal": gen_vocal, "drums": gen_drums, "kick": gen_kick, "bass": gen_bass, "keys": gen_keys, "pad": gen_pad, "fullmix": gen_fullmix}


def write_wav16(path, x):
    pcm = np.clip(np.round(x * 32767.0), -32768, 32767).astype("<i2")
    data = pcm.tobytes()
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 2, SR, SR * 4, 4, 16))
        f.write(b"data" + struct.pack("<I", len(data)) + data)


def make_clip(cls, idx, set_name="A"):
    """Returns (stereo float array, info dict). Deterministic per (cls, idx, set)."""
    seed = SET_BASE[set_name] * 10000 + CLASSES.index(cls) * 100 + idx
    rng = np.random.default_rng(seed)
    if cls == "unsure":
        kind = idx % 10; x = gen_unsure(rng, kind)
        peak_db = rng.uniform(-30, -6) if kind != 4 else None
    else:
        x = GEN[cls](rng); peak_db = rng.uniform(-30, -6)
    if peak_db is not None: x = x / (np.max(np.abs(x)) + 1e-12) * 10 ** (peak_db / 20)
    return x, {"seed": seed, "peak_db": None if peak_db is None else round(float(peak_db), 1)}


def make_set(out_dir, set_name="A", per_class=10, classes=CLASSES):
    os.makedirs(out_dir, exist_ok=True); labels = {}
    for cls in classes:
        for i in range(per_class):
            x, info = make_clip(cls, i, set_name); name = f"{cls}_{set_name}{i:02d}.wav"
            write_wav16(os.path.join(out_dir, name), x); labels[name] = dict(cls=cls, **info)
    json.dump(labels, open(os.path.join(out_dir, "labels.json"), "w"), indent=1)
    return labels


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True); ap.add_argument("--set", default="A", choices=sorted(SET_BASE)); ap.add_argument("--per-class", type=int, default=10)
    a = ap.parse_args()
    lab = make_set(a.out, a.set, a.per_class)
    print(f"wrote {len(lab)} clips ({a.per_class} per class, {len(CLASSES)} classes) to {a.out}")
