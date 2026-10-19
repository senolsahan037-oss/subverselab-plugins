#include "Analog.h"
#include "BinaryData.h"

namespace jfui
{
namespace
{
juce::Image loadNamed (const juce::String& resource)
{
    int size = 0;
    if (const char* data = BinaryData::getNamedResource (resource.toRawUTF8(), size))
        return juce::ImageFileFormat::loadFrom (data, (size_t) size);
    return {};
}
std::unique_ptr<juce::ParameterAttachment> attach (juce::RangedAudioParameter* p, juce::Component* c)
{
    if (p == nullptr) return nullptr;
    juce::Component::SafePointer<juce::Component> sp (c);
    return std::make_unique<juce::ParameterAttachment> (*p, [sp] (float) { if (sp != nullptr) sp->repaint(); }, nullptr);
}
int plainIndex (juce::RangedAudioParameter* p) { return p == nullptr ? 0 : juce::roundToInt (p->convertFrom0to1 (p->getValue())); }
void drawPart (juce::Graphics& g, const juce::Image& img, juce::Rectangle<float> r)
{
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    g.setOpacity (1.0f);
    g.drawImage (img, r);
}
} // namespace

const juce::var& doorsLayout()
{
    static const juce::var v = juce::JSON::parse (juce::String::fromUTF8 (BinaryData::doors_json, BinaryData::doors_jsonSize));
    return v;
}

const juce::Image& doorPlate (const juce::String& name)
{
    static std::map<juce::String, juce::Image> cache;
    auto& img = cache[name];
    if (! img.isValid()) img = loadNamed ("door_" + name + "_2x_png");
    return img;
}

const juce::Image& partImage (const juce::String& name)
{
    static std::map<juce::String, juce::Image> cache;
    auto& img = cache[name];
    if (! img.isValid()) img = loadNamed (name + "_png");
    return img;
}

juce::Rectangle<int> partBox (const juce::var& c, float d)
{
    const float cx = (float) (double) c[0], cy = (float) (double) c[1];
    const int s = juce::roundToInt (d + 6.0f);
    return { juce::roundToInt (cx - s * 0.5f), juce::roundToInt (cy - s * 0.5f), s, s };
}

//==============================================================================
BatSwitch::BatSwitch (JingFangProcessor& p, const juce::String& id, int pos, int up, int down, int mid)
    : param (p.apvts.getParameter (id)), positions (pos), upV (up), downV (down), midV (mid)
{
    attachment = attach (param, this);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    if (param != nullptr) setTooltip (param->getName (64));
}

BatSwitch::BatSwitch (std::function<int()> get, std::function<void (int)> set, int pos)
    : getter (std::move (get)), setter (std::move (set)), positions (pos)
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

int BatSwitch::position() const
{
    if (getter) return getter();
    const int v = plainIndex (param);
    if (v == upV) return 2;
    if (positions == 3 && v == midV) return 1;
    return 0;
}

void BatSwitch::setPosition (int p)
{
    if (setter) { setter (p); repaint(); return; }
    if (attachment == nullptr) return;
    attachment->setValueAsCompleteGesture ((float) (p == 2 ? upV : (p == 1 ? midV : downV)));
}

void BatSwitch::paint (juce::Graphics& g)
{
    const int p = position();
    const auto& img = partImage (p == 2 ? "switch_up" : (p == 1 ? "switch_mid" : "switch_down"));
    drawPart (g, img, getLocalBounds().toFloat().reduced (3.0f));
}

void BatSwitch::mouseUp (const juce::MouseEvent& e)
{
    // every click moves the lever (owner 18 Oct: "birçok buton işlevsiz": the old rule - upper half up, lower half down - did
    // nothing when the lever was already where the click was, which is where the lever is drawn)
    if (! isEnabled() || ! getLocalBounds().contains (e.getPosition())) return;
    const int cur = position();
    if (positions == 3) setPosition (cur == 1 ? 2 : (cur == 2 ? 0 : 1));   // mid -> high -> low -> mid
    else setPosition (cur == 2 ? 0 : 2);
}

//==============================================================================
ChickenSelector::ChickenSelector (JingFangProcessor& p, const juce::String& id, int n) : param (p.apvts.getParameter (id)), count (juce::jmax (2, n))
{
    attachment = attach (param, this);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    if (param != nullptr) setTooltip (param->getName (64));
}

// position i = the parameter's lowest value + i (a choice: 0..n-1; drums.octave: -2..+2)
int ChickenSelector::index() const
{
    if (param == nullptr) return 0;
    const float lo = param->getNormalisableRange().start;
    return juce::jlimit (0, count - 1, juce::roundToInt (param->convertFrom0to1 (param->getValue()) - lo));
}
void ChickenSelector::setIndex (int i)
{
    if (attachment == nullptr) return;
    attachment->setValueAsCompleteGesture (param->getNormalisableRange().start + (float) juce::jlimit (0, count - 1, i));
}

void ChickenSelector::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat().reduced (3.0f);
    const float a = juce::degreesToRadians (-135.0f + 270.0f * (float) index() / (float) (count - 1));
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillEllipse (b.reduced (b.getWidth() * 0.18f).translated (b.getWidth() * 0.03f, b.getWidth() * 0.06f));
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    g.drawImageTransformed (partImage ("chicken"), juce::AffineTransform::scale (b.getWidth() / (float) partImage ("chicken").getWidth())
                                                     .rotated (a, b.getWidth() * 0.5f, b.getHeight() * 0.5f).translated (b.getX(), b.getY()));
}

