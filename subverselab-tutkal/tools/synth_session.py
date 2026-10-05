#!/usr/local/bin/python3
"""A clean, dry, deterministic synthetic multitrack for judging a compressor (TUTKAL listening material, pack v3).

48 kHz, 100 bpm, 16 bars (38.4 s), seeded. Room-less and uncompressed: every stem is a plain synthesis with per-hit velocity
(level and brightness both follow it) and a few milliseconds of human timing. Stems: kick, snare (body + noise), hats (closed + open,
choked), toms (fill at the end of bar 8 and 16), bass (sub + bass line), pad (chords), lead (pluck, from bar 5).

Derived dry signals (sums of the exported stems, no processing):
  drumbus        kick + snare + hats + toms
  fullmix        every stem
  padbass        pad + bass                (PUMP: keyed by KEY_kick)
  backing        pad + bass + soft drums   (no toms, no lead)
  vocal_in_backing  the house test vocal (sung, 20 s, 44.1 kHz) sitting in `backing`, played twice (8 bars + 8 bars)

  synth_session.py [out_dir]     writes stems + dry mixes + events.json + sanity.json, prints the sanity table; exit code = failed sanity checks
"""
import json, os, sys
import numpy as np
import scipy.signal as ss
import soundfile as sf

SR = 48000
BPM = 100.0
BEAT = 60.0 / BPM                       # 0.6 s
STEP = BEAT / 4.0                       # a 16th, 0.15 s
BAR = 4 * BEAT                          # 2.4 s
BARS = 16
N = int(round(BARS * BAR * SR))
HERE = os.path.dirname(os.path.abspath(__file__))
VOCAL = os.path.join(HERE, "..", "..", "prototypes", "_kaynak", "Kadin_vokal_20s.wav")
STEMS = ("kick", "snare", "hats", "toms", "bass", "pad", "lead")
# Balance in dB before the session gain: peaks of the stems against each other (the session gain then puts the full mix at -3 dBFS).
BALANCE = {"kick": 0.0, "snare": -1.0, "hats": -13.0, "toms": -2.0, "bass": -3.5, "pad": -9.0, "lead": -9.0}

def db(a): return 20 * np.log10(np.maximum(np.abs(a), 1e-12))
def amp(d): return 10 ** (d / 20)

# -----------------------------------------------------------------------------------------------------------------
class Track:
    def __init__(self): self.buf = np.zeros((N, 2), np.float64)
    def add(self, t, sig, pan=0.0, gain=1.0):
        """sig mono, placed at time t (s) with constant-power pan (-1 .. +1)."""
        s = int(round(t * SR))
        if s < 0: sig = sig[-s:]; s = 0                       # a note pushed before the first sample by its timing jitter
        e = min(N, s + len(sig))
        if s >= N or e <= s: return
        a = (pan + 1.0) * np.pi / 4.0
        seg = sig[:e - s] * gain
        self.buf[s:e, 0] += seg * np.cos(a) * np.sqrt(2.0); self.buf[s:e, 1] += seg * np.sin(a) * np.sqrt(2.0)

def tt(secs): return np.arange(int(secs * SR)) / SR
def fade_tail(x, ms=25.0):
    n = int(ms * SR / 1000); x = x.copy(); x[-n:] *= np.linspace(1, 0, n) ** 2; return x
def noise(rng, n): return rng.standard_normal(n)
def bp(x, lo, hi, order=2): return ss.sosfilt(ss.butter(order, [lo, hi], "bandpass", fs=SR, output="sos"), x)
def hp(x, f, order=2): return ss.sosfilt(ss.butter(order, f, "highpass", fs=SR, output="sos"), x)
def lp(x, f, order=2): return ss.sosfilt(ss.butter(order, f, "lowpass", fs=SR, output="sos"), x)

# ---- the drum voices
def kick(v, rng):
    t = tt(0.55)
    f = 46.0 + (140.0 + 40.0 * v - 46.0) * np.exp(-t / 0.028)
    body = np.sin(2 * np.pi * np.cumsum(f) / SR) * (1 - np.exp(-t / 0.0007)) * np.exp(-t / 0.15)
    click = bp(noise(rng, len(t)), 1800, 6500) * np.exp(-t / 0.0028) * 0.22 * v * v
    return fade_tail(v * body + click)

