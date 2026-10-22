#include "Ui.h"
#include "BinaryData.h"

namespace jfui
{
Faces::Faces()
{
    bold = juce::Typeface::createSystemTypefaceFor (BinaryData::texgyreherosbold_otf, (size_t) BinaryData::texgyreherosbold_otfSize);
    regular = juce::Typeface::createSystemTypefaceFor (BinaryData::texgyreherosregular_otf, (size_t) BinaryData::texgyreherosregular_otfSize);
    mono = juce::Typeface::createSystemTypefaceFor (BinaryData::DejaVuSansMonoBold_ttf, (size_t) BinaryData::DejaVuSansMonoBold_ttfSize);
}

juce::Font heros (float px, bool bold)
{
    juce::SharedResourcePointer<Faces> f;
    return juce::Font (juce::FontOptions (bold ? f->bold : f->regular).withPointHeight (px));
}

juce::Font mono (float px)
{
    juce::SharedResourcePointer<Faces> f;
    return juce::Font (juce::FontOptions (f->mono).withPointHeight (px));
}

void drawTracked (juce::Graphics& g, const juce::String& s, juce::Rectangle<float> area, juce::Font f, float trackEm, juce::Justification j)
{
    if (trackEm != 0.0f) f = f.withExtraKerningFactor (trackEm);
    g.setFont (f);
    const float shift = trackEm * f.getHeight();
    if (j.testFlags (juce::Justification::horizontallyCentred)) area = area.translated (shift * 0.5f, 0.0f);
    else if (j.testFlags (juce::Justification::right)) area = area.translated (shift, 0.0f);
    g.drawText (s, area, j, false);
}

float trackedWidth (const juce::String& s, juce::Font f, float trackEm)
{
    if (trackEm != 0.0f) f = f.withExtraKerningFactor (trackEm);
    juce::GlyphArrangement ga;
    ga.addLineOfText (f, s, 0.0f, 0.0f);
    return ga.getBoundingBox (0, -1, true).getWidth();
}

void drawArrow (juce::Graphics& g, juce::Point<float> c, float s, bool up, juce::Colour col)
{
    juce::Path p;
    const float h = s * 0.5f;
    if (up) p.addTriangle (c.x - h, c.y + h * 0.8f, c.x + h, c.y + h * 0.8f, c.x, c.y - h * 0.8f);
    else p.addTriangle (c.x - h, c.y - h * 0.8f, c.x + h, c.y - h * 0.8f, c.x, c.y + h * 0.8f);
    g.setColour (col);
    g.fillPath (p);
}

juce::Image tinted (const juce::Image& src, juce::Colour c)
{
    juce::Image out (juce::Image::ARGB, src.getWidth(), src.getHeight(), true);
    const juce::Image::BitmapData in (src, juce::Image::BitmapData::readOnly), o (out, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < src.getHeight(); ++y)
        for (int x = 0; x < src.getWidth(); ++x)
            o.setPixelColour (x, y, c.withAlpha (in.getPixelColour (x, y).getFloatAlpha()));
    return out;
}

const juce::Image& panelImage()
{
    static const juce::Image img = juce::ImageFileFormat::loadFrom (BinaryData::panel_v3_2x_png, (size_t) BinaryData::panel_v3_2x_pngSize);
    return img;
}

const juce::Image& knobStrip()
{
    static const juce::Image img;   // v3 draws knobImage() + a pointer; the v1 filmstrip is retired
    return img;
}

const juce::var& panelLayout()
{
    static const juce::var v = juce::JSON::parse (juce::String::fromUTF8 (BinaryData::layout_v3_json, BinaryData::layout_v3_jsonSize));
    return v;
}

void drawScaledCached (juce::Graphics& g, const juce::Image& src, juce::Rectangle<float> dst)
{
    if (! src.isValid() || dst.isEmpty()) return;
    const float scale = g.getInternalContext().getPhysicalPixelScaleFactor();
    const int w = juce::jmax (1, juce::roundToInt (dst.getWidth() * scale)), h = juce::jmax (1, juce::roundToInt (dst.getHeight() * scale));
    struct Entry { const void* src; int w, h; juce::Image img; };
    static std::vector<Entry> cache;                     // message thread only
    const void* key = src.getPixelData();
    for (auto& e : cache)
        if (e.src == key && e.w == w && e.h == h)
        {
            g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
            g.drawImage (e.img, dst);
            return;
        }
    if (cache.size() > 12) cache.erase (cache.begin());
    cache.push_back ({ key, w, h, src.rescaled (w, h, juce::Graphics::highResamplingQuality) });
    g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
    g.drawImage (cache.back().img, dst);
}

const juce::Image& knobImage()
{
    static const juce::Image img = juce::ImageFileFormat::loadFrom (BinaryData::knob_green_png, (size_t) BinaryData::knob_green_pngSize);
    return img;
}

void keyFace (juce::Graphics& g, juce::Rectangle<float> b, bool on, bool hover, bool down, bool led)
{
    // a machined key standing on the module: its shadow, a graphite face lit from the top, a hairline highlight, a green LED
    if (! down) { g.setColour (juce::Colours::black.withAlpha (0.55f)); g.fillRoundedRectangle (b.translated (0.0f, 1.6f), 3.0f); }
    else b = b.translated (0.0f, 1.0f);
    juce::ColourGradient face (juce::Colour (hover ? 0xff34393B : 0xff2B2F31), b.getX(), b.getY(), juce::Colour (0xff17191A), b.getX(), b.getBottom(), false);
    g.setGradientFill (face); g.fillRoundedRectangle (b, 3.0f);
    g.setColour (juce::Colours::white.withAlpha (0.10f)); g.drawHorizontalLine ((int) b.getY() + 1, b.getX() + 3.0f, b.getRight() - 3.0f);
    g.setColour (juce::Colours::black.withAlpha (0.7f)); g.drawRoundedRectangle (b, 3.0f, 1.0f);
    if (led)
    {
        const auto c = juce::Point<float> (b.getX() + 7.0f, b.getCentreY());
        if (on)
        {
            g.setColour (kLed.withAlpha (0.25f)); g.fillEllipse (juce::Rectangle<float> (9.0f, 9.0f).withCentre (c));
            g.setColour (kLed); g.fillEllipse (juce::Rectangle<float> (4.2f, 4.2f).withCentre (c));
        }
        else { g.setColour (juce::Colour (0xff0E2A22)); g.fillEllipse (juce::Rectangle<float> (4.2f, 4.2f).withCentre (c)); }
    }
}

bool chainVisible (const juce::Component* c)
{
    for (; c != nullptr; c = c->getParentComponent()) if (! c->isVisible()) return false;
    return true;
}

void setParamGesture (juce::RangedAudioParameter* p, float plain)
{
    if (p == nullptr) return;
    p->beginChangeGesture();
    p->setValueNotifyingHost (p->convertTo0to1 (plain));
    p->endChangeGesture();
}

//==============================================================================
Knob::Knob (JingFangProcessor& p, const juce::String& paramId, const juce::String& l) : proc (p), id (paramId), label (l)
{
    param = proc.apvts.getParameter (id);
    jassert (param != nullptr);
    setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    setTooltip (param->getName (64));
    bipolar = param->getNormalisableRange().start < 0.0f && std::abs (param->convertTo0to1 (0.0f) - 0.5f) < 0.03f;
    attachment = std::make_unique<juce::ParameterAttachment> (*param, [this] (float v) { onParam (v); }, nullptr);
    drawnNorm = targetNorm = param->getValue();
}

Knob::~Knob() { attachment.reset(); }

float Knob::gridStep() const
{
    const auto& r = param->getNormalisableRange();
    const float span = r.end - r.start;
    return r.interval > 0.0f && span > 0.0f ? juce::jmax (1.0e-4f, r.interval / span) : 0.005f;
}

void Knob::glideTo (float n)
{
    targetNorm = n;
    if (held || ! chainVisible (this) || std::abs (n - drawnNorm) < 1.0e-6f) { drawnNorm = n; isGliding = false; }
    else isGliding = true;
    repaint();
}

void Knob::onParam (float plain)
{
    const float n = param->convertTo0to1 (plain);
    if (held) { drawnNorm = targetNorm = n; isGliding = false; repaint(); return; }
    if (forceGlide || proc.glideArmed()) glideTo (n);
    else { drawnNorm = targetNorm = n; isGliding = false; repaint(); }
}

void Knob::tick()
{
    if (! isGliding) return;
    if (held) { drawnNorm = targetNorm; isGliding = false; repaint(); return; }
    const float step = gridStep(), t = targetNorm, v = drawnNorm;
    float move = (t - v) * 0.3f;
    if (std::abs (move) < step) move = t > v ? step : -step;
    float next = v + move;
    if (std::abs (t - v) <= step * 1.01f || (t - next) * (t - v) <= 0.0f) { next = t; isGliding = false; }
    drawnNorm = next;
    repaint();
}

void Knob::setByHand (float n)
{
    n = juce::jlimit (0.0f, 1.0f, n);
    const float plain = param->getNormalisableRange().snapToLegalValue (param->convertFrom0to1 (n));
    if (! gestureOpen) { attachment->beginGesture(); gestureOpen = true; }
    attachment->setValueAsPartOfGesture (plain);
    drawnNorm = targetNorm = param->convertTo0to1 (plain);
    isGliding = false;
    repaint();
}

void Knob::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu()) return;
    held = true; isGliding = false;
    drawnNorm = targetNorm = param->getValue();
    startNorm = drawnNorm; startY = e.position.y;
}

