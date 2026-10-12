#include "PluginProcessor.h"
#include "PluginEditor.h"

#include "Theme.h"

namespace
{
    juce::String formatTime (double seconds)
    {
        const int s = juce::jmax (0, (int) seconds);
        return juce::String (s / 60) + ":" + juce::String (s % 60).paddedLeft ('0', 2);
    }

    // The seek bar is a 6 px track; the grab area is widened to something a hand can hit
    constexpr int kTimelineGrab = 10;

    void fillCard (juce::Graphics& g, juce::Rectangle<float> r, float radius, juce::Colour fill)
    {
        g.setColour (fill);
        g.fillRoundedRectangle (r, radius);
        g.setColour (Theme::line);
        g.drawRoundedRectangle (r.reduced (0.5f), radius, 1.0f);
    }

    // The three doors, worded as the web tool's operations.py
    struct Door { const char* label; const char* summary; const char* note; };
    const Door kDoors[3] = {
        { "Key & BPM",       "Read the tempo, the musical key and its Camelot code. No separation.",  "No model, a few seconds" },
        { "Vocal removal",   "Split the track into the vocal and everything behind it.",               "Vocals + instrumental, WAV" },
        { "Stem separation", "Split the track into drums, bass, vocals and other instruments.",        "Four stems, WAV" },
    };

