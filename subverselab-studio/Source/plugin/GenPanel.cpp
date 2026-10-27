#include "GenPanel.h"
#include <map>

namespace jfui
{
namespace
{
const juce::Colour kRowWhite (0xff1b2020), kRowBlack (0xff151919), kLine (0xff242a2a), kLineC (0xff3c4644), kBeat (0xff34403e), kBar (0xff4e5a58);
const juce::Colour kPlate (0xff1d1c1b), kGlass (0xff101313);
juce::String stepId (int s, const char* f) { return "seq.s" + juce::String (s + 1) + "." + f; }
juce::String keyName (int rel) { return juce::MidiMessage::getMidiNoteName (60 + rel, true, true, 4); }
}

//==============================================================================
PianoRoll::PianoRoll (JingFangProcessor& p, std::function<int()> current) : proc (p), stepNow (std::move (current))
{
    setTooltip ("Click a note: select its step (drag up or down: its note). Click an empty place: that step plays that key. Double-click a note: "
                "the step off. Shift-click above a note: add or remove a chord note there. Drag in VEL: how hard it plays. Wheel: scroll the keys.");
}

float PianoRoll::v (const juce::String& id) const
{
    if (auto* p = proc.apvts.getParameter (id)) return p->convertFrom0to1 (p->getValue());
    return 0.0f;
}

void PianoRoll::set (const juce::String& id, float value)
{
    if (auto* p = proc.apvts.getParameter (id)) setParamGesture (p, value);
}

std::array<PianoRoll::View, 16> PianoRoll::read() const
{
    std::array<View, 16> a {};
    for (int s = 0; s < 16; ++s)
    {
        auto& x = a[(size_t) s];
        x.on = v (stepId (s, "on")) > 0.5f; x.note = juce::roundToInt (v (stepId (s, "note"))); x.row = juce::roundToInt (v (stepId (s, "row")));
        x.vel = v (stepId (s, "vel")); x.gate = v (stepId (s, "gate")); x.prob = v (stepId (s, "prob")); x.ratchet = juce::roundToInt (v (stepId (s, "ratchet")));
        x.slide = v (stepId (s, "slide")) > 0.5f; x.accent = v (stepId (s, "accent")) > 0.5f;
        for (int c = 0; c < 3; ++c) x.chord[(size_t) c] = juce::roundToInt (v (stepId (s, ("c" + juce::String (c + 1)).toRawUTF8())));
    }
    return a;
}

juce::Rectangle<float> PianoRoll::grid() const
{
    return getLocalBounds().toFloat().withTrimmedLeft ((float) kKeysW).withTrimmedTop ((float) kRulerH).withTrimmedBottom ((float) kVelH + 6.0f);
}
juce::Rectangle<float> PianoRoll::velLane() const
{
    return getLocalBounds().toFloat().withTrimmedLeft ((float) kKeysW).removeFromBottom ((float) kVelH).reduced (0.0f, 3.0f);
}
int PianoRoll::stepAt (float x) const { const auto g = grid(); return juce::jlimit (0, 15, (int) std::floor ((x - g.getX()) / (g.getWidth() / 16.0f))); }
float PianoRoll::yOfKey (float key) const { const auto g = grid(); const float rh = g.getHeight() / kKeys; return g.getY() + ((float) (low + kKeys - 1) - key) * rh; }
int PianoRoll::keyAt (float y) const { const auto g = grid(); const float rh = g.getHeight() / kKeys; return low + kKeys - 1 - (int) std::floor ((y - g.getY()) / rh); }

void PianoRoll::fitView (const std::array<View, 16>& a)
{
    int lo = 1000, hi = -1000;
    for (int s = 0; s < len; ++s)
        if (a[(size_t) s].on) { lo = std::min (lo, a[(size_t) s].note); int top = a[(size_t) s].note; for (int c : a[(size_t) s].chord) top = std::max (top, a[(size_t) s].note + c); hi = std::max (hi, top); }
    if (lo > hi) return;
    if (lo >= low && hi <= low + kKeys - 1) return;   // everything on view: keep the view still
    low = juce::jlimit (-24, 24, (lo + hi) / 2 - kKeys / 2);
}

void PianoRoll::select (int s)
{
    s = juce::jlimit (0, 15, s);
    if (s == sel) return;
    sel = s; repaint();
    if (onSelect) onSelect (sel);
}

void PianoRoll::tick()
{
    const auto a = read();
    len = juce::jlimit (1, 16, juce::roundToInt (v ("seq.length")));
    juce::String sig;
    for (auto& x : a) sig << (x.on ? 1 : 0) << x.note << '.' << x.row << '.' << x.vel << '.' << x.gate << '.' << x.prob << x.ratchet << (x.slide ? 1 : 0) << (x.accent ? 1 : 0) << x.chord[0] << x.chord[1] << x.chord[2] << '|';
    sig << len << v ("tuning.row_up") << v ("tuning.edo");
    const int p = stepNow ? stepNow() : -1;
    if (sig != shownSig) { if (dragStep < 0) fitView (a); shownSig = sig; repaint(); }
    if (p != play) { play = p; repaint(); }
}

void PianoRoll::paint (juce::Graphics& g)
{
    const auto a = read();
    const auto b = getLocalBounds().toFloat();
    const auto gr = grid(), vl = velLane();
    const float cw = gr.getWidth() / 16.0f, rh = gr.getHeight() / kKeys;
    g.setColour (kGlass); g.fillRoundedRectangle (b, 4.0f);
    // rows
    for (int k = low; k < low + kKeys; ++k)
    {
        const float y = yOfKey ((float) k);
        const int pc = ((k % 12) + 12) % 12;
        const bool black = pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
        g.setColour (black ? kRowBlack : kRowWhite); g.fillRect (gr.getX(), y, gr.getWidth(), rh);
        g.setColour (pc == 0 ? kLineC : kLine); g.fillRect (gr.getX(), y + rh - 1.0f, gr.getWidth(), 1.0f);
    }
    // steps past the length are dimmed
    if (len < 16) { g.setColour (juce::Colours::black.withAlpha (0.45f)); g.fillRect (gr.getX() + len * cw, gr.getY(), (16 - len) * cw, b.getHeight() - gr.getY()); }
    for (int c = 0; c <= 16; ++c)
    {
        const float x = gr.getX() + c * cw;
        g.setColour (c % 16 == 0 ? kBar : (c % 4 == 0 ? kBeat : kLine));
        g.fillRect (x, gr.getY(), c % 4 == 0 ? 1.5f : 1.0f, gr.getHeight());
        g.fillRect (x, vl.getY(), 1.0f, vl.getHeight());
    }
    // ruler (bar.beat of the step grid; a step is a 16th at the default rate)
    g.setColour (juce::Colour (0xff1f2323)); g.fillRect (b.getX(), b.getY(), b.getWidth(), (float) kRulerH);
    g.setFont (heros (9.5f, true));
    for (int c = 0; c < 16; ++c)
    {
        const float x = gr.getX() + c * cw;
        if (c % 4 == 0) { g.setColour (kKeyText); g.drawText (c == 0 ? juce::String ("1") : "1." + juce::String (c / 4 + 1), juce::Rectangle<float> (x + 3.0f, 2.0f, 40.0f, 16.0f), juce::Justification::centredLeft, false);
                          g.setColour (kKeyTextDim); g.fillRect (x, 5.0f, 1.0f, (float) kRulerH - 5.0f); }
        else { g.setColour (kKeyTextDim.withAlpha (0.6f)); g.fillRect (x, (float) kRulerH - 5.0f, 1.0f, 5.0f); }
    }
    // piano keys
    {
        const juce::Rectangle<float> kb (b.getX(), gr.getY(), (float) kKeysW, gr.getHeight());
        g.setColour (Pal::cream); g.fillRect (kb);
        for (int k = low; k < low + kKeys; ++k)
        {
            const float y = yOfKey ((float) k);
            const int pc = ((k % 12) + 12) % 12;
            if (pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10) { g.setColour (juce::Colour (0xff161616)); g.fillRect (kb.getX(), y + 1.0f, kKeysW * 0.6f, rh - 2.0f); }
            if (pc == 0 || pc == 5) { g.setColour (Pal::rule); g.fillRect (kb.getX(), y + rh - 1.0f, kb.getWidth(), 1.0f); }
            if (pc == 0) { g.setColour (Pal::ink); g.setFont (heros (8.0f, true)); g.drawText (keyName (k), juce::Rectangle<float> (kb.getX(), y, kb.getWidth() - 3.0f, rh), juce::Justification::centredRight, false); }
        }
        g.setColour (Pal::gold); g.setFont (heros (8.0f, true));
        g.drawText ("VEL", juce::Rectangle<float> (b.getX(), vl.getY(), (float) kKeysW, vl.getHeight()), juce::Justification::centred, false);
    }
    // notes
    const float rowUp = v ("tuning.row_up") * 12.0f / juce::jmax (1.0f, v ("tuning.edo")), rowDown = v ("tuning.row_down") * 12.0f / juce::jmax (1.0f, v ("tuning.edo"));
    juce::Graphics::ScopedSaveState clip (g);
    g.reduceClipRegion (gr.toNearestInt().getUnion (vl.toNearestInt()));
    for (int s = 0; s < 16; ++s)
    {
        const auto& x = a[(size_t) s];
        if (! x.on) continue;
        const float shift = x.row == 2 ? rowUp : (x.row == 0 ? -rowDown : 0.0f);
        const bool micro = x.row != 1 && shift != 0.0f;
        const float x0 = gr.getX() + s * cw + 1.5f, w = juce::jmax (6.0f, cw * juce::jlimit (0.15f, 4.0f, x.slide ? juce::jmax (1.0f, x.gate) : x.gate) - 3.0f);
        auto base = micro ? Pal::gold : kLed;
        const auto fill = base.interpolatedWith (juce::Colour (0xff184630), 0.65f * (1.0f - juce::jlimit (0.0f, 1.0f, x.vel))).withMultipliedAlpha (0.45f + 0.55f * x.prob);
        std::vector<int> keys { x.note }; for (int c : x.chord) if (c > 0) keys.push_back (x.note + c);
        for (size_t i = 0; i < keys.size(); ++i)
        {
            const float y = yOfKey ((float) keys[i] + shift) + 1.0f;
            const juce::Rectangle<float> r (x0, y, w, rh - 2.0f);
            g.setColour (fill); g.fillRect (r);
            g.setColour (fill.darker (0.7f)); g.drawRect (r, 1.0f);
            if (x.ratchet > 1) { g.setColour (juce::Colours::black.withAlpha (0.5f)); for (int k = 1; k < x.ratchet; ++k) g.fillRect (r.getX() + r.getWidth() * k / x.ratchet, r.getY(), 1.0f, r.getHeight()); }
            if (w > 24.0f && rh >= 8.0f) { g.setColour (juce::Colour (0xff062010)); g.setFont (heros (juce::jmin (8.0f, rh - 2.0f), true)); g.drawText (keyName (keys[i]), r.reduced (2.0f, 0.0f), juce::Justification::centredLeft, false); }
            if (micro && i == 0)
            {
                g.setColour (Pal::gold); g.setFont (mono (8.5f));
                const juce::String c = juce::String (x.row == 2 ? "+" : "-") + juce::String (std::abs (shift) * 100.0f, 1);
                g.drawText (c, juce::Rectangle<float> (r.getRight() + 2.0f, r.getY() - 1.0f, 44.0f, rh), juce::Justification::centredLeft, false);
            }
        }
        if (x.accent)
        {
            const float y = yOfKey ((float) keys.back() + shift) - 2.0f;
            juce::Path t; t.addTriangle (x0, y - 6.0f, x0 + 8.0f, y - 6.0f, x0 + 4.0f, y); g.setColour (Pal::red); g.fillPath (t);
        }
        if (x.slide)
        {   // a line to the next note that plays
            for (int d = 1; d < 16; ++d)
            {
                const auto& n = a[(size_t) ((s + d) % 16)];
                if (! n.on) continue;
                if (s + d >= 16) break;
                const float nShift = n.row == 2 ? rowUp : (n.row == 0 ? -rowDown : 0.0f);
                g.setColour (kLed.withAlpha (0.8f));
                g.drawLine (x0 + w, yOfKey ((float) x.note + shift) + rh * 0.5f, gr.getX() + (s + d) * cw + 1.5f, yOfKey ((float) n.note + nShift) + rh * 0.5f, 1.6f);
                break;
            }
        }
        // velocity stem
        const float sx = gr.getX() + s * cw + 5.0f, top = vl.getBottom() - (vl.getHeight() - 4.0f) * juce::jlimit (0.0f, 1.0f, x.vel);
        g.setColour (s == sel ? Pal::cream : fill.withAlpha (1.0f)); g.fillRect (sx, top, 1.6f, vl.getBottom() - top); g.fillRect (sx - 3.0f, top - 1.5f, 7.6f, 3.0f);
        if (x.prob < 0.999f) { g.setColour (kKeyTextDim); g.setFont (mono (8.0f)); g.drawText (juce::String (juce::roundToInt (x.prob * 100.0f)) + "%", juce::Rectangle<float> (sx + 6.0f, vl.getY(), 36.0f, 12.0f), juce::Justification::centredLeft, false); }
    }
    // the selected step
    {
        const float x0 = gr.getX() + sel * cw;
        g.setColour (Pal::cream.withAlpha (0.10f)); g.fillRect (x0, gr.getY(), cw, gr.getHeight());
        g.setColour (Pal::cream.withAlpha (0.55f)); g.drawRect (x0, gr.getY(), cw, gr.getHeight(), 1.0f);
    }
    // the playhead
    if (play >= 0 && play < 16)
    {
        const float x = gr.getX() + play * cw;
        g.setColour (Pal::cream.withAlpha (0.08f)); g.fillRect (x, gr.getY(), cw, gr.getHeight());
        g.setColour (Pal::cream); g.fillRect (x, 0.0f, 1.5f, b.getHeight());
    }
    g.setColour (juce::Colour (0xff3a3835)); g.drawRect (gr.getUnion (vl), 1.0f);
}

void PianoRoll::mouseDown (const juce::MouseEvent& e)
{
    const auto p = e.position;
    dragStep = -1; dragVel = false;
    if (p.x < grid().getX()) return;
    const int s = stepAt (p.x);
    if (s >= len) return;
    select (s);
    const auto a = read();
    if (velLane().contains (p))
    {
        dragVel = true; dragStep = s;
        set (stepId (s, "vel"), juce::jlimit (0.0f, 1.0f, (velLane().getBottom() - p.y) / (velLane().getHeight() - 4.0f)));
        return;
    }
    if (! grid().contains (p)) return;
    const int k = keyAt (p.y);
    const auto& x = a[(size_t) s];
    if (e.mods.isShiftDown() && x.on)
    {   // a chord note there: add it, or remove it when it is one already
        const int rel = k - x.note;
        if (rel <= 0 || rel > 24) return;
        for (int c = 0; c < 3; ++c) if (x.chord[(size_t) c] == rel) { set (stepId (s, ("c" + juce::String (c + 1)).toRawUTF8()), 0.0f); return; }
        for (int c = 0; c < 3; ++c) if (x.chord[(size_t) c] == 0) { set (stepId (s, ("c" + juce::String (c + 1)).toRawUTF8()), (float) rel); return; }
        return;
    }
    if (! x.on) { set (stepId (s, "on"), 1.0f); set (stepId (s, "note"), (float) juce::jlimit (-24, 24, k)); }
    else if (k != x.note) { bool chordHit = false; for (int c : x.chord) chordHit |= c > 0 && x.note + c == k; if (! chordHit) set (stepId (s, "note"), (float) juce::jlimit (-24, 24, k)); }
    dragStep = s; dragNote = juce::roundToInt (v (stepId (s, "note"))); dragStartKey = k;
}

void PianoRoll::mouseDrag (const juce::MouseEvent& e)
{
    if (dragStep < 0) return;
    if (dragVel) { set (stepId (dragStep, "vel"), juce::jlimit (0.0f, 1.0f, (velLane().getBottom() - e.position.y) / (velLane().getHeight() - 4.0f))); return; }
    const int k = keyAt (e.position.y);
    const int n = juce::jlimit (-24, 24, dragNote + k - dragStartKey);
    if (n != juce::roundToInt (v (stepId (dragStep, "note")))) set (stepId (dragStep, "note"), (float) n);
}

void PianoRoll::mouseUp (const juce::MouseEvent&) { dragStep = -1; dragVel = false; }

void PianoRoll::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (! grid().contains (e.position)) return;
    const int s = stepAt (e.position.x);
    if (v (stepId (s, "on")) > 0.5f) set (stepId (s, "on"), 0.0f);
}

