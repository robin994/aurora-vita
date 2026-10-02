# Aurora Vita: GX/GXM performance refactor

Implementation authorized on 2026-10-01, validated on 2026-10-02. Source baseline:
`321ac195e6dc8d086e2312a2881860e9c491c946`, branch `vita-experiment`, initially clean.
Delivery was requested on 2026-10-02. Before delivery, remote commit
`a6b69f31051597e051e4305439c8d3d2797fa198` (GXM retirement hardening and CPU3
probe infrastructure) was integrated
without conflicts and validation repeated on the combined sources. The delivery
target is `origin/vita-experiment`; the resulting commit is recorded by Git.
The delivered renderer commit is
`ff5b2cf5ab6866fbf6e7708a0978157ddc5c0e7b`, verified against the remote branch.
This report supersedes historical assumptions about the current defaults;
GXM still defaults to 8 MiB of static geometry and
lit fixed-vertex GPU processing. Resolution and the EFB/scissor algorithms retain
their existing behavior.

## Implemented behavior

The original flow remains GX/FIFO → command processor → DrawSink → decode,
transform and pack → CommandStream → native renderer → SceGxm.

- `GXState::mark_dirty` publishes separate vertex, fragment, texture and raster
  revisions. Matrix/light/material updates rebuild vertex inputs; TEV colors,
  indirect matrices and fog uniforms rebuild fragment inputs. Scissor and clear
  writes avoid both work and texture resolution. Unclassified writes still
  invalidate all domains. A distinct state identity prevents revision reuse
  across reset. Pipeline/layout generations keep their existing ownership.
- The layout-only bridge reads vertex attributes and arrays directly, avoiding
  full TEV/fog `ShaderConfig` construction. CPU and GPU draw recipes retain
  structural requirements, decode masks and packed layout/stride until the
  relevant pipeline or GPU expansion flags change. Live matrices and guest
  resource pointers remain outside the recipe.
- Uniform reflection capacities are resolved when a native pipeline is built,
  rather than querying component and array counts for every upload.
- Native binding compares actual vertex/fragment programs, depth function,
  depth write and cull state within a scene. A fragment-only change can emit one
  setter instead of seven. Every BeginScene still invalidates all bindings;
  program setters invalidate the corresponding default uniform reservation.
- Adjacent streamed triangle lists can share a native draw when pipeline,
  viewport, exact scissor, uniforms, fixed-state snapshot, texture identity,
  sampler, EFB sampling mode and UV scale/bias agree. Both buffer slices must
  be contiguous, unflushed and owned by the current arena slot. All indices are
  validated before rebasing, and combined vertex count stays below 64,000.
  Barrier, clear, copy and target commands terminate the run. No ordering changes.
  Existing 16-byte arena padding is preserved and can prevent small ranges from
  merging; the optimization never consumes padding as geometry.
- Streamed and cached fixed-geometry packets share immutable uniform/texture
  state stored in a canonical packet in the same batch. Deque storage preserves
  its address until reset; geometry fields may merge while state stays immutable.
  There is no second state payload allocation per draw. Copying a CommandStream
  or inserting a packet from another stream materializes its state; moving a
  stream preserves references and resets the moved-from object. Existing inline
  packet producers continue to work through uniform/texture accessors.
- `completed_performance_snapshot()` returns a mutex-protected copy of the most
  recent completed frame without queueing a callback or draining GX. The existing
  synchronous `performance_snapshot()` contract remains available. The initial
  and post-shutdown value has `completedFrame=false`; `frameIndex` counts completed
  frames, while log records retain their original zero-based frame identifier.
- Telemetry includes fragment translations, native setters emitted/skipped,
  uniform upload calls/bytes, state copies and local-batch candidate/merge/rejection
  counts. Finish calls are attributed to explicit barriers, buffer mutation,
  texture mutation, resource destruction, readback, target mutation, stream page
  reuse and discarded presentation. Reason arrays are cumulative counters and
  CPU wall time; diff consecutive snapshots for an interval. The diagnostic
  `[NATIVE_STATE]` line exposes them without per-draw logging.

