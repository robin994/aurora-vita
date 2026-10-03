#include "vita_cpu_workers.hpp"
#include "vita_cpu_dispatch_plan.hpp"
#include "../vita_log.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <limits>

#if defined(__vita__)
#include "../vita_thread_utils.hpp"
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
    bool created = false;
    CpuWorkerProbeLane probe{};
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
  struct Core3Budget {
    CpuCore3BudgetConfig config{};
    bool baselineValid = false;
    bool telemetryValid = false;
    uint32_t targetPercent = 0;
    uint64_t lastWallUs = 0;
    uint64_t lastIdleRaw = 0;
    int64_t shortCreditUs = 0;
    int64_t longCreditUs = 0;
    uint64_t shortCapacityUs = 0;
    uint64_t longCapacityUs = 0;
    uint64_t reservedSinceSampleUs = 0;
    uint64_t chunks = 0;
    uint64_t denied = 0;
    uint64_t telemetryFailures = 0;
    uint64_t overruns = 0;
    uint64_t totalChunkUs = 0;
    uint32_t maxChunkUs = 0;
    uint32_t lastTotalPercentX100 = 0;
    uint64_t estimatedUsPerItemX1024 = 0;
    struct Published {
      std::atomic<uint32_t> configured{0},telemetryValid{0},dispatchAllowed{0},targetPercent{0};
      std::atomic<uint32_t> lastTotalPercentX100{0},maxChunkUs{0},estimatedUsPerItemX1024{0};
      std::atomic<uint64_t> shortCreditUs{0},shortCapacityUs{0},longCreditUs{0},longCapacityUs{0};
      std::atomic<uint64_t> chunks{0},denied{0},telemetryFailures{0},overruns{0},totalChunkUs{0};
    } published{};
  } core3Budget{};
  struct VertexParallelStats {
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> dynamicCalls{0};
    std::atomic<uint64_t> totalWallUs{0};
    std::atomic<uint64_t> callerWaitUs{0};
    std::array<std::atomic<uint64_t>, MaxExecutionLanes> laneItems{};
    std::array<std::atomic<uint64_t>, MaxExecutionLanes> laneChunks{};
    std::array<std::atomic<uint64_t>, MaxExecutionLanes> laneWorkUs{};
  } vertexStats{};
  bool initialized = false;
  bool distinctCoreDispatch = false;
  uint32_t workerCount = 0;
  uint32_t defaultExecutionLanes = 1;
  size_t minItems = 512;
  std::atomic_flag busy = ATOMIC_FLAG_INIT;
};

CpuWorkerState g_workers{};

void publish_core3_budget() noexcept {
  auto& b=g_workers.core3Budget;
  auto& p=b.published;
  const bool belowObservedTarget=b.lastTotalPercentX100 < b.targetPercent*100u;
  const bool dispatchAllowed=b.config.enabled&&b.telemetryValid&&g_workers.lanes[2].created&&
      b.targetPercent!=0&&belowObservedTarget&&b.shortCreditUs>0&&b.longCreditUs>0;
  p.configured.store(b.config.enabled?1u:0u,std::memory_order_relaxed);
  p.telemetryValid.store(b.telemetryValid?1u:0u,std::memory_order_relaxed);
  p.dispatchAllowed.store(dispatchAllowed?1u:0u,std::memory_order_relaxed);
  p.targetPercent.store(b.targetPercent,std::memory_order_relaxed);
  p.lastTotalPercentX100.store(b.lastTotalPercentX100,std::memory_order_relaxed);
  p.shortCreditUs.store(b.shortCreditUs>0?static_cast<uint64_t>(b.shortCreditUs):0u,std::memory_order_relaxed);
  p.shortCapacityUs.store(b.shortCapacityUs,std::memory_order_relaxed);
  p.longCreditUs.store(b.longCreditUs>0?static_cast<uint64_t>(b.longCreditUs):0u,std::memory_order_relaxed);
  p.longCapacityUs.store(b.longCapacityUs,std::memory_order_relaxed);
  p.chunks.store(b.chunks,std::memory_order_relaxed);
  p.denied.store(b.denied,std::memory_order_relaxed);
  p.telemetryFailures.store(b.telemetryFailures,std::memory_order_relaxed);
  p.overruns.store(b.overruns,std::memory_order_relaxed);
  p.totalChunkUs.store(b.totalChunkUs,std::memory_order_relaxed);
  p.maxChunkUs.store(b.maxChunkUs,std::memory_order_relaxed);
  p.estimatedUsPerItemX1024.store(static_cast<uint32_t>(std::min<uint64_t>(
      b.estimatedUsPerItemX1024,std::numeric_limits<uint32_t>::max())),std::memory_order_relaxed);
}

