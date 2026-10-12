#pragma once

#include <JuceHeader.h>

/*  ELEK's look: the SubverseLab tool interface (the web Splitter's styles.css), carried over value for value.

    Colours are the site tokens (--bg, --surface, --surface-2/3, --line, --gold, --teal, --text, --text-muted and the five track
    colours, which are shared across the platform so a drum lane is one colour everywhere). Shapes follow the same sheet: cards
    with an 18 px radius, inner tiles and buttons at 12 / 10 px, a 1 px --line border, gold for the one primary action, teal for
    progress and active toggles. The type is the family's embedded TeX Gyre Heros, so the panel reads the same on every machine.
*/
namespace Theme
{
    // Ground and surfaces (--bg, --surface, --surface-2, --surface-3, --line)
    const juce::Colour background   { 0xff032825 };
    const juce::Colour surface      { 0xff063730 };
    const juce::Colour surfaceRaised{ 0xff0a4740 };
    const juce::Colour surfaceHigh  { 0xff0e554a };
    const juce::Colour line         { 0xff1a5b52 };

    // The one accent, shared with the site
    const juce::Colour gold         { 0xffC5A059 };
    const juce::Colour goldLight    { 0xffDFBF7A };
    const juce::Colour goldDark     { 0xff8C6F35 };
    const juce::Colour ink          { 0xff101719 };   // text on gold (.primary)

    const juce::Colour teal         { 0xff2A9D8F };
    const juce::Colour tealLight    { 0xff3BBFAF };

    // Type
    const juce::Colour text         { 0xffF2EFE7 };
    const juce::Colour textMuted    { 0xff8faaa5 };
    const juce::Colour danger       { 0xffe8917f };

    // Stems — the platform's shared track palette
    const juce::Colour stemDrums    { 0xffb8935a };
    const juce::Colour stemBass     { 0xffcf7f5c };
    const juce::Colour stemOther    { 0xffa58cc9 };
    const juce::Colour stemVocals   { 0xff5cba95 };
    const juce::Colour stemInstrumental { 0xff5f9ec4 };

    // Radii (--radius, --radius-sm, buttons)
    constexpr float radius   = 18.0f;
    constexpr float radiusSm = 12.0f;
    constexpr float radiusBt = 10.0f;

    struct Faces
    {
        juce::Typeface::Ptr bold    = juce::Typeface::createSystemTypefaceFor (BinaryData::texgyreherosbold_otf, BinaryData::texgyreherosbold_otfSize);
        juce::Typeface::Ptr regular = juce::Typeface::createSystemTypefaceFor (BinaryData::texgyreherosregular_otf, BinaryData::texgyreherosregular_otfSize);
    };

    // Sizes as CSS px: JUCE's height is ascent + descent, which for TeX Gyre Heros sets the letters ~17 % smaller than a browser
    // does at the same number, so the factor brings them to what the web tool shows
    constexpr float cssPx = 1.17f;
    inline juce::Font bold (float px)    { juce::SharedResourcePointer<Faces> f; return juce::Font (juce::FontOptions (f->bold).withHeight (px * cssPx)); }
    inline juce::Font regular (float px) { juce::SharedResourcePointer<Faces> f; return juce::Font (juce::FontOptions (f->regular).withHeight (px * cssPx)); }

    /** Draws text with letter spacing in pixels (CSS letter-spacing), left / centre / right aligned in the box. */
    inline void drawTracked (juce::Graphics& g, const juce::String& s, juce::Rectangle<float> box, const juce::Font& font,
                             float trackingPx, juce::Justification just)
    {
        juce::GlyphArrangement ga;
        ga.addLineOfText (font, s, 0.0f, 0.0f);
        for (int i = 0; i < ga.getNumGlyphs(); ++i)
            ga.getGlyph (i).moveBy (trackingPx * (float) i, 0.0f);
        auto bb = ga.getBoundingBox (0, -1, false);
        float x = box.getX();
        if (just.testFlags (juce::Justification::horizontallyCentred)) x = box.getCentreX() - bb.getWidth() * 0.5f;
        else if (just.testFlags (juce::Justification::right))          x = box.getRight() - bb.getWidth();
        const float y = box.getCentreY() + (font.getAscent() - font.getDescent()) * 0.5f;
        ga.moveRangeOfGlyphs (0, -1, x - bb.getX(), y);
        ga.draw (g);
    }

