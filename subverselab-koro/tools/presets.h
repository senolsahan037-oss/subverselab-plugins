#pragma once
// KORO MOODS: the six front-panel preset keys. Each one is a full cifte::Controls set.
// Key detection is always AUTO in a mood; the per-note rule decides which intervals may sound.
#include "../Source/CifteEngine.h"
#include <string>

namespace cifte
{
namespace moods
{
inline const char* const kNames[6] = { "TIGHT DOUBLE", "ARABESK CHOIR", "BACKING STACK", "THIRD", "FIFTH", "CHOIR" };

inline bool get (const std::string& name, Controls& c)
{
    c = Controls ();
    c.keyMode = KeyMode::Auto; c.keyLock = false; c.shareWidthTone = true; c.forcedKeys = 0;
    c.glide = 1.0f / 3.0f; c.timing = 6.0f; c.monoSafe = false;   // GLIDE 1/3 = 40 ms portamento
    if (name == "TIGHT DOUBLE")
    {   // doubler only: two close, tight voices
        c.voices = 2; c.spread = 0.18f; c.tight = 0.88f; c.width = 0.38f; c.tone = 0.78f; c.mix = 0.42f;
        c.harmony = 0.0f; c.harmonyVoices = 1; c.color = 1.0f / 3.0f;
    }
    else if (name == "ARABESK CHOIR")
    {   // doubler only: three loose, wide voices (the approved doubler preset)
        c.voices = 3; c.spread = 0.78f; c.tight = 0.22f; c.width = 0.92f; c.tone = 0.58f; c.mix = 0.55f;
        c.harmony = 0.0f; c.harmonyVoices = 1; c.color = 1.0f / 3.0f;
    }
    else if (name == "BACKING STACK")
    {   // doubler only: dark three-voice stack
        c.voices = 3; c.spread = 0.48f; c.tight = 0.5f; c.width = 0.7f; c.tone = 0.42f; c.mix = 0.62f;
        c.harmony = 0.0f; c.harmonyVoices = 1; c.color = 1.0f / 3.0f;
    }
    else if (name == "THIRD")
    {   // one harmony voice, high third (falls back to 5th / unison when the key is not sure), no doubling
        c.voices = 1; c.spread = 0.3f; c.tight = 0.6f; c.width = 1.0f; c.tone = 0.85f; c.mix = 0.0f;
        c.harmony = 0.5f; c.harmonyVoices = 1; c.color = 1.0f / 3.0f;
        c.voiceLevel = {{ 0.631f, 0.631f, 0.631f }}; c.voicePan = {{ 0.35f, 0.0f, 0.0f }};
    }
    else if (name == "FIFTH")
    {   // one harmony voice, fifth, no doubling
        c.voices = 1; c.spread = 0.3f; c.tight = 0.6f; c.width = 1.0f; c.tone = 0.85f; c.mix = 0.0f;
        c.harmony = 0.5f; c.harmonyVoices = 1; c.color = 2.0f / 3.0f;
        c.voiceLevel = {{ 0.631f, 0.631f, 0.631f }}; c.voicePan = {{ -0.35f, 0.0f, 0.0f }};
    }
    else if (name == "CHOIR")
    {   // three harmony voices {-2,+2,+4} plus a light two-voice double of the lead
        c.voices = 2; c.spread = 0.5f; c.tight = 0.5f; c.width = 1.0f; c.tone = 0.7f; c.mix = 0.3f;
        c.harmony = 0.5f; c.harmonyVoices = 3; c.color = 0.0f; c.timing = 12.0f;
        c.voiceLevel = {{ 0.56f, 0.56f, 0.45f }}; c.voicePan = {{ -0.6f, 0.6f, 0.25f }};
    }
    else return false;
    return true;
}
}
}
