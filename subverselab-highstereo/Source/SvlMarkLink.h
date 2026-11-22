// SvlMarkLink.h: the SubverseLab icon's link (UI signature 3, owner 14 Nov 2026). One header, identical in every project.
//
// MASTER COPY: subverselab-plugin-common/MarkLink/SvlMarkLink.h. Every project compiles its own copy, Source/SvlMarkLink.h,
// so the standalone CI repo (which syncs only Source/) still builds. subverselab-plugin-common/MarkLink/sync_marklink.sh
// copies it into the projects and checks the md5.
//
// The rule: the bottom-right mark is the SubverseLab icon only (no wordmark), and a click on it opens THAT product's
// HTML manual, https://subverselab.com/plugins/<slug>/manual. The panel keeps drawing the icon itself (its own art and
// opacity); MarkLink is an invisible child laid exactly over the drawn icon that takes the click. The URL is passed as
// one full literal so the built binary carries the exact string (checked with `strings`).
#pragma once

#include <JuceHeader.h>
#include <functional>

namespace svl
{
/** Where the icon goes: right-aligned at `right`, vertically centred on `centreY`, `h` tall, the image's own aspect. */
inline juce::Rectangle<float> markRect (const juce::Image& mark, float right, float centreY, float h)
{
    const float w = mark.isNull() ? h : h * (float) mark.getWidth() / (float) mark.getHeight();
    return { right - w, centreY - h * 0.5f, w, h };
}

/** An invisible click target over the drawn SubverseLab icon: a pointing hand, a tooltip, and a click that opens the
    product's manual in the default browser. It paints nothing. */
class MarkLink : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit MarkLink (const char* manualUrl) : url (manualUrl)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setTooltip ("Manual");
        setTitle ("SubverseLab manual");
        setWantsKeyboardFocus (false);
        setInterceptsMouseClicks (true, false);
    }

    const juce::String& getUrl() const noexcept { return url; }

    /** Tests: set to catch the click instead of opening a browser (empty = the browser again). */
    std::function<void()> onOpenForTest;

    void open() const
    {
        if (onOpenForTest) { onOpenForTest(); return; }
        juce::URL (url).launchInDefaultBrowser();
    }

    void paint (juce::Graphics&) override {}   // the panel draws the icon underneath
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mouseWasClicked() && ! e.mods.isPopupMenu() && contains (e.getPosition()))
            open();
    }

private:
    juce::String url;
};

/** Tests: the first MarkLink under `c` (depth first), or nullptr. */
inline MarkLink* findMarkLink (juce::Component& c)
{
    if (auto* m = dynamic_cast<MarkLink*> (&c)) return m;
    for (auto* ch : c.getChildren())
        if (auto* m = findMarkLink (*ch)) return m;
    return nullptr;
}

/** Tests: true if `root` holds a visible MarkLink that opens `url` and is what a click at its centre reaches from `root`
    (nothing laid over it). `why` gets the measured facts either way. */
inline bool markLinkReachable (juce::Component& root, const juce::String& url, juce::String& why)
{
    auto* m = findMarkLink (root);
    if (m == nullptr) { why = "no MarkLink in the editor"; return false; }
    const auto centre = root.getLocalPoint (m, m->getLocalBounds().getCentre());
    const bool rootWasVisible = root.isVisible();   // an editor made in a test is not on screen; a host shows it
    root.setVisible (true);
    auto* hit = root.getComponentAt (centre);
    root.setVisible (rootWasVisible);
    bool shown = true;
    for (auto* p = static_cast<juce::Component*> (m); p != nullptr && p != &root; p = p->getParentComponent())
        shown = shown && p->isVisible();
    why = "url " + m->getUrl() + ", " + juce::String (m->getWidth()) + " x " + juce::String (m->getHeight()) + " at "
        + root.getLocalArea (m, m->getLocalBounds()).toString() + " in the editor, a click at its centre reaches "
        + (hit == m ? juce::String ("the link") : hit == nullptr ? juce::String ("nothing") : "another component (" + hit->getTitle() + ")");
    return shown && hit == m && m->getUrl() == url;
}
} // namespace svl
