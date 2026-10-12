#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Theme.h"
#if SVL_LICENSING
#include "../../subverselab-plugin-common/Licensing/LicenseOverlay.h"
#endif

/*  The face of an early-80s double-cassette deck of the kind that sat in
    every Turkish living room, as it left the factory: brand new, in
    SubverseLab colours. Two references set the layout: a stacked compo with
    A/B dubbing decks and a centre strip, and a "Double" boombox with its silver
    top strip. From the top: carrying handle; a strip with piano keys, a
    tuner-style dial whose pointer picks the dub generation, and the tape
    selector; a row of the machined knobs shared with Kubbe; deck A, the centre
    strip with counter and level, deck B; the name plate. Every control works;
    nothing is decoration. Everything is drawn in code except the SubverseLab mark.
*/

class KasetLookAndFeel : public juce::LookAndFeel_V4
{
public:
    KasetLookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider&) override;

private:
    juce::SharedResourcePointer<Theme::Faces> faces;   // the embedded panel typefaces
};

/** Tuner-style dial: the red pointer sits on the dub generation. Click or drag to move it. */
class GenerationDial : public juce::Component
{
public:
    explicit GenerationDial (KasetProcessor& p) : proc (p) {}
    void paint (juce::Graphics&) override;
    // One click or one drag = one gesture = one undo step.
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (auto* p = proc.apvts.getParameter ("generation")) p->beginChangeGesture();
        pick (e.position.x);
    }
    void mouseDrag (const juce::MouseEvent& e) override { pick (e.position.x); }
    // Double-click = back to the default (2ND DUB), inside the gesture the second mouseDown opened (so it is one undo step with the click).
    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        if (auto* p = proc.apvts.getParameter ("generation"))
        {
            p->setValueNotifyingHost (p->getDefaultValue());
            repaint();
        }
    }
    void mouseUp (const juce::MouseEvent&) override
    {
        if (auto* p = proc.apvts.getParameter ("generation")) p->endChangeGesture();
    }

private:
    void pick (float x);
    float xOf (int index) const;
    KasetProcessor& proc;
};

/** One cassette door; deck A records, deck B plays the dub. */
class CassetteWell : public juce::Component
{
public:
    CassetteWell (KasetProcessor& p, bool dubSide) : proc (p), dub (dubSide) {}
    void paint (juce::Graphics&) override;
    void advance (float seconds);

private:
    KasetProcessor& proc;
    bool dub;
    float angle = 0.0f;
};

/** LED level ladder, two channels. */
class LevelLadder : public juce::Component
{
public:
    void setLevels (float l, float r);
    void paint (juce::Graphics&) override;

private:
    float db[2] { -60.0f, -60.0f };
};

/** Three-digit mechanical counter; click RESET to zero it. */
class TapeCounter : public juce::Component
{
public:
    explicit TapeCounter (KasetProcessor& p) : proc (p) {}
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    KasetProcessor& proc;
    double offset = 0.0;
};

/** A long aluminium piano key with an LED above it. */
class PianoKey : public juce::Button
{
public:
    explicit PianoKey (juce::String text) : juce::Button (text) {}
    void paintButton (juce::Graphics&, bool over, bool down) override;
};

/** A small square selector button, like a compo's band selector. */
class SelectorButton : public juce::Button
{
public:
    explicit SelectorButton (juce::String text) : juce::Button (text) {}
    void paintButton (juce::Graphics&, bool over, bool down) override;
};

class KasetEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit KasetEditor (KasetProcessor&);
    ~KasetEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;   // Cmd+Z / Cmd+Shift+Z
    void mouseDown (const juce::MouseEvent&) override;  // takes the keyboard focus (knobs too, via addMouseListener)

    /** For the snapshot tool: run the UI clock so meters and reels settle. */
    void settleForSnapshot() { for (int i = 0; i < 30; ++i) timerCallback(); }

private:
    void timerCallback() override;
    void buildPanelTexture();
    void paintScale (juce::Graphics&, const juce::Slider&) const;

    struct Knob
    {
        juce::String id, name;
        juce::Slider slider;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    KasetProcessor& proc;
    KasetLookAndFeel lnf;
    GenerationDial dial;
    CassetteWell deckA, deckB;
    LevelLadder ladder;
    TapeCounter counter;
    std::vector<std::unique_ptr<SelectorButton>> tapeButtons;
    PianoKey playKey { "TAPE" }, stopKey { "BYPASS" }, autoKey { "AUTO GAIN" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> autoAttachment;
    SelectorButton trackKey { "TRACK" };   // low-latency head; the window beside it reads the latency in samples
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> trackAttachment;
    int lastLatency = -1;
    std::vector<std::unique_ptr<Knob>> knobs;

    juce::Image panelTexture, mark, wordmark;
    double lastTick = 0.0;
    int lastTape = -1;
    int lastPlay = -1;   // -1: not synced yet, so the first tick always applies the state

#if SVL_LICENSING
    juce::TextButton licenseButton { "LICENSE" };
    svl::LicenseOverlay licenseOverlay;
#endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KasetEditor)
};