Resource retirement remains synchronized. No device evidence establishes that
destruction waits dominate this checkout, so replacing completion with an
unverified frame heuristic would violate the lifetime contract. The new counters
make that decision measurable without removing existing synchronization.

## Runtime controls

The batching default is OFF pending hardware image and gameplay comparison:

```cpp
aurora::vita::BackendConfig config;
config.gxm_local_draw_batching = true;
```

For a runtime A/B, preserve existing feature bits and add/remove
`gxbridge::RuntimeLocalDrawBatching` (`0x80`) through
`aurora_vita_debug_set_runtime_flags`. The setter flushes pending commands before changing
mode. `aurora_vita_debug_runtime_capabilities` advertises local batching only on GXM.
When direct draw submission is compiled in, enabled local batching takes the
queued path so a per-draw flush cannot defeat merging.

Additional `BackendConfig::gxm_disable_mask` controls:

| Bit | Reference behavior |
| --- | --- |
| `0x0800` | Rebuild uniform/texture state on every dirty write |
| `0x1000` | Emit all seven native pipeline setters on a pipeline transition |
| `0x2000` | Keep one streamed packet per logical draw |
| `0x4000` | Copy inline uniform/texture state into every packet |

`0x7800` combines these four controls; it is not an old binary. Existing flags
remain intact. `0x0010` disables all native fragment-uniform reuse and also
disables producer state sharing. Exact partial scissor, EFB orientation/opacity and the
complete CPU fallback are preserved. Fixed-vertex snapshots still use their
existing byte-exact comparison rather than guessing equality from mutable pointers.

## Validation and evidence

Before changes: fresh host configure/build, **8/8 CTest targets passed** (2.54 s).
Host tests now compile the real Dawn-free GX frontend, command processor, FIFO,
bridge and DrawSink. Test-only Vita thread/semaphore shims provide scheduling on
the host; they are never linked into a Vita build and prove no Vita performance.

New coverage checks layout equivalence over direct/Index8/Index16, all eight vertex
formats and XYZ/NBT/NBT3; split fragment updates; scissor/clear/matrix/layout/reset
transitions; legacy dirty and disabled-domain controls; batching boundaries and
atomic invalid-index rejection; pooled packet lifetime, copy/move and reference
mode; native setter transitions; concurrent snapshot reads; and a completed-frame
reader while the actual FIFO worker is blocked. The comparator rejects aggregate
phase averages and mixed/reset sessions.

Final results after remote integration (host test durations include concurrent
Vita builds and are validation timings, not renderer benchmarks):

- Release host CTest: **11/11 passed**, 13.72 s. Frontend translation and
  transitions: **714 checks, 0 failures**. Submission/lifetime/state binding:
  **71 checks, 0 failures**. These counts exclude the pre-existing contracts.
- Debug AddressSanitizer + UndefinedBehaviorSanitizer: **11/11 passed**, 94.59 s.
- The integrated worker test verifies three helpers/four lanes, CPU3 probe
  fallback, capped renderer dispatch, range coverage, callback failure propagation
  and clean shutdown through the host semaphore/thread shims.
- CommandStream warm-up: 2,048 packets × 20 frames, **zero warm allocations**,
  1,088-byte host DrawPacket. Packet storage grows to its high-water mark; inline
  state remains for compatibility, so this is reduced copying, not a smaller
  packet claim. The contiguous local-batch fixture preserves nine vertices and
  nine indices while reducing three logical triangle lists to one packet.
- VitaSDK default `vita-gxm` configure/build/package: passed. The separate
  `build/vita-gxm-fast` build also passed with ASYNC_GX, GXM_DIRECT_STREAM_WRITE
  and GXM_DIRECT_DRAW_SUBMIT all ON. Default build flags remain OFF for these
  experimental options. Both configurations produce GX and native probe VPKs.
- Binary audits: all four ELF/map pairs passed. Native draw/present symbols were
  found and no GL/vgl/vita2d entry points or libraries were linked. Symbols inspected:
  default GX 2,166, default native 1,842, fast GX 2,173, fast native 1,854.
