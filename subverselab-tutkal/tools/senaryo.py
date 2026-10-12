#!/usr/local/bin/python3
"""TUTKAL scenario bench: the owner's Live session, rebuilt offline, through the installed (1.0.1) and the current (1.0.2) engine.

Owner, 2026-10-12, Live: TUTKAL on a Reason Rack synth (peaks -7 dBFS), key = a kick on another track (peaks -2 dBFS),
GLUE, EXT on, SC HPF AUTO (200 Hz), threshold -18 / -19.8, 4:1, attack 0.1 ms, release 0.3 s manual. The meter showed ~1 dB.
Question: is ~1 dB what the engine does, and why.

  senaryo.py              the table below; every row is one render (38.4 s, 48 kHz, 100 bpm four-on-the-floor kick)
  senaryo.py --wav DIR    also write each render's output as WAV for listening
  senaryo.py --synth a.wav --key b.wav   real bounces instead of the synthetic ones (any rate is resampled to 48 kHz;
                          levels are kept as bounced, so export them from Live with no normalising)

What it reports per row: GR p50 / p90 / max (dB, over the whole file) and the mean GR on the beat (0-50 ms after each kick).
Reference row "peak comp": a plain stereo-linked peak compressor on the same key, no HPF, same threshold / ratio / attack / release.
"""
import os, subprocess, sys, tempfile
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import synth_session as ss_                      # noqa: E402  deterministic stems (seed 7)

RAW = {"1.0.1": os.path.join(HERE, "..", "build", "tutkalraw_v3"),     # == the installed plug-in
       "1.0.2": os.path.join(HERE, "..", "build", "tutkalraw_102pre"), # 1.0.2 before the keyed-GLUE fix (2026-10-12)
       "fix":   os.path.join(HERE, "..", "build", "tutkalraw")}        # current Source/TutkalEngine.h
SR = ss_.SR
TMP = tempfile.mkdtemp(prefix="tutkal-senaryo-")
GLUE, LA, PUMP = 1, 2, 3

def amp(d): return 10 ** (d / 20)
def to_peak(x, dbfs): return (x / np.max(np.abs(x)) * amp(dbfs)).astype(np.float32)

def render(engine, x, key, **p):
    fi, fo, fg, fk = (os.path.join(TMP, n) for n in ("in.f32", "out.f32", "gr.f32", "key.f32"))
    x.astype(np.float32).tofile(fi)
    cmd = [RAW[engine], fi, fo, str(SR)] + [f"{k}={v}" for k, v in p.items()] + ["--gr", fg]
    if key is not None:
        key.astype(np.float32).tofile(fk); cmd += ["--key", fk]
    subprocess.run(cmd, check=True)
    return np.fromfile(fo, np.float32).reshape(-1, 2), np.fromfile(fg, np.float32)

def peak_comp(key, thr, ratio, att_ms, rel_ms):
    """Reference: stereo-linked peak detector, hard knee, attack/release on the GR. What a 'normal' compressor shows."""
    pk = np.max(np.abs(key), axis=1).astype(np.float64)
    lvl = 20 * np.log10(np.maximum(pk, 1e-7))
    target = np.maximum(0.0, (lvl - thr) * (1 - 1 / ratio))
    aA, aR = np.exp(-1 / (att_ms * 1e-3 * SR)), np.exp(-1 / (rel_ms * 1e-3 * SR))
    y = np.zeros_like(target); g = 0.0
    for i, t in enumerate(target):
        g = aA * g + (1 - aA) * t if t > g else aR * g + (1 - aR) * t
        y[i] = g
    return y

