#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace aurora::gx::fifo {

struct NativeModelSourceSpan {
  uintptr_t displayList = 0;
  const uint8_t* data = nullptr;
  uint32_t bytes = 0;
  uint32_t ownerLevel = 0;
  uint64_t generation = 0;
};

// Producer-owned asset lifetimes. Generations certify ownership, not byte
// immutability: the consumer still validates source writes and GPU residency.
// A full table declines registration without evicting a live asset's lifetime.
template<size_t Capacity = 1024>
class NativeModelSources {
  static_assert(Capacity > 0);
  enum class Status : uint8_t { Empty, Live, Tombstone };
  struct Slot { NativeModelSourceSpan source{}; Status status = Status::Empty; };
  std::array<Slot, Capacity> slots_{};
  uint64_t nextGeneration_ = 0;
  size_t size_ = 0;

  static size_t bucket(uintptr_t key) noexcept {
    key ^= key >> 7;
    key ^= key >> 17;
    return size_t(key) % Capacity;
  }

 public:
  const NativeModelSourceSpan* find(uintptr_t displayList) const noexcept {
    if (!displayList) return nullptr;
    const size_t start = bucket(displayList);
    for (size_t n = 0; n < Capacity; ++n) {
      const auto& slot = slots_[(start + n) % Capacity];
      if (slot.status == Status::Empty) return nullptr;
      if (slot.status == Status::Live && slot.source.displayList == displayList)
        return &slot.source;
    }
    return nullptr;
  }

  uint64_t register_source(uintptr_t displayList, const void* data,
                           uint32_t bytes, uint32_t ownerLevel) noexcept {
    if (!displayList || !data || bytes < 3 || bytes > 256u * 1024u ||
        nextGeneration_ == std::numeric_limits<uint64_t>::max()) return 0;
    Slot* destination = nullptr;
    const size_t start = bucket(displayList);
    for (size_t n = 0; n < Capacity; ++n) {
      auto& slot = slots_[(start + n) % Capacity];
      if (slot.status == Status::Live && slot.source.displayList == displayList) {
        destination = &slot;
        break;
      }
      if (slot.status != Status::Live && !destination) destination = &slot;
      if (slot.status == Status::Empty) break;
    }
    if (!destination) return 0;
    if (destination->status != Status::Live) ++size_;
    destination->status = Status::Live;
    destination->source = {displayList, static_cast<const uint8_t*>(data),
                           bytes, ownerLevel, ++nextGeneration_};
    return destination->source.generation;
  }

  template<class Retire>
  size_t release_after(uint32_t ownerLevel, Retire&& retire) noexcept {
    size_t removed = 0;
    for (auto& slot : slots_) {
      if (slot.status != Status::Live || slot.source.ownerLevel <= ownerLevel) continue;
      retire(slot.source);
      slot.source = {};
      slot.status = Status::Tombstone;
      --size_;
      ++removed;
    }
    return removed;
  }

  void clear() noexcept {
    slots_ = {};
    size_ = 0;
    // Never reuse a generation after a cache clear or a scene change.
  }
  size_t size() const noexcept { return size_; }
};

} // namespace aurora::gx::fifo
