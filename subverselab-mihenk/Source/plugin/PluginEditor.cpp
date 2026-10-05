#include "PluginEditor.h"

namespace mihenk
{
float MihenkEditor::fitScale()
{
    // Largest zoom that fits the display's usable area (room for a title bar; the menu bar / dock are outside userArea).
    if (auto* d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
    {
        const auto area = d->userArea;
        if (area.getWidth() >= 600 && area.getHeight() >= 400)
            return juce::jmin ((float) (area.getWidth() - 40) / (float) MainComponent::panelWidth,
                               (float) (area.getHeight() - 70) / (float) MainComponent::panelHeight);
    }
    return 1.6f;   // no usable display information: don't limit
}

float MihenkEditor::defaultScale()
{
    // Largest zoom up to 100 % that fits the main display (with room for a title bar and the menu bar / dock).
    float s = 1.0f;
    if (auto* d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
    {
        const auto area = d->userArea;
        if (area.getWidth() < 600 || area.getHeight() < 400) return 1.0f;      // no usable display information
        s = juce::jmin (1.0f, (float) (area.getWidth() - 40) / (float) MainComponent::panelWidth,
                        (float) (area.getHeight() - 70) / (float) MainComponent::panelHeight);
    }
    return juce::jlimit (0.6f, 1.0f, s);
}

MihenkEditor::MihenkEditor (MihenkProcessor& p) : juce::AudioProcessorEditor (p), processor (p), panel (p)
{
    addAndMakeVisible (panel);
    tooltips = std::make_unique<juce::TooltipWindow> (this, 600);
    const float W = (float) MainComponent::panelWidth, H = (float) MainComponent::panelHeight;
    setResizable (true, true);
    setResizeLimits ((int) (W * 0.6f), (int) (H * 0.6f), (int) (W * 1.6f), (int) (H * 1.6f));
    if (auto* c = getConstrainer()) c->setFixedAspectRatio ((double) W / (double) H);
    // A saved zoom is kept, but never larger than what fits this screen (a project saved on a bigger display).
    // The desktop app always opens at the size that fits the screen; in a DAW the project's zoom is kept.
    const float s = (p.uiScale > 0.0f && ! p.isStandaloneApp()) ? juce::jlimit (0.6f, juce::jmax (0.6f, fitScale()), p.uiScale) : defaultScale();
    setSize (juce::roundToInt (W * s), juce::roundToInt (H * s));
}

MihenkEditor::~MihenkEditor() = default;

void MihenkEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff070a09));
}

void MihenkEditor::resized()
{
    // Fit both ways and centre, so the whole panel is always visible whatever the window's proportions.
    const float W = (float) MainComponent::panelWidth, H = (float) MainComponent::panelHeight;
    const float s = juce::jmin ((float) getWidth() / W, (float) getHeight() / H);
    panel.setBounds (0, 0, MainComponent::panelWidth, MainComponent::panelHeight);
    panel.setTransform (juce::AffineTransform::scale (s).translated (((float) getWidth() - W * s) * 0.5f, ((float) getHeight() - H * s) * 0.5f));
    processor.uiScale = s;
}
} // namespace mihenk
