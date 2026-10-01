#pragma once

/*  The activation card laid over a plugin's panel. Unlicensed: it covers the
    panel and cannot be closed (audio passes through untouched meanwhile).
    Licensed: it opens from the panel's LICENSE label, shows the key hint and
    lets the owner release this computer.

    It is drawn as one more part of the hardware front panel: the panel dimmed
    behind it, an anodized plate with a gold hairline, cream silkscreen
    legends, the key in a recessed phosphor window and a cream push-button cap.
    Typefaces come from the plugin through Style (its Theme::legend / fine /
    mono, which are compiled into the plugin); the overlay names no font itself.
*/

#include <JuceHeader.h>
#include "License.h"

namespace svl
{
class LicenseOverlay : public juce::Component
{
public:
    /** The plugin's look. Defaults are the SubverseLab palette and the default
        sans-serif, which each plugin's look-and-feel points at its embedded face. */
    struct Style
    {
        juce::Colour panel { 0xff033733 }, panelDeep { 0xff022523 }, ink { 0xffF2EFE7 }, capCream { 0xffE6DFCC },
                     gold { 0xffC5A059 }, goldDark { 0xff8C6F35 }, screen { 0xff021512 }, vfd { 0xff63F2D8 },
                     danger { 0xffe8917f };
        std::function<juce::Font (float)> legend = [] (float s) { return juce::Font (juce::FontOptions (s * 0.822f).withStyle ("Bold")); };
        std::function<juce::Font (float)> fine   = [] (float s) { return juce::Font (juce::FontOptions (s * 0.822f)); };
        std::function<juce::Font (float)> mono   = [] (float s) { return juce::Font (juce::FontOptions (s * 0.859f)); };
    };

    explicit LicenseOverlay (License& l) : LicenseOverlay (l, Style()) {}

