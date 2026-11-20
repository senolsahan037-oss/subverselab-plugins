#include "PluginProcessor.h"
void logMessage (const juce::String& message);      // PluginProcessor.cpp: ELEK.log
#include "PluginEditor.h"
#include <iostream>

#include "Theme.h"

// The plug-in targets define the version from CMake (2.0.0); the snapshot harness compiles this file without the plug-in defines.
#ifndef JucePlugin_VersionString
 #define JucePlugin_VersionString "2.0.0"
#endif

using Editor = SubverseSplitterAudioProcessorEditor;

namespace
{
    juce::String formatTime (double seconds)
    {
        const int s = juce::jmax (0, (int) std::floor (seconds + 1.0e-6));
        return juce::String (s / 60) + ":" + juce::String (s % 60).paddedLeft ('0', 2);
    }

    const juce::String dot (juce::CharPointer_UTF8 (" \xc2\xb7 "));
    const juce::String emDash (juce::CharPointer_UTF8 ("\xe2\x80\x94"));

    // The four options, in the inspector's order
    // The four operations: each is a tile on the home screen, read as a tool of its own (owner 18 Nov). Tempo and key are
    // not an operation any more: they are measured as soon as a file is loaded and shown with it.
    struct Option { const char* label; const char* note; };
    constexpr int kNumOptions = 4;
    const Option kOptions[kNumOptions] = {
        // owner 20 Nov chose these names (option 1: "<n>-Stem Split" pattern)
        { "Vocal Split",     "The vocal, and the track without it." },
        { "4-Stem Split",    "Vocals, drums, bass and everything else." },
        { "6-Stem Split",    "The four stems, plus guitar and piano." },
        { "Drum Split",      "The drums, split into kick, snare and hats." },      // the drum split is its own operation, never automatic
    };

    Editor::Operation opFor (int option)
    {
        using Op = Editor::Operation;
        return option == 0 ? Op::VocalRemoval : option == 1 ? Op::Stems : option == 2 ? Op::Stems6 : Op::DrumsOnly;
    }

    juce::String opLabel (Editor::Operation op)
    {
        using Op = Editor::Operation;
        return op == Op::Analysis ? "Key & BPM" : op == Op::VocalRemoval ? "Vocal removal" : op == Op::Stems ? "4 stems"
             : op == Op::DrumsOnly || op == Op::DrumParts ? "Drums only" : "6 stems";
    }

    float textWidth (const juce::Font& f, const juce::String& s) { return juce::GlyphArrangement::getStringWidth (f, s); }

    /** Single line, cut with an ellipsis at the end when it is wider than `maxW`. */
    void drawLine (juce::Graphics& g, const juce::String& s, float x, float cy, float maxW, const juce::Font& f, juce::Colour c,
                   juce::Justification just = juce::Justification::centredLeft)
    {
        g.setColour (c);
        g.setFont (f);
        g.drawText (s, juce::Rectangle<float> (x, cy - f.getHeight() * 0.5f - 1.0f, maxW, f.getHeight() + 2.0f), just, true);
    }

    /** Word wrap into lines no wider than maxW; returns the y after the last line. */
    float drawWrapped (juce::Graphics& g, const juce::String& s, float x, float y, float maxW, float lineH, const juce::Font& f, juce::Colour c,
                       juce::Justification just = juce::Justification::centredLeft)
    {
        juce::StringArray words;
        words.addTokens (s, " ", "");
        juce::String cur;
        g.setColour (c);
        g.setFont (f);
        auto flush = [&] { g.drawText (cur, juce::Rectangle<float> (x, y - lineH * 0.5f, maxW, lineH), just, true); y += lineH; };
        for (auto& w : words)
        {
            const auto t = cur.isEmpty() ? w : cur + " " + w;
            if (cur.isNotEmpty() && textWidth (f, t) > maxW) { flush(); cur = w; }
            else cur = t;
        }
        if (cur.isNotEmpty()) flush();
        return y;
    }

    /** Text that does not fit loses its START ("...folder/run"), for paths. */
    juce::String fitFromEnd (const juce::String& s, const juce::Font& f, float maxW)
    {
        if (textWidth (f, s) <= maxW) return s;
        juce::String t = s;
        while (t.length() > 1 && textWidth (f, "..." + t) > maxW) t = t.substring (1);
        return "..." + t;
    }

    void caption (juce::Graphics& g, const juce::String& s, float x, float cy, juce::Colour c = Theme::textMuted, bool bold = false)
    {
        g.setColour (c);
        const auto f = bold ? Theme::bold (10.5f) : Theme::regular (10.5f);
        Theme::drawTracked (g, s.toUpperCase(), { x, cy - 8.0f, 400.0f, 16.0f }, f, 1.2f, juce::Justification::centredLeft);
    }

    /** THE MARK (18 Nov 2026, replaces the E of six bars): a sieve from above, the same numbers as scripts/make_elek_icon.py
        (100-unit box: rim radius 46, stroke 9; hexagonal mesh step 14.5; three dots in stem colours). Drawn at height h. */
    void drawMark (juce::Graphics& g, float x, float y, float h)
    {
        const float u = h / 100.0f;
        g.setColour (Theme::text);
        g.drawEllipse (x + (4.0f + 4.5f) * u, y + (4.0f + 4.5f) * u, (92.0f - 9.0f) * u, (92.0f - 9.0f) * u, 9.0f * u);
        for (int j = -3; j <= 3; ++j)
            for (int i = -3; i <= 3; ++i)
            {
                const float px = 50.0f + (float) i * 14.5f + ((j % 2) != 0 ? 7.25f : 0.0f), py = 50.0f + (float) j * 14.5f * 0.866f;
                if (std::hypot (px - 50.0f, py - 50.0f) > 46.0f - 9.0f - 4.2f - 2.2f) continue;
                const bool vocals = i == -1 && j == 0, drums = i == 0 && j == 1, bass = i == 1 && j == 0;
                const float r = (vocals || drums || bass ? 5.6f : 4.2f) * u;
                g.setColour (vocals ? Theme::stemVocals : drums ? Theme::stemDrums : bass ? Theme::stemBass : Theme::textMuted.withAlpha (0.75f));
                g.fillEllipse (x + px * u - r, y + py * u - r, 2.0f * r, 2.0f * r);
            }
    }

    /** One lane's frame: label panel and wave ground, colour tab, name, the drum group rail. `lane` in the caller's coordinates;
        every x is the mockup's, relative to the lane's left edge (kLaneX). */
    /** STEM GLYPHS (owner 18 Nov: an icon language for the stems instead of words). Drawn in a 16-unit box scaled to `box`,
        1.5-unit strokes, one colour; told apart by shape alone (they must also read in grey). Names are the lane labels. */
    void drawStemGlyph (juce::Graphics& g, const juce::String& stem, juce::Rectangle<float> box, juce::Colour colour)
    {
        const float k = box.getWidth() / 16.0f;
        const auto t = juce::AffineTransform::scale (k).translated (box.getX(), box.getY());
        const juce::PathStrokeType stroke (1.5f * k, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        juce::Path line, fill;
        if (stem == "Vocals")                 // microphone
        {
            line.addRoundedRectangle (5.5f, 1.5f, 5.0f, 8.0f, 2.5f);
            line.startNewSubPath (3.5f, 7.5f); line.cubicTo (3.5f, 13.5f, 12.5f, 13.5f, 12.5f, 7.5f);
            line.startNewSubPath (8.0f, 12.0f); line.lineTo (8.0f, 14.5f);
            line.startNewSubPath (5.5f, 14.5f); line.lineTo (10.5f, 14.5f);
        }
        else if (stem == "Drums")             // a drum with two crossed sticks over it
        {
            line.addEllipse (2.5f, 7.5f, 11.0f, 3.6f);
            line.startNewSubPath (2.5f, 9.3f); line.lineTo (2.5f, 12.2f);
            line.cubicTo (2.5f, 15.0f, 13.5f, 15.0f, 13.5f, 12.2f); line.lineTo (13.5f, 9.3f);
            line.startNewSubPath (2.0f, 1.5f); line.lineTo (9.5f, 7.0f);
            line.startNewSubPath (14.0f, 1.5f); line.lineTo (6.5f, 7.0f);
        }
        else if (stem == "Bass")              // the bass clef
        {
            line.startNewSubPath (3.8f, 6.4f);
            line.cubicTo (3.2f, 3.2f, 5.6f, 1.8f, 7.6f, 1.8f);
            line.cubicTo (10.6f, 1.8f, 11.6f, 4.2f, 11.6f, 6.2f);
            line.cubicTo (11.6f, 10.4f, 7.6f, 13.0f, 3.2f, 14.6f);
            fill.addEllipse (3.2f, 5.0f, 3.0f, 3.0f);
            fill.addEllipse (13.2f, 3.4f, 2.0f, 2.0f);
            fill.addEllipse (13.2f, 7.2f, 2.0f, 2.0f);
        }
        else if (stem == "Guitar")            // a solid figure-of-eight body with its sound hole, neck and headstock
        {
            fill.addEllipse (1.0f, 7.4f, 8.2f, 8.0f);
            fill.addEllipse (5.2f, 4.6f, 5.4f, 5.4f);
            line.startNewSubPath (9.4f, 6.4f); line.lineTo (13.6f, 2.2f);
            line.startNewSubPath (12.4f, 1.0f); line.lineTo (14.8f, 3.4f);
        }
        else if (stem == "Tempo")             // a metronome (Key & BPM)
        {
            line.startNewSubPath (6.3f, 2.0f); line.lineTo (9.7f, 2.0f); line.lineTo (12.2f, 14.2f); line.lineTo (3.8f, 14.2f); line.closeSubPath();
            line.startNewSubPath (8.0f, 11.5f); line.lineTo (11.8f, 4.2f);
        }
        else if (stem == "Piano")             // three keys
        {
            line.addRoundedRectangle (1.5f, 3.0f, 13.0f, 10.0f, 1.5f);
            line.startNewSubPath (5.8f, 8.5f); line.lineTo (5.8f, 13.0f);
            line.startNewSubPath (10.2f, 8.5f); line.lineTo (10.2f, 13.0f);
            fill.addRectangle (4.6f, 3.0f, 2.4f, 5.5f);
            fill.addRectangle (9.0f, 3.0f, 2.4f, 5.5f);
        }
        else if (stem == "Instrumental")      // a note
        {
            fill.addEllipse (3.0f, 10.2f, 5.6f, 4.0f);
            line.startNewSubPath (8.0f, 12.0f); line.lineTo (8.0f, 2.5f); line.lineTo (12.5f, 5.0f);
        }
        else                                  // Other: everything else, a sound wave (owner 20 Nov: the three dots read as a placeholder)
        {
            line.startNewSubPath (1.5f, 8.0f);
            line.cubicTo (3.0f, 2.0f, 4.5f, 2.0f, 5.75f, 8.0f);
            line.cubicTo (7.0f, 14.0f, 8.5f, 14.0f, 10.0f, 8.0f);
            line.cubicTo (11.25f, 3.5f, 12.75f, 3.5f, 14.5f, 8.0f);
        }
        g.setColour (colour);
        if (! line.isEmpty()) g.strokePath (line, stroke, t);
        if (! fill.isEmpty()) g.fillPath (fill, t);
    }

    void drawLaneFrame (juce::Graphics& g, juce::Rectangle<float> lane, juce::Colour tab, const juce::String& label, bool part,
                        bool dim, bool rail, float nameMaxW)
    {
        g.setColour (Theme::surface);
        g.fillRoundedRectangle (lane, Theme::radiusLane);
        g.setColour (Theme::background);
        g.fillRect (lane.getX() + (float) (Editor::kTL - 4 - Editor::kLaneX), lane.getY(), (float) Editor::kTW + 8.0f, lane.getHeight());
        if (rail)
        {
            g.setColour (Theme::stemDrums);
            g.fillRect (lane.getX() + 14.0f, lane.getY(), 2.0f, lane.getHeight());
        }
        g.setColour (dim ? tab.withAlpha (0.45f) : tab);
        g.fillRoundedRectangle (lane.getX() + (part ? 18.0f : 6.0f), lane.getY() + 5.0f, 4.0f, lane.getHeight() - 10.0f, 2.0f);
        if (! part)      // main lanes: the stem's glyph in its colour, then the name
            drawStemGlyph (g, label, { lane.getX() + 16.0f, lane.getCentreY() - 7.0f, 14.0f, 14.0f }, dim ? tab.withAlpha (0.45f) : tab);
        drawLine (g, label, lane.getX() + (part ? 30.0f : 36.0f), lane.getCentreY(), nameMaxW - (part ? 0.0f : 18.0f),
                  part ? Theme::regular (12.5f) : Theme::bold (13.5f), dim ? Theme::textMuted : Theme::text);
    }

    juce::Rectangle<float> laneWave (juce::Rectangle<float> lane)
    {
        return { lane.getX() + (float) (Editor::kTL - Editor::kLaneX), lane.getY() + 4.0f, (float) Editor::kTW, lane.getHeight() - 8.0f };
    }
}

// ==============================================================================
// MAIN EDITOR CLASS
// ==============================================================================

SubverseSplitterAudioProcessorEditor::SubverseSplitterAudioProcessorEditor (SubverseSplitterAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p)
#if SVL_LICENSING
    , licenseOverlay (p.license, [] {
          svl::LicenseOverlay::Style st;
          st.legend = [] (float px) { return Theme::bold (px * 0.85f); };
          st.fine   = [] (float px) { return Theme::regular (px * 0.9f); };
          return st; }())
#endif
{
    setLookAndFeel (&look);

    changeFileButton.getProperties().set ("variant", "ghost");
    changeFileButton.onClick = [this] { chooseFile(); };
    addChildComponent (changeFileButton);

    runButton.getProperties().set ("variant", "primary");
    runButton.onClick = [this] { runSelected(); };
    addChildComponent (runButton);

    // after a result: back to the options with the same file, or start over
    otherOpButton.getProperties().set ("variant", "ghost");
    otherOpButton.onClick = [this] {
        // the same track back at the options: the original file if it is still where it was, else the run's input
        juce::File src (shown.source);
        if (! src.existsAsFile()) src = audioProcessor.getModelRunner().getInputFile();
        const bool hadRegion = shown.hasRegion();
        const double a = shown.regionStart, b = shown.regionEnd;
        clearResult();
        if (src.existsAsFile())
        {
            loadFile (src);
            if (hadRegion && b - a >= 1.0) { regionA = a; regionB = b; }      // the markers of that run are still there
        }
        else goHome();
    };
    addChildComponent (otherOpButton);
    for (auto* b : { &homeButton, &historyButton, &cancelButton })
    {
        b->getProperties().set ("variant", "ghost");
        addChildComponent (*b);
    }
    homeButton.onClick = [this] { goHome(); };
    historyButton.onClick = [this] { showHistory(); };
    cancelButton.onClick = [this]
    {
        // stops the run at its next window; the timer then returns to where the run was started (no error line)
        cancelRequested = true;
        cancelButton.setEnabled (false);
        audioProcessor.getModelRunner().cancel();
    };

    fastButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("FAST \xc3\x97")) + "2");
    fastButton.getProperties().set ("variant", "switch");
    fastButton.setColour (juce::TextButton::textColourOnId, Theme::gold);
    fastButton.setTooltip ("Twice the speed, on computers with 16 GB of memory or more");
    fastButton.onClick = [this] { toggleFast(); };
    fastButton.setToggleState (audioProcessor.fastMode.load() && SubverseSplitterAudioProcessor::fastAvailable(), juce::dontSendNotification);
    addChildComponent (fastButton);

    // the gold action of a result: on the desktop the files are already on disk, so it shows them
    showFilesButton.getProperties().set ("variant", "ghost");      // gold is for Run only
    showFilesButton.onClick = [this] { if (shown.folder.isDirectory()) shown.folder.startAsProcess(); };
    addChildComponent (showFilesButton);

    audioButton.getProperties().set ("variant", "ghost");
    audioButton.onClick = [this] { if (audioProcessor.showAudioSettings) audioProcessor.showAudioSettings(); };
    addChildComponent (audioButton);
    audioButton.setVisible ((bool) audioProcessor.showAudioSettings);