bool read_core3_counters(uint64_t& wallUs,uint64_t& idleRaw) noexcept {
  const int64_t now=sceKernelGetSystemTimeWide();
  SceKernelSystemInfo info{};info.size=sizeof(info);
  if(now<=0 || sceKernelGetSystemInfo(&info)<0 ||
     (info.activeCpuMask&SCE_KERNEL_CPU_MASK_SYSTEM)==0)return false;
  wallUs=static_cast<uint64_t>(now);
  idleRaw=static_cast<uint64_t>(info.cpuInfo[3].idleClock);
  return true;
}

void reset_core3_budget(const CpuCore3BudgetConfig& requested) noexcept {
  auto& b=g_workers.core3Budget;
  b.config=requested;
  b.config.maxTotalPercent=std::min<uint32_t>(100,std::max<uint32_t>(1,b.config.maxTotalPercent));
  b.config.guardPercent=std::min(b.config.guardPercent,b.config.maxTotalPercent);
  b.config.shortWindowMs=std::max<uint32_t>(10,b.config.shortWindowMs);
  b.config.longWindowMs=std::max(b.config.shortWindowMs,b.config.longWindowMs);
  b.config.chunkTargetUs=std::max<uint32_t>(50,b.config.chunkTargetUs);
  b.config.samplePeriodUs=std::max<uint32_t>(1000,b.config.samplePeriodUs);
  b.targetPercent=b.config.maxTotalPercent>b.config.guardPercent?
      b.config.maxTotalPercent-b.config.guardPercent:0;
  b.baselineValid=false;b.telemetryValid=false;b.lastWallUs=0;b.lastIdleRaw=0;
  b.shortCreditUs=b.longCreditUs=0;b.reservedSinceSampleUs=0;
  b.shortCapacityUs=(uint64_t(b.config.shortWindowMs)*1000u*b.targetPercent)/100u;
  b.longCapacityUs=(uint64_t(b.config.longWindowMs)*1000u*b.targetPercent)/100u;
  b.chunks=b.denied=b.telemetryFailures=b.overruns=b.totalChunkUs=0;
  b.maxChunkUs=b.lastTotalPercentX100=0;b.estimatedUsPerItemX1024=0;
  if(b.config.enabled && g_workers.lanes[2].created) {
    b.baselineValid=read_core3_counters(b.lastWallUs,b.lastIdleRaw);
    b.telemetryValid=b.baselineValid;
    if(!b.baselineValid)++b.telemetryFailures;
  }
  publish_core3_budget();
}

void invalidate_core3_budget_sample() noexcept {
  auto& b=g_workers.core3Budget;
  b.baselineValid=false;b.telemetryValid=false;
  b.shortCreditUs=b.longCreditUs=0;b.reservedSinceSampleUs=0;
  ++b.telemetryFailures;
  publish_core3_budget();
}

bool sample_core3_budget(bool force=false) noexcept {
  auto& b=g_workers.core3Budget;
  if(!b.config.enabled || !g_workers.lanes[2].created || b.targetPercent==0)return false;
  uint64_t wallUs=0,idleRaw=0;
  if(!read_core3_counters(wallUs,idleRaw)) {invalidate_core3_budget_sample();return false;}
  if(!b.baselineValid) {
    b.lastWallUs=wallUs;b.lastIdleRaw=idleRaw;b.baselineValid=true;b.telemetryValid=true;
    publish_core3_budget();return true;
  }
  if(wallUs<=b.lastWallUs || idleRaw<b.lastIdleRaw) {invalidate_core3_budget_sample();return false;}
  const uint64_t deltaWall=wallUs-b.lastWallUs;
  if(!force && deltaWall<b.config.samplePeriodUs)return b.telemetryValid;
  const uint64_t deltaIdle=idleRaw-b.lastIdleRaw;
  const uint64_t tolerance=std::max<uint64_t>(100,deltaWall/20u);
  if(deltaIdle>deltaWall+tolerance) {invalidate_core3_budget_sample();return false;}
  const uint64_t busyUs=deltaIdle>=deltaWall?0u:deltaWall-deltaIdle;
  const uint64_t earnedUs=(deltaWall*b.targetPercent)/100u;
  const uint64_t refund=b.reservedSinceSampleUs;
  const auto updateCredit=[&](int64_t current,uint64_t capacity) noexcept {
    const int64_t added=static_cast<int64_t>(std::min<uint64_t>(earnedUs+refund,
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max())));
    const int64_t spent=static_cast<int64_t>(std::min<uint64_t>(busyUs,
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max())));
    int64_t next=current+added-spent;
    if(next<0)next=0;
    if(static_cast<uint64_t>(next)>capacity)next=static_cast<int64_t>(capacity);
    return next;
  };
  b.shortCreditUs=updateCredit(b.shortCreditUs,b.shortCapacityUs);
  b.longCreditUs=updateCredit(b.longCreditUs,b.longCapacityUs);
  b.reservedSinceSampleUs=0;
  b.lastWallUs=wallUs;b.lastIdleRaw=idleRaw;b.telemetryValid=true;
  b.lastTotalPercentX100=deltaWall?static_cast<uint32_t>(std::min<uint64_t>(
      10000u,(busyUs*10000u)/deltaWall)):0u;
  if(b.lastTotalPercentX100>b.config.maxTotalPercent*100u)++b.overruns;
  publish_core3_budget();
  return true;
}

