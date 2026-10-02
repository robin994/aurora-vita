# Experimental flags

This inventory reflects renderer revision `ff5b2cf` (2026-10-02).
`platforms/vita/aurora_vita_backend.hpp` and the CMake modules define the source
defaults; historical audit recommendations may describe a different revision.
Defaults below are generic CMake defaults unless a preset override is stated.

## Build and integration flags

| Flag | Default | Scope | Status | Purpose |
|---|---:|---|---|---|
| `AURORA_ENABLE_VITA_BACKEND` | `OFF` | Common | Integration | Parent Aurora option that enables Vita backend integration. The Vita presets set it to `ON`. |
| `AURORA_VITA_RENDERER` | `VITAGL` | Common | Required selector | Selects `VITAGL` or `GXM`. The two backends are mutually exclusive. |
| `AURORA_VITA_BACKEND_ONLY` | `OFF` | Common | Integration | Builds the standalone Vita graphics backend without the full desktop stack. |
| `AURORA_VITA_SDL3_NATIVE` | `ON` on Vita, otherwise `OFF` | Common/VitaGL | Integration | Enables SDL3 native Vita platform services. It is not valid as a GXM SDL probe path. |
| `AURORA_VITA_LTO` | `ON` on Vita, otherwise `OFF` | Common | Performance | Enables fat LTO objects and requests LTO at final link. Benchmark link time, binary size, and runtime. |
| `AURORA_VITA_LTO_JOBS` | `auto` | Common/GXM | Build tuning | LTO parallelism for common/native GXM objects; accepts `auto` or a positive GCC job count. The VitaGL-specific objects currently use `auto`. |
| `AURORA_VITA_RUNTIME_LOGGING` | `ON` | Common | Performance / release | Compile-time hard switch for Aurora Vita console logging. Set `OFF` for the lowest-overhead release build; all `AURORA_VITA_LOG_*` call sites compile out and log arguments are not evaluated. Explicit telemetry/coverage/trace files remain separate. |
| `AURORA_VITA_WITH_GX_FRONTEND` | `OFF`; all three public presets set `ON` | Common | Integration | Builds the Dawn-free Dolphin GX/VI frontend used by native Vita ports and real host frontend tests. |
| `AURORA_VITA_WITH_UPSTREAM_GX` | `OFF` | VitaGL/desktop integration | Experimental integration | Compiles the Vita bridge against upstream Aurora GX structs. Not supported for native GXM and not intended for Vita runtime while upstream GX still owns Dawn. |
| `AURORA_VITA_BUILD_PROBE` | `OFF` | Common | Validation | Builds the standalone probe for the selected renderer. |
| `AURORA_VITA_BUILD_SDL3_PROBE` | `OFF` | VitaGL | Validation | Builds the SDL3 + vitaGL coexistence probe. Rejected by the GXM backend. |
| `AURORA_VITA_BUILD_BACKEND_TESTS` | `OFF` | Host | Validation | Builds dependency-light host contract tests. |
| `AURORA_VITA_PORT_ABI` | `SDK` | Common | Integration | Public ABI selector: `SDK` or `GAMECUBE`. `GAMECUBE` enables the GameCube-style wchar/enum ABI expected by some ports. |

## Texture and streaming flags

| Flag | VitaGL default | GXM default | Risk | Notes |
|---|---:|---:|---|---|
| `AURORA_VITA_DIRECT_STREAM_WRITE` | `OFF` | forced `OFF` | High | VitaGL mapped-buffer switch. Native GXM has a separate option below; this macro does not select GXM direct writes. |
| `AURORA_VITA_GXM_DIRECT_STREAM_WRITE` | N/A | `OFF` | High | Writes native vertex/index data directly into CpuGpu ring pages. Hardware cache/lifetime comparison required. |
| `AURORA_VITA_ASYNC_GX` | N/A | `OFF` | High | Moves native GX decode/translation/submission to a dedicated consumer thread. Keep FIFO barriers and producer ownership intact. |
| `AURORA_VITA_GXM_DIRECT_DRAW_SUBMIT` | N/A | `OFF` | Medium/High | Bypasses CommandStream for streamed native draws; local batching selects queued submission when enabled. |
| `AURORA_VITA_RUNTIME_MIPMAP_GENERATION` | `OFF` | N/A | Medium | Generates missing mip chains at runtime. Can improve sampling completeness but adds CPU/GPU work and memory pressure. |
| `AURORA_VITA_NATIVE_CMPR` | `ON` | `OFF` | Medium/High | Uses native CMPR/DXT1/BC1 upload paths. GXM keeps this opt-in because image parity must be checked on hardware. |
| `AURORA_VITA_NATIVE_GX_TEXTURES` | `ON` | `OFF` | Medium/High | Uses native GX intensity, intensity-alpha, and RGB565 mappings where supported. GXM keeps this opt-in because format/channel/alpha parity must be checked. |