#if SVL_LICENSING
    licenseButton.getProperties().set ("variant", "ghost");
    licenseButton.onClick = [this] { showLicence(); };
    addAndMakeVisible (licenseButton);
    addChildComponent (licenseOverlay);
    licenseOverlay.onLicensed = [this] { licenseOverlay.setVisible (false); repaint(); };
#endif

    newFileButton.getProperties().set ("variant", "ghost");
    newFileButton.setTooltip ("Open another track (or drop one on the window)");
    newFileButton.onClick = [this] { chooseFile(); };
    addChildComponent (newFileButton);

    openFolderButton.getProperties().set ("variant", "ghost");
    openFolderButton.onClick = [] { ElekLibrary::outputRoot().startAsProcess(); };
    addChildComponent (openFolderButton);

    splitDrumsButton.getProperties().set ("variant", "ghost");
    splitDrumsButton.setTooltip ("Split the Drums lane into kick, snare and hats & cymbals");
    splitDrumsButton.onClick = [this] { splitDrums(); };
    addChildComponent (splitDrumsButton);

    playPauseButton.onClick = [this]
    {
        // preview of a loaded file with a region: Play plays the region, from its start
        if (view() == View::Ready && hasRegion() && ! audioProcessor.getIsPlaying())
        {
            const double pos = audioProcessor.getPlaybackPosition();
            if (pos < regionA || pos >= regionB - 0.05) audioProcessor.setPlaybackPosition (regionA);
        }
        audioProcessor.togglePlayPause();
        if (audioProcessor.getIsPlaying())
            logMessage ("PLAY at " + juce::String (audioProcessor.getPlaybackPosition(), 2) + " s of " + juce::String (audioProcessor.getTotalLength(), 2)
                        + " s; device: " + (audioProcessor.describeAudioDevice ? audioProcessor.describeAudioDevice() : juce::String ("plug-in host")));
    };
    addChildComponent (playPauseButton);
    stopButton.setTooltip ("Stop and go back to the start of the region");
    stopButton.onClick = [this] { stopPlayback(); };
    addChildComponent (stopButton);

    // the SubverseLab mark from the brand kit (the family plugins' mark_cream.png)
    int size = 0;
    if (const char* data = BinaryData::getNamedResource ("mark_cream_png", size))
        markImage = juce::ImageCache::getFromMemory (data, size);
    addAndMakeVisible (markLink);
    markLink.toBack();

    drumHeader.onToggle = [this] { setDrumGroupOpen (! drumGroupOpen); };
    addChildComponent (drumHeader);
    splitDrumsButton.toFront (false);      // it sits ON the drum strip: added before it, the strip took its clicks (owner 18 Nov: "çalışmıyor")
    addChildComponent (dialog);

    setSize (kW, kH);
    startTimer (40);   // 25 FPS for the playhead and the progress

    // Debug hook: SPLITTER_AUTOTEST=<file> loads that file, runs 4 stems and starts playback when it finishes. A bug that
    // only appears in the Standalone cannot be chased from a console harness, and driving the real window by clicking is less reliable.
    if (auto* autoTest = std::getenv ("SPLITTER_AUTOTEST"))
    {
        const juce::File file { juce::String (autoTest) };
        if (file.existsAsFile())
        {
            autoPlayWhenReady = true;
            juce::MessageManager::callAsync ([this, file] { loadFile (file); selectOperation (1); runSelected(); });
        }
    }
    // A DAW closes and reopens plugin windows at will; the processor keeps the last result (and may be playing it), so a new
    // editor shows that result rather than an empty drop zone over audio that is still running.
    {
        auto& runner = audioProcessor.getModelRunner();
        if (runner.isRunning())
            awaitingResult = true;
        else if (runner.isFinishedSuccessfully() && ! autoPlayWhenReady)
            showRunnerResult (false);
    }
    refreshRecent();
    resized();
}

SubverseSplitterAudioProcessorEditor::~SubverseSplitterAudioProcessorEditor()
{
    alive->store (false);
    drumPanels.clear();
    panels.clear();
    setLookAndFeel (nullptr);
}

// ---------------------------------------------------------------- state

SubverseSplitterAudioProcessorEditor::View SubverseSplitterAudioProcessorEditor::view() const
{
    if (audioProcessor.getModelRunner().isRunning() && ! autoAnalysis) return View::Running;      // the background tempo / key reading is not a run
    if (historyShown)                                 return View::History;
    if (resultShown)                                  return View::Done;
    if (pendingFile != juce::File())                  return View::Ready;
    return View::Empty;
}

juce::String SubverseSplitterAudioProcessorEditor::viewNameForTest() const
{
    switch (view())
    {
        case View::Empty:   return "home";
        case View::Ready:   return "home with a file";
        case View::Running: return "running";
        case View::Done:    return "result";
        case View::History: return "history";
    }
    return {};
}

void SubverseSplitterAudioProcessorEditor::goHome()
{
    if (audioProcessor.getModelRunner().isRunning() && ! autoAnalysis) return;      // a run is left with Cancel
    needAnalysis = false;
    pendingAnalysis = {};
    historyShown = false;
    clearResult();
    pendingFile = juce::File();
    fileName.clear();
    errorText.clear();
    mixPeaks = {};
    mixPeaksFile = juce::File();
    regionA = regionB = -1.0;
    refreshRecent();
    resized();
    repaint();
}

void SubverseSplitterAudioProcessorEditor::showHistory()
{
    if (audioProcessor.getModelRunner().isRunning() && ! autoAnalysis) return;
    clearResult();                 // the result on screen stops playing; it is the first row of the list
    refreshRecent();
    historyShown = true;
    resized();
    repaint();
}

void SubverseSplitterAudioProcessorEditor::toggleFast()
{
    const auto m = SubverseSplitterAudioProcessor::machine();
    if (! SubverseSplitterAudioProcessor::fastAvailable())
    {
        fastButton.setToggleState (false, juce::dontSendNotification);
        audioProcessor.fastMode = false;
        dialog.show (fastButton.getButtonText() + " needs more memory",
                     "To turn this on, your computer needs at least 16 GB of memory. This one has "
                     + juce::String (juce::roundToInt (m.ramGB)) + " GB (" + m.chip + ").");
        return;
    }
    const bool on = ! audioProcessor.fastMode.load();
    audioProcessor.fastMode = on;
    fastButton.setToggleState (on, juce::dontSendNotification);
}

bool SubverseSplitterAudioProcessorEditor::splitDrumsReceivesClicksForTest()
{
    // The editor is not on screen in the harness (getComponentAt needs that), so this walks the children the way JUCE does:
    // topmost first, the first visible one that contains the point takes the click.
    const auto p = splitDrumsButton.getBounds().getCentre();
    for (int i = getNumChildComponents(); --i >= 0;)
    {
        auto* child = getChildComponent (i);
        if (! child->isVisible() || ! child->getBounds().contains (p) || ! child->hitTest (p.x - child->getX(), p.y - child->getY())) continue;
        if (child != &splitDrumsButton)
            std::cout << "  the click lands on " << typeid (*child).name() << " at " << child->getBounds().toString() << ", not on the key\n";
        return child == &splitDrumsButton;
    }
    return false;
}

// ---------------------------------------------------------------- the in-window dialog

SubverseSplitterAudioProcessorEditor::Dialog::Dialog()
{
    ok.getProperties().set ("variant", "primary");
    ok.onClick = [this] { setVisible (false); };
    addAndMakeVisible (ok);
    setVisible (false);
}

void SubverseSplitterAudioProcessorEditor::Dialog::show (const juce::String& newTitle, const juce::String& newText)
{
    title = newTitle;
    text = newText;
    setVisible (true);
    toFront (true);
    repaint();
}

juce::Rectangle<int> SubverseSplitterAudioProcessorEditor::Dialog::card() const
{
    return juce::Rectangle<int> (0, 0, 440, 190).withCentre (getLocalBounds().getCentre());
}

void SubverseSplitterAudioProcessorEditor::Dialog::resized()
{
    const auto r = card();
    ok.setBounds (r.getRight() - 24 - 110, r.getBottom() - 24 - 34, 110, 34);
}

void SubverseSplitterAudioProcessorEditor::Dialog::paint (juce::Graphics& g)
{
    g.fillAll (Theme::background.withAlpha (0.78f));
    const auto r = card().toFloat();
    g.setColour (Theme::surfaceRaised);
    g.fillRoundedRectangle (r, Theme::radius);
    g.setColour (Theme::line);
    g.drawRoundedRectangle (r.reduced (0.5f), Theme::radius, 1.0f);
    drawLine (g, title, r.getX() + 24.0f, r.getY() + 34.0f, r.getWidth() - 48.0f, Theme::bold (16.0f), Theme::text);
    drawWrapped (g, text, r.getX() + 24.0f, r.getY() + 70.0f, r.getWidth() - 48.0f, 19.0f, Theme::regular (13.5f), Theme::textMuted);
}

void SubverseSplitterAudioProcessorEditor::loadFile (const juce::File& file)
{
    clearResult();
    historyShown = false;
    pendingFile = file;
    fileName = file.getFileName();
    errorText.clear();
    regionA = regionB = -1.0;
    pendingSeconds = 0.0;
    pendingChannels = 0;
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    if (std::unique_ptr<juce::AudioFormatReader> r { fm.createReaderFor (file) })
    {
        pendingSeconds = r->sampleRate > 0 ? (double) r->lengthInSamples / r->sampleRate : 0.0;
        pendingChannels = (int) r->numChannels;
        loadMixPeaksAsync (file);
    }
    else
        errorText = "This file could not be read. Try WAV, AIFF, MP3 or FLAC.";
    pendingAnalysis = {};
    needAnalysis = errorText.isEmpty();      // the timer starts it as soon as the engine is free
    if (errorText.isEmpty()) setWholeFileRegion();
    preparePreview();
    resized();
    repaint();
}

void SubverseSplitterAudioProcessorEditor::preparePreview()
{
    // the loaded file itself on playback channel 0, so it can be heard (and the region found) before anything is separated
    audioProcessor.clearPlayback();
    if (pendingFile.existsAsFile() && errorText.isEmpty())
        audioProcessor.loadStemForPlayback (0, pendingFile);
}

bool SubverseSplitterAudioProcessorEditor::previewReady() const
{
    return view() == View::Ready && errorText.isEmpty() && audioProcessor.getTotalLength() > 0.0;
}

int SubverseSplitterAudioProcessorEditor::regionEdgeAt (int x) const
{
    const double len = shownLength();
    if (! hasRegion() || len <= 0.0) return 0;
    const int xa = kTL + juce::roundToInt ((double) kTW * regionA / len), xb = kTL + juce::roundToInt ((double) kTW * regionB / len);
    const int da = std::abs (x - xa), db = std::abs (x - xb);
    if (juce::jmin (da, db) > kEdgeGrab) return 0;
    return da <= db ? 1 : 2;
}

void SubverseSplitterAudioProcessorEditor::timelineGestureForTest (int x1, int x2, int clicks)
{
    const int y = timelineBounds().getCentreY();
    auto source = juce::Desktop::getInstance().getMainMouseSource();
    auto event = [&] (int x, bool dragged)
    {
        return juce::MouseEvent (source, juce::Point<float> ((float) x, (float) y), juce::ModifierKeys::leftButtonModifier, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                 this, this, juce::Time::getCurrentTime(), juce::Point<float> ((float) x1, (float) y), juce::Time::getCurrentTime(), clicks, dragged);
    };
    mouseDown (event (x1, false));
    if (x2 != x1)
        for (int k = 1; k <= 4; ++k) mouseDrag (event (x1 + (x2 - x1) * k / 4, true));
    mouseUp (event (x2, x2 != x1));
}

