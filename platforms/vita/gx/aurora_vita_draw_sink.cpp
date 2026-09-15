#include "aurora_vita_draw_sink.hpp"
#include "../gfx/vita_renderer.hpp"
#include "../gfx/vita_pipeline_key.hpp"

#if defined(AURORA_VITA_UPSTREAM)
#if defined(AURORA_VITA_UPSTREAM_STUB)
#include "../../../tests/upstream_gx_stub.hpp"
#else
#include "../../../lib/gx/gx.hpp"
#endif
#endif
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace aurora::vita::gxbridge {

gfx::MemoryBudgetSnapshot DrawSink::memory_budget() const noexcept {
  if (!renderer_ || !arena_) return {};
  return gfx::capture_memory_budget(*arena_, renderer_->textures(), renderer_->pipelines(), &renderer_->efb());
}

DrawSink::~DrawSink() { shutdown(); }

bool DrawSink::initialize(gfx::Renderer& renderer, const DrawSinkConfig& config) noexcept {
  if (initialized_) return true;
  renderer_ = &renderer;
  arena_ = std::make_unique<gfx::StreamingArena>(renderer.buffers(), config.streaming);
  if (!arena_->initialize()) {
    arena_.reset();
    renderer_ = nullptr;
    return false;
  }
  stream_.reserve(config.commandReserve);
  telemetry_ = config.telemetry;
  coverage_ = config.coverage;
  trace_ = config.trace;
  strictUnsupported_ = config.strictUnsupported;
  strictFailed_ = false;
  whiteTexture_ = white_texture();
  if (!whiteTexture_) {
    arena_->shutdown();
    arena_.reset();
    renderer_ = nullptr;
    return false;
  }
  initialized_ = true;
  return true;
}

void DrawSink::shutdown() noexcept {
  if (!initialized_ && !arena_) return;
  stream_.reset();
#if defined(AURORA_VITA_UPSTREAM)
  clear_copy_textures();
#endif
  if (arena_) {
    arena_->shutdown();
    arena_.reset();
  }
  renderer_ = nullptr;
  whiteTexture_ = gfx::InvalidHandle;
#if defined(AURORA_VITA_UPSTREAM)
  queuedTranslatedPipelineKey_ = 0;
  queuedResolvedPipelineKey_ = 0;
#endif
  reset_pipeline_run_cache();
  submittedDraws_ = 0;
  telemetry_ = nullptr;
  coverage_ = nullptr;
  trace_ = nullptr;
  strictUnsupported_ = false;
  strictFailed_ = false;
  initialized_ = false;
}

void DrawSink::begin_frame(uint64_t frame) noexcept {
  if (!initialized_ || !arena_) return;
  stream_.reset();
  reset_pipeline_run_cache();
  arena_->begin_frame(frame);
  if (trace_) trace_->begin_frame(frame);
}

void DrawSink::flush() noexcept {
  if (!initialized_ || !renderer_ || stream_.size() == 0) return;
  if (!arena_ || !arena_->flush()) {
    if (telemetry_) telemetry_->arena_overflow();
    if (strictUnsupported_) strictFailed_ = true;
    stream_.reset();
    reset_pipeline_run_cache();
    return;
  }
  { gfx::ScopedTelemetryPhase phase(telemetry_, gfx::TelemetryPhase::Submit); renderer_->execute(stream_); }
  stream_.reset();
  reset_pipeline_run_cache();
}

gfx::Handle DrawSink::white_texture() noexcept {
  if (whiteTexture_) return whiteTexture_;
  if (!renderer_) return gfx::InvalidHandle;
  static constexpr std::array<uint8_t, 4> kWhite{{255,255,255,255}};
  gfx::TextureDesc d{};
  d.width = 1; d.height = 1; d.format = gfx::TextureFormat::RGBA8888;
  d.data = kWhite.data(); d.dataSize = kWhite.size(); d.sourceId = 0x4156525657484954ull; // "AVRVWHIT"
  d.cacheable = true;
  whiteTexture_ = renderer_->create_texture(d);
  return whiteTexture_;
}

#if defined(AURORA_VITA_UPSTREAM)
namespace {
bool color_uses_texture(gfx::TevColorArg a) noexcept { return a == gfx::TevColorArg::TexColor || a == gfx::TevColorArg::TexAlpha; }
bool alpha_uses_texture(gfx::TevAlphaArg a) noexcept { return a == gfx::TevAlphaArg::TexAlpha; }
bool stage_uses_texture(const gfx::TevStage& s) noexcept {
  return color_uses_texture(s.color.a) || color_uses_texture(s.color.b) || color_uses_texture(s.color.c) ||
         color_uses_texture(s.color.d) || alpha_uses_texture(s.alpha.a) || alpha_uses_texture(s.alpha.b) ||
         alpha_uses_texture(s.alpha.c) || alpha_uses_texture(s.alpha.d);
}

uint8_t sampled_texture_mask(const gfx::PipelineDesc& p) noexcept {
  uint8_t mask = 0;
  for (unsigned i = 0; i < p.tev.stageCount && i < p.tev.stages.size(); ++i) {
    const auto& s = p.tev.stages[i];
    if (stage_uses_texture(s) && s.texture < gfx::MaxTextures) mask |= static_cast<uint8_t>(1u << s.texture);
    if (s.indirectEnabled && s.indirectStage < p.tev.indirectStageCount) {
      const auto t = p.tev.indirectStages[s.indirectStage].texture;
      if (t < gfx::MaxTextures) mask |= static_cast<uint8_t>(1u << t);
    }
  }
  return mask;
}

#if defined(__vita__)
struct ClipPoint { float x=0.f,y=0.f,z=0.f,w=1.f; };

ClipPoint project_for_diag(const std::array<float,16>& m,const gfx::CanonicalVertex& v,bool alreadyClip) noexcept {
  if(alreadyClip)return {v.position[0],v.position[1],v.position[2],v.position[3]};
  const float x=v.position[0],y=v.position[1],z=v.position[2],w=v.position[3];
  return {m[0]*x+m[4]*y+m[8]*z+m[12]*w,
          m[1]*x+m[5]*y+m[9]*z+m[13]*w,
          m[2]*x+m[6]*y+m[10]*z+m[14]*w,
          m[3]*x+m[7]*y+m[11]*z+m[15]*w};
}

void log_large_draw_geometry(const gfx::PreparedDraw& prepared,const gfx::VertexTransformState& state,
                             const gfx::PipelineDesc& pipeline,uint32_t inputVertices) noexcept {
  // Runtime.log is intentionally sampled: enough to diagnose the first stadium /
  // character meshes without turning logging itself into the new performance issue.
  static uint32_t logged=0;
  if(inputVertices<512||logged>=16||prepared.vertices.empty())return;
  ++logged;

  constexpr float inf=std::numeric_limits<float>::infinity();
  float pmin[4]{inf,inf,inf,inf},pmax[4]{-inf,-inf,-inf,-inf};
  float cmin[4]{inf,inf,inf,inf},cmax[4]{-inf,-inf,-inf,-inf};
  uint32_t finite=0,wPositive=0,insideXY=0,insideGL=0,insideZ01=0;
  for(const auto& v:prepared.vertices){
    const auto c=project_for_diag(state.projection,v,prepared.positionIsClipSpace);
    const float pv[4]{v.position[0],v.position[1],v.position[2],v.position[3]};
    const float cv[4]{c.x,c.y,c.z,c.w};
    bool allFinite=true;
    for(unsigned i=0;i<4;i++){
      allFinite=allFinite&&std::isfinite(pv[i])&&std::isfinite(cv[i]);
      if(std::isfinite(pv[i])){pmin[i]=std::min(pmin[i],pv[i]);pmax[i]=std::max(pmax[i],pv[i]);}
      if(std::isfinite(cv[i])){cmin[i]=std::min(cmin[i],cv[i]);cmax[i]=std::max(cmax[i],cv[i]);}
    }
    if(!allFinite)continue;
    ++finite;
    if(c.w>0.f){
      ++wPositive;
      const bool xy=c.x>=-c.w&&c.x<=c.w&&c.y>=-c.w&&c.y<=c.w;
      if(xy)++insideXY;
      if(xy&&c.z>=-c.w&&c.z<=c.w)++insideGL;
      if(xy&&c.z>=0.f&&c.z<=c.w)++insideZ01;
    }
  }
  std::fprintf(stderr,
    "[aurora-vita][3d-geom] in=%u out=%u idx=%u clip=%u finite=%u wpos=%u xy=%u glz=%u z01=%u "
    "pos=[%g,%g,%g,%g..%g,%g,%g,%g] clipbox=[%g,%g,%g,%g..%g,%g,%g,%g] "
    "depth=%u/%u cull=%u revz=%u warn_zbefore=%u\n",
    inputVertices,static_cast<unsigned>(prepared.vertices.size()),static_cast<unsigned>(prepared.indices.size()),
    prepared.positionIsClipSpace?1u:0u,finite,wPositive,insideXY,insideGL,insideZ01,
    pmin[0],pmin[1],pmin[2],pmin[3],pmax[0],pmax[1],pmax[2],pmax[3],
    cmin[0],cmin[1],cmin[2],cmin[3],cmax[0],cmax[1],cmax[2],cmax[3],
    pipeline.depthTest?1u:0u,static_cast<unsigned>(pipeline.depthFunc),static_cast<unsigned>(pipeline.cull),
    pipeline.reversedZ?1u:0u,aurora::gx::g_gxState.zCompLocBeforeTex?1u:0u);

  std::fprintf(stderr,"[aurora-vita][3d-proj] %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g\n",
    state.projection[0],state.projection[1],state.projection[2],state.projection[3],
    state.projection[4],state.projection[5],state.projection[6],state.projection[7],
    state.projection[8],state.projection[9],state.projection[10],state.projection[11],
    state.projection[12],state.projection[13],state.projection[14],state.projection[15]);

  const unsigned samples=std::min<unsigned>(3,static_cast<unsigned>(prepared.vertices.size()));
  for(unsigned i=0;i<samples;i++){
    const auto& v=prepared.vertices[i];const auto c=project_for_diag(state.projection,v,prepared.positionIsClipSpace);
    std::fprintf(stderr,"[aurora-vita][3d-v] n=%u p=%g,%g,%g,%g c=%g,%g,%g,%g pn=%u\n",
      i,v.position[0],v.position[1],v.position[2],v.position[3],c.x,c.y,c.z,c.w,static_cast<unsigned>(v.pnMatrixIndex));
  }
  std::fprintf(stderr,"[aurora-vita][3d-i] %u %u %u %u %u %u\n",
    prepared.indices.size()>0?prepared.indices[0]:0u,prepared.indices.size()>1?prepared.indices[1]:0u,
    prepared.indices.size()>2?prepared.indices[2]:0u,prepared.indices.size()>3?prepared.indices[3]:0u,
    prepared.indices.size()>4?prepared.indices[4]:0u,prepared.indices.size()>5?prepared.indices[5]:0u);
}
#endif
}

bool DrawSink::copy_tex(const void* dest, bool clear) noexcept {
  if (!initialized_ || !renderer_ || !dest) return false;
  gfx::ScopedTelemetryPhase timer(telemetry_, gfx::TelemetryPhase::EfbCopy);
  if (coverage_) coverage_->observe(integration::FeatureClass::EfbCopy, reinterpret_cast<uintptr_t>(dest), "GXCopyTex");
  // GXCopyTex is an ordering boundary: all draws before it must hit the source EFB.
  flush();
  const auto& g = aurora::gx::g_gxState;
  gfx::Scissor src{};
#if defined(AURORA_VITA_UPSTREAM_STUB)
  // The stub does not expose Aurora's texCopySrc; cover the whole logical EFB.
  src = gfx::Scissor{0, 0, static_cast<int32_t>(std::max(g.renderViewport.width,1.f)),
                   static_cast<int32_t>(std::max(g.renderViewport.height,1.f))};
  const uint32_t dstW = static_cast<uint32_t>(std::max(g.renderViewport.width,1.f));
  const uint32_t dstH = static_cast<uint32_t>(std::max(g.renderViewport.height,1.f));
#else
  // Match Aurora's GXCopyTex mapping: map source scissor through the viewport policy,
  // and scale the destination against the logical framebuffer rather than the current viewport.
  const auto mapped = aurora::gx::map_logical_scissor(g.texCopySrc);
  src = gfx::Scissor{mapped.x,mapped.y,mapped.width,mapped.height};
  const auto logicalFb = aurora::gx::logical_fb_size();
  const float sx = logicalFb.x ? static_cast<float>(renderer_->target_width()) / static_cast<float>(logicalFb.x) : 1.f;
  const float sy = logicalFb.y ? static_cast<float>(renderer_->target_height()) / static_cast<float>(logicalFb.y) : 1.f;
  const uint32_t dstW = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(static_cast<float>(g.texCopyDstWidth) * sx)));
  const uint32_t dstH = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(static_cast<float>(g.texCopyDstHeight) * sy)));
