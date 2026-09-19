# Native GXM backend

The GXM backend talks directly to `sceGxm`. Its probe is audited to ensure that GL/vitaGL/vita2d entry points are not linked into the native binary.

## Recommended conservative baseline

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
config.static_geometry_budget = 0;
```

This baseline intentionally favors correctness over peak throughput.

For a quiet shipping build set `config.log_level = RuntimeLogLevel::Silent`. Explicit telemetry,
coverage, and trace paths still write if configured.

GXM uses a persistent compiled-program cache by default at
`ux0:data/aurora-vita/program_cache`. Set
`BackendConfig::program_binary_cache_path` to override that location.

It also uses a hot-pipeline manifest by default at
`ux0:data/aurora-vita/pipeline_hot_v1.bin`. `pipeline_prewarm_limit` defaults to 192.
Use `pipeline_warmup_path` to override the manifest location.

`gxm_scenes_per_frame` defaults to 5 and should remain above the title's measured peak native
scene count.

`gxm_lit_fixed_vertex_gpu` defaults to **false**. When enabled, eligible immutable lit geometry
can keep GX lighting/texgen work on the native GXM vertex path. Treat it as an A/B experiment
against the CPU vertex path and validate lighting, matrix updates, normals, texgen, and animation
before enabling it per-title.

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

Default: **0**

Enables the experimental fixed-geometry GPU path/cache when non-zero.

This path must remain opt-in until the eligible vertex categories are validated against the CPU reference path. It is especially sensitive to matrix, lighting, texgen, FIFO source revision, and cache lifetime.

Use telemetry:
- geometry cache hits/misses;
- resident geometry bytes;
- lookup fallback count;
- framebuffer parity.

## GXM streaming

`AURORA_VITA_DIRECT_STREAM_WRITE` is currently forced to **0** for GXM. Do not try to override it from a port. GXM uses its own memory/streaming path and cache-coherency rules.

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
