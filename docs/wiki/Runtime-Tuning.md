# Runtime tuning

Runtime tuning is configured through `aurora::vita::BackendConfig`.

The defaults below include the 2026-10-02 Vita-native implementation on base `bb5147e`. The public
declarations are in `platforms/vita/aurora_vita_backend.hpp`.

## Runtime logging

`BackendConfig::log_level` controls Aurora Vita backend logging:

| Value | Behavior |
|---|---|
| `RuntimeLogLevel::Silent` | Suppresses Aurora Vita runtime console logs, shader/compiler logs, hardcoded GXM memory logs, and automatic shader-failure artifacts. Structured errors remain available through `last_init_failure_detail()` / renderer error state. Explicit telemetry/coverage/trace output paths still write because the caller requested them. |
| `RuntimeLogLevel::Error` | Errors and allocation/compiler failures only. |
| `RuntimeLogLevel::Info` | Default. Startup/configuration and periodic informational messages plus errors. |
| `RuntimeLogLevel::Debug` | Includes high-volume geometry, stream, texture, shader and cache diagnostics. |

Example:

```cpp
aurora::vita::BackendConfig cfg{};
cfg.log_level = aurora::vita::RuntimeLogLevel::Silent;
```

This is separate from the top-level Aurora `AuroraConfig::logLevel`, which controls the generic
Aurora logger. A full port that wants completely quiet normal operation should configure both.

For performance builds, prefer the stronger compile-time switch:

```sh
cmake --preset vita-gxm -DAURORA_VITA_RUNTIME_LOGGING=OFF
```

or the equivalent VitaGL preset. With this option disabled, Aurora Vita log macros are compiled out
and their arguments are not evaluated. This removes even the runtime log-level branch from hot
paths. Explicit `telemetry_log_path`, `coverage_log_path`, and `trace_log_path` outputs remain
caller-controlled diagnostics and are not silently disabled by this build option.

## Completed-frame performance snapshot

For profiling a shipping-style build without console/file logging, read:

```cpp
const auto perf = aurora::vita::completed_performance_snapshot();
if (perf.completedFrame) {
  // Sample perf.frameIndex and counters through the port's own transport.
}
```

Important fields:

- `frameUs` — backend begin/end interval; not an independent GPU timestamp or complete game-simulation measurement.
- `rendererCpuFrameUs` — native renderer CPU frame time.
- `displayQueueLastUs`, `displayQueueAverageUs`, `displayQueueMaxUs` — CPU time spent in
  `sceGxmDisplayQueueAddEntry`.
- `displayQueueBlockedPercent` — percentage of presented frames where that call exceeded 500 us.
- `gpuBackpressureLikely` — becomes true after at least 30 samples when blocked frames reach 10%.
- `nativePipelineUs`, `nativeTextureUs`, `nativeDrawUs` — sampled native submission phases;
  consult `nativeTimingsSampled` because these are intentionally sampled rather than timed every frame.
- EFB copy and scene counters/timings are included in the same snapshot.
- `staticGeometryHits`, `staticGeometryMisses`, `staticGeometryBytes`,
  `staticGeometryEntries` — verify that the experimental geometry cache is actually active and
  reusing immutable draws instead of silently falling back to the CPU path.
- `nativePipelineSetters`, `nativePipelineSettersSkipped`, `nativeUniformUploadCalls`,
  `nativeUniformUploadBytes` — native binding/upload work for the frame.
- `batchCandidates`, `batchMerged`, `batchRejectedState`, `batchRejectedIndices` —
  verify whether optional local batching has applicable geometry.
- `nativeFinishReasonCalls`, `nativeFinishReasonWaitUs` — cumulative attributed
  completion counts and CPU wall time; subtract consecutive snapshots for an interval.

The completed snapshot uses a short mutex-protected copy and never queues a GX
callback or drains pending work. `completedFrame` is false before the first
successful end-frame and after shutdown. With async GX it can be an older
completed frame; use `frameIndex` to detect repeats.

The existing `performance_snapshot()` is synchronous and can fence the GX worker
to read live renderer/cache state. Neither API prints or writes a log. Use the
completed API for observation that must preserve producer/consumer overlap.

## Display and raster extent

| Field | Default | Notes |
|---|---:|---|
| `width`, `height` | 960 x 544 | Vita scanout/backing extent. |
| `render_width`, `render_height` | 0 x 0 | Zero follows display extent. Reduced resolution is opt-in. |
| `wait_vblank` | true | Controls present synchronization. |

Reduced internal resolution can cut raster/depth work, but GXCopyTex, shadows, UI and crop semantics must be checked before use.

## Streaming

| Field | Default |
|---|---:|
| `stream_vertex_bytes` | 4 MiB |
| `stream_index_bytes` | 1 MiB |
| `stream_slots` | 3 |

Increasing these values reduces recycle pressure but consumes more mapped GPU-visible memory. Decreasing them can expose frequent arena recycle/wait overhead.

## CPU workers

| Field | Default |
|---|---:|
| `cpu_worker_threads` | 2 |
| `cpu_renderer_execution_lanes` | 0 (all available) |
| `cpu_game_execution_lanes` | 0 (all available) |
| `cpu_parallel_min_vertices` | 512 |

