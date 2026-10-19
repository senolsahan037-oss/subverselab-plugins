#include "PluginEditor.h"

using namespace jfui;
using R = juce::Rectangle<int>;

JingFangEditor::JingFangEditor (JingFangProcessor& p) : juce::AudioProcessorEditor (&p), proc (p)
{
    setOpaque (true);
    addAndMakeVisible (panel);
    build();
    help.setInterceptsMouseClicks (true, false);
    panel.addChildComponent (help);
    help.setBounds (0, 0, kW, kH);
    help.onClosed = [this] { if (proc.isStandalone()) grabKeyboardFocus(); };

    // in a DAW every key belongs to the DAW: JING FANG takes no computer keys and asks for no focus (RU-8 rule 4)
    setWantsKeyboardFocus (proc.isStandalone());
    setResizable (true, true);
    if (auto* c = getConstrainer())
    {
        c->setFixedAspectRatio ((double) kW / (double) kH);
        c->setSizeLimits (juce::roundToInt (kW * 0.7), juce::roundToInt (kH * 0.7), juce::roundToInt (kW * 1.6), juce::roundToInt (kH * 1.6));
    }
    // open at full size when the screen has room, else scaled down to fit (measured 18 Oct on the owner's 1920 x 1080: the
    // menu bar is 30 px, so 1024 + 28 title bar + 30 left the window 2 px past the bottom)
    float fit = 1.0f;
    if (auto* d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        fit = jfui::fitScale (d->userArea.getWidth(), d->userArea.getHeight());
    setSize (juce::roundToInt (kW * fit), juce::roundToInt (kH * fit));
    startTimerHz (30);
    if (proc.isStandalone()) juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<JingFangEditor> (this)]
                                                              { if (safe != nullptr && safe->isShowing()) safe->grabKeyboardFocus(); });
}

JingFangEditor::~JingFangEditor()
{
    stopTimer();
    keyboard.releaseAllComputerKeys();
}

jfui::Knob& JingFangEditor::addKnob (const juce::String& id, const juce::String& label, R cell, juce::Component* parent)
{
    knobs.push_back (std::make_unique<Knob> (proc, id, label));
    auto& k = *knobs.back();
    (parent != nullptr ? parent : (juce::Component*) &panel)->addAndMakeVisible (k);
    k.setBounds (cell);
    return k;
}

void JingFangEditor::addScope (jfui::Scope* sc, R r)
{
    others.emplace_back (sc);
    scopes.push_back (sc);
    panel.addAndMakeVisible (sc);
    sc->setBounds (r);
}

