#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Theme.h"
#include "SvlMarkLink.h"
#include "ElekLibrary.h"
#if SVL_LICENSING
 #include "../../subverselab-plugin-common/Licensing/LicenseOverlay.h"
#endif

/*  ELEK 2.0.0 window, direction B "Overview" (docs/ui_directions/B_*.png; geometry from scripts/ui_directions/directions.py).
    The previous editor is kept, outside the build, in Source/_before_dirB/.

      header     the E lockup (spine + six stem bars + "LEK"), "STEM SEPARATOR" and the version as a separate label
      left       the overview card (the mix waveform, time ruler, play key and clock) over one lane per stem, all lanes on the
                 SAME time axis as the overview (kTL / kTW) with one playhead through all of them; the DRUM PARTS group sits
                 under Drums. A cream spine runs down the left (the mix), each lane has its colour tab.
      inspector  the slim panel on the right: What to make (start), Analysis + Working (run),
                 Analysis + Files + Show files / Other operation / New file (result)
      footer     one line of principle, the SubverseLab icon bottom right (links to the manual)

    The flow is unchanged: load a file, choose ONE operation, run it. Nothing starts on drop.
*/
class SubverseSplitterAudioProcessorEditor  : public juce::AudioProcessorEditor,
                                              public juce::FileDragAndDropTarget,
                                              public juce::Timer,
                                              public juce::DragAndDropContainer
{
public:
    static constexpr int kW = 1000, kH = 760;
    using Operation = SubverseSplitterAudioProcessor::ModelRunner::Operation;

    // ---- the one geometry every part reads (window points) ------------------------------------------------------------
    // LAYOUT 2 (18 Nov, owner's go on the lead's self-review): one column, no inspector. The waveforms get the width (642 of
    // 1000, was 428); the doors sit under the file; the result's actions are a bar above the footer; lanes share the height.
    static constexpr int kHeader = 44, kFooter = 32, kPad = 24;
    static constexpr int kCardTop = 56, kCardH = 156;       // the overview card
    static constexpr int kTL = 262, kTW = 642;              // the shared time axis: overview and every lane
    static constexpr int kLaneX = 34, kLaneW = 942;         // a lane, left to right
    static constexpr int kLaneGap = 3, kGroupH = 28;
    static constexpr int kLanesTop = 220, kBarH = 48;       // lanes start here; the action bar above the footer
    static constexpr int kPeakPoints = 1200;                // waveform resolution (peaks per file)

    SubverseSplitterAudioProcessorEditor (SubverseSplitterAudioProcessor&);
    ~SubverseSplitterAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** The overview's waveform (the seek area at the top). Drawing and hit-testing read the same rectangle. */
    juce::Rectangle<int> timelineBounds() const;

    // FileDragAndDropTarget overrides
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragMove (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    void timerCallback() override;

    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;
    void seekFromMouse (juce::Point<int> position, bool startedOnBar);

    /** Picks an option (0 Key & BPM, 1 Vocal removal, 2 4 stems, 3 6 stems); public for the snapshot harness. */
    void selectOperation (int index);

    /** Reopens an earlier run from the list (0 = newest); public for the snapshot harness. */
    void openRecent (int index);

    /** One drum part (bombo, redoblante, platillos, toms) as the group lists it. The list is data: rows are made from it. */
    struct DrumPart
    {
        juce::String key;       // manifest key of the part's WAV
        juce::String label;     // shown in the row
        juce::File   file;
        bool         enabled { false };   // false: no data for this run (the row shows the empty state)
        int          stemIndex { -1 };    // processor playback channel (kFirstDrumPart + part); -1 when the part has no audio
    };

    /** The four parts in display order; `enabled`, `file` and `stemIndex` are filled from the run's manifest (keys kick, snare,
        cymbals, toms, as the engine writes them). */
    static std::vector<DrumPart> drumPartsFor (const ElekLibrary::Entry& entry);

    /** Opens or closes the drum parts group under the Drums row. */
    void setDrumGroupOpen (bool shouldBeOpen);

    /** TEST ONLY (snapshot harness, never called by the product): shows a finished run from `entry`, with these drum parts
        instead of the manifest's. */
    void showEntryForTest (const ElekLibrary::Entry& entry, const std::vector<DrumPart>& parts);

    /** TEST ONLY: a press at x1, a drag to x2 and a release on the waveform row (window x; x1 == x2 is a click; clicks = 2 a
        double-click), through the same mouseDown / mouseDrag / mouseUp the real mouse uses. */
    void timelineGestureForTest (int x1, int x2, int clicks = 1);
    juce::String regionForTest() const { return hasRegion() ? juce::String (regionA, 2) + "-" + juce::String (regionB, 2) : juce::String ("none"); }
    void pressPlayForTest() { playPauseButton.onClick(); }
    void pressStopForTest() { stopButton.onClick(); }
    bool newFileVisibleForTest() const { return newFileButton.isVisible() && newFileButton.getWidth() > 0; }
    /** TEST ONLY (how-to video, 20 Nov): where a control is on screen, editor px; empty when it is not shown. */
    juce::Rectangle<int> controlBoundsForTest (const juce::String& name) const
    {
        auto vis = [] (const juce::Component& c) { return c.isVisible() ? c.getBounds() : juce::Rectangle<int>(); };
        if (name == "run") return vis (runButton);
        if (name == "play") return vis (playPauseButton);
        if (name == "stop") return vis (stopButton);
        if (name == "newfile") return vis (newFileButton);
        if (name == "split") return vis (splitDrumsButton);
        if (name == "history") return vis (historyButton);
        if (name == "home") return vis (homeButton);
        if (name == "changefile") return vis (changeFileButton);
        if (name.startsWith ("tile")) return optionBounds (name.getTrailingIntValue());
        return {};
    }
    bool stopVisibleForTest() const { return stopButton.isVisible(); }
    bool historyVisibleForTest() const { return historyButton.isVisible(); }

    /** TEST ONLY (snapshot harness): the region as a drag would set it, and a press of "Split the drums". */
    void setRegionForTest (double fromSec, double toSec) { regionA = fromSec; regionB = toSec; repaint(); }
    void pressSplitDrums() { splitDrums(); }
    bool splitDrumsVisibleForTest() const { return splitDrumsButton.isVisible(); }

    /** TEST ONLY (snapshot harness): navigation and FAST as the keys do them, and what the window is showing. */
    void pressHome() { goHome(); }
    void pressHistory() { showHistory(); }
    void pressFast() { toggleFast(); }
    juce::String viewNameForTest() const;
    /** TEST ONLY: the loaded file's tempo / key reading ("" while none), and whether Run can be pressed. */
    juce::String loadedAnalysisForTest() const { return pendingAnalysis.valid ? juce::String (pendingAnalysis.bpm, 1) + " BPM " + pendingAnalysis.key + " " + pendingAnalysis.scale : juce::String(); }
    bool runEnabledForTest() const { return runButton.isEnabled(); }
    juce::String dialogTextForTest() const { return dialog.isVisible() ? dialog.title + " | " + dialog.text : juce::String(); }
    /** true when a click at the centre of "Split the drums" reaches that key (18 Nov: the drum strip lay over it and took the click). */
    bool splitDrumsReceivesClicksForTest();

    /** Presses the gold Run button (snapshot harness). */
    void pressRun() { runSelected(); }

    /** Peak envelope of a file (max |sample| over both channels per point). `seconds` is the file length. */
    struct Peaks { std::vector<float> values; double seconds { 0.0 }; float peak { 0.0f }; float rms { 0.0f }; /* whole file, linear */ };
    static Peaks readPeaks (const juce::File& file, int points);

    /** Fills `path` with a mirrored filled envelope of `peaks` inside `area`; `curve` true = square root against `ref`
        (the lanes: a quiet stem looks quiet), false = linear against the envelope's own peak (the mix). */
    static void envelopePath (juce::Path& path, const std::vector<float>& peaks, juce::Rectangle<float> area, float ref, bool curve,
                              float from = 0.0f, float to = 1.0f);

    /** The round play / pause key of the overview. */
    class PlayButton : public juce::Button
    {
    public:
        PlayButton() : juce::Button ("Play") {}
        bool playing = false;
        void paintButton (juce::Graphics& g, bool over, bool down) override;
    };

    /** The stop key beside play (owner 20 Nov): stops and puts the playhead back at the region's start (or the file's). */
    class StopButton : public juce::Button
    {
    public:
        StopButton() : juce::Button ("Stop") {}
        void paintButton (juce::Graphics& g, bool over, bool down) override;
    };

    /** One lane: colour tab, name, M / S / level, the waveform on the shared time axis, the WAV tab. */
    class StemPanel : public juce::Component, public juce::Timer
    {
    public:
        StemPanel (const juce::String& name, const juce::File& file, juce::Colour themeColor, int index,
                   SubverseSplitterAudioProcessor& processor, juce::DragAndDropContainer& container);
        ~StemPanel() override;

        void paint (juce::Graphics& g) override;
        void resized() override;

        void mouseDown (const juce::MouseEvent& e) override;
        void mouseDrag (const juce::MouseEvent& e) override;
        void mouseUp (const juce::MouseEvent& e) override;

        void timerCallback() override;

        /** The waveform: peaks of this lane's WAV and the common reference (the loudest lane), drawn on a square-root curve. */
        void setPeaks (std::vector<float> newPeaks, float reference);
        float getPeak() const { return peak; }

        /** A drum part row: smaller type, indented tab, the group rail. */
        void setCompact (bool shouldBeCompact) { compact = shouldBeCompact; repaint(); }
        /** false: the row stays in place but empty and disabled. */
        void setHasData (bool hasData);
        /** true: the file holds (almost) nothing, drawn as an empty lane with the word "Silent" instead of a waveform that looks alive. */
        void setSilent (bool isSilent);
        /** The drum group rail runs through this row (x in lane coordinates). */
        void setRail (bool on) { rail = on; repaint(); }

        /** The waveform area in this component's coordinates: exactly the overview's time axis. */
        juce::Rectangle<float> waveArea() const;

    private:
        void seekTo (int x);

        juce::File targetFile;
        juce::Colour themeColor;
        int stemIndex;
        SubverseSplitterAudioProcessor& audioProcessor;
        juce::DragAndDropContainer& dragContainer;

        juce::Slider volumeSlider { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
        juce::TextButton muteButton { "M" };
        juce::TextButton soloButton { "S" };
        juce::TextButton exportButton { "WAV" };

        std::vector<float> peaks;
        float peak { 0.0f }, reference { 1.0f };
        juce::Path wavePath;
        juce::Rectangle<float> pathArea;
        bool dragging { false };
        bool compact { false };
        bool hasData { true };
        bool silent { false };
        bool rail { false };
        bool wired() const { return stemIndex >= 0; }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemPanel)
    };

private:
    enum class View { Empty, Ready, Running, Done, History };      // Empty and Ready are HOME (the tiles); History lists earlier runs
    View view() const;

    juce::Rectangle<int> overviewCard() const;            // the top card (drop zone on the start screen)
    juce::Rectangle<int> dropZone() const;                 // the dashed area of the start screen
    juce::Rectangle<int> optionBounds (int index) const;   // What to make, in the inspector
    juce::Rectangle<int> recentRow (int index) const;      // Earlier separations, in the inspector

    void paintHeader (juce::Graphics&);
    void paintFooter (juce::Graphics&);
    void paintOverview (juce::Graphics&);
    void paintSpineAndPlayhead (juce::Graphics&);
    void paintInspector (juce::Graphics&);
    void paintAnalysisCells (juce::Graphics&, const AudioAnalysis&, bool known);
    void paintOptions (juce::Graphics&);
    void paintRecent (juce::Graphics&, int top);
    void paintWorking (juce::Graphics&);
    void paintFiles (juce::Graphics&);

    bool drumModelInstalled() const;

    void chooseFile();
    void loadFile (const juce::File& file);
    void runSelected();
    void clearResult();
    void buildResult (const ElekLibrary::Entry& entry, bool loadPlayback);   // false: the processor already holds these stems
    void showRunnerResult (bool loadPlayback);                                 // the run that just ended, read back from its elek.json
    void refreshRecent();
    void loadMixPeaksAsync (const juce::File& file);
    double shownLength() const;
    float playheadX() const;
    void repaintPlayhead();

    SubverseSplitterAudioProcessor& audioProcessor;
    Theme::Look look;
    juce::Image markImage;
    // UI signature 3 (owner 14 Nov 2026): the SubverseLab icon, bottom right, opens this product's manual.
    svl::MarkLink markLink { "https://subverselab.com/plugins/elek/manual" };
    juce::Rectangle<float> markArea() const;   // where the footer draws the icon

    juce::File pendingFile;            // loaded, waiting for an operation
    double pendingSeconds { 0.0 };
    int pendingChannels { 0 };
    int selected { -1 };               // the chosen option, -1 = none
    int hoverOption { -1 };
    bool resultShown { false };        // the last run's result is on screen
    Operation shownOp { Operation::Stems };
    juce::String fileName;
    ElekLibrary::Entry shown;              // the result on screen (a run that just ended, or one reopened from the list)
    AudioAnalysis shownAnalysis;
    std::vector<ElekLibrary::Entry> recentRuns;
    int hoverRecent { -1 };
    juce::String errorText;

    // the mix overview: peaks of the loaded file (start / run, read on a worker thread) or of the sum of the stems (result)
    Peaks mixPeaks;
    juce::File mixPeaksFile;
    int mixPeaksToken { 0 };
    std::shared_ptr<std::atomic<bool>> alive { std::make_shared<std::atomic<bool>> (true) };

    // run progress as the engine reports it (ModelRunner::getProgress): 0.1 until the first window is done, then
    // 0.1 + 0.8 * windows done / windows, 0.9..0.99 the drum split of a Drums-only run (0.1..0.99 for Split the drums), 1.0 end
    double rawProgress { 0.0 };
    bool windowsReporting { false };   // the figure has moved off 0.1: it now counts finished windows
    double windowFraction { 0.0 };     // windows done / windows (never moves back)
    juce::File runningFile;

    juce::TextButton changeFileButton { "Change file" };
    juce::TextButton runButton { "Run" };
    juce::TextButton otherOpButton { "Other operation" };
    // NAVIGATION (owner 18 Nov): every screen has a way home; earlier runs are their own screen
    juce::TextButton homeButton { "Home" };                          // header: back to the tiles from a result or from History
    juce::TextButton historyButton { "History" };                    // header: earlier separations, from anywhere but a run
    juce::TextButton cancelButton { "Cancel" };                      // stops a run and returns to where it was started
    juce::TextButton fastButton;                                     // FAST x2 (text set in the constructor: it has a multiplication sign)
    bool historyShown { false }, cancelRequested { false };
    // Tempo and key of the loaded file, measured as soon as it is loaded (the engine's Analysis run, in the background; the
    // window stays on the tiles). needAnalysis: wanted, waits for the engine to be free; autoAnalysis: that run is going.
    bool needAnalysis { false }, autoAnalysis { false };
    AudioAnalysis pendingAnalysis;
    void goHome();
    void showHistory();
    void toggleFast();
    void paintHistory (juce::Graphics&);

    /** A message over the window (in place, never a separate OS window): dims the rest, takes every click until OK. */
    class Dialog : public juce::Component
    {
    public:
        Dialog();
        juce::String title, text;
        void show (const juce::String& newTitle, const juce::String& newText);
        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseDown (const juce::MouseEvent&) override {}      // swallows clicks on the dimmed window
    private:
        juce::Rectangle<int> card() const;
        juce::TextButton ok { "OK" };
    };
    Dialog dialog;
    juce::TextButton showFilesButton { "Show files" };
    juce::TextButton newFileButton { "New file" };      // result screen: open another track (owner 20 Nov)
    juce::TextButton openFolderButton { "Open ELEK folder" };
    juce::TextButton splitDrumsButton { "Split the drums" };   // on the Drums lane of a finished run (owner 18 Nov, rule 3)
    juce::TextButton audioButton { "Audio output" };   // desktop app only (processor.showAudioSettings)
#if SVL_LICENSING
    juce::TextButton licenseButton { "Licence" };
    svl::LicenseOverlay licenseOverlay;
    void showLicence();
#endif
    std::unique_ptr<juce::FileChooser> fileChooser;
    bool isDraggingActive { false };
    bool autoPlayWhenReady { false };   // set only by the SPLITTER_AUTOTEST hook
    bool awaitingResult { false };      // a run was started and its end has not been handled yet
    float lastPlayheadX { -1.0f };
    juce::String lastClock;

    /** The strip under the Drums lane: chevron, DRUM PARTS, state. Click opens and closes the group. */
    class DrumGroupHeader : public juce::Component
    {
    public:
        bool open { false };
        int rightInset { 0 };      // room kept free at the right for the Split the drums key
        juce::String state;
        std::function<void()> onToggle;
        void paint (juce::Graphics& g) override;
        void mouseUp (const juce::MouseEvent& e) override { if (getLocalBounds().contains (e.getPosition()) && onToggle) onToggle(); }
        void mouseEnter (const juce::MouseEvent&) override { repaint(); }
        void mouseExit (const juce::MouseEvent&) override { repaint(); }
    };

    void buildDrumPanels (const std::vector<DrumPart>& parts);
    /** "Split the drums" on a finished run: the DrumSep second stage on that run's Drums stem, parts written into the run. */
    void splitDrums();
    bool drumsCanBeSplit() const;    // a finished run with a Drums lane, the model installed, no parts yet

    // REGION (owner 18 Nov): dragged on the waveform of a loaded file; only that part is separated. Seconds, -1 = none.
    double regionA { -1.0 }, regionB { -1.0 }, regionAnchor { 0.0 };
    bool regionDragging { false };
    // One gesture on the loaded file's waveform: a click seeks the preview, a drag selects, a drag on an edge moves that edge.
    bool regionPress { false };                    // pressed on the waveform, not yet known whether it is a click or a drag
    int regionPressX { 0 };
    double regionKeptA { -1.0 }, regionKeptB { -1.0 };   // the region before this gesture: comes back if the drag ends under a second
    static constexpr int kEdgeGrab = 7;            // px either side of a region edge that take hold of it
    int regionEdgeAt (int x) const;                // 1: start edge, 2: end edge, 0: neither
    /** PREVIEW (owner 18 Nov): the loaded file can be played before anything is separated, so the part to cut can be found by ear. */
    void preparePreview();
    bool previewReady() const;
    bool hasRegion() const { return regionB - regionA >= 1.0; }
    /** owner 20 Nov: the markers are always there, at the file's start and end until moved; that is "no region" for the engine. */
    bool regionIsWholeFile() const { return hasRegion() && regionA <= 0.05 && regionB >= shownLength() - 0.05; }
    void setWholeFileRegion() { const double len = shownLength(); if (len >= 1.0) { regionA = 0.0; regionB = len; } else regionA = regionB = -1.0; }
    double secondsAtX (int x) const;
    void layoutLanes();
    int lanesBottom { 0 };   // the last lane's bottom edge (spine and playhead end there)
    float laneReference { 1.0f };   // the loudest lane's peak: every lane is drawn against it
    float laneReferenceRms { 0.0f };   // the loudest lane's RMS over the whole file (linear): a lane 30 dB or more below it is drawn Silent
    static constexpr float kSilentBelowDb = -30.0f;   // measured 18 Nov (docs/real_instr_test): empty stems sit 34-64 dB below the loudest, real quiet ones 11-24 dB
    bool lastAnalysisValid { false };

    std::vector<std::unique_ptr<StemPanel>> panels;
    int drumsRow { -1 };                          // index in `panels` of the Drums row, -1 when this run has none
    std::vector<DrumPart> drumParts;
    std::vector<std::unique_ptr<StemPanel>> drumPanels;
    DrumGroupHeader drumHeader;
    bool drumGroupOpen { true };
    PlayButton playPauseButton;
    StopButton stopButton;
    juce::uint32 lastPeakLogMs { 0 };
    void stopPlayback();
    int regionEdgeDragged { 0 };   // the gesture in progress: 0 a new region, 1 the start edge, 2 the end edge

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SubverseSplitterAudioProcessorEditor)
};
