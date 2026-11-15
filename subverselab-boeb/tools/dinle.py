#!/usr/local/bin/python3
"""BOE-B listening pack (P2): the owner's own takes, before / after, at equal loudness.

  dinle.py      -> ../prototypes/DINLE/23_BOEB/

Per take (the five track-4 takes of "mali demo", read in place). The take itself, uncut: from its first to its last
second within 20 dB of the loudest (measure.py's spliced test material is not used here: every clip had a splice).
  A  original take
  B  BOE-B, LISTEN 8 s, LIFT 6, exciter held off (measurement switch testoff=64)
  C  BOE-B, LISTEN 8 s, LIFT 6, exciter on (as the product runs)
  ABC  the three back to back, 0.8 s apart
The clip is the CLIP_S seconds with the most voice in them; the engine starts 2 s before it (settling) and LISTEN hears
those 2 s and the clip's first 6 s, as when LISTEN is pressed while the voice plays. Every file is set to TARGET_LUFS (BS.1770-4, gated), so louder never wins. No beat: the vocal alone, the rider evening it towards its own level.
"""
import json, os, subprocess, sys
import numpy as np
import soundfile as sf
from scipy import signal

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.argv = sys.argv[:1]
import measure as M

OUT = os.path.join(HERE, "..", "..", "prototypes", "DINLE", "23_BOEB")
TARGET_LUFS = -20.0
CLIP_S = 12
SR = M.SR
# measure.py keys -> the take's own name in the Live project (track 4 of "mali demo", time stamp where two share a number)
LABELS = {"take_00012241": "4-Audio 0001", "take_00022241": "4-Audio 0002 (22.41)", "take_00022242": "4-Audio 0002 (22.42)",
          "take_00032243": "4-Audio 0003", "take_00042244": "4-Audio 0004"}

def true_peak_db(x):
    up = signal.resample_poly(x, 4, 1, axis=0)
    return float(20 * np.log10(np.abs(up).max() + 1e-12))

def at_lufs(x):
    y = x * 10 ** ((TARGET_LUFS - M.lufs(x)) / 20)
    f = int(0.01 * SR)                       # 10 ms fades: clips start and end inside the take
    ramp = np.linspace(0, 1, f)[:, None]
    y[:f] *= ramp; y[-f:] *= ramp[::-1]
    return y.astype(np.float32)

def main():
    os.makedirs(OUT, exist_ok=True)
    rows = []
    takes = {}
    for fn in M.OWNER_TAKES:
        x, sr = sf.read(os.path.join(M.OWNER_DIR, fn), dtype="float32", always_2d=True)
        x = x.mean(axis=1)
        sec = [10 * np.log10((x[i * sr : (i + 1) * sr] ** 2).mean() + 1e-20) for i in range(len(x) // sr)]
        keep = [i for i, v in enumerate(sec) if v > max(sec) - 20]
        takes["take_" + fn[8:12] + fn[-11:-7]] = (x[keep[0] * sr : (keep[-1] + 1) * sr], np.array(sec[keep[0] : keep[-1] + 1]))
    for i, (name, (v, sec)) in enumerate(takes.items(), 1):
        n_s = len(v) // SR
        starts = range(2, max(3, n_s - CLIP_S + 1))
        t0 = max(starts, key=lambda t: float(np.sum(10 ** (sec[t : t + CLIP_S] / 10))))
        # As in use: LISTEN is pressed while the voice plays. The engine starts 2 s before the clip (settling) and
        # LISTEN hears those 2 s and the clip's first 6 s.
        w = v[(t0 - 2) * SR :]
        b, rb = M.run(w, listen=8, report=True, lift=6, testoff=64)
        c, rc = M.run(w, listen=8, report=True, lift=6)
        n = min(CLIP_S * SR, len(v) - t0 * SR)
        a = np.stack([v, v], 1)[t0 * SR : t0 * SR + n]
        files = {"A": at_lufs(a), "B": at_lufs(b[2 * SR : 2 * SR + n]), "C": at_lufs(c[2 * SR : 2 * SR + n])}
        seg = slice(0, n)
        label = LABELS.get(name, name)
        names = {"A": f"{i:02d}A_{label} orijinal.wav",
                 "B": f"{i:02d}B_{label} BOE-B LIFT 6, exciter kapali.wav",
                 "C": f"{i:02d}C_{label} BOE-B LIFT 6, exciter acik.wav"}
        gap = np.zeros((int(0.8 * SR), 2), np.float32)
        abc = np.concatenate([files["A"], gap, files["B"], gap, files["C"]])
        for k, x in files.items():
            sf.write(os.path.join(OUT, names[k]), x, SR, subtype="PCM_24")
        sf.write(os.path.join(OUT, f"{i:02d}_{label} A-B-C arka arkaya.wav"), abc, SR, subtype="PCM_24")
        row = {"take": label, "clip_from_s": t0, "clip_s": round((seg.stop - seg.start) / SR, 1),
               "cut_hz": rc["cutMeasured"], "exciter_db": rc["excite"]}
        for k, x in files.items():
            m = M.analyse(x)
            row[k] = {"lufs": round(M.lufs(x), 2), "tp": round(true_peak_db(x), 2), "mud": round(m["mud"], 1),
                      "pres": round(m["pres"], 1), "air": round(m["air"], 1), "spread": round(m["spread"], 1),
                      "hf": round(M.hf_content(x), 0)}
        rows.append(row)
        print(json.dumps(row))
    json.dump(rows, open(os.path.join(OUT, "olcum.json"), "w"), indent=1)

if __name__ == "__main__":
    main()
