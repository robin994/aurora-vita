#include "vita_cpu_workers.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>

#if defined(__vita__)
#include <psp2/kernel/threadmgr.h>
#endif

namespace aurora::vita::gfx {

#if defined(__vita__)
namespace {
constexpr uint32_t MaxWorkers = 2;
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
  };
  std::array<Lane, MaxWorkers> lanes{};
  bool initialized = false;
  uint32_t workerCount = 0;
  size_t minItems = 512;
};

CpuWorkerState g_workers{};

int cpu_worker_main(SceSize, void* opaque) {
  const uint32_t index = *static_cast<const uint32_t*>(opaque);
  auto &lane = g_workers.lanes[index];
  for (;;) {
    const int wait = sceKernelWaitSema(lane.wake, 1, nullptr);
    if (lane.stop.load(std::memory_order_acquire)) return 0;
    if (wait < 0) { sceKernelDelayThread(100); continue; }
    if (lane.state.load(std::memory_order_acquire) != 1) continue;
    lane.result = lane.task && lane.task(lane.context, lane.begin, lane.end, index + 1);
    lane.state.store(2, std::memory_order_release);
    sceKernelSignalSema(lane.done, 1);
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

bool initialize_cpu_workers(uint32_t workerThreads, size_t minItems) noexcept {
  if (g_workers.initialized) shutdown_cpu_workers();

  g_workers.workerCount = 0;
  g_workers.minItems = std::max<size_t>(1, minItems);
  const uint32_t requested = std::min(workerThreads, MaxWorkers);
  for (uint32_t i = 0; i < requested; ++i) {
    auto &lane=g_workers.lanes[i];
    lane.stop.store(false);lane.state.store(0);
    lane.wake=sceKernelCreateSema("aurora_cpu_wake",0,0,1,nullptr);
    lane.done=sceKernelCreateSema("aurora_cpu_done",0,0,1,nullptr);
    if(lane.wake<0 || lane.done<0) {destroy_lane(lane);break;}
    char name[24];std::snprintf(name,sizeof(name),"aurora_cpu_%u",i+1);
    const int affinity=i==0?SCE_KERNEL_CPU_MASK_USER_1:SCE_KERNEL_CPU_MASK_USER_2;
    lane.thread=sceKernelCreateThread(name,cpu_worker_main,0x10000110,
                                     WorkerStackBytes,0,affinity,nullptr);
    if(lane.thread<0 || sceKernelStartThread(lane.thread,sizeof(i),&i)<0) {
      destroy_lane(lane);break;
    }
    ++g_workers.workerCount;
  }

  g_workers.initialized = true;
  std::fprintf(stderr,
               "[aurora-vita] cpu workers=%u lanes=%u sync=native_semaphores cores=1,2 parallel_min_items=%llu\n",
               g_workers.workerCount, g_workers.workerCount + 1,
               static_cast<unsigned long long>(g_workers.minItems));
  return true;
}

void shutdown_cpu_workers() noexcept {
  if (!g_workers.initialized) return;
  // The single producer calls shutdown only outside cpu_parallel_for, so no
  // callback can still own a caller's capture/staging data at this boundary.
  for (uint32_t i = 0; i < g_workers.workerCount; ++i) {
    auto &lane=g_workers.lanes[i];
    lane.stop.store(true,std::memory_order_release);
    sceKernelSignalSema(lane.wake,1);
  }
  for (uint32_t i = 0; i < g_workers.workerCount; ++i) {
    auto &lane=g_workers.lanes[i];
    sceKernelWaitThreadEnd(lane.thread,nullptr,nullptr);
    destroy_lane(lane);
  }
  g_workers.initialized = false;
  g_workers.workerCount = 0;
}

bool cpu_parallel_for(size_t count, CpuRangeTask task, void* context) noexcept {
  if (!task) return false;
  if (count == 0) return true;
  if (!g_workers.initialized || g_workers.workerCount == 0) {
    return task(context, 0, count, 0);
  }

  // `minItems` is the minimum useful amount of work per execution lane, not
  // merely the threshold for waking every worker.  Waking two Vita pthreads for
  // a ~150-vertex draw used to split it into ~50-vertex chunks, where condition
  // variable traffic cost more than the decode/transform work itself.  Scale the
  // active lane count with the draw instead and keep small draws on the caller.
  // A dedicated semaphore per lane replaces the shared condition-variable
  // barrier, which stalled under sustained Vita hardware workloads.
  const uint32_t maxLanes = g_workers.workerCount + 1;
  const uint32_t usefulLanes = static_cast<uint32_t>(std::min<size_t>(
      maxLanes, std::max<size_t>(1, count / g_workers.minItems)));
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
    // Consume exactly one completion token before reusing any job memory.
    // Inactive lanes receive no wake token and cannot acknowledge a later job.
    while(sceKernelWaitSema(lane.done,1,nullptr)<0) sceKernelDelayThread(100);
    const bool completed=lane.state.load(std::memory_order_acquire)==2;
    workersResult=workersResult && completed && lane.result;
    lane.task=nullptr;lane.context=nullptr;
    lane.state.store(0,std::memory_order_release);
  }
  return mainResult && workersResult;
}

uint32_t cpu_worker_threads() noexcept { return g_workers.workerCount; }
uint32_t cpu_execution_lanes() noexcept { return g_workers.workerCount + 1; }
size_t cpu_parallel_min_items() noexcept { return g_workers.minItems; }

#else
namespace {
size_t g_minItems = 512;
}

bool initialize_cpu_workers(uint32_t, size_t minItems) noexcept {
  g_minItems = std::max<size_t>(1, minItems);
  return true;
}
void shutdown_cpu_workers() noexcept {}
bool cpu_parallel_for(size_t count, CpuRangeTask task, void* context) noexcept {
  return task ? task(context, 0, count, 0) : false;
}
uint32_t cpu_worker_threads() noexcept { return 0; }
uint32_t cpu_execution_lanes() noexcept { return 1; }
size_t cpu_parallel_min_items() noexcept { return g_minItems; }
#endif

} // namespace aurora::vita::gfx
