#include "MainComponent.h"
#include "BinaryData.h"
#include "../engine/Preset.h"
#include "ValueParse.h"
#include "../plugin/PluginProcessor.h"
#include "../plugin/Params.h"

namespace mihenk
{
namespace
{
class ValueHit : public juce::Component
{
public:
    std::function<void()> clicked;
    void mouseDown (const juce::MouseEvent&) override { if (clicked) clicked(); }
    void paint (juce::Graphics&) override {}
};

constexpr int kW = MainComponent::panelWidth, kH = MainComponent::panelHeight;
constexpr float kStart = juce::MathConstants<float>::pi * 1.25f, kEnd = juce::MathConstants<float>::pi * 2.75f;
constexpr int kModuleTop = 484, kModuleBottom = 838, kKnob = 42;
constexpr int kRowY[] { 562, 668, 774 };
constexpr int kGapA = 507, kGapB = 614, kGapC = 720, kCapH = 20;
constexpr int kContentRight = 1290;

enum Module { mInput = 0, mEq, mComp, mStereo, mLimiter, mReference, mCharacter, mDelivery, mTarget };
const juce::Rectangle<float> kBoxes[] {
    { 40.0f, (float) kModuleTop, 96.0f, (float) (kModuleBottom - kModuleTop) },
    { 146.0f, (float) kModuleTop, 394.0f, (float) (kModuleBottom - kModuleTop) },
    { 550.0f, (float) kModuleTop, 306.0f, (float) (kModuleBottom - kModuleTop) },
    { 866.0f, (float) kModuleTop, 136.0f, (float) (kModuleBottom - kModuleTop) },
    { 1012.0f, (float) kModuleTop, 136.0f, (float) (kModuleBottom - kModuleTop) },
    { 1158.0f, (float) kModuleTop, 132.0f, (float) (kModuleBottom - kModuleTop) },
    // SIMPLE face
    { 40.0f, (float) kModuleTop, 816.0f, (float) (kModuleBottom - kModuleTop) },
    { 866.0f, (float) kModuleTop, 282.0f, (float) (kModuleBottom - kModuleTop) },
    { 702.0f, 355.0f, 288.0f, 117.0f },
};
const char* kTitles[] { "INPUT", "EQUALIZER", "COMPRESSOR", "STEREO / LOW END", "LIMITER", "REFERENCE", "CHARACTER", "DELIVERY", "TARGET" };
constexpr int kPitch[] { 96, 54, 58, 60, 60, 60, 150, 88, 56 };
constexpr double kAirSteps[] { 10000.0, 15000.0, 20000.0, 40000.0 };
constexpr int kSimpleKnob = 54, kSimpleRowY = 566;
constexpr double kAirNeededDb = 0.05;      // AIR FREQ moves a shelf of this much gain; below it the knob is dimmed and says so

int colCentre (int module, int col)
{
    const auto& b = kBoxes[module];
    if (module == mInput) return (int) b.getCentreX();
    const int x0 = (int) b.getX() + (module == mReference ? 6 : 8);
    return x0 + kPitch[module] / 2 + col * kPitch[module];
}

juce::String hz (double v)
{
    if (v >= 1000.0) return juce::String (v / 1000.0, v < 10000.0 ? 2 : 1) + " KHZ";
    return juce::String (juce::roundToInt (v)) + " HZ";
}
juce::String db (double v, int dec = 1) { return (v > 0.0 ? "+" : "") + juce::String (v, dec) + " DB"; }
juce::String ms (double v) { return juce::String (v, v < 10 ? 1 : 0) + " MS"; }
} // namespace

MainComponent::MainComponent (MihenkProcessor& p)
    : processor (p), session (p.getSession()), live (p.getMeters()), ctx { session, live, p },
      scope (ctx), table (ctx), meters (ctx), info (ctx), learnDisplay (ctx), diagnosis (ctx), crossover (ctx), grTrace (ctx),
      correlation (ctx), limiterReadout (ctx), loudnessVfd (ctx), referencePanel (ctx), chainReadout (ctx), grReadout (ctx), profilePanel (ctx), batchPanel (ctx)
{
    presetsDir = defaultPresetsFolder();
    setLookAndFeel (&lnf);
    setWantsKeyboardFocus (true);
    addMouseListener (this, true);                      // a click anywhere on the panel gives it the keyboard (Cmd+Z in a plug-in window)
    setOpaque (true);
    mark = juce::ImageCache::getFromMemory (BinaryData::mark_cream_png, BinaryData::mark_cream_pngSize);
    wordmark = juce::ImageCache::getFromMemory (BinaryData::wordmark_cream_png, BinaryData::wordmark_cream_pngSize);
    const bool app = processor.isStandaloneApp();

    for (auto* c : std::initializer_list<juce::Component*> { &scope, &table, &meters, &info, &renderLamp, &targetLamp, &limitLamp, &grReadout, &presetDisplay,
                                                             &diagnosis, &crossover, &grTrace, &correlation, &limiterReadout, &loudnessVfd, &referencePanel, &chainReadout })
        addAndMakeVisible (*c);
    if (app) addAndMakeVisible (latencyVfd), latencyVfd.setVisible (false);
    else { addAndMakeVisible (learnDisplay); addAndMakeVisible (latencyVfd); }

    // ---- Knobs, in signal order. -------------------------------------------------------------------------------
    using S = MasterSettings;
    auto id = [] (const char* x) { return [x] { return juce::String (x); }; };
    auto bandId = [this] (const char* wide, const char* suffix)
    {
        return [this, wide, suffix] { return threeBand() ? "b" + juce::String (ctx.selectedBand + 1) + suffix : juce::String (wide); };
    };
    {
        // In AUTO TRIM the knob shows the headroom assistant's solved trim; turning it takes over from there.
        auto& k = addKnob (mInput, 0, 0, "TRIM", -24, 24, 0, 0, 0.1, 1, [] (S& x) -> double& { return x.trimDb; },
                           [this] (double v) { return db (v) + (session.settings.planned && ! session.isOverridden ("trim") ? " PLAN" : ""); }, id ("trim"));
        k.autoKind = Knob::autoTrim;
        k.display = [] (const S& x) { return x.trimDb; };
    }
    addKnob (mInput, 0, 1, "BALANCE", -3, 3, 0, 0, 0.05, 1, [] (S& x) -> double& { return x.balanceDb; }, [] (double v) { return db (v, 2); }, id ("balance"));

    addKnob (mEq, 0, 0, "LOW CUT", 10, 200, 25, 40, 1, 1, [] (S& x) -> double& { return x.eq.lowCutHz; }, [] (double v) { return hz (v); }, id ("lowCutHz"));
    addKnob (mEq, 1, 0, "LOW SHELF", -6, 6, 0, 0, 0.1, 1, [] (S& x) -> double& { return x.eq.lowShelfDb; }, [] (double v) { return db (v); }, id ("lowShelfDb"));
    addKnob (mEq, 2, 0, "SHELF FREQ", 30, 400, 90, 100, 1, 1, [] (S& x) -> double& { return x.eq.lowShelfHz; }, [] (double v) { return hz (v); }, id ("lowShelfHz"));
    addKnob (mEq, 3, 0, "BELL 1", -8, 8, 0, 0, 0.1, 1, [] (S& x) -> double& { return x.eq.bell1Db; }, [] (double v) { return db (v); }, id ("bell1Db"));
    addKnob (mEq, 4, 0, "BELL 1 FREQ", 40, 16000, 250, 700, 1, 1, [] (S& x) -> double& { return x.eq.bell1Hz; }, [] (double v) { return hz (v); }, id ("bell1Hz"));
    addKnob (mEq, 5, 0, "BELL 1 Q", 0.3, 6, 1, 1.2, 0.01, 1, [] (S& x) -> double& { return x.eq.bell1Q; }, [] (double v) { return "Q " + juce::String (v, 2); }, id ("bell1Q"));
    addKnob (mEq, 6, 0, "AIR", 0, 6, 0, 0, 0.1, 1, [] (S& x) -> double& { return x.eq.airDb; }, [] (double v) { return db (v); }, id ("airDb"));
    addKnob (mEq, 0, 1, "TILT", -1.5, 1.5, 0, 0, 0.05, 1, [] (S& x) -> double& { return x.eq.tiltDbPerOct; }, [] (double v) { return db (v, 2) + "/OCT"; }, id ("tilt"));
    addKnob (mEq, 1, 1, "HIGH SHELF", -6, 6, 0, 0, 0.1, 1, [] (S& x) -> double& { return x.eq.highShelfDb; }, [] (double v) { return db (v); }, id ("highShelfDb"));
    addKnob (mEq, 2, 1, "SHELF FREQ", 2000, 16000, 10000, 7000, 10, 1, [] (S& x) -> double& { return x.eq.highShelfHz; }, [] (double v) { return hz (v); }, id ("highShelfHz"));
    addKnob (mEq, 3, 1, "BELL 2", -8, 8, 0, 0, 0.1, 1, [] (S& x) -> double& { return x.eq.bell2Db; }, [] (double v) { return db (v); }, id ("bell2Db"));
    addKnob (mEq, 4, 1, "BELL 2 FREQ", 40, 16000, 3000, 2500, 1, 1, [] (S& x) -> double& { return x.eq.bell2Hz; }, [] (double v) { return hz (v); }, id ("bell2Hz"));
    addKnob (mEq, 5, 1, "BELL 2 Q", 0.3, 6, 1, 1.2, 0.01, 1, [] (S& x) -> double& { return x.eq.bell2Q; }, [] (double v) { return "Q " + juce::String (v, 2); }, id ("bell2Q"));
    {
        // AIR FREQ is a 4-position switch (10, 15, 20, 40 kHz, the plug-in's choice parameter): the travel is four equal zones, the pointer rests
        // in the middle of its zone, a drag that crosses into the next zone selects that step. It does nothing while AIR is 0 dB, so it is dimmed then.
        auto& k = addKnob (mEq, 6, 1, "AIR FREQ", 0, (double) std::size (kAirSteps), 2.5, 0, 0, 1, [] (S& x) -> double& { return x.eq.airHz; },
                           [] (double v) { return juce::String (juce::roundToInt (kAirSteps[(size_t) juce::jlimit (0, 3, (int) std::floor (v))] / 1000.0)) + " KHZ"; }, id ("airHz"));
        k.steps.assign (std::begin (kAirSteps), std::end (kAirSteps));
        k.slider.pixelsOverride = 120.0;           // 30 px of drag per step
        k.tip = "AIR FREQ: where the AIR shelf lifts the top (10, 15, 20 or 40 kHz). Drag to the next zone, or click the value, to change the step. It only does something while AIR is above 0 dB.";
    }
    addKnob (mEq, 0, 2, "LO-MID FREQ", 150, 800, 300, 320, 1, 1, [] (S& x) -> double& { return x.eq.dynLowMid.freqHz; }, [] (double v) { return hz (v); }, id ("lowMidHz"));
    addKnob (mEq, 1, 2, "LO-MID CUT", 0, 4, 1.5, 0, 0.1, 1, [] (S& x) -> double& { return x.eq.dynLowMid.amountDb; }, [] (double v) { return juce::String (v, 1) + " DB"; }, id ("lowMidAmount"));
    addKnob (mEq, 2, 2, "HI-MID FREQ", 1500, 8000, 3500, 3500, 10, 1, [] (S& x) -> double& { return x.eq.dynUpperMid.freqHz; }, [] (double v) { return hz (v); }, id ("upperMidHz"));
    addKnob (mEq, 3, 2, "HI-MID CUT", 0, 4, 1.5, 0, 0.1, 1, [] (S& x) -> double& { return x.eq.dynUpperMid.amountDb; }, [] (double v) { return juce::String (v, 1) + " DB"; }, id ("upperMidAmount"));
    addKnob (mEq, 4, 2, "TIGHT FREQ", 40, 200, 90, 90, 1, 1, [] (S& x) -> double& { return x.eq.tightener.freqHz; }, [] (double v) { return hz (v); }, id ("tightHz"));
    addKnob (mEq, 5, 2, "TIGHT CUT", 0, 4, 1.5, 0, 0.1, 1, [] (S& x) -> double& { return x.eq.tightener.amountDb; }, [] (double v) { return juce::String (v, 1) + " DB"; }, id ("tightAmount"));
    addKnob (mEq, 6, 2, "DE-HARSH", 0, 8, 4, 0, 0.1, 1, [] (S& x) -> double& { return x.eq.airDeHarshDb; }, [] (double v) { return juce::String (v, 1) + " DB"; }, id ("airDeHarsh"));

    // Compressor: WIDE, or the selected band in 3-BAND (the same knobs, the BAND caps pick which).
    addKnob (mComp, 0, 0, "GLUE", 0, 4, 1.2, 0, 0.1, 1, [this] (S& x) -> double& { return threeBand() ? x.comp.bands[ctx.selectedBand].glueDb : x.comp.glueDb; },
             [] (double v) { return juce::String (v, 1) + " DB GR"; }, bandId ("glue", "Glue"));
    addKnob (mComp, 1, 0, "THRESHOLD", -60, 0, -18, 0, 0.1, 1, [this] (S& x) -> double& { return threeBand() ? x.comp.bands[ctx.selectedBand].thresholdDb : x.comp.thresholdDb; },
             [this] (double v) { const bool a = threeBand() ? band().autoThreshold : session.settings.comp.autoThreshold; return juce::String (v, 1) + (a ? " DB AUTO" : " DBFS"); },
             bandId ("threshold", "Threshold")).autoKind = Knob::autoThreshold;
    addKnob (mComp, 2, 0, "RATIO", 1, 8, 2, 2.5, 0.05, 1, [this] (S& x) -> double& { return threeBand() ? x.comp.bands[ctx.selectedBand].ratio : x.comp.ratio; },
             [] (double v) { return juce::String (v, 2) + ":1"; }, bandId ("ratio", "Ratio"));
    addKnob (mComp, 3, 0, "ATTACK", 1, 200, 30, 25, 0.5, 1, [this] (S& x) -> double& { return threeBand() ? x.comp.bands[ctx.selectedBand].attackMs : x.comp.attackMs; },
             [] (double v) { return ms (v); }, bandId ("attack", "Attack"));
    addKnob (mComp, 4, 0, "RELEASE", 20, 1500, 250, 250, 1, 1, [this] (S& x) -> double& { return threeBand() ? x.comp.bands[ctx.selectedBand].releaseMs : x.comp.releaseMs; },
             [this] (double v) { return juce::String (juce::roundToInt (v)) + (session.settings.comp.autoRelease ? " MS AUTO" : " MS"); }, bandId ("release", "Release")).autoKind = Knob::autoRelease;
    addKnob (mComp, 0, 1, "SC HPF", 20, 300, 90, 90, 1, 1, [] (S& x) -> double& { return x.comp.scHpfHz; }, [] (double v) { return v <= 20.5 ? juce::String ("OFF") : hz (v); }, id ("scHpf")).showIn = 1;
    addKnob (mComp, 0, 1, "BAND GAIN", -6, 6, 0, 0, 0.1, 1, [this] (S& x) -> double& { return x.comp.bands[ctx.selectedBand].gainDb; }, [] (double v) { return db (v); },
             [this] { return "b" + juce::String (ctx.selectedBand + 1) + "Gain"; }).showIn = 2;
    addKnob (mComp, 1, 1, "RMS / PEAK", 0, 100, 70, 0, 1, 100, [] (S& x) -> double& { return x.comp.rmsBlend; }, [] (double v) { return juce::String (juce::roundToInt (v)) + " % RMS"; }, id ("rms"));
    addKnob (mComp, 2, 1, "KNEE", 0, 12, 6, 0, 0.5, 1, [] (S& x) -> double& { return x.comp.kneeDb; }, [] (double v) { return juce::String (v, 1) + " DB"; }, id ("knee"));
    addKnob (mComp, 3, 1, "MIX", 0, 100, 100, 0, 1, 100, [] (S& x) -> double& { return x.comp.mix; }, [] (double v) { return juce::String (juce::roundToInt (v)) + " %"; }, id ("mix"));
    addKnob (mComp, 0, 2, "XOVER LOW", 40, 400, 120, 120, 1, 1, [] (S& x) -> double& { return x.comp.xoverLowHz; }, [] (double v) { return hz (v); }, id ("xoverLow")).showIn = 2;
    addKnob (mComp, 1, 2, "XOVER HIGH", 2000, 10000, 4000, 4000, 10, 1, [] (S& x) -> double& { return x.comp.xoverHighHz; }, [] (double v) { return hz (v); }, id ("xoverHigh")).showIn = 2;

    addKnob (mStereo, 0, 0, "WIDTH", 0, 200, 100, 0, 1, 100, [] (S& x) -> double& { return x.width.width; }, [] (double v) { return juce::String (juce::roundToInt (v)) + " %"; }, id ("width"));
    addKnob (mStereo, 1, 0, "MONO BELOW", 0, 300, 0, 0, 5, 1, [] (S& x) -> double& { return x.width.monoBelowHz; }, [] (double v) { return v <= 0.0 ? juce::String ("OFF") : hz (v); }, id ("monoBelow"));
    addKnob (mStereo, 0, 1, "HIGH WIDTH", 0, 200, 100, 0, 1, 100, [] (S& x) -> double& { return x.width.highWidth; }, [] (double v) { return juce::String (juce::roundToInt (v)) + " %"; }, id ("highWidth"));
    addKnob (mStereo, 1, 1, "HIGH FROM", 1000, 12000, 4000, 4000, 10, 1, [] (S& x) -> double& { return x.width.highWidthHz; }, [] (double v) { return hz (v); }, id ("highWidthHz"));

    addKnob (mLimiter, 0, 0, "DRIVE", -24, 36, 0, 0, 0.01, 1, [] (S& x) -> double& { return x.limiter.driveDb; },
             [this] (double v) { return db (v, 2) + (session.settings.planned && ! session.isOverridden ("drive") ? " PLAN" : ""); }, id ("drive")).autoKind = Knob::autoDrive;
    addKnob (mLimiter, 1, 0, "CEILING", -3, -0.1, -1, 0, 0.1, 1, [] (S& x) -> double& { return x.limiter.ceilingDbtp; }, [] (double v) { return juce::String (v, 1) + " DBTP"; }, id ("ceiling"));
    addKnob (mLimiter, 0, 1, "RELEASE", 10, 1000, 150, 150, 1, 1, [] (S& x) -> double& { return x.limiter.releaseMs; }, [] (double v) { return juce::String (juce::roundToInt (v)) + " MS"; }, id ("limRelease"));
    addKnob (mLimiter, 1, 1, "LOOK-AHEAD", 0.5, 5, 1.5, 0, 0.1, 1, [] (S& x) -> double& { return x.limiter.lookaheadMs; }, [] (double v) { return juce::String (v, 1) + " MS"; }, id ("lookahead"));

    addKnob (mLimiter, 0, 2, "WARN AT", 1, 12, 6, 0, 0.5, 1, [] (S& x) -> double& { return x.limitWarnDb; }, [] (double v) { return juce::String (v, 1) + " DB GR"; }, id ("warnAt"));

    addKnob (mReference, 0, 0, "TONE AMOUNT", 0, 100, 100, 0, 1, 100, [] (S& x) -> double& { return x.eq.matchAmount; }, [] (double v) { return juce::String (juce::roundToInt (v)) + " %"; }, id ("toneAmount")).needsReference = true;
    addKnob (mReference, 1, 0, "SMOOTH", 0.33, 3, 1, 0, 0.01, 1, [] (S& x) -> double& { return x.toneSmoothOct; }, [] (double v) { return juce::String (v, 2) + " OCT"; }, id ("toneSmooth")).needsReference = true;

    // The TARGET block (both faces): the loudness target; turning it picks CUSTOM, KEEP INPUT shows the input's loudness.
    {
        auto& k = addKnob (mTarget, 0, 0, "TARGET", -30, -4, -14, 0, 0.1, 1, [] (S& x) -> double& { return x.targetLufs; },
                           [this] (double v) { return juce::String (v, 1) + (session.settings.targetPreset == TargetPreset::keepInput ? " KEEP" : " LUFS"); }, id ("targetLufs"));
        k.face = 0; k.cx = 729; k.cy = 418;
    }

    // SIMPLE face: five macro knobs on the character, and the delivery (drive and ceiling).
    {
        auto macro = [&] (int i, const juce::String& label, double mn, double mx, double def, double interval, double scale, std::function<double& (S&)> ref,
                          std::function<juce::String (double)> format, const char* pid, const juce::String& hint, int profileParam) -> Knob&
        {
            auto& k = addKnob (mCharacter, i, 0, label, mn, mx, def, 0, interval, scale, std::move (ref), std::move (format), id (pid));
            k.face = 2; k.size = kSimpleKnob; k.hint = hint;
            k.cx = (int) (kBoxes[mCharacter].getX() + kBoxes[mCharacter].getWidth() * (0.1f + 0.2f * (float) i));
            k.cy = kSimpleRowY;
            k.profileParam = profileParam;
            return k;
        };
        macro (0, "LOW END", -6, 6, 0, 0.1, 1, [] (S& x) -> double& { return x.eq.lowShelfDb; }, [] (double v) { return db (v); }, "lowShelfDb", "LESS   <   BASS   >   MORE", profile::pLowShelf);
        macro (1, "TONE", -1.5, 1.5, 0, 0.05, 1, [] (S& x) -> double& { return x.eq.tiltDbPerOct; }, [] (double v) { return db (v, 2) + "/OCT"; }, "tilt", "DARKER   <   >   BRIGHTER", -1);
        macro (2, "AIR", 0, 6, 0, 0.1, 1, [] (S& x) -> double& { return x.eq.airDb; }, [] (double v) { return db (v); }, "airDb", "OPENS THE VERY TOP", profile::pAir);
        {
            auto& g = macro (3, "GLUE", 0, 4, 1.2, 0.1, 1, [] (S& x) -> double& { return x.comp.glueDb; }, [] (double v) { return juce::String (v, 1) + " DB GR"; }, "glue",
                             "HOW MUCH THE MIX HOLDS TOGETHER", profile::pGlue);
            g.display = [] (const S& x)
            {
                if (x.comp.mode != CompMode::threeBand) return x.comp.glueDb;
                return (x.comp.bands[0].glueDb + x.comp.bands[1].glueDb + x.comp.bands[2].glueDb) / 3.0;
            };
        }
        macro (4, "WIDTH", 0, 200, 100, 1, 100, [] (S& x) -> double& { return x.width.width; }, [] (double v) { return juce::String (juce::roundToInt (v)) + " %"; }, "width",
               "NARROWER   <   >   WIDER", profile::pWidth);
        auto& d = addKnob (mDelivery, 0, 0, "DRIVE", -24, 36, 0, 0, 0.01, 1, [] (S& x) -> double& { return x.limiter.driveDb; },
                           [] (double v) { return db (v, 1) + " PLAN"; }, id ("drive"));
        d.label = "LEVEL";
        d.display = [] (const S& x) { return x.trimDb + x.limiter.driveDb; };
        d.face = 2; d.size = 48; d.cx = 918; d.cy = kSimpleRowY; d.hint = "TOTAL GAIN: PLANNED, WITH THE LIMITS";
        auto& c = addKnob (mDelivery, 1, 0, "CEILING", -3, -0.1, -1, 0, 0.1, 1, [] (S& x) -> double& { return x.limiter.ceilingDbtp; }, [] (double v) { return juce::String (v, 1) + " DBTP"; }, id ("ceiling"));
        c.face = 2; c.size = 48; c.cx = 1008; c.cy = kSimpleRowY; c.hint = "TRUE-PEAK LIMIT";
    }

    for (auto& k : knobs)
    {
        if (k->label == "TARGET")
            k->after = [this] { session.settings.targetPreset = TargetPreset::custom; session.settingsChanged(); syncFromSettings(); };
        if (k->label == "TRIM")
            k->after = [this] { session.settingsChanged(); syncFromSettings(); };     // by hand: an override over the plan
        if (k->label == "RELEASE" && k->module == mComp)
            k->after = [this] { session.settings.comp.autoRelease = false; session.settingsChanged(); syncFromSettings(); };
        if (k->label == "GLUE" && k->face == 2)
            k->after = [this]
            {
                // SIMPLE GLUE: the WIDE glue, or in 3-BAND every band scaled together (their balance kept).
                auto& c = session.settings.comp;
                c.enabled = true;
                if (c.mode == CompMode::threeBand)
                {
                    const double mean = (c.bands[0].glueDb + c.bands[1].glueDb + c.bands[2].glueDb) / 3.0;
                    for (auto& b : c.bands) { b.glueDb = mean > 0.05 ? juce::jlimit (0.0, 4.0, b.glueDb * c.glueDb / mean) : c.glueDb; b.autoThreshold = true; }
                }
                else c.autoThreshold = true;
                session.settingsChanged();
                syncFromSettings();
            };
        if (k->label == "SMOOTH")
            k->after = [this] { if (! session.settings.eq.matchCurveDb.empty()) session.matchTone(); else session.settingsChanged(); };
        if (k->label == "THRESHOLD")
            k->after = [this] { if (threeBand()) band().autoThreshold = false; else session.settings.comp.autoThreshold = false; session.settingsChanged(); syncFromSettings(); };
        if (k->label == "GLUE" && k->face != 2)
            k->after = [this] { if (threeBand()) band().autoThreshold = true; else session.settings.comp.autoThreshold = true; session.settingsChanged(); syncFromSettings(); };
        if (k->label == "AIR FREQ")
            k->after = [this] { session.settingsChanged(); syncFromSettings(); };      // the step itself is picked in the knob's value handler
        if (k->label == "MONO BELOW")
            k->after = [this]
            {
                auto& m = session.settings.width.monoBelowHz;
                if (m > 0.0 && m < 20.0) m = 0.0;
                else if (m > 0.0 && m < 40.0) m = 40.0;       // the stereo split's lowest frequency
                session.settingsChanged();
                syncFromSettings();
            };
        if (k->label == "DRIVE")
            k->after = [this] { session.settingsChanged(); syncFromSettings(); };
        if (k->label.startsWith ("LO-MID") || k->label.startsWith ("HI-MID") || k->label.startsWith ("TIGHT"))
            k->after = [this, raw = k.get()]
            {
                // Turning a dynamic band's knob switches it in.
                auto& e = session.settings.eq;
                if (raw->label.startsWith ("LO-MID")) e.dynLowMid.enabled = true;
                else if (raw->label.startsWith ("HI-MID")) e.dynUpperMid.enabled = true;
                else e.tightener.enabled = true;
                session.settingsChanged();
                syncFromSettings();
            };
    }

    // SIMPLE shows the plan: the five macros and the LEVEL dial are read-only (SPEC_V3: no macro writes a knob).
    for (auto& k : knobs)
        if (k->face == 2 && (k->module == mCharacter || k->label == "LEVEL")) k->slider.setEnabled (false);

    // Personal profile: the learnable knobs carry a lock badge while a learned value holds them.
    for (auto& k : knobs)
    {
        if (k->face != 2)
            for (int p = 0; p < profile::kNumParams; ++p)
                if (k->label == profile::def (p).knob && k->showIn != 2) k->profileParam = p;
        if (k->profileParam < 0) continue;
        k->lockBadge = std::make_unique<ui::LockBadge>();
        addChildComponent (*k->lockBadge);
        k->lockBadge->onClick = [this, raw = k.get()]
        {
            const int p = activeProfileParam (*raw);
            if (p < 0 || ! session.isLocked (p)) return;
            session.unlock (p);
            session.sendChangeMessage();
            syncFromSettings();
        };
    }

    // AUTO lamps: lit while MIHENK solves the knob's value; click to switch AUTO on or off.
    for (auto& k : knobs)
    {
        if (k->autoKind == Knob::autoNone) continue;
        k->autoLamp = std::make_unique<ui::AutoLamp>();
        addChildComponent (*k->autoLamp);
        auto* raw = k.get();
        k->autoLamp->onClick = [this, raw] { setAuto (*raw, ! isAuto (*raw)); };
        const char* tips[] { "", "AUTO threshold: solved from your mix so the loud part gets the GLUE amount of gain reduction. Lit: the knob shows the solved value. Turning the knob switches AUTO off; click the lamp to switch it back on.",
                             "AUTO release: program-dependent, it recovers at a third of this time after transients and four times it after long reduction. Turning the knob switches AUTO off; click the lamp to switch it back on.",
                             "AUTO trim (headroom assistant): the knob shows the pre-trim solved to bring the mix to the working level (peak -6 dBFS, short-term max -14 LUFS). Turning the knob takes over from that value; click the lamp for AUTO again.",
                             "AUTO drive: solved after every change so the output holds the loudness target. Turning the knob switches it off (the target is then not held); click the lamp or pick a target to hold it again." };
        k->autoLamp->setTooltip (tips[k->autoKind]);
    }

    // ---- Keys and switches. ------------------------------------------------------------------------------------
    for (auto* k : { &refKey, &abKey, &bypassKey }) addAndMakeVisible (*k);
    addChildComponent (matchRefKey);
    if (app) for (auto* k : { &openKey, &playKey, &stopKey, &loopKey, &exportKey, &batchKey, &audioKey }) addAndMakeVisible (*k);
    else addAndMakeVisible (learnKey);
    bypassKey.ledColour = Theme::coral;
    learnKey.ledColour = Theme::lamp;
    learnKey.blink = true;
    openKey.onClick = [this] { chooseFile (false); };
    refKey.onClick = [this] { chooseFile (true); };
    playKey.onClick = [this] { auto& pl = session.player; if (pl.isPlaying()) pl.pause(); else pl.play(); };
    stopKey.onClick = [this] { session.player.stop(); scope.repaint(); };
    loopKey.onClick = [this]
    {
        auto& pl = session.player;
        if (pl.getLoopEnd() <= pl.getLoopStart()) pl.setLoopRegion (0, pl.getLength());
        pl.setLoop (! pl.isLooping());
        scope.repaint();
    };
    abKey.onClick = [this] { processor.setListenOriginal (! processor.isListeningOriginal()); scope.repaint(); };
    exportKey.onClick = [this] { chooseExport(); };
    audioKey.onClick = [this]
    {
        if (processor.showAudioSettings) processor.showAudioSettings();
        else juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Audio", "The host owns the audio device.");
    };
    learnKey.onClick = [this] { processor.toggleLearn(); };
    matchRefKey.onClick = [this] { session.matchToReference(); syncFromSettings(); };
    clearRef.onClick = [this] { session.clearReference(); syncFromSettings(); };
    batchKey.onClick = [this] { showBatchPanel (! batchPanel.isVisible()); };
    abMatchCap.onClick = [this] { processor.setAbLevelMatched (abMatchCap.getToggleState()); };
    resetAllCap.showLed = false;
    resetAllCap.setTooltip ("RESET ALL: every knob and switch, the tone-match curve, the dynamic-EQ details, what MIHENK learned and the captured audio go back to a new instance. Presets, notes and the view stay. Asks first.");
    resetAllCap.onClick = [this]
    {
        juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon)
                                          .withTitle ("RESET ALL")
                                          .withMessage ("Every setting, the tone-match curve, the dynamic-EQ details, what MIHENK learned and the captured audio go back to a new instance.\n\nYour presets, notes and the view stay.")
                                          .withButton ("RESET").withButton ("CANCEL")
                                          .withAssociatedComponent (this),
                                      [this] (int result)
                                      {
                                          if (result != 1) return;
                                          processor.resetAll();
                                          syncFromSettings();
                                      });
    };
    faceSimple.onClick = [this] { setFace (false); };
    faceAdvanced.onClick = [this] { setFace (true); };
    charOff.onClick = [this] { session.setCharacter (PlanCharacter::OFF); syncFromSettings(); };
    charGentle.onClick = [this] { session.setCharacter (PlanCharacter::GENTLE); syncFromSettings(); };
    charFull.onClick = [this] { session.setCharacter (PlanCharacter::FULL); syncFromSettings(); };
    resetOverridesCap.showLed = false;
    resetOverridesCap.setTooltip ("RESET OVERRIDES: drops every value you set over the plan; every knob shows what MIHENK planned again. The target and the character stay.");
    resetOverridesCap.onClick = [this] { session.clearOverrides(); syncFromSettings(); };
    charOff.setTooltip ("CHARACTER OFF (default): level and a true-peak ceiling only. Nothing else is touched.");
    charGentle.setTooltip ("CHARACTER GENTLE: adds at most 1 dB of broadband glue and, when the top is dark, up to 1.5 dB of AIR. Never on a finished master.");
    charFull.setTooltip ("CHARACTER FULL: the diagnosis findings are applied as the plan, each bounded and listed in dB. Never on a finished master.");
    bypassKey.setClickingTogglesState (true);
    bypassKey.onClick = [this] { session.settings.bypass = bypassKey.getToggleState(); updateDimming(); session.settingsChanged(); };

    auto view = [this] (ui::CrtScope::Mode m) { setScopeMode (m); };
    viewWave.onClick = [view] { view (ui::CrtScope::Mode::wave); };
    viewSpec.onClick = [view] { view (ui::CrtScope::Mode::spectrum); };
    viewLoud.onClick = [view] { view (ui::CrtScope::Mode::loudness); };
    ui::CapButton* layerCaps[] { &layerIn, &layerEq, &layerComp, &layerOut };
    for (int l = 0; l < 4; ++l)
        layerCaps[l]->onClick = [this, l, cap = layerCaps[l]] { ctx.layers[(size_t) l] = cap->getToggleState(); scope.repaint(); };
    // Picking a target holds it: the drive is solved again after every change (the DRIVE knob's AUTO lamp).
    auto preset = [this] (TargetPreset t)
    {
        // The target is intent: the plan is solved again from the measurements, nothing else is touched.
        if (t == TargetPreset::custom) session.setCustomTarget (session.settings.targetLufs);
        else session.setTarget (Session::targetOfPreset (t));
        syncFromSettings();
    };
    presetStream.onClick = [preset] { preset (TargetPreset::streaming); };
    presetApple.onClick = [preset] { preset (TargetPreset::apple); };
    presetClub.onClick = [preset] { preset (TargetPreset::club); };
    presetKeep.onClick = [preset] { preset (TargetPreset::keepInput); };
    presetCustom.onClick = [preset] { preset (TargetPreset::custom); };
    bitsCap.onClick = [this] { exportBits = exportBits == 24 ? 16 : 24; syncFromSettings(); };

    // tone: the switch belongs to EQ / compressor / stereo; flipping it by hand ends the automatic skip for finished input.
    auto sw = [this] (ui::CapButton& b, std::function<void (bool)> set, bool tone = true)
    {
        b.onClick = [this, &b, set, tone] { if (syncing) return; if (tone) session.noteHandEdit(); set (b.getToggleState()); updateDimming(); session.settingsChanged(); syncFromSettings(); };
    };
    sw (eqIn, [this] (bool v) { session.settings.eq.enabled = v; });
    sw (eqLowCut, [this] (bool v) { session.settings.eq.lowCutOn = v; });
    sw (eqSteep, [this] (bool v) { session.settings.eq.lowCutSlope = v ? 24 : 12; });
    sw (eqLinear, [this] (bool v) { session.settings.eq.linearPhase = v; });
    sw (eqAirDyn, [this] (bool v) { session.settings.eq.airDynamic = v; });
    sw (dynLowMid, [this] (bool v) { session.settings.eq.dynLowMid.enabled = v; });
    sw (dynUpperMid, [this] (bool v) { session.settings.eq.dynUpperMid.enabled = v; });
    sw (dynTight, [this] (bool v) { session.unlock (profile::pTight); session.settings.eq.tightener.enabled = v; });
    sw (dcFilter, [this] (bool v) { session.settings.dcFilter = v; }, false);
    sw (compIn, [this] (bool v) { session.settings.comp.enabled = v; });
    sw (compThreeBand, [this] (bool v) { session.settings.comp.mode = v ? CompMode::threeBand : CompMode::wide; if (v) session.settings.comp.enabled = true; });
    sw (bandSolo, [this] (bool v) { band().solo = v; });
    sw (bandBypass, [this] (bool v) { band().bypass = v; });
    sw (widthIn, [this] (bool v) { session.settings.width.enabled = v; });
    sw (phaseSafe, [this] (bool v)
    {
        session.settings.width.phaseSafeLowMono = v;
        if (v && session.settings.width.monoBelowHz <= 0.0) session.settings.width.monoBelowHz = 120.0;   // phase-safe mono needs a mono-below frequency
    });
    sw (limIn, [this] (bool v) { session.settings.limiter.enabled = v; }, false);
    limIn.setEnabled (false);       // v3: the true-peak limiter is the ceiling; it is always in
    limIn.setTooltip ("The true-peak limiter is always in: it is the ceiling (SPEC_V3). Set the CEILING knob instead.");
    ui::CapButton* bands[] { &bandLow, &bandMid, &bandHigh };
    for (int b = 0; b < 3; ++b) bands[b]->onClick = [this, b] { selectBand (b); };
    crossover.onSelectBand = [this] (int b) { selectBand (b); };

    for (auto* c : { &viewWave, &viewSpec, &viewLoud, &layerIn, &layerEq, &layerComp, &layerOut, &presetStream, &presetApple, &presetClub, &presetKeep, &presetCustom,
                     &eqIn, &eqLowCut, &eqSteep, &eqLinear, &eqAirDyn, &dynLowMid, &dynUpperMid, &dynTight, &dcFilter,
                     &compIn, &compThreeBand, &bandLow, &bandMid, &bandHigh, &bandSolo, &bandBypass,
                     &widthIn, &phaseSafe, &limIn, &presetSave, &presetLoad, &presetDelete, &abMatchCap, &faceSimple, &faceAdvanced, &resetAllCap, &resetOverridesCap, &charOff, &charGentle, &charFull })
        addAndMakeVisible (*c);
    addChildComponent (clearRef);
    if (app) addAndMakeVisible (bitsCap);
    abMatchCap.ledColour = Theme::teal.brighter (0.3f);
    abMatchCap.setToggleState (session.abLevelMatched(), juce::dontSendNotification);
    clearRef.showLed = false;
    for (auto* c : { &layerIn, &layerEq, &layerComp, &layerOut }) c->setToggleState (true, juce::dontSendNotification);
    layerEq.ledColour = Theme::teal.brighter (0.3f);
    layerComp.ledColour = Theme::lamp;
    layerIn.ledColour = Theme::phosphor.darker (0.2f);
    layerOut.ledColour = Theme::phosphor;
    for (auto* c : { &presetSave, &presetLoad, &presetDelete }) c->showLed = false;
    presetSave.onClick = [this] { savePresetAs (false); };
    presetLoad.onClick = [this] { showPresetMenu(); };
    presetDelete.onClick = [this] { deleteCurrentPreset(); };
    for (auto* c : { &viewWave, &viewSpec, &viewLoud, &presetStream, &presetApple, &presetClub, &presetKeep, &presetCustom, &bandLow, &bandMid, &bandHigh, &faceSimple, &faceAdvanced,
                     &charOff, &charGentle, &charFull })
        c->setClickingTogglesState (false);

    // Personal profile: the NOTE key (its LED shows the profile is on) opens the panel over the CRT.
    addAndMakeVisible (noteKey);
    noteKey.setClickingTogglesState (false);
    noteKey.onClick = [this] { showProfilePanel (! profilePanel.isVisible()); };
    noteKey.setTooltip ("NOTE: listening notes and your personal profile (what MIHENK learned from your notes, and the knobs it locked)");
    addChildComponent (profilePanel);
    addChildComponent (batchPanel);
    batchPanel.onClose = [this] { showBatchPanel (false); };
    batchPanel.exportBits = [this] { return exportBits; };
    batchPanel.onOpenInPanel = [this] (const juce::File& f, const Intent* own)
    {
        if (own != nullptr)
        {
            // The track's own intent (target, ceiling, character); overrides belong to one measurement and stay in the batch.
            session.intent.target = own->target; session.intent.customLufs = own->customLufs; session.intent.albumTarget = own->albumTarget;
            session.intent.relativeDb = own->relativeDb; session.intent.ceilingDbtp = own->ceilingDbtp; session.intent.character = own->character;
        }
        session.loadInput (f);
        showBatchPanel (false);
    };
    profilePanel.startingPoint = [this] { return currentPresetName; };
    profilePanel.onClose = [this] { showProfilePanel (false); };
    profilePanel.onChanged = [this] { syncFromSettings(); };

    diagnosis.onStartingPoint = [this] (Genre g) { applyFactory (g); };
    diagnosis.onApply = [this] (int sc) { applyScenarioFromPanel (sc); };
    diagnosis.onRemove = [this] (int sc) { session.removeScenario (sc); syncFromSettings(); diagnosis.refresh(); };
    diagnosis.onSaveScenario = [this] { savePresetAs (true); };

    // Tooltips: what each control measures or does.
    openKey.setTooltip ("Open a WAV or AIFF (or drop it on the panel)");
    refKey.setTooltip ("Load a reference track (or drop it on the right half of the panel)");
    abKey.setTooltip (app ? "A = the original, B = the master. With A/B MATCHED (default) the original plays at the master's loudness, so only the sound differs"
                          : "A = the input, B = MIHENK. With A/B MATCHED (default) the input plays at the learned output's loudness");
    abMatchCap.setTooltip ("A/B MATCHED: the original plays at the master's loudness (louder always sounds better, so the honest comparison is at equal loudness). Off: raw levels");
    matchRefKey.setTooltip ("MATCH TO REFERENCE: the tone-match curve (TONE AMOUNT, SMOOTH) and the reference's loudness as the target, in one step");
    clearRef.setTooltip ("Remove the reference");
    batchKey.setTooltip ("BATCH / ALBUM: master many files into one folder with a report, in the background");
    presetKeep.setTooltip ("KEEP INPUT: hold the mix's own loudness; the limiter only guards the ceiling (at most 1 dB of extra drive), never louder than the input");
    presetStream.setTooltip ("-14 LUFS: Spotify and YouTube turn louder masters down to this");
    presetApple.setTooltip ("-16 LUFS: Apple Music's normalisation level");
    presetClub.setTooltip ("-8 LUFS: loud club / DJ delivery (no platform normalisation)");
    bitsCap.setTooltip ("Export word length: 24-bit (no dither) or 16-bit (TPDF dither); click to switch");
    faceSimple.setTooltip ("SIMPLE: starting point, target, diagnosis, five macro knobs, meters and views");
    faceAdvanced.setTooltip ("ADVANCED: every control of the chain");
    learnKey.setTooltip ("LEARN: press, then play the song once. MIHENK listens, then measures, diagnoses and solves AUTO and TARGET LOCK on the whole song");

    compThreeBand.setTooltip ("MODE: WIDE (one broadband glue) or 3-BAND (Linkwitz-Riley crossovers, a compressor per band, level-neutral)");
    eqAirDyn.setTooltip ("Dynamic AIR: eases the AIR boost, then cuts 5-9 kHz, while that band stands out (de-harsh / de-ess)");
    dynLowMid.setTooltip ("Dynamic low-mid cut: a bell that cuts only when its band gets loud (mud); CUT = mean cut on the loud part");
    dynUpperMid.setTooltip ("Dynamic upper-mid cut: a bell that cuts only when its band gets loud (harshness)");
    dynTight.setTooltip ("Low tightener: a dynamic low shelf that holds a loose sub in place");
    dcFilter.setTooltip ("DC filter: 5 Hz high-pass (zero phase in the app's render, causal in the plug-in)");
    phaseSafe.setTooltip ("Phase-safe low mono: below MONO BELOW the lows are summed with the polarity that adds, energy restored");
    eqLinear.setTooltip ("Linear-phase EQ (adds about 170 ms latency in a DAW); off = minimum phase");
    presetSave.setTooltip ("Save the current settings as a user preset");
    presetLoad.setTooltip ("Your presets, the 20 scenarios, open a preset file, reveal the presets folder (starting points: the START caps)");
    presetDelete.setTooltip ("Move the current user preset to the Trash (asks first)");
    layerIn.setTooltip ("Show the input layer");
    layerEq.setTooltip ("Show the signal after the EQ section (live spectrum)");
    layerComp.setTooltip ("Show the signal after the compressor (live spectrum)");
    layerOut.setTooltip ("Show the output layer");
    bandSolo.setTooltip ("Listen to the selected band only");
    bandBypass.setTooltip ("The selected band passes uncompressed (its gain still applies)");

    markPresetBaseline ("DEFAULT", false, {});
    advanced = ui::UiPrefs::advancedFace();
    setSize (kW, kH);
    syncFromSettings();
    animateKnobs = true;
    diagnosis.refresh();
    session.addChangeListener (this);
    startTimerHz (30);
}

