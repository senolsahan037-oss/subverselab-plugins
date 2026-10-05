"""Calibrates the ring model of the editor (Source/RingModel.h) against the real processor.
For each MOOD voicing and FEEDBACK knob position it renders a pink burst (1/8 dotted, 120 bpm), measures the first repeat re the dry
and the per-repeat slope in dB, at two TONE values. Run before changing sync indices (uses the render CLI sync 2 = 1/8 dotted)."""
import json, sys
import numpy as np
from lab import *
out = {}
sr = 44100
T = 0.375
for mood in range(7):
    for tone in (4500, 2200):
        for fb in (0.15, 0.30, 0.45, 0.60, 0.69):
            x, t0, bn = pink_burst(sr, 14.0, ms=120)
            p = dict(mood=mood, sync=2, tone=tone, wear=0.25, mix=0.30, spread=0.30, fb=fb)
            y = render(sr, x, p, BPM, "c")
            ref = rms(x[t0:t0 + bn])
            ser = repeat_series(y, sr, t0, bn, T, ref, n_rep=30)
            usable = [v for v in ser if v > -55]
            if len(usable) >= 3:
                ks = np.arange(len(usable)); slope = float(np.polyfit(ks, usable, 1)[0])
            else:
                slope = float((usable[-1] - usable[0]) / max(1, len(usable) - 1)) if len(usable) > 1 else -30.0
            out["%d_%d_%.2f" % (mood, tone, fb)] = dict(first=round(ser[0], 2), slope=round(slope, 2), n=len(usable))
            print(mood, tone, fb, out["%d_%d_%.2f" % (mood, tone, fb)], flush=True)
json.dump(out, open("ring_calib.json", "w"), indent=1)
