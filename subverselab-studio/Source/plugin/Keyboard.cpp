#include "Keyboard.h"

namespace jfui
{
namespace
{
const char* kPcNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
constexpr int kWhitePc[] = { 0, 2, 4, 5, 7, 9, 11 };
// Live's computer MIDI keys: A row whites, Q row blacks, C..D# of the next octave
const std::pair<char, int> kNormalKeys[] = { { 'a', 0 }, { 'w', 1 }, { 's', 2 }, { 'e', 3 }, { 'd', 4 }, { 'f', 5 }, { 't', 6 }, { 'g', 7 },
                                             { 'y', 8 }, { 'h', 9 }, { 'u', 10 }, { 'j', 11 }, { 'k', 12 }, { 'o', 13 }, { 'l', 14 }, { 'p', 15 } };
// micro: three letter rows, seven white keys each
const char* kMicroRows[3] = { "zxcvbnm", "asdfghj", "qwertyu" };
}

#if JUCE_MAC
} // namespace jfui
// CoreGraphics / HIToolbox / CoreFoundation, declared here because their headers collide with juce::Point
extern "C" bool CGEventSourceKeyState (int32_t stateID, uint16_t key);
extern "C" void* TISCopyCurrentKeyboardLayoutInputSource();
extern "C" void* TISGetInputSourceProperty (void* source, const void* key);
extern "C" const void* const kTISPropertyInputSourceID;
extern "C" bool CFStringGetCString (const void* s, char* buffer, long size, uint32_t encoding);
extern "C" void CFRelease (const void* cf);
namespace jfui
{
namespace
{
// a held letter as the OS sees it (JUCE clears its own key-down set on every Shift / Caps Lock change, which made a held
// chord drop when Shift was pressed for a sharp). Virtual key codes of the ANSI positions (Turkish Q has the same positions).
int macKeyCode (int c)
{
    static const std::pair<char, int> map[] = { { 'a', 0x00 }, { 's', 0x01 }, { 'd', 0x02 }, { 'f', 0x03 }, { 'h', 0x04 }, { 'g', 0x05 },
        { 'z', 0x06 }, { 'x', 0x07 }, { 'c', 0x08 }, { 'v', 0x09 }, { 'b', 0x0B }, { 'q', 0x0C }, { 'w', 0x0D }, { 'e', 0x0E },
        { 'r', 0x0F }, { 'y', 0x10 }, { 't', 0x11 }, { 'o', 0x1F }, { 'u', 0x20 }, { 'i', 0x22 }, { 'p', 0x23 }, { 'l', 0x25 },
        { 'j', 0x26 }, { 'k', 0x28 }, { 'n', 0x2D }, { 'm', 0x2E } };
    for (auto& [ch, kc] : map) if (ch == c) return kc;
    return -1;
}
} // namespace
#endif

#if JUCE_MAC
// the OS key state is read by key position (ANSI); only layouts whose letters sit on those positions may use it (round 2 of
// the bug sweep: on QWERTZ / AZERTY a held "y" / "a" was released at once). Others fall back to JUCE's own state.
static bool lettersOnAnsiPositions()
{
    static juce::uint32 checkedAt = 0;
    static bool ok = false;
    const auto now = juce::Time::getMillisecondCounter();
    if (checkedAt != 0 && now - checkedAt < 2000) return ok;   // the user may switch layouts: look again every 2 s
    checkedAt = now == 0 ? 1 : now;
    ok = false;
    if (void* src = TISCopyCurrentKeyboardLayoutInputSource())
    {
        char id[256] = {};
        if (const void* str = TISGetInputSourceProperty (src, kTISPropertyInputSourceID))
            if (CFStringGetCString (str, id, sizeof (id), 0x08000100 /* kCFStringEncodingUTF8 */))
            {
                const juce::String s (id);
                for (auto* l : { ".US", ".ABC", "British", "Australian", "Canadian", "Irish", "Turkish-QWERTY", "Dvorak-Left" })
                    if (s.contains (l) && ! s.contains ("Dvorak")) ok = true;
            }
        CFRelease (src);
    }
    return ok;
}
#endif

static bool keyIsDown (int c)
{
   #if JUCE_MAC
    const int kc = macKeyCode (c);
    if (kc >= 0 && lettersOnAnsiPositions()) return CGEventSourceKeyState (0 /* kCGEventSourceStateCombinedSessionState */, (uint16_t) kc);
   #endif
    return juce::KeyPress::isKeyCurrentlyDown (c);
}

Keyboard3::Keyboard3 (JingFangProcessor& p) : proc (p)
{
    startTimerHz (60);
    setWantsKeyboardFocus (false);   // the editor holds the focus and forwards the keys
}

bool Keyboard3::micro() const { return proc.exactValue (svl::TUNE_MODE) > 0.5f; }
int Keyboard3::rowOf (int pc) const { return juce::jlimit (0, 2, juce::roundToInt (proc.exactValue (svl::TUNE_ROW0 + pc))); }
float Keyboard3::stepCents() const { return 1200.0f / juce::jmax (1.0f, proc.exactValue (svl::TUNE_EDO)); }

int Keyboard3::whiteIndex (int key)
{
    const int oct = key / 12, pc = key % 12;
    int w = 0;
    for (int i = 0; i < 7; ++i) if (kWhitePc[i] <= pc) w = i;
    return oct * 7 + w;
}

juce::Rectangle<float> Keyboard3::manualRect (int row) const
{
    const float h = ((float) getHeight() - kStripH - 2.0f * kGap - 2.0f) / 3.0f;
    const int slot = 2 - row;                       // high on top
    return { kLabelW, kStripH + 2.0f + (float) slot * (h + kGap), (float) getWidth() - kLabelW, h };
}

juce::Rectangle<float> Keyboard3::keyRect (int row, int key) const
{
    const auto m = manualRect (row);
    const int whites = whiteIndex (kHi) - whiteIndex (kLo) + 1;
    const float ww = m.getWidth() / (float) whites;
    if (! isBlack (key))
        return { m.getX() + ww * (float) (whiteIndex (key) - whiteIndex (kLo)), m.getY(), ww, m.getHeight() };
    const float bw = ww * 0.62f;
    const float x = m.getX() + ww * (float) (whiteIndex (key) - whiteIndex (kLo) + 1) - bw * 0.5f;
    return { x, m.getY(), bw, m.getHeight() * 0.6f };
}

juce::Rectangle<float> Keyboard3::stripCell (int pc) const
{
    const float w = ((float) getWidth() - kLabelW) / 12.0f;
    return { kLabelW + w * (float) pc, 0.0f, w, kStripH };
}

std::pair<int, int> Keyboard3::hit (juce::Point<float> p) const
{
    for (int row = 0; row < 3; ++row)
    {
        if (! manualRect (row).contains (p)) continue;
        for (int k = kLo; k <= kHi; ++k) if (isBlack (k) && keyRect (row, k).contains (p)) return { row, k };
        for (int k = kLo; k <= kHi; ++k) if (! isBlack (k) && keyRect (row, k).contains (p)) return { row, k };
    }
    return { -1, -1 };
}

void Keyboard3::press (int row, int key, bool moveRow)
{
    release();
    if (row < 0 || key < 0) return;
    if (micro() && moveRow)
    {   // right-click / Alt + click: move the pitch class one row on: low -> mid -> high -> low
        cycleRow (key % 12);
        mouseRow = row; mouseKey = key; mousePlayRow = -2;
        return;
    }
    // a click plays (owner 18 Oct: in MICRO a click moved the row instead, so nothing sounded - not even the drum machine).
    // Normal mode: the middle manual plays the key as a MIDI key, the outer manuals one row away. Micro: the manual the pitch
    // class sits on plays it as a MIDI key (its row, the tuning, the drum transport); another manual plays that row.
    if (micro()) mousePlayRow = row == rowOf (key % 12) ? -1 : row;
    else mousePlayRow = row == 1 ? -1 : row;
    mouseRow = row; mouseKey = key;
    proc.uiNoteOn (key, mousePlayRow, 100.0f / 127.0f);
    repaint();
}

void Keyboard3::cycleRow (int pc)
{
    setParamGesture (proc.apvts.getParameter (svl::paramDefs()[(size_t) (svl::TUNE_ROW0 + pc)].id), (float) ((rowOf (pc) + 1) % 3));
    repaint();
}

void Keyboard3::release()
{
    if (mouseKey >= 0 && mousePlayRow != -2) proc.uiNoteOff (mouseKey, mousePlayRow);
    mouseRow = mouseKey = -1; mousePlayRow = -2;
}

void Keyboard3::mouseDown (const juce::MouseEvent& e)
{
    if (e.position.y < kStripH)
    {   // the pitch-class strip: a click moves that pitch class one row on
        for (int pc = 0; pc < 12; ++pc) if (stripCell (pc).contains (e.position)) { cycleRow (pc); return; }
        return;
    }
    const auto [row, key] = hit (e.position);
    press (row, key, e.mods.isAltDown() || e.mods.isPopupMenu());
}

void Keyboard3::mouseDrag (const juce::MouseEvent& e)
{
    if (mousePlayRow == -2) return;                  // a row click in micro mode does not slide
    const auto [row, key] = hit (e.position);
    if (row == mouseRow && key == mouseKey) return;
    if (row < 0) { release(); return; }
    press (row, key, false);
}

void Keyboard3::mouseUp (const juce::MouseEvent&) { release(); repaint(); }

//==============================================================================
bool Keyboard3::keyPressed (const juce::KeyPress& k)
{
    const int code = k.getKeyCode();
    if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey)
    {
        releaseAllComputerKeys();
        octave = juce::jlimit (0, 8, octave + (code == juce::KeyPress::rightKey ? 1 : -1));
        repaint();
        return true;
    }
    if (k.getModifiers().isCommandDown() || k.getModifiers().isCtrlDown()) return false;   // Cmd+Q, Cmd+S ... stay shortcuts
    const int c = (int) juce::CharacterFunctions::toLowerCase ((juce::juce_wchar) code);
    if (heldKeys.count (c) > 0 || octaveKeys.count (c) > 0) return true;   // key repeat
    const int base = (octave + 1) * 12;
    if (! micro())
    {
        if (c == 'z' || c == 'x')
        {   // one octave per press (holding the key does not keep shifting)
            releaseAllComputerKeys(); octaveKeys.insert (c);
            octave = juce::jlimit (0, 8, octave + (c == 'x' ? 1 : -1)); repaint(); return true;
        }
        for (const auto& [ch, off] : kNormalKeys)
            if (c == ch)
            {
                const int note = base + off;
                if (note > 127) return true;
                heldKeys[c] = { note, -1 };
                proc.uiNoteOn (note, -1, 100.0f / 127.0f);
                return true;
            }
        return false;
    }
    for (int row = 0; row < 3; ++row)
        for (int i = 0; i < 7; ++i)
            if (c == kMicroRows[row][i])
            {
                const int note = base + kWhitePc[i] + (k.getModifiers().isShiftDown() && i != 2 && i != 6 ? 1 : 0);
                if (note > 127) return true;
                heldKeys[c] = { note, row };
                proc.uiNoteOn (note, row, 100.0f / 127.0f);
                return true;
            }
    return false;
}

