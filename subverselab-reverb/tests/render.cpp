/*  Drives the real plugin processor with a fake host clock, so the tests go
    through the same code path a DAW does.

        render --selftest
            Decay, echo sync, freeze, ducking, determinism and level checks.
            Exit code 0 only if every check passes.

        render <in.wav> <out.wav> <place 0-2> [mix] [decay] [duck] [bpm] [echoSync 0/1] [lowCut] [highCut]
*/
#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"

namespace
{
struct FakeHost : juce::AudioPlayHead
{
    double bpm = 120.0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setBpm (bpm);
        info.setIsPlaying (true);
        return info;
    }
};

void set (KubbeProcessor& p, const char* id, float value)
{
    auto* param = p.apvts.getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (value));
}

using Setup = std::function<void (KubbeProcessor&)>;

/** A whole buffer through a fresh processor in fixed blocks. perBlock may change parameters mid-run. */
juce::AudioBuffer<float> run (const juce::AudioBuffer<float>& input, double sr, Setup setup, int block = 512,
                              double bpm = 120.0, std::function<void (KubbeProcessor&, int)> perBlock = {})
{
    KubbeProcessor proc;
#if SVL_LICENSING
    proc.license.setLicensedForTesting (true);
#endif
    FakeHost host;
    host.bpm = bpm;
    proc.setPlayHead (&host);
    setup (proc);
    proc.setRateAndBufferSizeDetails (sr, block);
    proc.prepareToPlay (sr, block);

    juce::AudioBuffer<float> buf (input);
    juce::MidiBuffer midi;
    for (int pos = 0; pos < buf.getNumSamples(); pos += block)
    {
        const int n = juce::jmin (block, buf.getNumSamples() - pos);
        if (perBlock) perBlock (proc, pos);
        juce::AudioBuffer<float> view (buf.getArrayOfWritePointers(), buf.getNumChannels(), pos, n);
        proc.processBlock (view, midi);
    }
    return buf;
}

juce::AudioBuffer<float> impulse (double sr, double seconds)
{
    juce::AudioBuffer<float> b (2, (int) (sr * seconds));
    b.clear();
    b.setSample (0, 0, 1.0f);
    b.setSample (1, 0, 1.0f);
    return b;
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

double energy (const juce::AudioBuffer<float>& b, int from, int to)
{
    double e = 0.0;
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = juce::jmax (0, from); i < juce::jmin (to, b.getNumSamples()); ++i)
            e += (double) b.getSample (c, i) * b.getSample (c, i);
    return e;
}

