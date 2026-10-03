#include "aurora_vita_backend.hpp"
#include "gfx/vita_cpu_workers.hpp"
#include "gfx/vita_memory_revision.hpp"
#include "gfx/vita_published_snapshot.hpp"
#include "gfx/vita_renderer.hpp"
#include "gfx/vita_vertex_decode.hpp"
#include "gfx/vita_texture_decode.hpp"
#include "vita_data_paths.hpp"
#if defined(__vita__)
#include "vita_thread_utils.hpp"
#endif
#if !defined(AURORA_VITA_RENDERER_GXM)
#include "gfx/vita_gl_util.hpp"
#endif
#include "gx/aurora_vita_draw_sink.hpp"
#include "../../lib/vita/render_size.hpp"
#if defined(MKW_TARGET_VITA)
#include "../../lib/gx/fifo.hpp"
#endif
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <type_traits>
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
#include <psp2/kernel/threadmgr/thread.h>
#if !defined(AURORA_VITA_RENDERER_GXM)
#include <vitaGL.h>
#endif
#else
#include <chrono>
#endif

namespace aurora::vita {
namespace {
BackendConfig g_config{};
gfx::PublishedSnapshot<PerformanceSnapshot> g_completedPerformance;
gfx::PublishedSnapshot<CompletedMemorySnapshot> g_completedMemory;
PerformanceSnapshot performance_snapshot_now(const gfx::MemoryBudgetSnapshot* memory=nullptr) noexcept;
bool g_initialized=false;
bool g_diagnosticsEnabled=false;
bool g_telemetryEnabled=false;
bool g_coverageEnabled=false;
bool g_traceEnabled=false;
uint64_t g_frame=0,g_start=0,g_last=0;
uint64_t g_displayQueueLastUs=0,g_displayQueueTotalUs=0,g_displayQueueMaxUs=0,g_displayQueueSamples=0;
uint64_t g_displayQueueBlockedSamples=0;
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
  if(!runtime_diagnostics_enabled())return 0;
#if defined(__vita__)
  return sceKernelGetProcessTimeWide();
#else
  using namespace std::chrono;
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
#endif
}

#if defined(__vita__)
bool path_exists(const char* path) noexcept {
  SceIoStat st{};
  return path && sceIoGetstat(path,&st)>=0 && st.st_size>0;
}

void ensure_parent_dir(const char* path) noexcept {
  if (!path) return;
  ensure_parent_directory(path);
}
#else
void ensure_parent_dir(const char*) noexcept {}
#endif

gfx::FifoProfile g_lastFifoProfile{};
uint64_t g_lastProducerWaitUs=0,g_lastConsumerWaitUs=0,g_lastWorkerFrameUs=0;
std::string g_lastInvalidations;

// With the asynchronous GX worker the renderer, DrawSink and their frame state
// are owned by the worker thread. Game-thread entry points queue their work
// behind the pending FIFO instead of touching that state concurrently.
bool gx_worker_active() noexcept {
#if defined(MKW_TARGET_VITA)
  return aurora::gx::fifo::worker_running();
#else
  return false;
#endif
}
template <class Args>
void queue_on_gx_worker(void (*task)(void*), const Args& args) noexcept {
#if defined(MKW_TARGET_VITA)
  static_assert(sizeof(Args) <= 128 && std::is_trivially_copyable_v<Args>);
  aurora::gx::fifo::run_async(task, &args, sizeof(args));
#else
  (void)task; (void)args;
#endif
}
// Last renderer failure observed by the worker, reported by the next begin_frame.
std::atomic<bool> g_workerFrameFailed{false};
// Game-thread view of discard_present() for the frame being built.
bool g_mainDiscardPresent=false;
struct RuntimeFlagsArgs { uint32_t flags=0; };
void set_runtime_flags_task(void* p) {
  if(g_drawSink)g_drawSink->set_runtime_feature_flags(static_cast<const RuntimeFlagsArgs*>(p)->flags);
}

// GxmDiagPhases: cheap per-phase averages without the diagnostic log machinery.
void accumulate_phase_profile() noexcept {
  constexpr unsigned Window=120;
  constexpr size_t N=static_cast<size_t>(gfx::TelemetryPhase::Count);
  static uint64_t sums[N]{},frameSum=0,drawSum=0,gxSum=0,lastGx=0,pipeT=0,vertT=0,texR=0,layT=0,slotW=0,doneW=0,workerF=0;static unsigned frames=0;
  const auto& f=g_telemetry.frame();
  for(size_t i=0;i<N;++i)sums[i]+=f.phaseUs[i];
  frameSum+=f.totalUs;drawSum+=f.counters.draws;pipeT+=f.counters.pipelineTranslations;vertT+=f.counters.vertexTranslations;
  texR+=f.counters.textureResolves;layT+=f.counters.layoutTranslations;
  slotW+=g_lastProducerWaitUs;doneW+=g_lastConsumerWaitUs;workerF+=g_lastWorkerFrameUs;
#if defined(MKW_TARGET_VITA)
  const uint64_t gx=aurora::gx::fifo::process_time_total_us();gxSum+=lastGx?gx-lastGx:0;lastGx=gx;
#endif
  if(++frames<Window)return;
  char line[1024];int n=std::snprintf(line,sizeof line,"frames=%u frame_us=%llu gx_us=%llu draws=%llu",frames,
      static_cast<unsigned long long>(frameSum/frames),static_cast<unsigned long long>(gxSum/frames),
      static_cast<unsigned long long>(drawSum/frames));
  n+=std::snprintf(line+n,sizeof line-size_t(n)," pipe_tr=%llu vert_tr=%llu tex_res=%llu layout_tr=%llu",
      static_cast<unsigned long long>(pipeT/frames),static_cast<unsigned long long>(vertT/frames),
      static_cast<unsigned long long>(texR/frames),static_cast<unsigned long long>(layT/frames));
  for(size_t i=0;i<N&&n>0&&n<int(sizeof line)-48;++i)if(sums[i])
    n+=std::snprintf(line+n,sizeof line-size_t(n)," %s=%llu",gfx::telemetry_phase_name(static_cast<gfx::TelemetryPhase>(i)),
        static_cast<unsigned long long>(sums[i]/frames));
  n+=std::snprintf(line+n,sizeof line-size_t(n)," worker=%u game_wait_slot=%llu game_wait_done=%llu worker_frame=%llu",
      gx_worker_active()?1u:0u,static_cast<unsigned long long>(slotW/frames),static_cast<unsigned long long>(doneW/frames),
      static_cast<unsigned long long>(workerF/frames));
  n+=std::snprintf(line+n,sizeof line-size_t(n)," inval_last_frame=[%s]",g_lastInvalidations.c_str());
  const auto path=data_path("diagnostics/phase_profile.log");
  if(!path.empty()){ensure_parent_dir(path.c_str());if(FILE* out=std::fopen(path.c_str(),"a")){std::fprintf(out,"%s\n",line);std::fclose(out);}}
  for(auto& s:sums)s=0;frameSum=drawSum=gxSum=pipeT=vertT=texR=layT=slotW=doneW=workerF=0;frames=0;
}

void emit_periodic_diagnostics() noexcept {
  if (!g_telemetryEnabled || !g_drawSink) return;
  const uint32_t period = g_config.diagnostics_period_frames;
  if (period == 0 || (g_frame % period) != 0) return;
  const bool writeConsole=runtime_log_enabled(RuntimeLogLevel::Info);
  const bool writeFile=g_config.telemetry_log_path&&*g_config.telemetry_log_path;
  if(!writeConsole&&!writeFile)return;
  const auto frameLine = g_telemetry.format_frame();
  const auto memLine = g_drawSink->memory_budget().format();
  const auto& rs=g_renderer->stats();
  char rendererLine[2048];
  std::snprintf(rendererLine,sizeof(rendererLine),
      "[AURORA-VITA][RENDERER] frame=%llu display=%ux%u internal=%ux%u scenes=%u sampled=%u "
      "submit_pipeline_us=%llu submit_texture_us=%llu submit_draw_us=%llu display_queue_us=%llu "
      "vertex_uniform_reuse=%u fragment_uniform_reuse=%u efb_copies=%u "
      "efb_end_us=%llu efb_submit_us=%llu efb_wait_us=%llu efb_fixup_us=%llu "
      "d16=%u gpu_geometry=%u split_vertex_phases=%u "
      "depth_load_scenes=%u depth_store_scenes=%u depthless_scenes=%u finish_calls=%u "
      "scissor_free_draws=%u fifo_us=%llu fifo_bytes=%llu "
      "fifo_bp_us=%llu fifo_bp=%u fifo_cp_us=%llu fifo_cp=%u fifo_xf_us=%llu fifo_xf=%u "
      "fifo_indx_us=%llu fifo_indx=%u fifo_calldl_us=%llu fifo_calldl=%u "
      "fifo_draw_us=%llu fifo_draw=%u fifo_aurora_us=%llu fifo_aurora=%u fifo_other_us=%llu fifo_other=%u "
      "dl_calls=%u dl_bytes=%llu dl_cdram_bytes=%llu dl_copy_us=%llu "
      "gx_worker=%u game_wait_slot_us=%llu game_wait_done_us=%llu worker_frame_us=%llu",
      static_cast<unsigned long long>(g_telemetry.frame().frame),
      g_config.width,g_config.height,
      g_config.render_width?g_config.render_width:g_config.width,
      g_config.render_height?g_config.render_height:g_config.height,
      rs.nativeSceneCount,rs.nativeTimingsSampled?1u:0u,
      static_cast<unsigned long long>(rs.nativePipelineUs),
      static_cast<unsigned long long>(rs.nativeTextureUs),
      static_cast<unsigned long long>(rs.nativeDrawUs),
      static_cast<unsigned long long>(rs.nativeDisplayQueueAddUs),
      rs.nativeVertexUniformReuses,rs.nativeFragmentUniformReuses,rs.nativeEfbCopies,
      static_cast<unsigned long long>(rs.nativeEfbEndSceneUs),
      static_cast<unsigned long long>(rs.nativeEfbTransferSubmitUs),
      static_cast<unsigned long long>(rs.nativeEfbTransferWaitUs),
      static_cast<unsigned long long>(rs.nativeEfbCpuFixupUs),
      g_config.gxm_d16_depth?1u:0u,g_config.static_geometry_budget?1u:0u,
      g_config.profile_split_vertex_phases?1u:0u,
      rs.nativeDepthLoadScenes,rs.nativeDepthStoreScenes,rs.nativeDepthlessScenes,
      rs.nativeFinishCalls,rs.nativeScissorFreeDraws,
      static_cast<unsigned long long>(g_lastFifoProfile.processUs),
      static_cast<unsigned long long>(g_lastFifoProfile.bytes),
      static_cast<unsigned long long>(g_lastFifoProfile.us[0]),g_lastFifoProfile.count[0],
      static_cast<unsigned long long>(g_lastFifoProfile.us[1]),g_lastFifoProfile.count[1],
      static_cast<unsigned long long>(g_lastFifoProfile.us[2]),g_lastFifoProfile.count[2],
      static_cast<unsigned long long>(g_lastFifoProfile.us[3]),g_lastFifoProfile.count[3],
      static_cast<unsigned long long>(g_lastFifoProfile.us[4]),g_lastFifoProfile.count[4],
      static_cast<unsigned long long>(g_lastFifoProfile.us[5]),g_lastFifoProfile.count[5],
      static_cast<unsigned long long>(g_lastFifoProfile.us[6]),g_lastFifoProfile.count[6],
      static_cast<unsigned long long>(g_lastFifoProfile.us[7]),g_lastFifoProfile.count[7],
      g_lastFifoProfile.dlCalls,static_cast<unsigned long long>(g_lastFifoProfile.dlBytes),
      static_cast<unsigned long long>(g_lastFifoProfile.dlCdramBytes),
      static_cast<unsigned long long>(g_lastFifoProfile.dlCopyUs),
      gx_worker_active()?1u:0u,static_cast<unsigned long long>(g_lastProducerWaitUs),
      static_cast<unsigned long long>(g_lastConsumerWaitUs),static_cast<unsigned long long>(g_lastWorkerFrameUs));
  const std::string invalidationLine="[AURORA-VITA][INVALIDATE] frame="+std::to_string(g_telemetry.frame().frame)+g_lastInvalidations;
  char nativeStateLine[2048];
  int used=std::snprintf(nativeStateLine,sizeof nativeStateLine,
      "[AURORA-VITA][NATIVE_STATE] frame=%llu pipeline_setters=%u pipeline_setters_skipped=%u "
      "uniform_upload_calls=%u uniform_upload_bytes=%llu local_batch=%u shared_state_copies=%llu",
      static_cast<unsigned long long>(g_telemetry.frame().frame),rs.nativePipelineSetters,
      rs.nativePipelineSettersSkipped,rs.nativeUniformUploadCalls,
      static_cast<unsigned long long>(rs.nativeUniformUploadBytes),
      (g_drawSink->runtime_feature_flags()&gxbridge::RuntimeLocalDrawBatching)?1u:0u,
      static_cast<unsigned long long>(g_telemetry.frame().counters.sharedStateCopies));
  constexpr const char* reasons[]{"explicit","buffer_mutation","texture_mutation","resource_destroy",
      "readback","target_mutation","stream_reuse","frame_discard"};
  static_assert(std::size(reasons)==gfx::FinishReasonCount);
  for(size_t i=0;i<gfx::FinishReasonCount&&used>0&&used<int(sizeof nativeStateLine)-160;++i)
    used+=std::snprintf(nativeStateLine+used,sizeof nativeStateLine-size_t(used),
        " finish_%s_calls_total=%llu finish_%s_us_total=%llu",reasons[i],
        static_cast<unsigned long long>(rs.nativeFinishReasonCalls[i]),reasons[i],
        static_cast<unsigned long long>(rs.nativeFinishReasonWaitUs[i]));
  if(writeConsole)
    AURORA_VITA_LOG_INFO("%s\n%s\n%s\n%s\n%s\n",frameLine.c_str(),rendererLine,memLine.c_str(),invalidationLine.c_str(),nativeStateLine);
  if (writeFile) {
    ensure_parent_dir(g_config.telemetry_log_path);
    g_telemetry.append_frame_log(g_config.telemetry_log_path);
    FILE* fp = std::fopen(g_config.telemetry_log_path, "ab");
    if (fp) {
      std::fwrite(rendererLine,1,std::strlen(rendererLine),fp);std::fwrite("\n",1,1,fp);
      std::fwrite(memLine.data(),1,memLine.size(),fp); std::fwrite("\n",1,1,fp);
      std::fwrite(nativeStateLine,1,std::strlen(nativeStateLine),fp);std::fwrite("\n",1,1,fp);
      std::fwrite(invalidationLine.data(),1,invalidationLine.size(),fp); std::fwrite("\n",1,1,fp); std::fclose(fp);
    }
  }
  if(g_coverageEnabled && g_config.coverage_log_path && *g_config.coverage_log_path) {
    ensure_parent_dir(g_config.coverage_log_path);
    g_coverage.write_report(g_config.coverage_log_path);
  }
}
}