bool Keyboard3::keyStateChanged (bool)
{
    bool any = false;
    for (auto it = octaveKeys.begin(); it != octaveKeys.end();) { if (! keyIsDown (*it)) it = octaveKeys.erase (it); else ++it; }
    for (auto it = heldKeys.begin(); it != heldKeys.end();)
    {
        if (! keyIsDown (it->first))
        {
            proc.uiNoteOff (it->second.first, it->second.second);
            it = heldKeys.erase (it);
            any = true;
        }
        else ++it;
    }
    return any;
}

void Keyboard3::releaseAllComputerKeys()
{
    for (const auto& h : heldKeys) proc.uiNoteOff (h.second.first, h.second.second);
    heldKeys.clear();
}

//==============================================================================
juce::String Keyboard3::stateKey() const
{
    juce::String s;
    s << (micro() ? "m" : "n") << octave << ":" << proc.exactValue (svl::TUNE_EDO) << ":" << proc.exactValue (svl::ROW_UP) << ":"
      << proc.exactValue (svl::ROW_DOWN) << ":" << proc.exactValue (svl::KSW_MODE) << proc.exactValue (svl::KSW_LOW) << ":";
    for (int pc = 0; pc < 12; ++pc) s << rowOf (pc);
    s << ":";
    for (int row = 0; row < 3; ++row)
        for (int k = kLo; k <= kHi; ++k) if (proc.keyLit (row, k)) s << row << "." << k << ",";
    return s;   // the sounding notes repaint from the 60 Hz timer
}

