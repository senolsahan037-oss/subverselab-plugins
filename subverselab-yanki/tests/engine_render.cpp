#include "../Source/YankiEngine.h"
#include "wav.h"
#include "presets.h"
#include <complex>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <random>

static yanki::Controls preset(const std::string& name) { return yankiPreset(name); }
static double rms(const std::vector<float>& x) { double s = 0; for (float v : x) s += static_cast<double>(v) * v; return std::sqrt(s / std::max<size_t>(1, x.size())); }
static double peak(const std::vector<float>& x) { double p = 0; for (float v : x) p = std::max(p, std::abs(static_cast<double>(v))); return p; }
static void process(std::vector<float>& x, int sr, const yanki::Controls& c) { yanki::YankiEngine e; e.prepare(sr); std::vector<float> l(x.size()/2), r(x.size()/2); for (size_t i=0;i<l.size();++i) l[i]=x[i*2], r[i]=x[i*2+1]; e.process(l.data(), r.data(), static_cast<int>(l.size()), c); for (size_t i=0;i<l.size();++i) x[i*2]=l[i], x[i*2+1]=r[i]; }
static bool finiteAndBound(const std::vector<float>& x, double bound=1.995) { for(float v:x) if(!std::isfinite(v)||std::abs(v)>=bound) return false; return true; }
static bool same(const std::vector<float>& a, const std::vector<float>& b) { return a == b; }
static double centroid(const std::vector<float>& x, size_t begin, int sr) {
    double sum = 0.0, weighted = 0.0; const size_t n = 512;
    for (size_t k=0;k<n/2;++k) { double re=0, im=0; for(size_t i=0;i<n && (begin+i)*2<x.size();++i){double w=0.5-0.5*std::cos(2*3.141592653589793*i/n); double ph=2*3.141592653589793*k*i/n; re+=x[(begin+i)*2]*w*std::cos(ph); im-=x[(begin+i)*2]*w*std::sin(ph);} double mag=std::sqrt(re*re+im*im); sum+=mag; weighted+=mag*(double(k)*sr/n); } return sum>0?weighted/sum:0.0;
}
static void line(const char* n, bool ok, const std::string& v) { std::cout << (ok ? "PASS " : "FAIL ") << n << " " << v << "\n"; }
static void line(const std::string& n, bool ok, const std::string& v) { line(n.c_str(), ok, v); }

static std::vector<float> burstInput(size_t n, int sr) {
    std::vector<float> x(n * 2, 0.0f);
    const size_t burst = static_cast<size_t>(1.5 * sr);
    for (size_t i = 0; i < std::min(n, burst); ++i) {
        const double t = double(i) / sr;
        const float env = static_cast<float>(0.55 * (0.8 + 0.2 * std::sin(2.0 * 3.141592653589793 * 3.0 * t)));
        const float v = env * static_cast<float>(std::sin(2.0 * 3.141592653589793 * 220.0 * t));
        x[i * 2] = x[i * 2 + 1] = v;
    }
    return x;
}

static double windowRms(const std::vector<float>& x, size_t beginFrame, size_t frames) {
    const size_t begin = beginFrame * 2;
    const size_t end = std::min(x.size(), begin + frames * 2);
    double s = 0.0; size_t count = 0;
    for (size_t i = begin; i < end; ++i) { s += double(x[i]) * x[i]; ++count; }
    return std::sqrt(s / std::max<size_t>(1, count));
}


