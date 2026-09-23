#include "vita_texture_cache.hpp"
#include "vita_pipeline_key.hpp"
#include "vita_texture_decode.hpp"
#include "vita_sampler_units.hpp"
#include "gxm/gxm_texture_layout.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>
#if defined(__vita__)
#include <vitaGL.h>
#include <psp2/kernel/sysmem.h>
#endif

#ifndef AURORA_VITA_RUNTIME_MIPMAP_GENERATION
#define AURORA_VITA_RUNTIME_MIPMAP_GENERATION 0
#endif
#ifndef AURORA_VITA_NATIVE_CMPR
#define AURORA_VITA_NATIVE_CMPR 0
#endif
#ifndef AURORA_VITA_NATIVE_GX_TEXTURES
#define AURORA_VITA_NATIVE_GX_TEXTURES 0
#endif
#ifndef AURORA_VITA_NATIVE_GX_I
#define AURORA_VITA_NATIVE_GX_I 0
#endif
#ifndef AURORA_VITA_NATIVE_GX_IA
#define AURORA_VITA_NATIVE_GX_IA 0
#endif
#ifndef AURORA_VITA_NATIVE_GX_RGB565
#define AURORA_VITA_NATIVE_GX_RGB565 0
#endif
namespace aurora::vita::gfx {
namespace {
bool native_gx_format_enabled(TextureFormat f) noexcept {
#if defined(__vita__) && AURORA_VITA_NATIVE_GX_TEXTURES
  switch(f){
  case TextureFormat::I4:
  case TextureFormat::I8:return AURORA_VITA_NATIVE_GX_I != 0;
  case TextureFormat::IA4:
  case TextureFormat::IA8:return AURORA_VITA_NATIVE_GX_IA != 0;
  case TextureFormat::RGB565:return AURORA_VITA_NATIVE_GX_RGB565 != 0;
  default:return false;
  }
#else
  (void)f;
  return false;
#endif
}
void vgl_log_texture_alloc_fail(uint32_t w,uint32_t h,unsigned fmt,uint8_t mips,uint64_t sourceId,
                                size_t estBytes,size_t cacheBytes,size_t budget) noexcept {
  std::fprintf(stderr,
    "[aurora-vita] texture_alloc_fail w=%u h=%u fmt=0x%X mips=%u source=0x%llX est=%llu cache_bytes=%llu budget=%llu\n",
    w,h,fmt,static_cast<unsigned>(mips),static_cast<unsigned long long>(sourceId),
    static_cast<unsigned long long>(estBytes),static_cast<unsigned long long>(cacheBytes),
    static_cast<unsigned long long>(budget));
#if defined(__vita__)
  SceKernelFreeMemorySizeInfo freeMemory{};
  freeMemory.size=sizeof(freeMemory);
  const int freeResult=sceKernelGetFreeMemorySize(&freeMemory);
  std::fprintf(stderr,
    "[aurora-vita] texture_alloc_mem vgl_ram=%llu/%llu vgl_cdram=%llu/%llu vgl_phycont=%llu/%llu kernel_user=%d kernel_cdram=%d kernel_phycont=%d query=0x%08X\n",
    static_cast<unsigned long long>(vglMemFree(VGL_MEM_RAM)),
    static_cast<unsigned long long>(vglMemTotal(VGL_MEM_RAM)),
    static_cast<unsigned long long>(vglMemFree(VGL_MEM_VRAM)),
    static_cast<unsigned long long>(vglMemTotal(VGL_MEM_VRAM)),
    static_cast<unsigned long long>(vglMemFree(VGL_MEM_SLOW)),
    static_cast<unsigned long long>(vglMemTotal(VGL_MEM_SLOW)),
    freeResult>=0?freeMemory.size_user:-1,
    freeResult>=0?freeMemory.size_cdram:-1,
    freeResult>=0?freeMemory.size_phycont:-1,
    static_cast<unsigned>(freeResult));
#endif
}
// vitaGL uploads RGBA8 as VGL_ALIGN(w,8)*h*4 (row-stride padded to 8 texels).
inline size_t aligned_rgba_bytes(uint32_t width,uint32_t height) noexcept {
  return static_cast<size_t>((width+7u)&~7u)*height*4u;
}
inline size_t aligned_native_bytes(uint32_t width,uint32_t height,uint8_t bpp) noexcept {
  return static_cast<size_t>((width+7u)&~7u)*height*bpp;
}
size_t estimate_native_candidate_bytes(TextureFormat format,uint32_t width,uint32_t height,uint8_t mipCount) noexcept {
  const unsigned levels=std::max<unsigned>(1u,mipCount);
  size_t total=0;
  if(format==TextureFormat::CMPR){
    for(unsigned level=0;level<levels;++level)
      total+=dxt1_texture_size(std::max(1u,width>>level),std::max(1u,height>>level));
  }else{
    const uint8_t bpp=native_texture_bytes_per_pixel(format);
    if(!bpp)return 0;
    for(unsigned level=0;level<levels;++level)
      total+=aligned_native_bytes(std::max(1u,width>>level),std::max(1u,height>>level),bpp);
  }
  return total+total/5u+65536u;
}
#if !defined(__vita__) || AURORA_VITA_RUNTIME_MIPMAP_GENERATION
size_t rgba_full_mip_bytes(uint32_t width,uint32_t height) noexcept {
  size_t total=0;
  for(;;){
    total+=aligned_rgba_bytes(width,height);
    if(width==1&&height==1)break;
    width=std::max(1u,width>>1);height=std::max(1u,height>>1);
  }
  return total;
}
#endif
// Conservative GPU-side footprint estimate for a texture we are about to upload.
// Deliberately over-estimates: vitaGL row alignment, mip chain, and internal
// temp buffers can all push real usage above width*height*4. Never underestimate
// here or the pre-eviction below will let vitaGL run out of mapped memory.
size_t estimate_gpu_bytes(const TextureDesc& d) noexcept {
#if defined(__vita__) && AURORA_VITA_NATIVE_CMPR
  if(d.format==TextureFormat::CMPR&&!d.generateMipmaps){
    const bool pow2=d.width&&d.height&&
        (d.width&(d.width-1u))==0&&(d.height&(d.height-1u))==0;
    if(pow2&&!d.generateMipmaps){
      size_t total=0;
      const unsigned levels=std::max<unsigned>(1u,d.mipCount);
      for(unsigned l=0;l<levels;l++){
        total+=dxt1_texture_size(std::max(1u,d.width>>l),std::max(1u,d.height>>l));
      }
      // Keep the same conservative allocator slack as the RGBA path. The
      // payload itself remains UBC1-sized instead of expanding CMPR to RGBA8.
      return total+total/5u+65536u;
    }
  }
#endif
#if defined(__vita__) && AURORA_VITA_NATIVE_GX_TEXTURES
  if(native_gx_format_enabled(d.format)){
    if(const uint8_t bpp=d.mipCount>1?0:native_texture_bytes_per_pixel(d.format)){
    size_t total=0;const unsigned levels=std::max<unsigned>(1u,d.mipCount);
    for(unsigned l=0;l<levels;l++)total+=aligned_native_bytes(std::max(1u,d.width>>l),std::max(1u,d.height>>l),bpp);
#if AURORA_VITA_RUNTIME_MIPMAP_GENERATION
    if(d.generateMipmaps&&levels==1){uint32_t w=d.width,h=d.height;total=0;for(;;){total+=aligned_native_bytes(w,h,bpp);if(w==1&&h==1)break;w=std::max(1u,w>>1);h=std::max(1u,h>>1);}}
#endif
    return total+total/5u+65536u;
    }
  }
#endif
  size_t total=0;
  const unsigned levels=std::max<unsigned>(1u,d.mipCount);
  for(unsigned l=0;l<levels;l++){
    total+=aligned_rgba_bytes(std::max(1u,d.width>>l),std::max(1u,d.height>>l));
  }
#if !defined(__vita__) || AURORA_VITA_RUNTIME_MIPMAP_GENERATION
  if(d.generateMipmaps){
    const size_t chain=rgba_full_mip_bytes(d.width,d.height);
    if(chain>total)total=chain;
  }
#endif
  return total+total/5u+65536u; // +20% slack +64 KiB fixed overhead
}
constexpr size_t kEvictHeadroom=512u*1024u;
#if defined(__vita__)
GLint wrap(WrapMode w){switch(w){case WrapMode::Clamp:return GL_CLAMP_TO_EDGE;case WrapMode::Repeat:return GL_REPEAT;case WrapMode::Mirror:return GL_MIRRORED_REPEAT;}return GL_REPEAT;}
GLint filt(Filter f){switch(f){case Filter::Nearest:return GL_NEAREST;case Filter::Linear:return GL_LINEAR;case Filter::NearestMipmapNearest:return GL_NEAREST_MIPMAP_NEAREST;case Filter::LinearMipmapNearest:return GL_LINEAR_MIPMAP_NEAREST;case Filter::NearestMipmapLinear:return GL_NEAREST_MIPMAP_LINEAR;case Filter::LinearMipmapLinear:return GL_LINEAR_MIPMAP_LINEAR;}return GL_LINEAR;}
bool mip_filter(Filter f) noexcept {return f==Filter::NearestMipmapNearest||f==Filter::LinearMipmapNearest||f==Filter::NearestMipmapLinear||f==Filter::LinearMipmapLinear;}
Filter without_mips(Filter f) noexcept {switch(f){case Filter::NearestMipmapNearest:case Filter::NearestMipmapLinear:return Filter::Nearest;case Filter::LinearMipmapNearest:case Filter::LinearMipmapLinear:return Filter::Linear;default:return f;}}
#endif
}
TextureCache::~TextureCache(){clear();}
void TextureCache::pre_evict(size_t requiredBytes,uint64_t frame,uint64_t protectKey) noexcept {
  while(bytes_+requiredBytes+kEvictHeadroom>budget_&&!byKey_.empty()){
    const Entry* victim=nullptr;
    for(const auto& kv:byKey_){
      if(kv.first==protectKey)continue;
      // Never evict a texture already referenced this frame: its GL id may sit in
      // an enqueued Aurora draw command and deleting it would crash the GPU.
      if(kv.second.lastUse>=frame)continue;
      if(!victim||kv.second.lastUse<victim->lastUse)victim=&kv.second;
    }
    if(!victim)break;
    const size_t vb=victim->bytes;const Handle vh=victim->handle;
    erase(vh);
    ++preEvictions_;preEvictedBytes_+=vb;
  }
}
void TextureCache::log_resident_breakdown() const noexcept {
#if defined(__vita__)
  constexpr size_t kFormatCount=static_cast<size_t>(TextureFormat::RGBA8888)+1u;
  std::array<uint64_t,kFormatCount> bytes{};
  std::array<uint64_t,kFormatCount> candidateBytes{};
  std::array<uint32_t,kFormatCount> entries{};
  std::array<uint32_t,kFormatCount> mipped{};
  for(const auto& kv:byKey_){
    const auto& e=kv.second;
    const size_t index=static_cast<size_t>(e.format);
    if(index>=kFormatCount)continue;
    bytes[index]+=e.bytes;
    ++entries[index];
    if(e.mipCount>1)++mipped[index];
    const size_t candidate=estimate_native_candidate_bytes(e.format,e.width,e.height,e.mipCount);
    candidateBytes[index]+=candidate?candidate:e.bytes;
  }
  for(size_t i=0;i<kFormatCount;++i){
    if(!entries[i])continue;
    const uint64_t saving=bytes[i]>candidateBytes[i]?bytes[i]-candidateBytes[i]:0;
    std::fprintf(stderr,
      "[aurora-vita] texture_resident fmt=0x%zX entries=%u mipped=%u bytes=%llu native_candidate=%llu saving=%llu\n",
      i,entries[i],mipped[i],
      static_cast<unsigned long long>(bytes[i]),
      static_cast<unsigned long long>(candidateBytes[i]),
      static_cast<unsigned long long>(saving));
  }
#endif
}
Handle TextureCache::get_or_upload(const TextureDesc& d,uint64_t frame,FrameStats* st) noexcept {
  uint64_t key=texture_key(d);
  if(!d.cacheable){
    // A non-cacheable GX texture may be uploaded more than once in one frame.
    // Give every successful handle distinct backing storage so queued draws can
    // never be redirected by a later upload of the same guest pointer.
    const uint64_t salt=(frame+0x9e3779b97f4a7c15ull)^(static_cast<uint64_t>(next_)*0xbf58476d1ce4e5b9ull);
    key^=salt+(key<<6)+(key>>2);
  }
  auto it=byKey_.find(key);if(d.cacheable&&it!=byKey_.end()){it->second.lastUse=frame;if(st)st->textureHits++;return it->second.handle;}if(st)st->textureMisses++;
  if(failedFrame_!=frame){failedFrame_=frame;failedKeys_.clear();}
  if(failedKeys_.find(key)!=failedKeys_.end()){
    ++retrySuppressTotal_;
    if(retrySuppressTotal_==1||(retrySuppressTotal_&(retrySuppressTotal_-1))==0){
      std::fprintf(stderr,"[aurora-vita] texture_retry_suppressed total=%llu frame=%llu source=0x%llX\n",
        static_cast<unsigned long long>(retrySuppressTotal_),static_cast<unsigned long long>(frame),
        static_cast<unsigned long long>(d.sourceId));
    }
    return InvalidHandle;
  }
  if(!d.data||!d.width||!d.height)return InvalidHandle;
  // Reserve GPU memory before touching vitaGL. Optimized allocators can be less
  // forgiving under memory pressure, so reject uploads that cannot fit first.
  const size_t estBytes=estimate_gpu_bytes(d);
  lastRequestedBytes_=estBytes;
  pre_evict(estBytes,frame,key);
  if(estBytes>budget_||bytes_+estBytes>budget_){
    ++allocFailTotal_;
    failedKeys_.insert(key);
    if(allocFailTotal_==1||(allocFailTotal_&(allocFailTotal_-1))==0){
      vgl_log_texture_alloc_fail(d.width,d.height,static_cast<unsigned>(d.format),d.mipCount,d.sourceId,estBytes,bytes_,budget_);
      log_resident_breakdown();
    }
    return InvalidHandle;
  }
  Entry e{};e.handle=next_;e.key=key;e.lastUse=frame;e.width=d.width;e.height=d.height;e.format=d.format;e.mipCount=d.mipCount;e.hasMipmaps=false;e.cacheable=d.cacheable;e.sourceId=d.sourceId;e.paletteSourceId=d.paletteSourceId;e.sourceBytes=d.dataSize;e.paletteBytes=d.paletteSize;
#if defined(__vita__)
  GLuint id=0;glGenTextures(1,&id);if(!id){++allocFailTotal_;failedKeys_.insert(key);return InvalidHandle;}e.gl=id;glBindTexture(GL_TEXTURE_2D,id);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
#else
  e.gl=e.handle;
#endif
  const auto* base=static_cast<const uint8_t*>(d.data);size_t encodedOffset=0;const unsigned levels=std::max<unsigned>(1,d.mipCount);unsigned uploadedLevels=0;
#if defined(__vita__)
  bool directCompressedMips=false;
#if AURORA_VITA_NATIVE_CMPR
  if(d.format==TextureFormat::CMPR&&!d.generateMipmaps){
    auto desc=d;desc.generateMipmaps=false;
    if(!desc.dataSize)desc.dataSize=encoded_mip_chain_size(d.width,d.height,d.format,d.mipCount);
    auto chain=gxm::prepare_ubc1_texture(desc);
    if(chain.ok()){
      // Initialize the GL object normally first so vitaGL owns its lifetime and
      // sampler metadata. Then replace the tiny backing allocation and native
      // descriptor with the compact UBC1 chain. This avoids the historical
      // glCompressedTexImage2D allocator-corruption path entirely.
      const uint32_t zero=0;
      glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,&zero);
      auto* native=vglGetGxmTexture(GL_TEXTURE_2D);
      void* old=vglGetTexDataPointer(GL_TEXTURE_2D);
      void* storage=(native&&old)?vglMemalign(SCE_GXM_TEXTURE_ALIGNMENT,chain.pixels.size()):nullptr;
      if(storage){
        std::memcpy(storage,chain.pixels.data(),chain.pixels.size());
        SceGxmTexture descriptor{};
        if(sceGxmTextureInitSwizzled(&descriptor,storage,SCE_GXM_TEXTURE_FORMAT_UBC1_ABGR,
                                    d.width,d.height,chain.mipCount)>=0){
          *native=descriptor;
          vglOverloadTexDataPointer(GL_TEXTURE_2D,storage);
          vglFree(old); // Cold texture: no submitted draw references this allocation.
          e.explicitMipCount=static_cast<uint8_t>(chain.mipCount);
          uploadedLevels=chain.mipCount;
          directCompressedMips=true;
          static unsigned compressedUploadCount=0;
          const unsigned n=++compressedUploadCount;
          if(n<=16u||(n&(n-1u))==0u)
            std::fprintf(stderr,
              "[aurora-vita] native_cmpr_upload n=%u %ux%u mips=%u bytes=%llu layout=ubc1_swizzled\n",
              n,d.width,d.height,chain.mipCount,
              static_cast<unsigned long long>(chain.pixels.size()));
        }else vglFree(storage);
      }
    }
  }
#endif
  const bool explicitLinearMips=!directCompressedMips&&levels>1 &&
      (d.width&(d.width-1u))==0 && (d.height&(d.height-1u))==0;
  if(explicitLinearMips) {
    // vitaGL generates uncompressed levels instead of honoring supplied bytes.
    // Keep GL ownership via public backing-store interop, and share the bounded
    // linear mip layout with the native backend (including rectangular tails).
    auto desc=d;desc.generateMipmaps=false;
    if(!desc.dataSize)desc.dataSize=encoded_mip_chain_size(d.width,d.height,d.format,d.mipCount);
    auto chain=gxm::prepare_linear_texture(desc);
    auto level0=desc;level0.mipCount=1;
    if(chain.ok() && decode_texture_rgba8(level0,rgbaDecodeScratch_)) {
      glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,d.width,d.height,0,GL_RGBA,GL_UNSIGNED_BYTE,rgbaDecodeScratch_.data());
      auto* native=vglGetGxmTexture(GL_TEXTURE_2D);
      void* old=vglGetTexDataPointer(GL_TEXTURE_2D);
      void* storage=old?vglMemalign(SCE_GXM_TEXTURE_ALIGNMENT,chain.pixels.size()):nullptr;
      if(storage && native) {
        std::memcpy(storage,chain.pixels.data(),chain.pixels.size());
        auto descriptor=*native;
        if(sceGxmTextureSetData(&descriptor,storage)>=0 && sceGxmTextureSetMipmapCount(&descriptor,chain.mipCount)>=0) {
          *native=descriptor;
          vglOverloadTexDataPointer(GL_TEXTURE_2D,storage);
          vglFree(old); // Cold texture: no draw or transfer has referenced it.
          e.explicitMipCount=static_cast<uint8_t>(chain.mipCount);
          uploadedLevels=chain.mipCount;
        } else vglFree(storage);
      } else if(storage) vglFree(storage);
    }
  }
  for(unsigned level=0;!directCompressedMips&&!explicitLinearMips && level<levels;level++){
#else
  for(unsigned level=0;level<levels;level++){
#endif
    TextureDesc ld=d;ld.width=std::max(1u,d.width>>level);ld.height=std::max(1u,d.height>>level);ld.data=base+encodedOffset;ld.mipCount=1;ld.generateMipmaps=false;const size_t encoded=encoded_texture_size(ld.width,ld.height,ld.format);if(d.dataSize&&encodedOffset+encoded>d.dataSize)break;ld.dataSize=encoded;
#if defined(__vita__)
#if AURORA_VITA_NATIVE_GX_TEXTURES
    NativeTextureFormat nativeFormat=NativeTextureFormat::None;
    if(native_gx_format_enabled(ld.format)&&transcode_texture_native(ld,nativeFormat,nativeLinearScratch_)){
      GLint internal=GL_RGBA;GLenum format=GL_RGBA,type=GL_UNSIGNED_BYTE;
      switch(nativeFormat){
      case NativeTextureFormat::Intensity8:internal=GL_INTENSITY;format=GL_LUMINANCE;type=GL_UNSIGNED_BYTE;break;
      case NativeTextureFormat::LuminanceAlpha8:internal=GL_LUMINANCE_ALPHA;format=GL_LUMINANCE_ALPHA;type=GL_UNSIGNED_BYTE;break;
      case NativeTextureFormat::Rgb565:internal=GL_RGB;format=GL_RGB;type=GL_UNSIGNED_SHORT_5_6_5;break;
      default:break;
      }
      static unsigned nativeUploadCount=0;
      ++nativeUploadCount;
      if(nativeUploadCount<=16u||(nativeUploadCount&(nativeUploadCount-1u))==0u){
        std::fprintf(stderr,
          "[aurora-vita] native_texture_upload n=%u fmt=0x%X native=%u %ux%u bytes=%llu\n",
          nativeUploadCount,static_cast<unsigned>(ld.format),static_cast<unsigned>(nativeFormat),
          ld.width,ld.height,static_cast<unsigned long long>(nativeLinearScratch_.size()));
      }
      glTexImage2D(GL_TEXTURE_2D,static_cast<GLint>(level),internal,ld.width,ld.height,0,format,type,nativeLinearScratch_.data());
      ++uploadedLevels;encodedOffset+=encoded;continue;
    }
#endif
#endif
    if(!decode_texture_rgba8(ld,rgbaDecodeScratch_))break;++uploadedLevels;
#if defined(__vita__)
    glTexImage2D(GL_TEXTURE_2D,static_cast<GLint>(level),GL_RGBA,ld.width,ld.height,0,GL_RGBA,GL_UNSIGNED_BYTE,rgbaDecodeScratch_.data());
#endif
    encodedOffset+=encoded;
  }
  if(uploadedLevels==0){
#if defined(__vita__)
    GLuint id=e.gl;glDeleteTextures(1,&id);
#endif
    return InvalidHandle;
  }
#if defined(__vita__)
  // An allocation failure can leave a GL texture id without backing storage.
  // Reject it deterministically before it can reach a draw command.
  if(vglGetTexDataPointer(GL_TEXTURE_2D)==nullptr){
    GLuint id=e.gl;glDeleteTextures(1,&id);
    ++allocFailTotal_;
    failedKeys_.insert(key);
    if(allocFailTotal_==1||(allocFailTotal_&(allocFailTotal_-1))==0){
      vgl_log_texture_alloc_fail(d.width,d.height,static_cast<unsigned>(d.format),d.mipCount,d.sourceId,estBytes,bytes_,budget_);
      log_resident_breakdown();
    }
    return InvalidHandle;
  }
#endif
#if defined(__vita__)
#if AURORA_VITA_RUNTIME_MIPMAP_GENERATION
  if(d.generateMipmaps&&uploadedLevels==1){glGenerateMipmap(GL_TEXTURE_2D);e.hasMipmaps=true;}else e.hasMipmaps=uploadedLevels>1;
#else
  // Runtime mip generation is off by default on Vita: explicit mip levels are safe,
  // while generated chains add allocation pressure and have historically exposed OOM faults.
  e.hasMipmaps=uploadedLevels>1;
#endif
#else
  e.hasMipmaps=(d.generateMipmaps&&uploadedLevels==1)||uploadedLevels>1;
#endif
  e.bytes=estBytes; // GPU-side footprint (row-aligned + slack), not the CPU decode size
  const Handle handle=e.handle;
  const unsigned uploadedGl=e.gl;
  auto [inserted,ok]=byKey_.emplace(key,std::move(e));
  if(!ok){
#if defined(__vita__)
    GLuint id=uploadedGl;if(id)glDeleteTextures(1,&id);
#endif
    return InvalidHandle;
  }
  ++next_;
  byHandle_[handle]=&inserted->second;
  bytes_+=inserted->second.bytes;
  if(bytes_>highWaterBytes_)highWaterBytes_=bytes_;
#if defined(__vita__)
  if(!pressureBreakdownLogged_&&budget_&&bytes_+512u*1024u>=budget_){
    pressureBreakdownLogged_=true;
    std::fprintf(stderr,
      "[aurora-vita] texture_pressure bytes=%llu budget=%llu entries=%u\n",
      static_cast<unsigned long long>(bytes_),static_cast<unsigned long long>(budget_),
      static_cast<unsigned>(byKey_.size()));
    log_resident_breakdown();
  }
#endif
  if(st)st->textureUploads++;trim(frame);return handle;
}
void TextureCache::bind(Handle h,unsigned unit,const SamplerDesc&s) noexcept {auto hi=byHandle_.find(h);if(hi==byHandle_.end()||!hi->second)return;auto&e=*hi->second;
#if defined(__vita__)
  glActiveTexture(GL_TEXTURE0+unit);glBindTexture(GL_TEXTURE_2D,e.gl);
  const auto&old=e.sampler;
  const bool samplerChanged=!e.samplerValid||old.wrapS!=s.wrapS||old.wrapT!=s.wrapT||
      old.minFilter!=s.minFilter||old.magFilter!=s.magFilter||old.lodBias!=s.lodBias;
  if(!e.samplerValid||old.wrapS!=s.wrapS)glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,wrap(s.wrapS));
  if(!e.samplerValid||old.wrapT!=s.wrapT)glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,wrap(s.wrapT));
  if(!e.samplerValid||old.minFilter!=s.minFilter){const Filter minFilter=!e.hasMipmaps&&mip_filter(s.minFilter)?without_mips(s.minFilter):s.minFilter;glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,filt(minFilter));}
  if(!e.samplerValid||old.magFilter!=s.magFilter)glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,filt(s.magFilter));
  if(!e.samplerValid||old.lodBias!=s.lodBias)glTexParameterf(GL_TEXTURE_2D,GL_TEXTURE_LOD_BIAS,vitagl_lod_bias(s.lodBias));
  if(e.explicitMipCount) {
    // GL setters consult their base-level allocation metadata. Reapply the
    // real count only when a setter could have disturbed it or the requested
    // mip state changed. Hot rebinds otherwise stay entirely in vitaGL's bind cache.
    const uint8_t requestedMipCount=mip_filter(s.minFilter)?e.explicitMipCount:1;
    if(samplerChanged||e.appliedMipCount!=requestedMipCount) {
      if(auto* native=vglGetGxmTexture(GL_TEXTURE_2D)) {
        sceGxmTextureSetMipmapCount(native,requestedMipCount);
        e.appliedMipCount=requestedMipCount;
      }
    }
  }
  e.sampler=s;e.samplerValid=true;
