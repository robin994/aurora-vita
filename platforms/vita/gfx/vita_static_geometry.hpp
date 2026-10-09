#pragma once
#include "vita_draw_adapter.hpp"
#include "vita_native_assets.hpp"
#include "vita_cpu_workers.hpp"
#include "vita_fixed_vertex.hpp"
#include "vita_byte_compare.hpp"
#include "vita_hash_map.hpp"
#include "vita_memory_revision.hpp"
#include <algorithm>
#include <cstdio>
#include <limits>
#include <memory>

namespace aurora::vita::gfx {

// Verified object-space geometry, immutable for its entire GPU lifetime. A
// source which changes at the same key becomes volatile and takes the CPU path;
// it is never overwritten while queued or in flight. When the budget is full,
// least-recently-used entries not referenced by the current frame are evicted.
// Their GPU storage stays charged to the cache budget until retirement and is
// released only after RetireFrames plus one explicit GPU-idle barrier. Frame age
// alone is not a sufficient lifetime proof across rapid scene/target changes.
class StaticGeometryCache {
public:
  struct Snapshot {
    const uint8_t* source=nullptr;
    std::vector<uint8_t> bytes{};
    size_t size=0;
    uint64_t revision=0;
    bool revisionTracked=false;
  };
  struct Entry {
    BufferSlice vertices{},indices{};
    uint32_t vertexCount=0,indexCount=0;
    uint64_t pipelineDescKey=0,pipelineKey=0;
    VertexDecodeLayout sourceLayout{};
    VertexLayout gpuLayout{};
    std::vector<uint8_t> raw{};
    std::vector<uint16_t> sourceIndices{};
    std::vector<Snapshot> snapshots{};
    const uint8_t* stableSource=nullptr;
    size_t stableSourceBytes=0;
    uint64_t stableSourceRevision=0;
    uint64_t validationEpoch=0;
    uint64_t lastUseFrame=0;
    size_t accountedBytes=0;
    bool volatileSource=false;
    // A6 may replay an already-submitted AVNR v2 draw in the *same frame*.
    // This is a cache lookup identity, never a serialized GPU handle.
    uint64_t replayKey=0;
    bool nativeGpuPacked=false;
  };
  static constexpr uint64_t RetireFrames=4;

  explicit StaticGeometryCache(Renderer& renderer,size_t budget,bool budgetPreflight=false,
                               bool nativeGpuStatic=false)
      : renderer_(renderer),budget_(budget),budgetPreflight_(budgetPreflight),
        nativeGpuStatic_(nativeGpuStatic) {}
  ~StaticGeometryCache() { clear(); }
  StaticGeometryCache(const StaticGeometryCache&)=delete;
  StaticGeometryCache& operator=(const StaticGeometryCache&)=delete;

  size_t bytes() const noexcept { return bytes_ + retiredBytes_; }
  size_t retired_bytes() const noexcept { return retiredBytes_; }
  size_t size() const noexcept { return entries_.size(); }
  uint64_t hits() const noexcept { return hits_; }
  uint64_t misses() const noexcept { return misses_; }
  // Counts every rejected get(), including existing volatile entries. Misses
  // above count absent keys and can overlap this counter; do not sum them.
  uint64_t lookup_fallbacks() const noexcept { return lookupFallbacks_; }
  uint64_t evictions() const noexcept { return evictions_; }
  uint64_t budget_preflight_rejects() const noexcept { return budgetPreflightRejects_; }

  // Only owner-thread, same-frame AVNR v2 resources are replayable. The
  // display-list transport separately pins/compares its original bytes.
  bool validate_native_replay(uint64_t key,const Entry* identity) noexcept {
    if(!key||!identity)return false;
    const auto it=entries_.find(key);
    if(it==entries_.end()||it->second.get()!=identity)return false;
    auto& e=*it->second;
    if(!e.nativeGpuPacked||e.volatileSource||!e.stableSource||e.lastUseFrame!=frame_)return false;
    return validate_native_model(key,identity);
  }

