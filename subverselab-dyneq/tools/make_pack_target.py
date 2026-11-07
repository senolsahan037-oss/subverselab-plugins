#!/usr/bin/env python3
"""Builds the TARGET listening pack (TASK_P1e, P1i) into 01_AI_Tools/prototypes/DINLE/16_DYNEQ_TARGET.
Synth material only (generated here, fixed seeds): a kick loop, a snare loop, a drum-bus loop and a short vocal-like line.
Runs build/dyneqraw (the engine as the plug-in will run it). P1h (SPEC 8d): every kick / snare / vocal / bus file starts with a pre-roll of hits that
fills the memory (TARGET OFF), the audible part follows with TARGET on, and the pre-roll is trimmed from the file, so every audible hit is processed.
Level matching: LUFS (BS.1770 via pyloudnorm, want +-0.2) except where the cut part is what dominates the loudness (kick SUB / BODY, snare BODY, drum-bus BODY pair):
those are matched on the band the cut does not touch. P1i (SPEC 8e): SLOPE 24 (the default) everywhere, plus 19 (kick SUB at 72 dB/oct), 20 (snare REVERB-READY at 72 dB/oct, no reverb) and
17 (kick A for 8 bars, kick B for 8 bars: active listening); every processed file gets a PNG (1600 x 900) of the form: the average hit spectrum before / after, the applied cut curve, the learned
points, one hit's waveform with the three analysis windows. Writes olcum.json and OKU.md."""
import os, sys, json
import numpy as np
import pyloudnorm as pyln
from scipy.signal import lfilter, butter, sosfilt, sosfiltfilt, fftconvolve, resample_poly
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import measure as M
import make_material as mm
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
PACK = os.path.abspath(os.path.join(HERE, "..", "..", "prototypes", "DINLE", "16_DYNEQ_TARGET")); SR = 48000

def lufs(x): return float(pyln.Meter(SR).integrated_loudness(np.asarray(x, dtype=np.float64)))
def true_peak(x): return float(20 * np.log10(np.max(np.abs(resample_poly(np.asarray(x, dtype=np.float64), 4, 1, axis=0))) + 1e-12))

# ------------------------------------------------------------------------------------------------ synth material
def kick(rng, f0=58.0, secs=0.5):
    t = np.arange(int(secs * SR)) / SR
    pitch = f0 * (1 + 1.6 * np.exp(-t / 0.018))                      # the beater drop: 58 Hz boom with a fast pitch fall
    boom = 0.8 * np.sin(2 * np.pi * np.cumsum(pitch) / SR) * np.exp(-t / 0.13)
    click = 0.45 * rng.standard_normal(len(t)) * np.exp(-t / 0.0008) * (t < 0.005)
    return boom + click

def snare(rng, secs=0.6):
    t = np.arange(int(secs * SR)) / SR
    noise = sosfilt(butter(2, 1500, "high", fs=SR, output="sos"), rng.standard_normal(len(t))) * 0.55 * np.exp(-t / 0.09)
    snap = 0.45 * rng.standard_normal(len(t)) * np.exp(-t / 0.006)
    ring = 0.38 * np.sin(2 * np.pi * 205 * t) * np.exp(-t / 0.11) + 0.16 * np.sin(2 * np.pi * 980 * t) * np.exp(-t / 0.07)   # the shell's ring and its upper partial
    return noise + snap + ring

def hat(rng, secs=0.12, tau=0.025):
    t = np.arange(int(secs * SR)) / SR
    return 0.3 * sosfilt(butter(2, 6500, "high", fs=SR, output="sos"), rng.standard_normal(len(t))) * np.exp(-t / tau)

def place(hits, times, secs):
    x = np.zeros(int(secs * SR))
    for h, t in zip(hits, times):
        a = int(round(t * SR)); n = min(len(h), len(x) - a)
        if n > 0: x[a:a + n] += h[:n]
    return x

def loops(secs=12.0):
    rng = np.random.default_rng(16); beat = 0.5                                   # 120 bpm
    kt = [k * beat for k in range(int(secs / beat) - 1) if k % 4 in (0, 2) or k % 8 == 7]
    st = [k * beat for k in range(int(secs / beat) - 1) if k % 4 in (1, 3)]
    ht = [k * beat / 2 for k in range(int(secs / beat * 2) - 2)]
    K = place([kick(rng, 56 + 4 * rng.random()) for _ in kt], kt, secs)
    S = place([snare(rng) for _ in st], st, secs)
    H = place([hat(rng, tau=0.02 if i % 2 else 0.04) for i in range(len(ht))], ht, secs)
    every = [k * beat for k in range(int(secs / beat) - 1)]
    kicks = {f0: place([kick(np.random.default_rng(100 + f0), f0) for _ in every], every, secs) for f0 in (55, 70)}   # one kick sample per loop: the fish memory sees the same kick again and again
    return dict(kick55=kicks[55], kick70=kicks[70], snare=S, bus=K + S + 0.6 * H, n_kicks=len(kt), n_snares=len(st), kt=kt, st=st, every=every)

def swap_loop():
    """16 bars at 120 bpm (32 s), a kick on every beat: kick A (50 Hz) for 8 bars, kick B (70 Hz, a different sample) for the next 8. One instance, TARGET selected throughout."""
    beat = 0.5; times = [k * beat for k in range(64)]
    hits = [kick(np.random.default_rng(300 + k), 50 if k < 32 else 70) for k in range(64)]
    return place(hits, times, 32.0), times

def vocal_line(secs=8.0):
    """A short vocal-like line: 's' + vowel syllables (formant synth: sawtooth through three resonators) with a pitch glide."""
    rng = np.random.default_rng(17); x = np.zeros(int(secs * SR))
    vowels = [(650, 1100, 2600), (400, 2000, 2800), (300, 800, 2500), (500, 1500, 2500)]
    hp = butter(2, 5000, "high", fs=SR, output="sos")
    for i in range(int(secs / 0.7) - 1):
        t0 = 0.2 + 0.7 * i; a = int(t0 * SR)
        s = sosfilt(hp, rng.standard_normal(int(0.07 * SR))) * 0.22 * np.hanning(int(0.07 * SR)) * (1.0 if i % 2 == 0 else 0.0)   # an 's' before every second vowel
        n = int(0.45 * SR); t = np.arange(n) / SR; f0 = 135 + 22 * np.sin(2 * np.pi * 0.7 * i) + 12 * t / 0.45
        src = 2 * ((np.cumsum(f0) / SR) % 1.0) - 1.0
        v = np.zeros(n)
        for f, g in zip(vowels[i % 4], (1.0, 0.6, 0.3)):
            w0 = 2 * np.pi * f / SR; r = 0.985; v += g * lfilter([1 - r], [1, -2 * r * np.cos(w0), r * r], src)
        env = np.minimum(1, t / 0.03) * np.minimum(1, (0.45 - t) / 0.12) * 0.9
        x[a:a + len(s)] += s; b = a + len(s); x[b:b + n] += (v * env)[:max(0, min(n, len(x) - b))]
    return x

def norm_peak(x, peak=0.7): return x / np.max(np.abs(x)) * peak

# ------------------------------------------------------------------------------------------------ processing
BLOCK = 32
PRE = {"kick": 4.0, "snare": 6.0, "bus": 5.0, "vocal": 5.8, "swap": 4.0}   # seconds of the file's own first part, played before it with TARGET OFF (>= 8 kicks / 6 snares / 5 kicks and 4 snares in the bus / 8 syllables)
SWITCH_EARLY = 0.2                                              # TARGET is chosen this long before the audible part (a CLICK cut needs ~6 ms of ramp, the first audible hit must not wait for it)

def with_preroll(x, pre_s):
    """pre-roll = the file's own first pre_s seconds (a real instance of the same material), faded out over 10 ms at its end so the join is not a click, then the whole file."""
    n = int(pre_s * SR); pre = x[:n].copy(); f = int(0.010 * SR); pre[-f:] *= np.linspace(1, 0, f)[:, None]
    return np.concatenate([pre, x])

def chain(x, pre_s, specs):
    """specs: one TARGET setting per instance in series. Every instance gets the pre-roll rendered with TARGET OFF and is switched to its setting just before the audible part.
    Returns the audible audio (the pre-roll is cut away), the TARGET cut per block over the audible part for each instance, and the learned points each instance ended with."""
    full = with_preroll(x, pre_s); cuts, learn = [], []
    for sp in specs:
        full = M.run(full, None, want_log=True, target="inst=off", learn=True, extra=["--switch", f"{pre_s - SWITCH_EARLY}:{sp}"])[0].astype(np.float64)
        cuts.append(M.LAST_TARGET[int(pre_s * SR / BLOCK):].copy()); learn.append(M.LAST_LEARN)
    return full[int(pre_s * SR):], cuts, learn

