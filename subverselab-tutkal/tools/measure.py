#!/usr/local/bin/python3
"""TUTKAL engine measurements (SPEC.md, M1-M18). Runs build/tutkalraw, the engine as the plugin will run it.

  measure.py            every check; exit code = number of failures
  measure.py M4 M12     only those
  measure.py --probe file.wav [seconds]   gain reduction on real programme, per ratio
"""
import os, subprocess, sys, time, tempfile
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
RAW = os.environ.get("TUTKALRAW", os.path.join(HERE, "..", "build", "tutkalraw"))
TMP = tempfile.mkdtemp(prefix="tutkal-")
LAT = 63
fails = 0

def run(x, sr=48000, key=None, block=None, blocks_random=False, mono=False, want_gr=True, at=(), **params):
    """x: (n,) or (n,2) float32. Returns (out, gr)."""
    x = np.asarray(x, dtype=np.float32)
    if x.ndim == 1 and not mono:
        x = np.stack([x, x], axis=1)
    fi, fo, fg, fk = (os.path.join(TMP, n) for n in ("in.f32", "out.f32", "gr.f32", "key.f32"))
    x.tofile(fi)
    params = {"voicing": 0, **params}          # M1-M18 are the core's checks; voicing checks (G*, L*, P*) name theirs
    # GLUE checks G1-G7 were written before the kGlueDetDb detector offset (2026-10-04). The offset is a pure level shift,
    # so threshold + GLUE_DET reproduces the old operating point exactly; G8 checks the calibration itself.
    if params["voicing"] == 1 and isinstance(params.get("threshold"), (int, float)) and not params.pop("raw_threshold", False):
        params["threshold"] = params["threshold"] + GLUE_DET
    params.pop("raw_threshold", None)
    cmd = [RAW, fi, fo, str(sr)] + [f"{k}={v}" for k, v in params.items()]
    if key is not None:
        k = np.asarray(key, dtype=np.float32)
        if k.ndim == 1: k = np.stack([k, k], axis=1)
        k.tofile(fk); cmd += ["--key", fk]
    if want_gr: cmd += ["--gr", fg]
    if block: cmd += ["--block", str(block)]
    if blocks_random: cmd += ["--blocks", "random"]
    if mono: cmd += ["--mono"]
    for n, k, v in at: cmd += ["--at", str(n), f"{k}={v}"]
    subprocess.run(cmd, check=True)
    out = np.fromfile(fo, dtype=np.float32)
    out = out if mono else out.reshape(-1, 2)
    gr = np.fromfile(fg, dtype=np.float32) if want_gr else None
    return out, gr

GLUE_DET = 11.0   # == kGlueDetDb in Source/TutkalEngine.h

def db(a): return 20 * np.log10(np.maximum(np.abs(a), 1e-12))
def amp(d): return 10 ** (d / 20)
def sine(f, level_db, secs, sr=48000): return (amp(level_db) * np.sin(2 * np.pi * f * np.arange(int(secs * sr)) / sr)).astype(np.float32)
def knee_for(r): return float(np.clip(10 * (2 / r) ** 0.7, 1.5, 12))
def static_gr(over, r, w):
    s = 1 - 1 / r
    if 2 * over < -w: return 0.0
    if 2 * over <= w: return s * (over + w / 2) ** 2 / (2 * w)
    return s * over

def harmonics(y, f0, sr, n=10):
    """Amplitudes of harmonics 1..n of f0 (Hann window, coherent gain removed)."""
    w = np.hanning(len(y)); spec = np.abs(np.fft.rfft(y * w)) / (np.sum(w) / 2)
    out = []
    for k in range(1, n + 1):
        b = int(round(k * f0 * len(y) / sr))
        if b + 3 >= len(spec): out.append(0.0); continue
        out.append(float(np.sqrt(np.sum(spec[b - 2:b + 3] ** 2) / 1.5)))   # Hann: power spread over the main lobe
    return np.array(out)
def thd(y, f0, sr):
    h = harmonics(y, f0, sr); return 100 * np.sqrt(np.sum(h[1:] ** 2)) / max(h[0], 1e-12), h

def report(tag, ok, text):
    global fails
    print(f"{tag} {'PASS' if ok else 'FAIL'} {text}")
    if not ok: fails += 1

def cross_time(gr, start, level, sr, rising=True):
    seg = gr[start:]
    idx = np.argmax(seg >= level) if rising else np.argmax(seg <= level)
    return idx / sr

# ---------------------------------------------------------------------------------------------
def M1():
    worst = 0.0; where = ""
    for f in (100, 1000, 5000):
        for r in (1.5, 2, 4, 10, 20):
            levels = np.arange(-50, 1, 5)
            x = np.concatenate([sine(f, l, 1.0) for l in levels])
            _, gr = run(x, threshold=-30, ratio=r, attack=1, release=1200)
            for i, l in enumerate(levels):
                got = float(np.mean(gr[int((i + 0.8) * 48000):int((i + 1) * 48000)]))
                want = static_gr(l + 30, r, knee_for(r))
                if abs(got - want) > worst: worst, where = abs(got - want), f"{f} Hz {r}:1 at {l} dBFS: {got:.2f} vs {want:.2f}"
    report("M1", worst <= 0.25, f"static curve, worst error {worst:.3f} dB ({where}) (want <= 0.25)")

def step(lo, hi, pre=0.5, hold=1.0, post=0.0, sr=48000):
    return np.concatenate([np.full(int(pre * sr), amp(lo)), np.full(int(hold * sr), amp(hi)), np.full(int(post * sr), amp(lo))]).astype(np.float32)

def attack_time(ms, sr=48000):
    _, gr = run(step(-60, -6, sr=sr), sr=sr, threshold=-30, ratio=4, attack=ms, release=1200)
    settled = float(np.mean(gr[-int(0.1 * sr):]))
    return cross_time(gr, int(0.5 * sr), 0.632 * settled, sr) * 1000, settled

def release_time(ms, sr=48000, att=1):
    _, gr = run(step(-60, -6, post=6.0, sr=sr), sr=sr, threshold=-30, ratio=4, attack=att, release=ms)
    at = int(1.5 * sr); top = float(gr[at - 1])
    return cross_time(gr, at, 0.368 * top, sr, rising=False) * 1000

def M2():
    ok = True; txt = []
    for ms in (0.1, 1, 10, 30):
        t, _ = attack_time(ms)
        tol = 0.2 * ms + 1000 / 48000
        ok &= abs(t - ms) <= tol; txt.append(f"{ms} ms -> {t:.3f}")
    report("M2", ok, "attack to 63 %: " + ", ".join(txt) + " ms (want within 20 % + one sample)")

def M3():
    ok = True; txt = []
    for ms in (100, 300, 1200):
        t = release_time(ms); want = ms + 1
        ok &= abs(t - want) <= 0.2 * want; txt.append(f"{ms} ms -> {t:.1f}")
    report("M3", ok, "release to 63 % recovered: " + ", ".join(txt) + " ms (want within 20 % of knob + attack)")

def M4():
    worst = 0.0; where = ""; worst_s = 0.0
    for r in (1.5, 2, 4, 10, 20):
        for a in (0.1, 1, 30):
            _, gr = run(step(-40, 0, hold=1.5), threshold=-30, ratio=r, attack=a, release=300)
            over = float(np.max(gr) - np.mean(gr[-4800:]))
            if over > worst: worst, where = over, f"{r}:1, attack {a} ms"
            # and on a tone burst: never more than the static curve gives for its peak
            x = np.concatenate([sine(1000, -40, 0.5), sine(1000, 0, 1.0)])
            _, g2 = run(x, threshold=-30, ratio=r, attack=a, release=300)
            worst_s = max(worst_s, float(np.max(g2)) - static_gr(30, r, knee_for(r)))
    report("M4", worst <= 0.1 and worst_s <= 0.1, f"overshoot: DC step {worst:.4f} dB over the settled value ({where}); tone burst {worst_s:.4f} dB over the static curve (want <= 0.1)")

