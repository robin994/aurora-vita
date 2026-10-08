#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>

namespace aurora::vita::gxm {

struct VertexUniformSpan {
  const float* data = nullptr;
  uint32_t count = 0;
};

// Per-program, CPU-owned copy of the *complete* prepared GXM vertex uniform
// buffer. Always reserve a fresh GXM buffer for each changed draw. Never keep
// a pointer to an old GXM reservation, which may have been recycled already.
class VertexUniformDeltaCache {
public:
  static constexpr size_t MaxBytes = 4096;
  static constexpr size_t MaxSourceBytes = 4096;
  static constexpr size_t MaxSpans = 32;

  bool configure(size_t bytes) noexcept {
    clear();
    if (!bytes || bytes > MaxBytes) return false;
    std::unique_ptr<uint8_t[]> prepared(new (std::nothrow) uint8_t[bytes]);
    if (!prepared) return false;
    prepared_ = std::move(prepared);
    preparedBytes_ = bytes;
    return true;
  }

  size_t bytes() const noexcept { return preparedBytes_; }
  bool ready() const noexcept { return valid_; }

  bool restore(void* fresh, const VertexUniformSpan* spans, size_t count) const noexcept {
    if (!fresh || !valid_ || !layout_matches(spans, count)) return false;
    std::memcpy(fresh, prepared_.get(), preparedBytes_);
    return true;
  }

  bool unchanged(size_t index, const VertexUniformSpan& span) const noexcept {
    if (!valid_ || index >= spanCount_ || span.count != layout_[index].count ||
        (!span.data && span.count)) return false;
    const size_t size = size_t(span.count) * sizeof(float);
    return size == 0 || std::memcmp(inputs_.get() + layout_[index].offset, span.data, size) == 0;
  }

  // Commit only after every GXM setter succeeds. A failed upload can never
  // poison the previously valid shadow of the complete uniform buffer.
  bool commit(const void* completed, const VertexUniformSpan* spans, size_t count) noexcept {
    if (!completed || !prepared_ || !spans || !count || count > MaxSpans) return false;
    size_t total = 0;
    for (size_t i = 0; i < count; ++i) {
      if (!spans[i].data || !spans[i].count ||
          size_t(spans[i].count) > (MaxSourceBytes - total) / sizeof(float)) return false;
      total += size_t(spans[i].count) * sizeof(float);
    }
    if (spanCount_ && !layout_matches(spans, count)) return false;
    if (!inputs_) {
      std::unique_ptr<uint8_t[]> inputs(new (std::nothrow) uint8_t[total]);
      if (!inputs) return false;
      inputs_ = std::move(inputs);
      spanCount_ = count;
      size_t offset = 0;
      for (size_t i = 0; i < count; ++i) {
        layout_[i] = {offset, spans[i].count};
        offset += size_t(spans[i].count) * sizeof(float);
      }
    }
    std::memcpy(prepared_.get(), completed, preparedBytes_);
    for (size_t i = 0; i < count; ++i)
      std::memcpy(inputs_.get() + layout_[i].offset, spans[i].data,
                  size_t(spans[i].count) * sizeof(float));
    valid_ = true;
    return true;
  }

  void invalidate() noexcept { valid_ = false; }

private:
  struct SpanLayout { size_t offset = 0; uint32_t count = 0; };
  bool layout_matches(const VertexUniformSpan* spans, size_t count) const noexcept {
    if (!spans || !inputs_ || !spanCount_ || count != spanCount_) return false;
    for (size_t i = 0; i < count; ++i)
      if (!spans[i].data || spans[i].count != layout_[i].count) return false;
    return true;
  }
  void clear() noexcept {
    inputs_.reset(); prepared_.reset();
    spanCount_ = preparedBytes_ = 0;
    valid_ = false;
  }
  std::unique_ptr<uint8_t[]> prepared_;
  std::unique_ptr<uint8_t[]> inputs_;
  std::array<SpanLayout, MaxSpans> layout_{};
  size_t preparedBytes_ = 0, spanCount_ = 0;
  bool valid_ = false;
};

} // namespace aurora::vita::gxm
