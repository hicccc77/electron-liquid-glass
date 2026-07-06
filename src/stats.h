// Internal performance counters (for the _stats diagnostic export): written by
// the worker thread, snapshotted from any thread. All relaxed atomics — the
// overhead on the render hot path is negligible.
#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>

struct GlassStats {
    std::atomic<uint64_t> loopIterations{ 0 };   // RenderTick entries (wakeup rate)
    std::atomic<uint64_t> framesAcquired{ 0 };   // desktop-update frames captured
    std::atomic<uint64_t> cacheCopies{ 0 };      // desktop mirror copy operations
    std::atomic<uint64_t> cacheCopyBytes{ 0 };   // total bytes copied into the mirror (GPU bandwidth)
    std::atomic<uint64_t> renders{ 0 };          // glass pipeline runs across panels
    std::atomic<uint64_t> lumaSamples{ 0 };      // luminance band samplings
    std::atomic<uint64_t> lumaMapWaitUs{ 0 };    // cumulative µs the staging Map waited on the GPU (sync stalls)
    std::atomic<uint64_t> lumaCpuUs{ 0 };        // cumulative CPU µs spent on histogram statistics

    static GlassStats& Instance() {
        static GlassStats stats;
        return stats;
    }

    void Reset() {
        loopIterations = 0;
        framesAcquired = 0;
        cacheCopies = 0;
        cacheCopyBytes = 0;
        renders = 0;
        lumaSamples = 0;
        lumaMapWaitUs = 0;
        lumaCpuUs = 0;
    }
};

inline uint64_t QpcNowUs() {
    static const uint64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<uint64_t>(f.QuadPart);
    }();
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return static_cast<uint64_t>(counter.QuadPart) * 1000000ull / freq;
}