def M5():
    lvl = -30 + 10 / 0.75 + 0.0   # 10 dB of gain reduction at 4:1 (above the knee)
    res = []
    for hold in (0.05, 5.0):
        _, gr = run(step(-70, lvl, pre=0.2, hold=hold, post=8.0), threshold=-30, ratio=4, attack=1, auto=1)
        at = int((0.2 + hold) * 48000); top = float(gr[at - 1])
        res.append((top, cross_time(gr, at, 0.1 * top, 48000, rising=False)))
    ok = res[0][1] <= 0.35 and 2.0 <= res[1][1] <= 5.0 and res[0][0] > 9 and res[1][0] > 9
    report("M5", ok, f"AUTO memory: after 50 ms of {res[0][0]:.1f} dB, 90 % recovered in {res[0][1]:.3f} s; after 5 s of {res[1][0]:.1f} dB, in {res[1][1]:.2f} s (want <= 0.35 and 2..5)")

def M6():
    lvl = -30 + 10 / 0.75
    vals = {}
    for name, kw in (("0.1 s", dict(release=100)), ("1.2 s", dict(release=1200)), ("AUTO", dict(auto=1))):
        out, gr = run(sine(50, lvl, 4.0), threshold=-30, ratio=4, attack=10, **kw)
        vals[name] = (thd(out[-96000:, 0], 50, 48000)[0], float(np.mean(gr[-96000:])))
    ok = vals["0.1 s"][0] < 3 and vals["1.2 s"][0] < 0.5 and vals["AUTO"][0] < 1
    report("M6", ok, "50 Hz THD at ~10 dB GR: " + ", ".join(f"{k} {v[0]:.3f} % (GR {v[1]:.1f})" for k, v in vals.items()) + " (want < 3 / < 0.5 / < 1)")

def M7():
    out, gr = run(sine(1000, -18, 2.0), threshold=0)
    t, _ = thd(out[-48000:, 0], 1000, 48000)
    report("M7", t < 0.01 and float(np.max(gr)) == 0.0, f"1 kHz at -18 dBFS, no gain reduction, drive 0: THD {t:.5f} % (want < 0.01)")

def M8():
    ok = True; txt = []
    for d in (0.5, 1.0):
        lo, hl = thd(run(sine(60, -12, 2.0), threshold=0, drive=d)[0][-48000:, 0], 60, 48000)
        hi, hh = thd(run(sine(3000, -12, 2.0), threshold=0, drive=d)[0][-48000:, 0], 3000, 48000)
        odd = hl[2] > 3 * hl[1]
        ok &= lo >= 3 * hi and odd and lo > 0.3
        txt.append(f"drive {int(d * 100)} %: 60 Hz {lo:.2f} %, 3 kHz {hi:.3f} %, H3/H2 {hl[2] / max(hl[1], 1e-12):.0f}")
    q0 = run(sine(1000, -24, 1.0), threshold=0)[0][-24000:, 0]; q1 = run(sine(1000, -24, 1.0), threshold=0, drive=1)[0][-24000:, 0]
    lev = float(db(np.sqrt(np.mean(q1 ** 2))) - db(np.sqrt(np.mean(q0 ** 2))))
    ok &= abs(lev) <= 1.0
    report("M8", ok, "; ".join(txt) + f"; level at -24 dBFS moves {lev:+.2f} dB (want LF >= 3x HF, odd, |level| <= 1)")

def M9():
    sr = 48000; f = 9000.0
    out, _ = run(sine(f, -3, 2.0), threshold=0, drive=1)
    y = out[-65536:, 0]; w = np.blackman(len(y)); spec = db(np.abs(np.fft.rfft(y * w)) / (np.sum(w) / 2))
    freqs = np.fft.rfftfreq(len(y), 1 / sr)
    keep = np.ones(len(spec), bool)
    for k in (1, 2):                                   # 9k and 18k are the harmonics below Nyquist
        keep &= np.abs(freqs - k * f) > 150
    keep &= freqs > 20
    worst = float(np.max(spec[keep]) - np.max(spec)); at = float(freqs[keep][np.argmax(spec[keep])])
    report("M9", worst <= -80, f"aliasing, 9 kHz at -3 dBFS, drive 100 %: strongest non-harmonic line {worst:.1f} dBc at {at:.0f} Hz (want <= -80)")

def delayed(x): return np.concatenate([np.zeros((LAT, 2), np.float32), x[:-LAT]])

def M10():
    rng = np.random.default_rng(1); x = (rng.standard_normal((48000, 2)) * 0.2).astype(np.float32)
    quiet = (x * 0.25).astype(np.float32)             # stays below the knee at threshold 0
    a = np.array_equal(run(x, threshold=-30, ratio=10, drive=1, makeup=6, **{"in": 0})[0], delayed(x))
    b = np.array_equal(run(x, threshold=-30, ratio=10, drive=1, makeup=6, mix=0)[0], delayed(x))
    c = np.array_equal(run(quiet, threshold=0)[0], delayed(quiet))
    report("M10", a and b and c, f"nulls against the input delayed by {LAT}: IN off {'exact' if a else 'DIFFERS'}, MIX 0 {'exact' if b else 'DIFFERS'}, no GR + drive 0 {'exact' if c else 'DIFFERS'}")

def M11():
    x = np.zeros(4800, np.float32); x[0] = 0.5
    p0 = int(np.argmax(np.abs(run(x, threshold=0)[0][:, 0]))); p1 = int(np.argmax(np.abs(run(x, threshold=0, drive=1)[0][:, 0])))
    # An impulse only shows whole samples. The drive path, run almost linear, must also null against
    # the plain delayed path: a quarter sample of error leaves about -10 dB at 10 kHz.
    rng = np.random.default_rng(5); n = rng.standard_normal(48000).astype(np.float32) * 0.1
    spec = np.fft.rfft(n); spec[int(12000 * len(n) / 48000):] = 0; n = np.fft.irfft(spec).astype(np.float32)
    a = run(n, threshold=0)[0][:, 0]; b = run(n, threshold=0, drive=0.0002)[0][:, 0]
    null = float(db(np.sqrt(np.mean((a - b)[4800:] ** 2))) - db(np.sqrt(np.mean(a[4800:] ** 2))))
    report("M11", p0 == LAT and p1 == LAT and null <= -60, f"impulse lands at sample {p0} (drive 0) and {p1} (drive 100 %); drive path against the delayed path nulls to {null:.1f} dB below 12 kHz (want {LAT}, {LAT}, <= -60)")

def busy(secs=3.0, sr=48000, seed=7):
    rng = np.random.default_rng(seed); n = int(secs * sr)
    x = rng.standard_normal((n, 2)).astype(np.float32) * 0.3
    env = (np.sin(2 * np.pi * 2.0 * np.arange(n) / sr) > 0.3).astype(np.float32) * 0.9 + 0.1
    return (x * env[:, None]).astype(np.float32)

def M12():
    # Every control jumps mid-file at fixed sample positions; the output must not depend on how the
    # host cuts the blocks.
    x = busy()
    kw = dict(threshold=-28, ratio=6, attack=3, release=200, makeup=4, mix=0.8, drive=0.7, hpf=120)
    at = [(20011, "threshold", -12), (31337, "ratio", 15), (40001, "makeup", 9), (52345, "mix", 0.4), (60013, "drive", 0.1),
          (70007, "auto", 1), (81111, "attack", 0.3), (90001, "hpf", 30), (100003, "in", 0), (110017, "in", 1), (120001, "release", 900)]
    ref = run(x, block=512, at=at, **kw)[0]; bad = []
    for b in (1, 7, 64, 4096):
        if not np.array_equal(run(x, block=b, at=at, **kw)[0], ref): bad.append(str(b))
    if not np.array_equal(run(x, blocks_random=True, at=at, **kw)[0], ref): bad.append("random")
    report("M12", not bad, f"block sizes 1, 7, 64, 512, 4096 and random, 11 control jumps mid-file: {'bit-identical' if not bad else 'DIFFER at ' + ','.join(bad)}")

