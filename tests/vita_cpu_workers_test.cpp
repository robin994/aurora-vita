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
struct Thread {int(*entry)(SceSize,void*);std::thread thread;};
std::mutex registryMutex;
std::map<int,std::shared_ptr<Sema>> semas;
std::map<int,std::unique_ptr<Thread>> threads;
int nextId=1;
std::atomic<unsigned> semaErrors{0};
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
SceUID sceKernelCreateThread(const char*,int(*entry)(SceSize,void*),int,SceSize,int,int,void*) {
  const int id=nextId++;auto t=std::make_unique<Thread>();t->entry=entry;threads.emplace(id,std::move(t));return id;
}
int sceKernelStartThread(SceUID id,SceSize size,void* args) {
  auto& t=*threads.at(id);std::vector<unsigned char> copy(size);std::memcpy(copy.data(),args,size);
  t.thread=std::thread([entry=t.entry,copy=std::move(copy)]() mutable {entry(copy.size(),copy.data());});return 0;
}
int sceKernelWaitThreadEnd(SceUID id,void*,void*) {threads.at(id)->thread.join();return 0;}
int sceKernelDeleteThread(SceUID id) {threads.erase(id);return 0;}
int sceKernelDelayThread(unsigned us) {std::this_thread::sleep_for(std::chrono::microseconds(us));return 0;}

namespace {
struct Job {
  std::array<std::atomic<unsigned>,192> visits{};
  std::atomic<unsigned> active{0};
  unsigned iteration=0;
};
bool task(void* opaque,size_t begin,size_t end,uint32_t lane) noexcept {
  auto& job=*static_cast<Job*>(opaque);++job.active;
  const auto iteration=job.iteration;
  // Vary whether the caller or either worker finishes first, and alternate
  // active lane counts across jobs so stale acknowledgements become visible.
  for(unsigned i=0;i<(iteration+lane*7)%31;++i)std::this_thread::yield();
  for(size_t i=begin;i<end;++i)++job.visits[i];
  --job.active;
  return iteration%53!=0||lane!=1;
}
}
int main() {
  using namespace aurora::vita::gfx;
  for(unsigned workers=1;workers<=2;++workers) {
    require(initialize_cpu_workers(workers,64),"initialize");
    for(unsigned iteration=1;iteration<=4000;++iteration) {
      Job job;job.iteration=iteration;
      const size_t count=iteration%3==0?192:(iteration%3==1?128:32);
      const bool result=cpu_parallel_for(count,task,&job);
      require(result==(iteration%53!=0||count<128),"callback result lost");
      require(job.active.load()==0,"returned before all callbacks completed");
      for(size_t i=0;i<job.visits.size();++i)
        require(job.visits[i].load()==unsigned(i<count),"range duplicated or missing");
    }
    shutdown_cpu_workers();
    require(semas.empty()&&threads.empty(),"shutdown leaked resources");
  }
  require(semaErrors==0,"semaphore token overflow");
  std::puts("vita workers: 8000 mixed-size jobs, exact coverage/results, clean shutdown");
}
