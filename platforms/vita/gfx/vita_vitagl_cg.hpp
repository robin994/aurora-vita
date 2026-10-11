#pragma once
#include "vita_gfx_types.hpp"
#include "../gxm/gxm_shader_gen.hpp"
#include <algorithm>

namespace aurora::vita::gfx {
// Reuse the reference TEV emitter, while leaving GL-owned fixed state to GL.
// The original PipelineDesc remains the cache key and fixed-state authority.
inline gxm::ShaderSources build_vitagl_cg(const PipelineDesc& desc) {
  auto shaderDesc=desc;
  shaderDesc.fragmentScissor=false; // Renderer::draw applies the exact GL scissor.
  shaderDesc.nativeTextureWrapMask=0xff; // Texture/EFB caches configure GL samplers.
  shaderDesc.polygonOffset=false; // GL handles polygon offset independently.
  if(shaderDesc.blendMode==BlendMode::Logic)shaderDesc.blendMode=BlendMode::None;
  return gxm::build_tev_cg(shaderDesc);
}

using CgTextureTransforms=std::array<std::array<float,4>,MaxTextures>;
inline CgTextureTransforms vitagl_cg_texture_transforms(
    const std::array<TextureBinding,MaxTextures>* bindings) noexcept {
  CgTextureTransforms result{};
  for(unsigned i=0;i<MaxTextures;++i){
    auto& t=result[i];t={1.f,1.f,0.f,0.f};
    if(!bindings)continue;
    const auto& b=(*bindings)[i];
    t={b.uvScaleX,b.uvScaleY,b.uvBiasX,b.uvBiasY};
    if(b.flipX){t[2]+=t[0];t[0]=-t[0];}
    if(b.flipY){t[3]+=t[1];t[1]=-t[1];}
  }
  return result;
}
inline std::array<float,12> vitagl_cg_fog_knots(const std::array<float,10>& knots) noexcept {
  std::array<float,12> result{};
  std::copy(knots.begin(),knots.end(),result.begin());
  return result;
}
} // namespace aurora::vita::gfx
