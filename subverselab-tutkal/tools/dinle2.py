#!/usr/local/bin/python3
"""Listening pack v2: one pack per VOICING (GLUE, LA, PUMP) on the same real recordings (no separated stems).

Per setting three files, all written from the same engine run (tools/tutkalraw):
  _1_esit    the setting, loudness matched to the dry (BS.1770), latency removed
  _2_artiGR  the same setting with about 3 dB more gain reduction (threshold / peak reduction lowered, DEPTH +3 dB), matched again
  _3_flip    the dry for one half, the _1_esit render for the other half (5 ms cross-fade at the switch)
and a GR-curve PNG for each. OKU.txt (Turkish) says what to listen for and carries every number.

  dinle2.py [out_dir]     default ../prototypes/DINLE/10_TUTKAL_v2
"""
import glob, json, os, shutil, sys
import numpy as np, soundfile as sf, pyloudnorm as pyln
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import measure as m

BPM = 165.0                                   # READY DRUM BOX loop: beat 0.3636 s, the loop repeats every 4 bars (5.817 s)
BEAT = 60.0 / BPM
GRID0 = 0.0085                                # first bar line: the kick channel's first strong onset (1.463 s) minus 4 beats
DRUMS = "~/Desktop/solo/READY DRUM BOX/Samples/Processed/Bounce/Bounce *.wav"
KICK = "~/Desktop/solo/READY DRUM BOX/Samples/Processed/Bounce/Bounce K*CK [[]2025-09-26 021057[]]-2.wav"
MIXF = "~/Desktop/Export/Static Ritual.wav"
VOCF = "~/Desktop/mali demo  Project/Samples/Recorded/4-Audio 0002 [[]2026-09-27 224148[]].wav"

# voice, source tag, letter, name, Turkish description, engine params, target GR p90 (dB) or None, searched control
SETTINGS = [
    ("GLUE", "DAVUL", "A", "Yapistir", "davul bus'i yapistirma: 4:1, atak 10 ms, AUTO release", dict(voicing=1, ratio=4, attack=10, auto=1), 4.0, "threshold"),
    ("GLUE", "DAVUL", "B", "Sikica", "hizli ve siki: 10:1, atak 1 ms, release 0.1 s (atak tabani vurusu geciriyor)", dict(voicing=1, ratio=10, attack=1, release=100), 6.0, "threshold"),
    ("GLUE", "MIX", "C", "MixBus", "mix bus: 2:1, atak 30 ms, AUTO release", dict(voicing=1, ratio=2, attack=30, auto=1), 2.5, "threshold"),
    ("LA", "VOKAL", "A", "Compress", "COMPRESS (~3:1), PEAK REDUCTION ile ~5 dB", dict(voicing=2, limit=0), 5.0, "pr"),
    ("LA", "VOKAL", "B", "Limit", "LIMIT (~12:1), PEAK REDUCTION ile ~6 dB", dict(voicing=2, limit=1), 6.0, "pr"),
    ("LA", "VOKAL", "C", "Paralel", "COMPRESS ~8 dB, MIX %50 (paralel)", dict(voicing=2, limit=0, mix=0.5), 8.0, "pr"),
    ("PUMP", "PAD", "A", "Exp_1-8", "exp toparlanma, 1/8 nota (165 bpm = 182 ms), DEPTH 10 dB", dict(voicing=3, ratio=10, attack=0.5, bpm=BPM, note=0.125, shape=0, depth=10, threshold=-30, ext=1), None, "depth"),
    ("PUMP", "PAD", "B", "Swell_1-4", "swell toparlanma, 1/4 nota (364 ms), DEPTH 14 dB", dict(voicing=3, ratio=10, attack=0.5, bpm=BPM, note=0.25, shape=2, depth=14, threshold=-30, ext=1), None, "depth"),
]
LABEL = {"GLUE": "VCA bus", "LA": "opto", "PUMP": "sidechain"}

def crest(x): return float(m.db(np.max(np.abs(x))) - m.db(np.sqrt(np.mean(x ** 2))))