- Every VPK's `eboot.bin` was byte-identical to its newly built local `.self`.
  `git diff --check` passed. No hardware deployment or game benchmark was run.

Build/test logs for delivery are in `/tmp/aurora-*-delivery-20261002.log`.

| VPK relative to this checkout | Bytes | SHA-256 |
| --- | ---: | --- |
| `build/vita-gxm/aurora_vita_gx_probe.vpk` | 622123 | `b188a5f34b829a0042c4838703dd964832428558d0aeb9a6f0fabc841a4b8aa7` |
| `build/vita-gxm/aurora_vita_gxm_probe.vpk` | 382919 | `dd540efbbb91be18f4775c5d21cf89cbbdb70296709fa1d59b7161deb350039e` |
| `build/vita-gxm-fast/aurora_vita_gx_probe.vpk` | 637043 | `9f2db88bd5c4553f34eeeddd80ff195f916fd9c91ebfd4c26747efd52b1e9899` |
| `build/vita-gxm-fast/aurora_vita_gxm_probe.vpk` | 391318 | `6e47a6a35f5bc71b9b54b568f645122926dea5be0915b4addd4c21a5449e57c3` |

Eboot identities for installation (hash `eboot.bin`, not the ZIP container;
no device installation was performed):

- `build/vita-gxm/aurora_vita_gx_probe.vpk`: `fea299368f506c1789618aac47f390970fc80079ce6c8528226845a13db2d97b`
- `build/vita-gxm/aurora_vita_gxm_probe.vpk`: `aaf0ef94729b981c67782d2ca4daecd5f2ab8a6e83da9d2dcef3878e5fc05fac`
- `build/vita-gxm-fast/aurora_vita_gx_probe.vpk`: `4778459f6ed57882b5aafd952d20ca3bf22c542795a3f5403cafe66c03e7ce06`
- `build/vita-gxm-fast/aurora_vita_gxm_probe.vpk`: `70d280cdd0f4fcdb224a532172b8cea31d5c63b81221ddc612506e5d85b5ec73`

## Device comparison

Use the same title, scene, asset set, clocks, resolution, cache warm-up and capture
options in both runs. Verify the installed eboot SHA-256 against the candidate
artifact. A standalone GX/native probe establishes a renderer build, not the
performance or shadows of a downstream game using an embedded Aurora copy.

Compare the baseline build, then the candidate with the reference mask, then
candidate domains/state sharing and finally local batching. Use identical logging
in each run. Inspect shadows/EFB alpha and orientation, partial scissors, texture
updates/TLUT, indexed PN, lines/points, CPU fallback, scene/target switches and
PS-button suspend/resume. Inspect merge counts and rejected-state/index counts
before assuming draw calls were reduced. Compare frame median/p95/p99, stalls,
scene count, native setter/upload counts, memory high water and finish reason
deltas. CPU submission timers and nested GX phases are not GPU timestamps.

For a recorded telemetry comparison:

```sh
python3 tools/compare_vita_performance.py reference.log candidate.log \
  --reference-id 'eboot hash; title/scene; config' \
  --candidate-id 'eboot hash; same title/scene; config' \
  --warmup 60 --output comparison.json
```

Warm-up excludes **logged samples**, not unseen frames. Percentiles use linear
interpolation over sample ranks. Reports disclose frame gaps: period-300 telemetry
is a sampled distribution, not consecutive frame pacing. `phase_profile.log`
contains 120-frame averages and cannot establish frame p95/p99. No current device
capture is included, so no FPS gain or absence of visual regressions is claimed.

## Codex model choice

Aurora contains no OpenAI model/API runtime to migrate. The coding workflow is
documented in root `AGENTS.md`: GPT-6 Astra with high reasoning for cross-cutting
renderer work; routine isolated edits can use GPT-6.1 Sol. Select the model with
the Codex picker; this change does not edit global settings or switch the active
conversation model. Keep tool reads batched and use the same evidence and hardware
gates for either model. No OpenAI dependency or call is added to a Vita frame.
