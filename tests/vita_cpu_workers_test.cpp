#include "gfx/vita_cpu_workers.hpp"
#include "vita_log.hpp"
#include "gfx/vita_cpu_dispatch_plan.hpp"
#include <limits>
#include <psp2/kernel/threadmgr.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {
struct Sema {bool wake=false;std::mutex mutex;std::condition_variable cv;int count=0,max=1;};
struct Thread {int(*entry)(SceSize,void*);std::thread thread;int affinity=0;int id=-1;};
std::mutex registryMutex;
std::map<int,std::shared_ptr<Sema>> semas;
std::map<int,std::unique_ptr<Thread>> threads;
int nextId=1;
std::atomic<unsigned> semaErrors{0};
std::atomic<bool> failSystemThreadCreate{false};
std::atomic<bool> failUser1ThreadCreate{false};
std::atomic<bool> failSystemInfo{false};
std::atomic<uint64_t> fakeSystemUs{100000};
std::atomic<uint64_t> fakeIdle3Us{100000};
std::atomic<bool> failNextWake{false};
std::atomic<bool> rejectSystemCpuId{false};
thread_local int currentThreadId=0;
thread_local int currentCpuId=0;
thread_local int currentAffinity=0;
std::shared_ptr<Sema> semaphore(int id) {
  std::lock_guard lock(registryMutex);return semas.at(id);
}
void require(bool ok,const char* reason) {
  if(!ok){std::fprintf(stderr,"worker regression: %s\n",reason);std::abort();}
}
}
SceUID sceKernelCreateSema(const char* name,int,int count,int max,void*) {
  std::lock_guard lock(registryMutex);auto s=std::make_shared<Sema>();s->count=count;s->max=max;s->wake=std::strcmp(name,"aurora_cpu_wake")==0;
  const int id=nextId++;semas.emplace(id,std::move(s));return id;
}
int sceKernelDeleteSema(SceUID id) {std::lock_guard lock(registryMutex);semas.erase(id);return 0;}
int sceKernelWaitSema(SceUID id,int count,void*) {
  auto s=semaphore(id);std::unique_lock lock(s->mutex);
  require(s->cv.wait_for(lock,std::chrono::seconds(3),[&]{return s->count>=count;}),"lost wake/completion (3 s timeout)");
  s->count-=count;return 0;
}
int sceKernelSignalSema(SceUID id,int count) {
  auto s=semaphore(id);std::lock_guard lock(s->mutex);
  if(s->wake&&failNextWake.exchange(false))return -1;
  if(s->count+count>s->max){++semaErrors;return -1;}
  s->count+=count;s->cv.notify_one();return 0;
}
SceUID sceKernelCreateThread(const char*,int(*entry)(SceSize,void*),int priority,SceSize,int,int affinity,void*) {
  if(priority>0x10000110)return static_cast<SceUID>(0x80028023u);
  if(affinity==SCE_KERNEL_CPU_MASK_SYSTEM&&failSystemThreadCreate.load())return -0x1234;
  if(affinity==SCE_KERNEL_CPU_MASK_USER_1&&failUser1ThreadCreate.load())return -0x2345;
  const int id=nextId++;auto t=std::make_unique<Thread>();t->entry=entry;t->affinity=affinity;t->id=id;
  threads.emplace(id,std::move(t));return id;
}
int sceKernelStartThread(SceUID id,SceSize size,void* args) {
  auto& t=*threads.at(id);std::vector<unsigned char> copy(size);std::memcpy(copy.data(),args,size);
  const int affinity=t.affinity;
  t.thread=std::thread([entry=t.entry,copy=std::move(copy),id,affinity]() mutable {
    currentThreadId=id;currentAffinity=affinity;
    currentCpuId=affinity==SCE_KERNEL_CPU_MASK_SYSTEM?(rejectSystemCpuId.load()?2:3):
        (affinity==SCE_KERNEL_CPU_MASK_USER_2?2:(affinity==SCE_KERNEL_CPU_MASK_USER_1?1:0));
    entry(copy.size(),copy.data());
  });return 0;
}
int sceKernelWaitThreadEnd(SceUID id,void*,void*) {threads.at(id)->thread.join();return 0;}
int sceKernelDeleteThread(SceUID id) {threads.erase(id);return 0;}
int sceKernelDelayThread(unsigned us) {
  require(us>0,"non-positive delay syscall");
  std::this_thread::sleep_for(std::chrono::microseconds(us));return 0;
}
int sceKernelGetCpuId(void) {return currentCpuId;}
SceUID sceKernelGetThreadId(void) {return currentThreadId;}
int sceKernelGetThreadCpuAffinityMask(SceUID id) {
  if(id==currentThreadId)return currentAffinity;
  std::lock_guard lock(registryMutex);
  const auto it=threads.find(id);return it==threads.end()?-1:it->second->affinity;
}
int64_t sceKernelGetSystemTimeWide(void) {return static_cast<int64_t>(fakeSystemUs.load());}
int sceKernelGetSystemInfo(SceKernelSystemInfo* info) {
  if(!info||failSystemInfo.load())return -1;
  info->activeCpuMask=SCE_KERNEL_CPU_MASK_USER_1|SCE_KERNEL_CPU_MASK_USER_2|SCE_KERNEL_CPU_MASK_SYSTEM;
  info->cpuInfo[3].idleClock=fakeIdle3Us.load();
  return 0;
}

