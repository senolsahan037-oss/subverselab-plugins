#!/usr/local/bin/python3
"""ALEV listening pack (owner's ear, v2 three-band). Renders through build/korraw_mb, the engine as the plug-in runs it.

Every processed file is loudness-matched (BS.1770) to its dry source, so only the character is heard, not the level.
'fark' = level of (processed - reference) against the reference, in dB, where the reference is the same file at MIX 0
(the crossover's all-pass phase included, so only the saturation is counted): how much the plug-in changes the sound.
Output: 24-bit WAV in prototypes/DINLE/13_ALEV/ and olcum.json.
"""
import os, sys, json
import numpy as np
import soundfile as sf
HERE = os.path.dirname(os.path.abspath(__file__))
os.environ["KORRAW"] = os.path.join(HERE, "..", "build", "korraw_mb")
sys.path.insert(0, HERE)
import measure as m

ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
OUT = os.path.join(ROOT, "prototypes", "DINLE", "13_ALEV")
DRUMS = os.path.join(ROOT, "prototypes", "DINLE", "11_TUTKAL_102", "dinle_24bit", "1_KURU (kompresör yok).wav")
MIX = os.path.join(ROOT, "subverselab-mihenk", "tests", "corpus", "bag_asis.wav")

def p(low, mid, high, mix=1.0): return dict(low=low / 100, mid=mid / 100, high=high / 100, mix=mix)

SETS = [
    (DRUMS, [
        ("01_DAVUL kuru", None),
        ("02_DAVUL varsayilan (LOW 30 MID 30 HIGH 20)", p(30, 30, 20)),
        ("03_DAVUL sadece LOW 80", p(80, 0, 0)),
        ("04_DAVUL sadece MID 80", p(0, 80, 0)),
        ("05_DAVUL sadece HIGH 80", p(0, 0, 80)),
        ("06_DAVUL hepsi 70", p(70, 70, 70)),
        ("07_DAVUL hepsi 100", p(100, 100, 100)),
    ]),
    (MIX, [
        ("08_MASTER kuru", None),
        ("09_MASTER hafif (20 20 15)", p(20, 20, 15)),
        ("10_MASTER varsayilan (30 30 20)", p(30, 30, 20)),
        ("11_MASTER sicak (LOW 60 MID 40 HIGH 20)", p(60, 40, 20)),
        ("12_MASTER parlak (LOW 20 MID 30 HIGH 70)", p(20, 30, 70)),
        ("13_MASTER yogun (60 60 60)", p(60, 60, 60)),
    ]),
]

def main():
    os.makedirs(OUT, exist_ok=True)
    lat = m.reported_latency()
    log = []
    for src, items in SETS:
        x, sr = sf.read(src, dtype="float32", always_2d=True)
        if x.shape[1] == 1: x = np.repeat(x, 2, axis=1)
        ref = m.loudness(x, sr)
        r0 = m.run(x, sr=sr, low=0, mid=0, high=0, mix=0)
        r0 = np.concatenate([r0[lat:], np.zeros((lat, 2), np.float32)])
        for name, params in items:
            if params is None:
                y = x.copy(); diff = None
            else:
                y = m.run(x, sr=sr, **params)
                y = np.concatenate([y[lat:], np.zeros((lat, 2), np.float32)])   # line up with the dry file
                y = y * 10 ** ((ref - m.loudness(y, sr)) / 20)
                diff = round(float(20 * np.log10(np.sqrt(np.mean((y - r0) ** 2)) / np.sqrt(np.mean(r0 ** 2)))), 1)
            peak = float(np.max(np.abs(y)))
            if peak > 0.999: y = y * (0.999 / peak)          # never clip the WAV itself
            path = os.path.join(OUT, name + ".wav")
            sf.write(path, y, sr, subtype="PCM_24")
            row = dict(file=os.path.basename(path), lufs=round(m.loudness(y, sr), 2), true_peak_dbfs=round(m.true_peak_db(y), 2),
                       fark_db=diff, params=params)
            log.append(row); print(row, flush=True)
    with open(os.path.join(OUT, "olcum.json"), "w") as f: json.dump(log, f, indent=1, ensure_ascii=False)

if __name__ == "__main__":
    main()
