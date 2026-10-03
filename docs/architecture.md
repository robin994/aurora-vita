# Aurora Vita architecture and current status

This guide describes renderer revision `ff5b2cf` and the CPU3/retirement work
integrated from `a6b69f3`, as of 2026-10-02. Source declarations and CMake options
remain authoritative for configuration.

## Execution and ownership

```text
Game callbacks / Dolphin GX and VI entry points
  → FIFO producer and command processor
  → GX state, layout and texture translation
  → DrawSink: decode/transform/pack or eligible fixed-vertex GPU preparation
  → streaming/static buffers + immutable command packets
  → gfx::Renderer facade
  → selected vitaGL or native GXM implementation
  → presentation / EFB / readback
```

One selected backend owns graphics resources and context state. The render
thread performs submission; CPU helper threads only prepare ranges. Native GXM
optionally runs FIFO consumption and submission on a dedicated GX thread with
`AURORA_VITA_ASYNC_GX=ON`. Original FIFO ordering and game frame callbacks remain
part of the integration contract.

`aurora::vita_backend` exports the common interface and ABI definitions.
`aurora::vita_common` contains CPU vertex/texture processing, keys and telemetry.
`cmake/aurora_vita_frontend.cmake` attaches the shared frontend; the selected
backend adds its hardware resource implementation. A native GXM executable
does not load the vitaGL resource path or Dawn.

## State and packet reuse

Decoded GX writes publish vertex, fragment, texture and raster revisions.
Known matrix/light writes update vertex inputs; TEV/fog uniform writes update
fragment inputs. Clear/scissor updates avoid unrelated translation. Unknown
writes retain broad invalidation, and a separate state identity protects reset.
Pipeline and decode-layout generations continue to define structural changes.

Draw recipes retain immutable requirements, semantic masks and packing layout.
They do not capture live guest memory or matrices. Packet-state sharing points
to an immutable canonical packet in the same stream; copies materialize that
state and moves preserve ownership. Deque high-water storage avoids repeated
packet allocation after warmup, without claiming smaller packet objects.

Native uniform capacities are resolved at pipeline creation. Binding comparison
can skip unchanged vertex/fragment programs, depth function/write and cull
setters **within a scene**. Every BeginScene resets context bindings, and changing
a program invalidates its default uniform reservation. Caching context state
across scenes previously caused a PS-button suspend/resume crash.

## Optional local draw batching

`BackendConfig::gxm_local_draw_batching` defaults to `false`. Adjacent triangle
packets can merge only when pipeline, viewport, pixel-exact scissor, uniforms,
fixed-state snapshot, texture/sampler and EFB UV semantics agree. Slices must be
contiguous in the current arena and their index data must still be unflushed.
Index validation completes before rebasing; combined vertices stay below 64,000.

Clear, EFB copy, target changes and barriers terminate adjacency. Existing arena
padding can prevent a merge. Enabling batching does not imply that a particular
title has mergeable geometry; measure `batchCandidates`, `batchMerged` and the
state/index rejection counters. Direct draw submission selects the queued path
when batching is enabled.

## Resource lifetime and guest writes

GPU-visible resources remain alive through queued work. Native retirement,
mutation, destruction and EFB fallback paths retain their completion rules.
Finish-reason counters expose where synchronization happens; they do not replace
GPU completion with an assumed frame delay.

The port must publish guest memory writes for display-list shadows, source
revision tracking, texture invalidation and immutable geometry reuse. Preserve
TLUT and EFB invalidation as well as explicit source-range updates. When source
stability or feature eligibility is unavailable, preparation uses the CPU path.

On Vita, guest-memory reuse has two explicit phases. A caller that is about to
overwrite or recycle GPU-visible storage calls `aurora_vita_prepare_memory_write()`;
while the async consumer is active this conservatively retires earlier FIFO work.
After bytes have changed, `aurora_vita_notify_memory_write()` publishes the new
revision without fencing. Keeping these operations separate is important because
GameCube cache-store/flush calls also occur on audio and other pthreads that have
no relationship to GX. The current prepare operation uses a global drain;
range-aware retirement may replace it without weakening the ordering guarantee.

