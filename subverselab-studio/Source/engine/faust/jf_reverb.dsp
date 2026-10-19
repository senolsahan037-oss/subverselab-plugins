// JING FANG reverb: the studio's room recipe (hw_room / space: Faust's zita_rev1, a public algorithm). The send is
// high-passed so the low end stays dry (no mud under basses and 808s); pre-delay keeps the attack in front; T60 in
// seconds; damping as the mid-band's high-frequency T60 crossover; size = low-band decay ratio and wet width. The plug-in
// mixes dry / wet itself (exact bypass at mix 0).
declare name "jf_reverb";
import("stdfaust.lib");
t60 = hslider("t60", 2.0, 0.2, 12, 0.01);
dampHz = hslider("damp_hz", 6000, 1200, 18000, 1);
pre = hslider("predelay_ms", 15, 0, 200, 1);
size = hslider("size", 0.5, 0, 1, 0.001);
hp = hslider("send_hp_hz", 180, 20, 1000, 1);
lowRatio = 0.75 + 0.7 * size;
width = 0.55 + 0.6 * size;
send = fi.highpass(2, hp) : de.fdelay(ma.SR * 0.25, pre * ma.SR / 1000);
ms(l, r) = (l + r) * 0.5 + (l - r) * 0.5 * width, (l + r) * 0.5 - (l - r) * 0.5 * width;
process = (send, send) : re.zita_rev1_stereo(0, 200, dampHz, t60 * lowRatio, t60, ma.SR) : ms;
