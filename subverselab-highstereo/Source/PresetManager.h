#pragma once

#include <JuceHeader.h>

/** Presets for HIGH STEREO.
    - Factory presets live in code. The list was chosen by MEASUREMENT on SubverseLab's own sample packs (owner 21 Nov 2026:
      measure, not listening sets): tools/preset_measure.py + preset_summary.py, logs/preset_measure/. Each preset's OUTPUT
      trim = the median sample-peak rise on its own material, rounded to 0.5 dB (the band tree rotates phase: peaks rise
      while loudness barely moves); the engine's peak guard catches the rest. Measured values:
      logs/preset_measure/factory_presets.json (the preset test checks the code against it).
    - User presets: one file per preset, no limit, in
        macOS:   ~/Library/Application Support/SubverseLab/HIGH STEREO/Presets
        Windows: %APPDATA%\SubverseLab\HIGH STEREO\Presets
      File = the parameter tree as XML, extension .hspreset. LISTEN is not a parameter, so it is never in a preset.
    All calls are on the message thread. */
class PresetManager
{
public:
    static constexpr const char* kExt = ".hspreset";
    static constexpr const char* kDefault = "Default";

    explicit PresetManager (juce::AudioProcessorValueTreeState& s, juce::File dir = defaultDirectory())
        : apvts (s), folder (std::move (dir)) {}