MainComponent::~MainComponent()
{
    stopTimer();
    session.removeChangeListener (this);
    setLookAndFeel (nullptr);
}

//==============================================================================
// Presets

void MainComponent::markPresetBaseline (const juce::String& name, bool factory, const juce::File& file, int scenario)
{
    currentScenario = scenario;
    currentPresetName = name;
    currentPresetFactory = factory;
    currentPresetFile = file;
    presetBaseline = settingsFingerprint (session.settings);
    updatePresetDisplay();
}

void MainComponent::updatePresetDisplay()
{
    presetDisplay.set (currentPresetName, settingsFingerprint (session.settings) != presetBaseline, currentPresetFactory);
    presetDelete.setEnabled (! currentPresetFactory && currentPresetFile.existsAsFile());
    diagnosis.startRap.setToggleState (currentPresetFactory && currentPresetName == genreName (Genre::rapTrap), juce::dontSendNotification);
    diagnosis.startPop.setToggleState (currentPresetFactory && currentPresetName == genreName (Genre::arabeskPop), juce::dontSendNotification);
    diagnosis.startElectronic.setToggleState (currentPresetFactory && currentPresetName == genreName (Genre::electronicLofi), juce::dontSendNotification);
}

void MainComponent::applyFactory (Genre g)
{
    session.applyGenre (g);                 // says that starting points are gone; changes nothing
    syncFromSettings();
}

