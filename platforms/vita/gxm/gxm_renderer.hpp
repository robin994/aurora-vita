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
  size_t parameterBufferBytes = 4 * 1024 * 1024;
  size_t resourceBudgetBytes = 24 * 1024 * 1024;
  size_t maxPipelines = 128;
  bool waitVblank = true;
  const char* shaderCompilerPath = nullptr;
};

// Standalone native device. It accepts the shared Aurora pipeline/draw types;
// it does not yet implement the legacy gfx::Renderer GX/EFB facade.
// Single render-thread owner. Resources are immutable, created between scenes,
// retained until shutdown, and bounded by Config rather than evicted in flight.
class Renderer {
public:
  Renderer();
  ~Renderer();
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;
  bool initialize(const Config& config = {});
  void shutdown() noexcept;
  uint64_t create_pipeline(const gfx::PipelineDesc& desc);
  gfx::Handle create_buffer(const void* data, size_t bytes);
  gfx::Handle create_texture(const gfx::TextureDesc& desc);
  bool begin_frame(const gfx::Color& clearColor, float clearDepth = 1.f);
  bool draw(const gfx::DrawPacket& packet);
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
