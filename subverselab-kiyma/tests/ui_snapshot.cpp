/*  Draws the editor into a PNG without a screen, deterministically, so the layout can be checked without
    Screen Recording permission.

        ui_snapshot --out x.png [--crank 0..1] [--t seconds] [--fake-audio] [--glitch v] [--brutal v] [--mix v]
                    [--spin] [--scale 1.0] [--hover glitch|brutal|...|seal] [--crop x,y,w,h] [--bench] [--preset N] [--trip]

    --t steps the animation in 1/60 s frames from a fresh editor; --fake-audio feeds a synthetic signal into the
    visual FIFO (and counts as audio passing); --spin turns the crank's own visual spin on (off by default so a
    crank angle in a PNG is the parameter's angle). A bare first argument is taken as the output path.
*/
#include <BinaryData.h>
#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"

int main (int argc, char** argv)
{
    juce::String out;
    double t = 0.0, scale = 1.0;
    bool fake = false, spin = false, bench = false;
    juce::String crop;
    bool benchLive = false, checkAssets = false;
    int preset = -1;
    bool trip = false;
    juce::String hover;
    std::vector<std::pair<const char*, float>> params;
    for (int i = 1; i < argc; ++i)
    {
        const juce::String a (argv[i]);
        auto next = [&] { return i + 1 < argc ? juce::String (argv[++i]) : juce::String(); };
        if (a == "--out") out = next();
        else if (a == "--t") t = next().getDoubleValue();
        else if (a == "--scale") scale = next().getDoubleValue();
        else if (a == "--fake-audio") fake = true;
        else if (a == "--spin") spin = true;
        else if (a == "--hover") hover = next();
        else if (a == "--crop") crop = next();
        else if (a == "--bench") bench = true;
        else if (a == "--bench-live") benchLive = true;
        else if (a == "--check-assets") checkAssets = true;
        else if (a == "--preset") preset = next().getIntValue();
        else if (a == "--trip") trip = true;
        else if (a == "--crank") params.push_back ({ kiymaIds::crank, next().getFloatValue() });
        else if (a == "--glitch") params.push_back ({ kiymaIds::glitch, next().getFloatValue() });
        else if (a == "--brutal") params.push_back ({ kiymaIds::brutal, next().getFloatValue() });
        else if (a == "--mix") params.push_back ({ kiymaIds::mix, next().getFloatValue() });
        else if (! a.startsWith ("--") && out.isEmpty()) out = a;
    }
    if (out.isEmpty() && ! bench && ! benchLive && ! checkAssets)
        return 1;

    juce::ScopedJuceInitialiser_GUI init;
    KiymaProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    for (const auto& p : params)
    {
        auto* par = processor.apvts.getParameter (p.first);
        par->setValueNotifyingHost (par->convertTo0to1 (p.second));
    }
    if (preset >= 0)
        processor.recallPreset (preset);   // raises the switch and writes its values
    if (checkAssets)
    {
        // the cached art must come out at full opacity: a 2x snapshot (the seal is drawn 1:1 from its 464 px asset) against the assets themselves
        KiymaProcessor proc;
        proc.prepareToPlay (48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
        auto* e = static_cast<KiymaEditor*> (ed.get());
        e->advance (0.0);
        const auto snap = e->createComponentSnapshot (e->getLocalBounds(), true, 2.0f);
        auto load = [] (const void* d, size_t n) { return juce::PNGImageFormat::loadFrom (d, n); };
        const auto seal = load (BinaryData::seal2x_png, (size_t) BinaryData::seal2x_pngSize);
        int fails = 0;
        const auto sc = snap.getPixelAt (80 + seal.getWidth() / 2, 80 + seal.getHeight() / 2), ac = seal.getPixelAt (seal.getWidth() / 2, seal.getHeight() / 2);
        const int dr = std::abs ((int) sc.getRed() - (int) ac.getRed()), dg = std::abs ((int) sc.getGreen() - (int) ac.getGreen()), db = std::abs ((int) sc.getBlue() - (int) ac.getBlue());
        const bool sealOk = dr <= 6 && dg <= 6 && db <= 6;
        std::printf ("%s seal centre pixel: snapshot (%d %d %d), asset (%d %d %d), difference (%d %d %d), limit 6\n", sealOk ? "PASS" : "FAIL", sc.getRed(), sc.getGreen(), sc.getBlue(), ac.getRed(), ac.getGreen(), ac.getBlue(), dr, dg, db);
        fails += sealOk ? 0 : 1;
        // the same for the mean brightness of the seal's opaque pixels (a faded seal reads about 0.6)
        double sumS = 0.0, sumA = 0.0;
        long n = 0;
        for (int y = 0; y < seal.getHeight(); y += 2)
            for (int x = 0; x < seal.getWidth(); x += 2)
            {
                const auto a = seal.getPixelAt (x, y);
                if (a.getAlpha() < 250) continue;
                const auto b = snap.getPixelAt (80 + x, 80 + y);
                sumS += b.getRed() + b.getGreen() + b.getBlue();
                sumA += a.getRed() + a.getGreen() + a.getBlue();
                ++n;
            }
        const double ratio = sumS / std::max (1.0, sumA);
        const bool ratioOk = ratio > 0.95 && ratio < 1.08;
        std::printf ("%s seal brightness, snapshot over asset, over %ld pixels: %.3f (limit 0.95 to 1.08, the sheen and the rim light are part of the design; a faded seal reads about 0.6)\n", ratioOk ? "PASS" : "FAIL", n, ratio);
        fails += ratioOk ? 0 : 1;
        return fails == 0 ? 0 : 1;
    }
    if (benchLive)
    {
        // the real tick loop (30 Hz cap, dirty regions only), 300 ticks, a fresh editor per run
        for (bool silent : { false, true })
            for (float scale : { 1.0f, 2.0f })
            {
                KiymaProcessor proc;
                proc.prepareToPlay (48000.0, 512);
                for (const auto& p : params)
                {
                    auto* par = proc.apvts.getParameter (p.first);
                    par->setValueNotifyingHost (par->convertTo0to1 (p.second));
                }
                std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
                const auto r = static_cast<KiymaEditor*> (ed.get())->benchLive (scale, 300, silent);
                std::printf ("bench-live %.0fx %s: %d ticks at 30 Hz, painted %d (%.1f frames/s), mean %.2f ms p95 %.2f ms per painted tick, main-thread share %.1f %%, painted after 0.5 s: %d, after 1.5 s: %d\n",
                             scale, silent ? "idle (silence)" : "animating (fake audio)", r.frames, r.painted, r.paintedPerSecond, r.meanMs, r.p95Ms, r.sharePercent, r.paintedAfter05, r.paintedAfter15);
            }
        return 0;
    }
    std::unique_ptr<juce::AudioProcessorEditor> base (processor.createEditor());
    auto* editor = static_cast<KiymaEditor*> (base.get());
    if (bench)
    {
        editor->setFakeAudio (true);
        for (float scale : { 1.0f, 2.0f })
        {
            double mean = 0, p95 = 0, build = 0;
            editor->bench (scale, 120, mean, p95, build);
            std::printf ("bench %.0fx: %d x %d px, 120 frames, mean %.2f ms, p95 %.2f ms per frame (animation step + paint); static layers built once in %.1f ms\n", scale, (int) (1200 * scale), (int) (800 * scale), mean, p95, build);
        }
        return 0;
    }
    editor->setFakeAudio (fake);
    editor->setSpinEnabled (spin);
    editor->setHover (hover);
    editor->advance (t);
    if (trip)
    {
        // a breaker trip: move a preset parameter 0.1 away, then show the frame once the snap has finished (every switch down)
        auto* g = processor.apvts.getParameter (kiymaIds::glitch);
        g->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, g->getValue() + (g->getValue() > 0.5f ? -0.1f : 0.1f)));
        editor->advance (0.25);
    }

    auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, (float) scale);
    if (crop.isNotEmpty())
    {
        const auto v = juce::StringArray::fromTokens (crop, ",", "");
        image = image.getClippedImage (juce::Rectangle<int> (juce::roundToInt (v[0].getFloatValue() * (float) scale), juce::roundToInt (v[1].getFloatValue() * (float) scale),
                                                              juce::roundToInt (v[2].getFloatValue() * (float) scale), juce::roundToInt (v[3].getFloatValue() * (float) scale)));
    }
    juce::File file { out };
    file.deleteFile();
    juce::FileOutputStream stream (file);
    juce::PNGImageFormat().writeImageToStream (image, stream);
    std::printf ("editor %d x %d, t %.2f -> %s (%d x %d)\n", editor->getWidth(), editor->getHeight(), t, out.toRawUTF8(), image.getWidth(), image.getHeight());
    return 0;
}
