// The JING FANG films, rendered from the plug-in itself: the ads (owner 18 Oct: "2 dakikalık 3 bölüm reklam çekelim") and the
// short narrated explainers ("birkaç tane kısa anlatımlı video"). No screen capture: the processor and the editor run here, a
// timeline plays notes and works the panel, every frame goes to ffmpeg and the audio to a WAV that is muxed in afterwards.
//
// The player is also a tester (owner 18 Oct: "reklam çekerken koşturduğun cihazı çalan ajan mantık hatalarını bulup
// raporlamalı"). On every frame it checks what must always hold (audio finite and under 0 dBFS, no note that stays silent,
// no voice or key light left on after the keys are up, one door at a time, the host values equal what the engine plays, the
// knobs' drawings settle, the panel's step LEDs follow the engine) and the script's own expectations at their times (a preset
// loaded, a door open, PLAY running, a save landing where the open door says). Everything found goes to <out>/<film>_report.md.
//
//   demo_render <1|2|3> <out_dir> [--vertical]   the ads: out_dir/film<N>_video.mp4 + film<N>.wav (film<N>_9x16_* with --vertical)
//   demo_render e<N> <out_dir>                   an explainer (9:16, e1..e8), timed to its narration: marketing/explainers/script.json
//                                                and out/explainers/voice/voice.json (tools/jf_voice.py)
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "GenPanel.h"
#include <cstdio>
#include <map>

