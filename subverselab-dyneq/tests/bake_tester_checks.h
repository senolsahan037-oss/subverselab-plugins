// plugin_check section 9b (25 Oct 2026, 0.9.1-test2): BAKE as the first tester ran it (Andre, Logic Pro 12 AU, build 0.9.0-test1, T5 BLOCKED):
// "BAKE ARM lit up with no explanation of the next step. APPLY during playback: no completion indication; the curve continued changing and returned
// flat after playback stopped; could not confirm a bake." His T4 (SELF on a drum bus) came just before T5, and his six free bands were unused.
// Here the same steps run through the real processor and the editor's own keys (BAKE ARM, APPLY), during "playback" (audio flowing, the play-head
// running) and after it stops. Included by plugin_check.cpp after editor_checks.h.
#pragma once

namespace bt
{
inline juce::Component* findAction (DyneqEditor& ed, const juce::String& action)
{
    juce::Component* r = nullptr;
    ed.forEachControl ([&] (juce::Component& c, PanelControl& pc) { if (r == nullptr && pc.uiAction() == action) r = &c; });
    return r;
}
inline juce::Component* findBound (DyneqEditor& ed, const juce::String& id)
{
    juce::Component* r = nullptr;
    ed.forEachControl ([&] (juce::Component& c, PanelControl& pc) { if (r == nullptr && dynamic_cast<juce::Button*> (&c) != nullptr && pc.boundParams().size() == 1 && pc.boundParams()[0] == id) r = &c; });
    return r;
}
inline bool bandsUnused (DyneqAudioProcessor& p) { for (int j = 0; j < dyneq::kFreeBands; ++j) if (! p.bandIsEmpty (j)) return false; return true; }
}