bool MainComponent::loadUserPreset (const juce::File& f)
{
    MasterSettings loaded = session.settings;
    juce::String error;
    if (! loadPreset (f, loaded, error))
    {
        session.status = "Preset " + f.getFileName() + ": " + error;
        session.sendChangeMessage();
        return false;
    }
    session.applyPreset (loaded);            // target and ceiling become intent, the rest overrides over the plan; level stays the plan's
    juce::String category;
    int scenario = 0;
    readPresetCategory (f, category, scenario);
    markPresetBaseline (f.getFileNameWithoutExtension(), false, f, category == kCategoryScenarios ? juce::jmax (1, scenario) : 0);
    syncFromSettings();
    return true;
}

void MainComponent::showPresetMenu()
{
    const auto list = listPresets (presetsFolder());
    juce::PopupMenu m;
    // Starting points live on the DIAGNOSIS panel's START caps; the menu lists the scenarios and your presets.
    int user = 0, scen = 0;
    for (size_t i = 0; i < list.size(); ++i)
    {
        if (list[i].factory && list[i].scenario <= 0) continue;
        if (list[i].factory && list[i].scenario > 0 && scen++ == 0) { m.addSeparator(); m.addSectionHeader ("SCENARIOS (applied on top, amounts from the diagnosis)"); }
        if (! list[i].factory && user++ == 0) { m.addSeparator(); m.addSectionHeader ("USER PRESETS"); }
        const bool current = list[i].factory ? (currentPresetFactory && currentPresetName == list[i].name)
                                             : (! currentPresetFactory && list[i].file == currentPresetFile);
        m.addItem ((int) i + 1, list[i].name + (! list[i].factory && list[i].category == kCategoryScenarios ? "  (scenario)" : ""), true, current);
    }
    if (user == 0) { m.addSeparator(); m.addItem (-1, "No user presets yet (SAVE)", false, false); }
    m.addSeparator();
    m.addItem (1000, "Open preset file...");
    m.addItem (1001, "Reveal presets folder");
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&presetLoad),
                     [this, list] (int idx)
                     {
                         if (idx >= 1 && idx <= (int) list.size())
                         {
                             const auto& e = list[(size_t) idx - 1];
                             if (e.factory && e.scenario > 0) applyScenarioFromPanel (e.scenario);
                             else if (e.factory) applyFactory (e.genre);
                             else loadUserPreset (e.file);
                         }
                         else if (idx == 1000)
                         {
                             chooser = std::make_unique<juce::FileChooser> ("Open a MIHENK preset", presetsFolder(), juce::String ("*") + kPresetExtension);
                             chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                                   [this] (const juce::FileChooser& fc) { if (fc.getResult().existsAsFile()) loadUserPreset (fc.getResult()); });
                         }
                         else if (idx == 1001)
                         {
                             presetsFolder().createDirectory();
                             presetsFolder().revealToUser();
                         }
                     });
}

