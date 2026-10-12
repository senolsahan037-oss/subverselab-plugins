/*  Drives the real plugin processor with a fake host clock, so the checks go
    through the same code path a DAW does.

        render --selftest
            1. The engine header is the verbatim prototype copy (md5).
            2. The engine's own selftest (the prototype harness, copied verbatim
               into tests/ and compiled against Source/YankiEngine.h). Its test
               audio lives in the prototype folder, so it runs from there:
               ../prototypes/YANKI, or $YANKI_PROTOTYPE_DIR.
            3. Plugin-layer checks: default = the engine's ECHO bit for bit,
               moods bit-identical on FREE, host tempo and the 120 bpm
               fallback, bypass bit-exact, glides without clicks, state
               round trip, swell, END CUT, latency.
            Exit code 0 only if everything passes.

        render <in.wav> <out.wav> [--mood 0-6] [--bpm B] [--sync 0-5] [--feedback 0-1]
               [--cap dB|off] [--mix 0-1] [--time ms]
            Renders through the processor (sync index in face order:
            0 1/16, 1 1/8, 2 1/8., 3 1/4, 4 1/4., 5 1/2, 6 FREE).
*/
#include <JuceHeader.h>
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"
JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wsign-conversion")   // the prototype's md5.h, used as is
#include "md5.h"
JUCE_END_IGNORE_WARNINGS_GCC_LIKE
#include "host_sim_native.h"
#include "../Source/RingModel.h"
#include "SvlUndoTests.h"
#include "SvlControlProof.h"
#include "SvlParamTextTests.h"
#include <fstream>
#include <sstream>

int yanki_engine_main (int argc, char** argv);   // tests/engine_render.cpp (prototype tools/render.cpp, main renamed)

namespace
{
constexpr const char* kEngineMd5 = "608b9d2317a63ab7939f7b64b3c2814e";

struct FakeHost : juce::AudioPlayHead
{
    std::optional<double> bpm = 120.0;
    double ppq = 0.0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        if (bpm) info.setBpm (*bpm);
        info.setIsPlaying (true);
        info.setPpqPosition (ppq);
        return info;
    }
};

int passed = 0, failed = 0;
void check (const juce::String& name, bool ok, const juce::String& detail)
{
    std::cout << (ok ? "PASS " : "FAIL ") << name << " " << detail << "\n";
    (ok ? passed : failed)++;
}

void set (YankiProcessor& p, const char* id, float value)
{
    auto* param = p.apvts.getParameter (id);
    param->beginChangeGesture();
    param->setValueNotifyingHost (param->convertTo0to1 (value));
    param->endChangeGesture();
}

/** A 1.5 s harmonic burst (220 Hz, vocal-ish envelope) then silence. */
juce::AudioBuffer<float> burst (double sr, double seconds, float amp = 0.3f, double burstSec = 1.5)
{
    juce::AudioBuffer<float> b (2, (int) (sr * seconds));
    b.clear();
    const int n = (int) (sr * burstSec);
    for (int i = 0; i < n && i < b.getNumSamples(); ++i)
    {
        const double t = i / sr;
        const double env = std::min (1.0, t / 0.01) * std::min (1.0, (burstSec - t) / 0.01) * (0.8 + 0.2 * std::sin (2 * juce::MathConstants<double>::pi * 3 * t));
        double v = 0;
        for (int h = 1; h <= 6; ++h)
            v += std::sin (2 * juce::MathConstants<double>::pi * 220 * h * t) / h;
        b.setSample (0, i, (float) (amp * env * v * 0.6));
        b.setSample (1, i, (float) (amp * env * v * 0.6));
    }
    return b;
}

juce::AudioBuffer<float> sine (double sr, double seconds, float amp, double hz = 220.0)
{
    juce::AudioBuffer<float> b (2, (int) (sr * seconds));
    for (int i = 0; i < b.getNumSamples(); ++i)
    {
        const float v = amp * (float) std::sin (2 * juce::MathConstants<double>::pi * hz * i / sr);
        b.setSample (0, i, v);
        b.setSample (1, i, v);
    }
    return b;
}

using Setup = std::function<void (YankiProcessor&)>;
using PerBlock = std::function<void (YankiProcessor&, FakeHost&, int pos)>;

juce::AudioBuffer<float> run (const juce::AudioBuffer<float>& input, double sr, Setup setup, int block = 512, bool withHost = true,
                              double bpm = 120.0, PerBlock perBlock = {}, YankiProcessor* external = nullptr)
{
    std::unique_ptr<YankiProcessor> own;
    YankiProcessor* proc = external;
    if (proc == nullptr)
    {
        own = std::make_unique<YankiProcessor>();
        proc = own.get();
    }
    FakeHost host;
    host.bpm = bpm;
    proc->setPlayHead (withHost ? &host : nullptr);
    if (setup) setup (*proc);
    proc->setRateAndBufferSizeDetails (sr, block);
    proc->prepareToPlay (sr, block);
    juce::AudioBuffer<float> buf (input);
    juce::MidiBuffer midi;
    for (int pos = 0; pos < buf.getNumSamples(); pos += block)
    {
        const int n = juce::jmin (block, buf.getNumSamples() - pos);
        if (perBlock) perBlock (*proc, host, pos);
        juce::AudioBuffer<float> view (buf.getArrayOfWritePointers(), buf.getNumChannels(), pos, n);
        proc->processBlock (view, midi);
        host.ppq += n / sr * (host.bpm ? *host.bpm : 120.0) / 60.0;
    }
    proc->setPlayHead (nullptr);
    return buf;
}

/** The engine alone, same block size. */
juce::AudioBuffer<float> engineRun (const juce::AudioBuffer<float>& input, double sr, yanki::Controls c, int block = 512)
{
    yanki::YankiEngine e;
    e.prepare (sr);
    juce::AudioBuffer<float> buf (input);
    for (int pos = 0; pos < buf.getNumSamples(); pos += block)
    {
        const int n = juce::jmin (block, buf.getNumSamples() - pos);
        e.process (buf.getWritePointer (0) + pos, buf.getWritePointer (1) + pos, n, c);
    }
    return buf;
}

bool identical (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    if (a.getNumSamples() != b.getNumSamples()) return false;
    for (int c = 0; c < 2; ++c)
        if (std::memcmp (a.getReadPointer (c), b.getReadPointer (c), sizeof (float) * (size_t) a.getNumSamples()) != 0)
            return false;
    return true;
}

double maxStep (const juce::AudioBuffer<float>& b, double sr, double t0, double t1)
{
    double m = 0;
    for (int c = 0; c < 2; ++c)
        for (int i = juce::jmax (1, (int) (t0 * sr)); i < juce::jmin (b.getNumSamples(), (int) (t1 * sr)); ++i)
            m = std::max (m, (double) std::abs (b.getSample (c, i) - b.getSample (c, i - 1)));
    return m;
}

double rmsDb (const juce::AudioBuffer<float>& b, double sr, double t0, double t1)
{
    double s = 0;
    int n = 0;
    for (int c = 0; c < 2; ++c)
        for (int i = (int) (t0 * sr); i < juce::jmin (b.getNumSamples(), (int) (t1 * sr)); ++i, ++n)
            s += (double) b.getSample (c, i) * b.getSample (c, i);
    return 10.0 * std::log10 (std::max (s / std::max (1, n), 1e-24));
}

bool finiteBounded (const juce::AudioBuffer<float>& b, float bound = 4.0f)
{
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = 0; i < b.getNumSamples(); ++i)
            if (! std::isfinite (b.getSample (c, i)) || std::abs (b.getSample (c, i)) > bound)
                return false;
    return true;
}

int engineSelftest()
{
    auto dir = juce::File::getCurrentWorkingDirectory().getChildFile ("../prototypes/YANKI");
    if (auto* env = std::getenv ("YANKI_PROTOTYPE_DIR"))
        dir = juce::File (juce::String (env));
    if (! dir.getChildFile ("tools/drum_loop.wav").existsAsFile())
    {
        std::cout << "FAIL engine_selftest cannot find the prototype test audio in " << dir.getFullPathName() << " (set YANKI_PROTOTYPE_DIR)\n";
        return 1;
    }
    const auto here = juce::File::getCurrentWorkingDirectory();
    dir.setAsCurrentWorkingDirectory();
    std::cout << "---- engine selftest (prototype harness, run in " << dir.getFullPathName() << ") ----\n" << std::flush;
    char a0[] = "render", a1[] = "--selftest";
    char* argv[] { a0, a1, nullptr };
    const int rc = yanki_engine_main (2, argv);
    std::cout << std::flush;
    here.setAsCurrentWorkingDirectory();
    std::cout << "---- plugin checks ----\n";
    return rc;
}

void regressionChecks()
{
    {
        // Regression (v2 step 1): a non-finite sample in the host's audio must never leave the plug-in dead.
        // Before the fix one NaN or +inf sample poisoned the tape loop for good (output NaN, every control ignored).
        const double rate = 48000.0;
        for (int kind = 0; kind < 2; ++kind)
        {
            auto in = sine (rate, 6.0, 0.1f, 220.0);
            const float bad = kind == 0 ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
            in.setSample (0, (int) (1.0 * rate), bad);
            in.setSample (1, (int) (1.5 * rate), bad);
            const auto o = run (in, rate, [] (YankiProcessor& q) { set (q, yankiIds::mix, 0.3f); }, 128);
            bool finite = true;
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < o.getNumSamples(); ++i)
                    if (! std::isfinite (o.getSample (c, i))) { finite = false; break; }
            // the wet part must be back: output differs from the input by an echo (> -30 dB re the input)
            double d = 0, e = 0;
            for (int c = 0; c < 2; ++c)
                for (int i = (int) (4.0 * rate); i < o.getNumSamples(); ++i)
                {
                    const double x = in.getSample (c, i), y = o.getSample (c, i);
                    d += (y - x) * (y - x); e += x * x;
                }
            const double rel = 10.0 * std::log10 (std::max (d, 1e-30) / std::max (e, 1e-30));
            check (kind == 0 ? "nonfinite_nan_input_recovers" : "nonfinite_inf_input_recovers", finite && rel > -30.0 && rel < 6.0,
                   juce::String (kind == 0 ? "one NaN" : "one +inf") + " sample per channel at 1.0 / 1.5 s: output finite " + (finite ? "yes" : "NO")
                   + ", echo 4-6 s " + juce::String (rel, 1) + " dB re the input");
        }
    }
    {
        // Host-like run: audio thread + message thread, MIX moved with gestures, then TIME / FEEDBACK / TONE (the owner's order).
        // The output must follow MIX after every step: MIX 0 -> the input, MIX 0.3 -> an echo within 75 ms.
        YankiProcessor q;
        hostsim::NativeParams np (q);
        hostsim::Rig rig (q, np, 48000.0, 128, false);
        rig.start();
        rig.waitSamples (48000);
        bool ok = true;
        juce::String detail;
        auto step = [&] (const char* tag)
        {
            const auto r = hostsim::probe (rig);
            ok = ok && r.follows();
            detail << tag << " " << juce::String (r.diffDbAtMix, 1) << " dB; ";
        };
        step ("start");
        hostsim::move (rig, hostsim::ids::mix, 0.6f);  rig.waitSamples (20000);
        hostsim::move (rig, hostsim::ids::mix, 0.2f);  rig.waitSamples (20000);
        step ("after 2 MIX moves");
        hostsim::move (rig, hostsim::ids::timeSync, hostsim::norm (rig, hostsim::ids::timeSync, 6.0f));
        hostsim::move (rig, hostsim::ids::timeMs, hostsim::norm (rig, hostsim::ids::timeMs, 400.0f));
        hostsim::move (rig, hostsim::ids::feedback, 0.6f);
        hostsim::move (rig, hostsim::ids::tone, hostsim::norm (rig, hostsim::ids::tone, 2500.0f));
        rig.waitSamples (48000);
        step ("after TIME/FEEDBACK/TONE");
        // a non-finite block from the host, then MIX again
        rig.inputKind = 6; rig.waitSamples (48000 * 2); rig.inputKind = 0;
        step ("after a NaN sample in the input");
        rig.stop();
        check ("host_like_follows_mix", ok, detail);
    }

}