void Knob::mouseDrag (const juce::MouseEvent& e)
{
    if (! held) return;
    const float scale = juce::jmax (0.25f, juce::Component::getApproximateScaleFactorForComponent (this));
    const float sens = (e.mods.isShiftDown() ? 700.0f : 200.0f) / scale;
    setByHand (startNorm + (startY - e.position.y) / sens);
}

void Knob::mouseUp (const juce::MouseEvent&)
{
    held = false;
    if (gestureOpen) attachment->endGesture();
    gestureOpen = false;
}

void Knob::resetToLoaded()
{
    held = false;
    if (gestureOpen) { attachment->endGesture(); gestureOpen = false; }
    const auto& r = param->getNormalisableRange();
    const float plain = juce::jlimit (r.start, r.end, proc.loadedValue (id));   // asked now: the loaded preset's value
    forceGlide = true;
    setParamGesture (param, plain);
    forceGlide = false;
    const float n = param->getValue();
    if (std::abs (n - targetNorm) > 1.0e-6f) glideTo (n);
}

void Knob::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    if (w.deltaY == 0.0f) return;
    held = true;
    setByHand (param->getValue() + (w.deltaY > 0.0f ? 0.02f : -0.02f));
    held = false;
    if (gestureOpen) attachment->endGesture();
    gestureOpen = false;
}

