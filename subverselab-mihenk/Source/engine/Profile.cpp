#include "Profile.h"
#include "Preset.h"
#include "../dsp/Spectrum.h"

namespace mihenk::profile
{
//==============================================================================
namespace
{
const ParamDef kDefs[kNumParams] {
    { "airDb",       "AIR",        0.5,  0.75, 0.3,  0.0, 6.0, 0.1 },
    { "highShelfDb", "HIGH SHELF", 0.5,  0.75, 0.3, -6.0, 6.0, 0.1 },
    { "bell2Db",     "BELL 2",     0.5,  0.75, 0.3, -8.0, 8.0, 0.1 },
    { "lowShelfDb",  "LOW SHELF",  0.5,  0.75, 0.3, -6.0, 6.0, 0.1 },
    { "tightDb",     "TIGHT CUT",  0.5,  0.75, 0.3,  0.0, 4.0, 0.1 },
    { "glueDb",      "GLUE",       0.3,  0.45, 0.2,  0.0, 4.0, 0.1 },
    { "width",       "WIDTH",      0.05, 0.08, 0.04, 0.0, 2.0, 0.01 },
};

const char* kTagIds[kNumTags] { "top-too-harsh", "not-enough-air", "low-end-loose", "vocal-buried", "too-squashed", "too-wide", "perfect" };
const char* kTagText[kNumTags] { "top too harsh", "not enough air", "low end loose", "vocal buried", "too squashed", "too wide", "perfect" };

// Input features: name, the step one unit stands for, and the clamp of the raw value.
struct FeatureDef { const char* name; double step, lo, hi; };
const FeatureDef kFeatures[kNumFeatures] {
    { "plr",         3.0,   0.0,  30.0 },     // peak-to-loudness ratio, LU
    { "lra",         3.0,   0.0,  30.0 },     // loudness range, LU
    { "slope",       1.0,  -6.0,   3.0 },     // spectral line, dB / octave (100 Hz..10 kHz)
    { "lowDb",       3.0, -30.0,  30.0 },     // 40-120 Hz over 300 Hz-1 kHz
    { "airDb",       3.0, -60.0,  20.0 },     // 10-16 kHz over 1-4 kHz
    { "harshDb",     2.0, -20.0,  20.0 },     // 2-5 kHz above the line
    { "mudDb",       2.0, -20.0,  20.0 },     // 200-500 Hz above the line
    { "sideMidDb",   4.0, -40.0,  10.0 },     // side / mid energy
    { "correlation", 0.2,  -1.0,   1.0 },
};

double finiteOr (double v, double fallback) { return std::isfinite (v) ? v : fallback; }
juce::var num (double v) { return std::isfinite (v) ? juce::var (v) : juce::var(); }
double roundTo (double v, double res) { return std::round (v / res) * res; }

juce::String signedDb (double v, int dec = 1)
{
    if (std::abs (v) < 0.5 * std::pow (10.0, -dec)) v = 0.0;
    return (v > 0.0 ? "+" : "") + juce::String (v, dec) + " dB";
}

std::array<double, kNumFeatures> rawFeatures (const Features& f)
{
    std::array<double, kNumFeatures> r {};
    for (int i = 0; i < kNumFeatures; ++i) r[(size_t) i] = f.v[(size_t) i] * kFeatures[i].step;
    return r;
}
} // namespace

const ParamDef& def (int p) { return kDefs[juce::jlimit (0, kNumParams - 1, p)]; }

int paramFromId (const juce::String& id)
{
    for (int p = 0; p < kNumParams; ++p) if (id == kDefs[p].id) return p;
    return -1;
}

double get (const MasterSettings& s, int p)
{
    switch (p)
    {
        case pAir:       return s.eq.airDb;
        case pHighShelf: return s.eq.highShelfDb;
        case pBell2:     return s.eq.bell2Db;
        case pLowShelf:  return s.eq.lowShelfDb;
        case pTight:     return s.eq.tightener.enabled ? s.eq.tightener.amountDb : 0.0;   // off counts as no cut
        case pGlue:      return s.comp.glueDb;
        case pWidth:     return s.width.width;
        default:         return 0.0;
    }
}

void set (MasterSettings& s, int p, double v)
{
    const auto& d = def (p);
    v = juce::jlimit (d.lo, d.hi, v);
    switch (p)
    {
        case pAir:       s.eq.airDb = v; break;
        case pHighShelf: s.eq.highShelfDb = v; break;
        case pBell2:     s.eq.bell2Db = v; break;
        case pLowShelf:  s.eq.lowShelfDb = v; break;
        case pTight:     s.eq.tightener.enabled = v >= 0.05; if (v >= 0.05) s.eq.tightener.amountDb = v; break;
        case pGlue:      s.comp.glueDb = v; s.comp.autoThreshold = true; break;
        case pWidth:     s.width.width = v; break;
        default:         break;
    }
}

juce::String formatValue (int p, double v)
{
    switch (p)
    {
        case pWidth: return juce::String (juce::roundToInt (v * 100.0)) + " %";
        case pGlue:  return juce::String (v, 1) + " dB GR";
        case pTight: return v < 0.05 ? juce::String ("off") : juce::String (v, 1) + " dB cut";
        default:     return signedDb (v);
    }
}

//==============================================================================
const char* tagId (int t) { return kTagIds[juce::jlimit (0, kNumTags - 1, t)]; }
const char* tagText (int t) { return kTagText[juce::jlimit (0, kNumTags - 1, t)]; }

int tagFromId (const juce::String& id)
{
    for (int t = 0; t < kNumTags; ++t) if (id == kTagIds[t]) return t;
    return -1;
}

std::vector<Direction> tagDirections (int t)
{
    switch (t)
    {
        case tHarsh:       return { { pHighShelf, -1 }, { pAir, -1 } };
        case tNoAir:       return { { pAir, +1 } };
        case tLowLoose:    return { { pTight, +1 }, { pLowShelf, -1 } };
        case tVocalBuried: return { { pBell2, +1 } };
        case tSquashed:    return { { pGlue, -1 } };
        case tTooWide:     return { { pWidth, -1 } };
        default:           return {};
    }
}

juce::String describeTagRule (int t)
{
    juce::String s = juce::String (tagText (t)).toUpperCase() + ": ";
    const auto dirs = tagDirections (t);
    if (dirs.empty()) return s + "KEEPS THE SETTINGS USED";
    juce::StringArray parts;
    for (auto& d : dirs)
    {
        const auto& pd = def (d.param);
        const double step = d.sign * pd.step;
        parts.add (juce::String (pd.knob) + " " + (d.param == pWidth ? juce::String (juce::roundToInt (step * 100.0)) + " %" : signedDb (step)).toUpperCase());
    }
    return s + parts.joinIntoString (", ");
}

//==============================================================================
Features featuresOf (const DiagnosisMeasures& m)
{
    Features f;
    if (! m.valid) return f;
    const double raw[kNumFeatures] { m.plr, m.lra, m.lineSlopeDbPerOct, m.lowDb, m.airRelDb, m.harshDb, m.mudDb, m.sideMidDb, m.correlation };
    for (int i = 0; i < kNumFeatures; ++i)
    {
        const auto& d = kFeatures[i];
        const double v = juce::jlimit (d.lo, d.hi, finiteOr (raw[i], i == 7 ? d.lo : 0.0));
        f.v[(size_t) i] = v / d.step;
    }
    f.valid = true;
    return f;
}

double distance (const Features& a, const Features& b)
{
    if (! a.valid || ! b.valid) return std::numeric_limits<double>::infinity();
    double s = 0.0;
    for (int i = 0; i < kNumFeatures; ++i) { const double d = a.v[(size_t) i] - b.v[(size_t) i]; s += d * d; }
    return std::sqrt (s / kNumFeatures);
}

juce::String describe (const Features& f)
{
    if (! f.valid) return "NO MEASUREMENT";
    const auto r = rawFeatures (f);
    juce::StringArray w;
    const double plr = r[0];
    w.add ((plr < 9.0 ? "DENSE (PLR " : plr > 14.0 ? "DYNAMIC (PLR " : "PLR ") + juce::String (plr, 1) + (plr < 9.0 || plr > 14.0 ? " LU)" : " LU"));
    if (r[4] > -12.0) w.add ("BRIGHT TOP"); else if (r[4] < -20.0) w.add ("DARK TOP");
    if (r[3] > 3.0) w.add ("HEAVY LOWS"); else if (r[3] < -3.0) w.add ("LIGHT LOWS");
    if (r[5] > 3.0) w.add ("FORWARD UPPER MIDS");
    if (r[6] > 3.0) w.add ("THICK LOW MIDS");
    if (r[7] > -8.0) w.add ("WIDE"); else if (r[7] < -20.0) w.add ("NARROW");
    return w.joinIntoString (", ");
}

//==============================================================================
namespace
{
juce::var measuresToVar (const Context& c)
{
    auto* o = new juce::DynamicObject();
    if (c.input != nullptr)
    {
        const auto& in = *c.input;
        o->setProperty ("integrated", num (in.integrated));
        o->setProperty ("shortTermMax", num (in.shortTermMax));
        o->setProperty ("lra", num (in.lra));
        o->setProperty ("truePeak", num (in.truePeak));
        o->setProperty ("samplePeak", num (in.samplePeak));
        o->setProperty ("rms", num (in.rms));
        o->setProperty ("crest", num (in.crest));
        o->setProperty ("correlation", num (in.correlation));
    }
    if (c.measures != nullptr && c.measures->valid)
    {
        const auto& m = *c.measures;
        o->setProperty ("plr", num (m.plr));
        o->setProperty ("lowCorrelation", num (m.lowCorrelation));
        o->setProperty ("sideMidDb", num (m.sideMidDb));
        o->setProperty ("sideMidHighDb", num (m.sideMidHighDb));
        o->setProperty ("dense", m.dense);
        auto* feat = new juce::DynamicObject();
        const auto f = featuresOf (m);
        const auto r = rawFeatures (f);
        for (int i = 0; i < kNumFeatures; ++i) feat->setProperty (kFeatures[i].name, r[(size_t) i]);
        o->setProperty ("features", juce::var (feat));
        if (! m.spectrumDb.empty())
        {
            auto* bands = new juce::DynamicObject();
            const std::pair<const char*, std::pair<double, double>> b[] {
                { "sub25_60", { 25.0, 60.0 } }, { "low60_150", { 60.0, 150.0 } }, { "lowMid150_500", { 150.0, 500.0 } }, { "mid500_2k", { 500.0, 2000.0 } },
                { "presence2k_5k", { 2000.0, 5000.0 } }, { "brilliance5k_10k", { 5000.0, 10000.0 } }, { "air10k_16k", { 10000.0, 16000.0 } } };
            for (auto& x : b) bands->setProperty (x.first, num (meanInRange (m.spectrumDb, x.second.first, x.second.second)));
            o->setProperty ("spectrumBandsDb", juce::var (bands));
        }
    }
    if (c.findings != nullptr)
    {
        juce::Array<juce::var> list;
        for (const auto& f : *c.findings)
        {
            auto* x = new juce::DynamicObject();
            x->setProperty ("id", f.id);
            x->setProperty ("severity", f.severity == Severity::problem ? "problem" : f.severity == Severity::warn ? "warn" : "info");
            x->setProperty ("value", num (f.value));
            list.add (juce::var (x));
        }
        o->setProperty ("findings", list);
    }
    return juce::var (o);
}

juce::var resultToVar (const Context& c)
{
    auto* o = new juce::DynamicObject();
    if (c.output != nullptr && c.output->valid)
    {
        const auto& m = *c.output;
        o->setProperty ("integrated", num (m.integrated));
        o->setProperty ("shortTermMax", num (m.shortTermMax));
        o->setProperty ("lra", num (m.lra));
        o->setProperty ("truePeak", num (m.truePeak));
        o->setProperty ("samplePeak", num (m.samplePeak));
        o->setProperty ("crest", num (m.crest));
        o->setProperty ("plr", num (m.truePeak - m.integrated));
        o->setProperty ("correlation", num (m.correlation));
    }
    if (c.limiter != nullptr)
    {
        o->setProperty ("limiterGrMaxDb", num (c.limiter->maxDb));
        o->setProperty ("limiterActive", num (c.limiter->activeFraction));
    }
    return juce::var (o);
}

Features featuresFromVar (const juce::var& input)
{
    Features f;
    const auto feat = input["features"];
    if (! feat.isObject()) return f;
    for (int i = 0; i < kNumFeatures; ++i)
    {
        const auto v = feat[kFeatures[i].name];
        if (v.isVoid()) return f;
        f.v[(size_t) i] = juce::jlimit (kFeatures[i].lo, kFeatures[i].hi, (double) v) / kFeatures[i].step;
    }
    f.valid = true;
    return f;
}
} // namespace

Note makeNote (const juce::StringArray& tags, const juce::String& text, const Context& c, juce::Time when)
{
    Note n;
    n.id = juce::Uuid().toDashedString();
    n.time = when;
    n.source = c.source;
    n.track = c.track;
    for (auto& t : tags) if (tagFromId (t) >= 0) n.tags.addIfNotAlreadyThere (t);
    n.text = text.trim().substring (0, 2000);
    n.startingPoint = c.startingPoint;
    n.scenarios = c.scenarios;
    n.input = measuresToVar (c);
    n.features = c.measures != nullptr ? featuresOf (*c.measures) : Features();
    if (c.settings != nullptr) { n.settings = *c.settings; n.settingsVar = settingsToVar (*c.settings); }
    n.result = resultToVar (c);
    n.overrides = c.overrides;
    return n;
}

juce::var toVar (const Note& n)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("format", 1);
    o->setProperty ("id", n.id);
    o->setProperty ("time", (juce::int64) n.time.toMilliseconds());
    o->setProperty ("when", n.time.toISO8601 (true));
    o->setProperty ("source", n.source);
    o->setProperty ("track", n.track);
    juce::Array<juce::var> tags;
    for (auto& t : n.tags) tags.add (t);
    o->setProperty ("tags", tags);
    o->setProperty ("text", n.text);
    o->setProperty ("startingPoint", n.startingPoint);
    juce::Array<juce::var> sc;
    for (auto& s : n.scenarios) sc.add (s);
    o->setProperty ("scenarios", sc);
    o->setProperty ("input", n.input);
    o->setProperty ("settings", n.settingsVar);
    o->setProperty ("result", n.result);
    juce::Array<juce::var> ov;
    for (auto& x : n.overrides)
    {
        auto* v = new juce::DynamicObject();
        v->setProperty ("knob", def (x.param).id);
        v->setProperty ("locked", x.from);
        v->setProperty ("set", x.to);
        ov.add (juce::var (v));
    }
    o->setProperty ("overrides", ov);
    juce::Array<juce::var> ig;
    for (auto& s : n.ignore) ig.add (s);
    o->setProperty ("ignore", ig);
    return juce::var (o);
}

