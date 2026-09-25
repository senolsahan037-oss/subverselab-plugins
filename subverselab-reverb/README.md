# SubverseLab Kubbe

"Kubbe" is Turkish for dome: the hammam dome the sound comes back from. A reverb built from three real spaces instead of the usual list of algorithms, on the face of a 1979 rack unit (Model KB-79) as it looked the day it left the factory:

- **Hammam**: a marble dome. Dense, bright first reflections, a flutter between parallel walls, a ring in the low mids.
- **Cistern**: an underground cistern. The column slaps arrive late and sparse, and the tail is long, dark and moves slowly like water.
- **Valley**: open air. The far slopes answer one by one, each answer darker and more blurred. With **Echo Sync** they land on the beat grid (3/4, 5/4, 7/4, 5/2, 7/2 beats).

VST3, AU and Standalone, macOS arm64.

## Controls

Mix, Decay (shown in measured seconds), Pre-delay (ms, or synced from 1/64 to 1/4), Distance (near: early reflections up front; far: the room, darker), Low Cut, High Cut, Width, Duck (the tail steps back while the source plays, up to 24 dB), Freeze, Echo Sync (Valley only).

When you switch place, the old tail rings out over 1.5 s instead of being cut.

## Measured, not nominal

The decay display runs the engine on an impulse every time a setting changes. It draws the energy per 20 ms and the Schroeder curve, and prints the T60 it measures. The Decay knob is scaled from that measurement.

## Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build -j8
```

To build against a JUCE checkout already on disk, add `-DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE`.

## Checks

```bash
./build/render_artefacts/Release/render --selftest
./build/render_artefacts/Release/render in.wav out.wav <place 0-2> [mix] [decay] [duck] [bpm] [echoSync]
./build/ui_snapshot_artefacts/Release/ui_snapshot out.png
auval -v aufx Kbbe Svlb
```

`--selftest` drives the real processor with a fake host:

- **Decay.** Each place's T60 is within range of its nominal value, and Decay x0.5 gives a shorter tail. The output is finite and silent 24 s after an impulse.
- **Echo Sync.** Valley's first echo lands on the 3/4 beat at 90, 120 and 140 BPM, within 3 ms.
- **Freeze.** The tail holds within 3 dB for 4 s while the input keeps playing.
- **Duck.** At 100 % the wet drops 12–26 dB. It ducks, it does not mute.
- **Determinism.** Identical bits across runs and across block sizes 512 and 97.
- **Level.** The three places sit within 4 dB of each other at full wet.
- **Place switch.** No step larger than the signal already has.

## Not done yet

- Code signing and notarisation.
- A Windows build.
- Switching place again while the previous tail is still fading cuts that older tail.