namespace {
struct Job {
  std::array<std::atomic<unsigned>,256> visits{};
  std::atomic<unsigned> active{0};
  std::atomic<unsigned> laneMask{0};
  unsigned iteration=0;
};
bool task(void* opaque,size_t begin,size_t end,uint32_t lane) noexcept {
  auto& job=*static_cast<Job*>(opaque);++job.active;
  job.laneMask.fetch_or(1u<<lane);
  const auto iteration=job.iteration;
  // Vary whether the caller or either worker finishes first, and alternate
  // active lane counts across jobs so stale acknowledgements become visible.
  for(unsigned i=0;i<(iteration+lane*7)%31;++i)std::this_thread::yield();
  for(size_t i=begin;i<end;++i)++job.visits[i];
  --job.active;
  return iteration%53!=0||lane!=1;
}
bool fail_lane3_task(void* opaque,size_t begin,size_t end,uint32_t lane) noexcept {
  auto& job=*static_cast<Job*>(opaque);++job.active;job.laneMask.fetch_or(1u<<lane);
  if(lane==3)fakeSystemUs.fetch_add(250);
  else std::this_thread::sleep_for(std::chrono::microseconds(500));
  for(size_t i=begin;i<end;++i)++job.visits[i];
  --job.active;
  return lane!=3;
}
struct BudgetJob {
  std::array<std::atomic<unsigned>,2048> visits{};
  std::atomic<unsigned> laneMask{0};
  uint32_t core3ChunkUs=500;
};
bool budget_task(void* opaque,size_t begin,size_t end,uint32_t lane) noexcept {
  auto& job=*static_cast<BudgetJob*>(opaque);job.laneMask.fetch_or(1u<<lane);
  if(lane==3) {
    fakeSystemUs.fetch_add(job.core3ChunkUs);
  } else {
    std::this_thread::sleep_for(std::chrono::microseconds(150));
  }
  for(size_t i=begin;i<end;++i)++job.visits[i];
  return true;
}
void verify_budget_coverage(const BudgetJob& job,size_t count,const char* reason) {
  for(size_t i=0;i<job.visits.size();++i)
    require(job.visits[i].load()==unsigned(i<count),reason);
}
}
namespace {
void dispatch_plans() {
  using namespace aurora::vita::gfx;
  const std::array<int,3> normal{2,1,3};
  auto p=cpu_dispatch_plan(256,64,3,0,normal,3);
  require(p.workerCount==2&&p.workers[0]==0&&p.workers[1]==1,"CPU0/cap3 plan");
  p=cpu_dispatch_plan(256,64,3,2,normal,3);
  require(p.workerCount==1&&p.workers[0]==1,"CPU2/cap3 plan must not replace CPU2 with CPU3");
  p=cpu_dispatch_plan(256,64,4,2,normal,3);
  require(p.workerCount==2&&p.workers[0]==1&&p.workers[1]==2,"CPU2/cap4 noncontiguous plan");
  require(cpu_dispatch_plan(256,64,0,-1,normal,3).workerCount==0,"unknown caller must be serial");
  require(cpu_dispatch_plan(256,64,0,0,normal,0).workerCount==0,"no workers");
  require(cpu_dispatch_plan(256,64,0,0,{2,2,-1},3).workerCount==1,"duplicate/unknown helper cores");
  require(cpu_dispatch_plan(1,0,0,0,normal,3).workerCount==0,"small job");
  for(int caller=0;caller<4;++caller)for(unsigned cap=0;cap<6;++cap)
    for(size_t count=0;count<260;++count)for(size_t minimum:{size_t(0),size_t(1),size_t(3),size_t(64)}) {
      p=cpu_dispatch_plan(count,minimum,cap,caller,normal,3);
      std::array<unsigned,260> visits{};
      for(unsigned rank=0;rank<p.lanes();++rank) {
        const auto [begin,end]=p.range(count,rank);
        require(begin<=end&&end<=count,"partition bounds");
        require(count==0||begin<end,"empty dispatched interval");
        for(size_t i=begin;i<end;++i)++visits[i];
      }
      for(size_t i=0;i<count;++i)require(visits[i]==1,"pure partition coverage");
      unsigned used=1u<<caller;
      for(unsigned rank=0;rank<p.workerCount;++rank){
        const auto i=p.workers[rank];require(i<3&&(cap==0||i+1<cap),"helper outside cap");
        require(!(used&(1u<<normal[i])),"helper duplicates physical core");used|=1u<<normal[i];
      }
    }
  p=cpu_dispatch_plan(std::numeric_limits<size_t>::max(),1,0,0,normal,3);
  size_t previous=0;
  for(unsigned rank=0;rank<p.lanes();++rank){auto [begin,end]=p.range(std::numeric_limits<size_t>::max(),rank);
    require(begin==previous&&end>begin,"overflow-safe partition");previous=end;}
  require(previous==std::numeric_limits<size_t>::max(),"overflow partition end");
}
void distinct_dispatch() {
  using namespace aurora::vita::gfx;
  require(initialize_cpu_workers(3,64,3,{},true),"distinct initialize");
  currentCpuId=2;
  for(unsigned iteration=1;iteration<=1000;++iteration) {
    Job job;job.iteration=iteration;
    require(cpu_parallel_for(256,task,&job),"excluded lane1 callback incorrectly ran");
    require(job.laneMask==5,"CPU2 cap3 must run caller and lane2 only");
    for(auto& v:job.visits)require(v==1,"noncontiguous coverage");
  }
  Job failing;
  require(cpu_parallel_for_min_lanes(256,64,4,fail_lane3_task,&failing),"CPU3 without quota fallback");
  require(failing.laneMask==5,"cap4 must preserve CPU3 quota gate");
  for(auto& v:failing.visits)require(v==1,"noncontiguous failure coverage");
  Job lane2;
  const auto fail2=[](void* opaque,size_t begin,size_t end,uint32_t lane) noexcept {
    auto& job=*static_cast<Job*>(opaque);job.laneMask.fetch_or(1u<<lane);
    for(size_t i=begin;i<end;++i)++job.visits[i];return lane!=2;
  };
  require(!cpu_parallel_for(256,fail2,&lane2)&&lane2.laneMask==5,"lane2 callback failure");
  for(auto& v:lane2.visits)require(v==1,"lane2 failure coverage");
  Job signal;signal.iteration=7;failNextWake=true;
  require(!cpu_parallel_for(256,task,&signal),"wake failure must be reported");
  require(!failNextWake,"wake failure not injected");
  for(auto& v:signal.visits)require(v==1,"wake failure lost or duplicated work");
  Job unknown;unknown.iteration=7;currentCpuId=-1;
  require(cpu_parallel_for(256,task,&unknown)&&unknown.laneMask==1,"unknown CPU fallback");
  currentCpuId=0;
  struct Nested {Job inner;std::atomic<unsigned> calls{0};} nested;
  nested.inner.iteration=7;
  const auto outer=[](void* opaque,size_t,size_t,uint32_t lane) noexcept {
    auto& n=*static_cast<Nested*>(opaque);
    if(lane==0){++n.calls;return cpu_parallel_for(256,task,&n.inner);}return true;
  };
  require(cpu_parallel_for(256,outer,&nested),"nested dispatch");
  require(nested.calls==1&&nested.inner.laneMask==1,"nested job should be caller-only");
  for(auto& v:nested.inner.visits)require(v==1,"nested coverage");
  struct Held {std::atomic<bool> entered{false},release{false};} held;
  const auto hold=[](void* opaque,size_t,size_t,uint32_t lane) noexcept {
    auto& h=*static_cast<Held*>(opaque);
    if(lane==0){h.entered=true;while(!h.release.load())std::this_thread::yield();}return true;
  };
  std::thread producer([&]{require(cpu_parallel_for(256,hold,&held),"held producer result");});
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
  while(!held.entered&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
  require(held.entered,"concurrent test producer not started");
  Job concurrent;concurrent.iteration=7;
  require(cpu_parallel_for(256,task,&concurrent)&&concurrent.laneMask==1,"concurrent serial fallback");
  held.release=true;producer.join();
  shutdown_cpu_workers();
  require(semas.empty()&&threads.empty(),"distinct shutdown leaked resources");
}
}
int main() {
  using namespace aurora::vita::gfx;
  dispatch_plans();distinct_dispatch();
  for(unsigned workers=1;workers<=3;++workers) {
    require(initialize_cpu_workers(workers,64,workers==3?3:0),"initialize");
    require(cpu_worker_threads()==workers,"worker count mismatch");
    require(cpu_core3_available()==(workers==3),"core3 availability mismatch");
    {
      const auto probe=cpu_worker_probe_snapshot();
      require(probe.lanes[0].priority==64,"CPU2 helper priority mismatch");
      if(workers>=2)require(probe.lanes[1].priority==65,"CPU1 helper priority mismatch");
      if(workers>=3)require(probe.lanes[2].priority==65,"CPU3 helper priority mismatch");
    }
    if(workers==3) {
      require(cpu_core3_cpu_id()==3,"core3 physical id not verified");
      require(cpu_core3_affinity_mask()==SCE_KERNEL_CPU_MASK_SYSTEM,"core3 affinity not verified");
    }
    for(unsigned iteration=1;iteration<=3000;++iteration) {
      Job job;job.iteration=iteration;
      const size_t count=iteration%4==0?256:(iteration%4==1?192:(iteration%4==2?128:32));
      const bool result=cpu_parallel_for(count,task,&job);
      require(result==(iteration%53!=0||count<128),"callback result lost");
      require(job.active.load()==0,"returned before all callbacks completed");
      for(size_t i=0;i<job.visits.size();++i)
        require(job.visits[i].load()==unsigned(i<count),"range duplicated or missing");
    }
    {
      Job job;job.iteration=7;
      const size_t count=workers==3?12:10;
      require(cpu_parallel_for_min(count,3,task,&job),"per-call minimum result");
      require(job.active.load()==0,"per-call minimum returned early");
      for(size_t i=0;i<job.visits.size();++i)
        require(job.visits[i].load()==unsigned(i<count),"per-call minimum coverage");
      if(workers==2)require((job.laneMask.load()&0x7u)==0x7u,"game override did not use three lanes");
      if(workers==3)require((job.laneMask.load()&(1u<<3))==0,"CPU3 ran without an active quota");
    }
    if(workers==3) {
      Job job;job.iteration=11;
      require(cpu_parallel_for(256,task,&job),"default lane cap result");
      require((job.laneMask.load()&0xfu)==0x7u,"renderer default lane cap used probe-only CPU3 helper");
    }
    shutdown_cpu_workers();
    require(semas.empty()&&threads.empty(),"shutdown leaked resources");
  }
  failSystemThreadCreate.store(true);
  require(initialize_cpu_workers(3,64,3),"cpu3 fallback initialize");
  require(cpu_worker_threads()==2,"cpu3 create failure did not fall back to two helpers");
  require(!cpu_core3_available(),"failed cpu3 probe reported available");
  {
    const auto probe=cpu_worker_probe_snapshot();
    require(probe.lanes[2].requestedAffinity==SCE_KERNEL_CPU_MASK_SYSTEM,"cpu3 probe affinity missing");
    require(probe.lanes[2].createResult==-0x1234,"cpu3 create error was not preserved");
    require(!probe.lanes[2].created,"failed cpu3 probe reported created lane");
  }
  {
    Job job;job.iteration=17;
    require(cpu_parallel_for(192,task,&job),"fallback parallel result");
    for(size_t i=0;i<job.visits.size();++i)
      require(job.visits[i].load()==unsigned(i<192),"fallback range coverage");
  }
  shutdown_cpu_workers();
  failSystemThreadCreate.store(false);
  require(semas.empty()&&threads.empty(),"fallback shutdown leaked resources");
  failUser1ThreadCreate.store(true);
  require(initialize_cpu_workers(3,64,4),"middle helper fallback initialize");
  require(cpu_worker_threads()==2,"CPU1 helper failure prevented CPU3 probe");
  require(cpu_core3_available(),"CPU3 was not attempted after CPU1 helper failure");
  {
    const auto probe=cpu_worker_probe_snapshot();
    require(probe.lanes[1].createResult==-0x2345,"CPU1 create error was not preserved");
    require(probe.lanes[2].created&&probe.lanes[2].actualCpu==3,"CPU3 probe result missing after sparse fallback");
  }
  {
    Job job;job.iteration=19;
    require(cpu_parallel_for_min_lanes(192,32,4,task,&job),"sparse worker topology result");
    require((job.laneMask.load()&(1u<<3))==0,"sparse probe executed CPU3 without quota");
    for(size_t i=0;i<job.visits.size();++i)
      require(job.visits[i].load()==unsigned(i<192),"sparse topology range coverage");
  }
  shutdown_cpu_workers();
  require(semas.empty()&&threads.empty(),"sparse topology shutdown leaked resources");

  fakeSystemUs.store(100000);fakeIdle3Us.store(100000);
  CpuCore3BudgetConfig budget{};
  budget.enabled=true;budget.maxTotalPercent=70;budget.guardPercent=5;
  budget.shortWindowMs=10;budget.longWindowMs=100;budget.chunkTargetUs=250;budget.samplePeriodUs=1000;
  require(initialize_cpu_workers(3,16,3,budget),"sparse budget initialize");
  require(cpu_worker_threads()==2&&cpu_core3_available(),"sparse budget lost CPU3");
  fakeSystemUs.fetch_add(10000);fakeIdle3Us.fetch_add(10000);
  BudgetJob sparseGeneralJob;
  require(cpu_parallel_for(256,budget_task,&sparseGeneralJob),"sparse general job");
  verify_budget_coverage(sparseGeneralJob,256,"sparse general coverage");
  require((sparseGeneralJob.laneMask.load()&(1u<<3))==0,"general renderer path used CPU3");
  fakeSystemUs.fetch_add(10000);fakeIdle3Us.fetch_add(10000);
  BudgetJob sparseBudgetJob;
  require(cpu_parallel_for_vertex(512,budget_task,&sparseBudgetJob),"sparse budget dynamic job");
  verify_budget_coverage(sparseBudgetJob,512,"sparse budget coverage");
  require((sparseBudgetJob.laneMask.load()&(1u<<3))!=0,"sparse budget never used CPU3");
  shutdown_cpu_workers();
  failUser1ThreadCreate.store(false);
  require(semas.empty()&&threads.empty(),"sparse budget shutdown leaked resources");

  // Distinct filtering must also apply to the existing quota-aware scheduler.
  fakeSystemUs.store(100000);fakeIdle3Us.store(100000);
  require(initialize_cpu_workers(3,16,3,budget,true),"distinct budget initialize");
  currentCpuId=2;
  fakeSystemUs.fetch_add(10000);fakeIdle3Us.fetch_add(10000);
  BudgetJob distinctBudget;
  require(cpu_parallel_for_vertex(1024,budget_task,&distinctBudget),"distinct quota job");
  verify_budget_coverage(distinctBudget,1024,"distinct quota coverage");
  require(!(distinctBudget.laneMask.load()&2u),"CPU2 caller woke CPU2 helper in quota mode");
  require(distinctBudget.laneMask.load()&8u,"eligible CPU3 was lost in quota mode");
  const auto chunks=cpu_core3_budget_snapshot().chunks;
  fakeSystemUs.fetch_add(20000);
  BudgetJob distinctBlocked;
  require(cpu_parallel_for_vertex(512,budget_task,&distinctBlocked),"distinct exhausted quota");
  verify_budget_coverage(distinctBlocked,512,"distinct exhausted quota coverage");
  require(cpu_core3_budget_snapshot().chunks==chunks,"distinct bypassed CPU3 quota");
  currentCpuId=0;shutdown_cpu_workers();

  fakeSystemUs.store(100000);fakeIdle3Us.store(100000);
  require(initialize_cpu_workers(3,16,3,budget),"budget initialize");
  require(cpu_core3_available(),"budget test needs CPU3");
  fakeSystemUs.fetch_add(10000);fakeIdle3Us.fetch_add(10000); // bootstrap with an idle core
  BudgetJob budgetJob;
  require(cpu_parallel_for_vertex(1024,budget_task,&budgetJob),"budgeted dynamic job");
  verify_budget_coverage(budgetJob,1024,"budgeted dynamic coverage");
  auto budgetSnap=cpu_core3_budget_snapshot();
  require(budgetSnap.configured&&budgetSnap.telemetryValid,"budget telemetry not valid");
  require(budgetSnap.targetPercent==65,"guard was not applied to total CPU3 target");
  require(budgetSnap.chunks>0&&(budgetJob.laneMask.load()&(1u<<3)),"CPU3 received no dynamic chunks");
  auto vertexSnap=cpu_vertex_parallel_snapshot();
  require(vertexSnap.calls>0&&vertexSnap.dynamicCalls>0,"vertex parallel telemetry missed dynamic call");
  require(vertexSnap.laneItems[3]>0&&vertexSnap.laneChunks[3]>0,"vertex parallel telemetry missed CPU3 work");

  // Diagnostic OFF must suppress optional profiling while preserving quota
  // admission, exact range coverage, failures and renewal. Keep it OFF through
  // the exhaustion/telemetry-failure tests below.
  aurora::vita::set_runtime_diagnostics_enabled(false);
  fakeSystemUs.fetch_add(10000);fakeIdle3Us.fetch_add(10000);
  BudgetJob quietJob;
  require(cpu_parallel_for_vertex(512,budget_task,&quietJob),"quiet budget job");
  verify_budget_coverage(quietJob,512,"quiet budget range coverage");
  auto quietVertex=cpu_vertex_parallel_snapshot();
  require(quietVertex.calls==vertexSnap.calls&&quietVertex.dynamicCalls==vertexSnap.dynamicCalls,
          "quiet job still collected vertex calls");
  require(quietVertex.totalWallUs==vertexSnap.totalWallUs&&quietVertex.callerWaitUs==vertexSnap.callerWaitUs,
          "quiet job still collected vertex timing");
  for(unsigned lane=0;lane<4;++lane)
    require(quietVertex.laneItems[lane]==vertexSnap.laneItems[lane]&&
            quietVertex.laneChunks[lane]==vertexSnap.laneChunks[lane]&&
            quietVertex.laneWorkUs[lane]==vertexSnap.laneWorkUs[lane],"quiet job still collected lane profiling");
  require(cpu_core3_budget_snapshot().configured,"diagnostics OFF disabled CPU3 quota");

  Job failing;
  fakeSystemUs.fetch_add(10000);fakeIdle3Us.fetch_add(10000);
  require(!cpu_parallel_for_vertex(256,fail_lane3_task,&failing),"budgeted lane3 callback failure lost");
  require((failing.laneMask.load()&(1u<<3))!=0,"budgeted failure test never reached CPU3");
  for(size_t i=0;i<failing.visits.size();++i)
    require(failing.visits[i].load()==unsigned(i<256),"budgeted lane3 failure lost range coverage");

  budgetSnap=cpu_core3_budget_snapshot();
  const uint64_t chunksBeforeExternal=budgetSnap.chunks;
  fakeSystemUs.fetch_add(20000); // 20 ms at 100% external CPU3 occupancy
  BudgetJob blockedJob;
  require(cpu_parallel_for_vertex(256,budget_task,&blockedJob),"externally blocked job");
  verify_budget_coverage(blockedJob,256,"externally blocked coverage");
  budgetSnap=cpu_core3_budget_snapshot();
  require(budgetSnap.chunks==chunksBeforeExternal,"CPU3 dispatched despite exhausted total-core budget");
  require(budgetSnap.denied>0,"budget exhaustion was not recorded");
  require(!budgetSnap.dispatchAllowed,"CPU3 dispatch remained enabled above observed target");

  fakeSystemUs.fetch_add(20000);fakeIdle3Us.fetch_add(20000); // idle time replenishes both buckets
  BudgetJob renewedJob;
  require(cpu_parallel_for_vertex(512,budget_task,&renewedJob),"renewed budget job");
  verify_budget_coverage(renewedJob,512,"renewed budget coverage");
  budgetSnap=cpu_core3_budget_snapshot();
  require(budgetSnap.chunks>chunksBeforeExternal,"CPU3 budget did not renew after idle time");

  const uint64_t chunksBeforeFailure=budgetSnap.chunks;
  failSystemInfo.store(true);
  BudgetJob telemetryFailureJob;
  require(cpu_parallel_for_vertex(512,budget_task,&telemetryFailureJob),"telemetry failure fallback job");
  verify_budget_coverage(telemetryFailureJob,512,"telemetry failure fallback coverage");
  budgetSnap=cpu_core3_budget_snapshot();
  require(!budgetSnap.telemetryValid,"failed telemetry remained valid");
  require(budgetSnap.chunks==chunksBeforeFailure,"CPU3 ran after telemetry failure");
  require(budgetSnap.telemetryFailures>0,"telemetry failure was not counted");
  failSystemInfo.store(false);
  shutdown_cpu_workers();
  aurora::vita::set_runtime_diagnostics_enabled(true);
  require(semas.empty()&&threads.empty(),"budget shutdown leaked resources");
  rejectSystemCpuId=true;
  require(initialize_cpu_workers(3,64,3,{},true),"rejected physical CPU3 initialize");
  require(cpu_worker_threads()==2&&!cpu_core3_available(),"CPU3 physical id rejection failed");
  shutdown_cpu_workers();rejectSystemCpuId=false;
  require(semas.empty()&&threads.empty(),"rejected CPU3 shutdown leaked resources");
  require(semaErrors==0,"semaphore token overflow");
  std::puts("vita workers: sparse CPU3 topology, quota scheduler, exact coverage/results, clean shutdown");
}
