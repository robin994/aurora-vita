#pragma once

#include "gfx/vita_gfx_types.hpp"
#include <array>
#include <cstdint>
#include <initializer_list>

namespace aurora::vita::gxm {

enum : uint8_t {
  TevOptSharedSample = 1u, // Share only identical texture unit/TEV coordinate pairs.
  TevOptDeadChannels = 2u // Remove only register components proven unobserved.
};

struct TevOptimizationPlan {
  std::array<bool,gfx::MaxTevStages> color{};
  std::array<bool,gfx::MaxTevStages> alpha{};
  std::array<bool,gfx::MaxTevStages> samples{};
  std::array<bool,gfx::MaxTevStages> storeSample{};
  std::array<int8_t,gfx::MaxTevStages> sampleFrom{};
  uint8_t unusedColor=0,unusedAlpha=0,unusedStages=0,reusedSamples=0;
};

// Analyze the immutable GX TEV register graph before emitting native Cg. The
// two channels are independently addressed and written at each stage. Reverse
// liveness starts at the final stage's *two* destination registers, before
// alpha test/destination-alpha override: discarded alpha can still affect kill.
// No numeric operation is reassociated or approximated by this planner.
inline TevOptimizationPlan plan_tev_optimization(const gfx::PipelineDesc& d,
                                                 uint8_t mask) noexcept {
  using namespace gfx;
  TevOptimizationPlan p{};
  p.sampleFrom.fill(-1);
  const unsigned count=d.tev.stageCount;
  if(!count || count>MaxTevStages)return p;
  const bool dead=(mask&TevOptDeadChannels)!=0;
  if(!dead) {
    for(unsigned i=0;i<count;++i)p.color[i]=p.alpha[i]=true;
  } else {
    unsigned liveC=1u<<unsigned(d.tev.stages[count-1].colorOut);
    unsigned liveA=1u<<unsigned(d.tev.stages[count-1].alphaOut);
    for(unsigned i=count;i-->0;) {
      const auto& stage=d.tev.stages[i];
      const unsigned cbit=1u<<unsigned(stage.colorOut);
      const unsigned abit=1u<<unsigned(stage.alphaOut);
      p.color[i]=(liveC&cbit)!=0;
      p.alpha[i]=(liveA&abit)!=0;
      liveC&=~cbit;
      liveA&=~abit;
      if(p.color[i]) {
        for(auto arg:{stage.color.a,stage.color.b,stage.color.c,stage.color.d}) {
          const unsigned value=unsigned(arg);
          if(value<8) {
            const unsigned bit=1u<<(value/2u);
            if(value&1u)liveA|=bit;
            else liveC|=bit;
          }
        }
      }
      if(p.alpha[i]) {
        for(auto arg:{stage.alpha.a,stage.alpha.b,stage.alpha.c,stage.alpha.d})
          if(unsigned(arg)<4u)liveA|=1u<<unsigned(arg);
      }
      p.unusedColor+=!p.color[i];
      p.unusedAlpha+=!p.alpha[i];
      p.unusedStages+=!p.color[i]&&!p.alpha[i];
    }
  }

  // A fetch is needed only if an observable channel actually consumes it.
  // Never use the sampler's texture handle as identity: the unit and TEV
  // coordinate are compile-time immutable, while runtime bindings vary.
  for(unsigned i=0;i<count;++i) {
    const auto& s=d.tev.stages[i];
    const bool colorTexture=p.color[i]&&(
        tev_color_arg_uses_texture(s.color.a)||tev_color_arg_uses_texture(s.color.b)||
        tev_color_arg_uses_texture(s.color.c)||tev_color_arg_uses_texture(s.color.d));
    const bool alphaTexture=p.alpha[i]&&(
        tev_alpha_arg_uses_texture(s.alpha.a)||tev_alpha_arg_uses_texture(s.alpha.b)||
        tev_alpha_arg_uses_texture(s.alpha.c)||tev_alpha_arg_uses_texture(s.alpha.d));
    p.samples[i]=(colorTexture||alphaTexture)&&s.texture<MaxTextures;
    if(!(mask&TevOptSharedSample)||!p.samples[i])continue;
    for(unsigned j=0;j<i;++j) {
      if(p.samples[j]&&d.tev.stages[j].texture==s.texture&&
          d.tev.stages[j].texCoord==s.texCoord) {
        p.sampleFrom[i]=int8_t(j);
        p.storeSample[j]=true;
        ++p.reusedSamples;
        break;
      }
    }
  }
  return p;
}

} // namespace aurora::vita::gxm
