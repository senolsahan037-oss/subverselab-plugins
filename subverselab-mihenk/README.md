# MIHENK MH-86

MIHENK is a headless-testable JUCE mastering tool. It measures loudness and true peak, applies EQ, glue compression, stereo width and a true-peak limiter, then exports a measured WAV report.

## Build

```bash
nice -n 15 cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 -DFETCHCONTENT_SOURCE_DIR_JUCE=/Users/senolsahan/Desktop/SubverseLab-Launchpad/01_AI_Tools/subverselab-splitter-desktop/build/_deps/juce-src
nice -n 15 cmake --build build --target render -j2
nice -n 15 cmake --build build --target ui_snapshot -j2
```

## Verify

```bash
./build/render_artefacts/Release/render --selftest
./build/ui_snapshot_artefacts/Release/ui_snapshot demos/mihenk_ui_v3.png 1 0
```

## Batch master

```bash
./build/render_artefacts/Release/render --master input.wav output.wav --target -14 --ceiling -1 --bits 24
./build/render_artefacts/Release/render --measure output.wav
```

The export writes a `.mihenk.txt` report beside the WAV. Loudness follows ITU-R BS.1770-4 and EBU Tech 3341/3342; true peak uses 4x oversampling. A stereo -20 dBFS sine reads -20 LUFS; the same sine in one mono channel reads -23 LUFS because BS.1770 channel weighting is applied.

For the four-minute performance fixture, use `tests/generate_music_like.sh tests/fixtures/mihenk_4m.wav`, then time the `--master` command above. The fixture is deterministic and stereo at 48 kHz.
