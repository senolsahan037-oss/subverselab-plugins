#!/usr/local/bin/python3
"""Regression lock for KorEngine.h (reviewer-owned; do not edit).

  regress.py --save   write tests/ref/*.f32 from the current build/korraw (reviewer only)
  regress.py          compare the current build/korraw against tests/ref; exit 1 if any max |diff| > TOL
"""
import os, sys, hashlib
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import measure as m

TOL = 2e-6
REF = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests", "ref")
CASES = [
    ("drums_default", "drums", {}),
    ("drums_hot", "drums", dict(drive=18, character=1.0, tone=3, ceiling=-1, mix=1)),
    ("drums_soft_mix", "drums", dict(drive=9, character=0.0, tone=-4, ceiling=-3, mix=0.6)),
    ("sweep_hot", "sweep", dict(drive=24, character=0.5, ceiling=-0.3)),
    ("pink_glide", "pink", dict(drive="0:24", character="0:1", tone="-6:6")),
]

def signal(name):
    if name == "drums": d = m.drums(); return np.stack([d, d], 1).astype(np.float32)
    if name == "sweep": s = m.sweep(); return np.stack([s, s], 1).astype(np.float32)
    return m.pink(10.0, seed=7).astype(np.float32)

def main():
    save = "--save" in sys.argv
    os.makedirs(REF, exist_ok=True)
    bad = 0
    for tag, sig, params in CASES:
        y = m.run(signal(sig), **params).astype(np.float32)
        path = os.path.join(REF, tag + ".f32")
        if save:
            y.tofile(path); print(f"saved {tag} {hashlib.md5(y.tobytes()).hexdigest()}"); continue
        ref = np.fromfile(path, dtype=np.float32).reshape(y.shape)
        d = float(np.max(np.abs(y - ref)))
        ok = d <= TOL; bad += (not ok)
        print(f"{tag:16s} max|diff| {d:.2e}  {'PASS' if ok else 'FAIL'} (tol {TOL:g})")
    if not save: print(f"regress failures: {bad}"); sys.exit(1 if bad else 0)

if __name__ == "__main__":
    main()
