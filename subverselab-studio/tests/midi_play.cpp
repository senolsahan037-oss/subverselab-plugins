// midi_play: plays a Standard MIDI File through the JING FANG processor with no host and no screen (GENERATE plan §7: how a
// clip dragged out of JING FANG sounds when it comes back in). Every channel's events reach processBlock on their sample.
//
//   midi_play --preset presets/Init/Init.xml --mid clip.mid --out x.wav [--sr 48000] [--tail 2]
// Output: 24-bit stereo WAV. Nothing is installed or changed.
#include <JuceHeader.h>
#include "PluginProcessor.h"

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    juce::StringArray args; for (int i = 1; i < argc; ++i) args.add (argv[i]);
    auto arg = [&] (const juce::String& k, const juce::String& d) { const int i = args.indexOf (k); return i >= 0 && i + 1 < args.size() ? args[i + 1] : d; };
    const juce::File presetFile (juce::File::getCurrentWorkingDirectory().getChildFile (arg ("--preset", ""))),
                     midFile (juce::File::getCurrentWorkingDirectory().getChildFile (arg ("--mid", ""))),
                     outFile (juce::File::getCurrentWorkingDirectory().getChildFile (arg ("--out", "")));
    const double sr = arg ("--sr", "48000").getDoubleValue(), tail = arg ("--tail", "2").getDoubleValue();
    if (! presetFile.existsAsFile() || ! midFile.existsAsFile() || arg ("--out", "").isEmpty())
    { std::printf ("usage: midi_play --preset p.xml --mid clip.mid --out x.wav [--sr 48000] [--tail 2]\n"); return 2; }

    juce::MidiFile mf;
    { juce::FileInputStream in (midFile); if (! in.openedOk() || ! mf.readFrom (in)) { std::printf ("midi_play: cannot read %s\n", midFile.getFullPathName().toRawUTF8()); return 3; } }
    mf.convertTimestampTicksToSeconds();
    juce::MidiMessageSequence all;
    for (int t = 0; t < mf.getNumTracks(); ++t) all.addSequence (*mf.getTrack (t), 0.0);
    all.sort();   // stable: events at one time keep their file order
    double end = 0;
    for (auto* e : all) end = std::max (end, e->message.getTimeStamp());

    JingFangProcessor p;
    if (! p.loadPresetXml (presetFile.loadFileAsString())) { std::printf ("midi_play: cannot load the preset\n"); return 4; }
    const int block = 512;
    p.setRateAndBufferSizeDetails (sr, block); p.prepareToPlay (sr, block);
    const int64_t total = (int64_t) std::ceil ((end + tail) * sr);
    juce::AudioBuffer<float> buf (2, block), out (2, (int) total);
    int next = 0, events = 0;
    for (int64_t pos = 0; pos < total; pos += block)
    {
        const int n = (int) std::min<int64_t> (block, total - pos);
        juce::MidiBuffer m;
        while (next < all.getNumEvents())
        {
            const auto& msg = all.getEventPointer (next)->message;
            if (msg.isMetaEvent()) { ++next; continue; }
            const int64_t s = (int64_t) std::llround (msg.getTimeStamp() * sr);
            if (s >= pos + n) break;
            m.addEvent (msg, (int) std::max<int64_t> (0, s - pos)); ++next; ++events;
        }
        buf.setSize (2, n, false, false, true); buf.clear();
        p.processBlock (buf, m);
        for (int c = 0; c < 2; ++c) out.copyFrom (c, (int) pos, buf, c, 0, n);
    }
    outFile.getParentDirectory().createDirectory(); outFile.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (outFile), sr, 2, 24, {}, 0));
    if (w == nullptr || ! w->writeFromAudioSampleBuffer (out, 0, out.getNumSamples())) { std::printf ("midi_play: cannot write %s\n", outFile.getFullPathName().toRawUTF8()); return 5; }
    std::printf ("midi_play: %d events, %.2f s, peak %.1f dBFS -> %s\n", events, total / sr,
                 juce::Decibels::gainToDecibels (out.getMagnitude (0, out.getNumSamples())), outFile.getFullPathName().toRawUTF8());
    return 0;
}
