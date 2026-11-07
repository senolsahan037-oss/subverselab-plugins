#pragma once

#include <JuceHeader.h>
#include <functional>
#include <vector>
#include "PluginProcessor.h"
#include "ParamText.h"
#include "Theme.h"

/*  The panel's logic, with no drawing: the selected band, the choosers, the layer lamps, and the curves.
    Every control reads and writes the parameters through ONE Ctl.
    Curves are never drawn from a model of our own: a band is the engine's own RBJ / Butterworth design (Engine::freeBandMagDb); COLLIDE, SELF and TARGET
    are the same exact bell / edge responses the engine uses for bandConflictDb / bandSelfDb / bandTargetDb (P1m), fed with the numbers the audio thread publishes. */
namespace panel
{
enum Mode { kBand0 = 0, kTarget = 6, kCollide = 7, kSelf = 8 };
constexpr int kBands = dyneq::kFreeBands;
constexpr int kCurveN = 361;                       // 20 Hz .. 20 kHz, log

inline juce::String bandId (int i, const char* n) { return "b" + juce::String (i + 1) + "_" + n; }
inline juce::String fmtHzShort (float f) { return f >= 1000.0f ? juce::String (f / 1000.0f, f >= 10000.0f ? 1 : 2) + "k" : juce::String (juce::roundToInt (f)); }
inline juce::String sgn (float v, int d = 1) { return (v > 0.04f ? "+" : "") + dyneq::ptext::fixed (v, d); }

struct ChooserItem { juce::String text; std::function<void()> apply; bool selected = false; bool heading = false; };

//==============================================================================
struct Ctl
{
    explicit Ctl (DyneqAudioProcessor& p) : proc (p)
    {
        const int m = proc.uiMode.load();
        selectedBand = m < kTarget && bandOn (juce::jlimit (0, kBands - 1, m)) ? juce::jlimit (0, kBands - 1, m) : -1;   // a layer mode or an unused band opens with nothing selected
    }
    DyneqAudioProcessor& proc;

    // ---- parameters
    juce::RangedAudioParameter* par (const juce::String& id) const { return dynamic_cast<juce::RangedAudioParameter*> (proc.apvts.getParameter (id)); }
    float raw (const juce::String& id) const { return proc.apvts.getRawParameterValue (id)->load(); }
    bool  on (const juce::String& id) const { return raw (id) > 0.5f; }
    void  setGesture (const juce::String& id, float plain) { svl::setParamAsGesture (par (id), plain); }
    void  setQuiet (const juce::String& id, float plain) { if (auto* p = par (id)) p->setValueNotifyingHost (p->convertTo0to1 (plain)); }   // inside a gesture / an undo group
    float defaultOf (const juce::String& id) const { auto* p = par (id); return p->convertFrom0to1 (p->getDefaultValue()); }

    // the panel's mode, kept in the state (processor uiMode): 0..5 = the band selected last, 6..8 = a layer (older projects saved a layer here)
    int  mode() const { return juce::jlimit (0, 8, proc.uiMode.load()); }
    void setMode (int m) { m = juce::jlimit (0, 8, m); proc.uiMode = m; }
    // the band the chips, the docked strip and the gold node edit (UX redesign 11 Oct). UI only, -1 = none. An ASSIST layer never moves it (the old fallback
    // to band 1 after TARGET / COLLIDE / SELF is what made "band 1 at 1000 Hz" edit a band the owner was not looking at). Selecting a band saves it as the mode.
    int  selectedBand = -1;
    void selectLab (int i) { selectedBand = i < 0 ? -1 : juce::jlimit (0, kBands - 1, i); if (selectedBand >= 0) setMode (selectedBand); }
    void selectBandPanel (int i) { selectLab (i); }
    int  analyzerMode() const { return juce::jlimit (0, 3, juce::roundToInt (raw ("analyzer"))); }   // 0 OFF, 1 PRE, 2 POST, 3 PRE+POST