  // Cross-frame recipes own no buffers. Resolve and validate the resident
  // entry before touching its pointer, and charge its use to this frame.
  bool validate_native_model(uint64_t key,const Entry* identity) noexcept {
    if(!key||!identity)return false;
    const auto it=entries_.find(key);
    if(it==entries_.end()||it->second.get()!=identity)return false;
    auto& e=*it->second;
    if(e.volatileSource||!e.stableSource)return false;
    const uint64_t epoch=memory_write_epoch();
    if(epoch!=e.validationEpoch){
      std::array<MemoryRevisionCheck,MaxVertexAttributes+1> checks{};
      size_t count=0;
      checks[count++]={e.stableSource,e.stableSourceBytes,e.stableSourceRevision};
      for(const auto& s:e.snapshots){
        if(s.revisionTracked){
          if(count>=checks.size())return false;
          checks[count++]={s.source,s.size,s.revision};
        } else if(!byte_spans_equal(s.source,s.bytes.data(),s.bytes.size()))return false;
      }
      if(!memory_ranges_match(checks.data(),count))return false;
      e.validationEpoch=epoch;
    }
    e.lastUseFrame=frame_;
    return true;
  }

  // Called once per frame before any lookup. Retired buffers are destroyed in
  // one synchronized batch: do not rely on per-buffer inFlight bookkeeping for
  // memory that may span several GXM scenes and display-queue submissions.
  void begin_frame(uint64_t frame) noexcept {
    frame_=frame;
    bool hasDue=false;
    for(const auto& r:retired_)
      if(frame_>=r.frame+RetireFrames){hasDue=true;break;}
    if(hasDue)renderer_.buffers().wait_idle();
    size_t kept=0;
    for(size_t i=0;i<retired_.size();++i) {
      auto& r=retired_[i];
      if(frame_>=r.frame+RetireFrames) {
        renderer_.buffers().destroy_retired(r.vertices);
        renderer_.buffers().destroy_retired(r.indices);
        retiredBytes_=r.gpuBytes<=retiredBytes_?retiredBytes_-r.gpuBytes:0;
      } else retired_[kept++]=r;
    }
    retired_.resize(kept);
  }

