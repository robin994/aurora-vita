#pragma once
#include "vita_draw_adapter.hpp"
#include "vita_fixed_vertex.hpp"
#include "vita_byte_compare.hpp"
#include "vita_hash_map.hpp"
#include "vita_memory_revision.hpp"
#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>

namespace aurora::vita::gfx {

// Verified object-space geometry, immutable for its entire GPU lifetime. A
// source which changes at the same key becomes volatile and takes the CPU path.
// Capacity pressure is handled only between frames after one GPU idle wait, so
// buffers are never destroyed while queued or in flight.
class StaticGeometryCache {
public:
  static constexpr size_t MaxEntries=1024;
  static constexpr size_t MaxVolatileKeys=4096;
  static constexpr uint64_t StaleFrames=8;
  struct Snapshot {
    const uint8_t* source=nullptr;
    std::vector<uint8_t> bytes{};
    size_t size=0;
    MemoryRangeStamp stamp{};
    VertexSemantic semantic=VertexSemantic::Position;
    bool revisionTracked=false;
  };
  struct Entry {
    BufferSlice vertices{},indices{};
    uint32_t vertexCount=0,indexCount=0;
    VertexDecodeLayout sourceLayout{};
    VertexLayout gpuLayout{};
    std::vector<uint8_t> raw{};
    std::vector<Snapshot> snapshots{};
    const uint8_t* stableSource=nullptr;
    size_t stableSourceBytes=0;
    MemoryRangeStamp stableSourceStamp{};
    uint64_t lastUseFrame=0;
    size_t residentBytes=0;
    bool volatileSource=false;
  };

  explicit StaticGeometryCache(Renderer& renderer,size_t budget) : renderer_(renderer),budget_(budget) {}
  ~StaticGeometryCache() { clear(); }
  StaticGeometryCache(const StaticGeometryCache&)=delete;
  StaticGeometryCache& operator=(const StaticGeometryCache&)=delete;

  size_t bytes() const noexcept { return bytes_; }
  size_t size() const noexcept { return entries_.size(); }
  uint64_t hits() const noexcept { return hits_; }
  uint64_t misses() const noexcept { return misses_; }
  uint64_t lookup_fallbacks() const noexcept { return lookupFallbacks_; }

  void begin_frame(uint64_t frame,Telemetry* telemetry) noexcept {
    currentFrame_=frame;
    if(!pressure_) {
      if(telemetry)telemetry->geometry_cache_state(entries_.size(),bytes_);
      return;
    }
    struct Victim { uint64_t key=0,lastUse=0; size_t bytes=0; bool volatileSource=false; };
    std::vector<Victim> victims;
    victims.reserve(entries_.size());
    for(const auto& [key,owned]:entries_) {
      const auto& e=*owned;
      const bool stale=frame>e.lastUseFrame && frame-e.lastUseFrame>StaleFrames;
      if(e.volatileSource||stale)victims.push_back({key,e.lastUseFrame,e.residentBytes,e.volatileSource});
    }
    std::sort(victims.begin(),victims.end(),[](const Victim& a,const Victim& b){
      if(a.volatileSource!=b.volatileSource)return a.volatileSource>b.volatileSource;
      if(a.lastUse!=b.lastUse)return a.lastUse<b.lastUse;
      return a.bytes>b.bytes;
    });
    const size_t targetEntries=MaxEntries*3u/4u;
    const size_t targetBytes=budget_*3u/4u;
    uint32_t evictions=0;uint64_t evictedBytes=0;
    if(!victims.empty()) {
      // Native GXM keeps an in-flight bit until finish(). Pay one synchronization
      // per trim pass, then all victim buffers can be destroyed without repeated
      // finish calls from BufferPool::destroy().
      renderer_.buffers().wait_idle();
      for(const auto& victim:victims) {
        if(!victim.volatileSource&&entries_.size()<=targetEntries&&bytes_<=targetBytes)break;
        auto it=entries_.find(victim.key);if(it==entries_.end())continue;
        const size_t resident=it->second->residentBytes;
        renderer_.buffers().destroy(it->second->vertices.buffer);
        renderer_.buffers().destroy(it->second->indices.buffer);
        bytes_=resident>bytes_?0:bytes_-resident;
        entries_.erase(it);
        ++evictions;evictedBytes+=resident;
      }
      if(telemetry&&evictions)telemetry->geometry_trim(evictions,evictedBytes,true);
    }
    pressure_=entries_.size()>targetEntries||bytes_>targetBytes;
    if(telemetry)telemetry->geometry_cache_state(entries_.size(),bytes_);
  }