    // ---- band
    int  bandType (int i) const { return juce::jlimit (0, 5, juce::roundToInt (raw (bandId (i, "type")))); }
    bool bandOn (int i) const { return on (bandId (i, "on")); }
    static bool typeHasGain (int t) { return t <= 2; }
    static bool typeHasQ (int) { return true; }          // P1n: the last stage of a cut follows Q (resonance), so every type has a live Q
    int  bandSlopeDb (int i) const { return dyneq::freeSlopeDbFromIndex (juce::roundToInt (raw (bandId (i, "slope")))); }
    // The frequency a band really runs at: the engine never goes above 0.45 x the sample rate (the FREQ box, the read-out and the node show this, not the request)
    float workingHz (float requested) const { const double sr = proc.getSampleRate() > 0.0 ? proc.getSampleRate() : 48000.0; return (float) std::min ((double) requested, dyneq::Engine::freeBandMaxFreqHz (sr)); }
    juce::String bandTypeName (int i) const
    {
        if (! bandOn (i)) return "OFF";
        static const char* n[] = { "BELL", "LOW SHELF", "HIGH SHELF", "NOTCH", "LOW CUT", "HIGH CUT" };
        const int t = bandType (i);
        return juce::String (n[t]) + (t >= 4 ? " " + juce::String (bandSlopeDb (i)) : juce::String());
    }

    // ---- ASSIST layers (TARGET, COLLIDE, SELF)
    bool layerOn (int m) const
    {
        if (m == kTarget) return juce::roundToInt (raw ("targetInst")) != 0;
        if (m == kCollide) return juce::roundToInt (raw ("mode")) == dyneq::kCollide && ! on ("assistOff");
        if (m == kSelf) return juce::roundToInt (raw ("mode")) == dyneq::kSelf && ! on ("assistOff");
        return false;
    }
    void toggleLayer (int m)
    {
        svl::ParamUndo::Group one (proc.undo, "LAYER");
        if (m == kTarget)
        {
            const int inst = juce::roundToInt (raw ("targetInst"));
            if (inst != 0) { proc.lastTargetInst = inst; setQuiet ("targetInst", 0.0f); }
            else { const int back = juce::jlimit (1, dyneq::kTargetInsts - 1, proc.lastTargetInst.load()); setQuiet ("targetInst", (float) back); }
            return;
        }
        const int md = m == kCollide ? dyneq::kCollide : dyneq::kSelf;
        if (layerOn (m)) setQuiet ("assistOff", 1.0f);
        else { setQuiet ("mode", (float) md); setQuiet ("assistOff", 0.0f); }
    }
    juce::String layerSub (int m) const
    {
        if (m == kTarget) return juce::roundToInt (raw ("targetInst")) != 0 ? targetName() : "OFF";
        if (m == kCollide) return proc.isSidechainConnected() ? "KEY - SIDECHAIN" : "NO KEY";
        return "RESONANCES";
    }