void MainComponent::savePresetAs (bool asScenario)
{
    const int sc = asScenario && ! session.appliedScenarios.isEmpty() ? session.appliedScenarios.getLast() : currentScenario;
    const juce::String suggestion = asScenario && sc > 0 ? juce::String (scenarioName (sc)) + " (mine)"
                                                         : (currentPresetFactory ? currentPresetName + " (mine)" : currentPresetName);
    auto* w = new juce::AlertWindow (asScenario ? "Save as my scenario" : "Save preset",
                                     "Name (saved in " + presetsFolder().getFullPathName() + (asScenario ? ", category Scenarios)" : ")"),
                                     juce::MessageBoxIconType::NoIcon, this);
    w->addTextEditor ("name", suggestion);
    w->addButton ("SAVE", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<MainComponent> safe (this);
    w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, w, asScenario] (int result)
    {
        const auto name = sanitisePresetName (w->getTextEditorContents ("name"));
        if (safe == nullptr || result != 1 || name.isEmpty()) return;
        const auto f = safe->presetsFolder().getChildFile (name + kPresetExtension);
        if (f.existsAsFile())
            juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Save preset", "Replace the preset \"" + name + "\"?", "REPLACE", "CANCEL", safe.getComponent(),
                                                juce::ModalCallbackFunction::create ([safe, name, asScenario] (int ok) { if (safe != nullptr && ok == 1) safe->writePreset (name, asScenario); }));
        else
            safe->writePreset (name, asScenario);
    }), true);
}

void MainComponent::writePreset (const juce::String& name, bool asScenario)
{
    presetsFolder().createDirectory();
    const auto f = presetsFolder().getChildFile (name + kPresetExtension);
    const int sc = asScenario && ! session.appliedScenarios.isEmpty() ? session.appliedScenarios.getLast() : currentScenario;
    juce::String error;
    if (savePreset (f, session.settings, error, sc > 0 ? juce::String (kCategoryScenarios) : juce::String(), sc))
    {
        session.status = "Preset saved: " + name + (sc > 0 ? " (Scenarios)" : "");
        markPresetBaseline (name, false, f, sc);
    }
    else
        session.status = "Preset not saved: " + error;
    session.sendChangeMessage();
}

void MainComponent::deleteCurrentPreset()
{
    if (currentPresetFactory || ! currentPresetFile.existsAsFile()) return;
    const auto f = currentPresetFile;
    juce::Component::SafePointer<MainComponent> safe (this);
    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::WarningIcon, "Delete preset",
                                        "Move the preset \"" + f.getFileNameWithoutExtension() + "\" to the Trash?", "DELETE", "CANCEL", this,
                                        juce::ModalCallbackFunction::create ([safe, f] (int ok)
                                        {
                                            if (safe == nullptr || ok != 1) return;
                                            if (f.moveToTrash())
                                            {
                                                safe->session.status = "Preset deleted: " + f.getFileNameWithoutExtension();
                                                safe->markPresetBaseline (safe->currentPresetName, false, {});
                                            }
                                            else
                                                safe->session.status = "Could not delete " + f.getFileName();
                                            safe->session.sendChangeMessage();
                                        }));
}

void MainComponent::applyScenarioFromPanel (int sc)
{
    session.applyScenario (sc);              // a finding is information; says what CHARACTER FULL does with it
    syncFromSettings();
    diagnosis.refresh();
}

//==============================================================================
// In-place value editing

juce::Rectangle<int> MainComponent::valueRect (const Knob& k) const
{
    const auto kb = k.slider.getBounds();
    const int w = kPitch[k.module] - 2;
    return { kb.getCentreX() - w / 2, kb.getBottom() + (k.size > 42 ? 19 : 16), w, 13 };
}

