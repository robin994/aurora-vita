#include "gfx/vita_fixed_vertex.hpp"
#include "gfx/vita_fixed_uniform_pool.hpp"
#include <cstdio>
#include <cstring>
#include <vector>

using namespace aurora::vita::gfx;
static unsigned checks=0;
#define CHECK(x) do { ++checks; if(!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1; } } while(0)

template<class Array> static void fill(Array& a,unsigned seed) {
  for(unsigned i=0;i<a.size();++i)a[i]=float(int((i*41+seed*19)%257)-128)*0.25f;
}
static void fill_state(VertexTransformState& state,unsigned seed) {
  for(unsigned i=0;i<20;++i) { fill(state.postexMatrices[i].v,seed+i);fill(state.postMatrices[i].v,seed+i+31); }
  for(unsigned i=0;i<10;++i)fill(state.normalMatrices[i].v,seed+i+67);
  for(unsigned i=0;i<4;++i) { fill(state.channelAmbient[i],seed+i+89);fill(state.channelMaterial[i],seed+i+107); }
  for(unsigned i=0;i<MaxLights;++i) {
    auto& l=state.lights[i];fill(l.position,seed+i);fill(l.direction,seed+i+2);
    fill(l.color,seed+i+4);fill(l.cosAtt,seed+i+6);fill(l.distAtt,seed+i+8);
  }
  state.currentPnMatrix=uint8_t(seed%23); // Include out-of-range PN selection.
  state.channelMaterial[0][0]=-0.f;
  const uint32_t nanBits=0x7fc01234u;
  std::memcpy(&state.postexMatrices[0].v[0],&nanBits,sizeof(nanBits));
}
int main() {
  gxm_disable_mask()=GxmDisableFixedSnapshot;
  CHECK(gxm_disabled(GxmDisableFixedSnapshot)&&!gxm_disabled(GxmDisableFixedBuild));
  gxm_disable_mask()|=GxmDisableFixedBuild;
  CHECK(gxm_disabled(GxmDisableFixedSnapshot)&&gxm_disabled(GxmDisableFixedBuild));
  gxm_disable_mask()=0;
  FixedVertexUniformBuilder builder;
  VertexTransformState state{};
  PipelineDesc pipeline{};
  for(unsigned step=0;step<4096;++step) {
    fill_state(state,step);
    pipeline=PipelineDesc{};
    pipeline.fixedVertexIndexedPn=step&1;
    pipeline.fixedVertexTexMtxMask=step%3?uint8_t(step):0;
    pipeline.texgenCount=uint8_t(step%10); // Clamp count to the public eight slots.
    pipeline.tev.stageCount=8;
    for(unsigned i=0;i<8;++i) {
      auto& stage=pipeline.tev.stages[i];stage.texCoord=i;stage.texture=i;
      stage.color.a=(step&(1u<<i))?TevColorArg::TexColor:TevColorArg::Zero;
      auto& t=pipeline.texgens[i];t.matrix=int8_t((step+i)%14-2);
      t.postMatrix=int8_t((step+i)%24-2);
      t.type=static_cast<TexGenType>((step+i)%11);t.embossSource=uint8_t(i?i-1:0);
    }
    for(unsigned i=0;i<4;++i) {
      pipeline.colorChannels[i].lightingEnabled=(step+i)%3!=0;
      pipeline.colorChannels[i].lightMask=uint8_t(step*37+i*19);
    }
    FixedVertexUniforms reference{};
    fixed_vertex_uniforms_into(reference,pipeline,state);
    CHECK(std::memcmp(&reference,&builder.build(pipeline,state),sizeof(reference))==0);
    // Inputs change without revision tracking: active data must still be read.
    state.postexMatrices[0].v[1]+=19.f;state.lights[0].color[0]+=7.f;
    fixed_vertex_uniforms_into(reference,pipeline,state);
    CHECK(std::memcmp(&reference,&builder.build(pipeline,state),sizeof(reference))==0);
    pipeline=PipelineDesc{};fixed_vertex_uniforms_into(reference,pipeline,state);
    CHECK(std::memcmp(&reference,&builder.build(pipeline,state),sizeof(reference))==0);
  }
  for(size_t budget:{size_t(0),sizeof(FixedVertexUniforms)*2}) {
    FixedUniformPool pool;pool.configure(budget);
    FixedVertexUniforms candidate{};candidate.position[0]=17;
    auto& first=pool.publish_from(candidate);const auto saved=first;
    CHECK(&first==&pool.publish_from(candidate)&&pool.size()==1);
    candidate.position[0]=42;
    auto& second=pool.publish_from(candidate);
    CHECK(&second!=&first&&second.revision!=first.revision);
    CHECK(std::memcmp(&first,&saved,sizeof(saved))==0);
    std::vector<const FixedVertexUniforms*> live{&first,&second};
    std::vector<FixedVertexUniforms> expected{first,second};
    for(unsigned i=0;i<16;++i) {
      candidate.position[0]=float(i);
      auto& next=pool.publish_from(candidate,false);
      live.push_back(&next);expected.push_back(next);
    }
    for(unsigned i=0;i<live.size();++i)CHECK(std::memcmp(live[i],&expected[i],sizeof(candidate))==0);
    // A sprite can be mutated after publish; distinct candidates cannot alias it.
    auto& sprite=pool.publish_from(candidate,true,true);sprite.primitiveExpand[0]=123;
    auto& normal=pool.publish_from(candidate);
    CHECK(&sprite!=&normal&&normal.primitiveExpand[0]==960);
    const auto revision=normal.revision;pool.reset();
    candidate=FixedVertexUniforms{};
    auto& warm=pool.publish_from(candidate);
    CHECK(warm.revision>revision);
    CHECK(std::memcmp(&warm,&candidate,offsetof(FixedVertexUniforms,revision))==0);
    pool.reset();auto& empty=pool.emplace_back();const FixedVertexUniforms defaults{};
    CHECK(std::memcmp(&empty,&defaults,sizeof(defaults))==0);
    pool.reset();pool.scratch()=candidate;
    auto& oldApi=pool.publish();CHECK(pool.scratch().revision==oldApi.revision);
  }
  std::printf("PASS fixed uniform builder/pool: %u exact payload and lifetime checks\n",checks);
  return 0;
}
