// HIGH STEREO panel mouse checks, headless: the real editor's own mouse handlers are called with synthetic events
// (no window, no screen capture, no real pointer). Covers drag, fine drag, wheel, click-to-type, double-click to the loaded
// value, glide, switches, LISTEN, the STAGE puck, the WIDTH field grips + magnet + relearn, undo.
// NOT covered here (needs a real window): the preset list menu, the save dialog, the hover summary from the real pointer.
#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"
#include <cstdio>

static int fails = 0, checks = 0;
static void check (bool ok, const char* what, double v)
{
    ++checks; if (! ok) ++fails;
    std::printf ("[%s] %-66s %.4f\n", ok ? "PASS" : "FAIL", what, v);
}

using P = juce::Point<float>;
static juce::MouseEvent ev (juce::Component& c, P pos, P down, int clicks, bool dragged, juce::ModifierKeys mods = {})
{
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), pos, mods,
                             juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
                             juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX, juce::MouseInputSource::defaultTiltY,
                             &c, &c, now, down, now, clicks, dragged);
}
static void click (juce::Component& c, P p, int clicks = 1)
{
    c.mouseDown (ev (c, p, p, clicks, false));
    c.mouseUp (ev (c, p, p, clicks, false));
    if (clicks >= 2) c.mouseDoubleClick (ev (c, p, p, clicks, false));
}
static void drag (juce::Component& c, P from, P to, juce::ModifierKeys mods = {}, int steps = 10)
{
    c.mouseDown (ev (c, from, from, 1, false, mods));
    for (int i = 1; i <= steps; ++i) c.mouseDrag (ev (c, from + (to - from) * ((float) i / (float) steps), from, 1, true, mods));
    c.mouseUp (ev (c, to, from, 1, true, mods));
}
static void pump (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("hs_ui_test_" + juce::String (juce::Random::getSystemRandom().nextInt()));
    {
        HighStereoAudioProcessor proc;
        proc.presets.setDirectory (dir);   // never the user's real preset folder
        const double rate = 48000.0;
        proc.setPlayConfigDetails (2, 2, rate, 512);
        proc.prepareToPlay (rate, 512);
        std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
        auto* hs = dynamic_cast<HighStereoAudioProcessorEditor*> (ed.get());
        auto get = [&] (const char* id) { return proc.apvts.getRawParameterValue (id)->load(); };
        auto set = [&] (const char* id, float v) { auto* p = proc.apvts.getParameter (id); p->setValueNotifyingHost (p->convertTo0to1 (v)); };
        auto tick = [&] (int n) { for (int i = 0; i < n; ++i) hs->tickForSnapshot(); };

        // ---- round control WIDTH (0..200, 200 px per full range, Shift 800 px)
        auto& w = *dynamic_cast<hsui::RoundControl*> (hs->controlNamed ("width"));
        const P c = w.getLocalBounds().toFloat().getCentre();
        drag (w, c, c + P (0, -50));
        check (std::abs (get ("width") - 150.0f) < 0.6f, "WIDTH drag up 50 px = +25 % of range (100 -> 150)", get ("width"));
        drag (w, c, c + P (0, -50), juce::ModifierKeys::shiftModifier);
        check (std::abs (get ("width") - 162.0f) < 1.1f, "WIDTH Shift-drag 50 px = fine (150 -> ~162.5)", get ("width"));
        drag (w, c, c + P (0, 2));
        check (std::abs (get ("width") - 162.0f) < 1.1f, "a 2 px wobble is not a drag", get ("width"));
        check (w.shownValue() == get ("width"), "held/dragged control is drawn at its value (no glide)", w.shownValue());
        w.mouseWheelMove (ev (w, c, c, 0, false), juce::MouseWheelDetails { 0, 0.2f, false, false, false });
        check (get ("width") > 162.0f + 1.0f, "wheel up raises WIDTH", get ("width"));
        click (w, c, 2);
        check (get ("width") == 100.0f, "double-click = default (no preset loaded) 100", get ("width"));

        // glide: a value the product sets is drawn gliding, 30 % per tick, settles in ~0.3-0.5 s
        tick (40);
        set ("width", 200.0f);
        int t = 0; while (w.shownValue() != 200.0f && t < 60) { hs->tickForSnapshot(); ++t; }
        check (t >= 7 && t <= 16, "set value glides, ticks to settle (30 Hz; 7..16 = 0.23..0.53 s)", t);

        // click-to-type: single click on the number opens a text box after the double-click window
        set ("width", 100.0f); tick (30);
        click (w, c + P (0, 4));
        pump (450);
        juce::TextEditor* te = nullptr;
        for (auto* ch : w.getChildren()) if ((te = dynamic_cast<juce::TextEditor*> (ch)) != nullptr) break;
        check (te != nullptr, "single click on the number opens the type-in box", te != nullptr);
        if (te != nullptr)
        {
            te->setText ("137");
            te->onReturnKey();
            pump (50);
            check (get ("width") == 137.0f, "typed 137 + Return sets WIDTH 137", get ("width"));
            bool gone = true; for (auto* ch : w.getChildren()) if (dynamic_cast<juce::TextEditor*> (ch)) gone = false;
            check (gone, "the type-in box closes after Return", gone);
        }
        // a double-click on the number must not open the box
        click (w, c + P (0, 4), 1); click (w, c + P (0, 4), 2);
        pump (450);
        bool none = true; for (auto* ch : w.getChildren()) if (dynamic_cast<juce::TextEditor*> (ch)) none = false;
        check (none && get ("width") == 100.0f, "double-click on the number: default, no type-in box", get ("width"));

        // MONO BASS type "OFF"-range and OUTPUT type a negative value
        {
            auto& o = *dynamic_cast<hsui::RoundControl*> (hs->controlNamed ("output"));
            const P oc = o.getLocalBounds().toFloat().getCentre();
            click (o, oc + P (0, 4)); pump (450);
            juce::TextEditor* t2 = nullptr; for (auto* ch : o.getChildren()) if ((t2 = dynamic_cast<juce::TextEditor*> (ch)) != nullptr) break;
            if (t2 != nullptr) { t2->setText ("-3.5"); t2->onReturnKey(); pump (50); }
            check (t2 != nullptr && std::abs (get ("output") + 3.5f) < 0.051f, "OUTPUT typed -3.5 dB", get ("output"));
            click (o, oc, 2);
            check (std::abs (get ("output")) < 0.01f, "OUTPUT double-click back to 0 dB", get ("output"));
        }

        // undo: Cmd+Z after a drag returns the value
        tick (5); pump (600);
        drag (w, c, c + P (0, -20));
        const float afterDrag = get ("width");
        pump (600);
        hs->keyPressed (juce::KeyPress ('z', juce::ModifierKeys::commandModifier, 0));
        check (afterDrag == 120.0f && get ("width") == 100.0f, "Cmd+Z undoes the drag (120 -> 100)", get ("width"));

        // ---- double-click = the loaded preset's value
        set ("width", 140.0f); set ("band5", 60.0f);
        check (proc.presets.save ("UiTest"), "save a test preset (temp folder)", 1);
        proc.loadPreset ("UiTest");
        set ("width", 80.0f); set ("band5", 180.0f); tick (2);
        click (w, c, 2);
        check (get ("width") == 140.0f, "WIDTH double-click = loaded preset's 140", get ("width"));

        // ---- slide switches
        for (const char* id : { "in", "match", "fix" })
        {
            auto& s = *hs->controlNamed (id);
            const float before = get (id);
            click (s, { 10, 12 });
            const float mid = get (id);
            drag (s, { 10, 12 }, { 30, 12 });
            char what[96]; std::snprintf (what, sizeof what, "%s: click toggles, a drag does not", id);
            check (mid != before && get (id) == mid, what, mid);
            click (s, { 10, 12 });
        }

        // ---- LISTEN
        {
            auto& l = *hs->controlNamed ("listen");
            const float seg = (float) l.getWidth() / 5.0f;
            for (int i = 4; i >= 0; --i)
            {
                click (l, { seg * ((float) i + 0.5f), (float) l.getHeight() * 0.5f });
                char what[64]; std::snprintf (what, sizeof what, "LISTEN click segment %d", i);
                check ((int) proc.getListen() == i, what, (int) proc.getListen());
            }
        }

        // ---- STAGE puck (scope 250 px wide, centre (150, 183) in a 300 x 330 block)
        {
            auto& s = *hs->controlNamed ("stage");
            check (s.getWidth() == 300 && s.getHeight() == 330, "stage block 300 x 330 (geometry the test assumes)", s.getWidth());
            const P sc (150.0f, 183.0f); const float R = 125.0f;
            auto at = [&] (float deg) { const float r = juce::degreesToRadians (deg); return sc + P (-std::sin (r) * R * 0.8f, -std::cos (r) * R * 0.8f); };
            drag (s, at (0), at (90));
            check (std::abs (get ("stage") - 90.0f) < 0.6f, "drag the puck to the left = +90", get ("stage"));
            drag (s, at (90), at (-90), {}, 1);   // straight jump across
            check (std::abs (get ("stage") + 90.0f) < 0.6f, "jump across the front to the right = -90", get ("stage"));
            drag (s, at (150), at (-150), {}, 20);   // across the bottom: blocked
            check (get ("stage") > 170.0f, "dragging across the bottom stops at +180 (no jump to -180)", get ("stage"));
            click (s, at (30), 2);
            check (std::abs (get ("stage")) < 0.01f, "double-click returns the puck to the front", get ("stage"));
            const float before = get ("stage");
            click (s, { 4, 4 });
            check (get ("stage") == before, "click outside the stage circle changes nothing", get ("stage"));
        }

        // ---- WIDTH field: feed a 1 kHz tone with side > mid so the 1k-2k band learns a limit of ~67 %
        {
            proc.loadPreset ("Default"); tick (30);
            juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi; double ph = 0;
            auto feed = [&] (float sideGain, int blocks)
            {
                for (int b = 0; b < blocks; ++b)
                {
                    for (int i = 0; i < 512; ++i)
                    {
                        const float x = 0.3f * (float) std::sin (ph); ph += juce::MathConstants<double>::twoPi * 1414.0 / rate;
                        buf.setSample (0, i, x); buf.setSample (1, i, sideGain * x);
                    }
                    proc.processBlock (buf, midi);
                    if (b % 3 == 0) hs->tickForSnapshot();
                }
            };
            feed (-0.2f, 200);   // L = x, R = -0.2x: M 0.4, S 0.6 -> fill 1.385 -> limit sqrt(0.615/1.385) = 66.6 %
            const auto& bv = proc.bandView (4);
            check (bv.learned.load() && std::abs (bv.limit.load() - 0.666f) < 0.03f, "1k-2k learned limit ~66.6 %", bv.limit.load() * 100.0f);

            auto& f = *hs->controlNamed ("field");
            const float x0 = 62.0f, x1 = (float) f.getWidth() - 80.0f, fc = (x0 + x1) * 0.5f, half = (x1 - x0) * 0.5f, y0 = 32.0f,
                        rh = ((float) f.getHeight() - 40.0f) / 8.0f;
            const int row = 3; const float yc = y0 + row * rh + rh * 0.5f;   // row 3 = 1k-2k (band5)
            auto gx = [&] (float pctv) { return fc + half * pctv / 200.0f; };
            drag (f, { gx (100), yc }, { gx (150), yc });
            check (get ("band5") == 150.0f, "drag the right grip 100 -> 150 %", get ("band5"));
            drag (f, { fc - half * 0.75f, yc }, { gx (-120) , yc });   // left grip, dragged outward on the left side
            check (get ("band5") == 120.0f, "the left grip works mirrored (150 -> 120 %)", get ("band5"));
            drag (f, { gx (120), yc }, { gx (70), yc });
            check (std::abs (get ("band5") - 67.0f) < 0.6f, "magnet: 70 % snaps to the gold limit (67)", get ("band5"));
            drag (f, { gx (67), yc }, { gx (76), yc });
            check (get ("band5") == 76.0f, "outside 4 % no magnet (76)", get ("band5"));
            const float before = get ("band5");
            drag (f, { gx (30), yc }, { gx (160), yc });   // not on a grip
            check (get ("band5") == before, "dragging empty field space moves nothing", get ("band5"));
            f.mouseMove (ev (f, { gx (30), yc }, { gx (30), yc }, 0, false));
            const auto sum = dynamic_cast<hsui::HasSummary&> (f).summary();
            check (sum.startsWith ("1k-2k") && sum.contains ("limit 67"), "hover over the 1k row names the band and its limit", sum.length());
            std::printf ("       summary: %s\n", sum.toRawUTF8());
            click (f, { gx (76), yc }, 2);
            check (get ("band5") == 100.0f, "double-click the grip = loaded value (Default 100)", get ("band5"));
            // click off a grip forgets the learned peak: after the click, a narrower source shows a higher limit
            // the source turns narrower; the held limit stays until the click (the 300 ms window must hold only the new source)
            feed (0.2f, 300);   // 3.2 s: the 300 ms one-pole window forgets the old source (0.64 s still held 12 %); L = x, R = 0.2x: M 0.6, S 0.4 -> fill 0.615 -> limit sqrt(1.385/0.615) = 150 %
            check (bv.limit.load() < 0.7f, "without a click the learned limit is held (67)", bv.limit.load() * 100.0f);
            click (f, { gx (30), yc });
            feed (0.2f, 100);
            check (std::abs (bv.limit.load() - 1.5f) < 0.05f, "click on the field relearns (limit 67 -> 150 %)", bv.limit.load() * 100.0f);
            // other bands unaffected by drags on row 3
            check (get ("band4") == 100.0f && get ("band6") == 100.0f, "neighbour bands untouched", get ("band6"));
        }

        // ---- saving several presets in a row (owner 21 Nov: the second one would not save). No dialog is opened here.
        {
            auto& bar = *dynamic_cast<hsui::PresetBar*> (hs->controlNamed ("preset"));
            auto& pm = proc.presets;
            bar.askReplace = false;
            const int before = pm.userNames().size();
            pm.loadDefault();
            const auto n1 = pm.suggestName(); set ("width", 130.0f); bar.commitSave (n1);
            const auto n2 = pm.suggestName(); set ("width", 160.0f); bar.commitSave (n2);
            const auto n3 = pm.suggestName(); set ("width", 90.0f);  bar.commitSave (n3);
            check (n1 != n2 && n2 != n3 && pm.userNames().size() == before + 3, "three saves in a row = three new presets", pm.userNames().size() - before);
            check (pm.load (n2) && get ("width") == 160.0f, "the second one holds its own values (160)", get ("width"));
            check (pm.currentName() == n2 && pm.suggestName() != n2, "after a save the dialog offers a new free name", 1);
            check (pm.nameProblem ("").isNotEmpty() && pm.nameProblem ("Default").isNotEmpty() && pm.nameProblem ("a/b").isNotEmpty()
                   && pm.nameProblem (juce::String::fromUTF8 ("lhbl\xc4\xb1")).isEmpty(), "bad names get a reason; Turkish letters are fine", 1);
            set ("width", 111.0f); bar.commitSave (n2);   // replace (the second question is answered yes)
            check (pm.load (n2) && get ("width") == 111.0f && pm.userNames().size() == before + 3, "replace keeps one file with the new values", get ("width"));
        }

        // ---- the icon link and the mark
        check (juce::String (HighStereoAudioProcessorEditor::kManualUrl) == "https://subverselab.com/plugins/high-stereo/manual", "icon links to the manual", 1);
        ed.reset();
    }
    dir.deleteRecursively();
    std::printf ("\n%d/%d checks passed\n", checks - fails, checks);
    return fails == 0 ? 0 : 1;
}
