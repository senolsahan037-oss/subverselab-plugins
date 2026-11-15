#!/usr/local/bin/python3
"""Low-end distortion of the compressor (owner 24 Oct on pack 25: "bir amfi göbeği gibi titriyor").

LISTEN runs on the first 8 s of the owner's take 0004, then a tone follows at the take's own level. What the engine adds
around the tone is measured as harmonics 2..10 re the fundamental (dB). Also prints the compressor's mean gain
reduction over each owner take at LIFT 6 (from --trace), so a fix can be held to the same amount of compression.

  lowend.py            all cases
"""
import os, subprocess, sys, tempfile
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import measure as M

SR = M.SR

def harmonics_db(y, f0):
    y = y * np.hanning(len(y))
    sp = np.abs(np.fft.rfft(y)) ** 2
    fr = np.fft.rfftfreq(len(y), 1 / SR)
    def band(f): return sp[(fr > f - 3) & (fr < f + 3)].sum()
    h1 = band(f0)
    return 10 * np.log10(sum(band(k * f0) for k in range(2, 11)) / h1 + 1e-30)

def tone_case(take, f0, bursts, **params):
    lead = take[: 8 * SR]
    rms = np.sqrt(np.mean(lead[np.abs(lead) > 1e-4] ** 2))
    t = np.arange(4 * SR) / SR
    x = np.sin(2 * np.pi * f0 * t) * rms * np.sqrt(2)
    if bursts:   # 3 Hz on / off, 40 ms ramps
        gate = (np.sin(2 * np.pi * 3 * t) > 0).astype(float)
        k = int(0.04 * SR); gate = np.convolve(gate, np.ones(k) / k, "same")
        x = x * gate
    sig = np.concatenate([lead, x]).astype(np.float32)
    y = M.mono(M.run(sig, listen=8, **params))[8 * SR + SR :]
    xin = x[SR:]
    return harmonics_db(y, f0), harmonics_db(xin, f0)

def mean_gr(x, **params):
    fi, fo, ft = (os.path.join(M.TMP, n) for n in ("g.f32", "go.f32", "gt.txt"))
    np.stack([x, x], 1).astype(np.float32).tofile(fi)
    subprocess.run([M.RAW, fi, fo, str(SR)] + [f"{k}={v}" for k, v in params.items()] + ["--listen", "8", "--trace", ft], check=True)
    gr = np.loadtxt(ft)[:, 2]
    act = gr[gr > 0.05]
    return float(act.mean()) if len(act) else 0.0, float(gr.max())

if __name__ == "__main__":
    clips = M.owner_clips()
    take = clips["take_00042244"][0]
    for label, f0, bursts, params in [("110 Hz steady, LIFT 10", 110, False, dict(lift=10)),
                                      ("110 Hz bursts, LIFT 10", 110, True, dict(lift=10)),
                                      ("220 Hz steady, LIFT 10 PUSH", 220, False, dict(lift=10, mood=1))]:
        out, inp = tone_case(take, f0, bursts, **params)
        print(f"{label:30} harmonics re fundamental: out {out:+6.1f} dB, in {inp:+6.1f} dB")
    for name, (w, n) in clips.items():
        m, mx = mean_gr(w, lift=6)
        print(f"{name:16} LIFT 6: compressor GR mean {m:4.1f} dB (active blocks), max {mx:4.1f} dB")
