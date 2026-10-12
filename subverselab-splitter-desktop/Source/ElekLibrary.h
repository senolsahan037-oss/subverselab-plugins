#pragma once

#include <JuceHeader.h>
#include "Analysis.h"

/*  ELEK's own output: every separation goes to ~/Music/ELEK/<one folder per run>, with an elek.json beside the WAVs so the
    window can list earlier runs (those the user has not deleted) and open them again.

    Names (owner, 12 Oct): the track title shortened, then the stem, the tempo and the key, and the ELEK tag:
        ~/Music/ELEK/Bir Araya Gelemeyiz_144BPM_Bmin_2026-10-12 1544/
            Bir Araya Gelemeyiz_Vocals_144BPM_Bmin_ELEK.wav
*/
namespace ElekLibrary
{
    inline juce::File outputRoot()
    {
        auto root = juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("ELEK");
        root.createDirectory();
        return root;
    }

    /** The track title, shortened: no extension, no bracketed tags ("(Official Video)", "[HQ]"), no leading track number,
        underscores and dots as spaces, at most 24 characters cut at a word, nothing a file system refuses. */
    inline juce::String shortTitle (const juce::File& source)
    {
        juce::String t = source.getFileNameWithoutExtension();
        for (auto [open, close] : { std::pair<char, char> { '(', ')' }, { '[', ']' }, { '{', '}' } })
            while (t.containsChar (open) && t.indexOfChar (open) < t.lastIndexOfChar (close))
            {
                const int a = t.indexOfChar (open), b = t.indexOfChar (a, close);
                if (b < 0) break;
                t = t.substring (0, a) + " " + t.substring (b + 1);
            }
        t = t.replaceCharacters ("_.", "  ").removeCharacters ("/\\:*?\"<>|");
        t = t.trim();
        // leading track number: "01 ", "01. ", "01 - "
        if (t.length() > 2 && juce::CharacterFunctions::isDigit (t[0]))
        {
            int i = 0;
            while (i < t.length() && juce::CharacterFunctions::isDigit (t[i])) ++i;
            if (i <= 3 && i < t.length() && (t[i] == ' ' || t[i] == '-'))
                t = t.substring (i).trimCharactersAtStart (" -").trim();
        }
        while (t.contains ("  ")) t = t.replace ("  ", " ");
        if (t.endsWith (" -")) t = t.dropLastCharacters (2).trim();
        constexpr int maxLen = 24;
        if (t.length() > maxLen)
        {
            auto cut = t.substring (0, maxLen + 1);
            const int sp = cut.lastIndexOfChar (' ');
            t = (sp >= 12 ? cut.substring (0, sp) : t.substring (0, maxLen)).trimCharactersAtEnd (" -").trim();
        }
        return t.isEmpty() ? juce::String ("Track") : t;
    }

    inline juce::String bpmTag (const AudioAnalysis& a) { return a.valid ? juce::String (juce::roundToInt (a.bpm)) + "BPM" : juce::String(); }
    inline juce::String keyTag (const AudioAnalysis& a) { return a.valid ? a.key + (a.scale == "min" ? "min" : "maj") : juce::String(); }

    /** A key goes into a file name only when the reading is decisive. Below 0.5 it is a coin toss that a DJ would trust because it
        is printed on the file: on 12 Oct the wrong keys read 0.02-0.42 and the right one 0.81 (7 files; small sample). The window
        still shows the key with its confidence, as the web tool does. */
    inline bool keyIsSure (const AudioAnalysis& a) { return a.valid && a.keyConfidence >= 0.5f; }

    /** "<title>_<Stem>_<bpm>BPM_<key>_ELEK.wav" (tempo and key left out when there was nothing to measure) */
    inline juce::String stemFileName (const juce::String& title, const juce::String& stem, const AudioAnalysis& a)
    {
        juce::StringArray parts { title, stem };
        if (a.valid) parts.add (bpmTag (a));
        if (keyIsSure (a)) parts.add (keyTag (a));
        parts.add ("ELEK");
        return parts.joinIntoString ("_") + ".wav";
    }

