#!/usr/bin/env python3
"""TARGET audit (11 Oct, owner: "kick body sectim, yarim cut etti; baska hangi hatalar var bilmiyorum").

Renders all 20 TARGET entries through dyneqraw on the demo material, AMOUNT 12 dB, and measures what the output really lost:
the long-term spectrum of the last 5 s (the memory is full and the cut static by then), output / input per 1/12 octave.

Per entry it prints:
  peak      the deepest cut and where
  full oct  how many octaves are cut by at least AMOUNT - 3 dB (a bell: a narrow tip; the part should be covered)
  half oct  how many octaves are cut by at least AMOUNT / 2
  in-range  the mean cut over the part's search range (SPEC 8b), and the share of it cut by >= AMOUNT - 3 dB
  guard     the cut at the protected neighbour (KICK / BASS BODY: the SUB at 55 Hz; SUB: the BODY at 120 Hz), dB

    target_audit.py [dyneqraw binary] [--amount 12] [--csv out.csv]
"""
import sys, subprocess, tempfile, os, argparse
import numpy as np, soundfile as sf

ROOT = os.path.dirname (os.path.dirname (os.path.abspath (__file__)))
MAT = os.path.join (ROOT, "tools", "demo_material")

# entry, dyneqraw inst, part, material, search range (Hz) or edge, guard frequency
ENTRIES = [
    ("VOCAL - BOX", "vocal", "box", "vocal.wav", (350, 900), None),
    ("VOCAL - NASAL", "vocal", "nasal", "vocal.wav", (800, 1600), None),
    ("VOCAL - HARSH", "vocal", "harsh", "vocal.wav", (2000, 4500), None),
    ("VOCAL - S", "vocal", "s", "vocal.wav", (5000, 10000), None),
    ("VOCAL - REVERB-READY", "vocal", "box,reverb=1", "vocal.wav", "rev", None),
    ("BASS - SUB", "bass", "sub", "bass.wav", "below", 150),
    ("BASS - BODY", "bass", "body", "bass.wav", (80, 250), 50),
    ("BASS - MUD", "bass", "mud", "bass.wav", (200, 400), None),
    ("BASS - TOP", "bass", "top", "bass.wav", "above", 150),
    ("DRUMS - KICK BODY", "drums", "kickbody", "beat.wav", (60, 160), 50),
    ("DRUMS - SNARE BODY", "drums", "snarebody", "beat.wav", (140, 300), None),
    ("DRUMS - HATS SHIMMER", "drums", "hatsshimmer", "beat.wav", (6000, 14000), None),
    ("KICK - SUB", "kick", "sub", "kick.wav", "below", 120),
    ("KICK - BODY", "kick", "body", "kick.wav", (60, 160), 50),
    ("KICK - MUD", "kick", "mud", "kick.wav", (100, 250), None),
    ("KICK - BOX", "kick", "box", "kick.wav", (200, 500), None),
    ("KICK - CLICK", "kick", "click", "kick.wav", "above", 120),
    ("SNARE - BODY", "snare", "body", "beat.wav", (140, 300), None),
    ("SNARE - BOX", "snare", "box", "beat.wav", (300, 600), None),
    ("SNARE - CRACK", "snare", "crack", "beat.wav", (900, 3500), None),
    ("SNARE - SNAP/AIR", "snare", "snapair", "beat.wav", "above", None),
    ("SNARE - REVERB-READY", "snare", "body,reverb=1", "beat.wav", "rev", None),
    ("HATS - CHICK", "hats", "chick", "beat.wav", (250, 1000), None),
    ("HATS - BITE", "hats", "bite", "beat.wav", (3000, 6000), None),
    ("HATS - SHIMMER", "hats", "shimmer", "beat.wav", (6000, 14000), None),
]

def mono (p):
    x, sr = sf.read (p, always_2d = True)
    return x[:, 0], sr