## Runtime experimental/tuning fields

These are members of `aurora::vita::BackendConfig`, not CMake options.

| Field | Default | Scope | Status / risk |
|---|---:|---|---|
| `log_level` | `RuntimeLogLevel::Info` | Common | Runtime console logging control. Explicit diagnostic files and the one-shot CPU3 startup evidence remain independent. |
| `render_width`, `render_height` | `0, 0` | Common | Experimental resolution scaling. Zero follows display extent. Reduced resolution must be checked against GXCopyTex shadows, UI, crop, and aspect handling. |
| `gxm_d16_depth` | `false` | GXM | Experimental. D16 can reduce depth bandwidth/memory but GX exposes higher depth precision; validate z-fighting and depth effects. |
| `gxm_local_draw_batching` | `false` | GXM | Adjacent compatible streamed triangles only; requires exact state, contiguous pending arena slices and valid indices. Hardware image/gameplay comparison required. |
| `gxm_scenes_per_frame` | `5` | GXM | Native render-target scene budget. Keep high enough for the title's observed peak scene count; lowering it without measurements can increase stalls or fail scene submission. |
| `gxm_parameter_buffer_bytes` | 4 MiB | GXM | Native parameter-buffer allocation. Measure memory and scene pressure before changing. |
| `gxm_lit_fixed_vertex_gpu` | GXM: `true`; otherwise `false` | GXM | Experimental A/B profile: eligible immutable lit geometry keeps lighting/texgen on the native GXM vertex path. Force `false` for the CPU control build. |
| `gxm_streamed_fixed_vertex_gpu` | `false` | GXM | Optional eligible dynamic/streamed fixed-vertex processing. CPU fallback remains for unsupported draws. |
| `gxm_dynamic_tex_matrix_gpu` | `false` | GXM | Optional vertex-selected texture-matrix processing. |
| `gxm_bump_fixed_vertex_gpu` | `false` | GXM | Optional bump/emboss fixed-vertex processing. |
| `gxm_primitive_expand_gpu` | `false` | GXM | Optional fixed-vertex line/point expansion. |
| `static_geometry_budget` | GXM: 8 MiB; otherwise 0 | Common, primarily GXM | Experimental immutable geometry cache. The GXM profile currently enables 8 MiB for hardware testing; force `0` for the conservative CPU control build. |
| `static_geometry_stable_only` | `false` | Common | Restricts caching to stable sources when enabled. Guest writes still require source invalidation. |
| `static_geometry_min_vertices` | `48` | Common, primarily GXM | Minimum immutable display-list vertex count eligible for the fixed-vertex GPU cache. Lower values increase cache coverage and memory pressure; dynamic/untracked sources keep the conservative 48-vertex floor. |
| `data_root_path` | `nullptr` | Common | Null automatically resolves to `ux0:data/aurora-vita/<TITLE_ID>`. Override only when a port intentionally owns a different writable data root. |
| `pipeline_warmup_path` | `nullptr` | GXM | Null selects `<data_root>/pipeline_hot_v1.bin` for the native GXM hot-pipeline manifest. An explicit path overrides the per-title default. |
| `pipeline_prewarm_limit` | `192` | Common | Maximum hot pipelines compiled/resident during startup prewarm when a warmup manifest is configured. |
| `gxm_preload_program_cache` | `false` | GXM | Warm-cache experiment. Loads validated cached GXP stages into RAM before pipeline prewarm so first gameplay use avoids shader-cache file I/O. |
| `gxm_program_cache_preload_limit` | `1024` | GXM | Maximum cached GXP stages loaded by the warm-cache preload. |
| `gxm_seal_shader_cache_after_prewarm` | `false` | GXM | Strict warm-cache mode. After prewarm, cache misses are counted and rejected instead of invoking vitaShaRK. Use only after a training run has populated the per-title cache. |
| `startup_progress`, `startup_progress_user` | `nullptr`, `nullptr` | GXM startup | Synchronous cache/prewarm progress callback and caller context. Called on the initialization thread; does not create a loading worker. |
| `display_list_shadow` | `true` | Native GX frontend | Cached-RAM shadows of display lists; requires publication of guest writes. Disable during invalidation diagnosis. |
| `gxm_disable_mask` | `0` | GX/GXM | Reference and diagnostic bits listed below; preserve existing bits when adding a control. |
| `profile_split_vertex_phases` | `false` | Common | Profiling-only. Changes execution/cache behavior; do not compare its FPS directly with fused mode. |
| `diagnostic_draw_limit` | `0` | Common | Diagnostic. Caps submitted GX draw packets for framebuffer bisection. |
| `strict_unsupported` | `false` | Common | Diagnostic/development. Promotes unsupported paths to strict failures. |
| `diagnostics` | `false` | Common | Enables expensive per-draw diagnostic collection. |
| `texture_decode_diagnostics` | `false` | Common | Enables texture decode diagnostics. |
| `telemetry_log_path` | `nullptr` | Common | Enables telemetry file output. |
| `coverage_log_path` | `nullptr` | Common | Enables feature-coverage output. |
| `trace_log_path` | `nullptr` | Common | Enables frame trace output. |
| `trace_capacity` | `4096` | Common | Trace buffer sizing. |
| `program_binary_cache_path` | `nullptr` | Common | Null selects `<data_root>/program_cache` for both GXM and VitaGL. Backend ABI/version subdirectories keep incompatible binary formats separate. |

