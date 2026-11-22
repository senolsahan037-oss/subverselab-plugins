// HIGH STEREO panel widgets (SPEC.md section 7, sketch v7). Drawn by hand; colours from the brand kit only.
#pragma once

#include <JuceHeader.h>
#include "Theme.h"
#include "PluginProcessor.h"

namespace hsui
{
// ---- colours: brand kit (cream / ink / gold) + the RU-8 panel grey family used by the sketch
const juce::Colour grey   { 0xffDADCD4 };   // panel ground
const juce::Colour greyHi { 0xffE6E8E1 };   // raised blocks
const juce::Colour greyLo { 0xffC8CBC2 };   // wells, rules
const juce::Colour ink    { 0xff033733 };
const juce::Colour inkSoft{ 0xff496C68 };
const juce::Colour gold   { 0xffC5A059 };
const juce::Colour cream  { 0xffF2EFE7 };
const juce::Colour light  { 0xffFCFAF4 };

inline juce::Colour mix (juce::Colour a, juce::Colour b, float t) { return a.interpolatedWith (b, juce::jlimit (0.0f, 1.0f, t)); }
/** Light colour by fill: narrow ink-soft green -> wide cream light -> gold past 100 % (vanishes in mono). */
inline juce::Colour lightFor (float w)
{
    if (w <= 1.0f) return mix (inkSoft, light, w);
    return mix (light, gold, (w - 1.0f) * 2.0f);
}

inline juce::Font bold (float px)    { return Theme::legendEm (px); }
inline juce::Font regular (float px) { return Theme::fineEm (px); }
inline juce::Font mono (float px)    { return Theme::monoEm (px); }

/** Anything that can say what it does for the bottom-left hover summary (family rule, 20 Nov). */
struct HasSummary
{
    virtual ~HasSummary() = default;
    virtual juce::String summary() const = 0;
};

// =====================================================================================================================
/** Round control (owner 20 Nov): not a knob. Name and value inside; the circle grows with a soft ripple as the value
    rises. Drag up/down (Shift = fine), wheel, click the number to type, double-click = the loaded preset's value.
    Values the product sets glide (30 % of the distance per 30 Hz tick, at least one grid step); a held control does not. */
class RoundControl : public juce::Component, public HasSummary
{
public:
    RoundControl (juce::RangedAudioParameter& p, juce::String nameIn, std::function<float()> loadedValue,
                  std::function<juce::String (float)> valueText, std::function<juce::String (float)> summaryText,
                  bool bipolarIn = false, float goldFromIn = -1.0f)
        : param (p), name (std::move (nameIn)), loaded (std::move (loadedValue)), text (std::move (valueText)),
          sum (std::move (summaryText)), bipolar (bipolarIn), goldFrom (goldFromIn)
    {
        attach = std::make_unique<juce::ParameterAttachment> (param, [this] (float v) { target = v; repaint(); }, nullptr);
        attach->sendInitialUpdate();
        shown = target;
        const auto& r = param.getNormalisableRange();
        step = r.interval > 0.0f ? r.interval : (r.end - r.start) * 0.001f;
        setRepaintsOnMouseActivity (true);
    }

    juce::String summary() const override { return sum (target); }
    float shownValue() const noexcept { return shown; }