#endif
#if defined(AURORA_VITA_UPSTREAM_STUB)
  const uint32_t rawCopyFormat = 0;
  const gfx::EfbCopyFormat copyFormat = gfx::EfbCopyFormat::Passthrough;
#else
  const uint32_t rawCopyFormat = static_cast<uint32_t>(g.texCopyFmt);
  const gfx::EfbCopyFormat copyFormat = gfx::efb_copy_format_from_gx_raw(rawCopyFormat);
#endif
  if (!gfx::is_supported_color_copy_format(copyFormat)) {
    const char* reason = gfx::is_depth_copy_format(copyFormat)
        ? "GXCopyTex depth conversion unsupported on vitaGL EFB path"
        : "GXCopyTex color format unsupported on Vita backend";
    if (coverage_) coverage_->fallback(rawCopyFormat, reason);
    if (telemetry_) telemetry_->unsupported();
    if (strictUnsupported_) strictFailed_ = true;
    return false;
  }
  const uintptr_t copyKey = reinterpret_cast<uintptr_t>(dest);
  const auto oldIt = copyTextures_.find(copyKey);
  const gfx::Handle oldHandle = oldIt == copyTextures_.end() ? gfx::InvalidHandle : oldIt->second.handle;
  const uint32_t oldRevision = oldIt == copyTextures_.end() ? 0 : oldIt->second.revision;
  const auto h = renderer_->capture_current(oldHandle, src, dstW, dstH, copyFormat);
  if (!h) {
    // capture_current may destroy an incompatible old target before allocation/copy; never retain
    // a potentially stale handle in the guest-destination map after failure.
    if (oldIt != copyTextures_.end()) copyTextures_.erase(oldIt);
    if (coverage_) coverage_->fallback(copyKey, "GXCopyTex capture failed");
    if (telemetry_) telemetry_->unsupported();
    if (strictUnsupported_) strictFailed_ = true;
    return false;
  }
  if (telemetry_) telemetry_->efb_copy();
  copyTextures_[copyKey] = CopyTextureEntry{h, dstW, dstH, oldRevision + 1};
  if (clear) {
#if defined(AURORA_VITA_UPSTREAM_STUB)
    renderer_->clear_current({0,0,0,0}, 0.f, g.colorUpdate, g.alphaUpdate, g.depthUpdate);
#else
    const gfx::Color cc{g.clearColor[0],g.clearColor[1],g.clearColor[2],g.clearColor[3]};
    renderer_->clear_current(cc, aurora::gx::clear_depth_value(), g.colorUpdate, g.alphaUpdate, g.depthUpdate);
#endif
  }
  return true;
}