void ChickenSelector::mouseDown (const juce::MouseEvent&) { dragFrom = index(); dragged = false; }
void ChickenSelector::mouseDrag (const juce::MouseEvent& e)
{
    const int steps = (int) std::round ((float) (e.getDistanceFromDragStartX() - e.getDistanceFromDragStartY()) / 18.0f);
    if (steps != 0) dragged = true;
    if (dragged) setIndex (dragFrom + steps);
}
void ChickenSelector::mouseUp (const juce::MouseEvent& e)
{
    if (dragged || ! getLocalBounds().contains (e.getPosition())) return;
    setIndex ((index() + (e.mods.isShiftDown() ? count - 1 : 1)) % count);
}
void ChickenSelector::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    if (w.deltaY != 0.0f) setIndex (index() + (w.deltaY > 0 ? 1 : -1));
}

//==============================================================================
int StepKey::paintState = -1;

StepKey::StepKey (JingFangProcessor& p, const juce::String& id, int n) : param (p.apvts.getParameter (id)), states (n)
{
    attachment = attach (param, this);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setTooltip (n == 3 ? "Click: off, on, accent. Right-click: off. Drag across steps to paint." : "Click: step on or off. Right-click: off. Drag across steps to paint.");
}

int StepKey::state() const { return juce::jlimit (0, states - 1, plainIndex (param)); }
void StepKey::setState (int s) { if (attachment && s != state()) attachment->setValueAsCompleteGesture ((float) juce::jlimit (0, states - 1, s)); }

void StepKey::paint (juce::Graphics& g)
{
    const int s = state();
    drawPart (g, partImage (s == 0 ? "step_off" : (s == 1 ? "step_on" : "step_acc")), getLocalBounds().toFloat().reduced (3.0f));
    if (playing)
    {   // the playhead: a faint wash over the cap
        g.setColour (juce::Colours::white.withAlpha (0.10f));
        g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (getWidth() * 0.2f), 3.0f);
    }
}

void StepKey::mouseDown (const juce::MouseEvent& e)
{
    if (! isEnabled()) return;
    if (e.mods.isPopupMenu()) setState (0);                   // right-click: this step off (drag on: clear across)
    else setState ((state() + 1) % states);
    paintState = state();
    if (paintState > 0 && onLit) onLit();
}

void StepKey::mouseDrag (const juce::MouseEvent& e)
{
    // paint the first key's new state onto every key of the same row the mouse passes over
    auto* parent = getParentComponent();
    if (parent == nullptr || paintState < 0) return;
    const auto at = e.getEventRelativeTo (parent).getPosition();
    for (auto* c : parent->getChildren())
        if (auto* k = dynamic_cast<StepKey*> (c))
            if (k->getBounds().contains (at) && std::abs (k->getY() - getY()) < 4) k->setState (paintState);
}

//==============================================================================
void StepLed::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat().reduced (3.0f);
    if (lit)
    {
        juce::ColourGradient glow (juce::Colour (0x88D0140A), b.getCentreX(), b.getCentreY(), juce::Colour (0x00D0140A), b.getRight() + 4.0f, b.getCentreY(), true);
        g.setGradientFill (glow); g.fillEllipse (b.expanded (3.0f));
    }
    drawPart (g, partImage (lit ? "led_on" : "led_off"), b);
}

//==============================================================================
void ValueKnob::paint (juce::Graphics& g)
{
    const auto& img = knobImage();
    const auto box = getLocalBounds().toFloat();
    const float d = juce::jmin (box.getWidth(), box.getHeight()) - 6.0f, r = d * 0.5f;
    const auto c = box.getCentre();
    const float nn = hi > lo ? (value - lo) / (hi - lo) : 0.0f, a = juce::degreesToRadians (-135.0f + nn * 270.0f);
    g.setColour (juce::Colours::black.withAlpha (0.35f)); g.fillEllipse (juce::Rectangle<float> (d * 1.02f, d * 1.02f).withCentre (c.translated (r * 0.06f, r * 0.12f)));
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    g.setOpacity (isEnabled() ? 1.0f : 0.55f);
    g.drawImage (img, juce::Rectangle<float> (d, d).withCentre (c));
    const float rp = r * 0.86f, tw = juce::jmax (2.6f, r * 0.13f), tl = juce::jmax (4.0f, r * 0.2f);
    juce::Path t;
    t.addTriangle (0.0f, -rp - tl * 0.5f, -tw, -rp + tl * 0.5f, tw, -rp + tl * 0.5f);
    t.applyTransform (juce::AffineTransform::rotation (a).translated (c.x, c.y));
    g.setColour (juce::Colours::black.withAlpha (0.45f)); g.fillPath (t, juce::AffineTransform::translation (0.0f, 0.8f));
    g.setColour (juce::Colour (0xffEEF3F1)); g.fillPath (t);
    if (hover && format)
    {
        const float cr = r * 0.56f;
        g.setColour (juce::Colour (0xe6101213)); g.fillEllipse (juce::Rectangle<float> (cr * 2.0f, cr * 2.0f).withCentre (c));
        g.setColour (kKeyText);
        g.setFont (heros (juce::jlimit (7.0f, 11.0f, cr * 0.42f), true));
        g.drawFittedText (format (value), juce::Rectangle<int> ((int) (c.x - cr), (int) (c.y - cr), (int) (cr * 2.0f), (int) (cr * 2.0f)), juce::Justification::centred, 2);
    }
}

void ValueKnob::mouseDrag (const juce::MouseEvent& e)
{
    if (! isEnabled()) return;
    const float px = (float) (e.getDistanceFromDragStartX() - e.getDistanceFromDragStartY());
    set (from + px / 200.0f * (hi - lo));
}
} // namespace jfui