GX serial waits use the monotonically increasing completion counter as their
authority. The wake semaphore is only a hint: waits periodically re-check the
counter so multiple producer-side waiters cannot deadlock by competing for one
binary semaphore token. Because the semaphore has a maximum count of one, the
consumer publishes only an edge when no wake token is already pending; it must
not signal once per completed job or Vita reports `SCE_KERNEL_ERROR_SEMA_OVF`.

Async GX also treats `end_frame()` as a producer/consumer lifetime boundary.
FIFO decode and rendering may overlap the game thread while a frame is being
built, but `end_frame()` does not return until that frame has been consumed and
presented by the GX worker. This prevents the game from recycling frame-owned
guest memory or display resources while the consumer still references them.

The GX worker is the sole owner of decoded `GXState`, `DrawSink`, renderer state
and their caches while async GX is active. Producer-side shortcuts must not
decode or submit draws directly. In particular, the single-draw display-list
fast path is disabled with the worker running; those bytes are appended to the
same FIFO as the preceding state commands and decoded in-order by the consumer.

## CPU3 probe and lane caps

The default is two helpers, with CPU2 and lower-priority CPU1 affinities. Requesting
three helpers also attempts the system-core affinity. Initialization accepts that
helper only after it reports physical CPU ID 3 and the expected affinity; failure
retains the original topology. Plugin installation alone is not proof of CPU3.

`cpu_renderer_execution_lanes` and `cpu_game_execution_lanes` independently cap
caller plus helper lanes. A cap of three keeps the probed fourth helper outside
the corresponding dispatch. This infrastructure does not implement a measured
CPU3 utilization quota or guarantee total CPU3 use by a downstream game.

## Observation and validation

`completed_performance_snapshot()` reads the latest successfully completed frame
under a short mutex without draining GX. Check `completedFrame`; it is false
before the first completed frame and after shutdown. The synchronous
`performance_snapshot()` may fence the worker and remains available for callers
that require that contract. CPU submission timings are distinct from GPU time.

Current validation passed 11 host tests, all 11 with ASan/UBSan, two native build
configurations and four ELF/map audits. The
[2026-10-02 report](../platforms/vita/gxm/GXM_REFACTOR_2026-10-02.md) records exact
artifacts and controls. Current hardware image/gameplay/FPS validation remains
pending; the [device protocol](wiki/Diagnostics-and-Validation.md) describes it.

## Source map

| Responsibility | Main source |
| --- | --- |
| Backend lifecycle and public configuration | `platforms/vita/aurora_vita_backend.hpp`, `.cpp` |
| FIFO ordering and GX register writes | `lib/gx/fifo.cpp`, `command_processor.cpp`, `state_revisions.hpp` |
| Pipeline/layout/uniform translation | `platforms/vita/gx/aurora_gx_bridge.cpp` |
| Draw preparation and selection | `platforms/vita/gx/aurora_vita_draw_sink.cpp`, `gfx/vita_draw_adapter.cpp` |
| Packet state and local batching | `platforms/vita/gfx/vita_command_stream.hpp`, `vita_draw_batch.hpp` |
| Stream/static lifetime | `platforms/vita/gfx/vita_streaming_arena.cpp`, `vita_static_geometry.hpp` |
| Native resources, programs and scenes | `platforms/vita/gxm/gxm_facade.cpp`, `gxm_renderer.cpp` |
| CPU helper protocol | `platforms/vita/gfx/vita_cpu_workers.cpp` |
| Completed telemetry and comparison | `platforms/vita/gfx/vita_published_snapshot.hpp`, `vita_telemetry.cpp`, `tools/compare_vita_performance.py` |

Historical audits under `platforms/vita/` retain their original measurements
and proposals. Their defaults and incomplete feature descriptions are dated
snapshots; use the [flag matrix](wiki/Experimental-Flags.md) for current settings.