    // ---- TARGET: the chooser's list maps to targetInst + targetPart + reverbReady. P4c: EVERY instrument and part the engine has (the P4 list left 7 of them out).
    struct TargetEntry { juce::String text; int inst, part; bool rev; };
    static const std::vector<TargetEntry>& targetEntries()
    {
        static const std::vector<TargetEntry> e = []
        {
            std::vector<TargetEntry> v; v.push_back ({ "OFF", 0, 0, false });
            for (int inst : { dyneq::kTgtKick, dyneq::kTgtSnare, dyneq::kTgtHats, dyneq::kTgtBass, dyneq::kTgtVocal, dyneq::kTgtDrums })
            {
                for (int p = 0; p < dyneq::kInst[inst].n; ++p) v.push_back ({ juce::String (dyneq::kInst[inst].name) + " - " + dyneq::partName (inst, p), inst, p, false });
                if (dyneq::kInst[inst].reverbOk) v.push_back ({ juce::String (dyneq::kInst[inst].name) + " - REVERB-READY", inst, 0, true });
            }
            return v;
        }();
        return e;
    }
    int targetEntryIndex() const
    {
        const int inst = juce::roundToInt (raw ("targetInst"));
        if (inst == 0) return 0;
        const int part = dyneq::partIndex (inst, raw ("targetPart"));
        const bool rev = on ("reverbReady");
        const auto& e = targetEntries();
        for (size_t i = 1; i < e.size(); ++i)
            if (e[i].inst == inst && e[i].part == part && e[i].rev == rev) return (int) i;
        return -1;
    }
    /** TARGET learns the chosen sound from the hits it hears; until kTargetMinHits have passed the analysis it cuts nothing (owner 12 Oct, Live:
        "target ... herhangi bir preset gelmiyor"). This line says which state it is in. Empty while TARGET is off. ready = cutting. */
    juce::String targetStatus (bool* ready = nullptr) const
    {
        if (ready != nullptr) *ready = false;
        if (! layerOn (kTarget)) return {};
        const int hits = proc.getTargetHits();
        if (hits >= dyneq::kTargetMinHits) { if (ready != nullptr) *ready = true; return "LEARNED FROM " + juce::String (hits) + " HITS - CUTTING"; }
        const auto t = proc.getTargetListen();
        if (t.onsets == 0) return "LISTENING - PLAY THE TRACK: NO HIT HEARD YET";
        juce::String s = "LISTENING - " + juce::String (t.onsets) + " HIT" + (t.onsets == 1 ? "" : "S") + " HEARD, " + juce::String (hits) + " OF " + juce::String (dyneq::kTargetMinHits) + " LEARNED";
        int worst = -1, n = 0;
        for (int i = 0; i < 4; ++i) if (t.rejected[i] > n) { n = t.rejected[i]; worst = i; }
        static const char* why[] = { "HITS TOO CLOSE TOGETHER", "HITS TOO SHORT", "NO PITCHED BOOM FOUND", "THE PART WAS NOT FOUND" };
        if (worst >= 0 && t.learned < t.onsets) s << " (" << why[worst] << ")";
        return s;
    }
    juce::String targetName() const
    {
        const int inst = juce::roundToInt (raw ("targetInst"));
        if (inst == 0) return "OFF";
        const int k = targetEntryIndex();
        if (k > 0) return targetEntries()[(size_t) k].text;
        return juce::String (dyneq::kInst[inst].name) + " - " + dyneq::partName (inst, dyneq::partIndex (inst, raw ("targetPart"))) + (on ("reverbReady") ? " (REVERB-READY)" : "");
    }
    void applyTarget (int k)
    {
        const auto& e = targetEntries()[(size_t) k];
        svl::ParamUndo::Group one (proc.undo, "TARGET");
        setQuiet ("targetInst", (float) e.inst);
        if (e.inst != 0) { setQuiet ("targetPart", dyneq::sliderForPart (e.inst, e.part)); setQuiet ("reverbReady", e.rev ? 1.0f : 0.0f); proc.lastTargetInst = e.inst; }
    }

