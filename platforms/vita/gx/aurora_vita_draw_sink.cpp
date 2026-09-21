#include "aurora_vita_draw_sink.hpp"
#include "../vita_log.hpp"
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
#if defined(__vita__)
#include <psp2/kernel/sysmem.h>
#endif

namespace aurora::vita::gxbridge {

gfx::MemoryBudgetSnapshot DrawSink::memory_budget() const noexcept {
  if (!renderer_ || !arena_) return {};
  auto result=gfx::capture_memory_budget(*arena_, renderer_->textures(), renderer_->pipelines(), &renderer_->efb());
  if(staticGeometry_){
    result.staticGeometryBytes=staticGeometry_->bytes();result.staticGeometryEntries=staticGeometry_->size();
    result.staticGeometryHits=staticGeometry_->hits();result.staticGeometryMisses=staticGeometry_->misses();
    result.staticGeometryLookupFallbacks=staticGeometry_->lookup_fallbacks();
  }
  return result;
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
  verboseGeometryDiagnostics_ = config.verboseGeometryDiagnostics;
  strictUnsupported_ = config.strictUnsupported;
  allowLitFixedVertexGpu_ = config.allowLitFixedVertexGpu;
  staticGeometryMinVertices_ = std::max<uint32_t>(3u,config.staticGeometryMinVertices);
  diagnosticDrawLimit_ = config.diagnosticDrawLimit;
  frameDrawIndex_ = 0;
  strictFailed_ = false;
  whiteTexture_ = white_texture();
  if (!whiteTexture_) {
    arena_->shutdown();
    arena_.reset();
    renderer_ = nullptr;
    return false;
  }
  initialized_ = true;
  if(config.staticGeometryBudget && renderer.supports_fixed_vertex())
    staticGeometry_=std::make_unique<gfx::StaticGeometryCache>(renderer,config.staticGeometryBudget);
  return true;
}

void DrawSink::shutdown() noexcept {
  if (!initialized_ && !arena_) return;
  stream_.reset();
  preparedScratch_=gfx::PreparedDraw{};
  fixedVertexUniforms_.clear();staticGeometry_.reset();fixedPipelineKeys_.clear();
  translatedVertexStateValid_=false;
  translatedVertexStateLightweight_=false;
#if defined(AURORA_VITA_UPSTREAM)
  translatedStateValid_=false;
  resolvedTextureBindingsValid_=false;
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
  verboseGeometryDiagnostics_ = false;
  strictUnsupported_ = false;
  strictFailed_ = false;
  diagnosticDrawLimit_ = 0;
  frameDrawIndex_ = 0;
  initialized_ = false;
}

void DrawSink::begin_frame(uint64_t frame) noexcept {
  if (!initialized_ || !arena_) return;
  stream_.reset();
  frameDrawIndex_ = 0;
  fixedVertexUniforms_.clear();
  reset_pipeline_run_cache();
#if defined(AURORA_VITA_UPSTREAM)
  resolvedTextureBindingsValid_=false;
#endif
  { gfx::ScopedTelemetryPhase phase(telemetry_,gfx::TelemetryPhase::StreamWait); arena_->begin_frame(frame); }
  if (trace_) trace_->begin_frame(frame);
}

void DrawSink::flush() noexcept {
  if (!initialized_ || !renderer_ || stream_.size() == 0) return;
  bool uploaded=false;
  { gfx::ScopedTelemetryPhase phase(telemetry_,gfx::TelemetryPhase::BufferUpload); uploaded=arena_&&arena_->flush(); }
  if (!uploaded) {
    if (telemetry_) telemetry_->arena_overflow();
    if (strictUnsupported_) strictFailed_ = true;
    stream_.reset();
    fixedVertexUniforms_.clear();
    reset_pipeline_run_cache();
    return;
  }
  // Mapping/uploading the streaming VBO/IBO and resolving textures happens
  // outside Renderer::draw and changes raw GL bindings. Invalidate only resource
  // bindings once per chunk; pipeline/fixed/viewport state stays hot across flushes.
  renderer_->invalidate_resource_bindings();
  { gfx::ScopedTelemetryPhase phase(telemetry_, gfx::TelemetryPhase::Submit); renderer_->execute(stream_); }
  if(renderer_->failed()) {
    if(telemetry_) telemetry_->unsupported();
    strictFailed_=true;
  }
  if(arena_->vertex_used()||arena_->index_used())arena_->mark_current_submitted();
  stream_.reset();
  fixedVertexUniforms_.clear();
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
#if defined(__vita__)
const char* prepare_draw_error_name(gfx::PrepareDrawError error) noexcept {
  switch(error){
  case gfx::PrepareDrawError::None:return "none";
  case gfx::PrepareDrawError::InvalidInput:return "invalid input";
  case gfx::PrepareDrawError::VertexDecodeFailed:return "vertex decode failed";
  case gfx::PrepareDrawError::VertexTransformFailed:return "vertex transform failed";
  case gfx::PrepareDrawError::TooManyVertices:return "too many vertices";
  case gfx::PrepareDrawError::UnsupportedLineExpansion:return "unsupported line expansion";
  case gfx::PrepareDrawError::StreamingOverflow:return "streaming overflow";
  case gfx::PrepareDrawError::PipelineFailed:return "pipeline creation failed";
  }
  return "unknown draw failure";
}
#endif
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
                             const gfx::PipelineDesc& pipeline,const uint8_t* rawVertices,size_t rawBytes,
                             const gfx::VertexDecodeLayout& layout,uint32_t inputVertices) noexcept {
  if(!runtime_log_enabled(RuntimeLogLevel::Debug))return;
  // Runtime.log is intentionally sampled: enough to diagnose the first stadium /
  // character meshes without turning logging itself into the new performance issue.
  static uint32_t logged=0;
  const auto currentPn=aurora::gx::g_gxState.currentPnMtx;
  if((inputVertices<512&&currentPn<10)||logged>=16||prepared.vertices.empty())return;
  ++logged;

  const gfx::VertexDecodeAttribute* posAttr=nullptr;
  const gfx::VertexDecodeAttribute* pnAttr=nullptr;
  for(unsigned i=0;i<layout.count;i++){
    const auto& a=layout.attributes[i];
    if(a.semantic==gfx::VertexSemantic::Position)posAttr=&a;
    else if(a.semantic==gfx::VertexSemantic::PnMatrixIndex)pnAttr=&a;
  }
  AURORA_VITA_LOG_DEBUG(
    "[aurora-vita][3d-layout] in=%u stride=%u current_pn=%u vita_current_pn=%u "
    "pos_src=%u pos_comp=%u pos_cnt=%u pos_frac=%u pos_arr_stride=%u pos_arr_le=%u pn_src=%u\n",
    inputVertices,static_cast<unsigned>(layout.streamStride),static_cast<unsigned>(currentPn),
    static_cast<unsigned>(state.currentPnMatrix),
    posAttr?static_cast<unsigned>(posAttr->source):0u,posAttr?static_cast<unsigned>(posAttr->component):0u,
    posAttr?static_cast<unsigned>(posAttr->components):0u,posAttr?static_cast<unsigned>(posAttr->frac):0u,
    posAttr?static_cast<unsigned>(posAttr->array.stride):0u,posAttr?(posAttr->array.littleEndian?1u:0u):0u,
    pnAttr?static_cast<unsigned>(pnAttr->source):0u);

  const unsigned rawSamples=std::min<unsigned>(3,inputVertices);
  for(unsigned i=0;i<rawSamples;i++){
    gfx::CanonicalVertex raw{};
    if(gfx::decode_vertex_into(rawVertices,rawBytes,i,layout,raw)){
      AURORA_VITA_LOG_DEBUG("[aurora-vita][3d-raw-v] n=%u p=%g,%g,%g pn=%u\n",i,
                   raw.position[0],raw.position[1],raw.position[2],static_cast<unsigned>(raw.pnMatrixIndex));
    }else{
      AURORA_VITA_LOG_DEBUG("[aurora-vita][3d-raw-v] n=%u decode_failed\n",i);
    }
  }

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
  AURORA_VITA_LOG_DEBUG(
    "[aurora-vita][3d-geom] in=%u out=%u idx=%u clip=%u finite=%u wpos=%u xy=%u glz=%u z01=%u "
    "pos=[%g,%g,%g,%g..%g,%g,%g,%g] clipbox=[%g,%g,%g,%g..%g,%g,%g,%g] "
    "depth=%u/%u cull=%u revz=%u warn_zbefore=%u\n",
    inputVertices,static_cast<unsigned>(prepared.vertices.size()),static_cast<unsigned>(prepared.indices.size()),
    prepared.positionIsClipSpace?1u:0u,finite,wPositive,insideXY,insideGL,insideZ01,
    pmin[0],pmin[1],pmin[2],pmin[3],pmax[0],pmax[1],pmax[2],pmax[3],
    cmin[0],cmin[1],cmin[2],cmin[3],cmax[0],cmax[1],cmax[2],cmax[3],
    pipeline.depthTest?1u:0u,static_cast<unsigned>(pipeline.depthFunc),static_cast<unsigned>(pipeline.cull),
    pipeline.reversedZ?1u:0u,aurora::gx::g_gxState.zCompLocBeforeTex?1u:0u);

  AURORA_VITA_LOG_DEBUG("[aurora-vita][3d-proj] %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g\n",
    state.projection[0],state.projection[1],state.projection[2],state.projection[3],
    state.projection[4],state.projection[5],state.projection[6],state.projection[7],
    state.projection[8],state.projection[9],state.projection[10],state.projection[11],
    state.projection[12],state.projection[13],state.projection[14],state.projection[15]);

  const unsigned samples=std::min<unsigned>(3,static_cast<unsigned>(prepared.vertices.size()));
  for(unsigned i=0;i<samples;i++){
    const auto& v=prepared.vertices[i];const auto c=project_for_diag(state.projection,v,prepared.positionIsClipSpace);
    AURORA_VITA_LOG_DEBUG("[aurora-vita][3d-v] n=%u p=%g,%g,%g,%g c=%g,%g,%g,%g pn=%u\n",
      i,v.position[0],v.position[1],v.position[2],v.position[3],c.x,c.y,c.z,c.w,static_cast<unsigned>(v.pnMatrixIndex));
  }
  AURORA_VITA_LOG_DEBUG("[aurora-vita][3d-i] %u %u %u %u %u %u\n",
    prepared.indices.size()>0?prepared.indices[0]:0u,prepared.indices.size()>1?prepared.indices[1]:0u,
    prepared.indices.size()>2?prepared.indices[2]:0u,prepared.indices.size()>3?prepared.indices[3]:0u,
    prepared.indices.size()>4?prepared.indices[4]:0u,prepared.indices.size()>5?prepared.indices[5]:0u);
}
#endif
}