void Knob::paint (juce::Graphics& g)
{
    // a physical knob on a printed scale: knurled ink skirt, cream cap, red pointer through both
    const float W = (float) getWidth();
    const float ks = juce::jmin (W - 8.0f, (float) getHeight() - 26.0f);
    const auto k = juce::Rectangle<float> (ks, ks).withCentre ({ W * 0.5f, 12.0f + ks * 0.5f });
    const float s = ks / 64.0f, cx = k.getCentreX(), cy = k.getCentreY();
    auto pt = [&] (float x, float y) { return juce::Point<float> (cx + (x - 32.0f) * s, cy + (y - 32.0f) * s); };
    const float n = juce::jlimit (0.0f, 1.0f, drawnNorm);

    if (printed && knobImage().isValid())
    {   // v3: the photoreal knob fills the component (its label and scale are printed on the panel); the pointer turns
        const auto& img = knobImage();
        const auto box = getLocalBounds().toFloat();
        const float d = juce::jmin (box.getWidth(), box.getHeight()) - 6.0f, r = d * 0.5f;
        const auto c = box.getCentre();
        const float nn = juce::jlimit (0.0f, 1.0f, drawnNorm), a = juce::degreesToRadians (-135.0f + nn * 270.0f);
        g.setColour (juce::Colours::black.withAlpha (0.35f)); g.fillEllipse (juce::Rectangle<float> (d * 1.02f, d * 1.02f).withCentre (c.translated (r * 0.06f, r * 0.12f)));
        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
        g.setOpacity (1.0f);
        g.drawImage (img, juce::Rectangle<float> (d, d).withCentre (c));
        // pointer: a small light triangle on the skirt, pointing outwards
        const float rp = r * 0.86f, tw = juce::jmax (2.6f, r * 0.13f), tl = juce::jmax (4.0f, r * 0.2f);
        juce::Path t;
        t.addTriangle (0.0f, -rp - tl * 0.5f, -tw, -rp + tl * 0.5f, tw, -rp + tl * 0.5f);
        t.applyTransform (juce::AffineTransform::rotation (a).translated (c.x, c.y));
        g.setColour (juce::Colours::black.withAlpha (0.45f)); g.fillPath (t, juce::AffineTransform::translation (0.0f, 0.8f));
        g.setColour (juce::Colour (0xffEEF3F1)); g.fillPath (t);
        if (hover || held)
        {   // the value on the cap while it is touched
            const auto txt = param->getText (nn, 0);
            const float cr = r * 0.56f;
            g.setColour (juce::Colour (0xe6101213)); g.fillEllipse (juce::Rectangle<float> (cr * 2.0f, cr * 2.0f).withCentre (c));
            g.setColour (held ? kLed : kKeyText);
            g.setFont (heros (juce::jlimit (7.0f, 11.0f, cr * 0.42f), true));
            g.drawFittedText (txt, juce::Rectangle<int> ((int) (c.x - cr), (int) (c.y - cr), (int) (cr * 2.0f), (int) (cr * 2.0f)), juce::Justification::centred, 2);
        }
        return;
    }
    g.setColour (Pal::inkSoft);
    drawTracked (g, label, { 0.0f, 0.0f, W, 10.5f }, heros (8.2f, true), 0.08f, juce::Justification::centred);

    // printed scale
    const float lo = bipolar ? juce::jmin (n, 0.5f) : 0.0f, hi = bipolar ? juce::jmax (n, 0.5f) : n;
    for (int i = 0; i <= 10; ++i)
    {
        const float f = (float) i / 10.0f;
        const float a = juce::degreesToRadians (-135.0f + 27.0f * (float) i), r1 = 28.5f, r2 = i % 5 == 0 ? 32.0f : 31.0f;
        const bool lit = f >= lo - 0.001f && f <= hi + 0.001f;
        g.setColour (lit ? Pal::ink : Pal::rule.darker (0.15f));
        g.drawLine ({ pt (32.0f + r1 * std::sin (a), 32.0f - r1 * std::cos (a)), pt (32.0f + r2 * std::sin (a), 32.0f - r2 * std::cos (a)) },
                    (i % 5 == 0 ? 2.4f : 1.7f) * s);
    }
    const float Rs = 24.0f * s, Rc = 17.0f * s;
    const float ang = juce::degreesToRadians (-135.0f + n * 270.0f);
    // cast shadow
    g.setColour (Pal::ground.withAlpha (0.28f)); g.fillEllipse (juce::Rectangle<float> (Rs * 2.08f, Rs * 2.08f).withCentre (pt (33.4f, 35.6f)));
    g.setColour (Pal::ground.withAlpha (0.18f)); g.fillEllipse (juce::Rectangle<float> (Rs * 2.3f, Rs * 2.3f).withCentre (pt (34.0f, 37.0f)));
    if (const auto& strip = knobStrip(); strip.isValid())
    {   // the rendered knob: frame round(value x 100) of the filmstrip; the frame shows 54 of the knob's 64 units
        const int fw = strip.getWidth(), frame = juce::jlimit (0, 100, juce::roundToInt (n * 100.0f));
        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
        g.setOpacity (1.0f);   // drawImage takes the current colour's alpha: the shadow's 18 % must not carry over
        const auto d = juce::Rectangle<float> (54.0f * s, 54.0f * s).withCentre ({ cx, cy });
        g.drawImage (strip, d.getX(), d.getY(), d.getWidth(), d.getHeight(), 0, frame * fw, fw, fw);
        if (hover || held)
        {
            g.setColour (held ? Pal::red : Pal::gold);
            g.drawEllipse (juce::Rectangle<float> (Rs * 2.0f + 3.0f * s, Rs * 2.0f + 3.0f * s).withCentre ({ cx, cy }), 1.4f * s);
        }
        g.setColour (Pal::ink);
        g.setFont (heros (9.0f));
        g.drawText (param->getText (n, 0), juce::Rectangle<float> (0.0f, k.getBottom() + 1.0f, W, 12.0f), juce::Justification::centred, false);
        return;
    }
    {
        juce::ColourGradient sk (Pal::groundHi.brighter (0.25f), cx - Rs * 0.6f, cy - Rs * 0.7f, Pal::ground, cx + Rs * 0.6f, cy + Rs, true);
        g.setGradientFill (sk); g.fillEllipse (juce::Rectangle<float> (Rs * 2.0f, Rs * 2.0f).withCentre ({ cx, cy }));
    }
    // knurl: turns with the knob
    for (int i = 0; i < 40; ++i)
    {
        const float a = ang + juce::MathConstants<float>::twoPi * (float) i / 40.0f;
        const auto p0 = juce::Point<float> (cx + (Rs - 3.6f * s) * std::sin (a), cy - (Rs - 3.6f * s) * std::cos (a));
        const auto p1 = juce::Point<float> (cx + (Rs - 0.6f * s) * std::sin (a), cy - (Rs - 0.6f * s) * std::cos (a));
        const float light = 0.5f + 0.5f * std::cos (a + 0.8f);   // lit from the top left
        g.setColour (Pal::dim.withAlpha (0.18f + 0.5f * light));
        g.drawLine ({ p0, p1 }, 1.0f * s);
    }
    // cap
    {
        juce::ColourGradient cap (Pal::capHi, cx - Rc * 0.45f, cy - Rc * 0.65f, Pal::capLo.darker (0.06f), cx + Rc * 0.5f, cy + Rc, true);
        g.setGradientFill (cap); g.fillEllipse (juce::Rectangle<float> (Rc * 2.0f, Rc * 2.0f).withCentre ({ cx, cy }));
        g.setColour (Pal::ground.withAlpha (0.25f)); g.drawEllipse (juce::Rectangle<float> (Rc * 2.0f, Rc * 2.0f).withCentre ({ cx, cy }), 0.8f * s);
    }
    // pointer: red line across the cap onto the skirt
    {
        const auto a0 = juce::Point<float> (cx + Rc * 0.18f * std::sin (ang), cy - Rc * 0.18f * std::cos (ang));
        const auto a1 = juce::Point<float> (cx + (Rs - 1.5f * s) * std::sin (ang), cy - (Rs - 1.5f * s) * std::cos (ang));
        g.setColour (Pal::red); g.drawLine ({ a0, a1 }, 3.0f * s);
    }
    if (hover || held)
    {
        g.setColour (held ? Pal::red : Pal::gold);
        g.drawEllipse (juce::Rectangle<float> (Rs * 2.0f + 3.0f * s, Rs * 2.0f + 3.0f * s).withCentre ({ cx, cy }), 1.4f * s);
    }
    g.setColour (Pal::ink);
    g.setFont (heros (9.0f));
    g.drawText (param->getText (n, 0), juce::Rectangle<float> (0.0f, k.getBottom() + 1.0f, W, 12.0f), juce::Justification::centred, false);
}

