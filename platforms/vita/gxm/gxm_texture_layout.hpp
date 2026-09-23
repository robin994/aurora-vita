#pragma once
#include "gfx/vita_texture_decode.hpp"
#include <string>

namespace aurora::vita::gxm {
struct LinearTextureData {
  std::vector<uint8_t> pixels;
  uint32_t mipCount=1;
  std::string error;
  bool ok() const {return error.empty()&&!pixels.empty();}
};
// Native linear RGBA rows are padded to eight texels. Multi-level storage is
// currently defined for power-of-two GX images; NPOT mip chains are rejected.
LinearTextureData prepare_linear_texture(const gfx::TextureDesc& desc);
// Power-of-two images in native Y/X Morton order, with each mip level swizzled
// independently and packed consecutively for sceGxmTextureInitSwizzled.
LinearTextureData prepare_swizzled_texture(const gfx::TextureDesc& desc);
// GX CMPR converted to UBC1/DXT1. SceGxm's compressed textures use swizzled
// block order, so Morton-order the 4x4 compressed blocks (8 bytes each), not
// the individual texels. Explicit mip levels are packed consecutively.
LinearTextureData prepare_ubc1_texture(const gfx::TextureDesc& desc);
}
