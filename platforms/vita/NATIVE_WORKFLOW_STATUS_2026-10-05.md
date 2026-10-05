# Native material and preparation candidate, 2026-10-05

This source snapshot adds incremental fixed-vertex uniform preparation,
specialized arithmetic materials with a reference generator, compact explicit
texture mips, conservative exact CMPR/BC1 conversion helpers, prepared geometry
recipe reuse and a portable CPU microbenchmark. It preserves the GX frontend,
draw ordering, resource retirement, fallback paths and per-BeginScene reset.

The Strikers integration produced a **GPU memory fault on Vita**. Device
acceptance and performance comparison are incomplete. Do not treat this snapshot
as a proven stable renderer or a measured FPS improvement.

The matching Strikers SELF is
`0563a5c5e7fd9c12fdd8b3305859d71132778874f94bbe95ae5664145c0bd4c4`;
dump `psp2core-1791191504-GPUCRASH.psp2dmp` contains BIF_INT_STAT `0x90400`
and BIF_FAULT `0x72e14061` in one of four register columns. The fault page is
inside an `aurora-gxm` 16 KiB USER allocation. No CPU thread has an exception
stop reason. The GX thread was in buffer allocation, which does not by itself
establish the cause of an asynchronous GPU fault.

Follow-up: `DrawSink::submit` refreshed XF vertex-program state without rebuilding
the CPU recipe or invalidating the GPU recipe. The transition regression keeps
the fragment/base pipeline generation unchanged while alternating register and
vertex material sources: 30,891 checks / 8 failures before, 0 failures after the
patch. The refresh now rebuilds the CPU recipe and invalidates the GPU recipe so
its layout and packing follow the current program after primitive flags are set.

The post-fix Strikers SELF is
`be6d1a0ebe17889181ff1de623c63e1864a81a9df3227be46d83f209ba9d643f`.
It passed 16/16 host tests, the frontend regression under ASan/UBSan, the full
Release Vita/VPK build and the GXM-only ELF/map audit (12,686 executable symbols).
With native candidates active (`gxm_disable=0x8`), it completed 180 diagnostic
live-play frames, then a diagnostics-OFF run with screenshot at play-frame 60
and 1,200 sampled live-play frames 600–1799 without a new dump. A second long run
without screenshot completed 1,200 live-play frames 0–1199 without a new dump.
The three post-fix sessions contain 2,580 samples. These bounded tests do not prove the original
fault was caused by this recipe defect: pipeline validation can reject some
stale layouts and the original dump lacks the offending draw/buffer contents.
Matched performance comparison and broader device acceptance remain incomplete.

Current implementation limits:

- Resident geometry uses `create_buffer` / `MemoryKind::CpuGpu`; migration to
  the CDRAM GPU resource pool remains pending.
- Exact CMPR admission requires `TextureDesc::cacheable`, but the GXM texture
  facade clears that flag because it owns cache retirement. The new BC1 path
  is therefore not reached through this frontend. Immutable-source eligibility
  needs a separate representation before enabling it there.
- Specialized materials cover arithmetic stages. Indirect and comparison
  programs continue through the existing generator. The TEV frontend remains.
- Reference bits are `0x10000` for the old uniform builder, `0x20000` for the
  old fragment generator and `0x40000` for the new compact texture paths.
- The material and texture guards do not isolate the prepared geometry change.

Validation before delivery: 16/16 host tests, sanitizer checks for materials,
compact textures and geometry, standalone `vita-gxm` cross-build, full Strikers
VPK build and a GXM-only binary audit passed. Intro screenshots were identical
between material candidate/reference, but live-play visual/performance parity
has not been established. Smaller generated Cg source did not consistently
produce smaller compiled fragments. Host tests do not validate GPU memory
mapping or the actual GXM mip layout on device.

See the integrating Strikers repository's `GPU_CRASH_ANALYSIS_2026-10-05.md`
and `PERFORMANCE_NATIVE_WORKFLOW_2026-10-05.md` for the exact build identities,
excluded benchmark runs, dump evidence and next hardware checks. Dumps, retail
assets and compiled artifacts are kept locally outside the source commit.