uint32_t core3_predicted_us(size_t items) noexcept {
  const auto& b=g_workers.core3Budget;
  if(!items)return 0;
  if(!b.estimatedUsPerItemX1024)return b.config.chunkTargetUs;
  const uint64_t predicted=(uint64_t(items)*b.estimatedUsPerItemX1024+1023u)/1024u;
  return static_cast<uint32_t>(std::min<uint64_t>(std::max<uint64_t>(1,predicted),UINT32_MAX));
}

size_t core3_chunk_items(size_t baseItems) noexcept {
  auto& b=g_workers.core3Budget;
  if(!b.estimatedUsPerItemX1024)return baseItems;
  const uint64_t target=(uint64_t(b.config.chunkTargetUs)*1024u)/b.estimatedUsPerItemX1024;
  const size_t floor=std::max<size_t>(1,baseItems/4u);
  return std::max(floor,std::min(baseItems,static_cast<size_t>(std::max<uint64_t>(1,target))));
}

bool reserve_core3_budget(uint32_t predictedUs) noexcept {
  auto& b=g_workers.core3Budget;
  if(!sample_core3_budget(false) || !b.telemetryValid || predictedUs==0) {
    ++b.denied;publish_core3_budget();return false;
  }
  if(b.lastTotalPercentX100>=b.targetPercent*100u) {
    ++b.denied;publish_core3_budget();return false;
  }
  if(b.shortCreditUs<static_cast<int64_t>(predictedUs) ||
     b.longCreditUs<static_cast<int64_t>(predictedUs)) {
    ++b.denied;publish_core3_budget();return false;
  }
  b.shortCreditUs-=predictedUs;b.longCreditUs-=predictedUs;
  b.reservedSinceSampleUs+=predictedUs;
  publish_core3_budget();
  return true;
}

void resize_core3_reservation(uint32_t fromUs,uint32_t toUs) noexcept {
  auto& b=g_workers.core3Budget;
  if(fromUs==toUs)return;
  if(toUs<fromUs) {
    const uint64_t refund=uint64_t(fromUs-toUs);
    b.shortCreditUs=std::min<int64_t>(static_cast<int64_t>(b.shortCapacityUs),b.shortCreditUs+static_cast<int64_t>(refund));
    b.longCreditUs=std::min<int64_t>(static_cast<int64_t>(b.longCapacityUs),b.longCreditUs+static_cast<int64_t>(refund));
    b.reservedSinceSampleUs=refund<=b.reservedSinceSampleUs?b.reservedSinceSampleUs-refund:0;
  } else {
    const uint64_t extra=uint64_t(toUs-fromUs);
    b.shortCreditUs=extra<=static_cast<uint64_t>(std::max<int64_t>(0,b.shortCreditUs))?
        b.shortCreditUs-static_cast<int64_t>(extra):0;
    b.longCreditUs=extra<=static_cast<uint64_t>(std::max<int64_t>(0,b.longCreditUs))?
        b.longCreditUs-static_cast<int64_t>(extra):0;
    b.reservedSinceSampleUs+=extra;
  }
  publish_core3_budget();
}

void finish_core3_chunk(uint32_t reservedUs,uint64_t actualUs,size_t items) noexcept {
  auto& b=g_workers.core3Budget;
  const uint32_t actual=static_cast<uint32_t>(std::min<uint64_t>(actualUs,UINT32_MAX));
  resize_core3_reservation(reservedUs,actual);
  ++b.chunks;b.totalChunkUs+=actual;b.maxChunkUs=std::max(b.maxChunkUs,actual);
  if(items&&actual) {
    const uint64_t sample=(uint64_t(actual)*1024u+items-1u)/items;
    b.estimatedUsPerItemX1024=b.estimatedUsPerItemX1024?
        (b.estimatedUsPerItemX1024*3u+sample)/4u:sample;
  }
  publish_core3_budget();
}

struct DynamicRangeContext {
  CpuRangeTask task=nullptr;
  void* taskContext=nullptr;
  std::atomic<size_t> next{0};
  size_t count=0;
  size_t baseChunk=1;
  bool profileVertex=false;
};

struct ProfiledRangeContext {
  CpuRangeTask task=nullptr;
  void* taskContext=nullptr;
};

