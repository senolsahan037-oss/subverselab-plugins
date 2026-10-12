#!/usr/local/bin/python3
"""Listening pack 11_TUTKAL_102: GLUE punch (1.0.2) against 1.0.1 on the whole of ~/Desktop/DED2.wav (raw drum bounce, 44.1 kHz, 0 s to the end, continuous).

  A_kuru           the source
  B_101_punch      1.0.1 (build/tutkalraw_v3), GLUE manual: attack 10 ms, release 0.1 s, 4:1, threshold for a GR p90 of 8 dB
  C_102_punch      1.0.2, same settings, threshold for a GR p90 of 8 dB
  D_102_atak30     1.0.2, as C with attack 30 ms (same threshold as C)
  E_102_AUTO       1.0.2 AUTO (bit-identical to 1.0.1 AUTO), 4:1, attack 10 ms, threshold for a GR p90 of 8 dB

Processed files: latency (63 samples) taken out, ONE gain per file to the dry's integrated LUFS (BS.1770), no limiter; 32-bit float WAV, peaks may pass 0 dBFS.
  dinle4.py [out_dir]     default ../../prototypes/DINLE/11_TUTKAL_102
"""
import json, os, sys
import numpy as np, soundfile as sf, pyloudnorm as pyln
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import measure as m

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "..", "prototypes", "DINLE", "11_TUTKAL_102")
SRC = os.path.expanduser("~/Desktop/DED2.wav")
LAT = m.LAT

def pct(gr, sr):
    g = gr[int(0.5 * sr):]; p10, p50, p90 = np.percentile(g, [10, 50, 90]); return float(p10), float(p50), float(p90), float(p90 - p10)

def search(x, sr, kw, raw, target=8.0):
    lo, hi = -60.0, -5.0
    for _ in range(12):
        mid = 0.5 * (lo + hi); gr = m.run_raw(raw, x, sr=sr, **{**kw, "threshold": mid})[1]
        if pct(gr, sr)[2] > target: lo = mid
        else: hi = mid
    return round(0.5 * (lo + hi), 2)

def main():
    os.makedirs(OUT, exist_ok=True)
    x, sr = sf.read(SRC, dtype="float32", always_2d=True)
    meter = pyln.Meter(sr); lufs_dry = meter.integrated_loudness(x.astype(np.float64))
    base = dict(voicing=1, ratio=4, attack=10)
    v3, v4 = m.raw_v3(), m.RAW
    items = [("A_kuru", None, None, None),
             ("B_101_punch", v3, dict(base, release=100, auto=0), "1.0.1 GLUE manual, attack 10 ms, release 0.1 s, 4:1"),
             ("C_102_punch", v4, dict(base, release=100, auto=0), "1.0.2 GLUE manual, attack 10 ms, release 0.1 s, 4:1"),
             ("D_102_atak30", v4, dict(base, attack=30, release=100, auto=0), "1.0.2 GLUE manual, attack 30 ms, release 0.1 s, 4:1"),
             ("E_102_AUTO", v4, dict(base, auto=1), "1.0.2 GLUE AUTO (= 1.0.1 AUTO), attack 10 ms, 4:1")]
    info = {}; thr_c = None
    for name, raw, kw, desc in items:
        if raw is None:
            y, gr, thr = x, None, None
        else:
            thr = thr_c if name == "D_102_atak30" else search(x, sr, kw, raw)
            if name == "C_102_punch": thr_c = thr
            y, gr = m.run_raw(raw, x, sr=sr, threshold=thr, **kw)
            y = np.concatenate([y[LAT:], np.zeros((LAT, 2), np.float32)])
        lu = meter.integrated_loudness(y.astype(np.float64)); gain = lufs_dry - lu
        yo = (y.astype(np.float64) * 10 ** (gain / 20)).astype(np.float32)
        sf.write(os.path.join(OUT, name + ".wav"), yo, sr, subtype="FLOAT")
        rec = dict(desc=desc or "kaynak, islenmemis", threshold=thr, gain_db=round(float(gain), 2), peak_dbfs=round(float(20 * np.log10(np.max(np.abs(yo)))), 2))
        if gr is not None:
            p10, p50, p90, mod = pct(gr, sr); res, fit = m.best_fit_residual(y, x)
            rec.update(gr_p10=round(p10, 2), gr_p50=round(p50, 2), gr_p90=round(p90, 2), gr_mod=round(mod, 2), gr_max=round(float(gr.max()), 2), residual_db=round(res, 1))
        info[name] = rec; print(name, rec, flush=True)
    # read back from disk
    back = {}
    for name, *_ in items:
        z, s2 = sf.read(os.path.join(OUT, name + ".wav"), dtype="float32", always_2d=True)
        back[name] = dict(frames=len(z), sr=s2, finite=bool(np.all(np.isfinite(z))), lufs=round(float(meter.integrated_loudness(z.astype(np.float64))), 2),
                          peak_dbfs=round(float(20 * np.log10(np.max(np.abs(z)))), 2), subtype=sf.info(os.path.join(OUT, name + ".wav")).subtype)
    json.dump(dict(source=SRC, lufs_dry=round(float(lufs_dry), 2), files=info, readback=back), open(os.path.join(OUT, "readback.json"), "w"), indent=1)
    print("dry LUFS %.2f" % lufs_dry); print(json.dumps(back, indent=1))

if __name__ == "__main__":
    main()
