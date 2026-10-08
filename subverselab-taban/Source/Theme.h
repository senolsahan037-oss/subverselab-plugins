#pragma once

#include <JuceHeader.h>
#include "BinaryData.h"

namespace Theme
{
    const juce::Colour gold          { 0xffC5A059 };
    const juce::Colour goldLight     { 0xffDFBF7A };
    const juce::Colour goldDark      { 0xff8C6F35 };
    const juce::Colour teal          { 0xff2A9D8F };

    const juce::Colour panel         { 0xff033733 };   // signature SubverseLab anodized green plate
    const juce::Colour panelDeep     { 0xff022523 };
    const juce::Colour ink           { 0xffF2EFE7 };   // silkscreen cream
    const juce::Colour lamp          { 0xffF6D58E };
    const juce::Colour muted         { 0xff8FAAA5 };
    const juce::Colour accent        { 0xffE76F51 };   // terracotta upward accent
    const juce::Colour label         { 0xffF5F1E5 };
    const juce::Colour labelDeep     { 0xffDED6C1 };
    const juce::Colour labelInk      { 0xff033733 };
    const juce::Colour labelInk2     { 0xff496C68 };
    const juce::Colour ledActive     { 0xff2A9D8F };
    const juce::Colour ledOff        { 0xff153430 };
    const juce::Colour ledRed        { 0xffFF4028 };
    const juce::Colour ledRedOff     { 0xff460E0A };
    const juce::Colour ledRedDim     { 0xff8C1E14 };
    const juce::Colour redWindow     { 0xff1C0605 };

    struct Faces
    {
        juce::Typeface::Ptr bold    = juce::Typeface::createSystemTypefaceFor (BinaryData::texgyreherosbold_otf, BinaryData::texgyreherosbold_otfSize);
        juce::Typeface::Ptr regular = juce::Typeface::createSystemTypefaceFor (BinaryData::texgyreherosregular_otf, BinaryData::texgyreherosregular_otfSize);
        juce::Typeface::Ptr mono    = juce::Typeface::createSystemTypefaceFor (BinaryData::DejaVuSansMonoBold_ttf, BinaryData::DejaVuSansMonoBold_ttfSize);
    };

    inline juce::Font face (const juce::Typeface::Ptr& tf, float em) { return juce::Font (juce::FontOptions (tf).withPointHeight (em)); }
    inline juce::Font legendEm (float em) { juce::SharedResourcePointer<Faces> f; return face (f->bold, em); }
    inline juce::Font fineEm (float em)   { juce::SharedResourcePointer<Faces> f; return face (f->regular, em); }
    inline juce::Font monoEm (float em)   { juce::SharedResourcePointer<Faces> f; return face (f->mono, em * 0.88f); }
}
