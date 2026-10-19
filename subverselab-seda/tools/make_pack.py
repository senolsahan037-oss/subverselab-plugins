#!/usr/bin/env python3
"""Builds the SEDA listening pack (TASK_P2) into 01_AI_Tools/prototypes/DINLE/15_SEDA from tools/demo_material. Runs build/sedaraw (the engine as the
plug-in will run it), level-matches every file to the dry file of its group (integrated LUFS, BS.1770 via pyloudnorm), writes olcum.json with LUFS,
true peak and the LISTEN event counts (--eventlog). Run tools/make_material.py first."""
import os, sys, json, subprocess, tempfile, csv, collections
import numpy as np
import pyloudnorm as pyln
from scipy.signal import resample_poly
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import measure as M, make_material as mm

DEMO = os.path.join(HERE, "demo_material"); SR = 48000; RAW = M.RAW
PACK = os.path.abspath(os.path.join(HERE, "..", "..", "prototypes", "DINLE", "15_SEDA"))
TAIL = 3.0   # seconds of silence after the source so the repeats ring out; the dry files are padded to the same length
# file 09 (P1d; COMPANDER 0.5, not 1: with COMPANDER 1 the loop regrows from its own noise after the 12 s decay, see STATUS.md P1d): the lead plays one phrase (0-4 s, MIX 30, FB 40, TIME 300), stops; at 4 s FEEDBACK goes to 105 %; 8-10 s TIME 300 -> 150 ms, 10-12 s back to 300; at 12 s FEEDBACK back to 40 %
OSC_DEMO = ["--age", "new", "--comp", 0.5, "--mix", 0.3, "--time-at", 0, 300, "--time-at", 8, 300, "--time-at", 10, 150, "--time-at", 12, 300, "--fb-at", 0, 0.4, "--fb-at", 4, 1.05, "--fb-at", 12, 0.4]

def lufs(x): return float(pyln.Meter(SR).integrated_loudness(np.asarray(x, dtype=np.float64)))
def true_peak(x): return float(20 * np.log10(np.max(np.abs(resample_poly(np.asarray(x, dtype=np.float64), 4, 1, axis=0))) + 1e-12))

def match_db(x, tgt):
    """Gain in dB that brings the integrated loudness of x to tgt. Bisection, not a single step: BS.1770 gating is not linear when the file is far below -70 LUFS
    (a very dark, quiet wet line gains blocks above the absolute gate as it is turned up)."""
    lo, hi = -40.0, 90.0
    for _ in range(40):
        mid = 0.5 * (lo + hi)
        if lufs(x * 10 ** (mid / 20)) < tgt: lo = mid
        else: hi = mid
    return 0.5 * (lo + hi)

def render(src, opts):
    """sedaraw on DEMO/<src>.wav; returns (audio, event counts or None, print-effective line or None)."""
    with tempfile.TemporaryDirectory() as d:
        fo, fe, fi = os.path.join(d, "o.wav"), os.path.join(d, "e.csv"), os.path.join(d, "i.wav")
        mm.write_wav24(fi, source(src))
        listen = "--listen" in opts
        r = subprocess.run([RAW, fi, fo, "--tail", str(TAIL), "--bpm", "120"] + [str(o) for o in opts]
                           + (["--eventlog", fe, "--print-effective"] if listen else []), check=True, capture_output=True, text=True)
        y = M.read_wav(fo).astype(np.float64)
        if y.shape[1] == 1: y = np.repeat(y, 2, axis=1)
        ev = None; eff = None
        if listen:
            c = collections.Counter(row["event"] for row in csv.DictReader(open(fe)))
            ev = {k: int(c.get(k, 0)) for k in ("duck", "throw_on", "throw_off", "throw_cancel", "note", "guard_on", "guard_off")}
            eff = next((l.strip() for l in r.stdout.splitlines() if l.startswith("effective:")), None)
    return y, ev, eff

def read24(src):
    d = open(os.path.join(DEMO, src + ".wav"), "rb").read()   # the 24-bit stereo file written by make_material.py (44-byte header)
    assert d[20:22] == b"\x01\x00" and d[34:36] == b"\x18\x00"
    b = np.frombuffer(d[44:], dtype=np.uint8).reshape(-1, 3).astype(np.int32)
    q = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16); q = np.where(q >= 1 << 23, q - (1 << 24), q)
    return (q / 8388607.0).reshape(-1, 2)