//==============================================================================
Segments::Segments (JingFangProcessor& p, const juce::String& paramId, juce::StringArray l) : labels (std::move (l))
{
    param = p.apvts.getParameter (paramId);
    attachment = std::make_unique<juce::ParameterAttachment> (*param, [this] (float) { repaint(); }, nullptr);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

int Segments::value() const { return juce::roundToInt (param->convertFrom0to1 (param->getValue())); }

void Segments::paint (juce::Graphics& g)
{
    const int n = labels.size();
    const auto b = getLocalBounds().toFloat().reduced (0.5f, 0.5f).withTrimmedBottom (1.5f);
    const float w = b.getWidth() / (float) n;
    for (int i = 0; i < n; ++i)
    {
        const auto r = juce::Rectangle<float> (b.getX() + w * (float) i, b.getY(), w, b.getHeight()).reduced (1.0f, 0.0f);
        const bool on = i == value();
        const bool narrow = r.getWidth() < 34.0f;   // a narrow key has no room for an LED: the active one is framed in green
        keyFace (g, r, on, false, false, ! narrow);
        if (narrow && on) { g.setColour (kLed.withAlpha (0.85f)); g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 1.2f); }
        g.setColour (on ? (narrow ? kLed : kKeyText) : kKeyTextDim);
        const auto tr = narrow ? r.reduced (1.0f, 0.0f) : r.withTrimmedLeft (10.0f).reduced (2.0f, 0.0f);
        float px = 7.6f;   // shrink a long label until it fits its key
        while (px > 5.2f && trackedWidth (labels[i], heros (px, true), 0.04f) > tr.getWidth()) px -= 0.2f;
        drawTracked (g, labels[i], tr, heros (px, true), 0.04f, juce::Justification::centred);
    }
}

