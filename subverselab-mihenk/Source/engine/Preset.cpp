#include "Preset.h"
#include "Diagnosis.h"
#include "../dsp/Spectrum.h"

namespace mihenk
{
namespace
{
juce::var dynToVar (const DynBandSettings& d)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("enabled", d.enabled); o->setProperty ("shape", d.shape); o->setProperty ("freqHz", d.freqHz); o->setProperty ("q", d.q);
    o->setProperty ("amountDb", d.amountDb); o->setProperty ("rangeDb", d.rangeDb); o->setProperty ("ratio", d.ratio);
    o->setProperty ("attackMs", d.attackMs); o->setProperty ("releaseMs", d.releaseMs);
    return juce::var (o);
}

juce::var toVar (const MasterSettings& s)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("version", 2);
    o->setProperty ("dcFilter", s.dcFilter); o->setProperty ("balanceDb", s.balanceDb); o->setProperty ("autoTrim", s.autoTrim); o->setProperty ("bypass", s.bypass); o->setProperty ("trimDb", s.trimDb);
    o->setProperty ("targetPreset", (int) s.targetPreset); o->setProperty ("targetLufs", s.targetLufs);
    o->setProperty ("limitWarnDb", s.limitWarnDb); o->setProperty ("toneSmoothOct", s.toneSmoothOct); o->setProperty ("targetLock", s.targetLock);
    o->setProperty ("toneByHand", s.toneByHand);
    { auto* k = new juce::DynamicObject(); k->setProperty ("active", s.autoSkip.active); k->setProperty ("eq", s.autoSkip.eq); k->setProperty ("comp", s.autoSkip.comp); k->setProperty ("width", s.autoSkip.width); o->setProperty ("autoSkip", juce::var (k)); }
    auto* e = new juce::DynamicObject(); e->setProperty ("enabled", s.eq.enabled); e->setProperty ("linearPhase", s.eq.linearPhase);
    e->setProperty ("lowCutOn", s.eq.lowCutOn); e->setProperty ("lowCutHz", s.eq.lowCutHz); e->setProperty ("lowCutSlope", s.eq.lowCutSlope);
    e->setProperty ("lowShelfDb", s.eq.lowShelfDb); e->setProperty ("lowShelfHz", s.eq.lowShelfHz); e->setProperty ("tiltDbPerOct", s.eq.tiltDbPerOct);
    e->setProperty ("highShelfDb", s.eq.highShelfDb); e->setProperty ("highShelfHz", s.eq.highShelfHz);
    e->setProperty ("bell1Db", s.eq.bell1Db); e->setProperty ("bell1Hz", s.eq.bell1Hz); e->setProperty ("bell1Q", s.eq.bell1Q);
    e->setProperty ("bell2Db", s.eq.bell2Db); e->setProperty ("bell2Hz", s.eq.bell2Hz); e->setProperty ("bell2Q", s.eq.bell2Q);
    e->setProperty ("airDb", s.eq.airDb); e->setProperty ("airHz", s.eq.airHz); e->setProperty ("airDynamic", s.eq.airDynamic); e->setProperty ("airDeHarshDb", s.eq.airDeHarshDb);
    e->setProperty ("dynLowMid", dynToVar (s.eq.dynLowMid)); e->setProperty ("dynUpperMid", dynToVar (s.eq.dynUpperMid)); e->setProperty ("tightener", dynToVar (s.eq.tightener));
    e->setProperty ("matchAmount", s.eq.matchAmount); juce::Array<juce::var> curve; for (auto v : s.eq.matchCurveDb) curve.add (v); e->setProperty ("matchCurveDb", curve);
    o->setProperty ("eq", juce::var (e));
    auto* c = new juce::DynamicObject(); c->setProperty ("enabled", s.comp.enabled); c->setProperty ("autoThreshold", s.comp.autoThreshold); c->setProperty ("glueDb", s.comp.glueDb); c->setProperty ("thresholdDb", s.comp.thresholdDb); c->setProperty ("ratio", s.comp.ratio); c->setProperty ("attackMs", s.comp.attackMs); c->setProperty ("releaseMs", s.comp.releaseMs); c->setProperty ("autoRelease", s.comp.autoRelease); c->setProperty ("rmsBlend", s.comp.rmsBlend); c->setProperty ("scHpfHz", s.comp.scHpfHz); c->setProperty ("mix", s.comp.mix); c->setProperty ("kneeDb", s.comp.kneeDb);
    c->setProperty ("mode", s.comp.mode == CompMode::threeBand ? "3-band" : "wide"); c->setProperty ("xoverLowHz", s.comp.xoverLowHz); c->setProperty ("xoverHighHz", s.comp.xoverHighHz);
    juce::Array<juce::var> bands;
    for (const auto& b : s.comp.bands)
    {
        auto* x = new juce::DynamicObject();
        x->setProperty ("bypass", b.bypass); x->setProperty ("solo", b.solo); x->setProperty ("autoThreshold", b.autoThreshold); x->setProperty ("glueDb", b.glueDb);
        x->setProperty ("thresholdDb", b.thresholdDb); x->setProperty ("ratio", b.ratio); x->setProperty ("attackMs", b.attackMs); x->setProperty ("releaseMs", b.releaseMs);
        x->setProperty ("gainDb", b.gainDb);
        bands.add (juce::var (x));
    }
    c->setProperty ("bands", bands);
    o->setProperty ("comp", juce::var (c));
    auto* w = new juce::DynamicObject(); w->setProperty ("enabled", s.width.enabled); w->setProperty ("width", s.width.width); w->setProperty ("monoBelowHz", s.width.monoBelowHz);
    w->setProperty ("phaseSafeLowMono", s.width.phaseSafeLowMono); w->setProperty ("highWidth", s.width.highWidth); w->setProperty ("highWidthHz", s.width.highWidthHz); o->setProperty ("width", juce::var (w));
    auto* l = new juce::DynamicObject(); l->setProperty ("enabled", s.limiter.enabled); l->setProperty ("ceilingDbtp", s.limiter.ceilingDbtp); l->setProperty ("releaseMs", s.limiter.releaseMs); l->setProperty ("lookaheadMs", s.limiter.lookaheadMs); l->setProperty ("driveDb", s.limiter.driveDb); o->setProperty ("limiter", juce::var (l));
    return juce::var (o);
}