  Entry* get(const uint8_t* raw,size_t bytes,uint32_t count,SourcePrimitive primitive,
             const VertexDecodeLayout& layout,const PipelineDesc& pipeline,
             const VertexTransformState& state,Telemetry* telemetry,
             const uint8_t* stableSource=nullptr,
             const uint16_t* sourceIndices=nullptr,uint32_t sourceIndexCount=0,
             const DrawRecipe* recipe=nullptr) noexcept {
    if(!raw||!count||!layout.streamStride||layout.count>layout.attributes.size()||
       count>bytes/layout.streamStride||!pipeline.fixedVertexOnGpu)return lookup_fallback();
    if(sourceIndexCount&&(!sourceIndices||primitive!=SourcePrimitive::Triangles||
                         pipeline.fixedPointSprite||pipeline.fixedLineSprite))return lookup_fallback();
    bytes=static_cast<size_t>(count)*layout.streamStride;
    const VertexLayout gpuLayout=recipe?recipe->gpuLayout:fixed_vertex_gpu_layout(pipeline);
    const VertexSemanticMask used=recipe?recipe->decodeSemantics:fixed_vertex_gpu_inputs(pipeline);
    uint64_t key=0;
    auto* detailed=telemetry&&telemetry->split_vertex_phases()?telemetry:nullptr;
    { ScopedTelemetryPhase phase(detailed,TelemetryPhase::GeometryKey);
      key=stableSource?make_stable_key(stableSource,count,primitive,layout,gpuLayout,used,sourceIndices,sourceIndexCount):
                       make_key(raw,bytes,count,primitive,layout,gpuLayout,used,sourceIndices,sourceIndexCount);
    }
    auto it=entries_.find(key);
    if(it!=entries_.end()) {
      ScopedTelemetryPhase phase(detailed,TelemetryPhase::GeometryValidate);
      auto& e=*it->second;
      if(e.volatileSource)return lookup_fallback();
      // Hashes select a candidate only. Every byte that could influence a
      // decoded attribute is checked before an immutable GPU buffer is reused.
      if(!same_layout(layout,e.sourceLayout)||!same_gpu_layout(gpuLayout,e.gpuLayout))return lookup_fallback();
      if(e.sourceIndices.size()!=sourceIndexCount||
         (sourceIndexCount&&!byte_spans_equal(sourceIndices,e.sourceIndices.data(),
                                              size_t(sourceIndexCount)*sizeof(uint16_t))))return lookup_fallback();
      if(stableSource) {
        if(e.stableSource!=stableSource||e.stableSourceBytes!=bytes) {
          e.volatileSource=true;
          return lookup_fallback();
        }
        const uint64_t epoch=memory_write_epoch();
        if(epoch!=e.validationEpoch) {
          std::array<MemoryRevisionCheck,MaxVertexAttributes+1> checks{};
          size_t checkCount=0;
          checks[checkCount++]={stableSource,bytes,e.stableSourceRevision};
          for(const auto& s:e.snapshots) {
            if(s.revisionTracked) {
              if(checkCount>=checks.size()){e.volatileSource=true;return lookup_fallback();}
              checks[checkCount++]={s.source,s.size,s.revision};
            } else if(!byte_spans_equal(s.source,s.bytes.data(),s.bytes.size())) {
              e.volatileSource=true;
              return lookup_fallback();
            }
          }
          if(!memory_ranges_match(checks.data(),checkCount)) {
            e.volatileSource=true;
            return lookup_fallback();
          }
          e.validationEpoch=epoch;
        }
      } else {
        if(e.raw.size()!=bytes||!byte_spans_equal(raw,e.raw.data(),bytes))return lookup_fallback();
        for(const auto& s:e.snapshots) {
          const bool changed=s.revisionTracked?
            memory_range_revision(s.source,s.size)!=s.revision:
            !byte_spans_equal(s.source,s.bytes.data(),s.bytes.size());
          if(changed) {
            e.volatileSource=true;
            return lookup_fallback();
          }
        }
      }
      ++hits_;
      e.lastUseFrame=frame_;
      return &e;
    }
    ++misses_;
    const bool evict=!gxm_disabled(GxmDisableGeometryEviction);
    if(evict&&retiredBytes_==0&&(entries_.size()>=1024||this->bytes()>=budget_))evict_for(budget_/8u);
    if(entries_.size()>=1024||this->bytes()>=budget_)return lookup_fallback();
    // Retired GPU storage cannot make room until begin_frame's idle barrier.
    // Reject impossible admissions before scanning source arrays or decoding
    // vertices. While eviction is available, preserve the existing exact-size
    // admission/eviction path instead of changing which entries it retires.
    if(budgetPreflight_&&(retiredBytes_!=0||!evict)) {
      const auto footprintPrimitive=pipeline.fixedPointSprite?SourcePrimitive::Points:
        (pipeline.fixedLineSprite&&primitive!=SourcePrimitive::Lines?SourcePrimitive::LineStrip:primitive);
      const auto footprint=estimate_draw_footprint(footprintPrimitive,count,sourceIndexCount,
                                                  gpuLayout.attributes[0].stride);
      if(footprint.valid) {
        // Triangle deduplication can shrink vertices to one record but never
        // removes indices. A worst-case vertex estimate would reject meshes
        // whose actual deduplicated representation still fits the budget.
        const bool mayDeduplicate=!pipeline.fixedPointSprite&&!pipeline.fixedLineSprite&&
          primitive==SourcePrimitive::Triangles&&sourceIndexCount==0&&count>=48u;
        const uint64_t minimumVertexBytes=mayDeduplicate?gpuLayout.attributes[0].stride:footprint.vertexBytes;
        const uint64_t minimumGpuBytes=minimumVertexBytes+footprint.indexBytes;
        if(minimumGpuBytes>budget_-this->bytes()) {
          ++budgetPreflightRejects_;
          return lookup_fallback();
        }
      }
    }
    auto entry=std::make_unique<Entry>();
    entry->replayKey=key;
    entry->sourceLayout=layout;entry->gpuLayout=gpuLayout;
    entry->stableSource=stableSource;entry->stableSourceBytes=stableSource?bytes:0;
    const uint64_t validationEpochBefore=stableSource?memory_write_epoch():0;
    entry->stableSourceRevision=stableSource?memory_range_revision(stableSource,bytes):0;
    if(!snapshot_sources(raw,bytes,count,layout,used,entry->snapshots,stableSource!=nullptr))return lookup_fallback();
    if(stableSource) {
      const uint64_t validationEpochAfter=memory_write_epoch();
      entry->validationEpoch=validationEpochBefore==validationEpochAfter?validationEpochAfter:0;
    }
    size_t storedBytes=stableSource?0:bytes;
    for(const auto& s:entry->snapshots)storedBytes+=s.revisionTracked?0:s.bytes.size();
    if(sourceIndexCount){
      entry->sourceIndices.assign(sourceIndices,sourceIndices+sourceIndexCount);
      storedBytes+=size_t(sourceIndexCount)*sizeof(uint16_t);
    }
    size_t committedBefore=this->bytes();
    if(committedBefore>budget_||storedBytes>budget_-committedBefore) {
      if(evict&&retiredBytes_==0)evict_for(storedBytes);
      committedBefore=this->bytes();
      if(committedBefore>budget_||storedBytes>budget_-committedBefore)return lookup_fallback();
    }
    NativeGpuGeometry direct{};
    const bool directPacked=nativeGpuStatic_&&stableSource&&!sourceIndexCount&&
        !pipeline.fixedPointSprite&&!pipeline.fixedLineSprite&&
        load_native_gpu_geometry(raw,bytes,count,primitive,layout,gpuLayout,direct);
    entry->nativeGpuPacked=directPacked;
    if(directPacked) {
      // AVNR v2 stores final GXM attribute bytes + ordered indices. No
      // CanonicalVertex staging, vertex decode or per-layout GPU repack.
      // Matrix, lighting, material and draw packet generation remain live.
    } else if(pipeline.fixedPointSprite||pipeline.fixedLineSprite) {
      scratch_.error=PrepareDrawError::None;
      scratch_.vertices.clear();scratch_.indices.clear();scratch_.scratch.clear();
      scratch_.scratch.resize(count);
      for(uint32_t i=0;i<count;++i)
        if(!decode_vertex_into(raw,bytes,i,layout,scratch_.scratch[i],used))return lookup_fallback();
      if(pipeline.fixedPointSprite) {
        if(count>std::numeric_limits<uint16_t>::max()/4u)return lookup_fallback();
        scratch_.vertices.reserve(size_t(count)*4u);
        scratch_.indices.reserve(size_t(count)*6u);
        for(uint32_t i=0;i<count;++i){
          const uint32_t base=static_cast<uint32_t>(scratch_.vertices.size());
          for(uint32_t corner=0;corner<4;++corner){
            auto v=scratch_.scratch[i];v.position[3]=static_cast<float>(corner);
            scratch_.vertices.push_back(v);
          }
          const uint16_t b=static_cast<uint16_t>(base);
          scratch_.indices.insert(scratch_.indices.end(),{b,uint16_t(b+1),uint16_t(b+3),
                                                          uint16_t(b+3),uint16_t(b+2),b});
        }
      } else {
        const uint32_t segments=primitive==SourcePrimitive::Lines?count/2u:(count>1u?count-1u:0u);
        if((primitive==SourcePrimitive::Lines&&(count&1u))||
           segments>std::numeric_limits<uint16_t>::max()/4u)return lookup_fallback();
        scratch_.vertices.reserve(size_t(segments)*4u);
        scratch_.indices.reserve(size_t(segments)*6u);
        for(uint32_t s=0;s<segments;++s){
          const uint32_t ai=primitive==SourcePrimitive::Lines?s*2u:s;
          const uint32_t bi=ai+1u;
          const auto a=scratch_.scratch[ai],b=scratch_.scratch[bi];
          const uint32_t base=static_cast<uint32_t>(scratch_.vertices.size());
          for(uint32_t corner=0;corner<4;++corner){
            const bool useB=corner>=2u;
            auto v=useB?b:a;const auto& other=useB?a:b;
            v.normal[0]=other.position[0];v.normal[1]=other.position[1];v.normal[2]=other.position[2];
            v.position[3]=static_cast<float>(corner);
            scratch_.vertices.push_back(v);
          }
          const uint16_t bs=static_cast<uint16_t>(base);
          scratch_.indices.insert(scratch_.indices.end(),{bs,uint16_t(bs+1),uint16_t(bs+3),
                                                          uint16_t(bs+3),uint16_t(bs+2),bs});
        }
      }
      scratch_.primitive=Primitive::Triangles;scratch_.positionIsClipSpace=false;
    } else {
      const bool precompiled=stableSource&&!sourceIndexCount&&
          load_native_geometry(raw,bytes,count,primitive,layout,scratch_);
      if(!precompiled&&!prepare_draw_into(scratch_,raw,bytes,count,primitive,layout,pipeline,state,nullptr,{},telemetry,
                            sourceIndexCount==0))return lookup_fallback();
      if(sourceIndexCount){
        for(uint32_t i=0;i<sourceIndexCount;++i)if(sourceIndices[i]>=scratch_.vertices.size())return lookup_fallback();
        scratch_.indices.assign(sourceIndices,sourceIndices+sourceIndexCount);
      }
    }
    if(!directPacked&&(scratch_.vertices.empty()||scratch_.indices.empty()))return lookup_fallback();
    const size_t stride=gpuLayout.attributes[0].stride;
    const size_t vertexBytes=directPacked?direct.vertices.size():scratch_.vertices.size()*stride;
    const size_t indexBytes=directPacked?direct.indices.size()*sizeof(uint16_t):scratch_.indices.size()*sizeof(uint16_t);
    size_t committed=this->bytes();
    if(committed>budget_||storedBytes>budget_-committed||
       vertexBytes+indexBytes>budget_-committed-storedBytes) {
      if(!gxm_disabled(GxmDisableGeometryEviction)&&retiredBytes_==0)
        evict_for(storedBytes+vertexBytes+indexBytes);
      committed=this->bytes();
      if(committed>budget_||storedBytes>budget_-committed||
         vertexBytes+indexBytes>budget_-committed-storedBytes)return lookup_fallback();
    }
    const uint8_t* vertexData=directPacked?direct.vertices.data():nullptr;
    if(!directPacked) {
      packed_.resize(vertexBytes);
      PackContext pack{packed_.data(),scratch_.vertices.data(),&gpuLayout,stride};
      if(!cpu_parallel_for_vertex(scratch_.vertices.size(),pack_range,&pack))return lookup_fallback();
      vertexData=packed_.data();
    }
    const Handle vb=renderer_.create_vertex_buffer(vertexData,vertexBytes,false);
    if(!vb)return lookup_fallback();
    const Handle ib=renderer_.create_index_buffer(directPacked?direct.indices.data():scratch_.indices.data(),indexBytes,false);
    if(!ib) {renderer_.buffers().destroy(vb);return lookup_fallback();}
    entry->vertices={vb,0,static_cast<uint32_t>(vertexBytes)};
    entry->indices={ib,0,static_cast<uint32_t>(indexBytes)};
    entry->vertexCount=directPacked?direct.vertexCount:static_cast<uint32_t>(scratch_.vertices.size());
    entry->indexCount=directPacked?static_cast<uint32_t>(direct.indices.size()):static_cast<uint32_t>(scratch_.indices.size());
    if(!stableSource)entry->raw.assign(raw,raw+bytes);
    bytes_+=storedBytes+vertexBytes+indexBytes;
    entry->accountedBytes=storedBytes+vertexBytes+indexBytes;
    entry->lastUseFrame=frame_;
    auto* result=entry.get();
    entries_.emplace(key,std::move(entry));
    return result;
  }

