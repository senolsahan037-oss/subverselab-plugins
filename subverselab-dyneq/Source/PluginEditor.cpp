#include "PluginEditor.h"
#include "ParamText.h"
#include "PanelPanels.h"

using fam::rgb;
using fam::silk;

namespace
{
constexpr double kLogSpan = 6.907755278982137;     // ln (1000): 20 Hz .. 20 kHz

// the analyzer's vertical scale: dBFS of a bin
constexpr float kAnaLo = -108.0f, kAnaHi = 16.0f;   // LAB tilts the trace +4.5 dB/oct, so music sits in the lower two thirds and never covers the EQ curve

float anaNorm (float db) { return juce::jlimit (0.0f, 1.0f, (db - kAnaLo) / (kAnaHi - kAnaLo)); }
}

namespace panelui
{
//==============================================================================
// the node read-out ("+x band . -y COLLIDE (KEY) = net z") of the display
struct NodeInfo
{
    static bool carved (DyneqEditor& ed, int i)   // a boost that COLLIDE / SELF / TARGET are carving
    {
        const auto& cv = ed.curves(); const auto& l = ed.ctl().proc.getBandLayers (i);
        const float other = l.conflict.load() + l.self.load() + l.target.load();
        return cv.band[i].on && cv.band[i].gainShown > 0.05f && other < -0.1f;
    }
    static juce::String readout (DyneqEditor& ed, int i)
    {
        const auto& l = ed.ctl().proc.getBandLayers (i); const auto& cv = ed.curves();
        juce::String s = panel::sgn (cv.band[i].gainShown) + " band";
        auto add = [&] (float v, const char* nm) { if (std::abs (v) >= 0.05f) s << " / " << panel::sgn (v) << " " << nm; };
        add (l.conflict.load(), ed.ctl().proc.isSidechainConnected() ? "COLLIDE (KEY)" : "COLLIDE"); add (l.self.load(), "SELF"); add (l.target.load(), "TARGET");
        s << " = net " << panel::sgn (l.net.load());
        return s;
    }
    /** The selected band's live line on the display: the conflict line when a layer acts on its frequency, a dynamic band's gain of now, else nothing. */
    static juce::String live (DyneqEditor& ed, int b)
    {
        const auto& l = ed.ctl().proc.getBandLayers (b);
        if (std::abs (l.conflict.load()) + std::abs (l.self.load()) + std::abs (l.target.load()) >= 0.05f && ed.ctl().bandOn (b)) return "BAND " + juce::String (b + 1) + ":  " + readout (ed, b);
        const auto& cb = ed.curves().band[b];
        if (cb.on && cb.dyn && panel::Ctl::typeHasDyn (cb.type)) return "BAND " + juce::String (b + 1) + ":  NOW " + panel::sgn (cb.gainShown) + " dB";
        return {};
    }
    static juce::String line1 (DyneqEditor& ed, int i)   // BAND 3  BELL  1.20 kHz  +3.0 dB  Q 1.00
    {
        auto& c = ed.ctl(); const auto& b = ed.curves().band[i];
        juce::String s = "BAND " + juce::String (i + 1) + "  " + c.bandTypeName (i) + "  " + dyneq::ptext::hz (b.freq);
        if (panel::Ctl::typeHasGain (b.type)) s << "  " << panel::sgn (b.gainShown) << " dB";
        s << "  Q " << dyneq::ptext::fixed (b.q, 2);
        return s;
    }
};

//==============================================================================
/** The node drag of the display: a drag moves the node (Shift = fine, x0.2; Alt = one axis); a click without a drag selects, an Alt-click toggles the band. */
struct NodeGesture
{
    explicit NodeGesture (DyneqEditor& e) : ed (e) {}
    bool active() const { return band >= 0; }
    void begin (int b, juce::Point<float> nodeXY, juce::Point<float> pointer, const juce::ModifierKeys& m)
    {
        auto& c = ed.ctl();
        band = b; cur = start = nodeXY; last = down = pointer; moved = false; axis = 0; altAtDown = m.isAltDown();
        c.selectBandPanel (b);
        gestures.clear();
        for (const char* n : { "freq", "gain", "q", "on" }) { auto* p = c.par (panel::bandId (b, n)); p->beginChangeGesture(); gestures.push_back (p); }
        q0 = c.raw (panel::bandId (b, "q"));
    }
    /** The node's position after this pointer event; `moved` stays false inside a 3 px dead zone. */
    juce::Point<float> move (juce::Point<float> pointer, const juce::ModifierKeys& m)
    {
        if (! moved)
        {
            if (pointer.getDistanceFrom (down) < 3.0f) return cur;
            moved = true; last = down;
        }
        auto d = pointer - last; last = pointer;
        if (m.isAltDown() && axis == 0) axis = std::abs (pointer.x - down.x) >= std::abs (pointer.y - down.y) ? 1 : 2;
        if (! m.isAltDown()) axis = 0;
        if (m.isShiftDown()) d = d * 0.2f;
        if (axis == 1) d.y = 0.0f;
        if (axis == 2) d.x = 0.0f;
        cur += d;
        if (axis == 1) cur.y = start.y;
        if (axis == 2) cur.x = start.x;
        return cur;
    }
    /** Ends the gestures. True when this was an Alt-click without a drag (the caller toggles the band). */
    bool end()
    {
        for (auto* p : gestures) p->endChangeGesture();
        gestures.clear();
        const bool altClick = band >= 0 && ! moved && altAtDown;
        band = -1;
        return altClick;
    }
    DyneqEditor& ed; int band = -1, axis = 0; bool moved = false, altAtDown = false; float q0 = 1.0f;
    juce::Point<float> cur, start, last, down;
    std::vector<juce::RangedAudioParameter*> gestures;
};

} // namespace panelui

#include "LabPanels.h"

