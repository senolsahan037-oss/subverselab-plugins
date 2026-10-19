#!/usr/bin/env python3
"""Synth listening material for the SEDA pack (TASK_P2). 48 kHz, 24-bit, stereo, 120 BPM, A minor.
Writes tools/demo_material/{vocal,lead,pluck}.wav (16 s) and drums.wav (8 s loop). Everything is synthesised here (house rule: demo sound is synth)."""
import os, struct
import numpy as np
from scipy.signal import lfilter, butter, sosfilt

HERE = os.path.dirname(os.path.abspath(__file__)); OUT = os.path.join(HERE, "demo_material")
SR = 48000; SPB = 0.5   # 120 BPM

def amp(d): return 10 ** (d / 20)
def rms_db(x, mask=None):
    x = np.asarray(x, dtype=np.float64); x = x[mask] if mask is not None else x
    return 10 * np.log10(np.mean(x ** 2) + 1e-30)
def rbj_peak(f0, q, gain_db):
    A = 10 ** (gain_db / 40); w0 = 2 * np.pi * f0 / SR; al = np.sin(w0) / (2 * q)
    b = np.array([1 + al * A, -2 * np.cos(w0), 1 - al * A]); a = np.array([1 + al / A, -2 * np.cos(w0), 1 - al / A])
    return b / a[0], a / a[0]
def lowpass(x, f, order=2): return sosfilt(butter(order, f, btype="low", fs=SR, output="sos"), x, axis=0)
def highpass(x, f, order=2): return sosfilt(butter(order, f, btype="high", fs=SR, output="sos"), x, axis=0)
def bandpass(x, lo, hi, order=2): return sosfilt(butter(order, [lo, hi], btype="band", fs=SR, output="sos"), x, axis=0)
def saw(f): return 2 * (np.cumsum(f / SR) % 1.0) - 1
def square(f): return np.sign(np.sin(2 * np.pi * np.cumsum(f / SR)))
def midi(n): return 440.0 * 2 ** ((n - 69) / 12)

def write_wav24(path, x):
    x = np.clip(np.asarray(x, dtype=np.float64), -1.0, 1.0)
    if x.ndim == 1: x = np.stack([x, x], axis=1)
    q = np.round(x * 8388607).astype(np.int32); b = np.empty((q.size, 3), dtype=np.uint8); flat = q.reshape(-1)
    b[:, 0] = flat & 255; b[:, 1] = (flat >> 8) & 255; b[:, 2] = (flat >> 16) & 255; data = b.tobytes()
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 2, SR, SR * 6, 6, 24))
        f.write(b"data" + struct.pack("<I", len(data)) + data)

def place(buf, sig, at, pan=0.5):
    i = int(round(at * SR)); m = min(len(sig), len(buf) - i)
    if m > 0: buf[i:i + m, 0] += sig[:m] * (1 - pan) * 2; buf[i:i + m, 1] += sig[:m] * pan * 2

# ---- vocal: formant-synth line, 4 s phrases with rests (the same recipe as the DYNEQ pack's vocal)
VOW = {"a": (800, 1200, 2800, 3500), "e": (500, 1900, 2600, 3300), "o": (450, 850, 2700, 3400), "i": (320, 2300, 3000, 3600)}
def make_vocal(rng, secs=16.0):
    N = int(secs * SR); T = np.arange(N) / SR
    notes = [(0.0, 440.0, "a", 0.9), (1.0, 392.0, "o", 0.5), (1.5, 329.63, "e", 1.0), (3.0, 349.23, "i", 0.8),
             (4.0, 329.63, "a", 0.6), (4.75, 293.66, "o", 0.5), (5.25, 261.63, "a", 1.2), (7.0, 220.0, "e", 0.8)]
    f0 = np.full(N, 220.0); gate = np.zeros(N); vw = {k: np.zeros(N) for k in VOW}
    for (at, f, v, d) in [(a + 4.0 * r, f, v, d) for r in range(4) for (a, f, v, d) in notes if a + 4.0 * r < 15.5]:
        s = int((0.3 + at * 0.97) * SR); e = min(N, s + int(d * SR))
        if s >= N: continue
        f0[s:e] = f; gate[s:e] = 1; vw[v][s:e] = 1
    f0 = f0 * (1 + 0.012 * np.sin(2 * np.pi * 5.6 * T))
    src = lowpass(saw(f0), 6000, 1)
    a_ = 1 - np.exp(-1 / (0.04 * SR)); r_ = 1 - np.exp(-1 / (0.12 * SR)); env = lfilter([a_], [1, -(1 - a_)], gate)
    env = np.minimum(env, 1.0)
    sm = 1 - np.exp(-1 / (0.06 * SR)); out = np.zeros(N); wsum = np.zeros(N)
    for v, fm in VOW.items():
        w = lfilter([sm], [1, -(1 - sm)], vw[v]); y = np.zeros(N)
        for f, g, q in zip(fm, (14, 12, 8, 5), (7, 8, 9, 9)): y += lfilter(*rbj_peak(f, q, g), src)
        out += y * w; wsum += w
    out = out / np.maximum(wsum, 1e-3) * env
    out += 0.05 * bandpass(rng.standard_normal(N), 2500, 6500) * env
    return bandpass(out, 300, 4200)

