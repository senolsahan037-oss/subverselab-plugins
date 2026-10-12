"""Listening package for the TRACK switch: a synthetic drum loop through KASET in MIX and in TRACK, level-matched.

    python3 tools/dinle_track.py <kasetraw binary> <scratch dir> [out dir]

kasetraw is built from tools/kasetraw.cpp (plain C++17, no JUCE):
    clang++ -std=c++17 -O2 tools/kasetraw.cpp -o <scratch>/kasetraw

Writes to 01_AI_Tools/prototypes/DINLE/18_KASET_TRACK/ (or [out dir]): the dry loop, then for three settings the MIX render and the
TRACK render (TRACK gain-matched to MIX by RMS over the loop), olcum.json with every number, and OKU.md (plain Turkish, ASCII).
Both renders have their own latency removed (kasetraw does that), so they line up with the dry loop and with each other.
NOT RUN as written (owner rule: nothing is built or run before the November builds).
"""
import json, os, subprocess, sys
import numpy as np, soundfile as sf, scipy.signal as sg

BIN, TMP = sys.argv[1], sys.argv[2]
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = sys.argv[3] if len(sys.argv) > 3 else os.path.normpath(os.path.join(HERE, '..', '..', 'prototypes', 'DINLE', '18_KASET_TRACK'))
SR = 44100
os.makedirs(TMP, exist_ok=True)
os.makedirs(OUT, exist_ok=True)

# name, text for OKU, kasetraw settings (generation = number of trips 1..5, depths 0..1)
SETTINGS = [
    ('A_G3_kopya',   '3. nesil (2. kopya), WOW 0.50 FLUTTER 0.50: onaylanan arkadastan kopya ayari', dict(gen=3, wow=.50, flutter=.50, drive=.55, hiss=.50, wear=.25)),
    ('B_G5_dorduncu', '5. nesil (4. kopya), WOW 0.55: kafa odasi en az, WOW kisitlamasi burada en cok isler', dict(gen=5, wow=.55, flutter=.50, drive=.55, hiss=.50, wear=.30)),
    ('C_G2_ilk_kopya', '2. nesil, WOW 0.40: tek kisitlamasiz ayar, iki mod birbirine en yakin', dict(gen=2, wow=.40, flutter=.40, drive=.50, hiss=.45, wear=.10)),
]


def drum_loop(bars=4, bpm=100):
    rng = np.random.default_rng(18)
    step = int(SR * 60 / bpm / 4)               # a sixteenth
    n = step * 16 * bars
    x = np.zeros(n)

    def add(at, s):
        e = min(n, at + len(s)); x[at:e] += s[:e - at]

    t = np.arange(int(0.45 * SR)) / SR
    kick = np.sin(2 * np.pi * (45 * t + 75 * (1 - np.exp(-t * 28)) / 28)) * np.exp(-t * 9)
    kick += 0.35 * np.exp(-t * 400) * rng.normal(0, 1, len(t))
    t2 = np.arange(int(0.30 * SR)) / SR
    b, a = sg.butter(2, [180 / (SR / 2), 7000 / (SR / 2)], 'band')
    snare = sg.lfilter(b, a, rng.normal(0, 1, len(t2))) * np.exp(-t2 * 16) + 0.5 * np.sin(2 * np.pi * 190 * t2) * np.exp(-t2 * 30)
    t3 = np.arange(int(0.07 * SR)) / SR
    bh, ah = sg.butter(2, 7500 / (SR / 2), 'high')
    hat = sg.lfilter(bh, ah, rng.normal(0, 1, len(t3))) * np.exp(-t3 * 60)
    for bar in range(bars):
        o = bar * 16 * step
        for k in (0, 6, 10):  add(o + k * step, 0.8 * kick)
        for k in (4, 12):     add(o + k * step, 0.55 * snare)
        for k in range(0, 16, 2): add(o + k * step, 0.22 * hat * (1.0 if k % 4 == 0 else 0.7))
    x /= np.abs(x).max() / 0.7
    return x


def run(x, p, track):
    a, b = os.path.join(TMP, 'k_in.f32'), os.path.join(TMP, 'k_out.f32')
    np.stack([x, x], 1).astype(np.float32).tofile(a)
    cmd = [BIN, a, b, str(SR)] + [str(p[k]) for k in ('gen', 'wow', 'flutter', 'drive', 'hiss', 'wear')] + ['0', str(int(track))]
    subprocess.run(cmd, check=True)
    return np.fromfile(b, dtype=np.float32).reshape(-1, 2).astype(np.float64)


def rms(v): return float(np.sqrt(np.mean(v ** 2)))
def db(v): return 20 * np.log10(v + 1e-20)