    // ---- band shapes and slopes (Pro-Q order). A shape or slope pick also switches the band on (a pick on an off band means "I want this").
    struct ShapeEntry { const char* text; int type; };
    static const std::vector<ShapeEntry>& shapeEntries()
    {
        static const std::vector<ShapeEntry> e { { "BELL", dyneq::kBell }, { "LOW SHELF", dyneq::kLowShelf }, { "LOW CUT", dyneq::kLowCut }, { "HIGH SHELF", dyneq::kHighShelf }, { "HIGH CUT", dyneq::kHighCut }, { "NOTCH", dyneq::kNotch } };
        return e;
    }
    static bool isCut (int t) { return t >= dyneq::kLowCut; }
    static bool typeHasDyn (int t) { return t <= dyneq::kHighShelf; }        // bell and shelves only (the engine has no dynamic cut or notch)
    static constexpr int kSlopeList[5] { 12, 24, 36, 48, 72 };
    static int slopeIndexOfDb (int db) { return dyneq::freeSlopeIndexFromDb (db); }   // the SLOPE choice index (12 24 48 36 72 order)
    static juce::String shapeName (int t) { for (auto& e : shapeEntries()) if (e.type == t) return e.text; return {}; }
    void setShape (int band, int type)
    {
        svl::ParamUndo::Group one (proc.undo, "SHAPE");
        const bool had = typeHasGain (bandType (band)) && bandOn (band);
        const float g = raw (bandId (band, "gain"));
        setQuiet (bandId (band, "type"), (float) type);
        if (typeHasGain (type) && (! had || std::abs (g) < 0.1f))   // BELL / LOW SHELF / HIGH SHELF from a type with no gain start at +3 / -3 / +2 dB
            setQuiet (bandId (band, "gain"), type == dyneq::kBell ? 3.0f : type == dyneq::kLowShelf ? -3.0f : 2.0f);
        setQuiet (bandId (band, "on"), 1.0f);
    }
    void setSlope (int band, int db)
    {
        svl::ParamUndo::Group one (proc.undo, "SLOPE");
        setQuiet (bandId (band, "slope"), (float) slopeIndexOfDb (db));
        setQuiet (bandId (band, "on"), 1.0f);
    }
    static constexpr const char* kBandParams[12] { "on", "type", "freq", "gain", "q", "slope", "dyn", "thr", "range", "att", "rel", "trig" };
    void deleteBand (int band)   // the band goes off and every one of its parameters returns to its default: one undo step
    {
        {
            svl::ParamUndo::Group one (proc.undo, "DELETE BAND");
            for (const char* n : kBandParams) setQuiet (bandId (band, n), defaultOf (bandId (band, n)));
        }
        if (selectedBand == band) selectedBand = -1;
    }
    /** An unused slot: the band is off and every one of its parameters is at its default (the chip shows + ADD). */
    bool isEmptySlot (int band) const
    {
        if (bandOn (band)) return false;
        for (const char* n : kBandParams) { auto* p = par (bandId (band, n)); if (std::abs (p->getValue() - p->getDefaultValue()) > 1.0e-6f) return false; }
        return true;
    }
    /** + ADD on a chip: the band goes on as a BELL at its own default frequency, 0 dB, and is selected. One undo step. */
    void addSlot (int band)
    {
        {
            svl::ParamUndo::Group one (proc.undo, "ADD BAND");
            setQuiet (bandId (band, "type"), (float) dyneq::kBell);
            setQuiet (bandId (band, "on"), 1.0f);
        }
        selectLab (band);
    }
    /** The RANGE box is the dynamic switch (Pro-Q's range ring): any range other than 0 switches the band's dynamics on, 0 switches them off. */
    static bool rangeMeansDyn (float rangeDb) { return std::abs (rangeDb) >= 0.05f; }
    std::vector<ChooserItem> shapeItems (int band) const
    {
        std::vector<ChooserItem> v; Ctl* self = const_cast<Ctl*> (this);
        for (auto& e : shapeEntries()) { const int t = e.type; v.push_back ({ e.text, [self, band, t] { self->setShape (band, t); }, bandOn (band) && bandType (band) == t }); }
        return v;
    }
    std::vector<ChooserItem> slopeItems (int band) const
    {
        std::vector<ChooserItem> v; Ctl* self = const_cast<Ctl*> (this);
        for (int db : kSlopeList) v.push_back ({ juce::String (db) + " dB/oct", [self, band, db] { self->setSlope (band, db); }, bandOn (band) && bandSlopeDb (band) == db });
        return v;
    }
    /** The right-click menu of a band node: shape, slope (cuts only), on/off, delete. */
    std::vector<ChooserItem> menuItems (int band) const
    {
        std::vector<ChooserItem> v; Ctl* self = const_cast<Ctl*> (this);
        ChooserItem h; h.text = "SHAPE"; h.heading = true; v.push_back (h);
        for (auto& it : shapeItems (band)) v.push_back (it);
        if (isCut (bandType (band)))
        {
            ChooserItem h2; h2.text = "SLOPE"; h2.heading = true; v.push_back (h2);
            for (auto& it : slopeItems (band)) v.push_back (it);
        }
        ChooserItem h3; h3.text = "BAND"; h3.heading = true; v.push_back (h3);
        v.push_back ({ bandOn (band) ? "SWITCH OFF" : "SWITCH ON", [self, band] { self->toggleBand (band); }, false });
        v.push_back ({ "DELETE", [self, band] { self->deleteBand (band); }, false });
        return v;
    }
    std::vector<ChooserItem> chooserItems() const   // the TARGET instrument / part list
    {
        std::vector<ChooserItem> v; Ctl* self = const_cast<Ctl*> (this);
        const int cur = targetEntryIndex();
        for (size_t i = 0; i < targetEntries().size(); ++i)
            v.push_back ({ targetEntries()[i].text, [self, i] { self->applyTarget ((int) i); }, (int) i == cur });
        return v;
    }