def source(src):
    """The 8 s drum loop is played twice (16 s; since P1d nothing needs 10 s of audio any more, kept so files 10-12 stay one length)."""
    if src == "lead4":   # file 09: the first 4 s of the lead (one phrase), 60 ms fade out, then silence to 16 s so the loop has nothing to feed on
        x = read24("lead")[:4 * SR].copy(); f = int(0.060 * SR); x[-f:] *= np.linspace(1, 0, f)[:, None]
        return np.vstack([x, np.zeros((12 * SR, 2))])
    x = read24(src)
    return np.vstack([x, x]) if src == "drums" else x

def mix_of(opts):
    """MIX the file was rendered with (sedaraw default 0.5 when no --mix is given)."""
    o = [str(v) for v in opts]; return float(o[o.index("--mix") + 1]) if "--mix" in o else 0.5

def delay_ms_of(opts):
    """The delay the first repeat sits at, read from the options (note at 120 BPM, or TIME; sedaraw default TIME 300 ms). None for a TIME sweep (file 09)."""
    o = [str(v) for v in opts]
    if "--time-to" in o or "--time-at" in o: return None
    if "--note" in o:
        import re
        base = {"1/32": 62.5, "1/16": 125.0, "1/8": 250.0, "1/4": 500.0, "1/2": 1000.0}; n = o[o.index("--note") + 1]; m = re.match(r"(1/\d+)([td]?)$", n)
        return base[m.group(1)] * {"": 1.0, "t": 2 / 3, "d": 1.5}[m.group(2)]
    return float(o[o.index("--time") + 1]) if "--time" in o else 300.0

def fb_of(opts):
    o = [str(v) for v in opts]; return float(o[o.index("--fb") + 1]) if "--fb" in o else 0.3   # sedaraw default FEEDBACK 0.3

def first_repeat(y, d, mix, T_ms, fb):
    """Level of the first repeat in the output against the dry signal, in dB (energy method), plus the regression figure and its correlation for reference.
    wet part = out - (1 - MIX) * dry (the dry path is linear), both channels, 1 .. 9 s. Energy method: the wet part holds the first repeat and the
    later ones (each about FEEDBACK x the one before), so E(first) = E(wet) x (1 - FEEDBACK^2), against the energy of the dry signal over the same stretch moved by T (so a silent gap lines up with its repeat).
    It works for ping-pong and a modulated delay, where a fixed-delay regression does not (regression: L+R sum of the wet part on the dry signal delayed by T; its correlation r says how much of
    the wet part that one copy explains). The MIX factor is inside both: an untouched repeat at MIX 30 % reads about -10.5 dB."""
    w = y - (1.0 - mix) * d; k = int(round(T_ms * SR / 1000)); a, b = 1 * SR, 9 * SR
    dd = d[a - k:b - k]; ww = w[a:b]
    e = 10 * np.log10(np.sum(ww ** 2) * (1.0 - fb ** 2) / (np.sum(dd ** 2) + 1e-30) + 1e-30)
    wm, dm = ww.sum(axis=1), dd.sum(axis=1)
    g = float(np.dot(wm, dm) / (np.dot(dm, dm) + 1e-30)); r = float(np.dot(wm, dm) / (np.sqrt(np.dot(wm, wm) * np.dot(dm, dm)) + 1e-30))
    return float(e), 20 * np.log10(abs(g) + 1e-12), r

def dry(src): return np.vstack([source(src), np.zeros((int(TAIL * SR), 2))])

def demo_09(y):
    """File 09 per second (before level matching, so the numbers are the engine's own): RMS in dBFS (L+R) and the strongest spectral line in 60 Hz - 6 kHz (Hann window, 1 s, 8x zero padding), with its level against the second's total."""
    rows = []; m = y.mean(axis=1)
    for t in range(int(len(y) / SR)):
        seg = m[t * SR:(t + 1) * SR]; rms = 10 * np.log10(np.mean(y[t * SR:(t + 1) * SR] ** 2) + 1e-20)
        sp = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), 8 * len(seg))); fr = np.fft.rfftfreq(8 * len(seg), 1.0 / SR); sel = (fr >= 60) & (fr <= 6000)
        i = int(np.argmax(sp[sel])); rows.append(dict(second=t, rms_dbfs=round(float(rms), 1), peak_hz=round(float(fr[sel][i]), 1), peak_share_db=round(float(20 * np.log10(sp[sel][i] / (np.sqrt(np.sum(sp[sel] ** 2)) + 1e-20) + 1e-20)), 1)))
    return rows