bool fromVar (const juce::var& v, Note& n)
{
    if (! v.isObject() || v["id"].toString().isEmpty() || ! v["settings"].isObject()) return false;
    n = Note();
    n.id = v["id"].toString();
    n.time = juce::Time ((juce::int64) v.getProperty ("time", 0));
    n.source = v["source"].toString();
    n.track = v["track"].toString();
    if (auto* a = v["tags"].getArray()) for (auto& t : *a) if (tagFromId (t.toString()) >= 0) n.tags.addIfNotAlreadyThere (t.toString());
    n.text = v["text"].toString();
    n.startingPoint = v["startingPoint"].toString();
    if (auto* a = v["scenarios"].getArray()) for (auto& s : *a) n.scenarios.add (s.toString());
    n.input = v["input"];
    n.features = featuresFromVar (n.input);
    n.settingsVar = v["settings"];
    settingsFromVar (n.settingsVar, n.settings);
    n.result = v["result"];
    if (auto* a = v["overrides"].getArray())
        for (auto& x : *a)
        {
            const int p = paramFromId (x["knob"].toString());
            if (p >= 0) n.overrides.push_back ({ p, (double) x.getProperty ("locked", 0.0), (double) x.getProperty ("set", 0.0) });
        }
    if (auto* a = v["ignore"].getArray()) for (auto& s : *a) n.ignore.add (s.toString());
    return true;
}

