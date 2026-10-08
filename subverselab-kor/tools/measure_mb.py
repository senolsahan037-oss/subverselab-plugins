#!/usr/local/bin/python3
"""KOR v2 measurements (SPEC.md "KOR v2", N1-N10). Runs build/korraw_mb (Source/KorMultiband.h).

  measure_mb.py           every check; exit code = number of failures
  measure_mb.py N3 N8     only those
"""
import os, sys, subprocess, time
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
os.environ["KORRAW"] = os.environ.get("KORRAW_MB", os.path.join(HERE, "..", "build", "korraw_mb"))
sys.path.insert(0, HERE)
import measure as m

fails = 0
def report(tag, ok, measured, want):
    global fails
    print(f"{tag:<4} {measured} | want {want} | {'PASS' if ok else 'FAIL'}", flush=True)
    if not ok: fails += 1

SR = 48000
ZERO = dict(low=0, mid=0, high=0)

def response_db(**params):
    """Magnitude response from an impulse (-20 dBFS, so heat 0 stays linear), 20 Hz .. 20 kHz."""
    n = 1 << 16
    x = np.zeros(n, np.float32); x[1000] = 0.1
    y = m.run(x, **params)[:, 0].astype(np.float64)
    f = np.fft.rfftfreq(n, 1 / SR)
    h = np.abs(np.fft.rfft(y)) / 0.1
    band = (f >= 20) & (f <= 20000)
    return 20 * np.log10(np.maximum(h[band], 1e-12))

def N1():
    r = response_db(mix=1, **ZERO)
    report("N1", np.max(np.abs(r)) <= 0.15, f"all heat 0, mix 100: response {r.min():+.3f} .. {r.max():+.3f} dB", "flat +-0.15 dB")

def N2():
    r = response_db(mix=0.5, **ZERO)
    report("N2", np.max(np.abs(r)) <= 0.15, f"all heat 0, mix 50: response {r.min():+.3f} .. {r.max():+.3f} dB", "flat +-0.15 dB")

def N3():
    x = m.corpus()
    worst_sp, worst_tp, where = -99.0, -99.0, ""
    for lo in (0, .5, 1):
        for mi in (0, .5, 1):
            for hi in (0, .5, 1):
                for mx in (1, .5):
                    y = m.run(x, low=lo, mid=mi, high=hi, mix=mx)
                    sp = float(m.db(np.max(np.abs(y)))); tp = m.true_peak_db(y)
                    if tp > worst_tp: worst_tp, where = tp, f"L{lo} M{mi} H{hi} mix{mx}"
                    worst_sp = max(worst_sp, sp)
    ok = worst_sp <= -1.0 + 1e-4 and worst_tp <= -0.9
    report("N3", ok, f"27 heat combos x mix 100/50 on the corpus: sample peak {worst_sp:.2f} dBFS, true peak {worst_tp:.2f} dBTP ({where})", "<= -1 dBFS, <= -0.9 dBTP")

def N4():
    x = m.pink(10.0, seed=5)
    base = m.loudness(m.run(x, **ZERO))
    worst, where, txt = 0.0, "", []
    for b in ("low", "mid", "high"):
        p = dict(ZERO); p[b] = 1.0
        d = m.loudness(m.run(x, **p)) - base
        txt.append(f"{b} {d:+.2f}")
        if abs(d) > abs(worst): worst, where = d, b
    report("N4", abs(worst) <= 1.5, "pink -18 dBFS RMS, one band 0 -> 100, loudness change: " + ", ".join(txt) + " LU", "within +-1.5 LU")

