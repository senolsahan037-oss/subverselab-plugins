#!/usr/local/bin/python3
"""BOE-B strength pack: how far does the change have to go before an untrained ear hears it at once?

  dinle_guc.py  -> ../prototypes/DINLE/24_BOEB_GUC/

Owner (24 Oct): "eğitimsiz kulaklar insert sonrası vokaldeki değişimi bariz farketmeli ... kanala tak işi bitir".
Same takes, clips and LISTEN as dinle.py (P2). Per take:
  A  original
  C  LIFT 6 (what the P2 pack's C was, with today's engine)
  D  LIFT 10
  E  LIFT 10, mood PUSH
  ACDE the four back to back, 0.8 s apart
Every file at TARGET_LUFS, so louder never wins. Exciter on (as the product runs).
"""
import json, os
import numpy as np
import soundfile as sf

import dinle as P
M = P.M

OUT = os.environ.get("BOEB_PACK_OUT", os.path.join(P.HERE, "..", "..", "prototypes", "DINLE", "24_BOEB_GUC"))
VARIANTS = {"C": ("LIFT 6", {"lift": 6}), "D": ("LIFT 10", {"lift": 10}), "E": ("LIFT 10 PUSH", {"lift": 10, "mood": 1})}

def main():
    os.makedirs(OUT, exist_ok=True)
    rows = []
    for i, fn in enumerate(M.OWNER_TAKES, 1):
        x, sr = sf.read(os.path.join(M.OWNER_DIR, fn), dtype="float32", always_2d=True)
        x = x.mean(axis=1)
        sec = [10 * np.log10((x[k * sr : (k + 1) * sr] ** 2).mean() + 1e-20) for k in range(len(x) // sr)]
        keep = [k for k, v in enumerate(sec) if v > max(sec) - 20]
        v, sec = x[keep[0] * sr : (keep[-1] + 1) * sr], np.array(sec[keep[0] : keep[-1] + 1])
        n_s = len(v) // P.SR
        t0 = max(range(2, max(3, n_s - P.CLIP_S + 1)), key=lambda t: float(np.sum(10 ** (sec[t : t + P.CLIP_S] / 10))))
        w = v[(t0 - 2) * P.SR :]
        n = min(P.CLIP_S * P.SR, len(v) - t0 * P.SR)
        files = {"A": P.at_lufs(np.stack([v, v], 1)[t0 * P.SR : t0 * P.SR + n])}
        reps = {}
        for k, (_, params) in VARIANTS.items():
            y, reps[k] = M.run(w, listen=8, report=True, **params)
            files[k] = P.at_lufs(y[2 * P.SR : 2 * P.SR + n])
        label = P.LABELS.get("take_" + fn[8:12] + fn[-11:-7], fn)
        names = {"A": f"{i:02d}A_{label} orijinal.wav"}
        names.update({k: f"{i:02d}{k}_{label} BOE-B {lab}.wav" for k, (lab, _) in VARIANTS.items()})
        gap = np.zeros((int(0.8 * P.SR), 2), np.float32)
        seq = []
        for k in ("A", "C", "D", "E"): seq += [files[k], gap]
        for k, y in files.items(): sf.write(os.path.join(OUT, names[k]), y, P.SR, subtype="PCM_24")
        sf.write(os.path.join(OUT, f"{i:02d}_{label} A-C-D-E arka arkaya.wav"), np.concatenate(seq[:-1]), P.SR, subtype="PCM_24")
        row = {"take": label, "clip_from_s": t0}
        for k, y in files.items():
            m = M.analyse(y)
            pk = float(np.abs(y).max()); rms = float(np.sqrt((y ** 2).mean()))
            row[k] = {"mud": round(m["mud"], 1), "pres": round(m["pres"], 1), "air": round(m["air"], 1),
                      "spread": round(m["spread"], 1), "crest_db": round(20 * np.log10(pk / max(rms, 1e-12)), 1),
                      "tp": round(P.true_peak_db(y), 1)}
            if k in reps: row[k]["max_gr"] = round(reps[k]["maxGr"], 1)
        rows.append(row)
        print(json.dumps(row))
    json.dump(rows, open(os.path.join(OUT, "olcum.json"), "w"), indent=1)

if __name__ == "__main__":
    main()
