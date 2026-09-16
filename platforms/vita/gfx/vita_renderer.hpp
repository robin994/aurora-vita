#pragma once
#include "vita_buffer_pool.hpp"
#include "vita_command_stream.hpp"
#include "vita_efb.hpp"
#include "vita_pipeline_cache.hpp"
#include "vita_texture_cache.hpp"
#include <cstddef>
#include <cstdint>
#include <array>
#include <memory>
namespace aurora::vita::gfx {
struct RendererConfig {
  uint32_t width=960,height=544;
  size_t textureBudget=24*1024*1024;
  size_t pipelineBudget=512;
  uint32_t displayBuffers=3;
  bool waitVblank=true;
  size_t nativeResourceBudget=64*1024*1024;
};
class Renderer {
public:
  explicit Renderer(const RendererConfig& cfg={});~Renderer();
  Renderer(const Renderer&)=delete;Renderer&operator=(const Renderer&)=delete;
  bool initialize() noexcept;void shutdown() noexcept;void begin_frame() noexcept;void end_frame() noexcept;
  bool present(bool display=true) noexcept;
  bool readback_rgba8(std::vector<uint8_t>& pixels) noexcept;
  const char* last_error() const noexcept;
  bool failed() const noexcept { return failed_; }
  static constexpr bool supports_fixed_vertex() noexcept {
#if defined(AURORA_VITA_RENDERER_GXM)
    return false;
#else
    return true;
#endif
  }
  static constexpr uint32_t max_indexed_vertices() noexcept {
#if defined(AURORA_VITA_RENDERER_GXM)
    return 64000;
#else
    return 65536;
#endif
  }
  uint64_t create_pipeline(const PipelineDesc& d) noexcept;
  Handle create_texture(const TextureDesc& d) noexcept;
  size_t invalidate_texture_source_range(uint64_t start,size_t bytes) noexcept;
  Handle create_vertex_buffer(const void*d,size_t n,bool dynamic=false) noexcept;
  Handle create_index_buffer(const void*d,size_t n,bool dynamic=false) noexcept;
  bool update_buffer(Handle h,const void*d,size_t n,size_t off=0) noexcept;
  Handle create_efb(uint32_t w,uint32_t h,bool depth=true) noexcept;
  bool bind_efb(Handle h) noexcept;void bind_default() noexcept;
  bool blit_efb(Handle h) noexcept;
  // Framebuffer copy. Source coordinates are top-left Aurora/GX coordinates.
  // Backends own synchronization and may use a conservative CPU conversion path.
  // flipX/flipY rotate only the sampled copy, leaving the display path untouched.
  Handle capture_current(Handle existing,const Scissor& src,uint32_t dstWidth,uint32_t dstHeight,
                         EfbCopyFormat format=EfbCopyFormat::Passthrough,bool flipX=false,bool flipY=false) noexcept;
  Handle upload_efb_rgba(Handle existing,uint32_t width,uint32_t height,const void* rgba) noexcept;
  void clear_current(const Color& color,float depth,bool clearRgb,bool clearAlpha,bool clearDepth) noexcept;
  void execute(const CommandStream& stream) noexcept;void draw(const DrawPacket& d) noexcept;
  // Resource uploads happen outside draw() and can change raw vitaGL buffer or
  // texture bindings. DrawSink calls this once per submitted command chunk so the
  // first draw re-establishes only those bindings, without throwing away pipeline
  // or viewport/scissor state.
  void invalidate_resource_bindings() noexcept;
  const FrameStats& stats()const noexcept{return stats_;}uint64_t frame()const noexcept{return frame_;}
  uint32_t target_width()const noexcept{return targetWidth_;}uint32_t target_height()const noexcept{return targetHeight_;}
  PipelineCache& pipelines() noexcept{return pipelines_;}TextureCache& textures() noexcept{return textures_;}BufferPool& buffers() noexcept{return buffers_;}EfbManager& efb() noexcept{return efb_;}
private:
  void invalidate_draw_state() noexcept;
  void invalidate_texture_bindings() noexcept;
  void invalidate_buffer_bindings() noexcept;
  RendererConfig cfg_{};uint32_t targetWidth_=960,targetHeight_=544;Handle boundEfb_=InvalidHandle;
#if defined(AURORA_VITA_RENDERER_GXM)
  // Declared before the resource facades: the native device outlives them.
  std::unique_ptr<gxm::Renderer> native_;
#endif
  bool failed_=false;
  Handle maskedClearVertices_=InvalidHandle,maskedClearIndices_=InvalidHandle;
  PipelineCache pipelines_{};TextureCache textures_;BufferPool buffers_{};EfbManager efb_{};FrameStats stats_{};uint64_t frame_=0;bool initialized_=false;
#if defined(__vita__)
  bool viewportValid_=false,scissorValid_=false,scissorEnabled_=false,vertexStateValid_=false,indexStateValid_=false;
  bool vertexAttribMaskValid_=false,vertexAttribDefaultsValid_=false;
  Viewport cachedViewport_{};Scissor cachedScissor_{};
  Handle cachedVertexBuffer_=InvalidHandle,cachedIndexBuffer_=InvalidHandle;
  uint32_t cachedVertexOffset_=0;
  uint64_t cachedVertexPipeline_=0;
  uint32_t enabledVertexAttribMask_=0;
  std::array<TextureBinding,MaxTextures> cachedTextures_{};
  std::array<bool,MaxTextures> textureStateValid_{};
#endif
};
} // namespace aurora::vita::gfx
