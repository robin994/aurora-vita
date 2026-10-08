#include "../platforms/vita/gxm/gxm_vertex_uniform_delta.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <array>
#include <cstring>
#include <cstdio>

using aurora::vita::gxm::VertexUniformDeltaCache;
using aurora::vita::gxm::VertexUniformSpan;

int main() {
  VertexUniformDeltaCache cache;
  assert(!cache.configure(0) && !cache.configure(4097));
  assert(cache.configure(2048));
  std::array<float,120> position{},normal{};
  std::array<float,16> material{},mvp{};
  for(unsigned i=0;i<120;++i) {position[i]=float(i+1);normal[i]=float(i*3);}
  for(unsigned i=0;i<16;++i) {material[i]=float(i+16);mvp[i]=float(i+32);}
  std::array<VertexUniformSpan,4> sources{{
      {mvp.data(),16},{position.data(),120},{normal.data(),120},{material.data(),16}}};
  const auto writeFull=[&](std::array<unsigned char,2048>& output) {
    std::memset(output.data(),0xA5,output.size());
    size_t at=32;
    for(const auto& source:sources) {
      std::memcpy(output.data()+at,source.data,source.count*sizeof(float));
      at+=size_t(source.count)*sizeof(float)+16; // Program-specific padding.
    }
  };
  std::array<unsigned char,2048> first{},candidate{},reference{};
  writeFull(first);
  assert(cache.commit(first.data(),sources.data(),sources.size()));
  assert(cache.ready() && cache.bytes()==2048);
  std::memset(first.data(),0xCD,first.size()); // Old reservation retired/recycled.
  position[9] = -24.f;
  material[0] = -0.f;
  std::memset(candidate.data(),0xF0,candidate.size());
  assert(cache.restore(candidate.data(),sources.data(),sources.size()));
  size_t at=32;
  unsigned changed=0,skipped=0;
  for(size_t i=0;i<sources.size();++i) {
    if(!cache.unchanged(i,sources[i])) {
      std::memcpy(candidate.data()+at,sources[i].data,sources[i].count*sizeof(float));
      ++changed;
    } else ++skipped;
    at+=size_t(sources[i].count)*sizeof(float)+16;
  }
  writeFull(reference);
  assert(candidate==reference && changed==2 && skipped==2);
  assert(cache.commit(candidate.data(),sources.data(),sources.size()));
  assert(cache.restore(first.data(),sources.data(),sources.size()));
  for(size_t i=0;i<sources.size();++i)assert(cache.unchanged(i,sources[i]));
  assert(first==reference);

  // A mis-sized span must not reuse a stale layout or poison the last valid
  // shadow. An unsuccessful draw must not commit partially uploaded bytes.
  auto malformed=sources;
  malformed[1].count=119;
  assert(!cache.restore(candidate.data(),malformed.data(),malformed.size()));
  assert(!cache.commit(candidate.data(),malformed.data(),malformed.size()));
  assert(cache.restore(candidate.data(),sources.data(),sources.size()));
  material[1]=17.5f;
  assert(!cache.unchanged(3,sources[3]));
  // Simulate a failed GXM uniform setter: do not call commit().
  assert(cache.restore(first.data(),sources.data(),sources.size()));
  assert(!cache.unchanged(3,sources[3]));

  // Bit-exact: signed zeros and NaN payloads may differ despite float equality.
  material[1]=17.f;
  material[0]=+0.f;
  assert(!cache.unchanged(3,sources[3]));
  const uint32_t nanA=0x7fc12345u,nanB=0x7fc54321u;
  std::memcpy(&position[2],&nanA,sizeof(nanA));
  assert(!cache.unchanged(1,sources[1]));
  writeFull(reference);
  assert(cache.commit(reference.data(),sources.data(),sources.size()));
  std::memcpy(&position[2],&nanB,sizeof(nanB));
  assert(!cache.unchanged(1,sources[1]));
  // Simulate many independent character draw calls, with changing poses and
  // mostly constant material/light groups, over multiple GXM buffer lifetimes.
  assert(cache.configure(2048));
  std::array<unsigned char,2048> previous{},fresh{},full{};
  for(unsigned frame=0;frame<5;++frame) {
    for(unsigned draw=0;draw<91;++draw) {
      mvp[0] = float(frame * 91 + draw);
      position[(frame + draw) % position.size()] = float(draw * 5 + frame);
      if(draw%13==0)normal[draw%normal.size()] = float(draw + frame);
      writeFull(full);
      const bool restored=cache.restore(fresh.data(),sources.data(),sources.size());
      if(!restored) std::memset(fresh.data(),0xA5,fresh.size());
      size_t start=32;
      for(size_t slot=0;slot<sources.size();++slot) {
        if(!restored||!cache.unchanged(slot,sources[slot]))
          std::memcpy(fresh.data()+start,sources[slot].data,sources[slot].count*sizeof(float));
        start+=size_t(sources[slot].count)*sizeof(float)+16;
      }
      assert(fresh==full);
      assert(cache.commit(fresh.data(),sources.data(),sources.size()));
      previous=fresh;
      std::memset(fresh.data(),0xCC,fresh.size());
    }
    // All buffers submitted in the last GXM scene may now be retired.
    assert(cache.restore(fresh.data(),sources.data(),sources.size()));
    assert(fresh==previous);
  }
  cache.invalidate();
  assert(!cache.ready() && !cache.restore(candidate.data(),sources.data(),sources.size()));
  assert(!cache.unchanged(0,sources[0]));
  assert(cache.configure(2048));
  assert(!cache.restore(candidate.data(),sources.data(),sources.size()));
  std::puts("Vita vertex uniform delta: exact old image, changed-only upload, failure fallback passed");
}
