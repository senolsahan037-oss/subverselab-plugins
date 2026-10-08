#!/usr/local/bin/python3
"""Per-band loudness trim for KorMultiband.h (kBandTrimDb). Pink -18 dBFS RMS, 20 s, seed 1; one band at heat h,
the others 0; trim = loudness(all 0) - loudness(band at h), on top of the table already compiled in (run twice to converge)."""
import os, sys, re
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
os.environ["KORRAW"] = os.path.join(HERE, "..", "build", "korraw_mb")
sys.path.insert(0, HERE)
import measure as m
m.RAW = os.environ["KORRAW"]
src = open(os.path.join(HERE, "..", "Source", "KorMultiband.h")).read()
cur = [[float(v.strip().rstrip("f")) for v in row.split(",") if v.strip()] for row in re.findall(r"\{ ([^{}]*) \}", src.split("kBandTrimDb[3][kTrimN] = {")[1].split("};")[0])]
x = m.pink(20.0, seed=1)
base = m.loudness(m.run(x, low=0, mid=0, high=0))
rows = []
for b, name in enumerate(("low", "mid", "high")):
    row = []
    for i in range(11):
        h = i / 10
        p = dict(low=0, mid=0, high=0); p[name] = h
        row.append(cur[b][i] + (base - m.loudness(m.run(x, **p))))
    rows.append(row)
print("static constexpr float kBandTrimDb[3][kTrimN] = {")
for r in rows: print("    { " + ", ".join(f"{v:.3f}f" for v in r) + " },")
print("};")
