#pragma once
#include <cstddef>
#include <cstdint>
#include "vita_log.hpp"
#include "gfx/vita_gfx_types.hpp"
#include "gfx/vita_telemetry.hpp"
#include "integration/vita_feature_coverage.hpp"
#include "integration/vita_frame_trace.hpp"
#include "gfx/vita_memory_budget.hpp"
#include "gfx/vita_native_assets.hpp"
namespace aurora::vita::gfx { class Renderer; }
namespace aurora::vita::gxbridge { class DrawSink; }
namespace aurora::vita {
enum class InitFailure : uint8_t {
  None = 0,
  ShaderCompilerMissing,
  ShaderCompilerLoadFailed,
  VitaGlInitFailed,
  RendererInitFailed,
  DrawSinkInitFailed,
};
struct PerformanceSnapshot {
  bool completedFrame=false;
  uint64_t frameIndex=0;
  uint64_t frameUs=0;
  uint64_t rendererCpuFrameUs=0;
  uint64_t displayQueueLastUs=0;
  uint64_t displayQueueAverageUs=0;
  uint64_t displayQueueMaxUs=0;
  uint64_t displayQueueSamples=0;
  uint32_t displayQueueBlockedPercent=0;
  bool gpuBackpressureLikely=false;
  bool nativeTimingsSampled=false;
  uint64_t nativePipelineUs=0;
  uint64_t nativeTextureUs=0;
  uint64_t nativeDrawUs=0;
  uint64_t nativeFragmentPrepareHits=0;
  uint64_t nativeFragmentPrepareMisses=0;
  uint32_t nativeSceneCount=0;
  uint32_t nativeEfbCopies=0;
  uint64_t nativeEfbEndSceneUs=0;
  uint64_t nativeEfbTransferSubmitUs=0;
  uint64_t nativeEfbTransferWaitUs=0;
  uint64_t nativeEfbCpuFixupUs=0;
  uint32_t nativeDepthLoadScenes=0;
  uint32_t nativeDepthStoreScenes=0;
  uint32_t nativeDepthlessScenes=0;
  uint32_t nativeFinishCalls=0;
  std::array<uint64_t,gfx::FinishReasonCount> nativeFinishReasonCalls{},nativeFinishReasonWaitUs{};
  uint32_t nativeScissorFreeDraws=0;
  uint32_t nativePipelineSetters=0,nativePipelineSettersSkipped=0,nativeUniformUploadCalls=0;
  uint64_t nativeUniformUploadBytes=0;
  uint64_t batchCandidates=0,batchMerged=0,batchRejectedState=0,batchRejectedIndices=0;
  uint64_t staticGeometryHits=0;
  uint64_t staticGeometryMisses=0;
  uint64_t staticGeometryLookupFallbacks=0;
  size_t staticGeometryBytes=0;
  size_t staticGeometryEntries=0;
  bool geometryPreflightEnabled=false;
  uint64_t geometryPreflightRejects=0;
  bool fixedUniformPoolEnabled=false;
  uint64_t fixedUniformPoolAllocations=0;
  uint64_t fixedUniformPoolReuses=0;
  uint64_t fixedUniformPoolFallbacks=0;
  size_t fixedUniformPoolBytes=0;
  bool shaderRuntimeCompilationEnabled=false;
  uint64_t shaderRuntimeCompiles=0;
  uint64_t shaderRuntimeCompileUs=0;
  uint64_t shaderCompileBlockedMisses=0;
  uint32_t shaderDiskCacheHits=0;
  uint32_t shaderDiskCacheMisses=0;
  // Cumulative wall time in top-level GX command processing (Aurora's share of
  // the frame when the GX worker is off). Consumers diff consecutive values.
  uint64_t gxProcessTotalUs=0;
  uint64_t producerWaitUs=0,consumerWaitUs=0;
  uint64_t preparedListHits=0,preparedListMisses=0,preparedListRejected=0;
  uint64_t poolBusyFallbacks=0;
  gfx::FrameTelemetry frontend{};
  gfx::NativeAssetStats nativeAssets{};
  // gxm_disable bit 0x100 diagnostic: GPU time per scene of the last frame.
  uint32_t diagSceneGpuUs[4]{};
  bool core3Available=false;
  bool core3BudgetConfigured=false;
  bool core3TelemetryValid=false;
  bool core3DispatchAllowed=false;
  uint32_t core3TargetPercent=0;
  uint32_t core3LastTotalPercentX100=0;
  uint64_t core3ShortCreditUs=0;
  uint64_t core3LongCreditUs=0;
  uint64_t core3Chunks=0;
  uint64_t core3Denied=0;
  uint64_t core3TelemetryFailures=0;
  uint64_t core3Overruns=0;
  uint64_t core3TotalChunkUs=0;
  uint32_t core3MaxChunkUs=0;
  uint64_t vertexParallelCalls=0;
  uint64_t vertexParallelDynamicCalls=0;
  uint64_t vertexParallelTotalWallUs=0;
  uint64_t vertexParallelCallerWaitUs=0;
  uint64_t vertexLaneItems[4]{};
  uint64_t vertexLaneChunks[4]{};
  uint64_t vertexLaneWorkUs[4]{};
};
struct CompletedMemorySnapshot {
  // Last successful CPU end_frame; does not claim GPU completion. A failed frame
  // retains the previous value. Initialization/shutdown reset validity.
  bool completedFrame=false;
  uint64_t frameIndex=0;
  gfx::MemoryBudgetSnapshot budget{};
};
using ParallelRangeTask = bool (*)(void* context, size_t begin, size_t end, uint32_t lane) noexcept;
struct BackendConfig {
  uint32_t width=960,height=544;
  // Zero follows the display extent, preserving the validated raster scale.
  // Reduced resolution is opt-in and must be checked with GXCopyTex shadows.
  uint32_t render_width=0,render_height=0;
  // Aurora never uses vitaGL immediate mode, so reserving a large legacy pool
  // only steals memory from textures, EFBs and the Wii guest runtime.
  uint32_t vgl_legacy_pool_size=0;
  // If any explicit pool size is non-zero, Aurora uses
  // vglInitWithCustomSizes instead of letting vglInitExtended consume every
  // currently-free CDRAM/PHYCONT page. This is important for ports that have
  // their own long-lived console-memory arenas beside vitaGL.
  uint32_t vgl_ram_pool_size=0;
  uint32_t vgl_cdram_pool_size=0;
  uint32_t vgl_phycont_pool_size=0;
  uint32_t vgl_cdlg_pool_size=0;
  uint32_t vgl_ram_threshold=16*1024*1024;
  uint32_t vgl_circular_pool_size=32*1024*1024;
  uint32_t vgl_display_buffer_count=3;
  bool vgl_scratch_dynamic=true;
  bool vgl_scratch_stream=true;
  size_t texture_cache_budget=24*1024*1024;
  bool wait_vblank=true;
  size_t stream_vertex_bytes=4*1024*1024;
  size_t stream_index_bytes=1024*1024;
  uint32_t stream_slots=3;
  // The render thread remains the sole graphics owner. These workers only run
  // CPU-side preparation. A third helper may exist on CPU3, but ports can keep
  // both renderer and game jobs capped to the original CPU0-2 topology until a
  // quota-aware scheduler is enabled.
  uint32_t cpu_worker_threads=2;
  // New dispatch is opt-in until device comparisons. false restores prefix dispatch.
#if defined(AURORA_VITA_DISTINCT_CPU_CORES) && AURORA_VITA_DISTINCT_CPU_CORES
  bool cpu_distinct_core_dispatch=true;
#else
  bool cpu_distinct_core_dispatch=false;
#endif
#if defined(AURORA_VITA_GXM_IMMEDIATE_DRAW_VIEW) && AURORA_VITA_GXM_IMMEDIATE_DRAW_VIEW
  bool gxm_immediate_draw_view=true;
#else
  bool gxm_immediate_draw_view=false;
#endif
  // Allowed prefix of caller+helper identities for decode/transform. Zero
  // permits all configured helpers. Distinct dispatch filters this prefix;
  // excluded helpers are never replaced with later helpers outside the cap.
  // Caller on CPU2 with cap=2 therefore executes serially in distinct mode.
  uint32_t cpu_renderer_execution_lanes=0;
  // Maximum caller+worker lanes used by the public game-side parallel_for().
  // Keeping this at 3 while probing a fourth lane makes CPU3 probe-only.
  uint32_t cpu_game_execution_lanes=0;
  // CPU3 is admitted only through the quota-aware dynamic scheduler. Creating
  // the worker with this disabled leaves it probe-only even when lane caps are 4.
  bool cpu_core3_budget_enabled=false;
  uint32_t cpu_core3_max_total_percent=70;
  uint32_t cpu_core3_guard_percent=5;
  uint32_t cpu_core3_window_ms=100;
  uint32_t cpu_core3_long_window_ms=1000;
  uint32_t cpu_core3_chunk_target_us=250;
  uint32_t cpu_core3_sample_period_us=10000;
  // Minimum useful work per CPU lane. Smaller draws stay on the render thread;
  // larger draws progressively use one or two workers as their size warrants.
  uint32_t cpu_parallel_min_vertices=512;
  // Runtime console logging for Aurora Vita itself. Silent suppresses normal
  // backend output; errors remain available through the structured failure APIs.
  RuntimeLogLevel log_level=RuntimeLogLevel::Info;
  // Master diagnostic gate; CPU3 safety measurements and cache policy remain
  // operational. Changing it requires backend shutdown/reinitialization.
  bool diagnostics_enabled=true;
  // Expensive per-draw coverage/trace instrumentation is opt-in for shipping
  // ports. Supplying any diagnostic output path still enables it automatically.
  bool diagnostics=false;
  bool performance_attribution=false; // Phase counters only, without coverage/trace.
  // Profiling only: separate CPU decode and transform passes. Leave disabled for
  // normal fused execution; enabling it changes cache locality and worker wakes.
  bool profile_split_vertex_phases=false;
  bool texture_decode_diagnostics=false;
  // Native GXM only. D16 halves depth bandwidth and tile backing size, but GX
  // exposes 24-bit Z so titles with tight depth ranges may prefer DF32.
  bool gxm_d16_depth=false;
  // Adjacent streamed triangles only; hardware A/B before enabling in a port.
  bool gxm_local_draw_batching=false;
  // Native GXM render-target scene budget. Gameplay telemetry should stay below
  // this in steady state; Strikers currently averages ~2.4 and peaks at 3.
  uint32_t gxm_scenes_per_frame=5;
  size_t gxm_parameter_buffer_bytes=4*1024*1024;
  // Experimental native-GXM A/B profile: cache immutable geometry and keep
  // eligible lit GX vertex work on the GPU. VitaGL/host keep the conservative
  // CPU defaults. Ports can still force the GXM control path with false/0.
  // Keep cached-RAM copies of CDRAM display lists between frames. Requires the
  // port to publish every write to display-list memory (see fifo.hpp).
  bool display_list_shadow=true;
  bool prepared_display_lists=false;
  bool resident_geometry_cdram=false,exact_bc1=false;
  gfx::NativeAssetReader native_asset_reader=nullptr;
  void* native_asset_context=nullptr;
  // Exact producer-side BP material-write elision; enable separately for A/B.
  bool bp_write_cache=false;
  // Prepared fragment bytes only; each draw still reserves a fresh GXM buffer.
  bool gxm_fragment_prepare_cache=false;
  // Retain CPU storage for fully rebuilt fixed-vertex snapshots, opt-in.
  bool gxm_fixed_uniform_pool=false;
  // Skip speculative geometry decode when even its minimum size cannot fit
  // and eviction is blocked by pending GPU retirement (or disabled).
  bool gxm_geometry_preflight=false;
  // gxm-optimization bisection switches, see gfx::GxmDisableBits.
  uint32_t gxm_disable_mask=0;
#if defined(AURORA_VITA_RENDERER_GXM)
  bool gxm_lit_fixed_vertex_gpu=true;
  bool gxm_streamed_fixed_vertex_gpu=false;
  size_t static_geometry_budget=8*1024*1024;
  bool static_geometry_stable_only=false;
#else
  bool gxm_lit_fixed_vertex_gpu=false;
  bool gxm_streamed_fixed_vertex_gpu=false;
  size_t static_geometry_budget=0;
  bool static_geometry_stable_only=false;
#endif
  // Extended native-GXM fixed-vertex features. These stay opt-in at the engine
  // level so ports can hardware-A/B them independently while retaining the
  // established CPU fallback for every unsupported draw.
  bool gxm_dynamic_tex_matrix_gpu=false;
  bool gxm_bump_fixed_vertex_gpu=false;
  bool gxm_primitive_expand_gpu=false;
  // Minimum vertices for immutable display-list geometry to use the native
  // fixed-vertex GPU cache. Dynamic/untracked sources retain the conservative
  // 48-vertex floor in DrawSink regardless of this value.
  uint32_t static_geometry_min_vertices=48;
  // Null automatically resolves to ux0:data/aurora-vita/<TITLE_ID> on Vita.
  // Override only when a port intentionally owns a different writable root.
  const char* data_root_path=nullptr;
  const char* program_binary_cache_path=nullptr;
  const char* pipeline_warmup_path=nullptr;
  size_t pipeline_prewarm_limit=192;
  // GXM warm-cache mode. Preload validated GXP binaries into RAM before the
  // hot-pipeline prewarm, then optionally seal vitaShaRK so gameplay can never
  // compile a new stage. Keep sealing off for cold/training runs.
  bool gxm_preload_program_cache=false;
  size_t gxm_program_cache_preload_limit=1024;
  bool gxm_seal_shader_cache_after_prewarm=false;
  // Optional synchronous boot progress callback. Called only from initialize()
  // on the calling thread while program/pipeline caches are being prepared.
  gfx::StartupProgressCallback startup_progress=nullptr;
  void* startup_progress_user=nullptr;
  // Diagnostic only: submit at most this many GX draw packets per frame. Zero
  // disables the limit. Useful for framebuffer bisection of rendering faults.
  uint32_t diagnostic_draw_limit=0;
  bool strict_unsupported=false;
  uint32_t diagnostics_period_frames=300;
  const char* telemetry_log_path=nullptr;
  const char* coverage_log_path=nullptr;
  const char* trace_log_path=nullptr;
  size_t trace_capacity=4096;
};
bool initialize(const BackendConfig& config={}) noexcept;
InitFailure last_init_failure() noexcept;
const char* last_init_failure_detail() noexcept;
bool begin_frame() noexcept;void end_frame() noexcept;void shutdown() noexcept;
// Block until the GX worker (if running) has processed every queued command,
// before reading renderer state or the framebuffer from the game thread.
void wait_for_render_idle() noexcept;
void set_presentation_aspect(float aspect) noexcept;
// GameCube glDiscardFrame means that the completed EFB must not become the
// visible XFB.  The Vita host loop owns vglSwapBuffers(), so the GX layer uses
// this hook to suppress that one present while still finishing renderer state.
void discard_present() noexcept;
void schedule_display_clear(float r,float g,float b,float a,float depth,bool clearRgb,bool clearAlpha,bool clearDepth) noexcept;
uint64_t frame_index() noexcept;uint64_t last_frame_time_us() noexcept;uint32_t width() noexcept;uint32_t height() noexcept;
PerformanceSnapshot performance_snapshot() noexcept;
// Latest complete frame; never fences the GX worker. completedFrame is false
// before the first successful end_frame. The existing synchronous API remains.
PerformanceSnapshot completed_performance_snapshot() noexcept;
// Safe game-thread observation: a short copy, no FIFO fence or renderer access.
CompletedMemorySnapshot completed_memory_snapshot() noexcept;
// GXM only: disabling runtime compilation guarantees that cache misses never
// call vitaShaRK. Missing stages fail that draw instead of stalling to compile.
void set_runtime_shader_compilation_enabled(bool enabled) noexcept;
bool runtime_shader_compilation_enabled() noexcept;
gfx::Renderer& renderer() noexcept;
gxbridge::DrawSink& draw_sink() noexcept;
gfx::Telemetry& telemetry() noexcept;
integration::FeatureCoverage& feature_coverage() noexcept;
integration::FrameTrace& frame_trace() noexcept;
// Owner-thread only, like renderer()/draw_sink()/telemetry(). Prefer the completed API
// from the game thread when async GX is active.
gfx::MemoryBudgetSnapshot memory_budget() noexcept;
size_t invalidate_texture_source_range(uint64_t start,size_t bytes) noexcept;
// Shared Vita CPU helper. The calling thread participates as lane 0; persistent helpers may
// occupy CPU2, CPU1 and (when verified) CPU3. Jobs are synchronous: this returns
// only after all dispatched ranges complete. Nested calls fall back to caller.
bool parallel_for(size_t count,size_t minItems,ParallelRangeTask task,void* context) noexcept;
uint32_t worker_threads() noexcept;
uint32_t execution_lanes() noexcept;
uint32_t game_execution_lanes() noexcept;
bool core3_available() noexcept;
int core3_cpu_id() noexcept;
int core3_affinity_mask() noexcept;
} // namespace aurora::vita