void Segments::mouseUp (const juce::MouseEvent& e)
{
    if (! contains (e.getPosition())) return;
    const int i = juce::jlimit (0, labels.size() - 1, (int) ((float) e.x / ((float) getWidth() / (float) labels.size())));
    attachment->setValueAsCompleteGesture ((float) i);
}

//==============================================================================
TypeStepper::TypeStepper (JingFangProcessor& p, const juce::String& paramId)
{
    param = p.apvts.getParameter (paramId);
    attachment = std::make_unique<juce::ParameterAttachment> (*param, [this] (float) { repaint(); }, nullptr);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

int TypeStepper::value() const { return juce::roundToInt (param->convertFrom0to1 (param->getValue())); }

static int choiceCount (juce::RangedAudioParameter* p)
{
    const int i = svl::findParam (p->paramID.toStdString());
    return i >= 0 ? svl::paramDefs()[(size_t) i].numNames : 1;
}

void TypeStepper::step (int d)
{
    const int n = choiceCount (param);
    attachment->setValueAsCompleteGesture ((float) (((value() + d) % n + n) % n));
}

void TypeStepper::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat().reduced (0.5f);
    keyFace (g, b, false, false, false, false);
    const float h = b.getHeight();
    {
        juce::Path l, r;
        const float cy = b.getCentreY();
        l.addTriangle (b.getX() + h * 0.35f, cy, b.getX() + h * 0.65f, cy - 4.5f, b.getX() + h * 0.65f, cy + 4.5f);
        r.addTriangle (b.getRight() - h * 0.35f, cy, b.getRight() - h * 0.65f, cy - 4.5f, b.getRight() - h * 0.65f, cy + 4.5f);
        g.setColour (kKeyTextDim); g.fillPath (l); g.fillPath (r);
    }
    const int t = value();
    if (! (param->paramID.startsWith ("osc") && param->paramID.endsWith (".type")))
    {   // any other choice: just its name
        g.setColour (kKeyText);
        drawTracked (g, param->getCurrentValueAsText().replace ("_", " ").toUpperCase(), b.reduced (h, 0.0f), heros (9.4f, true), 0.08f,
                     juce::Justification::centred);
        return;
    }
    // the wave
    const auto w = juce::Rectangle<float> (b.getX() + h + 4.0f, b.getY() + 5.0f, 26.0f, h - 10.0f);
    juce::Path p;
    const float x0 = w.getX(), x1 = w.getRight(), yT = w.getY(), yB = w.getBottom(), yM = w.getCentreY();
    switch ((svl::OscType) t)
    {
        case svl::OscType::off: p.startNewSubPath (x0, yM); p.lineTo (x1, yM); break;
        case svl::OscType::saw: p.startNewSubPath (x0, yB); p.lineTo (w.getCentreX(), yT); p.lineTo (w.getCentreX(), yB); p.lineTo (x1, yT); break;
        case svl::OscType::square: p.startNewSubPath (x0, yB); p.lineTo (x0, yT); p.lineTo (w.getCentreX(), yT); p.lineTo (w.getCentreX(), yB); p.lineTo (x1, yB); p.lineTo (x1, yT); break;
        case svl::OscType::triangle: p.startNewSubPath (x0, yM); p.lineTo (x0 + w.getWidth() * 0.25f, yT); p.lineTo (x0 + w.getWidth() * 0.75f, yB); p.lineTo (x1, yM); break;
        case svl::OscType::sine:
            p.startNewSubPath (x0, yM);
            for (int i = 1; i <= 24; ++i) p.lineTo (x0 + w.getWidth() * (float) i / 24.0f, yM - std::sin ((float) i / 24.0f * juce::MathConstants<float>::twoPi) * w.getHeight() * 0.5f);
            break;
        case svl::OscType::noiseWhite: case svl::OscType::noisePink:
        {
            juce::Random rnd (t == (int) svl::OscType::noiseWhite ? 7 : 11);
            p.startNewSubPath (x0, yM);
            for (int i = 1; i <= 16; ++i) p.lineTo (x0 + w.getWidth() * (float) i / 16.0f, yM + (rnd.nextFloat() - 0.5f) * w.getHeight() * (t == (int) svl::OscType::noiseWhite ? 1.0f : 0.6f));
            break;
        }
        case svl::OscType::string:
            p.startNewSubPath (x0, yM);
            for (int i = 1; i <= 24; ++i)
            {
                const float f = (float) i / 24.0f;
                p.lineTo (x0 + w.getWidth() * f, yM - std::sin (f * 18.0f) * std::exp (-3.0f * f) * w.getHeight() * 0.5f);
            }
            break;
        default: break;
    }
    g.setColour (kLed);
    g.strokePath (p, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (kKeyText);
    drawTracked (g, param->getCurrentValueAsText().toUpperCase(), { w.getRight() + 6.0f, b.getY(), b.getRight() - h - w.getRight() - 6.0f, h },
                 heros (9.4f, true), 0.08f, juce::Justification::centredLeft);
}

void TypeStepper::mouseUp (const juce::MouseEvent& e)
{
    if (! contains (e.getPosition())) return;
    const int h = getHeight();
    if (e.x < h) { step (-1); return; }
    if (e.x > getWidth() - h) { step (1); return; }
    juce::PopupMenu m;
    const auto& d = svl::paramDefs()[(size_t) svl::findParam (param->paramID.toStdString())];
    for (int i = 0; i < d.numNames; ++i) m.addItem (i + 1, juce::String (d.names[i]).replace ("_", " ").toUpperCase(), true, i == value());
    juce::Component::SafePointer<TypeStepper> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this), [safe] (int r)
    {
        if (safe != nullptr && r > 0) safe->attachment->setValueAsCompleteGesture ((float) (r - 1));
    });
}

