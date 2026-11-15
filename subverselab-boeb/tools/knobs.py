#!/usr/local/bin/python3
"""Does each knob make an audible difference? (owner 24 Oct in Live: "knobları sonuna kadar çeviriyorum ama duyulur bir
fark yaratmıyor"). A file (default: Live's 'Vocal Patience Main and Harmonies' loop, a polished full-band vocal, and the
owner's take 0004, band-limited) runs through LISTEN, then each setting; prints the output level and six bands re the input.

  knobs.py [file.wav]
"""
import os, subprocess, sys
import numpy as np, soundfile as sf
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import measure as M

LIVE = "/Applications/Ableton Live 12 Standard.app/Contents/App-Resources/Core Library/Samples/Loops/Tonal/Vocal/Vocal Patience Main and Harmonies.wav"
OWNER = os.path.join(M.OWNER_DIR, "4-Audio 0004 [2026-09-27 224426].wav")
BANDS = [(60, 200), (200, 500), (500, 1000), (1000, 4000), (4000, 9000), (9000, 16000)]
CASES = [("LIFT 0", dict(lift=0)), ("LIFT 6", dict(lift=6)), ("LIFT 10", dict(lift=10)),
         ("LIFT 6 BODY +6", dict(lift=6, body=6)), ("LIFT 6 BODY -6", dict(lift=6, body=-6)),
         ("LIFT 6 AIR 6", dict(lift=6, air=6)), ("LIFT 6 90S", dict(lift=6, mood=2)), ("LIFT 6 90S AIR 6", dict(lift=6, mood=2, air=6)),
         ("LIFT 10 PUSH", dict(lift=10, mood=1))]

def bands(y, sr):
    sp = np.abs(np.fft.rfft(y.mean(axis=1) * np.hanning(len(y)))) ** 2
    fr = np.fft.rfftfreq(len(y), 1 / sr)
    return np.array([10 * np.log10(sp[(fr >= a) & (fr < b)].sum() + 1e-20) for a, b in BANDS])

def one(path, skip_s=0):
    x, sr = sf.read(path, dtype="float32", always_2d=True)
    x = x[int(skip_s * sr):]
    x = np.tile(x, (int(np.ceil(24 * sr / len(x))), 1))[: 24 * sr]
    if x.shape[1] == 1: x = np.repeat(x, 2, 1)
    fi, fo = os.path.join(M.TMP, "k_in.f32"), os.path.join(M.TMP, "k_out.f32")
    x.astype(np.float32).tofile(fi)
    ref = x[10 * sr:]
    b0, l0 = bands(ref, sr), 20 * np.log10(np.sqrt((ref ** 2).mean()))
    print(f"{os.path.basename(path)}  ({sr} Hz; bands {' '.join(f'{a}-{b}' for a, b in BANDS)})")
    for name, p in CASES:
        subprocess.run([M.RAW, fi, fo, str(sr), "--listen", "8"] + [f"{k}={v}" for k, v in p.items()], check=True)
        y = np.fromfile(fo, dtype=np.float32).reshape(-1, 2)[10 * sr:]
        print(f"  {name:18} level {20*np.log10(np.sqrt((y**2).mean())) - l0:+5.1f} dB   bands " + " ".join(f"{d:+5.1f}" for d in bands(y, sr) - b0))

if __name__ == "__main__":
    if len(sys.argv) > 1: one(sys.argv[1])
    else:
        one(LIVE)
        if os.path.exists(OWNER): one(OWNER, skip_s=10)