def snare(v, rng):
    t = tt(0.5)
    f1 = 180.0 + 38.0 * np.exp(-t / 0.012); f2 = 335.0 + 60.0 * np.exp(-t / 0.010)
    body = 0.62 * np.sin(2 * np.pi * np.cumsum(f1) / SR) * np.exp(-t / 0.065) + 0.28 * np.sin(2 * np.pi * np.cumsum(f2) / SR) * np.exp(-t / 0.040)
    n = noise(rng, len(t))
    wires = bp(n, 1300, 9500) * (0.75 * np.exp(-t / 0.105) + 0.5 * np.exp(-t / 0.012))
    crack = hp(noise(rng, len(t)), 3500) * np.exp(-t / 0.006) * 0.4 * v
    return fade_tail(v * 0.8 * body + (v ** 1.3) * 0.9 * wires + crack)

_SQ = (205.3, 304.4, 369.6, 522.7, 540.0, 800.0)
def hat(v, rng, open_=False, choke=None):
    n = int((0.55 if open_ else 0.16) * SR); t = np.arange(n) / SR
    sq = sum(np.sign(np.sin(2 * np.pi * f * 2.6 * t + rng.uniform(0, 6.28))) for f in _SQ) / len(_SQ)
    s = hp(0.55 * sq + 0.65 * noise(rng, n), 6800, 4)
    tau = 0.2 if open_ else 0.026
    y = v * s * (np.exp(-t / tau) * (1 - np.exp(-t / 0.0004)))
    if choke is not None:
        c = int(choke * SR)
        if c < n: y[c:] *= 0; y[max(0, c - 120):c] *= np.linspace(1, 0, min(120, c))[:c - max(0, c - 120)]
    return fade_tail(y[:n], 8.0) * 0.45

_TOMS = {"hi": (235.0, 175.0, 0.16), "mid": (172.0, 128.0, 0.19), "lo": (132.0, 96.0, 0.23)}
def tom(v, rng, which):
    f0, f1, tau = _TOMS[which]; t = tt(0.6)
    f = f1 + (f0 + 25 * v - f1) * np.exp(-t / 0.04)
    body = np.sin(2 * np.pi * np.cumsum(f) / SR) * (1 - np.exp(-t / 0.0008)) * np.exp(-t / tau)
    click = bp(noise(rng, len(t)), 1500, 5000) * np.exp(-t / 0.004) * 0.25 * v * v
    return fade_tail(v * body + click)

# ---- tonal voices
_TABLES = {}
def saw_cycle(maxf, size=4096):
    """One cycle of a band-limited saw with harmonics up to maxf (Hz)."""
    h = int(maxf)
    if h not in _TABLES:
        ph = np.arange(size) / size * 2 * np.pi
        k = np.arange(1, h + 1)[:, None]
        _TABLES[h] = (np.sin(k * ph[None, :]) / k).sum(0) * 0.6
    return _TABLES[h]

def saw(f, n, phase=0.0, lfo=None):
    tab = saw_cycle(max(1, min(int(11000.0 / f), 160)))
    inc = f / SR * (lfo if lfo is not None else 1.0)
    ph = (phase + np.cumsum(inc * np.ones(n))) % 1.0
    idx = ph * len(tab); i0 = idx.astype(int) % len(tab); fr = idx - np.floor(idx)
    return tab[i0] * (1 - fr) + tab[(i0 + 1) % len(tab)] * fr

def adsr(n, a, r, hold):
    """attack a s, held for `hold` s from the start, release r s; n samples in all."""
    e = np.ones(n); na = max(1, int(a * SR)); e[:na] = np.linspace(0, 1, na) ** 2
    nh = int(hold * SR)
    if nh < n:
        nr = n - nh; e[nh:] = np.exp(-np.arange(nr) / (r * SR) * 4.0) * (1 - np.arange(nr) / nr)
    return e