  const Entry* get(const uint8_t* raw,size_t bytes,uint32_t count,SourcePrimitive primitive,
                   const VertexDecodeLayout& layout,const PipelineDesc& pipeline,
                   const VertexTransformState& state,Telemetry* telemetry,
                   const uint8_t* stableSource=nullptr) noexcept {
    if(!raw||!count||!layout.streamStride||layout.count>layout.attributes.size()||
       count>bytes/layout.streamStride||!pipeline.fixedVertexOnGpu)return nullptr;
    bytes=static_cast<size_t>(count)*layout.streamStride;
    const VertexLayout gpuLayout=fixed_vertex_gpu_layout(pipeline);
    uint64_t key=0;
    auto* detailed=telemetry&&telemetry->split_vertex_phases()?telemetry:nullptr;
    if(telemetry)telemetry->geometry_lookup(stableSource!=nullptr,stableSource?0:bytes);
    { ScopedTelemetryPhase phase(detailed,TelemetryPhase::GeometryKey);
      key=stableSource?make_stable_key(stableSource,count,primitive,layout,gpuLayout):
                       make_key(raw,bytes,count,primitive,layout,gpuLayout);
    }
    if(volatileKeys_.contains(key)) {
      if(telemetry)telemetry->geometry_volatile_bypass();
      return nullptr;
    }
    auto it=entries_.find(key);
    if(it!=entries_.end()) {
      ScopedTelemetryPhase phase(detailed,TelemetryPhase::GeometryValidate);
      auto& e=*it->second;
      if(e.volatileSource){pressure_=true;if(telemetry)telemetry->geometry_reject(GeometryRejectReason::AlreadyVolatile);return nullptr;}
      // Hashes select a candidate only. Every byte that could influence a
      // decoded attribute is checked before an immutable GPU buffer is reused.
      if(!same_layout(layout,e.sourceLayout)||!same_gpu_layout(gpuLayout,e.gpuLayout)){
        if(telemetry)telemetry->geometry_reject(GeometryRejectReason::LayoutMismatch);return nullptr;
      }
      if(stableSource) {
        if(telemetry)telemetry->geometry_validate(0,0,1);
        if(e.stableSource!=stableSource||e.stableSourceBytes!=bytes) {
          e.volatileSource=true;
          pressure_=true;
          remember_volatile_key(key);
          if(telemetry)telemetry->geometry_reject(GeometryRejectReason::StableIdentity);
          return nullptr;
        }
        if(memory_range_changed(stableSource,bytes,e.stableSourceStamp)) {
          e.volatileSource=true;
          pressure_=true;
          remember_volatile_key(key);
          if(telemetry)telemetry->geometry_reject(GeometryRejectReason::StableRevision);
          return nullptr;
        }
      } else {
        if(telemetry)telemetry->geometry_validate(bytes,0,0);
        if(e.raw.size()!=bytes||!byte_spans_equal(raw,e.raw.data(),bytes)){
          if(telemetry)telemetry->geometry_reject(GeometryRejectReason::RawContent);return nullptr;
        }
      }
      for(auto& s:e.snapshots) {
        if(telemetry)telemetry->geometry_validate(0,s.revisionTracked?0:s.bytes.size(),s.revisionTracked?1u:0u);
        const bool changed=s.revisionTracked?
          memory_range_changed(s.source,s.size,s.stamp):
          !byte_spans_equal(s.source,s.bytes.data(),s.bytes.size());
        if(changed) {
          e.volatileSource=true;
          pressure_=true;
          remember_volatile_key(key);
          if(telemetry)telemetry->geometry_reject(s.revisionTracked?GeometryRejectReason::SnapshotRevision:
                                                  GeometryRejectReason::SnapshotContent,
                                                  static_cast<uint32_t>(s.semantic));
          return nullptr;
        }
      }
      ++hits_;
      e.lastUseFrame=currentFrame_;
      return &e;
    }
    ++misses_;
    if(entries_.size()>=MaxEntries){pressure_=true;if(telemetry)telemetry->geometry_reject(GeometryRejectReason::EntryCapacity);return nullptr;}
    if(bytes_>=budget_){pressure_=true;if(telemetry)telemetry->geometry_reject(GeometryRejectReason::ByteCapacity);return nullptr;}
    auto entry=std::make_unique<Entry>();
    entry->sourceLayout=layout;entry->gpuLayout=gpuLayout;
    entry->stableSource=stableSource;entry->stableSourceBytes=stableSource?bytes:0;
    entry->stableSourceStamp=stableSource?memory_range_stamp(stableSource,bytes):MemoryRangeStamp{};
    if(!snapshot_sources(raw,bytes,count,layout,fixed_vertex_gpu_inputs(pipeline),entry->snapshots,stableSource!=nullptr)){
      if(telemetry)telemetry->geometry_reject(GeometryRejectReason::BuildFailure);return nullptr;
    }
    size_t storedBytes=stableSource?0:bytes;
    for(const auto& s:entry->snapshots)storedBytes+=s.revisionTracked?0:s.bytes.size();
    if(storedBytes>budget_-bytes_){pressure_=true;if(telemetry)telemetry->geometry_reject(GeometryRejectReason::ByteCapacity);return nullptr;}
    if(!prepare_draw_into(scratch_,raw,bytes,count,primitive,layout,pipeline,state,nullptr,{},telemetry,true)){
      if(telemetry)telemetry->geometry_reject(GeometryRejectReason::BuildFailure);return nullptr;
    }
    if(scratch_.vertices.empty()||scratch_.indices.empty()){
      if(telemetry)telemetry->geometry_reject(GeometryRejectReason::BuildFailure);return nullptr;
    }
    const size_t stride=gpuLayout.attributes[0].stride;
    const size_t vertexBytes=scratch_.vertices.size()*stride;
    const size_t indexBytes=scratch_.indices.size()*sizeof(uint16_t);
    if(vertexBytes+indexBytes>budget_-bytes_-storedBytes){pressure_=true;if(telemetry)telemetry->geometry_reject(GeometryRejectReason::ByteCapacity);return nullptr;}
    packed_.resize(vertexBytes);
    for(size_t i=0;i<scratch_.vertices.size();++i)
      pack_gpu_vertex_bytes(packed_.data()+i*stride,scratch_.vertices[i],gpuLayout);
    const Handle vb=renderer_.create_vertex_buffer(packed_.data(),vertexBytes,false);
    if(!vb){if(telemetry)telemetry->geometry_reject(GeometryRejectReason::BuildFailure);return nullptr;}
    const Handle ib=renderer_.create_index_buffer(scratch_.indices.data(),indexBytes,false);
    if(!ib) {renderer_.buffers().destroy(vb);if(telemetry)telemetry->geometry_reject(GeometryRejectReason::BuildFailure);return nullptr;}
    entry->vertices={vb,0,static_cast<uint32_t>(vertexBytes)};
    entry->indices={ib,0,static_cast<uint32_t>(indexBytes)};
    entry->vertexCount=static_cast<uint32_t>(scratch_.vertices.size());
    entry->indexCount=static_cast<uint32_t>(scratch_.indices.size());
    entry->lastUseFrame=currentFrame_;
    entry->residentBytes=storedBytes+vertexBytes+indexBytes;
    if(!stableSource)entry->raw.assign(raw,raw+bytes);
    bytes_+=entry->residentBytes;
    auto* result=entry.get();
    entries_.emplace(key,std::move(entry));
    return result;
  }