def main():
    os.makedirs(PACK, exist_ok=True)
    VOC = "--stereo mono".split()
    FBM = lambda fb, mix: ["--fb", fb, "--mix", mix]
    plan = [   # (file name, group, source, sedaraw options or None for the dry file, settings text)
        ("01_VOKAL kuru", "A", "vocal", None),
        ("02_VOKAL SEDA NEW 1-4 noktali (FB 40, MIX 30)", "A", "vocal", ["--note", "1/4d", "--age", "new"] + FBM(0.4, 0.3)),
        ("03_VOKAL SEDA OLD ayni ayar (FB 40, MIX 30)", "A", "vocal", ["--note", "1/4d", "--age", "old"] + FBM(0.4, 0.3)),
        ("04_VOKAL SEDA LISTEN acik (DUCK 60, THROW 6)", "A2", "vocal_p3e", ["--note", "1/4d", "--age", "new", "--listen", "--duck", 0.6, "--throw", 6] + FBM(0.4, 0.3)),
        ("04b_VOKAL kuru (yeni malzeme)", "A2", "vocal_p3e", None),
        ("05_VOKAL LISTEN acik, sadece islak (wet only)", "A2", "vocal_p3e", ["--note", "1/4d", "--age", "new", "--listen", "--duck", 0.6, "--throw", 6] + FBM(0.4, 1.0)),
        ("06_LEAD kuru", "B", "lead", None),
        ("07_LEAD NEW 375 ms modulasyonlu (DEPTH 40, RATE 0.8)", "B", "lead", ["--time", 375, "--age", "new", "--depth", 0.4, "--rate", 0.8] + FBM(0.4, 0.3)),
        ("08_LEAD OLD ping-pong (375 ms)", "B", "lead", ["--time", 375, "--age", "old", "--stereo", "pingpong"] + FBM(0.4, 0.3)),
        ("09_LEAD kendi kendine salinim (dur, FB 105, TIME cevir)", "B", "lead4", OSC_DEMO),
        ("10_DAVUL kuru", "C", "drums", None),
        ("11_DAVUL 1-16 NEW (MIX 20)", "C", "drums", ["--note", "1/16", "--age", "new", "--mix", 0.2, "--fb", 0.3]),
        ("12_DAVUL SOURCE=DAVUL (baslangic ayari)", "C", "drums", ["--listen", "--source", "drums"]),
        ("13_PLUCK kuru", "D", "pluck", None),
        ("14_PLUCK 1-8 ping-pong WIDE (FB 40, MIX 30)", "D", "pluck", ["--note", "1/8", "--stereo", "pingpong", "--width", 12, "--comp", 1] + FBM(0.4, 0.3)),
        ("15_PLUCK COMPANDER 0 (1-8 ping-pong WIDE, FB 55, MIX 30)", "D", "pluck", ["--note", "1/8", "--stereo", "pingpong", "--width", 12, "--comp", 0] + FBM(0.55, 0.3)),
        ("16_PLUCK COMPANDER 1 (1-8 ping-pong WIDE, FB 55, MIX 30)", "D", "pluck", ["--note", "1/8", "--stereo", "pingpong", "--width", 12, "--comp", 1] + FBM(0.55, 0.3)),
    ]
    items = []
    for name, grp, src, opts in plan:
        if opts is None: y, ev, eff = dry(src), None, None
        else: y, ev, eff = render(src, opts)
        items.append(dict(name=name, grp=grp, src=src, opts=opts, y=y, ev=ev, eff=eff))
    target = {i["grp"]: lufs(i["y"]) for i in items if i["opts"] is None}
    rows = []
    for it in items:
        x = it["y"]; l0 = lufs(x)
        if it["opts"] is not None: x = x * 10 ** (match_db(x, target[it["grp"]]) / 20)
        l1 = lufs(x); tp = true_peak(x)
        assert tp <= -1.0, (it["name"], tp)
        if it["opts"] is not None: assert abs(l1 - target[it["grp"]]) <= 0.2, (it["name"], l1, target[it["grp"]])
        mm.write_wav24(os.path.join(PACK, it["name"] + ".wav"), x)
        fr = fr_g = fr_r = wo = None; Tms = None
        if it["opts"] is not None:
            d = dry(it["src"]); mx = mix_of(it["opts"]); Tms = delay_ms_of(it["opts"])
            if Tms is not None: fr, fr_g, fr_r = first_repeat(it["y"], d, mx, Tms, fb_of(it["opts"]))
            wet = it["y"] - (1.0 - mx) * d; wo = 20 * np.log10(np.sqrt(np.mean(wet ** 2)) / np.sqrt(np.mean(d ** 2)))   # whole file incl. tail: RMS of the wet part against the dry file
        row = dict(file=it["name"] + ".wav", group=it["grp"], source=it["src"], lufs=round(l1, 2), true_peak_dbfs=round(tp, 2),
                   gain_applied_db=round(20 * np.log10(np.max(np.abs(x)) / max(np.max(np.abs(it["y"])), 1e-12)), 2) if it["opts"] is not None else 0.0,
                   sedaraw=None if it["opts"] is None else " ".join(str(o) for o in it["opts"]), listen_events=it["ev"], listen_effective=it["eff"],
                   first_repeat_db=None if fr is None else round(fr, 2), first_repeat_delay_ms=None if Tms is None else round(Tms, 1), first_repeat_regression_db=None if fr is None else round(fr_g, 2), first_repeat_correlation=None if fr is None else round(fr_r, 3),
                   wet_only_rms_vs_dry_db=None if wo is None else round(wo, 2))
        rows.append(row); print(f"{it['name']}: {l1:.2f} LUFS, TP {tp:.2f} dBFS, first repeat {row['first_repeat_db']} dB (regression {row['first_repeat_regression_db']}, r {row['first_repeat_correlation']}), wet-only RMS vs dry {row['wet_only_rms_vs_dry_db']} dB, events {it['ev']}")
    meta = dict(note="LUFS: integrated, BS.1770 (pyloudnorm). true_peak_dbfs: 4x oversampled. Every processed file is level-matched to the dry file of its group "
                     "(06 for B etc.: 01, 06, 10, 13) within 0.2 LU; gain_applied_db is that match. All files carry 3 s of tail after the source (dry files padded with silence). "
                     "listen_events: counts from sedaraw --eventlog (duck = duck state changes (in + out), throw_on/off, throw_cancel = throws cancelled by the returning source (P3e), note = note changes, guard_on/off); null where LISTEN is off. "
                     "listen_effective: what LISTEN ended up using (sedaraw --print-effective; the SOURCE selector's starting point where a knob was untouched). "
                     "first_repeat_db (P1c): level of the first repeat in the output against the dry signal, energy method on the wet part (output - (1 - MIX) x dry) over 1..9 s, both channels, x (1 - FEEDBACK^2) to leave out the later repeats, "
                     "measured on the render before level-matching; the MIX factor is inside (an untouched repeat at MIX 30 % reads about -10.5 dB). first_repeat_regression_db / first_repeat_correlation: the same from a fixed-delay regression of the L+R sum on the dry signal "
                     "delayed by first_repeat_delay_ms (r = how much of the wet part that one copy explains; low for a modulated delay, and the L+R sum reads ping-pong 6 dB low). All three null for the TIME-automated file (09) and the dry files. "
                     "wet_only_rms_vs_dry_db: RMS of the wet part over the whole file against the RMS of the dry file (check: no lower than -20 dB).", files=rows)
    low = [r["file"] for r in rows if r["wet_only_rms_vs_dry_db"] is not None and r["wet_only_rms_vs_dry_db"] < -20.0]
    meta["demo_09"] = demo_09(next(i for i in items if i["name"].startswith("09_"))["y"])
    meta["wet_only_check"] = dict(limit_db=-20.0, files_below=low); print("wet-only check (>= -20 dB against dry):", "all pass" if not low else "BELOW: " + "; ".join(low))
    json.dump(meta, open(os.path.join(PACK, "olcum.json"), "w"), indent=1, ensure_ascii=False)

if __name__ == "__main__":
    main()
