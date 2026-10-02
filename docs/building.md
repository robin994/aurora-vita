# Building and integrating Aurora Vita

The device presets build the standalone, Dawn-free Vita backend. The desktop
stack has a separate build path described at the end of this page.

## Prerequisites

- CMake 3.25 or later, Git and a C++20 compiler for host tests.
- Python 3 for the FRAME-log comparator and binary-audit contract tests.
- VitaSDK for device builds, with `VITASDK` pointing to its installation.
- Vita shader/runtime libraries used by the selected backend: vitaShaRK,
  SceShaccCgExt, taiHEN stubs and mathneon, alongside VitaSDK system stubs.
- vitaGL for `VITAGL` builds. Its exact archive is fingerprinted for program-cache
  compatibility; native GXM does not link this archive.

The shared backend fetches pinned xxHash and robin-hood-hashing dependencies
during configuration unless the parent project already provides their targets.
The initial configure therefore needs access to those dependencies. Device
builds use VitaSDK's toolchain and packaging helpers; host tests use the host
compiler and do not require vitaGL or a Vita graphics context.

Clone this fork:

```sh
git clone --branch vita-experiment https://github.com/robin994/aurora-vita.git
cd aurora-vita
```

## Host contract tests

```sh
cmake --preset vita-host-tests
cmake --build --preset vita-host-tests --parallel 8
ctest --preset vita-host-tests --output-on-failure
```

The host preset enables the real Dawn-free GX frontend with test-only
thread/semaphore shims. With Python available, the current suite contains 11
CTest targets covering CPU/shader contracts, byte comparison, vertex packing,
regressions, frontend translation, command-stream lifetime, submission,
workers, renderer selection, performance-log comparison and ELF classification.
These tests do not execute Vita GPU programs or prove device scheduling.

For a separate sanitizer configuration with Clang/GCC:

```sh
cmake --preset vita-host-tests -B build/vita-host-sanitizers \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build/vita-host-sanitizers --parallel 8
ctest --test-dir build/vita-host-sanitizers --output-on-failure
```

## Device builds

```sh
export VITASDK=/usr/local/vitasdk

cmake --preset vita-gxm
cmake --build --preset vita-gxm --parallel 8

cmake --preset vita-vitagl
cmake --build --preset vita-vitagl --parallel 8
```

Use separate build directories for the two renderer owners. Both device
presets enable `AURORA_VITA_BACKEND_ONLY`, `AURORA_VITA_WITH_GX_FRONTEND` and
`AURORA_VITA_BUILD_PROBE`, select `SDK` ABI, and disable the desktop GX/Dawn and
SDL3 probe paths.

| Preset | VPK relative to repository root | Title ID |
| --- | --- | --- |
| `vita-gxm` | `build/vita-gxm/aurora_vita_gx_probe.vpk` | `AURGXGM01` |
| `vita-gxm` | `build/vita-gxm/aurora_vita_gxm_probe.vpk` | `AURVGXM01` |
| `vita-vitagl` | `build/vita-vitagl/aurora_vita_gx_probe.vpk` | `AURGXGL01` |
| `vita-vitagl` | `build/vita-vitagl/aurora_vita_probe_diag2.vpk` | `AURVPRB02` |

The corresponding ELF, `.map` where configured, `.velf` and `.self` outputs
remain in each build directory. These probes exercise renderer contracts and
do not package a game or its assets.

The device shader compiler module must already be installed. The GXM renderer
uses vitaShaRK's default module path, normally `ur0:/data/libshacccg.suprx`;
the VitaGL initialization path also checks the
external-module layout under `ur0:/data/external/`. Neither VPK contains this
module. Inspect `last_init_failure()` and `last_init_failure_detail()` when
initialization fails.

### Experimental native submission build

Use a separate directory when testing async GX and direct native streaming:

```sh
cmake --preset vita-gxm -B build/vita-gxm-fast \
  -DAURORA_VITA_ASYNC_GX=ON \
  -DAURORA_VITA_GXM_DIRECT_STREAM_WRITE=ON \
  -DAURORA_VITA_GXM_DIRECT_DRAW_SUBMIT=ON
cmake --build build/vita-gxm-fast --parallel 8
```

