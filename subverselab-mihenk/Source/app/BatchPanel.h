#pragma once

#include <JuceHeader.h>
#include <atomic>
#include "Widgets.h"
#include "../engine/Batch.h"

/*  BATCH / ALBUM (app only): a queue of files mastered one after the other on a thread of its own, so the panel,
    the player and the meters stay usable while it runs. SAME FOR ALL gives every track the panel's settings;
    PER TRACK lets each keep its own (STORE PANEL SETTINGS on a selected row) or, when none are stored, the panel's
    settings plus what its own diagnosis finds. ALBUM makes the loudness album-relative and nudges the tilt towards
    the album mean (engine/Batch.h). Everything goes into one folder with a CSV + JSON report. */
namespace mihenk::ui
{
class BatchPanel : public juce::Component, public juce::TooltipClient, private juce::Timer
{
public:
    explicit BatchPanel (Context& c);
    ~BatchPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    juce::String getTooltip() override;

    /** Adds WAV / AIFF files (folders: their audio files) to the queue; each is measured in the background. */
    void addFiles (const juce::Array<juce::File>& files);
    bool isRunning() const;
    int numRows() const noexcept { return (int) rows.size(); }

    std::function<void()> onClose;
    std::function<void (const juce::File&, const Intent*)> onOpenInPanel;
    std::function<int()> exportBits;

    // Tests / snapshot tool
    void setOutputFolder (const juce::File& f) { outDir = f; repaint(); }
    void setModes (bool perTrackMode, bool albumMode);
    void selectRow (int r) { selected = r; repaint(); }
    void storePanelSettingsInSelected();
    void start();
    /** Waits (message loop) until the batch and the background measurements have finished. */
    bool waitForTesting (int timeoutMs);
    const batch::Result& lastResult() const noexcept { return result; }
    /** Snapshot tool: show a batch as if it were half-way. */
    void showProgressForSnapshot (int track, double fraction, const juce::String& stage);

private:
    struct Row
    {
        juce::File file;
        bool own = false;
        Intent intent;                  // the panel's intent when the row was added or stored (the track's own, PER TRACK)
        bool measured = false, measureFailed = false;
        double seconds = 0.0, lufs = kNegInf, truePeak = kNegInf;
        int state = 0;                  // 0 queued, 1 rendering, 2 done, 3 error
        batch::TrackReport report;
    };
    struct Runner;
    void timerCallback() override;
    void chooseFolder();
    void chooseFiles();
    void finished (batch::Result r, std::vector<juce::File> order);
    void updateButtons();
    juce::Rectangle<float> listArea() const;
    int rowAt (juce::Point<float>) const;

    Context& ctx;
    std::vector<Row> rows;
    int selected = -1, scroll = 0;
    bool perTrack = false;
    juce::File outDir;
    CapButton sameCap { "SAME FOR ALL", false }, perCap { "PER TRACK", false }, albumCap { "ALBUM" };
    CapButton addKey { "ADD FILES", false }, removeKey { "REMOVE", false }, clearKey { "CLEAR", false }, folderKey { "CHOOSE...", false };
    CapButton storeKey { "STORE PANEL SETTINGS", false }, openKey { "OPEN IN PANEL", false }, closeKey { "CLOSE", false }, revealKey { "SHOW FOLDER", false };
    KeyButton startKey { "START\nBATCH" };
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<Runner> runner;
    juce::ThreadPool pool { 1 };
    std::atomic<int> pendingMeasures { 0 };
    // Progress, written by the runner, read by the timer.
    std::atomic<double> progress { 0.0 };
    std::atomic<int> currentTrack { -1 };
    juce::CriticalSection stageLock;
    juce::String stage;
    batch::Result result;
    bool snapshotProgress = false;
    bool lastAlbum = false;
    bool undoReady = false;            // the constructor's own setModes is not an undo step

    JUCE_DECLARE_WEAK_REFERENCEABLE (BatchPanel)
};
} // namespace mihenk::ui