dry = drum_loop()
tail = np.zeros(SR // 2)                      # half a second of room so the last hit rings out
dry = np.concatenate([dry, tail])
sf.write(os.path.join(OUT, '00_KURU.wav'), np.stack([dry, dry], 1), SR, subtype='PCM_24')

report = {'sr': SR, 'latency_samples': {'MIX': 5 * round(0.003 * SR), 'TRACK': round(0.0015 * SR)}, 'files': []}
lines = []
i = 1
a0 = int(0.3 * SR)
for name, text, p in SETTINGS:
    mix = run(dry, p, False)
    trk = run(dry, p, True)
    r_mix, r_trk = rms(mix[a0:]), rms(trk[a0:])
    gain = r_mix / r_trk
    trk_m = trk * gain
    peak = max(np.abs(mix).max(), np.abs(trk_m).max(), 1e-9)
    scale = 0.95 / peak if peak > 0.95 else 1.0
    for tag, sig in (('MIX', mix * scale), ('TRACK', trk_m * scale)):
        fn = '%02d_%s_%s.wav' % (i, name, tag)
        sf.write(os.path.join(OUT, fn), sig, SR, subtype='PCM_24')
        report['files'].append(fn)
        i += 1
    # how far apart the two renders are once level-matched (rms of the difference against the MIX rms)
    d = db(rms((trk_m - mix)[a0:]) / r_mix)
    report[name] = dict(settings=p, rms_mix_dbfs=db(r_mix), rms_track_dbfs=db(r_trk), track_gain_db=db(gain), difference_vs_mix_db=d, scale_db=db(scale))
    lines.append('| %s | %.2f dB | %.1f dB |' % (name, db(gain), d))

json.dump(report, open(os.path.join(OUT, 'olcum.json'), 'w'), indent=2)

oku = '''# KASET - TRACK dinleme paketi (davul dongusu, MIX ve TRACK, ayni seviyede)

Sentetik davul dongusu (kick, snare, hat), 100 BPM, 4 olcu. Her ayarda iki dosya var: **MIX** (bildigimiz KASET: 660 ornek / 15 ms gecikme) ve **TRACK** (tek kafa: 66 ornek / 1.5 ms gecikme). TRACK dosyasi MIX ile ayni ses seviyesine getirildi (RMS), yani ikisini sirayla dinleyince "hangisi daha yuksek" farki yok. Gecikme iki dosyadan da cikarildi: dosyalar kuru donguyle ve birbiriyle ayni hizada.

## Neye bakilacak
1. **Kick'in ilk vurusu ve snare'in ucu (transient).** TRACK'te de ayni sertlikte mi? Doygunluk, EQ, hiss ve dropout iki modda ayni; fark sadece kafanin nasil hareket ettigi.
2. **WOW (yavas dalgalanma).** TRACK'te bes kafanin dalgalanmasi tek kafada toplaniyor. Uzun notada ya da hi-hat'te perde kaymasi MIX'tekiyle ayni karakterde mi, daha az mi?
3. **B ayari (5. nesil)**: burada TRACK'in WOW derinligi sinirlandi (kafanin odasi 1.5 ms, bes kopyanin dalgalanmasi 5 kat bindirir). Dalgalanma MIX'ten daha sig duyulabilir. Bu bir hata degil, tasarim: sinirin ne kadar oldugu `render --selftest` ciktisindaki "WOW cap" satirinda yazar. **Karar senin: sig dalgalanma kabul mu, yoksa TRACK'te kafa odasi buyutulsun mu (gecikme artar).**
4. **A ve C ayari**: iki modun farki cok kucuk olmali. Fark yok sayacak kadar azsa TRACK'i kayit icin guvenle birakabiliriz.
5. Dongunun icinde bir yerde TRACK dugmesine basilmis gibi bir "gecis" dosyasi YOK; gecis sirasinda cikis yaklasik 20-35 ms kisa bir sessizlige iner (tik yok). Bunu Live'da dugmeye basarak dinlemek gerek, burada yok.

## Olcumler
Render calistirildiktan sonra `olcum.json` ve asagidaki tablo dolar (tablo `tools/dinle_track.py` tarafindan yazilir).

| ayar | TRACK'e uygulanan kazanc (MIX ile esleme) | iki render arasindaki fark (MIX RMS'ine gore) |
|---|---|---|
''' + '\n'.join(lines) + '''

## Dosyalar
''' + '\n'.join('- ' + f for f in ['00_KURU.wav'] + report['files']) + '''

## Ayarlar
''' + '\n'.join('- **%s**: %s' % (n, t) for n, t, _ in SETTINGS) + '''

## Dogrulanmadi
Gercek bir DAW'da (Live) kayit yaparak gecikme turu, ve senin kulagin. Bu dosyalar motoru dogrudan calistirir (`kasetraw`), eklenti penceresinden gecmez: AUTO GAIN ve MIX dugmesi burada yok, seviye eslemesi RMS ile yapildi.
'''
open(os.path.join(OUT, 'OKU.md'), 'w').write(oku)
print('wrote', OUT)
