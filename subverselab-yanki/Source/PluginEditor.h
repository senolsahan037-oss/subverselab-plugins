#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PanelKit.h"
#include "Theme.h"
#include "RingModel.h"
#if SVL_LICENSING
#include "../../subverselab-plugin-common/Licensing/LicenseOverlay.h"
#endif

/*  YANKI v2: the face of docs/ui/v2_concept_A.png (approved 2026-10-03), drawn in code with the family kit (PanelKit) at the
    concept's own coordinates: a 1600 x 900 logical face, scaled as a whole.

    The middle is the echo field: a glass display with one ring per repeat, drawn from the plug-in's parameters through
    ringmodel (radius = time of the repeat, brightness and stroke = its level after feedback, warmth = TONE and WEAR, left/right
    offset = SPREAD where the voicing has heads on both sides) and pulsing with the engine's own loop level. Three gold handles on
    it drag TIME (radial; snaps to the note values when synced), FEEDBACK (vertical) and SPREAD (sideways); the knobs below mirror
    the same parameters. WET is the hero on the left, SWELL a section on the right, the preset browser sits in the top bar and the
    moods are gone from the front (a preset sets its voicing).
*/

/** A control drawn in panel coordinates (its paint is translated back to the face). */
class PanelPart : public juce::Component
{
public:
    PanelPart() { setOpaque (false); setPaintingIsUnclipped (false); }
    void paint (juce::Graphics& g) final
    {
        g.addTransform (juce::AffineTransform::translation ((float) -getX(), (float) -getY()));
        paintPanel (g);
    }
    virtual void paintPanel (juce::Graphics&) = 0;
    /** Called by the editor's timer: repaint when what is drawn changed. */
    virtual void refresh() {}
};

/** Knob base: vertical drag (shift = fine), wheel, double-click = default, a round hit area. */
class KnobBase : public PanelPart
{
public:
    KnobBase (juce::Point<float> centre, float radius, float hitRadius, float margin);
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool hitTest (int x, int y) override;
    void refresh() override;
    /** Leave a hole in the hit area (the TIME ring around its inner cap). */
    void setHitHole (float radius) { hole = radius; }

protected:
    virtual float getU() const = 0;
    virtual void setU (float) = 0;
    virtual float defaultU() const = 0;
    virtual void begin() = 0;
    virtual void end() = 0;
    virtual float dragPixels() const { return 220.0f; }
    juce::Point<float> c;
    float r, hitR, hole = 0.0f;

private:
    float startU = 0.0f, lastU = -1.0f, startY = 0.0f;
};

/** A knob on one parameter: the machined knob, or the bakelite pointer knob on a choice parameter. */
class ParamKnob : public KnobBase
{
public:
    ParamKnob (juce::RangedAudioParameter&, juce::Point<float> centre, float radius, bool pointer = false, std::vector<float> angles = {});
    void paintPanel (juce::Graphics&) override;

protected:
    float getU() const override { return param.getValue(); }
    void setU (float u) override { param.setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, u)); }
    float defaultU() const override { return param.getDefaultValue(); }
    void begin() override { param.beginChangeGesture(); }
    void end() override { param.endChangeGesture(); }
    float dragPixels() const override { return pointer ? 36.0f * (float) juce::jmax (1, (int) angles.size() - 1) : 220.0f; }
    juce::RangedAudioParameter& param;
    bool pointer;
    std::vector<float> angles;
};

/** CAP: one knob over CAP_ON and CAP_DB (OFF at the left stop, then -12 .. +12 dB). */
class CapKnob : public KnobBase
{
public:
    CapKnob (juce::RangedAudioParameter& on, juce::RangedAudioParameter& db, juce::Point<float> centre, float radius);
    void paintPanel (juce::Graphics&) override;
    static float angleForDb (float db) { return -100.0f + 235.0f * (db + 12.0f) / 24.0f; }

protected:
    float getU() const override;
    void setU (float) override;
    float defaultU() const override { return 0.0f; }
    void begin() override { on.beginChangeGesture(); db.beginChangeGesture(); }
    void end() override { on.endChangeGesture(); db.endChangeGesture(); }

private:
    juce::RangedAudioParameter &on, &db;
};

