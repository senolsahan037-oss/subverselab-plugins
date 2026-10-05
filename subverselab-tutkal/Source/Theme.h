#pragma once

#include <JuceHeader.h>
#include "BinaryData.h"

// SubverseLab palette and the hardware built from it (the family face of KORO / KASET / HAVA).
// The typefaces are compiled in (Assets/Fonts): TeX Gyre Heros for the legends, DejaVu Sans Mono for read-outs.
// No system font name is ever used.
namespace Theme
{
    const juce::Colour gold          { 0xffC5A059 };
    const juce::Colour goldLight     { 0xffDFBF7A };
    const juce::Colour goldDark      { 0xff8C6F35 };
    const juce::Colour teal          { 0xff2A9D8F };

    const juce::Colour panel         { 0xff033733 };
    const juce::Colour panelDeep     { 0xff022523 };
    const juce::Colour ink           { 0xffF2EFE7 };   // silkscreen cream
    const juce::Colour lamp          { 0xffF6D58E };
    const juce::Colour muted         { 0xff8faaa5 };
    const juce::Colour label         { 0xffF5F1E5 };   // cream label, top
    const juce::Colour labelDeep     { 0xffDED6C1 };   // cream label, bottom
    const juce::Colour labelInk2     { 0xff496c68 };   // unlit legend on the label
    const juce::Colour ledRed        { 0xffFF4028 };
    const juce::Colour ledRedOff     { 0xff460e0a };
    const juce::Colour ledRedDim     { 0xff8c1e14 };
    const juce::Colour redWindow     { 0xff1c0605 };

    struct Faces
    {
        juce::Typeface::Ptr bold    = juce::Typeface::createSystemTypefaceFor (BinaryData::texgyreherosbold_otf, BinaryData::texgyreherosbold_otfSize);
        juce::Typeface::Ptr regular = juce::Typeface::createSystemTypefaceFor (BinaryData::texgyreherosregular_otf, BinaryData::texgyreherosregular_otfSize);
        juce::Typeface::Ptr mono    = juce::Typeface::createSystemTypefaceFor (BinaryData::DejaVuSansMonoBold_ttf, BinaryData::DejaVuSansMonoBold_ttfSize);
    };

    inline juce::Font face (const juce::Typeface::Ptr& tf, float em) { return juce::Font (juce::FontOptions (tf).withPointHeight (em)); }
    // Sizes are em (point heights), as the KORO face was drawn.
    inline juce::Font legendEm (float em) { juce::SharedResourcePointer<Faces> f; return face (f->bold, em); }
    inline juce::Font fineEm (float em)   { juce::SharedResourcePointer<Faces> f; return face (f->regular, em); }
    inline juce::Font monoEm (float em)   { juce::SharedResourcePointer<Faces> f; return face (f->mono, em * 0.88f); }
}
