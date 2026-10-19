#include "LabDoor.h"

namespace jfui
{
namespace
{
const juce::Colour kDoor = juce::Colour (0xff121011), kEmber = juce::Colour (0xffD0140A);
using R = juce::Rectangle<int>;
using RF = juce::Rectangle<float>;

juce::String keyName (int key)
{
    static const char* n[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return juce::String (n[key % 12]) + juce::String (key / 12 - 1);
}
juce::String pct (float v) { return juce::String (juce::roundToInt (v * 100.0f)) + " %"; }
juce::String cts (float v) { return juce::String (v, 0) + " ct"; }
} // namespace

//==============================================================================
LabDoor::LabDoor (JingFangProcessor& p, juce::Rectangle<int> fullBounds) : proc (p), full (fullBounds)
{
    setInterceptsMouseClicks (true, true);
    const auto& D = doorsLayout()["lab"];
    auto rectOf = [] (const juce::var& a) { return R ((int) a[0], (int) a[1], (int) a[2], (int) a[3]); };
    auto add = [this] (juce::Component* c, R b) { owned.emplace_back (c); addAndMakeVisible (c); c->setBounds (b); return c; };
    auto key = [&] (const juce::String& t, R b, std::function<void()> click, const juce::String& tip)
    {
        auto* k = static_cast<PanelKey*> (add (new PanelKey (t), b));
        k->onClick = std::move (click); k->setTooltip (tip);
        return k;
    };
    const auto& st = proc.labState();
    cardA = rectOf (D["cards"][0]); cardB = rectOf (D["cards"][1]);
    const auto& rw = D["round_windows"][0];
    const int rr = (int) rw["r"];
    wheel = R ((int) rw["c"][0] - rr, (int) rw["c"][1] - rr, rr * 2, rr * 2);
    list = rectOf (D["sounds"]);
    statusBox = rectOf (D["status"]);

    // ---- the cards' keys (the face's machined keys)
    for (int which = 0; which < 2; ++which)
    {
        int kx = (int) D["card_keys"][which][0];
        const int ky = (int) D["card_keys"][which][1];
        auto ck = [&] (const juce::String& t, int w, std::function<void()> f, const juce::String& tip)
        { key (t, R (kx, ky, w, 28), std::move (f), tip); kx += w + 8; };
        ck ("LOAD", 74, [this, which] { loadMenu (which); }, "Load a built-in tuning, or a Scala tuning (.scl) or key map (.kbm)");
        ck ("LEARN", 80, [this, which] { choose (which, true); }, "Learn a tuning from a recording of a solo line (WAV, AIFF, FLAC)");
        ck ("EXPORT", 84, [this, which] { exportTuning (which); }, "Save as .scl + .kbm (and the falling values as a second .scl)");
        ck ("12 EQUAL", 96, [this, which] { apply ([which] (auto& s) { (which == 0 ? s.a : s.b) = jflab::Tuning(); }); status = "Back to 12 equal."; repaint(); },
            "Back to the usual 12 equal steps");
    }
    // ---- bat-lever switches: LAB ON / DIRECTION / ADAPTIVE
    for (auto& sw : *D["lab_switches"].getArray())
    {
        const auto what = sw["what"].toString();
        auto get = [this, what] { const auto& s = proc.labState(); return (what == "on" ? s.on : what == "directional" ? s.directional : s.adaptive) ? 2 : 0; };
        auto set = [this, what] (int pos) { apply ([what, pos] (auto& s) { (what == "on" ? s.on : what == "directional" ? s.directional : s.adaptive) = pos == 2; }); };
        auto* b = static_cast<BatSwitch*> (add (new BatSwitch (get, set, 2), partBox (sw["c"], (float) (double) sw["d"])));
        b->setTooltip (what == "on" ? "The Lab plays every note (off: the TUNING panel as usual)"
                     : what == "directional" ? "A step plays its falling value when the melody comes down onto it"
                                             : "Each new note settles on the simple ratio nearest its tuned pitch");
    }
    // ---- photoreal knobs: MORPH / WINDOW / HOME PULL (the Lab's own values, not host parameters)
    for (auto& kv : *D["lab_knobs"].getArray())
    {
        const auto what = kv["what"].toString();
        auto* k = static_cast<ValueKnob*> (add (new ValueKnob(), partBox (kv["c"], (float) (double) kv["d"])));
        labKnobs.push_back ({ what, k });
        if (what == "morph")
        {
            k->lo = 0; k->hi = 1; k->def = 0; k->value = st.morph;
            k->format = [] (float v) { return juce::String (juce::roundToInt (v * 100.0f)) + " %"; };
            k->onChange = [this] (float v) { apply ([v] (auto& s) { s.morph = v; }); };
            k->setTooltip ("Every key moves from tuning A to tuning B");
        }
        else if (what == "window")
        {
            k->lo = 0; k->hi = 50; k->def = 20; k->value = st.window;
            k->format = [] (float v) { return juce::String (v, 0) + " ct"; };
            k->onChange = [this] (float v) { apply ([v] (auto& s) { s.window = v; }); };
            k->setTooltip ("Adaptive: how far a note may move to reach its ratio");
        }
        else
        {
            k->lo = 0; k->hi = 1; k->def = 0.5f; k->value = st.pull;
            k->format = [] (float v) { return juce::String (juce::roundToInt (v * 100.0f)) + " %"; };
            k->onChange = [this] (float v) { apply ([v] (auto& s) { s.pull = v; }); };
            k->setTooltip ("Adaptive: how strongly new chords return to the tuning's home");
        }
    }
    applyLock();
    layoutDoor();
}

void LabDoor::applyLock()
{
    shownLocked = locked();
    for (auto& c : owned) { c->setEnabled (! shownLocked); c->setAlpha (shownLocked ? 0.5f : 1.0f); }
    repaint();
}

void LabDoor::layoutDoor()
{
    const int h = juce::roundToInt ((float) full.getHeight() * amount);
    setBounds (full.withHeight (juce::jmax (1, h)));
    setVisible (amount > 0.001f);
}

void LabDoor::apply (const std::function<void (JingFangProcessor::LabState&)>& change)
{
    if (locked()) return;
    auto st = proc.labState();
    change (st);
    proc.labTakenByUser();                                   // a hand on the Lab: it stays as it is when the next preset loads
    proc.setLabState (st);
    for (auto& c : owned) c->repaint();
    repaint();
}

void LabDoor::tick()
{
    if (amount != target)
    {
        const float d = target - amount;
        amount = std::abs (d) < 0.02f ? target : amount + d * 0.42f;
        layoutDoor();
    }
    if (! isVisible()) return;
    if (locked() != shownLocked) applyLock();
    {   // the Lab changed from outside the door (a preset's tuning, a host restore, LEARN): redraw the cards and switches
        const auto& st = proc.labState();
        auto sig = [] (const jflab::Tuning& t) { juce::String s (t.name); for (double c : t.cents) s << ' ' << c; return s << (t.directional() ? "/d" : ""); };
        const juce::String now = sig (st.a) + "|" + sig (st.b) + "|" + juce::String ((int) st.on) + juce::String ((int) st.directional) + juce::String ((int) st.adaptive);
        if (now != shownState) { shownState = now; for (auto& c : owned) c->repaint(); repaint(); }
    }
    for (auto& [what, k] : labKnobs)
    {   // follow the Lab's state (a host restore, a learned tuning) unless the knob is in the hand
        if (k->dragging) continue;
        const auto& st = proc.labState();
        const float v = what == "morph" ? st.morph : what == "window" ? st.window : st.pull;
        if (std::abs (k->value - v) > 1.0e-4f) { k->value = v; k->repaint(); }
    }
    auto s = proc.labSounding();
    const float dr = proc.labDrift();
    const int di = proc.labDirection();
    if (s != sounding || std::abs (dr - drift) > 0.05f || di != dir)
    {
        sounding = std::move (s); drift = dr; dir = di;
        repaint (wheel.getUnion (list).expanded (4));
    }
}

//==============================================================================
bool LabDoor::isInterestedInFileDrag (const juce::StringArray& files)
{
    if (locked()) return false;
    for (auto& f : files)
        if (juce::File (f).hasFileExtension ("scl;kbm;wav;aif;aiff;flac")) return true;
    return false;
}

void LabDoor::filesDropped (const juce::StringArray& files, int x, int y)
{
    const int which = cardB.contains (x, y) ? 1 : (y > cardA.getBottom() + 6 && ! cardA.contains (x, y) && x < wheel.getX() ? 1 : 0);
    for (auto& f : files) loadFile (juce::File (f), which);
}

void LabDoor::loadFile (const juce::File& f, int which)
{
    if (locked()) return;
    const auto slot = which == 0 ? "A" : "B";
    if (f.hasFileExtension ("scl;kbm"))
    {
        auto st = proc.labState();
        auto& t = which == 0 ? st.a : st.b;
        std::string err;
        const bool ok = f.hasFileExtension ("scl") ? jflab::scala::readScl (f.loadFileAsString().toStdString(), t, &err)
                                                   : jflab::scala::readKbm (f.loadFileAsString().toStdString(), t, &err);
        if (ok) { if (f.hasFileExtension ("scl")) t.name = f.getFileNameWithoutExtension().toStdString(); proc.setLabState (st); }
        status = ok ? juce::String (slot) + ": " + f.getFileName() + " loaded." : f.getFileName() + ": " + juce::String (err) + ".";
        repaint();
        return;
    }
    learning = true;
    status = juce::String (slot) + ": listening to " + f.getFileName() + " ... (the other pages wait until LEARN is done)";
    repaint();
    juce::Component::SafePointer<LabDoor> self (this);
    proc.labLearn (f, which, [self, slot] (juce::String msg, bool ok)
    {
        if (self == nullptr) return;
        self->learning = false;
        self->status = juce::String (slot) + ": " + (ok ? "learned, " : "") + msg + (ok ? "." : "");
        self->repaint();
    });
}

void LabDoor::loadBuiltin (int which, int entry)
{
    const auto& es = jflab::builtin::entries();
    if (locked() || entry < 0 || entry >= (int) es.size()) return;
    const auto t = jflab::builtin::make (es[(size_t) entry]);
    apply ([which, t] (auto& s) { (which == 0 ? s.a : s.b) = t; s.on = true; });
    status = juce::String (which == 0 ? "A: " : "B: ") + juce::String (t.name) + " loaded (53 commas to the octave, A4 = 440 Hz).";
    repaint();
}

void LabDoor::loadMenu (int which)
{
    if (locked() || learning) return;
    juce::PopupMenu m;
    m.addSectionHeader ("Built in (53 commas to the octave)");
    const auto& es = jflab::builtin::entries();
    const auto current = juce::String ((which == 0 ? proc.labState().a : proc.labState().b).name);
    for (int i = 0; i < (int) es.size(); ++i) m.addItem (1 + i, es[(size_t) i].name, true, current == es[(size_t) i].name);
    m.addSeparator();
    m.addItem (1000, "From a file (.scl / .kbm) ...");
    juce::Component::SafePointer<LabDoor> self (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (owned[(size_t) (which * 4)].get()), [self, which] (int r)
    {
        if (self == nullptr || r == 0) return;
        if (r == 1000) self->choose (which, false);
        else self->loadBuiltin (which, r - 1);
    });
}

void LabDoor::choose (int which, bool learn)
{
    if (locked() || learning) return;
    chooser = std::make_unique<juce::FileChooser> (learn ? "Learn a tuning from a recording" : "Load a Scala tuning or key map",
                                                   juce::File::getSpecialLocation (juce::File::userHomeDirectory),
                                                   learn ? "*.wav;*.aif;*.aiff;*.flac" : "*.scl;*.kbm");
    juce::Component::SafePointer<LabDoor> self (this);
    pickingToLearn = learn;
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [self, which] (const juce::FileChooser& fc)
    {
        if (self == nullptr) return;
        self->pickingToLearn = false;                       // picked (LEARN now listens) or cancelled
        if (fc.getResult().existsAsFile()) self->loadFile (fc.getResult(), which);
    });
}

void LabDoor::exportTuning (int which)
{
    if (locked()) return;
    const auto t = which == 0 ? proc.labState().a : proc.labState().b;
    chooser = std::make_unique<juce::FileChooser> ("Save the tuning", juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                                                   .getChildFile (juce::File::createLegalFileName (t.name) + ".scl"), "*.scl");
    juce::Component::SafePointer<LabDoor> self (this);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [self, t] (const juce::FileChooser& fc)
    {
        auto f = fc.getResult();
        if (self == nullptr || f == juce::File()) return;
        f = f.withFileExtension ("scl");
        const auto kbm = f.withFileExtension ("kbm"), fall = f.getSiblingFile (f.getFileNameWithoutExtension() + "_fall.scl");
        if ((kbm.existsAsFile() || (t.directional() && fall.existsAsFile()))
            && ! juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Replace the files beside it?",
                                                     "A .kbm (or _fall.scl) with this name is already there.", "Replace", "Cancel", self.getComponent(), nullptr))
        { self->status = "Not saved."; self->repaint(); return; }
        bool ok = f.replaceWithText (jflab::scala::writeScl (t)) && f.withFileExtension ("kbm").replaceWithText (jflab::scala::writeKbm (t));
        if (ok && t.directional())
            ok = f.getSiblingFile (f.getFileNameWithoutExtension() + "_fall.scl").replaceWithText (jflab::scala::writeScl (t, true));
        self->status = ok ? "Saved " + f.getFileName() + (t.directional() ? " (+ _fall.scl)" : juce::String()) + " and .kbm." : "Could not save " + f.getFileName() + ".";
        self->repaint();
    });
}

