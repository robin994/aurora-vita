#pragma once
#include "gfx/vita_gfx_types.hpp"
#include <cstring>
#include <vector>

namespace aurora::vita::gxm {

// Exact inputs of the fragment uploads. MVP belongs to the vertex stage and
// uniform revisions are deliberately excluded: neither proves fragment bytes.
struct FragmentPrepareInputs {
  gfx::GpuDrawUniforms uniforms{};
  gfx::Scissor scissor{};
  std::array<uint16_t,gfx::MaxTextures> textureFlags{};
  std::array<std::array<float,4>,gfx::MaxTextures> textureTransform{};
  uint8_t usedTextureCount=0;

  bool same(const FragmentPrepareInputs& other) const noexcept {
    if(usedTextureCount>gfx::MaxTextures||usedTextureCount!=other.usedTextureCount||
       scissor.x!=other.scissor.x||scissor.y!=other.scissor.y||
       scissor.width!=other.scissor.width||scissor.height!=other.scissor.height)return false;
    const auto equal=[](const void* a,const void* b,size_t size) noexcept {
      return size==0||std::memcmp(a,b,size)==0;
    };
    const auto& a=uniforms;
    const auto& b=other.uniforms;
    return equal(a.kcolor.data(),b.kcolor.data(),sizeof(a.kcolor))&&
        equal(a.tevreg.data(),b.tevreg.data(),sizeof(a.tevreg))&&
        equal(a.fogColor.data(),b.fogColor.data(),sizeof(a.fogColor))&&
        equal(a.fogParams.data(),b.fogParams.data(),sizeof(a.fogParams))&&
        equal(a.fogRangeK.data(),b.fogRangeK.data(),sizeof(a.fogRangeK))&&
        equal(&a.renderViewportWidth,&b.renderViewportWidth,sizeof(a.renderViewportWidth))&&
        equal(a.indirectMatrices.data(),b.indirectMatrices.data(),sizeof(a.indirectMatrices))&&
        equal(a.texcoordScale.data(),b.texcoordScale.data(),usedTextureCount*sizeof(a.texcoordScale[0]))&&
        equal(a.textureSizeBias.data(),b.textureSizeBias.data(),usedTextureCount*sizeof(a.textureSizeBias[0]))&&
        equal(textureFlags.data(),other.textureFlags.data(),usedTextureCount*sizeof(textureFlags[0]))&&
        equal(textureTransform.data(),other.textureTransform.data(),usedTextureCount*sizeof(textureTransform[0]));
  }
};

// One owned preparation per compiled fragment program, at most 4 KiB. The
// payload is copied after ALL sceGxmSetUniformDataF calls succeed. A hit copies
// into a freshly reserved default buffer; no GXM reservation/pointer is retained
// or reused across draws, scenes or program changes. Different programs own
// separate caches even when they happen to use the same GX input values.
class FragmentPrepareCache {
public:
  static constexpr size_t MaxBytes=4096;

  void configure(size_t bytes) {
    valid_=false;
    payload_.resize(bytes&&bytes<=MaxBytes?bytes:0);
  }
  size_t bytes() const noexcept { return payload_.size(); }

  bool copy_to(const FragmentPrepareInputs& inputs,void* freshBuffer) const noexcept {
    if(!freshBuffer||!valid_||!inputs_.same(inputs))return false;
    std::memcpy(freshBuffer,payload_.data(),payload_.size());
    return true;
  }

  bool store(const FragmentPrepareInputs& inputs,const void* completedBuffer) noexcept {
    if(!completedBuffer||payload_.empty()||inputs.usedTextureCount>gfx::MaxTextures)return false;
    inputs_=inputs;
    std::memcpy(payload_.data(),completedBuffer,payload_.size());
    valid_=true;
    return true;
  }

private:
  FragmentPrepareInputs inputs_{};
  std::vector<uint8_t> payload_{};
  bool valid_=false;
};
} // namespace aurora::vita::gxm
