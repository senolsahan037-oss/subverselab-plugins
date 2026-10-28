// TORNA panel (P2): the Kenwood GE-1100 front after its owner's manual drawing and photos of real units, in the family's
// colours and parts (docs/ui/mock/make_mock_v8.py is the reference picture; docs/ui/PHYSICAL_NOTES.md the notes).
// Design units 1600 x 602; SIZE 75 / 100 %. Everything is drawn; no bitmaps except the SubverseLab logo.
#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "FamilyDraw.h"

namespace torna::ui
{
constexpr float kW = 1600.0f, kH = 602.0f;
// The mock's coordinates are the manual drawing's, scaled 1.1 from (295, 272): kept so every number traces back to it.
inline float X (float dx) { return (dx - 295.0f) * 1.1f; }
inline float Y (float dy) { return (dy - 272.0f) * 1.1f; }
inline juce::Rectangle<float> Rf (float x0, float y0, float x1, float y1) { return { X (x0), Y (y0), X (x1) - X (x0), Y (y1) - Y (y0) }; }
inline juce::Rectangle<int> Ri (float x0, float y0, float x1, float y1) { return Rf (x0, y0, x1, y1).getSmallestIntegerContainer(); }
constexpr float kSlotTop = 347.0f, kSlotBot = 627.0f;   // drawing units
inline float markY (float db) { return kSlotTop + 9.0f + (12.0f - db) / 24.0f * (kSlotBot - kSlotTop - 18.0f); }
}

class TornaEditor;

/** One of the 24 slide pots: an invisible JUCE slider whose cap is drawn here (moulded black cap, red LED). The drawn cap
    glides to values the product writes (memory, COPY, FLAT, LINK) and follows a held cap at once (UI signature 1). */
class CapSlider : public juce::Slider
{
public:
    CapSlider (TornaEditor& e, int channel, int band);
    void paint (juce::Graphics&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void tick();                         // 30 Hz: glide the drawn cap
    float shown = 0.0f;
    const int ch, band;
private:
    TornaEditor& ed;
};

/** A flat ribbed key of the original's strip (momentary; the panel decides what a press means). */
class StripKey : public juce::Button
{
public:
    explicit StripKey (const juce::String& name) : juce::Button (name) {}
    void paintButton (juce::Graphics&, bool over, bool down) override;
    std::function<void (bool longHold)> onRelease;
    void mouseDown (const juce::MouseEvent& e) override { pressedAt = juce::Time::getMillisecondCounterHiRes(); juce::Button::mouseDown (e); }
    void mouseUp (const juce::MouseEvent& e) override
    {
        const bool hold = juce::Time::getMillisecondCounterHiRes() - pressedAt >= 1000.0;
        juce::Button::mouseUp (e);
        if (onRelease && getLocalBounds().contains (e.getPosition())) onRelease (hold);
    }
private:
    double pressedAt = 0.0;
};

/** A horizontal slide with a ribbed steel cap (INPUT, OUTPUT) or a small black cap (the analyser's LEVEL). */
class SlideH : public juce::Slider
{
public:
    explicit SlideH (bool steel) : steelCap (steel) { setSliderStyle (juce::Slider::LinearHorizontal); setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0); setSliderSnapsToMousePosition (false); }
    void paint (juce::Graphics&) override;
    const bool steelCap;
};

/** A rear-plate slide switch (like the original's voltage selector): three positions, a click picks the nearest. */
class RearSwitch : public juce::Component
{
public:
    RearSwitch (juce::StringArray legends) : labels (std::move (legends)) {}
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent& e) override
    {
        const int i = juce::jlimit (0, labels.size() - 1, (int) ((float) e.x / (float) getWidth() * (float) labels.size()));
        if (onPick) onPick (i);
    }
    std::function<void (int)> onPick;
    std::function<int()> current;
    juce::StringArray labels;
};

