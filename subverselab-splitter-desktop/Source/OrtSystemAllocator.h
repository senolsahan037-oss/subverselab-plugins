#pragma once

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>

#include <onnxruntime_cxx_api.h>

#ifdef _WIN32
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <malloc.h>
#else
 #include <sys/mman.h>
#endif

/*  OrtSystemAllocator: the CPU allocator ELEK hands to ONNX Runtime for its sessions (main separation and DrumSep).

    WHY (measured 2026-11-18, docs in STATUS.md "memory" entry; 6-stem model, 40 s clip, 8 GB Mac):
      - ORT's default CPU arena grows by powers of two and never gives memory back: the process footprint (what Activity
        Monitor shows, compressed and swapped pages included) stayed at 4.7-5.2 GB from the second window on.
      - Arena off, plain malloc: 3.4-3.7 GB between windows, 4.0 GB at the top. macOS malloc keeps freed large blocks.
      - This allocator (large blocks mapped from the system and unmapped on free): 0.75-1.2 GB between windows, 3.2-3.3 GB
        at the top, same speed. The live peak of ORT's own tensors is 2.42 GB whatever the threads, graph optimisation
        level or memory pattern (all measured), so that is the floor for this model; what the arena and malloc kept on top
        of it is gone.
    Blocks of 1 MB and more are mapped (mmap / VirtualAlloc) and returned to the system on Free; smaller ones come from
    the C heap. Every block is 64-byte aligned (MLAS needs that). Live bytes and the high-water mark are counted for the log.

    The allocator is one object for the process and is registered with the ORT environment (RegisterAllocator) by
    useFor(); sessions then pick it up through "session.use_env_allocators". ORT keeps one environment per process, so a
    second ELEK instance in the same DAW finds it already registered, which is fine: it is this same object.
*/
namespace OrtSystemAllocator
{
    struct Allocator : OrtAllocator
    {
        static constexpr size_t kLargeBlock = (size_t) 1 << 20;
        // Bytes in front of the pointer handed out: [total size][header size]. 64 keeps heap blocks 64-byte aligned;
        // mapped blocks use a full 64 KB so their data stays page aligned on every page size in use (4, 16, 64 KB).
        static constexpr size_t kSmallHeader = 64, kLargeHeader = (size_t) 1 << 16;

        std::atomic<long long> liveBytes { 0 }, highBytes { 0 };
        OrtMemoryInfo* info = nullptr;

        Allocator()
        {
            version = ORT_API_VERSION;
            OrtAllocator::Alloc   = [] (OrtAllocator* a, size_t n) { return static_cast<Allocator*> (a)->allocate (n); };
            OrtAllocator::Reserve = OrtAllocator::Alloc;
            OrtAllocator::Free    = [] (OrtAllocator* a, void* p) { static_cast<Allocator*> (a)->release (p); };
            OrtAllocator::Info    = [] (const OrtAllocator* a) -> const OrtMemoryInfo* { return static_cast<const Allocator*> (a)->info; };
        }

        void* allocate (size_t n)
        {
            const bool large = n + kSmallHeader >= kLargeBlock;
            const size_t header = large ? kLargeHeader : kSmallHeader;
            const size_t total = n + header;
            char* base = nullptr;
            if (large)
            {
               #ifdef _WIN32
                base = (char*) VirtualAlloc (nullptr, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
               #else
                void* m = mmap (nullptr, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
                base = m == MAP_FAILED ? nullptr : (char*) m;
               #endif
            }
            else
            {
               #ifdef _WIN32
                base = (char*) _aligned_malloc (total, 64);
               #else
                void* m = nullptr;
                base = posix_memalign (&m, 64, total) == 0 ? (char*) m : nullptr;
               #endif
            }
            if (base == nullptr)
                return nullptr;    // ORT turns this into an error status / exception, which the caller reports

            char* p = base + header;
            std::memcpy (p - 2 * sizeof (size_t), &total, sizeof (size_t));
            std::memcpy (p - sizeof (size_t), &header, sizeof (size_t));
            const long long now = liveBytes += (long long) total;
            long long high = highBytes.load();
            while (now > high && ! highBytes.compare_exchange_weak (high, now)) {}
            return p;
        }

        void release (void* ptr)
        {
            if (ptr == nullptr)
                return;
            char* p = (char*) ptr;
            size_t total = 0, header = 0;
            std::memcpy (&total, p - 2 * sizeof (size_t), sizeof (size_t));
            std::memcpy (&header, p - sizeof (size_t), sizeof (size_t));
            char* base = p - header;
            liveBytes -= (long long) total;
            if (header == kLargeHeader)
            {
               #ifdef _WIN32
                VirtualFree (base, 0, MEM_RELEASE);
               #else
                munmap (base, total);
               #endif
            }
            else
            {
               #ifdef _WIN32
                _aligned_free (base);
               #else
                std::free (base);
               #endif
            }
        }
    };

    inline Allocator& instance()
    {
        static Allocator* a = []
        {
            auto* created = new Allocator();     // never deleted: ORT may hold it until the process ends
            Ort::ThrowOnError (Ort::GetApi().CreateCpuMemoryInfo (OrtDeviceAllocator, OrtMemTypeDefault, &created->info));
            return created;
        }();
        return *a;
    }

    /** Turns ORT's arena off for these options and, when possible, routes the session's CPU memory through instance().
        Returns false (arena off, plain malloc) if the allocator could not be registered; the session still works. */
    inline bool useFor (Ort::Env& env, Ort::SessionOptions& options, std::string* why = nullptr)
    {
        options.DisableCpuMemArena();
        try
        {
            if (OrtStatus* status = Ort::GetApi().RegisterAllocator (env, &instance()))
            {
                const std::string message = Ort::GetApi().GetErrorMessage (status);
                Ort::GetApi().ReleaseStatus (status);
                if (message.find ("already") == std::string::npos)   // registered earlier in this process: that is this allocator
                {
                    if (why != nullptr) *why = message;
                    return false;
                }
            }
            options.AddConfigEntry ("session.use_env_allocators", "1");
            return true;
        }
        catch (const std::exception& e)
        {
            if (why != nullptr) *why = e.what();
            return false;
        }
    }

    inline double liveMB()  { return (double) instance().liveBytes.load() / 1048576.0; }
    inline double highMB()  { return (double) instance().highBytes.load() / 1048576.0; }
    inline void resetHigh() { instance().highBytes.store (instance().liveBytes.load()); }
}
