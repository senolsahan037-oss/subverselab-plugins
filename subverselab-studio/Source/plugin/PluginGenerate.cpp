// GENERATE in the processor (docs/GENERATE_PLAN.md §8): seeds, locks, history, the repetition filter, MUTATE, and writing the
// steps as host parameters (one gesture each, so the host's undo and automation see a roll like a hand move).
#include "PluginProcessor.h"

using namespace svl::gen;

namespace
{
const int kGenKnobIds[9] = { svl::GEN_DENSITY, svl::GEN_COMPLEX, svl::GEN_MUTATE, svl::GEN_RANGE, svl::GEN_MICRO, svl::GEN_SCALE,
                             svl::GEN_DRUM_DENSITY, svl::GEN_DRUM_COMPLEX, svl::GEN_DRUM_MUTATE };
juce::String upper (const char* s) { return juce::String (s).toUpperCase(); }
const juce::String kDot (juce::CharPointer_UTF8 (" \xc2\xb7 "));   // " · "
}

Role JingFangProcessor::genRole() const
{
    const bool arp = exactShared[svl::ARP_ON].load() > 0.5f;
    const bool mono = (int) exactShared[svl::V_MODE].load() != (int) svl::VoiceMode::poly;
    return roleForFamily (loadedMeta.family, arp, mono, 60.0f);
}

bool JingFangProcessor::genMicroAllowed() const
{
    return exactShared[svl::TUNE_MODE].load() > 0.5f && ! labActive();
}

Settings JingFangProcessor::genSettings (int unit) const
{
    Settings st;
    auto v = [this] (int i) { return exactShared[(size_t) i].load(); };
    if (unit == 0)
    {
        st.density = v (svl::GEN_DENSITY); st.complexity = v (svl::GEN_COMPLEX); st.range = v (svl::GEN_RANGE); st.micro = v (svl::GEN_MICRO);
        st.scale = juce::roundToInt (v (svl::GEN_SCALE)); st.microAllowed = genMicroAllowed();
        st.length = juce::jlimit (1, svl::kSeqSteps, juce::roundToInt (v (svl::SEQ_LEN)));
    }
    else
    {
        st.density = v (svl::GEN_DRUM_DENSITY); st.complexity = v (svl::GEN_DRUM_COMPLEX);
        st.length = juce::jlimit (1, svl::kDrumSteps, juce::roundToInt (v (svl::DRUMS_LEN)));
    }
    return st;
}

Scale JingFangProcessor::genScale() const
{
    Scale sc;
    if (! labActive()) return sc;   // empty: the generator picks a 12-key scale from the NOTES seed or gen.scale
    const auto& t = labMsg.a;
    if (t.mapSize <= 0) { sc.period = juce::jmax (1, t.size()); for (int d = 0; d < sc.period; ++d) sc.deg.push_back (d); }
    else
    {
        sc.period = t.mapSize;
        for (int k = 0; k < t.mapSize && k < (int) t.mapping.size(); ++k) if (t.mapping[(size_t) k] >= 0) sc.deg.push_back (k);
        if (sc.deg.empty() || sc.deg.front() != 0) { sc.deg.insert (sc.deg.begin(), 0); }
    }
    sc.id = 0;
    return sc;
}

std::array<Drum, svl::kDrumTracks> JingFangProcessor::genKit() const
{
    std::array<Drum, svl::kDrumTracks> k {};
    for (int t = 0; t < svl::kDrumTracks; ++t) k[(size_t) t] = drumForName (drumTrackName (t, true).toStdString());
    return k;
}

