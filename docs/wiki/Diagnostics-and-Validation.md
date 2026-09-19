# Diagnostics and validation

Performance work on Aurora Vita must be correctness-first. A build that compiles or boots does not prove framebuffer parity.

## Host contracts

```sh
cmake --preset vita-host-tests
cmake --build --preset vita-host-tests --parallel 8
ctest --preset vita-host-tests --output-on-failure
```

These tests cover CPU-side contracts, renderer selection, command-stream lifetime, worker synchronization, texture/decode behavior, and regression cases. They do not execute the Vita GPU.

## GXM probe

```sh
export VITASDK=/usr/local/vitasdk
cmake --preset vita-gxm
cmake --build build/vita-gxm --parallel 8
python3 tools/check_vita_gxm_binary.py \
  build/vita-gxm/aurora_vita_gx_probe \
  --map build/vita-gxm/aurora_vita_gx_probe.map \
  --nm "$VITASDK/bin/arm-vita-eabi-nm"
```

The binary audit verifies that the native GXM path does not pull in GL/vitaGL/vita2d graphics ownership.

## VitaGL probe

```sh
export VITASDK=/usr/local/vitasdk
cmake --preset vita-vitagl
cmake --build build/vita-vitagl --parallel 8
```

Use `AURORA_VITA_BUILD_SDL3_PROBE=ON` only when validating SDL3 native platform services with VitaGL.

## Hardware test matrix

For each experimental switch compare at least:

- title/menu;
- character/model screen;
- gameplay;
- long-distance geometry;
- shadows and render-to-texture effects;
- asymmetric EFB copy/crop;
- flip X/Y;
- RGB565 and alpha;
- mipmapped texture at multiple camera distances;
- pause/resume and scene transitions.

## Performance protocol

1. Use the same Aurora commit and game commit.
2. Use the same Vita clocks and power state.
3. Warm the scene before sampling.
4. Record at least 300 representative frames.
5. Compare median, p95 and p99, not only average FPS.
6. Separate CPU API submission time from GPU execution time.
7. Record scene count, EFB copies, stream recycles, uploads and cache hit/miss metrics.
8. Capture a screenshot/framebuffer reference when testing a visual-path flag.

## Accepting a flag

An experimental flag should become a project default only when:

- framebuffer output matches the control build in the tested categories;
- no new crashes or synchronization failures appear;
- the improvement is repeatable across multiple runs;
- memory growth is understood;
- the exact build/configuration is documented.

Keep title-specific overrides in the port until the behavior is validated broadly enough for an Aurora-wide default.
