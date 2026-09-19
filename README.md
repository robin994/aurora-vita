<div align="center">
  <img src="assets/aurora.png" alt="Logo" width="640">
</div>
<br/>

Aurora is a source-level GameCube & Wii compatibility layer intended for use with game decompilation projects.

> **Aurora Vita is an experimental project made for fun and research. It is not a serious, production-ready, or
> officially supported port. Expect incomplete functionality, rough edges, and breaking changes.**

Originally developed for use in [Metaforce](https://github.com/AxioDL/metaforce), a Metroid Prime reverse engineering project.
It now powers several completed source ports, including [Dusklight](https://github.com/TwilitRealm/dusklight).

### Features

- Application layer using SDL3
  - Runs on Windows, Linux, macOS, iOS, tvOS, Android
- GX compatibility layer
  - Graphics API support: D3D12, Vulkan, Metal
  - Highly accurate and performant GX implementation
  - Robust pipeline cache system with "transferable" cache support for releases
  - Dolphin-compatible texture pack support
  - Widescreen & resolution scaling support
  - Custom APIs for offscreen rendering
- PAD compatibility layer
  - Utilizes `SDL_Gamepad` for wide controller support, including GameCube controller adapters
  - Automatically saves and loads controller bindings and port mappings
  - Gyro & mouse support
- DVD compatibility layer
  - Utilizes [nod](https://github.com/encounter/nod) to support all GameCube/Wii disc image types, including RVZ
- CARD compatibility layer
  - Full compatibility with Dolphin `.gci` and `.raw` for game saves
- [Dear ImGui](https://github.com/ocornut/imgui) built-in for simple debug UIs

### Graphics

The GX compatibility layer is built on top of [WebGPU](https://www.w3.org/TR/webgpu/), a cross-platform graphics API
abstraction layer. WebGPU allows targeting all major platforms simultaneously with minimal overhead. The WebGPU
implementation used is Chromium's [Dawn](https://dawn.googlesource.com/dawn/).

![Screenshot](assets/screenshot.png)

### Building

See [docs/building.md](docs/building.md) for build instructions, CMake integration, and configuration options.

### PS Vita backends

Aurora Vita provides two mutually exclusive experimental hardware renderer paths:

- **VitaGL** for the vitaGL-based renderer.
- **GXM** for the native `sceGxm` renderer.

The Vita backend exposes both conservative defaults and opt-in performance paths for native GX
textures, CMPR/BC1, streaming, reduced internal resolution, D16 depth, GPU geometry, diagnostics,
memory tuning, and runtime logging (`BackendConfig::log_level`, including a fully quiet `Silent`
mode). Experimental options can change framebuffer output or synchronization behavior,
so they should be validated on real hardware before becoming a port default.

For performance/release builds where even the runtime log-level check is unwanted, configure
`-DAURORA_VITA_RUNTIME_LOGGING=OFF`. Aurora Vita also exposes
`performance_snapshot()` so CPU/GPU timing can be sampled without printing logs.

Persistent shader/program caches and pipeline warmup data are isolated automatically per title under
`ux0:data/aurora-vita/<TITLE_ID>/` on Vita. This keeps Aurora-owned cache files separate from each
homebrew's custom `ux0:data/<game-folder>/` layout. Ports normally should not override this root.

See the [Aurora Vita Wiki](docs/wiki/Home.md) for:

- the complete [experimental flag matrix](docs/wiki/Experimental-Flags.md);
- [native GXM configuration](docs/wiki/GXM-Backend.md);
- [VitaGL configuration](docs/wiki/VitaGL-Backend.md);
- [runtime and memory tuning](docs/wiki/Runtime-Tuning.md);
- [hardware validation and profiling](docs/wiki/Diagnostics-and-Validation.md);
- [known-safe configuration recipes](docs/wiki/Configuration-Recipes.md).

### License

Aurora is licensed under the [MIT License](LICENSE).