void JingFangEditor::build()
{
    auto addOther = [this] (juce::Component* c, R r) { others.emplace_back (c); panel.addAndMakeVisible (c); c->setBounds (r); return c; };
    // v3 (owner 17 Oct): every position comes from the layout the Blender scene was rendered from (tools/blender/v2/layout_v2.py
    // -> layout_v3.json, compiled in), so the controls sit on the rendered device by construction.
    const auto& L = panelLayout();
    auto num = [] (const juce::var& v) { return juce::roundToInt ((double) v); };
    auto rectOf = [num] (const juce::var& a) { return R (num (a[0]), num (a[1]), num (a[2]), num (a[3])); };
    auto list = [&L] (const char* k) -> const juce::Array<juce::var>& { static const juce::Array<juce::var> none; auto* a = L[k].getArray(); return a != nullptr ? *a : none; };
    auto btn = [&] (const juce::String& id) { for (auto& b : list ("buttons")) if (b["id"].toString() == id) return rectOf (b["rect"]); return R(); };

    // ---- header: preset browser and the keys beside it
    panel.addAndMakeVisible (prevBtn); prevBtn.arrow = -1; prevBtn.setBounds (btn ("prev")); prevBtn.onClick = [this] { proc.stepPreset (-1); };
    panel.addAndMakeVisible (presetFace); presetFace.setBounds (rectOf (L["synth_lcd"]));
    panel.addAndMakeVisible (bpmFace); bpmFace.setBounds (rectOf (L["bpm_lcd"]));
    // the preset synthesizer's slots, one per panel
    if (auto* sl = L["slots"].getArray())
        for (int i = 0; i < juce::jmin (sl->size(), JingFangProcessor::kNumSections); ++i)
        {
            slotCells.push_back (std::make_unique<SlotCell> (proc, i));
            panel.addAndMakeVisible (*slotCells.back());
            slotCells.back()->setBounds (rectOf ((*sl)[i]));
        }
    panel.addAndMakeVisible (nextBtn); nextBtn.arrow = 1; nextBtn.setBounds (btn ("next")); nextBtn.onClick = [this] { proc.stepPreset (1); };
    panel.addAndMakeVisible (saveBtn); saveBtn.setBounds (btn ("save")); saveBtn.onClick = [this] { savePreset(); };
    prevBtn.setTooltip ("Previous preset"); nextBtn.setTooltip ("Next preset"); saveBtn.setTooltip ("Save as your own preset");
    panel.addAndMakeVisible (patchBtn); patchBtn.setBounds (btn ("patch")); patchBtn.setTooltip ("Open the cable compartment");
    patchBtn.isOn = [this] { return bay.isOpenOrOpening(); };
    patchBtn.onClick = [this] { openDoor (bay.isOpenOrOpening() ? 0 : 1); };
    panel.addAndMakeVisible (seqBtn); seqBtn.setBounds (btn ("seq")); seqBtn.setTooltip ("Open the arpeggiator and the step sequencer");
    seqBtn.isOn = [this] { return (seqDoor != nullptr && seqDoor->isOpenOrOpening()) || proc.live().seqStep >= 0; };   // lit: open, or a sequence plays
    seqBtn.onClick = [this] { openDoor (seqDoor != nullptr && seqDoor->isOpenOrOpening() ? 0 : 2); };
    panel.addAndMakeVisible (drumBtn); drumBtn.setBounds (btn ("drum")); drumBtn.setTooltip ("Open the drum machine (lit while a beat plays; Esc on the panel stops it)");
    drumBtn.isOn = [this] { return (drumDoor != nullptr && drumDoor->isOpenOrOpening()) || proc.drumStep() >= 0; };   // lit: open, or a beat plays
    drumBtn.onClick = [this] { openDoor (drumDoor != nullptr && drumDoor->isOpenOrOpening() ? 0 : 4); };
    panel.addChildComponent (closeBtn);
    closeBtn.setTooltip ("Back to the panel (Esc)");
    closeBtn.onClick = [this] { openDoor (0); };
    panel.addAndMakeVisible (helpBtn); helpBtn.setBounds (btn ("help")); helpBtn.fontPx = 14.0f; helpBtn.setTooltip ("Help");
    helpBtn.isOn = [this] { return help.isOpen(); };
    helpBtn.onClick = [this] { if (help.isOpen()) help.close(); else { help.setCovered (anyDoorOpen() ? doorArea : R()); help.open(); } helpBtn.repaint(); };

    // ---- modules, knobs (photoreal, labels printed on the device), selectors, screens
    for (auto& s : list ("sections")) sections.push_back ({ s["title"].toString(), rectOf (s["rect"]) });
    for (auto& k : list ("knobs"))
    {
        const float cx = (float) (double) k["c"][0], cy = (float) (double) k["c"][1], d = (float) (double) k["d"];
        const int box = juce::roundToInt (d + 8.0f);
        auto& kn = addKnob (k["id"].toString(), k["label"].toString(), R (juce::roundToInt (cx - box * 0.5f), juce::roundToInt (cy - box * 0.5f), box, box));
        kn.setPrinted (true);
    }
    for (auto& st : list ("steppers")) addOther (new TypeStepper (proc, st["id"].toString()), rectOf (st["rect"]));
    for (auto& sg : list ("segments"))
    {
        juce::StringArray labels;
        if (auto* a = sg["labels"].getArray()) for (auto& l : *a) labels.add (l.toString());
        addOther (new Segments (proc, sg["id"].toString(), labels), rectOf (sg["rect"]));
    }
    for (auto& sc : list ("scopes"))
    {
        const auto id = sc["id"].toString(); const auto r = rectOf (sc["rect"]);
        if (id.startsWith ("osc")) addScope (new OscScope (proc, id.getTrailingIntValue() - 1), r);
        else if (id == "filter") addScope (new FilterScope (proc), r);
        else if (id == "amp_env" || id == "filter_env") addScope (new EnvScope (proc, id + "."), r);
        else if (id == "pitch") addScope (new PitchScope (proc), r);
    }

    // ---- keyboard deck and the NORMAL / MICRO keys
    panel.addAndMakeVisible (normalBtn); normalBtn.setBounds (btn ("normal"));
    panel.addAndMakeVisible (microBtn); microBtn.setBounds (btn ("micro"));
    panel.addAndMakeVisible (labBtn); labBtn.setBounds (btn ("lab"));
    labBtn.isOn = [this] { return labDoor != nullptr && labDoor->isOpenOrOpening(); };
    labBtn.onClick = [this] { openDoor (labDoor != nullptr && labDoor->isOpenOrOpening() ? 0 : 3); };
    labBtn.setTooltip (JingFangProcessor::labUnlocked() ? "Open the Tuning Lab" : "Open the Tuning Lab (locked in this version)");
    normalBtn.isOn = [this] { return proc.exactValue (svl::TUNE_MODE) < 0.5f; };
    microBtn.isOn = [this] { return proc.exactValue (svl::TUNE_MODE) > 0.5f; };
    normalBtn.setTooltip ("Every key plays its usual note");
    microBtn.setTooltip ("Each pitch class plays on its row: low, mid or high");
    auto* mode = proc.apvts.getParameter ("tuning.mode");
    normalBtn.onClick = [this, mode] { setParamGesture (mode, 0.0f); keyboard.releaseAllComputerKeys(); };
    microBtn.onClick = [this, mode] { setParamGesture (mode, 1.0f); keyboard.releaseAllComputerKeys(); };
    panel.addAndMakeVisible (keyboard);
    keyboard.setBounds (rectOf (L["keyboard"]));
    panel.addAndMakeVisible (lockup);
    lockup.setBounds (rectOf (L["lockup"]));
    // the dragon over everything below the cable compartment and the help (click-through)
    panel.addAndMakeVisible (dragon);
    dragon.setBounds (0, 0, kW, kH);
        // (the big renders are drawn through drawScaledCached: buffering the whole panel left ghosts of the doors)
    // the cable compartment: a door that rolls down over the modules
    panel.addChildComponent (bay);
    {   // the same place as the other doors (doors.json "origin": the modules' area)
        const auto& o = jfui::doorsLayout()["origin"];
        bay.setTopLeftPosition ((int) o[0], (int) o[1]);
    }
    bay.snapOpen (false);
    // the SEQ door: over all the modules (rows A..D)
    {
        R area;
        for (auto& sct : sections) if (sct.title != "PLAY") area = area.isEmpty() ? sct.r : area.getUnion (sct.r);
        auto maker = [this] (const juce::String& id, const juce::String& l, juce::Rectangle<int> r, juce::Component* parent) -> jfui::Knob&
                     { return addKnob (id, l, r, parent); };
        doorArea = area.expanded (4);
        seqDoor = std::make_unique<jfui::AnalogDoor> (proc, maker, area.expanded (4), "seq", [this] { return proc.live().seqStep; });
        panel.addChildComponent (*seqDoor);
        seqDoor->snapOpen (false);
        drumDoor = std::make_unique<jfui::AnalogDoor> (proc, maker, area.expanded (4), "drum", [this] { return proc.drumStep(); });
        panel.addChildComponent (*drumDoor);
        drumDoor->snapOpen (false);
        // the Tuning Lab door: the same place
        labDoor = std::make_unique<jfui::LabDoor> (proc, area.expanded (4));
        panel.addChildComponent (*labDoor);
        labDoor->snapOpen (false);
    }

    // ---- help: one popover per section
    auto sec = [this] (const juce::String& t) { for (auto& s : sections) if (s.title == t) return s.r; return R(); };
    // the number sits just after the section's title, where nothing else is drawn
    auto mark = [&] (const juce::String& t)
    {
        const auto r = sec (t);
        return juce::Point<int> (r.getX() + 16 + juce::roundToInt (trackedWidth (t, heros (10.5f, true), 0.22f)) + 12, r.getY() + 11);
    };
    help.setEntries ({
        { "Preset synthesizer", "The arrows load a whole preset; every slot below then shows its name. Each slot is one panel of the "
                     "synth: give it another preset and only that panel takes that preset's settings - OSC 1 from 5, OSC 2 from 9, the "
                     "amp envelope from 17. Click the left or right of a slot to step, the middle for the list, or use the wheel. SAVE "
                     "keeps the mix as your own preset, with where each panel came from. Double-click a knob: back to its slot's value.", presetFace.getBounds(), { saveBtn.getX() - 22, saveBtn.getBounds().getCentreY() } },
        { "Oscillators", "Three oscillators: saw, square (PW sets the width), triangle, sine, white or pink noise, and string (a plucked "
                         "string; RING is how long it rings, BRIGHT how bright). OCT, SEMI and FINE tune each one; DECAY fades it on "
                         "its own after the note starts (0 = no fade). UNISON stacks up to 8 copies of a saw / square / triangle / sine, "
                         "DETUNE spreads their pitch and SPREAD their place left to right.", sec ("OSC 1"), mark ("OSC 1") },
        { "Filter", "LP, BP, HP or notch. KEY makes the cutoff follow the key; ENV moves it with the filter envelope and VEL with "
                    "velocity, both in octaves. DRIVE saturates the signal before the filter.", sec ("FILTER"), mark ("FILTER") },
        { "Envelopes", "Attack, decay, sustain and release of the level and of the filter. The three curve knobs bend the attack, decay "
                       "and release stages.", sec ("AMP ENVELOPE"), mark ("AMP ENVELOPE") },
        { "Pitch, click, drum", "PITCH adds a falling pitch at the start of every note (kicks, 808s, toms); P DEC is how fast it falls. "
                                "CLICK is a short noise burst through a high-pass (C TONE). DRUM plays every key at the DRUM pitch, D KEY "
                                "brings the key back in. CHOKE: a new note cuts the notes of the same group.", sec ("PITCH  CLICK  DRUM"), mark ("PITCH  CLICK  DRUM") },
        { "Drift and floor", "Slow random movement of pitch and level per voice, the life inside a held note. FLOOR puts quiet pink noise "
                             "under the note.", sec ("DRIFT  FLOOR"), mark ("DRIFT  FLOOR") },
        { "Voice", "POLY plays chords; MONO and LEGATO play one note (LEGATO does not restart the envelopes). GLIDE slides between "
                   "notes. VEL: how much velocity changes the level. BEND: pitch-wheel range in semitones.", sec ("VOICE  OUTPUT"), mark ("VOICE  OUTPUT") },
        { "Tuning", "GRID divides the octave into equal steps (53: one step is 22.64 cents). HIGH ROW and LOW ROW: how many steps the "
                    "high and low rows are away. A4: the reference pitch. KEYSWITCH: 24 keys from the KEYSWITCH key move a pitch class "
                    "while you play (first octave to low, second to high, again to mid); they never sound.", sec ("TUNING"), mark ("TUNING") },
        { "Normal and micro", "NORMAL: every key plays its usual note, from any MIDI keyboard, the host or the computer keys. MICRO: "
                              "each pitch class plays on the row shown above the keyboard. LAB opens the Tuning Lab.", sec ("PLAY"), mark ("PLAY") },
        { "Tuning Lab", "Two tunings, A and B. LOAD reads Scala files (.scl, .kbm); LEARN listens to a recording of a solo line and "
                        "finds its steps, and when a step sounds lower on the way down than on the way up, it keeps both; EXPORT "
                        "writes them back as Scala files. MORPH moves every key from A to B. DIRECTION plays a step's falling value "
                        "when the melody comes down onto it. ADAPTIVE lets each new note settle on the simple ratio nearest its "
                        "tuned pitch (within the WINDOW); HOME PULL keeps chords from drifting away. WHAT SOUNDS lists each interval "
                        "you hold: its cents, the nearest ratio, how far off it is and how fast it beats.", labBtn.getBounds(), mark ("PLAY") + juce::Point<int> (24, 0) },
        { "The three rows", "Top = high row, middle = the usual keys, bottom = low row. Normal: click to play (the outer rows sound "
                            "one row away); computer keys as in Live: A W S E D F T G Y H U J K, Z / X octave. Micro: a click plays; "
                            "click the strip above (or right-click a key) to move its pitch class low, mid, high. Computer keys: Z row low, A row mid, Q row high, "
                            "the letters are the white keys, Shift sharpens; left / right arrow: octave.", keyboard.getBounds(), { keyboard.getX() + 14, keyboard.getY() + 12 } },
        { "Effects", "DRIVE warms and pushes the sound (TONE darkens or brightens it). ECHO repeats it in time with the song: TIME is the "
                     "note length, FEEDBACK how many repeats, DAMP darkens each repeat. REVERB puts it in a room or a hall: SIZE, DECAY, "
                     "PRE (the gap before the room answers), DAMP. MIX sets how much of each you hear. The FX slot of the preset "
                     "synthesizer takes all of these from one preset.", sec ("DRIVE"), mark ("DRIVE") },
        { "FM", "Oscillator 2 bends the phase of oscillator 1, 3 or both: a bell, a tine, a growl. Turn oscillator 2's level down to "
                "hear only what it does to the others.", sec ("FM"), mark ("FM") },
        { "Patch, sequencer, drums", "PATCH opens the cable compartment: three LFOs, the sources (top row) and the destinations. Drag "
                   "from a source to a destination to lay a cable; pull it out to move it, drop it anywhere else to remove it. SEQ opens "
                   "the arpeggiator and the 16-step sequencer: each step has its key, its NOTE, VEL and GATE knobs and its ROW, SLIDE and "
                   "ACCENT switches; the sequencer follows the key you hold (HOLD) or runs on its own (FREE; PLAY starts it, again stops it). DRUM opens the drum "
                   "machine: eight tracks, each playing a drum sound you pick, with 16 steps (click: on, again: accent; right-click: off); hold a "
                   "key to play the beat, or PLAY to run it on its own. CLEAR "
                   "on either door wipes its steps. Arps, sequences "
                   "and beats are part of a preset: SAVE keeps yours, and the SEQ and DRUMS slots take them from any preset.", patchBtn.getBounds(), { drumBtn.getRight() + 16, drumBtn.getBounds().getCentreY() } } });
}

