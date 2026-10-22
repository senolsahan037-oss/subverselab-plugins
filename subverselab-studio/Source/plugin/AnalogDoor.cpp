#include "AnalogDoor.h"
#include "MidiClip.h"

namespace jfui
{
namespace
{
/** the CLIP key: drag it onto a DAW track = the pattern as a clip (and arms the PATTERN / MIDI switch, RU-8 rule 3);
    a click without a drag saves the clip to Music/JING FANG/MIDI */
struct ClipKey : PanelKey
{
    ClipKey (JingFangProcessor& p, int u) : PanelKey ("CLIP"), proc (p), unit (u) { setMouseCursor (juce::MouseCursor::DraggingHandCursor); }
    void mouseDown (const juce::MouseEvent& e) override { dragged = false; PanelKey::mouseDown (e); }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragged || e.getDistanceFromDragStart() < 6) return;
        dragged = true;
        const auto f = jfclip::writeTemp (proc, unit);
        if (f == juce::File()) return;
        proc.armClip (unit);
        juce::DragAndDropContainer::performExternalDragDropOfFiles ({ f.getFullPathName() }, false, this);
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (dragged) { dragged = false; PanelKey::mouseUp (e.withNewPosition (juce::Point<float> (-10.0f, -10.0f))); return; }   // no click after a drag
        PanelKey::mouseUp (e);
    }
    JingFangProcessor& proc;
    int unit;
    bool dragged = false;
};
} // namespace

