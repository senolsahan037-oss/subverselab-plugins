#!/usr/bin/env python3
"""Create the deterministic 4-minute, 48 kHz stereo performance fixture."""
import math, os, struct, sys, wave
out = sys.argv[1] if len(sys.argv) > 1 else "tests/fixtures/mihenk_4m.wav"
os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
sr, seconds = 48000, 240
with wave.open(out, "wb") as w:
    w.setnchannels(2); w.setsampwidth(3); w.setframerate(sr)
    for i in range(sr * seconds):
        t=i/sr; beat=t%0.5; section=0.35 if (t%16.0)<8 else 1.0
        kick=math.exp(-beat*18)*math.sin(2*math.pi*(48*beat+60*(1-math.exp(-beat*30))/30))
        bass=.25*math.sin(2*math.pi*55*t)*(.6+.4*math.sin(2*math.pi*.5*t))
        hat=math.exp(-((t+.25)%0.25)*60)*(((i*1103515245+12345)&0x7fffffff)/0x7fffffff*2-1)*.15
        x=max(-1,min(1,section*(.55*kick+bass)+hat)); y=max(-1,min(1,section*(.55*kick+bass)-hat))
        a, b = int(x*8388607), int(y*8388607)
        w.writeframesraw(bytes((a & 255, (a >> 8) & 255, (a >> 16) & 255,
                                b & 255, (b >> 8) & 255, (b >> 16) & 255)))