bool DrawSink::copy_tex(const void* dest, bool clear) noexcept {
  if (!initialized_ || !renderer_ || !dest) return false;
  resolvedTextureBindingsValid_=false;
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
  // Use the same raster extent as map_logical_scissor, not the scanout backing
  // size. Otherwise reduced-resolution shadows copy/stretch unrelated pixels.
  const auto renderSize = aurora::gfx::get_render_target_size();
  const float sx = logicalFb.x ? static_cast<float>(renderSize.x) / static_cast<float>(logicalFb.x) : 1.f;
  const float sy = logicalFb.y ? static_cast<float>(renderSize.y) / static_cast<float>(logicalFb.y) : 1.f;
  const uint32_t dstW = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(static_cast<float>(g.texCopyDstWidth) * sx)));
  const uint32_t dstH = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(static_cast<float>(g.texCopyDstHeight) * sy)));
  // map_logical_scissor deliberately expands the far edge with ceil() so a
  // raster scissor never drops a covered pixel. That is the wrong policy for
  // an unscaled GXCopyTex: at fractional Vita scale (e.g. 256 * 544/480) it
  // turns a logical 256x256 copy into 384x291 while the destination is 384x290,
  // forcing the GXM backend into its full-frame CPU readback fallback. For the
  // common origin-aligned 1:1 guest copy, source and destination describe the
  // same logical extent, so use the destination's rounded physical extent too.
  // Strikers' projected-shadow atlas uses an exact 2:1 guest downscale.  At a
  // fractional physical scale the independently rounded source can differ by a
  // pixel (181x180 -> 91x90), which needlessly rejects the native GXM 2x
  // transfer.  Preserve the mapped origin but snap that known-ratio extent to
  // exactly twice the physical destination.
  if(g.texCopySrc.x==0 && g.texCopySrc.y==0 && g.texCopySrc.width>0 && g.texCopySrc.height>0 &&
     static_cast<uint32_t>(g.texCopySrc.width)==g.texCopyDstWidth &&
     static_cast<uint32_t>(g.texCopySrc.height)==g.texCopyDstHeight) {
    src.width=static_cast<int32_t>(std::min<uint32_t>(dstW,renderer_->target_width()-static_cast<uint32_t>(src.x)));
    src.height=static_cast<int32_t>(std::min<uint32_t>(dstH,renderer_->target_height()-static_cast<uint32_t>(src.y)));
  } else if(g.texCopySrc.width>0 && g.texCopySrc.height>0 && src.x>=0 && src.y>=0 &&
            static_cast<uint32_t>(g.texCopySrc.width)==g.texCopyDstWidth*2u &&
            static_cast<uint32_t>(g.texCopySrc.height)==g.texCopyDstHeight*2u) {
    src.width=static_cast<int32_t>(std::min<uint32_t>(dstW*2u,renderer_->target_width()-static_cast<uint32_t>(src.x)));
    src.height=static_cast<int32_t>(std::min<uint32_t>(dstH*2u,renderer_->target_height()-static_cast<uint32_t>(src.y)));
  }
