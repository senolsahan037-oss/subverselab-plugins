#include "ProfilePanel.h"
#include "Theme.h"

namespace mihenk::ui
{
namespace
{
constexpr float kPad = 14.0f, kHeader = 34.0f, kColGap = 18.0f, kButtonsY = 326.0f, kCap = 22.0f;

juce::String upper (const juce::String& s) { return s.toUpperCase(); }
juce::String lufs (double v) { return std::isfinite (v) ? juce::String (v, 1) + " LUFS" : juce::String ("-INF LUFS"); }
} // namespace

//==============================================================================
void drawPadlock (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c)
{
    const float w = r.getWidth(), h = r.getHeight();
    const auto body = juce::Rectangle<float> (r.getX(), r.getY() + h * 0.42f, w, h * 0.58f);
    juce::Path shackle;
    const float sw = w * 0.62f, sx = r.getCentreX() - sw * 0.5f;
    shackle.startNewSubPath (sx, body.getY() + 0.5f);
    shackle.lineTo (sx, r.getY() + sw * 0.5f);
    shackle.addArc (sx, r.getY(), sw, sw, -juce::MathConstants<float>::halfPi, juce::MathConstants<float>::halfPi);
    shackle.lineTo (sx + sw, body.getY() + 0.5f);
    g.setColour (juce::Colours::black.withAlpha (0.55f));
    g.strokePath (shackle, juce::PathStrokeType (juce::jmax (1.6f, w * 0.2f) + 1.4f), juce::AffineTransform::translation (0.0f, 0.8f));
    g.fillRoundedRectangle (body.translated (0.0f, 0.8f).expanded (0.7f), 1.8f);
    g.setColour (c);
    g.strokePath (shackle, juce::PathStrokeType (juce::jmax (1.6f, w * 0.2f)));
    g.setGradientFill (juce::ColourGradient (c.brighter (0.35f), body.getX(), body.getY(), c.darker (0.35f), body.getX(), body.getBottom(), false));
    g.fillRoundedRectangle (body, 1.6f);
    g.setColour (Theme::panelDeep);
    const auto hole = juce::Point<float> (body.getCentreX(), body.getY() + body.getHeight() * 0.45f);
    g.fillEllipse (hole.x - 1.3f, hole.y - 1.3f, 2.6f, 2.6f);
    g.fillRect (hole.x - 0.6f, hole.y, 1.2f, body.getHeight() * 0.32f);
}

void LockBadge::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    if (isMouseOver()) glow (g, b.getCentre(), b.getWidth(), Theme::lamp, 0.35f);
    drawPadlock (g, b.reduced (1.5f, 1.0f), isMouseOver() ? Theme::lamp : Theme::gold);
}

