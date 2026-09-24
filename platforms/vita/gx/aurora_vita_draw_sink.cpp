#include "aurora_vita_draw_sink.hpp"
#include "../gfx/vita_renderer.hpp"
#include "../gfx/vita_pipeline_key.hpp"
#include "../../../lib/vita/render_size.hpp"

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
#include "../vita_diag.hpp"
#include <limits>
#if defined(__vita__)
#include <psp2/kernel/sysmem.h>
#endif

namespace aurora::vita::gxbridge {

#if defined(AURORA_VITA_UPSTREAM)
DrawSink::TextureResolveStamp DrawSink::texture_resolve_stamp(unsigned slot) const noexcept {
  TextureResolveStamp stamp{};
  if(slot>=gfx::MaxTextures)return stamp;
  const auto& g=aurora::gx::g_gxState;
  const auto& obj=g.loadedTextures[slot];
  stamp.data=reinterpret_cast<uintptr_t>(obj.data);
  stamp.mode0=obj.mode0;
  stamp.mode1=obj.mode1;
  stamp.image0=obj.image0;
  stamp.image3=obj.image3;
  stamp.width=obj.mWidth;
  stamp.height=obj.mHeight;
  stamp.format=obj.mFormat;
  stamp.tlut=static_cast<uint32_t>(obj.tlut);
  stamp.texObjId=obj.texObjId;
  stamp.texDataVersion=obj.texDataVersion;
  stamp.flags=obj.flags;
  const unsigned tlut=static_cast<unsigned>(obj.tlut);
  if(tlut<aurora::gx::MaxTluts) {
    const auto& palette=g.loadedTluts[tlut];
    stamp.paletteData=reinterpret_cast<uintptr_t>(palette.data);
    stamp.paletteFormat=static_cast<uint32_t>(palette.format);
    stamp.paletteEntries=palette.numEntries;
    stamp.paletteObjId=palette.tlutObjId;
    stamp.paletteDataVersion=palette.tlutDataVersion;
    stamp.paletteFlags=palette.flags;
  }
  stamp.gxCopyPresent=g.copyTextures.find(obj.data)!=g.copyTextures.end();
  const auto copy=copyTextures_.find(reinterpret_cast<uintptr_t>(obj.data));
  if(copy!=copyTextures_.end()) {
    stamp.localCopyPresent=true;
    stamp.localCopyRevision=copy->second.revision;
  }
  return stamp;
}
#endif

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
#if !defined(AURORA_VITA_NO_DIAGNOSTICS)
  telemetry_ = config.telemetry;
  coverage_ = config.coverage;
  trace_ = config.trace;
#endif
  verboseGeometryDiagnostics_ = config.verboseGeometryDiagnostics;
  strictUnsupported_ = config.strictUnsupported;
  allowLitFixedVertexGpu_ = config.allowLitFixedVertexGpu;
  diagnosticDrawLimit_ = config.diagnosticDrawLimit;
  frameDrawIndex_ = 0;
  translatedUniformGeneration_=1;
  resolvedTextureGeneration_=1;
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
#if defined(AURORA_VITA_UPSTREAM)
  pipelineTranslationCache_=std::make_unique<PipelineTranslationCache>();
#endif
  return true;
}

