#pragma once

#include <JuceHeader.h>
#include <array>
#include "Session.h"
#include "LiveAnalysis.h"

namespace mihenk { class MihenkProcessor; }

/*  The parts of the MH-86 front panel. Everything is drawn in code except the SubverseLab mark; every part shows or
    changes something real. Live parts read LiveAnalysis (the real-time metering path); when the audio stops they
    show the whole-track analysis from the Session. */
namespace mihenk::ui
{
juce::Point<float> polar (juce::Point<float> c, float r, float angle);
/** Tracked legend text; shrinks the font (down to 75 %) so it never runs out of its box. */
void tracked (juce::Graphics& g, const juce::String& s, juce::Font f, juce::Rectangle<float> r, juce::Justification j, float tracking);
void glow (juce::Graphics& g, juce::Point<float> c, float radius, juce::Colour col, float alpha);
/** VFD text with its halo; shrinks to fit its box. */
void vfdText (juce::Graphics& g, const juce::String& s, juce::Rectangle<float> r, juce::Justification j, float size, juce::Colour c);
void vfdGlass (juce::Graphics& g, juce::Rectangle<float> b);
void vfdGlassTop (juce::Graphics& g, juce::Rectangle<float> b);
/** A small CRT screen (bezel, phosphor glass); returns the plot area inside. */
juce::Rectangle<float> smallScreen (juce::Graphics& g, juce::Rectangle<float> b);
void screenGlass (juce::Graphics& g, juce::Rectangle<float> b);
void phosphorPath (juce::Graphics& g, const juce::Path& p, juce::Colour c, float width = 1.3f);
juce::String timeText (double secs);

/** The product name in squared, extended mid-80s letterforms, drawn as strokes. Returns the width. */
float drawProductName (juce::Graphics& g, const juce::String& name, float x, float y, float h, juce::Colour ink);
float productNameWidth (const juce::String& name, float h);

/** What the live parts share. */
struct Context
{
    Session& session;
    LiveAnalysis& live;
    MihenkProcessor& processor;
    int selectedBand = 0;                                   // 3-BAND: the band the knobs show
    std::array<bool, 4> layers { true, true, true, true };  // IN, EQ, COMP, OUT
    bool isApp() const;
    bool liveNow() const { return live.running(); }
};

/** Machined knob shared with KASET and KUBBE, scaled for a denser panel. */
class MihenkLookAndFeel : public juce::LookAndFeel_V4
{
public:
    MihenkLookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos, float startAngle, float endAngle, juce::Slider&) override;
    juce::Font getSliderPopupFont (juce::Slider&) override;
    void fillTextEditorBackground (juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int width, int height, juce::TextEditor&) override;
    /** Tooltips on a small VFD: mono type, wrapped at 330 px. */
    juce::Rectangle<int> getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos, juce::Rectangle<int> parentArea) override;
    void drawTooltip (juce::Graphics&, const juce::String& text, int width, int height) override;
};

/** Rotary knob drag: vertical, 700 px for the whole range; with Cmd held, ten times finer. Double-click resets. */
class KnobSlider : public juce::Slider
{
public:
    static constexpr double pixelsForFullRange = 700.0, fineFactor = 10.0;
    /** > 0: this many pixels for the whole range (and no Cmd-fine drag); a stepped knob needs a short throw so a small drag crosses a step. */
    double pixelsOverride = 0.0;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    double dragProportion = 0.0;
    float lastY = 0.0f;
    bool dragging = false;
};

/** A long aluminium key with an LED above it (transport and actions). */
class KeyButton : public juce::Button
{
public:
    explicit KeyButton (const juce::String& text, bool latching = false) : juce::Button (text) { setClickingTogglesState (latching); }
    void paintButton (juce::Graphics&, bool over, bool down) override;
    juce::Colour ledColour = juce::Colour (0xffF6D58E);
    bool blink = false;               // the LED blinks (LEARN listening)
};

/** A small rectangular cap with an LED window, for selectors and module switches. */
class CapButton : public juce::Button
{
public:
    explicit CapButton (const juce::String& text, bool latching = true) : juce::Button (text) { setClickingTogglesState (latching); }
    void paintButton (juce::Graphics&, bool over, bool down) override;
    bool showLed = true;
    juce::Colour ledColour { 0xffF5A623 };
};

/** The preset name on a small VFD: "HIP-HOP / RAP", "MY MASTER *" (the star: changed since loaded or saved). */
class PresetDisplay : public juce::Component
{
public:
    void set (const juce::String& name, bool modified, bool factory);
    void paint (juce::Graphics&) override;

private:
    juce::String name { "DEFAULT" };
    bool modified = false, factory = false;
};

/** Compressor gain reduction over the loud part of the track (WIDE, or the selected band in 3-BAND). */
class GrReadout : public juce::Component
{
public:
    explicit GrReadout (Context& c) : ctx (c) {}
    void paint (juce::Graphics&) override;
    static constexpr int glassHeight = 44;

private:
    Context& ctx;
};

