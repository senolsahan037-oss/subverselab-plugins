// HIGH STEREO right screen: WIDTH field (SPEC.md section 7, option B).
#pragma once

#include "HsWidgets.h"

namespace hsui
{
/** One shape whose edges shimmer like a vertical waveform: the live fill of 40 fine bands from an FFT of the signal BEFORE
    the STAGE (the stage never moves this screen). Fill = 2*Ps/(Pm+Ps); edge = half * fill/200 %. Light under the shape.
    Per band row: grips = the band setting (drag; magnet to the LIMIT notch within 4 %; double-click = the loaded preset's
    value), gold LIMIT notches, the held peak fill in the middle, the loss in the LISTEN mode on the right.
    A click on the field away from a grip forgets the learned peaks. */
class WidthField : public juce::Component, public HasSummary
{
public:
    static constexpr int kBands = HighStereoAudioProcessor::kBands, kFine = 40, kOrder = 12, kN = 1 << kOrder;

    WidthField (HighStereoAudioProcessor& p, std::function<float (int)> loadedBand) : proc (p), loaded (std::move (loadedBand)), fft (kOrder)
    {
        for (int k = 0; k < kBands; ++k)
        {
            auto& prm = *proc.apvts.getParameter (HighStereoAudioProcessor::bandId (k));
            params[(size_t) k] = &prm;
            attach[(size_t) k] = std::make_unique<juce::ParameterAttachment> (prm, [this, k] (float v) { setting[(size_t) k] = v; repaint(); }, nullptr);
            attach[(size_t) k]->sendInitialUpdate();
            shown[(size_t) k] = setting[(size_t) k];
        }
        win.resize (kN); for (int i = 0; i < kN; ++i) win[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (kN - 1));
        bufL.resize (kN); bufR.resize (kN); fm.resize (2 * kN); fs.resize (2 * kN);
        const double lo = std::log (40.0), hi = std::log (16000.0);
        for (int j = 0; j <= kFine; ++j) edges[(size_t) j] = std::exp (lo + (hi - lo) * j / kFine);
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
    }

    juce::String summary() const override
    {
        const int row = hoverRow;
        if (row < 0) return "WIDTH field - how wide each band is now. Drag a band's dots to set it; the gold marks are its limit. Click to relearn.";
        const int k = 7 - row;
        const auto& v = proc.bandView (k);
        static const char* names[kBands] { "<100 Hz", "100-250 Hz", "250-500 Hz", "500 Hz-1k", "1k-2k", "2k-4k", "4k-8k", "8k+" };
        juce::String s = juce::String (names[k]) + "  " + juce::String (juce::roundToInt (setting[(size_t) k])) + " %";
        if (! v.learned.load()) return s + " - play some music: the limit is learned from it.";
        if (v.noSide.load()) return s + " - this band has almost no side to widen. Use SPREAD or go back to the mix.";
        const float lim = v.limit.load() * 100.0f;
        s += "  limit " + juce::String (juce::roundToInt (lim)) + " %  peak " + juce::String (juce::roundToInt (v.peakFill.load() * 100.0f)) + " %";
        if (lim < setting[(size_t) k] - 1.0f) return s + " - too wide at its loudest: narrow it to the gold mark.";
        if (lim >= 199.0f) return s + " - room to widen all the way.";
        return s + " - room to widen up to the gold mark; past it, more width has to come from the mix.";
    }

