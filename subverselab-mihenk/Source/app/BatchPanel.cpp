#include "BatchPanel.h"
#include "Theme.h"
#include "../engine/AudioIO.h"

namespace mihenk::ui
{
namespace
{
constexpr float kPad = 14.0f, kHeader = 32.0f, kRightX = 820.0f, kRowH = 19.0f;
juce::String upper (const juce::String& s) { return s.toUpperCase(); }
bool isAudio (const juce::File& f) { return f.hasFileExtension ("wav;aif;aiff"); }
} // namespace

struct BatchPanel::Runner : public juce::Thread
{
    Runner (BatchPanel& p, std::vector<batch::Job> j, batch::Options o, std::vector<juce::File> ord)
        : juce::Thread ("MIHENK batch"), panel (&p), jobs (std::move (j)), options (std::move (o)), order (std::move (ord)) {}
    ~Runner() override { cancel = true; stopThread (60000); }

    void run() override
    {
        auto* p = panel.get();
        if (p == nullptr) return;
        auto& prog = p->progress;
        auto& cur = p->currentTrack;
        auto& lock = p->stageLock;
        auto& stage = p->stage;
        auto res = batch::run (jobs, options, [&] (int i, int, double f, const juce::String& st)
        {
            prog.store (f);
            cur.store (i);
            const juce::ScopedLock sl (lock);
            stage = st;
        }, &cancel);
        juce::WeakReference<BatchPanel> weak = panel;
        auto shared = std::make_shared<batch::Result> (std::move (res));
        auto ord = order;
        juce::MessageManager::callAsync ([weak, shared, ord] { if (auto* bp = weak.get()) bp->finished (*shared, ord); });
        done = true;
    }

