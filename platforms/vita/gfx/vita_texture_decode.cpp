#include "vita_texture_decode.hpp"
#include "vita_cpu_workers.hpp"
#include <algorithm>
#include <array>
#include <cstdio>

namespace aurora::vita::gfx {
namespace {
bool textureDiagnostics=false;
unsigned textureReports=0;
unsigned textureInspections=0;
void report_texture_colors(const TextureDesc& d,const std::vector<uint8_t>& rgba) noexcept {
  if(!textureDiagnostics||textureReports>=32||textureInspections>=256)return;
  ++textureInspections;
  size_t magenta=0,transparentMagenta=0,transparent=0;
  for(size_t i=0;i+3<rgba.size();i+=4){
    if(rgba[i+3]<128)++transparent;
    if(rgba[i]>230&&rgba[i+1]<30&&rgba[i+2]>230){
      ++magenta;if(rgba[i+3]<128)++transparentMagenta;
    }
  }
  if(!magenta)return;
  ++textureReports;
  std::fprintf(stderr,"[aurora-vita] texture_colors source=%llx fmt=%u size=%ux%u magenta=%u transparent_magenta=%u transparent=%u pixels=%u\n",
    static_cast<unsigned long long>(d.sourceId),static_cast<unsigned>(d.format),d.width,d.height,
    static_cast<unsigned>(magenta),static_cast<unsigned>(transparentMagenta),
    static_cast<unsigned>(transparent),static_cast<unsigned>(rgba.size()/4));
}
inline uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
inline uint8_t expand4(uint8_t v) { return static_cast<uint8_t>((v << 4) | v); }
inline uint8_t expand5(uint8_t v) { return static_cast<uint8_t>((v << 3) | (v >> 2)); }
inline uint8_t expand6(uint8_t v) { return static_cast<uint8_t>((v << 2) | (v >> 4)); }
inline uint8_t expand3(uint8_t v) { return static_cast<uint8_t>((v << 5) | (v << 2) | (v >> 1)); }
struct RGBA { uint8_t r,g,b,a; };
RGBA decode_rgb565(uint16_t v) { return {expand5((v>>11)&31), expand6((v>>5)&63), expand5(v&31), 255}; }
RGBA decode_rgb5a3(uint16_t v) {
  if (v & 0x8000) return {expand5((v>>10)&31), expand5((v>>5)&31), expand5(v&31), 255};
  return {expand4((v>>8)&15), expand4((v>>4)&15), expand4(v&15), expand3((v>>12)&7)};
}
RGBA palette_color(const TextureDesc& d, uint32_t idx) {
  const auto* p = static_cast<const uint8_t*>(d.palette);
  if (!p || idx*2+1 >= d.paletteSize) {
#if defined(__vita__)
    // Report malformed palette metadata once per sampled source, not once per
    // texel. Preserve the diagnostic color rather than hide missing GX data.
    static unsigned reported=0;
    static uint64_t lastSource=~uint64_t{0};
    if(reported<8&&lastSource!=d.sourceId){
      lastSource=d.sourceId;++reported;
      std::fprintf(stderr,"[aurora-vita] palette_oob source=%llx palette=%llx fmt=%u size=%ux%u entries=%u index=%u palette_format=%u\n",
        static_cast<unsigned long long>(d.sourceId),static_cast<unsigned long long>(d.paletteSourceId),
        static_cast<unsigned>(d.format),d.width,d.height,static_cast<unsigned>(d.paletteSize/2),idx,
        static_cast<unsigned>(d.paletteFormat));
    }
#endif
    return {255,0,255,255};
  }
  const uint16_t v = be16(p + idx*2);
  switch (d.paletteFormat) {
  case PaletteFormat::IA8: return {static_cast<uint8_t>(v&255),static_cast<uint8_t>(v&255),static_cast<uint8_t>(v&255),static_cast<uint8_t>(v>>8)};
  case PaletteFormat::RGB565: return decode_rgb565(v);
  case PaletteFormat::RGB5A3: return decode_rgb5a3(v);
  default: return {255,0,255,255};
  }
}
void put(std::vector<uint8_t>& out, uint32_t w, uint32_t h, uint32_t x, uint32_t y, RGBA c) {
  if (x>=w || y>=h) return;
  const size_t o=(static_cast<size_t>(y)*w+x)*4;
  out[o]=c.r; out[o+1]=c.g; out[o+2]=c.b; out[o+3]=c.a;
}
size_t blocks(uint32_t n, uint32_t b) { return (n+b-1)/b; }
uint8_t reverse_cmpr_selector_pairs(uint8_t v) noexcept {
  return static_cast<uint8_t>(((v&0xc0u)>>6)|((v&0x30u)>>2)|((v&0x0cu)<<2)|((v&0x03u)<<6));
}
struct CmprTranscodeJob {
  const uint8_t* src=nullptr;
  uint8_t* dst=nullptr;
  uint32_t macroTilesX=0;
  uint32_t dstBlocksX=0;
  uint32_t dstBlocksY=0;
};
bool transcode_cmpr_range(void* opaque,size_t begin,size_t end,uint32_t) noexcept {
  auto& job=*static_cast<CmprTranscodeJob*>(opaque);
  for(size_t tile=begin;tile<end;++tile){
    const uint32_t macroX=static_cast<uint32_t>(tile%job.macroTilesX);
    const uint32_t macroY=static_cast<uint32_t>(tile/job.macroTilesX);
    const auto* macro=job.src+tile*32u;
    for(unsigned sub=0;sub<4;sub++){
      const uint32_t bx=macroX*2u+(sub&1u);
      const uint32_t by=macroY*2u+(sub>>1u);
      if(bx>=job.dstBlocksX||by>=job.dstBlocksY)continue;
      const auto* in=macro+sub*8u;
      auto* dst=job.dst+(static_cast<size_t>(by)*job.dstBlocksX+bx)*8u;
      dst[0]=in[1];dst[1]=in[0];dst[2]=in[3];dst[3]=in[2];
      for(unsigned y=0;y<4;y++)dst[4+y]=reverse_cmpr_selector_pairs(in[4+y]);
    }
  }
  return true;
}
struct NativeTranscodeJob {
  const uint8_t* src=nullptr;
  uint8_t* dst=nullptr;
  uint32_t width=0;
  uint32_t height=0;
  uint32_t tilesX=0;
  uint32_t tileWidth=0;
  uint32_t tileHeight=0;
  TextureFormat format=TextureFormat::RGBA8888;
};
bool transcode_native_range(void* opaque,size_t begin,size_t end,uint32_t) noexcept {
  auto& job=*static_cast<NativeTranscodeJob*>(opaque);
  const auto put8=[&](uint32_t x,uint32_t y,uint8_t value) noexcept {
    if(x<job.width&&y<job.height)job.dst[static_cast<size_t>(y)*job.width+x]=value;
  };
  const auto put16=[&](uint32_t x,uint32_t y,uint8_t lo,uint8_t hi) noexcept {
    if(x>=job.width||y>=job.height)return;
    const size_t offset=(static_cast<size_t>(y)*job.width+x)*2u;
    job.dst[offset]=lo;job.dst[offset+1]=hi;
  };
  for(size_t tileIndex=begin;tileIndex<end;++tileIndex){
    const uint32_t tileX=static_cast<uint32_t>(tileIndex%job.tilesX);
    const uint32_t tileY=static_cast<uint32_t>(tileIndex/job.tilesX);
    const uint32_t bx=tileX*job.tileWidth,by=tileY*job.tileHeight;
    const auto* tile=job.src+tileIndex*32u;
    size_t p=0;
    switch(job.format){
    case TextureFormat::I4:
      for(uint32_t y=0;y<8;y++)for(uint32_t x=0;x<8;x+=2){
        const uint8_t v=tile[p++];
        put8(bx+x,by+y,expand4(v>>4));put8(bx+x+1,by+y,expand4(v&15));
      }
      break;
    case TextureFormat::I8:
      for(uint32_t y=0;y<4;y++)for(uint32_t x=0;x<8;x++)put8(bx+x,by+y,tile[p++]);
      break;
    case TextureFormat::IA4:
      for(uint32_t y=0;y<4;y++)for(uint32_t x=0;x<8;x++){
        const uint8_t v=tile[p++];put16(bx+x,by+y,expand4(v&15),expand4(v>>4));
      }
      break;
    case TextureFormat::IA8:
      for(uint32_t y=0;y<4;y++)for(uint32_t x=0;x<4;x++){
        const uint8_t a=tile[p++],i=tile[p++];put16(bx+x,by+y,i,a);
      }
      break;
    case TextureFormat::RGB565:
      for(uint32_t y=0;y<4;y++)for(uint32_t x=0;x<4;x++){
        const uint8_t hi=tile[p++],lo=tile[p++];put16(bx+x,by+y,lo,hi);
      }
      break;
    default:return false;
    }
  }
  return true;
}
void cmpr_block(const uint8_t* src, std::vector<uint8_t>& out, uint32_t w, uint32_t h, uint32_t ox, uint32_t oy) {
  const uint16_t c0v=be16(src), c1v=be16(src+2);
  std::array<RGBA,4> c{}; c[0]=decode_rgb565(c0v); c[1]=decode_rgb565(c1v);
  auto dxt_blend=[](uint8_t v1,uint8_t v2)->uint8_t{return static_cast<uint8_t>((v1*3u+v2*5u)>>3);};
  if (c0v>c1v) {
    // GameCube/Wii CMPR is not PC DXT1: interpolation uses a 3/8,5/8 blend.
    c[2]={dxt_blend(c[1].r,c[0].r),dxt_blend(c[1].g,c[0].g),dxt_blend(c[1].b,c[0].b),255};
    c[3]={dxt_blend(c[0].r,c[1].r),dxt_blend(c[0].g,c[1].g),dxt_blend(c[0].b,c[1].b),255};
  } else {
    // GX keeps the RGB average for the transparent selector (DXT1 normally uses black).
    RGBA avg{static_cast<uint8_t>((c[0].r+c[1].r)/2),static_cast<uint8_t>((c[0].g+c[1].g)/2),static_cast<uint8_t>((c[0].b+c[1].b)/2),255};
    c[2]=avg; c[3]={avg.r,avg.g,avg.b,0};
  }
  for (uint32_t y=0;y<4;y++) {
    uint8_t bits=src[4+y];
    for (uint32_t x=0;x<4;x++) put(out,w,h,ox+x,oy+y,c[(bits>>(6-2*x))&3]);
  }
}
}

void set_texture_decode_diagnostics(bool enabled) noexcept {
  textureDiagnostics=enabled;textureReports=0;textureInspections=0;
}

size_t encoded_texture_size(uint32_t w,uint32_t h,TextureFormat f) noexcept {
  switch(f){
  case TextureFormat::I4: case TextureFormat::C4: return blocks(w,8)*blocks(h,8)*32;
  case TextureFormat::I8: case TextureFormat::IA4: case TextureFormat::C8: return blocks(w,8)*blocks(h,4)*32;
  case TextureFormat::IA8: case TextureFormat::RGB565: case TextureFormat::RGB5A3: case TextureFormat::C14X2: return blocks(w,4)*blocks(h,4)*32;
  case TextureFormat::RGBA8: return blocks(w,4)*blocks(h,4)*64;
  case TextureFormat::CMPR: return blocks(w,8)*blocks(h,8)*32;
  case TextureFormat::RGBA8888: return static_cast<size_t>(w)*h*4;
  }
  return 0;
}

size_t encoded_mip_chain_size(uint32_t w,uint32_t h,TextureFormat f,uint8_t mipCount) noexcept {
  size_t total=0;const unsigned levels=std::max<unsigned>(1,mipCount);
  for(unsigned level=0;level<levels;level++){total+=encoded_texture_size(std::max(1u,w>>level),std::max(1u,h>>level),f);}
  return total;
}

size_t dxt1_texture_size(uint32_t w,uint32_t h) noexcept {
  if(!w||!h)return 0;
  return blocks(w,4)*blocks(h,4)*8u;
}

uint8_t native_texture_bytes_per_pixel(TextureFormat f) noexcept {
  switch(f){
  case TextureFormat::I4:
  case TextureFormat::I8:return 1;
  case TextureFormat::IA4:
  case TextureFormat::IA8:
  case TextureFormat::RGB565:return 2;
  default:return 0;
  }
}

bool transcode_texture_native(const TextureDesc&d,NativeTextureFormat&format,std::vector<uint8_t>&out) noexcept {
  format=NativeTextureFormat::None;out.clear();
  if(!d.data||!d.width||!d.height)return false;
  const uint8_t bpp=native_texture_bytes_per_pixel(d.format);if(!bpp)return false;
  const size_t need=encoded_texture_size(d.width,d.height,d.format);
  if(d.dataSize&&d.dataSize<need)return false;
  out.assign(static_cast<size_t>(d.width)*d.height*bpp,0);
  uint32_t tileWidth=0,tileHeight=0;
  switch(d.format){
  case TextureFormat::I4:
    format=NativeTextureFormat::Intensity8;
    tileWidth=8;tileHeight=8;break;
  case TextureFormat::I8:
    format=NativeTextureFormat::Intensity8;
    tileWidth=8;tileHeight=4;break;
  case TextureFormat::IA4:
    format=NativeTextureFormat::LuminanceAlpha8;
    tileWidth=8;tileHeight=4;break;
  case TextureFormat::IA8:
    format=NativeTextureFormat::LuminanceAlpha8;
    tileWidth=4;tileHeight=4;break;
  case TextureFormat::RGB565:
    format=NativeTextureFormat::Rgb565;
    tileWidth=4;tileHeight=4;break;
  default:break;
  }
  if(!tileWidth||!tileHeight){format=NativeTextureFormat::None;out.clear();return false;}
  const uint32_t tilesX=static_cast<uint32_t>(blocks(d.width,tileWidth));
  const uint32_t tilesY=static_cast<uint32_t>(blocks(d.height,tileHeight));
  NativeTranscodeJob job{static_cast<const uint8_t*>(d.data),out.data(),d.width,d.height,
                         tilesX,tileWidth,tileHeight,d.format};
  return cpu_parallel_for(static_cast<size_t>(tilesX)*tilesY,transcode_native_range,&job);
}

bool transcode_cmpr_to_dxt1(const TextureDesc& d,std::vector<uint8_t>& out) noexcept {
  out.clear();
  if(d.format!=TextureFormat::CMPR||!d.data||!d.width||!d.height)return false;
  const size_t need=encoded_texture_size(d.width,d.height,d.format);
  if(d.dataSize&&d.dataSize<need)return false;

  const uint32_t dstBlocksX=static_cast<uint32_t>(blocks(d.width,4));
  const uint32_t dstBlocksY=static_cast<uint32_t>(blocks(d.height,4));
  out.assign(static_cast<size_t>(dstBlocksX)*dstBlocksY*8u,0);
  const uint32_t macroTilesX=static_cast<uint32_t>(blocks(d.width,8));
  const uint32_t macroTilesY=static_cast<uint32_t>(blocks(d.height,8));
  CmprTranscodeJob job{static_cast<const uint8_t*>(d.data),out.data(),macroTilesX,dstBlocksX,dstBlocksY};
  // Each 8x8 GX macro tile maps to four independent DXT1 blocks, so larger
  // textures can use the same persistent Vita worker pool as vertex preparation
  // without adding synchronization inside the transcode loop.
  return cpu_parallel_for(static_cast<size_t>(macroTilesX)*macroTilesY,transcode_cmpr_range,&job);
}

bool decode_texture_rgba8(const TextureDesc& d,std::vector<uint8_t>& out) noexcept {
  out.clear();
  if(!d.data || !d.width || !d.height) return false;
  const size_t need=encoded_texture_size(d.width,d.height,d.format);
  if(d.dataSize && d.dataSize<need) return false;
  out.assign(static_cast<size_t>(d.width)*d.height*4,0);
  const auto* s=static_cast<const uint8_t*>(d.data); size_t off=0;
  if(d.format==TextureFormat::RGBA8888){ std::copy_n(s,need,out.data()); report_texture_colors(d,out); return true; }
  auto tile=[&](uint32_t bw,uint32_t bh,auto fn){
    for(uint32_t by=0;by<d.height;by+=bh) for(uint32_t bx=0;bx<d.width;bx+=bw) fn(bx,by);
  };
  switch(d.format){
  case TextureFormat::I4: tile(8,8,[&](uint32_t bx,uint32_t by){for(uint32_t y=0;y<8;y++)for(uint32_t x=0;x<8;x+=2){uint8_t v=s[off++];uint8_t a=expand4(v>>4),b=expand4(v&15);put(out,d.width,d.height,bx+x,by+y,{a,a,a,a});put(out,d.width,d.height,bx+x+1,by+y,{b,b,b,b});}});break;
  case TextureFormat::I8: tile(8,4,[&](uint32_t bx,uint32_t by){for(uint32_t y=0;y<4;y++)for(uint32_t x=0;x<8;x++){uint8_t v=s[off++];put(out,d.width,d.height,bx+x,by+y,{v,v,v,v});}});break;
  case TextureFormat::IA4: tile(8,4,[&](uint32_t bx,uint32_t by){for(uint32_t y=0;y<4;y++)for(uint32_t x=0;x<8;x++){uint8_t v=s[off++],a=expand4(v>>4),i=expand4(v&15);put(out,d.width,d.height,bx+x,by+y,{i,i,i,a});}});break;
  case TextureFormat::IA8: tile(4,4,[&](uint32_t bx,uint32_t by){for(uint32_t y=0;y<4;y++)for(uint32_t x=0;x<4;x++){uint8_t a=s[off++],i=s[off++];put(out,d.width,d.height,bx+x,by+y,{i,i,i,a});}});break;
  case TextureFormat::RGB565: tile(4,4,[&](uint32_t bx,uint32_t by){for(uint32_t y=0;y<4;y++)for(uint32_t x=0;x<4;x++){put(out,d.width,d.height,bx+x,by+y,decode_rgb565(be16(s+off)));off+=2;}});break;
  case TextureFormat::RGB5A3: tile(4,4,[&](uint32_t bx,uint32_t by){for(uint32_t y=0;y<4;y++)for(uint32_t x=0;x<4;x++){put(out,d.width,d.height,bx+x,by+y,decode_rgb5a3(be16(s+off)));off+=2;}});break;
  case TextureFormat::RGBA8: tile(4,4,[&](uint32_t bx,uint32_t by){size_t base=off; for(uint32_t y=0;y<4;y++)for(uint32_t x=0;x<4;x++){size_t i=y*4+x; RGBA c{s[base+i*2+1],s[base+32+i*2],s[base+32+i*2+1],s[base+i*2]};put(out,d.width,d.height,bx+x,by+y,c);}off+=64;});break;
  case TextureFormat::C4: tile(8,8,[&](uint32_t bx,uint32_t by){for(uint32_t y=0;y<8;y++)for(uint32_t x=0;x<8;x+=2){uint8_t v=s[off++];put(out,d.width,d.height,bx+x,by+y,palette_color(d,v>>4));put(out,d.width,d.height,bx+x+1,by+y,palette_color(d,v&15));}});break;
  case TextureFormat::C8: tile(8,4,[&](uint32_t bx,uint32_t by){for(uint32_t y=0;y<4;y++)for(uint32_t x=0;x<8;x++)put(out,d.width,d.height,bx+x,by+y,palette_color(d,s[off++]));});break;
  case TextureFormat::C14X2: tile(4,4,[&](uint32_t bx,uint32_t by){for(uint32_t y=0;y<4;y++)for(uint32_t x=0;x<4;x++){uint16_t v=be16(s+off)&0x3fff;off+=2;put(out,d.width,d.height,bx+x,by+y,palette_color(d,v));}});break;
  case TextureFormat::CMPR: tile(8,8,[&](uint32_t bx,uint32_t by){cmpr_block(s+off,out,d.width,d.height,bx,by);cmpr_block(s+off+8,out,d.width,d.height,bx+4,by);cmpr_block(s+off+16,out,d.width,d.height,bx,by+4);cmpr_block(s+off+24,out,d.width,d.height,bx+4,by+4);off+=32;});break;
  case TextureFormat::RGBA8888: break;
  }
  report_texture_colors(d,out);
  return true;
}

DecodeResult decode_texture_rgba8(const TextureDesc& d) noexcept {
  DecodeResult r; r.width=d.width; r.height=d.height;
  r.ok=decode_texture_rgba8(d,r.rgba);
  return r;
}
} // namespace aurora::vita::gfx