/** A pink-ish noise burst (Paul Kellet's filter), RMS about -18 dBFS, then silence, `seconds` long. */
juce::AudioBuffer<float> pinkBurst (double sr, double seconds, double burstSec, double startSec, double& burstRms)
{
    juce::AudioBuffer<float> b (2, (int) (sr * seconds));
    b.clear();
    juce::Random rnd (3);
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0, acc = 0;
    const int n = (int) (burstSec * sr), s0 = (int) (startSec * sr), fade = (int) (0.02 * sr);
    std::vector<double> v ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        const double w = rnd.nextDouble() * 2.0 - 1.0;
        b0 = 0.99886 * b0 + w * 0.0555179; b1 = 0.99332 * b1 + w * 0.0750759; b2 = 0.96900 * b2 + w * 0.1538520;
        b3 = 0.86650 * b3 + w * 0.3104856; b4 = 0.55000 * b4 + w * 0.5329522; b5 = -0.7616 * b5 - w * 0.0168980;
        const double x = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362;
        b6 = w * 0.115926;
        const double env = std::min ({ 1.0, (double) i / fade, (double) (n - 1 - i) / fade });
        v[(size_t) i] = x * env;
        acc += v[(size_t) i] * v[(size_t) i];
    }
    const double g = std::pow (10.0, -18.0 / 20.0) / std::sqrt (acc / n);
    for (int i = 0; i < n; ++i)
        for (int c = 0; c < 2; ++c)
            b.setSample (c, s0 + i, (float) (v[(size_t) i] * g));
    burstRms = std::pow (10.0, -18.0 / 20.0);
    return b;
}

struct BurstMeasure { double firstDb = -999, t60 = 0; };

/** First repeat (dB re the dry burst) and the time the repeats cross -60 dB, through the real processor. */
BurstMeasure measureBurst (int presetIndex, int heads = 0, double seconds = 18.0)
{
    const double sr = 44100.0;
    const auto& q = YankiProcessor::kPresets[presetIndex];
    const double T = q.sync == YankiProcessor::kSyncFree ? (double) q.timeMs / 1000.0 : 0.5 * YankiProcessor::kSyncQuarters[q.sync];
    double ref = 0;
    const double burst = std::min (0.12, 0.45 * T), t0 = 0.05;
    const auto in = pinkBurst (sr, seconds, burst, t0, ref);
    const auto out = run (in, sr, [presetIndex, heads] (YankiProcessor& p)
    {
        p.applyPreset (presetIndex);
        if (heads > 0)
            set (p, yankiIds::heads, (float) heads);
    });
    BurstMeasure m;
    std::vector<double> ser;
    for (int k = 1; k < 80; ++k)
    {
        const int a = (int) ((t0 + k * T) * sr), e = a + (int) ((burst + 0.02) * sr);
        if (e > out.getNumSamples()) break;
        ser.push_back (rmsDb (out, sr, a / sr, e / sr) - 20.0 * std::log10 (ref));
    }
    m.firstDb = ser.empty() ? -999 : ser[0];
    m.t60 = 0;
    for (size_t i = 0; i < ser.size(); ++i)
        if (ser[i] < -60.0)
        {
            const double pr = i == 0 ? ser[0] : ser[i - 1];
            m.t60 = ((double) i + (pr - (-60.0)) / std::max (pr - ser[i], 1e-9)) * T;
            break;
        }
    return m;
}

void v2Checks()
{
    {
        // The seven-entry sync list: a project saved with the old six (1/2 = 4, FREE = 5) must open on the same divisions.
        bool ok = true;
        juce::String detail;
        for (auto [oldIdx, wantIdx, wantMs] : { std::tuple<int, int, double> { 4, 5, 1000.0 }, { 5, 6, 0.0 }, { 3, 3, 500.0 }, { 0, 0, 125.0 } })
        {
            YankiProcessor a;
            juce::MemoryBlock mb;
            a.getStateInformation (mb);
            auto xml = juce::AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize());
            auto tree = juce::ValueTree::fromXml (*xml);
            tree.removeProperty ("stateVersion", nullptr);
            for (auto child : tree)
                if (child.getProperty ("id").toString() == yankiIds::timeSync)
                    child.setProperty ("value", (double) oldIdx, nullptr);
            auto x2 = tree.createXml();
            juce::MemoryBlock mb2;
            juce::AudioProcessor::copyXmlToBinary (*x2, mb2);
            YankiProcessor b;
            b.setStateInformation (mb2.getData(), (int) mb2.getSize());
            const int got = juce::roundToInt (b.apvts.getRawParameterValue (yankiIds::timeSync)->load());
            const double ms = b.resolvedMs (120.0);
            const bool good = got == wantIdx && (wantMs < 1.0 || std::abs (ms - wantMs) < 1e-6);
            ok = ok && good;
            detail << "old " << oldIdx << " -> " << got << " (" << juce::String (ms, 1) << " ms); ";
        }
        check ("sync_list_old_state_maps", ok, detail);
        YankiProcessor c;
        c.apvts.getParameter (yankiIds::timeSync)->setValueNotifyingHost (c.apvts.getParameter (yankiIds::timeSync)->convertTo0to1 (4.0f));
        check ("sync_quarter_dotted", std::abs (c.resolvedMs (120.0) - 750.0) < 1e-6 && std::abs (c.resolvedMs (90.0) - 1000.0) < 1e-6,
               "1/4 dotted: " + juce::String (c.resolvedMs (120.0), 1) + " ms at 120 bpm, " + juce::String (c.resolvedMs (90.0), 1) + " ms at 90 bpm");
    }
    {
        check ("mix_ramp", std::abs (YankiProcessor::mixToEngine (0.3f) - 0.3f) < 1e-9f && std::abs (YankiProcessor::mixToEngine (0.03f) - 0.03f) < 1e-9f && std::abs (YankiProcessor::mixToEngine (0.0f)) < 1e-9f
                               && std::abs (YankiProcessor::mixToEngine (0.015f) - 0.0075f) < 1e-6f,
               "identity from 3 %, 1.5 % -> " + juce::String (YankiProcessor::mixToEngine (0.015f), 4));
    }
    {
        bool ok = true;
        juce::String detail;
        for (int i = 0; i < YankiProcessor::kNumPresets; ++i)
        {
            YankiProcessor p;
            p.applyPreset (i);
            const auto& q = YankiProcessor::kPresets[i];
            auto g = [&p] (const char* id) { return p.apvts.getRawParameterValue (id)->load(); };
            bool good = juce::roundToInt (g (yankiIds::mood)) == q.mood && juce::roundToInt (g (yankiIds::timeSync)) == q.sync
                     && std::abs (g (yankiIds::feedback) - q.feedback) < 1e-4f && std::abs (g (yankiIds::mix) - q.mix) < 1e-4f
                     && std::abs (g (yankiIds::tone) - q.tone) < 0.5f && std::abs (g (yankiIds::wear) - q.wear) < 1e-4f
                     && std::abs (g (yankiIds::spread) - q.spread) < 1e-4f && (g (yankiIds::capOn) > 0.5f) == q.capOn
                     && juce::roundToInt (g (yankiIds::lockBars)) == q.lockBars && std::abs (g (yankiIds::output) - q.outputDb) < 1e-3f
                     && p.currentPreset() == i && ! p.presetEdited();
            // moving MIX marks it edited; the state round trip keeps the index
            p.apvts.getParameter (yankiIds::mix)->setValueNotifyingHost (0.9f);
            good = good && p.presetEdited();
            juce::MemoryBlock mb;
            p.getStateInformation (mb);
            YankiProcessor r;
            r.setStateInformation (mb.getData(), (int) mb.getSize());
            good = good && r.currentPreset() == i;
            ok = ok && good;
            if (! good) detail << i << " ";
        }
        check ("presets_apply_and_state", ok, ok ? "10 presets: every parameter lands, EDITED follows a MIX move, the index survives a state round trip" : "failed: " + detail);
    }
    {
        // The ring model against fresh renders of the eight decaying starting points (burst through the real processor).
        bool ok = true;
        juce::String detail;
        double worstFirst = 0, worstDecay = 0;
        for (int i = 0; i < 8; ++i)
        {
            const auto m = measureBurst (i);
            const auto& q = YankiProcessor::kPresets[i];
            ringmodel::Params P;
            P.mood = q.mood;
            P.timeS = q.sync == YankiProcessor::kSyncFree ? (double) q.timeMs / 1000.0 : 0.5 * YankiProcessor::kSyncQuarters[q.sync];
            P.mix = YankiProcessor::mixToEngine (q.mix);
            P.fbKnob = q.feedback;
            P.tone = q.tone;
            P.wear = q.wear;
            P.spread = q.spread;
            const auto M = ringmodel::compute (P);
            const double dFirst = M.firstDb - m.firstDb;
            const double decayM = m.t60 / 2.0, dDecay = M.decayBars / std::max (decayM, 1e-6) - 1.0;
            worstFirst = std::max (worstFirst, std::abs (dFirst));
            worstDecay = std::max (worstDecay, std::abs (dDecay));
            detail << i + 1 << ": first " << juce::String (m.firstDb, 1) << "/" << juce::String (M.firstDb, 1) << " dB, decay " << juce::String (decayM, 2) << "/" << juce::String (M.decayBars, 2) << " bars; ";
            ok = ok && std::abs (dFirst) < 1.2 && std::abs (dDecay) < 0.2;
        }
        check ("ring_model_matches_renders", ok, "measured/model: " + detail + "worst first " + juce::String (worstFirst, 2) + " dB, worst decay " + juce::String (worstDecay * 100.0, 0) + " %");
    {
        // The same against the HEADS keys: every HEADS state on four representative presets (the model must follow the heads actually playing).
        bool ok = true;
        juce::String detail;
        double worstFirst = 0, worstDecay = 0;
        for (int i : { 2, 3, 6, 7 })
            for (int h = 1; h <= 3; ++h)
            {
                const auto m = measureBurst (i, h, 40.0);
                const auto& q = YankiProcessor::kPresets[i];
                ringmodel::Params P;
                P.mood = q.mood;
                P.heads = h;
                P.timeS = q.sync == YankiProcessor::kSyncFree ? (double) q.timeMs / 1000.0 : 0.5 * YankiProcessor::kSyncQuarters[q.sync];
                P.mix = YankiProcessor::mixToEngine (q.mix);
                P.fbKnob = q.feedback;
                P.tone = q.tone;
                P.wear = q.wear;
                P.spread = q.spread;
                const auto M = ringmodel::compute (P);
                const double dFirst = M.firstDb - m.firstDb;
                const double decayM = m.t60 / 2.0, dDecay = M.decayBars / std::max (decayM, 1e-6) - 1.0;
                worstFirst = std::max (worstFirst, std::abs (dFirst));
                worstDecay = std::max (worstDecay, std::abs (dDecay));
                detail << i + 1 << "/H" << h << ": first " << juce::String (m.firstDb, 1) << "/" << juce::String (M.firstDb, 1) << " dB, decay " << juce::String (decayM, 2) << "/" << juce::String (M.decayBars, 2) << " bars; ";
                ok = ok && std::abs (dFirst) < 1.2 && std::abs (dDecay) < 0.2;
            }
        check ("ring_model_matches_renders_heads", ok, "measured/model: " + detail + "worst first " + juce::String (worstFirst, 2) + " dB, worst decay " + juce::String (worstDecay * 100.0, 0) + " %");
    }
    }
}

