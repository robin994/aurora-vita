#pragma once
#include "vita_gfx_types.hpp"
#include "vita_hash_map.hpp"
#include "vita_native_fwd.hpp"
#include <array>
#include <cstdint>
#include <vector>

namespace aurora::vita::gfx {
class EfbManager {
public:
  ~EfbManager();
  Handle create(uint32_t width,uint32_t height,bool depth=true) noexcept;
  bool bind(Handle h) noexcept;
  void bind_default(uint32_t width,uint32_t height) noexcept;
  bool blit_to_default(Handle h,uint32_t width,uint32_t height) noexcept;
  // Capture a rectangle from the framebuffer that is currently bound, optionally
  // scaling it into an existing/new sampled EFB texture. srcY is GL bottom-left.
  // flipX/flipY affect only the copied texture; they do not change presentation.
  Handle capture_from_bound(Handle existing,int32_t srcX,int32_t srcY,uint32_t srcWidth,uint32_t srcHeight,
                            uint32_t dstWidth,uint32_t dstHeight,EfbCopyFormat format=EfbCopyFormat::Passthrough,
                            bool scissorEnabled=false,bool flipX=false,bool flipY=false) noexcept;
  // Low-memory sampled EFB path for optimized Vita builds. The caller supplies RGBA8 pixels
  // from the currently rendered target; no framebuffer/renderbuffer or temporary capture texture
  // is allocated. Existing handles are updated in-place when dimensions match.
  Handle upload_rgba(Handle existing,uint32_t width,uint32_t height,const void* rgba) noexcept;
  bool bind_texture(Handle h,unsigned unit,const SamplerDesc& sampler) noexcept;
  bool read_rgba(Handle h,std::vector<uint8_t>& out) noexcept;
  bool dimensions(Handle h,uint32_t& width,uint32_t& height) const noexcept;
  void destroy(Handle h) noexcept;
  void clear() noexcept;
  size_t bytes() const noexcept { return bytes_; }
  size_t high_water_bytes() const noexcept { return highWaterBytes_; }
  size_t entries() const noexcept { return map_.size(); }
#if defined(__vita__) && !defined(AURORA_VITA_RENDERER_GXM)
  unsigned color_texture(Handle h)const noexcept;
#endif
private:
  friend class Renderer;
#if defined(AURORA_VITA_RENDERER_GXM)
  gxm::Renderer* native_=nullptr;
#endif
  struct Entry{unsigned fbo=0,color=0,depth=0;uint32_t width=0,height=0;size_t bytes=0;SamplerDesc sampler{};bool samplerValid=false;};
  FlatHashMap<Handle,Entry> map_;
  Handle next_=1;
  static constexpr size_t CopyProgramCount = static_cast<size_t>(EfbCopyFormat::GB8) + 1;
  std::array<unsigned,CopyProgramCount> blitPrograms_{};
  std::array<int,CopyProgramCount> blitTex_{};
  unsigned blitVbo_=0;
  // Reusable GPU capture target for copy formats that need conversion. vitaGL's
  // glCopyTexImage2D currently performs a CPU glReadPixels + re-upload, so using
  // an FBO here keeps the copy on GXM before the conversion shader runs.
  unsigned captureFbo_=0,captureTexture_=0;
  uint32_t captureWidth_=0,captureHeight_=0;
  size_t captureBytes_=0;
  unsigned boundFbo_=0; // mirror GL_FRAMEBUFFER binding so helper allocations can restore the source target.
  size_t bytes_=0,highWaterBytes_=0;
  bool ensure_blitter(EfbCopyFormat format) noexcept;
  bool ensure_capture_target(uint32_t width,uint32_t height) noexcept;
  bool draw_texture(unsigned texture,uint32_t width,uint32_t height,EfbCopyFormat format=EfbCopyFormat::Passthrough) noexcept;
};
} // namespace aurora::vita::gfx
