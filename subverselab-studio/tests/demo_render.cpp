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
//   demo_render e<N> <out_dir>                   an explainer (9:16), timed to its narration: marketing/explainers/script.json
//                                                and out/explainers/voice/voice.json (tools/jf_voice.py)
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cstdio>
#include <map>

namespace
{
constexpr int kFps = 30, kSr = 48000, kSpf = kSr / kFps;
int W = 1920, H = 1080;
bool vertical = false;
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
    if (argc < 3) { std::fprintf (stderr, "usage: demo_render <1|2|3|e1..e6> <out_dir> [--vertical]\n"); return 2; }
    const juce::String which (argv[1]);
    const bool isExplainer = which.startsWith ("e");
    vertical = isExplainer || (argc > 3 && juce::String (argv[3]) == "--vertical");
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
        if (film.name.isEmpty() || n < 1 || n > 6) { std::fprintf (stderr, "no %s in script.json\n", which.toRawUTF8()); return 2; }
        explainerLines (film, script[which.toRawUTF8()]);
        void (*fns[]) (Film&, JingFangProcessor&, JingFangEditor&) = { e1, e2, e3, e4, e5, e6 };
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
    const Rect view (0, 290, (float) W, 900);                             // 9:16: the camera's window
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
