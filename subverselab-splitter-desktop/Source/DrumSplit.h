#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <cstring>

#include <onnxruntime_cxx_api.h>

#include "OverlapAdd.h"
#include "OrtSystemAllocator.h"

/*  DrumSplit: the second separation stage. It takes the DRUM stem the main
    separation produced and splits it into kick / snare / cymbals (+hats) / toms
    with the DrumSep model (inagoy/drumsep, HDemucs v3, exported to ONNX by
    scripts/export_drumsep_onnx.py, shrunk by scripts/onnx_fp16_storage.py and
    scripts/onnx_dft_tables.py).

    Model interface (read from the exported file, not assumed):
        input  "mix"   float [1, 2, 441000]      10 s at 44.1 kHz, stereo
        output "stems" float [1, 4, 2, 441000]   bombo, redoblante, platillos, toms

    Processing, mirroring the Python reference scripts/drumsplit_reference.py:
      1. Windows of 441000 samples placed by OverlapAdd::windowStarts (25 %
         overlap by default; the last window is pinned to the end of the input).
      2. EACH window is standardised on its own, as the verified Python does
         (scripts/verify_drumsep_onnx.py, verify_onnx_only.py):
             mono = x.mean(0);  x = (x - mono.mean()) / mono.std()
         The statistics use only the real samples of the window (population
         std, ddof 0). A window with std below 1e-8 (digital silence) uses
         std = 1 so it can never divide by zero.
      3. The model output is denormalised with the same window's numbers,
         y = out * std + mean, as scripts/drumsep_eval.py and Demucs' own
         separate path do (`* std + mean` on every source).
      4. Overlap-add under OverlapAdd::triangularWeight, then divide by the
         accumulated weight.
    An input shorter than one window is zero-padded in the standardised domain
    (after step 2) and the output is trimmed back to the input length.

    Memory (2026-11-18): processStream() keeps one window of input and one of accumulator, whatever the input length;
    the session's memory goes through OrtSystemAllocator (arena off, large blocks unmapped on free).

    The ONNX session lives only inside process(): it is created there and
    destroyed before process() returns (also on error or cancel). Measured
    (vmmap, 2026-11-18): afterwards live "Malloc Large" is 13.5 MB, but macOS
    malloc keeps about 1.1 GB of the freed large blocks resident as "Malloc
    Large (empty)" (malloc_zone_pressure_relief released 0 MB). Repeated calls
    reuse that cache and do not grow; the process footprint does not drop
    back to its pre-call size.

    No JUCE dependency: plain planar float buffers in and out. On Windows the
    caller must have loaded onnxruntime.dll before calling process() (ELEK
    builds with ORT_API_MANUAL_INIT and loads the DLL itself; see
    PluginProcessor.cpp, elekLoadOrt), and must call Ort::InitApi() once.
*/
namespace DrumSplit
{
    constexpr int kWindow     = 441000;   // 10 s at 44.1 kHz, fixed by the exported graph
    constexpr int kStems      = 4;
    constexpr int kChannels   = 2;
    constexpr int kSampleRate = 44100;

    /** Output order of the model. Spanish names are the model's own; the
        English meaning is kick, snare, cymbals (incl. hi-hats), toms. */
    inline const char* stemName (int index)
    {
        static const char* names[kStems] = { "bombo", "redoblante", "platillos", "toms" };
        return (index >= 0 && index < kStems) ? names[index] : "";
    }

    inline const char* stemDisplayName (int index)
    {
        static const char* names[kStems] = { "Kick", "Snare", "Cymbals", "Toms" };
        return (index >= 0 && index < kStems) ? names[index] : "";
    }

    struct Result
    {
        bool ok = false;
        std::string error;
        int numSamples = 0;
        int windowsRun = 0;
        /** Planar: data[(stem * kChannels + channel) * numSamples + n]. */
        std::vector<float> data;

        const float* channel (int stem, int ch) const { return data.data() + ((size_t) stem * kChannels + (size_t) ch) * (size_t) numSamples; }
        float*       channel (int stem, int ch)       { return data.data() + ((size_t) stem * kChannels + (size_t) ch) * (size_t) numSamples; }
    };

    struct Options
    {
        double overlap = 0.25;
        /** ORT intra-op threads; 0 leaves ORT's default. */
        int intraOpThreads = 0;
        /** ORT's CPU memory arena. Off by default: measured on the test
            program, with the arena the peak and the memory left resident
            after process() were higher (numbers in STATUS.md). */
        bool cpuMemArena = false;
        /** Route the session's CPU memory through OrtSystemAllocator (arena off, large blocks given back to the system
            on free). On by default since 2026-11-18 (STATUS.md, memory entry); cpuMemArena is ignored when this is on. */
        bool systemAllocator = true;
        /** Called after every window with (done, total). Returning false
            cancels: process() releases the session and returns an error. */
        std::function<bool (int, int)> progress;
        /** The caller's RunOptions for every model call (so it can end one in progress with SetTerminate); null: a default one. */
        Ort::RunOptions* runOptions = nullptr;
    };

