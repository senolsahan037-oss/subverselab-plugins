#pragma once

#include <JuceHeader.h>
#include "../engine/Settings.h"

/*  The EQ is designed from its analogue magnitude response, not from bilinear
    biquads: every band's s-plane magnitude is evaluated at the exact bin
    frequency, so there is no cramping near Nyquist (a 16 kHz bell at 44.1 kHz
    has the same shape as on an analogue desk). From that magnitude we build
    either
      - a linear-phase FIR (zero-phase response, windowed; latency is removed
        offline so the output is time-aligned), or
      - a minimum-phase FIR (real-cepstrum method), which rings only after
        the transient, like an analogue EQ.
    The FIR is about 0.34 s long (16384 taps at 44.1/48 kHz, 32768 at 96 kHz)
    and is applied by FFT convolution.

    Band shapes (RBJ analogue prototypes): shelves with S = 1 (Q = 0.707),
    peaking bells with the given Q, a Butterworth low cut (12 or 24 dB/oct),
    and a tilt of N dB/oct around 1 kHz, held flat outside 20 Hz..20 kHz.
*/
namespace mihenk
{
/** Manual bands only, in dB, at frequency f. */
double eqManualDb (const EqSettings& s, double f);

/** Manual bands plus the tone-match curve (scaled by matchAmount). */
double eqTotalDb (const EqSettings& s, double f);

/** AIR shelf response in dB: 20 log10 (1 + (G - 1) p(f)), p = w / sqrt(1 + w^2), w = f / hz, G = 10^(gainDb / 20). */
double airShelfDb (double f, double gainDb, double hz);

/** True when the static EQ (the FIR) would not change the signal (so it can be skipped bit-exactly).
    In dynamic AIR mode the AIR shelf is not part of the FIR (processAirDynamic applies it). */
bool eqIsNeutral (const EqSettings& s);

/** True when a dynamic stage (dynamic bands, dynamic AIR) is active. */
bool dynamicEqActive (const EqSettings& s);

struct FirKernel
{
    std::vector<float> taps;
    int latency = 0;   // samples of delay to remove after convolution
};

int eqFirLength (double sampleRate);
/** Length of the dynamic AIR shaping FIR P: 1024 taps at 44.1 / 48 kHz, 2048 at 88.2 / 96 kHz (about 11 ms each side).
    p(f) is a smooth first-order shape, so a short linear-phase FIR holds it; it also keeps the real-time delay short. */
int airFirLength (double sampleRate);
FirKernel designEqFir (const EqSettings& s, double sampleRate);

/** Convolve every channel with the kernel (FFT overlap-add), remove the latency, keep the length. */
void convolveInPlace (juce::AudioBuffer<float>& buffer, const FirKernel& kernel);

/** The linear-phase FIR P of the dynamic AIR stage (magnitude p(f) = w / sqrt(1 + w^2), w = f / hz), as processAirDynamic uses it. */
FirKernel airPKernel (double hz, double sampleRate);

/*  Dynamic AIR (de-harsh). The detector compares the 5-9 kHz band with the rest of the signal (zero-phase
    Linkwitz-Riley split at 5 and 9 kHz, channel mean), as a 20 ms RMS ratio in dB. Its typical value is the median over the loud part; wherever the ratio stands
    more than kAirDeHarshMarginDb above that, the reduction r(t) is the excess (up to airDeHarshDb), with 2 ms attack
    and 60 ms release. r first eases the AIR boost (AIR gain = airDb - r, down to 0 dB) and what is left of it cuts
    the 5-9 kHz band itself (so with AIR at 0 dB this is a de-esser):
        y = x + (g_air(t) - 1) * P(x) - (1 - g_cut(t)) * B(x)
    P is a linear-phase FIR of magnitude p(f) (at a fixed gain exactly the static AIR shape), B the constant 0 dB
    peak band-pass of a 7 kHz bell, Q 1.2 (so the cut is exactly g_cut at 7 kHz, -3 dB points near 4.6 / 10.6 kHz). */
constexpr double kAirDeHarshMarginDb = 2.0, kAirDeHarshBellHz = 7000.0, kAirDeHarshBellQ = 1.2;
struct AirDynamicResult
{
    bool active = false;
    double typicalRelDb = 0.0;          // median 5-9 kHz / full-band ratio over the loud part
    double meanReductionDb = 0.0;       // mean r(t) over the loud part (AIR easing + band cut)
    double maxReductionDb = 0.0;
    double maxBandCutDb = 0.0;          // largest cut of the 5-9 kHz band
};
void processAirDynamic (juce::AudioBuffer<float>& buffer, double sampleRate, const EqSettings& s, const std::vector<uint8_t>& loudMask,
                        AirDynamicResult& result);

/** 5-9 kHz band level relative to the rest of the signal (zero-phase LR4 split), dB, per sample (20 ms RMS each), on the channel mean. */
std::vector<float> harshRatioDb (const juce::AudioBuffer<float>& buffer, double sampleRate);
} // namespace mihenk
