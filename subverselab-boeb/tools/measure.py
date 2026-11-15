#!/usr/local/bin/python3
"""BOE-B engine measurements (SPEC.md "Acceptance"). Runs build/boebraw, the engine as the plug-in will run it.
Measurements here are independent of the engine's own analyser: scipy Butterworth bands, scipy A-weighting from the
analog IEC 61672 poles, BS.1770 loudness with gating. Reference values are read from Source/BoebReference.h.

  measure.py            every check; exit code = number of failures
  measure.py B4 B6      only those

Test material (nothing is listened to; every check is a number):
  vocals  macOS speech (say, rendered at 48 kHz) and one recorded take of the owner's (read only, if present)
  beat    a synthetic 90 BPM drum + bass loop
  dull    each vocal made dull on purpose by measured amounts, one cause at a time and all together
"""
import json, os, re, subprocess, sys, tempfile, time
import numpy as np
from scipy import signal

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..")
RAW = os.environ.get("BOEBRAW", os.path.join(ROOT, "build", "boebraw"))
TMP = tempfile.mkdtemp(prefix="boeb-")
import atexit, shutil
atexit.register(shutil.rmtree, TMP, True)   # every run left its renders behind (102 folders, 1.5 GB by 24 Oct)
SR = 48000
LISTEN = float(os.environ.get("BOEB_LISTEN", "6"))   # seconds LISTEN hears (PluginProcessor.h kListenSec)
fails = 0
results = []

# ---------------------------------------------------------------- reference values from the header (one source)
REF = {}
for line in open(os.path.join(ROOT, "Source", "BoebReference.h")):
    if "constexpr" in line:
        for m in re.finditer(r"(k\w+)\s*=\s*([^,;]+)", line.split("//")[0]):
            REF[m.group(1)] = m.group(2)
def ref(name):
    expr = re.sub(r"(\d)f\b", r"\1", REF[name])
    return float(eval(expr, {}, {k: ref(k) for k in re.findall(r"k\w+", expr)}))
BANDS = {b: (ref(f"k{b}Lo"), ref(f"k{b}Hi")) for b in ("Low", "Mud", "Mid", "Pres", "Sib", "Air")}
TARGET = {"mud": ref("kRefMud"), "pres": ref("kRefPres"), "air": ref("kRefAir"), "sib": ref("kRefSib")}
TOL = ref("kRefTolDb")
LAR = ref("kRapLarDb")
SPREAD = ref("kRefSpreadDb")

# ---------------------------------------------------------------- measurement helpers (independent of the engine)
def a_weight_sos(sr):
    f1, f2, f3, f4 = 20.598997, 107.65265, 737.86223, 12194.217
    z = [0, 0, 0, 0]
    p = [-2 * np.pi * f1] * 2 + [-2 * np.pi * f2, -2 * np.pi * f3] + [-2 * np.pi * f4] * 2
    k = 1.0
    zd, pd, kd = signal.bilinear_zpk(z, p, k, sr)
    sos = signal.zpk2sos(zd, pd, kd)
    w, h = signal.sosfreqz(sos, [1000], fs=sr)
    sos[0, :3] /= abs(h[0])
    return sos

def band_sos(lo, hi, sr):
    if hi >= 0.45 * sr:
        return signal.butter(4, lo, "highpass", fs=sr, output="sos")
    return np.vstack([signal.butter(4, lo, "highpass", fs=sr, output="sos"), signal.butter(4, hi, "lowpass", fs=sr, output="sos")])

def mono(x): return x.mean(axis=1) if x.ndim == 2 else x
def pdb(p): return 10 * np.log10(np.maximum(p, 1e-30))

def frames_power(x, sr, hop=0.1):
    n = int(hop * sr); m = len(x) // n
    return (x[: m * n].reshape(m, n) ** 2).mean(axis=1)

