"""Calibrates the ring model for the HEADS keys (Source/RingModel.h, kFirstHeads / kSlopeHeads).
Same burst, same measurement as calib_rings.py, but with HEADS forced to 1, 2 and 3 for every MOOD voicing (TONE 4500, SPREAD 0.30).
The slope is the per-repeat slope that reproduces where the repeats cross -60 dB (what the editor reads as DECAY), or the line fit when they never do.
Writes ring_calib_heads.json (run from tools/)."""
import json
import numpy as np
from lab import *
out = {}
sr = 44100
T = 0.375
for mood in range(7):
    for heads in (1, 2, 3):
        for fb in (0.15, 0.30, 0.45, 0.60, 0.69):
            x, t0, bn = pink_burst(sr, 14.0, ms=120)
            p = dict(mood=mood, sync=2, tone=4500, wear=0.25, mix=0.30, spread=0.30, fb=fb, heads=heads)
            y = render(sr, x, p, BPM, "ch")
            ref = rms(x[t0:t0 + bn])
            ser = repeat_series(y, sr, t0, bn, T, ref, n_rep=30)
            cross = t60_from_series(ser, T) / T          # repeat count (float) where the series crosses -60 dB
            if np.isfinite(cross) and cross > 1.0:
                slope = (-60.0 - ser[0]) / (cross - 1.0)
                how = "cross"
            else:
                usable = [v for v in ser if v > -55]
                slope = float(np.polyfit(np.arange(len(usable)), usable, 1)[0]) if len(usable) >= 3 else -30.0
                how = "fit"
            key = "%d_%d_%.2f" % (mood, heads, fb)
            out[key] = dict(first=round(ser[0], 2), slope=round(float(slope), 2), how=how)
            print(key, out[key], flush=True)
json.dump(out, open("ring_calib_heads.json", "w"), indent=1)
