#include "gxm_texture_layout.hpp"
#include <algorithm>
#include <cstring>

namespace aurora::vita::gxm {
namespace {
void append_swizzled_units(std::vector<uint8_t>& destination,const uint8_t* linear,
                           uint32_t width,uint32_t height,size_t unitBytes) {
  const size_t base=destination.size();
  destination.resize(base+size_t(width)*height*unitBytes);
  unsigned shared=0;
  while((1u<<shared)<std::min(width,height))++shared;
  const auto spread=[shared](uint32_t value,unsigned axis) {
    uint32_t result=0;
    for(unsigned bit=0;bit<shared;++bit)result|=((value>>bit)&1u)<<(bit*2u+axis);
    return result|((value>>shared)<<(shared*2u));
  };
  std::vector<uint32_t> xOffsets(width);
  for(uint32_t x=0;x<width;++x)xOffsets[x]=spread(x,1);
  for(uint32_t y=0;y<height;++y) {
    const uint32_t yOffset=spread(y,0);
    for(uint32_t x=0;x<width;++x)
      std::memcpy(destination.data()+base+size_t(xOffsets[x]|yOffset)*unitBytes,
                  linear+(size_t(y)*width+x)*unitBytes,unitBytes);
  }
}
}

LinearTextureData prepare_swizzled_texture(const gfx::TextureDesc& desc) {
  LinearTextureData out;
  if (!desc.data || !desc.width || !desc.height || desc.width > 4096 || desc.height > 4096 ||
      (desc.width & (desc.width-1u)) || (desc.height & (desc.height-1u))) {
    out.error="swizzled upload requires power-of-two dimensions";
    return out;
  }
  const unsigned explicitLevels=std::max<unsigned>(desc.mipCount,1);
  unsigned maxLevels=1;
  for(unsigned n=std::max(desc.width,desc.height);n>1;n>>=1)++maxLevels;
  if(explicitLevels>maxLevels){out.error="too many swizzled GX mip levels";return out;}
  out.mipCount=desc.generateMipmaps?maxLevels:explicitLevels;

  size_t inputOffset=0;
  std::vector<uint8_t> previous;
  uint32_t previousWidth=0,previousHeight=0;
  for(unsigned level=0;level<out.mipCount;++level) {
    const uint32_t width=std::max(1u,desc.width>>level);
    const uint32_t height=std::max(1u,desc.height>>level);
    std::vector<uint8_t> linear;
    if(level<explicitLevels) {
      auto mip=desc;mip.width=width;mip.height=height;mip.mipCount=1;mip.generateMipmaps=false;
      const size_t encoded=gfx::encoded_texture_size(width,height,mip.format);
      if(inputOffset>desc.dataSize||encoded>desc.dataSize-inputOffset){
        out.error="truncated swizzled GX mip chain";out.pixels.clear();return out;
      }
      mip.data=static_cast<const uint8_t*>(desc.data)+inputOffset;mip.dataSize=encoded;
      if(!gfx::decode_texture_rgba8(mip,linear)){
        out.error="swizzled GX mip decode failed";out.pixels.clear();return out;
      }
      inputOffset+=encoded;
    } else {
      linear.resize(size_t(width)*height*4);
      for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x)for(unsigned c=0;c<4;++c) {
        unsigned sum=0;
        for(unsigned dy=0;dy<2;++dy)for(unsigned dx=0;dx<2;++dx)
          sum+=previous[(size_t(std::min(2*y+dy,previousHeight-1))*previousWidth+
                         std::min(2*x+dx,previousWidth-1))*4+c];
        linear[(size_t(y)*width+x)*4+c]=static_cast<uint8_t>((sum+2)/4);
      }
    }
    append_swizzled_units(out.pixels,linear.data(),width,height,4);
    if(desc.generateMipmaps){previous=std::move(linear);previousWidth=width;previousHeight=height;}
  }
  return out;
}

LinearTextureData prepare_ubc1_texture(const gfx::TextureDesc& desc) {
  LinearTextureData out;
  const auto fail=[&](const char* text) {out.error=text;out.pixels.clear();return out;};
  if(desc.format!=gfx::TextureFormat::CMPR||!desc.data||!desc.width||!desc.height||
     desc.width>4096||desc.height>4096)
    return fail("invalid UBC1 texture");
  if((desc.width&(desc.width-1u))||(desc.height&(desc.height-1u)))
    return fail("UBC1 swizzled upload requires power-of-two dimensions");
  if(desc.generateMipmaps)
    return fail("runtime UBC1 mip generation is not supported");

  const unsigned levels=std::max<unsigned>(desc.mipCount,1);
  unsigned maxLevels=1;
  for(unsigned n=std::max(desc.width,desc.height);n>1;n>>=1)++maxLevels;
  if(levels>maxLevels)return fail("too many UBC1 mip levels");
  out.mipCount=levels;

  size_t inputOffset=0;
  for(unsigned level=0;level<levels;++level) {
    const uint32_t width=std::max(1u,desc.width>>level);
    const uint32_t height=std::max(1u,desc.height>>level);
    const size_t encoded=gfx::encoded_texture_size(width,height,gfx::TextureFormat::CMPR);
    if(desc.dataSize&&(inputOffset>desc.dataSize||encoded>desc.dataSize-inputOffset))
      return fail("truncated UBC1 GX mip chain");
    auto mip=desc;
    mip.width=width;mip.height=height;mip.mipCount=1;mip.generateMipmaps=false;
    mip.data=static_cast<const uint8_t*>(desc.data)+inputOffset;mip.dataSize=encoded;
    std::vector<uint8_t> linearBlocks;
    if(!gfx::transcode_cmpr_to_dxt1(mip,linearBlocks))
      return fail("CMPR to UBC1 transcode failed");
    const uint32_t blocksX=std::max(1u,(width+3u)/4u);
    const uint32_t blocksY=std::max(1u,(height+3u)/4u);
    if(linearBlocks.size()!=size_t(blocksX)*blocksY*8u)
      return fail("invalid UBC1 block count");
    append_swizzled_units(out.pixels,linearBlocks.data(),blocksX,blocksY,8);
    inputOffset+=encoded;
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