void JingFangEditor::openDoor (int which)
{
    if (labDoor && labDoor->learnOpen() && which != 3) return;   // LEARN works on its own page: no other page until it is done

    // SEQ on an instrument preset (no sequence, no arp) starts from an empty grid; on a Sequence / Arp preset it edits that
    // preset's pattern (owner 18 Oct: "seçilen enstrüman bir sequencer ya da arpejleyici ise düzenleme modu açılmalı, bir
    // enstrüman seçildiyse sıfırdan bir şey sequence ediliyor demektir")
    // only for an instrument preset as it was loaded (no unit on, steps untouched), once per loaded preset: a pattern the user
    // wrote and switched off is never wiped (bug sweep round 2)
    if (which == 2 && seqDoor && ! seqDoor->isOpenOrOpening())
    {
        const juce::String loadedId = proc.currentFamily() + "/" + proc.currentName();
        bool untouched = proc.loadedValue ("seq.on") < 0.5f && proc.loadedValue ("arp.on") < 0.5f
                      && proc.exactValue (svl::SEQ_ON) < 0.5f && proc.exactValue (svl::ARP_ON) < 0.5f && loadedId != seqScratchFor;
        for (int st = 1; st <= 16 && untouched; ++st)
        {
            const auto id = "seq.s" + juce::String (st) + ".on";
            untouched = std::abs (proc.apvts.getRawParameterValue (id)->load() - proc.loadedValue (id)) < 0.5f;
        }
        if (untouched)
        {
            seqScratchFor = loadedId;
            proc.armGlide();
            for (int st = 1; st <= 16; ++st)
                if (auto* p = proc.apvts.getParameter ("seq.s" + juce::String (st) + ".on")) setParamGesture (p, 0.0f);
        }
    }
    bay.setOpen (which == 1);
    if (seqDoor) seqDoor->setOpen (which == 2);
    if (labDoor) labDoor->setOpen (which == 3);
    if (drumDoor) drumDoor->setOpen (which == 4);
    patchBtn.repaint(); seqBtn.repaint(); labBtn.repaint(); drumBtn.repaint();
}

