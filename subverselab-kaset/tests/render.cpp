/*  Drives the real plugin processor, so the tests go through the same code
    path a DAW does.

        render --selftest
            Latency, generation, auto gain, bypass and determinism checks.
            Exit code 0 only if every check passes.

        render <in.wav> <out.wav> [generation 0-4] [tape 0-2] [mix]
*/
#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"

namespace
{
void set (KasetProcessor& p, const char* id, float value)
{
    auto* param = p.apvts.getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (value));
}

using Setup = std::function<void (KasetProcessor&)>;

juce::AudioBuffer<float> run (const juce::AudioBuffer<float>& input, double sr, Setup setup, int block = 512)
{
    KasetProcessor proc;
    proc.license.setLicensedForTesting (true);
    setup (proc);
    proc.setRateAndBufferSizeDetails (sr, block);
    proc.prepareToPlay (sr, block);
    juce::AudioBuffer<float> buf (input);
    juce::MidiBuffer midi;
    for (int pos = 0; pos < buf.getNumSamples(); pos += block)
    {
        const int n = juce::jmin (block, buf.getNumSamples() - pos);
        juce::AudioBuffer<float> view (buf.getArrayOfWritePointers(), buf.getNumChannels(), pos, n);
        proc.processBlock (view, midi);
    }
    return buf;
}

juce::AudioBuffer<float> noise (int samples, float amp)
{
    juce::AudioBuffer<float> b (2, samples);
    uint32_t s = 12345u;
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < samples; ++i)
        {
            s = s * 1664525u + 1013904223u;
            b.setSample (c, i, ((float) (s >> 8) / 16777216.0f - 0.5f) * 2.0f * amp);
        }
    return b;
}

double rmsDb (const juce::AudioBuffer<float>& b, int from, int to)
{
    double e = 0.0;
    for (int c = 0; c < 2; ++c)
        for (int i = from; i < to; ++i)
            e += (double) b.getSample (c, i) * b.getSample (c, i);
    return 10.0 * std::log10 (e / (2.0 * (to - from)) + 1.0e-30);
}

/** Where a click comes out: the index of the largest sample. */
int peakAt (const juce::AudioBuffer<float>& b)
{
    int best = 0;
    for (int i = 0; i < b.getNumSamples(); ++i)
        if (std::abs (b.getSample (0, i)) > std::abs (b.getSample (0, best))) best = i;
    return best;
}

int failures = 0;
void check (bool ok, const juce::String& what)
{
    std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8());
    if (! ok) ++failures;
}