void MainComponent::beginValueEdit (Knob& k)
{
    finishValueEdit (true);
    if (! k.slider.isEnabled() || ! k.slider.isVisible()) return;
    editingKnob = &k;
    valueEditor = std::make_unique<juce::TextEditor>();
    auto& ed = *valueEditor;
    ed.setFont (Theme::mono (8.5f));
    ed.setJustification (juce::Justification::centred);
    ed.setBorder ({});
    ed.setIndents (1, 0);
    ed.setColour (juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
    ed.setColour (juce::TextEditor::textColourId, Theme::lamp.withAlpha (0.9f));
    ed.setColour (juce::TextEditor::outlineColourId, Theme::gold);
    ed.setColour (juce::TextEditor::focusedOutlineColourId, Theme::gold);
    ed.setColour (juce::TextEditor::highlightColourId, Theme::gold.withAlpha (0.35f));
    ed.setColour (juce::TextEditor::highlightedTextColourId, Theme::ink);
    ed.setColour (juce::CaretComponent::caretColourId, Theme::lamp);
    ed.setSelectAllWhenFocused (true);
    ed.setText (k.format (k.slider.getValue()), false);
    ed.setBounds (valueRect (k));
    ed.onReturnKey = [this] { finishValueEdit (true); };
    ed.onEscapeKey = [this] { finishValueEdit (false); };
    ed.onFocusLost = [this] { finishValueEdit (true); };
    addAndMakeVisible (ed);
    ed.grabKeyboardFocus();
    ed.selectAll();
    repaint (valueRect (k).expanded (4));
}

void MainComponent::finishValueEdit (bool apply)
{
    if (editingKnob == nullptr || valueEditor == nullptr) return;
    auto* k = editingKnob;
    editingKnob = nullptr;
    const auto text = valueEditor->getText();
    auto* ed = valueEditor.release();
    removeChildComponent (ed);
    juce::MessageManager::callAsync ([ed] { delete ed; });
    double v = 0.0;
    if (apply && parseKnobText (text, k->slider.getMinimum(), k->slider.getMaximum(), v))
        k->slider.setValue (v, juce::sendNotificationSync);
    repaint (valueRect (*k).expanded (4));
    grabKeyboardFocus();
}

void MainComponent::openValueEditorForSnapshot (const juce::String& knobLabel, const juce::String& typed)
{
    for (auto& k : knobs)
        if (k->label == knobLabel && k->slider.isVisible())
        {
            beginValueEdit (*k);
            if (valueEditor != nullptr) { valueEditor->setText (typed, false); valueEditor->moveCaretToEnd(); }
            return;
        }
}

MainComponent::Knob& MainComponent::addKnob (int module, int col, int row, const juce::String& label, double min, double max, double def, double mid,
                                             double interval, double scale, std::function<double& (MasterSettings&)> ref, std::function<juce::String (double)> format,
                                             std::function<juce::String()> paramId)
{
    auto k = std::make_unique<Knob>();
    k->label = label;
    k->scale = scale;
    k->ref = std::move (ref);
    k->format = std::move (format);
    k->paramId = std::move (paramId);
    k->module = module;
    k->col = col;
    k->row = row;
    auto& sl = k->slider;
    sl.setSliderStyle (juce::Slider::RotaryVerticalDrag);
    sl.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    sl.setRotaryParameters (kStart, kEnd, true);
    sl.setRange (min, max, interval);
    if (mid > min && mid < max) sl.setSkewFactorFromMidPoint (mid);
    sl.setDoubleClickReturnValue (true, def);   // the fallback for a knob with no parameter id
    // Double-click goes to the PARAMETER's default (the same Def the plug-in's APVTS is built from), converted to the knob's display scale; read at
    // the click, so a band-aware id follows the selected band. A stepped knob (AIR FREQ) rests in the middle of its zone.
    {
        auto* raw = k.get();
        sl.doubleClickValueFn = [raw, def]
        {
            if (raw->paramId == nullptr) return def;
            const auto* d = params::findDef (raw->paramId());
            if (d == nullptr) return def;
            const double v = d->defaultValue();
            return raw->steps.empty() ? v * raw->scale : (double) juce::jlimit (0, (int) raw->steps.size() - 1, juce::roundToInt (v)) + 0.5;
        };
    }
    sl.setPopupDisplayEnabled (false, false, nullptr);
    auto* raw = k.get();
    sl.textFromValueFunction = [raw] (double v) { return raw->format (v); };
    sl.valueFromTextFunction = [raw] (const juce::String& text)
    {
        double v = raw->slider.getValue();
        parseKnobText (text, raw->slider.getMinimum(), raw->slider.getMaximum(), v);
        return v;
    };
    sl.onValueChange = [this, raw]
    {
        if (syncing || ! raw->slider.isEnabled()) return;
        raw->animating = false;
        if (raw->module == mEq || raw->module == mComp || raw->module == mStereo || raw->module == mReference || raw->module == mCharacter) session.noteHandEdit();
        if (! raw->steps.empty())
        {
            // A stepped knob: the zone the pointer is in picks the step; the pointer then rests in the middle of that zone.
            const int i = raw->stepIndex (raw->slider.getValue());
            raw->slider.setValue ((double) i + 0.5, juce::dontSendNotification);
            if (std::abs (raw->ref (session.settings) - raw->steps[(size_t) i]) < 1.0e-9) { repaint (raw->slider.getBounds().expanded (50, 60)); return; }
            raw->ref (session.settings) = raw->steps[(size_t) i];
        }
        else
            raw->ref (session.settings) = raw->slider.getValue() / raw->scale;
        // Moving a locked knob overrides the lock (and the override is learned with the next note).
        const int pp = activeProfileParam (*raw);
        if (pp >= 0 && session.isLocked (pp)) session.unlock (pp);
        if (raw->after) raw->after();
        else session.settingsChanged();
        repaint (raw->slider.getBounds().expanded (50, 60));
    };
    // A knob drag is one automation gesture on its parameter.
    sl.onDragStart = [this, raw]
    {
        raw->animating = false;
        session.beginUndoGesture();                     // the whole drag is one undo step
        if (raw->paramId == nullptr) return;
        if (auto* prm = processor.getBridge().find (raw->paramId()))
        {
            prm->beginChangeGesture();
            gestureParam = prm;
            processor.dragGesture = true;
        }
    };
    sl.onDragEnd = [this]
    {
        if (gestureParam != nullptr) gestureParam->endChangeGesture();
        gestureParam = nullptr;
        processor.dragGesture = false;
        session.endUndoGesture();
    };
    addAndMakeVisible (sl);
    auto hit = std::make_unique<ValueHit>();
    hit->setMouseCursor (juce::MouseCursor::IBeamCursor);
    hit->clicked = [this, raw]
    {
        if (raw->steps.empty()) { beginValueEdit (*raw); return; }
        // A stepped knob has no typed value: a click on the value is "next step" (wrapping).
        if (! raw->slider.isEnabled() || ! raw->slider.isVisible()) return;
        const int next = (raw->stepIndex (raw->slider.getValue()) + 1) % (int) raw->steps.size();
        raw->slider.setValue ((double) next + 0.5, juce::sendNotificationSync);
    };
    addAndMakeVisible (*hit);
    k->valueHit = std::move (hit);
    knobs.push_back (std::move (k));
    return *knobs.back();
}

int MainComponent::activeProfileParam (const Knob& k) const
{
    if (k.profileParam < 0) return -1;
    if (k.profileParam == profile::pGlue && threeBand()) return -1;     // in 3-BAND the GLUE knob is the selected band's
    return k.profileParam;
}

void MainComponent::updateLocks()
{
    for (auto& k : knobs)
    {
        if (k->lockBadge == nullptr) continue;
        const int p = activeProfileParam (*k);
        const bool locked = p >= 0 && session.isLocked (p) && k->slider.isVisible();
        const auto tip = locked ? session.locks[(size_t) p].reason + ". Click the lock (or move the knob) to unlock; your change is learned too." : k->tip;
        k->lockBadge->setVisible (locked);
        k->lockBadge->setTooltip (tip);
        k->slider.setTooltip (tip);
    }
    noteKey.setToggleState (session.profileEnabled, juce::dontSendNotification);
}

void MainComponent::showProfilePanel (bool show)
{
    if (show)
    {
        batchPanel.setVisible (false);
        profilePanel.refresh();
        profilePanel.toFront (false);
    }
    profilePanel.setVisible (show);
    noteKey.setButtonText (show ? "CLOSE NOTE" : "NOTE");
}

void MainComponent::openProfileForSnapshot (const juce::StringArray& tagIds, const juce::String& text)
{
    syncFromSettings();
    showProfilePanel (true);
    profilePanel.prefillForSnapshot (tagIds, text);
}

void MainComponent::showTooltipForSnapshot (const juce::String& knobLabel)
{
    struct TipPreview : juce::Component
    {
        juce::String text;
        void paint (juce::Graphics& g) override { getLookAndFeel().drawTooltip (g, text, getWidth(), getHeight()); }
    };
    for (auto& k : knobs)
        if (k->label == knobLabel && k->slider.isVisible())
        {
            auto tip = std::make_unique<TipPreview>();
            tip->text = k->slider.getTooltip();
            if (tip->text.isEmpty()) return;
            const auto kb = k->slider.getBounds();
            tip->setBounds (lnf.getTooltipBounds (tip->text, { kb.getCentreX(), kb.getBottom() + 30 }, getLocalBounds()));
            addAndMakeVisible (*tip);
            tipPreview = std::move (tip);
            return;
        }
}

void MainComponent::selectBand (int b)
{
    ctx.selectedBand = juce::jlimit (0, 2, b);
    syncFromSettings();
    crossover.repaint();
    grReadout.repaint();
}

double MainComponent::displayValue (const Knob& k) const
{
    auto& st = session.settings;
    if (! k.steps.empty())
    {
        // The pointer rests in the middle of the zone of the step nearest to the stored value.
        const double v = k.ref (const_cast<MasterSettings&> (st));
        size_t best = 0;
        for (size_t i = 1; i < k.steps.size(); ++i) if (std::abs (k.steps[i] - v) < std::abs (k.steps[best] - v)) best = i;
        return (double) best + 0.5;
    }
    return k.display ? k.display (st) * k.scale : k.ref (st) * k.scale;
}

bool MainComponent::knobShown (const Knob& k) const
{
    const bool three = threeBand();
    if (k.face == 1 && ! advanced) return false;
    if (k.face == 2 && advanced) return false;
    if (k.needsReference && session.reference == nullptr) return false;
    return k.showIn == 0 || (k.showIn == 1 && ! three) || (k.showIn == 2 && three);
}

bool MainComponent::isAuto (const Knob& k) const
{
    const auto& st = session.settings;
    switch (k.autoKind)
    {
        case Knob::autoThreshold: return threeBand() ? st.comp.bands[juce::jlimit (0, 2, ctx.selectedBand)].autoThreshold : st.comp.autoThreshold;
        case Knob::autoRelease:   return st.comp.autoRelease;
        case Knob::autoTrim:
        case Knob::autoDrive:     return st.planned && session.measurements.present;
        default:                  return false;
    }
}

void MainComponent::setAuto (Knob& k, bool on)
{
    auto& st = session.settings;
    switch (k.autoKind)
    {
        case Knob::autoThreshold: if (threeBand()) band().autoThreshold = on; else st.comp.autoThreshold = on; if (on) st.comp.enabled = true; break;
        case Knob::autoRelease:   st.comp.autoRelease = on; break;
        case Knob::autoTrim:
        case Knob::autoDrive:
            // The level is the plan's (SPEC_V3): this lamp only shows it. Turning the knob puts an override on top, which RESET OVERRIDES drops.
            session.status = "The level is planned from the measurements and the target (" + fmt (session.plan.totalGainDb, 1) + " dB now). Pick the TARGET; turn the knob to override the plan.";
            session.sendChangeMessage();
            return;
        default: return;
    }
    session.status = juce::String (k.label) + (on ? ": AUTO, MIHENK solves it from your mix" : ": set by hand (AUTO off)");
    session.settingsChanged();
    syncFromSettings();
}

void MainComponent::updateAutoLamps()
{
    for (auto& k : knobs)
    {
        if (k->autoLamp == nullptr) continue;
        const bool vis = k->slider.isVisible();
        if (k->autoLamp->isVisible() != vis) k->autoLamp->setVisible (vis);
        k->autoLamp->setOn (isAuto (*k));
    }
}

void MainComponent::syncFromSettings()
{
    syncing = true;
    auto& s = session.settings;
    updateModeVisibility();
    for (auto& k : knobs)
    {
        const double target = juce::jlimit (k->slider.getMinimum(), k->slider.getMaximum(), displayValue (*k));
        const double now = k->slider.getValue();
        const bool held = editingKnob == k.get() || (gestureParam != nullptr && k->paramId != nullptr && processor.getBridge().find (k->paramId()) == gestureParam);
        if (std::abs (target - now) <= 1.0e-9) { k->animating = false; continue; }
        // A value MIHENK changes (a solved AUTO value, a preset, a scenario) glides to its place so the move is seen.
        if (animateKnobs && ! held && k->slider.isVisible())
        {
            k->animTarget = target;
            k->animating = true;
        }
        else
        {
            k->animating = false;
            k->slider.setValue (target, juce::dontSendNotification);
        }
    }
    const bool three = threeBand();
    eqIn.setToggleState (s.eq.enabled, juce::dontSendNotification);
    eqLowCut.setToggleState (s.eq.lowCutOn, juce::dontSendNotification);
    eqSteep.setToggleState (s.eq.lowCutSlope >= 24, juce::dontSendNotification);
    eqLinear.setToggleState (s.eq.linearPhase, juce::dontSendNotification);
    eqAirDyn.setToggleState (s.eq.airDynamic, juce::dontSendNotification);
    dynLowMid.setToggleState (s.eq.dynLowMid.enabled, juce::dontSendNotification);
    dynUpperMid.setToggleState (s.eq.dynUpperMid.enabled, juce::dontSendNotification);
    dynTight.setToggleState (s.eq.tightener.enabled, juce::dontSendNotification);
    dcFilter.setToggleState (s.dcFilter, juce::dontSendNotification);
    compIn.setToggleState (s.comp.enabled, juce::dontSendNotification);
    compThreeBand.setToggleState (three, juce::dontSendNotification);
    bandLow.setToggleState (ctx.selectedBand == 0, juce::dontSendNotification);
    bandMid.setToggleState (ctx.selectedBand == 1, juce::dontSendNotification);
    bandHigh.setToggleState (ctx.selectedBand == 2, juce::dontSendNotification);
    bandSolo.setToggleState (band().solo, juce::dontSendNotification);
    bandBypass.setToggleState (band().bypass, juce::dontSendNotification);
    widthIn.setToggleState (s.width.enabled, juce::dontSendNotification);
    phaseSafe.setToggleState (s.width.phaseSafeLowMono, juce::dontSendNotification);
    limIn.setToggleState (s.limiter.enabled, juce::dontSendNotification);
    bypassKey.setToggleState (s.bypass, juce::dontSendNotification);
    presetStream.setToggleState (s.targetPreset == TargetPreset::streaming, juce::dontSendNotification);
    presetApple.setToggleState (s.targetPreset == TargetPreset::apple, juce::dontSendNotification);
    presetClub.setToggleState (s.targetPreset == TargetPreset::club, juce::dontSendNotification);
    presetKeep.setToggleState (s.targetPreset == TargetPreset::keepInput, juce::dontSendNotification);
    presetCustom.setToggleState (s.targetPreset == TargetPreset::custom, juce::dontSendNotification);
    bitsCap.setButtonText ("EXPORT " + juce::String (exportBits) + "-BIT");
    abMatchCap.setToggleState (session.abLevelMatched(), juce::dontSendNotification);
    faceSimple.setToggleState (! advanced, juce::dontSendNotification);
    faceAdvanced.setToggleState (advanced, juce::dontSendNotification);
    resetAllCap.setVisible (advanced);
    resetOverridesCap.setVisible (advanced);
    resetOverridesCap.setEnabled (session.overrideCount() > 0);
    charOff.setToggleState (session.intent.character == PlanCharacter::OFF, juce::dontSendNotification);
    charGentle.setToggleState (session.intent.character == PlanCharacter::GENTLE, juce::dontSendNotification);
    charFull.setToggleState (session.intent.character == PlanCharacter::FULL, juce::dontSendNotification);
    const auto m = scope.getMode();
    viewWave.setToggleState (m == ui::CrtScope::Mode::wave, juce::dontSendNotification);
    viewSpec.setToggleState (m == ui::CrtScope::Mode::spectrum, juce::dontSendNotification);
    viewLoud.setToggleState (m == ui::CrtScope::Mode::loudness, juce::dontSendNotification);
    layerEq.setEnabled (m == ui::CrtScope::Mode::spectrum);
    layerComp.setEnabled (m == ui::CrtScope::Mode::spectrum);
    syncing = false;
    updateLocks();
    updateAutoLamps();
    updateDimming();
    updatePresetDisplay();
    grReadout.repaint();
    crossover.repaint();
    loudnessVfd.repaint();
    referencePanel.repaint();
    chainReadout.repaint();
    repaint();
}

void MainComponent::updateModeVisibility()
{
    const bool three = threeBand(), hasRef = session.reference != nullptr, app = processor.isStandaloneApp();
    juce::String key = (advanced ? "A" : "S") + juce::String (three ? "3" : "W") + (hasRef ? "R" : "-");
    for (auto& k : knobs)
    {
        const bool vis = knobShown (*k);
        if (k->slider.isVisible() != vis) { k->slider.setVisible (vis); k->valueHit->setVisible (vis); }
    }
    // ADVANCED: the modules of the chain. SIMPLE: the character macros, the delivery and the reference.
    for (auto* c : std::initializer_list<juce::Component*> { &eqIn, &eqLowCut, &eqSteep, &eqLinear, &eqAirDyn, &dynLowMid, &dynUpperMid, &dynTight, &dcFilter,
                                                             &compIn, &compThreeBand, &widthIn, &phaseSafe, &limIn, &grReadout, &layerIn, &layerEq, &layerComp, &layerOut })
        c->setVisible (advanced);
    for (auto* c : { &bandLow, &bandMid, &bandHigh, &bandSolo, &bandBypass }) c->setVisible (advanced && three);
    crossover.setVisible (advanced && three);
    grTrace.setVisible (advanced && ! three);
    chainReadout.setVisible (! advanced);
    matchRefKey.setVisible (hasRef);
    clearRef.setVisible (hasRef);
    matchRefKey.setEnabled (hasRef && session.input != nullptr);
    juce::ignoreUnused (app);
    if (key != visibilityKey)
    {
        visibilityKey = key;
        panelTexture = {};
        resized();
    }
}

void MainComponent::updateDimming()
{
    const auto& s = session.settings;
    const bool three = threeBand();
    for (auto& k : knobs)
    {
        bool dim = s.bypass;
        if (k->module == mEq) dim |= ! s.eq.enabled;
        if (k->module == mComp) dim |= ! s.comp.enabled;
        if (k->module == mStereo) dim |= ! s.width.enabled;
        if (k->module == mLimiter && k->label != "DRIVE") dim |= ! s.limiter.enabled;
        if (k->label == "LOW CUT") dim |= ! s.eq.lowCutOn;
        if (k->label == "TONE AMOUNT" || k->label == "SMOOTH") dim |= s.eq.matchCurveDb.empty();
        if (k->label.startsWith ("LO-MID")) dim |= ! s.eq.dynLowMid.enabled;
        if (k->label.startsWith ("HI-MID")) dim |= ! s.eq.dynUpperMid.enabled;
        if (k->label.startsWith ("TIGHT")) dim |= ! s.eq.tightener.enabled;
        if (k->label == "DE-HARSH") dim |= ! s.eq.airDynamic;
        // A frequency / Q knob of a band with no gain moves nothing: dimmed until the band's gain is set.
        // A finished master (peaks within 11 dB of the loudness): the plan refuses every EQ / compressor / stereo override ("Finished master: ... not applied").
        if (session.plan.finishedMaster && (k->module == mEq || k->module == mComp || k->module == mStereo || k->module == mReference)) dim = true;
        if (k->module == mEq && k->label == "SHELF FREQ") dim |= std::abs (k->row == 0 ? s.eq.lowShelfDb : s.eq.highShelfDb) < 0.05;
        if (k->module == mEq && (k->label == "BELL 1 FREQ" || k->label == "BELL 1 Q")) dim |= std::abs (s.eq.bell1Db) < 0.05;
        if (k->module == mEq && (k->label == "BELL 2 FREQ" || k->label == "BELL 2 Q")) dim |= std::abs (s.eq.bell2Db) < 0.05;
        if (k->label == "AIR FREQ") dim |= s.eq.airDb < kAirNeededDb;       // the AIR shelf has no gain to place: this knob would do nothing
        if (k->label == "GLUE" && k->face != 2) dim |= three ? ! s.comp.bands[ctx.selectedBand].autoThreshold : ! s.comp.autoThreshold;
        if (three && k->module == mComp && k->row == 0) dim |= s.comp.bands[ctx.selectedBand].bypass;
        if (k->label == "HIGH FROM") dim |= std::abs (s.width.highWidth - 1.0) < 1.0e-9;
        if ((bool) k->slider.getProperties()["dim"] != dim)
        {
            k->slider.getProperties().set ("dim", dim);
            k->slider.repaint();
        }
    }
}

void MainComponent::setScopeMode (ui::CrtScope::Mode m)
{
    scope.setMode (m);
    syncFromSettings();
}

void MainComponent::setForceLive (bool on)
{
    scope.forceLive = on;
    freezeLive = on;
    scope.repaint();
}

void MainComponent::settleForSnapshot()
{
    for (int i = 0; i < 40; ++i) timerCallback();
    diagnosis.refresh();
}

//==============================================================================
void MainComponent::buildPanelTexture()
{
    panelTexture = juce::Image (juce::Image::ARGB, kW, kH, true);
    juce::Graphics g (panelTexture);
    const float W = (float) kW, H = (float) kH;

    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff141a19), 0.0f, 0.0f, juce::Colour (0xff070a09), 0.0f, H, false));
    g.fillAll();
    const auto body = juce::Rectangle<float> (4.0f, 4.0f, W - 8.0f, H - 8.0f);
    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.fillRoundedRectangle (body.translated (0.0f, 4.0f), 5.0f);
    g.setGradientFill (juce::ColourGradient (Theme::panel.brighter (0.16f), 0.0f, body.getY(), Theme::panelDeep, 0.0f, body.getBottom(), false));
    g.fillRoundedRectangle (body, 5.0f);
    juce::Random rng (1986);
    g.saveState();
    g.reduceClipRegion (body.toNearestInt());
    for (int i = 0; i < 3000; ++i)
    {
        const float x = body.getX() + rng.nextFloat() * body.getWidth(), len = 60.0f + rng.nextFloat() * 380.0f;
        const int y = (int) body.getY() + rng.nextInt ((int) body.getHeight());
        g.setColour ((rng.nextBool() ? juce::Colours::white : juce::Colours::black).withAlpha (0.018f + rng.nextFloat() * 0.022f));
        g.drawHorizontalLine (y, x, x + len);
    }
    g.restoreState();
    g.setColour (juce::Colours::white.withAlpha (0.14f));
    g.drawRoundedRectangle (body.reduced (0.5f), 5.0f, 1.0f);

    for (float ex : { body.getX(), body.getRight() - 24.0f })
    {
        const auto ear = juce::Rectangle<float> (ex, body.getY(), 24.0f, body.getHeight());
        g.setColour (juce::Colours::black.withAlpha (0.22f));
        g.fillRect (ear);
        g.setColour (juce::Colours::white.withAlpha (0.10f));
        g.drawVerticalLine ((int) (ex < W * 0.5f ? ear.getRight() : ear.getX()), ear.getY() + 2.0f, ear.getBottom() - 2.0f);
        for (float hy : { 46.0f, H - 72.0f })
        {
            const auto slot = juce::Rectangle<float> (ear.getCentreX() - 5.0f, hy, 10.0f, 26.0f);
            g.setColour (juce::Colour (0xff050807));
            g.fillRoundedRectangle (slot, 5.0f);
            g.setColour (juce::Colours::white.withAlpha (0.12f));
            g.drawLine (slot.getX() + 5.0f, slot.getBottom() + 0.5f, slot.getRight() - 5.0f, slot.getBottom() + 0.5f, 1.0f);
        }
    }

    g.setColour (Theme::ink.withAlpha (0.22f));
    g.drawHorizontalLine (102, 40.0f, (float) kContentRight);
    g.drawHorizontalLine (478, 40.0f, (float) kContentRight);

    const float nh = 30.0f;
    const float nw = ui::productNameWidth ("MIHENK", nh);
    ui::drawProductName (g, "MIHENK", 52.0f, 24.0f, nh, Theme::ink);
    const juce::Rectangle<float> plate (52.0f, 66.0f, juce::jmax (nw, 290.0f), 18.0f);
    g.setColour (Theme::gold);
    g.drawRect (plate, 1.0f);
    g.setColour (Theme::gold);
    ui::tracked (g, "DIGITAL MASTERING PROCESSOR   MODEL MH-86", Theme::legend (8.0f), plate, juce::Justification::centred, 0.18f);
    g.setColour (Theme::ink.withAlpha (0.6f));
    ui::tracked (g, "MIHENK TASI: THE TOUCHSTONE THAT TELLS GOLD", Theme::fine (7.5f), plate.translated (0.0f, 20.0f).withHeight (11.0f), juce::Justification::centredLeft, 0.16f);

    const std::vector<int> boxes = advanced ? std::vector<int> { mInput, mEq, mComp, mStereo, mLimiter, mReference } : std::vector<int> { mCharacter, mDelivery, mReference };
    for (size_t bi = 0; bi < boxes.size(); ++bi)
    {
        const int m = boxes[bi];
        const auto& b = kBoxes[m];
        g.setColour (Theme::ink.withAlpha (0.32f));
        g.drawRoundedRectangle (b, 5.0f, 1.0f);
        g.setColour (Theme::ink);
        ui::tracked (g, kTitles[m], Theme::legend (8.5f), { b.getX() + 9.0f, b.getY() + 6.0f, m == mStereo ? b.getWidth() - 50.0f : m == mReference ? 70.0f : b.getWidth() * 0.5f, 14.0f },
                     juce::Justification::centredLeft, 0.16f);
        if (m == mCharacter)
        {
            g.setColour (Theme::ink.withAlpha (0.45f));
            ui::tracked (g, "WHAT THE PLAN DOES (READ-ONLY); OVERRIDE IT ON THE ADVANCED FACE", Theme::fine (7.0f), { b.getX() + 110.0f, b.getY() + 6.0f, b.getWidth() - 120.0f, 14.0f },
                         juce::Justification::centredRight, 0.1f);
        }
        if (bi + 1 < boxes.size())
        {
            const float ax = b.getRight() + 5.0f, ay = advanced ? (float) kRowY[1] : (float) kSimpleRowY;
            juce::Path arrow;
            arrow.addTriangle (ax - 3.0f, ay - 5.0f, ax - 3.0f, ay + 5.0f, ax + 3.0f, ay);
            g.setColour (Theme::gold.withAlpha (0.8f));
            g.fillPath (arrow);
        }
    }

    g.setColour (Theme::ink.withAlpha (0.8f));
    ui::tracked (g, "VIEW", Theme::legend (7.5f), { 40.0f, 406.0f, 32.0f, 20.0f }, juce::Justification::centredLeft, 0.16f);
    if (advanced) ui::tracked (g, "LAYERS", Theme::legend (7.5f), { 336.0f, 406.0f, 48.0f, 20.0f }, juce::Justification::centredLeft, 0.16f);
    ui::tracked (g, "PRESET", Theme::legend (7.5f), { 40.0f, 439.0f, 48.0f, 20.0f }, juce::Justification::centredLeft, 0.16f);
    ui::tracked (g, "TARGET", Theme::legend (7.5f), { 702.0f, 354.0f, 100.0f, 11.0f }, juce::Justification::centredLeft, 0.16f);
    g.setColour (Theme::ink.withAlpha (0.45f));
    ui::tracked (g, "PICK ONE: MIHENK PLANS THE LEVEL", Theme::fine (7.0f), { 800.0f, 354.0f, 190.0f, 11.0f }, juce::Justification::centredRight, 0.1f);

    for (auto& k : knobs)
    {
        if (! knobShown (*k)) continue;
        const auto c = k->slider.getBounds().toFloat().getCentre();
        const float r = (float) k->slider.getWidth() * 0.5f;
        g.setColour (Theme::ink.withAlpha (0.75f));
        for (int i = 0; i <= 10; ++i)
        {
            const float a = kStart + (kEnd - kStart) * (float) i / 10.0f;
            g.drawLine ({ ui::polar (c, r + 1.5f, a), ui::polar (c, r + (i % 5 == 0 ? 6.0f : 4.0f), a) }, i % 5 == 0 ? 1.3f : 0.9f);
        }
    }

    g.setColour (Theme::ink.withAlpha (0.8f));
    ui::tracked (g, "CHARACTER", Theme::legend (7.5f), { 44.0f, H - 38.0f, 66.0f, 20.0f }, juce::Justification::centredLeft, 0.16f);
    g.setColour (Theme::ink.withAlpha (0.8f));
    ui::tracked (g, "FACE", Theme::legend (7.5f), { 296.0f, H - 38.0f, 36.0f, 20.0f }, juce::Justification::centredLeft, 0.16f);
    g.setColour (Theme::ink.withAlpha (0.55f));
    ui::tracked (g, "ITU-R BS.1770-4   EBU TECH 3341 / 3342   TRUE PEAK 4X METER, 8X LIMITER", Theme::fine (7.5f),
                 { 540.0f, H - 34.0f, 520.0f, 12.0f }, juce::Justification::centred, 0.16f);
    const float badgeRight = W - 44.0f;
    g.setOpacity (0.85f);
    g.drawImage (wordmark, juce::Rectangle<float> (badgeRight - 104.0f, H - 35.0f, 104.0f, 13.0f), juce::RectanglePlacement::xRight | juce::RectanglePlacement::yMid);
    g.drawImage (mark, juce::Rectangle<float> (badgeRight - 104.0f - 32.0f, H - 40.0f, 26.0f, 20.0f), juce::RectanglePlacement::centred);
    g.setOpacity (1.0f);
}

