#include "StateIO.h"

namespace mihenk::state
{
namespace
{
juce::var num (double v) { return std::isfinite (v) ? juce::var (v) : juce::var(); }
double num (const juce::var& o, const char* key, double fallback)
{
    const auto v = o.getProperty (key, juce::var());
    if (v.isVoid() || v.isUndefined()) return v.isVoid() && o.hasProperty (key) ? kNegInf : fallback;
    return (double) v;
}
} // namespace

juce::var floatsToVar (const std::vector<float>& v)
{
    juce::MemoryBlock mb (v.data(), v.size() * sizeof (float));
    return mb.toBase64Encoding();
}

std::vector<float> floatsFromVar (const juce::var& v)
{
    juce::MemoryBlock mb;
    if (! mb.fromBase64Encoding (v.toString())) return {};
    std::vector<float> out (mb.getSize() / sizeof (float));
    std::memcpy (out.data(), mb.getData(), out.size() * sizeof (float));
    return out;
}

juce::var toVar (const Measurement& m)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("valid", m.valid); o->setProperty ("sampleRate", m.sampleRate); o->setProperty ("channels", m.channels);
    o->setProperty ("seconds", m.seconds); o->setProperty ("integrated", num (m.integrated)); o->setProperty ("momentaryMax", num (m.momentaryMax));
    o->setProperty ("shortTermMax", num (m.shortTermMax)); o->setProperty ("lra", num (m.lra)); o->setProperty ("truePeak", num (m.truePeak));
    o->setProperty ("samplePeak", num (m.samplePeak)); o->setProperty ("rms", num (m.rms)); o->setProperty ("crest", num (m.crest));
    o->setProperty ("correlation", num (m.correlation)); o->setProperty ("truePeakAt", (juce::int64) m.truePeakAt);
    o->setProperty ("samplePeakAt", (juce::int64) m.samplePeakAt);
    o->setProperty ("shortTerm", floatsToVar (m.shortTermHistory)); o->setProperty ("spectrum", floatsToVar (m.spectrumDb));
    return juce::var (o);
}

void fromVar (const juce::var& o, Measurement& m)
{
    if (! o.isObject()) return;
    m.valid = o.getProperty ("valid", false); m.sampleRate = o.getProperty ("sampleRate", 0.0); m.channels = o.getProperty ("channels", 0);
    m.seconds = o.getProperty ("seconds", 0.0); m.integrated = num (o, "integrated", kNegInf); m.momentaryMax = num (o, "momentaryMax", kNegInf);
    m.shortTermMax = num (o, "shortTermMax", kNegInf); m.lra = num (o, "lra", 0.0); m.truePeak = num (o, "truePeak", kNegInf);
    m.samplePeak = num (o, "samplePeak", kNegInf); m.rms = num (o, "rms", kNegInf); m.crest = num (o, "crest", 0.0);
    m.correlation = num (o, "correlation", 1.0); m.truePeakAt = (juce::int64) o.getProperty ("truePeakAt", -1);
    m.samplePeakAt = (juce::int64) o.getProperty ("samplePeakAt", -1);
    m.shortTermHistory = floatsFromVar (o["shortTerm"]); m.spectrumDb = floatsFromVar (o["spectrum"]);
}

juce::var toVar (const RtPlan& p)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("valid", p.valid); o->setProperty ("sampleRate", p.sampleRate); o->setProperty ("autoTrimDb", p.autoTrimDb);
    o->setProperty ("sourceValid", p.sourceValid); o->setProperty ("denseWide", p.denseWide); o->setProperty ("densePeak", p.densePeak);
    o->setProperty ("wideMakeupDb", p.wideMakeupDb); o->setProperty ("bandFinalMakeupDb", p.bandFinalMakeupDb);
    juce::Array<juce::var> ba, bm, da, dt;
    for (int i = 0; i < 3; ++i) { ba.add (p.bandActive[i]); bm.add (p.bandMakeupDb[i]); da.add (p.dynActive[i]); dt.add (p.dynThresholdDb[i]); }
    o->setProperty ("bandActive", ba); o->setProperty ("bandMakeupDb", bm); o->setProperty ("dynActive", da); o->setProperty ("dynThresholdDb", dt);
    o->setProperty ("airActive", p.airActive); o->setProperty ("airTypicalRelDb", p.airTypicalRelDb);
    o->setProperty ("safeSign", p.safeSign); o->setProperty ("safeGain", p.safeGain); o->setProperty ("learnedSamples", p.learnedSamples);
    return juce::var (o);
}

