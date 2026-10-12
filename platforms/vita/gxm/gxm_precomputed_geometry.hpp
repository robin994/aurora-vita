#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <unordered_map>

namespace aurora::vita::gxm {

// The payload contains geometry bindings only. Uniforms, textures and raster
// state are deliberately absent: they remain live context state for each draw.
struct PrecomputedGeometryKey {
  uint64_t pipeline = 0, vertexProgram = 0, vertices = 0, indices = 0;
  uint64_t vertexBase = 0, indexOffset = 0;
  uint32_t indexCount = 0, primitive = 0;
  bool operator==(const PrecomputedGeometryKey&) const = default;
};
struct PrecomputedGeometryHash {
  size_t operator()(const PrecomputedGeometryKey& key) const noexcept {
    uint64_t hash = 14695981039346656037ull;
    for (const auto value : {key.pipeline, key.vertexProgram, key.vertices, key.indices,
                            key.vertexBase, key.indexOffset,
                            uint64_t(key.indexCount), uint64_t(key.primitive)}) {
      hash = (hash ^ value) * 1099511628211ull;
    }
    return static_cast<size_t>(hash ^ (hash >> 32));
  }
};

// A single graphics owner submits scenes in order. Extend the GPU's 32-bit
// notification value into that owner's 64-bit sequence. Call observe only
// with this context's exclusive notification word. Outstanding work must stay
// below 2^32 scenes; admission stops well before that ambiguity can arise.
class SceneCompletion {
public:
  static uint64_t extend(uint64_t issued, uint64_t previous, uint32_t value) noexcept {
    const uint64_t distance = uint32_t(uint32_t(issued) - value);
    return distance <= issued && issued - distance > previous ? issued - distance : previous;
  }
  uint64_t next() const noexcept { return issued_ + 1; }
  uint64_t completed() const noexcept { return completed_; }
  bool can_submit() const noexcept { return next() != 0 && issued_ - completed_ < 0x7fffffffull; }
  void submitted() noexcept { ++issued_; }
  void observe(uint32_t value) noexcept {
    completed_ = extend(issued_, completed_, value);
  }
  // Only after a real sceGxmFinish, including all previously submitted scenes.
  void finished() noexcept { completed_ = issued_; }
private:
  uint64_t issued_ = 0, completed_ = 0;
};

// Records are immutable after publish. A hit may be submitted again while an
// earlier GPU use is pending. Eviction/invalidation must NOT modify its payload
// until the last scene using it has completed. Capacity pressure falls back.
template <class Record, size_t Capacity = 512>
class PrecomputedGeometryCache {
public:
  static_assert(Capacity > 0);
  struct Entry {
    Record record{};
    PrecomputedGeometryKey key{};
    uint64_t lastScene = 0;
    bool occupied = false, indexed = false;
  };
  Entry* find(const PrecomputedGeometryKey& key, uint64_t scene) noexcept {
    const auto found = index_.find(key);
    if (found == index_.end()) return nullptr;
    auto& entry = entries_[found->second];
    entry.lastScene = scene;
    return &entry;
  }
  Entry* acquire(uint64_t completed) noexcept {
    for (size_t n = 0; n < Capacity; ++n) {
      const size_t slot = (cursor_ + n) % Capacity;
      auto& entry = entries_[slot];
      if (entry.occupied && entry.lastScene > completed) continue;
      if (entry.indexed) index_.erase(entry.key);
      entry = {};
      cursor_ = (slot + 1) % Capacity;
      return &entry;
    }
    return nullptr;
  }
  void publish(Entry& entry, const PrecomputedGeometryKey& key, uint64_t scene) {
    entry.key = key;
    entry.lastScene = scene;
    entry.occupied = entry.indexed = true;
    index_.emplace(key, slot(entry));
  }
  template <class Predicate>
  void invalidate(Predicate&& predicate) noexcept {
    for (auto& entry : entries_) if (entry.indexed && predicate(entry.key)) {
      index_.erase(entry.key);
      entry.indexed = false;
      // Keep occupied, lastScene and GPU-readable record/backing untouched.
    }
  }
  size_t slot(const Entry& entry) const noexcept { return &entry - entries_.data(); }
private:
  std::array<Entry, Capacity> entries_{};
  std::unordered_map<PrecomputedGeometryKey, size_t, PrecomputedGeometryHash> index_;
  size_t cursor_ = 0;
};
} // namespace aurora::vita::gxm