    LicenseOverlay (License& l, Style s) : lic (l), st (std::move (s))
    {
        keyField.setTextToShowWhenEmpty ("Licence key", st.vfd.withAlpha (0.28f));
        keyField.setFont (st.mono (14.0f));
        keyField.setJustification (juce::Justification::centred);
        keyField.setIndents (8, 0);
        keyField.setBorder ({});
        keyField.setColour (juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
        keyField.setColour (juce::TextEditor::textColourId, st.vfd);
        keyField.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        keyField.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
        keyField.setColour (juce::TextEditor::highlightColourId, st.vfd.withAlpha (0.25f));
        keyField.setColour (juce::TextEditor::highlightedTextColourId, st.vfd);
        keyField.setColour (juce::CaretComponent::caretColourId, st.vfd);
        keyField.onReturnKey = [this] { doActivate(); };
        addAndMakeVisible (keyField);

        addAndMakeVisible (primary);
        addAndMakeVisible (close);
        primary.onClick = [this] { lic.isLicensed() ? doDeactivate() : doActivate(); };
        close.onClick = [this] { if (lic.isLicensed()) setVisible (false); };
        refresh();
    }

    /** Call after the licence state may have changed. */
    void refresh()
    {
        const bool on = lic.isLicensed();
        keyField.setVisible (! on);
        primary.setText (on ? "RELEASE THIS COMPUTER" : "ACTIVATE");
        close.setVisible (on);
        resized();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black.withAlpha (0.66f));   // the panel, dimmed
        const auto card = cardBounds();
        const bool on = lic.isLicensed();

        // The plate: anodized panel metal with a hairline of gold set in from the edge.
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRoundedRectangle (card.translated (0.0f, 6.0f).expanded (2.0f), 6.0f);
        g.setGradientFill (juce::ColourGradient (st.panel.brighter (0.12f), card.getX(), card.getY(), st.panelDeep, card.getX(), card.getBottom(), false));
        g.fillRoundedRectangle (card, 5.0f);
        g.setColour (juce::Colours::white.withAlpha (0.10f));
        g.drawLine (card.getX() + 6.0f, card.getY() + 1.0f, card.getRight() - 6.0f, card.getY() + 1.0f, 1.0f);
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.drawRoundedRectangle (card, 5.0f, 1.0f);
        g.setColour (st.goldDark.withAlpha (0.9f));
        g.drawRoundedRectangle (card.reduced (9.0f), 3.0f, 1.0f);

        const auto r = inner();
        g.setColour (st.ink);
        tracked (g, lic.config().product, st.legend (22.0f), r.withHeight (28.0f), 0.26f);
        g.setColour (st.gold);
        tracked (g, on ? "LICENSED" : "ACTIVATE", st.legend (10.0f), r.withTrimmedTop (30.0f).withHeight (14.0f), 0.34f);

        // The key window: recessed, dark glass, as the CRT and VFD are.
        const auto well = fieldArea();
        g.setColour (juce::Colour (0xff011512));
        g.fillRoundedRectangle (well.expanded (3.0f), 5.0f);
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.drawRoundedRectangle (well.expanded (3.0f).translated (0.0f, 1.0f), 5.0f, 1.0f);
        g.setColour (st.screen);
        g.fillRoundedRectangle (well, 3.0f);
        if (keyField.hasKeyboardFocus (true))
        {
            g.setColour (st.vfd.withAlpha (0.35f));
            g.drawRoundedRectangle (well, 3.0f, 1.0f);
        }
        if (on)
        {
            g.setColour (st.vfd);
            g.setFont (st.mono (14.0f));
            g.drawText ("KEY ENDING  " + lic.keyHint(), well.toNearestInt(), juce::Justification::centred, false);
        }

        if (! on)
        {
            const auto f = st.fine (10.5f);
            const juce::String lead ("Buy or find your key at"), link ("subverselab.com/launch");
            auto width = [&f] (const juce::String& t)
            {
                juce::GlyphArrangement ga;
                ga.addLineOfText (f, t, 0.0f, 0.0f);
                return ga.getBoundingBox (0, -1, true).getWidth();
            };
            const float w1 = width (lead), w2 = width (link);
            const auto la = linkArea().toFloat();
            const float x0 = la.getCentreX() - (w1 + 4.0f + w2) * 0.5f;
            g.setFont (f);
            g.setColour (st.ink.withAlpha (0.72f));
            g.drawText (lead, juce::Rectangle<float> (x0, la.getY(), w1 + 2.0f, la.getHeight()), juce::Justification::centredLeft, false);
            g.setColour (st.gold);
            g.drawText (link, juce::Rectangle<float> (x0 + w1 + 4.0f, la.getY(), w2 + 4.0f, la.getHeight()), juce::Justification::centredLeft, false);
        }

        g.setColour (messageIsError ? st.danger : st.ink.withAlpha (0.75f));
        g.setFont (st.fine (10.5f));
        g.drawText (message, messageArea().toFloat(), juce::Justification::centred, true);
    }

    void resized() override
    {
        const auto r = inner();
        keyField.setBounds (fieldArea().toNearestInt().reduced (6, 2));
        const bool on = lic.isLicensed();
        const auto row = buttonRow();
        if (on)
        {
            const int gap = 12, w1 = 196, w2 = 96;
            const int x = row.getCentreX() - (w1 + gap + w2) / 2;
            primary.setBounds (x, row.getY(), w1, row.getHeight());
            close.setBounds (x + w1 + gap, row.getY(), w2, row.getHeight());
        }
        else
        {
            primary.setBounds (row.withSizeKeepingCentre (150, row.getHeight()));
        }
        juce::ignoreUnused (r);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! lic.isLicensed() && linkArea().contains (e.getPosition()))
            juce::URL (lic.config().buyUrl).launchInDefaultBrowser();
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        setMouseCursor (! lic.isLicensed() && linkArea().contains (e.getPosition()) ? juce::MouseCursor::PointingHandCursor
                                                                                   : juce::MouseCursor::NormalCursor);
    }

