#!/usr/bin/env python3
"""Fit the SourceSense v0 class table (mean and sigma per class and feature, in the transformed feature space of the header) from a dev set.

  sourcesense_fit.py --dir DIR [--dir DIR2]   print the C++ table (several dirs are pooled)
  sourcesense_fit.py --dir DIR --write    replace the BEGIN FIT .. END FIT block in plugin-common/SourceSense/SourceSense.h

DIR must be a set made by sourcesense_set.py (set A is the development set). The clips are run through build/sensecli with the rules of the
header as they are; only the raw features are used, so the order does not matter. Transform: see "Feature space" in SourceSense.h.
"""
import argparse, os, re, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import sourcesense_eval as ev

HDR = os.path.join(HERE, "..", "..", "subverselab-plugin-common", "SourceSense", "SourceSense.h")
CLASS7 = ev.CLASSES[:7]

def transform(f):
    return [np.log2(max(1.0, f["centroid"])), np.log10(f["flatness"] + 1e-5), f["lowShare"], np.log2(f["onsetRate"] + 0.25), f["crest"], f["harmonicity"], np.log10(f["width"] + 0.003), f["duty"]]

def table(rows):
    out = ["constexpr MuSigma kClassFit[7][kNumFeatures] = {"]
    for c in CLASS7:
        X = np.array([transform(r["f"]) for r in rows if r["truth"] == c])
        mu, sd = X.mean(0), X.std(0, ddof=1)
        out.append("    { " + ", ".join(f"{{ {m:.4f}, {s:.4f} }}" for m, s in zip(mu, sd)) + f" }},   // {c}, n = {len(X)}")
    out.append("};")
    return "\n".join(out)

if __name__ == "__main__":
    ap = argparse.ArgumentParser(); ap.add_argument("--dir", required=True, action="append", help="a set directory; repeat to pool several sets"); ap.add_argument("--write", action="store_true"); a = ap.parse_args()
    rows = [r for d in a.dir for r in ev.run_dir(d)]; t = table(rows); print(t)
    if a.write:
        h = open(HDR).read()
        h = re.sub(r"(// BEGIN FIT\nstruct MuSigma \{ double mu, sigma; \};\n).*?(// END FIT)", lambda m: m.group(1) + t + "\n" + m.group(2), h, flags=re.S)
        open(HDR, "w").write(h); print("written to", HDR)
