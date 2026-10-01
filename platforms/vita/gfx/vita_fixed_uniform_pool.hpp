#pragma once
#include "vita_gfx_types.hpp"
#include <algorithm>
#include <deque>
#include <memory>
#include <vector>

namespace aurora::vita::gfx {

// CPU snapshot storage only. Every acquisition is value-initialized and the
// caller rebuilds all uniforms. Live snapshots have distinct stable addresses
// until reset(), which is legal only after executing/discarding their commands.
// GXM reservation lifetime, snapshot sharing and GX memory are independent.
class FixedUniformPool {
public:
  static constexpr size_t MaxRetainedBytes = 1024u * 1024u;
  struct Stats {
    bool enabled = false;
    uint64_t allocations = 0, reuses = 0, fallbacks = 0;
    size_t retainedBytes = 0;
  };

  void configure(size_t retainedBytes) noexcept {
    clear();
    limit_ = std::min(retainedBytes, MaxRetainedBytes) / sizeof(FixedVertexUniforms);
  }

  FixedVertexUniforms& emplace_back() {
    if (used_ == limit_) {
      if (limit_) ++fallbacks_;
      return overflow_.emplace_back();
    }
    if (used_ == slots_.size()) {
      slots_.push_back(std::make_unique<FixedVertexUniforms>());
      ++allocations_;
    } else {
      *slots_[used_] = FixedVertexUniforms{};
      ++reuses_;
    }
    return *slots_[used_++];
  }

  // The most recent candidate can be discarded when exact snapshot comparison
  // chooses an older live snapshot. That older slot is never overwritten.
  void pop_back() noexcept {
    if (!overflow_.empty()) overflow_.pop_back();
    else if (used_) --used_;
  }

  void reset() noexcept { overflow_.clear(); used_ = 0; }
  void clear() noexcept {
    reset();
    std::vector<std::unique_ptr<FixedVertexUniforms>>{}.swap(slots_);
    limit_ = 0;
    allocations_ = reuses_ = fallbacks_ = 0;
  }
  size_t size() const noexcept { return used_ + overflow_.size(); }
  Stats stats() const noexcept {
    return {limit_ != 0, allocations_, reuses_, fallbacks_, slots_.size() * sizeof(FixedVertexUniforms)};
  }

private:
  std::vector<std::unique_ptr<FixedVertexUniforms>> slots_{};
  // OFF and over-budget draws retain the original std::deque storage path.
  std::deque<FixedVertexUniforms> overflow_{};
  size_t used_ = 0, limit_ = 0;
  uint64_t allocations_ = 0, reuses_ = 0, fallbacks_ = 0;
};

} // namespace aurora::vita::gfx