Aurora keeps the render thread as the graphics owner. Worker threads perform CPU-side vertex work only.

Lane counts include the caller. The helper order is CPU2, lower-priority CPU1,
then optional CPU3. `cpu_worker_threads=3` requests a third helper, accepted only
after physical core ID 3 and the system affinity mask are verified. A failed probe
retains two helpers. Installation of a CPU-unlock plugin is not proof of availability.

Set both lane caps to `3` to probe CPU3 while keeping renderer and public
`parallel_for()` dispatch on the original three-lane topology. Zero permits all
configured lanes, so requesting three helpers with zero caps allows four-lane
jobs when the workload is large enough. Read `core3_available()`, `core3_cpu_id()`
and `core3_affinity_mask()` before interpreting a run. These are separate topology
APIs; CPU3 probe fields are not members of `PerformanceSnapshot`.
There is no dynamic total-CPU3 utilization quota in this scheduler.

Device initialization also writes `<data_root>/cpu3_probe.log` with requested and
actual topology, core ID, affinity and system-info results. This one-shot evidence
file remains enabled even with `RuntimeLogLevel::Silent` or compiled-out normal
logging. Its initial idle-clock value is not a measured utilization percentage.

Lowering the threshold wakes workers for smaller draws and can lose performance to semaphore/scheduling overhead. Raising it leaves more work on the caller.

## Texture and geometry memory

| Field | Default | Notes |
|---|---:|---|
| `texture_cache_budget` | 24 MiB | Shared texture residency budget. |
| `static_geometry_budget` | GXM: 8 MiB; VitaGL/host: 0 | Experimental fixed/GPU geometry path. Set zero explicitly for the CPU control. |
| `static_geometry_min_vertices` | 48 | Minimum immutable display-list draw size eligible for fixed/GPU geometry caching. Lower only after measuring hit rate and resident bytes. |
| `static_geometry_stable_only` | false | Optional restriction to stable geometry sources. |

## VitaGL memory pools

| Field | Default |
|---|---:|
| `vgl_legacy_pool_size` | 0 |
| `vgl_ram_pool_size` | 0 |
| `vgl_cdram_pool_size` | 0 |
| `vgl_phycont_pool_size` | 0 |
| `vgl_cdlg_pool_size` | 0 |
| `vgl_ram_threshold` | 16 MiB |
| `vgl_circular_pool_size` | 32 MiB |
| `vgl_display_buffer_count` | 3 |
| `vgl_scratch_dynamic` | true |
| `vgl_scratch_stream` | true |

## GXM depth

`gxm_d16_depth=false` is the safe default. Enable D16 only as an explicit experiment.

## GXM local draw batching

`gxm_local_draw_batching=false` by default. Set it to true only for a hardware A/B
of adjacent compatible streamed triangles. The renderer must advertise native
local indices. Clear/copy/target/barrier commands prevent merges across ordering
boundaries, and arena padding can prevent otherwise compatible slices merging.
`gxm_disable_mask` bit `0x2000` overrides the batching feature for a reference run.

For a live change preserve existing runtime feature flags and add/remove
`gxbridge::RuntimeLocalDrawBatching` (`0x80`) through
`aurora_vita_debug_set_runtime_flags`. That API flushes pending commands before
changing mode. The runtime capability query advertises batching only for GXM.

## GXM scene budget

`gxm_scenes_per_frame` defaults to **5**. It configures the native GXM render-target
`scenesPerFrame` budget. Keep it above the measured steady-state/peak scene count for the title.
Do not reduce it as a generic memory optimization without hardware telemetry.
`gxm_parameter_buffer_bytes` defaults to 4 MiB and controls the native parameter
allocation independently of the shared vertex/index streaming ring.

## Shader/program binary cache

`data_root_path=nullptr` automatically resolves a per-title root on Vita:

```text
ux0:data/aurora-vita/<TITLE_ID>/
```

Aurora derives `TITLE_ID` from the current Vita application. This is the default storage boundary
for renderer-owned persistent data, so two games using Aurora Vita do not share shader caches or
pipeline manifests.

With no explicit override, both backends save program binaries under:

```text
ux0:data/aurora-vita/<TITLE_ID>/program_cache/
```

GXM and VitaGL then add their own ABI/version directory below that root. Set
`program_binary_cache_path` only when a port deliberately wants another location.

When comparing first-use shader compilation or menu/loading time, record whether the cache was cold
or warm and do not mix the two populations.

## Pipeline manifest and prewarm

`pipeline_warmup_path` and `pipeline_prewarm_limit` control the native GXM hot-pipeline manifest.
With a null path, GXM uses:

```text
ux0:data/aurora-vita/<TITLE_ID>/pipeline_hot_v1.bin
```

`pipeline_prewarm_limit=192` limits how many hot pipelines are restored/compiled during startup.

`startup_progress` optionally receives `(phase, completed, total, user)` progress
for program-cache preparation and pipeline prewarm, synchronously on the thread
calling `initialize()`. Keep the callback bounded; it is startup work, not a
per-frame callback or an asynchronous loader.