/** HEADS (round 5): the key row on the face. 0 = the MOOD's own heads (old projects), 1 / 2 / 3 force one head, two heads left / right, three heads. */
void headsChecks()
{
    const double sr = 44100.0;
    auto chDb = [] (const juce::AudioBuffer<float>& b, int ch, double sr_, double t0, double t1)
    {
        double sum = 0;
        int n = 0;
        for (int i = (int) (t0 * sr_); i < juce::jmin (b.getNumSamples(), (int) (t1 * sr_)); ++i, ++n)
            sum += (double) b.getSample (ch, i) * b.getSample (ch, i);
        return 10.0 * std::log10 (std::max (sum / std::max (1, n), 1e-24));
    };
    {
        // ECHO, 1/8 at 120 bpm (T = 250 ms), no feedback, SPREAD 100 %: repeats at T, 2T, 3T = head 1 (left), head 2 (right), head 3 (left).
        double ref = 0;
        const auto in = pinkBurst (sr, 3.0, 0.08, 0.05, ref);
        double lev[4][3][2] {};
        for (int h = 0; h <= 3; ++h)
        {
            const auto out = run (in, sr, [h] (YankiProcessor& p)
            {
                set (p, yankiIds::timeSync, 1.0f);
                set (p, yankiIds::feedback, 0.0f);
                set (p, yankiIds::mix, 0.4f);
                set (p, yankiIds::spread, 1.0f);
                set (p, yankiIds::heads, (float) h);
            }, 512, true, 120.0);
            for (int k = 1; k <= 3; ++k)
                for (int c = 0; c < 2; ++c)
                    lev[h][k - 1][c] = chDb (out, c, sr, 0.05 + 0.25 * k, 0.05 + 0.25 * k + 0.1);
        }
        const double quiet = -90.0;
        const bool one = lev[1][0][0] > quiet + 30 && lev[1][1][0] < quiet + 30 && lev[1][1][1] < quiet + 30 && lev[1][2][0] < quiet + 30;
        const bool two = lev[2][0][0] > lev[2][0][1] + 6 && lev[2][1][1] > lev[2][1][0] + 6 && lev[2][1][1] > quiet + 30 && lev[2][2][0] < quiet + 30 && lev[2][2][1] < quiet + 30;
        const bool three = lev[3][2][0] > quiet + 30 && lev[3][1][1] > lev[3][1][0] + 6 && lev[3][0][0] > lev[3][0][1] + 6;
        const bool mood0 = std::abs (lev[0][0][0] - lev[1][0][0]) < 1e-6 && lev[0][1][0] < quiet + 30;   // ECHO's own heads = one
        check ("heads_param_voices_the_rings", one && two && three && mood0,
               "ECHO, SPREAD 100 %, level per repeat L/R dB: 1 head " + juce::String (lev[1][0][0], 1) + "/" + juce::String (lev[1][0][1], 1) + " | " + juce::String (lev[1][1][0], 1)
               + " | " + juce::String (lev[1][2][0], 1) + "; 2 heads R2 " + juce::String (lev[2][1][1], 1) + " vs L2 " + juce::String (lev[2][1][0], 1)
               + ", repeat 3 " + juce::String (lev[2][2][0], 1) + "; 3 heads repeat 3 L " + juce::String (lev[3][2][0], 1) + "; 0 = mood's own (1 head)");
    }
    {
        // Old projects: a saved state without HEADS opens on the MOOD's own heads (also when the instance had another value); a saved HEADS comes back.
        YankiProcessor a;
        set (a, yankiIds::mood, 2.0f);
        a.applyPreset (7);   // mood 2
        set (a, yankiIds::heads, 2.0f);
        juce::MemoryBlock mb;
        a.getStateInformation (mb);
        YankiProcessor same;
        same.setStateInformation (mb.getData(), (int) mb.getSize());
        const bool saved = juce::roundToInt (same.apvts.getRawParameterValue (yankiIds::heads)->load()) == 2;
        auto xml = juce::AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize());
        auto tree = juce::ValueTree::fromXml (*xml);
        for (int i = tree.getNumChildren() - 1; i >= 0; --i)
            if (tree.getChild (i).getProperty ("id").toString() == yankiIds::heads)
                tree.removeChild (i, nullptr);
        auto x2 = tree.createXml();
        juce::MemoryBlock mb2;
        juce::AudioProcessor::copyXmlToBinary (*x2, mb2);
        YankiProcessor old;
        set (old, yankiIds::heads, 3.0f);
        old.setStateInformation (mb2.getData(), (int) mb2.getSize());
        const bool legacy = juce::roundToInt (old.apvts.getRawParameterValue (yankiIds::heads)->load()) == 0;
        // a preset, and a MOOD recall, set HEADS back to the mood's own
        YankiProcessor b;
        set (b, yankiIds::heads, 3.0f);
        const bool edited = b.currentPreset() < 0;
        b.applyPreset (2);
        const bool presetResets = juce::roundToInt (b.apvts.getRawParameterValue (yankiIds::heads)->load()) == 0 && ! b.presetEdited();
        set (b, yankiIds::heads, 2.0f);
        const bool marksEdited = b.presetEdited();
        check ("heads_state_old_projects_presets", saved && legacy && edited && presetResets && marksEdited,
               juce::String ("saved HEADS restored: ") + (saved ? "yes" : "NO") + "; state without HEADS -> mood's own: " + (legacy ? "yes" : "NO") + "; preset sets it back: "
               + (presetResets ? "yes" : "NO") + "; moving it marks the preset EDITED: " + (marksEdited ? "yes" : "NO"));
    }
}

/** Power (dB) of the plain channels sum (L+R)/2 and of the stereo pair (mean of L^2, R^2) in a window. */
void spreadLevels (const juce::AudioBuffer<float>& b, double sr, double t0, double t1, double& monoDb, double& totalDb)
{
    double sm = 0, st = 0;
    int n = 0;
    for (int i = (int) (t0 * sr); i < juce::jmin (b.getNumSamples(), (int) (t1 * sr)); ++i, ++n)
    {
        const double l = b.getSample (0, i), r = b.getSample (1, i);
        sm += 0.25 * (l + r) * (l + r);
        st += 0.5 * (l * l + r * r);
    }
    monoDb = 10.0 * std::log10 (std::max (sm / std::max (1, n), 1e-24));
    totalDb = 10.0 * std::log10 (std::max (st / std::max (1, n), 1e-24));
}

/** Regression (round 5): SPREAD at 100 % must not touch the dry, must keep the wet within 3 dB of 50 % and the mono sum within 3 dB of SPREAD 0. */
void spreadChecks()
{
    const double sr = 44100.0;
    const auto in = burst (sr, 9.0, 0.3f, 1.5);
    struct Row { double dryDb, wetTotal, wetMono; };
    auto measure = [&] (int preset, float spread)
    {
        // preset 0..9 = the starting points; 100 + m = the bare MOOD m (all seven voicings, including those no preset uses);
        // 1000 + 10 m + h = MOOD m forced to HEADS h
        const auto out = run (in, sr, [preset, spread] (YankiProcessor& p)
        {
            if (preset >= 1000) { set (p, yankiIds::mood, (float) ((preset - 1000) / 10)); set (p, yankiIds::heads, (float) ((preset - 1000) % 10)); }
            else if (preset >= 100) set (p, yankiIds::mood, (float) (preset - 100));
            else p.applyPreset (preset);
            set (p, yankiIds::spread, spread);
        });
        Row r {};
        double m, t;
        spreadLevels (out, sr, 0.010, 0.065, m, t);      // before the first repeat of any preset: the dry alone
        r.dryDb = t;
        spreadLevels (out, sr, 1.7, 7.0, r.wetMono, r.wetTotal);   // after the burst: the repeats alone
        return r;
    };
    bool ok = true;
    juce::String detail;
    double worstDry = 0, worstWet = 0, worstMono = 0;
    std::vector<int> ids;
    for (int i = 0; i < YankiProcessor::kNumPresets; ++i) ids.push_back (i);
    for (int m = 0; m < 7; ++m) ids.push_back (100 + m);
    for (int m : { 0, 2, 3 }) for (int h = 1; h <= 3; ++h) ids.push_back (1000 + 10 * m + h);   // HEADS forced, from a 1-head and 3-head voicing
    for (size_t i = 0; i < ids.size(); ++i)
    {
        const int id = ids[i];
        const auto a = measure (id, 0.0f), b = measure (id, 0.5f), c = measure (id, 1.0f);
        const double dDry = std::max (std::abs (c.dryDb - a.dryDb), std::abs (b.dryDb - a.dryDb));
        const double dWet = c.wetTotal - b.wetTotal;
        const double dMono = c.wetMono - a.wetMono;
        worstDry = std::max (worstDry, dDry);
        worstWet = std::min (worstWet, dWet);
        worstMono = std::min (worstMono, dMono);
        const bool good = dDry < 0.1 && std::abs (dWet) <= 3.0 && dMono >= -3.0;
        ok = ok && good;
        detail << (id < 100 ? "P" + juce::String (id + 1) : id < 1000 ? "M" + juce::String (id - 100) : "M" + juce::String ((id - 1000) / 10) + "H" + juce::String ((id - 1000) % 10)) << ": dry " << juce::String (dDry, 2) << " wet100-50 " << juce::String (dWet, 2) << " mono100-0 " << juce::String (dMono, 2) << (good ? "; " : " FAIL; ");
    }
    check ("spread_100_keeps_dry_and_wet", ok, detail + "worst dry shift " + juce::String (worstDry, 2) + " dB, worst wet change " + juce::String (worstWet, 2) + " dB, worst mono loss " + juce::String (worstMono, 2) + " dB");
}

