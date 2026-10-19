/*  plugin_check: the JING FANG processor against the raw renderer, offline (no host, no screen).

        plugin_check [--root <studio folder>] [--svl <path to build/svl>]

    1. Every factory preset: the same notes rendered through the processor (MIDI at the exact sample, blocks of 512) and through
       `svl render` (24-bit WAV). Compared as 24-bit integers: identical, or the difference is reported (max |diff| in LSB, the
       first differing sample).
    2. Block-size independence of the processor (64 / 333 / 4096 vs 512).
    3. State: getState -> a new processor -> the same exact values; the state is a preset file `svl` reads (0 warnings); the
       loaded preset's values (double-click targets) survive.
    4. Host parameter move reaches the engine; preset load reaches the engine exactly (not through the 0..1 round trip).
    5. Keyswitch: micro + keyswitch on; a keyswitch key moves its pitch class, never sounds, and is written back to the host
       parameter.
    6. Panel notes: a row note plays key + steps x 12 / edo (checked against svl::Engine::noteOnPitch); normal-mode panel note =
       the MIDI note.
    Exit code 0 only if every check held.
*/
#include <set>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include <cstdio>

namespace
{
int gPass = 0, gFail = 0;
void check (bool ok, const juce::String& what)
{
    std::printf ("[%s] %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8());
    std::fflush (stdout);
    ok ? ++gPass : ++gFail;
}

struct Note { float pitch, vel; double start, len; };

std::vector<Note> loadNotes (const juce::File& f)
{
    std::vector<Note> out;
    juce::StringArray lines; lines.addLines (f.loadFileAsString());
    for (auto l : lines)
    {
        l = l.upToFirstOccurrenceOf ("#", false, false).trim();
        juce::StringArray t; t.addTokens (l, " \t", ""); t.removeEmptyStrings();
        if (t.size() >= 4) out.push_back ({ t[0].getFloatValue(), t[1].getFloatValue(), t[2].getDoubleValue(), t[3].getDoubleValue() });
    }
    return out;
}

struct Stereo { std::vector<int> l, r; };   // 24-bit integer samples

int q24 (float x)
{
    double v = x;
    if (v > 1.0) v = 1.0; else if (v < -1.0) v = -1.0;
    return (int) std::lround (v * 8388607.0);
}

bool readWav24 (const juce::File& f, Stereo& s)
{
    juce::MemoryBlock mb;
    if (! f.loadFileAsData (mb) || mb.getSize() < 44) return false;
    const auto* d = static_cast<const unsigned char*> (mb.getData());
    size_t p = 12;
    while (p + 8 <= mb.getSize())
    {
        const juce::String id (reinterpret_cast<const char*> (d + p), 4);
        const uint32_t len = (uint32_t) d[p + 4] | ((uint32_t) d[p + 5] << 8) | ((uint32_t) d[p + 6] << 16) | ((uint32_t) d[p + 7] << 24);
        if (id == "data")
        {
            const size_t frames = len / 6;
            for (size_t i = 0; i < frames; ++i)
                for (int c = 0; c < 2; ++c)
                {
                    const unsigned char* b = d + p + 8 + i * 6 + (size_t) c * 3;
                    int v = (int) b[0] | ((int) b[1] << 8) | ((int) b[2] << 16);
                    if (v & 0x800000) v -= 0x1000000;
                    (c == 0 ? s.l : s.r).push_back (v);
                }
            return true;
        }
        p += 8 + len;
    }
    return false;
}

/** the notes through the processor, as `svl render` places them: velocity / 127, on and off at the rounded sample, sorted stably */
std::vector<float> renderProcessor (JingFangProcessor& proc, const std::vector<Note>& notes, int sr, int block, double tail, std::vector<float>* right = nullptr)
{
    struct Ev { long at; bool on; int key; int vel; };
    std::vector<Ev> evs;
    long lastOff = 0;
    for (const auto& n : notes)
    {
        const long on = std::lround (n.start * sr), off = std::lround ((n.start + n.len) * sr);
        evs.push_back ({ on, true, (int) n.pitch, (int) std::lround (n.vel) });
        evs.push_back ({ off, false, (int) n.pitch, 0 });
        lastOff = std::max (lastOff, off);
    }
    std::stable_sort (evs.begin(), evs.end(), [] (const Ev& a, const Ev& b) { return a.at < b.at; });
    const long total = lastOff + std::lround (tail * sr);
    // as `svl render --tail 4`: where the notes and the 4 s tail end, the block is cut and the patterns stop (arp, seq, drums
    // off); the tails ring on
    const long stopAt = lastOff + std::lround (4.0 * sr);
    proc.setRateAndBufferSizeDetails (sr, block);
    proc.prepareToPlay (sr, block);
    std::vector<float> L ((size_t) total), Rr ((size_t) total);
    juce::AudioBuffer<float> buf (2, block);
    size_t k = 0;
    for (long pos = 0, n = 0; pos < total; pos += n)
    {
        n = std::min ((long) block, total - pos);
        if (pos < stopAt) n = std::min (n, stopAt - pos);
        if (pos == stopAt)
            for (auto id : { "arp.on", "seq.on", "drums.on" })
                if (auto* hp = proc.apvts.getParameter (id)) hp->setValueNotifyingHost (0.0f);
        buf.setSize (2, n, false, false, true);
        juce::MidiBuffer midi;
        while (k < evs.size() && evs[k].at < pos + n)
        {
            const auto& e = evs[k++];
            midi.addEvent (e.on ? juce::MidiMessage::noteOn (1, e.key, (juce::uint8) juce::jlimit (1, 127, e.vel))
                                : juce::MidiMessage::noteOff (1, e.key), (int) (e.at - pos));
        }
        proc.processBlock (buf, midi);
        std::copy (buf.getReadPointer (0), buf.getReadPointer (0) + n, L.begin() + pos);
        std::copy (buf.getReadPointer (1), buf.getReadPointer (1) + n, Rr.begin() + pos);
    }
    if (right != nullptr) *right = Rr;
    return L;
}

juce::String noteFileFor (const juce::String& family)
{
    if (family == "Drums - Kick") return "drum_kick.txt";
    if (family == "Drums - Snare") return "drum_snare.txt";
    if (family == "Drums - Hat") return "drum_hat.txt";
    if (family == "Bass - 808") return "bass_808.txt";
    if (family == "Bass") return "saw_bass.txt";
    if (family == "Pad") return "pad.txt";
    if (family.startsWith ("Keys")) return "keys.txt";
    if (family == "Guitar") return "ks_string.txt";
    return "pluck.txt";
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    juce::File root = juce::File::getCurrentWorkingDirectory();
    juce::File svlBin;
    for (int i = 1; i + 1 < argc; ++i)
    {
        const juce::String a (argv[i]);
        if (a == "--root") root = juce::File (juce::String (argv[++i]));
        else if (a == "--svl") svlBin = juce::File (juce::String (argv[++i]));
    }
    if (svlBin == juce::File()) svlBin = root.getChildFile ("build/svl");
    const auto outDir = root.getChildFile ("out/plugin_check");
    outDir.createDirectory();
    std::printf ("plugin_check: root %s, svl %s\n", root.getFullPathName().toRawUTF8(), svlBin.getFullPathName().toRawUTF8());
    check (svlBin.existsAsFile(), "build/svl exists");

    constexpr int sr = 44100;
    // ---- 1. every factory preset against `svl render`
    {
        auto files = root.getChildFile ("presets").findChildFiles (juce::File::findFiles, true, "*.xml");
        files.sort();
        JingFangProcessor probe;
        int factory = 0;
        for (const auto& e : probe.presets()) if (e.factory) ++factory;
        check (factory == files.size(), "factory presets compiled in = files in presets/ (" + juce::String (factory) + " / " + juce::String (files.size()) + ")");
        int identical = 0;
        for (const auto& f : files)
        {
            svl::Preset pr;
            svl::loadPreset (f.getFullPathName().toStdString(), pr);
            const auto notesFile = root.getChildFile ("notes").getChildFile (noteFileFor (pr.meta.family));
            const auto notes = loadNotes (notesFile);
            const auto wav = outDir.getChildFile (f.getFileNameWithoutExtension().replaceCharacter (' ', '_') + "_svl.wav");
            juce::ChildProcess cp;
            juce::StringArray cmd { svlBin.getFullPathName(), "render", "--preset", f.getFullPathName(), "--notes", notesFile.getFullPathName(),
                                    "--out", wav.getFullPathName(), "--sr", "44100", "--block", "512", "--tail", "4" };
            const bool ran = cp.start (cmd) && cp.waitForProcessToFinish (60000) && cp.getExitCode() == 0;
            Stereo ref;
            if (! ran || ! readWav24 (wav, ref)) { check (false, f.getFileName() + ": svl render failed"); continue; }

            JingFangProcessor proc;
            proc.loadPresetXml (f.loadFileAsString());
            std::vector<float> R;
            // svl renders until the tail is silent (up to 60 s): render the plug-in at least as long as the svl file
            const auto L = renderProcessor (proc, notes, sr, 512, juce::jmax (4.0, (double) ref.l.size() / sr + 0.1), &R);
            long maxDiff = 0, firstDiff = -1, beyond = 0;
            const size_t n = ref.l.size();
            for (size_t i = 0; i < std::min (n, L.size()); ++i)
            {
                const long d = std::max (std::labs ((long) q24 (L[i]) - ref.l[i]), std::labs ((long) q24 (R[i]) - ref.r[i]));
                if (d > 0 && firstDiff < 0) firstDiff = (long) i;
                maxDiff = std::max (maxDiff, d);
            }
            for (size_t i = n; i < L.size(); ++i) if (q24 (L[i]) != 0 || q24 (R[i]) != 0) ++beyond;   // svl trims the silent tail
            const bool same = maxDiff == 0 && firstDiff < 0 && beyond == 0 && L.size() >= n;
            if (same) ++identical;
            juce::String what;
            what << pr.meta.family << " / " << pr.meta.name << " (" << notesFile.getFileName() << ", " << (int) n << " frames): ";
            if (same) what << "identical to svl render (24-bit)";
            else what << "max diff " << (int) maxDiff << " LSB, first at sample " << (int) firstDiff << ", " << (int) beyond << " non-zero samples after the svl file ends";
            check (same, what);
        }
        std::printf ("  presets identical: %d / %d\n", identical, files.size());
        // negative control: the comparison must see a difference when the sounds differ (Glass Pluck vs svl's Saw Pluck file)
        {
            Stereo ref;
            readWav24 (outDir.getChildFile ("Saw_Pluck_svl.wav"), ref);
            JingFangProcessor proc;
            proc.loadPresetXml (root.getChildFile ("presets/Pluck/Glass Pluck.xml").loadFileAsString());
            const auto L = renderProcessor (proc, loadNotes (root.getChildFile ("notes/pluck.txt")), sr, 512, 4.0);
            long maxDiff = 0;
            for (size_t i = 0; i < std::min (ref.l.size(), L.size()); ++i) maxDiff = std::max (maxDiff, std::labs ((long) q24 (L[i]) - ref.l[i]));
            check (! ref.l.empty() && maxDiff > 1000, "negative control: Glass Pluck vs Saw Pluck's svl file differ (max " + juce::String ((int) maxDiff) + " LSB)");
        }
    }

    // ---- 2. block sizes through the processor
    {
        const auto f = root.getChildFile ("presets/Pluck/Saw Pluck.xml");
        const auto notes = loadNotes (root.getChildFile ("notes/pluck.txt"));
        JingFangProcessor a; a.loadPresetXml (f.loadFileAsString());
        const auto ref = renderProcessor (a, notes, sr, 512, 1.0);
        for (int b : { 64, 333, 4096 })
        {
            JingFangProcessor p; p.loadPresetXml (f.loadFileAsString());
            const auto x = renderProcessor (p, notes, sr, b, 1.0);
            float d = 0;
            for (size_t i = 0; i < ref.size(); ++i) d = std::max (d, std::abs (x[i] - ref[i]));
            check (d == 0.0f, "block " + juce::String (b) + " vs 512: max difference " + juce::String (d));
        }
    }

    // ---- 3. state
    {
        JingFangProcessor a;
        a.loadPresetXml (root.getChildFile ("presets/Pad/Warm Pad.xml").loadFileAsString());
        auto* cut = a.apvts.getParameter ("filter.cutoff");
        cut->setValueNotifyingHost (cut->convertTo0to1 (1234.0f));
        a.setRateAndBufferSizeDetails (sr, 512); a.prepareToPlay (sr, 512);
        juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer m; a.processBlock (buf, m);
        juce::MemoryBlock st; a.getStateInformation (st);
        JingFangProcessor b;
        b.setStateInformation (st.getData(), (int) st.getSize());
        int diffs = 0;
        for (int i = 0; i < svl::kNumParams; ++i) if (a.exactValue (i) != b.exactValue (i)) ++diffs;
        check (diffs == 0, "state round trip: " + juce::String (diffs) + " of " + juce::String (svl::kNumParams) + " values differ");
        svl::Preset rp;
        const bool readable = svl::presetFromXml (st.toString().toStdString(), rp);
        check (readable && rp.warnings.empty(), "the state is a preset file svl reads (" + juce::String ((int) rp.warnings.size()) + " warnings)");
        check (b.currentName() == "Warm Pad" && b.currentFamily() == "Pad", "state keeps the preset name and family");
        check (std::abs (b.loadedValue ("filter.cutoff") - a.loadedValue ("filter.cutoff")) < 1.0e-6f && std::abs (b.exactValue (svl::FLT_CUTOFF) - a.exactValue (svl::FLT_CUTOFF)) < 1.0e-6f
                   && std::abs (b.loadedValue ("filter.cutoff") - b.exactValue (svl::FLT_CUTOFF)) > 1.0f,
               "state keeps the loaded preset's values (double-click target " + juce::String (b.loadedValue ("filter.cutoff")) + " Hz, playing "
                   + juce::String (b.exactValue (svl::FLT_CUTOFF)) + " Hz)");
        check (b.isModified(), "restored state shows modified");
    }

    // ---- 4. host moves and exact preset values
    {
        JingFangProcessor p;
        p.loadPresetXml (root.getChildFile ("presets/Pluck/Saw Pluck.xml").loadFileAsString());
        p.setRateAndBufferSizeDetails (sr, 256); p.prepareToPlay (sr, 256);
        juce::AudioBuffer<float> buf (2, 256); juce::MidiBuffer m; p.processBlock (buf, m);
        svl::Preset pr; svl::loadPreset (root.getChildFile ("presets/Pluck/Saw Pluck.xml").getFullPathName().toStdString(), pr);
        int off = 0;
        for (int i = 0; i < svl::kNumParams; ++i) if (p.engineParamsForTest()[i] != svl::clampParam (i, pr.params[i])) ++off;
        check (off == 0, "engine plays the preset's exact values (" + juce::String (off) + " differ)");
        auto* res = p.apvts.getParameter ("filter.reso");
        res->setValueNotifyingHost (0.77f);
        p.processBlock (buf, m);
        const float want = res->convertFrom0to1 (0.77f);
        check (std::abs (p.engineParamsForTest()[svl::FLT_RESO] - want) < 1.0e-6f, "a host move reaches the engine (reso " + juce::String (p.engineParamsForTest()[svl::FLT_RESO], 4) + ")");
        check (p.engineParamsForTest()[svl::FLT_CUTOFF] == pr.params[svl::FLT_CUTOFF], "untouched parameters keep their exact value");
    }

    // ---- 5. keyswitch write-back
    {
        JingFangProcessor p;
        p.loadPresetXml (root.getChildFile ("presets/Init/Init.xml").loadFileAsString());
        auto set = [&] (const char* id, float v) { auto* q = p.apvts.getParameter (id); q->setValueNotifyingHost (q->convertTo0to1 (v)); };
        set ("tuning.mode", 1); set ("tuning.keyswitch", 1); set ("tuning.keyswitch_low", 24);
        p.setRateAndBufferSizeDetails (sr, 512); p.prepareToPlay (sr, 512);
        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer m;
        m.addEvent (juce::MidiMessage::noteOn (1, 24 + 4, (juce::uint8) 100), 10);    // E, first octave: to low
        m.addEvent (juce::MidiMessage::noteOn (1, 24 + 12 + 9, (juce::uint8) 100), 20);   // A, second octave: to high
        p.processBlock (buf, m);
        float peak = 0; for (int c = 0; c < 2; ++c) peak = std::max (peak, buf.getMagnitude (c, 0, 512));
        juce::MidiBuffer none;
        for (int i = 0; i < 20; ++i) { p.processBlock (buf, none); for (int c = 0; c < 2; ++c) peak = std::max (peak, buf.getMagnitude (c, 0, 512)); }
        check (peak == 0.0f, "keyswitch keys never sound (peak " + juce::String (peak) + ")");
        p.runMessageWorkForTest();
        const float e = p.apvts.getRawParameterValue ("tuning.row.e")->load(), a = p.apvts.getRawParameterValue ("tuning.row.a")->load(),
                    c = p.apvts.getRawParameterValue ("tuning.row.c")->load();
        check (juce::roundToInt (e) == 0 && juce::roundToInt (a) == 2 && juce::roundToInt (c) == 1,
               "keyswitch rows written back to the host parameters (E " + juce::String (e) + ", A " + juce::String (a) + ", C " + juce::String (c) + ")");
        p.processBlock (buf, none);
        check (p.engineParamsForTest()[svl::TUNE_ROW0 + 4] == 0.0f && p.engineParamsForTest()[svl::TUNE_ROW0 + 9] == 2.0f,
               "the write-back does not undo the engine's rows");
        juce::MidiBuffer again; again.addEvent (juce::MidiMessage::noteOn (1, 24 + 4, (juce::uint8) 100), 0);
        p.processBlock (buf, again); p.runMessageWorkForTest();
        check (juce::roundToInt (p.apvts.getRawParameterValue ("tuning.row.e")->load()) == 1, "the same keyswitch again returns E to mid (host parameter)");
    }

    // ---- 6. panel notes
    {
        auto renderPanel = [&] (int row, bool normalMidi)
        {
            JingFangProcessor p;
            p.loadPresetXml (root.getChildFile ("presets/Pluck/Saw Pluck.xml").loadFileAsString());
            p.setRateAndBufferSizeDetails (sr, 512); p.prepareToPlay (sr, 512);
            p.uiNoteOn (64, normalMidi ? -1 : row, 100.0f / 127.0f);
            std::vector<float> out;
            juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer m;
            for (int b = 0; b < 40; ++b) { p.processBlock (buf, m); out.insert (out.end(), buf.getReadPointer (0), buf.getReadPointer (0) + 512); }
            return out;
        };
        auto renderEngine = [&] (float pitch, bool key)
        {
            svl::Preset pr; svl::loadPreset (root.getChildFile ("presets/Pluck/Saw Pluck.xml").getFullPathName().toStdString(), pr);
            svl::Engine e; e.setParams (pr.params, true); e.prepare (sr);
            if (key) e.noteOn (64, 100.0f / 127.0f, 0); else e.noteOnPitch (pitch, 100.0f / 127.0f, 2000, 0);
            std::vector<float> out, L (512), R (512);
            for (int b = 0; b < 40; ++b) { e.process (L.data(), R.data(), 512); out.insert (out.end(), L.begin(), L.end()); }
            return out;
        };
        auto maxd = [] (const std::vector<float>& a, const std::vector<float>& b) { float d = 0; for (size_t i = 0; i < a.size(); ++i) d = std::max (d, std::abs (a[i] - b[i])); return d; };
        const float dKey = maxd (renderPanel (1, true), renderEngine (64, true));
        check (dKey == 0.0f, "panel key in normal mode = MIDI note (max diff " + juce::String (dKey) + ")");
        const float low = 64.0f - 1.0f * 12.0f / 53.0f;
        const float dLow = maxd (renderPanel (0, false), renderEngine (low, false));
        check (dLow == 0.0f, "panel low row = key - 1 step of 53 (" + juce::String (low, 5) + " st; max diff " + juce::String (dLow) + ")");
        const float dUp = maxd (renderPanel (2, false), renderEngine (64.0f + 12.0f / 53.0f, false));
        check (dUp == 0.0f, "panel high row = key + 1 step of 53 (max diff " + juce::String (dUp) + ")");
    }

    // ---- preset synthesizer: each panel from its own preset slot
    {
        JingFangProcessor a;
        const auto& list = a.presets();
        check (list.size() > 20, "preset list for the slots (" + juce::String ((int) list.size()) + ")");
        int unmapped = 0;
        for (const auto& id : JingFangProcessor::paramIds())
            if (JingFangProcessor::sectionOf (id.toStdString()) < 0 && ! id.startsWith ("lfo") && ! id.startsWith ("mod") && ! id.startsWith ("daw.")) { ++unmapped; std::printf ("  no panel: %s\n", id.toRawUTF8()); }
        check (unmapped == 0, "every panel parameter belongs to a slot (only LFOs, cables and the DAW's PATTERN / MIDI stay outside)");
        const int i5 = 4, i9 = 8, i17 = 16;
        a.loadPresetIndex (i5);
        bool allFive = true;
        for (int sct = 0; sct < JingFangProcessor::kNumSections; ++sct) allFive = allFive && a.sectionSlot (sct) == i5;
        check (allFive, "a whole preset sets every slot to it");
        svl::Preset p9, p17, p5;
        svl::presetFromXml (list[(size_t) i9].xml.toStdString(), p9); svl::presetFromXml (list[(size_t) i17].xml.toStdString(), p17);
        svl::presetFromXml (list[(size_t) i5].xml.toStdString(), p5);
        a.loadSection (1, i9);   // OSC 2 <- preset 9
        a.loadSection (4, i17);  // AMP ENV <- preset 17
        int bad = 0;
        for (int i = 0; i < svl::kNumParams; ++i)
        {
            const int sct = JingFangProcessor::sectionOf (svl::paramDefs()[(size_t) i].id);
            const float want = sct == 1 ? p9.params[i] : sct == 4 ? p17.params[i] : p5.params[i];
            if (std::abs (a.exactValue (i) - svl::clampParam (i, want)) > 1.0e-6f) ++bad;
        }
        check (bad == 0, "OSC 2 from preset 9, AMP ENV from preset 17, the rest from preset 5 (" + juce::String (bad) + " values off)");
        check (a.sectionSlot (1) == i9 && a.sectionSlot (4) == i17 && a.sectionSlot (0) == i5 && a.currentName() == "Mix", "slots report 9 / 17 / 5 and the name is Mix");
        check (std::abs (a.loadedValue ("osc2.level") - p9.params[svl::findParam ("osc2.level")]) < 1.0e-6f, "double-click target of an OSC 2 knob = preset 9's value");
        juce::MemoryBlock st; a.getStateInformation (st);
        JingFangProcessor b; b.setStateInformation (st.getData(), (int) st.getSize());
        check (b.sectionSlot (1) == i9 && b.sectionSlot (4) == i17 && b.sectionSlot (0) == i5, "the slots survive a state round trip");
        svl::Preset rp;
        check (svl::presetFromXml (st.toString().toStdString(), rp) && rp.warnings.empty(), "a state with slots is still a preset file svl reads");
        a.stepSection (1, 1);
        check (a.sectionSlot (1) == i9 + 1, "stepping a slot moves only that slot");
    }

    // ---- arp / sequencer: the Arp / Sequence presets with one held key = `svl render --tempo 120`
    {
        juce::Array<juce::File> files;   // the "Sequence - ..." and "Arp - ..." families (the patterns live in presets)
        for (const auto& d : root.getChildFile ("presets").findChildFiles (juce::File::findDirectories, false))
            if (d.getFileName().startsWith ("Sequence") || d.getFileName().startsWith ("Arp"))
                files.addArray (d.findChildFiles (juce::File::findFiles, false, "*.xml"));
        check (! files.isEmpty(), "the Sequence and Arp presets exist (" + juce::String (files.size()) + ")");
        const auto held = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("jf_held_key.txt");
        held.replaceWithText ("# one key held 4 s\n60 100 0.000 4.0\n");
        int same = 0;
        for (const auto& f : files)
        {
            const auto wav = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("jf_seq_" + f.getFileNameWithoutExtension().replaceCharacter (' ', '_') + ".wav");
            juce::ChildProcess cp;
            const bool ran = cp.start (juce::StringArray { svlBin.getFullPathName(), "render", "--preset", f.getFullPathName(), "--notes", held.getFullPathName(),
                                                           "--out", wav.getFullPathName(), "--sr", "44100", "--block", "512", "--tail", "4", "--tempo", "120" })
                             && cp.waitForProcessToFinish (60000) && cp.getExitCode() == 0;
            Stereo ref;
            if (! ran || ! readWav24 (wav, ref)) continue;
            JingFangProcessor proc;
            proc.loadPresetXml (f.loadFileAsString());
            std::vector<float> R;
            const auto L = renderProcessor (proc, loadNotes (held), sr, 512, juce::jmax (4.0, (double) ref.l.size() / sr + 0.1), &R);
            long maxDiff = 0;
            for (size_t i = 0; i < std::min (ref.l.size(), L.size()); ++i)
                maxDiff = std::max (maxDiff, std::max (std::labs ((long) q24 (L[i]) - ref.l[i]), std::labs ((long) q24 (R[i]) - ref.r[i])));
            if (maxDiff == 0 && L.size() >= ref.l.size()) ++same;
            else std::printf ("  %s: max diff %ld LSB\n", f.getFileName().toRawUTF8(), maxDiff);
        }
        check (same == files.size(), "held key through the plug-in = svl render --tempo 120: " + juce::String (same) + " / " + juce::String (files.size()) + " Sequence / Arp presets identical");

    }

    // ---- owner 18 Oct morning: the drum machine waits for a key (HOLD); one key on an arp preset plays a chord, not one note
    {
        auto run = [&] (const juce::File& f, bool holdKey, std::set<int>* pitches, std::set<int>* shown = nullptr)
        {
            JingFangProcessor p;
            p.loadPresetXml (f.loadFileAsString());
            p.setRateAndBufferSizeDetails (sr, 512); p.prepareToPlay (sr, 512);
            juce::AudioBuffer<float> buf (2, 512);
            float peak = 0;
            for (int b = 0; b < 4 * sr / 512; ++b)
            {
                juce::MidiBuffer m;
                if (b == 0 && holdKey) m.addEvent (juce::MidiMessage::noteOn (1, 48, (juce::uint8) 100), 0);
                p.processBlock (buf, m);
                peak = std::max ({ peak, buf.getMagnitude (0, 0, 512), buf.getMagnitude (1, 0, 512) });
                if (shown)
                    for (int k = 0; k < 128; ++k) if (p.keySounding (1, k)) shown->insert (k);
                if (pitches)
                    for (int v = 0; v < svl::kVoicePool; ++v)
                    {
                        const auto vi = p.engineForTest().voiceInfo (v);
                        if (vi.active && vi.gate) pitches->insert (juce::roundToInt (vi.pitch * 1000.0f));
                    }
            }
            return peak;
        };
        const auto drums = root.getChildFile ("presets/Drum Machine").findChildFiles (juce::File::findFiles, false, "*.xml");
        int silent = 0, playing = 0, synthFree = 0;
        for (auto& f : drums)
        {
            std::set<int> synth;
            run (f, true, &synth);
            if (synth.empty()) ++synthFree;   // HOLD: the key is the drum machine's transport, no synth voice
            const float idle = run (f, false, nullptr);
            if (idle < 1.0e-5f) ++silent;   // below -100 dBFS: nothing plays (a reverb / DC blocker settling leaves crumbs)
            else std::printf ("  %s plays without a key: peak %.1f dBFS\n", f.getFileName().toRawUTF8(), 20.0 * std::log10 (idle));
            if (run (f, true, nullptr) > 0.01f) ++playing;
        }
        check (! drums.isEmpty() && silent == drums.size() && playing == drums.size(),
               "Drum Machine presets wait for a key: silent without one " + juce::String (silent) + " / " + juce::String (drums.size())
               + ", playing with one held " + juce::String (playing) + " / " + juce::String (drums.size()));
        {   // PLAY on the DRUM door = drums.on + free: the beat runs with no key (standalone: its own clock)
            JingFangProcessor p;
            p.loadPresetXml (drums.getFirst().loadFileAsString());
            if (auto* m = p.apvts.getParameter ("drums.mode")) m->setValueNotifyingHost (m->convertTo0to1 (1.0f));
            p.setRateAndBufferSizeDetails (sr, 512); p.prepareToPlay (sr, 512);
            juce::AudioBuffer<float> buf (2, 512);
            float peak = 0;
            for (int b = 0; b < 2 * sr / 512; ++b) { juce::MidiBuffer m; p.processBlock (buf, m); peak = std::max (peak, buf.getMagnitude (0, 512)); }
            check (peak > 0.01f, "PLAY (free) runs the beat with no key held: peak " + juce::String (20.0f * std::log10 (juce::jmax (1.0e-9f, peak)), 1) + " dBFS");
        }
        check (synthFree == drums.size(), "the held key plays only the drums, no synth voice: " + juce::String (synthFree) + " / " + juce::String (drums.size()));
        juce::Array<juce::File> arps;
        for (const auto& d : root.getChildFile ("presets").findChildFiles (juce::File::findDirectories, false))
            if (d.getFileName().startsWith ("Arp")) arps.addArray (d.findChildFiles (juce::File::findFiles, false, "*.xml"));
        int chords = 0, fewest = 99, shownAll = 0;
        for (auto& f : arps)
        {
            std::set<int> ps, shown;
            run (f, true, &ps, &shown);
            fewest = std::min (fewest, (int) ps.size());
            if (ps.size() >= 2) ++chords;
            if (shown.size() >= ps.size()) ++shownAll;   // every pitch the arp played lit a key on the panel keyboard
        }
        check (shownAll == arps.size(), "the panel keyboard lights the keys the arp plays: " + juce::String (shownAll) + " / " + juce::String (arps.size()) + " Arp presets");
        check (! arps.isEmpty() && chords == arps.size(), "one held key on every Arp preset plays several notes: " + juce::String (chords) + " / "
               + juce::String (arps.size()) + " (fewest distinct pitches " + juce::String (fewest) + ")");
    }

    // ---- every gated voice lights its key, in all of the engine's voice slots (the demo player, 18 Oct: a note in a spare slot
    //      past the polyphony ceiling, where a stolen voice fades, did not light; the keyboard looked out of step with the sound)
    {
        JingFangProcessor p;
        p.prepareToPlay (48000, 512);
        juce::AudioBuffer<float> buf (2, 512);
        int spareGated = 0, unlit = 0;
        juce::String missed;
        for (int b = 0; b < 60; ++b)
        {
            juce::MidiBuffer m;
            if (b < 24) m.addEvent (juce::MidiMessage::noteOn (1, 40 + b, (juce::uint8) 100), 0);   // more keys than voices: steals
            p.processBlock (buf, m);
            for (int v = 0; v < svl::kVoicePool; ++v)
            {
                const auto vi = p.engineForTest().voiceInfo (v);
                if (! vi.active || ! vi.gate) continue;
                if (v >= svl::kMaxVoices) ++spareGated;
                const int k = juce::roundToInt (vi.pitch);
                if (! (p.keySounding (0, k) || p.keySounding (1, k) || p.keySounding (2, k))) { ++unlit; if (missed.length() < 60) missed << k << " "; }
            }
        }
        check (spareGated > 0 && unlit == 0, "every gated voice lights its key, spare slots too (" + juce::String (spareGated) + " gated voice-blocks in slots "
               + juce::String (svl::kMaxVoices) + "+, " + juce::String (unlit) + " unlit " + missed + ")");
    }

    // ---- BPM and the DAW (owner 19 Oct; SPEC "BPM and the DAW", RU-8's DAW rules)
    {
        struct Head : juce::AudioPlayHead
        {
            bool playing = false; double ppq = 0, bpm = 120;
            juce::Optional<PositionInfo> getPosition() const override
            {
                PositionInfo p; p.setIsPlaying (playing); p.setPpqPosition (ppq); p.setBpm (bpm); return p;
            }
        };
        auto setP = [] (JingFangProcessor& p, const char* id, float plain)
        { if (auto* hp = p.apvts.getParameter (id)) hp->setValueNotifyingHost (hp->convertTo0to1 (plain)); };
        auto presetIndex = [] (JingFangProcessor& p, const char* fam, const char* name)
        { for (size_t i = 0; i < p.presets().size(); ++i) if (p.presets()[i].family == fam && p.presets()[i].name == name) return (int) i; return 0; };
        // the notes a free sequence makes in 2 s (standalone clock), as MIDI keys
        auto seqKeys = [&] (std::function<void (JingFangProcessor&)> setup)
        {
            JingFangProcessor p;
            p.prepareToPlay (48000, 512);
            setP (p, "seq.on", 1); setP (p, "seq.mode", 1); setP (p, "seq.length", 8);
            const float notes[] = { 0, 1, 2, 3, 4, 5, 6, 7 };
            for (int s = 0; s < 8; ++s) setP (p, ("seq.s" + juce::String (s + 1) + ".note").toRawUTF8(), notes[s]);
            setup (p);
            std::set<int> keys;
            juce::AudioBuffer<float> buf (2, 512);
            for (int b = 0; b < 190; ++b)
            {
                juce::MidiBuffer m; p.processBlock (buf, m);
                for (int v = 0; v < svl::kVoicePool; ++v) { const auto vi = p.engineForTest().voiceInfo (v); if (vi.active && vi.gate) keys.insert (juce::roundToInt (vi.pitch)); }
            }
            return keys;
        };
        const auto chrom = seqKeys ([] (JingFangProcessor&) {});
        check (chrom == std::set<int> { 60, 61, 62, 63, 64, 65, 66, 67 }, "a free sequence plays its steps as written from C (60..67)");
        // daw.seq = midi: a held key plays itself, the sequencer is out
        {
            JingFangProcessor p; p.prepareToPlay (48000, 512);
            setP (p, "seq.on", 1); setP (p, "daw.seq", 1);
            juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOn (1, 57, (juce::uint8) 100), 0);
            p.processBlock (buf, m);
            std::set<int> keys;
            for (int b = 0; b < 100; ++b) { juce::MidiBuffer none; p.processBlock (buf, none); for (int v = 0; v < svl::kVoicePool; ++v) { const auto vi = p.engineForTest().voiceInfo (v); if (vi.active && vi.gate) keys.insert (juce::roundToInt (vi.pitch)); } }
            check (keys == std::set<int> { 57 } && ! p.engineForTest().noteGen().isRunning(), "PATTERN / MIDI = MIDI on SEQ: a DAW note plays itself, the sequencer stays out");
        }
        // daw.drums = midi: a free beat is silent; notes 36..43 hit the tracks
        {
            JingFangProcessor p; p.prepareToPlay (48000, 512);
            p.loadPresetIndex (presetIndex (p, "Drum Machine", "Boom Bap"));
            juce::AudioBuffer<float> buf (2, 512);
            { juce::MidiBuffer none; p.processBlock (buf, none); }
            setP (p, "drums.mode", 1); setP (p, "daw.drums", 1);
            float peak = 0;
            for (int b = 0; b < 100; ++b) { juce::MidiBuffer none; p.processBlock (buf, none); peak = std::max (peak, buf.getMagnitude (0, 0, 512)); }
            juce::MidiBuffer hit; hit.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 110), 0);
            p.processBlock (buf, hit);
            float hitPeak = 0;
            for (int b = 0; b < 20; ++b) { juce::MidiBuffer none; p.processBlock (buf, none); hitPeak = std::max (hitPeak, buf.getMagnitude (0, 0, 512)); }
            check (peak < 1.0e-4f && ! p.engineForTest().drumMachine()->isRunning() && hitPeak > 0.01f,
                   "PATTERN / MIDI = MIDI on DRUM: the free beat is silent (" + juce::String (juce::Decibels::gainToDecibels (peak + 1e-9f), 1) + " dBFS), note 36 hits the kick ("
                   + juce::String (juce::Decibels::gainToDecibels (hitPeak), 1) + " dBFS)");
            // a preset load never changes the DAW mode (it belongs to the project)
            p.loadPresetIndex (presetIndex (p, "Drum Machine", "Four Floor House"));
            { juce::MidiBuffer none; p.processBlock (buf, none); }
            check (p.exactValue (svl::DRUMS_SOURCE) > 0.5f && ! p.isModified(), "a preset load keeps PATTERN / MIDI and is not 'modified' by it");
        }
        // a dragged clip arms; the first note while the host plays switches that unit to MIDI (RU-8 rule 3)
        {
            JingFangProcessor p; Head head; p.setPlayHead (&head); p.prepareToPlay (48000, 512);
            setP (p, "seq.on", 1);
            juce::AudioBuffer<float> buf (2, 512);
            p.armClip (0);
            { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0); p.processBlock (buf, m); }
            const bool stoppedNoSwitch = p.exactValue (svl::SEQ_SOURCE) < 0.5f && p.clipArmed (0);
            { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOff (1, 60), 0); p.processBlock (buf, m); }
            head.playing = true;
            { juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOn (1, 62, (juce::uint8) 100), 10); p.processBlock (buf, m); }
            p.runMessageWorkForTest();
            const float host = p.apvts.getRawParameterValue ("daw.seq")->load();
            check (stoppedNoSwitch && p.exactValue (svl::SEQ_SOURCE) > 0.5f && ! p.clipArmed (0) && host > 0.5f,
                   "an armed SEQ switches to MIDI on the first note while the host plays (not while it is stopped); the host sees it");
        }
        // PLAY in a stopped host previews; the host's start carries on without a cut; its stop ends it (RU-8 rule 2)
        {
            JingFangProcessor p; Head head; p.setPlayHead (&head); p.prepareToPlay (48000, 512);
            setP (p, "seq.on", 1); setP (p, "seq.mode", 1);
            juce::AudioBuffer<float> buf (2, 512);
            auto run = [&] (int blocks) { for (int b = 0; b < blocks; ++b) { juce::MidiBuffer none; head.ppq += head.playing ? 512.0 / 48000.0 * 2.0 : 0.0; p.processBlock (buf, none); } };
            run (10);
            const bool silentBefore = ! p.engineForTest().noteGen().isRunning();
            p.requestPreview (0, true);
            run (20);
            const bool previewing = p.engineForTest().noteGen().isRunning();
            head.playing = true;
            run (20);
            const bool carried = p.engineForTest().noteGen().isRunning() && ! p.engineForTest().previewing (false);
            head.playing = false;
            run (5);
            const bool stopped = ! p.engineForTest().noteGen().isRunning();
            run (40);
            check (silentBefore && previewing && carried && stopped && ! p.engineForTest().noteGen().isRunning(),
                   "in a host: a free sequence waits; PLAY previews while stopped; the host's start carries on, its stop ends it (and it stays stopped)");
        }
    }

    // ---- PLAY sounds on every preset (owner 19 Oct: "sequencer play tuşu çalışmıyor": an arp + seq preset ran its steps in
    //      silence). For every preset: SEQ PLAY (seq.on + free, no key) and, where the kit has steps, DRUM PLAY must make sound.
    {
        JingFangProcessor scan;
        juce::StringArray silentSeq, silentDrum;
        int seqTried = 0, drumTried = 0;
        auto setP = [] (JingFangProcessor& p, const char* id, float plain)
        { if (auto* hp = p.apvts.getParameter (id)) hp->setValueNotifyingHost (hp->convertTo0to1 (plain)); };
        for (size_t i = 0; i < scan.presets().size(); ++i)
        {
            const auto& e = scan.presets()[i];
            for (int unit = 0; unit < 2; ++unit)
            {
                JingFangProcessor p;
                p.prepareToPlay (48000, 512);
                p.loadPresetIndex ((int) i);
                juce::AudioBuffer<float> buf (2, 512);
                { juce::MidiBuffer m; p.processBlock (buf, m); }
                bool any = false;
                if (unit == 0)
                {
                    for (int st = 1; st <= 16; ++st) any |= p.apvts.getRawParameterValue ("seq.s" + juce::String (st) + ".on")->load() > 0.5f;
                    if (! any) continue;                                  // nothing to play (an empty grid)
                    setP (p, "seq.on", 1); setP (p, "seq.mode", 1);
                }
                else
                {
                    for (int t = 1; t <= 8 && ! any; ++t) for (int st = 1; st <= 16; ++st) any |= p.apvts.getRawParameterValue ("drums.t" + juce::String (t) + ".s" + juce::String (st))->load() > 0.5f;
                    if (! any) continue;
                    setP (p, "drums.on", 1); setP (p, "drums.mode", 1);
                }
                (unit == 0 ? seqTried : drumTried)++;
                float peak = 0;
                for (int b = 0; b < 190; ++b) { juce::MidiBuffer m; p.processBlock (buf, m); peak = std::max ({ peak, buf.getMagnitude (0, 0, 512), buf.getMagnitude (1, 0, 512) }); }
                if (peak < 0.001f) (unit == 0 ? silentSeq : silentDrum).add (e.family + " / " + e.name);
            }
        }
        check (silentSeq.isEmpty() && seqTried > 50, "SEQ PLAY sounds on every preset with steps (" + juce::String (seqTried) + " tried" + (silentSeq.isEmpty() ? juce::String() : ", silent: " + silentSeq.joinIntoString (", ")) + ")");
        check (silentDrum.isEmpty() && drumTried > 10, "DRUM PLAY sounds on every preset with a beat (" + juce::String (drumTried) + " tried" + (silentDrum.isEmpty() ? juce::String() : ", silent: " + silentDrum.joinIntoString (", ")) + ")");
    }

    // ---- tempo: the panel's TEMPO drives the clock with no host (owner 18 Oct), and survives a state round trip
    {
        auto stepsIn2s = [&] (float bpm)
        {
            JingFangProcessor p;
            p.loadPresetXml (root.getChildFile ("presets/Drum Machine").findChildFiles (juce::File::findFiles, false, "*.xml").getFirst().loadFileAsString());
            if (auto* m = p.apvts.getParameter ("drums.mode")) m->setValueNotifyingHost (m->convertTo0to1 (1.0f));   // free: runs alone
            p.setUserBpm (bpm);
            p.setRateAndBufferSizeDetails (sr, 512); p.prepareToPlay (sr, 512);
            juce::AudioBuffer<float> buf (2, 512);
            int changes = 0, last = -2;
            for (int b = 0; b < 2 * sr / 512; ++b)
            {
                juce::MidiBuffer m; p.processBlock (buf, m);
                const int st = p.drumStep();
                if (st != last && st >= 0) ++changes;
                last = st;
            }
            return changes;
        };
        const int at120 = stepsIn2s (120.0f), at60 = stepsIn2s (60.0f);
        check (at120 >= 15 && at60 >= 7 && at60 <= at120 / 2 + 1, "the panel's TEMPO sets the clock: " + juce::String (at120) + " steps in 2 s at 120 BPM, "
               + juce::String (at60) + " at 60 BPM (1/16 steps)");
        JingFangProcessor a; a.setUserBpm (97.0f);
        juce::MemoryBlock st; a.getStateInformation (st);
        JingFangProcessor b; b.setStateInformation (st.getData(), (int) st.getSize());
        check (std::abs (b.userTempo() - 97.0f) < 0.01f, "the tempo survives a state round trip (" + juce::String (b.userTempo(), 1) + " BPM)");
    }

    // ---- deleting presets (owner 18 Oct): a factory preset is hidden for good, the user's own goes away; drum kits still find
    // a hidden drum sound. Runs in a temporary user folder.
    {
        const auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("jf_presets_check");
        tmp.deleteRecursively(); tmp.createDirectory();
        JingFangProcessor::userFolderOverrideForTests() = tmp;
        JingFangProcessor p;
        auto find = [&p] (const juce::String& fam, const juce::String& name)
        { for (size_t i = 0; i < p.presets().size(); ++i) if (p.presets()[i].family == fam && p.presets()[i].name == name) return (int) i; return -1; };
        const int before = (int) p.presets().size();
        const int kick = find ("Drums - Kick", "Thump Kick");
        const bool hid = kick >= 0 && p.deletePreset (kick);
        JingFangProcessor q;   // a new instance (a new session) must not see it
        int inQ = -1; for (size_t i = 0; i < q.presets().size(); ++i) if (q.presets()[i].name == "Thump Kick" && q.presets()[i].family == "Drums - Kick") inQ = (int) i;
        check (hid && find ("Drums - Kick", "Thump Kick") < 0 && inQ < 0 && (int) p.presets().size() == before - 1,
               "deleting a factory preset hides it, also in a new session (" + juce::String (before) + " -> " + juce::String ((int) p.presets().size()) + ")");
        q.setDrumTrack (1, find ("Drums - Snare", "Crack Snare"));
        check (q.drumTrackName (0) == "Thump Kick", "the default kit still plays the hidden kick (track 1: " + q.drumTrackName (0) + ")");
        p.saveUserPreset ("Drum Machine", "Check Beat");
        const int mine = find ("Drum Machine", "Check Beat");
        const bool gone = mine >= 0 && p.deletePreset (mine) && find ("Drum Machine", "Check Beat") < 0;
        check (gone, "deleting your own preset removes it from the list");
        p.showHiddenPresets();
        check (find ("Drums - Kick", "Thump Kick") >= 0, "\"Show hidden factory presets\" brings the factory preset back");
        JingFangProcessor::userFolderOverrideForTests() = juce::File();
        tmp.deleteRecursively();
    }

    // ---- Tuning Lab (SPEC "Tuning Lab"): locked = untouched; unlocked = directional pitches, state round trip
    {
        jflab::Tuning dir;                       // 12 equal, but E falls to 360 ct
        dir.name = "Falling E";
        dir.fall = dir.cents;
        dir.fall[3] = 360;
        auto play = [&] (JingFangProcessor& p, std::vector<float>* out)
        {   // G4, then E4 from above: E is reached falling
            p.setRateAndBufferSizeDetails (sr, 512); p.prepareToPlay (sr, 512);
            juce::AudioBuffer<float> buf (2, 512);
            for (int b = 0; b < 40; ++b)
            {
                juce::MidiBuffer m;
                if (b == 0) m.addEvent (juce::MidiMessage::noteOn (1, 67, (juce::uint8) 100), 0);
                if (b == 4) m.addEvent (juce::MidiMessage::noteOff (1, 67), 0);
                if (b == 5) m.addEvent (juce::MidiMessage::noteOn (1, 64, (juce::uint8) 100), 0);
                p.processBlock (buf, m);
                if (out) out->insert (out->end(), buf.getReadPointer (0), buf.getReadPointer (0) + 512);
            }
        };
        auto withLab = [&] (bool unlocked, std::vector<float>* out, JingFangProcessor* keep = nullptr)
        {
            jflab::Lab::setUnlockedForTests (unlocked);
            JingFangProcessor local;
            JingFangProcessor& p = keep ? *keep : local;
            p.loadPresetXml (root.getChildFile ("presets/Pluck/Saw Pluck.xml").loadFileAsString());
            JingFangProcessor::LabState ls; ls.on = true; ls.a = dir; ls.b = dir;
            p.setLabState (ls);
            play (p, out);
            return p.labSounding();
        };
        std::vector<float> plain, locked;
        { JingFangProcessor p; p.loadPresetXml (root.getChildFile ("presets/Pluck/Saw Pluck.xml").loadFileAsString()); play (p, &plain); }
        withLab (false, &locked);
        float d = 0; for (size_t i = 0; i < plain.size(); ++i) d = std::max (d, std::abs (plain[i] - locked[i]));
        check (d == 0.0f && ! JingFangProcessor::labUnlocked(), "Lab switched on but locked: output identical to the plain plug-in (max diff " + juce::String (d) + ")");
        std::vector<float> unl;
        const auto snd = withLab (true, &unl);
        const bool fell = snd.size() == 1 && snd[0].first == 64 && std::abs (snd[0].second - 63.6f) < 1.0e-4f;
        check (fell, "Lab unlocked: E reached from G plays its falling value 63.60 (" + (snd.empty() ? juce::String ("-") : juce::String (snd[0].second, 4)) + ")");
        float du = 0; for (size_t i = 0; i < plain.size(); ++i) du = std::max (du, std::abs (plain[i] - unl[i]));
        check (du > 0.001f, "Lab unlocked: the falling E sounds different from the plain E (max diff " + juce::String (du, 4) + ")");
        // the Lab with the arpeggiator and the sequencer, in a 24-step tuning (keys 60 and 62 = 60.0 and 61.0)
        auto pitchesWith = [&] (const std::function<void (JingFangProcessor&)>& setUp)
        {
            jflab::Lab::setUnlockedForTests (true);
            JingFangProcessor p;
            p.loadPresetXml (root.getChildFile ("presets/Pluck/Saw Pluck.xml").loadFileAsString());
            jflab::Tuning e24; e24.name = "24 equal"; e24.cents.clear();
            for (int i = 1; i <= 24; ++i) e24.cents.push_back (50.0 * i);
            JingFangProcessor::LabState ls; ls.on = true; ls.a = e24; ls.b = e24;
            p.setLabState (ls);
            setUp (p);
            p.setRateAndBufferSizeDetails (sr, 512); p.prepareToPlay (sr, 512);
            std::set<int> got;   // millisemitones of every voice that sounds
            juce::AudioBuffer<float> buf (2, 512);
            for (int b = 0; b < 200; ++b)
            {
                juce::MidiBuffer m;
                if (b == 0) m.addEvent (juce::MidiMessage::noteOn (1, 62, (juce::uint8) 100), 0);
                p.processBlock (buf, m);
                for (int v = 0; v < svl::kVoicePool; ++v)
                {
                    const auto vi = p.engineForTest().voiceInfo (v);
                    if (vi.active && vi.gate) got.insert (juce::roundToInt (vi.pitch * 1000.0f));
                }
            }
            jflab::Lab::setUnlockedForTests (false);
            juce::String txt;
            for (int g : got) txt << juce::String (g / 1000.0, 3) << " ";
            return std::make_pair (got, txt.trim());
        };
        auto setP = [] (JingFangProcessor& p, const char* id, float plain)
        { if (auto* hp = p.apvts.getParameter (id)) hp->setValueNotifyingHost (hp->convertTo0to1 (plain)); };
        const auto arp = pitchesWith ([&] (JingFangProcessor& p) { setP (p, "arp.on", 1); setP (p, "arp.octaves", 2); });
        check (arp.first == std::set<int> { 61000, 73000 }, "Lab + arp in 24 steps: key 62 plays 61.000 and its octave 73.000 (" + arp.second + ")");
        const auto seqp = pitchesWith ([&] (JingFangProcessor& p) { setP (p, "seq.on", 1); setP (p, "seq.length", 2); setP (p, "seq.s2.note", 1); });
        check (seqp.first == std::set<int> { 61000, 61500 }, "Lab + seq in 24 steps: step 2's +1 is the next step of the tuning, 61.500 (" + seqp.second + ")");

        JingFangProcessor a;
        withLab (true, nullptr, &a);
        juce::MemoryBlock st; a.getStateInformation (st);
        JingFangProcessor b; b.setStateInformation (st.getData(), (int) st.getSize());
        const auto& bs = b.labState();
        check (bs.on && bs.a.directional() && std::abs (bs.a.fall[3] - 360.0) < 1.0e-3 && bs.a.name == "Falling E", "Lab state round trip keeps on, the tuning and its falling values");
        svl::Preset rp;
        check (svl::presetFromXml (st.toString().toStdString(), rp) && rp.warnings.empty(), "a state with the Lab is still a preset file svl reads");
        jflab::Lab::setUnlockedForTests (false);
    }

    // ---- built-in tunings + the Sequence - Lab presets (owner 19 Oct): a preset's <labtuning> puts the tuning on card A, the
    // Lab on; its phrase plays the tuning's pitches; an ordinary preset turns the Lab off again unless the user took the Lab
    {
        jflab::Lab::setUnlockedForTests (true);
        auto setP = [] (JingFangProcessor& p, const char* id, float plain)
        { if (auto* hp = p.apvts.getParameter (id)) hp->setValueNotifyingHost (hp->convertTo0to1 (plain)); };
        auto indexOf = [] (JingFangProcessor& p, const juce::String& fam, const juce::String& name)
        { for (size_t i = 0; i < p.presets().size(); ++i) if (p.presets()[i].family == fam && p.presets()[i].name == name) return (int) i; return -1; };
        const auto labDir = root.getChildFile ("presets/Sequence - Lab");
        const auto files = labDir.findChildFiles (juce::File::findFiles, false, "*.xml");
        check (files.size() == (int) jflab::builtin::entries().size(), "Sequence - Lab: one preset per built-in tuning (" + juce::String (files.size()) + ")");
        for (const auto& f : files)
        {
            const auto xml = f.loadFileAsString();
            svl::Preset sp;
            const bool reads = svl::presetFromXml (xml.toStdString(), sp) && sp.warnings.empty();
            const auto lt = svl::detail::elements (xml.toStdString(), "labtuning");
            jflab::Tuning want;
            const bool known = ! lt.empty() && jflab::builtin::find (svl::detail::attr (lt.front(), "a"), want);
            JingFangProcessor p;
            p.prepareToPlay (sr, 512);
            const int idx = indexOf (p, "Sequence - Lab", f.getFileNameWithoutExtension());
            const bool loaded = idx >= 0 && p.loadPresetIndex (idx);
            const auto& ls = p.labState();
            check (reads && known && loaded && ls.on && ls.a.name == want.name && p.labFromPreset(),
                   f.getFileNameWithoutExtension() + ": svl reads it, it loads " + juce::String (want.name) + " on card A, Lab on");
            // the phrase: every note the free sequence plays is the built-in table's pitch of key 60 + step note
            setP (p, "seq.mode", 1);
            const auto tab = want.table();
            std::set<int> wantP, gotP;
            for (int s = 0; s < 16; ++s)
                {
                    const auto* on = p.apvts.getRawParameterValue (("seq.s" + juce::String (s + 1) + ".on").toRawUTF8());
                    const auto* nt = p.apvts.getRawParameterValue (("seq.s" + juce::String (s + 1) + ".note").toRawUTF8());
                    if (on != nullptr && nt != nullptr && on->load() > 0.5f) wantP.insert (juce::roundToInt (tab[(size_t) (60 + juce::roundToInt (nt->load()))] * 1000.0f));
                }
            juce::AudioBuffer<float> buf (2, 512);
            // JF_LAB_WAV=<dir>: also write 8.5 s of the phrase as a listening file (2 cycles at 120 BPM + the tail)
            const juce::File wavDir (juce::SystemStats::getEnvironmentVariable ("JF_LAB_WAV", {}));
            juce::AudioBuffer<float> rec (2, (int) (8.5 * sr) + 512);
            int recAt = 0;
            for (int b = 0; b < (int) ((wavDir != juce::File() ? 8.5 : 5.0) * sr / 512); ++b)
            {
                juce::MidiBuffer m;
                p.processBlock (buf, m);
                if (wavDir != juce::File())
                {
                    if (b == (int) (8.0 * sr / 512)) setP (p, "seq.mode", 0);     // stop: the last note rings out
                    for (int c = 0; c < 2; ++c) rec.copyFrom (c, recAt, buf, c, 0, 512);
                    recAt += 512;
                }
                for (int v = 0; v < svl::kVoicePool; ++v)
                {
                    const auto vi = p.engineForTest().voiceInfo (v);
                    if (vi.active && vi.gate) gotP.insert (juce::roundToInt (vi.pitch * 1000.0f));
                }
            }
            if (wavDir != juce::File())
            {
                wavDir.createDirectory();
                const auto out = wavDir.getChildFile (f.getFileNameWithoutExtension() + ".wav");
                out.deleteFile();
                juce::WavAudioFormat wf;
                std::unique_ptr<juce::AudioFormatWriter> w (wf.createWriterFor (new juce::FileOutputStream (out), sr, 2, 24, {}, 0));
                if (w != nullptr) w->writeFromAudioSampleBuffer (rec, 0, recAt);
            }
            juce::String txt;
            for (int g : gotP) txt << juce::String (g / 1000.0, 3) << " ";
            check (! wantP.empty() && gotP == wantP, f.getFileNameWithoutExtension() + ": the phrase plays the tuning's pitches (" + txt.trim() + ")");
            // an ordinary preset afterwards: the Lab goes off
            p.loadPresetIndex (indexOf (p, "Winds", "Oboe"));
            check (! p.labState().on && ! p.labFromPreset(), f.getFileNameWithoutExtension() + ": the next ordinary preset turns the Lab off");
        }
        {   // the user took the Lab (a knob, LOAD, 12 EQUAL): the next preset leaves it as it is
            JingFangProcessor p;
            p.loadPresetIndex (indexOf (p, "Sequence - Lab", "Wide Second on A"));
            p.labTakenByUser();
            p.loadPresetIndex (indexOf (p, "Winds", "Oboe"));
            check (p.labState().on && p.labState().a.name == "Hicaz (53)", "a Lab the user touched stays on through the next preset");
        }
        {   // the project keeps where the Lab came from: after a restore an ordinary preset still turns it off
            JingFangProcessor a;
            a.loadPresetIndex (indexOf (a, "Sequence - Lab", "Neutral Third on G"));
            juce::MemoryBlock st; a.getStateInformation (st);
            JingFangProcessor b; b.setStateInformation (st.getData(), (int) st.getSize());
            const bool restored = b.labState().on && b.labState().a.name == "Rast (53)" && b.labFromPreset() && b.labState().a.directional();
            b.loadPresetIndex (indexOf (b, "Winds", "Oboe"));
            check (restored && ! b.labState().on, "state round trip: the preset's Lab comes back (with its falling F#) and still yields to the next preset");
        }
        jflab::Lab::setUnlockedForTests (false);
    }

    std::printf ("\nplugin_check: %d passed, %d failed\n", gPass, gFail);
    return gFail == 0 ? 0 : 1;
}
