#pragma once

#include <cstddef>
#include <cstdint>

namespace aurora::vita::gfx {

using CpuRangeTask = bool (*)(void* context, size_t begin, size_t end, uint32_t lane) noexcept;

// Vita applications have three CPU cores available for game work. Keep the
// render thread as one execution lane and add up to two persistent CPU workers.
bool initialize_cpu_workers(uint32_t workerThreads = 2, size_t minItems = 512) noexcept;
void shutdown_cpu_workers() noexcept;

// Runs a disjoint range on the caller and the persistent workers, or falls back
// to the caller for small jobs and when workers are unavailable.
bool cpu_parallel_for(size_t count, CpuRangeTask task, void* context) noexcept;

// Same worker pool, but with a per-call granularity override. This is intended
// for game-side jobs whose item count is much smaller than a vertex batch. The
// pool remains single-producer; nested/re-entrant calls fall back to the caller.
bool cpu_parallel_for_min(size_t count, size_t minItems, CpuRangeTask task, void* context) noexcept;

uint32_t cpu_worker_threads() noexcept;
uint32_t cpu_execution_lanes() noexcept;
size_t cpu_parallel_min_items() noexcept;

} // namespace aurora::vita::gfx