void fromVar (const juce::var& o, RtPlan& p)
{
    if (! o.isObject()) return;
    p.valid = o.getProperty ("valid", false); p.sampleRate = o.getProperty ("sampleRate", 0.0); p.autoTrimDb = o.getProperty ("autoTrimDb", 0.0);
    p.sourceValid = o.getProperty ("sourceValid", false); p.denseWide = o.getProperty ("denseWide", false); p.densePeak = o.getProperty ("densePeak", false);
    p.wideMakeupDb = o.getProperty ("wideMakeupDb", 0.0); p.bandFinalMakeupDb = o.getProperty ("bandFinalMakeupDb", 0.0);
    for (int i = 0; i < 3; ++i)
    {
        p.bandActive[i] = o["bandActive"][i]; p.bandMakeupDb[i] = o["bandMakeupDb"][i];
        p.dynActive[i] = o["dynActive"][i]; p.dynThresholdDb[i] = o["dynThresholdDb"][i];
    }
    p.airActive = o.getProperty ("airActive", false); p.airTypicalRelDb = o.getProperty ("airTypicalRelDb", 0.0);
    p.safeSign = o.getProperty ("safeSign", 1.0); p.safeGain = o.getProperty ("safeGain", 1.0);
    p.learnedSamples = (juce::int64) o.getProperty ("learnedSamples", 0);
    sanitisePlan (p);
}

namespace
{
juce::var quantisedToVar (const std::vector<float>& v)
{
    std::vector<int16_t> q (v.size());
    for (size_t i = 0; i < v.size(); ++i) q[i] = (int16_t) juce::jlimit (-20000, 20000, juce::roundToInt (v[i] * 100.0f));
    juce::MemoryBlock mb (q.data(), q.size() * sizeof (int16_t));
    return mb.toBase64Encoding();
}

std::vector<float> quantisedFromVar (const juce::var& v)
{
    juce::MemoryBlock mb;
    if (! mb.fromBase64Encoding (v.toString())) return {};
    std::vector<int16_t> q (mb.getSize() / sizeof (int16_t));
    std::memcpy (q.data(), mb.getData(), q.size() * sizeof (int16_t));
    std::vector<float> out (q.size());
    for (size_t i = 0; i < q.size(); ++i) out[i] = (float) q[i] * 0.01f;
    return out;
}
} // namespace

juce::var toVar (const LoudnessProxy& p)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("sampleRate", p.sampleRate); o->setProperty ("hop", p.hop);
    o->setProperty ("energy", quantisedToVar (p.energyDb)); o->setProperty ("peak", quantisedToVar (p.peakDb)); o->setProperty ("shares", quantisedToVar (p.shareDb));
    o->setProperty ("baselineDriveDb", p.baselineDriveDb); o->setProperty ("baselineLufs", num (p.baselineLufs));
    o->setProperty ("ceilingDb", p.ceilingDb); o->setProperty ("releaseMs", p.releaseMs); o->setProperty ("limiterOn", p.limiterOn); o->setProperty ("recovery", p.recovery);
    return juce::var (o);
}

void fromVar (const juce::var& o, LoudnessProxy& p)
{
    p = {};
    if (! o.isObject()) return;
    p.sampleRate = o.getProperty ("sampleRate", 0.0); p.hop = o.getProperty ("hop", 0);
    p.energyDb = quantisedFromVar (o["energy"]); p.peakDb = quantisedFromVar (o["peak"]); p.shareDb = quantisedFromVar (o["shares"]);
    p.baselineDriveDb = o.getProperty ("baselineDriveDb", 0.0); p.baselineLufs = num (o, "baselineLufs", kNegInf);
    p.ceilingDb = o.getProperty ("ceilingDb", -1.0); p.releaseMs = o.getProperty ("releaseMs", 150.0); p.limiterOn = o.getProperty ("limiterOn", true); p.recovery = o.getProperty ("recovery", 0.0);
    // A saved project is not trusted: anything out of shape leaves the proxy invalid (the drive is then only re-solved by LEARN).
    const bool ok = std::isfinite (p.sampleRate) && p.sampleRate >= 8000.0 && p.sampleRate <= 768000.0 && p.hop >= 1 && p.hop <= 80000
                    && p.energyDb.size() == p.peakDb.size() && p.energyDb.size() >= 40 && p.energyDb.size() <= 20u * 60u * 100u + 100u && p.energyDb.size() % 10 == 0 && p.shareDb.size() == p.energyDb.size() / 10 * (size_t) LoudnessProxy::numBands
                    && std::isfinite (p.baselineDriveDb) && std::isfinite (p.baselineLufs) && std::isfinite (p.ceilingDb) && std::isfinite (p.releaseMs);
    if (! ok) { p = {}; return; }
    p.baselineDriveDb = juce::jlimit (-48.0, 48.0, p.baselineDriveDb);
    p.ceilingDb = juce::jlimit (-12.0, 0.0, p.ceilingDb);
    p.releaseMs = juce::jlimit (1.0, 2000.0, p.releaseMs);
    p.recovery = std::isfinite (p.recovery) ? juce::jlimit (0.0, 40.0, p.recovery) : 0.0;
    p.valid = true;
    p.prepare();
}

