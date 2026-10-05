#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <functional>
#include "MasterChain.h"
#include "Diagnosis.h"
#include "Plan.h"

/*  Batch and album mastering: many files through the same engine, one after the other, into one folder, with a report.

    SAME settings: every track gets the panel's settings. PER TRACK: every track keeps its own settings (stored from
    the panel), or the panel's settings plus the scenarios its own diagnosis finds.

    ALBUM mode makes the tracks sit together (all measured first, then rendered):
      - loudness, album-relative: the loudest input meets the target; every other track is placed below the target by
        half of its distance below the loudest input, at most kAlbumMaxDropLu. Quiet songs stay quieter, the running
        order keeps its shape, and nothing is pushed up to the loudest song's level.
            target_i = T + max (-kAlbumMaxDropLu, kAlbumKeep * (L_i - L_max))
        SPEC_V3 2 REVISED: each track REACHES its own target_i (within +-0.2 LU, true peak <= ceiling) by trim + the limiter, like
        any chosen target; the old +3 / +8 dB caps do not apply. The offsets between tracks are kept exactly.
        With KEEP INPUT every track holds its own loudness (already album-relative).
      - tone: each track's spectral tilt (dB/oct, measured by the diagnosis) is nudged half-way towards the album's
        mean tilt, at most kAlbumMaxTiltNudge dB/oct, through the EQ's TILT (pivot 1 kHz).
    Every file is exported as <name>_MIHENK.wav with its own .mihenk.txt, and the batch writes
    MIHENK_batch_report.csv and .json (LUFS, true peak, LRA, tilt, and every change made).
*/
namespace mihenk::batch
{
constexpr double kAlbumKeep = 0.5, kAlbumMaxDropLu = 3.0, kAlbumTiltKeep = 0.5, kAlbumMaxTiltNudge = 0.5;

/** v3: every track is planned: solvePlan (the track's own measurements, the job's intent). The intent is the job's `intent` when
    `hasIntent`, else it is read from `settings` (target preset, target loudness, ceiling); nothing else in `settings` is used. */
struct Job
{
    juce::File file;
    MasterSettings settings;
    Intent intent;
    bool hasIntent = false;
    bool applyDiagnosis = false;       // PER TRACK without stored settings: CHARACTER FULL (the diagnosis findings applied as the plan)
    bool ownSettings = false;          // PER TRACK: an intent stored for this track (report only)
};

/** The intent a panel's settings stand for: the target, the ceiling (character OFF, no overrides). */
Intent intentFromSettings (const MasterSettings&);

struct Options
{
    bool album = false;
    juce::File outDir;
    int bits = 24;
    juce::String suffix { "_MIHENK" };
};

struct AlbumPlan
{
    bool valid = false;
    double targetLufs = -14.0, loudestLufs = kNegInf, meanSlope = 0.0;
    bool keepInput = false;
    std::vector<double> offsetLu, tiltNudge;
};

/** The album plan from each track's input loudness and tilt. */
AlbumPlan planAlbum (const std::vector<double>& integrated, const std::vector<double>& slopeDbPerOct, const Intent& shared);
AlbumPlan planAlbum (const std::vector<double>& integrated, const std::vector<double>& slopeDbPerOct, const MasterSettings& shared);

struct TrackReport
{
    juce::File source, output;
    bool ok = false;
    juce::String error, settingsMode;
    Measurement in, out;
    double inSlope = 0.0, outSlope = 0.0;
    double targetLufs = kNegInf, albumOffsetLu = 0.0, tiltNudge = 0.0;
    double trimDb = 0.0, driveDb = 0.0, glueGrDb = 0.0, limiterGrMaxDb = 0.0;
    double plannedGainDb = 0.0, neededGainDb = 0.0;   // what the plan applied, and what the target asked for before the caps
    bool targetUnreachable = false;
    juce::StringArray planLines;                         // the plan's explain, in words
    juce::StringArray scenarios, warnings;
    juce::String loudness;             // the plain loudness sentence
};

struct Result
{
    std::vector<TrackReport> tracks;
    AlbumPlan plan;
    juce::File csv, json;
    bool cancelled = false;
    juce::String message;
};

/** progress (track index, track count, overall 0..1, stage text); called on the calling thread. */
using ProgressFn = std::function<void (int, int, double, const juce::String&)>;

/** Runs the whole batch (blocking; the app calls it from its own thread). */
Result run (const std::vector<Job>& jobs, const Options& options, ProgressFn progress = {}, const std::atomic<bool>* cancel = nullptr);

/** The CSV / JSON report of a batch (also written by run()). */
bool writeReports (Result& result, const Options& options, juce::String& error);

/** Output file for a source in the batch folder: <stem><suffix>.wav, numbered when two sources share a name. */
juce::File outputFor (const juce::File& source, const Options& options, const std::vector<juce::File>& taken);
} // namespace mihenk::batch
