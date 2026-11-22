#!/usr/bin/env python3
"""Summarise logs/preset_measure/rows.json: per preset and material, median / p90 / worst, and the factory-preset gates.
Gates (proposed 21 Nov, a factory preset must pass on the material it is made for):
  mono      monoDelta p90 <= 1.0 LU      (summed to mono it loses at most 1 LU more than the original did)
  worst     corrP10 median >= 0.0        (its worst 400 ms moments do not go out of phase)
  antiPh    antiPhD p90 <= 5 points      (energy that vanishes in mono grows by at most 5 points)
  bass      lowSide median <= -30 dB     (only for presets with MONO BASS)
  effect    widthDb median >= +1.5 dB for widening presets, <= -3 dB for narrowing ones
  peak      reported: median / max rise of the sample peak (a preset trims its output by about the median rise)
  The effect gates ignore the preset's OUTPUT trim (a trim is level, not width or fill).
Usage: preset_summary.py [rows.json] [--md out.md]
"""
import json, os, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
src = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("--") else os.path.join(HERE, "..", "logs", "preset_measure", "rows.json")
md_out = sys.argv[sys.argv.index("--md") + 1] if "--md" in sys.argv else None
d = json.load(open(src))
rows, presets = d["rows"], d["presets"]
ROLES = ["Full", "Chords", "Texture", "Counter", "Hook", "Bass", "Hats"]
# what each preset is for (gates are judged there; other material is reported only)
TARGET = {"Default": ["Full"], "Mono Safe Wide": ["Full"], "Wide Master": ["Full"], "Vocal Stays Centre": ["Full"],
          "Air Wide": ["Full", "Hats"], "Tight Low End": ["Full", "Bass"], "Fill the Gap": ["Full", "Chords", "Counter"],
          "Behind You": ["Full", "Texture"], "Narrow Pad": ["Chords", "Texture", "Counter"]}
NARROW = {"Narrow Pad", "Tight Low End"}


def base(name):
    """Variant names ("Behind You 170 FIX") are judged as their preset ("Behind You")."""
    return max((k for k in TARGET if name.startswith(k)), key=len, default=name)


def stats(v):
    v = np.asarray(v, float)
    return np.median(v), np.percentile(v, 90), np.percentile(v, 10), v.max(), v.min()


lines = []
out = lines.append
out(f"# HIGH STEREO preset proposals measured on the sample packs\n\nSource: `{os.path.relpath(src)}`, {len(rows)} renders.\n")
out("| Preset | Material | n | dLU | mono loss vs orig (med / p90) | mono sum change | wider (dB) | corr p10 (med / worst) | vanishes in mono % (in -> out, p90 rise) | bass side dB | peak rise (med / max) | gates |")
out("|---|---|---|---|---|---|---|---|---|---|---|---|")
verdict = {}
for name, prm in presets.items():
    for role in ROLES:
        rs = [r for r in rows if r["preset"] == name and r["role"] == role]
        if not rs: continue
        g = lambda k: [r[k] for r in rs]
        dlu = stats(g("dLU")); md = stats(g("monoDelta")); mc = stats(g("monoChange")); wd = stats(g("widthDb"))
        cp = stats(g("corrP10")); ai = stats(g("antiIn")); ao = stats(g("anti")); ad = stats(g("antiPhD")); lo = stats(g("lowSide"))
        pr = stats([r["peak"] - r["peakIn"] for r in rs])
        fails = []
        b = base(name)
        if role in TARGET.get(b, []):
            if md[1] > 1.0: fails.append("mono")
            if cp[0] < 0.0: fails.append("worst")
            if ad[1] > 5.0: fails.append("antiPh")
            if "monobass" in prm and lo[0] > -30.0: fails.append("bass")
            if b == "Fill the Gap":   # SPREAD fills toward the centre: effect = mono sum up (OUTPUT trim taken out), nothing new vanishing
                if not (mc[0] - float(prm.get("output", 0)) >= 0.3 and ad[0] <= 0.5): fails.append("effect")
            elif b != "Default":
                if b in NARROW and b != "Tight Low End" and wd[0] > -3.0: fails.append("effect")
                if b not in NARROW and wd[0] < 1.5: fails.append("effect")
            verdict.setdefault(name, []).extend(f"{role}:{f}" for f in fails)
        gate = ("PASS" if not fails else "FAIL " + ",".join(fails)) if role in TARGET.get(b, []) else "-"
        out(f"| {name} | {role} | {len(rs)} | {dlu[0]:+.2f} | {md[0]:+.2f} / {md[1]:+.2f} | {mc[0]:+.2f} | {wd[0]:+.2f} | "
            f"{cp[0]:+.2f} / {cp[4]:+.2f} | {ai[0]:.1f} -> {ao[0]:.1f}, {ad[1]:+.1f} | {lo[0]:.1f} | {pr[0]:+.1f} / {pr[3]:+.1f} | {gate} |")
out("\n## Verdict per preset (on its own material)\n")
for name in presets:
    if not any(r["preset"] == name for r in rows):
        out(f"- **{name}**: NO DATA"); continue
    v = verdict.get(name, [])
    out(f"- **{name}** {json.dumps(presets[name])}: " + ("PASS" if not v else "FAIL " + ", ".join(v)))
txt = "\n".join(lines)
print(txt)
if md_out: open(md_out, "w").write(txt + "\n")
