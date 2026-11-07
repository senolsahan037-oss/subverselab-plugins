#!/usr/bin/env python3
"""Builds the Dynamic EQ listening pack (TASK_P2) into 01_AI_Tools/prototypes/DINLE/14_DYNEQ from tools/demo_material.
Runs build/dyneqraw (the engine as the plug-in will run it), level-matches every file to the dry file of its group (integrated LUFS, BS.1770 via
pyloudnorm), writes olcum.json. Run tools/make_material.py first."""
import os, sys, json, subprocess, tempfile
import numpy as np
import pyloudnorm as pyln
from scipy.signal import resample_poly
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import measure as M

HERE = os.path.dirname(os.path.abspath(__file__)); DEMO = os.path.join(HERE, "demo_material")
PACK = os.path.abspath(os.path.join(HERE, "..", "..", "prototypes", "DINLE", "14_DYNEQ")); SR = 48000
GROUPS = {"low": (0, 250), "mid": (250, 2000), "high": (2000, 20000)}

def rd(n): x, sr = M.read_wav(os.path.join(DEMO, n + ".wav")); assert sr == SR; return x
def lufs(x): return float(pyln.Meter(SR).integrated_loudness(np.asarray(x, dtype=np.float64)))
def true_peak(x): return float(20 * np.log10(np.max(np.abs(resample_poly(np.asarray(x, dtype=np.float64), 4, 1, axis=0))) + 1e-12))

def write24(path, x):
    import make_material as mm; mm.write_wav24(path, x)

def engine(main, key, **kw):
    """dyneqraw through measure.run; returns (audio, group stats). Cut statistics: dB, positive = cut, mean and max over the file's blocks."""
    out, cuts, _ = M.run(main, key, **kw)
    st = {}; centres = M.FC; bands = M.LAST_BANDS
    if kw.get("mode") == "self":
        # cuts holds 32 zeros + 4 x (hz, gain dB, Q); per block the deepest hunter bell whose centre is in the group (idle slots 0)
        hz = cuts[:, 32::3][:, :4]; g = -cuts[:, 33::3][:, :4]; hz = hz[:, :4]
        for name, (lo, hi) in GROUPS.items():
            v = np.where((hz >= lo) & (hz < hi), g, 0.0).max(axis=1); st[name] = (float(v.mean()), float(v.max()))
    else:
        c = cuts[:, :32]
        for name, (lo, hi) in GROUPS.items():
            sel = (centres >= lo) & (centres < hi); v = c[:, sel].mean(axis=1); st[name] = (float(v.mean()), float(c[:, sel].max()))
    if bands is not None and np.any(bands != 0):   # free bands: the band's own cut, filed under the group of its centre (only band 1 is used here)
        v = -bands[:, 0]; lo_hi = kw.get("bands", {}).get(1, "")
        if "f=" in lo_hi:
            f = float(lo_hi.split("f=")[1].split(",")[0])
            for name, (lo, hi) in GROUPS.items():
                if lo <= f < hi: st[name] = (float(v.mean()), float(v.max()))
    return out, st