std::vector<std::pair<juce::String, juce::Rectangle<int>>> JingFangEditor::sectionsForTest() const
{
    std::vector<std::pair<juce::String, juce::Rectangle<int>>> v;
    for (auto& s : sections) v.push_back ({ s.title, s.r });
    return v;
}

juce::String JingFangEditor::layoutJson() const
{
    auto rect = [] (juce::Rectangle<int> r) { return "[" + juce::String (r.getX()) + "," + juce::String (r.getY()) + "," + juce::String (r.getWidth()) + "," + juce::String (r.getHeight()) + "]"; };
    juce::String j = "{\n \"size\": [" + juce::String (kW) + "," + juce::String (kH) + "],\n \"sections\": [";
    for (size_t i = 0; i < sections.size(); ++i) j << (i ? "," : "") << "\n  {\"title\": \"" << sections[i].title << "\", \"rect\": " << rect (sections[i].r) << "}";
    j << "\n ],\n \"knobs\": [";
    bool first = true;
    for (auto& k : knobs)
        if (k->getParentComponent() == &panel) { j << (first ? "" : ",") << "\n  {\"id\": \"" << k->paramId() << "\", \"rect\": " << rect (k->getBounds()) << "}"; first = false; }
    j << "\n ],\n \"displays\": [";
    for (size_t i = 0; i < scopes.size(); ++i) j << (i ? "," : "") << "\n  " << rect (scopes[i]->getBounds());
    j << "\n ],\n \"keyboard\": " << rect (keyboard.getBounds()) << ",\n \"preset\": " << rect (presetFace.getBounds())
      << ",\n \"keys\": [" << rect (prevBtn.getBounds()) << "," << rect (nextBtn.getBounds()) << "," << rect (saveBtn.getBounds()) << ","
      << rect (patchBtn.getBounds()) << "," << rect (helpBtn.getBounds()) << "," << rect (normalBtn.getBounds()) << "," << rect (microBtn.getBounds()) << "],"
      << "\n \"lockup\": " << rect (lockup.getBounds()) << "\n}\n";
    return j;
}

Knob* JingFangEditor::knob (const juce::String& id) const
{
    for (auto& k : knobs) if (k->paramId() == id) return k.get();
    return nullptr;
}

void JingFangEditor::resized()
{
    const float s = (float) getWidth() / (float) kW;
    panel.setBounds (0, 0, kW, kH);
    panel.setTransform (juce::AffineTransform::scale (s));
}

void JingFangEditor::paint (juce::Graphics& g) { g.fillAll (Pal::ground); }


void JingFangEditor::Panel::paint (juce::Graphics& g)
{
    const auto all = getLocalBounds().toFloat();
    if (const auto& bg = panelImage(); bg.isValid())
    {   // v3: the Blender render of the whole device (chassis, layered modules, print, screens' glass, the dragon around it)
        drawScaledCached (g, bg, all);
    }
    else
    {   // no render compiled in: a plain dark face with the section frames and titles, so the panel still reads
        g.fillAll (juce::Colour (0xff141617));
        for (auto& s : ed.sections)
        {
            g.setColour (juce::Colour (0xff1E2123)); g.fillRoundedRectangle (s.r.toFloat(), 6.0f);
            g.setColour (kKeyText);
            drawTracked (g, s.title, { (float) s.r.getX() + 16.0f, (float) s.r.getY() + 3.0f, 300.0f, 16.0f }, heros (10.5f, true), 0.22f, juce::Justification::centredLeft);
        }
    }
    // the tuning grid in numbers on its small screen (live)
    const auto& tl = panelLayout()["tuning_lcd"];
    const auto box = juce::Rectangle<float> ((float) (double) tl[0], (float) (double) tl[1], (float) (double) tl[2], (float) (double) tl[3]);
    const float edo = ed.proc.exactValue (svl::TUNE_EDO), step = 1200.0f / edo;
    const float up = ed.proc.exactValue (svl::ROW_UP) * step, dn = ed.proc.exactValue (svl::ROW_DOWN) * step;
    g.setColour (kLed.withAlpha (0.92f));
    g.setFont (mono (10.5f));
    const auto t = box.reduced (10.0f, 7.0f);
    const float lh = t.getHeight() / 3.0f;
    g.drawText ("step " + juce::String (step, 3) + " ct /" + juce::String (juce::roundToInt (edo)), t.withHeight (lh), juce::Justification::centredLeft, false);
    g.drawText ("high +" + juce::String (up, 2) + " ct", t.withTrimmedTop (lh).withHeight (lh), juce::Justification::centredLeft, false);
    g.drawText ("low  -" + juce::String (dn, 2) + " ct", t.withTrimmedTop (2.0f * lh).withHeight (lh), juce::Justification::centredLeft, false);
}

