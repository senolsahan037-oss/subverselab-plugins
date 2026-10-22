/*  ui_snapshot: draws the JING FANG panel to PNGs without a screen and runs the panel checks.

        ui_snapshot [outdir]          (default: out/ui under the current folder)

    Pictures: front_init.png (Init, 100 %), front_pad.png (a preset with notes held), front_micro.png (micro mode, rows moved,
    keyswitch on, notes held), front_200.png (200 %), help_tuning.png / help_keys.png (help open on a popover).
    Checks: 1 glide (a preset load glides; one tick lands between start and target; settles), 2 double-click (the loaded preset's
    value, not the factory default), 3 help never resizes and every popover is inside the panel, 4 lockup link, 5 layout (every
    control inside its section, none overlapping), 6 every engine parameter has a control, 7 micro click plays, right-click / the strip cycles the pitch class
    low / mid / high, 8 normal click plays the MIDI note and lights the key, 9 computer keys (normal: Live layout; micro: rows).
*/
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "MidiClip.h"
#include <cstdio>
#include <set>

namespace
{
int gPass = 0, gFail = 0;
juce::File gOut;
void check (bool ok, const juce::String& what)
{
    std::printf ("[%s] %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8());
    std::fflush (stdout);
    ok ? ++gPass : ++gFail;
}

void snap (juce::Component& c, const juce::String& name, float scale = 1.0f)
{
    const auto img = c.createComponentSnapshot (c.getLocalBounds(), true, scale);
    const auto f = gOut.getChildFile (name);
    f.deleteFile();
    juce::FileOutputStream os (f);
    juce::PNGImageFormat png;
    png.writeImageToStream (img, os);
    std::printf ("  wrote %s (%d x %d)\n", f.getFullPathName().toRawUTF8(), img.getWidth(), img.getHeight());
}

void snapToFile (const juce::Image& img, const juce::String& name)
{
    const auto f = gOut.getChildFile (name); f.deleteFile();
    juce::FileOutputStream os (f); juce::PNGImageFormat().writeImageToStream (img, os);
}

int presetIndex (JingFangProcessor& p, const juce::String& family, const juce::String& name)
{
    for (size_t i = 0; i < p.presets().size(); ++i)
        if (p.presets()[i].family == family && p.presets()[i].name == name) return (int) i;
    return -1;
}

void play (JingFangProcessor& p, std::initializer_list<int> keys, int blocks = 6)
{
    juce::AudioBuffer<float> buf (2, 512);
    juce::MidiBuffer m;
    for (int k : keys) m.addEvent (juce::MidiMessage::noteOn (1, k, (juce::uint8) 100), 0);
    p.processBlock (buf, m);
    juce::MidiBuffer none;
    for (int b = 1; b < blocks; ++b) p.processBlock (buf, none);
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    gOut = argc > 1 ? juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]) : juce::File::getCurrentWorkingDirectory().getChildFile ("out/ui");
    gOut.createDirectory();

    JingFangProcessor proc;
    proc.setRateAndBufferSizeDetails (44100, 512);
    proc.prepareToPlay (44100, 512);
    std::unique_ptr<JingFangEditor> ed (static_cast<JingFangEditor*> (proc.createEditor()));
    ed->setSize (jfui::kW, jfui::kH);   // snapshots at the logical size, whatever screen this runs on (the fit is checked below)
    ed->setVisible (true);
    auto pump = [&] { proc.runMessageWorkForTest(); ed->tickForTest(); juce::MessageManager::getInstance()->runDispatchLoopUntil (5); };
    pump();

    std::printf ("panel %d x %d (logical size; the fit on the owner's screen is checked next)\n", ed->getWidth(), ed->getHeight());
    {   // the owner's screen, measured 18 Oct: 1920 x 1080, menu bar 30 px, dock at the left (about 64 px)
        const float f = jfui::fitScale (1920 - 64, 1080 - 30);
        const int h = juce::roundToInt (jfui::kH * f), w = juce::roundToInt (jfui::kW * f);
        check (30 + 28 + h <= 1080 && 64 + w <= 1920 && f >= 0.95f,
               "on 1920 x 1080 (menu 30, dock 64) the window opens at " + juce::String (w) + " x " + juce::String (h) + " (scale " + juce::String (f, 3)
               + "), its bottom at " + juce::String (30 + 28 + h) + " <= 1080");
    }
    snap (*ed, "front_init.png");

    // ---- 1. glide on a preset load
    {
        const int idx = presetIndex (proc, "Pad", "Warm Pad");
        check (idx >= 0, "preset Pad / Warm Pad found");
        auto* k = ed->knob ("filter.cutoff");
        const float start = k->drawn();
        proc.loadPresetIndex (idx);
        const float target = k->parameter()->getValue();
        check (k->gliding() && std::abs (k->drawn() - start) < 1.0e-6f, "preset load: the cutoff knob starts gliding (drawn " + juce::String (start, 4) + ", target " + juce::String (target, 4) + ")");
        ed->tickForTest();
        const float one = k->drawn();
        check ((one - start) * (target - one) > 0.0f, "after one 30 Hz tick the drawing is between start and target (" + juce::String (one, 4) + ")");
        int ticks = 1;
        while (k->gliding() && ticks < 100) { ed->tickForTest(); ++ticks; }
        check (! k->gliding() && std::abs (k->drawn() - target) < 1.0e-6f, "settles on the target after " + juce::String (ticks) + " ticks (" + juce::String (ticks * 1000 / 30) + " ms)");
        check (ticks >= 6 && ticks <= 20, "glide time inside 0.2 .. 0.67 s");
        // a move by hand never glides
        auto* reso = ed->knob ("filter.reso");
        reso->parameter()->setValueNotifyingHost (0.9f);
        check (! reso->gliding() && std::abs (reso->drawn() - 0.9f) < 1.0e-5f, "a value set by hand / host does not glide");
    }
    // ---- 2. double-click = the loaded preset's value
    {
        auto* k = ed->knob ("filter.cutoff");
        const float loaded = proc.loadedValue ("filter.cutoff");
        const float factory = svl::paramDefs()[svl::FLT_CUTOFF].def;
        k->parameter()->setValueNotifyingHost (0.95f);
        k->resetToLoaded();   // what mouseDoubleClick calls
        pump();
        juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer m; proc.processBlock (buf, m);
        const float now = k->parameter()->convertFrom0to1 (k->parameter()->getValue());
        check (std::abs (now - loaded) < 0.5f && std::abs (loaded - factory) > 1.0f,
               "double-click: cutoff back to the loaded preset's " + juce::String (loaded) + " Hz (factory default " + juce::String (factory) + ", now " + juce::String (now, 1) + ")");
        check (k->gliding(), "double-click glides");
        for (int i = 0; i < 40; ++i) ed->tickForTest();
    }
    proc.loadPresetIndex (presetIndex (proc, "Pad", "Warm Pad"));
    for (int i = 0; i < 40; ++i) ed->tickForTest();
    play (proc, { 48, 55, 60, 64 });
    pump();
    snap (*ed, "front_pad.png");