//==============================================================================
ProfilePanel::ProfilePanel (Context& c) : ctx (c)
{
    setWantsKeyboardFocus (false);
    for (auto* b : { &enable, &close, &save, &exportKey, &importKey, &resetKey, &folderKey }) addAndMakeVisible (*b);
    for (auto* b : { &close, &save, &exportKey, &importKey, &resetKey, &folderKey }) b->showLed = false;
    for (int t = 0; t < profile::kNumTags; ++t)
    {
        tags[(size_t) t] = std::make_unique<CapButton> (upper (profile::tagText (t)));
        auto& cap = *tags[(size_t) t];
        if (t == profile::tPerfect) cap.ledColour = Theme::teal.brighter (0.3f);
        cap.setTooltip (juce::String ("\"") + profile::tagText (t) + "\": " + profile::describeTagRule (t).fromFirstOccurrenceOf (": ", false, false)
                        + (t == profile::tPerfect ? " (every knob counts as accepted)" : " (one step per note)"));
        cap.onClick = [this, t]
        {
            // "perfect" and a complaint exclude each other.
            if (tags[(size_t) t]->getToggleState())
            {
                if (t == profile::tPerfect) { for (int o = 0; o < profile::kNumTags; ++o) if (o != t) tags[(size_t) o]->setToggleState (false, juce::dontSendNotification); }
                else tags[(size_t) profile::tPerfect]->setToggleState (false, juce::dontSendNotification);
            }
            repaint();
        };
        addAndMakeVisible (cap);
    }
    text.setMultiLine (true, true);
    text.setReturnKeyStartsNewLine (false);
    text.setFont (Theme::mono (9.5f));
    text.setBorder ({});
    text.setIndents (8, 6);
    text.setColour (juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
    text.setColour (juce::TextEditor::textColourId, Theme::vfd);
    text.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    text.setColour (juce::TextEditor::focusedOutlineColourId, Theme::gold.withAlpha (0.6f));
    text.setColour (juce::TextEditor::highlightColourId, Theme::teal.withAlpha (0.4f));
    text.setColour (juce::TextEditor::highlightedTextColourId, Theme::ink);
    text.setColour (juce::CaretComponent::caretColourId, Theme::lamp);
    text.setTextToShowWhenEmpty ("WHAT DID YOU HEAR? (KEPT FOR YOU; THE TAGS DECIDE WHAT IS LEARNED)", Theme::vfd.withAlpha (0.35f));
    text.onReturnKey = [this] { saveNote(); };
    addAndMakeVisible (text);

    enable.setTooltip ("PERSONAL PROFILE: MIHENK learns your taste from your listening notes and locks the settings you keep choosing. Off by default; stored only on this computer");
    close.setTooltip ("Close the profile panel");
    save.setTooltip ("Save the note with the measurements of this track, the settings, the starting point, the scenarios and the result");
    exportKey.setTooltip ("Save your whole profile (every note) as one JSON file");
    importKey.setTooltip ("Add the notes of an exported profile (notes already here are skipped)");
    resetKey.setTooltip ("Clear the profile: every rule and lock goes; the notes file is kept aside, not deleted");
    folderKey.setTooltip ("Show the profile folder: " + ctx.session.profile.folder().getFullPathName());

    enable.onClick = [this]
    {
        ctx.session.setProfileEnabled (enable.getToggleState());
        refresh();
        if (onChanged) onChanged();
    };
    close.onClick = [this] { if (onClose) onClose(); };
    save.onClick = [this] { saveNote(); };
    exportKey.onClick = [this] { exportProfile(); };
    importKey.onClick = [this] { importProfile(); };
    resetKey.onClick = [this] { resetAll(); };
    folderKey.onClick = [this]
    {
        auto d = ctx.session.profile.folder();
        d.createDirectory();
        d.revealToUser();
    };
}

juce::Rectangle<float> ProfilePanel::listArea() const
{
    const float colW = ((float) getWidth() - 2.0f * kPad - kColGap) * 0.5f;
    return { kPad + colW + kColGap, 58.0f, colW, 238.0f };
}

juce::Rectangle<float> ProfilePanel::contextArea() const
{
    const float colW = ((float) getWidth() - 2.0f * kPad - kColGap) * 0.5f;
    return { kPad, 198.0f, colW, 118.0f };
}

void ProfilePanel::resized()
{
    const int w = getWidth();
    close.setBounds (w - (int) kPad - 64, 8, 64, (int) kCap);
    enable.setBounds (close.getX() - 6 - 150, 8, 150, (int) kCap);
    const float colW = ((float) w - 2.0f * kPad - kColGap) * 0.5f;
    const int x0 = (int) kPad, cw = (int) ((colW - 3.0f * 4.0f) / 4.0f);
    for (int t = 0; t < profile::kNumTags; ++t)
    {
        const int row = t < 4 ? 0 : 1, col = t < 4 ? t : t - 4;
        tags[(size_t) t]->setBounds (x0 + col * (cw + 4), 58 + row * 26, cw, (int) kCap);
    }
    text.setBounds (x0, 116, (int) colW, 60);
    save.setBounds (x0 + (int) colW - 130, (int) kButtonsY, 130, (int) kCap);
    const auto list = listArea();
    int bx = (int) list.getRight();
    for (auto* b : { &resetKey, &importKey, &exportKey, &folderKey })
    {
        const int bw = b == &folderKey ? 92 : 84;
        bx -= bw;
        b->setBounds (bx, (int) kButtonsY, bw, (int) kCap);
        bx -= 4;
    }
}

void ProfilePanel::refresh()
{
    auto& s = ctx.session;
    enable.setToggleState (s.profileEnabled, juce::dontSendNotification);
    const bool on = s.profileEnabled;
    for (auto& t : tags) t->setEnabled (on);
    text.setEnabled (on);
    save.setEnabled (on && s.diagnosis.m.valid);
    const auto& notes = s.profile.notes();
    exportKey.setEnabled (! notes.empty());
    resetKey.setEnabled (! notes.empty());
    const int rows = (int) s.profile.learned().rules.size();
    scroll = juce::jlimit (0, juce::jmax (0, rows - 5), scroll);
    repaint();
}

void ProfilePanel::prefillForSnapshot (const juce::StringArray& tagIds, const juce::String& t)
{
    for (int i = 0; i < profile::kNumTags; ++i) tags[(size_t) i]->setToggleState (tagIds.contains (profile::tagId (i)), juce::dontSendNotification);
    text.setText (t, false);
    text.moveCaretToEnd();
    repaint();
}

void ProfilePanel::setMessage (const juce::String& m, bool error)
{
    message = m;
    messageIsError = error;
    repaint();
}

void ProfilePanel::saveNote()
{
    juce::StringArray picked;
    for (int t = 0; t < profile::kNumTags; ++t) if (tags[(size_t) t]->getToggleState()) picked.add (profile::tagId (t));
    juce::String error;
    if (ctx.session.addNote (picked, text.getText(), startingPoint ? startingPoint() : juce::String(), error))
    {
        for (auto& t : tags) t->setToggleState (false, juce::dontSendNotification);
        text.clear();
        const int n = (int) ctx.session.profile.notes().size();
        setMessage ("NOTE SAVED: " + juce::String (n) + (n == 1 ? " NOTE" : " NOTES") + " IN YOUR PROFILE");
    }
    else
        setMessage (upper (error), true);
    refresh();
}

void ProfilePanel::clearLocksOfRule (const juce::String& key)
{
    auto& s = ctx.session;
    for (auto& l : s.locks)
        if (l.on && (key.isEmpty() || l.ruleKey == key)) l = {};
}

void ProfilePanel::deleteRule (const juce::String& key, const juce::String& what)
{
    juce::Component::SafePointer<ProfilePanel> safe (this);
    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Delete rule",
                                        "Stop learning " + what + " on these tracks? Your notes and their text stay; this knob is taken out of them.",
                                        "DELETE", "CANCEL", this,
                                        juce::ModalCallbackFunction::create ([safe, key] (int ok)
                                        {
                                            if (safe == nullptr || ok != 1) return;
                                            juce::String err;
                                            if (safe->ctx.session.profile.deleteRule (key, err))
                                            {
                                                safe->clearLocksOfRule (key);
                                                safe->setMessage ("RULE DELETED");
                                            }
                                            else safe->setMessage (upper (err), true);
                                            safe->refresh();
                                            if (safe->onChanged) safe->onChanged();
                                        }));
}