    void paint (juce::Graphics& g) override
    {
        const auto c = getLocalBounds().toFloat().getCentre();
        const float norm = param.convertTo0to1 (shown);
        const float e = bipolar ? std::abs (norm - 0.5f) * 2.0f : norm;
        const float r0 = 30.0f, grow = 13.0f * e;
        const bool hover = isMouseOver (true);
        if (e > 0.02f)
        {
            const float ts[3] = { 0.30f, 0.18f, 0.10f };
            for (int k = 1; k <= 3; ++k)
            {
                const float rad = r0 + grow * (0.55f + 0.15f * (float) k);
                g.setColour (mix (greyHi, ink, ts[k - 1]));
                g.drawEllipse (c.x - rad, c.y - rad, rad * 2, rad * 2, 1.0f);
            }
            const float rb = r0 + grow * 0.5f;
            g.setColour (mix (greyHi, ink, 0.16f));
            g.fillEllipse (c.x - rb, c.y - rb, rb * 2, rb * 2);
        }
        g.setColour (hover ? cream : greyHi);
        g.fillEllipse (c.x - r0, c.y - r0, r0 * 2, r0 * 2);
        g.setColour (ink);
        g.drawEllipse (c.x - r0, c.y - r0, r0 * 2, r0 * 2, hover ? 3.0f : 2.0f);
        // value ring on the rim, from the top, clockwise (WIDTH: ink-soft up to 150 %, gold past it = BEHIND)
        const float frac = norm;
        if (frac > 0.001f)
        {
            const float ra = r0 - 4.5f;
            auto arc = [&] (float from, float to, juce::Colour col)
            {
                juce::Path p;
                p.addCentredArc (c.x, c.y, ra, ra, 0.0f, from * juce::MathConstants<float>::twoPi, to * juce::MathConstants<float>::twoPi, true);
                g.setColour (col);
                g.strokePath (p, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
            };
            if (goldFrom > 0.0f && frac > goldFrom) { arc (0.0f, goldFrom, inkSoft); arc (goldFrom, frac, gold); }
            else arc (0.0f, frac, goldFrom > 0.0f ? inkSoft : gold);
        }
        g.setColour (inkSoft);
        g.setFont (bold (name.length() > 6 ? 7.5f : 10.0f));
        g.drawText (name, juce::Rectangle<float> (c.x - 30, c.y - 17, 60, 12), juce::Justification::centred);
        g.setColour (ink);
        g.setFont (mono (14.0f));
        g.drawText (text (shown), valueRect(), juce::Justification::centred);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragged = false; startNorm = param.convertTo0to1 (target); startY = e.position.y;
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragged && e.getDistanceFromDragStart() < 3) return;
        if (! dragged) { dragged = true; held = true; attach->beginGesture(); }
        const float k = e.mods.isShiftDown() ? 800.0f : 200.0f;
        const float n = juce::jlimit (0.0f, 1.0f, startNorm + (startY - e.position.y) / k);
        attach->setValueAsPartOfGesture (param.convertFrom0to1 (n));
        shown = target;
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (dragged) { attach->endGesture(); held = false; dragged = false; return; }
        if (e.getNumberOfClicks() == 1 && valueRect().contains (e.position))
        {
            juce::Component::SafePointer<RoundControl> self (this);
            const auto stamp = ++clickStamp;
            juce::Timer::callAfterDelay (320, [self, stamp] { if (self != nullptr && self->clickStamp == stamp) self->openEditor(); });
        }
    }
    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        ++clickStamp;   // cancels the pending "type a value"
        attach->setValueAsCompleteGesture (loaded());
    }
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        const float n = juce::jlimit (0.0f, 1.0f, param.convertTo0to1 (target) + w.deltaY * 0.05f);
        attach->setValueAsCompleteGesture (param.convertFrom0to1 (n));
        shown = target;
    }

    int clickStamp = 0;

private:
    juce::Rectangle<float> valueRect() const
    {
        const auto c = getLocalBounds().toFloat().getCentre();
        return { c.x - 28, c.y - 4, 56, 16 };
    }
    void openEditor()
    {
        ed = std::make_unique<juce::TextEditor>();
        ed->setJustification (juce::Justification::centred);
        ed->setFont (mono (13.0f));
        ed->setColour (juce::TextEditor::backgroundColourId, cream);
        ed->setColour (juce::TextEditor::textColourId, ink);
        ed->setColour (juce::TextEditor::outlineColourId, ink);
        ed->setText (param.getCurrentValueAsText(), false);
        ed->selectAll();
        ed->setBounds (valueRect().toNearestInt().expanded (2, 2));
        addAndMakeVisible (*ed);
        ed->grabKeyboardFocus();
        // The SafePointer is made here, outside the lambdas: MSVC rejects `this` in an init-capture of a lambda nested in a
        // lambda (Windows CI run 45004976841, C2440); clang accepted it. Same behaviour on both.
        const juce::Component::SafePointer<RoundControl> self (this);
        auto commit = [this, self]
        {
            if (ed == nullptr) return;
            const auto t = ed->getText();
            const float v = param.convertFrom0to1 (param.getValueForText (t));
            attach->setValueAsCompleteGesture (v);
            juce::MessageManager::callAsync ([self] { if (self != nullptr) self->ed.reset(); });
        };
        ed->onReturnKey = commit;
        ed->onFocusLost = commit;
        ed->onEscapeKey = [self] { juce::MessageManager::callAsync ([self] { if (self != nullptr) self->ed.reset(); }); };
    }