/** A finite value inside [lo, hi]; NaN / Inf take the fallback. */
double fin (double v, double lo, double hi, double fallback) { return std::isfinite (v) ? juce::jlimit (lo, hi, v) : fallback; }
void sanDyn (DynBandSettings& t)
{
    t.shape = t.shape == DynBandSettings::lowShelf ? DynBandSettings::lowShelf : DynBandSettings::bell;
    t.freqHz = fin (t.freqHz, 20.0, 20000.0, 300.0); t.q = fin (t.q, 0.1, 20.0, 1.0);
    t.amountDb = fin (t.amountDb, 0.0, 12.0, 1.5); t.rangeDb = fin (t.rangeDb, 0.0, 24.0, 6.0); t.ratio = fin (t.ratio, 1.0, 20.0, 3.0);
    t.attackMs = fin (t.attackMs, 0.1, 1000.0, 15.0); t.releaseMs = fin (t.releaseMs, 1.0, 5000.0, 150.0);
}
double d (const juce::var& v, const char* n, double x) { return v.getProperty (n, x); }
bool b (const juce::var& v, const char* n, bool x) { return v.getProperty (n, x); }
void readDyn (DynBandSettings& t, const juce::var& v)
{
    if (! v.isObject()) return;
    t.enabled = b (v, "enabled", t.enabled); t.shape = (int) d (v, "shape", t.shape); t.freqHz = d (v, "freqHz", t.freqHz); t.q = d (v, "q", t.q);
    t.amountDb = d (v, "amountDb", t.amountDb); t.rangeDb = d (v, "rangeDb", t.rangeDb); t.ratio = d (v, "ratio", t.ratio);
    t.attackMs = d (v, "attackMs", t.attackMs); t.releaseMs = d (v, "releaseMs", t.releaseMs);
}
void read (MasterSettings& s, const juce::var& v)
{
    s.dcFilter=b(v,"dcFilter",s.dcFilter); s.balanceDb=d(v,"balanceDb",s.balanceDb); s.autoTrim=b(v,"autoTrim",s.autoTrim);
    s.bypass=b(v,"bypass",s.bypass); s.trimDb=d(v,"trimDb",s.trimDb); s.targetPreset=(TargetPreset)juce::jlimit(0,4,(int)d(v,"targetPreset",(int)s.targetPreset)); s.targetLufs=d(v,"targetLufs",s.targetLufs); s.limitWarnDb=d(v,"limitWarnDb",s.limitWarnDb); s.toneSmoothOct=d(v,"toneSmoothOct",s.toneSmoothOct); s.targetLock=b(v,"targetLock",s.targetLock);
    s.toneByHand=b(v,"toneByHand",s.toneByHand);
    { auto k=v["autoSkip"]; if (k.isObject()) { s.autoSkip.active=b(k,"active",s.autoSkip.active); s.autoSkip.eq=b(k,"eq",s.autoSkip.eq); s.autoSkip.comp=b(k,"comp",s.autoSkip.comp); s.autoSkip.width=b(k,"width",s.autoSkip.width); } }
    auto e=v["eq"]; s.eq.enabled=b(e,"enabled",s.eq.enabled); s.eq.linearPhase=b(e,"linearPhase",s.eq.linearPhase); s.eq.lowCutOn=b(e,"lowCutOn",s.eq.lowCutOn); s.eq.lowCutHz=d(e,"lowCutHz",s.eq.lowCutHz); s.eq.lowCutSlope=(int)d(e,"lowCutSlope",s.eq.lowCutSlope); s.eq.lowShelfDb=d(e,"lowShelfDb",s.eq.lowShelfDb); s.eq.lowShelfHz=d(e,"lowShelfHz",s.eq.lowShelfHz); s.eq.tiltDbPerOct=d(e,"tiltDbPerOct",s.eq.tiltDbPerOct); s.eq.highShelfDb=d(e,"highShelfDb",s.eq.highShelfDb); s.eq.highShelfHz=d(e,"highShelfHz",s.eq.highShelfHz); s.eq.bell1Db=d(e,"bell1Db",s.eq.bell1Db); s.eq.bell1Hz=d(e,"bell1Hz",s.eq.bell1Hz); s.eq.bell1Q=d(e,"bell1Q",s.eq.bell1Q); s.eq.bell2Db=d(e,"bell2Db",s.eq.bell2Db); s.eq.bell2Hz=d(e,"bell2Hz",s.eq.bell2Hz); s.eq.bell2Q=d(e,"bell2Q",s.eq.bell2Q); s.eq.matchAmount=d(e,"matchAmount",s.eq.matchAmount);
    s.eq.airDb=d(e,"airDb",s.eq.airDb); s.eq.airHz=d(e,"airHz",s.eq.airHz); s.eq.airDynamic=b(e,"airDynamic",s.eq.airDynamic); s.eq.airDeHarshDb=d(e,"airDeHarshDb",s.eq.airDeHarshDb);
    readDyn (s.eq.dynLowMid, e["dynLowMid"]); readDyn (s.eq.dynUpperMid, e["dynUpperMid"]); readDyn (s.eq.tightener, e["tightener"]); s.eq.matchCurveDb.clear(); if (auto* arr = e["matchCurveDb"].getArray()) for (auto& x : *arr) s.eq.matchCurveDb.push_back ((float)(double)x);
    auto c=v["comp"]; s.comp.enabled=b(c,"enabled",s.comp.enabled); s.comp.autoThreshold=b(c,"autoThreshold",s.comp.autoThreshold); s.comp.glueDb=d(c,"glueDb",s.comp.glueDb); s.comp.thresholdDb=d(c,"thresholdDb",s.comp.thresholdDb); s.comp.ratio=d(c,"ratio",s.comp.ratio); s.comp.attackMs=d(c,"attackMs",s.comp.attackMs); s.comp.releaseMs=d(c,"releaseMs",s.comp.releaseMs); s.comp.autoRelease=b(c,"autoRelease",s.comp.autoRelease); s.comp.rmsBlend=d(c,"rmsBlend",s.comp.rmsBlend); s.comp.scHpfHz=d(c,"scHpfHz",s.comp.scHpfHz); s.comp.mix=d(c,"mix",s.comp.mix); s.comp.kneeDb=d(c,"kneeDb",s.comp.kneeDb);
    if (c.hasProperty ("mode")) s.comp.mode = c["mode"].toString() == "3-band" ? CompMode::threeBand : CompMode::wide;
    s.comp.xoverLowHz=d(c,"xoverLowHz",s.comp.xoverLowHz); s.comp.xoverHighHz=d(c,"xoverHighHz",s.comp.xoverHighHz);
    if (auto* arr = c["bands"].getArray())
        for (int i = 0; i < juce::jmin (3, arr->size()); ++i)
        {
            auto x = (*arr)[i]; auto& t = s.comp.bands[i];
            t.bypass=b(x,"bypass",t.bypass); t.solo=b(x,"solo",t.solo); t.autoThreshold=b(x,"autoThreshold",t.autoThreshold); t.glueDb=d(x,"glueDb",t.glueDb);
            t.thresholdDb=d(x,"thresholdDb",t.thresholdDb); t.ratio=d(x,"ratio",t.ratio); t.attackMs=d(x,"attackMs",t.attackMs); t.releaseMs=d(x,"releaseMs",t.releaseMs); t.gainDb=d(x,"gainDb",t.gainDb);
        }
    auto w=v["width"]; s.width.enabled=b(w,"enabled",s.width.enabled); s.width.width=d(w,"width",s.width.width); s.width.monoBelowHz=d(w,"monoBelowHz",s.width.monoBelowHz);
    s.width.phaseSafeLowMono=b(w,"phaseSafeLowMono",s.width.phaseSafeLowMono); s.width.highWidth=d(w,"highWidth",s.width.highWidth); s.width.highWidthHz=d(w,"highWidthHz",s.width.highWidthHz);
    auto l=v["limiter"]; s.limiter.enabled=b(l,"enabled",s.limiter.enabled); s.limiter.ceilingDbtp=d(l,"ceilingDbtp",s.limiter.ceilingDbtp); s.limiter.releaseMs=d(l,"releaseMs",s.limiter.releaseMs); s.limiter.lookaheadMs=d(l,"lookaheadMs",s.limiter.lookaheadMs); s.limiter.driveDb=d(l,"driveDb",s.limiter.driveDb);
}
}
bool savePreset (const juce::File& f, const MasterSettings& s, juce::String& error, const juce::String& category, int scenario)
{
    auto v = toVar (s);
    if (category.isNotEmpty()) v.getDynamicObject()->setProperty ("category", category);
    if (scenario > 0) v.getDynamicObject()->setProperty ("scenario", scenarioId (scenario));
    if (! f.replaceWithText (juce::JSON::toString (v, true))) { error = "Cannot write preset"; return false; }
    return true;
}

