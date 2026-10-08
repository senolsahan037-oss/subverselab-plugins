# TABAN (Model TB-83)

Minimum dB Floor Leveler / Reverse Limiter by SubverseLab.

Where a traditional brickwall limiter prevents audio from rising above a designated ceiling, TABAN prevents audio from falling below a designated decibel floor.

## Overview

When the signal level drops below the user-selected `FLOOR` setting, TABAN instantly raises it to the floor level. Loud passages above the floor remain untouched (0.0 dB change). Absolute silence ($x = 0$) remains untouched silence.

## Controls

| Control | Panel Label | Range | Default | Unit | Description |
|---|---|---|---|---|---|
| Bypass | IN | Toggle | On | - | Click-free engine bypass switch. |
| Floor | FLOOR | -60.0 to 0.0 | -60.0 | dBFS | Minimum decibel floor setting. At -60 dB (OFF), floor leveling is inactive. |
| Output | OUTPUT | -12.0 to +12.0 | 0.0 | dB | Master clean output makeup trim. |

## Visual Indicators

- **IN Lamp:** Lights warm gold when engine is active.
- **Glass Display:** Recessed status display showing real-time upward gain boost (`+X.X dB`) or `PASS-THRU` state.
- **dB Scale:** Etched vertical scale markings alongside the fader travel.

## Supported Formats

- **macOS (Apple Silicon & Intel):** Audio Unit (AU v2), VST3
- **Windows (64-bit):** VST3

## Building

Build via CMake (requires JUCE 9+):

```bash
cmake -B build -DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/juce-src
cmake --build build --config Release
```