def ltas (v, sr, a, b, n = 16384):
    seg = v[a:b]; w = np.hanning (n); S = np.zeros (n // 2 + 1); k = 0
    for i in range (0, len (seg) - n, n // 4): S += np.abs (np.fft.rfft (seg[i:i + n] * w)) ** 2; k += 1
    return S / max (k, 1), np.fft.rfftfreq (n, 1.0 / sr)

def profile (x, y, sr):
    n = min (len (x), len (y)); a = n - 5 * sr
    L0, f = ltas (x, sr, a, n); L1, _ = ltas (y, sr, a, n)
    cen = 20.0 * 2.0 ** (np.arange (0, 12 * np.log2 (16000 / 20.0) + 1) / 12.0)
    cut = []
    for c in cen:
        m = (f > c / 2 ** (1 / 24)) & (f < c * 2 ** (1 / 24))
        e0 = L0[m].sum(); e1 = L1[m].sum()
        cut.append (-10 * np.log10 ((e1 + 1e-30) / (e0 + 1e-30)) if e0 > 1e-14 else 0.0)   # positive = cut
    return cen, np.array (cut)

def main():
    ap = argparse.ArgumentParser(); ap.add_argument ("bin", nargs = "?", default = os.path.join (ROOT, "build", "dyneqraw"))
    ap.add_argument ("--amount", type = float, default = 12.0); ap.add_argument ("--csv"); ap.add_argument ("--only")
    a = ap.parse_args()
    A = a.amount; rows = []
    tmp = tempfile.mkdtemp()
    print (f"TARGET audit, AMOUNT {A:g} dB, {os.path.basename (a.bin)}")
    print (f"{'entry':24s} {'peak dB @ Hz':>16s} {'full oct':>8s} {'half oct':>8s} {'in-range mean':>13s} {'>=A-3':>6s} {'guard':>6s}")
    for name, inst, part, mat, rng, guard in ENTRIES:
        if a.only and a.only.lower() not in name.lower(): continue
        src = os.path.join (MAT, mat); out = os.path.join (tmp, "o.wav")
        spec = f"inst={inst},part={part},amount={A:g}"
        r = subprocess.run ([a.bin, src, src, out, "--depth", "0", "--target", spec], capture_output = True, text = True)
        if r.returncode != 0: print (name, "FAILED", r.stderr[:200]); continue
        x, sr = mono (src); y, _ = mono (out)
        cen, cut = profile (x, y, sr)
        i = int (np.argmax (cut)); full = (cut >= A - 3).sum() / 12.0; half = (cut >= A / 2).sum() / 12.0
        if isinstance (rng, tuple):
            m = (cen >= rng[0]) & (cen <= rng[1]); mean = cut[m].mean(); share = (cut[m] >= A - 3).mean() * 100
        elif rng == "below": m = cen <= 60; mean = cut[m].mean(); share = (cut[m] >= A - 3).mean() * 100
        elif rng == "above": m = cen >= 3000; mean = cut[m].mean(); share = (cut[m] >= A - 3).mean() * 100
        else: m = (cen <= 80) | (cen >= 12000); mean = cut[m].mean(); share = (cut[m] >= A - 3).mean() * 100
        g = float (np.interp (np.log2 (guard), np.log2 (cen), cut)) if guard else float ("nan")
        rows.append ((name, cut[i], cen[i], full, half, mean, share, g))
        print (f"{name:24s} {cut[i]:6.1f} @ {cen[i]:7.0f} {full:8.2f} {half:8.2f} {mean:13.1f} {share:5.0f}% {g:6.1f}")
    if a.csv:
        with open (a.csv, "w") as fh:
            fh.write ("entry,peak_db,peak_hz,full_oct,half_oct,inrange_mean_db,inrange_full_share_pct,guard_db\n")
            for r in rows: fh.write (",".join (str (v) if not isinstance (v, float) else f"{v:.2f}" for v in r) + "\n")

if __name__ == "__main__": main()