See [Runtime tuning](Runtime-Tuning.md) for memory, worker, cache, and buffer sizing fields.

## Reference and diagnostic mask

`BackendConfig::gxm_disable_mask` uses `gfx::GxmDisableBits`. A set reference bit
disables the named optimization; diagnostic bits add work and alter timing.

| Bit | Type | Behavior when set |
| --- | --- | --- |
| `0x0001` | Reference | Disable depth-load/depthless-scene policy. |
| `0x0002` | Reference | Disable discard-free full-scissor shader variant; retain exact partial scissor. |
| `0x0004` | Reference | Disable native texture-binding reuse. Scene state always resets regardless of this bit. |
| `0x0008` | Reference | Disable fixed-uniform revision shortcut. |
| `0x0010` | Reference | Disable all fragment-uniform reuse and producer packet-state sharing. |
| `0x0020` | Reference | Disable native texture wrap path. |
| `0x0040` | Reference | Disable static geometry LRU eviction. |
| `0x0080` | Reference | Disable display-list shadows. |
| `0x0100` | Diagnostic | Finish each scene and record scene GPU wait; serializes CPU/GPU overlap. |
| `0x0200` | Diagnostic | Individually time up to 512 draws every 300th frame; heavily perturbs that frame. |
| `0x0400` | Diagnostic | Collect DrawSink phase averages every 120 frames; these are not frame samples. |
| `0x0800` | Reference | Rebuild translated uniform/texture state rather than using state domains. |
| `0x1000` | Reference | Emit all seven native pipeline setters on a pipeline transition. |
| `0x2000` | Reference | Disable local streamed draw merging. |
| `0x4000` | Reference | Copy uniform/texture state into each packet. |

`0x7800` combines the four new refactor reference controls. It does not reproduce
an older executable or disable every pre-existing optimization. Runtime feature
bits such as `gxbridge::RuntimeLocalDrawBatching=0x80` use a **different bitset**;
do not pass reference-mask bits to `aurora_vita_debug_set_runtime_flags`.

## Internal macros that are not public switches

The following names may appear in source or audit notes but should not be treated as supported user-facing configuration:

- `AURORA_VITA_RENDERER_GXM`, `AURORA_VITA_RENDERER_VITAGL` — generated by renderer selection.
- `AURORA_VITA_UPSTREAM` and seam/stub macros — internal integration definitions.
- `AURORA_VITA_HOST_SMOKE_FRAMES` — probe-only compile-time helper.
- `AURORA_VITA_NO_DISCARD` — historical/audit discussion, not a supported current CMake option.

Do not add new port configuration around internal macros; expose a real CMake option or `BackendConfig` field instead.