//==============================================================================
Lockup::Lockup()
{
    mark = tinted (juce::ImageFileFormat::loadFrom (BinaryData::mark_cream_png, (size_t) BinaryData::mark_cream_pngSize), Pal::cream);
    word = tinted (juce::ImageFileFormat::loadFrom (BinaryData::wordmark_cream_png, (size_t) BinaryData::wordmark_cream_pngSize), Pal::cream);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setTooltip ("subverselab.com");
}

void Lockup::paint (juce::Graphics& g)
{
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    g.setOpacity (hover ? 0.75f : 1.0f);
    const float h = (float) getHeight();
    g.drawImage (mark, juce::Rectangle<float> (0.0f, 0.0f, h * 1.41f, h), juce::RectanglePlacement::stretchToFit);
    const float ww = 60.6f, wh = ww * 90.0f / 698.0f;
    g.drawImage (word, juce::Rectangle<float> ((float) getWidth() - ww, (h - wh) * 0.5f, ww, wh), juce::RectanglePlacement::stretchToFit);
}

//==============================================================================
void Help::open() { opened = true; at = -1; setVisible (true); toFront (false); repaint(); }
void Help::close() { opened = false; at = -1; setVisible (false); if (onClosed) onClosed(); }

juce::Rectangle<int> Help::markerBounds (int i) const
{
    return juce::Rectangle<int> (20, 20).withCentre (entries[(size_t) i].marker);
}