juce::String JingFangProcessor::genWrites (int unit) const
{
    const auto& u = genUnits[(size_t) juce::jlimit (0, 1, unit)];
    if (unit == 1)
    {
        juce::String s = "BEAT";
        if (u.pos >= 0) s << kDot << upper (kDrumStyleNames[juce::jlimit (0, kNumDrumStyles - 1, u.hist[(size_t) u.pos].drum.style)]);
        return s;
    }
    const auto role = genRole();
    juce::String s = kRoleNames[(int) role];
    if (role == Role::arpRhythm) return s;
    if (labActive()) return s + kDot + "LAB";
    const int fixed = juce::roundToInt (exactShared[svl::GEN_SCALE].load());
    int id = fixed;
    if (id == 0 && u.pos >= 0) id = u.hist[(size_t) u.pos].seq.scaleId;
    return id > 0 ? s + kDot + upper (kScaleNames[id]) : s;
}

SeqPattern JingFangProcessor::genCurrentSeq() const
{
    SeqPattern p;
    auto v = [this] (int i) { return exactShared[(size_t) i].load(); };
    p.length = juce::jlimit (1, svl::kSeqSteps, juce::roundToInt (v (svl::SEQ_LEN)));
    for (int s = 0; s < svl::kSeqSteps; ++s)
    {
        auto& x = p.s[(size_t) s];
        x.on = v (svl::stepParam (s, svl::S_ON)) > 0.5f; x.note = juce::roundToInt (v (svl::stepParam (s, svl::S_NOTE)));
        x.row = juce::roundToInt (v (svl::stepParam (s, svl::S_ROW))); x.vel = v (svl::stepParam (s, svl::S_VEL)); x.gate = v (svl::stepParam (s, svl::S_GATE));
        x.slide = v (svl::stepParam (s, svl::S_SLIDE)) > 0.5f; x.accent = v (svl::stepParam (s, svl::S_ACCENT)) > 0.5f;
        x.prob = v (svl::stepProbParam (s)); x.ratchet = juce::roundToInt (v (svl::stepRatchetParam (s)));
        for (int c = 0; c < 3; ++c) x.chord[(size_t) c] = juce::roundToInt (v (svl::stepChordParam (s, c)));
    }
    return p;
}

void JingFangProcessor::genWrite (int unit, const GenEntry& e)
{
    std::vector<std::pair<int, float>> w;
    if (unit == 0)
    {
        const bool wasOff = exactShared[svl::SEQ_ON].load() < 0.5f;
        for (int s = 0; s < svl::kSeqSteps; ++s)
        {
            const auto& x = e.seq.s[(size_t) s];
            w.push_back ({ svl::stepParam (s, svl::S_ON), x.on ? 1.0f : 0.0f });
            w.push_back ({ svl::stepParam (s, svl::S_NOTE), (float) x.note });
            w.push_back ({ svl::stepParam (s, svl::S_ROW), (float) x.row });
            w.push_back ({ svl::stepParam (s, svl::S_VEL), x.vel });
            w.push_back ({ svl::stepParam (s, svl::S_GATE), x.gate });
            w.push_back ({ svl::stepParam (s, svl::S_SLIDE), x.slide ? 1.0f : 0.0f });
            w.push_back ({ svl::stepParam (s, svl::S_ACCENT), x.accent ? 1.0f : 0.0f });
            w.push_back ({ svl::stepProbParam (s), x.prob });
            w.push_back ({ svl::stepRatchetParam (s), (float) x.ratchet });
            for (int c = 0; c < 3; ++c) w.push_back ({ svl::stepChordParam (s, c), (float) x.chord[(size_t) c] });
        }
        w.push_back ({ svl::SEQ_ON, 1.0f });
        // a sound whose sequencer was off gets a rate that suits what is written: chords move in eighths (16 steps = 2 bars)
        if (wasOff) w.push_back ({ svl::SEQ_RATE, genRole() == Role::chords ? 1.0f : 3.0f });
    }
    else
    {
        for (int t = 0; t < svl::kDrumTracks; ++t)
            for (int s = 0; s < svl::kDrumSteps; ++s) w.push_back ({ svl::drumStepParam (t, s), (float) e.drum.s[(size_t) t][(size_t) s] });
        w.push_back ({ svl::DRUMS_ON, 1.0f });
    }
    armGlide();
    for (auto& [i, v] : w)
    {
        auto* p = hostParam[(size_t) i];
        const float n = p->convertTo0to1 (v);
        if (std::abs (p->getValue() - n) < 1.0e-7f) continue;
        p->beginChangeGesture();
        p->setValueNotifyingHost (n);
        p->endChangeGesture();
        // the panel and the next roll read exactShared: give it the value now (the audio thread confirms it on its next block)
        exactShared[(size_t) i].store (svl::clampParam (i, v));
    }
    if (juce::MessageManager::existsAndIsCurrentThread()) glideUntil = (juce::int64) juce::Time::getMillisecondCounter() + 600;
}

