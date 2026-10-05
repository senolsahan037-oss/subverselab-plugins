#include "Params.h"
#include "../engine/Plan.h"

namespace mihenk::params
{
namespace
{
template <typename Ref>
Def real (const char* id, const char* name, const char* unit, float lo, float hi, float mid, Ref ref)
{
    Def d;
    d.id = id; d.name = name; d.unit = unit; d.kind = Def::real;
    d.minimum = lo; d.maximum = hi; d.midpoint = mid;
    d.get = [ref] (const MasterSettings& s) { return (double) ref (const_cast<MasterSettings&> (s)); };
    d.set = [ref] (MasterSettings& s, double v) { ref (s) = v; };
    return d;
}

template <typename Ref>
Def flag (const char* id, const char* name, Ref ref)
{
    Def d;
    d.id = id; d.name = name; d.kind = Def::boolean;
    d.get = [ref] (const MasterSettings& s) { return ref (const_cast<MasterSettings&> (s)) ? 1.0 : 0.0; };
    d.set = [ref] (MasterSettings& s, double v) { ref (s) = v > 0.5; };
    return d;
}

Def choice (const char* id, const char* name, juce::StringArray items, std::function<double (const MasterSettings&)> get,
            std::function<void (MasterSettings&, double)> set)
{
    Def d;
    d.id = id; d.name = name; d.kind = Def::choice; d.choices = std::move (items);
    d.minimum = 0.0f; d.maximum = (float) (d.choices.size() - 1);
    d.get = std::move (get);
    d.set = std::move (set);
    return d;
}

constexpr double kAirSteps[] { 10000.0, 15000.0, 20000.0, 40000.0 };

std::vector<Def> build()
{
    using S = MasterSettings;
    std::vector<Def> v;
    v.push_back (flag ("bypass", "Bypass", [] (S& s) -> bool& { return s.bypass; }));
    v.push_back (real ("trim", "Trim", "dB", -24.0f, 24.0f, 0.0f, [] (S& s) -> double& { return s.trimDb; }));
    v.push_back (flag ("autoTrim", "Auto trim", [] (S& s) -> bool& { return s.autoTrim; }));
    v.push_back (real ("balance", "Balance", "dB", -3.0f, 3.0f, 0.0f, [] (S& s) -> double& { return s.balanceDb; }));
    v.push_back (flag ("dc", "DC filter", [] (S& s) -> bool& { return s.dcFilter; }));

    // Equaliser
    v.push_back (flag ("eqOn", "EQ in", [] (S& s) -> bool& { return s.eq.enabled; }));
    v.push_back (flag ("eqLinear", "EQ linear phase", [] (S& s) -> bool& { return s.eq.linearPhase; }));
    v.push_back (flag ("lowCutOn", "Low cut in", [] (S& s) -> bool& { return s.eq.lowCutOn; }));
    v.push_back (real ("lowCutHz", "Low cut", "Hz", 10.0f, 200.0f, 40.0f, [] (S& s) -> double& { return s.eq.lowCutHz; }));
    {
        Def d;
        d.id = "lowCutSteep"; d.name = "Low cut 24 dB"; d.kind = Def::boolean;
        d.get = [] (const S& s) { return s.eq.lowCutSlope >= 24 ? 1.0 : 0.0; };
        d.set = [] (S& s, double x) { s.eq.lowCutSlope = x > 0.5 ? 24 : 12; };
        v.push_back (d);
    }
    v.push_back (real ("lowShelfDb", "Low shelf", "dB", -6.0f, 6.0f, 0.0f, [] (S& s) -> double& { return s.eq.lowShelfDb; }));
    v.push_back (real ("lowShelfHz", "Low shelf freq", "Hz", 30.0f, 400.0f, 100.0f, [] (S& s) -> double& { return s.eq.lowShelfHz; }));
    v.push_back (real ("tilt", "Tilt", "dB/oct", -1.5f, 1.5f, 0.0f, [] (S& s) -> double& { return s.eq.tiltDbPerOct; }));
    v.push_back (real ("highShelfDb", "High shelf", "dB", -6.0f, 6.0f, 0.0f, [] (S& s) -> double& { return s.eq.highShelfDb; }));
    v.push_back (real ("highShelfHz", "High shelf freq", "Hz", 2000.0f, 16000.0f, 7000.0f, [] (S& s) -> double& { return s.eq.highShelfHz; }));
    v.push_back (real ("bell1Db", "Bell 1", "dB", -8.0f, 8.0f, 0.0f, [] (S& s) -> double& { return s.eq.bell1Db; }));
    v.push_back (real ("bell1Hz", "Bell 1 freq", "Hz", 40.0f, 16000.0f, 700.0f, [] (S& s) -> double& { return s.eq.bell1Hz; }));
    v.push_back (real ("bell1Q", "Bell 1 Q", "", 0.3f, 6.0f, 1.2f, [] (S& s) -> double& { return s.eq.bell1Q; }));
    v.push_back (real ("bell2Db", "Bell 2", "dB", -8.0f, 8.0f, 0.0f, [] (S& s) -> double& { return s.eq.bell2Db; }));
    v.push_back (real ("bell2Hz", "Bell 2 freq", "Hz", 40.0f, 16000.0f, 2500.0f, [] (S& s) -> double& { return s.eq.bell2Hz; }));
    v.push_back (real ("bell2Q", "Bell 2 Q", "", 0.3f, 6.0f, 1.2f, [] (S& s) -> double& { return s.eq.bell2Q; }));
    v.push_back (real ("airDb", "Air", "dB", 0.0f, 6.0f, 0.0f, [] (S& s) -> double& { return s.eq.airDb; }));
    v.push_back (choice ("airHz", "Air freq", { "10 kHz", "15 kHz", "20 kHz", "40 kHz" },
                         [] (const S& s) { int best = 0; for (int i = 1; i < 4; ++i) if (std::abs (kAirSteps[i] - s.eq.airHz) < std::abs (kAirSteps[best] - s.eq.airHz)) best = i; return (double) best; },
                         [] (S& s, double x) { s.eq.airHz = kAirSteps[juce::jlimit (0, 3, juce::roundToInt (x))]; }));
    v.push_back (flag ("airDyn", "Air dynamic", [] (S& s) -> bool& { return s.eq.airDynamic; }));
    v.push_back (real ("airDeHarsh", "Air de-harsh", "dB", 0.0f, 8.0f, 0.0f, [] (S& s) -> double& { return s.eq.airDeHarshDb; }));

    // Dynamic EQ (frequency, amount, on; the rest is set by the scenarios)
    v.push_back (flag ("lowMidOn", "Low-mid dyn in", [] (S& s) -> bool& { return s.eq.dynLowMid.enabled; }));
    v.push_back (real ("lowMidHz", "Low-mid dyn freq", "Hz", 150.0f, 800.0f, 320.0f, [] (S& s) -> double& { return s.eq.dynLowMid.freqHz; }));
    v.push_back (real ("lowMidAmount", "Low-mid dyn amount", "dB", 0.0f, 4.0f, 0.0f, [] (S& s) -> double& { return s.eq.dynLowMid.amountDb; }));
    v.push_back (flag ("upperMidOn", "Upper-mid dyn in", [] (S& s) -> bool& { return s.eq.dynUpperMid.enabled; }));
    v.push_back (real ("upperMidHz", "Upper-mid dyn freq", "Hz", 1500.0f, 8000.0f, 3500.0f, [] (S& s) -> double& { return s.eq.dynUpperMid.freqHz; }));
    v.push_back (real ("upperMidAmount", "Upper-mid dyn amount", "dB", 0.0f, 4.0f, 0.0f, [] (S& s) -> double& { return s.eq.dynUpperMid.amountDb; }));
    v.push_back (flag ("tightOn", "Tightener in", [] (S& s) -> bool& { return s.eq.tightener.enabled; }));
    v.push_back (real ("tightHz", "Tightener freq", "Hz", 40.0f, 200.0f, 90.0f, [] (S& s) -> double& { return s.eq.tightener.freqHz; }));
    v.push_back (real ("tightAmount", "Tightener amount", "dB", 0.0f, 4.0f, 0.0f, [] (S& s) -> double& { return s.eq.tightener.amountDb; }));

    // Compressor
    v.push_back (flag ("compOn", "Comp in", [] (S& s) -> bool& { return s.comp.enabled; }));
    v.push_back (choice ("compMode", "Comp mode", { "WIDE", "3-BAND" },
                         [] (const S& s) { return s.comp.mode == CompMode::threeBand ? 1.0 : 0.0; },
                         [] (S& s, double x) { s.comp.mode = x > 0.5 ? CompMode::threeBand : CompMode::wide; }));
    v.push_back (flag ("compAuto", "Comp AUTO", [] (S& s) -> bool& { return s.comp.autoThreshold; }));
    v.push_back (real ("glue", "Glue", "dB", 0.0f, 4.0f, 0.0f, [] (S& s) -> double& { return s.comp.glueDb; }));
    v.push_back (real ("threshold", "Threshold", "dBFS", -60.0f, 0.0f, 0.0f, [] (S& s) -> double& { return s.comp.thresholdDb; }));
    v.push_back (real ("ratio", "Ratio", ":1", 1.0f, 8.0f, 2.5f, [] (S& s) -> double& { return s.comp.ratio; }));
    v.push_back (real ("attack", "Attack", "ms", 1.0f, 200.0f, 25.0f, [] (S& s) -> double& { return s.comp.attackMs; }));
    v.push_back (real ("release", "Release", "ms", 20.0f, 1500.0f, 250.0f, [] (S& s) -> double& { return s.comp.releaseMs; }));
    v.push_back (flag ("autoRel", "Auto release", [] (S& s) -> bool& { return s.comp.autoRelease; }));
    v.push_back (real ("rms", "RMS / peak", "", 0.0f, 1.0f, 0.0f, [] (S& s) -> double& { return s.comp.rmsBlend; }));
    v.push_back (real ("scHpf", "SC HPF", "Hz", 20.0f, 300.0f, 90.0f, [] (S& s) -> double& { return s.comp.scHpfHz; }));
    v.push_back (real ("mix", "Comp mix", "", 0.0f, 1.0f, 0.0f, [] (S& s) -> double& { return s.comp.mix; }));
    v.push_back (real ("knee", "Knee", "dB", 0.0f, 12.0f, 0.0f, [] (S& s) -> double& { return s.comp.kneeDb; }));
    v.push_back (real ("xoverLow", "Crossover low", "Hz", 40.0f, 400.0f, 120.0f, [] (S& s) -> double& { return s.comp.xoverLowHz; }));
    v.push_back (real ("xoverHigh", "Crossover high", "Hz", 2000.0f, 10000.0f, 4000.0f, [] (S& s) -> double& { return s.comp.xoverHighHz; }));
    const char* bandNames[] { "Low", "Mid", "High" };
    for (int b = 0; b < 3; ++b)
    {
        const juce::String p = "b" + juce::String (b + 1), n = bandNames[b];
        auto band = [b] (S& s) -> BandSettings& { return s.comp.bands[b]; };
        auto mk = [&] (Def d, const juce::String& id, const juce::String& name) { d.id = p + id; d.name = n + " " + name; return d; };
        v.push_back (mk (flag ("", "", [band] (S& s) -> bool& { return band (s).bypass; }), "Bypass", "bypass"));
        v.push_back (mk (flag ("", "", [band] (S& s) -> bool& { return band (s).solo; }), "Solo", "solo"));
        v.push_back (mk (flag ("", "", [band] (S& s) -> bool& { return band (s).autoThreshold; }), "Auto", "AUTO"));
        v.push_back (mk (real ("", "", "dB", 0.0f, 4.0f, 0.0f, [band] (S& s) -> double& { return band (s).glueDb; }), "Glue", "glue"));
        v.push_back (mk (real ("", "", "dBFS", -60.0f, 0.0f, 0.0f, [band] (S& s) -> double& { return band (s).thresholdDb; }), "Threshold", "threshold"));
        v.push_back (mk (real ("", "", ":1", 1.0f, 8.0f, 2.5f, [band] (S& s) -> double& { return band (s).ratio; }), "Ratio", "ratio"));
        v.push_back (mk (real ("", "", "ms", 1.0f, 200.0f, 25.0f, [band] (S& s) -> double& { return band (s).attackMs; }), "Attack", "attack"));
        v.push_back (mk (real ("", "", "ms", 20.0f, 1500.0f, 200.0f, [band] (S& s) -> double& { return band (s).releaseMs; }), "Release", "release"));
        v.push_back (mk (real ("", "", "dB", -6.0f, 6.0f, 0.0f, [band] (S& s) -> double& { return band (s).gainDb; }), "Gain", "gain"));
    }

    // Stereo and low end
    v.push_back (flag ("widthOn", "Stereo in", [] (S& s) -> bool& { return s.width.enabled; }));
    v.push_back (real ("width", "Width", "", 0.0f, 2.0f, 0.0f, [] (S& s) -> double& { return s.width.width; }));
    {
        auto d = real ("monoBelow", "Mono below", "Hz", 0.0f, 300.0f, 0.0f, [] (S& s) -> double& { return s.width.monoBelowHz; });
        d.set = [] (S& s, double x) { s.width.monoBelowHz = x <= 0.0 ? 0.0 : juce::jmax (kMonoBelowMinHz, x); };
        v.push_back (d);
    }
    v.push_back (flag ("phaseSafe", "Phase-safe low mono", [] (S& s) -> bool& { return s.width.phaseSafeLowMono; }));
    v.push_back (real ("highWidth", "High width", "", 0.0f, 2.0f, 0.0f, [] (S& s) -> double& { return s.width.highWidth; }));
    v.push_back (real ("highWidthHz", "High width freq", "Hz", 1000.0f, 12000.0f, 4000.0f, [] (S& s) -> double& { return s.width.highWidthHz; }));

    // Limiter
    v.push_back (flag ("limOn", "Limiter in", [] (S& s) -> bool& { return s.limiter.enabled; }));
    v.push_back (real ("ceiling", "Ceiling", "dBTP", -3.0f, -0.1f, 0.0f, [] (S& s) -> double& { return s.limiter.ceilingDbtp; }));
    v.push_back (real ("limRelease", "Limiter release", "ms", 10.0f, 1000.0f, 150.0f, [] (S& s) -> double& { return s.limiter.releaseMs; }));
    v.push_back (real ("lookahead", "Look-ahead", "ms", 0.5f, 5.0f, 0.0f, [] (S& s) -> double& { return s.limiter.lookaheadMs; }));
    v.push_back (real ("drive", "Drive", "dB", -24.0f, 36.0f, 0.0f, [] (S& s) -> double& { return s.limiter.driveDb; }));

    // Target and reference
    v.push_back (choice ("targetPreset", "Target", { "STREAM -14", "APPLE -16", "CLUB -8", "CUSTOM", "KEEP INPUT" },
                         [] (const S& s) { return (double) (int) s.targetPreset; },
                         [] (S& s, double x) { s.targetPreset = (TargetPreset) juce::jlimit (0, 4, juce::roundToInt (x)); }));
    v.push_back (real ("targetLufs", "Target LUFS", "LUFS", -30.0f, -4.0f, 0.0f, [] (S& s) -> double& { return s.targetLufs; }));
    v.push_back (flag ("targetLock", "Target lock", [] (S& s) -> bool& { return s.targetLock; }));
    v.push_back (real ("warnAt", "Warn at", "dB", 1.0f, 12.0f, 0.0f, [] (S& s) -> double& { return s.limitWarnDb; }));
    v.push_back (real ("toneAmount", "Tone amount", "", 0.0f, 1.0f, 0.0f, [] (S& s) -> double& { return s.eq.matchAmount; }));
    v.push_back (real ("toneSmooth", "Tone smooth", "oct", 0.33f, 3.0f, 1.0f, [] (S& s) -> double& { return s.toneSmoothOct; }));
    return v;
}
} // namespace

double Def::defaultValue() const
{
    // v3: a new instance shows the plan of nothing measured (everything neutral, the level untouched), not the v2 factory chain.
    static const MasterSettings fresh = []
    {
        Measurements m; m.present = false;
        auto f = solvePlan (m, Intent()).settings;
        f.targetPreset = TargetPreset::streaming;      // the plan carries its target as CUSTOM + a number; the label of the default intent (STREAM -14) is what the Session shows
        return f;
    }();
    return get (fresh);
}

MasterSettings quantised (const MasterSettings& in)
{
    MasterSettings s = in;
    for (const auto& d : definitions())
    {
        double v = d.get (s);
        if (d.kind == Def::real) v = juce::jlimit ((double) d.minimum, (double) d.maximum, v);
        d.set (s, (double) (float) v);
    }
    return s;
}

const std::vector<Def>& definitions()
{
    static const std::vector<Def> defs = build();
    return defs;
}

juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    for (const auto& d : definitions())
    {
        const juce::ParameterID pid { d.id, 1 };
        const auto def = (float) d.defaultValue();
        switch (d.kind)
        {
            case Def::boolean:
                layout.add (std::make_unique<juce::AudioParameterBool> (pid, d.name, def > 0.5f));
                break;
            case Def::choice:
                layout.add (std::make_unique<juce::AudioParameterChoice> (pid, d.name, d.choices, juce::roundToInt (def)));
                break;
            case Def::real:
            default:
            {
                juce::NormalisableRange<float> range (d.minimum, d.maximum);
                if (d.midpoint > d.minimum && d.midpoint < d.maximum) range.setSkewForCentre (d.midpoint);
                layout.add (std::make_unique<juce::AudioParameterFloat> (pid, d.name, range, juce::jlimit (d.minimum, d.maximum, def),
                                                                         juce::AudioParameterFloatAttributes().withLabel (d.unit)));
                break;
            }
        }
    }
    return layout;
}