void readPresetCategory (const juce::File& f, juce::String& category, int& scenario)
{
    category.clear();
    scenario = 0;
    juce::var v;
    if (juce::JSON::parse (f.loadFileAsString(), v).failed() || ! v.isObject()) return;
    category = v.getProperty ("category", juce::String()).toString();
    scenario = scenarioFromId (v.getProperty ("scenario", juce::String()).toString());
}
void sanitiseSettings (MasterSettings& s)
{
    s.balanceDb = fin (s.balanceDb, -12.0, 12.0, 0.0); s.trimDb = fin (s.trimDb, -48.0, 48.0, 0.0);
    s.targetLufs = fin (s.targetLufs, -40.0, 0.0, -14.0); s.limitWarnDb = fin (s.limitWarnDb, 0.5, 24.0, 6.0); s.toneSmoothOct = fin (s.toneSmoothOct, 0.1, 6.0, 1.0);
    auto& e = s.eq;
    e.lowCutHz = fin (e.lowCutHz, 5.0, 400.0, 25.0); e.lowCutSlope = e.lowCutSlope >= 24 ? 24 : 12;
    e.lowShelfDb = fin (e.lowShelfDb, -12.0, 12.0, 0.0); e.lowShelfHz = fin (e.lowShelfHz, 20.0, 1000.0, 90.0);
    e.tiltDbPerOct = fin (e.tiltDbPerOct, -3.0, 3.0, 0.0);
    e.highShelfDb = fin (e.highShelfDb, -12.0, 12.0, 0.0); e.highShelfHz = fin (e.highShelfHz, 500.0, 20000.0, 10000.0);
    e.bell1Db = fin (e.bell1Db, -12.0, 12.0, 0.0); e.bell1Hz = fin (e.bell1Hz, 20.0, 20000.0, 250.0); e.bell1Q = fin (e.bell1Q, 0.1, 20.0, 1.0);
    e.bell2Db = fin (e.bell2Db, -12.0, 12.0, 0.0); e.bell2Hz = fin (e.bell2Hz, 20.0, 20000.0, 3000.0); e.bell2Q = fin (e.bell2Q, 0.1, 20.0, 1.0);
    e.matchAmount = fin (e.matchAmount, 0.0, 1.0, 1.0);
    e.airDb = fin (e.airDb, 0.0, 12.0, 0.0); e.airHz = fin (e.airHz, 5000.0, 40000.0, 20000.0); e.airDeHarshDb = fin (e.airDeHarshDb, 0.0, 12.0, 4.0);
    sanDyn (e.dynLowMid); sanDyn (e.dynUpperMid); sanDyn (e.tightener);
    bool curveOk = e.matchCurveDb.size() == spectrumGrid().size();
    for (auto& v : e.matchCurveDb) { if (! std::isfinite (v)) curveOk = false; else v = juce::jlimit (-24.0f, 24.0f, v); }
    if (! curveOk) e.matchCurveDb.clear();
    auto& c = s.comp;
    c.glueDb = fin (c.glueDb, 0.0, 12.0, 1.2); c.thresholdDb = fin (c.thresholdDb, -80.0, 0.0, -18.0); c.ratio = fin (c.ratio, 1.0, 20.0, 2.0);
    c.attackMs = fin (c.attackMs, 0.1, 500.0, 30.0); c.releaseMs = fin (c.releaseMs, 1.0, 5000.0, 250.0); c.rmsBlend = fin (c.rmsBlend, 0.0, 1.0, 0.7);
    c.scHpfHz = fin (c.scHpfHz, 0.0, 500.0, 90.0); c.mix = fin (c.mix, 0.0, 1.0, 1.0); c.kneeDb = fin (c.kneeDb, 0.0, 24.0, 6.0); c.maxGrDb = fin (c.maxGrDb, 0.0, 48.0, 0.0);
    c.xoverLowHz = fin (c.xoverLowHz, 20.0, 800.0, 120.0); c.xoverHighHz = fin (c.xoverHighHz, 1000.0, 16000.0, 4000.0);
    for (auto& b : c.bands)
    {
        b.glueDb = fin (b.glueDb, 0.0, 12.0, 1.0); b.thresholdDb = fin (b.thresholdDb, -80.0, 0.0, -24.0); b.ratio = fin (b.ratio, 1.0, 20.0, 2.0);
        b.attackMs = fin (b.attackMs, 0.1, 500.0, 20.0); b.releaseMs = fin (b.releaseMs, 1.0, 5000.0, 150.0); b.gainDb = fin (b.gainDb, -24.0, 24.0, 0.0);
    }
    auto& w = s.width;
    w.width = fin (w.width, 0.0, 4.0, 1.0); w.monoBelowHz = fin (w.monoBelowHz, 0.0, 600.0, 0.0);
    w.highWidth = fin (w.highWidth, 0.0, 4.0, 1.0); w.highWidthHz = fin (w.highWidthHz, 500.0, 20000.0, 4000.0);
    auto& l = s.limiter;
    l.ceilingDbtp = fin (l.ceilingDbtp, -12.0, 0.0, -1.0); l.releaseMs = fin (l.releaseMs, 1.0, 2000.0, 150.0);
    l.lookaheadMs = fin (l.lookaheadMs, 0.1, 10.0, 1.5); l.driveDb = fin (l.driveDb, -48.0, 48.0, 0.0);
}