//==============================================================================
void JingFangEditor::DragonLayer::paint (juce::Graphics& g)
{
    static const juce::Image img = juce::ImageFileFormat::loadFrom (BinaryData::dragon_overlay_png, (size_t) BinaryData::dragon_overlay_pngSize);
    if (! img.isValid()) return;
    drawScaledCached (g, img, getLocalBounds().toFloat());
}

//==============================================================================
void JingFangEditor::SlotCell::paint (juce::Graphics& g)
{
    // owner 17 Oct: names, not numbers. The panel's name small on top, the preset's name in green below; < > at the sides.
    const auto b = getLocalBounds().toFloat().reduced (2.0f, 2.0f);
    if (hover) { g.setColour (kLed.withAlpha (0.10f)); g.fillRoundedRectangle (b, 3.0f); g.setColour (kLed.withAlpha (0.5f)); g.drawRoundedRectangle (b, 3.0f, 1.0f); }
    const int idx = proc.sectionSlot (section);
    const auto& list = proc.presets();
    // 14 cells share the strip: the cell carries a short name (the full name is in the tooltip and the help)
    static const char* const kShort[] = { "OSC 1", "OSC 2", "OSC 3", "FILTER", "AMP ENV", "FLT ENV", "PITCH", "DRIFT", "VOICE", "FM",
                                          "TUNING", "FX", "SEQ", "DRUMS" };
    const juce::String nm (section >= 0 && section < (int) std::size (kShort) ? kShort[section] : JingFangProcessor::sectionName (section));
    float px = 7.4f;
    while (px > 5.4f && trackedWidth (nm, heros (px, true), 0.10f) > b.getWidth() - 4.0f) px -= 0.2f;
    g.setColour (Pal::gold);
    drawTracked (g, nm, b.withHeight (13.0f), heros (px, true), 0.10f, juce::Justification::centred);
    const auto nameBox = b.withTrimmedTop (14.0f).reduced (9.0f, 1.0f);
    const auto arrow = kKeyTextDim.withAlpha (hover ? 1.0f : 0.45f);
    {
        juce::Path l, r; const float cy = nameBox.getCentreY(), lx = b.getX() + 4.5f, rx = b.getRight() - 4.5f;
        l.addTriangle (lx - 2.5f, cy, lx + 2.0f, cy - 3.5f, lx + 2.0f, cy + 3.5f);
        r.addTriangle (rx + 2.5f, cy, rx - 2.0f, cy - 3.5f, rx - 2.0f, cy + 3.5f);
        g.setColour (arrow); g.fillPath (l); g.fillPath (r);
    }
    const juce::String txt = idx >= 0 && idx < (int) list.size() ? list[(size_t) idx].name : juce::String ("-");
    g.setColour (idx >= 0 ? kLed : kKeyTextDim);
    float fs = 11.0f;   // as large as fits in two lines
    while (fs > 7.0f && juce::GlyphArrangement::getStringWidth (heros (fs, true), txt) > nameBox.getWidth() * 1.85f) fs -= 0.25f;
    g.setFont (heros (fs, true));
    g.drawFittedText (txt, nameBox.toNearestInt(), juce::Justification::centred, 2, 0.85f);
}

void JingFangEditor::SlotCell::mouseUp (const juce::MouseEvent& e)
{
    if (! contains (e.getPosition())) return;
    const float w = (float) getWidth();
    if (e.position.x < w / 3.0f) { proc.stepSection (section, -1); return; }
    if (e.position.x > w * 2.0f / 3.0f) { proc.stepSection (section, 1); return; }
    juce::PopupMenu m;
    juce::String fam; juce::PopupMenu sub;
    const auto& list = proc.presets();
    for (int i = 0; i < (int) list.size(); ++i)
    {
        if (list[(size_t) i].family != fam)
        {
            if (fam.isNotEmpty()) m.addSubMenu (fam, sub);
            fam = list[(size_t) i].family; sub = juce::PopupMenu();
        }
        sub.addItem (i + 1, juce::String (i + 1).paddedLeft ('0', 3) + "  " + list[(size_t) i].name, true, i == proc.sectionSlot (section));
    }
    if (fam.isNotEmpty()) m.addSubMenu (fam, sub);
    juce::Component::SafePointer<SlotCell> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this), [safe] (int r)
    {
        if (safe != nullptr && r > 0) safe->proc.loadSection (safe->section, r - 1);
    });
}

//==============================================================================
void JingFangEditor::PresetFace::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (juce::Colour (0x66050707)); g.fillRoundedRectangle (b, 3.0f);
    g.setColour (kLed.withAlpha (0.85f));
    // centred: family over name, like the screen sits centred on the unit
    drawTracked (g, proc.currentFamily().toUpperCase(), { b.getX() + 14.0f, b.getY() + 5.0f, b.getWidth() - 28.0f, 13.0f }, heros (9.6f, true), 0.22f,
                 juce::Justification::centred);
    g.setColour (kKeyText);
    g.setFont (heros (22.0f, true));
    g.drawFittedText (proc.currentName(), juce::Rectangle<int> (juce::roundToInt (b.getX()) + 16, juce::roundToInt (b.getY()) + 18, juce::roundToInt (b.getWidth()) - 32, 28),
                      juce::Justification::centred, 1, 0.75f);
    if (proc.isModified()) { g.setColour (Pal::gold); g.fillEllipse (b.getRight() - 12.0f, b.getY() + 6.0f, 6.0f, 6.0f); }
}

juce::String JingFangEditor::Readout::shownText() const
{
    return (proc.tempoFromHost() ? "HOST " : "") + juce::String (juce::roundToInt (proc.tempoBpm()));
}

