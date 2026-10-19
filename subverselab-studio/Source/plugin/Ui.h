// JING FANG panel: colours (brand kit, the family's cream face), embedded typefaces, the knob with the SubverseLab glide and
// double-click, choice selectors, the lockup link and the "?" help popovers. No system font is ever asked for.
#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

namespace jfui
{
constexpr int kW = 1568, kH = 1024;   // logical panel size (v3: the device plus the band the dragon coils in); scaled as one piece
/** the scale the window opens at on a screen whose free area (no menu bar, no dock) is w x h: 1 when the panel and a 28 px
    title bar fit with 4 px to spare, else smaller (never below the 0.7 the window can be resized to) */
inline float fitScale (int freeW, int freeH)
{
    const float s = std::min ({ 1.0f, (float) (freeW - 8) / (float) kW, (float) (freeH - 28 - 4) / (float) kH });
    return std::max (0.7f, s);
}

struct Pal
{
    static juce::Colour c (juce::uint32 rgb) { return juce::Colour (0xff000000u | rgb); }
    // brand kit (Assets/Brand/_kit/brand.py) and the family's cream tokens (RU-8 Palette "cream")
    inline static const juce::Colour cream = c (0xF2EFE7), bed = c (0xEAE5D8), ink = c (0x033733), inkSoft = c (0x496C68),
        dim = c (0x8FA8A4), rule = c (0xCECAC0), gold = c (0xC5A059), goldDeep = c (0xA9853F), capHi = c (0xFDFBF6),
        capLo = c (0xE3DDCF), ground = c (0x032825), groundHi = c (0x033832),
        red = c (0xA8321F);   // the brand's red (site token --color-danger), the device's one loud accent
};

struct Faces
{
    juce::Typeface::Ptr bold, regular, mono;
    Faces();
};
juce::Font heros (float px, bool bold = false);
juce::Font mono (float px);
void drawTracked (juce::Graphics&, const juce::String&, juce::Rectangle<float>, juce::Font, float trackEm, juce::Justification);
float trackedWidth (const juce::String&, juce::Font, float trackEm);
void drawArrow (juce::Graphics&, juce::Point<float> centre, float size, bool up, juce::Colour);
juce::Image tinted (const juce::Image& alpha, juce::Colour);
/** the dragon that runs across the whole body, as an alpha mask the size of the panel (drawn once, cached) */
const juce::Image& dragonMask();
const juce::Image& dragonLines();
std::pair<juce::Image, juce::Image> dragonMasksScaled (float scale);   // body, lines (for the Blender scene)
/** the Blender renders (tools/blender/): the panel at 2x and the knob filmstrip (101 frames, -135..+135 degrees) */
const juce::Image& panelImage();
const juce::Image& knobStrip();
/** v3 panel (tools/blender/v3, owner 17 Oct): the layout the Blender scene and the editor share, the device render at 2x and
    the photoreal knob (static skirt / cap / highlight; only the pointer turns, drawn here) */
const juce::var& panelLayout();
const juce::Image& knobImage();
/** draws a large image (the device render, the dragon, the door plates) through a copy scaled once to the target's pixel size:
    scaling a 2x render at high quality on every repaint cost ~30 % CPU (18 Oct); caching whole components left ghosts of doors */
void drawScaledCached (juce::Graphics&, const juce::Image&, juce::Rectangle<float> destination);
/** the v3 machined key face: graphite, a hairline highlight, a green LED when on */
void keyFace (juce::Graphics&, juce::Rectangle<float>, bool on, bool hover, bool down, bool led);
inline const juce::Colour kKeyText = juce::Colour (0xffECE0C8), kKeyTextDim = juce::Colour (0xffA09482), kLed = juce::Colour (0xff27C25E);   // devil green (owner 17 Oct: "daha koyu şeytani bir yeşil")
bool chainVisible (const juce::Component*);

/** set a host parameter as one complete gesture (one undo step in the host) */
void setParamGesture (juce::RangedAudioParameter*, float plain);

//==============================================================================
/** Brand knob. The parameter takes a value at once; only the drawing glides (30 Hz, 30 % of the remaining distance, at least
    one grid step). Glides only when the product writes values (preset, double-click); a held knob, a hidden knob and a move by
    hand never glide. Double-click = the loaded preset's value for this control, asked at the click. */
class Knob : public juce::Component, public juce::SettableTooltipClient
{
public:
    Knob (JingFangProcessor&, const juce::String& paramId, const juce::String& label);
    ~Knob() override;
    void tick();
    void resetToLoaded();
    void glideTo (float norm);
    float drawn() const noexcept { return drawnNorm; }
    float target() const noexcept { return targetNorm; }
    bool gliding() const noexcept { return isGliding; }
    float parameterNorm() const { return param->getValue(); }
    const juce::String& paramId() const noexcept { return id; }
    juce::RangedAudioParameter* parameter() const noexcept { return param; }

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override { resetToLoaded(); }
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hover = false; repaint(); }
    /** v3: the label is printed on the panel, the value shows on the cap only while the knob is turned or hovered */
    void setPrinted (bool p) { printed = p; repaint(); }

private:
    void onParam (float plain);
    void setByHand (float norm);
    float gridStep() const;
    JingFangProcessor& proc;
    juce::String id, label;
    juce::RangedAudioParameter* param = nullptr;
    std::unique_ptr<juce::ParameterAttachment> attachment;
    float drawnNorm = 0, targetNorm = 0, startNorm = 0, startY = 0;
    bool held = false, gestureOpen = false, isGliding = false, forceGlide = false, hover = false, bipolar = false, printed = false;
};

