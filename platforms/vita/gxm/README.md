# Native SceGxm backend - first vertical slice

This is an additive experimental renderer, originally based on Aurora-Vita
`10c683b` and integrated with the ABI/ARM32 fixes from `ced956a`.
The existing vitaGL renderer remains the default and is not replaced. The native
renderer is not yet a drop-in implementation of the GX DrawSink/EFB facade.
Do not select it for a game until that adapter and its missing capabilities exist.

## Separation

`cmake/aurora_vita.cmake` selects one implementation at configure time:

- `VITAGL`: historical `aurora_vita_backend`, also named `aurora::vita_vitagl_backend`.
- `GXM`: `aurora_vita_gxm_backend`, also named `aurora::vita_gxm_backend`.

Both expose the selected target through `aurora::vita_backend` and link the new
`aurora::vita_common` target. The common target owns the existing CPU workers,
vertex decoding/transforms, texture decoding, pipeline keys and telemetry. Their
source paths remain under `gfx/` to avoid breaking downstream source-list builds.
Only the original vitaGL target compiles the GL resource classes, GLSL generator,
GL command submission, streaming arena and GX DrawSink. The native target does
not compile or link any of those GL implementations.

`gxm::Renderer` consumes the shared `gfx::PipelineDesc`, `gfx::DrawPacket`,
`gfx::TextureDesc`, vertex layouts and uniforms. It owns a native context, display
queue, sync objects, mapped memory, shader patcher and native programs. Its pimpl
keeps GXM types out of the public renderer header. No OpenGL entry points or
vitaGL/vita2d library are needed. vitaShaRK and SceShaccCgExt are used only for
native Cg-to-GXP compilation; no GLSL translation is involved.

## Implemented

- Checked initialization, partial-failure cleanup, two/three display buffers,
  DF32 depth surface, presentation, discarded presentation, and shutdown.
- Move-only GPU memory ownership, separate ordinary/vertex-USSE/fragment-USSE mappings.
- Immutable bounded vertex/index buffers and resource handles. U16 indices are
  checked against both their slice and GXM's index range below 64000.
- Direct Cg generation for direct TEV stages: add/subtract, compare operations,
  register outputs, konst inputs, swap tables, alpha test and destination alpha.
- CPU-prepared vertices, explicit column-major projection upload, optional
  already-projected positions, texture coordinates and vertex colors.
- Native depth/cull/blend state, per-unit texture descriptor copies and pixel-exact
  scissor implemented by fragment rejection rather than tile clipping alone.
- Single-mip GX textures decoded by the existing shared decoder into padded
  RGBA8 native linear textures. Filtering is point/linear; wrapping is native.
- Explicit diagnostic framebuffer readback after GPU completion.

The first hardware run exposed `SCE_GXM_ERROR_UNSUPPORTED` when changing the min
filter of a LINEAR_STRIDED texture. The upload now uses an ordinary native linear
texture with eight-texel row padding. This is a renderer correction, not a game
identifier or asset-specific workaround.

## Deliberate limits

This milestone is not the completed Aurora GX port. The following remain:

- GX DrawSink/legacy Renderer facade, FIFO integration and shared command dispatch.
- GX EFB copies, render-target switching, channel/depth copy conversions and RAM readback semantics.
- Indirect TEV, fog, GX z-compare-location semantics, logic operations and polygon offset.
- Mip chains, runtime mip generation, native compressed texture upload, cache
  invalidation/eviction and streaming buffer retirement.
- GPU-side fixed GX transforms, skinning/lighting/texgen acceleration, instancing,
  and common expansion of lines/points into triangles for this renderer.
- Persistent native shader cache, failure dumps, capability negotiation and
  broad differential tests against the existing backend/reference captures.

Unsupported operations return errors; they do not silently use vitaGL or draw
fallback geometry. Configuration fails for GXM combined with the existing GX
frontend or vitaGL SDL probe. Strikers' own source-list integration likewise
rejects `AURORA_VITA_RENDERER=GXM` instead of silently building a GL executable.

Resources are created between scenes and retained until shutdown, within explicit
budgets. There is no premature eviction or CPU overwrite of in-flight buffers.
This conservative policy is appropriate for the initial probe, not a final
streaming game workload. Scissor discard also needs validation against early-Z
semantics before claiming GX fidelity. Direct TEV generation is not a claim of
bit-exact GameCube arithmetic across every operation.

## Build

From the Aurora repository root, with an installed VitaSDK and native compiler
libraries and `VITASDK` set in the environment, select one of the checked-in
presets. Each has a separate output directory and can be rebuilt independently:

