#pragma once
#include "vita_draw_adapter.hpp"
#include "vita_fixed_vertex.hpp"
#include "vita_byte_compare.hpp"
#include <memory>
#include <unordered_map>

namespace aurora::vita::gfx {

// Verified object-space geometry, immutable for its entire GPU lifetime. A
// source which changes at the same key becomes volatile and takes the CPU path;
// it is never overwritten while queued or in flight. This first bounded cache
// intentionally does not evict during rendering.
class StaticGeometryCache {
public:
  struct Snapshot {
    const uint8_t* source=nullptr;
    std::vector<uint8_t> bytes{};
  };
  struct Entry {
    BufferSlice vertices{},indices{};
    uint32_t vertexCount=0,indexCount=0;
    VertexDecodeLayout sourceLayout{};
    VertexLayout gpuLayout{};
    std::vector<uint8_t> raw{};
    std::vector<Snapshot> snapshots{};
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

  const Entry* get(const uint8_t* raw,size_t bytes,uint32_t count,SourcePrimitive primitive,
                   const VertexDecodeLayout& layout,const PipelineDesc& pipeline,
                   const VertexTransformState& state,Telemetry* telemetry) noexcept {
    if(!raw||!count||!layout.streamStride||layout.count>layout.attributes.size()||
       count>bytes/layout.streamStride||!pipeline.fixedVertexOnGpu)return nullptr;
    bytes=static_cast<size_t>(count)*layout.streamStride;
    const VertexLayout gpuLayout=fixed_vertex_gpu_layout(pipeline);
    uint64_t key=0;
    auto* detailed=telemetry&&telemetry->split_vertex_phases()?telemetry:nullptr;
    { ScopedTelemetryPhase phase(detailed,TelemetryPhase::GeometryKey);
      key=make_key(raw,bytes,count,primitive,layout,gpuLayout);
    }
    auto it=entries_.find(key);
    if(it!=entries_.end()) {
      ScopedTelemetryPhase phase(detailed,TelemetryPhase::GeometryValidate);
      auto& e=*it->second;
      if(e.volatileSource)return nullptr;
      // Hashes select a candidate only. Every byte that could influence a
      // decoded attribute is checked before an immutable GPU buffer is reused.
      if(!same_layout(layout,e.sourceLayout)||!same_gpu_layout(gpuLayout,e.gpuLayout)||
         e.raw.size()!=bytes||!byte_spans_equal(raw,e.raw.data(),bytes))return nullptr;
      for(const auto& s:e.snapshots) {
        if(!byte_spans_equal(s.source,s.bytes.data(),s.bytes.size())) {
          e.volatileSource=true;
          return nullptr;
        }
      }
      ++hits_;
      return &e;
    }
    ++misses_;
    if(entries_.size()>=1024||bytes_>=budget_)return nullptr;
    auto entry=std::make_unique<Entry>();
    entry->sourceLayout=layout;entry->gpuLayout=gpuLayout;
    if(!snapshot_sources(raw,bytes,count,layout,fixed_vertex_gpu_inputs(pipeline),entry->snapshots))return nullptr;
    size_t storedBytes=bytes;
    for(const auto& s:entry->snapshots)storedBytes+=s.bytes.size();
    if(storedBytes>budget_-bytes_)return nullptr;
    if(!prepare_draw_into(scratch_,raw,bytes,count,primitive,layout,pipeline,state,nullptr,{},telemetry,true))return nullptr;
    if(scratch_.vertices.empty()||scratch_.indices.empty())return nullptr;
    const size_t stride=gpuLayout.attributes[0].stride;
    const size_t vertexBytes=scratch_.vertices.size()*stride;
    const size_t indexBytes=scratch_.indices.size()*sizeof(uint16_t);
    if(vertexBytes+indexBytes>budget_-bytes_-storedBytes)return nullptr;
    packed_.resize(vertexBytes);
    for(size_t i=0;i<scratch_.vertices.size();++i)
      pack_gpu_vertex_bytes(packed_.data()+i*stride,scratch_.vertices[i],gpuLayout);
    const Handle vb=renderer_.create_vertex_buffer(packed_.data(),vertexBytes,false);
    if(!vb)return nullptr;
    const Handle ib=renderer_.create_index_buffer(scratch_.indices.data(),indexBytes,false);
    if(!ib) {renderer_.buffers().destroy(vb);return nullptr;}
    entry->vertices={vb,0,static_cast<uint32_t>(vertexBytes)};
    entry->indices={ib,0,static_cast<uint32_t>(indexBytes)};
    entry->vertexCount=static_cast<uint32_t>(scratch_.vertices.size());
    entry->indexCount=static_cast<uint32_t>(scratch_.indices.size());
    entry->raw.assign(raw,raw+bytes);
    bytes_+=storedBytes+vertexBytes+indexBytes;
    auto* result=entry.get();
    entries_.emplace(key,std::move(entry));
    return result;
  }

  void clear() noexcept {
    for(auto& pair:entries_) {
      renderer_.buffers().destroy(pair.second->vertices.buffer);
      renderer_.buffers().destroy(pair.second->indices.buffer);
    }
    entries_.clear();bytes_=0;hits_=misses_=0;
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
                               std::vector<Snapshot>& output) noexcept {
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
      Snapshot s{};s.source=a.array.data+start;
      s.bytes.assign(s.source,s.source+(end-start));
      output.push_back(std::move(s));
    }
    return true;
  }

private:
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
  static uint32_t hash_bytes(const uint8_t* data,size_t size) noexcept {
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
    return (static_cast<uint64_t>(h)<<32)|hash_bytes(raw,bytes);
  }
  Renderer& renderer_;
  size_t budget_=0,bytes_=0;
  uint64_t hits_=0,misses_=0;
  std::unordered_map<uint64_t,std::unique_ptr<Entry>> entries_{};
  PreparedDraw scratch_{};
  std::vector<uint8_t> packed_{};
};

} // namespace aurora::vita::gfx
