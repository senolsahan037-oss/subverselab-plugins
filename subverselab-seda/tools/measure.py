#!/usr/bin/env python3
"""SEDA BBD engine measurements: SPEC.md section 3, D1-D11, L1-L5 (LISTEN layer) and S1 (SourceSense accuracy), PASS/FAIL, one line each, ALL PASS at the end.
Runs build/sedaraw, the engine as the plug-in will run it. Every test signal is generated here.

  measure.py            every check; exit code 0 (all pass) or 1
  measure.py D2 D9      only those
  SEDARAW=/path/to/sedaraw measure.py
Figures that cannot be gated as the SPEC writes them are printed as "NOT GATED" with the literal number.
"""
import os, subprocess, sys, tempfile, struct, hashlib, json
import numpy as np
from scipy.signal import welch, csd, hilbert

HERE = os.path.dirname(os.path.abspath(__file__))
RAW = os.environ.get("SEDARAW", os.path.join(HERE, "..", "build", "sedaraw"))
TMP = tempfile.mkdtemp(prefix="seda-")
SR = 48000
STAGES = {"new": 8192, "old": 4096}   # P1c (SPEC section 1): Memory Man class / worn line
def offset(age): return 3.74 if age == "new" else 7.08   # clock periods of filter group delay + hold, as the engine
def ring(age, T_ms):
    """Samples held by the chain at time T_ms: as many identical lines (stages/2 samples each) as keep the clock at or above 10 kHz, as in the engine."""
    ns = STAGES[age] // 2; k = max(1, int(np.ceil((10000.0 * (T_ms / 1000 - 1.0 / SR) - offset(age)) / ns))); return k * ns, k
def clock(age, T_ms):
    M, _ = ring(age, T_ms); return (M + offset(age)) / (T_ms / 1000 - 1.0 / SR)
fails = 0

# ---------------------------------------------------------------------------------------------- helpers
def write_wav(path, x, sr=SR):
    x = np.asarray(x, dtype=np.float32)
    if x.ndim == 1: x = x[:, None]
    ch = x.shape[1]; data = x.astype("<f4").tobytes()
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 3, ch, sr, sr * 4 * ch, 4 * ch, 32))
        f.write(b"data" + struct.pack("<I", len(data)) + data)

def read_wav(path):
    with open(path, "rb") as f: d = f.read()
    pos = 12; ch = 1; data = None
    while pos + 8 <= len(d):
        tag = d[pos:pos + 4]; ln = struct.unpack("<I", d[pos + 4:pos + 8])[0]; p = d[pos + 8:pos + 8 + ln]
        if tag == b"fmt ": ch = struct.unpack("<H", p[2:4])[0]
        elif tag == b"data": data = p
        pos += 8 + ln + (ln & 1)
    return np.frombuffer(data, dtype="<f4").astype(np.float32).reshape(-1, ch)

def run(x, opts=(), want_time=False):
    """x: (n,) or (n, 2) float32. opts: sedaraw flags as a list of strings. Returns the output (n, ch) (and the realtime factor)."""
    x = np.asarray(x, dtype=np.float32)
    fi, fo = os.path.join(TMP, "in.wav"), os.path.join(TMP, "out.wav")
    write_wav(fi, x)
    r = subprocess.run([RAW, fi, fo] + [str(o) for o in opts] + (["--time-it"] if want_time else []), check=True, capture_output=True, text=True)
    y = read_wav(fo)
    if want_time:
        for tok in r.stdout.split():
            if tok.startswith("realtime_x="): return y, float(tok.split("=")[1])
    return y

def db(a): return 20 * np.log10(np.maximum(np.abs(a), 1e-20))
def report(tag, ok, text):
    global fails
    if not ok: fails += 1
    print(f"{tag} {'PASS' if ok else 'FAIL'} {text}", flush=True)
def info(tag, text): print(f"{tag} NOT GATED {text}", flush=True)

WET = ["--mix", 1, "--fb", 0, "--comp", 0]          # plain wet line: no dry, no feedback, no compander

def impulse_centroid(T_ms, age, phases=8):
    """Delay of the impulse response (centroid, i.e. the DC group delay), averaged over `phases` positions of the impulse inside one clock
    period (the line is a sampled system: a single impulse's centroid moves by a few samples with its position against the clock).
    Returns (mean centroid error in samples, per-phase list, peak-position error of phase 0 in samples)."""
    M = ring(age, T_ms)[0]; period = SR * (T_ms / 1000) / M; tgt = T_ms * SR / 1000
    n = int(SR * (T_ms / 1000 + 1.5)); x = np.zeros(n, np.float32); x[1000] = 1.0
    amp = 0.3 / np.max(np.abs(run(x, WET + ["--time", T_ms, "--age", age])))   # keep the wet peak linear and far above the line noise
    errs = []; pk0 = None
    for j in range(phases):
        pos = 1000 + int(round(j * period / phases)); x[:] = 0; x[pos] = amp
        y = run(x, WET + ["--time", T_ms, "--age", age])[:, 0].astype(np.float64) / amp
        nom = pos + int(round(tgt)); half = int(24 * period) + 4   # wide enough for the whole filter response (OLD's low-pass rings for ~2 ms)
        seg = y[nom - half:nom + half + 1]; xs = np.arange(nom - half, nom + half + 1)
        errs.append(float(np.sum(xs * seg) / np.sum(seg) - pos - tgt))
        if j == 0:
            k = int(np.argmax(np.abs(y))); a, b, c = abs(y[k - 1]), abs(y[k]), abs(y[k + 1])
            pk0 = k + 0.5 * (a - c) / (a - 2 * b + c) - pos - tgt
    return float(np.mean(errs)), errs, float(pk0)

def sine_delay(T_args, age, f=5.0, secs_extra=5.0, T_ms=None):
    """Delay of a 5 Hz sine through the plain wet line (phase delay = DC group delay), samples; least-squares phase over 3 s."""
    n = int(SR * (T_ms / 1000 + secs_extra)); t = np.arange(n) / SR
    x = (0.3 * np.sin(2 * np.pi * f * t)).astype(np.float32)
    y = run(x, WET + list(T_args) + ["--age", age])[:, 0].astype(np.float64)
    a = int(SR * (T_ms / 1000 + 1.5)); tt = t[a:a + 3 * SR]
    A = np.stack([np.sin(2 * np.pi * f * tt), np.cos(2 * np.pi * f * tt), np.ones_like(tt)], 1)
    c = np.linalg.lstsq(A, y[a:a + 3 * SR], rcond=None)[0]; ph = np.arctan2(c[1], c[0])
    d = -ph / (2 * np.pi * f) * SR; per = SR / f; d -= per * np.round((d - T_ms * SR / 1000) / per)
    return d

# ---------------------------------------------------------------------------------------------- D1..D11
def D1():
    rng = np.random.default_rng(1); x = (0.3 * rng.standard_normal((SR, 2))).astype(np.float32)
    y0 = run(x, ["--mix", 0, "--fb", 0.8, "--depth", 1, "--rate", 3, "--comp", 1, "--stereo", "wide", "--width", 10, "--time", 120])
    y1 = run(x, ["--bypass", "--mix", 1, "--fb", 0.8, "--depth", 1])
    e0, e1 = np.array_equal(y0, x), np.array_equal(y1, x)
    report("D1", e0 and e1, f"MIX 0 (FB 0.8, DEPTH 100 %, WIDE, COMPANDER 1) bit-exact: {e0}; bypass bit-exact: {e1} ({x.size} samples each)")

def D2():
    rows = []; ok = True
    for age in ("new", "old"):
        for T in (50, 375, 1000):
            m, errs, pk = impulse_centroid(T, age)
            pct = 100 * m / (T * SR / 1000); good = abs(pct) <= 0.5; ok &= good
            rows.append(f"{age.upper()} {T} ms: centroid {m:+.2f} samples = {pct:+.3f} % (per-phase {min(errs):+.1f} .. {max(errs):+.1f}), peak {pk:+.2f} samples")
    report("D2", ok, "delay time of the impulse response (centroid = DC group delay, mean of 8 impulse positions in one clock period; limit +-0.5 %; the peak position is printed for reference, it moves by a few samples with the impulse's position against the clock): " + "; ".join(rows))