/** Stationary noise for `noiseSec`, then silence up to `totalSec`: the tape is full of signal when the input stops. */
juce::AudioBuffer<float> noiseThenSilence (double sr, double noiseSec, double totalSec, float amp)
{
    juce::AudioBuffer<float> b (2, (int) (sr * totalSec));
    b.clear();
    uint32_t seed = 4242u;
    for (int i = 0; i < (int) (sr * noiseSec); ++i)
        for (int c = 0; c < 2; ++c)
        {
            seed = seed * 1664525u + 1013904223u;
            b.setSample (c, i, amp * ((float) (seed >> 8) / 16777216.0f * 2.0f - 1.0f));
        }
    return b;
}

/** A live change of TIME (a sync division, a preset) must not cut the echo that is on the tape. The old engine cleared the tape whenever the new
    time needed the other tape length (3 x TIME past 2 s: 1/2 note at 120 bpm, 1/4 note at 75 bpm), so the tail died within about 1 ms (the tone
    filter's time constant). Written for the November family update, not run when it was written; see STATUS.md "PENDING BUILD". */
void tailChecks()
{
    const double sr = 48000.0;
    const double t0 = 4.0;                                   // the input stops, and the change happens, here (a multiple of 512 samples at 48 kHz)
    const int t0Sample = (int) (t0 * sr);
    const auto noiseIn = noiseThenSilence (sr, t0, 8.0, 0.3f);

    // 1. SYNC only: the new division needs the other tape length. Tail energy 5-205 ms after the change, against a run that had the new division
    //    from the start (same stationary noise on the tape, so the two must agree; a cleared tape reads silence).
    struct Pair { double bpm; int from, to; const char* what; };
    const Pair pairs[] = {
        { 120.0, 1, 5, "1/8 -> 1/2 at 120 bpm (250 -> 1000 ms, short -> long tape)" },
        { 120.0, 5, 1, "1/2 -> 1/8 at 120 bpm (1000 -> 250 ms, long -> short tape)" },
        {  75.0, 2, 3, "1/8. -> 1/4 at 75 bpm (600 -> 800 ms, short -> long tape)" },
        {  75.0, 3, 2, "1/4 -> 1/8. at 75 bpm (800 -> 600 ms, long -> short tape)" },
    };
    for (const auto& q : pairs)
    {
        const auto changed = run (noiseIn, sr, [q] (YankiProcessor& p) { set (p, yankiIds::timeSync, (float) q.from); }, 512, true, q.bpm,
                                  [q, t0Sample] (YankiProcessor& p, FakeHost&, int pos) { if (pos == t0Sample) set (p, yankiIds::timeSync, (float) q.to); });
        const auto steady = run (noiseIn, sr, [q] (YankiProcessor& p) { set (p, yankiIds::timeSync, (float) q.to); }, 512, true, q.bpm);
        const double a = rmsDb (changed, sr, t0 + 0.005, t0 + 0.205), b = rmsDb (steady, sr, t0 + 0.005, t0 + 0.205);
        check ("tail_kept_sync_" + juce::String (q.from) + "_" + juce::String (q.to) + "_" + juce::String ((int) q.bpm),
               finiteBounded (changed) && a >= b - 3.0 && b > -60.0,
               juce::String (q.what) + ": tail 5-205 ms after the change " + juce::String (a, 2) + " dB, steady new division " + juce::String (b, 2) + " dB (need >= -3 dB)");
    }

    // 2. A real preset change (applyPreset: MOOD, SYNC, FEEDBACK, MIX, TONE ... together), tail against the new preset running from the start.
    //    The presets differ in MIX and FEEDBACK, so the new preset's own level is the reference; the old engine gave silence here for the pairs
    //    that cross the tape length.
    struct PresetPair { double bpm; int from, to; };
    const PresetPair presetPairs[] = { { 120.0, 2, 6 }, { 120.0, 6, 2 }, { 120.0, 4, 6 }, { 120.0, 6, 4 }, { 75.0, 2, 4 }, { 75.0, 4, 2 }, { 75.0, 3, 4 }, { 75.0, 4, 3 } };
    for (const auto& q : presetPairs)
    {
        const auto changed = run (noiseIn, sr, [q] (YankiProcessor& p) { p.applyPreset (q.from); }, 512, true, q.bpm,
                                  [q, t0Sample] (YankiProcessor& p, FakeHost&, int pos) { if (pos == t0Sample) p.applyPreset (q.to); });
        const auto steady = run (noiseIn, sr, [q] (YankiProcessor& p) { p.applyPreset (q.to); }, 512, true, q.bpm);
        const double a = rmsDb (changed, sr, t0 + 0.005, t0 + 0.205), b = rmsDb (steady, sr, t0 + 0.005, t0 + 0.205);
        check ("tail_kept_preset_" + juce::String (q.from) + "_" + juce::String (q.to) + "_" + juce::String ((int) q.bpm),
               finiteBounded (changed) && a >= b - 3.0 && b > -60.0,
               juce::String (YankiProcessor::kPresets[q.from].name) + " -> " + YankiProcessor::kPresets[q.to].name + " at " + juce::String (q.bpm, 0) + " bpm: tail 5-205 ms after the change "
                   + juce::String (a, 2) + " dB, new preset from the start " + juce::String (b, 2) + " dB (need >= -3 dB)");
    }

    // 3. No click at the tape re-lay: a steady sine through the same sync changes. The echo's pitch glides with the TIME glide (by design, 250 ms),
    //    so the bound is 3x the larger of the two steady runs' largest step; a mis-ordered re-lay would be a phase jump of about 10x.
    for (const auto& q : { Pair { 75.0, 2, 3, "" }, Pair { 75.0, 3, 2, "" }, Pair { 120.0, 1, 5, "" }, Pair { 120.0, 5, 1, "" } })
    {
        const auto tone = sine (sr, 8.0, 0.3f);
        const auto changed = run (tone, sr, [q] (YankiProcessor& p) { set (p, yankiIds::timeSync, (float) q.from); }, 512, true, q.bpm,
                                  [q, t0Sample] (YankiProcessor& p, FakeHost&, int pos) { if (pos == t0Sample) set (p, yankiIds::timeSync, (float) q.to); });
        const auto fromRun = run (tone, sr, [q] (YankiProcessor& p) { set (p, yankiIds::timeSync, (float) q.from); }, 512, true, q.bpm);
        const auto toRun = run (tone, sr, [q] (YankiProcessor& p) { set (p, yankiIds::timeSync, (float) q.to); }, 512, true, q.bpm);
        const double own = std::max (maxStep (fromRun, sr, t0 - 0.5, t0 + 1.5), maxStep (toRun, sr, t0 - 0.5, t0 + 1.5));
        const double step = maxStep (changed, sr, t0 - 0.5, t0 + 1.5);
        check ("tape_relay_no_click_" + juce::String (q.from) + "_" + juce::String (q.to) + "_" + juce::String ((int) q.bpm), finiteBounded (changed) && step <= 3.0 * own,
               "sine, sync " + juce::String (q.from) + " -> " + juce::String (q.to) + " at " + juce::String (q.bpm, 0) + " bpm: largest step " + juce::String (step, 5)
                   + " against the steady runs' " + juce::String (own, 5) + " (limit 3x)");
    }
}

void undoChecks()   // Cmd+Z / Cmd+Shift+Z (SvlUndoTests.h; counted into the plugin-level totals)
{
    const auto u = svl::undotest::run<YankiProcessor> ("YANKI", [] { return std::make_unique<YankiProcessor>(); },
                                                       { { yankiIds::wear, 0.6f }, { yankiIds::spread, 0.77f }, { yankiIds::mix, 0.33f } },
                                                       [] (YankiProcessor& p) { p.applyPreset (8); });   // "Swell build": mood, sync, feedback, tone, wear, mix ...
    passed += u.pass;
    failed += u.fail;
    check ("undo_redo", u.fail == 0, juce::String (u.pass) + " of " + juce::String (u.pass + u.fail) + " undo checks passed");
}

