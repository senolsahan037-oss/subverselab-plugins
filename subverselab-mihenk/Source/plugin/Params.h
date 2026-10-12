#pragma once

#include <JuceHeader.h>
#include <functional>
#include <vector>
#include "../engine/Settings.h"

/*  The automatable parameters: every knob and switch on the panel is one APVTS parameter, mapped onto a field of
    MasterSettings. The ranges are continuous (the panel's knobs step, the host does not), so a value set by the
    engine (the drive under TARGET LOCK, an AUTO threshold) is stored as solved.

    Fields that are not parameters (the dynamic bands' Q / range / ratio / timing, the tone-match curve) live in the
    settings JSON saved with the state; scenarios set them.
*/
namespace mihenk::params
{
struct Def
{
    enum Kind { real, boolean, choice };
    juce::String id, name, unit;
    Kind kind = real;
    float minimum = 0.0f, maximum = 1.0f, midpoint = 0.0f;       // midpoint > 0: skewed range
    juce::StringArray choices;
    std::function<double (const MasterSettings&)> get;
    std::function<void (MasterSettings&, double)> set;           // value in the parameter's own units (index for choices)
    double defaultValue() const;
};

const std::vector<Def>& definitions();
/** The definition of a parameter id, or nullptr. */
const Def* findDef (const juce::String& id);
juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

/** The settings as the parameters hold them: every parameter-backed value clamped to its range and rounded to float. */
MasterSettings quantised (const MasterSettings& s);

/** Mono below: 0 is off; anything between 0 and 40 Hz counts as 40 Hz (the real-time split's lowest frequency). */
constexpr double kMonoBelowMinHz = 40.0;

class Bridge
{
public:
    explicit Bridge (juce::AudioProcessorValueTreeState& state);

    /** Parameters -> the settings' parameter fields. Allocation-free and lock-free: safe on the audio thread. */
    void read (MasterSettings& s) const noexcept;

    /** Settings -> parameters, notifying the host for each value that changed. Message thread.
        With `gesture` each change is wrapped in begin / end change gestures (automation "touch" recording). */
    void write (const MasterSettings& s, bool gesture);

    /** True when this parameter value is one this bridge wrote itself (so the listener can ignore the echo). */
    bool isEcho (const juce::String& id, float value) const;

    juce::RangedAudioParameter* find (const juce::String& id) const;
    int size() const { return (int) items.size(); }

private:
    struct Item
    {
        const Def* def = nullptr;
        std::atomic<float>* raw = nullptr;
        juce::RangedAudioParameter* param = nullptr;
        float written = std::numeric_limits<float>::quiet_NaN();
    };
    juce::AudioProcessorValueTreeState& apvts;
    std::vector<Item> items;
};
} // namespace mihenk::params