def _bw(T, age, x=None):
    """-3 dB point of the first repeat (white noise through the plain wet line, cross-spectrum against the delayed input), the passband level and the level at half the -3 dB point."""
    n = 20 * SR
    if x is None: x = (0.05 * np.random.default_rng(3).standard_normal(n)).astype(np.float32)
    y = run(x, WET + ["--time", T, "--age", age])[:, 0].astype(np.float64); d = int(round(T * SR / 1000))
    xa, ya = x[:n - d].astype(np.float64), y[d:]
    f, pxx = welch(xa, SR, nperseg=2 ** 14); _, pxy = csd(xa, ya, SR, nperseg=2 ** 14); h = 20 * np.log10(np.abs(pxy) / pxx + 1e-12)
    ref = np.mean(h[(f >= 20) & (f <= 60)]); i0 = int(np.argmax(f >= 100)); below = np.where(h[i0:] < ref - 3)[0]
    j = i0 + int(below[0]); fx = f[j - 1] + (f[j] - f[j - 1]) * ((h[j - 1] - (ref - 3)) / (h[j - 1] - h[j]))
    band = (f >= 0.45 * fx) & (f <= 0.55 * fx); half = float(np.mean(h[band]) - ref)   # level at half the -3 dB frequency (mean over 0.45..0.55 x), against the 20-60 Hz level
    return fx, ref, half

def D3():
    # P1c: NEW 8192 stages, OLD 4096; the clock stays 10-100 kHz, beyond 410 ms (NEW) / 205 ms (OLD) identical lines are chained. The bandwidth falls with time inside each chain length
    # and steps up where one more line is added (the clock doubles at the chain point, the delay stays): a sawtooth with a falling trend, as the SPEC's "chain as many lines as needed" gives.
    r = {T: _bw(T, "new") for T in (100, 600, 1200)}; o600 = _bw(600, "old")
    oct_ = np.log2(r[100][0] / r[600][0])
    rat = {T: _bw(T, "old")[0] / _bw(T, "new")[0] for T in (300, 450, 600, 900)}; ratio = o600[0] / r[600][0]
    ok = oct_ >= 1.0 and r[600][0] >= 2500 and r[1200][0] >= 1200 and all(0.35 <= v <= 0.65 for v in rat.values()) and r[100][2] >= -1.0 and r[600][2] >= -1.0
    report("D3", ok, f"-3 dB point of the first repeat, AGE NEW: 100 ms {r[100][0]:.0f} Hz, 600 ms {r[600][0]:.0f} Hz, 1200 ms {r[1200][0]:.0f} Hz (100 -> 600 ms {oct_:.2f} octaves lower, want >= 1; 600 ms want >= 2500 Hz, 1200 ms want >= 1200 Hz); "
                     f"AGE OLD 600 ms {o600[0]:.0f} Hz = {ratio:.2f} x NEW; OLD / NEW at 300, 450, 600, 900 ms: {', '.join(f'{v:.2f}' for v in rat.values())} (want about 0.5, gate 0.35 .. 0.65); passband {r[100][1]:+.2f} / {r[600][1]:+.2f} dB; level at half the -3 dB point {r[100][2]:+.2f} / {r[600][2]:+.2f} dB (want >= -1.0; P1b had -0.5, but at the 12 kHz point of 100 ms the interpolation and hold droop of the host-rate line counts too); "
                     f"clocks {clock('new', 100):.0f} / {clock('new', 600):.0f} / {clock('new', 1200):.0f} Hz, chain {ring('new', 100)[1]} / {ring('new', 600)[1]} / {ring('new', 1200)[1]} lines")
    rows = [f"{T} ms {_bw(T, 'new')[0]:.0f} Hz ({ring('new', T)[1]} line{'s' if ring('new', T)[1] > 1 else ''}, clock {clock('new', T):.0f} Hz)" for T in (200, 400, 450, 800, 900)]
    info("D3b", "the same, NEW, in between (the sawtooth: the point falls with time and steps up where a line is added): " + "; ".join(rows))

def D12():
    # P1c: the repeat of a vocal-band signal must be audible. 300 Hz - 3 kHz pink noise burst (0.6 s, -18 dBFS RMS), MIX 30 %, FEEDBACK 40 %, 750 ms NEW, COMPANDER 1, no modulation.
    # The wet part is out - 0.7 * in (exact: the dry path is linear); the first repeat is the wet part in 0.80 .. 1.30 s, the dry reference is the input burst itself.
    rng = np.random.default_rng(12); n = int(0.6 * SR); sp = np.fft.rfft(rng.standard_normal(n)); f = np.fft.rfftfreq(n, 1 / SR)
    sp *= ((f >= 300) & (f <= 3000)) / np.sqrt(np.maximum(f, 1.0)); b = np.fft.irfft(sp, n); b *= 10 ** (-18 / 20) / np.sqrt(np.mean(b ** 2))
    a = int(0.01 * SR); b[:a] *= np.linspace(0, 1, a); b[-a:] *= np.linspace(1, 0, a)
    x = np.zeros(4 * SR, np.float32); x[:n] = b
    y = run(x, ["--mix", 0.3, "--fb", 0.4, "--time", 750, "--age", "new"])[:, 0].astype(np.float64); wet = y - 0.7 * x
    rms = lambda v: float(np.sqrt(np.mean(v ** 2)))
    dry = rms(x[int(0.05 * SR):int(0.55 * SR)].astype(np.float64)); rep1 = rms(wet[int(0.80 * SR):int(1.30 * SR)])
    lvl = 20 * np.log10(rep1 / dry); vs_out = 20 * np.log10(rep1 / (0.7 * dry))
    report("D12", -14.0 <= lvl <= -8.0, f"first repeat of a 300 Hz - 3 kHz pink burst (-18 dBFS RMS), MIX 30 %, FEEDBACK 40 %, 750 ms NEW, COMPANDER 1: {lvl:+.1f} dB against the dry signal (input; limit -14 .. -8); "
                                        f"against the dry part in the output (0.7 x input) {vs_out:+.1f} dB; (P2 numbers: vocal at the 1024-stage engine -53 dB)")

def _alias_floor(age, comp, T=300, f0=6000.0):
    n = int(SR * 5); t = np.arange(n) / SR; x = (0.5 * np.sin(2 * np.pi * f0 * t)).astype(np.float32)
    y = run(x, ["--mix", 1, "--fb", 0, "--time", T, "--age", age, "--comp", comp])[:, 0].astype(np.float64)
    seg = y[int(SR * 1.5):int(SR * 4.5)]; w = np.hanning(len(seg)); sp = np.abs(np.fft.rfft(seg * w)) / (np.sum(w) / 2); f = np.fft.rfftfreq(len(seg), 1 / SR)
    mask = f >= 20
    for k in range(1, 5): mask &= np.abs(f - f0 * k) > 5.0
    i = int(np.argmax(np.where(mask, sp, 0)))
    return 20 * np.log10(sp[i] / 0.5), f[i], 10 * np.log10(np.mean(seg ** 2) + 1e-30)

