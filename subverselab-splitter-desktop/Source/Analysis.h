#pragma once

#include <JuceHeader.h>

/*  Tempo and key estimation.

    The product page promised "built-in BPM/key detection" from the day it
    shipped. The C++ carried no mention of tempo, key, chroma or Camelot
    anywhere — the claim described the Python prototype the plugin was ported
    from, and nobody moved it across. This is that feature, actually written.

    The web Splitter answers the same question with librosa, and its numbers and
    these will not be bit-identical: librosa's beat tracker and constant-Q
    chroma are not what runs here, and reimplementing them in C++ to chase the
    last decimal would buy nothing. What the two tools do share is everything
    that decides how a number is *read* — the Krumhansl-Schmuckler profiles, the
    Camelot map, the rule that folds a tempo into 70-180 BPM, and the confidence
    formula. So the two products agree about what a result means, which is the
    part a person notices when they cross-check one against the other.
*/
struct AudioAnalysis
{
    double bpm { 0.0 };
    double bpmAlternate { 0.0 };     // the half- or double-time reading
    juce::String key { "N/A" };      // "C", "F#", ...
    juce::String scale { "N/A" };    // "maj" or "min"
    juce::String camelot { "N/A" };
    float keyConfidence { 0.0f };    // 0 = a coin toss, 1 = decisive
    bool valid { false };

    /** Estimates tempo and key from a mono mix.

        Returns an invalid result rather than a confident wrong one when there
        is nothing to measure: silence, or a file too short to hold a bar.
    */
    static AudioAnalysis analyse (const juce::AudioBuffer<float>& audio, double sampleRate);
};
