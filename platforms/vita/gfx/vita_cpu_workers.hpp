#pragma once

#include <cstddef>
#include <cstdint>

namespace aurora::vita::gfx {

using CpuRangeTask = bool (*)(void* context, size_t begin, size_t end, uint32_t lane) noexcept;

inline constexpr uint32_t MaxExecutionLanes = 4;
inline constexpr uint32_t MaxWorkerThreads = MaxExecutionLanes - 1;

struct CpuCore3BudgetConfig {
  bool enabled = false;
  uint32_t maxTotalPercent = 70;
  uint32_t guardPercent = 5;
  uint32_t shortWindowMs = 100;
  uint32_t longWindowMs = 1000;
  uint32_t chunkTargetUs = 250;
  uint32_t samplePeriodUs = 10000;
};

struct CpuCore3BudgetSnapshot {
  bool configured = false;
  bool telemetryValid = false;
  bool dispatchAllowed = false;
  uint32_t targetPercent = 0;
  uint32_t lastTotalPercentX100 = 0;
  uint64_t shortCreditUs = 0;
  uint64_t shortCapacityUs = 0;
  uint64_t longCreditUs = 0;
  uint64_t longCapacityUs = 0;
  uint64_t chunks = 0;
  uint64_t denied = 0;
  uint64_t telemetryFailures = 0;
  uint64_t overruns = 0;
  uint64_t totalChunkUs = 0;
  uint32_t maxChunkUs = 0;
  uint32_t estimatedUsPerItemX1024 = 0;
};

struct CpuWorkerProbeLane {
  int requestedAffinity = -1;
  int priority = -1;
  int wakeResult = -1;
  int doneResult = -1;
  int createResult = -1;
  int startResult = -1;
  int waitResult = -1;
  int actualCpu = -1;
  int actualAffinity = -1;
  bool created = false;
};

struct CpuWorkerProbeSnapshot {
  CpuWorkerProbeLane lanes[MaxWorkerThreads]{};
};

struct CpuVertexParallelSnapshot {
  uint64_t calls = 0;
  uint64_t dynamicCalls = 0;
  uint64_t totalWallUs = 0;
  uint64_t callerWaitUs = 0;
  uint64_t laneItems[MaxExecutionLanes]{};
  uint64_t laneChunks[MaxExecutionLanes]{};
  uint64_t laneWorkUs[MaxExecutionLanes]{};
};

// Keep the render thread as lane 0. CPU1/CPU2 remain the normal helpers; an
// optional third helper may be pinned to the system-reserved core when the
// title has explicit access to it (for example through CapUnlocker).
bool initialize_cpu_workers(uint32_t workerThreads = 2, size_t minItems = 512,
                            uint32_t defaultExecutionLanes = 0,
                            const CpuCore3BudgetConfig& core3Budget = {}) noexcept;
void shutdown_cpu_workers() noexcept;

// Runs a disjoint range on the caller and the persistent workers, or falls back
// to the caller for small jobs and when workers are unavailable.
bool cpu_parallel_for(size_t count, CpuRangeTask task, void* context) noexcept;
// P3 entry point: identical semantics, but CPU3 may join through the quota
// scheduler when enabled. Keep non-vertex renderer work on cpu_parallel_for().
bool cpu_parallel_for_vertex(size_t count, CpuRangeTask task, void* context) noexcept;

// Same worker pool, but with a per-call granularity override. This is intended
// for game-side jobs whose item count is much smaller than a vertex batch. The
// pool remains single-producer; nested/re-entrant calls fall back to the caller.
bool cpu_parallel_for_min(size_t count, size_t minItems, CpuRangeTask task, void* context) noexcept;
bool cpu_parallel_for_min_lanes(size_t count, size_t minItems, uint32_t maxExecutionLanes,
                                CpuRangeTask task, void* context) noexcept;

uint32_t cpu_worker_threads() noexcept;
uint32_t cpu_execution_lanes() noexcept;
size_t cpu_parallel_min_items() noexcept;
bool cpu_core3_available() noexcept;
int cpu_core3_cpu_id() noexcept;
int cpu_core3_affinity_mask() noexcept;
CpuCore3BudgetSnapshot cpu_core3_budget_snapshot() noexcept;
CpuWorkerProbeSnapshot cpu_worker_probe_snapshot() noexcept;
CpuVertexParallelSnapshot cpu_vertex_parallel_snapshot() noexcept;

} // namespace aurora::vita::gfx
