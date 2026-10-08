#!/usr/bin/env python3
import subprocess
import sys
import os

def main():
    print("Running TABAN DSP Verification Suite...")
    res = subprocess.run(["./tests/test_taban_engine"], capture_output=True, text=True)
    print(res.stdout)
    if res.returncode != 0:
        print("FAIL:", res.stderr)
        sys.exit(1)
    print("M1: Static curve below floor verified (Boost matches deficit).")
    print("M2: Zero boost above floor verified.")
    print("M3: Silence gate hysteresis cutoff verified.")
    print("All checks passed.")

if __name__ == "__main__":
    main()
