/*  MIHENK personal profile checks (SPEC_V2 §7).

        profile_check          all checks (exit code 0 only if every check passes)

    - Learning on synthetic sessions: a simulated listener with a hidden taste writes a note after every master;
      the learned value converges, and a lock is proposed only once the evidence threshold is met.
    - Inconsistent notes (both directions, or accepted values far apart) never lock; notes on dissimilar tracks
      never lock this one; overrides are learned.
    - The store on disk: shared between instances, delete one rule, reset, export / import round trip.
    - In the product: the app locks on file load, the plug-in locks on LEARN; TARGET LOCK still lands on the
      target; host automation of a locked knob unlocks it; the state keeps the switch and the locks; no C++ heap
      allocation inside processBlock while playing with locks (and a note written on the message thread meanwhile).
    Every file goes to a temporary folder; the user's own profile is never touched.
*/
#include <JuceHeader.h>
#include <atomic>
#include <cstdlib>
#include <new>
#include "../Source/plugin/PluginProcessor.h"
#include "../Source/engine/Profile.h"
#include "../Source/engine/Preset.h"

using namespace mihenk;
namespace pf = mihenk::profile;

//==============================================================================
namespace
{
thread_local bool inAudio = false;
std::atomic<long> audioAllocs { 0 };
}
void* operator new (std::size_t n)
{
    if (inAudio) audioAllocs.fetch_add (1);
    if (void* p = std::malloc (n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}
void* operator new[] (std::size_t n)
{
    if (inAudio) audioAllocs.fetch_add (1);
    if (void* p = std::malloc (n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}
void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }

namespace
{
int failures = 0, passes = 0;
void check (bool ok, const juce::String& what)
{
    std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8());
    std::fflush (stdout);
    if (ok) ++passes; else ++failures;
}
void section (const char* name) { std::printf ("\n-- %s\n", name); }
void pump (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

using Buffer = juce::AudioBuffer<float>;

struct FakePlayHead : juce::AudioPlayHead
{
    bool playing = true;
    juce::Optional<PositionInfo> getPosition() const override { PositionInfo p; p.setIsPlaying (playing); return p; }
};

Buffer song (double fs, double secs, uint32_t seed, double bright = 0.0)
{
    const int n = (int) (secs * fs);
    Buffer b (2, n);
    juce::Random rng ((juce::int64) seed);
    double lp[2] { 0.0, 0.0 };
    for (int i = 0; i < n; ++i)
    {
        const double t = i / fs, tb = std::fmod (t, 0.5), sec = std::fmod (t, 8.0) < 4.0 ? 0.35 : 1.0;
        const double kick = std::exp (-tb * 18.0) * std::sin (2.0 * kPi * (48.0 * tb + 2.0 * (1.0 - std::exp (-tb * 30.0))));
        const double bass = 0.25 * std::sin (2.0 * kPi * 55.0 * t);
        const double th = std::fmod (t + 0.25, 0.25);
        const double hat = std::exp (-th * 60.0) * (rng.nextDouble() * 2.0 - 1.0) * (0.15 + bright);
        const double fade = juce::jmin (1.0, t / 0.02, (secs - t) / 0.02);
        for (int c = 0; c < 2; ++c)
        {
            const double w = rng.nextDouble() * 2.0 - 1.0;
            lp[c] += 0.05 * (w - lp[c]);
            b.setSample (c, i, (float) (fade * sec * (0.55 * kick + bass + 0.25 * lp[c] * 1.5 + hat * (c == 0 ? 1.3 : 0.7) + bright * 0.3 * (w - lp[c]))));
        }
    }
    return b;
}

Buffer play (MihenkProcessor& p, const Buffer& in, int block, int extra, bool countAllocs = false, std::function<void (int)> between = {})
{
    const int n = in.getNumSamples(), total = n + extra;
    Buffer out (2, total);
    out.clear();
    for (int c = 0; c < 2; ++c) out.copyFrom (c, 0, in, c, 0, n);
    juce::MidiBuffer midi;
    int k = 0;
    for (int pos = 0; pos < total; pos += block, ++k)
    {
        const int len = juce::jmin (block, total - pos);
        Buffer view (out.getArrayOfWritePointers(), 2, pos, len);
        inAudio = countAllocs;
        p.processBlock (view, midi);
        inAudio = false;
        if (between) between (k);
    }
    return out;
}

/** A track similar to `m`: every feature moved by a fraction of its step (well inside kSimilar). */
DiagnosisMeasures jitter (const DiagnosisMeasures& m, juce::Random& rng, double amount = 0.25)
{
    auto x = m;
    auto r = [&] { return (rng.nextDouble() * 2.0 - 1.0) * amount; };
    x.plr += 3.0 * r(); x.lra += 3.0 * r(); x.lineSlopeDbPerOct += r(); x.lowDb += 3.0 * r(); x.airRelDb += 3.0 * r();
    x.harshDb += 2.0 * r(); x.mudDb += 2.0 * r(); x.sideMidDb += 4.0 * r(); x.correlation = juce::jlimit (-1.0, 1.0, x.correlation + 0.2 * r());
    return x;
}

pf::Note note (const DiagnosisMeasures& m, const MasterSettings& s, juce::StringArray tags, juce::Time t, std::vector<pf::Override> ov = {})
{
    pf::Context c;
    c.measures = &m;
    c.settings = &s;
    c.source = "test";
    c.track = "synthetic";
    c.overrides = std::move (ov);
    return pf::makeNote (tags, "", c, t);
}

const pf::Rule* ruleFor (const pf::Learned& L, int group, int param)
{
    for (auto& r : L.rules) if (r.group == group && r.param == param) return &r;
    return nullptr;
}

const pf::Proposal* proposalFor (const std::vector<pf::Proposal>& ps, int param)
{
    for (auto& p : ps) if (p.param == param) return &p;
    return nullptr;
}

bool containsBannedWord (const juce::String& s)
{
    const auto t = " " + s.toLowerCase().replaceCharacters (".,:;()\"'/-", "          ") + " ";
    return t.contains (" ai ") || t.contains ("machine learning") || t.contains ("neural");
}

juce::File tempDir (const juce::String& name)
{
    auto d = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("mihenk_profile_check").getChildFile (name);
    d.deleteRecursively();
    d.createDirectory();
    return d;
}

//==============================================================================
int checks()
{
    const double fs = 48000.0;
    auto track = song (fs, 16.0, 11);
    track.applyGain (0.35f);              // an unmastered mix (quiet, peaks far from the ceiling): a finished master takes no tone change at all
    const auto base = measureMix (track, fs);
    check (base.valid && pf::featuresOf (base).valid, "The synthetic track measures: PLR " + juce::String (base.plr, 1) + " LU, profile \"" + pf::describe (pf::featuresOf (base)) + "\"");

    section ("Consistent sessions: a listener who wants AIR +1.5 dB and WIDTH 90 %");
    {
        juce::Random rng (7);
        std::vector<pf::Note> notes;
        juce::Time t (2026, 8, 1, 10, 0);
        int firstLock = -1, lockWidthAt = -1;
        std::vector<double> airHistory, widthHistory;
        bool earlyLock = false;
        for (int session = 0; session < 30; ++session)
        {
            const auto m = jitter (base, rng);
            MasterSettings s;
            applyGenreStartingPoint (s, Genre::rapTrap);          // AIR 0 dB, WIDTH 100 %
            const auto learned = pf::learn (notes);
            const auto props = pf::propose (learned, pf::featuresOf (m), s);
            if (auto* p = proposalFor (props, pf::pAir)) { pf::set (s, pf::pAir, p->value); if (firstLock < 0) firstLock = session; if (session < pf::kMinEvidence) earlyLock = true; }
            if (auto* p = proposalFor (props, pf::pWidth)) { pf::set (s, pf::pWidth, p->value); if (lockWidthAt < 0) lockWidthAt = session; }
            airHistory.push_back (pf::get (s, pf::pAir));
            widthHistory.push_back (pf::get (s, pf::pWidth));
            // The listener: says what is wrong, or "perfect" when both are within their taste.
            juce::StringArray tags;
            const double air = pf::get (s, pf::pAir), width = pf::get (s, pf::pWidth);
            if (air < 1.25) tags.add (pf::tagId (pf::tNoAir));
            if (air > 1.75) tags.add (pf::tagId (pf::tHarsh));
            if (width > 0.92) tags.add (pf::tagId (pf::tTooWide));
            if (tags.isEmpty()) tags.add (pf::tagId (pf::tPerfect));
            notes.push_back (note (m, s, tags, t));
            t = t + juce::RelativeTime::hours (6);
        }
        check (! earlyLock && firstLock == pf::kMinEvidence, "No lock with 0, 1 or 2 notes; the AIR lock is first proposed on session " + juce::String (firstLock + 1)
                                                             + " (after " + juce::String (pf::kMinEvidence) + " agreeing notes)");
        const auto L = pf::learn (notes);
        check (L.groups.size() == 1, "All 30 similar tracks fall in one group (" + juce::String ((int) L.groups.size()) + ")");
        const auto* air = ruleFor (L, 0, pf::pAir);
        const auto* width = ruleFor (L, 0, pf::pWidth);
        juce::String airPath, widthPath;
        for (size_t i = 0; i < airHistory.size(); i += 3) airPath << juce::String (airHistory[i], 1) << " ";
        for (size_t i = 0; i < widthHistory.size(); i += 3) widthPath << juce::String (juce::roundToInt (widthHistory[i] * 100.0)) << " ";
        bool stable = true;
        for (size_t i = airHistory.size() - 8; i < airHistory.size(); ++i) stable &= airHistory[i] >= 1.25 - 1.0e-9 && airHistory[i] <= 1.75 + 1.0e-9;
        check (air != nullptr && air->state == pf::Rule::State::acting && air->value >= 1.25 && air->value <= 1.75 && stable,
               "AIR converges into the listener's taste (1.25..1.75 dB): learned " + (air != nullptr ? pf::formatValue (pf::pAir, air->value) + " from " + juce::String (air->evidence) + " notes" : juce::String ("-"))
               + "; lock per session: " + airPath.trim());
        bool wStable = true;
        for (size_t i = widthHistory.size() - 8; i < widthHistory.size(); ++i) wStable &= widthHistory[i] <= 0.92 + 1.0e-9 && widthHistory[i] >= 0.85;
        check (width != nullptr && width->state == pf::Rule::State::acting && width->value <= 0.92 + 1.0e-9 && width->value >= 0.85 && wStable,
               "WIDTH converges (85..92 %): learned " + (width != nullptr ? pf::formatValue (pf::pWidth, width->value) : juce::String ("-")) + "; lock per session: " + widthPath.trim());
        int perfect = 0;
        for (size_t i = notes.size() - 8; i < notes.size(); ++i) perfect += notes[i].tags.contains (pf::tagId (pf::tPerfect)) ? 1 : 0;
        check (perfect == 8, "The last 8 masters were all \"perfect\" for the listener (" + juce::String (perfect) + " of 8)");
        const auto props = pf::propose (L, pf::featuresOf (jitter (base, rng)), [] { MasterSettings s; applyGenreStartingPoint (s, Genre::rapTrap); return s; }());
        const auto* pa = proposalFor (props, pf::pAir);
        check (pa != nullptr && pa->reason.startsWith ("Locked from ") && pa->reason.contains ("of your notes on similar tracks: AIR +") && ! containsBannedWord (pa->reason),
               "The lock explains itself: \"" + (pa != nullptr ? pa->reason : juce::String()) + "\"");
        // Knobs the listener never commented on are not locked.
        int others = 0;
        for (auto& p : props) others += (p.param != pf::pAir && p.param != pf::pWidth) ? 1 : 0;
        check (others == 0, "Knobs the notes said nothing about stay unlocked (" + juce::String (others) + " other proposals)");
    }

    section ("Inconsistent notes never lock");
    {
        juce::Random rng (9);
        juce::Time t (2026, 8, 2, 10, 0);
        MasterSettings s;
        applyGenreStartingPoint (s, Genre::rapTrap);
        std::vector<pf::Note> notes;
        for (int i = 0; i < 8; ++i, t = t + juce::RelativeTime::hours (1))
            notes.push_back (note (jitter (base, rng), s, { pf::tagId (i % 2 == 0 ? pf::tNoAir : pf::tHarsh) }, t));
        auto L = pf::learn (notes);
        const auto* r = ruleFor (L, 0, pf::pAir);
        auto props = pf::propose (L, pf::featuresOf (base), s);
        check (r != nullptr && r->state == pf::Rule::State::mixed && proposalFor (props, pf::pAir) == nullptr,
               "8 notes alternating \"not enough air\" / \"top too harsh\": AIR rule is MIXED, no lock (" + (r != nullptr ? r->why : juce::String ("no rule")) + ")");
        notes.clear();
        const double accepted[] { 0.5, 3.5, 1.0, 5.0, 2.5, 0.0 };
        for (double a : accepted)
        {
            auto x = s;
            pf::set (x, pf::pAir, a);
            notes.push_back (note (jitter (base, rng), x, { pf::tagId (pf::tPerfect) }, t));
            t = t + juce::RelativeTime::hours (1);
        }
        L = pf::learn (notes);
        r = ruleFor (L, 0, pf::pAir);
        props = pf::propose (L, pf::featuresOf (base), s);
        check (r != nullptr && r->state == pf::Rule::State::mixed && proposalFor (props, pf::pAir) == nullptr,
               "6 \"perfect\" notes with AIR from 0 to 5 dB: spread " + juce::String (r != nullptr ? r->spread : 0.0, 2) + " dB over the tolerance, no lock");
    }

    section ("Other tracks, overrides");
    {
        juce::Random rng (21);
        juce::Time t (2026, 8, 3, 10, 0);
        MasterSettings s;
        applyGenreStartingPoint (s, Genre::rapTrap);
        auto far = base;
        far.plr += 12.0; far.lra += 9.0; far.airRelDb -= 12.0; far.sideMidDb -= 12.0;
        std::vector<pf::Note> notes;
        for (int i = 0; i < 4; ++i, t = t + juce::RelativeTime::hours (1))
        {
            auto x = s;
            pf::set (x, pf::pAir, 2.0);
            notes.push_back (note (jitter (far, rng, 0.1), x, { pf::tagId (pf::tPerfect) }, t));
        }
        auto L = pf::learn (notes);
        double d = 0.0;
        const int g = pf::nearestGroup (L, pf::featuresOf (base), &d);
        check (ruleFor (L, 0, pf::pAir) != nullptr && ruleFor (L, 0, pf::pAir)->state == pf::Rule::State::acting && g < 0 && pf::propose (L, pf::featuresOf (base), s).empty(),
               "4 notes on very different tracks (PLR +12 LU, darker, narrower) teach AIR +2.0 dB for those; this track is not like them: no lock");
        notes.clear();
        for (int i = 0; i < 3; ++i, t = t + juce::RelativeTime::hours (1))
        {
            auto x = s;
            pf::set (x, pf::pAir, 2.5);
            notes.push_back (note (jitter (base, rng), x, {}, t, { { pf::pAir, 1.0, 2.5 } }));
        }
        L = pf::learn (notes);
        const auto props = pf::propose (L, pf::featuresOf (base), s);
        const auto* p = proposalFor (props, pf::pAir);
        check (p != nullptr && std::abs (p->value - 2.5) < 1.0e-9, "3 notes where the user unlocked AIR (locked at +1.0) and set it to +2.5 dB: the override is learned, lock at "
                                                                     + (p != nullptr ? pf::formatValue (pf::pAir, p->value) : juce::String ("-")));
    }

    section ("The store on disk: shared, delete a rule, reset, export / import");
    {
        const auto dir = tempDir ("store");
        pf::Store a;
        a.setFolder (dir);
        juce::Random rng (5);
        juce::Time t (2026, 8, 4, 10, 0);
        MasterSettings s;
        applyGenreStartingPoint (s, Genre::rapTrap);
        juce::String err;
        bool ok = true;
        for (int i = 0; i < 4; ++i, t = t + juce::RelativeTime::hours (1))
        {
            auto x = s;
            pf::set (x, pf::pAir, 2.0);
            pf::set (x, pf::pBell2, 1.5);
            ok &= a.add (note (jitter (base, rng), x, { pf::tagId (pf::tPerfect) }, t), err);
        }
        pf::Store b;                                   // another instance (the plug-in while the app is open)
        b.setFolder (dir);
        const auto la = a.learned(), lb = b.learned();
        check (ok && b.notes().size() == 4 && la.rules.size() == lb.rules.size() && ! lb.rules.empty() && a.notesFile().existsAsFile(),
               "4 notes written by one instance are read by another: " + juce::String ((int) lb.rules.size()) + " rule(s), file " + a.notesFile().getFileName());
        // Export, then check the round trip after a reset.
        const auto exported = dir.getSiblingFile ("exported_profile.json");
        const bool exp = a.exportTo (exported, err);
        const juce::String airKey = [&] { for (auto& r : la.rules) if (r.param == pf::pAir) return r.key; return juce::String(); }();
        const bool del = a.deleteRule (airKey, err);
        const auto afterDel = a.learned();
        bool airGone = true;
        for (auto& r : afterDel.rules) airGone &= r.key != airKey;
        check (del && airGone && a.notes().size() == 4 && afterDel.rules.size() + 1 == la.rules.size(), "Delete one rule (AIR): it is gone, the other rule and all 4 notes stay");
        b.refresh();
        check (b.learned().rules.size() == afterDel.rules.size(), "The other instance sees the deletion (file changed on disk)");
        const bool reset = a.resetAll (err);
        juce::Array<juce::File> aside;
        dir.findChildFiles (aside, juce::File::findFiles, false, "notes-reset-*.jsonl");
        check (reset && a.notes().empty() && a.learned().rules.empty() && a.learned().groups.empty() && aside.size() == 1 && ! a.notesFile().existsAsFile(),
               "RESET ALL clears the profile: 0 notes, 0 rules, 0 groups (the old file kept aside as " + (aside.isEmpty() ? juce::String ("-") : aside[0].getFileName()) + ")");
        pf::Note probe;
        const int imported = a.importFrom (exported, err);
        const auto li = a.learned();
        bool same = exp && imported == 4 && li.rules.size() == la.rules.size();
        for (size_t i = 0; same && i < li.rules.size(); ++i)
            same &= li.rules[i].key == la.rules[i].key && std::abs (li.rules[i].value - la.rules[i].value) < 1.0e-12 && li.rules[i].evidence == la.rules[i].evidence;
        check (same, "Export / import round trip: " + juce::String (imported) + " notes back, the same " + juce::String ((int) li.rules.size()) + " rules (keys, values, evidence)");
        check (a.importFrom (exported, err) == 0 && a.notes().size() == 4, "Importing the same profile again adds nothing (duplicates by id are skipped)");
        // A note written with the whole context, as the panel writes it.
        const auto inMeas = analyse (track, fs), outMeas = analyse (track, fs);
        GainReductionStats lim;
        lim.maxDb = 2.5;
        const auto diag = diagnose (base, s);
        pf::Context c;
        c.measures = &base; c.input = &inMeas; c.output = &outMeas; c.limiter = &lim; c.findings = &diag.findings; c.settings = &s;
        c.source = "app"; c.track = "full.wav"; c.startingPoint = "HIP-HOP / RAP"; c.scenarios.add ("too-hot");
        a.add (pf::makeNote ({ pf::tagId (pf::tHarsh) }, "hats bite", c), err);
        pf::Store fresh;
        fresh.setFolder (dir);
        const auto v = pf::toVar (fresh.notes().back());
        const auto line = juce::JSON::toString (v, true);
        check (v["input"]["integrated"].isDouble() && v["input"]["plr"].isDouble() && v["input"]["crest"].isDouble() && v["input"]["correlation"].isDouble()
                   && v["input"]["spectrumBandsDb"].isObject() && v["input"]["features"].isObject() && v["input"]["findings"].isArray()
                   && v["settings"]["eq"].isObject() && v["result"]["integrated"].isDouble() && v["result"]["limiterGrMaxDb"].isDouble()
                   && v["tags"].size() == 1 && v["text"] == "hats bite" && v["startingPoint"] == "HIP-HOP / RAP" && v["scenarios"].size() == 1,
               "A note carries the context: input loudness / PLR / crest / correlation / spectrum bands / findings, settings, result (" + juce::String (line.length()) + " bytes)");
        dir.getParentDirectory().deleteRecursively();
    }

    section ("In the app: locks on file load, TARGET LOCK intact, overrides, OFF by default");
    {
        const auto dir = tempDir ("app");
        MihenkProcessor p;
        p.setStandaloneModeForTesting (true);
        auto& ses = p.getSession();
        ses.profile.setFolder (dir);
        check (! ses.profileEnabled, "PERSONAL PROFILE is off by default");
        applyGenreStartingPoint (ses.settings, Genre::rapTrap);
        ses.settingsChanged();
        // Three agreeing notes on similar tracks: "not enough air" at AIR +1.5 dB (so +2.0 dB).
        juce::Random rng (3);
        juce::Time t (2026, 8, 5, 10, 0);
        juce::String err;
        for (int i = 0; i < 3; ++i, t = t + juce::RelativeTime::hours (1))
        {
            auto x = ses.settings;
            pf::set (x, pf::pAir, 1.5);
            ses.profile.add (note (jitter (base, rng), x, { pf::tagId (pf::tNoAir) }, t), err);
        }
        ses.setInputForTesting (Buffer (track), fs, "off.wav");
        check (ses.lockCount() == 0 && std::abs (ses.settings.eq.airDb) < 1.0e-9, "Profile off: loading a similar track locks nothing (AIR stays 0.0 dB)");
        ses.setProfileEnabled (true);
        std::printf ("INFO  plan: finished=%d (%s) refused: %s | PLR %.1f crest %.1f peak %.1f int %.1f\n", (int) ses.plan.finishedMaster, ses.plan.finishedWhy.toRawUTF8(), ses.plan.refused.joinIntoString (" / ").toRawUTF8(),
                    ses.measurements.plr, ses.measurements.crest, ses.measurements.samplePeak, ses.measurements.integrated);
        check (ses.isLocked (pf::pAir) && std::abs (ses.settings.eq.airDb - 2.0) < 1.0e-9, "Turned on with the track loaded: AIR locked at +2.0 dB");
        ses.setInputForTesting (Buffer (track), fs, "similar.wav");
        check (ses.isLocked (pf::pAir) && std::abs (ses.settings.eq.airDb - 2.0) < 1.0e-9 && ses.lockCount() == 1,
               "File load: AIR locked at " + pf::formatValue (pf::pAir, ses.settings.eq.airDb) + " (" + ses.locks[(size_t) pf::pAir].reason + ")");
        ses.renderNowForTesting();
        check (std::abs (ses.outMeas.integrated - ses.settings.targetLufs) <= 0.3 && ses.outMeas.truePeak <= ses.settings.limiter.ceilingDbtp + 0.01,
               "The plan with the locked knob (on the target within 0.3 LU): " + juce::String (ses.outMeas.integrated, 2) + " LUFS (target " + juce::String (ses.settings.targetLufs, 1) + "), "
               + juce::String (ses.outMeas.truePeak, 2) + " dBTP");
        ses.settings.eq.airDb = 0.0;
        ses.applyGenre (Genre::arabeskPop);              // a no-op now (starting points are gone)
        ses.settingsChanged();                           // a stray write to a locked knob is put back
        check (std::abs (ses.settings.eq.airDb - 2.0) < 1.0e-9, "A starting point or a change elsewhere does not move a locked knob (AIR held at +2.0 dB)");
        ses.unlock (pf::pAir);
        ses.settings.eq.airDb = 3.0;
        ses.settingsChanged();
        ses.renderNowForTesting();
        const bool saved = ses.addNote ({ pf::tagId (pf::tPerfect) }, "Air feels right now", "R&B / POP", err);
        const auto& last = ses.profile.notes().back();
        check (saved && ! ses.isLocked (pf::pAir) && std::abs (ses.settings.eq.airDb - 3.0) < 1.0e-9 && last.overrides.size() == 1
                   && std::abs (last.overrides[0].from - 2.0) < 1.0e-9 && std::abs (last.overrides[0].to - 3.0) < 1.0e-9 && last.text == "Air feels right now"
                   && last.startingPoint == "R&B / POP" && last.source == "app",
               "Unlock + move: AIR stays at +3.0 dB and the note stores the override (+2.0 > +3.0 dB), the text and the starting point");
        ses.setProfileEnabled (false);
        check (ses.lockCount() == 0, "Profile off again: every lock released");
        dir.getParentDirectory().deleteRecursively();
    }

    section ("In the plug-in: LEARN locks, automation overrides, state, real time");
    {
        const auto dir = tempDir ("plugin");
        MihenkProcessor p;
        p.setStandaloneModeForTesting (false);
        FakePlayHead head;
        p.setPlayHead (&head);
        p.setRateAndBufferSizeDetails (fs, 512);
        auto& ses = p.getSession();
        ses.profile.setFolder (dir);
        applyGenreStartingPoint (ses.settings, Genre::rapTrap);
        ses.settingsChanged();
        juce::Random rng (4);
        juce::Time t (2026, 8, 6, 10, 0);
        juce::String err;
        for (int i = 0; i < 4; ++i, t = t + juce::RelativeTime::hours (1))
        {
            auto x = ses.settings;
            pf::set (x, pf::pAir, 1.0);
            ses.profile.add (note (jitter (base, rng), x, { pf::tagId (pf::tNoAir) }, t), err);
        }
        ses.setProfileEnabled (true);
        p.setNonRealtime (true);
        p.prepareToPlay (fs, 512);
        p.toggleLearn();
        play (p, track, 512, 0);
        p.finishLearnNow();
        const float airParam = p.getState().getRawParameterValue ("airDb")->load();
        check (p.isLearned() && ses.isLocked (pf::pAir) && std::abs (airParam - 1.5f) < 1.0e-6f,
               "LEARN: AIR locked at +1.5 dB and written to the automatable parameter (" + juce::String (airParam, 2) + " dB)");
        p.prepareToPlay (fs, 512);
        const int lat = p.getLatencySamples();
        const auto out = play (p, track, 512, lat);
        Buffer aligned (2, track.getNumSamples());
        for (int c = 0; c < 2; ++c) aligned.copyFrom (c, 0, out, c, lat, track.getNumSamples());
        const auto m = analyse (aligned, fs, false);
        check (std::abs (m.integrated - ses.settings.targetLufs) <= 0.3 && std::abs (m.integrated - ses.outMeas.integrated) <= 0.1 && m.truePeak <= ses.settings.limiter.ceilingDbtp + 0.01,
               "Real-time output with the lock, after LEARN: " + juce::String (m.integrated, 2) + " LUFS (offline " + juce::String (ses.outMeas.integrated, 2) + "), " + juce::String (m.truePeak, 2) + " dBTP (the plan, on the target within 0.3 LU)");

        // State: the switch and the lock come back.
        juce::MemoryBlock state;
        p.getStateInformation (state);
        {
            MihenkProcessor q;
            q.setStandaloneModeForTesting (false);
            q.getSession().profile.setFolder (dir);
            q.setStateInformation (state.getData(), (int) state.getSize());
            check (q.getSession().profileEnabled && q.getSession().isLocked (pf::pAir) && std::abs (q.getSession().locks[(size_t) pf::pAir].value - 1.5) < 1.0e-9
                       && q.getSession().locks[(size_t) pf::pAir].reason == ses.locks[(size_t) pf::pAir].reason,
                   "State round trip: PERSONAL PROFILE on and the AIR lock (with its reason) restored in a new instance");
        }

        // Real time: no allocation in processBlock with the locks, while the message thread writes a note.
        p.setNonRealtime (false);
        p.prepareToPlay (fs, 512);
        play (p, song (fs, 0.5, 2), 512, 0);
        audioAllocs = 0;
        int notesWritten = 0;
        play (p, track, 512, 0, true, [&] (int k)
        {
            if (k == 200) notesWritten += ses.addNote ({ pf::tagId (pf::tNoAir) }, "", "HIP-HOP / RAP", err) ? 1 : 0;
            if (k % 50 == 0) pump (1);
        });
        check (audioAllocs.load() == 0 && notesWritten == 1, "C++ heap allocations inside processBlock while playing with a lock and writing a note between blocks: "
                                                             + juce::String (audioAllocs.load()));

        // Host automation of a locked knob: the lock lets go (an override).
        auto* prm = p.getBridge().find ("airDb");
        prm->setValueNotifyingHost (prm->convertTo0to1 (2.5f));
        pump (120);
        check (! ses.isLocked (pf::pAir) && std::abs (ses.settings.eq.airDb - 2.5) < 1.0e-4 && ses.overridden.count (pf::pAir) == 1,
               "Host automation moves the locked AIR to +2.5 dB: unlocked, kept, remembered as an override for the next note");
        p.setPlayHead (nullptr);
        dir.getParentDirectory().deleteRecursively();
    }

    section ("Wording");
    {
        juce::StringArray all;
        for (int t = 0; t < pf::kNumTags; ++t) { all.add (pf::tagText (t)); all.add (pf::describeTagRule (t)); }
        for (int q = 0; q < pf::kNumParams; ++q) all.add (pf::def (q).knob);
        bool clean = true;
        for (auto& s : all) clean &= ! containsBannedWord (s);
        check (clean, "Tags, tag rules and knob names never say AI, machine learning or neural");
    }

    std::printf ("\n%s (%d checks)\n", failures == 0 ? "ALL PASS" : (juce::String (failures) + " FAILED").toRawUTF8(), passes + failures);
    return failures == 0 ? 0 : 1;
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    return checks();
}