void Help::openTip (int i)
{
    if (i < 0 || i >= count() || ! shown (i)) { at = -1; repaint(); return; }
    at = i;
    layoutTip();
    repaint();
}

void Help::layoutTip()
{
    const auto& e = entries[(size_t) at];
    juce::AttributedString s;
    s.append (e.title + "\n", heros (12.5f, true), Pal::cream);
    s.append (e.text, heros (11.0f), Pal::cream);
    s.setLineSpacing (2.0f);
    const float w = 300.0f;
    layout.createLayout (s, w - 24.0f);
    const int h = (int) std::ceil (layout.getHeight()) + 22;
    // beside the section: below it if there is room inside the panel, else above, else over its lower part
    const auto t = entries[(size_t) at].target;
    juce::Rectangle<int> r (0, 0, (int) w, h);
    r.setX (juce::jlimit (8, getWidth() - (int) w - 8, t.getRight() - (int) w - 20));
    if (t.getBottom() + 8 + h < getHeight() - 8) r.setY (t.getBottom() + 8);
    else if (t.getY() - 8 - h > 8) r.setY (t.getY() - 8 - h);
    else r.setY (juce::jlimit (8, getHeight() - h - 8, t.getBottom() - h - 8));
    tip = r;
}

void Help::paint (juce::Graphics& g)
{
    if (! opened) return;
    g.fillAll (Pal::ground.withAlpha (0.12f));
    int number = 0;
    for (int i = 0; i < count(); ++i)
    {
        if (! shown (i)) continue;
        const auto m = markerBounds (i).toFloat();
        g.setColour (i == at ? Pal::gold : Pal::ink); g.fillEllipse (m);
        g.setColour (i == at ? Pal::ink : Pal::cream);
        g.setFont (heros (11.0f, true));
        g.drawText (juce::String (++number), m, juce::Justification::centred, false);
    }
    if (at >= 0)
    {
        const auto r = tip.toFloat();
        g.setColour (Pal::ground.withAlpha (0.3f)); g.fillRoundedRectangle (r.translated (2.0f, 3.0f), 5.0f);
        g.setColour (Pal::ink); g.fillRoundedRectangle (r, 5.0f);
        g.setColour (Pal::gold); g.drawRoundedRectangle (r.reduced (0.5f), 5.0f, 1.0f);
        layout.draw (g, r.reduced (12.0f, 11.0f));
    }
}