Prewarm trades startup/loading work and memory residency for fewer first-use pipeline stalls during
gameplay. Compare cold and warm boots separately.

### Zero runtime shader compilation on GXM

For a trained per-title cache, GXM can guarantee that gameplay never calls vitaShaRK:

```cpp
cfg.gxm_preload_program_cache = true;
cfg.gxm_program_cache_preload_limit = 1024;
cfg.gxm_seal_shader_cache_after_prewarm = true;
```

The preload validates cached GXP programs with `sceGxmProgramCheck()` before placing them in the
in-memory stage cache. The seal is applied only after the hot-pipeline prewarm. Once sealed, a
missing stage is **not compiled**; the affected pipeline creation fails for that draw and
`shaderCompileBlockedMisses` increases.

This therefore requires a training/cold run with sealing disabled first. Exercise menus, gameplay,
stages and effects that the shipping session must support so their GXP binaries are persisted.
For games that know their loading/gameplay boundary, the stricter alternative is to leave automatic
sealing disabled and call:

```cpp
aurora::vita::set_runtime_shader_compilation_enabled(false);
```

after loading. Re-enable it only during a controlled loading/training phase if new shader variants
are intentionally allowed.

Both snapshot APIs expose `shaderRuntimeCompilationEnabled`, `shaderRuntimeCompiles`,
`shaderRuntimeCompileUs`, `shaderCompileBlockedMisses`, `shaderDiskCacheHits`, and
`shaderDiskCacheMisses`. On a validated warm gameplay run, `shaderRuntimeCompiles` must remain
constant after the seal and `shaderCompileBlockedMisses` should remain zero.

Automatic shader failure artifacts also live under the same per-title root, in
`shader_failures/`, instead of a project-specific or global Aurora directory.

## Diagnostics

| Field | Default | Effect |
|---|---:|---|
| `log_level` | `RuntimeLogLevel::Info` | Console/backend logging threshold. |
| `diagnostics` | false | Expensive per-draw diagnostics. |
| `profile_split_vertex_phases` | false | Separates decode/transform phases for profiling; alters normal fused execution. |
| `texture_decode_diagnostics` | false | Texture decoder diagnostics. |
| `diagnostic_draw_limit` | 0 | Draw-count bisection; zero disables. |
| `strict_unsupported` | false | Treat unsupported paths strictly during development. |
| `diagnostics_period_frames` | 300 | Periodic diagnostic print interval. |
| `telemetry_log_path` | nullptr | Frame/renderer telemetry output. |
| `coverage_log_path` | nullptr | Feature coverage output. |
| `trace_log_path` | nullptr | Frame trace output. |
| `trace_capacity` | 4096 | Trace record capacity. |

Diagnostic builds are not directly comparable with shipping performance measurements. Always benchmark with diagnostics disabled after isolating the issue.

## Memory observation without a GX fence

```cpp
const auto memory = aurora::vita::completed_memory_snapshot();
if (memory.completedFrame) {
  // memory.frameIndex identifies the last successful CPU end_frame.
  const auto bytes = memory.budget.staticGeometryBytes;
}
```

This getter copies a published value under a short mutex; it neither queues a
GX callback nor accesses live caches. The value is invalid before the first
successful frame, after shutdown and at initialization, including failed init.
A failed frame retains the last successful value. It does not mean the GPU
has finished. Performance and memory are published separately: compare their
frameIndex values before combining two reads, rather than assuming atomicity
across both APIs.

`performance_snapshot()` is synchronous and can drain queued GX work.
`memory_budget()`, `renderer()`, `draw_sink()`, `telemetry()`, live renderer
`stats()` and `cache_counters()` are owner-thread tools. With async GX, overlays
should use the two completed getters. Direct renderer inspection in the
standalone diagnostic probe is serialized with the GX worker and intentionally
perturbs overlap; it is not the shipping overlay pattern.

The `stats()` reference reports current values at read time and is usable only
until the next mutating renderer call. Copy it for before/after comparisons.
`cache_counters()` copies only the five facade cache counters and is intended
for narrow owner-thread telemetry deltas.

## Candidate dispatch and draw view

`cpu_distinct_core_dispatch` and `gxm_immediate_draw_view` default false,
except when their candidate CMake defaults are enabled. They can be independently
set false in BackendConfig to compare original behavior. The dispatch cap bounds
the allowed helper prefix before filtering physical cores: a caller on CPU2
with cap3 uses CPU1, not CPU3; cap2 leaves that caller serial. Callback IDs can
have holes and remain within 0..3. `execution_lanes()` reports configured capacity,
not the number of active lanes in an individual job. CPU3 still needs its actual
CPU/affinity probe; no utilization quota is added by this change.

The draw view borrows const uniform/texture data for one synchronous native draw.
Uniform conversion uses a typed cache keyed by the producer revision; unversioned
inputs always refresh. Runtime feature changes invalidate it. Queued packet
state stays owned, and BeginScene, scissor and GPU page retirement retain their
existing rules. [Validation and handoff](../VITA_NATIVE_IMPLEMENTATION_2026-10-02.md).