AnalogDoor::AnalogDoor (JingFangProcessor& p, KnobMaker makeKnob, juce::Rectangle<int> fullBounds, juce::String n, std::function<int()> current)
    : proc (p), full (fullBounds), name (std::move (n)), stepNow (std::move (current))
{
    setInterceptsMouseClicks (true, true);
    const auto& D = doorsLayout()[juce::Identifier (name)];
    auto has = [this] (const juce::String& id) { return proc.apvts.getParameter (id) != nullptr; };
    auto add = [this] (juce::Component* c, juce::Rectangle<int> b) { owned.emplace_back (c); addAndMakeVisible (c); c->setBounds (b); return c; };
    auto list = [&D] (const char* k) -> const juce::Array<juce::var>& { static const juce::Array<juce::var> none; auto* a = D[k].getArray(); return a ? *a : none; };

    for (auto& k : list ("knobs"))
    {
        const auto id = k["id"].toString();
        if (! has (id)) continue;
        auto& kn = makeKnob (id, "", partBox (k["c"], (float) (double) k["d"]), this);
        kn.setPrinted (true);
        ++knobCount;
        if (name == "seq" && id.endsWith (".vel"))
        {   // the VEL row is a lane (work order 20 Oct, item 5): the same places carry PROB and RATCH knobs, one lane shown at a time
            const auto pre = id.upToLastOccurrenceOf (".", true, false);
            lanes[0].push_back (&kn);
            for (int l = 1; l <= 2; ++l)
            {
                const auto lid = pre + (l == 1 ? "prob" : "ratchet");
                if (! has (lid)) continue;
                auto& lk = makeKnob (lid, "", kn.getBounds(), this);
                lk.setPrinted (true);
                lk.setVisible (false);
                lanes[(size_t) l].push_back (&lk);
                ++knobCount;
            }
        }
    }
    if (name == "seq")
    {   // the lane key sits over the row's printed name; the direction selector in the empty corner left of the step numbers
        auto* lane = static_cast<PanelKey*> (add (new PanelKey ("VEL"), { 14, 345, 82, 22 }));
        lane->fontPx = 10.0f;
        lane->setTooltip ("This row: VEL (how hard each step plays), PROB (the chance a step plays) or RATCH (a step played 1 to 4 times in its length); click for the next");
        lane->onClick = [this, lane]
        {
            shownLane = (shownLane + 1) % 3;
            static const char* names[] = { "VEL", "PROB", "RATCH" };
            lane->text = names[shownLane];
            for (int l = 0; l < 3; ++l) for (auto* kk : lanes[(size_t) l]) kk->setVisible (l == shownLane);
            lane->repaint();
        };
        laneKey = lane;
        if (has ("seq.direction"))
        {
            auto* dir = add (new TypeStepper (proc, "seq.direction"), { 10, 171, 112, 22 });
            static_cast<TypeStepper*> (dir)->setTooltip ("The order the steps play: forward, reverse, bounce (there and back) or random (the same random every time round)");
        }
    }
    for (auto& s : list ("switches"))
    {
        const auto id = s["id"].toString();
        if (! has (id)) continue;
        add (new BatSwitch (proc, id, (int) s["positions"], (int) s["up"], (int) s["down"], s.hasProperty ("mid") ? (int) s["mid"] : -1),
             partBox (s["c"], (float) (double) s["d"]));
    }
    for (auto& s : list ("selectors"))
    {
        const auto id = s["id"].toString();
        if (! has (id)) continue;
        add (new ChickenSelector (proc, id, (int) s["count"]), partBox (s["c"], (float) (double) s["d"]));
    }
    // step keys: the column is the step number in the id (".s<N>" for the sequencer, ".s<N>" after the track for the drums)
    for (auto& s : list ("steps"))
    {
        const auto id = s["id"].toString();
        if (! has (id)) continue;
        auto* k = static_cast<StepKey*> (add (new StepKey (proc, id, (int) s["states"]), partBox (s["c"], (float) (double) s["d"])));
        {   // lighting a step switches its unit on (a pattern written from scratch plays with the next key / PLAY)
            const juce::String unitOn = id.startsWith ("drums.") ? "drums.on" : "seq.on";
            k->onLit = [this, unitOn]
            {
                if (auto* u = proc.apvts.getParameter (unitOn); u != nullptr && u->getValue() < 0.5f) setParamGesture (u, 1.0f);
            };
        }
        const auto tail = id.fromLastOccurrenceOf (".s", false, false);
        keys.push_back ({ tail.getIntValue() - 1, k });
    }
    if (! keys.empty() || name == "seq")
        for (auto& l : list ("leds"))
        {
            auto* led = static_cast<StepLed*> (add (new StepLed(), partBox (l["c"], (float) (double) l["d"])));
            leds.push_back ({ (int) l["step"], led });
        }
    // keys on the plate: CLEAR puts its parameters back to their defaults as one gesture each
    for (auto& k : list ("keys"))
    {
        const auto& r = k["rect"];
        auto* key = static_cast<PanelKey*> (add (new PanelKey (k["label"].toString()), { (int) r[0], (int) r[1], (int) r[2], (int) r[3] }));
        key->fontPx = 10.0f;
        key->setTooltip (k["tip"].toString());
        if (k.hasProperty ("source"))
        {   // PATTERN / MIDI (daw.seq / daw.drums): the label says which plays; a dragged clip shows ARMED until its first note
            auto* p = proc.apvts.getParameter (k["source"]["param"].toString());
            const int unit = (int) k["source"]["unit"];
            if (p == nullptr) { key->setVisible (false); continue; }
            key->isOn = [p] { return p->getValue() > 0.5f; };
            key->onClick = [this, p, unit, key]
            {
                setParamGesture (p, p->getValue() > 0.5f ? 0.0f : 1.0f);
                proc.disarmClip (unit);                                // a click decides: no armed switch left behind
                key->repaint();
            };
            sourceKeys.push_back ({ key, unit });
            continue;
        }
        if (k.hasProperty ("clip"))
        {   // replace the plain key by a ClipKey in the same place
            const int unit = (int) k["clip"];
            const auto b = key->getBounds();
            removeChildComponent (key);
            owned.pop_back();
            auto* ck = static_cast<ClipKey*> (add (new ClipKey (proc, unit), b));
            ck->fontPx = 10.0f;
            ck->setTooltip (k["tip"].toString());
            ck->onClick = [this, unit, ck]
            {
                const auto f = jfclip::saveToMusic (proc, unit);
                ck->setTooltip (f == juce::File() ? juce::String ("The clip could not be saved") : "Saved: " + f.getFullPathName());
            };
            continue;
        }
        if (k.hasProperty ("play"))
        {   // PLAY: on + free mode (runs on its own clock); lit while it runs; again: back to hold (waits for a key)
            const auto& pl = k["play"];
            auto* onP = proc.apvts.getParameter (pl["on"].toString());
            auto* modeP = proc.apvts.getParameter (pl["mode"].toString());
            const float freeV = (float) (double) pl["free"], holdV = (float) (double) pl["hold"];
            if (onP == nullptr || modeP == nullptr) { key->setVisible (false); continue; }
            auto plain = [] (juce::RangedAudioParameter* p) { return p->convertFrom0to1 (p->getValue()); };
            const int unit = pl["on"].toString().startsWith ("drums") ? 1 : 0;
            // in a playing host PLAY mirrors the host: lit while the unit runs on the host's grid (the DAW's notes drive it)
            key->isOn = [this, onP, modeP, freeV, plain, unit]
            {
                if (proc.inHost() && proc.hostPlaying()) return (unit == 0 ? proc.live().seqStep : proc.drumStep()) >= 0;
                return plain (onP) > 0.5f && std::abs (plain (modeP) - freeV) < 0.5f;
            };
            key->onClick = [this, onP, modeP, freeV, holdV, plain, key, unit]
            {
                if (proc.inHost() && proc.hostPlaying()) return;   // the DAW plays: no second playback inside the plug-in (owner 20 Oct)
                const bool playing = plain (onP) > 0.5f && std::abs (plain (modeP) - freeV) < 0.5f;
                if (playing) setParamGesture (modeP, holdV);
                else { setParamGesture (onP, 1.0f); setParamGesture (modeP, freeV); }
                // in a stopped host PLAY previews; the host's start carries it on, its stop ends it (RU-8 rule 2)
                if (proc.inHost()) proc.requestPreview (unit, ! playing && ! proc.hostPlaying());
                key->repaint();
            };
            if (! proc.isStandalone())
                key->setTooltip (key->getTooltip().replace ("on its own clock (no key needed)",
                                                            "as a preview while your host is stopped (no key needed); while the host plays, its notes drive it and PLAY follows the host"));
            playKeys.push_back (key);
            continue;
        }
        juce::StringArray ids;
        if (auto* a = k["clear"].getArray()) for (auto& id : *a) ids.add (id.toString());
        key->onClick = [this, ids]
        {
            proc.armGlide();                                   // product-set values glide (interface signature)
            for (auto& id : ids)
                if (auto* p = proc.apvts.getParameter (id))
                {   // every field back to its default, and every step off (an empty grid)
                    const bool stepOn = id.startsWith ("seq.s") && id.endsWith (".on");
                    p->beginChangeGesture();
                    p->setValueNotifyingHost (stepOn ? 0.0f : p->getDefaultValue());
                    p->endChangeGesture();
                }
        };
    }
    // TEMPO: the panel's tempo (both doors share it); with a host tempo it shows that and does not move
    for (auto& t : list ("tempo"))
    {
        auto* k = static_cast<ValueKnob*> (add (new ValueKnob(), partBox (t["c"], (float) (double) t["d"])));
        k->lo = 40; k->hi = 240; k->def = 120; k->value = proc.userTempo();
        k->onChange = [this] (float v) { proc.setUserBpm (std::round (v)); };
        k->setTooltip ("Tempo in BPM (the host's tempo when it has one). Double-click: 120");
        tempoKnobs.push_back ({ k, juce::Point<float> ((float) (double) t["c"][0], (float) (double) t["c"][1] + (float) (double) t["d"] * 0.5f + 11.0f) });
    }
    for (auto& pk : list ("pickers"))
    {
        if (! has ("drums.on")) break;
        const auto& r = pk["rect"];
        add (new TrackPicker (proc, (int) pk["track"]), { (int) r[0], (int) r[1], (int) r[2], (int) r[3] });
    }
    layoutDoor();
}