void Help::mouseDown (const juce::MouseEvent& e)
{
    for (int i = 0; i < count(); ++i)
        if (shown (i) && markerBounds (i).expanded (3).contains (e.getPosition())) { openTip (i == at ? -1 : i); return; }
    if (at >= 0 && tip.contains (e.getPosition())) { openTip (-1); return; }
    close();
}

//==============================================================================
void PanelKey::paint (juce::Graphics& g)
{
    const bool on = isOn && isOn();
    auto b = getLocalBounds().toFloat().reduced (0.5f).withTrimmedBottom (1.5f);
    const bool led = isOn != nullptr && arrow == 0;
    keyFace (g, b, on, hover, down, led);
    if (down) b = b.translated (0.0f, 1.0f);
    if (led) b = b.withTrimmedLeft (8.0f);
    g.setColour (on ? kKeyText : (arrow != 0 ? kKeyText : kKeyTextDim.brighter (0.25f)));
    if (arrow != 0)
    {
        juce::Path t;
        const float cx = b.getCentreX(), cy = b.getCentreY(), a = (float) arrow;
        t.addTriangle (cx + 4.0f * a, cy, cx - 3.5f * a, cy - 5.0f, cx - 3.5f * a, cy + 5.0f);
        g.fillPath (t);
        return;
    }
    drawTracked (g, text, b, heros (fontPx, true), 0.08f, juce::Justification::centred);
}
} // namespace jfui
