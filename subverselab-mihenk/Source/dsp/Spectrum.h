#pragma once

#include <JuceHeader.h>
#include <vector>

/*  Long-term spectrum: Hann-windowed FFT (about 0.34 s frames, 50 % overlap),
    power averaged over the whole file and the channels, then integrated into
    overlapping 1/6-octave bands centred on a 1/12-octave grid from 20 Hz to
    20.48 kHz. Band power, so pink noise reads flat. A sine of amplitude A
    reads 10*log10(A^2/2) in the band that holds it.
*/
namespace mihenk
{
/** Band centres: 20 Hz * 2^(i/12), i = 0..120. */
const std::vector<double>& spectrumGrid();

std::vector<float> longTermSpectrum (const juce::AudioBuffer<float>& buffer, double sampleRate);

/** Moving average over +-octaves/2 on the grid; the window narrows symmetrically at the ends, so a straight line stays straight. */
std::vector<float> smoothOctaves (const std::vector<float>& db, double octaves);

/** Value at frequency f, linear in log-frequency, clamped to the ends of the grid. */
double interpGrid (const std::vector<float>& db, double f);

/** Mean of db over grid points with lo <= f <= hi. */
double meanInRange (const std::vector<float>& db, double lo, double hi);
} // namespace mihenk
