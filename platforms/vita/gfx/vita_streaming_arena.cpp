#include "vita_streaming_arena.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#if defined(__vita__) && !defined(AURORA_VITA_RENDERER_GXM)
#include <vitaGL.h>
#endif

#ifndef AURORA_VITA_DIRECT_STREAM_WRITE
#if defined(AURORA_VITAGL_MAPPED_STREAM_UPLOAD)
#define AURORA_VITA_DIRECT_STREAM_WRITE 1
#else
#define AURORA_VITA_DIRECT_STREAM_WRITE 0
#endif
#endif

namespace aurora::vita::gfx {

StreamingArena::StreamingArena(BufferPool& pool, StreamingArenaConfig cfg) noexcept : pool_(pool), cfg_(cfg) {}
StreamingArena::~StreamingArena() { shutdown(); }

size_t StreamingArena::align_up(size_t value, size_t alignment) noexcept {
  if (alignment <= 1) return value;
  const size_t remainder = value % alignment;
  return remainder ? value + (alignment - remainder) : value;
}

bool StreamingArena::initialize() noexcept {
  if (initialized_) return true;
  if (cfg_.slots == 0 || cfg_.vertexBytes == 0 || cfg_.indexBytes == 0) return false;

  slots_.resize(cfg_.slots);
  inFlight_.assign(cfg_.slots, 0);
  submittedFrame_.assign(cfg_.slots, std::numeric_limits<uint64_t>::max());
  for (auto& slot : slots_) {
    slot.vertex = pool_.create_vertex(nullptr, cfg_.vertexBytes, true);
    slot.index = pool_.create_index(nullptr, cfg_.indexBytes, true);
    if (!slot.vertex || !slot.index) {
      shutdown();
      return false;
    }
  }
#if !AURORA_VITA_DIRECT_STREAM_WRITE
  vertexStage_.resize(cfg_.vertexBytes);
  indexStage_.resize(cfg_.indexBytes);
#else
  vertexStage_.clear();
  indexStage_.clear();
#endif
  initialized_ = true;
  return true;
}

void StreamingArena::shutdown() noexcept {
  for (auto& slot : slots_) {
#if defined(__vita__) && AURORA_VITA_DIRECT_STREAM_WRITE
    if (slot.vmapped && slot.vertex) { glBindBuffer(GL_ARRAY_BUFFER, pool_.gl_id(slot.vertex)); (void)glUnmapBuffer(GL_ARRAY_BUFFER); slot.vmapped = nullptr; }
    if (slot.imapped && slot.index) { glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, pool_.gl_id(slot.index)); (void)glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER); slot.imapped = nullptr; }
#endif
    if (slot.vertex) pool_.destroy(slot.vertex);
    if (slot.index) pool_.destroy(slot.index);
  }
  slots_.clear();
  inFlight_.clear();
  submittedFrame_.clear();
  vertexStage_.clear();
  indexStage_.clear();
  initialized_ = false;
  vertexHighWater_ = indexHighWater_ = 0;
  vertexOverflows_ = indexOverflows_ = recycles_ = gpuSyncs_ = 0;
  current_ = 0;
  currentFrame_ = 0;
}

bool StreamingArena::activate_slot(uint32_t index) noexcept {
  if (!initialized_ || index >= slots_.size()) return false;
  auto& slot = slots_[index];
#if defined(__vita__) && AURORA_VITA_DIRECT_STREAM_WRITE
  if (slot.vmapped && slot.vertex) { glBindBuffer(GL_ARRAY_BUFFER, pool_.gl_id(slot.vertex)); (void)glUnmapBuffer(GL_ARRAY_BUFFER); slot.vmapped = nullptr; }
  if (slot.imapped && slot.index) { glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, pool_.gl_id(slot.index)); (void)glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER); slot.imapped = nullptr; }
#endif
  current_ = index;
  slot.voff = 0;
  slot.ioff = 0;
  slot.vflushed = 0;
  slot.iflushed = 0;
  return true;
}

