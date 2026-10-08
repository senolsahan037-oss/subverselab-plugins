#!/usr/local/bin/python3
"""KOR listening pack (owner's ear, P3). Renders through build/korraw, the engine as the plugin runs it.

Every file except the "AYNI TEPE" one is loudness-matched (BS.1770) to its dry source, so only the
character is heard, not the level. Output: 24-bit WAV in prototypes/DINLE/12_KOR/.
"""
import os, sys, json
import numpy as np
import soundfile as sf
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import measure as m

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT = os.path.join(ROOT, "prototypes", "DINLE", "12_KOR")
DRUMS = os.path.join(ROOT, "prototypes", "DINLE", "11_TUTKAL_102", "dinle_24bit", "1_KURU (kompresör yok).wav")
MIX = os.path.join(ROOT, "subverselab-mihenk", "tests", "corpus", "bag_asis.wav")

SETS = [
    (DRUMS, [
        ("1_DAVUL kuru", None),
        ("2_DAVUL KOR yumusak (DRIVE 6, KARAKTER 0)", dict(drive=6, character=0.0)),
        ("3_DAVUL KOR varsayilan (DRIVE 6, KARAKTER 35)", dict(drive=6, character=0.35)),
        ("4_DAVUL KOR sert (DRIVE 12, KARAKTER 100)", dict(drive=12, character=1.0)),
        ("5_DAVUL KOR sicak (DRIVE 9, KARAKTER 35, TON -3)", dict(drive=9, character=0.35, tone=-3)),
        ("6_DAVUL KOR parlak (DRIVE 9, KARAKTER 35, TON +3)", dict(drive=9, character=0.35, tone=3)),
    ]),
    (MIX, [
        ("7_MASTER kuru", None),
        ("8_MASTER KOR hafif (DRIVE 3, KARAKTER 35)", dict(drive=3, character=0.35, ceiling=-1)),
        ("9_MASTER KOR yogun (DRIVE 8, KARAKTER 60)", dict(drive=8, character=0.6, ceiling=-1)),
        ("10_MASTER KOR AYNI TEPE daha yuksek (DRIVE 6, AUTO kapali, tavan -1)", dict(drive=6, character=0.35, ceiling=-1, auto=0)),
    ]),
]

def main():
    os.makedirs(OUT, exist_ok=True)
    log = []
    for src, items in SETS:
        x, sr = sf.read(src, dtype="float32", always_2d=True)
        ref = m.loudness(x, sr)
        for name, params in items:
            if params is None:
                y = x.copy(); note = "kaynak"
            else:
                y = m.run(x, sr=sr, **params)
                lat = m.reported_latency()
                y = np.concatenate([y[lat:], np.zeros((lat, 2), np.float32)])   # line up with the dry file
                if "AYNI TEPE" in name:
                    note = "tepe esit degil, ses yuksek: clipper'in asil isi"
                else:
                    y = y * 10 ** ((ref - m.loudness(y, sr)) / 20); note = "kuruya esit ses"
            peak = float(np.max(np.abs(y)))
            if peak > 0.999: y = y * (0.999 / peak)          # never clip the WAV itself
            path = os.path.join(OUT, name + ".wav")
            sf.write(path, y, sr, subtype="PCM_24")
            row = dict(file=os.path.basename(path), lufs=round(m.loudness(y, sr), 2), true_peak_dbfs=round(m.true_peak_db(y), 2), params=params, note=note)
            log.append(row); print(row)
    with open(os.path.join(OUT, "olcum.json"), "w") as f: json.dump(log, f, indent=1, ensure_ascii=False)

if __name__ == "__main__":
    main()