extern "C" void aurora_vita_prepare_memory_write(const void* address,size_t bytes) noexcept {
  (void)address;
  (void)bytes;
#if defined(MKW_TARGET_VITA)
  // This hook is for callers that are about to recycle GPU-visible guest
  // memory.  Keep the fence separate from the post-write revision notification:
  // cache flush/store calls may happen on audio and other pthreads and must not
  // all become global GX waits.
  if(gx_worker_active()) aurora::gx::fifo::wait_idle();
#endif
}

extern "C" void aurora_vita_notify_memory_write(const void* address,size_t bytes) noexcept {
  aurora::vita::gfx::note_memory_write(address,bytes);
}

extern "C" uint32_t aurora_vita_debug_runtime_flags(void) noexcept {
  return g_drawSink?g_drawSink->runtime_feature_flags():0;
}

extern "C" uint32_t aurora_vita_debug_runtime_capabilities(void) noexcept {
  return g_drawSink?g_drawSink->runtime_feature_capabilities():0;
}

extern "C" void aurora_vita_debug_set_runtime_flags(uint32_t flags) noexcept {
  if(gx_worker_active()) { queue_on_gx_worker(set_runtime_flags_task,RuntimeFlagsArgs{flags}); return; }
  if(g_drawSink)g_drawSink->set_runtime_feature_flags(flags);
}

