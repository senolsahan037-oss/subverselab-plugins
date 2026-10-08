# TABAN (Model TB-83)

Minimum level floor / reverse limiter by SubverseLab. Free, forever.

A limiter keeps audio from rising above a ceiling. TABAN keeps it from falling below a floor: signal under FLOOR is
lifted to FLOOR (at most 24 dB), signal above it passes bit for bit. Silence stays silence. Zero latency.

## Controls

| Control | Range | Default | |
|---|---|---|---|
| IN | On / Off | On | Off = bypassed, with a 10 ms crossfade. |
| FLOOR | -60 .. 0 dBFS | -60 (OFF) | The floor. Linear in dB. Double-click resets. |
| OUTPUT | -12 .. +12 dB | 0 dB | Trim after the floor. Double-click resets. |

The LIFT read-out shows the lift now; the IN and OUT bars beside the fader use the fader's own scale.

## Formats

- macOS 11+ (Apple Silicon and Intel): VST3, AU
- Windows 10+ 64-bit: VST3

## Build

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DSVL_COPY_PLUGINS=OFF "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"
cmake --build build-release --config Release --target Taban_VST3 Taban_AU test_taban_engine ui_snapshot
./build-release/test_taban_engine
```

Spec and measurements: `SPEC.md`. Log: `STATUS.md`.
