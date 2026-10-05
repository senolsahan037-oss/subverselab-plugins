#pragma once
// HAVA (YAY) factory presets (shared by render.cpp and make_hava_demos.cpp)
#include "../Source/YayEngine.h"
#include <cstring>

static const char* const kPresetNames[5] = {"AIR LIGHT", "AIR FULL", "AIR + SHEEN", "SHEEN ONLY", "MASTER AIR"};
static const char* const kPresetTags[5] = {"Air_Light", "Air_Full", "Air_Sheen", "Sheen_Only", "Master_Air"};

// rear defaults for every preset: TENSION .55, TWANG .7 (the Cemalim voicing), DRIP 0, LOW CUT 300 Hz, MASTER SAFE off,
// OUTPUT 0 dB, legacy KICK 0. 2026-10-04 (owner: SHEEN made drums "plastic"; only E = SHEEN .15, LOW CUT 300, DRIP 0 was
// clean): every mood keeps the kick out of the spring (LOW CUT 300, DRIP 0) and the tank amounts were cut
// (prototypes/DINLE/13_HAVA_yeni_moodlar). Old: .35/.25, .8/.5, .5/.8, 0/1.0, MASTER AIR = AIR FULL + safe, LOW CUT 80, DRIP .5.
static inline yay::Controls preset(const char* n) {
    yay::Controls c;
    c.drip = 0.0f; c.lowcut = 300.0f;
    if (!std::strcmp(n, "AIR LIGHT"))        { c.air = 0.35f; c.sheen = 0.08f; }
    else if (!std::strcmp(n, "AIR FULL"))    { c.air = 0.80f; c.sheen = 0.25f; c.drip = 0.5f; }  // = D (owner 2026-10-05: "D_sheen25_lowcut300")
    else if (!std::strcmp(n, "AIR + SHEEN")) { c.air = 0.50f; c.sheen = 0.30f; }
    else if (!std::strcmp(n, "SHEEN ONLY"))  { c.air = 0.00f; c.sheen = 0.25f; }  // owner: 0.40 still "a little" plastic
    else if (!std::strcmp(n, "MASTER AIR"))  { c.air = 0.80f; c.sheen = 0.00f; c.masterSafe = true; }  // master colour: no spring
    return c;
}