int selftest()
{
    const double sr = 48000.0;
    KasetProcessor probe;
    probe.prepareToPlay (sr, 512);
    const int lat = probe.getLatencySamples();

    // 1. The same latency for every generation, and it is where the audio really is.
    for (int gen = 0; gen < 5; ++gen)
    {
        juce::AudioBuffer<float> click (2, (int) sr);
        click.clear();
        click.setSample (0, 1000, 0.8f);
        click.setSample (1, 1000, 0.8f);
        auto out = run (click, sr, [gen] (KasetProcessor& p) {
            set (p, "generation", (float) gen); set (p, "wow", 0.0f); set (p, "flutter", 0.0f); set (p, "hiss", 0.0f); set (p, "wear", 0.0f); set (p, "autoGain", 0.0f); });
        const int off = peakAt (out) - 1000 - lat;
        check (std::abs (off) <= 6, "Generation " + juce::String (gen + 1) + ": click lands " + juce::String (off) + " samples from the reported latency (" + juce::String (lat) + ")");
    }

    // 2. Auto gain keeps the level within 1.5 dB of the input, every generation and tape.
    for (int gen : { 0, 2, 4 })
        for (int tape = 0; tape < 3; ++tape)
        {
            auto in = noise ((int) (8.0 * sr), 0.15f);
            auto out = run (in, sr, [gen, tape] (KasetProcessor& p) { set (p, "generation", (float) gen); set (p, "tape", (float) tape); });
            const double d = rmsDb (out, (int) (5.0 * sr), (int) (8.0 * sr)) - rmsDb (in, (int) (5.0 * sr), (int) (8.0 * sr));
            check (std::abs (d) < 1.5, "Auto gain, gen " + juce::String (gen + 1) + " tape " + KasetProcessor::tapeNames[tape] + ": " + juce::String (d, 2) + " dB");
        }

    // Every control changes the sound when the tape is in, and nothing does in BYPASS.
    {
        auto in = noise ((int) (3.0 * sr), 0.15f);
        auto base = run (in, sr, [] (KasetProcessor&) {});
        auto diffDb = [&] (const juce::AudioBuffer<float>& a)
        {
            juce::AudioBuffer<float> d (a);
            for (int c = 0; c < 2; ++c) d.addFrom (c, 0, base, c, 0, base.getNumSamples(), -1.0f);
            return rmsDb (d, (int) sr, 3 * (int) sr) - rmsDb (base, (int) sr, 3 * (int) sr);
        };
        const std::pair<const char*, float> moves[] { { "input", 12.0f }, { "drive", 1.0f }, { "wow", 1.0f }, { "flutter", 1.0f }, { "hiss", 1.0f },
                                                      { "wear", 1.0f }, { "mix", 0.3f }, { "output", 6.0f }, { "generation", 0.0f }, { "tape", 2.0f } };
        for (auto [id, v] : moves)
        {
            auto on = run (in, sr, [id = id, v = v] (KasetProcessor& p) { set (p, id, v); });
            auto off = run (in, sr, [id = id, v = v] (KasetProcessor& p) { set (p, "play", 0.0f); set (p, id, v); });
            auto offBase = run (in, sr, [] (KasetProcessor& p) { set (p, "play", 0.0f); });
            float offWorst = 0.0f;
            for (int i = 0; i < off.getNumSamples(); ++i)
                offWorst = juce::jmax (offWorst, std::abs (off.getSample (0, i) - offBase.getSample (0, i)));
            const double d = diffDb (on);
            check (d > -45.0 && offWorst < 1.0e-6f, juce::String (id).toUpperCase() + ": changes the sound by " + juce::String (d, 1)
                   + " dB against the default; in BYPASS the change is " + juce::String (offWorst, 7));
        }
    }

    // 3. More generations, less top: energy above 6 kHz falls step by step.
    {
        double last = 1.0e9;
        bool falling = true;
        juce::String list;
        for (int gen = 0; gen < 5; ++gen)
        {
            auto in = noise ((int) (4.0 * sr), 0.1f);
            auto out = run (in, sr, [gen] (KasetProcessor& p) { set (p, "generation", (float) gen); set (p, "hiss", 0.0f); set (p, "autoGain", 0.0f); });
            juce::dsp::IIR::Filter<float> hp;
            hp.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighPass (sr, 6000.0);
            double e = 0.0;
            for (int i = 0; i < out.getNumSamples(); ++i)
            {
                const float y = hp.processSample (out.getSample (0, i));
                if (i > (int) sr) e += (double) y * y;
            }
            const double db = 10.0 * std::log10 (e + 1.0e-30);
            falling &= db < last;
            last = db;
            list << juce::String (db, 1) << " ";
        }
        check (falling, "Top end above 6 kHz falls with every generation: " + list);
    }

    // 4. BYPASS is a true bypass: the dry signal, delayed by the latency, bit-exact after the fade.
    {
        auto in = noise ((int) sr, 0.2f);
        auto out = run (in, sr, [] (KasetProcessor& p) { set (p, "play", 0.0f); });
        float worst = 0.0f;
        for (int i = lat + 2000; i < out.getNumSamples(); ++i)
            worst = juce::jmax (worst, std::abs (out.getSample (0, i) - in.getSample (0, i - lat)));
        check (worst < 1.0e-6f, "BYPASS passes the dry signal through, largest difference " + juce::String (worst, 8));
    }

    // 5. Determinism: identical bits across runs and block sizes.
    {
        auto in = noise ((int) (2.0 * sr), 0.2f);
        auto setup = [] (KasetProcessor& p) { set (p, "generation", 4.0f); set (p, "wear", 1.0f); };
        auto a = run (in, sr, setup, 512), b = run (in, sr, setup, 97), c = run (in, sr, setup, 512);
        bool same = true, finite = true;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < a.getNumSamples(); ++i)
            {
                same &= a.getSample (ch, i) == b.getSample (ch, i) && a.getSample (ch, i) == c.getSample (ch, i);
                finite &= std::isfinite (a.getSample (ch, i));
            }
        check (same && finite, "4th dub, full wear: identical across runs and block sizes 512 / 97, all finite");
    }


    // Licensing: the answers the licence server can give, parsed without a network.
    {
        svl::LicenseConfig cfg;
        cfg.product = "TEST";
        cfg.storeId = 111;
        cfg.productIds = { 222, 333 };
        const juce::String good = R"({"activated":true,"error":null,"license_key":{"status":"active"},"instance":{"id":"inst-1"},"meta":{"store_id":111,"product_id":333}})";
        const juce::String other = R"({"activated":true,"error":null,"license_key":{"status":"active"},"instance":{"id":"inst-2"},"meta":{"store_id":111,"product_id":999}})";
        const juce::String limit = R"({"activated":false,"error":"This license key has reached the activation limit.","license_key":{"status":"active"},"meta":{}})";
        const auto a = svl::License::parseActivate (good, cfg);
        const auto b = svl::License::parseActivate (other, cfg);
        const auto c = svl::License::parseActivate (limit, cfg);
        const auto d = svl::License::parseActivate ("", cfg);
        check (a.ok && a.instanceId == "inst-1", "Licence: a key for our product (or the bundle) activates");
        check (! b.ok && b.message.contains ("different product"), "Licence: a key for another product is refused");
        check (! c.ok && c.message.contains ("activation limit"), "Licence: the server's own error is shown (activation limit)");
        check (! d.ok && ! d.definitelyRevoked, "Licence: no network is not a refusal");
        const auto v1 = svl::License::parseValidate (R"({"valid":false,"error":"license_key not found","license_key":{"status":"disabled"},"meta":{}})", cfg);
        const auto v2 = svl::License::parseValidate ("", cfg);
        check (v1.definitelyRevoked && ! v2.definitelyRevoked, "Licence: a disabled key is revoked on re-check, an offline re-check keeps the licence");
    }

    // Launch keys: SVL- keys go to our own server and must be for KASET or the bundle; other keys stay on Lemon Squeezy.
    {
        using B = svl::License::Backend;
        const auto cfg = kasetLicenseConfig();
        check (svl::License::apiBaseFor ("SVL-AB12-CD34-EF56-GH78", cfg) == "https://subverselab.com/api/license/"
                   && svl::License::apiBaseFor ("  svl-ab12-cd34-ef56-gh78 ", cfg) == "https://subverselab.com/api/license/"
                   && svl::License::apiBaseFor ("38b1460a-5104-4067-a91d-77b872934d51", cfg) == "https://api.lemonsqueezy.com/v1/licenses/",
               "Licence: SVL- keys (any case) go to subverselab.com, other keys to Lemon Squeezy");
        auto svlAnswer = [] (int store, int product)
        {
            return R"({"activated":true,"error":null,"license_key":{"status":"active"},"instance":{"id":"svl-inst"},"meta":{"store_id":)"
                   + juce::String (store) + R"(,"product_id":)" + juce::String (product) + "}}";
        };
        const auto bundle = svl::License::parseActivate (svlAnswer (1, 9003), cfg, B::svl);
        const auto own = svl::License::parseActivate (svlAnswer (1, 9002), cfg, B::svl);
        const auto sibling = svl::License::parseActivate (svlAnswer (1, 9001), cfg, B::svl);
        const auto foreign = svl::License::parseActivate (svlAnswer (2, 9003), cfg, B::svl);
        check (bundle.ok && bundle.instanceId == "svl-inst" && own.ok, "Licence: an SVL key for KASET or the launch bundle activates");
        check (! sibling.ok && sibling.definitelyRevoked && sibling.message.contains ("different product"),
               "Licence: an SVL key for KUBBE alone (9001) is refused by KASET");
        check (! foreign.ok, "Licence: an SVL answer from another store is refused");
        const auto used = svl::License::parseActivate (R"({"activated":false,"error":"This license key has reached the activation limit.","meta":{}})", cfg, B::svl);
        const auto gone = svl::License::parseValidate (R"({"valid":false,"error":"license_key not found","meta":{}})", cfg, B::svl);
        const auto fine = svl::License::parseValidate (R"({"valid":true,"error":null,"license_key":{"status":"active"},"meta":{"store_id":1,"product_id":9003}})", cfg, B::svl);
        const auto offline = svl::License::parseValidate ("", cfg, B::svl);
        check (! used.ok && used.message.contains ("activation limit") && gone.definitelyRevoked && fine.ok && ! offline.definitelyRevoked,
               "Licence: our server's refusals, re-checks and silence are read like Lemon Squeezy's");
        // A Lemon Squeezy key is never judged by the SVL rule, and our store id means nothing to it.
        const auto lsDev = svl::License::parseActivate (svlAnswer (555, 12345), cfg, B::lemonSqueezy);
        auto lsCfg = cfg;
        lsCfg.storeId = 111;
        lsCfg.productIds = { 222 };
        const auto lsOurs = svl::License::parseActivate (svlAnswer (111, 222), lsCfg);
        const auto lsSvlIds = svl::License::parseActivate (svlAnswer (1, 9003), lsCfg);
        check (lsDev.ok && lsOurs.ok && ! lsSvlIds.ok, "Licence: Lemon Squeezy keys keep the store / product rule");
        // The licence file remembers its server; files from before SVL keys are Lemon Squeezy.
        const auto back = svl::License::decodeStored (svl::License::encodeStored ({ "SVL-AB12-CD34-EF56-GH78", "svl-inst", 1234, B::svl }));
        const auto ls = svl::License::decodeStored (svl::License::encodeStored ({ "38b1460a", "ls-inst", 99, B::lemonSqueezy }));
        const auto old = svl::License::decodeStored (R"({"key":"38b1460a","instance":"ls-inst","validated":99})");
        check (back.backend == B::svl && back.key == "SVL-AB12-CD34-EF56-GH78" && back.instanceId == "svl-inst" && back.validated == 1234
                   && ls.backend == B::lemonSqueezy && old.backend == B::lemonSqueezy && old.instanceId == "ls-inst",
               "Licence: the stored server survives a save / load; an old file without it is Lemon Squeezy");
    }

    // Without a licence the audio passes through untouched (latency-aligned, like BYPASS).
    {
        KasetProcessor probeLatency;
        probeLatency.prepareToPlay (sr, 512);
        const int latency = probeLatency.getLatencySamples();
        auto in = noise ((int) sr, 0.2f);
        auto out = run (in, sr, [] (KasetProcessor& p) { p.license.setLicensedForTesting (false); });
        float worst = 0.0f;
        for (int i = latency + 2000; i < out.getNumSamples(); ++i)
            worst = juce::jmax (worst, std::abs (out.getSample (0, i) - in.getSample (0, i - latency)));
        check (worst < 1.0e-6f, "No licence: the audio passes through untouched, largest difference " + juce::String (worst, 8));
    }

    std::printf ("\n%s\n", failures == 0 ? "ALL PASS" : (juce::String (failures) + " FAILED").toRawUTF8());
    return failures == 0 ? 0 : 1;
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc >= 2 && juce::String (argv[1]) == "--selftest")
        return selftest();
    if (argc < 3)
    {
        std::printf ("render --selftest\nrender <in.wav> <out.wav> [generation 0-4] [tape 0-2] [mix]\n");
        return 2;
    }
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (juce::File (juce::String (argv[1]))));
    if (! reader) return 2;
    const double sr = reader->sampleRate;
    juce::AudioBuffer<float> in (2, (int) reader->lengthInSamples);
    reader->read (&in, 0, (int) reader->lengthInSamples, 0, true, true);
    auto arg = [&] (int i, float d) { return argc > i ? (float) std::atof (argv[i]) : d; };
    auto out = run (in, sr, [&] (KasetProcessor& p) { set (p, "generation", arg (3, 2)); set (p, "tape", arg (4, 0)); set (p, "mix", arg (5, 1.0f)); });
    juce::File f { juce::String (argv[2]) };
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (f), sr, 2, 24, {}, 0));
    w->writeFromAudioSampleBuffer (out, 0, out.getNumSamples());
    return 0;
}