bool profiled_range_worker(void* opaque,size_t begin,size_t end,uint32_t lane) noexcept {
  auto& job=*static_cast<ProfiledRangeContext*>(opaque);
  if(!job.task || lane>=MaxExecutionLanes)return false;
  const int64_t started=sceKernelGetSystemTimeWide();
  const bool result=job.task(job.taskContext,begin,end,lane);
  const int64_t finished=sceKernelGetSystemTimeWide();
  const uint64_t workUs=finished>started?static_cast<uint64_t>(finished-started):0u;
  g_workers.vertexStats.laneItems[lane].fetch_add(end-begin,std::memory_order_relaxed);
  g_workers.vertexStats.laneChunks[lane].fetch_add(1,std::memory_order_relaxed);
  g_workers.vertexStats.laneWorkUs[lane].fetch_add(workUs,std::memory_order_relaxed);
  return result;
}

bool dynamic_range_worker(void* opaque,size_t,size_t,uint32_t lane) noexcept {
  auto& job=*static_cast<DynamicRangeContext*>(opaque);
  bool result=true;
  const int64_t profileStarted=job.profileVertex?sceKernelGetSystemTimeWide():0;
  uint64_t profileItems=0;
  uint64_t profileChunks=0;
  for(;;) {
    size_t items=job.baseChunk;
    uint32_t reservedUs=0;
    if(lane==3) {
      items=core3_chunk_items(job.baseChunk);
      reservedUs=core3_predicted_us(items);
      if(!reserve_core3_budget(reservedUs))break;
    }
    const size_t begin=job.next.fetch_add(items,std::memory_order_relaxed);
    if(begin>=job.count) {
      if(lane==3)resize_core3_reservation(reservedUs,0);
      break;
    }
    const size_t end=std::min(job.count,begin+items);
    const size_t actualItems=end-begin;
    profileItems+=actualItems;
    ++profileChunks;
    if(lane==3 && actualItems!=items) {
      const uint32_t tailPrediction=core3_predicted_us(actualItems);
      resize_core3_reservation(reservedUs,tailPrediction);
      reservedUs=tailPrediction;
    }
    const int64_t started=lane==3?sceKernelGetSystemTimeWide():0;
    result=job.task(job.taskContext,begin,end,lane)&&result;
    if(lane==3) {
      const int64_t finished=sceKernelGetSystemTimeWide();
      const uint64_t actualUs=finished>started?static_cast<uint64_t>(finished-started):reservedUs;
      finish_core3_chunk(reservedUs,actualUs,actualItems);
    }
  }
  if(job.profileVertex && lane<MaxExecutionLanes) {
    const int64_t profileFinished=sceKernelGetSystemTimeWide();
    const uint64_t workUs=profileFinished>profileStarted?
        static_cast<uint64_t>(profileFinished-profileStarted):0u;
    g_workers.vertexStats.laneItems[lane].fetch_add(profileItems,std::memory_order_relaxed);
    g_workers.vertexStats.laneChunks[lane].fetch_add(profileChunks,std::memory_order_relaxed);
    g_workers.vertexStats.laneWorkUs[lane].fetch_add(workUs,std::memory_order_relaxed);
  }
  return result;
}

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
    if(wait<0){aurora::vita::thread::delay_us(100);continue;}
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
  lane.created=false;
  lane.task=nullptr;lane.context=nullptr;
  lane.state.store(0);
  lane.cpuId.store(-1);
  lane.affinityMask.store(-1);
}
} // namespace