    static juce::File defaultDirectory()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        #if JUCE_MAC
                   .getChildFile ("Application Support")
        #endif
                   .getChildFile ("SubverseLab").getChildFile ("HIGH STEREO").getChildFile ("Presets");
    }

    struct Factory { const char* name; std::vector<std::pair<const char*, float>> values; };   // values not listed = default
    static const std::vector<Factory>& factory()
    {
        static const std::vector<Factory> f {
            { kDefault,         {} },
            { "Mono Safe Wide", { { "width", 130 }, { "monobass", 120 }, { "fix", 1 }, { "output", -2.0f } } },
            { "Wide Master",    { { "width", 125 }, { "monobass", 100 }, { "band7", 140 }, { "band8", 150 }, { "fix", 1 },
                                  { "match", 1 } } },   // MATCH includes OUTPUT, so a trim here would do nothing
            { "Fill the Gap",   { { "width", 105 }, { "spread", 70 }, { "output", -1.0f } } },
            { "Narrow Pad",     { { "width", 60 }, { "spread", 30 } } },
            { "Tight Low End",  { { "monobass", 150 }, { "band1", 0 }, { "band2", 60 }, { "output", -1.5f } } },
        };
        return f;
    }
    static const Factory* factoryPreset (const juce::String& name)
    {
        for (auto& f : factory()) if (name == f.name) return &f;
        return nullptr;
    }

    juce::StringArray factoryNames() const { juce::StringArray n; for (auto& f : factory()) n.add (f.name); return n; }

    juce::StringArray userNames() const
    {
        juce::StringArray names;
        for (const auto& f : folder.findChildFiles (juce::File::findFiles, false, juce::String ("*") + kExt))
            names.add (f.getFileNameWithoutExtension());
        names.sortNatural();
        return names;
    }

    bool isFactory (const juce::String& name) const { return factoryNames().contains (name); }

    /** Returns false (and changes nothing) if the name is empty, has path characters, or is a factory name. */
    bool save (const juce::String& rawName)
    {
        const auto name = rawName.trim();
        if (name.isEmpty() || isFactory (name) || name.containsAnyOf ("/\\:*?\"<>|")) return false;
        if (! folder.createDirectory()) return false;
        for (auto* p : apvts.processor.getParameters())   // the saved values become what double-click returns to
            if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p)) loaded[rp->paramID] = rp->convertFrom0to1 (rp->getValue());
        std::unique_ptr<juce::XmlElement> xml (apvts.copyState().createXml());
        if (xml == nullptr || ! xml->writeTo (fileFor (name))) return false;
        current = name;
        return true;
    }

    bool load (const juce::String& name)
    {
        if (auto* fp = factoryPreset (name))
        {
            loaded.clear();
            for (auto* p : apvts.processor.getParameters())
                if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
                {
                    float v = rp->getDefaultValue();
                    for (auto& [id, val] : fp->values) if (rp->paramID == id) v = rp->convertTo0to1 (val);
                    rp->setValueNotifyingHost (v);
                    loaded[rp->paramID] = rp->convertFrom0to1 (v);
                }
            current = name;
            return true;
        }
        auto xml = juce::XmlDocument::parse (fileFor (name));
        if (xml == nullptr || ! xml->hasTagName (apvts.state.getType())) return false;
        // Only parameters this version knows are applied; anything missing goes to its default.
        for (auto* p : apvts.processor.getParameters())
            if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
            {
                float v = rp->getDefaultValue();
                for (auto* child : xml->getChildIterator())
                    if (child->getStringAttribute ("id") == rp->paramID)
                        v = rp->convertTo0to1 ((float) child->getDoubleAttribute ("value"));
                rp->setValueNotifyingHost (v);
                loaded[rp->paramID] = rp->convertFrom0to1 (v);
            }
        current = name;
        return true;
    }

    /** The value this parameter had when the current preset was loaded (UI signature: double-click returns to it). */
    float valueAtLoad (const juce::String& id, float fallback) const
    {
        const auto it = loaded.find (id);
        return it != loaded.end() ? it->second : fallback;
    }
    /** After a session reopens: re-read the named preset's values (missing file -> parameter defaults). */
    void refreshLoaded()
    {
        loaded.clear();
        if (current.isEmpty() || current == kDefault) return;
        if (auto* fp = factoryPreset (current))
        {
            for (auto* p : apvts.processor.getParameters())
                if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
                {
                    float v = rp->convertFrom0to1 (rp->getDefaultValue());
                    for (auto& [id, val] : fp->values) if (rp->paramID == id) v = val;
                    loaded[rp->paramID] = v;
                }
            return;
        }
        if (auto xml = juce::XmlDocument::parse (fileFor (current)))
            for (auto* child : xml->getChildIterator())
                if (child->hasAttribute ("id")) loaded[child->getStringAttribute ("id")] = (float) child->getDoubleAttribute ("value");
    }

    void loadDefault()
    {
        for (auto* p : apvts.processor.getParameters())
            if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
                rp->setValueNotifyingHost (rp->getDefaultValue());
        loaded.clear();   // defaults
        current = kDefault;
    }

    bool remove (const juce::String& name)
    {
        if (isFactory (name)) return false;
        const bool ok = fileFor (name).deleteFile();
        if (ok && current == name) current = {};
        return ok;
    }

    bool exists (const juce::String& name) const { return isFactory (name) || fileFor (name).existsAsFile(); }

    /** Why a name cannot be saved, or empty if it can (the save dialog shows this instead of closing silently). */
    juce::String nameProblem (const juce::String& rawName) const
    {
        const auto name = rawName.trim();
        if (name.isEmpty()) return "Type a name for the preset.";
        if (isFactory (name)) return "\"" + name + "\" is a built-in preset. Choose another name.";
        if (name.containsAnyOf ("/\\:*?\"<>|")) return "A name cannot contain / \\ : * ? \" < > |";
        return {};
    }
    /** A free name for the save dialog: "My preset 1", "My preset 2", ... */
    juce::String suggestName() const
    {
        for (int i = 1;; ++i)
        {
            const auto n = "My preset " + juce::String (i);
            if (! exists (n)) return n;
        }
    }
    juce::String currentName() const { return current; }
    void setCurrentName (const juce::String& n) { current = n; }
    juce::File directory() const { return folder; }
    void setDirectory (juce::File d) { folder = std::move (d); }   // tests only

    /** Next / previous across factory then user presets, wrapping around. */
    juce::String neighbour (int step) const
    {
        auto all = factoryNames(); all.addArray (userNames());
        if (all.isEmpty()) return {};
        int i = all.indexOf (current);
        if (i < 0) i = 0;
        else i = (i + step + all.size()) % all.size();
        return all[i];
    }

private:
    juce::File fileFor (const juce::String& name) const { return folder.getChildFile (name + kExt); }

    juce::AudioProcessorValueTreeState& apvts;
    juce::File folder;
    juce::String current { kDefault };
    std::map<juce::String, float> loaded;
};