void typeInChecks()   // family batch 2026-10-12: host type-in text of every float parameter, plus the audit's cases as literals
{
    YankiProcessor p;
    const auto t = svl::paramtexttest::run (p, { yankiIds::timeMs, yankiIds::feedback, yankiIds::capDb, yankiIds::tone, yankiIds::wear, yankiIds::mix,
                                                 yankiIds::output, yankiIds::darken, yankiIds::fineMs, yankiIds::spread },
                                            {}, { yankiIds::feedback });   // FEEDBACK: a bare number is "% of hold" even where the text says "Swell"
    passed += t.pass;
    failed += t.fail;
    check ("typein_all_float_params", t.fail == 0, juce::String (t.pass) + " of " + juce::String (t.pass + t.fail) + " type-in checks passed"
           + (t.failures.isEmpty() ? juce::String() : " first failures: " + t.failures.joinIntoString (" | ", 0, 6)));

    auto val = [&p] (const char* id, const char* text)
    {
        auto* r = dynamic_cast<juce::RangedAudioParameter*> (p.apvts.getParameter (id));
        return r->convertFrom0to1 (r->getValueForText (text));
    };
    struct Case { const char* id; const char* text; float want; float tol; };
    const float H = 0.7f;
    const Case cases[] = {
        { yankiIds::feedback, "50 % of hold", 0.35f, 1e-5f },        // the audit's runaway case: this used to give Swell 100 %
        { yankiIds::feedback, "50 % OF HOLD", 0.35f, 1e-5f }, { yankiIds::feedback, "34 % of hold", 0.34f * H, 1e-5f }, { yankiIds::feedback, "100 % of hold", H, 1e-5f },
        { yankiIds::feedback, "Swell 50 %", H + 0.15f, 1e-5f }, { yankiIds::feedback, "swell 50", H + 0.15f, 1e-5f }, { yankiIds::feedback, "Swell 100 %", 1.0f, 1e-5f },
        { yankiIds::feedback, "Swell 0 %", H, 1e-5f }, { yankiIds::feedback, "HOLD", H, 1e-5f }, { yankiIds::feedback, "hold", H, 1e-5f },
        { yankiIds::feedback, "50", 0.35f, 1e-5f }, { yankiIds::feedback, "34", 0.34f * H, 1e-5f }, { yankiIds::feedback, "0", 0.0f, 0.0f }, { yankiIds::feedback, "100", H, 1e-5f },
        { yankiIds::feedback, "250", H, 1e-5f }, { yankiIds::feedback, "-5", 0.0f, 0.0f }, { yankiIds::feedback, "Swell 400 %", 1.0f, 1e-5f },
        { yankiIds::timeMs, "250", 250.0f, 1e-3f }, { yankiIds::timeMs, "250.0 ms", 250.0f, 1e-3f }, { yankiIds::timeMs, "0.3 s", 300.0f, 1e-3f },
        { yankiIds::timeMs, "20", 50.0f, 0.0f }, { yankiIds::timeMs, "1000 ms", 700.0f, 0.0f },
        { yankiIds::capDb, "-6", -6.0f, 1e-4f }, { yankiIds::capDb, "+3.0 dB", 3.0f, 1e-4f }, { yankiIds::capDb, "99", 12.0f, 0.0f },
        { yankiIds::tone, "2.75 kHz", 2750.0f, 0.5f }, { yankiIds::tone, "2750", 2750.0f, 0.5f }, { yankiIds::tone, "2.75", 2750.0f, 0.5f },
        { yankiIds::tone, "6000 Hz", 6000.0f, 0.5f }, { yankiIds::tone, "6", 6000.0f, 0.5f }, { yankiIds::tone, "6k", 6000.0f, 0.5f },
        { yankiIds::tone, "1.00 kHz", 1000.0f, 0.5f }, { yankiIds::tone, "500", 1000.0f, 0.0f }, { yankiIds::tone, "20000", 8000.0f, 0.0f },
        { yankiIds::wear, "2.5", 0.25f, 1e-5f }, { yankiIds::wear, "10", 1.0f, 0.0f }, { yankiIds::wear, "99", 1.0f, 0.0f }, { yankiIds::wear, "0", 0.0f, 0.0f },
        { yankiIds::darken, "2.5", 0.25f, 1e-5f }, { yankiIds::darken, "7.5", 0.75f, 1e-5f }, { yankiIds::darken, "10", 1.0f, 0.0f },
        { yankiIds::mix, "25 %", 0.25f, 1e-5f }, { yankiIds::mix, "25", 0.25f, 1e-5f }, { yankiIds::mix, "100", 1.0f, 0.0f }, { yankiIds::mix, "250 %", 1.0f, 0.0f },
        { yankiIds::output, "-6", -6.0f, 1e-4f }, { yankiIds::output, "+3.5 dB", 3.5f, 1e-4f }, { yankiIds::output, "-40", -12.0f, 0.0f },
        { yankiIds::fineMs, "-2.5", -2.5f, 1e-4f }, { yankiIds::fineMs, "+5.0 ms", 5.0f, 1e-4f }, { yankiIds::fineMs, "0.001 s", 1.0f, 1e-4f },
        { yankiIds::spread, "40 %", 0.4f, 1e-5f }, { yankiIds::spread, "40", 0.4f, 1e-5f }, { yankiIds::spread, "100 %", 1.0f, 0.0f },
    };
    int bad = 0;
    juce::String first;
    for (const auto& c : cases)
    {
        const float got = val (c.id, c.text);
        if (! (std::abs (got - c.want) <= c.tol)) { if (++bad == 1) first = juce::String (c.id) + " \"" + c.text + "\" gave " + juce::String (got, 5) + " want " + juce::String (c.want, 5); }
    }
    const int n = (int) (sizeof (cases) / sizeof (cases[0]));
    check ("typein_literals", bad == 0, juce::String (n - bad) + " of " + juce::String (n) + " literal cases" + (bad ? " first failure: " + first : juce::String()));
}

// Double-click: the processor's one reset function, then the real knobs and echo-field handles driven through it.
void resetChecks()
{
    constexpr const char* floats[] { yankiIds::timeMs, yankiIds::feedback, yankiIds::capDb, yankiIds::tone, yankiIds::wear, yankiIds::mix,
                                     yankiIds::output, yankiIds::darken, yankiIds::fineMs, yankiIds::spread };
    auto def = [] (YankiProcessor& q, const char* id)
    {
        auto* r = dynamic_cast<juce::RangedAudioParameter*> (q.apvts.getParameter (id));
        return r->convertFrom0to1 (r->getDefaultValue());
    };
    auto value = [] (YankiProcessor& q, const char* id)
    {
        auto* r = dynamic_cast<juce::RangedAudioParameter*> (q.apvts.getParameter (id));
        return r->convertFrom0to1 (r->getValue());
    };
    {   // 1. no preset, MOOD Echo: every parameter resets to its default (the plug-in's own opening state is ECHO)
        YankiProcessor q;
        int bad = 0;
        juce::String first;
        for (auto* id : floats)
            if (std::abs (q.resetValueFor (id) - def (q, id)) > 1.0e-4f) { if (++bad == 1) first = id; }
        check ("reset_no_preset_is_default", bad == 0, juce::String (bad) + " parameters differ from the default" + (bad ? ", first " + first : juce::String()));
    }
    {   // 2. owner 2026-10-12: after any preset or MOOD recall, every control still resets to its parameter default
        int bad = 0;
        juce::String first;
        for (int i = 0; i < YankiProcessor::kNumPresets + 7; ++i)
        {
            YankiProcessor q;
            if (i < YankiProcessor::kNumPresets) q.applyPreset (i);
            else { set (q, yankiIds::mood, (float) (i - YankiProcessor::kNumPresets)); juce::MessageManager::getInstance()->runDispatchLoopUntil (50); }
            for (auto* id : floats)
                if (std::abs (q.resetValueFor (id) - def (q, id)) > 1.0e-4f && ++bad == 1)
                    first = juce::String (i) + " " + id + " reset " + juce::String (q.resetValueFor (id), 4) + " want " + juce::String (def (q, id), 4);
        }
        check ("reset_is_default_after_preset_and_mood", bad == 0, "10 presets + 7 moods" + (bad ? " first failure: " + first : juce::String(", every control resets to its default")));
    }
    {   // 4. the real editor: every KnobBase and the three echo-field handles, double-clicked
        YankiProcessor q;
        std::unique_ptr<juce::AudioProcessorEditor> ed (q.createEditor());
        ed->setSize (YankiEditor::kW, YankiEditor::kH);
        dynamic_cast<YankiEditor*> (ed.get())->snapshotRefresh();
        std::vector<KnobBase*> knobs;
        EchoField* field = nullptr;
        std::function<void (juce::Component&)> walk = [&] (juce::Component& comp)
        {
            for (auto* ch : comp.getChildren())
            {
                if (auto* k = dynamic_cast<KnobBase*> (ch)) knobs.push_back (k);
                if (auto* f = dynamic_cast<EchoField*> (ch)) field = f;
                walk (*ch);
            }
        };
        walk (*ed);
        auto dbl = [] (juce::Component& comp, juce::Point<int> pos)
        {
            const juce::MouseEvent e (juce::Desktop::getInstance().getMainMouseSource(), pos.toFloat(), juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                      &comp, &comp, juce::Time::getCurrentTime(), pos.toFloat(), juce::Time::getCurrentTime(), 2, false);
            comp.mouseDoubleClick (e);
        };
        const char* knobIds[] { yankiIds::mix, yankiIds::timeSync, yankiIds::timeMs, yankiIds::feedback, yankiIds::tone, yankiIds::wear, yankiIds::spread,
                                yankiIds::output, yankiIds::riseCurve, yankiIds::capOn, yankiIds::capDb, yankiIds::darken, yankiIds::fineMs };
        int bad = 0;
        juce::String first;
        for (int stage = 0; stage < 12; ++stage)   // stage 0: no preset; 1..10: preset stage-1; 11: no preset, MOOD Yesilcam
        {
            YankiProcessor pr;
            std::unique_ptr<juce::AudioProcessorEditor> e2 (pr.createEditor());
            e2->setSize (YankiEditor::kW, YankiEditor::kH);
            std::vector<KnobBase*> ks;
            std::function<void (juce::Component&)> w2 = [&] (juce::Component& comp)
            {
                for (auto* ch : comp.getChildren()) { if (auto* k = dynamic_cast<KnobBase*> (ch)) ks.push_back (k); w2 (*ch); }
            };
            w2 (*e2);
            if (stage >= 1 && stage <= 10) pr.applyPreset (stage - 1);
            if (stage == 11) { set (pr, yankiIds::mood, 5.0f); juce::MessageManager::getInstance()->runDispatchLoopUntil (50); }
            // knock every knob-driven parameter off any reset value
            set (pr, yankiIds::mix, 0.77f);  set (pr, yankiIds::timeSync, 5.0f);  set (pr, yankiIds::timeMs, 611.0f);  set (pr, yankiIds::feedback, 0.93f);
            set (pr, yankiIds::tone, 7777.0f);  set (pr, yankiIds::wear, 0.91f);  set (pr, yankiIds::spread, 0.83f);  set (pr, yankiIds::output, 7.0f);
            set (pr, yankiIds::riseCurve, 2.0f);  set (pr, yankiIds::capOn, 1.0f);  set (pr, yankiIds::capDb, -9.0f);  set (pr, yankiIds::darken, 0.6f);
            set (pr, yankiIds::fineMs, 6.0f);
            // the expected values are read BEFORE the clicks (a reset must not depend on order)
            std::map<juce::String, float> want;
            for (auto* id : knobIds) want[id] = pr.resetValueFor (id);
            for (auto* k : ks) dbl (*k, k->getLocalBounds().getCentre());
            for (auto* id : knobIds)
            {
                const float got = value (pr, id);
                const float tol = std::string (id) == yankiIds::tone ? 0.5f : 1.0e-3f;
                if (std::abs (got - want[id]) > tol && ++bad == 1) first = "stage " + juce::String (stage) + " " + id + " is " + juce::String (got, 4) + " want " + juce::String (want[id], 4);
            }
            if (stage == 0 && ks.size() != 13 - 1) { ++bad; first = "found " + juce::String ((int) ks.size()) + " knobs, expected 12 (CAP is one knob over two parameters)"; }
        }
        (void) knobs;
        check ("reset_every_knob_double_click", bad == 0, "12 start states (none, 10 presets, MOOD Yesilcam) x 12 knobs" + (bad ? " first failure: " + first : juce::String(", all at the reset value")));

        // echo field: scan the field with double-clicks; a hit on a handle resets the one parameter it drags
        for (int sync : { (int) YankiProcessor::kSyncFree, 3 })
        {
            int hitTime = 0, hitFb = 0, hitSpread = 0, wrongTime = 0;
            if (field == nullptr) { check ("reset_echo_field_handles", false, "no EchoField in the editor"); break; }
            q.applyPreset (6);   // "Half note long": sync 1/2, mood Echo
            set (q, yankiIds::timeSync, (float) sync);
            auto arm = [&]
            {
                set (q, yankiIds::timeMs, 611.0f);  set (q, yankiIds::feedback, 0.93f);  set (q, yankiIds::spread, 0.83f);
                set (q, yankiIds::timeSync, (float) sync);
            };
            arm();
            const float wantTime = q.resetValueFor (yankiIds::timeMs), wantSync = q.resetValueFor (yankiIds::timeSync), wantFb = q.resetValueFor (yankiIds::feedback),
                        wantSpread = q.resetValueFor (yankiIds::spread);
            for (int y = 0; y < field->getHeight(); y += 3)
                for (int x = 0; x < field->getWidth(); x += 3)
                {
                    dbl (*field, { x, y });
                    const bool tMs = std::abs (value (q, yankiIds::timeMs) - 611.0f) > 0.01f, tSync = (int) std::lround (value (q, yankiIds::timeSync)) != sync;
                    const bool fb = std::abs (value (q, yankiIds::feedback) - 0.93f) > 1.0e-4f, sp = std::abs (value (q, yankiIds::spread) - 0.83f) > 1.0e-4f;
                    if (tMs || tSync) { ++hitTime; if (sync == YankiProcessor::kSyncFree ? (tSync || std::abs (value (q, yankiIds::timeMs) - wantTime) > 0.01f)
                                                                                        : (tMs || (int) std::lround (value (q, yankiIds::timeSync)) != (int) wantSync)) ++wrongTime; }
                    if (fb) { ++hitFb; if (std::abs (value (q, yankiIds::feedback) - wantFb) > 1.0e-4f) ++wrongTime; }
                    if (sp) { ++hitSpread; if (std::abs (value (q, yankiIds::spread) - wantSpread) > 1.0e-4f) ++wrongTime; }
                    if (tMs || tSync || fb || sp) arm();
                }
            check (sync == YankiProcessor::kSyncFree ? "reset_echo_field_time_free" : "reset_echo_field_time_synced",
                   hitTime > 0 && hitFb > 0 && hitSpread > 0 && wrongTime == 0,
                   juce::String ("sync ") + (sync == YankiProcessor::kSyncFree ? "FREE" : "1/4") + ": handle hits time " + juce::String (hitTime) + ", feedback " + juce::String (hitFb)
                       + ", spread " + juce::String (hitSpread) + "; wrong resets " + juce::String (wrongTime)
                       + (sync == YankiProcessor::kSyncFree ? " (TIME FREE handle resets TIME_MS and leaves TIME_SYNC on FREE)" : " (synced TIME handle resets TIME_SYNC, TIME_MS untouched)"));
        }
    }
}

