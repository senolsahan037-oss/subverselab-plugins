#pragma once

/*  The activation card laid over a plugin's panel. Unlicensed: it covers the
    panel and cannot be closed (audio passes through untouched meanwhile).
    Licensed: it opens from the panel's LICENSE label, shows the key hint and
    lets the owner release this computer.
*/

#include <JuceHeader.h>
#include "License.h"

namespace svl
{
class LicenseOverlay : public juce::Component
{
public:
    explicit LicenseOverlay (License& l) : lic (l)
    {
        keyField.setTextToShowWhenEmpty ("XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX", ink.withAlpha (0.35f));
        keyField.setFont (juce::FontOptions ("Menlo", 14.0f, juce::Font::plain));
        keyField.setJustification (juce::Justification::centred);
        keyField.setColour (juce::TextEditor::backgroundColourId, juce::Colours::white);
        keyField.setColour (juce::TextEditor::textColourId, ink);
        keyField.setColour (juce::TextEditor::outlineColourId, ink.withAlpha (0.4f));
        keyField.setColour (juce::TextEditor::focusedOutlineColourId, gold);
        keyField.onReturnKey = [this] { doActivate(); };
        addAndMakeVisible (keyField);

        for (auto* b : { &primary, &buy, &close })
        {
            b->setColour (juce::TextButton::buttonColourId, ink);
            b->setColour (juce::TextButton::textColourOffId, paper);
            addAndMakeVisible (*b);
        }
        buy.setColour (juce::TextButton::buttonColourId, gold);
        buy.setColour (juce::TextButton::textColourOffId, ink);
        close.setColour (juce::TextButton::buttonColourId, paper.darker (0.08f));
        close.setColour (juce::TextButton::textColourOffId, ink);

        primary.onClick = [this] { lic.isLicensed() ? doDeactivate() : doActivate(); };
        buy.onClick = [this] { juce::URL (lic.config().buyUrl).launchInDefaultBrowser(); };
        close.onClick = [this] { if (lic.isLicensed()) setVisible (false); };
        refresh();
    }

    /** Call after the licence state may have changed. */
    void refresh()
    {
        const bool on = lic.isLicensed();
        keyField.setVisible (! on);
        primary.setButtonText (on ? "RELEASE THIS COMPUTER" : "ACTIVATE");
        buy.setVisible (! on);
        close.setVisible (on);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black.withAlpha (0.72f));
        const auto card = cardBounds();
        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.fillRoundedRectangle (card.translated (0.0f, 8.0f), 10.0f);
        g.setColour (paper);
        g.fillRoundedRectangle (card, 10.0f);
        g.setColour (gold);
        g.drawRoundedRectangle (card.reduced (8.0f), 6.0f, 1.0f);

        auto r = card.reduced (34.0f, 28.0f);
        const bool on = lic.isLicensed();
        g.setColour (ink);
        g.setFont (juce::Font (juce::FontOptions ("Helvetica Neue", 20.0f, juce::Font::bold)).withExtraKerningFactor (0.18f));
        g.drawText (on ? lic.config().product + "  LICENSED" : "ACTIVATE " + lic.config().product, r.removeFromTop (28.0f), juce::Justification::centred);
        g.setFont (juce::FontOptions ("Helvetica Neue", 13.0f, juce::Font::plain));
        g.setColour (ink.withAlpha (0.8f));
        const juce::String line = on
            ? "Key ending " + lic.keyHint() + " is active on this computer."
            : "Paste the licence key from your e-mail.\nNo key yet? Buy one, or redeem your DM code at checkout.";
        g.drawFittedText (line, r.removeFromTop (44.0f).toNearestInt(), juce::Justification::centred, 3);

        g.setFont (juce::FontOptions ("Helvetica Neue", 12.0f, juce::Font::plain));
        g.setColour (messageIsError ? juce::Colour (0xffB84A36) : juce::Colour (0xff2A7A6F));
        g.drawFittedText (message, messageArea().toNearestInt(), juce::Justification::centred, 2);

        if (! on)
        {
            g.setColour (ink.withAlpha (0.55f));
            g.setFont (juce::FontOptions ("Helvetica Neue", 10.5f, juce::Font::plain));
            g.drawText ("Audio passes through untouched until " + lic.config().product + " is activated.",
                        card.reduced (20.0f).removeFromBottom (16.0f), juce::Justification::centred);
        }
    }

    void resized() override
    {
        const auto card = cardBounds().reduced (34.0f, 28.0f);
        auto r = card.withTrimmedTop (84.0f);
        keyField.setBounds (r.removeFromTop (34.0f).toNearestInt());
        r.removeFromTop (14.0f);
        auto row = r.removeFromTop (34.0f);
        const bool on = lic.isLicensed();
        if (on)
        {
            primary.setBounds (row.removeFromLeft (row.getWidth() * 0.62f).toNearestInt());
            row.removeFromLeft (10.0f);
            close.setBounds (row.toNearestInt());
        }
        else
        {
            primary.setBounds (row.removeFromLeft (row.getWidth() * 0.5f - 5.0f).toNearestInt());
            row.removeFromLeft (10.0f);
            buy.setBounds (row.toNearestInt());
        }
    }

private:
    juce::Rectangle<float> cardBounds() const
    {
        return juce::Rectangle<float> (480.0f, 300.0f).withCentre (getLocalBounds().toFloat().getCentre());
    }
    juce::Rectangle<float> messageArea() const
    {
        return cardBounds().reduced (34.0f, 28.0f).withTrimmedTop (84.0f + 34.0f + 14.0f + 34.0f + 8.0f).withHeight (34.0f);
    }

    void setMessage (bool ok, const juce::String& text)
    {
        messageIsError = ! ok;
        message = text;
        refresh();
        resized();
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

public:
    std::function<void()> onLicensed;

private:
    License& lic;
    juce::TextEditor keyField;
    juce::TextButton primary { "ACTIVATE" }, buy { "BUY  /  REDEEM CODE" }, close { "CLOSE" };
    juce::String message;
    bool messageIsError = false;

    const juce::Colour paper { 0xffF2EFE7 }, ink { 0xff033733 }, gold { 0xffC5A059 };
};
} // namespace svl
