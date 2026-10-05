#pragma once
/*  Shared helpers of the audit tools (tests/audit_*.cpp). The auditor drives the real Session / MihenkProcessor / MainComponent
    headless, the same way ui_check does, and writes findings as one line each (FINDING ...) plus TSV tables to the
    audit output folder. Nothing here edits Source/. Rerun after the developer's changes: tools/audit_run.sh.

    API used (all existing public / testing hooks): Session::setInputForTesting + renderNowForTesting, MainComponent::setFace /
    turnKnobForTesting, button clicks through juce::Button::triggerClick on the real CapButton / KeyButton children.
*/
#include <JuceHeader.h>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include "../Source/app/MainComponent.h"
#include "../Source/plugin/PluginProcessor.h"
#include "../Source/engine/AudioIO.h"
#include "../Source/engine/Preset.h"
#include "../Source/engine/Batch.h"
#include "../Source/dsp/Loudness.h"

namespace audit
{
using namespace mihenk;
using Buffer = juce::AudioBuffer<float>;

inline juce::File rootDir() { return juce::File (__FILE__).getParentDirectory().getParentDirectory(); }
inline juce::File corpusDir() { return rootDir().getChildFile ("tests/corpus"); }
inline juce::File outDir()
{
    const char* e = std::getenv ("AUDIT_OUT");
    auto d = e != nullptr ? juce::File (e) : rootDir().getChildFile ("build-audit/audit_out");
    d.createDirectory();
    return d;
}
inline juce::File tmpDir (const juce::String& name)
{
    auto d = outDir().getChildFile ("tmp_" + name);
    d.createDirectory();
    return d;
}

inline int nFindings = 0;
inline juce::File findingsFile() { return outDir().getChildFile ("findings.tsv"); }
/** One finding: severity S1 (data loss / wrong audio) .. S4 (cosmetic), a stable id, and the measured text. */
inline void finding (const char* sev, const juce::String& id, const juce::String& text)
{
    ++nFindings;
    std::printf ("FINDING %s [%s] %s\n", sev, id.toRawUTF8(), text.toRawUTF8());
    std::fflush (stdout);
    findingsFile().appendText (juce::String (sev) + "\t" + id + "\t" + text.replaceCharacter ('\t', ' ').replaceCharacter ('\n', ' ') + "\n");
}
inline void info (const juce::String& s) { std::printf ("%s\n", s.toRawUTF8()); std::fflush (stdout); }

inline double toDb (double lin) { return lin > 1.0e-12 ? 20.0 * std::log10 (lin) : -240.0; }

inline Buffer loadCorpus (const juce::String& stem, double maxSeconds = 0.0)
{
    auto a = loadAudioFile (corpusDir().getChildFile (stem + ".wav"));
    if (! a.ok()) { info ("cannot load corpus " + stem + ": " + a.error); std::exit (2); }
    if (maxSeconds > 0.0 && a.buffer.getNumSamples() > (int) (maxSeconds * a.sampleRate))
    {
        Buffer cut (a.buffer.getNumChannels(), (int) (maxSeconds * a.sampleRate));
        for (int c = 0; c < cut.getNumChannels(); ++c) cut.copyFrom (c, 0, a.buffer, c, 0, cut.getNumSamples());
        return cut;
    }
    return std::move (a.buffer);
}
inline double corpusRate() { return 44100.0; }

inline juce::StringArray corpusStems()
{
    juce::StringArray s;
    for (auto& f : corpusDir().findChildFiles (juce::File::findFiles, false, "*.wav")) s.add (f.getFileNameWithoutExtension());
    s.sort (false);
    return s;
}

/** FNV-1a of the float bit patterns: two renders are bit-identical when the hashes agree. */
inline uint64_t hashBuffer (const Buffer& b)
{
    uint64_t h = 1469598103934665603ull;
    for (int c = 0; c < b.getNumChannels(); ++c)
    {
        const float* x = b.getReadPointer (c);
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            uint32_t u;
            std::memcpy (&u, &x[i], 4);
            h ^= u;
            h *= 1099511628211ull;
        }
    }
    return h;
}
inline bool hasNonFinite (const Buffer& b)
{
    for (int c = 0; c < b.getNumChannels(); ++c)
    {
        const float* x = b.getReadPointer (c);
        for (int i = 0; i < b.getNumSamples(); ++i) if (! std::isfinite (x[i])) return true;
    }
    return false;
}
inline double samplePeakDb (const Buffer& b)
{
    float m = 0.0f;
    for (int c = 0; c < b.getNumChannels(); ++c) m = juce::jmax (m, b.getMagnitude (c, 0, b.getNumSamples()));
    return toDb (m);
}
inline double truePeakDb (const Buffer& b) { return toDb (truePeakMax (b, 4)); }
inline double maxDiff (const Buffer& a, const Buffer& b)
{
    if (a.getNumSamples() != b.getNumSamples() || a.getNumChannels() != b.getNumChannels()) return 1.0e9;
    double m = 0.0;
    for (int c = 0; c < a.getNumChannels(); ++c)
        for (int i = 0; i < a.getNumSamples(); ++i) m = juce::jmax (m, (double) std::abs (a.getSample (c, i) - b.getSample (c, i)));
    return m;
}

