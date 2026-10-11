#include "../lib/gx/gx.hpp"
#include "../lib/gx/fifo.hpp"
#include "../lib/gx/native_model_registry.hpp"
#include "aurora_vita_backend.hpp"
#include "gx/aurora_vita_draw_sink.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>

using namespace aurora::gx::fifo;
using namespace aurora::vita;
#define CHECK(expr) do { if (!(expr)) { \
  std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #expr); std::exit(1); \
} } while (false)

size_t draw_count() {
  size_t count = 0;
  run_sync([](void* p) {
    auto& n = *static_cast<size_t*>(p);
    for (const auto& command : draw_sink().stream().commands())
      if (command.type == gfx::CommandType::Draw) ++n;
  }, &count);
  return count;
}

int main() {
  BackendConfig config{};
  config.cpu_worker_threads = 0;
  config.wait_vblank = false;
  config.static_geometry_budget = 1024 * 1024;
  config.static_geometry_min_vertices = 3;
  config.log_level = RuntimeLogLevel::Silent;

  CHECK(initialize(config));
  CHECK(start_worker());
  CHECK(begin_frame());
  run_sync([](void*) {
    auto& g = aurora::gx::g_gxState;
    g = aurora::gx::GXState{};
    g.proj.m0 = {1,0,0,0}; g.proj.m1 = {0,1,0,0};
    g.proj.m2 = {0,0,1,0}; g.proj.m3 = {0,0,0,1};
    g.renderViewport = {0,0,960,544,0,1};
    g.logicalViewport = g.renderViewport;
    g.renderScissor = {1,2,959,542};
    g.logicalScissor = g.renderScissor;
    g.numChans = 1; g.numTevStages = 1;
    g.tevStages[0].channelId = GX_COLOR0A0;
    g.tevStages[0].colorPass.d = GX_CC_RASC;
    g.tevStages[0].alphaPass.d = GX_CA_RASA;
    g.colorChannelConfig[GX_COLOR0].matSrc = GX_SRC_VTX;
    g.colorChannelConfig[GX_ALPHA0].matSrc = GX_SRC_VTX;
    g.vtxDesc[GX_VA_POS] = GX_DIRECT;
    g.vtxFmts[0].attrs[GX_VA_POS] = {GX_POS_XYZ,GX_F32,0};
    g.vtxDesc[GX_VA_CLR0] = GX_DIRECT;
    g.vtxFmts[0].attrs[GX_VA_CLR0] = {GX_CLR_RGBA,GX_RGBA8,0};
    g.mark_dirty();
  }, nullptr);

  std::array<uint8_t,64> list{};
  list[0] = 0x90; list[2] = 3;
  for (unsigned n = 0; n < 3; ++n) {
    const std::array<float,3> pos{float(n==1),float(n==2),0};
    for (unsigned j = 0; j < 3; ++j) {
      const uint32_t bits = std::bit_cast<uint32_t>(pos[j]);
      for (unsigned b = 0; b < 4; ++b)
        list[3+n*16+j*4+b] = uint8_t(bits >> (24-b*8));
    }
    list[3+n*16+12] = 32;
    list[3+n*16+13] = 64;
    list[3+n*16+14] = 128;
    list[3+n*16+15] = 255;
  }
  gfx::note_memory_write(list.data(),list.size());

  NativeModelDescriptor descriptor{};
  descriptor.displayList = reinterpret_cast<uintptr_t>(list.data());
  descriptor.displayListBytes = uint32_t(list.size());
  descriptor.view = 1; descriptor.program = 3;
  descriptor.vertexCount = 3;
  descriptor.primitive = 0; descriptor.streamCount = 1;
  descriptor.streams[0] = {reinterpret_cast<uintptr_t>(list.data()),
                           uint32_t(list.size()),0,16,true};
  CHECK(descriptor.valid());

  NativeModelRegistry registry;
  int emitted = 0;
  auto emit = [&](const void* bytes,uint32_t length) noexcept {
    ++emitted;
    write_stable_data(bytes,length);
  };
  auto stats = native_model_stats();
  CHECK(registry.dispatch_or_capture(descriptor,list.data(),
                                     uint32_t(list.size()),emit));
  CHECK(emitted == 1 && registry.size() == 1 && draw_count() == 1);
  CHECK(registry.lookup(descriptor));
  const auto first = native_model_stats();
  CHECK(first.attempted == stats.attempted+1);
  CHECK(first.fallback == stats.fallback+1);

  // Same semantic identity in this frame bypasses the game's GX emitter.
  CHECK(registry.dispatch_or_capture(descriptor,list.data(),
                                     uint32_t(list.size()),emit));
  CHECK(emitted == 1 && draw_count() == 2);
  const auto second = native_model_stats();
  CHECK(second.attempted == first.attempted+1);
  CHECK(second.dispatched+second.fallback ==
        first.dispatched+first.fallback+1);

  // A real revision change forces the *consumer* to execute the original GX
  // bytes in-order, rather than dropping the draw or reusing an old GPU mesh.
  list[15] ^= 1;
  gfx::note_memory_write(list.data(),list.size());
  CHECK(registry.dispatch_or_capture(descriptor,list.data(),
                                     uint32_t(list.size()),emit));
  CHECK(emitted == 1 && draw_count() == 3);
  const auto mutated = native_model_stats();
  CHECK(mutated.fallback == second.fallback+1);

  // Two materials sharing the same GX DL address are distinct registrations.
  auto other = descriptor; ++other.materialState;
  CHECK(!registry.lookup(other));
  CHECK(registry.dispatch_or_capture(other,list.data(),
                                     uint32_t(list.size()),emit));
  CHECK(emitted == 2 && registry.size() == 2 && draw_count() == 4);

  // R3-A: a cold GLG source cannot bypass anything. Once it has produced a
  // native recipe, only an exact static producer state can use the early slot.
  NativeModelEarlyFacts early{};
  early.callbackFlags = 0x800;
  early.compiledDraw = early.stableView = early.staticUnlitProgram = true;
  early.identityMatrix = early.matchingLiveProgram = early.matchingLiveStreams = true;
  early.noUserData = early.producerClean = early.noDrawModifiers = true;
  CHECK(native_model_early_admissible(early));
  auto require_rejection = [&](NativeModelEarlyFacts candidate) {
    CHECK(!native_model_early_admissible(candidate));
  };
  {auto f = early; f.callbackFlags = 0x820; require_rejection(f);}
  {auto f = early; f.compiledDraw = false; require_rejection(f);}
  {auto f = early; f.stableView = false; require_rejection(f);}
  {auto f = early; f.staticUnlitProgram = false; require_rejection(f);}
  {auto f = early; f.identityMatrix = false; require_rejection(f);}
  {auto f = early; f.matchingLiveProgram = false; require_rejection(f);}
  {auto f = early; f.matchingLiveStreams = false; require_rejection(f);}
  {auto f = early; f.noUserData = false; require_rejection(f);}
  {auto f = early; f.producerClean = false; require_rejection(f);}
  {auto f = early; f.noDrawModifiers = false; require_rejection(f);}
  int flushes = 0;
  const auto flush = [&]() noexcept { ++flushes; };
  CHECK(!registry.dispatch_prepared(early, descriptor, flush));
  CHECK(flushes == 0);
  auto permanent = descriptor;
  permanent.sourceGeneration = registry.register_source(
      permanent.displayList,list.data(),uint32_t(list.size()),1);
  CHECK(permanent.sourceGeneration != 0);
  CHECK(!registry.dispatch_prepared(early, permanent, flush));
  CHECK(flushes == 0);
  CHECK(registry.dispatch_or_capture(permanent,list.data(),uint32_t(list.size()),emit));
  CHECK(emitted == 3 && draw_count() == 5);
  CHECK(registry.dispatch_prepared(early,permanent,flush));
  CHECK(flushes == 1 && emitted == 3 && draw_count() == 6);
  {auto different = permanent; ++different.materialState;
    CHECK(!registry.dispatch_prepared(early,different,flush));}
  {auto untracked = permanent; untracked.sourceGeneration = 0;
    CHECK(!registry.dispatch_prepared(early,untracked,flush));}
  CHECK(flushes == 1);
  size_t retired = 0;
  CHECK(registry.release_sources_after(0,[&](const NativeModelSourceSpan&) {
    ++retired;
  }) == 1);
  CHECK(retired == 1 && !registry.lookup(permanent));
  CHECK(!registry.dispatch_prepared(early,permanent,flush));
  CHECK(flushes == 1);
  end_frame();
  registry.clear();
  shutdown();

  // Worker unavailable: return control to the caller so it can emit legacy
  // GX on its normal path; no cache handle survives to an invalid context.
  CHECK(!registry.dispatch_or_capture(descriptor,list.data(),
                                      uint32_t(list.size()),emit));
  CHECK(emitted == 3);
  std::puts("native model registry: first GX capture, same-frame reuse, "
            "source revision fallback, distinct material, R3-A early guards, "
            "source retirement, shutdown fallback");
}
