#pragma once

#include "Settings.h"

/*  Presets: the whole MasterSettings as JSON (*.mihenkpreset), stored in
    ~/Library/Application Support/SubverseLab/MIHENK/Presets. The list starts
    with the three factory starting points, then the 20 factory scenarios
    (category "Scenarios"; applied on top of the current settings, amounts from
    the diagnosis), then the user presets by name. A user preset saved from an
    edited scenario keeps the category "Scenarios" in its file. */
namespace mihenk
{
constexpr const char* kCategoryStartingPoints = "Starting points";
constexpr const char* kCategoryScenarios = "Scenarios";
constexpr const char* kCategoryUser = "User";

bool savePreset (const juce::File&, const MasterSettings&, juce::String& error, const juce::String& category = {}, int scenario = 0);
bool loadPreset (const juce::File&, MasterSettings&, juce::String& error);
/** Category and scenario stored in a preset file ("" / 0 for a plain user preset). */
void readPresetCategory (const juce::File&, juce::String& category, int& scenario);

constexpr const char* kPresetExtension = ".mihenkpreset";

/** The whole MasterSettings as a JSON object (the preset format) and back (missing fields keep their value). */
juce::var settingsToVar (const MasterSettings&);
void settingsFromVar (const juce::var&, MasterSettings&);
/** Every numeric field finite and inside a sane range (NaN / Inf take the default), the tone-match curve dropped when it is
    not on the spectrum grid. Called on everything read from a file or a saved state; parameter ranges are tighter and apply on top. */
void sanitiseSettings (MasterSettings&);
juce::File defaultPresetsFolder();

struct PresetEntry
{
    juce::String name;
    bool factory = false;
    Genre genre = Genre::rapTrap;      // factory starting points
    juce::File file;                   // user entries
    juce::String category = kCategoryUser;
    int scenario = 0;                  // factory scenarios (and user copies of one): 1..20
};

/** Factory starting points first (in panel order), then the 20 factory scenarios, then the user presets in `folder`, sorted by name. */
std::vector<PresetEntry> listPresets (const juce::File& folder);

/** A preset name made safe as a file name (no path separators or control characters), trimmed, max 64 chars. */
juce::String sanitisePresetName (const juce::String& name);

/** Settings as a string for "modified" checks. Values the engine solves (the drive under TARGET LOCK,
    the threshold in AUTO) are left out, so a re-solve never marks a preset as modified. */
juce::String settingsFingerprint (const MasterSettings&);
}