    /** Standardise one window into the model's planar [2, kWindow] input.
        n real samples are read; the rest of dst is left at 0 (padding in the
        standardised domain). Returns the mean and std used, for denormalising. */
    inline void standardiseWindow (const float* left, const float* right, int n,
                                   float* dst, float& meanOut, float& stdOut)
    {
        double sum = 0.0;
        for (int i = 0; i < n; ++i)
            sum += 0.5 * ((double) left[i] + (double) right[i]);
        const double mean = n > 0 ? sum / n : 0.0;

        double var = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double d = 0.5 * ((double) left[i] + (double) right[i]) - mean;
            var += d * d;
        }
        double sd = n > 0 ? std::sqrt (var / n) : 0.0;
        if (! (sd >= 1.0e-8))   // also catches NaN
            sd = 1.0;

        const float m = (float) mean, s = (float) sd;
        std::fill (dst, dst + (size_t) kChannels * kWindow, 0.0f);
        for (int i = 0; i < n; ++i)
        {
            dst[i]           = (left[i]  - m) / s;
            dst[kWindow + i] = (right[i] - m) / s;
        }
        meanOut = m;
        stdOut  = s;
    }

    class DrumSplitter
    {
    public:
        explicit DrumSplitter (std::string modelPathUtf8, Options options = {})
            : modelPath (std::move (modelPathUtf8)), opts (std::move (options)) {}

        /** Reads n samples of the drum stem starting at `start` into left/right. Returns false on a read error. */
        using ReadFn = std::function<bool (int start, int n, float* left, float* right)>;
        /** Receives n FINISHED output samples starting at `start`: parts[stem * kChannels + channel][0..n). Called in order,
            without gaps, covering [0, numSamples) once. Returns false on a write error. */
        using EmitFn = std::function<bool (int start, int n, const float* const* parts)>;

        struct StreamResult
        {
            bool ok = false;
            std::string error;
            int windowsRun = 0;
        };

        /** STREAMING split (ELEK's engine, 2026-11-18 memory fix): memory does not depend on the input length. Input is
            pulled one window at a time through read(); output samples are handed to emit() as soon as no later window
            can touch them (everything before the next window's start). The arithmetic is the same as process(), in the
            same order, so the result is bit-identical to it: per sample, the window contributions are added to a zeroed
            float in window order, the weights likewise, and the sum is divided by the weight sum at the end.
            Working memory: one window of input (2 x 441000 floats) and one window of accumulator (4 x 2 x 441000 + 441000
            floats), about 19 MB, plus the ONNX session. The session is created here and released before this returns. */
        StreamResult processStream (int numSamples, const ReadFn& read, const EmitFn& emit) const
        {
            StreamResult result;
            if (numSamples <= 0 || ! read || ! emit)
            {
                result.error = "empty input";
                return result;
            }

            try
            {
                const std::vector<int> starts = OverlapAdd::windowStarts (numSamples, kWindow, opts.overlap);
                const std::vector<float> weight = OverlapAdd::triangularWeight (kWindow);

                // acc holds absolute samples [accStart, accStart + kWindow): every sample a not yet finished window covers.
                std::vector<float> acc ((size_t) kStems * kChannels * kWindow, 0.0f);
                std::vector<float> wacc ((size_t) kWindow, 0.0f);
                int accStart = 0;
                std::vector<float> inL ((size_t) kWindow), inR ((size_t) kWindow);

                // Normalise and hand out the first `count` samples of acc, then move the rest to the front.
                auto flush = [&] (int count) -> bool
                {
                    if (count <= 0) return true;
                    const float* parts[kStems * kChannels];
                    for (int c = 0; c < kStems * kChannels; ++c)
                    {
                        float* d = acc.data() + (size_t) c * kWindow;
                        for (int i = 0; i < count; ++i)
                        {
                            const float wsum = wacc[(size_t) i];
                            d[i] /= (wsum > 0.0f ? wsum : 1.0f);
                        }
                        parts[c] = d;
                    }
                    if (! emit (accStart, count, parts))
                        return false;
                    const int keep = kWindow - count;
                    for (int c = 0; c < kStems * kChannels; ++c)
                    {
                        float* d = acc.data() + (size_t) c * kWindow;
                        if (keep > 0) std::memmove (d, d + count, (size_t) keep * sizeof (float));
                        std::fill (d + std::max (0, keep), d + kWindow, 0.0f);
                    }
                    if (keep > 0) std::memmove (wacc.data(), wacc.data() + count, (size_t) keep * sizeof (float));
                    std::fill (wacc.begin() + std::max (0, keep), wacc.end(), 0.0f);
                    accStart += count;
                    return true;
                };

                // Everything ORT owns is scoped to this block, so the session is freed on every path out of it.
                {
                    Ort::Env env (ORT_LOGGING_LEVEL_WARNING, "DrumSplit");
                    Ort::SessionOptions so;
                    if (opts.intraOpThreads > 0)
                        so.SetIntraOpNumThreads (opts.intraOpThreads);
                    so.SetGraphOptimizationLevel (GraphOptimizationLevel::ORT_ENABLE_ALL);
                    if (opts.systemAllocator)
                        OrtSystemAllocator::useFor (env, so);   // arena off, large blocks returned to the system (OrtSystemAllocator.h)
                    else if (! opts.cpuMemArena)
                        so.DisableCpuMemArena();

                   #ifdef _WIN32
                    const std::filesystem::path p = std::filesystem::u8path (modelPath);
                    Ort::Session session (env, p.c_str(), so);
                   #else
                    Ort::Session session (env, modelPath.c_str(), so);
                   #endif

                    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);
                    const int64_t inDims[] = { 1, kChannels, kWindow };
                    const char* inNames[]  = { "mix" };
                    const char* outNames[] = { "stems" };
                    std::vector<float> input ((size_t) kChannels * kWindow, 0.0f);

                    for (size_t w = 0; w < starts.size(); ++w)
                    {
                        const int start = starts[w];
                        const int n = std::min (kWindow, numSamples - start);

                        // Samples before this window's start are covered by no later window: finished.
                        if (! flush (start - accStart))
                        {
                            result.error = "could not write the output";
                            return result;
                        }

                        if (! read (start, n, inL.data(), inR.data()))
                        {
                            result.error = "could not read the drum stem";
                            return result;
                        }

                        float mean = 0.0f, sd = 1.0f;
                        standardiseWindow (inL.data(), inR.data(), n, input.data(), mean, sd);

                        Ort::Value in = Ort::Value::CreateTensor<float> (mem, input.data(), input.size(), inDims, 3);
                        auto outs = opts.runOptions != nullptr ? session.Run (*opts.runOptions, inNames, &in, 1, outNames, 1)
                                                               : session.Run (Ort::RunOptions { nullptr }, inNames, &in, 1, outNames, 1);

                        const auto shape = outs.front().GetTensorTypeAndShapeInfo().GetShape();
                        if (shape.size() != 4 || shape[0] != 1 || shape[1] != kStems || shape[2] != kChannels || shape[3] != kWindow)
                        {
                            result.error = "model output shape is not [1,4,2,441000]";
                            return result;
                        }

                        const float* out = outs.front().GetTensorData<float>();
                        for (int s = 0; s < kStems; ++s)
                            for (int ch = 0; ch < kChannels; ++ch)
                            {
                                const float* src = out + ((size_t) s * kChannels + (size_t) ch) * kWindow;
                                float* dst = acc.data() + ((size_t) s * kChannels + (size_t) ch) * kWindow;
                                for (int i = 0; i < n; ++i)
                                    dst[i] += (src[i] * sd) * weight[(size_t) i];      // no "+ mean": see PluginProcessor.cpp (the four parts each carried the stem's offset; they added back to the Drums stem at 7.4 dB instead of 18.8)
                            }

                        for (int i = 0; i < n; ++i)
                            wacc[(size_t) i] += weight[(size_t) i];

                        result.windowsRun = (int) w + 1;
                        if (opts.progress && ! opts.progress ((int) w + 1, (int) starts.size()))
                        {
                            result.error = "cancelled";
                            return result;
                        }
                    }
                }   // session and env released here

                if (! flush (numSamples - accStart))
                {
                    result.error = "could not write the output";
                    return result;
                }
                result.ok = true;
            }
            catch (const Ort::Exception& e)
            {
                result.error = std::string ("ONNX: ") + e.what();
            }
            catch (const std::exception& e)
            {
                result.error = e.what();
            }
            return result;
        }

        /** Split a stereo drum stem (44.1 kHz) held in memory. For a mono stem pass the same pointer twice. Same numbers as
            processStream() (it is built on it); the whole result is held in memory, 32 bytes per input sample, so ELEK's
            engine uses processStream() and this stays for tools/drumsplit_test. */
        Result process (const float* left, const float* right, int numSamples) const
        {
            Result result;
            result.numSamples = std::max (0, numSamples);

            if (left == nullptr || right == nullptr || numSamples <= 0)
            {
                result.error = "empty input";
                return result;
            }

            try
            {
                result.data.assign ((size_t) kStems * kChannels * (size_t) numSamples, 0.0f);
            }
            catch (const std::exception& e)
            {
                result.error = e.what();
                return result;
            }

            const auto streamed = processStream (numSamples,
                [&] (int start, int n, float* l, float* r)
                {
                    std::copy (left + start, left + start + n, l);
                    std::copy (right + start, right + start + n, r);
                    return true;
                },
                [&] (int start, int n, const float* const* parts)
                {
                    for (int c = 0; c < kStems * kChannels; ++c)
                        std::copy (parts[c], parts[c] + n, result.channel (c / kChannels, c % kChannels) + start);
                    return true;
                });

            result.windowsRun = streamed.windowsRun;
            result.ok = streamed.ok;
            result.error = streamed.error;
            if (! result.ok)
                result.data.clear();
            return result;
        }

        const std::string& getModelPath() const { return modelPath; }

    private:
        std::string modelPath;
        Options opts;
    };
}
