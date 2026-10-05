/*  hostsim: long host-like run for the "plugin dead after a MIX move" bug.

        hostsim [moves] [seed] [scenario]
            scenario 0  the owner's order: MIX twice, then TIME / FEEDBACK / TONE ...
            scenario 1  1000 random moves of every parameter (message thread)
            scenario 2  as 1, with a second thread writing automation concurrently
        Prints the probe after the run and every 100 moves. Exit 0 if the
        output still follows MIX at the end. */
#include "host_sim_native.h"
#include "../Source/PluginEditor.h"

using namespace hostsim;

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    const int moves = argc > 1 ? juce::String (argv[1]).getIntValue() : 1000;
    const int seed = argc > 2 ? juce::String (argv[2]).getIntValue() : 1;
    const int scenario = argc > 3 ? juce::String (argv[3]).getIntValue() : 1;

    YankiProcessor proc;
    std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
    NativeParams np (proc);
    const double hostSr = getenv ("HOSTSIM_SR") ? atof (getenv ("HOSTSIM_SR")) : 48000.0;
    const int hostBlock = getenv ("HOSTSIM_BLOCK") ? atoi (getenv ("HOSTSIM_BLOCK")) : (scenario == 6 ? 128 : 512);
    Rig rig (proc, np, hostSr, hostBlock, scenario != 6);
    rig.start();
    rig.waitSamples (48000);

    std::atomic<bool> autoPause { false };
    auto show = [&] (const char* tag)
    {
        autoPause = true;
        std::this_thread::sleep_for (std::chrono::milliseconds (20));
        auto r = probe (rig);
        autoPause = false;
        std::cout << tag << ": diff@MIX0 " << juce::String (r.diffDbAt0, 1) << " dB, diff@MIX0.3 " << juce::String (r.diffDbAtMix, 1)
                  << " dB, rise " << juce::String (r.riseDb(), 1) << " dB -> " << (r.follows() ? "FOLLOWS" : "DEAD") << std::endl;
        if (! r.follows())
        {
            std::cout << "    state:";
            for (auto* prm : proc.getParameters())
                if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (prm))
                    std::cout << " " << rp->paramID << "=" << juce::String (rp->convertFrom0to1 (rp->getValue()), 3);
            std::cout << "\n    uiEndLatched " << (int) proc.uiEndLatched.load() << " uiFrozen " << (int) proc.uiFrozen.load()
                      << " uiFeedbackNatural " << (int) proc.uiFeedbackNatural.load() << std::endl;
        }
        return r.follows();
    };
    bool ok = show ("start");
    juce::Random rnd (seed);

    if (scenario == 11)
    {
        // END CUT pressed on continuous program (no 1 s gap), then HOLD pressed: does the plug-in come back?
        auto probeNow = [&] (const char* tag)
        {
            auto r = probe (rig, false);
            std::cout << tag << ": diff@MIX0 " << juce::String (r.diffDbAt0, 1) << " dB, diff@MIX0.3 " << juce::String (r.diffDbAtMix, 1)
                      << " dB -> " << (r.follows() ? "FOLLOWS" : "DEAD") << std::endl;
            return r.follows();
        };
        move (rig, ids::endMode, 1.0f / 2.0f * 0.0f + norm (rig, ids::endMode, 1.0f));
        move (rig, ids::endTrigger, 1.0f);
        rig.waitSamples (4000);
        move (rig, ids::endTrigger, 0.0f);
        ok = probeNow ("CUT pressed, mode still CUT") && ok;
        move (rig, ids::endMode, 0.0f);   // HOLD key
        ok = probeNow ("HOLD pressed, continuous program") && ok;
    }
    else if (scenario == 10)
    {
        // A host that reports a bad tempo for a moment (0, NaN, inf, negative, huge) must not leave the plug-in dead.
        for (double bad : { 0.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), -120.0, 1.0e9 })
        {
            rig.host.bpm = bad;
            rig.waitSamples (48000);
            rig.host.bpm = 120.0;
            std::cout << "  after bpm " << bad << ": ";
            ok = show (("bpm " + juce::String (bad)).toRawUTF8()) && ok;
        }
    }
    else if (scenario == 9)
    {
        // Which component answers a click on the face? Scan the whole face in 10-unit steps and print what sits where a knob is.
        editor->setSize (1280, 512);
        editor->setVisible (true);
        const float sc = 1280.0f / 1600.0f;
        std::map<juce::String, int> counts;
        for (int y = 0; y < 640; y += 4)
        {
            juce::String line;
            for (int x = 0; x < 1600; x += 8)
            {
                auto* c = editor->getComponentAt ((int) (x * sc), (int) (y * sc));
                char ch = '.';
                if (dynamic_cast<ParamKnob*> (c)) ch = 'K';
                else if (dynamic_cast<CapKnob*> (c)) ch = 'C';
                else if (dynamic_cast<Key*> (c)) ch = 'k';
                else if (dynamic_cast<BypassToggle*> (c)) ch = 'B';
                else if (dynamic_cast<TapeDisplay*> (c)) ch = 'D';
                else if (dynamic_cast<Lamp*> (c)) ch = 'L';
                line << ch;
            }
            std::cout << line << "\n";
        }
        std::cout << std::flush;
    }
    else if (scenario == 8)
    {
        // Host lifecycle: state save / restore (undo, project save), prepareToPlay again (audio restart), a MIX move between.
        for (int round = 0; round < moves; ++round)
        {
            move (rig, ids::mix, rnd.nextFloat());
            move (rig, ids::timeMs, rnd.nextFloat());
            move (rig, ids::feedback, rnd.nextFloat() * 0.6f);
            rig.waitSamples (30000);
            juce::MemoryBlock state;
            proc.getStateInformation (state);
            move (rig, ids::mix, rnd.nextFloat());
            proc.setStateInformation (state.getData(), (int) state.getSize());     // undo
            juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
            if (round % 2 == 1)
            {
                rig.stop();
                proc.releaseResources();
                proc.prepareToPlay (48000.0, 128);                                   // audio engine restart
                rig.start();
            }
            ok = show (("round " + juce::String (round)).toRawUTF8()) && ok;
        }
    }
    else if (scenario == 7)
    {
        // Hostile inputs for 6 s each, then normal noise again: must still follow MIX.
        const char* names[] { "noise", "hot clipped noise", "DC 0.5 + noise", "full-scale impulses", "100 Hz full-scale square", "1e-19 noise", "one NaN sample", "one +inf sample", "noise x25 (+-4)" };
        for (int kind = 1; kind <= 8; ++kind)
        {
            rig.inputKind = kind;
            rig.waitSamples (48000 * 6);
            rig.inputKind = 0;
            std::cout << "  after " << names[kind] << ": ";
            ok = show (names[kind]) && ok;
        }
    }
    else if (scenario == 6)
    {
        // Long run at 128-sample blocks (the owner's Live buffer), not paced: `moves` x 60 s of audio, a MIX move every 20 s.
        for (int i = 0; i < moves; ++i)
        {
            for (int k = 0; k < 3; ++k)
            {
                move (rig, ids::mix, rnd.nextFloat());
                rig.waitSamples (48000 * 20);
            }
            if (i % 5 == 4) ok = show (("after " + juce::String ((i + 1) * 60) + " s of audio").toRawUTF8()) && ok;
        }
    }
    else if (scenario == 5)
    {
        // CPU per 128-sample block at 48 kHz (the owner's Live buffer): steady, gliding TIME, tempo that jitters every block.
        rig.stop();
        juce::AudioBuffer<float> buf (2, 128);
        juce::MidiBuffer midi;
        juce::Random r (5);
        auto bench = [&] (const char* name, std::function<void (int)> perBlock)
        {
            const int blocks = 4000;
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            double worst = 0;
            for (int b = 0; b < blocks; ++b)
            {
                perBlock (b);
                for (int c = 0; c < 2; ++c) for (int i = 0; i < 128; ++i) buf.setSample (c, i, 0.1f * (r.nextFloat() - 0.5f));
                const auto a = juce::Time::getMillisecondCounterHiRes();
                proc.processBlock (buf, midi);
                worst = std::max (worst, juce::Time::getMillisecondCounterHiRes() - a);
            }
            const double per = (juce::Time::getMillisecondCounterHiRes() - t0) / blocks;
            std::cout << name << ": avg " << juce::String (per * 1000.0, 1) << " us/block (budget 2667), worst " << juce::String (worst * 1000.0, 1) << " us" << std::endl;
        };
        rig.host.bpm = 120.0;
        proc.setPlayHead (&rig.host);
        bench ("steady", [] (int) {});
        bench ("tempo jitter every block", [&] (int b) { rig.host.bpm = 120.0 + 0.001 * (b % 7); });
        bench ("sweeping TIME FREE", [&] (int b) { move (rig, ids::timeSync, 1.0f); move (rig, ids::timeMs, 0.5f + 0.4f * std::sin ((float) b * 0.01f)); });
        proc.setPlayHead (nullptr);
    }
    else if (scenario == 4)
    {
        // Paint cost: the whole editor at 2x (a Retina window), as the 30 Hz timer makes it repaint the display.
        editor->setSize (1280, 512);
        for (int pass = 0; pass < 3; ++pass)
        {
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            for (int i = 0; i < 10; ++i)
                editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
            std::cout << "full editor snapshot @2x: " << juce::String ((juce::Time::getMillisecondCounterHiRes() - t0) / 10.0, 2) << " ms/frame" << std::endl;
        }
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        for (int i = 0; i < 100; ++i)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (1);
        std::cout << "100 x 1 ms dispatch with editor timer running: " << juce::String (juce::Time::getMillisecondCounterHiRes() - t0, 1) << " ms" << std::endl;
    }
    else if (scenario == 3)
    {
        // The editor's own MIX knob driven with real mouse events (down, drags, up, double click, wheel).
        std::vector<KnobBase*> knobs;
        std::function<void (juce::Component&)> walk = [&] (juce::Component& c)
        {
            if (auto* k = dynamic_cast<KnobBase*> (&c)) knobs.push_back (k);
            for (auto* ch : c.getChildren()) walk (*ch);
        };
        walk (*editor);
        KnobBase* mixKnob = nullptr;
        for (auto* k : knobs)
            if (std::abs (k->getBounds().toFloat().getCentreX() - 1350.0f) < 3.0f) mixKnob = k;
        if (mixKnob == nullptr) { std::cout << "MIX knob not found (" << knobs.size() << " knobs)" << std::endl; return 2; }
        auto src = juce::Desktop::getInstance().getMainMouseSource();
        auto ev = [&] (juce::Point<float> pos, int clicks, bool dragged)
        {
            return juce::MouseEvent (src, pos, juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, mixKnob, mixKnob,
                                     juce::Time::getCurrentTime(), juce::Point<float> (30.0f, 40.0f), juce::Time::getCurrentTime(), clicks, dragged);
        };
        auto param = proc.apvts.getParameter (ids::mix);
        for (int round = 0; round < moves; ++round)
        {
            const float startY = 40.0f;
            mixKnob->mouseDown (ev ({ 30.0f, startY }, 1, false));
            const float travel = (round % 2 == 0 ? -1.0f : 1.0f) * (30.0f + 10.0f * (float) (round % 7));
            for (int i = 1; i <= 10; ++i)
            {
                mixKnob->mouseDrag (ev ({ 30.0f, startY + travel * (float) i / 10.0f }, 1, true));
                rig.waitSamples (500);
            }
            mixKnob->mouseUp (ev ({ 30.0f, startY + travel }, 1, true));
            if (round % 5 == 4) { mixKnob->mouseDoubleClick (ev ({ 30.0f, startY }, 2, false)); }
            std::cout << "  drag " << round << ": MIX " << juce::String (param->getValue(), 3) << std::endl;
            ok = show (("after mouse drag " + juce::String (round + 1)).toRawUTF8()) && ok;
        }
    }
    else if (scenario == 0)
    {
        for (float v : { 0.6f, 0.2f })
        {
            move (rig, ids::mix, v);
            rig.waitSamples (20000);
        }
        ok = show ("after 2 MIX moves") && ok;
        move (rig, ids::timeSync, norm (rig, ids::timeSync, 6.0f));
        move (rig, ids::timeMs, norm (rig, ids::timeMs, 400.0f));
        move (rig, ids::feedback, 0.6f);
        move (rig, ids::tone, norm (rig, ids::tone, 2500.0f));
        rig.waitSamples (48000);
        ok = show ("after TIME/FEEDBACK/TONE") && ok;
    }
    else
    {
        std::atomic<bool> autoRun { scenario == 2 };
        std::thread automation ([&]
        {
            juce::Random r2 (seed + 99);
            while (autoRun.load())
            {
                if (autoPause.load()) { std::this_thread::sleep_for (std::chrono::milliseconds (2)); continue; }
                static const char* ids[] { ids::mix, ids::tone, ids::wear, ids::spread, ids::feedback, ids::timeMs };
                auto* prm = proc.apvts.getParameter (ids[r2.nextInt (6)]);
                prm->setValueNotifyingHost (r2.nextFloat());
                std::this_thread::sleep_for (std::chrono::milliseconds (3));
            }
        });
        static const char* all[] { ids::mood, ids::timeSync, ids::timeMs, ids::feedback, ids::capOn, ids::capDb,
                                   ids::tone, ids::wear, ids::mix, ids::output, ids::endMode,
                                   ids::freeze, ids::darken, ids::riseCurve, ids::lockBars, ids::fineMs, ids::spread };
        for (int i = 0; i < moves; ++i)
        {
            const char* id = all[rnd.nextInt ((int) (sizeof (all) / sizeof (all[0])))];
            if (auto* only = getenv ("HOSTSIM_ONLY"))
                if (! (juce::String (",") + juce::String (only) + juce::String (",")).contains ("," + juce::String (id) + ",")) continue;
            if (getenv ("HOSTSIM_NO_END") && (juce::String (id) == ids::endMode)) continue;
            move (rig, id, rnd.nextFloat());
            if (rnd.nextInt (4) == 0) rig.waitSamples (rnd.nextInt (6000));
            else juce::MessageManager::getInstance()->runDispatchLoopUntil (1);
            if ((i + 1) % 100 == 0)
                ok = show (("after " + juce::String (i + 1) + " moves").toRawUTF8()) && ok;
        }
        autoRun = false;
        automation.join();
        ok = show ("end") && ok;
    }
    rig.stop();
    editor.reset();
    std::cout << (ok ? "OK" : "FAILED: the output stopped following MIX") << std::endl;
    return ok ? 0 : 1;
}