def main():
    os.makedirs(PACK, exist_ok=True)
    beat, bass, kick, voc, res = (rd(n) for n in ("beat", "bass", "kick", "vocal", "resonant"))
    D = dict(depth=6, margin=3, speed="medium")
    items = []   # (name, audio, group dry key, params, stats, matched)
    def add(name, audio, ref, params, stats, matched=True): items.append(dict(name=name, audio=np.asarray(audio, dtype=np.float64), ref=ref, params=params, stats=stats, matched=matched))
    # A: vocal against the beat (COLLIDE, main = beat, key = vocal)
    add("01_VOKAL+BEAT kuru", beat + voc, "A", None, None)
    for name, kw, par in (("02_VOKAL+BEAT temizleyici varsayilan (DEPTH 6 MARGIN 3 MEDIUM)", dict(D), "DEPTH 6 MARGIN 3 MEDIUM"),
                          ("03_VOKAL+BEAT temizleyici derin (DEPTH 12)", dict(D, depth=12), "DEPTH 12 MARGIN 3 MEDIUM"),
                          ("04_VOKAL+BEAT temizleyici hizli (FAST)", dict(D, speed="fast"), "DEPTH 6 MARGIN 3 FAST"),
                          ("05_VOKAL+BEAT temizleyici yavas (SLOW)", dict(D, speed="slow"), "DEPTH 6 MARGIN 3 SLOW"),
                          ("06_VOKAL+BEAT tiling (DEPTH 6, -3 dB kenar)", dict(D, tiling=True), "DEPTH 6 MARGIN 3 MEDIUM, tiling -3 dB")):
        y, st = engine(beat, voc, **kw); add(name, y + voc, "A", par, st)
    y, st = engine(beat, voc, delta=1, **D); add("07_BEAT sadece kesilen (DELTA)", y, "A", "DEPTH 6 MARGIN 3 MEDIUM, DELTA", st, matched=False)
    # B: kick against the bass (COLLIDE, main = bass, key = kick)
    add("08_KICK+BAS kuru", bass + kick, "B", None, None)
    kb = dict(depth=6, margin=3, low=40, high=250, speed="medium")
    y, st = engine(bass, kick, **kb); add("09_KICK+BAS temizleyici (DEPTH 6, FOCUS 40-250)", y + kick, "B", "DEPTH 6 MARGIN 3 FOCUS 40-250 MEDIUM", st)
    y, st = engine(bass, kick, delta=1, **kb); add("10_KICK+BAS DELTA", y, "B", "DEPTH 6 MARGIN 3 FOCUS 40-250, DELTA", st, matched=False)
    # C: resonance hunter (SELF)
    sk = dict(mode="self", depth=12, margin=3)
    add("11_REZONANS kuru", res, "C", None, None)
    y, st = engine(res, None, **sk); add("12_REZONANS avci (SENS 3, DEPTH 12)", y, "C", "SELF SENS 3 DEPTH 12", st)
    y, st = engine(res, None, delta=1, **sk); add("13_REZONANS DELTA", y, "C", "SELF SENS 3 DEPTH 12, DELTA", st, matched=False)
    y, st = engine(beat, None, **sk); add("14_TEMIZ BEAT avci (SENS 3, DEPTH 12)", y, "D", "SELF SENS 3 DEPTH 12 on the plain beat", st)
    # D: free dynamic band
    dyn = "type=bell,f=2500,g=0,q=1.5,dyn=1,thr=-30,range=-6,att=10,rel=100,trig=self"
    y, st = engine(beat, None, depth=0, bands={1: dyn}); add("15_BEAT dinamik bell (2.5 kHz -6 dB, THR -30, SELF)", y, "D", "bell 2.5 kHz Q 1.5, RANGE -6, THR -30, ATTACK 10, RELEASE 100, TRIGGER SELF", st)
    add("16_BEAT kuru", beat, "D", None, None)
    target = {k: lufs(next(i["audio"] for i in items if i["ref"] == k and i["params"] is None)) for k in "ABCD"}
    out = []
    for it in items:
        x = it["audio"]; l0 = lufs(x)
        if it["matched"] and np.isfinite(l0): x = x * 10 ** ((target[it["ref"]] - l0) / 20)
        l1 = lufs(x); tp = true_peak(x)
        assert tp < -0.3, (it["name"], tp)
        if it["matched"]: assert abs(l1 - target[it["ref"]]) <= 0.2, (it["name"], l1, target[it["ref"]])
        write24(os.path.join(PACK, it["name"] + ".wav"), x)
        row = dict(file=it["name"] + ".wav", lufs=round(l1, 2), true_peak_dbfs=round(tp, 2), matched_to_dry=it["matched"], params=it["params"])
        row["cut_db"] = None if it["stats"] is None else {g: {"mean": round(v[0], 3), "max": round(v[1], 3)} for g, v in it["stats"].items()}
        out.append(row); print(f"{it['name']}: {l1:.2f} LUFS, TP {tp:.2f} dBFS, cut {row['cut_db']}")
    meta = dict(note="LUFS: integrated, BS.1770 (pyloudnorm). true_peak_dbfs: 4x oversampled. cut_db: positive = cut, per band group (low <250 Hz, mid 250-2000, high >2000); mean = average over all 32-sample blocks "
                     "of the mean cut of the 32-band centres in the group (SELF: per block the deepest hunter bell whose centre is in the group; free band: its own cut). max = deepest single band / bell / block. "
                     "Group references: A, B, C, D = the dry file of the group (01, 08, 11, 16); 14 is matched to 16.",
                files=out)
    json.dump(meta, open(os.path.join(PACK, "olcum.json"), "w"), indent=1, ensure_ascii=False)

if __name__ == "__main__":
    main()
