#pragma once
#include "vita_renderer.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>

namespace aurora::vita::gfx {

// Persistent immutable vertex/index buffers keyed by an exact caller-owned
// geometry signature. Entries are never overwritten while they may be queued
// or in flight; when the bounded budget is exhausted callers fall back to the
// normal streaming path.
class PersistentGeometryCache {
public:
  struct Entry {
    BufferSlice vertices{}, indices{};
    uint32_t vertexCount=0, indexCount=0;
    uint32_t vertexStride=0;
  };

  PersistentGeometryCache(Renderer& renderer,size_t budget) noexcept
      : renderer_(renderer),budget_(budget) {}
  ~PersistentGeometryCache(){clear();}
  PersistentGeometryCache(const PersistentGeometryCache&)=delete;
  PersistentGeometryCache& operator=(const PersistentGeometryCache&)=delete;

  const Entry* find(uint64_t key,uint32_t vertexCount,uint32_t indexCount,
                    uint32_t vertexStride) noexcept {
    const auto it=entries_.find(key);
    if(it==entries_.end()){++misses_;return nullptr;}
    const auto& e=*it->second;
    if(e.vertexCount!=vertexCount||e.indexCount!=indexCount||e.vertexStride!=vertexStride){
      ++misses_;return nullptr;
    }
    ++hits_;return &e;
  }

  const Entry* insert(uint64_t key,const void* vertices,size_t vertexBytes,
                      uint32_t vertexCount,uint32_t vertexStride,
                      const void* indices,size_t indexBytes,uint32_t indexCount) noexcept {
    if(!vertices||!indices||!vertexBytes||!indexBytes||!vertexStride)return nullptr;
    if(const auto it=entries_.find(key);it!=entries_.end())return it->second.get();
    const size_t remaining=budget_>bytes_?budget_-bytes_:0;
    if(vertexBytes+indexBytes>remaining||entries_.size()>=2048)return nullptr;
    auto entry=std::make_unique<Entry>();
    const Handle vb=renderer_.create_vertex_buffer(vertices,vertexBytes,false);
    if(!vb)return nullptr;
    const Handle ib=renderer_.create_index_buffer(indices,indexBytes,false);
    if(!ib){renderer_.buffers().destroy(vb);return nullptr;}
    entry->vertices={vb,0,static_cast<uint32_t>(vertexBytes)};
    entry->indices={ib,0,static_cast<uint32_t>(indexBytes)};
    entry->vertexCount=vertexCount;entry->indexCount=indexCount;entry->vertexStride=vertexStride;
    bytes_+=vertexBytes+indexBytes;
    auto* result=entry.get();
    entries_.emplace(key,std::move(entry));
    return result;
  }

  void clear() noexcept {
    for(auto& pair:entries_){
      renderer_.buffers().destroy(pair.second->vertices.buffer);
      renderer_.buffers().destroy(pair.second->indices.buffer);
    }
    entries_.clear();bytes_=0;hits_=misses_=0;
  }
  size_t bytes()const noexcept{return bytes_;}
  size_t size()const noexcept{return entries_.size();}
  uint64_t hits()const noexcept{return hits_;}
  uint64_t misses()const noexcept{return misses_;}

private:
  Renderer& renderer_;
  size_t budget_=0,bytes_=0;
  uint64_t hits_=0,misses_=0;
  std::unordered_map<uint64_t,std::unique_ptr<Entry>> entries_{};
};

} // namespace aurora::vita::gfx
