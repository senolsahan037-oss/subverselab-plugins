"""YANKI v2 starting points, defined by targets and fitted/verified by measurement through the real processor.

    python3 tools/presets_v2.py fit      fit FEEDBACK (decay) and MIX (first repeat) per preset, write tools/presets_v2.json
    python3 tools/presets_v2.py measure  measure every preset on the house sources, write the table + demos/v2_presets/*.wav
    python3 tools/presets_v2.py taper    measure the WET (MIX) taper and verify the proposed curve

Everything renders at the host tempo of 120 bpm (1 bar = 2.000 s) through build/render_artefacts/Release/render.
"""
import json, os, sys, math
import numpy as np
from lab import *

OUT_JSON = os.path.join(HERE, "presets_v2.json")
DEMOS = os.path.join(ROOT, "demos/v2_presets")

# name, group, targets and the free choices. decay_bars = repeats reach -60 dB re the dry; first_db = first repeat re the dry.
# sync index (v2 face order): 0 1/16, 1 1/8, 2 1/8 dotted, 3 1/4, 4 1/4 dotted, 5 1/2, 6 FREE.   mood: 0 Echo (clean loop), 2 Three Heads, 3 Dub Spiral.
PRESETS = [
    dict(name="Slap 80 ms", group="SLAP", mood=0, sync=6, time_ms=80.0, tone=5200, wear=0.25, spread=0.30, decay_bars=0.5, first_db=-12.0, overlap_max=-6.0),
    dict(name="Sixteenth tight", group="TIGHT", mood=0, sync=0, tone=4500, wear=0.25, spread=0.30, decay_bars=1.0, first_db=-12.0, overlap_max=-6.0),
    dict(name="Eighth clean", group="CLASSIC", mood=0, sync=1, tone=4200, wear=0.25, spread=0.30, decay_bars=2.0, first_db=-12.0, overlap_max=-6.0),
    dict(name="Dotted eighth lift", group="CLASSIC", mood=0, sync=2, tone=3800, wear=0.25, spread=0.40, decay_bars=2.0, first_db=-9.0, overlap_max=-6.0),
    dict(name="Quarter open", group="SPACE", mood=0, sync=3, tone=3600, wear=0.25, spread=0.40, decay_bars=4.0, first_db=-12.0, overlap_max=-8.0),
    dict(name="Dotted eighth behind", group="SPACE", mood=0, sync=2, tone=3000, wear=0.30, spread=0.40, decay_bars=2.0, first_db=-15.0, overlap_max=-12.0),
    dict(name="Half note long", group="SPACE", mood=0, sync=5, tone=3200, wear=0.30, spread=0.40, decay_bars=4.0, first_db=-15.0, overlap_max=-12.0),
    dict(name="Dotted eighth wide", group="WIDE", mood=2, sync=2, tone=3800, wear=0.30, spread=0.90, decay_bars=2.0, first_db=-12.0, overlap_max=-10.0),
    dict(name="Swell build", group="SWELL", mood=3, sync=2, tone=2600, wear=0.8, spread=0.65, fb=0.85, cap=3.0, mix=0.48, output=-2.0),
    dict(name="Swell four-bar arrival", group="SWELL", mood=3, sync=2, tone=2600, wear=0.8, spread=0.65, fb=0.85, cap=0.0, lock=3, mix=0.48, output=-2.0),
]
TAIL = 12.0   # seconds of silence after a source, so the tails are measured


def burst_run(p, mix, fb, bpm=BPM, seconds=18.0, sr=44100):
    T = syncT(p, bpm)
    x, t0, bn = pink_burst(sr, seconds, ms=min(120.0, 0.45 * T * 1000.0))
    q = dict(p, mix=mix, fb=fb)
    y = render(sr, x, q, bpm, "b")
    ref = rms(x[t0:t0 + bn])
    return sr, x, y, t0, bn, ref, T


def syncT(p, bpm=BPM):
    q = 60.0 / bpm
    s = p.get("sync", 6)
    return {0: q / 4, 1: q / 2, 2: q * 0.75, 3: q, 4: q * 1.5, 5: q * 2, 6: p.get("time_ms", 250.0) / 1000.0}[s]


def decay_seconds(p, mix, fb):
    sr, x, y, t0, bn, ref, T = burst_run(p, mix, fb)
    ser = repeat_series(y, sr, t0, bn, T, ref)
    return t60_from_series(ser, T), ser, T


