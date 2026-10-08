#include "../platforms/vita/gxm/gxm_tev_opt.hpp"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <random>

using namespace aurora::vita;
using namespace gfx;
using namespace gxm;

static uint64_t combine(uint64_t acc,uint64_t input) {
  return (acc^(input+UINT64_C(0x9e3779b97f4a7c15)+(acc<<6)+(acc>>2)))*
      UINT64_C(0xd6e8feb86659fd93);
}

struct Registers {
  std::array<uint64_t,4> c{},a{};
};

// Independent symbolic TEV interpreter: each output depends on all four inputs
// and is assigned simultaneously, just like separate GX RGB/alpha destinations.
// By using opaque 64-bit values, a removed register dependency is detected
// without relying on compiler Cg syntax or floating-point approximations.
static void stage(Registers& r,const TevStage& s,unsigned stageIndex,
                  uint64_t nonce,bool computeColor,bool computeAlpha) {
  const auto before=r;
  const auto value=[&](unsigned arg,bool isAlpha) {
    if(isAlpha&&arg<4)return before.a[arg];
    if(!isAlpha&&arg<8)return arg&1u?before.a[arg/2u]:before.c[arg/2u];
    return combine(combine(nonce,stageIndex),arg|(isAlpha?0x10000u:0u));
  };
  if(computeColor) {
    uint64_t x=combine(nonce,0xc000u+stageIndex);
    for(auto arg:{s.color.a,s.color.b,s.color.c,s.color.d})
      x=combine(x,value(unsigned(arg),false));
    r.c[unsigned(s.colorOut)]=x;
  }
  if(computeAlpha) {
    uint64_t x=combine(nonce,0xa000u+stageIndex);
    for(auto arg:{s.alpha.a,s.alpha.b,s.alpha.c,s.alpha.d})
      x=combine(x,value(unsigned(arg),true));
    r.a[unsigned(s.alphaOut)]=x;
  }
}

static void dependency_oracle() {
  std::mt19937_64 rng(0x5c4e554157345445ULL);
  for(unsigned trial=0;trial<16000;++trial) {
    PipelineDesc p{};
    p.tev.stageCount=uint8_t(1u+rng()%16u);
    for(unsigned i=0;i<p.tev.stageCount;++i) {
      auto& s=p.tev.stages[i];
      s.color={TevColorArg(rng()%16),TevColorArg(rng()%16),
               TevColorArg(rng()%16),TevColorArg(rng()%16)};
      s.alpha={TevAlphaArg(rng()%8),TevAlphaArg(rng()%8),
               TevAlphaArg(rng()%8),TevAlphaArg(rng()%8)};
      s.colorOut=TevReg(rng()%4);
      s.alphaOut=TevReg(rng()%4);
      s.texCoord=uint8_t(rng()%9==8?0xff:rng()%8);
      s.texture=uint8_t(rng()%9==8?0xff:rng()%8);
    }
    const auto plan=plan_tev_optimization(p,TevOptDeadChannels|TevOptSharedSample);
    const auto none=plan_tev_optimization(p,0);
    for(unsigned i=0;i<p.tev.stageCount;++i) {
      assert(none.color[i]&&none.alpha[i]);
      if(plan.sampleFrom[i]>=0) {
        const unsigned j=unsigned(plan.sampleFrom[i]);
        assert(j<i && plan.storeSample[j] && plan.samples[i] && plan.samples[j]);
        assert(p.tev.stages[i].texCoord==p.tev.stages[j].texCoord);
        assert(p.tev.stages[i].texture==p.tev.stages[j].texture);
      }
    }
    for(unsigned sample=0;sample<3;++sample) {
      Registers original{},optimized{};
      for(unsigned r=0;r<4;++r) {
        original.c[r]=rng();original.a[r]=rng();
      }
      optimized=original;
      const uint64_t nonce=rng();
      for(unsigned i=0;i<p.tev.stageCount;++i) {
        stage(original,p.tev.stages[i],i,nonce,true,true);
        stage(optimized,p.tev.stages[i],i,nonce,plan.color[i],plan.alpha[i]);
      }
      const auto& last=p.tev.stages[p.tev.stageCount-1];
      assert(original.c[unsigned(last.colorOut)]==optimized.c[unsigned(last.colorOut)]);
      assert(original.a[unsigned(last.alphaOut)]==optimized.a[unsigned(last.alphaOut)]);
    }
  }
  std::puts("A4 TEV liveness: 48,000 randomized complete-vs-pruned symbolic evaluations passed");
}

static void targeted_dependencies() {
  PipelineDesc p{};p.tev.stageCount=4;
  // First stage writes Reg0 RGB and Reg1 alpha. Its Reg1 alpha is used by
  // stage 1 RGB (PrevA/Reg1A); Reg0 RGB is overwritten without being read.
  auto& first=p.tev.stages[0];
  first.colorOut=TevReg::Reg0;first.alphaOut=TevReg::Reg1;
  first.color.d=TevColorArg::TexColor;first.alpha.d=TevAlphaArg::TexAlpha;
  auto& second=p.tev.stages[1];
  second.colorOut=TevReg::Prev;
  second.color.d=TevColorArg::Reg1A;
  second.alphaOut=TevReg::Reg2;
  auto& third=p.tev.stages[2];
  third.colorOut=TevReg::Reg0;third.alphaOut=TevReg::Reg1;
  third.color.d=TevColorArg::Prev;
  third.alpha.d=TevAlphaArg::Zero;
  auto& last=p.tev.stages[3];
  last.colorOut=TevReg::Prev;last.alphaOut=TevReg::Prev;
  last.color.d=TevColorArg::Reg0;last.alpha.d=TevAlphaArg::Reg1A;
  const auto plan=plan_tev_optimization(p,TevOptDeadChannels);
  assert(!plan.color[0]&&plan.alpha[0]);
  assert(plan.color[1]&&!plan.alpha[1]);
  assert(plan.color[2]&&plan.alpha[2]);
  assert(plan.color[3]&&plan.alpha[3]);

  // Same sampler unit with different TEV coords must NEVER share a fetch.
  for(unsigned i=0;i<4;++i) {
    auto& s=p.tev.stages[i];s.texture=1;s.texCoord=uint8_t(i);
    s.color.d=TevColorArg::TexColor;
  }
  auto noReuse=plan_tev_optimization(p,TevOptSharedSample);
  assert(noReuse.reusedSamples==0);
  p.tev.stages[3].texCoord=p.tev.stages[1].texCoord;
  auto reuse=plan_tev_optimization(p,TevOptSharedSample);
  assert(reuse.reusedSamples==1 && reuse.sampleFrom[3]==1 && reuse.storeSample[1]);
}

int main(){dependency_oracle();targeted_dependencies();std::puts("A4 planner: targeted cross-channel, index and fetch guards passed");}