void SubverseSplitterAudioProcessorEditor::selectOperation (int index)
{
    selected = juce::jlimit (-1, kNumOptions - 1, index);
    if (selected >= 0 && opFor (selected) == Operation::DrumsOnly && ! drumModelInstalled()) selected = -1;     // nothing to run without the drum model
    runButton.setButtonText (selected < 0 ? juce::String ("Choose what to make") : "Run " + juce::String (kOptions[selected].label));
    runButton.setEnabled (selected >= 0 && errorText.isEmpty() && ! autoAnalysis);      // a second or two while tempo and key are read
    repaint();
}

void SubverseSplitterAudioProcessorEditor::runSelected()
{
    if (selected < 0 || pendingFile == juce::File() || audioProcessor.getModelRunner().isRunning()) return;
#if SVL_LICENSING
    if (opFor (selected) != Operation::Analysis && ! audioProcessor.license.isLicensed())
    {
        showLicence();      // separations need a key; Key & BPM never does
        return;
    }
#endif
    clearResult();
    errorText.clear();
    rawProgress = 0.0; windowsReporting = false; windowFraction = 0.0;
    audioProcessor.getModelRunner().startOperation (pendingFile, opFor (selected), hasRegion() && ! regionIsWholeFile() ? regionA : -1.0,
                                                    hasRegion() && ! regionIsWholeFile() ? regionB : -1.0);
    awaitingResult = true;      // a Key & BPM run can end before the next timer tick sees it running
    resized();
    repaint();
}

void SubverseSplitterAudioProcessorEditor::clearResult()
{
    drumPanels.clear();
    drumParts.clear();
    drumsRow = -1;
    drumHeader.setVisible (false);
    panels.clear();
    audioProcessor.clearPlayback();
    resultShown = false;
    lanesBottom = 0;
}

void SubverseSplitterAudioProcessorEditor::showRunnerResult (bool loadPlayback)
{
    auto& runner = audioProcessor.getModelRunner();
    if (runner.getOperation() == Operation::Analysis)
    {
        // Key & BPM writes nothing: the analysis is the result
        shown = {};
        shownOp = Operation::Analysis;
        shownAnalysis = runner.getAnalysis();
        fileName = runner.getInputFile().getFileName();
        shown.source = runner.getInputFile().getFullPathName();
        resultShown = true;
        pendingFile = juce::File();
        loadMixPeaksAsync (runner.getInputFile());
        return;
    }
    ElekLibrary::Entry e;
    if (ElekLibrary::readManifest (runner.getOutputFolder(), e))
        buildResult (e, loadPlayback);
}

void SubverseSplitterAudioProcessorEditor::buildResult (const ElekLibrary::Entry& entry, bool loadPlayback)
{
    drumPanels.clear();
    panels.clear();
    drumsRow = -1;

    shown = entry;
    shownOp = entry.operation == "vocal_removal" ? Operation::VocalRemoval : entry.operation == "stem_separation_6" ? Operation::Stems6
            : entry.operation == "drums_only" ? Operation::DrumsOnly : Operation::Stems;
    shownAnalysis = entry.analysis;
    fileName = entry.hasRegion() && entry.title.containsChar (' ') ? entry.title.upToLastOccurrenceOf (" ", false, false) : entry.title;   // the region is in the subtitle
    resultShown = true;
    pendingFile = juce::File();

    struct Row { const char* label; const char* key; juce::Colour colour; };
    std::vector<Row> rows;
    if (shownOp == Operation::VocalRemoval)
        rows = { { "Vocals", "vocals", Theme::stemVocals }, { "Instrumental", "instrumental", Theme::stemInstrumental } };
    else if (shownOp == Operation::DrumsOnly)
        rows = { { "Drums", "drums", Theme::stemDrums } };
    else if (shownOp == Operation::Stems6)
        rows = { { "Vocals", "vocals", Theme::stemVocals }, { "Drums", "drums", Theme::stemDrums },
                 { "Bass", "bass", Theme::stemBass },       { "Guitar", "guitar", Theme::stemGuitar },
                 { "Piano", "piano", Theme::stemPiano },    { "Other", "other", Theme::stemOther } };
    else
        rows = { { "Vocals", "vocals", Theme::stemVocals }, { "Drums", "drums", Theme::stemDrums },
                 { "Bass", "bass", Theme::stemBass },       { "Other", "other", Theme::stemOther } };

    if (loadPlayback)      // false for a reopened editor: the stems are already loaded, and may be playing
        audioProcessor.clearPlayback();

    for (int i = 0; i < (int) rows.size(); ++i)
    {
        const auto file = entry.file (rows[(size_t) i].key);
        if (loadPlayback)
            audioProcessor.loadStemForPlayback (i, file);
        panels.push_back (std::make_unique<StemPanel> (rows[(size_t) i].label, file, rows[(size_t) i].colour, i, audioProcessor, *this));
        addAndMakeVisible (panels.back().get());
    }

    // Peaks of every lane and of the mix, in one pass over the stems: the overview is the SUM of the main stems (what the
    // player plays at rest), so it shares the lanes' time axis exactly. Read on the message thread, as the old rows did.
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::vector<std::unique_ptr<juce::AudioFormatReader>> readers;
        juce::int64 longest = 0;
        double rate = 44100.0;
        for (auto& r : rows)
        {
            readers.emplace_back (fm.createReaderFor (entry.file (r.key)));
            if (readers.back() != nullptr) { longest = juce::jmax (longest, readers.back()->lengthInSamples); rate = readers.back()->sampleRate; }
        }
        const int n = kPeakPoints;
        const int spp = (int) juce::jmax ((juce::int64) 1, longest / n);
        std::vector<std::vector<float>> lanePeaks (rows.size(), std::vector<float> ((size_t) n, 0.0f));
        Peaks mix;
        mix.values.assign ((size_t) n, 0.0f);
        mix.seconds = rate > 0.0 ? (double) longest / rate : 0.0;
        juce::AudioBuffer<float> sum (2, spp), tmp (2, spp);
        std::vector<double> laneEnergy (rows.size(), 0.0), laneDcL (rows.size(), 0.0), laneDcR (rows.size(), 0.0);
        float ref = 0.0f;
        for (int i = 0; i < n && longest > 0; ++i)
        {
            sum.clear();
            for (size_t s = 0; s < readers.size(); ++s)
            {
                if (readers[s] == nullptr) continue;
                tmp.clear();
                readers[s]->read (&tmp, 0, spp, (juce::int64) i * spp, true, true);
                const float m = tmp.getMagnitude (0, spp);
                lanePeaks[s][(size_t) i] = m;
                {
                    const double l = tmp.getRMSLevel (0, 0, spp), r = tmp.getRMSLevel (1, 0, spp);
                    laneEnergy[s] += 0.5 * (l * l + r * r);
                    double sl = 0.0, sr = 0.0;
                    for (int k = 0; k < spp; ++k) { sl += tmp.getSample (0, k); sr += tmp.getSample (1, k); }
                    laneDcL[s] += sl / (double) spp; laneDcR[s] += sr / (double) spp;
                }
                ref = juce::jmax (ref, m);
                sum.addFrom (0, 0, tmp, 0, 0, spp);
                sum.addFrom (1, 0, tmp, 1, 0, spp);
            }
            mix.values[(size_t) i] = sum.getMagnitude (0, spp);
            mix.peak = juce::jmax (mix.peak, mix.values[(size_t) i]);
        }
        laneReference = ref > 0.0f ? ref : 1.0f;
        laneReferenceRms = 0.0f;
        std::vector<float> laneRms (rows.size(), 0.0f);
        for (size_t s = 0; s < rows.size(); ++s)
        {
            // without the mean: a DC offset is not sound, and it kept a silent stem from being drawn Silent
            const double mL = laneDcL[s] / (double) n, mR = laneDcR[s] / (double) n;
            laneRms[s] = (float) std::sqrt (juce::jmax (0.0, laneEnergy[s] / (double) n - 0.5 * (mL * mL + mR * mR)));
            laneReferenceRms = juce::jmax (laneReferenceRms, laneRms[s]);
        }
        const float silentBelow = laneReferenceRms * juce::Decibels::decibelsToGain (kSilentBelowDb);
        for (size_t s = 0; s < panels.size(); ++s)
        {
            panels[s]->setPeaks (lanePeaks[s], laneReference);
            panels[s]->setSilent (laneReferenceRms > 0.0f && laneRms[s] < silentBelow);
        }
        mixPeaks = std::move (mix);
        mixPeaksFile = juce::File();
        ++mixPeaksToken;          // a peak read still running for an earlier file is dropped
    }

    // the drum parts sit under the Drums row (the two vocal-removal rows have none)
    for (int i = 0; i < (int) panels.size(); ++i)
        if (panels[(size_t) i]->getName() == "Drums") drumsRow = i;
    const auto parts = drumsRow >= 0 ? drumPartsFor (entry) : std::vector<DrumPart>();
    if (loadPlayback && drumsRow >= 0)
    {
        // the parts play in place of the Drums row when any part control is used (rule in PluginProcessor::applyGains)
        for (const auto& part : parts)
            if (part.stemIndex >= 0)
                audioProcessor.loadStemForPlayback (part.stemIndex, part.file);
        audioProcessor.setDrumsChannel (drumsRow);
    }
    buildDrumPanels (parts);
    int available = 0;
    for (auto& part : parts) available += part.enabled ? 1 : 0;
    setDrumGroupOpen (available > 0);    // open when there is something in it; "Not split for this run" stays closed
}

std::vector<SubverseSplitterAudioProcessorEditor::DrumPart>
SubverseSplitterAudioProcessorEditor::drumPartsFor (const ElekLibrary::Entry& entry)
{
    struct Def { const char* key; const char* label; };
    // bombo, redoblante, platillos, toms. Keys as the engine writes them (PluginProcessor.cpp, runDrumSplit): the stem word of
    // the file name in lower case, like the main stems ("drums" -> "<title>_Drums_..."), so "kick" -> "<title>_Kick_...".
    static const Def defs[] = { { "kick", "Kick" }, { "snare", "Snare" }, { "cymbals", "Hats & cymbals" }, { "toms", "Toms" } };
    std::vector<DrumPart> out;
    int k = 0;
    for (const auto& d : defs)
    {
        DrumPart part;
        part.key = d.key;
        part.label = d.label;
        part.file = entry.file (d.key);
        part.enabled = part.file.existsAsFile();
        if (juce::String (d.key) == "toms" && ! part.enabled) { ++k; continue; }      // from 20 Nov the toms are in the kick: no row
        // playback channel 6..9 (the processor's drum part channels); only a part with audio gets one
        part.stemIndex = part.enabled ? SubverseSplitterAudioProcessor::kFirstDrumPart + k : -1;
        out.push_back (part);
        ++k;
    }
    return out;
}

void SubverseSplitterAudioProcessorEditor::buildDrumPanels (const std::vector<DrumPart>& parts)
{
    drumPanels.clear();
    drumParts = parts;
    drumHeader.setVisible (! parts.empty() && resultShown);
    int available = 0;
    std::vector<std::vector<float>> partPeaks;
    std::vector<float> partRms;
    float ref = laneReference;
    for (const auto& part : parts)
    {
        auto row = std::make_unique<StemPanel> (part.label, part.file, Theme::stemDrums, part.stemIndex, audioProcessor, *this);
        row->setCompact (true);
        row->setRail (true);
        row->setHasData (part.enabled);
        Peaks pk;
        if (part.enabled)
        {
            ++available;
            pk = readPeaks (part.file, kPeakPoints);
            ref = juce::jmax (ref, pk.peak);
        }
        partRms.push_back (pk.rms);
        partPeaks.push_back (std::move (pk.values));
        addChildComponent (row.get());
        drumPanels.push_back (std::move (row));
    }
    if (ref > laneReference)      // a part louder than every stem: one common reference for all lanes
    {
        laneReference = ref;
        for (auto& panel : panels) panel->setPeaks ({}, laneReference);
    }
    for (size_t i = 0; i < drumPanels.size(); ++i)
    {
        drumPanels[i]->setPeaks (std::move (partPeaks[i]), laneReference);
        drumPanels[i]->setSilent (parts[i].enabled && laneReferenceRms > 0.0f && partRms[i] < laneReferenceRms * juce::Decibels::decibelsToGain (kSilentBelowDb));
    }
    drumHeader.state = available == 0 ? (drumModelInstalled() ? juce::String ("Not split yet") : juce::String ("Drum model not installed"))
                                      : juce::String (available) + " of " + juce::String ((int) parts.size()) + " parts";
    drumHeader.open = drumGroupOpen;
    for (auto& r : drumPanels) r->setVisible (drumGroupOpen);
}

bool SubverseSplitterAudioProcessorEditor::drumsCanBeSplit() const
{
    if (view() != View::Done || drumsRow < 0 || ! shown.folder.isDirectory() || ! shown.file ("drums").existsAsFile()) return false;
    for (const auto& p : drumParts) if (p.enabled) return false;
    return drumModelInstalled();
}

void SubverseSplitterAudioProcessorEditor::splitDrums()
{
    if (! drumsCanBeSplit() || audioProcessor.getModelRunner().isRunning()) return;
#if SVL_LICENSING
    if (! audioProcessor.license.isLicensed()) { showLicence(); return; }
#endif
    errorText.clear();
    rawProgress = 0.0; windowsReporting = false; windowFraction = 0.0;
    audioProcessor.getModelRunner().startOperation (shown.file ("drums"), Operation::DrumParts, -1.0, -1.0, shown.folder);
    awaitingResult = true;
    resized();
    repaint();
}

