#pragma once
// Named presets for render.cpp / make_demos.cpp / make_swell_demos.cpp.
// The four original presets and the two stage voicings are the engine's
// MOODs (yanki::moodControls); their values are unchanged (md5-checked).
#include "../Source/YankiEngine.h"
#include <string>

inline yanki::Controls yankiPreset(const std::string& name) {
    if (name == "Slapback") return yanki::moodControls(1);
    if (name == "ThreeHeads") return yanki::moodControls(2);
    if (name == "DubSpiral") return yanki::moodControls(3);
    if (name == "WornLoop") return yanki::moodControls(4);
    if (name == "Yesilcam Sahne") return yanki::moodControls(5);
    if (name == "Ters Yanki") return yanki::moodControls(6);
    yanki::Controls c = yanki::moodBase();
    // legacy stage preset (not a MOOD)
    if (name == "Kahraman Donusu") { c.timeMs = 320; c.mode = 12; c.feedback = 0.18; c.toneHz = 3000; c.drive = 0.4; c.wear = 0.6; c.drama = 0.9; c.spread = 1.0; c.width = 1.0; c.mix = 0.5; }
    return c;
}