/** The phosphor CRT: waveform, spectrum or loudness; live and layered while audio moves, whole-track when stopped. */
class CrtScope : public juce::Component
{
public:
    enum class Mode { wave, spectrum, loudness };
    explicit CrtScope (Context& c) : ctx (c) {}
    void setMode (Mode m) { mode = m; waveKey.clear(); repaint(); }
    Mode getMode() const { return mode; }
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    juce::Rectangle<float> plotArea() const;
    /** Snapshot tool: draw as if the audio were moving (the live analysis must hold data). */
    bool forceLive = false;

private:
    bool isLive() const { return forceLive || ctx.liveNow(); }
    void paintWave (juce::Graphics&, juce::Rectangle<float> plot);
    void paintSpectrum (juce::Graphics&, juce::Rectangle<float> plot);
    void paintLoudness (juce::Graphics&, juce::Rectangle<float> plot);
    void paintWaveLive (juce::Graphics&, juce::Rectangle<float> plot);
    void paintSpectrumLive (juce::Graphics&, juce::Rectangle<float> plot);
    void paintLoudnessLive (juce::Graphics&, juce::Rectangle<float> plot);
    void paintOverview (juce::Graphics&, juce::Rectangle<float> strip);
    void buildWave (int width);
    juce::int64 sampleAt (float x, juce::Rectangle<float> area) const;
    juce::Rectangle<float> overviewArea() const;

    Context& ctx;
    Mode mode = Mode::wave;
    juce::String waveKey;
    std::vector<float> inMin, inMax, outMin, outMax;
    float dragStartX = -1.0f;
    bool dragging = false, dragInOverview = false;
};

/** The measurement table on the VFD: INPUT, OUTPUT and REFERENCE columns (whole track). */
class VfdTable : public juce::Component
{
public:
    explicit VfdTable (Context& c) : ctx (c) {}
    void paint (juce::Graphics&) override;

private:
    Context& ctx;
};

/** The OUTPUT meter column, full panel height: IN, L, R sample peak with 3 s hold and max hold (click to reset),
    M (momentary LUFS with short-term, integrated and target marks), C (compressor GR) and LIM (limiter GR), on
    one dBFS / dBTP scale; the file's highest true and sample peak marked and written out with their time, for the
    input and the output, as soon as a track is loaded. */
class OutputMeters : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit OutputMeters (Context& c) : ctx (c) { setTooltip ("Click to reset the max holds and the integrated loudness"); }
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    static float levelToY (float db, juce::Rectangle<float> bar);

private:
    Context& ctx;
};

/** Small status lamp with a legend under it. */
class Lamp : public juce::Component
{
public:
    Lamp (const juce::String& text, juce::Colour c) : legend (text), colour (c) {}
    void setOn (bool on) { if (on != lit) { lit = on; repaint(); } }
    void paint (juce::Graphics&) override;

private:
    juce::String legend;
    juce::Colour colour;
    bool lit = false;
};

/** The file / status VFD in the header. */
class InfoDisplay : public juce::Component
{
public:
    explicit InfoDisplay (Context& c) : ctx (c) {}
    void paint (juce::Graphics&) override;

private:
    Context& ctx;
};

/** Plug-in header VFD: what LEARN is doing and what it knows. */
class LearnDisplay : public juce::Component
{
public:
    explicit LearnDisplay (Context& c) : ctx (c) {}
    void paint (juce::Graphics&) override;

private:
    Context& ctx;
};

/** A one- or two-line VFD readout (latency). */
class SmallVfd : public juce::Component
{
public:
    void set (const juce::String& label, const juce::String& value) { if (label != l || value != v) { l = label; v = value; repaint(); } }
    void paint (juce::Graphics&) override;

private:
    juce::String l, v;
};

/** DIAGNOSIS: starting points, the measured findings (severity lamp, title, one-line measured reason, APPLY), the
    applied scenarios as removable chips, and SAVE AS MY SCENARIO. */
class DiagnosisPanel : public juce::Component, public juce::TooltipClient
{
public:
    explicit DiagnosisPanel (Context& c);
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    juce::String getTooltip() override;
    void refresh();                                         // the findings or chips changed

    std::function<void (Genre)> onStartingPoint;
    std::function<void (int)> onApply, onRemove;
    std::function<void()> onSaveScenario;
    CapButton startRap { "HIP-HOP / RAP", false }, startPop { "R&B / POP", false }, startElectronic { "ELECTRONIC", false };
    CapButton saveScenario { "SAVE AS MY SCENARIO", false };

private:
    struct Hit { juce::Rectangle<float> area; int scenario = 0; int kind = 0; juce::String tip; };   // kind 0 row, 1 apply, 2 chip remove
    juce::Rectangle<float> listArea() const;
    juce::Rectangle<float> chipArea() const;
    Context& ctx;
    std::vector<Hit> hits;
    int scroll = 0;
    juce::Point<float> mouse { -1.0f, -1.0f };
    Diagnosis shown;
};

/** 3-BAND: the three bands on a frequency axis (Linkwitz-Riley 4th order, zero phase) with the crossovers, the
    selected band lit and each band's gain reduction (live, or its loud-part average when stopped). */
class CrossoverDisplay : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit CrossoverDisplay (Context& c) : ctx (c) {}
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    std::function<void (int)> onSelectBand;

private:
    Context& ctx;
};

/** WIDE: the compressor's gain reduction over time (live scrolling, or the whole track when stopped). */
class GrTrace : public juce::Component
{
public:
    explicit GrTrace (Context& c) : ctx (c) {}
    void paint (juce::Graphics&) override;

private:
    Context& ctx;
};

/** Stereo correlation of the output: whole band and below 150 Hz (live, or whole-track when stopped). */
class CorrelationMeter : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit CorrelationMeter (Context& c) : ctx (c) { setTooltip ("Correlation of the output, whole band and below 150 Hz: +1 mono, 0 unrelated, below 0 cancels in mono"); }
    void paint (juce::Graphics&) override;

private:
    Context& ctx;
};

/** Limiter: reduction, activity, true peak out. */
class LimiterReadout : public juce::Component
{
public:
    explicit LimiterReadout (Context& c) : ctx (c) {}
    void paint (juce::Graphics&) override;

private:
    Context& ctx;
};
} // namespace mihenk::ui