extern "C" int aurora_vita_debug_shader_runtime_compile(void) noexcept {
  return runtime_shader_compilation_enabled()?1:0;
}

extern "C" void aurora_vita_debug_set_shader_runtime_compile(int enabled) noexcept {
  set_runtime_shader_compilation_enabled(enabled!=0);
}

extern "C" uint32_t aurora_vita_debug_build_flags(void) noexcept {
  uint32_t flags=0;
#if defined(AURORA_VITA_RENDERER_GXM)
  flags|=1u<<0;
#endif
#if defined(AURORA_VITA_GXM_DIRECT_STREAM_WRITE) && AURORA_VITA_GXM_DIRECT_STREAM_WRITE
  flags|=1u<<1;
#endif
#if defined(AURORA_VITA_GXM_DIRECT_DRAW_SUBMIT) && AURORA_VITA_GXM_DIRECT_DRAW_SUBMIT
  flags|=1u<<2;
#endif
  return flags;
}

bool initialize(const BackendConfig& c) noexcept {
  if (g_initialized) return true;
  g_config=c;
  set_runtime_diagnostics_enabled(c.diagnostics_enabled);
  g_completedPerformance.publish(PerformanceSnapshot{});
  g_completedMemory.publish(CompletedMemorySnapshot{});
  g_displayQueueLastUs=g_displayQueueTotalUs=g_displayQueueMaxUs=g_displayQueueSamples=0;
  g_displayQueueBlockedSamples=0;
  set_runtime_log_level(c.log_level);
  configure_data_root(c.data_root_path);
  g_programCachePath=c.program_binary_cache_path&&*c.program_binary_cache_path?
      c.program_binary_cache_path:data_path("program_cache");
  g_pipelineWarmupPath=c.pipeline_warmup_path&&*c.pipeline_warmup_path?
      c.pipeline_warmup_path:data_path("pipeline_hot_v1.bin");
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
  gfx::set_texture_decode_diagnostics(c.diagnostics_enabled&&c.texture_decode_diagnostics);
#if defined(__vita__) && !defined(AURORA_VITA_RENDERER_GXM)
  gfx::configure_program_binary_cache(g_programCachePath.empty()?nullptr:g_programCachePath.c_str());
#endif
#if defined(MKW_TARGET_VITA)
  gfx::gxm_disable_mask()=c.gxm_disable_mask;
#endif
  g_telemetryEnabled=c.diagnostics_enabled&&(c.diagnostics||c.telemetry_log_path||gfx::gxm_disabled(gfx::GxmDiagPhases));
  // GxmDiagPhases is the lightweight shipping-style profiling switch used by
  // Strikers.  Keep FIFO timing in the same diagnostic envelope so per-view
  // phase snapshots can attribute display-list/command-processor cost without
  // enabling full diagnostics or file telemetry.
  gfx::set_fifo_profile_enabled(g_telemetryEnabled);
#if defined(MKW_TARGET_VITA)
  aurora::gx::fifo::set_bp_write_cache_enabled(c.bp_write_cache);
  aurora::gx::fifo::set_display_list_shadow_enabled(c.display_list_shadow&&
      !gfx::gxm_disabled(gfx::GxmDisableDisplayListShadow));
#endif
  g_coverageEnabled=c.diagnostics_enabled&&(c.diagnostics||c.coverage_log_path);
  g_traceEnabled=c.diagnostics_enabled&&(c.diagnostics||c.trace_log_path);
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
    AURORA_VITA_LOG_ERROR(
        "[aurora-vita] vitaGL framebuffer resolution fallback requested=%ux%u\n",
        g_config.width,g_config.height);
  }
  AURORA_VITA_LOG_INFO(
      "[aurora-vita] vitaGL pools mode=%s ram=%llu/%llu cdram=%llu/%llu phycont=%llu/%llu circular=%u display_buffers=%u\n",
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
  gfx::RendererConfig rc{}; rc.width=c.width; rc.height=c.height;
  rc.renderWidth=renderWidth; rc.renderHeight=renderHeight;
  rc.textureBudget=c.texture_cache_budget;
  rc.displayBuffers=c.vgl_display_buffer_count;
  rc.waitVblank=c.wait_vblank;
  rc.nativeD16Depth=c.gxm_d16_depth;
  rc.nativeFragmentPrepareCache=c.gxm_fragment_prepare_cache;
  rc.nativeScenesPerFrame=std::max(c.gxm_scenes_per_frame,1u);
  rc.nativeParameterBufferBytes=c.gxm_parameter_buffer_bytes;
  // The native budget covers persistent streaming buffers as well as textures.
  rc.nativeResourceBudget=c.texture_cache_budget+c.static_geometry_budget+
      (c.stream_vertex_bytes+c.stream_index_bytes)*c.stream_slots+16u*1024u*1024u;
  rc.startupProgress=c.startup_progress;
  rc.startupProgressUser=c.startup_progress_user;
#if defined(AURORA_VITA_RENDERER_GXM)
  // Program binaries and hot-pipeline manifests are per title by default so
  // unrelated games cannot consume each other's cache entries or storage.
  rc.programBinaryCachePath=g_programCachePath.empty()?nullptr:g_programCachePath.c_str();
  rc.pipelineWarmupPath=g_pipelineWarmupPath.empty()?nullptr:g_pipelineWarmupPath.c_str();
  rc.pipelinePrewarmLimit=c.pipeline_prewarm_limit;
  rc.preloadProgramBinaryCache=c.gxm_preload_program_cache;
  rc.programBinaryPreloadLimit=c.gxm_program_cache_preload_limit;
  rc.sealRuntimeShaderCompilationAfterPrewarm=c.gxm_seal_shader_cache_after_prewarm;
#else
  rc.programBinaryCachePath=g_programCachePath.empty()?nullptr:g_programCachePath.c_str();
  rc.pipelineWarmupPath=g_pipelineWarmupPath.empty()?nullptr:g_pipelineWarmupPath.c_str();
  rc.pipelinePrewarmLimit=c.pipeline_prewarm_limit;
#endif
  g_renderer=std::make_unique<gfx::Renderer>(rc);
  if(!g_renderer->initialize()) {
    g_initFailure=InitFailure::RendererInitFailed;
    std::snprintf(g_initFailureDetail,sizeof(g_initFailureDetail),"Renderer::initialize failed: %s",g_renderer->last_error());
    g_renderer.reset();
    return false;
  }
  gfx::CpuCore3BudgetConfig core3Budget{};
  core3Budget.enabled=c.cpu_core3_budget_enabled;
  core3Budget.maxTotalPercent=c.cpu_core3_max_total_percent;
  core3Budget.guardPercent=c.cpu_core3_guard_percent;
  core3Budget.shortWindowMs=c.cpu_core3_window_ms;
  core3Budget.longWindowMs=c.cpu_core3_long_window_ms;
  core3Budget.chunkTargetUs=c.cpu_core3_chunk_target_us;
  core3Budget.samplePeriodUs=c.cpu_core3_sample_period_us;
  if (!gfx::initialize_cpu_workers(c.cpu_worker_threads, c.cpu_parallel_min_vertices,
                                   c.cpu_renderer_execution_lanes,core3Budget,c.cpu_distinct_core_dispatch)) {
    AURORA_VITA_LOG_ERROR(
        "[aurora-vita] cpu worker initialization failed; using render-thread CPU path\n");
  }
#if defined(__vita__)
  // Keep this one-shot probe independent from the normal runtime log so
  // NO_LOGS builds still leave hardware evidence of CPU3 availability.
  const auto cpuProbePath=data_path("cpu3_probe.log");
  if(!cpuProbePath.empty()) {
    SceKernelSystemInfo system{};system.size=sizeof(system);
    const int systemRc=sceKernelGetSystemInfo(&system);
    uint64_t calibrationWallUs=0,calibrationIdleRaw=0;
    uint32_t calibrationIdlePercentX100=0;
    bool calibrationComparable=false;
    if(c.cpu_core3_budget_enabled&&gfx::cpu_core3_available()&&systemRc>=0) {
      const int64_t wallBefore=sceKernelGetSystemTimeWide();
      const uint64_t idleBefore=static_cast<uint64_t>(system.cpuInfo[3].idleClock);
      aurora::vita::thread::delay_us(10000);
      SceKernelSystemInfo after{};after.size=sizeof(after);
      const int afterRc=sceKernelGetSystemInfo(&after);
      const int64_t wallAfter=sceKernelGetSystemTimeWide();
      if(afterRc>=0&&wallAfter>wallBefore&&
         static_cast<uint64_t>(after.cpuInfo[3].idleClock)>=idleBefore) {
        calibrationWallUs=static_cast<uint64_t>(wallAfter-wallBefore);
        calibrationIdleRaw=static_cast<uint64_t>(after.cpuInfo[3].idleClock)-idleBefore;
        const uint64_t tolerance=std::max<uint64_t>(100,calibrationWallUs/20u);
        calibrationComparable=calibrationIdleRaw<=calibrationWallUs+tolerance;
        if(calibrationWallUs)
          calibrationIdlePercentX100=static_cast<uint32_t>(std::min<uint64_t>(
              10000u,(calibrationIdleRaw*10000u)/calibrationWallUs));
        system=after;
      }
    }
    if(FILE* probe=c.diagnostics_enabled?std::fopen(cpuProbePath.c_str(),"w"):nullptr) {
      const auto budget=gfx::cpu_core3_budget_snapshot();
      const auto workerProbe=gfx::cpu_worker_probe_snapshot();
      std::fprintf(probe,
          "requested_workers=%u\nworkers=%u\nlanes=%u\nrenderer_cap=%u\ngame_cap=%u\n"
          "core3_available=%u\ncore3_cpu=%d\ncore3_affinity=0x%08x\n"
          "budget_enabled=%u\nbudget_target_pct=%u\nbudget_short_us=%llu\nbudget_long_us=%llu\n"
          "calibration_wall_us=%llu\ncalibration_idle_raw=%llu\ncalibration_idle_pct_x100=%u\n"
          "calibration_comparable=%u\n"
          "system_info_rc=0x%08x\nactive_cpu_mask=0x%08x\ncore3_idle_raw=%llu\n",
          c.cpu_worker_threads,gfx::cpu_worker_threads(),gfx::cpu_execution_lanes(),
          c.cpu_renderer_execution_lanes,c.cpu_game_execution_lanes,
          gfx::cpu_core3_available()?1u:0u,gfx::cpu_core3_cpu_id(),
          static_cast<unsigned>(gfx::cpu_core3_affinity_mask()),budget.configured?1u:0u,budget.targetPercent,
          static_cast<unsigned long long>(budget.shortCapacityUs),
          static_cast<unsigned long long>(budget.longCapacityUs),
          static_cast<unsigned long long>(calibrationWallUs),
          static_cast<unsigned long long>(calibrationIdleRaw),calibrationIdlePercentX100,
          calibrationComparable?1u:0u,static_cast<unsigned>(systemRc),
          systemRc>=0?static_cast<unsigned>(system.activeCpuMask):0u,
          systemRc>=0?static_cast<unsigned long long>(system.cpuInfo[3].idleClock):0ull);
      for(uint32_t i=0;i<gfx::MaxWorkerThreads;++i) {
        const auto& lane=workerProbe.lanes[i];
        std::fprintf(probe,
            "lane%u_requested_affinity=0x%08x\nlane%u_priority=0x%08x\n"
            "lane%u_wake_rc=0x%08x\nlane%u_done_rc=0x%08x\n"
            "lane%u_create_rc=0x%08x\nlane%u_start_rc=0x%08x\nlane%u_wait_rc=0x%08x\n"
            "lane%u_actual_cpu=%d\nlane%u_actual_affinity=0x%08x\nlane%u_created=%u\n",
            i+1,static_cast<unsigned>(lane.requestedAffinity),
            i+1,static_cast<unsigned>(lane.priority),
            i+1,static_cast<unsigned>(lane.wakeResult),
            i+1,static_cast<unsigned>(lane.doneResult),
            i+1,static_cast<unsigned>(lane.createResult),
            i+1,static_cast<unsigned>(lane.startResult),
            i+1,static_cast<unsigned>(lane.waitResult),
            i+1,lane.actualCpu,
            i+1,static_cast<unsigned>(lane.actualAffinity),
            i+1,lane.created?1u:0u);
      }
      std::fclose(probe);
    }
  }
#endif
  AURORA_VITA_LOG_INFO(
      "[aurora-vita] cpu topology workers=%u lanes=%u renderer_cap=%u game_cap=%u core3=%u core3_cpu=%d core3_affinity=0x%08x\n",
      gfx::cpu_worker_threads(),gfx::cpu_execution_lanes(),c.cpu_renderer_execution_lanes,
      c.cpu_game_execution_lanes,gfx::cpu_core3_available()?1u:0u,gfx::cpu_core3_cpu_id(),
      static_cast<unsigned>(gfx::cpu_core3_affinity_mask()));
  AURORA_VITA_LOG_INFO(
      "[aurora-vita] cpu3 budget enabled=%u max_total=%u guard=%u target=%u short_ms=%u long_ms=%u chunk_us=%u sample_us=%u\n",
      c.cpu_core3_budget_enabled?1u:0u,c.cpu_core3_max_total_percent,c.cpu_core3_guard_percent,
      core3Budget.maxTotalPercent>core3Budget.guardPercent?
          core3Budget.maxTotalPercent-core3Budget.guardPercent:0u,
      c.cpu_core3_window_ms,c.cpu_core3_long_window_ms,c.cpu_core3_chunk_target_us,
      c.cpu_core3_sample_period_us);
  AURORA_VITA_LOG_INFO(
      "[aurora-vita] render config display=%ux%u internal=%ux%u native_cmpr=%u direct_stream=%u scratch_dynamic=%u scratch_stream=%u gpu_vertex_stride=%u gpu_geometry_mb=%llu stream_v=%llu stream_i=%llu slots=%u\n",
      c.width,c.height,renderWidth,renderHeight,
      AURORA_VITA_NATIVE_CMPR?1u:0u,AURORA_VITA_DIRECT_STREAM_WRITE?1u:0u,
      c.vgl_scratch_dynamic?1u:0u,c.vgl_scratch_stream?1u:0u,
      static_cast<unsigned>(sizeof(gfx::GpuVertex)),
      static_cast<unsigned long long>(c.static_geometry_budget/(1024u*1024u)),
      static_cast<unsigned long long>(c.stream_vertex_bytes),
      static_cast<unsigned long long>(c.stream_index_bytes),c.stream_slots);
  g_telemetry.reset(); g_coverage.reset();
  // Keep the public frame_trace() reference valid without reserving a ring
  // when diagnostic recording is disabled.
  g_trace=std::make_unique<integration::FrameTrace>(g_traceEnabled?c.trace_capacity:0);
  g_telemetry.set_split_vertex_phases(c.diagnostics_enabled&&c.profile_split_vertex_phases);
  gxbridge::DrawSinkConfig dc{};
  dc.streaming.vertexBytes=c.stream_vertex_bytes;
  dc.streaming.indexBytes=c.stream_index_bytes;
  dc.streaming.slots=c.stream_slots;
  dc.streaming.framesInFlight=std::max<uint32_t>(1,c.vgl_display_buffer_count);
  dc.telemetry=g_telemetryEnabled ? &g_telemetry : nullptr;
  dc.coverage=g_coverageEnabled ? &g_coverage : nullptr;
  dc.trace=g_traceEnabled ? g_trace.get() : nullptr;
  dc.verboseGeometryDiagnostics=c.diagnostics_enabled&&c.diagnostics;
  dc.strictUnsupported=c.strict_unsupported;
  dc.staticGeometryBudget=c.static_geometry_budget;
#if defined(AURORA_VITA_RENDERER_GXM)
  dc.fixedUniformPoolBytes=c.gxm_fixed_uniform_pool?gfx::FixedUniformPool::MaxRetainedBytes:0;
  dc.staticGeometryBudgetPreflight=c.gxm_geometry_preflight;
#endif
  dc.staticGeometryMinVertices=c.static_geometry_min_vertices;
  dc.staticGeometryStableOnly=c.static_geometry_stable_only;
  dc.allowLitFixedVertexGpu=c.gxm_lit_fixed_vertex_gpu;
  dc.localDrawBatching=g_config.gxm_local_draw_batching;
  dc.immediateDrawView=c.gxm_immediate_draw_view;
  dc.allowStreamedFixedVertexGpu=c.gxm_streamed_fixed_vertex_gpu;
  dc.allowDynamicTexMatrixGpu=c.gxm_dynamic_tex_matrix_gpu;
  dc.allowBumpFixedVertexGpu=c.gxm_bump_fixed_vertex_gpu;
  dc.allowPrimitiveExpansionGpu=c.gxm_primitive_expand_gpu;
  dc.diagnosticDrawLimit=c.diagnostic_draw_limit;
  g_drawSink=std::make_unique<gxbridge::DrawSink>();
  if(!g_drawSink->initialize(*g_renderer,dc)){
    g_initFailure=InitFailure::DrawSinkInitFailed;
    std::snprintf(g_initFailureDetail,sizeof(g_initFailureDetail),"DrawSink::initialize failed");
    gfx::shutdown_cpu_workers();
    g_renderer->shutdown(); g_renderer.reset(); g_drawSink.reset();
    return false;
  }
  if (g_telemetryEnabled&&c.telemetry_log_path) ensure_parent_dir(c.telemetry_log_path);
  if (g_coverageEnabled&&c.coverage_log_path) ensure_parent_dir(c.coverage_log_path);
  if (g_traceEnabled&&c.trace_log_path) ensure_parent_dir(c.trace_log_path);
  g_initialized=true; g_frame=0; g_last=0;
  return true;
}

