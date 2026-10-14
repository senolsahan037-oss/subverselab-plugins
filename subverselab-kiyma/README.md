# KIYMA

Free insert effect by SubverseLab. KIYMA is Turkish for "mince": a live grinder for the track. It never loads files and is not a sampler.

Since DSP v5 the engine is three layers over a Beats-warp grinder, and it is deterministic: there is no randomness anywhere in the audio path (every "random" value is a fixed table indexed from the transport position), so the same input, settings and transport give bit-identical output at any block size and from any start point on the same absolute grid. The specification is `docs/DSP_V5_LAYERS.md` (with its v5.1 correction); `docs/DSP_V4_GRIND.md` describes the grinder.

```
in -> CRANK grind (1/8 note segments: fill + loop-forward mince + grain texture + high-pass) -> base
base -> core (gain 1 - 0.95 smoothstep (0.1, 0.9, brutal): BRUTAL replaces it; never cut by OFFSET)
base -> GLITCH   +9 st (0.6, panned 35 % right) and +24 st (0.35, centre), 30 ms behind, spray, AR per 1/16 note  \
base -> BRUTAL   a 1/16 note moment played back and forth, jumping capture position, tanh drive                      > OFFSET gate (the layers only) -> + core
base -> ENVELOPE a clean synced recall from behind (division), AR per division line, feedback 0.30                  /
the sum of the layers: LowCut 90 Hz (48 dB/oct), then a Saturator (+2 dB drive), then the OFFSET gate
out = dry (1 - mix) + wet mix -> output gain -> safety limiter (-0.3 dBFS)
```

## Controls (parameter IDs in lowercase)

| id | range | what it does |
|---|---|---|
| `crank` | 0..1 | GRIND, the Beats-warp style grinder (no longer a macro). Every 1/8 note the input restarts; a segment plays its fill (1 - 0.55 c of it), then the last 1/64 note loops forward, each repeat x0.72, up to the next line; fades from 10 ms down to 0.7 ms; a deterministic grain texture (Hann grains 70 -> 28 ms, pitch +-15 cents x c, spray up to 13 ms x c) and a high-pass at 90 c Hz sit on top. 0 is bypassed bit for bit. The tag reads `GRIND xx %`. |
| `glitch` | 0..1 | the pitch-shifted followers: the base shifted +9 st and +24 st (a dual-head delay-line shifter, 40 ms window), 30 ms behind, grain-smeared by the spray table (up to 20 ms), pulsing with an AR envelope per 1/16 note (4 ms attack), with the project's EQs (+9: LowCut 250 Hz, shelf 4.5 kHz +2 dB; +24: LowCut 441 Hz, bell 499 Hz +2.1 dB, shelf 2.44 kHz +1.7 dB). Level g. The tag shows %. |
| `brutal` | 0..1 | the looped moment, which also REPLACES the core (core gain = 1 - 0.95 smoothstep (0.1, 0.9, b); what is left of the core gets the same LowCut). A 1/16 note window of the base (the most recent one at each half bar, shifted back by {-1.25, +3.75, -1.25, -6.25} sixteenths x b as a running sum) plays forward, then backward, and at every 1/8 note line restarts HARD (a 0.5 ms edge: the restart tick of the reference); the read start advances by 1/128 note per cycle and wraps inside the window; a 1 ms dip at the turnaround and at the wrap. Until the first capture the live input plays through (no silent start). Tanh drive 1 + 3 b, the project's EQ (LowCut 500 Hz 48 dB/oct instead of the project's 100: ref2 has nothing below about 400 Hz; bells at 806 Hz -1.9 dB and 2.5 kHz +2 dB, high shelf 4.5 kHz +2.6 dB), a Glue-like compressor (-10 dB, 4:1, 1 ms / 0.2 s, 60 % wet) and a Saturator (+7 dB, 20 % wet); level 0.85 b. The tag shows %. |
| `offset` | 0..1, 6 steps | the gate on the sum of the three layers (the core is never cut): they live from each beat for 1/64, 1/32, 1/16, 1/8, 1/8D, FULL (no gate, the default), then are gated out with a 4 ms raised-cosine ramp. The tag shows the division. |
| `envelope` | 0..1, 7 steps | the recall: OFF, 1/32, 1/16, 1/8, 1/4, 1/2, 1 BAR (index = round (v x 6)); a clean echo exactly that long at the host tempo (a bar follows the time signature; at most 4 s), feedback 0.30, send 0.7, 9 kHz high cut in the loop. A division or tempo change crossfades over 30 ms. OFF fades out over 20 ms and clears the line. The tag shows the division. |
| `attack` | 1-100 ms | the recall's gain envelope: it restarts on every grid line of the division, rises over ATTACK, then decays exponentially with RELEASE. No effect with `envelope` OFF. |
| `release` | 10-1000 ms | the decay time constant of that envelope |
| `mix` | 0..1 | linear dry / wet. 0 is the input (delayed by the latency), bit for bit |
| `output` | -24..+6 dB | output gain, before the limiter |

The beat position comes from the host's quarter-note position; with no playhead or a stopped transport it free-runs at the host tempo (120 BPM when there is none). Latency is 64 samples at every sample rate and setting (the limiter's lookahead), reported to the host. Whatever the settings, the output peak is at or below -0.3 dBFS. The five presets of the lever bank ("moods") are plain parameter sets, see `Source/Presets.h`; a recall sets CRANK to 0.

## Build

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/an/existing/JUCE-9.0.1/checkout   # optional, avoids the download
cmake --build build --config Release                        # Standalone, VST3, AU (universal arm64;x86_64, macOS 11)
cmake --build build --config Release --target render ui_snapshot
./build/render_artefacts/Release/render --selftest
```

`SVL_COPY_PLUGINS` is OFF by default: the build never writes into `~/Library/Audio/Plug-Ins`.

## Tools

- `render --selftest`: null, finite, steps, undo, presets and the acceptance checks of the v5 specification (STATUS.md lists the numbers).
- `render in.wav out.wav glitch=0.6 brutal=0.3 crank=0.5 bpm=103 mix=1 tail=2`: renders a file through the real processor (24-bit WAV, latency removed), at any tempo.
- `render --controlproof [--quick]`: the shared control harness; writes `docs/CONTROL_PROOF.md`.
- `ui_snapshot out.png`: draws the editor without a screen (`--bench-live`, `--check-assets`, see STATUS.md); `tools/snapshots.sh` renders the snapshot set.
- `tests/fixtures/` holds reference audio for analysis only (the owner's reference renders and a dry excerpt). It is never part of the plug-in.

## Layout

- `Source/KiymaEngine.h`: the whole DSP, plain C++ with no JUCE.
- `Source/PluginProcessor.*`, `Source/PluginEditor.*`, `Source/Theme.h`, `Source/KnobDrag.h`: the plug-in. The editor is the "everything is part of the machine" face (1200x800, assets in `Assets/`, specs `docs/UI_SPEC*.md`, snapshots in `docs/snapshots/`).
- `Source/SvlUndo.h`, `tests/SvlUndoTests.h`, `tests/SvlControlProof.h`: copied from `../subverselab-plugin-common` by its sync scripts.

## Knobs

A knob follows the mouse by the total distance from where the drag began (200 px for the whole range, shift = a sixth of that), so the stepped ENVELOPE and OFFSET knobs can be turned by hand. The wheel moves a stepped knob by exactly one step per event (events closer than 120 ms to the last are ignored); a continuous knob keeps its smooth wheel.