public:
    /** Called by the editor at 30 Hz: glide the drawing to values the product sets (UI signature). */
    void tick()
    {
        if (held) { if (std::abs (shown - target) > 1e-6f) { shown = target; repaint(); } return; }
        const float d = target - shown;
        if (std::abs (d) <= 1e-6f) return;
        const float mv = std::max (std::abs (d) * 0.3f, step);
        shown = std::abs (d) <= mv ? target : shown + (d > 0 ? mv : -mv);
        repaint();
    }

private:
    juce::RangedAudioParameter& param;
    juce::String name;
    std::function<float()> loaded;
    std::function<juce::String (float)> text, sum;
    bool bipolar = false; float goldFrom = -1.0f;
    std::unique_ptr<juce::ParameterAttachment> attach;
    std::unique_ptr<juce::TextEditor> ed;
    float target = 0, shown = 0, step = 1, startNorm = 0, startY = 0;
    bool dragged = false, held = false;
};

// =====================================================================================================================
/** IN / MATCH / FIX: one slide switch (owner 20 Nov): click -> the knob slides right = on, click -> left = off. */
class SlideSwitch : public juce::Component, public HasSummary
{
public:
    SlideSwitch (juce::RangedAudioParameter& p, juce::String labelIn, std::function<juce::String (bool)> summaryText,
                 std::function<juce::String()> subText = {}, std::function<bool()> warnFn = {})
        : param (p), label (std::move (labelIn)), sum (std::move (summaryText)), sub (std::move (subText)), warn (std::move (warnFn))
    {
        attach = std::make_unique<juce::ParameterAttachment> (param, [this] (float v) { on = v > 0.5f; repaint(); }, nullptr);
        attach->sendInitialUpdate();
        pos = on ? 1.0f : 0.0f;
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }
    juce::String summary() const override { return sum (on); }
    int unitWidth() const { return 38 + 8 + juce::roundToInt (juce::GlyphArrangement::getStringWidth (bold (12.0f), label)); }

    void paint (juce::Graphics& g) override
    {
        const float y = 2.0f;
        g.setColour (mix (greyLo, ink, pos));
        g.fillRoundedRectangle (0, y, 38, 21, 10.5f);
        const float kx = 3.0f + 17.0f * pos;
        g.setColour (mix (cream, gold, pos));
        g.fillEllipse (kx, y + 3, 15, 15);
        const bool w = warn && ! on && warn();
        g.setColour (w ? gold : ink);
        g.setFont (bold (12.0f));
        g.drawText (label, juce::Rectangle<float> (46, y + 2, 140, 16), juce::Justification::centredLeft);
        if (sub)
        {
            g.setColour (gold);
            g.setFont (mono (10.5f));
            g.drawText (sub(), juce::Rectangle<float> (46, y + 20, 140, 14), juce::Justification::centredLeft);
        }
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mouseWasClicked()) attach->setValueAsCompleteGesture (on ? 0.0f : 1.0f);
    }

    /** Called by the editor at 30 Hz: slide the knob. */
    void tick()
    {
        const float t = on ? 1.0f : 0.0f;
        if (std::abs (t - pos) > 1e-3f) { pos += (t - pos) * 0.45f; if (std::abs (t - pos) < 0.02f) pos = t; repaint(); }
        else if (warn || sub) repaint();
    }

