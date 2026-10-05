#!/usr/local/bin/python3
"""Listening pack v3: GLUE / LA / PUMP on a clean synthetic session (tools/synth_session.py: 48 kHz, 100 bpm, 16 bars, dry, room-less, uncompressed).

  GLUE   on the synthetic drum bus and on the full synthetic mix
  LA     on the house test vocal sitting in the synthetic backing (the vocal is processed, the backing is not)
  PUMP   on the synthetic pad + bass, keyed by the synthetic kick (the session's own kick, and a four-on-the-floor kick)

Per setting three files from one engine run: _1_esit (loudness matched to the dry, BS.1770), _2_artiGR (about 3 dB more gain reduction, matched again),
_3_flip (8 bars dry, then 8 bars of the _1_esit render), each with a GR PNG; GLUE also gets _1_esit_bar.png (one bar, dry against wet output envelope, and
the gain reduction against the v2 engine at the same 90th percentile). OKU.txt (Turkish) carries every number. Every file is read back from disk.

  dinle3.py [out_dir]     default ../prototypes/DINLE/10_TUTKAL_v3
"""
import json, os, shutil, sys, time
import numpy as np, soundfile as sf, pyloudnorm as pyln
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import measure as m
import synth_session as sy
from dinle2 import plot, flip, crest, pctl

SR = sy.SR
V2_RAW = os.path.join(HERE, "..", "build", "tutkalraw_v2")           # the engine as it was in pack v2 (reference for the before/after pictures)
FLIP_N = int(round(8 * sy.BAR * SR))
BPM = sy.BPM

# voice, source, letter, name, Turkish description, engine params, target GR p90 (dB) or None, searched control, key
SETTINGS = [
    ("GLUE", "DAVUL", "A", "Yapistir", "davul bus'i yapistirma: 4:1, atak 10 ms, AUTO release", dict(voicing=1, ratio=4, attack=10, auto=1), 4.0, "threshold", None),
    ("GLUE", "DAVUL", "B", "Yumusak", "yumusak: 2:1, atak 30 ms, release 0.6 s", dict(voicing=1, ratio=2, attack=30, release=600), 4.0, "threshold", None),
    ("GLUE", "DAVUL", "C", "Hizli", "hizli: 4:1, atak 3 ms, release 0.3 s", dict(voicing=1, ratio=4, attack=3, release=300), 4.0, "threshold", None),
    ("GLUE", "MIX", "A", "MixBus", "tum mix: 2:1, atak 30 ms, AUTO release", dict(voicing=1, ratio=2, attack=30, auto=1), 2.5, "threshold", None),
    ("GLUE", "MIX", "B", "Yapistir", "tum mix: 4:1, atak 10 ms, AUTO release", dict(voicing=1, ratio=4, attack=10, auto=1), 3.5, "threshold", None),
    ("LA", "VOKAL", "A", "Compress", "COMPRESS (~3:1), PEAK REDUCTION ile ~5 dB", dict(voicing=2, limit=0), 5.0, "pr", None),
    ("LA", "VOKAL", "B", "Limit", "LIMIT (~12:1), PEAK REDUCTION ile ~6 dB", dict(voicing=2, limit=1), 6.0, "pr", None),
    ("LA", "VOKAL", "C", "Paralel", "COMPRESS ~8 dB, MIX %50 (paralel)", dict(voicing=2, limit=0, mix=0.5), 8.0, "pr", None),
    ("PUMP", "PAD", "A", "Exp_1-8", "exp toparlanma, 1/8 nota (100 bpm = 300 ms), DEPTH 10 dB; anahtar: session kick'i", dict(voicing=3, ratio=10, attack=0.5, bpm=BPM, note=0.125, shape=0, depth=10, threshold=-30, ext=1), None, "depth", "kick"),
    ("PUMP", "PAD", "B", "Swell_1-4", "swell toparlanma, 1/4 nota (600 ms), DEPTH 14 dB; anahtar: session kick'i", dict(voicing=3, ratio=10, attack=0.5, bpm=BPM, note=0.25, shape=2, depth=14, threshold=-30, ext=1), None, "depth", "kick"),
    ("PUMP", "PAD", "C", "Klasik_4x4", "exp toparlanma, 1/8 nota, DEPTH 12 dB; anahtar: her vurusta kick (4/4)", dict(voicing=3, ratio=10, attack=0.5, bpm=BPM, note=0.125, shape=0, depth=12, threshold=-30, ext=1), None, "depth", "kick4"),
]
LABEL = {"GLUE": "VCA bus", "LA": "opto", "PUMP": "sidechain"}
HITS = None