def M13():
    att = {}; rel = {}
    for sr in (44100, 48000, 96000, 192000):
        att[sr] = attack_time(10, sr)[0]; rel[sr] = release_time(300, sr)
    x = np.zeros(4800, np.float32); x[0] = 0.5
    lat = {sr: int(np.argmax(np.abs(run(x, sr=sr, threshold=0)[0][:, 0]))) for sr in att}
    sa = (max(att.values()) - min(att.values())) / np.mean(list(att.values())); sr_ = (max(rel.values()) - min(rel.values())) / np.mean(list(rel.values()))
    ok = sa <= 0.05 and sr_ <= 0.05 and all(v == LAT for v in lat.values())
    report("M13", ok, "44.1/48/96/192 kHz: attack " + "/".join(f"{att[s]:.2f}" for s in att) + " ms, release " + "/".join(f"{rel[s]:.0f}" for s in rel) + f" ms, latency {sorted(set(lat.values()))} (want spread <= 5 %, latency {LAT})")

def M14():
    # Each control jumps from one end of its range to the other in a single sample.
    worst = 0.0; where = ""
    x = sine(1000, -12, 1.0); n = 24000 + 11                     # the jump lands mid-cycle, mid-file
    assert n + 4800 < len(x)
    for name, kw, at, final in (("threshold", dict(threshold=0, ratio=10, attack=0.1), [(n, "threshold", -40)], 1.0),
                                ("makeup", dict(threshold=0), [(n, "makeup", 20)], 10.0),
                                ("mix", dict(threshold=-40, ratio=10), [(n, "mix", 0)], 1.0),
                                ("drive", dict(threshold=0), [(n, "drive", 1)], 1.0),
                                ("in", dict(threshold=-40, ratio=20, makeup=0), [(n, "in", 0)], 1.0),
                                ("ratio", dict(threshold=-40, ratio=1.5, attack=0.1), [(n, "ratio", 20)], 1.0)):
        out = run(x, at=at, **kw)[0][:, 0]
        own = float(np.max(np.abs(np.diff(x)))) * final
        ratio = float(np.max(np.abs(np.diff(out[n - 200:n + 4800])))) / own
        if ratio > worst: worst, where = ratio, name
    report("M14", worst <= 1.5, f"controls jumping across their range in one sample: largest step {worst:.2f}x the sine's own ({where}) (want <= 1.5)")