//==============================================================================
void LabDoor::paint (juce::Graphics& g)
{
    const auto b = juce::Rectangle<float> (0.0f, 0.0f, (float) full.getWidth(), (float) full.getHeight());
    {
        juce::Graphics::ScopedSaveState ss (g);
        juce::Path clip; clip.addRoundedRectangle (b, 6.0f);
        g.reduceClipRegion (clip);
        const auto& plate = doorPlate ("lab");
        if (plate.isValid()) drawScaledCached (g, plate, b);
        else { g.setColour (kDoor); g.fillRect (b); }
    }
    g.setColour (juce::Colours::black.withAlpha (0.8f)); g.drawRoundedRectangle (b.reduced (0.5f), 6.0f, 1.2f);
    if (locked())
    {   // a padlock and one plain line beside the LAB module's title; no price, no promise
        const RF lk (62.0f, 372.0f, 12.0f, 16.0f);
        g.setColour (kKeyTextDim);
        g.drawRoundedRectangle (lk.withTrimmedBottom (7.0f).reduced (2.2f, 0.0f), 4.0f, 1.5f);
        g.fillRoundedRectangle (lk.withTrimmedTop (7.0f), 2.0f);
        g.setFont (heros (10.5f, true));
        g.drawText ("Locked in this version", RF (82.0f, 371.0f, 260.0f, 18.0f), juce::Justification::centredLeft, false);
    }
    drawCard (g, cardA.toFloat(), 0);
    drawCard (g, cardB.toFloat(), 1);
    drawWheel (g, wheel.toFloat());
    drawIntervals (g, list.toFloat());
    // the status window: what the Lab did last, or how to feed it
    g.setColour (status.isNotEmpty() ? kLed : kKeyTextDim);
    g.setFont (heros (10.5f, false));
    const juce::String hint = status.isNotEmpty() ? status
        : juce::String ("Drop a .scl, a .kbm or a recording (WAV, AIFF, FLAC) on tuning A or B. Double-click a knob: back to its default.");
    g.drawFittedText (hint, statusBox.reduced (10, 8), juce::Justification::topLeft, 8);
}

