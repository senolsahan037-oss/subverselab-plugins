#pragma once

#include <JuceHeader.h>
#include "BinaryData.h"

// SubverseLab palette (KUBBE's Theme.h), plus the parts the family kit
// (KORO A2 / KASET: cream label window, red LEDs and 7-segment digits) adds.
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

    // Family kit (KORO A2 / YANKI): print on the cream label, red LEDs.
    const juce::Colour labelInk      { 0xff033733 };   // (3, 55, 51) print on the cream label
    const juce::Colour labelInk2     { 0xff496C68 };   // (73, 108, 104) secondary print
    const juce::Colour muted         { 0xff8FAAA5 };   // (143, 170, 165) fine print on the plate
    const juce::Colour ledRed        { 0xffFF4028 };   // (255, 64, 40)
    const juce::Colour ledRedOff     { 0xff460E0A };   // (70, 14, 10)
    const juce::Colour ledRedDark    { 0xff50120C };   // (80, 18, 12) unlit round LED
    const juce::Colour segWindow     { 0xff1C0605 };   // (28, 6, 5) red filter glass
    const juce::Colour swellRed      { 0xffEC684E };   // (236, 104, 78) the swell arc on the plate

    // Panel legends set the way the studio units of the day were silkscreened:
    // grotesque caps, bold for names, a lighter cut for scales and fine print.
    // The faces are compiled into the plugin (Assets/Fonts), so the panel reads
    // the same on every machine and never depends on an installed font:
    // TeX Gyre Heros for the legends, DejaVu Sans Mono for the displays.
    // No system font name is used anywhere.
    struct Faces
    {
        juce::Typeface::Ptr bold    = juce::Typeface::createSystemTypefaceFor (BinaryData::texgyreherosbold_otf, BinaryData::texgyreherosbold_otfSize);
        juce::Typeface::Ptr regular = juce::Typeface::createSystemTypefaceFor (BinaryData::texgyreherosregular_otf, BinaryData::texgyreherosregular_otfSize);
        juce::Typeface::Ptr mono    = juce::Typeface::createSystemTypefaceFor (BinaryData::DejaVuSansMonoBold_ttf, BinaryData::DejaVuSansMonoBold_ttfSize);
    };

    // Sizes passed to legend / fine / mono are the line height the panel was first drawn with;
    // these turn them into the em size that keeps the capitals the height they were.
    constexpr float legendScale = 0.822f, monoScale = 0.859f;

    inline juce::Font face (const juce::Typeface::Ptr& tf, float em) { return juce::Font (juce::FontOptions (tf).withPointHeight (em)); }
    // The look-and-feel (or editor) holds a Faces for as long as a panel is open, so these are cheap.
    inline juce::Font legend (float size) { juce::SharedResourcePointer<Faces> f; return face (f->bold, size * legendScale); }
    inline juce::Font fine (float size)   { juce::SharedResourcePointer<Faces> f; return face (f->regular, size * legendScale); }
    inline juce::Font mono (float size)   { juce::SharedResourcePointer<Faces> f; return face (f->mono, size * monoScale); }

    // Em (point) height versions, used by PanelKit (kit::font).
    inline juce::Font legendEm (float em) { juce::SharedResourcePointer<Faces> f; return face (f->bold, em); }
    inline juce::Font fineEm (float em)   { juce::SharedResourcePointer<Faces> f; return face (f->regular, em); }
    inline juce::Font monoEm (float em)   { juce::SharedResourcePointer<Faces> f; return face (f->mono, em); }
}
