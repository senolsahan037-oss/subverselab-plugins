#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Theme.h"

/** Black bakelite knob with a cream line. Vertical drag; the editor syncs it with its parameter and makes it glide. */
class BakeliteKnob : public juce::Slider
{
public:
    explicit BakeliteKnob (bool bigKnob) : big (bigKnob)
    {
        setSliderStyle (juce::Slider::RotaryVerticalDrag);
        setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        setRotaryParameters (juce::degreesToRadians (-135.0f), juce::degreesToRadians (135.0f), true);
        setMouseDragSensitivity (big ? 360 : 260);
        setVelocityBasedMode (false);
    }
    void paint (juce::Graphics&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override { if (onDoubleClick) onDoubleClick(); }

    std::function<void()> onDoubleClick;
    bool animating = false;
    double animTarget = 0.0;

private:
    bool big;
};

/** The black keys (LISTEN, EQUAL, THROW): a heavy word and a line of small print. */
class BlackKey : public juce::Button
{
public:
    BlackKey (const juce::String& word, const juce::String& small) : juce::Button (word), sub (small) {}
    void paintButton (juce::Graphics&, bool over, bool down) override;
    juce::String sub;
    bool lampOn = false, hasLamp = false, lit = false;   // lit: latched (EQUAL on)
    float textPx = 24.0f;
};

/** A strip of Dymo tape: embossed white letters on black (cream when chosen). */
class DymoButton : public juce::Button
{
public:
    DymoButton (const juce::String& text, float rotationDeg) : juce::Button (text), rot (rotationDeg) {}
    void paintButton (juce::Graphics&, bool over, bool down) override;
    bool chosen = false;
    float rot;
};

/** IN: a red lamp and the word, clickable. */
class InSwitch : public juce::Button
{
public:
    InSwitch() : juce::Button ("IN") {}
    void paintButton (juce::Graphics&, bool over, bool down) override;
    bool on = true;
};

class BoebEditor : public juce::AudioProcessorEditor,
                   private juce::Timer
{
public:
    static constexpr int kW = 1280, kH = 680;

    explicit BoebEditor (BoebProcessor&);
    ~BoebEditor() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;

    /** One step of everything the timer does (glides, typewriter, scan); ui_snapshot calls it too. */
    void advance (float dt);
    void setHelpOpen (bool open) { helpOpen = open; repaint(); }
    /** ui_snapshot: knobs jump to their values, the typewriter has finished. */
    void settleForSnapshot();
    /** ui_snapshot --check: the drawn value of a knob, and a double-click on it. */
    double drawnValue (const juce::String& id) const;
    void doubleClick (const juce::String& id);

private:
    void timerCallback() override;

    struct Bound
    {
        BakeliteKnob* knob;
        juce::RangedAudioParameter* param;
        std::function<double()> doubleClickValue;
        bool held = false;
    };
    void bind (BakeliteKnob&, const char* id, std::function<double()> dc);
    void syncControls (bool glide);
    void paintSheet (juce::Graphics&);
    void paintHelp (juce::Graphics&);

    BoebProcessor& proc;

    BakeliteKnob lift { true }, body { false }, air { false }, space { false }, output { false }, dryWet { false };
    std::vector<Bound> bound;
    BlackKey listenKey { "LISTEN", "6 S OF THE SONG" }, equalKey { "EQUAL", "A/B SAME LOUDNESS" }, throwKey { "THROW", "ECHO THE LAST WORD" };
    std::array<std::unique_ptr<DymoButton>, 6> moods;
    DymoButton help { "?", 0.0f };
    InSwitch inSwitch;
    bool throwHeld = false, helpOpen = false, overLockup = false;

    // Typewriter on the sheet: characters shown per row, FOUND and NOW.
    BoebProcessor::Readout shown;
    int typedFoundSerial = -1, typedNowSerial = -1;
    float typeFound = 0.0f, typeNow = 0.0f;
    float scanPhase = 0.0f, flash = 0.0f, blink = 0.0f;
    int lastState = 0;

    juce::Image grain, portrait, mark;
    juce::Path sheetEdge;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BoebEditor)
};
