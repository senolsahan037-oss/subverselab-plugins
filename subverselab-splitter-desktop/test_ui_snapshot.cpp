/*  Renders the editor to a PNG without a screen.

    `screencapture` needs macOS Screen Recording permission, which a terminal
    does not have, so it hands back the desktop wallpaper instead of the window.
    `createComponentSnapshot` draws the component into an offscreen image
    through the same paint path the real window uses, needs no permission, and
    can be pointed at any state — including the one that only exists after a
    separation has finished.

        ./build/test_ui_snapshot_artefacts/Release/test_ui_snapshot <audio> <out.png>
*/
#include <JuceHeader.h>

#include "Source/PluginProcessor.h"
#include "Source/PluginEditor.h"

#include <thread>
#include <cstdio>
#if JUCE_MAC
 #include <mach/mach.h>
 #include <sys/resource.h>
#endif

int main (int argc, char** argv)
{
    if (argc < 3)
    {
        std::cout << "usage: test_ui_snapshot <audio file> <out.png>\n";
        return 1;
    }

    juce::ScopedJuceInitialiser_GUI juceInit;

    // TEST ONLY: ELEK_MEM_TRACE=<csv> samples this process every 50 ms (epoch ms, resident MB, phys_footprint MB, getrusage max MB)
    // so peaks shorter than an external 0.5 s sampler can see are on record. Off unless the variable is set.
   #if JUCE_MAC     // macOS only (mach task_info); the Windows CI build has no memory trace
    if (const char* trace = std::getenv ("ELEK_MEM_TRACE"); trace != nullptr && *trace != 0)
    {
        std::thread ([path = std::string (trace)]
        {
            FILE* f = std::fopen (path.c_str(), "w");
            if (f == nullptr) return;
            std::fprintf (f, "epoch_ms,rss_mb,footprint_mb,maxrss_mb\n");
            for (;;)
            {
                task_vm_info_data_t vm {};
                mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
                rusage ru {};
                getrusage (RUSAGE_SELF, &ru);
                if (task_info (mach_task_self(), TASK_VM_INFO, (task_info_t) &vm, &count) == KERN_SUCCESS)
                    std::fprintf (f, "%lld,%.1f,%.1f,%.1f\n", (long long) juce::Time::currentTimeMillis(), vm.resident_size / 1048576.0,
                                  vm.phys_footprint / 1048576.0, ru.ru_maxrss / 1048576.0);
                std::fflush (f);
                std::this_thread::sleep_for (std::chrono::milliseconds (50));
            }
        }).detach();
    }
   #endif

    // TEST ONLY (ELEK 2.0.0 engine, no window): "--engine <stems|stems6|drumsonly|drumparts> <audio file> <result.txt> [startSec endSec] [runFolder]"
    // runs one operation of the ModelRunner on its thread, waits, and writes success / error / folder / file list to result.txt.
    // startSec endSec of -1 -1 = the whole file; runFolder (drumparts only) = split into that existing run.
    if (juce::String (argv[1]) == "--engine" && argc >= 5)
    {
        using Op = SubverseSplitterAudioProcessor::ModelRunner::Operation;
        const juce::String which (argv[2]);
        const Op op = which == "stems6" ? Op::Stems6 : which == "drumsonly" ? Op::DrumsOnly : which == "drumparts" ? Op::DrumParts
                    : which == "analysis" ? Op::Analysis : which == "vocal" ? Op::VocalRemoval : Op::Stems;
        // TEST ONLY: ELEK_TEST_CANCEL_MS=<ms> presses Cancel that long after the start and reports how long the run took to stop
        const int cancelAfter = std::getenv ("ELEK_TEST_CANCEL_MS") ? std::atoi (std::getenv ("ELEK_TEST_CANCEL_MS")) : -1;
        const juce::File input { juce::String::fromUTF8 (argv[3]) }, result { juce::String::fromUTF8 (argv[4]) };
        const double from = argc >= 7 ? juce::String (argv[5]).getDoubleValue() : -1.0, to = argc >= 7 ? juce::String (argv[6]).getDoubleValue() : -1.0;
        const juce::File runFolder = argc >= 8 ? juce::File (juce::String::fromUTF8 (argv[7])) : juce::File();
        SubverseSplitterAudioProcessor processor;
        processor.prepareToPlay (44100.0, 512);
        auto& runner = processor.getModelRunner();
        runner.startOperation (input, op, from, to, runFolder);
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        double cancelledAt = -1.0, lastProgress = -1.0;
        bool progressWentBack = false;
        while (runner.isRunning())
        {
            juce::Thread::sleep (50);
            const double pr = runner.getProgress();
            if (pr < lastProgress - 1.0e-9) progressWentBack = true;
            lastProgress = pr;
            if (cancelAfter >= 0 && cancelledAt < 0.0 && juce::Time::getMillisecondCounterHiRes() - t0 >= cancelAfter)
            {
                cancelledAt = juce::Time::getMillisecondCounterHiRes();
                runner.cancel();
            }
        }
        juce::String text;
        const auto an = runner.getAnalysis();
        text << "bpm=" << (an.valid ? juce::String (an.bpm, 2) : juce::String ("none")) << "\n";
        text << "progress_went_backwards=" << (progressWentBack ? "yes" : "no") << "\n";
        if (cancelledAt >= 0.0)
        {
            text << "stopped_ms_after_cancel=" << juce::String (juce::Time::getMillisecondCounterHiRes() - cancelledAt, 0) << "\n";
            int left = 0;
            for (const auto& f : juce::File::getSpecialLocation (juce::File::tempDirectory).findChildFiles (juce::File::findFiles, false, "ELEK_*.f32")) { juce::ignoreUnused (f); ++left; }
            text << "temp_files_left=" << left << "\n";
        }
        text << "success=" << (runner.isFinishedSuccessfully() ? "yes" : "no") << "\nerror=" << runner.getErrorMessage()
             << "\nfolder=" << runner.getOutputFolder().getFullPathName() << "\n";
        if (runner.getOutputFolder().isDirectory())
            for (const auto& f : runner.getOutputFolder().findChildFiles (juce::File::findFiles, false, "*", juce::File::FollowSymlinks::no))
                text << "file=" << f.getFileName() << "\n";
        result.replaceWithText (text);
        std::cout << text;
        return runner.isFinishedSuccessfully() ? 0 : 1;
    }

    // TEST ONLY (ELEK 2.0.0 window): "--view-run <run folder> <out.png> [open]" shows a finished run as the window does (any operation);
    // "--split-click <run folder> <out.png>" opens it, presses "Split the drums", waits for the engine, snapshots the result;
    // "--ready-region <file> <from s> <to s> <out.png> [option]" loads a file, picks an option and a region, snapshots.
    {
        const juce::String m (argv[1]);
        const bool viewRun = m == "--view-run" && argc >= 4, splitClick = m == "--split-click" && argc >= 4, readyRegion = m == "--ready-region" && argc >= 6;
        if (m == "--howto" && argc >= 5)
        {
            // "--howto <audio> <out dir> <bpm>": the how-to video's scenes (20 Nov 2026), shot through the real editor and engine.
            // Each scene is a folder of 2x JPEGs at 30 fps; scenes.json lists them with the controls' positions (editor px) and
            // what plays, so the composer can fit each scene to its narration and draw the cursor on the real keys.
            using Ed = SubverseSplitterAudioProcessorEditor;
            const double bpm = juce::String (argv[4]).getDoubleValue(), bar = 240.0 / bpm, fps = 30.0;
            const juce::File in { juce::String::fromUTF8 (argv[2]) }, out { juce::String::fromUTF8 (argv[3]) };
            out.createDirectory();
            auto pumpFor = [] (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); };
            SubverseSplitterAudioProcessor processor;
            std::unique_ptr<Ed> editor (dynamic_cast<Ed*> (processor.createEditor()));
            processor.prepareToPlay (44100.0, 512);
            editor->setSize (Ed::kW, Ed::kH);
            juce::Array<juce::var> scenes;
            juce::JPEGImageFormat jpg; jpg.setQuality (0.92f);
            auto controls = [&]
            {
                juce::var c (new juce::DynamicObject());
                for (auto n : { "run", "play", "stop", "newfile", "split", "history", "home", "changefile", "tile0", "tile1", "tile2", "tile3" })
                {
                    const auto r = editor->controlBoundsForTest (n);
                    if (! r.isEmpty()) { juce::Array<juce::var> a; a.add (r.getX()); a.add (r.getY()); a.add (r.getWidth()); a.add (r.getHeight()); c.getDynamicObject()->setProperty (n, a); }
                }
                return c;
            };
            int sceneFrames = 0; juce::File sceneDir;
            juce::AudioBuffer<float> sceneAudio (2, 1); int sceneSamples = 0;     // what processBlock put out during the scene (playFor)
            auto begin = [&] (const char* name) { sceneDir = out.getChildFile (name); sceneDir.deleteRecursively(); sceneDir.createDirectory(); sceneFrames = 0; sceneSamples = 0; };
            auto shot = [&]
            {
                const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
                const auto f = sceneDir.getChildFile (juce::String::formatted ("f%05d.jpg", sceneFrames++));
                f.deleteFile();
                if (auto st = f.createOutputStream()) jpg.writeImageToStream (image, *st);
            };
            auto end = [&] (const char* name, const juce::String& note, juce::var extra = {})
            {
                juce::var o (new juce::DynamicObject());
                o.getDynamicObject()->setProperty ("name", name);
                o.getDynamicObject()->setProperty ("frames", sceneFrames);
                o.getDynamicObject()->setProperty ("note", note);
                o.getDynamicObject()->setProperty ("controls", controls());
                if (sceneSamples > 0)
                {
                    juce::WavAudioFormat wav;
                    const auto f = sceneDir.getChildFile ("audio.wav");
                    f.deleteFile();
                    if (auto st = f.createOutputStream())
                        if (auto w = std::unique_ptr<juce::AudioFormatWriter> (wav.createWriterFor (st.get(), 44100.0, 2, 24, {}, 0)))
                        {
                            st.release();
                            w->writeFromAudioSampleBuffer (sceneAudio, 0, sceneSamples);
                        }
                    o.getDynamicObject()->setProperty ("audio", sceneSamples);
                }
                if (! extra.isVoid()) o.getDynamicObject()->setProperty ("extra", extra);
                scenes.add (o);
                std::cout << "  " << name << ": " << sceneFrames << " frames\n";
            };
            // plays `seconds` of real time from where the playhead is, one frame per 1/30 s, the engine's blocks in between
            auto playFor = [&] (double seconds, std::function<void (double)> each = nullptr)
            {
                juce::AudioBuffer<float> block (2, 512); juce::MidiBuffer midi;
                const int frames = (int) std::lround (seconds * fps);
                double t = 0.0;
                for (int k = 0; k < frames; ++k)
                {
                    if (each) each (k / fps);
                    while (t < (k + 1) / fps)
                    {
                        block.clear(); processor.processBlock (block, midi); t += 512.0 / 44100.0;
                        if (sceneAudio.getNumSamples() < sceneSamples + 512) sceneAudio.setSize (2, sceneSamples + 44100 * 4, true, true);
                        for (int ch = 0; ch < 2; ++ch) sceneAudio.copyFrom (ch, sceneSamples, block, ch, 0, 512);
                        sceneSamples += 512;
                    }
                    pumpFor (2);
                    shot();
                }
            };

            begin ("empty"); shot(); end ("empty", "start screen");
            editor->filesDropped (juce::StringArray (in.getFullPathName()), 0, 0);
            editor->selectOperation (-1);
            pumpFor (4000);
            begin ("loaded"); shot(); end ("loaded", editor->loadedAnalysisForTest());
            const double len = processor.getTotalLength(), ra = bar, rb = juce::jmin (len, 7.0 * bar);
            // listening: Play from the start, whole track, 6 s
            begin ("play"); editor->pressPlayForTest(); playFor (6.0); end ("play", "playing the whole track");
            // the markers come in while it plays (3 s), then the playhead goes to the new start (as the real gesture does)
            begin ("markers");
            playFor (3.0, [&] (double t) { const double u = juce::jlimit (0.0, 1.0, t / 2.4), e = 1.0 - std::pow (1.0 - u, 3.0);
                                            editor->setRegionForTest (ra * e, len + (rb - len) * e); });
            processor.setPlaybackPosition (ra);
            playFor (3.0);
            end ("markers", juce::String (ra, 3) + "-" + juce::String (rb, 3));
            // the end marker moved in a bar while it plays: the playhead jumps to 3 s before the new end and plays the cut
            begin ("cut");
            const double rb2 = rb - bar;
            playFor (1.5, [&] (double t) { const double u = juce::jlimit (0.0, 1.0, t / 1.2); editor->setRegionForTest (ra, rb + (rb2 - rb) * u); });
            processor.setPlaybackPosition (juce::jmax (ra, rb2 - 3.0));
            playFor (4.0);
            end ("cut", "end marker moved to " + juce::String (rb2, 3));
            begin ("stop"); editor->pressStopForTest(); pumpFor (100); shot(); end ("stop", "stopped, back at the part's start");
            editor->setRegionForTest (ra, rb);      // the part used for the run: bars 2-7
            // the four tiles, one after the other, then 4-Stem Split chosen
            for (int k = 0; k < 4; ++k) { editor->selectOperation (k); pumpFor (40); begin ((juce::String ("tile") + juce::String (k)).toRawUTF8()); shot(); end ((juce::String ("tile") + juce::String (k)).toRawUTF8(), "tile"); }
            editor->selectOperation (1); pumpFor (40);
            begin ("ready4"); shot(); end ("ready4", "4-Stem Split chosen, Run");
            // the run, real; a frame every 0.25 s
            begin ("running");
            editor->pressRun();
            {
                auto& runner = processor.getModelRunner();
                pumpFor (100);
                const double t0 = juce::Time::getMillisecondCounterHiRes();
                while (runner.isRunning()) { pumpFor (250); if (runner.isRunning()) shot(); }
                pumpFor (600);
                end ("running", juce::String ((juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0, 1) + " s real (frames every 0.25 s)");
            }
            const auto runFolder = processor.getModelRunner().getOutputFolder();
            pumpFor (500);
            // the result: the stems, then Drums, Bass, Other soloed, 6 s each from the start
            const char* soloNames[4] = { "result", "solo_drums", "solo_bass", "solo_other" };
            const int soloLane[4] = { -1, 1, 2, 3 };
            for (int k = 0; k < 4; ++k)
            {
                for (int i = 0; i < 4; ++i) processor.setStemSolo (i, i == soloLane[k]);
                processor.setPlaybackPosition (0.0);
                if (! processor.getIsPlaying()) processor.togglePlayPause();
                begin (soloNames[k]); playFor (6.0); end (soloNames[k], "from 0:00");
            }
            for (int i = 0; i < 4; ++i) processor.setStemSolo (i, false);
            if (processor.getIsPlaying()) processor.togglePlayPause();
            processor.setPlaybackPosition (0.0); pumpFor (200);
            begin ("press"); shot(); end ("press", "Split the drums on screen");
            begin ("split");
            editor->pressSplitDrums();
            {
                auto& runner = processor.getModelRunner();
                pumpFor (100);
                while (runner.isRunning()) { pumpFor (250); if (runner.isRunning()) shot(); }
                pumpFor (800);
            }
            end ("split", "drum split, frames every 0.25 s");
            const char* partNames[3] = { "part_kick", "part_snare", "part_hats" };
            for (int k = 0; k < 3; ++k)
            {
                for (int i = 6; i < 10; ++i) processor.setStemSolo (i, i == 6 + k);
                processor.setPlaybackPosition (0.0);
                if (! processor.getIsPlaying()) processor.togglePlayPause();
                begin (partNames[k]); playFor (6.0); end (partNames[k], "from 0:00");
            }
            for (int i = 6; i < 10; ++i) processor.setStemSolo (i, false);
            if (processor.getIsPlaying()) processor.togglePlayPause();
            processor.setPlaybackPosition (0.0); pumpFor (200);
            begin ("final"); shot(); end ("final", "result with parts: drag, History, New file");

            juce::var log (new juce::DynamicObject());
            log.getDynamicObject()->setProperty ("bpm", bpm);
            log.getDynamicObject()->setProperty ("input", in.getFullPathName());
            log.getDynamicObject()->setProperty ("run", runFolder.getFullPathName());
            log.getDynamicObject()->setProperty ("region", juce::String (ra, 6) + "-" + juce::String (rb, 6));
            log.getDynamicObject()->setProperty ("scenes", scenes);
            out.getChildFile ("scenes.json").replaceWithText (juce::JSON::toString (log));
            std::cout << "  run folder " << runFolder.getFullPathName() << "\n";
            return 0;
        }
        if (m == "--film" && argc >= 5)
        {
            // "--film <audio> <out dir> <bpm>": the promo film's UI, shot through the real editor and engine (20 Nov). Frames are
            // 2x PNGs named by film frame (30 fps); film.json says what each section is and where the run's files are, so the
            // composer builds the sound from the real stems. Timeline in bars of <bpm> (see docs/film/SCRIPT.md).
            using Ed = SubverseSplitterAudioProcessorEditor;
            const double bpm = juce::String (argv[4]).getDoubleValue(), bar = 240.0 / bpm, fps = 30.0;
            const juce::File in { juce::String::fromUTF8 (argv[2]) }, out { juce::String::fromUTF8 (argv[3]) };
            out.createDirectory();
            auto pumpFor = [] (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); };
            SubverseSplitterAudioProcessor processor;
            std::unique_ptr<Ed> editor (dynamic_cast<Ed*> (processor.createEditor()));
            processor.prepareToPlay (44100.0, 512);
            editor->setSize (Ed::kW, Ed::kH);
            juce::var log (new juce::DynamicObject());
            juce::Array<juce::var> sections;
            auto frameAt = [&] (double t) { return juce::roundToInt (t * fps); };
            auto shoot = [&] (int frame)
            {
                const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
                juce::PNGImageFormat png;
                const auto f = out.getChildFile (juce::String::formatted ("f%05d.png", frame));
                f.deleteFile();
                if (auto st = f.createOutputStream()) png.writeImageToStream (image, *st);
            };
            auto hold = [&] (double fromBar, double toBar)          // one picture held over a span: written once per frame (cheap: same image)
            {
                const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
                juce::PNGImageFormat png;
                juce::MemoryOutputStream mem; png.writeImageToStream (image, mem);
                for (int fr = frameAt (fromBar * bar); fr < frameAt (toBar * bar); ++fr)
                    out.getChildFile (juce::String::formatted ("f%05d.png", fr)).replaceWithData (mem.getData(), mem.getDataSize());
            };
            auto section = [&] (const char* name, double fromBar, double toBar, juce::var extra = {})
            {
                juce::var o (new juce::DynamicObject());
                o.getDynamicObject()->setProperty ("name", name);
                o.getDynamicObject()->setProperty ("from", fromBar * bar);
                o.getDynamicObject()->setProperty ("to", toBar * bar);
                if (! extra.isVoid()) o.getDynamicObject()->setProperty ("extra", extra);
                sections.add (o);
                std::cout << "  " << name << " " << fromBar << "-" << toBar << "\n";
            };
            auto waitRunner = [&] (std::function<void (double)> each)
            {
                auto& runner = processor.getModelRunner();
                const double t0 = juce::Time::getMillisecondCounterHiRes();
                pumpFor (100);
                while (runner.isRunning()) { pumpFor (250); if (each) each ((juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0); }
                pumpFor (600);
                return (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
            };

            // bars 2-3: the empty window, then the track dropped in (tempo and key measured on load)
            hold (2.0, 3.0); section ("empty", 2.0, 3.0);
            editor->filesDropped (juce::StringArray (in.getFullPathName()), 0, 0);
            editor->selectOperation (-1);
            pumpFor (4000);
            hold (3.0, 4.0); section ("loaded", 3.0, 4.0, juce::var (editor->loadedAnalysisForTest()));
            // bars 4-5: the markers come in to bars 2-7 of the loop (one bar each edge), drawn every frame
            const double len = processor.getTotalLength(), ra = bar, rb = juce::jmin (len, 7.0 * bar);
            for (int fr = frameAt (4.0 * bar); fr < frameAt (6.0 * bar); ++fr)
            {
                const double u = juce::jlimit (0.0, 1.0, (fr / fps - 4.0 * bar) / (1.5 * bar));
                const double e = 1.0 - std::pow (1.0 - u, 3.0);
                editor->setRegionForTest (ra * e, len + (rb - len) * e);
                processor.setPlaybackPosition (ra * e);
                shoot (fr);
            }
            section ("region", 4.0, 6.0, juce::var (juce::String (ra, 3) + "-" + juce::String (rb, 3)));
            // bars 6-8: the four tiles, half a bar each, then 4-Stem Split
            for (int k = 0; k < 4; ++k) { editor->selectOperation (k); pumpFor (30); hold (6.0 + 0.5 * k, 6.5 + 0.5 * k); }
            editor->selectOperation (1); pumpFor (30); hold (8.0, 8.5);
            section ("tiles", 6.0, 8.5);
            // bars 8.5-10: the run, real, compressed into 1.5 bars
            editor->pressRun();
            std::vector<juce::MemoryBlock> runShots;
            const double runSecs = waitRunner ([&] (double) {
                if (processor.getModelRunner().isRunning())
                {
                    const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
                    juce::PNGImageFormat png; juce::MemoryOutputStream mem; png.writeImageToStream (image, mem);
                    runShots.push_back (mem.getMemoryBlock());
                }
            });
            {
                const int a = frameAt (8.5 * bar), b = frameAt (10.0 * bar);
                for (int fr = a; fr < b && ! runShots.empty(); ++fr)
                {
                    const auto& mb = runShots[(size_t) juce::jmin ((int) runShots.size() - 1, (int) ((fr - a) * (double) runShots.size() / (b - a)))];
                    out.getChildFile (juce::String::formatted ("f%05d.png", fr)).replaceWithData (mb.getData(), mb.getSize());
                }
            }
            const auto runFolder = processor.getModelRunner().getOutputFolder();
            section ("run", 8.5, 10.0, juce::var (juce::String (runSecs, 1) + " s real; " + runFolder.getFullPathName()));
            // bars 10-14: the result playing; solo none, Drums, Bass, Other (lanes 0 Vocals, 1 Drums, 2 Bass, 3 Other)
            pumpFor (500);
            const int soloAt[4] = { -1, 1, 2, 3 };
            for (int fr = frameAt (10.0 * bar); fr < frameAt (14.0 * bar); ++fr)
            {
                const double t = fr / fps - 10.0 * bar;
                const int want = soloAt[juce::jlimit (0, 3, (int) (t / bar))];
                for (int i = 0; i < 4; ++i) processor.setStemSolo (i, i == want);
                processor.setPlaybackPosition (t);
                pumpFor (5);
                shoot (fr);
            }
            for (int i = 0; i < 4; ++i) processor.setStemSolo (i, false);
            section ("result", 10.0, 14.0, juce::var (runFolder.getFullPathName()));
            // bar 14: the result with Split the drums on screen (the press); 20 Nov: the split itself then runs over bars 15-17
            processor.setPlaybackPosition (0.0);
            pumpFor (100);
            hold (14.0, 15.0); section ("press", 14.0, 15.0);
            editor->pressSplitDrums();
            std::vector<juce::MemoryBlock> splitShots;
            const double splitSecs = waitRunner ([&] (double) {
                const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
                juce::PNGImageFormat png; juce::MemoryOutputStream mem; png.writeImageToStream (image, mem);
                splitShots.push_back (mem.getMemoryBlock());
            });
            {
                const int a = frameAt (15.0 * bar), b = frameAt (17.0 * bar);
                for (int fr = a; fr < b && ! splitShots.empty(); ++fr)
                {
                    const auto& mb = splitShots[(size_t) juce::jmin ((int) splitShots.size() - 1, (int) ((fr - a) * (double) splitShots.size() / (b - a)))];
                    out.getChildFile (juce::String::formatted ("f%05d.png", fr)).replaceWithData (mb.getData(), mb.getSize());
                }
            }
            section ("split", 15.0, 17.0, juce::var (juce::String (splitSecs, 1) + " s real"));
            // bars 17-20: the parts; solo Kick, Snare, Hats & cymbals (channels 6, 7, 8)
            pumpFor (500);
            const int partAt[3] = { 6, 7, 8 };
            for (int fr = frameAt (17.0 * bar); fr < frameAt (20.0 * bar); ++fr)
            {
                const double t = fr / fps - 17.0 * bar;
                const int want = partAt[juce::jlimit (0, 2, (int) (t / bar))];
                for (int i = 6; i < 10; ++i) processor.setStemSolo (i, i == want);
                processor.setPlaybackPosition (t);
                pumpFor (5);
                shoot (fr);
            }
            for (int i = 6; i < 10; ++i) processor.setStemSolo (i, false);
            section ("parts", 17.0, 20.0);

            log.getDynamicObject()->setProperty ("bpm", bpm);
            log.getDynamicObject()->setProperty ("bar", bar);
            log.getDynamicObject()->setProperty ("input", in.getFullPathName());
            log.getDynamicObject()->setProperty ("run", runFolder.getFullPathName());
            log.getDynamicObject()->setProperty ("sections", sections);
            out.getChildFile ("film.json").replaceWithText (juce::JSON::toString (log));
            std::cout << "  run folder " << runFolder.getFullPathName() << "\n";
            return 0;
        }
        if (m == "--region-test" && argc >= 4)
        {
            // "--region-test <audio file (20 s or more)> <out.png>": the waveform gestures and the preview, through the real mouse handlers
            using Ed = SubverseSplitterAudioProcessorEditor;
            auto pumpFor = [] (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); };
            SubverseSplitterAudioProcessor processor;
            std::unique_ptr<Ed> editor (dynamic_cast<Ed*> (processor.createEditor()));
            processor.prepareToPlay (44100.0, 512);
            editor->setSize (Ed::kW, Ed::kH);
            editor->filesDropped (juce::StringArray (juce::String::fromUTF8 (argv[2])), 0, 0);
            editor->selectOperation (2);
            pumpFor (4000);
            const double len = processor.getTotalLength();
            auto xAt = [len] (double sec) { return Ed::kTL + juce::roundToInt ((double) Ed::kTW * sec / len); };
            int rc = 0;
            auto check = [&] (const char* what, bool ok, const juce::String& detail)
            {
                std::cout << (ok ? "PASS " : "FAIL ") << what << ": " << detail << "\n";
                if (! ok) rc = 1;
            };
            auto regionIs = [&] (double a, double b)
            {
                const auto r = editor->regionForTest();
                if (r == "none") return false;
                return std::abs (r.upToFirstOccurrenceOf ("-", false, false).getDoubleValue() - a) < 0.15 && std::abs (r.fromFirstOccurrenceOf ("-", false, false).getDoubleValue() - b) < 0.15;
            };
            check ("the loaded file can be played before any separation", len > 19.0, "preview length " + juce::String (len, 2) + " s");
            check ("the markers are there on load, at the start and the end (owner 20 Nov)", regionIs (0.0, len), editor->regionForTest());
            editor->timelineGestureForTest (xAt (5.0), xAt (15.0));
            check ("a drag selects a region", regionIs (5.0, 15.0), editor->regionForTest());
            editor->timelineGestureForTest (xAt (8.0), xAt (8.0));
            check ("a click keeps the region and moves the preview", regionIs (5.0, 15.0) && std::abs (processor.getPlaybackPosition() - 8.0) < 0.15,
                   "region " + editor->regionForTest() + ", preview at " + juce::String (processor.getPlaybackPosition(), 2) + " s");
            editor->timelineGestureForTest (xAt (15.0) + 3, xAt (12.0));
            check ("a drag on the end edge moves that edge only", regionIs (5.0, 12.0), editor->regionForTest());
            check ("moving the end edge puts the preview 3 s before the new end (owner 20 Nov)", std::abs (processor.getPlaybackPosition() - 9.0) < 0.15,
                   "preview at " + juce::String (processor.getPlaybackPosition(), 2) + " s");
            editor->timelineGestureForTest (xAt (5.0) - 2, xAt (6.5));
            check ("a drag on the start edge moves that edge only", regionIs (6.5, 12.0), editor->regionForTest());
            check ("moving the start edge puts the preview on the new start (owner 20 Nov)", std::abs (processor.getPlaybackPosition() - 6.5) < 0.15,
                   "preview at " + juce::String (processor.getPlaybackPosition(), 2) + " s");
            editor->timelineGestureForTest (xAt (17.0), xAt (17.0) + 10);
            check ("a drag of under a second leaves the region as it was", regionIs (6.5, 12.0), editor->regionForTest());

            // Play with a region: starts at its start, is audible, stops at its end and goes back to the start
            editor->timelineGestureForTest (xAt (2.0), xAt (2.0));       // the preview is outside the region (inside it, Play goes on from there)
            editor->pressPlayForTest();
            const double startedAt = processor.getPlaybackPosition();
            juce::AudioBuffer<float> block (2, 512);
            juce::MidiBuffer midi;
            float peak = 0.0f;
            int blocks = 0;
            for (; blocks < 1200 && processor.getIsPlaying(); ++blocks)       // up to 13.9 s of audio; the region is 5.5 s
            {
                block.clear();
                processor.processBlock (block, midi);
                peak = juce::jmax (peak, block.getMagnitude (0, 512));
                if (blocks % 8 == 7) pumpFor (45);                              // the editor's timer watches the region's end
            }
            pumpFor (100);
            check ("Play starts at the region's start and is audible", std::abs (startedAt - 6.5) < 0.15 && peak > 0.01f,
                   "started at " + juce::String (startedAt, 2) + " s, peak " + juce::String (peak, 3));
            check ("Play stops at the region's end and returns to its start", ! processor.getIsPlaying() && std::abs (processor.getPlaybackPosition() - 6.5) < 0.2
                   && blocks * 512.0 / 44100.0 < 7.5,
                   "stopped after " + juce::String (blocks * 512.0 / 44100.0, 2) + " s of audio, preview at " + juce::String (processor.getPlaybackPosition(), 2) + " s");
            {
                const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true);
                juce::PNGImageFormat png;
                const juce::File out { juce::String::fromUTF8 (argv[3]) };
                out.deleteFile();
                if (auto stream = out.createOutputStream()) { png.writeImageToStream (image, *stream); std::cout << "  wrote " << out.getFullPathName() << "\n"; }
            }
            // Stop (owner 20 Nov): in the middle of the region it stops and goes back to the region's start
            check ("the Stop key is shown beside Play, and History is in the header on the home screen", editor->stopVisibleForTest() && editor->historyVisibleForTest(),
                   juce::String ("stop ") + (editor->stopVisibleForTest() ? "shown" : "hidden") + ", history " + (editor->historyVisibleForTest() ? "shown" : "hidden"));
            editor->timelineGestureForTest (xAt (9.0), xAt (9.0));
            editor->pressPlayForTest();
            for (int i = 0; i < 40; ++i) { block.clear(); processor.processBlock (block, midi); }
            const bool wasPlaying = processor.getIsPlaying();
            editor->pressStopForTest();
            pumpFor (100);
            check ("Stop stops and returns to the region's start", wasPlaying && ! processor.getIsPlaying() && std::abs (processor.getPlaybackPosition() - 6.5) < 0.15,
                   juce::String ("was playing ") + (wasPlaying ? "yes" : "no") + ", preview at " + juce::String (processor.getPlaybackPosition(), 2) + " s");
            editor->timelineGestureForTest (xAt (9.0), xAt (9.0), 2);
            check ("a double-click puts the markers back at the start and the end", regionIs (0.0, len), editor->regionForTest());
            // the end of the track (owner 20 Nov): Play turns off by itself and goes back to the start, so one press plays again
            processor.setPlaybackPosition (len - 0.4);
            editor->pressPlayForTest();
            int endBlocks = 0;
            for (; endBlocks < 400 && processor.getIsPlaying(); ++endBlocks)
            {
                block.clear(); processor.processBlock (block, midi);
                if (endBlocks % 8 == 7) pumpFor (45);
            }
            pumpFor (100);
            check ("at the end of the track Play turns off and the playhead goes back to the start", ! processor.getIsPlaying() && processor.getPlaybackPosition() < 0.05,
                   "playing " + juce::String (processor.getIsPlaying() ? "yes" : "no") + " after " + juce::String (endBlocks * 512.0 / 44100.0, 2) + " s, playhead at " + juce::String (processor.getPlaybackPosition(), 2) + " s");
            editor->pressPlayForTest();
            check ("one press plays again from the start", processor.getIsPlaying(), processor.getIsPlaying() ? "playing" : "not playing");
            editor->pressStopForTest();
            return rc;
        }
        const bool nav = m == "--nav" && argc >= 4;       // "--nav <run folder> <out.png>": writes <out>_history.png and <out>_fast.png
        if (viewRun || splitClick || readyRegion || nav)
        {
            auto pumpFor = [] (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); };
            auto snapshot = [] (SubverseSplitterAudioProcessorEditor& ed, const juce::File& out)
            {
                const auto image = ed.createComponentSnapshot (ed.getLocalBounds(), true);
                juce::PNGImageFormat png;
                out.deleteFile();
                if (auto stream = out.createOutputStream()) { png.writeImageToStream (image, *stream); std::cout << "  wrote " << out.getFullPathName() << "  " << image.getWidth() << "x" << image.getHeight() << "\n"; return 0; }
                return 1;
            };
            SubverseSplitterAudioProcessor processor;
            std::unique_ptr<SubverseSplitterAudioProcessorEditor> editor (dynamic_cast<SubverseSplitterAudioProcessorEditor*> (processor.createEditor()));
            processor.prepareToPlay (44100.0, 512);
            editor->setSize (SubverseSplitterAudioProcessorEditor::kW, SubverseSplitterAudioProcessorEditor::kH);
            if (readyRegion)
            {
                editor->filesDropped (juce::StringArray (juce::String::fromUTF8 (argv[2])), 0, 0);
                editor->selectOperation (argc >= 7 ? juce::String (argv[6]).getIntValue() : 2);
                pumpFor (4000);     // the mix peaks are read on a worker; tempo and key are measured in the background
                {
                    // tempo and key must be there without any operation having been chosen to measure them, and the window must
                    // still be on the tiles with Run ready
                    const auto read = editor->loadedAnalysisForTest();
                    const bool ok = read.isNotEmpty() && editor->viewNameForTest() == "home with a file" && editor->runEnabledForTest();
                    std::cout << (ok ? "PASS " : "FAIL ") << "tempo and key read on load: \"" << read << "\", view \"" << editor->viewNameForTest()
                              << "\", Run " << (editor->runEnabledForTest() ? "enabled" : "disabled") << "\n";
                }
                editor->setRegionForTest (juce::String (argv[3]).getDoubleValue(), juce::String (argv[4]).getDoubleValue());
                pumpFor (200);
                return snapshot (*editor, juce::File (juce::String::fromUTF8 (argv[5])));
            }
            const juce::File folder { juce::String::fromUTF8 (argv[2]) }, out { juce::String::fromUTF8 (argv[3]) };
            ElekLibrary::Entry e;
            if (! ElekLibrary::readManifest (folder, e)) { std::cout << "  no elek.json in " << folder.getFullPathName() << "\n"; return 1; }
            editor->showEntryForTest (e, SubverseSplitterAudioProcessorEditor::drumPartsFor (e));
            pumpFor (400);
            std::cout << "  Split the drums key visible: " << (editor->splitDrumsVisibleForTest() ? "yes" : "no") << "\n";
            if (editor->splitDrumsVisibleForTest())
                std::cout << (editor->splitDrumsReceivesClicksForTest() ? "PASS" : "FAIL") << " a click at the centre of Split the drums reaches the key (not the strip under it)\n";
            if (m == "--nav")
            {
                // navigation: result -> History -> Home; FAST on this machine
                int rc = 0;
                auto expect = [&] (const char* what, const juce::String& want)
                {
                    const bool ok = editor->viewNameForTest() == want;
                    std::cout << (ok ? "PASS " : "FAIL ") << what << ": view is \"" << editor->viewNameForTest() << "\"\n";
                    if (! ok) rc = 1;
                };
                expect ("a run is shown", "result");
                {
                    const bool nf = editor->newFileVisibleForTest();
                    std::cout << (nf ? "PASS" : "FAIL") << " the result screen has a New file key (owner 20 Nov)\n";
                    if (! nf) rc = 1;
                }
                editor->pressHistory(); pumpFor (200);
                expect ("History from a result", "history");
                rc |= snapshot (*editor, out.getSiblingFile (out.getFileNameWithoutExtension() + "_history.png"));
                editor->pressHome(); pumpFor (200);
                expect ("Home from History", "home");
                editor->openRecent (0); pumpFor (300);
                editor->pressHome(); pumpFor (200);
                expect ("Home from a result", "home");
                const auto mach = SubverseSplitterAudioProcessor::machine();
                std::cout << "  machine: " << mach.chip << ", " << mach.ramGB << " GB, " << mach.performanceCores << " performance cores of " << mach.cores << "\n";
                editor->pressFast(); pumpFor (200);
                const bool refused = editor->dialogTextForTest().isNotEmpty(), should = ! SubverseSplitterAudioProcessor::fastAvailable();
                std::cout << (refused == should ? "PASS " : "FAIL ") << "FAST on this machine: " << (refused ? "dialog \"" + editor->dialogTextForTest() + "\"" : juce::String ("switched on"))
                          << ", fastMode " << (processor.fastMode.load() ? "on" : "off") << "\n";
                if (refused != should) rc = 1;
                rc |= snapshot (*editor, out.getSiblingFile (out.getFileNameWithoutExtension() + "_fast.png"));
                return rc;
            }
            if (viewRun)
            {
                editor->setDrumGroupOpen (argc >= 5 && juce::String (argv[4]) == "open");
                pumpFor (300);
                return snapshot (*editor, out);
            }
            editor->pressSplitDrums();
            const auto t0 = juce::Time::getMillisecondCounter();
            while (processor.getModelRunner().isRunning() && juce::Time::getMillisecondCounter() - t0 < 240000) pumpFor (200);
            pumpFor (1500);         // the editor's timer sees the end and rebuilds the lanes
            ElekLibrary::Entry after;
            ElekLibrary::readManifest (folder, after);
            for (auto k : { "kick", "snare", "cymbals", "toms" })
                std::cout << "  manifest " << k << ": " << (after.file (k).existsAsFile() ? "file exists" : "missing") << "\n";
            std::cout << "  Split the drums key visible afterwards: " << (editor->splitDrumsVisibleForTest() ? "yes" : "no") << "\n";
            return snapshot (*editor, out);
        }
    }

    // TEST ONLY: "--drumparts-empty <out.png> [open]" and "--drumparts-fake <out.png> [open]" show a finished 6-stem run made of
    // synthetic WAVs, with the drum parts group empty or filled with fake data. Needs no model and no audio file; never shipped.
    if (juce::String (argv[1]).startsWith ("--drumparts") && argc >= 3)
    {
        const bool fake = juce::String (argv[1]) == "--drumparts-fake";
        const bool open = argc >= 4 && juce::String (argv[3]) == "open";
        const juce::File out { juce::String::fromUTF8 (argv[2]) };
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("elek_drumparts_ui_test");
        dir.createDirectory();

        auto writeWav = [&dir] (const juce::String& name, float freq, float pulseHz)
        {
            const auto f = dir.getChildFile (name + ".wav");
            f.deleteFile();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (f), 44100.0, 2, 16, {}, 0));
            const int n = 44100 * 6;
            juce::AudioBuffer<float> b (2, n);
            for (int i = 0; i < n; ++i)
            {
                const float t = (float) i / 44100.0f, env = std::exp (-6.0f * std::fmod (t * pulseHz, 1.0f));
                const float v = 0.6f * env * std::sin (juce::MathConstants<float>::twoPi * freq * t);
                b.setSample (0, i, v); b.setSample (1, i, v);
            }
            w->writeFromAudioSampleBuffer (b, 0, n);
            return f.getFileName();
        };

        ElekLibrary::Entry e;
        e.folder = dir; e.title = "Synthetic test run"; e.operation = "stem_separation_6";
        e.created = juce::Time::getCurrentTime();
        const char* keys[] = { "vocals", "drums", "bass", "guitar", "piano", "other" };
        float freq = 220.0f;
        for (auto* k : keys) { e.files.set (k, writeWav (k, freq, 2.0f)); freq *= 1.3f; }

        std::vector<SubverseSplitterAudioProcessorEditor::DrumPart> parts = SubverseSplitterAudioProcessorEditor::drumPartsFor (e);
        if (fake)
        {
            float f = 60.0f, hz = 1.0f;
            for (auto& part : parts)
            {
                part.file = dir.getChildFile (writeWav (part.key, f, hz));
                part.enabled = true;
                f *= 3.0f; hz += 1.0f;
            }
            parts[3].enabled = false;   // one part missing, to see a mixed state
        }

        SubverseSplitterAudioProcessor processor;
        std::unique_ptr<SubverseSplitterAudioProcessorEditor> editor (
            dynamic_cast<SubverseSplitterAudioProcessorEditor*> (processor.createEditor()));
        processor.prepareToPlay (44100.0, 512);
        editor->setSize (SubverseSplitterAudioProcessorEditor::kW, SubverseSplitterAudioProcessorEditor::kH);
        editor->showEntryForTest (e, parts);
        editor->setDrumGroupOpen (open);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (400);
        const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true);
        juce::PNGImageFormat png;
        out.deleteFile();
        if (auto stream = out.createOutputStream()) { png.writeImageToStream (image, *stream); std::cout << "  wrote " << out.getFullPathName() << "  " << image.getWidth() << "x" << image.getHeight() << "\n"; return 0; }
        return 1;
    }

    // TEST ONLY: "--open-run <run folder> <out.png> [open]" shows a finished run read from its elek.json (as a click on the list
    // would, playback loaded), drum parts group open or closed, and checks the drum parts playback rule through processBlock:
    //   A  Drums row soloed, parts at rest       -> must equal the Drums WAV (parts silent, no doubling)
    //   B  Drums row soloed, Kick part muted     -> must equal Snare + Cymbals + Toms WAVs (parts replace the Drums stem)
    //   C  Kick part soloed                      -> must equal the Kick WAV
    // Prints SDR in dB of each output against its expected signal (higher = closer).
    if (juce::String (argv[1]) == "--open-run" && argc >= 4)
    {
        const juce::File folder { juce::String::fromUTF8 (argv[2]) };
        const juce::File out { juce::String::fromUTF8 (argv[3]) };
        const bool open = argc >= 5 && juce::String (argv[4]) == "open";
        ElekLibrary::Entry e;
        if (! ElekLibrary::readManifest (folder, e)) { std::cout << "  no elek.json in " << folder.getFullPathName() << "\n"; return 1; }

        SubverseSplitterAudioProcessor processor;
        std::unique_ptr<SubverseSplitterAudioProcessorEditor> editor (
            dynamic_cast<SubverseSplitterAudioProcessorEditor*> (processor.createEditor()));
        processor.prepareToPlay (44100.0, 512);
        editor->setSize (SubverseSplitterAudioProcessorEditor::kW, SubverseSplitterAudioProcessorEditor::kH);
        const auto parts = SubverseSplitterAudioProcessorEditor::drumPartsFor (e);
        editor->showEntryForTest (e, parts);
        editor->setDrumGroupOpen (open);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (400);

        for (auto& p : parts)
            std::cout << "  part " << p.key << ": " << (p.enabled ? "audio" : "none") << ", channel " << p.stemIndex << "\n";

        const int drumsCh = 1;   // the Drums row in the 4- and 6-stem layouts (Vocals, Drums, ...)
        constexpr int blocks = 172, bs = 512, skip = 4;      // 2 s; the first blocks carry the transports' gain ramp
        auto render = [&processor] (std::vector<float>& left)
        {
            left.clear();
            processor.setPlaybackPosition (0.0);
            processor.togglePlayPause();
            juce::AudioBuffer<float> block (2, bs);
            juce::MidiBuffer midi;
            for (int i = 0; i < blocks; ++i)
            {
                block.clear();
                processor.processBlock (block, midi);
                left.insert (left.end(), block.getReadPointer (0), block.getReadPointer (0) + bs);
            }
            processor.togglePlayPause();
        };
        auto readLeft = [] (const juce::File& f, std::vector<float>& dst, float gain)
        {
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
            if (r == nullptr) return false;
            juce::AudioBuffer<float> b (2, blocks * bs);
            b.clear();
            r->read (&b, 0, blocks * bs, 0, true, true);
            if (dst.empty()) dst.assign ((size_t) (blocks * bs), 0.0f);
            for (int i = 0; i < blocks * bs; ++i) dst[(size_t) i] += gain * b.getSample (0, i);
            return true;
        };
        auto sdr = [] (const std::vector<float>& ref, const std::vector<float>& est)
        {
            double num = 0.0, den = 0.0;
            for (size_t i = (size_t) (skip * bs); i < ref.size() && i < est.size(); ++i)
            {
                num += (double) ref[i] * ref[i];
                den += ((double) ref[i] - est[i]) * ((double) ref[i] - est[i]);
            }
            return 10.0 * std::log10 ((num + 1e-20) / (den + 1e-20));
        };

        int rc = 0;
        {
            // every main row soloed on its own must play its own WAV (the editor's row order)
            const bool six = e.operation == "stem_separation_6";
            std::vector<const char*> rows = six ? std::vector<const char*> { "vocals", "drums", "bass", "guitar", "piano", "other" }
                                                : std::vector<const char*> { "vocals", "drums", "bass", "other" };
            if (e.operation == "vocal_removal") rows = { "vocals", "instrumental" };
            for (int i = 0; i < (int) rows.size(); ++i)
            {
                std::vector<float> got, want;
                processor.setStemSolo (i, true);
                render (got);
                // 20 Nov: once a run has its drum parts they play in place of the Drums stem
                bool partsHere = juce::String (rows[(size_t) i]) == "drums";
                if (partsHere) { partsHere = false; for (auto& p : parts) partsHere = partsHere || p.enabled; }
                if (partsHere) { for (auto& p : parts) if (p.enabled) readLeft (p.file, want, 1.0f); }
                else readLeft (e.file (rows[(size_t) i]), want, 1.0f);
                processor.setStemSolo (i, false);
                const double s = sdr (want, got);
                std::cout << "  row " << i << " (" << rows[(size_t) i] << ") soloed vs its WAV: " << s << " dB\n";
                if (! (s > 60.0)) rc = 5;
            }
            std::cout << (rc == 0 ? "PASS" : "FAIL") << " every main row plays its own stem\n";
        }
        {
            // the end of a result (owner 20 Nov): Play turns off by itself and the playhead goes back to 0:00
            const double total = processor.getTotalLength();
            processor.setPlaybackPosition (total - 0.4);
            processor.togglePlayPause();
            juce::AudioBuffer<float> block (2, bs);
            juce::MidiBuffer midi;
            int n = 0;
            for (; n < 400 && processor.getIsPlaying(); ++n)
            {
                block.clear(); processor.processBlock (block, midi);
                if (n % 8 == 7) juce::MessageManager::getInstance()->runDispatchLoopUntil (45);
            }
            const bool ok = ! processor.getIsPlaying() && processor.getPlaybackPosition() < 0.05;
            std::cout << (ok ? "PASS" : "FAIL") << " a result at its end turns Play off and goes back to 0:00 (after "
                      << n * (double) bs / 44100.0 << " s, playhead " << processor.getPlaybackPosition() << " s)\n";
            if (processor.getIsPlaying()) processor.togglePlayPause();
            if (! ok) rc = 6;
        }

        bool allParts = parts.size() >= 3;
        for (auto& p : parts) allParts = allParts && p.enabled;
        if (allParts)
        {
            std::vector<float> got, want;
            processor.setStemSolo (drumsCh, true);
            render (got); want.clear(); for (auto& p : parts) readLeft (p.file, want, 1.0f);
            const double a = sdr (want, got);
            processor.setStemMute (parts[0].stemIndex, true);
            render (got); want.clear();
            for (int k = 1; k < (int) parts.size(); ++k) readLeft (parts[(size_t) k].file, want, 1.0f);
            const double b = sdr (want, got);
            processor.setStemMute (parts[0].stemIndex, false);
            processor.setStemSolo (drumsCh, false);
            processor.setStemSolo (parts[0].stemIndex, true);
            render (got); want.clear(); readLeft (parts[0].file, want, 1.0f);
            const double c = sdr (want, got);
            processor.setStemSolo (parts[0].stemIndex, false);
            std::cout << "  rule A (Drums solo: the parts replace the Drums stem) vs the parts' sum: " << a << " dB\n"
                      << "  rule B (Drums solo, Kick muted) vs the other parts: " << b << " dB\n"
                      << "  rule C (Kick solo) vs Kick WAV: " << c << " dB\n";
            const bool pass = a > 60.0 && b > 60.0 && c > 60.0;
            std::cout << (pass ? "PASS" : "FAIL") << " drum parts playback rule\n";
            if (! pass) rc = 4;
        }
        else
            std::cout << "  drum parts: not all four present, playback rule not checked\n";

        const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true);
        juce::PNGImageFormat png;
        out.deleteFile();
        if (auto stream = out.createOutputStream()) { png.writeImageToStream (image, *stream); std::cout << "  wrote " << out.getFullPathName() << "  " << image.getWidth() << "x" << image.getHeight() << "\n"; return rc; }
        return 1;
    }

    // TEST ONLY (owner 20 Nov: "ana panelden drum split yapınca ne oluyor"): "--panel-drumsplit <audio> <out dir>" goes the user's
    // way through the window: drop the file, click the Drum Split tile, press Run, wait, then shoots ready / running / result,
    // lists the run folder and elek.json, and pulls audio through processBlock with each lane soloed.
    if (juce::String (argv[1]) == "--panel-drumsplit" && argc >= 4)
    {
        const juce::File input { juce::String::fromUTF8 (argv[2]) };
        const juce::File outDir { juce::String::fromUTF8 (argv[3]) };
        outDir.createDirectory();
        SubverseSplitterAudioProcessor processor;
        std::unique_ptr<SubverseSplitterAudioProcessorEditor> editor (
            dynamic_cast<SubverseSplitterAudioProcessorEditor*> (processor.createEditor()));
        processor.prepareToPlay (44100.0, 512);
        editor->setSize (SubverseSplitterAudioProcessorEditor::kW, SubverseSplitterAudioProcessorEditor::kH);
        auto pumpMs = [] (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); };
        auto shoot = [&] (const juce::String& name)
        {
            const auto img = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
            const auto f = outDir.getChildFile (name + ".png");
            f.deleteFile();
            juce::PNGImageFormat png;
            if (auto s = f.createOutputStream()) png.writeImageToStream (img, *s);
            std::cout << "  shot " << f.getFileName() << "\n";
        };
        int fails = 0;
        auto check = [&fails] (bool ok, const juce::String& what) { std::cout << (ok ? "PASS " : "FAIL ") << what << "\n"; if (! ok) ++fails; };

        editor->filesDropped (juce::StringArray (input.getFullPathName()), 0, 0);
        pumpMs (1500);
        editor->selectOperation (3);
        pumpMs (300);
        shoot ("1_ready_drumsplit");
        const auto t0 = juce::Time::getMillisecondCounter();
        editor->pressRun();
        pumpMs (300);
        check (processor.getModelRunner().isRunning(), "Run with Drum Split chosen starts a run");
        bool shotRun = false;
        for (int waited = 0; waited < 6000 && processor.getModelRunner().isRunning(); ++waited)
        {
            pumpMs (100);
            if (! shotRun && juce::Time::getMillisecondCounter() - t0 > 4000) { shoot ("2_running"); shotRun = true; }
            if (waited % 50 == 0) std::cout << "    " << juce::roundToInt (processor.getModelRunner().getProgress() * 100.0) << "%\n";
        }
        const double secs = (juce::Time::getMillisecondCounter() - t0) / 1000.0;
        auto& runner = processor.getModelRunner();
        check (runner.isFinishedSuccessfully(), "run finished (" + juce::String (secs, 1) + " s)" + (runner.isFinishedSuccessfully() ? juce::String() : ": " + runner.getErrorMessage()));
        pumpMs (800);
        editor->resized();
        pumpMs (300);
        shoot ("3_result");
        const auto folder = runner.getOutputFolder();
        std::cout << "  run folder: " << folder.getFullPathName() << "\n";
        juce::StringArray names;
        for (auto& f : folder.findChildFiles (juce::File::findFiles, false))
            names.add (f.getFileName() + "  (" + juce::String (f.getSize() / 1024) + " kB)");
        names.sort (true);
        for (auto& n : names) std::cout << "    " << n << "\n";
        const auto has = [&folder] (const juce::String& part) { return folder.findChildFiles (juce::File::findFiles, false, "*_" + part + "_*.wav").size() == 1; };
        check (has ("Drums") && has ("Kick") && has ("Snare") && has ("Cymbals"), "Drums + Kick + Snare + Cymbals WAVs written");
        check (! has ("Vocals") && ! has ("Bass") && ! has ("Other") && ! has ("Instrumental") && ! has ("Toms"), "no other stems, no Toms");
        ElekLibrary::Entry e;
        const bool man = ElekLibrary::readManifest (folder, e);
        check (man && e.operation == "drums_only", "elek.json operation = " + (man ? e.operation : juce::String ("(none)")));
        processor.togglePlayPause();
        pumpMs (200);
        juce::AudioBuffer<float> block (2, 512);
        juce::MidiBuffer midi;
        float loudest = 0.0f;
        for (int i = 0; i < 200; ++i) { block.clear(); processor.processBlock (block, midi); loudest = juce::jmax (loudest, block.getMagnitude (0, 512)); }
        processor.togglePlayPause();
        check (loudest > 0.001f, "playback after the run is audible, peak " + juce::String (juce::Decibels::gainToDecibels (loudest), 1) + " dBFS");
        std::cout << (fails == 0 ? "ALL PASS" : juce::String (fails) + " FAIL") << "\n";
        return fails == 0 ? 0 : 4;
    }

    // modes taking <file> <out.png>: --during, --ready (file loaded, Stem separation chosen, nothing run), --vocal, --keybpm
    const juce::String mode (argv[1]);
    // "--during6" = "--during" with 6 stems. TEST ONLY: ELEK_TEST_DURING_MS="8000,16000" takes one snapshot per time (ms since
    // the run started; the second and later ones are written as <out>_2.png ...); default one snapshot at 6000 ms.
    const bool duringMode = mode == "--during" || mode == "--during6";
    const bool twoArgMode = duringMode || mode == "--ready" || mode == "--vocal" || mode == "--keybpm" || mode == "--locked" || mode == "--six";
    const juce::File input { juce::String::fromUTF8 (twoArgMode ? argv[2] : argv[1]) };   // UTF-8: Turkish file names
    const juce::File output { juce::String::fromUTF8 (twoArgMode ? argv[3] : argv[2]) };

    SubverseSplitterAudioProcessor processor;
    std::unique_ptr<SubverseSplitterAudioProcessorEditor> editor (
        dynamic_cast<SubverseSplitterAudioProcessorEditor*> (processor.createEditor()));

    if (editor == nullptr)
    {
        std::cout << "  editor could not be created\n";
        return 1;
    }

    // The editor builds its stem panels from a timerCallback, so the message
    // loop has to actually run — this is not a sleep with extra steps.
    auto pump = [] (int milliseconds)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (milliseconds);
    };

    // "--empty" snapshots the state before any file is loaded, which is the
    // other half of the layout and the one a first-time user sees.
    const bool separate = mode != "--empty" && mode != "--ready" && mode != "--reopen" && mode != "--locked";

    if (mode == "--locked")      // no licence: Run Stem separation must open the licence card, not separate
    {
        editor->filesDropped (juce::StringArray (input.getFullPathName()), 0, 0);
        editor->selectOperation (1);
        editor->pressRun();
        pump (300);
        std::cout << "  separation started without a licence: " << (processor.getModelRunner().isRunning() ? "YES (bug)" : "no") << "\n";
    }

    if (mode == "--reopen")      // the newest earlier run from ~/Music/ELEK, opened as a click on the list would
    {
        pump (300);
        editor->openRecent (0);
    }

    if (mode == "--ready")     // optional 4th argument: the option to select (0..3), default 2
    {
        editor->filesDropped (juce::StringArray (input.getFullPathName()), 0, 0);
        editor->selectOperation (argc >= 5 ? juce::String (argv[4]).getIntValue() : 1);
    }
    if (mode == "--empty" && argc >= 4)   // optional 3rd argument: the option to select on the start screen (0..3)
        editor->selectOperation (juce::String (argv[3]).getIntValue());

    // Prepared before anything is loaded, exactly as the Standalone wrapper
    // does it: the audio device opens when the window appears, long before a
    // file has been separated. Preparing afterwards — as this test used to —
    // hides any bug that depends on that order.
    processor.prepareToPlay (44100.0, 512);

    if (separate)
    {
        std::cout << "  separating " << input.getFileName() << " ...\n";
        if (const char* f = std::getenv ("ELEK_TEST_FAST"); f != nullptr && *f == '1') processor.fastMode = true;   // TEST: FAST on (no 16 GB here)
        using Op = SubverseSplitterAudioProcessor::ModelRunner::Operation;
        processor.getModelRunner().startOperation (input, mode == "--vocal" ? Op::VocalRemoval : mode == "--keybpm" ? Op::Analysis
                                                          : (mode == "--six" || mode == "--during6") ? Op::Stems6 : Op::Stems);
    }

    // "--during <file>" captures the screen a separation shows while it runs,
    // which is a different layout from both the empty state and the finished
    // one, and the only one a person looks at for minutes at a time.
    const bool captureDuring = duringMode;
    if (captureDuring)
    {
        juce::StringArray times;
        times.addTokens (std::getenv ("ELEK_TEST_DURING_MS") ? std::getenv ("ELEK_TEST_DURING_MS") : "6000", ",", "");
        const auto t0 = juce::Time::getMillisecondCounter();
        editor->setSize (SubverseSplitterAudioProcessorEditor::kW, SubverseSplitterAudioProcessorEditor::kH);
        for (int k = 0; k < times.size(); ++k)
        {
            const int at = times[k].getIntValue();
            while ((int) (juce::Time::getMillisecondCounter() - t0) < at && processor.getModelRunner().isRunning())
                pump (100);
            pump (100);
            const auto midImage = editor->createComponentSnapshot (editor->getLocalBounds(), true);
            const auto shot = k == 0 ? output : output.getSiblingFile (output.getFileNameWithoutExtension() + "_" + juce::String (k + 1) + ".png");
            juce::PNGImageFormat midPng;
            shot.deleteFile();
            if (auto stream = shot.createOutputStream())
            {
                midPng.writeImageToStream (midImage, *stream);
                std::cout << "  wrote mid-separation snapshot " << shot.getFullPathName() << " at "
                          << (int) (juce::Time::getMillisecondCounter() - t0) << " ms, engine progress "
                          << processor.getModelRunner().getProgress() << ", running " << (processor.getModelRunner().isRunning() ? "yes" : "no") << "\n";
            }
        }
        return 0;
    }

    for (int waited = 0; separate && waited < 600; ++waited)     // up to ten minutes
    {
        pump (1000);
        auto& runner = processor.getModelRunner();
        if (! runner.isRunning())
        {
            if (! runner.isFinishedSuccessfully())
            {
                std::cout << "  separation failed: " << runner.getErrorMessage() << "\n";
                return 1;
            }
            break;
        }
        if (waited % 10 == 0)
            std::cout << "    " << juce::roundToInt (runner.getProgress() * 100.0) << "%\n";
    }

    pump (500);                                       // let the panels appear
    editor->setSize (SubverseSplitterAudioProcessorEditor::kW, SubverseSplitterAudioProcessorEditor::kH);
    editor->resized();
    pump (300);

    // Does it actually make a sound?
    //
    // The stem files can be perfectly good and the mixer still silent, and the
    // two failures look identical from the outside. This pulls blocks through
    // processBlock exactly as the host would and measures what comes out.
    if (separate || mode == "--reopen")
    {
        processor.togglePlayPause();
        pump (200);

        juce::AudioBuffer<float> block (2, 512);
        juce::MidiBuffer midi;
        float loudest = 0.0f;
        for (int i = 0; i < 200; ++i)                 // about 2.3 seconds
        {
            block.clear();
            processor.processBlock (block, midi);
            loudest = juce::jmax (loudest, block.getMagnitude (0, block.getNumSamples()));
        }
        std::cout << "  playback peak over 2.3 s: " << loudest
                  << (loudest > 0.001f ? "   AUDIBLE" : "   SILENT") << "\n";
        processor.togglePlayPause();
    }

    // ELEK_SNAPSHOT_SCALE=2 renders at twice the size (covers, store images)
    const float snapScale = std::getenv ("ELEK_SNAPSHOT_SCALE") ? (float) std::atof (std::getenv ("ELEK_SNAPSHOT_SCALE")) : 1.0f;
    const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, snapScale);
    juce::PNGImageFormat png;
    output.deleteFile();

    if (auto stream = output.createOutputStream())
    {
        png.writeImageToStream (image, *stream);
        std::cout << "  wrote " << output.getFullPathName()
                  << "  " << image.getWidth() << "x" << image.getHeight() << "\n";
        // UI signature 3 (14 Nov 2026): the icon bottom right opens the manual, and nothing covers it.
        juce::String why;
        const bool linkOk = svl::markLinkReachable (*editor, "https://subverselab.com/plugins/elek/manual", why);
        std::cout << (linkOk ? "PASS" : "FAIL") << " marklink: " << why << "\n";
        return linkOk ? 0 : 3;
    }

    std::cout << "  could not write " << output.getFullPathName() << "\n";
    return 1;
}