# ---- lead: mono synth, detuned saw + square through a moving low-pass, vibrato, 8th-note melody with rests
def make_lead(secs=16.0):
    N = int(secs * SR); buf = np.zeros((N, 2))
    A = 57  # A3 as MIDI number base; melody in A minor, (step in 8ths, midi, length in 8ths)
    mel = [(0, 69, 3), (3, 72, 1), (4, 76, 2), (6, 74, 1), (7, 72, 1),            # bar 1
           (8, 71, 3), (11, 69, 1), (12, 67, 4),                                  # bar 2
           (16, 69, 2), (18, 72, 2), (20, 76, 3), (23, 79, 1),                    # bar 3
           (24, 77, 3), (27, 76, 1), (28, 74, 2), (30, 72, 1)]                    # bar 4 (rests at the end of each phrase)
    for rep in range(4):
        for (st, n, ln) in mel:
            at = (rep * 32 + st) * SPB / 2
            if rep % 2 == 1 and st >= 24: n -= 2      # the second pass of the tune bends a little
            if at >= secs - 0.2: continue
            d = ln * SPB / 2 * 0.92; tn = np.arange(int((d + 0.12) * SR)) / SR; f = midi(n)
            vib = 1 + 0.006 * np.sin(2 * np.pi * 5.5 * tn) * np.minimum(1, tn / 0.25)
            ff = f * vib
            s = 0.5 * saw(ff * 0.997) + 0.5 * saw(ff * 1.003) + 0.35 * square(ff * 0.5)
            env = np.minimum(1, tn / 0.008) * np.where(tn < d, 1.0, np.exp(-(tn - d) / 0.05)) * (0.75 + 0.25 * np.exp(-tn / 0.3))
            # filter sweep: one-pole low-pass whose cutoff opens over the first 120 ms
            fc = 900 + 3200 * (1 - np.exp(-tn / 0.12)); k = 1 - np.exp(-2 * np.pi * fc / SR)
            y = np.zeros(len(tn)); st_ = 0.0
            for i in range(len(tn)):
                st_ += k[i] * (s[i] - st_); y[i] = st_
            y = lowpass(y, 6500, 2) * env
            i0 = int(round(at * SR)); m = min(len(y), N - i0)
            buf[i0:i0 + m, 0] += y[:m]
    buf[:, 1] = buf[:, 0]
    return buf

# ---- drums: 8 s (4 bar) loop, kick/snare/hats with syncopation and ghost notes
def make_drums(rng, secs=8.0):
    N = int(secs * SR); buf = np.zeros((N, 2))
    tk = np.arange(int(0.45 * SR)) / SR
    ph = 2 * np.pi * np.cumsum(48 + 110 * np.exp(-tk / 0.025)) / SR
    kick = (np.sin(ph) * np.exp(-tk / 0.16) + 0.18 * np.exp(-tk / 0.003) * np.sin(2 * np.pi * 2500 * tk)) * 0.9
    ts = np.arange(int(0.25 * SR)) / SR
    snare = 0.5 * lowpass(rng.standard_normal(len(ts)), 9000) * np.exp(-ts / 0.07) + 0.45 * np.sin(2 * np.pi * 185 * ts) * np.exp(-ts / 0.07)
    th = np.arange(int(0.06 * SR)) / SR
    hat = highpass(rng.standard_normal(len(th)), 7000) * np.exp(-th / 0.014) * 0.25
    ohat = highpass(rng.standard_normal(int(0.2 * SR)), 6500) * np.exp(-np.arange(int(0.2 * SR)) / SR / 0.07) * 0.22
    s16 = SPB / 4
    kicks = [0, 6, 10, 16, 22, 26, 29, 32, 38, 42, 48, 54, 58, 61]            # 16th-note steps over 4 bars (64 steps)
    snares = [4, 12, 20, 28, 36, 44, 52, 60]; ghosts = [15, 31, 47, 63]
    for s in kicks: place(buf, kick, s * s16)
    for s in snares: place(buf, snare, s * s16)
    for s in ghosts: place(buf, snare * 0.22, s * s16)
    for s in range(64):
        if s % 2 == 0: place(buf, hat * (1.0 if s % 4 == 0 else 0.7), s * s16, 0.4 if s % 4 == 0 else 0.6)
        if s % 16 == 14: place(buf, ohat, s * s16, 0.55)
    return buf

