#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <iostream>
#include <onnxruntime_cxx_api.h>

int main(int argc, char* argv[])
{
    
    if (argc < 2) {
        std::cerr << "Usage: ./test_model <path_to_audio>\n";
        return 1;
    }
    
    juce::String path = argv[1];
    juce::File inputFile(path);
    
    std::cout << "Testing JUCE Decode on: " << path << "\n";
    
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();
    
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(inputFile));
    if (reader == nullptr)
    {
        std::cerr << "Failed to read input audio file format.\n";
        return 1;
    }
    
    int numChannels = 2; 
    int totalSamples = static_cast<int>(reader->lengthInSamples);
    
    juce::AudioBuffer<float> inputBuffer(numChannels, totalSamples);
    inputBuffer.clear();
    
    bool readSuccess = reader->read(&inputBuffer, 0, totalSamples, 0, true, true);
    if (!readSuccess)
    {
        std::cerr << "Failed to read audio samples from the file (Decoder error).\n";
        return 1;
    }
    
    float maxVal = inputBuffer.getMagnitude(0, totalSamples);
    float rmsVal = inputBuffer.getRMSLevel(0, 0, totalSamples);
    std::cout << "Input Audio Max Magnitude: " << maxVal << "\n";
    std::cout << "Input Audio RMS: " << rmsVal << "\n";
    
    // Test ONNX execution
    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "SubverseSplitter");
    Ort::SessionOptions sessionOptions;
#ifdef _WIN32
    Ort::Session session(env, L"Resources/htdemucs.onnx", sessionOptions);
#else
    Ort::Session session(env, "Resources/htdemucs.onnx", sessionOptions);
#endif
    Ort::MemoryInfo memoryInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    
    const int CHUNK_SIZE = 343980;
    std::vector<int64_t> inputDims = {1, 2, CHUNK_SIZE};
    const char* inputNames[] = {"mix"};
    const char* outputNames[] = {"stems"};
    
    std::vector<float> inputTensorValues(2 * CHUNK_SIZE, 0.0f);
    
    int samplesToProcess = std::min(CHUNK_SIZE, totalSamples);
    for (int ch = 0; ch < 2; ++ch)
    {
        const float* channelData = inputBuffer.getReadPointer(std::min(ch, numChannels - 1), 0);
        std::copy(channelData, channelData + samplesToProcess, inputTensorValues.begin() + (ch * CHUNK_SIZE));
    }
    
    Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
        memoryInfo, inputTensorValues.data(), inputTensorValues.size(), inputDims.data(), inputDims.size()
    );
    
    std::cout << "Running ONNX inference on first chunk...\n";
    auto outputTensors = session.Run(Ort::RunOptions{nullptr}, inputNames, &inputTensor, 1, outputNames, 1);
    
    const float* outData = outputTensors.front().GetTensorMutableData<float>();
    
    float maxOut = 0.0f;
    for (int i = 0; i < 4 * 2 * CHUNK_SIZE; ++i)
    {
        if (std::abs(outData[i]) > maxOut) maxOut = std::abs(outData[i]);
    }
    
    std::cout << "Output Audio Max Magnitude: " << maxOut << "\n";
    
    return 0;
}