def reverb(x):
    """The same synthetic reverb for every file: 1.5 s exponential-decay noise (-60 dB at 1.5 s), different noise per side, 35 % wet added to the signal."""
    wet = np.stack([fftconvolve(x[:, c], M.reverb_ir(1.5, 7 + 10 * c))[:len(x)] for c in range(2)], axis=1)
    return x + 0.35 * wet / np.sqrt(np.mean(wet ** 2)) * np.sqrt(np.mean(x ** 2))

PARTS = {"kick": ("SUB", "BODY", "CLICK"), "snare": ("BODY", "CRACK", "SNAP/AIR"), "vocal": ("BOX", "NASAL", "HARSH", "S"), "kickbody": ("SUB", "BODY", "CLICK"), "snarebody": ("BODY", "CRACK", "SNAP/AIR")}
def spec_kv(spec): return dict(kv.split("=") for kv in spec.split(","))
def learned(L, spec, audible_hits):
    """What the engine found in this material (the learned points after the whole file), where REVERB-READY put its corners and where the SUB / CLICK crossovers stood while they cut."""
    r = {k: float(L[k][-1]) for k in L.dtype.names}
    kv = spec_kv(spec); key = kv["part"] if kv["inst"] == "drums" else kv["inst"]; parts = {}
    for i, name in enumerate(PARTS.get(key, ())):
        if r[f"p{i}_ok"]: parts[name] = dict(centre_hz=round(r[f"p{i}_c"], 1), lo_hz=round(r[f"p{i}_lo"], 1), hi_hz=round(r[f"p{i}_hi"], 1))
    out = dict(onsets_incl_preroll=int(r["onsets"]), hits_learned_incl_preroll=int(r["learned"]), hits_in_memory=int(r["hits"]), audible_hits=audible_hits, confidence=round(r["conf"], 2),
               fundamental_hz=round(r["f0"], 2) if r["f0"] > 0 else None, transient_end_ms=round(r["trans_end_ms"], 1), parts=parts, amount_db=float(kv.get("amount", 6)), slope_db_oct=int(float(kv.get("slope", 24))))
    on = L["rev_mix"] > 0.99
    if on.any(): out["reverb_ready_corners_hz"] = dict(low_corner_hz=round(float(L["rev_low"][on][-1]), 1), high_corner_hz=round(float(L["rev_high"][on][-1]), 1))
    for nm, hz, dbc in (("below_the_edge (SUB)", "edge_lo_hz", "edge_lo_db"), ("above_the_edge (CLICK)", "edge_hi_hz", "edge_hi_db")):
        if float(np.max(L[dbc])) > 0.5 and not on.any(): out.setdefault("edge_crossovers_hz", {})[nm] = round(float(L[hz][int(np.argmax(L[dbc]))]), 1)
    return out

def stats(t):
    return dict(mean=round(float(t.mean()), 2), max=round(float(t.max()), 2), percent_blocks_cut=round(float(100 * np.mean(t > 0.1)), 1))

def band_rms_db(x, lo, hi):
    """Level of the mono sum inside lo..hi Hz over the whole file, dB (zero-phase 4th-order band-pass)."""
    m = np.mean(np.asarray(x, np.float64), axis=1); y = sosfiltfilt(butter(2, [lo, min(hi, 0.45 * SR)], "band", fs=SR, output="sos"), m)
    return float(10 * np.log10(np.mean(y ** 2) + 1e-30))

def comp_band_db(y, x, t0, t1, lo, hi):
    """Energy in lo..hi Hz of y relative to x over [t0, t1] s (negative = y has less). Hann window, zero-padded FFT."""
    a, b = int(t0 * SR), int(t1 * SR); w = np.hanning(b - a); f = np.fft.rfftfreq(1 << 16, 1 / SR); m = (f >= lo) & (f <= hi)
    e = lambda s: np.sum(np.abs(np.fft.rfft(np.mean(np.asarray(s, np.float64), axis=1)[a:b] * w, 1 << 16))[m] ** 2)
    return float(10 * np.log10((e(y) + 1e-30) / (e(x) + 1e-30)))

def per_hit_cut(dry, proc, times, lo, hi, t0, t1):
    """The cut of one part on every audible hit: energy of the part's band between t0 and t1 s after the onset, processed file against the dry file, dB."""
    return [round(comp_band_db(proc, dry, t + t0, t + t1, lo, hi), 2) for t in times if (t + t1 + 0.002) * SR <= len(dry)]

def summarise(cuts): return dict(cuts=cuts, worst=round(max(cuts), 2), best=round(min(cuts), 2), spread=round(max(cuts) - min(cuts), 2), hits=len(cuts))

# ------------------------------------------------------------------------------------------------ the form (SPEC 8e.1): one PNG per processed file
INK = "#efe9da"; BG = "#0f1411"; GREY = "#8f8f88"; GOLD = "#e3b341"; TEAL = "#3fb8a8"; ORANGE = "#ef7d3c"; PANEL = "#171d19"
def q_from_bw(bw_oct): return 1.0 / (2.0 * np.sinh(0.34657359 * np.clip(bw_oct, 0.3, 2.5)))
def bell_db(f, fc, q, cut_db):
    A = 10 ** (-cut_db / 40); w0 = 2 * np.pi * fc / SR; al = np.sin(w0) / (2 * q); b = np.array([1 + al * A, -2 * np.cos(w0), 1 - al * A]); a = np.array([1 + al / A, -2 * np.cos(w0), 1 - al / A])
    z = np.exp(-1j * 2 * np.pi * np.asarray(f, float) / SR); return 20 * np.log10(np.abs((b[0] + b[1] * z + b[2] * z ** 2) / (a[0] + a[1] * z + a[2] * z ** 2)))
def edge_db(f, fe, slope, amount, below):
    """The Linkwitz-Riley split at fe with the cut band at -amount (AMOUNT 60 = the band is removed), in dB."""
    h = np.abs(M.lr_h(f, fe, slope, 400.0 if amount >= 59.99 else amount, below)); return 20 * np.log10(np.maximum(h, 1e-6))
BELL_SCALE = {("KICK", "BODY"): 0.70}   # the engine's kTargetBwScaleBody for a BODY part of kind kPkBody (kick, bass); every other bell 0.9
def cut_curve(f, inst, part, spec, info, log):
    """The cut the engine applies at full depth, in dB over f: a bell for BODY / CRACK / vocal parts, a Linkwitz-Riley split for the kick's SUB and CLICK and for REVERB-READY. The crossovers are the
    ones the engine logged while it cut."""
    kv = spec_kv(spec); amount = float(kv.get("amount", 6)); slope = int(float(kv.get("slope", 24))); width = float(kv.get("width", 1)); curve = np.zeros(len(f)); note = ""
    if kv.get("reverb") == "1" and "reverb_ready_corners_hz" in info:
        lo, hi = info["reverb_ready_corners_hz"]["low_corner_hz"], info["reverb_ready_corners_hz"]["high_corner_hz"]
        return edge_db(f, lo, slope, amount, True) + edge_db(f, hi, slope, amount, False), [("alt kesme siniri", lo), ("ust kesme siniri", hi)]
    if inst == "KICK" and part == "SUB":
        e = float(log["edge_lo_hz"][int(np.argmax(log["edge_lo_db"]))]); return edge_db(f, e, slope, amount, True), [("kesme siniri", e)]
    if inst == "KICK" and part == "CLICK":
        e = float(log["edge_hi_hz"][int(np.argmax(log["edge_hi_db"]))]); return edge_db(f, e, slope, amount, False), [("kesme siniri", e)]
    pd = info["parts"].get(part)
    if pd:
        sc = BELL_SCALE.get((inst, part), 0.9); q = q_from_bw(np.log2(pd["hi_hz"] / max(pd["lo_hz"], 1.0)) * sc) * width
        return bell_db(f, pd["centre_hz"], q, min(amount, 18.0)), []
    return curve, []
def protect_lines(inst, part, spec, log):
    """SPEC 8g, from the engine's own numbers at the end of the run: the loss at the protected neighbour's centre (limit 1.0 dB), the cut reached at the target part's centre, and the 'slope too gentle' line."""
    kv = spec_kv(spec); amount = float(kv.get("amount", 6)); slope = int(float(kv.get("slope", 24))); rev = kv.get("reverb") == "1"
    edges = [("lo", "alt", "govde")] + [("hi", "ust", "ust parca")] if rev else ([("lo", "alt", "govde")] if (inst == "KICK" and part == "SUB") else ([("hi", "ust", "govde")] if (inst == "KICK" and part == "CLICK") else []))
    out = []; gentle = False
    for w, side, prot in edges:
        loss, ach = float(log[f"prot_{w}_db"][-1]), float(log[f"ach_{w}_db"][-1])
        out.append(f"{prot} merkezinde kayip: {-max(loss, 0.0):.1f} dB (sinir 1.0)")
        if ach < amount - 1.0:
            gentle = True; out.append(f"SLOPE yetersiz: {side} kesim -{ach:.1f} dB, govdeye dokunmadan; " + ("48/72 daha derin keser" if slope < 48 else "daha dik SLOPE yok"))
    return out
