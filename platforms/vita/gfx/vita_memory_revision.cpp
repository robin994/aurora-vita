#include "vita_memory_revision.hpp"
#include "vita_hash_map.hpp"
#include <algorithm>
#include <array>
#include <mutex>

namespace aurora::vita::gfx {
namespace {
constexpr uintptr_t PageBytes=64u*1024u;
constexpr size_t MaxWriteRecords=4096;
struct WriteRecord {
  uint64_t revision=0;
  uintptr_t first=0;
  uintptr_t last=0;
};
std::mutex g_mutex;
FlatHashMap<uintptr_t,uint64_t> g_pages;
std::array<WriteRecord,MaxWriteRecords> g_writes{};
size_t g_writeHead=0;
size_t g_writeCount=0;
uint64_t g_epoch=1;

uint64_t range_revision_unlocked(uintptr_t start,uintptr_t last) noexcept {
  const uintptr_t firstPage=start&~(PageBytes-1u);
  const uintptr_t lastPage=last&~(PageBytes-1u);
  uint64_t newest=0;
  for(uintptr_t page=firstPage;;page+=PageBytes) {
    const auto it=g_pages.find(page);
    if(it!=g_pages.end())newest=std::max(newest,it->second);
    if(page==lastPage||page>UINTPTR_MAX-PageBytes)break;
  }
  return newest;
}

bool overlaps(uintptr_t firstA,uintptr_t lastA,uintptr_t firstB,uintptr_t lastB) noexcept {
  return firstA<=lastB&&firstB<=lastA;
}
}

void note_memory_write(const void* address,size_t bytes) noexcept {
  if(!address||!bytes)return;
  const uintptr_t start=reinterpret_cast<uintptr_t>(address);
  const uintptr_t last=bytes-1>UINTPTR_MAX-start?UINTPTR_MAX:start+bytes-1;
  const uintptr_t firstPage=start&~(PageBytes-1u);
  const uintptr_t lastPage=last&~(PageBytes-1u);
  std::lock_guard<std::mutex> lock(g_mutex);
  const uint64_t revision=++g_epoch;
  g_writes[g_writeHead]={revision,start,last};
  g_writeHead=(g_writeHead+1u)%MaxWriteRecords;
  if(g_writeCount<MaxWriteRecords)++g_writeCount;
  for(uintptr_t page=firstPage;;page+=PageBytes) {
    g_pages[page]=revision;
    if(page==lastPage||page>UINTPTR_MAX-PageBytes)break;
  }
}

uint64_t memory_range_revision(const void* address,size_t bytes) noexcept {
  if(!address||!bytes)return 0;
  const uintptr_t start=reinterpret_cast<uintptr_t>(address);
  const uintptr_t last=bytes-1>UINTPTR_MAX-start?UINTPTR_MAX:start+bytes-1;
  std::lock_guard<std::mutex> lock(g_mutex);
  return range_revision_unlocked(start,last);
}

MemoryRangeStamp memory_range_stamp(const void* address,size_t bytes) noexcept {
  if(!address||!bytes)return {};
  const uintptr_t start=reinterpret_cast<uintptr_t>(address);
  const uintptr_t last=bytes-1>UINTPTR_MAX-start?UINTPTR_MAX:start+bytes-1;
  std::lock_guard<std::mutex> lock(g_mutex);
  return {range_revision_unlocked(start,last),g_epoch};
}

bool memory_range_changed(const void* address,size_t bytes,MemoryRangeStamp& stamp) noexcept {
  if(!address||!bytes)return false;
  const uintptr_t start=reinterpret_cast<uintptr_t>(address);
  const uintptr_t last=bytes-1>UINTPTR_MAX-start?UINTPTR_MAX:start+bytes-1;
  std::lock_guard<std::mutex> lock(g_mutex);
  const uint64_t pageRevision=range_revision_unlocked(start,last);
  const uint64_t currentEpoch=g_epoch;
  if(pageRevision==stamp.pageRevision) {
    stamp.writeEpoch=currentEpoch;
    return false;
  }
  if(g_writeCount) {
    const size_t oldestIndex=(g_writeHead+MaxWriteRecords-g_writeCount)%MaxWriteRecords;
    const uint64_t oldestRevision=g_writes[oldestIndex].revision;
    if(oldestRevision>1 && stamp.writeEpoch<oldestRevision-1u)
      return true;
    for(size_t i=0;i<g_writeCount;++i) {
      const auto& write=g_writes[(oldestIndex+i)%MaxWriteRecords];
      if(write.revision<=stamp.writeEpoch)continue;
      if(overlaps(start,last,write.first,write.last))return true;
    }
  }
  stamp.pageRevision=pageRevision;
  stamp.writeEpoch=currentEpoch;
  return false;
}

} // namespace aurora::vita::gfx
