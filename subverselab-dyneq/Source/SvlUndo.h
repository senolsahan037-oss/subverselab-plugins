// SvlUndo.h: Cmd+Z / Cmd+Shift+Z for the SubverseLab plug-ins. One header, identical in every project.
//
// MASTER COPY: subverselab-plugin-common/Undo/SvlUndo.h. Every project compiles its own copy,
// Source/SvlUndo.h, so the standalone CI repo (which syncs only Source/) still builds.
// subverselab-plugin-common/Undo/sync_undo.sh copies it into the projects and checks the md5.
//
// Why not hand a juce::UndoManager to the AudioProcessorValueTreeState? The APVTS then writes EVERY parameter
// change into the undo history from its flush timer, host automation included, and the history fills with
// automation moves the user never made. Here the history is fed only by what the editor does:
//
//   * a gesture (AudioProcessorParameter::beginChangeGesture .. endChangeGesture, which every JUCE attachment
//     and every hand-made knob already sends; nested begin/end pairs on one parameter count as one) is ONE step, however many values pass through it, and several
//     parameters moved inside overlapping gestures (an X/Y pad) are one step too;
//   * ParamUndo::Group wraps code that writes several parameters at once (a mood, a preset): one step. It
//     compares every parameter before and after, so it does not matter how the writes were made;
//   * host automation (setValueNotifyingHost with no gesture, or a gesture from another thread) is never recorded;
//   * undo / redo write the parameters through beginChangeGesture + setValueNotifyingHost + endChangeGesture,
//     so the host sees the move and can record it. They never create history of their own.
//
// Everything runs on the message thread; nothing here is called from, or allocates on, the audio thread.
#pragma once

#include <JuceHeader.h>
#include <vector>

namespace svl
{
class ParamUndo : private juce::AudioProcessorParameter::Listener
{
public:
    /** Declare it in the processor AFTER the AudioProcessorValueTreeState: `svl::ParamUndo undo { *this };` */
    explicit ParamUndo (juce::AudioProcessor& owner, int minTransactionsToKeep = 200)
        : processor (owner), manager (30000, minTransactionsToKeep)
    {
        const auto& all = processor.getParameters();
        const auto n = (size_t) all.size();
        open.assign (n, 0);
        ignored.assign (n, 0);
        startValue.assign (n, 0.0f);
        snapshot.assign (n, 0.0f);
        for (auto* p : all)
            p->addListener (this);
    }

    ~ParamUndo() override
    {
        for (auto* p : processor.getParameters())
            p->removeListener (this);
    }

    /** A parameter that is never undoable (a momentary trigger, say). By parameter ID. */
    void ignore (const juce::String& paramID)
    {
        const auto& all = processor.getParameters();
        for (int i = 0; i < all.size(); ++i)
            if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (all[i]))
                if (withId->paramID == paramID)
                    ignored[(size_t) i] = 1;
    }

    //==========================================================================
    bool canUndo() const { return idle() && manager.canUndo(); }
    bool canRedo() const { return idle() && manager.canRedo(); }
    bool undo() { return idle() && manager.undo(); }
    bool redo() { return idle() && manager.redo(); }
    int undoSteps() const { return manager.getUndoDescriptions().size(); }
    int redoSteps() const { return manager.getRedoDescriptions().size(); }

    /** The state was replaced (setStateInformation, a preset file): the old steps no longer describe it. */
    void clear()
    {
        manager.clearUndoHistory();
        // Gestures that are somehow still open stay open: their end event still has to find its start.
    }

    /** True while a knob is held or a Group is open. */
    bool userEditInProgress() const { return openCount > 0 || groupDepth > 0; }

    //==========================================================================
    /** One undo step around code that writes several parameters (message thread only). Nests; the outermost one counts. */
    class Group
    {
    public:
        explicit Group (ParamUndo& u, const juce::String& name = {}) : owner (u), active (u.beginGroup (name)) {}
        ~Group() { if (active) owner.endGroup(); }
        Group (const Group&) = delete;
        Group& operator= (const Group&) = delete;
    private:
        ParamUndo& owner;
        bool active;
    };

    /** For parameter writes the processor makes BECAUSE another parameter changed (a MOOD recall that the host
        automation or a state restore triggered): nothing is recorded, unless the user's own edit is already in
        progress, in which case the writes belong to that edit's step. */
    class Derived
    {
    public:
        explicit Derived (ParamUndo& u) : owner (u), on (! u.userEditInProgress()) { if (on) ++owner.suppress; }
        ~Derived() { if (on) --owner.suppress; }
        Derived (const Derived&) = delete;
        Derived& operator= (const Derived&) = delete;
    private:
        ParamUndo& owner;
        bool on;
    };

    juce::UndoManager& getManager() { return manager; }

private:
    struct Entry { int index; float from, to; };

    // The parameters a step changed, with their values (normalised, as the host and the APVTS see them).
    struct Step : juce::UndoableAction
    {
        Step (ParamUndo& o, std::vector<Entry> e) : owner (o), entries (std::move (e)) {}
        bool perform() override
        {
            if (first) { first = false; return true; }   // the user's own edit has already happened
            owner.apply (entries, false);
            return true;
        }
        bool undo() override { owner.apply (entries, true); return true; }
        int getSizeInUnits() override { return 8 + 12 * (int) entries.size(); }

        ParamUndo& owner;
        std::vector<Entry> entries;
        bool first = true;
    };