void PianoRoll::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    low = juce::jlimit (-36, 24, low + (w.deltaY > 0 ? 1 : -1) * (std::abs (w.deltaY) > 0.3f ? 3 : 1));
    repaint();
}

//==============================================================================
SeqGenPanel::SeqGenPanel (JingFangProcessor& p, KnobMaker makeKnob, std::function<int()> current, std::map<juce::String, juce::Component*> ctl, juce::Point<int> o)
    : proc (p), pianoRoll (p, std::move (current)), stepCtl (std::move (ctl)), origin (o)
{
    auto add = [this] (juce::Component* c, juce::Rectangle<int> b) { owned.emplace_back (c); addAndMakeVisible (c); c->setBounds (b); return c; };
    if (proc.apvts.getParameter ("seq.direction") != nullptr)
    {
        auto* dir = static_cast<TypeStepper*> (add (new TypeStepper (proc, "seq.direction"), { 8, 40, 108, 22 }));
        dir->setTooltip ("The order the steps play: forward, reverse, bounce (there and back) or random (the same random every time round)");
    }
    auto key = [&] (const char* text, juce::Rectangle<int> r, const char* tip, std::function<void()> f)
    {
        auto* k = static_cast<PanelKey*> (add (new PanelKey (text), r));
        k->fontPx = 10.5f; k->setTooltip (tip); k->onClick = std::move (f);
        return k;
    };
    key ("ROLL", { 124, 8, 84, 28 }, "A new pattern for this sound: new rhythm and new notes. It never repeats one of your last 16 rolls.", [this] { proc.genRoll (0, 0); });
    key ("RHYTHM", { 214, 8, 84, 28 }, "A new rhythm, the same notes (the NOTES seed stays)", [this] { proc.genRoll (0, 1); });
    key ("NOTES", { 304, 8, 84, 28 }, "New notes, the same rhythm (the RHYTHM seed stays)", [this] { proc.genRoll (0, 2); });
    struct GenKnob { const char* id; const char* tip; };
    const GenKnob ks[] = { { "gen.density", "DENSITY: how many steps play. Turning it after a roll adds or removes notes of the same pattern" },
                           { "gen.complexity", "COMPLEX: off-beats, leaps, ratchets, chance and slides" },
                           { "gen.mutate", "MUTATE: how much of the pattern the next roll changes (1 = all of it, 0.2 = about 3 of 16 steps)" },
                           { "gen.range", "RANGE: how many keys the line may span" },
                           { "gen.micro", "MICRO: how often the raised / lowered notes come (keyboard on MICRO only; NORMAL writes whole keys)" } };
    for (int i = 0; i < 5; ++i)
    {
        if (proc.apvts.getParameter (ks[i].id) == nullptr) continue;
        auto& kn = makeKnob (ks[i].id, "", juce::Rectangle<int> (34, 34).withCentre ({ 430 + i * 58, 24 }), this);
        kn.setPrinted (true); kn.setTooltip (ks[i].tip);
        if (i == 4) microKnob = &kn;
    }
    if (proc.apvts.getParameter ("gen.scale") != nullptr)
    {
        auto* sc = static_cast<TypeStepper*> (add (new TypeStepper (proc, "gen.scale"), { 998, 6, 140, 24 }));
        sc->setTooltip ("The scale: auto (each roll picks one) or a fixed one. With the Tuning Lab on, the Lab's own degrees are used.");
    }
    auto* prev = key ("", { 998, 36, 68, 24 }, "The roll before", [this] { proc.genHistory (0, -1); });
    prev->arrow = -1;
    auto* next = key ("", { 1070, 36, 68, 24 }, "The next roll (after stepping back)", [this] { proc.genHistory (0, +1); });
    next->arrow = 1;
    addAndMakeVisible (pianoRoll);
    pianoRoll.setBounds (8, 76, 984, 319);
    pianoRoll.onSelect = [this] (int s) { selectStep (s); };
    selectStep (0);
}

