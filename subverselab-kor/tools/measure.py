#!/usr/local/bin/python3
"""KOR engine measurements (SPEC.md, M1-M13). Runs build/korraw, the engine as the plugin will run it.

  measure.py            every check; exit code = number of failures
  measure.py M4 M12     only those

numpy/scipy only. Test signals are generated here: sines, pink noise, a log sine sweep and a synthetic drum
loop (kick = decaying 55 Hz sine with a pitch drop, snare = noise burst + 190 Hz, hats = high-passed noise,
120 bpm, 8 bars).
"""
import os, subprocess, sys, time, tempfile
import numpy as np
import scipy.signal as ss

HERE = os.path.dirname(os.path.abspath(__file__))
RAW = os.environ.get("KORRAW", os.path.join(HERE, "..", "build", "korraw"))
TMP = tempfile.mkdtemp(prefix="kor-")
fails = 0


def reported_latency():
    return int(subprocess.check_output([RAW, "--latency"]).decode().strip())


def run(x, sr=48000, block=None, blocks_random=False, mono=False, **params):
    """x: (n,) or (n,2) float32. Returns the output, same shape as the (stereo-expanded) input."""
    x = np.asarray(x, dtype=np.float32)
    if x.ndim == 1 and not mono:
        x = np.stack([x, x], axis=1)
    fi, fo = os.path.join(TMP, "in.f32"), os.path.join(TMP, "out.f32")
    np.ascontiguousarray(x).tofile(fi)
    cmd = [RAW, fi, fo, str(sr)] + [f"{k}={v}" for k, v in params.items()]
    if block: cmd += ["--block", str(block)]
    if blocks_random: cmd += ["--blocks", "random"]
    if mono: cmd += ["--mono"]
    subprocess.run(cmd, check=True)
    out = np.fromfile(fo, dtype=np.float32)
    return out if mono else out.reshape(-1, 2)


def report(tag, ok, measured, want):
    global fails
    print(f"{tag:<4} {measured} | want {want} | {'PASS' if ok else 'FAIL'}", flush=True)
    if not ok:
        fails += 1


# ---------------------------------------------------------------------------------------------
# Signals
def db(a): return 20 * np.log10(np.maximum(np.abs(a), 1e-300))
def amp(d): return 10 ** (d / 20)


def sine(f, level_db, secs, sr=48000):
    return (amp(level_db) * np.sin(2 * np.pi * f * np.arange(int(round(secs * sr))) / sr)).astype(np.float32)


def pink(secs, rms_db=-18.0, sr=48000, seed=2, channels=2):
    """Pink noise (1/f power), each channel independent, each at rms_db dBFS RMS."""
    rng = np.random.default_rng(seed)
    n = int(round(secs * sr))
    out = np.empty((n, channels))
    f = np.fft.rfftfreq(n, 1 / sr)
    shape = np.zeros_like(f); shape[1:] = 1 / np.sqrt(f[1:])
    shape[f < 10] = 0
    for c in range(channels):
        y = np.fft.irfft(np.fft.rfft(rng.standard_normal(n)) * shape, n)
        out[:, c] = y * amp(rms_db) / np.sqrt(np.mean(y ** 2))
    return out.astype(np.float32)


def sweep(secs=10.0, level_db=-6.0, sr=48000, f0=20.0, f1=20000.0):
    t = np.arange(int(round(secs * sr))) / sr
    return (amp(level_db) * ss.chirp(t, f0, secs, f1, method="logarithmic", phi=-90)).astype(np.float32)


def drums(sr=48000, bars=8, bpm=120.0, peak_db=-1.0, seed=3):
    """Synthetic drum loop, mono. 4/4: kick on 1 and 3 (and the 'and' of 3), snare on 2 and 4, hats on eighths."""
    rng = np.random.default_rng(seed)
    beat = 60.0 / bpm
    n = int(round(bars * 4 * beat * sr))
    y = np.zeros(n + sr)

    def add(at, sig):
        i = int(round(at * sr)); y[i:i + len(sig)] += sig

    tk = np.arange(int(0.5 * sr)) / sr
    fk = 55.0 + 95.0 * np.exp(-tk / 0.025)                       # pitch drop to 55 Hz
    kick = np.sin(2 * np.pi * np.cumsum(fk) / sr) * np.exp(-tk / 0.22)
    kick[:int(0.002 * sr)] *= np.linspace(0, 1, int(0.002 * sr))
    ts = np.arange(int(0.35 * sr)) / sr
    def snare():
        return 0.55 * rng.standard_normal(len(ts)) * np.exp(-ts / 0.07) + 0.6 * np.sin(2 * np.pi * 190 * ts) * np.exp(-ts / 0.10)
    th = np.arange(int(0.12 * sr)) / sr
    b, a = ss.butter(4, 7000, "highpass", fs=sr)
    def hat():
        return 0.35 * ss.lfilter(b, a, rng.standard_normal(len(th))) * np.exp(-th / 0.025)

    for bar in range(bars):
        t0 = bar * 4 * beat
        for k in (0, 2, 2.5): add(t0 + k * beat, kick)
        for k in (1, 3): add(t0 + k * beat, snare())
        for k in range(8): add(t0 + k * beat / 2, hat() * (1.0 if k % 2 == 0 else 0.6))
    y = y[:n]
    return (y * amp(peak_db) / np.max(np.abs(y))).astype(np.float32)