private:
    juce::RangedAudioParameter& param;
    juce::String label;
    std::function<juce::String (bool)> sum;
    std::function<juce::String()> sub;
    std::function<bool()> warn;
    std::unique_ptr<juce::ParameterAttachment> attach;
    bool on = false; float pos = 0.0f;
};

// =====================================================================================================================
/** LISTEN: one segmented control, five modes. Monitoring only, not a parameter, never saved (SPEC 1.3). */
class ListenBar : public juce::Component, public HasSummary, private juce::Timer
{
public:
    explicit ListenBar (HighStereoAudioProcessor& p) : proc (p) { setRepaintsOnMouseActivity (true); startTimerHz (10); }
    static constexpr int kModes = 5;
    static const char* modeName (int i) { static const char* n[kModes] { "STEREO", "MONO", "PHONE", "SPEAKERS", "HEADPHONES" }; return n[juce::jlimit (0, kModes - 1, i)]; }

    juce::String summary() const override
    {
        static const char* what[kModes] {
            "normal listening.",
            "the mix as a club, radio or Bluetooth speaker plays it (mono).",
            "the mix on one small phone speaker.",
            "on headphones: a speaker-like image (crossfeed).",
            "the mix as typical earbuds play it." };
        const int i = hovered >= 0 ? hovered : (int) proc.getListen();
        return juce::String ("LISTEN ") + modeName (i) + " - " + what[i] + (i == 0 ? "" : " Monitor only, not in the bounce.");
    }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        g.setColour (greyLo);
        g.fillRoundedRectangle (b, b.getHeight() * 0.5f);
        const float w = b.getWidth() / kModes;
        const int cur = (int) proc.getListen();
        for (int i = 0; i < kModes; ++i)
        {
            auto seg = juce::Rectangle<float> (b.getX() + w * (float) i, b.getY(), w, b.getHeight());
            const bool on = i == cur;
            if (on) { g.setColour (ink); g.fillRoundedRectangle (seg.reduced (3.0f), (b.getHeight() - 6) * 0.5f); }
            const auto col = on ? cream : (i == hovered ? ink : inkSoft);
            drawIcon (g, i, seg.getX() + 17.0f, seg.getCentreY(), col);
            g.setColour (col);
            g.setFont (bold (i == 4 ? 9.0f : 10.0f));
            g.drawText (modeName (i), juce::Rectangle<float> (seg.getX() + 31, seg.getY(), w - 32, seg.getHeight()), juce::Justification::centredLeft, false);
        }
    }
    void mouseMove (const juce::MouseEvent& e) override { const int h = segAt (e.position.x); if (h != hovered) { hovered = h; repaint(); } }
    void mouseExit (const juce::MouseEvent&) override { hovered = -1; repaint(); }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mouseWasClicked()) { proc.setListen ((svl::HighStereoEngine::Listen) segAt (e.position.x)); repaint(); }
    }

private:
    int segAt (float x) const { return juce::jlimit (0, kModes - 1, (int) (x / ((float) getWidth() / kModes))); }
    void timerCallback() override { if ((int) proc.getListen() != lastMode) { lastMode = (int) proc.getListen(); repaint(); } }
    static void drawIcon (juce::Graphics& g, int i, float cx, float cy, juce::Colour c)
    {
        g.setColour (c);
        switch (i)
        {
            case 0: g.drawEllipse (cx - 10, cy - 5, 10, 10, 2); g.drawEllipse (cx, cy - 5, 10, 10, 2); break;
            case 1: g.drawRoundedRectangle (cx - 6, cy - 8, 12, 16, 2, 2); g.drawEllipse (cx - 3, cy - 1, 6, 6, 1); break;
            case 2: g.drawRoundedRectangle (cx - 5, cy - 9, 10, 18, 2, 2); g.drawLine (cx - 2, cy + 5, cx + 2, cy + 5, 1); break;
            case 3: g.drawRoundedRectangle (cx - 11, cy - 7, 8, 14, 1, 2); g.drawRoundedRectangle (cx + 3, cy - 7, 8, 14, 1, 2); break;
            default:
            {
                juce::Path arc; arc.addCentredArc (cx, cy + 2, 8, 8, 0, -juce::MathConstants<float>::halfPi, juce::MathConstants<float>::halfPi, true);
                g.strokePath (arc, juce::PathStrokeType (2.0f));
                g.fillRoundedRectangle (cx - 10, cy, 4, 7, 1.5f); g.fillRoundedRectangle (cx + 6, cy, 4, 7, 1.5f);
            }
        }
    }
    HighStereoAudioProcessor& proc;
    int hovered = -1, lastMode = -1;
};

