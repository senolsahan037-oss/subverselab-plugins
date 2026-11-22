// HIGH STEREO left screen: STAGE (SPEC.md section 7). A real goniometer laid out as a 360-degree stage, and the STAGE puck.
#pragma once

#include "HsWidgets.h"

namespace hsui
{
/** Every output sample pair is one dot: M/S folded to M >= 0, stage angle = 2*atan2(S, M) (mono front, hard left left,
    hard right right, anti-phase behind); radius = amplitude (square root, against a slow peak). Colour mixed by angle
    (front ink, sides ink-soft, behind gold); dots fade with the signal's decay (the trail image loses alpha each tick).
    The puck: grab it anywhere in the stage and drag; double-click returns it to the front. */
class StageScope : public juce::Component, public HasSummary
{
public:
    explicit StageScope (HighStereoAudioProcessor& p) : proc (p), param (*p.apvts.getParameter ("stage"))
    {
        attach = std::make_unique<juce::ParameterAttachment> (param, [this] (float v) { stage = v; repaint(); }, nullptr);
        attach->sendInitialUpdate();
        shownStage = stage;
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    }

    juce::String summary() const override
    {
        return "STAGE " + signedDeg (stage) + " - drag the dot: the centre of the image moves round you. "
               "The bass stays centred. Off the front, mono changes.";
    }

    /** Called by the editor at 30 Hz: pull the newest output samples, fade the trail, plot the new dots, glide the puck. */
    void tick()
    {
        // glide the puck to values the product sets (preset load); a held puck does not glide
        if (held) shownStage = stage;
        else { const float d = stage - shownStage; shownStage = std::abs (d) <= std::max (std::abs (d) * 0.3f, 0.1f) ? stage : shownStage + d * 0.3f; }

        const int w = proc.ringPost.writePos();
        int count = (w - lastPos + HighStereoAudioProcessor::SampleRing::N) & (HighStereoAudioProcessor::SampleRing::N - 1);
        lastPos = w;
        count = juce::jlimit (0, 4096, count);
        const int sc = 2;   // trail image at 2x for sharp dots on retina
        const auto area = scopeArea();
        const int iw = juce::roundToInt (area.getWidth() * sc), ih = juce::roundToInt (area.getHeight() * sc);
        if (trail.getWidth() != iw || trail.getHeight() != ih) trail = juce::Image (juce::Image::ARGB, iw, ih, true);
        trail.multiplyAllAlphas (0.68f);   // decay: a dot fades over ~270 ms
        if (count > 0)
        {
            buf.resize ((size_t) count * 2);
            proc.ringPost.latest (buf.data(), buf.data() + count, count);
            float peak = 0.0f;
            for (int i = 0; i < count; ++i) peak = std::max (peak, std::hypot (0.5f * (buf[(size_t) i] + buf[(size_t) (count + i)]), 0.5f * (buf[(size_t) i] - buf[(size_t) (count + i)])));
            ampRef = std::max (0.02f, std::max (peak, ampRef * 0.995f));
            juce::Image::BitmapData bd (trail, juce::Image::BitmapData::readWrite);
            const float cx = (float) iw * 0.5f, cy = (float) ih * 0.5f, R = (float) iw * 0.5f - 3.0f * sc;
            for (int i = 0; i < count; ++i)
            {
                float m = 0.5f * (buf[(size_t) i] + buf[(size_t) (count + i)]), s = 0.5f * (buf[(size_t) i] - buf[(size_t) (count + i)]);
                if (m < 0) { m = -m; s = -s; }
                const float th = 2.0f * std::atan2 (s, m);
                const float amp = std::min (1.0f, std::sqrt (std::hypot (m, s) / ampRef));
                if (amp < 0.01f) continue;
                const float x = cx - std::sin (th) * amp * R, y = cy - std::cos (th) * amp * R;
                const float t = std::abs (th) / juce::MathConstants<float>::pi;            // 0 front .. 1 behind
                const auto hue = t < 0.5f ? mix (ink, inkSoft, t / 0.5f) : mix (inkSoft, gold, (t - 0.5f) / 0.5f);
                const auto col = hue.withAlpha (juce::jlimit (0.0f, 1.0f, 0.4f + amp));
                for (int dy = 0; dy < 3; ++dy)
                    for (int dx = 0; dx < 3; ++dx)
                    {
                        const int px = (int) x + dx - 1, py = (int) y + dy - 1;
                        if (px >= 0 && py >= 0 && px < iw && py < ih) bd.setPixelColour (px, py, col);
                    }
            }
        }
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto area = scopeArea();
        const auto c = area.getCentre(); const float R = area.getWidth() * 0.5f;
        g.setColour (greyLo);
        g.drawEllipse (area, 1.0f);
        for (int a = 0; a < 360; a += 45)
            g.drawLine (c.x, c.y, c.x + std::cos (juce::degreesToRadians ((float) a)) * R, c.y - std::sin (juce::degreesToRadians ((float) a)) * R, 1.0f);
        g.setColour (inkSoft);
        g.drawLine (c.x - R, c.y, c.x + R, c.y, 1.0f);   // the ear line: front above, behind below
        g.setFont (bold (11.0f));
        g.drawText ("L", juce::Rectangle<float> (area.getX() - 16, c.y - 8, 12, 16), juce::Justification::centredRight);
        g.drawText ("R", juce::Rectangle<float> (area.getRight() + 4, c.y - 8, 12, 16), juce::Justification::centredLeft);
        g.setFont (bold (8.5f));
        g.drawText ("FRONT", juce::Rectangle<float> (c.x - 30, area.getY() - 14, 60, 12), juce::Justification::centred);
        g.drawText ("BEHIND", juce::Rectangle<float> (c.x - 30, area.getBottom() + 2, 60, 12), juce::Justification::centred);
        g.drawImage (trail, area, juce::RectanglePlacement::stretchToFit);

        // readouts
        g.setColour (inkSoft); g.setFont (bold (9.0f));
        g.drawText ("CORRELATION", 12, 8, 120, 12, juce::Justification::centredLeft);
        g.drawText ("STAGE", getWidth() - 132, 8, 120, 12, juce::Justification::centredRight);
        g.setColour (ink); g.setFont (mono (20.0f));
        g.drawText (signed2 (proc.getCorrelation()), 12, 20, 120, 22, juce::Justification::centredLeft);
        g.drawText (signedDeg (shownStage), getWidth() - 132, 20, 120, 22, juce::Justification::centredRight);

        // the puck
        const float st = juce::degreesToRadians (shownStage), pr = R * 0.86f;
        const float px = c.x - std::sin (st) * pr, py = c.y - std::cos (st) * pr;
        g.setColour (mix (greyHi, inkSoft, 0.5f));
        g.drawLine (c.x, c.y, px, py, 1.0f);
        g.setColour (cream); g.fillEllipse (px - 9, py - 9, 18, 18);
        g.setColour (ink); g.drawEllipse (px - 9, py - 9, 18, 18, held || isMouseOver() ? 3.0f : 2.0f);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! inStage (e.position)) return;
        held = true; attach->beginGesture(); setFromPoint (e.position, true);
    }
    void mouseDrag (const juce::MouseEvent& e) override { if (held) setFromPoint (e.position, false); }
    void mouseUp (const juce::MouseEvent&) override { if (held) { attach->endGesture(); held = false; } }
    void mouseDoubleClick (const juce::MouseEvent& e) override { if (inStage (e.position)) attach->setValueAsCompleteGesture (0.0f); }

