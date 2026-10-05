"""First repeat (dB re dry) against MIX per voicing, FEEDBACK knob 0.30, 1/8 dotted, TONE 4500 (the old sync index 2 = 1/8 dotted still
applies to the render binary built before the v2 sync list; the new list keeps 2)."""
import json
from lab import *
sr = 44100; T = 0.375
out = {}
for mood in (0, 1, 2, 3, 4, 5):
    for mix in (0.05, 0.1, 0.2, 0.3, 0.4, 0.5, 0.7, 1.0):
        x, t0, bn = pink_burst(sr, 4.0, ms=120)
        y = render(sr, x, dict(mood=mood, sync=2, tone=4500, wear=0.25, mix=mix, spread=0.3, fb=0.30), BPM, "m")
        ref = rms(x[t0:t0 + bn])
        ser = repeat_series(y, sr, t0, bn, T, ref, n_rep=3)
        out["%d_%.2f" % (mood, mix)] = round(ser[0], 2)
        print(mood, mix, round(ser[0], 2), flush=True)
json.dump(out, open("ring_calib_mix.json", "w"), indent=1)
