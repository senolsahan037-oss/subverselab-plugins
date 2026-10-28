// plugin_check: the real TORNA processor and editor against the engine and the SPEC (§4 controls, §6 bus gates).
// One line per check, [PASS] / [FAIL]; exit code 1 on any failure.
#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"

namespace
{
int fails = 0;
void check (bool ok, const juce::String& what) { std::printf ("[%s] %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8()); if (! ok) ++fails; }

void setP (TornaAudioProcessor& p, const juce::String& id, float plain)
{
    if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (p.apvts.getParameter (id))) r->setValueNotifyingHost (r->convertTo0to1 (plain));
}
float getP (TornaAudioProcessor& p, const juce::String& id) { return p.apvts.getRawParameterValue (id)->load(); }

std::vector<float> sine (double sr, double f, double amp, int n)
{
    std::vector<float> x ((size_t) n);
    for (int i = 0; i < n; ++i) x[(size_t) i] = (float) (amp * std::sin (2.0 * juce::MathConstants<double>::pi * f * i / sr));
    return x;
}
std::vector<float> noise (int n, int seed)
{
    juce::Random r (seed); std::vector<float> x ((size_t) n);
    for (auto& v : x) v = (r.nextFloat() * 2.0f - 1.0f) * 0.2f;
    return x;
}

/** runs a stereo signal through the processor in blocks; `each` is called before every block with the sample index */
std::array<std::vector<float>, 2> run (TornaAudioProcessor& p, const std::vector<float>& l, const std::vector<float>& r, int block,
                                       std::function<void (int)> each = {}, bool hostBypass = false)
{
    const int n = (int) l.size();
    std::array<std::vector<float>, 2> out { std::vector<float> ((size_t) n), std::vector<float> ((size_t) n) };
    juce::AudioBuffer<float> b (2, block); juce::MidiBuffer m;
    for (int s = 0; s < n; s += block)
    {
        const int len = juce::jmin (block, n - s);
        if (each) each (s);
        b.setSize (2, len, false, false, true);
        for (int i = 0; i < len; ++i) { b.setSample (0, i, l[(size_t) (s + i)]); b.setSample (1, i, r[(size_t) (s + i)]); }
        if (hostBypass) p.processBlockBypassed (b, m); else p.processBlock (b, m);
        for (int i = 0; i < len; ++i) { out[0][(size_t) (s + i)] = b.getSample (0, i); out[1][(size_t) (s + i)] = b.getSample (1, i); }
    }
    return out;
}

std::unique_ptr<TornaAudioProcessor> fresh (double sr = 48000.0, int block = 512)
{
    auto p = std::make_unique<TornaAudioProcessor>();
    p->setPlayConfigDetails (2, 2, sr, block);
    p->prepareToPlay (sr, block);
    return p;
}

/** energy above 4 kHz in 30 ms around sample `at` against the steady level (the bus gates' click measure, B5) */
double clickDbc (const std::vector<float>& y, double sr, int at)
{
    juce::dsp::IIR::Filter<float> hp[4];
    for (auto& f : hp) f.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighPass (sr, 4000.0);
    std::vector<float> h (y.size());
    for (size_t i = 0; i < y.size(); ++i) { float v = y[i]; for (auto& f : hp) v = f.processSample (v); h[i] = v; }
    double e = 0, ref = 0; int ne = 0, nr = 0;
    for (int i = at - (int) (0.015 * sr); i < at + (int) (0.015 * sr); ++i) { e += (double) h[(size_t) i] * h[(size_t) i]; ++ne; }
    for (int i = (int) (0.4 * sr); i < (int) (0.8 * sr); ++i) { ref += (double) y[(size_t) i] * y[(size_t) i]; ++nr; }
    return 10.0 * std::log10 ((e / ne) / (ref / nr) + 1e-30);
}
}

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    const double sr = 48000.0;
    const float smile[12] = { 3, 4.5f, 3, 1, -1, -2, -2.5f, -1, 1, 3, 4.5f, 3.5f };

