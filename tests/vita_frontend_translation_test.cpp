#include "../lib/gx/gx.hpp"
#include "gx/aurora_gx_bridge.hpp"
#include "gx/aurora_vita_draw_sink.hpp"
#include "aurora_vita_backend.hpp"
#include "../lib/gx/fifo.hpp"
#include <array>
#include <bit>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstring>

extern "C" void aurora_vita_notify_memory_write(const void* address,size_t bytes) noexcept;

namespace {
using namespace aurora::vita;
unsigned failures=0,checks=0;
#undef CHECK
#define CHECK(x) do {++checks;if(!(x)){++failures;std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);}} while(0)
void pooled_snapshot_transitions() {
  using gfx::FixedUniformPool;
  for(size_t budget:{size_t(0),sizeof(gfx::FixedVertexUniforms),FixedUniformPool::MaxRetainedBytes}) {
    FixedUniformPool pool;pool.configure(budget);
    pool.scratch().position[0]=11;
    auto* first=&pool.publish();const auto firstRevision=first->revision;
    const auto counters=pool.stats();
    for(unsigned i=0;i<10000;++i)CHECK(&pool.publish()==first);
    CHECK(pool.size()==1);CHECK(first->position[0]==11);
    CHECK(pool.stats().allocations==counters.allocations&&pool.stats().reuses==counters.reuses&&pool.stats().fallbacks==counters.fallbacks);
    pool.scratch().position[0]=12;auto* next=&pool.publish();
    CHECK(next!=first&&next->revision>firstRevision&&first->position[0]==11);
    pool.scratch().normalPalette[3][4]=22;auto* palette=&pool.publish();CHECK(palette!=next);
    pool.scratch().texture[1][2]=33;auto* texture=&pool.publish();CHECK(texture!=palette);
    pool.scratch().light[0][0]=44;auto* light=&pool.publish();CHECK(light!=texture);
    CHECK(&pool.publish(false)!=light);
    auto* sprite=&pool.publish(true,true);CHECK(&pool.publish()!=sprite);
    const auto revision=pool.publish().revision;
    pool.reset();CHECK(pool.size()==0);CHECK(pool.publish().revision>revision);
    pool.clear();CHECK(pool.stats().retainedBytes==0);CHECK(pool.publish().revision>revision);
  }
}
void layouts() {
  auto& g=aurora::gx::g_gxState;
  std::array<uint8_t,512> array{};
  for(auto source:{GX_DIRECT,GX_INDEX8,GX_INDEX16})
    for(auto normal:{GX_NRM_XYZ,GX_NRM_NBT,GX_NRM_NBT3})
      for(auto component:{GX_S8,GX_S16,GX_F32})for(unsigned fmt=0;fmt<8;++fmt){
        g=aurora::gx::GXState{};
        g.vtxDesc[GX_VA_POS]=source;g.vtxDesc[GX_VA_NRM]=source;g.vtxDesc[GX_VA_TEX0]=source;
        g.vtxFmts[fmt].attrs[GX_VA_POS]={GX_POS_XYZ,component,3};
        g.vtxFmts[fmt].attrs[GX_VA_NRM]={normal,component,0};
        g.vtxFmts[fmt].attrs[GX_VA_TEX0]={GX_TEX_ST,component,2};
        for(auto attr:{GX_VA_POS,GX_VA_NRM,GX_VA_TEX0})g.arrays[attr]={array.data(),array.size(),32,false};
        gfx::PipelineDesc pipeline{};gfx::VertexDecodeLayout reference{};uint64_t key=0;
        gxbridge::translate_current_pipeline_and_layout(GX_TRIANGLES,fmt,pipeline,reference,key);
        const auto narrow=gxbridge::translate_current_vertex_layout(fmt);
        CHECK(!std::memcmp(&narrow,&reference,sizeof(reference)));
        // Array rebinding must affect layout while leaving TEV fields irrelevant.
        g.arrays[GX_VA_POS].stride=48;g.arrays[GX_VA_POS].le=true;
        const auto rebound=gxbridge::translate_current_vertex_layout(fmt);
        CHECK(source==GX_DIRECT||rebound.attributes[0].array.stride==48);
        CHECK(source==GX_DIRECT||rebound.attributes[0].array.littleEndian);
      }
}
void fragment_updates() {
  auto& g=aurora::gx::g_gxState;g=aurora::gx::GXState{};
  g.proj.m0={1,0,0,0};g.proj.m1={0,1,0,0};g.proj.m2={0,0,1,0};g.proj.m3={0,0,0,1};
  g.renderViewport={0,0,960,544,0,1};g.logicalViewport=g.renderViewport;
  gfx::PipelineDesc pipeline{};pipeline.fogMode=gfx::FogMode::Linear;pipeline.fogRangeEnabled=true;
  pipeline.tev.indirectStageCount=1;
  gfx::VertexDecodeLayout layout{};gfx::VertexTransformState state{};
  gfx::DrawUniforms reference{},split{};
  gxbridge::translate_vertex_state(state,reference,pipeline,layout);
  gxbridge::translate_vertex_state(state,split,pipeline,layout,false);
  gxbridge::translate_fragment_uniforms(split,pipeline);
  CHECK(!std::memcmp(&split,&reference,sizeof(reference)));
  g.colorRegs[0]={0.25f,0.5f,0.75f,1.f};g.fog.a=2.f;g.fog.color={0.1f,0.2f,0.3f,1.f};
  gxbridge::translate_vertex_state(state,reference,pipeline,layout);
  gxbridge::translate_fragment_uniforms(split,pipeline);
  CHECK(!std::memcmp(&split,&reference,sizeof(reference)));
}
void state_transitions() {
  using aurora::gx::StateDomain;
  auto& g=aurora::gx::g_gxState;
  const auto reset=[] {
    auto& state=aurora::gx::g_gxState;state=aurora::gx::GXState{};
    state.proj.m0={1,0,0,0};state.proj.m1={0,1,0,0};
    state.proj.m2={0,0,1,0};state.proj.m3={0,0,0,1};
    state.pnMtx[0].pos.m0={1,0,0,0};state.pnMtx[0].pos.m1={0,1,0,0};state.pnMtx[0].pos.m2={0,0,1,0};
    state.renderViewport={0,0,960,544,0,1};state.logicalViewport=state.renderViewport;
    state.renderScissor={0,0,960,544};state.logicalScissor=state.renderScissor;
    state.vtxDesc[GX_VA_POS]=GX_DIRECT;state.vtxFmts[0].attrs[GX_VA_POS]={GX_POS_XYZ,GX_F32,0};
  };
  reset();
  std::array<uint8_t,36> raw{};
  const std::array<float,9> positions{0,0,0,1,0,0,0,1,0};
  for(unsigned i=0;i<positions.size();++i){const auto bits=std::bit_cast<uint32_t>(positions[i]);
    for(unsigned b=0;b<4;++b)raw[i*4+b]=uint8_t(bits>>(24-8*b));}
  gfx::Renderer renderer;CHECK(renderer.initialize());
  gfx::Telemetry telemetry;telemetry.begin_frame(0);
  gxbridge::DrawSink sink;gxbridge::DrawSinkConfig config{};config.telemetry=&telemetry;
  CHECK(sink.initialize(renderer,config));renderer.begin_frame();sink.begin_frame(0);
  const auto submit=[&] {auto result=sink.submit(GX_TRIANGLES,0,raw.data(),raw.size(),3);
    CHECK(result.ok);if(result.ok)g.stateDirty=false;};
  submit();const auto first=telemetry.frame().counters;
  const auto firstUniforms=sink.stream().tail_draw()->gpu_uniforms();
  g.renderScissor={12,13,90,91};g.mark_dirty(StateDomain::Raster);submit();
  CHECK(telemetry.frame().counters.vertexTranslations==first.vertexTranslations);
  CHECK(telemetry.frame().counters.fragmentTranslations==first.fragmentTranslations);
  CHECK(telemetry.frame().counters.textureResolves==first.textureResolves);
  CHECK(sink.stream().tail_draw()->scissor.x==12);
  CHECK(!std::memcmp(&firstUniforms,&sink.stream().tail_draw()->gpu_uniforms(),sizeof(firstUniforms)));
  g.colorRegs[0]={0.2f,0.4f,0.6f,0.8f};g.mark_dirty(StateDomain::Fragment);submit();
  CHECK(telemetry.frame().counters.vertexTranslations==first.vertexTranslations);
  CHECK(telemetry.frame().counters.fragmentTranslations==first.fragmentTranslations+1);
  CHECK(telemetry.frame().counters.textureResolves==first.textureResolves);
  CHECK(sink.stream().tail_draw()->gpu_uniforms().tevreg[0][0]==0.2f);
  g.proj.m0[0]=2;g.mark_dirty(StateDomain::Vertex);submit();
  CHECK(telemetry.frame().counters.vertexTranslations==first.vertexTranslations+1);
  CHECK(sink.stream().tail_draw()->gpu_uniforms().mvp[0]==2);
  CHECK(telemetry.frame().counters.textureResolves==first.textureResolves);
  const auto beforeClear=telemetry.frame().counters;
  g.clearDepth=100;g.mark_dirty(StateDomain::Clear);submit();
  CHECK(telemetry.frame().counters.vertexTranslations==beforeClear.vertexTranslations);
  CHECK(telemetry.frame().counters.fragmentTranslations==beforeClear.fragmentTranslations);
  CHECK(telemetry.frame().counters.textureResolves==beforeClear.textureResolves);
  g.arrays[GX_VA_POS].stride=20;g.layoutStateGeneration=aurora::gx::next_gx_state_epoch();
  g.mark_dirty(StateDomain::Layout);submit();
  CHECK(telemetry.frame().counters.layoutTranslations==first.layoutTranslations+1);
  CHECK(telemetry.frame().counters.pipelineTranslations==first.pipelineTranslations);
  CHECK(telemetry.frame().counters.vertexTranslations==beforeClear.vertexTranslations);
  auto before=telemetry.frame().counters;
  g.stateDirty=true;submit(); // Legacy untracked writer retains a broad rebuild.
  CHECK(telemetry.frame().counters.vertexTranslations==before.vertexTranslations+1);
  CHECK(telemetry.frame().counters.textureResolves==before.textureResolves+1);
  before=telemetry.frame().counters;const auto oldIdentity=g.stateIdentity;reset();submit();
  CHECK(g.stateIdentity!=oldIdentity);
  CHECK(telemetry.frame().counters.textureResolves==before.textureResolves+1);
  CHECK(sink.stream().tail_draw()->gpu_uniforms().mvp[0]==1);
  before=telemetry.frame().counters;
  gfx::gxm_disable_mask()=gfx::GxmDisableStateDomains;
  g.renderScissor.x=7;g.mark_dirty(StateDomain::Raster);submit();
  CHECK(telemetry.frame().counters.vertexTranslations==before.vertexTranslations+1);
  CHECK(telemetry.frame().counters.textureResolves==before.textureResolves+1);
  gfx::gxm_disable_mask()=0;
  for(unsigned mode=0;mode<6;++mode){
    g.numTevStages=1;g.numTexGens=1;
    auto& stage=g.tevStages[0];stage.channelId=GX_COLOR0A0;
    stage.colorPass.d=(mode&1)?GX_CC_TEXC:GX_CC_RASC;
    stage.texCoordId=(mode&1)?GX_TEXCOORD0:GX_TEXCOORD_NULL;
    stage.texMapId=(mode&1)?GX_TEXMAP0:GX_TEXMAP_NULL;
    g.pipelineStateGeneration=aurora::gx::next_gx_state_epoch();g.mark_dirty();
    const auto prior=telemetry.frame().counters.pipelineTranslations;submit();
    gfx::PipelineDesc expected{};gfx::VertexDecodeLayout decode{};uint64_t key=0;
    gxbridge::translate_current_pipeline_and_layout(GX_TRIANGLES,0,expected,decode,key);
    const auto recipe=gfx::build_draw_recipe(expected);
    CHECK(sink.stream().tail_draw()->vertices.size==3*recipe.gpuStride);
    CHECK(telemetry.frame().counters.pipelineTranslations==prior+1);
  }
  sink.flush();sink.shutdown();renderer.shutdown();
}
void completed_frame_does_not_fence() {
  BackendConfig config{};config.cpu_worker_threads=0;config.wait_vblank=false;
  config.log_level=RuntimeLogLevel::Silent;
  CHECK(initialize(config));CHECK(!completed_performance_snapshot().completedFrame);CHECK(!completed_memory_snapshot().completedFrame);
  CHECK(begin_frame());end_frame();
  const auto first=completed_performance_snapshot();CHECK(first.completedFrame&&first.frameIndex==1);
  const auto firstMemory=completed_memory_snapshot();CHECK(firstMemory.completedFrame&&firstMemory.frameIndex==1);
  CHECK(firstMemory.budget.staticGeometryBytes==memory_budget().staticGeometryBytes);
  CHECK(aurora::gx::fifo::start_worker());
  struct Blocker {std::atomic<bool>* entered;std::atomic<bool>* release;};
  std::atomic<bool> entered=false,release=false,readerDone=false;
  Blocker blocker{&entered,&release};
  aurora::gx::fifo::run_async([](void* opaque){auto args=*static_cast<Blocker*>(opaque);
    args.entered->store(true,std::memory_order_release);
    while(!args.release->load(std::memory_order_acquire))std::this_thread::yield();},&blocker,sizeof blocker);
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
  while(!entered.load(std::memory_order_acquire)&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
  CHECK(entered.load());
  std::thread reader([&]{const auto value=completed_performance_snapshot();
    const auto memory=completed_memory_snapshot();
    readerDone.store(value.completedFrame&&value.frameIndex==first.frameIndex&&memory.completedFrame&&memory.frameIndex==firstMemory.frameIndex,std::memory_order_release);});
  while(!readerDone.load(std::memory_order_acquire)&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
  const bool beforeRelease=readerDone.load(std::memory_order_acquire);
  release.store(true,std::memory_order_release);reader.join();CHECK(beforeRelease);
  aurora::gx::fifo::drain_sync();CHECK(begin_frame());end_frame();aurora::gx::fifo::drain_sync();
  CHECK(completed_performance_snapshot().frameIndex==2);CHECK(completed_memory_snapshot().frameIndex==2);
  shutdown();CHECK(!completed_performance_snapshot().completedFrame);CHECK(!completed_memory_snapshot().completedFrame);
  BackendConfig invalid=config;invalid.render_width=invalid.width+1;
  CHECK(!initialize(invalid));CHECK(!completed_memory_snapshot().completedFrame);
  CHECK(initialize(config));CHECK(!completed_memory_snapshot().completedFrame);
  CHECK(begin_frame());end_frame();CHECK(completed_memory_snapshot().frameIndex==1);
  shutdown();CHECK(!completed_memory_snapshot().completedFrame);
}

void guest_memory_reuse_fences_async_gx() {
  BackendConfig config{};config.cpu_worker_threads=0;config.wait_vblank=false;
  config.log_level=RuntimeLogLevel::Silent;
  CHECK(initialize(config));
  CHECK(aurora::gx::fifo::start_worker());

  struct Blocker {std::atomic<bool>* entered;std::atomic<bool>* release;};
  std::atomic<bool> entered=false,release=false,writeReturned=false;
  Blocker blocker{&entered,&release};
  aurora::gx::fifo::run_async([](void* opaque){auto args=*static_cast<Blocker*>(opaque);
    args.entered->store(true,std::memory_order_release);
    while(!args.release->load(std::memory_order_acquire))std::this_thread::yield();},&blocker,sizeof blocker);

  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
  while(!entered.load(std::memory_order_acquire)&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
  CHECK(entered.load(std::memory_order_acquire));

  std::array<uint8_t,64> guest{};
  std::thread writer([&]{
    aurora_vita_notify_memory_write(guest.data(),guest.size());
    writeReturned.store(true,std::memory_order_release);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  CHECK(!writeReturned.load(std::memory_order_acquire));
  release.store(true,std::memory_order_release);
  writer.join();
  CHECK(writeReturned.load(std::memory_order_acquire));
  shutdown();
}

void async_end_frame_is_a_lifetime_barrier() {
  BackendConfig config{};config.cpu_worker_threads=0;config.wait_vblank=false;
  config.log_level=RuntimeLogLevel::Silent;
  CHECK(initialize(config));
  CHECK(aurora::gx::fifo::start_worker());
  CHECK(begin_frame());

  struct Blocker {std::atomic<bool>* entered;std::atomic<bool>* release;};
  std::atomic<bool> entered=false,release=false,endReturned=false;
  Blocker blocker{&entered,&release};
  aurora::gx::fifo::run_async([](void* opaque){auto args=*static_cast<Blocker*>(opaque);
    args.entered->store(true,std::memory_order_release);
    while(!args.release->load(std::memory_order_acquire))std::this_thread::yield();},&blocker,sizeof blocker);

  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
  while(!entered.load(std::memory_order_acquire)&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
  CHECK(entered.load(std::memory_order_acquire));

  std::thread frameEnd([&]{
    end_frame();
    endReturned.store(true,std::memory_order_release);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  CHECK(!endReturned.load(std::memory_order_acquire));
  release.store(true,std::memory_order_release);
  frameEnd.join();
  CHECK(endReturned.load(std::memory_order_acquire));
  CHECK(completed_memory_snapshot().completedFrame);
  shutdown();
}
}
int main(){pooled_snapshot_transitions();layouts();fragment_updates();state_transitions();completed_frame_does_not_fence();guest_memory_reuse_fences_async_gx();async_end_frame_is_a_lifetime_barrier();
  std::printf("frontend translation: %u checks, %u failures\n",checks,failures);return failures?1:0;}