double t60 (const juce::AudioBuffer<float>& b, double sr)
{
    const int n = b.getNumSamples();
    std::vector<double> edc ((size_t) n);
    double acc = 0.0;
    for (int i = n - 1; i >= 0; --i)
    {
        const double l = b.getSample (0, i), r = b.getSample (1, i);
        edc[(size_t) i] = (acc += l * l + r * r);
    }
    auto crossing = [&] (double db)
    {
        const double target = edc[0] * std::pow (10.0, db / 10.0);
        for (int i = 0; i < n; ++i)
            if (edc[(size_t) i] < target) return i / sr;
        return n / sr;
    };
    return (crossing (-35.0) - crossing (-5.0)) * 2.0;
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
    auto wetOnly = [] (int place, float decay) {
        return [=] (KubbeProcessor& p) { set (p, "place", (float) place); set (p, "mix", 1.0f); set (p, "decay", decay); };
    };

    // 1. The measured decay follows the Decay control, for every place.
    for (int place = 0; place < 3; ++place)
    {
        const auto& ch = svrb::characterAt (place);
        const auto a = run (impulse (sr, 24.0), sr, wetOnly (place, 0.5f));
        const auto b = run (impulse (sr, 24.0), sr, wetOnly (place, 1.0f));
        const double ta = t60 (a, sr), tb = t60 (b, sr);
        bool finite = true;
        for (int i = 0; i < b.getNumSamples(); ++i) finite &= std::isfinite (b.getSample (0, i));
        check (finite && energy (b, b.getNumSamples() - 4800, b.getNumSamples()) < 1.0e-9,
               juce::String (ch.name) + ": finite, and silent 24 s after an impulse");
        if (place != 2)   // Valley's tail is its echoes, not the room
            check (tb > ch.rt60 * 0.7 && tb < ch.rt60 * 1.2,
                   juce::String (ch.name) + ": T60 " + juce::String (tb, 2) + " s, nominal " + juce::String (ch.rt60, 2));
        check (ta < tb * 0.75,
               juce::String (ch.name) + ": Decay x0.5 gives a shorter tail (" + juce::String (ta, 2) + " vs " + juce::String (tb, 2) + " s)");
    }

    // 2. Echo Sync: in Valley the first slope answers on 3/4 beat at any tempo.
    for (double bpm : { 90.0, 120.0, 140.0 })
    {
        auto out = run (impulse (sr, 3.0), sr, [] (KubbeProcessor& p) {
            set (p, "place", 2.0f); set (p, "mix", 1.0f); set (p, "echoSync", 1.0f); set (p, "distance", 0.0f); }, 512, bpm);
        const int expect = (int) std::lround (0.75 * 60.0 / bpm * sr);
        // Onset: the first sample well above the early-reflection tail, after 200 ms.
        double floor = 0.0;
        for (int i = (int) (0.2 * sr); i < expect - (int) (0.01 * sr); ++i)
            floor = juce::jmax (floor, (double) std::abs (out.getSample (0, i)));
        int onset = -1;
        for (int i = expect - (int) (0.01 * sr); i < expect + (int) (0.05 * sr); ++i)
            if (std::abs (out.getSample (0, i)) > floor * 4.0 + 1.0e-4) { onset = i; break; }
        check (onset >= 0 && std::abs (onset - expect) < (int) (0.003 * sr),
               "Echo Sync at " + juce::String (bpm, 0) + " BPM: first echo " + juce::String ((onset - expect) / sr * 1000.0, 2) + " ms off the 3/4 beat");
    }

    // 3. Freeze holds the tail: 4 s later it is within 3 dB.
    {
        const int at = (int) (1.0 * sr);
        auto out = run (noise ((int) (7.0 * sr), 0.2f), sr, [] (KubbeProcessor& p) { set (p, "place", 1.0f); set (p, "mix", 1.0f); },
                        512, 120.0, [at] (KubbeProcessor& p, int pos) { if (pos >= at) set (p, "freeze", 1.0f); });
        const double e1 = energy (out, (int) (1.5 * sr), (int) (2.0 * sr)), e2 = energy (out, (int) (5.5 * sr), (int) (6.0 * sr));
        const double db = 10.0 * std::log10 (e2 / e1);
        check (std::abs (db) < 3.0, "Freeze: tail moves " + juce::String (db, 2) + " dB over 4 s while the input keeps playing");
    }

    // 4. Ducking: with a steady input, the wet drops 12-26 dB at Duck 100 %; it ducks, it does not mute.
    {
        auto in = noise ((int) (3.0 * sr), 0.3f);
        auto open = run (in, sr, [] (KubbeProcessor& p) { set (p, "mix", 1.0f); });
        auto duck = run (in, sr, [] (KubbeProcessor& p) { set (p, "mix", 1.0f); set (p, "duck", 1.0f); });
        const double db = 10.0 * std::log10 (energy (duck, (int) sr, 2 * (int) sr) / energy (open, (int) sr, 2 * (int) sr));
        check (db < -12.0 && db > -26.0, "Duck 100 %: wet " + juce::String (db, 1) + " dB while the source plays");
    }

    // 5. Determinism: the same input gives the same bits at any block size.
    for (int place = 0; place < 3; ++place)
    {
        auto in = noise ((int) (2.0 * sr), 0.2f);
        auto setup = [place] (KubbeProcessor& p) { set (p, "place", (float) place); set (p, "mix", 0.4f); };
        auto a = run (in, sr, setup, 512), b = run (in, sr, setup, 97), c = run (in, sr, setup, 512);
        bool same = true;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < a.getNumSamples(); ++i)
                same &= a.getSample (ch, i) == b.getSample (ch, i) && a.getSample (ch, i) == c.getSample (ch, i);
        check (same, juce::String (svrb::characterAt (place).name) + ": identical across runs and block sizes 512 / 97");
    }

    // 6. Level: at the default 30 % mix, no place is more than 3 dB above the dry input, and the places are within 4 dB of each other.
    {
        auto in = noise ((int) (4.0 * sr), 0.2f);
        double lo = 1.0e9, hi = -1.0e9;
        for (int place = 0; place < 3; ++place)
        {
            auto out = run (in, sr, [place] (KubbeProcessor& p) { set (p, "place", (float) place); set (p, "mix", 1.0f); });
            const double db = 10.0 * std::log10 (energy (out, (int) sr, 4 * (int) sr) / energy (in, (int) sr, 4 * (int) sr));
            lo = juce::jmin (lo, db); hi = juce::jmax (hi, db);
            std::printf ("      %s full wet: %.1f dB against the input\n", svrb::characterAt (place).name, db);
        }
        check (hi < 3.0 && hi - lo < 4.0, "Kubbe sit within " + juce::String (hi - lo, 1) + " dB of each other at full wet");
    }

    // 7. Switching place mid-tail: no jump bigger than the signal already has.
    {
        auto in = noise ((int) (4.0 * sr), 0.2f);
        auto out = run (in, sr, [] (KubbeProcessor& p) { set (p, "mix", 0.5f); }, 512, 120.0,
                        [] (KubbeProcessor& p, int pos) { if (pos >= 96000) set (p, "place", 1.0f); });
        float worst = 0.0f, typical = 0.0f;
        for (int i = 1; i < out.getNumSamples(); ++i)
        {
            const float d = std::abs (out.getSample (0, i) - out.getSample (0, i - 1));
            if (i > 95000 && i < 100000) worst = juce::jmax (worst, d); else typical = juce::jmax (typical, d);
        }
        check (worst <= typical * 1.1f, "Place switch: largest step " + juce::String (worst, 3) + " vs " + juce::String (typical, 3) + " elsewhere");
    }