// ---- stage-voicing checks (Yesilcam Sahne / Kahraman Donusu / Ters Yanki) ----
static std::vector<float> vocalBurst(int sr, double totalSec) {
    // Additive sung-vowel stand-in: f0 196 Hz with 5.5 Hz vibrato, harmonics
    // shaped by three formant bumps, 4 Hz syllable swell, 20 ms edges.
    const size_t n = static_cast<size_t>(totalSec * sr), burst = static_cast<size_t>(1.5 * sr);
    std::vector<float> x(n * 2, 0.0f);
    double amp[60]; int nh = 0;
    for (int h = 1; h < 60; ++h) {
        const double f = 196.0 * h; if (f > 0.42 * sr) break;
        const auto bump = [&](double fc, double bw, double g) { const double d = (f - fc) / bw; return g * std::exp(-0.5 * d * d); };
        amp[h - 1] = (1.0 / h) * (0.25 + bump(700, 150, 3.0) + bump(1200, 200, 2.0) + bump(2700, 350, 1.2)) + 0.004; nh = h;
    }
    std::vector<double> ph(static_cast<size_t>(nh), 0.0); double peakV = 0.0; std::vector<double> y(burst, 0.0);
    for (size_t i = 0; i < burst; ++i) {
        const double t = double(i) / sr; const double f0 = 196.0 * (1.0 + 0.015 * std::sin(2 * 3.141592653589793 * 5.5 * t));
        double v = 0.0; for (int h = 0; h < nh; ++h) { ph[h] += 2 * 3.141592653589793 * f0 * (h + 1) / sr; if (ph[h] > 6.283185307179586) ph[h] -= 6.283185307179586; v += amp[h] * std::sin(ph[h]); }
        const double edge = std::min(1.0, std::min(t / 0.02, (1.5 - t) / 0.02));
        v *= edge * (0.75 + 0.25 * std::sin(2 * 3.141592653589793 * 4.0 * t)); y[i] = v; peakV = std::max(peakV, std::abs(v));
    }
    for (size_t i = 0; i < burst; ++i) x[i * 2] = x[i * 2 + 1] = static_cast<float>(0.5 * y[i] / peakV);
    return x;
}
static double spanRms(const std::vector<float>& x, int sr, double t0, double t1) {
    const size_t a = static_cast<size_t>(std::max(0.0, t0) * sr), b = static_cast<size_t>(t1 * sr);
    return windowRms(x, a, b > a ? b - a : 0);
}
static double centroidSpan(const std::vector<float>& x, int sr, double t0, double t1) {
    const size_t N = 1024; static std::vector<double> cs, sn; if (cs.size() != N) { cs.resize(N); sn.resize(N); for (size_t i = 0; i < N; ++i) { cs[i] = std::cos(2 * 3.141592653589793 * i / N); sn[i] = std::sin(2 * 3.141592653589793 * i / N); } }
    std::vector<double> mag(N / 2, 0.0); const size_t a = static_cast<size_t>(t0 * sr), b = static_cast<size_t>(t1 * sr);
    std::vector<double> fr(N);
    for (size_t s = a; s + N <= b && (s + N) * 2 <= x.size(); s += N) {
        for (size_t i = 0; i < N; ++i) fr[i] = 0.5 * (x[(s + i) * 2] + x[(s + i) * 2 + 1]) * (0.5 - 0.5 * cs[i]);
        for (size_t k = 1; k < N / 2; ++k) { double re = 0, im = 0; for (size_t i = 0; i < N; ++i) { const size_t q = (k * i) % N; re += fr[i] * cs[q]; im -= fr[i] * sn[q]; } mag[k] += std::sqrt(re * re + im * im); }
    }
    double sum = 0, w = 0; for (size_t k = 1; k < N / 2; ++k) { sum += mag[k]; w += mag[k] * double(k) * sr / N; } return sum > 0 ? w / sum : 0.0;
}
static double dbOf(double r, double ref) { return 20.0 * std::log10(std::max(r, 1e-12) / std::max(ref, 1e-12)); }
static std::string fmt(double v, int p = 2) { char b[64]; std::snprintf(b, sizeof b, "%.*f", p, v); return b; }

