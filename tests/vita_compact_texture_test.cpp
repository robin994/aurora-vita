#include "gxm/gxm_texture_layout.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
using namespace aurora::vita;
using namespace gfx;
static size_t checks;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
static unsigned expand5(unsigned v){return (v<<3)|(v>>2);}
static unsigned expand6(unsigned v){return (v<<2)|(v>>4);}
static size_t address(unsigned x,unsigned y,unsigned w,unsigned h) {
  unsigned shared=0;while((1u<<shared)<std::min(w,h))++shared;
  size_t a=0;for(unsigned b=0;b<shared;++b)a|=((y>>b)&1u)<<(2*b)|((x>>b)&1u)<<(2*b+1);
  return a|((size_t(x>>shared)|size_t(y>>shared))<<(2*shared));
}
int main(){
  std::mt19937 rng(3749);
  for(auto fmt:{TextureFormat::I4,TextureFormat::I8,TextureFormat::IA4,TextureFormat::IA8,TextureFormat::RGB565})
    for(auto dimensions:{std::pair{32u,16u},{16u,32u},{1u,32u},{32u,1u},{1u,1u},{9u,7u}}) {
      TextureDesc d;d.width=dimensions.first;d.height=dimensions.second;d.format=fmt;
      const bool pow2=(d.width&(d.width-1))==0&&(d.height&(d.height-1))==0;
      unsigned levels=1;if(pow2)for(unsigned n=std::max(d.width,d.height);n>1;n>>=1)++levels;
      d.mipCount=levels;
      std::vector<uint8_t> source(encoded_mip_chain_size(d.width,d.height,fmt,levels));for(auto& b:source)b=rng();
      d.data=source.data();d.dataSize=source.size();
      const auto out=gxm::prepare_compact_texture(d);CHECK(out.ok());CHECK(out.mipCount==levels);CHECK(out.swizzled==pow2);
      size_t input=0,base=0;const size_t bpp=native_texture_bytes_per_pixel(fmt);
      for(unsigned l=0;l<levels;++l) {
        auto m=d;m.width=std::max(1u,d.width>>l);m.height=std::max(1u,d.height>>l);m.mipCount=1;
        m.data=source.data()+input;m.dataSize=encoded_texture_size(m.width,m.height,fmt);input+=m.dataSize;
        std::vector<uint8_t> rgba;CHECK(decode_texture_rgba8(m,rgba));
        const size_t stride=pow2?m.width:out.stride;
        for(unsigned y=0;y<m.height;++y)for(unsigned x=0;x<m.width;++x) {
          const size_t at=base+(pow2?address(x,y,m.width,m.height):size_t(y)*stride+x)*bpp;
          const size_t p=(size_t(y)*m.width+x)*4;
          CHECK(at+bpp<=out.pixels.size());
          if(out.format==NativeTextureFormat::Rgb565){const unsigned v=out.pixels[at]|(unsigned(out.pixels[at+1])<<8);
            CHECK(expand5(v>>11)==rgba[p]);CHECK(expand6((v>>5)&63)==rgba[p+1]);CHECK(expand5(v&31)==rgba[p+2]);
          }else{CHECK(out.pixels[at]==rgba[p]);CHECK(out.pixels[at]==rgba[p+1]);CHECK(out.pixels[at]==rgba[p+2]);
            CHECK((bpp==1?out.pixels[at]:out.pixels[at+1])==rgba[p+3]);}
        }
        base+=stride*m.height*bpp;
      }
      CHECK(base==out.pixels.size());
      --d.dataSize;CHECK(!gxm::prepare_compact_texture(d).ok());++d.dataSize;
      d.generateMipmaps=true;CHECK(!gxm::prepare_compact_texture(d).ok());d.generateMipmaps=false;
      d.mipCount=255;CHECK(!gxm::prepare_compact_texture(d).ok());
    }
  // Exact endpoint-only CMPR is eligible; the differing 5/8 versus 2/3
  // interpolants and non-black transparent RGB must use the RGBA8 fallback.
  std::vector<uint8_t> blocks(32,0);for(unsigned i=0;i<4;++i){blocks[i*8]=0xf8;blocks[i*8+2]=0x07;blocks[i*8+3]=0xe0;}
  TextureDesc d;d.width=8;d.height=8;d.format=TextureFormat::CMPR;d.data=blocks.data();d.dataSize=blocks.size();d.cacheable=true;
  auto bc=gxm::prepare_compact_texture(d);CHECK(bc.ok());CHECK(bc.format==NativeTextureFormat::Bc1);CHECK(bc.pixels.size()==32);
  CHECK(!gxm::prepare_compact_texture(d,false).ok());
  blocks[4]=0x80;CHECK(!gxm::prepare_compact_texture(d).ok());blocks[4]=0;
  for(unsigned i=0;i<4;++i){blocks[i*8]=0;blocks[i*8+1]=0;blocks[i*8+2]=0xff;blocks[i*8+3]=0xff;}
  blocks[4]=0xc0;CHECK(!gxm::prepare_compact_texture(d).ok());blocks[4]=0;
  d.cacheable=false;CHECK(!gxm::prepare_compact_texture(d).ok());d.cacheable=true;
  d.immutableSource=true;d.cacheable=false;
  CHECK(gxm::prepare_compact_texture(d).ok());
  auto native=gxm::prepare_texture_upload(d,true,true);
  CHECK(native.ok());CHECK(native.format==NativeTextureFormat::Bc1);
  CHECK(gxm::texture_upload_budget_bytes(native)==4096);
  auto reference=gxm::prepare_texture_upload(d,false,false);
  CHECK(reference.ok());CHECK(reference.format==NativeTextureFormat::Rgba8);
  CHECK(reference.pixels==gxm::prepare_swizzled_texture(d).pixels);
  d.immutableSource=false;
  CHECK(gxm::prepare_texture_upload(d,true,true).format==NativeTextureFormat::Rgba8);
  d.dataSize=31;CHECK(!gxm::prepare_compact_texture(d).ok());
  CHECK(!gxm::prepare_texture_upload(d).ok());
  std::vector<uint8_t> intensity(128*128,0x5a);
  d={};d.width=128;d.height=128;d.format=TextureFormat::I8;d.data=intensity.data();d.dataSize=intensity.size();
  native=gxm::prepare_texture_upload(d);reference=gxm::prepare_texture_upload(d,false);
  CHECK(gxm::texture_upload_budget_bytes(native)==16384);
  CHECK(gxm::texture_upload_budget_bytes(reference)==65536);
  CHECK(native.pixels.size()==16384);
  std::printf("compact texture levels/bounds/texel equivalence: %zu checks\n",checks);
}
