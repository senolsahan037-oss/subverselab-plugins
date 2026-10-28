#!/usr/bin/env python3
"""TORNA listening pack for the owner's ear (P2b): each setting as A (EQ OUT) and B (EQ IN), rendered by the engine
itself (build/geraw, Source/Ge1100Engine.h), both at -16 LUFS so the curve is heard, not the volume; peaks <= -1 dBFS.
Material: SEDA's synth stems (house rule). Output: ../demos_torna/ and OKU.md, olcum.json.

  make_torna_demos.py
"""
import json, os, subprocess, tempfile
import numpy as np, soundfile as sf, pyloudnorm as pyln

HERE = os.path.dirname(os.path.abspath(__file__))
RAW = os.path.join(HERE, "..", "build", "geraw")
MAT = os.path.join(HERE, "..", "..", "subverselab-seda", "tools", "demo_material")
OUT = os.path.join(HERE, "..", "demos_torna")
SR, TARGET = 48000, -16.0
meter = pyln.Meter(SR)
BANDS = ["16", "32", "63", "125", "250", "500", "1k", "2k", "4k", "8k", "16k", "32k"]

def load(name):
    x, sr = sf.read(os.path.join(MAT, name), always_2d=True); assert sr == SR
    return x
def fit(x, n): return np.tile(x, (int(np.ceil(n / len(x))), 1))[:n]
n = 16 * SR
drums, pluck, lead, vocal = fit(load("drums.wav"), n), load("pluck.wav")[:n], load("lead.wav")[:n], load("vocal.wav")[:n]
mix = drums + 0.5 * pluck + 0.5 * lead + 0.7 * vocal

def marks(**kw):
    m = [0.0] * 12
    for k, v in kw.items(): m[BANDS.index(k.lstrip("_"))] = v
    return m
SMILE = [3, 4, 3, 1, -1, -2, -2, -1, 1, 3, 4, 3]
SETTINGS = [
    # (file name, material, marks, extra flags, what it shows)
    ("01_karisim_gulumseme", mix, SMILE, [], "80'lerin hi-fi 'gülümseme' eğrisi: bas ve tiz yukarı, orta aşağı"),
    ("02_karisim_gulumseme_REV", mix, SMILE, ["--rev"], "aynı sürgüler, NOR/REV basılı: eğri ters döner"),
    ("03_vokal_varlik", vocal, marks(_125=-3, _250=-2, _2k=2, _4k=4, _8k=2), [], "vokal: çamur altta kesik, 4k varlık"),
    ("04_davul", fit(load("drums.wav"), n), marks(_63=6, _125=2, _500=-4, _4k=3, _8k=2), [], "davul: 63 Hz gövde, 500 Hz kutu sesi kesik"),
    ("05_pluck_uc_bant", pluck, marks(_63=-12, _1k=8, _16k=12), [], "uçlar: tek bantlar tam açık/kapalı, bantların genişliği duyulsun"),
]

def render(x, m, flags, bypass):
    with tempfile.TemporaryDirectory() as td:
        i, o = os.path.join(td, "i.wav"), os.path.join(td, "o.wav")
        sf.write(i, x.astype(np.float32), SR, subtype="FLOAT")
        subprocess.run([RAW, i, o, "--marks", ",".join(str(v) for v in m)] + flags + (["--bypass"] if bypass else []), check=True, capture_output=True)
        y, _ = sf.read(o, always_2d=True)
    return y[64:]                                   # remove the engine's 64-sample latency

def norm(x):
    g = 10 ** ((TARGET - meter.integrated_loudness(x)) / 20); y = x * g
    pk = np.max(np.abs(y))
    if pk > 10 ** (-1 / 20): y *= 10 ** (-1 / 20) / pk   # never above -1 dBFS (then a little under -16 LUFS)
    return y

os.makedirs(OUT, exist_ok=True)
log, lines = {}, ["# TORNA dinleme paketi (26 Ekim 2026)", "",
    "Her ayar iki dosya: **A = EQ OUT** (eşitleme yok), **B = EQ IN** (TORNA). İkisi de -16 LUFS'a eşitlendi; fark ses yüksekliği değil, eğri.",
    "Ses doğrudan TORNA motorundan (eklenti değil). Malzeme synth (SEDA'nın kanalları).", ""]
for name, x, m, flags, what in SETTINGS:
    for tag, byp in (("A_EQ_OUT", True), ("B_EQ_IN", False)):
        y = norm(render(x, m, flags, byp)); fn = f"{name}_{tag}.wav"
        sf.write(os.path.join(OUT, fn), y.astype(np.float32), SR, subtype="PCM_24")
        log[fn] = dict(lufs=round(meter.integrated_loudness(y), 2), peak_dbfs=round(20 * np.log10(np.max(np.abs(y))), 2), marks=m, flags=flags)
    lines.append(f"- **{name}**: {what}. Sürgüler (16 Hz … 32 kHz): {', '.join(f'{v:+g}' for v in m)}{' + NOR/REV' if flags else ''}")
lines += ["", "Sorular:", "- B, orijinal cihazın sesine benziyor mu (bantların genişliği, 'gülümseme' eğrisinin karakteri)?",
          "- 02'de REV beklediğin gibi mi (gülümseme yerine 'kaş çatma')?"]
open(os.path.join(OUT, "OKU.md"), "w").write("\n".join(lines) + "\n")
json.dump(log, open(os.path.join(OUT, "olcum.json"), "w"), indent=1)
for k, v in log.items(): print(f"{k:40s} {v['lufs']:+6.2f} LUFS  peak {v['peak_dbfs']:+6.2f} dBFS")