def smooth_log(f, p, grid):
    """Power spectrum p(f) averaged over +-1/12 octave around every grid frequency (dB)."""
    c = np.concatenate([[0.0], np.cumsum(p)]); lo = np.searchsorted(f, grid * 2 ** (-1 / 12)); hi = np.searchsorted(f, grid * 2 ** (1 / 12)); hi = np.maximum(hi, lo + 1)
    return 10 * np.log10((c[hi] - c[lo]) / (hi - lo) + 1e-30)
def avg_spectrum(x, times, t0=0.0, t1=0.15, nfft=1 << 15):
    """Average hit spectrum. P1j: the long windows (0-150 ms) start 10 ms before the onset with a cosine rise, flat, cosine fade over the last 30 % (measure.edge_win): a Hann from the onset weights the first
    milliseconds by ~0, so the all-pass delay of a steep split moved energy into the heavy part of the window and the picture showed a bump the signal does not have. The 7 ms transient windows keep a Hann."""
    m = np.mean(np.asarray(x, np.float64), axis=1); f = np.fft.rfftfreq(nfft, 1 / SR); acc = np.zeros(len(f)); n = 0
    long_ = (t1 - t0) >= 0.05; pre = int(M.EDGE_S * SR) if long_ else 0
    for t in times:
        a, b = int((t + t0) * SR) - pre, int((t + t1) * SR)
        if a < 0 or b > len(m): continue
        acc += np.abs(np.fft.rfft(m[a:b] * (M.edge_win(b - a) if long_ else np.hanning(b - a)), nfft)) ** 2; n += 1
    return f, acc / max(n, 1)
def worst_band(dry, proc_raw, times):
    """T25's number for one file: the engine output BEFORE the level match (the match is a gain, not part of the cut) against the dry file, the worst 1/3-octave band of the average hit spectrum, windows 0-150 and 0-500 ms, dB."""
    return max(M.worst_excess(dry, proc_raw, times, w)[0] for w in (0.150, 0.500))
def style(ax):
    ax.set_facecolor(PANEL)
    for sp in ax.spines.values(): sp.set_color("#3a443d")
    ax.tick_params(colors=INK, labelsize=10); ax.xaxis.label.set_color(INK); ax.yaxis.label.set_color(INK)
GRID = 20 * 2 ** (np.arange(0, 10 * 24 + 1) / 24)
def png_normal(path, title, inst, part, spec, info, log, dry, proc, times, rep, extra_lines=(), extra_marks=(), curve_override=None, worst_db=None):
    kv = spec_kv(spec); amount = float(kv.get("amount", 6)); slope = int(float(kv.get("slope", 24)))
    fig = plt.figure(figsize=(16, 9), dpi=100, facecolor=BG); ax = fig.add_axes([0.06, 0.47, 0.88, 0.45]); bx = fig.add_axes([0.06, 0.07, 0.88, 0.27]); style(ax); style(bx)
    win = (-0.001, 0.006) if part in ("CLICK", "CRACK", "SNAP/AIR", "CHICK", "TOP") else (0.0, 0.150)   # the transient parts live in the first 5 ms, which the boom of a 150 ms window buries
    f, pd_ = avg_spectrum(dry, times, *win); _, pp = avg_spectrum(proc, times, *win); sd, sp = smooth_log(f, pd_, GRID), smooth_log(f, pp, GRID); top = float(np.max(sd))
    ax.plot(GRID, sd - top, color=GREY, lw=2.0, label=f"kuru (ortalama vurus, {win[0] * 1000:.0f} ... {win[1] * 1000:.0f} ms)"); ax.plot(GRID, sp - top, color=GOLD, lw=2.0, label="islenmis")
    ax.set_xscale("log"); ax.set_xlim(20, 20000); lo_y = max(float(np.min(sp - top)), -90.0); ax.set_ylim(lo_y - 3, 6); ax.set_xlabel("frekans (Hz)"); ax.set_ylabel("seviye (dB, kurunun tepesine gore)"); ax.grid(True, which="both", color="#2a332d", lw=0.5)
    curve, marks = cut_curve(GRID, inst, part, spec, info, log)
    if curve_override is not None: curve = curve_override
    a2 = ax.twinx(); a2.set_facecolor("none"); a2.plot(GRID, curve, color=TEAL, lw=1.6, ls="-", label="uygulanan kesim (TARGET kazanci, dB)")
    cmin = float(np.min(curve)); a2.set_ylim(max(min(cmin - 3, -6), -70), 3); a2.fill_between(GRID, curve, 0, where=curve <= -1.0, color=TEAL, alpha=0.18); a2.set_ylabel("kesim (dB)", color=TEAL); a2.tick_params(colors=TEAL, labelsize=10)
    for sp_ in a2.spines.values(): sp_.set_color("#3a443d")
    tr = {"SUB": "alt (SUB)", "BODY": "govde", "CLICK": "tiklama", "CRACK": "crack", "SNAP/AIR": "snap", "BOX": "kutu", "NASAL": "genis", "HARSH": "sert", "S": "s sesi"}
    ymax = ax.get_ylim()[1]
    for name, pdv in info["parts"].items():
        hz = info["fundamental_hz"] if name == "SUB" and info.get("fundamental_hz") else pdv["centre_hz"]
        ax.axvline(hz, color=INK, lw=1.0, ls=":", alpha=0.8); ax.text(hz * 1.03, ymax - 1, f"{tr.get(name, name)} {hz:.0f} Hz", rotation=90, va="top", ha="left", color=INK, fontsize=9)
    for lab, hz in list(marks) + list(extra_marks):
        ax.axvline(hz, color=TEAL, lw=1.4, ls="--"); ax.text(hz * 0.97, ymax - 1, f"{lab} {hz:.0f} Hz", rotation=90, va="top", ha="right", color=TEAL, fontsize=9)
    h1, l1 = ax.get_legend_handles_labels(); h2, l2 = a2.get_legend_handles_labels(); ax.legend(h1 + h2, l1 + l2, loc="center right", facecolor=PANEL, edgecolor="#3a443d", labelcolor=INK, fontsize=10)
    pc = info["parts"].get(part) if part in info["parts"] else None; centre = (info.get("fundamental_hz") if part == "SUB" else None) or (pc["centre_hz"] if pc else 1000.0)
    cut_at = float(np.interp(np.log(centre), np.log(GRID), curve)); got = ", ".join(f"{tr.get(n, n)} {v['centre_hz']:.0f}" for n, v in info["parts"].items())
    if "reverb_ready_corners_hz" in info and kv.get("reverb") == "1": cut_line = f"kesilen bantlar: alt {info['reverb_ready_corners_hz']['low_corner_hz']:.0f} Hz alti, ust {info['reverb_ready_corners_hz']['high_corner_hz']:.0f} Hz ustu"
    else: cut_line = f"parca merkezinde kesim: {cut_at:.1f} dB (tam derinlikte)"
    box = [f"ENSTRUMAN: {inst}   PARCA: {part}", f"AMOUNT: {amount:g} dB   SLOPE: {slope} dB/okt", f"ogrenilen (Hz): {got}", cut_line] + list(extra_lines) + protect_lines(inst, part, spec, log)
    if worst_db is not None: box.append(f"en kotu bant: {worst_db:+.1f} dB (sinir +1.0)")
    ax.text(0.985, 0.03, "\n".join(box), transform=ax.transAxes, va="bottom", ha="right", color=INK, fontsize=10, family="DejaVu Sans Mono", bbox=dict(boxstyle="round,pad=0.5", fc=BG, ec=GOLD, lw=1.0, alpha=0.92))
    ax.set_title(title, color=INK, fontsize=14, loc="left", pad=10)
    # one hit, 0-150 ms: dry against processed, with the three analysis windows
    t0 = times[rep]; a, b = int(t0 * SR), int((t0 + 0.150) * SR); ms = (np.arange(b - a) / SR) * 1000.0; wd = np.mean(dry[a:b], axis=1); wp = np.mean(proc[a:b], axis=1); amp_ = 1.08 * max(np.max(np.abs(wd)), np.max(np.abs(wp)), 1e-6)
    for (x0, x1, c, lab) in ((0, 5, "#4a6f9c", "vurus 0-5 ms"), (5, 40, "#6a8f5a", "erken 5-40 ms"), (40, 150, "#8a6a9c", "gec 40 ms ->")):
        bx.axvspan(x0, x1, color=c, alpha=0.20, lw=0); bx.text((x0 + min(x1, 150)) / 2, amp_ * 0.93, lab, ha="center", va="top", color=INK, fontsize=9)
    bx.plot(ms, wd, color=GREY, lw=1.4, label="kuru"); bx.plot(ms, wp, color=GOLD, lw=1.4, label="islenmis")
    te = info.get("transient_end_ms")
    if te: bx.axvline(te, color=TEAL, lw=1.2, ls="--"); bx.text(te + 1, -amp_ * 0.88, f"ogrenilen vurus sonu {te:.1f} ms", color=TEAL, fontsize=9, va="bottom")
    bx.set_xlim(0, 150); bx.set_ylim(-amp_, amp_); bx.set_xlabel(f"vurustan sonra (ms) - ornek vurus: {t0:.1f} s"); bx.set_ylabel("genlik"); bx.grid(True, color="#2a332d", lw=0.5); bx.legend(loc="lower right", facecolor=PANEL, edgecolor="#3a443d", labelcolor=INK, fontsize=10)
    fig.savefig(path, facecolor=BG); plt.close(fig)