PanelKey* SeqGenPanel::key (const juce::String& text) const
{
    for (auto& c : owned) if (auto* k = dynamic_cast<PanelKey*> (c.get())) if (k->text == text) return k;
    return nullptr;
}

void SeqGenPanel::selectStep (int s)
{
    // the selected step's own controls in the column; the other steps' are hidden
    struct Place { const char* f; int x, y, d; };
    static const Place places[] = { { "note", 1035, 140, 38 }, { "vel", 1105, 140, 38 }, { "gate", 1035, 205, 38 }, { "prob", 1105, 205, 38 },
                                    { "ratchet", 1035, 270, 38 }, { "row", 1105, 270, 34 }, { "slide", 1035, 335, 30 }, { "accent", 1105, 335, 30 } };
    for (auto& [id, c] : stepCtl)
    {
        const int st = id.fromFirstOccurrenceOf ("seq.s", false, false).upToFirstOccurrenceOf (".", false, false).getIntValue() - 1;
        const auto f = id.fromLastOccurrenceOf (".", false, false);
        bool shown = false;
        for (auto& pl : places)
            if (f == pl.f && st == s)
            {
                const auto r = juce::Rectangle<int> (pl.d + 10, pl.d + 10).withCentre (origin + juce::Point<int> (pl.x, pl.y));
                c->setBounds (r); shown = true;
            }
        c->setVisible (shown);
        if (shown) c->toFront (false);
    }
    if (pianoRoll.selected() != s) pianoRoll.select (s);
    repaint (juce::Rectangle<int> (996, 76, 150, 325));
}