void AnalogDoor::layoutDoor()
{
    const int h = juce::roundToInt ((float) full.getHeight() * amount);
    setBounds (full.withHeight (juce::jmax (1, h)));
    setVisible (amount > 0.001f);
}

void AnalogDoor::tick()
{
    if (amount != target)
    {
        const float d = target - amount;
        amount = std::abs (d) < 0.02f ? target : amount + d * 0.42f;
        layoutDoor();
    }
    if (! isVisible()) return;
    for (auto& [k, unit] : sourceKeys)
    {   // PATTERN / MIDI / ARMED
        const auto want = proc.clipArmed (unit) ? juce::String ("ARMED") : (k->isOn && k->isOn() ? juce::String ("MIDI") : juce::String ("PATTERN"));
        if (k->text != want) { k->text = want; k->repaint(); }
    }
    {   // the PLAY light follows the switches (and the host); repaint only when it changes
        int sig = 0;
        for (auto* k : playKeys) sig = sig * 2 + (k->isOn && k->isOn() ? 1 : 0);
        if (sig != playSig) { playSig = sig; for (auto* k : playKeys) k->repaint(); }
        const float a = proc.inHost() && proc.hostPlaying() ? 0.5f : 1.0f;   // faint while the host plays: it follows the host
        for (auto* k : playKeys) if (k->getAlpha() != a) k->setAlpha (a);
    }
    {   // steps past the pattern's length are dim, so a 7/8 or 9/8 preset shows its own length (owner 18 Oct)
        auto* lenP = proc.apvts.getParameter (name == "drum" ? "drums.length" : "seq.length");
        const int len = lenP != nullptr ? juce::roundToInt (lenP->convertFrom0to1 (lenP->getValue())) : 16;
        if (len != shownLength)
        {
            shownLength = len;
            for (auto& [i, k] : keys) k->setAlpha (i < len ? 1.0f : 0.35f);
            for (auto& [i, l] : leds) l->setAlpha (i < len ? 1.0f : 0.35f);
        }
    }
    for (auto& [k, at] : tempoKnobs)
    {
        const float bpm = proc.tempoBpm();
        const bool host = proc.tempoFromHost();
        if (k->isEnabled() == host) k->setEnabled (! host);
        if (std::abs (k->value - bpm) > 0.01f || host != tempoHostShown)
        {
            k->value = bpm; k->repaint();
            repaint (juce::Rectangle<int> (90, 14).withCentre (at.toInt()));
        }
    }
    tempoHostShown = proc.tempoFromHost();
    // ---- 7: the track windows follow the kit (preset arrows, the DRUMS slot, a host restore)
    for (auto& c : owned)
        if (auto* tp = dynamic_cast<TrackPicker*> (c.get()))
            if (const auto n = proc.drumTrackName (tp->track); n != tp->shown) { tp->shown = n; tp->repaint(); }
    const int s = stepNow ? stepNow() : -1;
    if (s == lastStep) return;
    lastStep = s;
    for (auto& [i, l] : leds) { const bool lit = i == s; if (l->lit != lit) { l->lit = lit; l->repaint(); } }
    for (auto& [i, k] : keys) { const bool on = i == s; if (k->playing != on) { k->playing = on; k->repaint(); } }
}