    /*  Buttons and sliders in the sheet's terms.

        A TextButton is the default `.mix-toggle` / `.stem-download` (surface-2 fill, line border, 10 px radius). Set the
        property "variant" to "primary" for the gold `.primary` button, or "ghost" for the transparent outline. A toggle that is
        on takes its accent from the button's textColourOnId (teal for mute, gold for solo) on the border and the label.
    */
    class Look : public juce::LookAndFeel_V4
    {
    public:
        // Keeps the embedded typefaces loaded for as long as the editor (which owns this Look) is open. Without a holder,
        // every bold()/regular() call created and dropped the Faces, loading both fonts from memory again: dozens of loads per
        // repaint at 20 repaints a second while a separation ran, and the text visibly shook (TUTKAL's editor holds one too).
        juce::SharedResourcePointer<Faces> faces;

        static juce::String variantOf (const juce::Button& b) { return b.getProperties() ["variant"].toString(); }

        void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down) override
        {
            auto r = b.getLocalBounds().toFloat().reduced (0.5f);
            const auto v = variantOf (b);
            const float alpha = b.isEnabled() ? 1.0f : 0.5f;
            if (v == "primary")
            {
                g.setColour ((over || down ? goldLight : gold).withMultipliedAlpha (alpha));
                g.fillRoundedRectangle (r, radiusBt);
                return;
            }
            if (v != "ghost")
            {
                g.setColour ((over ? surfaceHigh : surfaceRaised).withMultipliedAlpha (alpha));
                g.fillRoundedRectangle (r, radiusBt);
            }
            else if (over)
            {
                g.setColour (surfaceRaised.withMultipliedAlpha (alpha));
                g.fillRoundedRectangle (r, radiusBt);
            }
            const bool on = b.getToggleState();
            g.setColour (on ? b.findColour (juce::TextButton::textColourOnId) : line);
            g.drawRoundedRectangle (r, radiusBt, 1.0f);
        }

        juce::Font getTextButtonFont (juce::TextButton& b, int) override
        {
            return variantOf (b) == "primary" ? bold (14.0f) : bold (13.0f);
        }

        void drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool) override
        {
            const auto v = variantOf (b);
            juce::Colour c = v == "primary" ? ink : (b.getToggleState() ? b.findColour (juce::TextButton::textColourOnId) : text);
            if (! b.isEnabled()) c = c.withMultipliedAlpha (0.5f);
            g.setColour (c);
            drawTracked (g, b.getButtonText(), b.getLocalBounds().toFloat(), getTextButtonFont (b, b.getHeight()),
                         v == "primary" ? 0.3f : 0.2f, juce::Justification::centred);
        }

        void drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float,
                               juce::Slider::SliderStyle, juce::Slider& s) override
        {
            const float cy = (float) y + (float) h * 0.5f, th = 4.0f;
            const auto accent = s.findColour (juce::Slider::trackColourId);
            g.setColour (surfaceHigh);
            g.fillRoundedRectangle ((float) x, cy - th * 0.5f, (float) w, th, th * 0.5f);
            g.setColour (accent);
            g.fillRoundedRectangle ((float) x, cy - th * 0.5f, pos - (float) x, th, th * 0.5f);
            g.setColour (text);
            g.fillEllipse (pos - 6.0f, cy - 6.0f, 12.0f, 12.0f);
        }

        int getSliderThumbRadius (juce::Slider&) override { return 6; }
    };
}