bool initialize_cpu_workers(uint32_t workerThreads, size_t minItems, uint32_t defaultExecutionLanes,
                            const CpuCore3BudgetConfig& core3Budget, bool distinctCoreDispatch) noexcept {
  if (g_workers.initialized) shutdown_cpu_workers();

  g_workers.workerCount = 0;
  g_workers.distinctCoreDispatch = distinctCoreDispatch;
  g_workers.minItems = std::max<size_t>(1, minItems);
  g_workers.vertexStats.calls.store(0,std::memory_order_relaxed);
  g_workers.vertexStats.dynamicCalls.store(0,std::memory_order_relaxed);
  g_workers.vertexStats.totalWallUs.store(0,std::memory_order_relaxed);
  g_workers.vertexStats.callerWaitUs.store(0,std::memory_order_relaxed);
  for(uint32_t lane=0;lane<MaxExecutionLanes;++lane) {
    g_workers.vertexStats.laneItems[lane].store(0,std::memory_order_relaxed);
    g_workers.vertexStats.laneChunks[lane].store(0,std::memory_order_relaxed);
    g_workers.vertexStats.laneWorkUs[lane].store(0,std::memory_order_relaxed);
  }
  for(auto& lane:g_workers.lanes)lane.probe={};
  const uint32_t requested = std::min(workerThreads, MaxWorkers);
  constexpr std::array<int,MaxWorkers> WorkerAffinities{{
      SCE_KERNEL_CPU_MASK_USER_2,
      SCE_KERNEL_CPU_MASK_USER_1,
      SCE_KERNEL_CPU_MASK_SYSTEM}};
  constexpr std::array<int,MaxWorkers> WorkerPriorities{{
      64,
      65,
      65}};
  for (uint32_t i = 0; i < requested; ++i) {
    auto &lane=g_workers.lanes[i];
    lane.stop.store(false);lane.state.store(0);
    lane.cpuId.store(-1);lane.affinityMask.store(-1);
    lane.probe={};
    lane.probe.requestedAffinity=WorkerAffinities[i];
    lane.probe.priority=WorkerPriorities[i];
    lane.wake=sceKernelCreateSema("aurora_cpu_wake",0,0,1,nullptr);
    lane.done=sceKernelCreateSema("aurora_cpu_done",0,0,1,nullptr);
    lane.probe.wakeResult=lane.wake;
    lane.probe.doneResult=lane.done;
    if(lane.wake<0 || lane.done<0) {destroy_lane(lane);continue;}
    char name[24];std::snprintf(name,sizeof(name),"aurora_cpu_%u",i+1);
    // Use the simple 64/65 Vita thread priorities used by established ports.
    // Lane 1 stays on CPU2, lane 2 on CPU1, and lane 3 targets the
    // system-reserved core. CPU3 is accepted only after the running thread
    // proves both affinity and physical CPU id.
    const int affinity=WorkerAffinities[i];
    const int priority=WorkerPriorities[i];
    lane.thread=sceKernelCreateThread(name,cpu_worker_main,priority,
                                     WorkerStackBytes,0,affinity,nullptr);
    const int createResult=lane.thread;
    const int startResult=lane.thread>=0?sceKernelStartThread(lane.thread,sizeof(i),&i):lane.thread;
    lane.probe.createResult=createResult;
    lane.probe.startResult=startResult;
    if(lane.thread<0 || startResult<0) {
      if(i==2)AURORA_VITA_LOG_INFO(
          "[aurora-vita] cpu3 probe unavailable create=0x%08x start=0x%08x; keeping CPU0-2 topology\n",
          static_cast<unsigned>(createResult),static_cast<unsigned>(startResult));
      destroy_lane(lane);continue;
    }
    const int startupWait=sceKernelWaitSema(lane.done,1,nullptr);
    const int actualCpu=lane.cpuId.load(std::memory_order_relaxed);
    const int actualAffinity=lane.affinityMask.load(std::memory_order_relaxed);
    lane.probe.waitResult=startupWait;
    lane.probe.actualCpu=actualCpu;
    lane.probe.actualAffinity=actualAffinity;
    if(startupWait<0 || (i==2 && (actualCpu!=3 || actualAffinity!=SCE_KERNEL_CPU_MASK_SYSTEM))) {
      if(i==2)AURORA_VITA_LOG_INFO(
          "[aurora-vita] cpu3 probe rejected wait=0x%08x cpu=%d affinity=0x%08x expected=0x%08x; keeping CPU0-2 topology\n",
          static_cast<unsigned>(startupWait),actualCpu,static_cast<unsigned>(actualAffinity),
          static_cast<unsigned>(SCE_KERNEL_CPU_MASK_SYSTEM));
      lane.stop.store(true,std::memory_order_release);
      sceKernelSignalSema(lane.wake,1);
      sceKernelWaitThreadEnd(lane.thread,nullptr,nullptr);
      destroy_lane(lane);
      continue;
    }
    lane.created=true;
    lane.probe.created=true;
    AURORA_VITA_LOG_INFO(
        "[aurora-vita] cpu helper lane=%u cpu=%d affinity=0x%08x priority=0x%08x\n",
        i+1,actualCpu,static_cast<unsigned>(actualAffinity),static_cast<unsigned>(priority));
    ++g_workers.workerCount;
  }

  g_workers.initialized = true;
  g_workers.defaultExecutionLanes=defaultExecutionLanes==0?MaxExecutionLanes:
      std::max<uint32_t>(1,std::min(defaultExecutionLanes,MaxExecutionLanes));
  reset_core3_budget(core3Budget);
  AURORA_VITA_LOG_INFO(
      "[aurora-vita] cpu workers=%u lanes=%u default_lanes=%u sync=paired_semaphores helpers=cpu2,cpu1-lowpri,cpu3-probed distinct_cores=%u parallel_min_items=%llu\n",
      g_workers.workerCount,g_workers.workerCount+1,g_workers.defaultExecutionLanes,
      g_workers.distinctCoreDispatch?1u:0u,
      static_cast<unsigned long long>(g_workers.minItems));
  return true;
}

void shutdown_cpu_workers() noexcept {
  if (!g_workers.initialized) return;
  // The single producer calls shutdown only outside cpu_parallel_for, so no
  // callback can still own a caller's capture/staging data at this boundary.
  for (uint32_t i = 0; i < MaxWorkers; ++i) {
    auto &lane=g_workers.lanes[i];
    if(!lane.created)continue;
    lane.stop.store(true,std::memory_order_release);
    sceKernelSignalSema(lane.wake,1);
  }
  for (uint32_t i = 0; i < MaxWorkers; ++i) {
    auto &lane=g_workers.lanes[i];
    if(!lane.created)continue;
    sceKernelWaitThreadEnd(lane.thread,nullptr,nullptr);
    destroy_lane(lane);
  }
  g_workers.initialized = false;
  g_workers.workerCount = 0;
  reset_core3_budget({});
}

