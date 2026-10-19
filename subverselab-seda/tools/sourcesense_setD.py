#!/usr/bin/env python3
"""Set D for SourceSense: the eight demo stems of the two listening packs, labelled by hand BEFORE any rule was looked at (9 Oct, P1c).

These are NOT made by the generators sourcesense_set.py fits and tests on (sets A, B, C), so D is independent of the rule fit.
  seda/vocal.wav   formant vocal line               -> vocal
  seda/lead.wav    mono synth lead                  -> keys   (the table's "synth/keys" row covers a lead)
  seda/drums.wav   drum loop, 8 s, played twice     -> drums  (looped to 16 s: SourceSense answers after 10 s)
  seda/pluck.wav   guitar-like pluck arpeggio       -> keys
  dyneq/beat.wav   drums about 5 dB over a pad bed  -> drums  (judgement call: a two-element mix whose rhythm is the drums; reported with the alternative label fullmix too)
  dyneq/bass.wav   bass line                        -> bass
  dyneq/kick.wav   four-on-the-floor kick           -> kick
  dyneq/vocal.wav  formant vocal line               -> vocal
Usage: sourcesense_setD.py --out DIR   (writes the wavs and labels.json that sourcesense_eval.py reads)
"""
import argparse, json, os, shutil
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
SEDA = os.path.join(HERE, "demo_material"); DYN = os.path.join(HERE, "..", "..", "subverselab-dyneq", "tools", "demo_material")
LABELS = {"seda_vocal.wav": ("vocal", SEDA, "vocal.wav"), "seda_lead.wav": ("keys", SEDA, "lead.wav"), "seda_drums.wav": ("drums", SEDA, "drums.wav"), "seda_pluck.wav": ("keys", SEDA, "pluck.wav"),
          "dyneq_beat.wav": ("drums", DYN, "beat.wav"), "dyneq_bass.wav": ("bass", DYN, "bass.wav"), "dyneq_kick.wav": ("kick", DYN, "kick.wav"), "dyneq_vocal.wav": ("vocal", DYN, "vocal.wav")}

def make_set(out):
    import wave, struct
    os.makedirs(out, exist_ok=True); labels = {}
    for name, (cls, d, src) in LABELS.items():
        dst = os.path.join(out, name)
        if name == "seda_drums.wav":   # 8 s loop twice
            w = wave.open(os.path.join(d, src)); p = w.getparams(); fr = w.readframes(w.getnframes()); w.close()
            o = wave.open(dst, "wb"); o.setparams(p); o.writeframes(fr + fr); o.close()
        else: shutil.copyfile(os.path.join(d, src), dst)
        labels[name] = dict(cls=cls)
    json.dump(labels, open(os.path.join(out, "labels.json"), "w"), indent=1)
    return labels

if __name__ == "__main__":
    ap = argparse.ArgumentParser(); ap.add_argument("--out", required=True); a = ap.parse_args(); print(f"wrote {len(make_set(a.out))} clips to {a.out}")