void Keyboard3::timerCallback() { pollSounding (juce::Time::getMillisecondCounter()); }

void Keyboard3::pollSounding (juce::uint32 now)
{
    const juce::uint32 before = nowMs;
    nowMs = now;
    bool changed = false;
    for (int row = 0; row < 3; ++row)
        for (int k = kLo; k <= kHi; ++k)
        {
            auto& until = soundingUntil[(size_t) row][(size_t) k];
            const bool was = until > before;                         // lit in the last frame
            // a light goes out on the first frame after its sound stops; a new one stays at least 45 ms so a short arp / seq
            // note is still seen (owner 19 Oct: "arpejin notalarını gösteren taraf hâlâ gecikmeli": the old 90 ms hold after the
            // sound measured 142 ms average / 476 ms max from sound off to light off, so the lights trailed a fast arp)
            if (proc.keySounding (row, k)) until = was ? juce::jmax (until, nowMs + 1) : nowMs + 45;
            if (was != (until > nowMs)) changed = true;
        }
    if (changed) repaint (juce::Rectangle<int> (0, (int) kStripH, getWidth(), getHeight() - (int) kStripH));
    const double t = juce::Time::getMillisecondCounterHiRes();
    for (int k = kLo; k <= kHi; ++k)
    {
        bool shown = false;
        for (int row = 0; row < 3; ++row) shown |= soundingUntil[(size_t) row][(size_t) k] > nowMs;
        if (shown && ! shownAny[(size_t) k]) { const double d = t - proc.keySoundOnMs (k); if (d >= 0 && d < 2000) { delayStats.onSum += d; delayStats.onMax = juce::jmax (delayStats.onMax, d); ++delayStats.on; } }
        if (! shown && shownAny[(size_t) k]) { const double d = t - proc.keySoundOffMs (k); if (d >= 0 && d < 2000) { delayStats.offSum += d; delayStats.offMax = juce::jmax (delayStats.offMax, d); ++delayStats.off; } }
        shownAny[(size_t) k] = shown;
    }
}

