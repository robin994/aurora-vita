#include "../lib/gx/fifo.hpp"
#include "../lib/gx/command_processor.hpp"
#include "../lib/gx/gx.hpp"
#include "../lib/dolphin/gx/__gx.h"
#include "aurora_vita_backend.hpp"
#include "gx/aurora_gx_bridge.hpp"
#include "gx/aurora_vita_draw_sink.hpp"
#include "gfx/vita_fixed_vertex.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

using namespace aurora::gx;
using namespace aurora::gx::fifo;
using namespace aurora::vita;
#undef CHECK
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); std::exit(1); } } while (false)

using Palette = std::array<NativePnMatrix, NativePnPaletteCapacity>;
struct Snapshot {
  std::array<PnMtx, MaxPnMtx> matrices{};
  gfx::FixedVertexUniforms fixed{};
  gfx::DrawUniforms fragment{};
  StateRevisions revisions{};
  uint32_t pipeline=0,layout=0,vertexProgram=0;
  uint64_t draws=0;
};

void reset() {
  run_sync([](void*) {
    auto& g=g_gxState;g=GXState{};
    g.proj.m0={1,0,0,0};g.proj.m1={0,1,0,0};g.proj.m2={0,0,1,0};g.proj.m3={0,0,0,1};
    g.currentPnMtx=0;
    g.numChans=1;g.numTevStages=1;
    g.tevStages[0].channelId=GX_COLOR0A0;
    g.tevStages[0].colorPass.d=GX_CC_RASC;g.tevStages[0].alphaPass.d=GX_CA_RASA;
    g.colorChannelConfig[GX_COLOR0].lightingEnabled=true;
    g.colorChannelConfig[GX_COLOR0].diffFn=GX_DF_CLAMP;
    g.colorChannelState[GX_COLOR0].lightMask=1;
    g.colorChannelState[GX_COLOR0].matColor={1,0.7f,0.4f,1};
    g.colorChannelState[GX_COLOR0].ambColor={0.2f,0.1f,0.3f,1};
    g.lights[0].color={0.5f,1,0.25f,1};g.lights[0].pos={1,2,3,1};
    g.lights[0].distAtt={1,0,0,0};
    for(unsigned i=0;i<MaxPnMtx;++i) {
      g.pnMtx[i].pos.m0={1,0,0,float(i)};g.pnMtx[i].pos.m1={0,1,0,0};g.pnMtx[i].pos.m2={0,0,1,0};
      // Normal translation words are not part of GXLoadNrmMtxImm's payload.
      g.pnMtx[i].nrm.m0={1,0,0,17};g.pnMtx[i].nrm.m1={0,1,0,18};g.pnMtx[i].nrm.m2={0,0,1,19};
    }
  },nullptr);
}

Snapshot snapshot() {
  Snapshot result;
  run_sync([](void* out) {
    auto& s=*static_cast<Snapshot*>(out);const auto& g=g_gxState;
    s.matrices=g.pnMtx;s.revisions=g.stateRevisions;
    s.pipeline=g.pipelineStateGeneration;s.layout=g.layoutStateGeneration;s.vertexProgram=g.vertexProgramStateGeneration;
    auto pipeline=gxbridge::translate_current_pipeline(GX_TRIANGLES,0);
    pipeline.fixedVertexOnGpu=true;pipeline.fixedVertexIndexedPn=true;
    gxbridge::refresh_current_vertex_program_state(pipeline);
    gfx::VertexTransformState vertex{};
    gxbridge::translate_fixed_vertex_state(vertex,s.fragment,pipeline,true);
    gfx::fixed_vertex_uniforms_into(s.fixed,pipeline,vertex);
    s.draws=draw_sink().submitted_draws();
  },&result);
  return result;
}

Palette palette(unsigned seed) {
  Palette result;
  for(unsigned i=0;i<result.size();++i) {
    auto& m=result[i];m.slot=(i*3)%result.size();
    m.position={1,0,0,float(seed+i),0,2,0,float(i),0,0,0.5f,-float(seed)};
    m.normal={1,0,0,0,0.5f,0,0,0,2};
  }
  return result;
}