void SubverseSplitterAudioProcessorEditor::setDrumGroupOpen (bool shouldBeOpen)
{
    drumGroupOpen = shouldBeOpen;
    drumHeader.open = shouldBeOpen;
    for (auto& r : drumPanels) r->setVisible (shouldBeOpen);
    resized();
    repaint();
}

void SubverseSplitterAudioProcessorEditor::showEntryForTest (const ElekLibrary::Entry& entry, const std::vector<DrumPart>& parts)
{
    buildResult (entry, true);
    if (drumsRow >= 0) buildDrumPanels (parts);
    resized();
    repaint();
}

void SubverseSplitterAudioProcessorEditor::openRecent (int index)
{
    if (index < 0 || index >= (int) recentRuns.size()) return;
    const auto entry = recentRuns[(size_t) index];
    historyShown = false;
    if (entry.usable()) buildResult (entry, true);
    else refreshRecent();                   // deleted since the list was read
    resized();
    repaint();
}

#if SVL_LICENSING
void SubverseSplitterAudioProcessorEditor::showLicence()
{
    licenseOverlay.refresh();
    licenseOverlay.setVisible (true);
    licenseOverlay.toFront (true);
}
#endif

void SubverseSplitterAudioProcessorEditor::refreshRecent()
{
    recentRuns = ElekLibrary::recent (20);
}

// ---------------------------------------------------------------- peaks

SubverseSplitterAudioProcessorEditor::Peaks SubverseSplitterAudioProcessorEditor::readPeaks (const juce::File& file, int points)
{
    Peaks out;
    if (! file.existsAsFile() || points <= 0) return out;
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
    if (reader == nullptr || reader->lengthInSamples <= 0) return out;

    // peak per block, not RMS: a waveform shows how far the signal went, and RMS flattens a drum stem to nothing
    const int spp = (int) juce::jmax ((juce::int64) 1, reader->lengthInSamples / points);
    juce::AudioBuffer<float> b (juce::jmax (2, (int) reader->numChannels), spp);
    out.values.assign ((size_t) points, 0.0f);
    out.seconds = reader->sampleRate > 0 ? (double) reader->lengthInSamples / reader->sampleRate : 0.0;
    double energy = 0.0, dc[2] = { 0.0, 0.0 };
    for (int i = 0; i < points; ++i)
    {
        b.clear();
        reader->read (&b, 0, spp, (juce::int64) i * spp, true, true);
        out.values[(size_t) i] = b.getMagnitude (0, spp);
        out.peak = juce::jmax (out.peak, out.values[(size_t) i]);
        const double l = b.getRMSLevel (0, 0, spp), r = b.getRMSLevel (1, 0, spp);
        energy += 0.5 * (l * l + r * r);
        for (int ch = 0; ch < 2; ++ch)
        {
            const float* d = b.getReadPointer (ch);
            double s = 0.0;
            for (int k = 0; k < spp; ++k) s += d[k];
            dc[ch] += s / (double) spp;
        }
    }
    // RMS with the mean removed: a DC offset is not sound (stems written before 18 Nov carry the mix's offset)
    const double mL = dc[0] / (double) points, mR = dc[1] / (double) points;
    out.rms = (float) std::sqrt (juce::jmax (0.0, energy / (double) points - 0.5 * (mL * mL + mR * mR)));
    return out;
}

void SubverseSplitterAudioProcessorEditor::loadMixPeaksAsync (const juce::File& file)
{
    if (file == mixPeaksFile && ! mixPeaks.values.empty()) return;
    mixPeaksFile = file;
    mixPeaks = {};
    const int token = ++mixPeaksToken;
    juce::Component::SafePointer<SubverseSplitterAudioProcessorEditor> safe (this);
    auto stillAlive = alive;
    // a whole MP3 is decoded for its peaks: on a worker, so the window never waits for it
    juce::Thread::launch ([safe, stillAlive, file, token]
    {
        if (! stillAlive->load()) return;
        auto peaks = readPeaks (file, kPeakPoints);
        juce::MessageManager::callAsync ([safe, token, peaks = std::move (peaks)] () mutable
        {
            if (auto* ed = safe.getComponent())
                if (ed->mixPeaksToken == token) { ed->mixPeaks = std::move (peaks); ed->repaint(); }
        });
    });
}

void SubverseSplitterAudioProcessorEditor::envelopePath (juce::Path& path, const std::vector<float>& peaks, juce::Rectangle<float> area,
                                                         float ref, bool curve, float from, float to)
{
    path.clear();
    const int n = (int) peaks.size();
    if (n == 0 || area.getWidth() < 2.0f || to <= from) return;
    if (! (ref > 0.0f)) ref = 1.0f;
    const int cols = juce::jmax (2, (int) std::ceil (area.getWidth() * 2.0f));   // half-pixel columns
    const int c0 = juce::jlimit (0, cols - 1, (int) std::floor (from * (float) cols));
    const int c1 = juce::jlimit (c0 + 1, cols, (int) std::ceil (to * (float) cols));
    const float cy = area.getCentreY(), half = area.getHeight() * 0.5f * 0.92f;
    std::vector<juce::Point<float>> top;
    top.reserve ((size_t) (c1 - c0 + 1));
    for (int c = c0; c < c1; ++c)
    {
        const int b0 = c * n / cols, b1 = juce::jmax (b0 + 1, (c + 1) * n / cols);
        float m = 0.0f;
        for (int b = b0; b < b1 && b < n; ++b) m = juce::jmax (m, peaks[(size_t) b]);
        float v = juce::jlimit (0.0f, 1.0f, m / ref);
        if (curve) v = std::sqrt (v);
        const float a = juce::jmax (0.02f, v) * half;
        top.push_back ({ area.getX() + area.getWidth() * (float) c / (float) cols, a });
    }
    top.push_back ({ area.getX() + area.getWidth() * (float) c1 / (float) cols, top.back().y });
    path.startNewSubPath (top.front().x, cy - top.front().y);
    for (auto& p : top) path.lineTo (p.x, cy - p.y);
    for (auto it = top.rbegin(); it != top.rend(); ++it) path.lineTo (it->x, cy + it->y);
    path.closeSubPath();
}

// ---------------------------------------------------------------- layout

juce::Rectangle<int> SubverseSplitterAudioProcessorEditor::overviewCard() const
{
    return view() == View::Empty ? juce::Rectangle<int> (kPad, kCardTop, getWidth() - 2 * kPad, 200)
                                 : juce::Rectangle<int> (kPad, kCardTop, getWidth() - 2 * kPad, kCardH);
}

juce::Rectangle<int> SubverseSplitterAudioProcessorEditor::dropZone() const
{
    return { kPad + 12, kCardTop + 12, getWidth() - 2 * kPad - 24, 176 };
}

juce::Rectangle<int> SubverseSplitterAudioProcessorEditor::optionBounds (int index) const
{
    // four tiles in a row under the file (home: under the drop zone)
    return { kPad + index * 241, view() == View::Empty ? 296 : 252, 229, 300 };
}

juce::Rectangle<int> SubverseSplitterAudioProcessorEditor::recentRow (int index) const
{
    // the History screen: three columns from the top
    const int top = kCardTop + 36, w = 309, h = 56;
    juce::Rectangle<int> r (kPad + (index % 3) * (w + 12), top + (index / 3) * (h + 8), w, h);
    return r.getBottom() <= getHeight() - kFooter - kBarH - 8 ? r : juce::Rectangle<int>();
}

juce::Rectangle<int> SubverseSplitterAudioProcessorEditor::timelineBounds() const
{
    return { kTL, kCardTop + 8, kTW, 132 };   // the mix waveform and its ruler: click and drag seek
}

juce::Rectangle<float> SubverseSplitterAudioProcessorEditor::markArea() const
{
    return svl::markRect (markImage, (float) (getWidth() - kPad), (float) (getHeight() - kFooter) + 16.0f, 20.0f);
}

double SubverseSplitterAudioProcessorEditor::shownLength() const
{
    if (view() == View::Done && ! panels.empty())
    {
        const double len = audioProcessor.getTotalLength();
        if (len > 0.0) return len;
    }
    if (mixPeaks.seconds > 0.0) return mixPeaks.seconds;
    return pendingSeconds;
}

float SubverseSplitterAudioProcessorEditor::playheadX() const
{
    const double len = shownLength();
    const double pos = audioProcessor.getPlaybackPosition();
    return (float) kTL + (float) kTW * (float) (len > 0.0 ? juce::jlimit (0.0, 1.0, pos / len) : 0.0);
}

void SubverseSplitterAudioProcessorEditor::repaintPlayhead()
{
    const float x = playheadX();
    const int bottom = juce::jmax (lanesBottom, kCardTop + kCardH);
    if (lastPlayheadX >= 0.0f) repaint ((int) lastPlayheadX - 3, 58, 8, bottom - 58 + 2);
    repaint ((int) x - 3, 58, 8, bottom - 58 + 2);
    lastPlayheadX = x;
    const auto clock = formatTime (audioProcessor.getPlaybackPosition());
    if (clock != lastClock) { lastClock = clock; repaint (118, kCardTop + 120, 136, 28); }
}

void SubverseSplitterAudioProcessorEditor::resized()
{
    markLink.setBounds (markArea().toNearestInt());
    audioButton.setBounds (getWidth() - kPad - 120, (kHeader - 28) / 2, 120, 28);
#if SVL_LICENSING
    licenseButton.setBounds ((audioButton.isVisible() ? audioButton.getX() - 10 : getWidth() - kPad) - 90, (kHeader - 28) / 2, 90, 28);
    licenseOverlay.setBounds (getLocalBounds());
#endif

    dialog.setBounds (getLocalBounds());
    const auto v = view();
    const bool separated = v == View::Done && shownOp != Operation::Analysis;
    const int barY = getHeight() - kFooter - kBarH + 4, right = getWidth() - kPad;      // the action bar: 32 high keys

    // header navigation, left of Audio output / Licence: Home from a result or History; History from anywhere but a run
    {
        int x = audioButton.isVisible() ? audioButton.getX() - 10 : right;
       #if SVL_LICENSING
        x = licenseButton.getX() - 10;
       #endif
        // owner 20 Nov: one way into History, always in the same place (it was also "Earlier separations" on the home row)
        homeButton.setVisible (v == View::Done || v == View::History);
        historyButton.setVisible (v != View::Running && v != View::History);
        historyButton.setBounds (x - 84, (kHeader - 28) / 2, 84, 28);
        homeButton.setBounds (historyButton.isVisible() ? x - 84 - 8 - 76 : x - 84, (kHeader - 28) / 2, historyButton.isVisible() ? 76 : 84, 28);
    }
    cancelButton.setVisible (v == View::Running);
    cancelButton.setBounds (right - 110, barY, 110, 32);
    if (v != View::Running) cancelButton.setEnabled (true);

    // home: the row under the tiles: earlier separations on the left, FAST and Run on the right
    {
        const bool home = v == View::Empty || v == View::Ready;
        const int y = optionBounds (0).getBottom() + 16;
        fastButton.setVisible (home);
        fastButton.setBounds (v == View::Ready ? right - 229 - 12 - 110 : right - 110, y, 110, 40);
    }

    openFolderButton.setVisible (v == View::History);
    openFolderButton.setBounds (right - 180, barY, 180, 32);

    changeFileButton.setVisible (v == View::Ready);
    changeFileButton.setBounds (kTL - 14 - 86, kCardTop + 6, 86, 22);      // on the MIX caption line, clear of the transport row
    runButton.setVisible (v == View::Ready);
    const auto lastDoor = optionBounds (kNumOptions - 1);
    runButton.setBounds (lastDoor.getX(), lastDoor.getBottom() + 16, lastDoor.getWidth(), 40);       // right under the tiles
    if (v == View::Ready) selectOperation (selected);

    playPauseButton.setVisible (separated || (v == View::Ready && errorText.isEmpty()));      // Ready: preview of the loaded file
    // the transport row (owner 20 Nov): play and stop the same size, the clock on one line beside them, on every view
    playPauseButton.setBounds (40, kCardTop + 117, 32, 32);
    stopButton.setVisible (playPauseButton.isVisible());
    stopButton.setBounds (80, kCardTop + 117, 32, 32);
    otherOpButton.setVisible (v == View::Done);
    otherOpButton.setBounds (right - 150, barY, 150, 32);
    showFilesButton.setVisible (separated);
    showFilesButton.setBounds (right - 150 - 10 - 110, barY, 110, 32);
    newFileButton.setVisible (v == View::Done);
    newFileButton.setBounds (right - 150 - 10 - 110 - 10 - 100, barY, 100, 32);

    for (auto& p : panels) p->setVisible (separated);
    layoutLanes();
}

/*  The lanes, top to bottom on the shared time axis: each main stem 42 high, 3 apart; under Drums the DRUM PARTS strip (24)
    and, when open, the four part lanes (32). Six stems + four parts end at y 670, inside the window (the lanes stop at 716). */
