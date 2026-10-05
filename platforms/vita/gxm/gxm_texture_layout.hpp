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
struct CompactTextureData : LinearTextureData {
  gfx::NativeTextureFormat format=gfx::NativeTextureFormat::None;
  uint32_t stride=0;
  bool swizzled=false;
};
// Preserve explicit GX mips in compact I8/IA8/RGB565 storage. Unsupported
// formats/generated levels use the existing RGBA8 path. BC1 is single-level,
// aligned and accepted only by the exact CMPR conversion.
CompactTextureData prepare_compact_texture(const gfx::TextureDesc& desc,bool exactCmpr=true);
}