    juce::WeakReference<BatchPanel> panel;
    std::vector<batch::Job> jobs;
    batch::Options options;
    std::vector<juce::File> order;
    std::atomic<bool> cancel { false }, done { false };
};

BatchPanel::BatchPanel (Context& c) : ctx (c)
{
    for (auto* b : { &sameCap, &perCap, &albumCap, &addKey, &removeKey, &clearKey, &folderKey, &storeKey, &openKey, &closeKey, &revealKey })
        addAndMakeVisible (*b);
    addAndMakeVisible (startKey);
    for (auto* b : { &addKey, &removeKey, &clearKey, &folderKey, &storeKey, &openKey, &closeKey, &revealKey }) b->showLed = false;
    outDir = juce::File::getSpecialLocation (juce::File::userDesktopDirectory).getChildFile ("MIHENK masters");
    sameCap.onClick = [this] { setModes (false, albumCap.getToggleState()); };
    perCap.onClick = [this] { setModes (true, albumCap.getToggleState()); };
    albumCap.onClick = [this] { setModes (perTrack, albumCap.getToggleState()); };
    addKey.onClick = [this] { chooseFiles(); };
    folderKey.onClick = [this] { chooseFolder(); };
    revealKey.onClick = [this] { if (outDir.isDirectory()) outDir.revealToUser(); };
    removeKey.onClick = [this]
    {
        if (isRunning() || selected < 0 || selected >= (int) rows.size()) return;
        rows.erase (rows.begin() + selected);
        selected = juce::jmin (selected, (int) rows.size() - 1);
        updateButtons();
        repaint();
    };
    clearKey.onClick = [this] { if (isRunning()) return; rows.clear(); selected = -1; scroll = 0; result = {}; updateButtons(); repaint(); };
    storeKey.onClick = [this] { storePanelSettingsInSelected(); };
    openKey.onClick = [this]
    {
        if (selected < 0 || selected >= (int) rows.size() || ! onOpenInPanel) return;
        const auto& r = rows[(size_t) selected];
        onOpenInPanel (r.file, r.own ? &r.intent : nullptr);
    };
    closeKey.onClick = [this] { if (onClose) onClose(); };
    startKey.onClick = [this]
    {
        if (isRunning()) { runner->cancel = true; ctx.session.status = "Batch: cancelling after the current step ..."; ctx.session.sendChangeMessage(); return; }
        start();
    };
    sameCap.setTooltip ("Every track is mastered with the panel's current settings");
    perCap.setTooltip ("Every track keeps its own settings: STORE PANEL SETTINGS on a selected row, or (nothing stored) the panel's settings plus the scenarios its own diagnosis finds");
    albumCap.setTooltip ("ALBUM: the loudest track reaches the target, the others sit below it by half of their distance below the loudest (at most 3 LU) and each reaches its own place, "
                         "and each track's tilt is nudged half-way to the album mean (at most 0.5 dB/oct)");
    storeKey.setTooltip ("PER TRACK: the selected track keeps the panel's current settings (tweak it with OPEN IN PANEL first)");
    openKey.setTooltip ("Load the selected track in the main panel (with its stored settings) to listen and tweak");
    startKey.setTooltip ("Master the queue in the background; the panel stays usable. Press again to cancel");
    setModes (false, false);
    undoReady = true;
    startTimerHz (10);
}

BatchPanel::~BatchPanel()
{
    stopTimer();
    runner.reset();
    pool.removeAllJobs (true, 20000);
}

bool BatchPanel::isRunning() const { return runner != nullptr && runner->isThreadRunning() && ! runner->done.load(); }

void BatchPanel::setModes (bool perTrackMode, bool albumMode)
{
    if (perTrackMode != perTrack || albumMode != lastAlbum)
    {
        // The batch's per-song settings are part of the undo history too (the modes: SAME FOR ALL / PER TRACK, ALBUM).
        const bool wasPer = perTrack, wasAlbum = lastAlbum;
        juce::WeakReference<BatchPanel> weak (this);
        auto put = [weak] (bool per, bool album)
        {
            auto* p = weak.get();
            if (p == nullptr || p->isRunning()) return;
            p->perTrack = per;
            p->sameCap.setToggleState (! per, juce::dontSendNotification);
            p->perCap.setToggleState (per, juce::dontSendNotification);
            p->albumCap.setToggleState (album, juce::dontSendNotification);
            p->lastAlbum = album;
            p->updateButtons();
            p->repaint();
        };
        const juce::String name = perTrackMode != wasPer ? (perTrackMode ? "Batch: PER TRACK" : "Batch: SAME FOR ALL") : (albumMode ? "Batch: ALBUM on" : "Batch: ALBUM off");
        if (undoReady)
            ctx.session.pushUndoAction (name, [put, wasPer, wasAlbum] { put (wasPer, wasAlbum); }, [put, perTrackMode, albumMode] { put (perTrackMode, albumMode); });
    }
    perTrack = perTrackMode;
    lastAlbum = albumMode;
    sameCap.setToggleState (! perTrack, juce::dontSendNotification);
    perCap.setToggleState (perTrack, juce::dontSendNotification);
    albumCap.setToggleState (albumMode, juce::dontSendNotification);
    updateButtons();
    repaint();
}

void BatchPanel::updateButtons()
{
    const bool run = isRunning(), sel = selected >= 0 && selected < (int) rows.size();
    storeKey.setEnabled (perTrack && sel && ! run);
    openKey.setEnabled (sel);
    removeKey.setEnabled (sel && ! run);
    clearKey.setEnabled (! rows.empty() && ! run);
    addKey.setEnabled (! run);
    folderKey.setEnabled (! run);
    sameCap.setEnabled (! run);
    perCap.setEnabled (! run);
    albumCap.setEnabled (! run);
    revealKey.setEnabled (outDir.isDirectory());
    startKey.setEnabled (run || ! rows.empty());
    startKey.setButtonText (run ? "CANCEL\nBATCH" : "START\nBATCH");
    startKey.setToggleState (run, juce::dontSendNotification);
    startKey.blink = run;
}

void BatchPanel::storePanelSettingsInSelected()
{
    if (selected < 0 || selected >= (int) rows.size()) return;
    auto& r = rows[(size_t) selected];
    {
        // One undo step: this song's own settings (the intent stored on its row) go back to what they were.
        juce::WeakReference<BatchPanel> weak (this);
        const auto file = r.file;
        const bool wasOwn = r.own;
        const Intent was = r.intent, now = ctx.session.intent;
        auto put = [weak, file] (bool own, const Intent& in)
        {
            auto* p = weak.get();
            if (p == nullptr || p->isRunning()) return;
            for (auto& row : p->rows)
                if (row.file == file) { row.own = own; row.intent = in; }
            p->repaint();
        };
        ctx.session.pushUndoAction ("Batch: store settings for " + r.file.getFileNameWithoutExtension(),
                                    [put, wasOwn, was] { put (wasOwn, was); }, [put, now] { put (true, now); });
    }
    r.own = true;
    r.intent = ctx.session.intent;
    ctx.session.status = "Batch: " + r.file.getFileName() + " keeps the panel's current intent (target, ceiling, character, overrides).";
    ctx.session.sendChangeMessage();
    repaint();
}

void BatchPanel::addFiles (const juce::Array<juce::File>& files)
{
    juce::Array<juce::File> list;
    for (auto& f : files)
    {
        if (f.isDirectory())
        {
            juce::Array<juce::File> inDir;
            for (const auto& e : juce::RangedDirectoryIterator (f, false, "*.wav;*.aif;*.aiff", juce::File::findFiles)) inDir.add (e.getFile());
            inDir.sort();
            list.addArray (inDir);
        }
        else if (isAudio (f) && f.existsAsFile()) list.add (f);
    }
    juce::WeakReference<BatchPanel> weak (this);
    for (auto& f : list)
    {
        if (std::any_of (rows.begin(), rows.end(), [&] (const Row& r) { return r.file == f; })) continue;
        Row r;
        r.file = f;
        r.intent = ctx.session.intent;
        rows.push_back (r);
        ++pendingMeasures;
        pool.addJob ([weak, f]
        {
            auto a = loadAudioFile (f);
            auto m = std::make_shared<Measurement>();
            const bool ok = a.ok();
            const double secs = ok ? a.buffer.getNumSamples() / a.sampleRate : 0.0;
            if (ok) *m = analyse (a.buffer, a.sampleRate);
            juce::MessageManager::callAsync ([weak, f, m, ok, secs]
            {
                auto* p = weak.get();
                if (p == nullptr) return;
                --p->pendingMeasures;
                for (auto& r : p->rows)
                    if (r.file == f)
                    {
                        r.measured = ok;
                        r.measureFailed = ! ok;
                        r.seconds = secs;
                        r.lufs = m->integrated;
                        r.truePeak = m->truePeak;
                    }
                p->repaint();
            });
        });
    }
    if (selected < 0 && ! rows.empty()) selected = 0;
    ctx.session.status = "Batch queue: " + juce::String ((int) rows.size()) + " track(s).";
    ctx.session.sendChangeMessage();
    updateButtons();
    repaint();
}

void BatchPanel::start()
{
    if (isRunning() || rows.empty()) return;
    if (! outDir.isDirectory() && ! outDir.createDirectory())
    {
        ctx.session.status = "Batch: cannot create " + outDir.getFullPathName();
        ctx.session.sendChangeMessage();
        return;
    }
    std::vector<batch::Job> jobs;
    std::vector<juce::File> order;
    for (auto& r : rows)
    {
        batch::Job j;
        j.file = r.file;
        j.ownSettings = perTrack && r.own;
        j.settings = ctx.session.settings;
        j.intent = j.ownSettings ? r.intent : ctx.session.intent;
        j.hasIntent = true;
        j.applyDiagnosis = perTrack && ! r.own;
        // The panel's tone-match curve belongs to the panel's source; a batch track never inherits it silently.
        if (! j.ownSettings) j.intent.referenceCurve.clear();
        jobs.push_back (j);
        order.push_back (r.file);
        r.state = 0;
        r.report = {};
    }
    batch::Options o;
    o.album = albumCap.getToggleState();
    o.outDir = outDir;
    o.bits = exportBits ? exportBits() : 24;
    progress = 0.0;
    currentTrack = 0;
    result = {};
    runner = std::make_unique<Runner> (*this, std::move (jobs), o, std::move (order));
    runner->startThread (juce::Thread::Priority::low);
    ctx.session.status = "Batch started: " + juce::String ((int) rows.size()) + " track(s)" + (o.album ? ", ALBUM" : "") + (perTrack ? ", PER TRACK" : ", SAME FOR ALL")
                         + " -> " + outDir.getFullPathName();
    ctx.session.sendChangeMessage();
    updateButtons();
    repaint();
}

void BatchPanel::finished (batch::Result r, std::vector<juce::File> order)
{
    result = std::move (r);
    for (size_t i = 0; i < result.tracks.size() && i < order.size(); ++i)
        for (auto& row : rows)
            if (row.file == order[i])
            {
                row.report = result.tracks[i];
                row.state = result.tracks[i].ok ? 2 : (result.tracks[i].error.isNotEmpty() ? 3 : 0);
            }
    ctx.session.status = result.message;
    ctx.session.sendChangeMessage();
    updateButtons();
    repaint();
}

bool BatchPanel::waitForTesting (int timeoutMs)
{
   #if JUCE_MODAL_LOOPS_PERMITTED
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
    while (juce::Time::getMillisecondCounter() < end)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
        const bool batchDone = runner == nullptr || (runner->done.load() && (! result.tracks.empty() || result.cancelled || result.message.isNotEmpty()));
        if (! isRunning() && pendingMeasures.load() == 0 && batchDone) return true;
    }
   #else
    juce::ignoreUnused (timeoutMs);
   #endif
    return false;
}