def png_swap(path, title, spec, info, log, dry, proc, times, pre_s, worst_db=None):
    """The active-listening file: the average hit spectrum before and after the swap (kick A, kick B), the two learned subs, and the learned sub / the cut depth over time."""
    kv = spec_kv(spec); amount = float(kv.get("amount", 6)); slope = int(float(kv.get("slope", 24))); nA = 32; tA, tB = times[:nA], times[nA:]
    fig = plt.figure(figsize=(16, 9), dpi=100, facecolor=BG); ax = fig.add_axes([0.06, 0.47, 0.88, 0.45]); bx = fig.add_axes([0.06, 0.07, 0.88, 0.27]); style(ax); style(bx)
    fa, pda = avg_spectrum(dry, tA[3:]); _, ppa = avg_spectrum(proc, tA[3:]); fb, pdb = avg_spectrum(dry, tB[3:]); _, ppb = avg_spectrum(proc, tB[3:])   # (P1j: the same window as the number in the box)
    sda, spa, sdb, spb = (smooth_log(fa, v, GRID) for v in (pda, ppa, pdb, ppb)); top = float(max(np.max(sda), np.max(sdb)))
    ax.plot(GRID, sda - top, color=GREY, lw=1.8, label="A kuru (1-8. olcu)"); ax.plot(GRID, spa - top, color=GOLD, lw=1.8, label="A islenmis"); ax.plot(GRID, sdb - top, color="#c9c9c0", lw=1.4, ls="--", label="B kuru (9-16. olcu)"); ax.plot(GRID, spb - top, color=ORANGE, lw=1.8, label="B islenmis")
    ax.set_xscale("log"); ax.set_xlim(20, 20000); ax.set_ylim(-80, 6); ax.set_xlabel("frekans (Hz)"); ax.set_ylabel("seviye (dB)"); ax.grid(True, which="both", color="#2a332d", lw=0.5)
    tt = log["t_s"] - pre_s; e = np.asarray(log["edge_lo_hz"]); d = np.asarray(log["edge_lo_db"]); f0 = np.asarray(log["f0"])
    iA = int(np.searchsorted(tt, times[nA - 1] + 0.3)); iB = len(tt) - 1; eA, eB = float(e[iA]), float(e[iB]); fA, fB = float(f0[iA]), float(f0[iB])
    a2 = ax.twinx(); a2.set_facecolor("none"); a2.plot(GRID, edge_db(GRID, eA, slope, amount, True), color=TEAL, lw=1.5, label="A: uygulanan kesim"); a2.plot(GRID, edge_db(GRID, eB, slope, amount, True), color=TEAL, lw=1.5, ls="--", label="B: uygulanan kesim")
    a2.set_ylim(-3 * amount - 3, 3); a2.set_ylabel("kesim (dB)", color=TEAL); a2.tick_params(colors=TEAL, labelsize=10)
    for sp_ in a2.spines.values(): sp_.set_color("#3a443d")
    for hz, lab, col in ((fA, "ogrenilen sub A", INK), (fB, "ogrenilen sub B", ORANGE)):
        ax.axvline(hz, color=col, lw=1.2, ls=":"); ax.text(hz * 1.03, 5, f"{lab} {hz:.1f} Hz", rotation=90, va="top", ha="left", color=col, fontsize=10)
    h1, l1 = ax.get_legend_handles_labels(); h2, l2 = a2.get_legend_handles_labels(); ax.legend(h1 + h2, l1 + l2, loc="center right", facecolor=PANEL, edgecolor="#3a443d", labelcolor=INK, fontsize=9)
    box = ["ENSTRUMAN: KICK   PARCA: SUB", f"AMOUNT: {amount:g} dB   SLOPE: {slope} dB/okt", f"A: sub {fA:.1f} Hz, kesme siniri {eA:.0f} Hz", f"B: sub {fB:.1f} Hz, kesme siniri {eB:.0f} Hz", f"yeniden ogrenme olayi: {int(log['relearns'][-1])} (sample degisimi)"] + ([f"en kotu bant: {worst_db:+.1f} dB (sinir +1.0)"] if worst_db is not None else [])
    ax.text(0.985, 0.03, "\n".join(box), transform=ax.transAxes, va="bottom", ha="right", color=INK, fontsize=10, family="DejaVu Sans Mono", bbox=dict(boxstyle="round,pad=0.5", fc=BG, ec=GOLD, lw=1.0, alpha=0.92))
    ax.set_title(title, color=INK, fontsize=14, loc="left", pad=10)
    m = tt >= 0; bx.step(tt[m], np.where(e[m] > 0, np.asarray(log["rule_lo_hz"])[m] / (1.2 * 1.5), np.nan), color=GOLD, lw=1.8, where="post", label="kesmenin oturdugu sub (kesme siniri / 1.8)")
    bx.axvline(times[nA], color=ORANGE, lw=1.4, ls="--"); bx.text(times[nA] + 0.15, 78, "sample degisimi (kick A -> kick B)", color=ORANGE, fontsize=10, va="top")
    bx.set_xlim(0, 32); bx.set_ylim(35, 80); bx.set_xlabel("sure (s)"); bx.set_ylabel("sub (Hz)", color=GOLD); bx.grid(True, color="#2a332d", lw=0.5); bx.tick_params(axis="y", colors=GOLD)
    b2 = bx.twinx(); b2.set_facecolor("none"); b2.plot(tt[m], d[m], color=TEAL, lw=1.0, alpha=0.9, label="kesim derinligi (dB)"); b2.set_ylim(0, amount * 2.2); b2.set_ylabel("kesim (dB)", color=TEAL); b2.tick_params(colors=TEAL, labelsize=10)
    for sp_ in b2.spines.values(): sp_.set_color("#3a443d")
    h1, l1 = bx.get_legend_handles_labels(); h2, l2 = b2.get_legend_handles_labels(); bx.legend(h1 + h2, l1 + l2, loc="center right", facecolor=PANEL, edgecolor="#3a443d", labelcolor=INK, fontsize=9)
    fig.savefig(path, facecolor=BG); plt.close(fig)

def click_cut_db(dry, proc, t, lo, hi):
    """The click band in the first 5 ms after t, processed against dry, dB; both signals through a zero-phase band-pass first (an edge cut moves the phase of what is below its crossover)."""
    return M.bandzp_db(np.mean(proc, axis=1), np.mean(dry, axis=1), t, t + 0.005, max(lo, 20.0), min(hi, 0.45 * SR))

