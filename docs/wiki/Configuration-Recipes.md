# Configuration recipes

These recipes are starting points, not universal performance presets.

## Conservative GXM control

```sh
cmake --preset vita-gxm -DAURORA_VITA_RUNTIME_LOGGING=OFF
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

## Conservative VitaGL debug control

```sh
cmake --preset vita-vitagl \
  -DAURORA_VITA_NATIVE_CMPR=OFF \
  -DAURORA_VITA_NATIVE_GX_TEXTURES=OFF \
  -DAURORA_VITA_RUNTIME_MIPMAP_GENERATION=OFF \
  -DAURORA_VITA_DIRECT_STREAM_WRITE=OFF
```

Use this when isolating texture/streaming regressions from VitaGL native-format optimizations.