void JingFangProcessor::genRoll (int unit, int which)
{
    unit = juce::jlimit (0, 1, unit);
    auto& u = genUnits[(size_t) unit];
    if (which != 0 && unit == 0 && u.r >= 0)
    {   // RHYTHM / NOTES: the other half keeps its seed for this roll
        const bool lr = u.lockR, ln = u.lockN;
        u.lockR = which == 2; u.lockN = which == 1;
        genRoll (unit, 0);
        u.lockR = lr; u.lockN = ln;
        return;
    }
    if (u.lockR && u.lockN && u.r >= 0) return;   // both halves kept: nothing to roll
    const auto st = genSettings (unit);
    const float m = exactShared[(size_t) (unit == 0 ? svl::GEN_MUTATE : svl::GEN_DRUM_MUTATE)].load();
    auto nextSeed = [this] { return genRandom.nextInt (10000); };
    GenEntry e;
    if (unit == 0)
    {
        const auto role = genRole();
        const auto sc = genScale();
        const auto now = genCurrentSeq();
        std::vector<SeqPattern> hist;
        for (auto& h : u.hist) hist.push_back (h.seq);
        const int keepR = u.r, keepN = u.n;
        // one random number feeds the half that moves; a locked half keeps its seed
        auto make = [&] (int s)
        {
            const int r = (u.lockR && keepR >= 0) ? keepR : s;
            const int n = (u.lockN && keepN >= 0) ? keepN : (s * 7919 + 4243) % 10000;
            auto fresh = makeSeq (role, sc, r, n, st);
            return m < 1.0f ? mutate (now, fresh, m, s) : fresh;
        };
        int seed = 0;
        if (m < 1.0f)
        {   // MUTATE: close to the pattern on purpose; only make sure something changed
            for (int i = 0; i < kMaxTries; ++i) { seed = nextSeed(); e.seq = make (seed); if (! (e.seq == now)) break; }
            u.skipped = 0;
        }
        else u.skipped = pickNovel (hist, make, nextSeed, seed, e.seq);
        e.r = (u.lockR && keepR >= 0) ? keepR : seed;
        e.n = (u.lockN && keepN >= 0) ? keepN : (seed * 7919 + 4243) % 10000;
    }
    else
    {
        const auto kit = genKit();
        DrumPattern now;
        now.length = st.length;
        for (int t = 0; t < svl::kDrumTracks; ++t) for (int s = 0; s < svl::kDrumSteps; ++s)
            now.s[(size_t) t][(size_t) s] = juce::roundToInt (exactShared[(size_t) svl::drumStepParam (t, s)].load());
        std::vector<DrumPattern> hist;
        for (auto& h : u.hist) hist.push_back (h.drum);
        auto make = [&] (int s) { auto fresh = makeDrums (kit, s, st); return m < 1.0f ? mutate (now, fresh, m, s) : fresh; };
        int seed = 0;
        if (m < 1.0f) { for (int i = 0; i < kMaxTries; ++i) { seed = nextSeed(); e.drum = make (seed); if (! (e.drum == now)) break; } u.skipped = 0; }
        else u.skipped = pickNovel (hist, make, nextSeed, seed, e.drum);
        e.r = seed; e.n = -1;
    }
    // the history: a roll after stepping back drops the rolls ahead of it (like an undo list)
    if (u.pos >= 0 && u.pos < (int) u.hist.size() - 1) u.hist.resize ((size_t) u.pos + 1);
    u.hist.push_back (e);
    if ((int) u.hist.size() > kHistory) u.hist.erase (u.hist.begin());
    u.pos = (int) u.hist.size() - 1;
    u.r = e.r; u.n = e.n; u.live = true;
    for (int i = 0; i < 9; ++i) genKnobsSeen[(size_t) i] = exactShared[(size_t) kGenKnobIds[i]].load();
    genWrite (unit, e);
}

