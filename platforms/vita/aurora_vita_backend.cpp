#include "aurora_vita_backend.hpp"
#include "gfx/vita_cpu_workers.hpp"
#include "gfx/vita_memory_revision.hpp"
#include "gfx/vita_renderer.hpp"
#include "gfx/vita_vertex_decode.hpp"
#include "gfx/vita_texture_decode.hpp"
#include "vita_data_paths.hpp"
#include "vita_io.hpp"
#if !defined(AURORA_VITA_RENDERER_GXM)
#include "gfx/vita_gl_util.hpp"
#endif
#include "gx/aurora_vita_draw_sink.hpp"
#include "../../lib/vita/render_size.hpp"
#include <cstdio>
#include <cstring>
#include <memory>
#ifndef AURORA_VITA_NATIVE_CMPR
#define AURORA_VITA_NATIVE_CMPR 0
#endif
#ifndef AURORA_VITA_DIRECT_STREAM_WRITE
#define AURORA_VITA_DIRECT_STREAM_WRITE 0
#endif
#if defined(__vita__)
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#if !defined(AURORA_VITA_RENDERER_GXM)
#include <vitaGL.h>
#endif
#else
#include <chrono>
#endif

namespace aurora::vita {
namespace {
BackendConfig g_config{};
bool g_initialized=false;
bool g_diagnosticsEnabled=false;
bool g_telemetryEnabled=false;
bool g_coverageEnabled=false;
bool g_traceEnabled=false;
uint64_t g_frame=0,g_start=0,g_last=0;
std::unique_ptr<gfx::Renderer> g_renderer;
std::unique_ptr<gxbridge::DrawSink> g_drawSink;
gfx::Telemetry g_telemetry;
integration::FeatureCoverage g_coverage;
std::unique_ptr<integration::FrameTrace> g_trace;
std::string g_programCachePath;
std::string g_pipelineWarmupPath;
InitFailure g_initFailure=InitFailure::None;
char g_initFailureDetail[384]{};
struct PendingDisplayClear {
  bool pending=false;
  gfx::Color color{0.f,0.f,0.f,1.f};
  float depth=0.f;
  bool clearRgb=false,clearAlpha=false,clearDepth=false;
};
PendingDisplayClear g_pendingDisplayClear{};
bool g_displayClearValid=false;
bool g_discardPresent=false;

uint64_t now_us() noexcept {
#if defined(AURORA_VITA_NO_DIAGNOSTICS)
  return 0;
#else
#if defined(__vita__)
  return sceKernelGetProcessTimeWide();
#else
  using namespace std::chrono;
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
#endif
#endif
}

#if defined(__vita__)
bool path_exists(const char* path) noexcept {
  SceIoStat st{};
  return path && sceIoGetstat(path,&st)>=0 && st.st_size>0;
}

void ensure_parent_dir(const char* path) noexcept {
  ensure_parent_directory(path);
}
#else
void ensure_parent_dir(const char*) noexcept {}
#endif

void emit_periodic_diagnostics() noexcept {
  if (!g_telemetryEnabled || !g_drawSink) return;
  const uint32_t period = g_config.diagnostics_period_frames;
  if (period == 0 || (g_frame % period) != 0) return;
  const auto frameLine = g_telemetry.format_frame();
  const auto memLine = g_drawSink->memory_budget().format();
  const auto& rs=g_renderer->stats();
  char rendererLine[1088];
  std::snprintf(rendererLine,sizeof(rendererLine),
      "[AURORA-VITA][RENDERER] frame=%llu display=%ux%u internal=%ux%u scenes=%u sampled=%u "
      "submit_pipeline_us=%llu submit_texture_us=%llu submit_draw_us=%llu display_queue_us=%llu "
      "pipeline_mru_hits=%u "
      "vertex_uniform_reuse=%u fragment_uniform_reuse=%u "
      "hsr_opaque=%u hsr_discard=%u hsr_translucent=%u hsr_pass_switch=%u "
      "direct_tex_draws=%u direct_tex_slots=%u "
      "frag_tev_ops=%u frag_tex_ops=%u frag_complex=%u frag_indirect=%u frag_max_tev=%u "
      "efb_copies=%u "
      "efb_downscale_attempts=%u efb_downscale_success=%u efb_downscale_fallback=%u "
      "depth_load_scenes=%u depth_written_scenes=%u depth_readonly_scenes=%u depth_clear_skipped_loads=%u "
      "scene_end_frame=%u scene_end_target=%u scene_end_transfer=%u scene_end_display=%u scene_end_finish=%u "
      "d16=%u gpu_geometry=%u split_vertex_phases=%u",
      static_cast<unsigned long long>(g_telemetry.frame().frame),
      g_config.width,g_config.height,
      g_config.render_width?g_config.render_width:g_config.width,
      g_config.render_height?g_config.render_height:g_config.height,
      rs.nativeSceneCount,rs.nativeTimingsSampled?1u:0u,
      static_cast<unsigned long long>(rs.nativePipelineUs),
      static_cast<unsigned long long>(rs.nativeTextureUs),
      static_cast<unsigned long long>(rs.nativeDrawUs),
      static_cast<unsigned long long>(rs.nativeDisplayQueueAddUs),
      rs.pipelineMruHits,
      rs.nativeVertexUniformReuses,rs.nativeFragmentUniformReuses,
      rs.nativeHsrOpaqueDraws,rs.nativeHsrDiscardDraws,rs.nativeHsrTranslucentDraws,
      rs.nativeHsrPassSwitches,rs.nativeDirectTextureDraws,rs.nativeDirectTextureSlots,
      rs.nativeFragmentTevOps,rs.nativeFragmentTextureOps,rs.nativeFragmentComplexDraws,
      rs.nativeFragmentIndirectDraws,rs.nativeFragmentMaxTevStages,
      rs.nativeEfbCopies,
      rs.nativeEfbDownscaleAttempts,rs.nativeEfbDownscaleSuccesses,rs.nativeEfbDownscaleFallbacks,
      rs.nativeDepthLoadScenes,rs.nativeDepthWrittenScenes,rs.nativeDepthReadOnlyScenes,
      rs.nativeDepthClearSkippedLoads,
      rs.nativeSceneEndFrame,rs.nativeSceneEndTargetSwitch,rs.nativeSceneEndTransfer,
      rs.nativeSceneEndDisplayCopy,rs.nativeSceneEndFinish,
      g_config.gxm_d16_depth?1u:0u,g_config.static_geometry_budget?1u:0u,
      g_config.profile_split_vertex_phases?1u:0u);
  std::printf("%s\n%s\n%s\n", frameLine.c_str(), rendererLine, memLine.c_str());
  if (g_config.telemetry_log_path) {
    ensure_parent_dir(g_config.telemetry_log_path);
    g_telemetry.append_frame_log(g_config.telemetry_log_path);
    io::BufferedWriter writer(g_config.telemetry_log_path, true);
    if (writer.is_open()) {
      writer.write(rendererLine,std::strlen(rendererLine));writer.write("\n",1);
      writer.write(memLine.data(),memLine.size());writer.write("\n",1);writer.close();
    }
  }
}
}

extern "C" void aurora_vita_notify_memory_write(const void* address,size_t bytes) noexcept {
  aurora::vita::gfx::note_memory_write(address,bytes);
}

bool initialize(const BackendConfig& c) noexcept {
  if (g_initialized) return true;
  g_config=c;
  configure_data_root(c.data_root_path);
  g_programCachePath=c.program_binary_cache_path?c.program_binary_cache_path:data_path("program_cache");
  g_pipelineWarmupPath=c.pipeline_warmup_path?c.pipeline_warmup_path:data_path("pipeline_hot_v1.bin");
  const uint32_t renderWidth=c.render_width?c.render_width:c.width;
  const uint32_t renderHeight=c.render_height?c.render_height:c.height;
  if(!renderWidth||!renderHeight||renderWidth>c.width||renderHeight>c.height) {
    g_initFailure=InitFailure::RendererInitFailed;
    std::snprintf(g_initFailureDetail,sizeof(g_initFailureDetail),
                  "invalid Vita render extent %ux%u for display %ux%u",
                  renderWidth,renderHeight,c.width,c.height);
    return false;
  }
  aurora::vita::render_size::configure(renderWidth,renderHeight,c.width,c.height);
  gfx::set_texture_decode_diagnostics(c.texture_decode_diagnostics);
#if defined(__vita__) && !defined(AURORA_VITA_RENDERER_GXM)
  gfx::configure_program_binary_cache(g_programCachePath.empty()?nullptr:g_programCachePath.c_str());
#endif
  g_telemetryEnabled=c.diagnostics||c.telemetry_log_path;
  g_coverageEnabled=c.diagnostics||c.coverage_log_path;
  g_traceEnabled=c.diagnostics||c.trace_log_path;
  g_diagnosticsEnabled=g_telemetryEnabled||g_coverageEnabled||g_traceEnabled;
  g_initFailure=InitFailure::None;
  g_initFailureDetail[0]='\0';
#if defined(__vita__) && !defined(AURORA_VITA_RENDERER_GXM)
  // The vitaGL archive shipped by the currently supported VitaSDK probes
  // ur0:data/external/libshacccg.suprx, while vitaShaRK's canonical default
  // remains ur0:/data/libshacccg.suprx. Accept both layouts. If only the
  // canonical path exists, pre-initialize vitaShaRK; shark_init() is
  // deliberately idempotent, so vitaGL's later init observes it as online.
  constexpr const char* ShaccPrimary="ur0:/data/libshacccg.suprx";
  constexpr const char* ShaccExternal="ur0:/data/external/libshacccg.suprx";
  const bool hasPrimary=path_exists(ShaccPrimary) || path_exists("ur0:data/libshacccg.suprx");
  const bool hasExternal=path_exists(ShaccExternal) || path_exists("ur0:data/external/libshacccg.suprx");
  if(!hasPrimary && !hasExternal){
    g_initFailure=InitFailure::ShaderCompilerMissing;
    std::snprintf(g_initFailureDetail,sizeof(g_initFailureDetail),
                  "libshacccg.suprx missing; checked %s and %s",ShaccPrimary,ShaccExternal);
    return false;
  }
  if(!hasExternal && hasPrimary){
    const int sharkRc=shark_init(ShaccPrimary);
    if(sharkRc<0){
      g_initFailure=InitFailure::ShaderCompilerLoadFailed;
      std::snprintf(g_initFailureDetail,sizeof(g_initFailureDetail),
                    "shark_init(%s) failed rc=0x%08x",ShaccPrimary,static_cast<unsigned>(sharkRc));
      return false;
    }
  }
  // Bound vitaGL's transient allocator before initialization. The upstream
  // default circular pool is intentionally large (32 MiB total); console ports
  // can lower it when they already own separate streaming arenas. Scratch
  // routing is a no-op when vitaGL was built without USE_SCRATCH_MEMORY.
  if(g_config.vgl_circular_pool_size)
    vglSetCircularPoolSize(g_config.vgl_circular_pool_size);
  if(g_config.vgl_display_buffer_count>=2 && g_config.vgl_display_buffer_count<=3)
    vglSetDisplayBufferCount(static_cast<int>(g_config.vgl_display_buffer_count));
  vglSetupScratchMemory(g_config.vgl_scratch_dynamic?GL_TRUE:GL_FALSE,
                        g_config.vgl_scratch_stream?GL_TRUE:GL_FALSE);
  // Aurora always compiles vertex/fragment shaders as an immediate pair before
  // linking them. Use vitaGL's matching semantic mode so compile failures are
  // reported by glCompileShader instead of being deferred into glLinkProgram.
  // The postponed path in current vitaGL can otherwise reach SceGxm with a null
  // compiled program when a deferred shader translation fails.
  vglSetSemanticBindingMode(VGL_MODE_SHADER_PAIR);

  // vitaGL's current API does not return a success flag here. The value is
  // `res_fallback`: GL_TRUE means the requested framebuffer was clamped to
  // the maximum supported resolution, while the normal successful path for
  // 960x544 returns GL_FALSE after setting vgl_inited=GL_TRUE.
  const bool explicitPools = g_config.vgl_ram_pool_size || g_config.vgl_cdram_pool_size ||
                             g_config.vgl_phycont_pool_size || g_config.vgl_cdlg_pool_size;
  const GLboolean resolutionFallback = explicitPools
      ? vglInitWithCustomSizes(static_cast<int>(g_config.vgl_legacy_pool_size),
                               static_cast<int>(g_config.width),
                               static_cast<int>(g_config.height),
                               static_cast<int>(g_config.vgl_ram_pool_size),
                               static_cast<int>(g_config.vgl_cdram_pool_size),
                               static_cast<int>(g_config.vgl_phycont_pool_size),
                               static_cast<int>(g_config.vgl_cdlg_pool_size),
                               SCE_GXM_MULTISAMPLE_NONE)
      : vglInitExtended(static_cast<int>(g_config.vgl_legacy_pool_size),
                        static_cast<int>(g_config.width),
                        static_cast<int>(g_config.height),
                        static_cast<int>(g_config.vgl_ram_threshold),
                        SCE_GXM_MULTISAMPLE_NONE);
  if(resolutionFallback){
    std::printf("[aurora-vita] vitaGL framebuffer resolution fallback requested=%ux%u\n",
                g_config.width,g_config.height);
  }
  std::printf("[aurora-vita] vitaGL pools mode=%s ram=%llu/%llu cdram=%llu/%llu phycont=%llu/%llu circular=%u display_buffers=%u\n",
              explicitPools?"fixed":"threshold",
              static_cast<unsigned long long>(vglMemFree(VGL_MEM_RAM)),
              static_cast<unsigned long long>(vglMemTotal(VGL_MEM_RAM)),
              static_cast<unsigned long long>(vglMemFree(VGL_MEM_VRAM)),
              static_cast<unsigned long long>(vglMemTotal(VGL_MEM_VRAM)),
              static_cast<unsigned long long>(vglMemFree(VGL_MEM_SLOW)),
              static_cast<unsigned long long>(vglMemTotal(VGL_MEM_SLOW)),
              g_config.vgl_circular_pool_size,g_config.vgl_display_buffer_count);
  vglWaitVblankStart(g_config.wait_vblank?GL_TRUE:GL_FALSE);
  glViewport(0,0,g_config.width,g_config.height);
  glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND); glDisable(GL_CULL_FACE);
  glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL); glClearDepth(1.0f);