    SubverseSplitterAudioProcessorEditor::Operation opFor (int door)
    {
        using Op = SubverseSplitterAudioProcessorEditor::Operation;
        return door == 0 ? Op::Analysis : door == 1 ? Op::VocalRemoval : Op::Stems;
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

    chooseButton.getProperties().set ("variant", "primary");
    chooseButton.onClick = [this] { chooseFile(); };
    addChildComponent (chooseButton);

    changeFileButton.getProperties().set ("variant", "ghost");
    changeFileButton.onClick = [this] { chooseFile(); };
    addChildComponent (changeFileButton);

    runButton.getProperties().set ("variant", "primary");
    runButton.onClick = [this] { runSelected(); };
    addChildComponent (runButton);

    // after a result: back to the doors with the same file, or start over
    otherOpButton.onClick = [this] {
        // the same track back at the doors: the original file if it is still where it was, else the run's input
        juce::File src (shown.source);
        if (! src.existsAsFile()) src = audioProcessor.getModelRunner().getInputFile();
        clearResult();
        if (src.existsAsFile()) loadFile (src);
        else { pendingFile = juce::File(); refreshRecent(); resized(); repaint(); }
    };
    addChildComponent (otherOpButton);
    newFileButton.onClick = [this] { clearResult(); pendingFile = juce::File(); fileName.clear(); refreshRecent(); resized(); repaint(); };
    addChildComponent (newFileButton);

    // `.download-all`: the gold action of the mixer head. On the desktop the files are already on disk, so it shows them.
    showFilesButton.getProperties().set ("variant", "primary");
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

    openFolderButton.getProperties().set ("variant", "ghost");
    openFolderButton.onClick = [] { ElekLibrary::outputRoot().startAsProcess(); };
    addChildComponent (openFolderButton);

    playPauseButton.onClick = [this] { audioProcessor.togglePlayPause(); };
    addChildComponent (playPauseButton);

    // the SubverseLab mark from the brand kit (the family plugins' mark_cream.png)
    int size = 0;
    if (const char* data = BinaryData::getNamedResource ("mark_cream_png", size))
        markImage = juce::ImageCache::getFromMemory (data, size);
    if (const char* data = BinaryData::getNamedResource ("wordmark_cream_png", size))
        wordmarkImage = juce::ImageCache::getFromMemory (data, size);

    setSize (kW, kH);
    startTimer (50); // 20 FPS refresh for the playhead and progress

    // Debug hook: SPLITTER_AUTOTEST=<file> loads that file, runs Stem separation and starts playback when it finishes. A bug that
    // only appears in the Standalone cannot be chased from a console harness, and driving the real window by clicking is less reliable.
    if (auto* autoTest = std::getenv ("SPLITTER_AUTOTEST"))
    {
        const juce::File file { juce::String (autoTest) };
        if (file.existsAsFile())
        {
            autoPlayWhenReady = true;
            juce::MessageManager::callAsync ([this, file] { loadFile (file); selectOperation (2); runSelected(); });
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
    panels.clear();
    setLookAndFeel (nullptr);
}

// ---------------------------------------------------------------- state

SubverseSplitterAudioProcessorEditor::View SubverseSplitterAudioProcessorEditor::view() const
{
    if (audioProcessor.getModelRunner().isRunning()) return View::Running;
    if (resultShown)                                  return View::Done;
    if (pendingFile != juce::File())                  return View::Ready;
    return View::Empty;
}

void SubverseSplitterAudioProcessorEditor::loadFile (const juce::File& file)
{
    clearResult();
    pendingFile = file;
    fileName = file.getFileName();
    errorText.clear();
    pendingSeconds = 0.0;
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    if (std::unique_ptr<juce::AudioFormatReader> r { fm.createReaderFor (file) })
        pendingSeconds = r->sampleRate > 0 ? (double) r->lengthInSamples / r->sampleRate : 0.0;
    else
        errorText = "This file could not be read. Try WAV, AIFF, MP3 or FLAC.";
    resized();
    repaint();
}

void SubverseSplitterAudioProcessorEditor::selectOperation (int index)
{
    selected = juce::jlimit (-1, 2, index);
    runButton.setButtonText (selected < 0 ? juce::String ("Run") : "Run " + juce::String (kDoors[selected].label));
    runButton.setEnabled (selected >= 0 && errorText.isEmpty());
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
    audioProcessor.getModelRunner().startOperation (pendingFile, opFor (selected));
    awaitingResult = true;      // a Key & BPM run can end before the next timer tick sees it running
    resized();
    repaint();
}

void SubverseSplitterAudioProcessorEditor::clearResult()
{
    for (auto& p : panels) removeChildComponent (p.get());
    panels.clear();
    audioProcessor.clearPlayback();
    resultShown = false;
}

void SubverseSplitterAudioProcessorEditor::showRunnerResult (bool loadPlayback)
{
    auto& runner = audioProcessor.getModelRunner();
    if (runner.getOperation() == Operation::Analysis)
    {
        // Key & BPM writes nothing: the tiles are the result
        shown = {};
        shownOp = Operation::Analysis;
        shownAnalysis = runner.getAnalysis();
        fileName = runner.getInputFile().getFileName();
        shown.source = runner.getInputFile().getFullPathName();
        resultShown = true;
        pendingFile = juce::File();
        return;
    }
    ElekLibrary::Entry e;
    if (ElekLibrary::readManifest (runner.getOutputFolder(), e))
        buildResult (e, loadPlayback);
}

void SubverseSplitterAudioProcessorEditor::buildResult (const ElekLibrary::Entry& entry, bool loadPlayback)
{
    for (auto& p : panels) removeChildComponent (p.get());
    panels.clear();

    shown = entry;
    shownOp = entry.operation == "vocal_removal" ? Operation::VocalRemoval : Operation::Stems;
    shownAnalysis = entry.analysis;
    fileName = entry.title;
    resultShown = true;
    pendingFile = juce::File();

    struct Row { const char* label; const char* key; juce::Colour colour; };
    std::vector<Row> rows;
    if (shownOp == Operation::VocalRemoval)
        rows = { { "Vocals", "vocals", Theme::stemVocals }, { "Instrumental", "instrumental", Theme::stemInstrumental } };
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
    }

    // Each stem drawn against its own peak, capped at 6x: the rows are for finding where a part enters, and a shared scale
    // flattened three of the four; the cap keeps a nearly empty stem (the vocal of an instrumental) looking nearly empty.
    for (auto& panel : panels)
    {
        addAndMakeVisible (panel.get());
        const float stemPeak = panel->getPeak();
        panel->setDisplayScale (stemPeak > 0.0001f ? juce::jmin (6.0f, 1.0f / stemPeak) : 1.0f);
    }
}

void SubverseSplitterAudioProcessorEditor::openRecent (int index)
{
    if (index < 0 || index >= (int) recentRuns.size()) return;
    const auto entry = recentRuns[(size_t) index];
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

// ---------------------------------------------------------------- layout

juce::Rectangle<int> SubverseSplitterAudioProcessorEditor::analysisArea() const
{
    const auto v = view();
    if (v == View::Empty || v == View::Ready) return {};
    if (! (v == View::Running ? audioProcessor.getModelRunner().getAnalysis() : shownAnalysis).valid) return {};
    return contentArea().withTrimmedTop (kPad - 4).withHeight (kAnalysisH);
}

juce::Rectangle<int> SubverseSplitterAudioProcessorEditor::cardArea() const
{
    auto a = contentArea().withTrimmedTop (kPad - 4);
    if (! analysisArea().isEmpty()) a.removeFromTop (kAnalysisH + kGap);
    return a;
}

juce::Rectangle<int> SubverseSplitterAudioProcessorEditor::dropZone() const
{
    return cardArea().reduced (20).withHeight (262);
}

juce::Rectangle<int> SubverseSplitterAudioProcessorEditor::recentArea() const
{
    // under the drop zone: caption row (with "Open ELEK folder"), then the list
    auto a = cardArea().reduced (20);
    a.removeFromTop (262 + 22);
    return a;
}

juce::Rectangle<int> SubverseSplitterAudioProcessorEditor::recentRow (int index) const
{
    auto a = recentArea().withTrimmedTop (44);
    const int h = 44, gap = 6;
    auto r = juce::Rectangle<int> (a.getX(), a.getY() + index * (h + gap), a.getWidth(), h);
    return r.getBottom() <= a.getBottom() ? r : juce::Rectangle<int>();
}

juce::Rectangle<int> SubverseSplitterAudioProcessorEditor::doorBounds (int index) const
{
    // `.doors`: three equal columns, 12 px apart, under the file row and the caption
    auto area = cardArea().reduced (24).withTrimmedTop (64 + 40);
    area = area.withHeight (172);
    const int gap = 12, w = (area.getWidth() - gap * 2) / 3;
    return { area.getX() + index * (w + gap), area.getY(), w, area.getHeight() };
}

juce::Rectangle<int> SubverseSplitterAudioProcessorEditor::mixerHeadArea() const
{
    return contentArea().withTrimmedTop (kPad - 4 + kAnalysisH + kGap).withHeight (kHeadH);
}

juce::Rectangle<int> SubverseSplitterAudioProcessorEditor::timelineBounds() const
{
    auto head = mixerHeadArea();
    return { head.getX(), head.getBottom() + 12 + (kSeekH - 6) / 2, head.getWidth(), 6 };
}

void SubverseSplitterAudioProcessorEditor::resized()
{
    audioButton.setBounds (getWidth() - kPad - 130, (kHeader - 36) / 2, 130, 36);
#if SVL_LICENSING
    licenseButton.setBounds ((audioButton.isVisible() ? audioButton.getX() - 10 : getWidth() - kPad) - 100, (kHeader - 36) / 2, 100, 36);
    licenseOverlay.setBounds (getLocalBounds());
#endif

    const auto v = view();
    const bool separated = v == View::Done && shownOp != Operation::Analysis;

    chooseButton.setVisible (v == View::Empty);
    openFolderButton.setVisible (v == View::Empty);
    if (v == View::Empty)
    {
        auto z = dropZone();
        chooseButton.setBounds (z.getCentreX() - 78, z.getCentreY() + 44, 156, 44);
        auto ra = recentArea();
        openFolderButton.setBounds (ra.getRight() - 160, ra.getY(), 160, 34);
    }

    changeFileButton.setVisible (v == View::Ready);
    runButton.setVisible (v == View::Ready);
    if (v == View::Ready)
    {
        auto card = cardArea().reduced (24);
        changeFileButton.setBounds (card.getRight() - 124, card.getY() + 8, 124, 40);
        runButton.setBounds (card.getRight() - 240, doorBounds (0).getBottom() + 24, 240, 48);
        selectOperation (selected);
    }

    playPauseButton.setVisible (separated);
    showFilesButton.setVisible (separated);
    otherOpButton.setVisible (v == View::Done);
    newFileButton.setVisible (v == View::Done);
    if (v != View::Done) return;

    if (! separated)
    {
        // Key & BPM: the tiles are the result; the two ways on sit in the card below them
        auto card = cardArea().reduced (24);
        auto row = juce::Rectangle<int> (card.getX(), card.getY() + 92, card.getWidth(), 44);
        otherOpButton.setBounds (row.removeFromLeft (160));
        row.removeFromLeft (10);
        newFileButton.setBounds (row.removeFromLeft (120));
        return;
    }

    auto head = mixerHeadArea();
    playPauseButton.setBounds (head.removeFromLeft (52));
    showFilesButton.setBounds (head.removeFromRight (124).withSizeKeepingCentre (124, 40));
    head.removeFromRight (10);
    newFileButton.setBounds (head.removeFromRight (100).withSizeKeepingCentre (100, 40));
    head.removeFromRight (10);
    otherOpButton.setBounds (head.removeFromRight (148).withSizeKeepingCentre (148, 40));

    auto list = contentArea();
    list.setTop (timelineBounds().getBottom() + 18);
    const int gap = 10, n = (int) panels.size();
    const int rowH = n > 0 ? juce::jmin (150, (list.getHeight() - gap * (n - 1)) / n) : 0;
    for (auto& p : panels)
    {
        p->setBounds (list.removeFromTop (rowH));
        list.removeFromTop (gap);
    }
}

// ---------------------------------------------------------------- painting

void SubverseSplitterAudioProcessorEditor::paint (juce::Graphics& g)
{
    // body: radial-gradient(circle at 80% 0, --surface-2, transparent 42%), --bg
    g.fillAll (Theme::background);
    {
        const float cx = getWidth() * 0.8f, r = (float) juce::jmax (getWidth(), getHeight()) * 0.42f;
        juce::ColourGradient glow (Theme::surfaceRaised, cx, 0.0f, Theme::background.withAlpha (0.0f), cx + r, 0.0f, true);
        g.setGradientFill (glow);
        g.fillEllipse (cx - r, -r, r * 2.0f, r * 2.0f);
    }

    paintHeader (g);
    paintFooter (g);
    paintAnalysis (g);

    switch (view())
    {
        case View::Empty:   paintEmpty (g);   break;
        case View::Ready:   paintReady (g);   break;
        case View::Running: paintRunning (g); break;
        case View::Done:    paintDone (g);    break;
    }
}

namespace
{
    /** The product name in the family's squared, extended stroke letters (TUTKAL / MIHENK PluginEditor drawProductName),
        with the E that ELEK needs. Strokes are outlined and filled, so every renderer draws the same corners. Returns the width. */
    float drawProductName (juce::Graphics& g, const juce::String& name, float x, float y, float h, juce::Colour ink)
    {
        const float w = 1.34f * h, t = 0.19f * h, gap = 0.30f * h, in = t * 0.5f;
        juce::Path all;
        float cx = x;
        for (auto ch : name)
        {
            juce::Path p;
            const float L = cx + in, R = cx + w - in, T = y + in, B = y + h - in, M = y + h * 0.5f;
            switch (ch)
            {
                case 'E':
                    p.startNewSubPath (R + in * 0.2f, T); p.lineTo (L, T); p.lineTo (L, B); p.lineTo (R + in * 0.2f, B);
                    p.startNewSubPath (L, M); p.lineTo (R - w * 0.16f, M);
                    break;
                case 'K':
                    p.startNewSubPath (L, T - in); p.lineTo (L, B + in);
                    p.startNewSubPath (L, M + t * 0.2f); p.lineTo (R, T - in * 0.3f);
                    p.startNewSubPath (L + (R - L) * 0.36f, M - h * 0.12f); p.lineTo (R + in * 0.2f, B + in);
                    break;
                case 'L':
                    p.startNewSubPath (L, T - in); p.lineTo (L, B); p.lineTo (R + in * 0.2f, B);
                    break;
                default: break;
            }
            all.addPath (p);
            cx += w + gap;
        }
        juce::Path outline;
        juce::PathStrokeType (t, juce::PathStrokeType::mitered, juce::PathStrokeType::butt).createStrokedPath (outline, all);
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillPath (outline, juce::AffineTransform::translation (0.0f, 1.2f));
        g.setColour (ink);
        g.fillPath (outline);
        return cx - gap - x;
    }
}

void SubverseSplitterAudioProcessorEditor::paintHeader (juce::Graphics& g)
{
    // the family's place for the product: the name top left in its own letters, one gold line of what it does under it.
    // The SubverseLab mark and name live elsewhere (paintFooter), as on every plug-in face.
    const float x = (float) kPad;
    drawProductName (g, "ELEK", x, 24.0f, 30.0f, Theme::text);
    g.setColour (Theme::gold);
    const juce::String dot (juce::CharPointer_UTF8 (" \xc2\xb7 "));
    Theme::drawTracked (g, "STEM SEPARATION" + dot + "VOCAL REMOVAL" + dot + "KEY & TEMPO", { x, 64.0f, 600.0f, 16.0f },
                        Theme::bold (11.0f), 2.2f, juce::Justification::centredLeft);

    g.setColour (Theme::line);
    g.fillRect (0, kHeader - 1, getWidth(), 1);
}

void SubverseSplitterAudioProcessorEditor::paintFooter (juce::Graphics& g)
{
    // footer, as the plug-ins: a short principle on the left, the SubverseLab mark + wordmark on the right
    const float y0 = (float) (getHeight() - kFooter);
    g.setColour (Theme::line);
    g.fillRect (0.0f, y0, (float) getWidth(), 1.0f);
    const float cy = y0 + kFooter * 0.5f;
    g.setColour (Theme::textMuted);
    Theme::drawTracked (g, "RUNS ON THIS COMPUTER. NOTHING IS UPLOADED.", { (float) kPad, cy - 8.0f, 500.0f, 16.0f },
                        Theme::regular (10.5f), 1.8f, juce::Justification::centredLeft);
    if (markImage.isValid() && wordmarkImage.isValid())
    {
        const float wordH = 15.0f, markH = 23.0f, right = (float) (getWidth() - kPad);
        const float ww = wordH * (float) wordmarkImage.getWidth() / (float) wordmarkImage.getHeight();
        const float mw = markH * (float) markImage.getWidth() / (float) markImage.getHeight();
        g.setOpacity (1.0f);
        g.drawImage (wordmarkImage, { right - ww, cy - wordH * 0.5f, ww, wordH }, juce::RectanglePlacement::stretchToFit);
        g.drawImage (markImage, { right - ww - 8.0f - mw, cy - markH * 0.5f - 1.0f, mw, markH }, juce::RectanglePlacement::stretchToFit);
    }
}

void SubverseSplitterAudioProcessorEditor::paintAnalysis (juce::Graphics& g)
{
    // `.analysis-bar`: four tiles on --bg, caption 11 px upper-case with .12em tracking, value 20 px bold
    auto area = analysisArea();
    if (area.isEmpty()) return;
    const auto analysis = view() == View::Running ? audioProcessor.getModelRunner().getAnalysis() : shownAnalysis;

    const juce::String captions[4] = { "TEMPO", "KEY", "CAMELOT", "KEY CONFIDENCE" };
    const juce::String values[4] = {
        juce::String (analysis.bpm, 1) + " BPM",
        analysis.key + " " + (analysis.scale == "min" ? "minor" : "major"),
        analysis.camelot,
        juce::String (juce::roundToInt (analysis.keyConfidence * 100.0f)) + "%"
    };

    const int gap = 10;
    const int w = (area.getWidth() - gap * 3) / 4;
    for (int i = 0; i < 4; ++i)
    {
        auto tile = area.removeFromLeft (i == 3 ? area.getWidth() : w).toFloat();
        area.removeFromLeft (gap);
        fillCard (g, tile, Theme::radiusSm, Theme::background);
        auto in = tile.reduced (14.0f, 12.0f);
        g.setColour (Theme::text);
        g.setFont (Theme::bold (21.0f));
        g.drawText (values[i], in.removeFromTop (26.0f), juce::Justification::centredLeft, true);
        g.setColour (Theme::textMuted);
        Theme::drawTracked (g, captions[i], in, Theme::regular (11.0f), 1.3f, juce::Justification::centredLeft);
    }
}

void SubverseSplitterAudioProcessorEditor::paintEmpty (juce::Graphics& g)
{
    // `.workbench` card with the dashed `.drop` inside
    fillCard (g, cardArea().toFloat(), Theme::radius, Theme::surface);
    auto zone = dropZone().toFloat();

    // a solid tile, not a dashed "cut here" outline (owner, 12 Oct: the first prototype's cut marks)
    g.setColour (isDraggingActive ? Theme::surfaceHigh : Theme::surfaceRaised.withAlpha (0.55f));
    g.fillRoundedRectangle (zone, Theme::radiusSm);
    g.setColour (isDraggingActive ? Theme::teal : Theme::line);
    g.drawRoundedRectangle (zone.reduced (0.5f), Theme::radiusSm, isDraggingActive ? 2.0f : 1.0f);

    // a tray with an arrow into it, above the words
    {
        const float ix = zone.getCentreX(), iy = zone.getCentreY() - 66.0f;
        juce::Path tray;
        tray.startNewSubPath (ix - 22.0f, iy + 4.0f); tray.lineTo (ix - 22.0f, iy + 16.0f); tray.lineTo (ix + 22.0f, iy + 16.0f); tray.lineTo (ix + 22.0f, iy + 4.0f);
        tray.startNewSubPath (ix, iy - 22.0f); tray.lineTo (ix, iy + 6.0f);
        tray.startNewSubPath (ix - 10.0f, iy - 4.0f); tray.lineTo (ix, iy + 6.0f); tray.lineTo (ix + 10.0f, iy - 4.0f);
        g.setColour (isDraggingActive ? Theme::tealLight : Theme::gold);
        g.strokePath (tray, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    const float cy = zone.getCentreY();
    g.setColour (Theme::text);
    g.setFont (Theme::bold (24.0f));
    g.drawText (isDraggingActive ? "Release to load" : "Drop a track here",
                juce::Rectangle<float> (zone.getX(), cy - 32, zone.getWidth(), 32), juce::Justification::centred);
    g.setColour (Theme::textMuted);
    g.setFont (Theme::regular (14.5f));
    g.drawText ("WAV, AIFF, MP3 or FLAC. You choose what to do with it next.",
                juce::Rectangle<float> (zone.getX(), cy + 4, zone.getWidth(), 22), juce::Justification::centred);

    paintRecent (g);

    if (errorText.isNotEmpty())
    {
        g.setColour (Theme::danger);
        g.setFont (Theme::regular (14.0f));
        g.drawFittedText (errorText, zone.withTrimmedTop (zone.getHeight() * 0.5f + 96).reduced (40, 0).toNearestInt(),
                          juce::Justification::centredTop, 3);
    }
}

void SubverseSplitterAudioProcessorEditor::paintRecent (juce::Graphics& g)
{
    auto ra = recentArea();
    g.setColour (Theme::textMuted);
    Theme::drawTracked (g, "EARLIER SEPARATIONS", ra.removeFromTop (34).toFloat(), Theme::regular (11.0f), 1.3f, juce::Justification::centredLeft);

    if (recentRuns.empty())
    {
        g.setFont (Theme::regular (14.0f));
        g.drawText ("Nothing separated yet. Every result is saved to Music/ELEK and listed here until you delete it.",
                    ra.withTrimmedTop (10).withHeight (40).toFloat(), juce::Justification::centredLeft, true);
        return;
    }

    for (int i = 0; i < (int) recentRuns.size(); ++i)
    {
        const auto row = recentRow (i);
        if (row.isEmpty()) break;
        const auto& e = recentRuns[(size_t) i];
        fillCard (g, row.toFloat(), Theme::radiusSm, hoverRecent == i ? Theme::surfaceRaised : Theme::background);
        auto in = row.reduced (16, 0).toFloat();

        const bool stems = e.operation == "stem_separation";
        g.setColour (stems ? Theme::stemDrums : Theme::stemVocals);
        g.fillEllipse (in.getX(), in.getCentreY() - 4.0f, 8.0f, 8.0f);
        in.removeFromLeft (20.0f);

        g.setColour (Theme::textMuted);
        g.setFont (Theme::regular (13.0f));
        g.drawText (e.created.formatted ("%d %b %Y  %H:%M"), in.removeFromRight (150.0f), juce::Justification::centredRight);
        const juce::String dot (juce::CharPointer_UTF8 ("  \xc2\xb7  "));
        juce::String tags = stems ? "4 stems" : "Vocal removal";
        if (e.analysis.valid) tags << dot << ElekLibrary::bpmTag (e.analysis).replace ("BPM", " BPM");
        if (ElekLibrary::keyIsSure (e.analysis)) tags << dot << ElekLibrary::keyTag (e.analysis);
        g.drawText (tags, in.removeFromRight (260.0f), juce::Justification::centredLeft);

        g.setColour (Theme::text);
        g.setFont (Theme::bold (15.0f));
        g.drawText (e.title, in, juce::Justification::centredLeft, true);
    }
}

void SubverseSplitterAudioProcessorEditor::paintReady (juce::Graphics& g)
{
    fillCard (g, cardArea().toFloat(), Theme::radius, Theme::surface);
    auto card = cardArea().reduced (24).toFloat();

    // the file: a tile with the name and its length
    auto fileRow = card.removeFromTop (56.0f).withTrimmedRight (140.0f);
    fillCard (g, fileRow, Theme::radiusSm, Theme::background);
    auto in = fileRow.reduced (16.0f, 0.0f);
    g.setColour (Theme::teal);
    g.fillEllipse (in.getX(), in.getCentreY() - 5.0f, 10.0f, 10.0f);
    in.removeFromLeft (22.0f);
    g.setColour (Theme::textMuted);
    g.setFont (Theme::regular (14.0f));
    const auto len = pendingSeconds > 0.0 ? formatTime (pendingSeconds) : juce::String();
    g.drawText (len, in, juce::Justification::centredRight);
    g.setColour (Theme::text);
    g.setFont (Theme::bold (16.0f));
    g.drawText (fileName, in.withTrimmedRight (70.0f), juce::Justification::centredLeft, true);

    // caption
    card.removeFromTop (20.0f);
    g.setColour (Theme::textMuted);
    Theme::drawTracked (g, "CHOOSE WHAT TO DO", card.removeFromTop (20.0f), Theme::regular (11.0f), 1.3f, juce::Justification::centredLeft);

    // `.door` ×3: surface card, line border, 18 px radius; active = teal border + inset; label 16, summary muted, note gold
    for (int i = 0; i < 3; ++i)
    {
        auto d = doorBounds (i).toFloat();
        const bool active = selected == i, hover = hoverDoor == i;
        g.setColour (active || hover ? Theme::surfaceRaised : Theme::surface);
        g.fillRoundedRectangle (d, Theme::radius);
        g.setColour (active ? Theme::teal : Theme::line);
        g.drawRoundedRectangle (d.reduced (0.5f), Theme::radius, active ? 2.0f : 1.0f);

        auto t = d.reduced (20.0f);
        g.setColour (Theme::text);
        g.setFont (Theme::bold (17.0f));
        g.drawText (kDoors[i].label, t.removeFromTop (24.0f), juce::Justification::centredLeft);
        t.removeFromTop (10.0f);
        g.setColour (Theme::textMuted);
        g.setFont (Theme::regular (14.0f));
        g.drawFittedText (kDoors[i].summary, t.removeFromTop (60.0f).toNearestInt(), juce::Justification::topLeft, 3, 1.0f);
        g.setColour (Theme::gold);
        g.setFont (Theme::bold (13.0f));
        g.drawText (kDoors[i].note, t, juce::Justification::bottomLeft);
    }

    if (errorText.isNotEmpty())
    {
        g.setColour (Theme::danger);
        g.setFont (Theme::regular (14.0f));
        g.drawFittedText (errorText, juce::Rectangle<int> ((int) card.getX(), doorBounds (0).getBottom() + 24, 560, 48),
                          juce::Justification::centredLeft, 2);
    }
    else if (selected < 0)
    {
        g.setColour (Theme::textMuted);
        g.setFont (Theme::regular (14.0f));
        g.drawText ("Nothing runs until you choose.", juce::Rectangle<int> ((int) card.getX(), doorBounds (0).getBottom() + 24, 560, 48),
                    juce::Justification::centredLeft);
    }
}

void SubverseSplitterAudioProcessorEditor::paintRunning (juce::Graphics& g)
{
    // `.status`: status line with the percentage, the teal progress bar, one muted line
    fillCard (g, cardArea().toFloat(), Theme::radius, Theme::surface);
    auto& runner = audioProcessor.getModelRunner();
    const auto op = runner.getOperation();
    auto zone = cardArea().reduced (20).toFloat();

    auto box = zone.withSizeKeepingCentre (juce::jmin (620.0f, zone.getWidth()), 132.0f);
    auto line = box.removeFromTop (26.0f);
    g.setColour (Theme::text);
    g.setFont (Theme::bold (17.0f));
    g.drawText (op == Operation::Analysis ? "Measuring tempo and key" : op == Operation::VocalRemoval ? "Removing the vocal" : "Separating stems",
                line, juce::Justification::centredLeft);
    g.setColour (Theme::textMuted);
    g.setFont (Theme::bold (15.0f));
    g.drawText (juce::String (juce::roundToInt (runner.getProgress() * 100.0)) + " %", line, juce::Justification::centredRight);

    box.removeFromTop (14.0f);
    auto bar = box.removeFromTop (8.0f);
    g.setColour (Theme::surfaceHigh);
    g.fillRoundedRectangle (bar, 4.0f);
    g.setColour (Theme::teal);
    g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * (float) juce::jlimit (0.0, 1.0, runner.getProgress())), 4.0f);

    box.removeFromTop (16.0f);
    g.setColour (Theme::textMuted);
    g.setFont (Theme::regular (14.0f));
    g.drawFittedText (runner.getInputFile().getFileName() + ". Everything runs on this computer; nothing is uploaded.",
                      box.toNearestInt(), juce::Justification::topLeft, 2);
}

void SubverseSplitterAudioProcessorEditor::paintDone (juce::Graphics& g)
{
    if (shownOp == Operation::Analysis)
    {
        // Key & BPM: the tiles above are the result; the card says what was measured and offers the next step
        fillCard (g, cardArea().toFloat(), Theme::radius, Theme::surface);
        auto card = cardArea().reduced (24).toFloat();
        g.setColour (Theme::text);
        g.setFont (Theme::bold (17.0f));
        g.drawText (fileName, card.removeFromTop (26.0f), juce::Justification::centredLeft, true);
        g.setColour (Theme::textMuted);
        g.setFont (Theme::regular (14.0f));
        g.drawText ("Tempo and key measured on the whole mix. Nothing was separated.", card.removeFromTop (30.0f), juce::Justification::centredLeft);
        return;
    }

    // `.mixer-head`: play, clock (current bold, "/ total" muted), file name over "N STEM MIX"; then the seek bar
    auto head = mixerHeadArea().toFloat();
    head.removeFromLeft (52 + 14);
    const double pos = audioProcessor.getPlaybackPosition(), len = audioProcessor.getTotalLength();

    auto clock = head.removeFromLeft (110);
    g.setColour (Theme::text);
    g.setFont (Theme::bold (17.0f));
    const auto now = formatTime (pos);
    g.drawText (now, clock, juce::Justification::centredLeft);
    const float nowW = juce::GlyphArrangement::getStringWidth (Theme::bold (17.0f), now);
    g.setColour (Theme::textMuted);
    g.setFont (Theme::regular (15.0f));
    g.drawText ("/ " + formatTime (len), clock.withTrimmedLeft (nowW + 6), juce::Justification::centredLeft);

    head.removeFromLeft (10);
    auto title = head.withTrimmedRight (124 + 10 + 100 + 10 + 148 + 14);
    g.setColour (Theme::text);
    g.setFont (Theme::bold (16.0f));
    g.drawText (fileName, title.removeFromTop (head.getHeight() * 0.55f), juce::Justification::bottomLeft, true);
    g.setColour (Theme::textMuted);
    Theme::drawTracked (g, shownOp == Operation::VocalRemoval ? "VOCAL REMOVAL" : "4 STEM MIX", title.withTrimmedTop (2),
                        Theme::regular (11.0f), 1.3f, juce::Justification::centredLeft);

    // seek bar: teal accent (`.master-seek { accent-color: var(--teal) }`)
    const auto track = timelineBounds().toFloat();
    const float progress = len > 0.0 ? (float) juce::jlimit (0.0, 1.0, pos / len) : 0.0f;
    g.setColour (Theme::surfaceHigh);
    g.fillRoundedRectangle (track, 3.0f);
    g.setColour (Theme::teal);
    g.fillRoundedRectangle (track.withWidth (track.getWidth() * progress), 3.0f);
    g.setColour (Theme::text);
    const float hx = track.getX() + track.getWidth() * progress;
    g.fillEllipse (hx - 7.0f, track.getCentreY() - 7.0f, 14.0f, 14.0f);
}

void SubverseSplitterAudioProcessorEditor::PlayButton::paintButton (juce::Graphics& g, bool over, bool)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (over ? Theme::surfaceHigh : Theme::surfaceRaised);
    g.fillRoundedRectangle (r, Theme::radiusBt);
    g.setColour (Theme::line);
    g.drawRoundedRectangle (r, Theme::radiusBt, 1.0f);

    g.setColour (Theme::text);
    const auto c = r.getCentre();
    if (playing)
    {
        g.fillRoundedRectangle (c.x - 7.0f, c.y - 8.0f, 5.0f, 16.0f, 1.5f);
        g.fillRoundedRectangle (c.x + 2.0f, c.y - 8.0f, 5.0f, 16.0f, 1.5f);
    }
    else
    {
        juce::Path tri;
        tri.addTriangle (c.x - 5.0f, c.y - 9.0f, c.x - 5.0f, c.y + 9.0f, c.x + 9.0f, c.y);
        g.fillPath (tri);
    }
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
    // loads only: what happens to the file is chosen at the doors
    isDraggingActive = false;
    if (files.size() > 0)
        loadFile (juce::File (files[0]));
    repaint();
}

void SubverseSplitterAudioProcessorEditor::mouseMove (const juce::MouseEvent& e)
{
    int h = -1, r = -1;
    if (view() == View::Ready)
        for (int i = 0; i < 3; ++i)
            if (doorBounds (i).contains (e.getPosition())) h = i;
    if (view() == View::Empty)
        for (int i = 0; i < (int) recentRuns.size(); ++i)
            if (recentRow (i).contains (e.getPosition())) r = i;
    if (h != hoverDoor || r != hoverRecent) { hoverDoor = h; hoverRecent = r; repaint(); }
}

void SubverseSplitterAudioProcessorEditor::mouseDown (const juce::MouseEvent& e)
{
    switch (view())
    {
        case View::Empty:
            // the whole drop zone opens the file chooser, as the web tool's <label class="drop"> does
            if (dropZone().contains (e.getPosition())) { chooseFile(); return; }
            for (int i = 0; i < (int) recentRuns.size(); ++i)
            {
                const auto row = recentRow (i);
                if (row.isEmpty()) break;
                if (row.contains (e.getPosition())) { openRecent (i); return; }
            }
            return;
        case View::Ready:
            for (int i = 0; i < 3; ++i)
                if (doorBounds (i).contains (e.getPosition())) { selectOperation (i); return; }
            return;
        case View::Done:
            seekFromMouse (e.getPosition(), true);
            return;
        case View::Running:
            return;
    }
}

void SubverseSplitterAudioProcessorEditor::mouseDrag (const juce::MouseEvent& e)
{
    // once the gesture started on the bar, following the pointer off it is what a scrub is
    if (view() == View::Done && timelineBounds().expanded (0, kTimelineGrab).contains (e.mouseDownPosition.toInt()))
        seekFromMouse (e.getPosition(), false);
}

void SubverseSplitterAudioProcessorEditor::seekFromMouse (juce::Point<int> position, bool startedOnBar)
{
    const auto track = timelineBounds();
    if (panels.empty() || track.getWidth() <= 0) return;
    if (startedOnBar && ! track.expanded (0, kTimelineGrab).contains (position)) return;

    const double length = audioProcessor.getTotalLength();
    if (length <= 0.0) return;

    audioProcessor.setPlaybackPosition (length * juce::jlimit (0.0, 1.0, (double) (position.x - track.getX()) / track.getWidth()));
    repaint();
}

void SubverseSplitterAudioProcessorEditor::timerCallback()
{
    if (playPauseButton.playing != audioProcessor.getIsPlaying())
    {
        playPauseButton.playing = audioProcessor.getIsPlaying();
        playPauseButton.repaint();
    }
    if (audioProcessor.getIsPlaying()) repaint();

    auto& runner = audioProcessor.getModelRunner();
    const bool running = runner.isRunning();

    if (running)
    {
        if (! awaitingResult) { resized(); repaint(); }
        awaitingResult = true;
        // only the status card, and only when the figure moves (the tiles appear once, part-way through)
        const int pct = juce::roundToInt (runner.getProgress() * 100.0);
        const bool tiles = runner.getAnalysis().valid;
        if (pct != lastPct || tiles != lastTiles)
        {
            if (tiles != lastTiles) repaint(); else repaint (cardArea());
            lastPct = pct; lastTiles = tiles;
        }
    }
    else if (awaitingResult)
    {
        awaitingResult = false;
        // a run just ended: show its result, or put the error back on the doors
        if (runner.isFinishedSuccessfully())
        {
            showRunnerResult (true);
            refreshRecent();
        }
        else
        {
            errorText = runner.getErrorMessage().isNotEmpty() ? runner.getErrorMessage() : juce::String ("The operation failed.");
            pendingFile = runner.getInputFile();
        }
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
// STEM PANEL IMPLEMENTATION
// ==============================================================================

namespace StemLayout
{
    // One definition of the row, read by paint() and resized() alike (`.stem-row { grid-template-columns: 110px 1fr auto }`)
    constexpr int padding   = 14;
    constexpr int name      = 142;
    constexpr int gap       = 16;
    constexpr int button    = 36;     // M and S
    constexpr int buttonGap = 8;
    constexpr int volumeNum = 30;
    constexpr int volume    = 120;
    constexpr int exportW   = 64;
    constexpr int controls  = button * 2 + buttonGap + gap + volumeNum + 6 + volume + gap + exportW;
}

SubverseSplitterAudioProcessorEditor::StemPanel::StemPanel(const juce::String& name, const juce::File& file, juce::Colour color, int index, SubverseSplitterAudioProcessor& processor, juce::DragAndDropContainer& container)
    : Component(), targetFile(file), themeColor(color), stemIndex(index), audioProcessor(processor), dragContainer(container)
{
    setName (name);

    // M and S as `.mix-toggle`: on = teal border and label (mute), gold for solo (`.solo.active`)
    muteButton.setClickingTogglesState (true);
    muteButton.setColour (juce::TextButton::textColourOnId, Theme::tealLight);
    muteButton.onClick = [this] { audioProcessor.setStemMute (stemIndex, muteButton.getToggleState()); repaint(); };
    addAndMakeVisible (muteButton);

    soloButton.setClickingTogglesState (true);
    soloButton.setColour (juce::TextButton::textColourOnId, Theme::goldLight);
    soloButton.onClick = [this] { audioProcessor.setStemSolo (stemIndex, soloButton.getToggleState()); };
    addAndMakeVisible (soloButton);

    // level, separate from mute (`.volume { accent-color: var(--gold) }`)
    volumeSlider.setRange (0.0, 1.0);
    volumeSlider.setValue (audioProcessor.getStemVolume (stemIndex), juce::dontSendNotification);
    volumeSlider.setColour (juce::Slider::trackColourId, Theme::gold);
    volumeSlider.onValueChange = [this] {
        audioProcessor.setStemVolume (stemIndex, (float) volumeSlider.getValue());
        repaint();
    };
    addAndMakeVisible (volumeSlider);

    // `.stem-download` (transparent): click shows the WAV in Finder, drag drops it into a DAW
    exportButton.getProperties().set ("variant", "ghost");
    exportButton.setTooltip ("Drag into your DAW, or click to show the file");
    exportButton.addMouseListener (this, false);
    addAndMakeVisible (exportButton);

    startTimerHz (30);
    generateWaveformPreview();
}

SubverseSplitterAudioProcessorEditor::StemPanel::~StemPanel() { stopTimer(); }

void SubverseSplitterAudioProcessorEditor::StemPanel::generateWaveformPreview()
{
    if (! targetFile.existsAsFile()) return;
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (targetFile));
    if (reader != nullptr)
    {
        // Peak per block, not RMS: a waveform shows how far the signal went, and RMS flattens a drum stem (mostly silence
        // with events in it) to nothing
        const int numPoints = 240;
        waveformData.assign ((size_t) numPoints, 0.0f);

        const int samplesPerPoint = juce::jmax (1, (int) (reader->lengthInSamples / numPoints));
        juce::AudioBuffer<float> tempBuffer (1, samplesPerPoint);

        peak = 0.0f;
        for (int i = 0; i < numPoints; ++i)
        {
            tempBuffer.clear();
            reader->read (&tempBuffer, 0, samplesPerPoint, (juce::int64) i * samplesPerPoint, true, false);
            const float blockPeak = tempBuffer.getMagnitude (0, 0, samplesPerPoint);
            waveformData[(size_t) i] = blockPeak;
            peak = juce::jmax (peak, blockPeak);
        }
    }
}

void SubverseSplitterAudioProcessorEditor::StemPanel::timerCallback()
{
    // sync with the processor (solo on one row changes what the others mean)
    const bool m = audioProcessor.getStemMute (stemIndex), s = audioProcessor.getStemSolo (stemIndex);
    if (m != muteButton.getToggleState() || s != soloButton.getToggleState())
    {
        muteButton.setToggleState (m, juce::dontSendNotification);
        soloButton.setToggleState (s, juce::dontSendNotification);
        repaint();
    }
    if (audioProcessor.getIsPlaying())
        repaint();     // the played part of the waveform moves
}

juce::Rectangle<float> SubverseSplitterAudioProcessorEditor::StemPanel::waveArea() const
{
    auto r = getLocalBounds().reduced (StemLayout::padding);
    r.removeFromLeft (StemLayout::name);
    r.removeFromRight (StemLayout::controls + StemLayout::gap);
    return r.toFloat().reduced (0.0f, 4.0f);
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
    if (e.originalComponent == this && waveArea().contains (e.position))   // the waveform seeks, as in the web tool
        seekTo (e.x);
}

void SubverseSplitterAudioProcessorEditor::StemPanel::mouseDrag (const juce::MouseEvent& e)
{
    if (e.originalComponent == this)
    {
        if (waveArea().contains (e.mouseDownPosition))
            seekTo (e.x);
        return;
    }
    if (e.originalComponent == &exportButton && ! dragging && e.getDistanceFromDragStart() > 4
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
    auto bounds = getLocalBounds().toFloat();

    // `.stem-row`: --bg tile, 1 px line, 12 px radius
    fillCard (g, bounds, Theme::radiusSm, Theme::background);

    // name column: colour dot + name (`.stem-name i` + strong)
    auto inner = getLocalBounds().reduced (StemLayout::padding).toFloat();
    auto nameCol = inner.removeFromLeft ((float) StemLayout::name);
    g.setColour (themeColor.withAlpha (isMuted ? 0.35f : 1.0f));
    g.fillEllipse (nameCol.getX(), nameCol.getCentreY() - 5.0f, 10.0f, 10.0f);
    g.setColour (isMuted ? Theme::textMuted : Theme::text);
    g.setFont (Theme::bold (16.0f));
    g.drawText (getName(), nameCol.withTrimmedLeft (20.0f), juce::Justification::centredLeft);

    // waveform: one bar per peak, base in --surface-3, the played part in the stem colour (Waveform.jsx)
    const auto w = waveArea();
    if (! waveformData.empty() && w.getWidth() > 8.0f)
    {
        const double length = audioProcessor.getTotalLength();
        const float played = length > 0.0 ? (float) juce::jlimit (0.0, 1.0, audioProcessor.getPlaybackPosition() / length) : 0.0f;
        const float playedX = w.getX() + w.getWidth() * played;
        const int n = (int) waveformData.size();
        const float step = w.getWidth() / (float) n;
        const float barW = juce::jmax (1.0f, step * 0.55f);
        const float half = w.getHeight() * 0.5f, cy = w.getCentreY();
        const auto base = Theme::surfaceHigh;
        const auto lit = isMuted ? themeColor.withAlpha (0.35f) : themeColor;
        for (int i = 0; i < n; ++i)
        {
            const float x = w.getX() + step * (float) i + (step - barW) * 0.5f;
            const float h = juce::jlimit (1.0f, half, juce::jmin (1.0f, waveformData[(size_t) i] * displayScale) * half);
            g.setColour (x + barW * 0.5f <= playedX ? lit : base);
            g.fillRoundedRectangle (x, cy - h, barW, h * 2.0f, barW * 0.5f);
        }
    }

    // the volume figure in front of the slider (`.volume span`)
    auto controls = getLocalBounds().reduced (StemLayout::padding);
    controls = controls.removeFromRight (StemLayout::controls);
    controls.removeFromLeft (StemLayout::button * 2 + StemLayout::buttonGap + StemLayout::gap);
    g.setColour (Theme::textMuted);
    g.setFont (Theme::regular (13.0f));
    g.drawText (juce::String (juce::roundToInt (volumeSlider.getValue() * 100.0)), controls.removeFromLeft (StemLayout::volumeNum),
                juce::Justification::centredRight);
}

void SubverseSplitterAudioProcessorEditor::StemPanel::resized()
{
    // one horizontal row, so the same control sits at the same x in every stem
    auto bounds = getLocalBounds().reduced (StemLayout::padding);
    auto row = bounds.withSizeKeepingCentre (bounds.getWidth(), 36);
    auto controls = row.removeFromRight (StemLayout::controls);

    muteButton.setBounds (controls.removeFromLeft (StemLayout::button));
    controls.removeFromLeft (StemLayout::buttonGap);
    soloButton.setBounds (controls.removeFromLeft (StemLayout::button));
    controls.removeFromLeft (StemLayout::gap + StemLayout::volumeNum + 6);
    volumeSlider.setBounds (controls.removeFromLeft (StemLayout::volume));
    controls.removeFromLeft (StemLayout::gap);
    exportButton.setBounds (controls);
}
