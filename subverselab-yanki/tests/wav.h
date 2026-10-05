#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

struct WavData { int sampleRate = 0; int channels = 0; std::vector<float> samples; };

inline bool readWav(const std::string& path, WavData& out) {
    std::ifstream f(path, std::ios::binary); if (!f) return false;
    char riff[4], wave[4]; uint32_t size = 0; f.read(riff, 4); f.read(reinterpret_cast<char*>(&size), 4); f.read(wave, 4);
    if (std::string(riff, 4) != "RIFF" || std::string(wave, 4) != "WAVE") return false;
    uint16_t fmt = 0, channels = 0, bits = 0; uint32_t rate = 0, dataSize = 0; std::streampos dataPos{};
    while (f && !dataPos) {
        char id[4]; uint32_t n = 0; f.read(id, 4); f.read(reinterpret_cast<char*>(&n), 4); if (!f) break;
        if (std::string(id, 4) == "fmt ") { f.read(reinterpret_cast<char*>(&fmt), 2); f.read(reinterpret_cast<char*>(&channels), 2); f.read(reinterpret_cast<char*>(&rate), 4); f.seekg(6, std::ios::cur); f.read(reinterpret_cast<char*>(&bits), 2); f.seekg(static_cast<std::streamoff>(n) - 16, std::ios::cur); }
        else if (std::string(id, 4) == "data") { dataSize = n; dataPos = f.tellg(); f.seekg(n, std::ios::cur); }
        else f.seekg(n, std::ios::cur);
    }
    if (!dataPos || channels == 0 || (fmt != 1 && fmt != 3 && fmt != 0xfffe)) return false;
    const size_t frames = dataSize / (channels * (bits / 8)); out.sampleRate = static_cast<int>(rate); out.channels = channels; out.samples.resize(frames * channels);
    f.clear(); f.seekg(dataPos);
    for (size_t i = 0; i < out.samples.size(); ++i) {
        if (fmt == 3 && bits == 32) f.read(reinterpret_cast<char*>(&out.samples[i]), 4);
        else if (fmt == 1 && bits == 16) { int16_t v = 0; f.read(reinterpret_cast<char*>(&v), 2); out.samples[i] = static_cast<float>(v / 32768.0); }
        else if ((fmt == 1 || fmt == 0xfffe) && bits == 24) { unsigned char b[3]{}; f.read(reinterpret_cast<char*>(b), 3); int32_t v = (b[0] | (b[1] << 8) | (b[2] << 16)); if (v & 0x800000) v |= ~0xffffff; out.samples[i] = static_cast<float>(v / 8388608.0); }
        else return false;
    } return true;
}

inline std::string encodeWav24(int rate, const std::vector<float>& interleaved) {
    std::string f; const uint16_t ch = 2, bits = 24; const uint32_t data = static_cast<uint32_t>(interleaved.size() * 3), riff = 36 + data; const uint16_t fmt = 1; const uint32_t byteRate = rate * ch * 3; const uint16_t block = ch * 3;
    const auto put = [&f](const void* p, size_t n) { f.append(reinterpret_cast<const char*>(p), n); };
    f.append("RIFF", 4); put(&riff, 4); f.append("WAVEfmt ", 8); const uint32_t fmtSize = 16; put(&fmtSize, 4); put(&fmt, 2); put(&ch, 2); put(&rate, 4); put(&byteRate, 4); put(&block, 2); put(&bits, 2); f.append("data", 4); put(&data, 4);
    f.reserve(f.size() + interleaved.size() * 3);
    for (float x : interleaved) { int32_t v = static_cast<int32_t>(std::lrint(std::max(-1.0f, std::min(0.999999f, x)) * 8388607.0f)); const char b[3] = {static_cast<char>(v), static_cast<char>(v >> 8), static_cast<char>(v >> 16)}; f.append(b, 3); }
    return f;
}

inline bool writeWav24(const std::string& path, int rate, const std::vector<float>& interleaved) {
    std::ofstream f(path, std::ios::binary); if (!f) return false; const std::string b = encodeWav24(rate, interleaved); f.write(b.data(), static_cast<std::streamsize>(b.size())); return static_cast<bool>(f);
}