#endif
  gfx::RendererConfig rc{}; rc.width=c.width; rc.height=c.height; rc.textureBudget=c.texture_cache_budget;
  rc.displayBuffers=c.vgl_display_buffer_count;
  rc.waitVblank=c.wait_vblank;
  rc.nativeD16Depth=c.gxm_d16_depth;
  rc.nativeScenesPerFrame=std::max(c.gxm_scenes_per_frame,1u);
  // The native budget covers persistent streaming buffers as well as textures.
  rc.nativeResourceBudget=c.texture_cache_budget+c.static_geometry_budget+
      (c.stream_vertex_bytes+c.stream_index_bytes)*c.stream_slots+16u*1024u*1024u;
#if defined(AURORA_VITA_RENDERER_GXM)
  // Native Cg/GXP compilation is expensive enough to stall first-use gameplay.
  // Keep each title isolated by default; explicit paths remain available to
  // ports that intentionally own a custom writable cache root.
  rc.programBinaryCachePath=g_programCachePath.empty()?nullptr:g_programCachePath.c_str();
  rc.pipelineWarmupPath=g_pipelineWarmupPath.empty()?nullptr:g_pipelineWarmupPath.c_str();
  rc.pipelinePrewarmLimit=c.pipeline_prewarm_limit;
#else
  rc.programBinaryCachePath=c.program_binary_cache_path;
  rc.pipelineWarmupPath=c.pipeline_warmup_path;
  rc.pipelinePrewarmLimit=c.pipeline_prewarm_limit;