  void clear() noexcept {
    for(auto& pair:entries_) {
      renderer_.buffers().destroy(pair.second->vertices.buffer);
      renderer_.buffers().destroy(pair.second->indices.buffer);
    }
    entries_.clear();volatileKeys_.clear();bytes_=0;hits_=misses_=lookupFallbacks_=0;pressure_=false;
  }

  static bool same_layout(const VertexDecodeLayout& a,const VertexDecodeLayout& b) noexcept {
    if(a.count!=b.count||a.streamStride!=b.streamStride||a.streamLittleEndian!=b.streamLittleEndian)return false;
    for(unsigned i=0;i<a.count;++i) {
      const auto& x=a.attributes[i];const auto& y=b.attributes[i];
      if(x.semantic!=y.semantic||x.source!=y.source||x.component!=y.component||x.components!=y.components||
         x.frac!=y.frac||x.streamOffset!=y.streamOffset||x.valueOffset!=y.valueOffset||
         x.array.data!=y.array.data||x.array.size!=y.array.size||x.array.stride!=y.array.stride||
         x.array.littleEndian!=y.array.littleEndian)return false;
    }
    return true;
  }

  static bool snapshot_sources(const uint8_t* raw,size_t bytes,uint32_t count,
                               const VertexDecodeLayout& layout,VertexSemanticMask used,
                               std::vector<Snapshot>& output,bool revisionTracked=false) noexcept {
    output.clear();
    if(!raw||!count||!layout.streamStride||layout.count>layout.attributes.size()||count>bytes/layout.streamStride)return false;
    size_t total=0;
    for(unsigned ai=0;ai<layout.count;++ai) {
      const auto& a=layout.attributes[ai];
      if(a.source==VertexSource::None||!(used&vertex_semantic_bit(a.semantic)))continue;
      const size_t valueBytes=component_bytes(a);
      if(!valueBytes)return false;
      if(a.source==VertexSource::Direct) {
        if(static_cast<size_t>(a.streamOffset)+a.valueOffset+valueBytes>layout.streamStride)return false;
        continue;
      }
      if(!a.array.data||!a.array.stride)return false;
      const size_t indexBytes=a.source==VertexSource::Index8?1:2;
      if(static_cast<size_t>(a.streamOffset)+indexBytes>layout.streamStride)return false;
      uint32_t lo=65535,hi=0;
      for(uint32_t i=0;i<count;++i) {
        const auto* p=raw+static_cast<size_t>(i)*layout.streamStride+a.streamOffset;
        const uint32_t index=indexBytes==1?p[0]:(layout.streamLittleEndian?
          (uint32_t(p[0])|(uint32_t(p[1])<<8)):(uint32_t(p[1])|(uint32_t(p[0])<<8)));
        lo=std::min(lo,index);hi=std::max(hi,index);
      }
      const size_t start=static_cast<size_t>(lo)*a.array.stride+a.valueOffset;
      const size_t end=static_cast<size_t>(hi)*a.array.stride+a.valueOffset+valueBytes;
      if(end>a.array.size||start>=end||end-start>2u*1024u*1024u)return false;
      total+=end-start;
      if(total>4u*1024u*1024u)return false;
      Snapshot s{};s.source=a.array.data+start;s.semantic=a.semantic;
      s.size=end-start;s.revisionTracked=revisionTracked;
      if(revisionTracked)s.stamp=memory_range_stamp(s.source,s.size);
      else s.bytes.assign(s.source,s.source+s.size);
      output.push_back(std::move(s));
    }
    return true;
  }

private:
  void remember_volatile_key(uint64_t key) noexcept {
    if(volatileKeys_.size()<MaxVolatileKeys)volatileKeys_.insert(key);
  }