    // ---- 3. help
    {
        auto& h = ed->helpOverlay();
        const auto before = ed->getBounds();
        ed->helpKey().onClick();
        check (h.isOpen(), "the ? key opens help");
        bool inside = true, same = true;
        for (int i = 0; i < h.count(); ++i)
        {
            h.openTip (i);
            inside = inside && juce::Rectangle<int> (0, 0, jfui::kW, jfui::kH).contains (h.tipBounds()) && ! h.tipBounds().isEmpty();
            same = same && ed->getBounds() == before;
        }
        int covered = 0;
        for (int i = 0; i < h.count(); ++i)
            for (auto* c : ed->panelView().getChildren())
                if (c != &h && c->getComponentID() != "dragon" && c->isVisible() && c->getBounds().intersects (h.markerBounds (i)))
                {
                    if (c == &ed->keyboardView())
                    {   // the keyboard's top-left corner is empty: its strip starts after the label column, its keys under the strip
                        const auto m = h.markerBounds (i) - c->getPosition();
                        const juce::Rectangle<float> strip (jfui::Keyboard3::kLabelW, 0.0f, (float) c->getWidth(), jfui::Keyboard3::kStripH),
                            keys (0.0f, jfui::Keyboard3::kStripH, (float) c->getWidth(), (float) c->getHeight());
                        if (! strip.intersects (m.toFloat()) && ! keys.intersects (m.toFloat())) continue;
                    }
                    ++covered; std::printf ("  marker %d covers %s\n", i + 1, c->getBounds().toString().toRawUTF8());
                }
        check (covered == 0, "no help marker lies on a control (" + juce::String (h.count()) + " markers)");
        check (inside, "every popover (" + juce::String (h.count()) + ") lies inside the panel");
        check (same && ed->getBounds() == before, "help never changes the window size");
        h.openTip (7);
        snap (*ed, "help_tuning.png");
        h.openTip (9);
        snap (*ed, "help_keys.png");
        ed->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey));
        check (! h.isOpen(), "Escape closes help");
        // a door open: the modules under it have no numbers (owner 19 Oct: LAB's "?" explained the hidden PITCH CLICK DRUM)
        for (auto* k : { &ed->labKey(), &ed->seqKey() })
        {
            k->onClick(); for (int t = 0; t < 40; ++t) ed->tickForTest();
            ed->helpKey().onClick();
            bool none = true;
            for (int i = 0; i < h.count(); ++i)
                if (h.shown (i)) none = none && ! ed->labDoorView()->getBounds().expanded (8).contains (h.markerBounds (i).getCentre());
            const int n = h.shownCount();
            ed->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey));
            k->onClick(); for (int t = 0; t < 40; ++t) ed->tickForTest();
            check (none && n > 0 && n < h.count(), "help over an open door: " + juce::String (n) + " of " + juce::String (h.count())
                   + " numbers, none on the door");
        }
        ed->helpKey().onClick();
        check (h.shownCount() == h.count(), "doors closed: every help number is back (" + juce::String (h.shownCount()) + ")");
        ed->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey));
    }
    // ---- 4. lockup
    {
        auto& l = ed->lockupLink();
        check (jfui::Lockup::url().toString (false) == "https://subverselab.com", "lockup links to https://subverselab.com");
        check (l.getRight() >= jfui::kW - 30 && l.getBottom() >= jfui::kH - 40, "lockup at the bottom right (" + l.getBounds().toString() + ")");
    }
    // ---- 5. layout
    {
        const auto secs = ed->sectionsForTest();
        int outside = 0, overlaps = 0;
        std::vector<juce::Rectangle<int>> rects;
        for (auto* c : ed->panelView().getChildren())
        {
            if (c == &ed->helpOverlay() || c->getComponentID() == "dragon" || ! c->isVisible()) continue;   // the dragon is a click-through layer drawn above on purpose
            rects.push_back (c->getBounds());
            if (! juce::Rectangle<int> (0, 0, jfui::kW, jfui::kH).contains (c->getBounds())) ++outside;
        }
        for (size_t i = 0; i < rects.size(); ++i)
            for (size_t j = i + 1; j < rects.size(); ++j) if (rects[i].intersects (rects[j])) { ++overlaps; std::printf ("  overlap %s / %s\n", rects[i].toString().toRawUTF8(), rects[j].toString().toRawUTF8()); }
        int knobsOut = 0;
        for (auto& k : ed->allKnobs())
        {
            if (k->getParentComponent() != &ed->panelView()) continue;   // the cable compartment's knobs: checked in 10
            bool in = false;
            for (auto& s : secs) if (s.second.contains (k->getBounds())) in = true;
            if (! in) { ++knobsOut; std::printf ("  knob outside its section: %s %s\n", k->paramId().toRawUTF8(), k->getBounds().toString().toRawUTF8()); }
        }
        int secOverlap = 0;
        for (size_t i = 0; i < secs.size(); ++i) for (size_t j = i + 1; j < secs.size(); ++j) if (secs[i].second.intersects (secs[j].second)) ++secOverlap;
        check (outside == 0 && overlaps == 0, "controls: " + juce::String ((int) rects.size()) + " inside the panel, " + juce::String (overlaps) + " overlapping");
        check (knobsOut == 0 && secOverlap == 0, "every knob inside a section (" + juce::String ((int) ed->allKnobs().size()) + " knobs, " + juce::String ((int) secs.size()) + " sections, " + juce::String (secOverlap) + " overlapping)");
    }
    // ---- 6. every engine parameter has a control
    {
        juce::StringArray covered;
        for (auto& k : ed->allKnobs()) covered.add (k->paramId());
        for (int o = 1; o <= 3; ++o) covered.add ("osc" + juce::String (o) + ".type");           // TypeStepper
        covered.add ("fx.echo.time");                                                              // TypeStepper
        covered.add ("fx.mod.type"); covered.add ("fx.echo.type");                                 // FX door TypeSteppers
        covered.add ("seq.direction");                                                             // SEQ door TypeStepper
        for (auto id : { "filter.mode", "drum.mode", "drum.one_shot", "voice.mode", "tuning.keyswitch", "tuning.mode" }) covered.add (id);   // segments, NORMAL / MICRO
        for (int pc = 0; pc < 12; ++pc) covered.add (juce::String ("tuning.row.") + svl::kPitchClassIds[pc]);   // keyboard click / keyswitches
        covered.add ("fm.target");                                                                                 // segments
        for (auto id : { "arp.on", "arp.mode", "arp.chord", "arp.rate", "arp.latch", "seq.on", "seq.mode", "seq.rate" }) covered.add (id);   // SEQ door switches / selectors
        for (auto id : { "drums.on", "drums.rate", "drums.octave", "drums.keys", "drums.mode" }) covered.add (id);                               // DRUM door switches / selector
        for (int t = 1; t <= 8; ++t)
        {
            covered.add ("drums.t" + juce::String (t) + ".mute");                                                  // DRUM door switches
            for (int st = 1; st <= 16; ++st) covered.add ("drums.t" + juce::String (t) + ".s" + juce::String (st)); // DRUM door step keys
        }
        for (int st = 1; st <= 16; ++st)                                                                           // SEQ door step lanes
            for (auto f : { ".on", ".row", ".slide", ".accent" }) covered.add ("seq.s" + juce::String (st) + f);   // step key + switches (NOTE / VEL / GATE are knobs)
        for (int l = 1; l <= 3; ++l) for (auto f : { ".shape", ".sync", ".retrigger" }) covered.add ("lfo" + juce::String (l) + f);   // compartment steppers
        for (int m = 1; m <= 16; ++m) { covered.add ("mod" + juce::String (m) + ".source"); covered.add ("mod" + juce::String (m) + ".destination"); }   // cables
        for (auto id : { "daw.seq", "daw.drums" }) covered.add (id);                                               // PATTERN / MIDI keys on the doors
        juce::StringArray missing;
        for (const auto& id : JingFangProcessor::paramIds()) if (! covered.contains (id)) missing.add (id);
        check (missing.isEmpty(), "every engine parameter (" + juce::String (svl::kNumParams) + ") has a control" + (missing.isEmpty() ? juce::String() : ": missing " + missing.joinIntoString (", ")));
    }
    // ---- 6b. BPM and KEY in the preset screen; PATTERN / MIDI and the MIDI clip keys on the doors (owner 19 Oct)
    {
        auto setP = [&] (const char* id, float v) { if (auto* hp = proc.apvts.getParameter (id)) jfui::setParamGesture (hp, v); juce::AudioBuffer<float> b (2, 512); juce::MidiBuffer m; proc.processBlock (b, m); ed->tickForTest(); };
        auto& bpm = ed->bpmReadout();
        // owner 19 Oct: "ok tuşları bunların dışında kalmış": SAVE ? [BPM] < preset > PATCH: the arrows hug the preset screen (no KEY)
        juce::Rectangle<int> preset, prevR, nextR;
        for (auto* c : ed->panelView().getChildren())
        {
            if (auto* k = dynamic_cast<jfui::PanelKey*> (c)) { if (k->arrow < 0) prevR = k->getBounds(); if (k->arrow > 0) nextR = k->getBounds(); }
            if (c->getMouseCursor() == juce::MouseCursor::PointingHandCursor && c->getWidth() > 200 && c->getY() < 80) preset = c->getBounds();
        }
        const bool order = bpm.getRight() < prevR.getX() && prevR.getRight() < preset.getX() && preset.getRight() < nextR.getX()
                        && bpm.getParentComponent() == &ed->panelView() && std::abs ((bpm.getX() + nextR.getRight()) - (ed->saveKey().getX() + ed->drumKey().getRight())) <= 6;   // centred between SAVE and DRUM
        check (order && ed->readoutText() == "120",
               "top bar BPM < preset >, the group centred: 120 (BPM " + bpm.getBounds().toString() + ", < " + prevR.toString() + ", preset " + preset.toString() + ", > " + nextR.toString() + ")");
        {
            auto src = juce::Desktop::getInstance().getMainMouseSource();
            const auto now = juce::Time::getCurrentTime();
            const juce::Point<float> a (40, 30), b (40, 0);
            bpm.mouseDown (juce::MouseEvent (src, a, {}, 1.0f, 0, 0, 0, 0, &bpm, &bpm, now, a, now, 1, false));
            bpm.mouseDrag (juce::MouseEvent (src, b, {}, 1.0f, 0, 0, 0, 0, &bpm, &bpm, now, a, now, 1, true));
            const float dragged = proc.userTempo();
            bpm.mouseDoubleClick (juce::MouseEvent (src, a, {}, 1.0f, 0, 0, 0, 0, &bpm, &bpm, now, a, now, 2, false));
            check (std::abs (dragged - 130.0f) < 0.5f && std::abs (proc.userTempo() - 120.0f) < 0.5f, "BPM: a drag up of 30 px = +10 BPM (" + juce::String (dragged, 0) + "), a double-click 120");
        }
        snap (*ed, "front_bpm.png");
        // the doors' PATTERN / MIDI keys and the clips
        auto findKey = [] (juce::Component* door, const juce::String& t) -> jfui::PanelKey*
        {
            for (auto* c : door->getChildren()) if (auto* k = dynamic_cast<jfui::PanelKey*> (c)) if (k->text == t) return k;
            return nullptr;
        };
        bool keysOk = true;
        for (auto* door : { (juce::Component*) ed->seqDoorView(), (juce::Component*) ed->drumDoorView() })
            keysOk &= findKey (door, "PATTERN") != nullptr && findKey (door, "CLIP") != nullptr && findKey (door, "PLAY") != nullptr;
        check (keysOk, "SEQ and DRUM doors carry PATTERN / MIDI and the CLIP key beside PLAY");
        if (keysOk)
        {
            findKey (ed->seqDoorView(), "PATTERN")->onClick(); ed->tickForTest();
            const bool midi = proc.apvts.getRawParameterValue ("daw.seq")->load() > 0.5f;
            ed->seqDoorView()->setOpen (true); for (int i = 0; i < 20; ++i) ed->tickForTest();
            const bool shown = findKey (ed->seqDoorView(), "PATTERN") == nullptr && findKey (ed->seqDoorView(), "MIDI") != nullptr;   // the source key reads MIDI
            for (auto* c : ed->seqDoorView()->getChildren()) if (auto* k = dynamic_cast<jfui::PanelKey*> (c)) if (k->getTooltip().startsWith ("PATTERN:")) k->onClick();
            ed->tickForTest();
            proc.armClip (1); ed->drumDoorView()->setOpen (true); for (int i = 0; i < 20; ++i) ed->tickForTest();
            const bool armedShown = findKey (ed->drumDoorView(), "ARMED") != nullptr;
            proc.disarmClip (1); ed->drumDoorView()->setOpen (false); ed->seqDoorView()->setOpen (false); for (int i = 0; i < 20; ++i) ed->tickForTest();
            check (midi && shown && armedShown && proc.apvts.getRawParameterValue ("daw.seq")->load() < 0.5f,
                   "PATTERN / MIDI: a click switches daw.seq and the key reads MIDI; a dragged clip shows ARMED; a click back to PATTERN");
        }
        // the clips: Boom Bap's beat and Climbing Acid's sequence
        auto notesOf = [] (const juce::MidiFile& f, std::set<int>& keys, double& endTick, int& count)
        {
            count = 0; endTick = 0;
            if (f.getNumTracks() < 1) return;
            const auto* t = f.getTrack (0);
            for (int i = 0; i < t->getNumEvents(); ++i)
            {
                const auto& m = t->getEventPointer (i)->message;
                if (m.isNoteOn()) { ++count; keys.insert (m.getNoteNumber()); }
                if (m.isEndOfTrackMetaEvent()) endTick = m.getTimeStamp();
            }
        };
        int idx = 0;
        for (size_t i = 0; i < proc.presets().size(); ++i) if (proc.presets()[i].family == "Drum Machine" && proc.presets()[i].name == "Boom Bap") idx = (int) i;
        proc.loadPresetIndex (idx); setP ("seq.on", 0);
        int onSteps = 0;
        for (int t = 1; t <= 8; ++t) for (int st = 1; st <= 16; ++st) if (proc.apvts.getRawParameterValue ("drums.t" + juce::String (t) + ".s" + juce::String (st))->load() > 0.5f
                                                                          && proc.apvts.getRawParameterValue ("drums.t" + juce::String (t) + ".mute")->load() < 0.5f) ++onSteps;
        std::set<int> dk; double dEnd = 0; int dn = 0;
        notesOf (jfclip::make (proc, 1), dk, dEnd, dn);
        const int bars = jfclip::bars (proc, 1);
        check (dn == onSteps * bars * 16 / juce::roundToInt (proc.exactValue (svl::DRUMS_LEN)) && ! dk.empty() && *dk.begin() >= 36 && *dk.rbegin() <= 43
               && std::abs (dEnd - bars * 4 * 96) < 0.5,
               "DRUM clip of Boom Bap: " + juce::String (dn) + " hits on notes 36..43 over " + juce::String (bars) + " bar(s) = every step that is on (" + juce::String (onSteps) + " per cycle)");
        for (size_t i = 0; i < proc.presets().size(); ++i) if (proc.presets()[i].family.startsWith ("Sequence") && proc.presets()[i].name == "Climbing Acid") idx = (int) i;
        proc.loadPresetIndex (idx);
        { juce::AudioBuffer<float> b (2, 512); juce::MidiBuffer m; proc.processBlock (b, m); }
        int seqOn = 0;
        const int len = juce::roundToInt (proc.exactValue (svl::SEQ_LEN));
        for (int st = 1; st <= len; ++st) if (proc.apvts.getRawParameterValue ("seq.s" + juce::String (st) + ".on")->load() > 0.5f) ++seqOn;
        std::set<int> sk; double sEnd = 0; int sn = 0;
        notesOf (jfclip::make (proc, 0), sk, sEnd, sn);
        const int sb = jfclip::bars (proc, 0);
        const double cyc = len * svl::kStepRateBeats[(int) proc.exactValue (svl::SEQ_RATE)];
        check (sn == juce::roundToInt (seqOn * sb * 4 / cyc) && std::abs (sEnd - sb * 4 * 96) < 0.5,
               "SEQ clip of Climbing Acid: " + juce::String (sn) + " notes over " + juce::String (sb) + " bar(s) = every step that is on (" + juce::String (seqOn) + " of " + juce::String (len) + ")");
        // a step's ROW goes into the clip as a pitch bend (one step of 53 at the bend range), the arp's octaves into the keys
        {
            setP ("seq.s1.row", 2.0f);
            const auto f = jfclip::make (proc, 0);
            int want = 8192 + juce::roundToInt ((12.0f / proc.exactValue (svl::TUNE_EDO) * proc.exactValue (svl::ROW_UP)) / proc.exactValue (svl::V_BEND) * 8192.0f), got = -1;
            const auto* t = f.getTrack (0);
            for (int i = 0; i < t->getNumEvents() && got < 0; ++i) if (t->getEventPointer (i)->message.isPitchWheel()) got = t->getEventPointer (i)->message.getPitchWheelValue();
            setP ("seq.s1.row", 1.0f);
            check (got == want, "a step on the high row is a pitch bend in the clip (" + juce::String (got) + ", want " + juce::String (want) + ")");
            for (size_t i = 0; i < proc.presets().size(); ++i) if (proc.presets()[i].family == "Arp - Classic" && proc.presets()[i].name == "Up Down Two Octaves") idx = (int) i;
            proc.loadPresetIndex (idx);
            { juce::AudioBuffer<float> b (2, 512); juce::MidiBuffer m; proc.processBlock (b, m); }
            std::set<int> ak; double aEnd = 0; int an = 0;
            notesOf (jfclip::make (proc, 0), ak, aEnd, an);
            check (an > 0 && ! ak.empty() && *ak.rbegin() - *ak.begin() >= 12, "an arp over two octaves: the clip spans " + juce::String (ak.empty() ? 0 : *ak.rbegin() - *ak.begin()) + " keys, " + juce::String (an) + " notes");
        }
        // a click on the clip key saves the .mid (here into a temp folder, never the user's Music folder)
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("jf_clip_test");
        dir.deleteRecursively();
        jfclip::saveFolderOverrideForTests() = dir;
        if (keysOk) { findKey (ed->seqDoorView(), "CLIP")->onClick(); findKey (ed->seqDoorView(), "CLIP")->onClick(); }
        const auto files = dir.findChildFiles (juce::File::findFiles, false, "*.mid");
        check (files.size() == 2, "a click on CLIP saves the clip, a second click a second file (" + juce::String (files.size()) + " .mid)");
        dir.deleteRecursively();
        jfclip::saveFolderOverrideForTests() = juce::File();
        proc.loadPresetIndex (0);
    }
    // ---- 6c. keyboard lights follow the sound (owner 19 Oct: "arpejin notalarını gösteren taraf hâlâ gecikmeli"): a light goes out
    //      on the first 60 Hz frame after its sound stops; a short note still shows for at least 45 ms
    {
        auto& kb = ed->keyboardView();
        juce::AudioBuffer<float> b (2, 64);
        auto block = [&] (juce::MidiBuffer m) { proc.processBlock (b, m); };
        juce::uint32 t = juce::Time::getMillisecondCounter() + 60000;   // ahead of the keyboard's own 60 Hz clock (it may have run before)
        { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOn (1, 64, (juce::uint8) 100), 0); block (m); }
        kb.pollSounding (t);
        const bool onAtOnce = kb.lightShown (1, 64);
        t += 100; kb.pollSounding (t);
        const bool heldOn = kb.lightShown (1, 64);
        { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOff (1, 64), 0); block (m); }
        for (int i = 0; i < 40; ++i) block ({});                     // the release: the gate is closed at once
        t += 17; kb.pollSounding (t);
        const bool offNextFrame = ! kb.lightShown (1, 64);
        // a 1-block note: lit at the frame that sees it, still lit 30 ms later, out by 50 ms
        { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOn (1, 67, (juce::uint8) 100), 0); block (m); }
        t += 17; kb.pollSounding (t);
        { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOff (1, 67), 0); block (m); }
        const bool shortOn = kb.lightShown (1, 67);
        t += 30; kb.pollSounding (t);
        const bool shortStill = kb.lightShown (1, 67);
        t += 20; kb.pollSounding (t);
        const bool shortOff = ! kb.lightShown (1, 67);
        check (onAtOnce && heldOn && offNextFrame && shortOn && shortStill && shortOff,
               "keyboard lights: on at the first frame, off on the first frame after the sound stops, a short note shown >= 45 ms ("
               + juce::String ((int) onAtOnce) + juce::String ((int) heldOn) + juce::String ((int) offNextFrame) + juce::String ((int) shortOn) + juce::String ((int) shortStill) + juce::String ((int) shortOff)
               + ", preset " + proc.currentFamily() + " / " + proc.currentName() + ")");
    }
    // ---- 7. micro: a click plays; right-click / the strip cycles the pitch class
    {
        auto& kb = ed->keyboardView();
        ed->microKey().onClick();
        juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer m; proc.processBlock (buf, m);
        check (kb.micro(), "MICRO switches the tuning mode");
        auto rowE = [&] { return juce::roundToInt (proc.apvts.getRawParameterValue ("tuning.row.e")->load()); };
        const int r0 = rowE();
        // a plain click plays (owner 18 Oct): E on its own row (mid) lights and does not move the row
        kb.press (1, 64, false); proc.processBlock (buf, m);
        check (rowE() == 1 && proc.keyLit (1, 64), "micro: a click plays E on its row and leaves the row alone");
        kb.release(); proc.processBlock (buf, m);
        // right-click / Alt + click (moveRow) or the strip moves the row
        kb.press (1, 64, true); kb.release(); proc.processBlock (buf, m);
        const int r1 = rowE();
        kb.press (0, 52, true); kb.release(); proc.processBlock (buf, m);
        const int r2 = rowE();
        kb.cycleRow (4); proc.processBlock (buf, m);
        const int r3 = rowE();
        check (r0 == 1 && r1 == 2 && r2 == 0 && r3 == 1, "right-click on any E (or its strip cell) cycles E: mid -> high -> low -> mid (" + juce::String (r0) + juce::String (r1) + juce::String (r2) + juce::String (r3) + ")");
        // the picture: B and E low, F# high, keyswitch on, a chord held
        auto set = [&] (const char* id, float v) { jfui::setParamGesture (proc.apvts.getParameter (id), v); };
        set ("tuning.row.e", 0); set ("tuning.row.b", 0); set ("tuning.row.gb", 2); set ("tuning.keyswitch", 1);
        proc.processBlock (buf, m);
        play (proc, { 52, 59, 64, 66 });
        pump();
        snap (*ed, "front_micro.png");
        // MIDI keys light on their pitch class's row; note-offs unlight them
        check (proc.keyLit (0, 52) && proc.keyLit (0, 59) && proc.keyLit (0, 64) && proc.keyLit (2, 66) && ! proc.keyLit (1, 64),
               "micro: MIDI E3, B3, E4 light on the low row, F#4 on the high row");
        {
            juce::MidiBuffer offs;
            for (int k : { 52, 59, 64, 66 }) offs.addEvent (juce::MidiMessage::noteOff (1, k), 0);
            proc.processBlock (buf, offs);
            check (! proc.keyLit (0, 52) && ! proc.keyLit (0, 59) && ! proc.keyLit (0, 64) && ! proc.keyLit (2, 66), "MIDI note-off unlights the key on its row");
        }
        proc.uiAllNotesOff(); proc.processBlock (buf, m);
        // ---- 9b. computer keys in micro: A row = mid, Z row = low (explicit rows, lit on their manual)
        const bool aDown = ed->keyPressed (juce::KeyPress ('a', {}, 'a'));
        const bool zDown = ed->keyPressed (juce::KeyPress ('z', {}, 'z'));
        check (aDown && zDown && proc.keyLit (1, 60) && proc.keyLit (0, 60), "micro: A plays C4 on the middle row, Z plays C4 on the low row");
        ed->keyboardView().releaseAllComputerKeys();
        check (! proc.keyLit (1, 60) && ! proc.keyLit (0, 60), "computer keys released");
        set ("tuning.mode", 0); set ("tuning.row.e", 1); set ("tuning.row.b", 1); set ("tuning.row.gb", 1); set ("tuning.keyswitch", 0);
        proc.processBlock (buf, m);
    }
    // ---- 8. normal click plays the MIDI note
    {
        auto& kb = ed->keyboardView();
        proc.uiAllNotesOff();
        juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer m; proc.processBlock (buf, m);
        kb.press (1, 69, false);
        proc.processBlock (buf, m);
        float peak = 0; for (int b = 0; b < 8; ++b) { proc.processBlock (buf, m); peak = std::max (peak, buf.getMagnitude (0, 0, 512)); }
        check (proc.keyLit (1, 69) && peak > 0.001f, "normal: a click on A4 lights it and sounds (peak " + juce::String (juce::Decibels::gainToDecibels (peak), 1) + " dBFS)");
        kb.release();
        check (! proc.keyLit (1, 69), "release unlights it");
        // ---- 9a. computer keys in normal mode: Live's layout
        const bool w = ed->keyPressed (juce::KeyPress ('w', {}, 'w'));
        check (w && proc.keyLit (1, 61), "normal: W plays C#4 (Live's computer keys)");
        ed->keyboardView().releaseAllComputerKeys();
        ed->keyPressed (juce::KeyPress ('x', {}, 'x'));
        check (ed->keyboardView().computerOctave() == 5, "X moves the computer keys one octave up");
        ed->keyPressed (juce::KeyPress ('z', {}, 'z'));
    }

    // ---- 10. the cable compartment
    {
        auto& bay = ed->patchBay();
        check (! bay.isVisible(), "the compartment is closed at start");
        ed->patchKey().onClick();
        int frames = 0;
        while (bay.openAmount() < 1.0f && frames < 60) { ed->tickForTest(); ++frames; }
        check (bay.isVisible() && bay.getHeight() == jfui::PatchBay::kFullH && frames >= 3 && frames <= 12,
               "PATCH rolls the door down in " + juce::String (frames) + " frames (" + juce::String (frames * 1000 / 30) + " ms)");
        int inside = 0, total = 0;
        for (auto& k : ed->allKnobs())
            if (k->getParentComponent() == &bay) { ++total; if (bay.getLocalBounds().contains (k->getBounds())) ++inside; }
        check (total == 22 && inside == total, "compartment: " + juce::String (inside) + " of " + juce::String (total) + " knobs inside (16 amounts + 3 x rate / fade)");
        proc.loadPresetIndex (presetIndex (proc, "Init", "Init"));
        for (int i = 0; i < 40; ++i) ed->tickForTest();
        juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer none; proc.processBlock (buf, none);
        const int before = bay.cableCount();
        bay.connect ((int) svl::ModSrc::lfo1, (int) svl::ModDst::cutoff);
        bay.connect ((int) svl::ModSrc::velocity, (int) svl::ModDst::fm);
        bay.connect ((int) svl::ModSrc::modWheel, (int) svl::ModDst::osc1Pw);
        proc.processBlock (buf, none);
        const float s1 = proc.apvts.getRawParameterValue ("mod1.source")->load(), d1 = proc.apvts.getRawParameterValue ("mod1.destination")->load(),
                    a1 = proc.apvts.getRawParameterValue ("mod1.amount")->load();
        check (before == 0 && bay.cableCount() == 3 && juce::roundToInt (s1) == (int) svl::ModSrc::lfo1 && juce::roundToInt (d1) == (int) svl::ModDst::cutoff
               && std::abs (a1 - 0.5f) < 1e-6f, "a cable sets a matrix slot (mod1: lfo1 -> cutoff, amount 0.5); 3 cables");
        // drag a cable out of its destination and drop it on another
        {
            const auto from = bay.jack (false, (int) svl::ModDst::cutoff - 1), to = bay.jack (false, (int) svl::ModDst::reso - 1);
            const auto t0 = juce::Time::getCurrentTime();
            juce::MouseInputSource src = juce::Desktop::getInstance().getMainMouseSource();
            bay.mouseDown (juce::MouseEvent (src, from, {}, 1.0f, 0, 0, 0, 0, &bay, &bay, t0, from, t0, 1, false));
            bay.mouseDrag (juce::MouseEvent (src, to, {}, 1.0f, 0, 0, 0, 0, &bay, &bay, t0, from, t0, 1, false));
            bay.mouseUp (juce::MouseEvent (src, to, {}, 1.0f, 0, 0, 0, 0, &bay, &bay, t0, from, t0, 1, false));
            proc.processBlock (buf, none);
            bool reso = false, cut = false;
            for (int m = 1; m <= 16; ++m)
            {
                const int sv = juce::roundToInt (proc.apvts.getRawParameterValue ("mod" + juce::String (m) + ".source")->load());
                const int dv = juce::roundToInt (proc.apvts.getRawParameterValue ("mod" + juce::String (m) + ".destination")->load());
                reso |= sv == (int) svl::ModSrc::lfo1 && dv == (int) svl::ModDst::reso;
                cut |= sv == (int) svl::ModSrc::lfo1 && dv == (int) svl::ModDst::cutoff;
            }
            check (reso && ! cut && bay.cableCount() == 3, "dragging the cable from CUTOFF to RESO moves it");
        }
        for (int i = 0; i < 4; ++i) ed->tickForTest();
        snap (*ed, "front_patch.png");
        ed->patchKey().onClick();
        frames = 0;
        while (bay.isVisible() && frames < 60) { ed->tickForTest(); ++frames; }
        check (! bay.isVisible(), "PATCH again rolls the door up (" + juce::String (frames) + " frames)");
        proc.loadPresetIndex (presetIndex (proc, "Init", "Init"));
        for (int i = 0; i < 40; ++i) ed->tickForTest();
    }

    // ---- the FX rack door (work order 20 Oct, item 4): its key in the REVERB title bar, 16 knobs, 3 selectors, the EQ curve,
    //      the gain reduction, double-click back to the loaded value
    {
        auto* fx = ed->fxDoorView();
        check (fx != nullptr && ! fx->isVisible(), "the FX door is closed at start");
        ed->openDoorForTest (5);
        int frames = 0;
        while (fx != nullptr && fx->openAmount() < 1.0f && frames < 60) { ed->tickForTest(); ++frames; }
        int knobsIn = 0, knobs = 0, steppers = 0;
        for (auto& k : ed->allKnobs()) if (k->getParentComponent() == fx) { ++knobs; if (fx->getLocalBounds().contains (k->getBounds())) ++knobsIn; }
        for (auto* c : fx->getChildren()) if (dynamic_cast<jfui::TypeStepper*> (c) != nullptr) ++steppers;
        check (fx->isVisible() && frames >= 3 && frames <= 12 && knobs == 16 && knobsIn == 16 && steppers == 3 && (int) fx->labelsForTest().size() == 16,
               "FX RACK rolls the door down in " + juce::String (frames) + " frames: " + juce::String (knobsIn) + " / " + juce::String (knobs)
                   + " knobs inside, " + juce::String (steppers) + " selectors (mod type, echo type, echo time), " + juce::String ((int) fx->labelsForTest().size()) + " labels");
        // a sound through EQ + COMP + phaser, then the snapshot
        proc.loadPresetIndex (presetIndex (proc, "Init", "Init"));
        for (auto [id, v] : { std::pair<const char*, float> { "fx.eq.low", 6.0f }, { "fx.eq.mid", -5.0f }, { "fx.eq.mid_freq", 2500.0f }, { "fx.eq.high", 4.0f },
                              { "fx.comp.ratio", 6.0f }, { "fx.comp.threshold", -30.0f }, { "fx.comp.makeup", 6.0f }, { "fx.mod.type", 3.0f }, { "fx.mod.mix", 0.7f },
                              { "fx.echo.type", 3.0f }, { "fx.echo.mix", 0.25f } })
            jfui::setParamGesture (proc.apvts.getParameter (id), v);
        juce::AudioBuffer<float> buf (2, 512);
        { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOn (1, 48, (juce::uint8) 110), 0); proc.processBlock (buf, m); }
        juce::MidiBuffer none;
        for (int b = 0; b < 40; ++b) proc.processBlock (buf, none);
        for (int i = 0; i < 20; ++i) ed->tickForTest();
        const float gr = proc.fxReductionDb();
        check (gr > 3.0f, "COMP's gain reduction reaches the door (" + juce::String (gr, 1) + " dB on a held saw at -30 dB threshold, 6:1)");
        snap (*ed, "front_fx.png");
        // double-click a door knob: back to the loaded preset's value (Init: 0 dB)
        jfui::Knob* mid = nullptr;
        for (auto& k : ed->allKnobs()) if (k->getParentComponent() == fx && k->paramId() == "fx.eq.mid") mid = k.get();
        if (mid != nullptr) mid->resetToLoaded();
        proc.processBlock (buf, none);
        check (mid != nullptr && std::abs (proc.exactValue (svl::FXQ_MID)) < 1.0e-4f && mid->gliding(),
               "double-click on EQ MID: back to the loaded 0 dB, the knob glides (" + juce::String (proc.exactValue (svl::FXQ_MID), 2) + " dB)");
        proc.loadPresetIndex (presetIndex (proc, "Init", "Init"));
        { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::allNotesOff (1), 0); proc.processBlock (buf, m); }
        ed->openDoorForTest (0);
        frames = 0;
        while (fx->isVisible() && frames < 60) { ed->tickForTest(); ++frames; }
        check (! fx->isVisible(), "the FX door rolls up again (" + juce::String (frames) + " frames)");
        for (int i = 0; i < 40; ++i) ed->tickForTest();
    }

    // ---- SEQ and LAB doors: one door at a time, both roll down over the modules
    {
        auto settle = [&] { for (int i = 0; i < 40; ++i) ed->tickForTest(); pump(); };
        ed->seqKey().onClick(); settle();
        check (ed->seqDoorView() != nullptr && ed->seqDoorView()->isVisible() && ed->seqDoorView()->stepKeyCount() == 16 && ed->seqDoorView()->controlCount() >= 16 * 7 + 9,
               "SEQ opens its analog door: 16 step columns (key, NOTE / VEL / GATE knobs, ROW / SLIDE / ACCENT switches) + the arp and seq controls ("
               + juce::String (ed->seqDoorView() ? ed->seqDoorView()->controlCount() : 0) + " controls)");
        check (ed->closeKey().isVisible() && ed->seqDoorView()->getBounds().contains (ed->closeKey().getBounds().getCentre()),
               "an open door shows its CLOSE key on its bottom edge");
        snap (*ed, "front_seq.png");
        {   // the VEL row's lanes (work order item 5): PROB and RATCH in the same places, one lane at a time
            auto* sd = ed->seqDoorView();
            const int v = sd->laneKnobCount (0), pr = sd->laneKnobCount (1), ra = sd->laneKnobCount (2);
            sd->showLaneForTest (1);
            int shown = 0;
            for (auto& k : ed->allKnobs()) if (k->getParentComponent() == sd && k->isVisible() && k->paramId().endsWith (".prob")) ++shown;
            int velShown = 0;
            for (auto& k : ed->allKnobs()) if (k->getParentComponent() == sd && k->isVisible() && k->paramId().endsWith (".vel")) ++velShown;
            snap (*ed, "front_seq_prob.png");
            sd->showLaneForTest (0);
            check (v == 16 && pr == 16 && ra == 16 && shown == 16 && velShown == 0,
                   "SEQ lanes: VEL / PROB / RATCH " + juce::String (v) + " / " + juce::String (pr) + " / " + juce::String (ra) + " knobs; PROB shows 16, VEL hidden while it does");
        }
        {   // CLEAR: a changed step goes back to its default
            auto* note = proc.apvts.getParameter ("seq.s3.note");
            jfui::setParamGesture (note, 7.0f);
            juce::Component* clear = nullptr;
            for (auto* c : ed->seqDoorView()->getChildren()) if (auto* k = dynamic_cast<jfui::PanelKey*> (c)) if (k->text == "CLEAR") clear = k;
            if (clear) static_cast<jfui::PanelKey*> (clear)->onClick();
            check (clear != nullptr && std::abs (note->getValue() - note->getDefaultValue()) < 1.0e-6f, "SEQ's CLEAR puts the steps back to their defaults");
        }
        {   // PLAY: seq on + free; again: hold
            jfui::PanelKey* play = nullptr;
            for (auto* c : ed->seqDoorView()->getChildren()) if (auto* k = dynamic_cast<jfui::PanelKey*> (c)) if (k->text == "PLAY") play = k;
            const bool ok = play != nullptr && (play->onClick(), proc.apvts.getRawParameterValue ("seq.on")->load() > 0.5f && proc.apvts.getRawParameterValue ("seq.mode")->load() > 0.5f)
                            && play->isOn() && (play->onClick(), proc.apvts.getRawParameterValue ("seq.mode")->load() < 0.5f);
            check (ok, "SEQ's PLAY runs the sequence without a key (free), again stops it (hold)");
        }
        ed->closeKey().onClick(); settle();
        check (! ed->seqDoorView()->isVisible() && ! ed->closeKey().isVisible(), "CLOSE rolls the door up and hides itself");
        ed->seqKey().onClick(); settle();
        ed->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey)); settle();
        check (! ed->seqDoorView()->isVisible(), "Esc rolls an open door up");
        {   // Esc on the panel stops a sequence / beat that runs on its own
            jfui::setParamGesture (proc.apvts.getParameter ("seq.mode"), 1.0f);
            jfui::setParamGesture (proc.apvts.getParameter ("drums.mode"), 1.0f);
            ed->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey));
            check (proc.apvts.getRawParameterValue ("seq.mode")->load() < 0.5f && proc.apvts.getRawParameterValue ("drums.mode")->load() < 0.5f,
                   "Esc on the panel stops a free-running sequence and beat (back to waiting for a key)");
        }
        ed->drumKey().onClick(); settle();
        check (ed->drumDoorView() != nullptr && ed->drumDoorView()->isVisible() && ! ed->seqDoorView()->isVisible(), "DRUM opens its door and closes SEQ");
        if (proc.apvts.getParameter ("drums.on") != nullptr)
            check (ed->drumDoorView()->stepKeyCount() == 8 * 16, "the drum door has 8 tracks x 16 step keys (" + juce::String (ed->drumDoorView()->stepKeyCount()) + ")");
        {   // OCT: the selector's 5 positions are -2..+2
            auto* oct = proc.apvts.getParameter ("drums.octave");
            jfui::ChickenSelector* sel = nullptr;
            for (auto* c : ed->drumDoorView()->getChildren()) if (auto* cs = dynamic_cast<jfui::ChickenSelector*> (c)) if (cs->getTooltip().containsIgnoreCase ("oct")) sel = cs;
            bool ok = oct != nullptr && sel != nullptr && sel->index() == 2;
            if (ok) { jfui::setParamGesture (oct, 1.0f); ok = sel->index() == 3; jfui::setParamGesture (oct, -2.0f); ok = ok && sel->index() == 0; jfui::setParamGesture (oct, 0.0f); }
            check (ok, "the drum OCT selector shows -2..+2 at its five positions");
        }
        snap (*ed, "front_drum.png");
        ed->labKey().onClick(); settle();
        check (ed->labDoorView() != nullptr && ed->labDoorView()->isVisible() && ! ed->seqDoorView()->isVisible(), "LAB opens its door and closes SEQ");
        check (ed->labDoorView()->locked() == ! jflab::Lab::kUnlockedByDefault && ! proc.labActive(),
               juce::String ("the Tuning Lab is ") + (jflab::Lab::kUnlockedByDefault ? "unlocked" : "locked") + " in this build and off until LAB ON");
        jflab::Lab::setUnlockedForTests (false);
        ed->labKey().onClick(); settle(); ed->labKey().onClick(); settle();
        snap (*ed, "front_lab_locked.png");
        // the same door unlocked, a learned-looking directional tuning in A and a chord held (a picture of what it shows)
        jflab::Lab::setUnlockedForTests (true);
        JingFangProcessor::LabState ls; ls.on = true;
        ls.a.name = "Learned line";
        ls.a.cents = { 180, 290, 500, 700, 830, 1000, 1200 };
        ls.a.fall = { 180, 270, 500, 700, 830, 1000, 1200 };
        ls.a.confidence = { 1, 0.9f, 0.8f, 0.9f, 1, 0.7f, 0.8f };
        proc.setLabState (ls);
        proc.uiNoteOn (60, -1, 0.8f); proc.uiNoteOn (64, -1, 0.8f); proc.uiNoteOn (67, -1, 0.8f);
        { juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer m; proc.processBlock (buf, m); }
        settle();
        check (proc.labSounding().size() == 3, "unlocked Lab reports the 3 sounding notes for the interval view (" + juce::String ((int) proc.labSounding().size()) + ")");
        snap (*ed, "front_lab_unlocked.png");
        proc.uiAllNotesOff();
        { juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer m; proc.processBlock (buf, m); }
        proc.setLabState ({});
        // LEARN works on its own page (owner 19 Oct): while it listens no other page opens (keys, Esc), the page keys are dimmed;
        // when it is done the learned tuning plays (Lab on) and the keys work again
        {
            const auto wav = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("jf_learn_line.wav");
            {
                const double sr = 44100;
                const int notes[] = { 62, 64, 66, 67, 69, 67, 66, 64 };
                juce::AudioBuffer<float> b (1, (int) (sr * 0.4) * 8 * 4);
                for (int i = 0; i < b.getNumSamples(); ++i)
                {
                    const int n = notes[(i / (int) (sr * 0.4)) % 8];
                    b.setSample (0, i, 0.4f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * std::pow (2.0, (n - 69) / 12.0) * i / sr));
                }
                wav.deleteFile();
                if (auto os = std::unique_ptr<juce::OutputStream> (wav.createOutputStream()))
                {
                    juce::WavAudioFormat fmt;
                    if (auto* w = fmt.createWriterFor (os.get(), sr, 1, 24, {}, 0)) { os.release(); std::unique_ptr<juce::AudioFormatWriter> wr (w); wr->writeFromAudioSampleBuffer (b, 0, b.getNumSamples()); }
                }
            }
            ed->labDoorView()->loadFileForTest (wav, 0);                // what a drop or the LEARN chooser does
            ed->tickForTest();                                          // no message loop yet: LEARN is still listening
            const bool open = ed->labDoorView()->learnOpen();
            ed->seqKey().onClick(); ed->drumKey().onClick(); ed->patchKey().onClick(); ed->closeKey().onClick(); ed->labKey().onClick();
            ed->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey));
            const bool stayed = ed->labDoorView()->isOpenOrOpening() && ! ed->seqDoorView()->isOpenOrOpening() && ! ed->drumDoorView()->isOpenOrOpening()
                             && ! ed->patchBay().isOpenOrOpening();
            const bool dimmed = ! ed->seqKey().isEnabled() && ed->seqKey().getAlpha() < 0.5f && ! ed->closeKey().isEnabled();
            check (open && stayed && dimmed, "while LEARN listens no other page opens (SEQ / DRUM / PATCH / CLOSE / LAB / Esc) and the page keys are dimmed");
            const auto t0 = juce::Time::getMillisecondCounter();
            while (ed->labDoorView()->learnOpen() && juce::Time::getMillisecondCounter() - t0 < 30000) juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
            ed->tickForTest();
            check (! ed->labDoorView()->learnOpen() && ed->labDoorView()->statusText().contains ("learned") && proc.labActive() && ed->seqKey().isEnabled()
                   && ed->seqKey().getAlpha() > 0.99f,
                   "after LEARN the learned tuning plays (Lab on) and the page keys work again (" + ed->labDoorView()->statusText() + ")");
            wav.deleteFile();
            proc.setLabState ({});
        }
        {   // LOAD's built-in list (what a pick in the menu does): card B gets the tuning, the Lab plays it
            ed->labDoorView()->loadBuiltinForTest (1, 2);
            settle();
            const auto& ls = proc.labState();
            check (ls.on && ls.b.name == jflab::builtin::entries()[2].name && ed->labDoorView()->statusText().contains (ls.b.name)
                   && ! proc.labFromPreset(),
                   "LOAD built-in: card B = " + juce::String (ls.b.name) + ", Lab on (" + ed->labDoorView()->statusText() + ")");
            snap (*ed, "lab_builtin.png");
            proc.setLabState ({});
        }
        jflab::Lab::setUnlockedForTests (false);
        ed->labKey().onClick(); settle();
        check (! ed->labDoorView()->isVisible(), "LAB again rolls the door up");
    }

    // ---- no ghost of a door after it rolls up (18 Oct: buffering the whole panel left the PATCH door's image on the face)
    {
        auto settle = [&] { for (int i = 0; i < 40; ++i) ed->tickForTest(); pump(); };
        proc.loadPresetIndex (presetIndex (proc, "Init", "Init")); settle();
        const auto before = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
        for (auto open : { std::function<void()> ([&] { ed->patchKey().onClick(); }), std::function<void()> ([&] { ed->seqKey().onClick(); }),
                           std::function<void()> ([&] { ed->drumKey().onClick(); }), std::function<void()> ([&] { ed->labKey().onClick(); }) })
        { open(); settle(); ed->closeKey().onClick(); settle(); }
        const auto after = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
        int diff = 0;
        for (int y = 0; y < before.getHeight(); y += 2)
            for (int x = 0; x < before.getWidth(); x += 2)
            {
                const auto a = before.getPixelAt (x, y), b = after.getPixelAt (x, y);
                auto trace = [] (juce::Colour c) { return c.getGreen() > c.getRed() + 40 && c.getGreen() > c.getBlue() + 30; };
                if (trace (a) || trace (b)) continue;   // the screens' live traces move on their own
                if (std::abs (a.getRed() - b.getRed()) + std::abs (a.getGreen() - b.getGreen()) + std::abs (a.getBlue() - b.getBlue()) > 60) ++diff;
            }
        if (diff >= 200) { snapToFile (before, "ghost_before.png"); snapToFile (after, "ghost_after.png"); }
        check (diff < 200, "after opening and closing every door the panel looks as before (" + juce::String (diff) + " sampled pixels differ)");
    }

    // ---- SEQ: an instrument preset opens an empty grid, a Sequence preset opens its own pattern to edit (owner 18 Oct)
    {
        auto settle = [&] { for (int i = 0; i < 40; ++i) ed->tickForTest(); pump(); };
        auto stepsOn = [&] { int n = 0; for (int st = 1; st <= 16; ++st) n += proc.apvts.getRawParameterValue ("seq.s" + juce::String (st) + ".on")->load() > 0.5f; return n; };
        proc.loadPresetIndex (presetIndex (proc, "Pluck", "Saw Pluck")); settle();
        ed->seqKey().onClick(); settle();
        const int fromScratch = stepsOn();
        ed->closeKey().onClick(); settle();
        int seqIdx = -1;
        for (size_t i = 0; i < proc.presets().size(); ++i) if (proc.presets()[i].family.startsWith ("Sequence")) { seqIdx = (int) i; break; }
        proc.loadPresetIndex (seqIdx); settle();
        const int inPreset = stepsOn();
        ed->seqKey().onClick(); settle();
        const int editing = stepsOn();
        check (fromScratch == 0 && inPreset > 0 && editing == inPreset, "SEQ on an instrument: empty grid (" + juce::String (fromScratch) + " steps on); on a Sequence preset: its pattern ("
               + juce::String (editing) + " of " + juce::String (inPreset) + " steps kept)");
        ed->closeKey().onClick(); settle();
        proc.loadPresetIndex (presetIndex (proc, "Init", "Init")); settle();
    }

    // ---- every control on the doors answers a real click (owner 18 Oct: "buralardaki birçok buton tuş işlevsiz")
    {
        auto settle = [&] { for (int i = 0; i < 40; ++i) ed->tickForTest(); pump(); };
        auto click = [] (juce::Component& c, juce::Point<int> at, bool right = false)
        {
            auto src = juce::Desktop::getInstance().getMainMouseSource();
            const auto now = juce::Time::getCurrentTime();
            const juce::ModifierKeys mods (right ? juce::ModifierKeys::rightButtonModifier : juce::ModifierKeys::leftButtonModifier);
            juce::MouseEvent down (src, at.toFloat(), mods, juce::MouseInputSource::defaultPressure, 0, 0, 0, 0, &c, &c, now, at.toFloat(), now, 1, false);
            c.mouseDown (down);
            juce::MouseEvent up (src, at.toFloat(), mods.withoutMouseButtons(), juce::MouseInputSource::defaultPressure, 0, 0, 0, 0, &c, &c, now, at.toFloat(), now, 1, false);
            c.mouseUp (up);
        };
        struct Door { juce::String name; juce::Component* door; std::function<void()> open; };
        std::vector<Door> doors { { "SEQ", ed->seqDoorView(), [&] { ed->seqKey().onClick(); } },
                                  { "DRUM", ed->drumDoorView(), [&] { ed->drumKey().onClick(); } } };
        for (auto& d : doors)
        {
            d.open(); settle();
            juce::StringArray dead;
            int tried = 0;
            for (auto* c : d.door->getChildren())
            {
                if (auto* sw = dynamic_cast<jfui::BatSwitch*> (c))
                {
                    ++tried;
                    // a click anywhere on the switch (here: on the lever, the upper part, where the old rule did nothing) moves it
                    const int before = sw->position();
                    click (*sw, juce::Point<int> (sw->getWidth() / 2, 4));
                    if (sw->position() == before) dead.add ("switch " + sw->getTooltip());
                    for (int i = 0; i < 2 && sw->position() != before; ++i) click (*sw, juce::Point<int> (sw->getWidth() / 2, 4));   // back to where it was
                }
                else if (auto* cs = dynamic_cast<jfui::ChickenSelector*> (c))
                {
                    ++tried;
                    const int before = cs->index();
                    click (*cs, cs->getLocalBounds().getCentre());
                    if (cs->index() == before) dead.add ("selector " + cs->getTooltip());
                }
                else if (auto* sk = dynamic_cast<jfui::StepKey*> (c))
                {
                    ++tried;
                    const int before = sk->state();
                    click (*sk, sk->getLocalBounds().getCentre());
                    if (sk->state() == before) dead.add ("step key");
                    click (*sk, sk->getLocalBounds().getCentre(), true);   // right-click: off
                }
            }
            for (auto& n : dead) std::printf ("  dead on %s: %s\n", d.name.toRawUTF8(), n.toRawUTF8());
            check (tried > 0 && dead.isEmpty(), d.name + ": every switch, selector and step key answers a click (" + juce::String (tried) + " tried, "
                   + juce::String (dead.size()) + " dead)");
            ed->closeKey().onClick(); settle();
        }
    }

    // ---- the layout, for the Blender scene (tools/blender/)
    {
        const auto f = gOut.getChildFile ("layout.json");
        f.replaceWithText (ed->layoutJson());
        const auto masks = jfui::dragonMasksScaled (2.0f);
        for (auto [name, img] : { std::pair<const char*, juce::Image> ("dragon_body_2x.png", masks.first), { "dragon_lines_2x.png", masks.second } })
        {
            juce::Image grey (juce::Image::RGB, img.getWidth(), img.getHeight(), false);
            for (int y = 0; y < img.getHeight(); ++y)
                for (int x = 0; x < img.getWidth(); ++x) { const auto a = img.getPixelAt (x, y).getAlpha(); grey.setPixelAt (x, y, juce::Colour (a, a, a)); }
            const auto mf = gOut.getChildFile (name); mf.deleteFile();
            juce::FileOutputStream os (mf); juce::PNGImageFormat().writeImageToStream (grey, os);
        }
        std::printf ("  wrote %s\n", f.getFullPathName().toRawUTF8());
    }

    // ---- the dragon alone (for review): body and line work on the chassis colour
    {
        juce::Image img (juce::Image::ARGB, jfui::kW, jfui::kH, true);
        juce::Graphics g (img);
        g.fillAll (jfui::Pal::ground);
        g.setColour (jfui::Pal::inkSoft); g.drawImageAt (jfui::dragonMask(), 0, 0, true);
        g.setColour (jfui::Pal::gold); g.drawImageAt (jfui::dragonLines(), 0, 0, true);
        const auto f = gOut.getChildFile ("dragon_alone.png");
        f.deleteFile();
        juce::FileOutputStream os (f);
        juce::PNGImageFormat().writeImageToStream (img, os);
    }

    // ---- 200 %
    {
        proc.loadPresetIndex (presetIndex (proc, "Keys - E-Piano", "Tine EP"));
        for (int i = 0; i < 40; ++i) ed->tickForTest();
        play (proc, { 60, 64, 67, 71 });
        pump();
        {
            const auto lv = proc.live();
            float sc[JingFangProcessor::kScopeLen]; proc.copyScope (sc);
            float pk = 0.0f; for (float v : sc) pk = juce::jmax (pk, std::abs (v));
            check (lv.voices >= 4 && lv.level > 0.01f && pk > 0.001f, "live view while playing: " + juce::String (lv.voices) + " voices, level "
                   + juce::String (lv.level, 3) + ", output peak " + juce::String (pk, 3) + " (the screens move)");
        }
        snap (*ed, "front_200.png", 2.0f);
        // the preset synthesizer: OSC 1 from preset 5, OSC 2 from 9, the amp envelope from 17
        proc.loadSection (0, 4); proc.loadSection (1, 8); proc.loadSection (4, 16);
        for (int i = 0; i < 40; ++i) ed->tickForTest();
        pump();
        check (proc.sectionSlot (0) == 4 && proc.sectionSlot (1) == 8 && proc.sectionSlot (4) == 16 && ed->slotCell (JingFangProcessor::kNumSections - 1) != nullptr,
               "the slots show 005 / 009 / 017 and every slot cell exists");
        snap (*ed, "front_mix_200.png", 2.0f);
    }
    std::printf ("\nui_snapshot: %d passed, %d failed\n", gPass, gFail);
    ed = nullptr;
    return gFail == 0 ? 0 : 1;
}
