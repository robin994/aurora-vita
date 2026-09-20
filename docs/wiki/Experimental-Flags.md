# Experimental flags

This page is the source-of-truth inventory for public Aurora Vita configuration switches.

The values below reflect the current code, not historical recommendations in old profiling notes.

## Build and integration flags

| Flag | Default | Scope | Status | Purpose |
|---|---:|---|---|---|
| `AURORA_ENABLE_VITA_BACKEND` | `OFF` | Common | Integration | Parent Aurora option that enables Vita backend integration. The Vita presets set it to `ON`. |
| `AURORA_VITA_RENDERER` | `VITAGL` | Common | Required selector | Selects `VITAGL` or `GXM`. The two backends are mutually exclusive. |
| `AURORA_VITA_BACKEND_ONLY` | `OFF` | Common | Integration | Builds the standalone Vita graphics backend without the full desktop stack. |
| `AURORA_VITA_SDL3_NATIVE` | `ON` on Vita, otherwise `OFF` | Common/VitaGL | Integration | Enables SDL3 native Vita platform services. It is not valid as a GXM SDL probe path. |
| `AURORA_VITA_LTO` | `ON` on Vita, otherwise `OFF` | Common | Performance | Enables fat LTO objects and requests LTO at final link. Benchmark link time, binary size, and runtime. |
| `AURORA_VITA_RUNTIME_LOGGING` | `ON` | Common | Performance / release | Compile-time hard switch for Aurora Vita console logging. Set `OFF` for the lowest-overhead release build; all `AURORA_VITA_LOG_*` call sites compile out and log arguments are not evaluated. Explicit telemetry/coverage/trace files remain separate. |
| `AURORA_VITA_WITH_GX_FRONTEND` | `OFF` | Common | Integration | Builds the Dawn-free Dolphin GX/VI frontend used by native Vita ports. |
| `AURORA_VITA_WITH_UPSTREAM_GX` | `OFF` | VitaGL/desktop integration | Experimental integration | Compiles the Vita bridge against upstream Aurora GX structs. Not supported for native GXM and not intended for Vita runtime while upstream GX still owns Dawn. |
| `AURORA_VITA_BUILD_PROBE` | `OFF` | Common | Validation | Builds the standalone probe for the selected renderer. |
| `AURORA_VITA_BUILD_SDL3_PROBE` | `OFF` | VitaGL | Validation | Builds the SDL3 + vitaGL coexistence probe. Rejected by the GXM backend. |
| `AURORA_VITA_BUILD_BACKEND_TESTS` | `OFF` | Host | Validation | Builds dependency-light host contract tests. |
| `AURORA_VITA_PORT_ABI` | `SDK` | Common | Integration | Public ABI selector: `SDK` or `GAMECUBE`. `GAMECUBE` enables the GameCube-style wchar/enum ABI expected by some ports. |

## Texture and streaming flags

| Flag | VitaGL default | GXM default | Risk | Notes |
|---|---:|---:|---|---|
| `AURORA_VITA_DIRECT_STREAM_WRITE` | `OFF` | forced `OFF` | High | Writes streaming data directly into mapped VitaGL buffers. Requires cache/coherency validation. GXM currently forces the conservative path. |
| `AURORA_VITA_RUNTIME_MIPMAP_GENERATION` | `OFF` | N/A | Medium | Generates missing mip chains at runtime. Can improve sampling completeness but adds CPU/GPU work and memory pressure. |
| `AURORA_VITA_NATIVE_CMPR` | `ON` | `OFF` | Medium/High | Uses native CMPR/DXT1/BC1 upload paths. GXM keeps this opt-in because image parity must be checked on hardware. |
| `AURORA_VITA_NATIVE_GX_TEXTURES` | `ON` | `OFF` | Medium/High | Uses native GX intensity, intensity-alpha, and RGB565 mappings where supported. GXM keeps this opt-in because format/channel/alpha parity must be checked. |

## Runtime experimental/tuning fields

These are members of `aurora::vita::BackendConfig`, not CMake options.

| Field | Default | Scope | Status / risk |
|---|---:|---|---|
| `log_level` | `RuntimeLogLevel::Info` | Common | Runtime logging control. Use `Silent` to suppress Aurora Vita backend console/file diagnostics that are not explicitly requested through output-path fields. |
| `render_width`, `render_height` | `0, 0` | Common | Experimental resolution scaling. Zero follows display extent. Reduced resolution must be checked against GXCopyTex shadows, UI, crop, and aspect handling. |
| `gxm_d16_depth` | `false` | GXM | Experimental. D16 can reduce depth bandwidth/memory but GX exposes higher depth precision; validate z-fighting and depth effects. |
| `gxm_scenes_per_frame` | `5` | GXM | Native render-target scene budget. Keep high enough for the title's observed peak scene count; lowering it without measurements can increase stalls or fail scene submission. |
| `gxm_lit_fixed_vertex_gpu` | GXM: `true`; otherwise `false` | GXM | Experimental A/B profile: eligible immutable lit geometry keeps lighting/texgen on the native GXM vertex path. Force `false` for the CPU control build. |
| `static_geometry_budget` | GXM: 8 MiB; otherwise 0 | Common, primarily GXM | Experimental immutable geometry cache. The GXM profile currently enables 8 MiB for hardware testing; force `0` for the conservative CPU control build. |
| `static_geometry_min_vertices` | `48` | Common, primarily GXM | Minimum immutable display-list vertex count eligible for the fixed-vertex GPU cache. Lower values increase cache coverage and memory pressure; dynamic/untracked sources keep the conservative 48-vertex floor. |
| `data_root_path` | `nullptr` | Common | Null automatically resolves to `ux0:data/aurora-vita/<TITLE_ID>`. Override only when a port intentionally owns a different writable data root. |
| `pipeline_warmup_path` | `nullptr` | GXM | Null selects `<data_root>/pipeline_hot_v1.bin` for the native GXM hot-pipeline manifest. An explicit path overrides the per-title default. |
| `pipeline_prewarm_limit` | `192` | Common | Maximum hot pipelines compiled/resident during startup prewarm when a warmup manifest is configured. |
| `gxm_preload_program_cache` | `false` | GXM | Warm-cache experiment. Loads validated cached GXP stages into RAM before pipeline prewarm so first gameplay use avoids shader-cache file I/O. |
| `gxm_program_cache_preload_limit` | `1024` | GXM | Maximum cached GXP stages loaded by the warm-cache preload. |
| `gxm_seal_shader_cache_after_prewarm` | `false` | GXM | Strict warm-cache mode. After prewarm, cache misses are counted and rejected instead of invoking vitaShaRK. Use only after a training run has populated the per-title cache. |
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

## Internal macros that are not public switches

The following names may appear in source or audit notes but should not be treated as supported user-facing configuration:

- `AURORA_VITA_RENDERER_GXM`, `AURORA_VITA_RENDERER_VITAGL` — generated by renderer selection.
- `AURORA_VITA_UPSTREAM` and seam/stub macros — internal integration definitions.
- `AURORA_VITA_HOST_SMOKE_FRAMES` — probe-only compile-time helper.
- `AURORA_VITA_NO_DISCARD` — historical/audit discussion, not a supported current CMake option.

Do not add new port configuration around internal macros; expose a real CMake option or `BackendConfig` field instead.
