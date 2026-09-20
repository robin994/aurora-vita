#pragma once
#include <cstddef>
#include <cstdint>
#include "vita_log.hpp"
#include "gfx/vita_telemetry.hpp"
#include "integration/vita_feature_coverage.hpp"
#include "integration/vita_frame_trace.hpp"
#include "gfx/vita_memory_budget.hpp"
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
  uint32_t nativeSceneCount=0;
  uint32_t nativeEfbCopies=0;
  uint64_t nativeEfbEndSceneUs=0;
  uint64_t nativeEfbTransferSubmitUs=0;
  uint64_t nativeEfbTransferWaitUs=0;
  uint64_t nativeEfbCpuFixupUs=0;
  uint64_t staticGeometryHits=0;
  uint64_t staticGeometryMisses=0;
  uint64_t staticGeometryLookupFallbacks=0;
  size_t staticGeometryBytes=0;
  size_t staticGeometryEntries=0;
};
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
  // The render thread remains the sole vitaGL owner. These workers only run
  // CPU-side per-vertex decode/transform work, giving Vita three CPU lanes in
  // total with the default two workers plus the caller.
  uint32_t cpu_worker_threads=2;
  // Minimum useful work per CPU lane. Smaller draws stay on the render thread;
  // larger draws progressively use one or two workers as their size warrants.
  uint32_t cpu_parallel_min_vertices=512;
  // Runtime console logging for Aurora Vita itself. Silent suppresses normal
  // backend output; errors remain available through the structured failure APIs.
  RuntimeLogLevel log_level=RuntimeLogLevel::Info;
  // Expensive per-draw coverage/trace instrumentation is opt-in for shipping
  // ports. Supplying any diagnostic output path still enables it automatically.
  bool diagnostics=false;
  // Profiling only: separate CPU decode and transform passes. Leave disabled for
  // normal fused execution; enabling it changes cache locality and worker wakes.
  bool profile_split_vertex_phases=false;
  bool texture_decode_diagnostics=false;
  // Native GXM only. D16 halves depth bandwidth and tile backing size, but GX
  // exposes 24-bit Z so titles with tight depth ranges may prefer DF32.
  bool gxm_d16_depth=false;
  // Native GXM render-target scene budget. Gameplay telemetry should stay below
  // this in steady state; Strikers currently averages ~2.4 and peaks at 3.
  uint32_t gxm_scenes_per_frame=5;
  // Experimental native-GXM A/B profile: cache immutable geometry and keep
  // eligible lit GX vertex work on the GPU. VitaGL/host keep the conservative
  // CPU defaults. Ports can still force the GXM control path with false/0.
#if defined(AURORA_VITA_RENDERER_GXM)
  bool gxm_lit_fixed_vertex_gpu=true;
  size_t static_geometry_budget=8*1024*1024;
#else
  bool gxm_lit_fixed_vertex_gpu=false;
  size_t static_geometry_budget=0;
#endif
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
// GameCube glDiscardFrame means that the completed EFB must not become the
// visible XFB.  The Vita host loop owns vglSwapBuffers(), so the GX layer uses
// this hook to suppress that one present while still finishing renderer state.
void discard_present() noexcept;
void schedule_display_clear(float r,float g,float b,float a,float depth,bool clearRgb,bool clearAlpha,bool clearDepth) noexcept;
uint64_t frame_index() noexcept;uint64_t last_frame_time_us() noexcept;uint32_t width() noexcept;uint32_t height() noexcept;
PerformanceSnapshot performance_snapshot() noexcept;
gfx::Renderer& renderer() noexcept;
gxbridge::DrawSink& draw_sink() noexcept;
gfx::Telemetry& telemetry() noexcept;
integration::FeatureCoverage& feature_coverage() noexcept;
integration::FrameTrace& frame_trace() noexcept;
gfx::MemoryBudgetSnapshot memory_budget() noexcept;
size_t invalidate_texture_source_range(uint64_t start,size_t bytes) noexcept;
} // namespace aurora::vita