//==============================================================================
Learned learn (const std::vector<Note>& notes)
{
    Learned L;
    std::vector<int> order;
    for (int i = 0; i < (int) notes.size(); ++i) if (notes[(size_t) i].features.valid) order.push_back (i);
    std::stable_sort (order.begin(), order.end(), [&] (int a, int b) { return notes[(size_t) a].time < notes[(size_t) b].time; });

    // Groups of similar tracks, in the order the notes were written (appending a note never regroups older ones).
    for (int i : order)
    {
        const auto& f = notes[(size_t) i].features;
        int best = -1;
        double bestD = kSimilar;
        for (int g = 0; g < (int) L.groups.size(); ++g)
        {
            const double d = distance (f, L.groups[(size_t) g].centre);
            if (d <= bestD) { bestD = d; best = g; }
        }
        if (best < 0)
        {
            Group g;
            g.centre = f;
            g.key = notes[(size_t) i].id;
            g.notes.push_back (i);
            L.groups.push_back (g);
        }
        else
        {
            auto& g = L.groups[(size_t) best];
            g.notes.push_back (i);
            const double k = (double) g.notes.size();
            for (int j = 0; j < kNumFeatures; ++j) g.centre.v[(size_t) j] += (f.v[(size_t) j] - g.centre.v[(size_t) j]) / k;
        }
    }

    const MasterSettings factory;
    for (int gi = 0; gi < (int) L.groups.size(); ++gi)
    {
        auto& g = L.groups[(size_t) gi];
        g.text = describe (g.centre);
        const int count = (int) g.notes.size();
        for (int p = 0; p < kNumParams; ++p)
        {
            const auto& pd = def (p);
            double sw = 0.0, swv = 0.0, swv2 = 0.0, wUp = 0.0, wDown = 0.0;
            int evidence = 0, up = 0, down = 0, accepted = 0, overridden = 0;
            juce::Time updated;
            for (int k = 0; k < count; ++k)
            {
                const auto& n = notes[(size_t) g.notes[(size_t) k]];
                if (n.ignore.contains (pd.id)) continue;
                const double recency = std::pow (0.85, (double) (count - 1 - k));
                const double used = get (n.settings, p);
                int dir = 0;
                for (auto& t : n.tags)
                    for (auto& d : tagDirections (tagFromId (t)))
                        if (d.param == p) dir += d.sign;
                const Override* ov = nullptr;
                for (auto& o : n.overrides) if (o.param == p && std::abs (o.to - o.from) > 1.0e-6) ov = &o;
                double value = 0.0, weight = 0.0;
                if (ov != nullptr)          { value = used; weight = 1.5; ++overridden; }
                else if (dir != 0)          { value = used + (dir > 0 ? 1.0 : -1.0) * pd.step; weight = 1.0; }
                else if (n.tags.contains (tagId (tPerfect))) { value = used; weight = 1.0; ++accepted; }
                else continue;
                value = juce::jlimit (pd.lo, pd.hi, value);
                const double w = recency * weight;
                if (dir > 0) { wUp += w; ++up; }
                if (dir < 0) { wDown += w; ++down; }
                sw += w; swv += w * value; swv2 += w * value * value;
                ++evidence;
                if (n.time > updated) updated = n.time;
            }
            if (evidence == 0) continue;
            const double mean = swv / sw;
            const double spread = std::sqrt (juce::jmax (0.0, swv2 / sw - mean * mean));
            const double value = roundTo (mean, pd.resolution);
            // A rule built only on "perfect" notes at the factory value teaches nothing.
            // Acceptance alone is listed only once it could act (otherwise every "perfect" note would list every knob).
            if (up + down + overridden == 0 && (std::abs (value - get (factory, p)) < pd.minDiff || evidence < kMinEvidence)) continue;
            Rule r;
            r.group = gi;
            r.param = p;
            r.value = value;
            r.spread = spread;
            r.evidence = evidence;
            r.towards = up;
            r.against = down;
            r.overrides = overridden;
            r.updated = updated;
            r.key = g.key + ":" + pd.id;
            const double tagW = wUp + wDown;
            const bool conflict = tagW > 0.0 && juce::jmin (wUp, wDown) / tagW > 0.30;
            juce::StringArray parts;
            if (up > 0) parts.add (juce::String (up) + " asked for more");
            if (down > 0) parts.add (juce::String (down) + " for less");
            if (accepted > 0) parts.add (juce::String (accepted) + " accepted it");
            if (overridden > 0) parts.add (juce::String (overridden) + " set it by hand");
            const juce::String evidenceText = juce::String (evidence) + (evidence == 1 ? " note" : " notes") + " (" + parts.joinIntoString (", ") + ")";
            if (evidence < kMinEvidence)
            {
                r.state = Rule::State::collecting;
                r.why = evidenceText + "; needs " + juce::String (kMinEvidence - evidence) + " more before it acts";
            }
            else if (conflict)
            {
                r.state = Rule::State::mixed;
                r.why = evidenceText + "; the notes point both ways, so nothing is locked";
            }
            else if (spread > pd.tolerance)
            {
                r.state = Rule::State::mixed;
                r.why = evidenceText + "; spread " + juce::String (spread, 2) + " is over " + juce::String (pd.tolerance, 2) + ", so nothing is locked";
            }
            else
            {
                r.state = Rule::State::acting;
                r.why = evidenceText + "; weighted average, spread " + juce::String (spread, 2);
            }
            L.rules.push_back (r);
        }
    }
    return L;
}