bool StreamingArena::acquire_slot(uint32_t preferred) noexcept {
  if (!initialized_ || slots_.empty()) return false;
  const uint32_t count = static_cast<uint32_t>(slots_.size());
  preferred %= count;
  if (!inFlight_[preferred]) return activate_slot(preferred);
  for (uint32_t step = 1; step < count; ++step) {
    const uint32_t index = (preferred + step) % count;
    if (!inFlight_[index]) return activate_slot(index);
  }

  // Completion is owned by the selected hardware backend, not the allocator.
#if defined(__vita__)
  pool_.wait_idle();
  ++gpuSyncs_;
#endif
  std::fill(inFlight_.begin(), inFlight_.end(), 0);
  std::fill(submittedFrame_.begin(), submittedFrame_.end(), std::numeric_limits<uint64_t>::max());
  return activate_slot(preferred);
}

void StreamingArena::begin_frame(uint64_t frame) noexcept {
  if (!initialized_ || slots_.empty()) return;
  currentFrame_=frame;
  const uint32_t preferred = static_cast<uint32_t>(frame % slots_.size());

  // Streaming pages are not intrinsically tied to display-buffer indices: a
  // large frame can spill into several pages. Retire each page by the frame in
  // which it was actually submitted rather than assuming `frame % slots` owns
  // it. Once the display ring has advanced `framesInFlight` times, vitaGL/GXM
  // has synchronized the corresponding work and that storage is safe to reuse.
  const uint64_t latency=std::max<uint32_t>(1,cfg_.framesInFlight);
  for(size_t i=0;i<inFlight_.size();++i){
    if(!inFlight_[i]||submittedFrame_[i]==std::numeric_limits<uint64_t>::max())continue;
    if(frame>=submittedFrame_[i]&&frame-submittedFrame_[i]>=latency){
      inFlight_[i]=0;
      submittedFrame_[i]=std::numeric_limits<uint64_t>::max();
    }
  }
  (void)acquire_slot(preferred);
}

BufferSlice StreamingArena::reserve(bool vertex, size_t bytes, size_t alignment, void** writable) noexcept {
  if (writable) *writable = nullptr;
  if (!initialized_ || !bytes || !writable) return {};
  auto& slot = slots_[current_];
  const size_t requestedAlignment = alignment ? alignment : cfg_.alignment;
  size_t& offset = vertex ? slot.voff : slot.ioff;
  const size_t capacity = vertex ? cfg_.vertexBytes : cfg_.indexBytes;
  const size_t aligned = align_up(offset, requestedAlignment);
  if (aligned > capacity || bytes > capacity - aligned) {
    if (vertex) ++vertexOverflows_; else ++indexOverflows_;
    return {};
  }

  const Handle handle = vertex ? slot.vertex : slot.index;
#if defined(__vita__) && AURORA_VITA_DIRECT_STREAM_WRITE
  void*& mapped = vertex ? slot.vmapped : slot.imapped;
  if (!mapped) {
    const GLenum target = vertex ? GL_ARRAY_BUFFER : GL_ELEMENT_ARRAY_BUFFER;
    const GLuint id = pool_.gl_id(handle);
    if (!id) return {};
    glBindBuffer(target, id);
    mapped = glMapBufferRange(target, 0, static_cast<GLsizeiptr>(capacity), GL_MAP_WRITE_BIT);
    if (!mapped) return {};
  }
  *writable = static_cast<uint8_t*>(mapped) + aligned;
#else
  auto& stage = vertex ? vertexStage_ : indexStage_;
  *writable = stage.data() + aligned;
#endif
  offset = aligned + bytes;
  if (vertex) { if (offset > vertexHighWater_) vertexHighWater_ = offset; }
  else { if (offset > indexHighWater_) indexHighWater_ = offset; }
  return {handle, static_cast<uint32_t>(aligned), static_cast<uint32_t>(bytes)};
}

BufferSlice StreamingArena::upload(bool vertex, const void* data, size_t bytes, size_t alignment) noexcept {
  if (!data) return {};
  void* writable = nullptr;
  const BufferSlice slice = reserve(vertex, bytes, alignment, &writable);
  if (!slice.buffer || !writable) return {};
  std::memcpy(writable, data, bytes);
  return slice;
}