void Keyboard3::tick()
{
    auto s = stateKey();
    if (s != lastState) { lastState = s; repaint(); }
}

void Keyboard3::paint (juce::Graphics& g)
{
    const double paintStart = juce::Time::getMillisecondCounterHiRes();
    struct PaintTimer { DelayStats& st; double t0; ~PaintTimer() { const double d = juce::Time::getMillisecondCounterHiRes() - t0; st.paintSum += d; st.paintMax = juce::jmax (st.paintMax, d); ++st.paints; } } paintTimer { delayStats, paintStart };
    const bool mic = micro();
    const float step = stepCents();
    const float upC = proc.exactValue (svl::ROW_UP) * step, downC = proc.exactValue (svl::ROW_DOWN) * step;
    auto cents = [] (float c) { return juce::String (c, std::abs (c - std::round (c)) < 0.05f ? 0 : 1); };

    // row-state strip: pitch class + arrow + cents of the row it sits on
    for (int pc = 0; pc < 12; ++pc)
    {
        const auto r = stripCell (pc).reduced (2.0f, 2.0f);
        const int row = rowOf (pc);
        const bool moved = mic && row != 1;
        keyFace (g, r, moved, false, false, false);
        if (moved) { g.setColour (kLed.withAlpha (0.8f)); g.drawRoundedRectangle (r, 3.0f, 1.2f); }
        const juce::Colour fg = moved ? kLed : (mic ? kKeyText : kKeyTextDim);
        g.setColour (fg);
        g.setFont (heros (10.5f, true));
        const juce::String name (kPcNames[pc]);
        if (moved)
        {
            const float tw = 48.0f, x0 = r.getCentreX() - tw * 0.5f;
            g.drawText (name, juce::Rectangle<float> (x0, r.getY(), 16.0f, r.getHeight()), juce::Justification::centredRight, false);
            drawArrow (g, { x0 + 24.0f, r.getCentreY() }, 8.0f, row == 2, Pal::gold);
            g.setFont (heros (10.0f));
            g.setColour (fg);
            g.drawText (cents (row == 2 ? upC : downC), juce::Rectangle<float> (x0 + 31.0f, r.getY(), 30.0f, r.getHeight()), juce::Justification::centredLeft, false);
        }
        else g.drawText (name, r, juce::Justification::centred, false);
    }

    // housing
    const auto all = juce::Rectangle<float> (0.0f, kStripH + 0.0f, (float) getWidth(), (float) getHeight() - kStripH);
    g.setColour (juce::Colour (0xff0B0D0E)); g.fillRoundedRectangle (all, 5.0f);

    const int ksLo = juce::roundToInt (proc.exactValue (svl::KSW_LOW));
    const bool ksOn = mic && proc.exactValue (svl::KSW_MODE) > 0.5f;
    for (int row = 2; row >= 0; --row)
    {
        const auto m = manualRect (row);
        // label column: the row and how far it is
        const auto lab = juce::Rectangle<float> (0.0f, m.getY(), kLabelW, m.getHeight());
        if (row == 1) { g.setColour (Pal::cream.withAlpha (0.7f)); g.fillRect (juce::Rectangle<float> (12.0f, 2.0f).withCentre (lab.getCentre().translated (0.0f, -6.0f))); }
        else drawArrow (g, lab.getCentre().translated (0.0f, -6.0f), 10.0f, row == 2, Pal::gold);
        g.setColour (Pal::cream.withAlpha (0.75f));
        g.setFont (heros (8.6f));
        g.drawText (row == 1 ? juce::String ("0") : cents (row == 2 ? upC : downC), lab.withTrimmedTop (lab.getHeight() * 0.5f).withHeight (14.0f),
                    juce::Justification::centred, false);

        auto paintKey = [&] (int k)
        {
            const auto r = keyRect (row, k).reduced (0.6f, 0.0f);
            const bool black = isBlack (k);
            const bool active = mic ? rowOf (k % 12) == row : row == 1;
            const bool held = proc.keyLit (row, k), sounds = shownSounding (row, k), on = held || sounds;
            juce::Colour face = black ? juce::Colour (0xff121415) : juce::Colour (0xffF4F0E6);
            if (held) face = kLed;                                         // the key you hold
            else if (sounds) face = kLed.withMultipliedBrightness (0.62f); // a note the arp / sequencer / chord plays
            g.setColour (face);
            g.fillRoundedRectangle (r.withTrimmedTop (-2.0f), 2.0f);
            if (! black && ! on)
            {
                g.setColour (Pal::capLo); g.fillRect (r.withTop (r.getBottom() - 4.0f));
            }
            if (! black) { g.setColour (Pal::ground.withAlpha (0.55f)); g.drawVerticalLine ((int) r.getRight(), r.getY(), r.getBottom()); }
            if (! active)
            {
                g.setColour (Pal::ground.withAlpha (black ? 0.35f : (mic ? 0.55f : 0.32f)));
                g.fillRoundedRectangle (r.withTrimmedTop (-2.0f), 2.0f);
            }
            else if (mic && row != 1)
            {   // the key its pitch class plays on: a gold bar and the row's arrow
                g.setColour (Pal::gold);
                g.fillRect (r.withTop (r.getBottom() - (black ? 3.0f : 4.0f)).reduced (black ? 1.0f : 2.0f, 0.0f));
                drawArrow (g, { r.getCentreX(), r.getBottom() - (black ? 9.0f : 11.0f) }, black ? 6.0f : 7.0f, row == 2, black ? Pal::cream : Pal::ink);
            }
            if (row == 1 && ksOn && k >= ksLo && k < ksLo + 24)
            {   // keyswitch keys: first octave = to low, second = to high; they never sound
                g.setColour (Pal::inkSoft.withAlpha (0.55f)); g.fillRect (r.withHeight (4.0f));
                drawArrow (g, { r.getCentreX(), black ? r.getY() + 10.0f : r.getBottom() - 26.0f }, 6.0f, k >= ksLo + 12, black ? Pal::gold : Pal::inkSoft);
            }
            if (! black && k % 12 == 0 && row == 1)
            {
                g.setColour (on ? Pal::ink : Pal::inkSoft);
                g.setFont (heros (8.4f, true));
                g.drawText ("C" + juce::String (k / 12 - 1), r.withTop (r.getBottom() - 16.0f), juce::Justification::centred, false);
            }
        };
        for (int k = kLo; k <= kHi; ++k) if (! isBlack (k)) paintKey (k);
        for (int k = kLo; k <= kHi; ++k) if (isBlack (k)) paintKey (k);
    }
}
} // namespace jfui