// ---- component walking ---------------------------------------------------------------------------------------------
inline bool chainVisible (const juce::Component* c, const juce::Component* root)
{
    for (; c != nullptr; c = c->getParentComponent())
    {
        if (c == root) return true;       // the panel itself is never shown in a headless run
        if (! c->isVisible()) return false;
    }
    return true;
}
template <typename T> void collectOf (juce::Component* c, std::vector<T*>& out)
{
    for (auto* ch : c->getChildren())
    {
        if (auto* t = dynamic_cast<T*> (ch)) out.push_back (t);
        collectOf<T> (ch, out);
    }
}
inline juce::Button* findButton (juce::Component& root, const juce::String& text, bool visibleOnly = true)
{
    std::vector<juce::Button*> all;
    collectOf<juce::Button> (&root, all);
    for (auto* b : all)
        if (b->getButtonText() == text && (! visibleOnly || chainVisible (b, &root)) && b->isEnabled()) return b;
    return nullptr;
}

/** A click as JUCE does it: triggerClick posts a command message, so the message loop must run for the button's onClick to fire. */
inline void fire (juce::Button* b)
{
    b->triggerClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil (60);
}

/** Everything a measurement of the current master needs. */
struct Snap
{
    double inLufs = 0, outLufs = 0, gainDb = 0, tp = 0, sp = 0, grMax = 0, grMean = 0, drive = 0, trim = 0, ceiling = 0, target = 0, lra = 0;
    uint64_t hash = 0;
    bool nonFinite = false, bypass = false, limiterOn = true, targetLock = true;
    juce::String status, warn;
};

/** One real panel on one real processor in the standalone app mode, no audio device. */
struct Rig
{
    MihenkProcessor p;
    std::unique_ptr<MainComponent> panel;
    bool advanced = false;
    juce::File tmp;

