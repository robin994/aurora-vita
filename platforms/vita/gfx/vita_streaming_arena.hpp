#pragma once
#include "vita_buffer_pool.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace aurora::vita::gfx {

struct StreamingArenaConfig {
  size_t vertexBytes = 4 * 1024 * 1024;
  size_t indexBytes = 1024 * 1024;
  uint32_t slots = 3;
  size_t alignment = 16;
};

// Multi-buffered, per-frame streaming arena. CPU writes are staged contiguously and
// flushed to the current GPU VBO/IBO in large ranges at submission boundaries.
class StreamingArena {
public:
  explicit StreamingArena(BufferPool& pool, StreamingArenaConfig cfg = {}) noexcept;
  ~StreamingArena();
  StreamingArena(const StreamingArena&) = delete;
  StreamingArena& operator=(const StreamingArena&) = delete;

  bool initialize() noexcept;
  void shutdown() noexcept;
  void begin_frame(uint64_t frame) noexcept;
  BufferSlice upload_vertices(const void* data, size_t bytes, size_t alignment = 0) noexcept;
  BufferSlice upload_indices(const void* data, size_t bytes, size_t alignment = 0) noexcept;
  // Reserve writable space directly in the frame staging arena. The caller fills
  // it in-place, avoiding an intermediate vector/copy before the frame flush.
  BufferSlice reserve_vertices(size_t bytes, size_t alignment, void** writable) noexcept;
  BufferSlice reserve_indices(size_t bytes, size_t alignment, void** writable) noexcept;
  // Writes U16 indices directly into the staging arena while adding a
  // frame-global vertex base. This avoids a temporary heap allocation per draw.
  BufferSlice upload_rebased_indices(const uint16_t* data, size_t count, uint32_t vertexBase) noexcept;
  bool flush() noexcept;
  void mark_current_submitted() noexcept;
  bool can_reserve(size_t vertexBytes,size_t vertexAlignment,size_t indexBytes,size_t indexAlignment) const noexcept;
  // Called only after the current command stream has been submitted. Advances to
  // a free persistent slot and only drains the GPU once every slot is in flight.
  bool recycle_current() noexcept;

  size_t vertex_used() const noexcept;
  size_t index_used() const noexcept;
  size_t vertex_capacity() const noexcept { return cfg_.vertexBytes; }
  size_t index_capacity() const noexcept { return cfg_.indexBytes; }
  uint32_t slot() const noexcept { return current_; }
  size_t vertex_high_water() const noexcept { return vertexHighWater_; }
  size_t index_high_water() const noexcept { return indexHighWater_; }
  uint64_t vertex_overflows() const noexcept { return vertexOverflows_; }
  uint64_t index_overflows() const noexcept { return indexOverflows_; }
  uint64_t recycles() const noexcept { return recycles_; }
  uint64_t gpu_syncs() const noexcept { return gpuSyncs_; }

private:
  struct Slot {
    Handle vertex=InvalidHandle,index=InvalidHandle;
    size_t voff=0,ioff=0,vflushed=0,iflushed=0;
    void* vmapped=nullptr;
    void* imapped=nullptr;
  };
  BufferSlice reserve(bool vertex, size_t bytes, size_t alignment, void** writable) noexcept;
  BufferSlice upload(bool vertex, const void* data, size_t bytes, size_t alignment) noexcept;
  bool activate_slot(uint32_t index) noexcept;
  bool acquire_slot(uint32_t preferred) noexcept;
  static size_t align_up(size_t value,size_t alignment) noexcept;
  BufferPool& pool_;
  StreamingArenaConfig cfg_{};
  std::vector<Slot> slots_{};
  std::vector<uint8_t> inFlight_{};
  std::vector<uint8_t> vertexStage_{};
  std::vector<uint8_t> indexStage_{};
  uint32_t current_=0;
  bool initialized_=false;
  size_t vertexHighWater_=0,indexHighWater_=0;
  uint64_t vertexOverflows_=0,indexOverflows_=0,recycles_=0,gpuSyncs_=0;
};

} // namespace aurora::vita::gfx
