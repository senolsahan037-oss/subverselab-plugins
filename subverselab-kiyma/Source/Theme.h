#pragma once

#include <JuceHeader.h>
#include <BinaryData.h>

// KIYMA "Artist Edition" palette and typefaces. This is not the SubverseLab palette: a restrained one-off
// series look (painted iron, enamel, copper), no neon and no pure white.
namespace Theme
{
    const juce::Colour backdrop  { 0xff121112 };
    const juce::Colour cream     { 0xffefe6d2 };
    const juce::Colour oxblood   { 0xff7a1712 };
    const juce::Colour copper    { 0xffb8673f };
    const juce::Colour silver    { 0xff9a9a98 };
    const juce::Colour iron      { 0xff1b1a1a };
    const juce::Colour strandRed { 0xff6e1a14 };
    const juce::Colour record    { 0xff0d0d0e };

    /** Sizes are em sizes in pixels (the same numbers a design tool uses). Shrikhand: the logo, the label on the record and the big numerals. */
    inline juce::Font display (float height)
    {
        static const auto face = juce::Typeface::createSystemTypefaceFor (BinaryData::ShrikhandRegular_ttf, (size_t) BinaryData::ShrikhandRegular_ttfSize);
        return juce::Font (juce::FontOptions (face).withPointHeight (height));
    }

    /** DejaVu Sans Mono Bold: every label and value. */
    inline juce::Font mono (float height)
    {
        static const auto face = juce::Typeface::createSystemTypefaceFor (BinaryData::DejaVuSansMonoBold_ttf, (size_t) BinaryData::DejaVuSansMonoBold_ttfSize);
        return juce::Font (juce::FontOptions (face).withPointHeight (height));
    }

    inline float textWidth (const juce::Font& f, const juce::String& s)
    {
        juce::GlyphArrangement ga;
        ga.addLineOfText (f, s, 0.0f, 0.0f);
        return ga.getBoundingBox (0, -1, true).getWidth();
    }
}
