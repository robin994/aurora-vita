#include "vita_cpu_workers.hpp"
#include "../vita_log.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>

#if defined(__vita__)
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>
#endif

namespace aurora::vita::gfx {

#if defined(__vita__)
namespace {
constexpr uint32_t MaxWorkers = MaxWorkerThreads;
constexpr size_t WorkerStackBytes = 128u * 1024u;

struct CpuWorkerState {
  struct Lane {
    SceUID thread = -1, wake = -1, done = -1;
    std::atomic<bool> stop{false};
    std::atomic<unsigned> state{0}; // release/acquire publication: idle, ready, done
    CpuRangeTask task = nullptr;
    void *context = nullptr;
    size_t begin = 0, end = 0;
    bool result = true;
    std::atomic<int> cpuId{-1};
    std::atomic<int> affinityMask{-1};
  };
  std::array<Lane, MaxWorkers> lanes{};
  bool initialized = false;
  uint32_t workerCount = 0;
  uint32_t defaultExecutionLanes = 1;
  size_t minItems = 512;
  std::atomic_flag busy = ATOMIC_FLAG_INIT;
};

CpuWorkerState g_workers{};

int cpu_worker_main(SceSize, void* opaque) {
  const uint32_t index = *static_cast<const uint32_t*>(opaque);
  auto &lane = g_workers.lanes[index];
  lane.cpuId.store(sceKernelGetCpuId(),std::memory_order_relaxed);
  lane.affinityMask.store(sceKernelGetThreadCpuAffinityMask(sceKernelGetThreadId()),std::memory_order_relaxed);
  // Reuse the completion semaphore for the one-shot startup acknowledgement.
  // initialize_cpu_workers consumes this token before any range can be posted.
  if(sceKernelSignalSema(lane.done,1)<0)return 0;
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
  lane.cpuId.store(-1);
  lane.affinityMask.store(-1);
}
} // namespace

bool initialize_cpu_workers(uint32_t workerThreads, size_t minItems, uint32_t defaultExecutionLanes) noexcept {
  if (g_workers.initialized) shutdown_cpu_workers();

  g_workers.workerCount = 0;
  g_workers.minItems = std::max<size_t>(1, minItems);
  const uint32_t requested = std::min(workerThreads, MaxWorkers);
  constexpr std::array<int,MaxWorkers> WorkerAffinities{{
      SCE_KERNEL_CPU_MASK_USER_2,
      SCE_KERNEL_CPU_MASK_USER_1,
      SCE_KERNEL_CPU_MASK_SYSTEM}};
  constexpr std::array<int,MaxWorkers> WorkerPriorities{{
      0x10000110,
      0x10000180,
      0x100001c0}};
  for (uint32_t i = 0; i < requested; ++i) {
    auto &lane=g_workers.lanes[i];
    lane.stop.store(false);lane.state.store(0);
    lane.cpuId.store(-1);lane.affinityMask.store(-1);
    lane.wake=sceKernelCreateSema("aurora_cpu_wake",0,0,1,nullptr);
    lane.done=sceKernelCreateSema("aurora_cpu_done",0,0,1,nullptr);
    if(lane.wake<0 || lane.done<0) {destroy_lane(lane);break;}
    char name[24];std::snprintf(name,sizeof(name),"aurora_cpu_%u",i+1);
    // Lane 1 stays on CPU2, lane 2 on CPU1 below the audio thread, and lane 3
    // targets the system-reserved core. The third mapping is accepted only
    // after the running thread proves both affinity and physical CPU id.
    const int affinity=WorkerAffinities[i];
    const int priority=WorkerPriorities[i];
    lane.thread=sceKernelCreateThread(name,cpu_worker_main,priority,
                                     WorkerStackBytes,0,affinity,nullptr);
    const int createResult=lane.thread;
    const int startResult=lane.thread>=0?sceKernelStartThread(lane.thread,sizeof(i),&i):lane.thread;
    if(lane.thread<0 || startResult<0) {
      if(i==2)AURORA_VITA_LOG_INFO(
          "[aurora-vita] cpu3 probe unavailable create=0x%08x start=0x%08x; keeping CPU0-2 topology\n",
          static_cast<unsigned>(createResult),static_cast<unsigned>(startResult));
      destroy_lane(lane);break;
    }
    const int startupWait=sceKernelWaitSema(lane.done,1,nullptr);
    const int actualCpu=lane.cpuId.load(std::memory_order_relaxed);
    const int actualAffinity=lane.affinityMask.load(std::memory_order_relaxed);
    if(startupWait<0 || (i==2 && (actualCpu!=3 || actualAffinity!=SCE_KERNEL_CPU_MASK_SYSTEM))) {
      if(i==2)AURORA_VITA_LOG_INFO(
          "[aurora-vita] cpu3 probe rejected wait=0x%08x cpu=%d affinity=0x%08x expected=0x%08x; keeping CPU0-2 topology\n",
          static_cast<unsigned>(startupWait),actualCpu,static_cast<unsigned>(actualAffinity),
          static_cast<unsigned>(SCE_KERNEL_CPU_MASK_SYSTEM));
      lane.stop.store(true,std::memory_order_release);
      sceKernelSignalSema(lane.wake,1);
      sceKernelWaitThreadEnd(lane.thread,nullptr,nullptr);
      destroy_lane(lane);
      break;
    }
    AURORA_VITA_LOG_INFO(
        "[aurora-vita] cpu helper lane=%u cpu=%d affinity=0x%08x priority=0x%08x\n",
        i+1,actualCpu,static_cast<unsigned>(actualAffinity),static_cast<unsigned>(priority));
    ++g_workers.workerCount;
  }

  g_workers.initialized = true;
  const uint32_t availableLanes=g_workers.workerCount+1;
  g_workers.defaultExecutionLanes=defaultExecutionLanes==0?availableLanes:
      std::max<uint32_t>(1,std::min(defaultExecutionLanes,availableLanes));
  AURORA_VITA_LOG_INFO(
      "[aurora-vita] cpu workers=%u lanes=%u default_lanes=%u sync=paired_semaphores helpers=cpu2,cpu1-lowpri,cpu3-probed parallel_min_items=%llu\n",
      g_workers.workerCount,g_workers.workerCount+1,g_workers.defaultExecutionLanes,
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
    if(!dispatched[i]) {
      workersResult=false;
      lane.task=nullptr;lane.context=nullptr;
      lane.state.store(0,std::memory_order_release);
    }
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
bool cpu_core3_available() noexcept {
  return g_workers.workerCount>=3 && g_workers.lanes[2].cpuId.load(std::memory_order_relaxed)==3 &&
      g_workers.lanes[2].affinityMask.load(std::memory_order_relaxed)==SCE_KERNEL_CPU_MASK_SYSTEM;
}
int cpu_core3_cpu_id() noexcept {
  return g_workers.workerCount>=3?g_workers.lanes[2].cpuId.load(std::memory_order_relaxed):-1;
}
int cpu_core3_affinity_mask() noexcept {
  return g_workers.workerCount>=3?g_workers.lanes[2].affinityMask.load(std::memory_order_relaxed):-1;
}

#else
namespace {
size_t g_minItems = 512;
}

bool initialize_cpu_workers(uint32_t, size_t minItems, uint32_t) noexcept {
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
bool cpu_core3_available() noexcept { return false; }
int cpu_core3_cpu_id() noexcept { return -1; }
int cpu_core3_affinity_mask() noexcept { return -1; }
#endif

} // namespace aurora::vita::gfx
