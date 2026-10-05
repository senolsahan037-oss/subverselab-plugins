#!/usr/local/bin/python3
"""Writes the owner's listening pack: for drums, mix bus, bass and vocal, the dry excerpt and three
settings, loudness-matched to the dry (BS.1770), and OKU.txt with what was measured.

  dinle.py [out_dir]     default ../prototypes/DINLE/10_TUTKAL
"""
import os, sys
import numpy as np, soundfile as sf, pyloudnorm as pyln
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import measure as m

SECS = 20.0
SOURCES = [
    ("DAVUL", "davul bus'ı: READY DRUM BOX'ın ayrı kanallarının (kick, sub kick, snare, hat, boom) toplamı", "~/Desktop/solo/READY DRUM BOX/Samples/Processed/Bounce/Bounce *.wav", [
        ("Glue",    "yapıştırıcı: vuruşlarda ~4 dB, atak yavaş, AUTO release", dict(ratio=4, attack=10, auto=1), 4.0),
        ("Punch",   "vuruşu öne çıkaran: atak 30 ms (transient geçer), release 0.1 s, ~6 dB", dict(ratio=4, attack=30, release=100), 6.0),
        ("Parallel","ezilmiş paralel: 20:1, atak 0.3 ms, ~12 dB, MIX %40, DRIVE %50", dict(ratio=20, attack=0.3, release=100, mix=0.4, drive=0.5), 12.0),
        ("Tight",   "sıkı: 4:1, atak 1 ms (vuruşu da yakalar), AUTO, ~5 dB", dict(ratio=4, attack=1, auto=1), 5.0)]),
    ("MIX", "mix bus (master'sız mix)", "~/Desktop/Export/Static Ritual.wav", [
        ("Glue_2to1", "hafif yapıştırma: 2:1, atak 30 ms, AUTO, ~2 dB", dict(ratio=2, attack=30, auto=1), 2.0),
        ("Glue_4to1", "klasik bus: 4:1, atak 10 ms, AUTO, ~4 dB", dict(ratio=4, attack=10, auto=1), 4.0),
        ("Glue_Drive", "aynı 4:1 ayar + DRIVE %40 (çıkış trafosu)", dict(ratio=4, attack=10, auto=1, drive=0.4), 4.0),
        ("Tight", "sıkı: 4:1, atak 1 ms (tepeleri de tutar), AUTO, ~4 dB", dict(ratio=4, attack=1, auto=1), 4.0)]),
    ("ALT", "alt uç: READY DRUM BOX 'OP Boom' kanalı tek başına (temiz bas kaydı bulamadım; içeriğini duyamıyorum)", "~/Desktop/solo/READY DRUM BOX/Samples/Processed/Bounce/Bounce 2-3 OP Boom*.wav", [
        ("Level",  "seviyeleme: 4:1, atak 10 ms, AUTO, ~5 dB", dict(ratio=4, attack=10, auto=1), 5.0),
        ("Drive",  "aynı ayar + DRIVE %60 (altlarda doygunluk)", dict(ratio=4, attack=10, auto=1, drive=0.6), 5.0),
        ("Tight",  "sıkı: 6:1, atak 1 ms, release 0.3 s, ~7 dB", dict(ratio=6, attack=1, release=300), 7.0)]),
    ("VOKAL", "vokal: 'mali demo' projesinden ham mikrofon kaydı (4-Audio 0002, kırpılmasız, baştan sona dolu)", "~/Desktop/mali demo  Project/Samples/Recorded/4-Audio 0002 [[]2026-09-27 224148[]].wav", [
        ("Gentle", "yumuşak: 2:1, atak 10 ms, AUTO, ~3 dB", dict(ratio=2, attack=10, auto=1), 3.0),
        ("Level",  "seviyeleme: 3:1, atak 3 ms, AUTO, ~5 dB", dict(ratio=3, attack=3, auto=1), 5.0),
        ("Firm",   "sıkı: 6:1, atak 1 ms, release 0.3 s, ~8 dB", dict(ratio=6, attack=1, release=300), 8.0)]),
]

def loudest_window(pattern, secs):
    """Reads one file, or sums every file a pattern matches (tracks of one session), and returns its loudest window."""
    import glob
    paths = sorted(glob.glob(os.path.expanduser(pattern)))
    if not paths: raise SystemExit(f"no file matches {pattern}")
    total, sr = None, None
    for path in paths:
        x, fs = sf.read(path, dtype="float32", always_2d=True)
        if x.shape[1] == 1: x = np.repeat(x, 2, axis=1)
        x = x[:, :2]
        if total is None: total, sr = x, fs
        else:
            if fs != sr: raise SystemExit(f"{path}: sample rate differs")
            n = min(len(total), len(x)); total = total[:n] + x[:n]
    x = total; n = int(secs * sr); hop = int(2 * sr)
    best, at = -1.0, 0
    for st in range(0, max(1, len(x) - n), hop):
        e = float(np.mean(x[st:st + n] ** 2))
        if e > best: best, at = e, st
    return x[at:at + n].copy(), sr, at / sr, [os.path.basename(q) for q in paths]

def crest(x): return float(m.db(np.max(np.abs(x))) - m.db(np.sqrt(np.mean(x ** 2))))

