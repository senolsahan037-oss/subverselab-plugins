#!/usr/bin/env python3
"""Dynamic EQ (collision cleaner) engine measurements: SPEC.md section 4, items 1-7 and 9 (PASS/FAIL), item 8 (numbers only), item 14 (BAKE) as A15,
SPEC 7.4's SELF acceptance as A13a-A13e (replaces item 13), and the free dynamic bands (SPEC 1b-A): A11, A11b, A12, A12b and the
CPU line A9b (six dynamic bands + the COLLIDE bank). Runs build/dyneqraw, the engine as the plug-in will run it.
Every test signal is generated here.

  measure.py            every check; exit code = number of failures
  measure.py A3 A13     only those
  DYNEQRAW=/path/to/dyneqraw measure.py
"""
import os, subprocess, sys, tempfile, struct
import numpy as np
from scipy.signal import lfilter

HERE = os.path.dirname(os.path.abspath(__file__))
RAW = os.environ.get("DYNEQRAW", os.path.join(HERE, "..", "build", "dyneqraw"))
P1K_RAW = os.environ.get("DYNEQRAW_P1K", os.path.join(HERE, "baseline", "dyneqraw_p1k"))   # the P1k engine (one stage per 4 ticks), kept as tools/baseline/; T21b compares the memory of every TARGET run with it
P1L_RAW = os.environ.get("DYNEQRAW_P1L", os.path.join(HERE, "baseline", "dyneqraw_p1l"))   # the P1l engine (SELF / TARGET detect on the band-processed signal), kept for T30 / T31's comparison
NEW_RAW = RAW   # learned_at() swaps RAW for the P1h engine for a moment; T21b must not compare that engine with P1k
T21B_SKIPPED = []   # runs whose memory file the P1k engine cannot read (newer layout): not comparable, listed by T21b
T21B_ROWS = []   # (what, same, detail) for every TARGET run that was also run through the P1k engine
DEMO = os.path.join(HERE, "demo_material")
TMP = tempfile.mkdtemp(prefix="dyneq-")
SR = 48000
BLOCK = 32
fails = 0
LAST_BANDS = None
LAST_TARGET = None
LAST_LEARN = None
LAST_STDOUT = ""
LAST_BANDLOG = None

# ---------------------------------------------------------------------------------------------- helpers
def erb_rate(f): return 21.4 * np.log10(1 + 0.00437 * np.asarray(f, dtype=float))
def erb_hz(e): return (10 ** (np.asarray(e, dtype=float) / 21.4) - 1) / 0.00437
E = np.linspace(erb_rate(40), erb_rate(16000), 32); STEP = E[1] - E[0]
FC = erb_hz(E); FLO = erb_hz(E - STEP / 2); FHI = erb_hz(E + STEP / 2)

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
    pos = 12; fmt = ch = sr = bits = 0; data = None
    while pos + 8 <= len(d):
        tag = d[pos:pos + 4]; ln = struct.unpack("<I", d[pos + 4:pos + 8])[0]; p = d[pos + 8:pos + 8 + ln]
        if tag == b"fmt ":
            fmt, ch, sr, _, _, bits = struct.unpack("<HHIIHH", p[:16])
            if fmt == 0xFFFE: fmt = struct.unpack("<H", p[24:26])[0]
        elif tag == b"data": data = p
        pos += 8 + ln + (ln & 1)
    if fmt == 3 and bits == 32: x = np.frombuffer(data, dtype="<f4").astype(np.float32)
    elif fmt == 1 and bits == 16: x = np.frombuffer(data, dtype="<i2").astype(np.float32) / 32768
    elif fmt == 1 and bits == 24:
        b = np.frombuffer(data, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        x = (((b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)) << 8) >> 8).astype(np.float32) / 8388608
    elif fmt == 1 and bits == 32: x = np.frombuffer(data, dtype="<i4").astype(np.float32) / 2147483648
    else: raise RuntimeError(f"{path}: unsupported WAV format {fmt}/{bits}")
    return x.reshape(-1, ch), sr

def t21b_compare(cmd, fm, fk, main, sr, tail_s=1.5):
    """P1l T21b: the same TARGET run through this engine and through the P1k engine (tools/baseline/dyneqraw_p1k), both on the input plus tail_s of silence, and the memory they hold at the end compared
    byte for byte. The tail is there because the new job finishes a record EARLIER than P1k's one-stage-per-4-ticks job (30 ms against about 55 ms), so a run that ends inside that window holds one record more;
    with a tail both have finished every record. --cutlog / --learnlog / --mem-at (a snapshot at a time) are left out of the comparison."""
    pad = np.zeros((int(tail_s * sr), 2), np.float32); mm, kk = os.path.join(TMP, "t21b_main.wav"), os.path.join(TMP, "t21b_key.wav")
    m_in, _ = read_wav(fm); write_wav(mm, np.concatenate([m_in, pad]), sr)
    if fk: k_in, _ = read_wav(fk); write_wav(kk, np.concatenate([k_in, pad]), sr)
    base = []; skip = 0
    for a in cmd[1:]:
        if skip: skip -= 1; continue
        if a in ("--cutlog", "--learnlog", "--bandlog"): skip = 1; continue
        if a == "--mem-at": skip = 2; continue
        if a == "--time": continue
        base.append(a)
    base[0] = mm; base[1] = kk if fk else "-"; base[2] = os.path.join(TMP, "t21b_out.wav")
    if "--mem-out" in base: base[base.index("--mem-out") + 1] = os.path.join(TMP, "t21b_x.mem")
    else: base += ["--mem-out", os.path.join(TMP, "t21b_x.mem")]
    mem = os.path.join(TMP, "t21b_x.mem"); res = []
    for exe, tag in ((RAW, "new"), (P1K_RAW, "p1k")):
        r = subprocess.run([exe] + base, capture_output=True, text=True)
        if r.returncode != 0 and tag == "p1k" and "is not a memory" in (r.stderr or ""):
            # 2026-10-25: a run that loads a memory file (--mem-in, T12 / T13 ...) written by THIS engine: the P1k engine refuses the newer memory layout
            # ("is not a memory", exit 2) and the suite stopped at T12. Such a run cannot be compared with P1k; it is counted and listed by T21b instead.
            T21B_SKIPPED.append(" ".join(a for a in cmd[4:6])); return
        if r.returncode != 0: raise subprocess.CalledProcessError(r.returncode, [exe] + base, r.stdout, r.stderr)
        os.replace(mem, os.path.join(TMP, f"t21b_{tag}.mem")); res.append(open(os.path.join(TMP, f"t21b_{tag}.mem"), "rb").read())
    what = " ".join(f"{a} {cmd[k + 5]}" for k, a in enumerate(cmd[4:], 0) if a in ("--target", "--block", "--atarget", "--mem-in", "--listen-until")) + f" [{len(main) / sr:.1f} s]"
    pre = fk is not None or any(a.startswith(("--band", "--aband")) for a in cmd) or ("--mode" in cmd and cmd[cmd.index("--mode") + 1] == "self")   # P1m: something cuts before TARGET reads (a key = the COLLIDE bank, a free band, the SELF hunters)
    T21B_ROWS.append((what, res[0] == res[1], f"{len(res[0])} bytes", pre))
    if res[0] != res[1] and os.environ.get("T21B_KEEP"):
        import shutil, json
        n = len(T21B_ROWS); shutil.copy(mm, f"{os.environ['T21B_KEEP']}/{n}_main.wav"); json.dump(base, open(f"{os.environ['T21B_KEEP']}/{n}_cmd.json", "w")); [shutil.copy(os.path.join(TMP, f"t21b_{t}.mem"), f"{os.environ['T21B_KEEP']}/{n}_{t}.mem") for t in ("new", "p1k")]

def run(main, key=None, mode="collide", depth=6.0, margin=3.0, low=120.0, high=8000.0, speed="medium", mix=1.0,
        delta=0, rawbells=False, block=BLOCK, want_log=True, timing=False, sr=SR, bands=None, tiling=False, after=None, target=None, atarget=None, learn=False, extra=None, bandlog=False):
    """main: (n,) or (n,2) float32; key: same or None. bands: {1..6: "type=bell,f=1000,g=6,q=1,..."} (dyneqraw --bandN). Returns
    (out (n,2), cuts (blocks, 32) in dB or None, realtime factor or None). The six free-band gains of the log (dB per block) are left in LAST_BANDS."""
    global LAST_BANDS, LAST_TARGET, LAST_LEARN, LAST_STDOUT, LAST_BANDLOG
    main = np.asarray(main, dtype=np.float32)
    if main.ndim == 1: main = np.stack([main, main], axis=1)
    fm, fk, fo, fl = (os.path.join(TMP, n) for n in ("main.wav", "key.wav", "out.wav", "cuts.csv"))
    write_wav(fm, main, sr)
    if key is not None:
        key = np.asarray(key, dtype=np.float32)
        if key.ndim == 1: key = np.stack([key, key], axis=1)
        write_wav(fk, key, sr)
    cmd = [RAW, fm, fk if key is not None else "-", fo, "--mode", mode, "--depth", str(depth), "--margin", str(margin),
           "--low", str(low), "--high", str(high), "--speed", speed, "--mix", str(mix), "--delta", str(delta), "--block", str(block)]
    if rawbells: cmd.append("--rawbells")
    if tiling: cmd.append("--tiling")
    if after is not None:   # (seconds, {band: spec or "off"}): the named bands change at that time
        cmd += ["--after", str(after[0])]
        for k, spec in sorted(after[1].items()): cmd += [f"--aband{k}", spec]
    for k, spec in sorted((bands or {}).items()): cmd += [f"--band{k}", spec]
    if target is not None: cmd += ["--target", target]
    if atarget is not None: cmd += ["--atarget", atarget] + ([] if after is not None else ["--after", "0"])
    fll = os.path.join(TMP, "learn.csv")
    if learn: cmd += ["--learnlog", fll]
    if want_log: cmd += ["--cutlog", fl]
    fbl = os.path.join(TMP, "bands.csv")
    if bandlog: cmd += ["--bandlog", fbl]
    if timing: cmd.append("--time")
    if extra: cmd += list(extra)
    r = subprocess.run(cmd, check=True, capture_output=True, text=True); LAST_STDOUT = r.stdout
    if target is not None and os.path.exists(P1K_RAW) and RAW == NEW_RAW:   # T21b
        t21b_compare(cmd, fm, fk if key is not None else None, main, sr)
    out, _ = read_wav(fo)
    if os.environ.get("DYNEQ_HASHLOG"):   # P1f: sha256 of every render, in call order, so two builds can be compared bit for bit
        import hashlib
        with open(os.environ["DYNEQ_HASHLOG"], "a") as hf: hf.write(hashlib.sha256(open(fo, "rb").read()).hexdigest() + "\n")
    cuts = None; LAST_BANDS = None; LAST_TARGET = None; LAST_LEARN = None; LAST_BANDLOG = None
    if bandlog: LAST_BANDLOG = np.genfromtxt(fbl, delimiter=",", names=True)
    if learn: LAST_LEARN = np.genfromtxt(fll, delimiter=",", names=True)
    if want_log:
        data = np.loadtxt(fl, delimiter=",", skiprows=1, ndmin=2)[:, 1:]
        cuts, LAST_BANDS, LAST_TARGET = data[:, :-7], data[:, -7:-1], data[:, -1]   # SELF: 32 cuts + 4 x (hz, gain dB, Q); then the six free bands, then target_db
    rt = None
    if timing:
        for tok in r.stdout.split():
            if tok.startswith("realtime_x="): rt = float(tok.split("=")[1])
    return out, cuts, rt

def db(a): return 20 * np.log10(np.maximum(np.abs(a), 1e-12))
def amp(d): return 10 ** (d / 20)
def sine(f, level_db, secs, sr=SR): return (amp(level_db) * np.sin(2 * np.pi * f * np.arange(int(secs * sr)) / sr)).astype(np.float32)
def rms_db(x): return float(10 * np.log10(np.mean(np.asarray(x, dtype=np.float64) ** 2) + 1e-30))

def pink(secs, rms_level_db, seed, sr=SR):
    n = int(secs * sr); rng = np.random.default_rng(seed)
    spec = np.fft.rfft(rng.standard_normal(n)); f = np.fft.rfftfreq(n, 1 / sr); f[0] = f[1]
    x = np.fft.irfft(spec / np.sqrt(f), n); x /= np.sqrt(np.mean(x ** 2)); return (x * amp(rms_level_db)).astype(np.float32)

def tone_level(y, f, sr=SR):
    """Level (dB) of a sine at f in y, Hann window, coherent gain removed."""
    w = np.hanning(len(y)); spec = np.abs(np.fft.rfft(y * w)) / (np.sum(w) / 2)
    b = int(round(f * len(y) / sr)); return float(db(np.sqrt(np.sum(spec[b - 2:b + 3] ** 2) / 1.5)))

def rbj_peak(f0, q, gain_db, sr=SR):
    A = 10 ** (gain_db / 40); w0 = 2 * np.pi * f0 / sr; al = np.sin(w0) / (2 * q)
    b = np.array([1 + al * A, -2 * np.cos(w0), 1 - al * A]); a = np.array([1 + al / A, -2 * np.cos(w0), 1 - al / A])
    return b / a[0], a / a[0]

