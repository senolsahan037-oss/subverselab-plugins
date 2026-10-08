#!/usr/local/bin/python3
"""KOR half-band FIR design (SPEC.md, Engine step 3 and 5). Prints the C++ array pasted into Source/KorEngine.h.

Stage 1 uses the existing 91-tap design. Stages 2 and 3 use separate shorter designs, with passbands of
0.125 and 0.0625 of their respective running rates.

Design (normalised to the rate the filter runs at, fs = 1):
  passband   0 .. 0.2083  (20 kHz at 96 kHz, i.e. 20 kHz of a 48 kHz base rate)
  transition 0.2083 .. 0.2917 (20 .. 28 kHz at 96 kHz); half-band: symmetric around 0.25, -6.02 dB at 0.25
  stopband   0.2917 .. 0.5, required attenuation >= 120 dB
  Method: the exact half-band construction (Vaidyanathan): a type-II equiripple filter g of length 2m is designed
  with Parks-McClellan over [0, 2 * 0.2083]; h = [g0/2, 0, g1/2, 0, ..., 0.5 (centre), ...], length N = 4m - 1.
  Every second tap is exactly zero and the centre is exactly 0.5, so the odd polyphase branch is a pure delay.
  The shortest N meeting 120 dB is chosen. The script prints the achieved numbers.
"""
import sys
import numpy as np
import scipy.signal as ss

NEED_DB = 120.0


def design(ntaps):
    assert (ntaps + 1) % 4 == 0
    g = ss.remez((ntaps + 1) // 2, [0, 2 * FPASS], [1], fs=1, maxiter=1000)
    h = np.zeros(ntaps)
    h[0::2] = g / 2
    h[ntaps // 2] = 0.5
    return h


def response(h):
    w, H = ss.freqz(h, worN=1 << 17, fs=1)
    a = np.abs(H)
    stop = 20 * np.log10(np.max(a[w >= FSTOP]))
    ripple = 20 * np.log10(np.max(np.abs(a[w <= FPASS] - 1)) + 1)
    return stop, ripple


def emit(name, fpass):
    global FPASS, FSTOP
    FPASS, FSTOP = fpass, 0.5 - fpass
    for n in range(31, 400, 4):
        h = design(n)
        stop, ripple = response(h)
        if -stop >= NEED_DB:
            break
    else:
        sys.exit("no design met the requirement")
    # Check what the float32 coefficients achieve (the engine runs in float).
    hf = h.astype(np.float32).astype(np.float64)
    stop_f, ripple_f = response(hf)
    side = h[0::2]                                    # the 2m non-zero side taps, symmetric
    m2 = len(side)
    print(f"// {name}: half-band FIR, designed by tools/design_halfband.py (scipy {__import__('scipy').__version__} remez, exact half-band construction).")
    print(f"// N = {n} taps (centre 0.5, every second tap zero), group delay {(n - 1) // 2} samples at the rate it runs at.")
    print(f"// Passband 0 .. {FPASS:.4f} fs, stopband {FSTOP:.4f} .. 0.5 fs (20 / 28 kHz at 96 kHz).")
    print(f"// Stopband attenuation {-stop:.1f} dB (float32 taps: {-stop_f:.1f} dB), passband ripple {ripple:.2e} dB (float32: {ripple_f:.2e} dB).")
    print(f"static constexpr int kHbTaps = {n};")
    print(f"static constexpr int kHbSide = {m2};   // non-zero side taps h[0], h[2], ..., h[N-1]")
    print("static constexpr float kHbCoef[kHbSide] = {")
    for i in range(0, m2, 4):
        print("    " + ", ".join(f"{v: .9e}f" for v in side[i:i + 4]) + ",")
    print("};")


def main():
    emit("Stage 2 (2x <-> 4x)", 0.125)
    print()
    emit("Stage 3 (4x <-> 8x)", 0.0625)


if __name__ == "__main__":
    main()