struct StageResult { bool ok = true; int checks = 0; };
static void stageChecks(const std::string& name, int sr, StageResult& R) {
    const yanki::Controls pc = preset(name);
    const double D = pc.timeMs * 0.001;
    const auto in = vocalBurst(sr, 5.5);
    const double burstRms = spanRms(in, sr, 0.0, 1.5);
    auto full = in; process(full, sr, pc);
    yanki::Controls wc = pc; wc.mix = 1.0; auto wet = in; process(wet, sr, wc);
    // 1) first repeat, wet only (dry muted) vs the same burst segment
    const double first = dbOf(spanRms(wet, sr, D + 0.01, 2 * D - 0.01), spanRms(in, sr, 0.01, D - 0.01));
    const bool okFirst = first >= -10.0 && first <= -5.0;
    line("stage_first_repeat_" + name, okFirst, "db_rel_burst=" + fmt(first) + " window=" + fmt(D + 0.01, 3) + "-" + fmt(2 * D - 0.01, 3) + "s range=[-10,-5]");
    // 2) repeat staircase after the burst: step k = [1.5+(k-1)D, 1.5+kD]
    std::string steps; int count = 0; bool counting = true; std::vector<double> cents;
    for (int k = 1; 1.5 + k * D <= 5.5 + 1e-9; ++k) {
        const double t0 = 1.5 + (k - 1) * D + 0.01, t1 = 1.5 + k * D - 0.01;
        const double lv = dbOf(spanRms(full, sr, t0, t1), burstRms);
        steps += (k == 1 ? "" : ",") + fmt(lv, 1);
        if (counting && lv > -40.0) { ++count; cents.push_back(centroidSpan(full, sr, t0, t1)); } else counting = false;
    }
    const bool okCount = count >= 5 && count <= 8;
    line("stage_repeat_count_" + name, okCount, "above_-40dB=" + std::to_string(count) + " step_s=" + fmt(D, 3) + " steps_db=" + steps + " range=[5,8]");
    // 3) tail
    const double tail = dbOf(spanRms(full, sr, 5.0, 5.5), burstRms);
    const bool okTail = tail < -50.0;
    line("stage_tail_" + name, okTail, "db_rel_burst_5.0-5.5s=" + fmt(tail) + " (3.5-4.0 s after burst end) limit=-50");
    // 4) darker per repeat
    bool darker = cents.size() >= 2; std::string cv;
    for (size_t k = 0; k < cents.size(); ++k) { cv += (k ? "," : "") + fmt(cents[k], 0); if (k && !(cents[k] < cents[k - 1])) darker = false; }
    line("stage_repeats_darker_" + name, darker, "centroid_hz=" + cv);
    // 5) FREEZE after the burst, 20 s
    yanki::YankiEngine e; e.prepare(sr); const size_t burstN = static_cast<size_t>(1.5 * sr), fz = static_cast<size_t>(20 * sr);
    std::vector<float> l(burstN + fz, 0.0f), r(burstN + fz, 0.0f); for (size_t i = 0; i < burstN; ++i) { l[i] = in[i * 2]; r[i] = in[i * 2 + 1]; }
    yanki::Controls fc = pc; for (size_t p = 0; p < burstN; p += 512) e.process(l.data() + p, r.data() + p, int(std::min<size_t>(512, burstN - p)), fc);
    fc.freeze = true; for (size_t p = burstN; p < l.size(); p += 512) e.process(l.data() + p, r.data() + p, int(std::min<size_t>(512, l.size() - p)), fc);
    std::vector<float> fr(l.size() * 2); for (size_t i = 0; i < l.size(); ++i) { fr[i * 2] = l[i]; fr[i * 2 + 1] = r[i]; }
    const double lastDb = dbOf(spanRms(fr, sr, 20.5, 21.5), burstRms); const double pk = peak(fr);
    bool fin = true; for (float v : fr) if (!std::isfinite(v)) fin = false;
    const bool okFreeze = fin && pk < 1.5 && lastDb > -60.0;
    line("stage_freeze_20s_" + name, okFreeze, "last_second_db_rel_burst=" + fmt(lastDb) + " peak=" + fmt(pk, 4) + " limits: peak<1.5 last>-60dB");
    R.ok = R.ok && okFirst && okCount && okTail && darker && okFreeze; R.checks += 5;
}
static void stageFilterInfo(const std::string& name, int sr) {
    // Analytic per-pass loop response (60 Hz low-cut + TONE high shelf), same coefficients as the engine.
    const yanki::Controls c = preset(name); const double tp = 6.283185307179586;
    const double hpC = std::exp(-tp * 60.0 / sr), shC = 1.0 - std::exp(-tp * std::clamp(c.toneHz, 1000.0, 8000.0) / sr), top = 0.66 - 0.20 * std::clamp(c.drama, 0.0, 1.0);
    std::string o; double prev = 0; for (double f : {1000.0, 2000.0, 4000.0, 8000.0, 16000.0}) {
        const std::complex<double> z = std::polar(1.0, -tp * f / sr);
        const std::complex<double> hp = (1.0 - z) / (1.0 - hpC * z), lp = shC / (1.0 - (1.0 - shC) * z), h = hp * (lp + top * (1.0 - lp));
        const double db = 20 * std::log10(std::abs(h)); o += " " + fmt(f / 1000, 0) + "k=" + fmt(db); if (f > 1000) o += "(" + fmt(db - prev) + "/oct)"; prev = db; }
    std::cout << "INFO per_pass_loop_filter_db " << name << o << "\n";
}

#include "swell_tests.h"