def M15():
    sr = 48000; x = busy(20.0, seed=3) * 3.0
    x[1000:1100] = np.nan; x[5000:5010] = np.inf; x[9000:9200] = -np.inf; x[20000:21000] = 1e-39; x[40000:48000] *= 40.0
    x = np.concatenate([x, np.zeros((sr * 4, 2), np.float32)]).astype(np.float32)
    t0 = time.time()
    out, gr = run(x, threshold="-40:0", ratio="1.5:20", attack="0.1:30", release="100:1200", makeup="0:20", mix="0.2:1", drive="0:1", hpf="20:300")
    out2, gr2 = run(x, threshold=-40, ratio=20, attack=0.1, auto=1, makeup=20, drive=1)
    fin = bool(np.all(np.isfinite(out)) and np.all(np.isfinite(out2)) and np.all(np.isfinite(gr)) and np.all(np.isfinite(gr2)))
    tail = max(float(np.max(np.abs(out[-sr:]))), float(np.max(np.abs(out2[-sr // 2:]))))
    ok = fin and max(gr.max(), gr2.max()) <= 60.0 and tail == 0.0
    report("M15", ok, f"hostile input (NaN, inf, denormals, +40 dBFS) with every control moving: finite {fin}, max GR {max(gr.max(), gr2.max()):.1f} dB, tail peak {tail:.1e} (want finite, <= 60, exactly 0)")

def M16():
    quiet = sine(1000, -40, 2.0); loud = sine(1000, -6, 2.0); key = sine(100, -6, 2.0)
    _, g_key = run(quiet, key=key, threshold=-30, ext=1)
    _, g_nokey = run(loud, threshold=-30, ext=1)
    off_a = run(loud, key=key, threshold=-30)[0]; off_b = run(loud, threshold=-30)[0]
    garbage = np.full(len(loud), np.nan, np.float32); garbage[::3] = np.inf; garbage[1::3] = 1e30
    off_c = run(loud, key=garbage, threshold=-30)[0]
    on_garbage, g_g = run(loud, key=garbage, threshold=-30, ext=1)
    same = np.array_equal(off_a, off_b) and np.array_equal(off_c, off_b)
    ok = g_key[-1] >= 6 and g_nokey.max() < 0.1 and same and bool(np.all(np.isfinite(on_garbage)))
    report("M16", ok, f"EXT on + key: {g_key[-1]:.2f} dB GR on a quiet programme; EXT on, nothing routed: {g_nokey.max():.2f} dB on a loud one; EXT off with a key or garbage on the sidechain: {'bit-identical' if same else 'DIFFERS'}; EXT on + garbage: finite {bool(np.all(np.isfinite(on_garbage)))} (want >= 6 / < 0.1 / identical / finite)")

def M17():
    x = busy(60.0); t0 = time.time(); run(x, threshold=-30, auto=1, drive=0.5, want_gr=False); t = time.time() - t0
    report("M17", t < 3.0, f"60 s stereo at 48 kHz rendered in {t:.2f} s including file I/O (want < 3)")

# Real recordings, not separated stems (separation artefacts hide what a compressor does).
STEMS = [("snare", "~/Desktop/solo/READY DRUM BOX/Samples/Processed/Bounce/Bounce SNARE [[]2025-09-26 022447[]]-2.wav"),
         ("boom", "~/Desktop/solo/READY DRUM BOX/Samples/Processed/Bounce/Bounce 2-3 OP Boom*.wav"),
         ("vocal", "~/Desktop/mali demo  Project/Samples/Recorded/4-Audio 0002 [[]2026-09-27 224148[]].wav"),
         ("mix", "~/Desktop/Export/Static Ritual.wav")]

def resolve(path):
    import glob
    hits = sorted(glob.glob(os.path.expanduser(path)))
    return hits[0] if hits else os.path.expanduser(path)

def load(path, secs=12.0, start=20.0):
    import soundfile as sf
    path = resolve(path); info = sf.info(path)
    s = int(min(start, max(0, info.duration - secs - 1)) * info.samplerate)
    x, sr = sf.read(path, start=s, frames=int(secs * info.samplerate), dtype="float32", always_2d=True)
    if x.shape[1] == 1: x = np.repeat(x, 2, axis=1)
    return x[:, :2].copy(), sr

def probe(path, secs=12.0, table=True):
    x, sr = load(path, secs); rows = []
    x = (x * amp(-6.0) / max(float(np.max(np.abs(x))), 1e-9)).astype(np.float32)      # gain-staged: peaks at -6 dBFS
    peak = float(db(np.max(np.abs(x)))); rms = float(db(np.sqrt(np.mean(x ** 2))))
    if rms < -40: raise RuntimeError(f"{os.path.basename(path)} is nearly empty ({rms:.1f} dBFS RMS): not programme")
    # A working setting for any material, dense or sparse: the threshold 10 dB under the level the
    # 10 ms peaks reach 5 % of the time.
    n10 = int(0.01 * sr); pk = np.max(np.abs(x[:len(x) // n10 * n10]).reshape(-1, n10, 2), axis=(1, 2))
    thr = round(float(db(np.percentile(pk, 95))) - 10)
    for r in (1.5, 2, 3, 4, 6, 10, 20):
        out, gr = run(x, sr=sr, threshold=thr, ratio=r, attack=10, auto=1)
        rows.append((r, float(np.mean(gr)), float(np.percentile(gr, 99)), float(np.max(gr)), float(db(np.sqrt(np.mean(out ** 2))))))
    if table:
        print(f"  {os.path.basename(path)}: {rms:.1f} dBFS RMS, peak {peak:.1f}, threshold {thr}")
        print("  ratio | GR mean  GR p99  GR max | out RMS")
        for r in rows: print(f"  {r[0]:5.1f} | {r[1]:7.2f} {r[2]:7.2f} {r[3]:7.2f} | {r[4]:7.2f}")
    return rows

def M18():
    ok = True; txt = []
    for name, path in STEMS:
        if not os.path.exists(resolve(path)):
            ok = False; txt.append(f"{name}: FILE MISSING"); continue
        try: rows = probe(path, table=False)
        except RuntimeError as e:
            ok = False; txt.append(f"{name}: {e}"); continue
        mono = all(rows[i + 1][1] >= rows[i][1] - 0.01 for i in range(len(rows) - 1))
        spread = max(r[3] - r[2] for r in rows)
        ok &= mono and spread <= 6.0 and rows[-1][2] >= 6.0          # and it really is being compressed (p99 at 20:1)
        txt.append(f"{name}: GR mean {rows[0][1]:.1f} -> {rows[-1][1]:.1f} dB, p99 at 20:1 {rows[-1][2]:.1f}, max over p99 {spread:.1f} dB{'' if mono else ' NOT MONOTONIC'}")
    report("M18", ok, "programme, ratio 1.5 -> 20: " + "; ".join(txt) + " (want rising, p99 >= 6 dB at 20:1, max within 6 dB of p99)")

# ---------------------------------------------------------------------------------------------
# VOICINGS (SPEC.md, CHARACTER). Voicing 1 GLUE, 2 LA, 3 PUMP. M1-M18 above are CORE-only (voicing 0);
# the checks below are per voicing, and V1-V5 repeat the core's guarantees for every voicing.
SR = 48000

def tone_steady(f, level, secs=3.0, **kw):
    out, gr = run(sine(f, level, secs), **kw)
    return out[-SR:, 0], gr

def share(y, f0):
    """THD in %, H2 and H3 as % of the fundamental."""
    t, h = thd(y, f0, SR); return t, 100 * h[1] / h[0], 100 * h[2] / h[0]

def hit_train(secs=6.0, period=0.5, seed=2):
    """Drum-like hits: a pitched thump and a noise snap, decaying; 20 dB crest."""
    rng = np.random.default_rng(seed); n = int(secs * SR); x = np.zeros(n, np.float32)
    t = np.arange(int(0.4 * SR)) / SR
    h = (np.sin(2 * np.pi * 120 * t * (1 + 0.5 * np.exp(-t / 0.02))) * np.exp(-t / 0.06) + 0.5 * rng.standard_normal(len(t)) * np.exp(-t / 0.015)).astype(np.float32)
    for k in range(int(secs / period)):
        s0 = int(k * period * SR); x[s0:s0 + len(h)] += h[:n - s0][:len(h)]
    return (x * 0.5 / np.max(np.abs(x))).astype(np.float32)

def snap_db(x, voicing, thr, att, period=0.5):
    """How much deeper the gain reduction is 40-80 ms after a hit's peak than at the peak itself (dB, mean over the hits).
    Positive: the transient got through."""
    _, gr = run(x, threshold=thr, ratio=4, attack=att, release=150, voicing=voicing)
    ds = []
    for k in range(1, int(len(x) / SR / period) - 1):
        s0 = int(k * period * SR); pk = s0 + int(np.argmax(np.abs(x[s0:s0 + int(0.02 * SR)])))
        ds.append(float(np.mean(gr[pk + int(0.04 * SR):pk + int(0.08 * SR)]) - gr[pk]))
    return float(np.mean(ds))

def G1():
    # Colour: 2nd + 3rd harmonic that rises with gain reduction and is a fixed share of the signal at any level.
    res = {}
    for L in (-12, -30):
        for g in (2, 4, 8):
            y, gr = tone_steady(1000, L, secs=8.0, threshold=L - g / 0.75, ratio=4, attack=10, release=300, voicing=1, hpf=0)
            res[(L, g)] = (float(np.mean(gr[-SR // 2:])),) + share(y, 1000)
    y, gr0 = tone_steady(1000, -24, threshold=0, voicing=1, hpf=0, raw_threshold=True); t0 = share(y, 1000)[0]   # -24 dBFS: below the top of the range even with kGlueDetDb
    y, _ = tone_steady(100, -12, secs=8.0, threshold=-12 - 5.33, ratio=4, attack=10, release=300, voicing=1, hpf=0); t100 = share(y, 100)[0]
    r4a, r4b = res[(-12, 4)], res[(-30, 4)]
    ok = (0.1 <= r4a[1] <= 0.3 and 0.1 <= r4b[1] <= 0.3 and r4a[2] >= 0.03 and r4a[3] >= 0.03
          and res[(-12, 2)][1] < r4a[1] < res[(-12, 8)][1] and t0 < 0.01 and t100 < 0.6 and abs(r4a[0] - 4) < 0.3)
    report("G1", ok, f"GLUE colour, 1 kHz: THD {res[(-12, 2)][1]:.3f} / {r4a[1]:.3f} / {res[(-12, 8)][1]:.3f} % at GR {res[(-12, 2)][0]:.1f} / {r4a[0]:.1f} / {res[(-12, 8)][0]:.1f} dB "
                     f"(at 4 dB: H2 {r4a[2]:.3f} %, H3 {r4a[3]:.3f} %; at -30 dBFS {r4b[1]:.3f} %); no GR {t0:.5f} %; 100 Hz at 4 dB {t100:.3f} % (want 0.1-0.3 at 4 dB, H2 and H3 >= 0.03, rising, < 0.01 at 0, 100 Hz < 0.6)")

def click_loss(v, att, thr=-34.0, **kw):
    """A 3 ms burst at -6 dBFS inside a -30 dBFS bed (24 dB over it): how far below its input the burst's peak comes out (dB), the bed's gain
    reduction just before it, and the deepest gain reduction during it."""
    x = sine(1000, -30, 8.0).copy(); at = int(6 * SR); L = int(0.003 * SR); f = int(0.0005 * SR)
    w = np.ones(L, np.float32); w[:f] = np.linspace(0, 1, f); w[-f:] = np.linspace(1, 0, f); x[at:at + L] = sine(1000, -6, 0.003) * w
    y, gr = run(x, voicing=v, threshold=thr, ratio=4, attack=att, release=300, **kw)
    return (float(db(np.max(np.abs(x[at:at + L]))) - db(np.max(np.abs(y[at + LAT:at + LAT + L + 60, 0])))),
            float(np.mean(gr[at - 4800:at])), float(gr[at:at + L + 60].max()))

def G2():
    # Transient pass-through, bounded: a 3 ms burst 24 dB over a bed loses the bed's gain reduction plus at most the fast stage's cap (1.5 dB),
    # where the core clamps it by ~18 dB. The attack knob still orders how much of the burst gets through.
    core, _, _ = click_loss(0, 0.1, hpf=0)
    l01, bed, gmax = click_loss(1, 0.1, hpf=0); l3, _, _ = click_loss(1, 3, hpf=0); l10, _, _ = click_loss(1, 10, hpf=0); l30, _, _ = click_loss(1, 30, hpf=0)
    x = hit_train(); sc = snap_db(x, 0, -22, 0.1); sg = snap_db(x, 1, -22, 0.1)
    ok = (l01 - bed <= 1.8 and core - l01 >= 8.0 and l01 > l3 > l10 > l30 and l01 - l30 >= 0.7 and sg >= 0.0 and sg - sc >= 2.0)
    report("G2", ok, f"GLUE burst pass-through, 3 ms at +24 dB over the bed: burst peak loses {l01:.2f} dB at attack 0.1 ms (bed GR {bed:.2f}, so +{l01 - bed:.2f}; core loses {core:.2f}); "
                     f"attack 0.1 / 3 / 10 / 30 ms: {l01:.2f} / {l3:.2f} / {l10:.2f} / {l30:.2f} dB; hit train, transient let through {sg:+.2f} dB (core {sc:+.2f}) "
                     f"(want <= +1.8 over the bed, core >= 8 dB worse, falling with the knob by >= 0.7, >= 0 and >= 2 over the core)")

def G3():
    # The detector is mostly a 100 ms mean square: a 1 ms pulse every 50 ms (same peak as the sine) reads far lower than on a peak detector,
    # a continuous sine of the same peak reads the same.
    n = int(8 * SR); gate = ((np.arange(n) % int(0.05 * SR)) < SR // 1000).astype(np.float32)
    pulsed = (sine(1000, -6, 8.0) * gate).astype(np.float32); cont = sine(1000, -6, 8.0); r = {}
    for v in (0, 1):
        kw = dict(threshold=-30, ratio=4, attack=0.1, release=1200, voicing=v, hpf=0)
        r[v] = (float(np.mean(run(pulsed, **kw)[1][-SR:])), float(np.mean(run(cont, **kw)[1][-SR:])))
    ok = r[0][0] - r[1][0] >= 6.0 and abs(r[1][1] - r[0][1]) <= 0.5
    report("G3", ok, f"GLUE detector, 1 ms pulses every 50 ms at -6 dBFS: GR {r[1][0]:.2f} dB (core {r[0][0]:.2f}, {r[0][0] - r[1][0]:.2f} dB less); continuous sine: {r[1][1]:.2f} dB (core {r[0][1]:.2f}) (want >= 6 dB less on pulses, within 0.5 dB on a sine)")

def la_release(hold, lvl=-6):
    x = np.concatenate([sine(1000, -80, 0.2), sine(1000, lvl, hold), sine(1000, -80, 40.0)])
    _, gr = run(x, voicing=2, pr=0.5, limit=0)
    at = int((0.2 + hold) * SR); top = float(gr[at - 1])
    t = lambda f: cross_time(gr, at, f * top, SR, rising=False)
    return top, t(0.5), t(0.1)

def L1():
    s_top, s50, s10 = la_release(0.2); l_top, l50, l10 = la_release(5.0)
    ts, tl = s10 - s50, l10 - l50
    ok = (0.040 <= s50 <= 0.100 and 0.040 <= l50 <= 0.120 and 0.8 <= ts <= 2.3 and 8.0 <= tl <= 20.0 and tl >= 4 * ts and s_top > 9 and l_top > 9)
    report("L1", ok, f"LA release after {s_top:.1f} dB: first half in {s50 * 1000:.0f} ms (0.2 s pressed) / {l50 * 1000:.0f} ms (5 s pressed); the long tail (half -> 10 %) takes "
                     f"{ts:.2f} s after 0.2 s of pressing, {tl:.2f} s after 5 s ({tl / max(ts, 1e-9):.1f}x) (want first half 40-100/120 ms, tail 0.8-2.3 s / 8-20 s, >= 4x)")

def L2():
    def t63(hi):
        x = np.concatenate([sine(1000, -80, 0.3), sine(1000, hi, 3.0)])
        _, gr = run(x, voicing=2, pr=0.5, limit=0)
        settled = float(np.mean(gr[-4800:])); return cross_time(gr, int(0.3 * SR), 0.632 * settled, SR) * 1000, settled
    (ta, ga), (tb, gb), (tc, gc) = t63(-24), t63(-12), t63(-3)
    def slope(limit):
        lv = np.arange(-10, 1, 5)
        _, gr = run(np.concatenate([sine(1000, l, 2.0) for l in lv]), voicing=2, pr=0.5, limit=limit)
        g = [float(np.mean(gr[int((2 * i + 1.8) * SR):int((2 * i + 2) * SR)])) for i in range(len(lv))]
        return (g[-1] - g[0]) / (lv[-1] - lv[0]), g
    sc, _ = slope(0); sl, _ = slope(1)
    prs = []
    for pr in (0.0, 0.3, 0.5, 0.7, 0.9):
        _, gr = run(sine(1000, -12, 3.0), voicing=2, pr=pr); prs.append(float(np.mean(gr[-SR // 2:])))
    mono = prs[0] == 0.0 and all(prs[i + 1] >= prs[i] + 1.5 for i in range(len(prs) - 1))
    ok = (7 <= tb <= 14 and tc <= 0.8 * tb and ta >= 1.3 * tb and abs(sc - 2 / 3) <= 0.06 and abs(sl - (1 - 1 / 12)) <= 0.06 and mono)
    report("L2", ok, f"LA: attack to 63 % {ta:.1f} ms at {ga:.1f} dB GR, {tb:.1f} ms at {gb:.1f} dB, {tc:.1f} ms at {gc:.1f} dB; slope GR/level {sc:.3f} COMPRESS (3:1 -> 0.667), {sl:.3f} LIMIT (12:1 -> 0.917); "
                     f"PEAK REDUCTION 0 .. 0.9 gives GR {' / '.join(f'{v:.1f}' for v in prs)} dB (want 7-14 ms, falling with demand, slopes within 0.06, rising >= 1.5 dB per step)")

def L3():
    res = {}
    for L, pr in ((-12, 0.5), (-24, 0.8), (-12, 0.8)):
        y, gr = tone_steady(1000, L, voicing=2, pr=pr); res[(L, pr)] = (float(gr[-1]),) + share(y, 1000)
    y, gr0 = tone_steady(1000, -20, voicing=2, pr=0.0); t0 = share(y, 1000)[0]
    a, b, c = res[(-12, 0.5)], res[(-24, 0.8)], res[(-12, 0.8)]
    ok = 0.3 <= a[1] <= 1.0 and a[2] >= 5 * a[3] and c[1] > a[1] and t0 < 0.01 and a[3] >= 0.02 and float(gr0[-1]) == 0.0
    report("L3", ok, f"LA colour, 1 kHz at -12 dBFS: THD {a[1]:.3f} % at GR {a[0]:.1f} dB (H2 {a[2]:.3f} %, H3 {a[3]:.3f} %, even/odd {a[2] / max(a[3], 1e-9):.0f}x), "
                     f"{c[1]:.3f} % at GR {c[0]:.1f} dB; -24 dBFS at GR {b[0]:.1f} dB: {b[1]:.3f} %; no GR {t0:.5f} % (want 0.3-1.0, H2 >= 5x H3, rising, < 0.01 at 0)")

def kick(secs=3.0, f=60, dur=0.1, lvl=-6):
    x = np.zeros(int(secs * SR), np.float32); n = int(dur * SR); t = np.arange(n) / SR
    x[int(0.2 * SR):int(0.2 * SR) + n] = (amp(lvl) * np.sin(2 * np.pi * f * t)).astype(np.float32); return x

def shape_fn(shape, p):
    return {0: np.exp(-np.log(10) * p), 1: np.maximum(0, 1 - 0.9 * p), 2: np.maximum(0, 1 - 0.9 * p * p)}[shape]

def P1():
    # After the key lets go the duck is 90 % gone after one note value; the time between 90 % and 10 % remaining is
    # what the shape and the tempo dictate. Also the same 10 % point sits one note value after the release started, whatever
    # the tempo, note or shape: the offset from the key's end is the detector's own (hold + drop).
    pad = sine(220, -12, 3.0); key = kick(); worst = 0.0; where = ""; offs = []
    pgrid = np.linspace(0, 2, 200001)
    for note, bpm in ((0.0625, 120), (0.125, 120), (0.25, 100), (0.5, 140), (0.125, 90), (0.0625, 150)):
        for shape in (0, 1, 2):
            _, gr = run(pad, key=key, ext=1, voicing=3, threshold=-30, ratio=10, attack=0.5, bpm=bpm, note=note, shape=shape, depth=12)
            plat = float(np.max(gr)); after = gr[int(np.argmax(gr)):]
            ta, tb = int(np.argmax(after <= 0.9 * plat)), int(np.argmax(after <= 0.1 * plat))
            s = shape_fn(shape, pgrid); T = note * 240 / bpm
            want = (pgrid[np.argmax(s <= 0.1)] - pgrid[np.argmax(s <= 0.9)]) * T
            err = abs((tb - ta) / SR / want - 1)
            if err > worst: worst, where = err, f"{note} at {bpm} bpm, shape {shape}"
            offs.append((int(np.argmax(gr)) + tb) / SR - (0.3 + T))      # key ends at 0.3 s
    spread = (max(offs) - min(offs)) * 1000
    report("P1", worst <= 0.02 and spread <= 3.0, f"PUMP recovery: 90 % -> 10 % time vs the tempo and shape, 6 tempo/note pairs x exp/linear/swell: worst error {worst * 100:.2f} % ({where}); "
                                                  f"the 10 % point lands one note value + {min(offs) * 1000:.1f}..{max(offs) * 1000:.1f} ms after the key ends (spread {spread:.2f} ms) (want <= 2 %, spread <= 3 ms)")

def P2():
    pad = sine(220, -12, 3.0); key = kick(); errs = []
    for depth in (0, 3, 6, 12, 18, 24):
        _, gr = run(pad, key=key, ext=1, voicing=3, threshold=-40, ratio=10, attack=30, depth=depth)
        errs.append(float(np.max(gr)) - depth)
    # attack on a key with an instant onset (a 1 kHz burst), the knob at its slowest: PUMP holds it to its own ceiling
    burst = np.zeros(int(1.5 * SR), np.float32); burst[int(0.2 * SR):int(0.7 * SR)] = sine(1000, -6, 0.5)
    _, gr = run(pad[:len(burst)], key=burst, ext=1, voicing=3, threshold=-40, ratio=10, attack=30, depth=12)
    plat = float(np.max(gr)); t63 = cross_time(gr, int(0.2 * SR), 0.632 * plat, SR) * 1000
    lv = []
    for kl in (-24, -18, -12, -6):
        _, g = run(pad, key=kick(lvl=kl), ext=1, voicing=3, threshold=-30, ratio=10, depth=12); lv.append(float(np.max(g)))
    _, g_nokey = run(sine(220, -3, 2.0), voicing=3, threshold=-30, ext=1, depth=12)
    _, g_int = run(sine(220, -3, 2.0), voicing=3, threshold=-30, ext=0, depth=12)
    g_key_off = run(sine(220, -50, 3.0), key=key, voicing=3, threshold=-30, ext=0, depth=12)[1]     # a quiet programme: only the key could duck it
    ok = (max(abs(e) for e in errs) <= 0.15 and t63 <= 1.0 and all(lv[i + 1] >= lv[i] for i in range(3)) and lv[1] >= lv[0] + 3 and lv[0] < 12 and lv[-1] >= 11.85
          and float(np.max(g_nokey)) < 0.1 and abs(float(np.max(g_int)) - 12) <= 0.15 and float(np.max(g_key_off)) < 0.1)
    report("P2", ok, f"PUMP depth: GR minus DEPTH at 0/3/6/12/18/24 dB: {' '.join(f'{e:+.3f}' for e in errs)} dB; attack (knob 30 ms, held to its ceiling) reaches 63 % in {t63:.2f} ms; "
                     f"a kick at -24/-18/-12/-6 dBFS ducks {' / '.join(f'{v:.1f}' for v in lv)} dB; EXT on, no key {float(np.max(g_nokey)):.2f} dB; internal key {float(np.max(g_int)):.2f} dB; "
                     f"EXT off ignores the key {float(np.max(g_key_off)):.2f} dB (want |err| <= 0.15, <= 1 ms, a softer kick ducks less up to DEPTH, no key < 0.1, internal 12 +- 0.15)")

def V1():
    # Nulls and latency, per voicing (the core's M10 and M11).
    rng = np.random.default_rng(1); x = (rng.standard_normal((48000, 2)) * 0.2).astype(np.float32); quiet = (x * 0.25).astype(np.float32)
    imp = np.zeros(4800, np.float32); imp[0] = 0.5; txt = []; ok = True
    for v, kw in ((1, dict(threshold=-30, ratio=10)), (2, dict(pr=0.9)), (3, dict(threshold=-30, depth=24))):
        a = np.array_equal(run(x, voicing=v, drive=1, makeup=6, **{"in": 0}, **kw)[0], delayed(x))
        b = np.array_equal(run(x, voicing=v, drive=1, makeup=6, mix=0, **kw)[0], delayed(x))
        c = np.array_equal(run(quiet, voicing=v, **({"pr": 0.0} if v == 2 else {"threshold": 0}))[0], delayed(quiet))
        l0 = int(np.argmax(np.abs(run(imp, voicing=v, **({"pr": 0.0} if v == 2 else {"threshold": 0}))[0][:, 0])))
        l1 = int(np.argmax(np.abs(run(imp, voicing=v, **kw)[0][:, 0])))
        ok &= a and b and c and l0 == LAT and l1 == LAT
        txt.append(f"voicing {v}: IN off {'exact' if a else 'DIFFERS'}, MIX 0 {'exact' if b else 'DIFFERS'}, no GR {'exact' if c else 'DIFFERS'}, impulse at {l0} / {l1} (coloured)")
    report("V1", ok, "; ".join(txt) + f" (want exact, {LAT})")

def V2():
    x = busy(); bad = []
    base = dict(threshold=-28, ratio=12, attack=3, release=200, makeup=4, mix=0.8, drive=0.5, hpf=120, bpm=128, note=0.125, depth=15, pr=0.6)
    at = [(20011, "threshold", -12), (31337, "ratio", 15), (40001, "makeup", 9), (52345, "mix", 0.4), (60013, "drive", 0.1), (70007, "auto", 1),
          (81111, "attack", 0.3), (90001, "pr", 0.9), (100003, "in", 0), (110017, "in", 1), (120001, "release", 900), (125001, "depth", 6),
          (130001, "bpm", 90), (135001, "note", 0.25), (138001, "shape", 2), (140003, "limit", 1)]
    for v in (1, 2, 3):
        ref = run(x, block=512, at=at, voicing=v, **base)[0]
        for b in (1, 7, 64, 4096):
            if not np.array_equal(run(x, block=b, at=at, voicing=v, **base)[0], ref): bad.append(f"v{v}/{b}")
        if not np.array_equal(run(x, blocks_random=True, at=at, voicing=v, **base)[0], ref): bad.append(f"v{v}/random")
    # and a voicing switched mid-file
    sw = [(30011, "voicing", 2), (70001, "voicing", 3), (110003, "voicing", 1), (131003, "voicing", 0)]
    ref = run(x, block=512, at=sw + at, voicing=1, **base)[0]
    for b in (1, 7, 4096):
        if not np.array_equal(run(x, block=b, at=sw + at, voicing=1, **base)[0], ref): bad.append(f"switch/{b}")
    if not np.array_equal(run(x, blocks_random=True, at=sw + at, voicing=1, **base)[0], ref): bad.append("switch/random")
    report("V2", not bad, f"block sizes 1, 7, 64, 512, 4096 and random, 16 control jumps and 4 voicing switches mid-file, voicings 1-3: {'bit-identical' if not bad else 'DIFFER at ' + ','.join(bad)}")

def V3():
    sr = 48000; x = busy(20.0, seed=3) * 3.0
    x[1000:1100] = np.nan; x[5000:5010] = np.inf; x[9000:9200] = -np.inf; x[20000:21000] = 1e-39; x[40000:48000] *= 40.0
    x = np.concatenate([x, np.zeros((sr * 4, 2), np.float32)]).astype(np.float32)
    key = np.copy(x)[::-1].copy(); txt = []; ok = True
    for v, kw in ((1, dict(threshold="-40:0", ratio="1.5:20", attack="0.1:30", release="100:1200", makeup="0:20", mix="0.2:1", drive="0:1", hpf="20:300")),
                  (2, dict(pr="0:1", makeup="0:20", mix="0.2:1", drive="0:1")),
                  (3, dict(threshold="-40:0", ratio="10:20", depth="0:24", bpm="60:200", note="0.0625:0.5", makeup="0:20", mix="0.2:1", drive="0:1"))):
        out, gr = run(x, voicing=v, key=key, **kw)
        out2, gr2 = run(x, voicing=v, key=key, ext=1, threshold=-40, ratio=20, makeup=20, drive=1, limit=1, pr=1, depth=48, auto=1, attack=0.1)
        loud = (np.random.default_rng(5).uniform(-1, 1, (sr * 8, 2)) * 40).astype(np.float32)          # 8 s of +32 dBFS noise: the gain reduction must still stop at 60 dB
        out3, gr3 = run(loud, voicing=v, threshold=-60, ratio=100, attack=0.1, auto=1, release=1200, depth=48, pr=1, limit=1)
        fin = bool(np.all(np.isfinite(out)) and np.all(np.isfinite(out2)) and np.all(np.isfinite(out3)) and np.all(np.isfinite(gr)) and np.all(np.isfinite(gr2)) and np.all(np.isfinite(gr3)))
        tail = max(float(np.max(np.abs(out[-sr:]))), float(np.max(np.abs(out2[-sr // 2:]))))
        mx = max(float(gr.max()), float(gr2.max()), float(gr3.max())); ok &= fin and mx <= 60.0 and tail == 0.0
        txt.append(f"voicing {v}: finite {fin}, max GR {mx:.1f} dB, tail {tail:.1e}")
    report("V3", ok, "hostile input (NaN, inf, denormals, +40 dBFS, garbage key) with every control moving: " + "; ".join(txt) + " (want finite, <= 60, tail exactly 0)")

def V4():
    x = busy(60.0); ts = []
    for v, kw in ((1, dict(threshold=-30, auto=1, drive=0.5)), (2, dict(pr=0.7, drive=0.5)), (3, dict(threshold=-30, drive=0.5, depth=12))):
        t0 = time.time(); run(x, voicing=v, want_gr=False, **kw); ts.append(time.time() - t0)
    report("V4", max(ts) < 3.0, f"60 s stereo at 48 kHz, GLUE / LA / PUMP with DRIVE 50 %: {ts[0]:.2f} / {ts[1]:.2f} / {ts[2]:.2f} s including file I/O (want < 3)")

def V5():
    # Switching voicing under a tone must not click: the gain reduction is handed over.
    x = sine(1000, -12, 1.0); own = float(np.max(np.abs(np.diff(x)))); worst = 0.0; where = ""
    for first, second in ((0, 1), (1, 2), (2, 3), (3, 1), (1, 3), (2, 1)):
        kw = dict(threshold=-18, ratio=10, attack=10, release=300, pr=0.5, depth=6, note=0.125)
        out = run(x, voicing=first, at=[(24011, "voicing", second)], **kw)[0][:, 0]
        r = float(np.max(np.abs(np.diff(out[24011 - 200:24011 + 4800])))) / own
        if r > worst: worst, where = r, f"{first} -> {second}"
    report("V5", worst <= 1.5, f"voicing switched mid-cycle under a 1 kHz tone, six pairs: largest step {worst:.2f}x the sine's own ({where}) (want <= 1.5)")

# ---------------------------------------------------------------------------------------------
# GLUE on a clean synthetic drum bus (tools/synth_session.py): the owner's "bir kendisi bir golgesi var" (pack v2) as numbers.
sys.path.insert(0, HERE)
import synth_session as sy
from scipy.ndimage import uniform_filter1d

_DRUMS = {}
def drums():
    """(drum bus, events, strong hit times) of the 16-bar synthetic session (cached)."""
    if not _DRUMS:
        d = sy.build(); _DRUMS["x"] = sy.drumbus(d["stems"]); _DRUMS["ev"] = d["events"]
    return _DRUMS["x"], _DRUMS["ev"]

def hits_of(events):
    """Strong hits (kick, snare backbeat, toms) as (t0, t_next): t_next is the next kick/snare/tom of any velocity (ghosts included)."""
    allh = sorted(e["t"] for e in events if e["inst"] in ("kick", "snare", "tom"))
    out = []
    for e in events:
        if e["inst"] in ("kick", "snare", "tom") and e["vel"] >= 0.5:
            nxt = next((t for t in allh if t > e["t"] + 0.02), e["t"] + 1.0); out.append((e["t"], nxt))
    return sorted(out)

def env_rms_db(x, sr=SR, ms=10.0):
    """RMS envelope in dB over a `ms` window, one value per millisecond (a window shorter than half a kick cycle would ripple by itself)."""
    a = x.astype(np.float64); a = np.max(np.abs(a), axis=1) if a.ndim == 2 else np.abs(a)
    return 10 * np.log10(np.maximum(uniform_filter1d(a * a, int(ms * sr / 1000))[::sr // 1000], 1e-14))

def aligned(x, sr=SR, **kw):
    """The engine's output with its latency taken out, plus the gain reduction."""
    y, gr = run(x, sr=sr, **kw)
    return np.concatenate([y[LAT:], np.zeros((LAT, 2), np.float32)]), gr

def rebound(e): return float(np.max(e - np.minimum.accumulate(e)))

def shadow_stats(dry, wet, hits, sr=SR):
    """The ducked shadow, per hit, from the audio alone (output envelope against the dry one, so snap and colour count too). dB.
    dip:     how much deeper than just before the hit the output is pushed below the dry envelope, anywhere from 4 ms after the hit to the next hit
             (the effective gain's hole after the attack; a continuous glue leaves none, a per-hit duck leaves its own).
    rebound: how far the output envelope rises from its own earlier minimum after the attack, over what the dry envelope does by itself
             (a second hump: dip and recovery inside the tail)."""
    ed, ew = env_rms_db(dry, sr, 6.0), env_rms_db(wet, sr, 6.0); g = ew - ed; dip, reb = [], []
    for t0, nxt in hits:
        a = int(t0 * 1000); b = int(min(nxt - 0.003, t0 + 0.25) * 1000)
        if a < 30 or b - a < 40 or t0 + 0.3 > len(dry) / sr: continue          # not the first or last hit of the file (no history / no tail)
        dip.append(max(0.0, float(np.median(g[a - 25:a - 3]) - np.min(g[a + 4:b]))))
        pk = a + int(np.argmax(ew[a:a + 12])); st = pk + 10
        reb.append(max(0.0, rebound(ew[st:b]) - rebound(ed[st:b])) if b - st >= 30 else 0.0)
    return np.array(dip), np.array(reb)

def ripple_stats(gr, hits, sr=SR):
    """Per hit: gain reduction on the hit (its maximum in the first 60 ms) minus the gain reduction just before the next hit. dB."""
    out = []
    for t0, nxt in hits:
        if nxt - t0 < 0.14 or nxt - t0 > 0.8: continue
        a = int(t0 * sr); b = int(min(t0 + 0.06, nxt - 0.01) * sr)
        out.append(float(np.max(gr[a:b]) - gr[int((nxt - 0.003) * sr)]))
    return np.array(out)

def search_p90(x, kw, target, control="threshold", lo=-40.0, hi=0.0, iters=9):
    """Bisection of the threshold until the 90th percentile of the gain reduction is `target` dB."""
    for _ in range(iters):
        mid = 0.5 * (lo + hi); gr = run(x, want_gr=True, **{**kw, control: mid})[1]
        if float(np.percentile(gr, 90)) > target: lo = mid
        else: hi = mid
    return round(0.5 * (lo + hi), 2)

GLUE_SETTINGS = {   # the settings the glue checks and the listening pack use: name -> engine params (threshold is searched)
    "Yapistir (4:1, atak 10, AUTO)": dict(ratio=4, attack=10, auto=1),
    "Yumusak (2:1, atak 30, 0.6 s)": dict(ratio=2, attack=30, release=600),
    "Hizli (4:1, atak 3, 0.3 s)": dict(ratio=4, attack=3, release=300),
}

# Thresholds that give the drum bus a 90th percentile gain reduction of about 4 dB at each setting (found with search_p90 on the default SC HPF).
GLUE_THR = {"Yapistir (4:1, atak 10, AUTO)": -32.46, "Yumusak (2:1, atak 30, 0.6 s)": -34.57, "Hizli (4:1, atak 3, 0.3 s)": -32.46}
_GLUE = {}
def glue_runs():
    """Drum bus through GLUE at the three glue settings: name -> (wet aligned, gr)."""
    if not _GLUE:
        x, _ = drums()
        for name, kw in GLUE_SETTINGS.items():
            _GLUE[name] = aligned(x, voicing=1, threshold=GLUE_THR[name], **kw)
    return _GLUE

def G4():
    # No shadow: on the drum bus, per hit (kick, snare backbeat, toms), the output is not pushed down below the dry envelope after the hit by more than
    # a continuous glue does, and there is no second hump in the tail.
    x, ev = drums(); hits = hits_of(ev); txt = []; ok = True
    for name, (wet, gr) in glue_runs().items():
        dip, reb = shadow_stats(x, wet, hits); p90 = float(np.percentile(gr, 90))
        ok &= 3.5 <= p90 <= 4.8 and np.percentile(dip, 95) <= 2.5 and dip.max() <= 3.0 and np.mean(dip) <= 1.5 and reb.max() <= 1.0
        txt.append(f"{name.split(' (')[0]}: GR p90 {p90:.1f}, dip mean {np.mean(dip):.2f} / p95 {np.percentile(dip, 95):.2f} / max {dip.max():.2f}, second hump max {reb.max():.2f} dB")
    report("G4", ok, f"GLUE shadow, drum bus, {len(dip)} hits: " + "; ".join(txt) + " (want GR p90 3.5-4.8, dip mean <= 1.5, p95 <= 2.5, max <= 3.0, second hump <= 1.0 dB; the v2 engine had dip p95 5.4, max 5.5)")

def G5():
    # Continuity: the gain reduction between hits stays within ~2 dB of the gain reduction on the hits, and does not fall back to zero across the bar.
    x, ev = drums(); hits = hits_of(ev); txt = []; ok = True
    for name, (wet, gr) in glue_runs().items():
        rp = ripple_stats(gr, hits); body = gr[int(4.8 * SR):]; p90 = float(np.percentile(gr, 90))
        bars = [gr[int((4.8 + 2.4 * k) * SR):int((7.2 + 2.4 * k) * SR)] for k in range(13)]
        swing = max(float(np.percentile(b, 95) - np.percentile(b, 5)) for b in bars)
        ok &= np.percentile(rp, 95) <= 2.2 and rp.max() <= 2.8 and np.mean(rp) <= 1.2 and body.min() >= 0.35 * p90 and swing <= 4.5
        txt.append(f"{name.split(' (')[0]}: on-hit minus between-hit GR mean {np.mean(rp):.2f} / p95 {np.percentile(rp, 95):.2f} / max {rp.max():.2f}, GR floor after 4.8 s {body.min():.2f} dB, largest in-bar p95-p5 {swing:.2f}")
    report("G5", ok, f"GLUE continuity, drum bus, {len(rp)} hits: " + "; ".join(txt) + " (want p95 <= 2.2, max <= 2.8, mean <= 1.2, floor >= 35 % of p90, in-bar swing <= 4.5 dB (the tom fills); the v2 engine had p95 5.3, max 6.1)")

def G6():
    # SC HPF defaults to 200 Hz in GLUE (a negative `hpf` means the voicing's default) so the kick does not drive the glue alone. Compare with 0 (the v2 default).
    x, ev = drums(); n8 = int(8 * SR); thr = GLUE_THR["Yapistir (4:1, atak 10, AUTO)"]; kw = dict(voicing=1, ratio=4, attack=10, auto=1, threshold=thr)
    a, _ = run(x[:n8], **kw); b, _ = run(x[:n8], hpf=200, **kw); c, _ = run(x[:n8], hpf=0, **kw)
    same = bool(np.array_equal(a, b)) and not np.array_equal(a, c)
    low = sine(50, -12, 8.0); g_def = float(np.mean(run(low, voicing=1, threshold=-20, ratio=4)[1][-SR:])); g_off = float(np.mean(run(low, voicing=1, threshold=-20, ratio=4, hpf=0)[1][-SR:]))
    S = sy.build()["stems"]; res = {}
    for hz in (-1, 0):
        res[hz] = {n: float(np.mean(run(S[n], hpf=hz, **kw)[1])) for n in ("kick", "snare")}; res[hz]["bus"] = float(np.mean(run(x, hpf=hz, **kw)[1]))
    kd, ko = res[-1]["kick"] / res[-1]["bus"], res[0]["kick"] / res[0]["bus"]; sd = res[-1]["snare"] / res[-1]["bus"]
    ok = same and g_off - g_def >= 5.0 and kd <= 0.75 and sd >= 0.25
    report("G6", ok, f"GLUE SC HPF default: default == hpf 200 bit-exact and differs from hpf 0 {same}; 50 Hz tone at -12 dBFS (threshold -20): GR {g_def:.2f} dB vs {g_off:.2f} with the HPF off; "
                     f"drum stems fed alone, share of the bus's mean GR: kick {100 * kd:.0f} % (HPF off {100 * ko:.0f} %), snare {100 * sd:.0f} % (HPF off {100 * res[0]['snare'] / res[0]['bus']:.0f} %) "
                     f"(want same, >= 5 dB less on the 50 Hz tone, kick alone <= 75 %, snare alone >= 25 %)")

def G7():
    # The release knob still orders GLUE's tail: after 4 s of programme the gain reduction lets go over a time set by the slow stage
    # (max of 1.6 s and 4 x the knob; AUTO 1.6 s). Time for the gain reduction to fall to half.
    x = np.concatenate([sine(1000, -6, 4.0), sine(1000, -80, 14.0)]); at = 4 * SR; t = {}
    for name, kw in (("0.3 s", dict(release=300)), ("1.2 s", dict(release=1200)), ("AUTO", dict(auto=1))):
        _, gr = run(x, voicing=1, threshold=-30, ratio=4, attack=10, hpf=0, **kw); top = float(gr[at - 1]); t[name] = cross_time(gr, at, 0.5 * top, SR, rising=False)
    ok = t["1.2 s"] >= 2.5 * t["0.3 s"] and 0.8 <= t["0.3 s"] <= 1.6 and 0.8 <= t["AUTO"] <= 1.6
    report("G7", ok, f"GLUE tail, GR to half after 4 s of programme: release 0.3 s {t['0.3 s']:.2f} s, 1.2 s {t['1.2 s']:.2f} s ({t['1.2 s'] / t['0.3 s']:.1f}x), AUTO {t['AUTO']:.2f} s (want 1.2 s >= 2.5x the 0.3 s, 0.3 s and AUTO within 0.8-1.6 s)")

def G8():
    """Calibration: at the same THRESHOLD / RATIO / ATTACK as a classic bus compressor (stereo-linked peak detector,
    log domain, attack 10 ms, release 0.3 s), GLUE's mean GR is within 1.5 dB of it (owner: Glue at -18 compresses, TUTKAL barely did)."""
    import soundfile as sf
    sess = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../prototypes/DINLE/10_TUTKAL_v3/_session")
    res, ok = [], True
    for name in ("DRY_drumbus", "DRY_fullmix"):
        x, sr = sf.read(os.path.join(sess, name + ".wav"), dtype="float32", always_2d=True)
        x = x[: 20 * sr]
        m = db(np.max(np.abs(x), axis=1))
        for thr in (-12.0, -18.0, -24.0):
            tgt = np.maximum(m - thr, 0.0) * 0.75
            aA, aR, st = np.exp(-1 / (0.010 * sr)), np.exp(-1 / (0.3 * sr)), 0.0
            ref = np.empty_like(tgt)
            for i, t in enumerate(tgt):
                st = aA * st + (1 - aA) * t if t > st else aR * st + (1 - aR) * t
                ref[i] = st
            _, gr = run(x, sr=sr, voicing=1, threshold=thr, ratio=4, attack=10, auto=1, raw_threshold=True)
            g = np.abs(gr); d = float(g.mean() - ref.mean())
            ok &= abs(d) <= 1.5
            res.append(f"{name[4:]} {thr:.0f}: {g.mean():.1f} vs {ref.mean():.1f}")
    report("G8", ok, "GLUE vs a peak bus comp, 4:1 att 10 AUTO, mean GR (dB): " + "; ".join(res) + " (want within 1.5 dB)")

CHECKS = [M1, M2, M3, M4, M5, M6, M7, M8, M9, M10, M11, M12, M13, M14, M15, M16, M17, M18, G1, G2, G3, G4, G5, G6, G7, G8, L1, L2, L3, P1, P2, V1, V2, V3, V4, V5]

if __name__ == "__main__":
    args = sys.argv[1:]
    if args and args[0] == "--probe":
        probe(args[1], float(args[2]) if len(args) > 2 else 12.0); sys.exit(0)
    for c in CHECKS:
        if not args or c.__name__ in args:
            c()
    print(f"\n{'ALL PASS' if fails == 0 else str(fails) + ' FAILED'}")
    sys.exit(fails)