def read_sum(pattern):
    paths = sorted(glob.glob(os.path.expanduser(pattern)))
    if not paths: raise SystemExit(f"no file matches {pattern}")
    total, sr = None, None
    for p in paths:
        x, fs = sf.read(p, dtype="float32", always_2d=True)
        if x.shape[1] == 1: x = np.repeat(x, 2, axis=1)
        x = x[:, :2]
        if total is None: total, sr = x.copy(), fs
        else:
            if fs != sr: raise SystemExit(f"{p}: sample rate differs")
            n = min(len(total), len(x)); total = total[:n] + x[:n]
    return total, sr, [os.path.basename(p) for p in paths]

def bar_window(x, sr, bars=8, loop_bars=4):
    """Start on a bar line, a whole number of loops in, length `bars` bars; the loudest such window."""
    n = int(round(bars * 4 * BEAT * sr)); hop = loop_bars * 4 * BEAT; best, at = -1.0, 0.0
    k = 0
    while GRID0 + k * hop + bars * 4 * BEAT < len(x) / sr:
        s = int(round((GRID0 + k * hop) * sr)); e = float(np.mean(x[s:s + n] ** 2))
        if e > best: best, at = e, GRID0 + k * hop
        k += 1
    s = int(round(at * sr)); return x[s:s + n].copy(), at

def loud_window(x, sr, secs):
    n = int(secs * sr); hop = int(2 * sr); best, at = -1.0, 0
    for s in range(0, max(1, len(x) - n), hop):
        e = float(np.mean(x[s:s + n] ** 2))
        if e > best: best, at = e, s
    return x[at:at + n].copy(), at / sr

def to_peak(x, db=-6.0): return (x * m.amp(db) / float(np.max(np.abs(x)))).astype(np.float32)

def synth_pad(secs, sr, seed=11):
    """A sustained Am9 pad, made here (no real pad recording on this Mac can be opened outside Live): band-limited
    detuned saws, two-pole low-pass at 1.4 kHz, slow filter-free chorus between the channels."""
    rng = np.random.default_rng(seed); n = int(secs * sr); t = np.arange(n) / sr; out = np.zeros((n, 2), np.float64)
    for f0 in (110.0, 164.81, 220.0, 261.63, 329.63, 493.88):
        for c in range(2):
            for det in (-0.12, 0.0, 0.11):
                f = f0 * 2 ** ((det + 0.04 * (c - 0.5)) / 12.0); ph = rng.uniform(0, 2 * np.pi)
                lfo = 1.0 + 0.0015 * np.sin(2 * np.pi * (0.13 + 0.05 * c) * t + ph)
                arg = 2 * np.pi * np.cumsum(f * lfo) / sr
                for h in range(1, int(5000 / f) + 1):
                    out[:, c] += np.sin(h * arg + ph) / h * (1.0 / (1.0 + (h * f / 1400.0) ** 4))
    out /= np.max(np.abs(out))
    return out.astype(np.float32)

# -----------------------------------------------------------------------------------------------------------------
def pctl(gr, q): return float(np.percentile(gr, q))

def render(x, sr, kw, key=None):
    """The engine, with its latency taken out so the result lines up with the input."""
    y, gr = m.run(x, sr=sr, key=key, **kw)
    return np.concatenate([y[m.LAT:], np.zeros((m.LAT, 2), np.float32)]), gr

def search(x, sr, kw, control, target, lo, hi, rising):
    """Bisection of one control until the 90th percentile of the gain reduction is `target`. rising: more control = more gain reduction."""
    for _ in range(12):
        mid = 0.5 * (lo + hi)
        probe = {k: v for k, v in kw.items() if k not in ("mix", "drive")}
        gr = m.run(x, sr=sr, want_gr=True, **{**probe, control: mid})[1]
        over = pctl(gr, 90) > target
        if over == rising: hi = mid
        else: lo = mid
    return 0.5 * (lo + hi)

def flip(dry, wet, n, sr):
    out = np.concatenate([dry[:n], wet[n:]]).copy(); h = int(0.0025 * sr); w = np.linspace(0, 1, 2 * h, dtype=np.float32)[:, None]
    out[n - h:n + h] = dry[n - h:n + h] * (1 - w) + wet[n - h:n + h] * w
    return out

def env_db(x, sr, ms=20):
    n = int(ms * sr / 1000); k = len(x) // n
    return m.db(np.max(np.abs(x[:k * n]).reshape(k, n, -1), axis=(1, 2))), (np.arange(k) + 0.5) * ms / 1000

