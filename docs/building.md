# Building

## Prerequisites

- CMake 3.25 or later
- A C++20 compatible compiler (Clang, GCC, or MSVC)
- Git (for cloning)

## Building the Examples

To build Aurora's included examples:

```bash
git clone https://github.com/encounter/aurora.git
cd aurora
mkdir build && cd build
cmake ..
cmake --build . --target simple
```

The `simple` example demonstrates a minimal Aurora application with a blue screen. The built executable will be in `build/examples/simple`.

## Using Aurora in Your Project

Aurora is designed to be integrated as a library in GameCube/Wii decompilation projects.

**CMakeLists.txt example:**

```cmake
cmake_minimum_required(VERSION 3.25)
project(your_game)

# Add Aurora as a subdirectory
add_subdirectory(extern/aurora EXCLUDE_FROM_ALL)

# Create your executable
add_executable(your_game src/main.c)

# Link against Aurora components
target_link_libraries(your_game PRIVATE 
    aurora::core 
    aurora::gx 
    aurora::main 
    aurora::vi
)
```

See [examples/simple.c](../examples/simple.c) for a minimal application template.

## CMake Options

- `AURORA_ENABLE_GX` (default: ON) - Enable GX implementation and WebGPU renderer
- `AURORA_ENABLE_DVD` (default: OFF) - Enable DVD implementation backed by nod
- `AURORA_ENABLE_CARD` (default: ON) - Enable CARD implementation based on kabufuda
- `AURORA_CACHE_USE_ZSTD` (default: ON) - Compress WebGPU cache entries with zstd

## PS Vita backend performance options

The experimental Vita backend includes the optimization paths validated during the Vita porting work:

- `AURORA_VITA_DIRECT_STREAM_WRITE=ON` maps the current streaming VBO/IBO and writes frame data directly, avoiding the extra CPU staging-to-buffer copy. Leave it `OFF` for conservative stock-vitaGL builds; enable it with a vitaGL build whose mapped-buffer path is known to be stable.
- `AURORA_VITA_RUNTIME_MIPMAP_GENERATION=OFF` is the default. Explicit source mip levels are still uploaded, but missing chains are not generated at runtime on Vita, reducing allocation pressure and avoiding historical OOM failures. It can be re-enabled after validating runtime mip generation with the selected vitaGL build.

The Vita texture cache also pre-evicts LRU textures before allocation, preserves textures referenced by the current frame, validates vitaGL backing storage after upload, and tracks allocation/pre-eviction telemetry. EFB passthrough copies use a GPU blit fast path while GX copy formats that require channel or quantization conversion keep the shader conversion path.

### Native SDL3 platform layer on PS Vita

When cross-compiling with VitaSDK, `AURORA_VITA_SDL3_NATIVE` defaults to `ON`. Aurora vendors SDL3 3.4.4 as a static library and uses SDL native Vita backends for window/events/touch, gamepad/joystick, audio, filesystem, threading, locale, timer, power and sensors.

Aurora/vitaGL remains the only graphics owner. The Vita profile disables SDL GPU, SDL Renderer, PIB and PVR integration, and Aurora deliberately does not create an `SDL_Renderer` on Vita. The SDL window is fullscreen 960x544 and exists only as the native platform/event surface. This avoids a second SDL GXM renderer/context competing with vitaGL.

Vita SELF builds also disable position-independent code because `vita-elf-create` does not accept ARM PIC relocations such as `R_ARM_BASE_PREL`.

For an isolated hardware gate, configure `AURORA_VITA_BUILD_SDL3_PROBE=ON`. The resulting `aurora_vita_sdl3_probe.vpk` initializes SDL3 Vita platform services first, then the Aurora vitaGL backend, renders a changing clear color, polls SDL gamepad events, and exits when START is pressed.