    // ---- bands
    void toggleBand (int i) { setGesture (bandId (i, "on"), bandOn (i) ? 0.0f : 1.0f); }
    int  firstFreeBand() const   // an unused slot first; then any band that is switched off
    {
        for (int i = 0; i < kBands; ++i) if (isEmptySlot (i)) return i;
        for (int i = 0; i < kBands; ++i) if (! bandOn (i)) return i;
        return -1;
    }
    /** Double-click on empty space of a screen: the next free band is created there. A BELL, or a LOW / HIGH CUT (24 dB/oct) at the extremes of the range. Returns the band, -1 when all six are in use. */
    static constexpr double kEdgeLowHz = 24.6, kEdgeHighHz = 16200.0;     // the outer 3 % of the log axis
    int newBandAt (double hz, double gainDb)
    {
        const int k = firstFreeBand();
        if (k < 0) return -1;
        svl::ParamUndo::Group one (proc.undo, "NEW BAND");
        const int type = hz <= kEdgeLowHz ? dyneq::kLowCut : hz >= kEdgeHighHz ? dyneq::kHighCut : dyneq::kBell;
        for (const char* n : { "type", "freq", "gain", "q", "slope", "dyn", "thr", "range", "att", "rel", "trig" }) setQuiet (bandId (k, n), defaultOf (bandId (k, n)));
        setQuiet (bandId (k, "type"), (float) type);
        setQuiet (bandId (k, "freq"), (float) juce::jlimit (20.0, 20000.0, hz));
        if (type == dyneq::kBell) setQuiet (bandId (k, "gain"), (float) juce::jlimit (-18.0, 18.0, std::round (gainDb * 10.0) / 10.0));
        if (isCut (type)) setQuiet (bandId (k, "slope"), (float) slopeIndexOfDb (24));
        setQuiet (bandId (k, "q"), 1.0f);
        setQuiet (bandId (k, "on"), 1.0f);
        selectBandPanel (k);
        return k;
    }

    // ---- DELTA (hold), BYPASS, KEY
    bool deltaHeld = false;
    void deltaSet (bool held)
    {
        if (held == deltaHeld) return;
        deltaHeld = held;
        auto* p = par ("delta");
        if (held) { p->beginChangeGesture(); p->setValueNotifyingHost (1.0f); }
        else { p->setValueNotifyingHost (0.0f); p->endChangeGesture(); }
    }
    bool delta() const { return on ("delta"); }
    bool bypass() const { return on ("bypass"); }
    bool keyListen() const { return on ("keyListen"); }
    juce::String keyText() const { return keyListen() ? "KEY - LISTEN" : proc.isSidechainConnected() ? "KEY - SIDECHAIN" : "KEY - NO SIGNAL"; }
};

//==============================================================================
/** All the curves of one frame, in dB over 20 Hz .. 20 kHz (kCurveN log-spaced points), plus the band nodes. */
struct Curves
{
    float f[kCurveN] {};
    float target[kCurveN] {}, collide[kCurveN] {}, self[kCurveN] {}, bands[kCurveN] {}, selected[kCurveN] {}, total[kCurveN] {};
    bool  selectedOn = false;
    float outDb = 0.0f;
    struct Band { bool on = false; int type = 0; float freq = 1000.0f, gainShown = 0.0f, q = 1.0f; int slopeDb = 12; bool dyn = false; };
    Band band[kBands];
    double sr = 48000.0;
    // layer data of the frame
    int nBell = 0; float bellF[32] {}, bellQ[32] {}, bellG[32] {};
    int nHunt = 0; float huntF[4] {}, huntQ[4] {}, huntG[4] {};
    bool tbOn = false; float tbF = 0, tbQ = 1, tbCut = 0;   // the middle bell (read-outs)
    bool tbsOn[3] {}; float tbsF[3] {}, tbsQ[3] { 1, 1, 1 }, tbsCut[3] {};   // the plateau's three bells (11 Oct)
    struct Edge { bool run = false, below = true; float hz = 0, cut = 0, mix = 0; int slope = 1; } edge[2];