static void runBakeTesterChecks (const Audio& beatFull, const Audio& vocalFull)
{
    std::printf ("\n---- 9b. BAKE as the first tester ran it (T5, 0.9.0-test1) ----\n");
    const double sr = 48000.0;
    const Audio beat8 = slice (beatFull, 0, 48000 * 8), vocal8 = slice (vocalFull, 0, 48000 * 8);
    const Audio beat2 = slice (beatFull, 48000 * 8, 48000 * 2), vocal2 = slice (vocalFull, 48000 * 8, 48000 * 2);

    // (a) his set-up: SELF on a drum bus (T4), BAKE ARM through the editor's key, the transport running, APPLY through the editor's key during playback
    {
        Rig r (2, true, sr, 512, { { "mode", 1 } });
        DyneqEditor ed (r.proc); ed.setAdvancedOpen (true); ec::settle (ed);
        auto* arm = dynamic_cast<juce::Button*> (bt::findBound (ed, "bakeArm"));
        auto* apply = dynamic_cast<juce::Button*> (bt::findAction (ed, "BAKE APPLY"));
        const auto idle = r.proc.bakeStatus();
        if (arm != nullptr) arm->triggerClick();
        // 26 Oct: wait until the click has reached the parameter (up to 2 s). A fixed 20 ms of message loop was not always enough on a loaded machine: twice the
        // render started un-armed and the check read the idle line (plugin_check run under three bench jobs; passes when the click has landed)
        for (int t = 0; t < 100 && r.proc.apvts.getRawParameterValue ("bakeArm")->load() <= 0.5f; ++t) juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
        r.render (beat8, nullptr, 512); ec::settle (ed);
        const auto during = r.proc.bakeStatus();
        const double heard = r.proc.bakeCountedSeconds();
        if (apply != nullptr) apply->triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
        r.render (beat2, nullptr, 512); r.ph.playing = false; r.render (beat2, nullptr, 512); ec::settle (ed);
        const auto after = r.proc.bakeStatus();
        check (arm != nullptr && apply != nullptr && r.proc.apvts.getRawParameterValue ("bakeArm")->load() > 0.5f && heard == 0.0 && bt::bandsUnused (r.proc)
                   && idle.state == DyneqAudioProcessor::kBakeIdle && during.state == DyneqAudioProcessor::kBakeNotListening && during.line1.contains ("COLLIDE only")
                   && after.state == DyneqAudioProcessor::kBakeRefused && after.line1 == "APPLY WROTE NOTHING",
               "T5 repro (a), SELF on the drum bus: BAKE ARM through its key lights, 8 s of playback count 0 s (BAKE learns from COLLIDE only), APPLY writes nothing; 0.9.0 said nothing in LAB, now the BAKE line says why",
               "idle: '" + idle.line1 + "' | armed in SELF: '" + during.line1 + " | " + during.line2 + "' | after APPLY: '" + after.line1 + " | " + after.line2 + "' | drawn: '" + ed.advancedBakeText() + "'");
    }
    // (b) COLLIDE with no key routed (Logic: the side-chain menu left at None)
    {
        Rig r (2, false, sr, 512, { { "bakeArm", 1 } });
        r.render (beat8, nullptr, 512);
        const auto st = r.proc.bakeStatus(); const int w = r.proc.bakeApply(); const auto after = r.proc.bakeStatus();
        check (w == 0 && r.proc.bakeCountedSeconds() == 0.0 && st.line1.contains ("NO KEY") && after.state == DyneqAudioProcessor::kBakeRefused && bt::bandsUnused (r.proc),
               "T5 repro (b), COLLIDE with no key routed: nothing is counted (0.9.0 counted blocks of zero cut), APPLY writes nothing and says why",
               "'" + st.line1 + "' -> '" + after.line2 + "'");
    }
    // (c) COLLIDE keyed by the vocal (his T1), armed through the key, APPLY during playback through the editor's key, playback goes on, then stops
    {
        Rig r (2, true, sr, 512, {});
        DyneqEditor ed (r.proc); ed.setAdvancedOpen (true); ec::settle (ed);
        auto* arm = dynamic_cast<juce::Button*> (bt::findBound (ed, "bakeArm"));
        auto* apply = dynamic_cast<juce::Button*> (bt::findAction (ed, "BAKE APPLY"));
        if (arm != nullptr) arm->triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
        r.render (beat8, &vocal8, 512); ec::settle (ed);
        const auto listening = r.proc.bakeStatus();
        const auto sug = r.proc.bakeSuggest();
        const int steps0 = r.proc.undo.undoSteps();
        const auto before = snapOf (r.proc);
        if (apply != nullptr) apply->triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
        const bool lampOff = r.proc.apvts.getRawParameterValue ("assistOff")->load() > 0.5f;
        bool written = sug.count >= 1;
        for (int i = 0; i < sug.count; ++i)
        {
            const int j = sug.slot[i];
            written = written && rawOf (r.proc.apvts, panel::bandId (j, "on").toRawUTF8()) == 1.0f && std::abs (rawOf (r.proc.apvts, panel::bandId (j, "freq").toRawUTF8()) - sug.bells[i].freqHz) < 0.011f
                      && std::abs (rawOf (r.proc.apvts, panel::bandId (j, "gain").toRawUTF8()) - sug.bells[i].gainDb) < 0.011f;
        }
        const double frozen = r.proc.bakeCountedSeconds();
        r.render (beat2, &vocal2, 512);                       // playback goes on after APPLY
        const double frozen2 = r.proc.bakeCountedSeconds();
        r.ph.playing = false; r.render (beat2, &vocal2, 512); ec::settle (ed);   // ... and stops (the host keeps calling with the transport stopped)
        // after the stop: the baked bands are still there and running at their gains; the dynamic layer is off, so no COLLIDE cut is shown
        bool stays = true; float maxCollide = 0.0f;
        for (int i = 0; i < sug.count; ++i) stays = stays && r.proc.apvts.getRawParameterValue (panel::bandId (sug.slot[i], "on"))->load() > 0.5f
                                                && std::abs (r.proc.getFreeBandGainDb (sug.slot[i]) - sug.bells[i].gainDb) < 0.05f;
        for (int b = 0; b < dyneq::kBands; ++b) maxCollide = std::max (maxCollide, r.proc.getCutDb (b));
        const auto baked = r.proc.bakeStatus();
        // the chips show the baked bands as in use
        int chipsOn = 0; for (int i = 0; i < sug.count; ++i) if (ed.ctl().bandOn (sug.slot[i]) && ! ed.ctl().isEmptySlot (sug.slot[i])) ++chipsOn;
        check (arm != nullptr && apply != nullptr && listening.state == DyneqAudioProcessor::kBakeListening && listening.line2.startsWith ("APPLY writes BAND")
                   && written && lampOff && r.proc.undo.undoSteps() == steps0 + 1 && frozen == frozen2 && stays && maxCollide == 0.0f && chipsOn == sug.count
                   && baked.state == DyneqAudioProcessor::kBakeBaked && baked.line1.startsWith ("BAKED " + juce::String (sug.count)),
               "T5 repro (c), COLLIDE keyed by the vocal: APPLY during playback writes the preview's bells into the empty bands and switches COLLIDE off in ONE undo step; "
               "the statistics stop; after the transport stops the baked bands stay at their gains (the display keeps the static curve), the BAKE line says what was baked",
               "listening: '" + listening.line1 + " | " + listening.line2 + "' | baked: '" + baked.line1 + " | " + baked.line2 + "' | counted " + juce::String (frozen, 2) + " s -> " + juce::String (frozen2, 2)
               + " s after APPLY | largest COLLIDE cut after the stop " + juce::String (maxCollide, 2) + " dB");
        // undo = the dynamic cut back, bit-exact; the BAKE line no longer claims the bake; redo = baked again
        r.proc.undo.undo();
        const auto undone = r.proc.bakeStatus();
        const bool back = snapDiff (before, snapOf (r.proc), &r.proc) == 0 && undone.state != DyneqAudioProcessor::kBakeBaked;
        r.proc.undo.redo();
        check (back && r.proc.bakeStatus().state == DyneqAudioProcessor::kBakeBaked && r.proc.apvts.getRawParameterValue ("assistOff")->load() > 0.5f,
               "Cmd+Z after a bake returns every parameter bit-exact (COLLIDE on, bands empty) and the line stops saying BAKED; Cmd+Shift+Z bakes again (A / B of the static and the dynamic cut)",
               "after undo: '" + undone.line1 + "'");
        // the hover help of BAKE ARM and APPLY: the popover opens over the panel and the window does not grow
        const auto size0 = ed.getBounds();
        const bool h1 = arm != nullptr && ed.showHelpFor (*arm) && ed.helpShown() && ed.helpTitle().startsWith ("BAKE ARM") && ed.helpText().contains ("APPLY");
        const bool h2 = apply != nullptr && ed.showHelpFor (*apply) && ed.helpShown() && ed.helpText().contains ("Cmd+Z");
        ed.hideHelp();
        check (h1 && h2 && ed.getBounds() == size0, "hover help: BAKE ARM and APPLY open a popover with the steps over the panel; the window stays " + juce::String (size0.getWidth()) + " x " + juce::String (size0.getHeight()));
    }
    // (d) an OFF band that holds the user's settings is never baked into (0.9.0 counted every OFF band as empty)
    {
        Rig r (2, true, sr, 512, { { "bakeArm", 1 }, { "b1_on", 0 }, { "b1_gain", 3.0f }, { "b1_freq", 2500.0f } });
        r.render (beat8, &vocal8, 512);
        const auto s = r.proc.bakeSuggest();
        bool notFirst = s.count >= 1; for (int i = 0; i < s.count; ++i) notFirst = notFirst && s.slot[i] != 0;
        r.proc.bakeApply();
        check (notFirst && s.emptyBands == 5 && std::abs (rawOf (r.proc.apvts, "b1_gain") - 3.0f) < 0.011f && std::abs (rawOf (r.proc.apvts, "b1_freq") - 2500.0f) < 0.5f && rawOf (r.proc.apvts, "b1_on") == 0.0f,
               "BAKE skips an OFF band that holds settings (band 1: off, +3 dB at 2.5 kHz): 5 empty bands, the bells go into 2..6, band 1 is untouched");
    }
}
