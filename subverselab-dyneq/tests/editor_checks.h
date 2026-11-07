// TASK_P4c section 12 of plugin_check: the editor, through the real processor.
// Included by plugin_check.cpp after its helpers (check, setParam, Rig, ...). Every control is driven with synthesized mouse events (the editor is not on a screen);
// a key (juce::Button) is clicked with triggerClick(), because a Button only fires while a real mouse is over it.
#pragma once
#include "../Source/PanelPanels.h"
#include <set>
#include <typeinfo>
#include <map>

namespace ec
{
using juce::Point;

inline juce::MouseEvent mk (juce::Component& c, Point<float> pos, Point<float> downPos, int clicks = 1, bool dragged = false, juce::ModifierKeys m = {})
{
    auto src = juce::Desktop::getInstance().getMainMouseSource();
    return juce::MouseEvent (src, pos, m, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, juce::Time::getCurrentTime(), downPos, juce::Time::getCurrentTime(), clicks, dragged);
}
inline const juce::ModifierKeys kAlt (juce::ModifierKeys::altModifier), kShift (juce::ModifierKeys::shiftModifier), kRight (juce::ModifierKeys::rightButtonModifier);

inline void click (juce::Component& c, Point<float> p, juce::ModifierKeys m = {}, int clicks = 1) { c.mouseDown (mk (c, p, p, clicks, false, m)); c.mouseUp (mk (c, p, p, clicks, false, m)); }
inline void doubleClick (juce::Component& c, Point<float> p, juce::ModifierKeys m = {})
{
    click (c, p, m, 1);
    c.mouseDown (mk (c, p, p, 2, false, m)); c.mouseUp (mk (c, p, p, 2, false, m)); c.mouseDoubleClick (mk (c, p, p, 2, false, m));
}
/** A drag from `a` to `b` in `steps` moves. */
inline void drag (juce::Component& c, Point<float> a, Point<float> b, int steps = 8, juce::ModifierKeys m = {})
{
    c.mouseDown (mk (c, a, a, 1, false, m));
    for (int i = 1; i <= steps; ++i) c.mouseDrag (mk (c, a + (b - a) * ((float) i / (float) steps), a, 1, true, m));
    c.mouseUp (mk (c, b, a, 1, true, m));
}
inline void wheel (juce::Component& c, Point<float> p, float deltaY)
{
    juce::MouseWheelDetails w; w.deltaX = 0.0f; w.deltaY = deltaY; w.isReversed = false; w.isSmooth = false; w.isInertial = false;
    c.mouseWheelMove (mk (c, p, p), w);
}
inline Point<float> centreOf (juce::Component& c) { return { (float) c.getWidth() * 0.5f, (float) c.getHeight() * 0.5f }; }

inline std::vector<juce::String> allIds (DyneqAudioProcessor& p)
{
    std::vector<juce::String> v;
    for (auto* ap : p.getParameters()) if (auto* id = dynamic_cast<juce::AudioProcessorParameterWithID*> (ap)) v.push_back (id->paramID);
    return v;
}
inline std::vector<float> norms (DyneqAudioProcessor& p) { std::vector<float> v; for (auto* ap : p.getParameters()) v.push_back (ap->getValue()); return v; }
inline void restore (DyneqAudioProcessor& p, const std::vector<float>& v) { for (int i = 0; i < p.getParameters().size(); ++i) p.getParameters()[i]->setValueNotifyingHost (v[(size_t) i]); }
inline std::vector<juce::String> changed (DyneqAudioProcessor& p, const std::vector<float>& before)
{
    std::vector<juce::String> r;
    for (int i = 0; i < p.getParameters().size(); ++i)
        if (p.getParameters()[i]->getValue() != before[(size_t) i]) if (auto* id = dynamic_cast<juce::AudioProcessorParameterWithID*> (p.getParameters()[i])) r.push_back (id->paramID);
    return r;
}
inline bool contains (const std::vector<juce::String>& v, const juce::String& s) { return std::find (v.begin(), v.end(), s) != v.end(); }
inline int stepsAfter (DyneqAudioProcessor& p, const std::function<void()>& fn) { p.undo.clear(); fn(); return p.undo.undoSteps(); }
inline float defNorm (DyneqAudioProcessor& p, const juce::String& id) { return p.apvts.getParameter (id)->getDefaultValue(); }
inline bool atDefault (DyneqAudioProcessor& p, const juce::String& id) { return std::abs (p.apvts.getParameter (id)->getValue() - defNorm (p, id)) < 1.0e-5f; }
/** A value that is not the default (normalised 0.8 or 0.2, whichever is further away; a choice snaps to a different index). */
inline void setOffDefault (DyneqAudioProcessor& p, const juce::String& id)
{
    auto* par = p.apvts.getParameter (id);
    const float d = par->getDefaultValue();
    par->setValueNotifyingHost (d < 0.5f ? 0.8f : 0.2f);
    if (atDefault (p, id)) par->setValueNotifyingHost (d < 0.5f ? 1.0f : 0.0f);
}

using Setup = std::function<void (DyneqEditor&)>;
struct State { juce::String name; Setup apply; };

/** The states the panel is walked through: every kind of control is visible in at least one of them. */
inline std::vector<State> labStates()
{
    std::vector<State> v;
    auto bandOn = [] (DyneqEditor& ed, int b, int type, int slopeIdx, float f = 1000.0f)
    {
        auto& c = ed.ctl(); auto& p = c.proc;
        setParam (p.apvts, panel::bandId (b, "on").toRawUTF8(), 1); setParam (p.apvts, panel::bandId (b, "type").toRawUTF8(), (float) type);
        setParam (p.apvts, panel::bandId (b, "slope").toRawUTF8(), (float) slopeIdx); setParam (p.apvts, panel::bandId (b, "freq").toRawUTF8(), f);
        setParam (p.apvts, panel::bandId (b, "gain").toRawUTF8 (), 4.0f);
    };
    // LAB since the UX redesign of 11 Oct: band chips, the docked strip, the "..." popover, ADVANCED collapsed
    v.push_back ({ "no band selected", [] (DyneqEditor& ed) { ed.ctl().selectLab (-1); ed.closeOverlay(); ed.setDynOpen (false); ed.setAdvancedOpen (false); } });
    v.push_back ({ "bell, selected", [=] (DyneqEditor& ed) { bandOn (ed, 0, dyneq::kBell, 0); ed.ctl().selectLab (0); ed.setDynOpen (false); ed.closeOverlay(); ed.setAdvancedOpen (false); } });
    for (int b = 0; b < 6; ++b)
        v.push_back ({ "dynamic bell band " + juce::String (b + 1) + ", selected, '...' open", [=] (DyneqEditor& ed)
                       { bandOn (ed, b, dyneq::kBell, 0); setParam (ed.ctl().proc.apvts, panel::bandId (b, "dyn").toRawUTF8(), 1); setParam (ed.ctl().proc.apvts, panel::bandId (b, "range").toRawUTF8(), -6.0f);
                         ed.ctl().selectLab (b); ed.setDynOpen (true); ed.closeOverlay(); ed.setAdvancedOpen (false); } });
    for (int b = 0; b < 6; ++b)
        v.push_back ({ "low cut 36 band " + juce::String (b + 1) + ", selected", [=] (DyneqEditor& ed) { bandOn (ed, b, dyneq::kLowCut, 3); ed.ctl().selectLab (b); ed.setDynOpen (false); ed.closeOverlay(); } });
    v.push_back ({ "notch, selected", [=] (DyneqEditor& ed) { bandOn (ed, 2, dyneq::kNotch, 0); ed.ctl().selectLab (2); ed.closeOverlay(); } });
    v.push_back ({ "high shelf, selected", [=] (DyneqEditor& ed) { bandOn (ed, 5, dyneq::kHighShelf, 0, 6000.0f); ed.ctl().selectLab (5); ed.closeOverlay(); } });
    v.push_back ({ "band 2 switched off, selected (hollow node)", [] (DyneqEditor& ed) { setParam (ed.ctl().proc.apvts, "b2_on", 0); ed.ctl().selectLab (1); ed.closeOverlay(); ed.setDynOpen (false); } });
    v.push_back ({ "ADVANCED", [] (DyneqEditor& ed) { ed.ctl().selectLab (-1); ed.closeOverlay(); ed.setDynOpen (false); ed.setAdvancedOpen (true); } });
    for (int m : { panel::kTarget, panel::kCollide, panel::kSelf })
        v.push_back ({ m == panel::kTarget ? "TARGET overlay" : m == panel::kCollide ? "COLLIDE overlay" : "SELF overlay", [m] (DyneqEditor& ed) { ed.setAdvancedOpen (false); ed.closeOverlay(); ed.toggleOverlay (m); } });
    return v;
}
inline void settle (DyneqEditor& ed) { ed.closeChooser(); ed.refreshFace(); ed.tickForSnapshot(); }

/** The pink noise of the analyzer tests. */
inline Audio pink (double secs, double rmsDbfs, double sr = 48000.0)
{
    const size_t n = (size_t) (secs * sr); Audio a (2, std::vector<float> (n)); juce::Random rnd (31337);
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0; double ss = 0;
    for (size_t i = 0; i < n; ++i)
    {
        const double w = (double) rnd.nextFloat() * 2.0 - 1.0;
        b0 = 0.99886 * b0 + w * 0.0555179; b1 = 0.99332 * b1 + w * 0.0750759; b2 = 0.96900 * b2 + w * 0.1538520; b3 = 0.86650 * b3 + w * 0.3104856; b4 = 0.55000 * b4 + w * 0.5329522; b5 = -0.7616 * b5 - w * 0.0168980;
        const double v = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362; b6 = w * 0.115926;
        a[0][i] = a[1][i] = (float) v; ss += v * v;
    }
    const float g = (float) (std::pow (10.0, rmsDbfs / 20.0) / std::sqrt (ss / (double) n));
    for (auto& c : a) for (auto& x : c) x *= g;
    return a;
}

/** The HF residue and the step of a switch (the A17 / A18 method of tools/measure.py, here through the real processor): 8th-order Butterworth high-pass at 4 kHz, zero phase. */
inline double hfResidueDbc (const std::vector<float>& y, int from, int to, double peak, double sr = 48000.0)
{
    std::vector<double> x (y.begin(), y.end());
    struct Bq { double b0, b1, b2, a1, a2; };
    std::vector<Bq> sec;
    const double w0 = 2.0 * juce::MathConstants<double>::pi * 4000.0 / sr, cs = std::cos (w0), sn = std::sin (w0);
    for (int k = 0; k < 4; ++k)
    {
        const double q = 1.0 / (2.0 * std::sin (juce::MathConstants<double>::pi * (2.0 * k + 1.0) / 16.0)), al = sn / (2.0 * q), a0 = 1.0 + al;
        sec.push_back ({ (1.0 + cs) / 2.0 / a0, -(1.0 + cs) / a0, (1.0 + cs) / 2.0 / a0, -2.0 * cs / a0, (1.0 - al) / a0 });
    }
    auto run = [&] (std::vector<double>& d)
    {
        for (auto& s : sec) { double z1 = 0, z2 = 0; for (auto& v : d) { const double o = s.b0 * v + z1; z1 = s.b1 * v - s.a1 * o + z2; z2 = s.b2 * v - s.a2 * o; v = o; } }
    };
    run (x); std::reverse (x.begin(), x.end()); run (x); std::reverse (x.begin(), x.end());
    double m = 0.0; for (int i = from; i < to; ++i) m = std::max (m, std::abs (x[(size_t) i]));
    return 20.0 * std::log10 (m / peak + 1e-30);
}
} // namespace ec