    Curves() { for (int i = 0; i < kCurveN; ++i) f[i] = 20.0f * std::pow (1000.0f, (float) i / (float) (kCurveN - 1)); }

    double bandOneDb (int i, double hz) const
    {
        const Band& b = band[i];
        if (! b.on) return 0.0;
        return dyneq::Engine::freeBandMagDb (b.type, b.freq, b.gainShown, b.q, b.slopeDb, hz, sr);
    }
    double bandsDb (double hz) const { double s = 0; for (int i = 0; i < kBands; ++i) s += bandOneDb (i, hz); return s; }
    double collideDb (double hz) const { double s = 0; for (int k = 0; k < nBell; ++k) s += dyneq::bellDbAt (hz, bellF[k], bellQ[k], -bellG[k], sr); return s; }
    double selfDb (double hz) const { double s = 0; for (int k = 0; k < nHunt; ++k) s += dyneq::bellDbAt (hz, huntF[k], huntQ[k], huntG[k], sr); return s; }
    double targetDb (double hz) const
    {
        double s = 0;
        for (int i = 0; i < 3; ++i) if (tbsOn[i]) s += dyneq::bellDbAt (hz, tbsF[i], tbsQ[i], -tbsCut[i], sr);
        for (int k = 0; k < 2; ++k)
            if (edge[k].run && edge[k].cut != 0.0f) s -= (double) edge[k].mix * dyneq::edgeCutDbAt (hz, edge[k].hz, edge[k].slope, edge[k].cut, edge[k].below, sr);
        return s;
    }
    double totalAt (double hz) const { return bandsDb (hz) + collideDb (hz) + selfDb (hz) + targetDb (hz) + outDb; }

