#include "Camera.h"

namespace jfui
{
namespace
{
const Camera::Rect kWhole (0, 0, (float) kW, (float) kH);
const float kAspect = (float) kW / (float) kH;
}

Camera::Rect Camera::frameFor (Rect r)
{
    if (r.isEmpty()) return kWhole;
    // a little air around it, then the window's aspect, never closer than kMaxZoom, never past the panel's edge
    r = r.expanded (juce::jmax (12.0f, r.getWidth() * 0.07f), juce::jmax (12.0f, r.getHeight() * 0.07f));
    float w = r.getWidth(), h = r.getHeight();
    if (w / h < kAspect) w = h * kAspect; else h = w / kAspect;
    const float minW = (float) kW / kMaxZoom;
    if (w < minW) { w = minW; h = w / kAspect; }
    if (w >= (float) kW) return kWhole;
    return Rect (w, h).withCentre (r.getCentre()).constrainedWithin (kWhole);
}

void Camera::frame (Rect r, Mode m)
{
    want = frameFor (r);
    md = want == kWhole && m == Mode::free ? Mode::whole : m;
}

void Camera::centreOn (juce::Point<float> c, float z, Mode m)
{
    z = juce::jlimit (1.0f, kMaxZoom, z);
    const float w = (float) kW / z;
    want = Rect (w, w / kAspect).withCentre (c).constrainedWithin (kWhole);
    md = z <= 1.0001f && m == Mode::free ? Mode::whole : m;
}

void Camera::zoomBy (float factor, juce::Point<float> around)
{
    const float z = juce::jlimit (1.0f, kMaxZoom, targetZoom() * factor);
    const float w = (float) kW / z, h = w / kAspect;
    // the point under the cursor keeps its place in the window
    const float rx = want.getWidth() > 0 ? (around.x - want.getX()) / want.getWidth() : 0.5f;
    const float ry = want.getHeight() > 0 ? (around.y - want.getY()) / want.getHeight() : 0.5f;
    want = Rect (around.x - rx * w, around.y - ry * h, w, h).constrainedWithin (kWhole);
    md = z <= 1.0001f ? Mode::whole : Mode::free;
}

void Camera::moveTo (Rect r)
{
    shown = want = r.constrainedWithin (kWhole);
    md = Mode::free;
}

void Camera::setMode (Mode m)
{
    md = m;
    if (m == Mode::whole) want = kWhole;
    if (m == Mode::follow) followed = Rect (-1, -1, 0, 0);   // the next follow() frames whatever is there now
}

void Camera::follow (Rect r)
{
    if (md != Mode::follow) return;
    if (r == followed) return;
    followed = r;
    want = frameFor (r);
}

bool Camera::tick()
{
    if (shown == want) return false;
    // the knobs' glide: 30 % of what is left per tick, at least two panel units (the grid step: under one unit it lands), so a
    // move across the whole panel settles in 15-18 ticks (0.5-0.6 s) and a short one in a few
    auto step = [] (float v, float t)
    {
        const float d = t - v;
        if (std::abs (d) < 1.0f) return t;
        float s = d * 0.3f;
        if (std::abs (s) < 2.0f) s = d > 0 ? juce::jmin (2.0f, d) : juce::jmax (-2.0f, d);
        return v + s;
    };
    shown = Rect (step (shown.getX(), want.getX()), step (shown.getY(), want.getY()), step (shown.getWidth(), want.getWidth()), step (shown.getHeight(), want.getHeight()));
    return true;
}

juce::AffineTransform Camera::transform (float windowScale) const
{
    const float f = windowScale * (float) kW / shown.getWidth();
    return juce::AffineTransform::translation (-shown.getX(), -shown.getY()).scaled (f);
}

//==============================================================================
CameraPanel::CameraPanel (Camera& c, std::function<std::vector<Place>()> places, std::function<std::pair<juce::String, juce::Rectangle<int>>()> openDoor)
    : cam (c), placesNow (std::move (places)), doorNow (std::move (openDoor))
{
    setOpaque (false);
    for (auto* k : { &wholeBtn, &followBtn, &outBtn, &inBtn, &closeBtn }) addAndMakeVisible (k);
    wholeBtn.fontPx = followBtn.fontPx = 9.5f;
    outBtn.fontPx = inBtn.fontPx = 13.0f;
    closeBtn.fontPx = 10.0f;
    wholeBtn.isOn = [this] { return cam.mode() == Camera::Mode::whole; };
    followBtn.isOn = [this] { return cam.mode() == Camera::Mode::follow; };
    wholeBtn.onClick = [this] { cam.whole(); repaint(); };
    followBtn.onClick = [this] { cam.setMode (cam.mode() == Camera::Mode::follow ? Camera::Mode::free : Camera::Mode::follow); repaint(); };
    outBtn.onClick = [this] { cam.zoomBy (1.0f / 1.25f, cam.target().getCentre()); repaint(); };
    inBtn.onClick = [this] { cam.zoomBy (1.25f, cam.target().getCentre()); repaint(); };
    closeBtn.onClick = [this] { if (onClose) onClose(); };
    wholeBtn.setTooltip ("The whole panel");
    followBtn.setTooltip ("The camera follows: the open page, else the module you touch");
    outBtn.setTooltip ("Further away");
    inBtn.setTooltip ("Closer");
    closeBtn.setTooltip ("Hide the camera controls (the view stays as it is)");
    setTooltip ("Click a module on the map to go there, drag the frame to move the view, wheel to zoom. Drag the title to move these controls.");
    setSize (kPanelW, kPanelH);
}

void CameraPanel::resized()
{
    title = juce::Rectangle<int> (0, 0, getWidth(), 24);
    map = juce::Rectangle<int> (10, 30, 206, juce::roundToInt (206.0f / (float) kW * (float) kH));
    const int x = map.getRight() + 10, w = getWidth() - x - 10;
    wholeBtn.setBounds (x, 30, w, 22);
    followBtn.setBounds (x, 58, w, 22);
    outBtn.setBounds (x, 86, w / 2 - 2, 22);
    inBtn.setBounds (x + w / 2 + 2, 86, w - w / 2 - 2, 22);
    closeBtn.setBounds (getWidth() - 30, 3, 22, 18);
}

juce::Point<float> CameraPanel::toMap (juce::Point<float> p) const
{
    const float k = (float) map.getWidth() / (float) kW;
    return { (float) map.getX() + p.x * k, (float) map.getY() + p.y * k };
}

juce::Point<float> CameraPanel::toPanel (juce::Point<float> p) const
{
    const float k = (float) kW / (float) map.getWidth();
    return { (p.x - (float) map.getX()) * k, (p.y - (float) map.getY()) * k };
}

juce::Rectangle<float> CameraPanel::onMap (juce::Rectangle<float> panelRect) const
{
    const auto a = toMap (panelRect.getTopLeft()), b = toMap (panelRect.getBottomRight());
    return juce::Rectangle<float>::leftTopRightBottom (a.x, a.y, b.x, b.y);
}

int CameraPanel::placeAt (juce::Point<float> mapPt) const
{
    if (! map.toFloat().contains (mapPt) || ! placesNow) return -1;
    const auto p = toPanel (mapPt);
    const auto places = placesNow();
    for (size_t i = 0; i < places.size(); ++i) if (places[i].r.toFloat().contains (p)) return (int) i;
    return -1;
}

void CameraPanel::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat().reduced (0.5f);
    // the plate: the doors' graphite, lit from the top, a hairline
    g.setColour (juce::Colours::black.withAlpha (0.5f)); g.fillRoundedRectangle (b.translated (0.0f, 2.0f), 6.0f);
    juce::ColourGradient face (juce::Colour (0xff2A2E30), b.getX(), b.getY(), juce::Colour (0xff17191A), b.getX(), b.getBottom(), false);
    g.setGradientFill (face); g.fillRoundedRectangle (b, 6.0f);
    g.setColour (juce::Colours::white.withAlpha (0.08f)); g.drawHorizontalLine ((int) b.getY() + 1, b.getX() + 6.0f, b.getRight() - 6.0f);
    g.setColour (juce::Colours::black.withAlpha (0.75f)); g.drawRoundedRectangle (b, 6.0f, 1.0f);
    // title row: CAMERA, the zoom on its small screen, the x key (a child)
    g.setColour (Pal::gold);
    drawTracked (g, "CAMERA", { 12.0f, 4.0f, 140.0f, 16.0f }, heros (10.5f, true), 0.22f, juce::Justification::centredLeft);
    g.setColour (Pal::gold.withAlpha (0.35f)); g.fillRect (10.0f, 25.0f, (float) getWidth() - 20.0f, 1.0f);
    {
        const auto screen = juce::Rectangle<float> ((float) closeBtn.getX() - 70.0f, 3.0f, 62.0f, 18.0f);
        g.setColour (juce::Colour (0xff0B0F0E)); g.fillRoundedRectangle (screen, 2.0f);
        g.setColour (juce::Colours::black); g.drawRoundedRectangle (screen, 2.0f, 1.0f);
        g.setColour (kLed.withAlpha (0.92f)); g.setFont (mono (10.5f));
        g.drawText (juce::String (cam.zoom(), 1) + " x", screen, juce::Justification::centred, false);
    }
    // the map: the device, the modules as faint frames (the one under the mouse lit), the open door, the view frame
    g.setColour (juce::Colours::black); g.fillRect (map.expanded (1));
    if (const auto& img = panelImage(); img.isValid()) drawScaledCached (g, img, map.toFloat());
    else { g.setColour (juce::Colour (0xff141617)); g.fillRect (map); }
    const auto places = placesNow ? placesNow() : std::vector<Place>();
    for (size_t i = 0; i < places.size(); ++i)
    {
        const auto r = onMap (places[i].r.toFloat());
        if ((int) i == hovered) { g.setColour (Pal::gold.withAlpha (0.28f)); g.fillRect (r); g.setColour (Pal::gold); g.drawRect (r, 1.0f); }
        else { g.setColour (Pal::cream.withAlpha (0.16f)); g.drawRect (r, 1.0f); }
    }
    if (doorNow)
    {
        const auto [name, dr] = doorNow();
        if (! dr.isEmpty())
        {
            const auto r = onMap (dr.toFloat());
            g.setColour (juce::Colour (0xff1E2224).withAlpha (0.88f)); g.fillRect (r);
            g.setColour (Pal::gold.withAlpha (0.6f)); g.drawRect (r, 1.0f);
            g.setColour (Pal::gold);
            drawTracked (g, name, r, heros (9.0f, true), 0.2f, juce::Justification::centred);
        }
    }
    {   // what the window does not show, veiled; the view framed in gold; the target thin while the camera glides
        const auto vr = onMap (cam.view());
        const auto m = map.toFloat();
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRect (m.withBottom (vr.getY()));
        g.fillRect (m.withTop (vr.getBottom()));
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (m.getX(), vr.getY(), vr.getX(), vr.getBottom()));
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (vr.getRight(), vr.getY(), m.getRight(), vr.getBottom()));
        if (cam.gliding())
        {
            g.setColour (Pal::gold.withAlpha (0.4f)); g.drawRect (onMap (cam.target()), 1.0f);
        }
        g.setColour (Pal::gold); g.drawRect (vr, 1.5f);
    }
    g.setColour (Pal::gold.withAlpha (0.5f)); g.drawRect (map.toFloat().expanded (1.0f), 1.0f);
    // how the map works, printed small under the keys
    g.setColour (kKeyTextDim.withAlpha (0.85f));
    const int hx = wholeBtn.getX(), hy = outBtn.getBottom() + 10;
    g.setFont (heros (8.0f, false));
    g.drawText ("CLICK a module to go there", juce::Rectangle<int> (hx, hy, wholeBtn.getWidth(), 12), juce::Justification::centredLeft, false);
    g.drawText ("DRAG the frame to move", juce::Rectangle<int> (hx, hy + 14, wholeBtn.getWidth(), 12), juce::Justification::centredLeft, false);
    g.drawText ("WHEEL zooms, title drags", juce::Rectangle<int> (hx, hy + 28, wholeBtn.getWidth(), 12), juce::Justification::centredLeft, false);
}

