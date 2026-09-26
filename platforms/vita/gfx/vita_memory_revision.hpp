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

} // namespace aurora::vita::gfx
