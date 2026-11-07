# ADALET sidechain bench (COLLIDE)

Synthetic main / key pairs whose every component is known, a ground truth of where and when they collide, and metrics for any `dyneqraw` build.
Deterministic: every random phase comes from numpy's PCG64 seeded per case (seed = crc32 of the case name). Nothing outside a temp folder is written.

```
python3 tools/bench/bench.py --raw build/dyneqraw                      # the plug-in's law (--keylaw signal), all 50 cases
python3 tools/bench/bench.py --raw build/dyneqraw --laws signal,091    # and the 0.9.0 / 0.9.1 law of the same build
python3 tools/bench/bench.py --n 20,12,30                              # the ground truth's N, sensitivity
BENCH_GT_FILTER=rect python3 tools/bench/bench.py ...                  # another band model for the ground truth (part | partpow | rect | roex)
python3 tools/bench/conflicts.py --raw build/dyneqraw                   # layers acting on one band (SIMPLIFY_PLAN)
```
`dyneqraw` needs `--reallog` (bell gains and each layer's realised cut at the 32 band centres; added 2026-10-25).

## Material (50 cases, `cases()` in bench.py)

Every signal is a sum of sinusoids with a known frequency and amplitude per sample; noise is a dense multi-sine (16 random-phase lines per ERB), so its power per
line is known too. Hit envelopes have raised-cosine attacks (2-3 ms): a linear 1 ms ramp on a 50 Hz sine spreads energy a few hundred Hz wide, which a
component list would not know about; a deliberate click is modelled as its own noise component.

| group | what | cases |
|---|---|---|
| KB | pitched kick (f = f_k + 2 f_k e^(-t/8 ms): 3 f_k at the hit, f_k after ~30 ms; 120 ms decay; knock = 2nd / 3rd harmonics at -8 / -14 dB, 40 ms decay; full = + 2-6 kHz click) every 0.5 s against a sustained bass (6 partials, 1/n). f_k = 40 / 48 / 60 / 80 Hz x bass f0 = 1.06 f_k (shares the kick's region), 1.5 f_k (next band), 3 f_k (no energy under the kick); pure / full kick; key +18 / -18 dB; the kick stops at 2 s while the bass goes on; bass 98 Hz under a pure kick | 18 |
| VB | vocal-like formant tone (f0 180 Hz male / 260 Hz female, 5 Hz vibrato, four formants) in three phrases, over a beat (kick, snare = 190 Hz tone + 150 Hz-6 kHz noise, 8th hats 6-14 kHz, a bass line); vocal at -10, +2.5 and -22 dB | 4 |
| PV | synth pad (C major, 10 partials each) under a male vocal; under a female vocal present 1 s of 4 | 2 |
| CH | C major against A minor (two shared notes) and against F# major (none) | 2 |
| EX | key where the main is silent; main with a silent key (zeros) and with no key bus; both silent; main silent; key 24 dB louder / quieter; transient key on a sustained main and the reverse; noise main against a key tone; noise against noise; a key that stops while the main goes on | 12 |
| SB | KB 48 / VB male at 44.1 and 96 kHz, and at host blocks 64 / 100 / 512 / 1024 | 12 |

Engine settings: DEPTH 6, MARGIN 3, SPEED MEDIUM, FOCUS 40 Hz - 16 kHz (the law itself; FOCUS is the user's restriction; `--focus 120,8000` runs the
plug-in default).

## Ground truth (per analysis band, per 32-sample law tick)

1. **Bands** = the engine's 32 ERB bands (40 Hz .. 16 kHz, centres `erb_hz(e0 + 1.23 k)`), extent = +-half a step in ERB rate.
2. **A component is in band b** when its frequency lies in b's extent. Within **0.075 ERB of an edge it is in both bands**: measured on the analysis bank,
   inside that zone the two neighbouring bands read a tone within 3 dB of each other (`EDGE_ZONE_ERB`), so which band holds it is not decidable.
3. **Band level** = the power of the components in the band **summed with their phases** (partials a few Hz apart beat exactly as in the signal), averaged
   over the last **1 / ERB(fc)** (an auditory filter's time resolution, Glasberg & Moore 1990).
4. **Present**: the main wherever its level is >= **-60 dBFS** (SPEC 1's floor, `kKeyFloorDb`): masking is the main against the key in the band (item 5),
   not the main against its own peak (a beat's mid and high bands sit 20-40 dB under its kick band and still mask a vocal). The key when its level is
   >= -60 dBFS and within **N = 20 dB** of its own loudest band over the case. N: the analysis bank rejects a neighbour band's centre by 22 dB (cascade, -12 dB
   edges), so a key band further down than ~20 dB under the key's peak cannot be told from leakage. Sensitivity runs at N = 12 and 30 are in the tables.
5. **G1 collision** (SPEC 1, the product's definition): main and key both present in the band and the key does not already lead the main by MARGIN
   (K - M <= MARGIN - 1 dB, i.e. the SPEC 1 need (M - K) + MARGIN is at least 1 dB). Expected cut = clamp(need, 0, DEPTH).
6. **G2 collision, kick / bass** ("listen to the signal"): when the key's loudest band is at or under **200 Hz** (research section 7: Mastrng "overlap
   below ~100-200 Hz", iZotope "masking from about 150 Hz and below") and present, the band holding that loudest component (the kick's fundamental), and a
   neighbour that receives it within 3 dB, collide with the main there **whoever leads** (research section 7: engineers duck the bass at the kick fundamental,
   SOS / McDSP / Mastrng). Expected cut = DEPTH. Only where the key's energy actually is and only while it is present: the knock and the attack sweep are
   their own components and collide only where the bass has energy.
7. Where both are present but the need is under 1 dB the band is not a collision (MISSED does not count it), but a cut up to that need **+ 3 dB** is not a
   false cut: a band's level cannot be measured closer than half power (measured: the cascade reads a component on an edge 12 dB low, a 15 ms burst 2-3 dB
   above its integrated level). Where either signal is absent there is no tolerance.

## Metrics

- **FALSE CUT**: realised cut at a band centre > allowed + **0.5 dB**. Allowed = DEPTH while the band collides, held for **1 / ERB(fc)** after the collision
  ends, then falling with the SPEED release (MEDIUM 120 ms, SPEC 2), and never less than the ground truth's own need (item 7). Reported as seconds with any
  false band, split into **law** (the law's per-band target asked for the cut) and **skirt only** (the target was clean; a neighbouring bell's skirt reaches the
  band: the bells are 1.4 bands wide, SPEC 7.2).
- **MISSED CUT**: a collided band, past the first max(10 ms, 1 / ERB) of the collision (5 ms attack, band-pass build-up), whose realised cut is < **1 dB**; % of
  collided band-ticks.
- **MISPLACED**: ticks with a collision and a realised cut >= 1 dB whose deepest band is more than **1/3 octave** from every collided band whose expected cut
  is within 0.5 dB of the largest; % of those ticks.
- **cut while key silent**: the key's total power under -60 dBFS for longer than the release allowance, and a cut > 0.5 dB anywhere.
- **timing**: median ms from a collision's start to a cut >= 1 dB, and from all collisions' end to every cut < 0.5 dB.
- **bit-exact**: a case with no collision anywhere must give output == input bit for bit.

The realised cut is computed from the bell gains the engine logs (`--reallog` g1..g32) with the engine's own RBJ bell formula and Q (`bellDbAt`); the
law's target is the cut log (`--cutlog`).

## Sensitivity models (`BENCH_GT_FILTER`)

`partpow` = item 2 without phases (power sum: beating ignored); `rect` = each component spread over a +-0.5 ERB rectangle and shared by overlap;
`roex` = what a roex(p) auditory filter at each band centre receives (a component at the next centre counts -13.6 dB). The rect and roex models make a strong
component "present" in its neighbour bands, which turns cuts next to it into collisions; they are reported, not used for decisions.

## Known limits (2026-10-26)

- The bench is synthetic: no reverb, no stereo, no mastered material. Keys and mains are stationary in level (the engine's own-loudest reference is a
  2 dB/s peak hold; not tested on sections that change level).
- The engine sees 32 bands; the ground truth is per band too, but its band power is ideal (a partition), the engine's cascade reads a component on an
  edge 12 dB low. Remaining misses and false cuts at the band resolution are listed per case in the logs.
