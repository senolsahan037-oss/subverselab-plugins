#pragma once

#include <JuceHeader.h>

/*  ELEK 2.0.0 look: direction B "Overview" (docs/ui_directions/B_*.png, scripts/ui_directions/directions.py), on a neutral
    graphite ground (lead / owner 18 Nov 2026: "renkleri değiştirelim"; the previous green-on-green tokens are kept in
    Source/_before_dirB/Theme.h, so this file is the one place to revert).

    Gold is the single accent (primary button, selection, playhead); teal only for progress and active states. Stem colours: see
    below. No orange anywhere (no hue under 33 deg): the old bass orange cf7f5c is gone.
    Contrast (WCAG, measured 18 Nov): text F2EFE7 >= 11.8:1 and textMuted 8FAAA5 >= 5.47:1 on every surface below; ink on gold 7.6:1.
    The type is the family's embedded TeX Gyre Heros, so the panel reads the same on every machine.
*/
namespace Theme
{
    // Ground and surfaces (graphite)
    const juce::Colour background   { 0xff0E1315 };   // header, footer, the lanes' wave ground
    const juce::Colour surface      { 0xff151C1F };   // inspector, overview card, lane label panels
    const juce::Colour surfaceRaised{ 0xff1C2629 };   // hover
    const juce::Colour surfaceHigh  { 0xff243034 };   // tracks, dashed drop outline, the unprocessed part of the mix
    const juce::Colour line         { 0xff2D3B3F };

    // The one accent
    const juce::Colour gold         { 0xffC5A059 };
    const juce::Colour goldLight    { 0xffDFBF7A };
    const juce::Colour goldDark     { 0xff8C6F35 };
    const juce::Colour ink          { 0xff0E1315 };   // text on gold

    // Progress and active states only
    const juce::Colour teal         { 0xff2A9D8F };
    const juce::Colour tealLight    { 0xff3BBFAF };

    // Type
    const juce::Colour text         { 0xffF2EFE7 };   // cream: also the mix waveform and the spine
    const juce::Colour textMuted    { 0xff8faaa5 };
    const juce::Colour warning      { 0xffDFBF7A };   // error lines (gold light; the old salmon e8917f read as orange)

    // Stems: the six bars of the E logo (stem_palette.png), with two changed 18 Nov so all six stay apart (lead: the brand-kit
    // restriction is lifted for this window): piano DFBF7A -> D982A8 (was 11.5 CIELAB dE from the drums gold), guitar 2A9D8F ->
    // 9FD356 (was 16.2 dE from vocals). Smallest pair now piano / other, 28.1 dE. The header logo draws its bars from these.
    const juce::Colour stemVocals   { 0xff5CBA95 };
    const juce::Colour stemDrums    { 0xffA58CC9 };   // 18 Nov (lead, self-review): no longer the accent gold (a gold Drums lane looked selected and hid the playhead); takes the violet Other had. Drum parts use it too
    const juce::Colour stemBass     { 0xff5F9EC4 };
    const juce::Colour stemGuitar   { 0xff9FD356 };
    const juce::Colour stemPiano    { 0xffD982A8 };
    const juce::Colour stemOther    { 0xff9AA9AD };   // neutral grey: "everything else" has no colour of its own
    const juce::Colour stemInstrumental { 0xffF2EFE7 };   // vocal removal: the mix without the vocal, drawn in the mix's cream

    // Radii
    constexpr float radius   = 10.0f;   // overview card
    constexpr float radiusSm = 7.0f;    // option cards, buttons
    constexpr float radiusBt = 7.0f;
    constexpr float radiusLane = 5.0f;

    struct Faces
    {
        juce::Typeface::Ptr bold    = juce::Typeface::createSystemTypefaceFor (BinaryData::texgyreherosbold_otf, BinaryData::texgyreherosbold_otfSize);
        juce::Typeface::Ptr regular = juce::Typeface::createSystemTypefaceFor (BinaryData::texgyreherosregular_otf, BinaryData::texgyreherosregular_otfSize);
    };

    // Sizes as CSS / PIL px: JUCE's height is ascent + descent, which for TeX Gyre Heros sets the letters ~17 % smaller than a
    // browser (or the PIL mockups) does at the same number, so the factor brings them to the mockup's size
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

    /** Width of a tracked string, as drawTracked lays it out. */
    inline float trackedWidth (const juce::String& s, const juce::Font& font, float trackingPx)
    {
        return juce::GlyphArrangement::getStringWidth (font, s) + trackingPx * (float) juce::jmax (0, s.length() - 1);
    }