void SubverseSplitterAudioProcessorEditor::layoutLanes()
{
    // LAYOUT 2: the lanes share the height between the overview card and the action bar. A main lane is 40..80 high, a drum
    // part lane 0.7 of that (30..48). Six stems: 67 each; six stems with the four parts open: 44 and 31.
    const bool separated = view() == View::Done && shownOp != Operation::Analysis;
    const bool hasGroup = separated && drumsRow >= 0 && ! drumParts.empty();
    drumHeader.setVisible (hasGroup);
    splitDrumsButton.setVisible (false);
    if (! separated) { for (auto& r : drumPanels) r->setVisible (false); lanesBottom = 0; return; }

    const int n = (int) panels.size();
    const int openParts = hasGroup && drumGroupOpen ? (int) drumPanels.size() : 0;
    const int avail = getHeight() - kFooter - kBarH - 8 - kLanesTop;
    const int fixed = (hasGroup ? kGroupH : 0) + kLaneGap * (n + openParts - 1);
    const double unit = (double) (avail - fixed) / ((double) n + 0.7 * (double) openParts);
    const int rowH = juce::jlimit (40, 80, (int) std::floor (unit));
    const int partH = juce::jlimit (30, 48, (int) std::floor (unit * 0.7));

    int y = kLanesTop;
    for (int i = 0; i < n; ++i)
    {
        panels[(size_t) i]->setBounds (kLaneX, y, kLaneW, rowH);
        y += rowH + kLaneGap;
        if (i == drumsRow && hasGroup)
        {
            drumHeader.setBounds (kLaneX, y - 1, kLaneW, kGroupH);
            const bool canSplit = drumsCanBeSplit();
            splitDrumsButton.setBounds (kTL + kTW - 140, y + 1, 140, kGroupH - 4);
            splitDrumsButton.setVisible (canSplit);
            drumHeader.rightInset = canSplit ? 150 : 0;
            y += kGroupH;
            for (auto& r : drumPanels)
            {
                r->setVisible (drumGroupOpen);
                if (! drumGroupOpen) continue;
                r->setBounds (kLaneX, y, kLaneW, partH);
                y += partH + kLaneGap;
            }
        }
    }
    lanesBottom = y - kLaneGap;
}

// ---------------------------------------------------------------- painting

void SubverseSplitterAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (Theme::background);
    paintHeader (g);
    if (view() == View::History) paintHistory (g);
    else
    {
        paintOverview (g);
        paintInspector (g);      // what sits under the card: the tiles, the working steps, or the result's bar
    }
    paintSpineAndPlayhead (g);
    paintFooter (g);
}

void SubverseSplitterAudioProcessorEditor::paintHeader (juce::Graphics& g)
{
    // product name top left: the sieve mark and the name; what it is; the version as its own label
    drawMark (g, (float) kPad, (kHeader - 28) * 0.5f, 28.0f);
    const auto nameFont = Theme::bold (17.0f);
    g.setColour (Theme::text);
    Theme::drawTracked (g, "ELEK", { (float) kPad + 38.0f, kHeader * 0.5f - 10.0f, 100.0f, 20.0f }, nameFont, 2.4f, juce::Justification::centredLeft);
    float x = (float) kPad + 38.0f + Theme::trackedWidth ("ELEK", nameFont, 2.4f) + 16.0f;
    g.setColour (Theme::line);
    g.fillRect (x, kHeader * 0.5f - 11.0f, 1.0f, 22.0f);
    x += 13.0f;
    g.setColour (Theme::textMuted);
    const auto f = Theme::regular (10.5f);
    Theme::drawTracked (g, "STEM SEPARATOR", { x, kHeader * 0.5f - 8.0f, 200.0f, 16.0f }, f, 1.2f, juce::Justification::centredLeft);
    x += Theme::trackedWidth ("STEM SEPARATOR", f, 1.2f) + 10.0f;
    drawLine (g, JucePlugin_VersionString, x, kHeader * 0.5f, 80.0f, Theme::bold (10.5f), Theme::textMuted);

    g.setColour (Theme::line);
    g.fillRect (0, kHeader, getWidth(), 1);
}

void SubverseSplitterAudioProcessorEditor::paintFooter (juce::Graphics& g)
{
    const float y0 = (float) (getHeight() - kFooter);
    g.setColour (Theme::background);
    g.fillRect (0.0f, y0, (float) getWidth(), (float) kFooter);
    g.setColour (Theme::line);
    g.fillRect (0.0f, y0, (float) getWidth(), 1.0f);
    g.setColour (Theme::textMuted);
    Theme::drawTracked (g, "RUNS ON THIS COMPUTER. NOTHING IS UPLOADED.", { (float) kPad, y0 + 8.0f, 600.0f, 16.0f },
                        Theme::regular (10.5f), 1.0f, juce::Justification::centredLeft);
    {
        // what this computer is (the app reads it; FAST is offered from 16 GB up)
        const auto m = SubverseSplitterAudioProcessor::machine();
        const juce::String spec = (m.chip + dot + juce::String (juce::roundToInt (m.ramGB)) + " GB memory" + dot
                                   + juce::String (m.performanceCores) + " performance cores").toUpperCase();
        Theme::drawTracked (g, spec, { (float) getWidth() - 480.0f - 70.0f, y0 + 8.0f, 480.0f, 16.0f }, Theme::regular (10.5f), 1.0f, juce::Justification::centredRight);
    }
    // icon only (owner 14 Nov 2026: no wordmark); markLink lies over it and opens the manual
    if (markImage.isValid())
    {
        g.setOpacity (1.0f);
        g.drawImage (markImage, markArea(), juce::RectanglePlacement::stretchToFit);
    }
}

void SubverseSplitterAudioProcessorEditor::paintOverview (juce::Graphics& g)
{
    const auto v = view();
    const auto card = overviewCard().toFloat();
    g.setColour (v == View::Empty && isDraggingActive ? Theme::surfaceRaised : Theme::surface);
    g.fillRoundedRectangle (card, Theme::radius);

    if (v == View::Empty)
    {
        const auto zone = dropZone().toFloat();
        juce::Path outline, dashed;
        outline.addRectangle (zone);
        const float dashes[] = { 8.0f, 6.0f };
        juce::PathStrokeType (1.5f).createDashedStroke (dashed, outline, dashes, 2);
        g.setColour (isDraggingActive ? Theme::teal : Theme::surfaceHigh);
        g.fillPath (dashed);

        // a tray with an arrow into it, above the words
        const float ix = zone.getCentreX(), iy = zone.getY() + 50.0f;
        juce::Path tray;
        tray.startNewSubPath (ix - 22.0f, iy + 4.0f); tray.lineTo (ix - 22.0f, iy + 16.0f); tray.lineTo (ix + 22.0f, iy + 16.0f); tray.lineTo (ix + 22.0f, iy + 4.0f);
        tray.startNewSubPath (ix, iy - 22.0f); tray.lineTo (ix, iy + 6.0f);
        tray.startNewSubPath (ix - 10.0f, iy - 4.0f); tray.lineTo (ix, iy + 6.0f); tray.lineTo (ix + 10.0f, iy - 4.0f);
        g.setColour (isDraggingActive ? Theme::tealLight : Theme::text);
        g.strokePath (tray, juce::PathStrokeType (3.0f, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));

        drawLine (g, isDraggingActive ? "Release to load" : "Drop a track here", zone.getX(), zone.getY() + 104.0f, zone.getWidth(),
                  Theme::bold (22.0f), Theme::text, juce::Justification::centred);
        drawLine (g, "WAV, AIFF, MP3 or FLAC. Or click to choose a file.", zone.getX(), zone.getY() + 132.0f, zone.getWidth(),
                  Theme::regular (13.5f), Theme::textMuted, juce::Justification::centred);
        return;
    }

    // left column of the card: what it is, name, length and operation, tempo and key; the waveform on the shared time axis
    const float tx = 40.0f, tw = (float) (kTL - 40 - 14), top = (float) kCardTop;
    const bool analysisDone = v == View::Done && shownOp == Operation::Analysis;
    caption (g, analysisDone ? "Key & BPM" : "Mix", tx, top + 16.0f);
    drawLine (g, fileName, tx, top + 35.0f, tw, Theme::bold (15.0f), Theme::text);

    const double len = shownLength();
    juce::String sub = len > 0.0 ? formatTime (len) : juce::String();
    if (v == View::Done)
    {
        sub << (sub.isNotEmpty() ? dot : juce::String()) << opLabel (shownOp);
        if (shown.hasRegion()) sub << dot << "region " << formatTime (shown.regionStart) << "-" << formatTime (shown.regionEnd);
    }
    else if (v == View::Running) sub << (sub.isNotEmpty() ? dot : juce::String()) << opLabel (audioProcessor.getModelRunner().getOperation());
    else if (pendingChannels > 0) sub << (sub.isNotEmpty() ? dot : juce::String()) << (pendingChannels == 1 ? "mono" : "stereo");
    drawLine (g, sub, tx, top + 53.0f, tw, Theme::regular (12.0f), Theme::textMuted);

    if (v == View::Running)
    {
        const auto a = audioProcessor.getModelRunner().getAnalysis();
        paintAnalysisCells (g, a, a.valid);
    }
    else if (v == View::Done && ! analysisDone)
        paintAnalysisCells (g, shownAnalysis, shownAnalysis.valid);
    else if (v == View::Ready && errorText.isEmpty())
        paintAnalysisCells (g, pendingAnalysis, pendingAnalysis.valid);

    const juce::Rectangle<float> wave ((float) kTL, top + 12.0f, (float) kTW, 112.0f);
    if (v == View::Ready && errorText.isNotEmpty())
    {
        drawWrapped (g, errorText, wave.getX(), wave.getCentreY() - 9.0f, wave.getWidth(), 18.0f, Theme::regular (13.5f), Theme::warning);
    }
    else if (mixPeaks.values.empty())
    {
        g.setColour (Theme::line);
        g.fillRect (wave.getX(), wave.getCentreY(), wave.getWidth(), 1.0f);
    }
    else
    {
        juce::Path p;
        const float ref = mixPeaks.peak > 0.0f ? mixPeaks.peak : 1.0f;
        if (v == View::Running)
        {
            // the part the engine has finished is drawn in cream: windows run front to back, so finished windows / windows is
            // also how far through the song it is (0 until the engine reports its first window)
            const float done = (float) windowFraction;
            envelopePath (p, mixPeaks.values, wave, ref, false, done, 1.0f);
            g.setColour (Theme::surfaceHigh);
            g.fillPath (p);
            if (done > 0.0f)
            {
                envelopePath (p, mixPeaks.values, wave, ref, false, 0.0f, done);
                g.setColour (Theme::text);
                g.fillPath (p);
                if (done < 1.0f)
                {
                    g.setColour (Theme::teal);
                    g.fillRect (wave.getX() + wave.getWidth() * done - 1.0f, wave.getY() - 4.0f, 2.0f, wave.getHeight() + 8.0f);
                }
            }
        }
        else
        {
            envelopePath (p, mixPeaks.values, wave, ref, false);
            g.setColour (Theme::text);
            g.fillPath (p);
        }
    }

    // ruler: five ticks over the real length
    for (int i = 0; i < 5 && len > 0.0; ++i)
    {
        const float x = (float) kTL + (float) kTW * (float) i / 4.0f;
        g.setColour (Theme::line);
        g.fillRect (x, wave.getBottom() + 6.0f, 1.0f, 6.0f);
        const auto label = formatTime (len * i / 4.0);
        const auto f = Theme::regular (11.0f);
        const float w = textWidth (f, label) + 2.0f;
        const float lx = i == 0 ? x + 5.0f : i == 4 ? x - w : x - w * 0.5f;      // the first label clears the playhead at 0:00
        drawLine (g, label, lx, wave.getBottom() + 21.0f, w + 2.0f, f, Theme::textMuted);
    }

    if (v == View::Ready && len > 0.0 && errorText.isEmpty())
    {
        // the preview's playhead, drawn BEFORE the markers: where they meet (at 0:00 on load) the gold marker shows, not two lines
        if (audioProcessor.getTotalLength() > 0.0)
        {
            const float px = playheadX();
            g.setColour (Theme::ink);
            g.fillRect (px - 1.75f, wave.getY() - 4.0f, 3.5f, wave.getHeight() + 8.0f);
            g.setColour (Theme::text);
            g.fillRect (px - 0.75f, wave.getY() - 4.0f, 1.5f, wave.getHeight() + 8.0f);
        }
        if (hasRegion())
        {
            const float x0 = (float) kTL + (float) kTW * (float) (regionA / len), x1 = (float) kTL + (float) kTW * (float) (regionB / len);
            g.setColour (Theme::surface.withAlpha (0.62f));                    // the part that is NOT separated, dimmed
            g.fillRect (wave.getX(), wave.getY(), x0 - wave.getX(), wave.getHeight());
            g.fillRect (x1, wave.getY(), wave.getRight() - x1, wave.getHeight());
            g.setColour (Theme::gold);
            g.fillRect (x0 - 1.0f, wave.getY() - 4.0f, 2.0f, wave.getHeight() + 8.0f);
            g.fillRect (x1 - 1.0f, wave.getY() - 4.0f, 2.0f, wave.getHeight() + 8.0f);
            // handles on the edges: they can be taken hold of and moved
            g.fillRoundedRectangle (x0 - 4.0f, wave.getY() - 6.0f, 8.0f, 10.0f, 2.0f);
            g.fillRoundedRectangle (x1 - 4.0f, wave.getY() - 6.0f, 8.0f, 10.0f, 2.0f);
            const double secs = regionB - regionA;
            if (regionIsWholeFile())
                drawLine (g, "Whole track" + dot + "drag a marker to pick a part", tx, top + 106.0f, tw, Theme::regular (11.5f), Theme::textMuted);
            else
                drawLine (g, "Region " + formatTime (regionA) + " - " + formatTime (regionB) + dot
                         + (secs >= 60.0 ? formatTime (secs) + " min" : juce::String (secs, 1) + " s"), tx, top + 106.0f, tw, Theme::bold (11.5f), Theme::gold);
        }
        else
            drawLine (g, "Click to listen from there, drag to pick a part", tx, top + 106.0f, tw, Theme::regular (11.5f), Theme::textMuted);
    }

    // the clock, one line beside play and stop: position bold, length muted
    if ((v == View::Done && shownOp != Operation::Analysis) || (v == View::Ready && errorText.isEmpty() && audioProcessor.getTotalLength() > 0.0))
    {
        const auto now = formatTime (audioProcessor.getPlaybackPosition());
        const auto fb = Theme::bold (14.0f);
        drawLine (g, now, 122.0f, top + 133.0f, 80.0f, fb, Theme::text);
        drawLine (g, " / " + formatTime (len), 122.0f + textWidth (fb, now), top + 133.0f, 80.0f, Theme::regular (12.5f), Theme::textMuted);
    }
}


