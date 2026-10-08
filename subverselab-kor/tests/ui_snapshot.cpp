/*  Draws the ALEV panel into PNGs without a screen, after real programme has gone through the real processor, so the
    ember shows the band heat the engine measured (nothing is faked), and checks the panel's controls.

        ui_snapshot [outdir] [scale]   (default docs/ui; scale > 1 also writes <name>_2x.png etc. at that pixel scale)

      alev_default.png  : LOW 30, MID 30, HIGH 20, MIX 100
      alev_hot.png      : LOW 90, MID 70, HIGH 100, MIX 100
      alev_cold.png     : all 0, MIX 0
      alev_dragging.png : defaults, MID being dragged to 62 % (the ember shows MID and the value, only the MID zone lights)
      alev_silent.png   : no audio at all, LOW 70, MID 30, HIGH 10, MIX 100 (the ember still shows the heat spread)
      alev_silent_mix40.png : the same at MIX 40 (dimmer, smaller core)
      alev_gang.png     : defaults, GANG pushed, LOW being dragged up 20 % (all three rings move, all three zones light)

    The programme is a 4 s drum-like loop generated here (decaying 55 Hz kick, noise snare, hats, peak -3 dBFS), run in
    512-sample blocks at 48 kHz; the editor's 30 Hz timer callback is called every 1600 samples (deterministic clock).
    Prints [PASS]/[FAIL] per check; the exit code is the number of failures.
*/
#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"
#include <cstdio>
#include <functional>

namespace
{
int gFail = 0;

void check (bool ok, const juce::String& what)
{
    std::printf ("[%s] %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8());
    if (! ok) ++gFail;
}

constexpr double kRate = 48000.0;
constexpr int kBlock = 512, kTickEvery = 1600;
const char* const kIds[4] { "low", "mid", "high", "mix" };
const char* const kNames[4] { "LOW", "MID", "HIGH", "MIX" };
const float kDefaults[4] { 0.30f, 0.30f, 0.20f, 1.00f };

/** 4 s, 120 bpm: a kick on every beat, a snare on beats 2 and 4, hats on the eighths; peak -3 dBFS. Deterministic. */
juce::AudioBuffer<float> drumLoop()
{
    const int n = (int) (4.0 * kRate);
    juce::AudioBuffer<float> b (2, n);
    b.clear();
    uint32_t seed = 0x12345678u;
    auto noise = [&seed] { seed = seed * 1664525u + 1013904223u; return (float) ((double) seed / 4294967296.0) * 2.0f - 1.0f; };
    const int beat = (int) (0.5 * kRate), eighth = beat / 2;
    float prevNoise = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const int inBeat = i % beat, beatIdx = i / beat, inEighth = i % eighth;
        const float tb = (float) inBeat / (float) kRate, te = (float) inEighth / (float) kRate;
        // Kick: 55 Hz with a short pitch drop, decaying (phase integrated analytically per beat).
        const float f0 = 55.0f, drop = 1.5f, tau = 0.03f;
        const float phase = 2.0f * juce::MathConstants<float>::pi * f0 * (tb + drop * tau * (1.0f - std::exp (-tb / tau)));
        float s = 0.9f * std::sin (phase) * std::exp (-tb / 0.25f);
        const float nz = noise();
        // Snare on beats 2 and 4: noise burst plus a 185 Hz body.
        if (beatIdx % 2 == 1)
            s += 0.45f * nz * std::exp (-tb / 0.12f) + 0.25f * std::sin (2.0f * juce::MathConstants<float>::pi * 185.0f * tb) * std::exp (-tb / 0.08f);
        // Hats on every eighth: first-difference (high-passed) noise, short.
        const float hp = nz - prevNoise;
        prevNoise = nz;
        s += 0.12f * hp * std::exp (-te / 0.03f);
        b.setSample (0, i, s);
        b.setSample (1, i, s);
    }
    const float peak = b.getMagnitude (0, n);
    if (peak > 0.0f) b.applyGain (std::pow (10.0f, -3.0f / 20.0f) / peak);
    return b;
}

void setParam (KorAudioProcessor& p, const char* id, float v)
{
    auto* prm = p.apvts.getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (v));
}