def search(x, kw, control, target, lo, hi, rising, iters=9):
    """Bisection of one control until the 90th percentile of the gain reduction is `target` dB."""
    probe = {k: v for k, v in kw.items() if k not in ("mix", "drive")}
    for _ in range(iters):
        mid = 0.5 * (lo + hi); gr = m.run(x, want_gr=True, **{**probe, control: mid})[1]
        if (float(np.percentile(gr, 90)) > target) == rising: hi = mid
        else: lo = mid
    return 0.5 * (lo + hi)

def render(x, kw, key=None, raw=None):
    old = m.RAW
    if raw: m.RAW = raw
    try: return m.aligned(x, key=key, **kw)
    finally: m.RAW = old

def plot_bar(path, title, dry, wet, gr, gr_old, t0):
    """One bar: dry and wet output envelope (dBFS, 4 ms RMS), and the gain reduction of this engine against the v2 one at the same 90th percentile."""
    s, e = int(t0 * SR), int((t0 + sy.BAR) * SR)
    ed, ew = m.env_rms_db(dry[s:e], SR, 4.0), m.env_rms_db(wet[s:e], SR, 4.0); t = t0 + np.arange(len(ed)) / 1000.0
    fig, (a, b) = plt.subplots(2, 1, figsize=(12, 5.2), dpi=100, sharex=True, gridspec_kw=dict(height_ratios=[1.4, 1], hspace=0.12))
    a.plot(t, ed, color="#9a9a9a", lw=1.0, label="kuru"); a.plot(t, ew, color="#a4442b", lw=1.0, label="islenmis (esitlenmemis)")
    top = float(np.max(ed)); a.set_ylim(top - 50, top + 3); a.set_ylabel("cikis zarfi (dB, 4 ms RMS)"); a.grid(alpha=0.25); a.set_title(title, fontsize=10, loc="left"); a.legend(loc="upper right", fontsize=8, ncol=2)
    tg = np.arange(s, e) / SR
    if gr_old is not None: b.plot(tg, -gr_old[s:e], color="#9a9a9a", lw=1.2, ls="--", label="eski motor (v2), ayni %90 GR")
    b.plot(tg, -gr[s:e], color="#1f6f4a", lw=1.5, label="yeni GLUE (v3)")
    lim = max(float(np.max(gr[s:e])), float(np.max(gr_old[s:e])) if gr_old is not None else 0.0)
    b.set_ylim(-(lim + 1.5), 0.6); b.set_ylabel("GR (dB)"); b.set_xlabel("sn"); b.grid(alpha=0.25); b.legend(loc="lower right", fontsize=8, ncol=2)
    for t0h, _ in HITS:
        if t0 <= t0h < t0 + sy.BAR:
            for ax in (a, b): ax.axvline(t0h, color="k", lw=0.4, alpha=0.5)
    fig.savefig(path, bbox_inches="tight"); plt.close(fig)