bool SubverseSplitterAudioProcessorEditor::drumModelInstalled() const
{
    // the same places the engine looks (PluginProcessor.cpp elekFindModel / elekDrumSepModel), so the window never promises
    // drum parts the engine will skip
    if (const char* o = std::getenv ("ELEK_TEST_DRUMSEP_MODEL"); o != nullptr && *o != 0)
        return juce::File (juce::String::fromUTF8 (o)).existsAsFile();
    const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
    for (const auto& f : { exe.getParentDirectory().getChildFile ("drumsep.onnx"),
                           exe.getParentDirectory().getParentDirectory().getChildFile ("Resources").getChildFile ("drumsep.onnx"),
                           exe.getParentDirectory().getParentDirectory().getParentDirectory().getChildFile ("drumsep.onnx") })
        if (f.existsAsFile()) return true;
    return false;
}



void SubverseSplitterAudioProcessorEditor::paintSpineAndPlayhead (juce::Graphics& g)
{
    const auto v = view();
    if (lanesBottom <= 0 || v != View::Done || panels.empty()) return;
    // the cream spine: the mix on the left, the stems coming out of it (the logo's E)
    g.setColour (Theme::text);
    g.fillRoundedRectangle ((float) kPad, (float) kCardTop, 5.0f, (float) (lanesBottom - kCardTop), 2.0f);

    // the drum group rail in the gaps between part lanes (the lanes draw their own stretch)
    if (drumGroupOpen && ! drumPanels.empty() && drumPanels.front()->isVisible())
    {
        g.setColour (Theme::stemDrums);
        g.fillRect (48.0f, (float) drumPanels.front()->getY(), 2.0f, (float) (drumPanels.back()->getBottom() - drumPanels.front()->getY()));
    }
    // one playhead through the overview and every lane (the lanes draw it over their own surface too): cream with a dark
    // edge, so it shows on the cream mix and on every stem colour (gold is kept for actions)
    const float x = playheadX(), y0 = (float) kCardTop + 8.0f;
    g.setColour (Theme::ink);
    g.fillRect (x - 1.75f, y0, 3.5f, (float) lanesBottom - y0);
    g.setColour (Theme::text);
    g.fillRect (x - 0.75f, y0, 1.5f, (float) lanesBottom - y0);
}

void SubverseSplitterAudioProcessorEditor::paintInspector (juce::Graphics& g)
{
    switch (view())
    {
        case View::Empty:
        case View::Ready:
            paintOptions (g);
            break;
        case View::Running:
            paintWorking (g);
            break;
        case View::History:
            break;
        case View::Done:
            if (shownOp != Operation::Analysis) { paintFiles (g); break; }
            {
                // Key & BPM: the reading is the result, so it is the largest thing on the page
                const auto& a = shownAnalysis;
                const bool sure = ElekLibrary::keyIsSure (a);
                const juce::String values[4] = {
                    a.valid ? juce::String (a.bpm, 1) + " BPM" : emDash,
                    a.valid ? a.key + " " + (a.scale == "min" ? "minor" : "major") : emDash,
                    a.valid ? a.camelot : emDash,
                    a.valid ? juce::String (juce::roundToInt (a.keyConfidence * 100.0f)) + " %" : emDash };
                const char* labels[4] = { "Tempo", "Key", "Camelot", "Key confidence" };
                for (int i = 0; i < 4; ++i)
                {
                    const float x = 40.0f + (float) i * 230.0f, y = (float) kLanesTop + 30.0f;
                    caption (g, labels[i], x, y);
                    drawLine (g, values[i], x, y + 34.0f, 220.0f, Theme::bold (28.0f), i == 0 || sure ? Theme::text : Theme::textMuted);
                }
                drawWrapped (g, sure ? "Tempo and key measured on the whole mix. Nothing was separated."
                                     : "Tempo measured on the whole mix. The key reading is uncertain: treat it as a guess. Nothing was separated.",
                             40.0f, (float) kLanesTop + 110.0f, 700.0f, 18.0f, Theme::regular (13.5f), Theme::textMuted);
            }
            break;
    }
}

void SubverseSplitterAudioProcessorEditor::paintAnalysisCells (juce::Graphics& g, const AudioAnalysis& a, bool known)
{
    // one line in the card, under the name: tempo, key, Camelot. A key the reading is not sure of (confidence under 0.5, the
    // same rule that keeps it out of file names) is drawn muted and says so, instead of standing as large as the tempo.
    const float x = 40.0f, cy = (float) kCardTop + 73.0f, maxW = (float) (kTL - 40 - 14);
    if (! known)
    {
        const bool measuring = view() == View::Running || autoAnalysis || needAnalysis;
        drawLine (g, measuring ? "Measuring tempo and key" : "Tempo and key not measured", x, cy, maxW, Theme::regular (12.0f), Theme::textMuted);
        return;
    }
    const bool sure = ElekLibrary::keyIsSure (a);
    const auto fb = Theme::bold (13.5f);
    const juce::String bpm = juce::String (a.bpm, 1) + " BPM";
    drawLine (g, bpm, x, cy, maxW, fb, Theme::text);
    const float used = textWidth (fb, bpm);
    const juce::String key = dot + a.key + (a.scale == "min" ? " minor" : " major") + dot + a.camelot;
    drawLine (g, key, x + used, cy, maxW - used, sure ? fb : Theme::regular (13.5f), sure ? Theme::text : Theme::textMuted);
    if (! sure)
        drawLine (g, "Key uncertain: " + juce::String (juce::roundToInt (a.keyConfidence * 100.0f)) + " % confidence", x, cy + 16.0f, maxW,
                  Theme::regular (11.5f), Theme::textMuted);
}

void SubverseSplitterAudioProcessorEditor::paintOptions (juce::Graphics& g)
{
    // four tiles: each operation as a tool of its own — what it makes as glyphs (the lanes' colours and order), its name, one line
    caption (g, "What to make", (float) kPad, (float) optionBounds (0).getY() - 14.0f);
    const bool drumModel = drumModelInstalled();
    for (int i = 0; i < kNumOptions; ++i)
    {
        const auto r = optionBounds (i).toFloat();
        const auto op = opFor (i);
        const bool missing = op == Operation::DrumsOnly && ! drumModel;
        const bool sel = selected == i, hot = hoverOption == i && ! missing;
        g.setColour (sel || hot ? Theme::surfaceRaised : Theme::surface);
        g.fillRoundedRectangle (r, Theme::radius);
        g.setColour (sel ? Theme::gold : hot ? Theme::textMuted.withAlpha (0.6f) : Theme::line);
        g.drawRoundedRectangle (r.reduced (sel ? 1.0f : 0.5f), Theme::radius, sel ? 2.0f : 1.0f);

        // owner 20 Nov chose look 2 of docs/ui_2026-11-20/tile_styles: the name large at the top, then what the tile makes, one
        // row per output with its lane colour (the order of the result's lanes)
        struct Out { const char* name; juce::Colour colour; };
        std::vector<Out> outs;
        if (op == Operation::VocalRemoval)   outs = { { "Vocals", Theme::stemVocals }, { "Instrumental", Theme::stemInstrumental } };
        else if (op == Operation::DrumsOnly) outs = { { "Kick", Theme::stemDrums }, { "Snare", Theme::stemDrums }, { "Hats & cymbals", Theme::stemDrums } };      // owner 20 Nov: no toms
        else
        {
            outs = { { "Vocals", Theme::stemVocals }, { "Drums", Theme::stemDrums }, { "Bass", Theme::stemBass } };
            if (op == Operation::Stems6) { outs.push_back ({ "Guitar", Theme::stemGuitar }); outs.push_back ({ "Piano", Theme::stemPiano }); }
            outs.push_back ({ "Other", Theme::stemOther });
        }
        drawLine (g, kOptions[i].label, r.getX() + 22.0f, r.getY() + 40.0f, r.getWidth() - 44.0f, Theme::bold (24.0f), missing ? Theme::textMuted : Theme::text);
        g.setColour (Theme::line);
        g.fillRect (r.getX() + 22.0f, r.getY() + 66.0f, r.getWidth() - 44.0f, 1.0f);
        if (missing)
        {
            drawWrapped (g, "The drum model is not installed.", r.getX() + 22.0f, r.getY() + 92.0f, r.getWidth() - 44.0f, 21.0f, Theme::regular (14.5f), Theme::textMuted);
            continue;
        }
        float y = r.getY() + 92.0f;
        for (auto& o : outs)
        {
            g.setColour (o.colour);
            g.fillRoundedRectangle (r.getX() + 22.0f, y - 8.0f, 4.0f, 16.0f, 2.0f);
            drawLine (g, o.name, r.getX() + 36.0f, y, r.getWidth() - 58.0f, Theme::bold (15.0f), Theme::text);
            y += 29.0f;
        }
    }
}

void SubverseSplitterAudioProcessorEditor::paintRecent (juce::Graphics& g, int top)
{
    juce::ignoreUnused (top);
    for (int i = 0; i < (int) recentRuns.size(); ++i)
    {
        const auto row = recentRow (i).toFloat();
        if (row.isEmpty()) break;
        const auto& e = recentRuns[(size_t) i];
        g.setColour (hoverRecent == i ? Theme::surfaceRaised : Theme::surface);
        g.fillRoundedRectangle (row, Theme::radiusSm);
        drawLine (g, e.title, row.getX() + 14.0f, row.getY() + 19.0f, row.getWidth() - 28.0f, Theme::bold (13.5f), Theme::text);
        juce::String tags = e.operation == "stem_separation_6" ? "6 stems" : e.operation == "stem_separation" ? "4 stems"
                          : e.operation == "drums_only" ? "Drums only" : "Vocal removal";
        if (e.analysis.valid) tags << dot << juce::String (e.analysis.bpm, 1) << " BPM";
        if (ElekLibrary::keyIsSure (e.analysis)) tags << dot << e.analysis.key << (e.analysis.scale == "min" ? " minor" : " major");
        tags << dot << e.created.formatted ("%d %b %Y");
        drawLine (g, tags, row.getX() + 14.0f, row.getY() + 39.0f, row.getWidth() - 28.0f, Theme::regular (12.0f), Theme::textMuted);
    }
}

void SubverseSplitterAudioProcessorEditor::paintHistory (juce::Graphics& g)
{
    // its own screen: every earlier run whose files are still there, newest first; a click opens it; Home is in the header
    caption (g, "History", (float) kPad, (float) kCardTop + 14.0f);
    if (recentRuns.empty())
    {
        drawLine (g, "Nothing separated yet. Results are saved to Music/ELEK and listed here.", (float) kPad, (float) kCardTop + 52.0f, 800.0f,
                  Theme::regular (13.5f), Theme::textMuted);
        return;
    }
    paintRecent (g, 0);
}

void SubverseSplitterAudioProcessorEditor::paintWorking (juce::Graphics& g)
{
    // What the engine reports, nothing more: ModelRunner::getProgress() (see the member comment for its stages). Steps are
    // read off that figure; a step without a measurable fraction shows no bar.
    const float x = 40.0f, iw = 400.0f;
    auto& runner = audioProcessor.getModelRunner();
    const auto op = runner.getOperation();
    caption (g, "Working", x, (float) kLanesTop + 24.0f);

    struct Step { juce::String label; int state; double fraction; };   // state 0 waiting, 1 active, 2 done; fraction < 0: no bar
    std::vector<Step> steps;
    if (op == Operation::Analysis)
        steps.push_back ({ "Measuring tempo and key", 1, -1.0 });
    else
    {
        const bool separating = windowsReporting;
        const bool separated = windowsReporting && windowFraction >= 1.0 - 1.0e-9;
        steps.push_back ({ "Reading the file, tempo and key, loading the model", separating ? 2 : 1, -1.0 });
        const double splitFrac = rawProgress > 0.9 + 1.0e-9 ? juce::jlimit (0.0, 1.0, (rawProgress - 0.9) / 0.09) : -1.0;
        if (op == Operation::DrumParts)
        {
            // the drums file goes straight to the parts: no main model; the engine reports the split's windows as 0.1 .. 0.99
            steps.clear();
            const bool splitting = rawProgress > 0.1 + 1.0e-9;
            steps.push_back ({ "Reading the drums, loading the drum model", splitting ? 2 : 1, -1.0 });
            steps.push_back ({ "Splitting the drums", splitting ? 1 : 0, splitting ? juce::jlimit (0.0, 1.0, (rawProgress - 0.1) / 0.89) : -1.0 });
        }
        else
        {
            const juce::String sep = op == Operation::VocalRemoval ? "Removing the vocal" : op == Operation::Stems6 ? "Separating six stems"
                                   : op == Operation::DrumsOnly ? "Separating the drums" : "Separating four stems";
            steps.push_back ({ sep, separated ? 2 : separating ? 1 : 0, separating && ! separated ? windowFraction : -1.0 });
            const bool split = op == Operation::DrumsOnly;      // the drum split is part of no other run (owner 18 Nov)
            steps.push_back ({ split ? "Writing the drums, splitting them into parts" : "Writing the files", separated ? 1 : 0, separated && split ? splitFrac : -1.0 });
        }
    }

    float y = (float) kLanesTop + 52.0f;
    for (auto& s : steps)
    {
        if (s.state == 2)      { g.setColour (Theme::teal); g.fillEllipse (x, y - 5.0f, 10.0f, 10.0f); }
        else if (s.state == 1) { g.setColour (Theme::text); g.fillEllipse (x, y - 5.0f, 10.0f, 10.0f); }
        else                   { g.setColour (Theme::textMuted); g.drawEllipse (x + 0.75f, y - 4.25f, 8.5f, 8.5f, 1.5f); }
        const auto f = s.state == 1 ? Theme::bold (13.0f) : Theme::regular (13.0f);
        juce::String pct;
        if (s.fraction >= 0.0) pct = juce::String (juce::roundToInt (s.fraction * 100.0)) + " %";
        const float pctW = pct.isEmpty() ? 0.0f : textWidth (Theme::regular (11.5f), pct) + 8.0f;
        drawLine (g, s.label, x + 18.0f, y, iw - 18.0f - pctW, f, s.state == 0 ? Theme::textMuted : Theme::text);
        if (pct.isNotEmpty())
            drawLine (g, pct, x + iw - pctW, y, pctW, Theme::regular (11.5f), Theme::textMuted, juce::Justification::centredRight);
        y += 16.0f;
        if (s.state == 1 && s.fraction >= 0.0)
        {
            g.setColour (Theme::surfaceHigh);
            g.fillRoundedRectangle (x + 18.0f, y - 2.5f, iw - 18.0f, 5.0f, 2.5f);
            g.setColour (Theme::teal);
            g.fillRoundedRectangle (x + 18.0f, y - 2.5f, (iw - 18.0f) * (float) s.fraction, 5.0f, 2.5f);
            y += 12.0f;
        }
        y += 14.0f;
    }
    drawWrapped (g, "The result appears here when the run ends. Nothing is uploaded.", x, y + 14.0f, iw, 18.0f,
                 Theme::regular (12.5f), Theme::textMuted);
}