static bool cpu_parallel_for_min_lanes_impl(size_t count, size_t minItems, uint32_t maxExecutionLanes,
                                            CpuRangeTask task, void* context,bool profileVertex) noexcept {
  if (!task) return false;
  if (count == 0) return true;
  ProfiledRangeContext profiled{task,context};
  const auto runCaller=[&](size_t begin,size_t end) noexcept {
    return profileVertex?profiled_range_worker(&profiled,begin,end,0):task(context,begin,end,0);
  };
  if (!g_workers.initialized || g_workers.workerCount == 0) {
    return runCaller(0,count);
  }

  // Renderer and game phases intentionally share one persistent producer. If
  // a callback ever recurses into another parallel-for, keep the inner call on
  // the caller instead of deadlocking on this pool's completion semaphores.
  if (g_workers.busy.test_and_set(std::memory_order_acquire))
    return runCaller(0,count);
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
  const uint32_t maxLanes=maxExecutionLanes==0?MaxExecutionLanes:
      std::max<uint32_t>(1,std::min(maxExecutionLanes,MaxExecutionLanes));
  std::array<uint32_t,MaxWorkers> workerSlots{};
  uint32_t selectableWorkers=0;
  for(uint32_t slot=0;slot<MaxWorkers;++slot) {
    const uint32_t logicalLane=slot+1;
    if(logicalLane>=maxLanes || !g_workers.lanes[slot].created)continue;
    if(logicalLane==3 && !g_workers.core3Budget.config.enabled)continue;
    workerSlots[selectableWorkers++]=slot;
  }
  if(g_workers.distinctCoreDispatch) {
    std::array<int,MaxWorkerThreads> cpus{};cpus.fill(-1);
    for(uint32_t i=0;i<selectableWorkers;++i) {
      const auto slot=workerSlots[i];
      cpus[slot]=g_workers.lanes[slot].cpuId.load(std::memory_order_relaxed);
    }
    const auto selected=cpu_dispatch_plan(count,minItems,maxLanes,sceKernelGetCpuId(),cpus,MaxWorkers);
    selectableWorkers=selected.workerCount;
    for(uint32_t i=0;i<selectableWorkers;++i)workerSlots[i]=selected.workers[i];
  }
  const uint32_t availableLanes=selectableWorkers+1;
  const bool core3Dynamic=maxLanes>=MaxExecutionLanes && g_workers.lanes[2].created &&
      g_workers.core3Budget.config.enabled && count/minItems>=2u &&
      std::find(workerSlots.begin(),workerSlots.begin()+selectableWorkers,2u)!=workerSlots.begin()+selectableWorkers;
  if(core3Dynamic) {
    DynamicRangeContext job{};
    job.task=task;job.taskContext=context;job.count=count;job.baseChunk=minItems;job.profileVertex=profileVertex;
    if(profileVertex)g_workers.vertexStats.dynamicCalls.fetch_add(1,std::memory_order_relaxed);
    std::array<bool,MaxWorkers> dispatched{};
    bool workersResult=true;
    for(uint32_t worker=0;worker<selectableWorkers;++worker) {
      const uint32_t i=workerSlots[worker];
      auto& lane=g_workers.lanes[i];
      lane.task=dynamic_range_worker;lane.context=&job;lane.begin=lane.end=0;
      lane.result=false;lane.state.store(1,std::memory_order_release);
      dispatched[i]=sceKernelSignalSema(lane.wake,1)>=0;
      if(!dispatched[i]) {
        workersResult=false;lane.task=nullptr;lane.context=nullptr;
        lane.state.store(0,std::memory_order_release);
      }
    }
    const bool mainResult=dynamic_range_worker(&job,0,0,0);
    const int64_t waitStarted=profileVertex?sceKernelGetSystemTimeWide():0;
    for(uint32_t worker=0;worker<selectableWorkers;++worker) {
      const uint32_t i=workerSlots[worker];
      if(!dispatched[i])continue;
      auto& lane=g_workers.lanes[i];
      while(sceKernelWaitSema(lane.done,1,nullptr)<0)aurora::vita::thread::delay_us(100);
      const bool completed=lane.state.load(std::memory_order_acquire)==2;
      workersResult=workersResult&&completed&&lane.result;
      lane.task=nullptr;lane.context=nullptr;lane.state.store(0,std::memory_order_release);
    }
    if(profileVertex) {
      const int64_t waitFinished=sceKernelGetSystemTimeWide();
      if(waitFinished>waitStarted)g_workers.vertexStats.callerWaitUs.fetch_add(
          static_cast<uint64_t>(waitFinished-waitStarted),std::memory_order_relaxed);
    }
    return mainResult&&workersResult;
  }
  const uint32_t usefulLanes = static_cast<uint32_t>(std::min<size_t>(
      availableLanes, std::max<size_t>(1, count / minItems)));
  if (usefulLanes <= 1) return runCaller(0,count);

  CpuDispatchPlan plan{};
  plan.workerCount=usefulLanes-1;
  for(uint32_t i=0;i<plan.workerCount;++i)plan.workers[i]=workerSlots[i];
  if(!plan.workerCount)return runCaller(0,count);
  const uint32_t lanes = plan.lanes();
  const size_t chunk = count / lanes + (count % lanes != 0);
  const size_t mainEnd = g_workers.distinctCoreDispatch?plan.range(count,0).second:std::min(count,chunk);

  const uint32_t activeWorkers=lanes-1;
  std::array<bool,MaxWorkers> dispatched{};
  bool workersResult=true;
  for (uint32_t rank = 0; rank < activeWorkers; ++rank) {
    const uint32_t i=plan.workers[rank];
    auto &lane=g_workers.lanes[i];
    const auto range=plan.range(count,rank+1);
    const size_t begin = g_workers.distinctCoreDispatch?range.first:std::min(count,chunk*size_t(rank+1));
    const size_t end = g_workers.distinctCoreDispatch?range.second:begin+std::min(count-begin,chunk);
    lane.task=profileVertex?profiled_range_worker:task;
    lane.context=profileVertex?static_cast<void*>(&profiled):context;
    lane.begin=begin;lane.end=end;
    lane.result=false;
    lane.state.store(1,std::memory_order_release);
    dispatched[i]=sceKernelSignalSema(lane.wake,1)>=0;
    if(!dispatched[i]) {
      workersResult=false;
      lane.task=nullptr;lane.context=nullptr;
      lane.state.store(0,std::memory_order_release);
      // A rejected wake has no consumer. Complete the range locally, preserving
      // exact coverage while reporting the dispatch failure to the caller.
      task(context,begin,end,i+1);
    }
  }

  const bool mainResult = runCaller(0,mainEnd);

  const int64_t waitStarted=profileVertex?sceKernelGetSystemTimeWide():0;
  for (uint32_t i = 0; i < MaxWorkers; ++i) if(dispatched[i]) {
    auto &lane=g_workers.lanes[i];
    // Consume exactly one acknowledgement for every published job, even if
    // state already says done. Conditional signalling can leave a stale token
    // that lets the producer reuse a later job's context while it is running.
    // Separate sleeping/state flags also permit a lost wake on ARM.
    while(sceKernelWaitSema(lane.done,1,nullptr)<0)aurora::vita::thread::delay_us(100);
    const bool completed=lane.state.load(std::memory_order_acquire)==2;
    workersResult=workersResult && completed && lane.result;
    lane.task=nullptr;lane.context=nullptr;
    lane.state.store(0,std::memory_order_release);
  }
  if(profileVertex) {
    const int64_t waitFinished=sceKernelGetSystemTimeWide();
    if(waitFinished>waitStarted)g_workers.vertexStats.callerWaitUs.fetch_add(
        static_cast<uint64_t>(waitFinished-waitStarted),std::memory_order_relaxed);
  }
  return mainResult && workersResult;
}

