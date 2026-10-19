#include "MidiClip.h"
#include "PluginProcessor.h"
#include <map>

namespace jfclip
{
static constexpr int kPpq = 96;

static svl::ParamSet values (const JingFangProcessor& p)
{
    svl::ParamSet ps;
    for (int i = 0; i < svl::kNumParams; ++i) ps[i] = p.exactValue (i);
    return ps;
}

static double cycleBeats (const svl::ParamSet& ps, int unit)
{
    if (unit == 1) return juce::jlimit (1, svl::kDrumSteps, (int) std::lround (ps[svl::DRUMS_LEN])) * (double) svl::kStepRateBeats[(int) ps[svl::DRUMS_RATE]];
    if (ps[svl::SEQ_ON] > 0.5f) return juce::jlimit (1, svl::kSeqSteps, (int) std::lround (ps[svl::SEQ_LEN])) * (double) svl::kStepRateBeats[(int) ps[svl::SEQ_RATE]];
    return -1;   // an arp alone: its length depends on the chord
}

int bars (const JingFangProcessor& p, int unit)
{
    const double c = cycleBeats (values (p), unit);
    if (c <= 0) return 2;
    for (int b = 1; b <= 8; ++b)
    {
        const double n = 4.0 * b / c;
        if (std::abs (n - std::round (n)) < 1.0e-6) return b;
    }
    return 4;
}

juce::String clipName (const JingFangProcessor& p, int unit)
{
    return "JING FANG " + p.currentName() + (unit == 0 ? " SEQ" : " DRUM");
}

juce::MidiFile make (const JingFangProcessor& p, int unit)
{
    const auto ps = values (p);
    const int nBars = bars (p, unit);
    const double totalBeats = 4.0 * nBars;
    juce::MidiMessageSequence seq;
    seq.addEvent (juce::MidiMessage::textMetaEvent (3, clipName (p, unit)), 0);
    seq.addEvent (juce::MidiMessage::tempoMetaEvent (juce::roundToInt (60.0e6 / juce::jlimit (20.0, 300.0, (double) p.tempoBpm()))), 0);
    seq.addEvent (juce::MidiMessage::timeSignatureMetaEvent (4, 4), 0);
    const int ch = 1;
    if (unit == 1)
    {   // the drum machine's steps, straight from its values (the same grid and swing as SvlDrums.h)
        const int len = juce::jlimit (1, svl::kDrumSteps, (int) std::lround (ps[svl::DRUMS_LEN]));
        const double stepLen = svl::kStepRateBeats[(int) ps[svl::DRUMS_RATE]], swing = ps[svl::DRUMS_SWING];
        for (int64_t k = 0;; ++k)
        {
            const double on = (double) k * stepLen + ((k & 1) ? 0.5 * swing * stepLen : 0.0);
            if (on >= totalBeats - 1.0e-9) break;
            const int s = (int) (k % len);
            for (int t = 0; t < svl::kDrumTracks; ++t)
            {
                const int st = (int) ps[svl::drumStepParam (t, s)];
                if (st <= 0 || ps[svl::drumTrackParam (t, svl::DT_MUTE)] > 0.5f) continue;
                const double tick = on * kPpq, off = std::min (on + 0.5 * stepLen, totalBeats) * kPpq;
                seq.addEvent (juce::MidiMessage::noteOn (ch, 36 + t, (juce::uint8) (st == 2 ? 127 : 89)), std::round (tick));
                seq.addEvent (juce::MidiMessage::noteOff (ch, 36 + t), std::round (off) - 1);
            }
        }
    }
    else
    {   // the arp / sequencer run offline from the key's root: the notes they would play
        const double sr = 48000, bpm = 120, spb = sr * 60.0 / bpm;
        auto gp = ps;
        gp[svl::SEQ_SOURCE] = 0;
        const bool arp = gp[svl::ARP_ON] > 0.5f, sq = gp[svl::SEQ_ON] > 0.5f;
        if (sq && ! arp) gp[svl::SEQ_MODE] = 1;   // free: from the key's root, no key needed
        auto gen = std::make_unique<svl::NoteGen>();
        gen->prepare (sr);
        gen->setTempo (bpm);
        gen->setParams (gp);
        gen->setTransport (false, false, 0);
        if (arp) gen->keyOn (1, 60, 0, 0.8f);
        const int64_t total = (int64_t) std::llround (totalBeats * spb);
        std::map<int, std::pair<int, double>> open;      // generated id -> key, tick
        const float bendRange = std::max (0.01f, ps[svl::V_BEND]);   // semitones (voice.bend)
        int bend = 8192;
        auto tickOf = [&] (int64_t s) { return std::round ((double) s / spb * kPpq); };
        auto close = [&] (int id, double tick)
        {
            auto it = open.find (id);
            if (it == open.end()) return;
            seq.addEvent (juce::MidiMessage::noteOff (ch, it->second.first), std::max (it->second.second + 1, tick - 1));
            open.erase (it);
        };
        int64_t t = 0;
        for (int guard = 0; t < total && guard < 200000; ++guard)
        {
            svl::GenNote g;
            while (gen->pop (g))
            {
                const double tick = tickOf (t);
                if (g.kind == svl::GenNote::off) { close (g.id, tick); continue; }
                if (g.kind == svl::GenNote::slide) close (g.prevId, tick + 2);   // a slide: the two notes overlap (legato in the DAW)
                // g.add = the arp's octaves (12 each) + the step's ROW (a grid step up or down): the octaves go into the key, the row
                // becomes a pitch bend at JING FANG's bend range, so the clip played back into JING FANG sounds what the sequencer plays
                const int oct = juce::roundToInt (g.add / 12.0f);
                const float row = g.add - 12.0f * (float) oct;
                const int key = (g.key >= 0 ? g.key : juce::roundToInt (g.pitch)) + 12 * oct;
                if (key < 0 || key > 127) continue;
                const int wantBend = juce::jlimit (0, 16383, 8192 + juce::roundToInt (row / bendRange * 8192.0f));
                if (wantBend != bend) { seq.addEvent (juce::MidiMessage::pitchWheel (ch, wantBend), std::max (0.0, tick - 1)); bend = wantBend; }
                seq.addEvent (juce::MidiMessage::noteOn (ch, key, (juce::uint8) juce::jlimit (1, 127, juce::roundToInt (g.vel * 127.0f))), tick);
                open[g.id] = { key, tick };
            }
            const int64_t n = juce::jlimit ((int64_t) 1, total - t, (int64_t) gen->samplesToNext());
            gen->advance ((int) juce::jmin<int64_t> (n, 1 << 30));
            t += n;
        }
        while (! open.empty()) close (open.begin()->first, totalBeats * kPpq);
        if (bend != 8192) seq.addEvent (juce::MidiMessage::pitchWheel (ch, 8192), totalBeats * kPpq - 1);   // the clip ends unbent
    }
    seq.addEvent (juce::MidiMessage::endOfTrack(), totalBeats * kPpq);
    seq.updateMatchedPairs();
    juce::MidiFile f;
    f.setTicksPerQuarterNote (kPpq);
    f.addTrack (seq);
    return f;
}

static bool write (const juce::MidiFile& m, const juce::File& file)
{
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    juce::FileOutputStream os (file);
    return os.openedOk() && m.writeTo (os, 0);
}

juce::File writeTemp (const JingFangProcessor& p, int unit)
{
    const auto f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("JING FANG")
                       .getChildFile (juce::File::createLegalFileName (clipName (p, unit)) + ".mid");
    return write (make (p, unit), f) ? f : juce::File();
}

juce::File& saveFolderOverrideForTests() { static juce::File f; return f; }

juce::File saveToMusic (const JingFangProcessor& p, int unit)
{
    const auto dir = saveFolderOverrideForTests() != juce::File() ? saveFolderOverrideForTests()
                   : juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("JING FANG").getChildFile ("MIDI");
    const auto base = juce::File::createLegalFileName (clipName (p, unit));
    auto f = dir.getChildFile (base + ".mid");
    for (int i = 2; f.exists(); ++i) f = dir.getChildFile (base + " " + juce::String (i) + ".mid");
    return write (make (p, unit), f) ? f : juce::File();
}
} // namespace jfclip
