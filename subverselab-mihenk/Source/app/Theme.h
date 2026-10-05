#pragma once

#include <JuceHeader.h>

// SubverseLab palette, and the 1986 mastering unit built from it: a dark green
// anodized panel, cream silkscreen, gold for lamps and pointers, a blue-green
// VFD for numbers and a phosphor CRT for measured curves. Shared with KASET
// and KUBBE.
namespace Theme
{
    const juce::Colour gold          { 0xffC5A059 };
    const juce::Colour goldLight     { 0xffDFBF7A };
    const juce::Colour goldDark      { 0xff8C6F35 };
    const juce::Colour teal          { 0xff2A9D8F };
    const juce::Colour coral         { 0xffD9573F };

    const juce::Colour panel         { 0xff033733 };   // the emblem's ink, as anodized aluminium
    const juce::Colour panelDeep     { 0xff022523 };
    const juce::Colour ink           { 0xffF2EFE7 };   // silkscreen: the emblem's cream
    const juce::Colour capCream      { 0xffE6DFCC };
    const juce::Colour lamp          { 0xffF6D58E };
    const juce::Colour vfd           { 0xff63F2D8 };
    const juce::Colour vfdOff        { 0xff0E2A27 };
    const juce::Colour phosphor      { 0xff9DF7DD };
    const juce::Colour screen        { 0xff021512 };

    // Panel legends set the way mid-80s studio units were silkscreened:
    // Helvetica caps, bold for names, medium for scales and fine print.
    inline juce::Font legend (float size) { return juce::FontOptions ("Helvetica Neue", size, juce::Font::bold); }
    inline juce::Font fine (float size)   { return juce::FontOptions ("Helvetica Neue", "Medium", size); }
    inline juce::Font mono (float size)   { return juce::FontOptions ("Menlo", size, juce::Font::bold); }
}
