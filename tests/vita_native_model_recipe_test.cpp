#include "../lib/gx/gx.hpp"
#include "../lib/gx/fifo.hpp"
#include "aurora_vita_backend.hpp"
#include "gx/aurora_vita_draw_sink.hpp"
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace aurora::vita;
using namespace aurora::gx::fifo;
#undef CHECK
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)

struct Snapshot {gfx::DrawPacket packet{};gfx::FixedVertexUniforms fixed{};bool gpu=false;};
Snapshot snapshot(){
  Snapshot out;
  run_sync([](void* context){auto& out=*static_cast<Snapshot*>(context);
    const auto* packet=draw_sink().stream().tail_draw();CHECK(packet);
    out.packet=*packet;out.packet.uniforms=packet->gpu_uniforms();out.packet.sharedState=nullptr;
    if(packet->fixedVertexUniforms){out.fixed=*packet->fixedVertexUniforms;out.gpu=true;}
    out.packet.fixedVertexUniforms=nullptr;
  },&out);
  return out;
}

int main(){
  CHECK(native_model_transport_rejections()&model_reject_bit(ModelReject::WorkerInactive));
  BackendConfig config{};config.cpu_worker_threads=0;config.wait_vblank=false;
  config.log_level=RuntimeLogLevel::Silent;config.static_geometry_budget=1024*1024;
  config.static_geometry_min_vertices=3;config.diagnostics=true;
  CHECK(initialize(config));CHECK(start_worker());CHECK(begin_frame());
  CHECK(native_model_transport_rejections()==0);
  run_sync([](void*){CHECK(native_model_transport_rejections()&model_reject_bit(ModelReject::WorkerContext));},nullptr);
  run_sync([](void*){
    auto& g=aurora::gx::g_gxState;g=aurora::gx::GXState{};
    g.proj.m0={1,0,0,0};g.proj.m1={0,1,0,0};g.proj.m2={0,0,1,0};g.proj.m3={0,0,0,1};
    g.renderViewport={0,0,960,544,0,1};g.logicalViewport=g.renderViewport;
    g.renderScissor={1,2,959,542};g.logicalScissor=g.renderScissor;
    g.numChans=1;g.numTevStages=1;g.tevStages[0].channelId=GX_COLOR0A0;
    g.tevStages[0].colorPass.d=GX_CC_RASC;g.tevStages[0].alphaPass.d=GX_CA_RASA;
    g.colorChannelConfig[GX_COLOR0].matSrc=GX_SRC_VTX;
    g.colorChannelConfig[GX_ALPHA0].matSrc=GX_SRC_VTX;
    g.vtxDesc[GX_VA_POS]=GX_DIRECT;g.vtxFmts[0].attrs[GX_VA_POS]={GX_POS_XYZ,GX_F32,0};
    g.vtxDesc[GX_VA_CLR0]=GX_DIRECT;g.vtxFmts[0].attrs[GX_VA_CLR0]={GX_CLR_RGBA,GX_RGBA8,0};
    g.mark_dirty();
  },nullptr);
  std::array<uint8_t,64> list{};list[0]=0x90;list[2]=3;
  for(unsigned n=0;n<3;++n){
    const std::array<float,3> p{float(n==1),float(n==2),0};
    for(unsigned j=0;j<3;++j){const auto bits=std::bit_cast<uint32_t>(p[j]);
      for(unsigned b=0;b<4;++b)list[3+n*16+j*4+b]=uint8_t(bits>>(24-b*8));}
    list[3+n*16+12]=32;list[3+n*16+13]=64;list[3+n*16+14]=128;list[3+n*16+15]=255;
  }
  gfx::note_memory_write(list.data(),list.size());
  std::array<float,12> position{1,0,0,2,0,1,0,3,0,0,1,4};
  CHECK(begin_native_model_recording(list.data(),list.size()));
  write_bp(0x400017);native_model_record_draw_boundary();
  write_u8(0);write_stable_data(list.data(),list.size());write_u8(0);
  auto recipe=finish_native_model_recording(position.data());CHECK(recipe);
  CHECK(recipe->materialBytes==5&&recipe->before.size()==6&&recipe->after.size()==1);
  auto reference=snapshot();CHECK(reference.gpu);
  auto counts=native_model_stats();CHECK(counts.fallback==1&&counts.dispatched==0);
  end_frame();

  // Cross-frame admission, new camera, new instance pose and exact scissor.
  CHECK(begin_frame());
  run_sync([](void*){auto& g=aurora::gx::g_gxState;g.proj.m0[0]=2;
    g.renderScissor={7,8,951,531};g.mark_dirty(aurora::gx::StateDomain::Vertex);
  },nullptr);
  position[3]=9;
  CHECK(write_native_model_recipe(recipe,position.data()));
  const auto first=snapshot();CHECK(first.gpu&&first.fixed.position[3]==9);
  CHECK(first.packet.uniforms.mvp[0]==2&&first.packet.scissor.x==7&&first.packet.scissor.y==8);
  CHECK(first.packet.vertices.buffer==reference.packet.vertices.buffer);
  CHECK(native_model_stats().dispatched==1);
  // Independent live snapshots must survive a later pose in the same batch.
  position[3]=12;CHECK(write_native_model_recipe(recipe,position.data()));
  const auto second=snapshot();CHECK(second.fixed.position[3]==12&&first.fixed.position[3]==9);
  CHECK(native_model_stats().dispatched==2);
  // Independent reference: execute the original GX draw at the same live
  // state, rather than deriving the expected packet from the native recipe.
  write_stable_data(list.data(),list.size());
  const auto oracle=snapshot();CHECK(oracle.gpu);
  CHECK(oracle.packet.pipelineKey==second.packet.pipelineKey);
  CHECK(!std::memcmp(&oracle.packet.uniforms,&second.packet.uniforms,sizeof(oracle.packet.uniforms)));
  CHECK(!std::memcmp(&oracle.fixed,&second.fixed,offsetof(gfx::FixedVertexUniforms,revision)));
  CHECK(oracle.packet.vertices.buffer==second.packet.vertices.buffer&&
        oracle.packet.indices.buffer==second.packet.indices.buffer);
  end_frame();

  // A raster transition must take GX fallback and create a new material guard.
  CHECK(begin_frame());
  run_sync([](void*){auto& g=aurora::gx::g_gxState;g.depthUpdate=!g.depthUpdate;
    g.pipelineStateGeneration=aurora::gx::next_gx_state_epoch();g.mark_dirty();},nullptr);
  CHECK(write_native_model_recipe(recipe,position.data()));snapshot();
  CHECK(native_model_stats().fallback==2);
  end_frame();

  // A real source write replaces its pin and rejects stale resident geometry.
  CHECK(begin_frame());list[15]=1;gfx::note_memory_write(list.data(),list.size());
  CHECK(write_native_model_recipe(recipe,position.data()));
  const auto changed=snapshot();CHECK(!changed.gpu);
  CHECK(native_model_stats().fallback==3);
  end_frame();

  // Overflow aborts recording, replays accumulated bytes, then continues the
  // original FIFO writer. No partial GX instruction or draw is discarded.
  CHECK(begin_frame());const auto before=native_model_stats();
  CHECK(begin_native_model_recording(list.data(),list.size()));
  std::array<uint8_t,NativeModelRecipe::MaxStateBytes+1> nops{};
  write_data(nops.data(),nops.size());
  CHECK(!detail::sNativeModelRecording);
  write_stable_data(list.data(),list.size());
  CHECK(!finish_native_model_recording(position.data()));snapshot();
  CHECK(native_model_stats().attempted==before.attempted);
  end_frame();
  // A compound source must execute both original draws on every attempt.
  CHECK(begin_frame());
  std::array<uint8_t,128> compound{};
  std::copy(list.begin(),list.end(),compound.begin());
  std::copy(list.begin(),list.end(),compound.begin()+64);
  gfx::note_memory_write(compound.data(),compound.size());
  CHECK(begin_native_model_recording(compound.data(),compound.size()));
  write_stable_data(compound.data(),compound.size());
  auto multi=finish_native_model_recording(position.data());CHECK(multi);
  auto count_draws=[](){size_t count=0;run_sync([](void* p){for(const auto& c:draw_sink().stream().commands())
      if(c.type==gfx::CommandType::Draw)++*static_cast<size_t*>(p);},&count);return count;};
  CHECK(count_draws()==2);
  CHECK(write_native_model_recipe(multi,position.data()));
  CHECK(count_draws()==4);
  CHECK(native_model_stats().dispatched==2);
  end_frame();multi.reset();

  // Game-like independently indexed position/color streams. Array rebinding
  // and writes must reject resident geometry even when the DL is unchanged.
  std::array<uint8_t,36> positions{},rebound{};
  std::array<uint8_t,12> colors{};
  std::array<uint8_t,64> indexed{};indexed[0]=0x90;indexed[2]=3;
  for(unsigned n=0;n<3;++n){
    std::copy_n(list.data()+3+n*16,12,positions.data()+n*12);
    std::copy_n(list.data()+3+n*16+12,4,colors.data()+n*4);
    indexed[3+n*4+1]=uint8_t(n);indexed[3+n*4+3]=uint8_t(n);
  }
  rebound=positions;
  gfx::note_memory_write(positions.data(),positions.size());
  gfx::note_memory_write(rebound.data(),rebound.size());
  gfx::note_memory_write(colors.data(),colors.size());
  gfx::note_memory_write(indexed.data(),indexed.size());
  CHECK(begin_frame());
  struct Arrays {uint8_t* positions;uint8_t* colors;} arrays{positions.data(),colors.data()};
  run_sync([](void* context){const auto& a=*static_cast<Arrays*>(context);
    auto& g=aurora::gx::g_gxState;
    g.vtxDesc[GX_VA_POS]=GX_INDEX16;g.vtxDesc[GX_VA_CLR0]=GX_INDEX16;
    g.arrays[GX_VA_POS]={a.positions,36,12,false};g.arrays[GX_VA_CLR0]={a.colors,12,4,false};
    g.layoutStateGeneration=aurora::gx::next_gx_state_epoch();g.mark_dirty();
  },&arrays);
  CHECK(begin_native_model_recording(indexed.data(),indexed.size()));
  write_stable_data(indexed.data(),indexed.size());
  auto indexedRecipe=finish_native_model_recording(nullptr);CHECK(indexedRecipe);
  CHECK(snapshot().gpu);end_frame();
  CHECK(begin_frame());const auto indexedCounts=native_model_stats();
  CHECK(write_native_model_recipe(indexedRecipe));
  const auto indexedNative=snapshot();CHECK(indexedNative.gpu);
  CHECK(native_model_stats().dispatched==indexedCounts.dispatched+1);
  write_stable_data(indexed.data(),indexed.size());
  const auto indexedOracle=snapshot();CHECK(indexedOracle.gpu);
  CHECK(indexedOracle.packet.pipelineKey==indexedNative.packet.pipelineKey);
  CHECK(!std::memcmp(&indexedOracle.packet.uniforms,&indexedNative.packet.uniforms,sizeof(indexedNative.packet.uniforms)));
  CHECK(!std::memcmp(&indexedOracle.fixed,&indexedNative.fixed,offsetof(gfx::FixedVertexUniforms,revision)));
  CHECK(indexedOracle.packet.vertices.buffer==indexedNative.packet.vertices.buffer&&
        indexedOracle.packet.indices.buffer==indexedNative.packet.indices.buffer);
  end_frame();
  CHECK(begin_frame());
  run_sync([](void* context){auto& g=aurora::gx::g_gxState;
    g.arrays[GX_VA_POS].data=static_cast<uint8_t*>(context);
    g.layoutStateGeneration=aurora::gx::next_gx_state_epoch();g.mark_dirty();
  },rebound.data());
  CHECK(write_native_model_recipe(indexedRecipe));CHECK(snapshot().gpu);
  CHECK(native_model_stats().fallback==indexedCounts.fallback+1);
  CHECK(write_native_model_recipe(indexedRecipe));CHECK(snapshot().gpu);
  CHECK(native_model_stats().dispatched==indexedCounts.dispatched+2);
  end_frame();
  CHECK(begin_frame());rebound[0]^=1;gfx::note_memory_write(rebound.data(),rebound.size());
  CHECK(write_native_model_recipe(indexedRecipe));snapshot();
  CHECK(native_model_stats().fallback==indexedCounts.fallback+2);
  end_frame();indexedRecipe.reset();

  // A bounded number of live recipes includes handles retained by queued jobs.
  std::vector<NativeModelRecipeRef> retained;
  CHECK(begin_frame());
  run_sync([](void*){auto& g=aurora::gx::g_gxState;
    g.vtxDesc[GX_VA_POS]=GX_DIRECT;g.vtxDesc[GX_VA_CLR0]=GX_DIRECT;
    g.layoutStateGeneration=aurora::gx::next_gx_state_epoch();g.mark_dirty();
  },nullptr);
  while(NativeModelRecipe::liveRecipes.load()<NativeModelRecipe::max_live_recipes()){
    CHECK(begin_native_model_recording(list.data(),list.size()));
    write_stable_data(list.data(),list.size());
    auto next=finish_native_model_recording(position.data());CHECK(next);
    retained.push_back(std::move(next));
  }
  CHECK(!begin_native_model_recording(list.data(),list.size()));
  CHECK(native_model_transport_rejections()&model_reject_bit(ModelReject::RecipeLimit));
  end_frame();retained.clear();
  CHECK(NativeModelRecipe::liveRecipes.load()==1);
  shutdown();
  CHECK(!write_native_model_recipe(recipe,position.data()));
  puts("native model: cross-frame draw, live pose/camera/scissor, raster/source fallback, bounded capture and shutdown");
}