    void update (Ctl& c)
    {
        auto& p = c.proc;
        sr = p.getSampleRate() > 0.0 ? p.getSampleRate() : 48000.0;
        outDb = c.raw ("output");
        for (int i = 0; i < kBands; ++i)
        {
            Band& b = band[i];
            b.on = c.bandOn (i); b.type = c.bandType (i); b.freq = c.workingHz (c.raw (bandId (i, "freq"))); b.q = c.raw (bandId (i, "q")); b.slopeDb = c.bandSlopeDb (i);
            b.dyn = c.on (bandId (i, "dyn"));
            b.gainShown = b.dyn ? p.getFreeBandGainDb (i) : c.raw (bandId (i, "gain"));   // a dynamic band: the gain the engine runs now
            if (! Ctl::typeHasGain (b.type)) b.gainShown = 0.0f;
        }
        nBell = 0;
        for (int b = 0; b < dyneq::kBands; ++b)
        {
            const float g = p.getBellGainDb (b);
            if (g > 0.01f && nBell < 32) { bellF[nBell] = DyneqAudioProcessor::bandCentresHz()[b]; bellQ[nBell] = DyneqAudioProcessor::bellQs()[b]; bellG[nBell] = g; ++nBell; }
        }
        nHunt = 0;
        for (int s = 0; s < dyneq::kHunters; ++s)
        {
            const float g = p.getHunterGainDb (s);
            if (g < -0.01f) { huntF[nHunt] = p.getHunterFreqHz (s); huntQ[nHunt] = p.getHunterQ (s); huntG[nHunt] = g; ++nHunt; }
        }
        tbOn = p.getTargetBell (tbF, tbQ, tbCut);
        for (int i = 0; i < 3; ++i) tbsOn[i] = p.getTargetBell (i, tbsF[i], tbsQ[i], tbsCut[i]);
        for (int k = 0; k < 2; ++k)
        {
            const auto& e = p.getEdge (k);
            edge[k].run = e.run.load (std::memory_order_relaxed); edge[k].hz = e.hz.load (std::memory_order_relaxed); edge[k].cut = e.cutDb.load (std::memory_order_relaxed);
            edge[k].mix = e.mix.load (std::memory_order_relaxed); edge[k].slope = e.slope.load (std::memory_order_relaxed); edge[k].below = e.below.load (std::memory_order_relaxed);
        }
        const int sel = c.selectedBand;   // the strip's band
        selectedOn = sel >= 0 && band[sel].on;
        for (int i = 0; i < kCurveN; ++i)
        {
            const double hz = f[i];
            bands[i] = (float) bandsDb (hz); collide[i] = (float) collideDb (hz); self[i] = (float) selfDb (hz); target[i] = (float) targetDb (hz);
            selected[i] = selectedOn ? (float) bandOneDb (sel, hz) : 0.0f;
            total[i] = bands[i] + collide[i] + self[i] + target[i] + outDb;
        }
    }
};

//==============================================================================
/** The analyzer: three taps of the real audio (PRE = the plug-in's input, POST = its output, KEY = the sidechain), each the newest 4096 mono samples, a Hann window,
    a radix-2 FFT, power averaged over +-1/8 octave per curve point, smoothed in time (quick to rise, slow to fall). The values are dBFS of a bin (a full-scale sine = 0 dB).
    `update` is called by the editor's timer (message thread) and reads the processor's tap rings; it never touches the audio thread. Silence falls to kFloorDb. */
struct Spectrum
{
    static constexpr int N = 4096;
    static constexpr float kFloorDb = -120.0f;
    struct Trace { float db[kCurveN]; bool fresh = false; Trace() { for (auto& d : db) d = kFloorDb; } };
    Trace pre, post, key;
    float (&db)[kCurveN] = pre.db;                  // the input trace (kept under its P4 name)
    int frames = 0;                                 // how many times update() ran (the test counts frames per second)
    Spectrum() { win.resize (N); for (int i = 0; i < N; ++i) win[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * (float) i / (float) N); }
    Spectrum (const Spectrum&) = delete;
    Spectrum& operator= (const Spectrum&) = delete;