int selftest()
{
    // 1. verbatim engine
    {
        juce::File f = juce::File::getCurrentWorkingDirectory().getChildFile ("Source/YankiEngine.h");
        std::ifstream in (f.getFullPathName().toStdString(), std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        const auto sum = md5hex (ss.str());
        check ("engine_header_verbatim", sum == kEngineMd5, "md5=" + juce::String (sum) + " want " + kEngineMd5);
    }
    // 2. engine selftest
    const int engineRc = engineSelftest();
    check ("engine_selftest", engineRc == 0, "prototype harness exit code " + juce::String (engineRc));

    // 3. plugin layer
    const double sr = 48000.0;
    const auto in = burst (sr, 5.0);

    {
        const auto plug = run (in, sr, {});
        const auto eng = engineRun (in, sr, yanki::defaultControls());
        check ("default_is_engine_echo", identical (plug, eng), "processor defaults at 120 bpm == engine defaultControls(), bit for bit, 512 blocks");
        const auto odd = run (in, sr, {}, 97);
        check ("block_size_independent", identical (plug, odd), "512 vs 97 sample blocks bit-identical");
    }
    {
        YankiProcessor p;
        const auto noHost = run (in, sr, {}, 512, false, 120.0, {}, &p);
        const auto eng = engineRun (in, sr, yanki::defaultControls());
        check ("no_host_tempo_fallback_120", identical (noHost, eng) && std::abs (p.uiBpm.load() - 120.0) < 1e-9 && ! p.uiHostTempo.load(),
               "no playhead: runs at " + juce::String (p.uiBpm.load()) + " bpm, host lamp off, output == engine default");
    }
    {
        YankiProcessor p;
        const auto at90 = run (in, sr, {}, 512, true, 90.0, {}, &p);
        auto c = yanki::defaultControls();
        c.bpm = 90.0;
        const auto eng = engineRun (in, sr, c);
        check ("host_tempo_90", identical (at90, eng) && std::abs (p.uiMs.load() - 1000.0 / 3.0) < 1e-6,
               "host 90 bpm: TIME 1/8 = " + juce::String (p.uiMs.load(), 3) + " ms, output == engine at 90 bpm");
    }
    for (int m = 1; m <= 6; ++m)
    {
        const auto plug = run (in, sr, [m] (YankiProcessor& p) { set (p, yankiIds::mood, (float) m); set (p, yankiIds::timeSync, 6.0f); });
        const auto eng = engineRun (in, sr, yanki::moodControls (m));
        check ("mood_recall_bit_identical_" + juce::String (yanki::kMoodNames[m]).removeCharacters (" "), identical (plug, eng),
               "MOOD " + juce::String (m) + " + TIME on FREE (recalled " + juce::String (yanki::moodControls (m).timeMs) + " ms) == engine moodControls(" + juce::String (m) + ")");
    }
    {
        const auto plug = run (in, sr, [] (YankiProcessor& p) { set (p, yankiIds::bypass, 1.0f); set (p, yankiIds::mood, 3.0f); });
        check ("bypass_bit_exact", identical (plug, in), "BYPASS on: output == input");
    }
    {
        // Tempo change mid-signal: TIME glides, no step larger than steady state.
        const auto s = sine (sr, 3.0, 0.3f);
        const auto out = run (s, sr, [] (YankiProcessor& p) { set (p, yankiIds::mix, 1.0f); }, 512, true, 120.0,
                              [sr] (YankiProcessor&, FakeHost& h, int pos) { if (pos >= (int) (1.5 * sr)) h.bpm = 97.0; });
        const double before = maxStep (out, sr, 0.8, 1.5), after = maxStep (out, sr, 1.5, 2.2);
        check ("tempo_change_no_click", after < before * 1.5, "120->97 bpm at 1.5 s, MIX 1: max step before=" + juce::String (before, 4) + " after=" + juce::String (after, 4));
        const auto div = run (s, sr, [] (YankiProcessor& p) { set (p, yankiIds::mix, 1.0f); }, 512, true, 120.0,
                              [sr] (YankiProcessor& p, FakeHost&, int pos) { if (pos == (int) (1.5 * sr) / 512 * 512) set (p, yankiIds::timeSync, 3.0f); });
        const double b2 = maxStep (div, sr, 0.8, 1.5), a2 = maxStep (div, sr, 1.5, 2.2);
        check ("sync_switch_no_click", a2 < b2 * 1.5, "1/8 -> 1/4 at 1.5 s: max step before=" + juce::String (b2, 4) + " after=" + juce::String (a2, 4));
        const auto mixJump = run (s, sr, {}, 512, true, 120.0,
                                  [sr] (YankiProcessor& p, FakeHost&, int pos) { if (pos == (int) (1.5 * sr) / 512 * 512) { set (p, yankiIds::mix, 1.0f); set (p, yankiIds::tone, 1000.0f); set (p, yankiIds::output, 12.0f); } });
        const double b3 = maxStep (mixJump, sr, 0.8, 1.5), a3 = maxStep (mixJump, sr, 1.5, 1.7);
        const double expect = b3 * juce::Decibels::decibelsToGain (12.0);   // OUTPUT +12 dB scales the waveform itself
        check ("param_jump_no_zipper", a3 < expect * 1.5, "MIX 0.3->1, TONE ->1k, OUTPUT ->+12 dB in one block: max step " + juce::String (a3, 4) + " (steady " + juce::String (b3, 4) + " x4 = " + juce::String (expect, 4) + ")");
    }
    {
        YankiProcessor a;
        set (a, yankiIds::mood, 3.0f);
        set (a, yankiIds::feedback, 0.8f);
        set (a, yankiIds::capOn, 1.0f);
        set (a, yankiIds::capDb, 3.0f);
        set (a, yankiIds::darken, 0.4f);
        set (a, yankiIds::lockBars, 2.0f);
        set (a, yankiIds::riseCurve, 2.0f);
        set (a, yankiIds::fineMs, 3.0f);
        set (a, yankiIds::timeSync, 2.0f);
        juce::MemoryBlock mb;
        a.getStateInformation (mb);
        YankiProcessor b;
        b.setStateInformation (mb.getData(), (int) mb.getSize());
        bool same = true;
        juce::String diff;
        for (auto* prm : a.getParameters())
            if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (prm))
            {
                const float va = r->getValue(), vb = b.apvts.getParameter (r->getParameterID())->getValue();
                if (std::abs (va - vb) > 1e-6f) { same = false; diff << r->getParameterID() << " "; }
            }
        const auto ra = run (in, sr, {}, 512, true, 120.0, {}, &a), rb = run (in, sr, {}, 512, true, 120.0, {}, &b);
        check ("state_round_trip", same && identical (ra, rb), same ? "all " + juce::String (a.getParameters().size()) + " parameters restored, renders identical" : "differs: " + diff);
        YankiProcessor c;
        set (c, yankiIds::mood, 4.0f);
        juce::MemoryBlock mc;
        c.getStateInformation (mc);
        YankiProcessor d;
        d.setStateInformation (mc.getData(), (int) mc.getSize());
        const auto rc = run (in, sr, [] (YankiProcessor& p) { set (p, yankiIds::timeSync, 6.0f); }, 512, true, 120.0, {}, &c);
        const auto rd = run (in, sr, [] (YankiProcessor& p) { set (p, yankiIds::timeSync, 6.0f); }, 512, true, 120.0, {}, &d);
        check ("state_keeps_mood_arithmetic", identical (rc, rd) && identical (rc, engineRun (in, sr, yanki::moodControls (4))),
               "a recalled MOOD saved and reloaded still renders == engine moodControls(4)");
    }
    {
        // Swell: DUB SPIRAL, FEEDBACK past HOLD, CAP +3: the loop climbs to the cap and holds.
        YankiProcessor p;
        const auto phrase = burst (sr, 10.0, 0.3f, 3.0);
        const auto out = run (phrase, sr, [] (YankiProcessor& q)
        {
            set (q, yankiIds::mood, 3.0f);
            set (q, yankiIds::feedback, 0.9f);
            set (q, yankiIds::capOn, 1.0f);
            set (q, yankiIds::capDb, 3.0f);
        }, 512, true, 120.0, {}, &p);
        const double dryDb = rmsDb (phrase, sr, 0.5, 3.0), tailDb = rmsDb (out, sr, 6.0, 9.0);
        check ("swell_holds_at_cap", p.uiSwelling.load() && finiteBounded (out) && tailDb > dryDb - 6.0 && tailDb < dryDb + 6.0,
               "DUB SPIRAL knob 0.9 CAP +3: loop 3-6 s after the phrase " + juce::String (tailDb - dryDb, 2) + " dB re the phrase (output incl. MIX), swelling="
                   + juce::String ((int) p.uiSwelling.load()));
        // END CUT fired on the press (trigger raised and dropped between two blocks).
        YankiProcessor q;
        const auto cut = run (phrase, sr, [] (YankiProcessor& r)
        {
            set (r, yankiIds::mood, 3.0f);
            set (r, yankiIds::feedback, 0.9f);
            set (r, yankiIds::capOn, 1.0f);
            set (r, yankiIds::capDb, 3.0f);
        }, 512, true, 120.0, [sr] (YankiProcessor& r, FakeHost&, int pos)
        {
            if (pos == (int) (6.0 * sr) / 512 * 512)
            {
                set (r, yankiIds::endMode, 1.0f);
                set (r, yankiIds::endTrigger, 1.0f);
                set (r, yankiIds::endTrigger, 0.0f);
            }
        }, &q);
        const double after = rmsDb (cut, sr, 6.2, 9.0);
        check ("end_cut_on_press", after < -80.0 && q.uiEndLatched.load(), "CUT pressed at 6 s (released before the next block): loop 0.2-3 s later " + juce::String (after, 1) + " dBFS, lamp latched");
    }
    {
        YankiProcessor p;
        check ("latency_zero", p.getLatencySamples() == 0, "reported " + juce::String (p.getLatencySamples()) + " samples (engine latency() = 0)");
    }
    {
        // Every mood, every sync division, extremes: finite and bounded, also at 44.1 and 96 kHz.
        bool ok = true;
        juce::String where;
        for (double rate : { 44100.0, 96000.0 })
            for (int m = 0; m <= 6; ++m)
            {
                const auto b = burst (rate, 3.0);
                const auto o = run (b, rate, [m] (YankiProcessor& q)
                {
                    set (q, yankiIds::mood, (float) m);
                    set (q, yankiIds::feedback, 1.0f);
                    set (q, yankiIds::riseCurve, 2.0f);
                    set (q, yankiIds::timeSync, (float) (m % 7));
                    set (q, yankiIds::wear, 1.0f);
                    set (q, yankiIds::output, 12.0f);
                }, 256, true, 60.0);
                if (! finiteBounded (o, 16.0f)) { ok = false; where << (int) rate << "/" << m << " "; }
            }
        check ("extremes_finite", ok, ok ? "7 moods x 44.1/96 kHz, FEEDBACK max, RISE FAST, 60 bpm, OUTPUT +12: finite, |x| < 16" : "failed: " + where);
    }


    regressionChecks();
    v2Checks();
    spreadChecks();
    headsChecks();
    tailChecks();
    undoChecks();
    typeInChecks();
    resetChecks();

    std::cout << (failed == 0 ? "ALL PASS" : "SELFTEST FAILED") << " (" << passed << " plugin-level checks passed, " << failed << " failed; engine selftest "
              << (engineRc == 0 ? "ALL PASS" : "FAILED") << ")\n";
    return failed == 0 ? 0 : 1;
}

