# VitaGL backend

The VitaGL backend uses vitaGL for graphics ownership while Aurora provides GX translation, caching, streaming and diagnostics.

## Recommended baseline

```sh
cmake --preset vita-vitagl
cmake --build build/vita-vitagl --parallel 8
```

Current VitaGL texture defaults are intentionally more aggressive than GXM:

- `AURORA_VITA_NATIVE_CMPR=ON`
- `AURORA_VITA_NATIVE_GX_TEXTURES=ON`
- `AURORA_VITA_RUNTIME_MIPMAP_GENERATION=OFF`
- `AURORA_VITA_DIRECT_STREAM_WRITE=OFF`

If debugging a texture regression, disable native texture options first to establish an RGBA/conservative control path.

`BackendConfig::program_binary_cache_path` is opt-in on VitaGL: the default `nullptr` disables
the experimental disk cache.

## `AURORA_VITA_DIRECT_STREAM_WRITE`

Default: **OFF**

Writes frame streaming data directly into mapped vitaGL buffers instead of staging then copying.

Potential benefit:
- fewer copies;
- lower CPU streaming overhead.

Risk:
- CPU/GPU cache-coherency mistakes;
- stale or partially visible vertex/index data;
- hardware-only corruption.

This flag requires repeated hardware testing; a host build cannot validate Vita cache behavior.

## `AURORA_VITA_RUNTIME_MIPMAP_GENERATION`

Default: **OFF**

Generates missing mip chains at runtime.

Use when a title requires mip completeness and does not provide explicit levels. Measure:
- load time;
- upload time;
- texture memory;
- frame spikes when new textures appear.

## `AURORA_VITA_NATIVE_CMPR`

Default: **ON**

Uploads GameCube CMPR through vitaGL's native DXT1 path when supported.

Disable this when investigating:
- block artifacts;
- alpha edge errors;
- mip corruption;
- unexpected texture memory behavior.

## `AURORA_VITA_NATIVE_GX_TEXTURES`

Default: **ON**

Uses exact/native vitaGL formats for supported GX I/I+A/RGB565 textures.

Disable to compare against the generic conversion path when debugging color, alpha, lighting lookup, UI or shadow textures.

## VitaGL memory tuning

These are runtime `BackendConfig` fields:

- `vgl_legacy_pool_size` — default 0.
- `vgl_ram_pool_size` — default 0.
- `vgl_cdram_pool_size` — default 0.
- `vgl_phycont_pool_size` — default 0.
- `vgl_cdlg_pool_size` — default 0.
- `vgl_ram_threshold` — default 16 MiB.
- `vgl_circular_pool_size` — default 32 MiB.
- `vgl_display_buffer_count` — default 3.
- `vgl_scratch_dynamic` — default true.
- `vgl_scratch_stream` — default true.

When any explicit pool size is non-zero, Aurora uses the custom-size vitaGL initialization path instead of allowing vitaGL to consume all currently-free pools. This is useful for ports that maintain large long-lived game heaps alongside graphics allocations.

Change one pool at a time and log total/free CDRAM, PHYCONT and texture residency.

## SDL3 native probe

`AURORA_VITA_BUILD_SDL3_PROBE=ON` is a VitaGL-only validation target. The GXM build rejects it because the native GXM process must not share graphics ownership with vitaGL.