void SeqGenPanel::tick()
{
    pianoRoll.tick();
    const bool microOk = proc.genMicroAllowed();
    if (microKnob != nullptr && microOk != microShown)
    {
        microShown = microOk;
        microKnob->setEnabled (microOk); microKnob->setAlpha (microOk ? 1.0f : 0.3f);
        microKnob->setTooltip (microOk ? "MICRO: how often the raised / lowered notes come" : "MICRO works with the keyboard on MICRO; NORMAL writes whole keys only");
        repaint (juce::Rectangle<int> (640, 40, 60, 30));
    }
    const auto& u = proc.genUnit (0);
    const auto writes = proc.genWrites (0);
    const juce::String seeds = u.r < 0 ? juce::String ("-") : "R " + juce::String (u.r).paddedLeft ('0', 4) + juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 N ")) + juce::String (juce::jmax (0, u.n)).paddedLeft ('0', 4)
                                                             + (u.hist.size() > 1 ? "   " + juce::String (u.pos + 1) + "/" + juce::String ((int) u.hist.size()) : juce::String());
    if (writes != shownWrites || seeds != shownSeeds) { shownWrites = writes; shownSeeds = seeds; repaint (juce::Rectangle<int> (696, 0, 300, 70)); }
}

void SeqGenPanel::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff201f1e), 0.0f, 0.0f, juce::Colour (0xff171615), 0.0f, b.getHeight(), false));
    g.fillRoundedRectangle (b, 5.0f);
    g.setColour (juce::Colours::black.withAlpha (0.7f)); g.drawRoundedRectangle (b.reduced (0.5f), 5.0f, 1.0f);
    g.setColour (Pal::gold); g.setFont (heros (11.0f, true));
    drawTracked (g, "GENERATE", juce::Rectangle<float> (10.0f, 8.0f, 110.0f, 22.0f), heros (11.0f, true), 0.12f, juce::Justification::centredLeft);
    g.setColour (kKeyTextDim); g.setFont (heros (9.0f, false));
    const char* under[] = { "all new", "keeps the notes", "keeps the rhythm" };
    for (int i = 0; i < 3; ++i) g.drawText (under[i], juce::Rectangle<float> (124.0f + i * 90.0f, 40.0f, 84.0f, 22.0f), juce::Justification::centred, false);
    const char* names[] = { "DENSITY", "COMPLEX", "MUTATE", "RANGE", "MICRO" };
    g.setFont (heros (9.0f, true));
    for (int i = 0; i < 5; ++i)
    {
        g.setColour (i == 4 && ! microShown ? kKeyTextDim.withAlpha (0.45f) : kKeyText);
        g.drawText (names[i], juce::Rectangle<float> (60.0f, 14.0f).withCentre ({ 430.0f + i * 58.0f, 52.0f }), juce::Justification::centred, false);
    }
    // the readout: what it writes for this sound, and the seeds
    const juce::Rectangle<float> lcd (700.0f, 6.0f, 290.0f, 56.0f);
    g.setColour (juce::Colour (0xff06120f)); g.fillRoundedRectangle (lcd, 4.0f);
    g.setColour (juce::Colour (0xff3c3a37)); g.drawRoundedRectangle (lcd, 4.0f, 1.0f);
    g.setColour (Pal::gold); g.setFont (heros (8.5f, true));
    g.drawText ("WRITES", lcd.reduced (10.0f, 6.0f).removeFromTop (12.0f), juce::Justification::centredLeft, false);
    g.drawText ("SEED", lcd.reduced (10.0f, 6.0f).removeFromTop (12.0f), juce::Justification::centredRight, false);
    g.setColour (kLed); g.setFont (mono (12.5f));
    g.drawText (shownWrites, juce::Rectangle<float> (lcd.getX() + 10.0f, lcd.getY() + 20.0f, lcd.getWidth() - 20.0f, 16.0f), juce::Justification::centredLeft, true);
    g.setColour (kLed.withAlpha (0.85f)); g.setFont (mono (11.0f));
    g.drawText (shownSeeds, juce::Rectangle<float> (lcd.getX() + 10.0f, lcd.getY() + 37.0f, lcd.getWidth() - 20.0f, 14.0f), juce::Justification::centredRight, false);
    // the selected step's column
    const int s = pianoRoll.selected();
    g.setColour (Pal::gold);
    drawTracked (g, "STEP " + juce::String (s + 1), juce::Rectangle<float> (1000.0f, 82.0f, 140.0f, 16.0f), heros (11.0f, true), 0.12f, juce::Justification::centred);
    {
        auto* on = proc.apvts.getParameter (stepId (s, "on"));
        auto* nt = proc.apvts.getParameter (stepId (s, "note"));
        const bool isOn = on != nullptr && on->getValue() > 0.5f;
        g.setColour (isOn ? kLed : kKeyTextDim); g.setFont (mono (10.5f));
        g.drawText (isOn && nt != nullptr ? keyName (juce::roundToInt (nt->convertFrom0to1 (nt->getValue()))) : juce::String ("off"),
                    juce::Rectangle<float> (1000.0f, 98.0f, 140.0f, 14.0f), juce::Justification::centred, false);
    }
    struct Lbl { const char* t; int x, y; };
    static const Lbl lbls[] = { { "NOTE", 1035, 168 }, { "VEL", 1105, 168 }, { "GATE", 1035, 233 }, { "PROB", 1105, 233 }, { "RATCH", 1035, 298 }, { "ROW", 1105, 298 },
                                { "SLIDE", 1035, 360 }, { "ACC", 1105, 360 } };
    g.setFont (heros (9.0f, true)); g.setColour (kKeyText);
    for (auto& l : lbls) g.drawText (l.t, juce::Rectangle<float> (60.0f, 12.0f).withCentre ({ (float) l.x, (float) l.y + 6.0f }), juce::Justification::centred, false);
}