void reference(const NativePnMatrix* matrices,size_t count) {
  for(size_t i=0;i<count;++i) {
    const auto& m=matrices[i];float normal[12]{};
    for(unsigned row=0;row<3;++row)std::memcpy(normal+row*4,m.normal.data()+row*3,3*sizeof(float));
    GXLoadPosMtxImm(m.position.data(),m.slot*3);
    GXLoadNrmMtxImm(normal,m.slot*3);
  }
}

void same(const Snapshot& a,const Snapshot& b) {
  CHECK(!std::memcmp(a.matrices.data(),b.matrices.data(),sizeof(a.matrices)));
  CHECK(!std::memcmp(&a.fixed,&b.fixed,offsetof(gfx::FixedVertexUniforms,revision)));
  CHECK(!std::memcmp(&a.fragment,&b.fragment,sizeof(a.fragment)));
  CHECK(a.draws==b.draws);
}

int main() {
  BackendConfig config{};config.log_level=RuntimeLogLevel::Silent;config.wait_vblank=false;
  config.cpu_worker_threads=0;config.gxm_native_pn_palette=true;config.diagnostics_enabled=true;
  CHECK(initialize(config));
  auto data=palette(7);
  CHECK(!can_write_native_pn_palette());CHECK(!write_native_pn_palette(data.data(),data.size()));
  CHECK(start_worker());CHECK(begin_frame());reset();
  const auto initial=snapshot();
  reference(data.data(),data.size());const auto expected=snapshot();
  reset();const auto before=snapshot();
  CHECK(can_write_native_pn_palette());CHECK(write_native_pn_palette(data.data(),data.size()));
  const auto native=snapshot();same(native,expected);
  CHECK(native.revisions.vertex!=before.revisions.vertex);
  CHECK(native.revisions.fragment==before.revisions.fragment&&native.revisions.textures==before.revisions.textures);
  CHECK(native.pipeline==before.pipeline&&native.layout==before.layout&&native.vertexProgram==before.vertexProgram);
  CHECK(native.draws==initial.draws);

  // Exact encoded words survive both transports, including non-finite bits.
  data[0].position[0]=std::bit_cast<float>(0x80000000u);
  data[1].position[4]=std::bit_cast<float>(0x7fc12345u);
  data[2].normal[7]=std::bit_cast<float>(0x80000000u);
  data[3].normal[2]=std::bit_cast<float>(0x7fc54321u);
  reset();reference(data.data(),data.size());const auto bitReference=snapshot();
  reset();CHECK(write_native_pn_palette(data.data(),data.size()));same(snapshot(),bitReference);

  // Both copies reject every malformed slot before changing any matrix.
  auto bad=data;bad.back().slot=NativePnPaletteCapacity;
  const auto untouched=snapshot();const auto statsBefore=native_pn_palette_stats();
  CHECK(!write_native_pn_palette(bad.data(),bad.size()));
  CHECK(!write_native_pn_palette(nullptr,1));CHECK(!write_native_pn_palette(data.data(),0));
  CHECK(!write_native_pn_palette(data.data(),data.size()+1));
  run_sync([](void* p) {
    const auto& v=*static_cast<Palette*>(p);
    CHECK(!apply_native_pn_palette(reinterpret_cast<const uint8_t*>(v.data()),sizeof(v)));
    CHECK(!apply_native_pn_palette(reinterpret_cast<const uint8_t*>(v.data()),sizeof(v)-1));
  },&bad);
  same(snapshot(),untouched);CHECK(native_pn_palette_stats().queued==statsBefore.queued);

  // Recording a display list or native model must keep ordinary GX bytes.
  std::array<uint8_t,64> list{};
  begin_display_list(list.data(),list.size());CHECK(!write_native_pn_palette(data.data(),1));
  CHECK(end_display_list()==0);
  CHECK(begin_native_model_recording(list.data(),list.size()));
  CHECK(!write_native_pn_palette(data.data(),1));CHECK(!finish_native_model_recording(nullptr));

  // Duplicate slots retain authored last-write order; ordinary XF writes on
  // both sides retain their exact position in a shared FIFO batch.
  auto a=palette(11),b=palette(33),c=palette(55);b[1].slot=b[0].slot;
  reset();reference(a.data(),2);reference(b.data(),b.size());reference(c.data()+4,1);
  const auto mixedReference=snapshot();
  reset();reference(a.data(),2);CHECK(write_native_pn_palette(b.data(),b.size()));reference(c.data()+4,1);
  same(snapshot(),mixedReference);

  // Force segment-limit drains and mixed inline/native batches. No state can
  // be reordered when a full pending batch is handed to the consumer.
  reset();
  for(unsigned n=0;n<300;++n) {auto p=palette(n);reference(p.data(),1);reference(p.data()+1,2);}
  const auto stressReference=snapshot();
  reset();
  for(unsigned n=0;n<300;++n) {auto p=palette(n);reference(p.data(),1);CHECK(write_native_pn_palette(p.data()+1,2));}
  same(snapshot(),stressReference);

  // Hold the consumer behind an earlier callback while the producer reuses
  // its storage. Only the owned batch copy may be observed afterwards.
  struct Gate {std::atomic<bool> entered{false},release{false};} gate;
  Gate* gatePointer=&gate;
  CHECK(run_async([](void* p) {
    auto& gate=**static_cast<Gate**>(p);gate.entered.store(true,std::memory_order_release);
    while(!gate.release.load(std::memory_order_acquire))std::this_thread::yield();
  },&gatePointer,sizeof(gatePointer)));
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(1);
  while(!gate.entered.load(std::memory_order_acquire))CHECK(std::chrono::steady_clock::now()<deadline);
  auto owned=palette(123);const auto original=owned;
  CHECK(write_native_pn_palette(owned.data(),owned.size()));drain();
  owned=palette(999);gate.release.store(true,std::memory_order_release);wait_idle();
  const auto copied=snapshot();reset();reference(original.data(),original.size());same(snapshot(),copied);
  const auto totals=native_pn_palette_stats();CHECK(totals.queued==totals.applied);CHECK(totals.applied==304);
  CHECK(totals.matrices==640);
  // Actual indexed-PN, lit GX draws retain their order around the native
  // palette. The palette segment itself must never manufacture another draw.
  reset();
  run_sync([](void*) {
    auto& g=g_gxState;
    g.vtxDesc[GX_VA_PNMTXIDX]=GX_DIRECT;
    g.vtxDesc[GX_VA_POS]=g.vtxDesc[GX_VA_NRM]=GX_DIRECT;
    g.vtxFmts[0].attrs[GX_VA_POS]={GX_POS_XYZ,GX_F32,0};
    g.vtxFmts[0].attrs[GX_VA_NRM]={GX_NRM_XYZ,GX_F32,0};
    g.mark_dirty(StateDomain::All);
  },nullptr);
  std::array<uint8_t,78> draw{};draw[0]=0x90;draw[2]=3;
  const auto put=[](uint8_t* p,float f) {
    const auto bits=std::bit_cast<uint32_t>(f);
    for(unsigned byte=0;byte<4;++byte)p[byte]=uint8_t(bits>>(24-byte*8));
  };
  for(unsigned vertex=0;vertex<3;++vertex) {
    auto* p=draw.data()+3+25*vertex;p[0]=uint8_t(vertex*3);
    put(p+1,float(vertex==1));put(p+5,float(vertex==2));put(p+21,1);
  }
  const auto beforeDraws=snapshot();
  write_data(draw.data(),draw.size());CHECK(write_native_pn_palette(a.data(),a.size()));
  write_data(draw.data(),draw.size());const auto afterDraws=snapshot();
  CHECK(afterDraws.draws==beforeDraws.draws+2);
  end_frame();aurora::vita::shutdown();

  // Disabled and quiet contexts: fallback is available; quiet counters stay
  // zero even when native parameters are queued and applied successfully.
  config.gxm_native_pn_palette=false;CHECK(initialize(config));CHECK(start_worker());
  CHECK(!can_write_native_pn_palette());CHECK(!write_native_pn_palette(data.data(),1));aurora::vita::shutdown();
  config.gxm_native_pn_palette=true;config.diagnostics_enabled=false;CHECK(initialize(config));CHECK(start_worker());
  CHECK(write_native_pn_palette(data.data(),1));wait_idle();
  CHECK(native_pn_palette_stats().queued==0&&native_pn_palette_stats().applied==0);aurora::vita::shutdown();
  std::puts("native PN palette: real XF equivalence, lighting/normal uniforms, exact bits, atomic rejection, mixed FIFO batches, owned lifetime, no extra draws, quiet counters");
}
