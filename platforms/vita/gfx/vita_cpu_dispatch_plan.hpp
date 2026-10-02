#pragma once
#include "vita_cpu_workers.hpp"
#include <algorithm>
#include <array>
#include <utility>

namespace aurora::vita::gfx {
// Worker identity is independent of partition rank. The cap selects a prefix
// before filtering, so excluding CPU2 never silently enables probe-only CPU3.
struct CpuDispatchPlan {
  uint32_t workerCount=0;
  std::array<uint32_t,MaxWorkerThreads> workers{};
  uint32_t lanes() const noexcept { return workerCount+1; }
  std::pair<size_t,size_t> range(size_t count,uint32_t rank) const noexcept {
    // Quotient/remainder partition avoids overflow and empty ceil-chunks.
    const size_t q=count/lanes(),r=count%lanes();
    const size_t begin=q*rank+std::min<size_t>(rank,r);
    return {begin,begin+q+(rank<r)};
  }
};
inline CpuDispatchPlan cpu_dispatch_plan(size_t count,size_t minItems,uint32_t cap,
    int callerCpu,const std::array<int,MaxWorkerThreads>& cpus,uint32_t workers) noexcept {
  CpuDispatchPlan plan{};
  if(callerCpu<0 || callerCpu>3 || !count)return plan;
  workers=std::min(workers,MaxWorkerThreads);
  const uint32_t allowed=cap==0?workers:std::min(workers,cap>0?cap-1:0);
  const size_t useful=std::max<size_t>(1,count/std::max<size_t>(1,minItems));
  unsigned used=1u<<callerCpu;
  for(uint32_t i=0;i<allowed && plan.lanes()<useful;++i) {
    const int cpu=cpus[i];
    if(cpu<0 || cpu>3 || (used&(1u<<cpu)))continue;
    used|=1u<<cpu;
    plan.workers[plan.workerCount++]=i;
  }
  return plan;
}
} // namespace aurora::vita::gfx
