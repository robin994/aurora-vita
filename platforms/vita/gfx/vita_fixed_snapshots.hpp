#pragma once
#include "vita_gfx_types.hpp"
#include <cstring>
#include <deque>

namespace aurora::vita::gfx {
// The reusable scratch never escapes. Only published immutable values are
// referenced by draws, and deque growth preserves their addresses until clear.
template<class Allocator=std::allocator<FixedVertexUniforms>> class FixedUniformSnapshotStore {
  FixedVertexUniforms scratch_{};
  std::deque<FixedVertexUniforms,Allocator> snapshots_{};
  FixedVertexUniforms* last_=nullptr;
  uint64_t revision_=0;
public:
  FixedVertexUniforms& scratch() noexcept {return scratch_;}
  FixedVertexUniforms& publish(bool reuse=true,bool distinct=false) noexcept {
    if(reuse && !distinct && last_ &&
        std::memcmp(last_,&scratch_,offsetof(FixedVertexUniforms,revision))==0)return *last_;
    scratch_.revision=++revision_;
    auto& stored=snapshots_.emplace_back(scratch_);
    last_=distinct?nullptr:&stored;
    return stored;
  }
  void clear() noexcept {snapshots_.clear();last_=nullptr;}
  size_t size() const noexcept {return snapshots_.size();}
};
using FixedUniformSnapshots=FixedUniformSnapshotStore<>;
} // namespace aurora::vita::gfx
