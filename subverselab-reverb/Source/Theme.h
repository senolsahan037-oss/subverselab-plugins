#pragma once

#include <JuceHeader.h>
#include "BinaryData.h"

// SubverseLab palette, and the 1979 hardware built from it: a dark green
// anodized panel, cream silkscreen, gold for lamps and pointers, a
// blue-green VFD and a phosphor CRT.
namespace Theme
{
    const juce::Colour background    { 0xff032825 };
    const juce::Colour surface       { 0xff063730 };
    const juce::Colour surfaceRaised { 0xff0a4740 };
    const juce::Colour line          { 0xff1a5b52 };

    const juce::Colour gold          { 0xffC5A059 };
    const juce::Colour goldLight     { 0xffDFBF7A };
    const juce::Colour goldDark      { 0xff8C6F35 };
    const juce::Colour teal          { 0xff2A9D8F };

    const juce::Colour text          { 0xffF2EFE7 };
    const juce::Colour textMuted     { 0xff8faaa5 };
    const juce::Colour danger        { 0xffe8917f };

    // Hardware
    const juce::Colour panel         { 0xff033733 };   // the emblem's ink, as anodized aluminium
    const juce::Colour panelDeep     { 0xff022523 };
    const juce::Colour ink           { 0xffF2EFE7 };   // silkscreen: the emblem's cream
    const juce::Colour capCream      { 0xffE6DFCC };
    const juce::Colour lamp          { 0xffF6D58E };
    const juce::Colour vfd           { 0xff63F2D8 };
    const juce::Colour vfdOff        { 0xff0E2A27 };
    const juce::Colour phosphor      { 0xff9DF7DD };
    const juce::Colour screen        { 0xff021512 };

    // Panel legends set the way late-70s studio units were silkscreened:
    // Helvetica caps, bold for names, a lighter cut for scales and fine print.
    // The faces are compiled into the plugin (Assets/Fonts), so the panel reads
    // the same on every machine and never depends on an installed font:
    // TeX Gyre Heros for the legends, DejaVu Sans Mono for the displays.
    struct Faces
    {
        juce::Typeface::Ptr bold    = juce::Typeface::createSystemTypefaceFor (BinaryData::texgyreherosbold_otf, BinaryData::texgyreherosbold_otfSize);
        juce::Typeface::Ptr regular = juce::Typeface::createSystemTypefaceFor (BinaryData::texgyreherosregular_otf, BinaryData::texgyreherosregular_otfSize);
        juce::Typeface::Ptr italic  = juce::Typeface::createSystemTypefaceFor (BinaryData::texgyreherositalic_otf, BinaryData::texgyreherositalic_otfSize);
        juce::Typeface::Ptr mono    = juce::Typeface::createSystemTypefaceFor (BinaryData::DejaVuSansMonoBold_ttf, BinaryData::DejaVuSansMonoBold_ttfSize);
    };

    // Sizes are given as the line height the panel was drawn with; these turn
    // them into the em size that keeps the capitals the height they were.
    constexpr float legendEm = 0.822f, monoEm = 0.859f;

    inline juce::Font face (const juce::Typeface::Ptr& tf, float em) { return juce::Font (juce::FontOptions (tf).withPointHeight (em)); }
    // The look-and-feel holds the faces for as long as an editor is open.
    inline juce::Font legend (float size)     { juce::SharedResourcePointer<Faces> f; return face (f->bold, size * legendEm); }
    inline juce::Font fine (float size)       { juce::SharedResourcePointer<Faces> f; return face (f->regular, size * legendEm); }
    inline juce::Font fineItalic (float size) { juce::SharedResourcePointer<Faces> f; return face (f->italic, size * legendEm); }
    inline juce::Font mono (float size)       { juce::SharedResourcePointer<Faces> f; return face (f->mono, size * monoEm); }
}
