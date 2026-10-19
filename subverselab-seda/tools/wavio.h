// Minimal WAV reader/writer shared by the offline tools (PCM 16/24/32 or float32 in; float32 out).
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

struct Wav { int sr = 0; int ch = 0; std::vector<float> l, r; };

inline uint32_t rd32 (const unsigned char* p) { return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24); }
inline uint16_t rd16 (const unsigned char* p) { return (uint16_t) (p[0] | (p[1] << 8)); }

inline Wav readWav (const char* path)
{
    Wav w;
    FILE* f = std::fopen (path, "rb");
    if (! f) { std::fprintf (stderr, "cannot open %s\n", path); std::exit (2); }
    std::vector<unsigned char> d;
    unsigned char buf[65536]; size_t got;
    while ((got = std::fread (buf, 1, sizeof (buf), f)) > 0) d.insert (d.end(), buf, buf + got);
    std::fclose (f);
    if (d.size() < 12 || std::memcmp (d.data(), "RIFF", 4) != 0 || std::memcmp (d.data() + 8, "WAVE", 4) != 0) { std::fprintf (stderr, "%s: not a WAV\n", path); std::exit (2); }
    int fmt = 0, bits = 0; size_t pos = 12; const unsigned char* data = nullptr; size_t dataLen = 0;
    while (pos + 8 <= d.size())
    {
        const uint32_t len = rd32 (d.data() + pos + 4); const unsigned char* p = d.data() + pos + 8;
        if (std::memcmp (d.data() + pos, "fmt ", 4) == 0)
        {
            fmt = rd16 (p); w.ch = rd16 (p + 2); w.sr = (int) rd32 (p + 4); bits = rd16 (p + 14);
            if (fmt == 0xFFFE && len >= 26) fmt = rd16 (p + 24);   // WAVE_FORMAT_EXTENSIBLE: sub-format GUID's first two bytes
        }
        else if (std::memcmp (d.data() + pos, "data", 4) == 0) { data = p; dataLen = std::min ((size_t) len, d.size() - pos - 8); }
        pos += 8 + len + (len & 1);
    }
    if (! data || w.ch < 1 || w.sr < 1) { std::fprintf (stderr, "%s: no data/fmt\n", path); std::exit (2); }
    const int bps = bits / 8; const size_t frames = dataLen / (size_t) (bps * w.ch);
    w.l.resize (frames); w.r.resize (frames);
    for (size_t i = 0; i < frames; ++i)
        for (int c = 0; c < std::min (w.ch, 2); ++c)
        {
            const unsigned char* s = data + (i * (size_t) w.ch + (size_t) c) * (size_t) bps; float v = 0.0f;
            if (fmt == 3 && bits == 32) { float x; std::memcpy (&x, s, 4); v = x; }
            else if (fmt == 1 && bits == 16) v = (float) (int16_t) rd16 (s) / 32768.0f;
            else if (fmt == 1 && bits == 24) v = (float) ((int32_t) (((uint32_t) s[0] << 8) | ((uint32_t) s[1] << 16) | ((uint32_t) s[2] << 24)) >> 8) / 8388608.0f;
            else if (fmt == 1 && bits == 32) v = (float) (int32_t) rd32 (s) / 2147483648.0f;
            else { std::fprintf (stderr, "%s: unsupported format %d / %d bit\n", path, fmt, bits); std::exit (2); }
            (c == 0 ? w.l : w.r)[i] = v;
        }
    if (w.ch == 1) w.r = w.l;
    return w;
}

inline void writeWav (const char* path, int sr, int ch, const std::vector<float>& l, const std::vector<float>& r)
{
    FILE* f = std::fopen (path, "wb");
    if (! f) { std::fprintf (stderr, "cannot write %s\n", path); std::exit (2); }
    const uint32_t n = (uint32_t) l.size(), dataLen = n * 4u * (uint32_t) ch;
    auto w32 = [f] (uint32_t v) { unsigned char b[4] { (unsigned char) v, (unsigned char) (v >> 8), (unsigned char) (v >> 16), (unsigned char) (v >> 24) }; std::fwrite (b, 1, 4, f); };
    auto w16 = [f] (uint16_t v) { unsigned char b[2] { (unsigned char) v, (unsigned char) (v >> 8) }; std::fwrite (b, 1, 2, f); };
    std::fwrite ("RIFF", 1, 4, f); w32 (36 + dataLen); std::fwrite ("WAVE", 1, 4, f);
    std::fwrite ("fmt ", 1, 4, f); w32 (16); w16 (3); w16 ((uint16_t) ch); w32 ((uint32_t) sr); w32 ((uint32_t) sr * 4u * (uint32_t) ch); w16 ((uint16_t) (4 * ch)); w16 (32);
    std::fwrite ("data", 1, 4, f); w32 (dataLen);
    std::vector<float> inter ((size_t) n * (size_t) ch);
    for (uint32_t i = 0; i < n; ++i) { inter[(size_t) i * (size_t) ch] = l[i]; if (ch == 2) inter[(size_t) i * 2 + 1] = r[i]; }
    std::fwrite (inter.data(), 4, inter.size(), f);
    std::fclose (f);
}