int renderFile (int argc, char** argv)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (juce::File (juce::String (argv[1]))));
    if (reader == nullptr)
    {
        std::cerr << "cannot read " << argv[1] << "\n";
        return 2;
    }
    juce::AudioBuffer<float> buf (2, (int) reader->lengthInSamples);
    reader->read (&buf, 0, (int) reader->lengthInSamples, 0, true, true);
    const double sr = reader->sampleRate;
    double bpm = 120.0;
    std::vector<std::pair<const char*, float>> sets;
    std::vector<std::pair<double, int>> timeline;   // --timeline "t_s:preset[:mix],...": applyPreset() live at the block holding t_s (optional absolute MIX)
    std::vector<float> timelineMix;
    float mixScale = 1.0f;                          // --mixscale: scales each timeline preset's MIX
    for (int i = 3; i + 1 < argc; i += 2)
    {
        const juce::String k (argv[i]), v (argv[i + 1]);
        if (k == "--timeline")
        {
            for (auto& tok : juce::StringArray::fromTokens (v, ",", ""))
            {
                const auto parts = juce::StringArray::fromTokens (tok, ":", "");
                timeline.push_back ({ parts[0].getDoubleValue(), parts[1].getIntValue() });
                timelineMix.push_back (parts.size() > 2 ? parts[2].getFloatValue() : -1.0f);
            }
        }
        else if (k == "--mixscale") mixScale = v.getFloatValue();
        else if (k == "--mood") sets.push_back ({ yankiIds::mood, v.getFloatValue() });
        else if (k == "--bpm") bpm = v.getDoubleValue();
        else if (k == "--sync") sets.push_back ({ yankiIds::timeSync, v.getFloatValue() });
        else if (k == "--feedback") sets.push_back ({ yankiIds::feedback, v.getFloatValue() });
        else if (k == "--mix") sets.push_back ({ yankiIds::mix, v.getFloatValue() });
        else if (k == "--time") sets.push_back ({ yankiIds::timeMs, v.getFloatValue() });
        else if (k == "--tone") sets.push_back ({ yankiIds::tone, v.getFloatValue() });
        else if (k == "--wear") sets.push_back ({ yankiIds::wear, v.getFloatValue() });
        else if (k == "--spread") sets.push_back ({ yankiIds::spread, v.getFloatValue() });
        else if (k == "--darken") sets.push_back ({ yankiIds::darken, v.getFloatValue() });
        else if (k == "--fine") sets.push_back ({ yankiIds::fineMs, v.getFloatValue() });
        else if (k == "--rise") sets.push_back ({ yankiIds::riseCurve, v.getFloatValue() });
        else if (k == "--lock") sets.push_back ({ yankiIds::lockBars, v.getFloatValue() });
        else if (k == "--output") sets.push_back ({ yankiIds::output, v.getFloatValue() });
        else if (k == "--heads") sets.push_back ({ yankiIds::heads, v.getFloatValue() });
        else if (k == "--cap")
        {
            if (v == "off") sets.push_back ({ yankiIds::capOn, 0.0f });
            else { sets.push_back ({ yankiIds::capOn, 1.0f }); sets.push_back ({ yankiIds::capDb, v.getFloatValue() }); }
        }
        else { std::cerr << "unknown option " << argv[i] << "\n"; return 2; }
    }
    const auto out = run (buf, sr, [&sets] (YankiProcessor& p)
    {
        // MOOD first: its recall writes the other controls, the options then override them.
        for (auto& [id, val] : sets) if (juce::String (id) == yankiIds::mood) set (p, id, val);
        for (auto& [id, val] : sets) if (juce::String (id) != yankiIds::mood) set (p, id, val);
    }, 512, true, bpm, [&] (YankiProcessor& p, FakeHost&, int pos)
    {
        static size_t next = 0;
        while (next < timeline.size() && (double) pos >= timeline[next].first * sr)
        {
            const int idx = timeline[next].second;
            p.applyPreset (idx);
            if (timelineMix[next] >= 0.0f) set (p, yankiIds::mix, timelineMix[next]);
            else if (mixScale != 1.0f) set (p, yankiIds::mix, YankiProcessor::kPresets[idx].mix * mixScale);
            std::cout << "t=" << (double) pos / sr << " s: preset " << idx << " (" << YankiProcessor::kPresets[idx].name << ")\n";
            ++next;
        }
    });
    juce::File f { juce::String (argv[2]) };
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> os (new juce::FileOutputStream (f));
    auto writer = wav.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (2).withBitsPerSample (24));
    if (writer == nullptr)
    {
        std::cerr << "cannot write " << argv[2] << "\n";
        return 2;
    }
    writer->writeFromAudioSampleBuffer (out, 0, out.getNumSamples());
    return 0;
}
} // namespace

// ---- --controlproof: every knob in every variation (SvlControlProof.h), through the real processor ----------------
// SWELL / END (designed to sustain): the sweep treats feedback past the hold point and FREEZE as "sustains by design" (bounded, no decay
// required). This extra check shows the other half: the sustain stays bounded for 8 s, and END (CUT / DUMP) really ends it.
static bool swellEndCheck (const svl::controlproof::Config& cfg, juce::StringArray& lines)
{
    namespace cp = svl::controlproof;
    cp::Host H (cfg);
    const double sr = 48000.0; const int total = (int) (8.0 * sr);
    auto mt = cp::makeMaterials (sr, 1.0);
    cp::Sig burst = cp::burstThenSilence (mt, 0.5, total), out;
    bool allOk = true;
    struct Case { const char* name; float endMode; bool trigger; float fb; bool freeze; };
    const Case cases[] = { { "swell, no END (bounded sustain)", 0.0f, false, 1.0f, false }, { "swell + END CUT at 3 s", 0.5f, true, 1.0f, false },
                           { "swell + END DUMP at 3 s", 1.0f, true, 1.0f, false }, { "FREEZE (bounded sustain)", 0.0f, false, 0.3f, true } };
    for (auto& cs : cases)
    {
        H.create (true);
        auto id = [&] (const char* name) { for (size_t i = 0; i < H.params().size(); ++i) if (H.params()[i].id == name) return (int) i; return -1; };
        cp::State st = H.defaults();
        st[(size_t) id ("MOOD")] = 3.0f / 6.0f;                 // Dub Spiral: the swell voicing
        st[(size_t) id ("MIX")] = 1.0f; st[(size_t) id ("FEEDBACK")] = cs.fb; st[(size_t) id ("END_MODE")] = cs.endMode;
        H.applyState (st); H.prepare (sr, 512);
        const int trig = id ("END_TRIGGER"), frz = id ("FREEZE");
        // FREEZE holds what the loop contains: engaged at 0.6 s, after the burst has gone in (frozen empty it would hold silence)
        H.render (burst, H.scEnabled() ? &mt.kick : nullptr, total, 512, nullptr, 0, [&] (int sampleAt)
        {
            if (cs.trigger && sampleAt >= (int) (3.0 * sr) && H.params()[(size_t) trig].p->getValue() < 0.5f) H.setOne ((size_t) trig, 1.0f);
            if (cs.freeze && sampleAt >= (int) (0.6 * sr) && H.params()[(size_t) frz].p->getValue() < 0.5f) H.setOne ((size_t) frz, 1.0f);
        }, out);
        double pkAfter = 0, preRms = 0, lastPk = 0; int cnt = 0;
        for (int ch = 0; ch < 2; ++ch)
        {
            for (int i = (int) (0.6 * sr); i < total; ++i) pkAfter = std::max (pkAfter, (double) std::abs (out.c[ch][(size_t) i]));
            for (int i = (int) (2.0 * sr); i < (int) (3.0 * sr); ++i) { preRms += (double) out.c[ch][(size_t) i] * out.c[ch][(size_t) i]; ++cnt; }
            for (int i = total - (int) (0.1 * sr); i < total; ++i) lastPk = std::max (lastPk, (double) std::abs (out.c[ch][(size_t) i]));
        }
        const double pkDb = cp::toDb (pkAfter), preDb = cp::toDb (std::sqrt (preRms / std::max (cnt, 1))), lastDb = cp::toDb (lastPk);
        const bool finite = out.finite();
        const bool bounded = finite && pkDb <= 12.0;   // the absolute bound; the level is printed, a swell past 0 dBFS is reported, not hidden
        const bool sustained = ! cs.trigger ? preDb > -90.0 : true;           // a swell / freeze keeps sounding after the burst
        const bool ended = cs.trigger ? lastDb < -90.0 : true;                 // END has to end it
        const bool ok = bounded && sustained && ended;
        allOk = allOk && ok;
        const juce::String line = juce::String::formatted ("[swell/end] %s: %s  (peak after the burst %.2f dBFS%s, rms 2-3 s %.1f dB, last 100 ms peak %.1f dBFS)", ok ? "PASS" : "FAIL", cs.name, pkDb, pkDb > 0.0 ? " = ABOVE 0 dBFS, within the +12 bound" : "", preDb, lastDb);
        std::printf ("%s\n", line.toRawUTF8()); lines.add (line);
    }
    return allOk;
}