#endif
#if defined(AURORA_VITA_UPSTREAM_STUB)
  const uint32_t rawCopyFormat = 0;
  const gfx::EfbCopyFormat copyFormat = gfx::EfbCopyFormat::Passthrough;
#else
  const uint32_t rawCopyFormat = static_cast<uint32_t>(g.texCopyFmt);
  const gfx::EfbCopyFormat copyFormat = gfx::efb_copy_format_from_gx_raw(rawCopyFormat);
#endif
#if defined(__vita__)
  {
    static uint64_t copyLogCount=0;
    const uint64_t n=++copyLogCount;
    if(n<=8 || (n&(n-1))==0)
      AURORA_VITA_LOG_DEBUG(
        "[aurora-vita] gx_copy_tex n=%llu src=%d,%d %dx%d dst=%ux%u fmt=0x%x mapped=%u clear=%u\n",
        static_cast<unsigned long long>(n),src.x,src.y,src.width,src.height,dstW,dstH,
        rawCopyFormat,static_cast<unsigned>(copyFormat),clear?1u:0u);
  }
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
  // vitaGL keeps the legacy physical 180-degree copy. Native GXM avoids touching
  // the uncached target pixels on the CPU: its transfer stores the image as-is
  // and the EFB texture binding carries the horizontal GX mirror to the shader.
