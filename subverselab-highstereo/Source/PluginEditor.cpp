#include "PluginEditor.h"

namespace
{
    juce::String pct (float v) { return juce::String (juce::roundToInt (v)) + "%"; }
    juce::String minusSign() { return juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92")); }
    juce::String dbText (float v, int dec = 1)
    {
        const auto a = juce::String (std::abs (v), dec);
        return (v > 0.04f ? "+" : v < -0.04f ? minusSign() : juce::String ("+")) + a;
    }
    const juce::Rectangle<int> kStageBlock { 14, 58, 300, 330 }, kFieldBlock { 322, 58, 624, 330 }, kStrip { 14, 396, 932, 110 };
    const juce::Rectangle<int> kSummary { 28, 485, 850, 18 };
}

HighStereoAudioProcessorEditor::HighStereoAudioProcessorEditor (HighStereoAudioProcessor& p)
    : AudioProcessorEditor (&p), proc (p), presetBar (p), listenBar (p), stage (p),
      field (p, [this] (int k) { return loadedValue (HighStereoAudioProcessor::bandId (k)); }), phase (p), guard (p)
{
    auto prm = [this] (const char* id) -> juce::RangedAudioParameter& { return *proc.apvts.getParameter (id); };
    auto loaded = [this] (const char* id) { return [this, id] { return loadedValue (id); }; };

    width = std::make_unique<hsui::RoundControl> (prm ("width"), "WIDTH", loaded ("width"), pct,
        [] (float v) { return "WIDTH " + juce::String (juce::roundToInt (v)) + " % - widens or narrows the sides of the whole mix; past 150 % they move behind you. Mono stays the same."; },
        true, 0.75f);
    spread = std::make_unique<hsui::RoundControl> (prm ("spread"), "SPREAD", loaded ("spread"), pct,
        [] (float v) { return "SPREAD " + juce::String (juce::roundToInt (v)) + " % - fills the gap between the centre and the sides, from the edges. The centre stays put."; });
    monoBass = std::make_unique<hsui::RoundControl> (prm ("monobass"), "MONO BASS", loaded ("monobass"),
        [] (float v) { return v < 40.0f ? juce::String ("OFF") : juce::String (juce::roundToInt (v)); },
        [] (float v) { return v < 40.0f ? juce::String ("MONO BASS OFF - turn it up to keep the low end in mono.")
                                        : "MONO BASS " + juce::String (juce::roundToInt (v)) + " Hz - below this the sides are removed: the low end stays in mono."; });
    output = std::make_unique<hsui::RoundControl> (prm ("output"), "OUTPUT", loaded ("output"),
        [] (float v) { return dbText (v); },
        [this] (float v) { return "OUTPUT " + dbText (v) + " dB - output level."
                                  + (proc.apvts.getRawParameterValue ("match")->load() > 0.5f ? juce::String (" MATCH is on: it keeps the level, so this changes nothing now.")
                                                                                             : juce::String()); }, true);
    mix = std::make_unique<hsui::RoundControl> (prm ("mix"), "MIX", loaded ("mix"), pct,
        [] (float v) { return "MIX " + juce::String (juce::roundToInt (v)) + " % - blend of the untouched and the processed signal. It never combs."; });

    auto bandOver = [this]
    {
        float top = -200.0f; bool over = false;
        for (int k = 0; k < HighStereoAudioProcessor::kBands; ++k) top = std::max (top, proc.bandView (k).levelDb.load());
        for (int k = 0; k < HighStereoAudioProcessor::kBands; ++k)
            over = over || (proc.bandView (k).overflow.load() && proc.bandView (k).levelDb.load() > top - 30.0f);
        return over;
    };
    inSw = std::make_unique<hsui::SlideSwitch> (prm ("in"), "IN",
        [] (bool on) { return juce::String (on ? "IN - on: HIGH STEREO is working. Click for the untouched input."
                                               : "IN - off: you hear the untouched input. Click to switch HIGH STEREO on."); });
    matchSw = std::make_unique<hsui::SlideSwitch> (prm ("match"), "MATCH",
        [this] (bool on) { return "MATCH - holds the output at the input's loudness, so IN on/off is a fair compare"
                                  + (on ? " (now " + dbText (proc.getMatchDb()) + " dB)." : juce::String (".")); },
        [this] { return proc.apvts.getRawParameterValue ("match")->load() > 0.5f ? dbText (proc.getMatchDb()) + " dB" : juce::String(); });
    fixSw = std::make_unique<hsui::SlideSwitch> (prm ("fix"), "FIX",
        [bandOver] (bool on) { return juce::String ("FIX - while on, no band may pass the point where it vanishes in mono.")
                                      + (! on && bandOver() ? " A band is over now." : ""); },
        nullptr, bandOver);

    for (auto* c : std::initializer_list<juce::Component*> { &presetBar, &listenBar, &stage, &field, width.get(), spread.get(), monoBass.get(),
                                                             output.get(), mix.get(), inSw.get(), matchSw.get(), fixSw.get(), &phase, &guard, &mark })
        addAndMakeVisible (c);

    wordmark = juce::ImageCache::getFromMemory (BinaryData::wordmark_png, BinaryData::wordmark_pngSize);
    // the SubverseLab icon in ink, drawn from the cream mark's alpha
    const auto cm = juce::ImageCache::getFromMemory (BinaryData::mark_cream_png, BinaryData::mark_cream_pngSize);
    inkMark = juce::Image (juce::Image::ARGB, cm.getWidth(), cm.getHeight(), true);
    {
        juce::Image::BitmapData src (cm, juce::Image::BitmapData::readOnly), dst (inkMark, juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < cm.getHeight(); ++y)
            for (int x = 0; x < cm.getWidth(); ++x)
                dst.setPixelColour (x, y, hsui::ink.withAlpha (src.getPixelColour (x, y).getFloatAlpha()));
    }

    setWantsKeyboardFocus (true);
    setSize (kW, kH);
    startTimerHz (30);
}

HighStereoAudioProcessorEditor::~HighStereoAudioProcessorEditor() { stopTimer(); }

float HighStereoAudioProcessorEditor::loadedValue (const juce::String& id) const
{
    auto* p = proc.apvts.getParameter (id);
    const float def = p != nullptr ? p->convertFrom0to1 (p->getDefaultValue()) : 0.0f;
    return proc.presets.valueAtLoad (id, def);
}

juce::Component* HighStereoAudioProcessorEditor::controlNamed (const juce::String& id)
{
    if (id == "width") return width.get();   if (id == "spread") return spread.get();   if (id == "monobass") return monoBass.get();
    if (id == "output") return output.get(); if (id == "mix") return mix.get();         if (id == "in") return inSw.get();
    if (id == "match") return matchSw.get(); if (id == "fix") return fixSw.get();       if (id == "stage") return &stage;
    if (id == "field") return &field;        if (id == "listen") return &listenBar;     if (id == "preset") return &presetBar;
    if (id == "phase") return &phase;        if (id == "mark") return &mark;           if (id == "guard") return &guard;
    return nullptr;
}

bool HighStereoAudioProcessorEditor::keyPressed (const juce::KeyPress& key)
{
    return svl::handleUndoKey (proc.undo, key);
}

void HighStereoAudioProcessorEditor::timerCallback()
{
    stage.tick();
    field.tick();
    for (auto* r : { width.get(), spread.get(), monoBass.get(), output.get(), mix.get() }) r->tick();
    for (auto* sw : { inSw.get(), matchSw.get(), fixSw.get() }) sw->tick();
    phase.repaint();
    guard.tick();

    // hover summary: the control under the pointer (or the one a snapshot forces) says what it does
    juce::String s;
    juce::Component* c = forcedHover;
    if (c == nullptr && isMouseOver (true)) c = getComponentAt (getMouseXYRelative());
    for (; c != nullptr && c != this; c = c->getParentComponent())
    {
        if (c == &mark) { s = "Manual - opens the HIGH STEREO manual in your browser."; break; }
        if (auto* h = dynamic_cast<hsui::HasSummary*> (c)) { s = h->summary(); break; }
    }
    if (s != summaryText) { summaryText = s; repaint (kSummary.expanded (4)); }
}

void HighStereoAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (hsui::grey);
    g.setColour (hsui::greyHi);
    g.fillRoundedRectangle (kStageBlock.toFloat(), 10.0f);
    g.fillRoundedRectangle (kFieldBlock.toFloat(), 10.0f);
    g.fillRoundedRectangle (kStrip.toFloat(), 10.0f);

