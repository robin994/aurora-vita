# Interchangeable Vita renderers: vitaGL and native SceGxm

The same public `aurora::vita` API, `gfx::Renderer` facade, GX frontend, FIFO
command processor and DrawSink now compile with either hardware implementation:

```text
AURORA_VITA_RENDERER=VITAGL
AURORA_VITA_RENDERER=GXM
```

vitaGL remains the default. The GXM build does not compile the GL resource
implementations, generate GLSL or link vitaGL/vita2d. Both expose the selected
implementation as `aurora::vita_backend`. This is source/build-time
interchangeability, not a runtime hot switch or identical coverage of every GX
feature. The earlier standalone milestone is archived in `BRINGUP_NOTES.md`.

## One frontend, separate hardware ownership

`cmake/aurora_vita_frontend.cmake` owns the frontend source list for both backends:
GX/VI entry points, FIFO processing, state translation, DrawSink, draw preparation,
streaming policy, capture/replay and diagnostics. `aurora::vita_common` shares CPU
vertex/texture decoding, transformation/lighting/texgen, keys and telemetry.

The existing vitaGL implementations remain under `gfx/`. Native implementations
of those same resource/renderer APIs live in `gxm/gxm_facade.cpp` and forward to
`gxm::Renderer`. Games use the facade, not the device-specific renderer directly.
The native device owns context, mapped memory, shader patcher, native programs,
color/depth surfaces, display queue and sync objects. Cg is compiled through
vitaShaRK; no OpenGL translation is involved.

`BufferPool::wait_idle()` moves hardware completion out of the shared streaming
allocator. Native GXM conservatively completes scenes before resource mutation,
destruction or EFB transitions. The same DrawSink respects the native index limit
and uses the existing CPU vertex path when fixed GX GPU transforms are unavailable.

The renderer and GX ABI definitions propagate PUBLIC through the CMake target.
Every object-library consumer must inherit them: `AURORA_VITA_UPSTREAM` changes
DrawSink's public layout and cannot be a backend-private compile definition.

## Build either Aurora implementation

From the Aurora repository root with VitaSDK and the shader compiler libraries:

```sh
export VITASDK=/usr/local/vitasdk

cmake --preset vita-vitagl
cmake --build --preset vita-vitagl --parallel 8

cmake --preset vita-gxm
cmake --build --preset vita-gxm --parallel 8

cmake --preset vita-host-tests
cmake --build --preset vita-host-tests --parallel 8
ctest --preset vita-host-tests
```

Both device presets enable `AURORA_VITA_WITH_GX_FRONTEND=ON` and compile the same
`platforms/vita/probe/gx_frontend_main.cpp`:

```text
build/vita-vitagl/aurora_vita_gx_probe.vpk    AURGXGL01
build/vita-gxm/aurora_vita_gx_probe.vpk       AURGXGM01
```

The previous renderer-specific probes are retained too. Use separate build
directories; never link both context owners into one process. The Dawn-backed
`AURORA_VITA_WITH_UPSTREAM_GX` integration and vitaGL-only SDL probe are not part
of native GXM. Invalid combinations fail configuration explicitly.

Equivalent manual native configure:

```sh
cmake -S . -B build-gxm \
  -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DAURORA_ENABLE_VITA_BACKEND=ON \
  -DAURORA_VITA_BACKEND_ONLY=ON \
  -DAURORA_ENABLE_GX=OFF \
  -DAURORA_VITA_WITH_GX_FRONTEND=ON \
  -DAURORA_VITA_RENDERER=GXM \
  -DAURORA_VITA_BUILD_PROBE=ON
cmake --build build-gxm --parallel 8
```

## Embed in a game without duplicating Aurora sources

```cmake
set(AURORA_VITA_WITH_GX_FRONTEND ON CACHE BOOL "" FORCE)
set(AURORA_VITA_WITH_UPSTREAM_GX OFF CACHE BOOL "" FORCE)
# Only for ports retaining the GameCube enum/wchar layout:
set(AURORA_VITA_PORT_ABI GAMECUBE CACHE STRING "" FORCE)
include("${AURORA_SOURCE_DIR}/cmake/aurora_vita_embed.cmake")
aurora_add_vita_backend()
target_link_libraries(game PRIVATE aurora::vita_backend)
```

Link the usage requirements into every object target that uses Aurora public
headers, not only the final executable. Strikers attaches them to its shared
`port_flags` interface. `SDK` is the standalone default ABI; `GAMECUBE` exports
`-fshort-wchar -fno-short-enums` consistently to common/backend/consumer code.
GameCube-layout builds can still report wchar/enum warnings from SDK libraries.

Strikers now uses the embed entry point. From its repository root:

```sh
cmake -S smstrikers-port -B smstrikers-port/build-gxm \
  -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DAURORA_VITA_RENDERER=GXM
cmake --build smstrikers-port/build-gxm --parallel 8

cmake -S smstrikers-port -B smstrikers-port/build-vita \
  -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DAURORA_VITA_RENDERER=VITAGL
cmake --build smstrikers-port/build-vita --parallel 8
```

Both output `strikers_vita.vpk` in their own build directory. The game title ID
is still `SMSVITA01`, so installing one variant replaces the other.

## Shared validation