void LabDoor::drawCard (juce::Graphics& g, juce::Rectangle<float> c, int which)
{
    const auto& t = which == 0 ? proc.labState().a : proc.labState().b;
    g.setColour (kKeyText);
    g.setFont (heros (15.0f, true));
    g.drawText (juce::String (t.name), RF (c.getX() + 12.0f, c.getY() + 12.0f, c.getWidth() - 24.0f, 20.0f), juce::Justification::centredLeft, true);
    juce::String info;
    info << t.size() << " steps, period " << juce::String (t.period(), 1) << " ct";
    if (t.directional()) info << ", climbing and falling values";
    if (! t.confidence.empty())
    {
        float m = 0; for (float v : t.confidence) m += v;
        info << ", learned (" << juce::roundToInt (100.0f * m / (float) t.confidence.size()) << " % sure)";
    }
    g.setColour (kKeyTextDim);
    g.setFont (heros (11.0f, false));
    g.drawText (info, RF (c.getX() + 12.0f, c.getY() + 34.0f, c.getWidth() - 24.0f, 16.0f), juce::Justification::centredLeft, true);
    // the degrees as numbers (cents), falling values beneath where they differ
    juce::String row, frow;
    const int n = juce::jmin (t.size() - 1, 16);
    for (int i = 0; i < n; ++i)
    {
        row << juce::String (t.cents[(size_t) i], 0) << "  ";
        if (t.directional()) frow << (std::abs (t.fall[(size_t) i] - t.cents[(size_t) i]) >= 0.5 ? juce::String (t.fall[(size_t) i], 0) : juce::String ("-")) << "  ";
    }
    if (t.size() - 1 > n) row << "...";
    g.setColour (kLed.withAlpha (0.9f));
    g.setFont (mono (11.0f));
    g.drawText (row, RF (c.getX() + 12.0f, c.getY() + 56.0f, c.getWidth() - 24.0f, 16.0f), juce::Justification::centredLeft, true);
    if (frow.isNotEmpty())
    {
        g.setColour (kEmber.brighter (0.3f));
        g.drawText (frow, RF (c.getX() + 12.0f, c.getY() + 74.0f, c.getWidth() - 24.0f, 16.0f), juce::Justification::centredLeft, true);
    }
}