// =====================================================================================================================
/** Presets: < [name v] > SAVE. The name opens the list (factory first, then the user's own, no limit). */
class PresetBar : public juce::Component, public HasSummary, private juce::Timer
{
public:
    explicit PresetBar (HighStereoAudioProcessor& p) : proc (p) { setRepaintsOnMouseActivity (true); startTimerHz (5); }
    juce::String summary() const override
    {
        switch (part)
        {
            case 0: return "Previous preset.";
            case 2: return "Next preset.";
            case 3: return "SAVE - keep these settings as your own preset (no limit).";
            default: return "Preset " + name() + " - click for the list. Your own presets have no limit.";
        }
    }
    void paint (juce::Graphics& g) override
    {
        const auto r = rects();
        g.setColour (greyLo); g.fillRoundedRectangle (r[0], 6); g.fillRoundedRectangle (r[2], 6);
        g.setColour (greyHi); g.fillRoundedRectangle (r[1], 6);
        g.setColour (inkSoft); g.drawRoundedRectangle (r[1].reduced (0.5f), 6, 1);
        g.setColour (ink); g.fillRoundedRectangle (r[3], 6);
        g.setColour (ink); g.setFont (bold (14.0f));
        g.drawText ("<", r[0], juce::Justification::centred); g.drawText (">", r[2], juce::Justification::centred);
        g.setFont (bold (12.0f));
        g.drawText (name(), r[1].withTrimmedLeft (10).withTrimmedRight (22), juce::Justification::centredLeft);
        g.setColour (inkSoft); g.setFont (bold (10.0f));
        g.drawText ("v", r[1].withTrimmedRight (9), juce::Justification::centredRight);
        g.setColour (cream); g.setFont (bold (10.0f));
        g.drawText ("SAVE", r[3], juce::Justification::centred);
    }
    void mouseMove (const juce::MouseEvent& e) override { part = partAt (e.position); }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! e.mouseWasClicked()) return;
        switch (partAt (e.position))
        {
            case 0: proc.loadPreset (proc.presets.neighbour (-1)); break;
            case 2: proc.loadPreset (proc.presets.neighbour (+1)); break;
            case 3: save(); break;
            default: showList(); break;
        }
        repaint();
    }

private:
    juce::String name() const { const auto n = proc.presets.currentName(); return n.isEmpty() ? juce::String ("(edited)") : n; }
    std::array<juce::Rectangle<float>, 4> rects() const
    {
        const float h = (float) getHeight();
        return { juce::Rectangle<float> (0, 0, 24, h), juce::Rectangle<float> (28, 0, 140, h),
                 juce::Rectangle<float> (172, 0, 24, h), juce::Rectangle<float> (202, 0, 42, h) };
    }
    int partAt (juce::Point<float> p) const { const auto r = rects(); for (int i = 0; i < 4; ++i) if (r[(size_t) i].contains (p)) return i; return 1; }
    void showList()
    {
        juce::PopupMenu m;
        int id = 1; juce::StringArray all;
        for (auto& n : proc.presets.factoryNames()) { m.addItem (id++, n, true, n == proc.presets.currentName()); all.add (n); }
        const auto user = proc.presets.userNames();
        if (! user.isEmpty()) m.addSeparator();
        for (auto& n : user) { m.addItem (id++, n, true, n == proc.presets.currentName()); all.add (n); }
        const auto cur = proc.presets.currentName();
        if (! proc.presets.isFactory (cur) && cur.isNotEmpty()) { m.addSeparator(); m.addItem (1000, "Delete \"" + cur + "\""); }
        juce::Component::SafePointer<PresetBar> self (this);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this), [self, all, cur] (int r)
        {
            if (self == nullptr || r == 0) return;
            if (r == 1000) self->proc.presets.remove (cur);
            else self->proc.loadPreset (all[r - 1]);
            self->repaint();
        });
    }