bool cpu_parallel_for_min_lanes(size_t count, size_t minItems, uint32_t maxExecutionLanes,
                                CpuRangeTask task, void* context) noexcept {
  return cpu_parallel_for_min_lanes_impl(count,minItems,maxExecutionLanes,task,context,false);
}

bool cpu_parallel_for_min(size_t count, size_t minItems, CpuRangeTask task, void* context) noexcept {
  return cpu_parallel_for_min_lanes_impl(count,minItems,MaxExecutionLanes,task,context,false);
}

bool cpu_parallel_for(size_t count, CpuRangeTask task, void* context) noexcept {
  return cpu_parallel_for_min_lanes_impl(count,g_workers.minItems,g_workers.defaultExecutionLanes,task,context,false);
}

bool cpu_parallel_for_vertex(size_t count, CpuRangeTask task, void* context) noexcept {
  const uint32_t lanes=g_workers.core3Budget.config.enabled?MaxExecutionLanes:g_workers.defaultExecutionLanes;
  if(!runtime_diagnostics_enabled())
    return cpu_parallel_for_min_lanes_impl(count,g_workers.minItems,lanes,task,context,false);
  g_workers.vertexStats.calls.fetch_add(1,std::memory_order_relaxed);
  const int64_t started=sceKernelGetSystemTimeWide();
  const bool result=cpu_parallel_for_min_lanes_impl(count,g_workers.minItems,lanes,task,context,true);
  const int64_t finished=sceKernelGetSystemTimeWide();
  if(finished>started)g_workers.vertexStats.totalWallUs.fetch_add(
      static_cast<uint64_t>(finished-started),std::memory_order_relaxed);
  return result;
}

