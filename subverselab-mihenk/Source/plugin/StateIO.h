#pragma once

#include <JuceHeader.h>
#include "../engine/RtChain.h"
#include "../engine/Diagnosis.h"
#include "../engine/Proxy.h"
#include "../dsp/Loudness.h"

/*  JSON forms of what LEARN found, so it stays with the DAW project (the captured audio itself does not: a
    re-learn is needed only to re-solve after the settings change). Non-finite values are stored as null. */
namespace mihenk::state
{
juce::var toVar (const Measurement&);
void fromVar (const juce::var&, Measurement&);
juce::var toVar (const RtPlan&);
void fromVar (const juce::var&, RtPlan&);
juce::var toVar (const Diagnosis&);
void fromVar (const juce::var&, Diagnosis&);
juce::var toVar (const LoudnessProxy&);
void fromVar (const juce::var&, LoudnessProxy&);
juce::var floatsToVar (const std::vector<float>&);
std::vector<float> floatsFromVar (const juce::var&);
} // namespace mihenk::state