juce::var toVar (const Diagnosis& d)
{
    auto* o = new juce::DynamicObject();
    const auto& m = d.m;
    auto* mm = new juce::DynamicObject();
    mm->setProperty ("valid", m.valid); mm->setProperty ("stereo", m.stereo); mm->setProperty ("seconds", m.seconds);
    const std::pair<const char*, double> fields[] {
        { "integrated", m.integrated }, { "shortTermMax", m.shortTermMax }, { "lra", m.lra }, { "truePeak", m.truePeak }, { "samplePeak", m.samplePeak },
        { "rms", m.rms }, { "crest", m.crest }, { "plr", m.plr }, { "correlation", m.correlation }, { "clipRunsPerMin", m.clipRunsPerMin },
        { "dcDbfs", m.dcDbfs }, { "balanceDb", m.balanceDb }, { "lowBalanceDb", m.lowBalanceDb }, { "lowCorrelation", m.lowCorrelation },
        { "monoLowLossDb", m.monoLowLossDb }, { "sideMidDb", m.sideMidDb }, { "sideMidHighDb", m.sideMidHighDb }, { "lineSlopeDbPerOct", m.lineSlopeDbPerOct },
        { "mudDb", m.mudDb }, { "mudPeakHz", m.mudPeakHz }, { "harshDb", m.harshDb }, { "harshPeakHz", m.harshPeakHz }, { "presenceDb", m.presenceDb },
        { "subDb", m.subDb }, { "lowCrestDb", m.lowCrestDb }, { "lowDb", m.lowDb }, { "airRelDb", m.airRelDb }, { "sibPeakDb", m.sibPeakDb },
        { "sibAbsDb", m.sibAbsDb }, { "unevenLowDb", m.unevenLowDb }, { "resonanceDb", m.resonanceDb }, { "resonanceHz", m.resonanceHz }, { "ispDb", m.ispDb } };
    for (auto& f : fields) mm->setProperty (f.first, num (f.second));
    mm->setProperty ("dense", m.dense); mm->setProperty ("overCompressed", m.overCompressed); mm->setProperty ("clipRuns", m.clipRuns);
    mm->setProperty ("spectrum", floatsToVar (m.spectrumDb));
    o->setProperty ("measures", juce::var (mm));
    juce::Array<juce::var> list;
    for (const auto& f : d.findings)
    {
        auto* x = new juce::DynamicObject();
        x->setProperty ("scenario", f.scenario); x->setProperty ("id", f.id); x->setProperty ("severity", (int) f.severity);
        x->setProperty ("title", f.title); x->setProperty ("reason", f.reason); x->setProperty ("delta", f.delta); x->setProperty ("value", num (f.value));
        list.add (juce::var (x));
    }
    o->setProperty ("findings", list);
    return juce::var (o);
}

void fromVar (const juce::var& o, Diagnosis& d)
{
    if (! o.isObject()) return;
    auto mm = o["measures"];
    auto& m = d.m;
    m.valid = mm.getProperty ("valid", false); m.stereo = mm.getProperty ("stereo", false); m.seconds = mm.getProperty ("seconds", 0.0);
    std::pair<const char*, double*> fields[] {
        { "integrated", &m.integrated }, { "shortTermMax", &m.shortTermMax }, { "lra", &m.lra }, { "truePeak", &m.truePeak }, { "samplePeak", &m.samplePeak },
        { "rms", &m.rms }, { "crest", &m.crest }, { "plr", &m.plr }, { "correlation", &m.correlation }, { "clipRunsPerMin", &m.clipRunsPerMin },
        { "dcDbfs", &m.dcDbfs }, { "balanceDb", &m.balanceDb }, { "lowBalanceDb", &m.lowBalanceDb }, { "lowCorrelation", &m.lowCorrelation },
        { "monoLowLossDb", &m.monoLowLossDb }, { "sideMidDb", &m.sideMidDb }, { "sideMidHighDb", &m.sideMidHighDb }, { "lineSlopeDbPerOct", &m.lineSlopeDbPerOct },
        { "mudDb", &m.mudDb }, { "mudPeakHz", &m.mudPeakHz }, { "harshDb", &m.harshDb }, { "harshPeakHz", &m.harshPeakHz }, { "presenceDb", &m.presenceDb },
        { "subDb", &m.subDb }, { "lowCrestDb", &m.lowCrestDb }, { "lowDb", &m.lowDb }, { "airRelDb", &m.airRelDb }, { "sibPeakDb", &m.sibPeakDb },
        { "sibAbsDb", &m.sibAbsDb }, { "unevenLowDb", &m.unevenLowDb }, { "resonanceDb", &m.resonanceDb }, { "resonanceHz", &m.resonanceHz }, { "ispDb", &m.ispDb } };
    for (auto& f : fields) *f.second = num (mm, f.first, *f.second);
    m.dense = mm.getProperty ("dense", false); m.overCompressed = mm.getProperty ("overCompressed", false); m.clipRuns = mm.getProperty ("clipRuns", 0);
    m.spectrumDb = floatsFromVar (mm["spectrum"]);
    d.findings.clear();
    if (auto* arr = o["findings"].getArray())
        for (auto& x : *arr)
        {
            Finding f;
            f.scenario = x.getProperty ("scenario", 0); f.id = x.getProperty ("id", "").toString(); f.severity = (Severity) (int) x.getProperty ("severity", 0);
            f.title = x.getProperty ("title", "").toString(); f.reason = x.getProperty ("reason", "").toString(); f.delta = x.getProperty ("delta", "").toString();
            f.value = num (x, "value", 0.0);
            d.findings.push_back (f);
        }
}
} // namespace mihenk::state