#endif
  g_renderer=std::make_unique<gfx::Renderer>(rc);
  if(!g_renderer->initialize()) {
    g_initFailure=InitFailure::RendererInitFailed;
    std::snprintf(g_initFailureDetail,sizeof(g_initFailureDetail),"Renderer::initialize failed: %s",g_renderer->last_error());
    g_renderer.reset();
    return false;
  }
  if (!gfx::initialize_cpu_workers(c.cpu_worker_threads, c.cpu_parallel_min_vertices)) {
    std::fprintf(stderr, "[aurora-vita] cpu worker initialization failed; using render-thread CPU path\n");
  }
  std::fprintf(stderr,
               "[aurora-vita] render config display=%ux%u internal=%ux%u native_cmpr=%u direct_stream=%u scratch_dynamic=%u scratch_stream=%u gpu_vertex_stride=%u gpu_geometry_mb=%llu stream_v=%llu stream_i=%llu slots=%u\n",
               c.width,c.height,renderWidth,renderHeight,
               AURORA_VITA_NATIVE_CMPR?1u:0u,AURORA_VITA_DIRECT_STREAM_WRITE?1u:0u,
               c.vgl_scratch_dynamic?1u:0u,c.vgl_scratch_stream?1u:0u,
               static_cast<unsigned>(sizeof(gfx::GpuVertex)),
               static_cast<unsigned long long>(c.static_geometry_budget/(1024u*1024u)),
               static_cast<unsigned long long>(c.stream_vertex_bytes),
               static_cast<unsigned long long>(c.stream_index_bytes),c.stream_slots);
  std::fprintf(stderr,"[aurora-vita] data_root=%s program_cache=%s pipeline_warmup=%s\n",
               data_root().empty()?"<disabled>":data_root().c_str(),
               g_programCachePath.empty()?"<disabled>":g_programCachePath.c_str(),
               g_pipelineWarmupPath.empty()?"<disabled>":g_pipelineWarmupPath.c_str());
  g_telemetry.reset(); g_coverage.reset(); g_trace=std::make_unique<integration::FrameTrace>(c.trace_capacity);
  g_telemetry.set_split_vertex_phases(c.profile_split_vertex_phases);
  gxbridge::DrawSinkConfig dc{};
  dc.streaming.vertexBytes=c.stream_vertex_bytes;
  dc.streaming.indexBytes=c.stream_index_bytes;
  dc.streaming.slots=c.stream_slots;
  dc.streaming.framesInFlight=std::max<uint32_t>(1,c.vgl_display_buffer_count);
  dc.telemetry=g_telemetryEnabled ? &g_telemetry : nullptr;
  dc.coverage=g_coverageEnabled ? &g_coverage : nullptr;
  dc.trace=g_traceEnabled ? g_trace.get() : nullptr;
  dc.verboseGeometryDiagnostics=c.diagnostics;
  dc.strictUnsupported=c.strict_unsupported;
  dc.staticGeometryBudget=c.static_geometry_budget;
  dc.allowLitFixedVertexGpu=c.gxm_lit_fixed_vertex_gpu;
  dc.diagnosticDrawLimit=c.diagnostic_draw_limit;
  g_drawSink=std::make_unique<gxbridge::DrawSink>();
  if(!g_drawSink->initialize(*g_renderer,dc)){
    g_initFailure=InitFailure::DrawSinkInitFailed;
    std::snprintf(g_initFailureDetail,sizeof(g_initFailureDetail),"DrawSink::initialize failed");
    gfx::shutdown_cpu_workers();
    g_renderer->shutdown(); g_renderer.reset(); g_drawSink.reset();
    return false;
  }
  if (c.telemetry_log_path) ensure_parent_dir(c.telemetry_log_path);
  if (c.coverage_log_path) ensure_parent_dir(c.coverage_log_path);
  if (c.trace_log_path) ensure_parent_dir(c.trace_log_path);
  g_initialized=true; g_frame=0; g_last=0;
  return true;
}