BufferSlice StreamingArena::upload_vertices(const void* data, size_t bytes, size_t alignment) noexcept {
  return upload(true, data, bytes, alignment);
}
BufferSlice StreamingArena::upload_indices(const void* data, size_t bytes, size_t alignment) noexcept {
  return upload(false, data, bytes, alignment);
}
BufferSlice StreamingArena::reserve_vertices(size_t bytes, size_t alignment, void** writable) noexcept {
  return reserve(true, bytes, alignment, writable);
}
BufferSlice StreamingArena::reserve_indices(size_t bytes, size_t alignment, void** writable) noexcept {
  return reserve(false, bytes, alignment, writable);
}
BufferSlice StreamingArena::upload_rebased_indices(const uint16_t* data, size_t count, uint32_t vertexBase) noexcept {
  if (!initialized_ || !data || !count || count > std::numeric_limits<size_t>::max() / sizeof(uint16_t)) return {};
  if (vertexBase > std::numeric_limits<uint16_t>::max()) return {};
  for (size_t i = 0; i < count; ++i) {
    if (data[i] > std::numeric_limits<uint16_t>::max() - vertexBase) return {};
  }
  const size_t bytes = count * sizeof(uint16_t);
  void* writable = nullptr;
  const BufferSlice slice = reserve_indices(bytes, alignof(uint16_t), &writable);
  if (!slice.buffer || !writable) return {};
  auto* destination = static_cast<uint16_t*>(writable);
  for (size_t i = 0; i < count; ++i) {
    destination[i] = static_cast<uint16_t>(vertexBase + data[i]);
  }
  return slice;
}
bool StreamingArena::flush() noexcept {
  if (!initialized_ || slots_.empty()) return false;
  auto& slot = slots_[current_];
#if defined(__vita__) && AURORA_VITA_DIRECT_STREAM_WRITE
  bool ok = true;
  if (slot.vmapped) {
    glBindBuffer(GL_ARRAY_BUFFER, pool_.gl_id(slot.vertex));
    ok = glUnmapBuffer(GL_ARRAY_BUFFER) == GL_TRUE && ok;
    slot.vmapped = nullptr;
    slot.vflushed = slot.voff;
  }
  if (slot.imapped) {
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, pool_.gl_id(slot.index));
    ok = glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER) == GL_TRUE && ok;
    slot.imapped = nullptr;
    slot.iflushed = slot.ioff;
  }
  return ok;
#else
  if (slot.voff > slot.vflushed) {
    const size_t bytes = slot.voff - slot.vflushed;
    if (!pool_.update(slot.vertex, vertexStage_.data() + slot.vflushed, bytes, slot.vflushed)) return false;
    slot.vflushed = slot.voff;
  }
  if (slot.ioff > slot.iflushed) {
    const size_t bytes = slot.ioff - slot.iflushed;
    if (!pool_.update(slot.index, indexStage_.data() + slot.iflushed, bytes, slot.iflushed)) return false;
    slot.iflushed = slot.ioff;
  }
  return true;
#endif
}
void StreamingArena::mark_current_submitted() noexcept {
  if (initialized_ && current_ < inFlight_.size()) {
    inFlight_[current_] = 1;
    submittedFrame_[current_] = currentFrame_;
  }
}
bool StreamingArena::can_reserve(size_t vertexBytes,size_t vertexAlignment,size_t indexBytes,size_t indexAlignment) const noexcept {
  if(!initialized_||slots_.empty())return false;
  const auto&slot=slots_[current_];
  const size_t va=align_up(slot.voff,vertexAlignment?vertexAlignment:cfg_.alignment);
  const size_t ia=align_up(slot.ioff,indexAlignment?indexAlignment:cfg_.alignment);
  return va<=cfg_.vertexBytes&&vertexBytes<=cfg_.vertexBytes-va&&
         ia<=cfg_.indexBytes&&indexBytes<=cfg_.indexBytes-ia;
}
bool StreamingArena::recycle_current() noexcept {
  if(!initialized_||slots_.empty())return false;
  auto&slot=slots_[current_];
#if defined(__vita__) && AURORA_VITA_DIRECT_STREAM_WRITE
  if(slot.vmapped||slot.imapped)return false;
#endif
  const uint32_t next = static_cast<uint32_t>((current_ + 1u) % slots_.size());
  if (!acquire_slot(next)) return false;
  ++recycles_;
  return true;
}
size_t StreamingArena::vertex_used() const noexcept { return initialized_ ? slots_[current_].voff : 0; }
size_t StreamingArena::index_used() const noexcept { return initialized_ ? slots_[current_].ioff : 0; }

} // namespace aurora::vita::gfx