void JingFangEditor::Readout::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (juce::Colour (0x66050707)); g.fillRoundedRectangle (b, 3.0f);   // as the preset screen
    const bool host = proc.tempoFromHost();
    g.setColour (kLed.withAlpha (0.85f));
    drawTracked (g, host ? "HOST BPM" : "BPM", { b.getX(), b.getY() + 3.0f, b.getWidth(), 13.0f }, heros (9.6f, true), 0.22f, juce::Justification::centred);
    if (host)
    {   // a small lock: the DAW sets the tempo (RU-8 rule 1)
        const float cx = b.getCentreX() + 31.0f, cy = b.getY() + 9.0f;
        g.drawRoundedRectangle (cx - 3.0f, cy - 1.0f, 6.0f, 5.0f, 1.0f, 1.0f);
        juce::Path arc; arc.addCentredArc (cx, cy - 1.0f, 2.0f, 2.2f, 0.0f, -juce::MathConstants<float>::halfPi, juce::MathConstants<float>::halfPi, true);
        g.strokePath (arc, juce::PathStrokeType (1.0f));
    }
    g.setColour (kKeyText);
    g.setFont (heros (18.0f, true));
    g.drawFittedText (juce::String (juce::roundToInt (proc.tempoBpm())), juce::Rectangle<int> (4, 18, getWidth() - 8, getHeight() - 21), juce::Justification::centred, 1, 0.75f);
}

void JingFangEditor::Readout::mouseDown (const juce::MouseEvent&) { dragFrom = proc.userTempo(); }

void JingFangEditor::Readout::mouseDrag (const juce::MouseEvent& e)
{
    if (proc.tempoFromHost()) return;
    proc.setUserBpm (juce::jlimit (40.0f, 240.0f, std::round (dragFrom - (float) e.getDistanceFromDragStartY() / 3.0f)));
    repaint();
}

void JingFangEditor::Readout::mouseDoubleClick (const juce::MouseEvent&)
{
    if (proc.tempoFromHost()) return;
    proc.setUserBpm (120.0f); repaint();
}

void JingFangEditor::Readout::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    if (proc.tempoFromHost()) return;
    proc.setUserBpm (juce::jlimit (40.0f, 240.0f, proc.userTempo() + (w.deltaY > 0 ? 1.0f : -1.0f)));
    repaint();
}

void JingFangEditor::Readout::mouseUp (const juce::MouseEvent&) {}

void JingFangEditor::PresetFace::mouseUp (const juce::MouseEvent& e)
{
    if (! getLocalBounds().contains (e.getPosition())) return;
    juce::PopupMenu menu;
    const auto& list = proc.presets();
    for (const auto& fam : proc.families())
    {
        juce::PopupMenu sub;
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i].family == fam) sub.addItem ((int) i + 1, list[i].name + (list[i].factory ? "" : "  (yours)"), true, (int) i == proc.currentPresetIndex());
        menu.addSubMenu (fam, sub, true, nullptr, fam == proc.currentFamily());
    }
    // owner 18 Oct: every preset can be deleted, factory ones included, and they do not come back
    constexpr int kDelete = 100000, kUnhide = 100001;
    const int cur = proc.currentPresetIndex();
    menu.addSeparator();
    if (cur >= 0) menu.addItem (kDelete, "Delete \"" + list[(size_t) cur].name + "\"...");
    if (! JingFangProcessor::hiddenPresetNames().isEmpty()) menu.addItem (kUnhide, "Show hidden factory presets");
    juce::Component::SafePointer<PresetFace> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this), [safe, cur] (int r)
    {
        if (safe == nullptr || r <= 0) return;
        if (r == kUnhide) { safe->proc.showHiddenPresets(); safe->repaint(); return; }
        if (r != kDelete) { safe->proc.loadPresetIndex (r - 1); return; }
        const auto& pe = safe->proc.presets()[(size_t) cur];
        juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Delete \"" + pe.name + "\"?",
            pe.factory ? juce::String ("This factory preset is hidden and will not appear again (\"Show hidden factory presets\" brings it back).")
                       : juce::String ("Your preset file goes to the bin."),
            "Delete", "Cancel", safe.getComponent(),
            juce::ModalCallbackFunction::create ([safe, cur] (int ok) { if (safe != nullptr && ok == 1) { safe->proc.deletePreset (cur); safe->repaint(); } }));
    });
}

JingFangEditor::SaveTarget JingFangEditor::saveTarget() const
{
    // the door that is open decides where a preset goes (owner 18 Oct: "drum'dan başka bir kaydetme seçeneği olmamalı, yanlış
    // kaydetme yolu kapatılmalı"): DRUM -> Drum Machine, SEQ -> Sequence / Arp (yours), otherwise any family
    juce::String fixedFamily;
    if (drumDoor && drumDoor->isOpenOrOpening()) fixedFamily = "Drum Machine";
    else if (seqDoor && seqDoor->isOpenOrOpening())
        fixedFamily = proc.exactValue (svl::SEQ_ON) < 0.5f && proc.exactValue (svl::ARP_ON) > 0.5f ? "Arp - Mine" : "Sequence - Mine";
    const auto curFamily = proc.currentFamily();
    const bool sameKind = fixedFamily.isEmpty() || curFamily == fixedFamily
                          || (fixedFamily.startsWith ("Sequence") && curFamily.startsWith ("Sequence"))
                          || (fixedFamily.startsWith ("Arp") && curFamily.startsWith ("Arp"));
    // the suggested name never repeats a factory preset's in that family (the demo player found "Boom Bap" saved next to the
    // factory "Boom Bap": two presets the list cannot tell apart); saving over your own preset of that name stays allowed
    auto unique = [this] (const juce::String& family, const juce::String& name)
    {
        auto factoryHas = [&] (const juce::String& n) { for (auto& e : proc.presets()) if (e.factory && e.family == family && e.name == n) return true; return false; };
        auto anyHas = [&] (const juce::String& n) { for (auto& e : proc.presets()) if (e.family == family && e.name == n) return true; return false; };
        if (! factoryHas (name)) return name;
        for (int i = 2;; ++i) if (! anyHas (name + " " + juce::String (i))) return name + " " + juce::String (i);
    };
    if (fixedFamily.isEmpty()) return { curFamily, unique (curFamily, proc.currentName()), false };
    const juce::String defaultName = sameKind && proc.currentName() != "Mix" ? proc.currentName()
                                   : (fixedFamily == "Drum Machine" ? juce::String ("My Beat") : fixedFamily.startsWith ("Arp") ? juce::String ("My Arp") : juce::String ("My Sequence"));
    return { fixedFamily, unique (fixedFamily, defaultName), true };
}