InitFailure last_init_failure() noexcept{return g_initFailure;}
const char* last_init_failure_detail() noexcept{return g_initFailureDetail;}

bool begin_frame() noexcept {
  if(!g_initialized) return false;
  g_discardPresent=false;
  g_start=now_us();
  if (g_telemetryEnabled) g_telemetry.begin_frame(g_frame);
  g_renderer->begin_frame();
  if(g_renderer->failed()) return false;
  if (g_pendingDisplayClear.pending) {
    g_renderer->clear_current(g_pendingDisplayClear.color,g_pendingDisplayClear.depth,
                              g_pendingDisplayClear.clearRgb,g_pendingDisplayClear.clearAlpha,
                              g_pendingDisplayClear.clearDepth);
    g_pendingDisplayClear.pending=false;
  }
  g_drawSink->begin_frame(g_frame);
  return true;
}

void schedule_display_clear(float r,float g,float b,float a,float depth,bool clearRgb,bool clearAlpha,bool clearDepth) noexcept {
  g_pendingDisplayClear.pending=true;
  g_displayClearValid=true;
  g_pendingDisplayClear.color={r,g,b,a};
  g_pendingDisplayClear.depth=depth;
  g_pendingDisplayClear.clearRgb=clearRgb;
  g_pendingDisplayClear.clearAlpha=clearAlpha;
  g_pendingDisplayClear.clearDepth=clearDepth;
}

