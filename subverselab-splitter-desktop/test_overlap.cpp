/*  Checks the window plan without needing the model.

    The property under test is the one the rewrite exists for: two windows
    covering the same audio do not produce the same numbers, and where they meet
    the result must ramp rather than step. A model is simulated by returning a
    different constant for each window — which is the only behaviour that
    matters here — so the test is about the arithmetic around the model, not the
    model itself.

        cmake --build build --target test_overlap && ./build/.../test_overlap
*/
#include <cmath>
#include <cstdio>
#include <vector>

#include "Source/OverlapAdd.h"

namespace
{
    int failures = 0;

    void check (bool condition, const char* what)
    {
        std::printf ("  %-58s %s\n", what, condition ? "ok" : "FAILED");
        if (! condition) ++failures;
    }

    /** Runs the same accumulate-and-normalise the separator runs, over a model
        that returns `windowIndex + 1` for every sample of its window. */
    std::vector<float> blend (int totalSamples, int windowSize, double overlap)
    {
        const auto starts = OverlapAdd::windowStarts (totalSamples, windowSize, overlap);
        const auto weight = OverlapAdd::triangularWeight (windowSize);

        std::vector<float> out ((size_t) totalSamples, 0.0f);
        std::vector<float> weightSum ((size_t) totalSamples, 0.0f);

        for (size_t w = 0; w < starts.size(); ++w)
        {
            const int start = starts[w];
            const int span = std::min (windowSize, totalSamples - start);
            const float modelOutput = (float) (w + 1);

            for (int n = 0; n < span; ++n)
            {
                out[(size_t) (start + n)] += modelOutput * weight[(size_t) n];
                weightSum[(size_t) (start + n)] += weight[(size_t) n];
            }
        }

        for (int n = 0; n < totalSamples; ++n)
            out[(size_t) n] /= (weightSum[(size_t) n] > 0.0f ? weightSum[(size_t) n] : 1.0f);

        return out;
    }

    float worstStep (const std::vector<float>& signal)
    {
        float worst = 0.0f;
        for (size_t i = 1; i < signal.size(); ++i)
            worst = std::max (worst, std::abs (signal[i] - signal[i - 1]));
        return worst;
    }
}

int main()
{
    constexpr int window = 343980;                 // the model's fixed input
    const int total = window * 5 + 12345;          // deliberately not a whole number of windows

    const auto butted = blend (total, window, 0.0);
    const auto faded  = blend (total, window, 0.25);

    std::printf ("  butt-joined worst step  %.4f\n", worstStep (butted));
    std::printf ("  cross-faded worst step  %.4f\n", worstStep (faded));

    // Consecutive windows differ by exactly 1.0 by construction, so butting
    // them together leaves a step of that size at every join.
    check (std::abs (worstStep (butted) - 1.0f) < 0.01f,
           "no overlap leaves a full-size step at each join");
    check (worstStep (faded) < worstStep (butted) / 100.0f,
           "25% overlap reduces the worst step by over 100x");

    // The cost ratio is checked over a realistic track length, not over the
    // short signal above. At five windows it comes out at 7/6 — the last window
    // is pinned to the end either way, so with so few of them the pinning
    // dominates the arithmetic and a test written there would be passing on a
    // rounding accident rather than on the claim.
    const int fourMinutes = 4 * 60 * 44100;
    const auto plain = OverlapAdd::windowStarts (fourMinutes, window, 0.0);
    const auto lapped = OverlapAdd::windowStarts (fourMinutes, window, 0.25);
    std::printf ("  a four-minute track: %zu windows without overlap, %zu with\n",
                 plain.size(), lapped.size());

    check (lapped.size() > plain.size(), "overlap costs more model runs");
    const double ratio = (double) lapped.size() / (double) plain.size();
    check (ratio > 1.25 && ratio < 1.45, "the extra cost is about a third");

    const auto shortLapped = OverlapAdd::windowStarts (total, window, 0.25);
    check (shortLapped.front() == 0, "the first window starts at the beginning");
    check (shortLapped.back() + window >= total, "the last window reaches the end");

    // Every sample must be covered, or the normalisation divides by a guard and
    // that stretch of the stem comes back at the wrong level.
    // A file shorter than one window still has to produce one window.
    check (OverlapAdd::windowStarts (1000, window, 0.25).size() == 1,
           "audio shorter than one window still gets a window");

    const auto weight = OverlapAdd::triangularWeight (window);
    check (std::abs (*std::max_element (weight.begin(), weight.end()) - 1.0f) < 1e-6f,
           "the weight peaks at 1");
    check (weight.front() < 0.01f && weight.back() < 0.01f,
           "the weight falls to nothing at both edges");

    std::printf ("\n  %s\n", failures == 0 ? "all checks passed" : "SOME CHECKS FAILED");
    return failures == 0 ? 0 : 1;
}
