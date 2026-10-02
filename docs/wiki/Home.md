# Aurora Vita Wiki

Aurora Vita is the experimental PS Vita backend for Aurora's GameCube/Wii compatibility layer.

This wiki documents the two mutually exclusive hardware renderer paths:

- **VitaGL** — OpenGL-like Vita renderer built on vitaGL.
- **GXM** — native `sceGxm` renderer with no GL/vitaGL dependency in the final GXM probe.

The Vita backend is still experimental. This guide reflects the source at
renderer revision `ff5b2cf` (2026-10-02). GXM currently enables an 8 MiB immutable
geometry cache and lit fixed-vertex GPU processing for eligible draws. The explicit
CPU reference profile differs from those defaults. Local batching, native packed
GXM textures, async/direct submission and extended GPU features remain optional.

The latest validation passed 11 host targets, the same suite with ASan/UBSan,
two GXM build configurations and four ELF/map audits. Hardware validation of the
current refactor remains pending; dated probe runs are not current gameplay evidence.

## Start here

- [Build and integration](../building.md) — dependencies, probes and embedding in a game.
- [Architecture](../architecture.md) — frontend flow, state ownership and current status.
- [Experimental flags](Experimental-Flags.md) — public flag matrix and source defaults.
- [Native GXM backend](GXM-Backend.md) — GXM-only switches, risks, and recommended baseline.
- [VitaGL backend](VitaGL-Backend.md) — VitaGL-only switches and memory tuning.
- [Runtime tuning](Runtime-Tuning.md) — `BackendConfig` fields shared by both backends.
- [Diagnostics and validation](Diagnostics-and-Validation.md) — probes, telemetry, framebuffer checks, and acceptance criteria.
- [Configuration recipes](Configuration-Recipes.md) — explicit controls and individual experiments.
- [GX/GXM refactor report](../../platforms/vita/gxm/GXM_REFACTOR_2026-10-02.md) — validation, artifact hashes and reference masks.

## Renderer selection

Only one hardware renderer should own the graphics context in a process.
Set `VITASDK` to the SDK installation before using either device preset.

```sh
cmake --preset vita-gxm
cmake --build --preset vita-gxm --parallel 8
```

or:

```sh
cmake --preset vita-vitagl
cmake --build --preset vita-vitagl --parallel 8
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

1. Record the source defaults and choose an explicit reference configuration.
2. Enable one experimental switch at a time.
3. Test on hardware with the same game scene, clocks, and build type.
4. Compare correctness before comparing frame time.
5. Record the exact Aurora commit and the port configuration.
6. Do not promote a switch to a project default from a successful build alone.

See [Diagnostics and validation](Diagnostics-and-Validation.md) for the full protocol.