//==============================================================================
DrumGenBlock::DrumGenBlock (JingFangProcessor& p, SeqGenPanel::KnobMaker makeKnob) : proc (p)
{
    // one line above the step keys: ROLL < >  DENSITY o  COMPLEX o  MUTATE o   what it wrote and the seed
    auto key = [this] (const char* t, juce::Rectangle<int> r, const char* tip, std::function<void()> f)
    {
        auto* k = new PanelKey (t);
        owned.emplace_back (k); addAndMakeVisible (k); k->setBounds (r); k->fontPx = 10.0f; k->setTooltip (tip); k->onClick = std::move (f);
        return k;
    };
    roll = key ("ROLL", { 0, 1, 64, 22 }, "A new beat for this kit (never one of your last 16)", [this] { proc.genRoll (1, 0); });
    key ("", { 70, 1, 30, 22 }, "The beat before", [this] { proc.genHistory (1, -1); })->arrow = -1;
    key ("", { 104, 1, 30, 22 }, "The next beat (after stepping back)", [this] { proc.genHistory (1, +1); })->arrow = 1;
    const char* ids[] = { "gen.drum_density", "gen.drum_complexity", "gen.drum_mutate" };
    const char* tips[] = { "DENSITY: how many hits (turning it after a roll adds or removes hits of the same beat)", "COMPLEX: ghost notes, busier hats, a fill at the end",
                           "MUTATE: how much of the beat the next roll changes (1 = all of it)" };
    for (int i = 0; i < 3; ++i)
    {
        if (proc.apvts.getParameter (ids[i]) == nullptr) continue;
        auto& kn = makeKnob (ids[i], "", juce::Rectangle<int> (22, 22).withCentre ({ 214 + i * 92, 12 }), this);
        kn.setPrinted (true); kn.setTooltip (tips[i]);
    }
}