def analyse(vocal, sr=SR, beat=None):
    """The five causes, measured independently. vocal/beat: (n,2) or (n,)."""
    v = mono(vocal)
    a = signal.sosfilt(a_weight_sos(sr), v)
    fa = frames_power(a, sr)
    fdb = pdb(fa)
    active = (fdb > max(-55.0, fdb.max() - 20.0))
    out = {"active": int(active.sum())}
    bandp = {}
    for b, (lo, hi) in BANDS.items():
        bandp[b] = frames_power(signal.sosfilt(band_sos(lo, hi, sr), v), sr)[active].sum()
    mid = pdb(bandp["Mid"])
    for b in ("Low", "Mud", "Pres", "Sib", "Air"):
        out[b.lower()] = float(pdb(bandp[b]) - mid)
    st = pdb(np.convolve(fa, np.ones(4) / 4, mode="full")[: len(fa)])
    st = st[3:][active[3:]]
    out["spread"] = float(np.percentile(st, 90) - np.percentile(st, 10)) if len(st) > 4 else 0.0
    out["vocal"] = float(pdb(fa[active].mean()))
    # spectral centroid of active frames (read-out; Hermes 2019)
    f, t, S = signal.stft(v, sr, nperseg=4096)
    P = np.abs(S) ** 2
    out["centroid"] = float((f[:, None] * P).sum() / max(P.sum(), 1e-30))
    if beat is not None:
        fb = frames_power(signal.sosfilt(a_weight_sos(sr), mono(beat)), sr)[: len(fa)]
        out["lar"] = float(pdb(fa[active].mean()) - pdb(fb[active].mean()))
    return out

def lufs(x, sr=SR):
    """ITU-R BS.1770-4 integrated loudness with the absolute and relative gates."""
    x = x if x.ndim == 2 else np.stack([x, x], 1)
    hs = np.array([[1.53512485958697, -2.69169618940638, 1.19839281085285, 1.0, -1.69065929318241, 0.73248077421585]])
    hp = np.array([[1.0, -2.0, 1.0, 1.0, -1.99004745483398, 0.99007225036621]])
    if sr != 48000:   # design for this rate (same formulas as the engine; only used off 48 kHz)
        raise ValueError("lufs() is 48 kHz only here")
    y = signal.sosfilt(np.vstack([hs, hp]), x, axis=0)
    blk, hop = int(0.4 * sr), int(0.1 * sr)
    ms = np.array([(y[i : i + blk] ** 2).mean(axis=0).sum() for i in range(0, len(y) - blk, hop)])
    l = -0.691 + 10 * np.log10(np.maximum(ms, 1e-30))
    g = ms[l > -70]
    rel = -0.691 + 10 * np.log10(g.mean()) - 10
    g2 = ms[(l > -70) & (l > rel)]
    return float(-0.691 + 10 * np.log10(g2.mean()))

# ---------------------------------------------------------------- engine runner
def run(x, sr=SR, key=None, listen=None, block=None, blocks_random=False, at=(), report=False, **params):
    x = np.asarray(x, dtype=np.float32)
    if x.ndim == 1: x = np.stack([x, x], 1)
    fi, fo, fk, fr = (os.path.join(TMP, n) for n in ("in.f32", "out.f32", "key.f32", "rep.json"))
    x.tofile(fi)
    cmd = [RAW, fi, fo, str(sr)] + [f"{k}={v}" for k, v in params.items()]
    if key is not None:
        k = np.asarray(key, dtype=np.float32)
        if k.ndim == 1: k = np.stack([k, k], 1)
        k.tofile(fk); cmd += ["--key", fk]
    if listen: cmd += ["--listen", str(listen)]
    if report: cmd += ["--report", fr]
    if block: cmd += ["--block", str(block)]
    if blocks_random: cmd += ["--blocks", "random"]
    for n, kv in at: cmd += ["--at", str(n), kv]
    subprocess.run(cmd, check=True)
    y = np.fromfile(fo, dtype=np.float32).reshape(-1, 2)
    if report:
        return y, json.load(open(fr))
    return y

def check(name, ok, line):
    global fails
    print(f"{name:4} {'PASS' if ok else 'FAIL'}  {line}")
    results.append((name, ok, line))
    if not ok: fails += 1

# ---------------------------------------------------------------- material
CACHE = os.path.join(ROOT, "build", "_material")
# The owner's recorded takes on track 4 of "mali demo" (read only). Their spectra are voice-shaped and dark (AIR
# -35..-42 dB re MID): real dull vocals. Each is trimmed to its active span (seconds within 15 dB of the loudest).
OWNER_DIR = os.path.expanduser("~/Desktop/mali demo  Project/Samples/Recorded")
OWNER_TAKES = ["4-Audio 0001 [2026-09-27 224105].wav", "4-Audio 0002 [2026-09-27 224148].wav",
               "4-Audio 0002 [2026-09-27 224200].wav", "4-Audio 0003 [2026-09-27 224300].wav",
               "4-Audio 0004 [2026-09-27 224426].wav"]
TEXT = ("Yeah. Check it, one two. We came up from the block with a message in the pocket, every line on the beat, "
        "no time to stop it. Mic in the hand and the city in the back, every word that I say is a brick on the track. "
        "Hold it down, hold it down, let the bassline ride, every corner of the street got a story inside.")