//==============================================================================
/** A row of segments for a choice parameter (filter mode, voice mode, on / off). */
class Segments : public juce::Component
{
public:
    Segments (JingFangProcessor&, const juce::String& paramId, juce::StringArray labels);
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;
    int value() const;
private:
    juce::RangedAudioParameter* param;
    juce::StringArray labels;
    std::unique_ptr<juce::ParameterAttachment> attachment;
};

/** ◀ name ▶ for any choice parameter: arrows step, the name opens the list. For an oscillator type, a small drawing of the wave. */
class TypeStepper : public juce::Component
{
public:
    TypeStepper (JingFangProcessor&, const juce::String& paramId);
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;
    int value() const;
    void step (int d);
private:
    juce::RangedAudioParameter* param;
    std::unique_ptr<juce::ParameterAttachment> attachment;
};

//==============================================================================
/** The SubverseLab lockup, in ink; a link to https://subverselab.com. */
class Lockup : public juce::Component, public juce::SettableTooltipClient
{
public:
    Lockup();
    static juce::URL url() { return juce::URL ("https://subverselab.com"); }
    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hover = false; repaint(); }
    void mouseUp (const juce::MouseEvent& e) override { if (e.mouseWasClicked() && contains (e.getPosition())) url().launchInDefaultBrowser(); }
private:
    juce::Image mark, word;
    bool hover = false;
};

//==============================================================================
/** "?" help: numbered markers over the panel; a marker opens its popover beside its section. Drawn over the panel, so the
    window never changes size. Escape or a click elsewhere closes it. */
class Help : public juce::Component
{
public:
    struct Entry { juce::String title, text; juce::Rectangle<int> target; juce::Point<int> marker; };   // marker: where its number sits (beside the section's title)
    void setEntries (std::vector<Entry> e) { entries = std::move (e); }
    /** an open door covers the modules: their numbers are not shown (owner 19 Oct evening: on the LAB page "?" explained the
        hidden PITCH CLICK DRUM module); the visible ones are numbered 1, 2, 3 ... in order */
    void setCovered (juce::Rectangle<int> r) { covered = r; at = -1; repaint(); }
    bool shown (int i) const noexcept { return covered.isEmpty() || ! covered.contains (entries[(size_t) i].marker); }
    int shownCount() const noexcept { int n = 0; for (int i = 0; i < count(); ++i) n += shown (i) ? 1 : 0; return n; }
    void open();
    void close();
    bool isOpen() const noexcept { return opened; }
    void openTip (int i);
    int current() const noexcept { return at; }
    int count() const noexcept { return (int) entries.size(); }
    juce::Rectangle<int> tipBounds() const noexcept { return tip; }
    juce::Rectangle<int> markerBounds (int i) const;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    bool hitTest (int, int) override { return opened; }
    std::function<void()> onClosed;
private:
    void layoutTip();
    std::vector<Entry> entries;
    juce::Rectangle<int> covered;
    bool opened = false;
    int at = -1;
    juce::Rectangle<int> tip;
    juce::TextLayout layout;
};

/** a plain key on the panel (prev / next / save / help / normal / micro) */
class PanelKey : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit PanelKey (juce::String t) : text (std::move (t)) {}
    juce::String text;
    int arrow = 0;                     // -1 / +1: a left / right triangle instead of the text
    std::function<bool()> isOn;
    std::function<void()> onClick;
    float fontPx = 11.0f;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override { down = true; repaint(); }
    void mouseUp (const juce::MouseEvent& e) override { down = false; repaint(); if (contains (e.getPosition()) && onClick) onClick(); }
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hover = false; repaint(); }
private:
    bool down = false, hover = false;
};
} // namespace jfui