def plot(path, title, gr, dry, wet, sr, flip_at=None, gr_ref=None, label_wet="islenmis"):
    fig, (a, b) = plt.subplots(2, 1, figsize=(12, 4.2), dpi=100, sharex=True, gridspec_kw=dict(height_ratios=[1.3, 1], hspace=0.12))
    t = np.arange(len(gr)) / sr
    if gr_ref is not None: a.plot(t, -gr_ref, color="#b0b0b0", lw=1.0, label="ana ayar")
    a.plot(t, -gr, color="#1f6f4a", lw=1.3, label="GR")
    a.set_ylabel("GR (dB)"); a.set_ylim(-(max(float(np.max(gr)), float(np.max(gr_ref)) if gr_ref is not None else 0) + 2), 0.8); a.grid(alpha=0.25); a.set_title(title, fontsize=10, loc="left")
    ed, te = env_db(dry, sr); ew, _ = env_db(wet, sr)
    b.plot(te, ed, color="#9a9a9a", lw=1.0, label="kuru"); b.plot(te, ew, color="#a4442b", lw=1.0, label=label_wet)
    b.set_ylabel("tepe (dBFS)"); b.set_xlabel("sn"); b.set_ylim(-60, 0); b.grid(alpha=0.25)
    for ax in (a, b):
        if flip_at is not None: ax.axvline(flip_at / sr, color="k", lw=0.8, ls="--")
    if flip_at is not None:
        a.axvspan(0, flip_at / sr, color="#000", alpha=0.07); a.text(0.01, 0.08, "KURU (kompresor duyulmuyor)", transform=a.transAxes, fontsize=8)
        a.text(flip_at / sr / (len(gr) / sr) + 0.01, 0.08, "ISLENMIS", transform=a.transAxes, fontsize=8)
    b.legend(loc="lower right", fontsize=8, ncol=2); a.legend(loc="upper right", fontsize=8, ncol=2)
    fig.savefig(path, bbox_inches="tight"); plt.close(fig)

