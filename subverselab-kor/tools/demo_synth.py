#!/usr/local/bin/python3
"""Synth demo material for the ALEV and TABAN videos (owner rule: demo sounds are synths). Deterministic.

  demo_synth.py alev  <out.wav>   # 32 bars at 120 bpm: synth kick/snare/hats, saw bass, detuned saw pad (A minor)
  demo_synth.py taban <out.wav>   # 16 bars of a pluck melody whose phrases alternate loud and quiet, long tails
48 kHz stereo float, peak -6 dBFS (ALEV) / -3 dBFS (TABAN).
"""
import sys, numpy as np, soundfile as sf
SR = 48000; BPM = 120.0; BEAT = 60.0 / BPM
rng = np.random.default_rng(7)

def saw(f, t):  # band-limited enough for a demo: polyBLEP-free additive up to 8 kHz
    y = np.zeros_like(t)
    for k in range(1, int(8000 / max(f, 20)) + 1):
        y += ((-1) ** (k + 1)) * np.sin(2 * np.pi * k * f * t) / k
    return y * (2 / np.pi)

def lp(x, fc):
    """One-pole low-pass; fc may be a number or a per-sample array (a filter envelope)."""
    if np.ndim(fc) == 0:
        from scipy.signal import lfilter
        a = np.exp(-2 * np.pi * fc / SR); return lfilter([1 - a], [1, -a], x)
    a = np.exp(-2 * np.pi * np.asarray(fc) / SR); y = np.empty_like(x); s = 0.0
    for i in range(len(x)): s = (1 - a[i]) * x[i] + a[i] * s; y[i] = s
    return y

def note_hz(n): return 440.0 * 2 ** ((n - 69) / 12)

def alev(bars=32):
    n = int(bars * 4 * BEAT * SR); L = np.zeros(n); R = np.zeros(n)
    chords = [[57, 60, 64], [53, 57, 60], [48, 52, 55], [55, 59, 62]]   # Am F C G
    bass = [45, 41, 36, 43]
    bar = int(4 * BEAT * SR)
    for b in range(bars):
        s0 = b * bar; t = np.arange(bar) / SR
        ch = chords[b % 4]
        pad = sum(saw(note_hz(m) * d, t) for m in ch for d in (0.997, 1.003)) / 6
        pad = lp(pad, 1800) * 0.22
        L[s0:s0 + bar] += pad; R[s0:s0 + bar] += np.roll(pad, 240)
        for e in range(8):   # eighth-note bass
            st = s0 + int(e * BEAT / 2 * SR); ln = int(BEAT / 2 * SR * 0.9); tt = np.arange(ln) / SR
            bs = lp(saw(note_hz(bass[b % 4]), tt), 600 + 900 * np.exp(-tt / 0.05)) * np.exp(-tt / 0.25) * 0.42
            L[st:st + ln] += bs; R[st:st + ln] += bs
        for q in range(4):
            st = s0 + int(q * BEAT * SR); ln = int(0.45 * SR); tt = np.arange(ln) / SR
            kick = np.sin(2 * np.pi * (48 * tt + 90 * 0.04 * (1 - np.exp(-tt / 0.04)))) * np.exp(-tt / 0.22) * 0.9
            L[st:st + ln] += kick; R[st:st + ln] += kick
            if q % 2 == 1:
                ln2 = int(0.25 * SR); t2 = np.arange(ln2) / SR
                sn = (rng.standard_normal(ln2) * 0.5 + np.sin(2 * np.pi * 190 * t2) * 0.4) * np.exp(-t2 / 0.09) * 0.5
                L[st:st + ln2] += sn; R[st:st + ln2] += sn
            for h in range(2):
                sh = st + int(h * BEAT / 2 * SR); ln3 = int(0.06 * SR); nz = np.diff(rng.standard_normal(ln3 + 1))
                hat = nz * np.exp(-np.arange(ln3) / SR / 0.015) * 0.10
                L[sh:sh + ln3] += hat * 0.8; R[sh:sh + ln3] += hat
    y = np.stack([L, R], 1); return y / np.max(np.abs(y)) * 10 ** (-6 / 20)

def taban(bars=16):
    n = int(bars * 4 * BEAT * SR) + SR; L = np.zeros(n); R = np.zeros(n)
    mel = [69, 72, 76, 74, 72, 69, 67, 69]
    for b in range(bars):
        loud = (b // 2) % 2 == 0
        g = 0.8 if loud else 0.8 * 10 ** (-18 / 20)          # quiet phrases 18 dB down (inside TABAN's 24 dB lift)
        for e in range(8):
            st = int((b * 4 + e / 2) * BEAT * SR); ln = int(1.2 * SR); tt = np.arange(ln) / SR
            m = mel[(b * 8 + e) % 8] - (0 if b % 4 < 2 else 5)
            pl = lp(saw(note_hz(m), tt), 900 + 3000 * np.exp(-tt / 0.08)) * np.exp(-tt / 0.35) * g * (1.0 if e % 2 == 0 else 0.6)
            L[st:st + ln] += pl; R[st:st + ln] += np.roll(pl, 180)
    y = np.stack([L, R], 1); return y / np.max(np.abs(y)) * 10 ** (-3 / 20)

if __name__ == "__main__":
    kind, out = sys.argv[1], sys.argv[2]
    y = alev() if kind == "alev" else taban()
    sf.write(out, y.astype(np.float32), SR, subtype="FLOAT"); print(out, y.shape[0] / SR, "s")
