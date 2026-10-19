// JING FANG: the 3-row keyboard (7 octaves, C1..B7). Top manual = high row, middle = the usual keys, bottom = low row.
//
// normal mode: every key plays its usual note. The mouse plays any manual (the outer manuals sound one row up / down);
//   computer keys play as in Live: A W S E D F T G Y H U J K O L P = C..D#+1, Z / X or left / right = octave.
// micro mode: each pitch class sits on one row (low / mid / high) and MIDI keys play it there. A click plays (on the pitch
//   class's own manual as a MIDI key, on another manual that row); the strip above, right-click or Alt + click moves the pitch
//   class one row on (low -> mid -> high -> low). Computer keys: Z row = low, A row = mid,
//   Q row = high; the seven letters are the white keys, Shift makes them sharp; left / right = octave.
#pragma once
#include <set>
#include "Ui.h"
#include <map>
#include <array>

namespace jfui
{
class Keyboard3 : public juce::Component, private juce::Timer
{
public:
    static constexpr int kLo = 24, kHi = 107;     // C1..B7
    static constexpr float kStripH = 24.0f, kLabelW = 30.0f, kGap = 4.0f;
    explicit Keyboard3 (JingFangProcessor&);

    void tick();                                   // 30 Hz: repaint when what is shown changed
    bool micro() const;
    int rowOf (int pitchClass) const;
    int computerOctave() const noexcept { return octave; }

    // geometry (local coordinates)
    juce::Rectangle<float> manualRect (int row) const;
    juce::Rectangle<float> keyRect (int row, int key) const;
    std::pair<int, int> hit (juce::Point<float>) const;   // row, key (-1, -1 when none)
    juce::Rectangle<float> stripCell (int pitchClass) const;

    // what a click does (the mouse and the tests)
    void press (int row, int key, bool moveRow);
    void cycleRow (int pc);
    void release();

    // computer keyboard (forwarded by the editor)
    bool keyPressed (const juce::KeyPress&) override;
    bool keyStateChanged (bool isKeyDown) override;
    void releaseAllComputerKeys();

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    static bool isBlack (int key) { const int pc = key % 12; return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10; }
    static int whiteIndex (int key);
    float stepCents() const;
    juce::String stateKey() const;

    JingFangProcessor& proc;
    int octave = 4;                                // computer keys: C4 = MIDI 60
    int mouseRow = -1, mouseKey = -1, mousePlayRow = -2;
    std::map<int, std::pair<int, int>> heldKeys;   // key code -> (note, row)
    std::set<int> octaveKeys;                       // Z / X held (normal mode): no repeat
    juce::String lastState;
    // the notes that sound, checked at 60 Hz and kept lit at least 90 ms, so short arp / sequencer notes are seen with their
    // sound (owner 18 Oct: "basılan notaları gösteriyor ama görsel ve ses senkron değil")
    void timerCallback() override;
public:
    /** the sounding-key display step (the 60 Hz timer calls it; an offline renderer calls it once per frame with its own clock) */
    void pollSounding (juce::uint32 nowMilliseconds);
    void stopOwnClock() { stopTimer(); }           // the offline renderer drives pollSounding itself
private:
    bool shownSounding (int row, int key) const noexcept { return soundingUntil[(size_t) juce::jlimit (0, 2, row)][(size_t) (key & 127)] > nowMs; }
    std::array<std::array<juce::uint32, 128>, 3> soundingUntil {};
public:
    // the display's delay, measured (the JF_PROBE probe reads it): sound start -> light on, sound end -> light off, paint time
    struct DelayStats { double onSum = 0, onMax = 0, offSum = 0, offMax = 0, paintSum = 0, paintMax = 0; int on = 0, off = 0, paints = 0; };
    DelayStats delayStats;
    bool lightShown (int row, int key) const noexcept { return shownSounding (row, key); }
private:
    std::array<bool, 128> shownAny {};
    juce::uint32 nowMs = 0;
};
} // namespace jfui
