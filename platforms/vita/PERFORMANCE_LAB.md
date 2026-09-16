# Vita performance laboratory

## Validation status (2026-09-16)

The 60 FPS gameplay target has NOT been reached or validated. Successful host
unit tests and a successful Vita build are not evidence of correct GPU output
or of a particular hardware frame rate.

The existing CPU geometry path remains the default. GXCopyTex orientation,
GXCopyDisp presentation, CPU index rebasing and mapped-stream defaults are not
changed by these experiments.

Hardware measurements on a deterministic Strikers introduction scene confirmed
that removing redundant unlit vertex-color quantization reduced the CPU
transform phase. At the same sampled frame numbers 909, 919, 929 and 939, the
triangle counts were identical in the before/after runs. The transform medians
were 95.565 ms and 69.001 ms respectively. Whole-frame medians were 453.1265 ms
and 426.6075 ms, not 16.67 ms. This measures that scene and configuration only.

A separate CPU-path run with larger renderer/texture pools produced warm 3D
samples between 377.805 and 420.851 ms. It is not a controlled measurement of a
single change. Persistent magenta regions were still visible in a native
framebuffer snapshot even after shader compilation failures reached zero.

The fixed-GPU-path experiment has no valid 3D hardware result yet. The console
subsequently stopped before the match and its video showed the lock screen.
Repeating the earlier CPU-control executable with the same configuration also
stopped in the startup UI. Do not interpret those interrupted runs as a measured
GPU-path regression or a speedup. Resume validation on an unlocked console.

## Changes that retain the CPU path

- Unlit vertex-material colors copy their original bytes directly.
- Register color quantization uses a bounded integer/fraction conversion instead
  of calling libm for every color channel. Half-integer boundaries are tested.
- Fragment shader generation no longer references a texcoord varying omitted by
  liveness analysis when a TEV stage does not sample that coordinate.
- Out-of-range texture-palette lookups retain their diagnostic magenta color and
  now report a bounded number of source/index/size diagnostics. This identifies
  bad metadata instead of concealing it with a transparent or white replacement.

## Optional instrumentation

`BackendConfig::profile_split_vertex_phases` defaults to false. When enabled,
CPU decode and transform use separate passes for measurement. This changes
cache locality and worker dispatch and must not be mixed with fused-mode
samples without recording the distinction.

Additional telemetry fields include `buffer_upload_us`, `stream_wait_us`,
`vertex_pack_us`, `geometry_cache_us`, `draw_frontend_us` and
`state_translate_us`. Some phases are intentionally nested:

- vertex packing is inside command building;
- geometry-cache misses may include decode and upload work;
- draw frontend encloses translation, geometry, texture and command work;
- an EFB copy may include a queued draw flush.

Do not sum all phase fields as if they were disjoint. `submit_us` is CPU API
submission time, not a GPU timestamp measurement. Resource statistics include
stream recycles, GPU syncs, static geometry bytes and static geometry entries.

## Experimental fixed-PN GPU geometry (default OFF)

`BackendConfig::static_geometry_budget = 0` disables this path. Nonzero values
opt in to the following experiment:

1. Accept only non-expanded primitives with a fixed PN matrix, no explicit host
   index stream, no active lighting/emboss and no per-vertex matrix selectors.
2. Decode object-space inputs once into immutable vertex/index buffers.
3. Verify the original FIFO records, decode layout and every referenced source
   array span before reusing an entry. Hash matches alone are insufficient.
4. Snapshot position, texgen, post-texgen and material uniforms per queued draw.
   Never reference mutable live GX state from a queued packet.
5. Apply model/view and projection separately in the vertex shader; do not
   reuse the previous combined-matrix workaround.
6. Use the existing CPU path when eligibility, source verification, allocation
   or shader creation fails. A source changed in place becomes volatile and is
   not permitted to overwrite a GPU buffer still in flight.

The first cache is bounded by its byte budget and 1024 entries and deliberately
has no in-frame eviction. Entries live until shutdown. It is not yet a general
streaming cache for arbitrary changing levels or long sessions. Lighting,
emboss, dynamic palettes, expanded primitives and hardware image equivalence
remain validation/extension work. Keep this option disabled in normal builds
until actual GPU output and sustained performance have been checked.

## Experimental program-binary disk cache (default OFF)

`BackendConfig::program_binary_cache_path = nullptr` disables disk caching.
When enabled, binaries are keyed by both complete shader sources and the
attribute binding contract. A versioned header checks lengths, source identity,
private serializer bounds and an integrity checksum before loading. Writes use
a temporary file and a rename inside the dedicated cache directory.

`cmake/AuroraVitaProgramCache.cmake` binds the exact vitaGL archive and embeds its
SHA-256 fingerprint in the executable. A separate cache directory is used for
each archive fingerprint. Builds without a known fingerprint do not deserialize
cached programs. These are implementation-specific vitaGL binaries, not a
portable shader format. Treat the directory as a local, application-generated
cache rather than accepting arbitrary external program binaries.

Cache files were observed being created on hardware. A complete cold/warm 3D
comparison and a reliable warm-load run are still outstanding; no reduction in
gameplay shader-stall time is claimed yet.

## Acceptance gates before enabling experiments

Use the same compiled binary and a recorded configuration for each on/off
comparison. Verify the installed eboot with a readback hash. Keep a known-good
binary and the original configuration. Record actual presentation state, draw
counts, first shader compilation, steady-state samples and a native snapshot.
Check both menus and gameplay, not only a probe or the introduction scene.

Preserve the distinction between startup latency, CPU rendering throughput,
GPU execution, display presentation and game simulation. Do not report a
60 FPS overlay or a successful build as proof of sustained 60 FPS gameplay.
