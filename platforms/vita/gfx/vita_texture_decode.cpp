#include "vita_texture_decode.hpp"
#include <algorithm>
#include <array>

namespace aurora::vita::gfx {
namespace {
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
  if (!p || idx*2+1 >= d.paletteSize) return {255,0,255,255};
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
  const auto*s=static_cast<const uint8_t*>(d.data);size_t off=0;
  auto put8=[&](uint32_t x,uint32_t y,uint8_t v) noexcept {if(x<d.width&&y<d.height)out[static_cast<size_t>(y)*d.width+x]=v;};
  auto put16=[&](uint32_t x,uint32_t y,uint8_t lo,uint8_t hi) noexcept {if(x<d.width&&y<d.height){const size_t p=(static_cast<size_t>(y)*d.width+x)*2u;out[p]=lo;out[p+1]=hi;}};
  switch(d.format){
  case TextureFormat::I4:
    format=NativeTextureFormat::Intensity8;
    for(uint32_t by=0;by<d.height;by+=8)for(uint32_t bx=0;bx<d.width;bx+=8){const auto*tile=s+off;size_t p=0;for(uint32_t y=0;y<8;y++)for(uint32_t x=0;x<8;x+=2){const uint8_t v=tile[p++];put8(bx+x,by+y,expand4(v>>4));put8(bx+x+1,by+y,expand4(v&15));}off+=32;}
    return true;
  case TextureFormat::I8:
    format=NativeTextureFormat::Intensity8;
    for(uint32_t by=0;by<d.height;by+=4)for(uint32_t bx=0;bx<d.width;bx+=8){const auto*tile=s+off;size_t p=0;for(uint32_t y=0;y<4;y++)for(uint32_t x=0;x<8;x++)put8(bx+x,by+y,tile[p++]);off+=32;}
    return true;
  case TextureFormat::IA4:
    format=NativeTextureFormat::LuminanceAlpha8;
    for(uint32_t by=0;by<d.height;by+=4)for(uint32_t bx=0;bx<d.width;bx+=8){const auto*tile=s+off;size_t p=0;for(uint32_t y=0;y<4;y++)for(uint32_t x=0;x<8;x++){const uint8_t v=tile[p++];put16(bx+x,by+y,expand4(v&15),expand4(v>>4));}off+=32;}
    return true;
  case TextureFormat::IA8:
    format=NativeTextureFormat::LuminanceAlpha8;
    for(uint32_t by=0;by<d.height;by+=4)for(uint32_t bx=0;bx<d.width;bx+=4){const auto*tile=s+off;size_t p=0;for(uint32_t y=0;y<4;y++)for(uint32_t x=0;x<4;x++){const uint8_t a=tile[p++],i=tile[p++];put16(bx+x,by+y,i,a);}off+=32;}
    return true;
  case TextureFormat::RGB565:
    format=NativeTextureFormat::Rgb565;
    for(uint32_t by=0;by<d.height;by+=4)for(uint32_t bx=0;bx<d.width;bx+=4){const auto*tile=s+off;size_t p=0;for(uint32_t y=0;y<4;y++)for(uint32_t x=0;x<4;x++){const uint8_t hi=tile[p++],lo=tile[p++];put16(bx+x,by+y,lo,hi);}off+=32;}
    return true;
  default:break;
  }
  format=NativeTextureFormat::None;out.clear();return false;
}

bool transcode_cmpr_to_dxt1(const TextureDesc& d,std::vector<uint8_t>& out) noexcept {
  out.clear();
  if(d.format!=TextureFormat::CMPR||!d.data||!d.width||!d.height)return false;
  const size_t need=encoded_texture_size(d.width,d.height,d.format);
  if(d.dataSize&&d.dataSize<need)return false;

  const uint32_t dstBlocksX=static_cast<uint32_t>(blocks(d.width,4));
  const uint32_t dstBlocksY=static_cast<uint32_t>(blocks(d.height,4));
  out.assign(static_cast<size_t>(dstBlocksX)*dstBlocksY*8u,0);
  const auto* src=static_cast<const uint8_t*>(d.data);
  size_t srcOffset=0;
  for(uint32_t my=0;my<d.height;my+=8){
    for(uint32_t mx=0;mx<d.width;mx+=8){
      // GX CMPR stores four 4x4 BC1-like blocks inside each 8x8 macro tile
      // (TL, TR, BL, BR). DXT1 expects a globally linear 4x4 block grid.
      for(unsigned sub=0;sub<4;sub++){
        const uint32_t bx=mx/4u+(sub&1u);
        const uint32_t by=my/4u+(sub>>1u);
        const auto* in=src+srcOffset+sub*8u;
        if(bx>=dstBlocksX||by>=dstBlocksY)continue;
        auto* dst=out.data()+(static_cast<size_t>(by)*dstBlocksX+bx)*8u;
        // GX endpoints are big-endian; S3TC stores the two RGB565 endpoints LE.
        dst[0]=in[1];dst[1]=in[0];dst[2]=in[3];dst[3]=in[2];
        // GX consumes selector pairs MSB-first per row; DXT1 consumes them LSB-first.
        for(unsigned y=0;y<4;y++)dst[4+y]=reverse_cmpr_selector_pairs(in[4+y]);
      }
      srcOffset+=32u;
    }
  }
  return true;
}

bool decode_texture_rgba8(const TextureDesc& d,std::vector<uint8_t>& out) noexcept {
  out.clear();
  if(!d.data || !d.width || !d.height) return false;
  const size_t need=encoded_texture_size(d.width,d.height,d.format);
  if(d.dataSize && d.dataSize<need) return false;
  out.assign(static_cast<size_t>(d.width)*d.height*4,0);
  const auto* s=static_cast<const uint8_t*>(d.data); size_t off=0;
  if(d.format==TextureFormat::RGBA8888){ std::copy_n(s,need,out.data()); return true; }
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
  return true;
}

DecodeResult decode_texture_rgba8(const TextureDesc& d) noexcept {
  DecodeResult r; r.width=d.width; r.height=d.height;
  r.ok=decode_texture_rgba8(d,r.rgba);
  return r;
}
} // namespace aurora::vita::gfx
