#pragma once
#include <cstddef>
#include <cstdint>

namespace aurora::vita::gfx {

// Tracks writes to guest-visible CPU memory at coarse page granularity. GX
// clients already call DCFlush/Store after modifying display lists and indexed
// arrays, so static geometry can use the resulting revision as a cheap validity
// proof instead of hashing and comparing every byte every frame.
void note_memory_write(const void* address,size_t bytes) noexcept;
uint64_t memory_write_epoch() noexcept;
uint64_t memory_range_revision(const void* address,size_t bytes) noexcept;

struct MemoryRevisionCheck {
  const void* address=nullptr;
  size_t bytes=0;
  uint64_t revision=0;
};

// Checks several source ranges under one page-table lock. Static geometry can
// reference multiple indexed arrays; validating them one-by-one otherwise pays
// the mutex/hash-map acquisition cost for every attribute after any guest write.
bool memory_ranges_match(const MemoryRevisionCheck* ranges,size_t count) noexcept;

} // namespace aurora::vita::gfx
