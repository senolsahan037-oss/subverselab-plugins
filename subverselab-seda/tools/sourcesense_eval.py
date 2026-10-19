#!/usr/bin/env python3
"""Evaluate SourceSense v0 on a labelled clip set (tools/sourcesense_set.py).

  sourcesense_eval.py --dir DIR            confusion matrix, accuracy, every miss
  sourcesense_eval.py --dir DIR --features per-class feature ranges (what the rules were read from) and every clip's vector
  sourcesense_eval.py --make A --dir DIR   generate set A into DIR first

Runs build/sensecli (the same header the plug-ins include) on the first 10 s of every clip.
"""
import argparse, json, os, subprocess, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
CLI = os.environ.get("SENSECLI", os.path.join(HERE, "..", "build", "sensecli"))
sys.path.insert(0, HERE)
import sourcesense_set as ss

CLASSES = ss.CLASSES
FEATS = ["centroid", "flatness", "lowShare", "onsetRate", "crest", "harmonicity", "width", "duty", "activeSec"]


def run_dir(d, seconds=10.0):
    labels = json.load(open(os.path.join(d, "labels.json"))); rows = []
    for name in sorted(labels):
        out = subprocess.run([CLI, os.path.join(d, name), "--seconds", str(seconds)], check=True, capture_output=True, text=True).stdout.strip().split(",")
        rows.append(dict(name=name, truth=labels[name]["cls"], pred=out[0], conf=float(out[1]), ready=int(out[2]), f=dict(zip(FEATS, map(float, out[3:12]))), score=dict(zip(CLASSES, map(float, out[12:19])))))
    return rows


def confusion(rows):
    """Text confusion matrix (truth = rows, prediction = columns), accuracy, and the list of misses."""
    idx = {c: i for i, c in enumerate(CLASSES)}; m = np.zeros((len(CLASSES), len(CLASSES)), int)
    for r in rows: m[idx[r["truth"]], idx[r["pred"]]] += 1
    w = 8; lines = ["truth \\ pred".ljust(14) + "".join(c.rjust(w) for c in CLASSES) + "   n"]
    for i, c in enumerate(CLASSES): lines.append(c.ljust(14) + "".join(str(v).rjust(w) for v in m[i]) + str(m[i].sum()).rjust(4))
    ok = int(np.trace(m)); acc = 100.0 * ok / max(1, len(rows))
    misses = [r for r in rows if r["truth"] != r["pred"]]
    return "\n".join(lines), acc, ok, misses


def fmt_miss(r):
    f = r["f"]
    return (f"  {r['name']}: truth {r['truth']}, said {r['pred']} (conf {r['conf']:.2f}); centroid {f['centroid']:.0f} Hz, flat {f['flatness']:.4f}, low {f['lowShare']:.2f}, "
            f"onsets {f['onsetRate']:.2f}/s, crest {f['crest']:.1f} dB, harm {f['harmonicity']:.2f}, width {f['width']:.3f}, duty {f['duty']:.2f}; "
            + "scores " + " ".join(f"{k} {v:.2f}" for k, v in sorted(r["score"].items(), key=lambda kv: -kv[1])[:3]))


if __name__ == "__main__":
    ap = argparse.ArgumentParser(); ap.add_argument("--dir", required=True); ap.add_argument("--make"); ap.add_argument("--features", action="store_true")
    a = ap.parse_args()
    if a.make: ss.make_set(a.dir, a.make)
    rows = run_dir(a.dir)
    if a.features:
        print("class      " + "".join(f"{k:>26}" for k in FEATS[:8]))
        for c in CLASSES:
            sel = [r for r in rows if r["truth"] == c]
            print(c.ljust(10) + "".join(f"{min(r['f'][k] for r in sel):>9.3f}..{max(r['f'][k] for r in sel):<7.3f}  " for k in FEATS[:8]))
        print()
        for r in rows: print(f"{r['name']:<18}" + " ".join(f"{r['f'][k]:>9.3f}" for k in FEATS[:8]))
        print()
    text, acc, ok, misses = confusion(rows)
    print(text)
    print(f"\naccuracy {acc:.1f} % ({ok} of {len(rows)})")
    print(f"misses: {len(misses)}")
    for r in misses: print(fmt_miss(r))
