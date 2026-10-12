#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

/*  The window plan for overlap-add separation.

    Lifted out of ModelRunner::run() so it can be checked without an ONNX
    session, a background thread or a file on disk. The claim these two
    functions carry — that overlapping windows under a triangular weight turn a
    step at every join into a ramp — was measured on real audio and is worth a
    test that fails if someone simplifies it back.
*/
namespace OverlapAdd
{
    /** Where each model window starts.

        The last one is pinned to the end of the file rather than left wherever
        the stride lands, so the tail is covered by a full window instead of by
        zero padding alone.
    */
    inline std::vector<int> windowStarts (int totalSamples, int windowSize, double overlap)
    {
        const int stride = std::max (1, (int) std::lround (windowSize * (1.0 - overlap)));

        std::vector<int> starts;
        for (int start = 0; start <= std::max (0, totalSamples - windowSize); start += stride)
            starts.push_back (start);

        if (starts.empty() || starts.back() + windowSize < totalSamples)
            starts.push_back (std::max (0, totalSamples - windowSize));

        return starts;
    }

    /** A triangular ramp peaking mid-window, normalised to 1 at the peak.

        Weighting by distance from the window edge is what makes overlapping
        windows combine into something continuous: where two windows disagree,
        the one whose centre is nearer carries more of the result. A rectangular
        weight would average them equally and leave the seam audible.
    */
    inline std::vector<float> triangularWeight (int windowSize)
    {
        std::vector<float> weight ((size_t) windowSize);
        const int half = windowSize / 2;

        for (int i = 0; i < windowSize; ++i)
            weight[(size_t) i] = (float) (i < half ? i + 1 : windowSize - i);

        const float peak = *std::max_element (weight.begin(), weight.end());
        for (auto& w : weight)
            w /= peak;

        return weight;
    }
}