    /*  Buttons and sliders.

        "variant" property of a TextButton:
          primary  gold fill, ink label (one per screen)
          ghost    line outline on the surface, cream label
          toggle   the 18 px M / S key: line outline; on = filled with textColourOnId (teal for mute, gold for solo), ink label
          wav      the WAV tab of a lane: outline, drag grip, cream label
          switch   a wide toggle (FAST): line outline; on = filled with textColourOnId, ink label
    */
    class Look : public juce::LookAndFeel_V4
    {
    public:
        // Keeps the embedded typefaces loaded for as long as the editor (which owns this Look) is open (one load, not one per
        // repaint: the text visibly shook when every call loaded the fonts again).
        juce::SharedResourcePointer<Faces> faces;

        Look()
        {
            setColour (juce::ScrollBar::thumbColourId, surfaceHigh);
            setColour (juce::ScrollBar::trackColourId, juce::Colours::transparentBlack);
            setColour (juce::TooltipWindow::backgroundColourId, surfaceRaised);
            setColour (juce::TooltipWindow::textColourId, text);
            setColour (juce::TooltipWindow::outlineColourId, line);
        }

        static juce::String variantOf (const juce::Button& b) { return b.getProperties() ["variant"].toString(); }

        void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down) override
        {
            auto r = b.getLocalBounds().toFloat().reduced (0.5f);
            const auto v = variantOf (b);
            const float alpha = b.isEnabled() ? 1.0f : 0.4f;
            if (v == "primary")
            {
                g.setColour ((over || down ? goldLight : gold).withMultipliedAlpha (alpha));
                g.fillRoundedRectangle (r, radiusBt);
                return;
            }
            const float rad = (v == "toggle" || v == "wav") ? 5.0f : radiusBt;
            if ((v == "toggle" || v == "switch") && b.getToggleState())
            {
                g.setColour (b.findColour (juce::TextButton::textColourOnId).withMultipliedAlpha (alpha));
                g.fillRoundedRectangle (r, rad);
                return;
            }
            if (over && b.isEnabled())
            {
                g.setColour (surfaceRaised);
                g.fillRoundedRectangle (r, rad);
            }
            g.setColour ((v == "wav" ? textMuted.withAlpha (0.7f) : line).withMultipliedAlpha (alpha));
            g.drawRoundedRectangle (r, rad, 1.0f);
        }

        juce::Font getTextButtonFont (juce::TextButton& b, int) override
        {
            const auto v = variantOf (b);
            if (v == "toggle") return bold (11.0f);
            if (v == "wav")    return bold (10.5f);
            return v == "primary" ? bold (13.0f) : bold (12.5f);
        }

        void drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool) override
        {
            const auto v = variantOf (b);
            juce::Colour c = v == "primary" || ((v == "toggle" || v == "switch") && b.getToggleState()) ? ink : text;
            if (! b.isEnabled()) c = c.withMultipliedAlpha (0.4f);
            g.setColour (c);
            auto box = b.getLocalBounds().toFloat();
            if (v == "wav")      // a grip (2 x 3 dots) says it can be dragged; the label sits right of it
            {
                for (int gx = 0; gx < 2; ++gx)
                    for (int gy = 0; gy < 3; ++gy)
                        g.fillEllipse (9.0f + (float) gx * 4.0f, box.getCentreY() - 5.0f + (float) gy * 4.0f, 2.0f, 2.0f);
                box.removeFromLeft (14.0f);
            }
            drawTracked (g, b.getButtonText(), box, getTextButtonFont (b, b.getHeight()),
                         v == "wav" ? 0.4f : 0.0f, juce::Justification::centred);
        }

        void drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float,
                               juce::Slider::SliderStyle, juce::Slider& s) override
        {
            const float cy = (float) y + (float) h * 0.5f, th = 3.0f;
            const float a = s.isEnabled() ? 1.0f : 0.35f;
            g.setColour (surfaceHigh);
            g.fillRoundedRectangle ((float) x, cy - th * 0.5f, (float) w, th, th * 0.5f);
            g.setColour (s.findColour (juce::Slider::trackColourId).withMultipliedAlpha (a));
            g.fillRoundedRectangle ((float) x, cy - th * 0.5f, pos - (float) x, th, th * 0.5f);
            g.setColour (text.withMultipliedAlpha (a));
            g.fillEllipse (pos - 5.5f, cy - 5.5f, 11.0f, 11.0f);
        }

        int getSliderThumbRadius (juce::Slider&) override { return 6; }
    };
}