    // ---- parameters and latency
    {
        auto p = fresh();
        check (p->getParameters().size() == 36, "36 parameters (24 sliders, POWER, EQ IN, ATT, NOR/REV, LINK, INPUT, OUTPUT, LEVEL, LEFT, RIGHT, ANALYZER, LINE LEVEL), found " + juce::String (p->getParameters().size()));
        bool ids = true;
        for (int c = 0; c < 2; ++c) for (int b = 0; b < 12; ++b) ids &= p->apvts.getParameter (torna::sliderId (c, b)) != nullptr;
        check (ids, "slider IDs l00..l11, r00..r11 exist");
        for (double rate : { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 }) { p->prepareToPlay (rate, 512); check (p->getLatencySamples() == 64, "latency 64 samples at " + juce::String (rate, 0) + " Hz"); }
    }

    // ---- the processor plays the engine, bit for bit
    {
        auto p = fresh();
        for (int b = 0; b < 12; ++b) { setP (*p, torna::sliderId (0, b), smile[b]); setP (*p, torna::sliderId (1, b), -smile[b]); }
        p->prepareToPlay (sr, 512);
        const auto xl = noise (48000, 1), xr = noise (48000, 2);
        const auto y = run (*p, xl, xr, 512);
        std::array<ge::Channel, 2> e;
        std::array<double, 12> ml {}, mr {};
        for (int b = 0; b < 12; ++b) { ml[(size_t) b] = getP (*p, torna::sliderId (0, b)); mr[(size_t) b] = getP (*p, torna::sliderId (1, b)); }
        e[0].setMarks (ml); e[1].setMarks (mr); e[0].prepare (sr); e[1].prepare (sr);
        juce::ScopedNoDenormals nd;   // the processor runs without denormals; so must the reference
        double d = 0;
        for (size_t i = 0; i < xl.size(); ++i) d = std::max ({ d, (double) std::abs (y[0][i] - e[0].process (xl[i])), (double) std::abs (y[1][i] - e[1].process (xr[i])) });
        check (d == 0.0, "processor output equals the raw engine, L smile / R reversed marks: max difference " + juce::String (d, 10));
    }

    // ---- block size and determinism
    {
        const auto xl = noise (30000, 3), xr = noise (30000, 4);
        std::vector<float> ref;
        bool same = true;
        for (int block : { 1, 7, 64, 512, 4096 })
        {
            auto p = fresh (sr, block);
            for (int b = 0; b < 12; ++b) setP (*p, torna::sliderId (0, b), smile[b]);
            p->prepareToPlay (sr, block);
            const auto y = run (*p, xl, xr, block);
            if (ref.empty()) ref = y[0]; else same &= y[0] == ref;
        }
        check (same, "identical output for block sizes 1, 7, 64, 512, 4096");
    }

    // ---- POWER, host bypass: delayed dry, no click
    {
        auto p = fresh();
        for (int b = 0; b < 12; ++b) setP (*p, torna::sliderId (0, b), smile[b]);
        p->prepareToPlay (sr, 512);
        const auto x = sine (sr, 1000.0, 0.5, 96000);
        const auto y = run (*p, x, x, 512, [&] (int s) { if (s >= 48000) setP (*p, "power", 0.0f); });
        double d = 0;
        for (int i = 60000; i < 96000; ++i) d = std::max (d, (double) std::abs (y[0][(size_t) i] - x[(size_t) (i - 64)]));
        check (d == 0.0, "POWER off: the output is the input delayed by 64 samples, bit for bit (max " + juce::String (d, 9) + ")");
        const double c = clickDbc (y[0], sr, 48000 + 64);
        check (c <= -60.0, "POWER off during a 1 kHz sine: no click (" + juce::String (c, 1) + " dBc above 4 kHz, <= -60)");
        auto q = fresh();
        for (int b = 0; b < 12; ++b) setP (*q, torna::sliderId (0, b), smile[b]);
        q->prepareToPlay (sr, 512);
        const auto z = run (*q, x, x, 512, {}, true);
        double dz = 0;
        for (int i = 2000; i < 96000; ++i) dz = std::max (dz, (double) std::abs (z[0][(size_t) i] - x[(size_t) (i - 64)]));
        check (dz == 0.0, "host bypass: the input delayed by 64 samples, bit for bit");
    }

