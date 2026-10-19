// Small displays on the panel: what the oscillator, the envelopes, the filter, the pitch envelope and the LFOs do, drawn
// from the parameters (not from the audio), on dark glass. They repaint only when one of their parameters changes.
#pragma once
#include "Ui.h"

namespace jfui
{
class Scope : public juce::Component
{
public:
    explicit Scope (JingFangProcessor& p) : proc (p) { setInterceptsMouseClicks (false, false); }
    void paint (juce::Graphics&) override;
    void tick();                              // 30 Hz from the editor
protected:
    float val (const juce::String& id) const;
    virtual void drawTrace (juce::Graphics&, juce::Rectangle<float> area) = 0;
    virtual juce::StringArray watched() const = 0;
    JingFangProcessor& proc;
    // live while notes sound (owner 17 Oct: "çalma sırasında sinüs dalgaları hareket etmiyor")
    float anim = 0.0f;
    bool liveNow = false;
    JingFangProcessor::Live lv;
private:
    juce::String last;
};

class OscScope : public Scope
{
public:
    OscScope (JingFangProcessor& p, int oscIndex) : Scope (p), o (oscIndex) {}
    static float shape (int type, float ph, float pw);   // one cycle of a VA shape, ph in [0, 1)
protected:
    void drawTrace (juce::Graphics&, juce::Rectangle<float>) override;
    juce::StringArray watched() const override;
private:
    int o;
};

class EnvScope : public Scope
{
public:
    EnvScope (JingFangProcessor& p, juce::String prefix) : Scope (p), pre (std::move (prefix)) {}
protected:
    void drawTrace (juce::Graphics&, juce::Rectangle<float>) override;
    juce::StringArray watched() const override;
private:
    juce::String pre;
};

class FilterScope : public Scope
{
public:
    using Scope::Scope;
protected:
    void drawTrace (juce::Graphics&, juce::Rectangle<float>) override;
    juce::StringArray watched() const override;
};

class PitchScope : public Scope
{
public:
    using Scope::Scope;
protected:
    void drawTrace (juce::Graphics&, juce::Rectangle<float>) override;
    juce::StringArray watched() const override;
};

class LfoScope : public Scope
{
public:
    LfoScope (JingFangProcessor& p, int lfoIndex) : Scope (p), l (lfoIndex) {}
protected:
    void drawTrace (juce::Graphics&, juce::Rectangle<float>) override;
    juce::StringArray watched() const override;
private:
    int l;
};
} // namespace jfui