int nearestGroup (const Learned& L, const Features& track, double* dist)
{
    int best = -1;
    double bestD = kSimilar;
    for (int g = 0; g < (int) L.groups.size(); ++g)
    {
        const double d = distance (track, L.groups[(size_t) g].centre);
        if (d <= bestD) { bestD = d; best = g; }
    }
    if (dist != nullptr) *dist = best >= 0 ? bestD : std::numeric_limits<double>::infinity();
    return best;
}

std::vector<Proposal> propose (const Learned& L, const Features& track, const MasterSettings& current)
{
    std::vector<Proposal> out;
    const int g = nearestGroup (L, track);
    if (g < 0) return out;
    for (const auto& r : L.rules)
    {
        if (r.group != g || r.state != Rule::State::acting) continue;
        if (r.towards + r.against + r.overrides == 0 && std::abs (r.value - get (current, r.param)) < def (r.param).minDiff) continue;
        Proposal p;
        p.param = r.param;
        p.value = r.value;
        p.evidence = r.evidence;
        p.ruleKey = r.key;
        p.reason = "Locked from " + juce::String (r.evidence) + " of your notes on similar tracks: " + def (r.param).knob + " " + formatValue (r.param, r.value);
        out.push_back (p);
    }
    return out;
}

//==============================================================================
Store::Store() : dir (defaultFolder()) {}