public:
    /** SAVE (owner 21 Nov: "ikinci preseti kaydedemiyoruz"). The dialog offers a free name, selected so typing replaces it;
        an existing user preset is replaced only after a second question; a name that cannot be saved says why and the
        dialog opens again with it, never closing silently. */
    void save (const juce::String& proposed = {}, const juce::String& problem = {})
    {
        auto& pm = proc.presets;
        const auto cur = pm.currentName();
        const auto start = proposed.isNotEmpty() ? proposed : (cur.isNotEmpty() && ! pm.isFactory (cur) ? cur : pm.suggestName());
        auto* w = new juce::AlertWindow ("Save preset", problem.isNotEmpty() ? problem : juce::String ("Name"),
                                         juce::MessageBoxIconType::NoIcon, this);
        w->addTextEditor ("name", start);
        w->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        juce::Component::SafePointer<PresetBar> self (this);
        w->enterModalState (true, juce::ModalCallbackFunction::create ([self, w] (int r)
        {
            if (self == nullptr || r != 1) return;
            self->commitSave (w->getTextEditorContents ("name").trim());
        }), true);
        if (auto* te = w->getTextEditor ("name")) { te->grabKeyboardFocus(); te->selectAll(); }
        lastDialog = w;
    }
    /** The save dialog's decision after the name is typed (public for the headless test). */
    void commitSave (const juce::String& name)
    {
        auto& pm = proc.presets;
        const auto why = pm.nameProblem (name);
        juce::Component::SafePointer<PresetBar> self (this);
        if (why.isNotEmpty()) { juce::MessageManager::callAsync ([self, name, why] { if (self != nullptr) self->save (name, why); }); return; }
        if (pm.exists (name) && askReplace)
        {
            juce::MessageManager::callAsync ([self, name]
            {
                if (self == nullptr) return;
                auto* q = new juce::AlertWindow ("Replace preset?", "\"" + name + "\" already exists. Replace it with these settings?",
                                                 juce::MessageBoxIconType::NoIcon, self.getComponent());
                q->addButton ("Replace", 1, juce::KeyPress (juce::KeyPress::returnKey));
                q->addButton ("Save as new", 2);
                q->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
                q->enterModalState (true, juce::ModalCallbackFunction::create ([self, name] (int r)
                {
                    if (self == nullptr) return;
                    if (r == 1) { self->proc.presets.save (name); self->repaint(); }
                    else if (r == 2) juce::MessageManager::callAsync ([self] { if (self != nullptr) self->save (self->proc.presets.suggestName()); });
                }), true);
                self->lastDialog = q;
            });
            return;
        }
        pm.save (name);
        repaint();
    }
    bool askReplace = true;                                    // tests switch the second question off
    juce::Component::SafePointer<juce::AlertWindow> lastDialog; // tests
private:
    void timerCallback() override { if (name() != lastName) { lastName = name(); repaint(); } }
    HighStereoAudioProcessor& proc;
    int part = 1; juce::String lastName;
};