# ---- pluck: Karplus-Strong arpeggio, Am F C G (2 bars each), 8th notes up-and-down
def ks(freq, dur, rng, decay=0.996, bright=0.5):
    N = int(round(SR / freq)); n = int(dur * SR)
    x = np.zeros(n); x[:N] = lowpass(rng.uniform(-1, 1, N), 1500 + 5000 * bright, 1)
    a = np.zeros(N + 2); a[0] = 1; a[N] = -decay * 0.5; a[N + 1] = -decay * 0.5
    y = lfilter([1.0], a, x)
    return y * np.minimum(1, np.arange(n) / (0.002 * SR))
def make_pluck(rng, secs=16.0):
    N = int(secs * SR); buf = np.zeros((N, 2))
    chords = [(57, 60, 64, 69), (53, 57, 60, 65), (55, 60, 64, 67), (55, 59, 62, 67)]   # Am F C G
    pattern = [0, 1, 2, 3, 2, 1, 2, 1]   # string index per 8th
    for bar in range(int(secs / (4 * SPB))):
        ch = chords[(bar // 2) % 4]
        for i in range(8):
            if (bar % 4 == 3) and i >= 6: continue   # a small gap at the end of every fourth bar
            n = ch[pattern[i]] + (12 if pattern[i] == 3 and bar % 2 else 0)
            at = bar * 4 * SPB + i * SPB / 2 + (0.006 if i % 2 else 0.0)
            y = ks(midi(n), 1.6, rng, 0.9965, 0.35 + 0.15 * (i % 2))
            place(buf, y, at, 0.35 + 0.1 * pattern[i])
    return highpass(buf, 90, 1)

# ---- P3e: 30 s vocal line with marked breath gaps and phrase ends (the LISTEN test material; vocal_p3e.wav + vocal_p3e.json)
# Fixed layout: (kind, length s, breath gaps inside, gap after s). kinds: "phrase" (1.5-4 s), "short" (one 0.8 s word, must NOT throw),
# "word300" (one word ending exactly 300 ms before the next phrase, must NOT throw).
P3E_LAYOUT = [("phrase", 3.6, 3, 1.4), ("phrase", 2.4, 2, 2.0), ("phrase", 4.0, 4, 1.0), ("short", 0.8, 0, 1.5), ("phrase", 3.0, 3, 0.8),
              ("word300", 0.7, 0, 0.3), ("phrase", 1.8, 2, 1.2), ("phrase", 2.6, 3, 2.4)]
def random_layout(rng, secs):
    out = []; t = 0.5
    while True:
        r = rng.random(); gap = float(rng.uniform(0.8, 2.5))
        if out and out[-1][0] == "word300": r = 1.0   # a word that sits 300 ms before the next phrase is followed by a real phrase (a short item there would bridge into one 1.6 s run of material)
        if r < 0.12: item = ("short", 0.8, 0, gap)
        elif r < 0.22: item = ("word300", float(rng.uniform(0.5, 0.9)), 0, 0.3)
        else:
            nb = int(rng.integers(2, 5)); item = ("phrase", float(rng.uniform(1.5, 4.0)), nb, gap)
        if t + item[1] + item[3] > secs - 0.3: break
        out.append(item); t += item[1] + item[3]
    return out

def make_vocal_line(seed=15, secs=30.0, layout=None, lead_in=0.5):
    """Returns (mono float64 signal, marks dict). Marks hold nominal gate times in seconds (the audible edge lags by the 40 ms envelope)."""
    rng = np.random.default_rng(seed); layout = P3E_LAYOUT if layout is None else layout
    N = int(secs * SR); T = np.arange(N) / SR
    gate = np.zeros(N); f0 = np.full(N, 220.0); vw = {k: np.zeros(N) for k in VOW}
    marks = dict(sr=SR, secs=secs, phrases=[], phrase_ends=[], breath_gaps=[], short_ends=[], word300=[], onsets=[])
    t = lead_in; scale = [220.0, 246.94, 261.63, 293.66, 329.63, 392.0, 440.0]
    for kind, L, nb, gap_after in layout:
        words = []
        if nb == 0: words.append((t, t + L))
        else:
            gaps = rng.uniform(0.15, 0.35, nb); wl = rng.uniform(0.7, 1.3, nb + 1); wl = wl / wl.sum() * (L - gaps.sum()); tt = t
            for k in range(nb + 1):
                words.append((tt, tt + wl[k]))
                if k < nb: marks["breath_gaps"].append(dict(t0=round(tt + wl[k], 4), t1=round(tt + wl[k] + gaps[k], 4))); tt += wl[k] + gaps[k]
        for (a, b) in words:
            s, e = int(a * SR), min(N, int(b * SR)); gate[s:e] = 1; f0[s:e] = rng.choice(scale); vw[rng.choice(list(VOW))][s:e] = 1
        end = t + L
        marks["phrases"].append(dict(kind=kind, t0=round(t, 4), t1=round(end, 4), words=[[round(a, 4), round(b, 4)] for a, b in words]))
        if kind == "phrase": marks["phrase_ends"].append(dict(t=round(end, 4), gap=gap_after)); marks["onsets"].append(round(t, 4))
        elif kind == "short": marks["short_ends"].append(dict(t=round(end, 4), gap=gap_after)); marks["onsets"].append(round(t, 4))
        else: marks["word300"].append(dict(t=round(end, 4), gap=gap_after)); marks["onsets"].append(round(t, 4))
        t = end + gap_after
    # the last phrase ends the sung part; its gap runs to the end of the file (a phrase end, the render tail rings out there)
    f0 = f0 * (1 + 0.012 * np.sin(2 * np.pi * 5.6 * T))
    # one-pole smoothing of the pitch steps so words glide a little (no clicks)
    f0 = lfilter([1 - np.exp(-1 / (0.02 * SR))], [1, -np.exp(-1 / (0.02 * SR))], f0)
    src = lowpass(saw(f0), 6000, 1)
    a_ = 1 - np.exp(-1 / (0.04 * SR)); env = np.minimum(lfilter([a_], [1, -(1 - a_)], gate), 1.0)
    sm = 1 - np.exp(-1 / (0.06 * SR)); out = np.zeros(N); wsum = np.zeros(N)
    for v, fm in VOW.items():
        w = lfilter([sm], [1, -(1 - sm)], vw[v]); y = np.zeros(N)
        for f, g, q in zip(fm, (14, 12, 8, 5), (7, 8, 9, 9)): y += lfilter(*rbj_peak(f, q, g), src)
        out += y * w; wsum += w
    out = out / np.maximum(wsum, 1e-3) * env
    out += 0.05 * bandpass(rng.standard_normal(N), 2500, 6500) * env
    out = bandpass(out, 300, 4200)
    out = out * amp(-25 - rms_db(out, np.abs(out) > 0.003 * np.max(np.abs(out)))); out = out * min(1.0, amp(-1.5) / np.max(np.abs(out)))
    return out, marks

def write_p3e():
    import json
    x, marks = make_vocal_line(); os.makedirs(OUT, exist_ok=True)
    write_wav24(os.path.join(OUT, "vocal_p3e.wav"), np.stack([x, x], axis=1)); json.dump(marks, open(os.path.join(OUT, "vocal_p3e.json"), "w"), indent=1)
    print(f"vocal_p3e.wav: {len(x) / SR:.1f} s, peak {20 * np.log10(np.max(np.abs(x))):.1f} dBFS, {len(marks['phrase_ends'])} phrase ends, {len(marks['breath_gaps'])} breath gaps, "
          f"{len(marks['short_ends'])} short phrase, {len(marks['word300'])} word with a 300 ms gap")

def level(x, target_db, active_thr=None):
    mask = np.abs(x).max(axis=1) > active_thr if active_thr is not None else None
    x = x * amp(target_db - rms_db(x, mask))
    pk = np.max(np.abs(x))
    return x * min(1.0, amp(-1.5) / pk)   # peaks of the dry files stay under -1.5 dBFS

def main():
    os.makedirs(OUT, exist_ok=True); rng = np.random.default_rng(15)
    voc = make_vocal(rng); voc = level(np.stack([voc, voc], axis=1), -25, 0.01)   # quieter: the wet-only file (05) has a ~20 dB crest and must match this loudness under -1 dBTP
    lead = level(make_lead(), -22, 0.01)
    drums = level(make_drums(rng), -20)
    pluck = level(make_pluck(rng), -23, 0.005)
    for name, x in (("vocal", voc), ("lead", lead), ("drums", drums), ("pluck", pluck)):
        write_wav24(os.path.join(OUT, name + ".wav"), x)
        print(f"{name}.wav: peak {20 * np.log10(np.max(np.abs(x))):.1f} dBFS, RMS {rms_db(x):.1f} dBFS, {len(x) / SR:.1f} s, "
              f"{'mono (L=R)' if np.array_equal(x[:, 0], x[:, 1]) else 'stereo'}")

if __name__ == "__main__":
    write_p3e() if "--p3e" in __import__("sys").argv else main()
