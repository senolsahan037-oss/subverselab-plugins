/*  Draws the P4b panel into PNGs without a screen, after real programme has gone through the real processor, so the meters, the movement display and the
    windows show what the engine measured (nothing is faked).

        ui_snapshot [outdir]        (default docs/ui, relative to the project)

    17 Oct 2026 (RUNAWAY / CHAOS): files are prefixed p5_ (p5_seda_default ...), and p5_seda_runaway: RUNAWAY 70 %, CHAOS 30 %, SYNC 1/8, the loop over 1.0 (LED lit).
      p4b_seda_default.png      : the factory settings after 4 s of the synth vocal (LISTEN off), at 1x; p4b_seda_default_2x.png at 2x
      p4b_seda_listen_duck.png  : LISTEN on, SOURCE vocal, stopped on a block where the duck is in (moving); 1x and _2x
      p4b_seda_sync_auto.png    : LISTEN on, SYNC, AUTO note on a busy pluck train (the chosen note in the TIME window); 1x and _2x
    Also checks that the panel says what the engine did and that Cmd+Z undoes a click. Exit code 0 only if all held.
*/
#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"
#include <cmath>
#include <set>
#include <cstdio>
#include <functional>

namespace
{
using Audio = std::vector<std::vector<float>>;
int gFail = 0;
void check (bool ok, const juce::String& what) { std::printf ("[%s] %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8()); if (! ok) ++gFail; }

void setParam (juce::AudioProcessorValueTreeState& s, const char* id, float v)
{
    auto* p = s.getParameter (id);
    p->setValueNotifyingHost (p->convertTo0to1 (v));
}

Audio vocal (double sr, double seconds)
{
    Audio a (2, std::vector<float> ((size_t) (seconds * sr), 0.0f));
    double t0 = 0.2; int note = 0;
    while (t0 < seconds - 0.4)
    {
        for (int w = 0; w < 3; ++w)
        {
            const double f0 = 220.0 * std::pow (2.0, (double) ((note++ * 5) % 12) / 12.0), len = 0.30;
            for (int i = 0; i < (int) (len * sr); ++i)
            {
                const int k = (int) (t0 * sr) + i; if (k >= (int) a[0].size()) break;
                const double t = (double) i / sr, env = std::min (1.0, t / 0.02) * std::min (1.0, (len - t) / 0.04);
                double v = 0.0; for (int h = 1; h <= 4; ++h) v += std::sin (2.0 * M_PI * f0 * h * t) / h;
                a[0][(size_t) k] += (float) (0.06 * env * v); a[1][(size_t) k] += (float) (0.05 * env * v);
            }
            t0 += len + 0.05;
        }
        t0 += 1.3;
    }
    return a;
}

Audio plucks (double sr, double seconds, double rate)
{
    Audio a (2, std::vector<float> ((size_t) (seconds * sr), 0.0f));
    for (double t0 = 0.1; t0 < seconds - 0.2; t0 += 1.0 / rate)
        for (int i = 0; i < (int) (0.12 * sr); ++i)
        {
            const int k = (int) (t0 * sr) + i; if (k >= (int) a[0].size()) break;
            const double t = (double) i / sr, v = 0.2 * std::exp (-t * 30.0) * std::sin (2.0 * M_PI * 440.0 * t);
            a[0][(size_t) k] += (float) v; a[1][(size_t) k] += (float) (0.8 * v);
        }
    return a;
}

// the programme goes through the real processor in 512-sample blocks; the editor's tick runs every 1600 samples. `watch` sees every block.
struct Seen { bool duck = false, thr = false, guard = false; float deepestDuck = 0.0f, maxIn = -90.0f, maxOut = -90.0f; std::set<int> notes; };
Seen play (SedaAudioProcessor& proc, SedaEditor& ed, const Audio& in)
{
    Seen seen;
    const int n = (int) in[0].size();
    juce::AudioBuffer<float> buf (2, 512);
    juce::MidiBuffer midi;
    int sinceTick = 0;
    for (int pos = 0; pos < n; pos += 512)
    {
        const int len = juce::jmin (512, n - pos);
        buf.setSize (2, len, false, false, true);
        for (int c = 0; c < 2; ++c) std::copy (in[(size_t) c].begin() + pos, in[(size_t) c].begin() + pos + len, buf.getWritePointer (c));
        proc.processBlock (buf, midi);
        const auto r = proc.getReadouts();
        seen.duck = seen.duck || r.duckDb < -3.0f; seen.thr = seen.thr || r.throwOn; seen.guard = seen.guard || r.guardOn;
        seen.deepestDuck = std::min (seen.deepestDuck, r.duckDb); seen.notes.insert (r.chosenNote);
        sinceTick += len;
        if (sinceTick >= 1600) { ed.tickForSnapshot(); sinceTick = 0; seen.maxIn = std::max (seen.maxIn, ed.shownEchoDb()); seen.maxOut = seen.maxIn; }
    }
    ed.tickForSnapshot();
    return seen;
}

bool save (SedaEditor& ed, const juce::File& dir, const juce::String& name)
{
    bool ok = true;
    for (float scale : { 1.0f, 2.0f })
    {
        const auto file = dir.getChildFile (name + (scale > 1.5f ? "_2x.png" : ".png"));
        file.deleteFile();
        juce::FileOutputStream stream (file);
        const auto image = ed.createComponentSnapshot (ed.getLocalBounds(), true, scale);
        ok = ok && stream.openedOk() && juce::PNGImageFormat().writeImageToStream (image, stream);
        std::printf ("  %s  %dx%d\n", file.getFullPathName().toRawUTF8(), image.getWidth(), image.getHeight());
    }
    return ok;
}

// play until the duck is deeper than `dbLimit` at the end of a block (the picture then shows the duck moving), at most to the end of `in`
bool playUntilDuck (SedaAudioProcessor& proc, SedaEditor& ed, const Audio& in, float dbLimit, int startBlock)
{
    const int n = (int) in[0].size();
    juce::AudioBuffer<float> buf (2, 512);
    juce::MidiBuffer midi;
    int sinceTick = 0, block = 0;
    for (int pos = 0; pos < n; pos += 512, ++block)
    {
        const int len = juce::jmin (512, n - pos);
        buf.setSize (2, len, false, false, true);
        for (int c = 0; c < 2; ++c) std::copy (in[(size_t) c].begin() + pos, in[(size_t) c].begin() + pos + len, buf.getWritePointer (c));
        proc.processBlock (buf, midi);
        sinceTick += len;
        if (sinceTick >= 1600) { ed.tickForSnapshot(); sinceTick = 0; }
        if (block >= startBlock && proc.getReadouts().duckDb < dbLimit) { ed.tickForSnapshot(); return true; }
    }
    return false;
}
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    const auto cwd = juce::File::getCurrentWorkingDirectory();
    const juce::File out = argc > 1 ? juce::File (argv[1]) : cwd.getChildFile ("docs/ui");
    out.createDirectory();
    const double sr = 48000.0;
    bool good = true;
    struct Head : juce::AudioPlayHead { double bpm = 120.0; juce::Optional<PositionInfo> getPosition() const override { PositionInfo i; i.setBpm (bpm); i.setIsPlaying (true); return i; } } head;

    {   // 1. the factory settings
        SedaAudioProcessor proc;
        proc.setRateAndBufferSizeDetails (sr, 512); proc.prepareToPlay (sr, 512); proc.setPlayHead (&head);
        SedaEditor ed (proc);
        const auto seen = play (proc, ed, vocal (sr, 4.0));
        check (! seen.duck && ! seen.thr && ed.shownDuckDb() == 0.0f && ! ed.throwLampLit(), "LISTEN off: the movement display shows no duck and no throw (deepest duck " + juce::String (seen.deepestDuck, 1) + " dB)");
        check (seen.maxIn > -36.0f, "the ECHO needle moved with the programme (highest shown " + juce::String (seen.maxIn, 1) + " dB)");
        good &= save (ed, out, "p5_seda_default");

        // the editor's own undo: click the SYNC key the way a hand does, then Cmd+Z on the editor
        auto* sync = ed.key ("sync");
        check (sync != nullptr, "the panel has a SYNC key");
        if (sync != nullptr)
        {
            const int steps = proc.undo.undoSteps();
            const float before = proc.apvts.getRawParameterValue ("sync")->load();
            sync->triggerClick();
            juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
            const float moved = proc.apvts.getRawParameterValue ("sync")->load();
            const int stepsAfter = proc.undo.undoSteps();
            const bool undone = ed.keyPressed (juce::KeyPress ('z', juce::ModifierKeys::commandModifier, 'z'));
            const float back = proc.apvts.getRawParameterValue ("sync")->load();
            check (before == 0.0f && moved == 1.0f && stepsAfter == steps + 1 && undone && back == 0.0f, "a click on SYNC is one undo step; Cmd+Z on the editor takes it back (steps " + juce::String (steps) + " -> " + juce::String (stepsAfter) + ", SYNC " + juce::String (before, 0) + " -> " + juce::String (moved, 0) + " -> " + juce::String (back, 0) + ")");
        }
    }
    {   // 2. LISTEN on, SOURCE vocal, the duck moving
        SedaAudioProcessor proc;
        proc.setRateAndBufferSizeDetails (sr, 512); proc.prepareToPlay (sr, 512); proc.setPlayHead (&head);
        setParam (proc.apvts, "listen", 1.0f); proc.applyProgram (1); setParam (proc.apvts, "feedback", 50.0f); setParam (proc.apvts, "mix", 40.0f);
        SedaEditor ed (proc);
        const bool got = playUntilDuck (proc, ed, vocal (sr, 14.0), -5.0f, 800);
        const float shown = ed.shownDuckDb(), engine = proc.getReadouts().duckDb;
        check (got && shown < -5.0f && std::abs (shown - engine) < 1.0e-6f, "LISTEN on a vocal: the analog duck bar shows the engine's number (panel " + juce::String (shown, 2) + " dB, engine " + juce::String (engine, 2) + " dB)");
        check (ed.sourceIndex() == 1 && ed.programIndex() == 1 && ed.timeWindowText() == "1/4d", "the LED program name shows P1 VOCAL and the red 7-segment time shows the note in force, 1/4d (" + ed.timeWindowText() + ")");
        good &= save (ed, out, "p5_seda_listen_duck");
    }
    {   // 3. SYNC + AUTO note on a busy pluck train
        SedaAudioProcessor proc;
        proc.setRateAndBufferSizeDetails (sr, 512); proc.prepareToPlay (sr, 512); head.bpm = 120.0; proc.setPlayHead (&head);
        setParam (proc.apvts, "listen", 1.0f); setParam (proc.apvts, "sync", 1.0f); setParam (proc.apvts, "noteAuto", 1.0f); setParam (proc.apvts, "feedback", 40.0f); setParam (proc.apvts, "mix", 40.0f);
        SedaEditor ed (proc);
        const auto seen = play (proc, ed, plucks (sr, 8.0, 8.0));
        check (seen.notes.count (6) == 1 && ed.timeWindowText() == "1/8" && ed.shownNoteText() == "1/8" && ed.key ("noteAuto")->isLit(), "AUTO note on 8 plucks/s: the TIME window shows 1/8 (" + ed.timeWindowText() + "), AUTO is lit");
        good &= save (ed, out, "p5_seda_sync_auto");
    }
    {   // 4. a mono output bus (tester finding T1): PING-PONG selected, WIDE / PING-PONG keys and the WIDTH window read as MONO BUS; nothing moves
        SedaAudioProcessor proc;
        auto layout = proc.getBusesLayout();
        layout.inputBuses.set (0, juce::AudioChannelSet::mono()); layout.outputBuses.set (0, juce::AudioChannelSet::mono());
        check (proc.setBusesLayout (layout), "the processor accepts a mono / mono layout");
        proc.setRateAndBufferSizeDetails (sr, 512); proc.prepareToPlay (sr, 512); proc.setPlayHead (&head);
        setParam (proc.apvts, "stereo", 2.0f); setParam (proc.apvts, "feedback", 50.0f); setParam (proc.apvts, "mix", 60.0f);
        SedaEditor ed (proc);
        auto in = vocal (sr, 4.0); in.resize (1);
        {   // a mono buffer through the processor, the editor ticking
            juce::AudioBuffer<float> buf (1, 512); juce::MidiBuffer midi; int sinceTick = 0;
            for (int pos = 0; pos < (int) in[0].size(); pos += 512)
            {
                const int len = juce::jmin (512, (int) in[0].size() - pos);
                buf.setSize (1, len, false, false, true);
                std::copy (in[0].begin() + pos, in[0].begin() + pos + len, buf.getWritePointer (0));
                proc.processBlock (buf, midi);
                sinceTick += len; if (sinceTick >= 1600) { ed.tickForSnapshot(); sinceTick = 0; }
            }
            ed.tickForSnapshot();
        }
        check (ed.monoBus() && proc.apvts.getRawParameterValue ("stereo")->load() == 2.0f, "mono bus: the editor knows it, and the STEREO parameter is still PING-PONG (state untouched)");
        good &= save (ed, out, "p5_seda_mono_bus");
    }
    {   // 5. RUNAWAY 70 %, CHAOS 30 %, SYNC 1/8 at 120 bpm: the loop runs away, the red RUNAWAY LED under the LED display is lit
        SedaAudioProcessor proc;
        proc.setRateAndBufferSizeDetails (sr, 512); proc.prepareToPlay (sr, 512); head.bpm = 120.0; proc.setPlayHead (&head);
        setParam (proc.apvts, "runaway", 70.0f); setParam (proc.apvts, "chaos", 30.0f); setParam (proc.apvts, "sync", 1.0f); setParam (proc.apvts, "note", 6.0f); setParam (proc.apvts, "mix", 50.0f);
        SedaEditor ed (proc);
        play (proc, ed, plucks (sr, 6.0, 2.0));
        const float g = proc.getReadouts().loopGain;
        check (g > 1.0f && ed.runawayLampLit(), "RUNAWAY 70 % (FEEDBACK 30 %): loop gain " + juce::String (g, 3) + " > 1, the RUNAWAY LED is lit");
        good &= save (ed, out, "p5_seda_runaway");
        setParam (proc.apvts, "runaway", 0.0f); setParam (proc.apvts, "feedback", 30.0f);
        play (proc, ed, plucks (sr, 1.0, 2.0));
        check (! ed.runawayLampLit(), "RUNAWAY back to 0: the LED goes out (loop gain " + juce::String (proc.getReadouts().loopGain, 3) + ")");
    }
    check (good, "ten PNGs written (five panels, 1x and 2x)");
    std::printf ("\nui_snapshot: %s\n", gFail == 0 ? "all passed" : "FAILED");
    return gFail == 0 ? 0 : 1;
}
