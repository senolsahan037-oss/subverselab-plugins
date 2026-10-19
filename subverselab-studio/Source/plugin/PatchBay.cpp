#include "PatchBay.h"
#include "Scopes.h"

namespace jfui
{
namespace
{
constexpr float kJackR = 16.0f;
const juce::var& patchLayout() { return jfui::doorsLayout()["patch"]; }
juce::String mid (int slot, const char* f) { return "mod" + juce::String (slot + 1) + "." + f; }
}

PatchBay::PatchBay (JingFangProcessor& p, std::function<Knob& (const juce::String&, const juce::String&, juce::Rectangle<int>, juce::Component*)> makeKnob)
    : proc (p)
{
    setInterceptsMouseClicks (true, true);
    const auto& L = patchLayout();
    auto rect = [] (const juce::var& a) { return juce::Rectangle<int> ((int) a[0], (int) a[1], (int) a[2], (int) a[3]); };
    auto add = [this] (juce::Component* c, juce::Rectangle<int> b) { owned.emplace_back (c); addAndMakeVisible (c); c->setBounds (b); };
    // the three LFOs: shape and sync selectors, their screen; knobs and the retrigger switch from the generic lists
    if (auto* lfos = L["lfos"].getArray())
        for (int l = 0; l < juce::jmin (svl::kNumLfo, lfos->size()); ++l)
        {
            const juce::String pre = "lfo" + juce::String (l + 1) + ".";
            const auto& lf = (*lfos)[l];
            add (new TypeStepper (proc, pre + "shape"), rect (lf["shape"]));
            add (new TypeStepper (proc, pre + "sync"), rect (lf["sync"]));
            add (new LfoScope (proc, l), rect (lf["scope"]));
        }
    if (auto* sw = L["switches"].getArray())
        for (auto& s : *sw)
            if (proc.apvts.getParameter (s["id"].toString()) != nullptr)
                add (new BatSwitch (proc, s["id"].toString(), (int) s["positions"], (int) s["up"], (int) s["down"], -1), partBox (s["c"], (float) (double) s["d"]));
    if (auto* ks = L["knobs"].getArray())
        for (auto& k : *ks)
            if (proc.apvts.getParameter (k["id"].toString()) != nullptr)
                makeKnob (k["id"].toString(), "", partBox (k["c"], (float) (double) k["d"]), this).setPrinted (true);
    layoutDoor();
}

juce::Colour PatchBay::cableColour (int slot)
{
    static const juce::Colour c[] = { Pal::red, Pal::gold, Pal::cream, Pal::dim };
    return c[slot % 4];
}

juce::Point<float> PatchBay::jack (bool source, int i) const
{
    const auto& list = patchLayout()["jacks"][source ? "src" : "dst"];
    if (i < 0 || i >= list.size()) return {};
    return { (float) (double) list[i][0], (float) (double) list[i][1] };
}

int PatchBay::hitJack (juce::Point<float> p, bool& isSource) const
{
    for (int i = 0; i < kNumSrc; ++i) if (p.getDistanceFrom (jack (true, i)) <= kJackR + 6.0f) { isSource = true; return i; }
    for (int i = 0; i < kNumDst; ++i) if (p.getDistanceFrom (jack (false, i)) <= kJackR + 6.0f) { isSource = false; return i; }
    return -1;
}

int PatchBay::slotValue (int slot, svl::ModField f) const
{   // the host parameter itself (the message thread's value), so two cables laid in a row see each other
    return juce::roundToInt (proc.apvts.getRawParameterValue (mid (slot, f == svl::M_SRC ? "source" : (f == svl::M_DST ? "destination" : "amount")))->load());
}

int PatchBay::cableCount() const
{
    int c = 0;
    for (int m = 0; m < svl::kNumModSlots; ++m) if (slotValue (m, svl::M_SRC) > 0 && slotValue (m, svl::M_DST) > 0) ++c;
    return c;
}

bool PatchBay::connect (int src, int dst)
{
    for (int m = 0; m < svl::kNumModSlots; ++m)
        if (slotValue (m, svl::M_SRC) == src && slotValue (m, svl::M_DST) == dst) return true;   // already patched
    for (int m = 0; m < svl::kNumModSlots; ++m)
        if (slotValue (m, svl::M_SRC) == 0 || slotValue (m, svl::M_DST) == 0)
        {
            setParamGesture (proc.apvts.getParameter (mid (m, "source")), (float) src);
            setParamGesture (proc.apvts.getParameter (mid (m, "destination")), (float) dst);
            if (std::abs (proc.apvts.getRawParameterValue (mid (m, "amount"))->load()) < 1.0e-6f)
                setParamGesture (proc.apvts.getParameter (mid (m, "amount")), 0.5f);
            repaint();
            return true;
        }
    return false;
}

void PatchBay::disconnect (int m)
{
    setParamGesture (proc.apvts.getParameter (mid (m, "source")), 0.0f);
    setParamGesture (proc.apvts.getParameter (mid (m, "destination")), 0.0f);
    repaint();
}

void PatchBay::layoutDoor()
{
    const int h = juce::roundToInt ((float) kFullH * amount);
    setSize (kFullW, juce::jmax (1, h));
    setVisible (amount > 0.001f);
}

void PatchBay::tick()
{
    if (amount != target)
    {
        const float d = target - amount;
        amount = std::abs (d) < 0.02f ? target : amount + d * 0.42f;   // ~6 frames at 30 Hz: the door rolls down / up
        layoutDoor();
    }
    if (! isVisible()) return;
    for (auto& c : owned) if (auto* sc = dynamic_cast<Scope*> (c.get())) sc->tick();
    juce::String st;
    for (int m = 0; m < svl::kNumModSlots; ++m) st << slotValue (m, svl::M_SRC) << ":" << slotValue (m, svl::M_DST) << ":" << proc.apvts.getRawParameterValue (mid (m, "amount"))->load() << ",";
    if (st != lastState) { lastState = st; repaint(); }
}

void PatchBay::paint (juce::Graphics& g)
{
    const auto full = juce::Rectangle<float> (0.0f, 0.0f, (float) kFullW, (float) kFullH);
    {
        juce::Graphics::ScopedSaveState ss (g);
        juce::Path clip; clip.addRoundedRectangle (full, 6.0f);
        g.reduceClipRegion (clip);
        const auto& plate = doorPlate ("patch");
        if (plate.isValid()) drawScaledCached (g, plate, full);
        else { g.setColour (juce::Colour (0xff121011)); g.fillRect (full); }
    }
    g.setColour (juce::Colours::black.withAlpha (0.8f)); g.drawRoundedRectangle (full.reduced (0.5f), 6.0f, 1.2f);
    // the jacks (photoreal), a red ring on the sources (outputs)
    const auto& jackImg = partImage ("jack");
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    for (int src = 1; src >= 0; --src)
        for (int i = 0; i < (src ? kNumSrc : kNumDst); ++i)
        {
            const auto c = jack (src == 1, i);
            g.drawImage (jackImg, juce::Rectangle<float> (kJackR * 2.2f, kJackR * 2.2f).withCentre (c));
            if (src) { g.setColour (juce::Colour (0xffD0140A).withAlpha (0.8f)); g.drawEllipse (juce::Rectangle<float> (kJackR * 2.5f, kJackR * 2.5f).withCentre (c), 1.6f); }
        }
    // each amount knob carries the colour of the cable it scales (dim when its slot is free)
    if (auto* am = patchLayout()["amounts"].getArray())
        for (int m = 0; m < juce::jmin (svl::kNumModSlots, am->size()); ++m)
        {
            const bool used = slotValue (m, svl::M_SRC) > 0 && slotValue (m, svl::M_DST) > 0;
            const auto& t = (*am)[m]["tab"];
            g.setColour (used ? cableColour (m) : Pal::rule.withAlpha (0.35f));
            g.fillRoundedRectangle ({ (float) (double) t[0], (float) (double) t[1], (float) (double) t[2], (float) (double) t[3] }, 2.0f);
        }
}

void PatchBay::drawCable (juce::Graphics& g, juce::Point<float> a, juce::Point<float> b, juce::Colour c, float alpha) const
{
    const float sag = 26.0f + 0.12f * a.getDistanceFrom (b);
    juce::Path p;
    p.startNewSubPath (a);
    p.cubicTo (a.translated (0.0f, sag), b.translated (0.0f, sag), b);
    g.setColour (juce::Colours::black.withAlpha (0.45f * alpha));
    g.strokePath (p, juce::PathStrokeType (7.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded), juce::AffineTransform::translation (1.5f, 3.0f));
    g.setColour (c.darker (0.35f).withAlpha (alpha));
    g.strokePath (p, juce::PathStrokeType (6.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (c.withAlpha (alpha));
    g.strokePath (p, juce::PathStrokeType (3.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (juce::Colours::white.withAlpha (0.25f * alpha));
    g.strokePath (p, juce::PathStrokeType (1.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded), juce::AffineTransform::translation (-0.8f, -1.2f));
    for (auto e : { a, b })
    {   // plug
        g.setColour (c.darker (0.4f).withAlpha (alpha)); g.fillEllipse (juce::Rectangle<float> (14.0f, 14.0f).withCentre (e));
        g.setColour (c.withAlpha (alpha)); g.fillEllipse (juce::Rectangle<float> (10.0f, 10.0f).withCentre (e));
    }
}

void PatchBay::paintOverChildren (juce::Graphics& g)
{
    for (int m = 0; m < svl::kNumModSlots; ++m)
    {
        const int s = slotValue (m, svl::M_SRC), d = slotValue (m, svl::M_DST);
        if (s <= 0 || d <= 0) continue;
        drawCable (g, jack (true, s - 1), jack (false, d - 1), cableColour (m), 0.92f);
    }
    if (dragSrc > 0) drawCable (g, jack (true, dragSrc - 1), dragPos, Pal::cream, 0.75f);
}

void PatchBay::mouseDown (const juce::MouseEvent& e)
{
    bool src = false;
    const int j = hitJack (e.position, src);
    if (j < 0) return;
    if (src) { dragSrc = j + 1; dragPos = e.position; repaint(); return; }
    // a destination: pull out its most recent cable and carry it
    for (int m = svl::kNumModSlots - 1; m >= 0; --m)
        if (slotValue (m, svl::M_DST) == j + 1 && slotValue (m, svl::M_SRC) > 0)
        {
            dragSrc = slotValue (m, svl::M_SRC);
            disconnect (m);
            dragPos = e.position;
            return;
        }
}

void PatchBay::mouseDrag (const juce::MouseEvent& e)
{
    if (dragSrc <= 0) return;
    dragPos = e.position;
    repaint();
}

void PatchBay::mouseUp (const juce::MouseEvent& e)
{
    if (dragSrc <= 0) return;
    bool src = false;
    const int j = hitJack (e.position, src);
    if (j >= 0 && ! src) connect (dragSrc, j + 1);   // dropped anywhere else: the cable is gone
    dragSrc = -1;
    repaint();
}
} // namespace jfui
