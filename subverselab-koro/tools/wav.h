#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

struct Wav { int rate = 0, channels = 0; std::vector<float> samples; };
static uint32_t rd32 (const unsigned char* p) { return uint32_t (p[0]) | uint32_t (p[1]) << 8 | uint32_t (p[2]) << 16 | uint32_t (p[3]) << 24; }
static uint16_t rd16 (const unsigned char* p) { return uint16_t (p[0]) | uint16_t (p[1]) << 8; }
static void put16 (FILE* f, uint16_t x) { unsigned char p[2] = { static_cast<unsigned char> (x), static_cast<unsigned char> (x >> 8) }; std::fwrite (p, 1, 2, f); }
static void put32 (FILE* f, uint32_t x) { unsigned char p[4] = { static_cast<unsigned char> (x), static_cast<unsigned char> (x >> 8), static_cast<unsigned char> (x >> 16), static_cast<unsigned char> (x >> 24) }; std::fwrite (p, 1, 4, f); }

static Wav readWav (const std::string& path)
{
    FILE* f = std::fopen (path.c_str (), "rb"); if (!f) throw std::runtime_error ("cannot open input WAV");
    unsigned char h[12]; if (std::fread (h, 1, 12, f) != 12 || std::memcmp (h, "RIFF", 4) || std::memcmp (h + 8, "WAVE", 4)) throw std::runtime_error ("not RIFF/WAVE");
    int rate = 0, channels = 0, bits = 0, format = 0; std::vector<unsigned char> data; unsigned char ch[8];
    while (std::fread (ch, 1, 8, f) == 8)
    {
        const uint32_t n = rd32 (ch + 4); std::vector<unsigned char> b (n); if (n && std::fread (b.data (), 1, n, f) != n) break;
        if (!std::memcmp (ch, "fmt ", 4) && n >= 16) { format = rd16 (b.data ()); channels = rd16 (b.data () + 2); rate = int (rd32 (b.data () + 4)); bits = rd16 (b.data () + 14); }
        if (!std::memcmp (ch, "data", 4)) data.swap (b); if (n & 1u) std::fseek (f, 1, SEEK_CUR);
    }
    std::fclose (f); if (!rate || !channels || data.empty ()) throw std::runtime_error ("incomplete WAV");
    const int bytes = bits / 8; if ((format != 1 && format != 3) || (bits != 16 && bits != 24 && bits != 32)) throw std::runtime_error ("unsupported WAV format");
    Wav out { rate, channels, std::vector<float> (data.size () / size_t (bytes)) };
    for (size_t i = 0; i < out.samples.size (); ++i)
    {
        const unsigned char* p = data.data () + i * size_t (bytes); float x = 0.0f;
        if (format == 3) std::memcpy (&x, p, 4);
        else if (bits == 16) x = float (int16_t (rd16 (p))) / 32768.0f;
        else if (bits == 24) { int32_t v = int32_t (p[0] | (p[1] << 8) | (p[2] << 16)); if (v & 0x800000) v |= ~0xffffff; x = float (v) / 8388608.0f; }
        else x = float (int32_t (rd32 (p))) / 2147483648.0f;
        out.samples[i] = x;
    }
    return out;
}

static void writeWav (const std::string& path, int rate, int channels, const std::vector<float>& x)
{
    FILE* f = std::fopen (path.c_str (), "wb"); if (!f) throw std::runtime_error ("cannot open output WAV");
    const uint32_t dataBytes = uint32_t (x.size () * 3u); std::fwrite ("RIFF", 1, 4, f); put32 (f, 36u + dataBytes); std::fwrite ("WAVEfmt ", 1, 8, f); put32 (f, 16); put16 (f, 1); put16 (f, uint16_t (channels)); put32 (f, uint32_t (rate)); put32 (f, uint32_t (rate * channels * 3)); put16 (f, uint16_t (channels * 3)); put16 (f, 24); std::fwrite ("data", 1, 4, f); put32 (f, dataBytes);
    for (float a : x) { const int32_t v = int32_t (std::max (-1.0f, std::min (0.999999f, a)) * 8388607.0f); unsigned char p[3] = { static_cast<unsigned char> (v), static_cast<unsigned char> (v >> 8), static_cast<unsigned char> (v >> 16) }; std::fwrite (p, 1, 3, f); }
    std::fclose (f);
}