    bool idle() const { return ! applying && openCount == 0 && groupDepth == 0 && onMessageThread(); }
    static bool onMessageThread() { return juce::MessageManager::existsAndIsCurrentThread(); }

    // Two normalised values are the same value when the parameter turns them into the same plain value.
    bool same (int index, float a, float b) const
    {
        if (juce::exactlyEqual (a, b))
            return true;
        if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (processor.getParameters()[index]))
            return juce::exactlyEqual (r->convertFrom0to1 (a), r->convertFrom0to1 (b));
        return false;
    }

    void apply (const std::vector<Entry>& entries, bool backward)
    {
        juce::ScopedValueSetter<bool> guard (applying, true);
        const auto& all = processor.getParameters();
        // Up to four passes: writing one parameter can make the processor write others (a MOOD recall); the
        // passes put every parameter back on its recorded value afterwards.
        for (int pass = 0; pass < 4; ++pass)
        {
            bool moved = false;
            for (size_t k = 0; k < entries.size(); ++k)
            {
                const auto& e = backward ? entries[entries.size() - 1 - k] : entries[k];
                const float target = backward ? e.from : e.to;
                auto* p = all[e.index];
                if (same (e.index, p->getValue(), target))
                    continue;
                p->beginChangeGesture();
                p->setValueNotifyingHost (target);   // the normal path: the host and the editor see it
                p->endChangeGesture();
                moved = true;
            }
            if (! moved)
                break;
        }
    }

    bool beginGroup (const juce::String& name)
    {
        if (applying || suppress > 0 || ! onMessageThread())
            return false;
        if (groupDepth == 0)
        {
            if (openCount == 0)
                manager.beginNewTransaction (name);
            const auto& all = processor.getParameters();
            for (int i = 0; i < all.size(); ++i)
                snapshot[(size_t) i] = all[i]->getValue();
        }
        ++groupDepth;
        return true;
    }

    void endGroup()
    {
        if (--groupDepth > 0)
            return;
        std::vector<Entry> changed;
        const auto& all = processor.getParameters();
        for (int i = 0; i < all.size(); ++i)
        {
            if (ignored[(size_t) i])
                continue;
            const float now = all[i]->getValue();
            if (! same (i, snapshot[(size_t) i], now))
                changed.push_back ({ i, snapshot[(size_t) i], now });
        }
        if (! changed.empty())
            manager.perform (new Step (*this, std::move (changed)));
    }

    void parameterValueChanged (int, float) override {}

    void parameterGestureChanged (int index, bool starting) override
    {
        if (index < 0 || index >= (int) open.size())
            return;
        auto* p = processor.getParameters()[index];
        auto& depth = open[(size_t) index];
        if (starting)
        {
            if (depth > 0)                  // a nested begin on a parameter that is already held (a knob whose callback sets itself)
            {
                ++depth;
                return;
            }
            if (applying || suppress > 0 || groupDepth > 0 || ignored[(size_t) index] || ! onMessageThread())
                return;
            if (openCount == 0)
                manager.beginNewTransaction (p->getName (48));
            depth = 1;
            ++openCount;
            startValue[(size_t) index] = p->getValue();
        }
        else
        {
            if (depth == 0 || --depth > 0)
                return;
            --openCount;
            const float now = p->getValue();
            if (! same (index, startValue[(size_t) index], now))
                manager.perform (new Step (*this, { { index, startValue[(size_t) index], now } }));
        }
    }

    juce::AudioProcessor& processor;
    juce::UndoManager manager;
    std::vector<char> ignored;
    std::vector<int> open;            // per parameter: how many begin-gestures are open (they can nest)
    std::vector<float> startValue, snapshot;
    int openCount = 0, groupDepth = 0, suppress = 0;
    bool applying = false;
};

//==============================================================================
/** Cmd+Z undo, Cmd+Shift+Z redo (Ctrl on Windows, which also takes Ctrl+Y). True only when a step was undone or redone. */
inline bool handleUndoKey (ParamUndo& undo, const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();
    if (! mods.isCommandDown() || mods.isAltDown())
        return false;
    const int code = juce::CharacterFunctions::toLowerCase ((juce::juce_wchar) key.getKeyCode());
    if (code == 'z')
        return mods.isShiftDown() ? undo.redo() : undo.undo();
   #if ! JUCE_MAC
    if (code == 'y' && ! mods.isShiftDown())
        return undo.redo();
   #endif
    return false;
}

/** One discrete edit by the editor (a click on a selector, a key): a complete gesture, so the host sees the touch and the
    undo history gets exactly one step. `plain` is the parameter's own range (an index for a choice, 0 / 1 for a switch). */
inline void setParamAsGesture (juce::RangedAudioParameter* p, float plain)
{
    if (p == nullptr)
        return;
    p->beginChangeGesture();
    p->setValueNotifyingHost (p->convertTo0to1 (plain));
    p->endChangeGesture();
}

/** For the editor's mouseDown (with addMouseListener (this, true), so clicks on knobs arrive too): the editor takes
    the keyboard focus, so Cmd+Z reaches keyPressed. A text box that is being typed into keeps it. */
inline void takeFocusOnClick (juce::Component& editor, const juce::MouseEvent& e)
{
    if (auto* c = e.originalComponent)
        if (dynamic_cast<juce::TextEditor*> (c) != nullptr || c->findParentComponentOfClass<juce::TextEditor>() != nullptr)
            return;
    editor.grabKeyboardFocus();
}
} // namespace svl
