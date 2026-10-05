/*  audit formats <case>|list|all: odd files through load -> render -> export -> read back (one process per case, so a crash is a finding).
    all = run every case in a child process of this binary. */
#include "audit_common.h"
#include <sys/resource.h>
#include <chrono>

using namespace audit;

namespace
{
Buffer readBack (const juce::File& f, double& rate) { auto a = loadAudioFile (f); rate = a.sampleRate; return std::move (a.buffer); }
double maxDiffAudit (const Buffer& a, const Buffer& b)
{
    if (a.getNumSamples() != b.getNumSamples()) return 1.0e9;
    double m = 0;
    for (int c = 0; c < juce::jmin (a.getNumChannels(), b.getNumChannels()); ++c)
        for (int i = 0; i < a.getNumSamples(); ++i) m = juce::jmax (m, (double) std::abs (a.getSample (c, i) - b.getSample (c, i)));
    return m;
}
struct Case { juce::String name; double rate; int channels, bits; double secs; juce::String kind, ext; };

const std::vector<Case>& cases()
{
    static const std::vector<Case> c {
        { "stereo16_44k", 44100, 2, 16, 10, "music", "wav" }, { "stereo24_48k", 48000, 2, 24, 10, "music", "wav" }, { "stereo32_96k", 96000, 2, 32, 10, "music", "wav" },
        { "stereo24_192k", 192000, 2, 24, 3, "music", "wav" }, { "stereo24_8k", 8000, 2, 24, 4, "music", "wav" },
        { "mono24_48k", 48000, 1, 24, 10, "music", "wav" }, { "mono16_44k", 44100, 1, 16, 10, "music", "wav" },
        { "aiff24_44k", 44100, 2, 24, 8, "music", "aiff" },
        { "ch6_48k", 48000, 6, 24, 5, "music", "wav" },
        { "short_0.5s", 44100, 2, 24, 0.5, "music", "wav" }, { "short_0.1s", 44100, 2, 24, 0.1, "music", "wav" }, { "short_0.01s", 44100, 2, 24, 0.01, "music", "wav" },
        { "one_sample", 44100, 2, 24, 1.0 / 44100.0, "music", "wav" },
        { "silence_5s", 44100, 2, 24, 5, "silence", "wav" }, { "silence_click", 44100, 2, 24, 5, "click", "wav" },
        { "dc_0.5", 44100, 2, 24, 5, "dc", "wav" }, { "square_fs", 44100, 2, 24, 5, "square", "wav" },
        { "float_over_0dB", 44100, 2, 32, 5, "hotfloat", "wav" }, { "float_nan_burst", 44100, 2, 32, 5, "nan", "wav" },
        { "long_10min", 44100, 2, 24, 600, "music", "wav" } };
    return c;
}

Buffer makeSignal (const Case& c)
{
    const int n = juce::jmax (1, (int) std::lround (c.secs * c.rate));
    Buffer b (c.channels, n);
    if (c.kind == "music")
    {
        auto m = synthMix (c.rate, juce::jmax (c.secs, 1.0 / c.rate), 21, juce::jmin (2, c.channels), -4.0);
        for (int ch = 0; ch < c.channels; ++ch)
            for (int i = 0; i < n; ++i) b.setSample (ch, i, i < m.getNumSamples() ? m.getSample (juce::jmin (ch, m.getNumChannels() - 1), i) : 0.0f);
    }
    else
    {
        b.clear();
        for (int i = 0; i < n; ++i)
            for (int ch = 0; ch < c.channels; ++ch)
            {
                float v = 0.0f;
                if (c.kind == "dc") v = 0.5f;
                else if (c.kind == "square") v = std::fmod (i / c.rate * 100.0, 1.0) < 0.5 ? 0.999f : -0.999f;
                else if (c.kind == "click") v = (i == n / 2) ? 0.9f : 0.0f;
                else if (c.kind == "hotfloat") v = (float) (2.0 * std::sin (2 * 3.14159265358979 * 220.0 * i / c.rate));
                else if (c.kind == "nan") v = (i > n / 3 && i < n / 3 + 441) ? std::numeric_limits<float>::quiet_NaN() : (float) (0.3 * std::sin (2 * 3.14159265358979 * 220.0 * i / c.rate));
                b.setSample (ch, i, v);
            }
    }
    return b;
}

bool writeFile (const juce::File& f, const Buffer& b, double rate, int bits, const juce::String& ext)
{
    f.deleteFile();
    std::unique_ptr<juce::AudioFormat> fmt;
    if (ext == "aiff") fmt = std::make_unique<juce::AiffAudioFormat>(); else fmt = std::make_unique<juce::WavAudioFormat>();
    std::unique_ptr<juce::FileOutputStream> os (f.createOutputStream());
    if (os == nullptr) return false;
    std::unique_ptr<juce::AudioFormatWriter> w (fmt->createWriterFor (os.get(), rate, (unsigned) b.getNumChannels(), bits, {}, 0));
    if (w == nullptr) return false;
    os.release();
    return w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
}

int runCase (const Case& c)
{
    auto dir = tmpDir ("formats");
    const auto f = dir.getChildFile (c.name + "." + c.ext);
    const auto sig = makeSignal (c);
    if (! writeFile (f, sig, c.rate, c.bits, c.ext)) { info (c.name + ": could not write the test file (writer refused); skipped"); return 0; }
    auto t0 = std::chrono::steady_clock::now();
    auto a = loadAudioFile (f);
    info (c.name + ": load " + (a.ok() ? "ok" : "FAILED: " + a.error) + ", " + juce::String (a.buffer.getNumChannels()) + " ch, " + juce::String (a.buffer.getNumSamples()) + " samples at " + f2 (a.sampleRate, 0) + " Hz, " + juce::String (a.sourceBits) + " bit, note '" + a.note + "'");
    if (! a.ok())
    {
        if (c.kind == "music" && c.secs >= 0.1) finding ("S2", "FMT_LOAD_" + c.name, "valid file refused: " + a.error);
        else info ("   (refused with a message: acceptable for a degenerate file)");
        return 0;
    }
    if (a.buffer.getNumSamples() != sig.getNumSamples()) finding ("S3", "FMT_LENGTH_" + c.name, "file loads with " + juce::String (a.buffer.getNumSamples()) + " samples, written " + juce::String (sig.getNumSamples()));
    if (hasNonFinite (a.buffer)) finding ("S1", "FMT_LOAD_NONFINITE_" + c.name, "loadAudioFile hands NaN/Inf samples to the chain");
    Rig r (false, "formats");
    r.p.prepareToPlay (a.sampleRate, 512);
    const double rate = a.sampleRate;
    for (auto target : { "STREAM -14", "CLUB -8" })
    {
        auto t1 = std::chrono::steady_clock::now();
        r.load (a.buffer, rate, f.getFileName());
        r.click (target);
        const auto s = r.snap();
        const double secsRender = std::chrono::duration<double> (std::chrono::steady_clock::now() - t1).count();
        struct rusage ru;
        getrusage (RUSAGE_SELF, &ru);
        info (juce::String ("   ") + target + ": in " + f2 (s.inLufs) + " LUFS, out " + f2 (s.outLufs) + " LUFS, TP " + f2 (s.tp) + ", gain " + f2 (s.gainDb) + ", GR max " + f2 (s.grMax) + ", finite " + juce::String ((int) ! s.nonFinite) + ", " + f2 (secsRender, 1) + " s (load+render), peak RSS " + f2 (ru.ru_maxrss / 1048576.0, 0) + " MB | " + r.session().status.substring (0, 100));
        if (s.nonFinite) finding ("S1", "FMT_NONFINITE_" + c.name, juce::String (target) + ": non-finite output from a " + c.kind + " file");
        if (std::isfinite (s.inLufs) && s.gainDb > capFor3 (s) + 0.3 && juce::String (target) != "CLUB -8") finding ("S1", "FMT_GAIN_" + c.name, juce::String (target) + ": gain +" + f2 (s.gainDb) + " dB on " + c.name);
        if (std::isfinite (s.tp) && s.limiterOn && s.tp > s.ceiling + 0.05 && c.kind != "silence") finding ("S1", "FMT_TP_" + c.name, juce::String (target) + ": true peak " + f2 (s.tp) + " above ceiling " + f2 (s.ceiling));
        if (c.secs >= 120.0 && secsRender > 60.0) finding ("S3", "FMT_SLOW_" + c.name, "load + render of " + f2 (c.secs, 0) + " s of audio took " + f2 (secsRender, 1) + " s");
    }
    // Export 24 / 16 and read back.
    for (int bits : { 24, 16 })
    {
        juce::String msg;
        const auto out = dir.getChildFile (c.name + "_out" + juce::String (bits) + ".wav");
        out.deleteFile();
        const bool ok = r.session().exportMaster (out, bits, msg);
        if (! ok) { info ("   export " + juce::String (bits) + ": refused: " + msg); continue; }
        double outRate = 0;
        auto back = readBack (out, outRate);
        const double md = maxDiffAudit (back, *r.session().output);
        const double lsb = std::pow (2.0, -(bits - 1));
        info ("   export " + juce::String (bits) + "-bit: " + juce::String (back.getNumChannels()) + " ch, " + juce::String (back.getNumSamples()) + " samples (master " + juce::String (r.session().output->getNumSamples()) + ") at " + f2 (outRate, 0) + " Hz, max |export - master| " + f2 (md / lsb, 2) + " LSB, TP of file " + f2 (truePeakDb (back)) + " dBTP | " + msg.substring (0, 80));
        if (back.getNumSamples() != r.session().output->getNumSamples() || std::abs (outRate - rate) > 0.5) finding ("S1", "FMT_EXPORT_SHAPE_" + c.name, juce::String (bits) + "-bit export has " + juce::String (back.getNumSamples()) + " samples at " + f2 (outRate, 0) + " Hz, master " + juce::String (r.session().output->getNumSamples()) + " at " + f2 (rate, 0));
        const double limitLsb = bits == 24 ? 1.01 : 2.2;
        if (md / lsb > limitLsb && back.getNumSamples() == r.session().output->getNumSamples())
            finding ("S2", "FMT_EXPORT_ERR_" + c.name + "_" + juce::String (bits), juce::String (bits) + "-bit export differs from the master by " + f2 (md / lsb, 2) + " LSB (expected <= " + f2 (limitLsb, 1) + ")");
        if (std::isfinite (truePeakDb (back)) && truePeakDb (back) > r.session().settings.limiter.ceilingDbtp + 0.1 && c.kind != "silence" && c.kind != "click")
            finding ("S2", "FMT_EXPORT_TP_" + c.name + "_" + juce::String (bits), juce::String (bits) + "-bit file true peak " + f2 (truePeakDb (back)) + " dBTP is above the " + f2 (r.session().settings.limiter.ceilingDbtp) + " dBTP ceiling after quantisation");
    }
    info ("   total " + f2 (std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count(), 1) + " s");
    return 0;
}
} // namespace

int auditFormats (int argc, char** argv)
{
    const juce::String which = argc > 2 ? juce::String (argv[2]) : juce::String ("list");
    if (which == "list") { for (auto& c : cases()) info (c.name); return 0; }
    if (which == "all")
    {
        int bad = 0;
        for (auto& c : cases())
        {
            const auto cmd = juce::String ("\"") + juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName() + "\" formats " + c.name + " 2>&1";
            FILE* p = popen (cmd.toRawUTF8(), "r");
            char line[4096];
            while (p != nullptr && std::fgets (line, sizeof line, p)) std::fputs (line, stdout);
            const int rc = p != nullptr ? pclose (p) : -1;
            if (rc != 0) { ++bad; finding ("S1", "FMT_CRASH_" + c.name, "process exited with status " + juce::String (rc) + " (crash or abort) on " + c.name); }
        }
        return bad > 0 ? 1 : 0;
    }
    for (auto& c : cases()) if (c.name == which) return runCase (c);
    info ("unknown case");
    return 1;
}
