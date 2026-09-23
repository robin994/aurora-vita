#pragma once
#include <cstddef>
#include <cstdint>

namespace aurora::vita::gfx {

struct MemoryRangeStamp {
  uint64_t pageRevision=0;
  uint64_t writeEpoch=0;
};

// Tracks writes to guest-visible CPU memory. The page revision is the cheap
// common-case filter; when that changes, the bounded write history resolves
// whether the actual source interval was touched or only a neighbour in the
// same 64 KiB page.
void note_memory_write(const void* address,size_t bytes) noexcept;
uint64_t memory_range_revision(const void* address,size_t bytes) noexcept;
MemoryRangeStamp memory_range_stamp(const void* address,size_t bytes) noexcept;
bool memory_range_changed(const void* address,size_t bytes,MemoryRangeStamp& stamp) noexcept;

} // namespace aurora::vita::gfx