void SubverseSplitterAudioProcessorEditor::paintFiles (juce::Graphics& g)
{
    // the action bar above the footer: where the files are, on the left of the three keys
    const float x = (float) kPad, y0 = (float) (getHeight() - kFooter - kBarH), maxW = (float) (showFilesButton.getX() - kPad - 16);
    const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    const juce::String where = shown.folder.isAChildOf (home) ? shown.folder.getRelativePathFrom (home) : shown.folder.getFullPathName();
    const auto f = Theme::regular (12.5f);
    drawLine (g, fitFromEnd (where, f, maxW), x, y0 + 12.0f, maxW, f, Theme::text);
    int wavs = 0;
    for (auto& k : shown.files.getAllKeys())
        if (shown.file (k).existsAsFile() && shown.file (k).hasFileExtension ("wav")) ++wavs;
    drawLine (g, juce::String (wavs) + " WAV files" + dot + "drag a lane's WAV tab into your DAW, or click it to show the file", x, y0 + 30.0f, maxW,
              Theme::regular (12.0f), Theme::textMuted);
}

void SubverseSplitterAudioProcessorEditor::DrumGroupHeader::paint (juce::Graphics& g)
{
    // transparent strip on the ground (the playhead and rail show through)
    const float cy = (float) getHeight() * 0.5f, cx = 10.0f;
    juce::Path chev;
    if (open) chev.addTriangle (cx - 5.0f, cy - 3.0f, cx + 5.0f, cy - 3.0f, cx, cy + 3.5f);
    else      chev.addTriangle (cx - 3.0f, cy - 5.0f, cx - 3.0f, cy + 5.0f, cx + 3.5f, cy);
    g.setColour (isMouseOver() ? Theme::text : Theme::textMuted);
    g.fillPath (chev);
    g.setColour (Theme::text);
    Theme::drawTracked (g, "DRUM PARTS", { 22.0f, cy - 8.0f, 200.0f, 16.0f }, Theme::bold (11.0f), 1.0f, juce::Justification::centredLeft);
    drawLine (g, state, (float) (kTL - kLaneX), cy, (float) (kTW - rightInset), Theme::regular (12.0f), Theme::textMuted, juce::Justification::centredRight);
}

void SubverseSplitterAudioProcessorEditor::PlayButton::paintButton (juce::Graphics& g, bool over, bool)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (over ? Theme::goldLight : Theme::text);
    g.fillEllipse (r);
    g.setColour (Theme::ink);
    const float s = r.getWidth();
    const auto c = r.getCentre();
    if (playing)
    {
        g.fillRoundedRectangle (c.x - s * 0.17f, c.y - s * 0.19f, s * 0.12f, s * 0.38f, 1.0f);
        g.fillRoundedRectangle (c.x + s * 0.05f, c.y - s * 0.19f, s * 0.12f, s * 0.38f, 1.0f);
    }
    else
    {
        juce::Path tri;
        const float cx = c.x + s * 0.05f;
        tri.addTriangle (cx - s * 0.14f, c.y - s * 0.19f, cx - s * 0.14f, c.y + s * 0.19f, cx + s * 0.2f, c.y);
        g.fillPath (tri);
    }
}

void SubverseSplitterAudioProcessorEditor::StopButton::paintButton (juce::Graphics& g, bool over, bool)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (over ? Theme::goldLight : Theme::text);
    g.drawEllipse (r, 1.5f);
    const float s = r.getWidth() * 0.3f;
    g.fillRoundedRectangle (r.getCentreX() - s * 0.5f, r.getCentreY() - s * 0.5f, s, s, 1.5f);
}

void SubverseSplitterAudioProcessorEditor::stopPlayback()
{
    if (audioProcessor.getIsPlaying()) audioProcessor.togglePlayPause();
    audioProcessor.setPlaybackPosition (view() == View::Ready && hasRegion() ? regionA : 0.0);
    lastPlayheadX = -1.0f;
    repaint();
}

// ---------------------------------------------------------------- input

void SubverseSplitterAudioProcessorEditor::chooseFile()
{
    fileChooser = std::make_unique<juce::FileChooser> ("Select Audio File", juce::File::getSpecialLocation (juce::File::userHomeDirectory),
                                                       "*.wav;*.mp3;*.aiff;*.aif;*.flac");
    auto chooserFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;
    fileChooser->launchAsync (chooserFlags, [this] (const juce::FileChooser& fc) {
        auto result = fc.getResult();
        if (result.existsAsFile())
            loadFile (result);
    });
}

bool SubverseSplitterAudioProcessorEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    if (audioProcessor.getModelRunner().isRunning()) return false;
    for (auto& file : files)
    {
        if (file.contains ("_vocals") || file.contains ("_drums") || file.contains ("_bass") || file.contains ("_other")) return false;
        auto ext = juce::File(file).getFileExtension().toLowerCase();
        if (ext == ".wav" || ext == ".mp3" || ext == ".aif" || ext == ".aiff" || ext == ".flac") return true;
    }
    return false;
}

void SubverseSplitterAudioProcessorEditor::fileDragEnter (const juce::StringArray&, int, int) { isDraggingActive = true; repaint(); }
void SubverseSplitterAudioProcessorEditor::fileDragMove (const juce::StringArray&, int, int) {}
void SubverseSplitterAudioProcessorEditor::fileDragExit (const juce::StringArray&) { isDraggingActive = false; repaint(); }

void SubverseSplitterAudioProcessorEditor::filesDropped (const juce::StringArray& files, int, int)
{
    // loads only: what happens to the file is chosen in What to make
    isDraggingActive = false;
    if (files.size() > 0)
        loadFile (juce::File (files[0]));
    repaint();
}

void SubverseSplitterAudioProcessorEditor::mouseMove (const juce::MouseEvent& e)
{
    int h = -1, r = -1;
    const auto v = view();
    if (v == View::Empty || v == View::Ready)
    {
        for (int i = 0; i < kNumOptions; ++i)
            if (optionBounds (i).contains (e.getPosition())) h = i;
    }
    if (v == View::History)
        for (int i = 0; i < (int) recentRuns.size(); ++i)
            if (! recentRow (i).isEmpty() && recentRow (i).contains (e.getPosition())) r = i;
    if (h != hoverOption || r != hoverRecent) { hoverOption = h; hoverRecent = r; repaint(); }
    const bool overZone = v == View::Empty && dropZone().contains (e.getPosition());
    const bool overSeek = v == View::Done && ! panels.empty() && timelineBounds().contains (e.getPosition());
    const bool overRegion = v == View::Ready && errorText.isEmpty() && timelineBounds().contains (e.getPosition());
    setMouseCursor (overRegion ? (regionEdgeAt (e.x) != 0 ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::CrosshairCursor)
                  : overZone || h >= 0 || r >= 0 || overSeek ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
}

void SubverseSplitterAudioProcessorEditor::mouseExit (const juce::MouseEvent&)
{
    if (hoverOption >= 0 || hoverRecent >= 0) { hoverOption = hoverRecent = -1; repaint(); }
}

double SubverseSplitterAudioProcessorEditor::secondsAtX (int x) const
{
    const auto track = timelineBounds();
    const double len = shownLength();
    return track.getWidth() > 0 ? len * juce::jlimit (0.0, 1.0, (double) (x - track.getX()) / track.getWidth()) : 0.0;
}

void SubverseSplitterAudioProcessorEditor::mouseDown (const juce::MouseEvent& e)
{
    const auto v = view();
    // The loaded file's waveform: a click moves the preview there, a drag picks the part to separate, a drag that starts on
    // an edge of the region moves that edge, a double-click clears the region. A click never touches the region (18 Nov: it
    // used to wipe it, "the markers are not kept").
    if (v == View::Ready && errorText.isEmpty() && shownLength() > 0.0 && timelineBounds().contains (e.getPosition()))
    {
        if (e.getNumberOfClicks() >= 2) { setWholeFileRegion(); regionPress = false; repaint(); return; }      // markers back to start and end
        regionKeptA = regionA; regionKeptB = regionB;
        if (const int edge = regionEdgeAt (e.x); edge != 0)
        {
            regionDragging = true;                          // the other edge stays where it is
            regionAnchor = edge == 1 ? regionB : regionA;
            regionEdgeDragged = edge;
            return;
        }
        regionPress = true;
        regionPressX = e.x;
        return;
    }
    if (v == View::Empty || v == View::Ready)
    {
        // the whole drop zone opens the file chooser
        if (v == View::Empty && dropZone().contains (e.getPosition())) { chooseFile(); return; }
        for (int i = 0; i < kNumOptions; ++i)
            if (optionBounds (i).contains (e.getPosition())) { selectOperation (i); return; }
        return;
    }
    if (v == View::History)
    {
        for (int i = 0; i < (int) recentRuns.size(); ++i)
        {
            const auto row = recentRow (i);
            if (row.isEmpty()) break;
            if (row.contains (e.getPosition())) { openRecent (i); return; }
        }
        return;
    }
    if (v == View::Done)
        seekFromMouse (e.getPosition(), true);
}

void SubverseSplitterAudioProcessorEditor::mouseUp (const juce::MouseEvent&)
{
    if (regionPress)                 // a click: the preview goes there; the region is not touched
    {
        regionPress = false;
        if (previewReady())
        {
            audioProcessor.setPlaybackPosition (secondsAtX (regionPressX));
            repaint (overviewCard());
        }
        return;
    }
    if (! regionDragging) return;
    regionDragging = false;
    if (! hasRegion()) { regionA = regionKeptA; regionB = regionKeptB; }      // under a second: the region from before the gesture stays
    else if (previewReady() && (regionA != regionKeptA || regionB != regionKeptB))
    {
        // owner 20 Nov: after an edge is moved, the preview goes straight to it (no waiting for the old end): a new region or
        // a moved start plays from the start, a moved end plays its last 3 seconds; playing or paused, it continues as it was
        const double to = regionEdgeDragged == 2 ? juce::jmax (regionA, regionB - 3.0) : regionA;
        audioProcessor.setPlaybackPosition (to);
    }
    repaint();
}

void SubverseSplitterAudioProcessorEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (regionPress && std::abs (e.x - regionPressX) > 4)        // it is a drag: a new region from where it was pressed
    {
        regionPress = false;
        regionDragging = true;
        regionAnchor = secondsAtX (regionPressX);
        regionEdgeDragged = 0;
    }
    if (regionPress) return;
    if (regionDragging)
    {
        const double now = secondsAtX (e.x);
        regionA = juce::jmin (regionAnchor, now);
        regionB = juce::jmax (regionAnchor, now);
        repaint (overviewCard());
        return;
    }
    // once the gesture started on the overview, following the pointer off it is what a scrub is
    if (view() == View::Done && timelineBounds().contains (e.mouseDownPosition.toInt()))
        seekFromMouse (e.getPosition(), false);
}

void SubverseSplitterAudioProcessorEditor::seekFromMouse (juce::Point<int> position, bool startedOnBar)
{
    const auto track = timelineBounds();
    if (panels.empty() || track.getWidth() <= 0) return;
    if (startedOnBar && ! track.contains (position)) return;

    const double length = audioProcessor.getTotalLength();
    if (length <= 0.0) return;

    audioProcessor.setPlaybackPosition (length * juce::jlimit (0.0, 1.0, (double) (position.x - track.getX()) / track.getWidth()));
    repaintPlayhead();
}

