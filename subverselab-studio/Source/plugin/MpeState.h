// MPE zone and per-channel state for the plug-in's MIDI input (no JUCE, audio thread only).
// A controller announces its zones with the MPE Configuration Message (RPN 6 on the master channel: 1 = lower zone, 16 = upper
// zone; the value is the number of member channels, 0 = zone off). Member channels carry one note each, so their pitch bend,
// channel pressure and CC74 (slide) belong to that note. Master channels and every channel outside a zone keep the old global
// behaviour. Bend ranges: members 48 semitones, masters 2, changed by RPN 0 on that channel.
#pragma once
#include <array>

struct MpeState
{
    int lower = 0, upper = 0;   // member channel counts; the lower zone's members are 2..1+lower, the upper zone's 15-upper+1..15

    bool active() const { return lower > 0 || upper > 0; }
    bool isMaster (int ch) const { return (ch == 1 && lower > 0) || (ch == 16 && upper > 0); }
    bool isMember (int ch) const { return (lower > 0 && ch >= 2 && ch <= 1 + lower) || (upper > 0 && ch >= 15 - upper + 1 && ch <= 15); }
    float bendRange (int ch) const { return range[(size_t) ch]; }

    // one controller message (ch 1..16). Returns true when it belonged to the RPN dance (consumed); sets zonesChanged when a
    // configuration message changed the zones (the caller resets the member state then)
    bool controller (int ch, int cc, int value, bool& zonesChanged)
    {
        auto& s = st[(size_t) ch];
        if (cc == 101) { s.msb = value; return true; }
        if (cc == 100) { s.lsb = value; return true; }
        if (cc != 6 && cc != 38) return false;
        const bool mcm = s.msb == 0 && s.lsb == 6, bend = s.msb == 0 && s.lsb == 0;
        if (! mcm && ! bend) return false;
        if (cc == 38) return true;   // the fine byte is not used
        if (mcm && (ch == 1 || ch == 16))
        {
            const int n = value < 0 ? 0 : (value > 15 ? 15 : value);
            int& zone = ch == 1 ? lower : upper;
            if (zone != n)
            {
                zone = n; zonesChanged = true;
                if (lower + upper > 15) { if (ch == 1) upper = 15 - lower; else lower = 15 - upper; }
                for (int c = 1; c <= 16; ++c) range[(size_t) c] = isMember (c) ? 48.0f : 2.0f;
            }
        }
        else if (bend) range[(size_t) ch] = (float) (value < 1 ? 0 : (value > 96 ? 96 : value));
        return true;
    }

private:
    struct Rpn { int msb = 127, lsb = 127; };
    std::array<Rpn, 17> st {};
    std::array<float, 17> range = [] { std::array<float, 17> r {}; r.fill (2.0f); return r; }();
};