void CameraPanel::mouseDown (const juce::MouseEvent& e)
{
    moved = draggingFrame = draggingPanel = false;
    if (title.contains (e.getPosition())) { draggingPanel = true; dragger.startDraggingComponent (this, e); return; }
    if (! map.contains (e.getPosition())) return;
    if (onMap (cam.view()).contains (e.position) && cam.zoom() > 1.001f)
    {
        draggingFrame = true;
        grab = toPanel (e.position);
        grabView = cam.view();
    }
}

void CameraPanel::mouseDrag (const juce::MouseEvent& e)
{
    if (draggingPanel)
    {   // anywhere inside the window, never past its edge (the window is kW x kH in these units)
        dragger.dragComponent (this, e, nullptr);
        setTopLeftPosition (juce::jlimit (0, kW - getWidth(), getX()), juce::jlimit (0, kH - getHeight(), getY()));
        return;
    }
    if (! draggingFrame) return;
    const auto d = toPanel (e.position) - grab;
    if (d.getDistanceFromOrigin() > 0.0f) moved = true;
    cam.moveTo (grabView.translated (d.x, d.y));   // a held frame does not glide
    repaint();
}

void CameraPanel::mouseUp (const juce::MouseEvent& e)
{
    const bool wasPanelDrag = draggingPanel;
    draggingPanel = false;
    if (wasPanelDrag) return;
    const bool frameDragged = draggingFrame && moved;
    draggingFrame = false;
    if (frameDragged || ! map.contains (e.getPosition()) || ! e.mouseWasClicked()) return;
    // a click on the map: a module goes to that module, elsewhere centres the view there (closer when it was the whole panel)
    const int i = placeAt (e.position);
    if (i >= 0) cam.frame (placesNow()[(size_t) i].r.toFloat());
    else cam.centreOn (toPanel (e.position), juce::jmax (2.0f, cam.targetZoom()));
    repaint();
}

void CameraPanel::mouseMove (const juce::MouseEvent& e)
{
    const int h = placeAt (e.position);
    if (h != hovered) { hovered = h; repaint (map.expanded (2)); }
}

void CameraPanel::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (! map.contains (e.getPosition()) || w.deltaY == 0.0f) return;
    cam.zoomBy (w.deltaY > 0 ? 1.15f : 1.0f / 1.15f, toPanel (e.position));
    repaint();
}

void CameraPanel::tick()
{
    juce::String sig = cam.view().toString() + "|" + juce::String ((int) cam.mode()) + "|" + cam.target().toString();
    if (doorNow) sig << "|" << doorNow().first;
    if (sig != lastDrawn) { lastDrawn = sig; repaint(); }
}
} // namespace jfui