#else
  (void)unit;(void)s;
#endif
}
void TextureCache::erase(Handle h) noexcept {auto hi=byHandle_.find(h);if(hi==byHandle_.end()||!hi->second)return;Entry*e=hi->second;const uint64_t key=e->key;bytes_-=e->bytes;
#if defined(__vita__)
  GLuint id=e->gl;glDeleteTextures(1,&id);
#endif
  byHandle_.erase(hi);byKey_.erase(key);}
void TextureCache::clear() noexcept {
#if defined(__vita__)
  for(auto&[_,e]:byKey_){GLuint id=e.gl;glDeleteTextures(1,&id);}
#endif
  byKey_.clear();byHandle_.clear();bytes_=0;failedKeys_.clear();failedFrame_=~uint64_t{0};}
void TextureCache::trim(uint64_t frame) noexcept {
  // Non-cacheable textures are intentionally unique within a frame so commands
  // already enqueued keep valid GL ids. Once that frame has executed they have no
  // reuse value; retire them regardless of the global cache budget.
  for(auto it=byKey_.begin();it!=byKey_.end();){
    if(!it->second.cacheable&&it->second.lastUse<frame){
      const Handle h=it->second.handle;bytes_-=it->second.bytes;
#if defined(__vita__)
      GLuint id=it->second.gl;glDeleteTextures(1,&id);
#endif
      byHandle_.erase(h);
      it=byKey_.erase(it);++evictions_;
    }else ++it;
  }
  const size_t target=budget_>kEvictHeadroom?budget_-kEvictHeadroom:budget_;
  while(bytes_>target&&!byKey_.empty()){
    auto victim=byKey_.end();
    for(auto it=byKey_.begin();it!=byKey_.end();++it){
      // Draw commands from this frame can still reference the GL texture until
      // the command stream is executed. Only retire entries from older frames.
      if(it->second.lastUse>=frame)continue;
      if(victim==byKey_.end()||it->second.lastUse<victim->second.lastUse)victim=it;
    }
    if(victim==byKey_.end())break;
    ++evictions_;erase(victim->second.handle);
  }
}
size_t TextureCache::invalidate_source_range(uint64_t start,size_t bytes) noexcept {
  if(bytes==0)return 0;
  const uint64_t end = bytes > UINT64_MAX - start ? UINT64_MAX : start + static_cast<uint64_t>(bytes);
  auto overlaps=[start,end](uint64_t p,size_t n) noexcept {
    if(p==0||n==0)return false;
    const uint64_t pe=n>UINT64_MAX-p?UINT64_MAX:p+static_cast<uint64_t>(n);
    return p<end&&start<pe;
  };
  std::vector<Handle> victims;
  for(const auto&[_,e]:byKey_)if(overlaps(e.sourceId,e.sourceBytes)||overlaps(e.paletteSourceId,e.paletteBytes))victims.push_back(e.handle);
  for(Handle h:victims)erase(h);
  return victims.size();
}

} // namespace aurora::vita::gfx