def corpus():
    """M5 / M6 corpus, stereo: drum loop (16 s, peak -1 dBFS), pink (10 s, -18 dBFS RMS), log sweep 20 Hz-20 kHz (10 s, -6 dBFS)."""
    d = drums(); s = sweep()
    return np.concatenate([np.stack([d, d], 1), pink(10.0, seed=4), np.stack([s, s], 1)]).astype(np.float32)


# ---------------------------------------------------------------------------------------------
# Analysis
def spectrum_db(y, sr):
    """Kaiser (beta 20) windowed amplitude spectrum in dB, a sine of amplitude A reads 20 log10(A)."""
    w = ss.windows.kaiser(len(y), 20.0)
    return np.fft.rfftfreq(len(y), 1 / sr), db(np.abs(np.fft.rfft(y * w)) / (np.sum(w) / 2))


def harmonics(y, f0, sr, nmax):
    """Amplitudes of harmonics 1..nmax (below Nyquist) of f0. y must hold a whole number of cycles (coherent bins)."""
    spec = np.abs(np.fft.rfft(y)) / (len(y) / 2)
    out = []
    for k in range(1, nmax + 1):
        b = k * f0 * len(y) / sr
        assert abs(b - round(b)) < 1e-9
        out.append(spec[int(round(b))] if round(b) < len(spec) - 1 else 0.0)
    return np.array(out)