    explicit Rig (bool adv = false, const juce::String& name = "rig")
    {
        tmp = tmpDir (name);
        ui::UiPrefs::setFileForTesting (tmp.getChildFile ("ui.json"));
        p.setStandaloneModeForTesting (true);
        p.setRateAndBufferSizeDetails (48000.0, 512);
        session().profile.setFolder (tmp.getChildFile ("profile"));
        panel = std::make_unique<MainComponent> (p);
        panel->setPresetsFolder (tmp.getChildFile ("presets"));
        advanced = adv;
        panel->setFace (adv, false);
    }
    Session& session() { return p.getSession(); }
    void sync() { panel->setFace (advanced, false); }
    void load (const Buffer& b, double rate, const juce::String& name = "audit.wav")
    {
        Buffer copy (b.getNumChannels(), b.getNumSamples());
        for (int c = 0; c < copy.getNumChannels(); ++c) copy.copyFrom (c, 0, b, c, 0, b.getNumSamples());
        session().setInputForTesting (std::move (copy), rate, name);
        render();
    }
    void render() { session().renderNowForTesting(); sync(); }
    /** Clicks the button with this text. The v2 starting-point buttons map to the v3 CHARACTER buttons when the tree has no starting points. */
    bool click (const juce::String& text)
    {
        auto* b = findButton (*panel, text);
        if (b == nullptr && text == "HIP-HOP / RAP") b = findButton (*panel, "FULL");
        if (b == nullptr && text == "R&B / POP") b = findButton (*panel, "GENTLE");
        if (b == nullptr && text == "ELECTRONIC") b = findButton (*panel, "OFF");
        if (b != nullptr) { fire (b); render(); return true; }
        return false;
    }
    void face (bool adv) { advanced = adv; panel->setFace (adv, false); }

    Snap snap()
    {
        auto& s = session();
        Snap r;
        r.inLufs = s.inMeas.integrated;
        if (s.output != nullptr)
        {
            r.outLufs = integratedLoudness (*s.output, s.sampleRate);
            r.tp = truePeakDb (*s.output);
            r.sp = samplePeakDb (*s.output);
            r.hash = hashBuffer (*s.output);
            r.nonFinite = hasNonFinite (*s.output);
            r.lra = measureLoudness (*s.output, s.sampleRate).lra;
        }
        r.gainDb = r.outLufs - r.inLufs;
        r.grMax = s.limGr.maxDb;
        r.grMean = s.limGr.meanDb;
        r.drive = s.settings.limiter.driveDb;
        r.trim = s.inputStage.effectiveTrimDb;
        r.ceiling = s.settings.limiter.ceilingDbtp;
        r.target = s.settings.targetLufs;
        r.bypass = s.settings.bypass;
        r.limiterOn = s.settings.limiter.enabled;
        r.targetLock = s.settings.targetLock;
        r.status = s.status;
        r.warn = s.warnings.joinIntoString (" | ");
        return r;
    }
};

/** The SPEC_V3 section 2 gain cap: 3 dB within 6 LU of the target, 8 dB otherwise. */
inline double capFor3 (const Snap& s) { return std::abs (s.inLufs - s.target) <= 6.0 ? 3.0 : 8.0; }

inline juce::String f2 (double v, int d = 2) { return std::isfinite (v) ? juce::String (v, d) : juce::String ("-inf"); }

/** A kick / bass / noise mix with a quiet verse and a loud chorus (same recipe as ui_check). */
inline Buffer synthMix (double fs, double secs, uint32_t seed, int channels = 2, double peakDb = -3.0)
{
    const int n = (int) (secs * fs);
    Buffer b (channels, n);
    juce::Random rng ((juce::int64) seed);
    double lp[2] { 0.0, 0.0 };
    for (int i = 0; i < n; ++i)
    {
        const double t = i / fs, tb = std::fmod (t, 0.5), section = std::fmod (t, 8.0) < 4.0 ? 0.35 : 1.0;
        const double kick = std::exp (-tb * 16.0) * std::sin (2.0 * 3.14159265358979 * (50.0 * tb + 2.0 * (1.0 - std::exp (-tb * 30.0))));
        const double bass = 0.2 * std::sin (2.0 * 3.14159265358979 * 55.0 * t);
        for (int c = 0; c < channels; ++c)
        {
            const double w = rng.nextDouble() * 2.0 - 1.0;
            lp[c] += 0.08 * (w - lp[c]);
            const double pad = 0.7 * lp[c] * 0.8 + 0.3 * (w - lp[c]) * 0.25;
            b.setSample (c, i, (float) (section * (0.5 * kick + bass + 0.3 * pad)));
        }
    }
    const float pk = b.getMagnitude (0, n);
    if (pk > 0.0f) b.applyGain ((float) (std::pow (10.0, peakDb / 20.0) / pk));
    return b;
}
} // namespace audit