void AnalogDoor::paint (juce::Graphics& g)
{
    const auto b = juce::Rectangle<float> (0.0f, 0.0f, (float) full.getWidth(), (float) full.getHeight());
    juce::Path clip; clip.addRoundedRectangle (b, 6.0f);
    g.saveState();
    g.reduceClipRegion (clip);
    const auto& plate = doorPlate (name);
    if (plate.isValid())
    {
        drawScaledCached (g, plate, b);
    }
    else { g.setColour (juce::Colour (0xff121011)); g.fillRect (b); }
    g.restoreState();
    g.setColour (juce::Colours::black.withAlpha (0.8f)); g.drawRoundedRectangle (b.reduced (0.5f), 6.0f, 1.2f);
    for (auto& [k, at] : tempoKnobs)
    {   // the tempo, always readable
        g.setColour (proc.tempoFromHost() ? Pal::gold : kKeyText);
        g.setFont (heros (9.0f, true));
        g.drawText ((proc.tempoFromHost() ? "HOST " : "") + juce::String (juce::roundToInt (proc.tempoBpm())) + " BPM",
                    juce::Rectangle<float> (90.0f, 14.0f).withCentre (at), juce::Justification::centred, false);
    }
    if (name == "drum" && proc.apvts.getParameter ("drums.on") == nullptr)
    {   // the drum engine's parameters are not in this build yet: say so on the glass
        g.setColour (kKeyTextDim);
        g.setFont (heros (12.0f, true));
        g.drawText ("The drum machine arrives with the engine's drums.* parameters.", b.withTop (b.getHeight() * 0.5f - 10.0f).withHeight (20.0f),
                    juce::Justification::centred, false);
    }
}

//==============================================================================
void AnalogDoor::TrackPicker::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    const auto n = proc.drumTrackName (track);
    g.setColour (n.isEmpty() ? kKeyTextDim : kLed);
    g.setFont (heros (10.5f, true));
    g.drawFittedText (n.isEmpty() ? juce::String ("-") : n, getLocalBounds().reduced (6, 2), juce::Justification::centredLeft, 1);
    g.setColour (kKeyTextDim);
    juce::Path t; t.addTriangle (b.getRight() - 12.0f, b.getCentreY() - 2.0f, b.getRight() - 6.0f, b.getCentreY() - 2.0f, b.getRight() - 9.0f, b.getCentreY() + 2.5f);
    g.fillPath (t);
}

void AnalogDoor::TrackPicker::mouseUp (const juce::MouseEvent& e)
{
    if (! contains (e.getPosition())) return;
    juce::PopupMenu menu;
    const auto idx = proc.drumPresetIndices();
    juce::String fam;
    juce::PopupMenu sub;
    for (int i : idx)
    {
        const auto& pe = proc.presetEntry (i);
        if (pe.family != fam) { if (fam.isNotEmpty()) menu.addSubMenu (fam.fromFirstOccurrenceOf ("Drums - ", false, false), sub); sub = juce::PopupMenu(); fam = pe.family; }
        sub.addItem (i + 1, pe.name, true, pe.family + "/" + pe.name == proc.drumTrackName (track, true));
    }
    if (fam.isNotEmpty()) menu.addSubMenu (fam.fromFirstOccurrenceOf ("Drums - ", false, false), sub);
    juce::Component::SafePointer<TrackPicker> self (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                        [self] (int r) { if (self != nullptr && r > 0) { self->proc.setDrumTrack (self->track, r - 1); self->repaint(); } });
}
} // namespace jfui
