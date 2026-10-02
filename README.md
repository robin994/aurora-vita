<div align="center">
  <img src="assets/aurora.png" alt="Aurora" width="640">
</div>

# Aurora Vita

Aurora Vita adapts Aurora's source-level GameCube/Wii compatibility layer for
PS Vita game ports. The original Dolphin GX/VI entry points feed a shared FIFO,
command processor and draw frontend, with a choice of native **SceGxm** or
**vitaGL** rendering. Games integrate the library through CMake and retain their
own callbacks, assets, simulation and main loop.

This fork is experimental. Renderer coverage and performance depend on the
title, configuration and device. The included VPKs are diagnostic probes;
gameplay fidelity and sustained frame rate require tests in the consuming port.

## Current implementation

| Area | Current behavior |
| --- | --- |
| Renderer selection | `AURORA_VITA_RENDERER=GXM` or `VITAGL`, selected at build time. The generic CMake default is `VITAGL`; each device preset selects its renderer explicitly. |
| GX frontend | Shared Dawn-free GX/VI frontend, FIFO ordering, vertex/texture decoding, lighting/texgen preparation and CPU fallback. |
| Native GXM | Cg/GXP shader generation, native resource ownership, TEV/fog, EFB targets/copies, presentation and readback. GXM builds exclude GL/vitaGL/vita2d renderer code. |
| State submission | Domain-specific GX revisions, cached draw recipes and uniform capacities, immutable packet-state sharing, and native binding comparisons within each scene. |
| Draw batching | Adjacent compatible streamed triangles can merge; `gxm_local_draw_batching` defaults to `false`. Ordering barriers and resource lifetime checks remain in place. |
| CPU preparation | Two helper threads by default. An optional third helper is accepted only after a CPU3 affinity/core-ID probe; renderer and game lane caps are separate. |
| Persistent data | Program caches and GXM pipeline warmup data are isolated under `ux0:data/aurora-vita/<TITLE_ID>/`. |
| Profiling | Completed-frame snapshots, submission/upload counters, attributed finish waits and sampled FRAME-log comparison tools. |

GXM currently defaults to display-sized raster output, DF32 depth, an **8 MiB
static geometry cache** and **lit fixed-vertex GPU processing** for eligible
draws. CPU control configurations explicitly set the geometry budget to `0`
and `gxm_lit_fixed_vertex_gpu=false`. Native packed textures, GXM direct writes,
async GX, direct draw submission, streamed fixed-vertex processing and local
batching remain optional. See the [flag matrix](docs/wiki/Experimental-Flags.md)
for defaults and the [configuration recipes](docs/wiki/Configuration-Recipes.md)
for explicit controls.

## Build and test

Host validation requires CMake 3.25+, a C++20 compiler and Git; Python 3 enables
the performance-comparison and binary-audit contract tests.

```sh
cmake --preset vita-host-tests
cmake --build --preset vita-host-tests --parallel 8
ctest --preset vita-host-tests --output-on-failure
```

To build the native GXM probes with VitaSDK and the required shader libraries:

```sh
export VITASDK=/usr/local/vitasdk
cmake --preset vita-gxm
cmake --build --preset vita-gxm --parallel 8
```

This produces `build/vita-gxm/aurora_vita_gx_probe.vpk` and
`build/vita-gxm/aurora_vita_gxm_probe.vpk`. Use `vita-vitagl` for the vitaGL
backend and its probes. The device needs the shader compiler module
`libshacccg.suprx`; the VPKs do not redistribute it.

See [Building and integration](docs/building.md) for dependencies, probe title
IDs, embedding `aurora::vita_backend`, ABI selection and native binary audits.

## Validation status

The renderer revision [`ff5b2cf`](https://github.com/robin994/aurora-vita/commit/ff5b2cf5ab6866fbf6e7708a0978157ddc5c0e7b),
validated on **2026-10-02**, passed **11/11 host CTest targets** and **11/11
AddressSanitizer/UndefinedBehaviorSanitizer targets**. Default GXM and the
async/direct experimental configuration built both probe VPKs; all four
ELF/map audits passed and packaged eboots matched their SELF files.

These checks establish host contracts and build integrity. The current refactor
has no device image comparison or measured gameplay FPS result. Earlier hardware
probe results are dated separately. The [refactor report](platforms/vita/gxm/GXM_REFACTOR_2026-10-02.md)
records the tested configuration, artifact hashes, reference switches and device
comparison procedure.

## Documentation

- [Architecture and current status](docs/architecture.md): execution flow, ownership, invariants and source map.
- [Build and game integration](docs/building.md): standalone presets, embedding and ABI propagation.
- [Vita wiki](docs/wiki/Home.md): GXM/VitaGL settings, runtime tuning and diagnostics.
- [Native GXM implementation](platforms/vita/gxm/README.md): native EFB paths, coverage and historical integration evidence.
- [Hardware validation](docs/wiki/Diagnostics-and-Validation.md): framebuffer checks, artifact identity and performance sampling.

## Upstream Aurora

Aurora was originally developed for [Metaforce](https://github.com/AxioDL/metaforce).
The upstream desktop stack uses SDL3 and Dawn/WebGPU, with PAD, DVD, CARD and
other compatibility components. Those integrations have separate build paths
from the standalone Vita renderer. See the [desktop build instructions](docs/building.md#desktop-upstream-build)
and [upstream repository](https://github.com/encounter/aurora).

## License

Aurora is licensed under the [MIT License](LICENSE).
