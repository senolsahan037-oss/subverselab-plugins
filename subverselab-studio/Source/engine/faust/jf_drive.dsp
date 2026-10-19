// JING FANG drive: the studio's hw_tube stage (our own recipe: pre-emphasis -> drive -> asymmetric soft clip whose bias
// grows with the level, programme-dependent sag -> DC block -> de-emphasis -> low bump -> top roll), stereo-linked.
// Small-signal gain 1, so drive changes colour, not level. The plug-in mixes dry / wet (exact bypass at mix 0).
declare name "jf_drive";
import("stdfaust.lib");
drive = hslider("drive_db", 6, 0, 30, 0.1);
emph = hslider("emph_db", 4, 0, 12, 0.1);
hf = hslider("hf_hz", 17000, 3000, 22000, 1);
bias = 0.15; biasEnv = 0.5; sag = 0.25; sagT = 30; emphF = 3000; lowDb = 0.8; lowF = 110;
gin = ba.db2linear(drive);
norm = gin * (1 - ma.tanh(bias) ^ 2);
// first-order shelves, post = the exact inverse of pre (same bilinear warp), so the small-signal path has no phase left over
// to cancel against the dry in a mix (the library's 3rd-order shelves turned 60 Hz by -75 deg: a 50 % mix lost 5 dB on basses)
eg = ba.db2linear(emph); wE = 2 * ma.PI * emphF;
pre = fi.tf1s(eg, 1, 1, wE);
post = fi.tf1s(1 / eg, 1 / eg, 1 / eg, wE);
det(l, r) = max(abs(l), abs(r)) * gin : ^(2) : si.smooth(ba.tau2pole(sagT / 1000)) : sqrt : min(4);
clip(e, x) = (ma.tanh(g * x + b) - ma.tanh(b)) / norm
with { g = gin / (1 + sag * e); b = bias * (1 + biasEnv * e); };
tone = fi.dcblockerat(10) : post : fi.low_shelf1(lowDb, lowF) : fi.lowpass(1, hf);
tube(l, r) = (clip(e, pl) : tone), (clip(e, pr) : tone)
with { pl = pre(l); pr = pre(r); e = det(pl, pr); };
process = tube;
