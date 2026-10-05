#pragma once

#include <JuceHeader.h>
#include <array>
#include <vector>
#include "Settings.h"
#include "Diagnosis.h"
#include "../dsp/Loudness.h"
#include "../dsp/Dynamics.h"

/*  Personal profile (SPEC_V2 §7): opt-in, local, explainable. MIHENK keeps the user's listening notes and learns from
    them which settings the user keeps choosing on which kind of track. Nothing leaves the computer and nothing is a
    black box: every learned value is a weighted average of values the notes asked for, and the panel lists each one
    with its evidence.

    A note = quick tags ("top too harsh", "not enough air", ...) and/or free text, saved with the full context: the
    input measurements (loudness, crest / PLR, spectrum bands, correlation, diagnosis findings), the settings used,
    the starting point and the applied scenarios, the result measurements, and any locked knob the user overrode.

    Learning, per group of similar tracks and per learnable knob:
      - Tracks are grouped by their measured input profile (9 features, each divided by a musically meaningful step,
        RMS distance). A note joins the nearest group whose centre is within kSimilar, else starts a new group.
      - Every note gives one value for a knob when it says something about it:
            a tag that points the knob one way  -> the value used + one step that way (the tag rules below)
            "perfect"                           -> the value used (accepted)
            a locked knob the user overrode     -> the value the user set (weight 1.5)
        Other notes say nothing about that knob and are not counted.
      - Newer notes weigh more (x0.85 per newer note in the group). The learned value is the weighted average.
      - A rule acts only with at least kMinEvidence such notes, when they agree: weighted spread within the knob's
        tolerance, and no more than 30 % of the tag weight pointing the other way. Otherwise it is listed as
        "collecting" or "mixed" and does nothing.
      - For a new track, the nearest group within kSimilar is its group; each acting rule is proposed and locks its knob.
        A rule learned only from "perfect" notes (no tag pointed the knob, no override) locks only when its value
        differs from the knob's current value by at least the knob's minimum difference: accepting a starting point
        over and over is not a reason to lock its knobs.

    The store is JSON Lines (one note per line) in ~/Library/Application Support/SubverseLab/MIHENK/Profile/notes.jsonl,
    shared by the app and the plug-in. File I/O happens on the message thread only; the audio thread never sees any of it.
*/
namespace mihenk::profile
{
//==============================================================================
// Learnable knobs

enum Param { pAir = 0, pHighShelf, pBell2, pLowShelf, pTight, pGlue, pWidth, kNumParams };

struct ParamDef
{
    const char* id;          // stable id in the notes ("airDb")
    const char* knob;        // the knob's label on the panel ("AIR")
    double step;             // what one tag moves it by
    double tolerance;        // largest weighted spread of the evidence for a rule to act
    double minDiff;          // smallest difference from the current value worth a lock
    double lo, hi;           // range
    double resolution;       // learned values are rounded to this
};

const ParamDef& def (int param);
int paramFromId (const juce::String& id);            // -1 when unknown
double get (const MasterSettings&, int param);
void set (MasterSettings&, int param, double value);
/** "+1.5 DB", "95 %", "1.2 DB GR". */
juce::String formatValue (int param, double value);

//==============================================================================
// Quick tags

enum Tag { tHarsh = 0, tNoAir, tLowLoose, tVocalBuried, tSquashed, tTooWide, tPerfect, kNumTags };

const char* tagId (int tag);                          // "top-too-harsh"
const char* tagText (int tag);                        // "top too harsh"
int tagFromId (const juce::String& id);               // -1 when unknown

struct Direction { int param; int sign; };
/** The tag rules: which knobs a tag points, and which way (one step each). "perfect" points none (it accepts). */
std::vector<Direction> tagDirections (int tag);
/** One line per tag rule, for the panel ("NOT ENOUGH AIR: AIR +0.5 DB"). */
juce::String describeTagRule (int tag);

//==============================================================================
// Input profile

constexpr int kNumFeatures = 9;
constexpr double kSimilar = 1.0;       // RMS distance in feature steps
constexpr int kMinEvidence = 3;

struct Features
{
    bool valid = false;
    std::array<double, kNumFeatures> v {};           // already divided by the feature steps
};

Features featuresOf (const DiagnosisMeasures&);
double distance (const Features&, const Features&);
/** Plain words for a group's centre: "DENSE (PLR 7.2 LU), BRIGHT TOP, WIDE". */
juce::String describe (const Features&);

//==============================================================================
// Notes

struct Override { int param = 0; double from = 0.0, to = 0.0; };

struct Note
{
    juce::String id;
    juce::Time time;
    juce::String source;                  // "app" / "plugin"
    juce::String track;                   // file name or the LEARN name
    juce::StringArray tags;               // tag ids
    juce::String text;                    // free text, kept for the user
    juce::String startingPoint;           // preset / starting point / scenario shown as loaded
    juce::StringArray scenarios;          // applied scenario ids
    Features features;
    juce::var input;                      // input measurements (object)
    juce::var settingsVar;                // the settings used (the preset format)
    MasterSettings settings;              // parsed from settingsVar
    juce::var result;                     // result measurements (object)
    std::vector<Override> overrides;
    juce::StringArray ignore;             // knob ids taken out of learning (a deleted rule)
};

struct Context
{
    const DiagnosisMeasures* measures = nullptr;
    const Measurement* input = nullptr;
    const Measurement* output = nullptr;
    const GainReductionStats* limiter = nullptr;
    const std::vector<Finding>* findings = nullptr;
    const MasterSettings* settings = nullptr;
    juce::String source, track, startingPoint;
    juce::StringArray scenarios;
    std::vector<Override> overrides;
};

Note makeNote (const juce::StringArray& tags, const juce::String& text, const Context&, juce::Time when = juce::Time::getCurrentTime());
juce::var toVar (const Note&);
bool fromVar (const juce::var&, Note&);

//==============================================================================
// Learning

struct Group
{
    Features centre;
    std::vector<int> notes;               // indices into the note list, oldest first
    juce::String key;                     // id of its first note (stable while that note exists)
    juce::String text;                    // describe (centre)
};

struct Rule
{
    enum class State { collecting, mixed, acting };
    int group = 0;
    int param = 0;
    double value = 0.0;                   // learned (rounded to the knob's resolution)
    double spread = 0.0;
    int evidence = 0;                     // notes that said something about this knob
    int towards = 0, against = 0;         // tag notes pointing up / down
    int overrides = 0;                    // notes where the user set a locked knob by hand
    State state = State::collecting;
    juce::Time updated;
    juce::String key;                     // group key + ":" + knob id
    juce::String why;                     // one line: how it was learned or why it does not act
};

struct Learned
{
    std::vector<Group> groups;
    std::vector<Rule> rules;              // by group, then knob
};

Learned learn (const std::vector<Note>& notes);

struct Proposal
{
    int param = 0;
    double value = 0.0;
    int evidence = 0;
    juce::String ruleKey;
    juce::String reason;                  // "Locked from 5 of your notes on similar tracks: AIR +1.5 dB"
};

/** Nearest group within kSimilar of `track`; -1 when none. */
int nearestGroup (const Learned&, const Features& track, double* dist = nullptr);
std::vector<Proposal> propose (const Learned&, const Features& track, const MasterSettings& current);

//==============================================================================
// Store: the notes on disk and what was learned from them (message thread only).

class Store
{
public:
    Store();
    static juce::File defaultFolder();
    void setFolder (const juce::File& folder);
    juce::File folder() const { return dir; }
    juce::File notesFile() const { return dir.getChildFile ("notes.jsonl"); }

    /** Reads the notes again when the file changed on disk (another instance, the app vs the plug-in). */
    void refresh();
    const std::vector<Note>& notes() { refresh(); return list; }
    const Learned& learned() { refresh(); return model; }

    bool add (const Note&, juce::String& error);
    /** Takes the rule's knob out of learning for every note of its group (the notes and their text stay). */
    bool deleteRule (const juce::String& ruleKey, juce::String& error);
    /** Clears the profile: the notes file is moved aside as notes-reset-<time>.jsonl (not deleted). */
    bool resetAll (juce::String& error);
    bool exportTo (const juce::File& f, juce::String& error);
    /** Adds the notes of an exported profile (duplicates by id are skipped). Returns the number added, -1 on error. */
    int importFrom (const juce::File& f, juce::String& error);

private:
    void load();
    bool rewrite (juce::String& error);

    juce::File dir;
    std::vector<Note> list;
    Learned model;
    juce::Time loadedTime;
    juce::int64 loadedSize = -1;
    bool loaded = false;
};
} // namespace mihenk::profile
