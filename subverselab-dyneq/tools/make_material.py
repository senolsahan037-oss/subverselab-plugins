#!/usr/bin/env python3
"""Synth listening material for the Dynamic EQ pack (TASK_P2). 48 kHz, 24-bit, stereo, 16 s, 120 BPM, A minor.
Writes tools/demo_material/{beat,bass,kick,vocal,resonant}.wav. Everything is synthesised here (house rule: demo sound is synth).
The KORO female vocal is used instead of the formant line if a dry stem exists under subverselab-koro (none was found on 9 Oct)."""
import os, sys, glob, struct
import numpy as np
from scipy.signal import lfilter, butter, sosfilt

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "demo_material")
SR = 48000; SECS = 16.0; N = int(SECS * SR); SPB = 0.5   # 120 BPM
T = np.arange(N) / SR

def amp(d): return 10 ** (d / 20)
def rbj_peak(f0, q, gain_db, sr=SR):
    A = 10 ** (gain_db / 40); w0 = 2 * np.pi * f0 / sr; al = np.sin(w0) / (2 * q)
    b = np.array([1 + al * A, -2 * np.cos(w0), 1 - al * A]); a = np.array([1 + al / A, -2 * np.cos(w0), 1 - al / A])
    return b / a[0], a / a[0]
def bandpass(x, lo, hi, order=2): return sosfilt(butter(order, [lo, hi], btype="band", fs=SR, output="sos"), x, axis=0)
def lowpass(x, f, order=2): return sosfilt(butter(order, f, btype="low", fs=SR, output="sos"), x, axis=0)
def rms_db(x, mask=None):
    x = np.asarray(x, dtype=np.float64); x = x[mask] if mask is not None else x
    return 10 * np.log10(np.mean(x ** 2) + 1e-30)
def saw(f): return 2 * (np.cumsum(f / SR) % 1.0) - 1

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
    i = int(round(at * SR)); m = min(len(sig), N - i)
    if m > 0: buf[i:i + m, 0] += sig[:m] * (1 - pan) * 2; buf[i:i + m, 1] += sig[:m] * pan * 2

def make_kick():
    tk = np.arange(int(0.45 * SR)) / SR
    ph = 2 * np.pi * np.cumsum(48 + 110 * np.exp(-tk / 0.025)) / SR
    k = np.sin(ph) * np.exp(-tk / 0.16) + 0.25 * np.sin(2 * ph * 0.5 + 0.5) * np.exp(-tk / 0.05)
    k += 0.18 * np.exp(-tk / 0.003) * np.sin(2 * np.pi * 2500 * tk)   # beater click
    return k * 0.9

def kick_track():
    buf = np.zeros((N, 2)); k = make_kick()
    for b in range(int(SECS / SPB)): place(buf, k, b * SPB)
    return buf

def drums_track(rng):
    buf = kick_track()
    ts = np.arange(int(0.22 * SR)) / SR
    snare = 0.5 * lowpass(rng.standard_normal(len(ts)), 9000) * np.exp(-ts / 0.06) + 0.45 * np.sin(2 * np.pi * 185 * ts) * np.exp(-ts / 0.07)
    th = np.arange(int(0.06 * SR)) / SR
    hat = sosfilt(butter(2, 7000, btype="high", fs=SR, output="sos"), rng.standard_normal(len(th))) * np.exp(-th / 0.014) * 0.22
    for b in range(int(SECS / SPB)):
        if b % 2 == 1: place(buf, snare, b * SPB)
        for h in range(2): place(buf, hat * (1.0 if h == 0 else 0.7), b * SPB + h * SPB / 2, 0.38 if h == 0 else 0.62)
    return buf

CHORDS = [(220.0, 261.63, 329.63, 440.0), (174.61, 220.0, 261.63, 349.23), (196.0, 261.63, 329.63, 392.0), (196.0, 246.94, 293.66, 392.0)]   # Am F C G
ROOTS = [55.0, 87.31, 65.41, 98.0]

def pad_track():
    pad = np.zeros((N, 2)); span = 4 * SPB * 2   # 2 bars per chord
    for c in range(int(SECS / span)):
        s = int(c * span * SR); e = min(N, s + int((span + 0.35) * SR)); tt = np.arange(e - s) / SR
        env = np.minimum(1, tt / 0.25) * np.minimum(1, np.maximum(0, (span + 0.35 - tt)) / 0.35)
        for f in CHORDS[c % 4]:
            for det, side in ((0.9965, 0), (1.0035, 1)):
                pad[s:e, side] += saw(np.full(e - s, f * det)) * env * 0.09
    pad = lowpass(pad, 3200); pad = bandpass(pad, 200, 3200)
    return pad