    /** wantPre / wantPost / wantKey say which taps to compute (an off trace is neither computed nor drawn; it is reset to the floor so a later switch-on starts clean). */
    void update (DyneqAudioProcessor& p, const float* curveHz, bool wantPre = true, bool wantPost = false, bool wantKey = false)
    {
        ++frames;
        run (p, DyneqAudioProcessor::kTapPre, pre, curveHz, wantPre);
        run (p, DyneqAudioProcessor::kTapPost, post, curveHz, wantPost);
        run (p, DyneqAudioProcessor::kTapKey, key, curveHz, wantKey);
    }
    void run (DyneqAudioProcessor& p, int tap, Trace& t, const float* curveHz, bool want)
    {
        t.fresh = false;
        if (! want) { for (auto& d : t.db) d = kFloorDb; return; }
        std::vector<float>& x = buf; x.resize (N);
        if (! p.copyTail (tap, x.data(), N)) return;
        re.assign (N, 0.0f); im.assign (N, 0.0f);
        float wsum = 0.0f;
        for (int i = 0; i < N; ++i) { re[(size_t) i] = x[(size_t) i] * win[(size_t) i]; wsum += win[(size_t) i]; }
        fft (re, im);
        const double sr = p.getSampleRate() > 0.0 ? p.getSampleRate() : 48000.0;
        pw.resize (N / 2); lv.resize (kCurveN);
        const float norm = 2.0f / wsum;
        for (int k = 0; k < N / 2; ++k) { const float a = norm * std::sqrt (re[(size_t) k] * re[(size_t) k] + im[(size_t) k] * im[(size_t) k]); pw[(size_t) k] = a * a; }
        for (int i = 0; i < kCurveN; ++i)
        {
            const double lo = curveHz[i] / 1.0905, hi = curveHz[i] * 1.0905;   // +-1/8 octave
            const double kb = (double) N / sr;
            float L;
            if ((hi - lo) * kb < 3.0)   // low frequencies: the window holds fewer than 3 bins, so the trace is the line through the neighbouring bins in dB (no stair steps)
            {
                const double kc = curveHz[i] * kb; const int a = juce::jlimit (1, N / 2 - 2, (int) std::floor (kc)); const float fr = (float) juce::jlimit (0.0, 1.0, kc - (double) a);
                const float d0 = 10.0f * std::log10 (pw[(size_t) a] + 1e-14f), d1 = 10.0f * std::log10 (pw[(size_t) a + 1] + 1e-14f);
                L = juce::jmax (kFloorDb, d0 + (d1 - d0) * fr);
            }
            else
            {
                const int k0 = juce::jlimit (1, N / 2 - 1, (int) std::floor (lo * kb)), k1 = juce::jlimit (k0, N / 2 - 1, (int) std::ceil (hi * kb));
                double s = 0; for (int k = k0; k <= k1; ++k) s += pw[(size_t) k];
                L = juce::jmax (kFloorDb, (float) (10.0 * std::log10 (s / (double) (k1 - k0 + 1) + 1e-14)));
            }
            lv[(size_t) i] = L;
        }
        // a light smoothing across neighbouring points (1-2-3-2-1) so the curve reads as a spectrum, not a comb, then the ballistics: fast up, slow down
        for (int i = 0; i < kCurveN; ++i)
        {
            float acc = 0.0f, wsum2 = 0.0f;
            for (int d = -2; d <= 2; ++d) { const int j = juce::jlimit (0, kCurveN - 1, i + d); const float w = (float) (3 - std::abs (d)); acc += lv[(size_t) j] * w; wsum2 += w; }
            const float L = acc / wsum2;
            t.db[i] += (L - t.db[i]) * (L > t.db[i] ? 0.5f : 0.09f);
            if (t.db[i] < kFloorDb + 0.05f) t.db[i] = kFloorDb;
        }
        t.fresh = true;
    }
    /** max - min of a trace: a flat trace (silence, or nothing computed) has a range near 0. */
    static float range (const Trace& t) { float lo = 1.0e9f, hi = -1.0e9f; for (float d : t.db) { lo = std::min (lo, d); hi = std::max (hi, d); } return hi - lo; }
    static float peak (const Trace& t) { float hi = -1.0e9f; for (float d : t.db) hi = std::max (hi, d); return hi; }
    static void fft (std::vector<float>& re, std::vector<float>& im)
    {
        const int n = (int) re.size();
        for (int i = 1, j = 0; i < n; ++i)
        {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) { std::swap (re[(size_t) i], re[(size_t) j]); std::swap (im[(size_t) i], im[(size_t) j]); }
        }
        for (int len = 2; len <= n; len <<= 1)
        {
            const float ang = -2.0f * juce::MathConstants<float>::pi / (float) len;
            const float wr = std::cos (ang), wi = std::sin (ang);
            for (int i = 0; i < n; i += len)
            {
                float cr = 1.0f, ci = 0.0f;
                for (int k = 0; k < len / 2; ++k)
                {
                    const size_t a = (size_t) (i + k), b = (size_t) (i + k + len / 2);
                    const float tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
                    re[b] = re[a] - tr; im[b] = im[a] - ti; re[a] += tr; im[a] += ti;
                    const float nr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = nr;
                }
            }
        }
    }
private:
    std::vector<float> win, buf, re, im, pw, lv;
};
} // namespace panel
