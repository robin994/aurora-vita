#include "gfx/vita_draw_adapter.hpp"
#include "gfx/vita_draw_batch.hpp"
#include "gfx/vita_published_snapshot.hpp"
#include "gfx/vita_pipeline_state_diff.hpp"
#include "gfx/vita_fixed_snapshots.hpp"
#include "gfx/vita_frame_stats.hpp"
#include "aurora_vita_backend.hpp"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <type_traits>

namespace {
using namespace aurora::vita::gfx;
unsigned checks=0,failures=0;
#define CHECK(x) do {++checks;if(!(x)){++failures;std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);}} while(0)
struct ReservedDraw {StreamedDraw draw{};uint16_t* indices=nullptr;};
ReservedDraw reserve_triangle(StreamingArena& arena,uint16_t last=2) {
  ReservedDraw out{};void* vertices=nullptr;void* indices=nullptr;
  out.draw.vertices=arena.reserve_vertices(3*16,16,&vertices);
  out.draw.indices=arena.reserve_indices(3*2,2,&indices);
  out.draw.vertexCount=3;out.draw.indexCount=3;
  out.indices=static_cast<uint16_t*>(indices);
  if(vertices)std::memset(vertices,0,48);
  if(indices){out.indices[0]=0;out.indices[1]=1;out.indices[2]=last;}
  return out;
}
void local_batching() {
  BufferPool pool;StreamingArenaConfig cfg{};cfg.vertexBytes=8192;cfg.indexBytes=8192;cfg.slots=1;
  cfg.alignment=2; // Make the U16 ranges contiguous; padded ranges must not merge.
  StreamingArena arena(pool,cfg);CHECK(arena.initialize());arena.begin_frame(0);
  CommandStream stream;Telemetry telemetry;telemetry.begin_frame(0);
  DrawUniforms uniforms{};Viewport viewport{};Scissor scissor{};
  std::array<TextureBinding,MaxTextures> textures{};
  const auto enqueue=[&](const ReservedDraw& draw,uint64_t key=123,const FixedVertexUniforms* fixed=nullptr){
    CHECK(enqueue_streamed_draw(stream,draw.draw,key,uniforms,viewport,scissor,textures,
                               nullptr,0,fixed,&arena,&telemetry));
  };
  auto a=reserve_triangle(arena);enqueue(a);
  auto b=reserve_triangle(arena);enqueue(b);
  CHECK(stream.size()==1&&stream.tail_draw()->vertexCount==6&&stream.tail_draw()->indexCount==6);
  CHECK(b.indices[0]==3&&b.indices[1]==4&&b.indices[2]==5);
  auto c=reserve_triangle(arena);enqueue(c);
  CHECK(stream.size()==1&&stream.tail_draw()->vertexCount==9);
  CHECK(c.indices[0]==6&&c.indices[2]==8);
  CHECK(telemetry.frame().counters.batchMerged==2);
  CHECK(telemetry.frame().counters.batchRebasedIndices==6);
  stream.barrier();auto barrierDraw=reserve_triangle(arena);enqueue(barrierDraw);
  CHECK(stream.size()==3&&barrierDraw.indices[0]==0);
  textures[0].uvBiasX=0.25f;auto crop=reserve_triangle(arena);enqueue(crop);
  CHECK(stream.size()==4&&crop.indices[0]==0);
  uniforms.mvp[12]=2;auto matrix=reserve_triangle(arena);enqueue(matrix);
  CHECK(stream.size()==5&&matrix.indices[0]==0);
  scissor.x=3;auto clipped=reserve_triangle(arena);enqueue(clipped);
  CHECK(stream.size()==6&&clipped.indices[0]==0);
  auto pipeline=reserve_triangle(arena);enqueue(pipeline,124);
  CHECK(stream.size()==7&&pipeline.indices[0]==0);
  FixedVertexUniforms fixedA{},fixedB{};fixedB.position[0]=3;
  auto fixed1=reserve_triangle(arena);enqueue(fixed1,124,&fixedA);
  auto fixed2=reserve_triangle(arena);enqueue(fixed2,124,&fixedB);
  CHECK(stream.size()==9&&fixed2.indices[0]==0);
  auto invalid=reserve_triangle(arena,3);enqueue(invalid,124,&fixedB);
  CHECK(stream.size()==10&&invalid.indices[0]==0&&invalid.indices[2]==3);
  CHECK(telemetry.frame().counters.batchRejectedIndices==1);
  CHECK(arena.flush());
  const auto afterFlush=reserve_triangle(arena);enqueue(afterFlush,124,&fixedB);
  // The new range is writable, but the earlier submitted command stream must
  // be reset at a real submission boundary. Rebase never changes flushed data.
  CHECK(!arena.rebase_pending_indices(a.draw.indices,3,3,3));
  CHECK(a.indices[0]==0&&a.indices[2]==2);
  CHECK(afterFlush.indices[0]==0);
  stream.reset();stream.clear(ClearCommand{});auto clearDraw=reserve_triangle(arena);enqueue(clearDraw);
  CHECK(stream.size()==2&&clearDraw.indices[0]==0);
  stream.set_render_target(4);auto targetDraw=reserve_triangle(arena);enqueue(targetDraw);
  CHECK(stream.size()==4&&targetDraw.indices[0]==0);
  stream.copy_efb(CopyEfbCommand{});auto copyDraw=reserve_triangle(arena);enqueue(copyDraw);
  CHECK(stream.size()==6&&copyDraw.indices[0]==0);
  const auto limit=reserve_triangle(arena);
  CHECK(!arena.rebase_pending_indices(limit.draw.indices,3,63997,3));
  CHECK(limit.indices[0]==0);
  CHECK(arena.rebase_pending_indices(limit.draw.indices,3,63996,3));
  CHECK(limit.indices[2]==63998);
  auto wrong=reserve_triangle(arena);auto badSlice=wrong.draw.indices;++badSlice.buffer;
  CHECK(!arena.rebase_pending_indices(badSlice,3,3,3));
  badSlice=wrong.draw.indices;badSlice.offset+=1;
  CHECK(!arena.rebase_pending_indices(badSlice,3,3,3));
  CHECK(wrong.indices[0]==0);
  StreamingArenaConfig paddedCfg=cfg;paddedCfg.alignment=16;
  StreamingArena padded(pool,paddedCfg);CHECK(padded.initialize());padded.begin_frame(0);stream.reset();
  const auto paddedA=reserve_triangle(padded),paddedB=reserve_triangle(padded);
  CHECK(enqueue_streamed_draw(stream,paddedA.draw,123,uniforms,viewport,scissor,textures,nullptr,0,nullptr,&padded));
  CHECK(enqueue_streamed_draw(stream,paddedB.draw,123,uniforms,viewport,scissor,textures,nullptr,0,nullptr,&padded));
  CHECK(stream.size()==2&&paddedB.indices[0]==0);
}
void shared_lifetimes() {
  CommandStream stream;DrawUniforms uniforms{};std::array<TextureBinding,MaxTextures> textures{};
  auto& first=stream.emplace_geometry_draw();stream.share_draw_state(first,uniforms,textures,1);
  const auto* stable=&first;
  auto& equal=stream.emplace_geometry_draw();stream.share_draw_state(equal,uniforms,textures,1);
  CHECK(equal.sharedState==stable&&stream.state_snapshot_count()==1);
  textures[0].texture=99;
  auto& texture=stream.emplace_geometry_draw();stream.share_draw_state(texture,uniforms,textures,1);
  CHECK(texture.sharedState==nullptr&&texture.texture_bindings()[0].texture==99);
  for(unsigned i=0;i<2048;++i){uniforms.mvp[0]=float(i);
    auto& draw=stream.emplace_geometry_draw();stream.share_draw_state(draw,uniforms,textures,i+2);}
  CHECK(first.sharedState==nullptr&&first.gpu_uniforms().mvp[0]==1);
  CHECK(first.texture_bindings()[0].texture==0);
  CHECK(texture.texture_bindings()[0].texture==99);
  stream.reset();auto& next=stream.emplace_draw();CHECK(next.sharedState==nullptr);
  uniforms.mvp[0]=17;stream.share_draw_state(next,uniforms,textures,0);
  auto& same=stream.emplace_geometry_draw();stream.share_draw_state(same,uniforms,textures,0);
  CHECK(same.sharedState==&next);
  uniforms.mvp[0]=18;auto& changed=stream.emplace_geometry_draw();stream.share_draw_state(changed,uniforms,textures,0);
  CHECK(changed.sharedState!=same.sharedState&&same.gpu_uniforms().mvp[0]==17);
  CommandStream copied=stream,assigned;assigned=stream;
  CommandStream packetCopy;packetCopy.draw(same);
  CommandStream moved=std::move(stream);
  CHECK(stream.size()==0&&moved.draw_packet(1).gpu_uniforms().mvp[0]==17);
  moved.reset();auto& overwrite=moved.emplace_draw();overwrite.uniforms.mvp[0]=99;
  CHECK(copied.draw_packet(1).gpu_uniforms().mvp[0]==17);
  CHECK(assigned.draw_packet(1).gpu_uniforms().mvp[0]==17);
  CHECK(packetCopy.draw_packet(0).gpu_uniforms().mvp[0]==17);
  gxm_disable_mask()=GxmDisableSharedState;
  auto& inlineA=stream.emplace_geometry_draw();stream.share_draw_state(inlineA,uniforms,textures,55);
  uniforms.mvp[0]=22;auto& inlineB=stream.emplace_geometry_draw();stream.share_draw_state(inlineB,uniforms,textures,55);
  CHECK(!inlineB.sharedState&&inlineB.gpu_uniforms().mvp[0]==22&&inlineA.gpu_uniforms().mvp[0]==18);
  gxm_disable_mask()=0;
}
void published_copies() {
  struct Snapshot {uint64_t serial=0;std::array<uint64_t,32> values{};};
  PublishedSnapshot<Snapshot> published;std::atomic<bool> done=false,torn=false;
  std::thread writer([&]{for(uint64_t serial=1;serial<50000;++serial){Snapshot next{};next.serial=serial;
    next.values.fill(serial);published.publish(next);}done.store(true,std::memory_order_release);});
  while(!done.load(std::memory_order_acquire)){
    auto value=published.read();for(auto field:value.values)if(field!=value.serial)torn=true;
  }
  writer.join();CHECK(!torn.load());CHECK(published.read().serial==49999);
}
size_t snapshotAllocations=0;
template<class T> struct CountAllocator {
  using value_type=T;
  CountAllocator()=default;
  template<class U> CountAllocator(const CountAllocator<U>&) {}
  T* allocate(size_t count){++snapshotAllocations;return std::allocator<T>{}.allocate(count);}
  void deallocate(T* p,size_t count){std::allocator<T>{}.deallocate(p,count);}
  template<class U> bool operator==(const CountAllocator<U>&)const{return true;}
};
void fixed_snapshots() {
  FixedUniformSnapshotStore<CountAllocator<FixedVertexUniforms>> store;
  auto& scratch=store.scratch();
  const auto* first=&store.publish();const auto firstValue=*first;
  const auto allocations=snapshotAllocations;
  for(unsigned i=0;i<10000;++i)CHECK(&store.publish()==first);
  CHECK(store.size()==1&&snapshotAllocations==allocations);
  // Revision is identity metadata, not an input to exact equality.
  scratch.revision=999;CHECK(&store.publish()==first);
  const auto changed=[&]{const auto* next=&store.publish();CHECK(next!=first&&next->revision!=first->revision);
    CHECK(!std::memcmp(first,&firstValue,sizeof(firstValue)));};
  scratch.position[0]=2;changed();
  scratch.positionPalette[3][5]=7;changed();
  scratch.normalPalette[1][1]=2;changed();
  scratch.light[3][0]=9;changed();
  scratch.material[0][1]=0.5f;changed();
  scratch.texture[2][6]=4;changed();
  const auto* sprite1=&store.publish(true,true);// sprite payload is always distinct
  const auto* sprite2=&store.publish(true,true);CHECK(sprite1!=sprite2);
  const auto* reference1=&store.publish(false);const auto* reference2=&store.publish(false);
  CHECK(reference1!=reference2);
  for(unsigned i=0;i<2048;++i){scratch.position[3]=float(i);store.publish();}
  CHECK(!std::memcmp(first,&firstValue,sizeof(firstValue)));
  const auto revision=store.publish().revision;
  store.clear();CHECK(store.size()==0);CHECK(store.publish().revision>revision);
}
void submission_views() {
  static_assert(!std::is_constructible_v<DrawSubmissionView,DrawPacket&&>);
  static_assert(!std::is_constructible_v<DrawSubmissionView,GpuDrawUniforms&&,std::array<TextureBinding,MaxTextures>&>);
  static_assert(!std::is_constructible_v<DrawSubmissionView,GpuDrawUniforms&,std::array<TextureBinding,MaxTextures>&&>);
  CommandStream stream;DrawUniforms cpu{};std::array<TextureBinding,MaxTextures> textures{};
  cpu.mvp[12]=7;textures[3].texture=42;
  auto& a=stream.emplace_draw();stream.share_draw_state(a,cpu,textures,21);
  a.pipelineKey=123;a.vertexCount=5;a.indexCount=6;a.absoluteVertexIndices=true;
  a.vertices={4,8,16};a.indices={5,10,12};a.viewport.x=17;a.scissor.y=18;
  FixedVertexUniforms fixed{};a.fixedVertexUniforms=&fixed;
  DrawSubmissionView inlineView(a);
  CHECK(&inlineView.uniforms==&a.uniforms&&&inlineView.textures==&a.textures);
  auto& b=stream.emplace_geometry_draw();stream.share_draw_state(b,cpu,textures,21);
  b.pipelineKey=a.pipelineKey;b.vertices=a.vertices;b.indices=a.indices;b.vertexCount=a.vertexCount;
  b.indexCount=a.indexCount;b.absoluteVertexIndices=true;b.viewport=a.viewport;b.scissor=a.scissor;b.fixedVertexUniforms=&fixed;
  DrawSubmissionView sharedView(b);
  CHECK(b.sharedState==&a&&&sharedView.uniforms==&a.uniforms&&&sharedView.textures==&a.textures);
  CHECK(sharedView.pipelineKey==123&&sharedView.indexCount==6&&sharedView.vertexCount==5);
  CHECK(sharedView.vertices.offset==8&&sharedView.indices.offset==10&&sharedView.absoluteVertexIndices);
  CHECK(sharedView.fixedVertexUniforms==&fixed&&sharedView.viewport.x==17&&sharedView.scissor.y==18);
  CHECK(!std::memcmp(&inlineView.uniforms,&sharedView.uniforms,sizeof(GpuDrawUniforms)));
  CommandStream copied=stream;CommandStream moved=std::move(stream);
  DrawSubmissionView copyView(copied.draw_packet(1));
  CHECK(&copyView.uniforms!=&sharedView.uniforms&&copyView.uniforms.mvp[12]==7);
  cpu.mvp[12]=99;textures[3].texture=88;
  CHECK(copyView.uniforms.mvp[12]==7&&copyView.textures[3].texture==42);
  // Borrowed state is consumed before producer mutation; queued state remains owned.
  GpuUniformSnapshot cache;const auto* state=&cache.get(cpu,1);
  DrawSubmissionView borrowed(*state,textures);CHECK(borrowed.uniforms.mvp[12]==99&&borrowed.textures[3].texture==88);
  cpu.mvp[12]=100;cache.get(cpu,2);CHECK(state->mvp[12]==100);
  cpu.fogParams[0]=3;cache.invalidate();CHECK(cache.get(cpu,2).fogParams[0]==3);
  cpu.fogParams[0]=4;CHECK(cache.get(cpu,0).fogParams[0]==4);
  cpu.fogParams[0]=5;CHECK(cache.get(cpu,0).fogParams[0]==5);
  CHECK(moved.draw_packet(1).gpu_uniforms().mvp[12]==7);
}
void composed_statistics() {
  FrameStats facade{},native{};facade.pipelineHits=17;facade.pipelineMisses=18;
  facade.textureHits=19;facade.textureMisses=20;facade.textureUploads=21;facade.stateChanges=22;
  native.drawCalls=3;native.triangles=4;native.nativeSceneCount=5;native.nativeEfbCopies=6;
  native.nativeDisplayQueueAddUs=7;native.cpuFrameUs=8;native.diagSceneGpuUs.fill(9);
  native.nativeFinishReasonCalls.fill(100);native.nativeFinishReasonWaitUs.fill(200);
  facade=compose_native_frame_stats(facade,native);
  CHECK(facade.pipelineHits==17&&facade.pipelineMisses==18&&facade.textureHits==19&&facade.textureMisses==20);
  CHECK(facade.textureUploads==21&&facade.stateChanges==22);
  CHECK(facade.drawCalls==3&&facade.triangles==4&&facade.nativeSceneCount==5&&facade.nativeEfbCopies==6);
  CHECK(facade.cpuFrameUs==8&&facade.nativeDisplayQueueAddUs==7&&facade.diagSceneGpuUs[3]==9);
  native.drawCalls=5;native.nativeFinishCalls=1;native.nativeFinishReasonCalls[2]++;
  facade=compose_native_frame_stats(facade,native);
  CHECK(facade.drawCalls==5&&facade.nativeFinishCalls==1&&facade.nativeFinishReasonCalls[2]==101);
  const auto calls=native.nativeFinishReasonCalls,wait=native.nativeFinishReasonWaitUs;
  reset_native_frame_stats(native,calls,wait);
  CHECK(native.nativeFinishReasonWaitUs[2]==200&&native.cpuFrameUs==0);
  facade=compose_native_frame_stats(facade,native);
  CHECK(facade.drawCalls==0&&facade.nativeFinishCalls==0&&facade.nativeFinishReasonCalls[2]==101);
  CHECK(facade.pipelineHits==17);
}
void published_memory() {
  using aurora::vita::CompletedMemorySnapshot;
  PublishedSnapshot<CompletedMemorySnapshot> snapshot;
  CHECK(!snapshot.read().completedFrame);
  std::atomic<bool> done=false,torn=false;
  std::thread writer([&]{for(uint64_t i=1;i<10000;++i){CompletedMemorySnapshot value{};
    value.completedFrame=true;value.frameIndex=i;value.budget.staticGeometryBytes=i;
    value.budget.staticGeometryHits=i;value.budget.staticGeometryEntries=i;snapshot.publish(value);}done=true;});
  while(!done){auto v=snapshot.read();if(v.completedFrame&&(v.frameIndex!=v.budget.staticGeometryBytes||
      v.frameIndex!=v.budget.staticGeometryHits||v.frameIndex!=v.budget.staticGeometryEntries))torn=true;}
  writer.join();CHECK(!torn&&snapshot.read().frameIndex==9999);
}
void pipeline_bindings() {
  int vertexA=0,vertexB=0,fragmentA=0,fragmentB=0;
  PipelineBindingState previous{&vertexA,&fragmentA,1,1,1};
  CHECK(pipeline_state_changes(previous,previous,false)==0);
  CHECK(pipeline_setter_count(pipeline_state_changes(previous,previous,true))==7);
  auto next=previous;next.fragment=&fragmentB;
  CHECK(pipeline_state_changes(previous,next,false)==BindFragment);
  CHECK(pipeline_setter_count(pipeline_state_changes(previous,next,false))==1);
  next.vertex=&vertexB;
  CHECK(pipeline_state_changes(previous,next,false)==(BindFragment|BindVertex));
  next=previous;next.depthFunction=2;
  CHECK(pipeline_setter_count(pipeline_state_changes(previous,next,false))==2);
  next.depthWrite=2;next.cull=2;
  CHECK(pipeline_setter_count(pipeline_state_changes(previous,next,false))==5);
  CHECK(pipeline_setter_count(pipeline_state_changes(previous,next,true))==7);
}
}
int main(){local_batching();shared_lifetimes();published_copies();pipeline_bindings();
  fixed_snapshots();submission_views();composed_statistics();published_memory();
  std::printf("submission: %u checks, %u failures\n",checks,failures);return failures?1:0;}
