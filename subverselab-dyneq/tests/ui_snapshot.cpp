/*  Draws the panel (LAB; the ORBIT face was removed on 8 Nov 2026) into PNGs without a screen, after real audio has gone through the real processor, so every curve, meter,
    spectrum and read-out is what the engine measured (nothing is faked).

        ui_snapshot [outdir] [demo_material_dir]     (default docs/ui, tools/demo_material, relative to the project)

    Writes outdir/p4d_<state>.png (1x) and p4d_<state>@2x.png (2x) (p4d = the LAB redesign of 11 Oct; the p4c_ files are the earlier LAB). The states are set through the panel's own controller (the same calls the mouse makes)
    and through the parameters. Exit code 0 only if every check held. */
#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"
#include <cstdio>

namespace
{
using Audio = std::vector<std::vector<float>>;
int gFail = 0;
void check (bool ok, const juce::String& what) { std::printf ("[%s] %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8()); if (! ok) ++gFail; }

Audio readWav (const juce::File& f)
{
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
    if (! r) { std::printf ("cannot read %s\n", f.getFullPathName().toRawUTF8()); std::exit (2); }
    juce::AudioBuffer<float> b ((int) r->numChannels, (int) r->lengthInSamples);
    r->read (&b, 0, (int) r->lengthInSamples, 0, true, true);
    Audio a ((size_t) b.getNumChannels());
    for (int c = 0; c < b.getNumChannels(); ++c) a[(size_t) c].assign (b.getReadPointer (c), b.getReadPointer (c) + b.getNumSamples());
    return a;
}

void setParam (juce::AudioProcessorValueTreeState& s, const char* id, float v)
{
    auto* p = s.getParameter (id);
    p->setValueNotifyingHost (p->convertTo0to1 (v));
}

// pink noise (Paul Kellet's filter), stereo, the same noise on both sides, about -20 dBFS rms
Audio pinkNoise (double seconds, double sr = 48000.0)
{
    const size_t n = (size_t) (seconds * sr);
    Audio a (2, std::vector<float> (n));
    juce::Random rnd (4242);
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
    for (size_t i = 0; i < n; ++i)
    {
        const double w = (double) rnd.nextFloat() * 2.0 - 1.0;
        b0 = 0.99886 * b0 + w * 0.0555179; b1 = 0.99332 * b1 + w * 0.0750759; b2 = 0.96900 * b2 + w * 0.1538520; b3 = 0.86650 * b3 + w * 0.3104856;
        b4 = 0.55000 * b4 + w * 0.5329522; b5 = -0.7616 * b5 - w * 0.0168980;
        const double pink = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362; b6 = w * 0.115926;
        const float v = (float) (pink * 0.05);
        a[0][i] = a[1][i] = v;
    }
    return a;
}

size_t gPos = 0;   // where in the programme the next play() continues
void play (DyneqAudioProcessor& proc, DyneqEditor& ed, const Audio& in, const Audio& key, int seconds)
{
    const size_t n = juce::jmin (in[0].size(), gPos + (size_t) seconds * 48000);
    juce::AudioBuffer<float> buf (4, 512);
    juce::MidiBuffer midi;
    int sinceTick = 0;
    for (; gPos < n; gPos += 512)
    {
        const int len = (int) juce::jmin ((size_t) 512, n - gPos);
        buf.setSize (4, len, false, false, true);
        buf.clear();
        for (int c = 0; c < 2; ++c)
        {
            std::copy (in[(size_t) c].begin() + (long) gPos, in[(size_t) c].begin() + (long) gPos + len, buf.getWritePointer (c));
            if (! key.empty()) std::copy (key[(size_t) c].begin() + (long) gPos, key[(size_t) c].begin() + (long) gPos + len, buf.getWritePointer (2 + c));
        }
        proc.processBlock (buf, midi);
        sinceTick += len;
        if (sinceTick >= 1200) { ed.tickForSnapshot(); sinceTick = 0; }
    }
    ed.tickForSnapshot(); ed.tickForSnapshot();
}

bool save (DyneqEditor& ed, const juce::File& dir, const juce::String& state)
{
    bool ok = true;
    for (int x : { 1, 2 })
    {
        const auto file = dir.getChildFile ("p4d_" + state + (x == 2 ? "@2x" : "") + ".png");
        file.deleteFile();
        juce::FileOutputStream stream (file);
        const auto image = ed.createComponentSnapshot (ed.getLocalBounds(), true, (float) x);
        ok = ok && stream.openedOk() && juce::PNGImageFormat().writeImageToStream (image, stream);
        std::printf ("  %s  %dx%d\n", file.getFullPathName().toRawUTF8(), image.getWidth(), image.getHeight());
    }
    return ok;
}

void shot (DyneqEditor& ed, const juce::File& out, const juce::String& state) { ed.refreshFace(); ed.tickForSnapshot(); check (save (ed, out, state), state + " written"); }
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    const auto cwd = juce::File::getCurrentWorkingDirectory();
    const juce::File out = argc > 1 ? juce::File (argv[1]) : cwd.getChildFile ("docs/ui");
    const juce::File dir = argc > 2 ? juce::File (argv[2]) : cwd.getChildFile ("tools/demo_material");
    out.createDirectory();
    const auto beat = readWav (dir.getChildFile ("beat.wav")), vocal = readWav (dir.getChildFile ("vocal.wav"));
    struct Play : juce::AudioPlayHead { juce::Optional<PositionInfo> getPosition() const override { PositionInfo i; i.setIsPlaying (true); return i; } } head;

    auto makeProc = [&] (DyneqAudioProcessor& proc)
    {
        auto layout = proc.getBusesLayout();
        layout.inputBuses.set (1, juce::AudioChannelSet::stereo());
        if (! proc.setBusesLayout (layout)) { std::printf ("setBusesLayout refused\n"); std::exit (2); }
        proc.setRateAndBufferSizeDetails (48000.0, 512);
        proc.prepareToPlay (48000.0, 512);
        proc.setPlayHead (&head);
    };

    // ---- 1. the default state with pink noise: the analyzer shows real audio (no band yet: the hint is on the glass)
    {
        DyneqAudioProcessor proc; makeProc (proc);
        const auto pink = pinkNoise (4.0);
        DyneqEditor ed (proc);
        gPos = 0; play (proc, ed, pink, {}, 3);
        const auto& sp = ed.spectrum();
        check (panel::Spectrum::range (sp.post) > 20.0f && panel::Spectrum::range (sp.pre) < 0.5f, "pink noise, the default analyzer: the POST trace is live (range " + juce::String (panel::Spectrum::range (sp.post), 1) + " dB), PRE is off (one trace by default)");
        shot (ed, out, "lab_default");
        // the SubverseLab mark, bottom right of the display, at 4x: icon only (no wordmark), inside the display, on the frequency scale
        {
            const auto area = ed.testLockupArea();
            const auto crop = area.expanded (40.0f, 10.0f).getSmallestIntegerContainer().getIntersection (ed.getLocalBounds());
            const auto img = ed.createComponentSnapshot (crop, true, 4.0f);
            const auto file = out.getChildFile ("p4d_lab_mark@4x.png"); file.deleteFile();
            bool ok = false; { juce::FileOutputStream st (file); ok = st.openedOk() && juce::PNGImageFormat().writeImageToStream (img, st); }
            std::printf ("  %s  %dx%d\n", file.getFullPathName().toRawUTF8(), img.getWidth(), img.getHeight());
            const auto scr = ed.getLocalArea (&ed.labScreen(), ed.labScreen().getLocalBounds()).toFloat();
            check (ok && ! area.isEmpty() && scr.contains (area) && area.getWidth() / area.getHeight() < 1.6f,
                   "the SubverseLab mark is the icon alone (" + juce::String (area.getWidth(), 1) + " x " + juce::String (area.getHeight(), 1) + " px), inside the display's bottom right: crop written");
        }
    }

    // ---- 2. the programme: the prototype's demonstration state, through the real parameters
    DyneqAudioProcessor proc; makeProc (proc);
    auto band = [&] (int i, int type, float f, float g, float q, int slope = 1)
    {
        const juce::String n = "b" + juce::String (i) + "_";
        setParam (proc.apvts, (n + "on").toRawUTF8(), 1); setParam (proc.apvts, (n + "type").toRawUTF8(), (float) type); setParam (proc.apvts, (n + "freq").toRawUTF8(), f);
        setParam (proc.apvts, (n + "gain").toRawUTF8(), g); setParam (proc.apvts, (n + "q").toRawUTF8(), q); setParam (proc.apvts, (n + "slope").toRawUTF8(), (float) slope);
    };
    band (1, dyneq::kLowCut, 30.0f, 0.0f, 0.71f, 1); band (2, dyneq::kLowShelf, 120.0f, -2.0f, 0.71f); band (3, dyneq::kBell, 400.0f, -2.5f, 1.4f);
    band (4, dyneq::kBell, 1020.0f, 3.0f, 1.2f); band (6, dyneq::kHighShelf, 9600.0f, -1.5f, 0.71f);
    setParam (proc.apvts, "targetInst", (float) dyneq::kTgtKick); setParam (proc.apvts, "targetPart", dyneq::sliderForPart (dyneq::kTgtKick, 0));
    setParam (proc.apvts, "targetAmount", 9.0f); setParam (proc.apvts, "targetSlope", 3.0f);
    setParam (proc.apvts, "depth", 6.0f); setParam (proc.apvts, "margin", 3.0f);
    proc.uiMode = 3;

    DyneqEditor ed (proc);
    gPos = 0; play (proc, ed, beat, vocal, 10);
    auto& c = ed.ctl();
    check (proc.isSidechainConnected(), "the panel's sources are live: sidechain connected, input " + juce::String (proc.getInDb(), 1) + " dB, output " + juce::String (proc.getOutDb(), 1) + " dB");
    check (panel::Spectrum::range (ed.spectrum().key) > 20.0f, "the KEY trace is live (range " + juce::String (panel::Spectrum::range (ed.spectrum().key), 1) + " dB)");

    // what one frame of the editor costs (the timer runs 40 times a second): the tick (curves, three FFTs, node places) and a full software paint of the face at 1x
    {
        c.selectLab (3); ed.refreshFace(); ed.tickForSnapshot();
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        for (int i = 0; i < 200; ++i) ed.tickForSnapshot();
        const double t1 = juce::Time::getMillisecondCounterHiRes();
        for (int i = 0; i < 10; ++i) { auto img = ed.createComponentSnapshot (ed.getLocalBounds(), true, 1.0f); (void) img; }
        const double t2 = juce::Time::getMillisecondCounterHiRes();
        { const double a0 = juce::Time::getMillisecondCounterHiRes(); auto& comp = ed.labScreen(); for (int i = 0; i < 10; ++i) { auto img = comp.createComponentSnapshot (comp.getLocalBounds(), true, 1.0f); (void) img; } std::printf ("  LAB display alone: %.1f ms per paint at 1x\n", (juce::Time::getMillisecondCounterHiRes() - a0) / 10.0); }
        std::printf ("  LAB: one timer tick %.2f ms, one full paint at 1x %.1f ms (40 frames a second = %.0f %% of one core for ticks)\n", (t1 - t0) / 200.0, (t2 - t1) / 10.0, (t1 - t0) / 200.0 * 40.0 / 10.0);
    }
    // LAB: the programme, no band selected (the strip shows its hint)
    c.selectLab (-1);
    shot (ed, out, "lab_programme");
    // LAB: band 4 (a bell) selected: chip 4 gold, the gold node, the docked strip
    c.selectLab (3); shot (ed, out, "lab_band");
    // LAB: an OFF band selected: band 5 holds a setting but is switched off: a hollow node, the strip says OFF
    setParam (proc.apvts, "b5_on", 0); setParam (proc.apvts, "b5_freq", 2500.0f); setParam (proc.apvts, "b5_gain", 2.0f);
    c.selectLab (4); play (proc, ed, beat, vocal, 1);
    check (ed.testNodePosition (4).x >= 0.0f, "the OFF band 5 has a (hollow) node on the LAB display");
    shot (ed, out, "lab_band_off");
    for (const char* n : { "b5_freq", "b5_gain" }) proc.apvts.getParameter (n)->setValueNotifyingHost (proc.apvts.getParameter (n)->getDefaultValue());
    // LAB: a cut band selected: SLOPE live
    c.selectLab (0); shot (ed, out, "lab_band_cut");
    // LAB: a dynamic bell: RANGE -6 dB (the dynamic switch), THRESHOLD live, the "..." popover open, the range bar on the node
    setParam (proc.apvts, "b4_dyn", 1); setParam (proc.apvts, "b4_thr", -30.0f); setParam (proc.apvts, "b4_range", -6.0f);
    c.selectLab (3); ed.setDynOpen (true); play (proc, ed, beat, vocal, 1); shot (ed, out, "lab_band_dyn");
    ed.setDynOpen (false); setParam (proc.apvts, "b4_dyn", 0); setParam (proc.apvts, "b4_range", 0.0f);
    // LAB: the right-click menu of a cut band (shape, slope, on/off, delete)
    c.selectLab (0); ed.refreshFace();
    { const auto np = ed.testNodePosition (0); ed.openItems (c.menuItems (0), { (int) np.x, (int) np.y }); ed.tickForSnapshot(); check (ed.chooserOpen() && ed.chooserCount() >= 14, "the band's right-click menu opens with shape, slope (a cut), on/off and delete"); shot (ed, out, "lab_menu"); ed.closeChooser(); }
    // LAB: the ASSIST overlays, anchored under their chips (band 4 stays selected: a layer never moves the selection)
    c.selectLab (3);
    for (int m : { panel::kTarget, panel::kCollide, panel::kSelf })
    {
        if (m == panel::kSelf) { c.toggleLayer (panel::kSelf); }
        ed.toggleOverlay (m); play (proc, ed, beat, vocal, 1);
        shot (ed, out, m == panel::kTarget ? "lab_target" : m == panel::kCollide ? "lab_assist_collide" : "lab_self");
        ed.closeOverlay();
    }
    check (c.selectedBand == 3, "after the three ASSIST overlays band 4 is still the selected band");
    c.toggleLayer (panel::kCollide);   // back to COLLIDE
    // LAB: ADVANCED expanded (the display keeps its size)
    c.selectLab (-1); ed.setAdvancedOpen (true); shot (ed, out, "lab_advanced"); ed.setAdvancedOpen (false);
    // LAB: the hover ghost: the bell a double-click at 2.5 kHz / +6 dB would make
    ed.testPointer ({ ed.testLabPoint (2500.0, 6.0) }); shot (ed, out, "lab_hover_ghost"); ed.testPointer ({});
    // LAB: the TARGET chooser open (every instrument and part)
    ed.toggleOverlay (panel::kTarget); ed.openChooser ({ 300, 200 }); check (ed.chooserOpen() && ed.chooserCount() >= 20, "the TARGET list holds every instrument and part (" + juce::String (ed.chooserCount()) + " rows)"); shot (ed, out, "lab_target_list"); ed.closeChooser(); ed.closeOverlay();
    // LAB: DELTA
    c.setMode (3); c.deltaSet (true); ed.refreshFace(); play (proc, ed, beat, vocal, 1);
    shot (ed, out, "lab_delta"); c.deltaSet (false); ed.refreshFace();

    // a carved boost: band 5 (a +4 dB bell) on the band where COLLIDE cuts deepest, hover read-out on its node
    {
        int deepest = 0; float best = 0.0f;
        for (int pass = 0; pass < 24; ++pass)
        {
            play (proc, ed, beat, vocal, 1);
            for (int b = 0; b < dyneq::kBands; ++b) if (proc.getCutDb (b) > best) { best = proc.getCutDb (b); deepest = b; }
            if (best > 3.0f) break;
        }
        const float hz = DyneqAudioProcessor::bandCentresHz()[deepest];
        std::printf ("  deepest COLLIDE cut seen: %.1f dB at %.0f Hz\n", best, hz);
        setParam (proc.apvts, "b5_on", 1); setParam (proc.apvts, "b5_type", 0); setParam (proc.apvts, "b5_freq", hz); setParam (proc.apvts, "b5_gain", 4.0f); setParam (proc.apvts, "b5_q", 1.2f);
        c.selectLab (4);
        float net = 99.0f; bool got = false;
        for (int pass = 0; pass < 30 && ! got; ++pass)
        {
            play (proc, ed, beat, vocal, 1);
            const auto& l = proc.getBandLayers (4);
            net = l.net.load(); got = l.conflict.load() < -0.5f;
        }
        const auto& l = proc.getBandLayers (4);
        std::printf ("  band 5: gain +4.0, COLLIDE %.2f dB, SELF %.2f, TARGET %.2f, net %.2f (engine accessors)\n", l.conflict.load(), l.self.load(), l.target.load(), l.net.load());
        check (got, "a boost sits inside a COLLIDE cut: the engine reports the conflict (" + juce::String (l.conflict.load(), 2) + " dB), net " + juce::String (net, 2) + " dB");
        c.selectLab (-1); ed.refreshFace(); ed.tickForSnapshot();
        check (ed.testNodePosition (4).x >= 0.0f, "the LAB node of band 5 is on the screen");
        ed.testHover (4); shot (ed, out, "lab_carved");
        ed.testHover (-1);
        setParam (proc.apvts, "b5_on", 0);
    }

    // P1n: LOW CUT 72 dB/oct at 140 Hz with Q 6: the panel draws the cut AND its resonance (the curve is the engine's own response, Engine::freeBandMagDb)
    {
        setParam (proc.apvts, "b1_on", 1); setParam (proc.apvts, "b1_type", (float) dyneq::kLowCut); setParam (proc.apvts, "b1_freq", 140.0f); setParam (proc.apvts, "b1_q", 6.0f); setParam (proc.apvts, "b1_slope", 4.0f);
        c.setMode (0);
        c.selectLab (0); play (proc, ed, beat, vocal, 2);
        const auto& cv = ed.curves();
        float peak = -99.0f, atLow = 0.0f; double peakHz = 0.0;
        for (int i = 0; i < panel::kCurveN; ++i) { if (cv.selected[i] > peak) { peak = cv.selected[i]; peakHz = cv.f[i]; } if (cv.f[i] < 50.0f) atLow = cv.selected[i]; }
        check (c.bandSlopeDb (0) == 72 && cv.selectedOn && peak >= 6.0f && std::abs (peakHz - 140.0) < 30.0 && atLow < -40.0,
               "the LC 72 curve at Q 6 shows its resonance: peak " + juce::String (peak, 1) + " dB at " + juce::String (peakHz, 0) + " Hz (>= +6 dB near 140 Hz), " + juce::String (atLow, 1) + " dB at 50 Hz (a 72 dB/oct cut)");
        shot (ed, out, "lab_lc72_resonance");
    }
    // ---- 0.9.1-test2 (25 Oct, tester report T3 / T5): the BAKE flow, an UNUSED band, the hover help. A fresh instance: COLLIDE keyed by the vocal, no band in use.
    {
        DyneqAudioProcessor p2; makeProc (p2);
        DyneqEditor e2 (p2); auto& c2 = e2.ctl();
        auto find = [&] (std::function<bool (PanelControl&)> pred) { juce::Component* r = nullptr; e2.forEachControl ([&] (juce::Component& cc, PanelControl& pc) { if (r == nullptr && pred (pc)) r = &cc; }); return r; };
        gPos = 0;
        // an UNUSED band selected (Andre: "the band showed UNUSED, 0.0 dB gain, Range OFF"): one sentence instead of read-outs
        c2.selectLab (2); play (p2, e2, beat, vocal, 2);
        check (c2.isEmptySlot (2), "band 3 is an unused slot"); shot (e2, out, "t2_lab_band_unused");
        // the same band active (ON): the state reads ACTIVE; DELETE stands apart, its hover help open
        setParam (p2.apvts, "b3_on", 1); setParam (p2.apvts, "b3_gain", -3.0f); c2.selectLab (2); play (p2, e2, beat, vocal, 1);
        if (auto* del = find ([] (PanelControl& pc) { return pc.uiAction() == "DELETE BAND"; })) check (e2.showHelpFor (*del), "DELETE has hover help");
        shot (e2, out, "t2_lab_band_active_help_delete"); e2.hideHelp();
        c2.deleteBand (2); c2.selectLab (-1);
        // ADVANCED before arming: the BAKE line explains the three steps
        e2.setAdvancedOpen (true); play (p2, e2, beat, vocal, 1); shot (e2, out, "t2_lab_bake_idle");
        // hover help over BAKE ARM
        if (auto* arm = find ([] (PanelControl& pc) { return pc.boundParams().size() == 1 && pc.boundParams()[0] == "bakeArm"; }))
        { check (e2.showHelpFor (*arm), "BAKE ARM has hover help"); shot (e2, out, "t2_lab_help_bake_arm"); e2.hideHelp(); }
        // armed in SELF (the tester's T4 set-up): not listening, and the line says why
        c2.toggleLayer (panel::kSelf); setParam (p2.apvts, "bakeArm", 1); play (p2, e2, beat, vocal, 2);
        check (p2.bakeStatus().state == DyneqAudioProcessor::kBakeNotListening, "armed in SELF: " + p2.bakeStatus().line1);
        shot (e2, out, "t2_lab_bake_armed_self");
        // COLLIDE on: listening, the preview of what APPLY will write
        c2.toggleLayer (panel::kCollide); setParam (p2.apvts, "bakeArm", 0); setParam (p2.apvts, "bakeArm", 1); play (p2, e2, beat, vocal, 6);
        check (p2.bakeStatus().state == DyneqAudioProcessor::kBakeListening && p2.bakeSuggest().count >= 1, "listening: " + p2.bakeStatus().line1 + " | " + p2.bakeStatus().line2);
        shot (e2, out, "t2_lab_bake_listening");
        if (auto* ap = find ([] (PanelControl& pc) { return pc.uiAction() == "BAKE APPLY"; })) { e2.showHelpFor (*ap); shot (e2, out, "t2_lab_help_apply"); e2.hideHelp(); }
        // APPLY during playback, playback goes on: the bands are in use (chips, nodes, the static curve), COLLIDE is off, the line says what was baked
        const int w = p2.bakeApply(); play (p2, e2, beat, vocal, 2);
        check (w >= 1 && p2.bakeStatus().state == DyneqAudioProcessor::kBakeBaked, "APPLY baked " + juce::String (w) + " band(s): " + p2.bakeStatus().line1);
        shot (e2, out, "t2_lab_bake_baked");
        e2.setAdvancedOpen (false); shot (e2, out, "t2_lab_bake_baked_closed");
        // the COLLIDE overlay: its note names the path to BAKE
        p2.undo.undo(); e2.toggleOverlay (panel::kCollide); play (p2, e2, beat, vocal, 1); shot (e2, out, "t2_lab_assist_collide"); e2.closeOverlay();
    }
    std::printf ("\nui_snapshot: %s\n", gFail == 0 ? "all passed" : "FAILED");
    return gFail == 0 ? 0 : 1;
}
