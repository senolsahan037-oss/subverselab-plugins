// SvlUndoTests.h: the undo / redo checks, identical in every project (master: subverselab-plugin-common/Undo).
//
//   svl::undotest::Result r = svl::undotest::run<MyProcessor> ("NAME", makeProc, params, recall);
//
// Header-only; include it AFTER the project's PluginProcessor.h. The processor needs `apvts` and `undo`
// (svl::ParamUndo) and the usual getStateInformation / setStateInformation.
//   params : at least three { parameter id, plain value } that differ from the processor's defaults.
//   recall : writes several parameters at once the way the plug-in's own MOOD / preset recall does
//            (it must call the real recall, which opens an svl::ParamUndo::Group).
// Every check uses the path the editor uses (begin gesture, setValueNotifyingHost, end gesture) and compares what the
// parameters hold: the normalised value bit for bit, the raw value the DSP reads bit for bit too, except that a factory
// default that is not on its parameter's own interval grid may come back as the nearest legal value (see kRawTolerance).
// The message of every check says which it was.
#pragma once

#include <JuceHeader.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "../Source/SvlUndo.h"

namespace svl::undotest
{
struct Result { int pass = 0, fail = 0; };

namespace detail
{
inline bool bitsEqual (float a, float b) { return std::memcmp (&a, &b, sizeof a) == 0; }

/** What a parameter holds, two ways: `raw` is the plain value the DSP reads (the APVTS raw value), `norm` is what the
    host and the editor see (getValue()). */
struct State
{
    std::vector<float> raw, norm;
};

template <class Proc>
State rawAll (Proc& p)
{
    State v;
    for (auto* prm : p.getParameters())
        if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (prm))
            if (auto* raw = p.apvts.getRawParameterValue (withId->paramID))
            {
                v.raw.push_back (raw->load());
                v.norm.push_back (prm->getValue());
            }
    return v;
}

// A parameter with an interval or a skew and a factory default that is not on its own grid (JUCE keeps the unsnapped
// default in the raw value on purpose) comes back from an undo as the nearest legal value, a few ulp from the default.
// Everything else is bit-identical. So: the normalised value must be bit-identical, the raw value within 1e-6 relative.
constexpr float kRawTolerance = 1.0e-6f;

inline bool rawClose (float a, float b)
{
    return bitsEqual (a, b) || std::abs (a - b) <= kRawTolerance * juce::jmax (1.0f, std::abs (a), std::abs (b));
}

inline bool sameState (const State& a, const State& b)
{
    if (a.raw.size() != b.raw.size() || a.norm.size() != b.norm.size())
        return false;
    for (size_t i = 0; i < a.raw.size(); ++i)
        if (! rawClose (a.raw[i], b.raw[i]) || ! bitsEqual (a.norm[i], b.norm[i]))
            return false;
    return true;
}

/** How many parameters differ at all (raw, bit for bit). */
inline int countDiff (const State& a, const State& b)
{
    int n = 0;
    for (size_t i = 0; i < a.raw.size() && i < b.raw.size(); ++i)
        n += bitsEqual (a.raw[i], b.raw[i]) && bitsEqual (a.norm[i], b.norm[i]) ? 0 : 1;
    return n;
}

/** "bit-identical", or the parameters that are not, with the plain values. */
template <class Proc>
std::string diffText (Proc& p, const State& a, const State& b)
{
    std::string names;
    size_t k = 0;
    int n = 0;
    float worst = 0.0f;
    for (auto* prm : p.getParameters())
        if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (prm))
            if (p.apvts.getRawParameterValue (withId->paramID) != nullptr)
            {
                if (k < a.raw.size() && k < b.raw.size() && ! (bitsEqual (a.raw[k], b.raw[k]) && bitsEqual (a.norm[k], b.norm[k])))
                {
                    char buf[200];
                    std::snprintf (buf, sizeof buf, " %s(raw %.9g vs %.9g, norm %.9g vs %.9g)", withId->paramID.toRawUTF8(),
                                   (double) a.raw[k], (double) b.raw[k], (double) a.norm[k], (double) b.norm[k]);
                    names += buf;
                    worst = juce::jmax (worst, std::abs (a.raw[k] - b.raw[k]) / juce::jmax (1.0f, std::abs (a.raw[k])));
                    ++n;
                }
                ++k;
            }
    return n == 0 ? std::string ("bit-identical") : std::to_string (n) + " not bit-identical, worst raw deviation " + std::to_string (worst) + ":" + names;
}