void BatchPanel::showProgressForSnapshot (int track, double fraction, const juce::String& st)
{
    snapshotProgress = true;
    currentTrack = track;
    progress = fraction;
    {
        const juce::ScopedLock sl (stageLock);
        stage = st;
    }
    for (int i = 0; i < (int) rows.size(); ++i) rows[(size_t) i].state = i < track ? rows[(size_t) i].state : i == track ? 1 : 0;
    startKey.setButtonText ("CANCEL\nBATCH");
    startKey.setToggleState (true, juce::dontSendNotification);
    repaint();
}

void BatchPanel::timerCallback()
{
    const bool run = isRunning();
    if (run)
    {
        const int t = currentTrack.load();
        for (int i = 0; i < (int) rows.size(); ++i)
            if (rows[(size_t) i].state < 2) rows[(size_t) i].state = i == t ? 1 : 0;
        repaint();
    }
    if (run != startKey.getToggleState() && ! snapshotProgress) updateButtons();
    if (run) startKey.repaint();
}

void BatchPanel::chooseFiles()
{
    chooser = std::make_unique<juce::FileChooser> ("Add mixes to the batch queue", ctx.session.inputFile.getParentDirectory(), "*.wav;*.aif;*.aiff");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectMultipleItems
                              | juce::FileBrowserComponent::canSelectDirectories,
                          [this] (const juce::FileChooser& fc) { addFiles (fc.getResults()); });
}

