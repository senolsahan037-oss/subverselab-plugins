#include "PluginEditor.h"
#include "KnobDrag.h"
#include <time.h>

// "Everything is part of the machine" (docs/UI_SPEC_v2.md): the grinder stands on a cast-iron plinth whose pinstriped
// front panel carries eight cast-iron knobs with riveted brass tags, a gold decal logo and a maker's plate.

namespace
{
using juce::AffineTransform;
using juce::Colour;
using juce::Graphics;
using juce::Path;
using juce::Point;
using juce::Rectangle;

constexpr float kPi = juce::MathConstants<float>::pi;
constexpr float kTwoPi = juce::MathConstants<float>::twoPi;

// Design space, 1200 x 800. The grinder is the v1 body at scale 0.75.
constexpr float kBodyScale = 0.75f;
constexpr float kBodyX = 433.6f, kBodyY = 104.0f;
const Point<float> kPivot { 600.0f, 292.5f };          // crank pivot
const Point<float> kOutlet { 458.0f, 282.5f };         // left barrel face
constexpr float kRimY = 111.0f, kFloorY = 519.0f;      // hopper rim; where the pour lands on the plinth's top surface (it spans y 502..522)
constexpr float kRecordX = 603.0f, kRecordR = 87.5f;   // the record is 175 px, centred on the hopper mouth
constexpr float kHopperL = 486.0f, kHopperR = 726.0f;  // the record is clipped to the hopper's inner x range
constexpr float kRecordClipBottom = 216.0f;            // below this the body no longer covers the funnel
constexpr float kPileL = 250.0f, kPileR = 445.0f, kPileMid = 347.0f;
constexpr float kPanelY = 620.0f;                      // knob centres
constexpr float kPlinthX = 10.0f, kPlinthY = 470.0f;
// The seal (mark of SubverseLab · BabaSultan Edition): a big brass medallion screwed onto the backdrop, top left (docs/UI_SPEC_v4_switches.md).
const Rectangle<float> kSealRect { 40.0f, 40.0f, 232.0f, 232.0f };   // ONE rectangle: square, centred (156, 156). Empty = no seal.
// The sweet-spot switch panel: a wall-mounted cast-iron panel screwed to the backdrop, top right, level with the seal (docs/UI_SPEC_v4_switches.md, v4.1).
// bolt heads: the oval recesses of the foot pads, measured in body@2x.png (95, 1052) and (810, 1052) and mapped through the body's placement
const Point<float> kFootL { 469.2f, 498.5f }, kFootR { 737.4f, 498.5f };
const Rectangle<float> kBankRect { 900.0f, 40.0f, 270.0f, 232.0f };
constexpr float kSwX[5] { 935.0f, 985.0f, 1035.0f, 1085.0f, 1135.0f };
constexpr float kSwPivotY = 160.0f, kSwLever = 46.0f, kSwJewelY = 96.0f, kSwTagY = 244.0f;
constexpr float kFlipSeconds = 0.09f, kSettleSeconds = 0.06f, kOvershootDeg = 14.0f;

inline float hash01 (uint32_t a, uint32_t b = 0, uint32_t c = 0)
{
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA6Bu ^ (c + 0x165667B1u) * 0xC2B2AE35u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return (float) (h >> 8) / 16777216.0f;
}

struct Rand
{
    uint32_t s;
    explicit Rand (uint32_t seed) : s (seed) {}
    float next() { s = s * 1664525u + 1013904223u; return (float) (s >> 8) / 16777216.0f; }
    float range (float a, float b) { return a + (b - a) * next(); }
};

inline float approach (float cur, float target, float rate, float dt) { return cur + (target - cur) * (1.0f - std::exp (-rate * dt)); }

const Colour kGold { 0xffd4a537 };

struct KnobDef { const char* id; const char* tag; float x; };
constexpr KnobDef kKnobs[8] {
    { kiymaIds::glitch, "DENSITY", 147.0f },    { kiymaIds::brutal, "REACH", 242.0f },    { kiymaIds::offset, "PATTERN", 337.0f },
    { kiymaIds::envelope, "LENGTH", 432.0f },   { kiymaIds::attack, "EDGE", 770.0f },     { kiymaIds::release, "STEPS", 865.0f },
    { kiymaIds::mix, "DRY/WET", 960.0f },       { kiymaIds::output, "OUTPUT", 1055.0f } };
} // namespace

//==============================================================================
class KiymaEditor::Stage : public juce::Component
{
public:
    Stage (KiymaProcessor& p, KiymaEditor&) : proc (p)
    {
        setOpaque (true);
        loadAssets();
        buildPile();
        buildBigPieces();
        buildStrands();
        seal = std::make_unique<SealHit> (*this);
        addChildComponent (*seal);
        seal->setBounds (kSealRect.toNearestInt());
        seal->setVisible (! kSealRect.isEmpty());
        for (int i = 0; i < 5; ++i)
        {
            swHits[(size_t) i] = std::make_unique<SwHit> (*this, i);
            addAndMakeVisible (*swHits[(size_t) i]);
            swHits[(size_t) i]->setBounds ((int) kSwX[i] - 24, 84, 48, 174);
        }
        shownPreset = proc.getActivePreset();   // the session's switch starts up, without a flip
        for (int i = 0; i < 5; ++i)
        {
            sw[(size_t) i].up = i == shownPreset;
            sw[(size_t) i].theta = sw[(size_t) i].from = sw[(size_t) i].to = sw[(size_t) i].up ? 0.0f : kPi;
        }

        for (int i = 0; i < 8; ++i)
        {
            knobs[(size_t) i] = std::make_unique<KnobComp> (*this, i);
            addAndMakeVisible (*knobs[(size_t) i]);
            knobs[(size_t) i]->setBounds ((int) (kKnobs[i].x - 36.0f), (int) (kPanelY - 36.0f), 72, 72);
        }

        crankParam = p.apvts.getParameter (kiymaIds::crank);
        crankAtt = std::make_unique<juce::ParameterAttachment> (*crankParam, [] (float) {});
        crankShown = crankParam->convertFrom0to1 (crankParam->getValue());
        rec.state = Rec::Fall;
        rec.y = -200.0f;
        setSize (kWidth, kHeight);
        startLoop();
    }

    //--------------------------------------------------------------------------
    bool fakeAudio = false, spinEnabled = true;
    int forcedHover = -1;

    //--------------------------------------------------------------------------
    // The live loop. A 30 Hz timer (never faster), real dt, dirty regions only, nothing at all when idle.
    struct Ticker : juce::Timer { Stage* st = nullptr; void timerCallback() override { st->onTimer(); } };
    struct Waker : juce::MouseListener
    {
        Stage* st = nullptr;
        void mouseEnter (const juce::MouseEvent&) override { st->wake(); }
        void mouseMove (const juce::MouseEvent&) override { st->wake(); }
        void mouseDown (const juce::MouseEvent&) override { st->wake(); }
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override { st->wake(); }
    };
    struct ParamWatch : juce::AudioProcessorValueTreeState::Listener
    {
        std::atomic<bool> changed { false };
        void parameterChanged (const juce::String&, float) override { changed.store (true, std::memory_order_relaxed); }   // may be the audio thread: only a flag
    };

    void startLoop()
    {
        ticker.st = this;
        waker.st = this;
        addMouseListener (&waker, true);
        for (auto* id : { kiymaIds::glitch, kiymaIds::brutal, kiymaIds::offset, kiymaIds::envelope, kiymaIds::attack, kiymaIds::release, kiymaIds::mix, kiymaIds::crank, kiymaIds::output })
            proc.apvts.addParameterListener (id, &paramWatch);
        startTimer30();
    }
    ~Stage() override
    {
        ticker.stopTimer();
        for (auto* id : { kiymaIds::glitch, kiymaIds::brutal, kiymaIds::offset, kiymaIds::envelope, kiymaIds::attack, kiymaIds::release, kiymaIds::mix, kiymaIds::crank, kiymaIds::output })
            proc.apvts.removeParameterListener (id, &paramWatch);
        removeMouseListener (&waker);
    }
    void startTimer30() { ticker.startTimer (33); }

    /** Back to animating (a mouse move, a parameter change, audio). */
    void wake()
    {
        if (! idle)
            return;
        idle = false;
        lastTick = 0.0;
        quietSince = juce::Time::getMillisecondCounterHiRes() * 0.001;
        startTimer30();
    }

    void onTimer()
    {
        const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
        if (idle)
        {
            // nothing is drawn while idle; this only looks at two numbers
            if (paramWatch.changed.exchange (false) || proc.uiOutputRms.load (std::memory_order_relaxed) >= kQuietRms)
                wake();
            return;
        }
        paramWatch.changed.store (false, std::memory_order_relaxed);
        if (now - lastTick < 0.031 && lastTick > 0.0)   // at least 31 ms between frames, whatever the timer does
            return;
        const auto rects = frame (now);
        for (const auto& r : rects)
            repaint (r);
        if (idle)
            ticker.startTimer (100);
    }

    static constexpr float kQuietRms = 3.1623e-4f;   // -70 dBFS

    static Rectangle<int> padded (Rectangle<float> r) { return r.expanded (3.0f).getSmallestIntegerContainer().getIntersection ({ 0, 0, kWidth, kHeight }); }
    static Rectangle<float> knobRect (int i) { return { kKnobs[i].x - 40.0f, kPanelY - 40.0f, 80.0f, 80.0f }; }
    static Rectangle<float> tagRect (int i) { return { kKnobs[i].x - 46.0f, 664.0f, 92.0f, 26.0f }; }
    static Rectangle<float> recordRect() { return { kHopperL - 8.0f, 0.0f, kHopperR - kHopperL + 16.0f, kRecordClipBottom + 8.0f }; }
    static Rectangle<float> pourRect() { return { kPileL - 45.0f, kOutlet.y - 34.0f, kOutlet.x + 14.0f - (kPileL - 45.0f), kFloorY + 10.0f - (kOutlet.y - 34.0f) }; }
    static Rectangle<float> switchArea() { return { kBankRect.getX() + 8.0f, 78.0f, kBankRect.getWidth() - 16.0f, 166.0f }; }

    uint64_t switchKeyNow() const
    {
        uint64_t key = (uint64_t) (swOver + 2) * 1000003ull;
        for (const auto& w : sw)
        {
            const float age = time - w.sparkT0;
            key = key * 1315423911ull + (uint64_t) juce::roundToInt (w.theta * 4000.0f + 40000.0f) + (w.up ? 77ull : 0ull)
                  + (w.sparkT0 >= 0.0f && age >= 0.0f && age < 0.05f ? (uint64_t) (age * 1000.0f) + 1ull : 0ull);
        }
        return key;
    }

    /** One tick of the live loop: advances the animation by the real dt and returns what has to be repainted (nothing = nothing). */
    std::vector<Rectangle<int>> frame (double now)
    {
        std::vector<Rectangle<int>> out;
        const float dt = (float) juce::jlimit (0.001, 0.1, lastTick > 0.0 ? now - lastTick : 1.0 / 30.0);
        lastTick = now;
        step (dt);

        frameDriven = true;
        if (rec.state == Rec::Fall && ! fakeAudio && proc.uiOutputRms.load (std::memory_order_relaxed) < kQuietRms && proc.uiInputRms.load (std::memory_order_relaxed) < kQuietRms)
        {
            // no audio: the record is parked at rest on the hopper mouth, it does not fall in for nobody
            rec.state = Rec::Sink;
            rec.y = kRimY + 1.0f;
            rec.vy = rec.squash = rec.depth = 0.0f;
            rec.t = 0.5f;
        }
        if (firstFrame || layers.ps <= 0.0f)
        {
            last.crankAngle = crankAngle();
            last.crankShown = crankShown;
            firstFrame = layers.ps <= 0.0f;
            out.push_back ({ 0, 0, kWidth, kHeight });
            return out;
        }
        // knobs and their tags
        for (int i = 0; i < 8; ++i)
        {
            const float n = knobs[(size_t) i]->norm(), f = tags[(size_t) i].f;
            if (n != last.norm[i])
                out.push_back (padded (knobRect (i)));
            if (f != last.tagF[i] || (f > 0.0f && n != last.norm[i]))
                out.push_back (padded (tagRect (i)));
            last.norm[i] = n;
            last.tagF[i] = f;
        }
        // the switches (cached image: the key says whether it was redrawn)
        const auto key = switchKeyNow();
        if (key != last.swKey) { out.push_back (padded (switchArea())); last.swKey = key; }
        // the seal, on hover only
        const bool sealNow = sealOver || forcedSeal;
        if (sealNow != last.seal) { out.push_back (padded (kSealRect)); last.seal = sealNow; }
        // the record and the hopper
        const bool recVisible = rec.state != Rec::Gone;
        if (recVisible || last.recVisible) out.push_back (padded (recordRect()));
        last.recVisible = recVisible;
        // the pour: strands, chunks, the pile
        const int nPieces = (int) std::lround (pile * (float) kPieces), nBig = (int) std::lround (pile * (float) bigPieces.size());
        const bool pileMoved = std::abs (pile - last.pile) > 1.0e-4f || nPieces != last.nPieces || nBig != last.nBig;
        const bool pourNow = activeStrands() > 0 || ! chunks.empty();
        if (pourNow || last.pour || pileMoved || (int) settled.size() != last.settledN) out.push_back (padded (pourRect()));
        last.pour = pourNow;
        last.settledN = (int) settled.size();
        last.pile = pile; last.nPieces = nPieces; last.nBig = nBig;
        // the crank, only when its angle moved by more than 0.2 degrees, or its look changed
        {
            const float a = crankAngle();
            const bool moving = std::abs (a - last.crankAngle) > 1.0e-6f;
            // a turning crank (not dragged) is redrawn at 20 Hz: two ticks of every three; everything else stays at 30 Hz
            const bool skipTurn = spinSpeed > 0.0f && ! dragging && (++crankTick % 3) == 0;
            const bool changed = ! skipTurn && (std::abs (a - last.crankAngle) > juce::degreesToRadians (0.2f) || dragging != last.crankDragging || (spinSpeed > 0.4f) != last.crankBlur);
            if (changed)
            {
                crankTiles (last.crankAngle, last.crankBlur, last.crankDragging, last.crankShown, out);
                crankTiles (a, spinSpeed > 0.4f, dragging, crankShown, out);
                last.crankAngle = a;
                last.crankDragging = dragging;
                last.crankBlur = spinSpeed > 0.4f;
                last.crankShown = crankShown;
            }
            crankMoving = moving && (spinSpeed > 0.0f || dragging);
        }

        // idle: quiet for 500 ms, and nothing is moving or being touched
        const bool quiet = ! fakeAudio && proc.uiOutputRms.load (std::memory_order_relaxed) < kQuietRms && proc.uiInputRms.load (std::memory_order_relaxed) < kQuietRms;
        if (! quiet) quietSince = now;
        if (quiet && ! dragging)   // in silence the knob-following values just arrive: nothing is left to animate
        {
            gl = param (kiymaIds::glitch); br = param (kiymaIds::brutal); mx = param (kiymaIds::mix);
            crankShown = param (kiymaIds::crank);
        }
        bool busy = (rec.state == Rec::Fall && (fakeAudio || ! quiet)) || dragging || swOver >= 0 || forcedHover >= 0 || forcedSeal || sealOver || activity > 0.002f || outActivity > 0.002f || spinSpeed > 0.0f
                    || std::abs (gl - param (kiymaIds::glitch)) > 1.0e-3f || std::abs (br - param (kiymaIds::brutal)) > 1.0e-3f || std::abs (mx - param (kiymaIds::mix)) > 1.0e-3f
                    || std::abs (crankShown - param (kiymaIds::crank)) > 1.0e-3f || ! chunks.empty() || paramWatch.changed.load (std::memory_order_relaxed);
        for (const auto& w : sw) busy = busy || w.t0 >= 0.0f;
        for (int i = 0; i < 8 && ! busy; ++i) busy = tags[(size_t) i].f > 0.0f || tags[(size_t) i].hold > 0.0f || knobs[(size_t) i]->active();
        idle = quiet && ! busy && now - quietSince >= 0.48;   // 500 ms of quiet, counted to the tick
        return out;
    }