namespace panelui
{
//==============================================================================
/** The LAB display (zone C of the UX redesign, 11 Oct): the log frequency grid with the EQ's dB scale on the left and the analyzer's on the right,
    the analyzer, the layers' fills, the total curve and the band nodes. Pro-Q's node gestures; every band has a node (an OFF band a hollow one). */
class LabScreen : public juce::Component, public PanelControl, private PanelCounted
{
public:
    explicit LabScreen (DyneqEditor& e) : ed (e), gesture (e)
    {
        mark = juce::ImageCache::getFromMemory (BinaryData::mark_cream_png, BinaryData::mark_cream_pngSize);
    }
    /** UI signature 3: the SubverseLab mark at the bottom right of the display, on the frequency scale; a click opens subverselab.com.
        The icon only, no wordmark (owner 7 Nov 2026); the cream icon is the brand kit's (Assets/Brand/logo/mark-only-cream.png from Assets/Brand/_kit). */
    static constexpr float kLogoMarkH = 14.0f;
    juce::Rectangle<float> lockupArea() const
    {
        if (mark.isNull()) return {};
        const float mw = kLogoMarkH * (float) mark.getWidth() / (float) mark.getHeight();
        const float right = (float) W - 8.0f, cy = (float) H - 12.0f;
        return { right - mw, cy - kLogoMarkH * 0.5f, mw, kLogoMarkH };
    }
    static constexpr const char* kLockupUrl = "https://subverselab.com";
    std::function<void()> openLockupLink_forTest;   // tests: set to catch the click instead of opening a browser
    void openLockupLink() { if (openLockupLink_forTest) { openLockupLink_forTest(); return; } juce::URL (kLockupUrl).launchInDefaultBrowser(); }
    static constexpr int W = lab::kPlotW, H = lab::kPlotH, PT = 12, PB = H - 24;   // the curves live between PT and PB; the frequency scale is the bottom 24 px
    float fx (double f) const { return (float) (std::log (f / 20.0) / kLogSpan * W); }
    double xf (float x) const { return 20.0 * std::exp ((double) x / W * kLogSpan); }
    float dy (double d) const { return (float) (PT + (18.0 - d) / 36.0 * (PB - PT)); }
    double yd (float y) const { return 18.0 - (double) (y - PT) / (PB - PT) * 36.0; }
    float ay (float db) const { return (float) PB - anaNorm (db) * (float) (PB - PT); }
    /** The nodes' places, from the curves of the last frame (the editor's timer calls it, so hit tests do not depend on a paint).
        An ON band sits on the total curve; an OFF band that holds settings, or the selected band, is a hollow node at its frequency on 0 dB. */
    void rebuildNodes()
    {
        nodes.clear();
        auto& cv = ed.curves(); auto& c = ed.ctl();
        if (c.bypass()) return;
        for (int i = 0; i < panel::kBands; ++i)
        {
            if (cv.band[i].on) nodes.push_back ({ i, fx (cv.band[i].freq), dy (juce::jlimit (-18.0, 18.0, cv.totalAt (cv.band[i].freq))), true });
            else if (i == c.selectedBand || ! c.isEmptySlot (i)) nodes.push_back ({ i, fx (cv.band[i].freq), dy (0.0), false });
        }
    }