InitFailure last_init_failure() noexcept{return g_initFailure;}
const char* last_init_failure_detail() noexcept{return g_initFailureDetail;}

namespace {
bool begin_frame_now() noexcept {
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
struct NoArgs { uint8_t unused=0; };
void begin_frame_task(void*) { g_workerFrameFailed.store(!begin_frame_now(),std::memory_order_relaxed); }
} // namespace

bool begin_frame() noexcept {
  if(!g_initialized) return false;
  g_mainDiscardPresent=false;
  if(gx_worker_active()) {
    // A failure is reported one frame late; the worker skips the failed frame's
    // draws through the renderer's own failed() state meanwhile.
    queue_on_gx_worker(begin_frame_task,NoArgs{});
    return !g_workerFrameFailed.load(std::memory_order_relaxed);
  }
  return begin_frame_now();
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

namespace {
void discard_present_now() noexcept {
  g_discardPresent=true;
  // begin_frame() may already have consumed the clear that belongs to this
  // backbuffer.  Because a discarded frame does not rotate buffers, re-arm it
  // so the same draw buffer is clean before the next real frame is built.
  if(g_displayClearValid) g_pendingDisplayClear.pending=true;
}
void discard_present_task(void*) { discard_present_now(); }
struct AspectArgs { float aspect=0.f; };
void set_presentation_aspect_task(void* p) {
  if(g_renderer)g_renderer->set_presentation_aspect(static_cast<const AspectArgs*>(p)->aspect);
}
} // namespace

void discard_present() noexcept {
  if(!g_initialized) return;
  g_mainDiscardPresent=true;
  if(gx_worker_active()) { queue_on_gx_worker(discard_present_task,NoArgs{}); return; }
  discard_present_now();
}

void set_presentation_aspect(float aspect) noexcept {
  if(gx_worker_active()) { queue_on_gx_worker(set_presentation_aspect_task,AspectArgs{aspect}); return; }
  if(g_renderer)g_renderer->set_presentation_aspect(aspect);
}

namespace {
void end_frame_now() noexcept {
  g_drawSink->flush();
  g_renderer->end_frame();
  const uint64_t presentStart = now_us();
  g_renderer->present(!g_discardPresent);
  const uint64_t end = now_us();
  g_last=end-g_start;
  if(g_config.diagnostics_enabled&&!g_discardPresent) {
    const uint64_t queueUs=g_renderer->stats().nativeDisplayQueueAddUs;
    g_displayQueueLastUs=queueUs;
    g_displayQueueTotalUs+=queueUs;
    g_displayQueueMaxUs=std::max(g_displayQueueMaxUs,queueUs);
    if(queueUs>500u)++g_displayQueueBlockedSamples;
    ++g_displayQueueSamples;
  }
  if (g_telemetryEnabled) {
    g_telemetry.add_time(gfx::TelemetryPhase::Present,end-presentStart);
    g_telemetry.end_frame(g_last);
  }
  ++g_frame;
  if(g_config.diagnostics_enabled) {
    g_lastFifoProfile=gfx::fifo_profile_take();
#if defined(MKW_TARGET_VITA)
    aurora::gx::fifo::take_wait_stats(g_lastProducerWaitUs,g_lastConsumerWaitUs);
#endif
    {
      static uint64_t lastEnd=0; const uint64_t nowEnd=now_us();
      g_lastWorkerFrameUs=lastEnd?nowEnd-lastEnd:0; lastEnd=nowEnd;
    }
    g_lastInvalidations=gfx::take_pipeline_invalidation_report();
    if(g_telemetryEnabled&&gfx::gxm_disabled(gfx::GxmDiagPhases)) accumulate_phase_profile();
    emit_periodic_diagnostics();
  }
  if(!g_renderer->failed()) {
    const auto memory=g_drawSink->memory_budget();
    if(g_config.diagnostics_enabled) {
      auto completed=performance_snapshot_now(&memory);completed.completedFrame=true;
      g_completedPerformance.publish(completed);
    }
    g_completedMemory.publish(CompletedMemorySnapshot{true,g_frame,memory});
  }
}
void end_frame_task(void*) {
  end_frame_now();
  g_workerFrameFailed.store(g_renderer->failed(),std::memory_order_relaxed);
}
struct ShaderCompileArgs { bool enabled=false; };
void set_shader_compile_task(void* p) {
  if(g_renderer)g_renderer->set_runtime_shader_compilation_enabled(static_cast<const ShaderCompileArgs*>(p)->enabled);
}
struct InvalidateArgs { uint64_t start=0; size_t bytes=0; };
void invalidate_texture_task(void* p) {
  const auto& a=*static_cast<const InvalidateArgs*>(p);
  if(!g_renderer)return;
  if(g_renderer->invalidate_texture_source_range(a.start,a.bytes)&&g_drawSink)
    g_drawSink->invalidate_texture_resolve_cache();
}
} // namespace

void end_frame() noexcept {
  if(!g_initialized) return;
  if(gx_worker_active()) {
    // The producer may recycle guest resources immediately after end_frame()
    // returns. Keep GX decode/rendering asynchronous within the frame, but do
    // not let the game advance into the next frame while this frame still owns
    // guest pointers or renderer work on the consumer thread.
    aurora::gx::fifo::run_sync(end_frame_task,nullptr);
    return;
  }
  end_frame_now();
}

void wait_for_render_idle() noexcept {
#if defined(MKW_TARGET_VITA)
  if(gx_worker_active()) aurora::gx::fifo::drain_sync();
#endif
}

void shutdown() noexcept {
  if(!g_initialized) return;
#if defined(MKW_TARGET_VITA)
  // Drain and stop the GX worker before tearing down what it renders with.
  if(gx_worker_active()) aurora::gx::fifo::shutdown_worker();
#endif
  if (g_coverageEnabled && g_config.coverage_log_path) g_coverage.write_report(g_config.coverage_log_path);
  if (g_traceEnabled && g_config.trace_log_path && g_trace) g_trace->write_report(g_config.trace_log_path, 2048);
  if(g_drawSink){g_drawSink->shutdown();g_drawSink.reset();}
  gfx::shutdown_cpu_workers();
  if(g_renderer){g_renderer->shutdown();g_renderer.reset();}
  g_trace.reset();
  g_diagnosticsEnabled=false;
  g_telemetryEnabled=false;
  gfx::set_fifo_profile_enabled(false);
  g_coverageEnabled=false;
  g_traceEnabled=false;
  g_initialized=false;
  g_completedPerformance.publish(PerformanceSnapshot{});
  g_completedMemory.publish(CompletedMemorySnapshot{});
}

uint64_t frame_index() noexcept{return g_frame;}
uint64_t last_frame_time_us() noexcept{return g_last;}
uint32_t width() noexcept{return g_config.width;}
uint32_t height() noexcept{return g_config.height;}

PerformanceSnapshot completed_performance_snapshot() noexcept {return g_completedPerformance.read();}
CompletedMemorySnapshot completed_memory_snapshot() noexcept {return g_completedMemory.read();}
PerformanceSnapshot performance_snapshot() noexcept {
#if defined(MKW_TARGET_VITA)
  // Renderer statistics and cache maps belong to the GX worker: read them on
  // it (ordered after the pending FIFO) instead of racing its mutations.
  if(gx_worker_active()){
    PerformanceSnapshot out{};
    aurora::gx::fifo::run_sync([](void* p){*static_cast<PerformanceSnapshot*>(p)=performance_snapshot_now();},&out);
    return out;
  }
#endif
  return performance_snapshot_now();
}
namespace {
PerformanceSnapshot performance_snapshot_now(const gfx::MemoryBudgetSnapshot* suppliedMemory) noexcept {
  PerformanceSnapshot out{};
#if defined(MKW_TARGET_VITA)
  out.gxProcessTotalUs=aurora::gx::fifo::process_time_total_us();
#endif
  out.frameIndex=g_frame;
  out.frameUs=g_last;
  out.displayQueueLastUs=g_displayQueueLastUs;
  out.displayQueueAverageUs=g_displayQueueSamples?g_displayQueueTotalUs/g_displayQueueSamples:0;
  out.displayQueueMaxUs=g_displayQueueMaxUs;
  out.displayQueueSamples=g_displayQueueSamples;
  out.displayQueueBlockedPercent=g_displayQueueSamples?
      static_cast<uint32_t>((g_displayQueueBlockedSamples*100u)/g_displayQueueSamples):0u;
  out.gpuBackpressureLikely=out.displayQueueSamples>=30u&&out.displayQueueBlockedPercent>=10u;
  {
    const auto core3=gfx::cpu_core3_budget_snapshot();
    const auto vertex=gfx::cpu_vertex_parallel_snapshot();
    out.core3Available=gfx::cpu_core3_available();
    out.core3BudgetConfigured=core3.configured;
    out.core3TelemetryValid=core3.telemetryValid;
    out.core3DispatchAllowed=core3.dispatchAllowed;
    out.core3TargetPercent=core3.targetPercent;
    out.core3LastTotalPercentX100=core3.lastTotalPercentX100;
    out.core3ShortCreditUs=core3.shortCreditUs;
    out.core3LongCreditUs=core3.longCreditUs;
    out.core3Chunks=core3.chunks;
    out.core3Denied=core3.denied;
    out.core3TelemetryFailures=core3.telemetryFailures;
    out.core3Overruns=core3.overruns;
    out.core3TotalChunkUs=core3.totalChunkUs;
    out.core3MaxChunkUs=core3.maxChunkUs;
    out.vertexParallelCalls=vertex.calls;
    out.vertexParallelDynamicCalls=vertex.dynamicCalls;
    out.vertexParallelTotalWallUs=vertex.totalWallUs;
    out.vertexParallelCallerWaitUs=vertex.callerWaitUs;
    for(uint32_t lane=0;lane<gfx::MaxExecutionLanes;++lane) {
      out.vertexLaneItems[lane]=vertex.laneItems[lane];
      out.vertexLaneChunks[lane]=vertex.laneChunks[lane];
      out.vertexLaneWorkUs[lane]=vertex.laneWorkUs[lane];
    }
  }
  if(!g_renderer)return out;
  const auto& stats=g_renderer->stats();
  out.rendererCpuFrameUs=stats.cpuFrameUs;
  out.nativeTimingsSampled=stats.nativeTimingsSampled;
  out.nativePipelineUs=stats.nativePipelineUs;
  out.nativeTextureUs=stats.nativeTextureUs;
  out.nativeDrawUs=stats.nativeDrawUs;
  out.nativeFragmentPrepareHits=stats.nativeFragmentPrepareHits;
  out.nativeFragmentPrepareMisses=stats.nativeFragmentPrepareMisses;
  out.nativeSceneCount=stats.nativeSceneCount;
  for(unsigned i=0;i<4;++i)out.diagSceneGpuUs[i]=stats.diagSceneGpuUs[i];
  out.nativeEfbCopies=stats.nativeEfbCopies;
  out.nativeEfbEndSceneUs=stats.nativeEfbEndSceneUs;
  out.nativeEfbTransferSubmitUs=stats.nativeEfbTransferSubmitUs;
  out.nativeEfbTransferWaitUs=stats.nativeEfbTransferWaitUs;
  out.nativeEfbCpuFixupUs=stats.nativeEfbCpuFixupUs;
  out.nativeDepthLoadScenes=stats.nativeDepthLoadScenes;
  out.nativeDepthStoreScenes=stats.nativeDepthStoreScenes;
  out.nativeDepthlessScenes=stats.nativeDepthlessScenes;
  out.nativeFinishCalls=stats.nativeFinishCalls;
  out.nativeFinishReasonCalls=stats.nativeFinishReasonCalls;out.nativeFinishReasonWaitUs=stats.nativeFinishReasonWaitUs;
  out.nativeScissorFreeDraws=stats.nativeScissorFreeDraws;
  out.nativePipelineSetters=stats.nativePipelineSetters;
  out.nativePipelineSettersSkipped=stats.nativePipelineSettersSkipped;
  out.nativeUniformUploadCalls=stats.nativeUniformUploadCalls;
  out.nativeUniformUploadBytes=stats.nativeUniformUploadBytes;
  const auto& counters=g_telemetry.frame().counters;
  out.batchCandidates=counters.batchCandidates;out.batchMerged=counters.batchMerged;
  out.batchRejectedState=counters.batchRejectedState;out.batchRejectedIndices=counters.batchRejectedIndices;
  out.shaderRuntimeCompilationEnabled=g_renderer->runtime_shader_compilation_enabled();
  out.shaderRuntimeCompiles=g_renderer->runtime_shader_compiles();
  out.shaderRuntimeCompileUs=g_renderer->runtime_shader_compile_us();
  out.shaderCompileBlockedMisses=g_renderer->blocked_shader_compile_misses();
  out.shaderDiskCacheHits=g_renderer->program_cache_hits();
  out.shaderDiskCacheMisses=g_renderer->program_cache_misses();
  if(g_drawSink) {
    out.geometryPreflightEnabled=g_config.gxm_geometry_preflight;
    out.geometryPreflightRejects=g_drawSink->geometry_preflight_rejects();
    const auto pool=g_drawSink->fixed_uniform_pool_stats();
    out.fixedUniformPoolEnabled=pool.enabled;
    out.fixedUniformPoolAllocations=pool.allocations;
    out.fixedUniformPoolReuses=pool.reuses;
    out.fixedUniformPoolFallbacks=pool.fallbacks;
    out.fixedUniformPoolBytes=pool.retainedBytes;
    const auto memory=suppliedMemory?*suppliedMemory:g_drawSink->memory_budget();
    out.staticGeometryHits=memory.staticGeometryHits;
    out.staticGeometryMisses=memory.staticGeometryMisses;
    out.staticGeometryLookupFallbacks=memory.staticGeometryLookupFallbacks;
    out.staticGeometryBytes=memory.staticGeometryBytes;
    out.staticGeometryEntries=memory.staticGeometryEntries;
  }
  return out;
}
} // namespace
void set_runtime_shader_compilation_enabled(bool enabled) noexcept {
  if(gx_worker_active()) { queue_on_gx_worker(set_shader_compile_task,ShaderCompileArgs{enabled}); return; }
  if(g_renderer)g_renderer->set_runtime_shader_compilation_enabled(enabled);
}
bool runtime_shader_compilation_enabled() noexcept {
  return g_renderer&&g_renderer->runtime_shader_compilation_enabled();
}
gfx::Renderer& renderer() noexcept{return *g_renderer;}
gxbridge::DrawSink& draw_sink() noexcept{return *g_drawSink;}
gfx::Telemetry& telemetry() noexcept{return g_telemetry;}
integration::FeatureCoverage& feature_coverage() noexcept{return g_coverage;}
integration::FrameTrace& frame_trace() noexcept{return *g_trace;}
gfx::MemoryBudgetSnapshot memory_budget() noexcept{return g_drawSink ? g_drawSink->memory_budget() : gfx::MemoryBudgetSnapshot{};}
size_t invalidate_texture_source_range(uint64_t start,size_t bytes) noexcept{
  if(!g_renderer)return 0;
  // Queued behind earlier draws; the count is unknown to the caller then.
  if(gx_worker_active()) { queue_on_gx_worker(invalidate_texture_task,InvalidateArgs{start,bytes}); return 0; }
  const size_t invalidated=g_renderer->invalidate_texture_source_range(start,bytes);
  if(invalidated&&g_drawSink)g_drawSink->invalidate_texture_resolve_cache();
  return invalidated;
}

bool parallel_for(size_t count,size_t minItems,ParallelRangeTask task,void* context) noexcept {
  return gfx::cpu_parallel_for_min_lanes(count,minItems,g_config.cpu_game_execution_lanes,task,context);
}

uint32_t worker_threads() noexcept { return gfx::cpu_worker_threads(); }
uint32_t execution_lanes() noexcept { return gfx::cpu_execution_lanes(); }
bool core3_available() noexcept { return gfx::cpu_core3_available(); }
int core3_cpu_id() noexcept { return gfx::cpu_core3_cpu_id(); }
int core3_affinity_mask() noexcept { return gfx::cpu_core3_affinity_mask(); }

// benchmark.c keeps this hook weak so non-Aurora builds do not need to provide
// it.  On Vita provide a strong implementation backed by the actual CPU time
// spent in sceGxmDisplayQueueAddEntry.  Values are reported in nanoseconds to
// match the benchmark API used by desktop backends.
extern "C" void aurora_gpu_frame_time(unsigned long long* lastNs,
                                       unsigned long long* meanNs,
                                       unsigned long long* maxNs,
                                       unsigned long long* count) {
  const uint64_t meanUs=g_displayQueueSamples?g_displayQueueTotalUs/g_displayQueueSamples:0;
  if(lastNs)*lastNs=g_displayQueueLastUs*1000ull;
  if(meanNs)*meanNs=meanUs*1000ull;
  if(maxNs)*maxNs=g_displayQueueMaxUs*1000ull;
  if(count)*count=g_displayQueueSamples;
}

} // namespace aurora::vita
