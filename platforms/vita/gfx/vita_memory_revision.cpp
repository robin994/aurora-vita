#include "vita_memory_revision.hpp"
#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace aurora::vita::gfx {
namespace {
constexpr uintptr_t PageBytes=64u*1024u;
std::mutex g_mutex;
std::unordered_map<uintptr_t,uint64_t> g_pages;
uint64_t g_epoch=1;
}

void note_memory_write(const void* address,size_t bytes) noexcept {
  if(!address||!bytes)return;
  const uintptr_t start=reinterpret_cast<uintptr_t>(address);
  const uintptr_t last=bytes-1>UINTPTR_MAX-start?UINTPTR_MAX:start+bytes-1;
  const uintptr_t firstPage=start&~(PageBytes-1u);
  const uintptr_t lastPage=last&~(PageBytes-1u);
  std::lock_guard<std::mutex> lock(g_mutex);
  const uint64_t revision=++g_epoch;
  for(uintptr_t page=firstPage;;page+=PageBytes) {
    g_pages[page]=revision;
    if(page==lastPage||page>UINTPTR_MAX-PageBytes)break;
  }
}

uint64_t memory_range_revision(const void* address,size_t bytes) noexcept {
  if(!address||!bytes)return 0;
  const uintptr_t start=reinterpret_cast<uintptr_t>(address);
  const uintptr_t last=bytes-1>UINTPTR_MAX-start?UINTPTR_MAX:start+bytes-1;
  const uintptr_t firstPage=start&~(PageBytes-1u);
  const uintptr_t lastPage=last&~(PageBytes-1u);
  uint64_t newest=0;
  std::lock_guard<std::mutex> lock(g_mutex);
  for(uintptr_t page=firstPage;;page+=PageBytes) {
    const auto it=g_pages.find(page);
    if(it!=g_pages.end())newest=std::max(newest,it->second);
    if(page==lastPage||page>UINTPTR_MAX-PageBytes)break;
  }
  return newest;
}

} // namespace aurora::vita::gfx
