#!/usr/local/bin/python3
"""Generate the offline KOR auto-gain table (BS.1770 loudness)."""
import os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
import measure

x = measure.pink(20.0, rms_db=-18.0, seed=1, channels=2)
rows = []
for c in (0, .25, .5, .75, 1):
    base = measure.loudness(measure.run(x, drive=0, character=c, ceiling=-0.3, tone=0, auto=0))
    row = []
    for d in range(0, 25, 2):
        level = measure.loudness(measure.run(x, drive=d, character=c, ceiling=-0.3, tone=0, auto=0))
        row.append(base - level)
    rows.append(row)
print("static constexpr float kAutoGainDb[5][13] = {")
for row in rows:
    print("    { " + ", ".join(f"{v:.6f}f" for v in row) + " },")
print("};")