void JingFangProcessor::genHistory (int unit, int delta)
{
    auto& u = genUnits[(size_t) juce::jlimit (0, 1, unit)];
    if (u.hist.empty()) return;
    const int p = juce::jlimit (0, (int) u.hist.size() - 1, u.pos + delta);
    if (p == u.pos) return;
    u.pos = p; u.r = u.hist[(size_t) p].r; u.n = u.hist[(size_t) p].n; u.live = true;
    genWrite (unit, u.hist[(size_t) p]);
}

void JingFangProcessor::genFollowKnobs()
{
    // a GEN knob moved after a roll: the same seeds are written again at the new setting (DENSITY adds / removes notes of the
    // same pattern). After a preset load nothing is live, so a knob only takes effect at the next roll.
    std::array<bool, 2> moved {};
    for (int i = 0; i < 9; ++i)
    {
        const float v = exactShared[(size_t) kGenKnobIds[i]].load();
        if (v != genKnobsSeen[(size_t) i])
        {
            genKnobsSeen[(size_t) i] = v;
            if (kGenKnobIds[i] == svl::GEN_MUTATE || kGenKnobIds[i] == svl::GEN_DRUM_MUTATE) continue;   // MUTATE acts on the next roll
            moved[i < 6 ? 0 : 1] = true;
        }
    }
    for (int unit = 0; unit < 2; ++unit)
    {
        auto& u = genUnits[(size_t) unit];
        if (! moved[(size_t) unit] || ! u.live || u.pos < 0) continue;
        auto& e = u.hist[(size_t) u.pos];
        const auto st = genSettings (unit);
        if (unit == 0) e.seq = makeSeq (genRole(), genScale(), e.r, e.n, st);
        else e.drum = makeDrums (genKit(), e.r, st);
        genWrite (unit, e);
    }
}

juce::String JingFangProcessor::genXmlLine() const
{
    juce::String l;
    for (int unit = 0; unit < 2; ++unit)
    {
        const auto& u = genUnits[(size_t) unit];
        if (u.r < 0) continue;
        l << "  <gen unit=\"" << unit << "\" r=\"" << u.r << "\" n=\"" << u.n << "\" lockr=\"" << (u.lockR ? 1 : 0) << "\" lockn=\"" << (u.lockN ? 1 : 0) << "\"/>\n";
    }
    return l;
}

void JingFangProcessor::genReadXml (const juce::String& xml)
{
    for (auto& u : genUnits) { u = GenUnit(); }
    for (auto& e : svl::detail::elements (xml.toStdString(), "gen"))
    {
        const int unit = juce::jlimit (0, 1, juce::String (svl::detail::attr (e, "unit")).getIntValue());
        auto& u = genUnits[(size_t) unit];
        u.r = juce::String (svl::detail::attr (e, "r")).getIntValue();
        u.n = juce::String (svl::detail::attr (e, "n")).getIntValue();
        u.lockR = juce::String (svl::detail::attr (e, "lockr")) == "1";
        u.lockN = juce::String (svl::detail::attr (e, "lockn")) == "1";
    }
    for (int i = 0; i < 9; ++i) genKnobsSeen[(size_t) i] = exactShared[(size_t) kGenKnobIds[i]].load();
}