static int runControlProof (int argc, char** argv)
{
    namespace cp = svl::controlproof;
    using cp::feat::devFromMinDb; using cp::feat::outRmsDb; using cp::feat::wetRmsDb; using cp::feat::bandDb; using cp::feat::stereoWidthDb; using cp::feat::outWindowDb;
    cp::Config c;
    c.name = "YANKI";
    c.make = [] { return std::make_unique<YankiProcessor>(); };
    c.applyFirst = { yankiIds::mood };            // a MOOD write recalls TIME, TONE, WEAR, MIX, SPREAD, HEADS and FEEDBACK
    c.recallParams = { yankiIds::mood };
    c.tailSeconds = 10.0;                         // getTailLengthSeconds() = 10 s
    c.latencyForce = { { yankiIds::mix, 0.0f } };
    c.designedGainDb = [] (const cp::StateView& s) { return (double) juce::jmax (0.0f, s.raw (yankiIds::output)) + 6.0; };
    // sustains by design: FREEZE, and FEEDBACK past the hold point (knob 0.7), where the loop swells / self-oscillates (DUB SPIRAL / CAP / END)
    // ... TEMPO LOCK (YankiEngine.h: "lockBars > 0 = tempo lock: loop reaches CAP lockBars bars after the phrase onset": it swells to the cap and stays there until END),
    // ... and an END pulse in mode HOLD (it holds the loop)
    c.selfSustains = [] (const cp::StateView& s)
    {
        return s.on (yankiIds::freeze) || s.raw (yankiIds::feedback) > 0.69f || s.choice (yankiIds::lockBars) > 0
            || (s.choice (yankiIds::endMode) == 0 && s.on (yankiIds::endTrigger));
    };
    c.notes[yankiIds::timeSync] = "1/16 .. 1/2 are ascending times; Free uses TIME, so the list is not one scale";
    using Force = std::vector<std::pair<juce::String, float>>;
    const float kEcho = 0.0f, kFree = 1.0f;
    // every declaration: Echo voicing, wet only (MIX 100 %), feedback below the hold point, no cap / freeze / END pulse
    auto D = [&] (const char* id, int dir, cp::Feature f, double tol, const char* fname, const char* note, Force force = {}, int mat = cp::kDrums)
    {
        cp::Direction d; d.id = id; d.dir = dir; d.feature = f; d.tol = tol; d.featureName = fname; d.note = note; d.material = mat;
        d.force = { { yankiIds::mood, kEcho }, { yankiIds::mix, 1.0f }, { yankiIds::feedback, 0.3f }, { yankiIds::capOn, 0.0f }, { yankiIds::freeze, 0.0f },
                    { yankiIds::endTrigger, 0.0f }, { yankiIds::bypass, 0.0f }, { yankiIds::darken, 0.0f }, { yankiIds::lockBars, 0.0f }, { yankiIds::endMode, 0.0f } };
        if (juce::String (id) != yankiIds::output) d.force.push_back ({ yankiIds::output, 0.5f });   // OUTPUT 0 dB: it would offset a wet-part measurement
        for (auto& x : force) d.force.push_back (x);
        return d;
    };
    const Force freeTime = { { yankiIds::timeSync, kFree }, { yankiIds::fineMs, 0.5f } };
    c.directions = {
        D (yankiIds::mood, 0, nullptr, 0, "", "seven voicings, no ordering"),
        D (yankiIds::timeSync, 0, nullptr, 0, "", "1/16 .. 1/2 ascend, Free hands over to TIME: not one ordered scale"),
        D (yankiIds::timeMs, -1, outWindowDb (0.02, 0.08), 0.5, "output rms 20-100 ms on drums, wet only (dB), TIME SYNC Free", "a later first repeat leaves the window after the first kick", freeTime),
        D (yankiIds::feedback, +1, outRmsDb(), 0.25, "output rms on drums, wet only (dB), Echo voicing", "more feedback = more repeats = more energy", { { yankiIds::feedback, 0.3f } }),
        D (yankiIds::capOn, 0, nullptr, 0, "", "switch: only acts in the swell"),
        D (yankiIds::capDb, 2, outRmsDb(), 0, "output rms (dB), wet only", "the cap acts on the swell level only (CAP ON and feedback past the hold)", { { yankiIds::capOn, 1.0f }, { yankiIds::feedback, 0.85f } }),
        D (yankiIds::tone, +1, bandDb (3000.0, 12000.0), 0.25, "output band 3-12 kHz power on pink, wet only (dB)", "TONE is the top of the repeats, 1..8 kHz", {}, cp::kPink),
        D (yankiIds::wear, +1, devFromMinDb(), 0.25, "rms of (output - the WEAR 0 render) on the chord, wet only (dB), values under -70 dB not judged", "WEAR is the wow depth of the repeats (YankiEngine.h: wowDepth = wear x ...); it does NOT change the top end (measured: the 3-12 kHz level of pink echo moves < 0.1 dB over the knob)", {}, cp::kChord),
        D (yankiIds::mix, +1, wetRmsDb(), 0.25, "wet part = output - input (rms dB) on pink", "0 is the dry signal exactly (pink: an echo of noise is not correlated with the dry, so the wet part cannot partly cancel)", {}, cp::kPink),
        D (yankiIds::output, +1, outRmsDb(), 0.25, "output rms (dB)", "output gain"),
        D (yankiIds::endMode, 0, nullptr, 0, "", "HOLD / CUT / DUMP: what END does, no ordering"),
        D (yankiIds::endTrigger, 0, nullptr, 0, "", "momentary pulse"),
        D (yankiIds::freeze, 0, nullptr, 0, "", "holds the loop; sustains by design"),
        D (yankiIds::bypass, 0, nullptr, 0, "", "switch"),
        D (yankiIds::darken, -1, bandDb (3000.0, 12000.0), 0.25, "output band 3-12 kHz power on pink, wet only (dB), feedback 0.6", "DARKEN = extra per-pass high-cut (YankiEngine.h: darken)", { { yankiIds::feedback, 0.6f } }, cp::kPink),
        D (yankiIds::riseCurve, 0, nullptr, 0, "", "shape of the swell law, no ordering"),
        D (yankiIds::lockBars, 0, nullptr, 0, "", "END timing in bars, no ordering"),
        D (yankiIds::fineMs, -1, outWindowDb (0.02, 0.055), 0.5, "output rms 20-75 ms on drums, wet only (dB), TIME 50 ms", "FINE adds to the free TIME: later repeat, less energy in the window", { { yankiIds::timeSync, kFree }, { yankiIds::timeMs, 0.0f } }),
        D (yankiIds::spread, +1, stereoWidthDb(), 0.25, "side / mid power on the (mono) chord, wet only (dB)", "SPREAD opens the repeats across the stereo field", {}, cp::kChord),
        D (yankiIds::heads, 2, outRmsDb(), 0, "output rms on drums, wet only (dB)", "0 = the mood's own heads, 1..3 fixed: not one scale"),
    };
    for (auto& d : c.directions) if (d.id == yankiIds::wear) d.floorValue = -70.0;
    const int rc = cp::main (c, argc, argv);
    bool onlyOne = false; for (int i = 1; i < argc; ++i) { const juce::String a (argv[i]); if (a == "--checks" || a == "--param" || a == "--probe") onlyOne = true; }
    if (onlyOne) return rc;
    std::printf ("\n--- YANKI SWELL / END ---\n");
    juce::StringArray lines;
    const bool ok = swellEndCheck (c, lines);
    bool noDocs = false; juce::String docs = "docs/CONTROL_PROOF.md";
    for (int i = 1; i < argc; ++i) { const juce::String a (argv[i]); if (a == "--no-docs") noDocs = true; if (a == "--docs" && i + 1 < argc) docs = argv[i + 1]; }
    if (! noDocs)
    {
        juce::File f (docs);
        f.appendText ("\n## SWELL / END (YANKI only, Dub Spiral voicing, wet only, 8 s)\n\nThe sweep treats FEEDBACK past the hold point, FREEZE and END mode HOLD as sustaining by design. These renders show the other half: the sustain stays within the +12 dBFS bound and END (CUT, DUMP) ends it.\n\n");
        for (auto& l : lines) f.appendText ("- " + l + "\n");
    }
    return rc == 0 && ok ? 0 : 1;
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    for (int i = 1; i < argc; ++i)
        if (juce::String (argv[i]).startsWith ("--controlproof")) return runControlProof (argc, argv);
    if (argc >= 2 && juce::String (argv[1]) == "--selftest")
        return selftest();
    if (argc >= 2 && juce::String (argv[1]) == "--regress")   // only the host-like / non-finite regression checks
    {
        regressionChecks();
        v2Checks();
        spreadChecks();
        headsChecks();
        undoChecks();
        typeInChecks();
        resetChecks();
        std::cout << (failed == 0 ? "REGRESS PASS" : "REGRESS FAIL") << " (" << passed << " passed, " << failed << " failed)\n";
        return failed == 0 ? 0 : 1;
    }
    if (argc < 3)
    {
        std::cerr << "usage: render --selftest | render in.wav out.wav [--mood 0-6] [--bpm B] [--sync 0-5] [--feedback 0-1] [--cap dB|off] [--mix 0-1] [--time ms] [--tone Hz] [--wear 0-1] [--spread 0-1] [--darken 0-1] [--fine ms] [--rise 0-2] [--lock 0-3] [--output dB]\n";
        return 2;
    }
    return renderFile (argc, argv);
}
