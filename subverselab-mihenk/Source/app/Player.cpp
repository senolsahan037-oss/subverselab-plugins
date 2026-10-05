#include "Player.h"

namespace mihenk
{
std::shared_ptr<const TapBuffer> TapBuffer::make (const juce::AudioBuffer<float>& b)
{
    auto t = std::make_shared<TapBuffer>();
    t->channels = b.getNumChannels();
    t->length = b.getNumSamples();
    float peak = 0.0f;
    for (int c = 0; c < t->channels; ++c) peak = juce::jmax (peak, b.getMagnitude (c, 0, t->length));
    t->scale = peak > 0.0f ? peak / 32767.0f : 1.0f;
    const float inv = 1.0f / t->scale;
    t->data.resize ((size_t) t->channels * (size_t) t->length);
    for (int c = 0; c < t->channels; ++c)
    {
        const float* x = b.getReadPointer (c);
        for (int i = 0; i < t->length; ++i)
            t->data[(size_t) i * (size_t) t->channels + (size_t) c] = (int16_t) juce::jlimit (-32767, 32767, juce::roundToInt (x[i] * inv));
    }
    return t;
}

void Player::setSources (BufferPtr o, BufferPtr m, double rate)
{
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        original = std::move (o);
        master = std::move (m);
        tapEq = nullptr;
        tapComp = nullptr;
        gr = nullptr;
    }
    fileRate.store (rate);
    const juce::int64 len = original != nullptr ? original->getNumSamples() : 0;
    length.store (len);
    position.store (0);
    loopStart.store (0);
    loopEnd.store (len);
    playing.store (false);
}

void Player::setMaster (BufferPtr m)
{
    const juce::SpinLock::ScopedLockType sl (lock);
    master = std::move (m);
}

void Player::setTaps (TapPtr eq, TapPtr comp, GrPtr g)
{
    const juce::SpinLock::ScopedLockType sl (lock);
    tapEq = std::move (eq);
    tapComp = std::move (comp);
    gr = std::move (g);
}

void Player::stop()
{
    playing.store (false);
    position.store (looping.load() ? loopStart.load() : 0);
}

void Player::setPosition (juce::int64 s)
{
    position.store (juce::jlimit ((juce::int64) 0, juce::jmax ((juce::int64) 0, length.load() - 1), s));
}

void Player::setLoopRegion (juce::int64 a, juce::int64 b)
{
    if (b < a) std::swap (a, b);
    const auto len = length.load();
    loopStart.store (juce::jlimit ((juce::int64) 0, len, a));
    loopEnd.store (juce::jlimit ((juce::int64) 0, len, b));
}

void Player::prepareToPlay (int, double) {}

void Player::getNextAudioBlock (const juce::AudioSourceChannelInfo& info)
{
    info.clearActiveBufferRegion();
    BufferPtr a, b;
    TapPtr te, tc;
    GrPtr g;
    {
        const juce::SpinLock::ScopedTryLockType sl (lock);
        if (! sl.isLocked()) return;
        a = original;
        b = master;
        te = tapEq;
        tc = tapComp;
        g = gr;
    }
    // The shared pointers above are copies: the message thread never frees a buffer while it plays here
    // (Session keeps the previous generation alive too).

    auto* out = info.buffer;
    const int outCh = out->getNumChannels();
    if (a == nullptr || ! playing.load())
    {
        if (meters != nullptr) meters->running.store (false);
        return;
    }
    if (b == nullptr || b->getNumSamples() != a->getNumSamples()) b = a;
    const juce::int64 len = a->getNumSamples();
    if (te != nullptr && te->length != len) te = nullptr;
    if (tc != nullptr && tc->length != len) tc = nullptr;

    const float gA = originalGain.load();
    const float target = listenOriginal.load() ? 1.0f : 0.0f;
    const float targetBy = bypass.load() ? 1.0f : 0.0f;
    const float step = 1.0f / (0.02f * (float) fileRate.load());
    juce::int64 pos = position.load();
    const bool loop = looping.load();
    const juce::int64 ls = loopStart.load(), le = loopEnd.load();
    const bool validLoop = loop && le - ls > 64;
    const int srcCh = a->getNumChannels();
    auto grAt = [&g] (const std::vector<float>& h, juce::int64 p) -> float
    {
        if (g == nullptr || h.empty()) return 0.0f;
        const auto k = (size_t) (p / juce::jmax (1, g->hop));
        return k < h.size() ? h[k] : 0.0f;
    };

    int nFrames = 0;
    for (int i = 0; i < info.numSamples; ++i)
    {
        if (validLoop && (pos >= le || pos < ls)) pos = ls;
        if (pos >= len)
        {
            if (loop) pos = 0;
            else { playing.store (false); pos = 0; break; }
        }
        mixA += mixA < target ? juce::jmin (step, target - mixA) : -juce::jmin (step, mixA - target);
        mixBy += mixBy < targetBy ? juce::jmin (step, targetBy - mixBy) : -juce::jmin (step, mixBy - targetBy);
        float* f = frames.data() + (size_t) nFrames * MeterFifo::frameSize;
        for (int c = 0; c < 2; ++c)
        {
            const int src = juce::jmin (c, srcCh - 1);
            const float vb = b->getSample (src, (int) pos);
            const float va = a->getSample (src, (int) pos);
            const float ab = vb + (va * gA - vb) * mixA;
            const float vo = va * gA;                                                                          // the original at the master's loudness (A/B LEVEL-MATCHED), raw when that is off
            const float heard = mixBy >= 1.0f ? vo : (mixBy > 0.0f ? ab + (vo - ab) * mixBy : ab);       // fully bypassed: the original, level-matched
            if (c < outCh) out->setSample (c, info.startSample + i, heard);
            f[MeterFifo::inL + c] = va;
            f[MeterFifo::eqL + c] = te != nullptr ? te->get (c, (int) pos) : vb;
            f[MeterFifo::compL + c] = tc != nullptr ? tc->get (c, (int) pos) : vb;
            f[MeterFifo::outL + c] = heard;
        }
        if (g != nullptr)
        {
            f[MeterFifo::grComp] = grAt (g->comp, pos);
            for (int k = 0; k < 3; ++k) f[MeterFifo::grLow + k] = grAt (g->band[k], pos);
            f[MeterFifo::grLim] = grAt (g->lim, pos);
        }
        else
            for (int k = MeterFifo::grComp; k <= MeterFifo::grLim; ++k) f[k] = 0.0f;
        ++pos;
        if (++nFrames == 256)
        {
            if (meters != nullptr) meters->push (frames.data(), nFrames);
            nFrames = 0;
        }
    }
    if (meters != nullptr)
    {
        if (nFrames > 0) meters->push (frames.data(), nFrames);
        meters->sampleRate.store (fileRate.load());
        meters->monoSource.store (srcCh == 1);
        meters->position.store (pos);
        meters->running.store (playing.load());
    }
    position.store (pos);
}
} // namespace mihenk
