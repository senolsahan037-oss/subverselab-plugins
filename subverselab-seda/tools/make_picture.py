#!/usr/bin/env python3
"""05.png (P3e): what LISTEN did on the 30 s vocal line, file 05's settings (wet only, NEW, 1/4 dotted, FB 40, LISTEN on, DUCK 60, THROW 6).
Top: dry envelope (grey), wet with LISTEN on (gold), wet with LISTEN off (thin white), 50 ms RMS in dB; shaded breath gaps and phrase-end gaps from vocal_p3e.json; teal = a throw fired, red = a throw cancelled by the returning source.
Bottom: the duck gain and the throw lift (sedaraw --gaintrace, what the engine applies). Run after tools/make_material.py --p3e. Writes <pack>/05.png."""
import os, sys, json, subprocess, tempfile
import numpy as np
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import measure as M
SR = 48000; RAW = M.RAW
PACK = os.path.abspath(os.path.join(HERE, "..", "..", "prototypes", "DINLE", "15_SEDA"))
SRC = os.path.join(HERE, "demo_material", "vocal_p3e.wav"); MARKS = os.path.join(HERE, "demo_material", "vocal_p3e.json")
BASE = ["--tail", "3", "--bpm", "120", "--note", "1/4d", "--age", "new", "--fb", "0.4", "--mix", "1.0"]

def render(extra):
    with tempfile.TemporaryDirectory() as d:
        fo, fe, ft = (os.path.join(d, n) for n in ("o.wav", "e.csv", "t.csv"))
        subprocess.run([RAW, SRC, fo] + BASE + extra + (["--eventlog", fe, "--gaintrace", ft] if "--listen" in extra else []), check=True, capture_output=True, text=True)
        y = M.read_wav(fo)[:, 0].astype(np.float64)
        if "--listen" not in extra: return y, None, None
        ev = [l.split(",") for l in open(fe).read().strip().split("\n")[1:]]; tr = np.loadtxt(ft, delimiter=",", skiprows=1)
        return y, ev, tr

def env50(y):
    k = int(0.05 * SR); n = len(y) // k
    return (np.arange(n) + 0.5) * 0.05, 10 * np.log10(np.mean(y[:n * k].reshape(n, k) ** 2, axis=1) + 1e-12)

def main():
    mk = json.load(open(MARKS)); d = M.p3e_material()[0][:, 0].astype(np.float64); d = np.concatenate([d, np.zeros(3 * SR)])
    on, ev, tr = render(["--listen", "--duck", "0.6", "--throw", "6"]); off, _, _ = render([])
    BG, FG = "#14181c", "#d8dde0"
    fig, (a1, a2) = plt.subplots(2, 1, figsize=(16, 9), dpi=100, sharex=True, gridspec_kw=dict(height_ratios=[3, 1.25], hspace=0.08), facecolor=BG)
    for a in (a1, a2):
        a.set_facecolor(BG); a.tick_params(colors=FG); [sp.set_color("#4a525a") for sp in a.spines.values()]; a.grid(color="#2a3036", lw=0.6)
    t, e_d = env50(d); _, e_on = env50(on); _, e_off = env50(off)
    top = -20.0; bot = -75.0
    a1.plot(t, np.maximum(e_d, bot), color="#8a9096", lw=1.4, label="kuru (kaynak)")
    a1.plot(t, np.maximum(e_off, bot), color="white", lw=0.8, label="islak, LISTEN kapali")
    a1.plot(t, np.maximum(e_on, bot), color="#e6b422", lw=2.0, label="islak, LISTEN acik")
    for i, g in enumerate(mk["breath_gaps"]):
        for a in (a1, a2): a.axvspan(g["t0"], g["t1"], color="#4aa3df", alpha=0.22, lw=0, label="nefes boslugu" if i == 0 and a is a1 else None)
    for i, e in enumerate(mk["phrase_ends"]):
        for a in (a1, a2): a.axvspan(e["t"], e["t"] + e["gap"], color="#e08a2e", alpha=0.13, lw=0, label="cumle sonu bosluk" if i == 0 and a is a1 else None)
    for i, e in enumerate(mk["short_ends"] + mk["word300"]):
        for a in (a1, a2): a.axvspan(e["t"], e["t"] + e["gap"], color="#a070d0", alpha=0.16, lw=0, label="kisa cumle / 300 ms bosluk (throw yok)" if i == 0 and a is a1 else None)
    th = [float(r[0]) for r in ev if r[1] == "throw_on"]; ca = [float(r[0]) for r in ev if r[1] == "throw_cancel"]
    a1.scatter(th, [top + 1.5] * len(th), marker="v", s=140, color="#1fc8b4", zorder=5, label=f"throw basladi ({len(th)})")
    a1.scatter(ca, [top + 1.5] * len(ca), marker="X", s=140, color="#e5484d", zorder=5, label=f"throw duck tarafindan iptal ({len(ca)})")
    for x in th: a1.axvline(x, color="#1fc8b4", lw=0.8, alpha=0.6)
    for x in ca: a1.axvline(x, color="#e5484d", lw=0.8, alpha=0.6)
    a1.set_ylim(bot, top + 3); a1.set_xlim(0, len(d) / SR); a1.set_ylabel("Seviye (dB, 50 ms RMS)", color=FG)
    a1.set_title("SEDA LISTEN: ne yapti (dosya 05, sadece islak; DUCK 60, THROW 6, 1/4 noktali, FB 40)", color=FG, fontsize=15, pad=10)
    a1.legend(loc="lower right", ncol=2, facecolor="#1c2228", edgecolor="#4a525a", labelcolor=FG, fontsize=10)
    a2.plot(tr[:, 0], tr[:, 1], color="#e6b422", lw=1.8, label="duck kazanci (dB)")
    a2.plot(tr[:, 0], tr[:, 2], color="#1fc8b4", lw=1.8, label="throw kazanci (dB, duck ustune)")
    for x in th: a2.axvline(x, color="#1fc8b4", lw=0.8, alpha=0.6)
    for x in ca: a2.axvline(x, color="#e5484d", lw=0.8, alpha=0.6)
    a2.axhline(0, color="#6a727a", lw=0.8); a2.set_ylim(-9, 7.5); a2.set_xlabel("Zaman (sn)", color=FG); a2.set_ylabel("Kazanc (dB)", color=FG)
    a2.legend(loc="upper right", ncol=2, facecolor="#1c2228", edgecolor="#4a525a", labelcolor=FG, fontsize=10)
    out = os.path.join(PACK, "05.png"); fig.savefig(out, facecolor=BG); plt.close(fig); print("wrote", out, f"throws {len(th)}, cancelled {len(ca)}")

if __name__ == "__main__":
    main()