namespace
{
constexpr int kFps = 30, kSr = 48000, kSpf = kSr / kFps;
int W = 1920, H = 1080;
bool vertical = false;
bool wideExplainer = false;                                      // a narrated film in 16:9 (YouTube long form): camera + caption band
int introMode = 0;                                               // 1 wide 1600 x 900, 2 tall 720 x 1280 (the site's opening film),
                                                                 // 3 face: the device body's face at 200 %, no dragons (the 3D intro's texture)
juce::Rectangle<int> faceRect, stripRect;                         // window units: the device body (red sides included), the preset strip
const juce::Colour kGround (0xff033832), kCream (0xffF2EFE7), kGold (0xffC5A059);   // the brand kit (store images: the panel on #033832)

using Rect = juce::Rectangle<float>;
struct Ev { double t; std::function<void()> f; };
struct Note { double t, len; int key, vel; };
struct Caption { double t0, t1; juce::String text, sub; int number = 0; };
struct Expect { double t; juce::String what; std::function<juce::String()> check; };   // "" = holds, "(..." = not judged, else what was found
struct CamKey { double t; std::function<Rect()> rect; };

struct Film
{
    juce::String name, stem;
    double length = 120;
    bool explainer = false;
    std::vector<Ev> evs;
    std::vector<Note> notes;
    std::vector<Caption> caps;
    std::vector<Expect> expects;
    std::vector<CamKey> cams;
    std::vector<double> L, E;                                     // explainers: line n (1-based) starts at L[n], ends at E[n]
    void at (double t, std::function<void()> f) { evs.push_back ({ t, std::move (f) }); }
    void note (double t, double len, int key, int vel = 100) { notes.push_back ({ t, len, key, vel }); }
    void chord (double t, double len, std::initializer_list<int> keys, int vel = 92) { for (int k : keys) note (t, len, k, vel); }
    void cap (double t0, double t1, juce::String text, juce::String sub = {}) { caps.push_back ({ t0, t1, std::move (text), std::move (sub) }); }
    void expect (double t, juce::String what, std::function<juce::String()> f) { expects.push_back ({ t, std::move (what), std::move (f) }); }
    void cam (double t, std::function<Rect()> r) { cams.push_back ({ t, std::move (r) }); }
};

int presetIndex (JingFangProcessor& p, const juce::String& fam, const juce::String& name)
{
    for (size_t i = 0; i < p.presets().size(); ++i) if (p.presets()[i].family == fam && p.presets()[i].name == name) return (int) i;
    std::fprintf (stderr, "no preset %s / %s\n", fam.toRawUTF8(), name.toRawUTF8());
    return 0;
}

void setP (JingFangProcessor& p, const juce::String& id, float plain)
{
    if (auto* hp = p.apvts.getParameter (id)) jfui::setParamGesture (hp, plain);
}
float raw (JingFangProcessor& p, const juce::String& id) { auto* v = p.apvts.getRawParameterValue (id); return v != nullptr ? v->load() : -999.0f; }
juce::String num (double v, int d = 2) { return juce::String (v, d); }

// a real click on a component (a PanelKey checks contains(), which needs a window: those are pressed through onClick)
void click (juce::Component& c, juce::Point<int> at = { -1, -1 })
{
    if (at.x < 0) at = c.getLocalBounds().getCentre();
    auto src = juce::Desktop::getInstance().getMainMouseSource();
    const auto now = juce::Time::getCurrentTime();
    const juce::ModifierKeys mods (juce::ModifierKeys::leftButtonModifier);
    c.mouseDown (juce::MouseEvent (src, at.toFloat(), mods, juce::MouseInputSource::defaultPressure, 0, 0, 0, 0, &c, &c, now, at.toFloat(), now, 1, false));
    c.mouseUp (juce::MouseEvent (src, at.toFloat(), mods.withoutMouseButtons(), juce::MouseInputSource::defaultPressure, 0, 0, 0, 0, &c, &c, now, at.toFloat(), now, 1, false));
}
template <typename T> T* findIn (juce::Component* root, const std::function<bool (T&)>& ok)
{
    if (root == nullptr) return nullptr;
    for (auto* c : root->getChildren())
    {
        if (auto* t = dynamic_cast<T*> (c)) if (ok (*t)) return t;
        if (auto* r = findIn<T> (c, ok)) return r;
    }
    return nullptr;
}
jfui::StepKey* stepKey (juce::Component* door, const juce::String& id) { return findIn<jfui::StepKey> (door, [&] (jfui::StepKey& k) { return k.paramId() == id; }); }
jfui::PanelKey* doorKey (juce::Component* door, const juce::String& text) { return findIn<jfui::PanelKey> (door, [&] (jfui::PanelKey& k) { return k.text == text; }); }
Rect inEditor (JingFangEditor& ed, juce::Component* c) { return c == nullptr ? Rect() : ed.getLocalArea (c, c->getLocalBounds()).toFloat(); }
Rect whole() { return Rect (0, 0, (float) jfui::kW, (float) jfui::kH); }
Rect section (JingFangEditor& ed, const juce::String& name)
{
    for (auto& [n, r] : ed.sectionsForTest()) if (n == name) return r.toFloat();
    return whole();
}
float voicePitchFor (JingFangProcessor& p, int key)                 // the pitch of the gated voice nearest to this key (-1: none)
{
    float best = -1, dist = 1e9f;
    for (int i = 0; i < svl::kVoicePool; ++i)
    {
        const auto v = p.engineForTest().voiceInfo (i);
        if (v.active && v.gate && std::abs (v.pitch - (float) key) < dist) { dist = std::abs (v.pitch - (float) key); best = v.pitch; }
    }
    return best;
}
bool drumsRunning (JingFangProcessor& p) { auto* d = p.engineForTest().drumMachine(); return d != nullptr && d->isRunning(); }

// ---------------------------------------------------------------- the three ads
void film1 (Film& f, JingFangProcessor& p, JingFangEditor& ed)
{
    f.name = "The instrument";
    auto load = [&p] (const char* fam, const char* name) { return [&p, fam, name] { p.loadPresetIndex (presetIndex (p, fam, name)); }; };
    f.cap (4, 15, "JING FANG", "A synthesizer with three oscillators, a filter, two envelopes, FM and a drum mode");
    f.at (4, load ("Pad", "Warm Pad"));
    for (int i = 0; i < 3; ++i) f.chord (4.5 + i * 3.4, 3.0, { 48 + i * 2, 55 + i * 2, 60 + i * 2, 64 + i * 2 });
    f.at (14.8, load ("Keys - E-Piano", "Tine EP"));
    f.cap (15, 34, "The preset synthesizer", "Every panel takes its settings from its own preset slot");
    const int prog[4][4] = { { 50, 57, 60, 65 }, { 48, 55, 59, 64 }, { 45, 52, 57, 60 }, { 47, 53, 57, 62 } };
    for (int bar = 0; bar < 8; ++bar)
    {
        const auto& c = prog[bar % 4];
        f.chord (15.2 + bar * 2.3, 2.0, { c[0], c[1], c[2], c[3] });
    }
    f.at (20, [&p] { p.loadSection (0, presetIndex (p, "Lead", "Supersaw Lead")); });
    f.at (24, [&p] { p.loadSection (4, presetIndex (p, "Pad", "Slow Pad")); });
    f.at (28, [&p] { p.loadSection (3, presetIndex (p, "Bass", "Acid Line")); });
    f.cap (34, 52, "Take the oscillators from one sound, the envelope from another", "and save the mix as your own preset");
    f.at (34, [&p] { p.loadSection (1, presetIndex (p, "Bell", "FM Bell")); });
    f.at (40, [&p] { p.loadSection (11, presetIndex (p, "Pad", "Air Pad")); });
    for (int bar = 0; bar < 8; ++bar) { const auto& c = prog[(bar + 1) % 4]; f.chord (34.2 + bar * 2.2, 1.9, { c[0], c[1], c[2], c[3] }); }
    f.cap (52, 70, "Drive, echo and reverb on every sound");
    f.at (52, load ("Pluck", "Glass Pluck"));
    const int mel[] = { 72, 76, 79, 83, 81, 79, 76, 74, 72, 76, 79, 84, 83, 79, 76, 72 };
    for (int i = 0; i < 32; ++i) f.note (52.3 + i * 0.5, 0.35, mel[i % 16]);
    f.at (55, [&p] { setP (p, "fx.echo.mix", 0.45f); setP (p, "fx.echo.feedback", 0.6f); });
    f.at (60, [&p] { setP (p, "fx.reverb.mix", 0.5f); setP (p, "fx.reverb.size", 0.85f); });
    f.at (65, [&p] { setP (p, "fx.drive.amount", 0.55f); });
    f.cap (70, 90, "The patch bay", "Three LFOs and sixteen cables: drag from a source to a destination");
    f.at (70, load ("Lead", "VA Lead"));
    f.at (71, [&ed] { ed.patchKey().onClick(); });
    f.at (73, [&ed] { ed.patchBay().connect (1, 11); });   // LFO 1 -> CUTOFF
    f.at (75, [&p] { setP (p, "mod1.amount", 0.7f); setP (p, "lfo1.rate", 3.0f); });
    f.at (78, [&ed] { ed.patchBay().connect (2, 1); });    // LFO 2 -> PITCH
    f.at (80, [&p] { setP (p, "mod2.amount", 0.12f); });
    for (int i = 0; i < 9; ++i) f.chord (71.5 + i * 2.0, 1.8, { 57 + (i % 3) * 2, 64 + (i % 3) * 2 });
    f.at (88.5, [&ed] { ed.closeKey().onClick(); });
    f.cap (90, 110, "Micro mode", "Any note can sit a step of the 53-step grid higher or lower, while you play");
    f.at (90, load ("Strings", "Legato Strings"));
    f.at (91, [&p] { setP (p, "tuning.mode", 1.0f); });
    f.at (93, [&p] { setP (p, "tuning.row.e", 0.0f); setP (p, "tuning.row.b", 0.0f); });
    f.at (97, [&p] { setP (p, "tuning.row.gb", 2.0f); });
    const int mak[] = { 62, 64, 65, 67, 69, 71, 72, 74, 72, 71, 69, 67, 65, 64, 62, 64 };
    for (int i = 0; i < 32; ++i) f.note (91.5 + i * 0.55, 0.5, mak[i % 16], 96);
    f.at (108.8, [&p] { setP (p, "tuning.mode", 0.0f); });
}

void film2 (Film& f, JingFangProcessor& p, JingFangEditor& ed)
{
    f.name = "Patterns";
    auto load = [&p] (const char* fam, const char* name) { return [&p, fam, name] { p.loadPresetIndex (presetIndex (p, fam, name)); }; };
    f.cap (4, 22, "One key, a whole pattern", "Every arp preset plays a chord shape from a single key");
    f.at (4, load ("Arp - Classic", "Up Down Two Octaves"));
    for (int i = 0; i < 4; ++i) f.note (4.5 + i * 4.3, 4.0, 48 + (i == 1 ? 5 : i == 2 ? -3 : i == 3 ? 7 : 0));
    f.at (13, load ("Arp - Chord", "Swung Chords"));
    f.cap (22, 46, "The sequencer", "Sixteen steps: a key, three knobs and three switches on every step");
    f.at (22, load ("Sequence - Acid", "Climbing Acid"));
    f.at (23, [&ed] { ed.seqKey().onClick(); });
    f.note (24, 9.5, 45);
    f.at (28, [&p] { setP (p, "seq.s5.note", 7.0f); setP (p, "seq.s9.accent", 1.0f); });
    f.at (31, [&p] { setP (p, "seq.s13.slide", 1.0f); setP (p, "seq.s12.note", 12.0f); });
    f.note (34, 10, 48);
    f.cap (46, 64, "Press PLAY and go back to the panel", "The pattern keeps running while you play on top");
    f.at (46, [&ed] { if (auto* k = doorKey (ed.seqDoorView(), "PLAY")) k->onClick(); });
    f.at (49, [&ed] { ed.closeKey().onClick(); });
    for (int i = 0; i < 6; ++i) f.chord (50 + i * 2.2, 1.6, { 72 + (i % 2) * 3, 79 });
    f.at (63.5, [&ed] { ed.keyPressed (juce::KeyPress (juce::KeyPress::escapeKey)); });
    f.cap (64, 100, "The drum machine", "Eight tracks, sixteen steps, a sound on every track. Hold a key, or press PLAY");
    f.at (64, load ("Drum Machine", "Boom Bap"));
    f.at (65, [&ed] { ed.drumKey().onClick(); });
    f.note (66, 9, 60);
    f.at (76, load ("Drum Machine", "Four Floor House"));
    f.note (77, 9, 60);
    f.at (82, [&p] { setP (p, "drums.t3.s7", 2.0f); setP (p, "drums.t4.s11", 1.0f); });
    f.at (87, load ("Drum Machine", "Nine Eight Groove"));
    f.note (88, 11, 60);
    f.at (93, [&p] { setP (p, "drums.octave", 1.0f); });
    f.cap (100, 112, "Tempo, octave, swing", "The whole kit moves in octaves; the tempo follows your host");
    f.at (100, [&p] { setP (p, "drums.octave", 0.0f); p.setUserBpm (96.0f); });
    f.note (100.5, 11, 60);
    f.at (111, [&ed] { ed.closeKey().onClick(); });
}

void film3 (Film& f, JingFangProcessor& p, JingFangEditor& ed)
{
    f.name = "Tuning Lab";
    auto load = [&p] (const char* fam, const char* name) { return [&p, fam, name] { p.loadPresetIndex (presetIndex (p, fam, name)); }; };
    f.cap (4, 22, "Between the notes", "Three rows of keys on a 53-step grid: the middle row is the usual keys");
    f.at (4, load ("Pluck", "Concert Harp"));
    f.at (5, [&p] { setP (p, "tuning.mode", 1.0f); setP (p, "tuning.row.eb", 0.0f); setP (p, "tuning.row.gb", 0.0f); });
    const int line[] = { 62, 63, 66, 67, 69, 70, 72, 74, 72, 70, 69, 67, 66, 63, 62, 62 };
    for (int i = 0; i < 30; ++i) f.note (5.5 + i * 0.55, 0.5, line[i % 16], 96);
    f.cap (22, 46, "The Tuning Lab", "Learn a tuning from a recording of a solo line: its steps, measured");
    f.at (22, [&p] { setP (p, "tuning.mode", 0.0f); });
    f.at (23, [&ed] { ed.labKey().onClick(); });
    f.at (24, [&p] {
        JingFangProcessor::LabState ls; ls.on = true;
        const auto scl = juce::File::getCurrentWorkingDirectory().getChildFile ("out/tuning/hicaz_lead_learned.scl");
        jflab::scala::readScl (scl.loadFileAsString().toStdString(), ls.a);
        ls.a.name = "Learned: solo line"; ls.a.middleKey = ls.a.refKey = 62; ls.a.refHz = 293.6648;
        ls.a.confidence = { 1, 0.53f, 1, 0.32f, 1, 0.55f, 0.48f };
        p.setLabState (ls);
    });
    static const int deg[] = { 0, 1, 2, 3, 4, 5, 6, 7, 6, 5, 4, 3, 2, 1 };
    for (int i = 0; i < 28; ++i) f.note (25 + i * 0.6, 0.55, 62 + deg[i % 14], 96);
    f.cap (46, 66, "Climbing and falling steps", "A step can sit lower when the melody comes down onto it");
    f.at (46, [&p] {
        auto ls = p.labState(); jflab::Tuning d; d.name = "E falls by a comma"; d.fall = d.cents; d.fall[3] = 378; ls.a = d; ls.directional = true; p.setLabState (ls);
    });
    const int dirLine[] = { 60, 62, 64, 65, 67, 65, 64, 62, 60, 64, 67, 72, 67, 64, 62, 60 };
    for (int i = 0; i < 32; ++i) f.note (46.5 + i * 0.6, 0.55, dirLine[i % 16], 96);
    f.cap (66, 86, "Morph", "Every key moves from tuning A to tuning B with one knob");
    f.at (66, [&p] {
        auto ls = p.labState(); jflab::Tuning e24; e24.name = "24 equal"; e24.cents.clear(); for (int i = 1; i <= 24; ++i) e24.cents.push_back (50.0 * i);
        ls.a = jflab::Tuning(); ls.b = e24; ls.morph = 0; p.setLabState (ls);
    });
    for (int k = 0; k < 20; ++k) f.at (68 + k * 0.8, [&p, k] { auto ls = p.labState(); ls.morph = (float) (k + 1) / 20.0f; p.setLabState (ls); });
    for (int i = 0; i < 16; ++i) f.chord (66.5 + i * 1.2, 1.1, { 60, 64, 67 }, 80);
    f.cap (86, 110, "What sounds", "Every interval you hold: its cents, the nearest ratio, how far off, how fast it beats");
    f.at (86, [&p] { auto ls = p.labState(); ls.a = jflab::Tuning(); ls.b = jflab::Tuning(); ls.morph = 0; ls.adaptive = true; p.setLabState (ls); });
    f.chord (87, 5, { 48, 55, 60, 64, 67 }, 84);
    f.chord (93, 5, { 50, 57, 62, 65, 69 }, 84);
    f.chord (99, 5, { 45, 52, 57, 60, 64 }, 84);
    f.chord (105, 4.5, { 47, 53, 57, 62 }, 84);
    f.at (110, [&ed] { ed.closeKey().onClick(); });
}

// ---------------------------------------------------------------- the explainers (9:16, timed to the narration)
using Check = std::function<juce::String()>;
Check presetIs (JingFangProcessor& p, juce::String fam, juce::String name)
{
    return [&p, fam, name] { return p.currentName() == name && p.currentFamily() == fam ? juce::String() : "loaded is " + p.currentFamily() + " / " + p.currentName(); };
}
Check doorOpen (JingFangEditor& ed, juce::String which)
{
    return [&ed, which]
    {
        const bool patch = ed.patchBay().isOpenOrOpening(), seq = ed.seqDoorView()->isOpenOrOpening(),
                   drum = ed.drumDoorView()->isOpenOrOpening(), lab = ed.labDoorView()->isOpenOrOpening();
        const bool want = which == "PATCH" ? patch : which == "SEQ" ? seq : which == "DRUM" ? drum : which == "LAB" ? lab : ! (patch || seq || drum || lab);
        return want ? juce::String() : "doors: PATCH " + juce::String ((int) patch) + " SEQ " + juce::String ((int) seq) + " DRUM " + juce::String ((int) drum) + " LAB " + juce::String ((int) lab);
    };
}
Check paramIs (JingFangProcessor& p, juce::String id, float v)
{
    return [&p, id, v] { const float r = raw (p, id); return std::abs (r - v) < 1e-3f ? juce::String() : id + " is " + num (r, 3); };
}
Check savedAs (JingFangProcessor& p, juce::String fam, juce::String name)
{
    return [&p, fam, name]
    {
        const auto f = JingFangProcessor::userPresetFolder().getChildFile (fam).getChildFile (name + ".xml");
        int found = 0;
        for (auto& e : p.presets()) if (e.family == fam && e.name == name) ++found;
        juce::String err;
        if (! f.existsAsFile()) err << "no file " << f.getFullPathName() << "; ";
        if (found != 1) err << found << " list entries named " << fam << " / " << name << "; ";
        if (p.currentName() != name || p.currentFamily() != fam) err << "loaded shows " << p.currentFamily() << " / " << p.currentName() << "; ";
        if (p.isModified()) err << "still marked modified; ";
        return err;
    };
}
juce::String stepsOn (JingFangProcessor& p, std::initializer_list<int> steps, bool wantOn)
{
    juce::String bad;
    for (int s : steps) if ((raw (p, "seq.s" + juce::String (s) + ".on") > 0.5f) != wantOn) bad << s << " ";
    return bad;
}

void explainerLines (Film& f, const juce::var& script)
{
    // line n: its narration's length (voice.json), then its hold for the action; the captions follow the lines
    const auto voice = juce::JSON::parse (juce::File::getCurrentWorkingDirectory().getChildFile ("out/explainers/voice/voice.json"));
    const auto* lines = script["lines"].getArray();
    double t = 0.8;
    f.L = { 0 }; f.E = { 0 };
    for (int n = 1; lines != nullptr && n <= lines->size(); ++n)
    {
        const auto& ln = lines->getReference (n - 1);
        double dur = (double) voice["films"][f.stem.toRawUTF8()][juce::String (n).toRawUTF8()]["seconds"];
        if (dur <= 0) { std::fprintf (stderr, "no narration length for %s line %d: run tools/jf_voice.py\n", f.stem.toRawUTF8(), n); dur = 4.0; }
        f.L.push_back (t); f.E.push_back (t + dur);
        const double next = t + dur + (double) ln["hold"] + 0.35;
        f.caps.push_back ({ t - 0.15, next - 0.1, ln["cap"].toString(), ln["sub"].toString(), n });
        t = next;
    }
    f.length = t + 3.5;                                            // the closing card
}

void e1 (Film& f, JingFangProcessor& p, JingFangEditor& ed)
{
    auto& L = f.L;
    f.at (0, [&p] { p.loadPresetIndex (presetIndex (p, "Keys - E-Piano", "Tine EP")); });
    f.expect (0.3, "Tine EP is loaded", presetIs (p, "Keys - E-Piano", "Tine EP"));
    f.cam (0, [] { return whole(); });
    static const int prog[4][4] = { { 50, 57, 60, 65 }, { 48, 55, 59, 64 }, { 45, 52, 57, 60 }, { 47, 53, 57, 62 } };
    const double end = f.E.back() + 3.0;
    int bar = 0;
    for (double t = 0.9; t < end; t += 2.2, ++bar) { const auto& c = prog[bar % 4]; f.chord (t, 1.9, { c[0], c[1], c[2], c[3] }); }
    // 2: one panel changes, the rest stays
    f.cam (L[2], [&ed] { return section (ed, "OSC 1").getUnion (section (ed, "OSC 2")).expanded (40); });
    f.at (L[2] + 1.2, [&p] { p.loadSection (0, presetIndex (p, "Lead", "Supersaw Lead")); });
    f.expect (L[2] + 1.5, "OSC 1 slot shows Supersaw Lead", [&p] { return p.sectionSlot (0) == presetIndex (p, "Lead", "Supersaw Lead") ? juce::String() : "slot 0 = " + juce::String (p.sectionSlot (0)); });
    f.expect (L[2] + 1.5, "the filter still comes from Tine EP", [&p] { return p.sectionSlot (3) == presetIndex (p, "Keys - E-Piano", "Tine EP") ? juce::String() : "slot 3 = " + juce::String (p.sectionSlot (3)); });
    // 3: oscillators, envelope, filter
    f.cam (L[3], [] { return whole().reduced (60, 40); });
    f.at (L[3] + 0.4, [&p] { p.loadSection (1, presetIndex (p, "Lead", "Supersaw Lead")); });
    f.at (L[3] + 2.0, [&p] { p.loadSection (4, presetIndex (p, "Pad", "Slow Pad")); });
    f.at (L[3] + 3.8, [&p] { p.loadSection (3, presetIndex (p, "Bass", "Acid Line")); });
    f.expect (L[3] + 4.2, "three panels from three presets", [&p]
    {
        const bool ok = p.sectionSlot (1) == presetIndex (p, "Lead", "Supersaw Lead") && p.sectionSlot (4) == presetIndex (p, "Pad", "Slow Pad")
                     && p.sectionSlot (3) == presetIndex (p, "Bass", "Acid Line");
        return ok ? juce::String() : "slots 1/4/3 = " + juce::String (p.sectionSlot (1)) + "/" + juce::String (p.sectionSlot (4)) + "/" + juce::String (p.sectionSlot (3));
    });
    // 4: save the mix
    f.cam (L[4], [] { return whole(); });
    f.expect (L[3] + 4.2, "a mixed sound shows as Preset synth / Mix", presetIs (p, "Preset synth", "Mix"));
    f.expect (L[4] + 0.9, "SAVE with no door open offers the family of what is loaded (a mix: Preset synth)", [&ed] { const auto t = ed.saveTarget(); return ! t.fixed && t.family == "Preset synth" ? juce::String() : "target " + t.family; });
    f.at (L[4] + 1.0, [&p, &ed] { const auto t = ed.saveTarget(); p.saveUserPreset (t.family, "My Tine Mix"); });
    f.expect (L[4] + 1.3, "saved as Preset synth / My Tine Mix", savedAs (p, "Preset synth", "My Tine Mix"));
    f.expect (L[4] + 1.4, "the saved preset reloads to the same sound", [&p]
    {
        std::vector<float> before;
        for (int i = 0; i < svl::kNumParams; ++i) before.push_back (p.exactValue (i));
        p.loadPresetIndex (p.currentPresetIndex());
        juce::AudioBuffer<float> b (2, 64); juce::MidiBuffer m; p.processBlock (b, m);
        juce::String err;
        for (int i = 0; i < svl::kNumParams && err.length() < 300; ++i)
            if (std::abs (p.exactValue (i) - before[(size_t) i]) > 1e-5f) err << JingFangProcessor::paramIds()[i] << " " << num (before[(size_t) i], 3) << " -> " << num (p.exactValue (i), 3) << "; ";
        return err;
    });
}

void e2 (Film& f, JingFangProcessor& p, JingFangEditor& ed)
{
    auto& L = f.L;
    f.at (0, [&p] { p.loadPresetIndex (presetIndex (p, "Bass", "Acid Line")); });
    f.cam (0, [] { return whole(); });
    f.note (0.9, 0.5, 45); f.note (1.6, 0.5, 48);
    // 1: open SEQ: an instrument preset gives an empty sequence (writing from scratch)
    f.at (L[1] + 0.3, [&ed] { ed.seqKey().onClick(); });
    f.cam (L[1] + 0.3, [&ed] { return inEditor (ed, ed.seqDoorView()); });
    f.expect (L[1] + 0.9, "SEQ door open", doorOpen (ed, "SEQ"));
    f.expect (L[1] + 0.9, "an instrument preset opens an empty sequence", [&p]
    {
        juce::String on;
        for (int s = 1; s <= 16; ++s) if (raw (p, "seq.s" + juce::String (s) + ".on") > 0.5f) on << s << " ";
        return on.isEmpty() ? juce::String() : "steps on: " + on;
    });
    // 2: click steps, turn a note
    static const int steps[] = { 1, 4, 7, 9, 11, 14 };
    for (int i = 0; i < 6; ++i)
        f.at (L[2] + 0.3 + i * 0.35, [&ed, s = steps[i]] { if (auto* k = stepKey (ed.seqDoorView(), "seq.s" + juce::String (s) + ".on")) click (*k); });
    f.expect (L[2] + 2.6, "six clicked steps are on", [&p] { const auto b = stepsOn (p, { 1, 4, 7, 9, 11, 14 }, true); return b.isEmpty() ? b : "off: " + b; });
    f.expect (L[2] + 2.6, "a step click switches the sequencer on", paramIs (p, "seq.on", 1.0f));
    f.at (L[2] + 2.8, [&p] { setP (p, "seq.s4.note", 7.0f); setP (p, "seq.s9.note", 12.0f); setP (p, "seq.s11.accent", 1.0f); setP (p, "seq.s14.slide", 1.0f); });
    f.note (L[2] + 0.4, f.E[2] + 2.2 - L[2], 45);
    // 3: PLAY, close
    f.at (L[3] + 0.2, [&ed] { if (auto* k = doorKey (ed.seqDoorView(), "PLAY")) k->onClick(); });
    f.expect (L[3] + 1.4, "PLAY runs the sequence with no key held", [&p] { return p.engineForTest().noteGen().isRunning() ? juce::String() : "not running"; });
    f.at (L[3] + 1.8, [&ed] { ed.closeKey().onClick(); });
    f.cam (L[3] + 1.8, [] { return whole(); });
    f.expect (L[3] + 2.6, "the door closed, the sequence still runs", [&p, &ed]
    {
        juce::String e = doorOpen (ed, "") ();
        if (! p.engineForTest().noteGen().isRunning()) e << " stopped";
        return e;
    });
    // 4: play on top
    for (int i = 0; i < 6; ++i) f.chord (L[4] + 0.3 + i * 0.75, 0.6, { 69 + (i % 3) * 2, 76 }, 84);
    // 5: back in SEQ the steps are still there; SAVE goes to Sequence - Mine
    f.at (L[5] - 0.6, [&ed] { ed.seqKey().onClick(); });
    f.cam (L[5] - 0.6, [&ed] { return inEditor (ed, ed.seqDoorView()); });
    f.expect (L[5] - 0.1, "reopening SEQ keeps the steps you wrote", [&p] { const auto b = stepsOn (p, { 1, 4, 7, 9, 11, 14 }, true); return b.isEmpty() ? b : "cleared: " + b; });
    f.expect (L[5] + 0.5, "SAVE from SEQ goes to Sequence - Mine", [&ed] { const auto t = ed.saveTarget(); return t.fixed && t.family == "Sequence - Mine" && t.name == "My Sequence" ? juce::String() : "target " + t.family + " / " + t.name; });
    f.at (L[5] + 0.7, [&p, &ed] { p.saveUserPreset (ed.saveTarget().family, "Acid Steps"); });
    f.expect (L[5] + 0.9, "saved as Sequence - Mine / Acid Steps", savedAs (p, "Sequence - Mine", "Acid Steps"));
    f.expect (L[5] + 1.0, "saving does not stop the sequence", [&p] { return p.engineForTest().noteGen().isRunning() ? juce::String() : "stopped"; });
    f.at (f.E[5] + 1.0, [&ed] { if (auto* k = doorKey (ed.seqDoorView(), "PLAY")) k->onClick(); });
    f.expect (f.E[5] + 1.4, "PLAY again stops it", [&p] { return ! p.engineForTest().noteGen().isRunning() ? juce::String() : "still running"; });
    f.at (f.E[5] + 1.5, [&ed] { ed.closeKey().onClick(); });
    f.cam (f.E[5] + 1.5, [] { return whole(); });
}

void e3 (Film& f, JingFangProcessor& p, JingFangEditor& ed)
{
    auto& L = f.L;
    f.at (0, [&p] { p.loadPresetIndex (presetIndex (p, "Drum Machine", "Boom Bap")); });
    f.cam (0, [] { return whole(); });
    f.at (L[1] + 0.3, [&ed] { ed.drumKey().onClick(); });
    f.cam (L[1] + 0.3, [&ed] { return inEditor (ed, ed.drumDoorView()); });
    f.expect (L[1] + 0.9, "DRUM door open", doorOpen (ed, "DRUM"));
    f.expect (L[1] + 0.9, "the drum machine waits for a key or PLAY", [&p] { return ! drumsRunning (p) ? juce::String() : "running on its own"; });
    // 2: a hit, then an accent, on the hat track
    f.at (L[2] + 0.3, [&ed] { if (auto* k = stepKey (ed.drumDoorView(), "drums.t3.s4")) click (*k); });
    f.at (L[2] + 1.6, [&ed] { if (auto* k = stepKey (ed.drumDoorView(), "drums.t3.s4")) click (*k); });
    f.at (L[2] + 2.2, [&ed] { if (auto* k = stepKey (ed.drumDoorView(), "drums.t3.s12")) click (*k); });
    auto before = std::make_shared<std::pair<float, float>>();
    f.at (L[2] + 0.2, [&p, before] { *before = { raw (p, "drums.t3.s4"), raw (p, "drums.t3.s12") }; });
    f.expect (L[2] + 2.5, "two clicks step a key two states on, one click one state", [&p, before]
    {
        const float a = raw (p, "drums.t3.s4"), b = raw (p, "drums.t3.s12");
        const float wa = std::fmod (before->first + 2.0f, 3.0f), wb = std::fmod (before->second + 1.0f, 3.0f);
        return std::abs (a - wa) < 1e-3f && std::abs (b - wb) < 1e-3f ? juce::String()
             : "t3 s4 " + num (before->first, 0) + " -> " + num (a, 0) + " (want " + num (wa, 0) + "), s12 " + num (before->second, 0) + " -> " + num (b, 0) + " (want " + num (wb, 0) + ")";
    });
    // 3: hold a key, let go, then PLAY
    f.note (L[3] + 0.2, 2.8, 60);
    f.expect (L[3] + 1.5, "holding a key runs the beat", [&p] { return drumsRunning (p) ? juce::String() : "not running"; });
    f.expect (L[3] + 3.6, "letting go stops it", [&p] { return ! drumsRunning (p) ? juce::String() : "still running"; });
    f.at (L[3] + 3.8, [&ed] { if (auto* k = doorKey (ed.drumDoorView(), "PLAY")) k->onClick(); });
    f.expect (L[3] + 4.6, "PLAY runs it with no key", [&p] { return drumsRunning (p) ? juce::String() : "not running"; });
    // 4: OCT
    f.at (L[4] + 0.8, [&p] { setP (p, "drums.octave", 1.0f); });
    f.expect (L[4] + 1.1, "OCT +1 reaches the engine", [&p] { const float v = p.exactValue (JingFangProcessor::paramIds().indexOf ("drums.octave")); return std::abs (v - 1.0f) < 1e-3f ? juce::String() : "engine " + num (v); });
    f.at (L[4] + 2.6, [&p] { setP (p, "drums.octave", -1.0f); });
    f.at (f.E[4] + 1.6, [&p] { setP (p, "drums.octave", 0.0f); });
    // 5: SAVE goes to Drum Machine
    f.expect (L[5] + 0.3, "SAVE from DRUM goes to Drum Machine", [&ed] { const auto t = ed.saveTarget(); return t.fixed && t.family == "Drum Machine" ? juce::String() : "target " + t.family; });
    f.expect (L[5] + 0.3, "SAVE's suggested name is not a factory preset's", [&p, &ed]
    {
        const auto t = ed.saveTarget();
        for (auto& e : p.presets()) if (e.family == t.family && e.name == t.name && e.factory)
            return "suggested name '" + t.name + "' is a factory preset: SAVE would keep two '" + t.name + "' in " + t.family;
        return juce::String();
    });
    f.at (L[5] + 0.7, [&p, &ed] { p.saveUserPreset (ed.saveTarget().family, "My Beat"); });
    f.expect (L[5] + 0.9, "saved as Drum Machine / My Beat", savedAs (p, "Drum Machine", "My Beat"));
    f.expect (L[5] + 1.0, "saving does not stop the beat", [&p] { return drumsRunning (p) ? juce::String() : "stopped"; });
    f.at (f.E[5] + 1.0, [&ed] { if (auto* k = doorKey (ed.drumDoorView(), "PLAY")) k->onClick(); });
    f.expect (f.E[5] + 1.6, "PLAY again stops it", [&p] { return ! drumsRunning (p) ? juce::String() : "still running"; });
    f.at (f.E[5] + 1.7, [&ed] { ed.closeKey().onClick(); });
    f.cam (f.E[5] + 1.7, [] { return whole(); });
}

void e4 (Film& f, JingFangProcessor& p, JingFangEditor& ed)
{
    auto& L = f.L;
    f.at (0, [&p] { p.loadPresetIndex (presetIndex (p, "Strings", "Legato Strings")); });
    f.cam (0, [] { return whole(); });
    static const int mak[] = { 62, 64, 65, 67, 69, 71, 72, 74, 72, 71, 69, 67, 65, 64, 62, 64 };
    const double end = f.E.back() + 1.0;
    int i = 0;
    for (double t = 0.9; t < end; t += 0.55, ++i) f.note (t, 0.5, mak[i % 16], 96);
    f.at (L[1] + 0.4, [&ed] { ed.microKey().onClick(); });
    f.cam (L[1] + 0.4, [&ed] { return inEditor (ed, &ed.keyboardView()).expanded (30, 60); });
    f.expect (L[1] + 0.8, "MICRO sets the tuning mode", paramIs (p, "tuning.mode", 1.0f));
    f.expect (L[1] + 1.0, "in MICRO with no row moved, a key plays its usual pitch", [&p]
    {
        for (int k = 60; k < 76; ++k) { const float v = voicePitchFor (p, k); if (v >= 0 && std::abs (v - (float) k) < 0.6f && std::abs (v - (float) k) > 0.01f) return "key " + juce::String (k) + " plays " + num (v, 3); }
        return juce::String();
    });
    // 3: E and B to the lower row while playing
    f.at (L[3] + 0.4, [&p] { setP (p, "tuning.row.e", 0.0f); });
    f.at (L[3] + 1.6, [&p] { setP (p, "tuning.row.b", 0.0f); });
    f.note (L[3] + 2.6, 1.2, 76, 80);                                       // a held E (and B, below) to judge the rows by
    f.note (L[4] + 1.0, 1.2, 76, 80); f.note (L[4] + 1.0, 1.2, 83, 80);
    for (double t : { L[3] + 3.0, L[3] + 3.4 })
        f.expect (t, "a moved E sounds a comma off E", [&p]
        {
            const float v = voicePitchFor (p, 76);
            if (v < 0 || std::abs (v - 76.0f) > 0.6f) return juce::String ("(E not sounding at this moment)");
            return std::abs (v - 76.0f) > 0.15f && std::abs (v - 76.0f) < 0.3f ? juce::String() : "E5 plays " + num (v, 3);
        });
    // 4: NORMAL
    f.at (L[4] + 0.3, [&ed] { ed.normalKey().onClick(); });
    f.cam (L[4] + 0.3, [] { return whole(); });
    f.expect (L[4] + 0.6, "NORMAL sets the tuning mode back", paramIs (p, "tuning.mode", 0.0f));
    for (double t : { L[4] + 1.4, L[4] + 1.9 })
        f.expect (t, "in NORMAL a moved key plays its usual pitch again", [&p]
        {
            bool judged = false;
            for (int k : { 76, 83 })
            {
                const float v = voicePitchFor (p, k);
                if (v < 0 || std::abs (v - (float) k) >= 0.6f) continue;
                judged = true;
                if (std::abs (v - (float) k) > 0.01f) return "key " + juce::String (k) + " plays " + num (v, 3);
            }
            return judged ? juce::String() : juce::String ("(E and B not sounding at this moment)");
        });
}

void e5 (Film& f, JingFangProcessor& p, JingFangEditor& ed)
{
    auto& L = f.L;
    f.at (0, [&p] { p.loadPresetIndex (presetIndex (p, "Pluck", "Concert Harp")); });
    f.cam (0, [] { return whole(); });
    f.note (0.9, 0.5, 62); f.note (1.5, 0.5, 65); f.note (2.1, 0.8, 69);
    f.at (L[1] + 0.3, [&ed] { ed.labKey().onClick(); });
    f.cam (L[1] + 0.3, [&ed] { return inEditor (ed, ed.labDoorView()); });
    f.expect (L[1] + 0.9, "LAB door open", doorOpen (ed, "LAB"));
    // 2: LEARN from the solo line (the real path: the file dropped on card A); the film's clock waits for it
    auto learnDone = std::make_shared<juce::String>();
    f.at (L[2] + 0.4, [&ed, learnDone]
    {
        const juce::File lead ("/Users/senolsahan/Desktop/Hicaz_Live/stems/lead.wav");
        ed.labDoorView()->loadFileForTest (lead, 0);
        const auto t0 = juce::Time::getMillisecondCounter();
        while (ed.labDoorView()->statusText().contains ("listening") && juce::Time::getMillisecondCounter() - t0 < 120000)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        *learnDone = ed.labDoorView()->statusText() + " (" + juce::String ((juce::Time::getMillisecondCounter() - t0) / 1000.0, 1) + " s)";
        std::printf ("  LEARN: %s\n", learnDone->toRawUTF8());
    });
    f.expect (L[2] + 0.5, "LEARN finishes with a tuning", [learnDone] { return learnDone->contains ("learned") ? juce::String() : "status: " + *learnDone; });
    f.expect (L[2] + 0.5, "the learned tuning has the Hicaz steps (113 / 385 ct, within 10)", [&p]
    {
        const auto& c = p.labState().a.cents;
        if (c.size() < 3) return "only " + juce::String ((int) c.size()) + " steps";
        return std::abs (c[0] - 113.2) < 10 && std::abs (c[1] - 384.9) < 10 ? juce::String() : "steps " + num (c[0], 1) + " / " + num (c[1], 1);
    });
    // 3: switch the Lab on and play the line
    f.at (L[3] + 0.1, [&p] { auto ls = p.labState(); ls.on = true; ls.directional = false; ls.a.middleKey = ls.a.refKey = 62; ls.a.refHz = 293.6648; p.setLabState (ls); });
    f.expect (L[3] + 0.3, "the Lab plays", [&p] { return p.labActive() ? juce::String() : "lab not active"; });
    static const int deg[] = { 0, 1, 2, 3, 4, 3, 2, 1 };
    for (int i = 0; i < 14; ++i) f.note (L[3] + 0.4 + i * 0.42, 0.38, 62 + deg[i % 8], 96);   // ends before line 4's notes
    f.expect (L[3] + 0.4 + 0.42 + 0.2, "key 63 plays the learned second (62 + 1.13)", [&p]
    {
        const float v = voicePitchFor (p, 63);
        if (v < 0 || p.labState().a.cents.empty()) return juce::String ("key 63 not sounding");
        return std::abs (v - (62.0f + (float) p.labState().a.cents[0] / 100.0f)) < 0.03f ? juce::String() : "plays " + num (v, 3);
    });
    // 4: morph to equal temperament
    f.at (L[4] + 0.2, [&p] { auto ls = p.labState(); ls.b = jflab::Tuning(); ls.morph = 0; p.setLabState (ls); });
    for (int k = 0; k < 16; ++k) f.at (L[4] + 0.6 + k * 0.25, [&p, k] { auto ls = p.labState(); ls.morph = (float) (k + 1) / 16.0f; p.setLabState (ls); });
    for (int i = 0; i < 18; ++i) f.note (L[4] + 0.3 + i * 0.42, 0.38, 62 + deg[i % 8], 96);
    f.expect (L[4] + 0.3 + 15 * 0.42 + 0.2, "at morph 1 every key is equal-tempered (B = 12 equal)", [&p]
    {
        for (int k = 62; k < 67; ++k) { const float v = voicePitchFor (p, k); if (v >= 0 && std::abs (v - (float) k) < 0.6f && std::abs (v - (float) k) > 0.02f) return "key " + juce::String (k) + " plays " + num (v, 3); }
        return juce::String();
    });
    f.at (f.E[4] + 3.6, [&ed] { ed.closeKey().onClick(); });
}

void e6 (Film& f, JingFangProcessor& p, JingFangEditor& ed)
{
    auto& L = f.L;
    f.at (0, [&p] { p.loadPresetIndex (presetIndex (p, "Lead", "VA Lead")); });
    f.cam (0, [] { return whole(); });
    const double end = f.E.back() + 2.5;
    int i = 0;
    for (double t = 0.9; t < end; t += 1.8, ++i) f.chord (t, 1.6, { 57 + (i % 3) * 2, 64 + (i % 3) * 2 }, 90);
    f.at (L[1] + 0.3, [&ed] { ed.patchKey().onClick(); });
    f.cam (L[1] + 0.3, [&ed] { return inEditor (ed, &ed.patchBay()); });
    f.expect (L[1] + 0.9, "PATCH door open", doorOpen (ed, "PATCH"));
    auto drag = [&ed] (juce::Point<float> from, juce::Point<float> to)
    {
        auto& bay = ed.patchBay();
        const auto t0 = juce::Time::getCurrentTime();
        auto src = juce::Desktop::getInstance().getMainMouseSource();
        bay.mouseDown (juce::MouseEvent (src, from, {}, 1.0f, 0, 0, 0, 0, &bay, &bay, t0, from, t0, 1, false));
        for (int s = 1; s <= 4; ++s) { const auto q = from + (to - from) * (float) s / 4.0f; bay.mouseDrag (juce::MouseEvent (src, q, {}, 1.0f, 0, 0, 0, 0, &bay, &bay, t0, from, t0, 1, false)); }
        bay.mouseUp (juce::MouseEvent (src, to, {}, 1.0f, 0, 0, 0, 0, &bay, &bay, t0, from, t0, 1, false));
    };
    auto cables = std::make_shared<int> (0);
    f.at (L[2] + 0.4, [&ed, cables] { *cables = ed.patchBay().cableCount(); });
    f.at (L[2] + 0.6, [&ed, drag] { drag (ed.patchBay().jack (true, (int) svl::ModSrc::lfo1 - 1), ed.patchBay().jack (false, (int) svl::ModDst::cutoff - 1)); });
    f.expect (L[2] + 0.9, "a dragged cable is one more cable, LFO 1 -> cutoff", [&p, &ed, cables]
    {
        bool found = false;
        for (int m = 1; m <= svl::kNumModSlots; ++m)
            found |= juce::roundToInt (raw (p, "mod" + juce::String (m) + ".source")) == (int) svl::ModSrc::lfo1
                  && juce::roundToInt (raw (p, "mod" + juce::String (m) + ".destination")) == (int) svl::ModDst::cutoff;
        return found && ed.patchBay().cableCount() == *cables + 1 ? juce::String()
             : "cables " + juce::String (*cables) + " -> " + juce::String (ed.patchBay().cableCount()) + (found ? "" : ", no lfo1 -> cutoff slot");
    });
    f.at (L[3] + 0.3, [&p]
    {
        for (int m = 1; m <= svl::kNumModSlots; ++m)
            if (juce::roundToInt (raw (p, "mod" + juce::String (m) + ".destination")) == (int) svl::ModDst::cutoff) { setP (p, "mod" + juce::String (m) + ".amount", 0.8f); break; }
        setP (p, "lfo1.rate", 4.0f);
    });
    f.at (L[3] + 3.0, [&ed, drag] { const auto j = ed.patchBay().jack (false, (int) svl::ModDst::cutoff - 1); drag (j, j + juce::Point<float> (-160.0f, 120.0f)); });
    f.expect (L[3] + 3.3, "pulling the cable out removes it", [&ed, cables] { return ed.patchBay().cableCount() == *cables ? juce::String() : "cables " + juce::String (ed.patchBay().cableCount()) + ", expected " + juce::String (*cables); });
    f.at (L[3] + 3.6, [&ed, drag] { drag (ed.patchBay().jack (true, (int) svl::ModSrc::lfo1 - 1), ed.patchBay().jack (false, (int) svl::ModDst::cutoff - 1)); });
    f.at (L[4] + 0.2, [&ed] { ed.closeKey().onClick(); });
    f.cam (L[4] + 0.2, [] { return whole(); });
    f.expect (L[4] + 0.8, "the door closed", doorOpen (ed, ""));
    f.expect (L[4] + 0.8, "the cable stays after closing", [&ed, cables] { return ed.patchBay().cableCount() == *cables + 1 ? juce::String() : "cables " + juce::String (ed.patchBay().cableCount()); });
}

// ---------------------------------------------------------------- the 1.1 sounds (owner 24 Oct: "yeni gelen sesler jing fang için 2 tutorial videosu")
Rect strip() { return Rect (350, 24, 878, 158).expanded (30, 40); }        // the preset strip (layout_v3.json "synth")

void e7 (Film& f, JingFangProcessor& p, JingFangEditor& ed)
{
    auto& L = f.L;
    auto load = [&p] (const char* fam, const char* name) { return [&p, fam, name] { p.loadPresetIndex (presetIndex (p, fam, name)); }; };
    f.at (0, load ("Kanun", "Kanun"));
    f.expect (0.3, "Kanun is loaded", presetIs (p, "Kanun", "Kanun"));
    f.cam (0, [] { return whole(); });
    static const int line[] = { 62, 63, 66, 67, 69, 67, 66, 63, 62, 63, 66, 69, 70, 69, 67, 66 };
    int i = 0;
    for (double t = 0.9; t < L[3] - 0.3; t += 0.42, ++i) f.note (t, 0.36, line[i % 16], 96);
    // 2: the Maqam category; the zurna takes the same line
    f.cam (L[2], [] { return strip(); });
    f.at (L[2] + 0.5, load ("Zurna", "Zurna"));
    f.expect (L[2] + 0.8, "Zurna is loaded", presetIs (p, "Zurna", "Zurna"));
    f.cam (f.E[2] + 0.4, [] { return whole(); });
    // 3: variations: a sliding sitar, then the kanun's repeated pick on long notes
    f.at (L[3] + 0.1, load ("Sitar", "Sitar Meend"));
    f.expect (L[3] + 0.4, "Sitar Meend is loaded", presetIs (p, "Sitar", "Sitar Meend"));
    static const int slide[] = { 62, 66, 67, 69, 67, 66 };
    const double tKanun = L[3] + 3.6;
    for (int k = 0; k < 6; ++k) f.note (L[3] + 0.3 + k * 0.55, 0.6, slide[k], 96);         // overlapping: legato, the glide joins them
    f.at (tKanun, load ("Kanun", "Kanun Tremolo"));
    f.expect (tKanun + 0.3, "Kanun Tremolo is loaded", presetIs (p, "Kanun", "Kanun Tremolo"));
    static const int held[] = { 69, 67, 62 };
    for (int k = 0; k < 3; ++k) f.note (tKanun + 0.3 + k * 1.5, 1.35, held[k], 96);
    // 4: a Sequence - Lab melody loads its tuning and switches the Lab on
    f.at (L[4] + 0.3, load ("Sequence - Lab", "Guzheng Running Water"));
    f.expect (L[4] + 0.6, "Guzheng Running Water is loaded", presetIs (p, "Sequence - Lab", "Guzheng Running Water"));
    f.expect (L[4] + 0.6, "the melody's tuning is on: the Lab plays", [&p] { return p.labActive() ? juce::String() : "lab not active"; });
    f.cam (L[4] + 0.3, [] { return strip(); });
    f.cam (f.E[4], [] { return whole(); });
    // 5: hold one key: the melody runs; let go: it stops
    const double hold0 = L[5] + 0.3, hold1 = f.E[5] + 3.4;
    f.note (hold0, hold1 - hold0, 60, 100);
    f.expect (hold0 + 1.2, "holding a key runs the melody", [&p] { return p.engineForTest().noteGen().isRunning() ? juce::String() : "not running"; });
    f.expect (hold1 + 0.6, "letting go stops it", [&p] { return ! p.engineForTest().noteGen().isRunning() ? juce::String() : "still running"; });
}

void e8 (Film& f, JingFangProcessor& p, JingFangEditor& ed)
{
    auto& L = f.L;
    auto load = [&p] (const char* fam, const char* name) { return [&p, fam, name] { p.loadPresetIndex (presetIndex (p, fam, name)); }; };
    f.at (0, load ("Bass", "Round Mono"));
    f.expect (0.3, "Round Mono is loaded", presetIs (p, "Bass", "Round Mono"));
    f.cam (0, [] { return whole(); });
    static const int bass[] = { 36, 36, 43, 46, 36, 48, 46, 43 };
    int i = 0;
    for (double t = 0.9; t < L[3] - 0.2; t += 0.36, ++i) f.note (t, 0.3, bass[i % 8], i % 4 == 0 ? 110 : 92);
    // 2: more bite on the same line
    f.at (L[2] + 0.2, load ("Bass", "Dirty Mono"));
    f.expect (L[2] + 0.5, "Dirty Mono is loaded", presetIs (p, "Bass", "Dirty Mono"));
    f.at (L[2] + 2.4, load ("Bass", "Chip Bass"));
    f.expect (L[2] + 2.7, "Chip Bass is loaded", presetIs (p, "Bass", "Chip Bass"));
    // 3: a soft start, long notes
    f.at (L[3] + 0.1, load ("Bass", "Bark Bass"));
    f.expect (L[3] + 0.4, "Bark Bass is loaded", presetIs (p, "Bass", "Bark Bass"));
    const double tBreath = L[3] + 3.3;
    static const int longA[] = { 36, 43 }, longB[] = { 38, 45 };
    for (int k = 0; k < 2; ++k) f.note (L[3] + 0.3 + k * 1.5, 1.3, longA[k], 100);
    f.at (tBreath, load ("Bass", "Breath Bass"));
    f.expect (tBreath + 0.3, "Breath Bass is loaded", presetIs (p, "Bass", "Breath Bass"));
    for (int k = 0; k < 2; ++k) f.note (tBreath + 0.2 + k * 1.5, 1.3, longB[k], 100);
    // 4: the arrows step to the next preset (what prevBtn / nextBtn do)
    f.cam (L[4], [] { return strip(); });
    auto before = std::make_shared<int> (-1);
    f.at (L[4] + 0.6, [&p, before] { *before = p.currentPresetIndex(); p.stepPreset (1); });
    f.expect (L[4] + 0.9, "the arrow loads the next preset", [&p, before] { return p.currentPresetIndex() != *before ? juce::String() : "index stayed " + juce::String (*before); });
    f.at (L[4] + 2.2, [&p] { p.stepPreset (1); });
    for (int k = 0; k < 6; ++k) f.note (L[4] + 0.9 + k * 0.5, 0.42, bass[k], 96);
    f.cam (f.E[4] + 1.0, [] { return whole(); });
    // 5: new pads, keys, plucks, strings and brass
    struct V { const char* fam; const char* name; };
    static const V more[] = { { "Pad", "Glass Box Pad" }, { "Keys - E-Piano", "Night EP" }, { "Pluck", "Harp Pluck" }, { "Strings", "Orchestral Strings" }, { "Brass", "Fat Brass" } };
    const double step = (f.E[5] + 3.6 - L[5]) / 5.0;
    for (int k = 0; k < 5; ++k)
    {
        const double t = L[5] + k * step;
        f.at (t, load (more[k].fam, more[k].name));
        f.expect (t + 0.3, juce::String (more[k].name) + " is loaded", presetIs (p, more[k].fam, more[k].name));
        f.chord (t + 0.15, step - 0.35, { 50, 57, 62, 66 }, 88);
    }
}

// ---------------------------------------------------------------- GENERATE (owner 27 Oct: "tanıtım videolarını çek")
jfui::SeqGenPanel* genPanel (JingFangEditor& ed) { return ed.seqDoorView() != nullptr ? ed.seqDoorView()->genPanelView() : nullptr; }
Rect genView (JingFangEditor& ed) { auto* g = genPanel (ed); return g != nullptr ? inEditor (ed, g).expanded (20) : whole(); }
int stepsOnCount (JingFangProcessor& p) { int n = 0; for (int s = 1; s <= 16; ++s) n += raw (p, "seq.s" + juce::String (s) + ".on") > 0.5f; return n; }
int chordSteps (JingFangProcessor& p) { int n = 0; for (int s = 1; s <= 16; ++s) n += raw (p, "seq.s" + juce::String (s) + ".on") > 0.5f && raw (p, "seq.s" + juce::String (s) + ".c1") > 0.5f; return n; }
int microSteps (JingFangProcessor& p) { int n = 0; for (int s = 1; s <= 16; ++s) n += raw (p, "seq.s" + juce::String (s) + ".on") > 0.5f && juce::roundToInt (raw (p, "seq.s" + juce::String (s) + ".row")) != 1; return n; }

void e9 (Film& f, JingFangProcessor& p, JingFangEditor& ed)
{
    auto& L = f.L;
    p.genSeedRandomForTest (2710);                                 // the same film every render
    f.at (0, [&p] { p.loadPresetIndex (presetIndex (p, "Lead", "Bright Saw Lead")); });
    f.expect (0.3, "Bright Saw Lead is loaded", presetIs (p, "Lead", "Bright Saw Lead"));
    f.cam (0, [] { return whole(); });
    f.at (L[1] + 0.3, [&ed] { ed.seqKey().onClick(); });
    f.expect (L[1] + 0.9, "SEQ door open", doorOpen (ed, "SEQ"));
    f.cam (L[1] + 0.9, [&ed] { return genView (ed); });
    // 2: ROLL, then a held key runs the melody
    f.at (L[2] + 0.6, [&ed] { genPanel (ed)->key ("ROLL")->onClick(); });
    f.expect (L[2] + 0.9, "ROLL wrote a melody (3+ notes, no chords)", [&p] { return stepsOnCount (p) >= 3 && chordSteps (p) == 0 ? juce::String() : "steps " + juce::String (stepsOnCount (p)) + ", chords " + juce::String (chordSteps (p)); });
    f.note (L[2] + 1.0, f.E[2] + 3.6 - (L[2] + 1.0), 60, 100);
    f.expect (L[2] + 2.0, "holding a key runs the sequence", [&p] { return p.engineForTest().noteGen().isRunning() ? juce::String() : "not running"; });
    // 3: NOTES keeps the rhythm
    auto rBefore = std::make_shared<int> (-1);
    f.at (L[3] + 0.3, [&p, &ed, rBefore] { *rBefore = p.genUnit (0).r; genPanel (ed)->key ("NOTES")->onClick(); });
    f.expect (L[3] + 0.6, "NOTES kept the RHYTHM seed", [&p, rBefore] { return p.genUnit (0).r == *rBefore ? juce::String() : "R " + juce::String (*rBefore) + " -> " + juce::String (p.genUnit (0).r); });
    f.note (L[3] + 0.6, f.E[3] + 3.1 - (L[3] + 0.6), 60, 100);
    // 4: DENSITY up: the same pattern, more notes
    auto nBefore = std::make_shared<int> (0);
    f.at (L[4] + 0.2, [&p, nBefore] { *nBefore = stepsOnCount (p); });
    for (int k = 1; k <= 8; ++k) f.at (L[4] + 0.3 + k * 0.12, [&p, k] { setP (p, "gen.density", 0.5f + 0.05f * (float) k); });
    f.expect (L[4] + 2.0, "DENSITY 0.9: more notes", [&p, nBefore] { return stepsOnCount (p) > *nBefore ? juce::String() : juce::String (*nBefore) + " -> " + juce::String (stepsOnCount (p)); });
    f.note (L[4] + 0.4, f.E[4] + 3.1 - (L[4] + 0.4), 60, 100);
    f.at (f.E[4] + 3.2, [&p] { setP (p, "gen.density", 0.5f); });
    // 5: a pad: chords
    f.at (L[5] + 0.2, [&p] { p.loadPresetIndex (presetIndex (p, "Pad", "Warm Pad")); });
    f.expect (L[5] + 0.5, "Warm Pad is loaded", presetIs (p, "Pad", "Warm Pad"));
    f.at (L[5] + 0.8, [&ed] { genPanel (ed)->key ("ROLL")->onClick(); });
    f.expect (L[5] + 1.1, "ROLL wrote chords", [&p] { return chordSteps (p) >= 2 && chordSteps (p) == stepsOnCount (p) ? juce::String() : "chord steps " + juce::String (chordSteps (p)) + " of " + juce::String (stepsOnCount (p)); });
    f.note (L[5] + 1.2, f.E[5] + 4.1 - (L[5] + 1.2), 60, 100);
    // 6: CLIP (the drag itself leaves the window; the camera shows the key)
    f.cam (L[6], [&ed] { return genView (ed).getUnion (inEditor (ed, ed.seqDoorView()).withHeight (60)).expanded (10); });
}

void e10 (Film& f, JingFangProcessor& p, JingFangEditor& ed)
{
    auto& L = f.L;
    p.genSeedRandomForTest (1031);
    f.at (0, [&p] { p.loadPresetIndex (presetIndex (p, "Init", "Init")); });
    f.cam (0, [] { return whole(); });
    f.at (L[1] + 0.3, [&ed] { ed.drumKey().onClick(); });
    f.expect (L[1] + 0.9, "DRUM door open", doorOpen (ed, "DRUM"));
    f.cam (L[1] + 0.9, [&ed] { return inEditor (ed, ed.drumDoorView()).expanded (10); });
    // 2: ROLL, a held key plays the beat
    auto hits = [&p] { int n = 0; for (int t = 1; t <= 8; ++t) for (int st = 1; st <= 16; ++st) n += raw (p, "drums.t" + juce::String (t) + ".s" + juce::String (st)) > 0.5f; return n; };
    f.at (L[2] + 0.5, [&ed] { ed.drumDoorView()->drumGenView()->rollKey()->onClick(); });
    f.expect (L[2] + 0.8, "ROLL wrote a beat with the kick on 1", [&p, hits] { return hits() >= 6 && raw (p, "drums.t1.s1") > 0.5f ? juce::String() : "hits " + juce::String (hits()); });
    f.note (L[2] + 0.9, f.E[2] + 3.6 - (L[2] + 0.9), 60, 100);
    // 3: another roll, then the arrow brings the first one back
    auto first = std::make_shared<int> (0);
    f.at (L[3] + 0.2, [&ed, first, hits] { *first = hits(); ed.drumDoorView()->drumGenView()->rollKey()->onClick(); });
    f.note (L[3] + 0.3, 1.3, 60, 100);
    f.at (L[3] + 1.8, [&p] { p.genHistory (1, -1); });
    f.expect (L[3] + 2.0, "the arrow brought the first beat back", [first, hits] { return hits() == *first ? juce::String() : juce::String (*first) + " vs " + juce::String (hits()); });
    f.note (L[3] + 1.9, f.E[3] + 2.6 - (L[3] + 1.9), 60, 100);
    // 4: MICRO: a flute line with raised / lowered notes
    f.at (L[4] + 0.1, [&p, &ed]
    {
        p.loadPresetIndex (presetIndex (p, "Winds", "Breathy Flute"));
        ed.microKey().onClick();
        setP (p, "gen.micro", 1.0f);
        ed.seqKey().onClick();
    });
    f.expect (L[4] + 0.4, "MICRO is on", [&p] { return raw (p, "tuning.mode") > 0.5f ? juce::String() : juce::String ("tuning.mode off"); });
    f.cam (L[4] + 0.6, [&ed] { return genView (ed); });
    f.at (L[4] + 0.9, [&p, &ed]
    {   // roll until the pattern has raised / lowered notes (a roll may have none; MICRO 1 makes most have some)
        for (int i = 0; i < 12; ++i) { genPanel (ed)->key ("ROLL")->onClick(); if (microSteps (p) > 0) break; }
    });
    f.expect (L[4] + 1.2, "the roll has raised / lowered notes", [&p] { return microSteps (p) > 0 ? juce::String() : juce::String ("none"); });
    f.note (L[4] + 1.3, f.E[4] + 4.1 - (L[4] + 1.3), 62, 100);
    // 5: CLIP writes MPE
    f.cam (L[5], [&ed] { return genView (ed).getUnion (inEditor (ed, ed.seqDoorView()).withHeight (60)).expanded (10); });
}

// the long one (owner 27 Oct: "uzun anlatım videosu çek, süre mühim değil, generate bölümü bitene kadar"): 16:9, every part of GENERATE
void e11 (Film& f, JingFangProcessor& p, JingFangEditor& ed)
{
    auto& L = f.L; auto& E = f.E;
    p.genSeedRandomForTest (2811);
    auto load = [&p] (const char* fam, const char* name) { return [&p, fam, name] { p.loadPresetIndex (presetIndex (p, fam, name)); }; };
    auto roll = [&ed] (const char* k) { return [&ed, k] { genPanel (ed)->key (k)->onClick(); }; };
    auto gpR = [&ed] { auto* g = genPanel (ed); return g != nullptr ? inEditor (ed, g) : whole(); };
    auto strip = [gpR] { const auto g = gpR(); return Rect (g.getX(), g.getY(), g.getWidth(), 80.0f).expanded (10, 30); };
    auto lcd = [gpR] { const auto g = gpR(); return Rect (g.getX() + 600.0f, g.getY(), 546.0f, 72.0f).expanded (20, 40); };
    auto column = [gpR] { const auto g = gpR(); return Rect (g.getX() + 700.0f, g.getY() + 60.0f, 446.0f, 340.0f); };
    auto hold = [&f] (double t0, double t1, int key) { if (t1 > t0 + 0.1) f.note (t0, t1 - t0, key, 100); };
    auto expectRole = [&f, &p] (double t, juce::String want) { f.expect (t, "WRITES " + want, [&p, want] { return p.genWrites (0).startsWith (want) ? juce::String() : p.genWrites (0); }); };

    f.at (0, load ("Lead", "Bright Saw Lead"));
    f.cam (0, [] { return whole(); });
    // 2: the door
    f.at (L[2] + 0.4, [&ed] { ed.seqKey().onClick(); });
    f.expect (L[2] + 1.0, "SEQ door open", doorOpen (ed, "SEQ"));
    f.cam (L[2] + 1.0, [&ed] { return genView (ed); });
    // 3-4: a lead: ROLL, hold a key, then another key
    f.cam (L[4], strip);
    f.at (L[4] + 0.6, roll ("ROLL"));
    expectRole (L[4] + 0.9, "MELODY");
    f.cam (L[4] + 1.5, [&ed] { return genView (ed); });
    hold (L[4] + 1.0, E[4] + 2.6, 60);
    hold (E[4] + 2.7, E[4] + 5.0, 65);
    // 5: the readout
    f.cam (L[5], lcd);
    hold (L[5] + 0.2, E[5] + 1.3, 60);
    // 6: RHYTHM, 7: NOTES
    f.cam (L[6], [&ed] { return genView (ed); });
    auto nBefore = std::make_shared<int> (-1), rBefore = std::make_shared<int> (-1);
    f.at (L[6] + 0.4, [&p, &ed, nBefore] { *nBefore = p.genUnit (0).n; genPanel (ed)->key ("RHYTHM")->onClick(); });
    f.expect (L[6] + 0.7, "RHYTHM kept the NOTES seed", [&p, nBefore] { return p.genUnit (0).n == *nBefore ? juce::String() : "N changed"; });
    hold (L[6] + 0.6, E[6] + 3.3, 60);
    f.at (L[7] + 0.4, [&p, &ed, rBefore] { *rBefore = p.genUnit (0).r; genPanel (ed)->key ("NOTES")->onClick(); });
    f.expect (L[7] + 0.7, "NOTES kept the RHYTHM seed", [&p, rBefore] { return p.genUnit (0).r == *rBefore ? juce::String() : "R changed"; });
    hold (L[7] + 0.6, E[7] + 3.3, 60);
    // 8: no repeats: two rolls, each differs
    auto prev = std::make_shared<svl::gen::SeqPattern>();
    f.at (L[8] + 0.3, [&p, prev] { *prev = p.genCurrentSeq(); });
    f.at (L[8] + 0.5, roll ("ROLL"));
    f.expect (L[8] + 0.8, "the roll differs from the one before", [&p, prev] { const float d = svl::gen::distance (*prev, p.genCurrentSeq()); return d >= svl::gen::kMinDistance ? juce::String() : "distance " + num (d); });
    f.at (E[8] + 1.2, roll ("ROLL"));
    hold (L[8] + 0.6, E[8] + 3.3, 60);
    // 9: back, back, forward
    auto cur = std::make_shared<svl::gen::SeqPattern>();
    f.at (L[9] + 0.2, [&p, cur] { *cur = p.genCurrentSeq(); });
    f.cam (L[9], lcd);
    f.at (L[9] + 0.6, [&p] { p.genHistory (0, -1); });
    f.at (L[9] + 2.0, [&p] { p.genHistory (0, -1); });
    f.at (E[9] + 1.4, [&p] { p.genHistory (0, +1); p.genHistory (0, +1); });
    f.expect (E[9] + 1.7, "forward twice is the roll we left", [&p, cur] { return p.genCurrentSeq() == *cur ? juce::String() : juce::String ("different"); });
    hold (L[9] + 0.7, E[9] + 3.3, 60);
    // 10: DENSITY sweep
    f.cam (L[10], [&ed] { return genView (ed); });
    for (int k = 0; k <= 16; ++k)
    {
        const double t = L[10] + 0.5 + k * 0.4;
        const float v = k <= 6 ? 0.5f + 0.07f * (float) k : (k <= 13 ? 0.92f - 0.09f * (float) (k - 6) : 0.29f + 0.07f * (float) (k - 13));
        f.at (t, [&p, v] { setP (p, "gen.density", juce::jlimit (0.0f, 1.0f, v)); });
    }
    hold (L[10] + 0.3, E[10] + 4.3, 60);
    // 11: COMPLEX up, roll
    f.at (L[11] + 0.4, [&p] { setP (p, "gen.complexity", 0.85f); });
    f.at (L[11] + 1.0, roll ("ROLL"));
    hold (L[11] + 1.1, E[11] + 3.8, 60);
    f.at (E[11] + 3.9, [&p] { setP (p, "gen.complexity", 0.4f); });
    // 12: RANGE narrow then wide (the same seeds rewritten)
    f.at (L[12] + 0.4, [&p] { setP (p, "gen.range", 5.0f); });
    f.at (E[12] + 0.8, [&p] { setP (p, "gen.range", 20.0f); });
    f.at (E[12] + 3.4, [&p] { setP (p, "gen.range", 12.0f); });
    hold (L[12] + 0.5, E[12] + 3.3, 60);
    // 13: MUTATE low: two small changes
    f.at (L[13] + 0.4, [&p] { setP (p, "gen.mutate", 0.2f); });
    auto before13 = std::make_shared<svl::gen::SeqPattern>();
    f.at (L[13] + 0.9, [&p, before13] { *before13 = p.genCurrentSeq(); });
    f.at (L[13] + 1.1, roll ("ROLL"));
    f.expect (L[13] + 1.4, "MUTATE 0.2 changed a few steps, not all", [&p, before13]
    {
        int ch = 0; const auto n = p.genCurrentSeq(); for (int s = 0; s < 16; ++s) ch += ! (n.s[(size_t) s] == before13->s[(size_t) s]);
        return ch >= 1 && ch <= 8 ? juce::String() : juce::String (ch) + " steps changed";
    });
    f.at (E[13] + 1.2, roll ("ROLL"));
    f.at (E[13] + 4.3, [&p] { setP (p, "gen.mutate", 1.0f); });
    hold (L[13] + 1.2, E[13] + 4.2, 60);
    // 14: a fixed scale
    f.cam (L[14], [gpR] { const auto g = gpR(); return Rect (g.getX() + 600.0f, g.getY(), 546.0f, 72.0f).expanded (20, 40); });
    f.at (L[14] + 0.6, [&p] { setP (p, "gen.scale", 2.0f); });
    f.at (L[14] + 1.2, roll ("ROLL"));
    expectRole (L[14] + 1.5, "MELODY");
    f.expect (L[14] + 1.5, "the fixed scale shows: DORIAN", [&p] { return p.genWrites (0).containsIgnoreCase ("dorian") ? juce::String() : p.genWrites (0); });
    f.at (E[14] + 3.4, [&p] { setP (p, "gen.scale", 0.0f); });
    hold (L[14] + 1.3, E[14] + 3.3, 60);
    // 15: bass
    f.cam (L[15], [&ed] { return genView (ed); });
    f.at (L[15] + 0.2, load ("Bass", "Round Mono"));
    f.at (L[15] + 0.6, roll ("ROLL"));
    expectRole (L[15] + 0.9, "BASS");
    hold (L[15] + 0.8, E[15] + 4.3, 48);
    // 16: pad chords
    f.at (L[16] + 0.2, load ("Pad", "Warm Pad"));
    f.at (L[16] + 0.6, roll ("ROLL"));
    expectRole (L[16] + 0.9, "CHORDS");
    hold (L[16] + 0.8, E[16] + 4.8, 60);
    // 17: arp rhythm
    f.at (L[17] + 0.2, load ("Arp - Classic", "Up Down Two Octaves"));
    f.at (L[17] + 0.6, roll ("ROLL"));
    expectRole (L[17] + 0.9, "ARP RHYTHM");
    f.chord (L[17] + 0.8, E[17] + 4.3 - (L[17] + 0.8), { 60, 64, 67 }, 96);
    // 18: the selected step's column
    f.at (L[18] + 0.1, load ("Lead", "Bright Saw Lead"));
    f.at (L[18] + 0.4, roll ("ROLL"));
    f.cam (L[18] + 0.8, column);
    f.at (L[18] + 1.2, [&ed] { genPanel (ed)->selectStep (2); });
    f.at (L[18] + 3.0, [&ed] { genPanel (ed)->selectStep (6); });
    f.at (E[18] + 1.0, [&ed] { genPanel (ed)->selectStep (10); });
    // 19: hand edits (what the clicks do): add a note, move it, a chord note, remove one
    f.cam (L[19], [&ed] { return inEditor (ed, &genPanel (ed)->roll()).expanded (20); });
    f.at (L[19] + 0.8, [&p, &ed] { setP (p, "seq.s16.on", 1.0f); setP (p, "seq.s16.note", 7.0f); genPanel (ed)->selectStep (15); });
    f.at (L[19] + 2.4, [&p] { setP (p, "seq.s16.note", 9.0f); });
    f.at (L[19] + 4.0, [&p] { setP (p, "seq.s16.c1", 4.0f); });
    f.at (L[19] + 5.6, [&p] { setP (p, "seq.s16.c1", 0.0f); setP (p, "seq.s16.on", 0.0f); });
    hold (L[19] + 0.6, E[19] + 4.3, 60);
    // 20: MICRO
    f.cam (L[20], [] { return whole(); });
    f.at (L[20] + 0.2, [&p, &ed] { p.loadPresetIndex (presetIndex (p, "Winds", "Breathy Flute")); ed.microKey().onClick(); setP (p, "gen.micro", 1.0f); });
    f.cam (L[20] + 1.5, [&ed] { return genView (ed); });
    f.at (L[20] + 1.0, [&p, &ed] { for (int i = 0; i < 12; ++i) { genPanel (ed)->key ("ROLL")->onClick(); if (microSteps (p) > 0) break; } });
    f.expect (L[20] + 1.3, "MICRO: raised / lowered notes written", [&p] { return microSteps (p) > 0 ? juce::String() : juce::String ("none"); });
    hold (L[20] + 1.2, E[20] + 4.8, 62);
    f.at (E[20] + 4.9, [&ed] { ed.normalKey().onClick(); });
    // 21: LAB
    f.at (L[21] + 0.2, load ("Sequence - Lab", "Guzheng Running Water"));
    f.at (L[21] + 0.8, roll ("ROLL"));
    f.expect (L[21] + 1.1, "the Lab is on and the roll uses it", [&p] { return p.labActive() && p.genWrites (0).contains ("LAB") ? juce::String() : "lab " + juce::String ((int) p.labActive()) + " " + p.genWrites (0); });
    hold (L[21] + 0.9, E[21] + 3.3, 60);
    // 22: the drum door
    f.at (L[22] + 0.2, [&p, &ed] { p.loadPresetIndex (presetIndex (p, "Init", "Init")); ed.drumKey().onClick(); });
    f.cam (L[22] + 0.6, [&ed] { return inEditor (ed, ed.drumDoorView()).expanded (10); });
    f.at (L[22] + 1.2, [&ed] { ed.drumDoorView()->drumGenView()->rollKey()->onClick(); });
    f.at (L[22] + 5.0, [&ed] { ed.drumDoorView()->drumGenView()->rollKey()->onClick(); });
    f.at (E[22] + 3.0, [&p] { p.genHistory (1, -1); });
    hold (L[22] + 1.3, E[22] + 5.3, 60);
    // 23: CLIP, 24: SAVE
    f.at (L[23] + 0.2, [&p, &ed] { p.loadPresetIndex (presetIndex (p, "Lead", "Bright Saw Lead")); ed.seqKey().onClick(); });
    f.at (L[23] + 0.8, roll ("ROLL"));
    f.cam (L[23] + 0.8, [&ed] { return inEditor (ed, ed.seqDoorView()).withHeight (190.0f).expanded (10); });
    hold (L[23] + 0.9, E[23] + 2.3, 60);
    f.at (L[24] + 0.8, [&p] { p.saveUserPreset ("Lead", "My Generated Line"); });
    f.expect (L[24] + 1.1, "saved as Lead / My Generated Line", savedAs (p, "Lead", "My Generated Line"));
    f.expect (L[24] + 1.2, "the saved preset carries its seeds", [&p]
    {
        const auto file = JingFangProcessor::userPresetFolder().getChildFile ("Lead").getChildFile ("My Generated Line.xml");
        return file.loadFileAsString().contains ("<gen unit=\"0\"") ? juce::String() : juce::String ("no <gen> line");
    });
    f.cam (L[24] + 1.0, [] { return whole(); });
}

// the learn film (owner 28 Oct: "jing fang'i komple anlatan bir video çek, bir tür learn videosu gibi"): 16:9, the whole instrument
void e12 (Film& f, JingFangProcessor& p, JingFangEditor& ed)
{
    auto& L = f.L; auto& E = f.E;
    p.genSeedRandomForTest (2810);
    auto load = [&p] (const char* fam, const char* name) { return [&p, fam, name] { p.loadPresetIndex (presetIndex (p, fam, name)); }; };
    // the names are copied into the camera (an initializer_list kept in a lambda points at a dead temporary: the 28 Oct crash at 64 s)
    auto sec = [&ed] (std::initializer_list<const char*> names)
    {
        std::vector<juce::String> ns (names.begin(), names.end());
        return [&ed, ns] { Rect r; for (auto& n : ns) r = r.isEmpty() ? section (ed, n) : r.getUnion (section (ed, n)); return r.expanded (30); };
    };
    auto loaded = [&f, &p] (double t, juce::String fam, juce::String name) { f.expect (t, name + " is loaded", presetIs (p, fam, name)); };
    auto chords = [&f] (double t0, double t1, double every, std::initializer_list<std::initializer_list<int>> prog, int vel = 88)
    {
        std::vector<std::vector<int>> pr; for (auto& c : prog) pr.push_back (c);
        int i = 0; for (double t = t0; t < t1 - 0.4; t += every, ++i) { const auto& c = pr[(size_t) i % pr.size()]; for (int k : c) f.note (t, std::min (every - 0.1, t1 - t), k, vel); }
    };
    auto line = [&f] (double t0, double t1, double every, std::initializer_list<int> keys, int vel = 96)
    {
        std::vector<int> ks (keys); int i = 0; for (double t = t0; t < t1 - 0.2; t += every, ++i) f.note (t, every * 0.85, ks[(size_t) i % ks.size()], vel);
    };
    auto drag = [&ed] (juce::Point<float> from, juce::Point<float> to)
    {
        auto& bay = ed.patchBay();
        const auto t0 = juce::Time::getCurrentTime();
        auto src = juce::Desktop::getInstance().getMainMouseSource();
        bay.mouseDown (juce::MouseEvent (src, from, {}, 1.0f, 0, 0, 0, 0, &bay, &bay, t0, from, t0, 1, false));
        for (int s = 1; s <= 4; ++s) { const auto q = from + (to - from) * (float) s / 4.0f; bay.mouseDrag (juce::MouseEvent (src, q, {}, 1.0f, 0, 0, 0, 0, &bay, &bay, t0, from, t0, 1, false)); }
        bay.mouseUp (juce::MouseEvent (src, to, {}, 1.0f, 0, 0, 0, 0, &bay, &bay, t0, from, t0, 1, false));
    };

    // 1: intro
    f.at (0, load ("Pad", "Warm Pad"));
    f.cam (0, [] { return whole(); });
    chords (0.9, E[1] + 1.4, 2.0, { { 50, 57, 62, 65 }, { 46, 53, 58, 62 }, { 48, 55, 60, 64 }, { 45, 52, 57, 60 } });
    // 2: presets: the arrows
    f.cam (L[2], [] { return strip(); });
    f.at (L[2] + 1.2, [&p] { p.stepPreset (1); });
    f.at (L[2] + 3.0, [&p] { p.stepPreset (1); });
    f.at (E[2] + 1.6, [&p] { p.stepPreset (-2); });
    chords (L[2] + 0.2, E[2] + 3.9, 1.8, { { 50, 57, 62, 65 }, { 48, 55, 60, 64 } });
    // 3: categories: a sound of each
    struct V { const char* fam; const char* name; std::initializer_list<int> keys; };
    static const V tour[] = { { "Lead", "Bright Saw Lead", { 69 } }, { "Pad", "Glass Pad", { 57, 64, 69 } }, { "Keys - E-Piano", "Night EP", { 57, 60, 64 } },
                              { "Bass", "Round Mono", { 38 } }, { "Pluck", "Harp Pluck", { 62, 69 } }, { "Strings", "Orchestral Strings", { 50, 57, 62 } },
                              { "Bell", "FM Bell", { 74 } }, { "Drums - Kick", "Punch Kick", { 36 } }, { "Kanun", "Kanun", { 62 } }, { "Sitar", "Sitar", { 57 } }, { "Guzheng", "Guzheng", { 64 } } };
    {
        const double step = (E[3] + 4.8 - L[3]) / 11.0;
        for (int k = 0; k < 11; ++k)
        {
            const double t = L[3] + k * step;
            f.at (t, load (tour[k].fam, tour[k].name));
            for (int key : tour[k].keys) f.note (t + 0.1, step - 0.25, key, 96);
        }
        loaded (L[3] + 0.3, "Lead", "Bright Saw Lead");
    }
    // 4-5: the preset synthesizer
    f.at (L[4] + 0.1, load ("Keys - E-Piano", "Tine EP"));
    f.cam (L[4], [] { return strip(); });
    chords (L[4] + 0.3, E[5] + 4.8, 2.0, { { 50, 57, 60, 65 }, { 48, 55, 59, 64 }, { 45, 52, 57, 60 }, { 47, 53, 57, 62 } });
    f.at (L[5] + 0.6, [&p] { p.loadSection (0, presetIndex (p, "Lead", "Supersaw Lead")); });
    f.at (L[5] + 2.2, [&p] { p.loadSection (4, presetIndex (p, "Pad", "Slow Pad")); });
    f.at (L[5] + 3.8, [&p] { p.loadSection (3, presetIndex (p, "Bass", "Acid Line")); });
    f.expect (L[5] + 4.1, "three panels from three presets", [&p]
    {
        return p.sectionSlot (0) == presetIndex (p, "Lead", "Supersaw Lead") && p.sectionSlot (4) == presetIndex (p, "Pad", "Slow Pad")
            && p.sectionSlot (3) == presetIndex (p, "Bass", "Acid Line") ? juce::String() : juce::String ("slots not set");
    });
    // 6-8: oscillators, unison, FM
    f.at (L[6] + 0.1, load ("Lead", "Bright Saw Lead"));
    f.cam (L[6], sec ({ "OSC 1", "OSC 2", "OSC 3" }));
    line (L[6] + 0.3, E[8] + 3.9, 0.45, { 62, 65, 69, 72, 69, 65 });
    f.cam (L[7], sec ({ "OSC 1" }));
    f.at (L[7] + 0.5, [&p] { setP (p, "osc1.unison", 1.0f); });
    f.at (L[7] + 2.0, [&p] { setP (p, "osc1.unison", 7.0f); setP (p, "osc1.unison_detune", 40.0f); });
    f.cam (L[8], sec ({ "FM", "OSC 2" }));
    f.at (L[8] + 0.2, load ("Bell", "FM Bell"));
    for (int k = 0; k <= 10; ++k) f.at (L[8] + 0.6 + k * 0.3, [&p, k] { setP (p, "fm.amount", 0.1f * (float) k); });
    // 9: filter sweep
    f.at (L[9] + 0.1, load ("Bass", "Acid Line"));
    f.cam (L[9], sec ({ "FILTER" }));
    line (L[9] + 0.3, E[9] + 3.9, 0.22, { 38, 38, 50, 38, 45, 38, 48, 50 }, 100);
    for (int k = 0; k <= 16; ++k) f.at (L[9] + 0.4 + k * 0.35, [&p, k] { setP (p, "filter.cutoff", 200.0f * std::pow (2.0f, (float) (k <= 8 ? k : 16 - k) * 0.6f)); });
    // 10: envelopes
    f.at (L[10] + 0.1, load ("Pad", "Slow Pad"));
    f.cam (L[10], sec ({ "AMP ENVELOPE", "FILTER ENVELOPE" }));
    chords (L[10] + 0.3, E[10] + 3.9, 2.4, { { 50, 57, 62 }, { 48, 55, 60 } });
    f.at (L[10] + 2.6, [&p] { setP (p, "amp_env.attack", 0.01f); });
    // 11: pitch, click, drum
    f.at (L[11] + 0.1, load ("Drums - Kick", "Punch Kick"));
    f.cam (L[11], sec ({ "PITCH  CLICK  DRUM" }));
    loaded (L[11] + 0.3, "Drums - Kick", "Punch Kick");
    line (L[11] + 0.3, E[11] + 3.4, 0.5, { 36 }, 110);
    // 12: drift, floor
    f.at (L[12] + 0.1, load ("Pad", "Warm Drift Pad"));
    f.cam (L[12], sec ({ "DRIFT  FLOOR" }));
    f.chord (L[12] + 0.3, E[12] + 2.6 - L[12], { 50, 57, 62, 65 }, 90);
    // 13: voice: legato glide
    f.at (L[13] + 0.1, load ("Lead", "Glide Sine Lead"));
    f.cam (L[13], sec ({ "VOICE  OUTPUT" }));
    { static const int g[] = { 62, 69, 67, 74, 72, 69, 67, 62 }; for (int k = 0; k < 8; ++k) f.note (L[13] + 0.3 + k * 0.6, 0.75, g[k], 96); }
    // 14: effects
    f.at (L[14] + 0.1, load ("Pluck", "Glass Pluck"));
    f.cam (L[14], sec ({ "DRIVE", "ECHO", "REVERB" }));
    line (L[14] + 0.3, E[14] + 3.9, 0.5, { 62, 69, 74, 72, 69, 65 });
    f.at (L[14] + 1.0, [&p] { setP (p, "fx.echo.mix", 0.45f); setP (p, "fx.echo.feedback", 0.5f); });
    f.at (L[14] + 2.6, [&p] { setP (p, "fx.reverb.mix", 0.4f); });
    // 15: FX RACK
    f.at (L[15] + 0.2, [&ed] { ed.openDoorForTest (5); });
    f.cam (L[15] + 0.6, [&ed] { return ed.fxDoorView() != nullptr ? inEditor (ed, ed.fxDoorView()).expanded (20) : whole(); });
    f.at (L[15] + 1.6, [&p] { setP (p, "fx.mod.type", 1.0f); setP (p, "fx.mod.mix", 0.5f); });
    f.at (L[15] + 3.2, [&p] { setP (p, "fx.comp.ratio", 4.0f); setP (p, "fx.comp.threshold", -20.0f); });
    line (L[15] + 0.3, E[15] + 3.9, 0.5, { 62, 69, 74, 72, 69, 65 });
    // 16-17: PATCH
    f.at (L[16] + 0.1, [&ed] { ed.closeKey().onClick(); });
    f.at (L[16] + 0.2, load ("Lead", "VA Lead"));
    f.at (L[16] + 0.6, [&ed] { ed.patchKey().onClick(); });
    f.expect (L[16] + 1.3, "PATCH door open", doorOpen (ed, "PATCH"));
    f.cam (L[16] + 0.6, [&ed] { return inEditor (ed, &ed.patchBay()); });
    chords (L[16] + 0.3, E[17] + 3.9, 1.8, { { 57, 64 }, { 59, 66 }, { 61, 68 } });
    auto cables = std::make_shared<int> (0);
    f.at (L[17] + 0.4, [&ed, cables] { *cables = ed.patchBay().cableCount(); });
    f.at (L[17] + 0.6, [&ed, drag] { drag (ed.patchBay().jack (true, (int) svl::ModSrc::lfo1 - 1), ed.patchBay().jack (false, (int) svl::ModDst::cutoff - 1)); });
    f.expect (L[17] + 0.9, "a dragged cable is one more cable", [&ed, cables] { return ed.patchBay().cableCount() == *cables + 1 ? juce::String() : "cables " + juce::String (ed.patchBay().cableCount()); });
    f.at (L[17] + 1.0, [&p] { for (int m = 1; m <= svl::kNumModSlots; ++m) if (juce::roundToInt (raw (p, "mod" + juce::String (m) + ".destination")) == (int) svl::ModDst::cutoff) { setP (p, "mod" + juce::String (m) + ".amount", 0.8f); break; } setP (p, "lfo1.rate", 4.0f); });
    f.at (E[17] + 2.8, [&ed, drag] { const auto j = ed.patchBay().jack (false, (int) svl::ModDst::cutoff - 1); drag (j, j + juce::Point<float> (-160.0f, 120.0f)); });
    // 18-21: the keyboard and tuning
    f.at (L[18] + 0.1, [&ed] { ed.closeKey().onClick(); });
    f.at (L[18] + 0.2, load ("Strings", "Legato Strings"));
    f.cam (L[18] + 0.3, [&ed] { return inEditor (ed, &ed.keyboardView()).expanded (30, 60); });
    { static const int mk[] = { 62, 64, 65, 67, 69, 71, 72, 74, 72, 71, 69, 67, 65, 64, 62, 64 }; int i = 0; for (double t = L[18] + 0.4; t < E[21] + 3.2; t += 0.55, ++i) f.note (t, 0.5, mk[i % 16], 96); }
    f.at (L[19] + 0.2, [&ed] { ed.normalKey().onClick(); });
    f.at (L[20] + 0.3, [&ed] { ed.microKey().onClick(); });
    f.expect (L[20] + 0.6, "MICRO is on", paramIs (p, "tuning.mode", 1.0f));
    f.at (L[20] + 1.5, [&p] { setP (p, "tuning.row.e", 0.0f); });
    f.at (L[20] + 3.0, [&p] { setP (p, "tuning.row.b", 0.0f); });
    f.cam (L[21], sec ({ "TUNING" }));
    f.at (E[21] + 3.3, [&p, &ed] { setP (p, "tuning.row.e", 1.0f); setP (p, "tuning.row.b", 1.0f); ed.normalKey().onClick(); });
    // 22-23: the Tuning Lab, a Lab melody with its tuning, morph
    f.at (L[22] + 0.2, [&ed] { ed.labKey().onClick(); });
    f.expect (L[22] + 0.9, "LAB door open", doorOpen (ed, "LAB"));
    f.cam (L[22] + 0.3, [&ed] { return inEditor (ed, ed.labDoorView()); });
    line (L[22] + 0.4, E[22] + 3.3, 0.5, { 62, 63, 66, 67, 69, 67, 66, 63 });
    f.at (L[23] + 0.1, [&ed] { ed.closeKey().onClick(); });
    f.at (L[23] + 0.2, load ("Sequence - Lab", "Guzheng Running Water"));
    f.expect (L[23] + 0.6, "the Lab melody switched the Lab on", [&p] { return p.labActive() ? juce::String() : juce::String ("lab off"); });
    f.cam (L[23] + 0.3, [] { return strip(); });
    f.note (L[23] + 0.5, E[23] + 5.2 - (L[23] + 0.5), 60, 100);
    f.at (L[23] + 3.0, [&p] { auto ls = p.labState(); ls.b = jflab::Tuning(); ls.morph = 0; p.setLabState (ls); });
    for (int k = 0; k < 12; ++k) f.at (L[23] + 3.2 + k * 0.25, [&p, k] { auto ls = p.labState(); ls.morph = (float) (k + 1) / 12.0f; p.setLabState (ls); });
    // 24-28: SEQ: arp, sequencer, HOLD / PLAY, GENERATE
    f.at (L[24] + 0.1, [&p] { auto ls = p.labState(); ls.on = false; ls.morph = 0; p.setLabState (ls); });
    f.at (L[24] + 0.2, load ("Arp - Classic", "Up Down Two Octaves"));
    f.at (L[24] + 0.5, [&ed] { ed.seqKey().onClick(); });
    f.expect (L[24] + 1.2, "SEQ door open", doorOpen (ed, "SEQ"));
    f.cam (L[24] + 0.5, [&ed] { return inEditor (ed, ed.seqDoorView()); });
    f.chord (L[25] + 0.2, E[25] + 4.8 - (L[25] + 0.2), { 60, 64, 67 }, 96);
    f.at (L[26] + 0.1, load ("Sequence - Melodic", "Lead Riff"));
    f.cam (L[26] + 0.3, [&ed] { return inEditor (ed, &genPanel (ed)->roll()).expanded (20); });
    f.note (L[26] + 0.4, E[26] + 4.8 - (L[26] + 0.4), 60, 100);
    f.at (L[27] + 0.5, [&ed] { if (auto* k = doorKey (ed.seqDoorView(), "PLAY")) k->onClick(); });
    f.expect (L[27] + 1.5, "PLAY runs the sequence with no key", [&p] { return p.engineForTest().noteGen().isRunning() ? juce::String() : "not running"; });
    f.at (E[27] + 3.6, [&ed] { if (auto* k = doorKey (ed.seqDoorView(), "PLAY")) k->onClick(); });
    f.cam (L[28], [&ed] { return genView (ed); });
    f.at (L[28] + 0.5, [&ed] { genPanel (ed)->key ("ROLL")->onClick(); });
    f.at (E[28] + 1.5, [&ed] { genPanel (ed)->key ("ROLL")->onClick(); });
    f.note (L[28] + 0.6, E[28] + 4.8 - (L[28] + 0.6), 60, 100);
    // 29-31: DRUM
    f.at (L[29] + 0.1, load ("Drum Machine", "Boom Bap"));
    f.at (L[29] + 0.3, [&ed] { ed.drumKey().onClick(); });
    f.expect (L[29] + 1.0, "DRUM door open", doorOpen (ed, "DRUM"));
    f.cam (L[29] + 0.3, [&ed] { return inEditor (ed, ed.drumDoorView()); });
    f.at (L[30] + 0.4, [&ed] { if (auto* k = stepKey (ed.drumDoorView(), "drums.t3.s4")) click (*k); });
    f.at (L[30] + 1.6, [&ed] { if (auto* k = stepKey (ed.drumDoorView(), "drums.t3.s4")) click (*k); });
    f.at (L[30] + 3.0, [&p] { setP (p, "drums.t1.tune", -3.0f); });
    f.note (L[30] + 0.3, E[30] + 4.3 - (L[30] + 0.3), 60, 100);
    f.note (L[31] + 0.3, E[31] + 4.8 - (L[31] + 0.3), 60, 100);
    f.at (L[31] + 2.5, [&ed] { ed.drumDoorView()->drumGenView()->rollKey()->onClick(); });
    f.expect (L[31] + 1.5, "holding a key runs the beat", [&p] { return drumsRunning (p) ? juce::String() : "not running"; });
    // 32-34: the DAW
    f.cam (L[32], [&ed] { return inEditor (ed, ed.drumDoorView()).withHeight (110.0f).expanded (10).getUnion (inEditor (ed, &ed.bpmReadout()).expanded (10)); });
    f.at (L[33] + 0.8, [&p] { setP (p, "daw.drums", 1.0f); });
    f.at (E[33] + 1.8, [&p] { setP (p, "daw.drums", 0.0f); });
    f.cam (L[34], [&ed] { return inEditor (ed, ed.drumDoorView()).withHeight (110.0f).expanded (10); });
    // 35: MPE (played as notes here; the controller's per-note bends are in plugin_check)
    f.at (L[35] + 0.1, [&ed] { ed.closeKey().onClick(); });
    f.at (L[35] + 0.2, load ("Lead", "VA Lead"));
    f.cam (L[35] + 0.3, [&ed] { return inEditor (ed, &ed.keyboardView()).expanded (30, 60); });
    line (L[35] + 0.4, E[35] + 3.3, 0.5, { 62, 65, 69, 72 });
    // 36: SAVE
    f.cam (L[36], [] { return strip(); });
    f.at (L[36] + 0.8, [&p] { p.saveUserPreset ("Lead", "My Learn Sound"); });
    f.expect (L[36] + 1.1, "saved as Lead / My Learn Sound", savedAs (p, "Lead", "My Learn Sound"));
    // 37: help
    f.at (L[37] + 0.3, [&ed] { ed.helpKey().onClick(); });
    f.cam (L[37] + 0.3, [] { return whole(); });
    f.expect (L[37] + 0.8, "help is open", [&ed] { return ed.helpOverlay().isOpen() ? juce::String() : juce::String ("closed"); });
    f.at (E[37] + 3.5, [&ed] { ed.helpOverlay().close(); });
    // 38: outro
    f.at (L[38] + 0.1, load ("Pad", "Warm Pad"));
    chords (L[38] + 0.2, E[38] + 3.0, 2.0, { { 50, 57, 62, 65 }, { 48, 55, 60, 64 } });
}

// ---------------------------------------------------------------- the site's opening film (19 Oct, the launch session for the owner:
// "ru-8 intro sayfasından çıkarıyoruz, bu defa introya jing fang geliyor, benzer bir intro videosu çekelim"): 7.5 s, silent, no type
// burned in. Close on the three-row keyboard while a line plays on the MICRO rows, then the camera glides back to the whole panel with
// its dragons (settled by 6.4 s) and holds; wide: the panel ends in the right 60 %, tall: in the upper 60 % (the site writes its text
// over the calm ground).
void introFilm (Film& f, JingFangProcessor& p, JingFangEditor& ed)
{
    f.name = "Intro"; f.length = 7.5;
    f.at (0, [&p, &ed]
    {
        p.loadPresetIndex (presetIndex (p, "Strings", "Legato Strings"));
        ed.microKey().onClick();
        setP (p, "tuning.row.e", 0.0f); setP (p, "tuning.row.b", 0.0f); setP (p, "tuning.row.gb", 2.0f);
    });
    f.expect (0.2, "MICRO is on", [&p] { return raw (p, "tuning.mode") > 0.5f ? juce::String() : juce::String ("tuning.mode off"); });
    f.note (0.05, 7.4, 50, 70);                                      // a held D under the line
    const int line[] = { 62, 64, 66, 67, 69, 71, 69, 67, 66, 64, 62, 64 };
    for (int i = 0; i < 12; ++i) f.note (0.1 + i * 0.6, 0.56, line[i], 92);
}

// the 3D intro, second version (launch session, 19 Oct; owner: "microtonal özellik ve hicaz bir melodi olsa iyi olurdu"): 6 s, a Hicaz
// phrase on D on the MICRO rows: Eb one step of 53 up (122.6 ct; AEU dik kürdi 113), F# one step down (377.4 ct; nim hicaz 385),
// the rest as usual; Winds / Breathy Flute; the first note at 0.15 s, the raised Eb by 0.6 s
void hicazFilm (Film& f, JingFangProcessor& p, JingFangEditor& ed)
{
    f.name = "Hicaz"; f.length = 6.0;
    f.at (0, [&p, &ed]
    {
        p.loadPresetIndex (presetIndex (p, "Winds", "Breathy Flute"));
        ed.microKey().onClick();
        setP (p, "tuning.row.eb", 2.0f); setP (p, "tuning.row.gb", 0.0f);
    });
    f.expect (0.1, "MICRO on, Eb high and F# low", [&p]
    {
        return raw (p, "tuning.mode") > 0.5f && juce::roundToInt (raw (p, "tuning.row.eb")) == 2 && juce::roundToInt (raw (p, "tuning.row.gb")) == 0
             ? juce::String() : juce::String ("rows not set");
    });
    // D Eb F# G A | G F# Eb | D Eb F# G | F# Eb D (held)
    struct N { double t, len; int key; };
    const N line[] = { { 0.15, 0.40, 62 }, { 0.58, 0.40, 63 }, { 1.00, 0.40, 66 }, { 1.42, 0.40, 67 }, { 1.84, 0.70, 69 },
                       { 2.58, 0.36, 67 }, { 2.96, 0.36, 66 }, { 3.34, 0.60, 63 },
                       { 3.98, 0.32, 62 }, { 4.32, 0.32, 63 }, { 4.66, 0.32, 66 }, { 5.00, 0.30, 63 }, { 5.32, 0.62, 62 } };
    for (auto& n : line) f.note (n.t, n.len, n.key, 96);
    f.expect (0.75, "the raised Eb sounds a step of 53 above Eb", [&p]
    {
        const float v = voicePitchFor (p, 63);
        return v >= 0 && std::abs (v - (63.0f + 12.0f / 53.0f)) < 0.01f ? juce::String() : "Eb plays " + num (v, 3);
    });
    f.expect (1.2, "the lowered F# sounds a step of 53 below F#", [&p]
    {
        const float v = voicePitchFor (p, 66);
        return v >= 0 && std::abs (v - (66.0f - 12.0f / 53.0f)) < 0.01f ? juce::String() : "F# plays " + num (v, 3);
    });
}

// the product page's stills (launch session, owner 19 Oct: "jing fang için ürün sayfasını hazırla ve vitrine ekle"): the whole
// editor at 200 %, straight from the editor (store rule: the real panel), in six states
juce::File siteOut;
void siteShot (JingFangEditor& ed, const juce::String& name)
{
    const auto img = ed.createComponentSnapshot (ed.getLocalBounds(), true, 2.0f);
    const auto f = siteOut.getChildFile (name + ".png");
    f.deleteFile();
    juce::PNGImageFormat png;
    if (auto os = std::unique_ptr<juce::FileOutputStream> (f.createOutputStream())) png.writeImageToStream (img, *os);
}
void siteFilm (Film& f, JingFangProcessor& p, JingFangEditor& ed)
{
    f.name = "Site"; f.length = 16.0;
    // 1 main: Warm Pad, a chord held
    f.at (0, [&p] { p.loadPresetIndex (presetIndex (p, "Pad", "Warm Pad")); });
    f.chord (0.3, 1.6, { 50, 57, 62, 65 }, 90);
    f.at (1.2, [&ed] { siteShot (ed, "panel_main"); });
    // 2 micro: the Hicaz rows, Eb and F# moved, a phrase held on them
    f.at (2.0, [&p, &ed] { p.loadPresetIndex (presetIndex (p, "Winds", "Breathy Flute")); ed.microKey().onClick(); setP (p, "tuning.row.eb", 2.0f); setP (p, "tuning.row.gb", 0.0f); });
    f.chord (2.4, 1.2, { 62, 63, 66 }, 90);
    f.at (3.2, [&ed] { siteShot (ed, "panel_micro"); });
    f.at (3.8, [&p, &ed] { ed.normalKey().onClick(); setP (p, "tuning.row.eb", 1.0f); setP (p, "tuning.row.gb", 1.0f); });
    // 3 doors, one per image (each with a musical preset that fills it)
    f.at (4.2, [&p, &ed] { p.loadPresetIndex (presetIndex (p, "Lead", "VA Lead")); ed.patchKey().onClick(); });
    f.at (4.6, [&ed] { ed.patchBay().connect (1, 11); ed.patchBay().connect (2, 1); });
    f.chord (4.8, 1.0, { 57, 64 }, 90);
    f.at (5.6, [&ed] { siteShot (ed, "panel_patch"); });
    f.at (6.0, [&p, &ed] { ed.closeKey().onClick(); p.loadPresetIndex (presetIndex (p, "Sequence - Acid", "Climbing Acid")); });
    f.at (6.4, [&ed] { ed.seqKey().onClick(); });
    f.at (6.8, [&ed] { if (auto* k = doorKey (ed.seqDoorView(), "PLAY")) k->onClick(); });
    f.at (8.0, [&ed] { siteShot (ed, "panel_seq"); });
    f.at (8.2, [&ed] { if (auto* k = doorKey (ed.seqDoorView(), "PLAY")) k->onClick(); ed.closeKey().onClick(); });
    f.at (8.8, [&p, &ed] { p.loadPresetIndex (presetIndex (p, "Drum Machine", "Boom Bap")); ed.drumKey().onClick(); });
    f.at (9.2, [&ed] { if (auto* k = doorKey (ed.drumDoorView(), "PLAY")) k->onClick(); });
    f.at (10.4, [&ed] { siteShot (ed, "panel_drum"); });
    f.at (10.6, [&ed] { if (auto* k = doorKey (ed.drumDoorView(), "PLAY")) k->onClick(); ed.closeKey().onClick(); });
    f.at (11.2, [&p, &ed] { p.loadPresetIndex (presetIndex (p, "Pluck", "Concert Harp")); ed.labKey().onClick(); });
    f.at (11.6, [&p]
    {
        JingFangProcessor::LabState ls; ls.on = true;
        const auto scl = juce::File::getCurrentWorkingDirectory().getChildFile ("out/tuning/hicaz_lead_learned.scl");
        jflab::scala::readScl (scl.loadFileAsString().toStdString(), ls.a);
        ls.a.name = "Learned: solo line";   // neutral names on the panel and in public images (SPEC) ls.a.middleKey = ls.a.refKey = 62; ls.a.refHz = 293.6648;
        p.setLabState (ls);
    });
    f.chord (12.0, 1.5, { 62, 63, 66 }, 90);
    f.at (13.0, [&ed] { siteShot (ed, "panel_lab"); });
    f.at (13.4, [&ed] { ed.closeKey().onClick(); });
}

// ---------------------------------------------------------------- the tester
struct Agent
{
    struct Finding { juce::String kind, what; double first = 0, last = 0; int count = 0; };
    std::map<juce::String, Finding> found;
    std::map<juce::String, long> ran;
    std::vector<std::pair<double, juce::String>> expectLog;
    int pass = 0, fail = 0;
    void hit (double t, const juce::String& kind, const juce::String& key, const juce::String& what)
    {
        auto& f = found[kind + "|" + key];
        if (f.count++ == 0) { f.kind = kind; f.what = what; f.first = t; }
        f.last = t;
    }
    void check (const char* name) { ++ran[name]; }
};

juce::String clock (double t) { return juce::String::formatted ("%d:%04.1f", (int) t / 60, std::fmod (t, 60.0)); }

void writeReport (const juce::File& file, const Film& film, const Agent& a, int frames)
{
    juce::String r;
    r << "# " << film.stem << " - " << film.name << "\n\n";
    r << "Rendered " << juce::Time::getCurrentTime().toString (true, true) << " by tests/demo_render (the player checks the device while it plays it). "
      << frames << " frames, " << num (film.length, 1) << " s.\n\n";
    r << "## Expectations of the script: " << a.pass << " held, " << a.fail << " failed\n\n";
    for (auto& [t, line] : a.expectLog) r << "- " << clock (t) << "  " << line << "\n";
    r << "\n## Checks on every frame\n\n| check | times run |\n|---|---|\n";
    for (auto& [k, n] : a.ran) r << "| " << k << " | " << juce::String ((juce::int64) n) << " |\n";
    std::vector<const Agent::Finding*> list;
    for (auto& [k, f] : a.found) list.push_back (&f);
    std::sort (list.begin(), list.end(), [] (auto* x, auto* y) { return x->first < y->first; });
    r << "\n## Findings: " << (int) list.size() << "\n\n";
    if (list.empty()) r << "None.\n";
    for (auto* f : list)
        r << "- **" << f->kind << "** " << clock (f->first) << (f->count > 1 ? " ... " + clock (f->last) + " (" + juce::String (f->count) + " frames)" : juce::String())
          << ": " << f->what << "\n";
    file.replaceWithText (r);
}

// ---------------------------------------------------------------- frame composition
juce::Image loadImage (const juce::File& f) { return juce::ImageFileFormat::loadFrom (f); }

void drawCard (juce::Graphics& g, const juce::Image& box, float fade, const juce::String& line1, const juce::String& line2)
{
    g.fillAll (vertical ? kGround : juce::Colour (0xff0B0C0C));
    if (box.isValid())
    {
        const float h = vertical ? (float) W * 0.98f * (float) box.getHeight() / (float) box.getWidth() : (float) H * 0.86f,
                    w = h * (float) box.getWidth() / (float) box.getHeight();
        g.setOpacity (fade);
        g.drawImage (box, Rect (w, h).withCentre ({ W * 0.5f, H * (vertical ? 0.42f : 0.47f) }));
        g.setOpacity (1.0f);
    }
    g.setColour ((vertical ? kCream : juce::Colour (0xffECE0C8)).withAlpha (fade));
    g.setFont (jfui::heros (vertical ? 72.0f : 30.0f, true));
    g.drawText (line1, juce::Rectangle<int> (0, H - (vertical ? 560 : 120), W, vertical ? 90 : 40), juce::Justification::centred);
    g.setColour (kGold.withAlpha (fade));
    g.setFont (jfui::heros (vertical ? 44.0f : 20.0f, false));
    g.drawText (line2, juce::Rectangle<int> (0, H - (vertical ? 450 : 78), W, vertical ? 60 : 30), juce::Justification::centred);
}

// the approved lockup is an ink drawing on cream (Assets/Brand/logo): its darkness becomes the alpha of a cream mark, cropped
// to the drawing (as RU-8's covers re-tint it); never redrawn
juce::Image creamLockup (const juce::Image& src)
{
    if (! src.isValid()) return {};
    juce::Image out (juce::Image::ARGB, src.getWidth(), src.getHeight(), true);
    juce::Rectangle<int> box;
    for (int y = 0; y < src.getHeight(); ++y)
        for (int x = 0; x < src.getWidth(); ++x)
        {
            const auto c = src.getPixelAt (x, y);
            const float lum = 0.299f * c.getRed() + 0.587f * c.getGreen() + 0.114f * c.getBlue();
            const auto a = (juce::uint8) juce::jlimit (0, 255, juce::roundToInt ((236.0f - lum) * 255.0f / 196.0f));
            if (a > 24) box = box.isEmpty() ? juce::Rectangle<int> (x, y, 1, 1) : box.getUnion ({ x, y, 1, 1 });
            out.setPixelAt (x, y, kCream.withAlpha (a));
        }
    return box.isEmpty() ? out : out.getClippedImage (box);
}

Rect fitAspect (Rect r, float aspect)                                 // grow r to the view's aspect, centred
{
    if (r.getWidth() / r.getHeight() > aspect) return r.withSizeKeepingCentre (r.getWidth(), r.getWidth() / aspect);
    return r.withSizeKeepingCentre (r.getHeight() * aspect, r.getHeight());
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    if (argc < 3) { std::fprintf (stderr, "usage: demo_render <1|2|3|e1..e8> <out_dir> [--vertical]\n"); return 2; }
    const juce::String which (argv[1]);
    const bool isExplainer = which.startsWith ("e");
    wideExplainer = isExplainer && argc > 3 && juce::String (argv[3]) == "--wide";
    vertical = (isExplainer && ! wideExplainer) || (argc > 3 && juce::String (argv[3]) == "--vertical");
    if (vertical) { W = 1080; H = 1920; }
    if (which == "intro") { introMode = argc > 3 && juce::String (argv[3]) == "tall" ? 2 : 1; vertical = false; W = introMode == 1 ? 1600 : 720; H = introMode == 1 ? 900 : 1280; }
    if (which == "site") { introMode = 5; vertical = false; W = 320; H = 180; siteOut = juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]); }
    if (which == "face")
    {   // the 3D intro's face texture (the launch session, 19 Oct): the device body from layout_v3.json "device" (+3 units for the red
        // sides) and the preset strip ("synth"), at 200 %, the dragon layer off
        introMode = 3; vertical = false;
        const auto L = juce::JSON::parse (juce::File::getCurrentWorkingDirectory().getChildFile ("Assets/Panel/layout_v3.json"));
        auto r = [&L] (const char* k, float grow) { const auto& a = L[k]; return juce::Rectangle<float> ((float) (double) a[0], (float) (double) a[1], (float) (double) a[2], (float) (double) a[3])
                                                                              .expanded (grow).getSmallestIntegerContainer(); };
        faceRect = r ("device", 3.0f); stripRect = r ("synth", 2.0f);
        W = faceRect.getWidth() * 2; H = faceRect.getHeight() * 2;
    }
    const juce::File out = juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]);
    out.createDirectory();
    JingFangProcessor::userFolderOverrideForTests() = out.getChildFile ("user_presets_" + which);   // never the user's presets
    JingFangProcessor::userFolderOverrideForTests().deleteRecursively();

    JingFangProcessor proc;
    proc.setRateAndBufferSizeDetails (kSr, kSpf);
    proc.prepareToPlay (kSr, kSpf);
    std::unique_ptr<JingFangEditor> ed (static_cast<JingFangEditor*> (proc.createEditor()));
    ed->setSize (jfui::kW, jfui::kH);
    ed->keyboardView().stopOwnClock();
    proc.loadPresetIndex (presetIndex (proc, "Init", "Init"));

    Film film;
    const bool hicaz = introMode == 3 && argc > 3 && juce::String (argv[3]) == "hicaz";
    if (introMode == 5) { film.stem = "site"; siteFilm (film, proc, *ed); }
    else if (introMode == 3) { film.stem = "face"; if (hicaz) hicazFilm (film, proc, *ed); else introFilm (film, proc, *ed); ed->panelView().findChildWithID ("dragon")->setVisible (false); }
    else if (introMode != 0) { film.stem = introMode == 1 ? "jf-intro-wide" : "jf-intro-tall"; introFilm (film, proc, *ed); }
    else if (isExplainer)
    {
        const auto script = juce::JSON::parse (juce::File::getCurrentWorkingDirectory().getChildFile ("marketing/explainers/script.json"));
        film.stem = which; film.explainer = true; film.name = script[which.toRawUTF8()]["title"].toString();
        const int n = which.getTrailingIntValue();
        if (film.name.isEmpty() || n < 1 || n > 12) { std::fprintf (stderr, "no %s in script.json\n", which.toRawUTF8()); return 2; }
        explainerLines (film, script[which.toRawUTF8()]);
        void (*fns[]) (Film&, JingFangProcessor&, JingFangEditor&) = { e1, e2, e3, e4, e5, e6, e7, e8, e9, e10, e11, e12 };
        fns[n - 1] (film, proc, *ed);
    }
    else
    {
        const int n = which.getIntValue();
        film.stem = "film" + which + (vertical ? "_9x16" : "");
        if (n == 1) film1 (film, proc, *ed); else if (n == 2) film2 (film, proc, *ed); else film3 (film, proc, *ed);
    }
    std::stable_sort (film.evs.begin(), film.evs.end(), [] (const Ev& a, const Ev& b) { return a.t < b.t; });
    std::stable_sort (film.expects.begin(), film.expects.end(), [] (const Expect& a, const Expect& b) { return a.t < b.t; });
    std::stable_sort (film.cams.begin(), film.cams.end(), [] (const CamKey& a, const CamKey& b) { return a.t < b.t; });

    const auto root = juce::File::getCurrentWorkingDirectory();
    const auto box = loadImage (root.getChildFile ("marketing/mockups/jingfang_box_right.png"));
    const auto lockup = creamLockup (loadImage (root.getChildFile ("../../Assets/Brand/logo/lockup-horizontal-cream.png")));
    const auto video = out.getChildFile (film.stem + (introMode != 0 ? ".mp4" : "_video.mp4"));
    const juce::String cmd = "ffmpeg -hide_banner -loglevel error -y -f rawvideo -pix_fmt bgra -s " + juce::String (W) + "x" + juce::String (H)
                           + " -r " + juce::String (kFps) + " -i - -c:v libx264 -preset slow -crf " + (introMode != 0 ? "14" : "18")
                           + " -pix_fmt yuv420p -movflags +faststart \"" + video.getFullPathName() + "\"";
    FILE* pipe = popen (cmd.toRawUTF8(), "w");
    if (pipe == nullptr) { std::fprintf (stderr, "cannot start ffmpeg\n"); return 1; }

    const int frames = (int) (film.length * kFps);
    juce::AudioBuffer<float> audio (2, frames * kSpf);
    audio.clear();
    juce::Image frame (juce::Image::ARGB, W, H, true);
    const float devScale = 952.0f / (float) jfui::kH;                      // 16:9: the device 1460 x 952 above a 110 px caption band
    const int devW = juce::roundToInt ((float) jfui::kW * devScale), devX = (W - devW) / 2, devY = 14;
    const Rect view = wideExplainer ? Rect (0, 0, (float) W, (float) H - 170) : Rect (0, 290, (float) W, 900);   // the camera's window (9:16, or 16:9 above the caption band)
    Rect cam = fitAspect (whole(), view.getWidth() / view.getHeight());
    size_t ev = 0, ex = 0, ck = 0;
    std::function<Rect()> camTarget = [] { return whole(); };
    std::vector<std::pair<double, int>> offs;                            // pending note-offs: time, key
    std::map<int, double> heldSince, lastOff;                            // key -> when its current note started / last note-off

    // the tester's state
    Agent agent;
    struct Pending { double t; int key; float peak; };
    std::vector<Pending> listen;                                         // notes waiting to be heard
    double lastHeld = -100, lastEvent = -100, lastBusy = -100;
    auto stats = proc.engineForTest().stats;
    std::map<juce::String, std::pair<float, double>> knobLast;           // knob: last parameter value, when it moved
    const auto& ids = JingFangProcessor::paramIds();
    std::vector<double> stillAt;                                          // the covers' pictures: the middle of every caption
    for (auto& c : film.caps) stillAt.push_back (0.5 * (c.t0 + c.t1));
    size_t st = 0;
    juce::String stillsJson;
    const auto stillDir = out.getChildFile (film.stem + "_stills");
    stillDir.deleteRecursively(); stillDir.createDirectory();

    for (int i = 0; i < frames; ++i)
    {
        const double t0 = (double) i / kFps, t1 = (double) (i + 1) / kFps;
        while (ev < film.evs.size() && film.evs[ev].t < t1) { film.evs[ev++].f(); lastEvent = t0; }
        while (ck < film.cams.size() && film.cams[ck].t < t1) camTarget = film.cams[ck++].rect;
        juce::MidiBuffer midi;
        for (auto& n : film.notes)
            if (n.t >= t0 && n.t < t1)
            {
                midi.addEvent (juce::MidiMessage::noteOn (1, n.key, (juce::uint8) n.vel), (int) ((n.t - t0) * kSr));
                offs.push_back ({ n.t + n.len, n.key });
                heldSince[n.key] = n.t;
                listen.push_back ({ n.t, n.key, 0.0f });
            }
        for (auto it = offs.begin(); it != offs.end();)
            if (it->first < t1) { midi.addEvent (juce::MidiMessage::noteOff (1, it->second), juce::jmax (0, (int) ((it->first - t0) * kSr))); lastOff[it->second] = it->first; it = offs.erase (it); }
            else ++it;
        juce::AudioBuffer<float> buf (2, kSpf);
        proc.processBlock (buf, midi);
        for (int c = 0; c < 2; ++c) audio.copyFrom (c, i * kSpf, buf, c, 0, kSpf);
        ed->tickForTest();
        ed->keyboardView().pollSounding ((juce::uint32) (t1 * 1000.0) + 1000);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (1);

        if (st < stillAt.size() && stillAt[st] < t1)
        {
            const auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f);
            const auto f = stillDir.getChildFile (juce::String::formatted ("%02d.png", (int) st + 1));
            juce::PNGImageFormat png;
            if (auto os = std::unique_ptr<juce::FileOutputStream> (f.createOutputStream())) png.writeImageToStream (img, *os);
            Rect tgt = camTarget ? camTarget() : whole();
            if (! film.explainer)
            {
                juce::Component* door = ed->patchBay().isOpenOrOpening() ? static_cast<juce::Component*> (&ed->patchBay())
                                      : ed->seqDoorView()->isOpenOrOpening() ? static_cast<juce::Component*> (ed->seqDoorView())
                                      : ed->drumDoorView()->isOpenOrOpening() ? static_cast<juce::Component*> (ed->drumDoorView())
                                      : ed->labDoorView()->isOpenOrOpening() ? static_cast<juce::Component*> (ed->labDoorView()) : nullptr;
                tgt = door != nullptr ? inEditor (*ed, door) : whole();
            }
            const auto& c = film.caps[st];
            stillsJson << (st > 0 ? ",\n " : " ") << "{\"file\": \"" << f.getFileName() << "\", \"t\": " << num (t0, 2) << ", \"cap\": " << juce::JSON::toString (c.text)
                       << ", \"sub\": " << juce::JSON::toString (c.sub) << ", \"focus\": [" << num (tgt.getX(), 1) << ", " << num (tgt.getY(), 1) << ", "
                       << num (tgt.getWidth(), 1) << ", " << num (tgt.getHeight(), 1) << "]}";
            ++st;
        }

        // ---- the tester: what must always hold
        const auto& eng = proc.engineForTest();
        const auto* dm = eng.drumMachine();
        const bool held = ! offs.empty();
        if (held) lastHeld = t1;
        const bool busy = eng.noteGen().isRunning() || (dm != nullptr && dm->isRunning());
        if (busy) lastBusy = t1;
        const double quiet = t1 - juce::jmax (lastHeld, lastBusy);      // no key held and no pattern running for this long
        float peak = 0; bool finite = true;
        for (int c = 0; c < 2; ++c) for (int s = 0; s < kSpf; ++s) { const float v = buf.getSample (c, s); finite &= std::isfinite (v); peak = juce::jmax (peak, std::abs (v)); }
        agent.check ("audio finite"); if (! finite) agent.hit (t0, "audio-nonfinite", "", "NaN or Inf in the output");
        agent.check ("peak under 0 dBFS"); if (peak > 1.0f) agent.hit (t0, "over-full-scale", "", "sample peak " + num (juce::Decibels::gainToDecibels (peak), 2)
                                                                       + " dBFS: JING FANG has no output limiter, a Standalone or a fixed-point host clips here (preset " + proc.currentFamily() + " / " + proc.currentName() + ")");
        agent.check ("engine counters (non-finite resets, dropped events, hard steals)");
        if (eng.stats.nonFiniteResets != stats.nonFiniteResets) agent.hit (t0, "engine-nonfinite-reset", "", "the engine reset a voice that went non-finite");
        if (eng.stats.droppedEvents != stats.droppedEvents) agent.hit (t0, "engine-dropped-event", "", "the engine dropped " + juce::String ((juce::int64) (eng.stats.droppedEvents - stats.droppedEvents)) + " events");
        if (eng.stats.hardSteals != stats.hardSteals) agent.hit (t0, "engine-hard-steal", "", "a sounding voice was cut to make room");
        stats = eng.stats;
        // a note must be heard within 250 ms
        agent.check ("every note is heard");
        for (auto it = listen.begin(); it != listen.end();)
        {
            it->peak = juce::jmax (it->peak, peak);
            if (t1 - it->t > 0.25)
            {
                if (it->peak < 1.0e-4f) agent.hit (it->t, "silent-note", juce::String (it->key), "note " + juce::String (it->key) + " sent, nothing above -80 dBFS for 250 ms"
                                                   + (dm != nullptr && dm->holdsKeys() ? " (drums hold the keys)" : ""));
                it = listen.erase (it);
            }
            else ++it;
        }
        // nothing left on after the keys are up and no pattern runs
        if (quiet > 0.4)                                                 // (a pattern's last note-off may land in the next block)
        {
            agent.check ("no gated voice after the keys are up");
            for (int v = 0; v < svl::kVoicePool; ++v)
            {
                const auto vi = eng.voiceInfo (v);
                if (vi.active && vi.gate) agent.hit (t0, "stuck-voice", num (vi.pitch, 1), "a voice at pitch " + num (vi.pitch, 2) + " still has its gate open "
                                                     + num (quiet, 1) + " s after the last key went up and the patterns stopped");
            }
            agent.check ("no key light after the keys are up");
            for (int row = 0; row < 3; ++row)
                for (int k = 0; k < 128; ++k)
                    if (proc.keyLit (row, k) || proc.keySounding (row, k))
                        agent.hit (t0, "stuck-light", juce::String (row) + "/" + juce::String (k), "key " + juce::String (k) + " row " + juce::String (row)
                                   + (proc.keyLit (row, k) ? " lit" : " shown sounding") + " with every key up");
        }
        if (quiet > 10.0) { agent.check ("sound dies 10 s after the keys and patterns stop"); if (peak > 0.0032f) agent.hit (t0, "stuck-sound", "", "output at " + num (juce::Decibels::gainToDecibels (peak), 1) + " dBFS, " + num (quiet, 1) + " s after the keys and patterns stopped"); }
        // a held key shows on the keyboard (arp / seq / drums change what sounds, so only when none runs)
        if (held && ! busy && (dm == nullptr || ! dm->holdsKeys()) && i % 3 == 0)
        {
            agent.check ("a held key is lit");
            for (auto& [tOff, k] : offs)
            {
                if (t0 - heldSince[k] < 0.12) continue;
                if (lastOff.count (k) && lastOff[k] > heldSince[k]) continue;   // an overlapping note of the same key was let go: MIDI lets the key up
                bool any = false;
                for (int row = 0; row < 3; ++row) any |= proc.keyLit (row, k) || proc.keySounding (row, k);
                if (! any)
                {
                    juce::String lit, voices;
                    for (int row = 0; row < 3; ++row) for (int kk = 0; kk < 128; ++kk) if (proc.keySounding (row, kk)) lit << row << "/" << kk << " ";
                    for (int v = 0; v < svl::kVoicePool; ++v) { const auto vi = eng.voiceInfo (v); if (vi.active && vi.gate) voices << num (vi.pitch, 3) << " "; }
                    agent.hit (t0, "light-missing", juce::String (k), "key " + juce::String (k) + " held, not lit on any row (gated voices at " + voices.trim()
                               + "; shown sounding " + lit.trim() + (proc.labActive() ? "; Lab on" : "") + ")");
                }
            }
        }
        // one door at a time
        agent.check ("one door at a time");
        {
            const int open = (int) ed->patchBay().isOpenOrOpening() + (int) ed->seqDoorView()->isOpenOrOpening() + (int) ed->drumDoorView()->isOpenOrOpening() + (int) ed->labDoorView()->isOpenOrOpening();
            if (open > 1) agent.hit (t0, "two-doors", "", juce::String (open) + " doors open at once");
        }
        // the panel's step LEDs follow the engine
        agent.check ("SEQ / DRUM step shown = engine step");
        if (eng.noteGen().isRunning() && proc.live().seqStep != eng.noteGen().currentStep())
            agent.hit (t0, "seq-step-shown", "", "panel step " + juce::String (proc.live().seqStep) + ", engine step " + juce::String (eng.noteGen().currentStep()));
        if (proc.drumStep() != eng.drumStep()) agent.hit (t0, "drum-step-shown", "", "panel step " + juce::String (proc.drumStep()) + ", engine step " + juce::String (eng.drumStep()));
        // the host values = what the engine plays (once the timeline has been still for 0.25 s)
        if (i % 10 == 0 && t0 - lastEvent > 0.25)
        {
            agent.check ("host value = engine value (all parameters)");
            for (int p = 0; p < svl::kNumParams; ++p)
            {
                const float h = raw (proc, ids[p]), e = proc.exactValue (p);
                if (std::abs (h - e) > 1e-4f * juce::jmax (1.0f, std::abs (e))) agent.hit (t0, "param-desync", ids[p], ids[p] + ": host " + num (h, 4) + ", engine " + num (e, 4));
            }
        }
        // the knobs' drawings settle on their value within 0.8 s (UI signature: the drawing glides, the value is at once)
        if (i % 3 == 0)
        {
            agent.check ("knob drawings settle");
            for (auto& k : ed->allKnobs())
            {
                const float v = k->parameterNorm();
                auto it = knobLast.find (k->paramId());
                if (it == knobLast.end() || std::abs (v - it->second.first) > 1e-6f) { knobLast[k->paramId()] = { v, t0 }; continue; }
                const double since = t0 - it->second.second;
                if (since > 0.8 && std::abs (k->drawn() - v) > 0.01f && ! k->gliding())
                    agent.hit (t0, "knob-drawing", k->paramId(), k->paramId() + " drawn at " + num (k->drawn(), 3) + ", value " + num (v, 3));
                else if (since > 2.0 && k->gliding()) agent.hit (t0, "knob-glide-long", k->paramId(), k->paramId() + " still gliding 2 s after its value moved");
            }
        }
        // the script's expectations
        while (ex < film.expects.size() && film.expects[ex].t < t1)
        {
            const auto& e = film.expects[ex++];
            const auto res = e.check();
            if (res.isEmpty()) { ++agent.pass; agent.expectLog.push_back ({ e.t, "held: " + e.what }); }
            else if (res.startsWith ("(")) agent.expectLog.push_back ({ e.t, "not judged: " + e.what + " " + res });
            else { ++agent.fail; agent.expectLog.push_back ({ e.t, "**FAILED**: " + e.what + " -> " + res }); agent.hit (e.t, "expect", e.what, e.what + ": " + res); }
        }

        // ---- the frame
        juce::Graphics g (frame);
        const double tEnd = film.length;
        const bool card = introMode == 0 && (film.explainer ? t0 > tEnd - 3.5 : (t0 < 3.5 || t0 > tEnd - 6.0));
        if (introMode == 3)
        {   // face_NNNN.png (the device body) and strip_NNNN.png (the preset strip), 200 %, straight from the editor
            const auto face = ed->createComponentSnapshot (faceRect, true, 2.0f);
            g.drawImageAt (face, 0, 0);
            juce::PNGImageFormat png;
            auto save = [&] (const juce::Image& img, const juce::String& n)
            {
                const auto f = out.getChildFile (n + juce::String::formatted ("_%04d.png", i + 1));
                f.deleteFile();
                if (auto os = std::unique_ptr<juce::FileOutputStream> (f.createOutputStream())) png.writeImageToStream (img, *os);
            };
            save (face, "face");
            save (ed->createComponentSnapshot (stripRect, true, 2.0f), "strip");
        }
        else if (introMode != 0)
        {   // the ground (brand ink, a little darker low), the view glides from the whole frame to its end place, the camera from
            // the keyboard to the whole panel; smootherstep from 0.5 s to 6.4 s, then still
            for (int y = 0; y < H; ++y) { g.setColour (kGround.interpolatedWith (juce::Colour (0xff032825), (float) y / (float) H)); g.fillRect (0, y, W, 1); }
            const double u = juce::jlimit (0.0, 1.0, (t0 - 0.5) / (6.4 - 0.5));
            const float e = (float) (u * u * u * (u * (u * 6 - 15) + 10));
            const Rect full (0, 0, (float) W, (float) H);
            const Rect endView = introMode == 1 ? Rect ((float) W * 0.40f, 0, (float) W * 0.57f, (float) H) : Rect (20, (float) H * 0.03f, (float) W - 40, (float) H * 0.58f);
            auto lerpR = [] (Rect a, Rect b, float k) { return Rect (a.getX() + (b.getX() - a.getX()) * k, a.getY() + (b.getY() - a.getY()) * k,
                                                                    a.getWidth() + (b.getWidth() - a.getWidth()) * k, a.getHeight() + (b.getHeight() - a.getHeight()) * k); };
            const Rect v = lerpR (full, endView, e);
            const auto kb = inEditor (*ed, &ed->keyboardView());
            const Rect startCam = fitAspect (kb.withSizeKeepingCentre (kb.getWidth() * 0.42f, kb.getHeight() * 1.15f), full.getWidth() / full.getHeight());
            const Rect endCam = fitAspect (whole(), endView.getWidth() / endView.getHeight());
            Rect c = fitAspect (lerpR (startCam, endCam, e), v.getWidth() / v.getHeight());
            if (c.getWidth() <= whole().getWidth() && c.getHeight() <= whole().getHeight()) c = c.constrainedWithin (whole());   // never past the panel's edge
            const float sc = v.getWidth() / c.getWidth();
            const auto inter = c.getIntersection (whole()).getSmallestIntegerContainer();
            if (! inter.isEmpty())
            {
                const auto snap = ed->createComponentSnapshot (inter, true, sc);
                g.saveState();
                g.reduceClipRegion (v.toNearestInt());
                g.drawImageAt (snap, juce::roundToInt (v.getX() + ((float) inter.getX() - c.getX()) * sc), juce::roundToInt (v.getY() + ((float) inter.getY() - c.getY()) * sc));
                g.restoreState();
            }
            if (i == frames - 1)
            {
                juce::PNGImageFormat png;
                const auto endPng = out.getChildFile (film.stem + "_end.png");
                endPng.deleteFile();
                if (auto os = std::unique_ptr<juce::FileOutputStream> (endPng.createOutputStream())) png.writeImageToStream (frame, *os);
            }
        }
        else if (card)
        {
            const bool intro = ! film.explainer && t0 < 3.5;
            const float fade = intro ? (float) juce::jlimit (0.0, 1.0, juce::jmin (t0 / 0.6, (3.5 - t0) / 0.6 + 0.0))
                                     : (float) juce::jlimit (0.0, 1.0, (t0 - (tEnd - (film.explainer ? 3.5 : 6.0))) / 0.8);
            drawCard (g, box, intro ? juce::jmax (0.15f, fade) : fade, intro ? "JING FANG  |  " + film.name : "JING FANG",
                      intro ? juce::String ("Microtonal synthesizer by SubverseLab") : juce::String ("subverselab.com"));
        }
        else if (vertical)
        {
            g.fillAll (kGround);
            g.setColour (kCream); g.setFont (jfui::heros (78.0f, true));
            g.drawText ("JING FANG", juce::Rectangle<int> (0, 96, W, 90), juce::Justification::centred);
            g.setColour (kGold); g.setFont (jfui::heros (40.0f, false));
            g.drawText (film.name, juce::Rectangle<int> (0, 190, W, 56), juce::Justification::centred);
            // the camera glides to its target (the door or the part being explained), 12 % of the way a frame
            Rect target = camTarget ? camTarget() : whole();
            if (! film.explainer)                                          // the ads: the open door, else the whole panel
            {
                juce::Component* door = ed->patchBay().isOpenOrOpening() ? static_cast<juce::Component*> (&ed->patchBay())
                                      : ed->seqDoorView()->isOpenOrOpening() ? static_cast<juce::Component*> (ed->seqDoorView())
                                      : ed->drumDoorView()->isOpenOrOpening() ? static_cast<juce::Component*> (ed->drumDoorView())
                                      : ed->labDoorView()->isOpenOrOpening() ? static_cast<juce::Component*> (ed->labDoorView()) : nullptr;
                target = door != nullptr ? inEditor (*ed, door) : whole();
            }
            const Rect want = fitAspect (target.isEmpty() ? whole() : target, view.getWidth() / view.getHeight());
            const float k = i == 0 ? 1.0f : 0.12f;
            cam = Rect (cam.getX() + (want.getX() - cam.getX()) * k, cam.getY() + (want.getY() - cam.getY()) * k,
                        cam.getWidth() + (want.getWidth() - cam.getWidth()) * k, cam.getHeight() + (want.getHeight() - cam.getHeight()) * k);
            const float sc = view.getWidth() / cam.getWidth();
            const auto inter = cam.getIntersection (whole()).getSmallestIntegerContainer();
            if (! inter.isEmpty())
            {
                const auto snap = ed->createComponentSnapshot (inter, true, sc);
                g.saveState();
                g.reduceClipRegion (view.toNearestInt());
                g.drawImageAt (snap, juce::roundToInt (view.getX() + ((float) inter.getX() - cam.getX()) * sc), juce::roundToInt (view.getY() + ((float) inter.getY() - cam.getY()) * sc));
                g.restoreState();
            }
            for (auto& c : film.caps)
                if (t0 >= c.t0 && t0 < c.t1)
                {
                    const float a = (float) juce::jlimit (0.0, 1.0, juce::jmin ((t0 - c.t0) / 0.3, (c.t1 - t0) / 0.3));
                    int x = 80;
                    if (c.number > 0)
                    {
                        g.setColour (kGold.withAlpha (a));
                        g.fillEllipse (80.0f, 1282.0f, 76.0f, 76.0f);
                        g.setColour (kGround.withAlpha (a)); g.setFont (jfui::heros (44.0f, true));
                        g.drawText (juce::String (c.number), juce::Rectangle<int> (80, 1282, 76, 76), juce::Justification::centred);
                        x = 186;
                    }
                    g.setColour (kCream.withAlpha (a)); g.setFont (jfui::heros (62.0f, true));
                    g.drawFittedText (c.text, juce::Rectangle<int> (x, 1280, W - x - 70, 150), juce::Justification::topLeft, 2, 1.0f);
                    g.setColour (kCream.withAlpha (a * 0.72f)); g.setFont (jfui::heros (40.0f, false));
                    g.drawFittedText (c.sub, juce::Rectangle<int> (x, 1440, W - x - 70, 110), juce::Justification::topLeft, 2, 1.0f);
                }
            if (lockup.isValid())
            {
                const float lw = 300.0f, lh = lw * (float) lockup.getHeight() / (float) lockup.getWidth();
                g.drawImage (lockup, Rect ((float) W - 70.0f - lw, (float) H - 130.0f - lh, lw, lh));
            }
            g.setColour (kGold); g.setFont (jfui::heros (34.0f, false));
            g.drawText ("subverselab.com", juce::Rectangle<int> (70, H - 190, 500, 50), juce::Justification::centredLeft);
        }
        else if (wideExplainer)
        {   // 16:9 narrated: the camera glides to the part being explained; the step's number, title and line in a band below
            g.fillAll (kGround);
            Rect target = camTarget ? camTarget() : whole();
            const Rect want = fitAspect (target.isEmpty() ? whole() : target, view.getWidth() / view.getHeight());
            const float k = i == 0 ? 1.0f : 0.10f;
            cam = Rect (cam.getX() + (want.getX() - cam.getX()) * k, cam.getY() + (want.getY() - cam.getY()) * k,
                        cam.getWidth() + (want.getWidth() - cam.getWidth()) * k, cam.getHeight() + (want.getHeight() - cam.getHeight()) * k);
            const float sc = view.getWidth() / cam.getWidth();
            const auto inter = cam.getIntersection (whole()).getSmallestIntegerContainer();
            if (! inter.isEmpty())
            {
                const auto snap = ed->createComponentSnapshot (inter, true, sc);
                g.saveState();
                g.reduceClipRegion (view.toNearestInt());
                g.drawImageAt (snap, juce::roundToInt (view.getX() + ((float) inter.getX() - cam.getX()) * sc), juce::roundToInt (view.getY() + ((float) inter.getY() - cam.getY()) * sc));
                g.restoreState();
            }
            const float bandY = view.getBottom();
            g.setColour (kGround); g.fillRect (0.0f, bandY, (float) W, (float) H - bandY);
            g.setColour (kGold.withAlpha (0.35f)); g.fillRect (0.0f, bandY, (float) W, 2.0f);
            for (auto& c : film.caps)
                if (t0 >= c.t0 && t0 < c.t1)
                {
                    const float a = (float) juce::jlimit (0.0, 1.0, juce::jmin ((t0 - c.t0) / 0.3, (c.t1 - t0) / 0.3));
                    int x = 60;
                    if (c.number > 0)
                    {
                        g.setColour (kGold.withAlpha (a)); g.fillEllipse (60.0f, bandY + 38.0f, 64.0f, 64.0f);
                        g.setColour (kGround.withAlpha (a)); g.setFont (jfui::heros (34.0f, true));
                        g.drawText (juce::String (c.number), juce::Rectangle<int> (60, (int) bandY + 38, 64, 64), juce::Justification::centred);
                        x = 150;
                    }
                    g.setColour (kCream.withAlpha (a)); g.setFont (jfui::heros (44.0f, true));
                    g.drawText (c.text, juce::Rectangle<int> (x, (int) bandY + 26, W - x - 520, 54), juce::Justification::centredLeft);
                    g.setColour (kCream.withAlpha (a * 0.72f)); g.setFont (jfui::heros (28.0f, false));
                    g.drawText (c.sub, juce::Rectangle<int> (x, (int) bandY + 84, W - x - 520, 40), juce::Justification::centredLeft);
                }
            if (lockup.isValid())
            {
                const float lw = 230.0f, lh = lw * (float) lockup.getHeight() / (float) lockup.getWidth();
                g.drawImage (lockup, Rect ((float) W - 60.0f - lw, bandY + 40.0f, lw, lh));
            }
            g.setColour (kGold); g.setFont (jfui::heros (26.0f, false));
            g.drawText ("subverselab.com", juce::Rectangle<int> (W - 520, (int) bandY + 100, 460, 36), juce::Justification::centredRight);
        }
        else
        {
            g.fillAll (juce::Colour (0xff0B0C0C));
            const auto snap = ed->createComponentSnapshot (ed->getLocalBounds(), true, devScale);
            g.drawImageAt (snap, devX, devY);
            for (auto& c : film.caps)
                if (t0 >= c.t0 && t0 < c.t1)
                {
                    const float a = (float) juce::jlimit (0.0, 1.0, juce::jmin ((t0 - c.t0) / 0.4, (c.t1 - t0) / 0.4));
                    g.setColour (juce::Colour (0xffECE0C8).withAlpha (a));
                    g.setFont (jfui::heros (30.0f, true));
                    g.drawText (c.text, juce::Rectangle<int> (0, H - 104, W, 40), juce::Justification::centred);
                    g.setColour (kGold.withAlpha (a));
                    g.setFont (jfui::heros (19.0f, false));
                    g.drawText (c.sub, juce::Rectangle<int> (0, H - 62, W, 28), juce::Justification::centred);
                }
        }
        juce::Image::BitmapData bd (frame, juce::Image::BitmapData::readOnly);
        for (int y = 0; y < H; ++y) fwrite (bd.getLinePointer (y), 4, (size_t) W, pipe);
        if (i % (kFps * 10) == 0) std::printf ("  %s: %d / %d s\n", film.name.toRawUTF8(), i / kFps, frames / kFps), std::fflush (stdout);
    }
    pclose (pipe);
    const auto wav = out.getChildFile (film.stem + ".wav");
    wav.deleteFile();
    if (auto os = std::unique_ptr<juce::OutputStream> (introMode != 0 && ! hicaz ? nullptr : wav.createOutputStream()))
    {
        juce::WavAudioFormat fmt;
        if (auto* w = fmt.createWriterFor (os.get(), kSr, 2, 24, {}, 0)) { os.release(); std::unique_ptr<juce::AudioFormatWriter> wr (w); wr->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples()); }
    }
    if (film.explainer)                                                  // the narration's times, for tools/explainer_mux.py
    {
        juce::String j = "{\"stem\": \"" + film.stem + "\", \"length\": " + num (film.length, 3) + ", \"lines\": [";
        for (size_t n = 1; n < film.L.size(); ++n) j << (n > 1 ? ", " : "") << "{\"n\": " << (int) n << ", \"t0\": " << num (film.L[n], 3) << ", \"t1\": " << num (film.E[n], 3) << "}";
        out.getChildFile (film.stem + "_lines.json").replaceWithText (j + "]}\n");
    }
    stillDir.getChildFile ("stills.json").replaceWithText ("{\"film\": " + juce::JSON::toString (film.name) + ", \"stem\": \"" + film.stem
                                                           + "\", \"panel\": [" + juce::String (jfui::kW) + ", " + juce::String (jfui::kH) + "], \"scale\": 2, \"stills\": [\n" + stillsJson + "]}\n");
    const auto report = out.getChildFile (film.stem + "_report.md");
    writeReport (report, film, agent, frames);
    std::printf ("wrote %s, %s and %s: expectations %d held, %d failed; %d findings\n", video.getFullPathName().toRawUTF8(), wav.getFullPathName().toRawUTF8(),
                 report.getFullPathName().toRawUTF8(), agent.pass, agent.fail, (int) agent.found.size());
    ed = nullptr;
    return 0;
}