    // ---- EQ IN/OUT, NOR/REV, ATT through the processor: no clicks (bus gate B5 on the plug-in)
    for (const char* id : { "eqIn", "rev", "att" })
    {
        auto p = fresh();
        for (int b = 0; b < 12; ++b) setP (*p, torna::sliderId (0, b), smile[b]);
        p->prepareToPlay (sr, 512);
        const auto x = sine (sr, 1000.0, 0.5, 96000);
        const float to = juce::String (id) == "eqIn" ? 0.0f : 1.0f;
        const auto y = run (*p, x, x, 512, [&] (int s) { if (s == 48128) setP (*p, id, to); });
        const double c = clickDbc (y[0], sr, 48128 + 64);
        check (c <= -60.0, juce::String (id) + " switched during a 1 kHz sine: " + juce::String (c, 1) + " dBc above 4 kHz (<= -60)");
    }

    // ---- LINK, COPY, FLAT, memories, double-click target, undo
    {
        auto p = fresh();
        for (int b = 0; b < 12; ++b) { setP (*p, torna::sliderId (0, b), smile[b]); setP (*p, torna::sliderId (1, b), 0.0f); }
        p->setLink (true);
        bool rFollows = true;
        for (int b = 0; b < 12; ++b) rFollows &= std::abs (getP (*p, torna::sliderId (1, b)) - smile[b]) < 0.051f;
        check (rFollows && getP (*p, "link") > 0.5f, "LINK engage: the right bank takes the left bank's values");
        const auto x = noise (24000, 5);
        p->prepareToPlay (sr, 512);
        const auto y = run (*p, x, x, 512);
        check (y[0] == y[1], "LINK on, same input both sides: L and R outputs identical");
        p->setLink (false);
        p->undo.undo();
        check (getP (*p, "link") > 0.5f, "undo of LINK off: LINK is on again");

        p->storeMemory (0);
        p->flatAll();
        bool flat = true; for (int i = 0; i < 24; ++i) flat &= std::abs (getP (*p, torna::sliderId (i / 12, i % 12))) < 1.0e-4f;
        check (flat, "FLAT: every slider at 0 dB");
        p->recallMemory (0);
        bool back = true; for (int b = 0; b < 12; ++b) back &= std::abs (getP (*p, torna::sliderId (0, b)) - smile[b]) < 0.051f;
        check (back && p->lastMemory() == 0, "MEMORY A recall: the stored sliders come back, lamp A");
        check (std::abs (p->sliderDefault (0, 1) - 4.5f) < 0.051f, "double-click target = the memory last recalled (32 Hz: " + juce::String (p->sliderDefault (0, 1), 1) + ")");
        p->clearLastMemory();
        check (std::abs (p->sliderDefault (0, 1)) < 1.0e-4f, "double-click target without a recalled memory = 0 dB");
        p->undo.undo();
        bool undone = true; for (int i = 0; i < 24; ++i) undone &= std::abs (getP (*p, torna::sliderId (i / 12, i % 12))) < 1.0e-4f;
        check (undone, "undo of the recall: one step back to FLAT");
        for (int b = 0; b < 12; ++b) setP (*p, torna::sliderId (1, b), -3.0f);
        p->copyBank (1);
        bool copied = true; for (int b = 0; b < 12; ++b) copied &= std::abs (getP (*p, torna::sliderId (0, b)) + 3.0f) < 1.0e-4f;
        check (copied, "COPY R > L");
    }

