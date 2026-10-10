#pragma once
#include "vita_gfx_types.hpp"
#include "vita_texture_decode.hpp"
#include "vita_hash_map.hpp"
#include <xxhash.h>
#include <algorithm>
#include <cstring>
#include <vector>

namespace aurora::vita::gfx {

struct TextureSnapshotStats {
  uint64_t hits=0,misses=0,uploads=0,evictions=0;
  size_t entries=0,gpuBytes=0,sourceBytes=0,budget=0;
};

// Content identity for small, privately uploaded textures. Guest addresses and
// revisions are deliberately excluded: compare the current bytes on every
// lookup, including no-cache sources and repeated writes within the same frame.
// This index owns CPU copies only. TextureCache retains sole GPU ownership and
// performs the existing synchronized destruction when an entry is evicted.
class TextureSnapshots {
public:
  static constexpr size_t MaxSourceBytes=4096,MaxEntries=1024;
  using Hasher=uint64_t (*)(const TextureDesc&) noexcept;
  explicit TextureSnapshots(Hasher hasher=content_hash) noexcept:hasher_(hasher) {}
  void configure(size_t budget) noexcept {clear();budget_=budget;}
  bool eligible(const TextureDesc& d) const noexcept {
    if(!budget_||!d.width||!d.height||d.width>4096||d.height>4096||
       !d.data||!d.dataSize||d.dataSize>MaxSourceBytes||
       d.paletteSize>MaxSourceBytes-d.dataSize||
       (d.paletteSize&&!d.palette)||d.mipCount!=1||d.generateMipmaps)return false;
    const size_t encoded=encoded_texture_size(d.width,d.height,d.format);
    return encoded&&encoded<=d.dataSize;
  }
  Handle find(const TextureDesc& d,uint64_t frame) noexcept {
    if(!eligible(d))return InvalidHandle;
    const auto bucket=buckets_.find(hasher_(d));
    if(bucket!=buckets_.end())for(const auto handle:bucket->second) {
      auto& record=records_.at(handle);
      if(same_shape(d,record.desc)&&
         std::memcmp(d.data,record.source.data(),d.dataSize)==0&&
         (!d.paletteSize||std::memcmp(d.palette,record.source.data()+d.dataSize,d.paletteSize)==0)) {
        record.lastUse=frame;++hits_;return handle;
      }
    }
    ++misses_;return InvalidHandle;
  }
  bool has_room(size_t gpuBytes) const noexcept {
    return budget_&&gpuBytes&&records_.size()<MaxEntries&&
           gpuBytes<=budget_&&gpuBytes_<=budget_-gpuBytes;
  }
  Handle oldest_before(uint64_t frame) const noexcept {
    Handle victim=InvalidHandle;uint64_t oldest=frame;
    for(const auto& [handle,record]:records_)
      if(record.lastUse<oldest){victim=handle;oldest=record.lastUse;}
    return victim;
  }
  bool remember(const TextureDesc& d,Handle handle,size_t gpuBytes,uint64_t frame) {
    if(!handle||records_.contains(handle)||!eligible(d)||!has_room(gpuBytes))return false;
    Record record;record.desc=d;record.desc.data=record.desc.palette=nullptr;
    record.key=hasher_(d);record.lastUse=frame;record.gpuBytes=gpuBytes;
    const auto* data=static_cast<const uint8_t*>(d.data);
    record.source.assign(data,data+d.dataSize);
    if(d.paletteSize){const auto* palette=static_cast<const uint8_t*>(d.palette);
      record.source.insert(record.source.end(),palette,palette+d.paletteSize);}
    sourceBytes_+=record.source.size();gpuBytes_+=gpuBytes;
    buckets_[record.key].push_back(handle);records_.emplace(handle,std::move(record));
    ++uploads_;return true;
  }
  void erase(Handle handle) noexcept {
    const auto it=records_.find(handle);if(it==records_.end())return;
    auto bucket=buckets_.find(it->second.key);
    auto& handles=bucket->second;
    handles.erase(std::remove(handles.begin(),handles.end(),handle),handles.end());
    if(handles.empty())buckets_.erase(bucket);
    gpuBytes_-=it->second.gpuBytes;sourceBytes_-=it->second.source.size();
    records_.erase(it);++evictions_;
  }
  void clear() noexcept {records_.clear();buckets_.clear();gpuBytes_=sourceBytes_=0;}
  TextureSnapshotStats stats() const noexcept {
    return {hits_,misses_,uploads_,evictions_,records_.size(),gpuBytes_,sourceBytes_,budget_};
  }
private:
  struct Record {
    TextureDesc desc{};uint64_t key=0,lastUse=0;size_t gpuBytes=0;
    std::vector<uint8_t> source;
  };
  static bool same_shape(const TextureDesc& a,const TextureDesc& b) noexcept {
    return a.width==b.width&&a.height==b.height&&a.format==b.format&&
      a.paletteFormat==b.paletteFormat&&a.mipCount==b.mipCount&&
      a.generateMipmaps==b.generateMipmaps&&a.dataSize==b.dataSize&&a.paletteSize==b.paletteSize;
  }
  static uint64_t content_hash(const TextureDesc& d) noexcept {
    const uint32_t shape[]{d.width,d.height,uint32_t(d.format),uint32_t(d.paletteFormat),
      d.mipCount,uint32_t(d.dataSize),uint32_t(d.paletteSize)};
    const uint32_t header=XXH32(shape,sizeof(shape),0x534e4150u);
    const uint32_t pixels=XXH32(d.data,d.dataSize,header);
    const uint32_t palette=d.paletteSize?XXH32(d.palette,d.paletteSize,pixels^header):header;
    return (uint64_t(pixels)<<32)|palette;
  }
  Hasher hasher_;size_t budget_=0,gpuBytes_=0,sourceBytes_=0;
  uint64_t hits_=0,misses_=0,uploads_=0,evictions_=0;
  NodeHashMap<Handle,Record> records_;
  FlatHashMap<uint64_t,std::vector<Handle>> buckets_;
};
} // namespace aurora::vita::gfx