float raw (KorAudioProcessor& p, int i) { return p.apvts.getRawParameterValue (kIds[i])->load(); }

juce::MouseEvent mouseAt (juce::Component& c, juce::Point<float> pos, juce::Point<float> downPos, juce::ModifierKeys m, int clicks)
{
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), pos, m,
                             juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation, juce::MouseInputSource::defaultRotation,
                             juce::MouseInputSource::defaultTiltX, juce::MouseInputSource::defaultTiltY, &c, &c, now, downPos, now, clicks, pos != downPos);
}

/** A point in the middle of each ring's band on the 3 o'clock side; the fader at 9 o'clock; a pusher's cap. */
juce::Point<float> pointOf (int c)
{
    if (c == KorEditor::mix) return { KorEditor::kCx - KorEditor::kBezelMid, KorEditor::kCy };
    if (c >= KorEditor::push0)
    {
        const float a = KorEditor::kPushA[c - KorEditor::push0], r = KorEditor::kUnitR + 11.0f;
        return { KorEditor::kCx + r * std::sin (a), KorEditor::kCy - r * std::cos (a) };
    }
    const float r = KorEditor::ringOuter (c) - KorEditor::ringWidth (c) * 0.5f;
    return { KorEditor::kCx + r, KorEditor::kCy };
}

/** Vertical drag on the editor by dy px (negative = up), as mouseDown / mouseDrag / mouseUp. If keepDown, no mouseUp. */
void drag (KorEditor& ed, juce::Point<float> at, float dy, bool keepDown = false)
{
    const juce::ModifierKeys btn (juce::ModifierKeys::leftButtonModifier);
    ed.mouseDown (mouseAt (ed, at, at, btn, 1));
    const auto to = at.translated (0.0f, dy);
    ed.mouseDrag (mouseAt (ed, at.translated (0.0f, dy * 0.5f), at, btn, 1));
    ed.mouseDrag (mouseAt (ed, to, at, btn, 1));
    if (! keepDown) ed.mouseUp (mouseAt (ed, to, at, juce::ModifierKeys(), 1));
}

/** Runs the loop through a fresh processor and editor with the given settings, then hands both to `done`. */
void run (const juce::AudioBuffer<float>& loop, const std::vector<std::pair<const char*, float>>& settings,
          const std::function<void (KorAudioProcessor&, KorEditor&)>& done, bool silent = false)
{
    KorAudioProcessor proc;
    proc.setRateAndBufferSizeDetails (kRate, kBlock);
    proc.prepareToPlay (kRate, kBlock);
    for (auto& kv : settings) setParam (proc, kv.first, kv.second);
    std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
    auto& editor = *static_cast<KorEditor*> (ed.get());
    const int n = loop.getNumSamples();
    juce::AudioBuffer<float> buf (2, kBlock);
    juce::MidiBuffer midi;
    int sinceTick = 0;
    for (int pos = 0; pos < n; pos += kBlock)
    {
        const int len = juce::jmin (kBlock, n - pos);
        buf.setSize (2, len, false, false, true);
        for (int c = 0; c < 2; ++c) buf.copyFrom (c, 0, loop, c, pos, len);
        if (silent) buf.clear();
        proc.processBlock (buf, midi);
        sinceTick += len;
        if (sinceTick >= kTickEvery)
        {
            sinceTick -= kTickEvery;
            editor.timerCallback();
        }
    }
    done (proc, editor);
}

bool save (const juce::Image& image, const juce::File& f)
{
    f.deleteFile();
    juce::FileOutputStream stream (f);
    const bool ok = stream.openedOk() && juce::PNGImageFormat().writeImageToStream (image, stream);
    std::printf ("  %s %s %dx%d\n", ok ? "wrote" : "FAILED", f.getFullPathName().toRawUTF8(), image.getWidth(), image.getHeight());
    return ok;
}

