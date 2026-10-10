#include "gfx/vita_texture_snapshots.hpp"
#include "gxm/gxm_texture_layout.hpp"
#include <cstdio>
#include <cstdlib>
#include <array>
using namespace aurora::vita::gfx;
static unsigned checks;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
static uint64_t collision(const TextureDesc&) noexcept {return 7;}
int main() {
  std::array<uint8_t,256> pixels{};for(unsigned i=0;i<pixels.size();++i)pixels[i]=uint8_t(i);
  TextureDesc d;d.width=64;d.height=4;d.format=TextureFormat::I8;
  d.data=pixels.data();d.dataSize=pixels.size();d.cacheable=false;d.sourceId=0x90000000;
  TextureSnapshots cache;
  CHECK(!cache.eligible(d));CHECK(!cache.find(d,0));
  cache.configure(8192);CHECK(cache.eligible(d));CHECK(!cache.find(d,0));
  const auto oldGpu=aurora::vita::gxm::prepare_texture_upload(d);
  CHECK(oldGpu.ok());CHECK(cache.remember(d,1,4096,0));
  // Thousands of identical transient draws consume one upload, including
  // multiple lookups in a frame and unrelated guest identities/revisions.
  for(unsigned frame=0;frame<1200;++frame) {
    d.sourceId+=32;d.revision++;d.cacheable=bool(frame&1);
    CHECK(cache.find(d,frame)==1);
  }
  auto same=pixels;d.data=same.data();CHECK(cache.find(d,1200)==1);
  CHECK(cache.stats().uploads==1&&cache.stats().entries==1&&cache.stats().gpuBytes==4096);
  // In-place writes must create a different immutable upload. A previously
  // submitted handle retains its pixels even with two updates in one frame.
  same[3]^=0xff;CHECK(!cache.find(d,1200));
  const auto nextGpu=aurora::vita::gxm::prepare_texture_upload(d);
  CHECK(nextGpu.ok()&&oldGpu.pixels!=nextGpu.pixels);
  CHECK(cache.remember(d,2,4096,1200));CHECK(cache.find(d,1200)==2);
  d.data=pixels.data();CHECK(cache.find(d,1200)==1);
  CHECK(!cache.has_room(4096));CHECK(!cache.oldest_before(1200));
  CHECK(cache.oldest_before(1201)!=0);cache.erase(2);
  CHECK(cache.has_room(4096)&&cache.stats().gpuBytes==4096&&cache.stats().sourceBytes==256);
  CHECK(!cache.remember(d,1,4096,1201));
  // Shape, format and palette are part of exact identity. Force every hash to
  // collide so byte comparison, rather than probabilistic identity, is tested.
  TextureSnapshots exact(collision);exact.configure(16384);
  CHECK(exact.remember(d,10,4096,1));d.data=same.data();
  CHECK(!exact.find(d,1));CHECK(exact.remember(d,11,4096,1));CHECK(exact.find(d,1)==11);
  d.width=32;d.height=8;CHECK(!exact.find(d,1));
  d.width=64;d.height=4;d.format=TextureFormat::I4;CHECK(!exact.find(d,1));
  std::array<uint8_t,32> indices{},palette{};
  d={};d.width=d.height=8;d.format=TextureFormat::C4;d.paletteFormat=PaletteFormat::RGB565;
  d.data=indices.data();d.dataSize=indices.size();d.palette=palette.data();d.paletteSize=palette.size();
  CHECK(exact.remember(d,12,4096,1));CHECK(exact.find(d,1)==12);
  palette[0]=1;CHECK(!exact.find(d,1));CHECK(exact.remember(d,13,4096,1));
  CHECK(exact.find(d,1)==13);d.paletteFormat=PaletteFormat::RGB5A3;CHECK(!exact.find(d,1));
  // Reject videos, mip chains, generated mips, invalid bounds and missing data
  // before reading any source. The ordinary upload/ring path handles these.
  d.paletteFormat=PaletteFormat::RGB565;d.palette=nullptr;CHECK(!cache.eligible(d));
  d.palette=palette.data();d.data=nullptr;CHECK(!cache.eligible(d));d.data=indices.data();
  d.dataSize=31;CHECK(!cache.eligible(d));d.dataSize=32;
  d.paletteSize=TextureSnapshots::MaxSourceBytes;CHECK(!cache.eligible(d));d.paletteSize=32;
  d.mipCount=2;CHECK(!cache.eligible(d));d.mipCount=1;
  d.generateMipmaps=true;CHECK(!cache.eligible(d));d.generateMipmaps=false;
  d.width=0;CHECK(!cache.eligible(d));d.width=0xffffffff;CHECK(!cache.eligible(d));
  // Bound both GPU bytes and the number of CPU identity records. If all entries
  // are used by this frame no victim is eligible; the caller keeps its fallback.
  TextureSnapshots bounded;bounded.configure(8*1024*1024);
  d={};d.width=64;d.height=4;d.format=TextureFormat::I8;d.data=pixels.data();d.dataSize=pixels.size();
  for(unsigned i=1;i<=TextureSnapshots::MaxEntries;++i)CHECK(bounded.remember(d,i,4096,3));
  CHECK(!bounded.has_room(4096));CHECK(!bounded.oldest_before(3));
  CHECK(!bounded.remember(d,1025,4096,3));CHECK(bounded.oldest_before(4));
  bounded.clear();CHECK(!bounded.find(d,4));
  CHECK(bounded.stats().entries==0&&bounded.stats().gpuBytes==0&&bounded.stats().sourceBytes==0);
  cache.clear();CHECK(!cache.find(d,1202));cache.configure(0);CHECK(!cache.eligible(d));
  std::printf("%u texture snapshot identity, palette, collision, mutation, pinning and budget checks passed\n",checks);
}