void LabDoor::drawWheel (juce::Graphics& g, juce::Rectangle<float> r)
{
    const auto c = r.getCentre();
    const float R0 = r.getWidth() * 0.5f - 10.0f;
    auto at = [&] (double cents, float rad) { const float a = (float) (cents / 1200.0 * juce::MathConstants<double>::twoPi); return c + juce::Point<float> (std::sin (a), -std::cos (a)) * rad; };
    // 12 equal reference: faint hairlines
    g.setColour (kKeyTextDim.withAlpha (0.18f));
    for (int i = 0; i < 12; ++i) g.drawLine ({ at (i * 100.0, R0 * 0.30f), at (i * 100.0, R0) }, 0.8f);
    const auto& st = proc.labState();
    auto ticks = [&] (const jflab::Tuning& t, float r1, float r2, juce::Colour col, bool falling)
    {
        const int n = t.size();
        const double per = t.period() > 0 ? t.period() : 1200.0;
        for (int d = 0; d < n; ++d)
        {
            const double cc = d == 0 ? 0.0 : (falling ? t.fall[(size_t) d - 1] : t.cents[(size_t) d - 1]);
            g.setColour (col);
            g.drawLine ({ at (std::fmod (cc / per * 1200.0, 1200.0), r1), at (std::fmod (cc / per * 1200.0, 1200.0), r2) }, 2.2f);
        }
    };
    ticks (st.a, R0 * 0.72f, R0, kLed, false);
    if (st.a.directional()) ticks (st.a, R0 * 0.58f, R0 * 0.72f, kEmber.brighter (0.2f), true);
    ticks (st.b, R0 * 0.42f, R0 * 0.56f, Pal::gold, false);
    // the morph: a ring between A (outer) and B (inner)
    g.setColour (kKeyText.withAlpha (0.25f));
    const float mr = R0 * (0.86f - 0.37f * st.morph);
    g.drawEllipse (RF (mr * 2, mr * 2).withCentre (c), 0.8f);
    // what sounds now, at its exact pitch (relative to C)
    for (auto& [k, p] : sounding)
    {
        juce::ignoreUnused (k);
        const auto pt = at (std::fmod ((double) p * 100.0 + 120000.0, 1200.0), R0 * 0.86f);
        g.setColour (kKeyText); g.fillEllipse (RF (9.0f, 9.0f).withCentre (pt));
    }
    g.setColour (kKeyTextDim);
    g.setFont (heros (9.0f, true));
    g.drawText ("C", RF (20.0f, 12.0f).withCentre (at (0.0, R0 + 0.0f).translated (0.0f, -2.0f)), juce::Justification::centred, false);
    // the direction of the phrase, in the middle
    if (st.on && st.directional) drawArrow (g, c, 14.0f, dir > 0, dir > 0 ? kLed : kEmber.brighter (0.2f));
}