#if SVL_LICENSING
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

    // Launch keys: SVL- keys go to our own server and must be for KUBBE or the bundle; other keys stay on Lemon Squeezy.
    {
        using B = svl::License::Backend;
        const auto cfg = kubbeLicenseConfig();
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
        const auto own = svl::License::parseActivate (svlAnswer (1, 9001), cfg, B::svl);
        const auto sibling = svl::License::parseActivate (svlAnswer (1, 9002), cfg, B::svl);
        const auto foreign = svl::License::parseActivate (svlAnswer (2, 9003), cfg, B::svl);
        check (bundle.ok && bundle.instanceId == "svl-inst" && own.ok, "Licence: an SVL key for KUBBE or the launch bundle activates");
        check (! sibling.ok && sibling.definitelyRevoked && sibling.message.contains ("different product"),
               "Licence: an SVL key for KASET alone (9002) is refused by KUBBE");
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

    // Without a licence the audio passes through untouched.
    {
        auto in = noise ((int) sr, 0.2f);
        auto out = run (in, sr, [] (KubbeProcessor& p) { p.license.setLicensedForTesting (false); set (p, "mix", 1.0f); });
        float worst = 0.0f;
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < out.getNumSamples(); ++i)
                worst = juce::jmax (worst, std::abs (out.getSample (c, i) - in.getSample (c, i)));
        check (worst < 1.0e-9f, "No licence: the audio passes through untouched, largest difference " + juce::String (worst, 8));
    }
#else
    // SVL_LICENSING=0: no gate at all, a fresh instance processes immediately.
    {
        auto in = noise ((int) sr, 0.2f);
        auto out = run (in, sr, [] (KubbeProcessor& p) { set (p, "mix", 1.0f); });
        float diff = 0.0f;
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < out.getNumSamples(); ++i)
                diff = juce::jmax (diff, std::abs (out.getSample (c, i) - in.getSample (c, i)));
        check (diff > 1.0e-4f, "SVL_LICENSING=0: a fresh instance processes with no licence check at all, difference " + juce::String (diff, 6));
    }
#endif

    std::printf ("\n%s\n", failures == 0 ? "ALL PASS" : (juce::String (failures) + " FAILED").toRawUTF8());
    return failures == 0 ? 0 : 1;
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc >= 2 && juce::String (argv[1]) == "--selftest")
        return selftest();
    if (argc < 4)
    {
        std::printf ("render --selftest\nrender <in.wav> <out.wav> <place 0-2> [mix] [decay] [duck] [bpm] [echoSync] [lowCut] [highCut]\n");
        return 2;
    }

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (juce::File (juce::String (argv[1]))));
    if (! reader) return 2;
    const double sr = reader->sampleRate;
    const float tail = 5.0f;
    juce::AudioBuffer<float> in (2, (int) reader->lengthInSamples + (int) (tail * sr));
    in.clear();
    reader->read (&in, 0, (int) reader->lengthInSamples, 0, true, true);

    auto arg = [&] (int i, float d) { return argc > i ? (float) std::atof (argv[i]) : d; };
    auto out = run (in, sr, [&] (KubbeProcessor& p) {
        set (p, "place", arg (3, 0)); set (p, "mix", arg (4, 0.3f)); set (p, "decay", arg (5, 1.0f));
        set (p, "duck", arg (6, 0.0f)); set (p, "echoSync", arg (8, 0.0f));
        set (p, "lowCut", arg (9, 20.0f)); set (p, "highCut", arg (10, 20000.0f)); }, 512, arg (7, 120.0f));

    juce::File f { juce::String (argv[2]) };
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (f), sr, 2, 24, {}, 0));
    w->writeFromAudioSampleBuffer (out, 0, out.getNumSamples());
    return 0;
}