void ProfilePanel::resetAll()
{
    juce::Component::SafePointer<ProfilePanel> safe (this);
    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::WarningIcon, "Reset personal profile",
                                        "Clear every rule and lock? The notes file is moved aside in the profile folder (not deleted).", "RESET", "CANCEL", this,
                                        juce::ModalCallbackFunction::create ([safe] (int ok)
                                        {
                                            if (safe == nullptr || ok != 1) return;
                                            juce::String err;
                                            if (safe->ctx.session.profile.resetAll (err))
                                            {
                                                safe->clearLocksOfRule ({});
                                                safe->ctx.session.overridden.clear();
                                                safe->setMessage ("PROFILE CLEARED");
                                            }
                                            else safe->setMessage (upper (err), true);
                                            safe->refresh();
                                            if (safe->onChanged) safe->onChanged();
                                        }));
}

void ProfilePanel::exportProfile()
{
    const auto base = juce::File::getSpecialLocation (juce::File::userDesktopDirectory).getChildFile ("MIHENK personal profile.json");
    chooser = std::make_unique<juce::FileChooser> ("Export personal profile", base, "*.json");
    juce::Component::SafePointer<ProfilePanel> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [safe] (const juce::FileChooser& fc)
    {
        if (safe == nullptr) return;
        auto f = fc.getResult();
        if (f == juce::File()) return;
        if (! f.hasFileExtension ("json")) f = f.withFileExtension ("json");
        juce::String err;
        if (safe->ctx.session.profile.exportTo (f, err)) safe->setMessage ("EXPORTED " + juce::String ((int) safe->ctx.session.profile.notes().size()) + " NOTES TO " + upper (f.getFileName()));
        else safe->setMessage (upper (err), true);
    });
}