    /** 30 Hz: FFT of the newest pre-STAGE samples, smoothed powers per fine band, glide the grips. */
    void tick()
    {
        proc.ringPre.latest (bufL.data(), bufR.data(), kN);
        for (int i = 0; i < kN; ++i)
        {
            fm[(size_t) i] = win[(size_t) i] * 0.5f * (bufL[(size_t) i] + bufR[(size_t) i]);
            fs[(size_t) i] = win[(size_t) i] * 0.5f * (bufL[(size_t) i] - bufR[(size_t) i]);
        }
        std::fill (fm.begin() + kN, fm.end(), 0.0f); std::fill (fs.begin() + kN, fs.end(), 0.0f);
        fft.performFrequencyOnlyForwardTransform (fm.data(), true);
        fft.performFrequencyOnlyForwardTransform (fs.data(), true);
        const double rate = proc.getRate();
        std::array<double, kFine> pm {}, ps {};
        for (int j = 0; j < kFine; ++j)
        {
            const int b0 = std::max (1, (int) std::floor (edges[(size_t) j] * kN / rate)), b1 = std::max (b0 + 1, (int) std::ceil (edges[(size_t) j + 1] * kN / rate));
            for (int b = b0; b < std::min (b1, kN / 2); ++b) { pm[(size_t) j] += (double) fm[(size_t) b] * fm[(size_t) b]; ps[(size_t) j] += (double) fs[(size_t) b] * fs[(size_t) b]; }
        }
        // smooth over neighbours, then over time (powers, not ratios: few-bin low bands would spike)
        std::array<double, kFine> qm {}, qs {};
        for (int j = 0; j < kFine; ++j)
        {
            const int a = std::max (0, j - 1), c = std::min (kFine - 1, j + 1);
            qm[(size_t) j] = 0.25 * pm[(size_t) a] + 0.5 * pm[(size_t) j] + 0.25 * pm[(size_t) c];
            qs[(size_t) j] = 0.25 * ps[(size_t) a] + 0.5 * ps[(size_t) j] + 0.25 * ps[(size_t) c];
            spm[(size_t) j] += (qm[(size_t) j] - spm[(size_t) j]) * 0.45; sps[(size_t) j] += (qs[(size_t) j] - sps[(size_t) j]) * 0.45;
            const double tot = spm[(size_t) j] + sps[(size_t) j];
            fill[(size_t) j] = tot > 1e-12 ? (float) (2.0 * sps[(size_t) j] / tot) : 0.0f;
            levelDb[(size_t) j] = (float) (10.0 * std::log10 (std::max (1e-12, tot)));
        }
        refDb = std::max (refDb - 0.05f, *std::max_element (levelDb.begin(), levelDb.end()));
        for (int k = 0; k < kBands; ++k)
        {
            if (dragBand == k) { shown[(size_t) k] = setting[(size_t) k]; continue; }
            const float d = setting[(size_t) k] - shown[(size_t) k];
            shown[(size_t) k] = std::abs (d) <= std::max (std::abs (d) * 0.3f, 1.0f) ? setting[(size_t) k] : shown[(size_t) k] + d * 0.3f;
        }
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto L = layout();
        g.setColour (inkSoft); g.setFont (bold (11.0f));
        g.drawText ("L", juce::Rectangle<float> (L.x0, 10, 20, 14), juce::Justification::centredLeft);
        g.drawText ("R", juce::Rectangle<float> (L.x1 - 20, 10, 20, 14), juce::Justification::centredRight);
        const int mode = (int) proc.getListen();
        g.setFont (bold (mode == 4 ? 8.0f : 10.0f));
        static const char* hdr[5] { "MONO", "MONO", "PHONE", "SPEAKERS", "HEADPH." };
        g.drawText (hdr[juce::jlimit (0, 4, mode)], juce::Rectangle<float> (L.x1 + 18, 10, 70, 14), juce::Justification::centredLeft);

        // rows: guides, then the light-filled shape, then grips / notches / numbers / labels on top
        for (int row = 0; row < kBands; ++row)
        {
            const float yc = L.y0 + row * L.rh + L.rh * 0.5f;
            g.setColour (mix (greyHi, greyLo, 0.5f)); g.drawLine (L.x0, yc, L.x1, yc, 1.0f);
        }
        g.setColour (greyLo); g.drawLine (L.fc, L.y0 - 4, L.fc, L.y0 + 8 * L.rh, 1.0f);
        drawShape (g, L);

        for (int row = 0; row < kBands; ++row)
        {
            const int k = 7 - row; const auto& v = proc.bandView (k);
            const float yc = L.y0 + row * L.rh + L.rh * 0.5f;
            g.setColour (ink); g.setFont (bold (12.0f));
            static const char* lab[kBands] { "8k+", "4k", "2k", "1k", "500", "250", "100", "<100" };
            g.drawText (lab[row], juce::Rectangle<float> (L.x0 - 54, yc - 8, 40, 16), juce::Justification::centredRight);   // fixed
            const bool learned = v.learned.load(), noSide = v.noSide.load();
            const float lim = v.limit.load();
            if (learned && lim < 1.999f)
                for (float gx : { L.fc - L.half * lim * 0.5f, L.fc + L.half * lim * 0.5f })
                { g.setColour (gold); g.fillRoundedRectangle (gx - 2, yc - 11, 4, 22, 2); }
            const float sh = L.half * juce::jlimit (0.0f, 1.0f, shown[(size_t) k] / 200.0f);
            const auto gcol = learned && noSide ? mix (greyHi, ink, 0.35f) : ink;
            for (float gx : { L.fc - sh, L.fc + sh })
            {
                g.setColour (cream); g.fillEllipse (gx - 7, yc - 7, 14, 14);
                g.setColour (gcol); g.drawEllipse (gx - 7, yc - 7, 14, 14, (dragBand == k || hoverRow == row) ? 3.0f : 2.0f);
            }
            const float pk = learned ? v.peakFill.load() : 0.0f;
            const auto num = learned ? juce::String (juce::roundToInt (std::min (pk, 2.0f) * 100.0f)) : juce::String ("--");
            g.setFont (mono (12.5f));
            const float bw = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), num) + 8.0f;
            g.setColour (greyHi); g.fillRoundedRectangle (L.fc - bw * 0.5f, yc - 8, bw, 16, 4);
            g.setColour (pk > 1.0f ? gold : ink);
            g.drawText (num, juce::Rectangle<float> (L.fc - bw * 0.5f, yc - 8, bw, 16), juce::Justification::centred);
            const float loss = v.liveLossDb.load(), lw = v.liveWidth.load();
            g.setColour (lw > 1.0f ? gold : inkSoft); g.setFont (mono (13.0f));
            const auto lossTxt = (loss > 0.049f ? "+" : loss < -0.049f ? juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92")) : juce::String (" "))
                                 + juce::String (std::abs (loss), 1);
            g.drawText (lossTxt, juce::Rectangle<float> (L.x1 + 18, yc - 8, 64, 16), juce::Justification::centredLeft);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override { hoverRow = rowAt (e.position.y); repaint(); }
    void setHoverRowForSnapshot (int r) { hoverRow = r; }
    void mouseExit (const juce::MouseEvent&) override { hoverRow = -1; repaint(); }
    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto L = layout(); const int row = rowAt (e.position.y);
        dragBand = -1; downOnGrip = false;
        if (row < 0) return;
        const int k = 7 - row;
        const float sh = L.half * setting[(size_t) k] / 200.0f;
        if (std::abs (std::abs (e.position.x - L.fc) - sh) <= 10.0f)
        { dragBand = k; downOnGrip = true; attach[(size_t) k]->beginGesture(); }
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragBand < 0) return;
        const auto L = layout();
        float v = juce::jlimit (0.0f, 200.0f, std::abs (e.position.x - L.fc) / L.half * 200.0f);
        const auto& bv = proc.bandView (dragBand);
        if (bv.learned.load() && bv.limit.load() < 1.999f && std::abs (v - bv.limit.load() * 100.0f) <= 4.0f) v = bv.limit.load() * 100.0f;   // magnet
        attach[(size_t) dragBand]->setValueAsPartOfGesture (std::round (v));
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (dragBand >= 0) { attach[(size_t) dragBand]->endGesture(); dragBand = -1; return; }
        if (e.mouseWasClicked() && ! downOnGrip) proc.requestPeakReset();
    }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int row = rowAt (e.position.y);
        if (row >= 0) { const int k = 7 - row; attach[(size_t) k]->setValueAsCompleteGesture (loaded (k)); }
    }