def main():
    global HITS
    out_dir = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "..", "prototypes", "DINLE", "10_TUTKAL_v3"))
    os.makedirs(out_dir, exist_ok=True); t_start = time.time()
    d = sy.build(); S = d["stems"]; HITS = m.hits_of(d["events"])
    rows, fails, st, mixst, vg = sy.sanity(S, d["events"])
    sess = os.path.join(out_dir, "_session"); sy.write(sess)
    json.dump(dict(stems=st, mixes=mixst, events=len(d["events"]), session_gain_db=d["gain_db"], vocal_gain_db=vg, fails=fails, checks=[(a, bool(b_), c) for a, b_, c in rows]), open(os.path.join(sess, "sanity.json"), "w"), indent=1)
    print(f"session: {len(rows)} sanity checks, {len(fails)} failed {fails}", flush=True)

    drums = sy.drumbus(S); mix = sy.fullmix(S); pb = sy.padbass(S); vmix, voc, bk, vgain = sy.vocal_in_backing(S)
    keys = {"kick": S["kick"], "kick4": sy.four_floor(S)}
    meter = pyln.Meter(SR)
    SRC = {"DAVUL": (drums, "sentetik davul bus'i: kick + snare + hat + tom (kuru, tek tek sentezlendi, 16 bar, 100 bpm)"),
           "MIX": (mix, "sentetik tam mix: davul + bas + pad + lead (kuru)"),
           "VOKAL": (voc, "ev test vokali (Kadin_vokal_20s, gercek sarki; 19.2 sn, iki kez art arda) sentetik fon icinde (pad + bas + yumusak davul); vokal islenir, fon islenmez"),
           "PAD": (pb, "sentetik pad + bas, kick anahtarli (kick islenmez, miksin icinde duyulur)")}

    groups = {}
    for voice, src, letter, name, desc, kw, target, control, keyname in SETTINGS:
        x, _ = SRC[src]; t0 = time.time()
        if control == "depth":
            key = keys[keyname]; ref = meter.integrated_loudness(x.astype(np.float64)); dry_full = x + key
            def make(depth):
                y, gr = render(x, {**kw, "depth": depth}, key=key); g = ref - meter.integrated_loudness(y.astype(np.float64))
                return (y * m.amp(g)).astype(np.float32), gr, g
            y1, gr1, g1 = make(kw["depth"]); y2, gr2, g2 = make(kw["depth"] + 3.0)
            wet1, wet2 = y1 + key, y2 + key; ctl1, ctl2 = f"DEPTH {kw['depth']:.0f} dB", f"DEPTH {kw['depth'] + 3:.0f} dB"; old = None
        else:
            ref = meter.integrated_loudness(x.astype(np.float64)); dry_full = vmix if src == "VOKAL" else x
            lo, hi, rising = (-60.0, 0.0, False) if control == "threshold" else (0.0, 1.0, True)
            c1 = round(search(x, kw, control, target, lo, hi, rising), 3 if control == "pr" else 1); c2 = round(search(x, kw, control, target + 3.0, lo, hi, rising), 3 if control == "pr" else 1)
            fmt = (lambda c: f"threshold {c} dB") if control == "threshold" else (lambda c: f"peak reduction {c:.3f}")
            ctl1, ctl2 = fmt(c1), fmt(c2)
            def fin(k):
                y, gr = render(x, k); g = ref - meter.integrated_loudness(y.astype(np.float64)); y = (y * m.amp(g)).astype(np.float32)
                return (y + bk if src == "VOKAL" else y), gr, g
            y1, gr1, g1 = fin({**kw, control: c1}); y2, gr2, g2 = fin({**kw, control: c2}); wet1, wet2 = y1, y2
            old = None
            if voice == "GLUE":                          # the v2 engine at the same 90th percentile (its default: no SC HPF), for the before/after picture
                kw0 = {**kw, "hpf": 0}; m.RAW, keep = V2_RAW, m.RAW
                try: c_old = round(m.search_p90(x, kw0, target), 2); _, gr_old = m.aligned(x, threshold=c_old, **kw0)
                finally: m.RAW = keep
                wet_old, _ = render(x, {**kw0, "threshold": c_old}, raw=V2_RAW); old = (c_old, gr_old, wet_old)
        gr_stats = lambda g: (pctl(g, 90), float(np.max(g)), float(np.mean(g)))
        groups.setdefault((voice, src), []).append(dict(letter=letter, name=name, desc=desc, kw=kw, ctl=(ctl1, ctl2), wet=(wet1, wet2), gr=(gr1, gr2), gain=(g1, g2), dry=dry_full, old=old,
                                                         stats=(gr_stats(gr1), gr_stats(gr2)), src=src, voice=voice, raw_wet=(None if control == "depth" else None)))
        print(f"  {voice} {src} {letter} {name}: {ctl1} / {ctl2}; GR p90 {pctl(gr1, 90):.1f} / {pctl(gr2, 90):.1f} dB ({time.time() - t0:.0f} s)", flush=True)

    # ---------------- write files, PNGs, OKU
    lines = ["TUTKAL — dinleme paketi v3. Motor prototipi; arayuz ve plugin henuz yok.",
             "TUM KAYNAKLAR SENTEZ (tools/synth_session.py): 48 kHz, 100 bpm, 16 bar (38.4 sn), kuru, oda yok, kompresor yok. Her vurusun hizi (velocity) farkli, birkac ms insan zamanlamasi var.",
             "Parcalar: kick, snare (govde + tel gurultusu), hat (kapali/acik), tom (bar 8 ve 16 sonunda fill), bas (sub + bas), pad (akor), lead (pluck, 5. bardan). Kuru parcalar _session/ klasorunde.",
             "Vokal: ev test vokali (gercek sarki, 3 sn'lik sessizlikler dahil) sentetik fonun (pad + bas + yumusak davul) ustunde; vokal seviye dalgasi ~14 dB (p90-p10). Vokal iki kez calar: ilk 8 bar kuru/ayarli, sonraki 8 bar ayni vokal.",
             "Her ayarin uc dosyasi var:  _1_esit = ayar, kuruyla ayni ses yuksekligi (BS.1770 LUFS; LA'da yalniz VOKAL esitlenir, PUMP'ta yalniz PAD+BAS esitlenir)",
             "                            _2_artiGR = ayni ayar, ~3 dB daha fazla gain reduction (threshold / peak reduction asagi, PUMP'ta DEPTH +3 dB), yine esitlenmis",
             "                            _3_flip = ilk 8 bar KURU, sonraki 8 bar ayni ayarin islenmis hali (_1_esit'in 9-16. barlari). Gecis 5 ms.",
             "Her dosyanin yaninda .png: ustte GR (dB, asagi = daha cok sikisma), altta kuru ve islenmis tepe seviyesi. GLUE'da ayrica _1_esit_bar.png: tek bar, kuru ve islenmis cikis zarfi + eski motor (v2) ile GR karsilastirmasi.",
             "Dosyalarin latency'si (63 ornek) cikarildi: kuru ile ayni zaman cizgisinde. GR = gain reduction; 'GR %90' vuruslardaki tipik sikistirma; crest = tepe - RMS.", ""]
    written = []; glue_nums = []
    for (voice, src), items in groups.items():
        folder = os.path.join(out_dir, voice); os.makedirs(folder, exist_ok=True)
        dryref = items[0]["dry"]; srcdesc = SRC[src][1]
        files = [(f"{voice}_{src}_0_KURU", dryref, None)]
        for it in items:
            stem = f"{voice}_{src}_{it['letter']}_{it['name']}"
            files += [(stem + "_1_esit", it["wet"][0], (it, 0)), (stem + "_2_artiGR", it["wet"][1], (it, 1)), (stem + "_3_flip", flip(dryref, it["wet"][0], FLIP_N, SR), (it, 2))]
        peak = max(float(np.max(np.abs(a))) for _, a, _ in files); common = min(1.0, m.amp(-1.0) / peak)
        lines.append(f"== {voice} ({LABEL[voice]}), kaynak {src}: {srcdesc}. Grup kazanci {m.db(common):+.1f} dB (tepe -1 dBFS'in altinda kalsin diye tek kazanc, kuru dahil). Kuru crest {crest(dryref):.1f} dB.")
        for stem, a, info in files:
            path = os.path.join(folder, stem + ".wav"); sf.write(path, a * common, SR, subtype="PCM_24"); written.append((path, a * common, dryref * common, info, stem, voice, src))
            if info is None:
                lines.append(f"{voice}/{stem}.wav: islenmemis referans" + (" (pad + bas + kick, kompresorsuz)." if src == "PAD" else (" (vokal + fon)." if src == "VOKAL" else ".")))
                continue
            it, kind = info; (p90a, mxa, mna), (p90b, mxb, mnb) = it["stats"]
            if kind == 0:
                plot(os.path.join(folder, stem + ".png"), f"{stem}  |  {it['ctl'][0]}  |  GR %90 {p90a:.1f} dB, tepe {mxa:.1f}", it["gr"][0], dryref, a, SR)
                extra = ""
                if voice == "GLUE":
                    raw, gr_raw = render(SRC[src][0], {**it["kw"], "threshold": float(it["ctl"][0].split()[1])})
                    dip, reb = m.shadow_stats(SRC[src][0], raw, HITS); rp = m.ripple_stats(gr_raw, HITS)
                    c_old, gr_old, wet_old = it["old"]; dip0, reb0 = m.shadow_stats(SRC[src][0], wet_old, HITS); rp0 = m.ripple_stats(gr_old, HITS)
                    plot_bar(os.path.join(folder, stem + "_bar.png"), f"{stem}  |  tek bar (6. bar): kuru / islenmis cikis zarfi, GR yeni vs eski motor  |  {it['ctl'][0]}", SRC[src][0], raw, gr_raw, gr_old, 5 * sy.BAR)
                    glue_nums.append((stem, np.mean(dip0), np.percentile(dip0, 95), dip0.max(), np.mean(dip), np.percentile(dip, 95), dip.max(), np.mean(rp0), np.percentile(rp0, 95), rp0.max(), np.mean(rp), np.percentile(rp, 95), rp.max(), reb.max(), crest(SRC[src][0]), crest(raw)))
                    extra = (f" GOLGE (vurustan sonra cikisin kuru zarfin altina itilmesi, vurus basina): yeni ort {np.mean(dip):.2f} / p95 {np.percentile(dip, 95):.2f} / maks {dip.max():.2f} dB (eski motor {np.mean(dip0):.2f} / {np.percentile(dip0, 95):.2f} / {dip0.max():.2f});"
                             f" DALGA (vurus ustu GR - sonraki vurus oncesi GR): yeni ort {np.mean(rp):.2f} / p95 {np.percentile(rp, 95):.2f} / maks {rp.max():.2f} dB (eski {np.mean(rp0):.2f} / {np.percentile(rp0, 95):.2f} / {rp0.max():.2f}); ikinci tümsek maks {reb.max():.2f} dB;"
                             f" crest kuru {crest(SRC[src][0]):.1f} -> yeni {crest(raw):.1f} dB.")
                lines.append(f"{voice}/{stem}.wav: {it['desc']}. [{it['ctl'][0]}] GR %90 {p90a:.1f} dB, tepe {mxa:.1f} dB, ortalama {mna:.1f} dB; crest {crest(a):.1f} dB; esitleme kazanci {it['gain'][0]:+.1f} dB.{extra}")
            elif kind == 1:
                plot(os.path.join(folder, stem + ".png"), f"{stem}  |  {it['ctl'][1]}  |  GR %90 {p90b:.1f} dB, tepe {mxb:.1f}", it["gr"][1], dryref, a, SR, gr_ref=it["gr"][0])
                lines.append(f"{voice}/{stem}.wav: ayni ayar, ~3 dB fazla GR. [{it['ctl'][1]}] GR %90 {p90b:.1f} dB, tepe {mxb:.1f} dB, ortalama {mnb:.1f} dB; crest {crest(a):.1f} dB; esitleme kazanci {it['gain'][1]:+.1f} dB.")
            else:
                plot(os.path.join(folder, stem + ".png"), f"{stem}  |  ilk 8 bar kuru, sonra {it['ctl'][0]}", it["gr"][0], dryref, a, SR, flip_at=FLIP_N)
                lines.append(f"{voice}/{stem}.wav: flip: 0-{FLIP_N / SR:.1f} sn (8 bar) kuru, {FLIP_N / SR:.1f}-{len(a) / SR:.1f} sn (8 bar) islenmis ({it['ctl'][0]}).")
        lines.append("")

    # ---------------- read every file back
    print("\nread-back ...", flush=True)
    rb, bad = [], []
    for path, audio, dry, info, stem, voice, src in written:
        y, f2 = sf.read(path, dtype="float64", always_2d=True)
        fin = bool(np.all(np.isfinite(y))); pk = float(m.db(np.max(np.abs(y)))); me = pyln.Meter(f2); L = me.integrated_loudness(y)
        L0 = me.integrated_loudness(dry.astype(np.float64)) if info is not None else L
        frame = int(0.1 * f2); k = len(y) // frame
        r_y = np.sqrt(np.mean(y[:k * frame].reshape(k, frame, -1) ** 2, axis=(1, 2))); r_d = np.sqrt(np.mean(dry.astype(np.float64)[:k * frame].reshape(k, frame, -1) ** 2, axis=(1, 2)))
        live = r_d > m.amp(-50.0)
        drop = float(np.max(m.db(r_d[live]) - m.db(np.maximum(r_y[live], 1e-12)))) if live.any() else 0.0
        vanish = bool(np.any(live & (r_y < m.amp(-65.0))))
        tol = None if (info is None or src == "PAD") else {"VOKAL": 1.0}.get(src, 0.3 if info[1] == 2 else 0.15)
        ok = fin and pk < -1.0 and (tol is None or abs(L - L0) <= tol) and not vanish and len(y) == len(dry)
        rb.append((voice, stem, fin, pk, L - L0, drop, vanish, ok));
        if not ok: bad.append(stem)
    for r in rb: print(f"  {r[1]:42s} finite {r[2]}  peak {r[3]:6.2f} dBFS  loudness vs dry {r[4]:+.2f} LU  max 100 ms drop {r[5]:5.1f} dB  vanish {r[6]}  {'OK' if r[7] else 'BAD'}")
    print(f"read-back: {len(rb)} files, {len(bad)} bad {bad}")

    lines += ["== GLUE: ONCE / SIMDI (ayni %90 GR'de, ayni davul bus'i, eski motor = v2: SC HPF kapali, per-hit duck)",
              "   golge = vurustan sonra cikis zarfinin kuru zarfa gore vurus oncesinden ne kadar daha asagi itildigi (dB); dalga = vurus ustu GR - bir sonraki vurus oncesi GR (dB). Olcum: measure.py G4/G5."]
    for g in glue_nums:
        lines.append(f"   {g[0]}: golge ort/p95/maks {g[1]:.2f}/{g[2]:.2f}/{g[3]:.2f} -> {g[4]:.2f}/{g[5]:.2f}/{g[6]:.2f} dB;  dalga ort/p95/maks {g[7]:.2f}/{g[8]:.2f}/{g[9]:.2f} -> {g[10]:.2f}/{g[11]:.2f}/{g[12]:.2f} dB;  ikinci tumsek maks (yeni) {g[13]:.2f} dB;  crest {g[14]:.1f} -> {g[15]:.1f} dB")
    lines += ["", "== DINLEME REHBERI (neye kulak ver)",
              "GLUE (DAVUL / MIX): v2'de 'bir kendisi bir golgesi' vardi: her vurusun arkasinda kendi bastirilmis golgesi. Simdi: (1) vurus sonrasi boslukta 'cukur' yok mu; (2) GR vurus aralarinda sifira donmuyor, bar boyunca surekli (PNG'de yesil cizgi duz, gri kesikli eski motor sallaniyor);",
              "  (3) kick tek basina surmuyor: GLUE'nun varsayilan SC HPF'i 200 Hz; (4) vurusun ilk ms'leri geciyor (snap) ama arkasinda cukur birakmiyor; (5) bar sonunda kuyruk (AUTO / release) nefes aliyor mu; (6) _2_artiGR: ayni his daha belirgin mi, yoksa ezilme mi basliyor?",
              "  Not: GR surekli oldugu icin crest dusmez, hafifce yukselir (vurus gecer, govde biraz asagida). Davul parcalari 'tek sey' gibi oturuyor mu, yoksa yine ayri ayri mi duyuluyor — bu karari yalniz sen verebilirsin.",
              "LA (VOKAL fonun icinde): vokal seviyelendi ama 'yakalanmadi' mi? Fon calarken vokalin sessiz bolumleri (-30 dB civari) one cikiyor mu, yuksek bolumler geri cekiliyor mu? (1) Compress: yumusak, nefeste yukselme; (2) hizli tepeden sonra ilk yari ~60 ms'de geri gelir, kalan saniyelerce;",
              "  (3) Limit: daha siki, tepeler tutulu; (4) cift harmonik sicaklik (~%0.6 THD @ 7 dB GR); (5) Paralel (MIX %50). Ikinci 8 bar ayni vokal: _3_flip'te ilk 8 bar kuru, ikinci 8 bar islenmis — ayni cumleleri karsilastirirsin.",
              f"PUMP (PAD + BAS, kick anahtari, {BPM:.0f} bpm): (1) Exp 1/8: kick'ten sonra hizli doner, 300 ms'de %90 geri; (2) Swell 1/4: pad gec gelip yukselir (600 ms); (3) Klasik_4x4: her vurusta kick, klasik pompa; (4) _2_artiGR: DEPTH +3 dB; (5) _3_flip: 8 bar kuru, 8 bar pompa.",
              "Karar: 'ses tamam' ya da yon (daha sert / daha yumusak / daha hizli / daha cok renk / pump daha derin). Ayar motorda yapilir, paket yenilenir.",
              "", "Dogrulanmayan: kulakla hicbir sey dinlenmedi. Renk miktarlari ve sureler olcumle yerlestirildi, begeniyle degil. Sentetik davul gercek davul degildir: yumusak/sert tonu ayri degerlendir."]
    s_ok = sum(1 for _, ok, _ in rows if ok)
    lines += ["", f"== SESSION SAGLIGI ({s_ok}/{len(rows)} kontrol gecti; _session/sanity.json)"] + [f"{'tamam' if ok else 'SORUN'} {a}: {c}" for a, ok, c in rows]
    lines += ["", "== OKU-GERI (dosyalar diskten geri okundu)"] + [f"{r[1]}: sonlu {r[2]}, tepe {r[3]:.2f} dBFS, kuruya gore {r[4]:+.2f} LU, 100 ms karede en buyuk dusus {r[5]:.1f} dB, kaybolan kare {r[6]} -> {'tamam' if r[7] else 'SORUN'}" for r in rb]
    open(os.path.join(out_dir, "OKU.txt"), "w").write("\n".join(lines) + "\n")
    json.dump(dict(readback=[dict(zip(("voice", "stem", "finite", "peak_db", "lu_vs_dry", "max_drop_db", "vanish", "ok"), r)) for r in rb],
                   glue=[dict(zip(("stem", "dip_old_mean", "dip_old_p95", "dip_old_max", "dip_mean", "dip_p95", "dip_max", "ripple_old_mean", "ripple_old_p95", "ripple_old_max", "ripple_mean", "ripple_p95", "ripple_max", "second_hump_max", "crest_dry", "crest_wet"), map(lambda v: v if isinstance(v, str) else float(v), g))) for g in glue_nums]),
              open(os.path.join(out_dir, "readback.json"), "w"), indent=1)
    shutil.rmtree(m.TMP, ignore_errors=True)
    print(f"\nyazildi: {out_dir}  ({time.time() - t_start:.0f} s)")
    return len(bad)

if __name__ == "__main__":
    sys.exit(main())
