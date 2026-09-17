#include "gxm_texture_layout.hpp"
#include <algorithm>
#include <cstring>

namespace aurora::vita::gxm {
LinearTextureData prepare_swizzled_texture(const gfx::TextureDesc& desc) {
  LinearTextureData out;
  if (!desc.width || !desc.height || desc.width > 4096 || desc.height > 4096 ||
      (desc.width & (desc.width-1u)) || (desc.height & (desc.height-1u)) ||
      desc.mipCount > 1 || desc.generateMipmaps) {
    out.error="swizzled upload requires a single power-of-two level";
    return out;
  }
  std::vector<uint8_t> linear;
  if (!gfx::decode_texture_rgba8(desc,linear)) {out.error="swizzled GX decode failed";return out;}
  out.pixels.resize(size_t(desc.width)*desc.height*4);
  unsigned shared=0;
  while ((1u<<shared) < std::min(desc.width,desc.height)) ++shared;
  // Interleave common Y/X bits, then append the rectangular major axis.
  const auto spread=[shared](uint32_t value,unsigned axis) {
    uint32_t result=0;
    for(unsigned bit=0;bit<shared;++bit) result|=((value>>bit)&1u)<<(bit*2u+axis);
    return result | ((value>>shared)<<(shared*2u));
  };
  std::vector<uint32_t> xOffsets(desc.width);
  for(unsigned x=0;x<desc.width;++x) xOffsets[x]=spread(x,1);
  for(unsigned y=0;y<desc.height;++y) {
    const uint32_t yOffset=spread(y,0);
    for(unsigned x=0;x<desc.width;++x)
      std::memcpy(out.pixels.data()+size_t(xOffsets[x]|yOffset)*4,
                  linear.data()+(size_t(y)*desc.width+x)*4,4);
  }
  return out;
}

LinearTextureData prepare_linear_texture(const gfx::TextureDesc& desc) {
  LinearTextureData out;
  const auto fail=[&](const char* text) {out.error=text;out.pixels.clear();return out;};
  if(!desc.data||!desc.width||!desc.height||desc.width>4096||desc.height>4096)return fail("invalid native texture dimensions");
  const bool pow2=(desc.width&(desc.width-1u))==0 && (desc.height&(desc.height-1u))==0;
  const unsigned explicitLevels=std::max<unsigned>(desc.mipCount,1);
  unsigned maxLevels=1;
  for(unsigned n=std::max(desc.width,desc.height);n>1;n>>=1)++maxLevels;
  if(explicitLevels>maxLevels)return fail("too many GX mip levels");
  out.mipCount=desc.generateMipmaps?maxLevels:explicitLevels;
  if(out.mipCount>1&&!pow2)return fail("NPOT native mip chains are not supported");
  size_t total=0;
  for(unsigned i=0;i<out.mipCount;++i)
    total+=size_t((std::max(1u,desc.width>>i)+7u)&~7u)*std::max(1u,desc.height>>i)*4;
  out.pixels.resize(total,0);
  std::vector<uint8_t> previous;
  uint32_t pw=0,ph=0;
  size_t inputOffset=0,outputOffset=0;
  for(unsigned level=0;level<out.mipCount;++level) {
    const uint32_t w=std::max(1u,desc.width>>level),h=std::max(1u,desc.height>>level);
    const uint32_t rowBytes=((w+7u)&~7u)*4;
    std::vector<uint8_t> pixels;
    if(level<explicitLevels) {
      auto ld=desc;ld.width=w;ld.height=h;ld.mipCount=1;ld.generateMipmaps=false;
      const size_t encoded=gfx::encoded_texture_size(w,h,ld.format);
      if(inputOffset>desc.dataSize||encoded>desc.dataSize-inputOffset)return fail("truncated GX mip chain");
      ld.data=static_cast<const uint8_t*>(desc.data)+inputOffset;ld.dataSize=encoded;
      if(!gfx::decode_texture_rgba8(ld,pixels))return fail("GX mip decode failed");
      inputOffset+=encoded;
    } else {
      pixels.resize(size_t(w)*h*4);
      for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x)for(unsigned c=0;c<4;++c) {
        unsigned sum=0;
        for(unsigned dy=0;dy<2;++dy)for(unsigned dx=0;dx<2;++dx)
          sum+=previous[(size_t(std::min(2*y+dy,ph-1))*pw+std::min(2*x+dx,pw-1))*4+c];
        pixels[(size_t(y)*w+x)*4+c]=uint8_t((sum+2)/4);
      }
    }
    for(uint32_t y=0;y<h;++y)
      std::memcpy(out.pixels.data()+outputOffset+size_t(y)*rowBytes,pixels.data()+size_t(y)*w*4,size_t(w)*4);
    outputOffset+=size_t(rowBytes)*h;
    if(desc.generateMipmaps) {previous=std::move(pixels);pw=w;ph=h;}
  }
  return out;
}
}