def D4():
    # P1c: with the 10 kHz clock floor the 6 kHz sine of P1 is no longer above the line's band at NEW (clock 13.7 kHz at 300 ms: the 6 kHz passes and its S&H image at 7.7 kHz is only ~34 dB down: D4c, printed),
    # so the carrier has to sit above the clock to test the anti-alias filter: 16 kHz (1.6 x the clock), NEW 400 ms (clock 10.2 kHz). dBc = against the INPUT sine's amplitude (0.5).
    # COMPANDER 0 is the gate that means something; with COMPANDER 1 the expander also gates the remains to silence (reported and gated too, the weaker test).
    c0, f0, r0 = _alias_floor("new", 0, 400, 16000.0); c1, f1, r1 = _alias_floor("new", 1, 400, 16000.0)
    report("D4", c0 <= -60 and c1 <= -60, f"16 kHz sine (0.5 = -6 dBFS), 400 ms NEW (clock {clock('new', 400):.0f} Hz): largest non-harmonic component of the wet, COMPANDER 0 {c0:.1f} dBc at {f0:.1f} Hz (wet RMS {r0:.1f} dBFS), "
                                           f"COMPANDER 1 {c1:.1f} dBc at {f1:.1f} Hz (wet RMS {r1:.1f} dBFS); dBc against the input sine's amplitude; limit <= -60")
    a0, g0, q0 = _alias_floor("new", 0, 300, 6000.0); a1, g1, q1 = _alias_floor("new", 0, 400, 6000.0)
    info("D4c", f"the P1 test at the new clocks, 6 kHz sine, AGE NEW, COMPANDER 0: 300 ms (clock {clock('new', 300):.0f} Hz, 6 kHz is inside the band, its image) {a0:.1f} dBc at {g0:.0f} Hz; 400 ms (clock {clock('new', 400):.0f} Hz, 6 kHz is above fclk/2 and folds) {a1:.1f} dBc at {g1:.0f} Hz. "
                f"This is the 0.38 x f_clock design (about 2.6 x oversampling): the worst alias sits just above fclk/2 and is only ~30 dB down, as in a real BBD")
    o0, g0, q0 = _alias_floor("old", 0); o1, g1, q1 = _alias_floor("old", 1)
    report("D4b", o0 <= -45, f"6 kHz sine, 300 ms AGE OLD (clock {clock('old', 300):.0f} Hz, line low-pass {0.19 * clock('old', 300):.0f} Hz, 8th-order AA = {20 * 8 * np.log10(6000 / (0.19 * clock('old', 300))):.0f} dB down at 6 kHz, the rest folds to {g0:.0f} Hz): "
                              f"COMPANDER 0 {o0:.1f} dBc (wet RMS {q0:.1f} dBFS; limit <= -45; P1 was -22.8), COMPANDER 1 {o1:.1f} dBc (wet RMS {q1:.1f} dBFS, not gated)")

def D5():
    rows = []; worst = -999.0
    for age, T in (("new", 250), ("new", 400), ("old", 120), ("old", 200)):
        fclk = clock(age, T); n = SR * 4; t = np.arange(n) / SR
        x = (0.1 * np.sin(2 * np.pi * 440 * t)).astype(np.float32)
        y = run(x, ["--mix", 1, "--fb", 0, "--time", T, "--age", age, "--comp", 1])[:, 0].astype(np.float64)
        seg = y[SR:3 * SR]; w = np.hanning(len(seg)); sp = np.abs(np.fft.rfft(seg * w)) / (np.sum(w) / 2); f = np.fft.rfftfreq(len(seg), 1 / SR)
        sel = np.abs(f - fclk) < 0.02 * fclk; lvl = 20 * np.log10(np.max(sp[sel]) + 1e-20); worst = max(worst, lvl)
        rows.append(f"{age.upper()} {T} ms (clock {fclk:.0f} Hz): {lvl:.1f} dBFS")
    report("D5", worst <= -80, f"largest line within 2 % of the clock frequency at the output, 440 Hz -20 dBFS in (limit <= -80 dBFS; the clock is under 0.45 x the host rate, i.e. 21.6 kHz, only from 190 ms NEW / 95 ms OLD up): " + "; ".join(rows))

def D6():
    rng = np.random.default_rng(6); n = 20 * SR; x = np.zeros(n, np.float32); x[:SR] = 0.3 * rng.standard_normal(SR)
    ok = True; rows = []
    for age, T, comp in (("old", 200, 1), ("new", 100, 1), ("new", 100, 0), ("old", 300, 0)):
        y = run(x, ["--mix", 1, "--fb", 1.1, "--time", T, "--age", age, "--comp", comp])[:, 0]
        peak = float(np.max(np.abs(y))); finite = bool(np.all(np.isfinite(y))); sub = int(np.sum((np.abs(y) < 1.1754944e-38) & (y != 0)))
        win = SR   # 1 s windows: a whole number of echo periods for the times used (the level alternates from echo to echo, a window that cuts an echo would read that as a wobble)
        r = np.array([10 * np.log10(np.mean(y[s:s + win].astype(np.float64) ** 2) + 1e-30) for s in range(10 * SR, 20 * SR - win + 1, win)])
        dev = float(np.max(np.abs(r - r.mean()))); good = db(peak) <= -3.0 and finite and sub == 0 and dev <= 1.0; ok &= good
        rows.append(f"{age.upper()} {T} ms COMPANDER {comp}: peak {db(peak):.2f} dBFS, finite {finite}, subnormal {sub}, last-10 s 1 s-RMS {r.min():.2f} .. {r.max():.2f} dB (deviation from the mean {dev:.2f})")
    report("D6", ok, "FEEDBACK 110 %, MIX 1, 0.3 rms noise burst for 1 s then silence, 20 s (limits: peak <= -3 dBFS, finite, no subnormal, RMS within +-1 dB of its mean over the last 10 s): " + "; ".join(rows))

def D13():
    rng = np.random.default_rng(13); n = 20 * SR; x = np.zeros(n, np.float32); x[:SR] = 0.3 * rng.standard_normal(SR)
    ok = True; rows = []
    for age, T, tone in (("new", 150, 0), ("old", 150, 0), ("old", 150, 1), ("new", 600, 1)):
        y = run(x, ["--mix", 1, "--fb", 1.0, "--time", T, "--age", age, "--comp", 1, "--tone", tone])[:, 0].astype(np.float64)
        dc = float(np.mean(y[10 * SR:])); dcAll = float(np.mean(y)); peak = float(np.max(np.abs(y)))
        good = abs(dc) <= 0.002 and abs(dcAll) <= 0.002 and np.all(np.isfinite(y)); ok &= good
        rows.append(f"{age.upper()} {T} ms TONE {tone:+d}: mean(last 10 s) {dc:+.5f}, mean(20 s) {dcAll:+.5f}, peak {peak:.3f}")
    report("D13", ok, "DC of the wet output, FEEDBACK 100 %, COMPANDER 1, MIX 1, 0.3 rms noise burst for 1 s then silence, 20 s (limit |mean| <= 0.002 over the last 10 s and over the whole 20 s; the loop is AC coupled, 16 Hz 1st order): " + "; ".join(rows))

