#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <iostream>

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();
    
    juce::File inputFile("/Users/senolsahan/Downloads/Dark Turkish Boom Bap Instrumental.mp3");
    if (!inputFile.existsAsFile()) {
        std::cerr << "File does not exist\n";
        return 1;
    }
    
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(inputFile));
    if (reader == nullptr) {
        std::cerr << "Failed to create reader\n";
        return 1;
    }
    
    std::cout << "Sample rate: " << reader->sampleRate << "\n";
    std::cout << "Length: " << reader->lengthInSamples << "\n";
    
    juce::AudioBuffer<float> buffer(2, reader->lengthInSamples);
    buffer.clear();
    
    bool success = reader->read(&buffer, 0, reader->lengthInSamples, 0, true, true);
    std::cout << "Read success: " << success << "\n";
    
    float maxVal = buffer.getMagnitude(0, buffer.getNumSamples());
    std::cout << "Max magnitude: " << maxVal << "\n";
    
    return 0;
}
