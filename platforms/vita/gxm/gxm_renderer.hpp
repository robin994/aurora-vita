#pragma once
#include "gfx/vita_gfx_types.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace aurora::vita::gxm {
struct Config {
  uint32_t width = 960;
  uint32_t height = 544;
  uint32_t displayBuffers = 3;
  uint32_t scenesPerFrame = 8;
  size_t parameterBufferBytes = 4 * 1024 * 1024;
  size_t resourceBudgetBytes = 24 * 1024 * 1024;
  size_t cdramPoolBytes = 64 * 1024 * 1024;
  size_t cdramReserveBytes = 8 * 1024 * 1024;
  size_t maxPipelines = 128;
  bool waitVblank = true;
  bool d16Depth = false;
  const char* shaderCompilerPath = nullptr;
  const char* programCachePath = nullptr;
  bool preloadProgramCache = false;
  size_t programCachePreloadLimit = 1024;
};

// Native implementation used by the public gfx::Renderer facade and the device
// probe. Single render-thread owner; mutation/destruction waits for submitted
// GPU users before recycling their storage.
class Renderer {
public:
  Renderer();
  ~Renderer();
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;
  bool initialize(const Config& config = {});
  void shutdown() noexcept;
  void set_runtime_shader_compilation_enabled(bool enabled) noexcept;
  bool runtime_shader_compilation_enabled() const noexcept;
  bool last_pipeline_compile_blocked() const noexcept;
  uint64_t runtime_shader_compiles() const noexcept;
  uint64_t runtime_shader_compile_us() const noexcept;
  uint64_t blocked_shader_compile_misses() const noexcept;
  uint32_t program_cache_hits() const noexcept;
  uint32_t program_cache_misses() const noexcept;
  uint64_t create_pipeline(const gfx::PipelineDesc& desc);
  void destroy_pipeline(uint64_t key);
  gfx::Handle create_buffer(const void* data, size_t bytes);
  // `storageRetired` is only for common streaming pages whose owner has already
  // observed the configured frames-in-flight retirement interval.
  bool update_buffer(gfx::Handle handle, const void* data, size_t bytes, size_t offset=0,
                     bool storageRetired=false);
  void destroy_buffer(gfx::Handle handle);
  gfx::Handle create_texture(const gfx::TextureDesc& desc);
  void destroy_texture(gfx::Handle handle);
  size_t texture_bytes(gfx::Handle handle) const noexcept;
  bool begin_frame();
  bool begin_frame(const gfx::Color& clearColor, float clearDepth = 1.f);
  bool clear(const gfx::Color& color, float depth, bool rgb=true, bool alpha=true, bool writeDepth=true);
  bool finish();
  gfx::Handle create_target(uint32_t width, uint32_t height, bool depth=true);
  bool bind_target(gfx::Handle handle);
  bool read_target(gfx::Handle handle, std::vector<uint8_t>& pixels);
  bool read_current(std::vector<uint8_t>& pixels, uint32_t& width, uint32_t& height);
  bool upload_target(gfx::Handle handle, const void* rgba, uint32_t width, uint32_t height);
  bool copy_current_to_target(gfx::Handle handle, const gfx::Scissor& source,
                              gfx::EfbCopyFormat format, bool flipX, bool flipY);
  bool blit_to_default(gfx::Handle handle);
  bool copy_display_region(const gfx::Scissor& source);
  bool draw(const gfx::DrawPacket& packet);
  bool bind_pipeline(uint64_t key,const gfx::GpuDrawUniforms& uniforms,const gfx::Scissor& scissor={},
                     const gfx::FixedVertexUniforms* fixedVertex=nullptr,
                     const std::array<gfx::TextureBinding,gfx::MaxTextures>* textures=nullptr);
  bool bind_texture(gfx::Handle handle,unsigned unit,const gfx::SamplerDesc& sampler,
                    bool requireNativeWrap=false);
  bool end_frame(bool present = true);
  // Diagnostic synchronization/readback, never part of the normal frame loop.
  bool readback_rgba8(std::vector<uint8_t>& pixels);
  const char* last_error() const noexcept;
  const gfx::FrameStats& stats() const noexcept;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace aurora::vita::gxm