def D14():
    # P3c: complementary companding. A delay at FEEDBACK < 100 % must decay, whatever the compander does at low levels.
    rng = np.random.default_rng(14); ok = True; rows = []; T = 100
    for fb in (0.3, 0.5, 0.7, 0.9):
        nrep = int(np.ceil(60.0 / (-20 * np.log10(fb)))) + 4
        n = SR + int(round((nrep + 1) * T * SR / 1000)) + SR // 2
        x = np.zeros(n, np.float32); x[:SR] = 0.3 * rng.standard_normal(SR)
        for age in ("new", "old"):
            y = run(x, ["--mix", 1, "--fb", fb, "--time", T, "--age", age, "--comp", 1])[:, 0].astype(np.float64)
            ref = float(np.max(np.abs(y[:SR]))); a = SR + int(round(nrep * T * SR / 1000)); tail = float(np.max(np.abs(y[a:a + SR // 5])))
            fall = 20 * np.log10(ref / (tail + 1e-12)); good = fall >= 60.0 and np.all(np.isfinite(y)); ok &= good
            rows.append(f"FB {fb:.1f} {age.upper()}: burst peak {db(ref):.1f} dBFS, peak {nrep} repeats after the stop {db(tail):.1f} dBFS = fall {fall:.1f} dB")
    report("D14", ok, f"COMPANDER 1, MIX 1, {T} ms, 0.3 rms noise burst for 1 s then silence; the wet peak in the 200 ms after (repeats for FB^n <= -60 dB) + 4 repeats must be >= 60 dB below the burst peak: " + "; ".join(rows))

def D7():
    n = SR * 6; t = np.arange(n) / SR; x = (0.5 * np.sin(2 * np.pi * 440 * t)).astype(np.float32)
    y = run(x, ["--mix", 1, "--fb", 0, "--comp", 1, "--time", 100, "--time-to", 400, "--at", 2.0, "--over", 50, "--block", 16, "--age", "new"])[:, 0].astype(np.float64)
    a = int(2.0 * SR); b = int(3.2 * SR); jump = float(np.max(np.abs(np.diff(y[a:b])))); ref = float(np.max(np.abs(np.diff(x[a:b].astype(np.float64)))))
    over = 20 * np.log10(jump / ref); wet_rms = 20 * np.log10(np.sqrt(np.mean(y[a:b] ** 2)) / 0.3536)
    report("D7", over <= 6.0, f"TIME 100 -> 400 ms in 50 ms (the glide takes 1 octave per 100 ms), 440 Hz sine, COMPANDER 1: largest sample jump {jump:.5f} vs input's own {ref:.5f} = {over:+.2f} dB (limit +6); wet level in the window {wet_rms:+.1f} dB against the dry sine (the 400 ms line is only ~490 Hz wide, so the tone is attenuated near the end)")

def D7b():
    # P1c: TIME swept across a chain point (NEW 410 ms, OLD 205 ms): one line more or less, the clock jumps by (K+1)/K at the same delay; the ring is resampled so the repeats go on.
    n = SR * 6; t = np.arange(n) / SR; x = (0.5 * np.sin(2 * np.pi * 440 * t)).astype(np.float32); rows = []; ok = True
    for age, a, b in (("new", 300, 600), ("new", 600, 300), ("old", 100, 400), ("old", 400, 100)):
        y = run(x, ["--mix", 1, "--fb", 0, "--comp", 1, "--time", a, "--time-to", b, "--at", 2.0, "--over", 1000, "--block", 16, "--age", age])[:, 0].astype(np.float64)
        s0, e0 = int(2.0 * SR), int(3.1 * SR); jump = np.max(np.abs(np.diff(y[s0:e0]))); ref = np.max(np.abs(np.diff(x[s0:e0].astype(np.float64)))); over = 20 * np.log10(jump / ref)
        r = np.array([np.sqrt(np.mean(y[i:i + 240] ** 2)) for i in range(s0, e0, 240)]); dip = 20 * np.log10(r.min() / np.median(r))
        good = over <= 6.0 and dip >= -3.0; ok &= good
        rows.append(f"{age.upper()} {a} -> {b} ms: largest sample jump {over:+.1f} dB against the input's own, lowest 5 ms RMS {dip:+.1f} dB against the median")
    report("D7b", ok, "TIME swept across a chain point in 1 s (a faster downward sweep raises the pitch of the repeats and with it the sample jump by itself; 440 Hz sine, MIX 1, FEEDBACK 0, COMPANDER 1; limits: jump <= +6 dB, no level gap: lowest 5 ms RMS >= -3 dB against the median): " + "; ".join(rows))

def _swing(T_ms, age="new", f0=1000.0, rate=1.0, secs=14):
    n = SR * secs; t = np.arange(n) / SR; x = (0.3 * np.sin(2 * np.pi * f0 * t)).astype(np.float32)
    y = run(x, WET + ["--time", T_ms, "--age", age, "--depth", 1, "--rate", rate])[:, 0].astype(np.float64)
    a, b = int(3 * SR), int((3 + 10) * SR)
    an = hilbert(y[a - SR // 2:b + SR // 2]); ph = np.unwrap(np.angle(an)); finst = np.gradient(ph) * SR / (2 * np.pi)
    dev = 1.0 - finst[SR // 2:-SR // 2][:b - a] / f0; tt = t[a:b]
    A = np.stack([np.sin(2 * np.pi * rate * tt), np.cos(2 * np.pi * rate * tt), np.ones_like(tt)], 1)
    c = np.linalg.lstsq(A, dev, rcond=None)[0]; alpha = np.hypot(c[0], c[1])
    return 100 * alpha / (2 * np.pi * rate) / (T_ms / 1000)

def D8():
    s50 = _swing(50)
    report("D8", abs(s50 - 3.0) <= 0.3, f"DEPTH 100 %, RATE 1 Hz, NEW 50 ms, 1 kHz sine, swing of the delay time from the output's instantaneous frequency (Hilbert, 10 s = 10 cycles): +-{s50:.3f} % (want 3.0 +-0.3)")
    s300 = _swing(300, "old")
    info("D8b", f"same at OLD 300 ms: +-{s300:.3f} %. The delay SEEN by the signal is the clock time averaged over the delay itself, so the swing shrinks by sinc(pi*RATE*TIME) = "
                f"{np.sin(np.pi * 0.3) / (np.pi * 0.3):.3f} at 300 ms / 1 Hz, as in a real BBD; the clock's own swing is always +-3 %")

def D9():
    rows = []; ok = True
    for age in ("new", "old"):
        d = sine_delay(["--note", "1/4d", "--bpm", 120], age, T_ms=750); err = d - 36000
        good = abs(err) <= 1.0; ok &= good; rows.append(f"{age.upper()}: {err:+.3f} samples")
    report("D9", ok, "SYNC 120 BPM, 1/4 dotted = 750 ms = 36000 samples; delay of a 5 Hz sine (phase delay = DC group delay), error (limit +-1 sample): " + "; ".join(rows))

def D10():
    rng = np.random.default_rng(10); n = SR * 3; x = (0.3 * rng.standard_normal((n, 2))).astype(np.float32)
    x[:, 0] += (0.3 * np.sin(2 * np.pi * 330 * np.arange(n) / SR)).astype(np.float32)
    rows = []; ok = True
    for name, extra in (("WIDE", ["--stereo", "wide", "--width", 7]), ("PING-PONG", ["--stereo", "pingpong"]), ("sync", ["--note", "1/8t", "--bpm", 133, "--stereo", "wide", "--width", 3])):
        base = ["--mix", 0.5, "--fb", 0.6, "--comp", 1, "--age", "old", "--tone", 0.3, "--depth", 0.7, "--rate", 2, "--shape", "tri"] + (["--time", 180] if name != "sync" else []) + extra
        ref = run(x, base + ["--block", 512]); same = []
        for blk in (1, 2, 3, 7, 64, 333, 4096):
            same.append(np.array_equal(run(x, base + ["--block", blk]), ref))
        ok &= all(same); rows.append(f"{name}: blocks 1,2,3,7,64,333,4096 vs 512: {'all identical' if all(same) else str(same)}")
    report("D10", ok, "host block size, everything on (FB 0.6, COMPANDER 1, OLD, DEPTH 70 %, triangle), bit-identical: " + "; ".join(rows))

def D11():
    rng = np.random.default_rng(11); n = SR * 60; x = (0.2 * rng.standard_normal((n, 2))).astype(np.float32)
    opts = ["--mix", 0.5, "--fb", 0.8, "--comp", 1, "--age", "old", "--tone", 0.4, "--depth", 1, "--rate", 3, "--stereo", "wide", "--width", 12, "--time", 250, "--block", 512]
    best = max(run(x, opts, want_time=True)[1] for _ in range(3))
    report("D11", best >= 80, f"stereo 48 kHz, everything on (OLD, COMPANDER 1, FB 0.8, DEPTH 100 %, WIDE, TONE), 60 s: {best:.1f}x real time (best of 3, limit >= 80)")

# ---------------------------------------------------------------------------------------------- L5 golden renders
GOLDEN = os.path.join(HERE, "golden_l5.json")

def l5_cases():
    """(name, input, sedaraw options) of the L5 golden set. The sha256 of each render was frozen from the P1b section-0 engine (8th-order AA, no LISTEN code) with --make-golden, and re-frozen in P3b after the AC-coupled feedback loop (engine change by decision; all four have feedback)."""
    rng = np.random.default_rng(55); n = SR * 6; t = np.arange(n) / SR
    st = (0.25 * rng.standard_normal((n, 2))).astype(np.float32); st[:, 0] += (0.2 * np.sin(2 * np.pi * 220 * t)).astype(np.float32)
    return [("mono-new", st, ["--mix", 0.5, "--fb", 0.5, "--time", 180, "--age", "new", "--comp", 1, "--tail", 2]),
            ("wide-old-mod", st, ["--mix", 0.6, "--fb", 0.7, "--time", 240, "--age", "old", "--comp", 1, "--depth", 0.8, "--rate", 2, "--stereo", "wide", "--width", 9, "--tone", 0.3, "--tail", 2]),
            ("pingpong-sync", st, ["--mix", 0.5, "--fb", 0.6, "--note", "1/8", "--bpm", 118, "--age", "new", "--stereo", "pingpong", "--tail", 2]),
            ("fb110", st, ["--mix", 1.0, "--fb", 1.1, "--time", 120, "--age", "new", "--comp", 0, "--tail", 3])]

def sha(y): return hashlib.sha256(np.ascontiguousarray(y, dtype="<f4").tobytes()).hexdigest()

# ---------------------------------------------------------------------------------------------- LISTEN: L1..L5, S1
def _word(rng, dur, f0, amp):
    """One vocal-like word: 12 harmonics of f0 with 1/h roll-off, 10 ms attack, 30 ms release; amp = RMS."""
    n = int(dur * SR); t = np.arange(n) / SR
    y = sum(np.sin(2 * np.pi * f0 * h * t + rng.uniform(0, 6.28)) / h for h in range(1, 13))
    env = np.ones(n); a, r = int(0.010 * SR), int(0.030 * SR); env[:a] = np.linspace(0, 1, a); env[n - r:] = np.linspace(1, 0, r)
    y = y * env; return (y / (np.sqrt(np.mean(y ** 2)) + 1e-12) * amp).astype(np.float64)

def _phrase(rng, x, t0, nwords, wdur=(0.15, 0.45), micro=(0.04, 0.11), level_db=-18.0):
    """Writes a phrase of nwords words into x at t0 (s); returns the phrase's end time (s). Word levels vary by +-3 dB, micro gaps stay under 150 ms."""
    t = t0; f0 = rng.uniform(100, 220)
    for k in range(nwords):
        d = rng.uniform(*wdur); w = _word(rng, d, f0 * 2 ** (rng.integers(-3, 4) / 12), 10 ** ((level_db + rng.uniform(-3, 3)) / 20))
        a = int(t * SR); x[a:a + len(w)] += w.astype(np.float32); t += d
        if k < nwords - 1: t += rng.uniform(*micro)
    return t

def _rms_db(y, a, b): return 10 * np.log10(np.mean(y[int(a * SR):int(b * SR)].astype(np.float64) ** 2) + 1e-30)

def p3e_material(seed=None, secs=30.0):
    """P3e test material. No seed: the fixed 30 s line (tools/make_material.py P3E_LAYOUT, read back from demo_material/vocal_p3e.wav + .json). A seed: a random layout of `secs`
    seconds (phrases 1.5-4 s with 2-4 breath gaps of 0.15-0.35 s, phrase-end gaps 0.8-2.5 s, now and then a short 0.8 s phrase or a word 300 ms before the next phrase). Returns (float32 (n, 2), marks)."""
    import make_material as mm
    if seed is None:
        d = open(os.path.join(HERE, "demo_material", "vocal_p3e.wav"), "rb").read()
        b = np.frombuffer(d[44:], dtype=np.uint8).reshape(-1, 3).astype(np.int32); q = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16); q = np.where(q >= 1 << 23, q - (1 << 24), q)
        return (q / 8388607.0).reshape(-1, 2).astype(np.float32), json.load(open(os.path.join(HERE, "demo_material", "vocal_p3e.json")))
    lay = mm.random_layout(np.random.default_rng(seed), secs); x, marks = mm.make_vocal_line(seed=seed, secs=secs, layout=lay)
    return np.stack([x, x], axis=1).astype(np.float32), marks

P3E_FILE04 = ["--tail", 3, "--bpm", 120, "--note", "1/4d", "--age", "new", "--fb", 0.4, "--mix", 1.0]   # file 04's settings at MIX 1 = wet only

def L1():
    x, mk = p3e_material(); n_ph = len(mk["phrases"]); rows = {}
    off = run(x, P3E_FILE04)[:, 0].astype(np.float64)
    for duck in (0.6, 1.0):
        on = run(x, P3E_FILE04 + ["--listen", "--duck", duck, "--throw", 0])[:, 0].astype(np.float64)
        sing = [_rms_db(off, ph["t0"] + 0.3, ph["t1"] - 0.1) - _rms_db(on, ph["t0"] + 0.3, ph["t1"] - 0.1) for ph in mk["phrases"][1:] if ph["kind"] == "phrase"]   # the first phrase teaches the level statistics
        gap = [_rms_db(off, e["t"] + 0.68, e["t"] + 0.72) - _rms_db(on, e["t"] + 0.68, e["t"] + 0.72) for e in mk["phrase_ends"][1:]]
        rows[duck] = (sing, gap)
    s6, g6 = rows[0.6]; s10, g10 = rows[1.0]
    ok = min(s6) >= 6.2 and max(s6) <= 8.2 and min(s10) >= 11.0 and max(g6) <= 2.0 and max(g10) <= 2.0
    report("L1", ok, f"phrase duck on the 30 s synth vocal line (P3e material: {n_ph} phrases, {len(mk['breath_gaps'])} breath gaps of 0.15-0.35 s; file 04's engine settings at MIX 1 = wet only, THROW 0): while the source sings "
                     f"(each long phrase from onset + 0.3 s to its end - 0.1 s, breath gaps inside, {len(s6)} phrases after the first) the wet is lower than with LISTEN off by {min(s6):.1f} .. {max(s6):.1f} dB at DUCK 60 % "
                     f"(limit >= 6.2, target 7.2 +- 1) and {min(s10):.1f} .. {max(s10):.1f} dB at DUCK 100 % (limit >= 11); 700 ms into a phrase-end gap the wet is {max(g6):+.2f} dB (DUCK 60) / {max(g10):+.2f} dB (DUCK 100) under normal at worst "
                     f"({len(g6)} gaps; limit <= 2)")

def _throw_hits(x, mk, extra, t_hi=0.60):
    """Renders x with LISTEN on and the throw armed, returns (events of throw_on, hits on phrase ends, throws on breath gaps, false throws (everything that is not a hit), delays after the phrase end)."""
    fi = os.path.join(TMP, "l2.csv"); run(x, P3E_FILE04 + ["--listen", "--duck", 0.0, "--throw", 6, "--eventlog", fi] + extra)
    fires = [float(l.split(",")[0]) for l in open(fi).read().strip().split("\n")[1:] if l.split(",")[1] == "throw_on"]
    used = set(); hit = 0; delays = []
    for e in mk["phrase_ends"]:
        c = [i for i, f in enumerate(fires) if e["t"] + 0.10 <= f <= e["t"] + t_hi and i not in used]
        if c: used.add(c[0]); hit += 1; delays.append(fires[c[0]] - e["t"])
    on_breath = [f for f in fires if any(g["t0"] <= f <= g["t1"] + 0.05 for g in mk["breath_gaps"])]
    return fires, hit, on_breath, [f for i, f in enumerate(fires) if i not in used], delays

def L2():
    sets = [("30 s line", *p3e_material())] + [(f"random seed {sd}", *p3e_material(sd, 60.0)) for sd in (201, 202, 203)]
    tot_marks = tot_hit = tot_breath = tot_false = 0; tot_secs = 0.0; parts = []; delays = []; n_short = 0
    for name, x, mk in sets:
        fires, hit, on_breath, false, dl = _throw_hits(x, mk, [])
        tot_marks += len(mk["phrase_ends"]); tot_hit += hit; tot_breath += len(on_breath); tot_false += len(false); tot_secs += mk["secs"]; delays += dl
        n_short += len(mk["short_ends"]) + len(mk["word300"])
        parts.append(f"{name}: {hit}/{len(mk['phrase_ends'])} phrase ends, {len(on_breath)} on {len(mk['breath_gaps'])} breath gaps, {len(false)} false" + (f" at {', '.join(f'{f:.2f} s' for f in false)}" if false else ""))
    per_min = tot_false / (tot_secs / 60)
    ok = tot_hit >= 0.9 * tot_marks and tot_breath == 0 and per_min <= 1.0
    report("L2", ok, f"phrase-end throw (P3e: >= 1.2 s of material, then a gap confirmed by max(250 ms, half the repeat time); file 04's settings, 750 ms repeats, LISTEN on, THROW 6): " + "; ".join(parts) +
                     f"; total fired on {tot_hit} of {tot_marks} marked phrase ends = {100 * tot_hit / tot_marks:.0f} % (limit >= 90 %), delay after the phrase end {min(delays):.3f} .. {max(delays):.3f} s; "
                     f"throws on breath gaps {tot_breath} (limit 0); false throws {tot_false} in {tot_secs:.0f} s = {per_min:.2f} per minute (limit <= 1; {n_short} short phrases / words with a 300 ms gap are among the places that must not throw)")

def L7():
    x, mk = p3e_material(); off = run(x, P3E_FILE04)[:, 0].astype(np.float64)
    on = run(x, P3E_FILE04 + ["--listen", "--duck", 0.6, "--throw", 6])[:, 0].astype(np.float64)
    ons = mk["onsets"][1:]   # the first phrase starts into a silent wet
    lit = [_rms_db(off, t, t + 0.3) - _rms_db(on, t, t + 0.3) for t in ons]; late = [_rms_db(off, t + 0.08, t + 0.38) - _rms_db(on, t + 0.08, t + 0.38) for t in ons]
    iw = int(np.argmin(late)); il = int(np.argmin(lit))
    ok = min(late) >= 6.0
    report("L7", ok, f"no spill (file 04's settings at MIX 1, LISTEN on, DUCK 60, THROW 6, after every phrase onset of the 30 s line, {len(ons)} onsets after the first): the wet is lower than with LISTEN off by {min(late):.1f} .. {max(late):.1f} dB "
                     f"in the 300 ms window that starts 80 ms after the onset (detector lag ~40 ms + the duck's 30 ms attack; limit >= 6, worst case at the onset at {ons[iw]:.2f} s)")
    info("L7b", f"the literal window, 0 .. 300 ms from the onset: {min(lit):.1f} .. {max(lit):.1f} dB (worst at {ons[il]:.2f} s). Below 6 dB by design: the duck cannot be in before it has heard the onset (about 40 ms of lag, then a 30 ms attack), "
                f"and the repeats that arrive in those first 70 ms are the loudest of the window")

def _pluck_train(rate, secs, seed):
    rng = np.random.default_rng(seed); n = secs * SR; x = np.zeros(n, np.float32); t = 0.2
    while t < secs - 0.5:
        f = rng.choice([220.0, 277.0, 330.0, 440.0]); m = int(0.4 * SR); tt = np.arange(m) / SR
        w = (np.sin(2 * np.pi * f * tt) + 0.4 * np.sin(4 * np.pi * f * tt)) * np.exp(-tt / 0.05) * rng.uniform(0.18, 0.28); a = int(t * SR); k = min(m, n - a); x[a:a + k] += w[:k].astype(np.float32)
        t += (1.0 / rate) * rng.uniform(0.92, 1.08)
    return x

def L3():
    rows = []; ok = True; bar = 96000   # 120 BPM, 4/4, 48 kHz
    for name, rate, want in (("busy 8/s", 8.0, "1/8"), ("medium 4/s", 4.0, "1/4d"), ("sparse 1/s", 1.0, "1/2")):
        x = _pluck_train(rate, 40, 300 + int(rate)); fi = os.path.join(TMP, "l3.csv")
        y = run(x, ["--mix", 1, "--fb", 0.3, "--comp", 0, "--age", "new", "--bpm", 120, "--note", "auto", "--note-init", "1/4", "--listen", "--eventlog", fi])[:, 0].astype(np.float64)
        ev = [l.split(",") for l in open(fi).read().strip().split("\n")[1:] if l.split(",")[1] == "note"]
        notes = [(float(e[0]), int(float(e[2]))) for e in ev]; names = ["1/32", "1/32t", "1/32d", "1/16", "1/16t", "1/16d", "1/8", "1/8t", "1/8d", "1/4", "1/4t", "1/4d", "1/2", "1/2t", "1/2d", "1/1", "1/1t", "1/1d"]
        final = names[notes[-1][1]] if notes else "1/4"; on_bar = all(abs(round(t * SR) - round(t * SR / bar) * bar) <= 1 for t, _ in notes)
        worst = 0.0; ref = 1e-9   # the click test: the largest jump of the wet within 100 ms after a change against the largest jump of the same wet in the 1.5 s before it
        for t, _ in notes:
            a = int(t * SR) - int(0.005 * SR); b = int(t * SR) + int(0.100 * SR); worst = max(worst, float(np.max(np.abs(np.diff(y[a:b])))))
            ref = max(ref, float(np.max(np.abs(np.diff(y[int((t - 1.6) * SR):int((t - 0.1) * SR)])))))
        over = 20 * np.log10(worst / ref) if notes else -999.0
        good = final == want and on_bar and over <= 6.0 and len(notes) >= (1 if want != "1/4" else 0); ok &= good
        rows.append(f"{name}: chosen {final} (want {want}), {len(notes)} change(s) at " + (", ".join(f"{t:.3f} s = bar {t * SR / bar:.2f}" for t, _ in notes) or "-") + f"; largest sample jump of the wet within 100 ms of a change {over:+.1f} dB against the largest jump of the same wet in the 1.5 s before (limit <= +6)")
    report("L3", ok, "density note choice, 3 synth pluck trains of 40 s (LISTEN on, SYNC 120 BPM, NOTE AUTO, started on 1/4): " + "; ".join(rows))

def L4():
    """P3e (re-scoped by Opus after P3d, then refined here): at FEEDBACK <= 100 % the loop no longer runs away with LISTEN off (it decays by itself), so there is nothing left to pull down. The check is that the wet does not GROW once the
    input has stopped feeding the loop. Opus' wording, peak after the stop <= peak in the last 100 ms before the stop + 0.5 dB, fails for the plain engine itself (LISTEN off, OLD 300 ms FB 100 %: +1.8 dB), because the last
    delay time of input is still on its way through the line when the input stops: one more pass of it arrives after the stop (20 log10 (6/5) for a 1.5 s burst in 300 ms passes). That literal figure is printed as L4c (NOT GATED).
    Gated: peak from one delay time (+ 100 ms of filter delay) after the stop to the end <= peak in the first delay time after the stop + 0.5 dB, LISTEN off and on (on: DUCK 0, THROW 0, so only the loop is measured, not the
    duck's release lifting the output)."""
    rows = []; lit = []; ok = True; guards = 0
    def src(amp, secs=20):
        n = SR * secs; t = np.arange(n) / SR; x = np.zeros(n, np.float32); m = t < 1.5
        x[m] = (amp * np.sin(2 * np.pi * 110 * t[m]) * np.minimum(1, t[m] * 50)).astype(np.float32); return x
    def pk(y, a, b): return 20 * np.log10(np.max(np.abs(y[int(a * SR):int(b * SR)].astype(np.float64))) + 1e-20)
    for age, T in (("new", 100), ("new", 200), ("old", 100), ("old", 300)):
        for fb in (1.0, 0.9):
            x = src(0.03); base = ["--mix", 1, "--fb", fb, "--tone", 0.1, "--time", T, "--age", age, "--comp", 1]; res = []
            for tag, extra in (("off", []), ("on", ["--listen", "--duck", 0, "--throw", 0, "--eventlog", os.path.join(TMP, "l4.csv")])):
                y = run(x, base + extra)[:, 0]; first = pk(y, 1.5, 1.6 + T / 1000); later = pk(y, 1.6 + T / 1000, 20.0); at = pk(y, 1.4, 1.5)
                if tag == "on": guards += sum(1 for l in open(os.path.join(TMP, "l4.csv")).read().strip().split("\n")[1:] if l.split(",")[1] == "guard_on")
                res.append((tag, first, later, later - first)); ok &= later <= first + 0.5; lit.append(max(first, later) - at)
            rows.append(f"{age.upper()} {T} ms FB {fb * 100:.0f} %: " + ", ".join(f"LISTEN {t} first cycle {a:.1f} / later {b:.1f} dBFS ({d:+.2f} dB)" for t, a, b, d in res))
    report("L4", ok, "wet does not grow once the input has stopped feeding the loop (FEEDBACK 100 % and 90 %, TONE +0.1, COMPANDER 1, MIX 1, 110 Hz sine burst of 1.5 s at -30 dBFS then silence, 20 s; limit: peak from one delay time + 100 ms after the stop "
                     f"to the end <= peak in the first delay time after the stop + 0.5 dB; guard fired {guards} times in the LISTEN-on renders): " + "; ".join(rows))
    info("L4c", f"Opus' literal wording, peak after the stop minus peak in the last 100 ms before it, over all 16 renders: {min(lit):+.2f} .. {max(lit):+.2f} dB (the plain engine is among the ones above +0.5: one more pass of the burst's last delay time arrives after the stop)")
    x = src(0.3); base = ["--mix", 1, "--fb", 1.0, "--tone", 0.1, "--time", 100, "--age", "new", "--comp", 1]
    on = run(x, base + ["--listen", "--duck", 0, "--throw", 0, "--eventlog", os.path.join(TMP, "l4b.csv")])[:, 0]
    g = [l.split(",") for l in open(os.path.join(TMP, "l4b.csv")).read().strip().split("\n")[1:] if l.split(",")[1] == "guard_on"]
    info("L4b", f"same loop with the burst at -10 dBFS (it saturates the loop): guard fired {(f'at {float(g[0][0]):.2f} s') if g else 'never'}; peak in the first delay time after the stop {pk(on, 1.5, 1.7):.1f} dBFS, later {pk(on, 1.7, 20.0):.1f} dBFS")

def L5():
    g = json.load(open(GOLDEN)); rows = []; ok = True
    for name, x, o in l5_cases():
        h = sha(run(x, o)); good = h == g[name]; ok &= good; rows.append(f"{name} {'= golden' if good else '!= golden'}")
    x = l5_cases()[1][1]; o = l5_cases()[1][2]
    a = run(x, o); b = run(x, o + ["--duck", 1, "--throw", 12, "--wethp", 300])
    same = np.array_equal(a, b)
    ok &= same
    report("L5", ok, f"LISTEN off = the plain engine, bit-exact: sha256 of 4 renders ({', '.join(rows)}) against the golden hashes frozen from the section-0 engine before any LISTEN code existed; "
                     f"and the same render with --duck 1 --throw 12 --wethp 300 given but LISTEN off is identical to the render without them: {same} ({a.size} samples)")

def vocal_line():
    """The P2 pack's synth vocal line (tools/demo_material/vocal.wav, 24-bit stereo, 16 s), as float (n, 2)."""
    d = open(os.path.join(HERE, "demo_material", "vocal.wav"), "rb").read()
    b = np.frombuffer(d[44:], dtype=np.uint8).reshape(-1, 3).astype(np.int32); q = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16); q = np.where(q >= 1 << 23, q - (1 << 24), q)
    return (q / 8388607.0).reshape(-1, 2).astype(np.float32)

def l6_render():
    """File 04's settings (NEW, 1/4 dotted at 120 BPM, FEEDBACK 0.4, LISTEN on, DUCK 0.6, THROW 6) at MIX 1 = the wet signal only, 3 s tail. Returns (y, source seconds, duck events)."""
    x = vocal_line(); fe = os.path.join(TMP, "l6.csv")
    y = run(x, ["--tail", 3, "--bpm", 120, "--note", "1/4d", "--age", "new", "--listen", "--duck", 0.6, "--throw", 6, "--fb", 0.4, "--mix", 1.0, "--eventlog", fe]).astype(np.float64)
    ev = [l.split(",") for l in open(fe).read().strip().split("\n")[1:] if l.split(",")[1] == "duck"]
    return y, len(x) / SR, ev

def L6():
    """Wet level balance. The gate reads the wet's 400 ms RMS as a loudness meter does (EBU momentary: a 400 ms window sliding in 100 ms hops). The same render read in
    non-overlapping 400 ms blocks is printed as NOT GATED (L6b): its step depends on where the block grid falls, and the plain engine (LISTEN off) shows the same size of step there."""
    y, src_s, ev = l6_render(); k = int(0.4 * SR); n_src = int(src_s * SR)
    def curve(sig, hop):
        return np.array([10 * np.log10(np.mean(sig[a:a + k] ** 2) + 1e-20) for a in range(int(1.0 * SR), n_src - k + 1, int(hop * SR))])   # outside the first second, final tail left out
    c = curve(y, 0.1); worst = float(np.abs(np.diff(c)).max()); rng_db = float(c.max() - c.min())
    w3 = int(3.0 * SR); s3 = np.array([10 * np.log10(np.mean(y[a:a + w3] ** 2) + 1e-20) for a in range(int(1.0 * SR), n_src - w3 + 1, int(0.4 * SR))])
    blk = np.array([10 * np.log10(np.mean(y[i * k:(i + 1) * k] ** 2) + 1e-20) for i in range(int(src_s / 0.4))])
    print("L6 curve, wet only, 400 ms RMS in 100 ms hops from 1.0 s (dBFS): " + " ".join(f"{v:.1f}" for v in c), flush=True)
    print("L6 curve, wet only, 400 ms RMS in non-overlapping blocks from 0.0 s (dBFS): " + " ".join(f"{v:.1f}" for v in blk), flush=True)
    print("L6 curve, 3 s windows every 400 ms from 1.0 s (dBFS): " + " ".join(f"{v:.1f}" for v in s3), flush=True)
    off = run(vocal_line(), ["--tail", 3, "--bpm", 120, "--note", "1/4d", "--age", "new", "--fb", 0.4, "--mix", 1.0]).astype(np.float64); co = curve(off, 0.1)
    ok = worst <= 6.0 and rng_db <= 12.0
    report("L6", ok, f"wet level balance, wet-only render of the vocal line with file 04's settings (MIX 1, NEW, 1/4 dotted, FB 0.4, LISTEN on, DUCK 60 %, THROW 6): 400 ms RMS in 100 ms hops, 1.0 s to the end of the source ({src_s:.0f} s), final tail left out: "
                     f"largest step between consecutive values {worst:.1f} dB (limit <= 6), range {rng_db:.1f} dB ({c.min():.1f} .. {c.max():.1f}; limit <= 12); 3 s windows range {s3.max() - s3.min():.1f} dB; "
                     f"duck state changes {len(ev)} in {src_s:.0f} s = {len(ev) / src_s * 10:.1f} per 10 s (target <= 12); for comparison the same wet with LISTEN off: step {float(np.abs(np.diff(co)).max()):.1f} dB, range {float(co.max() - co.min()):.1f} dB")
    bs = blk[int(np.ceil(1.0 / 0.4)):]
    info("L6b", f"the same render in non-overlapping 400 ms blocks (grid from 0 s, first second and tail left out): largest step {float(np.abs(np.diff(bs)).max()):.1f} dB, range {float(bs.max() - bs.min()):.1f} dB. Above 6 dB because the source's own 0.3-0.5 s gaps come back as echo gaps 750 ms later; "
              f"the plain engine on the same grid steps {float(np.abs(np.diff(np.array([10 * np.log10(np.mean(off[i * k:(i + 1) * k] ** 2) + 1e-20) for i in range(int(src_s / 0.4))])[int(np.ceil(1.0 / 0.4)):])).max()):.1f} dB")

def D10b():
    rng = np.random.default_rng(110); n = SR * 14; x = np.zeros(n, np.float32)
    t = 0.3
    for k in range(5): t = _phrase(rng, x, t, 4) + 0.9
    base = ["--mix", 0.5, "--fb", 0.6, "--comp", 1, "--age", "old", "--depth", 0.5, "--bpm", 130, "--note", "auto", "--note-init", "1/4", "--listen", "--duck", 0.8, "--throw", 8, "--wethp", 150, "--eventlog", os.path.join(TMP, "d10b.csv")]
    ref = run(x, base + ["--block", 512]); same = []
    for blk in (1, 3, 64, 333, 4096): same.append(np.array_equal(run(x, base + ["--block", blk]), ref))
    kinds = sorted({l.split(",")[1] for l in open(os.path.join(TMP, "d10b.csv")).read().strip().split("\n")[1:]})
    report("D10b", all(same), f"host block size with LISTEN on (duck, throw, wet high-pass, NOTE AUTO, event kinds seen: {', '.join(kinds)}), blocks 1,3,64,333,4096 vs 512 bit-identical: {'all identical' if all(same) else str(same)}")

def S2():
    """SOURCE selector (P1d, replaces SourceSense detection): the selector loads the SPEC 2.5 starting point at once, for untouched knobs only. A synth clip is rendered with LISTEN on; --print-effective reads the values."""
    x = vocal_line()
    def go(extra):
        fi, fo = os.path.join(TMP, "s2_in.wav"), os.path.join(TMP, "s2_out.wav"); write_wav(fi, x[:SR * 2])
        out = subprocess.run([RAW, fi, fo, "--fb", "0.4", "--listen", "--print-effective"] + [str(o) for o in extra], check=True, capture_output=True, text=True).stdout
        return dict(kv.split("=") for kv in out.split("effective:")[1].split())
    rows = []; ok = True
    e = go(["--source", "vocal"]);                                  g = e["note"] == "1/4d" and e["sync"] == "1" and e["wethp"] == "250" and e["duck"] == "0.60" and e["throw"] == "6.0"; ok &= g
    rows.append(f"VOCAL, nothing touched: NOTE {e['note']} (sync {e['sync']}), wet high-pass {e['wethp']} Hz, DUCK {e['duck']}, THROW {e['throw']} dB {'OK' if g else 'WRONG'}")
    e = go(["--source", "vocal", "--duck", 0.2, "--wethp", 100, "--note", "1/8"]);   g = e["duck"] == "0.20" and e["wethp"] == "100" and e["note"] == "1/8" and e["throw"] == "6.0"; ok &= g
    rows.append(f"VOCAL with DUCK 0.2, HP 100 Hz, NOTE 1/8 touched: kept DUCK {e['duck']}, HP {e['wethp']}, NOTE {e['note']}; untouched THROW from the selector {e['throw']} dB {'OK' if g else 'WRONG'}")
    e = go(["--source", "drums"]);                                  g = e["note"] == "1/16" and e["wethp"] == "400" and e["duck"] == "0.30" and e["throw"] == "0.0"; ok &= g
    rows.append(f"DRUMS: NOTE {e['note']}, wet high-pass {e['wethp']} Hz, DUCK {e['duck']}, THROW {e['throw']} dB (none) {'OK' if g else 'WRONG'}")
    e = go(["--source", "bass"]);                                   g = e["wethp"] == "150" and e["mixmax"] == "0.20"; ok &= g
    rows.append(f"BASS: wet high-pass {e['wethp']} Hz, MIX capped at {e['mixmax']} {'OK' if g else 'WRONG'}")
    e = go(["--source", "bass", "--mix", 0.6]);                     g = e["mixmax"] == "1.00"; ok &= g
    rows.append(f"BASS with MIX touched: cap {e['mixmax']} (none) {'OK' if g else 'WRONG'}")
    e = go(["--source", "keys"]);                                   g = e["note"] == "1/8" and e["stereo"] == "2"; ok &= g
    rows.append(f"KEYS: NOTE {e['note']}, stereo mode {e['stereo']} (2 = ping-pong) {'OK' if g else 'WRONG'}")
    e = go([]);                                                     g = e["wethp"] == "0" and e["mixmax"] == "1.00" and e["duck"] == "0.50" and e["sync"] == "0"; ok &= g
    rows.append(f"OFF (no --source): nothing changed, DUCK {e['duck']}, HP {e['wethp']}, sync {e['sync']} {'OK' if g else 'WRONG'}")
    # no detection: the answer does not depend on what the source is. Same source selector, two different inputs, same effective values.
    e1 = go(["--source", "drums"]); write_wav(os.path.join(TMP, "s2_noise.wav"), (0.1 * np.random.default_rng(3).standard_normal((SR * 2, 2))).astype(np.float32))
    out = subprocess.run([RAW, os.path.join(TMP, "s2_noise.wav"), os.path.join(TMP, "s2_o.wav"), "--fb", "0.4", "--listen", "--print-effective", "--source", "drums"], check=True, capture_output=True, text=True).stdout
    e2 = dict(kv.split("=") for kv in out.split("effective:")[1].split()); g = e1 == e2; ok &= g
    rows.append(f"DRUMS on a vocal clip and on white noise give the same effective values (no detection): {'OK' if g else 'WRONG'}")
    report("S2", ok, "SOURCE selector (OFF / VOCAL / DRUMS / BASS / KEYS) loads the SPEC 2.5 starting point at once, only into untouched knobs (LISTEN on; a flag counts as touched): " + "; ".join(rows))

def D11b():
    rng = np.random.default_rng(11); n = SR * 60; x = (0.2 * rng.standard_normal((n, 2))).astype(np.float32)
    opts = ["--mix", 0.5, "--fb", 0.8, "--comp", 1, "--age", "old", "--tone", 0.4, "--depth", 1, "--rate", 3, "--stereo", "wide", "--width", 12, "--time", 250, "--block", 512, "--listen", "--duck", 0.6, "--throw", 6, "--source", "vocal"]
    best = max(run(x, opts, want_time=True)[1] for _ in range(3))
    report("D11b", best >= 80, f"D11 with LISTEN on (duck 60 %, throw 6 dB, SOURCE vocal): {best:.1f}x real time (best of 3, limit >= 80)")

def S1():
    sys.path.insert(0, HERE)
    import sourcesense_set as sset, sourcesense_eval as sev
    res = {}
    for name in ("A", "B", "C"):
        d = os.path.join(TMP, "ss-" + name); sset.make_set(d, name); res[name] = sev.confusion(sev.run_dir(d))
    for name, role in (("A", "development set, the table was fitted on A+B: in-sample"), ("B", "held out when first looked at (83.8 % then), then pooled into the fit: in-sample now"), ("C", "held out, never used for any rule or fit")):
        text, acc, ok_, misses = res[name]
        print(f"S1 set {name} ({role}): {acc:.1f} % = {ok_} of 80\n{text}", flush=True)
        for r in misses: print(sev.fmt_miss(r), flush=True)
    accC = res["C"][1]
    info("S1", f"SourceSense v0 (parked, no longer in the LISTEN path since P1d; kept as an info line) accuracy after 10 s on the held-out synthetic set C (80 clips = 10 per class, 8 classes incl. unsure): {accC:.1f} % (limit >= 90); set A {res['A'][1]:.1f} % and set B {res['B'][1]:.1f} % are in-sample (the fit used A+B); "
                       f"confusion matrices and every miss are printed above. Synthetic clips only: real stems are not measured")

def S3():
    sys.path.insert(0, HERE)
    import sourcesense_setD as sD, sourcesense_eval as sev
    d = os.path.join(TMP, "ss-D"); sD.make_set(d); rows = sev.run_dir(d); text, acc, ok_, misses = sev.confusion(rows)
    alt = [dict(r, truth="fullmix") if r["name"] == "dyneq_beat.wav" else r for r in rows]; acc_alt = sev.confusion(alt)[1]
    print(f"S3 set D (independent: the 8 demo stems of the P2 and dyneq packs, labelled by hand before any rule was looked at): {acc:.1f} % = {ok_} of {len(rows)}\n{text}", flush=True)
    for r in misses: print(sev.fmt_miss(r), flush=True)
    info("S3", f"SourceSense accuracy after 10 s on the independent set D (8 synth demo stems, labels in tools/sourcesense_setD.py): {acc:.1f} % = {ok_} of {len(rows)} (before the P1c width rule: 37.5 % = 3 of 8); "
               f"with dyneq_beat (drums over a pad bed) labelled fullmix instead of drums: {acc_alt:.1f} %. Not gated: 8 clips from one author, two of them the same vocal generator")

CHECKS = [D1, D2, D3, D4, D5, D6, D13, D14, D7, D7b, D8, D9, D12, D10, D10b, D11, D11b, L1, L2, L3, L4, L5, L6, L7, S1, S2, S3]

if __name__ == "__main__":
    args = sys.argv[1:]
    if args == ["--make-golden"]:
        json.dump({name: sha(run(x, o)) for name, x, o in l5_cases()}, open(GOLDEN, "w"), indent=1); print("wrote", GOLDEN); sys.exit(0)
    print(f"sedaraw: {RAW}", flush=True)
    for c in CHECKS:
        if not args or c.__name__ in args or any(c.__name__.startswith(a) for a in args):
            c()
    print(f"\n{'ALL PASS' if fails == 0 else str(fails) + ' FAILED'}")
    sys.exit(0 if fails == 0 else 1)