    /** Snapshot/tests: plot without a message loop. */
    void tickForSnapshot() { tick(); }

private:
    juce::Rectangle<float> scopeArea() const
    {
        const float d = std::min ((float) getWidth() - 40.0f, (float) getHeight() - 80.0f);
        return juce::Rectangle<float> (d, d).withCentre ({ getWidth() * 0.5f, 52.0f + d * 0.5f + 6.0f });
    }
    bool inStage (juce::Point<float> p) const { const auto a = scopeArea(); return p.getDistanceFrom (a.getCentre()) <= a.getWidth() * 0.5f + 12.0f; }
    void setFromPoint (juce::Point<float> p, bool first)
    {
        const auto c = scopeArea().getCentre();
        float deg = juce::radiansToDegrees (std::atan2 (-(p.x - c.x), -(p.y - c.y)));   // 0 front, + left
        // the puck goes round the front: crossing the bottom (+180 <-> -180) would be a jump in the sound
        if (! first && std::abs (deg - stage) > 180.0f) deg = stage > 0 ? 180.0f : -180.0f;
        attach->setValueAsPartOfGesture (juce::jlimit (-180.0f, 180.0f, deg));
        shownStage = stage;
    }
    static juce::String signed2 (float v) { return (v >= 0 ? "+" : juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92"))) + juce::String (std::abs (v), 2); }
    static juce::String signedDeg (float v)
    {
        const int d = juce::roundToInt (v);
        return (d > 0 ? "+" : d < 0 ? juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92")) : juce::String()) + juce::String (std::abs (d)) + juce::String (juce::CharPointer_UTF8 ("\xc2\xb0"));
    }

    HighStereoAudioProcessor& proc;
    juce::RangedAudioParameter& param;
    std::unique_ptr<juce::ParameterAttachment> attach;
    juce::Image trail;
    std::vector<float> buf;
    float stage = 0, shownStage = 0, ampRef = 0.1f;
    int lastPos = 0;
    bool held = false;
};
} // namespace hsui