    struct LastFrame
    {
        float norm[8] {}, tagF[8] {};
        uint64_t swKey = 0;
        bool seal = false, recVisible = false, pour = false, crankDragging = false, crankBlur = false;
        int settledN = 0, nPieces = 0, nBig = 0;
        float pile = 0.0f;
        float crankAngle = 0.0f, crankShown = 0.0f;
    } last;

    /** For ui_snapshot --bench-live: the real tick loop at the 30 Hz cap with dirty regions, `frames` ticks, painted at a pixel scale. */
    struct LiveStats { double meanMs = 0, p95Ms = 0, paintedPerSecond = 0, sharePercent = 0, totalMs = 0; int painted = 0, paintedAfter05 = 0, paintedAfter15 = 0, frames = 0; };
    /** Milliseconds of CPU time of this thread: a busy machine does not inflate it the way wall time is. */
    static double cpuMs()
    {
       #if JUCE_WINDOWS
        return juce::Time::getMillisecondCounterHiRes();   // no per-thread CPU clock here without <windows.h>; wall time is enough for the bench
       #else
        timespec ts {};
        clock_gettime (CLOCK_THREAD_CPUTIME_ID, &ts);
        return (double) ts.tv_sec * 1000.0 + (double) ts.tv_nsec * 1.0e-6;
       #endif
    }
    LiveStats benchLive (float scale, int frames, bool silent)
    {
        fakeAudio = ! silent;
        ticker.stopTimer();
        Image img (Image::ARGB, juce::roundToInt ((float) kWidth * scale), juce::roundToInt ((float) kHeight * scale), true);
        layers.ps = 0.0f;
        {   // build the layers outside the measurement
            Graphics g (img);
            g.addTransform (AffineTransform::scale (scale));
            paintEntireComponent (g, false);
        }
        firstFrame = false;
        LiveStats st;
        std::vector<double> ms;
        double now = 1000.0, total = 0.0;
        lastTick = 0.0;
        quietSince = now;
        for (int f = 0; f < frames; ++f)
        {
            now += 1.0 / 30.0;
            const double t0 = cpuMs();
            if (! idle)
            {
                const auto rects = frame (now);
                if (! rects.empty())
                {
                    juce::RectangleList<int> list;
                    for (const auto& r : rects) list.add (r);
                    Graphics g (img);
                    g.addTransform (AffineTransform::scale (scale));
                    g.reduceClipRegion (list);
                    paint (g);
                }
                const double d = cpuMs() - t0;
                total += d;
                if (! rects.empty())
                {
                    ms.push_back (d);
                    ++st.painted;
                    if (f >= 15) ++st.paintedAfter05;
                    if (f >= 45) ++st.paintedAfter15;
                }
            }
            else
                total += cpuMs() - t0;
        }
        std::sort (ms.begin(), ms.end());
        for (double m : ms) st.meanMs += m / (double) std::max<size_t> (1, ms.size());
        st.p95Ms = ms.empty() ? 0.0 : ms[(size_t) ((double) ms.size() * 0.95)];
        const double seconds = (double) frames / 30.0;
        st.paintedPerSecond = (double) st.painted / seconds;
        st.sharePercent = 100.0 * total / (seconds * 1000.0);
        st.totalMs = total;
        st.frames = frames;
        return st;
    }

    void advance (double seconds)
    {
        const int frames = (int) std::lround (seconds * 60.0);
        for (int i = 0; i < frames; ++i)
            step (1.0f / 60.0f);
    }

    void setHover (const juce::String& id)
    {
        forcedSeal = id == "seal";
        forcedHover = -1;
        for (int i = 0; i < 8; ++i)
            if (id == kKnobs[i].id)
                forcedHover = i;
    }

    //--------------------------------------------------------------------------
    void paint (Graphics& g) override
    {
        ensureLayers (g.getInternalContext().getPhysicalPixelScaleFactor());
        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
        auto hit = [&] (Rectangle<float> r) { return g.clipRegionIntersects (r.getSmallestIntegerContainer()); };   // only the dirty region: whatever lies outside it is not even computed
        g.setOpacity (1.0f);   // drawImage takes its opacity from the current colour's alpha
        g.drawImage (layers.back, 0, 0, kWidth, kHeight, 0, 0, layers.back.getWidth(), layers.back.getHeight());
        paintKnobs (g);
        paintTagText (g);
        if (hit (switchArea()))
            paintSwitches (g);
        if (hit (recordRect()))
            paintRecord (g);
        g.setOpacity (1.0f);
        g.drawImage (layers.front, 0, 0, kWidth, kHeight, 0, 0, layers.front.getWidth(), layers.front.getHeight());   // body and seal
        if (! kSealRect.isEmpty() && (sealOver || forcedSeal))
        {
            g.setColour (juce::Colours::white.withAlpha (0.06f));
            g.fillEllipse (kSealRect);
        }
        if (hit (pourRect()))
        {
            paintPile (g);
            paintStrands (g);
            paintChunks (g);
        }
        {
            std::vector<Rectangle<int>> tiles;
            crankTiles (crankDrawAngle(), spinSpeed > 0.4f, dragging, crankShown, tiles);
            for (const auto& r : tiles)
                if (g.clipRegionIntersects (r)) { paintCrank (g); break; }
        }
    }

    /** For ui_snapshot --bench: milliseconds per frame (animation step + paint) at a pixel scale. */
    void bench (float scale, int frames, double& meanMs, double& p95Ms, double& buildMs)
    {
        Image img (Image::ARGB, juce::roundToInt ((float) kWidth * scale), juce::roundToInt ((float) kHeight * scale), true);
        Graphics g (img);
        g.addTransform (AffineTransform::scale (scale));
        layers.ps = 0.0f;
        std::vector<double> ms;
        for (int f = -3; f < frames; ++f)
        {
            const double t0 = juce::Time::getMillisecondCounterHiRes();
            step (1.0f / 60.0f);
            paintEntireComponent (g, false);
            const double dt = juce::Time::getMillisecondCounterHiRes() - t0;
            if (f >= 0)
                ms.push_back (dt);
        }
        buildMs = layerBuildMs;
        std::sort (ms.begin(), ms.end());
        meanMs = 0.0;
        for (double m : ms) meanMs += m / (double) ms.size();
        p95Ms = ms[(size_t) ((double) ms.size() * 0.95)];
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.getPosition().toFloat().getDistanceFrom (kPivot) > 195.0f)
            return;
        if (e.getNumberOfClicks() >= 2)
        {
            crankAtt->beginGesture();
            crankAtt->setValueAsPartOfGesture (0.0f);
            crankAtt->endGesture();
            return;
        }
        dragging = true;
        dragValue = crankShown;
        lastMouseAngle = angleAt (e.position);
        crankAtt->beginGesture();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragging)
            return;
        const float a = angleAt (e.position);
        float d = a - lastMouseAngle;
        while (d > kPi) d -= kTwoPi;
        while (d < -kPi) d += kTwoPi;
        lastMouseAngle = a;
        dragValue = juce::jlimit (0.0f, 1.0f, dragValue + d / kTwoPi);   // one turn = the whole range, anticlockwise lowers it
        crankShown = dragValue;
        crankAtt->setValueAsPartOfGesture (dragValue);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging)
        {
            dragging = false;
            crankAtt->endGesture();
        }
    }