    /** A new, unique folder for one run: "<title>_<bpm>BPM_<key>_<yyyy-mm-dd HHmm>" */
    inline juce::File newRunFolder (const juce::String& title, const AudioAnalysis& a)
    {
        juce::StringArray parts { title };
        if (a.valid) parts.add (bpmTag (a));
        if (keyIsSure (a)) parts.add (keyTag (a));
        parts.add (juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H%M"));
        auto folder = outputRoot().getNonexistentChildFile (parts.joinIntoString ("_"), {}, false);
        folder.createDirectory();
        return folder;
    }

    /** One earlier run, as its elek.json describes it. Stems the user deleted come back as files that do not exist. */
    struct Entry
    {
        juce::File folder;
        juce::String title, source, operation;       // operation: "vocal_removal" or "stem_separation" (the web tool's slugs)
        juce::Time created;
        AudioAnalysis analysis;
        juce::StringPairArray files;                  // "vocals" -> file name in the folder, ...

        juce::File file (const juce::String& stem) const
        {
            const auto name = files.getValue (stem, {});
            return name.isEmpty() ? juce::File() : folder.getChildFile (name);
        }

        bool usable() const
        {
            if (operation == "vocal_removal") return file ("vocals").existsAsFile() && file ("instrumental").existsAsFile();
            for (auto s : { "vocals", "drums", "bass", "other" })
                if (! file (s).existsAsFile()) return false;
            return true;
        }
    };

    inline void writeManifest (const Entry& e)
    {
        auto obj = std::make_unique<juce::DynamicObject>();
        obj->setProperty ("app", "ELEK");
        obj->setProperty ("title", e.title);
        obj->setProperty ("source", e.source);
        obj->setProperty ("operation", e.operation);
        obj->setProperty ("created", e.created.toISO8601 (true));
        auto an = std::make_unique<juce::DynamicObject>();
        an->setProperty ("valid", e.analysis.valid);
        an->setProperty ("bpm", e.analysis.bpm);
        an->setProperty ("key", e.analysis.key);
        an->setProperty ("scale", e.analysis.scale);
        an->setProperty ("camelot", e.analysis.camelot);
        an->setProperty ("keyConfidence", (double) e.analysis.keyConfidence);
        obj->setProperty ("analysis", juce::var (an.release()));
        auto files = std::make_unique<juce::DynamicObject>();
        for (auto& k : e.files.getAllKeys())
            files->setProperty (k, e.files[k]);
        obj->setProperty ("files", juce::var (files.release()));
        e.folder.getChildFile ("elek.json").replaceWithText (juce::JSON::toString (juce::var (obj.release())));
    }

    inline bool readManifest (const juce::File& folder, Entry& e)
    {
        const auto v = juce::JSON::parse (folder.getChildFile ("elek.json"));
        if (! v.isObject() || v["app"].toString() != "ELEK") return false;
        e.folder = folder;
        e.title = v["title"].toString();
        e.source = v["source"].toString();
        e.operation = v["operation"].toString();
        e.created = juce::Time::fromISO8601 (v["created"].toString());
        const auto an = v["analysis"];
        e.analysis.valid = (bool) an["valid"];
        e.analysis.bpm = (double) an["bpm"];
        e.analysis.key = an["key"].toString();
        e.analysis.scale = an["scale"].toString();
        e.analysis.camelot = an["camelot"].toString();
        e.analysis.keyConfidence = (float) (double) an["keyConfidence"];
        if (auto* f = v["files"].getDynamicObject())
            for (auto& p : f->getProperties())
                e.files.set (p.name.toString(), p.value.toString());
        return true;
    }

    /** Earlier runs whose stems are all still there, newest first. */
    inline std::vector<Entry> recent (int maxCount = 50)
    {
        std::vector<Entry> out;
        for (const auto& d : outputRoot().findChildFiles (juce::File::findDirectories, false))
        {
            Entry e;
            if (readManifest (d, e) && e.usable())
                out.push_back (std::move (e));
        }
        std::sort (out.begin(), out.end(), [] (const Entry& a, const Entry& b) { return a.created > b.created; });
        if ((int) out.size() > maxCount) out.resize ((size_t) maxCount);
        return out;
    }
}
