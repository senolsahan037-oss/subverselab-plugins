// rack_render: renders a note list from a saved Reason Rack Plugin state with no screen (see rack_capture).
//
//   rack_render --state out/rack/name.rackstate --notes n.txt --out x.wav [--sr 44100] [--tail 3] [--warmup 1]
// Notes file as `svl render`: "pitch velocity start length" (seconds), '#' comments. Output: 24-bit stereo WAV; prints the peak.
// Nothing is installed or changed.
#include "rack_common.h"

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    juce::StringArray args; for (int i = 1; i < argc; ++i) args.add (argv[i]);
    auto arg = [&] (const juce::String& k, const juce::String& d) { const int i = args.indexOf (k); return i >= 0 && i + 1 < args.size() ? args[i + 1] : d; };
    const double sr = arg ("--sr", "44100").getDoubleValue(), tail = arg ("--tail", "3").getDoubleValue(), warm = arg ("--warmup", "1").getDoubleValue();
    if (args.contains ("--make-default-state"))   // check of the path: load the plug-in with no patch and save its state (no window opens)
    {
        juce::AudioPluginFormatManager fm0; rack::addFormats (fm0);
        juce::PluginDescription d0; juce::String e0;
        if (! rack::findDescription (fm0, rack::defaultPlugin (args.contains ("--au")), d0, e0)) { std::printf ("rack_render: %s\n", e0.toRawUTF8()); return 3; }
        auto p0 = fm0.createPluginInstance (d0, 44100.0, 512, e0);
        if (p0 == nullptr) { std::printf ("rack_render: cannot load %s: %s\n", d0.name.toRawUTF8(), e0.toRawUTF8()); return 4; }
        p0->prepareToPlay (44100.0, 512);
        if (args.contains ("--list-programs"))
        {
            std::printf ("programs: %d, current %d\n", p0->getNumPrograms(), p0->getCurrentProgram());
            for (int i = 0; i < juce::jmin (50, p0->getNumPrograms()); ++i) std::printf ("  %d: %s\n", i, p0->getProgramName (i).toRawUTF8());
            int shown = 0;
            for (auto* q : p0->getParameters()) if (shown < 40 && (q->getName (64).containsIgnoreCase ("patch") || q->getName (64).containsIgnoreCase ("program") || q->getName (64).containsIgnoreCase ("preset"))) { std::printf ("  param %d '%s'\n", q->getParameterIndex(), q->getName (64).toRawUTF8()); ++shown; }
        }
        juce::MemoryBlock st; p0->getStateInformation (st);
        const juce::File o (arg ("--make-default-state", "")); o.getParentDirectory().createDirectory();
        std::printf ("rack_render: loaded %s (%s), %d parameters, state %d bytes, saved %s: %s\n", d0.name.toRawUTF8(), d0.pluginFormatName.toRawUTF8(),
                     p0->getParameters().size(), (int) st.getSize(), o.getFullPathName().toRawUTF8(), rack::save (o, d0, st) ? "ok" : "FAILED");
        return 0;
    }
    const juce::File stateFile (arg ("--state", "")), notesFile (arg ("--notes", "")), outFile (arg ("--out", ""));
    if (! stateFile.existsAsFile() || ! notesFile.existsAsFile() || outFile.getFullPathName().isEmpty())
    { std::printf ("usage: rack_render --state f.rackstate --notes n.txt --out x.wav [--sr 44100] [--tail 3] [--warmup 1]\n"); return 2; }

    struct Note { int key; int vel; double start, len; };
    std::vector<Note> notes; double end = 0;
    for (auto& line : juce::StringArray::fromLines (notesFile.loadFileAsString()))
    {
        const auto t = line.upToFirstOccurrenceOf ("#", false, false).trim();
        if (t.isEmpty()) continue;
        const auto w = juce::StringArray::fromTokens (t, " \t", "");
        if (w.size() < 4) continue;
        notes.push_back ({ w[0].getIntValue(), w[1].getIntValue(), w[2].getDoubleValue(), w[3].getDoubleValue() });
        end = juce::jmax (end, notes.back().start + notes.back().len);
    }
    juce::PluginDescription desc; juce::MemoryBlock state;
    if (! rack::load (stateFile, desc, state)) { std::printf ("rack_render: cannot read %s\n", stateFile.getFullPathName().toRawUTF8()); return 3; }
    juce::AudioPluginFormatManager fm; rack::addFormats (fm);
    juce::String err;
    auto plugin = fm.createPluginInstance (desc, sr, 512, err);
    if (plugin == nullptr) { std::printf ("rack_render: cannot load %s: %s\n", desc.name.toRawUTF8(), err.toRawUTF8()); return 4; }
    std::printf ("rack_render: %d in / %d out channels, %d parameters\n", plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels(), plugin->getParameters().size());
    std::fflush (stdout);
    plugin->prepareToPlay (sr, 512);
    plugin->setStateInformation (state.getData(), (int) state.getSize());
    auto pump = [] (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); };
    pump (800);   // the patch loads on the message thread

    const int block = 512, nCh = juce::jmax (2, plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels());
    juce::AudioBuffer<float> buf (nCh, block);
    juce::MidiBuffer midi;
    for (int i = 0; i < (int) (warm * sr) / block; ++i) { buf.clear(); midi.clear(); plugin->processBlock (buf, midi); pump (1); }

    const int total = (int) ((end + tail) * sr);
    juce::AudioBuffer<float> out (2, total); out.clear();
    for (int pos = 0; pos < total; pos += block)
    {
        const int n = juce::jmin (block, total - pos);
        buf.clear(); midi.clear();
        for (auto& nt : notes)
        {
            const int on = (int) std::lround (nt.start * sr), off = (int) std::lround ((nt.start + nt.len) * sr);
            if (on >= pos && on < pos + n) midi.addEvent (juce::MidiMessage::noteOn (1, nt.key, (juce::uint8) nt.vel), on - pos);
            if (off >= pos && off < pos + n) midi.addEvent (juce::MidiMessage::noteOff (1, nt.key), off - pos);
        }
        plugin->processBlock (buf, midi);
        for (int c = 0; c < 2; ++c) out.copyFrom (c, pos, buf, juce::jmin (c, buf.getNumChannels() - 1), 0, n);
        if ((pos / block) % 8 == 0) pump (1);
    }
    outFile.getParentDirectory().createDirectory(); outFile.deleteFile();
    juce::WavAudioFormat wav;
    if (auto os = std::unique_ptr<juce::FileOutputStream> (outFile.createOutputStream()))
        if (std::unique_ptr<juce::AudioFormatWriter> w { wav.createWriterFor (os.get(), sr, 2, 24, {}, 0) }) { os.release(); w->writeFromAudioSampleBuffer (out, 0, total); }
    const float pk = juce::jmax (out.getMagnitude (0, 0, total), out.getMagnitude (1, 0, total));
    std::printf ("rack_render: %s, %d notes, %.2f s, peak %.1f dBFS -> %s\n", desc.name.toRawUTF8(), (int) notes.size(), total / sr, juce::Decibels::gainToDecibels (pk), outFile.getFullPathName().toRawUTF8());
    return pk > 0.0f ? 0 : 5;   // 5: the plug-in made no sound (patch not loaded)
}
