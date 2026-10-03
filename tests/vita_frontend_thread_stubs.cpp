// Host-only Vita FIFO scheduling shim. Never linked into a Vita build.
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
thread_local SceUID currentThreadId=0;
std::shared_ptr<Sema> semaphore(int id) {
  std::lock_guard lock(registryMutex);return semas.at(id);
}
void require(bool ok,const char* reason) {
  if(!ok){std::fprintf(stderr,"frontend thread shim: %s\n",reason);std::abort();}
}
}
SceUID sceKernelCreateSema(const char*,int,int count,int max,void*) {
  std::lock_guard lock(registryMutex);auto s=std::make_shared<Sema>();s->count=count;s->max=max;
  const int id=nextId++;semas.emplace(id,std::move(s));return id;
}
int sceKernelDeleteSema(SceUID id) {std::lock_guard lock(registryMutex);semas.erase(id);return 0;}
int sceKernelWaitSema(SceUID id,int count,void* timeout) {
  auto s=semaphore(id);std::unique_lock lock(s->mutex);
  if(timeout!=nullptr) {
    const auto us=*static_cast<unsigned int*>(timeout);
    if(!s->cv.wait_for(lock,std::chrono::microseconds(us),[&]{return s->count>=count;}))return -1;
  } else {
    require(s->cv.wait_for(lock,std::chrono::seconds(3),[&]{return s->count>=count;}),"lost wake/completion (3 s timeout)");
  }
  s->count-=count;return 0;
}
int sceKernelSignalSema(SceUID id,int count) {
  auto s=semaphore(id);std::lock_guard lock(s->mutex);
  if(s->count+count>s->max){return -1;}
  s->count+=count;s->cv.notify_one();return 0;
}
SceUID sceKernelCreateThread(const char*,int(*entry)(SceSize,void*),int,SceSize,int,int,void*) {
  const int id=nextId++;auto t=std::make_unique<Thread>();t->entry=entry;threads.emplace(id,std::move(t));return id;
}
int sceKernelStartThread(SceUID id,SceSize size,void* args) {
  auto& t=*threads.at(id);std::vector<unsigned char> copy(size);if(size)std::memcpy(copy.data(),args,size);
  t.thread=std::thread([entry=t.entry,id,copy=std::move(copy)]() mutable {currentThreadId=id;entry(copy.size(),copy.data());});return 0;
}
int sceKernelWaitThreadEnd(SceUID id,void*,void*) {threads.at(id)->thread.join();return 0;}
int sceKernelDeleteThread(SceUID id) {threads.erase(id);return 0;}
int sceKernelDelayThread(unsigned us) {std::this_thread::sleep_for(std::chrono::microseconds(us));return 0;}

SceUID sceKernelGetThreadId() {return currentThreadId;}
