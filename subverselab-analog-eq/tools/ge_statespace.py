#!/usr/bin/env python3
"""GE-1100 equaliser stage as a continuous state-space model, built from the traced netlist (docs/reference/ge1100/VALUES.md).

States are the 28 capacitor voltages. For each unit state (and for the input) the resistive network is solved by MNA with
every capacitor replaced by a voltage source; the capacitor currents give dx/dt, the OUT node gives y. Ideal op-amps
(nullors). This is the reference the C++ engine is built from and gated against, next to ngspice.

  ge_statespace.py          prints the analog response at the checkpoints and compares with the ngspice runs
"""
import os
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REF = os.path.join(HERE, "..", "docs", "reference", "ge1100")

# (C series, C gyrator, R big, R series) per band, 16 Hz ... 32 kHz (VALUES.md)
BANDS = [(10e-6, 0.22e-6, 100e3, 470), (4.7e-6, 0.12e-6, 91e3, 470), (2.2e-6, 0.068e-6, 91e3, 470),
         (1.22e-6, 0.033e-6, 82e3, 470), (0.57e-6, 0.018e-6, 82e3, 470), (0.25e-6, 0.012e-6, 68e3, 470),
         (0.15e-6, 0.0068e-6, 51e3, 470), (0.068e-6, 0.0047e-6, 43e3, 470), (0.039e-6, 0.0022e-6, 39e3, 470),
         (0.018e-6, 0.0022e-6, 24e3, 430), (0.01e-6, 0.0012e-6, 20e3, 430), (0.0056e-6, 680e-12, 16e3, 390)]
LABELS = [16, 32, 64, 125, 250, 500, 1000, 2000, 4000, 8000, 16000, 32000]
RPOT = 100e3
RMIN = 1.0   # a pot at its end or at its tap: 1 ohm instead of 0


def netlist(pos):
    """pos[i] in [0, 1]: wiper from the N end (0) to the MID end (1); 0.5 = centre tap (flat).
    Returns (resistors, capacitors, opamps, nodes) with node 0 = ground and node 'src' driven by the input."""
    R, C, OA = [], [], []
    R += [("a", "P", 30e3), ("P", 0, 56e3), ("out1", "N", 2.7e3), ("out1", "MID", 2.7e3),
          ("out2", "n2", 20e3), ("n2", 0, 36e3), ("o3", "OUT", 620.0), ("OUT", 0, 18e3)]
    C += [("src", "a", 6.8e-6), ("out1", "N", 470e-12), ("out2", "n2", 68e-12), ("out2", "o3", 4.7e-6)]
    OA += [("out1", "P", "N"), ("out2", "MID", "n2")]                  # (output, +, -)
    for i, ((cs, cg, rb, rs), x) in enumerate(zip(BANDS, pos)):
        w, g, p = f"w{i}", f"g{i}", f"p{i}"
        x = min(max(x, 0.0), 1.0)
        if x <= 0.5:
            R += [("N", w, max(x * RPOT, RMIN)), (w, 0, max((0.5 - x) * RPOT, RMIN)), (0, "MID", 0.5 * RPOT)]
        else:
            R += [("N", 0, 0.5 * RPOT), (0, w, max((x - 0.5) * RPOT, RMIN)), (w, "MID", max((1 - x) * RPOT, RMIN))]
        C += [(w, g, cs), (g, p, cg)]
        R += [(p, 0, rb), (f"q{i}", g, rs)]
        OA += [(f"q{i}", p, f"q{i}")]                                   # follower: q = p
    nodes = sorted({n for e in R + C for n in e[:2] if n != 0} | {n for o in OA for n in o if n != 0}, key=str)
    return R, C, OA, nodes


def state_space(pos):
    R, Cs, OA, nodes = netlist(pos)
    ix = {n: k for k, n in enumerate(nodes)}
    nn, nc, no = len(nodes), len(Cs), len(OA)
    size = nn + nc + no + 1                                            # node voltages, cap currents, op-amp currents, input current
    M = np.zeros((size, size))
    def stamp_g(a, b, g):
        for u, v, s in ((a, a, 1), (b, b, 1), (a, b, -1), (b, a, -1)):
            if u != 0 and v != 0: M[ix[u], ix[v]] += s * g
    for a, b, r in R: stamp_g(a, b, 1.0 / r)
    vs = [(a, b) for a, b, _ in Cs] + [("src", 0)]                    # voltage sources: caps, then the input
    for k, (a, b) in enumerate(vs):
        row = nn + (k if k < nc else nc + no)
        for node, s in ((a, 1), (b, -1)):
            if node != 0: M[ix[node], row] += s; M[row, ix[node]] += s
    for k, (o, plus, minus) in enumerate(OA):
        col = nn + nc + k
        M[ix[o], col] += 1                                              # unknown current out of the op-amp into its output
        for node, s in ((plus, 1), (minus, -1)):
            if node != 0: M[col, ix[node]] += s                          # constraint V+ - V- = 0
    Minv = np.linalg.inv(M)
    capC = np.array([c for _, _, c in Cs])
    def solve(xstate, u):
        rhs = np.zeros(size)
        rhs[nn:nn + nc] = xstate; rhs[nn + nc + no] = u
        z = Minv @ rhs
        icap = z[nn:nn + nc]   # MNA source current = the current through the capacitor from its + to its - node
        # (26 Oct: the sign was flipped first; magnitude still matched ngspice, every pole was mirrored to the right half
        # plane. Gate on stability and phase, not on magnitude alone.)
        return icap / capC, z[ix["OUT"]]
    A = np.zeros((nc, nc)); Bm = np.zeros(nc); Cm = np.zeros(nc)
    for j in range(nc):
        e = np.zeros(nc); e[j] = 1
        A[:, j], Cm[j] = solve(e, 0.0)
    Bm, D = solve(np.zeros(nc), 1.0)
    return A, Bm, Cm, D


def analog_response(pos, f):
    A, B, C, D = state_space(pos)
    s = 2j * np.pi * np.asarray(f)
    I = np.eye(len(B))
    return np.array([C @ np.linalg.solve(si * I - A, B) + D for si in s])


if __name__ == "__main__":
    db = lambda z: 20 * np.log10(np.abs(z))
    runs = [("flat", [0.5] * 12), ("1k_wiperN", [0.5] * 6 + [0.0] + [0.5] * 5), ("1k_wiperMID", [0.5] * 6 + [1.0] + [0.5] * 5)]
    for name, pos in runs:
        path = os.path.join(REF, f"ac_{name}.txt")
        d = np.loadtxt(path); f, v = d[:, 0], d[:, 1]
        sel = (f >= 20) & (f <= 20000)
        h = db(analog_response(pos, f[sel]))
        print(f"{name:12s} state-space vs ngspice, 20 Hz-20 kHz: max |diff| {np.max(np.abs(h - v[sel])):.4f} dB; at 1 kHz {np.interp(1000, f[sel], h):+.2f} dB")
