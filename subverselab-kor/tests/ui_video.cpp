/*  Renders the ALEV demo video: real audio through the real processor, the real panel drawn every frame at 30 fps
    while a scripted hand turns the rings (mouse events, so the ember names the ring and shows its value).

        ui_video <in.wav> <outdir> [scale]

    Writes <outdir>/f00000.png ... (one per 1/30 s) and <outdir>/alev_out.wav (float, 48 kHz, as the plug-in played it),
    plus <outdir>/script.txt with the scene times. Input must be 48 kHz stereo.
*/
#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"
#include <cstdio>

namespace
{
juce::MouseEvent ev (juce::Component& c, juce::Point<float> pos, juce::Point<float> down, bool button)
{
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), pos,
                             button ? juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier) : juce::ModifierKeys(),
                             juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation, juce::MouseInputSource::defaultRotation,
                             juce::MouseInputSource::defaultTiltX, juce::MouseInputSource::defaultTiltY, &c, &c, now, down, now, 1, pos != down);
}
juce::Point<float> ringPoint (int c)
{
    const float r = KorEditor::ringOuter (c) - KorEditor::ringWidth (c) * 0.5f;
    return { KorEditor::kCx + r, KorEditor::kCy };
}
juce::Point<float> pushPoint (int i)
{
    const float a = KorEditor::kPushA[i], r = KorEditor::kUnitR + 11.0f;
    return { KorEditor::kCx + r * std::sin (a), KorEditor::kCy - r * std::cos (a) };
}

// One scripted gesture: drag control c by `amount` (0..1 of travel, + = up) between t0 and t1 seconds.
struct Drag { double t0, t1; int control; float amount; };
struct Press { double t; int pusher; };
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc < 3) { std::printf ("usage: ui_video <in.wav> <outdir> [scale]\n"); return 2; }
    const juce::File in (juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]));
    const juce::File dir (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]));
    const float scale = argc > 3 ? (float) std::atof (argv[3]) : 2.0f;
    dir.createDirectory();

    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (in));
    if (rd == nullptr || (rd->sampleRate != 48000.0 && rd->sampleRate != 44100.0)) { std::printf ("need a 44.1 or 48 kHz file\n"); return 2; }
    const int n = (int) rd->lengthInSamples;
    juce::AudioBuffer<float> src (2, n);
    rd->read (&src, 0, n, 0, true, true);

    const double sr = rd->sampleRate; const int perFrame = (int) (sr / 30.0);   // 30 fps: 1600 at 48 k, 1470 at 44.1 k
    KorAudioProcessor proc;
    proc.setRateAndBufferSizeDetails (sr, perFrame);
    proc.prepareToPlay (sr, perFrame);
    auto set = [&] (const char* id, float v) { auto* p = proc.apvts.getParameter (id); p->setValueNotifyingHost (p->convertTo0to1 (v)); };
    set ("low", 0.30f); set ("mid", 0.30f); set ("high", 0.20f); set ("mix", 0.0f);
    std::unique_ptr<juce::AudioProcessorEditor> edBase (proc.createEditor());
    auto& ed = *static_cast<KorEditor*> (edBase.get());

    // The story (seconds). 0-4 dry (fader at DRY); 4-7 fader up to WET; 8-12 LOW up (FREE); 13 LINK;
    // 14-19 HIGH up: MID follows at 70 %, LOW at 49 %; 20.5 GANG; 21.5-26 MID down: all three together; 27.5 FREE; it plays.
    const Drag drags[] { { 4.0, 7.0, KorEditor::mix, 1.0f }, { 8.0, 12.0, KorEditor::low, 0.30f }, { 14.0, 19.0, KorEditor::high, 0.40f },
                         { 21.5, 26.0, KorEditor::mid, -0.20f } };
    const Press presses[] { { 13.0, 1 }, { 20.5, 2 }, { 27.5, 0 } };
    juce::String script;
    for (auto& d : drags) script << "drag " << (int) d.control << " " << d.t0 << "-" << d.t1 << " by " << d.amount << "\n";
    for (auto& p : presses) script << "press " << p.pusher << " at " << p.t << "\n";
    dir.getChildFile ("script.txt").replaceWithText (script);

    juce::AudioBuffer<float> out (2, n), blk (2, perFrame);
    juce::MidiBuffer midi;
    int active = -1; float lastY = 0.0f; juce::Point<float> downAt;
    const int frames = n / perFrame;
    for (int f = 0; f < frames; ++f)
    {
        const double t = f / 30.0;
        for (auto& p : presses)
            if (t >= p.t && t < p.t + 1.0 / 30.0) { const auto at = pushPoint (p.pusher); ed.mouseDown (ev (ed, at, at, true)); ed.mouseUp (ev (ed, at, at, false)); }
        for (auto& d : drags)
        {
            const auto at = d.control == KorEditor::mix ? juce::Point<float> (KorEditor::kCx - KorEditor::kBezelMid, KorEditor::kCy) : ringPoint (d.control);
            if (t >= d.t0 && active < 0 && t < d.t1) { active = d.control; downAt = at; lastY = at.y; ed.mouseDown (ev (ed, at, at, true)); }
            if (active == d.control && t >= d.t0 && t <= d.t1 + 1.0 / 30.0)
            {
                // Ease in/out over the gesture; 220 px of travel = 100 %.
                const double u = juce::jlimit (0.0, 1.0, (t - d.t0) / (d.t1 - d.t0));
                const double e = u * u * (3.0 - 2.0 * u);
                const float y = downAt.y - (float) e * d.amount * 220.0f;
                ed.mouseDrag (ev (ed, { downAt.x, y }, downAt, true));
                lastY = y;
                if (u >= 1.0) { ed.mouseUp (ev (ed, { downAt.x, lastY }, downAt, false)); active = -1; }
            }
        }
        for (int c = 0; c < 2; ++c) blk.copyFrom (c, 0, src, c, f * perFrame, perFrame);
        proc.processBlock (blk, midi);
        for (int c = 0; c < 2; ++c) out.copyFrom (c, f * perFrame, blk, c, 0, perFrame);
        ed.timerCallback();
        const auto img = ed.createComponentSnapshot (ed.getLocalBounds(), true, scale);
        juce::FileOutputStream os (dir.getChildFile (juce::String::formatted ("f%05d.png", f)));
        juce::PNGImageFormat().writeImageToStream (img, os);
        if (f % 150 == 0) std::printf ("frame %d / %d\n", f, frames);
    }
    proc.releaseResources();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> os (dir.getChildFile ("alev_out.wav").createOutputStream().release());
    auto writer = wav.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (2).withBitsPerSample (32)
                                                                             .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    if (writer != nullptr) writer->writeFromAudioSampleBuffer (out, 0, frames * perFrame);
    std::printf ("done: %d frames, latency %d samples\n", frames, proc.getLatencySamples());
    return 0;
}
