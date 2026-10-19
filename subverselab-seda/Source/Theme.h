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

    // Brushed steel of the rings, black steel of the pointers and pusher collars (ALEV, 8 Oct), the bezel of the round case.
    const juce::Colour steelDark  { 0xff5b6066 };
    const juce::Colour steelLight { 0xffd4d8db };
    const juce::Colour steelEdge  { 0xff2a2d30 };
    const juce::Colour engrave    { 0xff1d1f21 };
    const juce::Colour bezelLight { 0xff2c302e };
    const juce::Colour bezelDark  { 0xff141716 };
    const juce::Colour blackSteelLight { 0xff474b50 };
    const juce::Colour blackSteelDark  { 0xff0d0e0f };
    // The read-out glass of the screens and the cream label colours of the faces
    const juce::Colour glassDeep  { 0xff010a09 };
    const juce::Colour glassMid   { 0xff03201d };
    const juce::Colour amber      { 0xffE9C97E };
    const juce::Colour amberLight { 0xffF2DFAE };

    // What the panel calls itself. The product name is NOT final: it lives here and nowhere else (SEDA is the working name;
    // the plug-in's own name, codes and bundle id are renamed in a separate step). The model line sits under the name on LAB and round the rim on ORBIT.
    inline const char* const kProductName = "SEDA";
    inline const char* const kModelLine   = "MODEL BD-79 - ANALOG DELAY";
    inline const char* const kModelCode   = "MODEL BD-79";
    inline const char* const kSubtitle    = "BBD ANALOG DELAY \xc2\xb7 LISTENING ECHO";
    inline const char* const kPlaque      = "SEDA \xc2\xb7 MODEL BD-79 \xc2\xb7 BBD ANALOG DELAY";

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
    // the screens' figures: the bold face squeezed to about 86 % (DIN Condensed, the prototype's face, is an Apple font and may not be embedded)
    inline juce::Font condEm (float em) { juce::SharedResourcePointer<Faces> f; return juce::Font (juce::FontOptions (f->bold).withPointHeight (em).withHorizontalScale (0.86f)); }
    inline juce::Font monoEm (float em)   { juce::SharedResourcePointer<Faces> f; return face (f->mono, em * 0.88f); }
}