void MainComponent::paintKnobLegends (juce::Graphics& g)
{
    for (auto& k : knobs)
    {
        if (! k->slider.isVisible()) continue;
        const auto kb = k->slider.getBounds().toFloat();
        const bool dim = (bool) k->slider.getProperties()["dim"];
        const bool big = k->size > 42;
        const float w = (float) kPitch[k->module] - 2.0f;
        const float y0 = kb.getBottom() + (big ? 7.0f : 5.0f);
        g.setColour (Theme::ink.withAlpha (dim ? 0.45f : 1.0f));
        ui::tracked (g, k->label, Theme::legend (big ? 8.5f : 7.5f), { kb.getCentreX() - w * 0.5f, y0, w, 11.0f }, juce::Justification::centred, 0.1f);
        if (k->hint.isNotEmpty())
        {
            g.setColour (Theme::ink.withAlpha (0.45f));
            ui::tracked (g, k->hint, Theme::fine (7.0f), { kb.getCentreX() - w * 0.5f, y0 + 26.0f, w, 10.0f }, juce::Justification::centred, 0.08f);
        }
        // An override: a gold ring and "+1.2 OVER PLAN" (the plan's own value is what the knob would show without it).
        if (k->paramId != nullptr)
        {
            const auto pid = k->paramId();
            if (pid.isNotEmpty() && session.isOverridden (pid))
            {
                g.setColour (Theme::gold.withAlpha (dim ? 0.4f : 0.95f));
                g.drawEllipse (kb.expanded (4.0f), 1.8f);
                const double d = session.overrideDelta (pid) * k->scale;
                if (std::abs (d) > 1.0e-6)
                {
                    g.setColour (Theme::gold.withAlpha (dim ? 0.4f : 0.9f));
                    ui::tracked (g, (d >= 0.0 ? "+" : "") + juce::String (d, std::abs (d) >= 100.0 ? 0 : 1) + " OVER PLAN", Theme::fine (6.5f), { kb.getCentreX() - w * 0.5f - 6.0f, y0 + 23.0f, w + 12.0f, 9.0f }, juce::Justification::centred, 0.0f);
                }
            }
        }
        if (k.get() == editingKnob) continue;
        const int pp = activeProfileParam (*k);
        const bool locked = pp >= 0 && session.isLocked (pp);
        const bool autoOn = k->autoKind != Knob::autoNone && isAuto (*k);
        g.setColour ((dim ? Theme::ink : locked ? Theme::gold.brighter (0.25f) : autoOn ? Theme::lamp.brighter (0.15f) : Theme::lamp).withAlpha (dim ? 0.35f : 0.95f));
        if (k->label == "AIR FREQ" && session.settings.eq.airDb < kAirNeededDb)
        {
            // Dimmed: the step stays readable, with the reason (a hint line would run into the AIR DYN key below).
            const auto step = k->format (k->slider.getValue()).upToFirstOccurrenceOf (" ", false, false);
            ui::tracked (g, step.replace ("KHZ", "") + "K SET AIR", Theme::mono (7.0f), { kb.getCentreX() - w * 0.5f - 6.0f, y0 + 12.0f, w + 12.0f, 11.0f }, juce::Justification::centred, 0.0f);
            continue;
        }
        ui::tracked (g, k->format (k->slider.getValue()), Theme::mono (big ? 9.5f : 8.5f), { kb.getCentreX() - w * 0.5f, y0 + 12.0f, w, 11.0f }, juce::Justification::centred, 0.0f);
    }
    if (! advanced) return;
    // 3-BAND: the band row legend.
    if (threeBand())
    {
        g.setColour (Theme::ink.withAlpha (0.8f));
        ui::tracked (g, "BAND", Theme::legend (7.5f), { kBoxes[mComp].getX() + 9.0f, (float) kGapA, 36.0f, (float) kCapH }, juce::Justification::centredLeft, 0.14f);
    }
    else
    {
        g.setColour (Theme::ink.withAlpha (0.4f));
        ui::tracked (g, "WIDE: ONE BROADBAND GLUE, SIDECHAIN HIGH-PASS", Theme::fine (7.0f), { kBoxes[mComp].getX() + 9.0f, (float) kGapA, kBoxes[mComp].getWidth() - 18.0f, (float) kCapH },
                     juce::Justification::centredLeft, 0.1f);
    }
}

