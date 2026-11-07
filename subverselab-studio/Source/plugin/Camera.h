// The camera (owner 31 Oct: "jing fangin içine kamera kontrol paneli koyalım"): the window shows a part of the panel instead
// of all of it, for a recording, a stream or a lesson. The films (tests/demo_render.cpp) had this camera since 18 Oct; this is
// the same idea live, in the editor. Only the view moves: every control stays where it is and keeps working under the camera.
//
//   Camera       the view (a rectangle of the panel in its logical units, always the window's aspect), its target and the glide
//                between them (30 Hz, 30 % of the remaining distance per tick, as the knobs), and the transform the editor puts
//                on the panel. WHOLE / FREE / FOLLOW: FOLLOW goes to the open door, else to the module last touched.
//   CameraPanel  the control surface: a map of the device with the view frame on it (click a module to go there, drag the frame
//                to move, wheel to zoom), WHOLE, FOLLOW, - / + and the zoom readout. It floats over the panel, outside the camera
//                (it never zooms with the view), and can be dragged by its title anywhere inside the window.
#pragma once
#include "Ui.h"

namespace jfui
{
class Camera
{
public:
    using Rect = juce::Rectangle<float>;
    enum class Mode { whole, free, follow };
    static constexpr float kMaxZoom = 4.0f;

    /** the part of the panel the window shows now, in panel units (kW x kH = the whole panel) */
    const Rect& view() const noexcept { return shown; }
    const Rect& target() const noexcept { return want; }
    Mode mode() const noexcept { return md; }
    bool gliding() const noexcept { return shown != want; }
    float zoom() const noexcept { return (float) kW / shown.getWidth(); }
    float targetZoom() const noexcept { return (float) kW / want.getWidth(); }
    bool isWhole() const noexcept { return md == Mode::whole && ! gliding(); }

    /** the view that frames r (a little air around it, the window's aspect, never past the panel's edge, at most kMaxZoom) */
    static Rect frameFor (Rect r);
    /** glides to frame r; FREE unless told otherwise */
    void frame (Rect r, Mode m = Mode::free);
    /** glides to the whole panel */
    void whole() { frame (Rect (0, 0, (float) kW, (float) kH), Mode::whole); }
    /** glides to a view of zoom z whose centre is c (panel units) */
    void centreOn (juce::Point<float> c, float z, Mode m = Mode::free);
    /** zooms by a factor around a point of the panel that stays where it is in the window (wheel on the map; - / + use the centre) */
    void zoomBy (float factor, juce::Point<float> around);
    /** moves the view at once, no glide (a dragged frame, like a held knob) */
    void moveTo (Rect r);
    /** FOLLOW: the thing to frame now (the open door, the touched module, or the whole panel); re-frames only when it changes */
    void setMode (Mode m);
    void follow (Rect r);

    /** one 30 Hz tick of the glide; true when the view moved */
    bool tick();
    /** the panel's transform for a window drawn at windowScale (= window width / kW) */
    juce::AffineTransform transform (float windowScale) const;

private:
    Rect shown { 0, 0, (float) kW, (float) kH }, want = shown, followed;
    Mode md = Mode::whole;
};

//==============================================================================
class CameraPanel : public juce::Component, public juce::SettableTooltipClient
{
public:
    static constexpr int kPanelW = 336, kPanelH = 174;
    struct Place { juce::String title; juce::Rectangle<int> r; };   // something the map can go to by one click

    CameraPanel (Camera&, std::function<std::vector<Place>()> places, std::function<std::pair<juce::String, juce::Rectangle<int>>()> openDoor);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override { hovered = -1; repaint(); }
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void tick();                                                     // repaints when the view moved

    std::function<void()> onClose;                                   // the x key
    juce::Rectangle<int> mapBounds() const noexcept { return map; }
    /** a panel point -> where it sits on the map, and back */
    juce::Point<float> toMap (juce::Point<float> panelPt) const;
    juce::Point<float> toPanel (juce::Point<float> mapPt) const;
    juce::Rectangle<float> onMap (juce::Rectangle<float> panelRect) const;
    PanelKey& wholeKey() noexcept { return wholeBtn; }
    PanelKey& followKey() noexcept { return followBtn; }
    PanelKey& closeKey() noexcept { return closeBtn; }
    PanelKey& inKey() noexcept { return inBtn; }
    PanelKey& outKey() noexcept { return outBtn; }
    int hoveredPlace() const noexcept { return hovered; }

private:
    int placeAt (juce::Point<float> mapPt) const;
    Camera& cam;
    std::function<std::vector<Place>()> placesNow;
    std::function<std::pair<juce::String, juce::Rectangle<int>>()> doorNow;
    juce::Rectangle<int> map, title;
    PanelKey wholeBtn { "WHOLE" }, followBtn { "FOLLOW" }, outBtn { "-" }, inBtn { "+" }, closeBtn { "x" };
    juce::ComponentDragger dragger;
    juce::ComponentBoundsConstrainer keepInside;
    bool draggingFrame = false, draggingPanel = false, moved = false;
    juce::Point<float> grab;                                          // frame drag: where the view was grabbed (panel units)
    Camera::Rect grabView;
    int hovered = -1;
    juce::String lastDrawn;
};
} // namespace jfui