The shared hardware probe checks RGB/alpha/depth clear masks, EFB copy/resize,
channel conversion, render-to-texture sampling, explicit mip levels, buffer
mutation and fog/indirect shader compilation. It then sends 4,920 draws through
the actual GX FIFO over 120 frames with a deliberately tiny streaming ring,
forcing repeated rollover. It reports an exit status and captures the completed
framebuffer using the common API. Strict unsupported checking forbids white
fallback textures; an interior-pixel assertion checks the GXCopyTex destination
identity and sampled orientation, not just the number of submitted draws:

```text
ux0:data/aurora-vita/gx_frontend_GXM.log
ux0:data/aurora-vita/gx_frontend_GXM.ppm
ux0:data/aurora-vita/gx_frontend_VITAGL.log
ux0:data/aurora-vita/gx_frontend_VITAGL.ppm
```

Host tests cover CPU contracts, 1,024 shader input-mask combinations, native
indirect/fog emission, mip packing, EFB conversion, selector validation and the
ELF audit's symbol classification. These tests do not execute GPU code.

Audit the final native executable, not just a static library:

```sh
python3 tools/check_vita_gxm_binary.py build/vita-gxm/aurora_vita_gx_probe \
  --map build/vita-gxm/aurora_vita_gx_probe.map \
  --nm "$VITASDK/bin/arm-vita-eabi-nm"
```

The audit rejects GL/vitaGL/vita2d entry points and libraries and requires native
GXM draw/present functions. Engine data symbols whose names begin with `gl` are
not OpenGL calls. The console Cg module must already be present, normally at
`ur0:/data/libshacccg.suprx`; neither VPK redistributes it.

### Verified integration snapshot (2026-09-16)

- The shared GX probe and the complete Strikers consumer both produced linked
  ELF, SELF and VPK outputs with either renderer selected.
- Host tests passed 3/3, including 45,704 shader/CPU/layout checks over 1,024
  texture/color input masks. These are host checks, not 45,704 GPU tests.
- The complete GXM game ELF passed the native dependency audit (12,504 code
  symbols inspected). No GL/vitaGL/vita2d renderer entry points or libraries
  were linked.
- The final hardware runs of the same strict GX probe passed for both backends:
  20 fog/indirect shader variants compiled, explicit mip data was sampled,
  color/alpha clear masks and sampled EFB copies were checked, and 4,920 GX
  draws completed over 120 frames with repeated tiny-ring rollovers. Both
  reported `exit=0`. The original diagnostic executable was restored and
  verified after each run; the game installation was not changed.
- The checked copied-image pixel was `(217,120,186)` on both backend paths.
  Full 960x544 captures were not bit-identical: 11,000 pixels differed, maximum
  channel error was 8/255, and mean absolute channel error was 0.05111443/255.
  This validates the exercised layout/orientation, not bit-exact GX fidelity.

One earlier vitaGL run exceeded a 100-second harness deadline after the EFB
test. A rerun with per-variant progress logs and a 240-second limit completed;
the cause of the earlier timeout was not isolated. These are functional runs,
not an exhaustive stability soak. Strikers gameplay and game framerate were
not validated in this integration snapshot.

## Coverage and deliberate limits

Native GXM includes direct/indirect TEV generation, fog modes/range adjustment,
CPU-prepared vertex/color/texcoord streams, depth/cull/blend, alpha comparison,
destination alpha, samplers, power-of-two mip chains, source invalidation,
dynamic buffers, EFB targets, color copies, presentation and readback.

Both backends resolve copied images using `loadedTextures`, the table actually
populated by the native GX FIFO frontend, not Dawn's unresolved `TextureBind`
table. Native EFB capture normalizes its top-left readback rows to the shared
render-texture convention before applying the caller's flip flags.

EFB color copies currently finish GPU work and use a CPU reference conversion
and resize before writing native texture storage. This is a functional,
conservative path, not an optimized GPU copy. Clears cover the active target
with independent RGB/alpha/depth masks. LOD bias is expressed in mip levels in
the shared API and quantized to native eighth-level units at the device edge.
The vitaGL cache preserves explicit uncompressed mip data using its public
backing-store interop and the shared linear packer; it does not substitute
automatically generated levels. Backing storage remains owned/freed by vitaGL.

Remaining limits include depth-copy formats, polygon offset, bitwise logic
beyond CLEAR/COPY/NOOP, non-power-of-two mip chains, fractional LOD clamping,
GPU-side fixed transforms and persistent native program caching. Native direct
device draws require indexed triangle data; the shared GX path expands other
primitives and supplies those indices. Unsupported paths remain visible as
errors/coverage warnings and never silently use vitaGL.

A successful build or probe does not establish complete gameplay fidelity,
bit-exact GX arithmetic, early-Z equivalence or game framerate. Native scene
fencing, GPU EFB copies and compressed uploads are subsequent measured
optimizations, separate from making the frontend/consumer interchangeable.

## Public implementation references

- VitaSDK GXM declarations: https://github.com/vitasdk/vita-headers/blob/master/include/psp2/gxm.h
- vitaShaRK: https://github.com/Rinnegatamante/vitaShaRK
- vitaGL textures: https://github.com/Rinnegatamante/vitaGL/blob/master/source/textures.c
- Vita3K sampler decoding: https://github.com/Vita3K/Vita3K/blob/master/vita3k/renderer/src/gl/texture.cpp

These are public homebrew implementation references, not restricted Sony SDK
documentation. Hardware tests remain necessary for GXM behavioral assumptions.
