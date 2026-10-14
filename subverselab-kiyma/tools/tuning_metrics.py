"""Before/after tables for docs/DSP_TUNING_v2.md on the drum loop: python3 tools/tuning_metrics.py <render_before> <render_after> <drum_loop.wav> <outdir>
Renders through both `render` binaries (the CLI aligns the output to the input), then prints the hole metric and the loudness / BRUTAL tables."""
import subprocess, sys, wave, os
import numpy as np

def load(p):
    w = wave.open(p); n = w.getnframes(); sw = w.getsampwidth(); fr = w.readframes(n); ch = w.getnchannels()
    if sw == 3:
        b = np.frombuffer(fr, np.uint8).reshape(-1, 3); x = (b[:, 0].astype(np.int32) | (b[:, 1].astype(np.int32) << 8) | (b[:, 2].astype(np.int32) << 16)); x = np.where(x >= 2**23, x - 2**24, x) / 2**23
    elif sw == 2: x = np.frombuffer(fr, np.int16) / 32768
    else: x = np.frombuffer(fr, np.int32) / 2**31
    return x.reshape(-1, ch), w.getframerate()

def render(exe, src, dst, **kw):
    subprocess.run([exe, src, dst] + [f"{k}={v}" for k, v in kw.items()], capture_output=True, check=True)
    return load(dst)[0]

def hits_of(x, sr):
    hop = int(0.01 * sr); fr = len(x) // hop
    env = np.array([np.sqrt(np.mean(x[i*hop:(i+1)*hop]**2)) for i in range(fr)])
    hits, last = [], -1000
    for f in range(10, fr):
        if env[f] > 0.01 and env[f] > 3 * env[f-10:f].max() and f - last >= 9:
            hits.append(f * hop); last = f
    return hits

def holes(dry, out, sr, hits, beat_only):
    win = int(0.06 * sr); beat = sr * 0.5; tot = ok = 0
    for h in hits:
        if h < 2 * sr or h + win >= len(dry): continue
        if beat_only and abs(((h / beat + 0.5) % 1.0) - 0.5) * beat >= 0.04 * sr: continue
        tot += 1; ed = np.sum(dry[h:h+win]**2); eo = np.sum(out[h:h+win]**2); ok += eo >= ed * 10**(-1.2)
    return 100.0 * ok / max(tot, 1), tot

def rms_db(x): return 20 * np.log10(np.sqrt(np.mean(x**2)) + 1e-12)
def crest_db(x): return 20 * np.log10(np.abs(x).max()) - rms_db(x)

before, after, src, outdir = sys.argv[1:5]
os.makedirs(outdir, exist_ok=True)
dry, sr = load(src); hits = hits_of(dry, sr)
base = dict(glitch=0, brutal=0, envelope=0, offset=0, crank=0, output=0)
print(f"drum loop: {len(dry)/sr:.1f} s, {len(hits)} detected hits, {sum(1 for h in hits if h>=2*sr and abs(((h/(sr*0.5)+0.5)%1.0)-0.5)*sr*0.5 < 0.04*sr)} on a beat after 2 s")
print("\nHOLES: share of dry hits whose 60 ms still has output energy within 12 dB (mix 1, brutal 0)")
print("glitch | on-beat hits before / after | all hits before / after")
for g in (0.2, 0.5, 1.0):
    row = []
    for exe, tag in ((before, 'b'), (after, 'a')):
        o = render(exe, src, f"{outdir}/h_{tag}_{g}.wav", **{**base, 'glitch': g, 'mix': 1})
        row.append((holes(dry, o, sr, hits, True), holes(dry, o, sr, hits, False)))
    print(f"  {g:.1f}  |  {row[0][0][0]:5.1f} % -> {row[1][0][0]:5.1f} % ({row[1][0][1]} hits) |  {row[0][1][0]:5.1f} % -> {row[1][1][0]:5.1f} % ({row[1][1][1]} hits)")
print("\nLOUDNESS: RMS of the output against dry, dB (glitch 0, brutal 0)")
for exe, tag in ((before, 'before'), (after, 'after')):
    print(f"  {tag}: " + "  ".join(f"mix {m:.2f}: {rms_db(render(exe, src, f'{outdir}/l_{tag}.wav', **{**base, 'mix': m})) - rms_db(dry):+.2f}" for m in (0, 0.25, 0.5, 0.75, 1.0)))
print("\nBRUTAL: RMS against brutal 0, dB, and crest factor (glitch 0, mix 1)")
for exe, tag in ((before, 'before'), (after, 'after')):
    ref = render(exe, src, f'{outdir}/b0_{tag}.wav', **{**base, 'mix': 1})
    cells = []
    for b in (0.1, 0.25, 0.5, 0.75, 1.0):
        o = render(exe, src, f'{outdir}/b_{tag}.wav', **{**base, 'mix': 1, 'brutal': b})
        cells.append(f"{b:.2f}: {rms_db(o) - rms_db(ref):+.2f} dB crest {crest_db(o):.1f}")
    print(f"  {tag} (dry crest {crest_db(dry):.1f} dB, brutal 0 crest {crest_db(ref):.1f}): " + "  |  ".join(cells))
