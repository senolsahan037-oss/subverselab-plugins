#!/usr/bin/env python3
"""Measure HIGH STEREO factory preset proposals on SubverseLab's own sample packs (owner 21 Nov: measure, not listening sets).

Every file runs through the real engine (build/hs_render) once per preset; the output is measured against the input.
Metrics (per file, then median / p90 / worst per material group):
  dLU        stereo loudness change, LU (BS.1770, pyloudnorm)
  monoDelta  how much more the output loses when summed to mono than the input did, LU (+ = worse)
  monoChange loudness of the mono sum, out - in, LU (0 = the mono-exact promise kept)
  widthDb    side/mid power ratio, out - in, dB (how much wider it really got)
  corr       correlation of the whole output; corrP10 = 10th percentile of 400 ms windows (gated -50 dB)
  antiPh     share of STFT energy with |S| > |M| (vanishes in mono), out, % ; antiPhD = out - in
  lowSide    side/mid power below 120 Hz, out, dB (bass should stay mono)
  peak       output sample peak, dBFS
  bandW[k]   side/mid change per band (<100 .. 8k+), dB
Usage: preset_measure.py [--per-role N] [--out logs/preset_measure]
"""
import argparse, glob, json, os, random, subprocess, sys, tempfile
from concurrent.futures import ProcessPoolExecutor
import numpy as np, soundfile as sf, pyloudnorm as pyln
from scipy.signal import stft, butter, sosfiltfilt

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
RENDER = os.path.join(ROOT, "build", "hs_render")
PACKS = os.path.join(ROOT, "..", "subverselab-packs", "out")
ON_SALE = ["SVL_Kotoflux", "SVL_Mirage", "SVL_Nightgrid", "SVL_Saffron", "SVL_Solstice", "SVL_Velvetroom", "SVL_Grindstone",
           "SVL_Woodpanel", "SVL_8BitOrchestral_v4"]
ROLES = ["Full", "Chords", "Texture", "Counter", "Hook", "Bass", "Hats"]

# Proposals told to the owner 21 Nov (names and values are proposals until the owner approves them).
PRESETS = {
    "Default":            {},
    "Mono Safe Wide":     {"width": 130, "monobass": 120, "fix": 1},
    "Wide Master":        {"width": 120, "monobass": 100, "b7": 130, "b8": 140, "fix": 1, "match": 1},
    "Vocal Stays Centre": {"width": 140, "b5": 80, "b6": 80, "spread": 20},
    "Air Wide":           {"b7": 150, "b8": 180},
    "Tight Low End":      {"monobass": 150, "b1": 0, "b2": 60},
    "Fill the Gap":       {"width": 110, "spread": 60},
    "Behind You":         {"width": 180, "monobass": 120, "match": 1},
    "Narrow Pad":         {"width": 60, "spread": 30},
}
EDGES = [20, 100, 250, 500, 1000, 2000, 4000, 8000, 20000]


def pick_files(per_role, seed=7):
    rnd = random.Random(seed)
    by = {r: [] for r in ROLES}
    for p in ON_SALE:
        for f in glob.glob(os.path.join(PACKS, p, "**", "*.wav"), recursive=True):
            stem = os.path.basename(f)[:-4].rsplit("_", 1)[-1]
            if stem in by: by[stem].append(f)
    out = []
    for r in ROLES:
        fs = sorted(by[r]); rnd.shuffle(fs)
        out += [(r, f) for f in (fs if r == "Full" else fs[:per_role])]
    return out


def lufs(meter, x):
    try:
        v = meter.integrated_loudness(x)
        return v if np.isfinite(v) else -120.0
    except Exception:
        return -120.0