/** The editor's path for one discrete edit (a click, a wheel notch, a preset-free knob jump). */
template <class Proc>
void uiSet (Proc& p, const char* id, float plain)
{
    auto* prm = p.apvts.getParameter (id);
    float n = prm->convertTo0to1 (plain);
    if (juce::exactlyEqual (prm->convertFrom0to1 (n), prm->convertFrom0to1 (prm->getValue())))   // it would not move: go to the other end instead
        n = prm->getValue() < 0.5f ? 1.0f : 0.0f;
    prm->beginChangeGesture();
    prm->setValueNotifyingHost (n);
    prm->endChangeGesture();
}

struct Counter : juce::AudioProcessorParameter::Listener   // stands in for the host: what does it get told?
{
    int values = 0, begins = 0, ends = 0;
    float last = -1.0f;
    void parameterValueChanged (int, float v) override { ++values; last = v; }
    void parameterGestureChanged (int, bool starting) override { (starting ? begins : ends)++; }
};
} // namespace detail

template <class Proc>
Result run (const char* name,
            std::function<std::unique_ptr<Proc>()> make,
            const std::vector<std::pair<const char*, float>>& params,
            std::function<void (Proc&)> recall)
{
    Result r;
    auto check = [&] (bool ok, const std::string& what)
    {
        std::printf ("[%s] %s undo: %s\n", ok ? "PASS" : "FAIL", name, what.c_str());
        (ok ? r.pass : r.fail)++;
    };

    std::unique_ptr<juce::ScopedJuceInitialiser_GUI> init;   // the undo history lives on the message thread
    if (juce::MessageManager::getInstanceWithoutCreating() == nullptr)
        init = std::make_unique<juce::ScopedJuceInitialiser_GUI>();

    using namespace detail;
    if (params.size() < 3)
    {
        check (false, "needs three parameters to test with");
        return r;
    }

    // 1. Three edits, three undos, three redos. Also through the keys the editor handles.
    {
        auto p = make();
        const auto start = rawAll (*p);
        for (int i = 0; i < 3; ++i)
            uiSet (*p, params[(size_t) i].first, params[(size_t) i].second);
        const auto end = rawAll (*p);
        check (p->undo.undoSteps() == 3, "3 edits = 3 steps (got " + std::to_string (p->undo.undoSteps()) + ")");
        check (countDiff (start, end) >= 3, "the 3 edits moved the parameters");
        for (int i = 0; i < 3; ++i)
            p->undo.undo();
        check (sameState (rawAll (*p), start), "undo x3: every parameter back on its start value (" + diffText (*p, rawAll (*p), start) + ")");
        check (! p->undo.canUndo() && p->undo.canRedo() && p->undo.redoSteps() == 3, "history empty, 3 redo steps");
        for (int i = 0; i < 3; ++i)
            p->undo.redo();
        check (sameState (rawAll (*p), end), "redo x3: every parameter on its end value (" + diffText (*p, rawAll (*p), end) + ")");
        check (p->undo.canUndo() && ! p->undo.canRedo(), "redo stack consumed");

        using juce::ModifierKeys;
        const auto cmdZ = juce::KeyPress ('z', ModifierKeys::commandModifier, 'z');
        const auto cmdShiftZ = juce::KeyPress ('z', ModifierKeys::commandModifier | ModifierKeys::shiftModifier, 'Z');
        const auto cmdShiftZUpper = juce::KeyPress ('Z', ModifierKeys::commandModifier | ModifierKeys::shiftModifier, 'Z');
        const bool handledUndo = svl::handleUndoKey (p->undo, cmdZ);
        const bool undoneBack = ! sameState (rawAll (*p), end);
        const bool handledRedo = svl::handleUndoKey (p->undo, cmdShiftZ);
        check (handledUndo && undoneBack && handledRedo && sameState (rawAll (*p), end), "Cmd+Z undoes one step, Cmd+Shift+Z redoes it");
        svl::handleUndoKey (p->undo, cmdZ);
        check (svl::handleUndoKey (p->undo, cmdShiftZUpper) && sameState (rawAll (*p), end), "Cmd+Shift+Z with an upper-case key code redoes");
        check (! svl::handleUndoKey (p->undo, juce::KeyPress ('z', 0, 'z')) && ! svl::handleUndoKey (p->undo, juce::KeyPress ('x', ModifierKeys::commandModifier, 'x')),
               "plain Z and Cmd+X are not handled (key returns false)");
        while (p->undo.canUndo()) p->undo.undo();
        check (! svl::handleUndoKey (p->undo, cmdZ), "Cmd+Z with an empty history is not handled (returns false)");
    }

    // 2. A drag: 200 intermediate values inside one gesture = one step.
    {
        auto p = make();
        auto* prm = p->apvts.getParameter (params[0].first);
        const auto start = rawAll (*p);
        const float a = prm->getValue(), b = prm->convertTo0to1 (params[0].second);
        prm->beginChangeGesture();
        for (int i = 1; i <= 200; ++i)
            prm->setValueNotifyingHost (a + (b - a) * (float) i / 200.0f);
        prm->endChangeGesture();
        const auto end = rawAll (*p);
        check (p->undo.undoSteps() == 1, "200 values inside one gesture = 1 step (got " + std::to_string (p->undo.undoSteps()) + ")");
        p->undo.undo();
        check (sameState (rawAll (*p), start), std::string ("undo of the drag lands on the value before the drag, not on an intermediate one") + " (" + diffText (*p, rawAll (*p), start) + ")");
        p->undo.redo();
        check (sameState (rawAll (*p), end), std::string ("redo of the drag lands on the final value") + " (" + diffText (*p, rawAll (*p), end) + ")");

        // A knob whose own callback begins and ends a gesture on the same parameter while it is held: still one step.
        {
            const auto before = rawAll (*p);
            const int stepsBefore = p->undo.undoSteps();
            prm->beginChangeGesture();
            prm->setValueNotifyingHost (0.25f);
            prm->beginChangeGesture(); prm->setValueNotifyingHost (0.5f); prm->endChangeGesture();   // nested
            prm->setValueNotifyingHost (0.75f);
            prm->endChangeGesture();
            const auto after = rawAll (*p);
            check (p->undo.undoSteps() == stepsBefore + 1, "a nested begin/end on the held parameter stays inside the same step");
            p->undo.undo();
            check (sameState (rawAll (*p), before), std::string ("undo of the nested gesture lands on the value before the whole drag") + " (" + diffText (*p, rawAll (*p), before) + ")");
            p->undo.redo();
            check (sameState (rawAll (*p), after), std::string ("redo of the nested gesture lands on its final value") + " (" + diffText (*p, rawAll (*p), after) + ")");
        }

        // Two parameters dragged together (an X/Y pad) are one step as well.
        auto* q = p->apvts.getParameter (params[1].first);
        const auto before2 = rawAll (*p);
        const int stepsBeforeXY = p->undo.undoSteps();
        prm->beginChangeGesture(); q->beginChangeGesture();
        for (int i = 1; i <= 50; ++i) { prm->setValueNotifyingHost (b * (float) (50 - i) / 50.0f); q->setValueNotifyingHost ((float) i / 50.0f); }
        q->endChangeGesture(); prm->endChangeGesture();
        check (p->undo.undoSteps() == stepsBeforeXY + 1, "an X/Y drag of two parameters = 1 more step (got " + std::to_string (p->undo.undoSteps() - stepsBeforeXY) + ")");
        p->undo.undo();
        check (sameState (rawAll (*p), before2), std::string ("undo of the X/Y drag restores both parameters") + " (" + diffText (*p, rawAll (*p), before2) + ")");
    }

    // 3. A preset / mood recall = one step, and undoes completely.
    {
        auto p = make();
        const auto start = rawAll (*p);
        recall (*p);
        const auto end = rawAll (*p);
        const int moved = countDiff (start, end);
        check (moved >= 2, "the recall wrote " + std::to_string (moved) + " parameters");
        check (p->undo.undoSteps() == 1, "the recall = 1 step (got " + std::to_string (p->undo.undoSteps()) + ")");
        p->undo.undo();
        check (sameState (rawAll (*p), start), "undo of the recall: every parameter back on its start value (" + diffText (*p, rawAll (*p), start) + ")");
        p->undo.redo();
        check (sameState (rawAll (*p), end), "redo of the recall: every parameter on its recalled value (" + diffText (*p, rawAll (*p), end) + ")");
        // A recall in the middle of other edits stays its own step.
        uiSet (*p, params[2].first, params[2].second);
        check (p->undo.undoSteps() == 2, "an edit after the recall = a second step");
    }

    // 4. Host automation never enters the history, and an undo does not touch it.
    {
        auto p = make();
        const auto start = rawAll (*p);
        for (int i : { 0, 2 })
        {
            auto* prm = p->apvts.getParameter (params[(size_t) i].first);
            prm->setValueNotifyingHost (prm->convertTo0to1 (params[(size_t) i].second));   // no gesture: the host
        }
        p->apvts.getParameter (params[0].first)->setValue (0.0f);                           // the wrapper's own setValue
        check (p->undo.undoSteps() == 0 && ! p->undo.canUndo(), "host automation (no gesture) adds no step");
        check (! sameState (rawAll (*p), start), "(the automation did move the parameters)");

        uiSet (*p, params[1].first, params[1].second);
        auto* hostPrm = p->apvts.getParameter (params[2].first);
        hostPrm->setValueNotifyingHost (0.123f);                                             // automation after the edit
        const float hostRaw = p->apvts.getRawParameterValue (params[2].first)->load();
        check (p->undo.undoSteps() == 1, "an edit among automation = 1 step (got " + std::to_string (p->undo.undoSteps()) + ")");
        p->undo.undo();
        check (bitsEqual (p->apvts.getRawParameterValue (params[2].first)->load(), hostRaw), "undo leaves the automated parameter where the host put it");

        // A block of audio with a parameter moving the host's way must not add anything either.
        p->prepareToPlay (44100.0, 256);
        juce::AudioBuffer<float> buf (2, 256);
        juce::MidiBuffer midi;
        buf.clear();
        const int before = p->undo.undoSteps();
        for (int blk = 0; blk < 8; ++blk)
        {
            hostPrm->setValueNotifyingHost ((float) blk / 8.0f);
            p->processBlock (buf, midi);
        }
        check (p->undo.undoSteps() == before, "processBlock with moving automation adds no step");
        p->releaseResources();
    }

    // 5. A state restore clears the history.
    {
        auto p = make();
        uiSet (*p, params[0].first, params[0].second);
        uiSet (*p, params[1].first, params[1].second);
        check (p->undo.undoSteps() == 2, "(two steps before the restore)");
        juce::MemoryBlock state;
        p->getStateInformation (state);
        p->undo.undo();
        check (p->undo.canRedo(), "(a redo step exists before the restore)");
        p->setStateInformation (state.getData(), (int) state.getSize());
        check (p->undo.undoSteps() == 0 && p->undo.redoSteps() == 0 && ! p->undo.canUndo() && ! p->undo.canRedo(), "setStateInformation clears undo and redo");
        uiSet (*p, params[2].first, params[2].second);
        check (p->undo.undoSteps() == 1, "history records again after the restore");
    }

    // 6. How undo reaches the host, and what it refuses.
    {
        auto p = make();
        auto* prm = p->apvts.getParameter (params[0].first);
        const float startN = prm->getValue();
        uiSet (*p, params[0].first, params[0].second);
        Counter hostView;
        prm->addListener (&hostView);
        p->undo.undo();
        check (hostView.values >= 1 && hostView.begins == hostView.ends && hostView.begins >= 1, "undo reaches the host as a value change inside a gesture");
        check (juce::exactlyEqual (prm->convertFrom0to1 (hostView.last), prm->convertFrom0to1 (startN)), "the host was told the restored value");
        check (p->undo.undoSteps() == 0 && p->undo.redoSteps() == 1, "undo itself adds no history");
        prm->removeListener (&hostView);

        // A new edit after an undo throws the redo steps away.
        uiSet (*p, params[1].first, params[1].second);
        check (p->undo.redoSteps() == 0 && p->undo.undoSteps() == 1, "a new edit after an undo clears the redo steps");

        // Undo is refused while a knob is held.
        auto* q = p->apvts.getParameter (params[2].first);
        q->beginChangeGesture();
        q->setValueNotifyingHost (q->convertTo0to1 (params[2].second));
        check (! p->undo.undo() && ! p->undo.canUndo(), "undo is refused while a gesture is open");
        q->endChangeGesture();
        check (p->undo.undoSteps() == 2, "the held knob became a step when it was let go");

        // A click that changes nothing is not a step.
        const int steps = p->undo.undoSteps();
        auto* same = p->apvts.getParameter (params[2].first);
        same->beginChangeGesture();
        same->setValueNotifyingHost (same->getValue());
        same->endChangeGesture();
        check (p->undo.undoSteps() == steps, "a gesture that ends where it began is not a step");
    }
    return r;
}
} // namespace svl::undotest
