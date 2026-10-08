// ALEV panel: one round unit. Three concentric brushed-steel ring knobs (HIGH inner, MID, LOW outer) that turn a full
// circle, each with a raised black-steel pointer bar; the ember in the centre is the only light and shows the heat of the
// three bands. On the bezel: a curved dry/wet fader on the left, three watch-style pushers on the right that choose how
// the rings move together (FREE / LINK / GANG). See SPEC.md "KOR v2" and STATUS.md (panel v4).
#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

class KorEditor final : public juce::AudioProcessorEditor,
                        private juce::Timer
{
public:
    // Wider than the unit: the pushers stand out of the case on the right (owner, 8 Oct: "like a watch").
    static constexpr int kW = 480, kH = 440;

    // Geometry (logical px, centre of the unit).
    static constexpr float kCx = 240.0f, kCy = 220.0f;
    static constexpr float kUnitR = 214.0f;                // the round unit (6 px margin top and bottom)
    static constexpr float kBezelIn = 190.0f;              // brushed bezel 190 .. 214: model line, badge, fader, pusher legends
    static constexpr float kBezelMid = 0.5f * (kBezelIn + kUnitR);
    // Ring positions, inner to outer. Widths shrink outwards: the unit is domed, so the outer rings are seen more
    // foreshortened (owner, 8 Oct). Bands sit HIGH inner, MID, LOW outer (owner, 8 Oct, panel v4).
    static constexpr float kPosW[3] { 36.0f, 28.0f, 20.0f };
    static constexpr float kRingGap = 3.0f;
    static constexpr float kPosOut2 = 186.0f;
    static constexpr float kPosOut1 = kPosOut2 - 20.0f - kRingGap;   // 163
    static constexpr float kPosOut0 = kPosOut1 - 28.0f - kRingGap;   // 132
    static constexpr float kEmberR = kPosOut0 - 36.0f - 6.0f;        // 90

    // The dry/wet fader: an arc slot in the bezel on the left, DRY at the bottom end, WET at the top (JUCE angles,
    // radians clockwise from 12 o'clock).
    static constexpr float kFaderA0 = 4.26f, kFaderA1 = 5.17f;       // 244 .. 296 degrees
    // The pushers, at 2, 3 and 4 o'clock.
    static constexpr float kPushA[3] { 1.0472f, 1.5708f, 2.0944f };

    enum Control { none = -1, low = 0, mid = 1, high = 2, mix = 3, push0 = 4, push1 = 5, push2 = 6 };
    enum Link { linkFree = 0, linkFollow = 1, linkGang = 2 };

    explicit KorEditor (KorAudioProcessor&);
    ~KorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override {}

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void visibilityChanged() override { updateTimer(); }
    void parentHierarchyChanged() override { updateTimer(); }

    /** The 30 Hz tick: reads the band glows from the processor, advances the flicker, repaints.
        Public so tests/ui_snapshot.cpp can drive it at a deterministic rate. */
    void timerCallback() override;

    /** Which control a point (editor coordinates) belongs to. */
    static Control controlAt (juce::Point<float> p) noexcept;
    static int position (int ring) noexcept { return ring == high ? 0 : ring == mid ? 1 : 2; }
    static float ringWidth (int ring) noexcept { return kPosW[position (ring)]; }
    static float ringOuter (int ring) noexcept { const int p = position (ring); return p == 0 ? kPosOut0 : p == 1 ? kPosOut1 : kPosOut2; }
    static float ringInner (int ring) noexcept { return ringOuter (ring) - ringWidth (ring); }
    /** LINK mode (owner, 8 Oct): the heat spreads to the neighbouring bands in frequency, each step 30 % behind the
        one before: the next band takes 70 % of the move, the band after it 49 %. HIGH -> MID 70 %, LOW 49 %;
        LOW -> MID 70 %, HIGH 49 %; MID -> HIGH and LOW 70 % each. */
    static float linkShare (int turned, int band) noexcept { return std::pow (kFollow, (float) std::abs (band - turned)); }
    static constexpr float kFollow = 0.7f;

private:
    void updateTimer();
    float value (int c) const noexcept;
    void setValue (int c, float v);
    int linkMode() const noexcept;
    void rebuildCaches (float scale);
    void buildCurvedText (juce::Path& dest, const juce::String& text, float rMid, float em = 10.0f, float tracking = 2.2f) const;
    void paintEmber (juce::Graphics&);
    void paintPointer (juce::Graphics&, int ring);
    void paintFader (juce::Graphics&);
    void paintPushers (juce::Graphics&);
    /** Starts a gesture on `c` and on the rings that move with it in the current link mode. */
    void beginMove (int c);
    void endMove();

    KorAudioProcessor& proc;
    juce::RangedAudioParameter* params[4] {};
    juce::RangedAudioParameter* linkParam = nullptr;

    // Caches, rebuilt only when the physical pixel scale changes.
    float cacheScale = 0.0f;
    juce::Image staticLayer;       // corners, face, bezel, the three steel rings, the fader slot, the legends
    juce::Image wordmark, mark;
    juce::Image coalTex;           // the ember's crust: cracked cells, dark where the crust is thick, clear in the cracks

    // Vector parts built once and reused every frame.
    juce::Path labelPath[3], namePath, bandName[4], modelLine, pushLabel[3], dryLabel, wetLabel;
    juce::Path pointer[3], pointerInlay[3], pointerSide[3][2];
    juce::ColourGradient emberGrad[3], coalGrad, rimGrad;

    // Live state.
    float glow[3] {}, flick[3] { 1.0f, 1.0f, 1.0f };
    double clock = 0.0;            // seconds of ticks (deterministic: advanced only by timerCallback)
    int active = none;             // control being dragged
    float moveBy[4] {};            // share of the drag each control takes (0 = not moving): 1 for the dragged one, 0.7 or 1 for followers
    float lastY = 0.0f;
    float dragValue[4] {};         // unquantised, so fine drags are not lost to the parameter step

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KorEditor)
};