juce::File Store::defaultFolder()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("SubverseLab/MIHENK/Profile");
}

void Store::setFolder (const juce::File& f)
{
    dir = f;
    loaded = false;
    list.clear();
    model = {};
}

void Store::refresh()
{
    const auto f = notesFile();
    const auto t = f.getLastModificationTime();
    const auto size = f.existsAsFile() ? f.getSize() : (juce::int64) -1;
    if (! loaded || t != loadedTime || size != loadedSize) load();
}

void Store::load()
{
    list.clear();
    const auto f = notesFile();
    if (f.existsAsFile())
    {
        juce::StringArray lines;
        f.readLines (lines);
        for (auto& line : lines)
        {
            if (line.trim().isEmpty()) continue;
            Note n;
            if (fromVar (juce::JSON::parse (line), n)) list.push_back (std::move (n));
        }
    }
    model = learn (list);
    loadedTime = f.getLastModificationTime();
    loadedSize = f.existsAsFile() ? f.getSize() : -1;
    loaded = true;
}

bool Store::add (const Note& n, juce::String& error)
{
    if (! dir.createDirectory()) { error = "Cannot create " + dir.getFullPathName(); return false; }
    juce::FileOutputStream out (notesFile());
    if (! out.openedOk()) { error = "Cannot write " + notesFile().getFullPathName(); return false; }
    out.setPosition (out.getFile().getSize());
    out.writeText (juce::JSON::toString (toVar (n), true) + "\n", false, false, "\n");
    out.flush();
    if (out.getStatus().failed()) { error = out.getStatus().getErrorMessage(); return false; }
    load();
    return true;
}