def main():
    os.makedirs(PACK, exist_ok=True)
    for f in os.listdir(PACK):   # the previous pack is replaced
        if f.endswith(".wav") or f.endswith(".png"): os.remove(os.path.join(PACK, f))
    L = loops(); stereo = lambda v: np.stack([v, v], axis=1)
    k55, k70 = (stereo(norm_peak(L[k], 0.45)) for k in ("kick55", "kick70")); ss = stereo(norm_peak(L["snare"], 0.4)); bb = stereo(norm_peak(L["bus"])); vv = stereo(norm_peak(vocal_line(), 0.6))
    swap_x, swap_t = swap_loop(); sw = stereo(norm_peak(swap_x, 0.45))
    beat = 0.5; k_times = [k * beat for k in range(int(12.0 / beat) - 1)]; v_times = [0.2 + 0.7 * i for i in range(int(8.0 / 0.7) - 1)]
    n_audible = {"K55": len(k_times), "K70": len(k_times), "S": L["n_snares"], "R": L["n_snares"], "B": L["n_kicks"] + L["n_snares"], "V": 8, "SW": 64}
    items = []   # name, audio (before the level match), ref, params, spec, how it is matched, learn info, cut series, what the PNG needs
    def add(name, audio, ref, params, spec=None, match="lufs", learn=None, cuts=None, extra=None, viz=None):
        items.append(dict(name=name, audio=np.asarray(audio, np.float64), ref=ref, params=params, spec=spec, match=match, learn=learn, cuts=cuts, extra=extra or {}, viz=viz))
    A = "amount=9,width=1,sweet=120"
    for f0, kk, tag, base in ((55, k55, "K55", 1), (70, k70, "K70", 5)):
        add(f"{base:02d}_KICK {f0} Hz kuru", kk, tag, None, match=None)
        for j, part in enumerate(("sub", "body", "click")):
            spec = f"inst=kick,part={part},{A}"; y, cuts, lr = chain(kk, PRE["kick"], [spec])
            info = learned(lr[0], spec, n_audible[tag]); ld = info["parts"]
            # SUB and BODY are what the loudness of a kick is made of: a LUFS match would lift the whole file and the untouched click with it. Both are matched on the click band, which the cut does not touch.
            match = ("band", ld["CLICK"]["lo_hz"], ld["CLICK"]["hi_hz"], "click bandi (SUB/BODY kesimi buraya dokunmaz)") if part in ("sub", "body") else "lufs"
            add(f"{base + 1 + j:02d}_KICK {f0} Hz {part.upper()}", y, tag, f"KICK PART {part.upper()} AMOUNT 9 WIDTH 1 SWEET 120 SLOPE 24", spec, match=match, learn={"KICK": info}, cuts=cuts[0], extra=dict(part=part),
                viz=dict(kind="normal", inst="KICK", part=part.upper(), info=info, log=lr[0], times=k_times, rep=len(k_times) // 2))
    add("09_SNARE kuru", ss, "S", None, match=None)
    snare_part = {}
    for j, part in enumerate(("body", "crack")):
        spec = f"inst=snare,part={part},{A}"; y, cuts, lr = chain(ss, PRE["snare"], [spec]); ld = learned(lr[0], spec, n_audible["S"]); snare_part[part] = (y, ld)
        match = ("band", ld["parts"]["CRACK"]["lo_hz"], ld["parts"]["CRACK"]["hi_hz"], "crack bandi (BODY kesimi buraya dokunmaz)") if part == "body" else "lufs"
        add(f"{10 + j:02d}_SNARE {part.upper()}", y, "S", f"SNARE PART {part.upper()} AMOUNT 9 WIDTH 1 SWEET 120 SLOPE 24", spec, match=match, learn={"SNARE": ld}, cuts=cuts[0],
            viz=dict(kind="normal", inst="SNARE", part=part.upper(), info=ld, log=lr[0], times=L["st"], rep=len(L["st"]) // 2))
    rr = "inst=snare,part=body,amount=60,width=1,sweet=120,reverb=1"   # P1i: REVERB-READY is two LR edges at -AMOUNT; AMOUNT max (60) removes the band, the band-limit of the earlier packs
    y_rr, cuts_rr, lr_rr = chain(ss, PRE["snare"], [rr]); ld_rr = learned(lr_rr[0], rr, n_audible["S"]); cr = ld_rr["reverb_ready_corners_hz"]
    add("12_SNARE kuru -> reverb (1.5 s)", reverb(ss), "R", "dry snare through the synthetic reverb (demo only)", None, match=None)
    add("13_SNARE REVERB-READY -> ayni reverb (1.5 s)", reverb(y_rr), "R", "SNARE REVERB-READY AMOUNT 60 SLOPE 24, then the same demo reverb", rr, match="lufs", learn={"SNARE": ld_rr}, cuts=cuts_rr[0],
        viz=dict(kind="normal", inst="SNARE", part="REVERB-READY", info=ld_rr, log=lr_rr[0], times=L["st"], rep=len(L["st"]) // 2, dry_ref="R"))
    add("13b_SNARE REVERB-READY (reverbsiz)", y_rr, "S", "SNARE REVERB-READY AMOUNT 60 SLOPE 24, no reverb: the EQ alone", rr, match=("band", round(1.5 * cr["low_corner_hz"]), round(cr["high_corner_hz"] / 1.5), "gecis bandi (iki kesme noktasinin arasi)"),
        learn={"SNARE": ld_rr}, cuts=cuts_rr[0], viz=dict(kind="normal", inst="SNARE", part="REVERB-READY", info=ld_rr, log=lr_rr[0], times=L["st"], rep=len(L["st"]) // 2))
    add("14_DRUM BUS kuru", bb, "B", None, match=None)
    d1 = f"inst=drums,part=kickbody,{A}"; d2 = f"inst=drums,part=snarebody,{A}"
    y_bus, cuts_bus, lr_bus = chain(bb, PRE["bus"], [d1, d2]); i_k = learned(lr_bus[0], d1, n_audible["B"]); i_s = learned(lr_bus[1], d2, n_audible["B"])
    add("15_DRUM BUS KICK BODY + SNARE BODY (iki ornek seri)", y_bus, "B", "instance 1 DRUMS PART KICK BODY AMOUNT 9, instance 2 DRUMS PART SNARE BODY AMOUNT 9 (SLOPE 24)", d1, match=("band", 1500, 6000, "1.5-6 kHz (kick tiklamasi + snare crack, BODY kesimleri buraya dokunmaz)"),
        learn={"DRUMS / KICK BODY": i_k, "DRUMS / SNARE BODY": i_s}, cuts=np.maximum(cuts_bus[0], cuts_bus[1]), viz=dict(kind="bus", info=i_k, info2=i_s, log=lr_bus[0], times=L["kt"], rep=len(L["kt"]) // 2))
    # P1i: 16 / 17 the active-listening file (kick A, then kick B, one instance, TARGET selected throughout), 19 / 20 the same cuts at 72 dB/oct
    add("16_KICK sample degisimi kuru", sw, "SW", None, match=None)
    spec = f"inst=kick,part=sub,{A}"; y, cuts, lr = chain(sw, PRE["swap"], [spec]); info = learned(lr[0], spec, n_audible["SW"]); ld = info["parts"]
    add("17_KICK SUB sample degisimi (aktif dinleme)", y, "SW", "KICK PART SUB AMOUNT 9 SLOPE 24, kick A 8 olcu, kick B 8 olcu, ayni eklenti, durmadan", spec, match=("band", ld["CLICK"]["lo_hz"], ld["CLICK"]["hi_hz"], "click bandi (SUB kesimi buraya dokunmaz)"),
        learn={"KICK": info}, cuts=cuts[0], viz=dict(kind="swap", info=info, log=lr[0], times=swap_t, pre_s=PRE["swap"]))
    spec72 = f"inst=kick,part=sub,{A},slope=72"; y, cuts, lr = chain(k55, PRE["kick"], [spec72]); info72 = learned(lr[0], spec72, n_audible["K55"]); ld72 = info72["parts"]
    add("19_KICK 55 Hz SUB 72 dB-oct", y, "K55", "KICK PART SUB AMOUNT 9 WIDTH 1 SWEET 120 SLOPE 72 (02 ile ayni, SLOPE 24 yerine 72)", spec72, match=("band", ld72["CLICK"]["lo_hz"], ld72["CLICK"]["hi_hz"], "click bandi (SUB kesimi buraya dokunmaz)"),
        learn={"KICK": info72}, cuts=cuts[0], extra=dict(part="sub"), viz=dict(kind="normal", inst="KICK", part="SUB", info=info72, log=lr[0], times=k_times, rep=len(k_times) // 2))
    spec48 = f"inst=kick,part=sub,{A},slope=48"; y, cuts, lr = chain(k55, PRE["kick"], [spec48]); info48 = learned(lr[0], spec48, n_audible["K55"]); ld48 = info48["parts"]
    add("19b_KICK 55 Hz SUB 48 dB-oct", y, "K55", "KICK PART SUB AMOUNT 9 WIDTH 1 SWEET 120 SLOPE 48 (02 ve 19 ile ayni, SLOPE 24 / 72 yerine 48)", spec48, match=("band", ld48["CLICK"]["lo_hz"], ld48["CLICK"]["hi_hz"], "click bandi (SUB kesimi buraya dokunmaz)"),
        learn={"KICK": info48}, cuts=cuts[0], extra=dict(part="sub"), viz=dict(kind="normal", inst="KICK", part="SUB", info=info48, log=lr[0], times=k_times, rep=len(k_times) // 2))
    rr72 = "inst=snare,part=body,amount=60,width=1,sweet=120,reverb=1,slope=72"; y_r72, cuts_r72, lr_r72 = chain(ss, PRE["snare"], [rr72]); ld_r72 = learned(lr_r72[0], rr72, n_audible["S"]); c72 = ld_r72["reverb_ready_corners_hz"]
    add("20_SNARE REVERB-READY 72 dB-oct (reverbsiz)", y_r72, "S", "SNARE REVERB-READY AMOUNT 60 SLOPE 72, no reverb (13b ile ayni, SLOPE 24 yerine 72)", rr72, match=("band", round(1.5 * c72["low_corner_hz"]), round(c72["high_corner_hz"] / 1.5), "gecis bandi (iki kesme noktasinin arasi)"),
        learn={"SNARE": ld_r72}, cuts=cuts_r72[0], viz=dict(kind="normal", inst="SNARE", part="REVERB-READY", info=ld_r72, log=lr_r72[0], times=L["st"], rep=len(L["st"]) // 2))
    V = f"inst=vocal,part=%s,{A}"
    add("21_VOKAL kuru", vv, "V", None, match=None)
    for nm, part in (("22_VOKAL BOX", "box"), ("23_VOKAL S", "s")):
        y, cuts, lr = chain(vv, PRE["vocal"], [V % part]); infov = learned(lr[0], V % part, n_audible["V"])
        add(nm, y, "V", f"VOCAL PART {part.upper()} AMOUNT 9 WIDTH 1 SWEET 120", V % part, match="lufs", learn={"VOCAL": infov}, cuts=cuts[0], viz=dict(kind="normal", inst="VOKAL", part=part.upper(), info=infov, log=lr[0], times=v_times, rep=4))

    base = {"K55": "01_KICK 55 Hz kuru", "K70": "05_KICK 70 Hz kuru", "S": "09_SNARE kuru", "B": "14_DRUM BUS kuru", "V": "21_VOKAL kuru", "R": "12_SNARE kuru -> reverb (1.5 s)", "SW": "16_KICK sample degisimi kuru"}
    dry = {k: next(i["audio"] for i in items if i["name"] == v) for k, v in base.items()}
    ref_l = {k: lufs(a) for k, a in dry.items()}
    rows = []; final = {}
    for it in items:
        x = it["audio"]; l0 = lufs(x); m = it["match"]; gain = 0.0; how = "kuru dosya (referans)" if m is None else None
        if m == "lufs":
            gain = ref_l[it["ref"]] - l0; how = "LUFS (BS.1770 butunlesik)"
            for _ in range(6):   # P1k: the -70 LUFS absolute gate of BS.1770 makes loudness not exactly linear in the gain for a quiet, band-limited tail (13: one step left 0.6 LU): correct until it sits on the target
                err = ref_l[it["ref"]] - lufs(x * 10 ** (gain / 20))
                if abs(err) < 0.02: break
                gain += err
        elif m is not None:
            _, lo, hi, why = m; gain = band_rms_db(dry[it["ref"]], lo, hi) - band_rms_db(x, lo, hi); how = f"{lo:.0f}-{hi:.0f} Hz bandi: {why}"
        x = x * 10 ** (gain / 20); l1 = lufs(x); tp = true_peak(x); final[it["name"]] = x
        if tp >= -0.3: raise SystemExit(f"{it['name']}: true peak {tp:.2f} dBFS")
        if m == "lufs": assert abs(l1 - ref_l[it["ref"]]) <= 0.2, (it["name"], l1, ref_l[it["ref"]], "l0", l0, "gain", gain)
        if m not in (None, "lufs"): assert abs(band_rms_db(x, m[1], m[2]) - band_rms_db(dry[it["ref"]], m[1], m[2])) <= 0.1, it["name"]
        mm.write_wav24(os.path.join(PACK, it["name"] + ".wav"), x)
        row = dict(file=it["name"] + ".wav", lufs=round(l1, 2), lufs_vs_dry_db=None if it["name"] == base[it["ref"]] else round(l1 - ref_l[it["ref"]], 2), true_peak_dbfs=round(tp, 2), match=how, gain_applied_db=round(gain, 2),
                   matched_to=None if m is None else base[it["ref"]], params=it["params"], target_cut_db=None, learned=None, png=None)
        if it["spec"] is not None: row["target_cut_db"] = stats(it["cuts"]); row["learned"] = it["learn"]
        rows.append(row); print(f"{it['name']}: {l1:.2f} LUFS ({row['lufs_vs_dry_db']} vs dry), gain {gain:+.2f} dB, {how}, TP {tp:.2f} dBFS")
    # the cut on every audible hit of the kick files, in the part's own band, measured on the delivered files
    by = {r["file"]: r for r in rows}
    for it in items:
        if it["extra"].get("part"):
            tag = it["ref"]; part = it["extra"]["part"]; ld = it["learn"]["KICK"]; name = {"sub": "SUB", "body": "BODY", "click": "CLICK"}[part]; pd = ld["parts"][name]
            lo, hi = pd["lo_hz"], pd["hi_hz"]
            if part == "click":
                wins = {"0-5 ms": summarise([round(click_cut_db(dry[tag], final[it["name"]], t, lo, hi), 2) for t in k_times])}
            else:   # the pack kicks drop in pitch (about 110 Hz at 10 ms to the fundamental at 60 ms, the cut ramps at 1 dB/ms from the transient end): the first window includes that glide, the second is the settled part
                wins = {w: summarise(per_hit_cut(dry[tag], final[it["name"]], k_times, lo, hi, a, b)) for w, (a, b) in {"10-60 ms": (0.010, 0.060), "20-120 ms": (0.020, 0.120)}.items()}
            by[it["name"] + ".wav"]["per_hit_part_cut_db"] = dict(band_hz=[round(lo), round(hi)], windows_after_onset=wins)
    # the swap file: the sub band cut on every hit of each half, measured on the delivered file (the first 3 hits of kick B may still be cut at kick A's place: 8d 7 allows 3 hits)
    sw_row = by["17_KICK SUB sample degisimi (aktif dinleme)" + ".wav"]; sw_final = final["17_KICK SUB sample degisimi (aktif dinleme)"]; swl = items[[i["name"] for i in items].index("17_KICK SUB sample degisimi (aktif dinleme)")]["viz"]["log"]
    iA = int(np.searchsorted(swl["t_s"] - PRE["swap"], swap_t[31] + 0.3)); iB = len(swl["t_s"]) - 1; fA, fB = float(swl["f0"][iA]), float(swl["f0"][iB])
    sw_row["swap"] = dict(learned_sub_A_hz=round(fA, 2), learned_sub_B_hz=round(fB, 2), relearn_events=int(swl["relearns"][-1]), per_hit_sub_cut_db={
        "A (hits 4-32)": summarise(per_hit_cut(dry["SW"], sw_final, swap_t[3:32], fA / 2 ** (1 / 6), fA * 2 ** (1 / 6), 0.020, 0.120)),
        "B hits 1-3": summarise(per_hit_cut(dry["SW"], sw_final, swap_t[32:35], fB / 2 ** (1 / 6), fB * 2 ** (1 / 6), 0.020, 0.120)),
        "B (hits 4-32)": summarise(per_hit_cut(dry["SW"], sw_final, swap_t[35:], fB / 2 ** (1 / 6), fB * 2 ** (1 / 6), 0.020, 0.120))})
    # the form: one PNG per processed file
    worst_by = {}
    for it in items:
        if it["viz"] is None: continue
        v = it["viz"]; png = os.path.join(PACK, it["name"] + ".png"); spec = it["spec"]; kv = spec_kv(spec); slope = int(float(kv.get("slope", 24)))
        dref = dry[v.get("dry_ref", it["ref"])]; proc = final[it["name"]]; wdb = worst_band(dref, it["audio"], v["times"][5:] if v["kind"] == "swap" else (sorted(L["kt"] + L["st"]) if v["kind"] == "bus" else v["times"])); worst_by[it["name"]] = round(wdb, 2)
        if v["kind"] == "swap": png_swap(png, f"{it['name']}  |  kick A -> kick B, ayni eklenti, hafiza degistirilmeden: ogrenilen sub ve uygulanan kesim", spec, v["info"], v["log"], dref, proc, v["times"], v["pre_s"], worst_db=wdb)
        elif v["kind"] == "bus":
            ck, _ = cut_curve(GRID, "KICK", "BODY", d1, v["info"], lr_bus[0]); cs, _ = cut_curve(GRID, "SNARE", "BODY", d2, v["info2"], lr_bus[1])
            png_normal(png, f"{it['name']}  |  iki ornek: kick govdesi + snare govdesi (ayni anda gosterilir)", "DRUMS (kick govdesi)", "BODY", d1, v["info"], v["log"], dref, proc, v["times"], v["rep"], curve_override=ck + cs, worst_db=wdb,
                                                                                           extra_lines=[f"2. ornek: SNARE GOVDE {v['info2']['parts'].get('BODY', {}).get('centre_hz', 0):.0f} Hz, AMOUNT 9, SLOPE 24"], extra_marks=[("snare govdesi", v["info2"]["parts"]["BODY"]["centre_hz"])] if "BODY" in v["info2"]["parts"] else [])
        else: png_normal(png, f"{it['name']}  |  {v['inst']} {v['part']}  |  SLOPE {slope} dB/okt", v["inst"], v["part"], spec, v["info"], v["log"], dref, proc, v["times"], v["rep"], worst_db=wdb)
        by[it["name"] + ".wav"]["png"] = it["name"] + ".png"; by[it["name"] + ".wav"]["worst_band_db"] = worst_by[it["name"]]
    meta = dict(note="LUFS: integrated, BS.1770 (pyloudnorm). true_peak_dbfs: 4x oversampled. match: how the level was set against the dry file (kuru): LUFS (+-0.2) or, where the cut part is what carries the loudness "
                     "(kick SUB / BODY, snare BODY, the drum-bus BODY pair, the REVERB-READY snare without reverb), the level of a band the cut does not touch (+-0.1 dB). gain_applied_db is what the match added; lufs_vs_dry_db is the "
                     "loudness difference that remains (negative = the file is quieter than the dry one because the cut took energy away, which is the point). 12 is the dry snare through the demo reverb and is the reference of 13 "
                     "(the reverb adds level, so 12 and 13 are not matched to 09); 13b is the same EQ'd snare as 13 without the reverb. target_cut_db: the deepest TARGET cut (bell or edge) per 32-sample block over the AUDIBLE part (dB, positive = cut): mean, "
                     "max, share of blocks cut. per_hit_part_cut_db: the cut on each audible hit in the part's own band, measured on the delivered files. learned: what the engine found after the last hit; reverb_ready_corners_hz / "
                     "edge_crossovers_hz: the Linkwitz-Riley crossovers of the edge cuts. Every file starts with a pre-roll of the file's own first seconds rendered with TARGET OFF (the memory fills), trimmed from the file. "
                     "P1i: SLOPE 24 unless the name says 72 (19, 20); REVERB-READY at AMOUNT 60 (the band is removed outside the corners). 16 / 17: kick A (50 Hz) for 8 bars, kick B (70 Hz) for 8 bars, one instance, the memory is never reset. "
                     "Every processed file has a PNG of the same name (the form: spectra before / after, the cut curve, the learned points, one hit's waveform with the analysis windows).",
                preroll_seconds=PRE, truth=dict(kicks_in_bus=L["n_kicks"], snares_in_bus=L["n_snares"], hits_in_each_kick_loop=len(k_times), hits_in_snare_loop=L["n_snares"], hits_in_swap_file=64), files=rows)
    json.dump(meta, open(os.path.join(PACK, "olcum.json"), "w"), indent=1, ensure_ascii=False)
    t28 = M.t28_run(); meta["t28_neighbour_protected"] = [{k: (round(v, 2) if isinstance(v, float) else v) for k, v in r.items()} for r in t28]
    json.dump(meta, open(os.path.join(PACK, "olcum.json"), "w"), indent=1, ensure_ascii=False)
    write_oku(rows, L, t28)
    for nm in ("02_KICK 55 Hz SUB.wav", "06_KICK 70 Hz SUB.wav", "19_KICK 55 Hz SUB 72 dB-oct.wav", "19b_KICK 55 Hz SUB 48 dB-oct.wav"):
        for w, c in by[nm]["per_hit_part_cut_db"]["windows_after_onset"].items(): print(f"{nm}: per-hit sub cut, window {w} after the onset, {c['hits']} audible hits: worst {c['worst']}, best {c['best']}, spread {c['spread']} dB")
    print("swap:", json.dumps(sw_row["swap"])[:900])

def fmt_parts(d, names):
    out = []
    for n in names:
        if n in d["parts"]: p = d["parts"][n]; out.append(f"{n} {p['centre_hz']:.0f} Hz ({p['lo_hz']:.0f}-{p['hi_hz']:.0f})")
        else: out.append(f"{n} bulunamadi")
    return ", ".join(out)

def write_oku(rows, L, t28):
    by = {r["file"]: r for r in rows}
    row = lambda prefix: next(r for n, r in by.items() if n.startswith(prefix))
    win = lambda r, w: r["per_hit_part_cut_db"]["windows_after_onset"][w]
    k55 = row("02_")["learned"]["KICK"]; k70 = row("06_")["learned"]["KICK"]; sn = row("10_")["learned"]["SNARE"]; rc = row("13b_")["learned"]["SNARE"]["reverb_ready_corners_hz"]; rc72 = row("20_")["learned"]["SNARE"]["reverb_ready_corners_hz"]
    sw = row("17_"); swp = sw["swap"]; s02, s19 = win(row("02_"), "10-60 ms"), win(row("19_"), "10-60 ms"); t02, t19 = win(row("02_"), "20-120 ms"), win(row("19_"), "20-120 ms")
    bus = row("15_")["learned"]; vb = row("22_")["learned"]["VOCAL"]; vs = row("23_")["learned"]["VOCAL"]
    g = lambda r: f"{r['gain_applied_db']:+.2f} dB"
    cut3 = swp["per_hit_sub_cut_db"]
    lines = ["# DYNEQ - TARGET dinleme paketi (9 Ekim, P1i: SLOPE, kanit toplayici, aktif dinleme, sekil)", "",
             "Hepsi sentetik malzeme. Olcumler `olcum.json` icinde. Her islenmis dosyanin yaninda ayni adli bir **PNG** var: kesimin SEKLINI gosteriyor.", "",
             "## Ne degisti",
             "- **P1k (10 Ekim), tek satir: kenar kesimi (SUB, CLICK, REVERB-READY) artik yanindaki govdeyi yemiyor: kesme sinirini, govde merkezi en fazla 1 dB kaybedecek yere koyuyor; SLOPE dusukse sub kesimi daha sig kalir ve PNG'de 'SLOPE yetersiz' yazar.**",
             "- **P1j (9 Ekim): TARGET kesimi STATIK.** Vurus basina acilip kapanan kazanc kalkti: kesim, secim / AMOUNT / SLOPE / WIDTH degisene ya da TARGET kapanana kadar vurus arasinda da AMOUNT'ta kalir. Eski kesimin kapanip acilmasi sub'i genlik-module edip yan bantlar uretiyordu (02'de 150-600 Hz'e ses EKLIYORDU); PNG'lerdeki 'en kotu bant' kutusu bunun olcusu: islenmis dosya, kuruya gore hicbir 1/3 oktav bandinda +1.0 dB'den fazla yukari cikmaz (seviye eslemesinden once olculur).",
             "- **SLOPE (12 / 24 / 36 / 48 / 72 dB/okt):** SUB, CLICK ve REVERB-READY kesimleri artik bir Linkwitz-Riley bolucu: kesim bandi -AMOUNT dB, diger bant 0 dB. AMOUNT 60 = bant tamamen kalkar (gercek yuksek-gecir / alcak-gecir). Pakette hepsi SLOPE 24; 19 ve 20 ayni kesimin 72 hali.",
             "- **REVERB-READY** artik AMOUNT'a bakiyor (eskiden hep tam band siniri gibiydi). Pakette AMOUNT 60: sinirin disi kalkiyor. Alt sinir govdenin alt kenarinin 0.6 kati, ust sinir crack'in ust kenarinin 1.25 kati.",
             "- **Aktif dinleme:** motor kanalda ses degisirse (yeni kick sample'i) kendini duzeltiyor, hicbir seyi sifirlamadan. 17 bunu gosteriyor.",
             "- **Hafif kanit toplayici:** surekli analiz yok; bos zamanda sadece bir vurus bulucu calisiyor, vurus gelince tek bir kayit cikariyor. Duyulan fark beklenmiyor; olculen: ayni noktalari buluyor.",
             "- **Vokal dosyalari 16-18'den 21-23'e tasindi** (17, 19, 20 yeni dosyalara verildi). 16 artik 17'nin kuru referansi.", "",
             "## PNG nasil okunur (hepsinde ayni)",
             "- **Ust grafik:** gri = kuru ortalama vurus spektrumu, altin = islenmis (kick SUB/BODY, snare, vokal: vurustan sonraki 0-150 ms; CLICK, CRACK: ilk 5 ms). **Teal egri = uygulanan kesim** (sag eksen, dB). Teal golge = kesilen bolge. Beyaz noktali cizgiler = motorun ogrendigi noktalar (SUB, govde, tiklama...). Kesik teal cizgi = kesme siniri.",
             "- **Alt grafik:** bir vurusun ilk 150 ms'si, kuru (gri) ve islenmis (altin). Renkli seritler analiz pencereleri: vurus 0-5 ms, erken 5-40 ms, gec 40 ms ->. Kesik teal cizgi = ogrenilen vurus sonu.",
             "- **Kutu:** enstruman, parca, AMOUNT, SLOPE, ogrenilen Hz degerleri, parca merkezinde kesim (tam derinlikte).", "",
             "## Seviye eslemesi (P1h kurali, ayni)", "", "| dosya | nasil eslendi | uygulanan kazanc | kuruya gore LUFS farki |", "|---|---|---|---|"]
    for r in rows:
        if r["matched_to"] is None: continue
        lines.append(f"| {r['file']} | {r['match']} | {g(r)} | {r['lufs_vs_dry_db']:+.2f} LU |")
    lines += ["", "## Govdeye dokunmadan sub kesimi ne kadar derin (T28 tablosu, 02 / 06 / 19 / 19b malzemesi, vurus 6-son)",
              "Kesme siniri, govde merkezi en fazla 1 dB kaybedecek yere konuyor. 'sub merkezinde kesim' = o SLOPE'ta sub'in merkezinde gercekte ulasilan kesim (dB, olculen; parantez icinde motorun filtre cevabindan hesabi). 'govde kaybi' = govde merkezindeki olculen kayip (negatif = govde biraz yukselmis, tum-gecir fazi).", "",
              "| malzeme | AMOUNT | SLOPE | kesme siniri (Hz) | govde merkezi (Hz) | govde kaybi (dB) | sub merkezi (Hz) | sub merkezinde kesim (dB) | |", "|---|---|---|---|---|---|---|---|---|"]
    for r in t28:
        if r["name"].endswith(" SUB"): lines.append(f"| {r['name']} | {r['amount']} | {r['slope']} | {r['placed']:.0f} | {r['prot_c']:.0f} | {r['loss_meas']:+.2f} | {r['ref']:.0f} | {r['ach_meas']:.1f} ({r['ach_eng']:.1f}) | {'SLOPE yetersiz' if r['gentle'] else ''} |")
    lines += ["", "(12, 13'un referansi; 13b / 20 kuru snare 09'a gore gecis bandindan eslendi; 17 kuru 16'ya gore tiklama bandindan eslendi.)", "",
              "## Kick (01-08, 19)",
              f"01-04: 55 Hz kick (kuru, SUB, BODY, CLICK). 05-08: 70 Hz kick. Bulunan: 55 Hz kick temel {k55['fundamental_hz']:.1f} Hz, 70 Hz kick temel {k70['fundamental_hz']:.1f} Hz (bu kickler vurulunca perde dusuruyor, temel 60. ms'de olculuyor).", "",
              f"- **02 / 06 SUB:** dinle: alt bolge inceliyor, tiklama ayni. Bak: teal egri ~{k55['edge_crossovers_hz']['below_the_edge (SUB)']:.0f} Hz'den asagisini yumusakca indiriyor; altin egri o bolgede grinin altinda. 02'de vurus basina SUB kesimi (10-60 ms): {s02['worst']:.1f} dB, 23 vurusun hepsi ayni ({s02['spread']:.2f} dB yayilim); oturmus kisim (20-120 ms): {t02['worst']:.1f} dB.",
              "- **03 / 07 BODY:** dinle: tiklamanin hemen arkasindaki 'yumruk' bolgesi inceliyor, sub ayni. Bak: teal egride dar bir cukur (SOFT edge, bu hala bir bell), cukurun altinda govde cizgisi.",
              "- **04 / 08 CLICK:** dinle: tiklama yumusuyor, alt bolge degismiyor. Bak (ilk 5 ms spektrumu): ~1.8 kHz'in ustunde teal egri ve altin egri gridden asagi.",
              f"- **19 SUB 72 dB-oct:** 02 ile ayni kesim, sadece SLOPE 72. Dinle: 02 ile 19'u art arda cal; 19'da kesim daha keskin, sub bandinda daha derin. Bak: teal egri 02'dekinden cok daha dik. Vurus basina SUB kesimi (10-60 ms): 02'de {s02['worst']:.1f} dB, 19'da {s19['worst']:.1f} dB; oturmus kisimda 02 {t02['worst']:.1f} dB, 19 {t19['worst']:.1f} dB.",
              "- **19b SUB 48 dB-oct:** 02 (24) ve 19 (72) ile ayni kesim, SLOPE 48. Dinle: 02, 19b, 19'u art arda cal; sub'in inmesi 24'ten 72'ye kademeli olmali, govde (~100 Hz) ucunde de ayni kalmali. Sayilar yukaridaki tabloda.", "",
              "## Aktif dinleme (16, 17)",
              f"16 kuru: 8 olcu kick A (50 Hz), 8 olcu kick B (70 Hz, baska sample), her vurus. 17: ayni dosya, tek eklenti, KICK SUB, hicbir seyi sifirlamadan. Motorun buldugu sub: A {swp['learned_sub_A_hz']:.1f} Hz, B {swp['learned_sub_B_hz']:.1f} Hz; yeniden ogrenme olayi: {swp['relearn_events']}. "
              f"Vurus basina SUB kesimi (kendi sub bandinda, 20-120 ms): A {cut3['A (hits 4-32)']['worst']:.1f} dB (hepsi ayni), B'nin ilk 3 vurusu {', '.join(f'{c:.1f}' for c in cut3['B hits 1-3']['cuts'])} dB, B'nin 4. vurusundan sonra {cut3['B (hits 4-32)']['worst']:.1f} dB.", "",
              "- Dinle: 9. olcuden (16. sn) sonra kick degisiyor. Ilk iki B vurusunda kesim hala A'nin yerinde (B'nin sub'una az dokunur, yukaridaki ilk iki sayi); 3. vurustan itibaren B'nin sub'una oturur. Kesim 1 dB/ms'den hizli degismez, atlama yok.",
              "- Bak (17.png): alt grafikte altin basamak 51 Hz'den 72 Hz'e ~16.8 sn'de (sample degisiminden 2 vurus sonra) atliyor; teal cizgi her vurustaki kesim derinligi (9 dB). Ust grafikte iki ogrenilen sub (A, B) ve iki kesim egrisi.", "",
              "## Snare (09-13b, 20)",
              f"09 kuru, 10 BODY, 11 CRACK. Bulunan: {fmt_parts(sn, ('BODY', 'CRACK', 'SNAP/AIR'))}.", "",
              "- **10 BODY:** dinle: halka (govde) inceliyor, 'tak' yerinde. Bak: teal egride dar cukur govde cizgisinin ustunde.",
              "- **11 CRACK:** dinle: crack bolgesi inceliyor. Bak: cukur crack cizgisinin ustunde.",
              f"- **12 / 13 / 13b:** 12 kuru snare + yapay reverb (1.5 sn), 13 REVERB-READY snare + ayni reverb, **13b REVERB-READY, reverbsiz.** Reverb yalniz gostermek icin; EQ reverb eklemez. Sinirlar: alt {rc['low_corner_hz']:.0f} Hz, ust {rc['high_corner_hz']:.0f} Hz (SLOPE 24, AMOUNT 60). Bak (13b.png): iki kesik teal cizgi, aralarinda teal egri duz, disinda iniyor; altin egri iki yanda grinin altinda.",
              f"- **20 (72 dB-oct):** 13b ile ayni, SLOPE 72. Sinirlar {rc72['low_corner_hz']:.0f} / {rc72['high_corner_hz']:.0f} Hz. Dinle: 13b ile 20'yi karsilastir; 72'de kenarlar daha keskin, kesim sinirinda 'ziller' (halkalanma) duyulabilir. Bak (20.png): alt sinir civarinda altin egride tumsek: keskin bir filtre kendi kesim frekansinda calar; ust grafikte teal egri dikdortgene yakin.",
              "- Not: bu sentetik snare'de motor crack'i 1 kHz civarina oturtuyor, bu yuzden ust sinir ~1.6 kHz ve 13b / 20 karanlik duyulur. Gercek snare'de farkli cikar.", "",
              "## Drum bus (14-15)",
              f"14 kuru, 15 DRUMS / KICK BODY + DRUMS / SNARE BODY (iki ornek arka arkaya). Kick govdesi: {fmt_parts(bus['DRUMS / KICK BODY'], ('BODY',))}; snare govdesi: {fmt_parts(bus['DRUMS / SNARE BODY'], ('BODY',))}. Bak (15.png): teal egride iki cukur (kick govdesi ve snare govdesi), ikisi toplanmis.", "",
              "## Vokal benzeri hat (21-23)",
              f"21 kuru, 22 BOX, 23 S. Ses formant sentezi (gercek vokal degil). BOX: {fmt_parts(vb, ('BOX',))}; S: {fmt_parts(vs, ('S',))}. 22 ve 23 LUFS ile eslendi (22: {g(row('22_'))}, 23: {g(row('23_'))}). Dinle: 22'de unluler 'kutu' olmaktan cikti mi? 23'te 's' patlamalari yumusadi mi? Bak: teal cukur BOX / S bandinda.", "",
              "## Genel",
              "- Hangi dosyada kesim hemen duyuluyor, hangisinde sadece grafikte gorunuyor?",
              "- 24 ile 72'yi karsilastirinca fark kabul edilebilir mi, yoksa 72 'zil' mi yapiyor?",
              "- Bu paketteki hicbir dosya dinlenmedi (ben dinleyemem); her sey olculdu ve PNG'lerde goruldu. Karar senin kulagin.", ""]
    open(os.path.join(PACK, "OKU.md"), "w").write("\n".join(lines))

if __name__ == "__main__":
    main()