def tts(voice, path):
    if not os.path.exists(path):
        wav = path + ".wav"
        subprocess.run(["say", "-v", voice, "-r", "230", "-o", wav, "--file-format=WAVE", "--data-format=LEF32@48000", TEXT], check=True)
        subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", wav, "-f", "f32le", "-ac", "1", "-ar", str(SR), path], check=True)
        os.remove(wav)
    return np.fromfile(path, dtype=np.float32)

def vocals():
    os.makedirs(CACHE, exist_ok=True)
    out = {}
    for name, voice in (("say_fred", "Fred"), ("say_reed", "Reed (English (US))")):
        try:
            v = tts(voice, os.path.join(CACHE, name + ".f32"))
        except subprocess.CalledProcessError:
            v = tts(voice.split(" ")[0], os.path.join(CACHE, name + ".f32"))
        out[name] = v
    import soundfile as sf
    for fn in OWNER_TAKES:
        path = os.path.join(OWNER_DIR, fn)
        if not os.path.exists(path): continue
        x, sr = sf.read(path, dtype="float32", always_2d=True)
        if sr != SR: continue
        x = x.mean(axis=1)
        sec = [10 * np.log10((x[i * sr : (i + 1) * sr] ** 2).mean() + 1e-20) for i in range(len(x) // sr)]
        keep = [i for i, v in enumerate(sec) if v > max(sec) - 20]       # the sung / rapped seconds, long silences dropped
        # Only the longest run of them (runs split by more than 3 s of silence): take 0001 has a 3 s loud event, 10 s of
        # silence, then the verse; splicing the event in front of the verse made a level jump no performance has.
        runs = [[keep[0]]]
        for i in keep[1:]:
            if i - runs[-1][-1] > 3: runs.append([])
            runs[-1].append(i)
        keep = max(runs, key=len)
        seg = np.concatenate([x[i * sr : (i + 1) * sr] for i in keep])
        while len(seg) < 16 * SR: seg = np.concatenate([seg, seg])
        out["take_" + fn[8:12] + fn[-11:-7]] = seg
    for k in out:   # common level: -20 dBFS A-weighted on active frames, 20 s max
        v = out[k][: 20 * SR]
        g = 10 ** ((-20 - analyse(v)["vocal"]) / 20)
        out[k] = (v * g).astype(np.float32)
    return out

def beat_loop(n, sr=SR, bpm=90, seed=7):
    rng = np.random.default_rng(seed)
    y = np.zeros(n)
    spb = 60 / bpm * sr
    t = np.arange(int(0.5 * sr)) / sr
    kick = np.sin(2 * np.pi * (45 + 80 * np.exp(-t * 30)) * t) * np.exp(-t * 6)
    snare = (rng.standard_normal(len(t)) * 0.6 + np.sin(2 * np.pi * 190 * t)) * np.exp(-t * 18)
    hat = signal.sosfilt(signal.butter(4, 7000, "highpass", fs=sr, output="sos"), rng.standard_normal(len(t))) * np.exp(-t * 60)
    bass_t = np.arange(n) / sr
    for b in range(int(n / spb) + 1):
        s = int(b * spb)
        for ev, amp, when in ((kick, 0.9, 0), (snare, 0.5, 1 if b % 2 else -1), (hat, 0.25, 0), (hat, 0.18, 0.5)):
            if when < 0: continue
            p = s + int(when * spb) if when else s
            if ev is snare and not (b % 2): continue
            seg = ev[: max(0, min(len(ev), n - p))]
            y[p : p + len(seg)] += amp * seg
    y += 0.35 * np.sin(2 * np.pi * 55 * bass_t) * (0.6 + 0.4 * np.sign(np.sin(2 * np.pi * bpm / 60 / 2 * bass_t)))
    pad = signal.sosfilt(signal.butter(2, [400, 2500], "bandpass", fs=sr, output="sos"), rng.standard_normal(n)) * 0.08
    y += pad
    return (y / np.abs(y).max() * 0.7).astype(np.float32)

def peq(x, f, q, g, sr=SR):
    A = 10 ** (g / 40); w = 2 * np.pi * f / sr; al = np.sin(w) / (2 * q); c = np.cos(w)
    b = [1 + al * A, -2 * c, 1 - al * A]; a = [1 + al / A, -2 * c, 1 - al / A]
    return signal.lfilter(b, a, x).astype(np.float32)

def hshelf(x, f, g, sr=SR, q=0.7071):
    A = 10 ** (g / 40); w = 2 * np.pi * f / sr; c = np.cos(w); al = np.sin(w) / (2 * q); s = 2 * np.sqrt(A) * al
    b = [A * ((A + 1) + (A - 1) * c + s), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - s)]
    a = [(A + 1) - (A - 1) * c + s, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - s]
    return signal.lfilter(b, a, x).astype(np.float32)

def wobble(x, depth_db=6, seg=0.3, seed=3, sr=SR):
    """Uneven level: a smooth random gain per segment, +-depth_db."""
    rng = np.random.default_rng(seed)
    m = int(len(x) / (seg * sr)) + 2
    pts = rng.uniform(-depth_db, depth_db, m)
    g = np.interp(np.arange(len(x)) / (seg * sr), np.arange(m), pts)
    return (x * 10 ** (g / 20)).astype(np.float32)

def put_against_beat(v, beat, lar):
    """Scale the vocal so its A-weighted level over active frames is lar dB over the beat's (same frames)."""
    cur = analyse(v, beat=beat)["lar"]
    return (v * 10 ** ((lar - cur) / 20)).astype(np.float32)

# ---------------------------------------------------------------- checks
def B1():
    v = vocals()["say_fred"]
    x = np.stack([v, v * 0.9], 1)
    worst = []
    for mood in (0, 1, 2):
        y = run(x, lift=0, mood=mood, air=3, body=2)
        worst.append(np.abs(y - x).max())
    y = run(x, lift=6, enabled=0)
    worst.append(np.abs(y - x).max())
    check("B1", max(worst) == 0.0, f"LIFT 0 (moods CLEAN / PUSH / 90S) and disabled at LIFT 6: largest difference from the input {max(worst):.3g} | want 0 (bit-exact)")

def B2():
    v = vocals()["say_reed"]
    beat = beat_loop(len(v))
    ref_ = run(v, key=beat, listen=LISTEN, lift=7, space=0.5, block=512)
    worst = 0.0
    for kw in ({"block": 1}, {"block": 64}, {"block": 4096}, {"blocks_random": True}):
        y = run(v, key=beat, listen=LISTEN, lift=7, space=0.5, **kw)
        worst = max(worst, float(np.abs(y - ref_).max()))
    check("B2", worst == 0.0, f"block sizes 1 / 64 / 512 / 4096 / random, LIFT 7, beat on the side input: largest difference {worst:.3g} | want 0")

def B3():
    n = SR * 4
    x = np.zeros((n, 2), np.float32)
    x[: SR, 0] = np.sign(np.sin(2 * np.pi * 100 * np.arange(SR) / SR))           # full-scale square
    x[SR : SR + 10, :] = np.nan; x[SR + 10 : SR + 20, :] = np.inf; x[SR + 20 : SR + 30, 1] = -np.inf
    x[2 * SR : 3 * SR, :] = 1e-38                                                # denormal-ish
    x[3 * SR :, :] = 4.0 * np.sin(2 * np.pi * 3000 * np.arange(n - 3 * SR) / SR)[:, None]   # +12 dBFS
    worst_pk, finite = 0.0, True
    for mood in range(6):
        y = run(x, lift=10, mood=mood, air=6, space=1, throw=1, output=12)
        finite &= bool(np.isfinite(y).all())
        worst_pk = max(worst_pk, float(np.abs(y).max()))
    lim = 10 ** (-1 / 20)
    check("B3", finite and worst_pk <= lim + 1e-6, f"hostile input (NaN, Inf, denormals, +12 dBFS), all six moods, LIFT 10, OUTPUT +12: finite {finite}, peak {20*np.log10(worst_pk):.3f} dBFS | want finite, <= -1.000 dBFS")

def dull_set(v, beat):
    base = put_against_beat(v, beat, LAR)
    return {
        "level -6 dB": put_against_beat(v, beat, LAR - 6),
        "presence -6 dB": put_against_beat(peq(base, 2000, 0.7, -6), beat, LAR),
        "mud +4 dB": put_against_beat(peq(base, 300, 1.0, 4), beat, LAR),
        "air -6 dB": put_against_beat(hshelf(base, 10000, -6), beat, LAR),
        "spread +-6 dB": put_against_beat(wobble(base), beat, LAR),
        "all five": put_against_beat(wobble(hshelf(peq(peq(base, 2000, 0.7, -6), 300, 1.0, 4), 10000, -6)), beat, LAR - 6),
    }


HF_NONE_DB = -60.0   # 11-16 kHz this far under MID = nothing up there for a shelf to lift (22 kHz speech synthesis,
                     # takes recorded band-limited): the AIR check does not apply and is reported as n/a

def hf_content(x, sr=SR):
    """11-16 kHz re MID, from the averaged spectrum (per-Hz density, so band widths do not enter)."""
    f, p = signal.welch(mono(x), sr, nperseg=8192)
    hi = p[(f >= 11000) & (f <= 16000)].mean(); mid = p[(f >= BANDS["Mid"][0]) & (f <= BANDS["Mid"][1])].mean()
    return float(pdb(hi) - pdb(mid))

CAP = {"pres": 6.0, "mud": 6.0, "air": 4.0}   # band dB the engine moves at LIFT 6 at most (BoebAnalyser::settingsFor)

def inside(m, beat_used, air_applies=True, before=None):
    """Which causes are inside the reference after processing. A tone cause that needed more than the engine's cap
    at LIFT 6 passes when it moved by at least cap - 1 dB towards the reference without going past it (a dark take is
    moved, not remade)."""
    def tone(k, ok_now):
        if ok_now or before is None: return ok_now
        need = abs(before[k] - TARGET[k])
        moved = (m[k] - before[k]) * np.sign(TARGET[k] - before[k])
        past = (m[k] - TARGET[k]) * np.sign(TARGET[k] - before[k]) > TOL
        return need > CAP[k] and moved >= CAP[k] - 1.0 and not past
    r = {
        "pres": tone("pres", abs(m["pres"] - TARGET["pres"]) <= TOL),
        "mud": tone("mud", m["mud"] <= TARGET["mud"] + TOL),
        "spread": m["spread"] <= SPREAD + TOL,
    }
    if air_applies: r["air"] = tone("air", abs(m["air"] - TARGET["air"]) <= TOL)
    if beat_used: r["lar"] = abs(m["lar"] - LAR) <= TOL
    return r

def B4():
    vs = vocals()
    rows, all_ok = [], True
    for vname, v in vs.items():
        beat = beat_loop(len(v))
        hf = hf_content(v)
        air_applies = True     # band-limited sources get their AIR from the exciter
        if hf <= HF_NONE_DB: print(f"     {vname}: no content above 11 kHz ({hf:+.0f} dB re MID: band-limited, AIR from the exciter)")
        for dname, d in dull_set(v, beat).items():
            before = analyse(d, beat=beat)
            y, rep = run(d, key=beat, listen=LISTEN, report=True, lift=6)
            after = analyse(y[SR * 2 :], beat=beat[SR * 2 :])     # skip the first 2 s (rider settling)
            ins = inside(after, True, air_applies, before)
            ok = all(ins.values())
            all_ok &= ok
            rows.append((vname, dname, before, after, ins, rep, ok))
    for vname, dname, b, a, ins, rep, ok in rows:
        print(f"     {vname:10} {dname:15} {'ok ' if ok else 'OUT'} "
              f"LAR {b['lar']:+5.1f}->{a['lar']:+5.1f}  spread {b['spread']:4.1f}->{a['spread']:4.1f}  "
              f"pres {b['pres']:+5.1f}->{a['pres']:+5.1f}  mud {b['mud']:+5.1f}->{a['mud']:+5.1f}  air {b['air']:+6.1f}->{a['air']:+6.1f}  "
              f"[{','.join(k for k, v in ins.items() if not v) or '-'}]  gr {rep['maxGr']:.1f} ride {rep['rideMin']:+.1f}..{rep['rideMax']:+.1f}")
    n_ok = sum(r[-1] for r in rows)
    check("B4", all_ok, f"five causes, {len(vs)} vocals x 6 dull versions, LISTEN {LISTEN:g} s then LIFT 6: {n_ok}/{len(rows)} inside the reference "
          f"(LAR {LAR}+-{TOL}, pres {TARGET['pres']:+.1f}+-{TOL}, mud <= {TARGET['mud']+TOL:+.1f}, air {TARGET['air']:+.1f}+-{TOL}, spread <= {SPREAD+TOL:.1f} dB)")

def B5():
    worst = 0.0
    lines = []
    for vname, v in vocals().items():
        beat = beat_loop(len(v))
        d = dull_set(v, beat)["all five"]
        y = run(d, key=beat, listen=LISTEN, lift=8, equal=1)
        diff = lufs(y[SR * 3 :]) - lufs(np.stack([d, d], 1)[SR * 3 :])
        worst = max(worst, abs(diff)); lines.append(f"{vname} {diff:+.2f}")
    check("B5", worst <= 0.5, f"EQUAL on, LIFT 8, 'all five' dull versions, BS.1770 after 3 s: processed - input = {', '.join(lines)} LU | want within +-0.5 LU")

def sib_ratio(x, sr=SR):
    """Sibilance re level on sibilant frames (SIB band within 12 dB of broadband), dB, 100 ms frames."""
    v = mono(x)
    s = frames_power(signal.sosfilt(band_sos(*BANDS["Sib"], sr), v), sr)
    b = frames_power(v, sr)
    r = pdb(s) - pdb(b)
    sel = r > -12
    return float(np.percentile(r[sel], 95)) if sel.sum() > 3 else float(r.max()), int(sel.sum())

def B6():
    worst, lines = -99.0, []
    for vname, v in vocals().items():
        x = hshelf(v, 5000, 4)    # an already bright, essy take
        y = run(x, listen=LISTEN, lift=10, air=6)
        a, na = sib_ratio(x); b, nb = sib_ratio(y)
        worst = max(worst, b - a); lines.append(f"{vname} {b - a:+.2f} ({na} frames)")
    check("B6", worst <= 1.0, f"de-esser, essy take, LIFT 10, AIR 6: sibilance re level (95th pct of sibilant frames) output - input = {', '.join(lines)} dB | want <= +1.0")

def B7():
    v = vocals()["say_fred"]
    a = run(v, listen=LISTEN, lift=6)
    b = run(v, key=np.zeros_like(v), listen=LISTEN, lift=6)
    d = float(np.abs(a - b).max())
    check("B7", d == 0.0 and np.isfinite(a).all(), f"no side input vs a silent side input, LIFT 6: largest difference {d:.3g} | want 0, finite")

def B8():
    imp = np.zeros(SR, np.float32); imp[1000] = 0.5
    v = vocals()["say_fred"]
    x = np.concatenate([v[: SR * 4], imp])
    y = run(x, lift=6)
    seg = mono(y)[SR * 4 :]
    pos = int(np.argmax(np.abs(seg)))
    check("B8", pos == 1000, f"latency: impulse at 1000 after 4 s of vocal, LIFT 6: output peak at {pos} | want 1000 (0 samples reported)")

def B9():
    v = vocals()["say_reed"][: SR * 10]
    on, off = SR * 4, SR * 5
    a = run(v, lift=6, bpm=90)
    b = run(v, lift=6, bpm=90, at=((on, "throw=1"), (off, "throw=0")))
    d = mono(b - a)
    delay = int(60 / 90 * SR)
    pre = float(np.abs(d[: on + delay - 200]).max())
    tail = float(np.sqrt((d[off + delay : off + delay + SR] ** 2).mean()))
    check("B9", pre < 1e-6 and tail > 1e-4, f"THROW held 4-5 s at 90 BPM: difference before the first echo (onset + 1 beat) {pre:.2g}, echo RMS after release {20*np.log10(tail+1e-12):.1f} dBFS | want 0 before, echoes after")

def B10():
    v = vocals()["say_fred"]
    lines, ok = [], True
    for sr in (44100, 96000, 192000):
        x = signal.resample_poly(v, sr // 100, 480).astype(np.float32)
        y = run(x, sr=sr, listen=LISTEN, lift=6)
        y0 = run(x, sr=sr, lift=0)
        good = bool(np.isfinite(y).all()) and np.abs(y0 - np.stack([x, x], 1)).max() == 0
        ok &= good
        lines.append(f"{sr}: {'ok' if good else 'BAD'}")
    check("B10", ok, f"other rates, finite at LIFT 6 and bit-exact at LIFT 0: {', '.join(lines)}")

def B11():
    v = np.tile(vocals()["say_fred"], 3)[: SR * 60]
    beat = beat_loop(len(v))
    t = time.time(); run(v, key=beat, lift=8, space=0.5, mood=1); dt = time.time() - t
    check("B11", dt < 6.0, f"speed: 60 s stereo with side input in {dt:.2f} s including file I/O ({60/dt:.0f}x real time) | want < 6 s")

def B12():
    """Exciter: band-limited takes get AIR, the band under the cut is untouched, nothing folds back, full-band
    sources leave it off."""
    vs = vocals()
    lines, ok = [], True
    for vname, v in vs.items():
        y_on, rep = run(v, listen=LISTEN, report=True, lift=6)
        if not rep["bandLimited"]:
            limited = hf_content(v) <= HF_NONE_DB
            ok &= not limited
            lines.append(f"{vname}: LISTEN says not band-limited (cut {rep['cutMeasured']:.0f} Hz){' BAD: it is' if limited else ''}"); continue
        y_off = run(v, listen=LISTEN, lift=6, testoff=64)
        cut = rep["cut"]
        f, pon = signal.welch(mono(y_on)[SR * 2 :], SR, nperseg=8192); f, poff = signal.welch(mono(y_off)[SR * 2 :], SR, nperseg=8192)
        below = (f > 200) & (f < 0.9 * cut)
        d_below = float(np.abs(10 * np.log10(pon[below].sum() / poff[below].sum())))
        # The gain is calibrated by LISTEN on what it heard and then held (by design: no loop), so the calibration is
        # checked on the listened 8 s; the whole take is printed (and judged in B4): passages differ by 15 dB in what
        # the exciter makes.
        a = analyse(y_on[: int(SR * LISTEN)]); whole = analyse(y_on[SR * 2 :])["air"]
        good = d_below < 0.2 and abs(a["air"] - TARGET["air"]) <= TOL
        ok &= good
        lines.append(f"{vname}: cut {cut:.0f}, AIR {a['air']:+.1f} in the listened {LISTEN:g} s ({whole:+.1f} whole take), under-cut change {d_below:.2f} dB {'ok' if good else 'BAD'}")
    # a full-band source must leave the exciter off
    rng = np.random.default_rng(5)
    v = vs["say_fred"]
    full = (v + 0.003 * signal.sosfilt(signal.butter(4, 9000, "highpass", fs=SR, output="sos"), rng.standard_normal(len(v)))).astype(np.float32)
    _, rep = run(full, listen=LISTEN, report=True, lift=6)
    ok &= not rep["bandLimited"]
    lines.append(f"full-band (speech + air noise, cut {rep['cutMeasured']:.0f} Hz): exciter {'off' if not rep['bandLimited'] else 'ON'}")
    # Fold-back: a pure tone under the cut must come out as itself and its octave only. Exciter forced on (cut 7400 and
    # 10600 Hz), every other stage held still; spurious = anything off f and 2f (and DC) above the cut, re 2f.
    for sr, cut, f0 in ((48000, 7400, 6900), (44100, 10600, 10200), (48000, 10600, 10200)):
        n = sr * 3
        x = (0.1 * np.sin(2 * np.pi * f0 * np.arange(n) / sr)).astype(np.float32)
        y = run(x, sr=sr, lift=6, cut=cut, excite=0, testoff=1 | 2 | 4 | 8 | 16 | 32, presence=0, air=0)
        f, p_ = signal.welch(mono(y)[sr:], sr, nperseg=16384, window="blackmanharris")
        h2 = p_[np.abs(f - 2 * f0) < 20].sum()
        spur = p_[(f > cut) & (np.abs(f - 2 * f0) > 60)].sum()
        rel = float(10 * np.log10(max(spur, 1e-30) / max(h2, 1e-30)))
        good = rel < -60
        ok &= good
        lines.append(f"tone {f0} Hz at {sr} Hz, cut {cut}: octave present, everything else above the cut {rel:+.0f} dB re the octave {'ok' if good else 'BAD'}")
    print("     " + "\n     ".join(lines))
    check("B12", ok, f"exciter on band-limited sources: AIR in the listened {LISTEN:g} s inside {TARGET['air']:+.1f}+-{TOL}, band under the cut changed < 0.2 dB, a tone gives only its octave (other content above the cut < -60 dB); off on full-band input")

def B13():
    """Exciter purity: a clean harmonic voice (additive, 130 Hz, band-limited at 7.3 kHz) must come out with only
    harmonics in the made octave. The fold-back tone test (B12) cannot see a nonlinearity that a steady sine never
    reaches; this one can (a hard divisor floor measured -24 dB here and was heard as fizz)."""
    f0 = 130.0
    t = np.arange(SR * 6) / SR
    x = np.zeros_like(t)
    for k in range(1, int(7300 / f0) + 1):
        fk = k * f0
        x += (1 / k) * (1 + 2 * np.exp(-((fk - 700) / 150) ** 2)) * np.sin(2 * np.pi * fk * t + k * 0.7)
    x = (x * 0.3 / np.abs(x).max()).astype(np.float32)
    base = dict(lift=6, cut=7400, excite=0, presence=0, air=0)
    on = run(x, testoff=1 | 2 | 4 | 8 | 16 | 32, **base); off = run(x, testoff=1 | 2 | 4 | 8 | 16 | 32 | 64, **base)
    f, p_ = signal.welch(mono(on - off)[SR:], SR, nperseg=65536, window="blackmanharris")
    band = (f > 7400) & (f < 23000); harm = np.abs(f - np.round(f / f0) * f0) < 6
    inh = float(10 * np.log10(p_[band & ~harm].sum() / p_[band & harm].sum()))
    check("B13", inh < -50, f"exciter purity: clean harmonic voice, inharmonic re harmonic energy in what the exciter adds {inh:+.1f} dB | want < -50")

def owner_clips(clip_s=12):
    """The owner's takes as the listening packs use them (tools/dinle.py): the take uncut from its first to its last
    voiced second, the clip_s seconds with most voice, the engine starting 2 s before it."""
    import soundfile as sf
    out = {}
    for fn in OWNER_TAKES:
        path = os.path.join(OWNER_DIR, fn)
        if not os.path.exists(path): continue
        x, sr = sf.read(path, dtype="float32", always_2d=True)
        if sr != SR: continue
        x = x.mean(axis=1)
        sec = [10 * np.log10((x[i * sr : (i + 1) * sr] ** 2).mean() + 1e-20) for i in range(len(x) // sr)]
        keep = [i for i, v in enumerate(sec) if v > max(sec) - 20]
        v, sec = x[keep[0] * sr : (keep[-1] + 1) * sr], np.array(sec[keep[0] : keep[-1] + 1])
        t0 = max(range(2, max(3, len(v) // SR - clip_s + 1)), key=lambda t: float(np.sum(10 ** (sec[t : t + clip_s] / 10))))
        n = min(clip_s * SR, len(v) - t0 * SR)
        out["take_" + fn[8:12] + fn[-11:-7]] = (v[(t0 - 2) * SR :], n)
    return out

def B14():
    """Between words ("dipler", owner 24 Oct: "dipte hafif hışırtı var"): at equal loudness, the gaps of the owner's
    takes must not come out louder than in the take (A-weighted), and above the band limit (where the take has nothing
    and the exciter makes the AIR) not more than 6 dB. Words must not drop: the quiet voiced frames (10th percentile of
    the take's voiced frames) keep their level re the loud ones within 1.5 dB."""
    aw = a_weight_sos(SR)
    hi = signal.butter(4, 7600, "highpass", fs=SR, output="sos")
    def fr(m, sos): return pdb(frames_power(signal.sosfilt(sos, m), SR, 0.05))
    def at_lufs(y): return y * 10 ** ((-20 - lufs(y)) / 20)
    ok, lines = True, []
    for name, (w, n) in owner_clips().items():
        a = at_lufs(np.stack([w, w], 1)[2 * SR : 2 * SR + n]); y = at_lufs(run(w, listen=LISTEN, lift=6)[2 * SR : 2 * SR + n])
        fa, fy = fr(mono(a), aw), fr(mono(y), aw)
        la, ly = np.percentile(fa, 90), np.percentile(fy, 90)
        gaps = fa < la - 25; voiced = (fa > la - 20)
        q = voiced & (fa <= np.percentile(fa[voiced], 10))
        dg = float(np.median(fy[gaps] - ly) - np.median(fa[gaps] - la)) if gaps.sum() >= 5 else 0.0
        dh = float(np.median(fr(mono(y), hi)[gaps] - ly) - np.median(fr(mono(a), hi)[gaps] - la)) if gaps.sum() >= 5 else 0.0
        dw = float(np.median(fy[q] - ly) - np.median(fa[q] - la))
        good = dg <= 0.0 and dh <= 6.0 and dw >= -1.5
        ok &= good
        lines.append(f"{name} gaps {int(gaps.sum())}: {dg:+.1f} / {dh:+.1f}, quiet words {dw:+.1f}")
    check("B14", ok, "between words, LIFT 6, equal loudness, processed - take: A-weighted gaps / gaps above 7.6 kHz, quiet voiced frames: "
          + "; ".join(lines) + " dB | want gaps <= 0, above 7.6 kHz <= +6, quiet words >= -1.5 (gap values 0 with < 5 gap frames)")

ALL = ["B1", "B2", "B3", "B4", "B5", "B6", "B7", "B8", "B9", "B10", "B11", "B12", "B13", "B14"]

if __name__ == "__main__":
    want = [a for a in sys.argv[1:] if a in ALL] or ALL
    print(f"boebraw: {os.path.abspath(RAW)}")
    for name in want:
        globals()[name]()
    print(f"{len(results) - fails}/{len(results)} PASS")
    sys.exit(fails)