def bass_track():
    buf = np.zeros((N, 2)); pat = [0.0, 1.5, 2.5, 3.0]   # beats within a bar
    for bar in range(int(SECS / (4 * SPB))):
        root = ROOTS[(bar // 2) % 4]
        for pb in pat:
            at = bar * 4 * SPB + pb * SPB; dur = 0.34 if pb != 3.0 else 0.2
            tn = np.arange(int(dur * SR) + int(0.05 * SR)) / SR
            env = np.minimum(1, tn / 0.006) * np.exp(-np.maximum(0, tn - dur) / 0.015) * np.exp(-tn / 0.5)
            sub = np.sin(2 * np.pi * root * tn); mid = lowpass(saw(np.full(len(tn), root)), 420) * 0.55 + 0.25 * np.sin(2 * np.pi * 2 * root * tn)
            place(buf, (sub * 0.9 + mid) * env, at, 0.5)
    return buf

VOW = {"a": (800, 1200, 2800, 3500), "e": (500, 1900, 2600, 3300), "o": (450, 850, 2700, 3400), "i": (320, 2300, 3000, 3600)}
def vocal_track(rng):
    notes = [(0.0, 440.0, "a", 0.9), (1.0, 392.0, "o", 0.5), (1.5, 329.63, "e", 1.0), (3.0, 349.23, "i", 0.8),   # one phrase = 4 s, rests between
             (4.0, 329.63, "a", 0.6), (4.75, 293.66, "o", 0.5), (5.25, 261.63, "a", 1.2), (7.0, 220.0, "e", 0.8)]
    f0 = np.full(N, 220.0); gate = np.zeros(N); vw = {k: np.zeros(N) for k in VOW}
    for (at, f, v, d) in [(a + 4.0 * r, f, v, d) for r in range(4) for (a, f, v, d) in notes if a + 4.0 * r < 15.5]:
        s = int((0.3 + at * 0.97) * SR); e = min(N, s + int(d * SR))
        if s >= N: continue
        f0[s:e] = f; gate[s:e] = 1; vw[v][s:e] = 1
    f0 = f0 * (1 + 0.012 * np.sin(2 * np.pi * 5.6 * T))
    src = lowpass(saw(f0), 6000, 1)
    a_ = 1 - np.exp(-1 / (0.04 * SR)); r_ = 1 - np.exp(-1 / (0.12 * SR)); env = np.zeros(N); st = 0.0
    for k in range(N):
        st += (gate[k] - st) * (a_ if gate[k] > st else r_); env[k] = st
    sm = 1 - np.exp(-1 / (0.06 * SR)); out = np.zeros(N); wsum = np.zeros(N)
    for v, fm in VOW.items():
        w = lfilter([sm], [1, -(1 - sm)], vw[v]); y = np.zeros(N)
        for f, g, q in zip(fm, (14, 12, 8, 5), (7, 8, 9, 9)): y += lfilter(*rbj_peak(f, q, g), src)
        out += y * w; wsum += w
    out = out / np.maximum(wsum, 1e-3) * env
    out += 0.05 * bandpass(rng.standard_normal(N), 2500, 6500) * env   # breath
    return bandpass(out, 300, 4200)

def set_level(x, target_db, active_thr=None):
    """Scale so the RMS over the active part (or all) reads target_db; return the scaled array."""
    mask = None
    if active_thr is not None: mask = np.abs(x).max(axis=1) > active_thr
    return x * amp(target_db - rms_db(x, mask))

def koro_dry_stem():
    root = os.path.abspath(os.path.join(HERE, "..", "..", "subverselab-koro"))
    return [p for p in glob.glob(os.path.join(root, "**", "*.wav"), recursive=True) if "dry" in os.path.basename(p).lower()]

def main():
    os.makedirs(OUT, exist_ok=True); rng = np.random.default_rng(9)
    kick = kick_track(); drums = drums_track(rng); pad = pad_track(); bass = bass_track()
    # beat: drums about 5 dB over the pad, whole beat about -20 dBFS RMS (headroom for the level-matched pack)
    drums_l = set_level(drums, -23); pad_l = set_level(pad, -22)
    beat = drums_l + pad_l; beat = set_level(beat, -20)
    bass = set_level(bass, -21); kick = set_level(kick, -20)
    voc = vocal_track(rng); voc = np.stack([voc, voc], axis=1); voc = set_level(voc, -21, active_thr=0.01)
    stems = koro_dry_stem()
    if stems: print("KORO dry stems found but not used by this script:", stems)
    res = np.zeros_like(beat)
    for ch in (0, 1):
        x = beat[:, ch]
        for f in (340.0, 2800.0): x = lfilter(*rbj_peak(f, 10.0, 10.0), x)
        res[:, ch] = x
    res = set_level(res, -20)
    for name, x in (("beat", beat), ("bass", bass), ("kick", kick), ("vocal", voc), ("resonant", res)):
        pk = 20 * np.log10(np.max(np.abs(x)) + 1e-12); write_wav24(os.path.join(OUT, name + ".wav"), x)
        print(f"{name}.wav: peak {pk:.1f} dBFS, RMS {rms_db(x):.1f} dBFS, {len(x) / SR:.1f} s")

if __name__ == "__main__":
    main()