def fit_one(p, tone, fb_range):
    """FEEDBACK knob (engine 0..0.7 = decaying range) by bisection to the decay target; MIX iterated to the first-repeat target."""
    p = dict(p, tone=tone)
    mix = 10 ** (p["first_db"] / 20.0) / 1.4          # clean loop: first repeat ~ 20 log10 (1.4 MIX)
    target = p["decay_bars"] * BAR
    lo, hi = fb_range
    for _ in range(9):
        mid = 0.5 * (lo + hi)
        t, ser, T = decay_seconds(p, mix, mid)
        if t > target: hi = mid
        else: lo = mid
    p["fb"] = round(0.5 * (lo + hi), 4)
    for _ in range(3):                                 # first repeat level: scale MIX
        t, ser, T = decay_seconds(p, mix, p["fb"])
        mix = min(0.5, mix * 10 ** ((p["first_db"] - ser[0]) / 20.0))
    p["mix"] = round(mix, 4)
    return p


def fit(p):
    if "fb" in p:
        return p
    return fit_one(p, p["tone"], (0.02, 0.69))


def cmd_fit():
    out = []
    for p in PRESETS:
        f = fit(p)
        print("%-24s fb %.4f mix %.4f" % (f["name"], f["fb"], f["mix"]), flush=True)
        out.append(f)
    json.dump(out, open(OUT_JSON, "w"), indent=1)


def wet_only(sr, x, p, bpm=BPM):
    y = render(sr, x, p, bpm, "w")
    d = render(sr, x, dict(p, mix=0.0), bpm, "d")
    return y, y - d


def measure_source(p, sr, x, name):
    n = len(x)
    xp = np.vstack([x, np.zeros((int(TAIL * sr), 2))])
    y, w = wet_only(sr, xp, p)
    dry = xp
    d14 = band(dry, sr, 1000, 4000)
    w14 = band(w, sr, 1000, 4000)
    # overlap = wet energy in 1-4 kHz in the 20 ms frames where the dry is active there (dry band within 35 dB of its peak),
    # relative to the dry energy in those frames: what sits UNDER the voice, not what falls in its gaps.
    fr = int(0.02 * sr); nf = len(dry) // fr
    de = (d14[:nf * fr] ** 2).reshape(nf, fr, 2).sum(axis=(1, 2)); we = (w14[:nf * fr] ** 2).reshape(nf, fr, 2).sum(axis=(1, 2))
    act = de > de.max() * 10 ** (-35 / 10)
    overlap = 10 * math.log10(max(we[act].sum(), 1e-30) / max(de[act].sum(), 1e-30))
    L, R = w[:, 0], w[:, 1]
    rho = float(np.corrcoef(L, R)[0, 1]) if L.std() > 0 and R.std() > 0 else 1.0
    mono = 10 * math.log10(max(((L + R) ** 2).mean() / (2 * (L ** 2 + R ** 2).mean()), 1e-12))
    wet_db = db(rms(w) / rms(dry[:n]))
    return dict(y=y, overlap=overlap, rho=rho, mono_loss=mono, wet_vs_dry=wet_db,
                tp_out=true_peak_db(y, sr), tp_dry=true_peak_db(dry, sr))