    // ---- presets (the user folder is a temporary one: the check never writes into the owner's ~/Library)
    const auto tmpPresets = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("torna_check_presets");
    tmpPresets.deleteRecursively();
    {
        auto p = fresh();
        p->setUserPresetDirForTests (tmpPresets);
        p->applyPreset (1);
        bool ok = true;
        for (int c = 0; c < 2; ++c) for (int b = 0; b < 12; ++b) ok &= std::abs (getP (*p, torna::sliderId (c, b)) - torna::presets()[1].marks[(size_t) b]) < 0.051f;
        check (ok && p->presetIndex() == 1, "PRESET MASTER SMILE writes both banks");
        check (std::abs (p->sliderDefault (1, 10) - 4.5f) < 0.051f, "double-click target = the preset loaded (16 kHz +4.5)");
        p->stepPreset (-1); p->stepPreset (-1);
        check (p->presetIndex() == torna::kNumPresets - 1, "PRESET < wraps from FLAT to the last preset (" + juce::String (torna::presets()[(size_t) p->presetIndex()].name) + ")");
        p->undo.undo();
        check (p->presetIndex() == torna::kNumPresets - 1 && std::abs (getP (*p, torna::sliderId (0, 0))) < 1.0e-4f, "undo of a preset step restores the sliders in one step");

        // the user's own presets
        for (int b = 0; b < 12; ++b) { setP (*p, torna::sliderId (0, b), (float) b - 5.5f); setP (*p, torna::sliderId (1, b), 2.0f); }
        const auto n1 = p->saveUserPreset();
        const auto n2 = p->saveUserPreset();
        check (n1 == "USER 01" && n2 == "USER 02" && tmpPresets.getChildFile ("USER 01.tornapreset").existsAsFile(), "hold PRESET >: saved as USER 01, then USER 02 (files written)");
        check (p->presetCount() == torna::kNumPresets + 2 && p->presetIndex() == torna::kNumPresets + 1, "user presets follow the factory list; the saved one is loaded");
        p->applyPreset (0);
        auto q = fresh();
        q->setUserPresetDirForTests (tmpPresets);
        q->applyPreset (torna::kNumPresets);
        bool same = true;
        for (int b = 0; b < 12; ++b) same &= std::abs (getP (*q, torna::sliderId (0, b)) - ((float) b - 5.5f)) < 0.051f && std::abs (getP (*q, torna::sliderId (1, b)) - 2.0f) < 0.051f;
        check (same && q->presetName (torna::kNumPresets) == "USER 01", "another instance (another project) lists USER 01 and recalls both banks");
        check (std::abs (q->sliderDefault (1, 3) - 2.0f) < 0.051f, "double-click target on a user preset = its value");
        q->stepPreset (1); q->stepPreset (1);
        check (q->presetIndex() == 0, "PRESET > wraps from the last user preset to FLAT");
    }
    tmpPresets.deleteRecursively();

    // ---- LINE LEVEL (rear): linear below the rails, moves only the clip point
    {
        auto run1 = [&] (int line, double amp)
        {
            auto p = fresh();
            setP (*p, "lineLevel", (float) line);
            p->prepareToPlay (sr, 512);
            const auto x = sine (sr, 1000.0, amp, 24000);
            return run (*p, x, x, 512)[0];
        };
        const auto a = run1 (1, 0.3), b = run1 (0, 0.3), c = run1 (2, 0.3);
        double d = 0; for (size_t i = 0; i < a.size(); ++i) d = std::max ({ d, (double) std::abs (a[i] - b[i]), (double) std::abs (a[i] - c[i]) });
        check (d < 1.0e-5, "LINE LEVEL 1 / 2 / 4 V: identical output at -10 dBFS (linear region), max difference " + juce::String (d, 7));
        auto peak = [] (const std::vector<float>& y) { float m = 0; for (size_t i = 12000; i < y.size(); ++i) m = std::max (m, std::abs (y[i])); return m; };
        const float p4 = peak (run1 (2, 4.0)), p1 = peak (run1 (0, 4.0));
        check (p4 < 3.2f && p1 > 3.8f, "at +12 dBFS: 4 V clips (peak " + juce::String (p4, 2) + "), 1 V does not (peak " + juce::String (p1, 2) + ")");
    }

    // ---- state round trip
    {
        auto p = fresh();
        juce::Random r (9);
        for (auto* ap : p->getParameters()) ap->setValueNotifyingHost (r.nextFloat());
        p->storeMemory (1); p->storeMemory (2);
        p->editorScalePct = 100;
        juce::MemoryBlock mb; p->getStateInformation (mb);
        auto q = fresh();
        q->setStateInformation (mb.getData(), (int) mb.getSize());
        // a switch's state is its side of 0.5 (a host may write any normalised value into it); everything else exactly
        auto same = [] (juce::AudioProcessorParameter* a, juce::AudioProcessorParameter* b)
        {
            if (dynamic_cast<juce::AudioParameterBool*> (a) != nullptr) return (a->getValue() >= 0.5f) == (b->getValue() >= 0.5f);
            return std::abs (a->getValue() - b->getValue()) < 1.0e-6f;
        };
        bool eq = true;
        for (int i = 0; i < p->getParameters().size(); ++i) eq &= same (p->getParameters()[i], q->getParameters()[i]);
        juce::String bad;
        for (int i = 0; i < p->getParameters().size(); ++i)
            if (! same (p->getParameters()[i], q->getParameters()[i]))
                bad << p->getParameters()[i]->getName (20) << " " << juce::String (p->getParameters()[i]->getValue(), 6) << "/" << juce::String (q->getParameters()[i]->getValue(), 6) << "; ";
        check (eq, "state round trip: all 36 parameters " + bad);
        check (q->hasMemory (1) && q->hasMemory (2) && ! q->hasMemory (0) && q->lastMemory() == 2 && q->editorScalePct.load() == 100, "state round trip: memories B, C, the lamp, SIZE");
        q->recallMemory (1);
        bool memEq = true; for (int i = 0; i < 24; ++i) memEq &= std::abs (getP (*q, torna::sliderId (i / 12, i % 12)) - getP (*p, torna::sliderId (i / 12, i % 12))) < 0.051f;
        check (memEq, "a restored memory recalls the stored sliders");
    }