void LabDoor::drawIntervals (juce::Graphics& g, juce::Rectangle<float> r)
{
    auto row = r.reduced (10.0f, 8.0f);
    g.setColour (kKeyTextDim);
    g.setFont (heros (10.0f, false));
    juce::String d;
    d << "drift " << (drift >= 0 ? "+" : "") << juce::String (drift, 1) << " ct";
    g.drawText (d, row.removeFromTop (16.0f), juce::Justification::centredLeft, false);
    row.removeFromTop (6.0f);
    const auto iv = jflab::intervals (sounding);
    if (iv.empty())
    {
        g.drawText (proc.labState().on ? "Hold two or more notes." : "Switch the Lab on and hold two or more notes.", row.removeFromTop (16.0f), juce::Justification::centredLeft, true);
        return;
    }
    g.setFont (mono (10.5f));
    for (const auto& i : iv)
    {
        if (row.getHeight() < 15.0f) break;
        juce::String l;
        l << keyName (i.lowKey).paddedRight (' ', 4) << "> " << keyName (i.highKey).paddedRight (' ', 4)
          << juce::String (i.cents, 1).paddedLeft (' ', 7) << " ct  " << (juce::String (i.ratio.p) + "/" + juce::String (i.ratio.q)).paddedRight (' ', 6)
          << (i.deviation >= 0 ? "+" : "") << juce::String (i.deviation, 1) << "  " << juce::String (i.beatHz, 2) << " Hz";
        g.setColour (std::abs (i.deviation) < 2.0 ? kLed : kKeyText);
        g.drawText (l, row.removeFromTop (15.0f), juce::Justification::centredLeft, false);
    }
}
} // namespace jfui