void JingFangEditor::savePreset()
{
    const auto target = saveTarget();
    const juce::String fixedFamily = target.fixed ? target.family : juce::String();
    saveWindow = std::make_unique<juce::AlertWindow> (fixedFamily.isEmpty() ? "Save preset" : "Save to " + fixedFamily,
                                                      "Your own presets are kept in " + JingFangProcessor::userPresetFolder().getFullPathName(),
                                                      juce::MessageBoxIconType::NoIcon, this);
    saveWindow->addTextEditor ("name", target.name, "Name");
    if (fixedFamily.isEmpty()) saveWindow->addTextEditor ("family", target.family, "Family");
    saveWindow->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    saveWindow->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<JingFangEditor> safe (this);
    saveWindow->enterModalState (true, juce::ModalCallbackFunction::create ([safe, fixedFamily] (int r)
    {
        if (safe == nullptr || safe->saveWindow == nullptr) return;
        if (r == 1)
            safe->proc.saveUserPreset (fixedFamily.isNotEmpty() ? fixedFamily : safe->saveWindow->getTextEditorContents ("family"),
                                       safe->saveWindow->getTextEditorContents ("name"));
        safe->saveWindow.reset();
        safe->presetFace.repaint();
    }), false);
}

//==============================================================================
bool JingFangEditor::keyPressed (const juce::KeyPress& k)
{
    if (help.isOpen() && k == juce::KeyPress::escapeKey) { help.close(); helpBtn.repaint(); return true; }
    if (k == juce::KeyPress::escapeKey && labDoor && labDoor->learnOpen()) return true;    // LEARN open: Esc does not leave the page
    if (k == juce::KeyPress::escapeKey && anyDoorOpen()) { openDoor (0); return true; }   // Esc: back to the panel
    if (k == juce::KeyPress::escapeKey) { stopEverything(); return true; }                 // Esc on the panel: stop all that plays
    if (k.getKeyCode() == juce::KeyPress::tabKey) return true;   // Tab would move the focus off the editor (to Options)
    return keyboard.keyPressed (k);
}

void JingFangEditor::stopEverything()
{
    // a sequence or a beat running on its own clock goes back to waiting for a key, and every note stops (owner 18 Oct: "bir
    // saattir aynı nota çalıp duruyor": a free-running sequence with no visible way to stop it)
    for (auto id : { "seq.mode", "drums.mode" })
        if (auto* p = proc.apvts.getParameter (id)) setParamGesture (p, 0.0f);   // hold
    if (auto* p = proc.apvts.getParameter ("arp.latch")) setParamGesture (p, 0.0f);
    keyboard.releaseAllComputerKeys();
    proc.uiAllNotesOff();
}

bool JingFangEditor::anyDoorOpen() const
{
    return bay.isOpenOrOpening() || (seqDoor && seqDoor->isOpenOrOpening()) || (drumDoor && drumDoor->isOpenOrOpening())
        || (labDoor && labDoor->isOpenOrOpening());
}

bool JingFangEditor::keyStateChanged (bool down) { return keyboard.keyStateChanged (down); }

