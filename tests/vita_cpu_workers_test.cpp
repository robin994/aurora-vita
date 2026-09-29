#include "gfx/vita_cpu_workers.hpp"
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
struct Sema {std::mutex mutex;std::condition_variable cv;int count=0,max=1;};
struct Thread {int(*entry)(SceSize,void*);std::thread thread;int affinity=0;int id=-1;};
std::mutex registryMutex;
std::map<int,std::shared_ptr<Sema>> semas;
std::map<int,std::unique_ptr<Thread>> threads;
int nextId=1;
std::atomic<unsigned> semaErrors{0};
std::atomic<bool> failSystemThreadCreate{false};
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
SceUID sceKernelCreateSema(const char*,int,int count,int max,void*) {
  std::lock_guard lock(registryMutex);auto s=std::make_shared<Sema>();s->count=count;s->max=max;
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
  if(s->count+count>s->max){++semaErrors;return -1;}
  s->count+=count;s->cv.notify_one();return 0;
}
SceUID sceKernelCreateThread(const char*,int(*entry)(SceSize,void*),int,SceSize,int,int affinity,void*) {
  if(affinity==SCE_KERNEL_CPU_MASK_SYSTEM&&failSystemThreadCreate.load())return -0x1234;
  const int id=nextId++;auto t=std::make_unique<Thread>();t->entry=entry;t->affinity=affinity;t->id=id;
  threads.emplace(id,std::move(t));return id;
}
int sceKernelStartThread(SceUID id,SceSize size,void* args) {
  auto& t=*threads.at(id);std::vector<unsigned char> copy(size);std::memcpy(copy.data(),args,size);
  const int affinity=t.affinity;
  t.thread=std::thread([entry=t.entry,copy=std::move(copy),id,affinity]() mutable {
    currentThreadId=id;currentAffinity=affinity;
    currentCpuId=affinity==SCE_KERNEL_CPU_MASK_SYSTEM?3:
        (affinity==SCE_KERNEL_CPU_MASK_USER_2?2:(affinity==SCE_KERNEL_CPU_MASK_USER_1?1:0));
    entry(copy.size(),copy.data());
  });return 0;
}
int sceKernelWaitThreadEnd(SceUID id,void*,void*) {threads.at(id)->thread.join();return 0;}
int sceKernelDeleteThread(SceUID id) {threads.erase(id);return 0;}
int sceKernelDelayThread(unsigned us) {std::this_thread::sleep_for(std::chrono::microseconds(us));return 0;}
int sceKernelGetCpuId(void) {return currentCpuId;}
SceUID sceKernelGetThreadId(void) {return currentThreadId;}
int sceKernelGetThreadCpuAffinityMask(SceUID id) {
  if(id==currentThreadId)return currentAffinity;
  std::lock_guard lock(registryMutex);
  const auto it=threads.find(id);return it==threads.end()?-1:it->second->affinity;
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
  for(size_t i=begin;i<end;++i)++job.visits[i];
  --job.active;
  return lane!=3;
}
}
int main() {
  using namespace aurora::vita::gfx;
  for(unsigned workers=1;workers<=3;++workers) {
    require(initialize_cpu_workers(workers,64,workers==3?3:0),"initialize");
    require(cpu_worker_threads()==workers,"worker count mismatch");
    require(cpu_core3_available()==(workers==3),"core3 availability mismatch");
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
      if(workers==3)require((job.laneMask.load()&0xfu)==0xfu,"four-lane override did not use CPU3 lane");
    }
    if(workers==3) {
      Job job;job.iteration=11;
      require(cpu_parallel_for(256,task,&job),"default lane cap result");
      require((job.laneMask.load()&0xfu)==0x7u,"renderer default lane cap used probe-only CPU3 helper");
      Job failing;
      require(!cpu_parallel_for_min(12,3,fail_lane3_task,&failing),"lane3 callback failure lost");
      for(size_t i=0;i<failing.visits.size();++i)
        require(failing.visits[i].load()==unsigned(i<12),"lane3 failure lost range coverage");
    }
    shutdown_cpu_workers();
    require(semas.empty()&&threads.empty(),"shutdown leaked resources");
  }
  failSystemThreadCreate.store(true);
  require(initialize_cpu_workers(3,64,3),"cpu3 fallback initialize");
  require(cpu_worker_threads()==2,"cpu3 create failure did not fall back to two helpers");
  require(!cpu_core3_available(),"failed cpu3 probe reported available");
  {
    Job job;job.iteration=17;
    require(cpu_parallel_for(192,task,&job),"fallback parallel result");
    for(size_t i=0;i<job.visits.size();++i)
      require(job.visits[i].load()==unsigned(i<192),"fallback range coverage");
  }
  shutdown_cpu_workers();
  failSystemThreadCreate.store(false);
  require(semas.empty()&&threads.empty(),"fallback shutdown leaked resources");
  require(semaErrors==0,"semaphore token overflow");
  std::puts("vita workers: 3 helpers/4 lanes, CPU3 probe fallback, exact coverage/results, clean shutdown");
}