bool Store::rewrite (juce::String& error)
{
    if (! dir.createDirectory()) { error = "Cannot create " + dir.getFullPathName(); return false; }
    juce::String text;
    for (auto& n : list) text << juce::JSON::toString (toVar (n), true) << "\n";
    if (! notesFile().replaceWithText (text, false, false, "\n")) { error = "Cannot write " + notesFile().getFullPathName(); return false; }
    load();
    return true;
}

bool Store::deleteRule (const juce::String& ruleKey, juce::String& error)
{
    refresh();
    for (const auto& r : model.rules)
    {
        if (r.key != ruleKey) continue;
        const auto& g = model.groups[(size_t) r.group];
        for (int i : g.notes) list[(size_t) i].ignore.addIfNotAlreadyThere (def (r.param).id);
        return rewrite (error);
    }
    error = "No such rule";
    return false;
}

bool Store::resetAll (juce::String& error)
{
    const auto f = notesFile();
    if (f.existsAsFile())
    {
        const auto aside = dir.getChildFile ("notes-reset-" + juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S") + ".jsonl").getNonexistentSibling();
        if (! f.moveFileTo (aside)) { error = "Cannot move " + f.getFullPathName(); return false; }
    }
    list.clear();
    model = {};
    loadedTime = {};
    loadedSize = -1;
    loaded = true;
    return true;
}

bool Store::exportTo (const juce::File& f, juce::String& error)
{
    refresh();
    auto* o = new juce::DynamicObject();
    o->setProperty ("format", "MIHENK personal profile");
    o->setProperty ("version", 1);
    o->setProperty ("exported", juce::Time::getCurrentTime().toISO8601 (true));
    juce::Array<juce::var> arr;
    for (auto& n : list) arr.add (toVar (n));
    o->setProperty ("notes", arr);
    if (! f.replaceWithText (juce::JSON::toString (juce::var (o), false), false, false, "\n")) { error = "Cannot write " + f.getFullPathName(); return false; }
    return true;
}

int Store::importFrom (const juce::File& f, juce::String& error)
{
    if (! f.existsAsFile()) { error = "No such file"; return -1; }
    refresh();
    std::vector<Note> incoming;
    const auto text = f.loadFileAsString();
    const auto whole = juce::JSON::parse (text);
    if (auto* arr = whole["notes"].getArray())
    {
        for (auto& v : *arr) { Note n; if (fromVar (v, n)) incoming.push_back (std::move (n)); }
    }
    else
    {
        juce::StringArray lines;
        lines.addLines (text);
        for (auto& line : lines) { Note n; if (fromVar (juce::JSON::parse (line), n)) incoming.push_back (std::move (n)); }
    }
    if (incoming.empty()) { error = "No MIHENK profile notes in " + f.getFileName(); return -1; }
    int added = 0;
    for (auto& n : incoming)
    {
        bool dup = false;
        for (auto& e : list) dup |= e.id == n.id;
        if (! dup) { list.push_back (std::move (n)); ++added; }
    }
    std::stable_sort (list.begin(), list.end(), [] (const Note& a, const Note& b) { return a.time < b.time; });
    if (added > 0 && ! rewrite (error)) return -1;
    return added;
}
} // namespace mihenk::profile
