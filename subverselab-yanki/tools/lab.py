"""YANKI preset lab: renders through the real processor (build/render_artefacts/Release/render)
and measures the output. Shared by tools/presets_v2.py. numpy/scipy only."""
import os, subprocess, tempfile, struct, math
import numpy as np
from scipy.io import wavfile
from scipy import signal

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
RENDER = os.path.join(ROOT, "build/render_artefacts/Release/render")
PROTO = os.path.normpath(os.path.join(ROOT, "../prototypes"))
SRC_VOCAL = os.path.join(PROTO, "_kaynak/Kadin_vokal_20s.wav")
SRC_PLUCK = os.path.join(PROTO, "YAY/_old_demos/hava_round4/public/pluck_0_KURU.wav")
SRC_LOOP = os.path.join(PROTO, "YAY/_old_demos/hava_round4/public/drums_0_KURU.wav")
SRC_PAD = os.path.join(PROTO, "YAY/_old_demos/hava_round4/public/pad_0_KURU.wav")
TMP = os.environ.get("YANKI_LAB_TMP", tempfile.gettempdir())
BPM = 120.0
BAR = 4 * 60.0 / BPM          # 2.0 s


def load(path):
    sr, d = wavfile.read(path)
    x = d.astype(np.float64) / (2 ** 31 if d.dtype == np.int32 else 32768.0)
    if x.ndim == 1:
        x = np.stack([x, x], axis=1)
    return sr, x


def save24(path, sr, x):
    x = np.clip(x, -1.0, 1.0 - 2 ** -23)
    n = (x * 2 ** 23).astype(np.int32)
    raw = bytearray()
    for frame in n:
        for v in frame:
            raw += struct.pack("<i", int(v))[:3]
    ch = x.shape[1]
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(raw)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, ch, sr, sr * ch * 3, ch * 3, 24))
        f.write(b"data" + struct.pack("<I", len(raw)))
        f.write(raw)


def pad_to(x, sr, seconds):
    n = int(seconds * sr)
    if len(x) >= n:
        return x[:n]
    return np.vstack([x, np.zeros((n - len(x), x.shape[1]))])


def pink_burst(sr, seconds, ms=150.0, level_db=-18.0, seed=3):
    """A pink-ish noise burst (1/f shaped, 20 ms fades) then silence."""
    rng = np.random.default_rng(seed)
    n = int(sr * ms / 1000.0)
    w = rng.standard_normal(n)
    f = np.fft.rfft(w)
    k = np.arange(len(f)); k[0] = 1
    f = f / np.sqrt(k)
    b = np.fft.irfft(f, n)
    fade = int(0.02 * sr)
    env = np.ones(n); env[:fade] = np.linspace(0, 1, fade); env[-fade:] = np.linspace(1, 0, fade)
    b = b * env
    b = b / np.sqrt((b ** 2).mean()) * 10 ** (level_db / 20.0)
    x = np.zeros((int(sr * seconds), 2))
    x[int(0.05 * sr):int(0.05 * sr) + n, :] = b[:, None]
    return x, int(0.05 * sr), n


def args_for(p, bpm=BPM):
    a = ["--bpm", str(bpm)]
    a += ["--mood", str(p.get("mood", 0))]
    a += ["--sync", str(p.get("sync", 6))]
    if "time_ms" in p: a += ["--time", str(p["time_ms"])]
    if "fine" in p: a += ["--fine", str(p["fine"])]
    if "fb" in p: a += ["--feedback", "%.6f" % p["fb"]]
    if "tone" in p: a += ["--tone", str(p["tone"])]
    if "wear" in p: a += ["--wear", str(p["wear"])]
    if "mix" in p: a += ["--mix", "%.6f" % p["mix"]]
    if "spread" in p: a += ["--spread", str(p["spread"])]
    if "darken" in p: a += ["--darken", str(p["darken"])]
    if "rise" in p: a += ["--rise", str(p["rise"])]
    if "lock" in p: a += ["--lock", str(p["lock"])]
    if "cap" in p: a += ["--cap", str(p["cap"])]
    if "output" in p: a += ["--output", str(p["output"])]
    if "heads" in p: a += ["--heads", str(p["heads"])]
    return a


def render(sr, x, p, bpm=BPM, tag="r"):
    fin = os.path.join(TMP, "yl_%s_%d_in.wav" % (tag, os.getpid()))
    fout = os.path.join(TMP, "yl_%s_%d_out.wav" % (tag, os.getpid()))
    save24(fin, sr, x)
    r = subprocess.run([RENDER, fin, fout] + args_for(p, bpm), capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("render failed: " + r.stderr)
    sr2, y = load(fout)
    os.remove(fin); os.remove(fout)
    return y


def db(v):
    return 20 * math.log10(max(v, 1e-12))


def rms(x):
    return math.sqrt(float((x ** 2).mean()) + 1e-30)


def true_peak_db(x, sr):
    y = signal.resample_poly(x, 4, 1, axis=0)
    return db(float(np.abs(y).max()))


def band(x, sr, lo, hi, order=4):
    sos = signal.butter(order, [lo, hi], btype="band", fs=sr, output="sos")
    return signal.sosfilt(sos, x, axis=0)


def repeat_series(y, sr, t0, burst_n, T, ref_rms, n_rep=80):
    """RMS (dB re the dry burst) of the output in each repeat window k*T after the burst."""
    out = []
    L = burst_n / sr
    for k in range(1, n_rep + 1):
        a = int((t0 / sr + k * T) * sr); b = int(a + (L + 0.02) * sr)
        if b > len(y): break
        w = y[a:b]
        out.append(db(rms(w) / ref_rms))
    return out


def t60_from_series(series, T, floor=-60.0):
    """Time (s, from the dry burst) at which the repeats cross `floor` dB re dry (linear interpolation)."""
    for i, v in enumerate(series):
        if v < floor:
            if i == 0:
                return T
            p = series[i - 1]
            frac = (p - floor) / max(p - v, 1e-9)
            return (i + frac) * T      # repeat i (0-based) is at (i+1) T
    return float("inf") if series else 0.0