def find_threshold(x, sr, target, kw):
    """Threshold that puts the 90th percentile of the gain reduction at `target` dB."""
    lo, hi = -40.0, 0.0
    for _ in range(12):
        mid = 0.5 * (lo + hi)
        probe = {k: v for k, v in kw.items() if k not in ("mix", "drive")}
        gr = m.run(x, sr=sr, threshold=mid, **probe)[1]
        if np.percentile(gr, 90) > target: lo = mid
        else: hi = mid
    return round(0.5 * (lo + hi), 1)

def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "prototypes", "DINLE", "10_TUTKAL")
    os.makedirs(out_dir, exist_ok=True)
    lines = ["TUTKAL — davul ve bus kompresörü, dinleme paketi (motor prototipi; arayüz ve plugin henüz yok).",
             "Kaynakların hiçbiri ayrıştırılmış stem değil: gerçek kanal kayıtları ve master'sız bir mix.",
             "Her kaynakta 0 = kuru, sonrakiler ayarlar. Kaynaklar önce tepe -6 dBFS'e getirildi (kanal seviyesi). İşlenmiş dosyalar kuruyla aynı ses yüksekliğine getirildi (BS.1770 LUFS),",
             "yani duyduğun fark seviye değil, kompresörün karakteri. Makeup yerine bu eşitleme kullanıldı.",
             "GR = kazanç azaltma. 'GR %90' vuruşlardaki tipik sıkıştırma, 'GR tepe' en yüksek an. Crest = tepe - RMS (küçülmesi: daha sıkı).", ""]
    for tag, title, path, settings in SOURCES:
        x, sr, at, names = loudest_window(path, SECS)
        x = (x * m.amp(-6.0) / float(np.max(np.abs(x)))).astype(np.float32)               # a gain-staged channel: peaks at -6 dBFS
        if m.db(np.sqrt(np.mean(x ** 2))) < -40: raise SystemExit(f"{path}: nearly empty, not programme")
        src = names[0] if len(names) == 1 else f"{len(names)} kanal toplamı"
        meter = pyln.Meter(sr); ref = meter.integrated_loudness(x.astype(np.float64))
        rendered = [("0_KURU", x, None)]
        for i, (name, desc, kw, target) in enumerate(settings, 1):
            thr = find_threshold(x, sr, target, kw)
            y, gr = m.run(x, sr=sr, threshold=thr, **kw)
            y = y[m.LAT:]; y = np.concatenate([y, np.zeros((m.LAT, 2), np.float32)])        # take the latency out
            gain = ref - meter.integrated_loudness(y.astype(np.float64))
            y = (y * m.amp(gain)).astype(np.float32)
            rendered.append((f"{i}_{name}", y, (desc, thr, kw, float(np.percentile(gr, 90)), float(np.max(gr)), float(np.mean(gr)), gain)))
        peak = max(float(np.max(np.abs(y))) for _, y, _ in rendered)
        common = min(1.0, m.amp(-1.0) / peak)                                               # one gain for the whole group, top at -1 dBFS
        lines.append(f"== {tag}: {title}. Kaynak: {src}, {int(at // 60)}:{at % 60:04.1f}'den {SECS:.0f} sn, {sr} Hz. Grup kazancı {m.db(common):+.1f} dB. Kuru crest {crest(x):.1f} dB.")
        for name, y, info in rendered:
            fn = f"{tag}_{name}.wav"
            sf.write(os.path.join(out_dir, fn), y * common, sr, subtype="PCM_24")
            if info is None:
                lines.append(f"{fn}: işlenmemiş (karşılaştırma için).")
            else:
                desc, thr, kw, p90, mx, mean, gain = info
                ctl = ", ".join(f"{k} {v}" for k, v in kw.items())
                lines.append(f"{fn}: {desc}. [threshold {thr} dB, {ctl}] GR %90 {p90:.1f} dB, tepe {mx:.1f} dB, ortalama {mean:.1f} dB; crest {crest(y):.1f} dB; eşitleme kazancı {gain:+.1f} dB.")
        lines.append("")
    lines += ["Not: atak süresi %63 tanımıyla (ör. 10 ms'de sıkıştırmanın %63'ü gelir, %90'ı ~23 ms'de). Yavaş atakta vuruş geçer, crest büyür: bu 'punch' etkisi, hata değil.",
              "Ne dinlemeli: (1) vuruşlar eziliyor mu, yoksa öne mi çıkıyor; (2) AUTO ayarlarda vuruş aralarında ses doğal mı toparlanıyor;",
              "(3) MIX_3 ve ALT_2'de DRIVE'ın altlara eklediği renk hoş mu, fazla mı; (4) hiçbir dosyada pompalama, çıtırtı ya da sesin kaybolması olmamalı.",
              "Karar: 'ses tamam' ya da yön (daha sert / daha yumuşak / daha hızlı bıraksın / drive daha az). Ayar motorda yapılır, paket yenilenir."]
    open(os.path.join(out_dir, "OKU.txt"), "w").write("\n".join(lines) + "\n")
    print("\n".join(lines)); print("\nyazıldı:", os.path.abspath(out_dir))

if __name__ == "__main__":
    main()