Bridge::Bridge (juce::AudioProcessorValueTreeState& state) : apvts (state)
{
    for (const auto& d : definitions())
    {
        Item it;
        it.def = &d;
        it.raw = apvts.getRawParameterValue (d.id);
        it.param = apvts.getParameter (d.id);
        jassert (it.raw != nullptr && it.param != nullptr);
        items.push_back (it);
    }
}

void Bridge::read (MasterSettings& s) const noexcept
{
    for (const auto& it : items)
        it.def->set (s, (double) it.raw->load (std::memory_order_relaxed));
}

void Bridge::write (const MasterSettings& s, bool gesture)
{
    for (auto& it : items)
    {
        const auto& d = *it.def;
        float target = (float) d.get (s);
        if (d.kind == Def::real) target = juce::jlimit (d.minimum, d.maximum, target);
        const float now = it.raw->load();
        const float tol = d.kind == Def::real ? 1.0e-6f * juce::jmax (1.0f, std::abs (target)) : 0.25f;
        if (std::abs (now - target) <= tol) continue;
        it.written = target;
        if (gesture) it.param->beginChangeGesture();
        it.param->setValueNotifyingHost (it.param->convertTo0to1 (target));
        if (gesture) it.param->endChangeGesture();
    }
}

bool Bridge::isEcho (const juce::String& id, float value) const
{
    for (const auto& it : items)
        if (it.def->id == id)
            return ! std::isnan (it.written) && std::abs (it.written - value) <= 1.0e-4f * juce::jmax (1.0f, std::abs (value));
    return false;
}

juce::RangedAudioParameter* Bridge::find (const juce::String& id) const
{
    for (const auto& it : items)
        if (it.def->id == id) return it.param;
    return nullptr;
}
} // namespace mihenk::params