void JingFangEditor::timerCallback()
{
    // the probe (JF_PROBE=1 in the environment, development only): once a second the editor writes its own picture and the
    // sequencer / drum state to /tmp/jf_probe, so the real app can be checked without a screen capture (CLAUDE.md screen rule)
    static const bool probe = juce::SystemStats::getEnvironmentVariable ("JF_PROBE", {}) == "1";
    if (probe)
    {   // the UI thread's own rhythm: the gap between two ticks of this 30 Hz timer (a long gap = a visible stall)
        const double nowT = juce::Time::getMillisecondCounterHiRes();
        if (probeLastTick > 0)
        {
            const double gap = nowT - probeLastTick;
            probeGapMax = juce::jmax (probeGapMax, gap); probeGapSum += gap; ++probeGaps;
            if (gap > 50.0) ++probeStalls;
        }
        probeLastTick = nowT;
    }
    if (probe && ++probeTick % 30 == 0)
    {
        const auto dir = juce::File ("/tmp/jf_probe");
        dir.createDirectory();
        // the picture only on request (touch /tmp/jf_probe/snap): encoding a PNG here stalled the UI for a moment every second
        if (dir.getChildFile ("snap").existsAsFile())
        {
            dir.getChildFile ("snap").deleteFile();
            juce::PNGImageFormat png;
            const auto img = createComponentSnapshot (getLocalBounds(), true, 1.0f);
            const auto f = dir.getChildFile ("editor.png");
            f.deleteFile();
            if (auto os = std::unique_ptr<juce::FileOutputStream> (f.createOutputStream())) png.writeImageToStream (img, *os);
        }
        const auto& eng = proc.engineForTest();
        float scope[JingFangProcessor::kScopeLen]; proc.copyScope (scope);
        float outPeak = 0; for (float v : scope) outPeak = juce::jmax (outPeak, std::abs (v));   // the last 2048 output samples
        juce::String j;
        j << "{\"t\": " << (juce::int64) juce::Time::currentTimeMillis()
          << ", \"seq_on\": " << proc.exactValue (svl::SEQ_ON) << ", \"seq_mode\": " << proc.exactValue (svl::SEQ_MODE)
          << ", \"arp_on\": " << proc.exactValue (svl::ARP_ON) << ", \"daw_seq\": " << proc.exactValue (svl::SEQ_SOURCE)
          << ", \"seq_running\": " << (int) eng.noteGen().isRunning() << ", \"seq_step\": " << eng.noteGen().currentStep()
          << ", \"drums_on\": " << proc.exactValue (svl::DRUMS_ON) << ", \"drums_mode\": " << proc.exactValue (svl::DRUMS_MODE)
          << ", \"daw_drums\": " << proc.exactValue (svl::DRUMS_SOURCE)
          << ", \"drums_running\": " << (int) (eng.drumMachine() != nullptr && eng.drumMachine()->isRunning()) << ", \"drum_step\": " << eng.drumStep()
          << ", \"level\": " << proc.live().level << ", \"voices\": " << proc.live().voices << ", \"out_peak\": " << outPeak
          << ", \"light_on_ms\": " << (keyboard.delayStats.on ? keyboard.delayStats.onSum / keyboard.delayStats.on : -1.0) << ", \"light_on_max\": " << keyboard.delayStats.onMax
          << ", \"light_off_ms\": " << (keyboard.delayStats.off ? keyboard.delayStats.offSum / keyboard.delayStats.off : -1.0) << ", \"light_off_max\": " << keyboard.delayStats.offMax
          << ", \"lights\": " << keyboard.delayStats.on << ", \"kb_paint_ms\": " << (keyboard.delayStats.paints ? keyboard.delayStats.paintSum / keyboard.delayStats.paints : -1.0)
          << ", \"kb_paint_max\": " << keyboard.delayStats.paintMax
          << ", \"tick_gap_avg\": " << (probeGaps ? probeGapSum / probeGaps : -1.0) << ", \"tick_gap_max\": " << probeGapMax
          << ", \"ticks\": " << probeGaps << ", \"stalls_over_50ms\": " << probeStalls
          << ", \"in_host\": " << (int) proc.inHost() << ", \"bpm\": " << proc.tempoBpm() << ", \"preset\": " << juce::JSON::toString (proc.currentFamily() + " / " + proc.currentName())
          << ", \"seq_door\": " << (int) (seqDoor && seqDoor->isOpenOrOpening()) << ", \"drum_door\": " << (int) (drumDoor && drumDoor->isOpenOrOpening()) << "}";
        dir.getChildFile ("state.json").replaceWithText (j);
    }
    // the computer keys play the keyboard: the editor keeps the keyboard focus (owner 18 Oct: the keys went dead after picking
    // a preset, and "in some situations completely"). Panel parts never want the focus; whenever it sits on one of them, on
    // the Standalone's own window or its Options button, or nowhere, it comes back here. A popup menu or a dialog (their own
    // windows, incl. the audio settings) keeps it while it is open.
    {
        static std::function<void (juce::Component&)> noFocus = [] (juce::Component& c)
        { c.setWantsKeyboardFocus (false); for (auto* ch : c.getChildren()) noFocus (*ch); };
        if (++focusTick % 15 == 0) for (auto* ch : getChildren()) noFocus (*ch);   // twice a second: doors and menus add children
        auto* f = juce::Component::getCurrentlyFocusedComponent();
        auto* top = getTopLevelComponent();
        const bool ours = f == nullptr || f == this || isParentOf (f) || f == top || (top != nullptr && top->isParentOf (f));
        const bool modal = juce::Component::getCurrentlyModalComponent() != nullptr;   // open popup menus are modal components
        if (proc.isStandalone() && ours && ! modal && f != this && isShowing() && getPeer() != nullptr && getPeer()->isFocused() && saveWindow == nullptr)
            grabKeyboardFocus();
        // a key released while a menu had the keys (the menu swallows the key-up): release it now
        keyboard.keyStateChanged (false);
    }
    // while LEARN is open the page keys only name their pages: dimmed, a click does nothing (openDoor refuses too)
    {
        const bool learnOpen = labDoor && labDoor->learnOpen();
        for (auto* k : { &patchBtn, &seqBtn, &drumBtn, &labBtn, &closeBtn })
            if (k->isEnabled() == learnOpen)
            {
                k->setEnabled (! learnOpen);
                k->setAlpha (learnOpen ? 0.35f : 1.0f);
            }
    }
    // the CLOSE key sits on the open door's bottom edge (owner 18 Oct: no way back from PATCH / SEQ / DRUM to the panel)
    {
        juce::Component* door = bay.isOpenOrOpening() && bay.isVisible() ? static_cast<juce::Component*> (&bay)
                              : seqDoor && seqDoor->isOpenOrOpening() && seqDoor->isVisible() ? static_cast<juce::Component*> (seqDoor.get())
                              : drumDoor && drumDoor->isOpenOrOpening() && drumDoor->isVisible() ? static_cast<juce::Component*> (drumDoor.get())
                              : labDoor && labDoor->isOpenOrOpening() && labDoor->isVisible() ? static_cast<juce::Component*> (labDoor.get()) : nullptr;
        if (door != nullptr)
        {
            const auto b = door->getBounds();
            const auto r = juce::Rectangle<int> (110, 26).withCentre ({ b.getCentreX(), b.getBottom() - 1 });
            if (closeBtn.getBounds() != r) closeBtn.setBounds (r);
            if (! closeBtn.isVisible()) { closeBtn.setVisible (true); closeBtn.toFront (false); }
        }
        else if (closeBtn.isVisible()) closeBtn.setVisible (false);
    }
    for (auto& k : knobs) k->tick();
    for (auto* sc : scopes) sc->tick();
    keyboard.tick();
    const bool wasOpen = bay.isVisible();
    bay.tick();
    if (seqDoor) { const bool was = seqDoor->isVisible(); seqDoor->tick(); if (was != seqDoor->isVisible()) seqBtn.repaint(); }
    if (labDoor) { const bool was = labDoor->isVisible(); labDoor->tick(); if (was != labDoor->isVisible()) labBtn.repaint(); }
    if (drumDoor) { const bool was = drumDoor->isVisible(); drumDoor->tick(); if (was != drumDoor->isVisible()) drumBtn.repaint(); }
    {
        const bool seqNow = proc.live().seqStep >= 0, drumNow = proc.drumStep() >= 0;
        if (seqNow != seqWasRunning) { seqWasRunning = seqNow; seqBtn.repaint(); }
        if (drumNow != drumWasRunning) { drumWasRunning = drumNow; drumBtn.repaint(); }
    }
    if (wasOpen != bay.isVisible()) patchBtn.repaint();
    juce::String sl;
    for (int i = 0; i < JingFangProcessor::kNumSections; ++i) sl << proc.sectionSlot (i) << ",";
    if (sl != lastSlots) { lastSlots = sl; for (auto& c : slotCells) c->repaint(); }
    const juce::String face = proc.currentFamily() + "|" + proc.currentName() + "|" + (proc.isModified() ? "*" : "");
    if (face != lastFace) { lastFace = face; presetFace.repaint(); }
    const juce::String readouts = bpmFace.shownText();
    if (readouts != lastReadouts) { lastReadouts = readouts; bpmFace.repaint(); }
    juce::String t;
    t << proc.exactValue (svl::TUNE_EDO) << proc.exactValue (svl::ROW_UP) << proc.exactValue (svl::ROW_DOWN) << proc.exactValue (svl::TUNE_MODE)
      << keyboard.computerOctave();
    if (t != lastTuning)
    {
        lastTuning = t;
        const auto& tl = panelLayout()["tuning_lcd"];
        panel.repaint (juce::roundToInt ((double) tl[0]) - 2, juce::roundToInt ((double) tl[1]) - 2, juce::roundToInt ((double) tl[2]) + 4, juce::roundToInt ((double) tl[3]) + 4);
        normalBtn.repaint(); microBtn.repaint();
    }
}