    std::function<void()> onLicensed;

private:
    /** A push button with a cream cap, like the panel's own. */
    class Cap : public juce::Button
    {
    public:
        Cap (const Style& s, bool secondaryLook) : juce::Button ({}), st (s), secondary (secondaryLook) {}
        void setText (const juce::String& t) { text = t; repaint(); }
        void paintButton (juce::Graphics& g, bool over, bool down) override
        {
            const float dim = isEnabled() ? 1.0f : 0.45f;
            auto cap = getLocalBounds().toFloat().reduced (1.0f).translated (0.0f, down ? 1.0f : 0.0f);
            if (secondary)
            {
                g.setColour (st.ink.withAlpha ((over ? 0.9f : 0.55f) * dim));
                g.drawRoundedRectangle (cap, 4.0f, 1.0f);
                g.setFont (st.legend (10.5f).withExtraKerningFactor (0.16f));
                g.drawText (text, cap, juce::Justification::centred, false);
                return;
            }
            g.setColour (juce::Colour (0xff011512));
            g.fillRoundedRectangle (getLocalBounds().toFloat(), 5.0f);
            g.setGradientFill (juce::ColourGradient (st.capCream.brighter (over ? 0.08f : 0.0f).withMultipliedAlpha (dim), cap.getX(), cap.getY(),
                                                     juce::Colour (0xffC4BBA3).withMultipliedAlpha (dim), cap.getX(), cap.getBottom(), false));
            g.fillRoundedRectangle (cap.reduced (2.0f), 4.0f);
            g.setColour (juce::Colours::white.withAlpha (0.35f * dim));
            g.drawLine (cap.getX() + 6.0f, cap.getY() + 3.0f, cap.getRight() - 6.0f, cap.getY() + 3.0f, 1.0f);
            g.setColour (juce::Colours::black.withAlpha (0.25f));
            g.drawRoundedRectangle (cap.reduced (2.0f), 4.0f, 1.0f);
            g.setColour (st.panel.withAlpha (0.9f * dim));
            g.setFont (st.legend (11.0f).withExtraKerningFactor (0.16f));
            g.drawText (text, cap.reduced (2.0f), juce::Justification::centred, false);
        }
    private:
        const Style& st;
        bool secondary;
        juce::String text;
    };

    void tracked (juce::Graphics& g, const juce::String& s, juce::Font f, juce::Rectangle<float> r, float tracking) const
    {
        g.setFont (f.withExtraKerningFactor (tracking));
        g.drawText (s, r, juce::Justification::centred, false);
    }

    juce::Rectangle<float> cardBounds() const
    {
        return juce::Rectangle<float> (440.0f, 252.0f).withCentre (getLocalBounds().toFloat().getCentre());
    }
    juce::Rectangle<float> inner() const { return cardBounds().reduced (34.0f, 28.0f); }
    juce::Rectangle<float> fieldArea() const { return inner().withTrimmedTop (62.0f).withHeight (36.0f); }
    juce::Rectangle<int> buttonRow() const { return inner().withTrimmedTop (62.0f + 36.0f + 18.0f).withHeight (36.0f).toNearestInt(); }
    juce::Rectangle<int> linkArea() const { return inner().withTrimmedTop (62.0f + 36.0f + 18.0f + 36.0f + 12.0f).withHeight (16.0f).toNearestInt(); }
    juce::Rectangle<int> messageArea() const { return inner().withTrimmedTop (62.0f + 36.0f + 18.0f + 36.0f + 12.0f + 18.0f).withHeight (16.0f).toNearestInt(); }

    void setMessage (bool ok, const juce::String& text)
    {
        messageIsError = ! ok;
        message = text;
        refresh();
    }

    void doActivate()
    {
        primary.setEnabled (false);
        setMessage (true, "Checking the key...");
        juce::Component::SafePointer<LicenseOverlay> self (this);
        lic.activate (keyField.getText(), [self] (bool ok, juce::String text)
        {
            if (self == nullptr) return;
            self->primary.setEnabled (true);
            self->setMessage (ok, text);
            if (ok && self->onLicensed) self->onLicensed();
        });
    }

    void doDeactivate()
    {
        primary.setEnabled (false);
        setMessage (true, "Releasing this computer...");
        juce::Component::SafePointer<LicenseOverlay> self (this);
        lic.deactivate ([self] (bool ok, juce::String text)
        {
            if (self == nullptr) return;
            self->primary.setEnabled (true);
            self->setMessage (ok, text);
        });
    }

    License& lic;
    Style st;
    juce::TextEditor keyField;
    Cap primary { st, false }, close { st, true };
    juce::String message;
    bool messageIsError = false;
};
} // namespace svl
