#include "aurora_vita_backend.hpp"
#include "gfx/vita_renderer.hpp"
#include "gfx/vita_vertex_decode.hpp"
#include "gx/aurora_vita_draw_sink.hpp"
#include <dolphin/gx.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#if !defined(AURORA_VITA_UPSTREAM)
#error The selected backend must export its GX frontend ABI to consumers
#endif

#ifndef AURORA_TEST_RENDERER
#define AURORA_TEST_RENDERER "unknown"
#endif
namespace {
using namespace aurora::vita;
void setup_gx() {
  GXInit(nullptr,0);
  AuroraSetViewportPolicy(AURORA_VIEWPORT_NATIVE);
  const float projection[4][4]{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}};
  const float identity[3][4]{{1,0,0,0},{0,1,0,0},{0,0,1,0}};
  GXSetProjection(projection,GX_ORTHOGRAPHIC);
  GXLoadPosMtxImm(identity,GX_PNMTX0);GXSetCurrentMtx(GX_PNMTX0);
  GXSetViewport(0,0,960,544,0,1);GXSetScissor(0,0,960,544);
  GXSetCullMode(GX_CULL_NONE);GXSetZMode(GX_TRUE,GX_LEQUAL,GX_TRUE);
  GXSetZCompLoc(GX_FALSE);GXSetColorUpdate(GX_TRUE);GXSetAlphaUpdate(GX_TRUE);
  GXSetAlphaCompare(GX_ALWAYS,0,GX_AOP_AND,GX_ALWAYS,0);
  GXSetNumChans(1);
  GXSetChanCtrl(GX_COLOR0A0,GX_FALSE,GX_SRC_REG,GX_SRC_VTX,0,GX_DF_NONE,GX_AF_NONE);
}
void material(bool textured) {
  GXClearVtxDesc();
  GXSetVtxDesc(GX_VA_POS,GX_DIRECT);GXSetVtxDesc(GX_VA_CLR0,GX_DIRECT);
  GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_POS,GX_POS_XYZ,GX_F32,0);
  GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_CLR0,GX_CLR_RGBA,GX_RGBA8,0);
  GXSetNumTevStages(1);GXSetNumTexGens(textured?1:0);
  if(textured) {
    GXSetVtxDesc(GX_VA_TEX0,GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_TEX0,GX_TEX_ST,GX_F32,0);
    GXSetTexCoordGen(GX_TEXCOORD0,GX_TG_MTX2x4,GX_TG_TEX0,GX_IDENTITY);
  }
  GXSetTevOrder(GX_TEVSTAGE0,textured?GX_TEXCOORD0:GX_TEXCOORD_NULL,
      textured?GX_TEXMAP0:GX_TEXMAP_NULL,GX_COLOR0A0);
  GXSetTevOp(GX_TEVSTAGE0,textured?GX_MODULATE:GX_PASSCLR);
}
void quad(float x,float y,float size,uint8_t r,uint8_t g,uint8_t b,bool textured=false) {
  const float uv[4][2]{{0,0},{1,0},{1,1},{0,1}};
  GXBegin(GX_QUADS,GX_VTXFMT0,4);
  for(unsigned i=0;i<4;++i) {
    GXPosition3f32(x+uv[i][0]*size,y+uv[i][1]*size,-.5f);
    GXColor4u8(r,g,b,255);
    if(textured)GXTexCoord2f32(uv[i][0],uv[i][1]);
  }
}
bool efb_contract() {
  auto& r=renderer();
  const auto target=r.create_efb(64,64,true);
  if(!target||!begin_frame()||!r.bind_efb(target)) {
    std::fprintf(stderr,"[gx-contract] EFB create/bind failed handle=%u\n",target);return false;
  }
  r.clear_current({.25f,0,0,.5f},1,true,true,true);
  // Separate clear masks must survive native scene boundaries/readback.
  r.clear_current({0,1,0,0},.25f,false,false,true);
  r.clear_current({0,0,1,.8f},1,false,true,false);
  std::vector<uint8_t> pixels;
  if(!r.efb().read_rgba(target,pixels)||pixels.size()!=64u*64u*4u) {
    std::fprintf(stderr,"[gx-contract] EFB readback failed size=%u\n",unsigned(pixels.size()));return false;
  }
  for(size_t p=0;p<pixels.size();p+=4)
    if(std::abs(int(pixels[p])-64)>1||pixels[p+1]!=0||pixels[p+2]!=0||std::abs(int(pixels[p+3])-204)>1) {
      std::fprintf(stderr,"[gx-contract] clear mask pixel=%u rgba=%u,%u,%u,%u expected=64,0,0,204\n",
        unsigned(p/4),pixels[p],pixels[p+1],pixels[p+2],pixels[p+3]);return false;
    }
  const auto copy=r.capture_current(0,{0,0,64,64},32,32,gfx::EfbCopyFormat::R8,true,true);
  if(!copy||!r.efb().read_rgba(copy,pixels)||pixels.size()!=32u*32u*4u) {
    std::fprintf(stderr,"[gx-contract] EFB copy/read failed handle=%u size=%u\n",copy,unsigned(pixels.size()));return false;
  }
  for(auto v:pixels)if(std::abs(int(v)-64)>1) {
    std::fprintf(stderr,"[gx-contract] EFB R8 copy byte=%u expected=64\n",v);return false;
  }
  if(!r.blit_efb(copy))return false;
  end_frame();
  if(!r.readback_rgba8(pixels)||pixels.size()!=960u*544u*4u)return false;
  for(size_t p=0;p<pixels.size();p+=4)
    for(unsigned c=0;c<3;++c)if(std::abs(int(pixels[p+c])-64)>1) {
      std::fprintf(stderr,"[gx-contract] EFB sampled copy pixel=%u channel=%u value=%u expected=64\n",
          unsigned(p/4),c,pixels[p+c]);return false;
    }
  r.efb().destroy(copy);r.efb().destroy(target);
  return !r.failed();
}
bool extended_contract() {
  using namespace gfx;
  auto& r=renderer();
  PipelineDesc p{};p.layout=gpu_vertex_layout();p.depthTest=false;p.depthWrite=false;p.cull=CullMode::None;
  p.texgenCount=1;p.tev.stages[0].texture=0;p.tev.stages[0].texCoord=0;
  p.tev.stages[0].color.d=TevColorArg::TexColor;p.tev.stages[0].alpha.d=TevAlphaArg::TexAlpha;
  for(unsigned fog=1;fog<=unsigned(FogMode::RevExp2);++fog)for(bool range:{false,true}) {
    auto d=p;d.fogMode=FogMode(fog);d.fogRangeEnabled=range;
    std::fprintf(stderr,"[gx-contract] compile fog mode=%u range=%u\n",fog,unsigned(range));
    if(!r.create_pipeline(d)) {
      std::fprintf(stderr,"[gx-contract] fog shader failed mode=%u range=%u\n",fog,unsigned(range));return false;
    }
  }
  for(unsigned matrix=0;matrix<=unsigned(IndirectMatrix::T2);++matrix) {
    auto d=p;d.tev.indirectStageCount=1;d.tev.indirectStages[0]={0,0,1,2};
    auto& s=d.tev.stages[0];s.indirectEnabled=true;s.indirectMatrix=IndirectMatrix(matrix);
    s.indirectBias=IndirectBias::STU;s.indirectAlpha=IndirectAlphaSel::T;
    s.indirectAddPrev=true;s.indirectWrapS=IndirectWrap::W16;s.rasterSource=RasterSource::AlphaBumpN;
    s.color.c=TevColorArg::RasColor;
    std::fprintf(stderr,"[gx-contract] compile indirect matrix=%u\n",matrix);
    if(!r.create_pipeline(d)) {
      std::fprintf(stderr,"[gx-contract] indirect shader failed matrix=%u\n",matrix);return false;
    }
  }
  const auto pipeline=r.create_pipeline(p);
  if(!pipeline) {std::fprintf(stderr,"[gx-contract] mip sample shader failed\n");return false;}
  std::array<uint8_t,84> texels{};
  for(unsigned i=0;i<21;++i) {
    texels[i*4+(i<16?0:i<20?1:2)]=255;texels[i*4+3]=255;
  }
  TextureDesc td{};td.width=td.height=4;td.format=TextureFormat::RGBA8888;
  td.data=texels.data();td.dataSize=texels.size();td.mipCount=3;td.cacheable=false;
  const auto texture=r.create_texture(td),target=r.create_efb(16,16,true);
  std::array<GpuVertex,4> v{};
  const float xy[4][2]{{-1,-1},{1,-1},{1,1},{-1,1}};
  for(unsigned i=0;i<4;++i) {
    v[i].position[0]=xy[i][0];v[i].position[1]=xy[i][1];v[i].position[2]=-.5f;
    v[i].texcoord[0][0]=(xy[i][0]+1)*.5f;v[i].texcoord[0][1]=(xy[i][1]+1)*.5f;v[i].texcoord[0][2]=1;
  }
  const uint16_t idx[]{0,1,2,0,2,3};
  const auto vb=r.create_vertex_buffer(v.data(),sizeof(v),true),ib=r.create_index_buffer(idx,sizeof(idx));
  if(!texture||!target||!vb||!ib||!begin_frame()||!r.bind_efb(target)) {
    std::fprintf(stderr,"[gx-contract] mip resources/bind failed tex=%u target=%u vb=%u ib=%u\n",texture,target,vb,ib);return false;
  }
  r.clear_current({0,0,0,1},1,true,true,true);
  DrawPacket draw{};draw.pipelineKey=pipeline;draw.vertices={vb,0,sizeof(v)};draw.indices={ib,0,sizeof(idx)};
  draw.vertexCount=4;draw.indexCount=6;draw.viewport.width=draw.viewport.height=16;
  draw.scissor.width=draw.scissor.height=16;draw.textures[0].texture=texture;
  auto& sampler=draw.textures[0].sampler;
  sampler.minFilter=Filter::NearestMipmapNearest;sampler.magFilter=Filter::Nearest;sampler.lodBias=20;
  r.draw(draw);
  std::vector<uint8_t> pixels;
  if(r.failed()||!r.efb().read_rgba(target,pixels)||pixels.size()!=16u*16u*4u) {
    std::fprintf(stderr,"[gx-contract] mip draw/readback failed size=%u\n",unsigned(pixels.size()));return false;
  }
  for(size_t i=0;i<pixels.size();i+=4)if(pixels[i]>1||pixels[i+1]>1||pixels[i+2]<254) {
    std::fprintf(stderr,"[gx-contract] mip sample pixel=%u rgba=%u,%u,%u,%u expected=0,0,255,255\n",
        unsigned(i/4),pixels[i],pixels[i+1],pixels[i+2],pixels[i+3]);return false;
  }
  // Mutation after GPU consumption must wait before touching the backing block.
  if(!r.update_buffer(vb,v.data(),sizeof(v)))return false;
  r.bind_default();end_frame();
  r.buffers().destroy(vb);r.buffers().destroy(ib);r.textures().erase(texture);r.efb().destroy(target);
  return !r.failed();
}
int run() {
  sceIoMkdir("ux0:data/aurora-vita",0777);
  char path[160];
  std::snprintf(path,sizeof(path),"ux0:data/aurora-vita/gx_frontend_%s.log",AURORA_TEST_RENDERER);
  if(std::freopen(path,"w",stderr))std::setvbuf(stderr,nullptr,_IONBF,0);
  std::fprintf(stderr,"[gx-contract] renderer=%s shared_frontend=1\n",AURORA_TEST_RENDERER);
  BackendConfig config{};
  config.stream_vertex_bytes=512;config.stream_index_bytes=128;config.stream_slots=2;
  config.texture_cache_budget=4*1024*1024;
  config.vgl_circular_pool_size=4*1024*1024;
  config.cpu_worker_threads=0;config.wait_vblank=true;
  // A successful draw count must not hide a missing GXCopyTex binding behind
  // the white fallback texture. This probe only exercises supported GX state.
  config.strict_unsupported=true;
  if(!initialize(config)) {
    std::fprintf(stderr,"[gx-contract] init FAILED %s\n",last_init_failure_detail());return 1;
  }
  if(!efb_contract()) {
    std::fprintf(stderr,"[gx-contract] EFB FAILED %s\n",renderer().last_error());shutdown();return 2;
  }
  std::fprintf(stderr,"[gx-contract] EFB clear_masks/copy/resize/R8/sample PASS\n");
  if(!extended_contract()) {
    std::fprintf(stderr,"[gx-contract] extended shader/mipmap FAILED %s\n",renderer().last_error());shutdown();return 8;
  }
  std::fprintf(stderr,"[gx-contract] 20 fog/indirect shader variants and sampled explicit mip chain PASS\n");
  setup_gx();
  alignas(32) static std::array<uint8_t,64*64*4> copyDestination{};
  GXTexObj copyTexture{};
  for(unsigned frame=0;frame<120;++frame) {
    if(!begin_frame()) {shutdown();return 3;}
    renderer().clear_current({.025f,.04f,.06f,1},1,true,true,true);
    material(false);
    for(unsigned i=0;i<40;++i) {
      const float x=-.95f+float(i%8)*.235f,y=-.85f+float(i/8)*.34f;
      quad(x,y,.18f,uint8_t(80+(frame+i)%170),uint8_t(30+i*5),uint8_t(240-i*3));
    }
    GXSetTexCopySrc(200,180,128,128);
    GXSetTexCopyDst(64,64,GX_TF_RGBA8,GX_FALSE);
    GXCopyTex(copyDestination.data(),GX_FALSE);
    GXInitTexObj(&copyTexture,copyDestination.data(),64,64,GX_TF_RGBA8,GX_CLAMP,GX_CLAMP,GX_FALSE);
    GXLoadTexObj(&copyTexture,GX_TEXMAP0);
    material(true);quad(.5f,.55f,.38f,255,255,255,true);
    GXDrawDone();
    end_frame();
    if(renderer().failed() || draw_sink().strict_failed()) {
      std::fprintf(stderr,"[gx-contract] frame=%u FAILED %s\n",frame,renderer().last_error());shutdown();return 4;
    }
  }
  const auto submitted=draw_sink().submitted_draws();
  std::fprintf(stderr,"[gx-contract] frames=120 gx_draws=%llu\n",static_cast<unsigned long long>(submitted));
  if(submitted!=120u*41u) {shutdown();return 5;}
  std::vector<uint8_t> image;
  if(!renderer().readback_rgba8(image)||image.size()!=960u*544u*4u) {shutdown();return 6;}
  // Interior of the copied quad: it samples source tile 18 from the last frame,
  // away from filter boundaries. A flipped copy or white fallback cannot pass.
  const size_t copyPixel=(25u*960u+740u)*4u;
  const uint8_t expectedCopy[]{uint8_t(80+(119+18)%170),uint8_t(30+18*5),uint8_t(240-18*3)};
  for(unsigned c=0;c<3;++c)if(std::abs(int(image[copyPixel+c])-int(expectedCopy[c]))>8) {
    std::fprintf(stderr,"[gx-contract] GXCopyTex orientation/sample channel=%u actual=%u expected=%u\n",
        c,image[copyPixel+c],expectedCopy[c]);shutdown();return 9;
  }
  std::fprintf(stderr,"[gx-contract] GXCopyTex sampled destination identity/orientation PASS\n");
  size_t colored=0;
  for(size_t i=0;i<image.size();i+=4)if(image[i]>40||image[i+1]>40||image[i+2]>40)++colored;
  if(colored<10000) {std::fprintf(stderr,"[gx-contract] blank framebuffer\n");shutdown();return 7;}
  std::snprintf(path,sizeof(path),"ux0:data/aurora-vita/gx_frontend_%s.ppm",AURORA_TEST_RENDERER);
  FILE* output=std::fopen(path,"wb");
  if(output) {
    std::fprintf(output,"P6\n960 544\n255\n");
    std::vector<uint8_t> rgb(960*544*3);
    for(size_t i=0,j=0;i<image.size();i+=4,j+=3)std::memcpy(rgb.data()+j,image.data()+i,3);
    std::fwrite(rgb.data(),1,rgb.size(),output);std::fclose(output);
  }
  shutdown();
  std::fprintf(stderr,"[gx-contract] pixels=%u clear/copy/FIFO/streaming/present PASS\n",unsigned(colored));
  return 0;
}
}
int main() {
  const int result=run();
  std::fprintf(stderr,"[gx-contract] exit=%d\n",result);
  sceKernelExitProcess(result);
  return result;
}