def measure(x, rate):
    L, R = x[:, 0].astype(np.float64), x[:, 1].astype(np.float64)
    M, S = 0.5 * (L + R), 0.5 * (L - R)
    meter = pyln.Meter(rate)
    st = lufs(meter, np.stack([L, R], 1))
    mono = lufs(meter, np.stack([M, M], 1))
    pm, ps = np.sum(M * M) + 1e-20, np.sum(S * S) + 1e-20
    corr = float(np.sum(L * R) / np.sqrt(np.sum(L * L) * np.sum(R * R) + 1e-30))
    w = int(0.4 * rate); cs = []
    peak_e = np.max(L * L + R * R) + 1e-20
    for i in range(0, len(L) - w, w // 2):
        l, r = L[i:i + w], R[i:i + w]
        e = np.mean(l * l + r * r)
        if e < peak_e * 1e-5: continue
        cs.append(np.sum(l * r) / np.sqrt(np.sum(l * l) * np.sum(r * r) + 1e-30))
    corrP10 = float(np.percentile(cs, 10)) if cs else corr
    f, _, ZM = stft(M, rate, nperseg=2048); _, _, ZS = stft(S, rate, nperseg=2048)
    em, es = np.abs(ZM) ** 2, np.abs(ZS) ** 2
    anti = float(100.0 * np.sum((em + es) * (es > em)) / (np.sum(em + es) + 1e-20))
    band = []
    for k in range(8):
        sel = (f >= EDGES[k]) & (f < EDGES[k + 1])
        band.append(10 * np.log10((np.sum(es[sel]) + 1e-20) / (np.sum(em[sel]) + 1e-20)))
    sos = butter(4, 120, "low", fs=rate, output="sos")
    ml, sl = sosfiltfilt(sos, M), sosfiltfilt(sos, S)
    low = 10 * np.log10((np.sum(sl * sl) + 1e-20) / (np.sum(ml * ml) + 1e-20))
    return dict(lufs=st, mono=mono, sm=10 * np.log10(ps / pm), corr=corr, corrP10=corrP10, anti=anti, band=band,
                lowSide=float(low), peak=float(20 * np.log10(np.max(np.abs(x)) + 1e-12)))


def run_file(item):
    role, path, presets = item   # presets travel with the item: macOS workers re-import this module (spawn)
    x, rate = sf.read(path, dtype="float32", always_2d=True)
    if x.shape[1] != 2 or len(x) < rate: return None
    base = measure(x, rate)
    rows = []
    with tempfile.TemporaryDirectory() as td:
        fin, fout = os.path.join(td, "in.f32"), os.path.join(td, "out.f32")
        x.tofile(fin)
        for name, prm in presets.items():
            args = [RENDER, fin, fout, str(rate)] + [f"{k}={v}" for k, v in prm.items()]
            subprocess.run(args, check=True)
            y = np.fromfile(fout, dtype=np.float32).reshape(-1, 2)
            m = measure(y, rate)
            rows.append(dict(role=role, file=os.path.relpath(path, PACKS), preset=name,
                             dLU=m["lufs"] - base["lufs"],
                             monoDelta=(m["lufs"] - m["mono"]) - (base["lufs"] - base["mono"]),
                             monoChange=m["mono"] - base["mono"],
                             widthDb=m["sm"] - base["sm"], corrIn=base["corr"], corr=m["corr"], corrP10=m["corrP10"],
                             antiIn=base["anti"], anti=m["anti"], antiPhD=m["anti"] - base["anti"],
                             lowSideIn=base["lowSide"], lowSide=m["lowSide"], peakIn=base["peak"], peak=m["peak"],
                             bandW=[a - b for a, b in zip(m["band"], base["band"])]))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--per-role", type=int, default=40)
    ap.add_argument("--out", default=os.path.join(ROOT, "logs", "preset_measure"))
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--presets", help="JSON file {name: {key: value}} replacing the built-in proposals")
    ap.add_argument("--roles", help="comma list of materials (default all)")
    a = ap.parse_args()
    global PRESETS
    if a.presets: PRESETS.clear(); PRESETS.update(json.load(open(a.presets)))
    items = pick_files(a.per_role)
    if a.roles: items = [i for i in items if i[0] in a.roles.split(",")]
    print(f"{len(items)} files x {len(PRESETS)} presets", flush=True)
    rows = []
    with ProcessPoolExecutor(a.jobs) as ex:
        for i, r in enumerate(ex.map(run_file, [(ro, f, dict(PRESETS)) for ro, f in items], chunksize=2)):
            if r: rows += r
            if (i + 1) % 25 == 0: print(f"  {i + 1}/{len(items)} files", flush=True)
    assert {r["preset"] for r in rows} == set(PRESETS), "rendered presets differ from the requested ones"
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, "rows.json"), "w") as fh: json.dump(dict(presets=PRESETS, rows=rows), fh)
    print("wrote", os.path.join(a.out, "rows.json"), len(rows), "rows")


if __name__ == "__main__":
    main()
