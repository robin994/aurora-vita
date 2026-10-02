# Native GXM backend

The GXM backend talks directly to `sceGxm`. Its probe is audited to ensure that GL/vitaGL/vita2d entry points are not linked into the native binary.

## Explicit CPU reference configuration

```sh
cmake --preset vita-gxm \
  -DAURORA_VITA_NATIVE_CMPR=OFF \
  -DAURORA_VITA_NATIVE_GX_TEXTURES=OFF
cmake --build build/vita-gxm --parallel 8
```

Runtime:

```cpp
aurora::vita::BackendConfig config{};
config.log_level = aurora::vita::RuntimeLogLevel::Info;
config.render_width = 0;
config.render_height = 0;
config.gxm_d16_depth = false;
config.static_geometry_budget = 0;          // explicit CPU/control override
config.gxm_lit_fixed_vertex_gpu = false;    // explicit CPU/control override
```

This reference overrides the current GXM defaults. The current GXM
experimental profile defaults to an 8 MiB immutable geometry cache with
`gxm_lit_fixed_vertex_gpu=true` so it can be A/B tested on hardware.

For a quiet shipping build set `config.log_level = RuntimeLogLevel::Silent`. Explicit telemetry,
coverage, and trace paths still write if configured.

GXM uses a persistent per-title compiled-program cache by default at
`ux0:data/aurora-vita/<TITLE_ID>/program_cache`. Set
`BackendConfig::program_binary_cache_path` to override that location, or
`data_root_path` to move all Aurora-owned persistent data for the title.

It also uses a hot-pipeline manifest by default at
`ux0:data/aurora-vita/<TITLE_ID>/pipeline_hot_v1.bin`. `pipeline_prewarm_limit` defaults to 192.
Use `pipeline_warmup_path` to override the manifest location.

`gxm_scenes_per_frame` defaults to 5 and should remain above the title's measured peak native
scene count.

`gxm_lit_fixed_vertex_gpu` currently defaults to **true on GXM** as an experimental hardware
profile. Eligible immutable lit geometry can keep GX lighting/texgen work on the native GXM vertex
path. Compare against an explicit `false` CPU control and validate lighting, matrix updates,
normals, texgen, and animation before promoting the result beyond the experiment.

## GXM experimental flags

### `AURORA_VITA_NATIVE_CMPR`

Default: **OFF**

Uses the native CMPR/BC1 upload path instead of always expanding to RGBA8.

Potential benefit:
- lower texture bandwidth and memory use;
- less texture conversion work.

Validation requirements:
- asymmetric checkerboard texture;
- alpha edges;
- mip transitions;
- wrap modes;
- EFB-derived textures.

Do not enable globally based only on a successful load or a single scene.

### `AURORA_VITA_NATIVE_GX_TEXTURES`

Default: **OFF**

Enables native mappings for supported GX packed formats such as intensity, intensity-alpha, and RGB565.

Potential benefit:
- lower memory footprint;
- fewer CPU conversions;
- reduced upload bandwidth.

Risks:
- channel swizzle mismatch;
- alpha semantic mismatch;
- packed-format precision differences;
- mip/wrap differences.

### `BackendConfig::gxm_d16_depth`

Default: **false**

Allocates D16 instead of DF32 depth surfaces.

Potential benefit:
- roughly half the depth backing bytes;
- lower depth bandwidth.

Risks:
- z-fighting;
- altered depth precision;
- regressions in effects relying on fine depth separation.

Accept only after long-distance geometry, coplanar surfaces, shadows, particles, and depth-copy effects are compared.

### `BackendConfig::static_geometry_budget`

Current GXM experimental default: **8 MiB**. VitaGL/host default: **0**.

Enables the experimental fixed-geometry GPU path/cache when non-zero.

The source default enables this experiment; it does not establish hardware
validation for every eligible vertex category. Compare against the explicit CPU
reference, especially matrix, lighting, texgen, FIFO source revision and cache lifetime.

Use an explicit `0` budget for the conservative CPU control. Use telemetry:
- geometry cache hits/misses;
- resident geometry bytes;
- lookup fallback count;
- framebuffer parity.

## GXM streaming

`AURORA_VITA_DIRECT_STREAM_WRITE` belongs to VitaGL and is forced to **0** in
the GXM target. Native GXM uses these separate CMake options, all default **OFF**:

- `AURORA_VITA_GXM_DIRECT_STREAM_WRITE`: mapped CpuGpu ring writes.
- `AURORA_VITA_ASYNC_GX`: dedicated GX consumer/submission thread.
- `AURORA_VITA_GXM_DIRECT_DRAW_SUBMIT`: direct streamed draw submission.

Use a separate build directory for the combined experiment in
[Building](../building.md#experimental-native-submission-build). Validate queue
ordering, cache visibility, mutation, ring reuse and PS-button suspend/resume.

## State reuse and local batching

The current renderer uses GX state domains, cached structural recipes and
immutable packet state. Native pipeline binding compares programs/depth/cull
within a scene and resolves uniform capacities at pipeline creation. Every
BeginScene resets the context bindings; reservations are invalidated when a
program changes. Exact partial scissors and EFB alpha/orientation remain part
of the contract.

`gxm_local_draw_batching=false` is the default. When enabled, only adjacent
compatible streamed triangles with contiguous pending arena slices can merge.
Clear/copy/target/barrier commands end the run, and indices are validated before
rebasing. A direct-submit build uses queued submission while batching is enabled.
Inspect merge/rejection counters before assuming native draw counts decreased.

See the [reference mask table](Experimental-Flags.md#reference-and-diagnostic-mask)
and [2026-10-02 refactor report](../../platforms/vita/gxm/GXM_REFACTOR_2026-10-02.md)
for current validation and per-feature controls.

## EFB copy paths

Eligible passthrough/RGB565 copies at equal size or 2:1 reduction use native GPU
paths. The unmodified equal-size case uses transfer; downscale, flip and opaque
RGB565 fixups use draw-based conversion. Feedback copies and unsupported format
or size combinations retain cached CPU conversion/readback/upload fallback.
Preserve source-target restoration, crop, flip, channel/alpha and synchronization
semantics when comparing them.

## Native binary audit

After building the GXM probe:

```sh
python3 tools/check_vita_gxm_binary.py \
  build/vita-gxm/aurora_vita_gx_probe \
  --map build/vita-gxm/aurora_vita_gx_probe.map \
  --nm "$VITASDK/bin/arm-vita-eabi-nm"
```

The check must report native GXM draw/present symbols and no GL/vgl/vita2d entry points or libraries.

## Hardware acceptance

Before promoting an experimental GXM flag:
1. verify the baseline build on the same commit;
2. enable only one flag;
3. compare framebuffer orientation, shadows, lighting, UI, alpha and mip behavior;
4. run gameplay long enough to exercise target switches and EFB copies;
5. compare median/p95/p99 frame times after warm-up;
6. record the exact VPK/eboot hash.
