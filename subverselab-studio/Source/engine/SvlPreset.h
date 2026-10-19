// SVL preset = one XML file: metadata + every engine parameter by id. Message thread only (does file I/O and allocates).
//
// <svlpreset version="1">
//   <meta name="Init" family="Init" author="SubverseLab" tags="init"/>
//   <param id="osc1.type" value="saw"/>
//   <param id="filter.cutoff" value="12000"/>
//   ...
// </svlpreset>
//
// Enum values are written by name. Missing ids keep their default; unknown ids are reported and ignored.
#pragma once
#include "SvlParams.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <array>
#include <vector>

namespace svl {

struct PresetMeta { std::string name = "Init", family = "Init", author = "SubverseLab", tags; };

struct Preset
{
    PresetMeta meta;
    ParamSet params;
    // the drum machine's kit: "Family/Name" of a drum preset per track; empty = kDefaultDrumKit (SvlDrums.h). Stored as
    // <drumkit t1="Drums - Kick/Thump Kick" .../> (only the tracks that differ from the default).
    std::array<std::string, kDrumTracks> drumKit {};
    std::vector<std::string> warnings;
};

namespace detail {
inline std::string xmlEscape (const std::string& s)
{
    std::string o;
    for (char c : s)
        switch (c) { case '&': o += "&amp;"; break; case '<': o += "&lt;"; break; case '>': o += "&gt;"; break;
                     case '"': o += "&quot;"; break; default: o += c; }
    return o;
}
inline std::string xmlUnescape (std::string s)
{
    const char* from[] = { "&lt;", "&gt;", "&quot;", "&apos;", "&amp;" };
    const char* to[] = { "<", ">", "\"", "'", "&" };
    for (int k = 0; k < 5; ++k)
        for (size_t p = s.find (from[k]); p != std::string::npos; p = s.find (from[k], p + 1)) s.replace (p, std::strlen (from[k]), to[k]);
    return s;
}
// Attributes of every element named `tag`, in file order.
inline std::vector<std::vector<std::pair<std::string, std::string>>> elements (const std::string& xml, const std::string& tag)
{
    std::vector<std::vector<std::pair<std::string, std::string>>> out;
    size_t p = 0;
    while ((p = xml.find ("<" + tag, p)) != std::string::npos)
    {
        const size_t q = xml.find ('>', p);
        if (q == std::string::npos) break;
        const char nx = xml[p + 1 + tag.size()];
        if (nx != ' ' && nx != '/' && nx != '>' && nx != '\t' && nx != '\n' && nx != '\r') { p = q; continue; }
        const std::string body = xml.substr (p + 1 + tag.size(), q - p - 1 - tag.size());
        std::vector<std::pair<std::string, std::string>> attrs;
        size_t i = 0;
        while (i < body.size())
        {
            while (i < body.size() && (std::isspace ((unsigned char) body[i]) || body[i] == '/')) ++i;
            const size_t ks = i;
            while (i < body.size() && body[i] != '=' && ! std::isspace ((unsigned char) body[i])) ++i;
            const std::string key = body.substr (ks, i - ks);
            while (i < body.size() && body[i] != '"' && body[i] != '\'') ++i;
            if (i >= body.size()) break;
            const char quote = body[i++];
            const size_t vs = i;
            while (i < body.size() && body[i] != quote) ++i;
            attrs.push_back ({ key, xmlUnescape (body.substr (vs, i - vs)) });
            ++i;
        }
        out.push_back (attrs);
        p = q;
    }
    return out;
}
inline std::string attr (const std::vector<std::pair<std::string, std::string>>& a, const std::string& k)
{
    for (auto& kv : a) if (kv.first == k) return kv.second;
    return {};
}
} // namespace detail

inline std::string valueToString (int i, float v)
{
    const auto& d = paramDefs()[(size_t) i];
    if (d.isEnum) return d.names[(int) v];
    char b[48];
    for (int prec : { 6, 7, 8, 9 })   // shortest text that reads back to the same float
    {
        std::snprintf (b, sizeof b, "%.*g", prec, (double) v);
        if ((float) std::strtod (b, nullptr) == v) break;
    }
    return b;
}

inline bool stringToValue (int i, const std::string& s, float& out)
{
    const auto& d = paramDefs()[(size_t) i];
    if (d.isEnum)
    {
        for (int k = 0; k < d.numNames; ++k) if (s == d.names[k]) { out = (float) k; return true; }
        return false;
    }
    char* end = nullptr;
    const double x = std::strtod (s.c_str(), &end);
    if (end == s.c_str()) return false;
    out = clampParam (i, (float) x);
    return true;
}

inline std::string presetToXml (const Preset& p)
{
    using detail::xmlEscape;
    std::ostringstream o;
    o << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<svlpreset version=\"1\">\n";
    o << "  <meta name=\"" << xmlEscape (p.meta.name) << "\" family=\"" << xmlEscape (p.meta.family) << "\" author=\""
      << xmlEscape (p.meta.author) << "\" tags=\"" << xmlEscape (p.meta.tags) << "\"/>\n";
    for (int i = 0; i < kNumParams; ++i)
        o << "  <param id=\"" << paramDefs()[(size_t) i].id << "\" value=\"" << valueToString (i, p.params[i]) << "\"/>\n";
    bool kit = false;
    for (auto& k : p.drumKit) kit |= ! k.empty();
    if (kit)
    {
        o << "  <drumkit";
        for (int t = 0; t < kDrumTracks; ++t)
            if (! p.drumKit[(size_t) t].empty()) o << " t" << (t + 1) << "=\"" << xmlEscape (p.drumKit[(size_t) t]) << "\"";
        o << "/>\n";
    }
    o << "</svlpreset>\n";
    return o.str();
}

inline bool presetFromXml (const std::string& xml, Preset& p)
{
    p = Preset();
    if (xml.find ("<svlpreset") == std::string::npos) { p.warnings.push_back ("not an svlpreset"); return false; }
    auto metas = detail::elements (xml, "meta");
    if (! metas.empty())
    {
        auto& m = metas.front();
        p.meta.name = detail::attr (m, "name"); p.meta.family = detail::attr (m, "family");
        p.meta.author = detail::attr (m, "author"); p.meta.tags = detail::attr (m, "tags");
    }
    auto kits = detail::elements (xml, "drumkit");
    if (! kits.empty())
        for (int t = 0; t < kDrumTracks; ++t) p.drumKit[(size_t) t] = detail::attr (kits.front(), "t" + std::to_string (t + 1));
    for (auto& e : detail::elements (xml, "param"))
    {
        const std::string id = detail::attr (e, "id"), val = detail::attr (e, "value");
        const int i = findParam (id);
        if (i < 0) { p.warnings.push_back ("unknown param " + id); continue; }
        float v;
        if (! stringToValue (i, val, v)) { p.warnings.push_back ("bad value for " + id + ": " + val); continue; }
        p.params[i] = v;
    }
    return true;
}

inline bool loadPreset (const std::string& path, Preset& p)
{
    std::ifstream f (path, std::ios::binary);
    if (! f) { p = Preset(); p.warnings.push_back ("cannot open " + path); return false; }
    std::stringstream ss; ss << f.rdbuf();
    return presetFromXml (ss.str(), p);
}

inline bool savePreset (const std::string& path, const Preset& p)
{
    std::ofstream f (path, std::ios::binary);
    if (! f) return false;
    f << presetToXml (p);
    return (bool) f;
}

} // namespace svl