// =====================================================================================================================
/** Phase bar (correlation of the output), -1 .. +1, filled from the centre; gold when below 0. */
class PhaseBar : public juce::Component, public HasSummary
{
public:
    explicit PhaseBar (HighStereoAudioProcessor& p) : proc (p) {}
    juce::String summary() const override
    {
        return "Correlation " + juce::String (proc.getCorrelation(), 2) + " - below 0 the mix starts to vanish in mono.";
    }
    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        const auto bar = b.withTrimmedLeft (18).withTrimmedRight (18).withSizeKeepingCentre (b.getWidth() - 36, 8);
        g.setColour (greyLo); g.fillRoundedRectangle (bar, 4);
        const float c = juce::jlimit (-1.0f, 1.0f, proc.getCorrelation()), mid = bar.getCentreX(), half = bar.getWidth() * 0.5f;
        g.setColour (c >= 0 ? ink : gold);
        if (c >= 0) g.fillRoundedRectangle (mid, bar.getY(), half * c, bar.getHeight(), 4);
        else g.fillRoundedRectangle (mid + half * c, bar.getY(), -half * c, bar.getHeight(), 4);
        g.setColour (inkSoft);
        g.drawLine (mid, bar.getY() - 4, mid, bar.getBottom() + 4, 1);
        g.setFont (regular (10.0f));
        g.drawText (juce::CharPointer_UTF8 ("\xe2\x88\x92" "1"), juce::Rectangle<float> (0, b.getY(), 16, b.getHeight()), juce::Justification::centredRight);
        g.drawText ("+1", juce::Rectangle<float> (b.getRight() - 16, b.getY(), 16, b.getHeight()), juce::Justification::centredLeft);
    }
private:
    HighStereoAudioProcessor& proc;
};

// =====================================================================================================================
/** PEAK GUARD meter (owner 21 Nov: "koruyucu gorsel olarak ekle"). Not a control: shows how much the always-on guard is
    taking off the output peaks. Gold bar on a 0..6 dB scale from the left, the number under it; falls back 7.5 dB/s. */
class GuardMeter : public juce::Component, public HasSummary
{
public:
    explicit GuardMeter (HighStereoAudioProcessor& p) : proc (p) {}
    static constexpr float kScaleDb = 6.0f;

    juce::String summary() const override
    {
        return juce::String ("PEAK GUARD - keeps the output peaks from rising above the input's own peak (never under -1 dBFS, "
                             "never over 0 dBFS). Always on, no latency.")
               + (shown > 0.05f ? " Taking off " + juce::String (shown, 1) + " dB now." : juce::String (" Idle now."));
    }
    float shownDb() const noexcept { return shown; }

    /** Called by the editor at 30 Hz. */
    void tick()
    {
        const float now = -proc.takeGuardDb();                    // reduction, >= 0
        const float next = std::max (now, shown - 0.25f);          // instant up, 7.5 dB/s down
        if (std::abs (next - shown) > 1e-3f || (next <= 0.0f && shown > 0.0f)) { shown = std::max (0.0f, next); repaint(); }
    }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        const bool on = shown > 0.05f;
        g.setColour (on ? gold : inkSoft); g.setFont (bold (9.0f));
        g.drawText ("PEAK GUARD", juce::Rectangle<float> (0, 0, b.getWidth(), 12), juce::Justification::centredLeft);
        const auto well = juce::Rectangle<float> (0, 17, b.getWidth(), 8);
        g.setColour (greyLo); g.fillRoundedRectangle (well, 4);
        if (on) { g.setColour (gold); g.fillRoundedRectangle (well.withWidth (std::max (4.0f, well.getWidth() * std::min (1.0f, shown / kScaleDb))), 4); }
        g.setColour (mix (greyLo, inkSoft, 0.6f));
        for (int db = 2; db < (int) kScaleDb; db += 2) { const float x = well.getWidth() * (float) db / kScaleDb; g.drawLine (x, 26, x, 29, 1); }
        g.setColour (on ? ink : inkSoft); g.setFont (mono (12.0f));
        const auto num = (on ? juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92")) : juce::String()) + juce::String (shown, 1) + " dB";
        g.drawText (num, juce::Rectangle<float> (0, 30, b.getWidth(), 16), juce::Justification::centredLeft);
    }

private:
    HighStereoAudioProcessor& proc;
    float shown = 0.0f;
};
} // namespace hsui
