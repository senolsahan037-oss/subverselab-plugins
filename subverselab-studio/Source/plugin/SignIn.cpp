#include "SignIn.h"

namespace jfui
{
using Phase = jf::rights::Rights::SignIn::Phase;
using R = juce::Rectangle<int>;

SignInScreen::SignInScreen (JingFangProcessor& p) : proc (p)
{
    setOpaque (true);
    setComponentID ("signin");
    for (auto* a : { &signInBtn, &again, &buy }) { addAndMakeVisible (*a); a->setWantsKeyboardFocus (false); }
    addAndMakeVisible (lockup);
    again.setVisible (false);                                  // only while the browser page is waiting
    signInBtn.setTooltip ("Opens subverselab.com in the browser to sign this computer in");
    buy.setTooltip (jf::rights::Rights::buyUrl());
    auto open = [this] (const juce::String& url)
    {
        if (const auto& o = proc.rightsNow().config().openUrl) o (url);
        else juce::URL (url).launchInDefaultBrowser();
    };
    signInBtn.onClick = [this]
    {
        auto& r = proc.rightsNow();
        const auto ph = r.signIn().phase;
        if (ph == Phase::starting || ph == Phase::waiting) r.cancelSignIn();
        else r.startSignIn();
        tick();
    };
    again.onClick = [this] { proc.rightsNow().openVerifyPageAgain(); };
    buy.onClick = [open] { open (jf::rights::Rights::buyUrl()); };
    setSize (kW, kH);
}

void SignInScreen::resized()
{
    card = R (424, 252, 720, 520);
    nameArea = R (56, 40, 520, 64);
    bodyArea = R (card.getX() + 60, card.getY() + 92, 600, 96);
    codeArea = R (card.getX() + 60, card.getY() + 204, 600, 96);
    signInBtn.setBounds (R (0, 0, 340, 46).withCentre ({ card.getCentreX(), card.getY() + 336 }));
    again.setBounds (R (0, 0, 260, 24).withCentre ({ card.getCentreX(), card.getY() + 392 }));
    buy.setBounds (R (0, 0, 520, 24).withCentre ({ card.getCentreX(), card.getY() + 476 }));
    const auto& L = panelLayout()["lockup"];
    if (L.isArray() && L.size() >= 4)
        lockup.setBounds (juce::roundToInt ((double) L[0]), juce::roundToInt ((double) L[1]), juce::roundToInt ((double) L[2]), juce::roundToInt ((double) L[3]));
    else
        lockup.setBounds (kW - 150, kH - 34, 130, 20);
}

juce::String SignInScreen::shownMessage() const
{
    switch (si.phase)
    {
        case Phase::idle:     return {};
        case Phase::starting: return "CONNECTING TO SUBVERSELAB.COM";
        case Phase::waiting:  return "FINISH IN THE BROWSER. THIS SCREEN OPENS BY ITSELF.";
        case Phase::done:
        case Phase::failed:   return si.message;
    }
    return {};
}

void SignInScreen::tick()
{
    const auto now = proc.rightsNow().signIn();
    if (now.phase == si.phase && now.userCode == si.userCode && now.message == si.message) return;
    si = now;
    const bool busy = si.phase == Phase::starting || si.phase == Phase::waiting;
    signInBtn.text = busy ? "CANCEL" : "SIGN IN WITH SUBVERSELAB";
    signInBtn.setVisible (si.phase != Phase::done);
    signInBtn.repaint();
    again.setVisible (si.phase == Phase::waiting && si.verifyUrl.isNotEmpty());
    repaint();
}

void SignInScreen::paint (juce::Graphics& g)
{
    const auto all = getLocalBounds().toFloat();
    g.fillAll (Pal::ground);
    if (const auto& bg = panelImage(); bg.isValid())
    {   // the device, far behind: the settings are all still there
        drawScaledCached (g, bg, all);
        g.fillAll (Pal::ground.withAlpha (0.93f));
    }
    // the product name top left (the interface signature)
    g.setColour (Pal::cream);
    drawTracked (g, "JING FANG", nameArea.toFloat().withHeight (40.0f), heros (34.0f, true), 0.2f, juce::Justification::centredLeft);
    g.setColour (Pal::gold);
    drawTracked (g, "MICROTONAL SYNTHESIZER", nameArea.toFloat().withTrimmedTop (44.0f).withHeight (18.0f).translated (2.0f, 0.0f), heros (12.0f, true), 0.3f,
                 juce::Justification::centredLeft);

    // the card
    const auto c = card.toFloat();
    g.setColour (Pal::groundHi);
    g.fillRoundedRectangle (c, 10.0f);
    g.setColour (Pal::gold);
    g.drawRoundedRectangle (c.reduced (0.75f), 10.0f, 1.5f);
    g.setColour (Pal::cream);
    drawTracked (g, kHeading, { c.getX(), c.getY() + 40.0f, c.getWidth(), 36.0f }, heros (24.0f, true), 0.16f, juce::Justification::centred);
    g.setColour (Pal::cream.withAlpha (0.86f));
    g.setFont (heros (17.0f));
    g.drawFittedText (kBody, bodyArea, juce::Justification::centred, 4, 1.0f);

    // the code area: the phase of the sign-in
    const auto code = codeArea.toFloat();
    if (si.phase == Phase::waiting && si.userCode.isNotEmpty())
    {
        g.setColour (Pal::gold);
        drawTracked (g, "YOUR CODE", code.withHeight (18.0f), heros (12.0f, true), 0.3f, juce::Justification::centred);
        g.setColour (Pal::cream);
        drawTracked (g, si.userCode, code.withTrimmedTop (20.0f).withHeight (50.0f), mono (40.0f), 0.08f, juce::Justification::centred);
        g.setColour (Pal::cream.withAlpha (0.8f));
        drawTracked (g, shownMessage(), code.withTrimmedTop (74.0f).withHeight (20.0f), heros (12.5f, true), 0.12f, juce::Justification::centred);
    }
    else if (const auto m = shownMessage(); m.isNotEmpty())
    {
        g.setColour (si.phase == Phase::done ? Pal::gold : Pal::cream);
        g.setFont (heros (15.0f, true).withExtraKerningFactor (0.1f));
        g.drawFittedText (m, codeArea.reduced (0, 18), juce::Justification::centred, 2, 1.0f);
    }

    g.setColour (Pal::dim);
    drawTracked (g, kSilence, { c.getX() + 40.0f, c.getY() + 430.0f, c.getWidth() - 80.0f, 20.0f }, heros (11.5f, true), 0.12f, juce::Justification::centred);
}

void SignInScreen::Action::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    if (isLink)
    {
        const auto f = heros (13.0f, true);
        const float trackEm = 0.1f;
        g.setColour (hover ? Pal::gold.brighter (0.25f) : Pal::gold);
        drawTracked (g, text, b, f, trackEm, juce::Justification::centred);
        const float w = juce::jmin (b.getWidth(), trackedWidth (text, f, trackEm));
        g.fillRect (juce::Rectangle<float> (b.getCentreX() - w * 0.5f, b.getBottom() - 3.0f, w, 1.0f));
        return;
    }
    if (down) b = b.translated (0.0f, 1.0f);
    g.setColour (hover ? Pal::gold.brighter (0.12f) : Pal::gold);
    g.fillRoundedRectangle (b.reduced (0.5f), 6.0f);
    g.setColour (Pal::goldDeep);
    g.drawRoundedRectangle (b.reduced (0.5f), 6.0f, 1.0f);
    g.setColour (Pal::ground);
    drawTracked (g, text, b, heros (15.0f, true), 0.14f, juce::Justification::centred);
}
} // namespace jfui