def N5():
    x = m.sine(1000, -12, 2.0)
    a = m.thd_db(m.run(x, **ZERO)[SR // 2:, 0].astype(np.float64), 1000, SR)
    b = m.thd_db(m.run(x, low=1, mid=0, high=0)[SR // 2:, 0].astype(np.float64), 1000, SR)
    a = a[0] if isinstance(a, tuple) else a; b = b[0] if isinstance(b, tuple) else b
    report("N5", abs(b - a) <= 1.0, f"1 kHz -12 dBFS THD: all 0 {a:.1f} dB, LOW 100 {b:.1f} dB (change {b - a:+.1f})", "change <= 1 dB")

def N6():
    lat = m.reported_latency(); got = {}
    for sr in (44100, 48000, 96000, 192000):
        x = np.zeros(sr // 2, np.float32); x[100] = 0.05
        y = m.run(x, sr=sr, **ZERO)[:, 0]
        got[sr] = int(np.argmax(np.abs(y))) - 100
    # LR4 sums are allpass: the impulse is smeared; use the cross-correlation peak against the input
    report("N6", all(abs(v - lat) <= 2 for v in got.values()), f"reported {lat}; impulse peak delay " + ", ".join(f"{k // 1000} k {v}" for k, v in got.items()) + " (allpass smear allowed +-2)", "= reported")

def N7():
    d = m.drums(); x = np.stack([d, d], 1).astype(np.float32)
    p = dict(low=.7, mid=.5, high=.6, mix=.8)
    ref = m.run(x, block=512, **p)
    res = []
    for blk in (1, 17, 64, 4096, None):
        y = m.run(x, block=blk, blocks_random=blk is None, **p)
        res.append((blk or "random", float(np.max(np.abs(y - ref)))))
    worst = max(v for _, v in res)
    report("N7", worst <= 1e-6, "drums, max |diff| vs block 512: " + ", ".join(f"{k}: {v:.1e}" for k, v in res), "<= 1e-6")

def N8():
    x = m.pink(60.0, seed=6).astype(np.float32)
    fi, fo = os.path.join(m.TMP, "b_in.f32"), os.path.join(m.TMP, "b_out.f32"); x.tofile(fi)
    ts = []
    for _ in range(3):
        r = subprocess.run([m.RAW, fi, fo, str(SR), "--bench"], capture_output=True, text=True, check=True)
        ts.append(float(r.stderr.strip().split()[-1]))
    t = sorted(ts)[1]
    report("N8", 60.0 / t >= 20, f"60 s pink 48 k stereo, defaults, process only: {', '.join(f'{v:.3f}' for v in ts)} s -> {60.0 / t:.0f}x", ">= 20x")

def N9():
    f0 = 10000.0
    y = m.run(m.sine(f0, -6, 2.5), low=0, mid=0, high=1)[-2 * SR:, 0].astype(np.float64)
    f, s = m.spectrum_db(y, SR)
    ref = float(np.max(s[np.abs(f - f0) < 20]))
    keep = f > 20
    for k in range(1, int(SR / 2 // f0) + 1): keep &= np.abs(f - k * f0) > 20
    i = int(np.argmax(np.where(keep, s, -1e9)))
    w = float(s[i] - ref)
    report("N9", w <= -60, f"10 kHz -6 dBFS, HIGH 100: worst non-harmonic {w:.1f} dBc at {f[i]:.0f} Hz", "<= -60 dBc")

def N10():
    z = m.run(np.zeros(SR, np.float32), low=1, mid=1, high=1)
    silent = bool(np.all(z == 0))
    tiny = np.full(SR * 5, 1e-30, np.float32); norm = m.pink(5.0, seed=8)[:, 0].astype(np.float32)
    def tm(x):
        t0 = time.perf_counter(); m.run(x, low=1, mid=1, high=1); return time.perf_counter() - t0
    ratio = tm(tiny) / tm(norm)
    big = m.run((10 * np.sin(np.arange(SR) * 0.05)).astype(np.float32), low=1, mid=1, high=1)
    bad = bool(np.any(~np.isfinite(big)))
    ok = silent and ratio <= 1.5 and not bad
    report("N10", ok, f"silence -> silence {'yes' if silent else 'no'}; 1e-30 time ratio {ratio:.2f}; +-10 FS NaN/Inf {'yes' if bad else 'none'}", "exact silence, <= 1.5, none")

def main():
    which = [a for a in sys.argv[1:] if a.startswith("N")] or [f"N{i}" for i in range(1, 11)]
    for w in which: globals()[w]()
    print(f"failures: {fails}"); sys.exit(fails)

if __name__ == "__main__":
    main()