  void clear() noexcept {
    if(!retired_.empty())renderer_.buffers().wait_idle();
    for(const auto& r:retired_) {
      renderer_.buffers().destroy_retired(r.vertices);
      renderer_.buffers().destroy_retired(r.indices);
    }
    retired_.clear();
    for(auto& pair:entries_) {
      renderer_.buffers().destroy(pair.second->vertices.buffer);
      renderer_.buffers().destroy(pair.second->indices.buffer);
    }
    entries_.clear();bytes_=retiredBytes_=0;hits_=misses_=lookupFallbacks_=budgetPreflightRejects_=0;
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
      Snapshot s{};s.source=a.array.data+start;
      s.size=end-start;s.revisionTracked=revisionTracked;
      if(revisionTracked)s.revision=memory_range_revision(s.source,s.size);
      else s.bytes.assign(s.source,s.source+s.size);
      output.push_back(std::move(s));
    }
    return true;
  }

private:
  struct PackContext {
    uint8_t* destination=nullptr;
    const CanonicalVertex* vertices=nullptr;
    const VertexLayout* layout=nullptr;
    size_t stride=0;
  };
  static bool pack_range(void* opaque,size_t begin,size_t end,uint32_t) noexcept {
    auto& ctx=*static_cast<PackContext*>(opaque);
    if(!ctx.destination||!ctx.vertices||!ctx.layout||!ctx.stride)return false;
    for(size_t i=begin;i<end;++i)
      pack_gpu_vertex_bytes(ctx.destination+i*ctx.stride,ctx.vertices[i],*ctx.layout);
    return true;
  }
  Entry* lookup_fallback() noexcept {
    ++lookupFallbacks_;
    return nullptr;
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
                           const VertexDecodeLayout& layout,const VertexLayout& gpu,
                           VertexSemanticMask used,const uint16_t* indices,uint32_t indexCount) noexcept {
    uint32_t h=2166136261u;
    const auto add=[&](uint64_t x){h=(h^static_cast<uint32_t>(x))*16777619u;h=(h^static_cast<uint32_t>(x>>32))*16777619u;};
    add(count);add(static_cast<uint8_t>(primitive));add(used);add(layout.count);add(layout.streamStride);add(layout.streamLittleEndian);
    for(unsigned i=0;i<layout.count;++i) {
      const auto& a=layout.attributes[i];
      add(static_cast<uint8_t>(a.semantic));add(static_cast<uint8_t>(a.source));add(static_cast<uint8_t>(a.component));
      add(a.components);add(a.frac);add(a.streamOffset);add(a.valueOffset);
      add(reinterpret_cast<uintptr_t>(a.array.data));add(a.array.size);add(a.array.stride);add(a.array.littleEndian);
    }
    add(gpu.count);
    for(unsigned i=0;i<gpu.count;++i) {const auto& a=gpu.attributes[i];add(a.location);add(a.components);add(static_cast<uint8_t>(a.scalar));add(a.normalized);add(a.offset);add(a.stride);}
    uint64_t content=hash_bytes(raw,bytes);
    if(indices&&indexCount)content=XXH3_64bits_withSeed(indices,size_t(indexCount)*sizeof(uint16_t),content);
    const uint64_t metadata=(static_cast<uint64_t>(h)<<32)|h;
    return content ^ metadata;
  }
  static uint64_t make_stable_key(const uint8_t* stable,uint32_t count,SourcePrimitive primitive,
                                  const VertexDecodeLayout& layout,const VertexLayout& gpu,
                                  VertexSemanticMask used,const uint16_t* indices,uint32_t indexCount) noexcept {
    uint32_t h=2166136261u;
    const auto add=[&](uint64_t x){h=(h^static_cast<uint32_t>(x))*16777619u;h=(h^static_cast<uint32_t>(x>>32))*16777619u;};
    add(reinterpret_cast<uintptr_t>(stable));add(count);add(static_cast<uint8_t>(primitive));add(used);
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
    uint64_t key=XXH3_64bits_withSeed(&address,sizeof(address),metadata);
    if(indices&&indexCount)key=XXH3_64bits_withSeed(indices,size_t(indexCount)*sizeof(uint16_t),key);
    return key;
  }
  Renderer& renderer_;
  size_t budget_=0,bytes_=0,retiredBytes_=0;
  bool budgetPreflight_=false;
  bool nativeGpuStatic_=false;
  uint64_t budgetPreflightRejects_=0;
  uint64_t hits_=0,misses_=0,lookupFallbacks_=0;
  FlatHashMap<uint64_t,std::unique_ptr<Entry>> entries_{};
  struct Retired {
    Handle vertices=InvalidHandle,indices=InvalidHandle;
    uint64_t frame=0;
    size_t gpuBytes=0;
  };
  std::vector<Retired> retired_{};
  uint64_t frame_=0,evictions_=0;