#if defined(AURORA_VITA_RENDERER_GXM)
  constexpr bool physicalFlipX=false,physicalFlipY=true;
  // Native GXM EFB copies are already sampled in the correct horizontal
  // orientation. Applying an additional logical X flip mirrors copy textures
  // (including several UI/effect layers in Strikers).
  constexpr bool logicalFlipX=false,logicalFlipY=false;
  const bool forceOpaque=copyFormat==gfx::EfbCopyFormat::RGB565;
  const bool deferR4=copyFormat==gfx::EfbCopyFormat::R4;
  const bool deferA8=copyFormat==gfx::EfbCopyFormat::A8;
  const auto physicalFormat=(forceOpaque||deferR4||deferA8)?gfx::EfbCopyFormat::Passthrough:copyFormat;
  const auto sampleFormat=deferR4?gfx::EfbCopyFormat::R4:
      (deferA8?gfx::EfbCopyFormat::A8:gfx::EfbCopyFormat::Passthrough);
#else
  constexpr bool physicalFlipX=true,physicalFlipY=true;
  constexpr bool logicalFlipX=false,logicalFlipY=false;
  constexpr bool forceOpaque=false;
  const auto physicalFormat=copyFormat;
  constexpr auto sampleFormat=gfx::EfbCopyFormat::Passthrough;
#endif
  const auto h = renderer_->capture_current(oldHandle, src, dstW, dstH, physicalFormat, physicalFlipX, physicalFlipY);
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
  copyTextures_[copyKey] = CopyTextureEntry{h, dstW, dstH, oldRevision + 1, logicalFlipX, logicalFlipY, forceOpaque, sampleFormat};
  resolvedTextureBindingsValid_=false;
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
  resolvedTextureBindingsValid_=false;
}

void DrawSink::clear_copy_textures() noexcept {
  if(renderer_)for(auto& [_,e]:copyTextures_)if(e.handle)renderer_->efb().destroy(e.handle);
  copyTextures_.clear();
  resolvedTextureBindingsValid_=false;
}

