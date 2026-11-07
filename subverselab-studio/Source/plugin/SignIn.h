// JING FANG's sign-in screen (the licence gate, owner 7 Nov 2026: only buyers use it). Shown over the whole panel while this
// computer does not own JING FANG; the processor plays silence meanwhile. One key signs in through the browser (device flow,
// Licensing/Rights.h): the browser opens, the panel shows the code, Rights polls until the account approves it. No key to type.
// A link opens the product room. The panel underneath keeps every setting. Drawn at the panel's logical size (kW x kH) and
// scaled with the window like the panel; the product name top left, the SubverseLab icon bottom right (linked).
#pragma once
#include "Ui.h"

namespace jfui
{
class SignInScreen : public juce::Component
{
public:
    explicit SignInScreen (JingFangProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;
    void tick();                                  // editor timer (30 Hz) while shown: follows the sign-in phase

    /** a text button on the screen: the gold key (SIGN IN) or a gold underlined link */
    struct Action : juce::Component, juce::SettableTooltipClient
    {
        Action (juce::String t, bool link) : text (std::move (t)), isLink (link) { setMouseCursor (juce::MouseCursor::PointingHandCursor); }
        juce::String text;
        bool isLink;
        std::function<void()> onClick;
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override { down = true; repaint(); }
        void mouseUp (const juce::MouseEvent& e) override { down = false; repaint(); if (contains (e.getPosition()) && onClick) onClick(); }
        void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
        void mouseExit (const juce::MouseEvent&) override { hover = false; repaint(); }
        bool down = false, hover = false;
    };

    // ---- tests
    Action& signInKey() noexcept { return signInBtn; }
    Action& againLink() noexcept { return again; }
    Action& buyLink() noexcept { return buy; }
    Lockup& lockupLink() noexcept { return lockup; }
    juce::Rectangle<int> cardBounds() const noexcept { return card; }
    juce::Rectangle<int> codeBounds() const noexcept { return codeArea; }
    juce::Rectangle<int> textBounds() const noexcept { return bodyArea; }
    juce::Rectangle<int> nameBounds() const noexcept { return nameArea; }
    juce::String shownCode() const { return si.phase == jf::rights::Rights::SignIn::Phase::waiting ? si.userCode : juce::String(); }
    juce::String shownMessage() const;            // the line in the code area now

    static constexpr const char* kHeading = "SIGN IN TO PLAY JING FANG";
    static constexpr const char* kBody = "JING FANG plays on a computer that is signed in with the subverselab.com account that bought it. "
                                         "Sign in once: the browser opens and there is no key to type. After that it works offline for 45 days.";
    static constexpr const char* kSilence = "UNTIL THEN JING FANG MAKES NO SOUND. A PROJECT KEEPS ITS SETTINGS.";

private:
    JingFangProcessor& proc;
    Action signInBtn { "SIGN IN WITH SUBVERSELAB", false }, again { "OPEN THE PAGE AGAIN", true },
           buy { "NOT BOUGHT YET? SUBVERSELAB.COM/PLUGINS/JINGFANG", true };
    Lockup lockup;
    jf::rights::Rights::SignIn si;
    juce::Rectangle<int> card, bodyArea, codeArea, nameArea;
};
} // namespace jfui
