#pragma once
#include "host_sim.h"
#include "../Source/PluginProcessor.h"

namespace hostsim
{
/** Parameters of the processor itself (the APVTS), plain-value aware. */
struct NativeParams : Params
{
    explicit NativeParams (YankiProcessor& proc) : p (proc) {}
    juce::AudioProcessorParameter* get (const char* id) override { return p.apvts.getParameter (id); }
    float norm (const char* id, float plain) override { return p.apvts.getParameter (id)->convertTo0to1 (plain); }
    YankiProcessor& p;
};
}