static int selftest() {
    constexpr int sr = 48000; const size_t n = sr * 3; yanki::Controls c = preset("ThreeHeads"); c.mix = 0.7;
    std::vector<float> in(n*2, 0), a, b; for(size_t i=0;i<n;++i) in[i*2]=in[i*2+1]=static_cast<float>(0.2*std::sin(2*3.141592653589793*440*i/sr));
    a=in; b=in; process(a,sr,c); process(b,sr,c); line("determinism", same(a,b), "bit-identical");
    auto dry=in; yanki::Controls off=c; off.mix=0; process(dry,sr,off); line("mix_zero_dry", same(in,dry), "bit-exact");
    bool safe=true; for(int rate:{44100,48000,96000}) { std::vector<float> z(static_cast<size_t>(rate/5)*2,0); z[0]=z[1]=1; process(z,rate,c); safe &= finiteAndBound(z); } line("finite_peak_all_rates",safe,"44.1/48/96kHz");
    bool controls=true; const double baseR=rms(a); std::string controlValue; for(int k=0;k<14;++k) { yanki::Controls q=c; if(k==0)q.timeMs=123; if(k==1)q.sync=1; if(k==2)q.bpm=80,q.sync=1; if(k==3)q.mode=1; if(k==4)q.feedback=0.9; if(k==5)q.toneHz=1100; if(k==6)q.drive=0.9; if(k==7)q.wear=0.9; if(k==8)q.width=0.1; if(k==9)q.mix=0.15; if(k==10)q.drama=1.0; if(k==11)q.freeze=true; if(k==12)q.spread=1.0; if(k==13)q.reverse=1.0; auto v=in; process(v,sr,q); double d=0; for(size_t i=0;i<v.size();++i)d+=std::abs(v[i]-a[i]); controls &= d > baseR*0.001; if(d<=baseR*0.001) controlValue += " k"+std::to_string(k)+"="+std::to_string(d); } line("controls_audible",controls,controls?"all_14_changed_output":"unchanged:"+controlValue);
    std::vector<float> imp(sr*4*2,0); imp[0]=imp[1]=1; yanki::Controls ec; ec.timeMs=100; ec.mode=1; ec.mix=1; process(imp,sr,ec); size_t first=0; for(size_t i=2;i<imp.size()/2;++i) if(std::abs(imp[i*2])>0.01){first=i;break;} line("echo_timing",first>=static_cast<size_t>(sr/10-1)&&first<=static_cast<size_t>(sr/10+1),"first_repeat="+std::to_string(first));
    bool heads=true; std::string headValue; for(int h=1;h<=3;++h){std::vector<float> hi(sr*4*2,0);hi[0]=hi[1]=1;yanki::Controls hc;hc.timeMs=100;hc.mode=h;hc.mix=1;hc.wear=0;process(hi,sr,hc);size_t at=0;for(size_t i=2;i<hi.size()/2;++i)if(std::abs(hi[i*2])>0.01){at=i;break;}const size_t want=static_cast<size_t>(sr/10)*static_cast<size_t>(h);heads &= at>=want-1&&at<=want+1;headValue += (h==1?"":" ")+std::to_string(at)+"/"+std::to_string(want);} line("three_head_timing",heads,"samples="+headValue+" wear=0");
    yanki::YankiEngine direct; direct.prepare(sr); std::vector<float> di(4096,0), dr(4096,0); di[0]=dr[0]=1; direct.process(di.data(),dr.data(),2048,ec); line("latency_zero",direct.latency()==0&&std::abs(di[0]-1.0f)<1e-6f,"reported=0 measured=0");
    std::vector<float> dark(sr*3*2,0); dark[0]=dark[1]=1; yanki::Controls dc=ec; dc.feedback=0.82; dc.toneHz=1800; process(dark,sr,dc); const size_t dly=sr/10; const double c1=centroid(dark,dly*2,sr), c2=centroid(dark,dly*4,sr); line("repeats_darken",c2<c1,"centroid1="+std::to_string(c1)+" centroid2="+std::to_string(c2));
    bool bypassControls=true; for(int k=0;k<4;++k){yanki::Controls q=dc;if(k==0)q.timeMs=600;if(k==1)q.mode=3;if(k==2)q.feedback=1.1;if(k==3)q.wear=1.0;q.mix=0;auto v=in;process(v,sr,q);bypassControls &= same(v,in);} line("bypass_controls_dry",bypassControls,"bit-exact");
    std::vector<float> run(sr*30*2,0); run[0]=run[1]=0.5f; yanki::Controls rc; rc.timeMs=80; rc.feedback=1.1; rc.mix=1; rc.mode=1; process(run,sr,rc); line("bounded_self_oscillation",finiteAndBound(run,1.42)&&rms(run)>1e-4,"peak="+std::to_string(peak(run)));
    yanki::YankiEngine e; e.prepare(sr); auto t0=std::chrono::steady_clock::now(); std::vector<float> sixty(static_cast<size_t>(sr*60)*2,0.01f); std::vector<float> ll(sr*60),rr(sr*60); for(size_t i=0;i<ll.size();++i)ll[i]=rr[i]=0.01f; e.process(ll.data(),rr.data(),sr*60,c); double sec=std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count(); line("cpu_60s",sec<3.0,"seconds="+std::to_string(sec));
    std::vector<float> cap(sr * 4 * 2, 0.0f); cap[0] = cap[1] = 1.0f; yanki::Controls capC = c; capC.drama = 1.0; capC.feedback = 1.1; capC.mix = 1.0; process(cap, sr, capC); const double capTail = windowRms(cap, sr * 3, sr / 2), capHead = windowRms(cap, sr / 4, sr * 3 / 4); const bool capOk = yanki::YankiEngine::kMaxEffectiveFeedback <= 0.92 && yanki::YankiEngine::kStageMaxFeedback <= 0.90 && capTail < capHead; line("drama_feedback_cap",capOk,"classic_max=0.920 stage_max=0.900 head_rms="+std::to_string(capHead)+" tail_rms="+std::to_string(capTail));
    std::vector<float> frozen = burstInput(sr * 20, sr); yanki::Controls fc; fc.mix = 1.0; fc.timeMs = 100; fc.mode = 1; yanki::YankiEngine freezeEngine; freezeEngine.prepare(sr); std::vector<float> fl(sr * 20), frch(sr * 20); for(size_t i=0;i<fl.size();++i){fl[i]=frozen[i*2];frch[i]=frozen[i*2+1];} fc.freeze=false; freezeEngine.process(fl.data(),frch.data(),sr*3/2,fc); std::fill(fl.begin()+sr*3/2,fl.end(),0.0f); std::fill(frch.begin()+sr*3/2,frch.end(),0.0f); fc.freeze=true; freezeEngine.process(fl.data()+sr*3/2,frch.data()+sr*3/2,sr*20-sr*3/2,fc); for(size_t i=0;i<fl.size();++i)frozen[i*2]=fl[i],frozen[i*2+1]=frch[i]; const double fr = windowRms(frozen, sr * 19, sr); const bool freezeOk = std::isfinite(fr) && fr > 1e-6 && peak(frozen) < 2.0; line("freeze_20s_bounded_non_silent",freezeOk,"rms="+std::to_string(fr));
    bool tails = true; std::string tailValues; for (const char* name : {"Yesilcam Sahne", "Kahraman Donusu", "Ters Yanki"}) { auto q = burstInput(sr * 5, sr); process(q, sr, preset(name)); const double br = windowRms(q, 0, static_cast<size_t>(1.5 * sr)); const double tr = windowRms(q, static_cast<size_t>(4.0 * sr), sr / 4); const double db = 20.0 * std::log10(std::max(tr, 1e-12) / std::max(br, 1e-12)); const bool one = db <= -25.0; tails &= one; tailValues += std::string(name) + "=" + std::to_string(db) + "dB "; line("decay_" + std::string(name), one, "tail_vs_burst_db=" + std::to_string(db)); }
    StageResult stage; for (const char* name : {"Yesilcam Sahne", "Kahraman Donusu", "Ters Yanki"}) { stageFilterInfo(name, sr); stageChecks(name, sr, stage); }
    // Reverse phrase lock: the same burst starting off the window grid (1.23 s of silence first).
    bool lockOk = true; { const yanki::Controls tc = preset("Ters Yanki"); const double D = tc.timeMs * 0.001, off = 1.23; const auto b = vocalBurst(sr, 5.5); std::vector<float> sh(static_cast<size_t>((5.5 + off) * sr) * 2, 0.0f); std::copy(b.begin(), b.end(), sh.begin() + static_cast<long>(static_cast<size_t>(off * sr) * 2)); yanki::Controls wc = tc; wc.mix = 1.0; auto wet = sh; process(wet, sr, wc); const double lv = dbOf(spanRms(wet, sr, off + D + 0.01, off + 2 * D - 0.01), spanRms(sh, sr, off + 0.01, off + D - 0.01)); lockOk = lv >= -10.0 && lv <= -5.0; line("stage_reverse_phrase_lock", lockOk, "first_repeat_db_offset_1.23s=" + fmt(lv) + " range=[-10,-5]"); }
    int swellChecks = 0; const bool swellOk0 = swelltest::run(swellChecks); const bool swellOk = swelltest::runClean(swellChecks) && swellOk0;
    // ---- ECHO (MOOD 0): the device's default state ----
    int echoChecks = 0; bool echoOk = true;
    { const auto chk = [&](const std::string& n, bool ok, const std::string& v) { line(n, ok, v); echoOk = echoOk && ok; ++echoChecks; };
      const yanki::Controls d = yanki::defaultControls(), m0 = yanki::moodControls(0);
      const bool same = d.timeMs == m0.timeMs && d.sync == m0.sync && d.bpm == m0.bpm && d.mode == m0.mode && d.feedback == m0.feedback && d.toneHz == m0.toneHz && d.drive == m0.drive
          && d.wear == m0.wear && d.mix == m0.mix && d.width == m0.width && d.drama == m0.drama && d.freeze == m0.freeze && d.spread == m0.spread && d.reverse == m0.reverse
          && d.rise == m0.rise && d.capOn == m0.capOn && d.capDb == m0.capDb && d.darken == m0.darken && d.endMode == m0.endMode && d.endTrigger == m0.endTrigger
          && d.lockBars == m0.lockBars && d.endBars == m0.endBars && d.cleanVoice == m0.cleanVoice && d.mood == m0.mood;
      auto ra = vocalBurst(sr, 5.5), rb = ra; process(ra, sr, d); process(rb, sr, m0);
      chk("echo_default_state_equals_preset", same && ra == rb, "defaultControls()==moodControls(0) all fields, renders bit-identical; sync=" + std::to_string(d.sync) + "(1/8) bpm=" + fmt(d.bpm, 0)
          + " time=" + fmt(yanki::YankiEngine::syncMs(d), 1) + "ms fb=" + fmt(d.feedback) + " tone=" + fmt(d.toneHz, 0) + " wear=" + fmt(d.wear) + " mix=" + fmt(d.mix) + " cap=" + (d.capOn ? "on" : "off") + " end=HOLD");
      { yanki::Controls c = d; c.wear = 0.0; c.mix = 1.0; std::vector<float> x(size_t(sr) * 2 * 2, 0.0f); x[0] = x[1] = 0.5f; process(x, sr, c);
        size_t at = 0; double pk = 0; for (size_t i = size_t(0.125 * sr); i < size_t(0.375 * sr); ++i) if (std::abs(x[2 * i]) > pk) { pk = std::abs(x[2 * i]); at = i; }
        const double ms = 1000.0 * double(at) / sr; chk("echo_first_repeat_timing", std::abs(ms - 250.0) <= 1.0, "120bpm 1/8 WEAR0 first_repeat_ms=" + fmt(ms, 3) + " want 250+-1"); }
      { const auto in = vocalBurst(sr, 5.5); const double br = spanRms(in, sr, 0.0, 1.5); auto o = in; process(o, sr, d); const double D = yanki::YankiEngine::syncMs(d) * 0.001;
        std::string steps; int cnt = 0; bool go = true; double first = 0;
        for (int k = 1; 1.5 + k * D <= 5.5 + 1e-9; ++k) { const double lv = dbOf(spanRms(o, sr, 1.5 + (k - 1) * D + 0.01, 1.5 + k * D - 0.01), br); if (k == 1) first = lv; if (k <= 8) steps += (k == 1 ? "" : ",") + fmt(lv, 1); if (go && lv > -40.0) ++cnt; else go = false; }
        const double tail = dbOf(spanRms(o, sr, 4.5, 5.0), br);
        chk("echo_repeats", cnt >= 3 && cnt <= 5, "vocal burst, repeats_above_-40dB=" + std::to_string(cnt) + " steps_db(after burst, 250ms each)=" + steps + " range=[3,5]");
        chk("echo_first_repeat_level", first >= -9.0 && first <= -6.0, "db_rel_burst=" + fmt(first) + " range=[-9,-6]");
        chk("echo_tail_3s", tail < -50.0, "db_rel_burst 3.0-3.5s_after_burst=" + fmt(tail) + " limit<-50"); } }
    const bool all = echoOk && swellOk && stage.ok && lockOk && same(a,b) && same(in,dry) && safe && controls && first>=static_cast<size_t>(sr/10-1)&&first<=static_cast<size_t>(sr/10+1) && heads && direct.latency()==0 && c2<c1 && bypassControls && finiteAndBound(run,1.42)&&rms(run)>1e-4 && sec<3.0 && capOk && freezeOk && tails;
    const int checks=17+stage.checks+swellChecks+echoChecks; /* 17 base lines are printed above (the old constant 19 counted 2 lines that are never printed) */ if(all) std::cout << "ALL PASS ("<<checks<<" checks)\n"; else std::cout << "SELFTEST FAILED\n"; return all ? 0 : 1;
}
static int parseEnd(const std::string& v) {
    if (v == "hold" || v == "HOLD" || v == "0") return yanki::kEndHold;
    if (v == "cut" || v == "CUT" || v == "1") return yanki::kEndCut;
    if (v == "dump" || v == "DUMP" || v == "2") return yanki::kEndDump;
    if (v == "freeze" || v == "FREEZE" || v == "3") return yanki::kEndFreeze;
    return -1;
}
static void printRiseMap(const yanki::Controls& c) {
    const double crit = yanki::YankiEngine::criticalMul(c);
    std::cout << "rise_map preset loop gain (FEEDBACK*0.45)=" << fmt(std::min(0.92, c.feedback) * 0.45, 4)
              << " critical multiplier=" << fmt(crit, 4) << " RISE off = x1.0000 = " << fmt(1.0 / crit, 2) << "x critical\n";
    for (int k = 0; k <= 10; ++k) {
        const double r = 0.1 * k, m = yanki::YankiEngine::riseToLoopMul(c, r);
        std::cout << "  RISE " << fmt(r, 1) << " -> " << fmt(yanki::YankiEngine::riseToOverGain(r), 3) << "x critical -> loop gain x" << fmt(m, 4)
                  << " (effective FEEDBACK*0.45 = " << fmt(std::min(0.92, c.feedback) * 0.45 * m, 4) << ") measured build "
                  << fmt(yanki::YankiEngine::riseRateMeasured(r)) << " dB/s\n";
    }
}
// TIME: 1/16, 1/8, 1/8d (dotted), 1/4, 1/2 at --bpm, or free milliseconds ("360" or "360ms").
static bool parseTime(const std::string& v, yanki::Controls& c) {
    if (v == "1/8") { c.sync = 1; return true; }
    if (v == "1/4") { c.sync = 2; return true; }
    if (v == "1/8d" || v == "1/8." || v == "dotted1/8") { c.sync = 3; return true; }
    if (v == "1/16") { c.sync = 4; return true; }
    if (v == "1/2") { c.sync = 5; return true; }
    try { c.timeMs = std::stod(v); c.sync = 0; return true; } catch (...) { return false; }
}
int main(int argc,char**argv) {
    if (argc > 1 && std::string(argv[1]) == "--selftest") return selftest();
    if (argc < 3) { std::cerr << "usage: render in.wav out.wav [--mood 0-6 | --preset P] [--time 1/16|1/8|1/8d|1/4|1/2|<ms>] [--bpm B] [--param k=v]\n"
                                 "       [--bars N] [--end hold|cut|dump|freeze] [--end-at S] [--print-rise-map]\n"
                                 "moods: 0 Echo (default), 1 Slapback, 2 Three Heads, 3 Dub Spiral, 4 Worn Loop, 5 Yesilcam, 6 Ters\n"; return 2; }
    WavData w; if (!readWav(argv[1], w) || w.channels < 1) { std::cerr << "cannot read wav\n"; return 2; }
    std::vector<float> x(w.samples.size()); for (size_t i = 0; i < w.samples.size(); ++i) x[i] = w.samples[i];
    if (w.channels == 1) { std::vector<float> s(x.size() * 2); for (size_t i = 0; i < x.size(); ++i) s[i*2] = s[i*2+1] = x[i]; x.swap(s); }
    else if (w.channels > 2) { std::vector<float> s((x.size() / w.channels) * 2); for (size_t i = 0; i < s.size() / 2; ++i) s[i*2] = x[i*w.channels], s[i*2+1] = x[i*w.channels+1]; x.swap(s); }
    yanki::Controls c = yanki::defaultControls(); double endAt = -1.0; bool riseMap = false;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--preset" && i + 1 < argc) c = preset(argv[++i]);
        else if (a == "--mood" && i + 1 < argc) { const int m = std::stoi(argv[++i]); if (m < 0 || m > 6) { std::cerr << "mood 0-6\n"; return 2; } c = yanki::moodControls(m); }
        else if (a == "--time" && i + 1 < argc) { if (!parseTime(argv[++i], c)) { std::cerr << "bad --time\n"; return 2; } }
        else if (a == "--bpm" && i + 1 < argc) c.bpm = std::stod(argv[++i]);
        else if (a == "--bars" && i + 1 < argc) c.lockBars = std::stod(argv[++i]);
        else if (a == "--end-at" && i + 1 < argc) endAt = std::stod(argv[++i]);
        else if (a == "--end" && i + 1 < argc) { const int m = parseEnd(argv[++i]); if (m < 0) { std::cerr << "bad --end\n"; return 2; } c.endMode = m; }
        else if (a == "--print-rise-map") riseMap = true;
        else if (a == "--param" && i + 1 < argc) {
            auto p = std::string(argv[++i]); auto eq = p.find('=');
            if (eq == std::string::npos) continue;
            auto k = p.substr(0, eq); auto sv = p.substr(eq + 1);
            if (k == "end") { const int m = parseEnd(sv); if (m < 0) { std::cerr << "bad end\n"; return 2; } c.endMode = m; continue; }
            if (k == "cap" && (sv == "off" || sv == "OFF")) { c.capOn = false; continue; }
            double v = std::stod(sv);
            if(k=="time")c.timeMs=v;else if(k=="feedback")c.feedback=v;else if(k=="tone")c.toneHz=v;else if(k=="drive")c.drive=v;else if(k=="wear")c.wear=v;else if(k=="mix")c.mix=v;else if(k=="width")c.width=v;else if(k=="mode")c.mode=static_cast<int>(v);else if(k=="bpm")c.bpm=v;else if(k=="sync")c.sync=static_cast<int>(v);else if(k=="drama")c.drama=v;else if(k=="spread")c.spread=v;else if(k=="reverse")c.reverse=v;else if(k=="freeze")c.freeze=v>0.5;
            else if (k == "rise") c.rise = v; else if (k == "cap") { c.capOn = true; c.capDb = v; } else if (k == "darken") c.darken = v;
            else if (k == "bars") c.lockBars = v; else if (k == "endbars") c.endBars = v;
            else if (k == "knob") yanki::YankiEngine::feedbackKnob(c, v);   // one FEEDBACK knob 0..1 (0.7 = hold), same helper as the plugin
            else { std::cerr << "unknown param " << k << "\n"; return 2; }
        }
    }
    if (riseMap) printRiseMap(c);
    std::cout << "feedback=" << fmt(c.feedback, 4) << " rise=" << (c.rise < 0 ? std::string("off") : fmt(c.rise, 3)) << " hold_feedback=" << fmt(yanki::YankiEngine::holdFeedback(c), 4)
              << " swelling=" << (yanki::YankiEngine::swellingNow(c) ? "yes" : "no") << "\n";
    std::cout << "mood=" << c.mood << " (" << yanki::kMoodNames[std::clamp(c.mood, 0, 6)] << ") time_ms=" << fmt(yanki::YankiEngine::syncMs(c), 3)
              << (c.sync ? " (synced at " + fmt(c.bpm, 1) + " bpm)" : std::string(" (free)")) << "\n";
    if (c.lockBars > 0.0) {
        const double T = yanki::YankiEngine::lockSeconds(c), ramp = yanki::YankiEngine::lockSlope(c);
        const double need = yanki::YankiEngine::rateToRise(ramp), used = yanki::YankiEngine::lockRise(c);
        std::cout << "tempo_lock bpm=" << fmt(c.bpm, 1) << " bars=" << fmt(c.lockBars, 2) << " cap_at_s=" << fmt(T, 3)
                  << " arrival_line: body " << fmt(ramp) << " dB/s, last " << fmt(yanki::YankiEngine::kLockFinalSec, 1) << " s " << fmt(yanki::YankiEngine::kLockFinalSlope, 0)
                  << " dB/s, starts at CAP" << fmt(yanki::YankiEngine::lockLineDb(ramp, T), 1) << " dB on the onset, lands on CAP" << fmt(yanki::YankiEngine::kLockAimDb, 2) << " dB on the downbeat"
                  << " required_RISE=" << fmt(need, 3) << " used_RISE(x" << fmt(yanki::YankiEngine::kLockMargin, 1) << " margin)=" << fmt(used, 3)
                  << " loop_gain_x=" << fmt(yanki::YankiEngine::riseToLoopMul(c, used), 4) << "\n";
    }
    if (endAt < 0.0) process(x, w.sampleRate, c);
    else {
        yanki::YankiEngine e; e.prepare(w.sampleRate);
        std::vector<float> l(x.size() / 2), r(l.size()); for (size_t i = 0; i < l.size(); ++i) { l[i] = x[i*2]; r[i] = x[i*2+1]; }
        const size_t at = std::min(l.size(), static_cast<size_t>(endAt * w.sampleRate));
        c.endTrigger = false; if (at > 0) e.process(l.data(), r.data(), static_cast<int>(at), c);
        c.endTrigger = true; if (at < l.size()) e.process(l.data() + at, r.data() + at, static_cast<int>(l.size() - at), c);
        for (size_t i = 0; i < l.size(); ++i) { x[i*2] = l[i]; x[i*2+1] = r[i]; }
        std::cout << "end_trigger_at_s=" << fmt(endAt, 3) << " mode=" << c.endMode << "\n";
    }
    if (!writeWav24(argv[2], w.sampleRate, x)) { std::cerr << "cannot write wav\n"; return 2; }
    return 0;
}