void BatchPanel::chooseFolder()
{
    chooser = std::make_unique<juce::FileChooser> ("Folder for the masters and the report", outDir);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this] (const juce::FileChooser& fc) { if (fc.getResult() != juce::File()) { outDir = fc.getResult(); updateButtons(); repaint(); } });
}

juce::Rectangle<float> BatchPanel::listArea() const { return { kPad, kHeader + 8.0f, kRightX - kPad - 12.0f, (float) getHeight() - kHeader - 8.0f - kPad }; }

int BatchPanel::rowAt (juce::Point<float> p) const
{
    const auto a = listArea().reduced (8.0f, 6.0f).withTrimmedTop (16.0f);
    if (! a.contains (p)) return -1;
    const int r = scroll + (int) ((p.y - a.getY()) / kRowH);
    return r < (int) rows.size() ? r : -1;
}

void BatchPanel::mouseDown (const juce::MouseEvent& e)
{
    const int r = rowAt (e.position);
    if (r >= 0) { selected = r; updateButtons(); repaint(); }
}

void BatchPanel::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    const int visible = (int) ((listArea().getHeight() - 28.0f) / kRowH);
    scroll = juce::jlimit (0, juce::jmax (0, (int) rows.size() - visible), scroll + (w.deltaY < 0 ? 1 : -1));
    repaint();
}

