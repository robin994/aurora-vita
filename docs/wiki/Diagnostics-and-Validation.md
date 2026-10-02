# Diagnostics and validation

Performance work on Aurora Vita must be correctness-first. A build that compiles or boots does not prove framebuffer parity.

## Host contracts

```sh
cmake --preset vita-host-tests
cmake --build --preset vita-host-tests --parallel 8
ctest --preset vita-host-tests --output-on-failure
```

These tests cover CPU-side contracts, renderer selection, command-stream lifetime, worker synchronization, texture/decode behavior, and regression cases. They do not execute the Vita GPU.

At renderer revision `ff5b2cf` (2026-10-02), the real GX frontend is enabled in
the host preset. With Python 3 available, all 11 CTest targets passed, including
714 frontend checks, 71 submission checks and zero allocations after
CommandStream warmup. All 11 also passed with ASan/UBSan. See
[Building](../building.md#host-contract-tests) for the sanitizer configuration.

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
Both GX and native probes built in the default and async/direct configurations;
all four executable/map audits passed. The
[refactor report](../../platforms/vita/gxm/GXM_REFACTOR_2026-10-02.md) records VPK
and eboot hashes. Current refactor hardware/gameplay validation remains pending.

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

1. Record reference/candidate Aurora and game commits. For a flag A/B, use the same executable and change only the intended setting.
2. Use the same Vita clocks and power state.
3. Warm the scene before sampling.
4. Record at least 300 representative frame samples and state the sample interval. Default period-300 diagnostics do not capture consecutive pacing.
5. Compare median, p95 and p99, not only average FPS.
6. Separate CPU API submission time from GPU execution time.
7. Record scene count, EFB copies, stream recycles, uploads and cache hit/miss metrics.
8. Capture a screenshot/framebuffer reference when testing a visual-path flag.

Use `completed_performance_snapshot()` with `AURORA_VITA_RUNTIME_LOGGING=OFF`
for observation without a GX queue drain. Check `completedFrame` and avoid
counting repeated `frameIndex` values as new samples. The older synchronous
`performance_snapshot()` may fence the worker. Display-queue waits above the
500 us blocked-frame threshold are a backpressure indicator; they are not an
independent GPU execution measurement.

For a FRAME telemetry log comparison:

```sh
python3 tools/compare_vita_performance.py reference.log candidate.log \
  --reference-id 'eboot hash; title/scene; settings' \
  --candidate-id 'eboot hash; same title/scene; settings' \
  --warmup 60 --output comparison.json
```

The comparator consumes `[AURORA-VITA][FRAME]` records with `total_us`, rejects
duplicate/reset or unordered sessions, and reports sample gaps. Warmup excludes
logged samples, not unseen frames. Percentiles use linear interpolation. Choose
the same telemetry settings in both runs; logging/profiling has a cost.
`phase_profile.log` contains 120-frame averages and cannot provide frame p95/p99.
Nested DrawSink/GX phases must not be summed as independent work.

`[NATIVE_STATE]` includes native setters, uploads, shared-state copies and
cumulative finish counts/wall time for explicit barriers, buffer/texture mutation,
destruction, readback, target mutation, stream reuse and discarded frames. Diff
consecutive snapshots for interval finish costs. Native submission timers remain
CPU timings; mask bits `0x0100`/`0x0200` deliberately serialize GPU work and must
be identified in the capture.

## CPU3 evidence

Capture `<data_root>/cpu3_probe.log` and the results of `core3_available()`,
`core3_cpu_id()` and `core3_affinity_mask()`. A configured plugin is insufficient:
the helper must report core ID 3 and the expected affinity. Record the two lane
caps alongside actual worker count. The startup idle-clock sample establishes
neither sustained utilization nor a total CPU3 budget.

## Accepting a flag

An experimental flag should become a project default only when:

- framebuffer output matches the control build in the tested categories;
- no new crashes or synchronization failures appear;
- the improvement is repeatable across multiple runs;
- memory growth is understood;
- the exact build/configuration is documented.

Keep title-specific overrides in the port until the behavior is validated broadly enough for an Aurora-wide default.
