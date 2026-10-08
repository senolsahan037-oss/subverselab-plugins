#pragma once

#include <JuceHeader.h>
#include "BinaryData.h"

// ALEV palette: the SubverseLab family colours, brushed-steel rings, and the heat scale of the ember (the only light).
// The typefaces are compiled in (Assets/Fonts): TeX Gyre Heros for the legends and the name. No system font name is used.
namespace Theme
{
    const juce::Colour hostDark   { 0xff033733 };   // brand green: the four corners outside the unit (owner, 8 Oct)
    const juce::Colour gold       { 0xffC5A059 };
    const juce::Colour ringBase   { 0xff033733 };   // ring body, dark green
    const juce::Colour ringEdge   { 0xff0a4a44 };
    const juce::Colour ink        { 0xffF2EFE7 };   // silkscreen cream
    const juce::Colour body       { 0xff033733 };   // brand green: the unit's face between the rings (owner, 8 Oct)
    // Brushed steel of the three rings: dark and light ends of the anisotropic sheen, edge shadow, engraving.
    const juce::Colour steelDark  { 0xff5b6066 };
    const juce::Colour steelLight { 0xffd4d8db };
    const juce::Colour steelEdge  { 0xff2a2d30 };
    const juce::Colour engrave    { 0xff1d1f21 };
    const juce::Colour bezelLight { 0xff2c302e };
    const juce::Colour bezelDark  { 0xff141716 };
    // Black steel: the raised pointer bars on the rings (owner, 8 Oct: "siyah celik bir gosterge cubugu").
    const juce::Colour blackSteelLight { 0xff474b50 };
    const juce::Colour blackSteelDark  { 0xff0d0e0f };

    // Heat scale: deep burgundy (0) -> burgundy -> ember orange -> hot core (1).
    const juce::Colour heat0 { 0xff5A0F1E };
    const juce::Colour heat1 { 0xff8B1A2B };
    const juce::Colour heat2 { 0xffD9572B };
    const juce::Colour heat3 { 0xffF4A259 };

    inline juce::Colour heat (float t) noexcept
    {
        t = juce::jlimit (0.0f, 1.0f, t) * 3.0f;
        if (t < 1.0f) return heat0.interpolatedWith (heat1, t);
        if (t < 2.0f) return heat1.interpolatedWith (heat2, t - 1.0f);
        return heat2.interpolatedWith (heat3, t - 2.0f);
    }

    // The coal: the ember's disc unlit (mix 0) and lit (mix 100), centre and edge.
    const juce::Colour coalCentre { 0xff2a0a10 };
    const juce::Colour coalEdge   { 0xff12050a };
    const juce::Colour litCentre  { 0xff5A0F1E };
    const juce::Colour litEdge    { 0xff2a0812 };

    struct Faces
    {
        juce::Typeface::Ptr bold    = juce::Typeface::createSystemTypefaceFor (BinaryData::texgyreherosbold_otf, BinaryData::texgyreherosbold_otfSize);
        juce::Typeface::Ptr regular = juce::Typeface::createSystemTypefaceFor (BinaryData::texgyreherosregular_otf, BinaryData::texgyreherosregular_otfSize);
        juce::Typeface::Ptr mono    = juce::Typeface::createSystemTypefaceFor (BinaryData::DejaVuSansMonoBold_ttf, BinaryData::DejaVuSansMonoBold_ttfSize);
    };

    inline juce::Font face (const juce::Typeface::Ptr& tf, float em) { return juce::Font (juce::FontOptions (tf).withPointHeight (em)); }
    // Sizes are em (point heights), as in the TUTKAL face.
    inline juce::Font legendEm (float em) { juce::SharedResourcePointer<Faces> f; return face (f->bold, em); }
}