//==============================================================================
static void runEditorChecks (const Audio& beat, const Audio& vocal, const juce::File& dir)
{
    using namespace ec;
    (void) dir;
    std::printf ("\n---- 12. P4c: the analyzer, the defaults, the nodes, the panels ----\n");

    //------------------------------------------------------------------------------------------------ 12a. the analyzer shows real audio
    {
        Rig r (2, true, 48000.0, 512, {});
        check (juce::roundToInt (r.proc.apvts.getRawParameterValue ("analyzer")->load()) == 2, "analyzer default on a new instance: POST only (one trace, UX redesign 11 Oct)");
        setParam (r.proc.apvts, "analyzer", 3.0f);   // the checks below want both traces
        DyneqEditor ed (r.proc); ed.refreshFace(); ed.tickForSnapshot();
        auto& sp = ed.spectrum();
        check (panel::Spectrum::range (sp.pre) < 0.5f && panel::Spectrum::peak (sp.pre) <= -119.9f, "analyzer before any audio: every trace at the floor (-120 dB)");
        const Audio p = pink (0.2, -20.0);   // 200 ms of pink noise
        r.render (p, &p, 512);
        for (int i = 0; i < 3; ++i) ed.tickForSnapshot();
        const float rPre = panel::Spectrum::range (sp.pre), rPost = panel::Spectrum::range (sp.post), rKey = panel::Spectrum::range (sp.key);
        check (rPre > 25.0f && rPost > 25.0f && panel::Spectrum::peak (sp.pre) > -90.0f,
               "analyzer, pink noise through the real processor: after 200 ms of audio the PRE and POST traces are not flat (range " + juce::String (rPre, 1) + " / " + juce::String (rPost, 1) + " dB, PRE peak " + juce::String (panel::Spectrum::peak (sp.pre), 1) + " dBFS per bin)");
        check (rKey > 25.0f, "analyzer: the sidechain KEY trace follows the key (COLLIDE on, key bus connected): range " + juce::String (rKey, 1) + " dB");
        // the shape: pink noise falls with frequency (about -3 dB per octave): 100 Hz is above 5 kHz by 8..25 dB
        auto at = [&] (double hz) { return sp.pre.db[(int) std::round (std::log (hz / 20.0) / std::log (1000.0) * (panel::kCurveN - 1))]; };
        check (at (100.0) - at (5000.0) > 8.0f && at (100.0) - at (5000.0) < 30.0f, "analyzer shape: pink noise falls with frequency (100 Hz is " + juce::String (at (100.0) - at (5000.0), 1) + " dB above 5 kHz)");
        // silence falls to the floor
        const Audio z (2, std::vector<float> (48000 * 2, 0.0f));
        r.render (z, &z, 512);
        for (int i = 0; i < 90; ++i) ed.tickForSnapshot();
        check (panel::Spectrum::peak (sp.pre) <= -119.9f && panel::Spectrum::peak (sp.post) <= -119.9f && panel::Spectrum::peak (sp.key) <= -119.9f, "analyzer, silence: every trace falls to the floor within 90 frames (PRE " + juce::String (panel::Spectrum::peak (sp.pre), 1) + ", POST " + juce::String (panel::Spectrum::peak (sp.post), 1) + ", KEY " + juce::String (panel::Spectrum::peak (sp.key), 1) + " dB)");
        // the analyzer modes compute only what they show
        r.render (p, &p, 512);
        for (int mode = 0; mode < 4; ++mode)
        {
            setParam (r.proc.apvts, "analyzer", (float) mode);
            for (int i = 0; i < 4; ++i) ed.tickForSnapshot();
            const bool preOn = panel::Spectrum::range (sp.pre) > 25.0f, postOn = panel::Spectrum::range (sp.post) > 25.0f;
            check (preOn == (mode == 1 || mode == 3) && postOn == (mode == 2 || mode == 3), "analyzer mode " + juce::String (mode) + " (0 OFF, 1 PRE, 2 POST, 3 PRE+POST): PRE trace " + (preOn ? "live" : "off") + ", POST trace " + (postOn ? "live" : "off"));
        }
        setParam (r.proc.apvts, "analyzer", 3.0f);
        // no sidechain bus: no KEY trace
        Rig r2 (2, false, 48000.0, 512, {}); setParam (r2.proc.apvts, "analyzer", 3.0f); DyneqEditor ed2 (r2.proc); r2.render (p, nullptr, 512); for (int i = 0; i < 4; ++i) ed2.tickForSnapshot();
        check (panel::Spectrum::range (ed2.spectrum().key) < 0.5f && panel::Spectrum::range (ed2.spectrum().pre) > 25.0f, "analyzer without a sidechain bus: no KEY trace, PRE still live");
        // the real timer: at least 30 analyzer frames per second
        {
            DyneqAudioProcessor pr; DyneqEditor et (pr);
            const int f0 = et.spectrumFrames(); const double t0 = juce::Time::getMillisecondCounterHiRes();
            juce::MessageManager::getInstance()->runDispatchLoopUntil (1000);
            const double dt = (juce::Time::getMillisecondCounterHiRes() - t0) * 0.001, fps = (double) (et.spectrumFrames() - f0) / dt;
            check (fps >= 30.0, "analyzer frame rate with the real editor timer: " + juce::String (fps, 1) + " frames per second over " + juce::String (dt, 2) + " s (limit 30)");
        }
        // the analyzer never touches the audio: ANALYZER OFF and PRE+POST give the same bits, and the editor open or closed changes nothing
        const Audio mat = slice (beat, 0, 48000 * 3);
        const auto o0 = runPlugin (mat, nullptr, false, 512, { { "analyzer", 0 }, { "b1_on", 1 }, { "b1_gain", 6 } });
        const auto o3 = runPlugin (mat, nullptr, false, 512, { { "analyzer", 3 }, { "b1_on", 1 }, { "b1_gain", 6 } });
        check (maxDiff (o0, o3) == 0.0, "ANALYZER OFF vs PRE+POST: the output is bit-identical (the analyzer is a view, not a processor)");
    }

    //------------------------------------------------------------------------------------------------ 12b. every parameter has a control
    {
        DyneqAudioProcessor proc; DyneqEditor ed (proc); settle (ed);
        std::set<juce::String> seen; int controls = 0, unbound = 0, badId = 0;
        for (auto& st : labStates())
        {
            st.apply (ed); settle (ed);
            ed.forEachControl ([&] (juce::Component&, PanelControl& pc)
            {
                ++controls;
                if (pc.boundParams().isEmpty() && pc.uiAction().isEmpty()) ++unbound;
                for (auto& id : pc.boundParams()) if (proc.apvts.getParameter (id) == nullptr) ++badId;
                if (pc.uiAction() == "LAB DISPLAY" || pc.uiAction() == "DELETE BAND") return;   // the display is checked in 12d; DELETE resets a band, it is not the band's control; here only the panel's own controls count
                for (auto& id : pc.boundParams()) seen.insert (id);
            });
        }
        juce::String missing; int nMissing = 0;
        for (auto& id : allIds (proc)) if (seen.find (id) == seen.end()) { missing << id << " "; ++nMissing; }
        check (nMissing == 0 && unbound == 0 && badId == 0, "LAB: every one of the plug-in's " + juce::String ((int) allIds (proc).size()) + " parameters has a control (a key, knob or value box, not only the display), "
               + juce::String (controls) + " control instances over " + juce::String ((int) labStates().size()) + " states, " + juce::String (unbound) + " unbound, " + juce::String (badId) + " unknown ids",
               nMissing > 0 ? "MISSING: " + missing : juce::String());
    }

    //------------------------------------------------------------------------------------------------ 12c. double-click and Alt-click return every knob and value box to its default, as one undo step
    {
        DyneqAudioProcessor proc; DyneqEditor ed (proc); settle (ed);
        int tested = 0, bad = 0, altTested = 0; juce::String firstBad; std::set<juce::String> covered;
        for (auto& st : labStates())
        {
            st.apply (ed); settle (ed);
            std::vector<std::pair<juce::Component*, PanelControl*>> list;
            ed.forEachControl ([&] (juce::Component& c, PanelControl& pc) { if (! pc.resetParams().isEmpty()) list.push_back ({ &c, &pc }); });
            for (auto& [comp, pc] : list)
            {
                const auto ids = pc->resetParams();
                if (ids.isEmpty()) continue;
                for (int variant = 0; variant < 2; ++variant)
                {
                    for (auto& id : ids) setOffDefault (proc, id);
                    std::vector<float> before; for (auto& id : ids) before.push_back (proc.apvts.getParameter (id)->getValue());
                    const auto all0 = norms (proc);
                    const int steps = stepsAfter (proc, [&]
                    {
                        if (variant == 0) doubleClick (*comp, centreOf (*comp)); else click (*comp, centreOf (*comp), kAlt);
                    });
                    bool atDef = true; for (auto& id : ids) atDef = atDef && atDefault (proc, id);
                    const auto ch = changed (proc, all0);
                    bool onlyOwn = true; for (auto& c2 : ch) onlyOwn = onlyOwn && contains (std::vector<juce::String> (ids.begin(), ids.end()), c2);
                    // one undo step takes it back to the off-default value
                    bool undone = false; if (proc.undo.canUndo()) { proc.undo.undo(); undone = true; for (size_t k = 0; k < ids.size(); ++k) undone = undone && proc.apvts.getParameter (ids[(int) k])->getValue() == before[k]; }
                    if (variant == 0) ++tested; else ++altTested;
                    if (! (atDef && onlyOwn && steps == 1 && undone))
                    {
                        ++bad; if (firstBad.isEmpty()) firstBad = st.name + ": " + ids.joinIntoString (",") + (variant ? " (Alt-click)" : " (double-click)") + " atDefault=" + juce::String ((int) atDef) + " onlyOwn=" + juce::String ((int) onlyOwn) + " steps=" + juce::String (steps) + " undone=" + juce::String ((int) undone);
                    }
                    for (auto& id : ids) covered.insert (id);
                    for (auto& id : ids) proc.apvts.getParameter (id)->setValueNotifyingHost (defNorm (proc, id));
                    settle (ed);
                }
            }
        }
        check (tested > 20 && bad == 0, "LAB: a double-click (" + juce::String (tested) + " controls) and an Alt-click (" + juce::String (altTested) + ") on every knob and value box returns its parameter to the default, changes nothing else, and is exactly one undo step that one undo reverts (" + juce::String ((int) covered.size()) + " distinct parameters, " + juce::String (bad) + " wrong)", firstBad);
        // list the continuous parameters no knob / box covers
        {
            juce::String notCovered;
            for (auto& id : allIds (proc))
            {
                auto* par = proc.apvts.getParameter (id);
                const bool numeric = dynamic_cast<juce::AudioParameterFloat*> (par) != nullptr;
                if (id == "targetPart") continue;   // the part is chosen from the TARGET list (the slider's value is the part's centre); it has no knob by design
                if (numeric && covered.find (id) == covered.end()) notCovered << id << " ";
            }
            check (notCovered.isEmpty(), "LAB: every continuous parameter (TARGET PART excepted: it is chosen from the instrument / part list) has a knob and a value box that reset on double-click", notCovered.isEmpty() ? juce::String() : "no knob/box: " + notCovered);
        }
    }
    // typed entry: one click into a value box, Return commits, junk changes nothing, Escape cancels; one undo step
    {
        DyneqAudioProcessor proc; DyneqEditor ed (proc); settle (ed);
        setParam (proc.apvts, "b1_on", 1); ed.ctl().selectLab (0); settle (ed);
        panelui::ValueWin* freqBox = nullptr;
        ed.forEachControl ([&] (juce::Component& c, PanelControl& pc) { if (auto* vw = dynamic_cast<panelui::ValueWin*> (&c)) if (pc.boundParams().size() == 1 && pc.boundParams()[0] == "b1_freq") freqBox = vw; });
        bool ok = freqBox != nullptr;
        if (ok)
        {
            click (*freqBox, centreOf (*freqBox));   // ONE click opens the box
            const bool opened = freqBox->isEditing();
            const int st = stepsAfter (proc, [&] { freqBox->typeAndCommit ("2.5k"); });
            const bool v1 = std::abs (proc.apvts.getRawParameterValue ("b1_freq")->load() - 2500.0f) < 1.0f;
            freqBox->typeAndCommit ("abc"); const bool v2 = std::abs (proc.apvts.getRawParameterValue ("b1_freq")->load() - 2500.0f) < 1.0f;
            freqBox->typeAndCommit ("99999 Hz"); const bool v3 = std::abs (proc.apvts.getRawParameterValue ("b1_freq")->load() - 20000.0f) < 1.0f;   // clamped to the range
            click (*freqBox, centreOf (*freqBox)); const bool again = freqBox->isEditing(); freqBox->cancelEdit(); const bool closed = ! freqBox->isEditing();
            ok = opened && st == 1 && v1 && v2 && v3 && again && closed;
        }
        check (ok, "typed entry: a single click opens the value box, '2.5k' commits 2500 Hz as one undo step, text that is not a value changes nothing, a value past the range is clamped, Escape closes");
        // a drag on a value box moves the value (no editor opens), one undo step
        if (freqBox != nullptr)
        {
            setParam (proc.apvts, "b1_freq", 1000.0f);
            const int st = stepsAfter (proc, [&] { drag (*freqBox, centreOf (*freqBox), centreOf (*freqBox) + Point<float> (0.0f, -40.0f)); });
            check (! freqBox->isEditing() && proc.apvts.getRawParameterValue ("b1_freq")->load() > 1100.0f && st == 1, "a vertical drag on a value box moves the value upwards without opening the editor, one undo step", "FREQ " + juce::String (proc.apvts.getRawParameterValue ("b1_freq")->load(), 1) + " Hz");
        }
    }

    //------------------------------------------------------------------------------------------------ 12d. the display: create, drag, wheel, menu, Alt-click, delete
    {
        DyneqAudioProcessor proc; DyneqEditor ed (proc); settle (ed);
        auto& c = ed.ctl();
        juce::Component& scr = ed.labScreen();
        auto pt = [&] (double hz, double db) { return ed.testLabPoint (hz, db); };
        auto nodeLocal = [&] (int b) { return ed.testNodePosition (b) - scr.getPosition().toFloat(); };
        auto raw = [&] (const char* id) { return proc.apvts.getRawParameterValue (id)->load(); };
        const juce::String F = "LAB";
        // create
        const int stepsNew = stepsAfter (proc, [&] { doubleClick (scr, pt (1000.0, 6.0)); });
        settle (ed);
        check (c.bandOn (0) && c.bandType (0) == dyneq::kBell && std::abs (std::log2 (raw ("b1_freq") / 1000.0f)) < 0.05f && std::abs (raw ("b1_gain") - 6.0f) < 0.4f && stepsNew == 1,
               F + " display: a double-click on empty glass creates a BELL at the pointer's frequency and gain as one undo step", "band 1: " + juce::String (c.bandTypeName (0)) + " " + juce::String (raw ("b1_freq"), 1) + " Hz " + juce::String (raw ("b1_gain"), 2) + " dB");
        check (c.selectedBand == 0, F + " display: the new band is the selected band");
        doubleClick (scr, pt (21.0, 0.0)); settle (ed);
        const bool lowCut = c.bandOn (1) && c.bandType (1) == dyneq::kLowCut && c.bandSlopeDb (1) == 24;
        doubleClick (scr, pt (19000.0, 0.0)); settle (ed);
        const bool highCut = c.bandOn (2) && c.bandType (2) == dyneq::kHighCut && c.bandSlopeDb (2) == 24;
        check (lowCut && highCut, F + " display: a double-click at the extremes creates a LOW CUT (21 Hz) or a HIGH CUT (19 kHz), 24 dB/oct");
        // the first band: drag the node to 2 kHz / -4 dB
        {
            const auto n0 = nodeLocal (0);
            const auto q0 = raw ("b1_q");
            const int st = stepsAfter (proc, [&] { drag (scr, n0, pt (2000.0, -4.0), 10); });
            settle (ed);
            check (n0.x > 0 && std::abs (std::log2 (raw ("b1_freq") / 2000.0f)) < 0.12f && std::abs (raw ("b1_gain") + 4.0f) < 1.2f && raw ("b1_q") == q0 && st == 1,
                   F + " display: dragging a node moves frequency and gain (one undo step, Q untouched)", "band 1 is now " + juce::String (raw ("b1_freq"), 0) + " Hz " + juce::String (raw ("b1_gain"), 2) + " dB");
        }
        // Shift = fine (x0.2), Alt = one axis
        {
            setParam (proc.apvts, "b1_freq", 1000.0f); setParam (proc.apvts, "b1_gain", 0.0f); settle (ed);
            const auto n0 = nodeLocal (0);
            drag (scr, n0, n0 + Point<float> (100.0f, 0.0f), 10); const float freqPlain = raw ("b1_freq");
            setParam (proc.apvts, "b1_freq", 1000.0f); setParam (proc.apvts, "b1_gain", 0.0f); settle (ed);
            const auto n1 = nodeLocal (0);
            drag (scr, n1, n1 + Point<float> (100.0f, 0.0f), 10, kShift); const float freqFine = raw ("b1_freq");
            const double moved = std::abs (std::log2 (freqPlain / 1000.0)), movedFine = std::abs (std::log2 (freqFine / 1000.0));
            check (moved > 0.05 && movedFine > 0.0 && std::abs (movedFine / moved - 0.2) < 0.08, F + " display: Shift-drag moves the node at one fifth of the speed", juce::String (movedFine, 4) + " / " + juce::String (moved, 4) + " = " + juce::String (movedFine / std::max (moved, 1e-9), 3));
            setParam (proc.apvts, "b1_freq", 1000.0f); setParam (proc.apvts, "b1_gain", 0.0f); settle (ed);
            const auto n2 = nodeLocal (0);
            const float fBefore = raw ("b1_freq"), gBefore = raw ("b1_gain");
            drag (scr, n2, n2 + Point<float> (60.0f, -18.0f), 10, kAlt);   // mostly horizontal: one axis only
            const bool oneAxis = (raw ("b1_freq") == fBefore) != (raw ("b1_gain") == gBefore) || (raw ("b1_freq") != fBefore && std::abs (raw ("b1_gain") - gBefore) < 0.05f);
            check (oneAxis, F + " display: Alt-drag moves along one axis only", "freq " + juce::String (fBefore, 1) + " -> " + juce::String (raw ("b1_freq"), 1) + ", gain " + juce::String (gBefore, 2) + " -> " + juce::String (raw ("b1_gain"), 2));
        }
        // wheel over a node = Q
        {
            setParam (proc.apvts, "b1_freq", 1000.0f); setParam (proc.apvts, "b1_gain", 3.0f); setParam (proc.apvts, "b1_q", 1.0f); settle (ed);
            const auto n0 = nodeLocal (0);
            const int st = stepsAfter (proc, [&] { wheel (scr, n0, 1.0f); });
            const float qUp = raw ("b1_q");
            wheel (scr, n0, -1.0f); wheel (scr, n0, -1.0f); const float qDown = raw ("b1_q");
            const float q1 = raw ("b1_q"); wheel (scr, pt (300.0, -12.0), 1.0f);   // over empty glass: nothing
            check (qUp > 1.3f && qDown < qUp && std::abs (raw ("b1_q") - q1) < 1e-6f && st == 1, F + " display: the wheel over a node changes its Q (up = narrower), over empty glass it does nothing", "Q 1 -> " + juce::String (qUp, 3) + " -> " + juce::String (qDown, 3));
        }
        // notch / cut: a vertical drag = Q
        {
            setParam (proc.apvts, "b1_type", (float) dyneq::kNotch); setParam (proc.apvts, "b1_q", 1.0f); setParam (proc.apvts, "b1_freq", 1000.0f); settle (ed);
            const auto n0 = nodeLocal (0); const float qb = raw ("b1_q"), fb = raw ("b1_freq");
            const Point<float> to = n0 + Point<float> (0.0f, -30.0f);
            drag (scr, n0, to, 8);
            check (raw ("b1_q") > qb * 1.2f && std::abs (std::log2 (raw ("b1_freq") / fb)) < 0.1f, F + " display: on a notch (and on the cuts) a vertical drag changes Q", "Q " + juce::String (qb, 2) + " -> " + juce::String (raw ("b1_q"), 2));
            setParam (proc.apvts, "b1_type", (float) dyneq::kBell); settle (ed);
        }
        // right-click menu: shape, slope (cuts), on/off, delete
        {
            setParam (proc.apvts, "b1_type", (float) dyneq::kBell); setParam (proc.apvts, "b1_gain", 3.0f); settle (ed);
            const auto n0 = nodeLocal (0);
            scr.mouseDown (mk (scr, n0, n0, 1, false, kRight));
            const bool open = ed.chooserOpen();
            int heads = 0; for (int i = 0; i < ed.chooserCount(); ++i) if (ed.chooserHeading (i)) ++heads;
            auto pick = [&] (const juce::String& t)
            {
                if (! ed.chooserOpen()) { const auto nl = nodeLocal (0); scr.mouseDown (mk (scr, nl, nl, 1, false, kRight)); }
                int idx = -1; for (int i = 0; i < ed.chooserCount(); ++i) if (ed.chooserText (i) == t && ! ed.chooserHeading (i)) idx = i;
                if (idx < 0) { ed.closeChooser(); return -1; }
                const int st = stepsAfter (proc, [&] { ed.chooserPick (idx); }); settle (ed); return st;
            };
            const bool noSlopeForBell = [&] { for (int i = 0; i < ed.chooserCount(); ++i) if (ed.chooserText (i) == "SLOPE") return false; return true; }();
            const int s1 = pick ("HIGH CUT"); const bool hc = c.bandType (0) == dyneq::kHighCut;
            const int s2 = pick ("48 dB/oct"); const bool sl = c.bandSlopeDb (0) == 48;
            const int s3 = pick ("SWITCH OFF"); const bool off = ! c.bandOn (0);
            if (! c.bandOn (0)) setParam (proc.apvts, "b1_on", 1);   // switched off: on again for the next picks
            settle (ed);
            const int s4 = pick ("LOW SHELF"); const bool ls = c.bandType (0) == dyneq::kLowShelf;
            setParam (proc.apvts, "b1_gain", 5.0f); setParam (proc.apvts, "b1_q", 2.0f); settle (ed);
            const int s5 = pick ("DELETE"); const bool del = ! c.bandOn (0) && atDefault (proc, "b1_gain") && atDefault (proc, "b1_q") && atDefault (proc, "b1_type") && atDefault (proc, "b1_freq") && c.selectedBand == -1;
            const bool undoRestores = proc.undo.undo() && c.bandOn (0) && std::abs (raw ("b1_gain") - 5.0f) < 1e-4f && std::abs (raw ("b1_q") - 2.0f) < 1e-3f && c.bandType (0) == dyneq::kLowShelf;
            check (open && heads == 2 && noSlopeForBell && s1 == 1 && hc && s2 == 1 && sl && s3 == 1 && off && s4 == 1 && ls && s5 == 1 && del && undoRestores,
                   F + " display: the right-click menu (SHAPE, SLOPE only on cuts, BAND) writes the right parameters: HIGH CUT, 48 dB/oct, SWITCH OFF, LOW SHELF, DELETE (the band returns to its defaults, one step each, one undo restores it)",
                   "open=" + juce::String ((int) open) + " heads=" + juce::String (heads) + " steps " + juce::String (s1) + juce::String (s2) + juce::String (s3) + juce::String (s4) + juce::String (s5) + " hc=" + juce::String ((int) hc) + " sl=" + juce::String ((int) sl) + " off=" + juce::String ((int) off) + " ls=" + juce::String ((int) ls) + " del=" + juce::String ((int) del) + " undo=" + juce::String ((int) undoRestores));
        }
        // Alt-click on a node toggles the band; a double-click opens typing in the strip's FREQ box (Pro-Q)
        {
            setParam (proc.apvts, "b1_on", 1); setParam (proc.apvts, "b1_type", (float) dyneq::kBell); setParam (proc.apvts, "b1_gain", 3.0f); setParam (proc.apvts, "b1_freq", 1000.0f); settle (ed);
            const auto n0 = nodeLocal (0);
            const int st = stepsAfter (proc, [&] { scr.mouseDown (mk (scr, n0, n0, 1, false, kAlt)); scr.mouseUp (mk (scr, n0, n0, 1, false, kAlt)); });
            const bool off = ! c.bandOn (0); settle (ed);
            setParam (proc.apvts, "b1_on", 1); settle (ed);
            const int st2 = stepsAfter (proc, [&] { doubleClick (scr, nodeLocal (0)); });
            {
                panelui::ValueWin* fb = nullptr;
                ed.forEachControl ([&] (juce::Component& cc, PanelControl& pc) { if (auto* vw = dynamic_cast<panelui::ValueWin*> (&cc)) if (pc.boundParams().size() == 1 && pc.boundParams()[0] == "b1_freq") fb = vw; });
                const bool typing = fb != nullptr && fb->isEditing(); if (fb != nullptr) fb->cancelEdit();
                check (off && st == 1 && c.bandOn (0) && st2 == 0 && typing && c.selectedBand == 0, "LAB display: an Alt-click on a node switches the band off (one undo step); a double-click on a node selects it and opens typing in the strip's FREQ box, changing nothing");
            }
            setParam (proc.apvts, "b1_on", 1); settle (ed);
        }
        // a click on empty glass deselects
        {
            c.selectLab (0); settle (ed);
            click (scr, pt (300.0, -12.0), {}, 1); settle (ed);
            const bool closed = c.selectedBand == -1;
            c.selectLab (0); settle (ed);
            const bool del = ed.keyPressed (juce::KeyPress (juce::KeyPress::deleteKey)); settle (ed);
            check (closed && del && ! c.bandOn (0) && c.selectedBand == -1, "LAB display: a click on empty glass deselects; the Delete key deletes the selected band");
        }
        // all six bands in use: a seventh double-click creates nothing
        {
            for (int i = 0; i < 6; ++i) setParam (proc.apvts, panel::bandId (i, "on").toRawUTF8(), 1);
            settle (ed); const auto before = norms (proc);
            doubleClick (scr, pt (150.0, -10.0)); settle (ed);
            check (changed (proc, before).empty(), F + " display: with six bands in use a double-click on empty glass creates nothing");
        }
    }

    //------------------------------------------------------------------------------------------------ 12e. every visible control changes only its own parameter
    {
        DyneqAudioProcessor proc; DyneqEditor ed (proc); settle (ed);
        int n = 0, bad = 0, knobsChanged = 0, knobs = 0; juce::String firstBad; std::set<juce::String> pressed;
        for (auto& st : labStates())
        {
            st.apply (ed); settle (ed);
            const auto base = norms (proc);
            std::vector<std::pair<juce::Component*, PanelControl*>> list;
            ed.forEachControl ([&] (juce::Component& c, PanelControl& pc) { list.push_back ({ &c, &pc }); });
            for (auto& [comp, pc] : list)
            {
                const auto ua = pc->uiAction();
                if (ua == "LAB DISPLAY" || ua.startsWith ("SIZE") || ua == "ASSIST OVERLAY" || ua == "ADVANCED") continue;   // the display has its own tests; SIZE opens a native menu
                const auto bound = pc->boundParams();
                const std::vector<juce::String> boundV (bound.begin(), bound.end());
                // the gesture
                if (dynamic_cast<panelui::KnobComp*> (comp) != nullptr || dynamic_cast<panelui::ValueWin*> (comp) != nullptr)
                {
                    if (auto* vw = dynamic_cast<panelui::ValueWin*> (comp)) if (vw->resetParams().isEmpty()) continue;   // IN / OUT read-outs
                    drag (*comp, centreOf (*comp), centreOf (*comp) + Point<float> (0.0f, defNorm (proc, bound[0]) >= 0.5f ? 80.0f : -80.0f), 8);
                }
                else if (auto* b = dynamic_cast<juce::Button*> (comp))
                {
                    if (dynamic_cast<panelui::PlateKeyButton*> (b) != nullptr && ua.isEmpty() && bound.size() == 1 && bound[0] == "delta") continue;   // DELTA is a hold: press and release leave it off
                    b->triggerClick();
                }
                else { comp->mouseDown (mk (*comp, centreOf (*comp), centreOf (*comp))); comp->mouseUp (mk (*comp, centreOf (*comp), centreOf (*comp))); }
                ++n; for (auto& id : bound) pressed.insert (id);
                const auto ch = changed (proc, base);
                bool only = true; for (auto& id : ch) only = only && contains (boundV, id);
                const bool isKnob = dynamic_cast<panelui::KnobComp*> (comp) != nullptr || (dynamic_cast<panelui::ValueWin*> (comp) != nullptr);
                if (isKnob) { ++knobs; if (contains (ch, bound[0]) && only) ++knobsChanged; }   // a box moves its own parameter (RANGE also writes its band's DYNAMIC switch, which it declares)
                if (! only) { ++bad; if (firstBad.isEmpty()) firstBad = st.name + ": '" + juce::String (bound.joinIntoString (",")) + "' " + ua + " changed " + (ch.empty() ? juce::String() : ch[0]); }
                restore (proc, base); settle (ed);
            }
        }
        check (n > 40 && bad == 0 && knobsChanged == knobs, "LAB: every visible control is bound and changes only its own parameters (" + juce::String (n) + " controls driven over " + juce::String ((int) labStates().size()) + " states, " + juce::String (bad) + " touched something else); every knob and value box (" + juce::String (knobs) + ") moves exactly its own parameter (" + juce::String (knobsChanged) + ")", firstBad);
    }

    //------------------------------------------------------------------------------------------------ 12f. open / close and state
    {   // open / close 20 times with everything open: nothing is left behind
        DyneqAudioProcessor proc; const int before = DyneqEditor::liveObjects();
        for (int i = 0; i < 40; ++i)
        {
            DyneqEditor ed (proc); ed.ctl().selectLab (0); setParam (proc.apvts, "b1_on", 1); ed.setDynOpen (true); ed.toggleOverlay (panel::kTarget); ed.setAdvancedOpen (true);
            ed.openItems (ed.ctl().menuItems (0), { 300, 300 }); ed.tickForSnapshot(); ed.tickForSnapshot();
        }
        check (DyneqEditor::liveObjects() == before, "editor open / close 40 times with the band panel, DYNAMIC, an overlay, ADVANCED and the menu open: no live panel object left (" + juce::String (before) + " -> " + juce::String (DyneqEditor::liveObjects()) + ")");
    }

    //------------------------------------------------------------------------------------------------ 12g. state round trip, bit-exact
    {
        juce::Random rnd (2024); int bad = 0; const int N = 60;
        for (int k = 0; k < N; ++k)
        {
            DyneqAudioProcessor a, b;
            for (auto* ap : a.getParameters())
            {
                float v = k % 4 == 0 ? (rnd.nextBool() ? 1.0f : 0.0f) : rnd.nextFloat();   // a quarter of the states are on the range ends; a host sets a switch to 0 or 1 and a choice to one of its steps
                if (dynamic_cast<juce::AudioParameterBool*> (ap) != nullptr) v = v < 0.5f ? 0.0f : 1.0f;
                else if (auto* ch = dynamic_cast<juce::AudioParameterChoice*> (ap)) v = ch->convertTo0to1 ((float) juce::roundToInt (v * (float) (ch->choices.size() - 1)));
                ap->setValueNotifyingHost (v);
            }
            a.editorScalePct = k % 3 == 0 ? 75 : k % 3 == 1 ? 85 : 100; a.uiMode = k % 9;
            juce::MemoryBlock mb; a.getStateInformation (mb); b.setStateInformation (mb.getData(), (int) mb.getSize());
            if (snapDiff (snapOf (a), snapOf (b), bad == 0 ? &a : nullptr) != 0 || a.editorScalePct != b.editorScalePct || a.uiMode != b.uiMode) ++bad;
        }
        check (bad == 0, "state round trip: " + juce::String (N) + " random states (every parameter, the panel's size and mode), getStateInformation -> setStateInformation on a new processor: every parameter bit-identical (normalised and plain), " + juce::String (bad) + " different");
    }

    //------------------------------------------------------------------------------------------------ 12i. LAB, UX redesign of 11 Oct (docs/ui/UX_REDESIGN_PROQ_EQ8.md 5.1): band chips, the docked strip, selection
    {
        std::printf ("\n---- 12i. LAB redesign: band chips, docked strip, selection (spec 5.1) ----\n");
        auto findCtlBy = [] (DyneqEditor& ed, std::function<bool (juce::Component&, PanelControl&)> pred) { juce::Component* r = nullptr; ed.forEachControl ([&] (juce::Component& c, PanelControl& pc) { if (r == nullptr && pred (c, pc)) r = &c; }); return r; };
        auto chipOf = [&] (DyneqEditor& ed, int b) { return findCtlBy (ed, [b] (juce::Component&, PanelControl& pc) { return pc.uiAction() == "BAND CHIP" && pc.boundParams()[0] == panel::bandId (b, "on"); }); };
        auto boxOf = [&] (DyneqEditor& ed, const juce::String& id) { return dynamic_cast<panelui::ValueWin*> (findCtlBy (ed, [id] (juce::Component& c, PanelControl& pc) { return dynamic_cast<panelui::ValueWin*> (&c) != nullptr && pc.boundParams().size() >= 1 && pc.boundParams()[0] == id; })); };
        auto assistOf = [&] (DyneqEditor& ed, int i) { int k = 0; juce::Component* r = nullptr; ed.forEachControl ([&] (juce::Component& c, PanelControl& pc) { if (pc.uiAction() == "ASSIST OVERLAY" && dynamic_cast<juce::Button*> (&c) == nullptr && c.getHeight() <= 40) { if (k == i) r = &c; ++k; } }); return r; };
        static const float kDef[6] { 80.0f, 200.0f, 500.0f, 1200.0f, 3000.0f, 8000.0f };
        {
            DyneqAudioProcessor proc; bool ok = true;
            for (int b = 0; b < 6; ++b) ok = ok && std::abs (proc.apvts.getRawParameterValue (panel::bandId (b, "freq"))->load() - kDef[b]) < 0.01f;
            check (ok, "the six bands start at 80 / 200 / 500 / 1200 / 3000 / 8000 Hz (no longer all at 1 kHz)");
        }
        DyneqAudioProcessor proc; DyneqEditor ed (proc); settle (ed);
        auto& c = ed.ctl(); auto raw = [&] (const juce::String& id) { return proc.apvts.getRawParameterValue (id)->load(); };
        check (c.selectedBand == -1 && boxOf (ed, "b1_freq") == nullptr && ! ed.advancedOpen(), "a new LAB opens with no band selected (the strip shows its hint, no box is bound) and ADVANCED collapsed");
        // 1, 3: a chip click selects (an OFF band too), the strip's boxes follow; an unused slot is + ADD
        {
            int good = 0;
            for (int b = 0; b < 6; ++b)
            {
                setParam (proc.apvts, panel::bandId (b, "on").toRawUTF8(), 0); setParam (proc.apvts, panel::bandId (b, "gain").toRawUTF8(), 2.0f); settle (ed);   // OFF but holds a setting
                auto* chip = chipOf (ed, b);
                if (chip == nullptr) continue;
                const auto before = norms (proc);
                click (*chip, { (float) chip->getWidth() - 20.0f, (float) chip->getHeight() * 0.5f }); settle (ed);
                const bool sel = c.selectedBand == b && changed (proc, before).empty();
                bool bound = true; for (const char* n : { "freq", "q" }) bound = bound && boxOf (ed, panel::bandId (b, n)) != nullptr;
                const bool hollow = ed.testNodePosition (b).x >= 0.0f;
                if (sel && bound && hollow) ++good;
                setParam (proc.apvts, panel::bandId (b, "gain").toRawUTF8(), 0.0f);
            }
            check (good == 6, "a click on chip N selects band N, also when it is switched off: nothing changes, the strip's FREQ / Q boxes are bound to bN_*, and the OFF band has a (hollow) node on the display (" + juce::String (good) + " / 6)");
        }
        // 2: keys 1..6 select; Esc deselects
        {
            int good = 0;
            for (int b = 0; b < 6; ++b) { ed.keyPressed (juce::KeyPress ('1' + b)); settle (ed); if (c.selectedBand == b) ++good; }
            ed.keyPressed (juce::KeyPress (juce::KeyPress::escapeKey)); settle (ed);
            check (good == 6 && c.selectedBand == -1, "keys 1..6 select bands 1..6; Esc deselects");
        }
        // 12: + ADD
        {
            for (int b = 0; b < 6; ++b) for (const char* n : panel::Ctl::kBandParams) proc.apvts.getParameter (panel::bandId (b, n))->setValueNotifyingHost (defNorm (proc, panel::bandId (b, n)));
            settle (ed);
            auto* chip = chipOf (ed, 3);
            const int st = chip != nullptr ? stepsAfter (proc, [&] { click (*chip, centreOf (*chip)); }) : -1; settle (ed);
            check (c.bandOn (3) && c.bandType (3) == dyneq::kBell && std::abs (raw ("b4_freq") - 1200.0f) < 0.01f && std::abs (raw ("b4_gain")) < 1e-6f && c.selectedBand == 3 && st == 1,
                   "+ ADD on chip 4: the band goes on as a BELL at its own 1200 Hz, 0 dB, selected, one undo step");
        }
        // 4: the FREQ box drag changes only its band's frequency; the node follows on the next frame
        {
            auto* fb = boxOf (ed, "b4_freq"); const float x0 = ed.testNodePosition (3).x;
            const auto before = norms (proc);
            if (fb != nullptr) drag (*fb, centreOf (*fb), centreOf (*fb) + Point<float> (0.0f, -48.0f));
            settle (ed);
            const auto ch = changed (proc, before);
            check (fb != nullptr && ch.size() == 1 && ch[0] == "b4_freq" && raw ("b4_freq") > 1500.0f && ed.testNodePosition (3).x > x0 + 20.0f,
                   "a drag on the strip's FREQ box changes b4_freq only (1200 -> " + juce::String (raw ("b4_freq"), 0) + " Hz, 1/96 octave per pixel) and the node moves with it");
        }
        // 5: dragging an OFF node switches the band on and moves it: one undo step
        {
            setParam (proc.apvts, "b2_on", 0); setParam (proc.apvts, "b2_gain", 1.0f); settle (ed);
            const auto n0 = ed.testNodePosition (1) - ed.labScreen().getPosition().toFloat();
            const int st = stepsAfter (proc, [&] { drag (ed.labScreen(), n0, ed.testLabPoint (400.0, 5.0), 10); }); settle (ed);
            check (n0.x > 0.0f && c.bandOn (1) && std::abs (std::log2 (raw ("b2_freq") / 400.0f)) < 0.1f && st == 1, "dragging the hollow node of an OFF band switches it on and moves it, as one undo step (band 2 at " + juce::String (raw ("b2_freq"), 0) + " Hz)");
        }
        // 6: the ASSIST chips (lamp and name) never move the selection
        {
            int good = 0, n = 0;
            for (int sel : { -1, 0, 3 })
                for (int i = 0; i < 3; ++i)
                {
                    c.selectLab (sel); settle (ed);
                    auto* a = assistOf (ed, i); if (a == nullptr) continue;
                    click (*a, { 12.0f, 18.0f }); settle (ed); click (*a, { 12.0f, 18.0f }); settle (ed);   // the lamp twice
                    click (*a, { 80.0f, 18.0f }); settle (ed); const bool opened = ed.overlayId() == panel::kTarget + i;
                    click (*a, { 80.0f, 18.0f }); settle (ed);
                    ++n; if (c.selectedBand == sel && opened && ed.overlayId() < 0) ++good;
                }
            check (n == 9 && good == 9, "TARGET / COLLIDE / SELF: switching a layer (its lamp) and opening / closing its overlay (its name) leave the selected band as it was (none, 1, 4): " + juce::String (good) + " / " + juce::String (n));
        }
        // 7: an editor opened on a project saved with a layer as the panel's mode starts with no band selected
        {
            int good = 0;
            for (int m : { panel::kTarget, panel::kCollide, panel::kSelf }) { proc.uiMode = m; DyneqEditor e2 (proc); settle (e2); if (e2.ctl().selectedBand == -1 && boxOf (e2, "b1_freq") == nullptr) ++good; }
            check (good == 3, "reopening the editor with TARGET / COLLIDE / SELF as the saved panel mode: no band selected, no box edits any band (the old fallback to band 1 is gone)");
        }
        // 8, 9: SHAPE and SLOPE are lists in the control row; SLOPE is live on a cut only
        {
            c.selectLab (0); setParam (proc.apvts, "b1_on", 1); setParam (proc.apvts, "b1_type", (float) dyneq::kBell); settle (ed);
            auto slopeKey = [&] { return findCtlBy (ed, [] (juce::Component&, PanelControl& pc) { return pc.uiAction() == "SLOPE LIST" && pc.boundParams()[0] == "b1_slope"; }); };
            auto shapeKey = [&] { return findCtlBy (ed, [] (juce::Component&, PanelControl& pc) { return pc.uiAction() == "SHAPE LIST" && pc.boundParams()[0] == "b1_type"; }); };
            const bool bellOff = slopeKey() != nullptr && ! slopeKey()->isEnabled();
            c.setShape (0, dyneq::kLowCut); settle (ed);
            const bool cutOn = slopeKey() != nullptr && slopeKey()->isEnabled();
            bool listOk = false;
            if (auto* sk = dynamic_cast<panelui::PlateKeyButton*> (shapeKey())) { sk->onClick(); listOk = ed.chooserOpen() && ed.chooserCount() == 6; ed.closeChooser(); }
            if (auto* sl = dynamic_cast<panelui::PlateKeyButton*> (slopeKey())) { sl->onClick(); listOk = listOk && ed.chooserOpen() && ed.chooserCount() == 5; if (ed.chooserOpen()) { const int st = stepsAfter (proc, [&] { ed.chooserPick (3); }); listOk = listOk && st == 1 && c.bandSlopeDb (0) == 48; } }
            check (bellOff && cutOn && listOk, "SHAPE and SLOPE are lists in the control row (6 shapes, 5 slopes; a pick is one undo step); SLOPE is dimmed on a bell and live on a cut");
            c.setShape (0, dyneq::kBell); settle (ed);
        }
        // 10: RANGE is the dynamic switch; THRESHOLD is live only with it; Alt + wheel over a node edits it
        {
            setParam (proc.apvts, "b1_type", (float) dyneq::kBell); setParam (proc.apvts, "b1_dyn", 0); setParam (proc.apvts, "b1_range", 0.0f); setParam (proc.apvts, "b1_freq", 1000.0f); c.selectLab (0); settle (ed);
            auto* rb = boxOf (ed, "b1_range"); auto* tb0 = boxOf (ed, "b1_thr");
            const bool thrDeadBefore = tb0 == nullptr;
            int st = -1; if (rb != nullptr) st = stepsAfter (proc, [&] { rb->typeAndCommit ("-6"); });
            settle (ed);
            const bool dynOn = c.on ("b1_dyn") && std::abs (raw ("b1_range") + 6.0f) < 0.01f && boxOf (ed, "b1_thr") != nullptr;
            if (rb != nullptr) rb->typeAndCommit ("0");
            settle (ed);
            const bool dynOff = ! c.on ("b1_dyn") && boxOf (ed, "b1_thr") == nullptr;
            const auto n0 = ed.testNodePosition (0) - ed.labScreen().getPosition().toFloat();
            juce::MouseWheelDetails w; w.deltaX = 0.0f; w.deltaY = -1.0f; w.isReversed = false; w.isSmooth = false; w.isInertial = false;
            ed.labScreen().mouseWheelMove (mk (ed.labScreen(), n0, n0, 1, false, kAlt), w); settle (ed);
            check (thrDeadBefore && st == 1 && dynOn && dynOff && raw ("b1_range") < -0.4f && c.on ("b1_dyn"),
                   "RANGE: a value other than 0 switches the band's dynamics on (one undo step), 0 switches them off; THRESHOLD is a live box only while dynamic; Alt + wheel over the node moves RANGE (now " + juce::String (raw ("b1_range"), 1) + " dB)");
        }
        // wheel over a cut node steps the slope (Pro-Q)
        {
            c.setShape (0, dyneq::kLowCut); c.setSlope (0, 24); settle (ed);
            const auto n0 = ed.testNodePosition (0) - ed.labScreen().getPosition().toFloat();
            wheel (ed.labScreen(), n0, 1.0f); settle (ed);
            check (c.bandSlopeDb (0) == 36, "the wheel over a LOW CUT node steps its slope (24 -> " + juce::String (c.bandSlopeDb (0)) + " dB/oct)");
            c.setShape (0, dyneq::kBell); settle (ed);
        }
        // 13: Delete: the band back to its defaults and off, nothing selected, one step; undo restores it bit-exact
        {
            setParam (proc.apvts, "b1_gain", 4.5f); setParam (proc.apvts, "b1_q", 2.2f); c.selectLab (0); settle (ed);
            const auto before = norms (proc);
            const int st = stepsAfter (proc, [&] { ed.keyPressed (juce::KeyPress (juce::KeyPress::deleteKey)); }); settle (ed);
            const bool gone = c.isEmptySlot (0) && c.selectedBand == -1;
            proc.undo.undo(); settle (ed);
            check (gone && st == 1 && changed (proc, before).empty(), "Delete: band 1 returns to its defaults and goes off (the chip is + ADD again), nothing selected, one undo step; one undo restores every parameter bit-exact");
        }
        // 14: Tab walks the live boxes; the arrows nudge the selected band
        {
            c.selectLab (0); setParam (proc.apvts, "b1_on", 1); setParam (proc.apvts, "b1_type", (float) dyneq::kBell); setParam (proc.apvts, "b1_dyn", 0); setParam (proc.apvts, "b1_range", 0.0f); setParam (proc.apvts, "b1_freq", 1000.0f); setParam (proc.apvts, "b1_gain", 0.0f); settle (ed);
            juce::StringArray seq;
            for (int i = 0; i < 5; ++i)
            {
                ed.keyPressed (juce::KeyPress (juce::KeyPress::tabKey));
                ed.forEachControl ([&] (juce::Component& cc, PanelControl& pc) { if (auto* vw = dynamic_cast<panelui::ValueWin*> (&cc)) if (vw->isEditing()) seq.add (pc.boundParams()[0].substring (3)); });
            }
            ed.forEachControl ([&] (juce::Component& cc, PanelControl&) { if (auto* vw = dynamic_cast<panelui::ValueWin*> (&cc)) vw->cancelEdit(); });
            ed.keyPressed (juce::KeyPress (juce::KeyPress::rightKey, juce::ModifierKeys::shiftModifier, 0)); const float f1 = raw ("b1_freq");
            ed.keyPressed (juce::KeyPress (juce::KeyPress::upKey, juce::ModifierKeys::shiftModifier, 0)); const float g1 = raw ("b1_gain");
            check (seq.joinIntoString (" ") == "freq gain q range freq" && std::abs (f1 / 1000.0f - std::pow (2.0f, 1.0f / 12.0f)) < 0.002f && std::abs (g1 - 1.0f) < 0.01f,
                   "Tab walks FREQ -> GAIN -> Q -> RANGE (THRESHOLD skipped while not dynamic) -> FREQ; Shift+Right raises FREQ a semitone, Shift+Up raises GAIN 1 dB", "Tab: " + seq.joinIntoString (" "));
        }
        // 15, 17: ADVANCED is a drawer under the footer; nothing but the allowed popovers lies over the display
        {
            const auto plot = ed.labScreen().getBoundsInParent();
            auto overPlot = [&] (bool allowPopovers)
            {
                juce::String bad;
                std::function<void (juce::Component&, juce::Point<int>)> walk = [&] (juce::Component& p, juce::Point<int> off)
                {
                    for (auto* ch : p.getChildren())
                    {
                        if (! ch->isVisible() || ch == &ed.labScreen() || ch == &ed.labFace()) { if (ch->isVisible() && ch == &ed.labFace()) walk (*ch, off + ch->getPosition()); continue; }
                        const auto r = ch->getBounds() + off;
                        const bool popover = ch->getName() == "POPOVER";
                        if (r.intersects (plot.withTrimmedBottom (24)) && ! (allowPopovers && popover)) bad << ch->getName() << "@" << r.toString() << " ";
                    }
                };
                walk (ed, {});
                return bad;
            };
            c.selectLab (0); ed.closeOverlay(); ed.setDynOpen (false); ed.setAdvancedOpen (false); settle (ed);
            const auto bad0 = overPlot (false);
            ed.setAdvancedOpen (true); settle (ed);
            const auto plot2 = ed.labScreen().getBoundsInParent(); const auto bad1 = overPlot (true);
            ed.setDynOpen (true); ed.toggleOverlay (panel::kCollide); settle (ed); const auto bad2 = overPlot (true);
            ed.setAdvancedOpen (false); ed.setDynOpen (false); ed.closeOverlay(); settle (ed);
            check (bad0.isEmpty() && bad1.isEmpty() && bad2.isEmpty() && plot2 == plot && plot.getY() == 96 && plot.getBottom() == 474,
                   "the display keeps y 96..474 with ADVANCED open or closed; nothing lies over the curve area except the popovers (ASSIST overlay, ADVANCED, '...', the lists)", bad0 + bad1 + bad2);
        }
        // 18: the zones do not overlap: every visible control of the LAB lies inside the window
        {
            juce::String bad;
            ed.forEachControl ([&] (juce::Component& cc, PanelControl&) { const auto r = ed.getLocalArea (&cc, cc.getLocalBounds()); if (! ed.getLocalBounds().contains (r)) bad << r.toString() << " "; });
            check (bad.isEmpty(), "every visible LAB control lies inside the 1200 x 540 window", bad);
        }
    }

    //------------------------------------------------------------------------------------------------ 12j. the REAL mouse path: a click at the centre of every visible control reaches that control
    // (Component::getComponentAt, as a host delivers it). The synthesized events above call the control directly, so they missed that ParamKnob / MicroKnob
    // blocked their knob and value box (owner, Live 11 Oct: "knoblar elle cevrilemiyor, elle deger girilemiyor"). A control covered by an open popover is counted, not failed.
    {
        DyneqAudioProcessor proc; DyneqEditor ed (proc); ed.setVisible (true); settle (ed);   // getComponentAt asks the editor's own visibility (a host shows it)
        int n = 0, bad = 0, covered = 0; juce::String firstBad;
        for (auto& st : labStates())
        {
            st.apply (ed); settle (ed);
            std::vector<juce::Component*> list;
            ed.forEachControl ([&] (juce::Component& c, PanelControl& pc) { const auto ua = pc.uiAction(); if (ua != "LAB DISPLAY" && ua != "INACTIVE") list.push_back (&c); });
            for (auto* comp : list)
            {
                const auto ctr = ed.getLocalPoint (comp, comp->getLocalBounds().getCentre());
                auto* hit = ed.getComponentAt (ctr);
                ++n;
                if (hit == comp || (hit != nullptr && comp->isParentOf (hit))) continue;
                bool underPopover = false;
                for (auto* p = hit; p != nullptr; p = p->getParentComponent()) if (p->getName() == "POPOVER" && ! p->isParentOf (comp)) underPopover = true;
                if (underPopover) { ++covered; continue; }
                ++bad; if (firstBad.isEmpty()) { auto* pc = dynamic_cast<PanelControl*> (comp); firstBad = st.name + ": '" + (pc != nullptr ? pc->boundParams().joinIntoString (",") + " " + pc->uiAction() : juce::String()) + "' at " + ctr.toString() + " reaches " + (hit != nullptr ? juce::String (typeid (*hit).name()) : juce::String ("nothing")); }
            }
        }
        check (n > 100 && bad == 0, "LAB: a real click at the centre of every visible control reaches that control (" + juce::String (n) + " controls over " + juce::String ((int) labStates().size()) + " states, " + juce::String (covered) + " under an open popover, " + juce::String (bad) + " unreachable)", firstBad);
    }

    //------------------------------------------------------------------------------------------------ 12h. no clicks: switch a band, a type, a slope, BYPASS from the editor, in the middle of a 1 kHz cosine (the A17 / A18 method)
    {
        struct Row { const char* name; std::function<void (DyneqEditor&)> prep, act; double stepMax, hfMax; };
        const double own = 0.25 * 2.0 * juce::MathConstants<double>::pi * 1000.0 / 48000.0;
        auto setBand = [] (DyneqEditor& e, int type, float f, float g, float q, int slopeIdx, bool on)
        {
            auto& p = e.ctl().proc; setParam (p.apvts, "b1_type", (float) type); setParam (p.apvts, "b1_freq", f); setParam (p.apvts, "b1_gain", g); setParam (p.apvts, "b1_q", q); setParam (p.apvts, "b1_slope", (float) slopeIdx); setParam (p.apvts, "b1_on", on ? 1.0f : 0.0f);
        };
        std::vector<Row> rows;
        rows.push_back ({ "band ON: bell +12 (node double-click)", [=] (DyneqEditor& e) { setBand (e, dyneq::kBell, 1000, 12, 1, 0, false); }, [] (DyneqEditor& e) { e.ctl().toggleBand (0); }, 1.5, -60.0 });
        rows.push_back ({ "band ON: high shelf 3k +12", [=] (DyneqEditor& e) { setBand (e, dyneq::kHighShelf, 3000, 12, 0.71f, 0, false); }, [] (DyneqEditor& e) { e.ctl().toggleBand (0); }, 1.5, -60.0 });
        rows.push_back ({ "band ON: notch 1k Q 8", [=] (DyneqEditor& e) { setBand (e, dyneq::kNotch, 1000, 0, 8, 0, false); }, [] (DyneqEditor& e) { e.ctl().toggleBand (0); }, 1.5, -60.0 });
        rows.push_back ({ "band ON: low cut 1k 72", [=] (DyneqEditor& e) { setBand (e, dyneq::kLowCut, 1000, 0, 1, 4, false); }, [] (DyneqEditor& e) { e.ctl().toggleBand (0); }, 1.5, -60.0 });
        rows.push_back ({ "band ON: high cut 500 12", [=] (DyneqEditor& e) { setBand (e, dyneq::kHighCut, 500, 0, 1, 0, false); }, [] (DyneqEditor& e) { e.ctl().toggleBand (0); }, 1.5, -60.0 });
        rows.push_back ({ "band OFF: bell +12", [=] (DyneqEditor& e) { setBand (e, dyneq::kBell, 1000, 12, 1, 0, true); }, [] (DyneqEditor& e) { e.ctl().toggleBand (0); }, 1.5, -60.0 });
        rows.push_back ({ "type: bell 1.5k +12 -> notch (SHAPE list)", [=] (DyneqEditor& e) { setBand (e, dyneq::kBell, 1500, 12, 2, 0, true); }, [] (DyneqEditor& e) { e.ctl().setShape (0, dyneq::kNotch); }, 99.0, -70.0 });
        rows.push_back ({ "slope: low cut 2k 12 -> 48 (SLOPE list)", [=] (DyneqEditor& e) { setBand (e, dyneq::kLowCut, 2000, 0, 1, 0, true); }, [] (DyneqEditor& e) { e.ctl().setSlope (0, 48); }, 99.0, -70.0 });
        rows.push_back ({ "slope: high cut 3k 24 -> 72", [=] (DyneqEditor& e) { setBand (e, dyneq::kHighCut, 3000, 0, 1, 1, true); }, [] (DyneqEditor& e) { e.ctl().setSlope (0, 72); }, 99.0, -70.0 });
        rows.push_back ({ "BYPASS on (level matched), with a live bell +9", [=] (DyneqEditor& e) { setBand (e, dyneq::kBell, 1000, 9, 1, 0, true); }, [] (DyneqEditor& e) { e.ctl().setGesture ("bypass", 1.0f); }, 1.5, -60.0 });
        rows.push_back ({ "BYPASS off again", [=] (DyneqEditor& e) { setBand (e, dyneq::kBell, 1000, 9, 1, 0, true); setParam (e.ctl().proc.apvts, "bypass", 1.0f); }, [] (DyneqEditor& e) { e.ctl().setGesture ("bypass", 0.0f); }, 1.5, -60.0 });
        juce::String worst; bool ok = true;
        for (auto& row : rows)
        {
            Rig r (2, false, 48000.0, 512, {}); DyneqEditor ed (r.proc); row.prep (ed); ed.tickForSnapshot();
            Audio x (2, std::vector<float> (57600)); for (size_t i = 0; i < x[0].size(); ++i) x[0][i] = x[1][i] = (float) (0.25 * std::cos (2.0 * juce::MathConstants<double>::pi * 1000.0 * (double) i / 48000.0));   // double precision: a float phase at i = 50000 jitters by 5e-4 rad, a -66 dBc floor of its own
            const Audio a = slice (x, 0, 48000), b = slice (x, 48000, 9600);
            Audio out = r.render (a, nullptr, 512); row.act (ed); const Audio out2 = r.render (b, nullptr, 512);
            for (size_t c2 = 0; c2 < 2; ++c2) out[c2].insert (out[c2].end(), out2[c2].begin(), out2[c2].end());
            double step = 0.0; for (int i = 47990; i < 48100; ++i) step = std::max (step, (double) std::abs (out[0][(size_t) i + 1] - out[0][(size_t) i]));
            // the step is read against the sine's own step at the level it has around the switch (a bell +12 is 4 times louder: its own step is 4 times larger)
            double preAmp = 0, postAmp = 0; for (int i = 47000; i < 47990; ++i) preAmp = std::max (preAmp, (double) std::abs (out[0][(size_t) i])); for (int i = 48400; i < 49400; ++i) postAmp = std::max (postAmp, (double) std::abs (out[0][(size_t) i]));
            step /= own * std::max (preAmp, postAmp) / 0.25; const double hf = hfResidueDbc (out[0], 47000, 49500, 0.25);
            const bool good = step <= row.stepMax && hf <= row.hfMax && allFinite (out);
            ok = ok && good; worst << juce::String (row.name) << ": " << juce::String (step, 2) << "x, " << juce::String (hf, 1) << " dBc" << (good ? "" : "  <-- OVER") << "; ";
        }
        check (ok, "no clicks, switched from the editor in the middle of a 1 kHz cosine: band ON / OFF, shape, slope and BYPASS stay within the P1n bounds (step <= 1.5x the sine's own and HF above 4 kHz <= -60 dBc for ON / OFF / BYPASS; <= -70 dBc for shape and slope)", worst);
    }
}