void ProfilePanel::importProfile()
{
    chooser = std::make_unique<juce::FileChooser> ("Import personal profile", juce::File::getSpecialLocation (juce::File::userDesktopDirectory), "*.json;*.jsonl");
    juce::Component::SafePointer<ProfilePanel> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc)
    {
        if (safe == nullptr || ! fc.getResult().existsAsFile()) return;
        juce::String err;
        const int n = safe->ctx.session.profile.importFrom (fc.getResult(), err);
        if (n >= 0) safe->setMessage ("IMPORTED " + juce::String (n) + " NEW NOTE(S)");
        else safe->setMessage (upper (err), true);
        safe->refresh();
    });
}

//==============================================================================
void ProfilePanel::paint (juce::Graphics& g)
{
    hits.clear();
    auto& s = ctx.session;
    const auto b = getLocalBounds().toFloat();
    // A hinged service panel over the CRT: the same anodized green, a gold rule round it.
    g.setColour (juce::Colours::black.withAlpha (0.55f));
    g.fillRoundedRectangle (b.translated (0.0f, 3.0f), 7.0f);
    g.setGradientFill (juce::ColourGradient (Theme::panel.brighter (0.22f), 0.0f, 0.0f, Theme::panelDeep, 0.0f, b.getBottom(), false));
    g.fillRoundedRectangle (b, 7.0f);
    g.setColour (Theme::gold.withAlpha (0.85f));
    g.drawRoundedRectangle (b.reduced (0.5f), 7.0f, 1.2f);
    g.setColour (Theme::gold.withAlpha (0.25f));
    g.drawRoundedRectangle (b.reduced (4.0f), 5.0f, 0.8f);

    const float w = b.getWidth();
    const float colW = (w - 2.0f * kPad - kColGap) * 0.5f;
    g.setColour (Theme::ink);
    tracked (g, "PERSONAL PROFILE", Theme::legend (10.5f), { kPad, 9.0f, 170.0f, 20.0f }, juce::Justification::centredLeft, 0.18f);
    g.setColour (Theme::ink.withAlpha (0.6f));
    tracked (g, "LEARNED FROM YOUR LISTENING NOTES. STORED ON THIS COMPUTER ONLY; NOTHING IS SENT ANYWHERE", Theme::fine (7.5f),
             { kPad + 176.0f, 9.0f, (float) enable.getX() - kPad - 186.0f, 20.0f }, juce::Justification::centredLeft, 0.12f);
    g.setColour (Theme::ink.withAlpha (0.18f));
    g.drawHorizontalLine ((int) kHeader + 2, kPad, w - kPad);
    g.drawVerticalLine ((int) (kPad + colW + kColGap * 0.5f), kHeader + 10.0f, kButtonsY + kCap);

    // ---- Left: the listening note ---------------------------------------------------------------------------------
    const bool on = s.profileEnabled;
    g.setColour (Theme::ink.withAlpha (on ? 1.0f : 0.5f));
    tracked (g, "LISTENING NOTE", Theme::legend (8.5f), { kPad, 42.0f, 140.0f, 12.0f }, juce::Justification::centredLeft, 0.16f);
    g.setColour (Theme::ink.withAlpha (0.55f));
    tracked (g, on ? "PICK WHAT YOU HEARD, OR WRITE IT" : "OFF: TURN ON PERSONAL PROFILE TO KEEP NOTES", Theme::fine (7.5f),
             { kPad + 140.0f, 42.0f, colW - 140.0f, 12.0f }, juce::Justification::centredRight, 0.1f);
    const auto textBox = text.getBounds().toFloat().expanded (4.0f, 3.0f);
    vfdGlass (g, textBox);

    g.setColour (Theme::ink.withAlpha (on ? 0.85f : 0.45f));
    const auto ctxArea = contextArea();
    tracked (g, "SAVED WITH THE NOTE", Theme::legend (7.5f), { ctxArea.getX(), ctxArea.getY() - 15.0f, 160.0f, 12.0f }, juce::Justification::centredLeft, 0.14f);
    vfdGlass (g, ctxArea);
    {
        auto inner = ctxArea.reduced (11.0f, 8.0f);
        const float lh = inner.getHeight() / 6.0f;
        const auto line = [&] (const juce::String& label, const juce::String& value)
        {
            auto r = inner.removeFromTop (lh);
            vfdText (g, label, r.withWidth (74.0f), juce::Justification::centredLeft, 7.5f, Theme::lamp.withAlpha (on ? 0.8f : 0.4f));
            vfdText (g, value, r.withTrimmedLeft (78.0f), juce::Justification::centredLeft, 8.0f, Theme::vfd.withAlpha (on ? 0.85f : 0.4f));
        };
        const auto& m = s.diagnosis.m;
        if (! m.valid)
        {
            line ("TRACK", ctx.isApp() ? "NOTHING LOADED: OPEN A MIX FIRST" : "NOTHING LEARNED: PRESS LEARN AND PLAY THE SONG");
        }
        else
        {
            line ("TRACK", upper (s.inputName) + "   (" + upper (s.profileSource) + ")");
            line ("INPUT", lufs (s.inMeas.integrated) + "  PLR " + juce::String (m.plr, 1) + " LU  CREST " + juce::String (s.inMeas.crest, 1) + " DB  CORR "
                               + juce::String (s.inMeas.correlation, 2));
            const auto f = profile::featuresOf (m);
            juce::StringArray finds;
            for (auto& x : s.diagnosis.findings) finds.add (upper (x.id));
            line ("MEASURED", upper (profile::describe (f)) + (finds.isEmpty() ? juce::String() : "; " + juce::String (finds.size()) + " FINDING(S)"));
            juce::StringArray set;
            for (int p = 0; p < profile::kNumParams; ++p)
                if (p != profile::pLowShelf && p != profile::pHighShelf) set.add (juce::String (profile::def (p).knob) + " " + upper (profile::formatValue (p, profile::get (s.settings, p))));
            line ("SETTINGS", (startingPoint ? upper (startingPoint()) : juce::String()) + (s.appliedScenarios.isEmpty() ? juce::String() : " + " + juce::String (s.appliedScenarios.size()) + " SCENARIO(S)")
                                  + "   " + set.joinIntoString ("  "));
            line ("RESULT", s.outMeas.valid ? lufs (s.outMeas.integrated) + "  " + juce::String (s.outMeas.truePeak, 2) + " DBTP  LIMITER " + juce::String (s.limGr.maxDb, 1) + " DB MAX"
                                            : juce::String ("NOT RENDERED YET"));
            juce::StringArray ov;
            for (auto& [p, from] : s.overridden)
            {
                const double to = profile::get (s.settings, p);
                if (std::abs (to - from) > 1.0e-6) ov.add (juce::String (profile::def (p).knob) + " " + upper (profile::formatValue (p, from)) + " > " + upper (profile::formatValue (p, to)));
            }
            line ("OVERRIDES", ov.isEmpty() ? juce::String ("NONE (MOVE A LOCKED KNOB TO OVERRIDE IT)") : ov.joinIntoString ("   "));
        }
    }
    vfdGlassTop (g, ctxArea);
    if (message.isNotEmpty())
    {
        g.setColour (messageIsError ? Theme::coral : Theme::lamp);
        tracked (g, message, Theme::legend (7.5f), { kPad, kButtonsY, colW - 140.0f, kCap }, juce::Justification::centredLeft, 0.1f);
    }

    // ---- Right: what was learned ----------------------------------------------------------------------------------
    const auto list = listArea();
    const auto& learned = s.profile.learned();
    const int nNotes = (int) s.profile.notes().size();
    g.setColour (Theme::ink);
    tracked (g, "LEARNED FROM YOUR NOTES", Theme::legend (8.5f), { list.getX(), 42.0f, 200.0f, 12.0f }, juce::Justification::centredLeft, 0.16f);
    g.setColour (Theme::ink.withAlpha (0.6f));
    tracked (g, juce::String (nNotes) + (nNotes == 1 ? " NOTE, " : " NOTES, ") + juce::String ((int) learned.groups.size())
                 + ((int) learned.groups.size() == 1 ? " GROUP OF TRACKS" : " GROUPS OF TRACKS"), Theme::fine (7.5f),
             { list.getX() + 200.0f, 42.0f, list.getWidth() - 200.0f, 12.0f }, juce::Justification::centredRight, 0.1f);
    vfdGlass (g, list);
    const auto inner = list.reduced (10.0f, 7.0f);
    const int n = (int) learned.rules.size();
    if (n == 0)
    {
        vfdText (g, "NOTHING LEARNED YET", inner.withHeight (inner.getHeight() * 0.5f - 4.0f), juce::Justification::centredBottom, 9.0f, Theme::vfd.withAlpha (0.75f));
        vfdText (g, "A KNOB IS LOCKED ONLY AFTER " + juce::String (profile::kMinEvidence) + " AGREEING NOTES ON SIMILAR TRACKS",
                 inner.withTrimmedTop (inner.getHeight() * 0.5f + 2.0f).withHeight (14.0f), juce::Justification::centredTop, 7.5f, Theme::vfd.withAlpha (0.45f));
    }
    else
    {
        const float rowH = inner.getHeight() / 5.0f;
        for (int i = 0; i < 5 && scroll + i < n; ++i)
        {
            const auto& r = learned.rules[(size_t) (scroll + i)];
            const auto& pd = profile::def (r.param);
            auto row = juce::Rectangle<float> (inner.getX(), inner.getY() + rowH * (float) i, inner.getWidth(), rowH);
            if (row.contains (mouse)) { g.setColour (Theme::vfd.withAlpha (0.05f)); g.fillRect (row); }
            if (i > 0) { g.setColour (Theme::vfd.withAlpha (0.10f)); g.drawHorizontalLine ((int) row.getY(), row.getX(), row.getRight()); }
            const bool acting = r.state == profile::Rule::State::acting;
            const juce::Colour lampC = acting ? Theme::teal.brighter (0.3f) : r.state == profile::Rule::State::mixed ? Theme::coral : Theme::lamp;
            const juce::Point<float> lc (row.getX() + 5.0f, row.getY() + 10.0f);
            glow (g, lc, 8.0f, lampC, 0.45f);
            g.setColour (lampC);
            g.fillEllipse (lc.x - 3.5f, lc.y - 3.5f, 7.0f, 7.0f);
            // Delete cap on the first line.
            const auto del = juce::Rectangle<float> (row.getRight() - 52.0f, row.getY() + 3.0f, 52.0f, 15.0f);
            g.setColour (juce::Colour (0xff020d0c));
            g.fillRoundedRectangle (del, 2.5f);
            const auto capR = del.reduced (1.2f);
            g.setGradientFill (juce::ColourGradient (Theme::capCream.brighter (del.contains (mouse) ? 0.08f : 0.0f), capR.getX(), capR.getY(),
                                                     juce::Colour (0xffC4BBA3), capR.getX(), capR.getBottom(), false));
            g.fillRoundedRectangle (capR, 2.0f);
            g.setColour (Theme::panel);
            tracked (g, "DELETE", Theme::legend (7.0f), capR, juce::Justification::centred, 0.1f);
            bool lockedNow = false;
            for (auto& l : s.locks) lockedNow |= l.on && l.ruleKey == r.key;
            const juce::String stateText = acting ? (lockedNow ? "LOCKED NOW" : "ACTS") : r.state == profile::Rule::State::mixed ? "MIXED: NO LOCK"
                                                  : "COLLECTING " + juce::String (r.evidence) + "/" + juce::String (profile::kMinEvidence);
            const float x0 = row.getX() + 15.0f;
            vfdText (g, juce::String (pd.knob) + "  " + upper (profile::formatValue (r.param, r.value)), { x0, row.getY() + 2.0f, 150.0f, 16.0f },
                     juce::Justification::centredLeft, 10.0f, acting ? Theme::vfd : Theme::vfd.withAlpha (0.6f));
            vfdText (g, stateText, { x0 + 150.0f, row.getY() + 2.0f, 104.0f, 16.0f }, juce::Justification::centredLeft, 7.5f, lockedNow ? Theme::lamp : lampC);
            vfdText (g, juce::String (r.evidence) + (r.evidence == 1 ? " NOTE" : " NOTES") + "   " + upper (r.updated.formatted ("%d %b %H:%M")),
                     { x0 + 254.0f, row.getY() + 2.0f, del.getX() - x0 - 262.0f, 16.0f }, juce::Justification::centredRight, 7.5f, Theme::vfd.withAlpha (0.6f));
            vfdText (g, "TRACKS LIKE: " + upper (learned.groups[(size_t) r.group].text), { x0, row.getY() + 21.0f, row.getRight() - x0, 14.0f },
                     juce::Justification::centredLeft, 7.5f, Theme::vfd.withAlpha (0.5f));
            hits.push_back ({ del, r.key, 1, "Delete this rule: " + juce::String (pd.knob) + " is taken out of your notes on these tracks (the notes stay)" });
            hits.push_back ({ row, r.key, 0, juce::String (pd.knob) + " " + profile::formatValue (r.param, r.value) + ": " + r.why });
        }
        if (n > 5)
        {
            const juce::String more = (scroll > 0 ? juce::String ("^ ") : juce::String()) + juce::String (n - 5 - scroll) + " MORE (SCROLL)";
            vfdText (g, more, { list.getRight() - 124.0f, list.getBottom() - 13.0f, 114.0f, 10.0f }, juce::Justification::centredRight, 7.0f, Theme::lamp.withAlpha (0.8f));
        }
    }
    vfdGlassTop (g, list);
    g.setColour (Theme::ink.withAlpha (0.5f));
    tracked (g, "EACH TAG MOVES ITS KNOBS ONE STEP: AIR, SHELVES, BELL 2, TIGHT 0.5 DB; GLUE 0.3 DB; WIDTH 5 %. NEWER NOTES WEIGH MORE",
             Theme::fine (7.0f), { list.getX(), list.getBottom() + 5.0f, list.getWidth(), 12.0f }, juce::Justification::centredLeft, 0.08f);
}

void ProfilePanel::mouseDown (const juce::MouseEvent& e)
{
    for (const auto& h : hits)
        if (h.area.contains (e.position))
        {
            if (h.kind == 1)
            {
                juce::String what = "this knob";
                for (auto& r : ctx.session.profile.learned().rules) if (r.key == h.ruleKey) what = profile::def (r.param).knob;
                deleteRule (h.ruleKey, what);
            }
            return;
        }
}

void ProfilePanel::mouseMove (const juce::MouseEvent& e) { mouse = e.position; repaint (listArea().toNearestInt()); }
void ProfilePanel::mouseExit (const juce::MouseEvent&) { mouse = { -1.0f, -1.0f }; repaint (listArea().toNearestInt()); }

void ProfilePanel::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    const int n = (int) ctx.session.profile.learned().rules.size();
    scroll = juce::jlimit (0, juce::jmax (0, n - 5), scroll + (w.deltaY < 0.0f ? 1 : -1));
    repaint();
}

juce::String ProfilePanel::getTooltip()
{
    for (const auto& h : hits)
        if (h.area.contains (mouse)) return h.tip;
    return {};
}
} // namespace mihenk::ui