private:
    struct Lay { float x0, x1, fc, half, y0, rh; };
    Lay layout() const
    {
        const float x0 = 62.0f, x1 = (float) getWidth() - 80.0f;
        return { x0, x1, (x0 + x1) * 0.5f, (x1 - x0) * 0.5f, 32.0f, ((float) getHeight() - 40.0f) / 8.0f };
    }
    int rowAt (float y) const { const auto L = layout(); const int r = (int) std::floor ((y - L.y0) / L.rh); return r >= 0 && r < kBands ? r : -1; }
    float yFor (double f) const
    {
        static const double e8[9] { 20, 100, 250, 500, 1000, 2000, 4000, 8000, 20000 };
        int k = 0; while (k < 7 && f >= e8[k + 1]) ++k;
        const double frac = (std::log (f) - std::log (e8[k])) / (std::log (e8[k + 1]) - std::log (e8[k]));
        const auto L = layout();
        return L.y0 + (float) (7 - k) * L.rh + L.rh * (float) (1.0 - frac);
    }
    void drawShape (juce::Graphics& g, const Lay& L)
    {
        std::array<float, kFine> yf {};
        for (int j = 0; j < kFine; ++j) yf[(size_t) j] = yFor (std::sqrt (edges[(size_t) j] * edges[(size_t) j + 1]));
        // yf decreases with j (higher frequency higher up): walk the rows top -> bottom
        auto at = [&] (float y, const std::array<float, kFine>& arr)
        {
            if (y <= yf[kFine - 1]) return arr[kFine - 1];
            if (y >= yf[0]) return arr[0];
            for (int j = kFine - 1; j > 0; --j)
                if (y >= yf[(size_t) j] && y <= yf[(size_t) j - 1])
                { const float t = (y - yf[(size_t) j]) / std::max (1e-3f, yf[(size_t) j - 1] - yf[(size_t) j]); return arr[(size_t) j] + (arr[(size_t) j - 1] - arr[(size_t) j]) * t; }
            return arr[0];
        };
        juce::Path left, right;
        const float top = L.y0 + 2, bottom = L.y0 + 8 * L.rh - 2;
        bool first = true;
        for (float y = top; y <= bottom; y += 1.5f)
        {
            const float w = at (y, fill), lv = at (y, levelDb);
            const float h = L.half * juce::jlimit (0.012f, 1.0f, w * 0.5f);
            const float strength = juce::jlimit (0.0f, 1.0f, (lv - (refDb - 45.0f)) / 40.0f);
            const auto c = lightFor (w);
            g.setColour (c.withAlpha (0.18f * (0.4f + strength)));   // soft halo
            g.fillRect (L.fc - h - 4, y, 2 * h + 8, 1.6f);
            g.setColour (c.withAlpha (0.35f + 0.6f * strength));
            g.fillRect (L.fc - h, y, 2 * h, 1.6f);
            if (first) { left.startNewSubPath (L.fc - h, y); right.startNewSubPath (L.fc + h, y); first = false; }
            else { left.lineTo (L.fc - h, y); right.lineTo (L.fc + h, y); }
        }
        g.setColour (mix (greyHi, inkSoft, 0.6f));
        g.strokePath (left, juce::PathStrokeType (1.0f)); g.strokePath (right, juce::PathStrokeType (1.0f));
    }

    HighStereoAudioProcessor& proc;
    std::function<float (int)> loaded;
    juce::dsp::FFT fft;
    std::array<juce::RangedAudioParameter*, kBands> params {};
    std::array<std::unique_ptr<juce::ParameterAttachment>, kBands> attach;
    std::array<float, kBands> setting {}, shown {};
    std::vector<float> win, bufL, bufR, fm, fs;
    std::array<double, kFine + 1> edges {};
    std::array<double, kFine> spm {}, sps {};
    std::array<float, kFine> fill {}, levelDb {};
    float refDb = -60.0f;
    int hoverRow = -1, dragBand = -1;
    bool downOnGrip = false;
};
} // namespace hsui