def band_energies(x, sr=SR, nfft=8192):
    """Mean power per ERB band (Welch, Hann 8192, hop 4096), shape (32,)."""
    x = np.asarray(x, dtype=np.float64); w = np.hanning(nfft); f = np.fft.rfftfreq(nfft, 1 / sr); acc = np.zeros(len(f)); cnt = 0
    for s in range(0, len(x) - nfft + 1, nfft // 2):
        acc += np.abs(np.fft.rfft(x[s:s + nfft] * w)) ** 2; cnt += 1
    p = acc / max(cnt, 1); e = np.zeros(32)
    for b in range(32): e[b] = np.sum(p[(f >= FLO[b]) & (f < FHI[b])])
    return e

def lufs(x, sr=SR):
    """Integrated loudness, ITU-R BS.1770-4 (K-weighting, 400 ms blocks, absolute and relative gate). x: (n,2) or (n,)."""
    x = np.asarray(x, dtype=np.float64)
    if x.ndim == 1: x = x[:, None]
    b1 = [1.53512485958697, -2.69169618940638, 1.19839281085285]; a1 = [1, -1.69065929318241, 0.73248077421585]
    b2 = [1.0, -2.0, 1.0]; a2 = [1, -1.99004745483398, 0.99007225036621]
    y = lfilter(b2, a2, lfilter(b1, a1, x, axis=0), axis=0)
    blk = int(0.4 * sr); hop = blk // 4
    z = np.array([np.sum(np.mean(y[s:s + blk] ** 2, axis=0)) for s in range(0, len(y) - blk + 1, hop)])
    l = -0.691 + 10 * np.log10(np.maximum(z, 1e-30))
    keep = l > -70
    if not keep.any(): return -np.inf
    rel = -0.691 + 10 * np.log10(np.mean(z[keep])) - 10
    keep &= l > rel
    return float(-0.691 + 10 * np.log10(np.mean(z[keep])))

def report(tag, ok, text):
    global fails
    print(f"{tag} {'PASS' if ok else 'FAIL'} {text}")
    if not ok: fails += 1

def info(tag, text): print(f"{tag} INFO {text}")

def nearest_band(f): return int(np.argmin(np.abs(erb_rate(FC) - erb_rate(f))))

def hunters(cuts):
    """SELF cut log -> (freq (blocks, 4) Hz, cut (blocks, 4) dB positive = cut, Q (blocks, 4))."""
    h = cuts[:, 32:].reshape(len(cuts), 4, 3); return h[:, :, 0], -h[:, :, 1], h[:, :, 2]

def resonant_pink(secs, f_of_t, q=8.0, gain_db=12.0, level_db=-20.0, seed=41, sr=SR):
    """Pink noise through an RBJ peak whose centre follows f_of_t(t) (coefficients per 32 samples, state carried)."""
    x = pink(secs, level_db, seed, sr); y = np.zeros_like(x, dtype=np.float64); zi = np.zeros(2)
    for s in range(0, len(x), BLOCK):
        b, a = rbj_peak(float(f_of_t(s / sr)), q, gain_db, sr)
        y[s:s + BLOCK], zi = lfilter(b, a, x[s:s + BLOCK].astype(np.float64), zi=zi)
    return y.astype(np.float32)

def glide_f(t):
    """2.0 kHz until 1 s, then 2.0 -> 3.0 kHz over 4 s (constant octaves per second), 3.0 kHz after 5 s."""
    t = np.clip(np.asarray(t, dtype=float), 1.0, 5.0); return 2000.0 * 1.5 ** ((t - 1.0) / 4.0)

def narrow_ratio_db(x, y, f0, half_hz=30.0, sr=SR, nfft=8192):
    """Attenuation (dB) of y against x within f0 +- half_hz (Welch, Hann 8192, hop 4096)."""
    w = np.hanning(nfft); f = np.fft.rfftfreq(nfft, 1 / sr); sel = (f >= f0 - half_hz) & (f <= f0 + half_hz)
    def p(z):
        z = np.asarray(z, dtype=np.float64); acc = 0.0
        for s in range(0, len(z) - nfft + 1, nfft // 2): acc += np.sum(np.abs(np.fft.rfft(z[s:s + nfft] * w)[sel]) ** 2)
        return acc
    return float(10 * np.log10(p(x) / p(y)))

def dominant_slot(freq, cut):
    """Per block: index of the slot with the deepest cut (-1 when no slot cuts)."""
    idx = np.argmax(cut, axis=1); idx[cut.max(axis=1) <= 0] = -1; return idx

# ---------------------------------------------------------------------------------------------- material
def beat(secs=12.0, seed=1, sr=SR):
    """Synth demo beat: kick, snare, hats, pad. 120 BPM. About -14 dBFS RMS, stereo."""
    rng = np.random.default_rng(seed); n = int(secs * sr); t = np.arange(n) / sr; out = np.zeros((n, 2))
    spb = 60 / 120
    def place(sig, at, pan=0.5):
        i = int(at * sr); m = min(len(sig), n - i)
        if m > 0: out[i:i + m, 0] += sig[:m] * (1 - pan) * 2; out[i:i + m, 1] += sig[:m] * pan * 2
    tk = np.arange(int(0.35 * sr)) / sr
    kick = np.sin(2 * np.pi * (50 * tk + (150 - 50) * 0.03 * (1 - np.exp(-tk / 0.03)))) * np.exp(-tk / 0.12)
    ts = np.arange(int(0.2 * sr)) / sr
    snare = (0.6 * rng.standard_normal(len(ts)) * np.exp(-ts / 0.05) + 0.5 * np.sin(2 * np.pi * 190 * ts) * np.exp(-ts / 0.08))
    th = np.arange(int(0.05 * sr)) / sr
    hat = lfilter(*rbj_peak(9000, 0.7, 12), rng.standard_normal(len(th))) * np.exp(-th / 0.012) * 0.25
    hat -= lfilter([1], [1, -0.98], hat) * 0.02
    for beat_i in range(int(secs / spb)):
        place(kick, beat_i * spb)
        if beat_i % 2 == 1: place(snare, beat_i * spb, 0.5)
        for h in range(2): place(hat, beat_i * spb + h * spb / 2, 0.35 if h == 0 else 0.65)
    chords = [(110, 164.8, 220, 261.6), (98, 146.8, 196, 246.9), (87.3, 130.8, 174.6, 220), (116.5, 174.6, 233.1, 293.7)]
    pad = np.zeros((n, 2))
    for ci, ch in enumerate(chords):
        for rep in range(int(np.ceil(secs / (len(chords) * 4 * spb)))):
            s = int((ci + rep * len(chords)) * 4 * spb * sr); e = min(n, s + int(4 * spb * sr))
            if s >= n: continue
            tt = np.arange(e - s) / sr; env = np.minimum(1, tt / 0.05) * np.minimum(1, (e - s) / sr - tt + 0.001) ** 0.3
            for f in ch:
                for det, side in ((0.997, 0), (1.003, 1)):
                    ph = (tt * f * det) % 1.0; pad[s:e, side] += (2 * ph - 1) * env * 0.08
    pad = lfilter(*rbj_peak(1500, 0.7, -14), pad, axis=0)
    out += pad; out /= np.sqrt(np.mean(out ** 2)) / amp(-14)
    return np.clip(out, -0.99, 0.99).astype(np.float32)

def vocal(secs=12.0, seed=2, sr=SR):
    """Vocal-like key: formant-filtered saw with vibrato, phrases with gaps. About -18 dBFS RMS when on, stereo."""
    rng = np.random.default_rng(seed); n = int(secs * sr); t = np.arange(n) / sr
    notes = [220, 246.9, 261.6, 196, 220, 293.7, 261.6, 246.9]; f0 = np.zeros(n); gate = np.zeros(n)
    pos = 0.3; i = 0
    while pos < secs:
        dur = 0.5 + 0.3 * (i % 3); s, e = int(pos * sr), min(n, int((pos + dur) * sr))
        f0[s:e] = notes[i % len(notes)]; gate[s:e] = 1; pos += dur + (0.25 if i % 4 != 3 else 0.9); i += 1
    f0 = np.where(f0 > 0, f0, 220) * (1 + 0.017 * np.sin(2 * np.pi * 5.5 * t))
    ph = np.cumsum(f0 / sr) % 1.0; saw = 2 * ph - 1
    v = sum(lfilter(*rbj_peak(f, 6, g), saw) for f, g in ((650, 14), (1100, 12), (2500, 8), (3300, 6)))
    v = lfilter(*rbj_peak(300, 0.6, -6), v)
    a = 1 - np.exp(-1 / (0.03 * sr)); r = 1 - np.exp(-1 / (0.1 * sr)); env = np.zeros(n); st = 0.0
    for k in range(n):
        st += (gate[k] - st) * (a if gate[k] > st else r); env[k] = st
    v *= env; v += 0.002 * rng.standard_normal(n) * env
    on = env > 0.5; v /= np.sqrt(np.mean(v[on] ** 2)) / amp(-18)
    return np.stack([v, v], axis=1).astype(np.float32)

def demo_pair():
    """tools/demo_material/beat.wav + vocal.wav if present (resampled not supported: must be 48 kHz), else the synth pair."""
    pb, pv = os.path.join(DEMO, "beat.wav"), os.path.join(DEMO, "vocal.wav")
    if os.path.exists(pb) and os.path.exists(pv):
        b, sb = read_wav(pb); v, sv = read_wav(pv)
        if sb == SR and sv == SR:
            n = min(len(b), len(v), 30 * SR); return b[:n], v[:n], "demo_material"
    return beat(), vocal(), "synthetic"

# ---------------------------------------------------------------------------------------------- checks
def A1():
    x = beat(10.0); y, cuts, _ = run(x, np.zeros_like(x))
    d = float(np.max(np.abs(y - x))); mc = float(np.max(cuts))
    report("A1", d == 0.0, f"silent key over 10 s of music: max |out - in| = {d:g} (want 0), largest logged cut {mc:.3f} dB")

def A2():
    main = sine(4000, -12, 4.0); key = sine(1000, -12, 4.0)
    y, cuts, _ = run(main, key, depth=12, margin=3)
    seg = slice(2 * SR, 4 * SR); d = tone_level(y[seg, 0], 4000) - tone_level(main[seg], 4000)
    report("A2", abs(d) <= 0.05, f"no overlap (key 1 kHz, main 4 kHz, both -12 dBFS): main out - in = {d:+.4f} dB (want within 0.05); largest cut {np.max(cuts):.3f} dB")

def collide_1k(depth):
    main = sine(1000, -12, 4.0); key = sine(1000, -18, 4.0)
    y, cuts, _ = run(main, key, depth=depth, margin=3)
    seg = slice(2 * SR, 4 * SR); cut = tone_level(main[seg], 1000) - tone_level(y[seg, 0], 1000)
    steady = cuts[-int(1.0 * SR / BLOCK):].mean(axis=0)
    far = np.abs(erb_rate(FC) - erb_rate(1000)) >= 2.0
    return cut, steady, float(np.max(steady[far]))

def A3():
    cut, steady, far = collide_1k(12)
    near = ", ".join(f"{FC[b]:.0f} Hz {steady[b]:.2f}" for b in range(8, 17))
    ok = abs(cut - 9) <= 1 and far < 1
    report("A3", ok, f"overlap at 1 kHz (main -12, key -18 dBFS, MARGIN 3, DEPTH 12): main cut at 1 kHz = {cut:.2f} dB (want 9 +- 1); "
                     f"largest logged cut in a band >= 2 ERB from 1 kHz = {far:.2f} dB (want < 1). Logged cuts: {near}")

def A3m():
    """(26 Oct) MARGIN's direction (ControlProof check 2 cannot judge it any more: its key is a kick, whose fundamental the low-end rule ducks by DEPTH whatever
    MARGIN says, SPEC 10): A3's 1 kHz overlap at MARGIN 0 / 3 / 6, DEPTH 12; the cut must rise with MARGIN, about 1 dB per dB (need = (M - K) + MARGIN)."""
    main = sine(1000, -12, 4.0); key = sine(1000, -18, 4.0); cuts = []
    for mg in (0.0, 3.0, 6.0):
        y, _, _ = run(main, key, depth=12, margin=mg); seg = slice(2 * SR, 4 * SR)
        cuts.append(tone_level(main[seg], 1000) - tone_level(y[seg, 0], 1000))
    ok = cuts[0] + 2.0 < cuts[1] and cuts[1] + 2.0 < cuts[2]
    report("A3m", ok, "MARGIN 0 / 3 / 6, the A3 overlap, DEPTH 12: main cut at 1 kHz " + " / ".join(f"{c:.2f}" for c in cuts) + " dB (want rising by more than 2 dB per 3 dB step)")

def A4():
    cut, steady, far = collide_1k(6)
    report("A4", abs(cut - 6) <= 0.5, f"DEPTH 6, same test: main cut at 1 kHz = {cut:.2f} dB (want 6 +- 0.5); largest logged cut {np.max(steady):.2f} dB; bands >= 2 ERB away {far:.2f} dB")

def A5():
    main = sine(60, -12, 4.0); key = sine(60, -18, 4.0)
    y, cuts, _ = run(main, key, depth=12, margin=3, low=120)
    seg = slice(2 * SR, 4 * SR); d = tone_level(y[seg, 0], 60) - tone_level(main[seg], 60)
    report("A5", abs(d) <= 0.05, f"collision at 60 Hz with FOCUS LOW 120 Hz: main out - in = {d:+.4f} dB (want within 0.05); largest logged cut {np.max(cuts):.3f} dB (bands below 120 Hz: {np.max(cuts[:, FC < 120]):.3f})")

def A6():
    # gain curve: key in 10 Hz on/off bursts on pink noise
    n = 6 * SR; main = pink(6.0, -14, 11); g = ((np.arange(n) % (SR // 10)) < SR // 20).astype(np.float32)
    key = pink(6.0, -18, 12) * g
    _, cuts, _ = run(main, key, depth=12, margin=3)
    step = np.max(np.abs(np.diff(cuts, axis=0))) / (1000 * BLOCK / SR)
    rise = np.max(np.diff(cuts, axis=0)) / (1000 * BLOCK / SR)
    # spectral: a 1 kHz collision whose key level glides 12 dB over 6 s, so the cut moves all the time (about 0.002 dB per block)
    # but the modulation itself has no energy beyond a few Hz. Anything outside +-50 Hz of the carrier is then an artefact of the
    # per-block coefficient updates (zipper), not of the control law.
    main2 = sine(1000, -12, 6.0); key2 = sine(1000, -24, 6.0) * amp(12 * np.arange(n) / n).astype(np.float32)
    y, c2, _ = run(main2, key2, depth=12, margin=3)
    seg = y[2 * SR:6 * SR, 0]; w = np.hanning(len(seg)); spec = db(np.abs(np.fft.rfft(seg * w)) / (np.sum(w) / 2)); f = np.fft.rfftfreq(len(seg), 1 / SR)
    carrier = float(np.max(spec[(f > 950) & (f < 1050)]))
    outside = (f < 950) | (f > 1050)
    spur = float(np.max(spec[outside]) - carrier); fspur = float(f[outside][np.argmax(spec[outside])])
    moved = cuts_range = float(c2[-1].max() - c2[int(2 * SR / BLOCK)].max())
    # the gated collision for information: the attack ramp (1 dB/ms, kinked at its ends) has its own AM skirt around the carrier
    main3 = sine(1000, -12, 6.0); key3 = sine(1000, -18, 6.0) * g
    y3, _, _ = run(main3, key3, depth=12, margin=3)
    seg3 = y3[2 * SR:6 * SR, 0]; spec3 = db(np.abs(np.fft.rfft(seg3 * w)) / (np.sum(w) / 2)); car3 = float(np.max(spec3[(f > 900) & (f < 1100)]))
    far3 = (f < 500) | (f > 1500); skirt = float(np.max(spec3[far3]) - car3); fsk = float(f[far3][np.argmax(spec3[far3])])
    ok = step <= 1.0 + 1e-3 and spur <= -90
    report("A6", ok, f"no zipper: key 10 Hz bursts on pink noise: largest cut step {step:.3f} dB/ms (rise {rise:.3f}, want <= 1); "
                     f"1 kHz collision with the key gliding 12 dB over 6 s (cut moved {moved:.2f} dB over the measured 4 s): largest spur outside 950-1050 Hz = {spur:.1f} dBc at {fspur:.0f} Hz (want <= -90). "
                     f"For information, the gated (10 Hz) 1 kHz collision: largest component outside 500-1500 Hz = {skirt:.1f} dBc at {fsk:.0f} Hz (the attack ramp's own AM skirt, see STATUS.md)")

def A7():
    n = 3 * SR; main = pink(3.0, -12, 21); key = pink(3.0, -24, 22); at = int(1.5 * SR)
    imp = main.copy(); imp[at] += 0.5
    y0, _, _ = run(main, key, depth=12, margin=3, want_log=False); y1, cuts, _ = run(imp, key, depth=12, margin=3)
    d = (y1 - y0)[:, 0]; peak = int(np.argmax(np.abs(d))); first = int(np.argmax(np.abs(d) > 1e-3 * np.abs(d[peak])))
    y2, _, _ = run(imp, np.zeros_like(imp), want_log=False); d2 = y2[:, 0] - imp
    ok = peak == at and first == at and np.max(np.abs(d2)) == 0
    report("A7", ok, f"latency: impulse under a live collision (cuts active, mean {np.mean(cuts[cuts > 0]):.2f} dB): response peak at {peak - at:+d} samples, first sample at {first - at:+d} (want 0, 0); "
                     f"with no key the impulse passes bit-exact (max diff {np.max(np.abs(d2)):g}); reported latency 0 (engine has no delay line)")

def A8():
    b, v, src = demo_pair(); n = min(len(b), len(v)); b, v = b[:n], v[:n]
    y, cuts, _ = run(b, v)
    lb, ly = lufs(b), lufs(y)
    on = cuts > 0.1; pct = 100 * on.mean(axis=0)
    eb_k = band_energies(v[:, 0]); keyb = eb_k >= eb_k.max() * 10 ** (-18 / 10)
    e_before = band_energies(b[:, 0])[keyb].sum(); e_after = band_energies(y[:, 0])[keyb].sum()
    busiest = np.argsort(pct)[::-1][:5]
    info("A8", f"real material ({src}, {n / SR:.1f} s, defaults DEPTH 6 MARGIN 3 FOCUS 120-8000 MEDIUM): beat {lb:.2f} LUFS -> {ly:.2f} LUFS, change {ly - lb:+.2f} LU (expected -0.5 to -2); "
               f"cut-time % per band (cut > 0.1 dB): " + " ".join(f"{FC[i]:.0f}:{pct[i]:.0f}" for i in range(32)) + "; "
               f"busiest: " + ", ".join(f"{FC[i]:.0f} Hz {pct[i]:.0f} % (mean {cuts[on[:, i], i].mean() if on[:, i].any() else 0:.1f} dB)" for i in busiest) + "; "
               f"key bands ({', '.join(f'{FC[i]:.0f}' for i in np.where(keyb)[0])} Hz): beat energy before/after {10 * np.log10(e_before / e_after):+.2f} dB = key/beat SNR improvement")

def A9():
    # wall-clock of the engine loop only (dyneqraw --time). The Mac is shared, so the best of three runs counts; all three are printed.
    main = pink(60.0, -12, 31); key = pink(60.0, -18, 32)
    busy = [run(main, key, depth=12, margin=3, want_log=False, timing=True)[2] for _ in range(3)]
    idle = run(main, np.zeros_like(main), want_log=False, timing=True)[2]
    # P1h (SPEC 8d): TARGET is OFF here and its analysis still runs (always listening); the same run with the listening switched off shows what that costs
    nolisten = [run(main, key, depth=12, margin=3, want_log=False, timing=True, extra=["--no-listen"])[2] for _ in range(3)]
    cost = 100.0 * (1.0 / max(busy) - 1.0 / max(nolisten)) / (1.0 / max(nolisten))
    report("A9", max(busy) >= 70, f"CPU, stereo 48 kHz, 60 s, 32 bands, every band cutting (pink main -12 / pink key -18), TARGET OFF with the always-on analysis running: best of 3 = {max(busy):.0f}x real time (want >= 70; runs {', '.join(f'{b:.0f}' for b in busy)}); silent key (analysis only, bells skipped) {idle:.0f}x; "
                                  f"the same without the listening: best of 3 = {max(nolisten):.0f}x (runs {', '.join(f'{b:.0f}' for b in nolisten)}), so the listening costs {cost:.1f} % of the run time")

def self_run(x, **kw): return run(x, None, mode="self", depth=12, margin=3, **kw)

def A13a():
    # static resonance: pink -20 dBFS + RBJ peak +12 dB Q 8 at 2.5 kHz
    res = resonant_pink(6.0, lambda t: 2500.0); y, cuts, _ = self_run(res)
    fr, ct, q = hunters(cuts); last = slice(-int(2 * SR / BLOCK), None)
    mean_cut = ct[last].mean(axis=0); s = int(np.argmax(mean_cut)); on = ct[last, s] > 0
    fmean = float(fr[last][on, s].mean()) if on.any() else 0.0; qmean = float(q[last][on, s].mean()) if on.any() else 0.0
    fstd = float(fr[last][on, s].std()) if on.any() else 0.0; qstd = float(q[last][on, s].std()) if on.any() else 0.0; cstd = float(ct[last, s].std())
    realised = narrow_ratio_db(res[2 * SR:], y[2 * SR:, 0], 2500.0)
    others = ", ".join(f"slot {i + 1} {mean_cut[i]:.2f} dB" for i in range(4) if i != s)
    ok = realised >= 6 and abs(fmean / 2500 - 1) <= 0.05 and on.any()
    report("A13a", ok, f"SELF static resonance (pink -20 dBFS + 12 dB Q 8 at 2.5 kHz, SENS 3, DEPTH 12): realised attenuation at 2.5 kHz +-30 Hz = {realised:.2f} dB (want >= 6); "
                       f"hunter slot {s + 1} over the last 2 s: mean centre {fmean:.0f} Hz ({100 * (fmean / 2500 - 1):+.1f} %, want within +-5), mean cut {mean_cut[s]:.2f} dB (std {cstd:.2f}), mean Q {qmean:.1f} (std {qstd:.1f}), centre std {fstd:.0f} Hz, "
                       f"on {100 * on.mean():.0f} % of blocks; other slots: {others}")

def A13b():
    # gliding resonance on pink: 2.0 kHz for 1 s, then 2.0 -> 3.0 kHz over 4 s. Tracking from 0.5 s to 5 s.
    secs = 5.0; res = resonant_pink(secs, glide_f); y, cuts, _ = self_run(res)
    fr, ct, q = hunters(cuts); t = (np.arange(len(cuts)) + 1) * BLOCK / SR; dom = dominant_slot(fr, ct)
    win = (t >= 0.5) & (t <= 5.0); lost = int(np.sum(dom[win] < 0))
    ftrue = glide_f(t); fs = np.where(dom >= 0, fr[np.arange(len(dom)), np.maximum(dom, 0)], np.nan)
    dev = np.abs(fs / ftrue - 1); dev_w = dev[win]; worst = float(np.nanmax(dev_w)) if np.isfinite(dev_w).any() else 1.0
    tw = float(t[win][np.nanargmax(dev_w)]) if np.isfinite(dev_w).any() else 0.0
    step = float(np.max(np.abs(np.diff(ct, axis=0))) / (1000 * BLOCK / SR))
    mean_cut = float(np.nanmean(np.where(dom[win] >= 0, ct[win].max(axis=1), np.nan)))
    ok = lost == 0 and worst <= 0.08 and step <= 1.0 + 1e-3
    report("A13b", ok, f"SELF gliding resonance (pink + 12 dB Q 8, 2.0 -> 3.0 kHz over 4 s): deepest slot's centre vs the resonance from 0.5 s to 5 s: "
                       f"largest deviation {100 * worst:.2f} % at {tw:.2f} s (want <= 8), blocks with no slot cutting {lost} (want 0), mean cut of the tracking slot {mean_cut:.2f} dB; "
                       f"largest hunter gain step {step:.3f} dB/ms (want <= 1)")

def A13c():
    # spur check needs a deterministic carrier: a -12 dBFS sine gliding 2.0 -> 3.0 kHz the same way (1 s static, then 4 s glide).
    # The output is de-chirped (multiplied by exp(-j phi)), so the carrier sits at 0 Hz; anything beyond +-100 Hz is then a
    # modulation artefact of the hunter bell (per-block gain / frequency / Q updates), not the signal.
    secs = 5.0; n = int(secs * SR); t = np.arange(n) / SR; ph = 2 * np.pi * np.cumsum(glide_f(t)) / SR
    x = (amp(-12) * np.sin(ph)).astype(np.float32); y, cuts, _ = self_run(x)
    fr, ct, q = hunters(cuts); tb = (np.arange(len(cuts)) + 1) * BLOCK / SR; dom = dominant_slot(fr, ct)
    win = (tb >= 0.5) & (tb <= 5.0); fs = np.where(dom >= 0, fr[np.arange(len(dom)), np.maximum(dom, 0)], np.nan)
    dev = np.abs(fs / glide_f(tb) - 1)[win]; worst = float(np.nanmax(dev)) if np.isfinite(dev).any() else 1.0
    lost = int(np.sum(dom[win] < 0)); step = float(np.max(np.abs(np.diff(ct, axis=0))) / (1000 * BLOCK / SR))
    seg = slice(int(1.5 * SR), n); z = y[seg, 0].astype(np.float64) * np.exp(-1j * ph[seg]); w = np.hanning(len(z))
    spec = db(np.abs(np.fft.fft(z * w)) / (np.sum(w) / 2)); f = np.fft.fftfreq(len(z), 1 / SR)
    carrier = float(np.max(spec[np.abs(f) <= 100])); out = (f > 100)   # positive side: the image of the real signal sits at -2 f
    spur = float(np.max(spec[out]) - carrier); fspur = float(f[out][np.argmax(spec[out])])
    cut_in = tone = float(ct[win].max(axis=1).mean())
    ok = spur <= -90 and worst <= 0.08 and lost == 0 and step <= 1.0 + 1e-3
    report("A13c", ok, f"SELF gliding sine (-12 dBFS, 2.0 -> 3.0 kHz over 4 s): de-chirped output, largest spur beyond +-100 Hz of the carrier = {spur:.1f} dBc at {fspur:+.0f} Hz (want <= -90); "
                       f"deepest slot's centre vs the sine: largest deviation {100 * worst:.2f} % (want <= 8), blocks with no slot {lost}, mean cut {cut_in:.2f} dB (a lone sine is the extreme resonance: DEPTH), "
                       f"largest gain step {step:.3f} dB/ms (want <= 1)")

def A13d():
    plain = pink(6.0, -20, 42); y, cuts, _ = self_run(plain); fr, ct, q = hunters(cuts); w = slice(int(1 * SR / BLOCK), None)
    mean_cut = ct[w].mean(axis=0); peak = float(ct[w].max()); pct = 100 * (ct[w] > 0).mean(axis=0)
    ok = float(np.max(mean_cut)) < 0.5
    report("A13d", ok, f"SELF plain pink noise (-20 dBFS, from 1 s): mean cut per slot {', '.join(f'{c:.3f}' for c in mean_cut)} dB (want every one < 0.5); "
                       f"largest instantaneous cut {peak:.2f} dB; slots cutting {', '.join(f'{p:.0f}' for p in pct)} % of blocks")

def A13e():
    z = np.zeros((5 * SR, 2), np.float32); yz, cz, _ = self_run(z); d = float(np.max(np.abs(yz - z)))
    main = resonant_pink(60.0, lambda t: 2500.0, seed=33)
    busy = [self_run(main, want_log=False, timing=True)[2] for _ in range(3)]
    ok = d == 0 and max(busy) >= 60
    report("A13e", ok, f"SELF silence: max |out - in| = {d:g} (want 0), largest hunter cut {float(hunters(cz)[1].max()):.3f} dB; "
                       f"CPU with SELF active (stereo 48 kHz, 60 s pink + resonance, 64-band analysis + hunter bells): best of 3 = {max(busy):.0f}x real time (want >= 60; runs {', '.join(f'{b:.0f}' for b in busy)})")

def R1():
    # realisation of the target: do the bells deliver the logged cut at the band centres? (solve vs literal -cut_b)
    main = pink(6.0, -12, 51); key = pink(6.0, -18, 52); ein = band_energies(main[2 * SR:])
    for name, raw in (("solve", False), ("rawbells", True)):
        y, cuts, _ = run(main, key, depth=12, margin=3, rawbells=raw); t = cuts[int(2 * SR / BLOCK):].mean(axis=0)
        got = 10 * np.log10(ein / band_energies(y[2 * SR:, 0])); foc = (FC >= 120) & (FC <= 8000); err = got - t
        info("R1", f"target realisation ({name}): wide collision (pink main -12 / key -18), FOCUS 120-8000: logged target mean {t[foc].mean():.2f} dB, realised mean {got[foc].mean():.2f} dB, "
                   f"realised - target in focus: max {np.max(err[foc]):+.2f}, min {np.min(err[foc]):+.2f} dB; outside focus realised max {np.max(got[~foc]):.2f} dB; "
                   f"per band (Hz: target/realised): " + " ".join(f"{FC[i]:.0f}:{t[i]:.1f}/{got[i]:.1f}" for i in range(32)))

def R2():
    b, v, _ = demo_pair(); n = 6 * SR; b, v = b[:n], v[:n]
    ref, _, _ = run(b, v, want_log=False); bad = []
    for blk in (1, 7, 64, 517, 4096):
        if not np.array_equal(run(b, v, block=blk, want_log=False)[0], ref): bad.append(str(blk))
    info("R2", f"host block sizes 1, 7, 32, 64, 517, 4096 on the demo pair: {'bit-identical output' if not bad else 'DIFFER at ' + ','.join(bad)}")

# ---------------------------------------------------------------------------------------------- free bands (SPEC 1b-A, items 11 and 12)
BAND_TYPES = ("bell", "lowshelf", "highshelf", "notch", "lowcut", "highcut")

def rbj(kind, f0, q, gain_db, sr=SR):
    """RBJ cookbook biquad (Q form of alpha), normalised (b, a). kind: bell, lowshelf, highshelf, notch, lowcut (high-pass), highcut (low-pass)."""
    A = 10 ** (gain_db / 40); w0 = 2 * np.pi * f0 / sr; c = np.cos(w0); al = np.sin(w0) / (2 * q); sA = np.sqrt(A)
    if kind == "bell":       b = [1 + al * A, -2 * c, 1 - al * A];                      a = [1 + al / A, -2 * c, 1 - al / A]
    elif kind == "notch":    b = [1, -2 * c, 1];                                         a = [1 + al, -2 * c, 1 - al]
    elif kind == "lowcut":   b = [(1 + c) / 2, -(1 + c), (1 + c) / 2];                   a = [1 + al, -2 * c, 1 - al]
    elif kind == "highcut":  b = [(1 - c) / 2, 1 - c, (1 - c) / 2];                      a = [1 + al, -2 * c, 1 - al]
    elif kind == "lowshelf":
        b = [A * ((A + 1) - (A - 1) * c + 2 * sA * al), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - 2 * sA * al)]
        a = [(A + 1) + (A - 1) * c + 2 * sA * al, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - 2 * sA * al]
    elif kind == "highshelf":
        b = [A * ((A + 1) + (A - 1) * c + 2 * sA * al), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - 2 * sA * al)]
        a = [(A + 1) - (A - 1) * c + 2 * sA * al, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - 2 * sA * al]
    else: raise ValueError(kind)
    b = np.array(b, dtype=float); a = np.array(a, dtype=float); return b / a[0], a / a[0]

def response_db(b, a, f, sr=SR):
    with np.errstate(divide='ignore'): return _response_db(b, a, f, sr)

def _response_db(b, a, f, sr=SR):
    z = np.exp(-1j * 2 * np.pi * np.asarray(f, dtype=float) / sr)
    return 20 * np.log10(np.abs((b[0] + b[1] * z + b[2] * z ** 2) / (1 + a[1] * z + a[2] * z ** 2)))

def analog_db(kind, f0, q, gain_db, f, slope=12):
    """The analog prototype the free bands are matched to (TASK_P1n fix 5): RBJ s-domain bell / shelves / notch, Butterworth cascades for the cuts
    (the last stage's Q is multiplied by q)."""
    s = 1j * np.asarray(f, dtype=float) / f0; A = 10 ** (gain_db / 40)
    if kind == "bell": H = (s ** 2 + s * A / q + 1) / (s ** 2 + s / (A * q) + 1)
    elif kind == "lowshelf": H = A * (s ** 2 + np.sqrt(A) / q * s + A) / (A * s ** 2 + np.sqrt(A) / q * s + 1)
    elif kind == "highshelf": H = A * (A * s ** 2 + np.sqrt(A) / q * s + 1) / (s ** 2 + np.sqrt(A) / q * s + A)
    elif kind == "notch": H = (s ** 2 + 1) / (s ** 2 + s / q + 1)
    else:
        N = {12: 2, 24: 4, 36: 6, 48: 8, 72: 12}[slope]; H = np.ones_like(s)
        for k in range(N // 2):
            Q = 1 / (2 * np.sin((2 * k + 1) * np.pi / (2 * N))) * (q if k == 0 else 1.0)
            H = H * ((s ** 2 if kind == "lowcut" else 1) / (s ** 2 + s / Q + 1))
    with np.errstate(divide='ignore'): return 20 * np.log10(np.maximum(np.abs(H), 1e-30))

def measured_response(spec, n=1 << 17):
    """Magnitude response (freq Hz, dB) of one free band from the impulse response through dyneqraw, no key."""
    x = np.zeros(n, np.float32); x[0] = 0.5
    y, _, _ = run(x, None, want_log=False, bands={1: spec})
    f = np.fft.rfftfreq(n, 1 / SR); return f, 20 * np.log10(np.maximum(np.abs(np.fft.rfft(y[:, 0].astype(np.float64))), 1e-30) / 0.5)

def A11():
    sel_f = lambda f: (f >= 20) & (f <= 20000)
    # (a) bell +6 dB, 1 kHz, Q 1 against the analog prototype over the whole band (P1n: the matched design; it used to be the bilinear RBJ biquad, which is also printed)
    f, h = measured_response("type=bell,f=1000,g=6,q=1"); m = sel_f(f)
    dev = float(np.max(np.abs(h[m] - analog_db("bell", 1000, 1, 6, f[m])))); worst = float(f[m][np.argmax(np.abs(h[m] - analog_db("bell", 1000, 1, 6, f[m])))])
    dev_rbj = float(np.max(np.abs(h[m] - response_db(*rbj("bell", 1000, 1, 6), f[m]))))
    # (b) the other gain types and the notch against the same prototype (limit 0.5 dB, TASK_P1n fix 5)
    extra = []
    for kind, spec, args in (("lowshelf", "type=lowshelf,f=200,g=-9,q=0.7", (200, 0.7, -9)), ("highshelf", "type=highshelf,f=5000,g=7,q=0.9", (5000, 0.9, 7)),
                             ("notch", "type=notch,f=3000,q=4", (3000, 4, 0))):
        f2, h2 = measured_response(spec); a2 = analog_db(kind, args[0], args[1], args[2], f2); m2 = sel_f(f2) & (a2 > -60)   # a notch's null is below the float noise floor
        extra.append((kind, float(np.max(np.abs(h2[m2] - a2[m2])))))
    # (c) cuts: one octave past the corner (12 / 24 / 36 / 48 / 72 dB per octave nominal, 1 dB), and the second octave's slope
    slopes = []
    for kind, f_corner, f_probe in (("lowcut", 1000.0, 500.0), ("highcut", 1000.0, 2000.0)):
        for slope in (12, 24, 36, 48, 72):
            f3, h3 = measured_response(f"type={kind},f={f_corner:g},slope={slope}")
            i = int(np.argmin(np.abs(f3 - f_probe))); att = -float(h3[i]); i0 = int(np.argmin(np.abs(f3 - f_corner)))
            slopes.append((kind, slope, att, float(h3[i0])))
    sl_ok = all(abs(att - sl) <= 1.0 and abs(hc + 3.01) <= 0.05 for _, sl, att, hc in slopes)
    ok = dev <= 0.1 and all(d <= 0.5 for _, d in extra) and sl_ok
    report("A11", ok, f"free band, static (vs the analog prototype since P1n): bell +6 dB, 1 kHz, Q 1: largest deviation 20 Hz-20 kHz = {dev:.4f} dB at {worst:.0f} Hz (want <= 0.1; vs the old bilinear RBJ biquad {dev_rbj:.4f} dB); "
                      + "; ".join(f"{k} vs analog {d:.4f} dB" for k, d in extra) + " (want <= 0.5); cuts, attenuation one octave past the corner (want nominal +-1, corner -3.01 +-0.05): "
                      + ", ".join(f"{k} {sl} dB/oct {att:.2f} dB (corner {hc:+.2f} dB)" for k, sl, att, hc in slopes))

def A11c():
    # TASK_P1n fix 7: the last stage of a cut follows Q. Default Q (1) is exact Butterworth; Q 4 puts a peak of at least +6 dB at the corner; the second octave keeps its slope
    rows = []; ok = True
    for kind in ("lowcut", "highcut"):
        for slope in (12, 24, 36, 72):
            r = {}
            for q in (0.3, 1.0, 2.0, 4.0, 8.0):
                f, h = measured_response(f"type={kind},f=1000,slope={slope},q={q}"); m = (f >= 20) & (f <= 20000)
                an = analog_db(kind, 1000, q, 0, f[m], slope); sel = an > -60
                r[q] = (float(np.max(h[m])), float(np.max(np.abs(h[m][sel] - an[sel]))), float(h[int(np.argmin(np.abs(f - 1000)))]))
            f2, h2 = measured_response(f"type={kind},f=1000,slope={slope},q=1")
            pa, pb = (250.0, 500.0) if kind == "lowcut" else (2000.0, 4000.0)
            oct2 = float(h2[int(np.argmin(np.abs(f2 - pb)))] - h2[int(np.argmin(np.abs(f2 - pa)))]) * (1 if kind == "lowcut" else -1)
            good = abs(r[1.0][2] + 3.01) <= 0.05 and r[4.0][0] >= 6.0 and r[2.0][0] < r[4.0][0] < r[8.0][0] and max(v[1] for v in r.values()) <= 1.0
            ok = ok and good
            rows.append(f"{kind} {slope}: corner at Q 1 {r[1.0][2]:+.2f} dB, peak at Q 2/4/8 = {r[2.0][0]:+.2f}/{r[4.0][0]:+.2f}/{r[8.0][0]:+.2f} dB, worst vs analog {max(v[1] for v in r.values()):.2f} dB, second-octave slope {oct2:.1f} (nominal {slope})")
    report("A11c", ok, "cut Q / resonance (corner at default Q -3.01 +-0.05; peak at Q 4 >= +6 dB and rising with Q; all within 1 dB of the analog prototype): " + "; ".join(rows))

def A11b():
    x = beat(10.0); k = np.zeros_like(x)
    off = {i + 1: f"type={BAND_TYPES[i]},f={300 * (i + 1)},g=6,q=1,dyn=1,thr=-40,range=-6,on=0" for i in range(6)}   # settings present, every band off
    y, _, _ = run(x, k, want_log=False, bands=off); d_off = float(np.max(np.abs(y - x)))
    flat = {1: "type=bell,f=1000,g=0,q=1", 2: "type=bell,f=300,g=0,q=2,dyn=1,thr=-30,range=0", 3: "type=lowshelf,f=100,g=0", 4: "type=highshelf,f=8000,g=0"}   # 0 dB bells and shelves, dynamics off / range 0
    y2, c2, _ = run(x, k, bands=flat); d_flat = float(np.max(np.abs(y2 - x)))
    ok = d_off == 0.0 and d_flat == 0.0
    report("A11b", ok, f"all six bands off (settings present) on 10 s of music: max |out - in| = {d_off:g} (want 0); bell / shelves at 0 dB with dynamics off or RANGE 0: max |out - in| = {d_flat:g} (want 0), "
                       f"largest logged band gain {float(np.max(np.abs(LAST_BANDS))):.3f} dB")

def A16():
    # TASK_P1n fix 8: the engine's DELTA is always dry - wet. With nothing active it is silence; with bands it is the removed signal
    x = beat(6.0); k = np.zeros_like(x)
    y0, _, _ = run(x, k, depth=0, delta=1, want_log=False); d0 = float(np.max(np.abs(y0)))
    bands = {1: "type=bell,f=1000,g=-6,q=2", 2: "type=lowcut,f=120,slope=36", 3: "type=notch,f=3000,q=6"}
    yw, _, _ = run(x, k, depth=0, want_log=False, bands=bands); yd, _, _ = run(x, k, depth=0, delta=1, want_log=False, bands=bands)
    x2 = x if x.ndim == 2 else np.stack([x, x], axis=1); ref = x2 - yw; e = float(np.max(np.abs(yd - ref))); lvl = rms_db(yd)
    report("A16", d0 == 0.0 and e <= 1e-6 and lvl > -60, f"engine DELTA: no band, no cut -> silence, max |out| = {d0:g} (want 0); three bands -> out == in - wet, max difference {e:.2e} (want <= 1e-6), removed signal {lvl:.1f} dBFS rms")

def A12d():
    # TASK_P1n fix 6: the dynamic band's detector reads a sine's peak level. THR -23, KNEE 6, RANGE -6: an exact detector gives -3.00 dB of gain at -20 dBFS
    rows = []; ok = True
    for f0 in (20, 30, 100, 1000, 10000, 18000):
        for q in (1, 4, 18):
            x = sine(f0, -20, 4.0); y, _, _ = run(x, None, depth=0, bands={1: f"type=bell,f={f0},g=0,q={q},dyn=1,thr=-23,range=-6,att=10,rel=100"})
            g = LAST_BANDS[int(2.0 * SR / BLOCK):, 0]; read = -23 + 6 * float(np.mean(g)) / -6; err = read + 20; rip = float(np.ptp(g))
            ok = ok and abs(err) <= 0.3 and (f0 != 30 or rip <= 0.05)
            if q == 4 or f0 in (20, 30): rows.append(f"{f0} Hz Q {q}: {err:+.3f} dB, ripple {rip:.4f} dB")
    h3 = {}
    for att, rel in ((10, 100), (1, 10)):
        x = sine(30, -20, 4.0); y, _, _ = run(x, None, depth=0, bands={1: f"type=bell,f=30,g=0,q=2,dyn=1,thr=-23,range=-6,att={att},rel={rel}"})
        seg = np.asarray(y[2 * SR:, 0], dtype=np.float64); h3[(att, rel)] = tone_level(seg, 90) - tone_level(seg, 30)
    ok = ok and h3[(10, 100)] <= -60 and h3[(1, 10)] <= -45
    report("A12d", ok, "dynamic detector, -20 dBFS sines 20 Hz - 18 kHz at Q 1 / 4 / 18 (want the level read within +-0.3 dB, gain ripple <= 0.05 dB p-p at 30 Hz): " + "; ".join(rows)
                       + f"; 3rd harmonic of a steady 30 Hz tone at mid knee: {h3[(10, 100)]:.1f} dBc at ATTACK 10 / RELEASE 100 (want <= -60), {h3[(1, 10)]:.1f} dBc at ATTACK 1 / RELEASE 10 (want <= -45)")

def _click_rows(rows, tag, step_max, hf_max, what):
    from scipy import signal
    sos = signal.butter(8, 4000, "highpass", fs=SR, output="sos"); own = 0.25 * 2 * np.pi * 1000 / SR
    n = int(1.2 * SR); x = (0.25 * np.cos(2 * np.pi * 1000 * np.arange(n) / SR)).astype(np.float32); out = []; ok = True
    for name, before, after_ in rows:
        y, _, _ = run(x, None, depth=0, bands={1: before}, after=(1.0, {1: after_}), want_log=False)
        yy = y[:, 0].astype(np.float64); step = float(np.max(np.abs(np.diff(yy[47990:48100])))) / own
        h = signal.sosfiltfilt(sos, yy); hf = 20 * np.log10(np.max(np.abs(h[47000:49500])) / 0.25 + 1e-30)
        ok = ok and step <= step_max and hf <= hf_max; out.append(f"{name}: step {step:.2f}x, HF {hf:.1f} dBc")
    report(tag, ok, f"{what} (a 1 kHz cosine -12 dBFS peak, switch at a waveform peak, 1.000 s; want step <= {step_max}x the sine's own step and HF above 4 kHz <= {hf_max} dBc): " + "; ".join(out))

def A17():
    # TASK_P1n fix 1: a band that switches ON
    _click_rows([("bell +12", "type=bell,f=1000,g=12,q=1,on=0", "type=bell,f=1000,g=12,q=1"), ("bell -18", "type=bell,f=1000,g=-18,q=1,on=0", "type=bell,f=1000,g=-18,q=1"),
                 ("high shelf 3k +12", "type=highshelf,f=3000,g=12,q=0.71,on=0", "type=highshelf,f=3000,g=12,q=0.71"), ("notch 1k Q 8", "type=notch,f=1000,q=8,on=0", "type=notch,f=1000,q=8"),
                 ("high cut 500 /12", "type=highcut,f=500,slope=12,on=0", "type=highcut,f=500,slope=12"), ("low cut 2k /48", "type=lowcut,f=2000,slope=48,on=0", "type=lowcut,f=2000,slope=48"),
                 ("low cut 1k /72", "type=lowcut,f=1000,slope=72,on=0", "type=lowcut,f=1000,slope=72")], "A17", 1.5, -60.0, "band switch ON")

def A18():
    # TASK_P1n fix 3: a type or slope change while the band is on
    _click_rows([("bell 1.5k +12 -> notch 1.5k", "type=bell,f=1500,g=12,q=2", "type=notch,f=1500,q=2"), ("low cut 2k 12 -> 48", "type=lowcut,f=2000,slope=12", "type=lowcut,f=2000,slope=48"),
                 ("high cut 3k 24 -> 72", "type=highcut,f=3000,slope=24", "type=highcut,f=3000,slope=72"), ("bell 1k +9 -> high shelf 2k +6", "type=bell,f=1000,g=9,q=1", "type=highshelf,f=2000,g=6,q=0.71")],
                "A18", 99.0, -70.0, "band type / slope change")

DYN = "type=bell,f=1000,g=0,q=1,dyn=1,thr=-30,range=-6,att=10,rel=100"

def tail_cut(main, key, spec, secs=4.0):
    y, c, _ = run(main, key, depth=0, bands={1: spec})   # DEPTH 0: the 32-band bank stays out of the way
    seg = slice(int(2.0 * SR), int(secs * SR)); return tone_level(main[seg], 1000) - tone_level(y[seg, 0], 1000), LAST_BANDS[-1, 0]

def A12():
    t = 4.0; res = []
    for lvl in (-20, -40): res.append(("SELF", f"main {lvl}", tail_cut(sine(1000, lvl, t), None, DYN)))
    k_on = DYN + ",trig=key"
    res.append(("KEY", "key -20, main -40", tail_cut(sine(1000, -40, t), sine(1000, -20, t), k_on)))
    res.append(("KEY", "key -40, main -20", tail_cut(sine(1000, -20, t), sine(1000, -40, t), k_on)))
    res.append(("KEY", "no key routed, main -20", tail_cut(sine(1000, -20, t), None, k_on)))
    ok = (abs(res[0][2][0] - 6) <= 0.5 and abs(res[1][2][0]) <= 0.1 and abs(res[2][2][0] - 6) <= 0.5 and abs(res[3][2][0]) <= 0.1 and abs(res[4][2][0]) <= 0.1)
    report("A12", ok, "free band, dynamic (bell 1 kHz, GAIN 0, THR -30, RANGE -6, ATTACK 10, RELEASE 100): measured cut at 1 kHz over the last 2 s (want 6 +-0.5 at -20 dBFS, 0 +-0.1 at -40) -> "
                      + "; ".join(f"TRIGGER {m}, {w}: {c:.3f} dB (logged gain {g:+.3f})" for m, w, (c, g) in res))

def stepped(secs, lo, hi, period=1.0, edge=0.0, sr=SR):
    """Amplitude (linear) alternating lo -> hi -> lo ... every period/2 s, starting at lo; edge > 0: raised-cosine transitions of that length."""
    n = int(secs * sr); t = np.arange(n) / sr; ph = (t % period) / period; up = ph >= 0.5
    if edge <= 0: return np.where(up, amp(hi), amp(lo))
    half = period / 2; tt = t % half; frac = np.clip(tt / edge, 0, 1); w = 0.5 - 0.5 * np.cos(np.pi * frac)   # 0 -> 1 over `edge` after each switch
    a_from = np.where(up, lo, hi); a_to = np.where(up, hi, lo)
    return amp(a_from) + (amp(a_to) - amp(a_from)) * w

def spur_outside(y, f0=1000.0, half=50.0, start=0.5):
    seg = np.asarray(y[int(start * SR):], dtype=np.float64); w = np.hanning(len(seg)); sp = db(np.abs(np.fft.rfft(seg * w)) / (np.sum(w) / 2)); f = np.fft.rfftfreq(len(seg), 1 / SR)
    inn = np.abs(f - f0) <= half; car = float(np.max(sp[inn])); out = ~inn
    return float(np.max(sp[out]) - car), float(f[out][np.argmax(sp[out])])

def A12b():
    # What can and cannot be measured here (numbers in STATUS.md "P1b"): any 6 dB gain move that takes only a few ms has AM sidebands of its own
    # beyond +-50 Hz of the carrier (the control law, not zipper), and a hard step in the input splatters by itself. So, as in A6 / A13c:
    #  - the 1 dB/ms limit is measured on hard steps, with the harshest ATTACK / RELEASE (the limiter then works at every edge);
    #  - the -90 dBc spur limit is measured on a glide of the trigger level (the gain keeps moving, but its modulation sits within a few Hz),
    #    so anything outside +-50 Hz is an artefact of the per-block coefficient updates;
    #  - the literal "hard step, spur outside +-50 Hz" figure is printed for information (not gated).
    secs = 6.0; n = int(secs * SR); ph = 2 * np.pi * 1000.0 * np.arange(n) / SR; fast = "type=bell,f=1000,g=0,q=1,dyn=1,thr=-30,range=-6,att=1,rel=10"
    main = (amp(-30) * np.sin(ph)).astype(np.float32)
    # (1) hard steps -40 <-> -20 dBFS every 0.5 s: KEY trigger (steady main), SELF trigger (the tone steps)
    key = (stepped(secs, -40, -20) * np.sin(ph)).astype(np.float32)
    yk, _, _ = run(main, key, depth=0, bands={1: fast + ",trig=key"}); gk = LAST_BANDS[:, 0]
    stepk = float(np.max(np.abs(np.diff(gk))) / (1000 * BLOCK / SR)); swing_k = float(gk.max() - gk.min()); spk, fk = spur_outside(yk[:, 0])
    ys, _, _ = run(key, None, depth=0, bands={1: fast}); gs = LAST_BANDS[:, 0]
    steps = float(np.max(np.abs(np.diff(gs))) / (1000 * BLOCK / SR)); swing_s = float(gs.max() - gs.min()); sps, fs = spur_outside(ys[:, 0]); spin, fin = spur_outside(key)
    def beyond(y):   # offset from the carrier beyond which every component is <= -90 dBc
        seg = np.asarray(y[int(0.5 * SR):], dtype=np.float64); w = np.hanning(len(seg)); sp = db(np.abs(np.fft.rfft(seg * w)) / (np.sum(w) / 2)); f = np.fft.rfftfreq(len(seg), 1 / SR)
        sp = sp - sp[np.abs(f - 1000) <= 50].max(); off = np.abs(f - 1000); return float(off[sp > -90].max())
    # (2) glide: the trigger level moves -40 -> -20 dBFS over the 6 s (the gain moves 6 dB while L crosses the knee, about 1 dB/s)
    g_amp = amp(-40 + 20 * np.arange(n) / n).astype(np.float32); glide = (g_amp * np.sin(ph)).astype(np.float32)
    ygk, _, _ = run(main, glide, depth=0, bands={1: fast + ",trig=key"}); swg_k = float(LAST_BANDS[:, 0].max() - LAST_BANDS[:, 0].min()); spgk, fgk = spur_outside(ygk[:, 0])
    ygs, _, _ = run(glide, None, depth=0, bands={1: fast}); swg_s = float(LAST_BANDS[:, 0].max() - LAST_BANDS[:, 0].min()); spgs, fgs = spur_outside(ygs[:, 0])
    ok = stepk <= 1.0 + 1e-3 and steps <= 1.0 + 1e-3 and swing_k > 5 and swing_s > 5 and spgk <= -90 and spgs <= -90 and swg_k > 5 and swg_s > 5
    report("A12b", ok, f"1 kHz tone stepping -40 <-> -20 dBFS, ATTACK 1 / RELEASE 10 (the rate limiter works at every edge): largest gain step {stepk:.3f} dB/ms with TRIGGER KEY, {steps:.3f} dB/ms with TRIGGER SELF (want <= 1), "
                       f"gain swing {swing_k:.2f} / {swing_s:.2f} dB (the full 6); spur check on a trigger-level glide -40 -> -20 dBFS over 6 s (gain moves {swg_k:.2f} / {swg_s:.2f} dB): largest spur outside +-50 Hz "
                       f"{spgk:.1f} dBc at {fgk:.0f} Hz (KEY), {spgs:.1f} dBc at {fgs:.0f} Hz (SELF) (want <= -90). NOT GATED, literal reading (hard steps): largest component outside +-50 Hz = {spk:.1f} dBc at {fk:.0f} Hz (KEY, steady main), "
                       f"{sps:.1f} dBc at {fs:.0f} Hz (SELF; the stepping input alone has {spin:.1f} dBc at {fin:.0f} Hz); everything stays <= -90 dBc beyond +-{beyond(yk[:, 0]):.0f} Hz (KEY)")

def A9b():
    # all six bands dynamic (bell, shelves, notch static, bells; self and key triggers) plus the COLLIDE bank cutting everywhere
    main = pink(60.0, -12, 31); key = pink(60.0, -18, 32)
    bands = {1: "type=bell,f=200,g=2,q=1,dyn=1,thr=-40,range=-6,trig=key", 2: "type=lowshelf,f=120,g=-2,q=0.7,dyn=1,thr=-40,range=4",
             3: "type=bell,f=1000,g=0,q=2,dyn=1,thr=-40,range=-6,trig=key", 4: "type=highshelf,f=8000,g=1,q=0.7,dyn=1,thr=-40,range=-4",
             5: "type=bell,f=3000,g=-1,q=3,dyn=1,thr=-40,range=5", 6: "type=bell,f=5000,g=0,q=1.5,dyn=1,thr=-40,range=-6,trig=key"}
    busy = [run(main, key, depth=12, margin=3, want_log=False, timing=True, bands=bands)[2] for _ in range(3)]
    report("A9b", max(busy) >= 50, f"CPU, stereo 48 kHz, 60 s, six dynamic bands (bell / shelves, self and key triggers) + the 32-band COLLIDE bank cutting (pink main -12 / pink key -18): best of 3 = {max(busy):.0f}x real time "
                                   f"(want >= 50; runs {', '.join(f'{b:.0f}' for b in busy)})")

def R3():
    # host block sizes with free bands live (dynamic, key and self triggers, moving gains): bit-identical output?
    b, v, _ = demo_pair(); n = 6 * SR; b, v = b[:n], v[:n]
    bands = {1: "type=bell,f=900,g=1,q=1.5,dyn=1,thr=-40,range=-6,trig=key", 2: "type=lowshelf,f=150,g=0,q=0.7,dyn=1,thr=-35,range=-5", 3: "type=lowcut,f=60,slope=24", 4: "type=highshelf,f=7000,g=-2,q=0.8"}
    ref, _, _ = run(b, v, want_log=False, bands=bands); bad = []
    for blk in (1, 7, 64, 517, 4096):
        if not np.array_equal(run(b, v, block=blk, want_log=False, bands=bands)[0], ref): bad.append(str(blk))
    info("R3", f"host block sizes 1, 7, 32, 64, 517, 4096 with four free bands live on the demo pair: {'bit-identical output' if not bad else 'DIFFER at ' + ','.join(bad)}")

def R4():
    # SPEC 7.3 A/B (Params::tiling, --tiling): analysis band-passes -3 dB instead of -12 dB at the band edges.
    # Calibration: the COLLIDE gate K_b >= -60 dBFS is a hard threshold on the key's band level, so the lowest key level (a sine at a band centre)
    # that makes any cut appear reads that level. Bisected on the key level (main pink -12), both modes; want the two within 0.5 dB.
    main = pink(4.0, -12, 61); thr = {}
    for b in (10, 16, 22):
        for mode in (False, True):
            lo, hi = -66.0, -54.0
            for _ in range(7):
                mid = 0.5 * (lo + hi); _, c, _ = run(main, sine(float(FC[b]), mid, 4.0), depth=12, margin=3, low=40, high=16000, tiling=mode)
                if np.max(c[-int(1.0 * SR / BLOCK):]) > 1e-3: hi = mid
                else: lo = mid
            thr[(b, mode)] = 0.5 * (lo + hi)
    dev = max(abs(thr[(b, True)] - thr[(b, False)]) for b in (10, 16, 22))
    txt = ", ".join(f"{FC[b]:.0f} Hz: {thr[(b, False)]:.2f} / {thr[(b, True)]:.2f} dBFS" for b in (10, 16, 22))
    a3 = []
    for mode in (False, True):
        cut, steady, far = collide_1k_t(12, mode); a3.append((cut, far, steady))
    near = ", ".join(f"{FC[b]:.0f} Hz {a3[1][2][b]:.2f}" for b in range(8, 17))
    info("R4", f"tiling calibration: key level at which the -60 dB gate opens, default / tiling: {txt}; largest difference {dev:.2f} dB (want <= 0.5, {'OK' if dev <= 0.5 else 'OUT'}); "
               f"A3 with tiling on (DEPTH 12): main cut at 1 kHz = {a3[1][0]:.2f} dB (default {a3[0][0]:.2f}), largest cut >= 2 ERB away {a3[1][1]:.2f} dB (default {a3[0][1]:.2f}). Logged cuts with tiling: {near}")

def collide_1k_t(depth, tiling):
    main = sine(1000, -12, 4.0); key = sine(1000, -18, 4.0)
    y, cuts, _ = run(main, key, depth=depth, margin=3, tiling=tiling)
    seg = slice(2 * SR, 4 * SR); cut = tone_level(main[seg], 1000) - tone_level(y[seg, 0], 1000)
    steady = cuts[-int(1.0 * SR / BLOCK):].mean(axis=0); far = np.abs(erb_rate(FC) - erb_rate(1000)) >= 2.0
    return cut, steady, float(np.max(steady[far]))

def A12c():
    # A12 for a shelf: highshelf at 4 kHz, 6 kHz tone, GAIN 0, THR -30, RANGE -6 (the shelf's plateau is the full 6 dB at 6 kHz only if its
    # response there is the plateau; the measured cut is compared with the 6 dB the law asks for through the shelf's own response at 6 kHz)
    spec = "type=highshelf,f=4000,g=0,q=0.7,dyn=1,thr=-30,range=-6,att=10,rel=100"; res = {}
    for lvl in (-20, -40):
        main = sine(6000, lvl, 4.0); y, _, _ = run(main, None, depth=0, bands={1: spec}); seg = slice(2 * SR, 4 * SR)
        res[lvl] = (tone_level(main[seg], 6000) - tone_level(y[seg, 0], 6000), LAST_BANDS[-1, 0])
    # static reference: the same shelf at -6 dB, so the expected cut at 6 kHz is what the filter really does there
    main = sine(6000, -20, 4.0); y, _, _ = run(main, None, depth=0, bands={1: "type=highshelf,f=4000,g=-6,q=0.7"}); seg = slice(2 * SR, 4 * SR)
    ref = tone_level(main[seg], 6000) - tone_level(y[seg, 0], 6000)
    # The literal 6 +-0.5 cannot hold: a 4 kHz shelf with Q 0.7 has not reached its plateau at 6 kHz (the static shelf measures the same number).
    # Gated instead: the dynamic cut equals the static -6 dB shelf at 6 kHz (+-0.1), the logged gain is the full -6, and 0 +-0.1 at -40.
    ok = abs(res[-20][0] - ref) <= 0.1 and abs(res[-20][1] + 6) <= 0.01 and abs(res[-40][0]) <= 0.1
    report("A12c", ok, f"free band, dynamic shelf (highshelf 4 kHz, Q 0.7, GAIN 0, THR -30, RANGE -6, ATTACK 10, RELEASE 100), 6 kHz tone, cut measured over the last 2 s "
                       f"(literal want 6 +-0.5 at -20 dBFS: {'met' if abs(res[-20][0] - 6) <= 0.5 else 'NOT met, the shelf is not at its plateau at 6 kHz'}; gated: equals the static -6 dB shelf +-0.1, and 0 +-0.1 at -40): -20 dBFS {res[-20][0]:.3f} dB (logged gain {res[-20][1]:+.3f}), -40 dBFS {res[-40][0]:.3f} dB (logged gain {res[-40][1]:+.3f}); "
                       f"static shelf at -6 dB gives {ref:.3f} dB at 6 kHz")

def max_jump(x, a=0, b=None): x = np.asarray(x, dtype=np.float64); return float(np.max(np.abs(np.diff(x[a:b]))))

def A14():
    # click check on band changes mid-stream: 1 kHz sine at -12 dBFS; (a) notch at 1 kHz switched off, (b) a bell (+3 dB; at +6 dB the bell's own steady output already jumps 6 dB more than the input, so the limit could not tell a click from the gain) turned into a low cut.
    # Metric: the largest sample-to-sample jump of the output from the switch to 100 ms after, against the input's own largest jump (want <= +6 dB).
    secs = 2.0; t_sw = 1.0; x = sine(1000, -12, secs); a = int(t_sw * SR); b = a + int(0.1 * SR); injump = max_jump(x[a:b + 1]); rows = []; ok = True
    for name, bands, after in (("notch 1 kHz -> off", {1: "type=notch,f=1000,q=1"}, {1: "off"}),
                               ("bell +3 dB -> low cut 24 dB/oct at 1 kHz", {1: "type=bell,f=1000,g=3,q=1"}, {1: "type=lowcut,f=1000,slope=24"}),
                               ("low cut 12 dB/oct at 1 kHz -> off", {1: "type=lowcut,f=1000,slope=12"}, {1: "off"})):
        worst = None
        for t0 in (t_sw, t_sw + 0.00017, t_sw + 0.00041):   # three switch phases of the sine
            xx = sine(1000, -12, secs); y, _, _ = run(xx, None, depth=0, bands=bands, after=(t0, after), want_log=False)
            a0 = int(t0 * SR); j = max_jump(y[a0:a0 + int(0.1 * SR) + 1, 0]); worst = j if worst is None else max(worst, j)
        over = 20 * np.log10(worst / injump); good = over <= 6.0; ok &= good
        rows.append(f"{name}: largest jump {worst:.5f} vs input {injump:.5f} = {over:+.2f} dB ({'ok' if good else 'CLICK'})")
    report("A14", ok, "band change mid-stream, 1 kHz sine -12 dBFS, worst of three switch phases, 100 ms after the switch (want <= +6 dB over the input's own largest jump): " + "; ".join(rows))

def run_bake(main, key, depth=12.0, margin=3.0, bands=None, speed="medium"):
    """dyneqraw --bake: returns (parsed BAKE lines, cuts (blocks, 32), stdout). The render itself is the normal one."""
    main = np.asarray(main, dtype=np.float32); key = np.asarray(key, dtype=np.float32)
    fm, fk, fo, fl = (os.path.join(TMP, n) for n in ("bm.wav", "bk.wav", "bo.wav", "bc.csv"))
    write_wav(fm, np.stack([main, main], axis=1)); write_wav(fk, np.stack([key, key], axis=1))
    cmd = [RAW, fm, fk, fo, "--depth", str(depth), "--margin", str(margin), "--speed", speed, "--cutlog", fl, "--bake"]
    for k, spec in sorted((bands or {}).items()): cmd += [f"--band{k}", spec]
    r = subprocess.run(cmd, check=True, capture_output=True, text=True)
    lines = [l for l in r.stdout.splitlines() if l.startswith("BAKE ")]
    cuts = np.loadtxt(fl, delimiter=",", skiprows=1, ndmin=2)[:, 1:33]
    return lines, cuts

def parse_bake(lines):
    out = {"bells": [], "specs": []}
    for l in lines:
        t = l[5:]
        if t.startswith("counted="): out["head"] = dict(kv.split("=") for kv in t.split())
        elif t.startswith("mean_cut_db"): out["mean"] = np.array([float(x) for x in t.split()[1:]])
        elif t.startswith("percent_cut"): out["pct"] = np.array([float(x) for x in t.split()[1:]])
        elif t.startswith("curve_db"): out["curve"] = np.array([float(x) for x in t.split()[1:]])
        elif t.startswith("residual_db"): out["resid"] = np.array([float(x) for x in t.split()[1:]])
        elif t.startswith("bell "):
            body = t.split("|")[0].split(":", 1)[1]; out["bells"].append(dict(kv.split("=") for kv in body.split())); out["text"] = out.get("text", []) + [t.split("|")[1].strip()]
        elif t.startswith("specs"): out["specs"] = t.split()[1:]
        elif t.startswith("undo_params"): out["undo"] = dict(kv.split("=") for kv in t.split()[1:])
    return out

def A15():
    # SPEC item 14: a fixed 1 kHz collision of 9 dB present about 50 % of the time (main -12 dBFS continuous, key -18, DEPTH 12, MARGIN 3, SPEED fast).
    # The cut outlives the key: it releases only after the key's analysis envelope has fallen under the -60 dBFS floor (about 0.4 s at FAST). So the key is on
    # for the first 1.6 s of every 4 s and the cut is present about 50 % of the time (the percentage the engine measured is in the report).
    secs = 16.0; main = sine(1000, -12, secs); key = sine(1000, -18, secs)
    on = (np.arange(len(key)) % (4 * SR)) < int(1.6 * SR); key = (key * on).astype(np.float32)
    lines, cuts = run_bake(main, key, speed="fast"); r = parse_bake(lines)
    measured = cuts.mean(axis=0)                                  # mean of the logged target cut per band over the whole run (every block)
    stat_dev = float(np.max(np.abs(r["mean"] - measured)))        # the engine's own statistics against the log
    bells = r["bells"]; nb = len(bells); b1 = bells[0] if bells else None
    near = nearest_band(1000)
    within_band = b1 is not None and abs(int(b1["band"]) - near) <= 1
    gain_dev = abs(-float(b1["g"]) - measured[int(b1["band"])]) if b1 else 99.0
    # applied: the static bells through the engine (no key, impulse) against the mean-cut curve at the 32 band centres
    n = 1 << 17; x = np.zeros(n, np.float32); x[0] = 0.5
    y, _, _ = run(x, None, depth=0, want_log=False, bands={i + 1: sp for i, sp in enumerate(r["specs"])})
    f = np.fft.rfftfreq(n, 1 / SR); h = 20 * np.log10(np.maximum(np.abs(np.fft.rfft(y[:, 0].astype(np.float64))), 1e-30) / 0.5)
    realised = np.array([np.interp(fc, f, h) for fc in FC])      # dB at the 32 centres (negative = cut)
    curve_dev = float(np.max(np.abs(realised + measured)))
    analytic_dev = float(np.max(np.abs(realised - r["curve"])))
    undo_ok = r.get("undo", {}).get("restored_bit_exact") == "1" and int(r["undo"]["bands_changed"]) == nb == int(r["undo"]["written"])
    present = float(np.mean(cuts[:, int(np.argmax(measured))] > 0.1)) * 100
    ok = (abs(present - 50) <= 12 and within_band and gain_dev <= 1.0 and curve_dev <= 1.0 and analytic_dev <= 0.1 and stat_dev <= 1e-3 and undo_ok)
    txt = ", ".join(r["text"])
    report("A15", ok, f"BAKE, 1 kHz collision of 9 dB present 50 % of the time (key on 1.6 s of every 4 s, SPEED fast, {secs:.0f} s): counted blocks {r['head']['counted']}; largest mean cut {measured.max():.2f} dB at "
                      f"{FC[int(np.argmax(measured))]:.0f} Hz, that band cut {present:.0f} % of the blocks (want 50 +-12); engine statistics vs mean of the cut log: largest difference {stat_dev:.5f} dB (want <= 0.001); {nb} bell(s): {txt}; "
                      f"first bell band {b1['band'] if b1 else '-'} ({float(b1['f']):.0f} Hz) vs the band of 1 kHz {near} ({FC[near]:.0f} Hz): within one band = {within_band} (want True); "
                      f"first bell gain {float(b1['g']):.2f} dB vs measured mean cut {measured[int(b1['band'])]:.2f} dB at that band: difference {gain_dev:.2f} dB (want <= 1); "
                      f"applied static curve (impulse through the engine, no key) vs the mean-cut curve at the 32 centres: largest difference {curve_dev:.2f} dB (want <= 1), vs the analytic curve {analytic_dev:.3f} dB (want <= 0.1); "
                      f"undo at the parameter level (applied into {r['undo']['bands_changed']} empty bands, saved copy restored): bit-exact {r['undo']['restored_bit_exact']} (the plug-in's real SvlUndo step is in plugin_check)")


# ---------------------------------------------------------------------------------------------- TARGET (SPEC 8, P1e): T1-T8
from scipy.signal import butter, sosfilt, sosfiltfilt, fftconvolve

def kick_hit(rng, secs=0.6, sr=SR):
    """Synthetic kick: a 4 ms noise click (white, tau 0.7 ms, 0.6) + a 60 Hz boom (tau 120 ms, 0.7)."""
    t = np.arange(int(secs * sr)) / sr
    click = 0.6 * rng.standard_normal(len(t)) * np.exp(-t / 0.0007) * (t < 0.004)
    boom = 0.7 * np.sin(2 * np.pi * 60 * t) * np.exp(-t / 0.12)
    return (click + boom).astype(np.float32)

def snare_hit(rng, secs=0.8, sr=SR):
    """Synthetic snare: a noise hit (white, tau 25 ms, 0.5) + a 200 Hz ring (tau 100 ms, 0.3)."""
    t = np.arange(int(secs * sr)) / sr
    return (0.5 * rng.standard_normal(len(t)) * np.exp(-t / 0.025) + 0.3 * np.sin(2 * np.pi * 200 * t) * np.exp(-t / 0.1)).astype(np.float32)

def place(hits, times, secs, sr=SR):
    x = np.zeros(int(secs * sr), np.float32)
    for h, t in zip(hits, times):
        a = int(round(t * sr)); n = min(len(h), len(x) - a); x[a:a + n] += h[:n]
    return x

def bp(x, lo, hi, sr=SR): return sosfilt(butter(2, [lo, hi], "band", fs=sr, output="sos"), np.asarray(x, dtype=np.float64))
def seg_db(y, x, t0, t1, sr=SR):
    """Energy of y over [t0, t1] s relative to x, dB (negative = y has less)."""
    a, b = int(t0 * sr), int(t1 * sr); return 10 * np.log10((np.sum(np.asarray(y[a:b], np.float64) ** 2) + 1e-30) / (np.sum(np.asarray(x[a:b], np.float64) ** 2) + 1e-30))
def band_db(y, x, t0, t1, lo, hi, hann=True, sr=SR):
    """Energy in lo..hi Hz of y over [t0, t1] s relative to x, dB (negative = y has less). FFT of the segment, so a narrow filter's own memory cannot smear a fast gain change."""
    a, b = int(t0 * sr), int(t1 * sr); w = np.hanning(b - a) if hann else np.ones(b - a); f = np.fft.rfftfreq(b - a, 1 / sr); m = (f >= lo) & (f <= hi)
    e = lambda s: np.sum(np.abs(np.fft.rfft(np.asarray(s[a:b], np.float64) * w))[m] ** 2)
    return float(10 * np.log10((e(y) + 1e-30) / (e(x) + 1e-30)))
def peak_db(y, x, t0, t1, sr=SR):
    a, b = int(t0 * sr), int(t1 * sr); return float(db(np.max(np.abs(y[a:b]))) - db(np.max(np.abs(x[a:b]))))

def T1():
    b, v, src = demo_pair(); n = min(len(b), 10 * SR); b, v = b[:n], v[:n]
    ref, _, _ = run(b, v, depth=6, margin=3, want_log=False)
    off, _, _ = run(b, v, depth=6, margin=3, want_log=False, target="inst=off,part=20,amount=18,width=2,reverb=1")
    quiet, _, _ = run(b, None, want_log=False); quiet_off, _, _ = run(b, None, want_log=False, target="inst=off,part=100,amount=18,width=0.5,reverb=1")
    cut = float(np.max(np.abs(ref - np.stack([b, b], axis=1) if b.ndim == 1 else ref - b)))
    same = np.array_equal(ref, off) and np.array_equal(quiet, quiet_off)
    report("T1", same, f"TARGET OFF with PART 20 / AMOUNT 18 / WIDTH 2 / REVERB-READY on, {n / SR:.0f} s of {src} music (COLLIDE cutting, largest change the COLLIDE made {cut:.4f}): output bit-identical to the run without TARGET = {np.array_equal(ref, off)}; no other cut: bit-identical = {np.array_equal(quiet, quiet_off)}")

# ---- P1g (TASK_P1g): the generators have a known truth, so "found where it is" is a number, not an opinion
SUB_EDGE_X = 1.2 * 1.5   # SPEC 8e.2 / engine: the SUB crossover = kEdgeBelowShare (1.2) x the sub's upper edge (1.5 f0)
BODY_X = 2.75   # the generator's body sits at 3 x the fundamental (45 / 55 / 70 Hz -> 135 / 165 / 210 Hz)
def kick6_parts(f0, fc, secs=0.7, sr=SR, bodyx=None):
    """Synthetic kick with three parts at known places: SUB = a sine at f0 (tau 120 ms), BODY = 5 lines 1/10 oct apart around 3 x f0 (tau 40 ms),
    CLICK = 5 lines 1/6 oct apart around fc (+-1/3 oct, the 4 ms burst with tau 0.7 ms). Truth: fundamental f0, body centre 3 f0, click centre fc."""
    t = np.arange(int(secs * sr)) / sr
    sub = 0.55 * np.sin(2 * np.pi * f0 * t) * np.exp(-t / 0.12)
    body = sum(0.07 * np.sin(2 * np.pi * (BODY_X if bodyx is None else bodyx) * f0 * 2 ** (k / 10) * t) for k in range(-2, 3)) * np.exp(-t / 0.04)
    click = sum(0.12 * np.sin(2 * np.pi * fc * 2 ** (k / 6) * t) for k in range(-2, 3)) * np.exp(-t / 0.0007) * (t < 0.004)
    return sub.astype(np.float32), body.astype(np.float32), click.astype(np.float32)
def kick6(f0, fc, secs=0.7, sr=SR): return sum(kick6_parts(f0, fc, secs, sr))

def hit_track(hit_list, spacing=0.5, lead=0.5, tail=0.6, sr=SR):
    """hit_list: arrays; returns the mono track and the onset times."""
    times = [lead + k * spacing for k in range(len(hit_list))]
    x = np.zeros(int((times[-1] + tail) * sr) + max(len(h) for h in hit_list), np.float32)
    for h, tt in zip(hit_list, times):
        a = int(round(tt * sr)); x[a:a + len(h)] += h
    return x[:int((times[-1] + tail) * sr)], times

KICKS6 = [(f0, fc) for f0 in (45, 55, 70) for fc in (3000, 6000)]
def kick6_track(f0, fc, n=10): return hit_track([kick6(f0, fc) for _ in range(n)])
def oct_err(a, b): return float(np.log2(a / b))

def comp_db(y, x, comp, t0, t1, lo, hi, sr=SR):
    """What happened to one known component of the input, in lo..hi Hz over [t0, t1] s (negative = it lost energy). The processor is a time-varying filter, hence
    linear for a given gain schedule: the processed component is the component plus what the processor changed in the whole signal (y - x). This keeps the sub's
    Hann lobe (a 50 ms window cannot separate a 45 Hz sub from a body at 112 Hz) out of the body band. Hann window, zero-padded FFT."""
    a, b = int(t0 * sr), int(t1 * sr); w = np.hanning(b - a); f = np.fft.rfftfreq(1 << 16, 1 / sr); m = (f >= lo) & (f <= hi)
    e = lambda s: np.sum(np.abs(np.fft.rfft(np.asarray(s[a:b], np.float64) * w, 1 << 16))[m] ** 2)
    return float(10 * np.log10((e(np.asarray(comp, np.float64) + (np.asarray(y, np.float64) - np.asarray(x, np.float64))) + 1e-30) / (e(comp) + 1e-30)))
def pband_db(y, x, t0, t1, lo, hi, sr=SR):
    """Energy in lo..hi Hz of y relative to x over [t0, t1] s (negative = y has less). Hann window, zero-padded FFT (a narrow band between two FFT bins still counts)."""
    a, b = int(t0 * sr), int(t1 * sr); w = np.hanning(b - a); f = np.fft.rfftfreq(1 << 16, 1 / sr); m = (f >= lo) & (f <= hi)
    e = lambda s: np.sum(np.abs(np.fft.rfft(np.asarray(s[a:b], np.float64) * w, 1 << 16))[m] ** 2)
    return float(10 * np.log10((e(y) + 1e-30) / (e(x) + 1e-30)))
def click_db(y, x, t, fc, sr=SR):
    """The click band (fc +-1/3 oct) in the first 5 ms after t, y relative to x, dB. P1i: both signals go through a zero-phase band-pass (4th order, forward-backward) first.
    The SUB cut is a Linkwitz-Riley split now: it changes the phase of the sub inside the 5 ms window, and a rectangular 5 ms window leaks that sub into the click band
    (the P1h measure read -5.2 dB for a CLICK cut that is -7.8 dB in the band)."""
    sos = butter(4, [fc / 2 ** (1 / 3), fc * 2 ** (1 / 3)], "band", fs=sr, output="sos"); a0, b0 = int((t - 0.05) * sr), int((t + 0.1) * sr)
    f = lambda s: sosfiltfilt(sos, np.asarray(s[a0:b0], np.float64)); a = int(0.05 * sr); b = a + int(0.005 * sr)
    return float(10 * np.log10((np.sum(f(y)[a:b] ** 2) + 1e-30) / (np.sum(f(x)[a:b] ** 2) + 1e-30)))

def bandzp_db(y, x, t0, t1, lo, hi, sr=SR):
    """The energy of y relative to x in lo..hi Hz over [t0, t1] s, both through a zero-phase band-pass (4th order, forward-backward). P1i: an edge cut is a Linkwitz-Riley split whose all-pass sum
    delays what is below its crossover by a fraction of a millisecond (energy kept); an FFT band energy of the difference y - x reads that delay as a level change, this does not."""
    sos = butter(4, [lo, hi], "band", fs=sr, output="sos"); a0, b0 = max(0, int((t0 - 0.1) * sr)), int((t1 + 0.1) * sr)
    f = lambda z: sosfiltfilt(sos, np.asarray(z[a0:b0], np.float64)); a = int(t0 * sr) - a0; b = a + int((t1 - t0) * sr)
    return float(10 * np.log10((np.sum(f(y)[a:b] ** 2) + 1e-30) / (np.sum(f(x)[a:b] ** 2) + 1e-30)))
def learned_row(L, i=-1): return {k: float(L[k][i]) for k in L.dtype.names}

def T2():
    rows = []; ok = True
    for f0, fc in KICKS6:
        x, times = kick6_track(f0, fc); run(x, None, want_log=False, target="inst=kick,part=body,amount=9", learn=True); r = learned_row(LAST_LEARN)
        eF, eB, eC = (r["f0"] / f0 - 1) * 100, oct_err(r["p1_c"], BODY_X * f0), oct_err(r["p2_c"], fc)
        good = r["ready"] == 1 and abs(eF) <= 3 and abs(eB) <= 1 / 6 and abs(eC) <= 1 / 6 and r["p1_ok"] == 1 and r["p2_ok"] == 1; ok = ok and good
        rows.append(f"{f0} Hz kick / click {fc}: fundamental {r['f0']:.2f} Hz ({eF:+.2f} %), body {r['p1_c']:.1f} Hz [{r['p1_lo']:.0f}-{r['p1_hi']:.0f}] truth {BODY_X * f0:.1f} ({eB:+.3f} oct), click {r['p2_c']:.0f} Hz [{r['p2_lo']:.0f}-{r['p2_hi']:.0f}] truth {fc} ({eC:+.3f} oct), "
                    f"sub [{r['p0_lo']:.0f}-{r['p0_hi']:.0f}], transient end {r['trans_end_ms']:.1f} ms, confidence {r['conf']:.2f}, {int(r['hits'])} hits in memory of {int(r['onsets'])} onsets")
    report("T2", ok, "6 synthetic kicks (10 hits each, 0.5 s apart), learned points after the last hit (want fundamental within 3 %, body and click centre within 1/6 oct = 0.167): " + "; ".join(rows))

def part_run(part, f0, fc, amount=9, n=10):
    parts = [kick6_parts(f0, fc) for _ in range(n)]; x, times = hit_track([sum(p) for p in parts])
    comps = [hit_track([p[i] for p in parts])[0] for i in range(3)]   # sub, body, click tracks, same onsets
    y, _, _ = run(x, None, want_log=False, target=f"inst=kick,part={part},amount={amount}", learn=True); return x, y[:, 0], times, comps
def sub_band(f0): return f0 / 2 ** (1 / 6), f0 * 2 ** (1 / 6)
def body_band(f0): return BODY_X * f0 / 2 ** (1 / 6), BODY_X * f0 * 2 ** (1 / 6)

def T3():
    res = []; ok = True
    for f0, fc in KICKS6:
        x, y, times, (csub, cbody, cclick) = part_run("body", f0, fc); bd, sb, pk = [], [], []
        for t in times[4:]:   # hits 5..10: the memory has been ready since the third hit's analysis
            bd.append(comp_db(y, x, cbody, t + 0.010, t + 0.060, *body_band(f0))); sb.append(comp_db(y, x, csub, t + 0.010, t + 0.060, *sub_band(f0))); pk.append(abs(peak_db(y, x, t, t + 0.005)))
        good = max(bd) <= -6 and max(abs(v) for v in sb) <= 1 and max(pk) <= 2.5; ok = ok and good
        res.append(f"{f0}/{fc}: body band {max(bd):.1f} dB (worst of {len(bd)} hits), sub {min(sb):+.2f}..{max(sb):+.2f} dB, 0-5 ms peak {max(pk):.2f} dB")
    report("T3", ok, "KICK + BODY, AMOUNT 9, hits 5-10 of 10: the true body band (3 f0 +-1/6 oct) 10-60 ms after the onset (want <= -6 dB); the sub band (f0 +-1/6 oct) within +-1 dB; transient peak 0-5 ms within 2.5 dB (P1j, SPEC 8f: the BODY bell is static, there is no transient guard any more; the peak moves by the bell's phase on the click, was 0.5 dB with the guard). Component energies by comp_db (the input's known component + what the processor changed). " + "; ".join(res))

def T3b():
    res = []; ok = True
    for f0, fc in KICKS6:
        x, y, times, (csub, cbody, cclick) = part_run("sub", f0, fc); sb, ck = [], []
        for t in times[4:]: sb.append(comp_db(y, x, csub, t + 0.010, t + 0.060, *sub_band(f0))); ck.append(abs(click_db(y, x, t, fc)))
        good = max(sb) <= -6 and max(ck) <= 0.5; ok = ok and good
        res.append(f"{f0}/{fc}: sub {max(sb):.1f} dB, click band {max(ck):.2f} dB")
    report("T3b", ok, "KICK + SUB, AMOUNT 9, hits 5-10: sub band 10-60 ms (want <= -6 dB); click band (fc +-1/3 oct, 0-5 ms) within 0.5 dB. " + "; ".join(res))

def T3c():
    res = []; ok = True
    for f0, fc in KICKS6:
        x, y, times, (csub, cbody, cclick) = part_run("click", f0, fc); ck, bd, sb = [], [], []
        for t in times[4:]:
            ck.append(click_db(y, x, t, fc)); bd.append(abs(bandzp_db(y, x, t + 0.010, t + 0.060, *body_band(f0)))); sb.append(abs(comp_db(y, x, csub, t + 0.010, t + 0.060, *sub_band(f0))))
        good = max(ck) <= -6 and max(bd) <= 0.5 and max(sb) <= 0.5; ok = ok and good
        res.append(f"{f0}/{fc}: click band {max(ck):.1f} dB, body {max(bd):.2f} dB, sub {max(sb):.2f} dB")
    report("T3c", ok, "KICK + CLICK, AMOUNT 9, hits 5-10: click band (fc +-1/3 oct) inside 0-5 ms (want <= -6 dB); body and sub 10-60 ms within 0.5 dB. The CLICK cut is armed between hits and released after the transient (the 1 dB/ms limit cannot reach 6 dB inside 5 ms from the onset). " + "; ".join(res))

def snare6(rng, secs=0.9, sr=SR, body=200.0):
    """Synthetic snare with its parts at known places: BODY a 200 Hz ring (tau 100 ms) + its 2nd partial, CRACK noise 800-3500 Hz (tau 25 ms), SNAP/AIR noise 4-10 kHz (tau 8 ms)."""
    t = np.arange(int(secs * sr)) / sr
    nz = lambda lo, hi: sosfilt(butter(4, [lo, hi], "band", fs=sr, output="sos"), rng.standard_normal(len(t)))
    ring = 0.30 * np.sin(2 * np.pi * body * t) * np.exp(-t / 0.10) + 0.10 * np.sin(2 * np.pi * 2 * body * t) * np.exp(-t / 0.07)
    crack = 0.9 * nz(800, 3500) * np.exp(-t / 0.025); snap = 1.3 * nz(4000, 10000) * np.exp(-t / 0.008)
    return (ring + crack + snap).astype(np.float32)

def reverb_ir(secs=1.5, seed=7, sr=SR):
    t = np.arange(int(secs * sr)) / sr; r = np.random.default_rng(seed).standard_normal(len(t)) * np.exp(-t * 6.9078 / secs)   # -60 dB at 1.5 s
    return (r / np.sqrt(np.sum(r ** 2))).astype(np.float64)

def T4():
    rng = np.random.default_rng(8); x, times = hit_track([snare6(rng) for _ in range(7)], spacing=1.0, lead=0.5, tail=2.2)
    ir = reverb_ir(); wet = lambda s: fftconvolve(np.asarray(s, np.float64), ir)[:len(x)]; wx = wet(x); t = times[-1]; a, b = t + 0.1, t + 1.6
    res = {}
    for amount in (60, 9):   # P1i: the corners are LR splits with the cut band at -AMOUNT; AMOUNT max (60) is the real band-limit of P1h, AMOUNT 9 is a gentle shelf cut
        y, _, _ = run(x, None, want_log=False, target=f"inst=snare,part=body,amount={amount},width=1,reverb=1", learn=True); y = y[:, 0]; r = learned_row(LAST_LEARN)
        lo_b, hi_b, hi_crack = r["p0_lo"], r["p0_hi"], r["p1_hi"]
        below = (30.0, 0.6 * lo_b); above = (1.5 * hi_crack, 16000.0)
        res[amount] = dict(r=r, lo_b=lo_b, hi_b=hi_b, hi_crack=hi_crack, below=below, above=above, red_lo=-band_db(wet(y), wx, a, b, *below, hann=False), red_hi=-band_db(wet(y), wx, a, b, *above, hann=False),
                           body=abs(pband_db(y, x, t + 0.010, t + 0.110, lo_b, hi_b)), body200=abs(pband_db(y, x, t + 0.010, t + 0.110, 200 / 2 ** (1 / 6), 200 * 2 ** (1 / 6))))
    q = res[60]; g = res[9]; r = q["r"]
    ok = q["red_lo"] >= 6 and q["red_hi"] >= 3 and q["body"] <= 1 and q["body200"] <= 1
    report("T4", ok, f"SNARE + REVERB-READY (AMOUNT 60 = the band-limit) on 7 synthetic snares 1 s apart, through the synthetic 1.5 s reverb (FFT convolution), last hit's tail 0.1-1.6 s after the onset: learned body [{q['lo_b']:.0f}-{q['hi_b']:.0f}] Hz (truth 200), crack upper edge {q['hi_crack']:.0f} Hz "
           f"-> low corner {r['rev_low']:.0f} Hz, high corner {r['rev_high']:.0f} Hz (LR 24 dB/oct); reverb-tail energy below the body (30 Hz - {q['below'][1]:.0f} Hz) {q['red_lo']:.1f} dB lower (want >= 6); above the crack ({q['above'][0]:.0f} Hz - 16 kHz) {q['red_hi']:.1f} dB lower (want >= 3); "
           f"dry body band (learned [{q['lo_b']:.0f}-{q['hi_b']:.0f}]) {q['body']:.2f} dB, 200 Hz +-1/6 oct {q['body200']:.2f} dB (want <= 1). For information, AMOUNT 9 (a -9 dB shelf each side): below {g['red_lo']:.1f} dB, above {g['red_hi']:.1f} dB, body {g['body']:.2f} dB")

def T7():
    hits = [kick6(45, 3000) for _ in range(8)] + [kick6(70, 3000) for _ in range(8)]; x, times = hit_track(hits, spacing=0.5)
    y, _, _ = run(x, None, want_log=False, target="inst=kick,part=sub,amount=9", learn=True); L = LAST_LEARN; tt = L["t_s"]
    first_new = None; f_at = []
    for k, t in enumerate(times):
        i = int(np.searchsorted(tt, t - 0.001)); f_at.append(float(L["f0"][i]))
        if k >= 8 and first_new is None and abs(L["f0"][i] / 70 - 1) <= 0.03: first_new = k - 8 + 1
    new_hits_before = first_new
    t4 = times[8 + 3]   # the 4th new hit
    cx = L["rule_lo_hz"] / SUB_EDGE_X   # (P1k: the rule's crossover, before the neighbour is protected) the SUB is an edge cut (P1i): the crossover / 1.8 stands for the sub frequency
    m = (tt >= t4 - 0.001) & (L["edge_lo_db"] > 0.5); worst = float(np.max(np.abs(np.log2(cx[m] / 70)))) if m.any() else float("nan")
    old = ((tt >= times[8]) & (tt < t4 - 0.001) & (L["edge_lo_db"] > 0.5)); old_hits = bool(old.any() and np.min(np.abs(np.log2(cx[old] / 45))) < 1 / 6)
    ok = first_new is not None and first_new <= 4 and m.any() and worst <= 1 / 6
    report("T7", ok, f"relearn: 8 kicks at 45 Hz then 8 at 70 Hz, 0.5 s apart, KICK + SUB AMOUNT 9: learned fundamental at the onset of each hit {', '.join(f'{v:.1f}' for v in f_at)} Hz; "
           f"memory first at 70 Hz (+-3 %) at new hit {first_new} (want <= 4); from the 4th new hit on, {int(m.sum())} blocks with a cut > 0.5 dB, the SUB crossover's (/ 1.8) furthest distance from 70 Hz {worst:.3f} oct (want <= 0.167); "
           f"cut on the old 45 Hz during new hits 1-3: {old_hits} (allowed: the memory still holds the old sample)")

def T8():
    out = []; ok = True
    rng = np.random.default_rng(11)
    for name, odd in (("snare bleed (the kick track gets a snare hit)", snare6(rng, 0.7)), ("an 80 Hz kick among 55 Hz kicks", kick6(80, 3000))):
        hits = [kick6(55, 3000) for _ in range(10)] + [odd] + [kick6(55, 3000) for _ in range(2)]; x, times = hit_track(hits, spacing=0.5)
        run(x, None, want_log=False, target="inst=kick,part=body,amount=9", learn=True); L = LAST_LEARN; tt = L["t_s"]
        i0 = int(np.searchsorted(tt, times[10] - 0.001)); i1 = int(np.searchsorted(tt, times[10] + 0.45))
        before, after = learned_row(L, i0), learned_row(L, i1); shifts = {"f0": oct_err(after["f0"], before["f0"]), "body": oct_err(after["p1_c"], before["p1_c"]), "click": oct_err(after["p2_c"], before["p2_c"])}
        learned_odd = after["learned"] - before["learned"]; worst = max(abs(v) for v in shifts.values()); good = worst <= 1 / 12 and before["ready"] == 1; ok = ok and good
        out.append(f"{name}: the odd hit was {'taken into the memory' if learned_odd >= 1 else 'rejected by the analysis (no fundamental in 30-90 Hz)'}; learned points before -> after: fundamental {before['f0']:.2f} -> {after['f0']:.2f} Hz ({shifts['f0']:+.4f} oct), "
                   f"body {before['p1_c']:.1f} -> {after['p1_c']:.1f} Hz ({shifts['body']:+.4f} oct), click {before['p2_c']:.0f} -> {after['p2_c']:.0f} Hz ({shifts['click']:+.4f} oct)")
    report("T8", ok, "a single odd hit, 10 kicks (55 Hz / 3 kHz) before it and 2 after (want every learned point to move <= 1/12 oct = 0.0833): " + "; ".join(out))

def T5():
    # P1g: the P1e check, re-aimed on purpose: the slider is the PART now (0 -> 100 steps SUB, BODY, CLICK) and the memory needs 3 hits before it cuts and again after the instrument
    # switch, so the signal is 8 s (switch at 4 s) to leave cutting time on both sides
    rng = np.random.default_rng(9); ts = [0.25 + 0.5 * k for k in range(15)]; secs = 8.0
    kx = place([kick_hit(rng, 0.4) for _ in ts], ts, secs); sx = place([snare_hit(rng, 0.4) for _ in ts], [t + 0.25 for t in ts], secs); x = (kx + sx) * 0.8
    onsets = ts + [t + 0.25 for t in ts]
    y, cuts, _ = run(x, None, target="inst=kick,part=0,part_end=100,amount=12,width=1", after=(4.0, {}), atarget="inst=snare,part=60,amount=12,width=1,reverb=1"); y = y[:, 0]
    tcut = LAST_TARGET.copy()
    rate = lambda a: float(np.max(np.abs(np.diff(np.asarray(a, np.float64)))))
    jin, jout = rate(x), rate(y)
    mask = np.ones(len(x) - 1, bool)
    for t in onsets: mask[max(0, int((t - 0.002) * SR)):int((t + 0.015) * SR)] = False   # the hits' own attacks are the input's own jumps
    jin2, jout2 = float(np.max(np.abs(np.diff(x.astype(np.float64)))[mask])), float(np.max(np.abs(np.diff(y.astype(np.float64)))[mask]))
    ok = jout <= jin * 10 ** (6 / 20)
    jchg = rate(y.astype(np.float64) - x.astype(np.float64))
    sw = slice(int(3.95 * SR), int(4.1 * SR)); jsw_in, jsw_out = rate(x[sw]), rate(y[sw])
    report("T5", ok, f"PART sweep 0 -> 100 over 8 s with the instrument switched KICK -> SNARE (+ REVERB-READY) at 4 s (kick + snare hits every 0.25 s, AMOUNT 12; TARGET cut > 0.5 dB in {100 * np.mean(tcut > 0.5):.0f} % of the blocks, max {tcut.max():.1f} dB): largest sample jump {jout:.5f} vs the input's own {jin:.5f} = {db(jout) - db(jin):+.2f} dB (want <= +6); "
           f"outside the hits' attacks {jout2:.5f} vs {jin2:.5f} = {db(jout2) - db(jin2):+.2f} dB; around the switch (3.95-4.10 s) {jsw_out:.5f} vs {jsw_in:.5f} = {db(jsw_out) - db(jsw_in):+.2f} dB; "
           f"largest jump of the change the layer made (output - input) {jchg:.5f} = {db(jchg) - db(jin):+.2f} dB re the input's own")

def T6():
    rng = np.random.default_rng(10); secs = 60.0; ts = [0.1 + 0.25 * k for k in range(int(secs / 0.25) - 3)]
    drums = place([kick_hit(rng, 0.25) if k % 2 == 0 else snare_hit(rng, 0.25) for k in range(len(ts))], ts, secs)
    main = (pink(secs, -26, 31) + 0.5 * drums).astype(np.float32); key = pink(secs, -30, 32)
    specs = ["inst=snare,part=body,amount=12,width=1,reverb=1", "inst=drums,part=snarebody,amount=12,width=1", "inst=kick,part=click,amount=12,width=1"]
    best = []
    for sp in specs: best.append(max(run(main, key, depth=12, margin=3, want_log=False, timing=True, target=sp)[2] for _ in range(3)))
    _, cuts, _ = run(main[:10 * SR], key[:10 * SR], depth=12, margin=3, target=specs[0], learn=True)
    report("T6", min(best) >= 55, f"CPU, stereo 48 kHz, 60 s, COLLIDE cutting (pink main -26 + drum hits every 0.25 s, kick and snare alternating / pink key -30) + TARGET: SNARE + REVERB-READY {best[0]:.0f}x, DRUMS (snare body, bus detector) {best[1]:.0f}x, KICK + CLICK {best[2]:.0f}x, each best of 3 (want all >= 55); "
           f"first 10 s, SNARE + REVERB-READY: COLLIDE mean cut {cuts.mean():.2f} dB over all bands / {cuts.max():.2f} dB max, hits learned {int(LAST_LEARN['learned'][-1])} of {int(LAST_LEARN['onsets'][-1])} onsets, band-limit blend {LAST_LEARN['rev_mix'][-1]:.2f}")


# ---- P1h (TASK_P1h, SPEC 8d): the memory belongs to the instance, it always listens, and it never needs to listen again
def sub_cut(y, x, csub, t, f0): return comp_db(y, x, csub, t + 0.010, t + 0.060, *sub_band(f0))   # the sub band 10-60 ms after the onset, same window as T3b
def sub_gap(y, x, csub, t, f0): return comp_db(y, x, csub, t + 0.300, t + 0.450, *sub_band(f0))   # P1j (SPEC 8f): the sub band between two hits, 300-450 ms after the onset (the next one comes at 500 ms)
def kick_set(f0=55, fc=3000, n=12):
    parts = [kick6_parts(f0, fc) for _ in range(n)]; x, times = hit_track([sum(p) for p in parts])
    return x, times, hit_track([p[0] for p in parts])[0], hit_track([p[1] for p in parts])[0], hit_track([p[2] for p in parts])[0]
def memfile(name): return os.path.join(TMP, name)
def mem_floats(path): return np.fromfile(path, dtype="<f4")
def mem_slot_counts(path):
    m = mem_floats(path); per = 3 + 4 * 5; at = 2; out = []   # 3 + 4 x kMaxParts (DyneqEngine.h: 5 parts since 11 Oct; the old 3 + 16 = 4 parts made T14 stop with an IndexError, 2026-10-25)
    for _ in range(int(m[1])):
        n = int(m[at]); out.append(n); at += 1 + n * per
    return out

def T9():
    f0 = 55; x, times, csub, cbody, cclick = kick_set(f0, 3000, 12)
    t_sel = times[6] - 0.2   # six hits with TARGET OFF, the choice between hit 6 and hit 7
    y, _, _ = run(x, None, want_log=False, target="inst=off", extra=["--switch", f"{t_sel}:inst=kick,part=sub,amount=9"]); y = y[:, 0]
    first, fifth = sub_cut(y, x, csub, times[6], f0), sub_cut(y, x, csub, times[10], f0); before = sub_cut(y, x, csub, times[5], f0); gap = sub_gap(y, x, csub, times[9], f0); gap_off = sub_gap(y, x, csub, times[4], f0)
    report("T9", first <= -6 and abs(first - fifth) <= 1 and abs(before) <= 0.01 and gap <= -6 and abs(gap_off) <= 0.01,
           f"6 kicks (55 Hz) with TARGET OFF, then KICK + SUB (AMOUNT 9) selected before hit 7: the sub band 10-60 ms after the onset: hit 6 (OFF) {before:+.2f} dB (want 0), first hit after the selection {first:.2f} dB (want <= -6), "
           f"hit 5 after the selection {fifth:.2f} dB, difference {abs(first - fifth):.2f} dB (want <= 1); P1j (static cut): between hits 300-450 ms after hit 10 {gap:.2f} dB (want <= -6), before the selection (after hit 5, TARGET OFF) {gap_off:+.3f} dB (want 0)")

def T10():
    f0 = 55; x, times, csub, cbody, cclick = kick_set(f0, 3000, 8)
    y, _, _ = run(x, None, want_log=True, target="inst=kick,part=sub,amount=9", learn=True); y = y[:, 0]; tc = LAST_TARGET.copy()
    cuts = [sub_cut(y, x, csub, t, f0) for t in times[:5]]; gap = sub_gap(y, x, csub, times[5], f0)
    step = float(np.max(np.abs(np.diff(tc)))) / (BLOCK / SR * 1000.0)
    mono = all(cuts[i + 1] < cuts[i] for i in range(3)) and abs(cuts[0]) <= 0.1 and cuts[3] <= -6
    report("T10", mono and step <= 1.0 + 1e-3 and gap <= -6, f"empty memory, KICK + SUB (AMOUNT 9) selected from the first sample, 8 kicks: the sub cut on hits 1-5 {', '.join(f'{c:.2f}' for c in cuts)} dB (want hit 1 about 0, hits 1 < 2 < 3 < 4 more and more cut, hit 4 <= -6: "
           f"the ramp is the memory's confidence: 1/3 of the amount after the first hit is learned, 2/3 after the second, full after the third); largest step of the TARGET cut {step:.3f} dB/ms (want <= 1); P1j (static cut): between hits, 300-450 ms after hit 6, the cut {gap:.2f} dB (want <= -6)")

def T11():
    f0 = 55; x, times, csub, cbody, cclick = kick_set(f0, 3000, 20)
    sw = [(times[10] - 0.15, "body"), (times[11] - 0.15, "click"), (times[12] - 0.15, "sub")]
    extra = []
    for t, part in sw: extra += ["--switch", f"{t}:inst=kick,part={part},amount=9"]
    fa, fb_, fc_ = memfile("m11a.bin"), memfile("m11b.bin"), memfile("m11c.bin")
    extra += ["--mem-at", str(sw[0][0] - 0.05), fa, "--mem-at", str(sw[0][0] + 0.05), fb_, "--mem-out", fc_]
    y, _, _ = run(x, None, want_log=False, target="inst=kick,part=sub,amount=9", extra=extra); y = y[:, 0]
    r = []
    body, bsub = comp_db(y, x, cbody, times[10] + 0.010, times[10] + 0.060, *body_band(f0)), comp_db(y, x, csub, times[10] + 0.010, times[10] + 0.060, *sub_band(f0))
    click = click_db(y, x, times[11], 3000); sub = sub_cut(y, x, csub, times[12], f0); sub_ref = sub_cut(y, x, csub, times[9], f0); gap = sub_gap(y, x, csub, times[13], f0); gap_body = comp_db(y, x, cbody, times[10] + 0.200, times[10] + 0.330, *body_band(f0))   # the next switch (to CLICK) comes 0.35 s after hit 11
    same_ab = open(fa, "rb").read() == open(fb_, "rb").read(); same_ac = open(fa, "rb").read() == open(fc_, "rb").read()
    ok = body <= -6 and abs(bsub) <= 1 and click <= -6 and sub <= -6 and abs(sub - sub_ref) <= 1 and same_ab and same_ac and gap <= -6 and gap_body <= -6
    report("T11", ok, f"learned kick (10 hits, KICK + SUB), then SUB -> BODY -> CLICK -> SUB, one switch 0.15 s before each hit: hit 11 on BODY: body band {body:.2f} dB (want <= -6), sub {bsub:+.2f} dB (want within 1); hit 12 on CLICK: click band {click:.2f} dB (want <= -6); "
           f"hit 13 back on SUB: {sub:.2f} dB (want <= -6, within 1 dB of the SUB hit before the switches {sub_ref:.2f}); P1j (static cut): between hits, the body band 200-330 ms after hit 11 (BODY) {gap_body:.2f} dB, the sub band after hit 14 (SUB) {gap:.2f} dB (want <= -6 both); the memory before the first switch, just after it and at the end of the run: bit-identical {same_ab} (before vs just after) / {same_ac} (before vs end)")

def T12():
    f0 = 55; x, times, csub, cbody, cclick = kick_set(f0, 3000, 12)
    fm = memfile("m12.bin")
    y1, _, _ = run(x, None, want_log=False, target="inst=kick,part=sub,amount=9", extra=["--mem-out", fm]); y1 = y1[:, 0]
    ref = sub_cut(y1, x, csub, times[8], f0)
    x2, t2, c2, _, _ = kick_set(f0, 3000, 4)
    y2, _, _ = run(x2, None, want_log=False, target="inst=kick,part=sub,amount=9", extra=["--mem-in", fm]); y2 = y2[:, 0]
    first = sub_cut(y2, x2, c2, t2[0], f0)
    report("T12", first <= -6 and abs(first - ref) <= 0.5, f"state saved after 12 kicks ({os.path.getsize(fm)} bytes), loaded into a fresh instance that is fed 4 kicks: the sub cut on its very first hit {first:.2f} dB (want <= -6), "
           f"the original instance on its hit 9 {ref:.2f} dB, difference {abs(first - ref):.2f} dB (want <= 0.5)")

def T13():
    f0 = 55; x, times, csub, cbody, cclick = kick_set(f0, 3000, 12); fm = memfile("m13.bin")
    run(x, None, want_log=False, target="inst=kick,part=sub,amount=9", extra=["--mem-out", fm])
    rng = np.random.default_rng(13); xs, ts = hit_track([snare6(rng, 0.7) for _ in range(10)], spacing=0.5)
    y, _, _ = run(xs, None, want_log=True, target="inst=kick,part=sub,amount=9", learn=True, extra=["--mem-in", fm]); L = LAST_LEARN; tt = L["t_s"]; tc = LAST_TARGET
    after3 = tt >= ts[2]; late = float(tc[after3].max()); i3 = int(np.searchsorted(tt, ts[2])); hits3 = int(L["hits"][i3]); i_end = int(L["hits"][-1])
    early = float(tc[tt < ts[2]].max()); ctrl = None
    report("T13", late <= 0.1 and hits3 == 0 and i_end == 0, f"the memory of 12 kicks (55 Hz) loaded into an instance that is fed snares (10 hits, 0.5 s apart, KICK + SUB AMOUNT 9): kick hits still in the memory at the 3rd snare's onset {hits3}, at the end {i_end} (want 0, 0); "
           f"largest TARGET cut before the 3rd snare {early:.2f} dB (allowed), from the 3rd snare on {late:.3f} dB (want <= 0.1)")

def T14():
    fm = memfile("m14.bin"); sil = np.zeros(2 * SR, np.float32)
    run(sil, None, want_log=False, target="inst=kick,part=sub,amount=9", extra=["--mem-out", fm]); fresh = mem_slot_counts(fm); nfl = len(mem_floats(fm))
    x, times, *_ = kick_set(55, 3000, 12); run(x, None, want_log=False, target="inst=off", extra=["--mem-out", fm]); learned = mem_slot_counts(fm)
    fm2 = memfile("m14b.bin"); run(sil, None, want_log=False, target="inst=kick,part=sub,amount=9", extra=["--mem-out", fm2]); again = mem_slot_counts(fm2)
    fm3 = memfile("m14c.bin"); run(pink(2.0, -20, 5), None, want_log=False, target="inst=kick,part=sub,amount=9", extra=["--mem-out", fm3]); noise = mem_slot_counts(fm3)
    ok = sum(fresh) == 0 and nfl == 2 + 7 and sum(again) == 0 and learned[0] == 8
    report("T14", ok, f"hits stored per detector slot (kick, snare, hats, bass, vocal, kick-in-bus, snare-in-bus): a fresh instance after 2 s of silence {fresh} ({nfl} floats = the header and seven zero counts); after 12 kicks with TARGET OFF {learned} (the listening works with TARGET OFF); "
           f"another fresh instance after 2 s of silence, started after the learning run {again} (nothing carries over between instances); for information, 2 s of pink noise stores {noise} (noise is read as hits by the detectors that accept broadband sound). The plug-in's state and presets are in plugin_check")

# ---- P1i (TASK_P1i, SPEC 8d 7): active listening on a live channel. The sound changes under the same instance; nothing is loaded, nothing is reset
def offline_peak(h, lo, hi, t0=0.040, t1=0.200, sr=SR):
    """The fundamental of one isolated hit, measured offline: the FFT peak (zero padded, Hann) of t0..t1 s between lo and hi Hz."""
    a, b = int(t0 * sr), int(t1 * sr); f = np.fft.rfftfreq(1 << 18, 1 / sr); m = (f >= lo) & (f <= hi)
    sp = np.abs(np.fft.rfft(np.asarray(h[a:b], np.float64) * np.hanning(b - a), 1 << 18)); return float(f[m][np.argmax(sp[m])])
def bell_at(L, t, dt=0.030):
    i = min(int(np.searchsorted(L["t_s"], t + dt)), len(L["t_s"]) - 1); return float(L["bell_hz"][i]), float(L["bell_db"][i])
def sub_at(L, t, dt=0.030):
    """The SUB cut 30 ms after an onset: the sub frequency the crossover stands for (crossover / 1.8) and the cut depth."""
    i = min(int(np.searchsorted(L["t_s"], t + dt)), len(L["t_s"]) - 1); e = float(L["rule_lo_hz"][i]); return (e / SUB_EDGE_X if e > 0 else float("nan")), float(L["edge_lo_db"][i])
def swap_run(hits_a, hits_b, target, f_a, f_b, lo, hi, comp_a, comp_b, at=None):
    """hits_*: arrays; comp_*: the isolated component whose cut is measured (sub of the kick, body of the snare). Returns the table of every hit."""
    hl = list(hits_a) + list(hits_b); x, times = hit_track(hl, spacing=0.5)
    comps = hit_track(list(comp_a) + list(comp_b), spacing=0.5)[0]
    y, _, _ = run(x, None, want_log=True, target=target, learn=True); L = LAST_LEARN; tc = LAST_TARGET
    rows = []
    for k, t in enumerate(times):
        hz, g = (at or bell_at)(L, t); f = f_a if k < len(hits_a) else f_b
        rows.append(dict(k=k, t=t, hz=hz, g=g, f=f, oct=float(np.log2(hz / f)) if hz > 0 else float("nan"), cut=comp_db(y[:, 0], x, comps, t + 0.010, t + 0.060, f / 2 ** (1 / 6), f * 2 ** (1 / 6)), gap=comp_db(y[:, 0], x, comps, t + 0.300, t + 0.450, f / 2 ** (1 / 6), f * 2 ** (1 / 6))))
    step = float(np.max(np.abs(np.diff(tc)))) / (BLOCK / SR * 1000.0)
    return rows, step, int(L["relearns"][-1]), L, y, x, times
def swap_report(tag, what, rows, step, relearns, n_a, f_a, f_b, nA_skip=3, gaps=False):
    b = rows[n_a:]; ok_pos = all(abs(r["oct"]) <= 1 / 12 for r in b[2:])
    gap = [r["k"] - n_a + 1 for r in rows[nA_skip:] if r["g"] < 3.0]
    gap_b = max(r["gap"] for r in b[2:]); ok = ok_pos and not gap and step <= 1.0 + 1e-3 and (not gaps or gap_b <= -6)   # P1j (SPEC 8f): the cut is on between the hits too
    tbl = "; ".join(f"B{r['k'] - n_a + 1}: {r['hz']:.1f} Hz ({r['oct']:+.3f} oct) {r['g']:.1f} dB, {what} cut {r['cut']:.1f} dB" for r in b[:6])
    report(tag, ok, f"{what}: 16 hits A ({f_a:.1f} Hz measured offline) then 16 hits B ({f_b:.1f} Hz), same instance, no state load, selected throughout, relearn events {relearns}. Cut at 30 ms after each B onset (the SUB crossover / 1.8 for an edge cut, the bell for a bell): {tbl}; "
           f"from B hit 3 on the bell is within 1/12 oct of B: worst {max(abs(r['oct']) for r in b[2:]):.3f} oct (want <= 0.083); hits from A hit 4 to the end with a bell cut under 3 dB: {gap if gap else 'none'} (want none); largest TARGET gain step {step:.3f} dB/ms (want <= 1)" + (f"; P1j (static cut): between hits (300-450 ms after the onset) of B hits 3-16 the cut is at most {gap_b:.2f} dB at its weakest (want <= -6)" if gaps else ""))
def T15():
    n = 16; ka = [kick6_parts(50, 3000) for _ in range(n)]; kb = [kick6_parts(70, 3000, bodyx=2.82) for _ in range(n)]   # body 137 Hz -> 197 Hz = +0.52 oct
    f_a = offline_peak(ka[0][0], 30, 90); f_b = offline_peak(kb[0][0], 30, 120)
    rows, step, rel, *_ = swap_run([sum(p) for p in ka], [sum(p) for p in kb], "inst=kick,part=sub,amount=9", f_a, f_b, 30, 120, [p[0] for p in ka], [p[0] for p in kb], at=sub_at)
    swap_report("T15", "KICK + SUB", rows, step, rel, n, f_a, f_b, gaps=True)
def T18():
    n = 16; rng = np.random.default_rng(18); sa = [snare6(rng, 0.9, body=200.0) for _ in range(n)]; sb = [snare6(rng, 0.9, body=260.0) for _ in range(n)]
    f_a = offline_peak(sa[0], 150, 300, 0.040, 0.200); f_b = offline_peak(sb[0], 200, 400, 0.040, 0.200)
    ring = lambda f: np.zeros(1, np.float32)
    t = np.arange(int(0.9 * SR)) / SR; comp = lambda f: (0.30 * np.sin(2 * np.pi * f * t) * np.exp(-t / 0.10)).astype(np.float32)
    rows, step, rel, *_ = swap_run(sa, sb, "inst=snare,part=body,amount=9", f_a, f_b, 150, 400, [comp(200.0)] * n, [comp(260.0)] * n)
    swap_report("T18", "SNARE + BODY", rows, step, rel, n, f_a, f_b)
def T16():
    n = 32; fs = [45 + 15 * k / (n - 1) for k in range(n)]; hits = [kick6(f, 3000) for f in fs]; x, times = hit_track(hits, spacing=0.5)
    y, _, _ = run(x, None, want_log=True, target="inst=kick,part=sub,amount=9", learn=True); L = LAST_LEARN
    row = [(k,) + sub_at(L, t) + (fs[k],) for k, t in enumerate(times)]
    dev = [(k + 1, float(np.log2(hz / f))) for k, hz, g, f in row if k >= 3]; worst = max(abs(d) for _, d in dev); rel = int(L["relearns"][-1])
    gaps = [k + 1 for k, hz, g, f in row if k >= 3 and g < 3.0]
    report("T16", worst <= 1 / 12 and rel == 0 and not gaps, f"32 kicks, the fundamental glides 45 -> 60 Hz linearly (one value per hit), KICK + SUB, no state load: the bell at 30 ms after each onset vs that hit's fundamental, hits 4-32: worst {worst:.3f} oct (want <= 0.083), "
           f"signed at hits 4 / 8 / 16 / 24 / 32: {', '.join(f'{d:+.3f}' for h, d in dev if h in (4, 8, 16, 24, 32))}; hits 4-32 with a bell cut under 3 dB: {gaps if gaps else 'none'}; relearn events {rel} (want 0)")
def T17():
    rng = np.random.default_rng(17); n = 32; f0 = 55.0; vel = rng.uniform(-12, 12, n)
    hits = []; times = []
    t = 0.5; ghosts = {6, 13, 20, 27}; flam = 22; crash = {15, 16}
    layers = []   # (onset time, array)
    for k in range(n):
        layers.append((t, kick6(f0, 3000) * amp(vel[k]))); hit_t = t
        if k in ghosts: layers.append((t + 0.27, kick6(f0, 3000) * amp(-20)))
        if k == flam: layers.append((t + 0.030, kick6(f0, 3000) * amp(vel[k] - 3)))
        t += 0.5
    secs = t + 0.8; x = np.zeros(int(secs * SR), np.float32)
    for tt, h in layers:
        a = int(round(tt * SR)); x[a:a + len(h)] += h[:len(x) - a]
    nz = np.random.default_rng(170).standard_normal(len(x)); cr = sosfilt(butter(4, 6000, "high", fs=SR, output="sos"), nz)
    for k in crash:
        a = int(round((0.5 + 0.5 * k - 0.005) * SR)); m = int(0.9 * SR); x[a:a + m] += (0.25 * cr[a:a + m] * np.exp(-np.arange(m) / (0.25 * SR))).astype(np.float32)
    y, _, _ = run(x.astype(np.float32), None, want_log=True, target="inst=kick,part=sub,amount=9", learn=True); L = LAST_LEARN; tt = L["t_s"]
    rel = int(L["relearns"][-1]); m = (tt >= 0.5 + 0.5 * 4) & (L["edge_lo_db"] > 0.5); hz = L["rule_lo_hz"][m] / SUB_EDGE_X
    span = float(np.log2(hz.max() / hz.min())) if m.any() else float("nan"); worst = float(np.max(np.abs(np.log2(hz / np.median(hz))))) if m.any() else float("nan")
    report("T17", rel == 0 and span <= 1 / 12, f"32 kicks (55 Hz) with velocity uniform within +-12 dB, 4 ghost notes at -20 dB, one flam (30 ms apart), 2 hits under a crash cymbal (6 kHz high-passed noise), KICK + SUB: relearn events {rel} (want 0); "
           f"from hit 5 on the bell's frequency while cutting ({int(m.sum())} blocks) spans {span:.3f} oct (min {hz.min():.1f}, max {hz.max():.1f} Hz; want <= 0.083), furthest from its median {worst:.3f} oct; "
           f"hits learned {int(L['learned'][-1])} of {int(L['onsets'][-1])} onsets")


# ---- P1i (TASK_P1i, SPEC 8e.2): SLOPE. The edge cuts are Linkwitz-Riley splits; REVERB-READY has two of them (low and high corner), the others one
from scipy.signal import sosfreqz
SLOPES = (12, 24, 36, 48, 72)
def lr_h(f, fc, slope, cut_db, below, sr=SR):
    """The exact response of one LR split (Butterworth n squared, scipy's bilinear design = the engine's) with the cut band at -cut_db and the other band at 0 dB."""
    n = {12: 1, 24: 2, 36: 3, 48: 4, 72: 6}[slope]; sgn = -1.0 if slope in (12, 36) else 1.0; w = 2 * np.pi * np.asarray(f, float) / sr
    lp = butter(n, fc, "low", fs=sr, output="sos"); hp = butter(n, fc, "high", fs=sr, output="sos")
    _, hl = sosfreqz(np.vstack([lp, lp]), worN=w); _, hh = sosfreqz(np.vstack([hp, hp]), worN=w); g = 10 ** (-cut_db / 20)
    return g * hl + sgn * hh if below else hl + sgn * g * hh
def multisine(n=1 << 17, per_oct=12, level_db=-45.0, seed=22, sr=SR):
    """Equal-amplitude sines on every 1/12 octave from 20 Hz to 20 kHz, each on an exact FFT bin of an n-sample period: the steady output of a linear system is one period of it, so the
    response at every sine is read without leakage (down to the float noise, about -100 dB)."""
    rng = np.random.default_rng(seed); fr = 20.0 * 2 ** (np.arange(0, int(np.log2(1000) * per_oct) + 1) / per_oct); ks = np.unique(np.round(fr * n / sr).astype(int)); t = np.arange(n)
    x = np.zeros(n)
    for k, ph in zip(ks, rng.uniform(0, 2 * np.pi, len(ks))): x += np.cos(2 * np.pi * k * t / n + ph)
    return (x * amp(level_db)).astype(np.float32), ks
PROBE_PERIODS = 6
def edge_response(slope, amount, edge_in=False, kind="reverb"):
    """kind "reverb": 7 synthetic snares, SNARE + REVERB-READY (two corners). kind "click": 10 synthetic kicks (55 Hz, click 3000 Hz), KICK + CLICK (one crossover, the cut band above it).
    The memory is frozen after the last analysis, then 6 periods of the multisine go through the same instance. Returns the response (dB) on the sines, their frequencies and the crossovers
    the engine used (low, high; 0 = none)."""
    if kind == "reverb":
        rng = np.random.default_rng(8); hits, times = hit_track([snare6(rng) for _ in range(7)], spacing=1.0, lead=0.5, tail=0.6); tgt = f"inst=snare,part=body,amount={amount},width=1,reverb=1,slope={slope}"
    else:
        hits, times = hit_track([kick6(55, 3000) for _ in range(10)], spacing=0.5); tgt = f"inst=kick,part=click,amount={amount},slope={slope}"
    t_freeze = times[-1] + 0.35   # the track's own 0.6 s tail is longer than the analysis of the last hit needs
    probe, ks = multisine(); x = np.concatenate([hits, np.tile(probe, PROBE_PERIODS)]).astype(np.float32); n = len(probe); p0 = len(hits)
    y, _, _ = run(x, None, want_log=False, target=tgt, learn=True, extra=["--listen-until", f"{t_freeze:.3f}"] + (["--edge-in"] if edge_in else []))
    L = LAST_LEARN; lo, hi = (float(L["rev_low"][-1]), float(L["rev_high"][-1])) if kind == "reverb" else (0.0, float(L["edge_hi_hz"][-1])); a = p0 + (PROBE_PERIODS - 2) * n
    X = np.fft.rfft(x[a:a + n].astype(np.float64)); Y = np.fft.rfft(y[a:a + n, 0].astype(np.float64)); f = ks * SR / n
    return f, 20 * np.log10(np.maximum(np.abs(Y[ks]) / np.abs(X[ks]), 1e-12)), lo, hi
def local_slopes(f, h, fe, below, span_oct=3.0, floor_db=-80.0, step=4):
    """Local slope (dB per octave, positive = more attenuation away from the edge) over `step` grid steps (1/3 octave), for the points between the edge and span_oct octaves past it
    whose both ends are above floor_db. The grid is 12 points per octave. The octaves are counted on the pre-warped frequency (fs / pi) tan (pi f / fs): a bilinear (digital)
    filter is the analog one on that axis, and its slope per real octave grows towards Nyquist (an analog 24 dB/oct low-pass reads 31 dB/oct at 0.2 fs), which is the sample rate's doing."""
    f = np.asarray(f, float); fw = (SR / np.pi) * np.tan(np.pi * f / SR); few = (SR / np.pi) * np.tan(np.pi * fe / SR); out = []
    for i in range(len(f)):
        j = i - step if below else i + step
        if not (0 <= j < len(f)): continue
        if below and not (fw[i] <= few and fw[i] >= few / 2 ** span_oct): continue
        if (not below) and not (fw[i] >= few and fw[i] <= few * 2 ** span_oct): continue
        if h[i] >= floor_db and h[j] >= floor_db: out.append((h[i] - h[j]) / abs(np.log2(fw[i] / fw[j])))
    return out
def T22():
    rows = []; ok = True
    for sl in SLOPES:
        f, h, lo, hi = edge_response(sl, 60)                       # SNARE + REVERB-READY: the low corner is judged here, the high corner is shown
        fk, hk, _, ek = edge_response(sl, 60, kind="click")        # KICK + CLICK: the high crossover, away from Nyquist's warping, is judged here
        nom = 20 * np.log10(np.maximum(np.abs(lr_h(f, lo, sl, 400, True) * lr_h(f, hi, sl, 400, False)), 1e-12)); nomk = 20 * np.log10(np.maximum(np.abs(lr_h(fk, ek, sl, 400, False)), 1e-12))
        s_lo = max(local_slopes(f, h, lo, True)); s_hi_r = max(local_slopes(f, h, hi, False)); s_hik = max(local_slopes(fk, hk, ek, False)); d_lo = max(local_slopes(f, nom, lo, True)); d_hik = max(local_slopes(fk, nomk, ek, False))
        dev = float(max(np.max(np.abs(h - nom)[nom > -70]), np.max(np.abs(hk - nomk)[nomk > -70]))); cl = lambda v: max(float(v), -90.0); h20, n20, h20k, n20k = cl(h[0]), cl(nom[0]), cl(hk[-1]), cl(nomk[-1])   # below -90 dB the float32 output's own noise floor is read, not the filter
        deep = [(hh, nn) for hh, nn in ((h20, n20), (h20k, n20k)) if nn <= -66]
        good = abs(s_lo - sl) <= 2 and abs(s_hik - sl) <= 2 and dev <= 1.0 and abs(h20 - n20) <= 1.5 and abs(h20k - n20k) <= 1.5 and all(hh <= -60 for hh, nn in deep); ok = ok and good
        rows.append(f"{sl} dB/oct: low corner {lo:.0f} Hz (REVERB-READY) {s_lo:.1f} (design {d_lo:.1f}); CLICK crossover {ek:.0f} Hz {s_hik:.1f} (design {d_hik:.1f}); REVERB-READY high corner {hi:.0f} Hz {s_hi_r:.1f} (shown only); "
                    f"largest deviation from the exact LR response {dev:.2f} dB; 20 Hz {h20:.1f} dB (design {n20:.1f}), 20 kHz {h20k:.1f} dB (design {n20k:.1f})")
    report("T22", ok, "SLOPE 12 / 24 / 36 / 48 / 72, AMOUNT max (60), memory frozen after the learning, response read from a multi-sine (a sine per 1/12 octave, 20 Hz - 20 kHz, one exact period). "
           "Slope = the steepest 1/3-octave local slope between a crossover and 3 octaves past it, over the points above -80 dB, pre-warped octaves (want within +-2 dB/oct of the setting; a Linkwitz-Riley corner is -6 dB and reaches its slope some way past it, so the first octave alone reads lower). "
           "The high side is judged on the KICK + CLICK crossover (the REVERB-READY high corner is shown for information). "
           "AMOUNT max removes the cut band (a real high-pass / low-pass): the response equals the exact LR response (want within 1.5 dB of it at 20 Hz and 20 kHz, <= -60 dB wherever the exact response is <= -66 dB, largest deviation <= 1 dB over the points above -70 dB). " + "; ".join(rows))
def T23():
    rows = []; ok = True
    for sl in SLOPES:
        f, h, lo, hi = edge_response(sl, 0, edge_in=True); worst = float(np.max(np.abs(h))); at = float(f[int(np.argmax(np.abs(h)))])
        good = worst <= 0.1; ok = ok and good; rows.append(f"{sl}: worst {worst:.4f} dB at {at:.0f} Hz")
    # the product at AMOUNT 0 (no measurement hook): the stage is out of the path, the output is the input
    rng = np.random.default_rng(8); hits, times = hit_track([snare6(rng) for _ in range(7)], spacing=1.0, lead=0.5, tail=0.6)
    y0, _, _ = run(hits, None, want_log=False, target="inst=snare,part=body,amount=0,width=1,reverb=1,slope=72"); same0 = bool(np.array_equal(y0[:, 0], hits))
    y1, _, _ = run(hits, None, want_log=False, target="inst=off,part=50,amount=60,width=1,reverb=1,slope=72"); ref, _, _ = run(hits, None, want_log=False); same1 = bool(np.array_equal(y1, ref))
    report("T23", ok and same0 and same1, "AMOUNT 0, SNARE + REVERB-READY with the split forced into the path (dyneqraw --edge-in, measurement only): |output / input| over the 20 Hz - 20 kHz multi-sine, every slope (want <= 0.1 dB): " + "; ".join(rows) +
           f". The product with AMOUNT 0 (no hook): output bit-identical to the input = {same0}; TARGET OFF with SLOPE 72 and AMOUNT 60: bit-identical to a run without TARGET = {same1}")
def T24():
    n = 40; fs_ = [45 + 25 * k / (n - 1) for k in range(n)]; att = lambda h: h * np.where(np.arange(len(h)) < 0.008 * SR, 0.5 * (1 - np.cos(np.pi * np.arange(len(h)) / (0.008 * SR))), 1.0)   # P1j: an 8 ms raised-cosine attack, so the input has neither a step nor a kink (a step's HF is changed by the static split's all-pass phase, which is not an artifact)
    hits = [att(kick6_parts(f, 3000)[0]) for f in fs_]; x, times = hit_track(hits, spacing=0.5)   # the sub alone: everything above 1 kHz in the DELTA is an artifact
    seq = [12, 24, 36, 48, 72, 48, 36, 24, 12, 72, 36, 12, 72, 24, 48, 72, 12, 36]; sw = []
    for j, sl in enumerate(seq): sw += ["--switch", f"{times[4 + j] + 0.1:.3f}:inst=kick,part=sub,amount=9,slope={sl}"]   # 0.1 s into a hit: the cut is in
    y, _, _ = run(x, None, want_log=True, target="inst=kick,part=sub,amount=9,slope=24", learn=True, extra=sw); tc = LAST_TARGET; L = LAST_LEARN
    ref, _, _ = run(x, None, want_log=False, target="inst=kick,part=sub,amount=9,slope=24")
    hpf = lambda d: sosfilt(butter(4, 1000, "high", fs=SR, output="sos"), np.asarray(d, np.float64))
    d = x.astype(np.float64) - y[:, 0]; dr = x.astype(np.float64) - ref[:, 0]; pk = float(db(np.max(np.abs(hpf(d))[SR // 4:]))); pkr = float(db(np.max(np.abs(hpf(dr))[SR // 4:])))
    finite = bool(np.all(np.isfinite(y))); step = float(np.max(np.abs(np.diff(tc)))) / (BLOCK / SR * 1000.0); edge = L["edge_lo_hz"][L["edge_lo_db"] > 0.5]
    report("T24", finite and pk <= -60, f"{n} sub-only kicks gliding 45 -> 70 Hz (the learned edge follows: crossover {edge.min():.0f} -> {edge.max():.0f} Hz), KICK + SUB AMOUNT 9, SLOPE switched {len(seq)} times (12 / 24 / 36 / 48 / 72 in turn, each 0.1 s into a hit so the cut is in): "
           f"NaN / inf in the output: {not finite} (want none); DELTA (input - output) high-passed at 1 kHz (4th order) peak {pk:.1f} dBFS (want <= -60; the same material with SLOPE fixed at 24: {pkr:.1f} dBFS); largest TARGET gain step {step:.3f} dB/ms (want <= 1)")


# ---- P1i (TASK_P1i, SPEC 8e.3): the evidence collector gives the same answers as the continuous analysis of P1h
BASELINE_RAW = os.environ.get("DYNEQRAW_P1H", os.path.join(HERE, "baseline", "dyneqraw_p1h"))   # the P1h engine (plus the quick-swap memory rule of the stopped P1i run), kept as tools/baseline/
def learned_at(raw, x, target, times, key=None):
    """Run `raw` on x and return the learn log and the log rows 1 ms before each of `times` (the state the next hit will be cut with)."""
    global RAW
    old = RAW; RAW = raw
    try: run(x, key, want_log=False, target=target, learn=True)
    finally: RAW = old
    L = LAST_LEARN; return L, [min(int(np.searchsorted(L["t_s"], t - 0.001)), len(L["t_s"]) - 1) for t in times]
LCOLS = ["f0"] + [f"p{j}_{w}" for j in range(4) for w in ("c", "lo", "hi")]
def learned_diff(x, target, times, key=None):
    """Largest distance in octaves between the learned fundamental / part centres and edges of the new engine and of the P1h engine on the same input, per column, over the rows 1 ms before every time
    in `times`; how often only one of them was ready / had the part; and how far the P1h engine's own reading wanders over the run (max / min once the part exists, octaves)."""
    Ln, rn = learned_at(RAW, x, target, times, key); Lo, ro = learned_at(BASELINE_RAW, x, target, times, key)
    worst = {}; only = 0; n = 0; wander = {}
    for name in LCOLS[1:]:
        ok = Lo[name[:3] + "ok"] > 0.5; v = Lo[name][ok]
        if len(v) and v.min() > 0: wander[name] = float(np.log2(v.max() / v.min()))
    for a, b in zip(rn, ro):
        for name in LCOLS:
            vn, vo = float(Ln[name][a]), float(Lo[name][b])
            if name != "f0":
                ok_name = name[:3] + "ok"
                if (Ln[ok_name][a] > 0.5) != (Lo[ok_name][b] > 0.5): only += 1; continue
                if Ln[ok_name][a] < 0.5: continue
            elif (vn > 0) != (vo > 0): only += 1; continue
            if vn > 0 and vo > 0: n += 1; worst[name] = max(worst.get(name, 0.0), abs(float(np.log2(vn / vo))))
    return worst, only, n, wander
def T21():
    """Gated: the readings a cut stands on and a test judges: the fundamental, the sub, the body of the kick and of the snare, the click of the synthetic kick of T2, the edges the SUB, CLICK and
    REVERB-READY crossovers are built from. Shown, not gated: the readings that are the peak of a 5 ms white-noise burst or of a stretch of pink noise (the click of the noise kick of T5 / T6, the crack
    centre and the snap of every synthetic snare, the kick body and the snare ring under pink noise): the position of a peak in such a window moves with the window by sample, and the P1h engine's own
    reading wanders over the run by the amount shown."""
    res = []; ok = True
    def add(name, x, targets, times, gates, key=None):
        nonlocal ok
        for tg, gate in zip(targets, gates):
            w, only, n, wander = learned_diff(x, tg, times, key); g = [c for c in gate if c in w]; wg = max([w[c] for c in g] + [0.0]); ok = ok and wg <= 1 / 12
            shown = ", ".join(f"{c} {v:.2f} (P1h's own wander {wander.get(c, 0.0):.2f})" for c, v in sorted(w.items()) if c not in gate and v > 0.083)
            res.append(f"{name} [{tg.split(',amount')[0].replace('inst=', '')}]: gated {wg:.4f} oct" + (f"; not gated: {shown}" if shown else "") + (f"; {only} reading(s) present in only one" if only else "") + f"; {n} compared")
    kick_all = ["f0", "p0_c", "p0_hi", "p1_c", "p2_c", "p2_lo"]; snare_body = ["f0", "p0_c", "p0_lo", "p0_hi"]; kick_sub_body = ["f0", "p0_c", "p0_hi", "p1_c"]
    for f0, fc in KICKS6:
        x, times = kick6_track(f0, fc); add(f"kick {f0}/{fc}", x, ["inst=kick,part=body,amount=9"], times, [kick_all])
    rng = np.random.default_rng(8); x, times = hit_track([snare6(rng) for _ in range(7)], spacing=1.0, lead=0.5, tail=2.2); add("7 snares", x, ["inst=snare,part=body,amount=9"], times, [snare_body]); add("7 snares (end: the crack's upper edge, the REVERB-READY high corner is built from it)", x, ["inst=snare,part=body,amount=9"], [times[-1] + 2.0], [["p1_hi"]])
    rng = np.random.default_rng(9); ts_ = [0.25 + 0.5 * k for k in range(15)]; kx = place([kick_hit(rng, 0.4) for _ in ts_], ts_, 8.0); sx = place([snare_hit(rng, 0.4) for _ in ts_], [t + 0.25 for t in ts_], 8.0)
    x5 = np.concatenate([((kx + sx) * 0.8).astype(np.float32), np.zeros(int(0.8 * SR), np.float32)])   # 0.25 s apart: read at the end, when both engines have analysed every hit
    add("T5 kick+snare track (end)", x5, ["inst=kick,part=sub", "inst=snare,part=body", "inst=drums,part=kickbody", "inst=drums,part=snarebody", "inst=drums,part=hatsshimmer"], [8.7],
        [kick_sub_body, snare_body, kick_sub_body, snare_body, ["p1_c"]])
    rng = np.random.default_rng(10); ts_ = [0.1 + 0.25 * k for k in range(int(10 / 0.25) - 3)]
    drums = place([kick_hit(rng, 0.25) if k % 2 == 0 else snare_hit(rng, 0.25) for k in range(len(ts_))], ts_, 10.0); main = np.concatenate([(pink(10.0, -26, 31) + 0.5 * drums).astype(np.float32), np.zeros(int(0.8 * SR), np.float32)])
    add("T6 pink + drums (end)", main, ["inst=snare,part=body,amount=12,reverb=1", "inst=drums,part=kickbody", "inst=drums,part=snarebody", "inst=kick,part=click"], [10.7],
        [["f0"], ["f0", "p0_c", "p1_c"], ["f0", "p0_c"], ["f0"]])
    x, times = hit_track([kick6(45, 3000) for _ in range(8)] + [kick6(70, 3000) for _ in range(8)], spacing=0.5); add("T7 45 -> 70 Hz", x, ["inst=kick,part=sub,amount=9"], times, [kick_all])
    n = 16; ka = [kick6_parts(50, 3000) for _ in range(n)]; kb = [kick6_parts(70, 3000, bodyx=2.82) for _ in range(n)]; x, times = hit_track([sum(p) for p in ka + kb], spacing=0.5); add("T15 sample swap", x, ["inst=kick,part=sub,amount=9"], times, [kick_all])
    n = 32; x, times = hit_track([kick6(45 + 15 * k / (n - 1), 3000) for k in range(n)], spacing=0.5); add("T16 glide 45 -> 60", x, ["inst=kick,part=sub,amount=9"], times, [kick_all])
    rng = np.random.default_rng(18); sa = [snare6(rng, 0.9, body=200.0) for _ in range(16)]; sb = [snare6(rng, 0.9, body=260.0) for _ in range(16)]; x, times = hit_track(sa + sb, spacing=0.5); add("T18 snare swap", x, ["inst=snare,part=body,amount=9"], times, [snare_body])
    report("T21", ok, "the learned fundamental and part centres / edges of the evidence collector against the P1h engine (tools/baseline/dyneqraw_p1h) on the same input, read 1 ms before every hit (the 0.25 s-apart materials: at the end, "
           "after both engines have analysed the last hit); gated <= 1/12 oct = 0.0833 for the readings that belong to the material; " + "; ".join(res))


# ---- P1i (TASK_P1i, SPEC 8e.3): what the collector costs
def block_stats(stdout):
    for line in stdout.splitlines():
        if line.startswith("block_us"): return {k: float(v) for k, v in (t.split("=") for t in line.split()[1:])}
    return None
def T19():
    """A9's material, TARGET OFF, the evidence collector on against off (dyneqraw --no-listen), alternating, best run of each."""
    main = pink(60.0, -12, 31); key = pink(60.0, -18, 32); on, off = [], []
    for _ in range(9):   # P1k: 9 alternating pairs; the verdict is the median of the per-pair differences (under load the best-of-N compares two lucky runs and read +2.2 % where the pairs' median was -4.4 %)
        on.append(run(main, key, depth=12, margin=3, want_log=False, timing=True)[2]); off.append(run(main, key, depth=12, margin=3, want_log=False, timing=True, extra=["--no-listen"])[2])
    pair = sorted(100.0 * (b / a - 1.0) for a, b in zip(on, off)); cost = pair[len(pair) // 2]; best = 100.0 * (1.0 / max(on) - 1.0 / max(off)) / (1.0 / max(off))
    report("T19", cost <= 1.0, f"idle cost of the evidence collector: A9's material (stereo 48 kHz, 60 s, 32 bands cutting, pink main -12 / pink key -18, TARGET OFF), 9 alternating pairs (with the collector, then without): "
           f"with {', '.join(f'{v:.1f}' for v in on)}x real time, without {', '.join(f'{v:.1f}' for v in off)}x; per pair (without / with - 1) range {pair[0]:+.1f} .. {pair[-1]:+.1f} %, MEDIAN {cost:+.2f} % (want <= 1; the verdict); "
           f"for information, best-of-9 each {max(on):.1f} / {max(off):.1f}x = {best:+.2f} %")
def T20():
    """P1l: the largest single process() call over 60 s of drum loop, collector on against off (TARGET OFF, so only the collector differs), alternating, at host blocks 32, 64 and 512; the median of the 5 per-run
    maxima of each. ABSOLUTE: with the collector the worst call is at most the worst call without it + 10 % of the block's duration. The old ratio (P1i: 2x) is printed as information."""
    rng = np.random.default_rng(10); secs = 60.0; ts = [0.1 + 0.25 * k for k in range(int(secs / 0.25) - 3)]
    drums = place([kick_hit(rng, 0.25) if k % 2 == 0 else snare_hit(rng, 0.25) for k in range(len(ts))], ts, secs)
    main = (pink(secs, -26, 31) + 0.5 * drums).astype(np.float32); key = pink(secs, -30, 32)
    med = lambda L, k: sorted(d[k] for d in L)[len(L) // 2]
    for blk in (32, 64, 512):
        on, off = [], []
        for _ in range(5):
            run(main, key, depth=12, margin=3, want_log=False, block=blk, extra=["--blocktimes"]); on.append(block_stats(LAST_STDOUT))
            run(main, key, depth=12, margin=3, want_log=False, block=blk, extra=["--blocktimes", "--no-listen"]); off.append(block_stats(LAST_STDOUT))
        mx_on, mx_off = med(on, "max"), med(off, "max"); allow = 0.10 * blk / SR * 1e6
        report("T20", mx_on <= mx_off + allow, f"host block {blk} ({blk / SR * 1e6:.0f} us): largest single call over 60 s of drum loop (pink -26 + a kick or snare every 0.25 s, COLLIDE cutting against a pink key at -30, TARGET OFF), 5 alternating runs each, median of the per-run maximum: "
               f"with the collector {mx_on:.0f} us (runs {', '.join(f'{d['max']:.0f}' for d in on)}), without {mx_off:.0f} us (runs {', '.join(f'{d['max']:.0f}' for d in off)}); with - without = {mx_on - mx_off:+.0f} us (want <= {allow:.0f} us = 10 % of the block); "
               f"for information, the old ratio {mx_on / mx_off:.2f}x (P1i's limit was 2x), mean {med(on, 'mean'):.0f} / {med(off, 'mean'):.0f} us, p99.9 {med(on, 'p999'):.0f} / {med(off, 'p999'):.0f} us (with / without)")
def T20b():
    """P1l: how late the record arrives. The job of a hit is sliced to a work budget per 32-sample tick, so the time from the start of its job (where P1k's first stage ran) to its last slot does not depend on the host block size. Shown at 32 / 64 / 512 (dyneqraw --latency)."""
    rng = np.random.default_rng(10); secs = 60.0; ts = [0.1 + 0.25 * k for k in range(int(secs / 0.25) - 3)]
    drums = place([kick_hit(rng, 0.25) if k % 2 == 0 else snare_hit(rng, 0.25) for k in range(len(ts))], ts, secs)
    main = (pink(secs, -26, 31) + 0.5 * drums).astype(np.float32); key = pink(secs, -30, 32); rows = []; ok = True
    for blk in (32, 64, 512):
        run(main, key, depth=12, margin=3, want_log=False, block=blk, extra=["--latency"])
        line = [l for l in LAST_STDOUT.splitlines() if l.startswith("evidence_latency")][0]; v = {k: float(x) for k, x in (t.split("=") for t in line.split()[1:])}
        rows.append(f"block {blk}: {int(v['records'])} records, worst {v['max_samples']:.0f} samples = {v['max_samples'] / SR * 1000:.1f} ms (the last one {v['last_samples'] / SR * 1000:.1f} ms)"); ok = ok and v["max_samples"] / SR * 1000 < 40.0
    report("T20b", ok, "latency of the evidence record from the start of its job (200 ms + 9 ms after the hit) to its last slot, drum loop of T20 (want < 40 ms, the shortest gap between two hits, so a record is ready before the next hit can be analysed): " + "; ".join(rows))

# ---- P1j (TASK_P1j, SPEC 8f): a TARGET cut is static, and a cut never adds sound
BANDS3 = [31.5 * 2 ** (i / 3) for i in range(29)]   # 1/3-octave bands, lower edges 31.5 Hz .. 20.3 kHz (the top band is cut at Nyquist) (band i: [lo, lo * 2^(1/3)))
_B3 = {}
def third_mat(nfft, sr=SR):
    if nfft not in _B3:
        f = np.fft.rfftfreq(nfft, 1 / sr); _B3[nfft] = np.array([(f >= lo) & (f < lo * 2 ** (1 / 3)) for lo in BANDS3], dtype=np.float64)
    return _B3[nfft]
EDGE_S = 0.010   # the cosine edge of every analysis window
def edge_win(n, rise=True, tail=0.3, edge_s=EDGE_S):
    """Smooth edges at both ends, flat between. The window of a hit starts EDGE_S before the onset (see worst_excess) with a cosine rise, so the onset itself has full weight, and fades out over its last 30 %.
    Why not a Hann from the onset (P1i's PNGs): it weights the first milliseconds by ~0, so an all-pass delay of 20 ms (LR72 at 100 Hz) moves energy into the heavy part of the window and reads as +10..+25 dB in a band
    the dry hit hardly has (found in P1j: the same excess appears with an offline LTI filter). Why not a hard edge: the step where a loud low tone is cut off leaks 1/f into every high band, and
    the dry and the processed signal have different values there (the all-pass moves the phase), so the leakage differs by tens of dB where there is no signal at all."""
    w = np.ones(n); k = int(tail * n); w[n - k:] = 0.5 * (1 + np.cos(np.pi * np.arange(k) / k))
    if rise: e = int(edge_s * SR); w[:e] *= 0.5 * (1 - np.cos(np.pi * np.arange(e) / e))
    return w
def band_e(m, a, b, win):
    n = b - a; nfft = 1 << int(np.ceil(np.log2(n))); return third_mat(nfft) @ (np.abs(np.fft.rfft(np.asarray(m[a:b], np.float64) * win, nfft)) ** 2)
def worst_excess(x, y, starts, w, win=None, floor_db=-90.0):
    """Average-hit spectrum in 1/3-octave bands over windows of w seconds from each start; processed over dry, dB. Returns (worst dB, band lower edge Hz, the table)."""
    mx, my = np.mean(np.asarray(x, np.float64), axis=1) if np.ndim(x) > 1 else np.asarray(x, np.float64), np.mean(np.asarray(y, np.float64), axis=1) if np.ndim(y) > 1 else np.asarray(y, np.float64)
    pre = int(EDGE_S * SR) if win is None else 0; n = int(w * SR) + pre; win = edge_win(n) if win is None else win; ed = np.zeros(len(BANDS3)); ep = np.zeros(len(BANDS3)); k = 0
    for t in starts:
        a = int(round(t * SR)) - pre
        if a < 0 or a + n > len(mx): continue
        ed += band_e(mx, a, a + n, win); ep += band_e(my, a, a + n, win); k += 1
    eps = 10 ** (floor_db / 10) * ed.sum(); ex = 10 * np.log10((ep + eps) / (ed + eps)); i = int(np.argmax(ex)); return float(ex[i]), BANDS3[i], ex

def hat6(rng, secs=0.4, sr=SR):
    t = np.arange(int(secs * sr)) / sr; return (0.5 * sosfilt(butter(2, 6500, "high", fs=sr, output="sos"), rng.standard_normal(len(t))) * np.exp(-t / 0.025)).astype(np.float32)
def bass6(secs=0.8, sr=SR, f0=55.0):
    t = np.arange(int(secs * sr)) / sr; rng = np.random.default_rng(66)
    note = sum(a * np.sin(2 * np.pi * k * f0 * t) for k, a in ((1, 0.5), (2, 0.25), (3, 0.15), (5, 0.06))) * np.exp(-t / 0.25)
    pluck = 0.3 * sosfilt(butter(2, [500, 2000], "band", fs=sr, output="sos"), rng.standard_normal(len(t))) * np.exp(-t / 0.01)
    return (note + pluck).astype(np.float32)
def syl6(i, secs=0.5, sr=SR):
    """One vocal-like syllable: an optional 's' burst, then a vowel (saw through three resonators), pitch moving from syllable to syllable."""
    rng = np.random.default_rng(600 + i); n = int(secs * sr); t = np.arange(n) / sr; f0 = 135 + 22 * np.sin(i) + 12 * t / secs; src = 2 * ((np.cumsum(f0) / sr) % 1.0) - 1.0
    v = np.zeros(n)
    for f, g in zip([(650, 1100, 2600), (400, 2000, 2800), (300, 800, 2500), (500, 1500, 2500)][i % 4], (1.0, 0.6, 0.3)):
        w0 = 2 * np.pi * f / sr; r = 0.985; v += g * lfilter([1 - r], [1, -2 * r * np.cos(w0), r * r], src)
    v *= np.minimum(1, t / 0.03) * np.minimum(1, (secs - t) / 0.12) * 0.9
    if i % 2 == 0:
        m = int(0.07 * sr); v[:m] += 0.22 * sosfilt(butter(2, 5000, "high", fs=sr, output="sos"), rng.standard_normal(m)) * np.hanning(m)
    return v.astype(np.float32)
def t25_material(kind):
    """(mono track, onset times) for each instrument: the same synthetic generators as T2 / T4 / the pack, 14-16 hits."""
    rng = np.random.default_rng(25)
    if kind == "kick": return hit_track([kick6(55, 3000) for _ in range(14)])
    if kind == "snare": return hit_track([snare6(rng, 0.7) for _ in range(14)])
    if kind == "hats": return hit_track([hat6(rng) for _ in range(14)], spacing=0.25)
    if kind == "bass": return hit_track([bass6() for _ in range(14)], spacing=0.6)
    if kind == "vocal": return hit_track([syl6(i) for i in range(14)], spacing=0.7)
    if kind == "drums": return hit_track([kick6(55, 3000) if k % 2 == 0 else snare6(rng, 0.7) for k in range(16)])
T25_CASES = [("KICK", "kick", "sub", (12, 24, 36, 48, 72), 9, 0), ("KICK", "kick", "body", (24,), 9, 0), ("KICK", "kick", "click", (12, 24, 36, 48, 72), 9, 0),
             ("SNARE", "snare", "body", (24,), 9, 0), ("SNARE", "snare", "crack", (24,), 9, 0), ("SNARE", "snare", "snapair", (12, 24, 36, 48, 72), 9, 0), ("SNARE", "snare", "body", (12, 24, 36, 48, 72), 60, 1),
             ("HATS", "hats", "chick", (24,), 9, 0), ("HATS", "hats", "shimmer", (24,), 9, 0),
             ("BASS", "bass", "sub", (12, 24, 36, 48, 72), 9, 0), ("BASS", "bass", "body", (24,), 9, 0), ("BASS", "bass", "top", (12, 24, 36, 48, 72), 9, 0),
             ("VOCAL", "vocal", "box", (24,), 9, 0), ("VOCAL", "vocal", "nasal", (24,), 9, 0), ("VOCAL", "vocal", "harsh", (24,), 9, 0), ("VOCAL", "vocal", "s", (24,), 9, 0), ("VOCAL", "vocal", "box", (12, 24, 36, 48, 72), 60, 1),
             ("DRUMS", "drums", "kickbody", (24,), 9, 0), ("DRUMS", "drums", "snarebody", (24,), 9, 0), ("DRUMS", "drums", "hatsshimmer", (24,), 9, 0)]
def T25():
    """A cut never adds. Every TARGET instrument x part x SLOPE on its own material: the average hit spectrum (hits 6 to the end) in 1/3-octave bands, windows 0-150 ms and 0-500 ms from the onset,
    processed against dry. Limit +1.0 dB in every band. Bell parts ignore SLOPE (one run); the edge cuts and REVERB-READY run at all five slopes. Also reported: the same with a Hann window from the onset."""
    rows = []; worst_all = -99.0; worst_case = ""; mats = {}; hann_worst = -99.0; hann_case = ""; n_runs = 0; weak = []
    for inst, kind, part, slopes, amount, rev in T25_CASES:
        if kind not in mats: mats[kind] = t25_material(kind)
        x, times = mats[kind]
        for sl in slopes:
            spec = f"inst={inst.lower()},part={part},amount={amount},width=1,sweet=120,slope={sl}" + (",reverb=1" if rev else "")
            y, _, _ = run(x, None, want_log=True, target=spec, learn=True); n_runs += 1; Lr = LAST_LEARN; learned_ok = int(Lr["hits"][-1]) >= 3
            cut = float(np.max(LAST_TARGET)) if LAST_TARGET is not None else 0.0
            if not learned_ok or cut < 0.5: weak.append(f"{inst} {part}{' RR' if rev else ''} {sl}")
            st = times[5:]; w150, f150, _ = worst_excess(x, y, st, 0.150); w500, f500, _ = worst_excess(x, y, st, 0.500)
            h150, hf150, _ = worst_excess(x, y, st, 0.150, np.hanning(int(0.150 * SR))); h500, hf500, _ = worst_excess(x, y, st, 0.500, np.hanning(int(0.500 * SR)))
            name = f"{inst} {part.upper()}{' REVERB-READY' if rev else ''} @{sl}"
            rows.append(f"{name}: {max(w150, w500):+.2f} dB (0-150 ms {w150:+.2f} @{f150:.0f} Hz, 0-500 ms {w500:+.2f} @{f500:.0f} Hz; cut up to {cut:.1f} dB)")
            if max(w150, w500) > worst_all: worst_all, worst_case = max(w150, w500), name
            if max(h150, h500) > hann_worst: hann_worst, hann_case = max(h150, h500), f"{name} {h150 if h150 >= h500 else h500:+.1f} dB @{(hf150 if h150 >= h500 else hf500):.0f} Hz"
    report("T25", worst_all <= 1.0 and not weak, f"a cut never adds: {n_runs} runs (every instrument x part, the edge cuts and REVERB-READY at SLOPE 12 / 24 / 36 / 48 / 72), average hit spectrum over hits 6-end in 1/3-octave bands (31.5 Hz - 24 kHz, bands under -90 dB of the dry total in both signals are not judged), "
           f"window from 10 ms before the onset (cosine rise) to 150 / 500 ms after it, flat, cosine fade over its last 30 %; limit +1.0 dB; worst band over all cases {worst_all:+.2f} dB ({worst_case}); runs where nothing was learned or cut: {weak if weak else 'none'}. For information, the same with a Hann window from the onset (what the P1i PNGs showed: "
           f"the all-pass delay of the split moves energy into the heavy part of the window, it is not added sound): worst {hann_case}. Per case: " + "; ".join(rows))

def sliding_excess(x, y, t0, t1, fmin, win_s=0.1, hop_s=0.01, floor_db=-60.0):
    """processed over dry in the 1/3-octave bands from fmin up, over sliding 100 ms windows (10 ms cosine edges, hop 10 ms). A band is judged only where the dry window carries at least floor_db of the window's own energy
    (under that, both signals are leakage and rounding floor and their ratio means nothing). Why only bands from fmin up: an all-pass split delays what is near its crossover by milliseconds, and a window that cuts through a
    hit then sees energy move from one window to the next in those bands; an ideal static filter does it too (measured: the engine and the offline LTI response give the same +5.5 dB there). fmin = 4 x the highest
    crossover of the run: there the delay is under 0.2 ms and any excess is a click, a ripple or a sideband. Returns window starts, the table (n, bands from fmin) with -99 where not judged."""
    mx = np.mean(np.asarray(x, np.float64), axis=1) if np.ndim(x) > 1 else np.asarray(x, np.float64); my = np.mean(np.asarray(y, np.float64), axis=1) if np.ndim(y) > 1 else np.asarray(y, np.float64)
    n = int(win_s * SR); hop = int(hop_s * SR); a0 = int(t0 * SR); a1 = min(int(t1 * SR), len(mx)) - n; starts = np.arange(a0, a1, hop); e = int(EDGE_S * SR)
    win = np.ones(n); win[:e] *= 0.5 * (1 - np.cos(np.pi * np.arange(e) / e)); win[n - e:] *= 0.5 * (1 + np.cos(np.pi * np.arange(e) / e))
    keep = np.array(BANDS3) >= fmin; tab = []
    for a in starts:
        ex_, ey_ = band_e(mx, a, a + n, win), band_e(my, a, a + n, win); ok = ex_ > ex_.sum() * 10 ** (floor_db / 10)
        tab.append(np.where(ok, 10 * np.log10((ey_ + 1e-30) / (ex_ + 1e-30)), -99.0)[keep])
    return starts / SR, np.array(tab), np.array(BANDS3)[keep]
def t26_judge(x, y, times, f0max, switches=(), t_from=2.5):
    """(A) every hit on its own, from 10 ms before its onset: 100 ms and 500 ms windows, all bands, floor -60 dB re the hit; (B) sliding 100 ms windows, bands from 4 x the highest crossover up.
    +1.0 dB; +3.0 dB for the hit / the windows that hold the first 20 ms of a SLOPE change."""
    swl = list(switches); res = dict(worst_a=-99.0, over_a=0, n_a=0, worst_b=-99.0, over_b=0, n_b=0, where_a="", where_b="")
    for t in times:
        if t < t_from: continue
        for w in (0.1, 0.5):
            if int((t + w + 0.01) * SR) > len(x): continue
            lim = 3.0 if any(t - EDGE_S - 0.02 <= sw <= t + w for sw in swl) else 1.0
            ex_, fb, _ = worst_excess(x, y, [t], w, floor_db=-60.0); res["n_a"] += 1
            if ex_ - lim > res["worst_a"]: res["worst_a"] = ex_ - lim; res["where_a"] = f"hit at {t:.2f} s, {int(w * 1000)} ms window, band {fb:.0f} Hz, {ex_:+.2f} dB (limit {lim:+.0f})"
            res["over_a"] += ex_ > lim
    ts, tab, fr = sliding_excess(x, y, t_from, times[-1] + 0.5, 4.0 * 1.8 * f0max)
    for i, t in enumerate(ts):
        lim = 3.0 if any(t < sw + 0.02 and t + 0.1 > sw for sw in swl) else 1.0; ex_ = float(tab[i].max()); res["n_b"] += 1
        if ex_ - lim > res["worst_b"]: res["worst_b"] = ex_ - lim; res["where_b"] = f"window at {t:.2f} s, band {fr[int(np.argmax(tab[i]))]:.0f} Hz, {ex_:+.2f} dB (limit {lim:+.0f})"
        res["over_b"] += ex_ > lim
    res["ok"] = res["over_a"] == 0 and res["over_b"] == 0; return res
def T26():
    """T25's rule over the swap (T15), drift (T16) and automation (T24) runs (see t26_judge for the two views and why)."""
    out = []; ok = True
    n = 16; ka = [kick6_parts(50, 3000) for _ in range(n)]; kb = [kick6_parts(70, 3000, bodyx=2.82) for _ in range(n)]
    x, times = hit_track([sum(p) for p in ka] + [sum(p) for p in kb], spacing=0.5); y, _, _ = run(x, None, want_log=False, target="inst=kick,part=sub,amount=9")
    r = t26_judge(x, y[:, 0], times, 70.0); ok &= r["ok"]; out.append(("T15 swap 50 -> 70 Hz", r))
    n = 32; fs = [45 + 15 * k / (n - 1) for k in range(n)]; x, times = hit_track([kick6(f, 3000) for f in fs], spacing=0.5); y, _, _ = run(x, None, want_log=False, target="inst=kick,part=sub,amount=9")
    r = t26_judge(x, y[:, 0], times, 60.0); ok &= r["ok"]; out.append(("T16 drift 45 -> 60 Hz", r))
    n = 40; fs_ = [45 + 25 * k / (n - 1) for k in range(n)]; att = lambda h: h * np.where(np.arange(len(h)) < 0.008 * SR, 0.5 * (1 - np.cos(np.pi * np.arange(len(h)) / (0.008 * SR))), 1.0)
    x, times = hit_track([att(kick6_parts(f, 3000)[0]) for f in fs_], spacing=0.5); seq = [12, 24, 36, 48, 72, 48, 36, 24, 12, 72, 36, 12, 72, 24, 48, 72, 12, 36]; sw = []; swt = []
    for j, sl in enumerate(seq): sw += ["--switch", f"{times[4 + j] + 0.1:.3f}:inst=kick,part=sub,amount=9,slope={sl}"]; swt.append(times[4 + j] + 0.1)
    y, _, _ = run(x, None, want_log=False, target="inst=kick,part=sub,amount=9,slope=24", extra=sw)
    r = t26_judge(x, y[:, 0], times, 70.0, swt, t_from=1.0); ok &= r["ok"]; out.append(("T24 automation (18 SLOPE changes, sub only)", r))
    txt = "; ".join(f"{nm}: (A) {r['n_a']} hit windows, {r['over_a']} over the limit" + (f", worst {r['where_a']}" if r["over_a"] else "") + f"; (B) {r['n_b']} sliding windows, {r['over_b']} over the limit" + (f", worst {r['where_b']}" if r["over_b"] else "") for nm, r in out)
    mg = "; ".join(f"{nm.split(' ')[0]} margin to the limit: (A) {-r['worst_a']:.2f} dB, (B) {-r['worst_b']:.2f} dB" for nm, r in out)
    report("T26", ok, f"a cut never adds, over the swap / drift / automation runs. (A) each hit on its own from 10 ms before its onset, 100 ms and 500 ms windows, 1/3-octave bands, bands under -60 dB re the hit not judged, hits from 2.5 s on (T24 from 1 s); "
           f"(B) sliding 100 ms windows (hop 10 ms), bands from 4 x the highest crossover up (an all-pass delay near the crossover moves energy between windows in an ideal static filter too: the engine and an offline LTI response agree to 0.00 dB on it), same floor. "
           f"Limit +1.0 dB, +3 dB for the hit / the windows holding the first 20 ms of a SLOPE change. " + txt + ". " + mg)
def probe_gain_db(r, f, t0, t1, win_s=0.1, hop_s=0.05):
    """Lock-in amplitude of r at f over sliding windows (an integer number of periods), dB re 1."""
    n = int(round(win_s * f)) / f; n = int(n * SR); idx = np.arange(n) / SR; c = np.exp(-2j * np.pi * f * idx); out = []; tt = []
    for a in range(int(t0 * SR), int(t1 * SR) - n, int(hop_s * SR)): out.append(20 * np.log10(2 * abs(np.sum(np.asarray(r[a:a + n], np.float64) * c)) / n + 1e-12)); tt.append(a / SR)
    return np.array(tt), np.array(out)
def T27():
    """SPEC 8f: a learned kick, SUB selected: the TARGET gain at the part centre does not move over 20 s, between hits and on hits alike; it moves only when AMOUNT or the selection changes.
    (a) the logged cut depth with the listening live; (b) the realised gain at the sub's centre: the memory is frozen after 6 s (dyneqraw --listen-until), a 55 Hz probe sine (-30 dBFS) is added
    from 6 s, and the probe's response = (render with probe) - (render without): exact while nothing learns, measured by lock-in over 100 ms windows."""
    f0 = 55.0; n = 56; x, times = hit_track([kick6(f0, 3000) for _ in range(n)], spacing=0.5, lead=0.5, tail=0.6)   # 28 s
    sw = 20.0; spec = "inst=kick,part=sub,amount=9,slope=24"
    extra = ["--switch", f"{sw}:inst=kick,part=sub,amount=12,slope=24", "--switch", f"{sw + 4.0}:inst=kick,part=body,amount=12,slope=24"]
    y, _, _ = run(x, None, want_log=True, target=spec, learn=True, extra=extra); L = LAST_LEARN; tt = L["t_s"]; db_ = np.asarray(L["edge_lo_db"], np.float64)
    m = (tt >= 3.0) & (tt < sw); span_a = float(db_[m].max() - db_[m].min()); depth = float(db_[m].mean())
    i_amt = (tt >= sw + 0.3) & (tt < sw + 3.9); span_amt = float(db_[i_amt].max() - db_[i_amt].min()); depth_amt = float(db_[i_amt].mean())
    probe = (amp(-30) * np.sin(2 * np.pi * f0 * np.arange(len(x)) / SR)).astype(np.float32); probe[:int(6 * SR)] = 0
    ex2 = ["--listen-until", "6.0"] + extra
    y1, _, _ = run(x, None, want_log=False, target=spec, extra=ex2); y2, _, _ = run((x + probe).astype(np.float32), None, want_log=False, target=spec, extra=ex2)
    r = (y2[:, 0].astype(np.float64) - y1[:, 0]); ta, ga = probe_gain_db(r, f0, 7.0, sw - 0.2); g_ref = ga - db(amp(-30))
    tb, gb = probe_gain_db(r, f0, sw + 0.5, sw + 3.5); g_b = gb - db(amp(-30)); tc, gc = probe_gain_db(r, f0, sw + 4.6, 27.0); g_c = gc - db(amp(-30))
    # the same windows straddle hits and silence: 0.5 s spacing, 100 ms windows hopping 50 ms
    span_b = float(g_ref.max() - g_ref.min()); step_amt = float(g_b.mean() - g_ref.mean()); span_amt2 = float(g_b.max() - g_b.min()); step_sel = float(g_c.mean() - g_b.mean()); span_sel = float(g_c.max() - g_c.min())
    ok = span_a <= 0.1 and span_b <= 0.1 and abs(depth - 9.0) <= 0.05 and abs(depth_amt - 12.0) <= 0.05 and span_amt <= 0.1 and span_amt2 <= 0.1 and abs(step_amt) >= 1.0 and abs(step_sel) >= 1.0 and span_sel <= 0.1
    report("T27", ok, f"static cut: 56 kicks (55 Hz, 28 s), KICK + SUB AMOUNT 9, SLOPE 24, learned from the start; (a) the logged cut depth, listening live, 3-20 s: mean {depth:.3f} dB, spread {span_a:.4f} dB (want <= 0.1, mean 9.0); "
           f"(b) the realised gain of a 55 Hz probe through the TARGET stage (memory frozen at 6 s), 100 ms lock-in windows every 50 ms across hits and the gaps between them, 7-19.8 s: {g_ref.mean():+.2f} dB, spread {span_b:.4f} dB over {len(g_ref)} windows (want <= 0.1); "
           f"AMOUNT 9 -> 12 at 20 s: logged depth {depth_amt:.3f} dB (spread {span_amt:.4f}), probe gain {g_b.mean():+.2f} dB (step {step_amt:+.2f} dB, spread {span_amt2:.4f}); selection SUB -> BODY at 24 s: probe gain {g_c.mean():+.2f} dB (step {step_sel:+.2f} dB, spread {span_sel:.4f}); "
           f"the gain moves only at the two changes")


# ---- P1k (TASK_P1k, SPEC 8g): an edge cut must not eat the neighbouring part
T28_ROWS = None
def t28_windowed_power(x, starts, pre=0.020, length=0.470, nfft=1 << 16):
    """Average power spectrum of the hits through the onset-safe window: Tukey (alpha 0.25), from 20 ms before each onset, 470 ms long. Returns (freqs, power)."""
    from scipy.signal.windows import tukey
    m = np.mean(np.asarray(x, np.float64), axis=1) if np.ndim(x) > 1 else np.asarray(x, np.float64); n = int(round(length * SR)); w = tukey(n, 0.25); acc = np.zeros(nfft // 2 + 1); k = 0
    for t in starts:
        a = int(round((t - pre) * SR))
        if a < 0 or a + n > len(m): continue
        acc += np.abs(np.fft.rfft(m[a:a + n] * w, nfft)) ** 2; k += 1
    return np.fft.rfftfreq(nfft, 1 / SR), acc / max(k, 1)
def t28_cut_db(f, pd, pp, fc, half_oct=1 / 12):
    """cut (positive dB) of the processed average spectrum over the dry one, energy within +-1/12 octave of fc"""
    m = (f >= fc * 2 ** -half_oct) & (f <= fc * 2 ** half_oct); return float(-10 * np.log10(pp[m].sum() / pd[m].sum()))
def t28_cases():
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import make_pack_target as P
    Lp = P.loops(); st = lambda v: v.astype(np.float32)
    k_times = [k * 0.5 for k in range(int(12.0 / 0.5) - 1)]; v_times = [0.2 + 0.7 * i for i in range(int(8.0 / 0.7) - 1)]
    mats = {"KICK 55 Hz": (st(P.norm_peak(Lp["kick55"], 0.45)), k_times), "KICK 70 Hz": (st(P.norm_peak(Lp["kick70"], 0.45)), k_times),
            "SNARE": (st(P.norm_peak(Lp["snare"], 0.4)), Lp["st"]), "VOCAL": (st(P.norm_peak(P.vocal_line(), 0.6)), v_times)}
    # name, material, target spec, edges: (log column prefix, protected part index, reference: part index or ("x", factor of a part's centre), below)
    return [("KICK 55 Hz SUB", "KICK 55 Hz", "inst=kick,part=sub", [("lo", 1, 0)]), ("KICK 70 Hz SUB", "KICK 70 Hz", "inst=kick,part=sub", [("lo", 1, 0)]),
            ("KICK 55 Hz CLICK", "KICK 55 Hz", "inst=kick,part=click", [("hi", 1, 2)]), ("KICK 70 Hz CLICK", "KICK 70 Hz", "inst=kick,part=click", [("hi", 1, 2)]),
            ("SNARE REVERB-READY", "SNARE", "inst=snare,part=body,reverb=1", [("lo", 0, ("x", 0, 0.5)), ("hi", 1, ("x", 1, 2.0))]),
            ("VOCAL REVERB-READY", "VOCAL", "inst=vocal,part=box,reverb=1", [("lo", 0, ("x", 0, 0.5)), ("hi", 2, ("x", 2, 2.0))])], mats
def t28_run():
    global T28_ROWS
    if T28_ROWS is not None: return T28_ROWS
    cases, mats = t28_cases(); rows = []
    for name, mat, tgt, edges in cases:
        x, times = mats[mat]; x2 = np.stack([x, x], axis=1); hits = times[5:]
        for amount in (9, 60):
            for sl in SLOPES:
                y, _, _ = run(x2, None, want_log=False, target=f"{tgt},amount={amount},width=1,sweet=120,slope={sl}", learn=True); L = LAST_LEARN; f, pd = t28_windowed_power(x2, hits)
                _, ppy = t28_windowed_power(y, hits); n_ok = int(L["hits"][-1])
                for ci, (which, prot_part, ref) in enumerate(edges):
                    pc = float(L[f"p{prot_part}_c"][-1]); ok_p = bool(L[f"p{prot_part}_ok"][-1])
                    if isinstance(ref, tuple): rf = ref[2] * float(L[f"p{ref[1]}_c"][-1])
                    else: rf = float(L[f"p{ref}_c"][-1])
                    k = 0 if which == "lo" else 1; placed = float(L[f"edge_{which}_hz"][-1]); rule = float(L[f"rule_{which}_hz"][-1]); below = which == "lo"
                    rows.append(dict(name=name, amount=amount, slope=sl, which=which, below=below, prot_c=pc, ref=rf, placed=placed, rule=rule, loss_meas=t28_cut_db(f, pd, ppy, pc), loss_eng=float(L[f"prot_{which}_db"][-1]),
                                     ach_meas=t28_cut_db(f, pd, ppy, rf), ref_sig_db=float(10 * np.log10(pd[(f >= rf * 2 ** (-1 / 12)) & (f <= rf * 2 ** (1 / 12))].sum() / pd[(f >= pc * 2 ** (-1 / 12)) & (f <= pc * 2 ** (1 / 12))].sum())), ach_eng=float(L[f"ach_{which}_db"][-1]), gentle=int(L["gentle"][-1]), hits=n_ok, learned_ok=ok_p, cut_db=float(L[f"edge_{which}_db"][-1])))
    T28_ROWS = rows; return rows
def T28():
    """SPEC 8g: the neighbour is protected. KICK SUB, KICK CLICK, SNARE REVERB-READY, VOCAL REVERB-READY on the pack's materials (tools/make_pack_target.py loops: 55 / 70 Hz kick, snare, vocal line), SLOPE 12 / 24 / 36 / 48 / 72,
    AMOUNT 9 and 60, memory learned on the file itself, hits 6-end. Cut = processed / dry average power spectrum over the hits through a Tukey window (alpha 0.25) from 20 ms before each onset, 470 ms long, energy within
    +-1/12 octave. Limit 1.0 dB at the protected part's centre (the body for SUB / CLICK / the snare's and vocal's low corner, the crack / harsh part for the high corner)."""
    rows = t28_run(); worst = max(r["loss_meas"] for r in rows); bad = [r for r in rows if r["loss_meas"] > 1.0 or not r["learned_ok"]]
    table = []
    for r in rows: table.append("%-20s %5d %6d | %-4s  protected part centre %6.1f  edge placed (rule) %7.1f (%7.1f) | loss there: measured %5.2f engine %5.2f | target centre %7.1f: cut measured %s engine %6.2f%s" % (
        r["name"], r["slope"], r["amount"], r["which"], r["prot_c"], r["placed"], r["rule"], r["loss_meas"], r["loss_eng"], r["ref"], ("%6.2f" % r["ach_meas"]) if r["ref_sig_db"] > -25 else " no sig", r["ach_eng"], "  SLOPE TOO GENTLE" if r["gentle"] else ""))
    report("T28", not bad, f"{len(rows)} edge rows; worst loss at the protected part's centre {worst:.2f} dB (want <= 1.0); where the dry hit has less than -25 dB re the protected centre at the target centre (the REVERB-READY low corner's octave below the body) the audio holds no signal to read and the table says 'no sig' (the engine's number is the filter's response there); failures: {[(r['name'], r['slope'], r['amount'], round(r['loss_meas'], 2)) for r in bad] if bad else 'none'}.\n" + "\n".join(table))
def T29():
    """SPEC 8g: the 'slope too gentle' flag is set exactly when the cut reached at the target part's centre is below AMOUNT - 1 dB. The cut is recomputed here from the OFFLINE response (scipy's filters, lr_h) at the
    crossover the engine placed, so the engine's own number is checked as well (want within 0.05 dB), and the flag is compared per run (any of its edges)."""
    rows = t28_run(); by = {}
    for r in rows: by.setdefault((r["name"], r["amount"], r["slope"]), []).append(r)
    mism = []; worst_d = 0.0; n_set = 0; n_run = 0; near = 99.0
    for key, rs in by.items():
        want = False; n_run += 1
        for r in rs:
            off = float(-20 * np.log10(abs(lr_h(np.array([r["ref"]]), r["placed"], r["slope"], 400 if r["amount"] >= 60 else r["amount"], r["below"])[0])))
            worst_d = max(worst_d, abs(off - r["ach_eng"])); near = min(near, abs(off - (r["amount"] - 1.0)))
            if off < r["amount"] - 1.0: want = True
        n_set += rs[0]["gentle"]
        if bool(rs[0]["gentle"]) != want: mism.append((key, rs[0]["gentle"], want))
    report("T29", not mism and worst_d <= 0.05 and 0 < n_set < n_run, f"flag set exactly when the cut at the target part's centre (offline response at the placed crossover) < AMOUNT - 1: {n_run} runs, flag set in {n_set}, clear in {n_run - n_set}; "
           f"mismatches {mism if mism else 'none'}; engine's cut vs the offline response: worst difference {worst_d:.3f} dB (want <= 0.05); closest approach to the AMOUNT - 1 line {near:.2f} dB")


def T21b():
    """P1l T21b, reworded for P1m (SPEC 8h: TARGET reads the INPUT, so what cuts before it no longer reaches the memory): every TARGET run of the suite (run(): target is not None) was also run through
    tools/baseline/dyneqraw_p1k and its memory file(s) compared byte for byte. A run with NOTHING in front of TARGET (no key = no COLLIDE bank, no free band, not SELF) must be bit-identical: that is the P1l slicing
    result kept. A run with a layer in front (key / band / SELF) learned from the cut signal in P1k and learns from the input now, so a difference there is expected and only counted."""
    n = len(T21B_ROWS); plain = [r for r in T21B_ROWS if not r[3]]; front = [r for r in T21B_ROWS if r[3]]
    bad = [r for r in plain if not r[1]]; fdiff = [r for r in front if not r[1]]
    report("T21b", len(plain) > 0 and not bad, f"memory (every slot's last 8 records and the learned points built from them) after {n} TARGET runs of the suite against the P1k engine (tools/baseline/dyneqraw_p1k): "
           f"{len(plain)} runs with nothing in front of TARGET: {len(plain) - len(bad)} bit-identical, {len(bad)} different (want 0); {len(front)} runs with a key / free band / SELF in front: {len(front) - len(fdiff)} identical, "
           f"{len(fdiff)} different (expected: they learned from the cut signal in P1k, from the input now); {len(T21B_SKIPPED)} runs not comparable (they load a memory file in this engine's layout, which the P1k engine refuses)" + ("".join(f"; DIFFERENT (nothing in front): {r[0]}" for r in bad[:30]) if bad else ""))

def with_raw(exe, fn, *a, **k):
    """Run fn with the engine binary swapped for exe (T30 / T31 compare the P1l engine)."""
    global RAW
    old = RAW; RAW = exe
    try: return fn(*a, **k)
    finally: RAW = old

def self_cut_at(cuts, f, q_floor=0.0):
    """SELF cut log -> per block the summed hunter bells' response at f (dB, <= 0 = cut), from the logged centre / gain / Q of the four hunters."""
    fr, ct, q = hunters(cuts); tot = np.zeros(len(cuts))
    for s in range(fr.shape[1]):
        on = ct[:, s] > 0
        for i in np.nonzero(on)[0]:
            b, a = rbj_peak(float(fr[i, s]), float(q[i, s]), -float(ct[i, s])); tot[i] += float(response_db(b, a, f))
    return tot

def T30():
    """SPEC 8h: SELF reads the INPUT. Pink noise -20 dBFS (12 s), SELF on at the default DEPTH 6 / SENS 3, one free bell +6 dB Q 8 at 2.5 kHz. The summed hunter bells' response at 2.5 kHz (from the logged centre /
    gain / Q of the four hunters, RBJ) averaged over the last 8 s must be <= 0.5 dB of cut. The same case without the bell is the control (what SELF cuts there on this pink noise by itself); the P1l engine runs the same case."""
    x = pink(12.0, -20, 71); bell = {1: "type=bell,f=2500,g=6,q=8"}; last = slice(-int(8 * SR / BLOCK), None)
    _, cn, _ = run(x, None, mode="self", depth=6, margin=3, bands=bell, bandlog=True); bl = LAST_BANDLOG
    _, cc, _ = run(x, None, mode="self", depth=6, margin=3)
    _, co, _ = with_raw(P1L_RAW, run, x, None, mode="self", depth=6, margin=3, bands=bell)
    new, ctl, old = (-self_cut_at(c, 2500.0)[last] for c in (cn, cc, co))
    acc = -bl["b1_self_db"][last]
    ok = new.mean() <= 0.5
    report("T30", ok, f"SELF + a +6 dB Q 8 bell at 2.5 kHz on pink -20 dBFS, DEPTH 6 SENS 3, hunter cut at 2.5 kHz over the last 8 s: this engine mean {new.mean():.3f} dB (max {new.max():.3f}, want mean <= 0.5); "
                       f"control without the bell {ctl.mean():.3f} dB (max {ctl.max():.3f}); P1l engine {old.mean():.3f} dB (max {old.max():.3f}); the engine's own bandSelfDb accessor, same blocks: mean {acc.mean():.3f} dB (agrees with the logged hunters to {np.abs(acc - new).max():.3f} dB)")

def T31():
    """SPEC 8h: TARGET reads the INPUT. KICK + SUB (55 Hz kick, 10 hits) and SNARE + BODY (10 hits): the learned points (fundamental, every part's centre and edges, read from the learn log at every row) with no free band,
    with a +6 dB bell at the body and with a -6 dB bell at the body. Largest distance in octaves between the runs, want <= 1/24 = 0.0417. The P1l engine runs the same three for comparison."""
    rng = np.random.default_rng(31); kx, _ = kick6_track(55, 3000); sx, _ = hit_track([snare6(rng) for _ in range(10)])
    cases = [("KICK + SUB", kx, "inst=kick,part=sub,amount=9,width=1", BODY_X * 55), ("SNARE + BODY", sx, "inst=snare,part=body,amount=9,width=1", 200.0)]
    names = ["f0"] + [f"p{j}_{w}" for j in range(4) for w in ("c", "lo", "hi")]
    def spread(exe, x, tgt, fb):
        runs = []
        for b in (None, {1: f"type=bell,f={fb:.2f},g=6,q=2"}, {1: f"type=bell,f={fb:.2f},g=-6,q=2"}):
            with_raw(exe, run, x, None, want_log=False, target=tgt, learn=True, bands=b); runs.append(LAST_LEARN)
        worst = 0.0; n = 0; rows = len(runs[0]["t_s"])
        for L in runs[1:]:
            if len(L["t_s"]) != rows: return 99.0, 0, rows
            for nm in names:
                a, b = runs[0][nm], L[nm]; okc = (runs[0][nm[:3] + "ok"] > 0.5) & (L[nm[:3] + "ok"] > 0.5) if nm != "f0" else (a > 0) & (b > 0)
                okc &= (a > 0) & (b > 0)
                if okc.any(): worst = max(worst, float(np.max(np.abs(np.log2(b[okc] / a[okc]))))); n += int(okc.sum())
        return worst, n, rows
    out = []; ok = True
    for name, x, tgt, fb in cases:
        wn, nn, rows = spread(RAW, x, tgt, fb); wo, no, _ = spread(P1L_RAW, x, tgt, fb); ok = ok and wn <= 1 / 24 and nn > 0
        out.append(f"{name} (bell at the body, {fb:.0f} Hz, Q 2): this engine worst {wn:.4f} oct over {nn} values in {rows} rows (want <= 0.0417); P1l engine {wo:.4f} oct over {no} values")
    report("T31", ok, "learned points with no band / +6 dB bell / -6 dB bell at the body: " + "; ".join(out))

def T32():
    """SPEC 8h: COLLIDE judges the post-band signal and the engine reports the conflict. The synthetic / demo beat (main) against the vocal (key), COLLIDE at the defaults, a +3 dB bell Q 2 at the collision centre (the
    band with the deepest mean cut of a control run without the bell). bandConflictDb (the engine) against the sum of the 32 logged COLLIDE bells' RBJ responses at the band's frequency, recomputed here in numpy,
    over every block; bandNetDb against 3 + conflict + SELF + TARGET (the logged columns)."""
    b, v, src = demo_pair(); n = min(len(b), len(v), 12 * SR); b, v = b[:n], v[:n]
    _, cuts, _ = run(b, v); bidx = int(np.argmax(cuts.mean(axis=0))); fb = float(FC[bidx])
    _, _, _ = run(b, v, bands={1: f"type=bell,f={fb:.3f},g=3,q=2"}, bandlog=True); L = LAST_BANDLOG
    bw = erb_hz(E + STEP / 2) - erb_hz(E - STEP / 2); f_eng = float(L["b1_hz"][-1]); conf = L["b1_conflict_db"]
    ref = np.zeros(len(conf))
    for i in range(len(conf)):
        for k in range(32):
            g = float(L[f"g{k + 1}"][i])
            if g > 0:
                bb, aa = rbj_peak(float(FC[k]), float(FC[k] / (1.4 * bw[k])), -g); ref[i] += float(response_db(bb, aa, f_eng))
    net_ref = L["b1_gain_db"] + conf + L["b1_self_db"] + L["b1_target_db"]
    d_conf = float(np.max(np.abs(conf - ref))); d_net = float(np.max(np.abs(L["b1_net_db"] - net_ref))); steady = L["b1_gain_db"][-int(2 * SR / BLOCK):]
    ok = d_conf <= 0.1 and d_net <= 0.1 and conf.min() <= -1.0 and abs(float(steady.mean()) - 3.0) < 0.05
    report("T32", ok, f"COLLIDE ({src}, {n / SR:.1f} s, defaults) with a +3 dB Q 2 bell at {fb:.0f} Hz (the deepest-cut band): conflict at the band over the run: min {conf.min():.2f} dB, mean {conf.mean():.2f} dB; "
                       f"bandConflictDb against the 32 logged bells recomputed here: worst difference {d_conf:.4f} dB over {len(conf)} blocks (want <= 0.1); bandNetDb against gain + conflict + SELF + TARGET: worst {d_net:.4f} dB (want <= 0.1); "
                       f"band gain over the last 2 s {steady.mean():.3f} dB; example block with the deepest conflict: gain {L['b1_gain_db'][int(np.argmin(conf))]:.2f}, conflict {conf.min():.2f}, SELF {L['b1_self_db'][int(np.argmin(conf))]:.2f}, TARGET {L['b1_target_db'][int(np.argmin(conf))]:.2f}, net {L['b1_net_db'][int(np.argmin(conf))]:.2f} dB")

# ---- 2026-10-25 (0.9.1-test2): COLLIDE on kick vs bass (tester report T2, owner 12 Oct). Where does the cut land when the bass is keyed by a kick peaking at 48 Hz?
def kb_synth(secs=8.0):
    """The 12 Oct repro: a bass of sines 55 / 110 / 165 Hz (-12 / -15 / -18 dBFS) and a kick every 0.5 s: a chirp 160 -> 48 Hz (12 ms) decaying over 120 ms, -6 dBFS."""
    t = np.arange(int(secs * SR)) / SR
    bass = 10 ** (-12 / 20) * np.sin(2 * np.pi * 55 * t) + 10 ** (-15 / 20) * np.sin(2 * np.pi * 110 * t) + 10 ** (-18 / 20) * np.sin(2 * np.pi * 165 * t)
    kick = np.zeros_like(t); onsets = []
    for t0 in np.arange(0.25, secs, 0.5):
        i0 = int(t0 * SR); n = min(int(0.35 * SR), len(t) - i0); tt = np.arange(n) / SR
        f = 48 + (160 - 48) * np.exp(-tt / 0.012); kick[i0:i0 + n] += 10 ** (-6 / 20) * np.sin(2 * np.pi * np.cumsum(f) / SR) * np.exp(-tt / 0.12); onsets.append(t0)
    st = lambda x: np.stack([x, x], 1).astype(np.float32)
    return st(bass), st(kick), onsets
def kb_realised(cut, sr=SR):
    """The response of the 32 bank bells (Q = fc / (1.4 bw)) for a target cut per band, the gains solved as the engine does (Gauss-Seidel, 8 passes), on 20-400 Hz."""
    q = FC / (1.4 * (FHI - FLO))
    def bell(f0, g, qq, f):
        A = 10 ** (g / 40); w0 = 2 * np.pi * f0 / sr; al = np.sin(w0) / (2 * qq); cw = np.cos(w0); z1 = np.exp(-2j * np.pi * f / sr)
        return 20 * np.log10(np.abs(((1 + al * A) - 2 * cw * z1 + (1 - al * A) * z1 * z1) / ((1 + al / A) - 2 * cw * z1 + (1 - al / A) * z1 * z1)))
    shape = np.array([[1.0 if k == b else max(-bell(FC[k], -6.0, q[k], FC[b]) / 6.0, 0.0) for k in range(32)] for b in range(32)]); gain = np.zeros(32)
    for _ in range(8):
        for j in range(32): gain[j] = max(cut[j] - sum(gain[k] * shape[j, k] for k in range(32) if k != j), 0.0)
    grid = np.geomspace(20, 400, 400); resp = np.zeros_like(grid)
    for k in range(32):
        if gain[k] > 0: resp += bell(FC[k], -gain[k], q[k], grid)
    return grid, resp
def kb_row(main, key, onsets, depth, low, keylaw):
    _, cuts, _ = run(main, key, depth=depth, low=low, extra=["--keylaw", str(keylaw)])
    tb = (np.arange(cuts.shape[0]) + 1) * BLOCK / SR; hit = np.zeros(len(tb), bool)
    for t0 in onsets: hit |= (tb >= t0) & (tb < t0 + 0.150)
    mean, mhit = cuts.mean(0), cuts[hit].mean(0)
    g, r = kb_realised(mhit); deep = g[np.argmin(r)]; g2, r2 = kb_realised(mean); deep2 = g2[np.argmin(r2)]
    return mean, mhit, deep, r.min(), deep2, float(np.interp(48.0, g, r))
def K1():
    """INFO: per band 40 / 78 / 121 / 171 / 227 Hz the mean cut over the run and over the kick hits (0-150 ms after each onset), and where the realised cut of the hits is
    deepest. Acceptance asked on 25 Oct: the deepest cut within 1/3 octave of the kick's 48 Hz, and 121 / 171 Hz cut clearly less than the 0.9.0 plug-in (FOCUS 120).
    2026-10-26: keylaw 091 = the 0.9.0 / 0.9.1 law (the reference row: FOCUS 120, the plug-in's default), signal = the plug-in's law since 26 Oct
    (tools/bench); each at FOCUS LOW 120 (the default) and 40 (the minimum: the 40 / 78 Hz bands need to be in FOCUS). The candidates 1..3 of 25 Oct
    were removed (their numbers: STATUS.md 2026-10-25)."""
    bs, ks, on_s = kb_synth()
    bd, _ = read_wav(os.path.join(HERE, "demo_material", "bass.wav")); kd, _ = read_wav(os.path.join(HERE, "demo_material", "kick.wav"))
    env = np.abs(kd[:, 0]); on_d = []; last = -1e9
    for i in np.where(env > 0.3 * env.max())[0]:
        if i - last > 0.2 * SR: on_d.append(i / SR)
        last = i
    for name, m, k, on in (("synthetic 55/110/165 + 48 Hz chirp", bs, ks, on_s), ("demo_material bass keyed by kick", bd, kd, on_d)):
        for depth in (6.0, 12.0):
            ref = None
            for low, law in ((120.0, "091"), (40.0, "091"), (120.0, "signal"), (40.0, "signal")):
                mean, mhit, deep, dmin, deep2, r48 = kb_row(m, k, on, depth, low, law)
                if ref is None: ref = mean
                oct_ = np.log2(deep / 48.0); ok = abs(oct_) <= 1 / 3 and mean[2] < ref[2] - 0.5 and mean[3] < ref[3] - 0.5
                info("K1", f"{name}, DEPTH {depth:.0f}, FOCUS LOW {low:.0f}, keylaw {law}: mean cut over the run / over the hits (dB) " + " ".join(f"{FC[b]:.0f}:{mean[b]:.2f}/{mhit[b]:.2f}" for b in range(5)) +
                     f"; realised cut of the hits deepest {dmin:.2f} dB at {deep:.1f} Hz ({oct_:+.2f} oct from 48 Hz), {r48:.2f} dB at 48 Hz; of the whole run deepest at {deep2:.1f} Hz; "
                     f"against FOCUS 120 keylaw 091: 121 Hz {mean[2] - ref[2]:+.2f}, 171 Hz {mean[3] - ref[3]:+.2f} dB; acceptance {'met' if ok else 'NOT met'}")

def A13f():
    """INFO (25 Oct): why FOCUS LOW stays 120 Hz. FOCUS also bounds SELF's hunters: SELF at FOCUS 120 and at 30 Hz on plain pink noise (A13d's set-up), the demo beat
    (a drum bus, the tester's T4) and the resonant demo: the mean hunter cut per slot and the share of hunter work below 120 Hz."""
    b, _ = read_wav(os.path.join(HERE, "demo_material", "beat.wav")); rs, _ = read_wav(os.path.join(HERE, "demo_material", "resonant.wav"))
    for name, sig, depth in (("pink -20 dBFS, DEPTH 12", pink(20.0, -20, 7), 12), ("demo beat (drum bus), DEPTH 6", b, 6), ("demo resonant, DEPTH 6", rs, 6)):
        rows = []
        for low in (120.0, 30.0):
            _, cuts, _ = run(sig, None, mode="self", depth=depth, margin=3, low=low)
            fr, ct, q = hunters(cuts); w = slice(int(1 * SR / BLOCK), None); fr, ct = fr[w], ct[w]
            rows.append(f"FOCUS {low:.0f}: mean hunter cut per slot " + " / ".join(f"{v:.2f}" for v in np.abs(ct).mean(0)) + f" dB (all {np.abs(ct).mean():.2f}), hunter work below 120 Hz {100 * ((fr < 120) & (ct > 0.1)).mean():.1f} %")
        info("A13f", f"SELF, {name}: " + "; ".join(rows))

def A8f():
    """INFO (25 Oct): A8 at FOCUS LOW 30 Hz, the research's default (measured for the decision, NOT the plug-in's default: see A13f)."""
    b, v, src = demo_pair(); n = min(len(b), len(v)); b, v = b[:n], v[:n]
    y, cuts, _ = run(b, v, low=30.0)
    lb, ly = lufs(b), lufs(y); on = cuts > 0.1; pct = 100 * on.mean(axis=0)
    eb_k = band_energies(v[:, 0]); keyb = eb_k >= eb_k.max() * 10 ** (-18 / 10)
    e_before = band_energies(b[:, 0])[keyb].sum(); e_after = band_energies(y[:, 0])[keyb].sum()
    info("A8f", f"A8 with FOCUS LOW 30 (the research's default, not kept): beat {lb:.2f} LUFS -> {ly:.2f} LUFS, change {ly - lb:+.2f} LU; cut-time % per band: " + " ".join(f"{FC[i]:.0f}:{pct[i]:.0f}" for i in range(32)) +
                f"; key/beat SNR improvement {10 * np.log10(e_before / e_after):+.2f} dB; mean cut 40 / 78 Hz {cuts[:, 0].mean():.2f} / {cuts[:, 1].mean():.2f} dB")


CHECKS = [A1, A2, A3, A3m, A4, A5, A6, A7, A8, A8f, A9, A9b, A11, A11c, A11b, A12, A12b, A12c, A12d, A16, A17, A18, A13a, A13b, A13c, A13d, A13e, A13f, A14, A15, T1, T2, T3, T3b, T3c, T4, T5, T6, T7, T8, T9, T10, T11, T12, T13, T14, T15, T16, T17, T18, T19, T20, T20b, T21, T22, T23, T24, T25, T26, T27, T28, T29, T30, T31, T32, T21b, R1, R2, R3, R4, K1]

if __name__ == "__main__":
    args = sys.argv[1:]
    print(f"dyneqraw: {RAW}")
    for c in CHECKS:
        if not args or c.__name__ in args or any(c.__name__.startswith(a) for a in args):
            c()
    print(f"\n{'ALL PASS' if fails == 0 else str(fails) + ' FAILED'}")
    sys.exit(fails)
