#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Theme.h"

/** The "Artist Edition" face: a 1200 x 800 design space drawn from assets and code, scaled by an AffineTransform
    (aspect locked, 0.75x to 1.25x). See docs/UI_SPEC.md. The processor does not depend on this class. */
class KiymaEditor : public juce::AudioProcessorEditor
{
public:
    explicit KiymaEditor (KiymaProcessor&);
    ~KiymaEditor() override;

    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    void mouseDown (const juce::MouseEvent&) override;

    static constexpr int kWidth = 1200, kHeight = 800;

    // Deterministic driving, for ui_snapshot (setHover also takes "seal"): the live editor is driven by a 60 Hz vblank callback instead.
    void setFakeAudio (bool on);      // a synthetic signal feeds the strands, the record and the crank
    void setSpinEnabled (bool on);    // the crank's own visual spin (off in snapshots unless asked for)
    void setHover (const juce::String& parameterId);   // shows that knob's tag as if the mouse were over it
    void advance (double seconds);
    /** Mean and p95 milliseconds per frame (animation step + paint) over `frames` frames at a pixel scale, and the one-off cost of building the static layers. */
    void bench (float scale, int frames, double& meanMs, double& p95Ms, double& buildMs);    // steps the animation in 1/60 s frames, from the state it is in

    /** The live tick loop at the 30 Hz cap, simulated (ui_snapshot --bench-live). */
    struct LiveStats { double meanMs = 0, p95Ms = 0, paintedPerSecond = 0, sharePercent = 0; int painted = 0, paintedAfter05 = 0, paintedAfter15 = 0, frames = 0; };
    LiveStats benchLive (float scale, int frames, bool silent);

    class Stage;

private:
    std::unique_ptr<Stage> stage;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KiymaEditor)
};