def thd_db(y, f0, sr):
    nmax = int(20000 // f0)
    h = harmonics(y, f0, sr, nmax)
    return float(db(np.sqrt(np.sum(h[1:] ** 2)) / max(h[0], 1e-300))), h


def k_weight(x, sr):
    """BS.1770-4 K-weighting (pre-filter shelf + RLB high-pass), coefficients derived for any sample rate."""
    f0, G, Q = 1681.974450955533, 3.999843853973347, 0.7071752369554196
    K = np.tan(np.pi * f0 / sr); Vh = 10 ** (G / 20); Vb = Vh ** 0.4996667741545416
    a0 = 1 + K / Q + K * K
    b1 = [(Vh + Vb * K / Q + K * K) / a0, 2 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0]
    a1 = [1, 2 * (K * K - 1) / a0, (1 - K / Q + K * K) / a0]
    f0, Q = 38.13547087602444, 0.5003270373238773
    K = np.tan(np.pi * f0 / sr)
    a0 = 1 + K / Q + K * K
    b2 = [1, -2, 1]
    a2 = [1, 2 * (K * K - 1) / a0, (1 - K / Q + K * K) / a0]
    return ss.lfilter(b2, a2, ss.lfilter(b1, a1, x, axis=0), axis=0)


def loudness(x, sr=48000):
    """BS.1770-4 integrated loudness (LUFS), gated: 400 ms blocks, 75 % overlap, -70 LUFS absolute, -10 LU relative."""
    x = np.asarray(x, dtype=np.float64)
    if x.ndim == 1: x = x[:, None]
    y = k_weight(x, sr)
    blk, hop = int(round(0.4 * sr)), int(round(0.1 * sr))
    starts = np.arange(0, len(y) - blk + 1, hop)
    cs = np.vstack([np.zeros((1, y.shape[1])), np.cumsum(y ** 2, axis=0)])
    z = (cs[starts + blk] - cs[starts]) / blk                     # mean square per block, per channel
    zsum = np.sum(z, axis=1)                                      # channel weights 1 (L, R)
    lk = -0.691 + 10 * np.log10(np.maximum(zsum, 1e-300))
    keep = lk > -70.0
    rel = -0.691 + 10 * np.log10(np.mean(zsum[keep])) - 10.0
    keep &= lk > rel
    return float(-0.691 + 10 * np.log10(np.mean(zsum[keep])))


def true_peak_db(x):
    """4x oversampled peak (BS.1770-4 Annex 2 method, polyphase interpolation by resample_poly)."""
    x = np.asarray(x, dtype=np.float64)
    if x.ndim == 1: x = x[:, None]
    return float(max(db(np.max(np.abs(ss.resample_poly(x[:, c], 4, 1)))) for c in range(x.shape[1])))


def delayed(x, lat):
    return np.concatenate([np.zeros((lat,) + x.shape[1:], np.float32), x[:-lat]])


# ---------------------------------------------------------------------------------------------
def worst_non_harmonic(c):
    sr, f0 = 48000, 10000.0
    out = run(sine(f0, -6, 2.5), drive=18, character=c)[:, 0]
    y = out[-2 * sr:].astype(np.float64)                         # 2 s after 0.5 s of settling
    f, s = spectrum_db(y, sr)
    ref = float(np.max(s[np.abs(f - f0) < 20]))
    keep = f > 20
    for k in range(1, int(sr / 2 // f0) + 1):
        keep &= np.abs(f - k * f0) > 20
    i = int(np.argmax(np.where(keep, s, -1e9)))
    return float(s[i] - ref), float(f[i])


def M1():
    w, at = worst_non_harmonic(0.0)
    report("M1", w <= -65, f"10 kHz -6 dBFS 48 kHz, DRIVE +18, CHARACTER 0: worst non-harmonic {w:.1f} dBc at {at:.0f} Hz", "<= -65 dBc")


def M2():
    w, at = worst_non_harmonic(1.0)
    report("M2", w <= -60, f"10 kHz -6 dBFS 48 kHz, DRIVE +18, CHARACTER 100: worst non-harmonic {w:.1f} dBc at {at:.0f} Hz", "<= -60 dBc")


def M3():
    lat = reported_latency()
    rng = np.random.default_rng(11)
    x = (rng.standard_normal((96000, 2)) * 0.3).astype(np.float32)
    ref = delayed(x, lat)
    a = np.array_equal(run(x, drive=18, character=0.7, tone=4, ceiling=-3, mix=0), ref)
    b = np.array_equal(run(x, drive=18, character=0.7, tone=4, ceiling=-3, **{"in": 0}), ref)
    report("M3", a and b, f"against the input delayed by {lat}: MIX 0 {'bit-exact' if a else 'DIFFERS'}, IN off {'bit-exact' if b else 'DIFFERS'}", "bit-exact")


def M4():
    sr = 48000
    x = sine(1000, -40, 1.5)
    worst_g, worst_t, txt = 0.0, -999.0, []
    for c in (0.0, 0.25, 0.5, 0.75, 1.0):
        y = run(x, drive=0, character=c)[-sr:, 0].astype(np.float64)
        ref = x[-sr:].astype(np.float64)
        t, h = thd_db(y, 1000, sr)
        g = float(db(h[0] / harmonics(ref, 1000, sr, 1)[0]))
        worst_g = max(worst_g, abs(g)); worst_t = max(worst_t, t)
        txt.append(f"{int(c * 100)}%: {g:+.4f} dB / {t:.1f} dB")
    report("M4", worst_g <= 0.05 and worst_t <= -80, "-40 dBFS 1 kHz, DRIVE 0, gain error / THD: " + ", ".join(txt), "<= 0.05 dB / <= -80 dB")


_corpus_runs = None
def corpus_runs():
    global _corpus_runs
    if _corpus_runs is None:
        x = corpus(); _corpus_runs = []
        for ceil in (0.0, -1.0, -6.0):
            for drive in (0, 6, 12, 18, 24):
                _corpus_runs.append((ceil, drive, run(x, drive=drive, ceiling=ceil)))
    return _corpus_runs


def M5():
    over_total, worst_frac, where, n_total = 0, 0.0, "", 0
    for ceil, drive, out in corpus_runs():
        c32 = np.float32(10 ** (ceil / 20))
        a = np.abs(out)
        over_total += int(np.sum(a > c32))
        frac = float(np.mean(a >= c32))                          # samples sitting exactly on the ceiling: the step-7 limit acted
        n_total += a.size
        if frac >= worst_frac: worst_frac, where = frac, f"CEILING {ceil:g} DRIVE {drive}"
    report("M5", over_total == 0 and worst_frac < 0.001,
           f"corpus (drums -1 dBFS pk, pink -18 dBFS RMS, sweep -6 dBFS), CEILING 0/-1/-6 x DRIVE 0/6/12/18/24: {over_total} samples over; safety limit active on {100 * worst_frac:.4f} % worst ({where})",
           "0 samples; < 0.1 %")


def M6():
    worst, where = -999.0, ""
    for ceil, drive, out in corpus_runs():
        tp = true_peak_db(out) - ceil
        if tp > worst: worst, where = tp, f"CEILING {ceil:g} DRIVE {drive}"
    report("M6", worst <= 0.5, f"true peak over the ceiling, same corpus: worst {worst:+.2f} dB ({where})", "<= +0.5 dB")


def M7():
    x = pink(10.0, seed=2)                                       # make_autogain.py uses seed 1
    worst, where, txt = 0.0, "", []
    for c in (0.0, 0.5, 1.0):
        base = loudness(run(x, drive=0, character=c))
        row = []
        for d in range(0, 19, 3):
            dl = loudness(run(x, drive=d, character=c)) - base
            row.append(f"{dl:+.2f}")
            if abs(dl) > abs(worst): worst, where = dl, f"DRIVE {d} CHARACTER {int(c * 100)}"
        txt.append(f"C{int(c * 100)} [" + " ".join(row) + "]")
    report("M7", abs(worst) <= 1.0, "AUTO on, pink -18 dBFS RMS, loudness vs DRIVE 0 for DRIVE 0..18 step 3: " + "; ".join(txt) + f"; worst {worst:+.2f} LU ({where})", "within +-1 LU")


def M8():
    lat = reported_latency()
    got = {}
    for sr in (44100, 48000, 96000, 192000):
        x = np.zeros(4096, np.float32); x[1000] = 0.25
        got[sr] = int(np.argmax(np.abs(run(x, sr=sr)[:, 0]))) - 1000
    ok = all(v == lat for v in got.values())
    report("M8", ok, f"reported {lat}; impulse delay " + ", ".join(f"{sr / 1000:g} kHz {v}" for sr, v in got.items()), "exact")


def M9():
    x = drums()
    kw = dict(drive=15, character=0.7, tone=2.5, ceiling=-1.5, mix=0.8)
    ref = run(x, block=512, **kw)
    worst, txt = 0.0, []
    for b in (1, 17, 64, 4096, "random"):
        y = run(x, blocks_random=True, **kw) if b == "random" else run(x, block=b, **kw)
        d = float(np.max(np.abs(y - ref)))
        worst = max(worst, d); txt.append(f"{b}: {d:.1e}")
    report("M9", worst <= 1e-6, "drum loop, DRIVE 15 CHAR 70 TONE +2.5 CEIL -1.5 MIX 80, max |diff| vs block 512: " + ", ".join(txt), "<= 1e-6")


def timed(x, reps=3, **kw):
    ts = []
    fi, fo = os.path.join(TMP, "t_in.f32"), os.path.join(TMP, "t_out.f32")
    np.ascontiguousarray(x, dtype=np.float32).tofile(fi)
    cmd = [RAW, fi, fo, "48000"] + [f"{k}={v}" for k, v in kw.items()]
    for _ in range(reps):
        t0 = time.perf_counter(); subprocess.run(cmd, check=True); ts.append(time.perf_counter() - t0)
    return ts, np.fromfile(fo, dtype=np.float32)


def M10():
    sr = 48000
    z = np.zeros((2 * sr, 2), np.float32)
    silent = all(np.all(run(z, drive=d, character=c, tone=t) == 0) for d, c, t in ((6, 0.35, 0), (24, 1.0, 6), (24, 0.0, -6)))
    rng = np.random.default_rng(13)
    noise = rng.standard_normal((20 * sr, 2)).astype(np.float32)
    tiny = np.concatenate([(noise[:10 * sr] * 1e-30), np.zeros((10 * sr, 2))]).astype(np.float32)   # 1e-30, then the tails decay in silence
    t_norm, _ = timed(noise * 0.1, drive=12, tone=3)
    t_tiny, y_tiny = timed(tiny, drive=12, tone=3)
    ratio = float(np.median(t_tiny) / np.median(t_norm))
    big = np.concatenate([noise[:sr] * 10, np.sign(noise[sr:2 * sr]) * 10, np.full((sr, 2), 10.0), np.full((sr, 2), -10.0)]).astype(np.float32)
    finite = True
    for d, c in ((0, 0.0), (24, 1.0), (24, 0.0), (12, 0.35)):
        finite &= bool(np.all(np.isfinite(run(big, drive=d, character=c, tone=-6))))
    finite &= bool(np.all(np.isfinite(y_tiny)))
    ok = silent and ratio <= 1.5 and finite
    report("M10", ok, f"silence -> silence {'yes' if silent else 'NO'}; 1e-30 input time / normal input time {ratio:.2f} (median of 3); +-10 FS: NaN/Inf {'none' if finite else 'FOUND'}",
           "silence exact zero; time ratio <= 1.5; no NaN/Inf")


def M11():
    sr = 48000
    x = sine(100, -6, 1.5)
    def thd_of(**kw):
        y = run(x, **kw)[-sr:, 0].astype(np.float64)
        return thd_db(y, 100, sr)[0], float(db(np.sqrt(np.mean(y ** 2))))
    ok, txt = True, []
    for name, lo, hi in (("drive", 0, 24), ("character", 0, 1), ("tone", -6, 6), ("ceiling", 0, -12), ("mix", 0, 1), ("in", 0, 1)):
        (a, _), (b, _) = thd_of(**{name: lo}), thd_of(**{name: hi})
        d = abs(b - a); ok &= d >= 1.0
        txt.append(f"{name} {a:.1f}->{b:.1f} ({d:.1f})")
    # AUTO is a gain: it cannot change THD. It is judged on the level it moves (see STATUS.md).
    (a, la), (b, lb) = thd_of(auto=0), thd_of(auto=1)
    ok &= abs(lb - la) >= 1.0
    txt.append(f"auto THD {a:.1f}->{b:.1f}, level {la:.2f}->{lb:.2f} dB ({abs(lb - la):.2f})")
    report("M11", ok, "100 Hz -6 dBFS, THD dB at each end of the range (delta): " + ", ".join(txt), ">= 1 dB each (AUTO: level >= 1 dB)")


def M12():
    x = pink(60.0, seed=5)
    fi, fo = os.path.join(TMP, "bench_in.f32"), os.path.join(TMP, "bench_out.f32")
    np.ascontiguousarray(x, dtype=np.float32).tofile(fi)
    ts = []
    for _ in range(3):
        p = subprocess.run([RAW, fi, fo, "48000", "--bench"], check=True, capture_output=True, text=True)
        line = next(v for v in p.stderr.splitlines() if v.startswith("process_seconds "))
        ts.append(float(line.split()[1]))
    rt = 60.0 / float(np.median(ts))
    report("M12", rt >= 80, f"48 kHz stereo, 60 s pink, korraw --bench (Engine::process only): " + ", ".join(f"{t:.3f}" for t in ts) + f" s -> {rt:.0f}x realtime (median)", ">= 80x")


def M13():
    sr = 48000
    x = sine(1000, -6, 1.5)
    rows = {}
    print("M13  harmonic profile, 1 kHz -6 dBFS, DRIVE +12 (dBc):", flush=True)
    print("     CHAR   " + " ".join(f"{'H' + str(k):>7}" for k in range(2, 10)), flush=True)
    for c in (0.0, 0.5, 1.0):
        y = run(x, drive=12, character=c)[-sr:, 0].astype(np.float64)
        h = harmonics(y, 1000, sr, 9)
        rel = db(h[1:] / h[0])
        rows[c] = rel
        print(f"     {int(c * 100):>3} %  " + " ".join(f"{v:7.1f}" for v in rel), flush=True)
    r0, r1 = rows[0.0], rows[1.0]
    h3_dom = int(np.argmax(r0)) == 1                              # index 1 is H3
    odd0 = r0[[1, 3, 5, 7]]                                       # H3 H5 H7 H9
    smooth = bool(np.all(np.diff(odd0) < 0))
    hi0 = float(db(np.sqrt(np.sum(amp(r0[3:]) ** 2)))); hi1 = float(db(np.sqrt(np.sum(amp(r1[3:]) ** 2))))   # H5..H9
    ok = h3_dom and smooth and hi1 > hi0
    report("M13", ok, f"CHAR 0: H3 dominant {'yes' if h3_dom else 'no'}, H3>H5>H7>H9 {'yes' if smooth else 'no'}; H5..H9 energy CHAR 0 {hi0:.1f} dBc, CHAR 100 {hi1:.1f} dBc",
           "report (CHAR 0 H3 dominant, odd decay; CHAR 100 more H5..H9)")


CHECKS = [M1, M2, M3, M4, M5, M6, M7, M8, M9, M10, M11, M12, M13]

if __name__ == "__main__":
    want = set(sys.argv[1:])
    for fn in CHECKS:
        if not want or fn.__name__ in want:
            fn()
    print(f"failures: {fails}")
    sys.exit(fails)