def cmd_measure():
    presets = json.load(open(OUT_JSON))
    os.makedirs(DEMOS, exist_ok=True)
    sr_v, vocal = load(SRC_VOCAL)
    sr_p, pluck = load(SRC_PLUCK)
    pluck = pad_to(pluck, sr_p, 20.0)
    rows = []
    save24(os.path.join(DEMOS, "0_DRY_vocal.wav"), sr_v, np.vstack([vocal, np.zeros((int(TAIL * sr_v), 2))]))
    save24(os.path.join(DEMOS, "0_DRY_pluck_loop.wav"), sr_p, np.vstack([pluck, np.zeros((int(TAIL * sr_p), 2))]))
    for i, p in enumerate(presets, 1):
        row = dict(n=i, name=p["name"], group=p["group"])
        mv = measure_source(p, sr_v, vocal, "vocal")
        mp = measure_source(p, sr_p, pluck, "pluck")
        slug = p["name"].replace(" ", "_")
        save24(os.path.join(DEMOS, "%d_%s_vocal.wav" % (i, slug)), sr_v, mv["y"])
        save24(os.path.join(DEMOS, "%d_%s_pluck_loop.wav" % (i, slug)), sr_p, mp["y"])
        if p["group"] != "SWELL":
            t, ser, T = decay_seconds(p, p["mix"], p["fb"])
            row.update(time_ms=round(T * 1000, 1), decay_bars_t=p["decay_bars"], decay_bars_m=round(t / BAR, 2),
                       first_t=p["first_db"], first_m=round(ser[0], 1), overlap_t=p["overlap_max"], overlap_m=round(mv["overlap"], 1))
        else:
            row.update(swell_metrics(p, sr_v, vocal))
        row.update(rho=round(mv["rho"], 2), mono_loss=round(mv["mono_loss"], 1), wet_vs_dry=round(mv["wet_vs_dry"], 1),
                   tp_out=round(max(mv["tp_out"], mp["tp_out"]), 1), tp_dry=round(max(mv["tp_dry"], mp["tp_dry"]), 1),
                   overlap_pluck=round(mp["overlap"], 1))
        rows.append(row)
        print(json.dumps(row), flush=True)
    json.dump(rows, open(os.path.join(HERE, "presets_v2_measured.json"), "w"), indent=1)


def swell_metrics(p, sr, vocal):
    """Wet level over time on the vocal: plateau re the dry phrase RMS, seconds to reach within 1 dB of it, peak overshoot."""
    xp = np.vstack([vocal, np.zeros((int(TAIL * sr), 2))])
    y, w = wet_only(sr, xp, p)
    win = int(0.5 * sr)
    wl = [db(rms(w[a:a + win])) for a in range(0, len(w) - win, win)]
    dry_db = db(rms(vocal[np.abs(vocal[:, 0]) > 0.01]))
    peak = max(wl)
    t_reach = next((i * 0.5 for i, v in enumerate(wl) if v >= peak - 1.0), 0.0)
    return dict(time_ms=round(syncT(p) * 1000, 1), swell_plateau_db=round(peak - dry_db, 1), swell_reach_s=t_reach,
                swell_tail_to_m40_s=round(next((i * 0.5 for i in range(len(wl) - 1, -1, -1) if wl[i] > peak - 40.0), 0.0), 1))


def cmd_taper():
    sr_v, vocal = load(SRC_VOCAL)
    sr_p, pluck = load(SRC_PLUCK)
    pluck = pad_to(pluck, sr_p, 20.0)
    base = dict(mood=0, sync=2, tone=3800, wear=0.25, spread=0.40, fb=0.45)
    def wet_level(x, sr, mix):
        xp = np.vstack([x, np.zeros((int(4 * sr), 2))])
        y = render(sr, xp, dict(base, mix=mix), BPM, "t"); d = render(sr, xp, dict(base, mix=0.0), BPM, "t0")
        w = y - d
        return db(rms(w) / rms(xp)), db(rms(y) / rms(xp))
    def taper(x):   # proposed WET curve: knob position -> engine MIX
        if x <= 0.0: return 0.0
        L = -42.0 + 120.0 * min(x, 0.3) if x <= 0.3 else -6.0 + (x - 0.3) / 0.05 * 2.9
        if x <= 0.35: return min(1.0, 10 ** (L / 20.0) / 1.4) * min(1.0, x / 0.03)
        return 0.5 + 0.5 * (x - 0.35) / 0.65
    rows = []
    for k in [0.0, 0.02, 0.05, 0.1, 0.15, 0.2, 0.25, 0.3, 0.35, 0.5, 0.65, 0.8, 1.0]:
        cur = dict(knob=k)
        for nm, (x, sr) in dict(vocal=(vocal, sr_v), loop=(pluck, sr_p)).items():
            a, o = wet_level(x, sr, k)                      # today's MIX = knob
            b, o2 = wet_level(x, sr, taper(k))              # proposed
            cur[nm + "_now_wet_db"] = round(a, 1); cur[nm + "_new_wet_db"] = round(b, 1); cur[nm + "_new_out_db"] = round(o2, 1)
        cur["mix_new"] = round(taper(k), 4)
        rows.append(cur); print(json.dumps(cur), flush=True)
    json.dump(rows, open(os.path.join(HERE, "taper_v2.json"), "w"), indent=1)


if __name__ == "__main__":
    {"fit": cmd_fit, "measure": cmd_measure, "taper": cmd_taper}[sys.argv[1]]()
