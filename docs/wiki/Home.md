# Aurora Vita Wiki

Aurora Vita is the experimental PS Vita backend for Aurora's GameCube/Wii compatibility layer.

This wiki documents the two mutually exclusive hardware renderer paths:

- **VitaGL** — OpenGL-like Vita renderer built on vitaGL.
- **GXM** — native `sceGxm` renderer with no GL/vitaGL dependency in the final GXM probe.

The Vita backend is still experimental. Performance switches that change texture layout, raster resolution, vertex processing, depth precision, or streaming behavior must be validated on real hardware before becoming a port default.

## Start here

- [Experimental flags](Experimental-Flags.md) — complete flag matrix and defaults.
- [Native GXM backend](GXM-Backend.md) — GXM-only switches, risks, and recommended baseline.
- [VitaGL backend](VitaGL-Backend.md) — VitaGL-only switches and memory tuning.
- [Runtime tuning](Runtime-Tuning.md) — `BackendConfig` fields shared by both backends.
- [Diagnostics and validation](Diagnostics-and-Validation.md) — probes, telemetry, framebuffer checks, and acceptance criteria.
- [Configuration recipes](Configuration-Recipes.md) — known-safe starting configurations.

## Renderer selection

Only one hardware renderer should own the graphics context in a process.

```sh
cmake --preset vita-gxm
cmake --build build/vita-gxm --parallel 8
```

or:

```sh
cmake --preset vita-vitagl
cmake --build build/vita-vitagl --parallel 8
```

The compile-time selector is:

```cmake
-DAURORA_VITA_RENDERER=GXM
```

or:

```cmake
-DAURORA_VITA_RENDERER=VITAGL
```

## Policy for experimental switches

1. Keep the conservative/default path as the control.
2. Enable one experimental switch at a time.
3. Test on hardware with the same game scene, clocks, and build type.
4. Compare correctness before comparing frame time.
5. Record the exact Aurora commit and the port configuration.
6. Do not promote a switch to a project default from a successful build alone.

See [Diagnostics and validation](Diagnostics-and-Validation.md) for the full protocol.