    void paint (juce::Graphics& g) override
    {
        auto& c = ed.ctl(); auto& cv = ed.curves(); auto& sp = ed.spectrum();
        const bool byp = c.bypass(), delta = c.delta();
        const int am = c.analyzerMode();
        juce::Path clip; clip.addRoundedRectangle (getLocalBounds().toFloat(), 6.0f);
        bgLayer.draw (g, W, H, [&] (juce::Graphics& s)   // everything that never changes (plot, grid, scales) is drawn once into an image: 40 repaints a second only move the traces
        {
            s.saveState(); s.reduceClipRegion (clip);
            // the display is the same glass as the rest of the screen (owner 26 Oct: everything embedded in ONE screen): no box of its own, only the grid
            const juce::Font small = Theme::condEm (11.0f);
            for (double f : { 30.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
            {
                s.setColour (dig::grid().withAlpha (0.8f)); s.drawVerticalLine ((int) fx (f), (float) PT, (float) PB);
                if (f > 30.0) silk (s, panel::fmtHzShort ((float) f), small, fx (f), (float) H - 12.0f, 'm', 0.6f, dig::dim());
            }
            for (int d : { -18, -12, -6, 0, 6, 12, 18 })
            {
                s.setColour (d ? dig::grid().withAlpha (0.8f) : rgb (40, 96, 88)); s.drawHorizontalLine ((int) dy (d), 30.0f, (float) W - 34.0f);
                silk (s, (d > 0 ? "+" : "") + juce::String (d), small, 8.0f, juce::jlimit ((float) PT + 6.0f, (float) PB - 6.0f, dy (d)), 'l', 0.0f, d ? dig::dim() : dig::text());   // the EQ scale, cream
            }
            for (int a : { 0, -24, -48, -72, -96 })   // the analyzer scale, grey, on the other side: never confused with the EQ's
                silk (s, juce::String (a), Theme::condEm (9.5f), (float) W - 6.0f, juce::jlimit ((float) PT + 6.0f, (float) PB - 6.0f, ay ((float) a)), 'r', 0.0f, rgb (90, 110, 106));
            // UI signature 3: the SubverseLab mark (icon only), bottom right, on the frequency scale (a click opens subverselab.com)
            if (! mark.isNull()) { s.setOpacity (1.0f); s.drawImage (mark, lockupArea(), juce::RectanglePlacement::stretchToFit); }
            s.restoreState();
        });
        g.saveState(); g.reduceClipRegion (clip);
        auto curve = [&] (const float* db, bool closeToZero)   // a filled curve uses every second point (180 points over 968 px: not visible, half the rasteriser's work)
        {
            juce::Path p; const int stride = closeToZero ? 2 : 1;
            for (int i = 0; i < panel::kCurveN; i += stride)
            {
                const float x = fx (cv.f[i]), y = dy (juce::jlimit (-18.0f, 18.0f, db[i]));
                if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
            }
            if (closeToZero) { p.lineTo ((float) W, dy (0)); p.lineTo (0.0f, dy (0)); p.closeSubPath(); }
            return p;
        };
        auto trace = [&] (const panel::Spectrum::Trace& t, bool filled)   // the analyzer as Pro-Q shows it: tilted +4.5 dB/oct about 1 kHz (music reads flat), drawn as a smooth curve
        {
            juce::Path p; float px = 0.0f, py = 0.0f;
            for (int i = 0; i < panel::kCurveN; ++i)
            {
                const float tilt = t.db[i] <= panel::Spectrum::kFloorDb + 1.0f ? 0.0f : 4.5f * std::log2 (cv.f[i] / 1000.0f);   // silence stays on the floor
                const float x = fx (cv.f[i]), y = juce::jlimit ((float) PT + 4.0f, (float) PB, ay (t.db[i] + tilt));
                if (i == 0) p.startNewSubPath (x, y);
                else p.quadraticTo (px, py, (px + x) * 0.5f, (py + y) * 0.5f);
                px = x; py = y;
            }
            p.lineTo (px, py);
            if (filled) { p.lineTo ((float) W, (float) PB); p.lineTo (0.0f, (float) PB); p.closeSubPath(); }
            return p;
        };
        // the analyzer: PRE filled and dim, the sidechain KEY a teal line, POST (the default, one trace) a light line over a soft fill
        if (am & 1)
        {
            g.setColour (rgb (120, 150, 148).withAlpha (0.14f)); g.fillPath (trace (sp.pre, true));
            g.setColour (rgb (150, 178, 174).withAlpha (0.40f)); g.strokePath (trace (sp.pre, false), juce::PathStrokeType (1.0f));
        }
        if (ed.keyTraceOn() && c.layerOn (panel::kCollide) && c.proc.isSidechainConnected())   // the key: a thin teal line, no fill (POST is the one filled trace)
        {
            g.setColour (Theme::teal.brighter (0.3f).withAlpha (0.6f)); g.strokePath (trace (sp.key, false), juce::PathStrokeType (1.0f));
        }
        const float k = delta ? 1.8f : 1.0f;
        if (! byp)
        {
            // the layers' cuts: a fill that is strongest at the 0 dB line and fades into the cut, and a fine edge along the cut (26 Oct, next level)
            auto layer = [&] (const float* db, juce::Colour col, float alpha)
            {
                const auto p = curve (db, true);
                g.setGradientFill (juce::ColourGradient (col.withAlpha (juce::jmin (1.0f, alpha * k)), 0.0f, dy (0), col.withAlpha (juce::jmin (1.0f, alpha * 0.35f * k)), 0.0f, dy (-12), false));
                g.fillPath (p);
                juce::Path edge; bool pen = false;   // the edge only where the layer cuts (not along the whole 0 dB line)
                for (int i = 0; i < panel::kCurveN; ++i)
                {
                    const bool acts = db[i] < -0.05f;
                    const float x = fx (cv.f[i]), y = dy (juce::jlimit (-18.0f, 18.0f, db[i]));
                    if (acts) { if (pen) edge.lineTo (x, y); else edge.startNewSubPath (x, y); }
                    pen = acts;
                }
                g.setColour (col.withAlpha (juce::jmin (1.0f, alpha * 1.8f * k))); g.strokePath (edge, juce::PathStrokeType (1.0f));
            };
            layer (cv.target, Theme::gold, 0.30f);
            layer (cv.collide, Theme::teal.brighter (0.2f), 0.30f);
            layer (cv.self, Theme::labelDeep, 0.22f);
            const int hb = ed.hoverChip();   // the chip under the pointer lights its band's own curve
            if (hb >= 0 && hb != c.selectedBand && cv.band[hb].on)
            {
                float own[panel::kCurveN]; for (int i = 0; i < panel::kCurveN; ++i) own[i] = (float) cv.bandOneDb (hb, cv.f[i]);
                g.setColour (dig::text().withAlpha (0.55f)); g.strokePath (curve (own, false), juce::PathStrokeType (1.2f));
            }
            if (cv.selectedOn)   // the selected band's own curve, dashed gold, only where the band does something (not along the whole 0 dB line)
            {
                juce::Path p; bool pen = false;
                for (int i = 0; i < panel::kCurveN; ++i)
                {
                    const bool acts = std::abs (cv.selected[i]) > 0.05f || (i > 0 && std::abs (cv.selected[i - 1]) > 0.05f) || (i + 1 < panel::kCurveN && std::abs (cv.selected[i + 1]) > 0.05f);
                    const float x = fx (cv.f[i]), y = dy (juce::jlimit (-18.0f, 18.0f, cv.selected[i]));
                    if (acts) { if (pen) p.lineTo (x, y); else p.startNewSubPath (x, y); }
                    pen = acts;
                }
                g.setColour (dig::accent()); g.strokePath (dashed (p, 5.0f, 4.0f, 1.3f), juce::PathStrokeType (1.3f));
            }
        }
        if (am & 2)
        {
            const auto pp = trace (sp.post, true);
            g.setGradientFill (juce::ColourGradient (rgb (70, 170, 155).withAlpha (0.20f), 0.0f, (float) PT + 80.0f, rgb (40, 110, 100).withAlpha (0.0f), 0.0f, (float) PB, false)); g.fillPath (pp);
            const auto pl = trace (sp.post, false);
            g.setColour (rgb (150, 226, 208).withAlpha (0.10f)); g.strokePath (pl, juce::PathStrokeType (4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (rgb (150, 226, 208).withAlpha (0.62f)); g.strokePath (pl, juce::PathStrokeType (1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        {
            float flat[panel::kCurveN] {};
            const auto tp = curve (byp ? flat : cv.total, false);
            const float a = delta ? 0.25f : 1.0f;
            // the total curve glows on the glass (26 Oct, next level): two soft halos under a crisp cream line
            g.setColour (Theme::ink.withAlpha (0.05f * a)); g.strokePath (tp, juce::PathStrokeType (9.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (Theme::ink.withAlpha (0.10f * a)); g.strokePath (tp, juce::PathStrokeType (4.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (Theme::ink.withAlpha (a)); g.strokePath (tp, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        rebuildNodes();
        // the hover ghost (Pro-Q): the band a double-click here would make, dashed, at the pointer
        if (inside && hover < 0 && ! gesture.active() && ! ed.chooserOpen() && ! byp && c.firstFreeBand() >= 0)
        {
            const double hz = juce::jlimit (20.0, 20000.0, xf (ptr.x));
            const int type = hz <= panel::Ctl::kEdgeLowHz ? dyneq::kLowCut : hz >= panel::Ctl::kEdgeHighHz ? dyneq::kHighCut : dyneq::kBell;
            const double gdb = type == dyneq::kBell ? juce::jlimit (-18.0, 18.0, yd (ptr.y) - cv.totalAt (hz)) : 0.0;
            float gh[panel::kCurveN];
            for (int i = 0; i < panel::kCurveN; ++i) gh[i] = cv.total[i] + (float) dyneq::Engine::freeBandMagDb (type, hz, gdb, 1.0, 24, cv.f[i], cv.sr);
            g.setColour (dig::text().withAlpha (0.35f)); g.strokePath (dashed (curve (gh, false), 4.0f, 4.0f, 1.2f), juce::PathStrokeType (1.2f));
        }
        if (! byp)
            for (auto& nd : nodes)
            {
                const int i = nd.band; const float x = nd.x, y = nd.y;
                const bool sel = c.selectedBand == i;
                const float r = sel ? 11.0f : 9.0f;
                if (nd.on)
                {
                    if (panelui::NodeInfo::carved (ed, i))
                    {
                        juce::Path ring; ring.addEllipse (x - 17.0f, y - 17.0f, 34.0f, 34.0f);
                        g.setColour (Theme::teal); g.strokePath (dashed (ring, 3.0f, 3.0f, 1.6f), juce::PathStrokeType (1.6f));
                    }
                    const auto& b = cv.band[i];
                    if (b.dyn && panel::Ctl::typeHasDyn (b.type))   // the dynamic range: a gold bar from the static gain to static + RANGE, the node rides on it at the gain of now
                    {
                        const double base = cv.totalAt (b.freq) - b.gainShown, g0 = base + c.raw (panel::bandId (i, "gain")), g1 = g0 + c.raw (panel::bandId (i, "range"));
                        const float y0 = dy (juce::jlimit (-18.0, 18.0, g0)), y1 = dy (juce::jlimit (-18.0, 18.0, g1));
                        g.setColour (dig::gold().withAlpha (0.85f)); g.fillRoundedRectangle (x - 2.0f, juce::jmin (y0, y1), 4.0f, std::abs (y1 - y0), 2.0f);
                        g.drawLine (x - 6.0f, y1, x + 6.0f, y1, 2.0f);
                    }
                    if (sel) { g.setColour (dig::gold().withAlpha (0.14f)); g.fillEllipse (x - r - 8.0f, y - r - 8.0f, 2 * r + 16.0f, 2 * r + 16.0f); }   // the selected node glows
                    g.setColour (juce::Colours::black.withAlpha (0.55f)); g.fillEllipse (x - r - 2.5f, y - r - 1.5f, 2 * r + 5.0f, 2 * r + 5.0f);   // a dark halo: the node reads on top of the curve
                    g.setGradientFill (juce::ColourGradient (rgb (14, 56, 51), x, y - r, rgb (1, 20, 19), x, y + r, false)); g.fillEllipse (x - r, y - r, 2 * r, 2 * r);
                    g.setColour (sel ? dig::accent() : Theme::ink); g.drawEllipse (x - r, y - r, 2 * r, 2 * r, sel ? 2.4f : 1.6f);
                    silk (g, juce::String (i + 1), Theme::condEm (12.5f), x, y + 0.5f, 'm', 0.0f, sel ? dig::accent() : Theme::ink);
                }
                else   // an OFF band: hollow and dim, still clickable and draggable
                {
                    g.setColour (dig::plot().withAlpha (0.8f)); g.fillEllipse (x - r, y - r, 2 * r, 2 * r);
                    g.setColour ((sel ? dig::accent() : Theme::ink).withAlpha (sel ? 0.85f : 0.4f));
                    g.strokePath (dashed ([&] { juce::Path p; p.addEllipse (x - r, y - r, 2 * r, 2 * r); return p; }(), 3.0f, 2.5f, 1.4f), juce::PathStrokeType (1.4f));
                    silk (g, juce::String (i + 1), Theme::condEm (12.5f), x, y + 0.5f, 'm', 0.0f, (sel ? dig::accent() : Theme::ink).withAlpha (sel ? 0.85f : 0.45f));
                }
            }
        if (c.selectedBand >= 0) { const auto lv = NodeInfo::live (ed, c.selectedBand); if (lv.isNotEmpty()) silk (g, lv, Theme::condEm (12.5f), (float) W - 44.0f, (float) PT + 14.0f, 'r', 0.6f, dig::text().withAlpha (0.8f)); }
        {   // TARGET on but not cutting yet: say so on the display itself (it learns from the hits it hears; nothing is drawn until then)
            bool ready = false; const auto st = c.targetStatus (&ready);
            if (st.isNotEmpty() && ! ready) silk (g, "TARGET " + st, Theme::condEm (12.5f), 48.0f, (float) PT + 14.0f, 'l', 0.6f, dig::gold().withAlpha (0.9f));
        }
        if (delta) silk (g, "DELTA - ONLY WHAT IS CUT", Theme::condEm (15.0f), (float) W * 0.5f, (float) PT + 22.0f, 'm', 2.0f, dig::accent());
        if (byp) silk (g, "BYPASS - LEVEL MATCHED", Theme::condEm (15.0f), (float) W * 0.5f, (float) PT + 22.0f, 'm', 2.0f, Theme::labelDeep);
        // hover read-out: shape, frequency, gain, Q, and the net / conflict line
        if (hover >= 0 && ! gesture.active())
            for (auto& n : nodes)
                if (n.band == hover)
                {
                    const auto t1 = NodeInfo::line1 (ed, hover), t2 = NodeInfo::readout (ed, hover); const auto f = Theme::condEm (12.5f);
                    const float w = juce::jmax (fam::textWidth (t1, f, 0.4f), fam::textWidth (t2, f, 0.4f)) + 22.0f;
                    const float x = juce::jlimit (4.0f, (float) W - w - 4.0f, n.x - w * 0.5f), y = n.y > 80.0f ? n.y - 62.0f : n.y + 22.0f;
                    g.setColour (rgb (1, 14, 13).withAlpha (0.94f)); g.fillRoundedRectangle (x, y, w, 42.0f, 4.0f);
                    g.setColour (dig::line()); g.drawRoundedRectangle (x, y, w, 42.0f, 4.0f, 1.0f);
                    silk (g, t1, f, x + 11.0f, y + 12.0f, 'l', 0.4f, dig::text());
                    silk (g, t2, f, x + 11.0f, y + 30.0f, 'l', 0.4f, dig::accent());
                }
        g.restoreState();
    }
    void mouseMove (const juce::MouseEvent& e) override
    {
        ptr = e.position; inside = true; hover = nodeAt (e.position);
        setMouseCursor (hover < 0 && lockupArea().expanded (4.0f).contains (e.position) ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    }
    void mouseEnter (const juce::MouseEvent& e) override { ptr = e.position; inside = true; }
    void mouseExit (const juce::MouseEvent&) override { hover = -1; inside = false; }
    void mouseDown (const juce::MouseEvent& e) override
    {
        auto& c = ed.ctl();
        const int n = nodeAt (e.position);
        if (e.mods.isPopupMenu())   // right-click: the band's menu
        {
            if (n < 0) return;
            c.selectLab (n); ed.refreshFace();
            ed.openItems (c.menuItems (n), e.getEventRelativeTo (&ed).getPosition());
            return;
        }
        if (n < 0) return;
        for (auto& nd : nodes) if (nd.band == n) { gesture.begin (n, { nd.x, nd.y }, e.position, e.mods); ed.refreshFace(); return; }
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        ptr = e.position;
        if (! gesture.active()) return;
        const auto p = gesture.move (e.position, e.mods);
        if (! gesture.moved) return;
        auto& c = ed.ctl(); auto& cv = ed.curves(); const int i = gesture.band; const int t = c.bandType (i);
        if (! c.bandOn (i)) c.setQuiet (panel::bandId (i, "on"), 1.0f);   // dragging an OFF node switches the band on, in the same undo step
        if (e.mods.isCommandDown())   // Cmd + vertical = Q (Pro-Q), the frequency stays
        {
            c.setQuiet (panel::bandId (i, "q"), (float) juce::jlimit (0.1, 18.0, (double) gesture.q0 * std::pow (1.02, (double) (gesture.start.y - p.y))));
            return;
        }
        const double hz = juce::jlimit (20.0, 20000.0, xf (juce::jlimit (0.0f, (float) W, p.x)));
        c.setQuiet (panel::bandId (i, "freq"), (float) hz);
        if (panel::Ctl::typeHasGain (t))
        {
            cv.band[i].freq = (float) hz;   // the base is read at the new frequency, without the band's own curve
            const double base = cv.totalAt (hz) - cv.bandOneDb (i, hz);
            c.setQuiet (panel::bandId (i, "gain"), (float) juce::jlimit (-18.0, 18.0, std::round ((yd (p.y) - base) * 10.0) / 10.0));
        }
        else   // notch and the cuts: vertical = Q
            c.setQuiet (panel::bandId (i, "q"), (float) juce::jlimit (0.1, 18.0, (double) gesture.q0 * std::pow (1.02, (double) (gesture.start.y - p.y))));
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        auto& c = ed.ctl();
        if (gesture.active())
        {
            const int b = gesture.band;
            if (gesture.end() && b >= 0) c.toggleBand (b);   // Alt-click toggles on / off
            ed.refreshFace(); return;
        }
        if (! e.mouseWasDraggedSinceMouseDown() && e.getNumberOfClicks() == 1 && ! e.mods.isPopupMenu() && nodeAt (e.position) < 0 && lockupArea().expanded (4.0f).contains (e.position))
        { openLockupLink(); return; }
        if (! e.mouseWasDraggedSinceMouseDown() && e.getNumberOfClicks() == 1 && ! e.mods.isPopupMenu() && nodeAt (e.position) < 0)   // a click on empty glass deselects
        {
            c.selectLab (-1); ed.closeChooser(); ed.setDynOpen (false); ed.refreshFace();
        }
    }
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        const int n = nodeAt (e.position);
        if (n < 0) return;
        auto& c = ed.ctl();
        c.selectLab (n);
        const float d = w.deltaY != 0.0f ? w.deltaY : w.deltaX;   // macOS turns Shift + wheel into a horizontal wheel
        if (d == 0.0f) return;
        const int t = c.bandType (n);
        if (e.mods.isAltDown())   // Alt + wheel = RANGE: the dynamic switch (bell and shelves)
        {
            if (panel::Ctl::typeHasDyn (t))
            {
                svl::ParamUndo::Group one (c.proc.undo, "RANGE");
                const float r = juce::jlimit (-12.0f, 12.0f, std::round ((c.raw (panel::bandId (n, "range")) + (d > 0.0f ? 0.5f : -0.5f)) * 10.0f) / 10.0f);
                c.setQuiet (panel::bandId (n, "range"), r); c.setQuiet (panel::bandId (n, "dyn"), panel::Ctl::rangeMeansDyn (r) ? 1.0f : 0.0f);
            }
        }
        else if (panel::Ctl::isCut (t) && ! e.mods.isShiftDown())   // a cut: the wheel steps the slope (Pro-Q); Shift + wheel = Q
        {
            int k = 0; for (int j = 0; j < 5; ++j) if (panel::Ctl::kSlopeList[j] == c.bandSlopeDb (n)) k = j;
            const int k2 = juce::jlimit (0, 4, k + (d > 0.0f ? 1 : -1));
            if (k2 != k) c.setSlope (n, panel::Ctl::kSlopeList[k2]);
        }
        else
        {
            const auto id = panel::bandId (n, "q");
            c.setGesture (id, (float) juce::jlimit (0.1, 18.0, (double) c.raw (id) * std::exp (0.3 * (double) juce::jlimit (-3.0f, 3.0f, d))));
        }
        ed.refreshFace();
    }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int n = nodeAt (e.position);
        if (n >= 0) { ed.ctl().selectLab (n); ed.refreshFace(); ed.editFreqBox(); return; }   // a double-click on a node: type its frequency (Pro-Q); on / off is Alt-click and the chip's dot
        const double hz = xf (e.position.x);
        ed.curves().update (ed.ctl());
        ed.ctl().newBandAt (hz, yd (e.position.y) - ed.curves().totalAt (hz));
        ed.refreshFace();
    }
    juce::StringArray boundParams() const override
    {
        juce::StringArray a;
        for (int i = 0; i < panel::kBands; ++i) for (const char* n : { "freq", "gain", "q", "on", "type", "slope", "range", "dyn" }) a.add (panel::bandId (i, n));
        return a;
    }
    juce::String uiAction() const override { return "LAB DISPLAY"; }
    // test access
    juce::Point<float> nodePosition (int band) const { for (auto& n : nodes) if (n.band == band) return { n.x, n.y }; return { -1.0f, -1.0f }; }
    bool nodeIsHollow (int band) const { for (auto& n : nodes) if (n.band == band) return ! n.on; return false; }
    int hoveredBand() const { return hover; }
    void setHover (int b) { hover = b; }
    void setPointer (juce::Point<float> p, bool in) { ptr = p; inside = in; hover = in ? nodeAt (p) : -1; }
private:
    struct Node { int band; float x, y; bool on; };
    int nodeAt (juce::Point<float> p) const
    {
        int best = -1; float bd = 14.0f;
        for (auto& n : nodes) { const float d = p.getDistanceFrom ({ n.x, n.y }); if (d < bd) { bd = d; best = n.band; } }
        return best;
    }
    DyneqEditor& ed; std::vector<Node> nodes; int hover = -1; bool inside = false; juce::Point<float> ptr; NodeGesture gesture; CachedLayer bgLayer;
    juce::Image mark;
};

class SizeChip : public juce::Component, public PanelControl, private PanelCounted
{
public:
    explicit SizeChip (DyneqEditor& e) : ed (e) { setBounds (DyneqEditor::kW - 72, lab::kChipY, 56, lab::kChipH); }
    void paint (juce::Graphics& g) override
    {
        // a small key at the right end of the band strip
        const auto b = getLocalBounds().toFloat().reduced (1.0f);
        dig::key (g, b, "", false, isMouseOver(), false, 8.0f);
        silk (g, "SIZE", Theme::legendEm (7.0f), b.getCentreX(), b.getCentreY() - 8.0f, 'm', 1.4f, dig::dim());
        silk (g, juce::String (ed.scalePercent()) + " %", Theme::condEm (12.0f), b.getCentreX(), b.getCentreY() + 8.0f, 'm', 0.4f, dig::text());
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.mouseWasDraggedSinceMouseDown()) return;
        juce::PopupMenu m;
        for (int p : { 75, 85, 100 }) m.addItem (p, juce::String (p) + " %", true, p == ed.scalePercent());
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this), [this] (int r) { if (r > 0) ed.setScalePercent (r); });
    }
    juce::StringArray boundParams() const override { return {}; }
    juce::String uiAction() const override { return "SIZE 75 / 85 / 100 %"; }
private:
    DyneqEditor& ed;
};

//==============================================================================
/** LAB, five zones from the top (UX redesign 11 Oct): A the top bar (name and model, the ASSIST chips, analyzer, DELTA, BYPASS), B the band chips,
    C the display, D the docked strip of the selected band, E ADVANCED (collapsed), the meters and OUTPUT. Nothing floats over the display. */
class LabFace : public juce::Component, private PanelCounted
{
public:
    explicit LabFace (DyneqEditor& e);
    void paint (juce::Graphics& g) override;
    void refresh (int tick);
    LabScreen& screen() { return *scr; }
    BandStrip& strip() { return *strip_; }
    DynPop& dynPop() { return *pop; }
    BandChip& chip (int i) { return *chips[(size_t) i]; }
    AssistChip& assistChip (int i) { return *assistChips[i]; }
    bool popoverOwns (const juce::Component* c) const { return c != nullptr && (c == pop.get() || pop->isParentOf (c)); }
private:
    DyneqEditor& ed;
    std::unique_ptr<LabScreen> scr;
    std::vector<std::unique_ptr<BandChip>> chips;
    std::unique_ptr<BandStrip> strip_;
    std::unique_ptr<DynPop> pop;
    std::unique_ptr<LabFooter> footer;
    std::unique_ptr<LabMeters> meters;
    std::unique_ptr<AssistChip> assistChips[3];
    std::unique_ptr<PlateKeyButton> anaPre, anaPost, anaKey, deltaKey, bypassKey;
    friend class ::DyneqEditor;
};

} // namespace panelui

//==============================================================================
using namespace panelui;

LabFace::LabFace (DyneqEditor& e) : ed (e)
{
    setBounds (0, 0, DyneqEditor::kW, DyneqEditor::kH);
    constexpr int W = DyneqEditor::kW;
    scr = std::make_unique<LabScreen> (ed); scr->setBounds (lab::kPlotX, lab::kPlotY, LabScreen::W, LabScreen::H); addAndMakeVisible (*scr);
    for (int i = 0; i < panel::kBands; ++i)
    {
        chips.push_back (std::make_unique<BandChip> (ed, i));
        chips.back()->setBounds (lab::kChipX0 + i * lab::kChipStep, lab::kChipY, lab::kChipW, lab::kChipH);
        addAndMakeVisible (*chips.back());
    }
    strip_ = std::make_unique<BandStrip> (ed); strip_->setBounds (lab::kStripX, lab::kStripY, lab::kStripW, lab::kStripH); addAndMakeVisible (*strip_);
    pop = std::make_unique<DynPop> (ed); pop->setBounds (lab::kPopX, lab::kPopY, lab::kPopW, lab::kPopH); addChildComponent (*pop);
    footer = std::make_unique<LabFooter> (ed); footer->setBounds (lab::kFootX, lab::kFootY, lab::kFootW, lab::kFootH); addAndMakeVisible (*footer);
    meters = std::make_unique<LabMeters> (ed); meters->setBounds (lab::kMeterX, lab::kTopY, lab::kMeterW, lab::kTopH); addAndMakeVisible (*meters);
    // zone A: the ASSIST chips (TARGET / COLLIDE / SELF), the analyzer (PRE / POST: the analyzer parameter's two bits; KEY: the sidechain trace), DELTA (hold), BYPASS
    for (int i = 0; i < 3; ++i)
    {
        assistChips[i] = std::make_unique<AssistChip> (ed, panel::kTarget + i);
        assistChips[i]->setBounds (lab::kAssistX0 + i * lab::kAssistStep, lab::kTopY, lab::kAssistW, lab::kTopH); addAndMakeVisible (*assistChips[i]);
    }
    auto key = [&] (const char* text, std::function<bool()> lit, int x, int w)
    {
        auto k = std::make_unique<PlateKeyButton> (text, std::move (lit), 8.4f, 1.2f, 2.0f);
        k->setBounds (x, lab::kTopY, w, lab::kTopH); addAndMakeVisible (*k); return k;
    };
    auto& c = ed.ctl();
    anaPre = key ("PRE", [&c] { return (c.analyzerMode() & 1) != 0; }, W - 308, 46);
    anaPost = key ("POST", [&c] { return (c.analyzerMode() & 2) != 0; }, W - 260, 52);
    for (auto* k : { anaPre.get(), anaPost.get() })
    {
        const int bit = k == anaPre.get() ? 1 : 2;
        k->bound = [] { return juce::StringArray { "analyzer" }; };
        k->onClick = [this, bit] { auto& cc = ed.ctl(); cc.setGesture ("analyzer", (float) (cc.analyzerMode() ^ bit)); ed.refreshFace(); };
    }
    anaKey = key ("KEY", [this] { return ed.keyTraceOn(); }, W - 206, 46);
    anaKey->action = "KEY TRACE";
    anaKey->onClick = [this] { ed.setKeyTrace (! ed.keyTraceOn()); };
    deltaKey = key ("DELTA", [&c] { return c.delta(); }, W - 154, 62);
    deltaKey->bound = [] { return juce::StringArray { "delta" }; };
    deltaKey->onDownChange = [this] (bool d) { ed.ctl().deltaSet (d); };   // momentary: held = DELTA
    bypassKey = key ("BYPASS", [&c] { return c.bypass(); }, W - 88, 72);
    bypassKey->bound = [] { return juce::StringArray { "bypass" }; };
    bypassKey->onClick = [this] { ed.ctl().setGesture ("bypass", ed.ctl().bypass() ? 0.0f : 1.0f); ed.refreshFace(); };
    refresh (0);
}

void LabFace::refresh (int tick)
{
    auto& c = ed.ctl();
    if (c.selectedBand >= 0 && ! juce::isPositiveAndBelow (c.selectedBand, panel::kBands)) c.selectedBand = -1;
    strip_->refresh();
    const bool popShown = ed.dynOpen() && c.selectedBand >= 0;
    pop->setVisible (popShown);
    if (popShown) { pop->setTopLeftPosition (lab::kPopX, ed.advancedOpen() ? lab::kAdvY - lab::kPopH - 4 : lab::kPopY); pop->refresh(); pop->toFront (false); }
    for (auto& ch : chips) { const double s = ch->signature(); if (s != ch->lastSig) { ch->lastSig = s; ch->repaint(); } }
    if (tick % 3 == 0) { for (auto& a : assistChips) a->repaint(); footer->repaint(); meters->repaint(); }
}

void LabFace::paint (juce::Graphics& g)
{
    const float W = (float) DyneqEditor::kW, H = (float) DyneqEditor::kH, w = W - 16.0f;
    // the chassis round the glass (owner 26 Oct: "bir sonraki seviyeye çıkar, prototip gibi duruyor"): dark anodised green, lit along the top
    g.setGradientFill (juce::ColourGradient (rgb (8, 58, 53), 0.0f, 0.0f, rgb (2, 26, 24), 0.0f, H, false)); g.fillAll();
    g.setColour (juce::Colours::white.withAlpha (0.08f)); g.fillRect (0.0f, 0.0f, W, 1.0f);
    g.setColour (juce::Colours::black.withAlpha (0.5f)); g.fillRect (0.0f, H - 1.0f, W, 1.0f);
    // ONE screen holds everything (owner 11 Oct: "her sey ekrana gomulu", "herşey dijital ekranda olsun"): a sheet of glass set into the chassis
    const juce::Rectangle<float> glass (8.0f, (float) lab::kScreenY, w, (float) lab::kScreenH);
    g.setColour (juce::Colours::black.withAlpha (0.55f)); g.fillRoundedRectangle (glass.expanded (1.5f), 11.0f);   // the bezel's shadow line
    g.setGradientFill (juce::ColourGradient (rgb (6, 40, 37), 0.0f, glass.getY(), rgb (2, 22, 20), 0.0f, glass.getBottom(), false));
    g.fillRoundedRectangle (glass, 10.0f);
    g.setColour (juce::Colours::white.withAlpha (0.06f)); g.drawRoundedRectangle (glass.reduced (0.5f), 10.0f, 1.0f);   // the glass edge catches the light
    // UI signature 3: the product name and its model, top left
    const float cy = (float) lab::kTopY + 0.5f * (float) lab::kTopH;
    silk (g, Theme::kProductName, Theme::condEm (17.5f), 18.0f, cy - 5.0f, 'l', 3.2f, dig::accent());
    silk (g, juce::String (Theme::kModelLine).upToFirstOccurrenceOf (" - ", false, false), Theme::legendEm (6.2f), 19.0f, cy + 10.0f, 'l', 2.2f, dig::dim());
}

//==============================================================================
//==============================================================================
DyneqEditor::DyneqEditor (DyneqAudioProcessor& p) : AudioProcessorEditor (p), proc (p), model (p)
#if SVL_LICENSING
    , licenseOverlay (p.license)
#endif
{
    lab = std::make_unique<LabFace> (*this);
    sizeChip = std::make_unique<SizeChip> (*this);
    chooser = std::make_unique<Chooser> (*this);
    addAndMakeVisible (*lab);
    addAndMakeVisible (*sizeChip);
    for (int i = 0; i < 3; ++i) { assist[i] = std::make_unique<AssistOverlay> (*this, panel::kTarget + i); addChildComponent (*assist[i]); }
    adv = std::make_unique<AdvancedPanel> (*this); addChildComponent (*adv);
    addChildComponent (*chooser);
    helpTip = std::make_unique<HelpTip>(); addChildComponent (*helpTip);
    addMouseListener (this, true);
    setWantsKeyboardFocus (true);
    setResizable (false, false);
    setSize (kW, kH);
    layoutGlobals();
#if SVL_LICENSING
    licenseButton.setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    licenseButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::transparentBlack);
    licenseButton.setColour (juce::TextButton::textColourOffId, Theme::ink.withAlpha (0.6f));
    licenseButton.setBounds (kW - 84, kH - 16, 70, 14);   // the strip under the bottom bar
    licenseButton.onClick = [this] { licenseOverlay.refresh(); licenseOverlay.setVisible (true); licenseOverlay.toFront (true); };
    addAndMakeVisible (licenseButton);
    addChildComponent (licenseOverlay);
    licenseOverlay.setBounds (getLocalBounds());
    licenseOverlay.setVisible (! proc.license.isLicensed());
#endif
    setScaleFactor ((float) proc.editorScalePct.load() / 100.0f);
    cv.update (model);
    refreshFace();
    startTimerHz (kTimerHz);
}

DyneqEditor::~DyneqEditor()
{
    stopTimer();
    model.deltaSet (false);
    removeMouseListener (this);
}

void DyneqEditor::paint (juce::Graphics& g) { g.fillAll (Theme::panel); }
void DyneqEditor::resized() {}

juce::Component& DyneqEditor::labFace() { return *lab; }
juce::Component& DyneqEditor::labScreen() { return lab->screen(); }
juce::Point<float> DyneqEditor::testNodePosition (int band) { return lab->screen().nodePosition (band) + lab->screen().getPosition().toFloat(); }
juce::Point<float> DyneqEditor::testLabPoint (double hz, double db) { auto& s = lab->screen(); return { s.fx (hz), s.dy (db) }; }
juce::String DyneqEditor::testReadout (int band) { return NodeInfo::readout (*this, band); }
juce::String DyneqEditor::testReadout1 (int band) { return NodeInfo::line1 (*this, band); }
bool DyneqEditor::testCarved (int band) { return NodeInfo::carved (*this, band); }
void DyneqEditor::testPointer (std::optional<juce::Point<float>> p) { lab->screen().setPointer (p.value_or (juce::Point<float>()), p.has_value()); }
void DyneqEditor::testHover (int band) { lab->screen().setHover (band); }
juce::Rectangle<float> DyneqEditor::testLockupArea() { return lab->screen().lockupArea() + lab->screen().getPosition().toFloat(); }
void DyneqEditor::setLockupLinkForTest (std::function<void()> fn) { lab->screen().openLockupLink_forTest = std::move (fn); }

void DyneqEditor::layoutGlobals()
{
    sizeChip->setBounds (kW - 72, lab::kChipY, 56, lab::kChipH);
    adv->setBounds (lab::kAdvX, lab::kAdvY, lab::kAdvW, lab::kAdvH); adv->layoutRow();   // a popover over the bottom of the display
    for (int i = 0; i < 3; ++i)   // under its chip, over the band strip and the top of the display
        assist[i]->setTopLeftPosition (juce::jmin (lab::kAssistX0 + i * lab::kAssistStep, kW - 8 - assist[i]->getWidth()), lab::kTopY + lab::kTopH + 2);
}

void DyneqEditor::refreshFace()
{
    // ADVANCED: a popover over the bottom of the display (collapsed by default)
    adv->setVisible (advShown);
    for (int i = 0; i < 3; ++i) { assist[i]->setVisible (overlay == panel::kTarget + i); if (overlay == panel::kTarget + i) assist[i]->refresh(); }
    sizeChip->toFront (false);
    adv->toFront (false); for (int i = 0; i < 3; ++i) assist[i]->toFront (false);
    chooser->toFront (false);
    if (helpTip->isVisible()) helpTip->toFront (false);
    lab->refresh (tick);
    sizeChip->repaint();
    lab->repaint();
}

void DyneqEditor::toggleOverlay (int m) { overlay = overlay == m ? -1 : m; refreshFace(); }
void DyneqEditor::closeOverlay() { overlay = -1; refreshFace(); }
bool DyneqEditor::advancedOpen() const { return advShown; }
void DyneqEditor::setAdvancedOpen (bool open) { advShown = open; refreshFace(); }
void DyneqEditor::setDynOpen (bool open) { dynShown = open; refreshFace(); }

juce::String DyneqEditor::advancedBakeText() const { return adv != nullptr ? adv->bakeText() : juce::String(); }
void DyneqEditor::editFreqBox() { lab->strip().beginEditFreq(); }

void DyneqEditor::setScalePercent (int pct)
{
    pct = pct == 75 || pct == 85 ? pct : 100;
    proc.editorScalePct = pct;
    setScaleFactor ((float) pct / 100.0f);
    sizeChip->repaint();
}

bool DyneqEditor::openChooser (juce::Point<int> where)
{
    auto items = model.chooserItems();
    if (items.empty()) return false;
    chooser->show (std::move (items), where);
    return true;
}
void DyneqEditor::openItems (std::vector<panel::ChooserItem> items, juce::Point<int> where)
{
    if (items.empty()) return;
    chooser->show (std::move (items), where);
}
void DyneqEditor::closeChooser() { if (chooser != nullptr) chooser->setVisible (false); }
int DyneqEditor::chooserCount() const { return chooser->numItems(); }
juce::String DyneqEditor::chooserText (int i) const { return chooser->itemText (i); }
bool DyneqEditor::chooserSelected (int i) const { return chooser->itemSelected (i); }
bool DyneqEditor::chooserHeading (int i) const { return chooser->itemHeading (i); }
void DyneqEditor::chooserPick (int i) { chooser->choose (i); }
bool DyneqEditor::chooserOpen() const { return chooser != nullptr && chooser->isVisible(); }

void DyneqEditor::timerCallback()
{
#if SVL_LICENSING
    if (! proc.license.isLicensed() && ! licenseOverlay.isVisible())   // a licence the server revoked while the editor is open
    {
        licenseOverlay.refresh();
        licenseOverlay.setVisible (true);
        licenseOverlay.toFront (true);
    }
#endif
    ++tick;
    cv.update (model);
    const float in = proc.getInDb(), out = proc.getOutDb();
    inShown += (in - inShown) * (in > inShown ? 0.6f : 0.15f);
    outShown += (out - outShown) * (out > outShown ? 0.6f : 0.15f);
    // only the traces the display shows are computed
    const int am = model.analyzerMode();
    spec.update (proc, cv.f, (am & 1) != 0, (am & 2) != 0, keyTrace && model.layerOn (panel::kCollide) && proc.isSidechainConnected());
    lab->screen().rebuildNodes();
    // what moves every frame is the display (analyzer, curves, nodes); the rest of the panel (keys, knobs, popovers, meters) is repainted about 7 times a second
    lab->refresh (tick);
    lab->screen().repaint();
    if (tick % 6 == 0) { lab->repaint(); for (int i = 0; i < 3; ++i) if (assist[i]->isVisible()) assist[i]->refresh(); }
    if (tick % 6 == 0) { adv->repaint(); }
    if (adv->isVisible() && tick % 3 == 0) adv->refreshBake();
    // hover help: open after the pointer rested kHelpDelayTicks on a control that has help; while open, its text follows what is true now
    if (helpOver != nullptr)   // the control went away (a popover closed): its help goes too. Visibility up to the editor, not isShowing(): headless snapshots have no window
    {
        bool vis = true;
        for (auto* p = helpOver.getComponent(); p != nullptr && p != this; p = p->getParentComponent()) vis = vis && p->isVisible();
        if (! vis) { helpOver = nullptr; hideHelp(); }
    }
    if (helpOver != nullptr)
    {
        if (! helpTip->isVisible()) { if (++helpTicks >= kHelpDelayTicks) showHelpFor (*helpOver); }
        else if (tick % 10 == 0) { const auto it = helps.find (helpOver.getComponent()); if (it != helps.end() && it->second.second () != helpTip->text()) showHelpFor (*helpOver); }
    }
}

void DyneqEditor::setHelp (juce::Component& c, const juce::String& title, HelpFn body) { helps[&c] = { title, std::move (body) }; }
bool DyneqEditor::showHelpFor (juce::Component& c)
{
    const auto it = helps.find (&c);
    if (it == helps.end() || helpTip == nullptr) return false;
    const auto anchor = getLocalArea (&c, c.getLocalBounds());
    helpTip->show (it->second.first, it->second.second ? it->second.second() : juce::String(), anchor, getLocalBounds());
    helpOver = &c;
    return true;
}
void DyneqEditor::hideHelp() { if (helpTip != nullptr) helpTip->setVisible (false); helpTicks = 0; }
bool DyneqEditor::helpShown() const { return helpTip != nullptr && helpTip->isVisible(); }
juce::String DyneqEditor::helpTitle() const { return helpTip != nullptr ? helpTip->title() : juce::String(); }
juce::String DyneqEditor::helpText() const { return helpTip != nullptr ? helpTip->text() : juce::String(); }
void DyneqEditor::mouseMove (const juce::MouseEvent& e)
{
    juce::Component* hit = nullptr;
    for (auto* p = e.eventComponent; p != nullptr && p != this; p = p->getParentComponent()) if (helps.count (p) != 0) { hit = p; break; }
    if (hit != helpOver.getComponent()) { helpOver = hit; hideHelp(); }
}
void DyneqEditor::mouseExit (const juce::MouseEvent& e)
{
    if (e.eventComponent == helpOver.getComponent() || e.eventComponent == this) { helpOver = nullptr; hideHelp(); }
}

bool DyneqEditor::keyPressed (const juce::KeyPress& k)
{
#if SVL_LICENSING
    if (licenseOverlay.isVisible()) return false;
#endif
    if (helpShown() && k.getKeyCode() == juce::KeyPress::escapeKey) { helpOver = nullptr; hideHelp(); return true; }
    if (svl::handleUndoKey (proc.undo, k)) return true;
    return labKey (k);
}

bool DyneqEditor::labKey (const juce::KeyPress& k)   // spec 3.10; Space and everything else stays with the host. Key codes are compared, not KeyPress objects: KeyPress == int is false while Shift is down
{
    auto& c = model;
    const int code = k.getKeyCode(); const auto m = k.getModifiers();
    if (m.isCommandDown() || m.isCtrlDown()) return false;
    if (code >= '1' && code <= '6') { c.selectLab (code - '1'); refreshFace(); return true; }
    if (code == juce::KeyPress::escapeKey)
    {
        if (chooserOpen()) closeChooser();
        else if (overlay >= 0) closeOverlay();
        else if (dynShown) setDynOpen (false);
        else c.selectLab (-1);
        refreshFace(); return true;
    }
    const int b = c.selectedBand;
    if (code == juce::KeyPress::tabKey) { lab->strip().editNext (m.isShiftDown()); return b >= 0; }
    if (b < 0) return false;
    if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey) { c.deleteBand (b); refreshFace(); return true; }   // Pro-Q
    if (code == juce::KeyPress::returnKey) { if (c.isEmptySlot (b)) c.addSlot (b); else lab->strip().beginEditFreq(); refreshFace(); return true; }
    if (code == 'b' || code == 'B') { c.toggleBand (b); refreshFace(); return true; }
    if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey)
    {
        const auto id = panel::bandId (b, "freq");
        const double oct = (m.isShiftDown() ? 1.0 / 12.0 : 1.0 / 48.0) * (code == juce::KeyPress::rightKey ? 1.0 : -1.0);
        c.setGesture (id, (float) juce::jlimit (20.0, 20000.0, c.raw (id) * std::pow (2.0, oct))); refreshFace(); return true;
    }
    if (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey)
    {
        const float dir = code == juce::KeyPress::upKey ? 1.0f : -1.0f;
        if (panel::Ctl::typeHasGain (c.bandType (b))) { const auto id = panel::bandId (b, "gain"); c.setGesture (id, juce::jlimit (-18.0f, 18.0f, c.raw (id) + dir * (m.isShiftDown() ? 1.0f : 0.1f))); }
        else { const auto id = panel::bandId (b, "q"); c.setGesture (id, juce::jlimit (0.1f, 18.0f, c.raw (id) * std::pow (m.isShiftDown() ? 1.2f : 1.02f, dir))); }
        refreshFace(); return true;
    }
    return false;
}
bool DyneqEditor::keyStateChanged (bool)
{
    const bool d = juce::KeyPress::isKeyCurrentlyDown ('d') || juce::KeyPress::isKeyCurrentlyDown ('D');
    if (d != model.deltaHeld) { model.deltaSet (d); refreshFace(); }
    return false;
}
void DyneqEditor::mouseDown (const juce::MouseEvent& e)
{
#if SVL_LICENSING
    if (licenseOverlay.isVisible()) return;
#endif
    svl::takeFocusOnClick (*this, e);
    hideHelp(); helpOver = nullptr;   // a click closes the hover help (it never takes the click itself)
    if (chooser != nullptr && chooser->isVisible() && e.eventComponent != chooser.get() && ! chooser->isParentOf (e.eventComponent)) closeChooser();
    // the "..." popover closes on a click anywhere else (its own key toggles it)
    if (dynShown && ! lab->popoverOwns (e.eventComponent))
    {
        auto* b = dynamic_cast<panelui::PlateKeyButton*> (e.eventComponent);
        if (b == nullptr || b->uiAction() != "DYNAMIC SETTINGS") setDynOpen (false);
    }
}

void DyneqEditor::forEachControl (const std::function<void (juce::Component&, PanelControl&)>& fn)
{
    std::function<void (juce::Component&)> walk = [&] (juce::Component& c)
    {
        for (auto* ch : c.getChildren())
        {
            if (! ch->isVisible()) continue;
            if (auto* pc = dynamic_cast<PanelControl*> (ch)) fn (*ch, *pc);
            walk (*ch);
        }
    };
    walk (*this);
}