/** A key (the family push button) with an optional lamp state. */
class Key : public PanelPart
{
public:
    Key (kit::Box box, juce::String label, kit::KeyStyle style, float size, float track);
    void paintPanel (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    bool hitTest (int x, int y) override;
    void refresh() override;

    std::function<bool()> isLit;
    std::function<void()> onPress, onRelease;

private:
    kit::Box box;
    juce::String label;
    kit::KeyStyle style;
    float size, track;
    bool down = false, lastLit = false;
};

/** The bat toggle for BYPASS: up = effect on. */
class BypassToggle : public PanelPart
{
public:
    BypassToggle (juce::RangedAudioParameter&, float cx, float cy, float scale);
    void paintPanel (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void refresh() override;

private:
    juce::RangedAudioParameter& param;
    float cx, cy, s;
    bool last = false;
};

/** A live line of text (a value under a knob, the WET readout): repainted when its text changes. */
class ValueLabel : public PanelPart
{
public:
    ValueLabel (float cx, float cy, float width, std::function<juce::String()> getText, float size = 9.0f, juce::Colour col = kit::rgb (157, 247, 221));
    void paintPanel (juce::Graphics&) override;
    void refresh() override;

private:
    float cx, cy, size;
    juce::Colour colour;
    std::function<juce::String()> getText;
    juce::String shown;
};

/** The preset browser in the top bar: previous / name / next, a menu on the name. */
class PresetBar : public PanelPart
{
public:
    explicit PresetBar (YankiProcessor&);
    void paintPanel (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    bool hitTest (int x, int y) override;
    void refresh() override;
    void step (int delta);
    static constexpr float kX0 = 470.0f, kX1 = 1130.0f, kY = 36.0f;

private:
    YankiProcessor& proc;
    int lastIndex = -2;
    bool lastEdited = false;
};

/** The echo field. */
class EchoField : public PanelPart
{
public:
    explicit EchoField (YankiProcessor&);
    void paintPanel (juce::Graphics&) override;
    bool hitTest (int x, int y) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void refresh() override { repaint(); }

    /** The model's view of the current parameters (also used by the snapshot tool and the tests). */
    ringmodel::Params modelParams() const;

    static const kit::Box kBox;

private:
    enum Handle { none, handleTime, handleFeedback, handleSpread };
    struct Geo { float ox, oy, R; };
    Geo geo() const;
    juce::Point<float> handlePos (Handle) const;
    Handle handleAt (juce::Point<float> facePos) const;
    juce::Point<float> toFace (const juce::MouseEvent&) const;
    void setSyncNearest (double ms);

    YankiProcessor& proc;
    Handle active = none;
    float startU = 0.0f, startX = 0.0f, startY = 0.0f, dragView = 4.4f;
};

class FrontPanel : public juce::Component
{
public:
    FrontPanel();
    void paint (juce::Graphics&) override;

private:
    juce::Image cache;
    float cacheScale = 0.0f;
    juce::Image mark, wordmark;
};

/** Holds the embedded typefaces for as long as a panel is open and makes them the menus' and buttons' sans-serif. */
class YankiLookAndFeel : public juce::LookAndFeel_V4
{
public:
    YankiLookAndFeel() { setDefaultSansSerifTypeface (faces->regular); }
private:
    juce::SharedResourcePointer<Theme::Faces> faces;
};

class YankiEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit YankiEditor (YankiProcessor&);
    ~YankiEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;   // Cmd+Z / Cmd+Shift+Z
    void mouseDown (const juce::MouseEvent&) override;  // takes the keyboard focus (knobs too, via addMouseListener)

    /** For the headless snapshot: pull the processor state once. */
    void snapshotRefresh();

    static constexpr int kW = 1600, kH = 900;

private:
    void timerCallback() override;
    void setParam (const char* id, float value, bool gesture = true);

    YankiLookAndFeel lnf;   // first member: built before, destroyed after, every child component
    YankiProcessor& proc;
    juce::Component stage;
    FrontPanel front;
    std::vector<std::unique_ptr<PanelPart>> parts;
    PresetBar* presetBar = nullptr;

    double lastTick = 0.0, freezePressedAt = 0.0;
    bool freezeWasOn = false;

#if SVL_LICENSING
    juce::TextButton licenseButton { "LICENSE" };
    svl::LicenseOverlay licenseOverlay;
#endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (YankiEditor)
};
