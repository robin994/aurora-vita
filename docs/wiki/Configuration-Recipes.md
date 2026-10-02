# Configuration recipes

These recipes are explicit controls and individual experiments for revision
`ff5b2cf` (2026-10-02). The generic GXM runtime defaults include an 8 MiB geometry
cache and lit fixed-vertex processing; an explicit CPU control overrides them.

## Conservative GXM control

```sh
cmake --preset vita-gxm \
  -DAURORA_VITA_RUNTIME_LOGGING=OFF \
  -DAURORA_VITA_ASYNC_GX=OFF \
  -DAURORA_VITA_GXM_DIRECT_STREAM_WRITE=OFF \
  -DAURORA_VITA_GXM_DIRECT_DRAW_SUBMIT=OFF \
  -DAURORA_VITA_NATIVE_CMPR=OFF \
  -DAURORA_VITA_NATIVE_GX_TEXTURES=OFF
```

```cmake
set(AURORA_VITA_RENDERER GXM CACHE STRING "" FORCE)
set(AURORA_VITA_NATIVE_CMPR OFF CACHE BOOL "" FORCE)
set(AURORA_VITA_NATIVE_GX_TEXTURES OFF CACHE BOOL "" FORCE)
```

```cpp
aurora::vita::BackendConfig cfg{};
cfg.log_level = aurora::vita::RuntimeLogLevel::Silent;
cfg.render_width = 0;
cfg.render_height = 0;
cfg.gxm_d16_depth = false;
cfg.static_geometry_budget = 0;
cfg.gxm_lit_fixed_vertex_gpu = false;
cfg.gxm_streamed_fixed_vertex_gpu = false;
cfg.gxm_dynamic_tex_matrix_gpu = false;
cfg.gxm_bump_fixed_vertex_gpu = false;
cfg.gxm_primitive_expand_gpu = false;
cfg.gxm_local_draw_batching = false;
cfg.diagnostics = false;
```

Use this control when investigating rendering regressions.
Persistent renderer caches use `ux0:data/aurora-vita/<TITLE_ID>/` automatically; leave
`data_root_path` null unless the port deliberately needs another Aurora-owned location.

## GXM texture experiment

Enable only one format family at a time:

```sh
cmake --preset vita-gxm -DAURORA_VITA_NATIVE_CMPR=ON
```

Then separately:

```sh
cmake --preset vita-gxm -DAURORA_VITA_NATIVE_GX_TEXTURES=ON
```

Do not enable both until each passes hardware image comparison independently.

## Reduced-resolution experiment

```cpp
cfg.render_width = 640;
cfg.render_height = 448;
```

Compare against `0/0` on the exact same scene. Pay particular attention to GXCopyTex shadows, crop rectangles and UI.

## D16 experiment

```cpp
cfg.gxm_d16_depth = true;
```

Run depth-heavy stages and inspect z-fighting before considering performance.

## Fixed/GPU geometry experiment

```cpp
cfg.static_geometry_budget = 8 * 1024 * 1024;
cfg.gxm_lit_fixed_vertex_gpu = true;
```

This is currently the GXM experimental default. For the A/B control use:

```cpp
cfg.static_geometry_budget = 0;
cfg.gxm_lit_fixed_vertex_gpu = false;
```

Do not combine the first comparison with reduced resolution, native GXM textures or D16. A
single-variable experiment makes regressions bisectable.

## GXM zero-gameplay shader compilation

First perform a training run with the normal compiler path enabled:

```cpp
cfg.gxm_preload_program_cache = false;
cfg.gxm_seal_shader_cache_after_prewarm = false;
```

Exercise the menus, stages, characters and effects that must be available without compilation.
On the next warm run use:

```cpp
cfg.gxm_preload_program_cache = true;
cfg.gxm_program_cache_preload_limit = 1024;
cfg.pipeline_prewarm_limit = 512;
cfg.gxm_seal_shader_cache_after_prewarm = true;
```

After initialization, `shark_compile_shader()` cannot run. A previously unseen shader stage becomes
a counted blocked miss instead of a gameplay compile. Verify that
`completed_performance_snapshot().shaderCompileBlockedMisses == 0` once a frame
has completed, for the complete test session.

## Local streamed-triangle batching

Keep every other setting identical to the reference run:

```cpp
cfg.gxm_local_draw_batching = true;
```

Compare `batchCandidates`, `batchMerged`, rejection counts, native draws and
frame samples. A successful build or an enabled feature bit does not establish
that the game emits contiguous compatible slices. Compare UI, alpha, EFB copies,
scissors and scene transitions on device.

To isolate the four refactor optimizations in the same executable:

```cpp
cfg.gxm_disable_mask |= 0x7800;
```

Remove those bits for the candidate. This is a reference-cost control, not an old
binary or a complete rollback of all GXM optimizations. `0x0010` additionally
disables all fragment-uniform reuse and packet-state sharing.

## CPU3 probe without fourth-lane dispatch

```cpp
cfg.cpu_worker_threads = 3;
cfg.cpu_renderer_execution_lanes = 3;
cfg.cpu_game_execution_lanes = 3;
```

Check `core3_available()`, `core3_cpu_id()` and `core3_affinity_mask()` after
initialization. An accepted probe enables infrastructure, not a measured CPU3
budget or a gameplay performance result. A failed probe keeps two helpers.

## Conservative VitaGL debug control

```sh
cmake --preset vita-vitagl \
  -DAURORA_VITA_NATIVE_CMPR=OFF \
  -DAURORA_VITA_NATIVE_GX_TEXTURES=OFF \
  -DAURORA_VITA_RUNTIME_MIPMAP_GENERATION=OFF \
  -DAURORA_VITA_DIRECT_STREAM_WRITE=OFF
```

Use this when isolating texture/streaming regressions from VitaGL native-format optimizations.
