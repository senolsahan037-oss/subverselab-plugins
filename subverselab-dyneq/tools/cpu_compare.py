#!/usr/bin/env python3
"""CPU of the P1h engine (tools/baseline/dyneqraw_p1h) against the current build, alternating old / new within every round, in one session (TASK_P1i 1).
  cpu_compare.py [rounds=3]
Prints realtime_x per run (one render per test, binary and round) and the load average before and after. The tests are the ones of measure.py: A9, A9b, A13e, T6 (three configurations),
and, new binary only, T6 with SLOPE 72 (the P1h engine has no SLOPE)."""
import os, sys, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import measure as m

rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 3
new_raw, old_raw = os.environ.get("CPU_NEW", m.RAW), os.environ.get("CPU_OLD", m.BASELINE_RAW)
main_a9, key_a9 = m.pink(60.0, -12, 31), m.pink(60.0, -18, 32)
bands = {1: "type=bell,f=200,g=2,q=1,dyn=1,thr=-40,range=-6,trig=key", 2: "type=lowshelf,f=120,g=-2,q=0.7,dyn=1,thr=-40,range=4", 3: "type=bell,f=1000,g=0,q=2,dyn=1,thr=-40,range=-6,trig=key",
         4: "type=highshelf,f=8000,g=1,q=0.7,dyn=1,thr=-40,range=-4", 5: "type=bell,f=3000,g=-1,q=3,dyn=1,thr=-40,range=5", 6: "type=bell,f=5000,g=0,q=1.5,dyn=1,thr=-40,range=-6,trig=key"}
res13 = m.resonant_pink(60.0, lambda t: 2500.0, seed=33)
rng = np.random.default_rng(10); secs = 60.0; ts = [0.1 + 0.25 * k for k in range(int(secs / 0.25) - 3)]
drums = m.place([m.kick_hit(rng, 0.25) if k % 2 == 0 else m.snare_hit(rng, 0.25) for k in range(len(ts))], ts, secs)
main6 = (m.pink(secs, -26, 31) + 0.5 * drums).astype(np.float32); key6 = m.pink(secs, -30, 32)
T6 = {"T6 SNARE+REVERB-READY": "inst=snare,part=body,amount=12,width=1,reverb=1", "T6 DRUMS snare body": "inst=drums,part=snarebody,amount=12,width=1", "T6 KICK+CLICK": "inst=kick,part=click,amount=12,width=1"}
tests = [("A9", lambda: m.run(main_a9, key_a9, depth=12, margin=3, want_log=False, timing=True)[2], True),
         ("A9 no listening", lambda: m.run(main_a9, key_a9, depth=12, margin=3, want_log=False, timing=True, extra=["--no-listen"])[2], True),
         ("A9b", lambda: m.run(main_a9, key_a9, depth=12, margin=3, want_log=False, timing=True, bands=bands)[2], True),
         ("A13e", lambda: m.run(res13, None, mode="self", depth=12, margin=3, want_log=False, timing=True)[2], True)]
for name, spec in T6.items():
    tests.append((name, (lambda sp: lambda: m.run(main6, key6, depth=12, margin=3, want_log=False, timing=True, target=sp)[2])(spec), True))
for name, spec in T6.items():
    tests.append((name + " @72", (lambda sp: lambda: m.run(main6, key6, depth=12, margin=3, want_log=False, timing=True, target=sp + ",slope=72")[2])(spec), False))
load = lambda: subprocess.run(["uptime"], capture_output=True, text=True).stdout.split("load averages:")[1].strip()
print(f"load before: {load()}")
out = {n: {"old": [], "new": []} for n, _, _ in tests}
for r in range(rounds):
    for name, fn, both in tests:
        if both: m.RAW = old_raw; out[name]["old"].append(fn())
        m.RAW = new_raw; out[name]["new"].append(fn())
    print(f"round {r + 1} done, load {load()}", flush=True)
print(f"\n{'test':28s} {'P1h engine (x real time, per round)':40s} {'this build':40s}  best old / best new")
for name, _, both in tests:
    o, n = out[name]["old"], out[name]["new"]
    print(f"{name:28s} {', '.join(f'{v:6.1f}' for v in o) if o else '-':40s} {', '.join(f'{v:6.1f}' for v in n):40s}  {max(o) if o else float('nan'):6.1f} / {max(n):6.1f}")
print(f"load after: {load()}")