/** The FL spectrum analyser behind its navy glass: twelve band columns and TOTAL, 20 dash segments each (pairs follow the
    ten comparators), the dot scales, the L / DISPLAY / R / TOTAL pills. */
class FLDisplay : public juce::Component
{
public:
    explicit FLDisplay (TornaAudioProcessor& p) : proc (p) { setInterceptsMouseClicks (false, false); }
    void paint (juce::Graphics&) override;
private:
    TornaAudioProcessor& proc;
};

/** EQUALIZER GRAPHIC: the true response of both channels from the circuit model, and the moved slider's read-out. */
class CurveDisplay : public juce::Component
{
public:
    explicit CurveDisplay (TornaAudioProcessor& p);
    void paint (juce::Graphics&) override;
    void refreshIfChanged();             // 30 Hz: recompute when a slider, REV, EQ IN or LINK changed
    void showReadout (const juce::String& s) { readout = s; readoutAt = juce::Time::getMillisecondCounterHiRes(); repaint(); }
    const std::vector<double>& curve (int ch) const { return db[(size_t) ch]; }
    const std::vector<double>& freqs() const { return f; }
private:
    TornaAudioProcessor& proc;
    std::vector<double> f;
    std::array<std::vector<double>, 2> db;
    juce::String signature, readout;
    double readoutAt = -1.0e9;
    double flatRef = 0.0;
};

class TornaEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit TornaEditor (TornaAudioProcessor&);
    ~TornaEditor() override;
    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    void mouseDown (const juce::MouseEvent&) override;

    TornaAudioProcessor& proc;
    CurveDisplay curve { proc };
    void sliderMoved (int ch, int band, double v);   // a held slider moved: LINK partner, read-out, memory lamp
    bool powered() const;
    bool helpOpen = false;

    // test hooks (plugin_check / ui_snapshot)
    CapSlider& slider (int ch, int band) { return *sliders[(size_t) (ch * 12 + band)]; }
    StripKey* key (const juce::String& name);
    void setScalePct (int pct);
    void timerTickForTests() { timerCallback(); }
    void showRear (bool r);
    bool rearShown() const { return rear; }
    juce::URL lockupUrl() const { return lockup.getURL(); }

private:
    void timerCallback() override;
    void buildBackground();
    void paintLamps (juce::Graphics&);
    juce::RangedAudioParameter* param (const char* id) const { return proc.apvts.getParameter (id) != nullptr ? dynamic_cast<juce::RangedAudioParameter*> (proc.apvts.getParameter (id)) : nullptr; }
    void toggle (const char* id);
    bool on (const char* id) const { return proc.apvts.getRawParameterValue (id)->load() > 0.5f; }

    struct Face : juce::Component   // everything sits on the face, which is scaled by SIZE
    {
        std::function<void (juce::Graphics&)> onPaint, onPaintOver;
        void paint (juce::Graphics& g) override { if (onPaint) onPaint (g); }
        void paintOverChildren (juce::Graphics& g) override { if (onPaintOver) onPaintOver (g); }
    } face;
    struct ThumbLnF : juce::LookAndFeel_V4 { int r = 10; int getSliderThumbRadius (juce::Slider&) override { return r; } };
    ThumbLnF capLnF, steelLnF, levelLnF;
    juce::String lampSignature;
    juce::Image rearBackground;
    void buildRear();
    bool rear = false;
    RearSwitch lineSwitch { { "1 V", "2 V", "4 V" } }, lightSwitch { { "ON", "DIM", "OFF" } };
    juce::Image background;
    std::array<std::unique_ptr<CapSlider>, 24> sliders;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>> attachments;
    juce::OwnedArray<StripKey> keys;
    SlideH input { true }, output { true }, level { false };
    FLDisplay fl { proc };
    juce::TextButton help { "?" };
    juce::HyperlinkButton lockup { {}, juce::URL ("https://subverselab.com") };
    juce::Image mark, wordmark;
    bool linkGuard = false;
    float scale = 0.75f;
};
