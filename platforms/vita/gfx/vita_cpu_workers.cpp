#include "vita_cpu_workers.hpp"
#include "../vita_log.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>

#if defined(__vita__)
#include <psp2/kernel/threadmgr.h>
#ifndef SCE_KERNEL_CPU_MASK_SYSTEM
#define SCE_KERNEL_CPU_MASK_SYSTEM 0x00080000
#endif
extern "C" int sceKernelGetCpuId(void);
extern "C" int sceKernelGetThreadCpuAffinityMask(SceUID thid);
#endif

namespace aurora::vita::gfx {

#if defined(__vita__)
namespace {
constexpr uint32_t MaxWorkers = 3;
constexpr size_t WorkerStackBytes = 128u * 1024u;
constexpr size_t ProbeStackBytes = 16u * 1024u;

struct CpuWorkerState {
  struct Lane {
    SceUID thread = -1, wake = -1, done = -1;
    std::atomic<bool> stop{false};
    std::atomic<unsigned> state{0}; // release/acquire publication: idle, ready, done
    CpuRangeTask task = nullptr;
    void *context = nullptr;
    size_t begin = 0, end = 0;
    bool result = true;
  };
  std::array<Lane, MaxWorkers> lanes{};
  bool initialized = false;
  uint32_t workerCount = 0;
  uint32_t defaultExecutionLanes = 1;
  size_t minItems = 512;
  std::atomic_flag busy = ATOMIC_FLAG_INIT;
};

CpuWorkerState g_workers{};

struct SystemCoreProbeArgs {
  int* cpuId = nullptr;
};

int system_core_probe_main(SceSize, void* opaque) {
  auto* args = static_cast<SystemCoreProbeArgs*>(opaque);
  if (args && args->cpuId) *args->cpuId = sceKernelGetCpuId();
  return 0;
}

int cpu_worker_main(SceSize, void* opaque) {
  const uint32_t index = *static_cast<const uint32_t*>(opaque);
  auto &lane = g_workers.lanes[index];
  for (;;) {
    const int wait=sceKernelWaitSema(lane.wake,1,nullptr);
    if (lane.stop.load(std::memory_order_acquire)) return 0;
    if(wait<0){sceKernelDelayThread(100);continue;}
    if(lane.state.load(std::memory_order_acquire)!=1)continue;
    lane.result = lane.task && lane.task(lane.context, lane.begin, lane.end, index + 1);
    lane.state.store(2, std::memory_order_release);
    sceKernelSignalSema(lane.done,1);
  }
}

void destroy_lane(CpuWorkerState::Lane &lane) noexcept {
  if (lane.thread >= 0) sceKernelDeleteThread(lane.thread);
  if (lane.wake >= 0) sceKernelDeleteSema(lane.wake);
  if (lane.done >= 0) sceKernelDeleteSema(lane.done);
  lane.thread=lane.wake=lane.done=-1;
  lane.task=nullptr;lane.context=nullptr;
  lane.state.store(0);
}
} // namespace

bool probe_system_core() noexcept {
  int cpuId = -1;
  SystemCoreProbeArgs args{&cpuId};
  const SceUID thread = sceKernelCreateThread(
      "aurora_core3_probe", system_core_probe_main, 0x10000180,
      ProbeStackBytes, 0, SCE_KERNEL_CPU_MASK_SYSTEM, nullptr);
  if (thread < 0) return false;

  const int affinity = sceKernelGetThreadCpuAffinityMask(thread);
  const int start = sceKernelStartThread(thread, sizeof(args), &args);
  int wait = -1;
  if (start >= 0) wait = sceKernelWaitThreadEnd(thread, nullptr, nullptr);
  sceKernelDeleteThread(thread);
  return affinity == SCE_KERNEL_CPU_MASK_SYSTEM && start >= 0 && wait >= 0 && cpuId == 3;
}

bool initialize_cpu_workers(uint32_t workerThreads, size_t minItems, uint32_t defaultExecutionLanes,
                            bool useSystemCore) noexcept {
  if (g_workers.initialized) shutdown_cpu_workers();

  g_workers.workerCount = 0;
  g_workers.minItems = std::max<size_t>(1, minItems);
  const uint32_t maxAllowedWorkers = useSystemCore ? MaxWorkers : 2u;
  const uint32_t requested = std::min(workerThreads, maxAllowedWorkers);
  for (uint32_t i = 0; i < requested; ++i) {
    auto &lane=g_workers.lanes[i];
    lane.stop.store(false);lane.state.store(0);
    lane.wake=sceKernelCreateSema("aurora_cpu_wake",0,0,1,nullptr);
    lane.done=sceKernelCreateSema("aurora_cpu_done",0,0,1,nullptr);
    if(lane.wake<0 || lane.done<0) {destroy_lane(lane);break;}
    char name[24];std::snprintf(name,sizeof(name),"aurora_cpu_%u",i+1);
    // Stable 3-core topology: CPU2 is the primary helper and CPU1 is a low-
    // priority opportunistic lane below audio. With CapUnlocker, insert CPU3
    // before CPU1 so renderer jobs capped to three execution lanes use
    // CPU0+CPU2+CPU3 and never depend on the audio core.
    int affinity = SCE_KERNEL_CPU_MASK_USER_1;
    int priority = 0x10000180;
    if (i == 0) {
      affinity = SCE_KERNEL_CPU_MASK_USER_2;
      priority = 0x10000110;
    } else if (useSystemCore && i == 1) {
      affinity = SCE_KERNEL_CPU_MASK_SYSTEM;
      priority = 0x10000120;
    }
    lane.thread=sceKernelCreateThread(name,cpu_worker_main,priority,
                                     WorkerStackBytes,0,affinity,nullptr);
    if(lane.thread<0 || sceKernelStartThread(lane.thread,sizeof(i),&i)<0) {
      destroy_lane(lane);break;
    }
    ++g_workers.workerCount;
  }

  g_workers.initialized = true;
  const uint32_t availableLanes=g_workers.workerCount+1;
  g_workers.defaultExecutionLanes=defaultExecutionLanes==0?availableLanes:
      std::max<uint32_t>(1,std::min(defaultExecutionLanes,availableLanes));
  AURORA_VITA_LOG_INFO(
      "[aurora-vita] cpu workers=%u lanes=%u default_lanes=%u system_core=%u sync=paired_semaphores parallel_min_items=%llu\n",
      g_workers.workerCount,g_workers.workerCount+1,g_workers.defaultExecutionLanes,
      useSystemCore?1u:0u,
      static_cast<unsigned long long>(g_workers.minItems));
  return true;
}

void shutdown_cpu_workers() noexcept {
  if (!g_workers.initialized) return;
  // The single producer calls shutdown only outside cpu_parallel_for, so no
  // callback can still own a caller's capture/staging data at this boundary.
  for (uint32_t i = 0; i < g_workers.workerCount && i < MaxWorkers; ++i) {
    auto &lane=g_workers.lanes[i];
    lane.stop.store(true,std::memory_order_release);
    sceKernelSignalSema(lane.wake,1);
  }
  for (uint32_t i = 0; i < g_workers.workerCount && i < MaxWorkers; ++i) {
    auto &lane=g_workers.lanes[i];
    sceKernelWaitThreadEnd(lane.thread,nullptr,nullptr);
    destroy_lane(lane);
  }
  g_workers.initialized = false;
  g_workers.workerCount = 0;
}

bool cpu_parallel_for_min_lanes(size_t count, size_t minItems, uint32_t maxExecutionLanes,
                                CpuRangeTask task, void* context) noexcept {
  if (!task) return false;
  if (count == 0) return true;
  if (!g_workers.initialized || g_workers.workerCount == 0) {
    return task(context, 0, count, 0);
  }

  // Renderer and game phases intentionally share one persistent producer. If
  // a callback ever recurses into another parallel-for, keep the inner call on
  // the caller instead of deadlocking on this pool's completion semaphores.
  if (g_workers.busy.test_and_set(std::memory_order_acquire))
    return task(context, 0, count, 0);
  struct BusyGuard {
    ~BusyGuard() { g_workers.busy.clear(std::memory_order_release); }
  } busyGuard;

  minItems = std::max<size_t>(1, minItems);

  // `minItems` is the minimum useful amount of work per execution lane, not
  // merely the threshold for waking every worker.  Waking two Vita pthreads for
  // a ~150-vertex draw used to split it into ~50-vertex chunks, where condition
  // variable traffic cost more than the decode/transform work itself.  Scale the
  // active lane count with the draw instead and keep small draws on the caller.
  // A dedicated semaphore per lane replaces the shared condition-variable
  // barrier, which stalled under sustained Vita hardware workloads.
  const uint32_t availableLanes = g_workers.workerCount + 1;
  const uint32_t maxLanes = maxExecutionLanes==0?availableLanes:
      std::max<uint32_t>(1,std::min(maxExecutionLanes,availableLanes));
  const uint32_t usefulLanes = static_cast<uint32_t>(std::min<size_t>(
      maxLanes, std::max<size_t>(1, count / minItems)));
  if (usefulLanes <= 1) return task(context, 0, count, 0);

  const uint32_t lanes = usefulLanes;
  const size_t chunk = count / lanes + (count % lanes != 0);
  const size_t mainEnd = std::min(count, chunk);

  const uint32_t activeWorkers=lanes-1;
  std::array<bool,MaxWorkers> dispatched{};
  bool workersResult=true;
  for (uint32_t i = 0; i < activeWorkers; ++i) {
    auto &lane=g_workers.lanes[i];
    const size_t begin = std::min(count, chunk * static_cast<size_t>(i + 1));
    const size_t end = std::min(count, begin + chunk);
    lane.task=task;lane.context=context;lane.begin=begin;lane.end=end;
    lane.result=false;
    lane.state.store(1,std::memory_order_release);
    dispatched[i]=sceKernelSignalSema(lane.wake,1)>=0;
    if(!dispatched[i]) workersResult=false;
  }

  const bool mainResult = task(context, 0, mainEnd, 0);

  for (uint32_t i = 0; i < activeWorkers; ++i) if(dispatched[i]) {
    auto &lane=g_workers.lanes[i];
    // Consume exactly one acknowledgement for every published job, even if
    // state already says done. Conditional signalling can leave a stale token
    // that lets the producer reuse a later job's context while it is running.
    // Separate sleeping/state flags also permit a lost wake on ARM.
    while(sceKernelWaitSema(lane.done,1,nullptr)<0)sceKernelDelayThread(100);
    const bool completed=lane.state.load(std::memory_order_acquire)==2;
    workersResult=workersResult && completed && lane.result;
    lane.task=nullptr;lane.context=nullptr;
    lane.state.store(0,std::memory_order_release);
  }
  return mainResult && workersResult;
}

bool cpu_parallel_for_min(size_t count, size_t minItems, CpuRangeTask task, void* context) noexcept {
  return cpu_parallel_for_min_lanes(count,minItems,g_workers.workerCount+1,task,context);
}

bool cpu_parallel_for(size_t count, CpuRangeTask task, void* context) noexcept {
  return cpu_parallel_for_min_lanes(count,g_workers.minItems,g_workers.defaultExecutionLanes,task,context);
}

uint32_t cpu_worker_threads() noexcept { return g_workers.workerCount; }
uint32_t cpu_execution_lanes() noexcept { return g_workers.workerCount + 1; }
size_t cpu_parallel_min_items() noexcept { return g_workers.minItems; }

#else
namespace {
size_t g_minItems = 512;
}

bool probe_system_core() noexcept { return false; }
bool initialize_cpu_workers(uint32_t, size_t minItems, uint32_t, bool) noexcept {
  g_minItems = std::max<size_t>(1, minItems);
  return true;
}
void shutdown_cpu_workers() noexcept {}
bool cpu_parallel_for(size_t count, CpuRangeTask task, void* context) noexcept {
  return task ? task(context, 0, count, 0) : false;
}
bool cpu_parallel_for_min(size_t count, size_t, CpuRangeTask task, void* context) noexcept {
  return task ? task(context, 0, count, 0) : false;
}
bool cpu_parallel_for_min_lanes(size_t count, size_t, uint32_t, CpuRangeTask task, void* context) noexcept {
  return task ? task(context, 0, count, 0) : false;
}
uint32_t cpu_worker_threads() noexcept { return 0; }
uint32_t cpu_execution_lanes() noexcept { return 1; }
size_t cpu_parallel_min_items() noexcept { return g_minItems; }
#endif

} // namespace aurora::vita::gfx