void discard_present() noexcept {
  if(!g_initialized) return;
  g_discardPresent=true;
  // begin_frame() may already have consumed the clear that belongs to this
  // backbuffer.  Because a discarded frame does not rotate buffers, re-arm it
  // so the same draw buffer is clean before the next real frame is built.
  if(g_displayClearValid) g_pendingDisplayClear.pending=true;
}

void end_frame() noexcept {
  if(!g_initialized) return;
  g_drawSink->flush();
  g_renderer->end_frame();
  const uint64_t presentStart = now_us();
  g_renderer->present(!g_discardPresent);
  const uint64_t end = now_us();
  g_last=end-g_start;
  if (g_telemetryEnabled) {
    g_telemetry.add_time(gfx::TelemetryPhase::Present,end-presentStart);
    g_telemetry.end_frame(g_last);
  }
  ++g_frame;
  emit_periodic_diagnostics();
}

void shutdown() noexcept {
  if(!g_initialized) return;
  if (g_coverageEnabled && g_config.coverage_log_path) g_coverage.write_report(g_config.coverage_log_path);
  if (g_traceEnabled && g_config.trace_log_path && g_trace) g_trace->write_report(g_config.trace_log_path, 2048);
  if(g_drawSink){g_drawSink->shutdown();g_drawSink.reset();}
  gfx::shutdown_cpu_workers();
  if(g_renderer){g_renderer->shutdown();g_renderer.reset();}
  g_trace.reset();
  g_diagnosticsEnabled=false;
  g_telemetryEnabled=false;
  g_coverageEnabled=false;
  g_traceEnabled=false;
  g_initialized=false;
}

uint64_t frame_index() noexcept{return g_frame;}
uint64_t last_frame_time_us() noexcept{return g_last;}
uint32_t width() noexcept{return g_config.width;}
uint32_t height() noexcept{return g_config.height;}
gfx::Renderer& renderer() noexcept{return *g_renderer;}
gxbridge::DrawSink& draw_sink() noexcept{return *g_drawSink;}
gfx::Telemetry& telemetry() noexcept{return g_telemetry;}
integration::FeatureCoverage& feature_coverage() noexcept{return g_coverage;}
integration::FrameTrace& frame_trace() noexcept{return *g_trace;}
gfx::MemoryBudgetSnapshot memory_budget() noexcept{return g_drawSink ? g_drawSink->memory_budget() : gfx::MemoryBudgetSnapshot{};}
size_t invalidate_texture_source_range(uint64_t start,size_t bytes) noexcept{
  if(!g_renderer)return 0;
  const size_t invalidated=g_renderer->invalidate_texture_source_range(start,bytes);
  if(invalidated&&g_drawSink)g_drawSink->invalidate_texture_resolve_cache();
  return invalidated;
}

} // namespace aurora::vita