void DrawSink::evict_copy_tex(const void* dest) noexcept {
  if (!dest || !renderer_) return;
  const auto it=copyTextures_.find(reinterpret_cast<uintptr_t>(dest));
  if(it==copyTextures_.end())return;
  if(it->second.handle)renderer_->efb().destroy(it->second.handle);
  copyTextures_.erase(it);
}

void DrawSink::clear_copy_textures() noexcept {
  if(renderer_)for(auto& [_,e]:copyTextures_)if(e.handle)renderer_->efb().destroy(e.handle);
  copyTextures_.clear();
}

SubmitResult DrawSink::submit(uint8_t primitive, uint8_t fmt, const uint8_t* rawVertices,
                              size_t rawBytes, uint32_t vertexCount,
                              const uint16_t* rawIndices, uint32_t indexCount) noexcept {
  SubmitResult result{};
  if (!initialized_ || !renderer_ || !arena_ || !rawVertices || vertexCount == 0) {
    result.drawError = gfx::PrepareDrawError::InvalidInput;
    return result;
  }

  if (coverage_) {
    coverage_->observe(integration::FeatureClass::Primitive, primitive, "GX primitive");
    coverage_->observe(integration::FeatureClass::VertexFormat, fmt, "GX vertex format");
  }
  auto pipeline = translate_current_pipeline(primitive, fmt);
  if (pipeline.blendMode == gfx::BlendMode::Logic && pipeline.logicOp != gfx::LogicOp::Clear &&
      pipeline.logicOp != gfx::LogicOp::Copy && pipeline.logicOp != gfx::LogicOp::Noop) {
    result.warnings |= SubmitWarning::LogicOpFallback;
    if (coverage_) coverage_->fallback(static_cast<uint64_t>(pipeline.logicOp), "GX logic op reduced to COPY on vitaGL");
    if (telemetry_) telemetry_->unsupported();
    if (strictUnsupported_) strictFailed_ = true;
  }
  const uint64_t translatedPipelineKey = gfx::pipeline_key(pipeline);
  bool usesOrigLod = false;
  for (unsigned i=0;i<pipeline.tev.stageCount && i<pipeline.tev.stages.size();++i)
    usesOrigLod = usesOrigLod || pipeline.tev.stages[i].indirectUseOrigLod;
  if (usesOrigLod) {
    result.warnings |= SubmitWarning::OrigLodApproximation;
    if (coverage_) coverage_->fallback(translatedPipelineKey ^ 0x4f5249474c4f44ull, "indTexUseOrigLOD approximated by vitaGL sampler derivatives");
    if (telemetry_) telemetry_->unsupported();
    if (strictUnsupported_) strictFailed_ = true;
  }
  if (aurora::gx::g_gxState.zCompLocBeforeTex) {
    result.warnings |= SubmitWarning::ZCompLocApproximation;
    if (coverage_) coverage_->fallback(1, "zCompLocBeforeTex cannot be represented exactly through vitaGL");
    if (telemetry_) telemetry_->unsupported();
    if (strictUnsupported_) strictFailed_ = true;
  }
  if (coverage_) {
    coverage_->observe(integration::FeatureClass::TevProgram, translatedPipelineKey, "translated GX pipeline/TEV");
    bool hasIndirect = pipeline.tev.indirectStageCount != 0;
    for (unsigned i = 0; i < pipeline.tev.stageCount && i < pipeline.tev.stages.size(); ++i) hasIndirect = hasIndirect || pipeline.tev.stages[i].indirectEnabled;
    if (hasIndirect) coverage_->observe(integration::FeatureClass::IndirectTev, translatedPipelineKey, "indirect TEV");
    if (pipeline.fogMode != gfx::FogMode::None) coverage_->observe(integration::FeatureClass::Fog, static_cast<uint64_t>(pipeline.fogMode), "GX fog mode");
    if (pipeline.texgenCount) coverage_->observe(integration::FeatureClass::TexGen, translatedPipelineKey ^ pipeline.texgenCount, "GX texgen program");
    bool lit = false; for (const auto& c : pipeline.colorChannels) lit = lit || c.lightingEnabled;
    if (lit) coverage_->observe(integration::FeatureClass::Lighting, translatedPipelineKey, "GX lighting");
  }
  const auto layout = translate_current_vertex_layout(fmt);
  gfx::VertexTransformState vertexState{};
  gfx::DrawUniforms uniforms{};
  translate_vertex_state(vertexState, uniforms);
  const auto source = translate_source_primitive(primitive);
  const auto expansion = translate_primitive_expansion(translate_line_mode(primitive));
  const auto footprint = gfx::estimate_draw_footprint(source,vertexCount,indexCount);
  if(!footprint.valid){
    result.drawError=gfx::PrepareDrawError::TooManyVertices;
    if(telemetry_)telemetry_->unsupported();
    return result;
  }
  // A GX frame is not required to fit in one giant CPU/GPU staging buffer.
  // Submit the completed chunk and orphan the dynamic backing store before doing
  // any decode/lighting/texture work for the next draw. Previously Strikers hit
  // the 2 MiB arena and then spent hundreds of milliseconds preparing hundreds
  // of draws that could only fail at enqueue time.
  if(!arena_->can_reserve(footprint.vertexBytes,alignof(gfx::GpuVertex),
                          footprint.indexBytes,alignof(uint16_t))){
    flush();
    if(!arena_->recycle_current()||
       !arena_->can_reserve(footprint.vertexBytes,alignof(gfx::GpuVertex),
                            footprint.indexBytes,alignof(uint16_t))){
      result.drawError=gfx::PrepareDrawError::StreamingOverflow;
      if(telemetry_)telemetry_->arena_overflow();
      return result;
    }
#if defined(__vita__)
    static uint64_t rolloverLogCount=0;
    const auto n=arena_->recycles();
    if(rolloverLogCount<8||(n&&(n&(n-1))==0)){
      std::fprintf(stderr,"[aurora-vita] stream_rollover total=%llu gpu_stride=%u next_vtx=%u next_idx=%u\n",
                   static_cast<unsigned long long>(n),static_cast<unsigned>(sizeof(gfx::GpuVertex)),
                   footprint.vertexCount,footprint.indexCount);
      ++rolloverLogCount;
    }
#endif
  }
  auto prepared = gfx::prepare_draw(rawVertices, rawBytes, vertexCount, source, layout,
                                    pipeline, vertexState, &uniforms, expansion, telemetry_);
  if (prepared.ok() && rawIndices && indexCount) {
    prepared.indices.assign(rawIndices, rawIndices + indexCount);
    for (const uint16_t index : prepared.indices) {
      if (index >= prepared.vertices.size()) {
        result.drawError = gfx::PrepareDrawError::InvalidInput;
        if (coverage_) coverage_->unsupported(index, "indexed draw out of range");
        if (telemetry_) telemetry_->unsupported();
        if (strictUnsupported_) strictFailed_ = true;
        return result;
      }
    }
  }

  if (!prepared.ok()) {
    result.drawError = prepared.error;
    if (coverage_) coverage_->unsupported(static_cast<uint64_t>(prepared.error), "prepare_draw failed");
    if (telemetry_) telemetry_->unsupported();
    if (strictUnsupported_) strictFailed_ = true;
    return result;
  }

#if defined(__vita__)
  log_large_draw_geometry(prepared,vertexState,pipeline,vertexCount);
#endif

  std::array<gfx::TextureBinding, gfx::MaxTextures> bindings{};
  const uint8_t textureMask = sampled_texture_mask(pipeline);
  const auto white = white_texture();
  { gfx::ScopedTelemetryPhase textureTimer(telemetry_, gfx::TelemetryPhase::TextureResolve);
  for (unsigned slot = 0; slot < gfx::MaxTextures; ++slot) {
    if ((textureMask & (1u << slot)) == 0) continue;
    const auto translated = translate_texture(slot);
    if (coverage_ && translated.valid) {
      const uint64_t texKey = (static_cast<uint64_t>(translated.texture.format) << 32) | translated.texture.width;
      coverage_->observe(integration::FeatureClass::TextureFormat, texKey, "sampled GX texture format");
    }
    if (translated.dynamicCopy) {
      const auto ptr = reinterpret_cast<uintptr_t>(aurora::gx::g_gxState.textures[slot].texObj.data);
      const auto ci = copyTextures_.find(ptr);
      if (ci != copyTextures_.end() && ci->second.handle) {
        bindings[slot] = gfx::TextureBinding{ci->second.handle, translated.sampler, gfx::TextureSource::Efb};
        continue;
      }
    }
    if (translated.valid) {
      const auto statsBeforeTexture = renderer_->stats();
      const auto handle = renderer_->create_texture(translated.texture);
      if (telemetry_) {
        const auto statsAfterTexture = renderer_->stats();
        const uint32_t hits = statsAfterTexture.textureHits - statsBeforeTexture.textureHits;
        const uint32_t misses = statsAfterTexture.textureMisses - statsBeforeTexture.textureMisses;
        const uint32_t uploads = statsAfterTexture.textureUploads - statsBeforeTexture.textureUploads;
        for (uint32_t i = 0; i < hits; ++i) telemetry_->texture(true, false, 0);
        for (uint32_t i = 0; i < misses; ++i) telemetry_->texture(false, uploads != 0, uploads ? translated.texture.dataSize : 0);
      }
      if (handle) {
        bindings[slot] = gfx::TextureBinding{handle, translated.sampler, gfx::TextureSource::Cache};
        continue;
      }
    }
    bindings[slot].texture = white;
    bindings[slot].source = gfx::TextureSource::Cache;
    if (translated.valid || translated.dynamicCopy) bindings[slot].sampler = translated.sampler;
    result.fallbackTextureMask |= static_cast<uint8_t>(1u << slot);
    if (translated.dynamicCopy) result.warnings |= SubmitWarning::DynamicCopyFallback;
    else result.warnings |= SubmitWarning::MissingTextureFallback;
  }}
  if (result.fallbackTextureMask != 0) {
    uint32_t fallbackCount = 0;
    for (uint8_t bits = result.fallbackTextureMask; bits; bits >>= 1) fallbackCount += bits & 1u;
    if (telemetry_) telemetry_->fallback_texture(fallbackCount);
    if (coverage_) coverage_->fallback(result.fallbackTextureMask, "texture fallback mask");
    if (strictUnsupported_) strictFailed_ = true;
  }

  if (trace_) {
    trace_->record(translatedPipelineKey, integration::trace_hash_bytes(rawVertices, rawBytes),
                   static_cast<uint32_t>(rawBytes), vertexCount, static_cast<uint32_t>(prepared.indices.size()),
                   primitive, fmt, result.fallbackTextureMask, static_cast<uint8_t>(result.warnings));
  }

  gfx::PrepareDrawError error = gfx::PrepareDrawError::None;
  const auto statsBeforeEnqueue = renderer_->stats();
  uint64_t resolvedPipelineKey = 0;
  if (queuedPipelineValid_ && translatedPipelineKey == queuedTranslatedPipelineKey_ &&
      prepared.primitive == queuedPrimitive_ &&
      prepared.positionIsClipSpace == queuedPositionIsClipSpace_) {
    resolvedPipelineKey = queuedResolvedPipelineKey_;
  } else {
    resolvedPipelineKey = gfx::resolve_draw_pipeline(*renderer_, prepared, pipeline, telemetry_);
    if (resolvedPipelineKey) {
      queuedTranslatedPipelineKey_ = translatedPipelineKey;
      queuedResolvedPipelineKey_ = resolvedPipelineKey;
      queuedPrimitive_ = prepared.primitive;
      queuedPositionIsClipSpace_ = prepared.positionIsClipSpace;
      queuedPipelineValid_ = true;
    }
  }
  if (!gfx::enqueue_draw(*renderer_, *arena_, stream_, prepared, pipeline, uniforms,
                         translate_viewport(), translate_scissor(), bindings, &error, telemetry_,
                         resolvedPipelineKey)) {
    result.drawError = error;
    if (coverage_) coverage_->unsupported(static_cast<uint64_t>(error), "enqueue_draw failed");
    if (telemetry_) { telemetry_->arena_overflow(); telemetry_->unsupported(); }
    if (strictUnsupported_) strictFailed_ = true;
    return result;
  }
  if (telemetry_) {
    const auto statsAfterEnqueue = renderer_->stats();
    const uint32_t hitDelta = statsAfterEnqueue.pipelineHits - statsBeforeEnqueue.pipelineHits;
    const uint32_t missDelta = statsAfterEnqueue.pipelineMisses - statsBeforeEnqueue.pipelineMisses;
    for (uint32_t i = 0; i < hitDelta; ++i) telemetry_->pipeline(true);
    for (uint32_t i = 0; i < missDelta; ++i) telemetry_->pipeline(false);
  }
  ++submittedDraws_;
  if (telemetry_) telemetry_->add_draw(static_cast<uint32_t>(prepared.vertices.size()), static_cast<uint32_t>(prepared.indices.size()), static_cast<uint32_t>(prepared.indices.empty() ? prepared.vertices.size() / 3 : prepared.indices.size() / 3));
  result.ok = true;
  result.drawError = gfx::PrepareDrawError::None;
  return result;
}
#endif

} // namespace aurora::vita::gxbridge
