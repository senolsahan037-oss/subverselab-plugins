#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Theme.h"
#include "ElekLibrary.h"
#if SVL_LICENSING
 #include "../../subverselab-plugin-common/Licensing/LicenseOverlay.h"
#endif

/*  ELEK's window, laid out as the SubverseLab web tool (subverse-splitter-web: styles.css, main.jsx, operations.py).

    The flow is the web tool's: load a file, choose ONE operation, run it. Nothing starts on drop.
      empty     the card with the dashed drop zone and the gold "Choose file" button
      ready     the file (name, length, "Change file"), the three doors — Key & BPM / Vocal removal / Stem separation —
                and the gold "Run …" button, disabled until a door is chosen
      running   status line, percentage, teal bar
      done      tempo / key tiles for every operation; for the two separations the mixer head (play, clock, file name,
                "Other operation", "New file", gold "Show files") over the seek bar and the stem rows
                (Vocal removal: Vocals + Instrumental; Stem separation: Vocals, Drums, Bass, Other)
*/
class SubverseSplitterAudioProcessorEditor  : public juce::AudioProcessorEditor,
                                              public juce::FileDragAndDropTarget,
                                              public juce::Timer,
                                              public juce::DragAndDropContainer
{
public:
    static constexpr int kW = 1000, kH = 760;
    using Operation = SubverseSplitterAudioProcessor::ModelRunner::Operation;

    SubverseSplitterAudioProcessorEditor (SubverseSplitterAudioProcessor&);
    ~SubverseSplitterAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Where the seek bar is. Drawing it and hit-testing it read the same rectangle. */
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
    void mouseMove (const juce::MouseEvent& e) override;
    void seekFromMouse (juce::Point<int> position, bool startedOnBar);

    /** Picks a door (0 Key & BPM, 1 Vocal removal, 2 Stem separation); public for the snapshot harness. */
    void selectOperation (int index);

    /** Reopens an earlier run from the list (0 = newest); public for the snapshot harness. */
    void openRecent (int index);

    /** Presses the gold Run button (snapshot harness). */
    void pressRun() { runSelected(); }

    /** The round-cornered play / pause key of the mixer head (`.play`): an icon, not a word. */
    class PlayButton : public juce::Button
    {
    public:
        PlayButton() : juce::Button ("Play") {}
        bool playing = false;
        void paintButton (juce::Graphics& g, bool over, bool down) override;
    };

    // One stem row (`.stem-row`)
    class StemPanel : public juce::Component, public juce::Timer
    {
    public:
        StemPanel(const juce::String& name, const juce::File& file, juce::Colour themeColor, int index, SubverseSplitterAudioProcessor& processor, juce::DragAndDropContainer& container);
        ~StemPanel() override;

        void paint(juce::Graphics& g) override;
        void resized() override;

        void mouseDown (const juce::MouseEvent& e) override;
        void mouseDrag (const juce::MouseEvent& e) override;
        void mouseUp (const juce::MouseEvent& e) override;

        void timerCallback() override;
        void generateWaveformPreview();

        /** The loudest point in this stem, before any display scaling. */
        float getPeak() const { return peak; }

        /** Multiplier the waveform is drawn at (each stem against its own peak, capped at 6x). */
        void setDisplayScale (float newScale) { displayScale = newScale; repaint(); }

    private:
        juce::Rectangle<float> waveArea() const;
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

        std::vector<float> waveformData;
        float peak { 0.0f };
        float displayScale { 1.0f };
        bool dragging { false };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StemPanel)
    };

private:
    enum class View { Empty, Ready, Running, Done };
    View view() const;

    juce::Rectangle<int> contentArea() const { return getLocalBounds().withTrimmedTop (kHeader).withTrimmedBottom (kFooter).reduced (kPad, 0).withTrimmedBottom (16); }
    juce::Rectangle<int> analysisArea() const;
    juce::Rectangle<int> cardArea() const;                 // the empty / ready / running card
    juce::Rectangle<int> dropZone() const;
    juce::Rectangle<int> doorBounds (int index) const;
    juce::Rectangle<int> mixerHeadArea() const;
    void paintHeader (juce::Graphics&);
    void paintFooter (juce::Graphics&);
    void paintAnalysis (juce::Graphics&);
    void paintEmpty (juce::Graphics&);
    void paintReady (juce::Graphics&);
    void paintRunning (juce::Graphics&);
    void paintDone (juce::Graphics&);
    void chooseFile();
    void loadFile (const juce::File& file);
    void runSelected();
    void clearResult();
    void buildResult (const ElekLibrary::Entry& entry, bool loadPlayback);   // false: the processor already holds these stems
    void showRunnerResult (bool loadPlayback);                                 // the run that just ended, read back from its elek.json
    void refreshRecent();
    juce::Rectangle<int> recentArea() const;
    juce::Rectangle<int> recentRow (int index) const;
    void paintRecent (juce::Graphics&);

    static constexpr int kHeader = 96, kFooter = 46, kPad = 24, kAnalysisH = 66, kGap = 14, kHeadH = 44, kSeekH = 18;

    SubverseSplitterAudioProcessor& audioProcessor;
    Theme::Look look;
    juce::Image markImage, wordmarkImage;

    juce::File pendingFile;            // loaded, waiting for an operation
    double pendingSeconds { 0.0 };
    int selected { -1 };               // the chosen door, -1 = none
    int hoverDoor { -1 };
    bool resultShown { false };        // the last run's result is on screen
    Operation shownOp { Operation::Stems };
    juce::String fileName;
    ElekLibrary::Entry shown;              // the result on screen (a run that just ended, or one reopened from the list)
    AudioAnalysis shownAnalysis;
    std::vector<ElekLibrary::Entry> recentRuns;
    int hoverRecent { -1 };
    juce::String errorText;

    juce::TextButton chooseButton { "Choose file" };
    juce::TextButton changeFileButton { "Change file" };
    juce::TextButton runButton { "Run" };
    juce::TextButton otherOpButton { "Other operation" };
    juce::TextButton newFileButton { "New file" };
    juce::TextButton showFilesButton { "Show files" };
    juce::TextButton openFolderButton { "Open ELEK folder" };
    juce::TextButton audioButton { "Audio output" };   // desktop app only (processor.showAudioSettings)
#if SVL_LICENSING
    juce::TextButton licenseButton { "Licence" };
    svl::LicenseOverlay licenseOverlay;
    void showLicence();
#endif
    std::unique_ptr<juce::FileChooser> fileChooser;
    bool isDraggingActive{ false };
    bool autoPlayWhenReady { false };   // set only by the SPLITTER_AUTOTEST hook
    bool awaitingResult { false };
    int  lastPct { -1 };                // the running card is repainted only when these change
    bool lastTiles { false };      // a run was started and its end has not been handled yet

    std::vector<std::unique_ptr<StemPanel>> panels;
    PlayButton playPauseButton;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SubverseSplitterAudioProcessorEditor)
};