All three flags default to `OFF`. Local triangle batching is a separate runtime
setting; when enabled it selects queued submission so direct per-draw flushes
do not prevent merging. Refer to the [flag matrix](wiki/Experimental-Flags.md)
before changing the texture, depth or geometry controls.

### Audit the final GXM executable

```sh
python3 tools/check_vita_gxm_binary.py build/vita-gxm/aurora_vita_gx_probe \
  --map build/vita-gxm/aurora_vita_gx_probe.map \
  --nm "$VITASDK/bin/arm-vita-eabi-nm"
```

Run the same audit for the native probe and for the final executable of a
consuming game. It requires native draw/present symbols and rejects linked
GL/vitaGL/vita2d renderer functions or libraries. It cannot validate rendering
output. Record the source revision and compare the packaged `eboot.bin` hash
with the installed bytes before a hardware performance comparison.

## Embed in a game

Use the Vita-only entry point to avoid loading the desktop dependency stack or
duplicating Aurora's source list:

```cmake
set(AURORA_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern/aurora-vita")
set(AURORA_VITA_RENDERER GXM CACHE STRING "" FORCE)
set(AURORA_VITA_WITH_GX_FRONTEND ON CACHE BOOL "" FORCE)
set(AURORA_VITA_WITH_UPSTREAM_GX OFF CACHE BOOL "" FORCE)
set(AURORA_VITA_PORT_ABI SDK CACHE STRING "" FORCE)
include("${AURORA_SOURCE_DIR}/cmake/aurora_vita_embed.cmake")
aurora_add_vita_backend()
target_link_libraries(game PRIVATE aurora::vita_backend)
```

Call the function once. Choose `GAMECUBE` ABI only for a port that retains
GameCube-style short wchar and 32-bit enums; this propagates `-fshort-wchar`
and `-fno-short-enums`. Every object library compiling public Aurora headers
must inherit the backend's usage requirements, not only the final executable.
`AURORA_VITA_UPSTREAM` affects public class layout and is exported by the target.

The game owns initialization, frame boundaries and shutdown through
`aurora::vita`, and owns invalidation for guest memory changes. Display-list
shadows and immutable geometry reuse require publication of relevant guest
writes; see [Architecture](architecture.md) and `lib/gx/fifo.hpp`.

The [runtime configuration](wiki/Runtime-Tuning.md) and
[recipes](wiki/Configuration-Recipes.md) document source defaults and explicit
CPU reference configurations. Persistent caches use the application's title ID
automatically; a port normally leaves `data_root_path=nullptr`.

## SDL3 platform probe

`AURORA_VITA_BUILD_SDL3_PROBE=ON` is a VitaGL-only integration target. It also
requires `AURORA_VITA_SDL3_NATIVE=ON` and an SDL3-providing parent build; the
standalone Vita presets disable this path. The SDL platform layer handles
events/services while Aurora/vitaGL owns graphics. Native GXM rejects the
SDL3/vitaGL probe combination during configuration.

## Desktop upstream build

The inherited desktop implementation uses SDL3 and Dawn/WebGPU. Build it in a
separate directory without the Vita backend-only preset:

```sh
cmake -S . -B build/desktop
cmake --build build/desktop --target simple
```

The executable is produced under `build/desktop/examples/`. A desktop consumer
can use `add_subdirectory(extern/aurora EXCLUDE_FROM_ALL)` and link components
such as `aurora::core`, `aurora::gx`, `aurora::main` and `aurora::vi`.
See [examples/simple.c](../examples/simple.c) for the application API.

The top-level desktop defaults include `AURORA_ENABLE_GX=ON`,
`AURORA_ENABLE_DVD=OFF`, `AURORA_ENABLE_CARD=ON`, `AURORA_ENABLE_THP=ON` and
`AURORA_CACHE_USE_ZSTD=ON`. Device backend-only builds return before loading
those desktop targets. A successful Vita build does not validate the desktop
examples or vice versa.