uint32_t cpu_worker_threads() noexcept { return g_workers.workerCount; }
uint32_t cpu_execution_lanes() noexcept { return g_workers.workerCount + 1; }
size_t cpu_parallel_min_items() noexcept { return g_workers.minItems; }
bool cpu_core3_available() noexcept {
  return g_workers.lanes[2].created && g_workers.lanes[2].cpuId.load(std::memory_order_relaxed)==3 &&
      g_workers.lanes[2].affinityMask.load(std::memory_order_relaxed)==SCE_KERNEL_CPU_MASK_SYSTEM;
}
int cpu_core3_cpu_id() noexcept {
  return g_workers.lanes[2].created?g_workers.lanes[2].cpuId.load(std::memory_order_relaxed):-1;
}
int cpu_core3_affinity_mask() noexcept {
  return g_workers.lanes[2].created?g_workers.lanes[2].affinityMask.load(std::memory_order_relaxed):-1;
}
CpuCore3BudgetSnapshot cpu_core3_budget_snapshot() noexcept {
  CpuCore3BudgetSnapshot out{};
  const auto& p=g_workers.core3Budget.published;
  out.configured=p.configured.load(std::memory_order_relaxed)!=0;
  out.telemetryValid=p.telemetryValid.load(std::memory_order_relaxed)!=0;
  out.dispatchAllowed=p.dispatchAllowed.load(std::memory_order_relaxed)!=0;
  out.targetPercent=p.targetPercent.load(std::memory_order_relaxed);
  out.lastTotalPercentX100=p.lastTotalPercentX100.load(std::memory_order_relaxed);
  out.shortCreditUs=p.shortCreditUs.load(std::memory_order_relaxed);
  out.shortCapacityUs=p.shortCapacityUs.load(std::memory_order_relaxed);
  out.longCreditUs=p.longCreditUs.load(std::memory_order_relaxed);
  out.longCapacityUs=p.longCapacityUs.load(std::memory_order_relaxed);
  out.chunks=p.chunks.load(std::memory_order_relaxed);
  out.denied=p.denied.load(std::memory_order_relaxed);
  out.telemetryFailures=p.telemetryFailures.load(std::memory_order_relaxed);
  out.overruns=p.overruns.load(std::memory_order_relaxed);
  out.totalChunkUs=p.totalChunkUs.load(std::memory_order_relaxed);
  out.maxChunkUs=p.maxChunkUs.load(std::memory_order_relaxed);
  out.estimatedUsPerItemX1024=p.estimatedUsPerItemX1024.load(std::memory_order_relaxed);
  return out;
}
CpuWorkerProbeSnapshot cpu_worker_probe_snapshot() noexcept {
  CpuWorkerProbeSnapshot out{};
  for(uint32_t i=0;i<MaxWorkers;++i)out.lanes[i]=g_workers.lanes[i].probe;
  return out;
}
CpuVertexParallelSnapshot cpu_vertex_parallel_snapshot() noexcept {
  CpuVertexParallelSnapshot out{};
  out.calls=g_workers.vertexStats.calls.load(std::memory_order_relaxed);
  out.dynamicCalls=g_workers.vertexStats.dynamicCalls.load(std::memory_order_relaxed);
  out.totalWallUs=g_workers.vertexStats.totalWallUs.load(std::memory_order_relaxed);
  out.callerWaitUs=g_workers.vertexStats.callerWaitUs.load(std::memory_order_relaxed);
  for(uint32_t lane=0;lane<MaxExecutionLanes;++lane) {
    out.laneItems[lane]=g_workers.vertexStats.laneItems[lane].load(std::memory_order_relaxed);
    out.laneChunks[lane]=g_workers.vertexStats.laneChunks[lane].load(std::memory_order_relaxed);
    out.laneWorkUs[lane]=g_workers.vertexStats.laneWorkUs[lane].load(std::memory_order_relaxed);
  }
  return out;
}

#else
namespace {
size_t g_minItems = 512;
}

bool initialize_cpu_workers(uint32_t, size_t minItems, uint32_t,const CpuCore3BudgetConfig&, bool) noexcept {
  g_minItems = std::max<size_t>(1, minItems);
  return true;
}
void shutdown_cpu_workers() noexcept {}
bool cpu_parallel_for(size_t count, CpuRangeTask task, void* context) noexcept {
  return task ? task(context, 0, count, 0) : false;
}
bool cpu_parallel_for_vertex(size_t count, CpuRangeTask task, void* context) noexcept {
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
CpuCore3BudgetSnapshot cpu_core3_budget_snapshot() noexcept { return {}; }
CpuWorkerProbeSnapshot cpu_worker_probe_snapshot() noexcept { return {}; }
CpuVertexParallelSnapshot cpu_vertex_parallel_snapshot() noexcept { return {}; }
#endif

} // namespace aurora::vita::gfx
