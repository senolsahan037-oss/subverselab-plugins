#pragma once

#include <cstdint>

// The knobs' mouse maths, plain C++ so the selftest can drive it with simulated events.
namespace kiyma
{
/** Absolute vertical drag: the value follows the TOTAL distance from where the mouse went down, not the sum of per-event deltas.
    (A per-event delta of less than half a step snaps back to the same step on a stepped parameter, so such a knob never moved.)
    200 px for the whole range, a sixth of that with shift. */
struct KnobDrag
{
    float startNorm = 0.0f, startY = 0.0f;
    void begin (float norm, float y) noexcept { startNorm = norm; startY = y; }
    float at (float y, bool shift) const noexcept { return startNorm - (y - startY) / (shift ? 1200.0f : 200.0f); }
};

/** A parameter with 2 to 32 steps is moved by whole steps with the wheel. */
inline bool isStepped (int numSteps) noexcept { return numSteps > 1 && numSteps <= 32; }

/** The mouse wheel on a stepped parameter: exactly one step per wheel event, and events closer than 120 ms to the last accepted one are
    ignored (a trackpad sends a stream of them). Returns -1, 0 or +1 steps. */
struct WheelStep
{
    uint32_t lastMs = 0;
    bool have = false;
    int onWheel (float deltaY, bool reversed, uint32_t nowMs) noexcept
    {
        if (deltaY == 0.0f) return 0;
        if (have && nowMs - lastMs < 120u) return 0;
        have = true;
        lastMs = nowMs;
        return ((reversed ? -deltaY : deltaY) > 0.0f) ? 1 : -1;
    }
};
} // namespace kiyma
