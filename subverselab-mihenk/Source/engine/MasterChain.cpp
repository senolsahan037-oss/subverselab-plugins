#include "MasterChain.h"
#include "../dsp/Equalizer.h"
#include "../dsp/Spectrum.h"
#include "../dsp/Filters.h"

namespace mihenk
{
juce::String MasterChain::preLimiterKey (const MasterSettings& s, double fs, int n)
{
    juce::String k;
    auto add = [&k] (double v) { k << juce::String (v, 6) << "|"; };
    add (fs); add (n); add (s.trimDb);
    add (s.dcFilter); add (s.balanceDb); add (s.autoTrim);
    const auto& e = s.eq;
    add (e.enabled); add (e.linearPhase); add (e.lowCutOn); add (e.lowCutHz); add (e.lowCutSlope);
    add (e.lowShelfDb); add (e.lowShelfHz); add (e.tiltDbPerOct); add (e.highShelfDb); add (e.highShelfHz);
    add (e.bell1Db); add (e.bell1Hz); add (e.bell1Q); add (e.bell2Db); add (e.bell2Hz); add (e.bell2Q); add (e.matchAmount);
    double h = 0.0;
    for (size_t i = 0; i < e.matchCurveDb.size(); ++i) h += (double) e.matchCurveDb[i] * (double) (i + 1) * 1.0001;
    add (h); add ((double) e.matchCurveDb.size());
    add (e.airDb); add (e.airHz); add (e.airDynamic); add (e.airDeHarshDb);
    for (const auto* d : { &e.dynLowMid, &e.dynUpperMid, &e.tightener })
    {
        add (d->enabled);
        if (d->enabled) { add (d->shape); add (d->freqHz); add (d->q); add (d->amountDb); add (d->rangeDb); add (d->ratio); add (d->attackMs); add (d->releaseMs); }
    }
    const auto& c = s.comp;
    // In AUTO the threshold is an output of the solver, not an input; in manual the glue target is unused.
    add (c.enabled); add (c.autoThreshold); add (c.autoThreshold ? c.glueDb : 0.0); add (c.autoThreshold ? 0.0 : c.thresholdDb); add (c.ratio); add (c.attackMs); add (c.releaseMs); add (c.autoRelease);
    add (c.rmsBlend); add (c.scHpfHz); add (c.mix); add (c.kneeDb); add (c.maxGrDb);
    add ((int) c.mode);
    if (c.mode == CompMode::threeBand)
    {
        add (c.xoverLowHz); add (c.xoverHighHz);
        for (const auto& b : c.bands)
        {
            add (b.bypass); add (b.solo); add (b.autoThreshold); add (b.autoThreshold ? b.glueDb : 0.0); add (b.autoThreshold ? 0.0 : b.thresholdDb);
            add (b.ratio); add (b.attackMs); add (b.releaseMs); add (b.gainDb);
        }
    }
    const auto& w = s.width;
    add (w.enabled); add (w.width); add (w.monoBelowHz); add (w.phaseSafeLowMono); add (w.highWidth); add (w.highWidthHz);
    return k;
}

bool isFinishedMaster (const SourceStats& src, double ceilingDbtp)
{
    return src.valid && std::isfinite (src.integrated) && std::isfinite (src.plr) && src.plr < kDensePlrLu
           && std::isfinite (src.samplePeakDb) && src.samplePeakDb >= ceilingDbtp - kFinishedHeadroomDb;
}

double MasterChain::effectiveTarget (const MasterSettings& s, const SourceStats& source)
{
    if (s.targetPreset == TargetPreset::keepInput)
        return source.valid && std::isfinite (source.integrated) ? std::round (source.integrated * 100.0) / 100.0 : s.targetLufs;
    return presetLufs (s.targetPreset, s.targetLufs);
}

LoudnessStory MasterChain::loudnessStory (const MasterSettings& s, double inL, double outL, double limGr)
{
    LoudnessStory st;
    st.inLufs = inL;
    st.outLufs = outL;
    if (! std::isfinite (inL)) return st;
    st.valid = true;
    const auto upper = [] (juce::String x) { return x.toUpperCase(); };
    if (s.bypass)
    {
        st.sentence = "BYPASS: you hear the mix as it is (" + fmt (inL) + " LUFS).";
        st.shortLine = "BYPASS: THE MIX AS IT IS, " + fmt (inL) + " LUFS";
        st.line1 = "BYPASS  " + fmt (inL) + " LUFS";
        st.line2 = "THE CHAIN IS OUT";
        return st;
    }
    if (! std::isfinite (outL))
    {
        st.sentence = "Your mix is " + fmt (inL) + " LUFS.";
        st.shortLine = "MIX " + fmt (inL) + " LUFS";
        st.line1 = st.shortLine;
        return st;
    }
    st.changeDb = outL - inL;
    const double d = st.changeDb, a = std::abs (d);
    juce::String name = presetName (s.targetPreset);
    if (s.targetPreset == TargetPreset::streaming) name = "STREAM -14";
    if (s.targetPreset == TargetPreset::custom) name = "CUSTOM " + fmt (s.targetLufs);
    juce::String platform, platformShort;
    switch (s.targetPreset)
    {
        case TargetPreset::streaming: platform = "Spotify and YouTube would do the same"; platformShort = "SPOTIFY WOULD TOO"; break;
        case TargetPreset::apple:     platform = "Apple Music would do the same"; platformShort = "APPLE MUSIC WOULD TOO"; break;
        default: break;
    }
    const juce::String lim = limGr > 0.05 ? fmt (limGr, 1) + " dB of peak limiting" : juce::String ("no peak limiting");
    if (! s.levelHeld())
    {
        st.sentence = "Drive set by hand (" + (s.limiter.driveDb >= 0.0 ? juce::String ("+") : juce::String()) + fmt (s.limiter.driveDb, 2)
                      + " dB): your mix is " + fmt (inL) + " LUFS, the master " + fmt (outL) + " LUFS (" + (d >= 0.0 ? "+" : "") + fmt (d, 1)
                      + " dB); the target is not held. Pick a target to hold it.";
        st.shortLine = "DRIVE BY HAND: " + fmt (inL) + " > " + fmt (outL) + " LUFS, TARGET NOT HELD";
        st.line1 = "MIX " + fmt (inL) + "  >  MASTER " + fmt (outL) + " LUFS";
        st.line2 = "DRIVE BY HAND: TARGET NOT HELD";
        return st;
    }
    if (s.targetPreset == TargetPreset::keepInput)
    {
        st.line1 = "MIX " + fmt (inL) + "  >  MASTER " + fmt (outL) + " LUFS";
        if (-d > 0.2)
        {
            // The mix peaks above the ceiling and is already limited: holding it would take real limiting, so it is not done.
            st.sentence = "KEEP INPUT: your mix is " + fmt (inL) + " LUFS; holding it under the " + fmt (s.limiter.ceilingDbtp, 1)
                          + " dBTP ceiling would take more limiting, so the master is " + fmt (outL) + " LUFS (the ceiling costs " + fmt (a, 1) + " dB; " + lim + ").";
            st.shortLine = "KEEP INPUT: " + fmt (inL) + " > " + fmt (outL) + ", THE CEILING COSTS " + fmt (a, 1) + " DB";
            st.line2 = "THE CEILING COSTS " + fmt (a, 1) + " DB (LIMITER " + fmt (limGr, 1) + " DB)";
            return st;
        }
        st.sentence = "KEEP INPUT holds your mix's " + fmt (inL) + " LUFS (master " + fmt (outL) + " LUFS); the limiter only guards the "
                      + fmt (s.limiter.ceilingDbtp, 1) + " dBTP ceiling (" + lim + ").";
        st.shortLine = "KEEP INPUT: " + fmt (inL) + " > " + fmt (outL) + " LUFS, CEILING ONLY";
        st.line2 = "HELD; LIMITER " + (limGr > 0.05 ? fmt (limGr, 1) + " DB, CEILING ONLY" : juce::String ("IDLE, CEILING ONLY"));
        return st;
    }
    st.line1 = "MIX " + fmt (inL) + "  >  MASTER " + fmt (outL) + " LUFS";
    if (a <= 0.3)
    {
        st.sentence = "Your mix is already at " + fmt (inL) + " LUFS; " + name + " keeps it there (" + lim + ").";
        st.shortLine = "MIX " + fmt (inL) + " LUFS: ALREADY ON " + upper (name);
        st.line2 = "ALREADY ON TARGET: NO CHANGE";
    }
    else if (d < 0.0)
    {
        st.sentence = "Your mix is " + fmt (inL) + " LUFS; " + name + " turns it down " + fmt (a, 1) + " dB"
                      + (platform.isNotEmpty() ? " (" + platform + ")" : juce::String()) + "; " + lim + ".";
        st.shortLine = "MIX " + fmt (inL) + " > " + fmt (outL) + ": TURNED DOWN " + fmt (a, 1) + " DB"
                       + (platformShort.isNotEmpty() ? " (" + platformShort + ")" : juce::String());
        st.line2 = "TURNED DOWN " + fmt (a, 1) + " DB" + (platformShort.isNotEmpty() ? " (" + platformShort + ")" : juce::String());
    }
    else
    {
        st.sentence = "Your mix is " + fmt (inL) + " LUFS; " + name + " turns it up " + fmt (a, 1) + " dB with " + lim + ".";
        st.shortLine = "MIX " + fmt (inL) + " > " + fmt (outL) + ": TURNED UP " + fmt (a, 1) + " DB, LIMITER " + fmt (limGr, 1) + " DB";
        st.line2 = "TURNED UP " + fmt (a, 1) + " DB, LIMITER " + fmt (limGr, 1) + " DB";
    }
    return st;
}

double MasterChain::toneDistanceDb (const std::vector<float>& a, const std::vector<float>& b)
{
    const auto& grid = spectrumGrid();
    const size_t n = juce::jmin (grid.size(), a.size(), b.size());
    if (n == 0) return 0.0;
    std::vector<float> d (grid.size(), 0.0f);
    for (size_t i = 0; i < n; ++i) d[i] = a[i] - b[i];
    const double off = meanInRange (d, 60.0, 12000.0);
    double ss = 0.0;
    int count = 0;
    for (size_t i = 0; i < n; ++i)
        if (grid[i] >= 60.0 && grid[i] <= 12000.0) { const double x = d[i] - off; ss += x * x; ++count; }
    return count > 0 ? std::sqrt (ss / count) : 0.0;
}

SourceStats MasterChain::measureSource (const juce::AudioBuffer<float>& in, double fs)
{
    SourceStats d;
    const auto L = measureLoudness (in, fs);
    double peak = 0.0, ss = 0.0;
    const int chans = in.getNumChannels(), n = in.getNumSamples();
    for (int ch = 0; ch < chans; ++ch)
    {
        const float* x = in.getReadPointer (ch);
        for (int i = 0; i < n; ++i) { peak = juce::jmax (peak, (double) std::abs (x[i])); ss += (double) x[i] * x[i]; }
    }
    d.valid = std::isfinite (L.integrated) && peak > 0.0 && ss > 0.0;
    if (d.valid)
    {
        d.samplePeakDb = gainToDb (peak);
        d.integrated = L.integrated;
        d.shortTermMax = L.shortTermMax;
        d.lra = L.lra;
        d.plr = d.samplePeakDb - L.integrated;
        d.crest = d.samplePeakDb - 10.0 * std::log10 (ss / ((double) n * chans));
    }
    return d;
}

InputStage headroomAssistant (const SourceStats& src)
{
    InputStage st;
    st.autoTrim = true;
    if (! src.valid)
    {
        st.message = "Headroom assistant: the input is silent, no pre-trim.";
        return st;
    }
    // The larger cut (or smaller lift) that puts the sample peak at or under -6 dBFS and the short-term max at or
    // under -14 LUFS; when the short-term max is not measurable (shorter than 3 s) the peak decides.
    double t = kWorkingPeakDbfs - src.samplePeakDb;
    if (std::isfinite (src.shortTermMax)) t = juce::jmin (t, kWorkingShortTermLufs - src.shortTermMax);
    st.autoTrimDb = juce::jlimit (-kMaxAutoTrimDb, kMaxAutoTrimDb, t);
    const auto where = "peak " + fmt (src.samplePeakDb, 1) + " dBFS, short-term max " + fmt (src.shortTermMax, 1) + " LUFS";
    const auto level = juce::String ("the working level (peak ") + fmt (kWorkingPeakDbfs, 0) + " dBFS / short-term max " + fmt (kWorkingShortTermLufs, 0) + " LUFS)";
    if (st.autoTrimDb < -0.05)
        st.message = "Input arrives " + fmt (-st.autoTrimDb, 1) + " dB hotter than " + level + ": " + where + "; pre-trim " + fmt (st.autoTrimDb, 1) + " dB.";
    else if (st.autoTrimDb > 0.05)
        st.message = "Input arrives " + fmt (st.autoTrimDb, 1) + " dB under " + level + ": " + where + "; pre-gain +" + fmt (st.autoTrimDb, 1) + " dB.";
    else
        st.message = "Input is at " + level + ": " + where + "; no pre-trim.";
    return st;
}

namespace
{
using BufferPtr = std::shared_ptr<const juce::AudioBuffer<float>>;
BufferPtr copyOf (const juce::AudioBuffer<float>& b) { return std::make_shared<const juce::AudioBuffer<float>> (b); }

double meanSquare (const juce::AudioBuffer<float>& b)
{
    double ss = 0.0;
    for (int c = 0; c < b.getNumChannels(); ++c)
    {
        const float* x = b.getReadPointer (c);
        for (int i = 0; i < b.getNumSamples(); ++i) ss += (double) x[i] * x[i];
    }
    return ss / juce::jmax (1.0, (double) b.getNumSamples() * b.getNumChannels());
}

/** 3-BAND compressor on `buf` (in place). `levels` are the bands' detector levels (computed when `fresh`). */
void processThreeBand (juce::AudioBuffer<float>& buf, double fs, const CompSettings& comp, const std::vector<uint8_t>& mask,
                       std::vector<float> (&levels)[3], bool fresh, GlueAnalysis& glue, GainReductionStats& stats)
{
    const int chans = buf.getNumChannels(), n = buf.getNumSamples();
    juce::AudioBuffer<float> band[3];
    splitThreeBands (buf, fs, comp.xoverLowHz, comp.xoverHighHz, band[0], band[1], band[2]);
    const double levelBefore = integratedLoudness (buf, fs);
    CompSettings det = comp;
    det.scHpfHz = 0.0;
    bool anySolo = false;
    for (const auto& b : comp.bands) anySolo |= b.solo;
    std::vector<float> gr, worst ((size_t) n, 0.0f);
    double loudWeighted = 0.0, loudEnergy = 0.0;
    glue.threeBand = true;
    glue.xoverLowHz = comp.xoverLowHz;
    glue.xoverHighHz = comp.xoverHighHz;
    glue.autoThreshold = false;
    glue.loudMaxGrDb = 0.0;
    for (int b = 0; b < 3; ++b)
    {
        const auto& bs = comp.bands[b];
        auto& ba = glue.bands[b];
        ba = {};
        ba.solo = bs.solo;
        if (fresh || (int) levels[b].size() != n) compressorDetector (band[b], fs, det, levels[b]);
        const double eBefore = meanSquare (band[b]);
        // Loud-part energy of the band, for the stage's weighted GR.
        double le = 0.0;
        for (int c = 0; c < chans; ++c)
        {
            const float* x = band[b].getReadPointer (c);
            for (int i = 0; i < n; ++i) if (mask[(size_t) i]) le += (double) x[i] * x[i];
        }
        if (! bs.bypass)
        {
            CompSettings c = comp;
            c.ratio = bs.ratio; c.attackMs = bs.attackMs; c.releaseMs = bs.releaseMs; c.thresholdDb = bs.thresholdDb; c.maxGrDb = 0.0;
            ba.targetGrDb = juce::jlimit (0.0, 4.0, bs.glueDb);
            if (bs.autoThreshold && glue.valid)
            {
                glue.autoThreshold = true;
                // Per band only the peak criteria count (PLR, crest): a low LRA alone is a steady arrangement,
                // not a squashed low band (the diagnosis' over-compressed rule reads it the same way).
                if (glue.plr < kDensePlrLu || glue.crest < kDenseCrestDb)
                {
                    ba.targetGrDb = juce::jmin (kDenseGlueDb, ba.targetGrDb);
                    c.ratio = juce::jmin (kDenseRatio, c.ratio);
                }
                int its = 0;
                c.thresholdDb = solveGlueThreshold (levels[b], mask, fs, c, ba.targetGrDb, &its);
                glue.solverIterations += its;
            }
            compressorGainDb (levels[b], fs, c, gr);
            const double mix = juce::jlimit (0.0, 1.0, comp.mix);
            std::vector<float> gain ((size_t) n);
            for (int i = 0; i < n; ++i)
            {
                const double g = std::pow (10.0, (double) gr[(size_t) i] / 20.0);
                gain[(size_t) i] = (float) g;
                worst[(size_t) i] = juce::jmin (worst[(size_t) i], gr[(size_t) i]);
                const float k = (float) (1.0 - mix + mix * g);
                for (int ch = 0; ch < chans; ++ch) band[b].getWritePointer (ch)[i] *= k;
            }
            maskedGrStats (gr, mask, ba.loudMeanGrDb, ba.loudMaxGrDb);
            ba.gr = gainStats (gain, fs);
            ba.active = true;
            ba.thresholdDb = c.thresholdDb;
            ba.ratio = c.ratio;
            const double eAfter = meanSquare (band[b]);
            if (eBefore > 0.0 && eAfter > 0.0) ba.makeupDb = juce::jlimit (0.0, 24.0, 10.0 * std::log10 (eBefore / eAfter));
        }
        loudWeighted += le * ba.loudMeanGrDb;
        loudEnergy += le;
        glue.loudMaxGrDb = juce::jmax (glue.loudMaxGrDb, ba.loudMaxGrDb);
    }
    glue.loudMeanGrDb = loudEnergy > 0.0 ? loudWeighted / loudEnergy : 0.0;
    glue.dense = glue.autoThreshold && glue.valid && (glue.plr < kDensePlrLu || glue.crest < kDenseCrestDb);
    glue.ratio = 0.0;
    glue.targetGrDb = 0.0;
    glue.thresholdDb = 0.0;

    // Level-neutral: each band's energy is restored, then the remaining loudness change of the neutral sum is
    // corrected, so compression changes feel, not level. Band gains and solo come after that correction.
    juce::AudioBuffer<float> sum (chans, n);
    sum.clear();
    for (int b = 0; b < 3; ++b)
        for (int c = 0; c < chans; ++c) sum.addFrom (c, 0, band[b], c, 0, n, (float) dbToGain (glue.bands[b].makeupDb));
    const double levelAfter = integratedLoudness (sum, fs);
    glue.makeupDb = (std::isfinite (levelBefore) && std::isfinite (levelAfter)) ? levelBefore - levelAfter : 0.0;
    buf.clear();
    for (int b = 0; b < 3; ++b)
    {
        if (anySolo && ! comp.bands[b].solo) continue;
        const float g = (float) dbToGain (glue.bands[b].makeupDb + glue.makeupDb + juce::jlimit (-12.0, 12.0, comp.bands[b].gainDb));
        for (int c = 0; c < chans; ++c) buf.addFrom (c, 0, band[b], c, 0, n, g);
    }
    std::vector<float> wg ((size_t) n);
    for (int i = 0; i < n; ++i) wg[(size_t) i] = (float) std::pow (10.0, (double) worst[(size_t) i] / 20.0);
    stats = gainStats (wg, fs);
}
} // namespace

void MasterChain::renderPreLimiter (const juce::AudioBuffer<float>& in, double fs, const MasterSettings& s, PreLimiterCache& cache)
{
    const auto inId = juce::String::toHexString ((juce::pointer_sized_int) in.getReadPointer (0)) + "|";
    const auto key = inId + preLimiterKey (s, fs, in.getNumSamples()) + (cache.keepTaps ? "T" : "");
    if (cache.buffer != nullptr && cache.key == key)
        return;

    // Source statistics: a property of the mix as it arrives (trim does not change PLR, crest or LRA).
    const auto densityKey = inId + juce::String (in.getNumSamples()) + "|" + juce::String (fs);
    if (cache.densityKey != densityKey)
    {
        cache.source = measureSource (in, fs);
        GlueAnalysis d;
        d.valid = cache.source.valid;
        if (d.valid)
        {
            d.plr = cache.source.plr;
            d.crest = cache.source.crest;
            d.lra = cache.source.lra;
            juce::StringArray why;
            if (d.plr < kDensePlrLu) why.add ("PLR " + fmt (d.plr) + " LU");
            if (d.crest < kDenseCrestDb) why.add ("crest " + fmt (d.crest) + " dB");
            if (d.lra < kDenseLraLu) why.add ("LRA " + fmt (d.lra) + " LU");
            d.dense = ! why.isEmpty();
            d.denseReason = why.joinIntoString (", ");
        }
        cache.densityKey = densityKey;
        cache.density = d;
    }

    auto buf = std::make_shared<juce::AudioBuffer<float>> (in);
    StageTaps taps;

    // Input stage: DC filter, balance, trim (+ headroom assistant).
    InputStage input;
    if (s.autoTrim) input = headroomAssistant (cache.source);
    input.effectiveTrimDb = s.trimDb + input.autoTrimDb;
    if (s.dcFilter) dcFilter (*buf, fs);
    applyBalance (*buf, s.balanceDb);
    if (std::abs (input.effectiveTrimDb) > 1.0e-9)
        buf->applyGain ((float) dbToGain (input.effectiveTrimDb));

    // Static EQ (FIR).
    if (s.eq.enabled && ! eqIsNeutral (s.eq))
        convolveInPlace (*buf, designEqFir (s.eq, fs));
    if (cache.keepTaps) taps.afterEq = copyOf (*buf);

    // Loud part of the post-EQ signal: the reference for every AUTO solver (dynamic EQ, glue, bands).
    const bool dynOn = s.eq.enabled && dynamicEqActive (s.eq);
    if (s.comp.enabled || dynOn)
    {
        MasterSettings ms = s;
        ms.comp = CompSettings{};
        ms.width = WidthSettings{};
        ms.eq.dynLowMid.enabled = ms.eq.dynUpperMid.enabled = ms.eq.tightener.enabled = false;
        const auto mk = inId + preLimiterKey (ms, fs, in.getNumSamples());
        if (cache.maskKey != mk || (int) cache.loudMask.size() != buf->getNumSamples())
        {
            cache.loudMask = loudPartMask (measureLoudness (*buf, fs).shortTerm, buf->getNumSamples(), fs, 0.7);
            cache.maskKey = mk;
            cache.detectorKey.clear();
            cache.bandDetectorKey.clear();
        }
    }

    // Dynamic EQ.
    DynamicEqAnalysis dyn;
    if (dynOn)
    {
        processDynBand (*buf, fs, s.eq.tightener, cache.loudMask, dyn.tightener);
        processDynBand (*buf, fs, s.eq.dynLowMid, cache.loudMask, dyn.lowMid);
        processDynBand (*buf, fs, s.eq.dynUpperMid, cache.loudMask, dyn.upperMid);
        processAirDynamic (*buf, fs, s.eq, cache.loudMask, dyn.air);
    }
    if (cache.keepTaps) taps.afterDynamicEq = dynOn ? copyOf (*buf) : taps.afterEq;

    GainReductionStats comp;
    GlueAnalysis glue;
    if (s.comp.enabled)
    {
        auto c = s.comp;
        glue = cache.density;
        // Detector levels depend only on the signal entering the compressor and the detector settings,
        // so ratio / attack / release / glue moves reuse them.
        MasterSettings ks = s;          // key of everything before the compressor + detector settings only
        ks.comp = CompSettings{};
        ks.comp.scHpfHz = s.comp.scHpfHz;
        ks.comp.rmsBlend = s.comp.rmsBlend;
        ks.width = WidthSettings{};
        if (c.mode == CompMode::threeBand)
        {
            ks.comp.mode = CompMode::threeBand;
            ks.comp.xoverLowHz = c.xoverLowHz;
            ks.comp.xoverHighHz = c.xoverHighHz;
            for (auto& b : ks.comp.bands) b = BandSettings{};
            const auto dk = inId + preLimiterKey (ks, fs, in.getNumSamples());
            const bool fresh = cache.bandDetectorKey != dk;
            processThreeBand (*buf, fs, c, cache.loudMask, cache.bandLevel, fresh, glue, comp);
            cache.bandDetectorKey = dk;
        }
        else
        {
            const auto dk = inId + preLimiterKey (ks, fs, in.getNumSamples());
            if (cache.detectorKey != dk || (int) cache.level.size() != buf->getNumSamples())
            {
                compressorDetector (*buf, fs, c, cache.level);
                cache.detectorKey = dk;
            }
            glue.autoThreshold = c.autoThreshold;
            if (c.autoThreshold && glue.valid)
            {
                glue.targetGrDb = juce::jlimit (0.0, 4.0, c.glueDb);
                if (glue.dense)
                {
                    glue.targetGrDb = juce::jmin (kDenseGlueDb, glue.targetGrDb);
                    c.ratio = juce::jmin (kDenseRatio, c.ratio);
                }
                c.thresholdDb = solveGlueThreshold (cache.level, cache.loudMask, fs, c, glue.targetGrDb, &glue.solverIterations);
            }
            else
            {
                glue.dense = false;          // manual threshold: nothing was reduced
            }
            glue.ratio = c.ratio;
            glue.thresholdDb = c.thresholdDb;

            const double levelBefore = integratedLoudness (*buf, fs);
            std::vector<float> grDb;
            processCompressor (*buf, fs, c, comp, &grDb, &cache.level);
            maskedGrStats (grDb, cache.loudMask, glue.loudMeanGrDb, glue.loudMaxGrDb);
            // Level-neutral: make-up equals the loudness the stage took, so compression changes feel, not level.
            const double levelAfter = integratedLoudness (*buf, fs);
            if (std::isfinite (levelBefore) && std::isfinite (levelAfter))
            {
                glue.makeupDb = levelBefore - levelAfter;
                buf->applyGain ((float) dbToGain (glue.makeupDb));
            }
        }
    }
    if (cache.keepTaps) taps.afterCompressor = s.comp.enabled ? copyOf (*buf) : taps.afterDynamicEq;
    WidthAnalysis wa;
    processWidth (*buf, fs, s.width, nullptr, &wa.safeSign, &wa.safeGain);

    cache.key = key;
    cache.width = wa;
    cache.buffer = buf;
    cache.truePeakEnv.clear();
    cache.comp = comp;
    cache.glue = glue;
    cache.input = input;
    cache.dynEq = dyn;
    cache.taps = taps;
    cache.integratedPre = integratedLoudness (*buf, fs);
}

void MasterChain::storeSolvedThresholds (MasterSettings& s, const GlueAnalysis& g)
{
    if (! g.valid) return;
    if (g.threeBand)
    {
        for (int b = 0; b < 3; ++b)
            if (s.comp.bands[b].autoThreshold && g.bands[b].active) s.comp.bands[b].thresholdDb = g.bands[b].thresholdDb;
    }
    else if (s.comp.autoThreshold)
        s.comp.thresholdDb = g.thresholdDb;
}

RenderResult MasterChain::render (const juce::AudioBuffer<float>& in, double fs, const MasterSettings& s,
                                  PreLimiterCache& cache, const std::atomic<bool>* cancel)
{
    RenderResult r;
    r.driveDb = s.limiter.driveDb;
    if (s.bypass)
    {
        r.output = std::make_shared<juce::AudioBuffer<float>> (in);
        r.measured = analyse (*r.output, fs);
        r.limiter.truePeakOut = dbToGain (r.measured.truePeak);
        return r;
    }
    renderPreLimiter (in, fs, s, cache);
    if (cancel != nullptr && cancel->load()) { r.cancelled = true; return r; }

    r.output = std::make_shared<juce::AudioBuffer<float>>();
    if (s.limiter.enabled)
    {
        if (cache.truePeakEnv.empty())
            cache.truePeakEnv = truePeakEnvelope (*cache.buffer, kLimiterOversampling);
        if (cancel != nullptr && cancel->load()) { r.cancelled = true; return r; }
        r.limiter = limitWithSafety (*cache.buffer, cache.truePeakEnv, fs, s.limiter, *r.output);
    }
    else
    {
        *r.output = *cache.buffer;
        if (std::abs (s.limiter.driveDb) > 1.0e-9)
            r.output->applyGain ((float) dbToGain (s.limiter.driveDb));
    }
    if (cancel != nullptr && cancel->load()) { r.cancelled = true; return r; }
    r.measured = analyse (*r.output, fs);
    if (! s.limiter.enabled) r.limiter.truePeakOut = dbToGain (r.measured.truePeak);
    r.comp = cache.comp;
    r.glue = cache.glue;
    r.density = cache.density;
    r.input = cache.input;
    r.dynEq = cache.dynEq;
    r.width = cache.width;
    r.taps = cache.taps;
    r.preLimiter = cache.buffer;
    if (cache.wantProxy && r.measured.valid && (cache.truePeakEnv.size() == (size_t) cache.buffer->getNumSamples() || ! s.limiter.enabled))
    {
        auto proxy = std::make_shared<LoudnessProxy> (makeLoudnessProxy (*cache.buffer, s.limiter.enabled ? cache.truePeakEnv : std::vector<float>(), fs));
        if (proxy->valid)
        {
            proxy->baselineDriveDb = s.limiter.driveDb;
            proxy->baselineLufs = r.measured.integrated;
            proxy->ceilingDb = s.limiter.ceilingDbtp;
            proxy->releaseMs = s.limiter.releaseMs;
            proxy->limiterOn = s.limiter.enabled;
            if (s.limiter.enabled)
            {
                // One real render well above the baseline drive tells how the real limiter recovers between peaks (the model alone is too quiet when the limiter works hard).
                juce::AudioBuffer<float> scratch;
                std::vector<float> g;
                const double probe = juce::jmin (36.0, s.limiter.driveDb + 18.0);
                if (probe > s.limiter.driveDb + 3.0)
                {
                    applyDriveAndLimit (*cache.buffer, cache.truePeakEnv, fs, s.limiter, probe, scratch, g);
                    fitProxyRecovery (*proxy, probe, integratedLoudness (scratch, fs));
                }
            }
            r.proxy = proxy;
        }
    }
    if (s.targetPreset == TargetPreset::keepInput)
    {
        auto keep = s;
        keep.targetLufs = effectiveTarget (s, cache.source);
        r.warnings = warningsFor (keep, r.measured, r.limiter);
    }
    else
        r.warnings = warningsFor (s, r.measured, r.limiter);
    if (r.input.autoTrim && std::abs (r.input.autoTrimDb) > 0.05) r.warnings.add (r.input.message);
    if (r.glue.valid && r.glue.autoThreshold && r.glue.dense)
        r.warnings.add ("Input already dense: glue reduced to " + fmt (r.glue.threeBand ? kDenseGlueDb : r.glue.targetGrDb, 1) + " dB ("
                        + r.glue.denseReason.upToFirstOccurrenceOf (",", false, false) + ")");   // short enough for the VFD line; the report has all reasons
    return r;
}

RenderResult MasterChain::renderLocked (const juce::AudioBuffer<float>& in, double fs, MasterSettings& s, PreLimiterCache& cache,
                                        const std::atomic<bool>* cancel, MatchResult* match)
{
    RenderResult r;
    if (s.targetLock && ! s.bypass)
    {
        const auto m = matchTarget (in, fs, s, cache, &r, cancel);
        if (match != nullptr) *match = m;
        if (r.output == nullptr)
        {
            if (cancel != nullptr && cancel->load()) r.cancelled = true;
            else r = render (in, fs, s, cache, cancel);     // silent input: nothing to solve, show it as it is
        }
    }
    else
        r = render (in, fs, s, cache, cancel);
    if (! r.cancelled) storeSolvedThresholds (s, r.glue);
    if (! r.cancelled && s.targetPreset == TargetPreset::keepInput && cache.source.valid) s.targetLufs = effectiveTarget (s, cache.source);
    return r;
}

MatchResult MasterChain::matchTarget (const juce::AudioBuffer<float>& in, double fs, MasterSettings& s, PreLimiterCache& cache,
                                      RenderResult* finalRender, const std::atomic<bool>* cancel)
{
    MatchResult m;
    if (s.bypass)
    {
        m.message = "BYPASS is on; the chain is out, nothing to match.";
        return m;
    }
    renderPreLimiter (in, fs, s, cache);
    // KEEP INPUT: the target is the source's own loudness, so the limiter only holds the ceiling.
    if (s.targetPreset == TargetPreset::keepInput && cache.source.valid) s.targetLufs = effectiveTarget (s, cache.source);
    const double T = s.targetLufs, Lpre = cache.integratedPre;
    if (! std::isfinite (Lpre))
    {
        m.message = "The signal is silent (below the -70 LUFS gate); nothing to match.";
        return m;
    }
    // KEEP INPUT asks the limiter only for the ceiling: at most kKeepMaxExtraDriveDb more drive than the level-neutral
    // one (output loudness = input loudness before limiting). A mix already limited above the ceiling then ends a little
    // under its own loudness instead of being crushed to hold it.
    const bool keep = s.targetPreset == TargetPreset::keepInput;
    const double minDrive = -36.0;
    const double userCap = juce::jlimit (minDrive, 36.0, s.limiter.maxDriveDb);          // LEARN that heard too little holds the drive down
    const bool userCapped = userCap < 36.0 - 1.0e-9 && ! keep;
    const double maxDrive = keep ? juce::jlimit (minDrive, 36.0, T - Lpre + kKeepMaxExtraDriveDb) : userCap;
    // A finished master is never pushed hard to reach a louder target (KEEP INPUT has its own rule: never louder than the mix).
    const bool finished = ! keep && s.limiter.enabled && isFinishedMaster (cache.source, s.limiter.ceilingDbtp);
    const double srcLufs = cache.source.integrated;
    bool guarded = false;                // the finished-master guard held the drive back
    double guardGrMax = 0.0, guardBoost = 0.0;
    double drive = juce::jlimit (minDrive, maxDrive, T - Lpre);

    if (s.limiter.enabled)
    {
        if (cache.truePeakEnv.empty())
            cache.truePeakEnv = truePeakEnvelope (*cache.buffer, kLimiterOversampling);
        juce::AudioBuffer<float> scratch;
        std::vector<float> g;
        double lastGrMax = 0.0, lastGrMean = 0.0, lastLufs = kNegInf;
        auto eval = [&] (double d)
        {
            ++m.iterations;
            applyDriveAndLimit (*cache.buffer, cache.truePeakEnv, fs, s.limiter, d, scratch, g);
            lastLufs = integratedLoudness (scratch, fs);
            if (finished)
            {
                float gMin = 1.0f;
                double sum = 0.0;
                for (float v : g) { gMin = juce::jmin (gMin, v); sum += v >= 1.0f ? 0.0 : -20.0 * std::log10 (juce::jmax (1.0e-9f, v)); }
                lastGrMax = gMin >= 1.0f ? 0.0 : -20.0 * std::log10 (juce::jmax (1.0e-9f, gMin));
                lastGrMean = g.empty() ? 0.0 : sum / (double) g.size();
            }
            return lastLufs - T;
        };

        // Bracket the root. More drive never makes the output quieter.
        double lo = drive, flo = eval (drive), hi = drive, fhi = flo;
        if (flo < 0.0)
        {
            double step = juce::jmax (0.5, -flo);
            while (fhi < 0.0 && hi < maxDrive)
            {
                lo = hi; flo = fhi;
                hi = juce::jmin (maxDrive, hi + step);
                fhi = eval (hi);
                step *= 2.0;
                if (cancel != nullptr && cancel->load()) return m;
            }
        }
        else
        {
            double step = juce::jmax (0.5, flo);
            while (flo > 0.0 && lo > minDrive)
            {
                hi = lo; fhi = flo;
                lo = juce::jmax (minDrive, lo - step);
                flo = eval (lo);
                step *= 2.0;
            }
        }
        if (fhi < 0.0 && keep)
        {
            drive = maxDrive;
            m.message = "KEEP";   // written after the verification render below
        }
        else if (fhi < 0.0 && userCapped)
        {
            drive = maxDrive;
            m.message = "Target not reachable: the drive is held at +" + juce::String (maxDrive, 1) + " dB because LEARN heard only a short part of the song, which may be a quiet part"
                        " (achieved " + fmt (fhi + T) + " LUFS). Play the whole song through LEARN to solve the real level.";
        }
        else if (fhi < 0.0)
        {
            // Do not keep adding drive after the limiter has flattened the
            // loudness curve.  Back off one dB when its marginal loudness
            // gain is below the audible/useful 0.1 LU threshold.
            drive = maxDrive;
            double bestLufs = fhi + T;
            if (maxDrive - 1.0 >= minDrive)
            {
                const double previous = eval (maxDrive - 1.0) + T;
                if (bestLufs - previous < 0.1)
                {
                    drive = maxDrive - 1.0;
                    bestLufs = previous;
                }
            }
            m.message = "Target not reachable: backed off to +" + juce::String (drive, 0)
                        + " dB at the point of diminishing returns (achieved " + fmt (bestLufs) + " LUFS; +1 dB adds < 0.1 LU).";
        }
        else if (flo > 0.0)
        {
            drive = minDrive;
            m.message = "Target not reachable: " + fmt (flo + T) + " LUFS even at " + juce::String (minDrive, 0) + " dB.";
        }
        else
        {
            // Illinois regula falsi.
            int side = 0;
            drive = lo;
            for (int it = 0; it < 30; ++it)
            {
                const double d = (fhi - flo) > 1.0e-12 ? hi - fhi * (hi - lo) / (fhi - flo) : 0.5 * (lo + hi);
                const double f = eval (d);
                drive = d;
                if (std::abs (f) < 0.02 || hi - lo < 1.0e-4) break;
                if (f < 0.0) { lo = d; flo = f; if (side == -1) fhi *= 0.5; side = -1; }
                else         { hi = d; fhi = f; if (side == +1) flo *= 0.5; side = +1; }
                if (cancel != nullptr && cancel->load()) return m;
            }
        }

        // Finished-master guard: the drive the target asks for may cost more than a finished master can take (boost over the
        // mix, peak and mean limiting). Hold it at the largest drive that stays inside, and say the target is not reachable.
        if (finished)
        {
            const double peakCap = kFinishedMaxPeakGrDb - 0.1, meanCap = kFinishedMaxMeanGrDb - 0.1, boostCap = kFinishedMaxBoostDb - 0.05;   // the safety passes add a little
            auto within = [&] (double d)
            {
                eval (d);
                return lastGrMax <= peakCap && lastGrMean <= meanCap && lastLufs - srcLufs <= boostCap;
            };
            if (! within (drive))
            {
                double lo = minDrive, hi = drive;
                for (int it = 0; it < 14 && hi - lo > 0.01; ++it)
                {
                    const double mid = 0.5 * (lo + hi);
                    if (within (mid)) lo = mid; else hi = mid;
                    if (cancel != nullptr && cancel->load()) return m;
                }
                drive = lo;
                eval (drive);
                guarded = true;
                guardGrMax = lastGrMax;
                guardBoost = lastLufs - srcLufs;
                m.message = "Target not reachable: this master is already finished (" + fmt (cache.source.plr, 1) + " LU peak-to-loudness, no headroom). "
                            + fmt (T) + " LUFS would cost more than " + fmt (kFinishedMaxBoostDb, 0) + " dB of boost or " + fmt (kFinishedMaxPeakGrDb, 0)
                            + " dB of limiting; held at " + fmt (lastLufs) + " LUFS (+" + fmt (juce::jmax (0.0, guardBoost), 1) + " dB, " + fmt (guardGrMax, 1) + " dB peak limiting).";
            }
        }
    }

    // Verify with the real render (safety passes included) and correct the small residue.
    RenderResult r;
    for (int pass = 0; pass < (guarded ? 8 : 4); ++pass)
    {
        s.limiter.driveDb = drive;
        r = render (in, fs, s, cache, cancel);
        if (r.cancelled) return m;
        if (guarded)
        {
            // The real render includes the limiter's safety passes: step down until it is inside the guard too.
            const double over = juce::jmax (r.limiter.gr.maxDb - kFinishedMaxPeakGrDb, juce::jmax (r.limiter.gr.meanDb - kFinishedMaxMeanGrDb,
                                                                                                r.measured.integrated - srcLufs - kFinishedMaxBoostDb));
            if (over <= 0.0) break;
            drive = juce::jmax (minDrive, drive - (over + 0.02));
            continue;
        }
        const double err = T - r.measured.integrated;
        if (std::abs (err) <= 0.05 || ! m.message.isEmpty()) break;
        drive = juce::jlimit (minDrive, maxDrive, drive + err);
    }
    if (guarded && std::abs (s.limiter.driveDb - drive) > 1.0e-12)
    {
        s.limiter.driveDb = drive;                      // the loop ended on a step that was not rendered
        r = render (in, fs, s, cache, cancel);
        if (r.cancelled) return m;
    }
    m.driveDb = s.limiter.driveDb;
    m.achievedLufs = r.measured.integrated;
    if (m.message == "KEEP")
        m.message = "KEEP INPUT: holding " + fmt (T) + " LUFS under the " + fmt (s.limiter.ceilingDbtp, 1) + " dBTP ceiling would take more than "
                    + fmt (kKeepMaxExtraDriveDb, 0) + " dB of extra limiting; held at " + fmt (r.measured.integrated) + " LUFS (the ceiling costs "
                    + fmt (T - r.measured.integrated, 1) + " dB).";
    m.truePeak = r.measured.truePeak;
    m.maxLimitingDb = r.limiter.gr.maxDb;
    m.reached = std::abs (m.achievedLufs - T) <= 0.2;
    if (m.message.isEmpty())
    {
        m.message = "Target " + fmt (T) + " LUFS: achieved " + fmt (m.achievedLufs, 2) + " LUFS, true peak " + fmt (m.truePeak, 2)
                    + " dBTP, drive " + fmt (m.driveDb, 2) + " dB, peak limiting " + fmt (m.maxLimitingDb, 1) + " dB.";
        if (! m.reached) m.message << " Missed by more than 0.2 LU.";
        if (m.maxLimitingDb > s.limitWarnDb)
            m.message << " WARNING: more than " << fmt (s.limitWarnDb, 1) << " dB of limiting.";
    }
    if (finalRender != nullptr) *finalRender = r;
    return m;
}

std::vector<float> MasterChain::toneMatchCurve (const std::vector<float>& src, const std::vector<float>& ref,
                                                const EqSettings& manual, double smoothOct)
{
    const auto& grid = spectrumGrid();
    const size_t n = juce::jmin (grid.size(), src.size(), ref.size());
    std::vector<float> diff (grid.size(), 0.0f);
    for (size_t i = 0; i < n; ++i)
        diff[i] = ref[i] - (float) (src[i] + eqManualDb (manual, grid[i]));
    const float offset = (float) meanInRange (diff, 60.0, 12000.0);
    for (auto& d : diff) d -= offset;
    auto curve = smoothOctaves (diff, smoothOct);
    for (size_t i = 0; i < curve.size(); ++i)
    {
        const double f = grid[i];
        double w = 1.0;
        if (f < 40.0) w = 0.5 - 0.5 * std::cos (kPi * juce::jlimit (0.0, 1.0, std::log2 (f / 20.0)));
        if (f > 14000.0) w = 0.5 + 0.5 * std::cos (kPi * juce::jlimit (0.0, 1.0, std::log2 (f / 14000.0) / std::log2 (20480.0 / 14000.0)));
        curve[i] = (float) (juce::jlimit (-6.0, 6.0, (double) curve[i]) * w);
    }
    return curve;
}

std::vector<float> MasterChain::differenceCurve (const std::vector<float>& src, const std::vector<float>& ref, const EqSettings* eq)
{
    const auto& grid = spectrumGrid();
    const size_t n = juce::jmin (grid.size(), src.size(), ref.size());
    std::vector<float> diff (n);
    for (size_t i = 0; i < n; ++i)
        diff[i] = ref[i] - (float) (src[i] + (eq != nullptr && eq->enabled ? eqTotalDb (*eq, grid[i]) : 0.0));
    const float offset = (float) meanInRange (diff, 60.0, 12000.0);
    for (auto& d : diff) d -= offset;
    return diff;
}

juce::StringArray MasterChain::warningsFor (const MasterSettings& s, const Measurement& out, const LimiterResult& lim)
{
    juce::StringArray w;
    if (s.bypass) return w;
    if (s.limiter.enabled && lim.gr.maxDb > s.limitWarnDb)
        w.add ("Limiter peak reduction " + fmt (lim.gr.maxDb, 1) + " dB is above the " + fmt (s.limitWarnDb, 1)
               + " dB warning level: this loudness is costing transients.");
    if (lim.finalTrimDb < -1.0e-6)
        w.add ("The limiter needed a final trim of " + fmt (lim.finalTrimDb, 2) + " dB to hold the ceiling.");
    if (out.valid && std::isfinite (out.integrated) && std::abs (out.integrated - s.targetLufs) > 0.2)
        w.add ("Output " + fmt (out.integrated) + " LUFS is " + fmt (std::abs (out.integrated - s.targetLufs)) + " LU "
               + (out.integrated < s.targetLufs ? "below" : "above") + " the " + fmt (s.targetLufs) + " target.");
    if (out.valid && out.truePeak > s.limiter.ceilingDbtp + 0.05)
        w.add ("True peak " + fmt (out.truePeak, 2) + " dBTP is over the " + fmt (s.limiter.ceilingDbtp, 1) + " dBTP ceiling"
               + (s.limiter.enabled ? "." : " (limiter is off)."));
    if (out.valid && out.samplePeak >= -0.001)
        w.add ("Sample peak at 0 dBFS: the export will clip.");
    if (out.valid && out.channels >= 2 && out.correlation < -0.1)
        w.add ("Negative stereo correlation (" + fmt (out.correlation, 2) + "): check mono compatibility.");
    return w;
}

juce::String MasterChain::report (const juce::String& sourceName, const juce::String& outName, int bits, const MasterSettings& s,
                                  const Measurement& before, const Measurement& after, const RenderResult& r, const Measurement* ref)
{
    juce::String t;
    auto line = [&t] (const juce::String& x) { t << x << "\n"; };
    auto row = [&] (const juce::String& name, double a, double b, const juce::String& unit, int dec = 1)
    {
        line (name.paddedRight (' ', 22) + fmt (a, dec).paddedLeft (' ', 9) + fmt (b, dec).paddedLeft (' ', 10) + "  " + unit);
    };
    line ("MIHENK MH-86  -  mastering report");
    line ("SubverseLab  -  we measure, we don't guess");
    line (juce::String::repeatedString ("=", 60));
    line ("Date        " + juce::Time::getCurrentTime().toString (true, true, true, true));
    line ("Source      " + sourceName);
    line ("Master      " + outName);
    line ("Format      WAV " + juce::String (bits) + "-bit, " + juce::String (after.sampleRate, 0) + " Hz, "
          + juce::String (after.channels) + " ch" + (bits == 16 ? ", TPDF dither (+-1 LSB, no noise shaping)" : ", no dither"));
    line ("Length      " + juce::String (after.seconds, 2) + " s");
    line ("");
    line (juce::String ("MEASUREMENT").paddedRight (' ', 22) + juce::String ("INPUT").paddedLeft (' ', 9) + juce::String ("OUTPUT").paddedLeft (' ', 10));
    row ("Integrated", before.integrated, after.integrated, "LUFS");
    row ("Short-term max", before.shortTermMax, after.shortTermMax, "LUFS");
    row ("Momentary max", before.momentaryMax, after.momentaryMax, "LUFS");
    row ("Loudness range", before.lra, after.lra, "LU");
    row ("True peak", before.truePeak, after.truePeak, "dBTP", 2);
    row ("Sample peak", before.samplePeak, after.samplePeak, "dBFS", 2);
    row ("RMS", before.rms, after.rms, "dBFS");
    row ("Crest factor", before.crest, after.crest, "dB");
    row ("Correlation", before.correlation, after.correlation, "", 2);
    line ("");
    if (s.bypass)
        line ("Chain       BYPASS (output is the input)");
    else
    {
        line ("Target      " + fmt (s.targetLufs) + " LUFS (" + presetName (s.targetPreset) + (s.targetPreset == TargetPreset::keepInput ? ": the input's own loudness" : "")
              + "), ceiling " + fmt (s.limiter.ceilingDbtp, 1) + " dBTP");
        line ("Loudness    " + loudnessStory (s, before.integrated, after.integrated, r.limiter.gr.maxDb).sentence);
        line ("Trim        " + fmt (s.trimDb, 1) + " dB" + (s.autoTrim ? ", headroom assistant " + fmt (r.input.autoTrimDb, 1) + " dB (" + r.input.message + ")" : juce::String())
              + (s.dcFilter ? ", DC filter 5 Hz" : "") + (std::abs (s.balanceDb) > 1.0e-9 ? ", balance correction " + fmt (s.balanceDb, 2) + " dB" : juce::String()));
        const auto& e = s.eq;
        line (juce::String ("EQ          ") + (e.enabled ? (e.linearPhase ? "linear phase" : "minimum phase") : "off")
              + (e.enabled ? ", low cut " + (e.lowCutOn ? fmt (e.lowCutHz, 0) + " Hz " + juce::String (e.lowCutSlope) + " dB/oct" : juce::String ("off"))
                             + ", low shelf " + fmt (e.lowShelfDb) + " dB @ " + fmt (e.lowShelfHz, 0) + " Hz"
                             + ", tilt " + fmt (e.tiltDbPerOct, 2) + " dB/oct"
                             + ", bell " + fmt (e.bell1Db) + " dB @ " + fmt (e.bell1Hz, 0) + " Hz Q" + fmt (e.bell1Q, 2)
                             + ", bell " + fmt (e.bell2Db) + " dB @ " + fmt (e.bell2Hz, 0) + " Hz Q" + fmt (e.bell2Q, 2)
                             + ", high shelf " + fmt (e.highShelfDb) + " dB @ " + fmt (e.highShelfHz, 0) + " Hz"
                             + (e.matchCurveDb.empty() ? juce::String() : ", tone match " + fmt (e.matchAmount * 100.0, 0) + " %")
                             + (std::abs (e.airDb) > 1.0e-9 || e.airDynamic ? ", AIR " + fmt (e.airDb) + " dB @ " + fmt (e.airHz / 1000.0, 0) + " kHz"
                                  + (e.airDynamic ? " dynamic (de-harsh up to " + fmt (e.airDeHarshDb) + " dB, mean " + fmt (r.dynEq.air.meanReductionDb, 2) + " dB)" : juce::String())
                                : juce::String())
                           : juce::String()));
        auto dynLine = [&] (const char* name, const DynBandSettings& d, const DynBandResult& res)
        {
            if (e.enabled && d.enabled)
                line (juce::String ("Dynamic EQ  ") + name + " " + (d.shape == DynBandSettings::lowShelf ? "low shelf " : "bell ") + fmt (d.freqHz, 0) + " Hz"
                      + (d.shape == DynBandSettings::bell ? " Q" + fmt (d.q, 2) : juce::String()) + ", AUTO threshold " + fmt (res.thresholdDb) + " dB, mean cut "
                      + fmt (res.loudMeanCutDb, 2) + " dB on the loud part (target " + fmt (d.amountDb, 1) + "), max " + fmt (res.maxCutDb, 1) + " dB (range " + fmt (d.rangeDb, 1) + ")");
        };
        dynLine ("low-mid", e.dynLowMid, r.dynEq.lowMid);
        dynLine ("upper-mid", e.dynUpperMid, r.dynEq.upperMid);
        dynLine ("tightener", e.tightener, r.dynEq.tightener);
        const auto& c = s.comp;
        const auto& gl = r.glue;
        line (juce::String ("Compressor  ") + (c.enabled ? (c.autoThreshold && gl.valid ? "AUTO threshold " + fmt (gl.thresholdDb) + " dBFS (glue target " + fmt (gl.targetGrDb, 2)
                                                             + " dB on the loud part" + (gl.dense ? ", reduced: input already dense (" + gl.denseReason + ")" : juce::String()) + ")"
                                                           : "threshold " + fmt (c.thresholdDb) + " dBFS")
                             + ", ratio " + fmt (gl.valid ? gl.ratio : c.ratio, 2) + ":1, attack "
                             + fmt (c.attackMs, 0) + " ms, release " + (c.autoRelease ? "auto (" + fmt (c.releaseMs, 0) + " ms)" : fmt (c.releaseMs, 0) + " ms")
                             + ", RMS " + fmt (c.rmsBlend * 100.0, 0) + " %, SC HPF " + fmt (c.scHpfHz, 0) + " Hz, mix " + fmt (c.mix * 100.0, 0) + " %"
                             + "; reduction on the loud part mean " + fmt (gl.loudMeanGrDb, 2) + " dB, max " + fmt (gl.loudMaxGrDb, 1)
                             + " dB; whole file max " + fmt (r.comp.maxDb, 1) + " dB, mean " + fmt (r.comp.meanDb, 2) + " dB; level-neutral make-up "
                             + fmt (gl.makeupDb, 2) + " dB"
                           : juce::String ("off")));
        if (c.enabled && gl.threeBand)
        {
            line ("            3-BAND, crossovers " + fmt (gl.xoverLowHz, 0) + " Hz / " + fmt (gl.xoverHighHz, 0) + " Hz (Linkwitz-Riley 4th order, zero phase)");
            const char* names[] { "low", "mid", "high" };
            for (int b = 0; b < 3; ++b)
            {
                const auto& ba = gl.bands[b];
                const auto& bs = c.bands[b];
                line (juce::String ("            ") + names[b] + ": " + (ba.active ? (bs.autoThreshold ? "AUTO threshold " : "threshold ") + fmt (ba.thresholdDb) + " dBFS, ratio "
                          + fmt (ba.ratio, 2) + ":1, target " + fmt (ba.targetGrDb, 2) + " dB, loud-part GR " + fmt (ba.loudMeanGrDb, 2) + " dB (max " + fmt (ba.loudMaxGrDb, 1)
                          + "), make-up " + fmt (ba.makeupDb, 2) + " dB" : juce::String ("bypassed")) + ", gain " + fmt (bs.gainDb, 1) + " dB" + (bs.solo ? ", SOLO" : ""));
            }
        }
        line ("Target lock " + juce::String (s.targetLock ? "on (drive re-solved after every change)" : "off"));
        const auto& w = s.width;
        line (juce::String ("Stereo      ") + (w.enabled ? "width " + fmt (w.width * 100.0, 0) + " %, mono below "
                             + (w.monoBelowHz > 0.0 ? fmt (w.monoBelowHz, 0) + " Hz" + (w.phaseSafeLowMono ? " (phase-safe)" : "") : juce::String ("off"))
                             + (std::abs (w.highWidth - 1.0) > 1.0e-9 ? ", width above " + fmt (w.highWidthHz, 0) + " Hz x" + fmt (w.highWidth, 2) : juce::String())
                           : juce::String ("off")));
        const auto& l = s.limiter;
        line (juce::String ("Limiter     ") + (l.enabled ? "drive " + fmt (l.driveDb, 2) + " dB, ceiling " + fmt (l.ceilingDbtp, 1) + " dBTP, release "
                             + fmt (l.releaseMs, 0) + " ms, look-ahead " + fmt (l.lookaheadMs, 1) + " ms"
                             + "; reduction max " + fmt (r.limiter.gr.maxDb, 2) + " dB, mean " + fmt (r.limiter.gr.meanDb, 2) + " dB, active "
                             + fmt (r.limiter.gr.activeFraction * 100.0, 1) + " % of the time, " + juce::String (r.limiter.passes) + " pass(es)"
                           : "off, drive " + fmt (l.driveDb, 2) + " dB"));
    }
    if (ref != nullptr && ref->valid)
        line ("Reference   integrated " + fmt (ref->integrated) + " LUFS, true peak " + fmt (ref->truePeak, 2) + " dBTP, LRA " + fmt (ref->lra) + " LU");
    line ("");
    if (r.warnings.isEmpty()) line ("Warnings    none");
    for (auto& w : r.warnings) line ("WARNING     " + w);
    line ("");
    line ("Method: ITU-R BS.1770-4 (K-weighting, gating), EBU Tech 3341 (momentary / short-term),");
    line ("EBU Tech 3342 (loudness range), true peak by 4x oversampling (BS.1770-4 Annex 2 method).");
    line ("Measured on the exported samples before quantisation.");
    return t;
}
} // namespace mihenk