    // ---- mono bus
    {
        TornaAudioProcessor p;
        TornaAudioProcessor::BusesLayout mono; mono.inputBuses.add (juce::AudioChannelSet::mono()); mono.outputBuses.add (juce::AudioChannelSet::mono());
        check (p.setBusesLayout (mono), "mono -> mono layout accepted");
        p.prepareToPlay (sr, 512);
        for (int b = 0; b < 12; ++b) setP (p, torna::sliderId (0, b), smile[b]);
        juce::AudioBuffer<float> b (1, 512); juce::MidiBuffer m;
        bool finite = true;
        for (int k = 0; k < 40; ++k) { for (int i = 0; i < 512; ++i) b.setSample (0, i, std::sin ((float) (k * 512 + i) * 0.05f) * 0.3f); p.processBlock (b, m); for (int i = 0; i < 512; ++i) finite &= std::isfinite (b.getSample (0, i)); }
        check (finite, "mono bus processes (finite)");
    }

    // ---- analyser published, editor: glide, help, lockup, size
    {
        auto p = fresh();
        const auto x = sine (sr, 1032.0, 0.1, 48000);
        run (*p, x, x, 512);
        check (p->analyserSegments (6) > 0 && p->analyserSegments (0) == 0, "analyser: a 1 kHz tone lights the 1 kHz column (" + juce::String (p->analyserSegments (6)) + " segments), not 16 Hz");
        check (p->analyserSegments (12) > 0, "analyser: TOTAL column lit (" + juce::String (p->analyserSegments (12)) + ")");
        std::unique_ptr<TornaEditor> ed (dynamic_cast<TornaEditor*> (p->createEditor()));
        check (ed->getWidth() == 1200 && ed->getHeight() == 452, "default window 75 %: " + juce::String (ed->getWidth()) + " x " + juce::String (ed->getHeight()));
        ed->setScalePct (100);
        check (ed->getWidth() == 1600 && ed->getHeight() == 602, "SIZE 100 %: 1600 x 602");
        for (int b = 0; b < 12; ++b) setP (*p, torna::sliderId (0, b), 0.0f);
        for (int k = 0; k < 40; ++k) ed->timerTickForTests();
        p->storeMemory (0);
        for (int b = 0; b < 12; ++b) setP (*p, torna::sliderId (0, b), 0.0f);
        setP (*p, torna::sliderId (0, 3), 0.0f);
        p->flatAll();
        for (int k = 0; k < 40; ++k) ed->timerTickForTests();
        setP (*p, torna::sliderId (0, 3), 12.0f);   // a value the product writes (as a memory would)
        ed->timerTickForTests();
        const float after1 = ed->slider (0, 3).shown;
        for (int k = 0; k < 40; ++k) ed->timerTickForTests();
        check (after1 > 0.0f && after1 < 12.0f && ed->slider (0, 3).shown == 12.0f, "glide: after one tick the cap is between (" + juce::String (after1, 2) + "), then settles at +12");
        const auto size = ed->getBounds();
        ed->helpOpen = true; ed->repaint();
        check (ed->getBounds() == size, "help opens over the panel: window size unchanged");
        check (ed->lockupUrl().toString (false) == "https://subverselab.com", "the SubverseLab lockup links to https://subverselab.com");
        ed->showRear (true);
        check (ed->rearShown() && ! ed->slider (0, 0).isVisible() && ed->getBounds() == size, "REAR: the rear plate replaces the front, same window size");
        ed->showRear (false);
        check (ed->slider (0, 0).isVisible(), "FRONT: the sliders are back");
    }

    std::printf ("%s\n", fails == 0 ? "ALL PASS" : (juce::String (fails) + " FAIL").toRawUTF8());
    return fails == 0 ? 0 : 1;
}