void DrumGenBlock::tick()
{
    const auto& u = proc.genUnit (1);
    const auto t = proc.genWrites (1) + (u.r >= 0 ? "     SEED " + juce::String (u.r).paddedLeft ('0', 4)
                                                    + (u.hist.size() > 1 ? "   " + juce::String (u.pos + 1) + "/" + juce::String ((int) u.hist.size()) : juce::String()) : juce::String());
    if (t != shown) { shown = t; repaint(); }
}

void DrumGenBlock::paint (juce::Graphics& g)
{
    g.setFont (heros (9.0f, true)); g.setColour (kKeyText);
    const char* n[] = { "DENSITY", "COMPLEX", "MUTATE" };
    for (int i = 0; i < 3; ++i) g.drawText (n[i], juce::Rectangle<float> (140.0f + i * 92.0f, 0.0f, 60.0f, 24.0f), juce::Justification::centredRight, false);
    const juce::Rectangle<float> lcd (420.0f, 1.0f, (float) getWidth() - 422.0f, 22.0f);
    g.setColour (juce::Colour (0xff06120f)); g.fillRoundedRectangle (lcd, 3.0f);
    g.setColour (juce::Colour (0xff3c3a37)); g.drawRoundedRectangle (lcd, 3.0f, 1.0f);
    g.setColour (Pal::gold); g.setFont (heros (8.5f, true));
    g.drawText ("WRITES", lcd.reduced (8.0f, 0.0f), juce::Justification::centredLeft, false);
    g.setColour (kLed); g.setFont (mono (11.0f));
    g.drawText (shown, lcd.reduced (8.0f, 0.0f).withTrimmedLeft (52.0f), juce::Justification::centredLeft, true);
}
} // namespace jfui
