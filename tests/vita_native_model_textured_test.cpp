#include "../lib/gx/gx.hpp"
#include "../lib/gx/fifo.hpp"
#include "../lib/dolphin/gx/__gx.h"
#include "aurora_vita_backend.hpp"
#include "gx/aurora_vita_draw_sink.hpp"
#include "gx/aurora_gx_bridge.hpp"
#include "gfx/vita_pipeline_key.hpp"
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace aurora::vita;
using namespace aurora::gx::fifo;
#undef CHECK
#define CHECK(x) do { if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::exit(1);} } while(0)

struct Snapshot {gfx::DrawPacket packet{};gfx::FixedVertexUniforms fixed{};uintptr_t fixedAddress=0;bool gpu=false;};
Snapshot snapshot(bool requireGpu=true){
  Snapshot result;
  run_sync([](void* p){auto& out=*static_cast<Snapshot*>(p);
    const auto* packet=draw_sink().stream().tail_draw();
    CHECK(packet);
    out.packet=*packet;out.packet.uniforms=packet->gpu_uniforms();
    out.packet.textures=packet->texture_bindings();out.packet.sharedState=nullptr;
    if(packet->fixedVertexUniforms){out.fixed=*packet->fixedVertexUniforms;
      out.fixedAddress=reinterpret_cast<uintptr_t>(packet->fixedVertexUniforms);out.gpu=true;}
    out.packet.fixedVertexUniforms=nullptr;
  },&result);CHECK(!requireGpu||result.gpu);return result;
}
void same(const Snapshot& native,const Snapshot& reference){
  CHECK(native.packet.pipelineKey==reference.packet.pipelineKey);
  CHECK(!std::memcmp(&native.packet.uniforms,&reference.packet.uniforms,sizeof(native.packet.uniforms)));
  CHECK(!std::memcmp(&native.fixed,&reference.fixed,offsetof(gfx::FixedVertexUniforms,revision)));
  CHECK(native.packet.vertices.buffer==reference.packet.vertices.buffer);
  CHECK(native.packet.indices.buffer==reference.packet.indices.buffer);
  CHECK(!std::memcmp(&native.packet.textures,&reference.packet.textures,sizeof(native.packet.textures)));
  CHECK(!std::memcmp(&native.packet.viewport,&reference.packet.viewport,sizeof(native.packet.viewport)));
  CHECK(!std::memcmp(&native.packet.scissor,&reference.packet.scissor,sizeof(native.packet.scissor)));
}
int main(){
  BackendConfig config{};config.cpu_worker_threads=0;config.wait_vblank=false;
  config.log_level=RuntimeLogLevel::Silent;config.static_geometry_budget=1024*1024;
  config.static_geometry_min_vertices=3;config.gxm_lit_fixed_vertex_gpu=true;
  config.diagnostics=true;CHECK(initialize(config));CHECK(start_worker());
  std::array<uint8_t,36> positions{},normals{};std::array<uint8_t,24> uv{};
  std::array<uint8_t,32> texture{},palette{};std::array<uint8_t,64> list{};
  const auto put=[](uint8_t* p,float f){const auto u=std::bit_cast<uint32_t>(f);
    for(unsigned b=0;b<4;++b)p[b]=uint8_t(u>>(24-b*8));};
  list[0]=0x90;list[2]=3;
  for(unsigned n=0;n<3;++n){
    put(positions.data()+12*n,float(n==1));put(positions.data()+12*n+4,float(n==2));
    put(normals.data()+12*n+8,1);put(uv.data()+8*n,float(n==1));put(uv.data()+8*n+4,float(n==2));
    for(unsigned i=0;i<3;++i)list[3+6*n+1+2*i]=uint8_t(n);
  }
  texture.fill(255);palette.fill(255);
  for(const auto& range:{std::pair{positions.data(),positions.size()},std::pair{normals.data(),normals.size()},
      std::pair{uv.data(),uv.size()},std::pair{list.data(),list.size()}})gfx::note_memory_write(range.first,range.second);
  struct Data {uint8_t* p;uint8_t* n;uint8_t* uv;uint8_t* texture;uint8_t* palette;} data{positions.data(),normals.data(),uv.data(),texture.data(),palette.data()};
  CHECK(begin_frame());run_sync([](void* p){const auto& d=*static_cast<Data*>(p);auto& g=aurora::gx::g_gxState;
    g=aurora::gx::GXState{};
    g.proj.m0={1,0,0,0};g.proj.m1={0,1,0,0};g.proj.m2={0,0,1,0};g.proj.m3={0,0,0,1};
    for(unsigned i=0;i<3;++i){g.pnMtx[i].pos.m0={1,0,0,float(i)};g.pnMtx[i].pos.m1={0,1,0,0};g.pnMtx[i].pos.m2={0,0,1,0};
      g.pnMtx[i].nrm.m0={1,0,0,0};g.pnMtx[i].nrm.m1={0,1,0,0};g.pnMtx[i].nrm.m2={0,0,1,0};}
    g.renderViewport={0,0,960,544,0,1};g.logicalViewport=g.renderViewport;
    g.renderScissor={7,8,951,531};g.logicalScissor=g.renderScissor;
    g.numChans=1;g.numTevStages=1;g.numTexGens=1;
    g.tevStages[0].texMapId=GX_TEXMAP0;g.tevStages[0].texCoordId=GX_TEXCOORD0;
    g.tevStages[0].channelId=GX_COLOR0A0;
    g.tevStages[0].colorPass.b=GX_CC_TEXC;g.tevStages[0].colorPass.c=GX_CC_RASC;
    g.tevStages[0].alphaPass.d=GX_CA_TEXA;
    g.colorChannelConfig[GX_COLOR0].lightingEnabled=false;
    g.colorChannelConfig[GX_COLOR0].diffFn=GX_DF_CLAMP;
    g.colorChannelState[GX_COLOR0].lightMask=1;
    g.colorChannelState[GX_COLOR0].matColor={1,1,1,1};g.lights[0].color={1,0.5f,0.25f,1};
    g.lights[0].pos={0,0,2,1};g.lights[0].distAtt={1,0,0,0};
    g.tcgs[0].src=GX_TG_TEX0;g.tcgs[0].mtx=GX_IDENTITY;
    g.vtxDesc[GX_VA_PNMTXIDX]=GX_NONE;
    g.vtxDesc[GX_VA_POS]=g.vtxDesc[GX_VA_NRM]=g.vtxDesc[GX_VA_TEX0]=GX_INDEX16;
    g.vtxFmts[0].attrs[GX_VA_POS]={GX_POS_XYZ,GX_F32,0};g.vtxFmts[0].attrs[GX_VA_NRM]={GX_NRM_XYZ,GX_F32,0};
    g.vtxFmts[0].attrs[GX_VA_TEX0]={GX_TEX_ST,GX_F32,0};
    g.arrays[GX_VA_POS]={d.p,36,12,false};g.arrays[GX_VA_NRM]={d.n,36,12,false};g.arrays[GX_VA_TEX0]={d.uv,24,8,false};
    auto& t=g.loadedTextures[0];t.data=d.texture;t.mWidth=t.mHeight=4;t.mFormat=GX_TF_RGB5A3;
    t.image0=(3u)|(3u<<10)|(uint32_t(GX_TF_RGB5A3)<<20);t.texDataVersion=1;
    g.loadedTluts[0].data=d.palette;g.loadedTluts[0].numEntries=16;g.loadedTluts[0].format=GX_TL_RGB5A3;
    g.texCopySrc={0,0,4,4};g.texCopyDstWidth=g.texCopyDstHeight=4;g.texCopyFmt=GX_TF_RGBA8;
    g.mark_dirty();
  },&data);
  // Exercise the game's real GXCallDisplayList boundary. Flush dirty producer
  // registers outside recording; they must never be replayed from an earlier
  // instance, even when the native draw skips a later GXCallDisplayList.
  __gx->dirtyState|=2;GXFlush();CHECK(__gx->dirtyState==0);
  CHECK(begin_native_model_recording(list.data(),list.size()));GXCallDisplayList(list.data(),list.size());
  auto recipe=finish_native_model_recording(nullptr);CHECK(recipe&&recipe->before.empty()&&recipe->after.empty());
  snapshot();end_frame();
  CHECK(begin_frame());const auto before=native_model_stats();
  __gx->dirtyState|=2;GXFlush();CHECK(__gx->dirtyState==0);
  CHECK(write_native_model_recipe(recipe));
  auto native=snapshot();CHECK(native_model_stats().dispatched==before.dispatched+1);
  write_stable_data(list.data(),list.size());same(native,snapshot());
  // Live pose, lighting, fragment constants and pixel-exact scissor; no captured userdata replay.
  run_sync([](void*){auto& g=aurora::gx::g_gxState;g.pnMtx[0].pos.m0[3]=19;g.pnMtx[0].nrm.m0[0]=0.5f;
    g.lights[0].pos={1,2,3,1};g.colorRegs[0]={0.1f,0.2f,0.3f,1};g.renderScissor={11,12,900,500};
    g.mark_dirty(aurora::gx::StateDomain::Vertex|aurora::gx::StateDomain::Fragment);
  },nullptr);
  CHECK(write_native_model_recipe(recipe));auto pose=snapshot();CHECK(native_model_stats().dispatched==before.dispatched+2);
  CHECK(std::memcmp(&pose.fixed,&native.fixed,offsetof(gfx::FixedVertexUniforms,revision)));
  CHECK(pose.packet.scissor.x==11&&native.packet.scissor.x==7);
  write_stable_data(list.data(),list.size());same(pose,snapshot());end_frame();
  // Exact duplicate publication already shares CPU snapshots in the legacy
  // native branch. The opt-in builder must also honor the reference mask and
  // keep all older queued values immutable across ordinary/native transitions.
  CHECK(begin_frame());
  CHECK(write_native_model_recipe(recipe));auto duplicateA=snapshot();
  CHECK(write_native_model_recipe(recipe));auto duplicateB=snapshot();
  same(duplicateA,duplicateB);CHECK(duplicateA.fixedAddress==duplicateB.fixedAddress);
  run_sync([](void*){gfx::gxm_disable_mask()=gfx::GxmDisableFixedSnapshot;},nullptr);
  CHECK(write_native_model_recipe(recipe));auto maskedA=snapshot();
  CHECK(write_native_model_recipe(recipe));auto maskedB=snapshot();same(maskedA,maskedB);
  const char* uniformBuild=std::getenv("STRIKERS_GXM_NATIVE_MODEL_UNIFORM_BUILD");
  if(uniformBuild&&std::strcmp(uniformBuild,"1")==0)CHECK(maskedA.fixedAddress!=maskedB.fixedAddress);
  run_sync([](void*){auto& g=aurora::gx::g_gxState;g.pnMtx[0].pos.m0[3]=-31;
    g.mark_dirty(aurora::gx::StateDomain::Vertex);},nullptr);
  CHECK(write_native_model_recipe(recipe));auto moved=snapshot();
  CHECK(moved.fixedAddress!=maskedB.fixedAddress);CHECK(moved.fixed.position[3]==-31);
  struct Held {uintptr_t address;gfx::FixedVertexUniforms value;} held{maskedB.fixedAddress,maskedB.fixed};
  run_sync([](void* p){const auto& h=*static_cast<Held*>(p);
    CHECK(!std::memcmp(reinterpret_cast<const void*>(h.address),&h.value,
        offsetof(gfx::FixedVertexUniforms,revision)));},&held);
  write_stable_data(list.data(),list.size());same(moved,snapshot());
  run_sync([](void*){gfx::gxm_disable_mask()=gfx::GxmDisableFixedSnapshot|gfx::GxmDisableFixedBuild;},nullptr);
  CHECK(write_native_model_recipe(recipe));auto fullBuild=snapshot();
  CHECK(write_native_model_recipe(recipe));same(fullBuild,snapshot());
  run_sync([](void*){gfx::gxm_disable_mask()=0;},nullptr);
  CHECK(write_native_model_recipe(recipe));snapshot();end_frame();
  // A source revision must resolve a fresh texture handle, never the recipe's old binding.
  CHECK(begin_frame());run_sync([](void*){auto& g=aurora::gx::g_gxState;++g.loadedTextures[0].texDataVersion;
    g.loadedTextures[0].mode0=uint32_t(GX_MIRROR);g.mark_dirty(aurora::gx::StateDomain::Textures);
  },nullptr);
  CHECK(write_native_model_recipe(recipe));auto changed=snapshot();CHECK(changed.packet.textures[0].texture!=native.packet.textures[0].texture);
  CHECK(changed.packet.textures[0].sampler.wrapS==gfx::WrapMode::Mirror);
  write_stable_data(list.data(),list.size());same(changed,snapshot());end_frame();
  // CI texture / TLUT revision changes also resolve independently of the material guard.
  CHECK(begin_frame());run_sync([](void*){auto& g=aurora::gx::g_gxState;auto& t=g.loadedTextures[0];
    t.mWidth=t.mHeight=8;t.mFormat=GX_TF_C4;t.image0=7u|(7u<<10)|(uint32_t(GX_TF_C4)<<20);
    t.texDataVersion++;g.loadedTluts[0].tlutDataVersion=1;g.mark_dirty();
  },nullptr);
  CHECK(write_native_model_recipe(recipe));auto ci=snapshot();write_stable_data(list.data(),list.size());same(ci,snapshot());end_frame();
  CHECK(begin_frame());run_sync([](void*){auto& g=aurora::gx::g_gxState;++g.loadedTluts[0].tlutDataVersion;g.mark_dirty(aurora::gx::StateDomain::Textures);},nullptr);
  CHECK(write_native_model_recipe(recipe));auto tlut=snapshot();CHECK(tlut.packet.textures[0].texture!=ci.packet.textures[0].texture);
  write_stable_data(list.data(),list.size());same(tlut,snapshot());end_frame();
  // GXCopyTex is an ordered boundary; recreating the copy must replace its current binding.
  CHECK(begin_frame());run_sync([](void*){auto& g=aurora::gx::g_gxState;
    g.copyTextures[g.loadedTextures[0].data]={};CHECK(draw_sink().copy_tex(g.loadedTextures[0].data,false));g.mark_dirty();
  },nullptr);
  CHECK(write_native_model_recipe(recipe));auto efb=snapshot();CHECK(efb.packet.textures[0].source==gfx::TextureSource::Efb);
  write_stable_data(list.data(),list.size());same(efb,snapshot());end_frame();
  CHECK(begin_frame());run_sync([](void*){auto& g=aurora::gx::g_gxState;draw_sink().evict_copy_tex(g.loadedTextures[0].data);
    CHECK(draw_sink().copy_tex(g.loadedTextures[0].data,false));g.mark_dirty();
  },nullptr);
  CHECK(write_native_model_recipe(recipe));auto newEfb=snapshot();CHECK(newEfb.packet.textures[0].texture!=efb.packet.textures[0].texture);
  write_stable_data(list.data(),list.size());same(newEfb,snapshot());end_frame();
  // Keep the first recipe hot while more than 64 other identities arrive.
  // Candidate replacement must retain it, and queued snapshots must remain
  // equal to ordinary GX. The legacy clear-all policy is still testable.
  CHECK(begin_frame());std::vector<NativeModelRecipeRef> pressure;
  for(unsigned n=0;n<72;++n){
    CHECK(begin_native_model_recording(list.data(),list.size()));
    write_stable_data(list.data(),list.size());
    auto extra=finish_native_model_recording(nullptr);CHECK(extra);pressure.push_back(extra);snapshot();
    const auto stats=native_model_stats();CHECK(write_native_model_recipe(recipe));
    auto hot=snapshot();
    if(native_model_cache_enabled())CHECK(native_model_stats().dispatched==stats.dispatched+1);
    write_stable_data(list.data(),list.size());same(hot,snapshot());
  }
  end_frame();pressure.clear();
  // Raster transition rejects the old pipeline and executes the exact-slot GX draw.
  CHECK(begin_frame());const auto raster=native_model_stats();run_sync([](void*){auto& g=aurora::gx::g_gxState;
    g.depthUpdate=!g.depthUpdate;g.pipelineStateGeneration=aurora::gx::next_gx_state_epoch();g.mark_dirty();
  },nullptr);CHECK(write_native_model_recipe(recipe));auto fallback=snapshot();CHECK(native_model_stats().fallback==raster.fallback+1);
  write_stable_data(list.data(),list.size());same(fallback,snapshot());end_frame();
  CHECK(begin_frame());const auto geometry=native_model_stats();positions[0]^=1;gfx::note_memory_write(positions.data(),positions.size());
  CHECK(write_native_model_recipe(recipe));snapshot(false);CHECK(native_model_stats().fallback==geometry.fallback+1);end_frame();
  // Host frontend uses vitaGL eligibility, so normal/indexed-PN GPU draws are
  // not executable there. Independently compare their complete native uniform
  // builder with the full GX-state translator instead of relaxing that gate.
  run_sync([](void*){auto& g=aurora::gx::g_gxState;
    g.colorChannelConfig[GX_COLOR0].lightingEnabled=true;
    g.vtxDesc[GX_VA_PNMTXIDX]=GX_DIRECT;g.pnMtx[1].pos.m0[3]=23;g.pnMtx[2].nrm.m1[1]=0.7f;
    g.mark_dirty();auto pipeline=gxbridge::translate_current_pipeline(GX_TRIANGLES,0);
    pipeline.fixedVertexOnGpu=true;pipeline.fixedVertexIndexedPn=true;
    pipeline.colorChannels[0].lightingEnabled=true;pipeline.colorChannels[0].lightMask=1;
    const auto layout=gxbridge::translate_current_vertex_layout(0);
    gfx::VertexTransformState full{},narrow{};gfx::DrawUniforms a{},b{};
    gxbridge::translate_vertex_state(full,a,pipeline,layout);
    gxbridge::translate_fixed_vertex_state(narrow,b,pipeline,true);
    gfx::FixedVertexUniforms expected{},actual{};
    gfx::fixed_vertex_uniforms_into(expected,pipeline,full);
    gfx::fixed_vertex_uniforms_into(actual,pipeline,narrow);
    CHECK(!std::memcmp(&actual,&expected,offsetof(gfx::FixedVertexUniforms,revision)));
    CHECK(b.mvp==narrow.projection);
    a.mvp=narrow.projection; // fixed shader applies the PN palette separately
    CHECK(!std::memcmp(&a,&b,sizeof(gfx::GpuDrawUniforms)));
    CHECK(b.channelAmbient[0]==a.channelAmbient[0]&&b.channelMaterial[0]==a.channelMaterial[0]);
    CHECK(b.lights[0].position==a.lights[0].position&&b.lights[0].color==a.lights[0].color);
    CHECK(b.lights[0].cosAtt==a.lights[0].cosAtt&&b.lights[0].distAtt==a.lights[0].distAtt);
    // XF light masks belong to the generated vertex program, but are absent
    // from the fragment PipelineConfig guard used by native recipes.
    aurora::gx::PipelineConfig before{},after{};
    gxbridge::current_native_model_guard(GX_TRIANGLES,0,before);
    auto live=pipeline;const auto key=gfx::pipeline_key(live);
    g.colorChannelState[GX_COLOR0].lightMask=2;
    gxbridge::current_native_model_guard(GX_TRIANGLES,0,after);
    CHECK(!std::memcmp(&before,&after,sizeof(before)));
    gxbridge::refresh_current_vertex_program_state(live);
    CHECK(live.colorChannels[0].lightMask==2&&pipeline.colorChannels[0].lightMask==1);
    CHECK(gfx::pipeline_key(live)!=key);
    // Crowd atlas selection uses GX_TEXMTX8. Live translation must carry the
    // current frame's offset through the fixed-vertex payload.
    g.tcgs[0].mtx=GX_TEXMTX8;g.texMtxs[8].m0={1,0,0,0};g.texMtxs[8].m1={0,1,0,0.375f};
    gxbridge::refresh_current_vertex_program_state(live);
    gfx::VertexTransformState crowdFull{},crowdNative{};gfx::DrawUniforms ca{},cb{};
    gxbridge::translate_vertex_state(crowdFull,ca,live,layout);
    gxbridge::translate_fixed_vertex_state(crowdNative,cb,live,true);
    gfx::fixed_vertex_uniforms_into(expected,live,crowdFull);
    gfx::fixed_vertex_uniforms_into(actual,live,crowdNative);
    CHECK(!std::memcmp(&actual,&expected,offsetof(gfx::FixedVertexUniforms,revision)));
    // Mixed native/ordinary builder history: changing palette/texgen/light
    // activity must clear every field no longer used, including after reuse.
    gfx::FixedVertexUniformBuilder incremental;
    for(unsigned step=0;step<20;++step){
      live.fixedVertexIndexedPn=(step%3)!=0;
      live.fixedVertexTexMtxMask=(step%4)==0?1:0;
      live.colorChannels[0].lightingEnabled=(step%2)==0;
      live.colorChannels[0].lightMask=uint8_t(1u<<(step%2));
      live.texgenCount=(step%5)==0?0:1;
      g.pnMtx[0].pos.m0[3]=float(step+7);g.pnMtx[1].nrm.m1[1]=float(step+11);
      g.texMtxs[8].m1[3]=float(step)*0.125f;g.lights[0].color[0]=float(step)*0.01f;
      gfx::VertexTransformState fresh{};gfx::DrawUniforms fragment{};
      gxbridge::translate_fixed_vertex_state(fresh,fragment,live,true);
      gfx::fixed_vertex_uniforms_into(expected,live,fresh);
      const auto& built=incremental.build(live,fresh);
      CHECK(!std::memcmp(&built,&expected,offsetof(gfx::FixedVertexUniforms,revision)));
    }
  },nullptr);
  recipe.reset();shutdown();CHECK(NativeModelRecipe::liveRecipes.load()==0);
  if(native_model_cache_diagnostics()){
    const auto cache=native_model_cache_snapshot();const auto stats=native_model_stats();
    CHECK(cache.events[size_t(ModelCacheEvent::ConsumerAttempt)]==stats.attempted);
    CHECK(cache.events[size_t(ModelCacheEvent::ConsumerHit)]==stats.dispatched);
    uint64_t rejected=0;
    for(size_t i=size_t(ModelCacheEvent::ConsumerDisabled);i<=size_t(ModelCacheEvent::ConsumerTextureModes);++i)rejected+=cache.events[i];
    CHECK(rejected==stats.fallback);
    CHECK(cache.peaks[size_t(ModelCachePeak::ConsumerSlots)]<=NativeModelCacheCapacity);
    std::printf("consumer metadata bytes=%llu slots=%llu live=%llu\n",
      (unsigned long long)cache.peaks[size_t(ModelCachePeak::ConsumerMetadataBytes)],
      (unsigned long long)cache.peaks[size_t(ModelCachePeak::ConsumerSlots)],
      (unsigned long long)cache.peaks[size_t(ModelCachePeak::LiveRecipes)]);
  }
  puts("native textured model: textured draw equivalence, live pose/scissor, texture revision/sampler, TLUT, EFB recreation, raster/geometry fallback; indexed-PN/normal/light uniform oracle");
}