void SubverseSplitterAudioProcessorEditor::timerCallback()
{
    // diagnostics (20 Nov): once a second while playing, what actually left processBlock
    if (audioProcessor.getIsPlaying() && juce::Time::getMillisecondCounter() - lastPeakLogMs >= 1000)
    {
        lastPeakLogMs = juce::Time::getMillisecondCounter();
        const float pk = audioProcessor.takeOutputPeak();
        logMessage ("PLAYING at " + juce::String (audioProcessor.getPlaybackPosition(), 2) + " s, output peak " + juce::String (juce::Decibels::gainToDecibels (pk, -120.0f), 1) + " dBFS");
    }
    if (playPauseButton.playing != audioProcessor.getIsPlaying())
    {
        playPauseButton.playing = audioProcessor.getIsPlaying();
        playPauseButton.repaint();
    }
    if (view() == View::Done && ! panels.empty() && std::abs (playheadX() - lastPlayheadX) >= 0.5f)
        repaintPlayhead();
    // the end of the track (owner 20 Nov): Play turns off and the playhead goes back to the start, so one press plays again
    if (view() == View::Done && audioProcessor.playbackReachedEnd())
    {
        audioProcessor.togglePlayPause();
        audioProcessor.setPlaybackPosition (0.0);
        repaintPlayhead();
    }
    if (view() == View::Ready && audioProcessor.getTotalLength() > 0.0)
    {
        // the preview: with a region, Play stops at its end and goes back to its start
        if (audioProcessor.getIsPlaying() && hasRegion() && ! regionDragging
            && (audioProcessor.getPlaybackPosition() >= regionB - 0.01 || audioProcessor.playbackReachedEnd()))
        {
            audioProcessor.togglePlayPause();
            audioProcessor.setPlaybackPosition (regionA);
        }
        if (std::abs (playheadX() - lastPlayheadX) >= 0.5f) { lastPlayheadX = playheadX(); repaint (overviewCard()); }
    }

    auto& runner = audioProcessor.getModelRunner();

    // tempo and key of a loaded file: the engine's Analysis run, started when the engine is free, never shown as a run
    if (autoAnalysis && ! runner.isRunning())
    {
        autoAnalysis = false;
        if (runner.getInputFile() == pendingFile) pendingAnalysis = runner.getAnalysis();
        else needAnalysis = pendingFile != juce::File();        // the file changed while it was being read: read the new one
        resized();
        repaint();
    }
    if (needAnalysis && ! runner.isRunning() && pendingFile != juce::File() && ! resultShown && ! historyShown && ! awaitingResult)
    {
        needAnalysis = false;
        autoAnalysis = true;
        runner.startOperation (pendingFile, Operation::Analysis);
        resized();
        repaint();
    }
    const bool running = runner.isRunning() && ! autoAnalysis;

    if (running)
    {
        if (! awaitingResult) { rawProgress = 0.0; windowsReporting = false; windowFraction = 0.0; resized(); repaint(); }
        awaitingResult = true;
        if (runner.getInputFile() != mixPeaksFile) { fileName = runner.getInputFile().getFileName(); loadMixPeaksAsync (runner.getInputFile()); }

        // ModelRunner holds 0.1 until the first window is done, then 0.1 + 0.8 * done / windows
        const double raw = runner.getProgress();
        bool changed = raw != rawProgress;
        if (! windowsReporting && raw > 0.1 + 1.0e-9)
            windowsReporting = true;
        if (windowsReporting)
        {
            const double f = juce::jlimit (0.0, 1.0, (raw - 0.1) / 0.8);
            if (f > windowFraction) { windowFraction = f; changed = true; }
        }
        rawProgress = raw;
        const bool analysisValid = runner.getAnalysis().valid;
        if (changed || analysisValid != lastAnalysisValid) repaint();
        lastAnalysisValid = analysisValid;
    }
    else if (awaitingResult)
    {
        awaitingResult = false;
        // a run just ended: show its result, or put the error back on the options
        if (runner.isFinishedSuccessfully())
        {
            showRunnerResult (true);
            refreshRecent();
        }
        else if (runner.getOperation() == Operation::DrumParts && shown.folder.isDirectory())
        {
            // the split failed or was cancelled: the run it was started from comes back; a failure's reason goes on the strip
            const auto reason = runner.getErrorMessage().isNotEmpty() ? runner.getErrorMessage() : juce::String ("The drums could not be split.");
            const auto entry = shown;
            buildResult (entry, true);
            if (! cancelRequested) { drumHeader.state = reason; drumHeader.repaint(); }
        }
        else
        {
            // back to the tiles with the same file; a cancelled run leaves no error line
            errorText = cancelRequested ? juce::String()
                      : runner.getErrorMessage().isNotEmpty() ? runner.getErrorMessage() : juce::String ("The operation failed.");
            pendingFile = runner.getInputFile();
            fileName = pendingFile.getFileName();
            preparePreview();
        }
        cancelRequested = false;
        resized();
        repaint();

        if (autoPlayWhenReady && ! panels.empty())
        {
            autoPlayWhenReady = false;
            audioProcessor.togglePlayPause();
        }
    }
}

// ==============================================================================
// LANE (StemPanel)
// ==============================================================================

namespace LaneLayout
{
    // lane coordinates (0 = the lane's left edge, kLaneX in the window); from the mockup's window x minus 34
    constexpr int mute = 122, solo = 146, toggle = 20;
    constexpr int sliderL = 178, sliderR = 216, thumb = 6;
    constexpr int wavX = Editor::kTL + Editor::kTW + 8 - Editor::kLaneX, wavW = 56, wavH = 24;
    constexpr float nameMain = 102.0f, namePart = 90.0f;
}

SubverseSplitterAudioProcessorEditor::StemPanel::StemPanel (const juce::String& name, const juce::File& file, juce::Colour color, int index,
                                                            SubverseSplitterAudioProcessor& processor, juce::DragAndDropContainer& container)
    : targetFile (file), themeColor (color), stemIndex (index), audioProcessor (processor), dragContainer (container)
{
    setName (name);
    setOpaque (false);

    // M and S: 18 px keys; on = teal fill (mute), gold fill (solo)
    muteButton.getProperties().set ("variant", "toggle");
    muteButton.setClickingTogglesState (true);
    muteButton.setColour (juce::TextButton::textColourOnId, Theme::teal);
    muteButton.onClick = [this] { if (wired()) audioProcessor.setStemMute (stemIndex, muteButton.getToggleState()); repaint(); };
    muteButton.setTooltip ("Mute");
    addAndMakeVisible (muteButton);

    soloButton.getProperties().set ("variant", "toggle");
    soloButton.setClickingTogglesState (true);
    soloButton.setColour (juce::TextButton::textColourOnId, Theme::gold);
    soloButton.onClick = [this] { if (wired()) audioProcessor.setStemSolo (stemIndex, soloButton.getToggleState()); };
    soloButton.setTooltip ("Solo");
    addAndMakeVisible (soloButton);

    // level, separate from mute; the track in the lane's colour
    volumeSlider.setRange (0.0, 1.0);
    volumeSlider.setValue (wired() ? audioProcessor.getStemVolume (stemIndex) : 1.0f, juce::dontSendNotification);
    volumeSlider.setScrollWheelEnabled (false);
    volumeSlider.setColour (juce::Slider::trackColourId, themeColor);
    volumeSlider.setDoubleClickReturnValue (true, 1.0);
    volumeSlider.onValueChange = [this] {
        if (wired()) audioProcessor.setStemVolume (stemIndex, (float) volumeSlider.getValue());
    };
    addAndMakeVisible (volumeSlider);

    // the WAV tab: click shows the WAV in Finder, drag drops it into a DAW
    exportButton.getProperties().set ("variant", "wav");
    exportButton.setTooltip ("Drag into your DAW, or click to show the file");
    exportButton.addMouseListener (this, false);
    addAndMakeVisible (exportButton);

    startTimerHz (10);   // mute / solo follow the processor (solo on one lane changes what the others mean)
}

SubverseSplitterAudioProcessorEditor::StemPanel::~StemPanel() { stopTimer(); }

void SubverseSplitterAudioProcessorEditor::StemPanel::setPeaks (std::vector<float> newPeaks, float newReference)
{
    if (! newPeaks.empty())
    {
        peaks = std::move (newPeaks);
        peak = 0.0f;
        for (auto v : peaks) peak = juce::jmax (peak, v);
    }
    reference = newReference > 0.0f ? newReference : 1.0f;
    pathArea = {};
    repaint();
}

void SubverseSplitterAudioProcessorEditor::StemPanel::setHasData (bool shouldHaveData)
{
    hasData = shouldHaveData;
    const bool on = hasData && wired() && ! silent;
    muteButton.setEnabled (on);
    soloButton.setEnabled (on);
    volumeSlider.setEnabled (on);
    exportButton.setEnabled (hasData);
    setMouseCursor (hasData ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
    repaint();
}

void SubverseSplitterAudioProcessorEditor::StemPanel::setSilent (bool isSilent)
{
    silent = isSilent;
    const bool on = hasData && wired() && ! silent;
    muteButton.setEnabled (on);
    soloButton.setEnabled (on);
    volumeSlider.setEnabled (on);
    repaint();
}

void SubverseSplitterAudioProcessorEditor::StemPanel::timerCallback()
{
    if (! wired()) return;
    const bool m = audioProcessor.getStemMute (stemIndex), s = audioProcessor.getStemSolo (stemIndex);
    if (m != muteButton.getToggleState() || s != soloButton.getToggleState())
    {
        muteButton.setToggleState (m, juce::dontSendNotification);
        soloButton.setToggleState (s, juce::dontSendNotification);
        repaint();
    }
}

juce::Rectangle<float> SubverseSplitterAudioProcessorEditor::StemPanel::waveArea() const
{
    return laneWave (getLocalBounds().toFloat());
}

void SubverseSplitterAudioProcessorEditor::StemPanel::seekTo (int x)
{
    const auto w = waveArea();
    const double length = audioProcessor.getTotalLength();
    if (length > 0.0 && w.getWidth() > 0.0f)
        audioProcessor.setPlaybackPosition (length * juce::jlimit (0.0, 1.0, (double) ((float) x - w.getX()) / w.getWidth()));
}

void SubverseSplitterAudioProcessorEditor::StemPanel::mouseDown (const juce::MouseEvent& e)
{
    dragging = false;
    if (e.originalComponent == this && waveArea().contains (e.position))   // a click on the waveform seeks
        seekTo (e.x);
}

void SubverseSplitterAudioProcessorEditor::StemPanel::mouseDrag (const juce::MouseEvent& e)
{
    // Grabbing the stem itself (its waveform) or its WAV tab and pulling drops the WAV into a DAW track (owner, 14 Oct:
    // "sesi tutup DAW'da bir kanala sürükleme"). A click on the waveform still seeks (mouseDown).
    const bool fromWave = e.originalComponent == this && waveArea().contains (e.mouseDownPosition);
    const bool fromButton = e.originalComponent == &exportButton;
    if ((fromWave || fromButton) && ! dragging && e.getDistanceFromDragStart() > 4
        && ! dragContainer.isDragAndDropActive() && targetFile.existsAsFile())
    {
        dragging = true;
        juce::StringArray files;
        files.add (targetFile.getFullPathName());
        dragContainer.performExternalDragDropOfFiles (files, false, this, [this] { dragging = false; });
    }
}

void SubverseSplitterAudioProcessorEditor::StemPanel::mouseUp (const juce::MouseEvent& e)
{
    if (e.originalComponent == &exportButton && ! dragging && exportButton.getLocalBounds().contains (e.getEventRelativeTo (&exportButton).getPosition())
        && targetFile.existsAsFile())
        targetFile.revealToUser();
    dragging = false;
}

void SubverseSplitterAudioProcessorEditor::StemPanel::paint (juce::Graphics& g)
{
    const bool isMuted = muteButton.getToggleState();
    const auto lane = getLocalBounds().toFloat();
    drawLaneFrame (g, lane, themeColor, getName(), compact, isMuted || ! hasData || silent, rail, compact ? LaneLayout::namePart : LaneLayout::nameMain);

    const auto w = waveArea();
    if (! hasData)
    {
        g.setColour (Theme::line);
        g.fillRect (w.getX(), w.getCentreY(), w.getWidth() * 0.5f - 80.0f, 1.0f);
        g.fillRect (w.getCentreX() + 80.0f, w.getCentreY(), w.getWidth() * 0.5f - 80.0f, 1.0f);
        drawLine (g, "No audio for this part", w.getCentreX() - 80.0f, w.getCentreY(), 160.0f, Theme::regular (12.0f), Theme::textMuted,
                  juce::Justification::centred);
        return;
    }
    if (silent)
    {
        // the file holds (almost) nothing: a dashed line and the word, not a waveform that looks alive
        juce::Path line, dashed;
        line.startNewSubPath (w.getX(), w.getCentreY());
        line.lineTo (w.getCentreX() - 30.0f, w.getCentreY());
        line.startNewSubPath (w.getCentreX() + 30.0f, w.getCentreY());
        line.lineTo (w.getRight(), w.getCentreY());
        const float dashes[] = { 4.0f, 4.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (dashed, line, dashes, 2);
        g.setColour (Theme::line);
        g.fillPath (dashed);
        drawLine (g, "Silent", w.getCentreX() - 30.0f, w.getCentreY(), 60.0f, Theme::regular (12.0f), Theme::textMuted, juce::Justification::centred);
    }
    else if (! peaks.empty())
    {
        if (pathArea != w) { envelopePath (wavePath, peaks, w, reference, true); pathArea = w; }
        g.setColour (isMuted ? themeColor.withAlpha (0.3f) : themeColor);
        g.fillPath (wavePath);
    }

    // the playhead, the same x as the overview's
    const double len = audioProcessor.getTotalLength();
    if (len > 0.0)
    {
        const float x = w.getX() + w.getWidth() * (float) juce::jlimit (0.0, 1.0, audioProcessor.getPlaybackPosition() / len);
        g.setColour (Theme::ink);
        g.fillRect (x - 1.75f, 0.0f, 3.5f, (float) getHeight());
        g.setColour (Theme::text);
        g.fillRect (x - 0.75f, 0.0f, 1.5f, (float) getHeight());
    }
}

void SubverseSplitterAudioProcessorEditor::StemPanel::resized()
{
    using namespace LaneLayout;
    const int cy = getHeight() / 2;
    muteButton.setBounds (mute, cy - toggle / 2, toggle, toggle);
    soloButton.setBounds (solo, cy - toggle / 2, toggle, toggle);
    volumeSlider.setBounds (sliderL - thumb, cy - 9, sliderR - sliderL + 2 * thumb, 18);
    exportButton.setBounds (wavX, cy - wavH / 2, wavW, wavH);
    pathArea = {};
}