def load(path):
    import soundfile as sf
    from scipy.signal import resample_poly
    x, sr = sf.read(path, dtype="float32", always_2d=True)
    if x.shape[1] == 1: x = np.repeat(x, 2, axis=1)
    x = x[:, :2]
    if sr != SR:
        from math import gcd
        g = gcd(SR, sr); x = resample_poly(x, SR // g, sr // g, axis=0).astype(np.float32)
    return x

def stats(gr, beats):
    on = np.concatenate([gr[b:b + int(0.05 * SR)] for b in beats if b + int(0.05 * SR) < len(gr)])
    return np.percentile(gr, 50), np.percentile(gr, 90), gr.max(), on.mean()

def main():
    wav_dir = sys.argv[sys.argv.index("--wav") + 1] if "--wav" in sys.argv else None
    def arg(n): return sys.argv[sys.argv.index(n) + 1] if n in sys.argv else None
    if arg("--synth") and arg("--key"):
        synth, kick = load(arg("--synth")), load(arg("--key"))
        n = min(len(synth), len(kick)); synth, kick = synth[:n], kick[:n]
        env = np.max(np.abs(kick), axis=1); thr = 0.3 * env.max(); beats, last = [], -SR
        for i in np.flatnonzero(env > thr):
            if i - last > int(0.1 * SR): beats.append(int(i))
            last = i
        print(f"real files: synth peak {20*np.log10(np.abs(synth).max()):.1f} dBFS, key peak {20*np.log10(np.abs(kick).max()):.1f} dBFS, {len(beats)} kick hits, {n/SR:.1f} s")
    else:
        S = ss_.build()["stems"]
        synth = to_peak(S["pad"] + S["bass"], -7.0)          # the Reason synth: pad + bass, peaks -7 dBFS
        kick = to_peak(ss_.four_floor(S), -2.0)              # the kick track: four on the floor, peaks -2 dBFS
        beats = [int(round(b * ss_.BEAT * SR)) for b in range(ss_.BARS * 4)]

    k = kick
    spec = np.abs(np.fft.rfft(k[:, 0])); f = np.fft.rfftfreq(len(k), 1 / SR)
    below = spec[f < 200] ** 2; print(f"kick energy below 200 Hz: {100 * below.sum() / (spec ** 2).sum():.1f} %")

    base = dict(voicing=GLUE, threshold=-18, ratio=4, attack=0.1, release=300, auto=0, ext=1, makeup=0, mix=1)
    rows = [
        ("A owner's setting (GLUE, EXT, HPF auto 200)",  base),
        ("B  same, threshold -19.8",                      {**base, "threshold": -19.8}),
        ("C  SC HPF off",                                 {**base, "hpf": 5}),
        ("D  SC HPF 50 Hz",                               {**base, "hpf": 50}),
        ("E  EXT off (detector hears the synth)",         {**base, "ext": 0}),
        ("F  EXT off, HPF off",                           {**base, "ext": 0, "hpf": 5}),
        ("G  AUTO release on",                            {**base, "auto": 1}),
        ("H  release 0.1 s (1.0.2 punch)",                {**base, "release": 100}),
        ("I  PUMP, thr -18, depth 12, 1/8, EXT",          dict(voicing=PUMP, threshold=-18, ratio=10, attack=0.5, ext=1, depth=12, note=0.125, bpm=100, mix=1)),
    ]
    print(f"\n{'row':46s} {'engine':6s} {'p50':>6s} {'p90':>6s} {'max':>6s} {'beat':>6s}   (GR dB)")
    for name, p in rows:
        for eng in RAW:
            out, gr = render(eng, synth, kick, **p)
            a, b, c, d = stats(gr, beats)
            print(f"{name:46s} {eng:6s} {a:6.2f} {b:6.2f} {c:6.2f} {d:6.2f}")
            if wav_dir:
                import soundfile as sf
                os.makedirs(wav_dir, exist_ok=True)
                sf.write(os.path.join(wav_dir, f"{name[:1]}_{eng}.wav"), out, SR, subtype="FLOAT")
    ref = peak_comp(kick, -18, 4, 0.1, 300)
    a, b, c, d = stats(ref, beats)
    print(f"{'R  reference peak comp on the key, no HPF':46s} {'ref':6s} {a:6.2f} {b:6.2f} {c:6.2f} {d:6.2f}")
    if wav_dir:
        import soundfile as sf
        sf.write(os.path.join(wav_dir, "0_synth_dry.wav"), synth, SR, subtype="FLOAT")
        sf.write(os.path.join(wav_dir, "0_kick_key.wav"), kick, SR, subtype="FLOAT")

if __name__ == "__main__" and "--denetim" not in sys.argv:
    main()


# ---------------------------------------------------------------------------------------------------------------------
# senaryo.py --denetim [engine]   knob authority audit: every front knob swept over its range, GLUE and CORE, on two
# programmes (drum bus unkeyed; synth keyed by a kick), next to a reference peak compressor with the same settings.
# A knob "works" when it moves the result about as much as the same knob on the reference. Numbers, no verdicts by ear.
def peak_comp_full(x, key, thr, ratio, att_ms, rel_ms):
    det = key if key is not None else x
    return peak_comp(det, thr, ratio, att_ms, rel_ms)

def denetim(engines=("1.0.1", "fix")):
    """Each knob is read where its effect lives: THRESHOLD and RATIO by the GR on the hits (mean GR 0-50 ms after each hit),
    ATTACK by how much of the hit's first 2 ms is caught (mean GR 0.5-2.5 ms after each hit), RELEASE by the GR between hits
    (median of the whole file). A knob is WEAK when it moves its reading less than half as much as the same knob on a plain
    peak compressor with the same settings, DEAD under 0.5 dB, BACKWARDS when it moves the other way."""
    S = ss_.build(); st = S["stems"]
    drum = to_peak(ss_.drumbus(st), -3.0)
    drum_hits = sorted({int(round(e["t"] * SR)) for e in S["events"] if e["inst"] in ("kick", "snare")})
    synth = to_peak(st["pad"] + st["bass"], -7.0); kick = to_peak(ss_.four_floor(st), -2.0)
    beats = [int(round(b * ss_.BEAT * SR)) for b in range(ss_.BARS * 4)]
    progs = [("davul bus", drum, None, drum_hits), ("synth + kick key", synth, kick, beats)]
    base = dict(threshold=-18, ratio=4, attack=10, release=300, auto=0, mix=1, makeup=0)
    def on_hits(gr, hits, a_ms, b_ms):
        a, b = int(a_ms * 1e-3 * SR), int(b_ms * 1e-3 * SR)
        return float(np.mean(np.concatenate([gr[h + a:h + b] for h in hits[1:] if h + b < len(gr)])))
    read = {"threshold": lambda g, h: on_hits(g, h, 0, 50), "ratio": lambda g, h: on_hits(g, h, 0, 50),
            "attack": lambda g, h: on_hits(g, h, 0.5, 2.5), "release": lambda g, h: float(np.median(g))}
    sweeps = [("threshold", [-30, -24, -18, -12]), ("ratio", [1.5, 2, 4, 8, 20]),
              ("attack", [0.1, 1, 3, 10, 30]), ("release", [100, 200, 300, 600, 1200])]
    verdicts = []
    for vname, v in (("GLUE", GLUE), ("CORE", 0)):
        for pname, x, key, hits in progs:
            print(f"\n== {vname} / {pname}   (dB; ref = plain peak comp, same settings)", flush=True)
            refs = {}
            for knob, vals in sweeps:
                ref = []
                for val in vals:
                    rp = {**base, knob: val}
                    ref.append(read[knob](peak_comp_full(x, key, rp["threshold"], rp["ratio"], rp["attack"], rp["release"]), hits))
                refs[knob] = ref
            for eng in engines:
                for knob, vals in sweeps:
                    row = []
                    for val in vals:
                        p = {**base, knob: val, "voicing": v, "ext": 1 if key is not None else 0}
                        _, gr = render(eng, x, key, **p)
                        row.append(read[knob](gr, hits))
                    ref = refs[knob]
                    d, rd = row[-1] - row[0], ref[-1] - ref[0]
                    if abs(d) < 0.5: tag = "DEAD"
                    elif np.sign(d) != np.sign(rd): tag = "BACKWARDS"
                    elif abs(d) < 0.5 * abs(rd): tag = "WEAK"
                    else: tag = "ok"
                    verdicts.append((vname, pname, eng, knob, tag))
                    print(f"  {eng:5s} {knob:9s} " + " ".join(f"{a:5.1f}" for a in row) + "  | ref " + " ".join(f"{a:5.1f}" for a in ref)
                          + f"  | moves {d:+5.1f} vs ref {rd:+5.1f}  {tag}", flush=True)
    bad = [vv for vv in verdicts if vv[4] != "ok"]
    print(f"\n{len(verdicts) - len(bad)} of {len(verdicts)} ok")
    for vv in bad: print("  " + " / ".join(vv))
    return bad

if __name__ == "__main__" and "--denetim" in sys.argv:
    i = sys.argv.index("--denetim")
    denetim(tuple(sys.argv[i + 1].split(",")) if len(sys.argv) > i + 1 else ("1.0.1", "fix"))
