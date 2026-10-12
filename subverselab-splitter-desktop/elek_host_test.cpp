// elek_host_test: loads a built ELEK.vst3 the way a DAW does (JUCE plug-in hosting, LoadLibrary of the bundle's binary), opens its
// editor with SPLITTER_AUTOTEST pointing at a generated 20 s test file, and waits until ELEK has written its stems to Music/ELEK.
// Exit code 0 only when the plug-in loaded, the separation finished and every stem file is a non-silent WAV of the right length.
//
// Written 2026-10-12 for the first Windows build: on Windows a VST3 that imports onnxruntime.dll directly does not load in a DAW
// (the DLL is searched next to the host, not the plug-in). This proves the delay-load path end to end on a real Windows runner.
//
//   elek_host_test <path to ELEK.vst3> [timeout seconds = 900]
#include <JuceHeader.h>
#include <cstdio>
#include <cstdlib>

static int fail (const juce::String& why) { std::printf ("FAIL %s\n", why.toRawUTF8()); return 1; }

static juce::File writeTestFile()
{
    // 20 s, 44.1 kHz stereo: a kick every half second, a bass line, a chord pad and a sung-like vibrato tone. Enough for every stem.
    const double sr = 44100.0; const int n = (int) (20.0 * sr);
    juce::AudioBuffer<float> b (2, n);
    for (int i = 0; i < n; ++i)
    {
        const double t = i / sr, beat = std::fmod (t, 0.5);
        const double kick = 0.6 * std::exp (-beat / 0.12) * std::sin (2 * juce::MathConstants<double>::pi * (50.0 + 80.0 * std::exp (-beat / 0.03)) * beat);
        const double bass = 0.25 * std::sin (2 * juce::MathConstants<double>::pi * (std::fmod (t, 4.0) < 2.0 ? 55.0 : 73.4) * t);
        const double pad = 0.08 * (std::sin (2 * juce::MathConstants<double>::pi * 220.0 * t) + std::sin (2 * juce::MathConstants<double>::pi * 277.2 * t) + std::sin (2 * juce::MathConstants<double>::pi * 329.6 * t));
        const double voice = 0.2 * std::sin (2 * juce::MathConstants<double>::pi * (440.0 + 6.0 * std::sin (2 * juce::MathConstants<double>::pi * 5.5 * t)) * t) * (std::fmod (t, 2.0) < 1.5 ? 1.0 : 0.0);
        const float x = (float) (kick + bass + pad + voice);
        b.setSample (0, i, x); b.setSample (1, i, x);
    }
    auto f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("elek_host_test.wav");
    f.deleteFile();
    juce::WavAudioFormat wav;
    if (auto* os = f.createOutputStream().release())
        if (auto w = std::unique_ptr<juce::AudioFormatWriter> (wav.createWriterFor (os, sr, 2, 16, {}, 0)))
            w->writeFromAudioSampleBuffer (b, 0, n);
    return f;
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc < 2) return fail ("usage: elek_host_test <ELEK.vst3> [timeout s]");
    const juce::File bundle { juce::String (argv[1]) };
    const int timeoutS = argc > 2 ? std::atoi (argv[2]) : 900;
    const auto input = writeTestFile();
    std::printf ("test file %s (%lld bytes)\n", input.getFullPathName().toRawUTF8(), (long long) input.getSize());

    const auto root = juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("ELEK");
    juce::Array<juce::File> before;
    if (root.isDirectory()) before = root.findChildFiles (juce::File::findDirectories, false);

    juce::AudioPluginFormatManager fm;
    juce::addDefaultFormatsToManager (fm);
    juce::OwnedArray<juce::PluginDescription> types;
    for (auto* f : fm.getFormats())
        if (f->fileMightContainThisPluginType (bundle.getFullPathName()))
            f->findAllTypesForFile (types, bundle.getFullPathName());
    if (types.isEmpty()) return fail ("no plug-in found in " + bundle.getFullPathName() + " (the binary did not load: missing DLL?)");
    juce::String err;
    auto inst = fm.createPluginInstance (*types[0], 44100.0, 512, err);
    if (inst == nullptr) return fail ("createPluginInstance: " + err);
    std::printf ("PASS loaded %s %s (%s)\n", types[0]->name.toRawUTF8(), types[0]->version.toRawUTF8(), types[0]->pluginFormatName.toRawUTF8());
    inst->prepareToPlay (44100.0, 512);

   #if JUCE_WINDOWS
    _putenv_s ("SPLITTER_AUTOTEST", input.getFullPathName().toRawUTF8());
   #else
    setenv ("SPLITTER_AUTOTEST", input.getFullPathName().toRawUTF8(), 1);
   #endif
    std::unique_ptr<juce::AudioProcessorEditor> editor (inst->createEditorIfNeeded());
    if (editor == nullptr) return fail ("no editor");
    std::printf ("PASS editor open, separation started by SPLITTER_AUTOTEST\n");

    // Wait for a new run folder holding Vocals, Drums, Bass, Other (and Instrumental) WAVs, while ELEK's render keeps the host fed.
    juce::AudioBuffer<float> io (juce::jmax (2, inst->getTotalNumInputChannels(), inst->getTotalNumOutputChannels()), 512);
    juce::MidiBuffer midi;
    const auto start = juce::Time::getMillisecondCounter();
    juce::File run;
    while ((int) ((juce::Time::getMillisecondCounter() - start) / 1000) < timeoutS)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
        io.clear(); inst->processBlock (io, midi);
        if (root.isDirectory())
            for (auto& d : root.findChildFiles (juce::File::findDirectories, false))
                if (! before.contains (d) && d.findChildFiles (juce::File::findFiles, false, "*.wav").size() >= 4)
                    run = d;
        if (run.exists()) { juce::MessageManager::getInstance()->runDispatchLoopUntil (3000); break; }
    }
    if (! run.exists()) return fail ("no stems in " + root.getFullPathName() + " after " + juce::String (timeoutS) + " s (see the ELEK.log printed below)");
    std::printf ("PASS stems written in %u s to %s\n", (juce::Time::getMillisecondCounter() - start) / 1000, run.getFullPathName().toRawUTF8());

    int bad = 0;
    juce::AudioFormatManager afm; afm.registerBasicFormats();
    for (auto& w : run.findChildFiles (juce::File::findFiles, false, "*.wav"))
    {
        std::unique_ptr<juce::AudioFormatReader> r (afm.createReaderFor (w));
        if (r == nullptr) { ++bad; std::printf ("FAIL unreadable %s\n", w.getFileName().toRawUTF8()); continue; }
        juce::AudioBuffer<float> b ((int) r->numChannels, (int) r->lengthInSamples);
        r->read (&b, 0, (int) r->lengthInSamples, 0, true, true);
        const double secs = (double) r->lengthInSamples / r->sampleRate, peak = b.getMagnitude (0, b.getNumSamples());
        const bool ok = std::abs (secs - 20.0) < 0.5 && peak > 1.0e-4;
        bad += ok ? 0 : 1;
        std::printf ("%s %-60s %.2f s peak %.4f\n", ok ? "PASS" : "FAIL", w.getFileName().toRawUTF8(), secs, peak);
    }
    editor.reset(); inst->releaseResources(); inst.reset();
    std::printf ("\nelek_host_test: %s\n", bad == 0 ? "ALL PASS" : "FAILED");
    return bad == 0 ? 0 : 1;
}