void DrawSink::shutdown() noexcept {
  if (!initialized_ && !arena_) return;
  stream_.reset();
  preparedScratch_=gfx::PreparedDraw{};
  fixedVertexUniforms_.clear();staticGeometry_.reset();fixedPipelineKeys_.clear();
  translatedVertexStateValid_=false;
  translatedVertexStateLightweight_=false;
  translatedUniformGeneration_=1;
  resolvedTextureGeneration_=1;
#if defined(AURORA_VITA_UPSTREAM)
  translatedEntry_=nullptr;
  translatedFingerprintLo_=0;
  translatedFingerprintHi_=0;
  pipelineTranslationCache_.reset();
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
#if !defined(AURORA_VITA_NO_DIAGNOSTICS)
  telemetry_ = nullptr;
  coverage_ = nullptr;
  trace_ = nullptr;
#endif
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
  if(staticGeometry_)staticGeometry_->begin_frame(frame,telemetry_);
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
  AURORA_VITA_DIAGF(
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
      AURORA_VITA_DIAGF("[aurora-vita][3d-raw-v] n=%u p=%g,%g,%g pn=%u\n",i,
                   raw.position[0],raw.position[1],raw.position[2],static_cast<unsigned>(raw.pnMatrixIndex));
    }else{
      AURORA_VITA_DIAGF("[aurora-vita][3d-raw-v] n=%u decode_failed\n",i);
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
  AURORA_VITA_DIAGF(
    "[aurora-vita][3d-geom] in=%u out=%u idx=%u clip=%u finite=%u wpos=%u xy=%u glz=%u z01=%u "
    "pos=[%g,%g,%g,%g..%g,%g,%g,%g] clipbox=[%g,%g,%g,%g..%g,%g,%g,%g] "
    "depth=%u/%u cull=%u revz=%u warn_zbefore=%u\n",
    inputVertices,static_cast<unsigned>(prepared.vertices.size()),static_cast<unsigned>(prepared.indices.size()),
    prepared.positionIsClipSpace?1u:0u,finite,wPositive,insideXY,insideGL,insideZ01,
    pmin[0],pmin[1],pmin[2],pmin[3],pmax[0],pmax[1],pmax[2],pmax[3],
    cmin[0],cmin[1],cmin[2],cmin[3],cmax[0],cmax[1],cmax[2],cmax[3],
    pipeline.depthTest?1u:0u,static_cast<unsigned>(pipeline.depthFunc),static_cast<unsigned>(pipeline.cull),
    pipeline.reversedZ?1u:0u,aurora::gx::g_gxState.zCompLocBeforeTex?1u:0u);

  AURORA_VITA_DIAGF("[aurora-vita][3d-proj] %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g\n",
    state.projection[0],state.projection[1],state.projection[2],state.projection[3],
    state.projection[4],state.projection[5],state.projection[6],state.projection[7],
    state.projection[8],state.projection[9],state.projection[10],state.projection[11],
    state.projection[12],state.projection[13],state.projection[14],state.projection[15]);

  const unsigned samples=std::min<unsigned>(3,static_cast<unsigned>(prepared.vertices.size()));
  for(unsigned i=0;i<samples;i++){
    const auto& v=prepared.vertices[i];const auto c=project_for_diag(state.projection,v,prepared.positionIsClipSpace);
    AURORA_VITA_DIAGF("[aurora-vita][3d-v] n=%u p=%g,%g,%g,%g c=%g,%g,%g,%g pn=%u\n",
      i,v.position[0],v.position[1],v.position[2],v.position[3],c.x,c.y,c.z,c.w,static_cast<unsigned>(v.pnMatrixIndex));
  }
  AURORA_VITA_DIAGF("[aurora-vita][3d-i] %u %u %u %u %u %u\n",
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
  uint32_t copyDstW=dstW,copyDstH=dstH;
#if defined(__vita__) && defined(AURORA_VITA_RENDERER_GXM) && !defined(AURORA_VITA_UPSTREAM_STUB)
  const bool promoteReducedFullFrameCopy =
      copyFormat==gfx::EfbCopyFormat::RGB565 &&
      src.x==0 && src.y==0 &&
      static_cast<uint32_t>(src.width)==renderer_->target_width() &&
      static_cast<uint32_t>(src.height)==renderer_->target_height() &&
      g.texCopySrc.width>0 && g.texCopySrc.height>0 &&
      static_cast<uint32_t>(g.texCopySrc.width)==g.texCopyDstWidth*2u &&
      static_cast<uint32_t>(g.texCopySrc.height)==g.texCopyDstHeight*2u;
  if(promoteReducedFullFrameCopy) {
    // The pre-match/glass effect consumes the half-resolution XFB image rather
    // than the raw EFB. Keep its physical size tied to Vita scanout even when
    // the default EFB raster is reduced to 640x448.
    const auto frameSize=aurora::gfx::get_frame_buffer_size();
    const float frameSx=logicalFb.x?static_cast<float>(frameSize.x)/static_cast<float>(logicalFb.x):1.f;
    const float frameSy=logicalFb.y?static_cast<float>(frameSize.y)/static_cast<float>(logicalFb.y):1.f;
    copyDstW=std::max<uint32_t>(1,static_cast<uint32_t>(
        std::lround(static_cast<float>(g.texCopyDstWidth)*frameSx)));
    copyDstH=std::max<uint32_t>(1,static_cast<uint32_t>(
        std::lround(static_cast<float>(g.texCopyDstHeight)*frameSy)));
  }
#endif
  uint32_t captureW=copyDstW,captureH=copyDstH;
#if defined(__vita__) && !defined(AURORA_VITA_RENDERER_GXM)
  const bool promoteFullFrameHalfScale =
      copyFormat==gfx::EfbCopyFormat::RGB565 &&
      src.x==0 && src.y==0 &&
      static_cast<uint32_t>(src.width)==renderer_->target_width() &&
      static_cast<uint32_t>(src.height)==renderer_->target_height() &&
      static_cast<uint32_t>(src.width)==dstW*2u &&
      static_cast<uint32_t>(src.height)==dstH*2u;
  if(promoteFullFrameHalfScale){
    // Strikers uses a logical half-resolution EFB copy for the pre-match
    // fullscreen effect. Keep the guest-visible 480x272 identity, but retain
    // the Vita framebuffer's physical resolution so the fullscreen resample
    // does not discard every second source pixel.
    captureW=static_cast<uint32_t>(src.width);
    captureH=static_cast<uint32_t>(src.height);
  }
#endif
#if defined(__vita__)
  {
    static uint64_t copyLogCount=0;
    const uint64_t n=++copyLogCount;
    if(n<=8 || (n&(n-1))==0)
      AURORA_VITA_DIAGF(
        "[aurora-vita] gx_copy_tex n=%llu src=%d,%d %dx%d dst=%ux%u physical=%ux%u fmt=0x%x mapped=%u clear=%u\n",
        static_cast<unsigned long long>(n),src.x,src.y,src.width,src.height,copyDstW,copyDstH,captureW,captureH,
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
  // The source EFB and the sampled copy already use the same horizontal
  // orientation on vitaGL. Mirroring here makes full-frame copy textures
  // (notably Strikers' 960x544 -> 480x272 pre-match image) appear reversed.
  constexpr bool physicalFlipX=false,physicalFlipY=true;
  constexpr bool logicalFlipX=false,logicalFlipY=false;
  constexpr bool forceOpaque=false;
  const auto physicalFormat=copyFormat;
  constexpr auto sampleFormat=gfx::EfbCopyFormat::Passthrough;
#endif
  const auto h = renderer_->capture_current(oldHandle, src, captureW, captureH, physicalFormat, physicalFlipX, physicalFlipY);
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
  copyTextures_[copyKey] = CopyTextureEntry{h, copyDstW, copyDstH, oldRevision + 1, logicalFlipX, logicalFlipY, forceOpaque, sampleFormat};
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
  const auto& gxState=aurora::gx::g_gxState;
  const uint64_t fingerprintLo=gxState.vitaPipelineFingerprintLo;
  const uint64_t fingerprintHi=gxState.vitaPipelineFingerprintHi;
  const bool runCacheHit=translatedEntry_&&translatedFingerprintLo_==fingerprintLo&&
                         translatedFingerprintHi_==fingerprintHi&&translatedFmt_==fmt;
  bool associativeCacheHit=false;
  if (!runCacheHit) {
    gfx::ScopedTelemetryPhase phase(telemetry_,gfx::TelemetryPhase::StatePipelineTranslate,
                                    gfx::TelemetryPhase::StateTranslate);
    const uint64_t rotatedHi=(fingerprintHi<<17)|(fingerprintHi>>(64-17));
    const uint64_t setHash=fingerprintLo^rotatedHi^
                           (static_cast<uint64_t>(fmt)*0x9e3779b97f4a7c15ull);
    auto& set=(*pipelineTranslationCache_)[static_cast<size_t>(setHash)&
                                           (PipelineTranslationSetCount-1u)];
    PipelineTranslationEntry* entry=nullptr;
    for(size_t way=0;way<set.ways.size();++way) {
      auto& candidate=set.ways[way];
      if(candidate.valid&&candidate.fingerprintLo==fingerprintLo&&
         candidate.fingerprintHi==fingerprintHi&&candidate.fmt==fmt) {
        entry=&candidate;
        set.mru=static_cast<uint8_t>(way);
        associativeCacheHit=true;
        break;
      }
    }
    if(!entry) {
      size_t replacementWay=0;
      if(set.ways[0].valid)replacementWay=set.ways[1].valid?(set.mru^1u):1u;
      entry=&set.ways[replacementWay];
      {
        gfx::ScopedTelemetryPhase subphase(telemetry_,gfx::TelemetryPhase::StatePipelineBuild);
        translate_current_pipeline_and_layout(primitive,fmt,entry->pipeline,entry->layout);
      }
      {
        gfx::ScopedTelemetryPhase subphase(telemetry_,gfx::TelemetryPhase::StatePipelineKey);
        entry->pipelineKey=gfx::pipeline_key(entry->pipeline);
      }
      {
        gfx::ScopedTelemetryPhase subphase(telemetry_,gfx::TelemetryPhase::StatePipelineDerived);
        entry->gpuPipeline=entry->pipeline;
        if(staticGeometry_){
          entry->gpuPipeline.fixedVertexOnGpu=true;
          entry->gpuPipeline.fixedVertexIndexedPn=gfx::vertex_layout_has_semantic(
              entry->layout,gfx::VertexSemantic::PnMatrixIndex);
          entry->gpuPipeline.layout=gfx::fixed_vertex_gpu_layout(entry->gpuPipeline);
        }
        entry->textureMask=gfx::pipeline_sampled_texture_mask(entry->pipeline);
        entry->usesOrigLod=false;
        entry->hasIndirect=entry->pipeline.tev.indirectStageCount!=0;
        entry->lit=false;
        for(unsigned i=0;i<entry->pipeline.tev.stageCount&&i<entry->pipeline.tev.stages.size();++i){
          const auto&s=entry->pipeline.tev.stages[i];
          entry->usesOrigLod=entry->usesOrigLod||s.indirectUseOrigLod;
          entry->hasIndirect=entry->hasIndirect||s.indirectEnabled;
        }
        for(const auto&c:entry->pipeline.colorChannels)entry->lit=entry->lit||c.lightingEnabled;
      }
      entry->fingerprintLo=fingerprintLo;
      entry->fingerprintHi=fingerprintHi;
      entry->fmt=fmt;
      entry->valid=true;
      set.mru=static_cast<uint8_t>(replacementWay);
    }
    translatedEntry_=entry;
    translatedFingerprintLo_=fingerprintLo;
    translatedFingerprintHi_=fingerprintHi;
    translatedFmt_ = fmt;
    if(telemetry_)telemetry_->frontend_pipeline_fingerprint(associativeCacheHit);
  }
  const bool translatedCacheHit=runCacheHit||associativeCacheHit;
  if(telemetry_)telemetry_->frontend_pipeline_translate(translatedCacheHit);
  const auto& pipeline=translatedEntry_->pipeline;
  const auto& layout=translatedEntry_->layout;
  if(!aurora::gx::g_gxState.zCompLocBeforeTex &&
     gfx::alpha_compare_static_result(pipeline.tev.alphaCompare)==gfx::AlphaTestStaticResult::Fail) {
    // GX alpha compare rejects every fragment, so the draw cannot affect color
    // or late depth. ZCompLoc-before-texture is intentionally excluded because
    // its early depth semantics can outlive a later alpha reject. Otherwise
    // drop before vertex decode and before it reaches the TBDR.
    return result;
  }
#if defined(__vita__)
  static bool memoryProfileLogged=false;
  if(!memoryProfileLogged&&telemetry_&&telemetry_->split_vertex_phases()&&vertexCount>=100) {
    memoryProfileLogged=true;
    const void* sourceArray=nullptr;
    for(unsigned i=0;i<layout.count;++i)if(layout.attributes[i].array.data){sourceArray=layout.attributes[i].array.data;break;}
    const void* addresses[]={this,&aurora::gx::g_gxState,rawVertices,sourceArray};
    const char* labels[]={"heap","gx_state","fifo","source_array"};
    for(unsigned i=0;i<4;++i)if(addresses[i]){
      SceKernelMemBlockInfo info{};info.size=sizeof(info);
      const int rc=sceKernelGetMemBlockInfoByAddr(const_cast<void*>(addresses[i]),&info);
      AURORA_VITA_DIAGF("[aurora-vita] memory_profile object=%s address=%p rc=%d type=0x%08x memory_type=0x%x\n",
        labels[i],addresses[i],rc,static_cast<unsigned>(info.type),info.memoryType);
    }
  }
#endif
  const uint64_t translatedPipelineKey=translatedEntry_->pipelineKey;
  if (pipeline.blendMode == gfx::BlendMode::Logic && pipeline.logicOp != gfx::LogicOp::Clear &&
      pipeline.logicOp != gfx::LogicOp::Copy && pipeline.logicOp != gfx::LogicOp::Noop) {
    result.warnings |= SubmitWarning::LogicOpFallback;
    if (coverage_) coverage_->fallback(static_cast<uint64_t>(pipeline.logicOp), "GX logic op reduced to COPY on vitaGL");
    if (telemetry_) telemetry_->unsupported();
    if (strictUnsupported_) strictFailed_ = true;
  }
  if (translatedEntry_->usesOrigLod) {
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
    if (translatedEntry_->hasIndirect) coverage_->observe(integration::FeatureClass::IndirectTev, translatedPipelineKey, "indirect TEV");
    if (pipeline.fogMode != gfx::FogMode::None) coverage_->observe(integration::FeatureClass::Fog, static_cast<uint64_t>(pipeline.fogMode), "GX fog mode");
    if (pipeline.texgenCount) coverage_->observe(integration::FeatureClass::TexGen, translatedPipelineKey ^ pipeline.texgenCount, "GX texgen program");
    if (translatedEntry_->lit) coverage_->observe(integration::FeatureClass::Lighting, translatedPipelineKey, "GX lighting");
  }
  const auto source = translate_source_primitive(primitive);
  const auto drawViewport=translate_viewport();
  const auto drawScissor=translate_scissor();
  auto pipelineForDraw=pipeline;
  auto gpuPipelineForDraw=translatedEntry_->gpuPipeline;
#if defined(AURORA_VITA_RENDERER_GXM)
  // GXM region clipping is tile-granular, so partial GX scissors still need
  // the exact fragment-side test.  For a full-target scissor that test can
  // never reject a pixel.  Omitting its discard keeps the draw in the opaque
  // HSR path instead of forcing SGX into a discard/punch-through pass.
  // The native display surface stays 960x544 even when the logical GX EFB is
  // rasterized at a smaller extent.  On the default EFB, pixels outside that
  // internal extent are not part of GX rendering and are not sampled by the
  // following GXCopyDisp, so the internal extent is the semantic full target.
  const bool fullTargetScissor=scissor_covers_full_target(
      drawScissor,renderer_->target_is_default(),
      renderer_->target_width(),renderer_->target_height(),
      aurora::vita::render_size::g_renderWidth,aurora::vita::render_size::g_renderHeight);
  if(fullTargetScissor) {
    pipelineForDraw.fragmentScissor=false;
    gpuPipelineForDraw.fragmentScissor=false;
  }
#endif
  translatedVertexState_.currentPnMatrix=static_cast<uint8_t>(std::min<u32>(
      aurora::gx::g_gxState.currentPnMtx,translatedVertexState_.postexMatrices.size()-1));
  gfx::FixedVertexReject fixedReject=gfx::FixedVertexReject::UnsupportedFeatures;
  bool fixedCandidate=false;
  if(!staticGeometry_)fixedReject=gfx::FixedVertexReject::NoCache;
  else if(translatedEntry_->lit&&!allowLitFixedVertexGpu_)fixedReject=gfx::FixedVertexReject::LitDisabled;
  else if(vertexCount<48)fixedReject=gfx::FixedVertexReject::SmallDraw;
  else if(rawIndices!=nullptr||indexCount!=0)fixedReject=gfx::FixedVertexReject::IndexedDraw;
  else if(source==gfx::SourcePrimitive::Lines||source==gfx::SourcePrimitive::LineStrip||source==gfx::SourcePrimitive::Points)
    fixedReject=gfx::FixedVertexReject::Primitive;
  else if(gfx::supports_fixed_vertex_gpu(pipeline,layout,translatedVertexState_))fixedCandidate=true;
  if(telemetry_){
    if(fixedCandidate)telemetry_->fixed_vertex_candidate(vertexCount);
    else telemetry_->fixed_vertex_reject(fixedReject,vertexCount);
  }
  // The GPU vertex path copies only the state its generated shader can consume.
  // Indexed-PN and lit draws include their position/normal palettes and lights;
  // unsupported bump/dynamic-tex-matrix cases remain on the CPU path.
  if(!translatedVertexStateValid_||aurora::gx::g_gxState.stateDirty||
     (translatedVertexStateLightweight_&&!fixedCandidate)){
    const auto statePhase=fixedCandidate?gfx::TelemetryPhase::StateVertexLightweight:
                                         gfx::TelemetryPhase::StateVertexFull;
    gfx::ScopedTelemetryPhase phase(telemetry_,statePhase,gfx::TelemetryPhase::StateTranslate);
    if(fixedCandidate) {
      translate_fixed_vertex_state(translatedVertexState_,translatedUniforms_,translatedEntry_->gpuPipeline);
      translatedVertexStateLightweight_=true;
    } else {
      translate_vertex_state(translatedVertexState_,translatedUniforms_,pipeline,layout);
      translatedVertexStateLightweight_=false;
    }
    if(++translatedUniformGeneration_==0)translatedUniformGeneration_=1;
    if(telemetry_)telemetry_->frontend_vertex_state_build(fixedCandidate);
    translatedVertexStateValid_=true;
  } else if(telemetry_)telemetry_->frontend_vertex_state_reuse();
  const auto& vertexState=translatedVertexState_;
  auto& uniforms=translatedUniforms_;
  const gfx::StaticGeometryCache::Entry* gpuGeometry=nullptr;
  uint64_t fixedPipelineKey=0;
  if(fixedCandidate){
#if defined(__vita__)
    static unsigned debugGpuDraws=0;
    const bool debugGpu=telemetry_&&telemetry_->split_vertex_phases()&&debugGpuDraws++<4;
    if(debugGpu)AURORA_VITA_DIAGF("[aurora-vita] gpu_vertex_probe begin count=%u primitive=%u key=%llx\n",vertexCount,primitive,static_cast<unsigned long long>(translatedPipelineKey));
#endif
    const uint64_t gpuPipelineDescKey=gfx::pipeline_key(gpuPipelineForDraw);
    auto key=fixedPipelineKeys_.find(gpuPipelineDescKey);
    if(key!=fixedPipelineKeys_.end()&&renderer_->pipelines().find(key->second))fixedPipelineKey=key->second;
    else{
      gfx::ScopedTelemetryPhase phase(telemetry_,gfx::TelemetryPhase::PipelineResolve);
      fixedPipelineKey=renderer_->create_pipeline(gpuPipelineForDraw);
      if(fixedPipelineKey)fixedPipelineKeys_[gpuPipelineDescKey]=fixedPipelineKey;
    }
#if defined(__vita__)
    if(debugGpu)AURORA_VITA_DIAGF("[aurora-vita] gpu_vertex_probe pipeline=%llx\n",static_cast<unsigned long long>(fixedPipelineKey));
#endif
    if(fixedPipelineKey){
      // A warm geometry hit still queues a reference to this program. Protect
      // it from cache eviction until the current command batch is executed.
      renderer_->pipelines().pin(fixedPipelineKey);
      gfx::ScopedTelemetryPhase phase(telemetry_,gfx::TelemetryPhase::GeometryCache);
      const auto before=staticGeometry_->hits();
      gpuGeometry=staticGeometry_->get(rawVertices,rawBytes,vertexCount,source,layout,translatedEntry_->gpuPipeline,vertexState,telemetry_,stableSource);
#if defined(__vita__)
      if(debugGpu)AURORA_VITA_DIAGF("[aurora-vita] gpu_vertex_probe geometry=%p entries=%u\n",static_cast<const void*>(gpuGeometry),static_cast<unsigned>(staticGeometry_->size()));
#endif
      if(gpuGeometry&&telemetry_)telemetry_->gpu_geometry(staticGeometry_->hits()!=before,gpuGeometry->vertexCount);
    }
  }
  if(!gpuGeometry&&translatedVertexStateLightweight_) {
    // A shader/cache miss or a full geometry budget can reject a GPU candidate.
    // The CPU fallback needs its complete matrices, lights and texgen state.
    gfx::ScopedTelemetryPhase phase(telemetry_,gfx::TelemetryPhase::StateVertexFallback,
                                    gfx::TelemetryPhase::StateTranslate);
    translate_vertex_state(translatedVertexState_,translatedUniforms_,pipeline,layout);
    translatedVertexStateLightweight_=false;
    if(++translatedUniformGeneration_==0)translatedUniformGeneration_=1;
    if(telemetry_)telemetry_->frontend_vertex_state_fallback();
  }
  const auto expansion = translate_primitive_expansion(translate_line_mode(primitive));
  const auto gpuLayout=gfx::gpu_vertex_layout(gfx::pipeline_texcoord_mask(pipeline),gfx::pipeline_raster_color_mask(pipeline));
  const size_t gpuStride=gpuLayout.count?gpuLayout.attributes[0].stride:sizeof(gfx::GpuVertex);
  const auto footprint = gfx::estimate_draw_footprint(source,vertexCount,indexCount,gpuStride);
#if defined(AURORA_VITA_RENDERER_GXM)
  const bool useStreamed=!gpuGeometry&&!(telemetry_&&telemetry_->split_vertex_phases())&&
      source!=gfx::SourcePrimitive::Lines&&source!=gfx::SourcePrimitive::LineStrip&&
      source!=gfx::SourcePrimitive::Points;
#else
  // Keep the VitaGL comparison on the older correctness-first path.  Native
  // GXM has hardware-validated direct streaming, while the current VitaGL
  // build renders the 2D frontend correctly but corrupts large 3D draws after
  // they enter prepare_streamed_draw_into().  PreparedDraw also makes the
  // existing geometry diagnostics available if this A/B does not restore 3D.
  const bool useStreamed=false;
#endif
  const bool exactTriangleDedup=!useStreamed&&source==gfx::SourcePrimitive::Triangles&&
      rawIndices==nullptr&&indexCount==0;
  if(!gpuGeometry&&telemetry_)telemetry_->cpu_fallback(vertexCount);
  if(!footprint.valid){
    result.drawError=gfx::PrepareDrawError::TooManyVertices;
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
        AURORA_VITA_DIAGF("[aurora-vita] stream_rollover total=%llu syncs=%llu slot=%u gpu_stride=%u next_vtx=%u next_idx=%u\n",
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
                                          useStreamed?"prepare_streamed_draw failed":"prepare_draw failed");
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
  std::array<TextureResolveStamp,gfx::MaxTextures> currentTextureStamps{};
  const uint8_t textureMask=translatedEntry_->textureMask;
  const auto white = white_texture();
  bool reuseResolvedTextures=resolvedTextureBindingsValid_&&resolvedTextureMask_==textureMask&&
                             resolvedVolatileTextureMask_==0;
  for(unsigned slot=0;slot<gfx::MaxTextures;++slot) {
    if((textureMask&(1u<<slot))==0)continue;
    currentTextureStamps[slot]=texture_resolve_stamp(slot);
    if(reuseResolvedTextures&&currentTextureStamps[slot]!=resolvedTextureStamps_[slot])
      reuseResolvedTextures=false;
  }
  if(telemetry_)telemetry_->frontend_texture_binding(reuseResolvedTextures);
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
#if !defined(AURORA_VITA_NO_DIAGNOSTICS)
      const auto statsBeforeTexture = telemetry_ ? renderer_->stats() : gfx::FrameStats{};
#endif
      const auto handle = renderer_->create_texture(translated.texture);
#if !defined(AURORA_VITA_NO_DIAGNOSTICS)
      if (telemetry_) {
        const auto statsAfterTexture = renderer_->stats();
        const uint32_t hits = statsAfterTexture.textureHits - statsBeforeTexture.textureHits;
        const uint32_t misses = statsAfterTexture.textureMisses - statsBeforeTexture.textureMisses;
        const uint32_t uploads = statsAfterTexture.textureUploads - statsBeforeTexture.textureUploads;
        for (uint32_t i = 0; i < hits; ++i) telemetry_->texture(true, false, 0);
        for (uint32_t i = 0; i < misses; ++i) telemetry_->texture(false, uploads != 0, uploads ? translated.texture.dataSize : 0);
      }
#endif
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
  resolvedTextureStamps_=currentTextureStamps;
  resolvedTextureMask_=textureMask;
  resolvedVolatileTextureMask_=volatileTextureMask;
  resolvedFallbackTextureMask_=result.fallbackTextureMask;
  resolvedTextureWarnings_=static_cast<SubmitWarning>(static_cast<uint8_t>(result.warnings)&
    (static_cast<uint8_t>(SubmitWarning::MissingTextureFallback)|static_cast<uint8_t>(SubmitWarning::DynamicCopyFallback)));
  resolvedTextureBindingsValid_=true;
  if(++resolvedTextureGeneration_==0)resolvedTextureGeneration_=1;
  }
#if defined(AURORA_VITA_RENDERER_GXM)
  // Keep the shader-side direct-sample specialization available, but do not
  // select it automatically yet. Hardware validation showed missing in-match
  // HUD elements while this path was active. The exact GX sampling path is the
  // correctness baseline until direct sampling can be constrained by stronger
  // texcoord/sampler invariants.
  pipelineForDraw.nativeTextureWrapMask=0;
#endif
#if defined(__vita__) && !defined(AURORA_VITA_RENDERER_GXM)
  // Diagnose Strikers' full-screen EFB consumer without changing sampler or
  // shader semantics. The visible vertical seams are regularly spaced, so
  // capture the per-draw screen/UV ranges and the exact guest sampler state.
  if(!gpuGeometry&&!useStreamed&&!prepared.vertices.empty()){
    static unsigned fullFrameEfbReports=0;
    for(unsigned slot=0;slot<gfx::MaxTextures&&fullFrameEfbReports<48;++slot){
      const auto& binding=bindings[slot];
      if(binding.source!=gfx::TextureSource::Efb||!binding.texture)continue;
      uint32_t physicalW=0,physicalH=0;
      if(!renderer_->efb().dimensions(binding.texture,physicalW,physicalH))continue;
      if(physicalW!=renderer_->target_width()||physicalH!=renderer_->target_height())continue;
      const auto& loaded=aurora::gx::g_gxState.loadedTextures[slot];
      const auto& scale=aurora::gx::g_gxState.texCoordScales[slot];
      float minX=prepared.vertices[0].position[0],maxX=minX;
      float minY=prepared.vertices[0].position[1],maxY=minY;
      float minS=prepared.vertices[0].texcoord[slot][0],maxS=minS;
      float minT=prepared.vertices[0].texcoord[slot][1],maxT=minT;
      for(const auto& v:prepared.vertices){
        minX=std::min(minX,v.position[0]);maxX=std::max(maxX,v.position[0]);
        minY=std::min(minY,v.position[1]);maxY=std::max(maxY,v.position[1]);
        minS=std::min(minS,v.texcoord[slot][0]);maxS=std::max(maxS,v.texcoord[slot][0]);
        minT=std::min(minT,v.texcoord[slot][1]);maxT=std::max(maxT,v.texcoord[slot][1]);
      }
      ++fullFrameEfbReports;
      AURORA_VITA_DIAGF(
        "[aurora-vita] efb_sample n=%u pipe=%llx slot=%u verts=%u logical=%ux%u physical=%ux%u "
        "wrap=%u,%u filter=%u,%u tcscale=%u,%u pos=[%.4f,%.4f]x[%.4f,%.4f] uv=[%.6f,%.6f]x[%.6f,%.6f]\n",
        fullFrameEfbReports,static_cast<unsigned long long>(translatedPipelineKey),slot,
        static_cast<unsigned>(prepared.vertices.size()),loaded.width(),loaded.height(),physicalW,physicalH,
        static_cast<unsigned>(binding.sampler.wrapS),static_cast<unsigned>(binding.sampler.wrapT),
        static_cast<unsigned>(binding.sampler.minFilter),static_cast<unsigned>(binding.sampler.magFilter),
        static_cast<unsigned>(scale.scaleS)+1u,static_cast<unsigned>(scale.scaleT)+1u,
        minX,maxX,minY,maxY,minS,maxS,minT,maxT);
    }
  }
#endif
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
#if !defined(AURORA_VITA_NO_DIAGNOSTICS)
  const auto statsBeforeEnqueue = telemetry_ ? renderer_->stats() : gfx::FrameStats{};
#endif
  uint64_t resolvedPipelineKey = 0;
  if(gpuGeometry){
    resolvedPipelineKey=fixedPipelineKey;
  }else {
    const auto preparedPrimitive=useStreamed?streamed.primitive:prepared.primitive;
    const bool preparedClipSpace=useStreamed?streamed.positionIsClipSpace:prepared.positionIsClipSpace;
    if (queuedPipelineValid_ && translatedPipelineKey == queuedTranslatedPipelineKey_ &&
        preparedPrimitive == queuedPrimitive_ &&
        preparedClipSpace == queuedPositionIsClipSpace_ &&
        pipelineForDraw.fragmentScissor == queuedFragmentScissor_ &&
        pipelineForDraw.nativeTextureWrapMask == queuedNativeTextureWrapMask_) {
      resolvedPipelineKey = queuedResolvedPipelineKey_;
      if(telemetry_)telemetry_->frontend_queued_pipeline(true);
    } else {
      if(telemetry_)telemetry_->frontend_queued_pipeline(false);
      resolvedPipelineKey = gfx::resolve_draw_pipeline(*renderer_,preparedPrimitive,preparedClipSpace,pipelineForDraw,telemetry_);
      if (resolvedPipelineKey) {
        queuedTranslatedPipelineKey_ = translatedPipelineKey;
        queuedResolvedPipelineKey_ = resolvedPipelineKey;
        queuedPrimitive_ = preparedPrimitive;
        queuedPositionIsClipSpace_ = preparedClipSpace;
        queuedFragmentScissor_ = pipelineForDraw.fragmentScissor;
        queuedNativeTextureWrapMask_ = pipelineForDraw.nativeTextureWrapMask;
        queuedPipelineValid_ = true;
      }
    }
  }
  if(telemetry_)telemetry_->frontend_draw_path(gpuGeometry,useStreamed);
  bool enqueued=false;
  if(gpuGeometry){
    gfx::ScopedTelemetryPhase phase(telemetry_,gfx::TelemetryPhase::CommandBuild);
    fixedVertexUniforms_.push_back(gfx::fixed_vertex_uniforms(translatedEntry_->gpuPipeline,vertexState));
    gfx::DrawPacket& packet=stream_.emplace_draw_fast();
    packet.pipelineKey=resolvedPipelineKey;packet.vertices=gpuGeometry->vertices;packet.indices=gpuGeometry->indices;
    packet.vertexCount=gpuGeometry->vertexCount;packet.indexCount=gpuGeometry->indexCount;
    stream_.bind_state(packet,uniforms,bindings,translatedUniformGeneration_,resolvedTextureGeneration_);
    packet.viewport=drawViewport;packet.scissor=drawScissor;
    packet.fixedVertexUniforms=&fixedVertexUniforms_.back();
    enqueued=true;
    queuedPipelineValid_=false;
  }else if(useStreamed) {
    enqueued=gfx::enqueue_streamed_draw(stream_,streamed,resolvedPipelineKey,uniforms,
                         drawViewport,drawScissor,bindings,&error,
                         translatedUniformGeneration_,resolvedTextureGeneration_);
  }else enqueued=gfx::enqueue_draw(*renderer_, *arena_, stream_, prepared, pipelineForDraw, uniforms,
                         drawViewport, drawScissor, bindings, &error, telemetry_,
                         resolvedPipelineKey,translatedUniformGeneration_,resolvedTextureGeneration_);
  if(!enqueued) {
    result.drawError = error;
    if (coverage_) coverage_->unsupported(static_cast<uint64_t>(error), "enqueue_draw failed");
    if (telemetry_) { telemetry_->arena_overflow(); telemetry_->unsupported(); }
    if (strictUnsupported_) strictFailed_ = true;
    return result;
  }
#if !defined(AURORA_VITA_NO_DIAGNOSTICS)
  if (telemetry_) {
    const auto statsAfterEnqueue = renderer_->stats();
    const uint32_t hitDelta = statsAfterEnqueue.pipelineHits - statsBeforeEnqueue.pipelineHits;
    const uint32_t missDelta = statsAfterEnqueue.pipelineMisses - statsBeforeEnqueue.pipelineMisses;
    for (uint32_t i = 0; i < hitDelta; ++i) telemetry_->pipeline(true);
    for (uint32_t i = 0; i < missDelta; ++i) telemetry_->pipeline(false);
  }
#endif
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