juce::String hex (juce::Colour c) { return "#" + c.toDisplayString (false); }
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::File dir = juce::File::getCurrentWorkingDirectory().getChildFile (argc > 1 ? argv[1] : "docs/ui");
    const float hiScale = argc > 2 ? (float) std::atof (argv[2]) : 1.0f;
    dir.createDirectory();
    const auto loop = drumLoop();
    std::printf ("programme: %d samples at %.0f Hz, peak %.2f dBFS\n", loop.getNumSamples(), kRate,
                 (double) juce::Decibels::gainToDecibels (loop.getMagnitude (0, loop.getNumSamples())));

    struct Shot { const char* file; std::vector<std::pair<const char*, float>> settings; bool dragMid; bool gangLow = false; bool silent = false; };
    const Shot shots[] {
        { "alev_default.png",  { { "low", 0.30f }, { "mid", 0.30f }, { "high", 0.20f }, { "mix", 1.00f } }, false },
        { "alev_hot.png",      { { "low", 0.90f }, { "mid", 0.70f }, { "high", 1.00f }, { "mix", 1.00f } }, false },
        { "alev_cold.png",     { { "low", 0.00f }, { "mid", 0.00f }, { "high", 0.00f }, { "mix", 0.00f } }, false },
        { "alev_dragging.png", { { "low", 0.30f }, { "mid", 0.30f }, { "high", 0.20f }, { "mix", 1.00f } }, true },
        { "alev_silent.png",   { { "low", 0.70f }, { "mid", 0.30f }, { "high", 0.10f }, { "mix", 1.00f } }, false, false, true },
        { "alev_silent_mix40.png", { { "low", 0.70f }, { "mid", 0.30f }, { "high", 0.10f }, { "mix", 0.40f } }, false, false, true },
        { "alev_gang.png",     { { "low", 0.30f }, { "mid", 0.30f }, { "high", 0.20f }, { "mix", 1.00f } }, false, true },
    };

    for (const auto& shot : shots)
    {
        std::printf ("%s\n", shot.file);
        run (loop, shot.settings, [&] (KorAudioProcessor& proc, KorEditor& editor)
        {
            if (shot.dragMid)
            {
                // MID from 30 % up to 62 %: 0.32 x 220 px = 70.4 px up; the snapshot is taken with the button still down.
                drag (editor, pointOf (KorEditor::mid), -70.4f, true);
                check (std::lround (raw (proc, 1) * 100.0f) == 62, "MID dragged to " + juce::String (raw (proc, 1) * 100.0f, 1) + " % (want 62)");
            }
            if (shot.gangLow)
            {
                const auto btn = juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier);
                editor.mouseDown (mouseAt (editor, pointOf (KorEditor::push2), pointOf (KorEditor::push2), btn, 1));
                editor.mouseUp (mouseAt (editor, pointOf (KorEditor::push2), pointOf (KorEditor::push2), juce::ModifierKeys(), 1));
                drag (editor, pointOf (KorEditor::low), -44.0f, true);
                check (std::lround (raw (proc, 0) * 100.0f) == 50 && std::lround (raw (proc, 1) * 100.0f) == 50 && std::lround (raw (proc, 2) * 100.0f) == 40,
                       "GANG drag on LOW: " + juce::String (raw (proc, 0) * 100.0f, 1) + " / " + juce::String (raw (proc, 1) * 100.0f, 1) + " / "
                           + juce::String (raw (proc, 2) * 100.0f, 1) + " % (want 50 / 50 / 40)");
            }
            std::printf ("  params low %.2f mid %.2f high %.2f mix %.2f | glow low %.3f mid %.3f high %.3f\n",
                         (double) raw (proc, 0), (double) raw (proc, 1), (double) raw (proc, 2), (double) raw (proc, 3),
                         (double) proc.glow (0), (double) proc.glow (1), (double) proc.glow (2));
            const auto image = editor.createComponentSnapshot (editor.getLocalBounds(), true, 1.0f);
            check (save (image, dir.getChildFile (shot.file)), juce::String ("write ") + shot.file);
            if (hiScale > 1.0f)
            {
                const auto hi = editor.createComponentSnapshot (editor.getLocalBounds(), true, hiScale);
                const juce::String name = juce::String (shot.file).replace (".png", "_" + juce::String ((int) hiScale) + "x.png");
                check (save (hi, dir.getChildFile (name)), "write " + name);
            }
            const auto corner = image.getPixelAt (2, 2), centre = image.getPixelAt ((int) KorEditor::kCx, (int) KorEditor::kCy);
            check (corner.getRed() == 0x03 && corner.getGreen() == 0x37 && corner.getBlue() == 0x33,
                   juce::String (shot.file) + ": corner pixel (2,2) " + hex (corner) + " (want #033733, brand green)");
            check (centre.getRed() > centre.getGreen() && centre.getRed() > centre.getBlue(),
                   juce::String (shot.file) + ": centre pixel " + hex (centre) + " burgundy-ish (R > G and R > B)");
            if (shot.gangLow) editor.mouseUp (mouseAt (editor, pointOf (KorEditor::low), pointOf (KorEditor::low), juce::ModifierKeys(), 1));
            if (shot.dragMid) editor.mouseUp (mouseAt (editor, pointOf (KorEditor::mid), pointOf (KorEditor::mid), juce::ModifierKeys(), 1));
        }, shot.silent);
    }

    // Size, hit areas, resets, gestures.
    {
        struct Gestures : juce::AudioProcessorListener
        {
            int begins[4] {}, ends[4] {};
            void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {}
            void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}
            void audioProcessorParameterChangeGestureBegin (juce::AudioProcessor*, int i) override { if (i >= 0 && i < 4) ++begins[i]; }
            void audioProcessorParameterChangeGestureEnd (juce::AudioProcessor*, int i) override { if (i >= 0 && i < 4) ++ends[i]; }
        };
        KorAudioProcessor proc;
        proc.prepareToPlay (kRate, kBlock);
        Gestures gestures;
        proc.addListener (&gestures);
        std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
        auto& editor = *static_cast<KorEditor*> (ed.get());
        check (editor.getWidth() == 480 && editor.getHeight() == 440 && ! editor.isResizable(),
               "editor " + juce::String (editor.getWidth()) + " x " + juce::String (editor.getHeight()) + ", resizable " + juce::String ((int) editor.isResizable()) + " (want 480 x 440, fixed)");

        // The parameter index of each id in the processor (gesture callbacks report indices).
        int index[4] {};
        for (int c = 0; c < 4; ++c)
            if (auto* prm = dynamic_cast<juce::AudioProcessorParameterWithID*> (proc.apvts.getParameter (kIds[c]))) index[c] = prm->getParameterIndex();

        for (int c = 0; c < 4; ++c)
        {
            const auto at = pointOf (c);
            const int hit = (int) KorEditor::controlAt (at);
            // Drag 44 px = 20 %: up for a control at its low default, down for MIX (default 100 %).
            const float dir = kDefaults[c] > 0.5f ? 1.0f : -1.0f;
            float before[4];
            for (int k = 0; k < 4; ++k) before[k] = raw (proc, k);
            const int b0 = gestures.begins[index[c]], e0 = gestures.ends[index[c]];
            drag (editor, at, 44.0f * dir);
            bool othersStill = true;
            for (int k = 0; k < 4; ++k) if (k != c && raw (proc, k) != before[k]) othersStill = false;
            const float moved = raw (proc, c) - before[c];
            check (hit == c && std::abs (moved + 0.2f * dir) < 0.002f && othersStill,
                   juce::String ("drag on ") + kNames[c] + " at (" + juce::String (at.x, 0) + "," + juce::String (at.y, 0) + "): "
                       + kNames[c] + " " + juce::String (before[c] * 100.0f, 1) + " -> " + juce::String (raw (proc, c) * 100.0f, 1)
                       + " %, others unchanged " + (othersStill ? "yes" : "no"));
            check (gestures.begins[index[c]] == b0 + 1 && gestures.ends[index[c]] == e0 + 1,
                   juce::String ("drag on ") + kNames[c] + ": one change gesture (begin " + juce::String (gestures.begins[index[c]] - b0)
                       + ", end " + juce::String (gestures.ends[index[c]] - e0) + ")");

            for (int k = 0; k < 4; ++k) before[k] = raw (proc, k);
            editor.mouseDoubleClick (mouseAt (editor, at, at, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier), 2));
            othersStill = true;
            for (int k = 0; k < 4; ++k) if (k != c && raw (proc, k) != before[k]) othersStill = false;
            check (std::abs (raw (proc, c) - kDefaults[c]) < 1.0e-6f && othersStill,
                   juce::String ("double-click on ") + kNames[c] + ": " + juce::String (raw (proc, c) * 100.0f, 1) + " % (default "
                       + juce::String (kDefaults[c] * 100.0f, 0) + "), others unchanged " + (othersStill ? "yes" : "no"));
        }
        // The pushers and the link modes. All rings to 30 % first.
        for (int c = 0; c < 3; ++c) setParam (proc, kIds[c], 0.30f);
        const auto btn = juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier);
        auto push = [&] (int i)
        {
            const auto at = pointOf (KorEditor::push0 + i);
            check ((int) KorEditor::controlAt (at) == KorEditor::push0 + i, "pusher " + juce::String (i + 1) + " hit at (" + juce::String (at.x, 0) + "," + juce::String (at.y, 0) + ")");
            editor.mouseDown (mouseAt (editor, at, at, btn, 1));
            editor.mouseUp (mouseAt (editor, at, at, juce::ModifierKeys(), 1));
            return juce::roundToInt (proc.apvts.getRawParameterValue ("link")->load());
        };
        check (juce::roundToInt (proc.apvts.getRawParameterValue ("link")->load()) == 0, "link mode default FREE");
        check (push (1) == 1, "pusher 2 -> LINK");
        // LINK: the heat spreads to the neighbours in frequency, 30 % behind per step (owner, 8 Oct):
        // HIGH +20 -> MID +14, LOW +9.8; LOW +20 -> MID +14, HIGH +9.8; MID +20 -> LOW +14, HIGH +14.
        const float want[3][3] { { 0.20f, 0.14f, 0.098f }, { 0.14f, 0.20f, 0.14f }, { 0.098f, 0.14f, 0.20f } };
        for (int t : { 2, 0, 1 })
        {
            for (int c = 0; c < 3; ++c) setParam (proc, kIds[c], 0.30f);
            float before[4];
            for (int k = 0; k < 4; ++k) before[k] = raw (proc, k);
            int b0[3];
            for (int k = 0; k < 3; ++k) b0[k] = gestures.begins[index[k]];
            drag (editor, pointOf (t), -44.0f);
            bool ok = raw (proc, 3) == before[3];
            juce::String got;
            for (int k = 0; k < 3; ++k)
            {
                const float d = raw (proc, k) - before[k];
                ok = ok && std::abs (d - want[t][k]) < 0.002f && gestures.begins[index[k]] == b0[k] + 1;
                got << kNames[k] << " +" << juce::String (d * 100.0f, 1) << " ";
            }
            check (ok, juce::String ("LINK drag on ") + kNames[t] + ": " + got + "(want " + juce::String (want[t][0] * 100.0f, 1) + " / "
                           + juce::String (want[t][1] * 100.0f, 1) + " / " + juce::String (want[t][2] * 100.0f, 1) + "), one gesture each, MIX unchanged");
        }
        check (push (2) == 2, "pusher 3 -> GANG");
        for (int c = 0; c < 3; ++c) setParam (proc, kIds[c], 0.30f);
        drag (editor, pointOf (KorEditor::mid), 44.0f);
        check (std::abs (raw (proc, 0) - 0.10f) < 0.002f && std::abs (raw (proc, 1) - 0.10f) < 0.002f && std::abs (raw (proc, 2) - 0.10f) < 0.002f,
               "GANG drag on MID down 20 %: " + juce::String (raw (proc, 0) * 100.0f, 1) + " / " + juce::String (raw (proc, 1) * 100.0f, 1) + " / "
                   + juce::String (raw (proc, 2) * 100.0f, 1) + " % (want 10 / 10 / 10)");
        drag (editor, pointOf (KorEditor::mix), 44.0f);
        check (std::abs (raw (proc, 0) - 0.10f) < 0.002f, "GANG does not move MIX's neighbours: LOW " + juce::String (raw (proc, 0) * 100.0f, 1) + " %");
        check (push (0) == 0, "pusher 1 -> FREE");
        check (KorEditor::controlAt ({ KorEditor::kCx, KorEditor::kCy }) == KorEditor::none, "the ember is not a control");
        proc.removeListener (&gestures);
    }

    // Host bypass: the input delayed by exactly the reported latency once settled; a 20 ms crossfade each way.
    {
        KorAudioProcessor proc;
        proc.setRateAndBufferSizeDetails (kRate, kBlock);
        proc.prepareToPlay (kRate, kBlock);
        for (int c = 0; c < 3; ++c) setParam (proc, kIds[c], 1.0f);   // full heat: wet and dry differ a lot
        const int lat = proc.getLatencySamples();
        juce::MidiBuffer midi;
        juce::AudioBuffer<float> buf (2, kBlock);
        juce::Random rng (7);
        std::vector<float> in, out;
        for (int blk = 0; blk < 40; ++blk)
        {
            for (int i = 0; i < kBlock; ++i) { const float v = 0.5f * (rng.nextFloat() * 2.0f - 1.0f); buf.setSample (0, i, v); buf.setSample (1, i, v); in.push_back (v); }
            proc.processBlockBypassed (buf, midi);
            for (int i = 0; i < kBlock; ++i) out.push_back (buf.getSample (0, i));
        }
        float err = 0.0f;
        for (size_t i = (size_t) lat; i < out.size(); ++i) err = juce::jmax (err, std::abs (out[i] - in[i - (size_t) lat]));
        check (err == 0.0f, "bypass = input delayed by " + juce::String (lat) + " samples, max error " + juce::String (err, 9) + " (want 0)");
        // Release the bypass and compare with a twin that was never bypassed (same input, so the same engine state):
        // out = dry + w (wet - dry); w must rise steadily from 0 to 1 over 20 ms (960 samples) and then stay at 1.
        KorAudioProcessor twin;
        twin.setRateAndBufferSizeDetails (kRate, kBlock);
        twin.prepareToPlay (kRate, kBlock);
        for (int c = 0; c < 3; ++c) setParam (twin, kIds[c], 1.0f);
        juce::AudioBuffer<float> tb (2, kBlock);
        for (int blk = 0; blk < 40; ++blk)
        {
            for (int i = 0; i < kBlock; ++i) { tb.setSample (0, i, in[(size_t) (blk * kBlock + i)]); tb.setSample (1, i, in[(size_t) (blk * kBlock + i)]); }
            twin.processBlock (tb, midi);
        }
        float wPrev = 0.0f, wMin = 1.0f, back = 0.0f; int firstFull = -1;
        for (int blk = 0; blk < 4; ++blk)
        {
            for (int i = 0; i < kBlock; ++i)
            {
                const float v = 0.5f * (rng.nextFloat() * 2.0f - 1.0f);
                buf.setSample (0, i, v); buf.setSample (1, i, v); tb.setSample (0, i, v); tb.setSample (1, i, v); in.push_back (v);
            }
            proc.processBlock (buf, midi);
            twin.processBlock (tb, midi);
            for (int i = 0; i < kBlock; ++i)
            {
                const size_t k = in.size() - (size_t) kBlock + (size_t) i;
                const float dry = in[k - (size_t) lat], wet = tb.getSample (0, i), o = buf.getSample (0, i);
                if (std::abs (wet - dry) < 0.05f) continue;
                const float w = (o - dry) / (wet - dry);
                back = juce::jmax (back, wPrev - w);
                wPrev = w; wMin = juce::jmin (wMin, w);
                if (firstFull < 0 && w > 0.999f) firstFull = blk * kBlock + i;
            }
        }
        check (wMin < 0.01f && back < 0.002f && firstFull > 900 && firstFull < 1000,
               "bypass release: crossfade rises 0 -> 1 without stepping back (back " + juce::String (back, 4) + "), full at sample "
                   + juce::String (firstFull) + " (want ~960 = 20 ms)");
    }

    std::printf ("failures: %d\n", gFail);
    return gFail;
}