def main():
    out_dir = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "prototypes", "DINLE", "10_TUTKAL_v2"))
    os.makedirs(out_dir, exist_ok=True)
    seg_bars = 4; seg_s = seg_bars * 4 * BEAT

    drums, sr, dnames = read_sum(DRUMS); davul, d_at = bar_window(drums, sr, bars=2 * seg_bars); davul = to_peak(davul)
    kick_full, ksr, knames = read_sum(KICK); assert ksr == sr
    s0 = int(round(d_at * sr)); kick = to_peak(kick_full[s0:s0 + len(davul)])
    mix_full, msr, _ = read_sum(MIXF); mix, mix_at = loud_window(mix_full, msr, 16.0); mix = to_peak(mix)
    voc_full, vsr, _ = read_sum(VOCF); voc, voc_at = loud_window(voc_full, vsr, 16.0); voc = to_peak(voc)
    pad = to_peak(synth_pad(len(davul) / sr, sr), -9.0)
    SRC = {"DAVUL": (davul, sr, seg_s, f"{len(dnames)} kanalin toplami (kick, sub kick, snare, hat, boom), {d_at:.2f} sn'den {2 * seg_s:.1f} sn (8 bar, {BPM:.0f} bpm)"),
           "MIX": (mix, msr, 8.0, f"Static Ritual.wav (master'siz mix), {mix_at:.1f} sn'den 16 sn"),
           "VOKAL": (voc, vsr, 8.0, f"mali demo ham mikrofon kaydi (4-Audio 0002), {voc_at:.1f} sn'den 16 sn"),
           "PAD": (pad, sr, seg_s, "sentez Am9 pad (bu bilgisayarda Live disinda acilan gercek pad kaydi yok) + READY DRUM BOX KICK kanali anahtar ve duyulur (zirve -6 dBFS; pad -9 dBFS)")}

    groups = {}                                      # (voice, src) -> list of dicts
    for voice, src, letter, name, desc, kw, target, control in SETTINGS:
        x, fs, seg, _ = SRC[src]; n = int(round(seg * fs))
        meter = pyln.Meter(fs)
        if control == "depth":     # PUMP: the pad is processed, the kick is the key and is mixed in unprocessed
            ref_pad = x; ref_mix = x + kick; ref = meter.integrated_loudness(ref_pad.astype(np.float64))
            def make(depth):
                y, gr = render(x, fs, {**kw, "depth": depth}, key=kick)
                g = ref - meter.integrated_loudness(y.astype(np.float64))
                return (y * m.amp(g)).astype(np.float32), gr, g
            y1, gr1, g1 = make(kw["depth"]); y2, gr2, g2 = make(kw["depth"] + 3.0)
            wet1, wet2, dry_full = y1 + kick, y2 + kick, ref_mix
            ctl1, ctl2 = f"DEPTH {kw['depth']:.0f} dB", f"DEPTH {kw['depth'] + 3:.0f} dB"
            st1 = (float(np.max(gr1)), float(np.mean(gr1)))
        else:
            ref = meter.integrated_loudness(x.astype(np.float64)); dry_full = x
            if control == "threshold":
                c1 = round(search(x, fs, kw, "threshold", target, -40.0, 0.0, False), 1); c2 = round(search(x, fs, kw, "threshold", target + 3.0, -40.0, 0.0, False), 1)
                k1, k2 = {**kw, "threshold": c1}, {**kw, "threshold": c2}; ctl1, ctl2 = f"threshold {c1} dB", f"threshold {c2} dB"
            else:
                c1 = round(search(x, fs, kw, "pr", target, 0.0, 1.0, True), 3); c2 = round(search(x, fs, kw, "pr", target + 3.0, 0.0, 1.0, True), 3)
                k1, k2 = {**kw, "pr": c1}, {**kw, "pr": c2}; ctl1, ctl2 = f"peak reduction {c1:.3f}", f"peak reduction {c2:.3f}"
            def fin(k):
                y, gr = render(x, fs, k); g = ref - meter.integrated_loudness(y.astype(np.float64)); return (y * m.amp(g)).astype(np.float32), gr, g
            y1, gr1, g1 = fin(k1); y2, gr2, g2 = fin(k2); wet1, wet2 = y1, y2
            st1 = (float(np.max(gr1)), float(np.mean(gr1)))
        groups.setdefault((voice, src), []).append(dict(letter=letter, name=name, desc=desc, kw=kw, ctl=(ctl1, ctl2), wet=(wet1, wet2), gr=(gr1, gr2), gain=(g1, g2), n=n, fs=fs,
                                                         dry=dry_full, p90=(pctl(gr1, 90), pctl(gr2, 90)), mx=(float(np.max(gr1)), float(np.max(gr2))), mean=(float(np.mean(gr1)), float(np.mean(gr2)))))
        print(f"  {voice} {src} {letter} {name}: {ctl1} / {ctl2}; GR p90 {pctl(gr1, 90):.1f} / {pctl(gr2, 90):.1f} dB", flush=True)

    lines = ["TUTKAL — dinleme paketi v2: uc karakter (GLUE / LA / PUMP). Motor prototipi; arayuz ve plugin henuz yok.",
             "Ayni gercek kayitlar (ayristirilmis stem YOK). Her kaynak once tepe -6 dBFS'e getirildi (kanal seviyesi).",
             "Her ayarin uc dosyasi var:  _1_esit = ayar, kuruyla ayni ses yuksekligi (BS.1770 LUFS; duydugun fark seviye degil karakter)",
             "                            _2_artiGR = ayni ayar, ~3 dB daha fazla gain reduction (threshold / peak reduction asagi, PUMP'ta DEPTH +3 dB), yine esitlenmis",
             "                            _3_flip = bir yarisi kuru, sonra ayni ayarin islenmis hali (davulda 4 bar kuru + 4 bar islenmis, 165 bpm; mix ve vokalde 8 sn + 8 sn). Gecis 5 ms.",
             "Her dosyanin yaninda ayni adli .png: ustte GR (dB, asagi = daha cok sikisma), altta kuru ve islenmis tepe seviyesi.",
             "GR = gain reduction. 'GR %90' = vuruslardaki/zirvelerdeki tipik sikistirma; 'tepe' = en yuksek an. Crest = tepe - RMS.",
             "Dosyalarin latency'si (63 ornek) cikarildi: kuru ile ayni zaman cizgisinde.", ""]
    written = []
    for (voice, src), items in groups.items():
        x, fs, seg, srcdesc = SRC[src]; folder = os.path.join(out_dir, voice); os.makedirs(folder, exist_ok=True)
        files = []                                       # (stem, audio, info)
        dryref = items[0]["dry"]; n = items[0]["n"]
        files.append((f"{voice}_{src}_0_KURU", dryref, None))
        for it in items:
            stem = f"{voice}_{src}_{it['letter']}_{it['name']}"
            files.append((stem + "_1_esit", it["wet"][0], (it, 0)))
            files.append((stem + "_2_artiGR", it["wet"][1], (it, 1)))
            files.append((stem + "_3_flip", flip(dryref, it["wet"][0], n, fs), (it, 2)))
        peak = max(float(np.max(np.abs(a))) for _, a, _ in files)
        common = min(1.0, m.amp(-1.0) / peak)
        lines.append(f"== {voice} ({LABEL[voice]}), kaynak {src}: {srcdesc}. {fs} Hz. Grup kazanci {m.db(common):+.1f} dB (tepe -1 dBFS'in altinda kalsin diye tek kazanc, kuru dahil). Kuru crest {crest(dryref):.1f} dB.")
        for stem, a, info in files:
            path = os.path.join(folder, stem + ".wav"); sf.write(path, a * common, fs, subtype="PCM_24"); written.append((path, a * common, dryref * common, info, stem, voice, src, fs))
            if info is None:
                lines.append(f"{voice}/{stem}.wav: islenmemis referans" + (" (pad + kick, kompresorsuz)." if src == "PAD" else "."))
                continue
            it, kind = info
            if kind == 0:
                plot(os.path.join(folder, stem + ".png"), f"{stem}  |  {it['ctl'][0]}  |  GR %90 {it['p90'][0]:.1f} dB, tepe {it['mx'][0]:.1f}", it["gr"][0], dryref, a, fs)
                lines.append(f"{voice}/{stem}.wav: {it['desc']}. [{it['ctl'][0]}] GR %90 {it['p90'][0]:.1f} dB, tepe {it['mx'][0]:.1f} dB, ortalama {it['mean'][0]:.1f} dB; crest {crest(a):.1f} dB; esitleme kazanci {it['gain'][0]:+.1f} dB.")
            elif kind == 1:
                plot(os.path.join(folder, stem + ".png"), f"{stem}  |  {it['ctl'][1]}  |  GR %90 {it['p90'][1]:.1f} dB, tepe {it['mx'][1]:.1f}", it["gr"][1], dryref, a, fs, gr_ref=it["gr"][0])
                lines.append(f"{voice}/{stem}.wav: ayni ayar, ~3 dB fazla GR. [{it['ctl'][1]}] GR %90 {it['p90'][1]:.1f} dB, tepe {it['mx'][1]:.1f} dB, ortalama {it['mean'][1]:.1f} dB; crest {crest(a):.1f} dB; esitleme kazanci {it['gain'][1]:+.1f} dB.")
            else:
                plot(os.path.join(folder, stem + ".png"), f"{stem}  |  ilk {n / fs:.1f} sn kuru, sonra {it['ctl'][0]}", it["gr"][0], dryref, a, fs, flip_at=n)
                lines.append(f"{voice}/{stem}.wav: flip: 0-{n / fs:.1f} sn kuru, {n / fs:.1f}-{2 * n / fs:.1f} sn islenmis ({it['ctl'][0]}).")
        lines.append("")

    # ---- read every written file back from disk
    print("\nread-back ...", flush=True)
    rb = []; bad = []
    for path, audio, dry, info, stem, voice, src, fs in written:
        y, f2 = sf.read(path, dtype="float64", always_2d=True)
        fin = bool(np.all(np.isfinite(y))); pk = float(m.db(np.max(np.abs(y))))
        meter = pyln.Meter(f2); L = meter.integrated_loudness(y); L0 = meter.integrated_loudness(dry.astype(np.float64)) if info is not None else L
        pad_only = src == "PAD"
        if info is not None and pad_only: L0 = L                                   # PUMP: the matching is on the pad alone; the mix with the kick is not matched
        frame = int(0.1 * f2); k = len(y) // frame
        r_y = np.sqrt(np.mean(y[:k * frame].reshape(k, frame, -1) ** 2, axis=(1, 2))); r_d = np.sqrt(np.mean(dry.astype(np.float64)[:k * frame].reshape(k, frame, -1) ** 2, axis=(1, 2)))
        live = r_d > m.amp(-50.0)
        drop = float(np.max(m.db(r_d[live]) - m.db(np.maximum(r_y[live], 1e-12)))) if live.any() else 0.0
        vanish = bool(np.any(live & (r_y < m.amp(-65.0))))
        ok = fin and pk < -1.0 and (info is None or abs(L - L0) <= (0.3 if info[1] == 2 else 0.15) or pad_only) and not vanish
        rb.append((voice, stem, fin, pk, L - L0, drop, vanish, ok))
        if not ok: bad.append(stem)
    for r in rb: print(f"  {r[1]:44s} finite {r[2]}  peak {r[3]:6.2f} dBFS  loudness vs dry {r[4]:+.2f} LU  max 100 ms drop {r[5]:5.1f} dB  vanish {r[6]}  {'OK' if r[7] else 'BAD'}")
    print(f"read-back: {len(rb)} files, {len(bad)} bad {bad}")

    lines += ["== DINLEME REHBERI (neye kulak ver)",
              "GLUE (DAVUL / MIX): kompresor 'yapistirma' yapiyor mu? (1) Davulun parcalari ayri ayri degil tek bir sey gibi mi oturuyor; (2) vurusun ilk 1-3 ms'si geciyor (snap), govdesi bastiriliyor:",
              "  kuruya gore vurus daha 'on'de ama arkasi daha siki mi? (3) bar sonunda kuyruk (AUTO / release) nefes aliyor mu; (4) _2_artiGR'de ayni his daha belirgin mi, yoksa ezilme mi basliyor?",
              "  (5) cok hafif renk (2. + 3. harmonik, ~%0.2 THD @ 4 dB GR): kulakla ayirt edilmeyebilir; sorun degil. _3_flip'te gecis anindaki fark ne? 'Sikica' ayarinda snare ezilmeli DEGIL, onde kalmali.",
              "LA (VOKAL): vokal seviyelendi ama 'yakalanmadi' mi? (1) Compress: yumusak, nefes ve sessiz yerlerde yukselme var mi; (2) hizli tepeden sonra ilk yari hizli (~60 ms) geri geliyor, kalani saniyelerce yavas:",
              "  uzun dolu bir cumleden sonra sessizlikte kazanc ne kadar yavas aciliyor; (3) Limit: belirgin sekilde daha siki, tepeler tutulu ama kesilmiyor; (4) cift harmonik sicaklik (~%0.6 THD @ 7 dB GR) tup gibi yumusak mi;",
              "  (5) Paralel (MIX %50) vokali daha yogun ama dogal birakiyor mu.",
              f"PUMP (PAD + KICK anahtari, {BPM:.0f} bpm): pad kick'e gore nefes aliyor mu ve toparlanma tempoda mi? (1) Exp 1/8: kick'ten sonra hizli doner, 182 ms'de %90 geri; (2) Swell 1/4: pad gec gelip sonra yukseliyor (364 ms);",
              "  (3) _2_artiGR: DEPTH +3 dB; (4) _3_flip: 4 bar kuru, 4 bar pompa. Pad sentez, davul gercek. Pompa baska nota degerlerinde de (1/16 .. 1/2) ayni sekilde tempoya kilitli (olcum P1).",
              "Karar: 'ses tamam' ya da yon (daha sert / daha yumusak / daha hizli / daha cok renk / pump daha derin). Ayar motorda yapilir, paket yenilenir.",
              "", "Dogrulanmayan: kulakla hicbir sey dinlenmedi; renk miktarlari ve atak/release degerleri olcumle yerlestirildi, begeniyle degil."]
    lines += ["", "== OKU-GERI (dosyalar diskten geri okundu)"] + [f"{r[1]}: sonlu {r[2]}, tepe {r[3]:.2f} dBFS, kuruya gore {r[4]:+.2f} LU, 100 ms karede en buyuk dusus {r[5]:.1f} dB, kaybolan kare {r[6]} -> {'tamam' if r[7] else 'SORUN'}" for r in rb]
    open(os.path.join(out_dir, "OKU.txt"), "w").write("\n".join(lines) + "\n")
    json.dump([dict(zip(("voice", "stem", "finite", "peak_db", "lu_vs_dry", "max_drop_db", "vanish", "ok"), r)) for r in rb], open(os.path.join(out_dir, "readback.json"), "w"), indent=1)
    shutil.rmtree(m.TMP, ignore_errors=True)
    print("\nyazildi:", out_dir)
    return len(bad)

if __name__ == "__main__":
    sys.exit(main())
