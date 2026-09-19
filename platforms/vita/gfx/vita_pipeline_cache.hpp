#pragma once
#include "vita_gfx_types.hpp"
#include "vita_hash_map.hpp"
#include "vita_native_fwd.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace aurora::vita::gfx {
struct CompiledPipeline {
  uint64_t key=0;
  uint64_t lastUsed=0;
  PipelineDesc desc{};
  unsigned program=0;
  int uMvp=-1,uKColor=-1,uTevReg=-1,uFogColor=-1,uFogParams=-1,uFogRangeK=-1,uRenderViewportWidth=-1,uIndMtx=-1,uTexcoordScale=-1,uTextureSizeBias=-1;
  std::array<int,MaxTextures> uTex{};
  mutable GpuDrawUniforms cachedUniforms{};
  mutable uint16_t uniformValidMask=0;
  int uGxPosition=-1,uGxMaterial=-1;
  std::array<int,MaxTextures> uGxTexture{},uGxPost{};
  mutable FixedVertexUniforms cachedFixedVertex{};
  mutable bool fixedVertexValid=false;
};

// Only the subset that maps to vitaGL fixed-function state. Keeping this small
// avoids copying the 840-byte PipelineDesc every time a TEV program changes.
struct FixedStateSnapshot {
  Compare depthFunc=Compare::Always;
  CullMode cull=CullMode::None;
  BlendMode blendMode=BlendMode::None;
  BlendFactor srcFactor=BlendFactor::One,dstFactor=BlendFactor::Zero;
  LogicOp logicOp=LogicOp::Copy;
  int16_t dstAlpha=-1;
  bool depthTest=false,depthWrite=false,colorWrite=true,alphaWrite=true,polygonOffset=false;
  float polygonOffsetFactor=0.f,polygonOffsetUnits=0.f;
};

class PipelineCache {
public:
  explicit PipelineCache(size_t maxEntries=512) noexcept : maxEntries_(maxEntries) {}
  ~PipelineCache();
  const CompiledPipeline* get_or_create(const PipelineDesc& desc,FrameStats* stats=nullptr) noexcept;
  const CompiledPipeline* find(uint64_t key) noexcept;
  void bind(const CompiledPipeline& p,const GpuDrawUniforms& u,FrameStats* stats=nullptr,
            const FixedVertexUniforms* fixedVertex=nullptr) noexcept;
  void clear() noexcept;
  void invalidate_bound() noexcept{bound_=0;boundPipeline_=nullptr;fixedStateValid_=false;}
  void set_max_entries(size_t maxEntries) noexcept;
  void configure_hot_manifest(const char* path,size_t prewarmLimit=192) noexcept;
  size_t prewarm_hot(FrameStats* stats=nullptr) noexcept;
  void save_hot_manifest() noexcept;
  void pin(uint64_t key) noexcept { if(key) pinned_.insert(key); }
  void clear_pins() noexcept { pinned_.clear(); }
  void trim_to_budget() noexcept;
  size_t pinned_entries() const noexcept { return pinned_.size(); }
  size_t max_entries() const noexcept{return maxEntries_;}
  size_t size()const noexcept{return map_.size();}
  size_t high_water_entries() const noexcept{return highWaterEntries_;}
  uint64_t compile_failures() const noexcept{return compileFailures_;}
  uint64_t evictions() const noexcept{return evictions_;}
private:
  friend class Renderer;
#if defined(AURORA_VITA_RENDERER_GXM)
  gxm::Renderer* native_=nullptr;
#endif
  bool evict_one() noexcept;
  void destroy_pipeline(CompiledPipeline& pipeline) noexcept;
  struct HotRecord { PipelineDesc desc{}; uint64_t hits=0; };
  NodeHashMap<uint64_t,CompiledPipeline> map_;
  NodeHashMap<uint64_t,HotRecord> hot_;
  FlatHashSet<uint64_t> pinned_{};
  // Shader failures are deterministic for a pipeline description. Retrying the
  // same broken TEV program every draw causes severe stalls and repeated compiler
  // pressure on Vita, so suppress it until the cache is explicitly cleared.
  FlatHashSet<uint64_t> failedKeys_{};
  uint64_t bound_=0;
  CompiledPipeline* boundPipeline_=nullptr;
  FixedStateSnapshot fixedState_{};
  bool fixedStateValid_=false;
  uint64_t useSequence_=0;
  size_t maxEntries_=512;
  size_t highWaterEntries_=0;
  uint64_t compileFailures_=0;
  uint64_t evictions_=0;
  std::string hotManifestPath_{};
  size_t prewarmLimit_=192;
  bool hotDirty_=false;
};
} // namespace aurora::vita::gfx