    if (wordmark.isValid())
    {
        const float w = 84.0f, h = w * (float) wordmark.getHeight() / (float) wordmark.getWidth();
        g.drawImage (wordmark, juce::Rectangle<float> (14.0f, 9.0f, w, h), juce::RectanglePlacement::stretchToFit);
    }

    g.setColour (hsui::ink);
    g.setFont (hsui::regular (11.5f));
    g.drawText (summaryText, kSummary, juce::Justification::centredLeft, true);

    if (inkMark.isValid())
        g.drawImage (inkMark, mark.getBounds().toFloat(), juce::RectanglePlacement::centred);
}

void HighStereoAudioProcessorEditor::resized()
{
    presetBar.setBounds (120, 16, 244, 26);
    listenBar.setBounds (kW - 14 - 505, 10, 505, 38);
    stage.setBounds (kStageBlock);
    field.setBounds (kFieldBlock);

    const int cy = kStrip.getY() + 44;
    int cx = 70;
    for (auto* r : { width.get(), spread.get(), monoBass.get(), output.get(), mix.get() }) { r->setBounds (cx - 50, cy - 50, 100, 100); cx += 100; }
    // switches on the knobs' own 100 px step after MIX (owner 20 Nov), the phase bar centred under them
    int sx = 570;
    for (auto* s : { inSw.get(), matchSw.get(), fixSw.get() })
    {
        const int w = s->unitWidth() + 6;
        s->setBounds (sx - w / 2, cy - 24, w, 40);
        sx += 100;
    }
    phase.setBounds (670 - 130, cy + 18, 260, 16);
    guard.setBounds (836, cy - 24, 98, 48);   // after FIX, in the strip's free right end (owner 21 Nov)

    const auto mr = svl::markRect (inkMark, (float) (kStrip.getRight() - 12), (float) (kSummary.getCentreY() - 2), 26.0f);
    mark.setBounds (mr.toNearestInt());
}