```sh
# Preserve the OpenGL/vitaGL implementation.
cmake --preset vita-vitagl
cmake --build --preset vita-vitagl --parallel 8

# Select the exclusively SceGxm implementation (standalone renderer/probe).
cmake --preset vita-gxm
cmake --build --preset vita-gxm --parallel 8
python3 tools/check_vita_gxm_binary.py build/vita-gxm/aurora_vita_gxm_probe \
  --map build/vita-gxm/aurora_vita_gxm_probe.map

# Shared CPU, native shader-generation and renderer-selection contract tests.
cmake --preset vita-host-tests
cmake --build --preset vita-host-tests --parallel 8
ctest --preset vita-host-tests
```

Preset VPKs are `build/vita-vitagl/aurora_vita_probe_diag2.vpk` and
`build/vita-gxm/aurora_vita_gxm_probe.vpk`. The preset switch selects the hardware
implementation, not interchangeable game APIs or a runtime toggle. GXM still
rejects the game frontend until the shared DrawSink/EFB adapter is implemented.

Without presets, the same choice is exposed as the single cache setting
`AURORA_VITA_RENDERER=VITAGL|GXM`. `cmake/AuroraVitaRendererSelection.cmake` also
exposes/validates it for downstream source-list integrations. Neither value
silently falls back to the other; unknown or combined values fail configuration.

Equivalent manual native build:

```sh
cmake -S . -B build-gxm \
  -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DAURORA_ENABLE_VITA_BACKEND=ON \
  -DAURORA_VITA_BACKEND_ONLY=ON \
  -DAURORA_ENABLE_GX=OFF \
  -DAURORA_VITA_WITH_GX_FRONTEND=OFF \
  -DAURORA_VITA_RENDERER=GXM \
  -DAURORA_VITA_BUILD_PROBE=ON
cmake --build build-gxm -j8
python3 tools/check_vita_gxm_binary.py build-gxm/aurora_vita_gxm_probe \
  --map build-gxm/aurora_vita_gxm_probe.map
```

Output: `build-gxm/aurora_vita_gxm_probe.vpk`, title ID `AURVGXM01`.
The unstripped ELF and linker map are retained for dependency verification.
Both standalone builds use VitaSDK's default enum/wchar ABI. Game-specific ABI
flags remain the responsibility of the downstream game integration.

For the preserved renderer, use another build directory and
`-DAURORA_VITA_RENDERER=VITAGL`. Do not link both context owners into one process.

## Probe and validation

The probe renders a rotating, colored, textured cube: 24 vertices, 12 cube
triangles plus one clear triangle. It uses the existing CPU vertex pipeline and
a real tiled GX RGB565 checkerboard through the existing texture decoder.

It runs for 600 frames, with a 120-frame interval FPS log. Start exits early;
holding Cross applies a non-tile-aligned scissor rectangle. Framebuffer capture
and the automatic pixel-scissor test run after the timed loop, not inside FPS
intervals. A successful run reports `exit=0`, a nonzero inside-scissor pixel count,
and zero changed pixels outside the scissor rectangle.

Runtime files:

```text
ux0:data/aurora-vita/gxm_probe.log
ux0:data/aurora-vita/gxm_probe.ppm
```

The native Cg compiler module must already be available to vitaShaRK on the
console (default `ur0:/data/libshacccg.suprx`). The VPK does not redistribute it.

The dependency-free host contract test exercises 1,024 texture/color input-mask
combinations, TEV source emission, rejection of unsupported/invalid descriptors,
projection/depth algebra and the shared CPU decoders:

```sh
cmake -S . -B build-vita-host \
  -DCMAKE_BUILD_TYPE=Release \
  -DAURORA_ENABLE_VITA_BACKEND=ON \
  -DAURORA_VITA_BACKEND_ONLY=ON \
  -DAURORA_ENABLE_GX=OFF \
  -DAURORA_VITA_RENDERER=VITAGL \
  -DAURORA_VITA_BUILD_BACKEND_TESTS=ON
cmake --build build-vita-host -j8
ctest --test-dir build-vita-host --output-on-failure
```

Host tests validate source generation/contracts, not compilation by SceShaccCg
or GPU output. Hardware probe success covers its exercised pipelines, not every
TEV combination. Probe FPS is not Strikers/Melee gameplay FPS and does not
establish that removing vitaGL resolves CPU-side GX processing bottlenecks.

## Next integration boundary

Introduce a hardware-neutral submission/resource contract below DrawSink instead
of duplicating the FIFO decoder. Move legacy buffer mapping and GPU completion
out of the common streaming policy; each hardware backend must own allocation,
submission and retirement. Add EFB surfaces/copies and immutable draw snapshots
before permitting the game frontend to select GXM. Compare captured GX commands
across implementations and report unsupported features explicitly.

## Public API references

- VitaSDK GXM headers: https://github.com/vitasdk/vita-headers/blob/master/include/psp2/gxm.h
- VitaSDK display headers: https://github.com/vitasdk/vita-headers/blob/master/include/psp2/display.h
- vitaShaRK compiler: https://github.com/Rinnegatamante/vitaShaRK

These are public homebrew headers/compiler sources, not a claim to use Sony's
restricted SDK documentation.