juce::var settingsToVar (const MasterSettings& s) { return toVar (s); }
void settingsFromVar (const juce::var& v, MasterSettings& s) { if (v.isObject()) { read (s, v); sanitiseSettings (s); } }
bool loadPreset (const juce::File& f, MasterSettings& s, juce::String& error) { juce::var v; auto r=juce::JSON::parse(f.loadFileAsString(),v); if (r.failed() || !v.isObject()) { error="Invalid preset JSON"; return false; } read(s,v); sanitiseSettings (s); return true; }
}

namespace mihenk
{
juce::File defaultPresetsFolder()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("SubverseLab/MIHENK/Presets");
}

std::vector<PresetEntry> listPresets (const juce::File& folder)
{
    std::vector<PresetEntry> list;
    for (auto g : { Genre::rapTrap, Genre::arabeskPop, Genre::electronicLofi })
    {
        PresetEntry e;
        e.name = genreName (g);
        e.factory = true;
        e.genre = g;
        e.category = kCategoryStartingPoints;
        list.push_back (e);
    }
    for (int sc = 1; sc <= kNumScenarios; ++sc)
    {
        PresetEntry e;
        e.name = scenarioName (sc);
        e.factory = true;
        e.category = kCategoryScenarios;
        e.scenario = sc;
        list.push_back (e);
    }
    auto files = folder.findChildFiles (juce::File::findFiles, false, juce::String ("*") + kPresetExtension);
    std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b)
               { return a.getFileNameWithoutExtension().compareNatural (b.getFileNameWithoutExtension()) < 0; });
    for (const auto& f : files)
    {
        PresetEntry e;
        e.name = f.getFileNameWithoutExtension();
        e.file = f;
        juce::String cat;
        readPresetCategory (f, cat, e.scenario);
        e.category = cat == kCategoryScenarios ? kCategoryScenarios : kCategoryUser;
        list.push_back (e);
    }
    return list;
}

juce::String sanitisePresetName (const juce::String& name)
{
    juce::String out;
    for (auto ch : name)
        out += (ch < 32 || juce::String ("/\\:*?\"<>|").containsChar (ch)) ? juce::juce_wchar ('-') : ch;
    return out.trim().substring (0, 64).trim();
}

juce::String settingsFingerprint (const MasterSettings& s)
{
    auto t = s;
    if (t.targetLock) t.limiter.driveDb = 0.0;
    if (t.targetPreset == TargetPreset::keepInput) t.targetLufs = 0.0;     // solved from the source
    if (t.comp.autoThreshold) t.comp.thresholdDb = 0.0;
    else t.comp.glueDb = 0.0;
    for (auto& b : t.comp.bands)
    {
        if (b.autoThreshold) b.thresholdDb = 0.0;
        else b.glueDb = 0.0;
    }
    return juce::JSON::toString (toVar (t), true);
}
}