juce::String BatchPanel::getTooltip()
{
    const int r = rowAt (getMouseXYRelative().toFloat());
    if (r < 0) return "Drop WAV / AIFF files or a folder anywhere on the panel to queue them";
    const auto& row = rows[(size_t) r];
    juce::String t = row.file.getFullPathName();
    if (row.state == 2) t << "\n" << row.report.loudness << "\nOutput: " << row.report.output.getFileName();
    if (row.state == 3) t << "\nError: " << row.report.error;
    if (! row.report.warnings.isEmpty()) t << "\n" << row.report.warnings.joinIntoString ("\n");
    return t;
}

void BatchPanel::resized()
{
    const int x = (int) kRightX;
    const int w = getWidth() - x - (int) kPad;
    closeKey.setBounds (getWidth() - (int) kPad - 70, 8, 70, 20);
    sameCap.setBounds (x, 58, 128, 22);
    perCap.setBounds (x + 132, 58, 110, 22);
    albumCap.setBounds (x + 246, 58, w - 246, 22);
    folderKey.setBounds (x + w - 150, 108, 72, 22);
    revealKey.setBounds (x + w - 74, 108, 74, 22);
    addKey.setBounds (x, 154, 96, 22);
    removeKey.setBounds (x + 100, 154, 80, 22);
    clearKey.setBounds (x + 184, 154, 70, 22);
    storeKey.setBounds (x, 182, 172, 22);
    openKey.setBounds (x + 176, 182, 120, 22);
    startKey.setBounds (x + w - 104, getHeight() - (int) kPad - 76, 104, 76);
}