  static size_t component_bytes(const VertexDecodeAttribute& a) noexcept {
    if(a.components<1||a.components>4)return 0;
    switch(a.component) {
      case VertexComponent::U8:case VertexComponent::S8:return a.components;
      case VertexComponent::U16:case VertexComponent::S16:return a.components*2u;
      case VertexComponent::F32:return a.components*4u;
      case VertexComponent::RGB565:case VertexComponent::RGBA4:return 2;
      case VertexComponent::RGB8:case VertexComponent::RGBA6:return 3;
      case VertexComponent::RGBX8:case VertexComponent::RGBA8:return 4;
    }
    return 0;
  }
  static bool same_gpu_layout(const VertexLayout& a,const VertexLayout& b) noexcept {
    if(a.count!=b.count)return false;
    for(unsigned i=0;i<a.count;++i) {
      const auto& x=a.attributes[i];const auto& y=b.attributes[i];
      if(x.location!=y.location||x.components!=y.components||x.scalar!=y.scalar||
         x.normalized!=y.normalized||x.stride!=y.stride||x.offset!=y.offset)return false;
    }
    return true;
  }
  static uint64_t hash_bytes(const uint8_t* data,size_t size) noexcept {
    return byte_span_hash(data,size);
  }
  static uint64_t make_key(const uint8_t* raw,size_t bytes,uint32_t count,SourcePrimitive primitive,
                           const VertexDecodeLayout& layout,const VertexLayout& gpu) noexcept {
    uint32_t h=2166136261u;
    const auto add=[&](uint64_t x){h=(h^static_cast<uint32_t>(x))*16777619u;h=(h^static_cast<uint32_t>(x>>32))*16777619u;};
    add(count);add(static_cast<uint8_t>(primitive));add(layout.count);add(layout.streamStride);add(layout.streamLittleEndian);
    for(unsigned i=0;i<layout.count;++i) {
      const auto& a=layout.attributes[i];
      add(static_cast<uint8_t>(a.semantic));add(static_cast<uint8_t>(a.source));add(static_cast<uint8_t>(a.component));
      add(a.components);add(a.frac);add(a.streamOffset);add(a.valueOffset);
      add(reinterpret_cast<uintptr_t>(a.array.data));add(a.array.size);add(a.array.stride);add(a.array.littleEndian);
    }
    add(gpu.count);
    for(unsigned i=0;i<gpu.count;++i) {const auto& a=gpu.attributes[i];add(a.location);add(a.components);add(static_cast<uint8_t>(a.scalar));add(a.normalized);add(a.offset);add(a.stride);}
    const uint64_t content=hash_bytes(raw,bytes);
    const uint64_t metadata=(static_cast<uint64_t>(h)<<32)|h;
    return content ^ metadata;
  }
  static uint64_t make_stable_key(const uint8_t* stable,uint32_t count,SourcePrimitive primitive,
                                  const VertexDecodeLayout& layout,const VertexLayout& gpu) noexcept {
    uint32_t h=2166136261u;
    const auto add=[&](uint64_t x){h=(h^static_cast<uint32_t>(x))*16777619u;h=(h^static_cast<uint32_t>(x>>32))*16777619u;};
    add(reinterpret_cast<uintptr_t>(stable));add(count);add(static_cast<uint8_t>(primitive));
    add(layout.count);add(layout.streamStride);add(layout.streamLittleEndian);
    for(unsigned i=0;i<layout.count;++i) {
      const auto& a=layout.attributes[i];
      add(static_cast<uint8_t>(a.semantic));add(static_cast<uint8_t>(a.source));add(static_cast<uint8_t>(a.component));
      add(a.components);add(a.frac);add(a.streamOffset);add(a.valueOffset);
      add(reinterpret_cast<uintptr_t>(a.array.data));add(a.array.size);add(a.array.stride);add(a.array.littleEndian);
    }
    add(gpu.count);
    for(unsigned i=0;i<gpu.count;++i) {const auto& a=gpu.attributes[i];add(a.location);add(a.components);add(static_cast<uint8_t>(a.scalar));add(a.normalized);add(a.offset);add(a.stride);}
    const uint64_t metadata=(static_cast<uint64_t>(h)<<32)|h;
    const uintptr_t address=reinterpret_cast<uintptr_t>(stable);
    return XXH3_64bits_withSeed(&address,sizeof(address),metadata);
  }
  Renderer& renderer_;
  size_t budget_=0,bytes_=0;
  uint64_t hits_=0,misses_=0,lookupFallbacks_=0;
  uint64_t currentFrame_=0;
  bool pressure_=false;
  FlatHashMap<uint64_t,std::unique_ptr<Entry>> entries_{};
  FlatHashSet<uint64_t> volatileKeys_{};
  PreparedDraw scratch_{};
  std::vector<uint8_t> packed_{};
};

} // namespace aurora::vita::gfx
