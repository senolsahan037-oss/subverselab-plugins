#pragma once

#include <JuceHeader.h>
#include "BinaryData.h"

/*  BOE-B is not the green family plate: the panel is his 1992 sleeve (owner 24 Oct, sketch v3 / v4). Red ground, black
    heavy condensed type, a photocopied face, 90s objects (bakelite knobs, Dymo tape, a typed sheet taped on).
    The red is the sleeve's, not a kit colour: an exception the owner approved with the sketch. */
namespace Theme
{
    const juce::Colour sleeve     { 0xffE8321F };   // the 1992 sleeve red
    const juce::Colour sleeveDeep { 0xffC92614 };
    const juce::Colour ink        { 0xff111010 };   // print black
    const juce::Colour paper      { 0xffF2EFE7 };   // brand cream: the typed sheet, the lettering on the jacket
    const juce::Colour paperDeep  { 0xffE4DDCB };
    const juce::Colour paperInk2  { 0xff6D6656 };   // faded type on the sheet
    const juce::Colour tape       { 0xb8ECE2C4 };
    const juce::Colour led        { 0xffFF4028 };   // kit LED red
    const juce::Colour ledOff     { 0xff4A0D07 };
    const juce::Colour ledWindow  { 0xff1C0605 };
    const juce::Colour capText    { 0xffBDB6A4 };   // small print on the black keys

    struct Faces
    {
        juce::Typeface::Ptr stamp   = juce::Typeface::createSystemTypefaceFor (BinaryData::OswaldBold_ttf, BinaryData::OswaldBold_ttfSize);
        juce::Typeface::Ptr bold    = juce::Typeface::createSystemTypefaceFor (BinaryData::texgyreherosbold_otf, BinaryData::texgyreherosbold_otfSize);
        juce::Typeface::Ptr regular = juce::Typeface::createSystemTypefaceFor (BinaryData::texgyreherosregular_otf, BinaryData::texgyreherosregular_otfSize);
        juce::Typeface::Ptr mono    = juce::Typeface::createSystemTypefaceFor (BinaryData::DejaVuSansMonoBold_ttf, BinaryData::DejaVuSansMonoBold_ttfSize);
    };

    inline juce::Font face (const juce::Typeface::Ptr& tf, float px) { return juce::Font (juce::FontOptions (tf).withHeight (px)); }
    /** Heavy condensed print (Oswald Bold, OFL): the sleeve's lettering. */
    inline juce::Font stamp (float px)  { juce::SharedResourcePointer<Faces> f; return face (f->stamp, px); }
    inline juce::Font legend (float px) { juce::SharedResourcePointer<Faces> f; return face (f->bold, px); }
    inline juce::Font fine (float px)   { juce::SharedResourcePointer<Faces> f; return face (f->regular, px); }
    /** Typewriter on the sheet, the LED read-outs. */
    inline juce::Font mono (float px)   { juce::SharedResourcePointer<Faces> f; return face (f->mono, px); }
}