def hz(m): return 440.0 * 2 ** ((m - 69) / 12.0)

# chords, 2 bars each; (bass root midi for the sub, pad notes)
CHORDS = [(33, (45, 48, 52, 55, 64)),   # Am7
          (29, (41, 45, 48, 52, 60)),   # Fmaj7
          (36, (48, 52, 55, 59, 67)),   # Cmaj7
          (31, (43, 47, 50, 53, 62))]   # G7
def chord_at(bar): return CHORDS[(bar // 2) % 4]

def build_bass(rng, events):
    tr = Track(); pat = [(0, 1.0, 0.85), (3, 1.0, 0.7), (6, 2.0, 0.78), (8, 1.0, 0.8), (10, 1.0, 0.72), (14, 1.0, 0.82)]
    for bar in range(BARS):
        root = chord_at(bar)[0]
        for k, (st, octv, vel) in enumerate(pat):
            nxt = pat[k + 1][0] if k + 1 < len(pat) else 16
            dur = (nxt - st) * STEP * 0.92
            v = float(np.clip(vel + rng.normal(0, 0.06), 0.5, 1.0)); t0 = bar * BAR + st * STEP + rng.normal(0, 0.001)
            n = int((dur + 0.06) * SR); t = np.arange(n) / SR; f = hz(root + (12 if octv == 2.0 and st == 6 else 0))
            sub = np.sin(2 * np.pi * f * t)
            body = sum(np.sin(2 * np.pi * f * 2 * h * 0.5 * t) / h ** 1.2 for h in (2, 3, 4, 5)) * 0.5 * v
            e = adsr(n, 0.004, 0.05, dur)
            tr.add(t0, (0.9 * sub + body) * e, 0.0, v)
            events.append(dict(t=round(t0, 4), inst="bass", vel=round(v, 3), note=int(root)))
    return tr.buf

def build_pad(rng):
    tr = Track(); rng2 = np.random.default_rng(1001)
    for c in range(BARS // 2):
        notes = CHORDS[c % 4][1]; dur = 2 * BAR; nrel = 0.9
        n = int((dur + nrel) * SR); e = adsr(n, 0.45, 0.45, dur)
        for m in notes:
            f0 = hz(m)
            for det, pan in ((-0.14, -0.7), (0.0, 0.0), (0.12, 0.7)):
                f = f0 * 2 ** (det / 12.0); ph0 = rng2.uniform(); depth = 0.0014
                lfo = 1.0 + depth * np.sin(2 * np.pi * (0.11 + 0.07 * rng2.uniform()) * np.arange(n) / SR + rng2.uniform(0, 6.28))
                s = saw(f, n, ph0, lfo) * e
                tr.add(c * dur, s, pan, 0.18)
    y = tr.buf
    y = np.stack([lp(y[:, i], 1500, 2) for i in range(2)], axis=1)         # a soft top end, no resonance
    return y

LEAD = [  # (step in a 2-bar phrase of 32 steps, midi, length in steps, vel)
    (0, 69, 3, .85), (3, 72, 2, .7), (6, 76, 2, .8), (8, 72, 3, .65), (12, 67, 2, .75), (14, 69, 4, .7),
    (18, 64, 2, .6), (20, 67, 2, .72), (22, 69, 2, .78), (24, 72, 3, .85), (28, 76, 2, .7), (30, 79, 2, .9)]
def pluck(v, midi, dur, rng):
    n = int((max(dur, 0.35) + 0.5) * SR); t = np.arange(n) / SR; f = hz(midi); y = np.zeros(n)
    for h in range(1, 15):
        if f * h > 14000: break
        tau = 0.42 / (1.0 + (0.6 + 0.9 * (1 - v)) * h)
        y += np.sin(2 * np.pi * f * h * t + rng.uniform(0, 0.3)) / h ** 1.05 * np.exp(-t / tau)
    y += bp(noise(rng, n), 2000, 7000) * np.exp(-t / 0.004) * 0.05 * v
    return fade_tail(y * (1 - np.exp(-t / 0.0006)) * v, 40.0)

def build_lead(rng, events):
    tr = Track()
    for bar in range(4, BARS, 2):
        for st, m, ln, vel in LEAD:
            if bar >= 14 and st > 22 and rng.uniform() < 0.3: continue
            v = float(np.clip(vel * (0.85 + 0.05 * (bar // 2 % 3)) + rng.normal(0, 0.05), 0.4, 1.0)); t0 = bar * BAR + st * STEP + rng.normal(0, 0.002)
            y = pluck(v, m, ln * STEP, rng); tr.add(t0, y, -0.25)
            tr.add(t0 + 0.0, pluck(v * 0.8, m + 0.07, ln * STEP, rng), 0.3, 0.45)
            events.append(dict(t=round(t0, 4), inst="lead", vel=round(v, 3), note=int(m)))
    return tr.buf

def build_drums(rng, events):
    K, S, H, T = Track(), Track(), Track(), Track()
    def ev(t, inst, v): events.append(dict(t=round(t, 4), inst=inst, vel=round(float(v), 3)))
    for bar in range(BARS):
        b0 = bar * BAR
        fill = bar % 8 == 7
        kicks = {0: .95, 7: .78, 10: .85}
        if bar % 2 == 1: kicks[13] = .7
        if bar % 4 == 3: kicks.pop(7, None); kicks[6] = .75
        if fill: kicks.pop(13, None); kicks.pop(10, None)
        for st, base in kicks.items():
            v = float(np.clip(base + rng.normal(0, 0.06), 0.45, 1.0)); t = b0 + st * STEP + rng.normal(0, 0.0004)
            K.add(t, kick(v, rng), 0.0); ev(t, "kick", v)
        snares = {4: .9, 12: .9}
        ghosts = {7: .27, 15: .25} if bar % 2 == 0 else {9: .24, 14: .3}
        if fill: snares.pop(12, None)
        for st, base in snares.items():
            v = float(np.clip(base + rng.normal(0, 0.06), 0.55, 1.0)); t = b0 + st * STEP + 0.0015 + rng.normal(0, 0.0008)
            S.add(t, snare(v, rng), 0.05); ev(t, "snare", v)
        for st, base in ghosts.items():
            if fill and st >= 12: continue
            v = float(np.clip(base + rng.normal(0, 0.05), 0.15, 0.4)); t = b0 + st * STEP + 0.0015 + rng.normal(0, 0.001)
            S.add(t, snare(v, rng), 0.05); ev(t, "snare", v)
        # hats: 8ths with accented offbeat 16ths, an open hat on the "and" of 4 in odd bars (choked by the next closed hat or the bar line)
        acc = [1.0, .45, .75, .45]
        for st in range(16):
            if fill and st >= 12: continue
            if st % 2 == 1 and not (bar % 4 == 3 or st in (3, 11, 15)): continue
            v = float(np.clip(0.55 * acc[st % 4] + rng.normal(0, 0.06), 0.15, 0.8)); t = b0 + st * STEP + rng.normal(0, 0.0015)
            if bar % 2 == 1 and st == 14:
                H.add(t, hat(0.72, rng, open_=True, choke=STEP * 2), 0.35); ev(t, "openhat", 0.72); continue
            if bar % 2 == 1 and st == 15: continue
            H.add(t, hat(v, rng), 0.35); ev(t, "hat", v)
        if fill:                                           # tom fill on the last beat: 16ths, crescendo, high to low
            for k, which in enumerate(("hi", "hi", "mid", "lo")):
                v = float(np.clip(0.6 + 0.11 * k + rng.normal(0, 0.04), 0.4, 1.0)); t = b0 + (12 + k) * STEP + rng.normal(0, 0.001)
                T.add(t, tom(v, rng, which), {"hi": -0.3, "mid": 0.0, "lo": 0.3}[which]); ev(t, "tom", v)
    return K.buf, S.buf, H.buf, T.buf

# -----------------------------------------------------------------------------------------------------------------
_CACHE = {}
def build(seed=7):
    """-> dict(stems={name: (N, 2) float32}, events=[...], gain_db=session gain). Deterministic."""
    if seed in _CACHE: return _CACHE[seed]
    rng = np.random.default_rng(seed); events = []
    k, s, h, t = build_drums(rng, events)
    raw = {"kick": k, "snare": s, "hats": h, "toms": t, "bass": build_bass(rng, events), "pad": build_pad(rng), "lead": build_lead(rng, events)}
    stems = {}
    for name in STEMS:
        x = raw[name]; x = x / np.max(np.abs(x)) * amp(BALANCE[name])
        stems[name] = x
    g = amp(-3.0) / np.max(np.abs(sum(stems.values())))
    stems = {n: (x * g).astype(np.float32) for n, x in stems.items()}
    events.sort(key=lambda e: e["t"])
    out = dict(stems=stems, events=events, gain_db=float(db(g)))
    _CACHE[seed] = out
    return out

def drumbus(S): return (S["kick"] + S["snare"] + S["hats"] + S["toms"]).astype(np.float32)
def fullmix(S): return sum(S[n] for n in STEMS).astype(np.float32)
def four_floor(S, seed=21):
    """The same kick voice on every beat of the 16 bars (a classic four-on-the-floor key for PUMP), peak equal to the session kick's."""
    rng = np.random.default_rng(seed); tr = Track()
    for b in range(BARS * 4): tr.add(b * BEAT, kick(0.95, rng), 0.0)
    y = tr.buf; return (y / np.max(np.abs(y)) * np.max(np.abs(S["kick"]))).astype(np.float32)

def padbass(S): return (S["pad"] + S["bass"]).astype(np.float32)
def backing(S): return (S["pad"] + S["bass"] + 0.5 * S["kick"] + 0.35 * S["snare"] + 0.6 * S["hats"]).astype(np.float32)

def vocal_track():
    """The house vocal: mono 44.1 kHz singing, 0.4 .. 19.6 s, resampled to 48 kHz, 15 ms fades -> 8 bars (19.2 s), stereo."""
    x, sr = sf.read(VOCAL, dtype="float64", always_2d=True); x = x[:, 0]
    x = x[int(0.4 * sr):int(19.6 * sr)]
    x = ss.resample_poly(x, SR * 1, sr)
    n = int(8 * BAR * SR); x = x[:n] if len(x) >= n else np.concatenate([x, np.zeros(n - len(x))])
    f = int(0.015 * SR); x[:f] *= np.linspace(0, 1, f); x[-f:] *= np.linspace(1, 0, f)
    return x

def vocal_in_backing(S, ahead_db=2.0):
    """Vocal twice (bars 1-8, bars 9-16) over the backing. Returns (mix, vocal_only, backing, vocal_gain_db)."""
    v1 = vocal_track(); v = np.concatenate([v1, v1])[:N]
    b = backing(S).astype(np.float64)
    frame = int(0.1 * SR); k = len(v) // frame
    rv = np.sqrt(np.mean(v[:k * frame].reshape(k, frame) ** 2, axis=1)); live = rv > amp(-48.0)
    rb = np.sqrt(np.mean(b[:k * frame].reshape(k, frame, 2) ** 2, axis=(1, 2)))
    g = amp(db(np.mean(rb)) + ahead_db) / np.median(rv[live])      # median sung frame sits `ahead_db` over the backing's mean level
    voc = np.stack([v, v], axis=1) * g
    mix = b + voc
    s = amp(-3.0) / float(np.max(np.abs(mix)))                      # the whole scene (vocal and backing alike) goes to -3 dBFS, balance untouched
    return (mix * s).astype(np.float32), (voc * s).astype(np.float32), (b * s).astype(np.float32), float(db(g * s))

# -----------------------------------------------------------------------------------------------------------------
def bands(x, edges=(0, 100, 150, 500, 2000, 6000, 24000)):
    m = x.mean(axis=1) if x.ndim == 2 else x
    S = np.abs(np.fft.rfft(m * np.hanning(len(m)))) ** 2; f = np.fft.rfftfreq(len(m), 1 / SR); tot = S.sum() + 1e-30
    return [float(S[(f >= a) & (f < b)].sum() / tot) for a, b in zip(edges[:-1], edges[1:])]

def stats(x):
    pk = float(np.max(np.abs(x))); r = float(np.sqrt(np.mean(x.astype(np.float64) ** 2)))
    return dict(peak_db=float(db(pk)), rms_db=float(db(r)), crest_db=float(db(pk) - db(r)), dc=float(np.mean(x)), finite=bool(np.all(np.isfinite(x))),
                clipped=int(np.sum(np.abs(x) >= 0.999)), bands=bands(x))

def sanity(S, events):
    """Per-stem sanity and spectral-content checks. Returns (rows, failures)."""
    rows, fails = [], []
    def check(name, ok, text):
        rows.append((name, ok, text));
        if not ok: fails.append(name)
    st = {n: stats(S[n]) for n in STEMS}
    for n in STEMS:
        s = st[n]
        check(f"{n}: clean", s["finite"] and s["clipped"] == 0 and s["peak_db"] < -2.0 and abs(s["dc"]) < 0.005 * 10 ** (s["peak_db"] / 20),
              f"peak {s['peak_db']:.1f} dBFS, RMS {s['rms_db']:.1f}, crest {s['crest_db']:.1f} dB, DC {s['dc']:.1e}, clipped samples {s['clipped']}")
    b = {n: st[n]["bands"] for n in STEMS}                  # <100, 100-150, 150-500, 500-2k, 2k-6k, >6k
    check("kick: low", b["kick"][0] + b["kick"][1] >= 0.80, f"{100 * (b['kick'][0] + b['kick'][1]):.0f} % of the energy below 150 Hz")
    check("snare: body and noise", sum(b["snare"][1:3]) >= 0.2 and sum(b["snare"][3:]) >= 0.35, f"150-500 Hz {100 * sum(b['snare'][1:3]):.0f} %, above 500 Hz {100 * sum(b['snare'][3:]):.0f} %")
    check("hats: high", sum(b["hats"][4:]) >= 0.90 and b["hats"][5] >= 0.4, f"{100 * sum(b['hats'][4:]):.0f} % above 2 kHz, {100 * b['hats'][5]:.0f} % above 6 kHz")
    check("toms: mid-low", sum(b["toms"][:3]) >= 0.80, f"{100 * sum(b['toms'][:3]):.0f} % below 500 Hz")
    check("bass: low", sum(b["bass"][:3]) >= 0.85, f"{100 * sum(b['bass'][:3]):.0f} % below 500 Hz, {100 * b['bass'][0]:.0f} % below 100 Hz")
    check("pad: mid", sum(b["pad"][1:5]) >= 0.85 and b["pad"][5] <= 0.02, f"{100 * sum(b['pad'][1:5]):.0f} % in 100 Hz - 6 kHz, above 6 kHz {100 * b['pad'][5]:.2f} %")
    check("lead: mid-high", sum(b["lead"][3:5]) >= 0.5 and sum(b["lead"][:2]) <= 0.1, f"{100 * sum(b['lead'][3:5]):.0f} % in 500 Hz - 6 kHz, below 150 Hz {100 * sum(b['lead'][:2]):.0f} %")
    dyn = {n: st[n]["crest_db"] for n in ("kick", "snare", "hats", "toms")}
    check("drum crest", all(v >= 9.0 for v in dyn.values()), "crest " + ", ".join(f"{n} {v:.1f}" for n, v in dyn.items()) + " dB (uncompressed drums: >= 9 dB)")
    check("pad crest", st["pad"]["crest_db"] <= 14.0, f"pad crest {st['pad']['crest_db']:.1f} dB (sustained: <= 14)")
    mixes = {"drumbus": drumbus(S), "fullmix": fullmix(S), "padbass": padbass(S), "backing": backing(S)}
    vmix, vo, bk, vg = vocal_in_backing(S)
    mixes["vocal_in_backing"] = vmix
    for n, x in mixes.items():
        s = stats(x); check(f"{n}: clean", s["finite"] and s["clipped"] == 0 and s["peak_db"] < -1.0, f"peak {s['peak_db']:.1f} dBFS, RMS {s['rms_db']:.1f}, crest {s['crest_db']:.1f} dB")
    # onsets: every kick/snare event shows energy above the surrounding 40 ms; velocity spread is real
    for inst in ("kick", "snare"):
        ev = [e for e in events if e["inst"] == inst]; x = np.abs(S[inst]).max(axis=1); ok = True; v = [e["vel"] for e in ev]
        for e in ev:
            s0 = int(e["t"] * SR)
            if s0 + 480 < N and x[s0:s0 + 480].max() < 0.005: ok = False
        check(f"{inst}: events", ok and (max(v) - min(v)) >= 0.25 if inst == "snare" else ok and (max(v) - min(v)) >= 0.15,
              f"{len(ev)} hits, velocity {min(v):.2f} .. {max(v):.2f}, every hit audible: {ok}")
    # the vocal is real singing: pitched frames dominate the active frames, in a vocal range, with the level swing the riding needs
    v1 = vocal_track(); fr = int(0.1 * SR); k = len(v1) // fr; r = np.sqrt(np.mean(v1[:k * fr].reshape(k, fr) ** 2, axis=1)); live = r > amp(-45.0); pitched = 0; f0s = []
    for i in np.where(live)[0]:
        seg = v1[i * fr:(i + 1) * fr]; a = np.correlate(seg, seg, "full")[len(seg) - 1:]; lo, hi = int(SR / 800), int(SR / 80); p = lo + int(np.argmax(a[lo:hi]))
        if a[p] / a[0] > 0.6: pitched += 1; f0s.append(SR / p)
    sw = float(np.percentile(db(r[live]), 90) - np.percentile(db(r[live]), 10))
    check("vocal: real singing", live.sum() >= 80 and pitched / live.sum() >= 0.8 and 200 <= np.median(f0s) <= 600 and sw >= 10.0,
          f"{int(live.sum())} active 100 ms frames, {100 * pitched / max(1, live.sum()):.0f} % pitched, median f0 {np.median(f0s):.0f} Hz, level swing p90-p10 {sw:.1f} dB")
    return rows, fails, st, {n: stats(x) for n, x in mixes.items()}, vg

def write(out_dir, S=None, events=None):
    d = build() if S is None else dict(stems=S, events=events, gain_db=0.0)
    S, events = d["stems"], d["events"]
    os.makedirs(os.path.join(out_dir, "stems"), exist_ok=True)
    for n in STEMS: sf.write(os.path.join(out_dir, "stems", f"{n}.wav"), S[n], SR, subtype="PCM_24")
    vmix, vo, bk, vg = vocal_in_backing(S)
    dry = {"DRY_drumbus": drumbus(S), "DRY_fullmix": fullmix(S), "DRY_padbass": padbass(S), "DRY_backing": backing(S), "DRY_vocal_in_backing": vmix, "KEY_kick": S["kick"], "KEY_kick_4x4": four_floor(S)}
    for n, x in dry.items(): sf.write(os.path.join(out_dir, f"{n}.wav"), x, SR, subtype="PCM_24")
    json.dump(events, open(os.path.join(out_dir, "events.json"), "w"), indent=0)
    return dry

def main():
    out = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(HERE, "..", "..", "prototypes", "DINLE", "10_TUTKAL_v3", "_session")
    d = build(); rows, fails, st, mixst, vg = sanity(d["stems"], d["events"])
    write(out)
    for name, ok, text in rows: print(f"{'PASS' if ok else 'FAIL'} {name}: {text}")
    print(f"session gain {d['gain_db']:+.2f} dB (full mix peak -3 dBFS); vocal gain {vg:+.1f} dB; events {len(d['events'])}; written to {out}")
    json.dump(dict(stems=st, mixes=mixst, events=len(d["events"]), session_gain_db=d["gain_db"], vocal_gain_db=vg, fails=fails), open(os.path.join(out, "sanity.json"), "w"), indent=1)
    return len(fails)

if __name__ == "__main__":
    sys.exit(main())
