#include "../platforms/vita/gxm/gxm_precomputed_geometry.hpp"
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <deque>

using namespace aurora::vita::gxm;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); std::abort(); } } while (0)
struct Record { uint64_t stream = 0, indices = 0; uint32_t count = 0; };
using Cache = PrecomputedGeometryCache<Record, 2>;

int main() {
  SceneCompletion completion;
  CHECK(completion.next() == 1 && completion.completed() == 0 && completion.can_submit());
  completion.observe(1); // A future/stale foreign value cannot retire scene 1.
  CHECK(completion.completed() == 0);
  Cache cache;
  PrecomputedGeometryKey a{1, 100, 10, 11, 64, 32, 6, 3};
  auto b = a; b.vertexBase = 128;
  auto* first = cache.acquire(completion.completed()); CHECK(first);
  first->record = {1000, 2000, 6}; cache.publish(*first, a, completion.next());
  auto* second = cache.acquire(completion.completed()); CHECK(second && second != first);
  second->record = {3000, 4000, 6}; cache.publish(*second, b, completion.next());
  CHECK(!cache.acquire(completion.completed())); // Same scene, even before EndScene.
  CHECK(cache.find(a, completion.next()) == first); // Safe immutable repeated use.
  completion.submitted(); completion.observe(0);
  CHECK(!cache.acquire(completion.completed()));
  // Use a again in a second scene before the first scene completes.
  CHECK(cache.find(a, completion.next()) == first);
  completion.submitted(); completion.observe(1);
  CHECK(first->record.stream == 1000 && first->record.count == 6);
  // Invalidate a pending geometry/program: hide the key but retain GPU data.
  cache.invalidate([&](const auto& k) { return k.vertices == a.vertices; });
  CHECK(!cache.find(a, completion.next()) && !cache.find(b, completion.next()));
  CHECK(first->record.stream == 1000);
  auto* retired = cache.acquire(completion.completed()); CHECK(retired == second);
  auto c = a; c.vertexProgram = 101; c.indexCount = 12;
  retired->record = {5000, 6000, 12}; cache.publish(*retired, c, completion.next());
  CHECK(!cache.acquire(completion.completed()));
  completion.observe(0); // An older observation must not move completion back.
  CHECK(completion.completed() == 1);
  completion.observe(3); // An impossible future observation must not release c.
  CHECK(completion.completed() == 1);
  completion.observe(2);
  auto* nowRetired = cache.acquire(completion.completed()); CHECK(nowRetired == first);
  CHECK(retired->record.count == 12);
  // Geometry identity includes every address/range/count/primitive/program.
  for (unsigned field = 0; field < 8; ++field) {
    auto changed = c;
    switch (field) {
    case 0: ++changed.pipeline; break; case 1: ++changed.vertexProgram; break;
    case 2: ++changed.vertices; break; case 3: ++changed.indices; break;
    case 4: ++changed.vertexBase; break; case 5: ++changed.indexOffset; break;
    case 6: ++changed.indexCount; break; case 7: ++changed.primitive; break;
    }
    CHECK(!cache.find(changed, completion.next()));
  }
  // Notification rollover preserves the owner's 64-bit ordering.
  constexpr uint64_t wrap = uint64_t{1} << 32;
  CHECK(SceneCompletion::extend(wrap + 2, wrap - 2, 0xffffffffu) == wrap - 1);
  CHECK(SceneCompletion::extend(wrap + 2, wrap - 1, 0) == wrap);
  CHECK(SceneCompletion::extend(wrap + 2, wrap, 2) == wrap + 2);
  CHECK(SceneCompletion::extend(wrap + 2, wrap, 3) == wrap);
  // Exercise 100k scenes, variable completion delays, repeated immutable hits
  // and finish fences against an independent pending-payload oracle.
  PrecomputedGeometryCache<Record, 8> stress;
  SceneCompletion serial;
  struct Pending { const Record* address; Record snapshot; uint64_t scene; };
  std::deque<Pending> pending;
  for (uint64_t scene = 1; scene <= 100000; ++scene) {
    PrecomputedGeometryKey key = a; key.vertices = scene % 13;
    auto* entry = stress.find(key, serial.next());
    if (entry) CHECK(entry->record.stream == key.vertices);
    else if ((entry = stress.acquire(serial.completed()))) {
      entry->record = {key.vertices, key.indices, key.indexCount};
      stress.publish(*entry, key, serial.next());
    }
    // Independent oracle: every payload referenced by a pending GPU scene
    // retains its exact contents after hits, invalidation and pressure.
    for (const auto& use : pending) {
      CHECK(use.address->stream == use.snapshot.stream);
      CHECK(use.address->indices == use.snapshot.indices && use.address->count == use.snapshot.count);
    }
    if (entry) pending.push_back({&entry->record, entry->record, scene});
    if (scene % 23 == 0) stress.invalidate([](const auto& k) { return k.vertices % 2 == 0; });
    serial.submitted();
    if (scene > 5) serial.observe(uint32_t(scene - 5));
    if (scene % 97 == 0) serial.finished();
    while (!pending.empty() && pending.front().scene <= serial.completed()) pending.pop_front();
    CHECK(serial.completed() <= scene);
  }
  std::puts("PASS: immutable reuse, pending invalidation, multiple scenes, capacity fallback and completion ordering.");
}