void MainComponent::paint (juce::Graphics& g)
{
    if (panelTexture.isNull())
        buildPanelTexture();
    g.drawImageAt (panelTexture, 0, 0);
    paintKnobLegends (g);
}

juce::Point<int> MainComponent::knobCentre (const Knob& k) const
{
    if (k.cx >= 0) return { k.cx, k.cy };
    return { colCentre (k.module, k.col), kRowY[k.row] };
}

void MainComponent::resized()
{
    const bool app = processor.isStandaloneApp();
    info.setBounds (360, 18, 476, 80);
    if (app)
    {
        juce::Button* keys[] { &openKey, &refKey, &playKey, &stopKey, &loopKey, &abKey, &exportKey, &batchKey, &audioKey };
        for (int i = 0; i < 9; ++i) keys[i]->setBounds (848 + i * 49, 20, 46, 72);
    }
    else
    {
        learnKey.setBounds (848, 20, 107, 72);
        abKey.setBounds (958, 20, 52, 72);
        refKey.setBounds (1013, 20, 52, 72);
        learnDisplay.setBounds (1070, 20, 220, 72);
    }

    scope.setBounds (40, 108, 650, 290);
    viewWave.setBounds (74, 404, 80, 24);
    viewSpec.setBounds (158, 404, 80, 24);
    viewLoud.setBounds (242, 404, 80, 24);
    layerIn.setBounds (388, 404, 48, 24);
    layerEq.setBounds (440, 404, 48, 24);
    layerComp.setBounds (492, 404, 60, 24);
    layerOut.setBounds (556, 404, 52, 24);
    noteKey.setBounds (614, 404, 76, 24);
    profilePanel.setBounds (40, 108, kContentRight - 40, 364);
    batchPanel.setBounds (40, 108, kContentRight - 40, 364);
    presetDisplay.setBounds (90, 436, 282, 26);
    presetSave.setBounds (378, 437, 60, 24);
    presetLoad.setBounds (442, 437, 60, 24);
    presetDelete.setBounds (506, 437, 66, 24);
    if (app) bitsCap.setBounds (580, 437, 110, 24);
    else latencyVfd.setBounds (580, 435, 110, 28);

    table.setBounds (702, 108, 288, 244);
    // TARGET block: the targets, the TARGET knob, what it does to the mix, the lamps and the A/B matching.
    {
        int x = 702;
        const std::pair<juce::Component*, int> caps[] { { &presetStream, 63 }, { &presetApple, 57 }, { &presetClub, 49 }, { &presetKeep, 66 }, { &presetCustom, 49 } };
        for (auto& [c, w] : caps) { c->setBounds (x, 367, w, 22); x += w + 1; }
    }
    loudnessVfd.setBounds (758, 394, 232, 40);
    renderLamp.setBounds (758, 438, 44, 34);
    targetLamp.setBounds (802, 438, 54, 34);
    limitLamp.setBounds (856, 438, 50, 34);
    abMatchCap.setBounds (910, 443, 80, 22);

    diagnosis.setBounds (1002, 108, 288, 362);
    meters.setBounds (1300, 18, 146, 820);

    for (auto& k : knobs)
    {
        const auto c = knobCentre (*k);
        k->slider.setBounds (c.x - k->size / 2, c.y - k->size / 2, k->size, k->size);
        k->valueHit->setBounds (valueRect (*k));
        if (k->lockBadge != nullptr) k->lockBadge->setBounds (c.x + k->size / 2 - 6, c.y - k->size / 2 - 9, 12, 15);
        if (k->autoLamp != nullptr) k->autoLamp->setBounds (c.x + k->size / 2 - 5, c.y - k->size / 2 - 8, 14, 14);
    }
    faceSimple.setBounds (330, kH - 40, 66, 22);
    faceAdvanced.setBounds (400, kH - 40, 84, 22);
    resetAllCap.setBounds (494, kH - 40, 84, 22);
    resetOverridesCap.setBounds (582, kH - 40, 118, 22);
    charOff.setBounds (112, kH - 40, 44, 22);
    charGentle.setBounds (159, kH - 40, 64, 22);
    charFull.setBounds (226, kH - 40, 44, 22);

    // The reference module (both faces): a hint until a reference is loaded, then the graph and MATCH TO REFERENCE.
    const auto& rb = kBoxes[mReference];
    clearRef.setBounds ((int) rb.getRight() - 52, kModuleTop + 4, 46, kCapH);
    matchRefKey.setBounds ((int) rb.getX() + 8, 748, (int) rb.getWidth() - 16, 64);
    const bool hasRef = session.reference != nullptr;
    if (advanced && hasRef) referencePanel.setBounds ((int) rb.getX() + 6, 616, (int) rb.getWidth() - 12, 126);
    else referencePanel.setBounds ((int) rb.getX() + 6, 510, (int) rb.getWidth() - 12, hasRef ? 230 : 228);

    if (advanced)
    {
        // Compressor: the GR readout in row 1, column 4; row 2 holds the crossover display (3-BAND) or the GR trace (WIDE).
        const int cx = colCentre (mComp, 4);
        grReadout.setBounds (cx - 29, kRowY[1] - kKnob / 2, 58, ui::GrReadout::glassHeight + 30);
        const int left = colCentre (mComp, 2) - 29, right = (int) kBoxes[mComp].getRight() - 8;
        crossover.setBounds (left, 746, right - left, 86);
        grTrace.setBounds ((int) kBoxes[mComp].getX() + 8, 746, right - (int) kBoxes[mComp].getX() - 8, 86);
        correlation.setBounds ((int) kBoxes[mStereo].getX() + 8, 746, (int) kBoxes[mStereo].getWidth() - 16, 86);
        // Limiter: WARN AT in the last row, the readout beside it.
        limiterReadout.setBounds (colCentre (mLimiter, 1) - 30, 736, 62, 96);
        bypassKey.setBounds ((int) kBoxes[mInput].getCentreX() - 30, 748, 60, 64);
    }
    else
    {
        const auto& d = kBoxes[mDelivery];
        bypassKey.setBounds ((int) d.getRight() - 72, kSimpleRowY - 32, 60, 64);
        limiterReadout.setBounds ((int) d.getX() + 12, 656, (int) d.getWidth() - 24, 80);
        correlation.setBounds ((int) d.getX() + 12, 744, (int) d.getWidth() - 24, 86);
        chainReadout.setBounds ((int) kBoxes[mCharacter].getX() + 12, 664, (int) kBoxes[mCharacter].getWidth() - 24, 166);
    }

    // Module switches, right-aligned in each module's header.
    auto header = [] (int module, std::initializer_list<std::pair<juce::Component*, int>> items)
    {
        int x = (int) kBoxes[module].getRight() - 6;
        for (auto it = std::rbegin (items); it != std::rend (items); ++it)
        {
            x -= it->second;
            it->first->setBounds (x, kModuleTop + 4, it->second, kCapH);
            x -= 3;
        }
    };
    header (mEq, { { &eqIn, 36 }, { &eqLowCut, 66 }, { &eqSteep, 52 }, { &eqLinear, 60 } });
    header (mComp, { { &compThreeBand, 64 }, { &compIn, 34 } });
    header (mStereo, { { &widthIn, 34 } });
    header (mLimiter, { { &limIn, 34 } });
    // Row caps.
    dcFilter.setBounds ((int) kBoxes[mInput].getX() + 6, kGapC, (int) kBoxes[mInput].getWidth() - 12, kCapH);
    dynLowMid.setBounds (colCentre (mEq, 0) - 25, kGapC, 102, kCapH);
    dynUpperMid.setBounds (colCentre (mEq, 2) - 25, kGapC, 102, kCapH);
    dynTight.setBounds (colCentre (mEq, 4) - 25, kGapC, 102, kCapH);
    eqAirDyn.setBounds (colCentre (mEq, 4) - 25 + 106, kGapC, (int) kBoxes[mEq].getRight() - 6 - (colCentre (mEq, 4) - 25 + 106), kCapH);
    phaseSafe.setBounds (colCentre (mStereo, 1) - 36, kGapB, 70, kCapH);
    {
        int x = (int) kBoxes[mComp].getX() + 44;
        for (auto* c : { &bandLow, &bandMid, &bandHigh }) { c->setBounds (x, kGapA, 46, kCapH); x += 48; }
        x += 6;
        bandSolo.setBounds (x, kGapA, 50, kCapH);
        bandBypass.setBounds (x + 53, kGapA, (int) kBoxes[mComp].getRight() - 6 - (x + 53), kCapH);
    }
    panelTexture = {};
}

//==============================================================================
void MainComponent::timerCallback()
{
    const bool fresh = freezeLive ? false : live.update();
    const bool running = live.running() || scope.forceLive;
    const bool app = processor.isStandaloneApp();
    meters.repaint();
    if (running || fresh)
    {
        scope.repaint();
        if (threeBand()) crossover.repaint(); else grTrace.repaint();
        correlation.repaint();
    }
    auto& pl = session.player;
    if (app)
    {
        if (pl.isPlaying() && ! running) scope.repaint();
        playKey.setToggleState (pl.isPlaying(), juce::dontSendNotification);
        loopKey.setToggleState (pl.isLooping(), juce::dontSendNotification);
        exportKey.setEnabled (session.outputCurrent());
    }
    else
    {
        const auto st = processor.learnState();
        learnKey.setToggleState (st == MihenkProcessor::Learn::armed || st == MihenkProcessor::Learn::capturing, juce::dontSendNotification);
        learnKey.blink = st == MihenkProcessor::Learn::armed;
        learnKey.setButtonText (st == MihenkProcessor::Learn::capturing ? "LEARN\nSTOP TO SOLVE" : st == MihenkProcessor::Learn::armed ? "LEARN\nARMED"
                               : st == MihenkProcessor::Learn::analysing ? "LEARN\nANALYSING" : "LEARN");
        learnKey.repaint();
        learnDisplay.repaint();
        const int lat = processor.getLatencySamples();
        latencyVfd.set ("LATENCY", juce::String (1000.0 * lat / juce::jmax (1.0, processor.getSampleRate()), 1) + " MS");
    }
    const bool a = processor.isListeningOriginal();
    abKey.setToggleState (a, juce::dontSendNotification);
    const float aGain = app ? session.originalGainDb() : (float) gainToDb (processor.effectiveListenGain());
    abKey.setButtonText (a ? "A / B\nA " + (session.abLevelMatched() && std::abs (aGain) >= 0.05f ? (aGain > 0.0f ? "+" : "") + juce::String (aGain, 1) : juce::String ("ORIG"))
                           : "A / B\nB MSTR");
    if (app)
    {
        const bool running = batchPanel.isRunning();
        batchKey.setToggleState (running || batchPanel.isVisible(), juce::dontSendNotification);
        batchKey.blink = running;
        if (running) batchKey.repaint();
    }
    // Knobs glide to values MIHENK set (solved AUTO values, presets, scenarios): about a third of a second.
    for (auto& k : knobs)
    {
        if (! k->animating) continue;
        const double v = k->slider.getValue(), t = k->animTarget;
        const double step = juce::jmax (k->slider.getInterval(), 1.0e-6);
        double move = (t - v) * 0.3;
        if (std::abs (move) < step) move = t > v ? step : -step;     // at least one step, so the knob never stalls on its grid
        double next = v + move;
        if (std::abs (t - v) <= step * 1.01 || (t - next) * (t - v) <= 0.0) { next = t; k->animating = false; }
        k->slider.setValue (next, juce::dontSendNotification);
        repaint (k->slider.getBounds().expanded (50, 60));
    }
    const bool busy = session.busy() || processor.learnState() == MihenkProcessor::Learn::analysing;
    renderLamp.setOn (busy);
    const bool haveOut = session.outMeas.valid;
    targetLamp.setOn (! busy && haveOut && ! session.settings.bypass && std::isfinite (session.outMeas.integrated)
                      && std::abs (session.outMeas.integrated - session.settings.targetLufs) <= 0.2);
    limitLamp.setOn (! session.settings.bypass && session.settings.limiter.enabled && session.limGr.maxDb > session.settings.limitWarnDb);
    updatePresetDisplay();
    // The diagnosis list follows the findings and the applied chips.
    int h = (int) session.appliedScenarios.size() * 131 + (int) session.diagnosis.findings.size() * 7 + (session.diagnosis.m.valid ? 1 : 0);
    for (auto& f : session.diagnosis.findings) h = h * 31 + f.scenario;
    for (int sc : session.appliedScenarios) h = h * 17 + sc;
    if (h != lastFindingsHash) { lastFindingsHash = h; diagnosis.refresh(); }
}

void MainComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    syncFromSettings();
    if (profilePanel.isVisible()) profilePanel.refresh();
    info.repaint();
    table.repaint();
    scope.repaint();
    grReadout.repaint();
    limiterReadout.repaint();
    diagnosis.refresh();
}

void MainComponent::mouseDown (const juce::MouseEvent& e)
{
    // The panel takes the keyboard on a click so Cmd+Z reaches keyPressed; a text box that is being typed into keeps it.
    if (auto* c = e.originalComponent)
        if (dynamic_cast<juce::TextEditor*> (c) != nullptr || c->findParentComponentOfClass<juce::TextEditor>() != nullptr) return;
    if (! hasKeyboardFocus (true)) grabKeyboardFocus();
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    // Undo / redo (the app's Edit menu does the same). In a plug-in the host's own Cmd+Z undoes the automatable parameters; what the
    // session's history holds there is only the rest, and when there is nothing to take back the key goes on to the host.
    {
        const auto mods = key.getModifiers();
        if (mods.isCommandDown() && ! mods.isAltDown())
        {
            const int code = juce::CharacterFunctions::toLowerCase ((juce::juce_wchar) key.getKeyCode());
            if (code == 'z') return mods.isShiftDown() ? session.redo() : session.undo();
           #if ! JUCE_MAC
            if (code == 'y' && ! mods.isShiftDown()) return session.redo();
           #endif
        }
    }
    if (key == juce::KeyPress::spaceKey && processor.isStandaloneApp())
    {
        playKey.triggerClick();
        return true;
    }
    return false;
}

bool MainComponent::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (f.endsWithIgnoreCase (".wav") || f.endsWithIgnoreCase (".aif") || f.endsWithIgnoreCase (".aiff")
            || (processor.isStandaloneApp() && juce::File (f).isDirectory())) return true;
    return false;
}

void MainComponent::filesDropped (const juce::StringArray& files, int x, int)
{
    // Several files or a folder (app): the batch queue. One file: left of the measurement VFD the source,
    // the right half the reference (app and plug-in).
    int audio = 0;
    bool folder = false;
    for (auto& f : files) { audio += isInterestedInFileDrag ({ f }) ? 1 : 0; folder |= juce::File (f).isDirectory(); }
    if (processor.isStandaloneApp() && (batchPanel.isVisible() || audio > 1 || folder))
    {
        juce::Array<juce::File> list;
        for (auto& f : files) list.add (juce::File (f));
        showBatchPanel (true);
        batchPanel.addFiles (list);
        return;
    }
    for (auto& f : files)
        if (isInterestedInFileDrag ({ f }))
        {
            if (x >= 700 || ! processor.isStandaloneApp()) session.loadReference (juce::File (f));
            else session.loadInput (juce::File (f));
            return;
        }
}

void MainComponent::chooseFile (bool reference)
{
    chooser = std::make_unique<juce::FileChooser> (reference ? "Load a reference track" : "Open a mix to master",
                                                   session.inputFile.existsAsFile() ? session.inputFile.getParentDirectory() : juce::File(),
                                                   "*.wav;*.aif;*.aiff");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this, reference] (const juce::FileChooser& fc)
    {
        const auto f = fc.getResult();
        if (! f.existsAsFile()) return;
        if (reference) session.loadReference (f);
        else session.loadInput (f);
    });
}

void MainComponent::chooseExport()
{
    if (session.output == nullptr) return;
    const auto base = session.inputFile.existsAsFile() ? session.inputFile.getSiblingFile (session.inputFile.getFileNameWithoutExtension() + "_MIHENK.wav")
                                                       : juce::File::getSpecialLocation (juce::File::userDesktopDirectory).getChildFile ("master_MIHENK.wav");
    chooser = std::make_unique<juce::FileChooser> ("Export master (" + juce::String (exportBits) + "-bit WAV)", base, "*.wav");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [this] (const juce::FileChooser& fc)
    {
        auto f = fc.getResult();
        if (f == juce::File()) return;
        if (! f.hasFileExtension ("wav")) f = f.withFileExtension ("wav");
        juce::String msg;
        if (! session.exportMaster (f, exportBits, msg))
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Export", msg);
    });
}

//==============================================================================
void MainComponent::setFace (bool advancedFace, bool persist)
{
    if (persist) ui::UiPrefs::setAdvancedFace (advancedFace);
    if (advanced == advancedFace) { syncFromSettings(); return; }
    advanced = advancedFace;
    if (! advanced) for (auto& l : ctx.layers) l = true;     // SIMPLE shows every layer
    animateKnobs = false;
    syncFromSettings();
    animateKnobs = true;
    if (! persist) return;
    session.status = advanced ? "ADVANCED face: every control of the chain." : "SIMPLE face: starting point, target, diagnosis and five macros (ADVANCED has every control).";
    session.sendChangeMessage();
}

void MainComponent::showBatchPanel (bool show)
{
    if (! processor.isStandaloneApp()) return;
    if (show)
    {
        showProfilePanel (false);
        batchPanel.toFront (false);
    }
    batchPanel.setVisible (show);
}

const MainComponent::Knob* MainComponent::findKnob (const juce::String& label) const
{
    // "RELEASE#2": the second knob of that name that is shown (the limiter's after the compressor's; SHELF FREQ#2 is the high shelf's).
    const int hash = label.lastIndexOfChar ('#');
    const juce::String name = hash > 0 ? label.substring (0, hash) : label;
    int nth = hash > 0 ? label.substring (hash + 1).getIntValue() : 1;
    const Knob* any = nullptr;
    for (auto& k : knobs)
        if (k->label == name)
        {
            if (knobShown (*k) && --nth <= 0) return k.get();
            if (any == nullptr) any = k.get();
        }
    return hash > 0 ? nullptr : any;
}

double MainComponent::knobValueForTesting (const juce::String& label) const
{
    auto* k = findKnob (label);
    return k != nullptr ? k->slider.getValue() : std::nan ("");
}

juce::String MainComponent::knobTextForTesting (const juce::String& label) const
{
    auto* k = findKnob (label);
    return k != nullptr ? k->format (k->slider.getValue()) : juce::String();
}

bool MainComponent::autoLampForTesting (const juce::String& label) const
{
    auto* k = findKnob (label);
    return k != nullptr && k->autoLamp != nullptr && k->autoLamp->isVisible() && k->autoLamp->isOn();
}

bool MainComponent::knobVisibleForTesting (const juce::String& label) const
{
    auto* k = findKnob (label);
    return k != nullptr && k->slider.isVisible();
}

void MainComponent::turnKnobForTesting (const juce::String& label, double v)
{
    if (auto* k = const_cast<Knob*> (findKnob (label)))
        k->slider.setValue (v, juce::sendNotificationSync);
}

void MainComponent::dragKnobForTesting (const juce::String& label, double from, double to, int steps)
{
    auto* k = const_cast<Knob*> (findKnob (label));
    if (k == nullptr || steps < 1) return;
    if (k->slider.onDragStart) k->slider.onDragStart();
    for (int i = 0; i <= steps; ++i)
        k->slider.setValue (from + (to - from) * (double) i / (double) steps, juce::sendNotificationSync);
    if (k->slider.onDragEnd) k->slider.onDragEnd();
}

void MainComponent::clickAutoLampForTesting (const juce::String& label)
{
    if (auto* k = const_cast<Knob*> (findKnob (label)))
        if (k->autoLamp != nullptr && k->autoLamp->onClick) k->autoLamp->onClick();
}

void MainComponent::clickCharacterForTesting (PlanCharacter c)
{
    (c == PlanCharacter::OFF ? charOff : c == PlanCharacter::GENTLE ? charGentle : charFull).onClick();
}

void MainComponent::clickResetOverridesForTesting() { resetOverridesCap.onClick(); }

bool MainComponent::overrideRingForTesting (const juce::String& label) const
{
    auto* k = findKnob (label);
    return k != nullptr && k->paramId != nullptr && session.isOverridden (k->paramId());
}

juce::Button* MainComponent::switchByName (const juce::String& name) const
{
    const std::pair<const char*, juce::Button*> list[] {
        { "EQ IN", const_cast<ui::CapButton*> (&eqIn) }, { "LOW CUT", const_cast<ui::CapButton*> (&eqLowCut) }, { "24 DB", const_cast<ui::CapButton*> (&eqSteep) },
        { "LINEAR", const_cast<ui::CapButton*> (&eqLinear) }, { "AIR DYN", const_cast<ui::CapButton*> (&eqAirDyn) },
        { "LOW-MID DYN", const_cast<ui::CapButton*> (&dynLowMid) }, { "UPPER-MID DYN", const_cast<ui::CapButton*> (&dynUpperMid) },
        { "TIGHTEN", const_cast<ui::CapButton*> (&dynTight) }, { "DC FILTER", const_cast<ui::CapButton*> (&dcFilter) },
        { "COMP IN", const_cast<ui::CapButton*> (&compIn) }, { "3-BAND", const_cast<ui::CapButton*> (&compThreeBand) },
        { "SOLO", const_cast<ui::CapButton*> (&bandSolo) }, { "BAND BYPASS", const_cast<ui::CapButton*> (&bandBypass) },
        { "BAND LOW", const_cast<ui::CapButton*> (&bandLow) }, { "BAND MID", const_cast<ui::CapButton*> (&bandMid) }, { "BAND HIGH", const_cast<ui::CapButton*> (&bandHigh) },
        { "WIDTH IN", const_cast<ui::CapButton*> (&widthIn) }, { "PHASE SAFE", const_cast<ui::CapButton*> (&phaseSafe) }, { "LIMITER IN", const_cast<ui::CapButton*> (&limIn) },
        { "BYPASS", const_cast<ui::KeyButton*> (&bypassKey) } };
    for (auto& [n, b] : list) if (name == n) return b;
    return nullptr;
}

bool MainComponent::clickSwitchForTesting (const juce::String& name, bool on)
{
    auto* b = switchByName (name);
    if (b == nullptr || ! b->isEnabled()) return false;
    if (b->getToggleState() == on) return true;
    b->setToggleState (on, juce::dontSendNotification);
    if (b->onClick) b->onClick();
    return true;
}

bool MainComponent::switchStateForTesting (const juce::String& name) const { auto* b = switchByName (name); return b != nullptr && b->getToggleState(); }
bool MainComponent::switchEnabledForTesting (const juce::String& name) const { auto* b = switchByName (name); return b != nullptr && b->isEnabled(); }

juce::String MainComponent::knobParamIdForTesting (const juce::String& label) const
{
    auto* k = findKnob (label);
    return k != nullptr && k->paramId != nullptr ? k->paramId() : juce::String();
}
bool MainComponent::knobDimmedForTesting (const juce::String& label) const
{
    auto* k = findKnob (label);
    return k != nullptr && (bool) k->slider.getProperties()["dim"];
}
bool MainComponent::knobEnabledForTesting (const juce::String& label) const
{
    auto* k = findKnob (label);
    return k != nullptr && k->slider.isEnabled();
}
bool MainComponent::knobDoubleClickInfoForTesting (int index, juce::String& label, juce::String& paramId, double& scale, double& fixedDef, double& target, bool& stepped) const
{
    if (index < 0 || index >= (int) knobs.size()) return false;
    auto& k = *knobs[(size_t) index];
    label = k.label;
    paramId = k.paramId != nullptr ? k.paramId() : juce::String();
    scale = k.scale;
    fixedDef = k.slider.getDoubleClickReturnValue();
    target = k.slider.doubleClickValue();
    stepped = ! k.steps.empty();
    return true;
}
void MainComponent::doubleClickKnobForTesting (const juce::String& label)
{
    if (auto* k = const_cast<Knob*> (findKnob (label))) k->slider.resetToDefault();
}
bool MainComponent::knobRangeForTesting (const juce::String& label, double& mn, double& mx, double& scale) const
{
    auto* k = findKnob (label);
    if (k == nullptr) return false;
    mn = k->slider.getMinimum(); mx = k->slider.getMaximum(); scale = k->scale;
    return true;
}
juce::String MainComponent::knobHintForTesting (const juce::String& label) const
{
    auto* k = findKnob (label);
    if (k == nullptr) return {};
    if (k->label == "AIR FREQ" && session.settings.eq.airDb < kAirNeededDb) return "SET AIR";
    return k->hint;
}
void MainComponent::clickKnobValueForTesting (const juce::String& label)
{
    if (auto* k = const_cast<Knob*> (findKnob (label)))
        if (auto* h = dynamic_cast<ValueHit*> (k->valueHit.get())) h->clicked();
}

bool MainComponent::isComponentVisibleForTesting (const juce::String& name) const
{
    const std::pair<const char*, const juce::Component*> list[] {
        { "matchRef", &matchRefKey }, { "clearRef", &clearRef }, { "referencePanel", &referencePanel }, { "chain", &chainReadout }, { "batch", &batchPanel },
        { "grReadout", &grReadout }, { "compThreeBand", &compThreeBand }, { "layerIn", &layerIn }, { "keep", &presetKeep }, { "abMatch", &abMatchCap } };
    for (auto& [n, c] : list) if (name == n) return c->isVisible();
    return false;
}
} // namespace mihenk
