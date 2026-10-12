/*  A console harness for Source/Analysis.cpp.

    The point is not that the code compiles — that much the plugin build shows.
    It is that the numbers are right, checked against the web Splitter, which
    reports 136.0 BPM and B minor (Camelot 10A) for the file this is run on.
    Two independent implementations agreeing is worth more than either one
    looking plausible on its own.

        cmake --build build --target test_analysis
        ./build/test_analysis <file.wav>
*/
#include <JuceHeader.h>

#include "Source/Analysis.h"

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::cout << "usage: test_analysis <audio file>\n";
        return 1;
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    // Braces, not parentheses: `juce::File file (juce::String (argv[1]))`
    // parses as a function declaration, not a variable.
    const juce::File file { juce::String (argv[1]) };
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr)
    {
        std::cout << "could not read: " << file.getFullPathName() << "\n";
        return 1;
    }

    juce::AudioBuffer<float> buffer ((int) reader->numChannels, (int) reader->lengthInSamples);
    reader->read (&buffer, 0, (int) reader->lengthInSamples, 0, true, true);

    const auto start = juce::Time::getMillisecondCounterHiRes();
    const auto result = AudioAnalysis::analyse (buffer, reader->sampleRate);
    const auto elapsed = juce::Time::getMillisecondCounterHiRes() - start;

    std::cout << "  file        " << file.getFileName() << "\n"
              << "  length      " << juce::String (buffer.getNumSamples() / reader->sampleRate, 1) << " s\n"
              << "  valid       " << (result.valid ? "yes" : "no") << "\n";

    if (result.valid)
        std::cout << "  bpm         " << juce::String (result.bpm, 1)
                  << "   (alternate " << juce::String (result.bpmAlternate, 1) << ")\n"
                  << "  key         " << result.key << " " << result.scale << "\n"
                  << "  camelot     " << result.camelot << "\n"
                  << "  confidence  " << juce::String (result.keyConfidence, 3) << "\n";

    std::cout << "  took        " << juce::String (elapsed, 0) << " ms\n";
    return 0;
}
