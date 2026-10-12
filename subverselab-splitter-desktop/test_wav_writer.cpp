#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <iostream>

bool writeValidWavFile (const juce::File& file)
{
    file.deleteFile();
    
    juce::WavAudioFormat wavFormat;
    auto outStream = file.createOutputStream();
    if (outStream == nullptr)
        return false;
        
    std::unique_ptr<juce::AudioFormatWriter> writer (wavFormat.createWriterFor (outStream.get(), 44100.0, 2, 16, {}, 0));
    if (writer != nullptr)
    {
        outStream.release(); // Writer took ownership of the raw pointer
        
        // Write 1 second of silence (44100 samples)
        juce::AudioBuffer<float> buffer (2, 44100);
        buffer.clear();
        return writer->writeFromAudioSampleBuffer (buffer, 0, 44100);
    }
    
    return false;
}

int main()
{
    // Initialize JUCE
    juce::ScopedJuceInitialiser_GUI initialiser;
    
    juce::File testFile ("/Users/senolsahan/.gemini/antigravity-ide/scratch/test_output.wav");
    std::cout << "Writing test WAV to: " << testFile.getFullPathName() << std::endl;
    
    bool success = writeValidWavFile (testFile);
    if (success && testFile.existsAsFile())
    {
        std::cout << "WAV File written successfully!" << std::endl;
        return 0;
    }
    
    std::cerr << "Failed to write WAV file!" << std::endl;
    return 1;
}