  // Free at least `required` bytes (and one entry slot) by evicting the least
  // recently used entries. Entries used in the current frame may still be
  // referenced by queued draw packets and are never evicted.
  void evict_for(size_t required) noexcept {
    std::vector<std::pair<uint64_t,uint64_t>> order;
    order.reserve(entries_.size());
    for(const auto& [key,e]:entries_)
      if(e->lastUseFrame<frame_)order.emplace_back(e->volatileSource?0:e->lastUseFrame,key);
    std::sort(order.begin(),order.end());
    size_t freed=0;
    for(const auto& [_,key]:order) {
      if(freed>=required&&bytes_<budget_&&entries_.size()<1024)break;
      const auto it=entries_.find(key);
      if(it==entries_.end())continue;
      auto& e=*it->second;
      const size_t gpuBytes=size_t(e.vertices.size)+size_t(e.indices.size);
      retired_.push_back(Retired{e.vertices.buffer,e.indices.buffer,frame_,gpuBytes});
      retiredBytes_+=gpuBytes;
      freed+=e.accountedBytes;
      bytes_=e.accountedBytes<=bytes_?bytes_-e.accountedBytes:0;
      entries_.erase(it);
      ++evictions_;
    }
  }
  PreparedDraw scratch_{};
  std::vector<uint8_t> packed_{};
};

} // namespace aurora::vita::gfx