private:
    static constexpr int kWidth = KiymaEditor::kWidth, kHeight = KiymaEditor::kHeight;
    static constexpr int kStrands = 26, kPieces = 640, kMaxChunks = 140, kWave = 512;

    //--------------------------------------------------------------------------
    /** An invisible hit area for one cast-iron knob; the Stage draws it, so the draw order stays the spec's. */
    class KnobComp : public juce::Component
    {
    public:
        KnobComp (Stage& s, int i)
        {
            param = s.proc.apvts.getParameter (kKnobs[i].id);
            att = std::make_unique<juce::ParameterAttachment> (*param, [] (float) {});
            setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
            setRepaintsOnMouseActivity (false);
            setTitle (kKnobs[i].tag);
        }

        bool hitTest (int x, int y) override { return Point<float> ((float) x, (float) y).getDistanceFrom ({ 36.0f, 36.0f }) <= 36.0f; }
        float norm() const { return param->getValue(); }
        bool active() const { return over || dragging; }

        void mouseEnter (const juce::MouseEvent&) override { over = true; }
        void mouseExit (const juce::MouseEvent&) override { over = false; }

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (e.getNumberOfClicks() >= 2)
            {
                att->beginGesture();
                att->setValueAsPartOfGesture (param->convertFrom0to1 (param->getDefaultValue()));
                att->endGesture();
                return;
            }
            dragging = true;
            drag.begin (norm(), e.position.y);
            lastShift = e.mods.isShiftDown();
            att->beginGesture();
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (! dragging)
                return;
            if (e.mods.isShiftDown() != lastShift)   // fine / coarse switched in mid-drag: carry on from where the knob is
            {
                lastShift = e.mods.isShiftDown();
                drag.begin (norm(), e.position.y);
            }
            set (drag.at (e.position.y, lastShift));   // absolute: from where the drag began, so a stepped parameter can move too
        }

        void mouseUp (const juce::MouseEvent&) override
        {
            if (dragging)
            {
                dragging = false;
                att->endGesture();
            }
        }

        void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
        {
            if (dragging)
                return;
            const int steps = param->getNumSteps();
            att->beginGesture();
            if (kiyma::isStepped (steps))
            {
                const int dir = wheel.onWheel (w.deltaY, w.isReversed, juce::Time::getMillisecondCounter());   // one step per event, 120 ms apart at the fastest
                if (dir != 0)
                    set (((float) juce::roundToInt (norm() * (float) (steps - 1)) + (float) dir) / (float) (steps - 1));
            }
            else
                set (norm() + (w.isReversed ? -1.0f : 1.0f) * w.deltaY * (e.mods.isShiftDown() ? 0.02f : 0.1f));
            att->endGesture();
            wheelHold = true;
        }

        bool wheelHold = false;

    private:
        void set (float n) { att->setValueAsPartOfGesture (param->convertFrom0to1 (juce::jlimit (0.0f, 1.0f, n))); }

        juce::RangedAudioParameter* param = nullptr;
        std::unique_ptr<juce::ParameterAttachment> att;
        bool over = false, dragging = false, lastShift = false;
        kiyma::KnobDrag drag;
        kiyma::WheelStep wheel;
        friend class Stage;
    };

    /** The seal on the hopper: a hand cursor, a slight brighten, a tooltip, and a click that opens the SubverseLab site. */
    class SealHit : public juce::Component, public juce::SettableTooltipClient
    {
    public:
        explicit SealHit (Stage& s) : stage (s)
        {
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
            setTooltip (juce::String::fromUTF8 ("SubverseLab \xc2\xb7 BabaSultan Edition"));
            setTitle ("SubverseLab");
        }
        bool hitTest (int x, int y) override { const float r = 0.5f * (float) getWidth(); return Point<float> ((float) x, (float) y).getDistanceFrom ({ r, r }) <= r; }
        void mouseEnter (const juce::MouseEvent&) override { stage.sealOver = true; }
        void mouseExit (const juce::MouseEvent&) override { stage.sealOver = false; }
        void mouseUp (const juce::MouseEvent& e) override
        {
            if (e.mouseWasClicked() && hitTest (e.x, e.y))
                juce::URL ("https://subverselab.com").launchInDefaultBrowser();
        }
    private:
        Stage& stage;
    };

    /** One lever switch's hit area: click raises it (recall) or drops it (no preset). */
    class SwHit : public juce::Component
    {
    public:
        SwHit (Stage& s, int i) : stage (s), index (i)
        {
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
            setTitle (kiyma::kPresets[i].name);
        }
        void mouseEnter (const juce::MouseEvent&) override { stage.swOver = index; }
        void mouseExit (const juce::MouseEvent&) override { if (stage.swOver == index) stage.swOver = -1; }
        void mouseDown (const juce::MouseEvent&) override
        {
            if (stage.proc.getActivePreset() == index)
                stage.proc.clearPreset();
            else
                stage.proc.recallPreset (index);
        }
    private:
        Stage& stage;
        int index;
    };

    struct Rec { enum State { Fall, Sink, Gone } state = Fall; float y = -200.0f, vy = 0.0f, spin = 0.0f, depth = 0.0f, t = 0.0f, squash = 0.0f, wait = 0.0f, dx = 0.0f, freeze = 0.0f; };
    struct Strand { float y0, xEnd, c1x, c2y, width, idx0, speckle; int colour; float uEnd = 1.0f, fixedW = 0.0f; };
    struct Chunk { float x, y, vx, vy, w, h, rot, vr; Colour colour; };
    struct Piece { float x, y, w, h, rot; Colour colour; };
    struct TagState { float f = 0.0f, hold = 0.0f; };

    float param (const char* id) const { return proc.apvts.getRawParameterValue (id)->load(); }
    static float angleAt (Point<float> p) { return std::atan2 (p.y - kPivot.y, p.x - kPivot.x); }

    //--------------------------------------------------------------------------
    static Image loadPng (const void* d, int n) { return juce::ImageFileFormat::loadFrom (d, (size_t) n); }
    using Image = juce::Image;

    static void blurImage (Image& img, float radius)
    {
        juce::ImageConvolutionKernel k (1 + 2 * (int) std::ceil (radius * 1.5f));
        k.createGaussianBlur (radius);
        k.applyToImage (img, img, img.getBounds());
    }

    void loadAssets()
    {
        bodyImg = loadPng (BinaryData::body2x_png, BinaryData::body2x_pngSize);
        crankImg = loadPng (BinaryData::crank2x_png, BinaryData::crank2x_pngSize);
        knobImg = loadPng (BinaryData::knob2x_png, BinaryData::knob2x_pngSize);
        plinthImg = loadPng (BinaryData::plinth2x_png, BinaryData::plinth2x_pngSize);
        boardImg = loadPng (BinaryData::backdrop2x_jpg, BinaryData::backdrop2x_jpgSize);
        sealImg = loadPng (BinaryData::seal2x_png, BinaryData::seal2x_pngSize);
        bImg = loadPng (BinaryData::babasultan_B2x_png, BinaryData::babasultan_B2x_pngSize);

        grinderShadow = Image (Image::ARGB, 2 * 380, 2 * 80, true);
        {
            Graphics sh (grinderShadow);
            sh.addTransform (AffineTransform::scale (2.0f));
            sh.setColour (juce::Colours::black.withAlpha (0.66f));
            sh.fillEllipse (30.0f, 20.0f, 320.0f, 20.0f);
        }
        blurImage (grinderShadow, 11.0f);

        {   // the switch panel's drop shadow on the board, same recipe as the seal's, on a canvas 60 px bigger than the panel
            const auto& r = kBankRect;
            bankShadow = Image (Image::ARGB, juce::roundToInt (r.getWidth() + 60.0f), juce::roundToInt (r.getHeight() + 60.0f), true);   // 1x is plenty for a soft shadow
            Graphics s2 (bankShadow);
            s2.setColour (juce::Colours::black.withAlpha (0.55f));
            s2.fillRoundedRectangle (34.0f, 37.0f, r.getWidth(), r.getHeight(), 5.0f);
            blurImage (bankShadow, 9.0f);
        }
        {   // the seal's drop shadow on the board: offset (+4, +7), blur 9, 55 % black, on a canvas 60 px bigger than the seal
            const float cv = kSealRect.getWidth() + 60.0f;
            sealShadow = Image (Image::ARGB, juce::jmax (2, juce::roundToInt (2.0f * cv)), juce::jmax (2, juce::roundToInt (2.0f * cv)), true);
            Graphics sh (sealShadow);
            sh.addTransform (AffineTransform::scale (2.0f));
            sh.setColour (juce::Colours::black.withAlpha (0.55f));
            sh.fillEllipse (Rectangle<float> (kSealRect.getWidth(), kSealRect.getWidth()).withCentre ({ cv * 0.5f + 4.0f, cv * 0.5f + 7.0f }));
            blurImage (sealShadow, 18.0f);
        }
    }

    /** Everything that never moves, rendered once per pixel scale (on resize or a display change): the back layer is backdrop, light,
        plinth, panel paint, tag plates and the grinder's contact shadow; the front layer is the body and the seal. */
    void ensureLayers (float ps)
    {
        ps = std::max (1.0f, std::round (ps * 4.0f) / 4.0f);
        if (std::abs (ps - layers.ps) < 0.01f)
            return;
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        layers.ps = ps;
        const int w = juce::roundToInt ((float) kWidth * ps), h = juce::roundToInt ((float) kHeight * ps);
        layers.back = Image (Image::ARGB, w, h, true);
        {
            Graphics g (layers.back);
            g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
            g.addTransform (AffineTransform::scale ((float) w / (float) kWidth, (float) h / (float) kHeight));
            paintBackLayer (g);
        }
        layers.front = Image (Image::ARGB, w, h, true);
        {
            Graphics g (layers.front);
            g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
            g.addTransform (AffineTransform::scale ((float) w / (float) kWidth, (float) h / (float) kHeight));
            paintFrontLayer (g);
        }
        for (int i = 0; i < 8; ++i)
        {
            tagBare[(size_t) i] = Image (Image::ARGB, juce::roundToInt (80.0f * ps), juce::roundToInt (18.0f * ps), true);
            Graphics g (tagBare[(size_t) i]);
            g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
            g.addTransform (AffineTransform::translation (-(kKnobs[i].x - 40.0f), -668.0f).scaled (ps));
            paintTagPlate (g, kKnobs[i].x, i, false, false);
        }
        recordBase = Image (Image::ARGB, juce::roundToInt (200.0f * ps), juce::roundToInt (200.0f * ps), true);
        {
            Graphics g (recordBase);
            g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
            g.addTransform (AffineTransform::scale (ps));
            paintRecordFace (g, 100.0f, 100.0f);
        }
        crankPx = crankImg.rescaled (juce::roundToInt ((float) crankImg.getWidth() * 0.5f * kBodyScale * ps), juce::roundToInt ((float) crankImg.getHeight() * 0.5f * kBodyScale * ps), juce::Graphics::highResamplingQuality);
        knobPx = knobImg.rescaled (juce::roundToInt (72.0f * ps), juce::roundToInt (72.0f * ps), juce::Graphics::highResamplingQuality);
        knobLight = Image (Image::ARGB, juce::roundToInt (72.0f * ps), juce::roundToInt (72.0f * ps), true);
        {
            Graphics g (knobLight);
            g.addTransform (AffineTransform::scale (ps));
            Path disc;
            disc.addEllipse (Rectangle<float> (68.0f, 68.0f).withCentre ({ 36.0f, 36.0f }));
            g.reduceClipRegion (disc);
            g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.10f), 20.0f, 20.0f, juce::Colours::white.withAlpha (0.0f), 50.0f, 50.0f, true));
            g.fillEllipse (Rectangle<float> (72.0f, 72.0f));
            g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.25f), 56.0f, 56.0f, juce::Colours::black.withAlpha (0.0f), 32.0f, 32.0f, true));
            g.fillEllipse (Rectangle<float> (72.0f, 72.0f));
        }
        fineDrawn = bigDrawn = 0; pileLayerPs = 0.0f;
        layerBuildMs = juce::Time::getMillisecondCounterHiRes() - t0;
    }

    void paintBackLayer (Graphics& g)
    {
        g.drawImage (boardImg, 0, 0, kWidth, kHeight, 0, 0, boardImg.getWidth(), boardImg.getHeight());
        g.setColour (Theme::backdrop.withAlpha (0.35f));
        g.fillAll();
        g.setGradientFill (juce::ColourGradient (Colour (0xffc98a46).withAlpha (0.22f), 600.0f, 300.0f, Colour (0xffc98a46).withAlpha (0.0f), 600.0f + 480.0f, 300.0f, true));
        g.fillAll();
        g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.0f), 600.0f, 360.0f, juce::Colours::black.withAlpha (0.82f), 600.0f + 840.0f, 360.0f, true));
        g.fillAll();
        g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.0f), 0.0f, 600.0f, juce::Colours::black.withAlpha (0.5f), 0.0f, 800.0f, false));   // the bottom quarter is darker
        g.fillRect (0, 600, kWidth, 200);
        {   // the plinth's wide contact shadow, mostly below the window
            g.saveState();
            g.addTransform (AffineTransform::scale (1.0f, 0.1f, 600.0f, 800.0f));
            g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.75f), 600.0f, 800.0f, juce::Colours::black.withAlpha (0.0f), 600.0f + 760.0f, 800.0f, true));
            g.fillEllipse (Rectangle<float> (1520.0f, 1520.0f).withCentre ({ 600.0f, 800.0f }));
            g.restoreState();
        }
        paintCables (g);   // the cables run behind the iron plinth, so they are drawn before it
        g.setOpacity (1.0f);
        g.drawImage (plinthImg, kPlinthX, kPlinthY, 1181.0f, 330.0f, 0, 0, plinthImg.getWidth(), plinthImg.getHeight());
        paintPanelPaint (g);
        paintBankPanel (g);
        g.setOpacity (1.0f);
        g.drawImage (grinderShadow, 444.0f - 30.0f, 509.0f - 20.0f, (float) grinderShadow.getWidth() / 2.0f, (float) grinderShadow.getHeight() / 2.0f, 0, 0, grinderShadow.getWidth(), grinderShadow.getHeight());
    }

    /** A hex bolt seen from slightly above (everything squashed to about 0.42 vertically): contact shadow, washer, a dark gap ring, three side facets, the top face with a gradient and a glint, a few rust specks. */
    static void footBolt (Graphics& g, Point<float> c, float k, bool withWasher)
    {
        const float sq = 0.42f;
        auto flat = [&] (float rx, float ry, Point<float> at) { return Rectangle<float> (2.0f * rx, 2.0f * ry).withCentre (at); };
        if (withWasher)
        {
            g.setColour (juce::Colours::black.withAlpha (0.5f));
            g.fillEllipse (flat (11.0f * k + 0.8f, 11.0f * k * sq + 0.5f, c + Point<float> (1.4f, 1.8f)));   // contact shadow, down and right
            g.setGradientFill (juce::ColourGradient (Colour (0xffb3b2ad), c.x - 8.0f * k, c.y - 3.0f * k, Colour (0xff4a4845), c.x + 9.0f * k, c.y + 3.0f * k, false));
            g.fillEllipse (flat (11.0f * k, 11.0f * k * sq, c));
            g.setColour (Colour (0xff171615));
            g.drawEllipse (flat (11.0f * k, 11.0f * k * sq, c), 1.0f);   // the dark rim of the worn steel washer
        }
        else
        {
            g.setColour (juce::Colours::black.withAlpha (0.45f));
            g.fillEllipse (flat (8.5f * k, 8.5f * k * sq, c + Point<float> (1.3f, 1.7f)));
        }
        const float R = 7.0f * k / 0.866f;                  // 14 px across the flats = 16.2 px corner to corner
        const float headH = 2.6f * k;
        g.setColour (Colour (0xff0b0a09));                  // a 1 px dark gap ring between washer and head
        g.drawEllipse (flat (R + 1.2f, (R + 1.2f) * sq, c + Point<float> (0.0f, headH * 0.5f)), 1.0f);
        Point<float> top[6], bot[6];
        for (int i = 0; i < 6; ++i)
        {
            const float a = kPi / 3.0f * (float) i;
            top[i] = { c.x + std::cos (a) * R, c.y - headH * 0.5f + std::sin (a) * R * sq };
            bot[i] = top[i] + Point<float> (0.0f, headH);
        }
        for (int i = 0; i < 3; ++i)   // the three visible side facets (the lower edges), darker, the left one lit a little more
        {
            Path f;
            f.startNewSubPath (top[i]); f.lineTo (top[i + 1]); f.lineTo (bot[i + 1]); f.lineTo (bot[i]); f.closeSubPath();
            g.setColour (Colour (i == 2 ? 0xff5c5955u : i == 1 ? 0xff3a3835u : 0xff23211fu));
            g.fillPath (f);
            g.setColour (Colour (0xff0e0d0c).withAlpha (0.8f));
            g.strokePath (f, juce::PathStrokeType (0.6f));
        }
        Path face;
        for (int i = 0; i < 6; ++i)
            i == 0 ? face.startNewSubPath (top[i]) : face.lineTo (top[i]);
        face.closeSubPath();
        g.setGradientFill (juce::ColourGradient (Colour (0xffd2d0c9), c.x - R * 0.7f, c.y - headH - R * sq * 0.8f, Colour (0xff3a3835), c.x + R * 0.8f, c.y + R * sq, false));
        g.fillPath (face);
        g.setColour (Colour (0xff121110).withAlpha (0.85f));
        g.strokePath (face, juce::PathStrokeType (0.7f));
        g.setColour (juce::Colours::white.withAlpha (0.8f));   // a small specular glint at the top left
        g.fillEllipse (flat (1.6f * k, 0.8f * k, { c.x - R * 0.45f, c.y - headH * 0.5f - R * sq * 0.45f }));
        Rand rn ((uint32_t) (c.x * 7.0f + c.y));
        for (int i = 0; i < 5; ++i)   // copper rust speckle
        {
            g.setColour (Theme::copper.withAlpha (rn.range (0.35f, 0.7f)));
            g.fillEllipse (flat (rn.range (0.4f, 0.8f), rn.range (0.3f, 0.5f), { c.x + rn.range (-R * 0.7f, R * 0.7f), c.y - headH * 0.5f + rn.range (-R * sq * 0.6f, R * sq * 0.6f) }));
        }
    }

    void paintFrontLayer (Graphics& g)
    {
        g.setOpacity (1.0f);
        g.drawImage (bodyImg, kBodyX, kBodyY, 449.5f * kBodyScale, 556.5f * kBodyScale, 0, 0, bodyImg.getWidth(), bodyImg.getHeight());
        // the grinder is bolted to the plate: a washer and a hex bolt head in each foot pad's oval recess
        for (const auto& c : { kFootL, kFootR })
            footBolt (g, c, 1.0f, true);
        if (kSealRect.isEmpty())
            return;
        // the seal: contact shadow, the medallion as rendered (not redrawn), and a still, faint sheen at the top left
        const auto c = kSealRect.getCentre();
        const float f = kSealRect.getWidth() / 104.0f;   // the sheen was designed for a 104 px seal
        const float cvs = kSealRect.getWidth() + 60.0f;
        g.setOpacity (1.0f);   // drawImage takes its opacity from the current colour's alpha: footBolt above left a translucent one
        g.drawImage (sealShadow, c.x - cvs * 0.5f, c.y - cvs * 0.5f, cvs, cvs, 0, 0, sealShadow.getWidth(), sealShadow.getHeight());
        g.drawImage (sealImg, kSealRect.getX(), kSealRect.getY(), kSealRect.getWidth(), kSealRect.getHeight(), 0, 0, sealImg.getWidth(), sealImg.getHeight());
        g.saveState();
        Path disc;
        disc.addEllipse (kSealRect);
        g.reduceClipRegion (disc);
        const Point<float> sheen { c.x - 18.0f * f, c.y - 20.0f * f };
        g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.06f), sheen.x, sheen.y, juce::Colours::white.withAlpha (0.0f), sheen.x + 38.0f * f, sheen.y + 18.0f * f, true));
        g.fillEllipse (Rectangle<float> (76.0f * f, 44.0f * f).withCentre (sheen));
        g.restoreState();
        {   // a faint warm rim light on the upper-left edge, from the light pool (the medallion's rim is at about 0.94 of the artwork's half size)
            const float rr = kSealRect.getWidth() * 0.5f * 0.945f;
            for (const auto& arcSpec : { std::tuple<float, float, float, float> { rr - 1.0f, 262.0f, 340.0f, 0.30f }, { rr - 3.0f, 275.0f, 328.0f, 0.16f } })
            {
                Path arc;
                arc.addCentredArc (c.x, c.y, std::get<0> (arcSpec), std::get<0> (arcSpec), 0.0f, juce::degreesToRadians (std::get<1> (arcSpec)), juce::degreesToRadians (std::get<2> (arcSpec)), true);
                g.setColour (Colour (0xffffcf8a).withAlpha (std::get<3> (arcSpec)));
                g.strokePath (arc, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }
        }
        {   // four slotted brass screw heads (r 4) at 45, 135, 225 and 315 degrees on the rim, radius 108, each slot at a fixed random angle
            Rand rn (1923u);
            for (int k = 0; k < 4; ++k)
            {
                const float a = juce::degreesToRadians (45.0f + 90.0f * (float) k), rad = 108.0f * kSealRect.getWidth() / 232.0f;
                const Point<float> sc { c.x + std::cos (a) * rad, c.y + std::sin (a) * rad };
                g.setColour (juce::Colours::black.withAlpha (0.5f));
                g.fillEllipse (Rectangle<float> (8.0f, 8.0f).withCentre (sc + Point<float> (1.2f, 1.8f)));
                g.setGradientFill (juce::ColourGradient (Colour (0xffeacc86), sc.x - 1.6f, sc.y - 1.8f, Colour (0xff6b4a1c), sc.x + 3.0f, sc.y + 3.5f, true));
                g.fillEllipse (Rectangle<float> (8.0f, 8.0f).withCentre (sc));
                g.setColour (Colour (0xff46300f));
                g.drawEllipse (Rectangle<float> (8.0f, 8.0f).withCentre (sc), 0.8f);
                const float sa = rn.range (0.0f, kPi);
                g.setColour (Colour (0xff24180a));
                g.drawLine (sc.x - std::cos (sa) * 3.2f, sc.y - std::sin (sa) * 3.2f, sc.x + std::cos (sa) * 3.2f, sc.y + std::sin (sa) * 3.2f, 1.1f);
            }
        }
    }

    /** Gold dial ticks, knob recesses, the KIYMA decal, brass tags and the maker's plate, painted onto the panel. */
    void paintPanelPaint (Graphics& g)
    {
        for (int i = 0; i < 8; ++i)
        {
            const float kx = kKnobs[i].x;
            for (int t = 0; t <= 10; ++t)
            {
                const float a = juce::degreesToRadians (-135.0f + 27.0f * (float) t);
                const bool major = t == 0 || t == 5 || t == 10;
                const float r1 = 40.0f, r2 = major ? 45.0f : 43.0f;
                g.setColour (kGold.withAlpha (0.85f));
                g.drawLine (kx + r1 * std::sin (a), kPanelY - r1 * std::cos (a), kx + r2 * std::sin (a), kPanelY - r2 * std::cos (a), major ? 2.0f : 1.0f);
            }
            // recess shadow: a soft black ellipse offset (+1, +4), radius about 37
            const Point<float> c { kx + 1.0f, kPanelY + 4.0f };
            juce::ColourGradient sh (juce::Colours::black.withAlpha (0.75f), c.x, c.y, juce::Colours::black.withAlpha (0.0f), c.x + 42.0f, c.y, true);
            sh.addColour (0.62, juce::Colours::black.withAlpha (0.72f));
            g.setGradientFill (sh);
            g.fillEllipse (Rectangle<float> (84.0f, 84.0f).withCentre (c));
        }

        paintLogo (g);

        for (int i = 0; i < 8; ++i)
            paintTagPlate (g, kKnobs[i].x, i);

        paintMakersPlate (g);
    }

    void paintLogo (Graphics& g)
    {
        // The decal is built on its own layer so the wear can knock alpha out of the gold only.
        const int w = 360, h = 130;
        Image decal (Image::ARGB, 2 * w, 2 * h, true);
        {
            Graphics d (decal);
            d.addTransform (AffineTransform::scale (2.0f));
            juce::GlyphArrangement ga;
            ga.addLineOfText (Theme::display (58.0f), "KIYMA", 0.0f, 0.0f);
            Path text;
            ga.createPath (text);
            const auto b = text.getBounds();
            text.applyTransform (AffineTransform::translation ((float) w * 0.5f - b.getCentreX(), 4.0f - b.getY()));   // centred, top 4 px into the layer
            for (const auto& s : { std::tuple<float, Colour> { 3.0f, Colour (0xff140403) }, { 2.0f, Theme::oxblood }, { 1.0f, Theme::oxblood } })
                { d.setColour (std::get<1> (s)); d.fillPath (text, AffineTransform::translation (std::get<0> (s), std::get<0> (s))); }
            d.setColour (kGold);
            d.fillPath (text);
            d.setColour (Colour (0xff3c1e0a));
            d.strokePath (text, juce::PathStrokeType (1.0f));
        }
        {   // decal wear: small fixed-seed flecks knock 0..15 % alpha out of the gold
            juce::Image::BitmapData bd (decal, juce::Image::BitmapData::readWrite);
            Rand r (6502u);
            for (int n = 0; n < 700; ++n)
            {
                const int cx = (int) r.range (0.0f, (float) bd.width), cy = (int) r.range (0.0f, (float) bd.height);
                const float rad = r.range (0.8f, 2.6f), amount = r.range (0.0f, 0.15f);
                for (int y = juce::jmax (0, cy - 3); y < juce::jmin (bd.height, cy + 4); ++y)
                    for (int x = juce::jmax (0, cx - 3); x < juce::jmin (bd.width, cx + 4); ++x)
                        if (std::hypot ((float) (x - cx), (float) (y - cy)) <= rad)
                            bd.setPixelColour (x, y, bd.getPixelColour (x, y).withMultipliedAlpha (1.0f - amount));
            }
        }
        g.setOpacity (1.0f);
        g.drawImage (decal, 600.0f - (float) w * 0.5f, 585.0f - 4.0f, (float) w, (float) h, 0, 0, decal.getWidth(), decal.getHeight());

        g.setColour (kGold);
        g.setFont (Theme::mono (9.0f));
        g.drawText ("A U D I O   M I N C E R", Rectangle<float> (440.0f, 655.0f, 320.0f, 12.0f), juce::Justification::centred, false);
    }

    static void rivet (Graphics& g, float x, float y, float size = 6.0f)
    {
        const float k = size / 6.0f;
        g.setGradientFill (juce::ColourGradient (Colour (0xffe9c987), x - 1.0f * k, y - 1.2f * k, Colour (0xff6b4a1c), x + 1.8f * k, y + 2.0f * k, true));
        g.fillEllipse (Rectangle<float> (size, size).withCentre ({ x, y }));
        g.setColour (Colour (0xff46300f));
        g.drawEllipse (Rectangle<float> (size, size).withCentre ({ x, y }), 0.8f);
        g.setColour (juce::Colour (0xfffff0c8).withAlpha (0.9f));
        g.fillEllipse (Rectangle<float> (1.6f * k, 1.4f * k).withCentre ({ x - 0.9f * k, y - 1.1f * k }));   // the specular dot
    }

    static void engrave (Graphics& g, const juce::String& text, Rectangle<float> area, float alpha)
    {
        if (alpha <= 0.004f)
            return;
        g.setColour (Colour (0xfff5d78c).withAlpha (0.8f * alpha));   // the lighter duplicate, 0.6 px down: the engraving's bevel
        g.drawText (text, area.translated (0.0f, 0.6f), juce::Justification::centred, false);
        g.setColour (Colour (0xff301e0a).withAlpha (alpha));
        g.drawText (text, area, juce::Justification::centred, false);
    }

    void paintTagPlate (Graphics& g, float cx, int index, bool withShadow = true, bool withLabel = true)
    {
        const Rectangle<float> r (cx - 40.0f, 668.0f, 80.0f, 18.0f);
        if (withShadow)
            juce::DropShadow (juce::Colours::black.withAlpha (0.65f), 3, { 2, 2 }).drawForRectangle (g, r.toNearestInt());
        juce::ColourGradient grad (Colour (0xffb0863c), 0.0f, r.getY(), Colour (0xffb0863c), 0.0f, r.getBottom(), false);
        grad.addColour (0.35, Colour (0xffe2bd72));
        g.setGradientFill (grad);
        g.fillRect (r);
        Rand rn ((uint32_t) (index * 977 + 13));
        for (int i = 0; i < 90; ++i)   // patina speckle, fixed seed
        {
            g.setColour (Colour (0xff785520).withAlpha (0.15f));
            g.fillRect (r.getX() + rn.range (1.0f, 79.0f), r.getY() + rn.range (1.0f, 17.0f), rn.range (0.6f, 1.3f), rn.range (0.6f, 1.3f));
        }
        g.setColour (Colour (0xff5a3e16));
        g.drawRect (r, 1.0f);
        rivet (g, r.getX() + 6.0f, r.getCentreY());
        rivet (g, r.getRight() - 6.0f, r.getCentreY());
        if (withLabel)
        {
            g.setFont (Theme::mono (9.0f));
            engrave (g, kKnobs[index].tag, r.reduced (12.0f, 0.0f), 1.0f);   // the resting label is part of the static layer
        }
    }

    /** A riveted brass plate with an engraved label (the header of the switch bank and the tags under the levers). */
    void paintBrassPlate (Graphics& g, Rectangle<float> r, const juce::String& label, float fontH, uint32_t seed, float rivetInset, float rivetSize)
    {
        juce::DropShadow (juce::Colours::black.withAlpha (0.65f), 3, { 2, 2 }).drawForRectangle (g, r.toNearestInt());
        juce::ColourGradient grad (Colour (0xffb0863c), 0.0f, r.getY(), Colour (0xffb0863c), 0.0f, r.getBottom(), false);
        grad.addColour (0.35, Colour (0xffe2bd72));
        g.setGradientFill (grad);
        g.fillRect (r);
        Rand rn (seed);
        for (int i = 0; i < (int) (r.getWidth() * r.getHeight() / 16.0f); ++i)
        {
            g.setColour (Colour (0xff785520).withAlpha (0.15f));
            g.fillRect (r.getX() + rn.range (1.0f, r.getWidth() - 1.0f), r.getY() + rn.range (1.0f, r.getHeight() - 1.0f), rn.range (0.6f, 1.3f), rn.range (0.6f, 1.3f));
        }
        g.setColour (Colour (0xff5a3e16));
        g.drawRect (r, 1.0f);
        rivet (g, r.getX() + rivetInset, r.getCentreY(), rivetSize);
        rivet (g, r.getRight() - rivetInset, r.getCentreY(), rivetSize);
        g.setFont (Theme::mono (fontH));
        engrave (g, label, r.reduced (rivetInset + rivetSize * 0.5f + 2.0f, 0.0f), 1.0f);
    }

    static void hexBolt (Graphics& g, Point<float> c, float r)
    {
        g.setColour (juce::Colours::black.withAlpha (0.5f));
        Path sh;
        sh.addPolygon (c + Point<float> (1.2f, 1.8f), 6, r, 0.0f);
        g.fillPath (sh);
        Path hex;
        hex.addPolygon (c, 6, r, 0.0f);
        g.setGradientFill (juce::ColourGradient (Colour (0xff5a5651), c.x - r, c.y - r, Colour (0xff171615), c.x + r, c.y + r, false));
        g.fillPath (hex);
        g.setColour (Colour (0xffa59c90).withAlpha (0.65f));
        g.strokePath (hex, juce::PathStrokeType (0.8f));
        g.setColour (Colour (0xff0c0b0a));
        g.fillEllipse (Rectangle<float> (r * 0.8f, r * 0.8f).withCentre (c));   // the socket
    }

    /** The wall panel: face cut from the plinth's own panel texture (about 1:1, so its grain matches the main panel), a gold / red
        pinstripe frame inset 8 px, four hex bolts, a drop shadow on the board like the seal's and a warm rim light on the upper left. */
    void paintBankPanel (Graphics& g)
    {
        const auto r = kBankRect;
        g.setOpacity (1.0f);   // drop shadow on the board: offset (+4, +7), blur 9, 55 % (built once, in loadAssets)
        g.drawImage (bankShadow, r.getX() - 30.0f, r.getY() - 30.0f, r.getWidth() + 60.0f, r.getHeight() + 60.0f, 0, 0, bankShadow.getWidth(), bankShadow.getHeight());
        // face: the plinth's interior texture (2x section x 910.., rows 200..430 = 270 x 115 logical, chosen for the fewest chips), used twice,
        // the second copy mirrored top to bottom, so 232 px are covered with no stretching
        g.saveState();
        Path clip;
        clip.addRoundedRectangle (r, 5.0f);
        g.reduceClipRegion (clip);
        g.setOpacity (1.0f);
        g.drawImage (plinthImg, 900, 40, 270, 116, 910, 200, 540, 232);
        g.saveState();
        g.addTransform (AffineTransform::scale (1.0f, -1.0f, 0.0f, 156.0f));   // the same strip mirrored about y = 156, covering 156..272
        g.drawImage (plinthImg, 900, 40, 270, 116, 910, 200, 540, 232);
        g.restoreState();
        g.restoreState();
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.drawRoundedRectangle (r, 5.0f, 1.2f);
        // pinstripe frame inset 8 px: a thin gold line outside, a red stripe, a thin gold line inside (same weights as the main panel's)
        g.setColour (Colour (0xffc6a04a).withAlpha (0.9f));
        g.drawRect (r.reduced (8.0f), 1.3f);
        g.setColour (Colour (0xff8a1d15));
        g.drawRect (r.reduced (9.8f), 2.6f);
        g.setColour (Colour (0xffc6a04a).withAlpha (0.8f));
        g.drawRect (r.reduced (13.0f), 1.0f);
        // a warm rim light on the upper-left edge, like the seal's
        g.setColour (Colour (0xffffcf8a).withAlpha (0.30f));
        g.drawLine (r.getX() + 6.0f, r.getY() + 1.6f, r.getRight() - 60.0f, r.getY() + 1.6f, 1.4f);
        g.drawLine (r.getX() + 1.6f, r.getY() + 6.0f, r.getX() + 1.6f, r.getBottom() - 60.0f, 1.4f);
        for (const auto& corner : { Point<float> (r.getX() + 10.0f, r.getY() + 10.0f), Point<float> (r.getRight() - 10.0f, r.getY() + 10.0f),
                                    Point<float> (r.getX() + 10.0f, r.getBottom() - 10.0f), Point<float> (r.getRight() - 10.0f, r.getBottom() - 10.0f) })
            hexBolt (g, corner, 5.2f);

        gland (g, { 1010.0f, 272.0f });
        gland (g, { 1060.0f, 272.0f });
        paintBrassPlate (g, Rectangle<float> (150.0f, 18.0f).withCentre ({ 1035.0f, 61.0f }), "SWEET SPOTS", 9.0f, 31u, 8.0f, 4.5f);
        for (int i = 0; i < 5; ++i)
        {
            const float x = kSwX[i];
            // escutcheon: a round brass plate (r 17) with a dark hole and a fine ring line
            g.setColour (juce::Colours::black.withAlpha (0.5f));
            g.fillEllipse (Rectangle<float> (34.0f, 34.0f).withCentre ({ x + 2.0f, kSwPivotY + 3.5f }));
            g.setGradientFill (juce::ColourGradient (Colour (0xffeacc86), x - 8.0f, kSwPivotY - 9.0f, Colour (0xff7a5a22), x + 11.0f, kSwPivotY + 12.0f, true));
            g.fillEllipse (Rectangle<float> (34.0f, 34.0f).withCentre ({ x, kSwPivotY }));
            g.setColour (Colour (0xff4d3a12));
            g.drawEllipse (Rectangle<float> (34.0f, 34.0f).withCentre ({ x, kSwPivotY }), 1.0f);
            g.setColour (Colour (0xff4d3a12).withAlpha (0.6f));
            g.drawEllipse (Rectangle<float> (25.0f, 25.0f).withCentre ({ x, kSwPivotY }), 0.6f);
            g.setColour (Colour (0xff0c0b0a));
            g.fillEllipse (Rectangle<float> (12.0f, 12.0f).withCentre ({ x, kSwPivotY }));
            const juce::String label = kiyma::kPresets[i].name;
            paintBrassPlate (g, Rectangle<float> (46.0f, 16.0f).withCentre ({ x, kSwTagY }), label, label.length() > 6 ? 7.5f : 8.0f, (uint32_t) (700 + i), 3.4f, 2.6f);
        }
    }

    /** A brass gland (cable entry) on the panel's lower edge. */
    static void gland (Graphics& g, Point<float> c)
    {
        const float w = 18.0f, h = 11.0f;
        const Rectangle<float> body = Rectangle<float> (w, h).withCentre (c);
        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.fillRoundedRectangle (body.translated (1.5f, 2.0f), 2.5f);
        juce::ColourGradient gr (Colour (0xffe9c987), body.getX(), 0.0f, Colour (0xff7a5a22), body.getRight(), 0.0f, false);
        gr.addColour (0.4, Colour (0xfff2dc9a));
        g.setGradientFill (gr);
        g.fillRoundedRectangle (body, 2.5f);
        g.setColour (Colour (0xff46300f));
        g.drawRoundedRectangle (body, 2.5f, 0.8f);
        g.setColour (Colour (0xff46300f).withAlpha (0.7f));
        for (int k = -1; k <= 1; ++k)   // the ridges of the gland nut
            g.drawLine (c.x + (float) k * 5.0f, body.getY() + 1.5f, c.x + (float) k * 5.0f, body.getBottom() - 1.5f, 0.8f);
    }

    /** Two braided cloth-wrapped cables (oxblood and black twist, 7 px, a diagonal stripe every 6 px) from the panel's glands at (1010, 272) and
        (1060, 272), hanging in a soft curve down and slightly right and on to y 560: the plinth, drawn after them, hides the lower end, and a
        short darker gradient just above its back edge makes them read as going behind. Right of x 1000 everywhere. */
    void paintCables (Graphics& g)
    {
        struct Cable { Point<float> a, b; };
        const Cable cables[2] { { { 1010.0f, 272.0f }, { 1058.0f, 560.0f } }, { { 1060.0f, 272.0f }, { 1108.0f, 560.0f } } };
        for (const auto& cb : cables)
        {
            const Point<float> p0 = cb.a + Point<float> (0.0f, 6.0f), p3 = cb.b;
            const Point<float> p1 = p0 + Point<float> (-4.0f, 170.0f), p2 = p3 + Point<float> (-2.0f, -120.0f);
            auto at = [&] (float t) { const float v = 1.0f - t; return p0 * (v * v * v) + p1 * (3.0f * v * v * t) + p2 * (3.0f * v * t * t) + p3 * (t * t * t); };
            std::vector<Point<float>> pts;
            for (int i = 0; i <= 200; ++i) pts.push_back (at ((float) i / 200.0f));
            Path path;
            path.startNewSubPath (pts[0]);
            for (size_t i = 1; i < pts.size(); ++i) path.lineTo (pts[i]);
            // soft shadow on the board
            for (const auto& sh : { std::pair<float, float> { 11.0f, 0.10f }, std::pair<float, float> { 9.0f, 0.12f }, std::pair<float, float> { 7.5f, 0.16f } })
            {
                g.setColour (juce::Colours::black.withAlpha (sh.second));
                g.strokePath (path, juce::PathStrokeType (sh.first, juce::PathStrokeType::curved, juce::PathStrokeType::rounded), AffineTransform::translation (4.0f, 6.0f));
            }
            g.setColour (Colour (0xff0e0c0b));
            g.strokePath (path, juce::PathStrokeType (7.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            // braid: a diagonal oxblood stripe every 6 px along the length
            float travelled = 0.0f, nextStripe = 3.0f;
            for (size_t i = 1; i < pts.size(); ++i)
            {
                const Point<float> d = pts[i] - pts[i - 1];
                const float seg = std::sqrt (d.x * d.x + d.y * d.y);
                travelled += seg;
                if (travelled >= nextStripe)
                {
                    nextStripe += 6.0f;
                    const Point<float> t = d / std::max (1.0e-3f, seg), n { -t.y, t.x };
                    const Point<float> c = pts[i];
                    g.setColour (Colour (0xff7a1712));
                    g.drawLine (c.x - n.x * 3.2f - t.x * 2.0f, c.y - n.y * 3.2f - t.y * 2.0f, c.x + n.x * 3.2f + t.x * 2.0f, c.y + n.y * 3.2f + t.y * 2.0f, 2.4f);
                    g.setColour (Colour (0xff2a0a08).withAlpha (0.8f));   // the shadow side of each twist
                    g.drawLine (c.x - n.x * 3.2f + t.x * 1.2f, c.y - n.y * 3.2f + t.y * 1.2f, c.x + n.x * 3.2f + t.x * 3.2f, c.y + n.y * 3.2f + t.y * 3.2f, 0.9f);
                }
            }
            g.setColour (juce::Colours::white.withAlpha (0.16f));   // a soft highlight along the top-left of the cloth
            g.strokePath (path, juce::PathStrokeType (1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded), AffineTransform::translation (-1.6f, -0.4f));
            {   // occlusion: darker toward the plinth's back edge (its top surface starts at y 502), so the cable goes behind it rather than being cut off
                g.saveState();
                g.reduceClipRegion (Rectangle<int> (1000, 478, 140, 30));
                g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.0f), 0.0f, 480.0f, juce::Colours::black.withAlpha (0.78f), 0.0f, 504.0f, false));
                g.strokePath (path, juce::PathStrokeType (8.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                g.restoreState();
            }
        }
    }

    // ---- the lever switches (live) --------------------------------------------------------------
    /** Starts a flip: 90 ms with an 8 degree overshoot that settles in 60 ms. */
    void startFlip (int i, bool up)
    {
        auto& w = sw[(size_t) i];
        w.from = w.theta;
        w.to = up ? 0.0f : kPi;
        w.t0 = time;
        w.up = up;
        if (up)
            w.sparkT0 = time;   // sparks at the pivot of the switch that just went up: about 50 ms, fading
    }

    void stepSwitches (float dt)
    {
        juce::ignoreUnused (dt);
        const int ap = proc.getActivePreset();
        if (ap != shownPreset)
        {
            for (int i = 0; i < 5; ++i)
                if ((i == ap) != sw[(size_t) i].up)
                    startFlip (i, i == ap);   // the one that was up drops at the same moment: the interlock
            shownPreset = ap;
        }
        const float over = juce::degreesToRadians (kOvershootDeg);
        for (auto& w : sw)
        {
            if (w.t0 < 0.0f)
                continue;
            const float tau = time - w.t0;
            const float dir = w.to > w.from ? 1.0f : -1.0f, peak = w.to + dir * over;
            if (tau < kFlipSeconds)
            {
                const float x = tau / kFlipSeconds, e = x * x * (3.0f - 2.0f * x);
                w.theta = w.from + (peak - w.from) * e;
            }
            else if (tau < kFlipSeconds + kSettleSeconds)
            {
                const float x = (tau - kFlipSeconds) / kSettleSeconds, e = x * x * (3.0f - 2.0f * x);
                w.theta = peak + (w.to - peak) * e;
            }
            else
            {
                w.theta = w.to;
                w.t0 = -1.0f;
            }
        }
    }

    /** The switches change only when one flips, is hovered or sparks, so they are drawn into a cached image and redrawn only then. */
    void paintSwitches (Graphics& g)
    {
        const uint64_t key = switchKeyNow();
        const float ps = layers.ps;
        const Rectangle<float> area = switchArea();
        if (key != switchKey || ! switchImg.isValid() || std::abs (ps - switchPs) > 0.01f)
        {
            switchKey = key;
            switchPs = ps;
            switchImg = Image (Image::ARGB, juce::roundToInt (area.getWidth() * ps), juce::roundToInt (area.getHeight() * ps), true);
            Graphics gi (switchImg);
            gi.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
            gi.addTransform (AffineTransform::translation (-area.getX(), -area.getY()).scaled (ps));
            paintSwitchesInto (gi);
        }
        g.setOpacity (1.0f);
        g.drawImage (switchImg, area.getX(), area.getY(), area.getWidth(), area.getHeight(), 0, 0, switchImg.getWidth(), switchImg.getHeight());
    }

    void paintSwitchesInto (Graphics& g)
    {
        for (int i = 0; i < 5; ++i)
        {
            auto& w = sw[(size_t) i];
            const float x = kSwX[i], theta = w.theta, ct = std::cos (theta), st = std::sin (theta), ast = std::abs (st);
            const bool active = w.up;
            const bool hover = swOver == i;
            // pilot jewel (r 6) above the lever
            {
                const Point<float> jc { x, kSwJewelY };
                g.setColour (juce::Colours::black.withAlpha (0.45f));
                g.fillEllipse (Rectangle<float> (15.0f, 15.0f).withCentre (jc + Point<float> (1.2f, 1.8f)));   // its seat
                if (active)
                {
                    g.setGradientFill (juce::ColourGradient (Colour (0xffff3a1a).withAlpha (0.35f + (hover ? 0.08f : 0.0f)), jc.x, jc.y, Colour (0xffff3a1a).withAlpha (0.0f), jc.x + 16.0f, jc.y, true));
                    g.fillEllipse (Rectangle<float> (32.0f, 32.0f).withCentre (jc));
                }
                Path facet;
                facet.addPolygon (jc, 6, 6.0f, 0.5f);
                g.setGradientFill (active ? juce::ColourGradient (Colour (0xffffb09a), jc.x - 1.8f, jc.y - 2.1f, Colour (0xffff3a1a), jc.x + 3.8f, jc.y + 4.5f, true)
                                          : juce::ColourGradient (Colour (hover ? 0xff7a2a24u : 0xff5a1c18u), jc.x - 1.8f, jc.y - 2.1f, Colour (0xff1c0605), jc.x + 3.8f, jc.y + 4.5f, true));
                g.fillPath (facet);
                g.setColour (Colour (0xff2a0c08).withAlpha (0.9f));
                g.strokePath (facet, juce::PathStrokeType (1.0f));
                g.setColour (juce::Colours::white.withAlpha (active ? 0.7f : 0.25f));
                g.fillEllipse (Rectangle<float> (2.3f, 1.8f).withCentre ({ jc.x - 1.9f, jc.y - 2.4f }));
            }
            // The lever. A perspective tip: the lever swings in a plane tilted toward the viewer, so the tip is pivot + (0.3 L sin(theta), -L cos(theta)): the rod
            // is foreshortened near horizontal (length 0 at 90 degrees, the ball over the pivot and 15 % bigger) and a flip bows sideways, which makes the overshoot visible.
            const Point<float> pivot { x, kSwPivotY }, tip { x + 0.3f * kSwLever * st, kSwPivotY - kSwLever * ct };
            const float ballR = 11.0f * (1.0f + 0.15f * ast);
            {   // the ball's shadow on the face, away from the light (top left)
                g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.45f), tip.x + 5.0f, tip.y + 8.0f, juce::Colours::black.withAlpha (0.0f), tip.x + 5.0f + ballR * 1.4f, tip.y + 8.0f, true));
                g.fillEllipse (Rectangle<float> (ballR * 2.9f, ballR * 1.9f).withCentre ({ tip.x + 5.0f, tip.y + 8.0f }));
            }
            g.setColour (juce::Colours::black.withAlpha (0.4f));
            g.drawLine (pivot.x + 2.0f, pivot.y + 3.0f, tip.x + 2.0f, tip.y + 3.0f, 7.0f);
            g.setColour (Colour (0xff1c1a18));
            g.drawLine (pivot.x, pivot.y, tip.x, tip.y, 7.0f);
            {   // nickel rod: dark rim, bright body, a highlight line
                const float dx = tip.x - pivot.x, dy = tip.y - pivot.y, len = std::max (1.0e-3f, std::sqrt (dx * dx + dy * dy));
                const Point<float> nrm { -dy / len, dx / len };
                g.setColour (Colour (0xff62666c));
                g.drawLine (pivot.x, pivot.y, tip.x, tip.y, 5.4f);
                g.setColour (Colour (0xffc9ccd2));
                g.drawLine (pivot.x - nrm.x * 0.8f, pivot.y - nrm.y * 0.8f, tip.x - nrm.x * 0.8f, tip.y - nrm.y * 0.8f, 3.2f);
                g.setColour (juce::Colours::white.withAlpha (0.6f));
                g.drawLine (pivot.x - nrm.x * 1.4f, pivot.y - nrm.y * 1.4f, tip.x - nrm.x * 1.4f, tip.y - nrm.y * 1.4f, 0.9f);
            }
            // the bakelite ball
            const Colour base = active ? Colour (0xff7a1712) : Colour (0xff141212);
            g.setGradientFill (juce::ColourGradient (base.brighter (active ? 0.9f : 0.55f), tip.x - ballR * 0.4f, tip.y - ballR * 0.45f, base.darker (0.65f), tip.x + ballR * 0.75f, tip.y + ballR * 0.85f, true));
            g.fillEllipse (Rectangle<float> (ballR * 2.0f, ballR * 2.0f).withCentre (tip));
            g.setColour (juce::Colours::black.withAlpha (0.6f));
            g.drawEllipse (Rectangle<float> (ballR * 2.0f, ballR * 2.0f).withCentre (tip), 1.0f);
            {   // a proper specular highlight: a soft glow, a crisp dot, a faint reflected rim light at the lower right
                const Point<float> hl { tip.x - ballR * 0.36f, tip.y - ballR * 0.42f };
                g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (active ? 0.5f : 0.22f), hl.x, hl.y, juce::Colours::white.withAlpha (0.0f), hl.x + ballR * 0.7f, hl.y + ballR * 0.7f, true));
                g.fillEllipse (Rectangle<float> (ballR * 1.4f, ballR * 1.4f).withCentre (hl));
                g.setColour (juce::Colours::white.withAlpha (active ? 0.95f : 0.45f));
                g.fillEllipse (Rectangle<float> (ballR * 0.42f, ballR * 0.32f).withCentre ({ hl.x - ballR * 0.05f, hl.y - ballR * 0.05f }));
                Path rim;
                rim.addCentredArc (tip.x, tip.y, ballR - 1.6f, ballR - 1.6f, 0.0f, juce::degreesToRadians (110.0f), juce::degreesToRadians (170.0f), true);
                g.setColour (juce::Colours::white.withAlpha (active ? 0.18f : 0.10f));
                g.strokePath (rim, juce::PathStrokeType (1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }
            // sparks at the pivot of the switch that just went up: three frames, about 50 ms, fading
            const float age = time - w.sparkT0;
            if (w.sparkT0 >= 0.0f && age >= 0.0f && age < 0.05f)
            {
                const float fade = 1.0f - age / 0.05f;
                g.setColour (Colour (0xffe6c25a).withAlpha (0.55f * fade));
                for (int k = 0; k < 4; ++k)
                {
                    const float a = kTwoPi * ((float) k + 0.3f * (float) i) / 4.0f + 0.5f, r0 = 20.0f + 6.0f * (1.0f - fade), r1 = r0 + 6.0f + 2.0f * (float) (k % 2);
                    g.drawLine (pivot.x + std::cos (a) * r0, pivot.y + std::sin (a) * r0, pivot.x + std::cos (a) * r1, pivot.y + std::sin (a) * r1, 1.3f);
                }
            }
        }
    }

    void paintMakersPlate (Graphics& g)
    {
        const auto r = Rectangle<float> (150.0f, 18.0f).withCentre ({ 600.0f, 686.0f });   // riveted under the decal, inside the panel
        juce::DropShadow (juce::Colours::black.withAlpha (0.65f), 3, { 2, 2 }).drawForRectangle (g, r.toNearestInt());
        Path oval;
        oval.addEllipse (r);
        juce::ColourGradient grad (Colour (0xffb0863c), 0.0f, r.getY(), Colour (0xffb0863c), 0.0f, r.getBottom(), false);
        grad.addColour (0.35, Colour (0xffe2bd72));
        g.setGradientFill (grad);
        g.fillPath (oval);
        Rand rn (4711u);
        for (int i = 0; i < 70; ++i)
        {
            const float x = rn.range (r.getX() + 8.0f, r.getRight() - 8.0f), y = rn.range (r.getY() + 3.0f, r.getBottom() - 3.0f);
            g.setColour (Colour (0xff785520).withAlpha (0.15f));
            g.fillRect (x, y, rn.range (0.6f, 1.3f), rn.range (0.6f, 1.3f));
        }
        g.setColour (Colour (0xff5a3e16));
        g.strokePath (oval, juce::PathStrokeType (1.0f));
        rivet (g, r.getX() + 8.0f, r.getCentreY());
        rivet (g, r.getRight() - 8.0f, r.getCentreY());
        g.setFont (Theme::mono (7.0f));
        const auto tr = Rectangle<float> (r.getX() + 13.0f, r.getY(), r.getWidth() - 26.0f, r.getHeight());
        const auto plateText = juce::String::fromUTF8 ("No. 001 \xc2\xb7 FREE EDITION");
        g.setColour (Colour (0xfff5d78c).withAlpha (0.8f));
        g.drawText (plateText, tr.translated (0.0f, 0.6f), juce::Justification::centred, false);
        g.setColour (Colour (0xff301e0a));
        g.drawText (plateText, tr, juce::Justification::centred, false);
    }

    //--------------------------------------------------------------------------
    float surfaceY (float x) const { return kFloorY - 2.0f - moundProfile (x) * pile * 0.95f; }

    static float moundProfile (float x) { return 36.0f * std::exp (-std::pow ((x - kPileMid) / 48.0f, 2.0f)); }

    void buildPile()
    {
        Rand r (9001u);
        pieces.reserve (kPieces);
        for (int i = 0; i < kPieces; ++i)
        {
            const float x = juce::jlimit (kPileL + 2.0f, kPileR - 2.0f, kPileMid + (r.next() + r.next() + r.next() - 1.5f) * 56.0f);
            const float depth = std::pow (r.next(), 0.45f) * moundProfile (x);
            Piece p;
            p.x = x;
            p.y = kFloorY - 2.0f - depth;
            p.w = r.range (2.0f, 4.4f);
            p.h = r.range (1.5f, 3.0f);
            p.rot = r.range (-1.3f, 1.3f);
            const float c = r.next();
            p.colour = (c < 0.5f ? Theme::copper : c < 0.8f ? Theme::silver : Theme::strandRed).withMultipliedBrightness (r.range (0.6f, 1.15f));
            pieces.push_back (p);
        }
        std::sort (pieces.begin(), pieces.end(), [] (const Piece& a, const Piece& b) { return a.y > b.y; });   // the mound grows from the floor up
    }

    /** About 180 larger pieces, stacked column by column (heights of 3 px columns), so the heap reads as distinct chunks. */
    void buildBigPieces()
    {
        Rand r (5309u);
        constexpr int cols = 72;
        const float x0 = kPileL - 8.0f, colW = 3.0f;
        float h[cols] {};
        for (int i = 0; i < 180; ++i)
            for (int attempt = 0; attempt < 12; ++attempt)
            {
                const float x = juce::jlimit (kPileL + 3.0f, kPileR - 3.0f, kPileMid + (r.next() + r.next() + r.next() - 1.5f) * 60.0f);
                const float w = r.range (4.0f, 8.0f), hh = r.range (3.5f, 6.0f);
                const int c0 = juce::jlimit (0, cols - 1, (int) ((x - w * 0.5f - x0) / colW)), c1 = juce::jlimit (0, cols - 1, (int) ((x + w * 0.5f - x0) / colW));
                float top = 0.0f;
                for (int c = c0; c <= c1; ++c) top = std::max (top, h[c]);
                if (top + hh > moundProfile (x) + 2.0f)
                    continue;
                Piece p;
                p.x = x; p.y = kFloorY - 2.0f - top - hh * 0.5f; p.w = w; p.h = hh; p.rot = r.range (-0.9f, 0.9f);
                const float c = r.next();
                p.colour = (c < 0.5f ? Theme::copper : c < 0.8f ? Theme::silver : Theme::strandRed).withMultipliedBrightness (r.range (0.7f, 1.2f));
                bigPieces.push_back (p);
                for (int cc = c0; cc <= c1; ++cc) h[cc] = top + hh * 0.75f;
                break;
            }
    }


    void buildStrands()
    {
        Rand r (31337u);
        for (int i = 0; i < kStrands; ++i)
        {
            Strand s;
            const int rank = (i * 7) % kStrands;   // so the first few strands already spread over the whole outlet height
            s.y0 = 266.0f + 34.0f * (float) rank / (float) (kStrands - 1) + r.range (-1.0f, 1.0f);
            s.xEnd = r.range (kPileL, kPileR - 4.0f);
            s.c1x = r.range (1.35f, 1.9f);                      // how long the strand keeps its leftward speed
            s.c2y = kFloorY - 3.0f - r.range (0.0f, 8.0f);      // where it lands on the plinth top
            s.width = r.range (0.82f, 1.0f);
            s.idx0 = r.range (0.0f, (float) kWave);
            s.speckle = r.next();
            const float c = r.next();
            s.colour = c < 0.46f ? 0 : c < 0.82f ? 1 : 2;
            strands[(size_t) i] = s;
        }
        for (int i = 0; i < 4; ++i)   // short "mince" strands that break off close to the outlet
        {
            Strand s;
            s.y0 = 270.0f + 28.0f * (float) i / 3.0f + r.range (-2.0f, 2.0f);
            s.xEnd = r.range (kPileL + 40.0f, kPileR - 20.0f);
            s.c1x = r.range (1.3f, 1.7f);
            s.c2y = kFloorY - 6.0f;
            s.width = 1.0f;
            s.fixedW = r.range (2.5f, 4.0f);
            s.uEnd = r.range (0.18f, 0.30f);
            s.idx0 = r.range (0.0f, (float) kWave);
            s.speckle = r.next();
            s.colour = i % 2 == 0 ? 0 : 2;
            strands[(size_t) (kStrands + i)] = s;
        }
        for (size_t si = 0; si < strands.size(); ++si)
            for (int k = 0; k <= 24; ++k)
            {
                Point<float> tan;
                baseSt[si][(size_t) k] = curve (strands[si], (float) k / 24.0f, &tan);
                const float len = std::max (1.0e-3f, std::sqrt (tan.x * tan.x + tan.y * tan.y));
                baseNr[si][(size_t) k] = { -tan.y / len, tan.x / len };
            }
    }

    /** Point k of 24 on strand si, pushed sideways by the waveform; the curve and its normals are precomputed. */
    Point<float> displacedK (size_t si, int k) const
    {
        const auto& s = strands[si];
        const float u = (float) k / 24.0f;
        const float pos = s.idx0 + u * 70.0f;
        const int i0 = (int) pos % kWave;
        float w = 0.0f;
        for (int j = -2; j <= 2; ++j)
            w += wave[(size_t) ((i0 + j + kWave) % kWave)] * (j == 0 ? 0.32f : (std::abs (j) == 1 ? 0.22f : 0.12f));
        w = juce::jlimit (-1.0f, 1.0f, w * 3.0f);
        float d = w * 11.0f * u * u;
        if (br > 0.02f)
        {
            const float stepQ = 0.8f + 5.5f * br;
            d = std::round (d / stepQ) * stepQ;
            d += (hash01 ((uint32_t) si, (uint32_t) (u * 40.0f), (uint32_t) (time * 24.0f)) - 0.5f) * 4.5f * br;
        }
        return baseSt[si][(size_t) k] + baseNr[si][(size_t) k] * d;
    }

    //--------------------------------------------------------------------------
    void step (float dt)
    {
        time += dt;
        const float glitch = param (kiymaIds::glitch), brutal = param (kiymaIds::brutal), mix = param (kiymaIds::mix);
        const float crankParamValue = param (kiymaIds::crank);
        gl = approach (gl, glitch, 12.0f, dt);
        br = approach (br, brutal, 12.0f, dt);
        mx = approach (mx, mix, 12.0f, dt);

        float inTarget, outTarget;
        if (fakeAudio)
        {
            const int samples = (int) std::lround (44100.0f * dt);
            for (int i = 0; i < samples; ++i)
            {
                const float t = (float) fakeSample / 44100.0f;
                const float env = 0.62f + 0.38f * std::sin (kTwoPi * 2.1f * t);
                const float s = env * (0.42f * std::sin (kTwoPi * 96.0f * t) + 0.22f * std::sin (kTwoPi * 310.0f * t + 1.1f) + 0.2f * (fakeRand.next() - 0.5f));
                ++fakeSample;
                fakeAcc += s;
                if (++fakeCount == KiymaProcessor::kVizDecimate)
                {
                    proc.pushViz (fakeAcc / (float) KiymaProcessor::kVizDecimate);
                    fakeAcc = 0.0f;
                    fakeCount = 0;
                }
            }
            inTarget = outTarget = 0.85f;
        }
        else
        {
            const auto blocks = proc.uiBlocks.load (std::memory_order_relaxed);
            const bool live = blocks != lastBlocks;   // the audio thread is running blocks
            lastBlocks = blocks;
            inTarget = live ? juce::jlimit (0.0f, 1.0f, proc.uiInputRms.load (std::memory_order_relaxed) * 8.0f) : 0.0f;
            outTarget = live ? juce::jlimit (0.0f, 1.0f, proc.uiOutputRms.load (std::memory_order_relaxed) * 8.0f) : 0.0f;
        }
        activity = approach (activity, inTarget, 7.0f, dt);
        outActivity = approach (outActivity, outTarget, 7.0f, dt);
        proc.readViz (wave.data(), kWave);

        const float grindRaw = std::max (crankShown, 0.5f * gl + 0.5f * br) * activity;
        grind = approach (grind, grindRaw, 3.5f, dt);

        stepRecord (dt);
        stepCrank (dt, crankParamValue);
        stepChunks (dt);
        stepTags (dt);
        stepSwitches (dt);
        if (pile < 0.03f)
            settled.clear();

        const float flow = mx * outActivity;
        pile = juce::jlimit (0.0f, 1.0f, pile + (0.3f * flow - 0.015f * pile) * dt);   // full in about 4 s of audio, then it slowly decays
    }

    /** Tag text: label -> value in 120 ms on hover or drag, back 600 ms after release. */
    void stepTags (float dt)
    {
        for (int i = 0; i < 8; ++i)
        {
            auto& k = *knobs[(size_t) i];
            auto& t = tags[(size_t) i];
            const bool on = k.active() || i == forcedHover || k.wheelHold;
            if (k.wheelHold) { t.hold = 0.6f; k.wheelHold = false; }
            if (on) t.hold = 0.6f;
            if (on || t.hold > 0.0f)
            {
                t.f = juce::jmin (1.0f, t.f + dt / 0.12f);
                if (! on) t.hold -= dt;
            }
            else
                t.f = juce::jmax (0.0f, t.f - dt / 0.12f);
        }
    }

    void stepRecord (float dt)
    {
        auto& r = rec;
        if (gl > 0.05f && hash01 (frameCounter++, 1) < gl * gl * 0.03f)
            r.freeze = 1.0f / 120.0f;   // an occasional half-frame freeze
        if (r.freeze > 0.0f)
        {
            r.freeze -= dt;
            return;
        }
        r.dx = gl > 0.05f && hash01 (frameCounter, 2) < gl * gl * 0.4f ? (hash01 (frameCounter, 3) < 0.5f ? -1.0f : 1.0f) * (1.0f + 2.0f * hash01 (frameCounter, 4)) : approach (r.dx, 0.0f, 30.0f, dt);

        const bool flowing = activity > 0.02f;
        const float mouthY = kRimY + 1.0f;
        switch (r.state)
        {
            case Rec::Fall:
                r.vy += 2200.0f * dt;
                r.y += r.vy * dt;
                r.spin += kTwoPi * 0.35f * dt;
                if (r.y >= mouthY)
                {
                    r.y = mouthY;
                    r.vy = 0.0f;
                    r.state = Rec::Sink;
                    r.t = 0.0f;
                    r.depth = 0.0f;
                    r.squash = 0.12f;
                }
                break;
            case Rec::Sink:
            {
                r.t += dt;
                r.squash = approach (r.squash, 0.0f, 9.0f, dt);
                const float rps = (33.333f / 60.0f) * (0.3f + 2.0f * grind);
                r.spin += kTwoPi * rps * dt;
                if (r.t > 0.35f && flowing)
                    r.depth += (6.0f + 90.0f * grind) * dt;
                const float bob = r.t < 0.35f ? 2.2f * std::sin (r.t / 0.35f * kTwoPi) * (1.0f - r.t / 0.35f) : 0.0f;
                const float idleJiggle = flowing ? 0.0f : 0.5f * std::sin (time * 23.0f);
                r.y = mouthY + r.depth + bob + idleJiggle;
                if (r.y - kRecordR > kRimY + 10.0f)   // fully behind the hopper front
                {
                    r.state = Rec::Gone;
                    r.wait = 0.4f + 0.8f * hash01 (frameCounter, 9);
                }
                break;
            }
            case Rec::Gone:
                r.wait -= dt;
                if (r.wait <= 0.0f)
                {
                    r.state = Rec::Fall;
                    r.y = -200.0f;
                    r.vy = 0.0f;
                    r.spin = kTwoPi * hash01 (frameCounter, 11);
                }
                break;
        }
    }

    void stepCrank (float dt, float paramValue)
    {
        if (! dragging)
            crankShown = approach (crankShown, paramValue, 12.0f, dt);
        if (spinEnabled && ! dragging && paramValue > 0.001f && activity > 0.02f)
        {
            spinPhase += grind * 0.9f * dt;   // turns per second; visual only, never written to the parameter
            spinPhase -= std::floor (spinPhase);
            spinSpeed = grind * 0.9f;
        }
        else
            spinSpeed = 0.0f;
    }

    // ---- strands ---------------------------------------------------------------------------
    /** Leaves the outlet horizontally to the left, then droops on a parabola to the plinth top. */
    Point<float> curve (const Strand& s, float u, Point<float>* tangent = nullptr) const
    {
        const float dx = kOutlet.x - s.xEnd, dy = s.c2y - s.y0;
        const float inv = 1.0f - u;
        if (tangent != nullptr)
            *tangent = { -dx * s.c1x * std::pow (inv, s.c1x - 1.0f), 2.0f * dy * u };
        return { kOutlet.x - dx * (1.0f - std::pow (inv, s.c1x)), s.y0 + dy * u * u };
    }

    int activeStrands() const
    {
        if (mx < 0.01f)
            return 0;
        const float n = mx < 0.25f ? 8.0f * mx / 0.25f : 8.0f + (mx - 0.25f) / 0.75f * 18.0f;   // 8 at mix 0.25, 26 at mix 1
        return juce::jlimit (1, kStrands, (int) std::lround (n));
    }

    void stepChunks (float dt)
    {
        for (size_t i = 0; i < chunks.size();)
        {
            auto& c = chunks[i];
            c.vy += 900.0f * dt;
            c.x += c.vx * dt;
            c.y += c.vy * dt;
            c.rot += c.vr * dt;
            const float top = surfaceY (c.x);
            if (c.y >= top - 1.0f)
            {
                // it settles on the heap: a bigger piece resting on the surface (column height), deterministic
                if (settled.size() >= 40)
                    settled.erase (settled.begin());
                Piece pc;
                pc.x = c.x; pc.y = top - 1.5f; pc.w = c.w + 1.8f; pc.h = c.h + 1.2f; pc.rot = c.rot * 0.2f; pc.colour = c.colour;
                settled.push_back (pc);
                pile = juce::jmin (1.0f, pile + 0.002f);
                chunks.erase (chunks.begin() + (long) i);
            }
            else
                ++i;
        }
        const int n = activeStrands();
        if (gl > 0.05f && n > 0 && (int) chunks.size() < kMaxChunks)
        {
            const float rate = gl * gl * 24.0f * outActivity * dt;   // chunks per frame
            float acc = rate + chunkCarry;
            while (acc >= 1.0f && (int) chunks.size() < kMaxChunks)
            {
                acc -= 1.0f;
                const auto& s = strands[(size_t) (hash01 (frameCounter, (uint32_t) chunks.size(), 5) * (float) n) % (size_t) n];
                const float u = 0.15f + 0.7f * hash01 (frameCounter, (uint32_t) chunks.size(), 6);
                const auto p = displaced (s, u, 0);
                Chunk c;
                c.x = p.x; c.y = p.y;
                c.vx = (hash01 (frameCounter, 7, (uint32_t) chunks.size()) - 0.7f) * 30.0f;
                c.vy = hash01 (frameCounter, 8, (uint32_t) chunks.size()) * 15.0f;
                c.w = 2.5f + 3.5f * hash01 (frameCounter, 12, (uint32_t) chunks.size());
                c.h = 1.8f + 2.0f * hash01 (frameCounter, 13, (uint32_t) chunks.size());
                c.rot = hash01 (frameCounter, 14, (uint32_t) chunks.size()) * kTwoPi;
                c.vr = (hash01 (frameCounter, 15, (uint32_t) chunks.size()) - 0.5f) * 14.0f;
                c.colour = colourOf (s);
                chunks.push_back (c);
            }
            chunkCarry = acc;
        }
    }

    static Colour colourOf (const Strand& s) { return s.colour == 0 ? Theme::copper : s.colour == 1 ? Theme::silver : Theme::strandRed; }

    /** A point on a strand, pushed sideways by the live output waveform (anchored at the outlet, most at the tip). */
    Point<float> displaced (const Strand& s, float u, int strandIndex) const
    {
        Point<float> tan;
        const auto p = curve (s, u, &tan);
        const float len = std::max (1.0e-3f, std::sqrt (tan.x * tan.x + tan.y * tan.y));
        const Point<float> nrm { -tan.y / len, tan.x / len };
        const float pos = s.idx0 + u * 70.0f;
        const int i0 = (int) pos % kWave;
        float w = 0.0f;
        for (int k = -2; k <= 2; ++k)
            w += wave[(size_t) ((i0 + k + kWave) % kWave)] * (k == 0 ? 0.32f : (std::abs (k) == 1 ? 0.22f : 0.12f));
        w = juce::jlimit (-1.0f, 1.0f, w * 3.0f);
        float d = w * 11.0f * u * u;
        if (br > 0.02f)
        {
            const float stepQ = 0.8f + 5.5f * br;
            d = std::round (d / stepQ) * stepQ;
            d += (hash01 ((uint32_t) strandIndex, (uint32_t) (u * 40.0f), (uint32_t) (time * 24.0f)) - 0.5f) * 4.5f * br;
        }
        return p + nrm * d;
    }

    void paintStrands (Graphics& g) const
    {
        const int n = activeStrands();
        if (n == 0)
            return;
        const float alpha = juce::jlimit (0.0f, 1.0f, (0.3f + 0.7f * mx) * (0.35f + 0.65f * outActivity));
        constexpr int N = 24;
        const int total = n + (mx >= 0.5f ? 4 : 0);
        struct Drawn { Point<float> pts[N + 1]; bool hidden[N + 1]; int last; int si; };
        std::array<Drawn, kStrands + 4> d;
        Path shadowAll, coreAll, whiteAll;
        for (int i = 0; i < total; ++i)
        {
            const int si = i < n ? i : kStrands + (i - n);
            const auto& s = strands[(size_t) si];
            auto& dr = d[(size_t) i];
            dr.si = si;
            dr.last = s.uEnd < 1.0f ? juce::roundToInt (s.uEnd * (float) N) : N;
            for (int k = 0; k <= dr.last; ++k)
            {
                dr.pts[k] = displacedK ((size_t) si, k);
                dr.hidden[k] = gl > 0.05f && k > 1 && hash01 ((uint32_t) si, (uint32_t) k, (uint32_t) (time * 9.0f)) < gl * gl * 0.5f;
            }
            bool pen = false;
            for (int k = 0; k <= dr.last; ++k)
            {
                if (dr.hidden[k]) { pen = false; continue; }
                const Point<float> ps = dr.pts[k] + Point<float> (1.3f, 1.8f), pw = dr.pts[k] + Point<float> (-0.4f, -0.6f);   // the shadow's and the highlight's offsets, applied to the points
                if (! pen) { shadowAll.startNewSubPath (ps); whiteAll.startNewSubPath (pw); pen = true; }
                else { shadowAll.lineTo (ps); whiteAll.lineTo (pw); }
            }
            if (s.fixedW <= 0.0f)
            {
                // the extruded mass: every strand emerges from a dark core over its first 18 %
                const int kc = juce::roundToInt (0.18f * (float) N);
                coreAll.startNewSubPath (dr.pts[0]);
                for (int k = 1; k <= kc; ++k) coreAll.lineTo (dr.pts[k]);
            }
        }
        g.setColour (Theme::iron.withAlpha (0.7f * alpha));
        g.strokePath (coreAll, juce::PathStrokeType (6.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (juce::Colours::black.withAlpha (0.36f * alpha));
        g.strokePath (shadowAll, juce::PathStrokeType (3.2f * (1.0f + 0.8f * br) + 0.7f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        for (int i = 0; i < total; ++i)
        {
            const auto& dr = d[(size_t) i];
            const auto& s = strands[(size_t) dr.si];
            const float wScale = s.width * (1.0f + 0.8f * br);
            g.setColour (colourOf (s).withAlpha (alpha));
            // the body, tapering in six tiers (one stroke per tier, not per segment): thick at the outlet, thin at the tip
            constexpr int tiers = 6;
            for (int tier = 0; tier < tiers; ++tier)
            {
                const int k0 = dr.last * tier / tiers, k1 = dr.last * (tier + 1) / tiers;
                Path part;
                bool on = false;
                for (int k = k0; k < k1; ++k)
                {
                    if (dr.hidden[k] || dr.hidden[k + 1]) { on = false; continue; }
                    if (! on) { part.startNewSubPath (dr.pts[k]); on = true; }
                    part.lineTo (dr.pts[k + 1]);
                }
                if (part.isEmpty())
                    continue;
                const float u = ((float) tier + 0.5f) / (float) tiers * (float) dr.last / (float) N;
                const float w = s.fixedW > 0.0f ? s.fixedW : (2.8f + (1.1f - 2.8f) * u) * wScale;
                g.strokePath (part, juce::PathStrokeType (w, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }
        }
        g.setColour (juce::Colours::white.withAlpha (0.25f * alpha));   // a 1 px highlight so it reads as round wire
        g.strokePath (whiteAll, juce::PathStrokeType (1.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    void paintChunks (Graphics& g) const
    {
        for (const auto& c : chunks)
        {
            g.saveState();
            g.addTransform (AffineTransform::rotation (c.rot, c.x, c.y));
            g.setColour (c.colour);
            g.fillRoundedRectangle (c.x - c.w * 0.5f, c.y - c.h * 0.5f, c.w, c.h, 1.1f);
            g.restoreState();
        }
    }

    /** The heap: the mound (cheap, drawn each time) under two cached layers that only ever GROW (the fine pieces, then the big ones): a new piece is
        drawn onto its layer once, a layer is rebuilt only when the pile shrinks or the pixel scale changes. */
    void paintPile (Graphics& g)
    {
        const int n = (int) std::lround (pile * (float) kPieces), nb = (int) std::lround (pile * (float) bigPieces.size());
        if (n > 0 || nb > 0)
        {
            paintMound (g);
            const float ps = layers.ps;
            const Rectangle<float> area (kPileMid - 110.0f, kFloorY - 55.0f, 220.0f, 70.0f);
            if (pileLayerPs != ps || ! pileFine.isValid() || n < fineDrawn || nb < bigDrawn)
            {
                pileLayerPs = ps;
                pileFine = Image (Image::ARGB, juce::roundToInt (area.getWidth() * ps), juce::roundToInt (area.getHeight() * ps), true);
                pileBig = Image (Image::ARGB, juce::roundToInt (area.getWidth() * ps), juce::roundToInt (area.getHeight() * ps), true);
                fineDrawn = bigDrawn = 0;
            }
            if (n > fineDrawn)
            {
                Graphics gi (pileFine);
                gi.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
                gi.addTransform (AffineTransform::translation (-area.getX(), -area.getY()).scaled (ps));
                paintFinePieces (gi, fineDrawn, n);
                fineDrawn = n;
            }
            if (nb > bigDrawn)
            {
                Graphics gi (pileBig);
                gi.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
                gi.addTransform (AffineTransform::translation (-area.getX(), -area.getY()).scaled (ps));
                for (int i = bigDrawn; i < nb; ++i)
                    drawBlob (gi, bigPieces[(size_t) i], juce::jlimit (0.0f, 1.0f, (kFloorY - bigPieces[(size_t) i].y) / 34.0f));
                bigDrawn = nb;
            }
            g.setOpacity (1.0f);
            g.drawImage (pileFine, area.getX(), area.getY(), area.getWidth(), area.getHeight(), 0, 0, pileFine.getWidth(), pileFine.getHeight());
            g.drawImage (pileBig, area.getX(), area.getY(), area.getWidth(), area.getHeight(), 0, 0, pileBig.getWidth(), pileBig.getHeight());
        }
        for (const auto& p : settled)
            drawBlob (g, p, 0.7f);
    }

    static void drawBlob (Graphics& g, const Piece& p, float lit)
    {
        g.saveState();
        g.addTransform (AffineTransform::rotation (p.rot, p.x, p.y));
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.fillRoundedRectangle (p.x - p.w * 0.5f + 0.8f, p.y - p.h * 0.5f + 1.3f, p.w, p.h, p.h * 0.5f);
        g.setColour (p.colour.withMultipliedBrightness (0.6f + 0.5f * lit));
        g.fillRoundedRectangle (p.x - p.w * 0.5f, p.y - p.h * 0.5f, p.w, p.h, p.h * 0.5f);
        g.setColour (p.colour.brighter (0.6f).withAlpha (0.7f));   // a 1 px highlight along the top
        g.drawLine (p.x - p.w * 0.3f, p.y - p.h * 0.5f + 1.0f, p.x + p.w * 0.3f, p.y - p.h * 0.5f + 1.0f, 1.0f);
        g.restoreState();
    }

    void paintMound (Graphics& g) const
    {
        const float spread = 0.45f + 0.55f * pile;
        g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.55f), kPileMid, kFloorY, juce::Colours::black.withAlpha (0.0f), kPileMid + 105.0f * spread, kFloorY, true));
        g.fillEllipse (Rectangle<float> (210.0f * spread, 20.0f).withCentre ({ kPileMid, kFloorY - 1.0f }));
        Path body;
        body.startNewSubPath (kPileL, kFloorY - 1.0f);
        for (float x = kPileL; x <= kPileR; x += 5.0f)
            body.lineTo (x, kFloorY - 2.0f - moundProfile (x) * pile * 0.95f);
        body.lineTo (kPileR, kFloorY - 1.0f);
        body.closeSubPath();
        g.setGradientFill (juce::ColourGradient (Colour (0xff4a2a1d), kPileMid, kFloorY - 34.0f * pile, Colour (0xff1c100c), kPileMid, kFloorY, false));
        g.fillPath (body);
    }

    void paintFinePieces (Graphics& g, int from, int to) const
    {
        for (int i = from; i < to; ++i)
        {
            const auto& p = pieces[(size_t) i];
            g.saveState();
            g.addTransform (AffineTransform::rotation (p.rot, p.x, p.y));
            g.setColour (juce::Colours::black.withAlpha (0.5f));
            g.fillRoundedRectangle (p.x - p.w * 0.5f + 0.7f, p.y - p.h * 0.5f + 1.1f, p.w, p.h, 1.2f);
            const float lit = juce::jlimit (0.0f, 1.0f, (kFloorY - p.y) / 34.0f);
            g.setColour (p.colour.withMultipliedBrightness (0.62f + 0.5f * lit));
            g.fillRoundedRectangle (p.x - p.w * 0.5f, p.y - p.h * 0.5f, p.w, p.h, 1.2f);
            g.setColour (p.colour.brighter (0.5f).withAlpha (0.45f));
            g.drawLine (p.x - p.w * 0.4f, p.y - p.h * 0.3f, p.x + p.w * 0.3f, p.y - p.h * 0.3f, 0.6f);
            g.restoreState();
        }
    }

    // ---- record ------------------------------------------------------------------------------
    /** The disc itself (shadow, grooves, two fixed sheens, rim): everything on the record that does not turn. */
    void paintRecordFace (Graphics& g, float cx, float cy) const
    {
        constexpr float k = kBodyScale;
        const Rectangle<float> disc = Rectangle<float> (2 * kRecordR, 2 * kRecordR).withCentre ({ cx, cy });
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillEllipse (disc.translated (2.5f, 4.0f));
        g.setGradientFill (juce::ColourGradient (Colour (0xff171718), cx, cy, Theme::record, cx + kRecordR, cy, true));
        g.fillEllipse (disc);
        for (int i = 0; i < 40; ++i)
        {
            const float rad = 41.0f * k + (float) i * (kRecordR - 4.0f - 41.0f * k) / 39.0f;
            g.setColour (Theme::cream.withAlpha (i % 3 == 0 ? 0.095f : 0.06f));
            g.drawEllipse (Rectangle<float> (2 * rad, 2 * rad).withCentre ({ cx, cy }), 0.6f);
        }
        for (const float centreDeg : { 40.0f, 220.0f })   // two soft conic sheens that do not turn with the disc
            for (const float widthDeg : { 12.0f, 9.0f, 6.0f, 3.0f })
            {
                const float c0 = juce::degreesToRadians (centreDeg), hw = juce::degreesToRadians (widthDeg * 0.5f);
                Path wedge;
                wedge.addPieSegment (disc.reduced (3.0f), c0 - hw, c0 + hw, 0.33f);
                g.setColour (juce::Colours::white.withAlpha (0.025f));
                g.fillPath (wedge);
            }
        g.setColour (Colour (0xff5a5a5c).withAlpha (0.5f));
        g.drawEllipse (disc.reduced (0.5f), 0.9f);
        {
            Path arc;
            arc.addCentredArc (cx, cy, kRecordR - 1.0f, kRecordR - 1.0f, 0.0f, -1.2f, -0.25f, true);
            g.setColour (juce::Colours::white.withAlpha (0.3f));
            g.strokePath (arc, juce::PathStrokeType (1.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

    }

    void paintRecord (Graphics& g) const
    {
        const auto& r = rec;
        if (r.state == Rec::Gone)
            return;
        constexpr float k = kBodyScale;   // the record is the v1 disc at 0.75
        g.saveState();
        g.reduceClipRegion (Rectangle<int> ((int) kHopperL, -600, (int) (kHopperR - kHopperL), (int) kRecordClipBottom + 600));
        const float cx = kRecordX + r.dx, cy = r.y;
        const float stretch = r.state == Rec::Fall ? 0.04f * juce::jmin (1.0f, r.vy / 1500.0f) : -r.squash;
        g.addTransform (AffineTransform::scale (1.0f - stretch * 0.5f, 1.0f + stretch, cx, cy));

        g.setOpacity (1.0f);
        g.drawImage (recordBase, cx - 100.0f, cy - 100.0f, 200.0f, 200.0f, 0, 0, recordBase.getWidth(), recordBase.getHeight());   // disc, grooves, sheens, rim: static, cached

        g.saveState();
        g.addTransform (AffineTransform::rotation (r.spin, cx, cy));
        const float lr = 37.0f * k;
        g.setGradientFill (juce::ColourGradient (Colour (0xff8d1f19), cx - 15, cy - 15, Colour (0xff5e100c), cx + 18, cy + 18, false));
        g.fillEllipse (Rectangle<float> (2 * lr, 2 * lr).withCentre ({ cx, cy }));
        g.setColour (Theme::cream.withAlpha (0.45f));
        g.drawEllipse (Rectangle<float> (2 * lr - 6.0f, 2 * lr - 6.0f).withCentre ({ cx, cy }), 0.7f);
        {   // the BabaSultan B, gold with a darker 1 px offset shadow; it turns with the disc
            const float bh = 30.0f, bw = bh * 124.0f / 240.0f;
            const auto place = AffineTransform::scale (bw / 124.0f, bh / 240.0f).translated (cx - bw * 0.5f, cy - bh * 0.5f);
            g.setColour (Colour (0xff5a3a0a));
            g.drawImageTransformed (bImg, place.translated (1.0f, 1.0f), true);
            g.setColour (kGold);
            g.drawImageTransformed (bImg, place, true);
        }
        g.restoreState();
        g.setColour (Theme::backdrop);
        g.fillEllipse (Rectangle<float> (4.0f, 4.0f).withCentre ({ cx, cy }));
        g.restoreState();
    }

    // ---- crank -------------------------------------------------------------------------------
    /** In the live loop the crank is drawn at the angle of the last frame that repainted it (they differ by under 0.2 degrees); a snapshot draws the exact one. */
    float crankDrawAngle() const { return frameDriven ? last.crankAngle : crankAngle(); }
    float crankAngle() const
    {
        const float wobble = 0.008f * std::sin (time * 1.7f) * (1.0f - activity);
        return (crankShown + spinPhase + wobble) * kTwoPi;
    }

    AffineTransform crankPlace (float angle) const
    {
        const float ps = layers.ps, fc = 0.5f * kBodyScale * ps;   // crankPx is the 2x image scaled by fc
        return AffineTransform::translation (-41.3f * fc, -124.6f * fc).scaled (1.0f / ps).rotated (angle).translated (kPivot.x, kPivot.y);
    }

    /** Where the crank (with its shadow, ghosts and the drag label) can have drawn, at an angle. */
    Rectangle<float> crankBounds (float a, bool blur, bool label, float shownValue) const
    {
        if (! crankPx.isValid() || layers.ps <= 0.0f)
            return { 0.0f, 0.0f, (float) kWidth, 540.0f };
        const Rectangle<float> src (0.0f, 0.0f, (float) crankPx.getWidth(), (float) crankPx.getHeight());
        auto r = src.transformedBy (crankPlace (a));
        r = r.getUnion (r.translated (3.0f, 4.5f));
        if (blur)
            r = r.getUnion (src.transformedBy (crankPlace (a - juce::degreesToRadians (12.0f)))).getUnion (src.transformedBy (crankPlace (a - juce::degreesToRadians (6.0f))));
        if (label)
            r = r.getUnion (crankLabelBox (a, shownValue));
        return r;
    }

    /** The same area as crankBounds, but as small tiles of the rotated image (a long diagonal arm would otherwise dirty a huge box). */
    void crankTiles (float a, bool blur, bool label, float shownValue, std::vector<Rectangle<int>>& out) const
    {
        if (! crankPx.isValid() || layers.ps <= 0.0f)
        {
            out.push_back ({ 0, 0, kWidth, 540 });
            return;
        }
        constexpr int n = 6;
        const float w = (float) crankPx.getWidth() / (float) n, h = (float) crankPx.getHeight() / (float) n;
        std::vector<float> angles { a };
        if (blur) { angles.push_back (a - juce::degreesToRadians (6.0f)); angles.push_back (a - juce::degreesToRadians (12.0f)); }
        for (float ang : angles)
        {
            const auto tr = crankPlace (ang);
            for (int iy = 0; iy < n; ++iy)
                for (int ix = 0; ix < n; ++ix)
                {
                    const auto r = Rectangle<float> ((float) ix * w, (float) iy * h, w, h).transformedBy (tr);
                    out.push_back ((r.getUnion (r.translated (3.0f, 4.5f))).expanded (2.0f).getSmallestIntegerContainer().getIntersection ({ 0, 0, kWidth, kHeight }));
                }
        }
        if (label)
            out.push_back (padded (crankLabelBox (a, shownValue)));
    }

    Rectangle<float> crankLabelBox (float a, float shownValue) const
    {
        const Point<float> tipRel { 364.0f * kBodyScale, 133.0f * kBodyScale };
        const Point<float> tip = kPivot + Point<float> (tipRel.x * std::cos (a) - tipRel.y * std::sin (a), tipRel.x * std::sin (a) + tipRel.y * std::cos (a));
        const juce::String text = "SHUFFLE +" + juce::String (kiyma::shuffleOf (shownValue));
        const float w = Theme::textWidth (Theme::mono (11.0f), text) + 18.0f;
        auto box = Rectangle<float> (w, 24.0f).withCentre (tip + Point<float> (0.0f, 34.0f));
        return box.withX (juce::jlimit (8.0f, 1190.0f - w, box.getX())).withY (juce::jlimit (8.0f, 530.0f, box.getY()));
    }

    void paintCrank (Graphics& g) const
    {
        const float a = crankDrawAngle();
        auto place = [&] (float angle) { return crankPlace (angle); };
        const auto t = place (a);
        // a crank that is turning (spinning or dragged) is drawn with the cheaper filter; still, with the best one
        g.setImageResamplingQuality (crankMoving ? juce::Graphics::mediumResamplingQuality : juce::Graphics::highResamplingQuality);
        const float blur = juce::jlimit (0.0f, 1.0f, (spinSpeed - 0.4f) / 0.3f);   // motion-blur ghosts above 0.4 turns/s
        if (frameDriven && (crankMoving || blur > 0.0f) && layers.ps > 0.0f)
        {
            // Turning: ONE draw per frame, straight from the pre-scaled crank image with the medium filter. No shadow, no motion ghosts while it turns
            // (the still crank, drawn from its cached bitmap, has its shadow).
            g.setOpacity (1.0f);
            g.drawImageTransformed (crankPx, t, false);
        }
        else
        {
        if (blur > 0.0f)
            for (const auto& gh : { std::pair<float, float> { 6.0f, 0.25f }, std::pair<float, float> { 12.0f, 0.12f } })
            {
                g.setOpacity (gh.second * blur);
                g.drawImageTransformed (crankPx, place (a - juce::degreesToRadians (gh.first)), false);
            }
        if (! crankMoving && blur <= 0.0f && layers.ps > 0.0f)
        {
            // still: the crank with its shadow is rendered once per angle into a bitmap on whole physical pixels, and only blitted on every repaint
            const float ps = layers.ps;
            const auto cb = crankBounds (a, false, false, 0.0f);
            const int ox = (int) std::floor (cb.getX() * ps), oy = (int) std::floor (cb.getY() * ps);
            const int w = (int) std::ceil (cb.getRight() * ps) - ox, h = (int) std::ceil (cb.getBottom() * ps) - oy;
            if (crankCache.isNull() || crankCacheAngle != a || crankCachePs != ps || crankCache.getWidth() != w || crankCache.getHeight() != h)
            {
                crankCache = Image (Image::ARGB, w, h, true);
                Graphics gi (crankCache);
                gi.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
                const auto tp = t.scaled (ps).translated ((float) -ox, (float) -oy);
                gi.setColour (juce::Colours::black.withAlpha (0.4f));
                gi.drawImageTransformed (crankPx, tp.translated (3.0f * ps, 4.5f * ps), true);
                gi.setOpacity (1.0f);
                gi.drawImageTransformed (crankPx, tp, false);
                crankCacheAngle = a;
                crankCachePs = ps;
            }
            g.setOpacity (1.0f);
            g.drawImage (crankCache, (float) ox / ps, (float) oy / ps, (float) w / ps, (float) h / ps, 0, 0, w, h);
        }
        else
        {
            g.setColour (juce::Colours::black.withAlpha (0.4f));
            g.drawImageTransformed (crankPx, t.translated (3.0f, 4.5f), true);
            g.setOpacity (1.0f);
            g.drawImageTransformed (crankPx, t, false);
        }
        }

        if (dragging)
        {
            const juce::String text = "SHUFFLE +" + juce::String (kiyma::shuffleOf (crankShown));
            const auto font = Theme::mono (11.0f);
            const auto box = crankLabelBox (a, crankShown);
            g.setColour (Theme::iron.withAlpha (0.88f));
            g.fillRoundedRectangle (box, 4.0f);
            g.setColour (kGold.withAlpha (0.8f));
            g.drawRoundedRectangle (box, 4.0f, 1.0f);
            g.setColour (Theme::cream);
            g.setFont (font);
            g.drawText (text, box, juce::Justification::centred, false);
        }
    }

    // ---- knobs and tags ------------------------------------------------------------------------
    void paintKnobs (Graphics& g) const
    {
        g.setImageResamplingQuality (juce::Graphics::mediumResamplingQuality);
        const float ps = layers.ps, inv = 1.0f / ps;
        for (int i = 0; i < 8; ++i)
        {
            if (! g.clipRegionIntersects (knobRect (i).getSmallestIntegerContainer()))
                continue;
            const float cx = kKnobs[i].x, cy = kPanelY;
            const float a = juce::degreesToRadians (-135.0f + 270.0f * knobs[(size_t) i]->norm());
            g.setOpacity (1.0f);
            g.drawImageTransformed (knobPx, AffineTransform::translation (-0.5f * (float) knobPx.getWidth(), -0.5f * (float) knobPx.getHeight()).scaled (inv).rotated (a).translated (cx, cy), false);
            // the light stays top-left whatever the knob's angle: a static overlay that does not rotate
            g.drawImage (knobLight, cx - 36.0f, cy - 36.0f, 72.0f, 72.0f, 0, 0, knobLight.getWidth(), knobLight.getHeight());
        }
    }

    juce::String valueText (int i) const
    {
        const float v = param (kKnobs[i].id);
        switch (i)
        {
            case 1: return juce::String (kiyma::reachOf (v)) + " STEPS";                       // REACH: how far a displaced step may jump
            case 2: return "NO. " + juce::String (kiyma::patternIndex (v) + 1);                 // PATTERN: the seed
            case 3: return kiyma::lengthName (kiyma::lengthIndex (v));                           // LENGTH: bars of the channel squeezed into one
            case 4: return v < 0.05f ? juce::String ("HARD") : juce::String (v, 1) + " MS";     // EDGE: the cut's rounding
            case 5: return juce::String (kiyma::stepsOf (kiyma::stepsIndex (v)));               // STEPS: slices per bar
            case 7: return juce::String (v, 1) + " DB";
            default: return juce::String (juce::roundToInt (v * 100.0f)) + " %";
        }
    }

    /** Only a tag that is hovering, fading or holding is drawn live: a bare plate over the baked one, then the label and the value cross-faded. */
    void paintTagText (Graphics& g) const
    {
        g.setFont (Theme::mono (9.0f));
        for (int i = 0; i < 8; ++i)
        {
            const float f = tags[(size_t) i].f;
            if (f <= 0.004f || ! g.clipRegionIntersects (tagRect (i).getSmallestIntegerContainer()))
                continue;
            const Rectangle<float> r (kKnobs[i].x - 40.0f, 668.0f, 80.0f, 18.0f);
            g.setOpacity (1.0f);
            g.drawImage (tagBare[(size_t) i], r.getX(), r.getY(), r.getWidth(), r.getHeight(), 0, 0, tagBare[(size_t) i].getWidth(), tagBare[(size_t) i].getHeight());
            engrave (g, kKnobs[i].tag, r.reduced (12.0f, 0.0f), 1.0f - f);
            engrave (g, valueText (i), r.reduced (12.0f, 0.0f), f);
        }
    }

    //--------------------------------------------------------------------------
    KiymaProcessor& proc;
    std::array<std::unique_ptr<KnobComp>, 8> knobs;
    std::unique_ptr<SealHit> seal;
    std::array<std::unique_ptr<SwHit>, 5> swHits;
    std::array<TagState, 8> tags;

    Image bankShadow, switchImg;
    uint64_t switchKey = 0;
    float switchPs = 0.0f;
    Image tagBare[8], recordBase, crankPx, knobPx, knobLight;   // crank and knob pre-scaled to the pixel scale, the knob's still light overlay
    std::array<std::array<Point<float>, 25>, 30> baseSt {}, baseNr {};   // each strand's curve and normals, fixed
    Image pileFine, pileBig;
    int fineDrawn = 0, bigDrawn = 0;
    float pileLayerPs = 0.0f;
    Image bodyImg, crankImg, knobImg, grinderShadow, boardImg, plinthImg, sealImg, bImg, sealShadow;
    struct Layers { Image back, front; float ps = 0.0f; } layers;   // the static art, rendered once per pixel scale
    double layerBuildMs = 0.0;
    juce::RangedAudioParameter* crankParam = nullptr;
    std::unique_ptr<juce::ParameterAttachment> crankAtt;

    std::array<Strand, kStrands + 4> strands {};   // the last four are the short mince strands
    std::vector<Piece> bigPieces, settled;
    struct Sw { float theta = 3.14159265f, from = 3.14159265f, to = 3.14159265f, t0 = -1.0f, sparkT0 = -1.0f; bool up = false; };
    std::array<Sw, 5> sw {};
    int shownPreset = -1, swOver = -1;
    float spinSpeed = 0.0f;
    bool sealOver = false, forcedSeal = false;
    juce::TooltipWindow tooltipWindow { this, 500 };
    std::vector<Piece> pieces;
    std::vector<Chunk> chunks;
    std::array<float, kWave> wave {};
    Rec rec;

    double lastTick = 0.0;
    float time = 0.0f, gl = 0.0f, br = 0.0f, mx = 0.0f, activity = 0.0f, outActivity = 0.0f, grind = 0.0f, pile = 0.0f, chunkCarry = 0.0f;
    uint32_t frameCounter = 1, lastBlocks = 0;
    float crankShown = 0.0f, spinPhase = 0.0f, dragValue = 0.0f, lastMouseAngle = 0.0f;
    bool dragging = false;
    long fakeSample = 0;
    int fakeCount = 0;
    float fakeAcc = 0.0f;
    Rand fakeRand { 5150u };

    Ticker ticker;
    Waker waker;
    ParamWatch paramWatch;
    int crankTick = 0;
    bool frameDriven = false, idle = false, firstFrame = true, crankMoving = false;
    mutable Image crankCache;
    mutable float crankCacheAngle = 1.0e9f, crankCachePs = 0.0f;
    double quietSince = 0.0;
};

//==============================================================================
KiymaEditor::KiymaEditor (KiymaProcessor& p)
    : AudioProcessorEditor (&p), stage (std::make_unique<Stage> (p, *this))
{
    addAndMakeVisible (*stage);
    setResizable (true, true);
    setResizeLimits (kWidth * 3 / 4, kHeight * 3 / 4, kWidth * 5 / 4, kHeight * 5 / 4);
    getConstrainer()->setFixedAspectRatio ((double) kWidth / (double) kHeight);
    setSize (kWidth, kHeight);
    setWantsKeyboardFocus (true);
    addMouseListener (this, true);   // a click anywhere takes keyboard focus for Cmd+Z
}

KiymaEditor::~KiymaEditor() = default;

void KiymaEditor::resized()
{
    stage->setTransform ({});
    stage->setBounds (0, 0, kWidth, kHeight);
    stage->setTransform (AffineTransform::scale ((float) getWidth() / (float) kWidth));
}

bool KiymaEditor::keyPressed (const juce::KeyPress& key)
{
    return svl::handleUndoKey (static_cast<KiymaProcessor&> (processor).undo, key);
}

void KiymaEditor::mouseDown (const juce::MouseEvent& e)
{
    svl::takeFocusOnClick (*this, e);
}

void KiymaEditor::setFakeAudio (bool on) { stage->fakeAudio = on; }
void KiymaEditor::setSpinEnabled (bool on) { stage->spinEnabled = on; }
void KiymaEditor::setHover (const juce::String& id) { stage->setHover (id); }
void KiymaEditor::advance (double seconds) { stage->advance (seconds); }
void KiymaEditor::bench (float scale, int frames, double& meanMs, double& p95Ms, double& buildMs) { stage->bench (scale, frames, meanMs, p95Ms, buildMs); }
KiymaEditor::LiveStats KiymaEditor::benchLive (float scale, int frames, bool silent)
{
    const auto r = stage->benchLive (scale, frames, silent);
    LiveStats o;
    o.meanMs = r.meanMs; o.p95Ms = r.p95Ms; o.paintedPerSecond = r.paintedPerSecond; o.sharePercent = r.sharePercent;
    o.painted = r.painted; o.paintedAfter05 = r.paintedAfter05; o.paintedAfter15 = r.paintedAfter15; o.frames = r.frames;
    return o;
}