void BatchPanel::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    g.setColour (juce::Colours::black.withAlpha (0.55f));
    g.fillRoundedRectangle (b.translated (0.0f, 3.0f), 7.0f);
    g.setGradientFill (juce::ColourGradient (Theme::panel.brighter (0.22f), 0.0f, 0.0f, Theme::panelDeep, 0.0f, b.getBottom(), false));
    g.fillRoundedRectangle (b, 7.0f);
    g.setColour (Theme::gold.withAlpha (0.85f));
    g.drawRoundedRectangle (b.reduced (0.5f), 7.0f, 1.2f);
    g.setColour (Theme::gold.withAlpha (0.25f));
    g.drawRoundedRectangle (b.reduced (4.0f), 5.0f, 0.8f);

    g.setColour (Theme::ink);
    tracked (g, "BATCH / ALBUM", Theme::legend (10.5f), { kPad, 9.0f, 150.0f, 20.0f }, juce::Justification::centredLeft, 0.18f);
    g.setColour (Theme::ink.withAlpha (0.6f));
    tracked (g, "DROP FILES OR A FOLDER ON THE PANEL. MASTERED ONE BY ONE IN THE BACKGROUND; KEEP WORKING MEANWHILE", Theme::fine (7.5f),
             { kPad + 150.0f, 9.0f, (float) closeKey.getX() - kPad - 160.0f, 20.0f }, juce::Justification::centredLeft, 0.12f);
    g.setColour (Theme::ink.withAlpha (0.18f));
    g.drawHorizontalLine ((int) kHeader + 2, kPad, b.getWidth() - kPad);

    // ---- The queue ------------------------------------------------------------------------------------------------
    const auto la = listArea();
    vfdGlass (g, la);
    auto inner = la.reduced (8.0f, 6.0f);
    const float cols[] { 22.0f, 250.0f, 48.0f, 62.0f, 56.0f, 104.0f, 0.0f };
    auto head = inner.removeFromTop (14.0f);
    const char* names[] { "#", "FILE", "LENGTH", "IN LUFS", "TP", "SETTINGS", "RESULT" };
    {
        auto h = head;
        for (int c = 0; c < 7; ++c)
        {
            auto r = c < 6 ? h.removeFromLeft (cols[c]) : h;
            vfdText (g, names[c], r, c >= 2 && c <= 4 ? juce::Justification::centredRight : juce::Justification::centredLeft, 7.0f, Theme::vfd.withAlpha (0.5f));
            if (c >= 2 && c <= 4) h.removeFromLeft (c == 4 ? 16.0f : 6.0f);
        }
    }
    inner.removeFromTop (2.0f);
    if (rows.empty())
    {
        vfdText (g, "THE QUEUE IS EMPTY", inner.withTrimmedBottom (inner.getHeight() * 0.5f), juce::Justification::centredBottom, 13.0f, Theme::vfd.withAlpha (0.5f));
        vfdText (g, "DROP MIXES OR A FOLDER HERE, OR PRESS ADD FILES", inner.withTrimmedTop (inner.getHeight() * 0.5f + 6.0f), juce::Justification::centredTop, 9.0f, Theme::vfd.withAlpha (0.4f));
    }
    const int visible = (int) (inner.getHeight() / kRowH);
    for (int i = scroll; i < (int) rows.size() && i < scroll + visible; ++i)
    {
        const auto& r = rows[(size_t) i];
        auto line = inner.removeFromTop (kRowH);
        if (i == selected)
        {
            g.setColour (Theme::vfd.withAlpha (0.10f));
            g.fillRoundedRectangle (line.expanded (3.0f, 0.0f), 2.0f);
        }
        const auto col = r.state == 3 ? Theme::coral : r.state == 1 ? Theme::lamp : Theme::vfd;
        auto cell = [&] (int c) { auto x = line.removeFromLeft (cols[c]); if (c >= 2 && c <= 4) line.removeFromLeft (c == 4 ? 16.0f : 6.0f); return x; };
        vfdText (g, juce::String (i + 1), cell (0), juce::Justification::centredLeft, 8.5f, Theme::vfd.withAlpha (0.6f));
        vfdText (g, upper (r.file.getFileName()), cell (1).withTrimmedRight (8.0f), juce::Justification::centredLeft, 8.5f, col);
        vfdText (g, r.measured ? timeText (r.seconds) : r.measureFailed ? juce::String ("--") : juce::String ("..."), cell (2), juce::Justification::centredRight, 8.5f, Theme::vfd.withAlpha (0.8f));
        vfdText (g, r.measured ? fmt (r.lufs) : juce::String ("..."), cell (3), juce::Justification::centredRight, 8.5f, Theme::vfd);
        vfdText (g, r.measured ? fmt (r.truePeak, 1) : juce::String ("..."), cell (4), juce::Justification::centredRight, 8.5f,
                 r.measured && r.truePeak > -1.0 ? Theme::coral.withAlpha (0.9f) : Theme::vfd.withAlpha (0.8f));
        vfdText (g, perTrack ? (r.own ? juce::String ("OWN (STORED)") : juce::String ("PANEL + DIAG")) : juce::String ("PANEL"), cell (5), juce::Justification::centredLeft, 8.0f,
                 perTrack && r.own ? Theme::lamp : Theme::vfd.withAlpha (0.7f));
        juce::String res;
        if (r.state == 1) res = "RENDERING " + juce::String (juce::roundToInt (progress.load() * 100.0)) + " %";
        else if (r.state == 2)
            res = fmt (r.report.in.integrated) + " > " + fmt (r.report.out.integrated) + " LUFS  TP " + fmt (r.report.out.truePeak, 1) + "  LRA " + fmt (r.report.out.lra)
                  + (albumCap.getToggleState() || std::abs (r.report.albumOffsetLu) > 0.0 ? "  ALB " + fmt (r.report.albumOffsetLu, 1) : juce::String());
        else if (r.state == 3) res = "ERROR: " + upper (r.report.error);
        else res = "QUEUED";
        vfdText (g, res, line, juce::Justification::centredLeft, 8.5f, r.state == 2 ? Theme::lamp : col.withAlpha (r.state == 0 ? 0.5f : 1.0f));
    }
    if ((int) rows.size() > visible)
        vfdText (g, juce::String (scroll + 1) + "-" + juce::String (juce::jmin ((int) rows.size(), scroll + visible)) + " OF " + juce::String ((int) rows.size()) + " (SCROLL)",
                 la.reduced (10.0f, 4.0f).removeFromBottom (10.0f), juce::Justification::bottomRight, 7.0f, Theme::vfd.withAlpha (0.5f));
    vfdGlassTop (g, la);

    // ---- Controls ---------------------------------------------------------------------------------------------------
    const float x = kRightX, w = b.getWidth() - kRightX - kPad;
    g.setColour (Theme::ink);
    tracked (g, "SETTINGS", Theme::legend (7.5f), { x, 42.0f, 120.0f, 12.0f }, juce::Justification::centredLeft, 0.16f);
    g.setColour (Theme::ink.withAlpha (0.55f));
    tracked (g, albumCap.getToggleState() ? "ALBUM: RELATIVE LOUDNESS + TILT TOWARDS THE ALBUM MEAN" : (perTrack ? "EACH TRACK ITS OWN (OR ITS DIAGNOSIS)" : "EVERY TRACK: THE PANEL'S SETTINGS"),
             Theme::fine (7.0f), { x + 70.0f, 42.0f, w - 70.0f, 12.0f }, juce::Justification::centredRight, 0.08f);
    g.setColour (Theme::ink);
    tracked (g, "OUTPUT FOLDER", Theme::legend (7.5f), { x, 92.0f, 140.0f, 12.0f }, juce::Justification::centredLeft, 0.16f);
    const auto folderBox = juce::Rectangle<float> (x, 106.0f, (float) folderKey.getX() - x - 6.0f, 26.0f);
    vfdGlass (g, folderBox);
    vfdText (g, upper (outDir.getFullPathName().replace (juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName(), "~")),
             folderBox.reduced (8.0f, 2.0f), juce::Justification::centredLeft, 8.0f, Theme::vfd);
    vfdGlassTop (g, folderBox);
    g.setColour (Theme::ink);
    tracked (g, "QUEUE", Theme::legend (7.5f), { x, 140.0f, 120.0f, 12.0f }, juce::Justification::centredLeft, 0.16f);

    // Progress and summary.
    const auto pBox = juce::Rectangle<float> (x, 214.0f, w - 112.0f, b.getHeight() - 214.0f - kPad);
    vfdGlass (g, pBox);
    auto pa = pBox.reduced (10.0f, 8.0f);
    juce::String st;
    {
        const juce::ScopedLock sl (stageLock);
        st = stage;
    }
    const bool run = isRunning() || snapshotProgress;
    const int exported = (int) std::count_if (rows.begin(), rows.end(), [] (const Row& r) { return r.state == 2; });
    vfdText (g, run ? "RUNNING  " + juce::String (currentTrack.load() + 1) + " / " + juce::String ((int) rows.size()) : result.tracks.empty() ? juce::String ("READY") : juce::String ("FINISHED"),
             pa.removeFromTop (14.0f), juce::Justification::centredLeft, 10.0f, run ? Theme::lamp : Theme::vfd);
    vfdText (g, upper (run ? st : result.message.isNotEmpty() ? result.message : juce::String (juce::String ((int) rows.size()) + " TRACK(S) QUEUED, " + juce::String (exportBits ? exportBits() : 24) + "-BIT WAV + CSV / JSON REPORT")),
             pa.removeFromTop (13.0f), juce::Justification::centredLeft, 8.0f, Theme::vfd.withAlpha (0.8f));
    auto bar = pa.removeFromTop (12.0f).withTrimmedTop (4.0f).withHeight (5.0f);
    g.setColour (Theme::vfd.withAlpha (0.12f));
    g.fillRect (bar);
    g.setColour (Theme::lamp);
    g.fillRect (bar.withWidth (bar.getWidth() * (float) (run ? progress.load() : result.tracks.empty() ? 0.0 : 1.0)));
    pa.removeFromTop (4.0f);
    if (! result.tracks.empty() && ! run)
    {
        double lo = 100.0, hi = -100.0;
        for (auto& t : result.tracks) if (t.ok) { lo = juce::jmin (lo, t.out.integrated); hi = juce::jmax (hi, t.out.integrated); }
        vfdText (g, juce::String (exported) + " EXPORTED" + (exported > 0 ? ", MASTERS " + fmt (lo) + " .. " + fmt (hi) + " LUFS" : juce::String()),
                 pa.removeFromTop (12.0f), juce::Justification::centredLeft, 8.0f, Theme::lamp);
        vfdText (g, "REPORT: " + upper (result.csv.getFileName()) + " + .JSON", pa.removeFromTop (12.0f), juce::Justification::centredLeft, 7.5f, Theme::vfd.withAlpha (0.7f));
    }
    vfdGlassTop (g, pBox);
}
} // namespace mihenk::ui
