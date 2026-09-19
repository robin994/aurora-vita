# Runtime tuning

Runtime tuning is configured through `aurora::vita::BackendConfig`.

The defaults below are current source defaults.

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
| `cpu_parallel_min_vertices` | 512 |

Aurora keeps the render thread as the graphics owner. Worker threads perform CPU-side vertex work only.

Lowering the threshold wakes workers for smaller draws and can lose performance to semaphore/scheduling overhead. Raising it leaves more work on the caller.

## Texture and geometry memory

| Field | Default | Notes |
|---|---:|---|
| `texture_cache_budget` | 24 MiB | Shared texture residency budget. |
| `static_geometry_budget` | 0 | Experimental fixed/GPU geometry path. Keep zero as conservative control. |

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

## Shader/program binary cache

`program_binary_cache_path` has backend-specific behavior:

- **VitaGL:** `nullptr` disables the experimental program-binary disk cache. Set a path to opt in.
- **GXM:** `nullptr` uses Aurora's default persistent cache directory,
  `ux0:data/aurora-vita/program_cache`. Set a path to override that location.

When comparing first-use shader compilation or menu/loading time, record whether the cache was cold
or warm and do not mix the two populations.

## Diagnostics

| Field | Default | Effect |
|---|---:|---|
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