SubmitResult DrawSink::submit(uint8_t primitive, uint8_t fmt, const uint8_t* rawVertices,
                              size_t rawBytes, uint32_t vertexCount,
                              const uint16_t* rawIndices, uint32_t indexCount,
                              const uint8_t* stableSource) noexcept {
  gfx::ScopedTelemetryPhase drawTimer(telemetry_,gfx::TelemetryPhase::DrawFrontend);
  SubmitResult result{};
  if (!initialized_ || !renderer_ || !arena_ || !rawVertices || vertexCount == 0) {
    result.drawError = gfx::PrepareDrawError::InvalidInput;
    return result;
  }
  const uint32_t drawIndex=++frameDrawIndex_;
  if(diagnosticDrawLimit_!=0&&drawIndex>diagnosticDrawLimit_) {
    // The FIFO decoder expects a successful sink submission before it clears
    // transient dirty state. This debug gate skips only the actual rendering.
    result.ok=true;
    return result;
  }

  if (coverage_) {
    coverage_->observe(integration::FeatureClass::Primitive, primitive, "GX primitive");
    coverage_->observe(integration::FeatureClass::VertexFormat, fmt, "GX vertex format");
  }
  const uint32_t stateGeneration = aurora::gx::g_gxState.pipelineStateGeneration;
  const bool translatedCacheHit = translatedStateValid_ && translatedStateGeneration_ == stateGeneration &&
                                  translatedPrimitive_ == primitive && translatedFmt_ == fmt;
  if (!translatedCacheHit) {
    gfx::ScopedTelemetryPhase phase(telemetry_,gfx::TelemetryPhase::StateTranslate);
    translatedPipeline_ = translate_current_pipeline(primitive, fmt);
    translatedLayout_ = translate_current_vertex_layout(fmt);
    translatedPipelineKey_ = gfx::pipeline_key(translatedPipeline_);
    if(staticGeometry_){
      translatedGpuPipeline_=translatedPipeline_;
      translatedGpuPipeline_.fixedVertexOnGpu=true;
      translatedGpuPipeline_.fixedVertexIndexedPn=gfx::vertex_layout_has_semantic(
          translatedLayout_,gfx::VertexSemantic::PnMatrixIndex);
      translatedGpuPipeline_.layout=gfx::fixed_vertex_gpu_layout(translatedGpuPipeline_);
    }
    translatedTextureMask_=gfx::pipeline_sampled_texture_mask(translatedPipeline_);
    translatedUsesOrigLod_=false;
    translatedHasIndirect_=translatedPipeline_.tev.indirectStageCount!=0;
    translatedLit_=false;
    for(unsigned i=0;i<translatedPipeline_.tev.stageCount&&i<translatedPipeline_.tev.stages.size();++i){
      const auto&s=translatedPipeline_.tev.stages[i];
      translatedUsesOrigLod_=translatedUsesOrigLod_||s.indirectUseOrigLod;
      translatedHasIndirect_=translatedHasIndirect_||s.indirectEnabled;
    }
    for(const auto&c:translatedPipeline_.colorChannels)translatedLit_=translatedLit_||c.lightingEnabled;
    translatedStateGeneration_ = stateGeneration;
    translatedPrimitive_ = primitive;
    translatedFmt_ = fmt;
    translatedStateValid_ = true;
  }
  const auto& pipeline = translatedPipeline_;
  const auto& layout = translatedLayout_;
#if defined(__vita__)
  static bool memoryProfileLogged=false;
  if(!memoryProfileLogged&&runtime_log_enabled(RuntimeLogLevel::Debug)&&
      telemetry_&&telemetry_->split_vertex_phases()&&vertexCount>=100) {
    memoryProfileLogged=true;
    const void* sourceArray=nullptr;
    for(unsigned i=0;i<layout.count;++i)if(layout.attributes[i].array.data){sourceArray=layout.attributes[i].array.data;break;}
    const void* addresses[]={this,&aurora::gx::g_gxState,rawVertices,sourceArray};
    const char* labels[]={"heap","gx_state","fifo","source_array"};
    for(unsigned i=0;i<4;++i)if(addresses[i]){
      SceKernelMemBlockInfo info{};info.size=sizeof(info);
      const int rc=sceKernelGetMemBlockInfoByAddr(const_cast<void*>(addresses[i]),&info);
      AURORA_VITA_LOG_DEBUG("[aurora-vita] memory_profile object=%s address=%p rc=%d type=0x%08x memory_type=0x%x\n",
        labels[i],addresses[i],rc,static_cast<unsigned>(info.type),info.memoryType);
    }
  }
#endif
  const uint64_t translatedPipelineKey = translatedPipelineKey_;
  if (pipeline.blendMode == gfx::BlendMode::Logic && pipeline.logicOp != gfx::LogicOp::Clear &&
      pipeline.logicOp != gfx::LogicOp::Copy && pipeline.logicOp != gfx::LogicOp::Noop) {
    result.warnings |= SubmitWarning::LogicOpFallback;
    if (coverage_) coverage_->fallback(static_cast<uint64_t>(pipeline.logicOp), "GX logic op reduced to COPY on vitaGL");
    if (telemetry_) telemetry_->unsupported();
    if (strictUnsupported_) strictFailed_ = true;
  }
  if (translatedUsesOrigLod_) {
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
    if (translatedHasIndirect_) coverage_->observe(integration::FeatureClass::IndirectTev, translatedPipelineKey, "indirect TEV");
    if (pipeline.fogMode != gfx::FogMode::None) coverage_->observe(integration::FeatureClass::Fog, static_cast<uint64_t>(pipeline.fogMode), "GX fog mode");
    if (pipeline.texgenCount) coverage_->observe(integration::FeatureClass::TexGen, translatedPipelineKey ^ pipeline.texgenCount, "GX texgen program");
    if (translatedLit_) coverage_->observe(integration::FeatureClass::Lighting, translatedPipelineKey, "GX lighting");
  }
  const auto source = translate_source_primitive(primitive);
  translatedVertexState_.currentPnMatrix=static_cast<uint8_t>(std::min<u32>(
      aurora::gx::g_gxState.currentPnMtx,translatedVertexState_.postexMatrices.size()-1));
  const uint32_t fixedMinVertices=stableSource?staticGeometryMinVertices_:48u;
  const bool fixedCandidate=staticGeometry_&&(!translatedLit_||allowLitFixedVertexGpu_)&&
      vertexCount>=fixedMinVertices&&rawIndices==nullptr&&indexCount==0&&
      source!=gfx::SourcePrimitive::Lines&&source!=gfx::SourcePrimitive::LineStrip&&source!=gfx::SourcePrimitive::Points&&
      gfx::supports_fixed_vertex_gpu(pipeline,layout,translatedVertexState_);
  // The GPU vertex path copies only the state its generated shader can consume.
  // Indexed-PN and lit draws include their position/normal palettes and lights;
  // unsupported bump/dynamic-tex-matrix cases remain on the CPU path.
  if(!translatedVertexStateValid_||aurora::gx::g_gxState.stateDirty||
     (translatedVertexStateLightweight_&&!fixedCandidate)){
    gfx::ScopedTelemetryPhase phase(telemetry_,gfx::TelemetryPhase::StateTranslate);
    if(fixedCandidate) {
      translate_fixed_vertex_state(translatedVertexState_,translatedUniforms_,translatedGpuPipeline_);
      translatedVertexStateLightweight_=true;
    } else {
      translate_vertex_state(translatedVertexState_,translatedUniforms_,pipeline,layout);
      translatedVertexStateLightweight_=false;
    }
    translatedVertexStateValid_=true;
  }
  const auto& vertexState=translatedVertexState_;
  auto& uniforms=translatedUniforms_;
  const gfx::StaticGeometryCache::Entry* gpuGeometry=nullptr;
  uint64_t fixedPipelineKey=0;
  if(fixedCandidate){
#if defined(__vita__)
    static unsigned debugGpuDraws=0;
    const bool debugGpu=telemetry_&&telemetry_->split_vertex_phases()&&debugGpuDraws++<4;
    if(debugGpu)AURORA_VITA_LOG_DEBUG("[aurora-vita] gpu_vertex_probe begin count=%u primitive=%u key=%llx\n",vertexCount,primitive,static_cast<unsigned long long>(translatedPipelineKey));
#endif
    const uint64_t gpuPipelineDescKey=gfx::pipeline_key(translatedGpuPipeline_);
    auto key=fixedPipelineKeys_.find(gpuPipelineDescKey);
    if(key!=fixedPipelineKeys_.end()&&renderer_->pipelines().find(key->second))fixedPipelineKey=key->second;
    else{
      gfx::ScopedTelemetryPhase phase(telemetry_,gfx::TelemetryPhase::PipelineResolve);
      fixedPipelineKey=renderer_->create_pipeline(translatedGpuPipeline_);
      if(fixedPipelineKey)fixedPipelineKeys_[gpuPipelineDescKey]=fixedPipelineKey;
    }
#if defined(__vita__)
    if(debugGpu)AURORA_VITA_LOG_DEBUG("[aurora-vita] gpu_vertex_probe pipeline=%llx\n",static_cast<unsigned long long>(fixedPipelineKey));
#endif
    if(fixedPipelineKey){
      // A warm geometry hit still queues a reference to this program. Protect
      // it from cache eviction until the current command batch is executed.
      renderer_->pipelines().pin(fixedPipelineKey);
      gfx::ScopedTelemetryPhase phase(telemetry_,gfx::TelemetryPhase::GeometryCache);
      const auto before=staticGeometry_->hits();
      gpuGeometry=staticGeometry_->get(rawVertices,rawBytes,vertexCount,source,layout,translatedGpuPipeline_,vertexState,telemetry_,stableSource);
#if defined(__vita__)
      if(debugGpu)AURORA_VITA_LOG_DEBUG("[aurora-vita] gpu_vertex_probe geometry=%p entries=%u\n",static_cast<const void*>(gpuGeometry),static_cast<unsigned>(staticGeometry_->size()));
#endif
      if(gpuGeometry&&telemetry_)telemetry_->gpu_geometry(staticGeometry_->hits()!=before,gpuGeometry->vertexCount);
    }
  }
  if(!gpuGeometry&&translatedVertexStateLightweight_) {
    // A shader/cache miss or a full geometry budget can reject a GPU candidate.
    // The CPU fallback needs its complete matrices, lights and texgen state.
    gfx::ScopedTelemetryPhase phase(telemetry_,gfx::TelemetryPhase::StateTranslate);
    translate_vertex_state(translatedVertexState_,translatedUniforms_,pipeline,layout);
    translatedVertexStateLightweight_=false;
  }
  const auto expansion = translate_primitive_expansion(translate_line_mode(primitive));
  const auto gpuLayout=gfx::gpu_vertex_layout(gfx::pipeline_texcoord_mask(pipeline),gfx::pipeline_raster_color_mask(pipeline));
  const size_t gpuStride=gpuLayout.count?gpuLayout.attributes[0].stride:sizeof(gfx::GpuVertex);
  const auto footprint = gfx::estimate_draw_footprint(source,vertexCount,indexCount,gpuStride);
  const bool useStreamed=!gpuGeometry&&!(telemetry_&&telemetry_->split_vertex_phases())&&
      source!=gfx::SourcePrimitive::Lines&&source!=gfx::SourcePrimitive::LineStrip&&
      source!=gfx::SourcePrimitive::Points;
  const bool exactTriangleDedup=!useStreamed&&source==gfx::SourcePrimitive::Triangles&&
      rawIndices==nullptr&&indexCount==0;
  if(!footprint.valid){
    result.drawError=gfx::PrepareDrawError::TooManyVertices;
    if(coverage_)coverage_->unsupported(static_cast<uint64_t>(result.drawError),prepare_draw_error_name(result.drawError));
    if(telemetry_)telemetry_->unsupported();
    return result;
  }
  // A GX frame is not required to fit in one giant CPU/GPU staging buffer.
  // Submit the completed chunk and safely rewind its backing store before doing
  // any decode/lighting/texture work for the next draw. Previously Strikers hit
  // the 2 MiB arena and then spent hundreds of milliseconds preparing hundreds
  // of draws that could only fail at enqueue time.
  const auto arenaCanFit=[&](const gfx::DrawFootprint& required) noexcept {
    if(!arena_->can_reserve(required.vertexBytes,gpuStride,required.indexBytes,alignof(uint16_t)))return false;
    if(required.indexBytes){
      if(renderer_->uses_local_stream_indices()){
        if(required.vertexCount>renderer_->max_indexed_vertices())return false;
      }else{
        const size_t used=arena_->vertex_used();
        const size_t rem=used%gpuStride;
        const size_t aligned=rem?used+(gpuStride-rem):used;
        if(aligned/gpuStride+required.vertexCount>renderer_->max_indexed_vertices())return false;
      }
    }
    return true;
  };
  const auto ensureArena=[&](const gfx::DrawFootprint& required) noexcept {
    if(arenaCanFit(required))return true;
    flush();
    bool recycled=false;
    { gfx::ScopedTelemetryPhase phase(telemetry_,gfx::TelemetryPhase::StreamWait); recycled=arena_->recycle_current(); }
    if(!recycled||
       !arenaCanFit(required)){
      result.drawError=gfx::PrepareDrawError::StreamingOverflow;
      if(telemetry_)telemetry_->arena_overflow();
      return false;
    }
#if defined(__vita__)
    if(telemetry_||coverage_||trace_){
      static uint64_t rolloverLogCount=0;
      const auto n=arena_->recycles();
      if(rolloverLogCount<8||(n&&(n&(n-1))==0)){
        AURORA_VITA_LOG_DEBUG("[aurora-vita] stream_rollover total=%llu syncs=%llu slot=%u gpu_stride=%u next_vtx=%u next_idx=%u\n",
                     static_cast<unsigned long long>(n),static_cast<unsigned long long>(arena_->gpu_syncs()),arena_->slot(),
                     static_cast<unsigned>(gpuStride),required.vertexCount,required.indexCount);
        ++rolloverLogCount;
      }
    }
#endif
    return true;
  };
  // Exact triangle-record deduplication can shrink the VBO substantially, so
  // defer rollover until the real unique-vertex count is known. Other paths
  // keep the early guard that avoids preparing a draw which cannot fit.
  if(!gpuGeometry&&!exactTriangleDedup&&!ensureArena(footprint))return result;
  auto& prepared=preparedScratch_;
  gfx::StreamedDraw streamed{};
  if(useStreamed) {
    (void)gfx::prepare_streamed_draw_into(streamed,*arena_,rawVertices,rawBytes,vertexCount,
        source,rawIndices,indexCount,layout,pipeline,vertexState,&uniforms,telemetry_);
  } else if(!gpuGeometry) {
    (void)gfx::prepare_draw_into(prepared,rawVertices,rawBytes,vertexCount,source,layout,
                                pipeline,vertexState,&uniforms,expansion,telemetry_,exactTriangleDedup);
  }
  if (!useStreamed && prepared.ok() && rawIndices && indexCount) {
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
  if (!gpuGeometry && !(useStreamed?streamed.ok():prepared.ok())) {
    result.drawError = useStreamed?streamed.error:prepared.error;
    if (coverage_) coverage_->unsupported(static_cast<uint64_t>(result.drawError),
                                          prepare_draw_error_name(result.drawError));
    if (telemetry_) telemetry_->unsupported();
    if (strictUnsupported_) strictFailed_ = true;
    return result;
  }

  if(!gpuGeometry&&!useStreamed&&exactTriangleDedup){
    gfx::DrawFootprint compactFootprint{};
    compactFootprint.vertexCount=static_cast<uint32_t>(prepared.vertices.size());
    compactFootprint.indexCount=static_cast<uint32_t>(prepared.indices.size());
    compactFootprint.vertexBytes=prepared.vertices.size()*gpuStride;
    compactFootprint.indexBytes=prepared.indices.size()*sizeof(uint16_t);
    compactFootprint.valid=true;
    if(!ensureArena(compactFootprint))return result;
  }

#if defined(__vita__)
  if(verboseGeometryDiagnostics_&&!gpuGeometry&&!useStreamed)
    log_large_draw_geometry(prepared,vertexState,pipeline,rawVertices,rawBytes,layout,vertexCount);
#endif

  std::array<gfx::TextureBinding, gfx::MaxTextures> bindings{};
  const uint8_t textureMask = translatedTextureMask_;
  const auto white = white_texture();
  const bool reuseResolvedTextures=resolvedTextureBindingsValid_&&
                                   !aurora::gx::g_gxState.stateDirty&&resolvedTextureMask_==textureMask&&
                                   resolvedVolatileTextureMask_==0;
  if(reuseResolvedTextures){
    bindings=resolvedTextureBindings_;
    result.fallbackTextureMask=resolvedFallbackTextureMask_;
    result.warnings|=resolvedTextureWarnings_;
    if(telemetry_)for(unsigned slot=0;slot<gfx::MaxTextures;++slot)if(textureMask&(1u<<slot))telemetry_->texture(true,false,0);
  }else{ gfx::ScopedTelemetryPhase textureTimer(telemetry_, gfx::TelemetryPhase::TextureResolve);
  uint8_t volatileTextureMask=0;
  for (unsigned slot = 0; slot < gfx::MaxTextures; ++slot) {
    if ((textureMask & (1u << slot)) == 0) continue;
    const auto translated = translate_texture(slot);
    if (coverage_ && translated.valid) {
      const uint64_t texKey = (static_cast<uint64_t>(translated.texture.format) << 32) | translated.texture.width;
      coverage_->observe(integration::FeatureClass::TextureFormat, texKey, "sampled GX texture format");
    }
    if (translated.dynamicCopy) {
      // Match translate_texture(): the native frontend populates loadedTextures,
      // not Dawn's resolved TextureBind table. Reading that table loses the
      // guest destination identity and turns a valid EFB copy into white fallback.
      const auto ptr = reinterpret_cast<uintptr_t>(aurora::gx::g_gxState.loadedTextures[slot].data);
      const auto ci = copyTextures_.find(ptr);
      if (ci != copyTextures_.end() && ci->second.handle) {
        bindings[slot] = gfx::TextureBinding{ci->second.handle, translated.sampler, gfx::TextureSource::Efb,
                                             ci->second.logicalFlipX,ci->second.logicalFlipY,ci->second.forceOpaque,
                                             ci->second.sampleFormat};
        continue;
      }
    }
    if (translated.valid) {
      if(!translated.texture.cacheable)volatileTextureMask|=static_cast<uint8_t>(1u<<slot);
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
  }
  resolvedTextureBindings_=bindings;
  resolvedTextureMask_=textureMask;
  resolvedVolatileTextureMask_=volatileTextureMask;
  resolvedFallbackTextureMask_=result.fallbackTextureMask;
  resolvedTextureWarnings_=static_cast<SubmitWarning>(static_cast<uint8_t>(result.warnings)&
    (static_cast<uint8_t>(SubmitWarning::MissingTextureFallback)|static_cast<uint8_t>(SubmitWarning::DynamicCopyFallback)));
  resolvedTextureBindingsValid_=true;
  }
  if (result.fallbackTextureMask != 0) {
    uint32_t fallbackCount = 0;
    for (uint8_t bits = result.fallbackTextureMask; bits; bits >>= 1) fallbackCount += bits & 1u;
    if (telemetry_) telemetry_->fallback_texture(fallbackCount);
    if (coverage_) coverage_->fallback(result.fallbackTextureMask, "texture fallback mask");
    if (strictUnsupported_) strictFailed_ = true;
  }

  if (trace_) {
    trace_->record(translatedPipelineKey, integration::trace_hash_bytes(rawVertices, rawBytes),
                   static_cast<uint32_t>(rawBytes), vertexCount,
                   gpuGeometry?gpuGeometry->indexCount:(useStreamed?streamed.indexCount:static_cast<uint32_t>(prepared.indices.size())),
                   primitive, fmt, result.fallbackTextureMask, static_cast<uint8_t>(result.warnings));
  }

  gfx::PrepareDrawError error = gfx::PrepareDrawError::None;
  const auto statsBeforeEnqueue = renderer_->stats();
  uint64_t resolvedPipelineKey = 0;
  if(gpuGeometry){
    resolvedPipelineKey=fixedPipelineKey;
  }else {
    const auto preparedPrimitive=useStreamed?streamed.primitive:prepared.primitive;
    const bool preparedClipSpace=useStreamed?streamed.positionIsClipSpace:prepared.positionIsClipSpace;
    if (queuedPipelineValid_ && translatedPipelineKey == queuedTranslatedPipelineKey_ &&
        preparedPrimitive == queuedPrimitive_ &&
        preparedClipSpace == queuedPositionIsClipSpace_) {
    resolvedPipelineKey = queuedResolvedPipelineKey_;
    } else {
      resolvedPipelineKey = gfx::resolve_draw_pipeline(*renderer_,preparedPrimitive,preparedClipSpace,pipeline,telemetry_);
      if (resolvedPipelineKey) {
        queuedTranslatedPipelineKey_ = translatedPipelineKey;
        queuedResolvedPipelineKey_ = resolvedPipelineKey;
        queuedPrimitive_ = preparedPrimitive;
        queuedPositionIsClipSpace_ = preparedClipSpace;
        queuedPipelineValid_ = true;
      }
    }
  }
  bool enqueued=false;
  if(gpuGeometry){
    gfx::ScopedTelemetryPhase phase(telemetry_,gfx::TelemetryPhase::CommandBuild);
    fixedVertexUniforms_.push_back(gfx::fixed_vertex_uniforms(translatedGpuPipeline_,vertexState));
    gfx::DrawPacket& packet=stream_.emplace_draw();
    packet.pipelineKey=resolvedPipelineKey;packet.vertices=gpuGeometry->vertices;packet.indices=gpuGeometry->indices;
    packet.vertexCount=gpuGeometry->vertexCount;packet.indexCount=gpuGeometry->indexCount;
    packet.textures=bindings;packet.uniforms=uniforms;packet.viewport=translate_viewport();packet.scissor=translate_scissor();
    packet.fixedVertexUniforms=&fixedVertexUniforms_.back();
    enqueued=true;
    queuedPipelineValid_=false;
  }else if(useStreamed) {
    enqueued=gfx::enqueue_streamed_draw(stream_,streamed,resolvedPipelineKey,uniforms,
                         translate_viewport(),translate_scissor(),bindings,&error);
  }else enqueued=gfx::enqueue_draw(*renderer_, *arena_, stream_, prepared, pipeline, uniforms,
                         translate_viewport(), translate_scissor(), bindings, &error, telemetry_,
                         resolvedPipelineKey);
  if(!enqueued) {
    result.drawError = error;
    if (coverage_) coverage_->unsupported(static_cast<uint64_t>(error),prepare_draw_error_name(error));
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
  if (telemetry_) {
    if(gpuGeometry)telemetry_->add_draw(gpuGeometry->vertexCount,gpuGeometry->indexCount,gpuGeometry->indexCount/3);
    else if(useStreamed)telemetry_->add_draw(streamed.vertexCount,streamed.indexCount,
                                             streamed.indexCount?streamed.indexCount/3:streamed.vertexCount/3);
    else telemetry_->add_draw(static_cast<uint32_t>(prepared.vertices.size()), static_cast<uint32_t>(prepared.indices.size()), static_cast<uint32_t>(prepared.indices.empty() ? prepared.vertices.size() / 3 : prepared.indices.size() / 3));
  }
  result.ok = true;
  result.drawError = gfx::PrepareDrawError::None;
  return result;
}
#endif

} // namespace aurora::vita::gxbridge
