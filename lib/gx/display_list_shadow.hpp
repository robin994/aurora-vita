#pragma once

#include "../../platforms/vita/gfx/vita_byte_compare.hpp"
#include "../../platforms/vita/gfx/vita_hash_map.hpp"
#include "../../platforms/vita/gfx/vita_memory_revision.hpp"

#include <cstdio>
#include <vector>

namespace aurora::gx::fifo {

// Diagnostic safety guard for the revision-only CDRAM shadow. Exact validation
// reads the guest bytes on every hit, so this is not a performance fast path.
// The caller owns the source until get() returns and copies the returned bytes
// immediately, before any other cache operation. GX recording is excluded by
// the caller; this cache never writes the original GX display list.
class DisplayListShadowCache {
public:
  explicit DisplayListShadowCache(size_t budget = 8u * 1024u * 1024u) : budget_(budget) {}

  const uint8_t* get(const void* data, uint32_t length) {
    if (!data || !length || length > budget_) return nullptr;
    const uintptr_t address = reinterpret_cast<uintptr_t>(data);
    uint64_t revision = aurora::vita::gfx::memory_range_revision(data, length);
    auto it = shadows_.find(address);
    if (it != shadows_.end()) {
      auto& shadow = it->second;
      if (shadow.bytes.size() == length && shadow.revision == revision) {
        if (aurora::vita::gfx::byte_spans_equal(data, shadow.bytes.data(), length))
          return shadow.bytes.data();

        // A revision-only hit would have returned stale bytes. Publish the
        // actual write before submitting fresh bytes under the same guest
        // identity, invalidating downstream static-geometry entries as well.
        const auto* live = static_cast<const uint8_t*>(data);
        size_t offset = 0;
        while (offset < length && live[offset] == shadow.bytes[offset]) ++offset;
        ++untrackedWrites_;
        if (untrackedWrites_ <= 8 && offset < length) {
          std::fprintf(stderr,
              "[aurora-vita][dl-shadow] untracked-write src=%p bytes=%u revision=%llu "
              "offset=%zu cached=0x%02x live=0x%02x\n",
              data, static_cast<unsigned>(length), static_cast<unsigned long long>(revision),
              offset, static_cast<unsigned>(shadow.bytes[offset]), static_cast<unsigned>(live[offset]));
        }
        aurora::vita::gfx::note_memory_write(data, length);
        revision = aurora::vita::gfx::memory_range_revision(data, length);
      }
      bytes_ -= shadow.bytes.size();
      shadows_.erase(it);
    }
    if (length > budget_ - bytes_) {
      shadows_.clear();
      bytes_ = 0;
    }
    auto& shadow = shadows_[address];
    shadow.bytes.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + length);
    shadow.revision = revision;
    bytes_ += length;
    return shadow.bytes.data();
  }

  void clear() {
    shadows_.clear();
    bytes_ = 0;
    untrackedWrites_ = 0;
  }

  size_t bytes() const noexcept { return bytes_; }
  uint64_t untracked_writes() const noexcept { return untrackedWrites_; }

private:
  struct Shadow {
    std::vector<uint8_t> bytes;
    uint64_t revision = 0;
  };
  aurora::vita::gfx::FlatHashMap<uintptr_t, Shadow> shadows_;
  size_t budget_;
  size_t bytes_ = 0;
  uint64_t untrackedWrites_ = 0;
};

} // namespace aurora::gx::fifo
